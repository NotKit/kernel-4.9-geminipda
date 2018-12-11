/*
 * Copyright (C) 2016 MediaTek Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See http://www.gnu.org/licenses/gpl-2.0.html for more details.
 */

#include "mtk_cpufreq_config.h"

#define NR_FREQ		16
#define ARRAY_COL_SIZE	4

static unsigned int fyTbl[NR_FREQ * NR_MT_CPU_DVFS][ARRAY_COL_SIZE] = {
	/* Freq, Vproc, post_div, clk_div */
	{ 2001, 81, 1, 1 },	/* LL */
	{ 1917, 76, 1, 1 },
	{ 1834, 71, 1, 1 },
	{ 1767, 67, 1, 1 },
	{ 1700, 63, 1, 1 },
	{ 1633, 59, 1, 1 },
	{ 1533, 53, 1, 1 },
	{ 1466, 49, 2, 1 },
	{ 1400, 45, 2, 1 },
	{ 1308, 41, 2, 1 },
	{ 1216, 37, 2, 1 },
	{ 1125, 33, 2, 1 },
	{ 1056, 30, 2, 1 },
	{  987, 27, 2, 1 },
	{  918, 24, 2, 1 },
	{  850, 21, 2, 1 },
};

static unsigned int sbTbl[NR_FREQ * NR_MT_CPU_DVFS][ARRAY_COL_SIZE] = {
	/* Freq, Vproc, post_div, clk_div */
	{ 2201, 81, 1, 1 },	/* LL */
	{ 2089, 76, 1, 1 },
	{ 1978, 71, 1, 1 },
	{ 1889, 67, 1, 1 },
	{ 1800, 63, 1, 1 },
	{ 1711, 59, 1, 1 },
	{ 1622, 55, 1, 1 },
	{ 1533, 51, 1, 1 },
	{ 1466, 48, 2, 1 },
	{ 1400, 45, 2, 1 },
	{ 1308, 41, 2, 1 },
	{ 1216, 37, 2, 1 },
	{ 1125, 33, 2, 1 },
	{ 1033, 29, 2, 1 },
	{  941, 25, 2, 1 },
	{  850, 21, 2, 1 },
};

static unsigned int fy2Tbl[NR_FREQ * NR_MT_CPU_DVFS][ARRAY_COL_SIZE] = {
	/* Freq, Vproc, post_div, clk_div */
	{ 2001, 81, 1, 1 },	/* LL */
	{ 1917, 76, 1, 1 },
	{ 1834, 71, 1, 1 },
	{ 1767, 67, 1, 1 },
	{ 1700, 63, 1, 1 },
	{ 1633, 59, 1, 1 },
	{ 1533, 53, 1, 1 },
	{ 1466, 49, 2, 1 },
	{ 1400, 45, 2, 1 },
	{ 1308, 45, 2, 1 },
	{ 1216, 45, 2, 1 },
	{ 1125, 45, 2, 1 },
	{ 1056, 45, 2, 1 },
	{  987, 45, 2, 1 },
	{  918, 45, 2, 1 },
	{  850, 45, 2, 1 },
};

unsigned int *xrecordTbl[NUM_CPU_LEVEL] = {	/* v0.3 */
	[CPU_LEVEL_0] = &fyTbl[0][0],
	[CPU_LEVEL_1] = &sbTbl[0][0],
	[CPU_LEVEL_2] = &fy2Tbl[0][0],
};
