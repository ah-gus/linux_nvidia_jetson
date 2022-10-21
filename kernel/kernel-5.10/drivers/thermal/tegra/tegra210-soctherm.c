// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2014-2018, NVIDIA CORPORATION.  All rights reserved.
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <linux/module.h>
#include <linux/platform_device.h>
#include <soc/tegra/fuse.h>

#include <dt-bindings/thermal/tegra124-soctherm.h>

#include "soctherm.h"

#define TEGRA210_THERMTRIP_ANY_EN_MASK		(0x1 << 31)
#define TEGRA210_THERMTRIP_MEM_EN_MASK		(0x1 << 30)
#define TEGRA210_THERMTRIP_GPU_EN_MASK		(0x1 << 29)
#define TEGRA210_THERMTRIP_CPU_EN_MASK		(0x1 << 28)
#define TEGRA210_THERMTRIP_TSENSE_EN_MASK	(0x1 << 27)
#define TEGRA210_THERMTRIP_GPUMEM_THRESH_MASK	(0x1ff << 18)
#define TEGRA210_THERMTRIP_CPU_THRESH_MASK	(0x1ff << 9)
#define TEGRA210_THERMTRIP_TSENSE_THRESH_MASK	0x1ff

#define TEGRA210_THERM_IRQ_MEM_MASK		(0x3 << 24)
#define TEGRA210_THERM_IRQ_GPU_MASK		(0x3 << 16)
#define TEGRA210_THERM_IRQ_CPU_MASK		(0x3 << 8)
#define TEGRA210_THERM_IRQ_TSENSE_MASK		(0x3 << 0)

#define TEGRA210_THERMCTL_LVL0_UP_THRESH_MASK	(0x1ff << 18)
#define TEGRA210_THERMCTL_LVL0_DN_THRESH_MASK	(0x1ff << 9)

#define TEGRA210_THRESH_GRAIN			500
#define TEGRA210_BPTT				9

static const struct tegra_tsensor_configuration tegra210_tsensor_config = {
	.tall = 16300,
	.tiddq_en = 1,
	.ten_count = 1,
	.tsample = 120,
	.tsample_ate = 480,
	.pdiv = 8,
	.pdiv_ate = 8,
};

static const struct tegra_tsensor_configuration tegra210b01_tsensor_config = {
	.tall = 16300,
	.tiddq_en = 1,
	.ten_count = 1,
	.tsample = 240,
	.tsample_ate = 480,
	.pdiv = 12,
	.pdiv_ate = 6,
};

static const struct tegra_tsensor_group tegra210_tsensor_group_cpu = {
	.id = TEGRA124_SOCTHERM_SENSOR_CPU,
	.name = "cpu",
	.sensor_temp_offset = SENSOR_TEMP1,
	.sensor_temp_mask = SENSOR_TEMP1_CPU_TEMP_MASK,
	.pdiv_mask = SENSOR_PDIV_CPU_MASK,
	.pllx_hotspot_diff = 10,
	.pllx_hotspot_mask = SENSOR_HOTSPOT_CPU_MASK,
	.hw_pllx_offset_mask = SENSOR_HW_PLLX_OFFSET_CPU_MASK,
	.hw_pllx_offset_en_mask = SENSOR_HW_PLLX_OFFSET_CPU_EN_MASK,
	.thermtrip_any_en_mask = TEGRA210_THERMTRIP_ANY_EN_MASK,
	.thermtrip_enable_mask = TEGRA210_THERMTRIP_CPU_EN_MASK,
	.thermtrip_threshold_mask = TEGRA210_THERMTRIP_CPU_THRESH_MASK,
	.thermctl_isr_mask = THERM_IRQ_CPU_MASK,
	.thermctl_lvl0_offset = THERMCTL_LEVEL0_GROUP_CPU,
	.thermctl_isr_mask = TEGRA210_THERM_IRQ_CPU_MASK,
	.thermctl_lvl0_up_thresh_mask = TEGRA210_THERMCTL_LVL0_UP_THRESH_MASK,
	.thermctl_lvl0_dn_thresh_mask = TEGRA210_THERMCTL_LVL0_DN_THRESH_MASK,
};

static const struct tegra_tsensor_group tegra210_tsensor_group_gpu = {
	.id = TEGRA124_SOCTHERM_SENSOR_GPU,
	.name = "gpu",
	.sensor_temp_offset = SENSOR_TEMP1,
	.sensor_temp_mask = SENSOR_TEMP1_GPU_TEMP_MASK,
	.pdiv_mask = SENSOR_PDIV_GPU_MASK,
	.pllx_hotspot_diff = 5,
	.pllx_hotspot_mask = SENSOR_HOTSPOT_GPU_MASK,
	.hw_pllx_offset_mask = SENSOR_HW_PLLX_OFFSET_GPU_MASK,
	.hw_pllx_offset_en_mask = SENSOR_HW_PLLX_OFFSET_GPU_EN_MASK,
	.thermtrip_any_en_mask = TEGRA210_THERMTRIP_ANY_EN_MASK,
	.thermtrip_enable_mask = TEGRA210_THERMTRIP_GPU_EN_MASK,
	.thermtrip_threshold_mask = TEGRA210_THERMTRIP_GPUMEM_THRESH_MASK,
	.thermctl_isr_mask = THERM_IRQ_GPU_MASK,
	.thermctl_lvl0_offset = THERMCTL_LEVEL0_GROUP_GPU,
	.thermctl_isr_mask = TEGRA210_THERM_IRQ_GPU_MASK,
	.thermctl_lvl0_up_thresh_mask = TEGRA210_THERMCTL_LVL0_UP_THRESH_MASK,
	.thermctl_lvl0_dn_thresh_mask = TEGRA210_THERMCTL_LVL0_DN_THRESH_MASK,
};

static const struct tegra_tsensor_group tegra210_tsensor_group_pll = {
	.id = TEGRA124_SOCTHERM_SENSOR_PLLX,
	.name = "pll",
	.sensor_temp_offset = SENSOR_TEMP2,
	.sensor_temp_mask = SENSOR_TEMP2_PLLX_TEMP_MASK,
	.pdiv_mask = SENSOR_PDIV_PLLX_MASK,
	.thermtrip_any_en_mask = TEGRA210_THERMTRIP_ANY_EN_MASK,
	.thermtrip_enable_mask = TEGRA210_THERMTRIP_TSENSE_EN_MASK,
	.thermtrip_threshold_mask = TEGRA210_THERMTRIP_TSENSE_THRESH_MASK,
	.thermctl_isr_mask = THERM_IRQ_TSENSE_MASK,
	.thermctl_lvl0_offset = THERMCTL_LEVEL0_GROUP_TSENSE,
	.thermctl_isr_mask = TEGRA210_THERM_IRQ_TSENSE_MASK,
	.thermctl_lvl0_up_thresh_mask = TEGRA210_THERMCTL_LVL0_UP_THRESH_MASK,
	.thermctl_lvl0_dn_thresh_mask = TEGRA210_THERMCTL_LVL0_DN_THRESH_MASK,
};

static const struct tegra_tsensor_group tegra210_tsensor_group_mem = {
	.id = TEGRA124_SOCTHERM_SENSOR_MEM,
	.name = "mem",
	.sensor_temp_offset = SENSOR_TEMP2,
	.sensor_temp_mask = SENSOR_TEMP2_MEM_TEMP_MASK,
	.pdiv_mask = SENSOR_PDIV_MEM_MASK,
	.pllx_hotspot_diff = 0,
	.pllx_hotspot_mask = SENSOR_HOTSPOT_MEM_MASK,
	.hw_pllx_offset_mask = SENSOR_HW_PLLX_OFFSET_MEM_MASK,
	.hw_pllx_offset_en_mask = SENSOR_HW_PLLX_OFFSET_MEM_EN_MASK,
	.thermtrip_any_en_mask = TEGRA210_THERMTRIP_ANY_EN_MASK,
	.thermtrip_enable_mask = TEGRA210_THERMTRIP_MEM_EN_MASK,
	.thermtrip_threshold_mask = TEGRA210_THERMTRIP_GPUMEM_THRESH_MASK,
	.thermctl_isr_mask = THERM_IRQ_MEM_MASK,
	.thermctl_lvl0_offset = THERMCTL_LEVEL0_GROUP_MEM,
	.thermctl_isr_mask = TEGRA210_THERM_IRQ_MEM_MASK,
	.thermctl_lvl0_up_thresh_mask = TEGRA210_THERMCTL_LVL0_UP_THRESH_MASK,
	.thermctl_lvl0_dn_thresh_mask = TEGRA210_THERMCTL_LVL0_DN_THRESH_MASK,
};

static const struct tegra_tsensor_group *tegra210_tsensor_groups[] = {
	&tegra210_tsensor_group_cpu,
	&tegra210_tsensor_group_gpu,
	&tegra210_tsensor_group_pll,
	&tegra210_tsensor_group_mem,
};

static const struct tegra_tsensor_group *tegra210b01_tsensor_groups[] = {
	&tegra210_tsensor_group_cpu,
	&tegra210_tsensor_group_gpu,
	&tegra210_tsensor_group_pll,
};

static struct tsensor_group_offsets tegra210_tsensor_group_offsets[] = {
	{.ttg = &tegra210_tsensor_group_cpu,},
	{.ttg = &tegra210_tsensor_group_gpu,},
	{.ttg = &tegra210_tsensor_group_mem,},
	{.ttg = &tegra210_tsensor_group_pll,},
};

static const struct tegra_tsensor tegra210_tsensors[] = {
	{
		.name = "cpu0",
		.base = 0xc0,
		.config = &tegra210_tsensor_config,
		.calib_fuse_offset = 0x098,
		.fuse_corr = {
			.alpha = 1085000,
			.beta = 3244200,
		},
		.group = &tegra210_tsensor_group_cpu,
	}, {
		.name = "cpu1",
		.base = 0xe0,
		.config = &tegra210_tsensor_config,
		.calib_fuse_offset = 0x084,
		.fuse_corr = {
			.alpha = 1126200,
			.beta = -67500,
		},
		.group = &tegra210_tsensor_group_cpu,
	}, {
		.name = "cpu2",
		.base = 0x100,
		.config = &tegra210_tsensor_config,
		.calib_fuse_offset = 0x088,
		.fuse_corr = {
			.alpha = 1098400,
			.beta = 2251100,
		},
		.group = &tegra210_tsensor_group_cpu,
	}, {
		.name = "cpu3",
		.base = 0x120,
		.config = &tegra210_tsensor_config,
		.calib_fuse_offset = 0x12c,
		.fuse_corr = {
			.alpha = 1108000,
			.beta = 602700,
		},
		.group = &tegra210_tsensor_group_cpu,
	}, {
		.name = "mem0",
		.base = 0x140,
		.config = &tegra210_tsensor_config,
		.calib_fuse_offset = 0x158,
		.fuse_corr = {
			.alpha = 1069200,
			.beta = 3549900,
		},
		.group = &tegra210_tsensor_group_mem,
	}, {
		.name = "mem1",
		.base = 0x160,
		.config = &tegra210_tsensor_config,
		.calib_fuse_offset = 0x15c,
		.fuse_corr = {
			.alpha = 1173700,
			.beta = -6263600,
		},
		.group = &tegra210_tsensor_group_mem,
	}, {
		.name = "gpu",
		.base = 0x180,
		.config = &tegra210_tsensor_config,
		.calib_fuse_offset = 0x154,
		.fuse_corr = {
			.alpha = 1074300,
			.beta = 2734900,
		},
		.group = &tegra210_tsensor_group_gpu,
	}, {
		.name = "pllx",
		.base = 0x1a0,
		.config = &tegra210_tsensor_config,
		.calib_fuse_offset = 0x160,
		.fuse_corr = {
			.alpha = 1039700,
			.beta = 6829100,
		},
		.group = &tegra210_tsensor_group_pll,
	},
};

static const struct tegra_tsensor tegra210b01_tsensors[] = {
	{
		.name = "cpu0",
		.base = 0xc0,
		.config = &tegra210b01_tsensor_config,
		.calib_fuse_offset = 0x098,
		.fuse_corr = {
			.alpha = 1085000,
			.beta = 3244200,
		},
		.group = &tegra210_tsensor_group_cpu,
	}, {
		.name = "cpu1",
		.base = 0xe0,
		.config = &tegra210b01_tsensor_config,
		.calib_fuse_offset = 0x084,
		.fuse_corr = {
			.alpha = 1126200,
			.beta = -67500,
		},
		.group = &tegra210_tsensor_group_cpu,
	}, {
		.name = "cpu2",
		.base = 0x100,
		.config = &tegra210b01_tsensor_config,
		.calib_fuse_offset = 0x088,
		.fuse_corr = {
			.alpha = 1098400,
			.beta = 2251100,
		},
		.group = &tegra210_tsensor_group_cpu,
	}, {
		.name = "cpu3",
		.base = 0x120,
		.config = &tegra210b01_tsensor_config,
		.calib_fuse_offset = 0x12c,
		.fuse_corr = {
			.alpha = 1108000,
			.beta = 602700,
		},
		.group = &tegra210_tsensor_group_cpu,
	}, {
		.name = "gpu",
		.base = 0x180,
		.config = &tegra210b01_tsensor_config,
		.calib_fuse_offset = 0x154,
		.fuse_corr = {
			.alpha = 1074300,
			.beta = 2734900,
		},
		.group = &tegra210_tsensor_group_gpu,
	}, {
		.name = "pllx",
		.base = 0x1a0,
		.config = &tegra210b01_tsensor_config,
		.calib_fuse_offset = 0x160,
		.fuse_corr = {
			.alpha = 1039700,
			.beta = 6829100,
		},
		.group = &tegra210_tsensor_group_pll,
	},
};

/*
 * Mask/shift bits in FUSE_TSENSOR_COMMON and
 * FUSE_TSENSOR_COMMON, which are described in
 * tegra_soctherm_fuse.c
 */
static const struct tegra_tsensor_fuse tegra210_soctherm_fuse = {
	.fuse_base_cp_mask = 0x3ff << 11,
	.fuse_base_cp_shift = 11,
	.fuse_base_ft_mask = 0x7ff << 21,
	.fuse_base_ft_shift = 21,
	.fuse_shift_ft_mask = 0x1f << 6,
	.fuse_shift_ft_shift = 6,
	.fuse_spare_realignment = 0,
};

static struct tsensor_group_thermtrips tegra210_tsensor_thermtrips[] = {
	{.id = TEGRA124_SOCTHERM_SENSOR_NUM},
	{.id = TEGRA124_SOCTHERM_SENSOR_NUM},
	{.id = TEGRA124_SOCTHERM_SENSOR_NUM},
	{.id = TEGRA124_SOCTHERM_SENSOR_NUM},
};

const struct tegra_soctherm_soc tegra210_soctherm = {
	.tsensors = tegra210_tsensors,
	.num_tsensors = ARRAY_SIZE(tegra210_tsensors),
	.ttgs = tegra210_tsensor_groups,
	.toffs = tegra210_tsensor_group_offsets,
	.num_ttgs = ARRAY_SIZE(tegra210_tsensor_groups),
	.tfuse = &tegra210_soctherm_fuse,
	.thresh_grain = TEGRA210_THRESH_GRAIN,
	.bptt = TEGRA210_BPTT,
	.use_ccroc = false,
	.thermtrips = tegra210_tsensor_thermtrips,
};

const struct tegra_soctherm_soc tegra210b01_soctherm = {
	.tsensors = tegra210b01_tsensors,
	.num_tsensors = ARRAY_SIZE(tegra210b01_tsensors),
	.ttgs = tegra210b01_tsensor_groups,
	.toffs = tegra210_tsensor_group_offsets,
	.num_ttgs = ARRAY_SIZE(tegra210b01_tsensor_groups),
	.tfuse = &tegra210_soctherm_fuse,
	.thresh_grain = TEGRA210_THRESH_GRAIN,
	.bptt = TEGRA210_BPTT,
	.use_ccroc = false,
	.thermtrips = tegra210_tsensor_thermtrips,
};