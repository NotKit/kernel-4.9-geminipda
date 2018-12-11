/*
 * Copyright (C) 2015 MediaTek Inc.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 */

#ifndef __M4U_PORT_H__
#define __M4U_PORT_H__

/* ==================================== */
/* about portid */
/* ==================================== */

enum {
	/*larb0 */
	M4U_PORT_DISP_OVL0,
	M4U_PORT_DISP_2L_OVL0_LARB0,
	M4U_PORT_DISP_RDMA0,
	M4U_PORT_DISP_WDMA0,
	M4U_PORT_MDP_RDMA0,
	M4U_PORT_MDP_WDMA0,
	M4U_PORT_MDP_WROT0,
	M4U_PORT_DISP_FAKE0,
	/*larb1 */
	M4U_PORT_VENC_RCPU,
	M4U_PORT_VENC_REC,
	M4U_PORT_VENC_BSDMA,
	M4U_PORT_VENC_SV_COMV,
	M4U_PORT_VENC_RD_COMV,
	M4U_PORT_JPGENC_RDMA,
	M4U_PORT_JPGENC_BSDMA,
	M4U_PORT_VENC_CUR_LUMA,
	M4U_PORT_VENC_CUR_CHROMA,
	M4U_PORT_VENC_REF_LUMA,
	M4U_PORT_VENC_REF_CHROMA,
	/*larb2 */
	M4U_PORT_CAM_IMGO,
	M4U_PORT_CAM_RRZO,
	M4U_PORT_CAM_AAO,
	M4U_PORT_CAM_LCSO,
	M4U_PORT_CAM_ESFKO,
	M4U_PORT_CAM_IMGO_S,
	M4U_PORT_CAM_IMGO_S2,
	M4U_PORT_CAM_LSCI,
	M4U_PORT_CAM_LSCI_D,
	M4U_PORT_CAM_AFO,
	M4U_PORT_CAM_AFO_D,
	M4U_PORT_CAM_BPCI,
	M4U_PORT_CAM_BPCI_D,
	M4U_PORT_CAM_UFDI,
	M4U_PORT_CAM_IMGI,
	M4U_PORT_CAM_IMG2O,
	M4U_PORT_CAM_IMG3O,
	M4U_PORT_CAM_VIPI,
	M4U_PORT_CAM_VIP2I,
	M4U_PORT_CAM_VIP3I,
	M4U_PORT_CAM_ICEI,
	M4U_PORT_CAM_FD_RP,
	M4U_PORT_CAM_FD_WR,
	M4U_PORT_CAM_FD_RB,

	M4U_PORT_UNKNOWN
};
#define M4U_PORT_NR M4U_PORT_UNKNOWN

#endif
