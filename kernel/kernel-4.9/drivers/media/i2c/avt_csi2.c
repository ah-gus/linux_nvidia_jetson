/*
 * Allied Vision CSI2 Camera
 *
 * This program is free software; you may redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 2 of the License.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/i2c.h>
#include <linux/lcm.h>
#include <linux/crc32.h>
#include <media/v4l2-device.h>
#include <media/v4l2-event.h>
#include <media/v4l2-of.h>
#include <media/tegra-v4l2-camera.h>
#include <media/camera_common.h>
#include <media/mc_common.h>

#include "avt_csi2.h"
#include <uapi/linux/libcsi_ioctl.h>
#include <media/avt_csi2_soc.h>

static int debug;
MODULE_PARM_DESC(debug, "debug");
module_param(debug, int, 0600);/* S_IRUGO */

/* For overriding alignment value. 0=Use internal value.*/
static int v4l2_width_align = 0;
MODULE_PARM_DESC(v4l2_width_align, "v4l2_width_align");
module_param(v4l2_width_align, int, 0600);/* S_IRUGO */


static int add_wait_time_ms = 2000;
module_param(add_wait_time_ms, int, 0600);


#define AVT_DBG_LVL 3

#define avt_dbg(dev, fmt, args...) \
		v4l2_dbg(AVT_DBG_LVL, debug, dev, "%s:%d: " fmt "", \
				__func__, __LINE__, ##args) \

#define avt_err(dev, fmt, args...) \
		v4l2_err(dev, "%s:%d: " fmt "", __func__, __LINE__, ##args) \

#define avt_warn(dev, fmt, args...) \
		v4l2_warn(dev, "%s:%d: " fmt "", __func__, __LINE__, ##args) \

#define avt_info(dev, fmt, args...) \
		v4l2_info(dev, "%s:%d: " fmt "", __func__, __LINE__, ##args) \

#define DEFAULT_FPS 30

#define AV_CAM_DEFAULT_FMT	MEDIA_BUS_FMT_VYUY8_2X8

#define IO_LIMIT	1024
#define BCRM_WAIT_HANDSHAKE_TIMEOUT	3000

static int avt_set_selection(struct v4l2_subdev *sd,
		struct v4l2_subdev_pad_config *cfg,
		struct v4l2_subdev_selection *sel);

static int avt_reg_read(struct i2c_client *client, __u32 reg,
		__u32 reg_size, __u32 count, char *buffer);

static int avt_init_mode(struct v4l2_subdev *sd);

static int avt_init_frame_param(struct v4l2_subdev *sd);

static int avt_s_parm(struct v4l2_subdev *sd, struct v4l2_streamparm *parm);
static int avt_align_width(struct v4l2_subdev *sd, int width);
static int avt_get_align_width(struct v4l2_subdev *sd);
static bool common_range(uint32_t nMin1, uint32_t nMax1, uint32_t nInc1,
				uint32_t nMin2, uint32_t nMax2, uint32_t nInc2,
				uint32_t *rMin, uint32_t *rMax, uint32_t *rInc);

static void bcrm_dump(struct i2c_client *client);
static void dump_bcrm_reg_8(struct i2c_client *client, u16 nOffset, const char *pRegName);
static void dump_bcrm_reg_32(struct i2c_client *client, u16 nOffset, const char *pRegName);
static void dump_bcrm_reg_64(struct i2c_client *client, u16 nOffset, const char *pRegName);
static int soft_reset(struct i2c_client *client);
static void dump_frame_param(struct v4l2_subdev *sd);

static void swapbytes(void *_object, size_t _size)
{
	switch (_size) {
	case 2:
		cpu_to_be16s((uint16_t *)_object);
		break;
	case 4:
		cpu_to_be32s((uint32_t *)_object);
		break;
	case 8:
		cpu_to_be64s((uint64_t *)_object);
		break;
	}
}

static uint32_t i2c_read(struct i2c_client *client, uint32_t reg, uint32_t size, uint32_t count, char *buf)
{
	struct i2c_msg msg[2];
	u8 msgbuf[size];
	int ret = 0, i = 0, j = 0, reg_size_bkp;

	reg_size_bkp = size;

	/* clearing i2c msg with 0's */
	memset(msg, 0, sizeof(msg));

	if (count > IO_LIMIT) {
		dev_err(&client->dev, "Limit excedded! i2c_reg->count > IO_LIMIT\n");
		count = IO_LIMIT;
	}

	/* find start address of buffer */
	for (i = --size; i >= 0; i--, j++)
		msgbuf[i] = ((reg >> (8*j)) & 0xFF);

	msg[0].addr = client->addr;
	msg[0].flags = 0;
	msg[0].len = reg_size_bkp;
	msg[0].buf = msgbuf;
	msg[1].addr = client->addr; /* slave address */
	msg[1].flags = I2C_M_RD; /* read flag setting */
	msg[1].len = count;
	msg[1].buf = buf; /* dest buffer */

	ret = i2c_transfer(client->adapter, msg, 2);

	return ret;
}

static int i2c_write(struct i2c_client *client, uint32_t reg, uint32_t reg_size, uint32_t buf_size, char *buf)
{
	int j = 0, i = 0;
	char *i2c_w_buf;
	int ret = 0;

	/* count exceeds writing IO_LIMIT characters */
	if (buf_size > IO_LIMIT) {
		dev_err(&client->dev, "limit excedded! i2c_reg->count > IO_LIMIT\n");
		buf_size = IO_LIMIT;
	}

	i2c_w_buf = kzalloc(buf_size + reg_size, GFP_KERNEL);
	if (!i2c_w_buf)
		return -ENOMEM;

	/* Fill the address in buffer upto size of address want to write */
	for (i = reg_size - 1, j = 0; i >= 0; i--, j++)
		i2c_w_buf[i] = ((reg >> (8 * j)) & 0xFF);

	/* Append the data value in the same buffer */
	memcpy(i2c_w_buf + reg_size, buf, buf_size);

	ret = i2c_master_send(client, i2c_w_buf, buf_size + reg_size);
	
        kfree(i2c_w_buf);

	return ret;
}

static bool bcrm_get_write_handshake_availibility(struct i2c_client *client)
{
	struct camera_common_data *s_data = to_camera_common_data(&client->dev);
	struct avt_csi2_priv *priv = (struct avt_csi2_priv *)s_data->priv;
	u8 value = 0;
	int status;

	/* Reading the device firmware version from camera */
	status = avt_reg_read(client,
					priv->cci_reg.bcrm_addr +
					BCRM_WRITE_HANDSHAKE_8RW,
				 	AV_CAM_REG_SIZE,
					AV_CAM_DATA_SIZE_8,
					(char *) &value);

	if ((status >= 0) && (value & 0x80))
	{
		dev_dbg(&client->dev, "BCRM write handshake supported!");
		return true;
	}
	else
	{
		dev_warn(&client->dev, "BCRM write handshake NOT supported!");
		return false;
	}
}

/**
 * @brief Since the camera needs a few ms to process written data, we need to poll
   the handshake register to make sure to continue not too early with the next write access.
 *
 * @param timeout_ms : Timeout value in ms
 * @param reg : Register previously written to (used just for debug msg)
 * @return uint64_t : Duration in ms
 */
static uint64_t wait_for_bcrm_write_handshake(struct i2c_client *client, uint64_t timeout_ms, u16 reg)
{
	struct camera_common_data *s_data = to_camera_common_data(&client->dev);
	struct avt_csi2_priv *priv = (struct avt_csi2_priv *)s_data->priv;
	static const int poll_interval_ms = 2;
	static const int default_wait_time_ms = 20;
	int status = 0;
	u8 buffer[3] = {0};
	u8 handshake_val = 0;
	bool handshake_valid = false;
	uint64_t duration_ms = 0;
	uint64_t start_jiffies = get_jiffies_64();
	unsigned long timeout_jiffies = jiffies + msecs_to_jiffies(timeout_ms);

	if (priv->write_handshake_available)
	{
		/* We need to poll the handshake register and wait until the camera has processed the data */
		dev_dbg(&client->dev, " Wait for 'write done' bit (0x81) ...");
		do
		{
			usleep_range(poll_interval_ms*1000, (poll_interval_ms*1000)+1);
			/* Read handshake register */
			status = avt_reg_read(client,
					        priv->cci_reg.bcrm_addr +
					        BCRM_WRITE_HANDSHAKE_8RW,
					        AV_CAM_REG_SIZE,
					        AV_CAM_DATA_SIZE_8,
				                (char *)&handshake_val);

			if (status >= 0)
			{
				/* Check, if handshake bit is set */
				if ((handshake_val & 0x01) == 1)
				{
					do
					{
						/* Handshake set by camera. We should to reset it */
						buffer[0] = (priv->cci_reg.bcrm_addr + BCRM_WRITE_HANDSHAKE_8RW) >> 8;
						buffer[1] = (priv->cci_reg.bcrm_addr + BCRM_WRITE_HANDSHAKE_8RW) & 0xff;
						buffer[2] = (handshake_val & 0xFE); /* Reset LSB (handshake bit)*/
						status = i2c_master_send(client, buffer, sizeof(buffer));
						if (status >= 0)
						{
							/* Since the camera needs a few ms for every write access to finish, we need to poll here too */
							dev_dbg(&client->dev, " Wait for reset of 'write done' bit (0x80) ...");
							do
							{
								usleep_range(poll_interval_ms*1000, (poll_interval_ms*1000)+1);
								/* We need to wait again until the bit is reset */
								status = avt_reg_read(client,
											priv->cci_reg.bcrm_addr +
											BCRM_WRITE_HANDSHAKE_8RW,
											AV_CAM_REG_SIZE,
											AV_CAM_DATA_SIZE_8,
											(char *)&handshake_val);

								if (status >= 0)
								{
									if ((handshake_val & 0x1) == 0) /* Verify write */
									{
										duration_ms = jiffies_to_msecs(get_jiffies_64() - start_jiffies);
										handshake_valid = true;
										break;
									}
									usleep_range(poll_interval_ms*1000, (poll_interval_ms*1000)+1);
								}
								else
								{
									dev_err(&client->dev, " Error while reading WRITE_HANDSHAKE_REG_8RW register.");
									break;
								}
							} while (time_before(jiffies, timeout_jiffies));
							if (!handshake_valid)
							{
								dev_warn(&client->dev, " Verify handshake timeout :-)");
							}
							break;
						}
						else
						{
							dev_err(&client->dev, " Error while writing WRITE_HANDSHAKE_REG_8RW register.");
							break;
						}
					} while (!handshake_valid && time_before(jiffies, timeout_jiffies));
				}
			}
			else
			{
				dev_err(&client->dev, " Error while reading WRITE_HANDSHAKE_REG_8RW register.");
				break;
			}
		}
		while (!handshake_valid && time_before(jiffies, timeout_jiffies));

		if (!handshake_valid)
		{
			dev_err(&client->dev, " Write handshake timeout! (Register 0x%02X)", reg);
		}
	}
	else
	{
		/* Handshake not supported. Use static sleep at least once as fallback */

		usleep_range(default_wait_time_ms*1000, (default_wait_time_ms*1000)+1);
		//for (i=0; i<default_wait_time_ms; i++) {
		//	udelay(1000);
		//}
		duration_ms = jiffies_to_msecs(get_jiffies_64() - start_jiffies);
	}

	return duration_ms;
}

static int avt_reg_read(struct i2c_client *client, __u32 reg,
		__u32 reg_size, __u32 count, char *buffer)
{
	int ret;

	uint32_t i2c_reg;
	uint32_t i2c_reg_size;
	uint32_t i2c_reg_count;

	char *i2c_reg_buf;

	i2c_reg = reg;
	i2c_reg_size = reg_size;
	i2c_reg_count = count;
	i2c_reg_buf = buffer;

	ret = i2c_read(client, i2c_reg,
			i2c_reg_size, i2c_reg_count, i2c_reg_buf);

	if (ret < 0)
		return ret;

	swapbytes(buffer, count);
	return ret;
}

static int avt_reg_write(struct i2c_client *client, u16 reg, u8 val)
{
	int ret = 0;
	u8 au8Buf[3] = {0};
	uint64_t duration = 0;

	au8Buf[0] = reg >> 8;
	au8Buf[1] = reg & 0xff;
	au8Buf[2] = val;

	ret = i2c_master_send(client, au8Buf, 3);

	if (ret < 0)
		dev_err(&client->dev, "%s, i2c write failed reg=%x,val=%x error=%d\n",
			__func__, reg, val, ret);

	duration = wait_for_bcrm_write_handshake(client, BCRM_WAIT_HANDSHAKE_TIMEOUT, reg);

	dev_dbg(&client->dev, "i2c write success reg=0x%x, duration=%lldms, ret=%d\n", reg, duration, ret);

	return ret;
}

static struct avt_csi2_priv *avt_get_priv(struct v4l2_subdev *sd)
{
	struct i2c_client *client;
	struct camera_common_data *s_data;

	if (sd == NULL)
		return NULL;

	client = v4l2_get_subdevdata(sd);
	if (client == NULL)
		return NULL;

	s_data = to_camera_common_data(&client->dev);
	if (s_data == NULL)
		return NULL;

	return (struct avt_csi2_priv *)s_data->priv;
}

static struct v4l2_ctrl *avt_get_control(struct v4l2_subdev *sd, u32 id)
{
	int i;
	struct avt_csi2_priv *priv = avt_get_priv(sd);

	for (i = 0; i < AVT_MAX_CTRLS; i++) {
		if (priv->ctrls[i] == NULL)
			continue;
		if (priv->ctrls[i]->id == id)
			return priv->ctrls[i];
	}

	return NULL;
}

static int ioctl_gencam_i2cwrite_reg(struct i2c_client *client, uint32_t reg,
		uint32_t size, uint32_t count, const char *buf)
{
	int j = 0, i = 0;
	char *i2c_w_buf;
	int ret = 0;
	uint64_t duration = 0;
	struct camera_common_data *s_data = to_camera_common_data(&client->dev);
	struct avt_csi2_priv *priv = (struct avt_csi2_priv *)s_data->priv;

	/* count exceeds writting IO_LIMIT characters */
	if (count > IO_LIMIT) {
		dev_err(&client->dev, "limit excedded! i2c_reg->count > IO_LIMIT\n");
		count = IO_LIMIT;
	}

	i2c_w_buf = kzalloc(count + size, GFP_KERNEL);
	if (!i2c_w_buf)
		return -ENOMEM;

	/* Fill the address in buffer upto size of address want to write */
	for (i = size - 1, j = 0; i >= 0; i--, j++)
		i2c_w_buf[i] = ((reg >> (8 * j)) & 0xFF);

	/* Append the data value in the same buffer */
	memcpy(i2c_w_buf + size, buf, count);

	ret = i2c_master_send(client, i2c_w_buf, count + size);

	if (ret < 0)
		dev_err(&client->dev, "%s:%d: i2c write failed ret %d\n",
				__func__, __LINE__, ret);


	/* Wait for write handshake register only for BCM registers */
	if ((reg >= priv->cci_reg.bcrm_addr) && (reg <= priv->cci_reg.bcrm_addr + _BCRM_LAST_ADDR))
	{
		duration = wait_for_bcrm_write_handshake(client, BCRM_WAIT_HANDSHAKE_TIMEOUT, reg);
		dev_dbg(&client->dev, "i2c write success reg=0x%x, duration=%lldms, ret=%d\n", reg, duration, ret);
	}

	kfree(i2c_w_buf);

	return ret;
}

static int ioctl_bcrm_i2cwrite_reg(struct i2c_client *client,
		struct v4l2_ext_control *vc,
		unsigned int reg,
		int length)
{
	uint32_t i2c_reg;
	uint32_t i2c_reg_size;
	uint32_t i2c_reg_count;
	char *i2c_reg_buf;

	ssize_t ret;
	__u64 temp = 0;

	if (length > AV_CAM_DATA_SIZE_32) {
		temp = vc->value;
		swapbytes(&temp, length);
	} else
		swapbytes(&vc->value, length);

	i2c_reg = reg;
	i2c_reg_size = AV_CAM_REG_SIZE;
	i2c_reg_count = length;

	if (length > AV_CAM_DATA_SIZE_32)
		i2c_reg_buf = (char *) &temp;
	else
		i2c_reg_buf = (char *) &vc->value;

	ret = ioctl_gencam_i2cwrite_reg(client, i2c_reg, i2c_reg_size,
			i2c_reg_count, i2c_reg_buf);

	if (ret < 0)
		dev_err(&client->dev, "%s:%d i2c write failed\n",
				__func__, __LINE__);

	return ret;
}

static int set_bayer_format(struct i2c_client *client, __u8 value)
{
	struct camera_common_data *s_data = to_camera_common_data(&client->dev);
	struct avt_csi2_priv *priv = (struct avt_csi2_priv *)s_data->priv;
	int ret = 0;

	uint32_t i2c_reg;
	uint32_t i2c_reg_size;
	uint32_t i2c_reg_count;

	char *i2c_reg_buf;

	CLEAR(i2c_reg);
	i2c_reg = priv->cci_reg.bcrm_addr + BCRM_IMG_BAYER_PATTERN_8RW;
	i2c_reg_size = AV_CAM_REG_SIZE;
	i2c_reg_count = AV_CAM_DATA_SIZE_8;
	i2c_reg_buf = (char *) &value;

	ret = ioctl_gencam_i2cwrite_reg(client, i2c_reg, i2c_reg_size,
					i2c_reg_count, i2c_reg_buf);

	if (ret < 0) {
		dev_err(&client->dev, "%s:%d i2c write failed\n",
				__func__, __LINE__);
		return ret;
	}

	return 0;
}


static bool avt_check_fmt_available(struct i2c_client *client, u32 media_bus_fmt)
{
	struct camera_common_data *s_data = to_camera_common_data(&client->dev);
	struct avt_csi2_priv *priv = (struct avt_csi2_priv *)s_data->priv;
	u64 avail_mipi = 0;
	unsigned char bayer_val = 0;
	union bcrm_avail_mipi_reg feature_inquiry_reg;
	union bcrm_bayer_inquiry_reg bayer_inquiry_reg;
	int ret;

    dev_dbg(&client->dev,"%s: media_bus_fmt: 0x%x\n", __FUNCTION__, media_bus_fmt);

	if (media_bus_fmt ==  MEDIA_BUS_FMT_CUSTOM)
		return true;

	/* read the MIPI format register to check whether the camera
	 * really support the requested pixelformat format
	 */
	ret = avt_reg_read(client,
			priv->cci_reg.bcrm_addr +
			BCRM_IMG_AVAILABLE_MIPI_DATA_FORMATS_64R,
			AV_CAM_REG_SIZE, AV_CAM_DATA_SIZE_64,
			(char *) &avail_mipi);

	if (ret < 0) {
		dev_err(&client->dev, "i2c read failed (%d)\n", ret);
		return false;
	}

    dev_dbg(&client->dev,"%s: Camera available MIPI data formats: 0x%llx\n", __FUNCTION__, avail_mipi);
    if (avail_mipi == 0)
    {       
            /* Fallback app running? -> Fake pixelformat */
            dev_warn(&client->dev, "avail_mipi=0. Fallback app running?");
            avail_mipi = 0x80;      // RGB888 
    }

	feature_inquiry_reg.value = avail_mipi;

	/* read the Bayer Inquiry register to check whether the camera
	 * really support the requested RAW format
	 */
	ret = avt_reg_read(client,
			priv->cci_reg.bcrm_addr +
			BCRM_IMG_BAYER_PATTERN_INQUIRY_8R,
			AV_CAM_REG_SIZE, AV_CAM_DATA_SIZE_8,
			(char *) &bayer_val);

    dev_dbg(&client->dev,"%s: Camera bayer pattern inq: 0x%x\n", __FUNCTION__, bayer_val);
	if (ret < 0) {
		dev_err(&client->dev, "i2c read failed (%d)\n", ret);
		return false;
	}

	bayer_inquiry_reg.value = bayer_val;

	switch (media_bus_fmt) {
		case MEDIA_BUS_FMT_RGB444_1X12:
			return feature_inquiry_reg.avail_mipi.rgb444_avail;
		case MEDIA_BUS_FMT_RGB565_1X16:
			return feature_inquiry_reg.avail_mipi.rgb565_avail;
		case MEDIA_BUS_FMT_RGB888_1X24:
		case MEDIA_BUS_FMT_BGR888_1X24:
			return feature_inquiry_reg.avail_mipi.rgb888_avail;
		case MEDIA_BUS_FMT_VYUY8_2X8:
			return feature_inquiry_reg.avail_mipi.yuv422_8_avail;
		/* RAW 8 */
		case MEDIA_BUS_FMT_Y8_1X8:
			return feature_inquiry_reg.avail_mipi.raw8_avail &&
				bayer_inquiry_reg.bayer_pattern.monochrome_avail;
		case MEDIA_BUS_FMT_SBGGR8_1X8:
			return feature_inquiry_reg.avail_mipi.raw8_avail &&
				bayer_inquiry_reg.bayer_pattern.bayer_BG_avail;
		case MEDIA_BUS_FMT_SGBRG8_1X8:
			return feature_inquiry_reg.avail_mipi.raw8_avail
				&& bayer_inquiry_reg.bayer_pattern.bayer_GB_avail;
		case MEDIA_BUS_FMT_SGRBG8_1X8:
			return feature_inquiry_reg.avail_mipi.raw8_avail &&
				bayer_inquiry_reg.bayer_pattern.bayer_GR_avail;
		case MEDIA_BUS_FMT_SRGGB8_1X8:
			return feature_inquiry_reg.avail_mipi.raw8_avail &&
				bayer_inquiry_reg.bayer_pattern.bayer_RG_avail;
		/* RAW 10 */
		case MEDIA_BUS_FMT_Y10_1X10:
			return feature_inquiry_reg.avail_mipi.raw10_avail &&
				bayer_inquiry_reg.bayer_pattern.monochrome_avail;
		case MEDIA_BUS_FMT_SGBRG10_1X10:
			return feature_inquiry_reg.avail_mipi.raw10_avail &&
				bayer_inquiry_reg.bayer_pattern.bayer_GB_avail;
		case MEDIA_BUS_FMT_SGRBG10_1X10:
			return feature_inquiry_reg.avail_mipi.raw10_avail &&
				bayer_inquiry_reg.bayer_pattern.bayer_GR_avail;
		case MEDIA_BUS_FMT_SRGGB10_1X10:
			return feature_inquiry_reg.avail_mipi.raw10_avail &&
				bayer_inquiry_reg.bayer_pattern.bayer_RG_avail;
		/* RAW 12 */
		case MEDIA_BUS_FMT_Y12_1X12:
			return feature_inquiry_reg.avail_mipi.raw12_avail &&
				bayer_inquiry_reg.bayer_pattern.monochrome_avail;
		case MEDIA_BUS_FMT_SBGGR12_1X12:
			return feature_inquiry_reg.avail_mipi.raw12_avail &&
				bayer_inquiry_reg.bayer_pattern.bayer_BG_avail;
		case MEDIA_BUS_FMT_SGBRG12_1X12:
			return feature_inquiry_reg.avail_mipi.raw12_avail &&
				bayer_inquiry_reg.bayer_pattern.bayer_GB_avail;
		case MEDIA_BUS_FMT_SGRBG12_1X12:
			return feature_inquiry_reg.avail_mipi.raw12_avail &&
				bayer_inquiry_reg.bayer_pattern.bayer_GR_avail;
		case MEDIA_BUS_FMT_SRGGB12_1X12:
			return feature_inquiry_reg.avail_mipi.raw12_avail &&
				bayer_inquiry_reg.bayer_pattern.bayer_RG_avail;
	}

	return false;
}

static int avt_ctrl_send(struct i2c_client *client,
		struct avt_ctrl *vc)
{
	struct camera_common_data *s_data = to_camera_common_data(&client->dev);
	struct avt_csi2_priv *priv = (struct avt_csi2_priv *)s_data->priv;
	int ret = 0;
	unsigned int reg = 0;
	int length = 0;

	uint32_t i2c_reg;
	uint32_t i2c_reg_size;
	uint32_t i2c_reg_count;

	char *i2c_reg_buf;

	//void *mipi_csi2_info;
	int r_wn = 0;/* write -> r_wn = 0, read -> r_wn = 1 */
	u64 avail_mipi = 0;
	union bcrm_avail_mipi_reg feature_inquiry_reg;
	union bcrm_bayer_inquiry_reg bayer_inquiry_reg;
	unsigned char bayer_val = 0;
	u64 temp = 0;
	int gencp_mode_local = 0;/* Default BCRM mode */
	__u8 bayer_temp = 0;

	bayer_inquiry_reg.value = 0;
	feature_inquiry_reg.value = 0;

	if (vc->id == V4L2_AV_CSI2_PIXELFORMAT_W) {

		/* read the MIPI format register to check whether the camera
		 * really support the requested pixelformat format
		 */
		ret = avt_reg_read(client,
				priv->cci_reg.bcrm_addr +
				BCRM_IMG_AVAILABLE_MIPI_DATA_FORMATS_64R,
				AV_CAM_REG_SIZE, AV_CAM_DATA_SIZE_64,
				(char *) &avail_mipi);

		if (ret < 0)
			dev_err(&client->dev, "i2c read failed (%d)\n", ret);

                if (avail_mipi == 0)
                {
                        /* Fallback app running? */
                        dev_warn(&client->dev, "avail_mipi=0. Fallback app running?");
                        avail_mipi = 0x80; // fake RGB888
                }
		feature_inquiry_reg.value = avail_mipi;

        	/* read the Bayer Inquiry register to check whether the camera
		 * really support the requested RAW format
		 */
		ret = avt_reg_read(client,
				priv->cci_reg.bcrm_addr +
				BCRM_IMG_BAYER_PATTERN_INQUIRY_8R,
				AV_CAM_REG_SIZE, AV_CAM_DATA_SIZE_8,
				(char *) &bayer_val);

		if (ret < 0)
			dev_err(&client->dev, "i2c read failed (%d)\n", ret);

		dev_dbg(&client->dev, "Bayer Inquiry Reg value : 0x%x\n",
				bayer_val);

		bayer_inquiry_reg.value = bayer_val;
	}

	switch (vc->id) {
	case V4L2_AV_CSI2_STREAMON_W:
		reg = BCRM_ACQUISITION_START_8RW;
		length = AV_CAM_DATA_SIZE_8;
		r_wn = 0;
		break;
	case V4L2_AV_CSI2_STREAMOFF_W:
		reg = BCRM_ACQUISITION_STOP_8RW;
		length = AV_CAM_DATA_SIZE_8;
		r_wn = 0;
		break;
	case V4L2_AV_CSI2_ABORT_W:
		reg = BCRM_ACQUISITION_ABORT_8RW;
		length = AV_CAM_DATA_SIZE_8;
		r_wn = 0;
		break;
	case V4L2_AV_CSI2_WIDTH_W:
		reg = BCRM_IMG_WIDTH_32RW;
		length = AV_CAM_DATA_SIZE_32;
		r_wn = 0;
		break;
	case V4L2_AV_CSI2_HEIGHT_W:
		reg = BCRM_IMG_HEIGHT_32RW;
		length = AV_CAM_DATA_SIZE_32;
		r_wn = 0;
		break;
	case V4L2_AV_CSI2_OFFSET_X_W:
		reg = BCRM_IMG_OFFSET_X_32RW;
		length = AV_CAM_DATA_SIZE_32;
		r_wn = 0;
		break;
	case V4L2_AV_CSI2_OFFSET_Y_W:
		reg = BCRM_IMG_OFFSET_Y_32RW;
		length = AV_CAM_DATA_SIZE_32;
		r_wn = 0;
		break;
	case V4L2_AV_CSI2_HFLIP_W:
		reg = BCRM_IMG_REVERSE_X_8RW;
		length = AV_CAM_DATA_SIZE_8;
		r_wn = 0;
		break;
	case V4L2_AV_CSI2_VFLIP_W:
		reg = BCRM_IMG_REVERSE_Y_8RW;
		length = AV_CAM_DATA_SIZE_8;
		r_wn = 0;
		break;
	case V4L2_AV_CSI2_PIXELFORMAT_W:
		reg = BCRM_IMG_MIPI_DATA_FORMAT_32RW;
		length = AV_CAM_DATA_SIZE_32;
		r_wn = 0;

		if(!avt_check_fmt_available(client, vc->value0)) {
				dev_err(&client->dev, "format 0x%x not supported\n", vc->value0);
				return -EINVAL;
		}

		switch (vc->value0) {
		case MEDIA_BUS_FMT_CUSTOM:
			vc->value0 = MIPI_DT_CUSTOM;
			break;
		case MEDIA_BUS_FMT_RGB444_1X12:
			vc->value0 = MIPI_DT_RGB444;
			break;
		case MEDIA_BUS_FMT_RGB565_1X16:
			vc->value0 = MIPI_DT_RGB565;
			break;
		case MEDIA_BUS_FMT_RGB888_1X24:
		case MEDIA_BUS_FMT_BGR888_1X24:
			vc->value0 = MIPI_DT_RGB888;
			break;
		case MEDIA_BUS_FMT_VYUY8_2X8:
			vc->value0 = MIPI_DT_YUV422;
			break;
		/* RAW 8 */
		case MEDIA_BUS_FMT_Y8_1X8:
			vc->value0 = MIPI_DT_RAW8;
			bayer_temp = monochrome;
			break;
		case MEDIA_BUS_FMT_SBGGR8_1X8:
			vc->value0 = MIPI_DT_RAW8;
			bayer_temp = bayer_bg;
			break;
		case MEDIA_BUS_FMT_SGBRG8_1X8:
			vc->value0 = MIPI_DT_RAW8;
			bayer_temp = bayer_gb;
			break;
		case MEDIA_BUS_FMT_SGRBG8_1X8:
			vc->value0 = MIPI_DT_RAW8;
			bayer_temp = bayer_gr;
			break;
		case MEDIA_BUS_FMT_SRGGB8_1X8:
			vc->value0 = MIPI_DT_RAW8;
			bayer_temp = bayer_rg;
			break;
		/* RAW 10 */
		case MEDIA_BUS_FMT_Y10_1X10:
			vc->value0 = MIPI_DT_RAW10;
			bayer_temp = monochrome;
			break;
		case MEDIA_BUS_FMT_SGBRG10_1X10:
			vc->value0 = MIPI_DT_RAW10;
			bayer_temp = bayer_gb;
			break;
		case MEDIA_BUS_FMT_SGRBG10_1X10:
			vc->value0 = MIPI_DT_RAW10;
			bayer_temp = bayer_gr;
			break;
		case MEDIA_BUS_FMT_SRGGB10_1X10:
			vc->value0 = MIPI_DT_RAW10;
			bayer_temp = bayer_rg;
			break;
		/* RAW 12 */
		case MEDIA_BUS_FMT_Y12_1X12:
			vc->value0 = MIPI_DT_RAW12;
			bayer_temp = monochrome;
			break;
		case MEDIA_BUS_FMT_SBGGR12_1X12:
			vc->value0 = MIPI_DT_RAW12;
			bayer_temp = bayer_bg;
			break;
		case MEDIA_BUS_FMT_SGBRG12_1X12:
			vc->value0 = MIPI_DT_RAW12;
			bayer_temp = bayer_gb;
			break;
		case MEDIA_BUS_FMT_SGRBG12_1X12:
			vc->value0 = MIPI_DT_RAW12;
			bayer_temp = bayer_gr;
			break;
		case MEDIA_BUS_FMT_SRGGB12_1X12:
			vc->value0 = MIPI_DT_RAW12;
			bayer_temp = bayer_rg;
			break;

		case 0:
			/* Fallback app running */
			dev_warn(&client->dev, "Invalid pixelformat detected (0). Fallback app running?");
			vc->value0 = MIPI_DT_RGB888;
			break;       

		default:
			dev_err(&client->dev, "%s: format 0x%x not supported by the host\n",
					__func__, vc->value0);
			return -EINVAL;
		}
		break;

	case V4L2_AV_CSI2_WIDTH_R:
		reg = BCRM_IMG_WIDTH_32RW;
		length = AV_CAM_DATA_SIZE_32;
		r_wn = 1;
		break;
	case V4L2_AV_CSI2_WIDTH_MINVAL_R:
		reg = BCRM_IMG_WIDTH_MIN_32R;
		length = AV_CAM_DATA_SIZE_32;
		r_wn = 1;
		break;
	case V4L2_AV_CSI2_WIDTH_MAXVAL_R:
		reg = BCRM_IMG_WIDTH_MAX_32R;
		length = AV_CAM_DATA_SIZE_32;
		r_wn = 1;
		break;
	case V4L2_AV_CSI2_WIDTH_INCVAL_R:
		reg = BCRM_IMG_WIDTH_INC_32R;
		length = AV_CAM_DATA_SIZE_32;
		r_wn = 1;
		break;
	case V4L2_AV_CSI2_HEIGHT_R:
		reg = BCRM_IMG_HEIGHT_32RW;
		length = AV_CAM_DATA_SIZE_32;
		r_wn = 1;
		break;
	case V4L2_AV_CSI2_HEIGHT_MINVAL_R:
		reg = BCRM_IMG_HEIGHT_MIN_32R;
		length = AV_CAM_DATA_SIZE_32;
		r_wn = 1;
		break;
	case V4L2_AV_CSI2_HEIGHT_MAXVAL_R:
		reg = BCRM_IMG_HEIGHT_MAX_32R;
		length = AV_CAM_DATA_SIZE_32;
		r_wn = 1;
		break;
	case V4L2_AV_CSI2_HEIGHT_INCVAL_R:
		reg = BCRM_IMG_HEIGHT_INC_32R;
		length = AV_CAM_DATA_SIZE_32;
		r_wn = 1;
		break;
	case V4L2_AV_CSI2_OFFSET_X_R:
		reg = BCRM_IMG_OFFSET_X_32RW;
		length = AV_CAM_DATA_SIZE_32;
		r_wn = 1;
		break;
	case V4L2_AV_CSI2_OFFSET_X_MIN_R:
		reg = BCRM_IMG_OFFSET_X_MIN_32R;
		length = AV_CAM_DATA_SIZE_32;
		r_wn = 1;
		break;
	case V4L2_AV_CSI2_OFFSET_X_MAX_R:
		reg = BCRM_IMG_OFFSET_X_MAX_32R;
		length = AV_CAM_DATA_SIZE_32;
		r_wn = 1;
		break;
	case V4L2_AV_CSI2_OFFSET_X_INC_R:
		reg = BCRM_IMG_OFFSET_X_INC_32R;
		length = AV_CAM_DATA_SIZE_32;
		r_wn = 1;
		break;
	case V4L2_AV_CSI2_OFFSET_Y_R:
		reg = BCRM_IMG_OFFSET_Y_32RW;
		length = AV_CAM_DATA_SIZE_32;
		r_wn = 1;
		break;
	case V4L2_AV_CSI2_OFFSET_Y_MIN_R:
		reg = BCRM_IMG_OFFSET_Y_MIN_32R;
		length = AV_CAM_DATA_SIZE_32;
		r_wn = 1;
		break;
	case V4L2_AV_CSI2_OFFSET_Y_MAX_R:
		reg = BCRM_IMG_OFFSET_Y_MAX_32R;
		length = AV_CAM_DATA_SIZE_32;
		r_wn = 1;
		break;
	case V4L2_AV_CSI2_OFFSET_Y_INC_R:
		reg = BCRM_IMG_OFFSET_Y_INC_32R;
		length = AV_CAM_DATA_SIZE_32;
		r_wn = 1;
		break;
	case V4L2_AV_CSI2_SENSOR_WIDTH_R:
		reg = BCRM_SENSOR_WIDTH_32R;
		length = AV_CAM_DATA_SIZE_32;
		r_wn = 1;
		break;
	case V4L2_AV_CSI2_SENSOR_HEIGHT_R:
		reg = BCRM_SENSOR_HEIGHT_32R;
		length = AV_CAM_DATA_SIZE_32;
		r_wn = 1;
		break;
	case V4L2_AV_CSI2_MAX_WIDTH_R:
		reg = BCRM_WIDTH_MAX_32R;
		length = AV_CAM_DATA_SIZE_32;
		r_wn = 1;
		break;
	case V4L2_AV_CSI2_MAX_HEIGHT_R:
		reg = BCRM_HEIGHT_MAX_32R;
		length = AV_CAM_DATA_SIZE_32;
		r_wn = 1;
		break;
	case V4L2_AV_CSI2_PIXELFORMAT_R:
		reg = BCRM_IMG_MIPI_DATA_FORMAT_32RW;
		length = AV_CAM_DATA_SIZE_32;
		r_wn = 1;
		break;
	case V4L2_AV_CSI2_PALYLOADSIZE_R:
		reg = BCRM_BUFFER_SIZE_32R;
		length = AV_CAM_DATA_SIZE_32;
		r_wn = 1;
		break;
	case V4L2_AV_CSI2_ACQ_STATUS_R:
		reg = BCRM_ACQUISITION_STATUS_8R;
		length = AV_CAM_DATA_SIZE_8;
		r_wn = 1;
		break;
	case V4L2_AV_CSI2_HFLIP_R:
		reg = BCRM_IMG_REVERSE_X_8RW;
		length = AV_CAM_DATA_SIZE_8;
		r_wn = 1;
		break;
	case V4L2_AV_CSI2_VFLIP_R:
		reg = BCRM_IMG_REVERSE_Y_8RW;
		length = AV_CAM_DATA_SIZE_8;
		r_wn = 1;
		break;
	case V4L2_AV_CSI2_CURRENTMODE_R:
		reg = CCI_CURRENT_MODE_8R;
		length = AV_CAM_DATA_SIZE_8;
		gencp_mode_local = 1;
		r_wn = 1;
		break;
	case V4L2_AV_CSI2_CHANGEMODE_W:
		reg = CCI_CHANGE_MODE_8W;
		length = AV_CAM_DATA_SIZE_8;
		gencp_mode_local = 1;
		if (vc->value0 == MIPI_DT_CUSTOM)
			priv->mode = AVT_GENCP_MODE;
		else
			priv->mode = AVT_BCRM_MODE;
		r_wn = 0;
		break;
	default:
		dev_err(&client->dev, "%s: unknown ctrl 0x%x\n",
				__func__, vc->id);
		return -EINVAL;
	}

	if (r_wn) {/* read (r_wn=1) */

		if (gencp_mode_local) {

			ret = avt_reg_read(client,
					reg, AV_CAM_REG_SIZE, length,
					(char *) &vc->value0);

			if (ret < 0) {
				dev_err(&client->dev, "i2c read failed (%d)\n",
						ret);
				return ret;
			}
			return 0;
		}

		ret = avt_reg_read(client,
				priv->cci_reg.bcrm_addr + reg,
				AV_CAM_REG_SIZE, length,
				(char *) &vc->value0);

		if (ret < 0) {
			dev_err(&client->dev, "i2c read failed (%d)\n", ret);
			return ret;
		}

		if (vc->id == V4L2_AV_CSI2_PIXELFORMAT_R) {
			/* To avoid ambiguity, resulting from
			 * two MBUS formats linked with
			 * one camera image format,
			 * we return value stored in private data
			 */
			vc->value0 = priv->mbus_fmt_code;
		}

		return 0;

	} else {/* write (r_wn=0) */
		dev_dbg(&client->dev, "reg %x, length %d, vc->value0 0x%x\n",
				reg, length, vc->value0);

		if (gencp_mode_local) {

			i2c_reg = reg;
			i2c_reg_size = AV_CAM_REG_SIZE;
			i2c_reg_count = length;

			if (length > AV_CAM_DATA_SIZE_32)
				i2c_reg_buf = (char *) &temp;
			else
				i2c_reg_buf = (char *) &vc->value0;

			ret = ioctl_gencam_i2cwrite_reg(client,
					i2c_reg, i2c_reg_size,
					i2c_reg_count, i2c_reg_buf);

			if (ret < 0) {
				dev_err(&client->dev, "%s:%d i2c write failed\n",
						__func__, __LINE__);
				return ret;
			} else {
				return 0;
			}
		}

		if (vc->id == V4L2_AV_CSI2_PIXELFORMAT_W) {
			/* Set pixelformat then set bayer format, refer OCT-2417
			 *
			 * XXX implement these somehow, below imx6 code:
			 * mipi_csi2_info = mipi_csi2_get_info();
			 * mipi_csi2_set_datatype(mipi_csi2_info, vc->value0);
			 */
		}

		temp = vc->value0;

		if (length > AV_CAM_DATA_SIZE_32)
			swapbytes(&temp, length);
		else
			swapbytes(&vc->value0, length);

		i2c_reg       = priv->cci_reg.bcrm_addr + reg;
		i2c_reg_size  = AV_CAM_REG_SIZE;
		i2c_reg_count = length;

		if (length > AV_CAM_DATA_SIZE_32)
			i2c_reg_buf = (char *) &temp;
		else
			i2c_reg_buf = (char *) &vc->value0;

		ret = ioctl_gencam_i2cwrite_reg(client, i2c_reg, i2c_reg_size,
						i2c_reg_count, i2c_reg_buf);

		if (ret < 0) {
			dev_err(&client->dev, "%s:%d i2c write failed\n",
					__func__, __LINE__);
			return ret;
		}

		/* Set pixelformat then set bayer format, refer OCT-2417 */
		if (vc->id == V4L2_AV_CSI2_PIXELFORMAT_W) {
			ret = set_bayer_format(client, bayer_temp);
			if (ret < 0) {
				dev_err(&client->dev, "%s:%d i2c write failed, ret %d\n",
						__func__, __LINE__, ret);
				return ret;
			}
		}

		return 0;
	}
}

static void set_channel_avt_cam_mode(struct v4l2_subdev *sd, bool cam_mode)
{
	struct tegra_channel *tch;
	struct media_pad *pad_csi, *pad_vi;
	struct v4l2_subdev *sd_csi, *sd_vi;
	struct video_device *vdev_vi;

	if (!sd->entity.pads)
		return;

	pad_csi = media_entity_remote_pad(&sd->entity.pads[0]);
	sd_csi = media_entity_to_v4l2_subdev(pad_csi->entity);
	pad_vi = media_entity_remote_pad(&sd_csi->entity.pads[1]);
	sd_vi = media_entity_to_v4l2_subdev(pad_vi->entity);
	vdev_vi = media_entity_to_video_device(pad_vi->entity);
	tch = video_get_drvdata(vdev_vi);

	tch->avt_cam_mode = cam_mode;
}

static void set_channel_trigger_mode(struct v4l2_subdev *sd, bool trigger_mode)
{
	struct tegra_channel *tch;
	struct media_pad *pad_csi, *pad_vi;
	struct v4l2_subdev *sd_csi, *sd_vi;
	struct video_device *vdev_vi;

	if (!sd->entity.pads)
		return;

	pad_csi = media_entity_remote_pad(&sd->entity.pads[0]);
	sd_csi = media_entity_to_v4l2_subdev(pad_csi->entity);
	pad_vi = media_entity_remote_pad(&sd_csi->entity.pads[1]);
	sd_vi = media_entity_to_v4l2_subdev(pad_vi->entity);
	vdev_vi = media_entity_to_video_device(pad_vi->entity);
	tch = video_get_drvdata(vdev_vi);

	tch->trigger_mode = trigger_mode;
}

static void set_channel_pending_trigger(struct v4l2_subdev *sd)
{
	struct tegra_channel *tch;
	struct media_pad *pad_csi, *pad_vi;
	struct v4l2_subdev *sd_csi, *sd_vi;
	struct video_device *vdev_vi;

	if (!sd->entity.pads)
		return;

	pad_csi = media_entity_remote_pad(&sd->entity.pads[0]);
	sd_csi = media_entity_to_v4l2_subdev(pad_csi->entity);
	pad_vi = media_entity_remote_pad(&sd_csi->entity.pads[1]);
	sd_vi = media_entity_to_v4l2_subdev(pad_vi->entity);
	vdev_vi = media_entity_to_video_device(pad_vi->entity);
	tch = video_get_drvdata(vdev_vi);

	tch->pending_trigger = true;
}

static void set_channel_timeout(struct v4l2_subdev *sd, unsigned long timeout)
{
	struct tegra_channel *tch;
	struct media_pad *pad_csi, *pad_vi;
	struct v4l2_subdev *sd_csi, *sd_vi;
	struct video_device *vdev_vi;

	if (!sd->entity.pads)
		return;

	pad_csi = media_entity_remote_pad(&sd->entity.pads[0]);
	sd_csi = media_entity_to_v4l2_subdev(pad_csi->entity);
	pad_vi = media_entity_remote_pad(&sd_csi->entity.pads[1]);
	sd_vi = media_entity_to_v4l2_subdev(pad_vi->entity);
	vdev_vi = media_entity_to_video_device(pad_vi->entity);
	tch = video_get_drvdata(vdev_vi);

	if (timeout == AVT_TEGRA_TIMEOUT_DISABLED) {
		tch->timeout = timeout;
	}
	else
		tch->timeout = msecs_to_jiffies(timeout);
}

static void set_channel_stride_align(struct v4l2_subdev *sd, uint8_t align)
{
	struct tegra_channel *tch;
	struct media_pad *pad_csi, *pad_vi;
	struct v4l2_subdev *sd_csi, *sd_vi;
	struct video_device *vdev_vi;

	if (!sd->entity.pads)
		return;

	pad_csi = media_entity_remote_pad(&sd->entity.pads[0]);
	sd_csi = media_entity_to_v4l2_subdev(pad_csi->entity);
	pad_vi = media_entity_remote_pad(&sd_csi->entity.pads[1]);
	sd_vi = media_entity_to_v4l2_subdev(pad_vi->entity);
	vdev_vi = media_entity_to_video_device(pad_vi->entity);
	tch = video_get_drvdata(vdev_vi);

	tch->stride_align = align;
}

static void set_channel_stride_align_for_format(struct v4l2_subdev *sd, uint32_t mbus_code)
{
	/* Set stride alignment required for the format */
	switch (mbus_code) {
	case MEDIA_BUS_FMT_RGB888_1X24:
	case MEDIA_BUS_FMT_BGR888_1X24:
		set_channel_stride_align(sd, 16);
		break;
	case MEDIA_BUS_FMT_VYUY8_2X8:
	case MEDIA_BUS_FMT_RGB565_1X16:
		set_channel_stride_align(sd, 32);
		break;
	case MEDIA_BUS_FMT_CUSTOM:
		set_channel_stride_align(sd, 64);
		break;
	/* 8 bit bayer formats */
	case MEDIA_BUS_FMT_SBGGR8_1X8:
	case MEDIA_BUS_FMT_SGBRG8_1X8:
	case MEDIA_BUS_FMT_SGRBG8_1X8:
	case MEDIA_BUS_FMT_SRGGB8_1X8:
	case MEDIA_BUS_FMT_Y8_1X8:
		set_channel_stride_align(sd, 64);
		break;
	/* the remaining formats */
	default:
		set_channel_stride_align(sd, 1);
		break;
	}
}

static int avt_tegra_s_ctrl(struct v4l2_ctrl *ctrl)
{
	struct v4l2_subdev *sd;
	struct avt_csi2_priv *priv;
	unsigned long timeout;
	int i;

	priv = container_of(ctrl->handler, struct avt_csi2_priv, hdl);
	sd = priv->subdev;

	switch (ctrl->id) {
	case AVT_TEGRA_TIMEOUT:
		if (ctrl->val == 0)
			set_channel_timeout(sd, AVT_TEGRA_TIMEOUT_DISABLED);
		else {
			for (i = 0; i < ARRAY_SIZE(priv->ctrls); ++i) {
				if (priv->ctrls[i]->id == AVT_TEGRA_TIMEOUT_VALUE) {
					timeout = priv->ctrls[i]->val;
					set_channel_timeout(sd, timeout);
					return 0;
				}
			}
		}
		break;
	case AVT_TEGRA_TIMEOUT_VALUE:
		for (i = 0; i < ARRAY_SIZE(priv->ctrls); ++i) {
			/* First check if the timouet is not disabled */
			if (priv->ctrls[i]->id == AVT_TEGRA_TIMEOUT) {
				if (priv->ctrls[i]->val == 0)
					return 0;
				else
					break;
			}
		}

		/* If it is not disabled, set it in HW */
		set_channel_timeout(sd, ctrl->val);
		break;
	case AVT_TEGRA_STRIDE_ALIGN:
		if (ctrl->val == 0)
			priv->stride_align_enabled = false;
		else
			priv->stride_align_enabled = true;
		break;
	case AVT_TEGRA_CROP_ALIGN:
		if (ctrl->val == 0)
			priv->crop_align_enabled = false;
		else
			priv->crop_align_enabled = true;
		break;
	}

	return 0;
}

static const struct v4l2_ctrl_ops avt_tegra_ctrl_ops = {
	.s_ctrl = avt_tegra_s_ctrl,
};

static const struct v4l2_ctrl_config avt_tegra_ctrl[] = {
	{
		.ops = &avt_tegra_ctrl_ops,
		.id = AVT_TEGRA_TIMEOUT,
		.name = "Frame timeout enabled",
		.type = V4L2_CTRL_TYPE_BOOLEAN,
		.def = 1,
		.min = 0,
		.max = 1,
		.step = 1,
	},
	{
		.ops = &avt_tegra_ctrl_ops,
		.id = AVT_TEGRA_TIMEOUT_VALUE,
		.name = "Frame timeout",
		.type = V4L2_CTRL_TYPE_INTEGER,
		.min = 100,
		.max = 12000,
		.step = 1,
		.def = AVT_TEGRA_TIMEOUT_DEFAULT,
	},
	{
		.ops = &avt_tegra_ctrl_ops,
		.id = AVT_TEGRA_STRIDE_ALIGN,
		.name = "Stride alignment enabled",
		.type = V4L2_CTRL_TYPE_BOOLEAN,
		.def = 1,
		.min = 0,
		.max = 1,
		.step = 1,
	},
	{
		.ops = &avt_tegra_ctrl_ops,
		.id = AVT_TEGRA_CROP_ALIGN,
		.name = "Crop alignment enabled",
		.type = V4L2_CTRL_TYPE_BOOLEAN,
		.def = 1,
		.min = 0,
		.max = 1,
		.step = 1,
	},
};

/* --------------- INIT --------------- */

static int avt_csi2_subscribe_event(struct v4l2_subdev *sd, struct v4l2_fh *fh,
		struct v4l2_event_subscription *sub)
{
	switch (sub->type) {
	case V4L2_EVENT_SOURCE_CHANGE:
		return v4l2_src_change_event_subdev_subscribe(sd, fh, sub);
	case V4L2_EVENT_CTRL:
		return v4l2_ctrl_subdev_subscribe_event(sd, fh, sub);
	default:
		return -EINVAL;
	}
}


/* --------------- CUSTOM IOCTLS --------------- */

long avt_csi2_ioctl(struct v4l2_subdev *sd, unsigned int cmd, void *arg)
{
	int ret = -ENOTTY;
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	struct avt_csi2_priv *priv = avt_get_priv(sd);
	struct v4l2_i2c *i2c_reg;
	char *i2c_reg_buf;
	int *i2c_clk_freq;
	struct v4l2_gencp_buffer_sizes *gencp_buf_sz;
	struct v4l2_csi_driver_info *info;
	struct v4l2_csi_config *config;
	uint32_t i2c_reg_address;
	uint32_t i2c_reg_size;
	uint32_t i2c_reg_count;
	uint32_t clk;
	uint8_t avt_supported_lane_counts = 0;
	uint32_t avt_min_clk = 0;
	uint32_t avt_max_clk = 0;
	uint32_t common_min_clk = 0;
	uint32_t common_max_clk = 0;
	uint32_t common_inc_clk = 0;

	avt_dbg(sd, "%s(cmd=%u)\n", __func__, cmd);

	switch(cmd) {
	case VIDIOC_R_I2C:
		avt_dbg(sd, "VIDIOC_R_I2C\n");
		i2c_reg = (struct v4l2_i2c *)arg;
		i2c_reg_buf = kzalloc(i2c_reg->num_bytes, GFP_KERNEL);
		if (!i2c_reg_buf)
			return -ENOMEM;

		ret = i2c_read(client, i2c_reg->register_address, i2c_reg->register_size,
				i2c_reg->num_bytes, i2c_reg_buf);

		if (ret < 0)
			avt_err(sd, " I2C read failed. addr=0x%04X, num_bytes=%d, ret=%d\n", i2c_reg->register_address, i2c_reg->num_bytes, ret);
		else
		{
			ret = copy_to_user((char *)i2c_reg->ptr_buffer, i2c_reg_buf, i2c_reg->num_bytes);

			if (ret == 0)
				avt_dbg(sd, " I2C read success. addr=0x%04X, num_bytes=%d, ret=%d\n", i2c_reg->register_address, i2c_reg->num_bytes, ret);
			else
				avt_err(sd, " I2C read failed. copy_to_user failed. addr=0x%04X, num_bytes=%d, ret=%d\n", i2c_reg->register_address, i2c_reg->num_bytes, ret);
		}

		kfree(i2c_reg_buf);
		break;

	case VIDIOC_W_I2C:
		avt_dbg(sd, "VIDIOC_W_I2C\n");
		i2c_reg = (struct v4l2_i2c *)arg;

		i2c_reg_buf = kzalloc(i2c_reg->num_bytes, GFP_KERNEL);
		if (!i2c_reg_buf)
			return -ENOMEM;

		ret = copy_from_user(i2c_reg_buf, (char *) i2c_reg->ptr_buffer, i2c_reg->num_bytes);

		ret = ioctl_gencam_i2cwrite_reg(client, i2c_reg->register_address, i2c_reg->register_size, i2c_reg->num_bytes, i2c_reg_buf);

		if (ret < 0)
		{
			avt_err(sd, " I2C write failed. addr=0x%04X, num_bytes=%d, ret=%d\n", i2c_reg->register_address, i2c_reg->num_bytes, ret);
		}
		/* Check if mode (BCRM or GenCP) is changed */
		else
		{
			avt_dbg(sd, " I2C write success. addr=0x%04X, num_bytes=%d, ret=%d\n", i2c_reg->register_address, i2c_reg->num_bytes, ret);
            
			if(i2c_reg->register_address == CCI_CHANGE_MODE_8W)
			{
				priv->mode = i2c_reg_buf[0] == 0 ? AVT_BCRM_MODE : AVT_GENCP_MODE;
				set_channel_avt_cam_mode(sd, priv->mode);
				if (priv->mode)
					set_channel_timeout(sd, AVT_TEGRA_TIMEOUT_DISABLED);
				else
					set_channel_timeout(sd, CAPTURE_TIMEOUT_MS);
			}
		}

		break;

	case VIDIOC_G_I2C_CLOCK_FREQ:
		avt_dbg(sd, "VIDIOC_G_I2C_CLOCK_FREQ\n");

		i2c_clk_freq = arg;
		*i2c_clk_freq = i2c_get_adapter_bus_clk_rate(client->adapter);
		ret = 0;

		break;

	case VIDIOC_G_GENCP_BUFFER_SIZES:
		avt_dbg(sd, "VIDIOC_G_GENCP_BUFFER_SIZE\n");
		gencp_buf_sz = arg;
		gencp_buf_sz->gencp_in_buffer_size = priv->gencp_reg.gencp_in_buffer_size;
		gencp_buf_sz->gencp_out_buffer_size = priv->gencp_reg.gencp_out_buffer_size;
		ret = 0;
		break;

	case VIDIOC_G_DRIVER_INFO:
		avt_dbg(sd, "VIDIOC_G_DRIVER_INFO\n");
		info = (struct v4l2_csi_driver_info *)arg;

		info->id.manufacturer_id = MANUFACTURER_ID_NVIDIA;
		info->id.soc_family_id = SOC_FAMILY_ID_TEGRA;
		info->id.driver_id = TEGRA_DRIVER_ID_DEFAULT;

		info->driver_version = (DRV_VER_MAJOR << 16) + (DRV_VER_MINOR << 8) + DRV_VER_PATCH;
		info->driver_interface_version = (LIBCSI_DRV_SPEC_VERSION_MAJOR << 16) + (LIBCSI_DRV_SPEC_VERSION_MINOR << 8) + LIBCSI_DRV_SPEC_VERSION_PATCH;
		info->driver_caps = AVT_DRVCAP_MMAP | AVT_DRVCAP_USRPTR;
		info->usrptr_alignment = dma_get_cache_alignment();

		ret = 0;
		break;

	case VIDIOC_G_CSI_CONFIG:
		avt_dbg(sd, "VIDIOC_G_CSI_CONFIG\n");
		config = (struct v4l2_csi_config *)arg;

		config->lane_count = priv->numlanes;
		config->csi_clock = priv->csi_clk_freq;

		ret = 0;
		break;

	case VIDIOC_S_CSI_CONFIG:
		avt_dbg(sd, "VIDIOC_S_CSI_CONFIG\n");
		config = (struct v4l2_csi_config *)arg;

		/* Set number of lanes */
		priv->s_data->numlanes = config->lane_count;

		ret = avt_reg_read(priv->client,
				priv->cci_reg.bcrm_addr + BCRM_SUPPORTED_CSI2_LANE_COUNTS_8R,
				AV_CAM_REG_SIZE, AV_CAM_DATA_SIZE_8,
				(char *) &avt_supported_lane_counts);
		if (ret < 0) {
			avt_err(sd, " BCRM_SUPPORTED_CSI2_LANE_COUNTS_8R: i2c read failed (%d)\n", ret);
			ret = -1;
			break;
		}
		if(!(test_bit(priv->s_data->numlanes - 1, (const long *)(&avt_supported_lane_counts)))) {
			avt_err(sd, " requested number of lanes (%u) not supported by this camera!\n",
					priv->s_data->numlanes);
			ret = -1;
			break;
		}
		ret = avt_reg_write(priv->client,
				priv->cci_reg.bcrm_addr + BCRM_CSI2_LANE_COUNT_8RW,
				priv->s_data->numlanes);
		if (ret < 0){
			avt_err(sd, " i2c write failed (%d)\n", ret);
			ret = -1;
			break;
		}
		priv->numlanes = priv->s_data->numlanes;

		/* Set CSI clock frequency */

		ret = avt_reg_read(priv->client,
				priv->cci_reg.bcrm_addr + BCRM_CSI2_LANE_COUNT_8RW,
				AV_CAM_REG_SIZE, AV_CAM_DATA_SIZE_32,
				(char *) &avt_min_clk);
	
		if (ret < 0) {
			avt_err(sd, " BCRM_CSI2_LANE_COUNT_8RW: i2c read failed (%d)\n", ret);
			ret = -1;
			break;
		}
	
		ret = avt_reg_read(priv->client,
				priv->cci_reg.bcrm_addr + BCRM_CSI2_CLOCK_MAX_32R,
				AV_CAM_REG_SIZE, AV_CAM_DATA_SIZE_32,
				(char *) &avt_max_clk);
	
		if (ret < 0) {
			avt_err(sd, " BCRM_CSI2_CLOCK_MAX_32R: i2c read failed (%d)\n", ret);
			ret = -1;
			break;
		}

		if (common_range(avt_min_clk, avt_max_clk, 1,
				config->csi_clock, config->csi_clock, 1,
				&common_min_clk, &common_max_clk, &common_inc_clk)
				== false) {
			avt_err(sd, " clock value does not fit the supported frequency range!\n");
			return -EINVAL;
		}

		CLEAR(i2c_reg_address);
		clk = config->csi_clock;
		swapbytes(&clk, AV_CAM_DATA_SIZE_32);
		i2c_reg_address = priv->cci_reg.bcrm_addr + BCRM_CSI2_CLOCK_32RW;
		i2c_reg_size = AV_CAM_REG_SIZE;
		i2c_reg_count = AV_CAM_DATA_SIZE_32;
		i2c_reg_buf = (char *) &clk;
		ret = ioctl_gencam_i2cwrite_reg(priv->client, i2c_reg_address, i2c_reg_size,
						i2c_reg_count, i2c_reg_buf);
		if (ret < 0) {
			avt_err(sd, " BCRM_CSI2_CLOCK_32RW: i2c write failed (%d)\n", ret);
			ret = -1;
			break;
		}

		/* Read back CSI clock frequency */
		ret = avt_reg_read(priv->client,
				priv->cci_reg.bcrm_addr + BCRM_CSI2_CLOCK_32RW,
				AV_CAM_REG_SIZE, AV_CAM_DATA_SIZE_32,
				(char *) &priv->csi_clk_freq);

		if (ret < 0) {
			avt_err(sd, "BCRM_CSI2_CLOCK_32RW: i2c read failed (%d)\n", ret);
			ret = -1;
			break;
		}
		ret = 0;
		break;

#if 0
	case VIDIOC_S_TRIGGER_STATUS:

		/* Setting '1' to the Frame Start Trigger mode */
		ret = avt_reg_write(client,
				priv->cci_reg.bcrm_addr + FRAME_START_TRIGGER_MODE_8RW, 1);
		if (ret < 0)
			return ret;
		set_channel_trigger_mode(sd, true);

		/* Check if we need to write to Trigger Source register.
		 * If we do this more than once in the power cycle,
		 * triggering non-empty frames is not  possible.
		 */
		ret = avt_reg_read(client,
			priv->cci_reg.bcrm_addr +
			FRAME_START_TRIGGER_SOURCE_8RW,
			AV_CAM_REG_SIZE, AV_CAM_DATA_SIZE_8,
			(char *) &trigger_source);
		if (ret < 0)
			return ret;
		if (trigger_source == AV_CAM_SOFTWARE_TRIGGER)
			avt_err(sd, "Trigger source already set to software!\n");
		else {
			/* Setting Frame Start Trigger Source to 'software' */
			ret = avt_reg_write(client,
					priv->cci_reg.bcrm_addr + FRAME_START_TRIGGER_SOURCE_8RW, AV_CAM_SOFTWARE_TRIGGER);
			if (ret < 0)
				return ret;
		}

		/* Setting Frame Start Trigger Activation to 'any edge' */
		ret = avt_reg_write(client,
				priv->cci_reg.bcrm_addr + FRAME_START_TRIGGER_ACTIVATION_8RW, 2);
		if (ret < 0)
			return ret;

		ret = 0;
		break;

	case VIDIOC_G_TRIGGER_STATUS:

		trigger_status = (struct v4l2_trigger_status *)arg;

		ret = avt_reg_read(client,
			priv->cci_reg.bcrm_addr +
			FRAME_START_TRIGGER_MODE_8RW,
			AV_CAM_REG_SIZE, AV_CAM_DATA_SIZE_8,
			(char *) &trigger_status->trigger_mode_enabled);
		if (ret < 0)
			return ret;

		ret = avt_reg_read(client,
			priv->cci_reg.bcrm_addr +
			FRAME_START_TRIGGER_SOURCE_8RW,
			AV_CAM_REG_SIZE, AV_CAM_DATA_SIZE_8,
			(char *) &trigger_status->trigger_source);
		if (ret < 0)
			return ret;

		ret = avt_reg_read(client,
			priv->cci_reg.bcrm_addr +
			FRAME_START_TRIGGER_ACTIVATION_8RW,
			AV_CAM_REG_SIZE, AV_CAM_DATA_SIZE_8,
			(char *) &trigger_status->trigger_activation);
		if (ret < 0)
			return ret;

		ret = 0;
		break;
#endif

	case VIDIOC_TRIGGER_SOFTWARE:
    {
        u8 trigger_source;
        /* Sanity check */
        ret = avt_reg_read(client, priv->cci_reg.bcrm_addr + BCRM_FRAME_START_TRIGGER_SOURCE_8RW, AV_CAM_REG_SIZE, AV_CAM_DATA_SIZE_8, (char *) &trigger_source);

        if (ret < 0) {
            return ret;
        }

        if (trigger_source != AV_CAM_SOFTWARE_TRIGGER) {
            return -EPERM;
        }
    }

	/* Check if stream is already on */
	if (!priv->stream_on)
		return -EAGAIN;

    /* Generate Trigger */
    ret = avt_reg_write(client,
        priv->cci_reg.bcrm_addr + BCRM_FRAME_START_TRIGGER_SOFTWARE_8W, 1);
    

		if (ret < 0) {
			avt_err(sd, "generating trigger failed (%d)\n", ret);
			return ret;
		}
		set_channel_pending_trigger(sd);
		ret = 0;
		break;

	case VIDIOC_TRIGGER_MODE_OFF:

		/* Setting '0' to the Frame Start Trigger mode */
		ret = avt_reg_write(client,
				priv->cci_reg.bcrm_addr + BCRM_FRAME_START_TRIGGER_MODE_8RW, 0);
		if (ret < 0) {
			return ret;
        }
		set_channel_trigger_mode(sd, false);
		set_channel_timeout(sd, CAPTURE_TIMEOUT_MS);
		ret = 0;
		break;

	case VIDIOC_TRIGGER_MODE_ON:

		/* Setting '1' to the Frame Start Trigger mode */
		ret = avt_reg_write(client,
				priv->cci_reg.bcrm_addr + BCRM_FRAME_START_TRIGGER_MODE_8RW, 1);
		//pr_err("trigger mode enabled; ret: %d\n", ret);
		if (ret < 0) {
			return ret;
        }
		set_channel_trigger_mode(sd, true);
		set_channel_timeout(sd, AVT_TEGRA_TIMEOUT_DISABLED);
		ret = 0;
		break;

	case VIDIOC_S_TRIGGER_ACTIVATION:
    {
        int trigger_activation = *(int*)arg;

        /* Setting Frame Start Trigger Activation */
        ret = avt_reg_write(client,
            priv->cci_reg.bcrm_addr + BCRM_FRAME_START_TRIGGER_ACTIVATION_8RW, trigger_activation);

        if (ret < 0) {
            return ret;
        }

        ret = 0;
        break;
    }

	case VIDIOC_G_TRIGGER_ACTIVATION:
    {
        u8 trigger_activation;
        ret = avt_reg_read(client,
            priv->cci_reg.bcrm_addr + BCRM_FRAME_START_TRIGGER_ACTIVATION_8RW,
            AV_CAM_REG_SIZE, AV_CAM_DATA_SIZE_8,
            &trigger_activation);

        if (ret < 0) {
            return ret;
        }

        *(int*)arg = trigger_activation;

        ret = 0;
        break;
    }

	case VIDIOC_G_TRIGGER_SOURCE:
    {
        int *trigger_source = (int*)arg;
        u8 trigger_source_reg;
        
        ret = avt_reg_read(client,
        priv->cci_reg.bcrm_addr +
        BCRM_FRAME_START_TRIGGER_SOURCE_8RW,
        AV_CAM_REG_SIZE, AV_CAM_DATA_SIZE_8,
        (char *) &trigger_source_reg);

        if (ret < 0) {
            return ret;
        }
        
        switch(trigger_source_reg) {
        case 4:
            *trigger_source = V4L2_TRIGGER_SOURCE_SOFTWARE;
            break;
        case 0:
            *trigger_source = V4L2_TRIGGER_SOURCE_LINE0;
            break;
        case 1:
            *trigger_source = V4L2_TRIGGER_SOURCE_LINE1;
            break;
        case 2:
            *trigger_source = V4L2_TRIGGER_SOURCE_LINE2;
            break;
        case 3:
            *trigger_source = V4L2_TRIGGER_SOURCE_LINE3;
            break;
        default:
            avt_err(sd, " Unknown trigger mode (%d) returned from camera. Driver outdated?", trigger_source_reg);
            return -1;
        }

        ret = 0;
        break;
    }

	case VIDIOC_S_TRIGGER_SOURCE:
    {
        u8 cur_trigger_source;
        int trigger_source = *(int*)arg;
        u8 trigger_source_reg;
        
        switch(trigger_source) {
        case V4L2_TRIGGER_SOURCE_SOFTWARE:
            trigger_source_reg = 4;
            break;
        case V4L2_TRIGGER_SOURCE_LINE0:
            trigger_source_reg = 0;
            break;
        case V4L2_TRIGGER_SOURCE_LINE1:
            trigger_source_reg = 1;
            break;
        case V4L2_TRIGGER_SOURCE_LINE2:
            trigger_source_reg = 2;
            break;
        case V4L2_TRIGGER_SOURCE_LINE3:
            trigger_source_reg = 3;
            break;
        default:
            avt_err(sd, " invalid trigger source (%d)", trigger_source);
            return -1;
    }

      /* Check if we need to write to Trigger Source register.
       * If we do this more than once in the power cycle,
       * triggering non-empty frames is not  possible.
       */
    ret = avt_reg_read(client,
        priv->cci_reg.bcrm_addr +
        BCRM_FRAME_START_TRIGGER_SOURCE_8RW,
        AV_CAM_REG_SIZE, AV_CAM_DATA_SIZE_8,
        (char *) &cur_trigger_source);

    if (ret < 0) {
        return ret;
    }

    if (cur_trigger_source == trigger_source_reg) {
        avt_err(sd, " Trigger source already set!\n");
        return 0;
    }

    ret = avt_reg_write(client,
        priv->cci_reg.bcrm_addr + BCRM_FRAME_START_TRIGGER_SOURCE_8RW, trigger_source_reg);

    if (ret < 0) {
        return ret;
    }
    ret = 0;
    break;
    }

	default:
		break;
	}

	return ret;
}


/* --------------- VIDEO OPS --------------- */

static int avt_csi2_g_mbus_config(struct v4l2_subdev *sd,
		struct v4l2_mbus_config *cfg)
{
	cfg->type = V4L2_MBUS_CSI2;

	cfg->flags = V4L2_MBUS_CSI2_CONTINUOUS_CLOCK;
	cfg->flags |= V4L2_MBUS_CSI2_2_LANE; /* XXX wierd */

	return 0;
}

static int avt_set_param(struct i2c_client *client,
			uint32_t id, uint32_t value)
{
	struct avt_ctrl ct;

	CLEAR(ct);
	ct.id = id;
	ct.value0 = value;

	return avt_ctrl_send(client, &ct);
}

static int avt_get_param(struct i2c_client *client,
			uint32_t id, uint32_t *value)
{
	struct avt_ctrl ct;
	int ret;

	CLEAR(ct);
	ct.id = id;
	ret = avt_ctrl_send(client, &ct);

	if (ret < 0)
		return ret;

	*value = ct.value0;

	return 0;
}

/* Start/Stop streaming from the device */
static int avt_csi2_s_stream(struct v4l2_subdev *sd, int enable)
{
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	struct avt_csi2_priv *priv = avt_get_priv(sd);
	int ret = 0;
	uint8_t trigger_mode;

	if (enable) {
		if (priv->numlanes != priv->s_data->numlanes) {
			ret = avt_init_mode(sd);
			if (ret < 0)
				return ret;
			priv->numlanes = priv->s_data->numlanes;
		}
		ret = avt_reg_read(client,
			priv->cci_reg.bcrm_addr +
			BCRM_FRAME_START_TRIGGER_MODE_8RW,
			AV_CAM_REG_SIZE, AV_CAM_DATA_SIZE_8,
			(char *) &trigger_mode);
		if (ret < 0)
			return ret;

		if (priv->mode == AVT_BCRM_MODE)
			ret = avt_set_param(client, V4L2_AV_CSI2_STREAMON_W, 1);
	} else if (priv->mode == AVT_BCRM_MODE) {
		ret = avt_set_param(client, V4L2_AV_CSI2_STREAMOFF_W, 1);
		if (priv->trig_thread)
			kthread_stop(priv->trig_thread);
	}

	if (ret < 0)
		return ret;

	priv->stream_on = enable ? true : false;

	return 0;
}

static int avt_csi2_get_fmt(struct v4l2_subdev *sd,
		struct v4l2_subdev_pad_config *cfg,
		struct v4l2_subdev_format *format)
{
	int ret = 0;
	uint32_t val = 0;

	struct i2c_client *client = v4l2_get_subdevdata(sd);

	if (format->pad != 0)
		return -EINVAL;

	ret = avt_get_param(client, V4L2_AV_CSI2_WIDTH_R, &val);
	if (ret < 0)
		return ret;
	format->format.width = val;

	ret = avt_get_param(client, V4L2_AV_CSI2_HEIGHT_R, &val);
	if (ret < 0)
		return ret;
	format->format.height = val;

	ret = avt_get_param(client, V4L2_AV_CSI2_PIXELFORMAT_R, &val);
	if (ret < 0)
		return ret;
	format->format.code = val;

	/* Hardcoded default format */
	format->format.field = V4L2_FIELD_NONE;
	format->format.colorspace = V4L2_COLORSPACE_SRGB;

	return 0;
}

static int avt_frm_supported(int wmin, int wmax, int ws,
				int hmin, int hmax, int hs,
				int w, int h)
{
	if (
		w > wmax || w < wmin ||
		h > hmax || h < hmin ||
		(h - hmin) % hs != 0 ||
		(w - wmin) % ws != 0
	)
		return -EINVAL;

	return 0;
}

static int avt_csi2_try_fmt(struct v4l2_subdev *sd,
		struct v4l2_subdev_pad_config *cfg,
		struct v4l2_subdev_format *format)
{
	struct avt_csi2_priv *priv = avt_get_priv(sd);
	int ret = 0;

	ret = avt_frm_supported(
			priv->frmp.minw, priv->frmp.maxw, priv->frmp.sw,
			priv->frmp.minh, priv->frmp.maxh, priv->frmp.sh,
			format->format.width, format->format.height);

	if (ret < 0) {
		avt_err(sd, "Not supported format!\n");
		return ret;
	}

	return 0;
}

static int avt_csi2_set_fmt(struct v4l2_subdev *sd,
		struct v4l2_subdev_pad_config *cfg,
		struct v4l2_subdev_format *format)
{
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	struct avt_csi2_priv *priv = avt_get_priv(sd);
	struct v4l2_subdev_selection sel;
	int ret;

	// changing the resolution is not allowed with VIDIOC_S_FMT
	if (priv->mode == AVT_BCRM_MODE &&
		(format->format.width != priv->frmp.r.width ||
		 format->format.height != priv->frmp.r.height) &&
		!priv->crop_align_enabled)
	{
		avt_err(sd, "Changing the resolution is not allowed with VIDIOC_S_FMT!\n");
		return -EINVAL;
	}

	format->format.colorspace = V4L2_COLORSPACE_SRGB;
	format->format.field = V4L2_FIELD_NONE;

	if (format->which == V4L2_SUBDEV_FORMAT_TRY) {
		if (priv->mode == AVT_BCRM_MODE)
			return avt_csi2_try_fmt(sd, cfg, format);
		else
			return 0;
	}

	if (priv->mode != AVT_BCRM_MODE)
		return 0;
	sel.target = V4L2_SEL_TGT_CROP;
	sel.r = priv->frmp.r;

	/* Save format to private data only if
	 * set_param succeded
	 */
	ret = avt_set_param(client, V4L2_AV_CSI2_PIXELFORMAT_W,
		format->format.code);
	if(ret < 0)
		return ret;

	priv->mbus_fmt_code = format->format.code;
	avt_set_selection(sd, NULL, &sel);

	if (priv->stride_align_enabled)
		set_channel_stride_align_for_format(sd, format->format.code);
	else
		set_channel_stride_align(sd, 1);

	format->format.width = priv->frmp.r.width;
	return 0;
}

static uint16_t avt_mbus_formats[] = {
	/* RAW 8 */
	MEDIA_BUS_FMT_Y8_1X8,
	MEDIA_BUS_FMT_SBGGR8_1X8,
	MEDIA_BUS_FMT_SGBRG8_1X8,
	MEDIA_BUS_FMT_SGRBG8_1X8,
	MEDIA_BUS_FMT_SRGGB8_1X8,
        
	/* RAW10 */
	MEDIA_BUS_FMT_Y10_1X10,
	MEDIA_BUS_FMT_SBGGR10_1X10,
	MEDIA_BUS_FMT_SGBRG10_1X10,
	MEDIA_BUS_FMT_SGRBG10_1X10,
	MEDIA_BUS_FMT_SRGGB10_1X10, 

	/* RAW12 */
	MEDIA_BUS_FMT_Y12_1X12,
	MEDIA_BUS_FMT_SRGGB12_1X12,
	MEDIA_BUS_FMT_SGRBG12_1X12,
	MEDIA_BUS_FMT_SGBRG12_1X12,
	MEDIA_BUS_FMT_SBGGR12_1X12,
	/* RGB565 */
	MEDIA_BUS_FMT_RGB565_1X16,
	/* RGB888 */
	MEDIA_BUS_FMT_RGB888_1X24,
	MEDIA_BUS_FMT_BGR888_1X24,
	/* YUV422 */
	MEDIA_BUS_FMT_VYUY8_2X8,
	/* CUSTOM TP31 */
	MEDIA_BUS_FMT_CUSTOM,
};

/* These formats are hidden from VIDIOC_ENUM_FMT ioctl */
static uint16_t avt_hidden_mbus_formats[] = {
/*KHO	
	MEDIA_BUS_FMT_Y10_1X10,
	MEDIA_BUS_FMT_Y12_1X12,

        MEDIA_BUS_FMT_SRGGB12_1X12,
	MEDIA_BUS_FMT_SGRBG12_1X12,
	MEDIA_BUS_FMT_SGBRG12_1X12,
	MEDIA_BUS_FMT_SBGGR12_1X12,
        */
};

//static int32_t avt_avail_formats[ARRAY_SIZE(avt_mbus_formats)];

static bool avt_mbus_fmt_is_hidden(uint16_t mbus_fmt)
{
	int i;
	for(i = 0; i < ARRAY_SIZE(avt_hidden_mbus_formats); i++)
		if(avt_hidden_mbus_formats[i] == mbus_fmt)
			return true;

	return false;
}

static void avt_init_avail_formats(struct v4l2_subdev *sd)
{
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	struct avt_csi2_priv *priv = avt_get_priv(sd);

	int fmt_iter = 0;
	int i;
	int32_t *avail_fmts;

	avail_fmts = kmalloc(sizeof(int32_t)*ARRAY_SIZE(avt_mbus_formats), GFP_KERNEL);

	for(i = 0; i < ARRAY_SIZE(avt_mbus_formats); i++){
		if(avt_check_fmt_available(client, avt_mbus_formats[i])
		   && !avt_mbus_fmt_is_hidden(avt_mbus_formats[i])) {
			avail_fmts[fmt_iter++] = avt_mbus_formats[i];
		}
	}

	avail_fmts[fmt_iter] = -EINVAL;

	priv->available_fmts = avail_fmts;
	priv->available_fmts_cnt = fmt_iter+1;;
}

static int avt_csi2_enum_mbus_code(struct v4l2_subdev *sd,
				struct v4l2_subdev_pad_config *cfg,
				struct v4l2_subdev_mbus_code_enum *code)
{
	struct avt_csi2_priv *priv = avt_get_priv(sd);

	avt_dbg(sd, "()\n");
	if (code->index >= priv->available_fmts_cnt)
		return -EINVAL;

	code->code = priv->available_fmts[code->index];
	if (code->code == -EINVAL)
		return -EINVAL;

	return 0;
}

static int avt_csi2_enum_framesizes(struct v4l2_subdev *sd,
				struct v4l2_subdev_pad_config *cfg,
				struct v4l2_subdev_frame_size_enum *fse)
{
	struct avt_csi2_priv *priv = avt_get_priv(sd);
	bool format_present = false;
	int i;

	avt_dbg(sd, "()\n");

	fse->min_width = fse->max_width = priv->frmp.r.width;
	fse->min_height = fse->max_height = priv->frmp.r.height;

	for (i = 0; i < ARRAY_SIZE(avt_mbus_formats); i++)
		if (avt_mbus_formats[i] == fse->code)
			format_present = true;

	if (fse->index != 0 || format_present == false)
		return -EINVAL;

	return 0;
}

static int avt_csi2_enum_frameintervals(struct v4l2_subdev *sd,
				struct v4l2_subdev_pad_config *cfg,
				struct v4l2_subdev_frame_interval_enum *fie)
{
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	struct camera_common_data *s_data = to_camera_common_data(&client->dev);
	struct avt_csi2_priv *priv = (struct avt_csi2_priv *)s_data->priv;
	struct v4l2_fract tpf;
	int ret;
	u64 framerate;

	/* Only one frame rate should be returned
	 * - the current frame rate
	 */
	if(fie->index > 0)
		return -EINVAL;

	ret = avt_reg_read(client,
                       priv->cci_reg.bcrm_addr +
                       BCRM_ACQUISITION_FRAME_RATE_64RW,
                       AV_CAM_REG_SIZE, AV_CAM_DATA_SIZE_64,
                       (char *) &framerate);
	if (ret < 0) {
			dev_err(&client->dev, "read frameinterval failed\n");
			return ret;
	}

	/* Translate frequency to timeperframe
	* by inverting the fraction
	*/
	tpf.numerator = FRAQ_NUM;
	tpf.denominator = (framerate * FRAQ_NUM) / UHZ_TO_HZ;

	fie->interval = tpf;

	return 0;
}

static int convert_bcrm_to_v4l2(struct bcrm_to_v4l2 *bcrmv4l2,
		int conversion, bool abs)
{
	int64_t value = 0;
	int32_t min = 0;
	int32_t max = 0;
	int32_t step = 0;
	int32_t result = 0;
	int32_t valuedown = 0;
	int32_t valueup = 0;

	step = 1;
	max = S32_MAX;

	/* 1. convert to double */
	if (conversion == min_enum) {
		value = bcrmv4l2->min_bcrm;
		//min = 1;
	} else if (conversion == max_enum) {
		value = bcrmv4l2->max_bcrm;
		min = bcrmv4l2->min_bcrm;
	} else if (conversion == step_enum) {
		value = bcrmv4l2->step_bcrm;
		min = S32_MIN;
	}

	/* Clamp to limits of int32 representation */
	if (value > S32_MAX)
		value = S32_MAX;
	if (value < S32_MIN)
		value = S32_MIN;

	/* 2. convert the units */
/*	value *= factor; */
	if (abs) {
		if (value != 0)
			do_div(value, 1UL);
	}
	/* V4L2_CID_EXPOSURE_ABSOLUTE */
	else {
		if (value != 0)
			do_div(value, 100000UL);
	}

	/* 3. Round value to next integer */
	if (value < S32_MIN)
		result = S32_MIN;
	else if (value > S32_MAX)
		result = S32_MAX;
	else
		result = value;

	/* 4. Clamp to limits */
	if (result > max)
		result = max;
	else if (result < min)
		result = min;

	/* 5. Use nearest increment */
	valuedown = result - ((result - min) % (step));
	valueup = valuedown + step;

	if (result >= 0) {
		if (((valueup - result) <= (result - valuedown))
		&& (valueup <= bcrmv4l2->max_bcrm))
			result = valueup;
		else
			result = valuedown;
	} else {
		if (((valueup - result) < (result - valuedown))
			&& (valueup <= bcrmv4l2->max_bcrm))
			result = valueup;
		else
			result = valuedown;
	}

	if (conversion == min_enum)
		bcrmv4l2->min_v4l2 = result;
	else if (conversion == max_enum)
		bcrmv4l2->max_v4l2 = result;
	else if (conversion == step_enum)
		bcrmv4l2->step_v4l2 = result;
	return 0;
}

static __s32 convert_s_ctrl(__s32 val, __s32 min, __s32 max, __s32 step)
{
	int32_t valuedown = 0, valueup = 0;

	if (val > max)
		val = max;

	else if (val < min)
		val = min;

	valuedown = val - ((val - min) % step);
	valueup = valuedown + step;

	if (val >= 0) {
		if (((valueup - val) <= (val - valuedown)) && (valueup <= max))
			val = valueup;
		else
			val = valuedown;
	} else {
		if (((valueup - val) < (val - valuedown)) && (valueup <= max))
			val = valueup;
		else
			val = valuedown;
	}

	return val;
}

static int ioctl_queryctrl(struct v4l2_subdev *sd,
		struct v4l2_queryctrl *qctrl)
{
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	struct camera_common_data *s_data = to_camera_common_data(&client->dev);
	struct avt_csi2_priv *priv = (struct avt_csi2_priv *)s_data->priv;

	u64 value_feature = 0;
	u32 value = 0;
	u64 value64 = 0;
	int ret;
	union bcrm_feature_reg feature_inquiry_reg;
	struct bcrm_to_v4l2 bcrm_v4l2;

	avt_dbg(sd, "\n");

	CLEAR(bcrm_v4l2);

	/* reading the Feature inquiry register */
	ret = avt_reg_read(client,
			priv->cci_reg.bcrm_addr + BCRM_FEATURE_INQUIRY_64R,
			AV_CAM_REG_SIZE, AV_CAM_DATA_SIZE_64,
			(char *) &value_feature);
	if (ret < 0) {
		avt_err(sd, "BCRM_FEATURE_INQUIRY_64R: i2c read failed (%d)\n", ret);
		return ret;
	}

	feature_inquiry_reg.value = value_feature;

	switch (qctrl->id) {

	/* BLACK LEVEL is deprecated and thus we use Brightness */
	case V4L2_CID_BRIGHTNESS:
		avt_dbg(sd, "case V4L2_CID_BRIGHTNESS\n");

		if (!feature_inquiry_reg.feature_inq.black_level_avail) {
			avt_info(sd, "control 'Brightness' not supported by firmware\n");
			qctrl->flags = V4L2_CTRL_FLAG_DISABLED;
			return 0;
		}

		/* reading the current Black Level value */
		ret = avt_reg_read(client,
				priv->cci_reg.bcrm_addr + BCRM_BLACK_LEVEL_32RW,
				AV_CAM_REG_SIZE, AV_CAM_DATA_SIZE_32,
				(char *) &value);
		if (ret < 0) {
			avt_err(sd, "BCRM_BLACK_LEVEL_32RW: i2c read failed (%d)\n",
					ret);
			return ret;
		}

		qctrl->default_value = value;

		/* reading the Minimum Black Level */
		ret = avt_reg_read(client,
				priv->cci_reg.bcrm_addr + BCRM_BLACK_LEVEL_MIN_32R,
				AV_CAM_REG_SIZE, AV_CAM_DATA_SIZE_32,
				(char *) &value);
		if (ret < 0) {
			avt_err(sd, "BCRM_BLACK_LEVEL_MIN_32R: i2c read failed (%d)\n",
					ret);
			return ret;
		}

		qctrl->minimum = value;

		/* reading the Maximum Black Level */
		ret = avt_reg_read(client,
				priv->cci_reg.bcrm_addr + BCRM_BLACK_LEVEL_MAX_32R,
				AV_CAM_REG_SIZE, AV_CAM_DATA_SIZE_32,
				(char *) &value);
		if (ret < 0) {
			avt_err(sd, "BCRM_BLACK_LEVEL_MAX_32R: i2c read failed (%d)\n",
					ret);
			return ret;
		}

		qctrl->maximum = value;

		/* reading the Black Level step increment */
		ret = avt_reg_read(client,
				priv->cci_reg.bcrm_addr + BCRM_BLACK_LEVEL_INC_32R,
				AV_CAM_REG_SIZE, AV_CAM_DATA_SIZE_32,
				(char *) &value);
		if (ret < 0) {
			avt_err(sd, "BCRM_BLACK_LEVEL_INC_32R: i2c read failed (%d)\n",
					ret);
			return ret;
		}

		qctrl->step = value;

		if (qctrl->minimum > qctrl->maximum) {
			avt_err(sd, "Brightness: min > max! (%d > %d)\n",
					qctrl->minimum, qctrl->maximum);
			return -EINVAL;
		}
		if (qctrl->step <= 0) {
			avt_err(sd, "Brightness: non-positive step value (%d)!\n",
					qctrl->step);
			return -EINVAL;
		}

		qctrl->type = V4L2_CTRL_TYPE_INTEGER;
		strcpy(qctrl->name, "Brightness");
		break;

	case V4L2_CID_EXPOSURE:
		avt_dbg(sd, "case V4L2_CID_EXPOSURE\n");

		/* reading the Exposure time */
		ret = avt_reg_read(client,
				priv->cci_reg.bcrm_addr + BCRM_EXPOSURE_TIME_64RW,
				AV_CAM_REG_SIZE, AV_CAM_DATA_SIZE_64,
				(char *) &value64);
		if (ret < 0) {
			avt_err(sd, "BCRM_EXPOSURE_TIME_64RW: i2c read failed (%d)\n",
					ret);
			return ret;
		}

		qctrl->default_value = (__s32) value64;

		/* reading the Minimum Exposure time */
		ret = avt_reg_read(client,
				priv->cci_reg.bcrm_addr + BCRM_EXPOSURE_TIME_MIN_64R,
				AV_CAM_REG_SIZE, AV_CAM_DATA_SIZE_64,
				(char *) &value64);
		if (ret < 0) {
			avt_err(sd, "BCRM_EXPOSURE_TIME_MIN_64R: i2c read failed (%d)\n",
					ret);
			return ret;
		}

		bcrm_v4l2.min_bcrm = value64;

		/* reading the Maximum Exposure time */
		ret = avt_reg_read(client,
				priv->cci_reg.bcrm_addr + BCRM_EXPOSURE_TIME_MAX_64R,
				AV_CAM_REG_SIZE, AV_CAM_DATA_SIZE_64,
				(char *) &value64);
		if (ret < 0) {
			avt_err(sd, "BCRM_EXPOSURE_TIME_MAX_64R: i2c read failed (%d)\n",
					ret);
			return ret;
		}

		bcrm_v4l2.max_bcrm = value64;

		/* reading the Exposure time step increment */
		ret = avt_reg_read(client,
				priv->cci_reg.bcrm_addr + BCRM_EXPOSURE_TIME_INC_64R,
				AV_CAM_REG_SIZE, AV_CAM_DATA_SIZE_64,
				(char *) &value64);
		if (ret < 0) {
			avt_err(sd, "BCRM_EXPOSURE_TIME_INC_64R: i2c read failed (%d)\n",
					ret);
			return ret;
		}

		bcrm_v4l2.step_bcrm = value64;

		convert_bcrm_to_v4l2(&bcrm_v4l2, min_enum, true);
		convert_bcrm_to_v4l2(&bcrm_v4l2, max_enum, true);
		convert_bcrm_to_v4l2(&bcrm_v4l2, step_enum, true);

		qctrl->minimum = bcrm_v4l2.min_v4l2;
		qctrl->maximum = bcrm_v4l2.max_v4l2;
		qctrl->step = bcrm_v4l2.step_v4l2;

		if (qctrl->minimum > qctrl->maximum) {
			avt_err(sd, "Exposure: min > max! (%d > %d)\n",
					qctrl->minimum, qctrl->maximum);
			return -EINVAL;
		}
		if (qctrl->step <= 0) {
			avt_err(sd, "Exposure: non-positive step value (%d)!\n",
					qctrl->step);
			return -EINVAL;
		}

		qctrl->type = V4L2_CTRL_TYPE_INTEGER;
		strcpy(qctrl->name, "Exposure");
		break;

	case V4L2_CID_EXPOSURE_ABSOLUTE:
		avt_dbg(sd, "case V4L2_CID_EXPOSURE_ABSOLUTE\n");

		/* reading the Maximum Exposure time */
		ret = avt_reg_read(client,
				priv->cci_reg.bcrm_addr + BCRM_EXPOSURE_TIME_MAX_64R,
				AV_CAM_REG_SIZE, AV_CAM_DATA_SIZE_64,
				(char *) &value64);
		if (ret < 0) {
			avt_err(sd, "BCRM_EXPOSURE_TIME_MAX_64R: i2c read failed (%d)\n",
					ret);
			return ret;
		}

		/* convert unit [ns] to [100*us] */
		value64 = value64 / 100000UL;
		
		/* clamp to s32 max */
		qctrl->maximum = clamp(value64, (u64)0, (u64)S32_MAX);

		qctrl->minimum = 1;
		qctrl->default_value = 1;
		qctrl->step = 1;
		qctrl->type = V4L2_CTRL_TYPE_INTEGER64;

		if (qctrl->minimum > qctrl->maximum) {
			avt_err(sd, "Exposure Absolute: min > max! (%d > %d)\n",
					qctrl->minimum, qctrl->maximum);
			return -EINVAL;
		}

		strcpy(qctrl->name, "Exposure Absolute");
		break;

	case V4L2_CID_EXPOSURE_AUTO:

		avt_dbg(sd, "case V4L2_CID_EXPOSURE_AUTO\n");

		if (!feature_inquiry_reg.feature_inq.exposure_auto) {
			avt_info(sd, "control 'Exposure Auto' not supported by firmware\n");
			qctrl->flags = V4L2_CTRL_FLAG_DISABLED;
			return 0;
		}

		/* reading the current exposure auto value */
		ret = avt_reg_read(client,
				priv->cci_reg.bcrm_addr + BCRM_EXPOSURE_AUTO_8RW,
				AV_CAM_REG_SIZE, AV_CAM_DATA_SIZE_8,
				(char *) &value);
		if (ret < 0) {
			avt_err(sd, "BCRM_EXPOSURE_AUTO_8RW: i2c read failed (%d)\n",
					ret);
			return ret;
		}

		if (value == 2)
			/* continous mode, Refer BCRM doc */
			qctrl->default_value = V4L2_EXPOSURE_AUTO;
		else
			/* false (OFF) */
			qctrl->default_value = V4L2_EXPOSURE_MANUAL;

		qctrl->minimum = 0;
		qctrl->step = 0;
		qctrl->maximum = 1;
		qctrl->type = V4L2_CTRL_TYPE_MENU;

		strcpy(qctrl->name, "Exposure Auto");
		break;

	case V4L2_CID_GAIN:

		avt_dbg(sd, "case V4L2_CID_GAIN\n");

		if (!feature_inquiry_reg.feature_inq.gain_avail) {
			avt_info(sd, "control 'Gain' not supported by firmware\n");
			qctrl->flags = V4L2_CTRL_FLAG_DISABLED;
			return 0;
		}

		/* reading the Gain value */
		ret = avt_reg_read(client,
				priv->cci_reg.bcrm_addr + BCRM_GAIN_64RW,
				AV_CAM_REG_SIZE, AV_CAM_DATA_SIZE_64,
				(char *) &value64);
		if (ret < 0) {
			avt_err(sd, "BCRM_GAIN_64RW: i2c read failed (%d)\n", ret);
			return ret;
		}

		qctrl->default_value = (__s32) value64;

		/* reading the Minimum Gain value */
		ret = avt_reg_read(client,
				priv->cci_reg.bcrm_addr + BCRM_GAIN_MIN_64R,
				AV_CAM_REG_SIZE, AV_CAM_DATA_SIZE_64,
				(char *) &value64);
		if (ret < 0) {
			avt_err(sd, "BCRM_GAIN_MIN_64R: i2c read failed (%d)\n",
					ret);
			return ret;
		}

		bcrm_v4l2.min_bcrm = value64;

		/* reading the Maximum Gain value */
		ret = avt_reg_read(client,
				priv->cci_reg.bcrm_addr + BCRM_GAIN_MAX_64R,
				AV_CAM_REG_SIZE, AV_CAM_DATA_SIZE_64,
				(char *) &value64);
		if (ret < 0) {
			avt_err(sd, "BCRM_GAIN_MAX_64R: i2c read failed (%d)\n",
					ret);
			return ret;
		}

		bcrm_v4l2.max_bcrm = value64;

		/* reading the Gain step increment */
		ret = avt_reg_read(client,
				priv->cci_reg.bcrm_addr + BCRM_GAIN_INC_64R,
				AV_CAM_REG_SIZE, AV_CAM_DATA_SIZE_64,
				(char *) &value64);
		if (ret < 0) {
			avt_err(sd, "BCRM_GAIN_INC_64R: i2c read failed (%d)\n",
					ret);
			return ret;
		}

		bcrm_v4l2.step_bcrm = value64;

		convert_bcrm_to_v4l2(&bcrm_v4l2, min_enum, true);
		convert_bcrm_to_v4l2(&bcrm_v4l2, max_enum, true);
		convert_bcrm_to_v4l2(&bcrm_v4l2, step_enum, true);

		qctrl->minimum = bcrm_v4l2.min_v4l2;
		qctrl->maximum = bcrm_v4l2.max_v4l2;
		qctrl->step = bcrm_v4l2.step_v4l2;
		qctrl->type = V4L2_CTRL_TYPE_INTEGER;

		if (qctrl->minimum > qctrl->maximum) {
			avt_err(sd, "Gain: min > max! (%d > %d)\n",
					qctrl->minimum, qctrl->maximum);
			return -EINVAL;
		}
		if (qctrl->step <= 0) {
			avt_err(sd, "Gain: non-positive step value (%d)!\n",
					qctrl->step);
			return -EINVAL;
		}

		strcpy(qctrl->name, "Gain");
		break;

	case V4L2_CID_AUTOGAIN:
		avt_dbg(sd, "case V4L2_CID_AUTOGAIN\n");

		if (!feature_inquiry_reg.feature_inq.gain_auto) {
			avt_info(sd, "control 'Gain Auto' not supported by firmware\n");
			qctrl->flags = V4L2_CTRL_FLAG_DISABLED;
			return 0;
		}

		/* reading the Auto Gain value */
		ret = avt_reg_read(client,
				priv->cci_reg.bcrm_addr + BCRM_GAIN_AUTO_8RW,
				AV_CAM_REG_SIZE, AV_CAM_DATA_SIZE_8,
				(char *) &value);

		if (ret < 0) {
			avt_err(sd, "BCRM_GAIN_AUTO_8RW: i2c read failed (%d)\n",
					ret);
			return ret;
		}

		if (value == 2)
			/* true (ON) for continous mode, Refer BCRM doc */
			qctrl->default_value = true;
		else
			/* false (OFF) */
			qctrl->default_value = false;

		qctrl->minimum = 0;
		qctrl->step = 1;
		qctrl->maximum = 1;
		qctrl->type = V4L2_CTRL_TYPE_BOOLEAN;

		strcpy(qctrl->name, "Auto Gain");
		break;

	case V4L2_CID_HFLIP:
		avt_dbg(sd, "case V4L2_CID_HFLIP\n");

		if (!feature_inquiry_reg.feature_inq.reverse_x_avail) {
			avt_info(sd, "control 'Reversing X (Horizantal Flip)' not supported by firmware\n");
			qctrl->flags = V4L2_CTRL_FLAG_DISABLED;
			return 0;
		}

		/* reading the Reverse X value */
		ret = avt_reg_read(client,
				priv->cci_reg.bcrm_addr + BCRM_IMG_REVERSE_X_8RW,
				AV_CAM_REG_SIZE, AV_CAM_DATA_SIZE_8,
				(char *) &value);
		if (ret < 0) {
			avt_err(sd, "BCRM_IMG_REVERSE_X_8RW: i2c read failed (%d)\n",
					ret);
			return ret;
		}

		qctrl->default_value = value;

		qctrl->minimum = 0;
		qctrl->step = qctrl->maximum = 1;
		qctrl->type = V4L2_CTRL_TYPE_BOOLEAN;
		strcpy(qctrl->name, "Reverse X");

		break;

	case V4L2_CID_VFLIP:
		avt_dbg(sd, "case V4L2_CID_VFLIP\n");
		if (!feature_inquiry_reg.feature_inq.reverse_y_avail) {
			avt_info(sd, "control 'Reversing Y (Vertical Flip)' not supported by firmware\n");
			qctrl->flags = V4L2_CTRL_FLAG_DISABLED;
			return 0;
		}

		/* reading the Reverse Y value */
		ret = avt_reg_read(client,
				priv->cci_reg.bcrm_addr + BCRM_IMG_REVERSE_Y_8RW,
				AV_CAM_REG_SIZE, AV_CAM_DATA_SIZE_8,
				(char *) &value);
		if (ret < 0) {
			avt_err(sd, "BCRM_IMG_REVERSE_Y_8RW: i2c read failed (%d)\n",
					ret);
			return ret;
		}

		qctrl->default_value = value;

		qctrl->minimum = 0;
		qctrl->step = qctrl->maximum = 1;
		qctrl->type = V4L2_CTRL_TYPE_BOOLEAN;
		strcpy(qctrl->name, "Reverse Y");

		break;

	case V4L2_CID_GAMMA:
		avt_dbg(sd, "case V4L2_CID_GAMMA\n");

		if (!feature_inquiry_reg.feature_inq.gamma_avail) {
			avt_info(sd, "control 'Gamma' not supported by firmware\n");
			qctrl->flags = V4L2_CTRL_FLAG_DISABLED;
			return 0;
		}

		/* reading the Gamma value */
		ret = avt_reg_read(client,
				priv->cci_reg.bcrm_addr + BCRM_GAMMA_64RW,
				AV_CAM_REG_SIZE, AV_CAM_DATA_SIZE_64,
				(char *) &value64);
		if (ret < 0) {
			avt_err(sd, "BCRM_GAMMA_64RW: i2c read failed (%d)\n", ret);
			return ret;
		}

		qctrl->default_value = (__s32) value64;

		/* reading the Minimum Gamma */
		ret = avt_reg_read(client,
				priv->cci_reg.bcrm_addr + BCRM_GAMMA_MIN_64R,
				AV_CAM_REG_SIZE, AV_CAM_DATA_SIZE_64,
				(char *) &value64);
		if (ret < 0) {
			avt_err(sd, "BCRM_GAMMA_MIN_64R: i2c read failed (%d)\n",
					ret);
			return ret;
		}

		bcrm_v4l2.min_bcrm = value64;

		/* reading the Maximum Gamma */
		ret = avt_reg_read(client,
				priv->cci_reg.bcrm_addr + BCRM_GAMMA_MAX_64R,
				AV_CAM_REG_SIZE, AV_CAM_DATA_SIZE_64,
				(char *) &value64);
		if (ret < 0) {
			avt_err(sd, "BCRM_GAMMA_MAX_64R: i2c read failed (%d)\n",
					ret);
			return ret;
		}

		bcrm_v4l2.max_bcrm = value64;

		/* reading the Gamma step increment */
		ret = avt_reg_read(client,
				priv->cci_reg.bcrm_addr + BCRM_GAMMA_INC_64R,
				AV_CAM_REG_SIZE, AV_CAM_DATA_SIZE_64,
				(char *) &value64);
		if (ret < 0) {
			avt_err(sd, "BCRM_GAMMA_INC_64R: i2c read failed (%d)\n",
					ret);
			return ret;
		}

		bcrm_v4l2.step_bcrm = value64;

		convert_bcrm_to_v4l2(&bcrm_v4l2, min_enum, true);
		convert_bcrm_to_v4l2(&bcrm_v4l2, max_enum, true);
		convert_bcrm_to_v4l2(&bcrm_v4l2, step_enum, true);

		qctrl->minimum = bcrm_v4l2.min_v4l2;
		qctrl->maximum = bcrm_v4l2.max_v4l2;
		qctrl->step = bcrm_v4l2.step_v4l2;

		if (qctrl->minimum > qctrl->maximum) {
			avt_err(sd, "Gamma: min > max! (%d > %d)\n",
					qctrl->minimum, qctrl->maximum);
			return -EINVAL;
		}
		if (qctrl->step <= 0) {
			avt_err(sd, "Gamma: non-positive step value (%d)!\n",
					qctrl->step);
			return -EINVAL;
		}

		qctrl->type = V4L2_CTRL_TYPE_INTEGER;
		strcpy(qctrl->name, "Gamma");
		break;

	case V4L2_CID_CONTRAST:
		avt_dbg(sd, "case V4L2_CID_CONTRAST\n");

		if (!feature_inquiry_reg.feature_inq.contrast_avail) {
			avt_info(sd, "control 'Contrast' not supported by firmware\n");
			qctrl->flags = V4L2_CTRL_FLAG_DISABLED;
			return 0;
		}

		/* reading the Contrast value */
		ret = avt_reg_read(client,
				priv->cci_reg.bcrm_addr + BCRM_CONTRAST_VALUE_32RW,
				AV_CAM_REG_SIZE, AV_CAM_DATA_SIZE_32,
				(char *) &value);
		if (ret < 0) {
			avt_err(sd, "BCRM_CONTRAST_VALUE_32RW: i2c read failed (%d)\n",
					ret);
			return ret;
		}

		qctrl->default_value = value;

		/* reading the Minimum Contrast */
		ret = avt_reg_read(client,
				priv->cci_reg.bcrm_addr + BCRM_CONTRAST_VALUE_MIN_32R,
				AV_CAM_REG_SIZE, AV_CAM_DATA_SIZE_32,
				(char *) &value);
		if (ret < 0) {
			avt_err(sd, "BCRM_CONTRAST_VALUE_MIN_32R: i2c read failed (%d)\n",
					ret);
			return ret;
		}

		qctrl->minimum = value;

		/* reading the Maximum Contrast */
		ret = avt_reg_read(client,
				priv->cci_reg.bcrm_addr + BCRM_CONTRAST_VALUE_MAX_32R,
				AV_CAM_REG_SIZE, AV_CAM_DATA_SIZE_32,
				(char *) &value);
		if (ret < 0) {
			avt_err(sd, "BCRM_CONTRAST_VALUE_MAX_32R: i2c read failed (%d)\n",
					ret);
			return ret;
		}

		qctrl->maximum = value;

		/* reading the Contrast step increment */
		ret = avt_reg_read(client,
				priv->cci_reg.bcrm_addr +
				BCRM_CONTRAST_VALUE_INC_32R,
				AV_CAM_REG_SIZE, AV_CAM_DATA_SIZE_32,
				(char *) &value);
		if (ret < 0) {
			avt_err(sd, "BCRM_CONTRAST_VALUE_INC_32R: i2c read failed (%d)\n",
					ret);
			return ret;
		}

		qctrl->step = value;

		qctrl->type = V4L2_CTRL_TYPE_INTEGER;

		if (qctrl->minimum > qctrl->maximum) {
			avt_err(sd, "Contrast: min > max! (%d > %d)\n",
					qctrl->minimum, qctrl->maximum);
			return -EINVAL;
		}
		if (qctrl->step <= 0) {
			avt_err(sd, "Contrast: non-positive step value (%d)!\n",
					qctrl->step);
			return -EINVAL;
		}

		strcpy(qctrl->name, "Contrast");
		break;

	case V4L2_CID_AUTO_WHITE_BALANCE:
		avt_dbg(sd, "case V4L2_CID_AUTO_WHITE_BALANCE\n");

		if (!feature_inquiry_reg.feature_inq.white_balance_auto_avail) {
			avt_info(sd, "control 'White balance Auto' not supported by firmware\n");
			qctrl->flags = V4L2_CTRL_FLAG_DISABLED;
			return 0;
		}

		/* reading the White balance auto value */
		ret = avt_reg_read(client,
				priv->cci_reg.bcrm_addr + BCRM_WHITE_BALANCE_AUTO_8RW,
				AV_CAM_REG_SIZE, AV_CAM_DATA_SIZE_8,
				(char *) &value);
		if (ret < 0) {
			avt_err(sd, "BCRM_WHITE_BALANCE_AUTO_8RW: i2c read failed (%d)\n",
					ret);
			return ret;
		}

		if (value == 2)
			/* true (ON) */
			qctrl->default_value = true;
		else
			/* false (OFF) */
			qctrl->default_value = false;

		qctrl->minimum = 0;
		qctrl->step = 1;
		qctrl->maximum = 1;
		qctrl->type = V4L2_CTRL_TYPE_BOOLEAN;

		strcpy(qctrl->name, "White Balance Auto");
		break;

	case V4L2_CID_DO_WHITE_BALANCE:
		avt_dbg(sd, "case V4L2_CID_DO_WHITE_BALANCE\n");

		if (!feature_inquiry_reg.feature_inq.white_balance_avail) {
			avt_info(sd, "control 'White balance' not supported by firmware\n");
			qctrl->flags = V4L2_CTRL_FLAG_DISABLED;
			return 0;
		}

		/* reading the White balance auto reg */
		ret = avt_reg_read(client,
				priv->cci_reg.bcrm_addr + BCRM_WHITE_BALANCE_AUTO_8RW,
				AV_CAM_REG_SIZE, AV_CAM_DATA_SIZE_8,
				(char *) &value);
		if (ret < 0) {
			avt_err(sd, "BCRM_WHITE_BALANCE_AUTO_8RW: i2c read failed (%d)\n",
					ret);
			return ret;
		}

		qctrl->default_value = 0;
		qctrl->minimum = 0;
		qctrl->step = 0;
		qctrl->maximum = 0;
		qctrl->type = V4L2_CTRL_TYPE_BUTTON;

		strcpy(qctrl->name, "White Balance");
		break;

	case V4L2_CID_SATURATION:
		avt_dbg(sd, "case V4L2_CID_SATURATION\n");

		if (!feature_inquiry_reg.feature_inq.saturation_avail) {
			avt_info(sd, "control 'Saturation' not supported by firmware\n");
			qctrl->flags = V4L2_CTRL_FLAG_DISABLED;
			return 0;
		}

		/* reading the Saturation value */
		ret = avt_reg_read(client,
				priv->cci_reg.bcrm_addr + BCRM_SATURATION_32RW,
				AV_CAM_REG_SIZE, AV_CAM_DATA_SIZE_32,
				(char *) &value);
		if (ret < 0) {
			avt_err(sd, "BCRM_SATURATION_32RW: i2c read failed (%d)\n",
					ret);
			return ret;
		}

		qctrl->default_value = value;

		/* reading the Minimum Saturation */
		ret = avt_reg_read(client,
				priv->cci_reg.bcrm_addr + BCRM_SATURATION_MIN_32R,
				AV_CAM_REG_SIZE, AV_CAM_DATA_SIZE_32,
				(char *) &value);
		if (ret < 0) {
			avt_err(sd, "BCRM_SATURATION_MIN_32R: i2c read failed (%d)\n",
					ret);
			return ret;
		}

		qctrl->minimum = value;

		/* reading the Maximum Saturation */
		ret = avt_reg_read(client,
				priv->cci_reg.bcrm_addr + BCRM_SATURATION_MAX_32R,
				AV_CAM_REG_SIZE, AV_CAM_DATA_SIZE_32,
				(char *) &value);
		if (ret < 0) {
			avt_err(sd, "BCRM_SATURATION_MAX_32R: i2c read failed (%d)\n",
					ret);
			return ret;
		}

		qctrl->maximum = value;

		/* reading the Saturation step increment */
		ret = avt_reg_read(client,
				priv->cci_reg.bcrm_addr + BCRM_SATURATION_INC_32R,
				AV_CAM_REG_SIZE, AV_CAM_DATA_SIZE_32,
				(char *) &value);
		if (ret < 0) {
			avt_err(sd, "BCRM_SATURATION_INC_32R: i2c read failed (%d)\n",
					ret);
			return ret;
		}

		qctrl->step = value;

		if (qctrl->minimum > qctrl->maximum) {
			avt_err(sd, "Saturation: min > max! (%d > %d)\n",
					qctrl->minimum, qctrl->maximum);
			return -EINVAL;
		}
		if (qctrl->step <= 0) {
			avt_err(sd, "Saturation: non-positive step value (%d)!\n",
					qctrl->step);
			return -EINVAL;
		}

		qctrl->type = V4L2_CTRL_TYPE_INTEGER;
		strcpy(qctrl->name, "Saturation");
		break;

	case V4L2_CID_HUE:
		avt_dbg(sd, "case V4L2_CID_HUE\n");

		if (!feature_inquiry_reg.feature_inq.hue_avail) {
			avt_info(sd, "control 'Hue' not supported by firmware\n");
			qctrl->flags = V4L2_CTRL_FLAG_DISABLED;
			return 0;
		}

		/* reading the Hue value */
		ret = avt_reg_read(client,
				priv->cci_reg.bcrm_addr + BCRM_HUE_32RW,
				AV_CAM_REG_SIZE, AV_CAM_DATA_SIZE_32,
				(char *) &value);
		if (ret < 0) {
			avt_err(sd, "BCRM_HUE_32RW: i2c read failed (%d)\n", ret);
			return ret;
		}

		qctrl->default_value = value;

		/* reading the Minimum HUE */
		ret = avt_reg_read(client,
				priv->cci_reg.bcrm_addr + BCRM_HUE_MIN_32R,
				AV_CAM_REG_SIZE, AV_CAM_DATA_SIZE_32,
				(char *) &value);
		if (ret < 0) {
			avt_err(sd, "BCRM_HUE_MIN_32R: i2c read failed (%d)\n",
					ret);
			return ret;
		}

		qctrl->minimum = value;

		/* reading the Maximum HUE */
		ret = avt_reg_read(client,
				priv->cci_reg.bcrm_addr + BCRM_HUE_MAX_32R,
				AV_CAM_REG_SIZE, AV_CAM_DATA_SIZE_32,
				(char *) &value);
		if (ret < 0) {
			avt_err(sd, "BCRM_HUE_MAX_32R: i2c read failed (%d)\n", ret);
			return ret;
		}

		qctrl->maximum = value;

		/* reading the HUE step increment */
		ret = avt_reg_read(client,
				priv->cci_reg.bcrm_addr + BCRM_HUE_INC_32R,
				AV_CAM_REG_SIZE, AV_CAM_DATA_SIZE_32,
				(char *) &value);
		if (ret < 0) {
			avt_err(sd, "BCRM_HUE_INC_32R: i2c read failed (%d)\n",
					ret);
			return ret;
		}

		qctrl->step = value;

		if (qctrl->minimum > qctrl->maximum) {
			avt_err(sd, "Hue: min > max! (%d > %d)\n",
					qctrl->minimum, qctrl->maximum);
			return -EINVAL;
		}
		if (qctrl->step <= 0) {
			avt_err(sd, "Hue: non-positive step value (%d)!\n",
					qctrl->step);
			return -EINVAL;
		}

		qctrl->type = V4L2_CTRL_TYPE_INTEGER;
		strcpy(qctrl->name, "Hue");
		break;

	case V4L2_CID_RED_BALANCE:
		avt_dbg(sd, "case V4L2_CID_RED_BALANCE\n");

		if (!feature_inquiry_reg.feature_inq.white_balance_avail) {
			avt_info(sd, "control 'Red balance' not supported by firmware\n");
			qctrl->flags = V4L2_CTRL_FLAG_DISABLED;
			return 0;
		}

		/* reading the Red balance value */
		ret = avt_reg_read(client,
				priv->cci_reg.bcrm_addr + BCRM_RED_BALANCE_RATIO_64RW,
				AV_CAM_REG_SIZE, AV_CAM_DATA_SIZE_64,
				(char *) &value64);
		if (ret < 0) {
			avt_err(sd, "BCRM_RED_BALANCE_RATIO_64RW: i2c read failed (%d)\n",
					ret);
			return ret;
		}

		qctrl->default_value = (__s32) value64;

		/* reading the Minimum Red balance */
		ret = avt_reg_read(client,
				priv->cci_reg.bcrm_addr + BCRM_RED_BALANCE_RATIO_MIN_64R,
				AV_CAM_REG_SIZE, AV_CAM_DATA_SIZE_64,
				(char *) &value64);
		if (ret < 0) {
			avt_err(sd, "BCRM_RED_BALANCE_RATIO_MIN_64R: i2c read failed (%d)\n",
					ret);
			return ret;
		}

		bcrm_v4l2.min_bcrm = value64;

		/* reading the Maximum Red balance */
		ret = avt_reg_read(client,
				priv->cci_reg.bcrm_addr + BCRM_RED_BALANCE_RATIO_MAX_64R,
				AV_CAM_REG_SIZE, AV_CAM_DATA_SIZE_64,
				(char *) &value64);
		if (ret < 0) {
			avt_err(sd, "BCRM_RED_BALANCE_RATIO_MAX_64R: i2c read failed (%d)\n",
					ret);
			return ret;
		}

		bcrm_v4l2.max_bcrm = value64;

		/* reading the Red balance step increment */
		ret = avt_reg_read(client,
				priv->cci_reg.bcrm_addr +
				BCRM_RED_BALANCE_RATIO_INC_64R,
				AV_CAM_REG_SIZE, AV_CAM_DATA_SIZE_64,
				(char *) &value64);
		if (ret < 0) {
			avt_err(sd, "BCRM_RED_BALANCE_RATIO_INC_64R: i2c read failed (%d)\n",
					ret);
			return ret;
		}

		bcrm_v4l2.step_bcrm = value64;

		convert_bcrm_to_v4l2(&bcrm_v4l2, min_enum, true);
		convert_bcrm_to_v4l2(&bcrm_v4l2, max_enum, true);
		convert_bcrm_to_v4l2(&bcrm_v4l2, step_enum, true);

		qctrl->minimum = bcrm_v4l2.min_v4l2;
		qctrl->maximum = bcrm_v4l2.max_v4l2;
		qctrl->step = bcrm_v4l2.step_v4l2;

		if (qctrl->minimum > qctrl->maximum) {
			avt_err(sd, "Red Balance: min > max! (%d > %d)\n",
					qctrl->minimum, qctrl->maximum);
			return -EINVAL;
		}
		if (qctrl->step <= 0) {
			avt_err(sd, "Red Balance: non-positive step value (%d)!\n",
					qctrl->step);
			return -EINVAL;
		}

		qctrl->type = V4L2_CTRL_TYPE_INTEGER;
		strcpy(qctrl->name, "Red Balance");
		break;

	case V4L2_CID_BLUE_BALANCE:
		avt_dbg(sd, "case V4L2_CID_BLUE_BALANCE\n");

		if (!feature_inquiry_reg.feature_inq.white_balance_avail) {
			avt_info(sd, "control 'Blue balance' not supported by firmware\n");
			qctrl->flags = V4L2_CTRL_FLAG_DISABLED;
			return 0;
		}

		/* reading the Blue balance value */
		ret = avt_reg_read(client,
				priv->cci_reg.bcrm_addr + BCRM_BLUE_BALANCE_RATIO_64RW,
				AV_CAM_REG_SIZE, AV_CAM_DATA_SIZE_64,
				(char *) &value64);
		if (ret < 0) {
			avt_err(sd, "BCRM_BLUE_BALANCE_RATIO_64RW: i2c read failed (%d)\n",
					ret);
			return ret;
		}

		qctrl->default_value = (__s32) value64;

		/* reading the Minimum Blue balance */
		ret = avt_reg_read(client,
				priv->cci_reg.bcrm_addr + BCRM_BLUE_BALANCE_RATIO_MIN_64R,
				AV_CAM_REG_SIZE, AV_CAM_DATA_SIZE_64,
				(char *) &value64);
		if (ret < 0) {
			avt_err(sd, "BCRM_BLUE_BALANCE_RATIO_MIN_64R: i2c read failed (%d)\n",
					ret);
			return ret;
		}

		bcrm_v4l2.min_bcrm = value64;

		/* reading the Maximum Blue balance */
		ret = avt_reg_read(client,
				priv->cci_reg.bcrm_addr + BCRM_BLUE_BALANCE_RATIO_MAX_64R,
				AV_CAM_REG_SIZE, AV_CAM_DATA_SIZE_64,
				(char *) &value64);
		if (ret < 0) {
			avt_err(sd, "BCRM_BLUE_BALANCE_RATIO_MAX_64R: i2c read failed (%d)\n",
					ret);
			return ret;
		}

		bcrm_v4l2.max_bcrm = value64;

		/* reading the Blue balance step increment */
		ret = avt_reg_read(client,
				priv->cci_reg.bcrm_addr +
				BCRM_BLUE_BALANCE_RATIO_INC_64R,
				AV_CAM_REG_SIZE, AV_CAM_DATA_SIZE_64,
				(char *) &value64);
		if (ret < 0) {
			avt_err(sd, "BCRM_BLUE_BALANCE_RATIO_INC_64R: i2c read failed (%d)\n",
					ret);
			return ret;
		}

		bcrm_v4l2.step_bcrm = value64;

		convert_bcrm_to_v4l2(&bcrm_v4l2, min_enum, true);
		convert_bcrm_to_v4l2(&bcrm_v4l2, max_enum, true);
		convert_bcrm_to_v4l2(&bcrm_v4l2, step_enum, true);

		qctrl->minimum = bcrm_v4l2.min_v4l2;
		qctrl->maximum = bcrm_v4l2.max_v4l2;
		qctrl->step = bcrm_v4l2.step_v4l2;

		if (qctrl->minimum > qctrl->maximum) {
			avt_err(sd, "Blue Balance: min > max! (%d > %d)\n",
					qctrl->minimum, qctrl->maximum);
			return -EINVAL;
		}
		if (qctrl->step <= 0) {
			avt_err(sd, "Blue Balance: non-positive step value (%d)!\n",
					qctrl->step);
			return -EINVAL;
		}

		qctrl->type = V4L2_CTRL_TYPE_INTEGER;
		strcpy(qctrl->name, "Blue Balance");
		break;

	case V4L2_CID_SHARPNESS:
		avt_dbg(sd, "case V4L2_CID_SHARPNESS\n");

		if (!feature_inquiry_reg.feature_inq.sharpness_avail) {
			avt_info(sd, "control 'Sharpness' not supported by firmware\n");
			qctrl->flags = V4L2_CTRL_FLAG_DISABLED;
			return 0;
		}

		/* reading the Sharpness value */
		ret = avt_reg_read(client,
				priv->cci_reg.bcrm_addr + BCRM_SHARPNESS_32RW,
				AV_CAM_REG_SIZE, AV_CAM_DATA_SIZE_32,
				(char *) &value);
		if (ret < 0) {
			avt_err(sd, "BCRM_SHARPNESS_32RW: i2c read failed (%d)\n",
					ret);
			return ret;
		}

		qctrl->default_value = value;

		/* reading the Minimum sharpness */
		ret = avt_reg_read(client,
				priv->cci_reg.bcrm_addr + BCRM_SHARPNESS_MIN_32R,
				AV_CAM_REG_SIZE, AV_CAM_DATA_SIZE_32,
				(char *) &value);
		if (ret < 0) {
			avt_err(sd, "BCRM_SHARPNESS_MIN_32R: i2c read failed (%d)\n",
					ret);
			return ret;
		}

		qctrl->minimum = value;

		/* reading the Maximum sharpness */
		ret = avt_reg_read(client,
				priv->cci_reg.bcrm_addr + BCRM_SHARPNESS_MAX_32R,
				AV_CAM_REG_SIZE, AV_CAM_DATA_SIZE_32,
				(char *) &value);
		if (ret < 0) {
			avt_err(sd, "BCRM_SHARPNESS_MAX_32R: i2c read failed (%d)\n",
					ret);
			return ret;
		}

		qctrl->maximum = value;

		/* reading the sharpness step increment */
		ret = avt_reg_read(client,
				priv->cci_reg.bcrm_addr + BCRM_SHARPNESS_INC_32R,
				AV_CAM_REG_SIZE, AV_CAM_DATA_SIZE_32,
				(char *) &value);
		if (ret < 0) {
			avt_err(sd, "BCRM_SHARPNESS_INC_32R: i2c read failed (%d)\n",
					ret);
			return ret;
		}

		qctrl->step = value;

		if (qctrl->minimum > qctrl->maximum) {
			avt_err(sd, "Sharpness: min > max! (%d > %d)\n",
					qctrl->minimum, qctrl->maximum);
			return -EINVAL;
		}
		if (qctrl->step <= 0) {
			avt_err(sd, "Sharpness: non-positive step value (%d)!\n",
					qctrl->step);
			return -EINVAL;
		}

		qctrl->type = V4L2_CTRL_TYPE_INTEGER;
		strcpy(qctrl->name, "Sharpness");
		break;

	case V4L2_CID_EXPOSURE_AUTO_MIN:
		avt_dbg(sd, "case V4L2_CID_EXPOSURE_AUTO_MIN\n");

		if (!feature_inquiry_reg.feature_inq.exposure_auto) {
			avt_info(sd, "control 'Exposure Auto Min' not supported by firmware\n");
			qctrl->flags = V4L2_CTRL_FLAG_DISABLED;
			return 0;
		}

		/* Exposure Auto max value should be non-zero */
		ret = avt_reg_read(client,
				priv->cci_reg.bcrm_addr + BCRM_EXPOSURE_AUTO_MAX_64RW,
				AV_CAM_REG_SIZE, AV_CAM_DATA_SIZE_64,
				(char *) &value64);
		if (ret < 0) {
			avt_err(sd, "BCRM_EXPOSURE_AUTO_MAX_64RW: i2c read failed (%d)\n",
					ret);
			return ret;
		}
		bcrm_v4l2.max_bcrm = value64;

		/* reading the Auto Exposure min time */
		ret = avt_reg_read(client,
				priv->cci_reg.bcrm_addr + BCRM_EXPOSURE_AUTO_MIN_64RW,
				AV_CAM_REG_SIZE, AV_CAM_DATA_SIZE_64,
				(char *) &value64);
		if (ret < 0) {
			avt_err(sd, "BCRM_EXPOSURE_AUTO_MIN_64RW: i2c read failed (%d)\n",
					ret);
			return ret;
		}
		qctrl->default_value = value64;

		/* get min and max times for Exposure they are also valid for
		   Auto Exposure min time */
		ret = avt_reg_read(client,
				priv->cci_reg.bcrm_addr + BCRM_EXPOSURE_TIME_MIN_64R,
				AV_CAM_REG_SIZE, AV_CAM_DATA_SIZE_64,
				(char *) &value64);
		if (ret < 0) {
			avt_err(sd, "BCRM_EXPOSURE_TIME_MIN_64R: i2c read failed (%d)\n",
					ret);
			return ret;
		}
		bcrm_v4l2.min_bcrm = value64;

		/* reading the Maximum Exposure time */
		ret = avt_reg_read(client,
				priv->cci_reg.bcrm_addr + BCRM_EXPOSURE_TIME_MAX_64R,
				AV_CAM_REG_SIZE, AV_CAM_DATA_SIZE_64,
				(char *) &value64);
		if (ret < 0) {
			avt_err(sd, "BCRM_EXPOSURE_TIME_MAX_64R: i2c read failed (%d)\n",
					ret);
			return ret;
		}
		/* max auto exposure time should be
		   <= (min(Exposure Auto max value, max exposure time)*/
		if(bcrm_v4l2.max_bcrm > value64)
			bcrm_v4l2.max_bcrm = value64;

		/* reading the Step from Exposure time */
		ret = avt_reg_read(client,
				priv->cci_reg.bcrm_addr + BCRM_EXPOSURE_TIME_INC_64R,
				AV_CAM_REG_SIZE, AV_CAM_DATA_SIZE_64,
				(char *) &value64);
		if (ret < 0) {
			avt_err(sd, "BCRM_EXPOSURE_TIME_INC_64R: i2c read failed (%d)\n",
					ret);
			return ret;
		}
		bcrm_v4l2.step_bcrm = value64;

		convert_bcrm_to_v4l2(&bcrm_v4l2, min_enum, true);
		convert_bcrm_to_v4l2(&bcrm_v4l2, max_enum, true);
		convert_bcrm_to_v4l2(&bcrm_v4l2, step_enum, true);
		qctrl->minimum = bcrm_v4l2.min_v4l2;
		qctrl->maximum = bcrm_v4l2.max_v4l2;
		qctrl->step = bcrm_v4l2.step_v4l2;
		qctrl->type = V4L2_CTRL_TYPE_INTEGER;
		strcpy(qctrl->name, "Exposure auto min");
		break;

	case V4L2_CID_EXPOSURE_AUTO_MAX:
		avt_dbg(sd, "case V4L2_CID_EXPOSURE_AUTO_MAX\n");

		if (!feature_inquiry_reg.feature_inq.exposure_auto) {
			avt_info(sd, "control 'Exposure Auto Max' not supported by firmware\n");
			qctrl->flags = V4L2_CTRL_FLAG_DISABLED;
			return 0;
		}

		/* Exposure Auto max value should be non-zero */
		ret = avt_reg_read(client,
				priv->cci_reg.bcrm_addr + BCRM_EXPOSURE_AUTO_MAX_64RW,
				AV_CAM_REG_SIZE, AV_CAM_DATA_SIZE_64,
				(char *) &value64);
		if (ret < 0) {
			avt_err(sd, "BCRM_EXPOSURE_AUTO_MAX_64RW: i2c read failed (%d)\n",
					ret);
			return ret;
		}

        /* workaround for older driver release:
           Avoid default out of range for 32 bit controls */
        if(value64 > S32_MAX) {
            value64 = S32_MAX;
        }
		qctrl->default_value = value64;

		/* reading the Auto Exposure min time */
		ret = avt_reg_read(client,
				priv->cci_reg.bcrm_addr + BCRM_EXPOSURE_AUTO_MIN_64RW,
				AV_CAM_REG_SIZE, AV_CAM_DATA_SIZE_64,
				(char *) &value64);
		if (ret < 0) {
			avt_err(sd, "BCRM_EXPOSURE_AUTO_MIN_64RW: i2c read failed (%d)\n",
					ret);
			return ret;
		}
		bcrm_v4l2.min_bcrm = value64;

		/* get min and max times for Exposure they are also valid for
		   Auto Exposure max time */
		ret = avt_reg_read(client,
				priv->cci_reg.bcrm_addr + BCRM_EXPOSURE_TIME_MAX_64R,
				AV_CAM_REG_SIZE, AV_CAM_DATA_SIZE_64,
				(char *) &value64);
		if (ret < 0) {
			avt_err(sd, "BCRM_EXPOSURE_TIME_MAX_64R: i2c read failed (%d)\n",
					ret);
			return ret;
		}
		bcrm_v4l2.max_bcrm = value64;

		/* reading the Minimum Exposure time */
		ret = avt_reg_read(client,
				priv->cci_reg.bcrm_addr + BCRM_EXPOSURE_TIME_MIN_64R,
				AV_CAM_REG_SIZE, AV_CAM_DATA_SIZE_64,
				(char *) &value64);
		if (ret < 0) {
			avt_err(sd, "BCRM_EXPOSURE_TIME_MIN_64R: i2c read failed (%d)\n",
					ret);
			return ret;
		}
		/* main auto exposure time should be
		   >= (max(Exposure Auto min value, min exposure time)*/
		if(bcrm_v4l2.min_bcrm < value64)
			bcrm_v4l2.min_bcrm = value64;

		/* reading the Step from Exposure time */
		ret = avt_reg_read(client,
				priv->cci_reg.bcrm_addr + BCRM_EXPOSURE_TIME_INC_64R,
				AV_CAM_REG_SIZE, AV_CAM_DATA_SIZE_64,
				(char *) &value64);
		if (ret < 0) {
			avt_err(sd, "BCRM_EXPOSURE_TIME_INC_64R: i2c read failed (%d)\n",
					ret);
			return ret;
		}
		bcrm_v4l2.step_bcrm = value64;

		convert_bcrm_to_v4l2(&bcrm_v4l2, min_enum, true);
		convert_bcrm_to_v4l2(&bcrm_v4l2, max_enum, true);
		convert_bcrm_to_v4l2(&bcrm_v4l2, step_enum, true);
		qctrl->minimum = bcrm_v4l2.min_v4l2;
		qctrl->maximum = bcrm_v4l2.max_v4l2;
		qctrl->step = bcrm_v4l2.step_v4l2;
		qctrl->type = V4L2_CTRL_TYPE_INTEGER;
		strcpy(qctrl->name, "Exposure auto max");
		break;
	case V4L2_CID_AUTOGAIN_MIN:

		avt_dbg(sd, "case V4L2_CID_AUTOGAIN_MIN\n");

		if (!feature_inquiry_reg.feature_inq.gain_auto) {
			avt_info(sd, "control 'Auto Gain Min' not supported by firmware\n");
			qctrl->flags = V4L2_CTRL_FLAG_DISABLED;
			return 0;
		}

		/* Auto gain max value should be non-zero */
		ret = avt_reg_read(client,
				priv->cci_reg.bcrm_addr + BCRM_GAIN_AUTO_MAX_64RW,
				AV_CAM_REG_SIZE, AV_CAM_DATA_SIZE_64,
				(char *) &value64);
		if (ret < 0) {
			avt_err(sd, "BCRM_GAIN_AUTO_MAX_64RW: i2c read failed (%d)\n",
					ret);
			return ret;
		}
		bcrm_v4l2.max_bcrm = value64;

		/* reading the Auto gain min val */
		ret = avt_reg_read(client,
				priv->cci_reg.bcrm_addr + BCRM_GAIN_AUTO_MIN_64RW,
				AV_CAM_REG_SIZE, AV_CAM_DATA_SIZE_64,
				(char *) &value64);
		if (ret < 0) {
			avt_err(sd, "BCRM_GAIN_AUTO_MIN_64RW: i2c read failed (%d)\n",
					ret);
			return ret;
		}
		qctrl->default_value = value64;

		/* get min and max vals for auto gain they are also valid for
		   Auto gain min val */
		ret = avt_reg_read(client,
				priv->cci_reg.bcrm_addr + BCRM_GAIN_MIN_64R,
				AV_CAM_REG_SIZE, AV_CAM_DATA_SIZE_64,
				(char *) &value64);
		if (ret < 0) {
			avt_err(sd, "BCRM_GAIN_MIN_64R: i2c read failed (%d)\n",
					ret);
			return ret;
		}
		bcrm_v4l2.min_bcrm = value64;

		/* reading the Maximum gain time */
		ret = avt_reg_read(client,
				priv->cci_reg.bcrm_addr + BCRM_GAIN_MAX_64R,
				AV_CAM_REG_SIZE, AV_CAM_DATA_SIZE_64,
				(char *) &value64);
		if (ret < 0) {
			avt_err(sd, "BCRM_GAIN_MAX_64R: i2c read failed (%d)\n",
					ret);
			return ret;
		}
		/* max auto gain val should be
		   <= (min(Auto gian max value, max gain val)*/
		if(bcrm_v4l2.max_bcrm > value64)
			bcrm_v4l2.max_bcrm = value64;

		/* reading the Step from Exposure time */
		ret = avt_reg_read(client,
				priv->cci_reg.bcrm_addr + BCRM_GAIN_INC_64R,
				AV_CAM_REG_SIZE, AV_CAM_DATA_SIZE_64,
				(char *) &value64);
		if (ret < 0) {
			avt_err(sd, "BCRM_GAIN_INC_64R: i2c read failed (%d)\n",
					ret);
			return ret;
		}
		bcrm_v4l2.step_bcrm = value64;

		convert_bcrm_to_v4l2(&bcrm_v4l2, min_enum, true);
		convert_bcrm_to_v4l2(&bcrm_v4l2, max_enum, true);
		convert_bcrm_to_v4l2(&bcrm_v4l2, step_enum, true);
		qctrl->minimum = bcrm_v4l2.min_v4l2;
		qctrl->maximum = bcrm_v4l2.max_v4l2;
		qctrl->step = bcrm_v4l2.step_v4l2;
		qctrl->type = V4L2_CTRL_TYPE_INTEGER;
		strcpy(qctrl->name, "Auto gain min");
		break;

	case V4L2_CID_AUTOGAIN_MAX:

		avt_dbg(sd, "case V4L2_CID_AUTOGAIN_MAX\n");

		if (!feature_inquiry_reg.feature_inq.gain_auto) {
			avt_info(sd, "control 'Auto Gain Max' not supported by firmware\n");
			qctrl->flags = V4L2_CTRL_FLAG_DISABLED;
			return 0;
		}

		/* Auto gain max value should be non-zero */
		ret = avt_reg_read(client,
				priv->cci_reg.bcrm_addr + BCRM_GAIN_AUTO_MAX_64RW,
				AV_CAM_REG_SIZE, AV_CAM_DATA_SIZE_64,
				(char *) &value64);
		if (ret < 0) {
			avt_err(sd, "BCRM_GAIN_AUTO_MAX_64RW: i2c read failed (%d)\n",
					ret);
			return ret;
		}
		qctrl->default_value = value64;

		/* reading the Auto gain min val */
		ret = avt_reg_read(client,
				priv->cci_reg.bcrm_addr + BCRM_GAIN_AUTO_MIN_64RW,
				AV_CAM_REG_SIZE, AV_CAM_DATA_SIZE_64,
				(char *) &value64);
		if (ret < 0) {
			avt_err(sd, "BCRM_GAIN_AUTO_MIN_64RW: i2c read failed (%d)\n",
					ret);
			return ret;
		}
		bcrm_v4l2.min_bcrm = value64;

		/* get min and max vals for auto gain they are also valid for
		   Auto gain max val */
		ret = avt_reg_read(client,
				priv->cci_reg.bcrm_addr + BCRM_GAIN_MAX_64R,
				AV_CAM_REG_SIZE, AV_CAM_DATA_SIZE_64,
				(char *) &value64);
		if (ret < 0) {
			avt_err(sd, "BCRM_GAIN_MAX_64R: i2c read failed (%d)\n",
					ret);
			return ret;
		}
		bcrm_v4l2.max_bcrm = value64;

		/* reading the Minimum gain time */
		ret = avt_reg_read(client,
				priv->cci_reg.bcrm_addr + BCRM_GAIN_MIN_64R,
				AV_CAM_REG_SIZE, AV_CAM_DATA_SIZE_64,
				(char *) &value64);
		if (ret < 0) {
			avt_err(sd, "BCRM_GAIN_MIN_64R: i2c read failed (%d)\n",
					ret);
			return ret;
		}
		/* min auto gain val should be
		   >= (max(Auto gain min value, min gain val)*/
		if(bcrm_v4l2.min_bcrm < value64)
			bcrm_v4l2.min_bcrm = value64;

		/* reading the Step from Exposure time */
		ret = avt_reg_read(client,
				priv->cci_reg.bcrm_addr + BCRM_GAIN_INC_64R,
				AV_CAM_REG_SIZE, AV_CAM_DATA_SIZE_64,
				(char *) &value64);
		if (ret < 0) {
			avt_err(sd, "BCRM_GAIN_INC_64R: i2c read failed (%d)\n",
					ret);
			return ret;
		}
		bcrm_v4l2.step_bcrm = value64;

		convert_bcrm_to_v4l2(&bcrm_v4l2, min_enum, true);
		convert_bcrm_to_v4l2(&bcrm_v4l2, max_enum, true);
		convert_bcrm_to_v4l2(&bcrm_v4l2, step_enum, true);
		qctrl->minimum = bcrm_v4l2.min_v4l2;
		qctrl->maximum = bcrm_v4l2.max_v4l2;
		qctrl->step = bcrm_v4l2.step_v4l2;
		qctrl->default_value = qctrl->maximum;
		qctrl->type = V4L2_CTRL_TYPE_INTEGER;
		strcpy(qctrl->name, "Auto gain max");
		break;

	case V4L2_CID_EXPOSURE_ACTIVE_LINE_MODE:
		avt_dbg(sd, "case V4L2_CID_EXPOSURE_ACTIVE_LINE_MODE\n");

		if (!feature_inquiry_reg.feature_inq.exposure_active_line_avail) {
			avt_info(sd, "control 'exposure active line' not supported by firmware\n");
			qctrl->flags = V4L2_CTRL_FLAG_DISABLED;
			return 0;
		}

		/* reading the exposure active line mode value */
		ret = avt_reg_read(client,
				priv->cci_reg.bcrm_addr + BCRM_EXPOSURE_ACTIVE_LINE_MODE_8RW,
				AV_CAM_REG_SIZE, AV_CAM_DATA_SIZE_8,
				(char *) &value);
		if (ret < 0) {
			avt_err(sd, "BCRM_EXPOSURE_ACTIVE_LINE_MODE_8RW: i2c read failed (%d)\n",
					ret);
			return ret;
		}

		if (value == 1)
			/* true (ON) */
			qctrl->default_value = true;
		else
			/* false (OFF) */
			qctrl->default_value = false;

		qctrl->minimum = 0;
		qctrl->step = 1;
		qctrl->maximum = 1;
		qctrl->type = V4L2_CTRL_TYPE_BOOLEAN;

		strcpy(qctrl->name, "Exposure Active Line Mode");
		break;

	case V4L2_CID_EXPOSURE_ACTIVE_LINE_SELECTOR:
		{
			char selector;
			avt_dbg(sd, "case V4L2_CID_EXPOSURE_ACTIVE_LINE_SELECTOR\n");

			if (!feature_inquiry_reg.feature_inq.exposure_active_line_avail) {
				avt_info(sd, "control 'exposure active line' not supported by firmware\n");
				qctrl->flags = V4L2_CTRL_FLAG_DISABLED;
				return 0;
			}

			/* reading the exposure active line mode value */
			ret = avt_reg_read(client,
					priv->cci_reg.bcrm_addr + BCRM_EXPOSURE_ACTIVE_LINE_SELECTOR_8RW,
					AV_CAM_REG_SIZE, AV_CAM_DATA_SIZE_8,
					(char *) &selector);
			if (ret < 0) {
				avt_err(sd, "BCRM_EXPOSURE_ACTIVE_LINE_SELECTOR_8RW: i2c read failed (%d)\n",
						ret);
				return ret;
			}

			qctrl->default_value = selector;
			qctrl->minimum = 0;
			qctrl->step = 1;
			qctrl->maximum = 1;
			qctrl->type = V4L2_CTRL_TYPE_INTEGER;

			strcpy(qctrl->name, "Exposure Active Line Selector");
			break;
		}

	case V4L2_CID_EXPOSURE_ACTIVE_INVERT:
		{
			avt_dbg(sd, "case V4L2_CID_EXPOSURE_ACTIVE_INVERT\n");

			if (!feature_inquiry_reg.feature_inq.exposure_active_line_avail) {
				avt_info(sd, "control 'exposure active line' not supported by firmware\n");
				qctrl->flags = V4L2_CTRL_FLAG_DISABLED;
				return 0;
			}

			qctrl->default_value = false;
			qctrl->minimum = 0;
			qctrl->step = 1;
			qctrl->maximum = 1;
			qctrl->type = V4L2_CTRL_TYPE_BOOLEAN;

			strcpy(qctrl->name, "Exposure Active Invert");
			break;
		}

	default:
		avt_info(sd, "case default or not supported qctrl->id 0x%x\n",
				qctrl->id);
		qctrl->flags = V4L2_CTRL_FLAG_DISABLED;
		return 0;
	}

	if (ret < 0) {
		avt_err(sd, "i2c read failed (%d)\n", ret);
		return ret;
	}

	avt_dbg(sd, "ret = %d\n", ret);

	return 0;
}

static int32_t convert_bcrm_to_v4l2_gctrl(struct bcrm_to_v4l2 *bcrmv4l2,
		int64_t val64, bool abs)
{
	int32_t value = 0;
	int32_t min = 0;
	int32_t max = 0;
	int32_t step = 0;
	int32_t result = 0;
	int32_t valuedown = 0;
	int32_t valueup = 0;

/* 1. convert to double */
	step = bcrmv4l2->step_v4l2;
	max = bcrmv4l2->max_v4l2;
	min = bcrmv4l2->min_v4l2;
	value = (int32_t) val64;

	/* 2. convert the units */
/*	value *= factor; */

	/* V4L2_CID_EXPOSURE_ABSOLUTE */
	if (abs) {
		if (value != 0)
			do_div(value, 100000UL);
	}

	/* 3. Round value to next integer */

	if (value < S32_MIN)
		result = S32_MIN;
	else if (value > S32_MAX)
		result = S32_MAX;
	else
		result = value;

	/* 4. Clamp to limits */
	if (result > max)
		result = max;
	else if (result < min)
		result = min;

	/* 5. Use nearest increment */
	valuedown = result - ((result - min) % (step));
	valueup = valuedown + step;

	if (result >= 0) {
		if (((valueup - result) <= (result - valuedown))
		&& (valueup <= bcrmv4l2->max_bcrm))
			result = valueup;
		else
			result = valuedown;
	} else {
		if (((valueup - result) < (result - valuedown))
			&& (valueup <= bcrmv4l2->max_bcrm))
			result = valueup;
		else
			result = valuedown;
	}

	return result;
}

static int avt_ioctl_g_ctrl(struct v4l2_subdev *sd, struct v4l2_control *vc)
{
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	struct camera_common_data *s_data = to_camera_common_data(&client->dev);
	struct avt_csi2_priv *priv = (struct avt_csi2_priv *)s_data->priv;

	unsigned int reg = 0;
	int length = 0;
	struct bcrm_to_v4l2 bcrm_v4l2;
	struct v4l2_queryctrl qctrl;
	int ret = 0;
	uint64_t val64 = 0;

	avt_dbg(sd, "\n");

	vc->value = 0;

	switch (vc->id) {

/* BLACK LEVEL is deprecated and thus we use Brightness */
	case V4L2_CID_BRIGHTNESS:
		avt_dbg(sd, "V4L2_CID_BRIGHTNESS\n");
		reg = BCRM_BLACK_LEVEL_32RW;
		length = AV_CAM_DATA_SIZE_32;
		break;
	case V4L2_CID_GAMMA:
		avt_dbg(sd, "V4L2_CID_GAMMA\n");
		reg = BCRM_GAIN_64RW;
		length = AV_CAM_DATA_SIZE_64;
		break;
	case V4L2_CID_CONTRAST:
		avt_dbg(sd, "V4L2_CID_CONTRAST\n");
		reg = BCRM_CONTRAST_VALUE_32RW;
		length = AV_CAM_DATA_SIZE_32;
		break;
	case V4L2_CID_DO_WHITE_BALANCE:
		avt_dbg(sd, "V4L2_CID_DO_WHITE_BALANCE\n");
		reg = BCRM_WHITE_BALANCE_AUTO_8RW;
		length = AV_CAM_DATA_SIZE_8;
		break;
	case V4L2_CID_AUTO_WHITE_BALANCE:
		avt_dbg(sd, "V4L2_CID_AUTO_WHITE_BALANCE\n");
		reg = BCRM_WHITE_BALANCE_AUTO_8RW;
		length = AV_CAM_DATA_SIZE_8;
		break;
	case V4L2_CID_SATURATION:
		avt_dbg(sd, "V4L2_CID_SATURATION\n");
		reg = BCRM_SATURATION_32RW;
		length = AV_CAM_DATA_SIZE_32;
		break;
	case V4L2_CID_HUE:
		avt_dbg(sd, "V4L2_CID_HUE\n");
		reg = BCRM_HUE_32RW;
		length = AV_CAM_DATA_SIZE_32;
		break;
	case V4L2_CID_RED_BALANCE:
		avt_dbg(sd, "V4L2_CID_RED_BALANCE\n");
		reg = BCRM_RED_BALANCE_RATIO_64RW;
		length = AV_CAM_DATA_SIZE_64;
		break;
	case V4L2_CID_BLUE_BALANCE:
		avt_dbg(sd, "V4L2_CID_BLUE_BALANCE\n");
		reg = BCRM_BLUE_BALANCE_RATIO_64RW;
		length = AV_CAM_DATA_SIZE_64;
		break;
	case V4L2_CID_EXPOSURE:
		reg = BCRM_EXPOSURE_TIME_64RW;
		length = AV_CAM_DATA_SIZE_64;
		break;

	case V4L2_CID_GAIN:
		avt_dbg(sd, "V4L2_CID_GAIN\n");
		reg = BCRM_GAIN_64RW;
		length = AV_CAM_DATA_SIZE_64;
		break;

	case V4L2_CID_AUTOGAIN:
		avt_dbg(sd, "V4L2_CID_AUTOGAIN\n");
		reg = BCRM_GAIN_AUTO_8RW;
		length = AV_CAM_DATA_SIZE_8;
		break;

	case V4L2_CID_SHARPNESS:
		avt_dbg(sd, "V4L2_CID_SHARPNESS\n");
		reg = BCRM_SHARPNESS_32RW;
		length = AV_CAM_DATA_SIZE_32;
		break;

	case V4L2_CID_EXPOSURE_AUTO_MIN:
		avt_dbg(sd, "V4L2_CID_EXPOSURE_AUTO_MIN\n");
		reg = BCRM_EXPOSURE_AUTO_MIN_64RW;
		length = AV_CAM_DATA_SIZE_64;
		break;

	case V4L2_CID_EXPOSURE_AUTO_MAX:
		avt_dbg(sd, "V4L2_CID_EXPOSURE_AUTO_MAX\n");
		reg = BCRM_EXPOSURE_AUTO_MAX_64RW;
		length = AV_CAM_DATA_SIZE_64;
		break;

	case V4L2_CID_AUTOGAIN_MIN:
		avt_dbg(sd, "V4L2_CID_AUTOGAIN_MIN\n");
		reg = BCRM_GAIN_AUTO_MIN_64RW;
		length = AV_CAM_DATA_SIZE_64;
		break;

	case V4L2_CID_AUTOGAIN_MAX:
		avt_dbg(sd, "V4L2_CID_AUTOGAIN_MAX\n");
		reg = BCRM_GAIN_AUTO_MAX_64RW;
		length = AV_CAM_DATA_SIZE_64;
		break;

	case V4L2_CID_EXPOSURE_ACTIVE_LINE_MODE:
		avt_dbg(sd, "V4L2_CID_EXPOSURE_ACTIVE_LINE_MODE\n");
		reg = BCRM_EXPOSURE_ACTIVE_LINE_MODE_8RW;
		length = AV_CAM_DATA_SIZE_8;
		break;

	case V4L2_CID_EXPOSURE_ACTIVE_LINE_SELECTOR:
		avt_dbg(sd, "V4L2_CID_EXPOSURE_ACTIVE_LINE_SELECTOR\n");
		reg = BCRM_EXPOSURE_ACTIVE_LINE_SELECTOR_8RW;
		length = AV_CAM_DATA_SIZE_8;
		break;

	case V4L2_CID_EXPOSURE_ACTIVE_INVERT:
		vc->value = priv->acquisition_active_invert;
		return 0;

	default:
		avt_err(sd, "case default or not supported\n");
		return -EINVAL;
	}

	CLEAR(bcrm_v4l2);
	CLEAR(qctrl);

	qctrl.id = vc->id;
	ret = ioctl_queryctrl(sd, &qctrl);

	if (ret < 0) {
		avt_err(sd, "queryctrl failed: ret %d\n", ret);
		return ret;
	}

	bcrm_v4l2.min_v4l2 = qctrl.minimum;
	bcrm_v4l2.max_v4l2 = qctrl.maximum;
	bcrm_v4l2.step_v4l2 = qctrl.step;

	/* Overwrite the queryctrl maximum value for auto features since value
	 * 2 is 'true' (1)
	 */
	if (vc->id == V4L2_CID_AUTOGAIN ||
			vc->id == V4L2_CID_AUTO_WHITE_BALANCE)
		bcrm_v4l2.max_v4l2 = 2;

	/* Check values from BCRM */
	if ((bcrm_v4l2.min_v4l2 > bcrm_v4l2.max_v4l2) ||
			(bcrm_v4l2.step_v4l2 <= 0)) {
		avt_err(sd, "invalid BCRM values found. vc->id %d\n", vc->id);
		return -EINVAL;
	}

	ret = avt_reg_read(client,
			priv->cci_reg.bcrm_addr + reg,
			AV_CAM_REG_SIZE, length, (char *) &val64);

	if (vc->id == V4L2_CID_EXPOSURE_ABSOLUTE)
		/* Absolute */
		vc->value = convert_bcrm_to_v4l2_gctrl(&bcrm_v4l2,
				val64, true);
	else
		vc->value = convert_bcrm_to_v4l2_gctrl(&bcrm_v4l2,
				val64, false);

	/* BCRM Auto Exposure changes -> Refer to BCRM document */
	if (vc->id == V4L2_CID_EXPOSURE_AUTO) {

		if (vc->value == 2)
			 /* continous mode, Refer BCRM doc */
			vc->value = V4L2_EXPOSURE_AUTO;
		else
			/* OFF for off & once mode, Refer BCRM doc */
			vc->value = V4L2_EXPOSURE_MANUAL;
	}

	/* BCRM Auto Gain/WB changes -> Refer to BCRM document */
	if (vc->id == V4L2_CID_AUTOGAIN ||
			vc->id == V4L2_CID_AUTO_WHITE_BALANCE) {

		if (vc->value == 2)
			/* continous mode, Refer BCRM doc */
			vc->value = true;
		else
			/* OFF for off & once mode, Refer BCRM doc */
			vc->value = false;
	}

	return ret;
}


static int avt_get_acquitision_active_line(struct v4l2_subdev *sd, int *line)
{
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	struct camera_common_data *s_data = to_camera_common_data(&client->dev);
	struct avt_csi2_priv *priv = (struct avt_csi2_priv *)s_data->priv;
	int ret;

	ret = avt_reg_read(client,
		priv->cci_reg.bcrm_addr + BCRM_EXPOSURE_ACTIVE_LINE_SELECTOR_8RW,
		AV_CAM_REG_SIZE, AV_CAM_DATA_SIZE_8, (char *) line);

	if (ret < 0) {
		avt_err(sd, "BCRM_EXPOSURE_ACTIVE_LINE_SELECTOR_8RW: i2c read failed (%d)\n",
				ret);
		return ret;
	}
	return 0;
}


static int avt_get_acquisition_active_mode(struct v4l2_subdev *sd, int *mode)
{
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	struct camera_common_data *s_data = to_camera_common_data(&client->dev);
	struct avt_csi2_priv *priv = (struct avt_csi2_priv *)s_data->priv;
	int ret;
	char mode_tmp;

	ret = avt_reg_read(client,
		priv->cci_reg.bcrm_addr + BCRM_EXPOSURE_ACTIVE_LINE_MODE_8RW,
		AV_CAM_REG_SIZE, AV_CAM_DATA_SIZE_8, &mode_tmp);

	if (ret < 0) {
		avt_err(sd, "BCRM_EXPOSURE_ACTIVE_LINE_SELECTOR_8RW: i2c read failed (%d)\n",
				ret);
		return ret;
	}

	*mode = mode_tmp;

	return 0;
}


static int avt_set_acquitision_active_line(struct v4l2_subdev *sd, int line)
{
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	struct camera_common_data *s_data = to_camera_common_data(&client->dev);
	struct avt_csi2_priv *priv = (struct avt_csi2_priv *)s_data->priv;
	int ret, active;
	struct v4l2_ext_control ctrl = { .value = line };

	ret = avt_get_acquisition_active_mode(sd, &active);
	if (ret < 0) {
		return ret;
	}

	if (active) {
		avt_err(sd, "Cannot set acquisition active line while acquisition active mode is enabled\n");
		return -EBUSY;
	}

	ret = ioctl_bcrm_i2cwrite_reg(client, &ctrl, BCRM_EXPOSURE_ACTIVE_LINE_SELECTOR_8RW + priv->cci_reg.bcrm_addr,
			AV_CAM_DATA_SIZE_8);

	if (ret < 0) {
		avt_err(sd, "BCRM_EXPOSURE_ACTIVE_LINE_SELECTOR_8RW: i2c write failed (%d)\n", ret);
		return ret;
	}
	return 0;
}


static int avt_set_acquisition_active_mode(struct v4l2_subdev *sd, int mode)
{
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	struct camera_common_data *s_data = to_camera_common_data(&client->dev);
	struct avt_csi2_priv *priv = (struct avt_csi2_priv *)s_data->priv;
	int line;
	int ret;
	struct v4l2_ext_control ctrl = { 0 };

	ret = avt_get_acquitision_active_line(sd, &line);
	if (ret < 0) {
		return ret;
	}

	ctrl.value = (mode ? (1 | (priv->acquisition_active_invert ? 2 : 0)) : 0) << (8*line);
	ret = ioctl_bcrm_i2cwrite_reg(client, &ctrl, BCRM_LINE_CONFIGURATION_32RW + priv->cci_reg.bcrm_addr,
			AV_CAM_DATA_SIZE_32);
	if (ret < 0) {
		avt_err(sd, "BCRM_LINE_CONFIGURATION_32RW: i2c write failed (%d)\n", ret);
		return ret;
	}

	ctrl.value = mode;
	ret = ioctl_bcrm_i2cwrite_reg(client, &ctrl, BCRM_EXPOSURE_ACTIVE_LINE_MODE_8RW + priv->cci_reg.bcrm_addr,
			AV_CAM_DATA_SIZE_8);
	if (ret < 0) {
		avt_err(sd, "BCRM_EXPOSURE_ACTIVE_LINE_MODE_8RW: i2c write failed (%d)\n", ret);
		return ret;
	}

	return 0;
}


static int avt_set_acquisition_active_invert(struct v4l2_subdev *sd, int invert)
{
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	struct camera_common_data *s_data = to_camera_common_data(&client->dev);
	struct avt_csi2_priv *priv = (struct avt_csi2_priv *)s_data->priv;
	int ret, active;

	ret = avt_get_acquisition_active_mode(sd, &active);
	if (ret < 0) {
		return ret;
	}

	if (active) {
		avt_err(sd, "Cannot set acquisition active invert while acquisition active mode is enabled\n");
		return -EBUSY;
	}

	priv->acquisition_active_invert = invert;

	return 0;
}


static int avt_ioctl_s_ctrl(struct v4l2_subdev *sd, struct v4l2_ext_control *vc)
{
	int ret = 0;
	unsigned int reg = 0;
	int length = 0;
	__s32 value_bkp = 0;
	struct v4l2_queryctrl qctrl;
	struct v4l2_ctrl *vctrl_handle;
	struct avt_csi2_priv *priv = avt_get_priv(sd);
	struct i2c_client *client = v4l2_get_subdevdata(sd);

	switch (vc->id) {

	case V4L2_CID_DO_WHITE_BALANCE:
		avt_dbg(sd, "V4L2_CID_DO_WHITE_BALANCE vc->value %u\n",
				vc->value);
		reg = BCRM_WHITE_BALANCE_AUTO_8RW;
		length = AV_CAM_DATA_SIZE_8;
		vc->value = 1; /* Set 'once' in White Balance Auto Register. */
		break;

/* BLACK LEVEL is deprecated and thus we use Brightness */
	case V4L2_CID_BRIGHTNESS:
		avt_dbg(sd, "V4L2_CID_BRIGHTNESS vc->value %u\n", vc->value);
		reg = BCRM_BLACK_LEVEL_32RW;
		length = AV_CAM_DATA_SIZE_32;

		qctrl.id = V4L2_CID_BRIGHTNESS;/* V4L2_CID_BLACK_LEVEL; */

		ret = ioctl_queryctrl(sd, &qctrl);

		if (ret < 0) {
			avt_err(sd, "queryctrl failed: ret %d\n", ret);
			return ret;
		}
		vc->value = convert_s_ctrl(vc->value,
				qctrl.minimum, qctrl.maximum, qctrl.step);
		break;

	case V4L2_CID_CONTRAST:
		avt_dbg(sd, "V4L2_CID_CONTRAST vc->value %u\n", vc->value);
		reg = BCRM_CONTRAST_VALUE_32RW;
		length = AV_CAM_DATA_SIZE_32;

		qctrl.id = V4L2_CID_CONTRAST;
		ret = ioctl_queryctrl(sd, &qctrl);

		if (ret < 0) {
			avt_err(sd, "queryctrl failed: ret %d\n", ret);
			return ret;
		}

		vc->value = convert_s_ctrl(vc->value,
				qctrl.minimum, qctrl.maximum, qctrl.step);
		break;
	case V4L2_CID_SATURATION:
		avt_dbg(sd, "V4L2_CID_SATURATION vc->value %u\n", vc->value);
		reg = BCRM_SATURATION_32RW;
		length = AV_CAM_DATA_SIZE_32;

		qctrl.id = V4L2_CID_SATURATION;
		ret = ioctl_queryctrl(sd, &qctrl);

		if (ret < 0) {
			avt_err(sd, "queryctrl failed: ret %d\n", ret);
			return ret;
		}

		vc->value = convert_s_ctrl(vc->value,
				qctrl.minimum, qctrl.maximum, qctrl.step);
		break;
	case V4L2_CID_HUE:
		avt_dbg(sd, "V4L2_CID_HUE vc->value %u\n", vc->value);
		reg = BCRM_HUE_32RW;
		length = AV_CAM_DATA_SIZE_32;

		qctrl.id = V4L2_CID_HUE;
		ret = ioctl_queryctrl(sd, &qctrl);

		if (ret < 0) {
			avt_err(sd, "queryctrl failed: ret %d\n", ret);
			return ret;
		}

		vc->value = convert_s_ctrl(vc->value,
				qctrl.minimum, qctrl.maximum, qctrl.step);

		break;
	case V4L2_CID_RED_BALANCE:
		avt_dbg(sd, "V4L2_CID_RED_BALANCE vc->value %u\n", vc->value);
		reg = BCRM_RED_BALANCE_RATIO_64RW;
		length = AV_CAM_DATA_SIZE_64;

		qctrl.id = V4L2_CID_RED_BALANCE;
		ret = ioctl_queryctrl(sd, &qctrl);

		if (ret < 0) {
			avt_err(sd, "queryctrl failed: ret %d\n", ret);
			return ret;
		}

		vc->value = convert_s_ctrl(vc->value,
				qctrl.minimum, qctrl.maximum, qctrl.step);
		break;
	case V4L2_CID_BLUE_BALANCE:
		avt_dbg(sd, "V4L2_CID_BLUE_BALANCE vc->value %u\n", vc->value);
		reg = BCRM_BLUE_BALANCE_RATIO_64RW;
		length = AV_CAM_DATA_SIZE_64;

		qctrl.id = V4L2_CID_BLUE_BALANCE;
		ret = ioctl_queryctrl(sd, &qctrl);

		if (ret < 0) {
			avt_err(sd, "queryctrl failed: ret %d\n", ret);
			return ret;
		}

		vc->value = convert_s_ctrl(vc->value,
				qctrl.minimum, qctrl.maximum, qctrl.step);
		break;

	case V4L2_CID_AUTO_WHITE_BALANCE:
		avt_dbg(sd, "V4L2_CID_AUTO_WHITE_BALANCE vc->value %u\n",
				vc->value);
		reg = BCRM_WHITE_BALANCE_AUTO_8RW;
		length = AV_CAM_DATA_SIZE_8;

		/* BCRM Auto White balance changes	*/
		if (vc->value == true)
			vc->value = 2;/* Continouous mode */
		else
			vc->value = 0;/* 1; OFF/once mode */

		break;
	case V4L2_CID_GAMMA:
		avt_dbg(sd, "V4L2_CID_GAMMA vc->value %u\n", vc->value);
		reg = BCRM_GAMMA_64RW;
		length = AV_CAM_DATA_SIZE_64;

		qctrl.id = V4L2_CID_GAMMA;
		ret = ioctl_queryctrl(sd, &qctrl);

		if (ret < 0) {
			avt_err(sd, "queryctrl failed: ret %d\n", ret);
			return ret;
		}
		vc->value = convert_s_ctrl(vc->value,
				qctrl.minimum, qctrl.maximum, qctrl.step);
		break;
	case V4L2_CID_EXPOSURE:
		avt_dbg(sd, "V4L2_CID_EXPOSURE, cci_reg.bcrm_addr 0x%x, vc->value %u\n",
				priv->cci_reg.bcrm_addr, vc->value);

		value_bkp = vc->value;/* backup the actual value */

		/*  i) Setting 'Manual' in Exposure Auto reg. Refer to BCRM
		 *  document
		 */
		vc->value = 0;

		avt_dbg(sd, "V4L2_CID_EXPOSURE, cci_reg.bcrm_addr 0x%x, vc->value %u\n",
				priv->cci_reg.bcrm_addr, vc->value);

		ret = ioctl_bcrm_i2cwrite_reg(client,
				vc, BCRM_EXPOSURE_AUTO_8RW + priv->cci_reg.bcrm_addr,
				AV_CAM_DATA_SIZE_8);

		if (ret < 0) {
			avt_err(sd, "queryctrl failed: ret %d\n", ret);
			return ret;
		}

		/* ii) Setting value in Exposure reg. */
		vc->value = value_bkp;/* restore the actual value */
		reg = BCRM_EXPOSURE_TIME_64RW;
		length = AV_CAM_DATA_SIZE_64;

		qctrl.id = V4L2_CID_EXPOSURE;
		ret = ioctl_queryctrl(sd, &qctrl);

		if (ret < 0) {
			avt_err(sd, "queryctrl failed: ret %d\n", ret);
			return ret;
		}
		vc->value = convert_s_ctrl(vc->value,
				qctrl.minimum, qctrl.maximum, qctrl.step);

		/* Setting the absolute exposure control */
		vctrl_handle = avt_get_control(sd, V4L2_CID_EXPOSURE_ABSOLUTE);
		if (vctrl_handle != NULL) {
			if (!priv->cross_update) {
				priv->cross_update = true;
				__v4l2_ctrl_s_ctrl_int64(vctrl_handle, vc->value / EXP_ABS);
			} else {
				priv->cross_update = false;
			}
		}

		break;

	case V4L2_CID_EXPOSURE_ABSOLUTE:
		avt_dbg(sd, "V4L2_CID_EXPOSURE_ABSOLUTE, cci_reg.bcrm_addr 0x%x, vc->value %u\n",
				priv->cci_reg.bcrm_addr, vc->value);

		value_bkp = vc->value;/* backup the actual value */

		/* i) Setting 'Manual' in Exposure Auto reg. */
		vc->value = 0;

		avt_dbg(sd, "V4L2_CID_EXPOSURE_ABSOLUTE, cci_reg.bcrm_addr 0x%x, vc->value %u\n",
				priv->cci_reg.bcrm_addr, vc->value);

		ret = ioctl_bcrm_i2cwrite_reg(client,
				vc, BCRM_EXPOSURE_AUTO_8RW + priv->cci_reg.bcrm_addr,
				AV_CAM_DATA_SIZE_8);

		if (ret < 0) {
			avt_err(sd, "i2c write failed\n");
			return ret;
		}

		/* ii) Setting value in Exposure reg. */
		vc->value = value_bkp;/* restore the actual value */
		reg = BCRM_EXPOSURE_TIME_64RW;
		length = AV_CAM_DATA_SIZE_64;

		qctrl.id = V4L2_CID_EXPOSURE;
		ret = ioctl_queryctrl(sd, &qctrl);

		if (ret < 0) {
			avt_err(sd, "queryctrl failed: ret %d\n", ret);
			return ret;
		}

		vc->value64 = vc->value64 * EXP_ABS;

		vc->value64 = convert_s_ctrl(vc->value64,
				qctrl.minimum, qctrl.maximum, qctrl.step);

		/* Setting the exposure control */
		vctrl_handle = avt_get_control(sd, V4L2_CID_EXPOSURE);
		if (vctrl_handle != NULL) {
			if (!priv->cross_update) {
				priv->cross_update = true;
				__v4l2_ctrl_s_ctrl(vctrl_handle, vc->value64);
			} else {
				priv->cross_update = false;
			}
		}

		break;

	case V4L2_CID_EXPOSURE_AUTO:
		avt_dbg(sd, "V4L2_CID_EXPOSURE_AUTO vc->value %u\n", vc->value);
		reg = BCRM_EXPOSURE_AUTO_8RW;
		length = AV_CAM_DATA_SIZE_8;

		/* BCRM Auto Gain changes */
		if (vc->value == V4L2_EXPOSURE_AUTO) {
			vc->value = 2;/* Continouous mode */
		} else {
			vc->value = 0;/* 1; OFF/once mode */
		}

		break;

	case V4L2_CID_AUTOGAIN:
		avt_dbg(sd, "V4L2_CID_AUTOGAIN vc->value %u\n", vc->value);
		reg = BCRM_GAIN_AUTO_8RW;
		length = AV_CAM_DATA_SIZE_8;

		/* BCRM Auto Gain changes */
		if (vc->value == true)
			vc->value = 2;/* Continouous mode */
		else
			vc->value = 0;/* 1; OFF/once mode */

		break;
	case V4L2_CID_GAIN:
		avt_dbg(sd, "V4L2_CID_GAIN, vc->value %u\n", vc->value);
		reg = BCRM_GAIN_64RW;
		length = AV_CAM_DATA_SIZE_64;

		qctrl.id = V4L2_CID_GAIN;
		ret = ioctl_queryctrl(sd, &qctrl);

		if (ret < 0) {
			avt_err(sd, "queryctrl failed: ret %d\n", ret);
			return ret;
		}

		vc->value = convert_s_ctrl(vc->value,
				qctrl.minimum, qctrl.maximum, qctrl.step);

		break;

	case V4L2_CID_HFLIP:
		avt_dbg(sd, "V4L2_CID_HFLIP, vc->value %u\n", vc->value);
		reg = BCRM_IMG_REVERSE_X_8RW;
		length = AV_CAM_DATA_SIZE_8;

		qctrl.id = V4L2_CID_HFLIP;
		ret = ioctl_queryctrl(sd, &qctrl);

		if (ret < 0) {
			avt_err(sd, "queryctrl failed: ret %d\n", ret);
			return ret;
		}

		vc->value = convert_s_ctrl(vc->value,
				qctrl.minimum, qctrl.maximum, qctrl.step);
		break;

	case V4L2_CID_VFLIP:
		avt_dbg(sd, "V4L2_CID_VFLIP, vc->value %u\n", vc->value);
		reg = BCRM_IMG_REVERSE_Y_8RW;
		length = AV_CAM_DATA_SIZE_8;

		qctrl.id = V4L2_CID_VFLIP;
		ret = ioctl_queryctrl(sd, &qctrl);

		if (ret < 0) {
			avt_err(sd, "queryctrl failed: ret %d\n", ret);
			return ret;
		}

		vc->value = convert_s_ctrl(vc->value,
				qctrl.minimum, qctrl.maximum, qctrl.step);
		break;

	case V4L2_CID_SHARPNESS:
		avt_dbg(sd, "V4L2_CID_SHARPNESS, vc->value %u\n", vc->value);
		reg = BCRM_SHARPNESS_32RW;
		length = AV_CAM_DATA_SIZE_32;

		qctrl.id = V4L2_CID_SHARPNESS;
		ret = ioctl_queryctrl(sd, &qctrl);

		if (ret < 0) {
			avt_err(sd, "queryctrl failed: ret %d\n", ret);
			return ret;
		}

		vc->value = convert_s_ctrl(vc->value,
				qctrl.minimum, qctrl.maximum, qctrl.step);
		break;

	case V4L2_CID_EXPOSURE_AUTO_MIN:
		avt_dbg(sd, "V4L2_CID_EXPOSURE_AUTO_MIN, vc->value %u\n", vc->value);
		reg = BCRM_EXPOSURE_AUTO_MIN_64RW;
		length = AV_CAM_DATA_SIZE_64;

		qctrl.id = V4L2_CID_EXPOSURE_AUTO_MIN;
		ret = ioctl_queryctrl(sd, &qctrl);

		if (ret < 0) {
			avt_err(sd, "queryctrl failed: ret %d\n", ret);
			return ret;
		}

		vc->value = convert_s_ctrl(vc->value,
				qctrl.minimum, qctrl.maximum, qctrl.step);
		break;

	case V4L2_CID_EXPOSURE_AUTO_MAX:
		avt_dbg(sd, "V4L2_CID_EXPOSURE_AUTO_MAX, vc->value %u\n", vc->value);
		reg = BCRM_EXPOSURE_AUTO_MAX_64RW;
		length = AV_CAM_DATA_SIZE_64;

		qctrl.id = V4L2_CID_EXPOSURE_AUTO_MAX;
		ret = ioctl_queryctrl(sd, &qctrl);

		if (ret < 0) {
			avt_err(sd, "queryctrl failed: ret %d\n", ret);
			return ret;
		}

		vc->value = convert_s_ctrl(vc->value,
				qctrl.minimum, qctrl.maximum, qctrl.step);
		break;

	case V4L2_CID_AUTOGAIN_MIN:
		avt_dbg(sd, "V4L2_CID_AUTOGAIN_MIN, vc->value %u\n", vc->value);
		reg = BCRM_GAIN_AUTO_MIN_64RW;
		length = AV_CAM_DATA_SIZE_64;

		qctrl.id = V4L2_CID_AUTOGAIN_MIN;
		ret = ioctl_queryctrl(sd, &qctrl);

		if (ret < 0) {
			avt_err(sd, "queryctrl failed: ret %d\n", ret);
			return ret;
		}

		vc->value = convert_s_ctrl(vc->value,
				qctrl.minimum, qctrl.maximum, qctrl.step);
		break;

	case V4L2_CID_AUTOGAIN_MAX:
		avt_dbg(sd, "V4L2_CID_AUTOGAIN_MAX, vc->value %u\n", vc->value);
		reg = BCRM_GAIN_AUTO_MAX_64RW;
		length = AV_CAM_DATA_SIZE_64;

		qctrl.id = V4L2_CID_AUTOGAIN_MAX;
		ret = ioctl_queryctrl(sd, &qctrl);

		if (ret < 0) {
			avt_err(sd, "queryctrl failed: ret %d\n", ret);
			return ret;
		}

		vc->value = convert_s_ctrl(vc->value,
				qctrl.minimum, qctrl.maximum, qctrl.step);
		break;

	case V4L2_CID_EXPOSURE_ACTIVE_LINE_MODE:
		return avt_set_acquisition_active_mode(sd, vc->value);

	case V4L2_CID_EXPOSURE_ACTIVE_LINE_SELECTOR:
		return avt_set_acquitision_active_line(sd, vc->value);

	case V4L2_CID_EXPOSURE_ACTIVE_INVERT:
		return avt_set_acquisition_active_invert(sd, vc->value);

	default:
		avt_err(sd, "case default or not supported\n");
		ret = -EPERM;
		return ret;
	}

	ret = ioctl_bcrm_i2cwrite_reg(client,
			vc, reg + priv->cci_reg.bcrm_addr, length);

	avt_dbg(sd, "ret %d\n", ret);

	return ret < 0 ? ret : 0;
}

static int avt_s_ctrl(struct v4l2_ctrl *ctrl)
{
	struct v4l2_subdev *sd;
	struct v4l2_ext_control c;

	struct avt_csi2_priv *priv;

	priv = container_of(ctrl->handler, struct avt_csi2_priv, hdl);
	sd = priv->subdev;

	c.id = ctrl->id;
	c.value = ctrl->val;
	/* For 64-bit extended V4L2 controls,
	 * new value comes in this pointer
	 */
	c.value64 = *(ctrl->p_new.p_s64);

	return avt_ioctl_s_ctrl(sd, &c);
}

static int avt_g_volatile_ctrl(struct v4l2_ctrl *ctrl)
{
	struct v4l2_subdev *sd;
	struct v4l2_control c;
	struct avt_csi2_priv *priv;
	int ret;

	priv = container_of(ctrl->handler, struct avt_csi2_priv, hdl);
	sd = priv->subdev;

	c.id = ctrl->id;
	ret = avt_ioctl_g_ctrl(sd, &c);
	ctrl->val = c.value;

	return ret;
}

static const struct v4l2_ctrl_ops avt_ctrl_ops = {
	.g_volatile_ctrl = avt_g_volatile_ctrl,
	.s_ctrl		= avt_s_ctrl,
};

static int read_max_resolution(struct v4l2_subdev *sd, uint32_t *max_width, uint32_t *max_height)
{
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	int ret = 0;

	ret = avt_get_param(client, V4L2_AV_CSI2_WIDTH_MAXVAL_R, max_width);
	if (ret < 0)
		return ret;

	ret = avt_get_param(client, V4L2_AV_CSI2_HEIGHT_MAXVAL_R, max_height);
	if (ret < 0)
		return ret;

	return ret;
}

static int avt_cropcap(struct v4l2_subdev *sd, struct v4l2_cropcap *cc)
{
	uint32_t max_width = 0, max_height = 0;

	if (cc->pixelaspect.numerator != 1 ||
			cc->type != V4L2_BUF_TYPE_VIDEO_CAPTURE) {
		return -EINVAL;
	}

	if(read_max_resolution(sd, &max_width, &max_height) < 0)
	{
		return -EINVAL;
	}

	cc->bounds.top = cc->bounds.left = 0;
	cc->defrect.top = cc->defrect.left = 0;

	cc->bounds.width = cc->defrect.width = max_width;
	cc->bounds.height = cc->defrect.height = max_height;

	// align defrect if alignment is enabled
	cc->defrect.width = avt_align_width(sd, cc->defrect.width);
    avt_info(sd, "Default crop rect width %d\n", cc->defrect.width);

	return 0;
}

static int avt_get_selection(struct v4l2_subdev *sd,
		struct v4l2_subdev_pad_config *cfg,
		struct v4l2_subdev_selection *sel)
{
	struct avt_csi2_priv *priv = avt_get_priv(sd);
	uint32_t max_width = 0, max_height = 0;

	if(read_max_resolution(sd, &max_width, &max_height) < 0)
	{
		return -EINVAL;
	}
    
 	switch (sel->target) {
	case V4L2_SEL_TGT_CROP:
		sel->r = priv->frmp.r;
		break;
	case V4L2_SEL_TGT_CROP_DEFAULT:
	case V4L2_SEL_TGT_NATIVE_SIZE:
	case V4L2_SEL_TGT_CROP_BOUNDS:
		sel->r.top = sel->r.left = 0;
		sel->r.width = max_width;
		sel->r.height = max_height;
		break;
	default:
		return -EINVAL;
	}

	sel->flags = V4L2_SEL_FLAG_LE;

	return 0;
}

static int avt_g_crop(struct v4l2_subdev *sd, struct v4l2_crop *crop)
{
	struct v4l2_subdev_selection sel;

	sel.target = V4L2_SEL_TGT_CROP;

	avt_get_selection(sd, NULL, &sel);

	memcpy(&crop->c, &sel.r, sizeof(struct v4l2_rect));

	return 0;
}

static int avt_get_align_width(struct v4l2_subdev *sd)
{
    struct avt_csi2_priv *priv = avt_get_priv(sd);
    int width_align = 0;

	if (priv->crop_align_enabled) {
		/*
		* Align with VI capabilities
		*
		* For each format line length has to be aligned to specific
		* value
		*/
		switch (priv->mbus_fmt_code) {
		case MEDIA_BUS_FMT_RGB888_1X24:
		case MEDIA_BUS_FMT_BGR888_1X24:
			width_align = 16;
            break;

		case MEDIA_BUS_FMT_VYUY8_2X8:
		case MEDIA_BUS_FMT_RGB565_1X16:
			width_align = 32;
            break;

		case MEDIA_BUS_FMT_SRGGB8_1X8:
        case MEDIA_BUS_FMT_SGBRG8_1X8:
        case MEDIA_BUS_FMT_SGRBG8_1X8:
        case MEDIA_BUS_FMT_SBGGR8_1X8:
            width_align = 16;
            break;

		default:
			width_align = 64;
            break;
		}
	}

    /* Use kernel param override? */
    if (v4l2_width_align != 0)
    {
        width_align = v4l2_width_align;
        avt_warn(sd, "v4l2_width_align override: %d\n", width_align);
    }

	return width_align;
}

static int avt_align_width(struct v4l2_subdev *sd, int width)
{
    struct avt_csi2_priv *priv = avt_get_priv(sd);
    avt_dbg(sd, "input width: %d\n", width);
	if (priv->crop_align_enabled) {
		int const align_size = avt_get_align_width(sd);
        avt_dbg(sd, "align_size: %d\n", align_size);

		width = roundup(width, align_size);
		if (width > priv->frmp.maxw) {
			width -= align_size;
		}
        avt_dbg(sd, "output width: %d\n", width);
    }
    else
    {
        avt_dbg(sd, "crop_align_enabled DISABLED\n");
    }
    
	return width;
}

static int avt_set_selection(struct v4l2_subdev *sd,
		struct v4l2_subdev_pad_config *cfg,
		struct v4l2_subdev_selection *sel)
{
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	struct avt_csi2_priv *priv = avt_get_priv(sd);

	// update width, height, offset x/y restrictions from camera
	avt_init_frame_param(sd);

	switch (sel->target) {
	case V4L2_SEL_TGT_CROP:

		if (priv->crop_align_enabled) {
			sel->r.width = avt_align_width(sd, sel->r.width);
		}

/*
*       As per the crop concept document, the following steps should be followed before setting crop to the sensor.
*
* i)    If new width is less or equal than current width, write the width register first then write offset X (left) register,
*       both values should be within the range (min, max and inc).
* ii)   If new width is higher than current width, write the offset X (left) register first then write the width register,
*       both values should be within the range (min, max, and inc)
* iii)  If new height is less or equal than current height, write the height register first then write offset Y (top) register,
*       both values should be within the range (min, max and inc).
* iv)   If new height is higher than current height, write the offset Y (top) register first then write the height register,
*       both values should be within the range (min, max, and inc)
*/

		if (sel->r.width <= priv->frmp.r.width) { /* case i) New width is lesser or equal than current */

			// write width
			sel->r.width = clamp(roundup(sel->r.width, priv->frmp.sw), priv->frmp.minw, priv->frmp.maxw);
			avt_set_param(client, V4L2_AV_CSI2_WIDTH_W, sel->r.width);

			// update width, height, offset x/y restrictions from camera
			avt_init_frame_param(sd);

			// write offset x
			sel->r.left = clamp(roundup(sel->r.left, priv->frmp.swoff), priv->frmp.minwoff, priv->frmp.maxwoff);
			avt_set_param(client, V4L2_AV_CSI2_OFFSET_X_W, sel->r.left);
		}
		else { /* case ii) New width is higher than current */

			// write offset x
			sel->r.left = clamp(roundup(sel->r.left, priv->frmp.swoff), priv->frmp.minwoff, priv->frmp.maxwoff);
			avt_set_param(client, V4L2_AV_CSI2_OFFSET_X_W, sel->r.left);

			// update width, height, offset x/y restrictions from camera
			avt_init_frame_param(sd);

			// write width
			sel->r.width = clamp(roundup(sel->r.width, priv->frmp.sw), priv->frmp.minw, priv->frmp.maxw);
			avt_set_param(client, V4L2_AV_CSI2_WIDTH_W, sel->r.width);
		}

		if (sel->r.height <= priv->frmp.r.height) { /* case iii) New height is lesser or equal than current */
			// write height
			sel->r.height = clamp(roundup(sel->r.height, priv->frmp.sh), priv->frmp.minh, priv->frmp.maxh);
			avt_set_param(client, V4L2_AV_CSI2_HEIGHT_W, sel->r.height);

			// update width, height, offset x/y restrictions from camera
			avt_init_frame_param(sd);

			// write offset y
			sel->r.top = clamp(roundup(sel->r.top, priv->frmp.shoff), priv->frmp.minhoff, priv->frmp.maxhoff);
			avt_set_param(client, V4L2_AV_CSI2_OFFSET_Y_W, sel->r.top);
		}
		else { /* case iv) New height is higher than current */

			// write offset y
			sel->r.top = clamp(roundup(sel->r.top, priv->frmp.shoff), priv->frmp.minhoff, priv->frmp.maxhoff);
			avt_set_param(client, V4L2_AV_CSI2_OFFSET_Y_W, sel->r.top);

			// update width, height, offset x/y restrictions from camera
			avt_init_frame_param(sd);

			// write height
			sel->r.height = clamp(roundup(sel->r.height, priv->frmp.sh), priv->frmp.minh, priv->frmp.maxh);
			avt_set_param(client, V4L2_AV_CSI2_HEIGHT_W, sel->r.height);
		}

		// update width, height, offset x/y restrictions from camera
		avt_init_frame_param(sd);

		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static int avt_s_crop(struct v4l2_subdev *sd, const struct v4l2_crop *crop)
{
	struct v4l2_subdev_selection sel;

	sel.target = V4L2_SEL_TGT_CROP;

	memcpy(&sel.r, &crop->c, sizeof(struct v4l2_rect));

	return avt_set_selection(sd, NULL, &sel);
}

static int avt_s_parm(struct v4l2_subdev *sd, struct v4l2_streamparm *parm)
{
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	struct camera_common_data *s_data = to_camera_common_data(&client->dev);
	struct avt_csi2_priv *priv = (struct avt_csi2_priv *)s_data->priv;
	struct v4l2_fract *tpf = &(priv->streamcap.timeperframe);
	struct v4l2_ext_control vc;
	int ret;
	u64 value64, min, max, step;
	struct bcrm_to_v4l2 bcrm_v4l2;
	union bcrm_feature_reg feature_inquiry_reg;
	CLEAR(bcrm_v4l2);

	/* reading the Feature inquiry register */
	ret = avt_reg_read(client,
			priv->cci_reg.bcrm_addr + BCRM_FEATURE_INQUIRY_64R,
			AV_CAM_REG_SIZE, AV_CAM_DATA_SIZE_64,
			(char *) &value64);
	if (ret < 0) {
		avt_err(sd, "i2c read failed (%d)\n", ret);
		return ret;
	}

	feature_inquiry_reg.value = value64;

	/* Check if setting acquisition frame rate is available */
	if(!feature_inquiry_reg.feature_inq.acquisition_frame_rate) {
			avt_info(sd, "Acquisition frame rate setting not supported by firmware\n");
			return 0;
	}

	/* Copy new settings to internal structure */
	memcpy(&priv->streamcap, &parm->parm.capture, sizeof(struct v4l2_captureparm));

	if (tpf->numerator == 0) {
		/* Enable auto frame rate */
		vc.value = 0;
		ret = ioctl_bcrm_i2cwrite_reg(client, &vc, priv->cci_reg.bcrm_addr + BCRM_ACQUISITION_FRAME_RATE_ENABLE_8RW, AV_CAM_DATA_SIZE_8);
		if (ret < 0) {
			avt_err(sd, "ACQUISITION_FRAME_RATE_64RW: i2c write failed (%d)\n",
					ret);
			return ret;
		}

	} else {
		/* Enable and set manual frame rate */


		/* reading the Minimum Frame Rate Level */
		ret = avt_reg_read(client,
				priv->cci_reg.bcrm_addr + BCRM_ACQUISITION_FRAME_RATE_MIN_64R,
				AV_CAM_REG_SIZE, AV_CAM_DATA_SIZE_64,
				(char *) &value64);
		if (ret < 0) {
			avt_err(sd, "ACQUISITION_FRAME_RATE_MIN_64R: i2c read failed (%d)\n",
					ret);
			return ret;
		}

		bcrm_v4l2.min_bcrm = value64;

		/* reading the Maximum Frame Rate Level */
		ret = avt_reg_read(client,
				priv->cci_reg.bcrm_addr + BCRM_ACQUISITION_FRAME_RATE_MAX_64R,
				AV_CAM_REG_SIZE, AV_CAM_DATA_SIZE_64,
				(char *) &value64);
		if (ret < 0) {
			avt_err(sd, "ACQUISITION_FRAME_RATE_MAX_64R: i2c read failed (%d)\n",
					ret);
			return ret;
		}

		bcrm_v4l2.max_bcrm = value64;

		/* reading the Frame Rate Level step increment */
		ret = avt_reg_read(client,
				priv->cci_reg.bcrm_addr + BCRM_ACQUISITION_FRAME_RATE_INC_64R,
				AV_CAM_REG_SIZE, AV_CAM_DATA_SIZE_64,
				(char *) &value64);
		if (ret < 0) {
			avt_err(sd, "ACQUISITION_FRAME_RATE_INCREMENT_64R: i2c read failed (%d)\n",
					ret);
			return ret;
		}

		bcrm_v4l2.step_bcrm = value64;

		convert_bcrm_to_v4l2(&bcrm_v4l2, min_enum, true);
		convert_bcrm_to_v4l2(&bcrm_v4l2, max_enum, true);
		convert_bcrm_to_v4l2(&bcrm_v4l2, step_enum, true);

		min = bcrm_v4l2.min_v4l2;
		max = bcrm_v4l2.max_v4l2;
		/* Set step to 1 uHz because zero value came from camera register */
		if(!step)
			step = 1;

		if (min > max) {
			avt_err(sd, "Frame rate: min > max! (%llu > %llu)\n",
					min, max);
			return -EINVAL;
		}
		if (step <= 0) {
			avt_err(sd, "Frame rate: non-positive step value (%llu)!\n",
					step);
			return -EINVAL;
		}

		/* Translate timeperframe to frequency
		 * by inverting the fraction
		 */
		avt_dbg(sd, "[mjsob] %ul/%ul\n", tpf->denominator, tpf->numerator);
		value64 = (tpf->denominator / tpf->numerator) * UHZ_TO_HZ;
		value64 = convert_s_ctrl(value64, min, max, step);
		if (value64 < 0) {
			avt_err(sd, "Frame rate: non-positive value (%llu)!\n",
					value64);
			return -EINVAL;
		}

		/* Enable manual frame rate */
		vc.value = 1;
		ret = ioctl_bcrm_i2cwrite_reg(client, &vc, priv->cci_reg.bcrm_addr + BCRM_ACQUISITION_FRAME_RATE_ENABLE_8RW, AV_CAM_DATA_SIZE_8);
		if (ret < 0) {
			avt_err(sd, "ACQUISITION_FRAME_RATE_64RW: i2c write failed (%d)\n",
					ret);
			return ret;
		}

		/* Save new frame rate to camera register */
		vc.value = value64;
		ret = ioctl_bcrm_i2cwrite_reg(client, &vc, priv->cci_reg.bcrm_addr + BCRM_ACQUISITION_FRAME_RATE_64RW, AV_CAM_DATA_SIZE_64);
		if (ret < 0) {
			avt_err(sd, "ACQUISITION_FRAME_RATE_64RW: i2c write failed (%d)\n",
					ret);
			return ret;
		}

		tpf->numerator = FRAQ_NUM;
		tpf->denominator = value64 / FRAQ_NUM;

		/* Copy modified settings back */
		memcpy(&parm->parm.capture, &priv->streamcap, sizeof(struct v4l2_captureparm));
	}

	return 0;
}

static int avt_g_parm(struct v4l2_subdev *sd, struct v4l2_streamparm *parm)
{
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	struct camera_common_data *s_data = to_camera_common_data(&client->dev);
	struct avt_csi2_priv *priv = (struct avt_csi2_priv *)s_data->priv;

	memcpy(&parm->parm.capture, &priv->streamcap, sizeof(struct v4l2_captureparm));

	return 0;
}

static int avt_csi2_open(struct v4l2_subdev *sd, struct v4l2_subdev_fh *fh)
{
	// called when userspace app calls 'open'
	struct avt_csi2_priv *priv = avt_get_priv(sd);
	int ret;
	uint32_t i2c_reg;
	uint32_t i2c_reg_size;
	uint32_t i2c_reg_count;
    const uint32_t poll_delay_ms = 2;
    const uint32_t timeout_ms = 3000;
    unsigned long timeout_jiffies = 0;
	uint8_t bcm_mode = 0;
	char *i2c_reg_buf;;  

	// set stride align
	if (priv->stride_align_enabled)
		set_channel_stride_align_for_format(sd, priv->mbus_fmt_code);
	else
		set_channel_stride_align(sd, 1);

	// set BCRM mode if required
	ret = avt_reg_read(priv->client, CCI_CURRENT_MODE_8R, AV_CAM_REG_SIZE, AV_CAM_DATA_SIZE_8, &bcm_mode);
	if (ret < 0) {
		avt_err(sd, "Failed to get BCM mode: i2c read failed (%d)\n", ret);
		return ret;
	}

	if (bcm_mode != OPERATION_MODE_BCRM)
	{
                // GenCP mode -> Switch back to BCRM
		CLEAR(i2c_reg);
        bcm_mode = OPERATION_MODE_BCRM;
		i2c_reg = CCI_CHANGE_MODE_8W;
		i2c_reg_size = AV_CAM_REG_SIZE;
		i2c_reg_count = AV_CAM_DATA_SIZE_8;
		i2c_reg_buf = (char *)&bcm_mode;
        timeout_jiffies = jiffies + msecs_to_jiffies(timeout_ms);

		ret = ioctl_gencam_i2cwrite_reg(priv->client,
						i2c_reg, i2c_reg_size,
						i2c_reg_count, i2c_reg_buf);
		if (ret < 0)
		{
			avt_err(sd, "Failed to set BCM mode: i2c write failed (%d)\n", ret);
			return ret;
		}

        // Wait for mode change
        do
        {
            usleep_range(poll_delay_ms*1000, (poll_delay_ms*1000)+1);
            ret = avt_reg_read(priv->client, CCI_CURRENT_MODE_8R, AV_CAM_REG_SIZE, AV_CAM_DATA_SIZE_8, &bcm_mode);
        } while ((ret >=0 ) && (bcm_mode != OPERATION_MODE_BCRM) && time_before(jiffies, timeout_jiffies));

        if (bcm_mode != OPERATION_MODE_BCRM)
        {
            return -EINVAL;
        }
	}

	priv->mode = AVT_BCRM_MODE;

	return 0;
}


static const struct v4l2_subdev_core_ops avt_csi2_core_ops = {
	.subscribe_event = avt_csi2_subscribe_event,
	.unsubscribe_event = v4l2_event_subdev_unsubscribe,
	.ioctl = avt_csi2_ioctl,
};

static const struct v4l2_subdev_internal_ops avt_csi2_int_ops = {
	.open = avt_csi2_open,
};

static const struct v4l2_subdev_video_ops avt_csi2_video_ops = {
	.g_mbus_config = avt_csi2_g_mbus_config,
	.s_stream = avt_csi2_s_stream,
	.cropcap = avt_cropcap,
	.s_crop = avt_s_crop,
	.g_crop = avt_g_crop,
	.s_parm = avt_s_parm,
	.g_parm = avt_g_parm,
};

static const struct v4l2_subdev_pad_ops avt_csi2_pad_ops = {
	.set_fmt = avt_csi2_set_fmt,
	.get_fmt = avt_csi2_get_fmt,
	.enum_mbus_code = avt_csi2_enum_mbus_code,
	.enum_frame_size = avt_csi2_enum_framesizes,
	.enum_frame_interval = avt_csi2_enum_frameintervals,
	.get_selection = avt_get_selection,
	.set_selection = avt_set_selection,
};

static const struct v4l2_subdev_ops avt_csi2_subdev_ops = {
	.core = &avt_csi2_core_ops,
	.video = &avt_csi2_video_ops,
	.pad = &avt_csi2_pad_ops,
};

static const struct media_entity_operations avt_csi2_media_ops = {
	.link_validate = v4l2_subdev_link_validate,
};

const struct of_device_id avt_csi2_of_match[] = {
	{ .compatible = "alliedvision,avt_csi2",},
	{ },
};

MODULE_DEVICE_TABLE(of, avt_csi2_of_match);

static int read_cci_registers(struct i2c_client *client)
{
	struct camera_common_data *s_data = to_camera_common_data(&client->dev);
	struct avt_csi2_priv *priv = (struct avt_csi2_priv *)s_data->priv;

	int ret = 0;
	uint32_t crc = 0;
	uint32_t crc_byte_count = 0;

	uint32_t i2c_reg;
	uint32_t i2c_reg_size;
	uint32_t i2c_reg_count;

	char *i2c_reg_buf;

	i2c_reg = cci_cmd_tbl[CCI_REGISTER_LAYOUT_VERSION].address;
	i2c_reg_size = AV_CAM_REG_SIZE;
	/*
	 * Avoid last 4 bytes read as its WRITE only register except
	 * CURRENT MODE REG
	 */
	i2c_reg_count = sizeof(priv->cci_reg) - 4;

	i2c_reg_buf = (char *)&priv->cci_reg;
	/* Calculate CRC from each reg up to the CRC reg */
	crc_byte_count =
		(uint32_t)((char *)&priv->cci_reg.checksum - (char *)&priv->cci_reg);

	dev_info(&client->dev, "crc_byte_count = %d, i2c_reg.count = %d\n",
			crc_byte_count, i2c_reg_count);

	/* read CCI registers */
	ret = i2c_read(client, i2c_reg, i2c_reg_size,
			i2c_reg_count, i2c_reg_buf);

	if (ret < 0) {
		dev_err(&client->dev, "Camera not responding. Error=%d\n", ret);
		return ret;
	}

        /* CRC calculation */
	crc = crc32(U32_MAX, &priv->cci_reg, crc_byte_count);

        /* Swap bytes if neccessary */
	cpu_to_be32s(&priv->cci_reg.layout_version);
	cpu_to_be64s(&priv->cci_reg.device_capabilities);
	cpu_to_be16s(&priv->cci_reg.gcprm_address);
	cpu_to_be16s(&priv->cci_reg.bcrm_addr);
	cpu_to_be32s(&priv->cci_reg.checksum);

        /* Check the checksum of received with calculated. */
	if (crc != priv->cci_reg.checksum) {
		dev_err(&client->dev,
			"wrong CCI CRC value! calculated = 0x%08x, received = 0x%08x\n",
			crc, priv->cci_reg.checksum);
		return -EINVAL;
	}

	dev_info(&client->dev, "cci layout version: 0x%08x\n",
			priv->cci_reg.layout_version);
	dev_info(&client->dev, "cci device capabilities: 0x%016llx\n",
			priv->cci_reg.device_capabilities);
	dev_info(&client->dev, "cci device guid: %s\n",
			priv->cci_reg.device_guid);
	dev_info(&client->dev, "cci gcprm_address: 0x%04x\n",
			priv->cci_reg.gcprm_address);

	return 0;
}

static int read_gencp_registers(struct i2c_client *client)
{
	struct camera_common_data *s_data = to_camera_common_data(&client->dev);
	struct avt_csi2_priv *priv = (struct avt_csi2_priv *)s_data->priv;

	int ret = 0;
	uint32_t crc = 0;
	uint32_t crc_byte_count = 0;

	uint32_t i2c_reg;
	uint32_t i2c_reg_size;
	uint32_t i2c_reg_count;

	char *i2c_reg_buf;

	i2c_reg = priv->cci_reg.gcprm_address + 0x0000;
	i2c_reg_size = AV_CAM_REG_SIZE;
	i2c_reg_count = sizeof(priv->gencp_reg);
	i2c_reg_buf = (char *)&priv->gencp_reg;

	/* Calculate CRC from each reg up to the CRC reg */
	crc_byte_count =
		(uint32_t)((char *)&priv->gencp_reg.checksum - (char *)&priv->gencp_reg);

	ret = i2c_read(client, i2c_reg, i2c_reg_size,
			i2c_reg_count, i2c_reg_buf);

	crc = crc32(U32_MAX, &priv->gencp_reg, crc_byte_count);

	if (ret < 0) {
		pr_err("%s : I2C read failed, ret %d\n", __func__, ret);
		return ret;
	}

	be32_to_cpus(&priv->gencp_reg.gcprm_layout_version);
	be16_to_cpus(&priv->gencp_reg.gencp_out_buffer_address);
	be16_to_cpus(&priv->gencp_reg.gencp_in_buffer_address);
	be16_to_cpus(&priv->gencp_reg.gencp_out_buffer_size);
	be16_to_cpus(&priv->gencp_reg.gencp_in_buffer_size);
	be32_to_cpus(&priv->gencp_reg.checksum);

	if (crc != priv->gencp_reg.checksum) {
		dev_warn(&client->dev,
			"wrong GENCP CRC value! calculated = 0x%08x, received = 0x%08x\n",
			crc, priv->gencp_reg.checksum);
	}

	dev_info(&client->dev, "gcprm layout version: 0x%08x\n",
		priv->gencp_reg.gcprm_layout_version);
	dev_info(&client->dev, "gcprm out buf addr: 0x%04x\n",
		priv->gencp_reg.gencp_out_buffer_address);
	dev_info(&client->dev, "gcprm out buf size: 0x%04x\n",
		priv->gencp_reg.gencp_out_buffer_size);
	dev_info(&client->dev, "gcprm in buf addr: 0x%04x\n",
		priv->gencp_reg.gencp_in_buffer_address);
	dev_info(&client->dev, "gcprm in buf size: 0x%04x\n",
		priv->gencp_reg.gencp_in_buffer_size);

	return 0;
}

static int cci_version_check(struct i2c_client *client)
{
	struct camera_common_data *s_data = to_camera_common_data(&client->dev);
	struct avt_csi2_priv *priv = (struct avt_csi2_priv *)s_data->priv;
	uint32_t cci_minver, cci_majver;

	cci_minver = (priv->cci_reg.layout_version & CCI_REG_LAYOUT_MINVER_MASK)
				>> CCI_REG_LAYOUT_MINVER_SHIFT;

	if (cci_minver == CCI_REG_LAYOUT_MINVER) {
		dev_dbg(&client->dev, "%s: correct cci register minver: %d (0x%x)\n",
				__func__, cci_minver, priv->cci_reg.layout_version);
	} else {
		dev_err(&client->dev, "%s: cci reg minver mismatch! read: %d (0x%x) expected: %d\n",
				__func__, cci_minver, priv->cci_reg.layout_version,
				CCI_REG_LAYOUT_MINVER);
		return -EINVAL;
	}

	cci_majver = (priv->cci_reg.layout_version & CCI_REG_LAYOUT_MAJVER_MASK)
				>> CCI_REG_LAYOUT_MAJVER_SHIFT;

	if (cci_majver == CCI_REG_LAYOUT_MAJVER) {
		dev_dbg(&client->dev, "%s: correct cci register majver: %d (0x%x)\n",
				__func__, cci_majver, priv->cci_reg.layout_version);
	} else {
		dev_err(&client->dev, "%s: cci reg majver mismatch! read: %d (0x%x) expected: %d\n",
				__func__, cci_majver, priv->cci_reg.layout_version,
				CCI_REG_LAYOUT_MAJVER);
		return -EINVAL;
	}

	return 0;
}

static int bcrm_version_check(struct i2c_client *client)
{
	struct camera_common_data *s_data = to_camera_common_data(&client->dev);
	struct avt_csi2_priv *priv = (struct avt_csi2_priv *)s_data->priv;
	u32 value = 0;
	int ret;

	/* reading the BCRM version */
	ret = avt_reg_read(client,
			priv->cci_reg.bcrm_addr + BCRM_VERSION_32R,
			AV_CAM_REG_SIZE, AV_CAM_DATA_SIZE_32,
			(char *) &value);

	if (ret < 0) {
		dev_err(&client->dev, "i2c read failed (%d)\n", ret);
		return ret;
	}

	dev_info(&client->dev, "bcrm version (driver): 0x%08x (%d.%d)\n",
						BCRM_DEVICE_VERSION,
						BCRM_MAJOR_VERSION,
						BCRM_MINOR_VERSION);

	dev_info(&client->dev, "bcrm version (camera): 0x%08x (%d.%d)\n",
						value,
						(value & 0xffff0000) >> 16,
						(value & 0x0000ffff));

	return (value >> 16) == BCRM_MAJOR_VERSION ? 1 : 0;
}

static int gcprm_version_check(struct i2c_client *client)
{

	struct camera_common_data *s_data = to_camera_common_data(&client->dev);
	struct avt_csi2_priv *priv = (struct avt_csi2_priv *)s_data->priv;
	u32 value = priv->gencp_reg.gcprm_layout_version;

	dev_info(&client->dev, "gcprm layout version (driver): 0x%08x (%d.%d)\n",
						GCPRM_DEVICE_VERSION,
						GCPRM_MAJOR_VERSION,
						GCPRM_MINOR_VERSION);

	dev_info(&client->dev, "gcprm layout version (camera): 0x%08x (%d.%d)\n",
						value,
						(value & 0xffff0000) >> 16,
						(value & 0x0000ffff));

	return (value & 0xffff0000) >> 16 == GCPRM_MAJOR_VERSION ? 1 : 0;
}

static void bcrm_dump(struct i2c_client *client)
{
    return; /* DISABLED. DEBUG ONLY */

	/* Dump all BCRM registers (client, except write only ones) */

	dump_bcrm_reg_32(client, BCRM_VERSION_32R, 				            "BCRM_VERSION_32R");
	dump_bcrm_reg_64(client, BCRM_FEATURE_INQUIRY_64R, 			        "BCRM_FEATURE_INQUIRY_64R");
	dump_bcrm_reg_64(client, BCRM_DEVICE_FIRMWARE_VERSION_64R, 		    "BCRM_DEVICE_FIRMWARE_VERSION_64R");
	dump_bcrm_reg_8(client, BCRM_WRITE_HANDSHAKE_8RW, 			        "BCRM_WRITE_HANDSHAKE_8RW");

	/* Streaming Control Registers */
	dump_bcrm_reg_8(client, BCRM_SUPPORTED_CSI2_LANE_COUNTS_8R, 		"BCRM_SUPPORTED_CSI2_LANE_COUNTS_8R");
	dump_bcrm_reg_8(client, BCRM_CSI2_LANE_COUNT_8RW, 			        "BCRM_CSI2_LANE_COUNT_8RW");
	dump_bcrm_reg_32(client, BCRM_CSI2_CLOCK_MIN_32R, 			        "BCRM_CSI2_CLOCK_MIN_32R");
	dump_bcrm_reg_32(client, BCRM_CSI2_CLOCK_MAX_32R, 			        "BCRM_CSI2_CLOCK_MAX_32R");
	dump_bcrm_reg_32(client, BCRM_CSI2_CLOCK_32RW, 				        "BCRM_CSI2_CLOCK_32RW");
	dump_bcrm_reg_32(client, BCRM_BUFFER_SIZE_32R, 				        "BCRM_BUFFER_SIZE_32R");
	dump_bcrm_reg_32(client, BCRM_PHY_RESET_8RW, 			            "BCRM_PHY_RESET_8RW");

	/* Acquisition Control Registers */
	dump_bcrm_reg_8(client, BCRM_ACQUISITION_START_8RW, 			    "BCRM_ACQUISITION_START_8RW");
	dump_bcrm_reg_8(client, BCRM_ACQUISITION_STOP_8RW, 			        "BCRM_ACQUISITION_STOP_8RW");
	dump_bcrm_reg_8(client, BCRM_ACQUISITION_ABORT_8RW, 			    "BCRM_ACQUISITION_ABORT_8RW");
	dump_bcrm_reg_8(client, BCRM_ACQUISITION_STATUS_8R,			        "BCRM_ACQUISITION_STATUS_8R");
	dump_bcrm_reg_64(client, BCRM_ACQUISITION_FRAME_RATE_64RW, 		    "BCRM_ACQUISITION_FRAME_RATE_64RW");
	dump_bcrm_reg_64(client, BCRM_ACQUISITION_FRAME_RATE_MIN_64R, 		"BCRM_ACQUISITION_FRAME_RATE_MIN_64R");
	dump_bcrm_reg_64(client, BCRM_ACQUISITION_FRAME_RATE_MAX_64R, 		"BCRM_ACQUISITION_FRAME_RATE_MAX_64R");
	dump_bcrm_reg_64(client, BCRM_ACQUISITION_FRAME_RATE_INC_64R, 		"BCRM_ACQUISITION_FRAME_RATE_INC_64R");
	dump_bcrm_reg_8(client, BCRM_ACQUISITION_FRAME_RATE_ENABLE_8RW, 	"BCRM_ACQUISITION_FRAME_RATE_ENABLE_8RW");

	dump_bcrm_reg_8(client, BCRM_FRAME_START_TRIGGER_MODE_8RW, 		    "BCRM_FRAME_START_TRIGGER_MODE_8RW");
	dump_bcrm_reg_8(client, BCRM_FRAME_START_TRIGGER_SOURCE_8RW,		"BCRM_FRAME_START_TRIGGER_SOURCE_8RW");
	dump_bcrm_reg_8(client, BCRM_FRAME_START_TRIGGER_ACTIVATION_8RW,	"BCRM_FRAME_START_TRIGGER_ACTIVATION_8RW");
	
	/* Image Format Control Registers */
	dump_bcrm_reg_32(client, BCRM_IMG_WIDTH_32RW, 				    "BCRM_IMG_WIDTH_32RW");
	dump_bcrm_reg_32(client, BCRM_IMG_WIDTH_MIN_32R, 			    "BCRM_IMG_WIDTH_MIN_32R");
	dump_bcrm_reg_32(client, BCRM_IMG_WIDTH_MAX_32R, 			    "BCRM_IMG_WIDTH_MAX_32R");
	dump_bcrm_reg_32(client, BCRM_IMG_WIDTH_INC_32R, 			    "BCRM_IMG_WIDTH_INC_32R");

	dump_bcrm_reg_32(client, BCRM_IMG_HEIGHT_32RW,				    "BCRM_IMG_HEIGHT_32RW");
	dump_bcrm_reg_32(client, BCRM_IMG_HEIGHT_MIN_32R,			    "BCRM_IMG_HEIGHT_MIN_32R");
	dump_bcrm_reg_32(client, BCRM_IMG_HEIGHT_MAX_32R,			    "BCRM_IMG_HEIGHT_MAX_32R");
	dump_bcrm_reg_32(client, BCRM_IMG_HEIGHT_INC_32R, 			    "BCRM_IMG_HEIGHT_INC_32R");
	dump_bcrm_reg_32(client, BCRM_IMG_OFFSET_X_32RW, 			    "BCRM_IMG_OFFSET_X_32RW");
	dump_bcrm_reg_32(client, BCRM_IMG_OFFSET_X_MIN_32R, 			"BCRM_IMG_OFFSET_X_MIN_32R");
	dump_bcrm_reg_32(client, BCRM_IMG_OFFSET_X_MAX_32R, 			"BCRM_IMG_OFFSET_X_MAX_32R");
	dump_bcrm_reg_32(client, BCRM_IMG_OFFSET_X_INC_32R, 			"BCRM_IMG_OFFSET_X_INC_32R");

	dump_bcrm_reg_32(client, BCRM_IMG_OFFSET_Y_32RW, 			    "BCRM_IMG_OFFSET_Y_32RW");
	dump_bcrm_reg_32(client, BCRM_IMG_OFFSET_Y_MIN_32R, 			"BCRM_IMG_OFFSET_Y_MIN_32R");
	dump_bcrm_reg_32(client, BCRM_IMG_OFFSET_Y_MAX_32R, 			"BCRM_IMG_OFFSET_Y_MAX_32R");
	dump_bcrm_reg_32(client, BCRM_IMG_OFFSET_Y_INC_32R, 			"BCRM_IMG_OFFSET_Y_INC_32R");

	dump_bcrm_reg_32(client, BCRM_IMG_MIPI_DATA_FORMAT_32RW, 		    "BCRM_IMG_MIPI_DATA_FORMAT_32RW");
	dump_bcrm_reg_64(client, BCRM_IMG_AVAILABLE_MIPI_DATA_FORMATS_64R, 	"BCRM_IMG_AVAILABLE_MIPI_DATA_FORMATS_64R");

	dump_bcrm_reg_8(client, BCRM_IMG_BAYER_PATTERN_INQUIRY_8R, 		"BCRM_IMG_BAYER_PATTERN_INQUIRY_8R");
	dump_bcrm_reg_8(client, BCRM_IMG_BAYER_PATTERN_8RW, 			"BCRM_IMG_BAYER_PATTERN_8RW");

	dump_bcrm_reg_8(client, BCRM_IMG_REVERSE_X_8RW, 			    "BCRM_IMG_REVERSE_X_8RW");
	dump_bcrm_reg_8(client, BCRM_IMG_REVERSE_Y_8RW, 			    "BCRM_IMG_REVERSE_Y_8RW");

	dump_bcrm_reg_32(client, BCRM_SENSOR_WIDTH_32R, 			    "BCRM_SENSOR_WIDTH_32R");
	dump_bcrm_reg_32(client, BCRM_SENSOR_HEIGHT_32R, 			    "BCRM_SENSOR_HEIGHT_32R");

	dump_bcrm_reg_32(client, BCRM_WIDTH_MAX_32R, 				    "BCRM_WIDTH_MAX_32R");
	dump_bcrm_reg_32(client, BCRM_HEIGHT_MAX_32R, 				    "BCRM_HEIGHT_MAX_32R");

	/* Brightness Control Registers */
	dump_bcrm_reg_64(client, BCRM_EXPOSURE_TIME_64RW, 			    "BCRM_EXPOSURE_TIME_64RW");
	dump_bcrm_reg_64(client, BCRM_EXPOSURE_TIME_MIN_64R, 			"BCRM_EXPOSURE_TIME_MIN_64R");
	dump_bcrm_reg_64(client, BCRM_EXPOSURE_TIME_MAX_64R, 			"BCRM_EXPOSURE_TIME_MAX_64R");
	dump_bcrm_reg_64(client, BCRM_EXPOSURE_TIME_INC_64R, 			"BCRM_EXPOSURE_TIME_INC_64R");
	dump_bcrm_reg_8(client, BCRM_EXPOSURE_AUTO_8RW, 			    "BCRM_EXPOSURE_AUTO_8RW");

	dump_bcrm_reg_8(client, BCRM_INTENSITY_AUTO_PRECEDENCE_8RW, 		"BCRM_INTENSITY_AUTO_PRECEDENCE_8RW");
	dump_bcrm_reg_32(client, BCRM_INTENSITY_AUTO_PRECEDENCE_VALUE_32RW, "BCRM_INTENSITY_AUTO_PRECEDENCE_VALUE_32RW");
	dump_bcrm_reg_32(client, BCRM_INTENSITY_AUTO_PRECEDENCE_MIN_32R, 	"BCRM_INTENSITY_AUTO_PRECEDENCE_MIN_32R");
	dump_bcrm_reg_32(client, BCRM_INTENSITY_AUTO_PRECEDENCE_MAX_32R, 	"BCRM_INTENSITY_AUTO_PRECEDENCE_MAX_32R");
	dump_bcrm_reg_32(client, BCRM_INTENSITY_AUTO_PRECEDENCE_INC_32R, 	"BCRM_INTENSITY_AUTO_PRECEDENCE_INC_32R");

	dump_bcrm_reg_32(client, BCRM_BLACK_LEVEL_32RW, 			    "BCRM_BLACK_LEVEL_32RW");
	dump_bcrm_reg_32(client, BCRM_BLACK_LEVEL_MIN_32R, 			    "BCRM_BLACK_LEVEL_MIN_32R");
	dump_bcrm_reg_32(client, BCRM_BLACK_LEVEL_MAX_32R, 			    "BCRM_BLACK_LEVEL_MAX_32R");
	dump_bcrm_reg_32(client, BCRM_BLACK_LEVEL_INC_32R, 			    "BCRM_BLACK_LEVEL_INC_32R");

	dump_bcrm_reg_64(client, BCRM_GAIN_64RW, 				        "BCRM_GAIN_64RW");
	dump_bcrm_reg_64(client, BCRM_GAIN_MIN_64R,			 	        "BCRM_GAIN_MIN_64R");
	dump_bcrm_reg_64(client, BCRM_GAIN_MAX_64R, 				    "BCRM_GAIN_MAX_64R");
	dump_bcrm_reg_64(client, BCRM_GAIN_INC_64R, 				    "BCRM_GAIN_INC_64R");
	dump_bcrm_reg_8(client, BCRM_GAIN_AUTO_8RW, 				    "BCRM_GAIN_AUTO_8RW");

	dump_bcrm_reg_64(client, BCRM_GAMMA_64RW, 				        "BCRM_GAMMA_64RW");
	dump_bcrm_reg_64(client, BCRM_GAMMA_MIN_64R, 				    "BCRM_GAMMA_MIN_64R");
	dump_bcrm_reg_64(client, BCRM_GAMMA_MAX_64R, 				    "BCRM_GAMMA_MAX_64R");
	dump_bcrm_reg_64(client, BCRM_GAMMA_INC_64R, 				    "BCRM_GAMMA_INC_64R");

	dump_bcrm_reg_32(client, BCRM_CONTRAST_VALUE_32RW, 			    "BCRM_CONTRAST_VALUE_32RW");
	dump_bcrm_reg_32(client, BCRM_CONTRAST_VALUE_MIN_32R, 			"BCRM_CONTRAST_VALUE_MIN_32R");
	dump_bcrm_reg_32(client, BCRM_CONTRAST_VALUE_MAX_32R, 			"BCRM_CONTRAST_VALUE_MAX_32R");
	dump_bcrm_reg_32(client, BCRM_CONTRAST_VALUE_INC_32R, 			"BCRM_CONTRAST_VALUE_INC_32R");

	/* Color Management Registers */
	dump_bcrm_reg_32(client, BCRM_SATURATION_32RW,				    "BCRM_SATURATION_32RW");
	dump_bcrm_reg_32(client, BCRM_SATURATION_MIN_32R,			    "BCRM_SATURATION_MIN_32R");
	dump_bcrm_reg_32(client, BCRM_SATURATION_MAX_32R, 			    "BCRM_SATURATION_MAX_32R");
	dump_bcrm_reg_32(client, BCRM_SATURATION_INC_32R, 			    "BCRM_SATURATION_INC_32R");

	dump_bcrm_reg_32(client, BCRM_HUE_32RW,				 	        "BCRM_HUE_32RW");
	dump_bcrm_reg_32(client, BCRM_HUE_MIN_32R, 				        "BCRM_HUE_MIN_32R");
	dump_bcrm_reg_32(client, BCRM_HUE_MAX_32R,				        "BCRM_HUE_MAX_32R");
	dump_bcrm_reg_32(client, BCRM_HUE_INC_32R, 				        "BCRM_HUE_INC_32R");

	dump_bcrm_reg_64(client, BCRM_ALL_BALANCE_RATIO_64RW,			"BCRM_ALL_BALANCE_RATIO_64RW");
	dump_bcrm_reg_64(client, BCRM_ALL_BALANCE_RATIO_MIN_64R, 		"BCRM_ALL_BALANCE_RATIO_MIN_64R");
	dump_bcrm_reg_64(client, BCRM_ALL_BALANCE_RATIO_MAX_64R, 		"BCRM_ALL_BALANCE_RATIO_MAX_64R");

	dump_bcrm_reg_64(client, BCRM_RED_BALANCE_RATIO_64RW, 			"BCRM_RED_BALANCE_RATIO_64RW");
	dump_bcrm_reg_64(client, BCRM_RED_BALANCE_RATIO_MIN_64R, 		"BCRM_RED_BALANCE_RATIO_MIN_64R");
	dump_bcrm_reg_64(client, BCRM_RED_BALANCE_RATIO_MAX_64R, 		"BCRM_RED_BALANCE_RATIO_MAX_64R");
	dump_bcrm_reg_64(client, BCRM_RED_BALANCE_RATIO_INC_64R, 		"BCRM_RED_BALANCE_RATIO_INC_64R");

	dump_bcrm_reg_64(client, BCRM_GREEN_BALANCE_RATIO_64RW,			"BCRM_GREEN_BALANCE_RATIO_64RW");
	dump_bcrm_reg_64(client, BCRM_GREEN_BALANCE_RATIO_MIN_64R,		"BCRM_GREEN_BALANCE_RATIO_MIN_64R");
	dump_bcrm_reg_64(client, BCRM_GREEN_BALANCE_RATIO_MAX_64R, 		"BCRM_GREEN_BALANCE_RATIO_MAX_64R");
	dump_bcrm_reg_64(client, BCRM_GREEN_BALANCE_RATIO_INC_64R,		"BCRM_GREEN_BALANCE_RATIO_INC_64R");

	dump_bcrm_reg_64(client, BCRM_BLUE_BALANCE_RATIO_64RW, 			"BCRM_BLUE_BALANCE_RATIO_64RW");
	dump_bcrm_reg_64(client, BCRM_BLUE_BALANCE_RATIO_MIN_64R, 		"BCRM_BLUE_BALANCE_RATIO_MIN_64R");
	dump_bcrm_reg_64(client, BCRM_BLUE_BALANCE_RATIO_MAX_64R, 		"BCRM_BLUE_BALANCE_RATIO_MAX_64R");
	dump_bcrm_reg_64(client, BCRM_BLUE_BALANCE_RATIO_INC_64R, 		"BCRM_BLUE_BALANCE_RATIO_INC_64R");

	dump_bcrm_reg_8(client, BCRM_WHITE_BALANCE_AUTO_8RW, 			"BCRM_WHITE_BALANCE_AUTO_8RW");

	/* Other Registers */
	dump_bcrm_reg_32(client, BCRM_SHARPNESS_32RW, 				    "BCRM_SHARPNESS_32RW");
	dump_bcrm_reg_32(client, BCRM_SHARPNESS_MIN_32R, 			    "BCRM_SHARPNESS_MIN_32R");
	dump_bcrm_reg_32(client, BCRM_SHARPNESS_MAX_32R, 			    "BCRM_SHARPNESS_MAX_32R");
	dump_bcrm_reg_32(client, BCRM_SHARPNESS_INC_32R, 			    "BCRM_SHARPNESS_INC_32R");

	dump_bcrm_reg_32(client, BCRM_DEVICE_TEMPERATURE_32R, 			"BCRM_DEVICE_TEMPERATURE_32R");
}

static void dump_bcrm_reg_8(struct i2c_client *client, u16 nOffset, const char *pRegName)
{
    struct avt_csi2_priv *priv;
	int status = 0;
	u8 data = 0;

	priv = devm_kzalloc(&client->dev, sizeof(struct avt_csi2_priv),	GFP_KERNEL);
	if (!priv)
		return;

	status = i2c_read(client, priv->cci_reg.bcrm_addr + nOffset, AV_CAM_REG_SIZE,
			AV_CAM_DATA_SIZE_8, (char *)&data);

	if (status >= 0)
                dev_info(&client->dev, "%s: %u (0x%x)", pRegName, data, data);
	else
		dev_err(&client->dev, "%s: ERROR", pRegName);
}

static void dump_bcrm_reg_32(struct i2c_client *client, u16 nOffset, const char *pRegName)
{
    struct avt_csi2_priv *priv;
	int status = 0;
	u32 data = 0;

	priv = devm_kzalloc(&client->dev, sizeof(struct avt_csi2_priv),	GFP_KERNEL);
	if (!priv)
		return;

	status = i2c_read(client, priv->cci_reg.bcrm_addr + nOffset, AV_CAM_REG_SIZE,
			  AV_CAM_DATA_SIZE_32, (char *)&data);

	swapbytes(&data, sizeof(data));
	if (status >= 0)
                dev_info(&client->dev, "%s: %u (0x%08x)", pRegName, data, data);
	else
		dev_err(&client->dev, "%s: ERROR", pRegName);
}

static void dump_bcrm_reg_64(struct i2c_client *client, u16 nOffset, const char *pRegName)
{
    struct avt_csi2_priv *priv;
	int status = 0;
	u64 data = 0;
   
	priv = devm_kzalloc(&client->dev, sizeof(struct avt_csi2_priv),	GFP_KERNEL);
	if (!priv)
		return;

	status = i2c_read(client, priv->cci_reg.bcrm_addr + nOffset, AV_CAM_REG_SIZE,
			  AV_CAM_DATA_SIZE_64, (char *)&data);

 	swapbytes(&data, sizeof(data));
	if (status >= 0)
                dev_info(&client->dev, "%s: %llu (0x%016llx)", pRegName, data, data);
	else
		dev_err(&client->dev, "%s: ERROR", pRegName);               
}

/* Check if the device is answering to an I2C read request */
static bool device_present(struct i2c_client *client)
{
    int status = 0;
    u64 data = 0;

    status = i2c_read(client, CCI_DEVICE_CAP_64R, AV_CAM_REG_SIZE, AV_CAM_DATA_SIZE_64,  (char *)&data);         

    return ((status < 0) || (data == 0)) ? false : true;
}

static int soft_reset(struct i2c_client *client)
{
    int status = 0;
    uint8_t reset_val = 1;
    static const uint8_t default_heartbeat_val = 0x80;
    uint8_t heartbeat_val = default_heartbeat_val;
    uint64_t duration_ms = 0;
    static const uint8_t heartbeat_low_limit = 0;
    static const uint32_t delay_ms = 400;
    static const uint32_t max_time_ms = 10000;
    uint64_t start_jiffies = get_jiffies_64();
    bool device_available = false;
    bool heartbeat_available = false;

    /* Check, if heartbeat register is available (write default value and read it back)*/
    status = i2c_write(client, CCI_HEARTBEAT_8RW, AV_CAM_REG_SIZE, sizeof(heartbeat_val), (char*)&heartbeat_val);
    heartbeat_available = (i2c_read(client, CCI_HEARTBEAT_8RW, AV_CAM_REG_SIZE, sizeof(heartbeat_val), (char*)&heartbeat_val) < 0) ? false : true;
    /* If camera does not support heartbeat it delivers always 0 */
    heartbeat_available = ((heartbeat_val != 0) && (status != 0)) ? true : false;
    dev_info(&client->dev, "Heartbeat %ssupported", (heartbeat_available) ? "" : "NOT ");

    /* Execute soft reset */
    status = i2c_write(client, CCI_SOFT_RESET_8W, AV_CAM_REG_SIZE, sizeof(reset_val), (char*)&reset_val);
        
	if (status >= 0)
    {
        dev_info(&client->dev, "Soft reset executed. Initializing camera...");
    }
	else
    {
        dev_err(&client->dev, "Soft reset ERROR");   
        return -EIO; 
    }

    /* Poll camera register to check if camera is back again */
    do
    {
        usleep_range(delay_ms*1000, (delay_ms*1000)+1);
        device_available = device_present(client);
        duration_ms = jiffies_to_msecs(get_jiffies_64() - start_jiffies);       
    } while((duration_ms < max_time_ms) && !device_available);

    if (!heartbeat_available)
    {
        /* Camera might need a few more seconds to be fully booted */
        usleep_range(add_wait_time_ms*1000, (add_wait_time_ms*1000)+1);
    }
    else
    {
        /* Heartbeat is supported. Poll heartbeat register until value is lower than the default value again */
        do
        {
            usleep_range(delay_ms*1000, (delay_ms*1000)+1);
            status = i2c_read(client, CCI_HEARTBEAT_8RW, AV_CAM_REG_SIZE, AV_CAM_DATA_SIZE_8, (char*)&heartbeat_val);
            //dev_info(&client->dev, "Heartbeat val=0x%02X", heartbeat_val);
            duration_ms = jiffies_to_msecs(get_jiffies_64() - start_jiffies);       
            if ((heartbeat_val > heartbeat_low_limit) && (heartbeat_val < default_heartbeat_val) && (status >= 0))
            {
                /* Heartbeat active -> Camera alive */
                dev_info(&client->dev, "Heartbeat active!");
                break;
            }
        } while (duration_ms < max_time_ms);
    }

    dev_info(&client->dev, "Camera boot time: %llums", duration_ms);
    if (!device_available)
            dev_err(&client->dev, "Camera not reconnected");   

    return 0;
}

static ssize_t cci_register_layout_version_show(struct device *dev,
			struct device_attribute *attr, char *buf)
{
	struct camera_common_data *s_data = to_camera_common_data(dev);
	struct avt_csi2_priv *priv = (struct avt_csi2_priv *)s_data->priv;

	return sprintf(buf, "%d\n", priv->cci_reg.layout_version);
}

static ssize_t csi_clock_show(struct device *dev,
			struct device_attribute *attr, char *buf)
{
	struct camera_common_data *s_data = to_camera_common_data(dev);
	struct avt_csi2_priv *priv = (struct avt_csi2_priv *)s_data->priv;

	return sprintf(buf, "%d\n", priv->csi_clk_freq);
}

static ssize_t device_capabilities_show(struct device *dev,
			struct device_attribute *attr, char *buf)
{
	struct camera_common_data *s_data = to_camera_common_data(dev);
	struct avt_csi2_priv *priv = (struct avt_csi2_priv *)s_data->priv;

	return sprintf(buf, "%llu\n", priv->cci_reg.device_capabilities);
}

static ssize_t device_guid_show(struct device *dev,
			struct device_attribute *attr, char *buf)
{
	struct camera_common_data *s_data = to_camera_common_data(dev);
	struct avt_csi2_priv *priv = (struct avt_csi2_priv *)s_data->priv;

	return sprintf(buf, "%s\n", priv->cci_reg.device_guid);
}

static ssize_t manufacturer_name_show(struct device *dev,
			struct device_attribute *attr, char *buf)
{
	struct camera_common_data *s_data = to_camera_common_data(dev);
	struct avt_csi2_priv *priv = (struct avt_csi2_priv *)s_data->priv;

	return sprintf(buf, "%s\n", priv->cci_reg.manufacturer_name);
}

static ssize_t model_name_show(struct device *dev,
			struct device_attribute *attr, char *buf)
{
	struct camera_common_data *s_data = to_camera_common_data(dev);
	struct avt_csi2_priv *priv = (struct avt_csi2_priv *)s_data->priv;

	return sprintf(buf, "%s\n", priv->cci_reg.model_name);
}

static ssize_t family_name_show(struct device *dev,
			struct device_attribute *attr, char *buf)
{
	struct camera_common_data *s_data = to_camera_common_data(dev);
	struct avt_csi2_priv *priv = (struct avt_csi2_priv *)s_data->priv;

	return sprintf(buf, "%s\n", priv->cci_reg.family_name);
}

static ssize_t lane_count_show(struct device *dev,
			struct device_attribute *attr, char *buf)
{
	struct camera_common_data *s_data = to_camera_common_data(dev);
	struct avt_csi2_priv *priv = (struct avt_csi2_priv *)s_data->priv;

	return sprintf(buf, "%d\n", priv->s_data->numlanes);
}

static ssize_t device_version_show(struct device *dev,
			struct device_attribute *attr, char *buf)
{
	struct camera_common_data *s_data = to_camera_common_data(dev);
	struct avt_csi2_priv *priv = (struct avt_csi2_priv *)s_data->priv;

	return sprintf(buf, "%s\n", priv->cci_reg.device_version);
}

static ssize_t manufacturer_info_show(struct device *dev,
			struct device_attribute *attr, char *buf)
{
	struct camera_common_data *s_data = to_camera_common_data(dev);
	struct avt_csi2_priv *priv = (struct avt_csi2_priv *)s_data->priv;

	return sprintf(buf, "%s\n", priv->cci_reg.manufacturer_info);
}

static ssize_t serial_number_show(struct device *dev,
			struct device_attribute *attr, char *buf)
{
	struct camera_common_data *s_data = to_camera_common_data(dev);
	struct avt_csi2_priv *priv = (struct avt_csi2_priv *)s_data->priv;

	return sprintf(buf, "%s\n", priv->cci_reg.serial_number);
}

static ssize_t user_defined_name_show(struct device *dev,
			struct device_attribute *attr, char *buf)
{
	struct camera_common_data *s_data = to_camera_common_data(dev);
	struct avt_csi2_priv *priv = (struct avt_csi2_priv *)s_data->priv;

	return sprintf(buf, "%s\n", priv->cci_reg.user_defined_name);
}

static ssize_t driver_version_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	return sprintf(buf, "%d.%d.%d.%d\n",
			DRV_VER_MAJOR, DRV_VER_MINOR, DRV_VER_PATCH, DRV_VER_BUILD);
}

static ssize_t debug_en_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	return sprintf(buf, "%d\n", debug);
}

static ssize_t debug_en_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	int ret;

	ret = kstrtoint(buf, 10, &debug);
	if (ret < 0)
		return ret;

	return count;
}

static DEVICE_ATTR_RO(cci_register_layout_version);
static DEVICE_ATTR_RO(csi_clock);
static DEVICE_ATTR_RO(device_capabilities);
static DEVICE_ATTR_RO(device_guid);
static DEVICE_ATTR_RO(device_version);
static DEVICE_ATTR_RO(driver_version);
static DEVICE_ATTR_RO(family_name);
static DEVICE_ATTR_RO(lane_count);
static DEVICE_ATTR_RO(manufacturer_info);
static DEVICE_ATTR_RO(manufacturer_name);
static DEVICE_ATTR_RO(model_name);
static DEVICE_ATTR_RO(serial_number);
static DEVICE_ATTR_RO(user_defined_name);
static DEVICE_ATTR_RW(debug_en);

static struct attribute *avt_csi2_attrs[] = {
	&dev_attr_cci_register_layout_version.attr,
	&dev_attr_csi_clock.attr,
	&dev_attr_device_capabilities.attr,
	&dev_attr_device_guid.attr,
	&dev_attr_device_version.attr,
	&dev_attr_driver_version.attr,
	&dev_attr_family_name.attr,
	&dev_attr_lane_count.attr,
	&dev_attr_manufacturer_info.attr,
	&dev_attr_manufacturer_name.attr,
	&dev_attr_model_name.attr,
	&dev_attr_serial_number.attr,
	&dev_attr_user_defined_name.attr,
	&dev_attr_debug_en.attr,
	NULL
};

static struct attribute_group avt_csi2_attr_grp = {
	.attrs = avt_csi2_attrs,
};

static bool common_range(uint32_t nMin1, uint32_t nMax1, uint32_t nInc1,
				uint32_t nMin2, uint32_t nMax2, uint32_t nInc2,
				uint32_t *rMin, uint32_t *rMax, uint32_t *rInc)
{
	bool bResult = false;

	uint32_t nMin = max(nMin1, nMin2);
	uint32_t nMax = min(nMax1, nMax2);

	/* Check if it is overlapping at all */
	if (nMax >= nMin) {
		/* if both minima are equal,
		 * then the computation is a bit simpler
		 */
		if (nMin1 == nMin2) {
			uint32_t nLCM = lcm(nInc1, nInc2);
			*rMin = nMin;
			*rMax = nMax - ((nMax - nMin) % nLCM);

			if (*rMin == *rMax)
				*rInc = 1;
			else
				*rInc = nLCM;

			bResult = true;
		} else if (nMin1 > nMin2) {
			/* Find the first value that is ok for Host and BCRM */
			uint32_t nMin1Shifted = nMin1 - nMin2;
			uint32_t nMaxShifted = nMax - nMin2;
			uint32_t nValue = nMin1Shifted;

			for (; nValue <= nMaxShifted; nValue += nInc1) {
				if ((nValue % nInc2) == 0)
					break;
			}

			/* Compute common increment and maximum */
			if (nValue <= nMaxShifted) {
				uint32_t nLCM = lcm(nInc1, nInc2);
				*rMin = nValue + nMin2;
				*rMax = nMax - ((nMax - *rMin) % nLCM);

				if (*rMin == *rMax)
					*rInc = 1;
				else
					*rInc = nLCM;

				bResult = true;
			}
		} else {
			/* Find the first value that is ok for Host and BCRM */
			uint32_t nMin2Shifted = nMin2 - nMin1;
			uint32_t nMaxShifted = nMax - nMin1;
			uint32_t nValue = nMin2Shifted;

			for (; nValue <= nMaxShifted; nValue += nInc2) {
				if ((nValue % nInc1) == 0)
					break;
			}

			/* Compute common increment and maximum */
			if (nValue <= nMaxShifted) {
				uint32_t nLCM = lcm(nInc2, nInc1);
				*rMin = nValue + nMin1;
				*rMax = nMax - ((nMax - *rMin) % nLCM);
				if (*rMin == *rMax)
					*rInc = 1;
				else
					*rInc = nLCM;

				bResult = true;
			}
		}
	}

	return bResult;
}

static void dump_frame_param(struct v4l2_subdev *sd)
{
    struct avt_csi2_priv *priv = avt_get_priv(sd);
    avt_dbg(sd, "\n");
    avt_dbg(sd, "priv->frmp.minh=%d\n", priv->frmp.minh);
    avt_dbg(sd, "priv->frmp.maxh=%d\n", priv->frmp.maxh);
    avt_dbg(sd, "priv->frmp.sh=%d\n", priv->frmp.sh);
    avt_dbg(sd, "priv->frmp.minw=%d\n", priv->frmp.minw);
    avt_dbg(sd, "priv->frmp.maxw=%d\n", priv->frmp.maxw);
    avt_dbg(sd, "priv->frmp.sw=%d\n", priv->frmp.sw);
    avt_dbg(sd, "priv->frmp.minhoff=%d\n", priv->frmp.minhoff);
    avt_dbg(sd, "priv->frmp.maxhoff=%d\n", priv->frmp.maxhoff);
    avt_dbg(sd, "priv->frmp.shoff=%d\n", priv->frmp.shoff);
    avt_dbg(sd, "priv->frmp.minwoff=%d\n", priv->frmp.minwoff);
    avt_dbg(sd, "priv->frmp.maxwoff=%d\n", priv->frmp.maxwoff);
    avt_dbg(sd, "priv->frmp.swoff=%d\n", priv->frmp.swoff);
    avt_dbg(sd, "priv->frmp.r.width=%d\n", priv->frmp.r.width);
    avt_dbg(sd, "priv->frmp.r.height=%d\n", priv->frmp.r.height);
    avt_dbg(sd, "priv->frmp.r.left=%d\n", priv->frmp.r.left);
    avt_dbg(sd, "priv->frmp.r.top=%d\n", priv->frmp.r.top);
}

static int avt_init_frame_param(struct v4l2_subdev *sd)
{
    struct avt_csi2_priv *priv = avt_get_priv(sd);
    dump_frame_param(sd);
	if (avt_get_param(priv->client, V4L2_AV_CSI2_HEIGHT_MINVAL_R,
				&priv->frmp.minh))
		return -EINVAL;

	if (avt_get_param(priv->client, V4L2_AV_CSI2_HEIGHT_MAXVAL_R,
				&priv->frmp.maxh))
		return -EINVAL;

	if (avt_get_param(priv->client, V4L2_AV_CSI2_HEIGHT_INCVAL_R,
				&priv->frmp.sh))
		return -EINVAL;

	if (avt_get_param(priv->client, V4L2_AV_CSI2_WIDTH_MINVAL_R,
				&priv->frmp.minw))
		return -EINVAL;

	if (avt_get_param(priv->client, V4L2_AV_CSI2_WIDTH_MAXVAL_R,
				&priv->frmp.maxw))
		return -EINVAL;

	if (avt_get_param(priv->client, V4L2_AV_CSI2_WIDTH_INCVAL_R,
				&priv->frmp.sw))
		return -EINVAL;

	if (avt_get_param(priv->client, V4L2_AV_CSI2_OFFSET_Y_MIN_R,
				&priv->frmp.minhoff))
		return -EINVAL;

	if (avt_get_param(priv->client, V4L2_AV_CSI2_OFFSET_Y_MAX_R,
				&priv->frmp.maxhoff))
		return -EINVAL;

	if (avt_get_param(priv->client, V4L2_AV_CSI2_OFFSET_Y_INC_R,
				&priv->frmp.shoff))
		return -EINVAL;

	if (avt_get_param(priv->client, V4L2_AV_CSI2_OFFSET_X_MIN_R,
				&priv->frmp.minwoff))
		return -EINVAL;

	if (avt_get_param(priv->client, V4L2_AV_CSI2_OFFSET_X_MAX_R,
				&priv->frmp.maxwoff))
		return -EINVAL;

	if (avt_get_param(priv->client, V4L2_AV_CSI2_OFFSET_X_INC_R,
				&priv->frmp.swoff))
		return -EINVAL;

	if (avt_get_param(priv->client, V4L2_AV_CSI2_WIDTH_R,
				&priv->frmp.r.width))
		return -EINVAL;

	if (avt_get_param(priv->client, V4L2_AV_CSI2_HEIGHT_R,
				&priv->frmp.r.height))
		return -EINVAL;

	if (avt_get_param(priv->client, V4L2_AV_CSI2_OFFSET_X_R,
				&priv->frmp.r.left))
		return -EINVAL;

	if (avt_get_param(priv->client, V4L2_AV_CSI2_OFFSET_Y_R,
				&priv->frmp.r.top))
		return -EINVAL;

    /* We might need to correct some values */
    /* Tegra doesn't seem to accept offsets that are not divisible by 8. */
    roundup(priv->frmp.swoff, OFFSET_INC_W);
    roundup(priv->frmp.shoff, OFFSET_INC_H);
    /* Tegra doesn't allow image resolutions smaller than 64x32 */
    priv->frmp.minw = max_t(uint32_t, priv->frmp.minw, FRAMESIZE_MIN_W);
    priv->frmp.minh = max_t(uint32_t, priv->frmp.minh, FRAMESIZE_MIN_H);
   
    priv->frmp.maxw = min_t(uint32_t, priv->frmp.maxw, FRAMESIZE_MAX_W);
    priv->frmp.maxh = min_t(uint32_t, priv->frmp.maxh, FRAMESIZE_MAX_H);
  
   //max_height = rounddown(max_height, FRAMESIZE_INC_H);

    /* Take care of image width alignment*/
    if (priv->crop_align_enabled) {
        priv->frmp.maxwoff = avt_align_width(sd, priv->frmp.maxwoff);

        priv->frmp.maxw = avt_align_width(sd, priv->frmp.maxw);
    }

    dump_frame_param(sd);
	return 0;
}

/* Read image format from camera,
 * should be only called once, during initialization
 * */
static int avt_read_fmt_from_device(struct v4l2_subdev *sd, uint32_t *fmt)
{
	struct avt_csi2_priv *priv = avt_get_priv(sd);
	struct i2c_client *client = priv->client;
	uint32_t avt_img_fmt = 0;
	uint8_t bayer_pattern;
	int ret = 0;

	ret = avt_reg_read(client,
			priv->cci_reg.bcrm_addr + BCRM_IMG_BAYER_PATTERN_8RW,
			AV_CAM_REG_SIZE, AV_CAM_DATA_SIZE_8,
			(char *) &bayer_pattern);

	if (ret < 0) {
		dev_err(&client->dev, "i2c read failed (%d)\n", ret);
		return ret;
	}
    dev_dbg(&client->dev, "Camera bayer_pattern=0x%X", bayer_pattern);

	ret = avt_reg_read(client,
			priv->cci_reg.bcrm_addr + BCRM_IMG_MIPI_DATA_FORMAT_32RW,
			AV_CAM_REG_SIZE, AV_CAM_DATA_SIZE_32,
			(char *) &avt_img_fmt);

	if (ret < 0) {
		dev_err(&client->dev, "i2c read failed (%d)\n", ret);
		return ret;
	}

    dev_dbg(&client->dev, "BCRM_IMG_MIPI_DATA_FORMAT_32RW=0x%08X\n", avt_img_fmt);

	switch (avt_img_fmt) {
		case	MIPI_DT_RGB888:
			avt_img_fmt = MEDIA_BUS_FMT_RGB888_1X24;
			break;
		case	MIPI_DT_RGB565:
			avt_img_fmt = MEDIA_BUS_FMT_RGB565_1X16;
			break;
		case	MIPI_DT_YUV422:
			avt_img_fmt = MEDIA_BUS_FMT_VYUY8_2X8;
			break;
		case	MIPI_DT_CUSTOM:
			avt_img_fmt = MEDIA_BUS_FMT_CUSTOM;
			break;
		case	MIPI_DT_RAW8:
				switch (bayer_pattern) {
				case	monochrome:
					avt_img_fmt =
						MEDIA_BUS_FMT_Y8_1X8;
					break;
				case	bayer_gr:
					avt_img_fmt =
						MEDIA_BUS_FMT_SGRBG8_1X8;
					break;
				case	bayer_rg:
					avt_img_fmt =
						MEDIA_BUS_FMT_SRGGB8_1X8;
					break;
				case	bayer_gb:
					avt_img_fmt =
						MEDIA_BUS_FMT_SGBRG8_1X8;
					break;
				case	bayer_bg:
					avt_img_fmt =
						MEDIA_BUS_FMT_SBGGR8_1X8;
					break;
				default:
					dev_err(&client->dev, "%s:Unknown RAW8 pixelformat read, bayer_pattern %d\n",
							__func__,
							bayer_pattern);
					return -EINVAL;
				}
			break;
		case	MIPI_DT_RAW10:
				switch (bayer_pattern) {
				case	monochrome:
					avt_img_fmt =
						MEDIA_BUS_FMT_Y10_1X10;
					break;
				case	bayer_gr:
					avt_img_fmt =
						MEDIA_BUS_FMT_SGRBG10_1X10;
					break;
				case	bayer_rg:
					avt_img_fmt =
						MEDIA_BUS_FMT_SRGGB10_1X10;
					break;
				case	bayer_gb:
					avt_img_fmt =
						MEDIA_BUS_FMT_SGBRG10_1X10;
					break;
				case	bayer_bg:
					avt_img_fmt =
						MEDIA_BUS_FMT_SBGGR10_1X10;
					break;
				default:
					dev_err(&client->dev, "%s:Unknown RAW10 pixelformat read, bayer_pattern %d\n",
							__func__,
							bayer_pattern);
					return -EINVAL;
				}
			break;
		case	MIPI_DT_RAW12:
				switch (bayer_pattern) {
				case	monochrome:
					avt_img_fmt =
						MEDIA_BUS_FMT_Y12_1X12;
					break;
				case	bayer_gr:
					avt_img_fmt =
						MEDIA_BUS_FMT_SGRBG12_1X12;
					break;
				case	bayer_rg:
					avt_img_fmt =
						MEDIA_BUS_FMT_SRGGB12_1X12;
					break;
				case	bayer_gb:
					avt_img_fmt =
						MEDIA_BUS_FMT_SGBRG12_1X12;
					break;
				case	bayer_bg:
					avt_img_fmt =
						MEDIA_BUS_FMT_SBGGR12_1X12;
					break;
				default:
					dev_err(&client->dev, "%s:Unknown RAW12 pixelformat read, bayer_pattern %d\n",
							__func__,
							bayer_pattern);
					return -EINVAL;
				}
			break;

                case 0:
                        /* Pixelformat 0 -> Probably fallback app running -> Emulate RAW888 */
                        avt_img_fmt = MEDIA_BUS_FMT_RGB888_1X24;
                        dev_warn(&client->dev, "Invalid pixelformat detected (0). Fallback app running?");
                        break;

		default:
			dev_err(&client->dev, "%s:Unknown pixelformat read, avt_img_fmt 0x%x\n",
					__func__, avt_img_fmt);
			return -EINVAL;
		}

	*fmt = avt_img_fmt;

	return 0;
}

static int avt_init_mode(struct v4l2_subdev *sd)
{
	struct avt_csi2_priv *priv = avt_get_priv(sd);
	int ret = 0;
	uint32_t common_min_clk = 0;
	uint32_t common_max_clk = 0;
	uint32_t common_inc_clk = 0;
	uint32_t avt_min_clk = 0;
	uint32_t avt_max_clk = 0;
	uint8_t avt_supported_lane_counts = 0;

	uint32_t i2c_reg;
	uint32_t i2c_reg_size;
	uint32_t i2c_reg_count;
	uint32_t clk;
	uint8_t bcm_mode = 0;

	char *i2c_reg_buf;
	struct v4l2_subdev_selection sel;

	/* Check if requested number of lanes is supported */
	ret = avt_reg_read(priv->client,
			priv->cci_reg.bcrm_addr + BCRM_SUPPORTED_CSI2_LANE_COUNTS_8R,
			AV_CAM_REG_SIZE, AV_CAM_DATA_SIZE_8,
			(char *) &avt_supported_lane_counts);
	if (ret < 0) {
		avt_err(sd, "i2c read failed (%d)\n", ret);
		return ret;
	}

    avt_info(sd, "Camera supported lane counts value: 0x%x\n", avt_supported_lane_counts);

	if(!(test_bit(priv->s_data->numlanes - 1, (const long *)(&avt_supported_lane_counts)))) {
		avt_err(sd, "requested number of lanes (%u) not supported by this camera!\n",
				priv->s_data->numlanes);
		return -EINVAL;
	}

	/* Set number of lanes */
	ret = avt_reg_write(priv->client,
			priv->cci_reg.bcrm_addr + BCRM_CSI2_LANE_COUNT_8RW,
			priv->s_data->numlanes);
	if (ret < 0) {
		avt_err(sd, "i2c write failed (%d)\n", ret);
		return ret;
	}

	ret = avt_reg_read(priv->client,
			priv->cci_reg.bcrm_addr + BCRM_CSI2_CLOCK_MIN_32R,
			AV_CAM_REG_SIZE, AV_CAM_DATA_SIZE_32,
			(char *) &avt_min_clk);

	if (ret < 0) {
		avt_err(sd, "i2c read failed (%d)\n", ret);
		return ret;
	}

	ret = avt_reg_read(priv->client,
			priv->cci_reg.bcrm_addr + BCRM_CSI2_CLOCK_MAX_32R,
			AV_CAM_REG_SIZE, AV_CAM_DATA_SIZE_32,
			(char *) &avt_max_clk);

	if (ret < 0) {
		avt_err(sd, "i2c read failed (%d)\n", ret);
		return ret;
	}

	avt_dbg(sd, "csi clock camera range: %d:%d Hz, host range: %d:%d Hz\n",
		avt_min_clk, avt_max_clk,
		CSI_HOST_CLK_MIN_FREQ, CSI_HOST_CLK_MAX_FREQ);

	if (common_range(avt_min_clk, avt_max_clk, 1,
			CSI_HOST_CLK_MIN_FREQ, CSI_HOST_CLK_MAX_FREQ, 1,
			&common_min_clk, &common_max_clk, &common_inc_clk)
			== false) {
		avt_err(sd, "no common clock range for camera and host possible!\n");
		return -EINVAL;
	}

	avt_dbg(sd, "camera/host common csi clock range: %d:%d Hz\n",
			common_min_clk, common_max_clk);

	if (priv->csi_clk_freq == 0) {
		avt_dbg(sd, "no csi clock requested, using common max (%d Hz)\n",
				common_max_clk);
		priv->csi_clk_freq = common_max_clk;
	} else {
		avt_dbg(sd, "using csi clock from dts: %u Hz\n",
				priv->csi_clk_freq);
	}

	if ((priv->csi_clk_freq < common_min_clk) ||
			(priv->csi_clk_freq > common_max_clk)) {
		avt_err(sd, "unsupported csi clock frequency (%d Hz, range: %d:%d Hz)!\n",
				priv->csi_clk_freq, common_min_clk,
				common_max_clk);
		return -EINVAL;
	}

	CLEAR(i2c_reg);
	clk = priv->csi_clk_freq;
	swapbytes(&clk, AV_CAM_DATA_SIZE_32);
	i2c_reg = priv->cci_reg.bcrm_addr + BCRM_CSI2_CLOCK_32RW;
	i2c_reg_size = AV_CAM_REG_SIZE;
	i2c_reg_count = AV_CAM_DATA_SIZE_32;
	i2c_reg_buf = (char *) &clk;
	ret = ioctl_gencam_i2cwrite_reg(priv->client, i2c_reg, i2c_reg_size,
					i2c_reg_count, i2c_reg_buf);

	ret = avt_reg_read(priv->client,
			priv->cci_reg.bcrm_addr + BCRM_CSI2_CLOCK_32RW,
			AV_CAM_REG_SIZE, AV_CAM_DATA_SIZE_32,
			(char *) &avt_max_clk);

	if (ret < 0) {
		avt_err(sd, "i2c read failed (%d)\n", ret);
		return ret;
	}

	avt_dbg(sd, "csi clock read from camera: %d Hz\n", avt_max_clk);

	ret = avt_read_fmt_from_device(sd, &(priv->mbus_fmt_code));
	if (ret < 0)
		return ret;

	ret = avt_init_frame_param(sd);
	if (ret < 0)
		return ret;

	sel.target = V4L2_SEL_TGT_CROP;
	sel.r = priv->frmp.r;
	ret = avt_set_selection(sd, NULL, &sel);
	if (ret < 0)
		return ret;

	// set BCRM mode
	CLEAR(i2c_reg);
	i2c_reg = CCI_CHANGE_MODE_8W;
	i2c_reg_size = AV_CAM_REG_SIZE;
	i2c_reg_count = AV_CAM_DATA_SIZE_8;
	i2c_reg_buf = (char *) &bcm_mode;

	ret = ioctl_gencam_i2cwrite_reg(priv->client,
			i2c_reg, i2c_reg_size,
			i2c_reg_count, i2c_reg_buf);
	if (ret < 0) {
		avt_err(sd, "Failed to set BCM mode: i2c write failed (%d)\n", ret);
		return ret;
	}
	priv->mode = AVT_BCRM_MODE;

	return 0;
}

static int avt_csi2_probe(struct i2c_client *client,
			const struct i2c_device_id *id)
{
	struct avt_csi2_priv *priv;
	struct device *dev = &client->dev;
	int ret;
	u64 framerate;
	struct v4l2_fract *tpf;

	struct v4l2_of_endpoint *endpoint;
	struct device_node *ep;
	struct camera_common_data *common_data;
	struct v4l2_queryctrl qctrl;
	struct v4l2_ctrl *ctrl;
	union cci_device_caps_reg device_caps;
	int i, j;
    
	v4l_dbg(1, debug, client, "chip found @ 0x%x (%s)\n",
		client->addr << 1, client->adapter->name);

	common_data = devm_kzalloc(&client->dev,
			sizeof(struct camera_common_data), GFP_KERNEL);
	if (!common_data)
		return -ENOMEM;

	priv = devm_kzalloc(&client->dev, sizeof(struct avt_csi2_priv),
			GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->subdev = &common_data->subdev;
	priv->subdev->ctrl_handler = &priv->hdl;
	priv->client = client;

	priv->s_data = common_data;

	ep = of_graph_get_next_endpoint(dev->of_node, NULL);
	if (!ep) {
		dev_err(dev, "missing endpoint node\n");
		return -EINVAL;
	}

	endpoint = v4l2_of_alloc_parse_endpoint(ep);
	if (IS_ERR(endpoint)) {
		dev_err(dev, "failed to parse endpoint\n");
		return PTR_ERR(endpoint);
	}

	v4l2_i2c_subdev_init(priv->subdev, client, &avt_csi2_subdev_ops);

	priv->subdev->internal_ops = &avt_csi2_int_ops;
	priv->subdev->dev = &client->dev;

	/* Set owner to NULL so we can unload the driver module */
	priv->subdev->owner = NULL;

	common_data->priv = priv;
	common_data->dev = &client->dev;
	common_data->ctrl_handler = &priv->hdl;
	common_data->ctrls = priv->ctrls;

	priv->streamcap.capability = V4L2_MODE_HIGHQUALITY |
					V4L2_CAP_TIMEPERFRAME;
	priv->streamcap.capturemode = 0;
	priv->streamcap.timeperframe.denominator = DEFAULT_FPS;
	priv->streamcap.timeperframe.numerator = 1;

    if (!device_present(client))
    {
        dev_err(dev, "No camera detected");
        return -ENXIO;
    }
    else
    {
        dev_info(dev, "Camera detected!");
    }
            
    /* Execute softreset to ensure camera is not in GenCP mode anymore */
    ret = soft_reset(client);
    if (ret < 0)
    {
        return ret;
    }

	ret = read_cci_registers(client);

    /* DEBUG: Dump all BCRM registers */
    bcrm_dump(client);

	/* Set subdev name */
	snprintf(priv->subdev->name, sizeof(priv->subdev->name), "%s %s %d-%x",
		priv->cci_reg.family_name,
		priv->cci_reg.model_name,
		i2c_adapter_id(client->adapter),
		client->addr);

	if (ret < 0) {
		dev_err(dev, "%s: read_cci_registers failed: %d\n",
				__func__, ret);
		return -EIO;
	}

	ret = cci_version_check(client);
	if (ret < 0) {
		dev_err(&client->dev, "cci version mismatch!\n");
		return -EINVAL;
	}

	ret = bcrm_version_check(client);
	if (ret < 0) {
		dev_err(&client->dev, "bcrm version mismatch!\n");
		return -EINVAL;
	}

	dev_dbg(&client->dev, "correct bcrm version\n");

	priv->write_handshake_available =
		bcrm_get_write_handshake_availibility(client);

	avt_init_avail_formats(priv->subdev);

	ret = avt_reg_read(client,
			priv->cci_reg.bcrm_addr +
			BCRM_ACQUISITION_FRAME_RATE_64RW,
			AV_CAM_REG_SIZE, AV_CAM_DATA_SIZE_64,
			(char *) &framerate);
	if (ret < 0) {
		dev_err(&client->dev, "read frameinterval failed\n");
		return ret;
	}

	/* Translate frequency to timeperframe
	 * by inverting the fraction
	 */
	tpf = &(priv->streamcap.timeperframe);
	tpf->numerator = FRAQ_NUM;
	tpf->denominator = (framerate * FRAQ_NUM) / UHZ_TO_HZ;

	device_caps.value = priv->cci_reg.device_capabilities;

	if (device_caps.caps.gencp) {
		ret = read_gencp_registers(client);
		if (ret < 0) {
			dev_err(dev, "%s: read_gencp_registers failed: %d\n",
					__func__, ret);
			return ret;
		}

		ret = gcprm_version_check(client);
		if (ret < 0) {
			dev_err(&client->dev, "gcprm version mismatch!\n");
			return ret;
		}

		dev_dbg(&client->dev, "correct gcprm version\n");
	}

	ret = sysfs_create_group(&dev->kobj, &avt_csi2_attr_grp);
	if (ret) {
		dev_err(dev, "Failed to create sysfs group (%d)\n", ret);
		return ret;
	}

	v4l2_ctrl_handler_init(&priv->hdl, ARRAY_SIZE(avt_ctrl_mappings));

	for (i = 0, j = 0; j < ARRAY_SIZE(avt_ctrl_mappings); ++j) {
		CLEAR(qctrl);
		qctrl.id = avt_ctrl_mappings[j].id;

		ret = ioctl_queryctrl(priv->subdev, &qctrl);
		if (ret < 0)
			continue;

		dev_dbg(&client->dev, "Checking caps: %s - Range: %d-%d s: %d d: %d - %sabled\n",
			avt_ctrl_mappings[j].attr.name,
			qctrl.minimum,
			qctrl.maximum,
			qctrl.step,
			qctrl.default_value,
			(qctrl.flags & V4L2_CTRL_FLAG_DISABLED) ?
			"dis" : "en");

		if (qctrl.flags & V4L2_CTRL_FLAG_DISABLED)
			continue;

		priv->ctrl_cfg[i].type = qctrl.type;

		if (qctrl.type == V4L2_CTRL_TYPE_INTEGER)
			priv->ctrl_cfg[i].flags |= V4L2_CTRL_FLAG_SLIDER;

		priv->ctrl_cfg[i].ops = &avt_ctrl_ops;
		priv->ctrl_cfg[i].name = avt_ctrl_mappings[j].attr.name;
		priv->ctrl_cfg[i].id = avt_ctrl_mappings[j].id;

		priv->ctrl_cfg[i].min = qctrl.minimum;
		priv->ctrl_cfg[i].max = qctrl.maximum;
		priv->ctrl_cfg[i].def = qctrl.default_value;
		priv->ctrl_cfg[i].step = qctrl.step;
		priv->hdl.error = 0;

		ctrl = v4l2_ctrl_new_custom(&priv->hdl,
			&priv->ctrl_cfg[i], NULL);

		if (ctrl == NULL) {
			dev_err(&client->dev, "Failed to init %s ctrl (%d)\n",
				priv->ctrl_cfg[i].name, priv->hdl.error);
			continue;
		}

		priv->ctrls[i] = ctrl;
		i++;
	}

	for (j = 0; j < ARRAY_SIZE(avt_tegra_ctrl); ++j, ++i) {
		ctrl = v4l2_ctrl_new_custom(
			&priv->hdl,
			&avt_tegra_ctrl[j],
			NULL);

		if (ctrl == NULL) {
			dev_err(&client->dev, "Failed to init %s ctrl\n",
				priv->ctrl_cfg[i].name);
			continue;
		}

		priv->ctrls[i] = ctrl;
	}

	ret = v4l2_ctrl_handler_setup(priv->subdev->ctrl_handler);
	common_data->numctrls = i;

	priv->pad.flags = MEDIA_PAD_FL_SOURCE;
	priv->subdev->entity.ops = &avt_csi2_media_ops;
	ret = tegra_media_entity_init(&priv->subdev->entity, 1,
				&priv->pad, true, true);
	if (ret < 0)
		return ret;

	ret = camera_common_initialize(common_data, "avt_csi2");
	if (ret) {
		dev_err(&client->dev, "Failed to initialize tegra common for avt.\n");
		return ret;
	}

	if (of_property_read_u32(dev->of_node,
				"csi_clk_freq",
				&priv->csi_clk_freq))
		priv->csi_clk_freq = 0;

	priv->numlanes = priv->s_data->numlanes;

	priv->stream_on = false;
	priv->cross_update = false;
	priv->stride_align_enabled = true;
	priv->crop_align_enabled = true;

	ret = avt_init_mode(priv->subdev);
	if (ret < 0)
		return ret;

	ret = v4l2_async_register_subdev(priv->subdev);
	if (ret < 0)
		return ret;

	dev_info(&client->dev, "sensor %s registered\n",
			priv->subdev->name);

	return 0;
}

static int avt_csi2_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);

	sysfs_remove_group(&client->dev.kobj, &avt_csi2_attr_grp);

	v4l2_async_unregister_subdev(sd);
	v4l2_device_unregister_subdev(sd);
	media_entity_cleanup(&sd->entity);

	return 0;
}

static struct i2c_device_id avt_csi2_id[] = {
	{"avt_csi2", 0},
	{}
};

MODULE_DEVICE_TABLE(i2c, avt_csi2_id);

static struct i2c_driver avt_csi2_driver = {
	.driver = {
		.name = "avt_csi2",
		.owner = THIS_MODULE,
		.of_match_table = of_match_ptr(avt_csi2_of_match),
	},
	.probe = avt_csi2_probe,
	.remove = avt_csi2_remove,
	.id_table = avt_csi2_id,
};

module_i2c_driver(avt_csi2_driver);

MODULE_AUTHOR("Allied Vision Inc.");
MODULE_DESCRIPTION("Allied Vision's MIPI-CSI2 Camera Driver");
MODULE_LICENSE("GPL");
MODULE_VERSION("l4t-0.1");
