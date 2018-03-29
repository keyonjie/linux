/*
 * dwmac5.h: DW EQoS version 5.00 Header file
 *
 * Copyright (c) 2017, Intel Corporation.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 */

#ifndef __DWMAC5_H__
#define __DWMAC5_H__

#include "dwmac4.h"

/* DWMAC v5.00 supports the following Time Sensitive Network protocols:
 * 1) IEEE 802.1 Qbv Enhancements for Scheduled Traffic (EST)
 * 2) IEEE 802.1 Qbu Frame Preemption (FPE)
 *
 * In addition, the IP supports Time-based Scheduling (TBS).
 */

#define GMAC_INT_FPE_EN			BIT(17)

/* FPRQ only available in EQoS ver5.00 MAC_RxQ_Ctrl1 */
#define GMAC_RXQCTRL_FPRQ_MASK		GENMASK(26, 24)	/* FPE Residue Queue */
#define GMAC_RXQCTRL_FPRQ_SHIFT		24

/* MAC HW features3 bitmap */
#define GMAC_HW_FEAT_TBSSEL		BIT(27)
#define GMAC_HW_FEAT_FPESEL		BIT(26)
#define GMAC_HW_FEAT_ESTTISW		GENMASK(24, 23)
#define GMAC_HW_FEAT_ESTTISW_SHIFT	23
#define GMAC_HW_FEAT_ESTWID		GENMASK(21, 20)
#define GMAC_HW_FEAT_ESTWID_SHIFT	20
#define GMAC_HW_FEAT_ESTDEP		GENMASK(19, 17)
#define GMAC_HW_FEAT_ESTDEP_SHIFT	17
#define GMAC_HW_FEAT_ESTSEL		BIT(16)

/* MAC FPE control status */
#define MAC_FPE_CTRL_STS		0x00000234
#define MAC_FPE_CTRL_STS_TRSP		BIT(19)
#define MAC_FPE_CTRL_STS_TVER		BIT(18)
#define MAC_FPE_CTRL_STS_RRSP		BIT(17)
#define MAC_FPE_CTRL_STS_RVER		BIT(16)
#define MAC_FPE_CTRL_STS_SRSP		BIT(2)
#define MAC_FPE_CTRL_STS_SVER		BIT(1)
#define MAC_FPE_CTRL_STS_EFPE		BIT(0)

/* MTL TBS Control register */
#define MTL_TBS_CTRL			0x00000c40
#define MTL_TBS_CTRL_LEOS		GENMASK(31, 8)
#define MTL_TBS_CTRL_LEOS_SHIFT		8
#define MTL_TBS_CTRL_LEGOS		GENMASK(6, 4)
#define MTL_TBS_CTRL_LEGOS_SHIFT	4
#define MTL_TBS_CTRL_LEOV		BIT(1)
#define MTL_TBS_CTRL_ESTM		BIT(0)

/* MTL EST control register */
#define MTL_EST_CTRL			0x00000c50
#define MTL_EST_CTRL_PTOV		GENMASK(31, 24)
#define MTL_EST_CTRL_PTOV_SHIFT		24
#define MTL_EST_CTRL_CTOV		GENMASK(23, 12)
#define MTL_EST_CTRL_CTOV_SHIFT		12
#define MTL_EST_CTRL_TILS		GENMASK(10, 8)
#define MTL_EST_CTRL_TILS_SHIFT		8
#define MTL_EST_CTRL_SSWL		BIT(1)	/* Switch to SWOL */
#define MTL_EST_CTRL_EEST		BIT(0)	/* Enable EST */

/* MTL EST status register */
#define MTL_EST_STATUS			0x00000c58
#define MTL_EST_STATUS_BTRL		GENMASK(11, 8)	/* BTR ERR loop cnt */
#define MTL_EST_STATUS_BTRL_SHIFT	8
#define MTL_EST_STATUS_BTRL_MAX		(0xF << 8)
#define MTL_EST_STATUS_SWOL		BIT(7)	/* SW owned list */
#define MTL_EST_STATUS_SWOL_SHIFT	7
#define MTL_EST_STATUS_CGCE		BIT(4)	/* Constant gate ctrl err */
#define MTL_EST_STATUS_HLBS		BIT(3)	/* HLB due to scheduling */
#define MTL_EST_STATUS_HLBF		BIT(2)	/* HLB due to frame size */
#define MTL_EST_STATUS_BTRE		BIT(1)	/* BTR Error */
#define MTL_EST_STATUS_SWLC		BIT(0)	/* Switch to SWOL complete */

/* MTL EST Scheduling error */
#define MTL_EST_SCH_ERR			0x00000c60
#define MTL_EST_FRM_SZ_ERR		0x00000c64
#define MTL_EST_FRM_SZ_CAP		0x00000c68
#define MTL_EST_FRM_SZ_CAP_HBFS_MASK	GENMASK(14, 0)
#define MTL_EST_FRM_SZ_CAP_HBFQ_SHIFT	16
#define MTL_EST_FRM_SZ_CAP_HBFQ_MASK(x)		(x > 4 ? GENMASK(18, 16) : \
						 x > 2 ? GENMASK(17, 16) : \
						 BIT(16))

/* MTL EST interrupt enable */
#define MTL_EST_INT_EN			0x00000c70
#define MTL_EST_INT_EN_CGCE		BIT(4)
#define MTL_EST_INT_EN_IEHS		BIT(3)
#define MTL_EST_INT_EN_IEHF		BIT(2)
#define MTL_EST_INT_EN_IEBE		BIT(1)
#define MTL_EST_INT_EN_IECC		BIT(0)

/* MTL EST GCL control register */
#define MTL_EST_GCL_CTRL		0x00000c80
#define MTL_EST_GCL_CTRL_ADDR		GENMASK(10, 8)	/* GCL Address */
#define MTL_EST_GCL_CTRL_ADDR_VAL(addr)	(addr << 8)
#define GCL_CTRL_ADDR_BTR_LO		0x0
#define GCL_CTRL_ADDR_BTR_HI		0x1
#define GCL_CTRL_ADDR_CTR_LO		0x2
#define GCL_CTRL_ADDR_CTR_HI		0x3
#define GCL_CTRL_ADDR_TER		0x4
#define GCL_CTRL_ADDR_LLR		0x5
#define MTL_EST_GCL_CTRL_DBGB1		BIT(5)	/* Debug Mode Bank Select */
#define MTL_EST_GCL_CTRL_DBGM		BIT(4)	/* Debug Mode */
#define MTL_EST_GCL_CTRL_GCRR		BIT(2)	/* GC Related Registers */
#define MTL_EST_GCL_CTRL_R1W0		BIT(1)	/* Read / Write Operation */
#define GCL_OPS_R			BIT(1)
#define GCL_OPS_W			0
#define MTL_EST_GCL_CTRL_SRWO		BIT(0)	/* Start R/W Operation */

/* MTL EST GCL data register */
#define MTL_EST_GCL_DATA		0x00000c84

/* MTL FPE control status */
#define MTL_FPE_CTRL_STS		0x00000c90
#define MTL_FPE_CTRL_STS_HRS		BIT(28)	/* Hold/Release Status */
#define MTL_FPE_CTRL_STS_HRS_SHIFT	28
#define MTL_FPE_CTRL_STS_PEC		GENMASK(15, 8)	/* FPE Classification */
#define MTL_FPE_CTRL_STS_PEC_SHIFT	8
#define MTL_FPE_CTRL_STS_AFSZ		GENMASK(1, 0)	/* Extra Frag Size */

/* MTL FPE Advance */
#define MTL_FPE_ADVANCE			0x00000c94
#define MTL_FPE_ADVANCE_RADV		GENMASK(31, 16)	/* Release Advance */
#define MTL_FPE_ADVANCE_RADV_SHIFT	16
#define MTL_FPE_ADVANCE_HADV		GENMASK(15, 0)	/* Hold Advance */

/* EST Global defines */
#define EST_CTR_HI_MAX			0xff	/* CTR Hi is 8-bit only */
#define EST_PTOV_MAX			0xff	/* Max PTP time offset */
#define EST_CTOV_MAX			0xfff	/* Max Current time offset */
#define EST_TIWID_TO_EXTMAX(ti_wid)	((1 << (ti_wid + 7)) - 1)

/* FPE Global defines */
#define FPE_AFSZ_MAX			0x3	/* Max AFSZ */
#define FPE_ADV_MAX			0xFFFF	/* Max Release/Hold advance */
#define FPE_PMAC_BIT			0x01	/* pMAC bit in GC entry */

/* TBS Global defines */
#define TBS_LEOS_MAX			999999999 /* Max LEOS (ns) */

/* DMA Tx Channel X Control register TBS bits defines */
#define DMA_CONTROL_EDSE		BIT(28)

int dwmac_set_tsn_hwtunable(struct net_device *ndev, u32 id,
			    const void *data);
int dwmac_get_tsn_hwtunable(struct net_device *ndev, u32 id,
			    void *data);
int dwmac_get_est_bank(struct net_device *ndev, u32 own);
int dwmac_set_est_gce(struct net_device *ndev,
		      struct est_gc_entry *gce, u32 row,
		      u32 dbgb, u32 dbgm);
int dwmac_get_est_gcrr_llr(struct net_device *ndev, u32 *gcl_len,
			   u32 dbgb, u32 dbgm);
int dwmac_set_est_gcrr_llr(struct net_device *ndev, u32 gcl_len,
			   u32 dbgb, u32 dbgm);
int dwmac_set_est_gcrr_times(struct net_device *ndev,
			     struct est_gcrr *gcrr,
			     u32 dbgb, u32 dbgm);
int dwmac_set_est_enable(struct net_device *ndev, bool enable);
int dwmac_get_est_gcc(struct net_device *ndev,
		      struct est_gc_config **gcc, bool frmdrv);
int dwmac_est_irq_status(struct net_device *ndev);
int dwmac_get_est_err_stat(struct net_device *ndev,
			   struct tsn_err_stat **err_stat);
int dwmac_set_fpe_config(struct net_device *ndev, struct fpe_config *fpec);
int dwmac_set_fpe_enable(struct net_device *ndev, bool enable);
int dwmac_get_fpe_config(struct net_device *ndev, struct fpe_config **fpec,
			 bool frmdrv);
int dwmac_get_fpe_pmac_sts(struct net_device *ndev, u32 *hrs);
int dwmac_fpe_irq_status(struct net_device *ndev);
int dwmac_reconfigure_cbs(struct net_device *ndev);
#endif /* __DWMAC5_H__ */
