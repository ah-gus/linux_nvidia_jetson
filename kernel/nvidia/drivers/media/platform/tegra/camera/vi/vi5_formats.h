/*
 * NVIDIA Tegra Video Input Device Driver VI5 formats
 *
 * Copyright (c) 2017-2023, NVIDIA CORPORATION.  All rights reserved.
 *
 * Author: Bhanu Murthy V <bmurthyv@nvidia.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#ifndef __VI5_FORMATS_H_
#define __VI5_FORMATS_H_

#include <media/tegra_camera_core.h>

/*
 * These go into the VI_CHn_PIXFMT_FORMAT register bits 7:0
 * Output pixel memory format for the VI channel.
 */
enum tegra_image_format {
	TEGRA_IMAGE_FORMAT_T_R5G6B5 = 1,
	TEGRA_IMAGE_FORMAT_T_B5G6R5,

	TEGRA_IMAGE_FORMAT_T_R8 = 5,

	TEGRA_IMAGE_FORMAT_T_A8B8G8R8 = 8,
	TEGRA_IMAGE_FORMAT_T_A8R8G8B8,
	TEGRA_IMAGE_FORMAT_T_B8G8R8A8,
	TEGRA_IMAGE_FORMAT_T_R8G8B8A8,

	TEGRA_IMAGE_FORMAT_T_Y8_U8__Y8_V8 = 16,
	TEGRA_IMAGE_FORMAT_T_Y8_V8__Y8_U8,
	TEGRA_IMAGE_FORMAT_T_V8_Y8__U8_Y8,
	TEGRA_IMAGE_FORMAT_T_U8_Y8__V8_Y8,

	TEGRA_IMAGE_FORMAT_T_Y8__U8V8_N420 = 34,
	TEGRA_IMAGE_FORMAT_T_Y8__V8U8_N420,

	TEGRA_IMAGE_FORMAT_T_B5G5R5A1 = 42,
	TEGRA_IMAGE_FORMAT_T_R5G5B5A1,
	TEGRA_IMAGE_FORMAT_T_Y8__U8V8_N422,
	TEGRA_IMAGE_FORMAT_T_Y8__V8U8_N422,
	TEGRA_IMAGE_FORMAT_T_Y8__U8__V8_N422,
	TEGRA_IMAGE_FORMAT_T_Y8__U8__V8_N420,

	TEGRA_IMAGE_FORMAT_T_DPCM_RAW10 = 64,

	TEGRA_IMAGE_FORMAT_T_A2B10G10R10 = 68,
	TEGRA_IMAGE_FORMAT_T_A2R10G10B10,
	TEGRA_IMAGE_FORMAT_T_B10G10R10A2,
	TEGRA_IMAGE_FORMAT_T_R10G10B10A2,

	TEGRA_IMAGE_FORMAT_T_A4B4G4R4 = 80,
	TEGRA_IMAGE_FORMAT_T_A4R4G4B4,
	TEGRA_IMAGE_FORMAT_T_B4G4R4A4,
	TEGRA_IMAGE_FORMAT_T_R4G4B4A4,
	TEGRA_IMAGE_FORMAT_T_A1B5G5R5,
	TEGRA_IMAGE_FORMAT_T_A1R5G5B5,

	TEGRA_IMAGE_FORMAT_T_Y10__V10U10_N420 = 98,
	TEGRA_IMAGE_FORMAT_T_Y10__U10V10_N420,
	TEGRA_IMAGE_FORMAT_T_Y10__U10__V10_N420,
	TEGRA_IMAGE_FORMAT_T_Y10__V10U10_N422,
	TEGRA_IMAGE_FORMAT_T_Y10__U10V10_N422,
	TEGRA_IMAGE_FORMAT_T_Y10__U10__V10_N422,

	TEGRA_IMAGE_FORMAT_T_DPCM_RAW12 = 128,

	TEGRA_IMAGE_FORMAT_T_R16_ISP = 194,
	TEGRA_IMAGE_FORMAT_T_R16_F,
	TEGRA_IMAGE_FORMAT_T_R16,
	TEGRA_IMAGE_FORMAT_T_R16_I,

	TEGRA_IMAGE_FORMAT_T_R32 = 230,

	TEGRA_IMAGE_FORMAT_T_R32_F = 232,

	TEGRA_IMAGE_FORMAT_T_DPCM_RAW16 = 254,
	TEGRA_IMAGE_FORMAT_T_DPCM_RAW20,
};

static const struct tegra_video_format vi5_video_formats[] = {
	/* CUSTOM MIPI DATATYPE */
	TEGRA_VIDEO_FORMAT(RAW8, 8, CUSTOM, 1, 1, T_R8,
					   RAW8, CUSTOM, "0x31 MIPI DATATYPE"),

	/* RAW 6: TODO */

	/* RAW 7: TODO */

	/* RAW 8 */
	TEGRA_VIDEO_FORMAT(RAW8, 8, Y8_1X8, 1, 1, T_R8,
				RAW8, GREY, "GREY 8"),
	TEGRA_VIDEO_FORMAT(RAW8, 8, SRGGB8_1X8, 1, 1, T_R8,
				RAW8, SRGGB8, "RGRG.. GBGB.."),
	TEGRA_VIDEO_FORMAT(RAW8, 8, SGRBG8_1X8, 1, 1, T_R8,
				RAW8, SGRBG8, "GRGR.. BGBG.."),
	TEGRA_VIDEO_FORMAT(RAW8, 8, SGBRG8_1X8, 1, 1, T_R8,
				RAW8, SGBRG8, "GBGB.. RGRG.."),
	TEGRA_VIDEO_FORMAT(RAW8, 8, SBGGR8_1X8, 1, 1, T_R8,
				RAW8, SBGGR8, "BGBG.. GRGR.."),

	/* RAW 10 */
	TEGRA_VIDEO_FORMAT(RAW10, 10, Y10_1X10, 2, 1, T_R16_I,
				RAW10, XAVIER_Y10, "GREY 10"),
	TEGRA_VIDEO_FORMAT(RAW10, 10, SRGGB10_1X10, 2, 1, T_R16_I,
				RAW10, XAVIER_SRGGB10, "RGRG.. GBGB.."),
	TEGRA_VIDEO_FORMAT(RAW10, 10, SGRBG10_1X10, 2, 1, T_R16_I,
				RAW10, XAVIER_SGRBG10, "GRGR.. BGBG.."),
	TEGRA_VIDEO_FORMAT(RAW10, 10, SGBRG10_1X10, 2, 1, T_R16_I,
				RAW10, XAVIER_SGBRG10, "GBGB.. RGRG.."),
	TEGRA_VIDEO_FORMAT(RAW10, 10, SBGGR10_1X10, 2, 1, T_R16_I,
				RAW10, XAVIER_SBGGR10, "BGBG.. GRGR.."),

	/* RAW 12 */
	TEGRA_VIDEO_FORMAT(RAW12, 12, Y12_1X12, 2, 1, T_R16_I,
				RAW12, XAVIER_Y12, "GREY 12"),
	TEGRA_VIDEO_FORMAT(RAW12, 12, SRGGB12_1X12, 2, 1, T_R16_I,
				RAW12, XAVIER_SRGGB12, "RGRG.. GBGB.."),
	TEGRA_VIDEO_FORMAT(RAW12, 12, SGRBG12_1X12, 2, 1, T_R16_I,
				RAW12, XAVIER_SGRBG12, "GRGR.. BGBG.."),
	TEGRA_VIDEO_FORMAT(RAW12, 12, SGBRG12_1X12, 2, 1, T_R16_I,
				RAW12, XAVIER_SGBRG12, "GBGB.. RGRG.."),
	TEGRA_VIDEO_FORMAT(RAW12, 12, SBGGR12_1X12, 2, 1, T_R16_I,
				RAW12, XAVIER_SBGGR12, "BGBG.. GRGR.."),

	/* RGB444 */
	TEGRA_VIDEO_FORMAT(RGB444, 16, RGB444_1X12, 2, 1, T_A4B4G4R4,
				RGB444, ARGB444, "RGB-4-4-4"),

	/* RGB565 */
	TEGRA_VIDEO_FORMAT(RGB565, 16, RGB565_1X16, 2, 1, T_B5G6R5,
				RGB565, RGB565, "RGB-5-6-5"),

	/* RGB888 */
	TEGRA_VIDEO_FORMAT(RGB888, 24, RGB888_1X24, 4, 1, T_B8G8R8A8,
				RGB888, XRGB32, "RGB-8-8-8"),
	TEGRA_VIDEO_FORMAT(RGB888, 24, BGR888_1X24, 4, 1, T_A8R8G8B8,
				RGB888, XBGR32, "BGR-8-8-8"),

	/* RGB666 */
	TEGRA_VIDEO_FORMAT(RGB666, 24, RGB666_1X18, 4, 1, T_A8B8G8R8,
				RGB666, ABGR32, "BGRA-8-8-8-8"),


	/* YUV422 */
	TEGRA_VIDEO_FORMAT(YUV422, 16, UYVY8_2X8, 2, 1, T_U8_Y8__V8_Y8,
				YUV422_8, UYVY, "YUV 4:2:2 UYVY"),
	TEGRA_VIDEO_FORMAT(YUV422, 16, VYUY8_2X8, 2, 1, T_V8_Y8__U8_Y8,
				YUV422_8, VYUY, "YUV 4:2:2 VYUY"),
	TEGRA_VIDEO_FORMAT(YUV422, 16, YUYV8_2X8, 2, 1, T_Y8_U8__Y8_V8,
				YUV422_8, YUYV, "YUV 4:2:2 YUYV"),
	TEGRA_VIDEO_FORMAT(YUV422, 16, YVYU8_2X8, 2, 1, T_Y8_V8__Y8_U8,
				YUV422_8, YVYU, "YUV 4:2:2 YVYU"),
	TEGRA_VIDEO_FORMAT(YUV422, 16, UYVY8_1X16, 2, 1, T_U8_Y8__V8_Y8,
				YUV422_8, UYVY, "YUV 4:2:2"),
	TEGRA_VIDEO_FORMAT(YUV422, 16, VYUY8_1X16, 2, 1, T_V8_Y8__U8_Y8,
				YUV422_8, VYUY, "YUV 4:2:2"),
	TEGRA_VIDEO_FORMAT(YUV422, 16, YUYV8_1X16, 2, 1, T_Y8_U8__Y8_V8,
				YUV422_8, YUYV, "YUV 4:2:2"),
	TEGRA_VIDEO_FORMAT(YUV422, 16, YVYU8_1X16, 2, 1, T_Y8_V8__Y8_U8,
				YUV422_8, YVYU, "YUV 4:2:2"),
	TEGRA_VIDEO_FORMAT(YUV422, 16, UYVY8_1X16, 1, 1, T_Y8__V8U8_N422,
				YUV422_8, NV16, "NV16"),
};

#endif
