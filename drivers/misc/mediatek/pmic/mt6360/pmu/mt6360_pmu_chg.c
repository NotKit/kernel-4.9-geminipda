/*
 *  drivers/misc/mediatek/pmic/mt6360/mt6360_pmu_chg.c
 *  Driver for MT6360 PMU CHG part
 *
 *  Copyright (C) 2018 Mediatek Technology Inc.
 *  cy_huang <cy_huang@richtek.com>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 2 as
 *  published by the Free Software Foundation.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 *  See http://www.gnu.org/licenses/gpl-2.0.html for more details.
 */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/platform_device.h>
#include <linux/pm.h>
#include <linux/delay.h>
#include <linux/power_supply.h>
#include <linux/iio/consumer.h>
#include <linux/slab.h>
#include <linux/version.h>
#include <linux/completion.h>
#include <linux/atomic.h>
#include <linux/wait.h>
#include <linux/kthread.h>

#include "../inc/mt6360_pmu.h"
#include "../inc/mt6360_pmu_chg.h"
#include <mt-plat/charger_class.h>
#include <mt-plat/charger_type.h>
/* add notify vbusov, eoc, rechg */
#include <mtk_charger_intf.h>
/* switch USB config */
#include <mt-plat/upmu_common.h>
#include <mt-plat/mtk_boot.h>

#define MT6360_PMU_CHG_DRV_VERSION	"1.0.1_MTK"

enum mt6360_adc_channel {
	MT6360_ADC_VBUSDIV5,
	MT6360_ADC_VSYS,
	MT6360_ADC_VBAT,
	MT6360_ADC_IBUS,
	MT6360_ADC_IBAT,
	MT6360_ADC_TEMP_JC,
	MT6360_ADC_MAX,
};

static const char * const mt6360_adc_chan_list[] = {
	"VBUSDIV5", "VSYS", "VBAT", "IBUS", "IBAT", "TEMP_JC",
};

struct mt6360_pmu_chg_info {
	struct device *dev;
	struct mt6360_pmu_info *mpi;
	struct iio_channel *channels[MT6360_ADC_MAX];
	struct power_supply *psy;
	struct charger_device *chg_dev;
	int hidden_mode_cnt;
	struct mutex hidden_mode_lock;
	struct mutex pe_lock;
	struct mutex aicr_lock;
	struct mutex tchg_lock;
	int tchg;
	u32 zcv;

	/* Charger type detection */
	struct mutex chgdet_lock;
	enum charger_type chg_type;
	bool pwr_rdy;
	bool bc12_en;
#ifdef CONFIG_TCPC_CLASS
	bool tcpc_attach;
#else
	struct work_struct chgdet_work;
#endif /* CONFIG_TCPC_CLASS */

	struct completion aicc_done;
	struct completion pumpx_done;
	atomic_t pe_complete;
	/* mivr */
	atomic_t mivr_cnt;
	wait_queue_head_t waitq;
	struct task_struct *mivr_task;
};

enum mt6360_iinlmtsel {
	MT6360_IINLMTSEL_AICR_3250 = 0,
	MT6360_IINLMTSEL_CHG_TYPE,
	MT6360_IINLMTSEL_AICR,
	MT6360_IINLMTSEL_LOWER_LEVEL,
};

enum mt6360_charging_status {
	MT6360_CHG_STATUS_READY = 0,
	MT6360_CHG_STATUS_PROGRESS,
	MT6360_CHG_STATUS_DONE,
	MT6360_CHG_STATUS_FAULT,
	MT6360_CHG_STATUS_MAX,
};

enum mt6360_usbsw_state {
	MT6360_USBSW_CHG = 0,
	MT6360_USBSW_USB,
};

enum mt6360_pmu_chg_type {
	MT6360_CHG_TYPE_NOVBUS = 0,
	MT6360_CHG_TYPE_UNDER_GOING,
	MT6360_CHG_TYPE_SDP,
	MT6360_CHG_TYPE_SDPNSTD,
	MT6360_CHG_TYPE_DCP,
	MT6360_CHG_TYPE_CDP,
	MT6360_CHG_TYPE_MAX,
};

static const char *mt6360_chg_status_name[MT6360_CHG_STATUS_MAX] = {
	"ready", "progress", "done", "fault",
};

static const struct mt6360_chg_platform_data def_platform_data = {
	.ichg = 2000000,		/* uA */
	.aicr = 500000,			/* uA */
	.mivr = 4400000,		/* uV */
	.cv = 4350000,			/* uA */
	.ieoc = 250000,			/* uA */
	.safety_timer = 12,		/* hour */
#ifdef CONFIG_MTK_BIF_SUPPORT
	.ircmp_resistor = 0,		/* uohm */
	.ircmp_vclamp = 0,		/* uV */
#else
	.ircmp_resistor = 25000,	/* uohm */
	.ircmp_vclamp = 32000,		/* uV */
#endif
	.en_te = true,
	.en_wdt = true,
	.chg_name = "primary_chg"
};

/* ================== */
/* Internal Functions */
/* ================== */
static int mt6360_enable_hidden_mode(struct mt6360_pmu_chg_info *mpci, bool en)
{
	static const u8 pascode[] = { 0x69, 0x96, 0x63, 0x72, };
	int ret = 0;

	mutex_lock(&mpci->hidden_mode_lock);
	if (en) {
		if (mpci->hidden_mode_cnt == 0) {
			ret = mt6360_pmu_reg_block_write(mpci->mpi,
					   MT6360_PMU_TM_PAS_CODE1, 4, pascode);
			if (ret < 0)
				goto err;
			mpci->hidden_mode_cnt++;
		}
	} else {
		if (mpci->hidden_mode_cnt == 1) {
			ret = mt6360_pmu_reg_write(mpci->mpi,
						 MT6360_PMU_TM_PAS_CODE1, 0x00);
			if (ret < 0)
				goto err;
			mpci->hidden_mode_cnt--;
		}
	}
	dev_dbg(mpci->dev, "%s: en = %d\n", __func__, en);
	goto out;
err:
	dev_err(mpci->dev, "%s failed, en = %d\n", __func__, en);
out:
	mutex_unlock(&mpci->hidden_mode_lock);
	return ret;
}

static inline u32 mt6360_trans_ichg_sel(u32 uA)
{
	u32 data = 0;

	if (uA >= 100000)
		data = (uA - 100000) / 100000;
	if (data > MT6360_ICHG_MAXVAL)
		data = MT6360_ICHG_MAXVAL;
	return data;
}

static inline u32 mt6360_trans_aicr_sel(u32 uA)
{
	u32 data = 0;

	if (uA >= 100000)
		data = (uA - 100000) / 50000;
	if (data > MT6360_AICR_MAXVAL)
		data = MT6360_AICR_MAXVAL;
	return data;
}

static inline u32 mt6360_trans_mivr_sel(u32 uV)
{
	u32 data = 0;

	if (uV >= 3900000)
		data = (uV - 3900000) / 100000;
	if (data > MT6360_MIVR_MAXVAL)
		data = MT6360_MIVR_MAXVAL;
	return data;
}

static inline u32 mt6360_trans_cv_sel(u32 uV)
{
	u32 data = 0;

	if (uV >= 3900000)
		data = (uV - 3900000) / 10000;
	if (data > MT6360_VOREG_MAXVAL)
		data = MT6360_VOREG_MAXVAL;
	return data;
}

static inline u32 mt6360_trans_ieoc_sel(u32 uA)
{
	u32 data = 0;

	if (uA >= 100000)
		data = (uA - 100000) / 50000;
	if (data > MT6360_IEOC_MAXVAL)
		data = MT6360_IEOC_MAXVAL;
	return data;
}

static inline u32 mt6360_trans_ircmp_r_sel(u32 uohm)
{
	u32 data = 0;

	data = uohm / 25000;
	if (data > MT6360_BAT_COMP_MAXVAL)
		data = MT6360_BAT_COMP_MAXVAL;
	return data;
}

static inline u32 mt6360_trans_ircmp_vclamp_sel(u32 uV)
{
	u32 data = 0;

	data = uV / 32000;
	if (data > MT6360_VCLAMP_MAXVAL)
		data = MT6360_VCLAMP_MAXVAL;
	return data;
}

static inline int mt6360_get_mivr(struct mt6360_pmu_chg_info *mpci, u32 *uV)
{
	int ret = 0;

	ret = mt6360_pmu_reg_read(mpci->mpi, MT6360_PMU_CHG_CTRL6);
	if (ret < 0)
		return ret;
	ret = (ret & MT6360_MASK_MIVR) >> MT6360_SHFT_MIVR;
	*uV = 3900000 + (ret * 100000);
	return 0;
}

static inline int mt6360_get_ieoc(struct mt6360_pmu_chg_info *mpci, u32 *uA)
{
	int ret = 0;

	ret = mt6360_pmu_reg_read(mpci->mpi, MT6360_PMU_CHG_CTRL9);
	if (ret < 0)
		return ret;
	ret = (ret & MT6360_MASK_IEOC) >> MT6360_SHFT_IEOC;
	*uA = 100000 + (ret * 50000);
	return ret;
}

static inline int mt6360_get_charging_status(
					struct mt6360_pmu_chg_info *mpci,
					enum mt6360_charging_status *chg_stat)
{
	int ret = 0;

	ret = mt6360_pmu_reg_read(mpci->mpi, MT6360_PMU_CHG_STAT);
	if (ret < 0)
		return ret;
	*chg_stat = (ret & MT6360_MASK_CHG_STAT) >> MT6360_SHFT_CHG_STAT;
	return 0;
}

static inline int mt6360_is_charger_enabled(struct mt6360_pmu_chg_info *mpci,
					    bool *en)
{
	int ret = 0;

	ret = mt6360_pmu_reg_read(mpci->mpi, MT6360_PMU_CHG_CTRL2);
	if (ret < 0)
		return ret;
	*en = (ret & MT6360_MASK_CHG_EN) ? true : false;
	return 0;
}

static inline int mt6360_select_input_current_limit(
		struct mt6360_pmu_chg_info *mpci, enum mt6360_iinlmtsel sel)
{
	dev_dbg(mpci->dev,
		"%s: select input current limit = %d\n", __func__, sel);
	return mt6360_pmu_reg_update_bits(mpci->mpi,
					  MT6360_PMU_CHG_CTRL2,
					  MT6360_MASK_IINLMTSEL,
					  sel << MT6360_SHFT_IINLMTSEL);
}

static int mt6360_enable_wdt(struct mt6360_pmu_chg_info *mpci, bool en)
{
	struct mt6360_chg_platform_data *pdata = dev_get_platdata(mpci->dev);

	dev_dbg(mpci->dev, "%s enable wdt, en = %d\n", __func__, en);
	if (!pdata->en_wdt)
		return 0;
	return mt6360_pmu_reg_update_bits(mpci->mpi,
					  MT6360_PMU_CHG_CTRL13,
					  MT6360_MASK_CHG_WDT_EN,
					  en ? 0xff : 0);
}

static inline int mt6360_get_chrdet_ext_stat(struct mt6360_pmu_chg_info *mpci,
					  bool *pwr_rdy)
{
	int ret = 0;

	ret = mt6360_pmu_reg_read(mpci->mpi, MT6360_PMU_FOD_STAT);
	if (ret < 0)
		return ret;
	*pwr_rdy = (ret & BIT(4)) ? true : false;
	return 0;
}

#ifdef CONFIG_MT6360_PMU_CHARGER_TYPE_DETECT
static int mt6360_psy_online_changed(struct mt6360_pmu_chg_info *mpci)
{
	int ret = 0;
#if 1 /* (LINUX_VERSION_CODE >= KERNEL_VERSION(4, 1, 0)) */
	union power_supply_propval propval;

	propval.intval = mpci->pwr_rdy;
	ret = power_supply_set_property(mpci->psy, POWER_SUPPLY_PROP_ONLINE,
					&propval);
	if (ret < 0)
		dev_err(mpci->dev, "%s: psy online fail(%d)\n", __func__, ret);
	else
		dev_info(mpci->dev,
			 "%s: pwr_rdy = %d\n",  __func__, mpci->pwr_rdy);
#endif
	return ret;
}

static int mt6360_psy_chg_type_changed(struct mt6360_pmu_chg_info *mpci)
{
	int ret = 0;
#if 1 /* (LINUX_VERSION_CODE >= KERNEL_VERSION(4, 1, 0)) */
	union power_supply_propval propval;

	propval.intval = mpci->chg_type;
	ret = power_supply_set_property(mpci->psy,
					POWER_SUPPLY_PROP_CHARGE_TYPE,
					&propval);
	if (ret < 0)
		dev_err(mpci->dev,
			"%s: psy type failed, ret = %d\n", __func__, ret);
	else
		dev_info(mpci->dev,
			 "%s: chg_type = %d\n", __func__, mpci->chg_type);
#endif
	return ret;
}
#endif /* CONFIG_MT6360_PMU_CHARGER_TYPE_DETECT */

static int mt6360_set_usbsw_state(struct mt6360_pmu_chg_info *mpci, int state)
{
	dev_info(mpci->dev, "%s: state = %d\n", __func__, state);

	/* Switch D+D- to AP/MT6360 */
	if (state == MT6360_USBSW_CHG)
		Charger_Detect_Init();
	else
		Charger_Detect_Release();

	return 0;
}

static int __mt6360_enable_usbchgen(struct mt6360_pmu_chg_info *mpci, bool en)
{
	int i, ret = 0;
	const int max_wait_cnt = 200;
	bool pwr_rdy = false;
	enum mt6360_usbsw_state usbsw =
				       en ? MT6360_USBSW_CHG : MT6360_USBSW_USB;

	dev_info(mpci->dev, "%s: en = %d\n", __func__, en);
	if (en) {
		/* Workaround for CDP port */
		for (i = 0; i < max_wait_cnt; i++) {
			if (is_usb_rdy())
				break;
			dev_info(mpci->dev, "%s: CDP block\n", __func__);
			/* Check vbus */
			ret = mt6360_get_chrdet_ext_stat(mpci, &pwr_rdy);
			if (ret < 0) {
				dev_err(mpci->dev, "%s: fail, ret = %d\n",
					 __func__, ret);
				return ret;
			}
			dev_info(mpci->dev, "%s: pwr_rdy = %d\n", __func__,
				 pwr_rdy);
			if (!pwr_rdy) {
				dev_info(mpci->dev, "%s: plug out\n", __func__);
				return ret;
			}
			msleep(100);
		}
		if (i == max_wait_cnt)
			dev_err(mpci->dev, "%s: CDP timeout\n", __func__);
		else
			dev_info(mpci->dev, "%s: CDP free\n", __func__);
	}
	mt6360_set_usbsw_state(mpci, usbsw);
	ret = mt6360_pmu_reg_update_bits(mpci->mpi, MT6360_PMU_DEVICE_TYPE,
					 MT6360_MASK_USBCHGEN, en ? 0xff : 0);
	if (ret >= 0)
		mpci->bc12_en = en;
	return ret;
}

static int mt6360_enable_usbchgen(struct mt6360_pmu_chg_info *mpci, bool en)
{
	int ret = 0;

	mutex_lock(&mpci->chgdet_lock);
	ret = __mt6360_enable_usbchgen(mpci, en);
	mutex_unlock(&mpci->chgdet_lock);
	return ret;
}

#ifdef CONFIG_MT6360_PMU_CHARGER_TYPE_DETECT
static int mt6360_chgdet_pre_process(struct mt6360_pmu_chg_info *mpci,
				     bool attach)
{
	if (!attach) {
		mpci->chg_type = CHARGER_UNKNOWN;
		mt6360_psy_chg_type_changed(mpci);
	}
/* TODO : skip for meta mode condition */
#if 0
	if (attach && is_meta_mode()) {
		/* Skip charger type detection to speed up meta boot.*/
		dev_notice(mpci->dev, "%s: force Standard USB Host in meta\n",
			   __func__);
		mpci->pwr_rdy = true;
		mpci->chg_type = STANDARD_HOST;
		return mt6360_psy_chg_type_changed(mpci);
	}
#endif
	return __mt6360_enable_usbchgen(mpci, attach);
}

static int mt6360_chgdet_post_process(struct mt6360_pmu_chg_info *mpci)
{
	int ret = 0;
	bool attach = false;
	u8 usb_status = CHARGER_UNKNOWN;

#ifdef CONFIG_TCPC_CLASS
	attach = mpci->tcpc_attach;
#else
	attach = mpci->pwr_rdy;
#endif /* CONFIG_TCPC_CLASS */
	dev_info(mpci->dev, "%s: attach = %d\n", __func__, attach);
	/* Plug out during BC12 */
	if (!attach) {
		mpci->chg_type = CHARGER_UNKNOWN;
		goto out;
	}
	/* Plug in */
	ret = mt6360_pmu_reg_read(mpci->mpi, MT6360_PMU_USB_STATUS1);
	if (ret < 0)
		goto out;
	usb_status = (ret & MT6360_MASK_USB_STATUS) >> MT6360_SHFT_USB_STATUS;
	switch (usb_status) {
	case MT6360_CHG_TYPE_UNDER_GOING:
		dev_info(mpci->dev, "%s: under going...\n", __func__);
		return ret;
	case MT6360_CHG_TYPE_SDP:
		mpci->chg_type = STANDARD_HOST;
		break;
	case MT6360_CHG_TYPE_SDPNSTD:
		mpci->chg_type = NONSTANDARD_CHARGER;
		break;
	case MT6360_CHG_TYPE_CDP:
		mpci->chg_type = CHARGING_HOST;
		break;
	case MT6360_CHG_TYPE_DCP:
		mpci->chg_type = STANDARD_CHARGER;
		break;
	}
out:
	ret = __mt6360_enable_usbchgen(mpci, false);
	if (ret < 0)
		dev_err(mpci->dev, "%s: disable chgdet fail\n", __func__);
	return mt6360_psy_chg_type_changed(mpci);
}
#endif /* CONFIG_MT6360_PMU_CHARGER_TYPE_DETECT */

static const u32 mt6360_vinovp_list[] = {
	5500000, 6500000, 10500000, 14500000,
};

static int mt6360_select_vinovp(struct mt6360_pmu_chg_info *mpci, u32 uV)
{
	int i;

	if (uV < mt6360_vinovp_list[0])
		return -EINVAL;
	for (i = 1; i < ARRAY_SIZE(mt6360_vinovp_list); i++) {
		if (uV < mt6360_vinovp_list[i])
			break;
	}
	i--;
	return mt6360_pmu_reg_update_bits(mpci->mpi,
					  MT6360_PMU_CHG_CTRL19,
					  MT6360_MASK_CHG_VIN_OVP_VTHSEL,
					  i << MT6360_SHFT_CHG_VIN_OVP_VTHSEL);
}

static inline int mt6360_read_zcv(struct mt6360_pmu_chg_info *mpci)
{
	int ret = 0;
	u8 zcv_data[2] = {0};

	dev_dbg(mpci->dev, "%s\n", __func__);
	/* Read ZCV data */
	ret = mt6360_pmu_reg_block_read(mpci->mpi, MT6360_PMU_ADC_BAT_DATA_H,
					2, zcv_data);
	if (ret < 0) {
		dev_err(mpci->dev, "%s: read zcv data fail\n", __func__);
		return ret;
	}
	mpci->zcv = 5000 * (zcv_data[0] * 256 + zcv_data[1]);
	dev_info(mpci->dev, "%s: zcv = (0x%02X, 0x%02X, %dmV)\n",
		 __func__, zcv_data[0], zcv_data[1], mpci->zcv/1000);
	/* Disable ZCV */
	ret = mt6360_pmu_reg_clr_bits(mpci->mpi, MT6360_PMU_ADC_CONFIG,
				      MT6360_MASK_ZCV_EN);
	if (ret < 0)
		dev_err(mpci->dev, "%s: disable zcv fail\n", __func__);
	return ret;
}

/* ================== */
/* External Functions */
/* ================== */
static int mt6360_charger_enable(struct charger_device *chg_dev, bool en)
{
	struct mt6360_pmu_chg_info *mpci = charger_get_data(chg_dev);

	dev_dbg(mpci->dev, "%s: en = %d\n", __func__, en);
	return mt6360_pmu_reg_update_bits(mpci->mpi, MT6360_PMU_CHG_CTRL2,
					  MT6360_MASK_CHG_EN, en ? 0xff : 0);
}

static int mt6360_charger_set_ichg(struct charger_device *chg_dev, u32 uA)
{
	struct mt6360_pmu_chg_info *mpci = charger_get_data(chg_dev);
	u32 data = 0;

	dev_dbg(mpci->dev, "%s\n", __func__);
	data = mt6360_trans_ichg_sel(uA);
	return mt6360_pmu_reg_update_bits(mpci->mpi,
					  MT6360_PMU_CHG_CTRL7,
					  MT6360_MASK_ICHG,
					  data << MT6360_SHFT_ICHG);
}

static int mt6360_charger_get_ichg(struct charger_device *chg_dev, u32 *uA)
{
	struct mt6360_pmu_chg_info *mpci = charger_get_data(chg_dev);
	int ret = 0;

	ret = mt6360_pmu_reg_read(mpci->mpi, MT6360_PMU_CHG_CTRL7);
	if (ret < 0)
		return ret;
	ret = (ret & MT6360_MASK_ICHG) >> MT6360_SHFT_ICHG;
	*uA = 100000 + (ret * 100000);
	return 0;
}

static int mt6360_charger_get_min_ichg(struct charger_device *chg_dev, u32 *uA)
{
	*uA = 300000;
	return 0;
}

static int mt6360_charger_set_cv(struct charger_device *chg_dev, u32 uV)
{
	struct mt6360_pmu_chg_info *mpci = charger_get_data(chg_dev);
	u8 data = 0;

	dev_dbg(mpci->dev, "%s\n", __func__);
	data = mt6360_trans_cv_sel(uV);
	return mt6360_pmu_reg_update_bits(mpci->mpi,
					  MT6360_PMU_CHG_CTRL4,
					  MT6360_MASK_VOREG,
					  data << MT6360_SHFT_VOREG);
}

static int mt6360_charger_get_cv(struct charger_device *chg_dev, u32 *uV)
{
	struct mt6360_pmu_chg_info *mpci = charger_get_data(chg_dev);
	int ret = 0;

	ret = mt6360_pmu_reg_read(mpci->mpi, MT6360_PMU_CHG_CTRL4);
	if (ret < 0)
		return ret;
	ret = (ret & MT6360_MASK_VOREG) >> MT6360_SHFT_VOREG;
	*uV = 3900000 + (ret * 10000);
	return 0;
}

static int mt6360_toggle_aicc(struct mt6360_pmu_chg_info *mpci)
{
	int ret = 0;
	u8 data = 0;

	mutex_lock(&mpci->mpi->io_lock);
	/* read aicc */
	ret = i2c_smbus_read_i2c_block_data(mpci->mpi->i2c,
					       MT6360_PMU_CHG_CTRL14, 1, &data);
	if (ret < 0) {
		dev_err(mpci->dev, "%s: read aicc fail\n", __func__);
		goto out;
	}
	/* aicc off */
	data &= ~MT6360_MASK_RG_EN_AICC;
	ret = i2c_smbus_read_i2c_block_data(mpci->mpi->i2c,
					       MT6360_PMU_CHG_CTRL14, 1, &data);
	if (ret < 0) {
		dev_err(mpci->dev, "%s: aicc off fail\n", __func__);
		goto out;
	}
	/* aicc on */
	data |= MT6360_MASK_RG_EN_AICC;
	ret = i2c_smbus_read_i2c_block_data(mpci->mpi->i2c,
					       MT6360_PMU_CHG_CTRL14, 1, &data);
	if (ret < 0)
		dev_err(mpci->dev, "%s: aicc on fail\n", __func__);
out:
	mutex_unlock(&mpci->mpi->io_lock);
	return ret;
}

static int mt6360_charger_set_aicr(struct charger_device *chg_dev, u32 uA)
{
	struct mt6360_pmu_chg_info *mpci = charger_get_data(chg_dev);
	int ret = 0;
	u8 data = 0;

	dev_dbg(mpci->dev, "%s\n", __func__);
	/* Toggle aicc for auto aicc mode */
	if (!mpci->aicc_once) {
		ret = mt6360_toggle_aicc(mpci);
		if (ret < 0) {
			dev_err(mpci->dev, "%s: toggle aicc fail\n", __func__);
			return ret;
		}
	}
	/* Disable sys drop improvement for download mode */
	ret = mt6360_pmu_reg_clr_bits(mpci->mpi, MT6360_PMU_CHG_CTRL20,
				      MT6360_MASK_EN_SDI);
	if (ret < 0) {
		dev_err(mpci->dev, "%s: disable en_sdi fail\n", __func__);
		return ret;
	}
	data = mt6360_trans_aicr_sel(uA);
	return mt6360_pmu_reg_update_bits(mpci->mpi,
					  MT6360_PMU_CHG_CTRL3,
					  MT6360_MASK_AICR,
					  data << MT6360_SHFT_AICR);
}

static int mt6360_charger_get_aicr(struct charger_device *chg_dev, u32 *uA)
{
	struct mt6360_pmu_chg_info *mpci = charger_get_data(chg_dev);
	int ret = 0;

	ret = mt6360_pmu_reg_read(mpci->mpi, MT6360_PMU_CHG_CTRL3);
	if (ret < 0)
		return ret;
	ret = (ret & MT6360_MASK_AICR) >> MT6360_SHFT_AICR;
	*uA = 100000 + (ret * 50000);
	return 0;
}

static int mt6360_charger_get_min_aicr(struct charger_device *chg_dev, u32 *uA)
{
	*uA = 100000;
	return 0;
}

static int mt6360_charger_set_ieoc(struct charger_device *chg_dev, u32 uA)
{
	struct mt6360_pmu_chg_info *mpci = charger_get_data(chg_dev);
	u8 data = 0;

	dev_dbg(mpci->dev, "%s\n", __func__);
	data = mt6360_trans_ieoc_sel(uA);
	return mt6360_pmu_reg_update_bits(mpci->mpi,
					  MT6360_PMU_CHG_CTRL9,
					  MT6360_MASK_IEOC,
					  data << MT6360_SHFT_IEOC);
}

static int mt6360_charger_set_mivr(struct charger_device *chg_dev, u32 uV)
{
	struct mt6360_pmu_chg_info *mpci = charger_get_data(chg_dev);
	u32 aicc_vth = 0, data = 0;
	u8 aicc_vth_sel = 0;
	int ret = 0;

	dev_dbg(mpci->dev, "%s\n", __func__);
	if (uV < 3900000 || uV > 1340000) {
		dev_err(mpci->dev,
			"%s: unsuitable mivr val(%d)\n", __func__, uV);
		return -EINVAL;
	}
	/* Check if there's a suitable AICC_VTH */
	aicc_vth = uV + 200000;
	aicc_vth_sel = (aicc_vth - 3900000) / 100000;
	if (aicc_vth_sel > MT6360_AICC_VTH_MAXVAL) {
		dev_err(mpci->dev, "%s: can't match, aicc_vth_sel = %d\n",
			__func__, aicc_vth_sel);
		return -EINVAL;
	}
	/* Set AICC_VTH threshold */
	ret = mt6360_pmu_reg_update_bits(mpci->mpi,
					 MT6360_PMU_CHG_CTRL16,
					 MT6360_MASK_AICC_VTH,
					 aicc_vth_sel << MT6360_SHFT_AICC_VTH);
	if (ret < 0)
		return ret;
	/* Set MIVR */
	data = mt6360_trans_mivr_sel(uV);
	return mt6360_pmu_reg_update_bits(mpci->mpi,
					  MT6360_PMU_CHG_CTRL6,
					  MT6360_MASK_MIVR,
					  data << MT6360_SHFT_MIVR);
}

static int mt6360_charger_enable_te(struct charger_device *chg_dev, bool en)
{
	struct mt6360_pmu_chg_info *mpci = charger_get_data(chg_dev);
	struct mt6360_chg_platform_data *pdata = dev_get_platdata(mpci->dev);

	dev_info(mpci->dev, "%s: en = %d\n", __func__, en);
	if (!pdata->en_te)
		return 0;
	return mt6360_pmu_reg_update_bits(mpci->mpi, MT6360_PMU_CHG_CTRL2,
					  MT6360_MASK_TE_EN, en ? 0xff : 0);
}

static int mt6360_enable_pump_express(struct mt6360_pmu_chg_info *mpci,
				      bool pe20)
{
	long timeout, pe_timeout = pe20 ? 1400 : 2800;
	int ret = 0;

	dev_info(mpci->dev, "%s\n", __func__);
	ret = mt6360_charger_set_aicr(mpci->chg_dev, 800000);
	if (ret < 0)
		return ret;
	ret = mt6360_charger_set_ichg(mpci->chg_dev, 2000000);
	if (ret < 0)
		return ret;
	ret = mt6360_charger_enable(mpci->chg_dev, true);
	if (ret < 0)
		return ret;
	ret = mt6360_pmu_reg_clr_bits(mpci->mpi, MT6360_PMU_CHG_CTRL17,
				      MT6360_MASK_EN_PUMPX);
	if (ret < 0)
		return ret;
	ret = mt6360_pmu_reg_set_bits(mpci->mpi, MT6360_PMU_CHG_CTRL17,
				      MT6360_MASK_EN_PUMPX);
	if (ret < 0)
		return ret;
	reinit_completion(&mpci->pumpx_done);
	atomic_set(&mpci->pe_complete, 1);
	timeout = wait_for_completion_interruptible_timeout(
			       &mpci->pumpx_done, msecs_to_jiffies(pe_timeout));
	if (timeout == 0)
		ret = -ETIMEDOUT;
	else if (timeout < 0)
		ret = -EINTR;
	if (ret < 0)
		dev_err(mpci->dev,
			"%s: wait pumpx timeout, ret = %d\n", __func__, ret);
	return ret;
}

static int mt6360_set_pep_current_pattern(struct charger_device *chg_dev,
					  bool is_inc)
{
	struct mt6360_pmu_chg_info *mpci = charger_get_data(chg_dev);
	int ret = 0;

	dev_dbg(mpci->dev, "%s\n", __func__);

	mutex_lock(&mpci->pe_lock);
	/* Set to PE1.0 */
	ret = mt6360_pmu_reg_clr_bits(mpci->mpi,
				      MT6360_PMU_CHG_CTRL17,
				      MT6360_MASK_PUMPX_20_10);
	if (ret < 0) {
		dev_err(mpci->dev, "%s: enable pumpx 10 fail\n", __func__);
		goto out;
	}

	/* Set Pump Up/Down */
	ret = mt6360_pmu_reg_update_bits(mpci->mpi,
					 MT6360_PMU_CHG_CTRL17,
					 MT6360_MASK_PUMPX_UP_DN,
					 is_inc ? 0xff : 0);
	if (ret < 0) {
		dev_err(mpci->dev, "%s: set pumpx up/down fail\n", __func__);
		goto out;
	}
	ret = mt6360_enable_pump_express(mpci, false);
out:
	mutex_unlock(&mpci->pe_lock);
	return ret;
}

static int mt6360_set_pep20_efficiency_table(struct charger_device *chg_dev)
{
	struct charger_manager *chg_mgr = NULL;

	chg_mgr = charger_dev_get_drvdata(chg_dev);
	if (!chg_mgr)
		return -EINVAL;

	chg_mgr->pe2.profile[0].vchr = 8000000;
	chg_mgr->pe2.profile[1].vchr = 8000000;
	chg_mgr->pe2.profile[2].vchr = 8000000;
	chg_mgr->pe2.profile[3].vchr = 8500000;
	chg_mgr->pe2.profile[4].vchr = 8500000;
	chg_mgr->pe2.profile[5].vchr = 8500000;
	chg_mgr->pe2.profile[6].vchr = 9000000;
	chg_mgr->pe2.profile[7].vchr = 9000000;
	chg_mgr->pe2.profile[8].vchr = 9500000;
	chg_mgr->pe2.profile[9].vchr = 9500000;
	return 0;
}

static int mt6360_set_pep20_current_pattern(struct charger_device *chg_dev,
					    u32 uV)
{
	struct mt6360_pmu_chg_info *mpci = charger_get_data(chg_dev);
	int ret = 0;
	u8 data = 0;

	dev_dbg(mpci->dev, "%s: vol = %d\n", __func__, uV);
	mutex_lock(&mpci->pe_lock);
	if (uV >= 5500000)
		data = (uV - 5500000) / 500000;
	if (data > MT6360_PUMPX_20_MAXVAL)
		data = MT6360_PUMPX_20_MAXVAL;
	/* Set to PE2.0 */
	ret = mt6360_pmu_reg_set_bits(mpci->mpi, MT6360_PMU_CHG_CTRL17,
				      MT6360_MASK_PUMPX_20_10);
	if (ret < 0) {
		dev_err(mpci->dev, "%s: enable pumpx 20 fail\n", __func__);
		goto out;
	}
	/* Set Voltage */
	ret = mt6360_pmu_reg_update_bits(mpci->mpi,
					 MT6360_PMU_CHG_CTRL17,
					 MT6360_MASK_PUMPX_DEC,
					 data << MT6360_SHFT_PUMPX_DEC);
	if (ret < 0) {
		dev_err(mpci->dev, "%s: set pumpx voltage fail\n", __func__);
		goto out;
	}
	ret = mt6360_enable_pump_express(mpci, true);
out:
	mutex_unlock(&mpci->pe_lock);
	return ret;
}

static int mt6360_reset_ta(struct charger_device *chg_dev)
{
	struct mt6360_pmu_chg_info *mpci = charger_get_data(chg_dev);
	int ret = 0;

	dev_dbg(mpci->dev, "%s\n", __func__);
	ret = mt6360_charger_set_mivr(chg_dev, 4600000);
	if (ret < 0)
		return ret;
	ret = mt6360_select_input_current_limit(mpci, MT6360_IINLMTSEL_AICR);
	if (ret < 0)
		return ret;
	ret = mt6360_charger_set_aicr(chg_dev, 100000);
	if (ret < 0)
		return ret;
	msleep(250);
	return mt6360_charger_set_aicr(chg_dev, 500000);
}

static int mt6360_enable_cable_drop_comp(struct charger_device *chg_dev,
					 bool en)
{
	struct mt6360_pmu_chg_info *mpci = charger_get_data(chg_dev);
	int ret = 0;

	if (en)
		return ret;

	/* Set to PE2.0 */
	mutex_lock(&mpci->pe_lock);
	ret = mt6360_pmu_reg_set_bits(mpci->mpi, MT6360_PMU_CHG_CTRL17,
				      MT6360_MASK_PUMPX_20_10);
	if (ret < 0) {
		dev_err(mpci->dev, "%s: enable pumpx 20 fail\n", __func__);
		goto out;
	}
	/* Disable cable drop compensation */
	ret = mt6360_pmu_reg_update_bits(mpci->mpi,
					 MT6360_PMU_CHG_CTRL17,
					 MT6360_MASK_PUMPX_DEC,
					 0x1F << MT6360_SHFT_PUMPX_DEC);
	if (ret < 0) {
		dev_err(mpci->dev, "%s: set pumpx voltage fail\n", __func__);
		goto out;
	}
	ret = mt6360_enable_pump_express(mpci, true);
out:
	mutex_unlock(&mpci->pe_lock);
	return ret;
}

static inline int mt6360_get_aicc(struct mt6360_pmu_chg_info *mpci,
				  u32 *aicc_val)
{
	u8 aicc_sel = 0;
	int ret = 0;

	ret = mt6360_pmu_reg_read(mpci->mpi, MT6360_PMU_CHG_AICC_RESULT);
	if (ret < 0) {
		dev_err(mpci->dev, "%s: read aicc result fail\n", __func__);
		return ret;
	}
	aicc_sel = (ret & MT6360_MASK_RG_AICC_RESULT) >>
						     MT6360_SHFT_RG_AICC_RESULT;
	*aicc_val = (aicc_sel * 50000) + 100000;
	return 0;
}

static int mt6360_charger_run_aicc(struct charger_device *chg_dev, u32 *uA)
{
	struct mt6360_pmu_chg_info *mpci = charger_get_data(chg_dev);
	struct mt6360_chg_platform_data *pdata = dev_get_platdata(mpci->dev);
	int ret = 0;
	u32 aicc_val = 0;
	long timeout;

	dev_info(mpci->dev, "%s: aicc_once = %d\n", __func__, pdata->aicc_once);
	if (pdata->aicc_once) {
		if (try_wait_for_completion(&mpci->aicc_done)) {
			dev_info(mpci->dev, "%s: aicc is not act\n", __func__);
			return 0;
		}

		/* get aicc result */
		ret = mt6360_get_aicc(mpci, &aicc_val);
		if (ret < 0) {
			dev_err(mpci->dev,
				"%s: get aicc fail\n", __func__);
			return ret;
		}
		*uA = aicc_val;
		reinit_completion(&mpci->aicc_done);
		return ret;
	}

	/* Use aicc once method */
	/* Run AICC measure */
	mutex_lock(&mpci->pe_lock);
	ret = mt6360_pmu_reg_set_bits(mpci->mpi, MT6360_PMU_CHG_CTRL14,
				      MT6360_MASK_RG_EN_AICC);
	if (ret < 0)
		goto out;
	/* Clear AICC measurement IRQ */
	reinit_completion(&mpci->aicc_done);
	timeout = wait_for_completion_interruptible_timeout(
				   &mpci->aicc_done, msecs_to_jiffies(3000));
	if (timeout == 0)
		ret = -ETIMEDOUT;
	else if (timeout < 0)
		ret = -EINTR;
	if (ret < 0) {
		dev_err(mpci->dev,
			"%s: wait AICC time out, ret = %d\n", __func__, ret);
		goto out;
	}
	/* get aicc_result */
	ret = mt6360_get_aicc(mpci, &aicc_val);
	if (ret < 0) {
		dev_err(mpci->dev, "%s: get aicc result fail\n", __func__);
		goto out;
	} else
		dev_info(mpci->dev, "%s: aicc val = %d\n", __func__, aicc_val);
	/* Clear EN_AICC */
	ret = mt6360_pmu_reg_clr_bits(mpci->mpi, MT6360_PMU_CHG_CTRL14,
				      MT6360_MASK_RG_EN_AICC);
	if (ret < 0)
		goto out;
	*uA = aicc_val;
out:
	mutex_unlock(&mpci->pe_lock);
	return ret;
}

static int mt6360_charger_enable_power_path(struct charger_device *chg_dev,
					    bool en)
{
	struct mt6360_pmu_chg_info *mpci = charger_get_data(chg_dev);

	dev_dbg(mpci->dev, "%s\n", __func__);
	return mt6360_pmu_reg_update_bits(mpci->mpi, MT6360_PMU_CHG_CTRL1,
					MT6360_MASK_FORCE_SLEEP, en ? 0 : 0xff);
}

static int mt6360_charger_is_power_path_enabled(struct charger_device *chg_dev,
						bool *en)
{
	struct mt6360_pmu_chg_info *mpci = charger_get_data(chg_dev);
	int ret = 0;

	ret = mt6360_pmu_reg_read(mpci->mpi, MT6360_PMU_CHG_CTRL1);
	if (ret < 0)
		return ret;
	*en = (ret & MT6360_MASK_FORCE_SLEEP) ? false : true;
	return 0;
}

static int mt6360_charger_enable_safety_timer(struct charger_device *chg_dev,
					      bool en)
{
	struct mt6360_pmu_chg_info *mpci = charger_get_data(chg_dev);

	dev_dbg(mpci->dev, "%s: en = %d\n", __func__, en);
	return mt6360_pmu_reg_update_bits(mpci->mpi, MT6360_PMU_CHG_CTRL12,
					  MT6360_MASK_TMR_EN, en ? 0xff : 0);
}

static int mt6360_charger_is_safety_timer_enabled(
				struct charger_device *chg_dev, bool *en)
{
	struct mt6360_pmu_chg_info *mpci = charger_get_data(chg_dev);
	int ret = 0;

	ret = mt6360_pmu_reg_read(mpci->mpi, MT6360_PMU_CHG_CTRL12);
	if (ret < 0)
		return ret;
	*en = (ret & MT6360_MASK_TMR_EN) ? true : false;
	return 0;
}

static const u32 otg_oc_table[] = {
	500000, 700000, 1100000, 1300000, 1800000, 2100000, 2400000, 3000000
};

static int mt6360_charger_set_otg_current_limit(struct charger_device *chg_dev,
						u32 uA)
{
	struct mt6360_pmu_chg_info *mpci = charger_get_data(chg_dev);
	int i;

	/* Set higher OC threshold protect */
	for (i = 0; i < ARRAY_SIZE(otg_oc_table); i++) {
		if (uA <= otg_oc_table[i])
			break;
	}
	if (i == ARRAY_SIZE(otg_oc_table))
		i = MT6360_OTG_OC_MAXVAL;
	dev_dbg(mpci->dev,
		"%s: select oc threshold = %d\n", __func__, otg_oc_table[i]);

	return mt6360_pmu_reg_update_bits(mpci->mpi,
					  MT6360_PMU_CHG_CTRL10,
					  MT6360_MASK_OTG_OC,
					  i << MT6360_SHFT_OTG_OC);
}

static int mt6360_charger_enable_otg(struct charger_device *chg_dev, bool en)
{
	struct mt6360_pmu_chg_info *mpci = charger_get_data(chg_dev);
	int ret = 0;

	dev_dbg(mpci->dev, "%s: en = %d\n", __func__, en);
	ret = mt6360_enable_wdt(mpci, en ? true : false);
	if (ret < 0) {
		dev_err(mpci->dev, "%s: set wdt fail, en = %d\n", __func__, en);
		return ret;
	}
	return mt6360_pmu_reg_update_bits(mpci->mpi, MT6360_PMU_CHG_CTRL1,
					  MT6360_MASK_OPA_MODE, en ? 0xff : 0);
}

static int mt6360_charger_enable_discharge(struct charger_device *chg_dev,
					   bool en)
{
	struct mt6360_pmu_chg_info *mpci = charger_get_data(chg_dev);
	int i, ret = 0;
	const int dischg_retry_cnt = 3;
	bool is_dischg;

	dev_dbg(mpci->dev, "%s\n", __func__);
	ret = mt6360_enable_hidden_mode(mpci, true);
	if (ret < 0)
		return ret;
	/* Set bit2 of reg[0x31] to 1/0 to enable/disable discharging */
	ret = mt6360_pmu_reg_update_bits(mpci->mpi, MT6360_PMU_CHG_HIDDEN_CTRL2,
					 MT6360_MASK_DISCHG, en ? 0xff : 0);
	if (ret < 0) {
		dev_err(mpci->dev, "%s: fail, en = %d\n", __func__, en);
		goto out;
	}

	for (i = 0; i < dischg_retry_cnt; i++) {
		ret = mt6360_pmu_reg_read(mpci->mpi,
					  MT6360_PMU_CHG_HIDDEN_CTRL2);
		is_dischg = (ret & MT6360_MASK_DISCHG) ? true : false;
		if (!is_dischg)
			break;
		ret = mt6360_pmu_reg_clr_bits(mpci->mpi,
					      MT6360_PMU_CHG_HIDDEN_CTRL2,
					      MT6360_MASK_DISCHG);
		if (ret < 0) {
			dev_err(mpci->dev,
				"%s: disable dischg failed\n", __func__);
			goto out;
		}
	}
	if (i == dischg_retry_cnt) {
		dev_err(mpci->dev, "%s: dischg failed\n", __func__);
		ret = -EINVAL;
	}
out:
	mt6360_enable_hidden_mode(mpci, false);
	return ret;
}

static int mt6360_enable_chg_type_det(struct charger_device *chg_dev, bool en)
{
	int ret = 0;
#if defined(CONFIG_MT6360_PMU_CHARGER_TYPE_DETECT) && defined(CONFIG_TCPC_CLASS)
	struct mt6360_pmu_chg_info *mpci = charger_get_data(chg_dev);

	dev_info(mpci->dev, "%s\n", __func__);
	mutex_lock(&mpci->chgdet_lock);
	if (mpci->tcpc_attach == en) {
		dev_info(mpci->dev, "%s attach(%d) is the same\n",
			 __func__, mpci->tcpc_attach);
		goto out;
	}
	mpci->tcpc_attach = en;
	ret = mt6360_chgdet_pre_process(mpci, en);
out:
	mutex_unlock(&mpci->chgdet_lock);
#endif /* CONFIG_MT6360_PMU_CHARGER_TYPE_DETECT && CONFIG_TCPC_CLASS */
	return ret;
}

static int mt6360_charger_get_vbus(struct charger_device *chg_dev, u32 *vbus)
{
	struct mt6360_pmu_chg_info *mpci = charger_get_data(chg_dev);
	int ret = 0;

	dev_dbg(mpci->dev, "%s\n", __func__);
	ret = iio_read_channel_processed(mpci->channels[MT6360_ADC_VBUSDIV5],
					 vbus);
	if (ret < 0)
		return ret;
	return 0;
}

static int mt6360_charger_get_ibus(struct charger_device *chg_dev, u32 *ibus)
{
	struct mt6360_pmu_chg_info *mpci = charger_get_data(chg_dev);
	int ret = 0;

	dev_dbg(mpci->dev, "%s\n", __func__);
	ret = iio_read_channel_processed(mpci->channels[MT6360_ADC_IBUS], ibus);
	if (ret < 0)
		return ret;
	return 0;
}

static int mt6360_charger_get_tchg(struct charger_device *chg_dev,
				   int *tchg_min, int *tchg_max)
{
	struct mt6360_pmu_chg_info *mpci = charger_get_data(chg_dev);
	int temp_jc, ret = 0, retry_cnt = 3;

	dev_dbg(mpci->dev, "%s\n", __func__);
	/* temp abnormal Workaround */
	do {
		ret = iio_read_channel_processed(
				  mpci->channels[MT6360_ADC_TEMP_JC], &temp_jc);
		if (ret < 0) {
			dev_err(mpci->dev,
				"%s: failed, ret = %d\n", __func__, ret);
			return ret;
		}
	} while (temp_jc >= 120 && (retry_cnt--) > 0);
	mutex_lock(&mpci->tchg_lock);
	if (temp_jc >= 120)
		temp_jc = mpci->tchg;
	else
		mpci->tchg = temp_jc;
	mutex_unlock(&mpci->tchg_lock);
	*tchg_min = *tchg_max = temp_jc;
	dev_info(mpci->dev, "%s: tchg = %d\n", __func__, temp_jc);
	return 0;
}

static int mt6360_charger_kick_wdt(struct charger_device *chg_dev)
{
	struct mt6360_pmu_chg_info *mpci = charger_get_data(chg_dev);

	dev_dbg(mpci->dev, "%s\n", __func__);
	return mt6360_pmu_reg_read(mpci->mpi, MT6360_PMU_CHG_CTRL1);
}

static int mt6360_charger_safety_check(struct charger_device *chg_dev)
{
	struct mt6360_pmu_chg_info *mpci = charger_get_data(chg_dev);
	int ret, ibat = 0;
	static int eoc_cnt;

	dev_dbg(mpci->dev, "%s\n", __func__);
	ret = iio_read_channel_processed(mpci->channels[MT6360_ADC_IBAT],
					 &ibat);
	if (ret < 0)
		dev_err(mpci->dev, "%s: failed, ret = %d\n", __func__, ret);

	if (ibat <= 300000)
		eoc_cnt++;
	else
		eoc_cnt = 0;
	/* If ibat is less than 300mA for 3 times, trigger EOC event */
	if (eoc_cnt == 3) {
		dev_info(mpci->dev, "%s: ibat = %d\n", __func__, ibat);
		charger_dev_notify(mpci->chg_dev, CHARGER_DEV_NOTIFY_EOC);
		eoc_cnt = 0;
	}
	return ret;
}

static int mt6360_charger_reset_eoc_state(struct charger_device *chg_dev)
{
	struct mt6360_pmu_chg_info *mpci = charger_get_data(chg_dev);
	int ret = 0;

	dev_dbg(mpci->dev, "%s\n", __func__);

	ret = mt6360_enable_hidden_mode(mpci, true);
	if (ret < 0)
		return ret;
	ret = mt6360_pmu_reg_set_bits(mpci->mpi, MT6360_PMU_CHG_HIDDEN_CTRL1,
				      MT6360_MASK_EOC_RST);
	if (ret < 0) {
		dev_err(mpci->dev, "%s: set failed, ret = %d\n", __func__, ret);
		goto out;
	}
	udelay(100);
	ret = mt6360_pmu_reg_clr_bits(mpci->mpi, MT6360_PMU_CHG_HIDDEN_CTRL1,
				      MT6360_MASK_EOC_RST);
	if (ret < 0) {
		dev_err(mpci->dev,
			"%s: clear failed, ret = %d\n", __func__, ret);
		goto out;
	}
out:
	mt6360_enable_hidden_mode(mpci, false);
	return ret;
}

static int mt6360_charger_is_charging_done(struct charger_device *chg_dev,
					   bool *done)
{
	struct mt6360_pmu_chg_info *mpci = charger_get_data(chg_dev);
	enum mt6360_charging_status chg_stat;
	int ret = 0;

	dev_dbg(mpci->dev, "%s\n", __func__);
	ret = mt6360_get_charging_status(mpci, &chg_stat);
	if (ret < 0)
		return ret;
	return (chg_stat == MT6360_CHG_STATUS_DONE) ? true : false;
}

static int mt6360_charger_get_zcv(struct charger_device *chg_dev, u32 *uV)
{
	struct mt6360_pmu_chg_info *mpci = charger_get_data(chg_dev);

	/* TODO : need get zcv at init sw workaround */
	dev_info(mpci->dev, "%s: zcv = %dmV\n", __func__, mpci->zcv / 1000);
	*uV = mpci->zcv;
	return 0;
}

static int mt6360_charger_dump_registers(struct charger_device *chg_dev)
{
	struct mt6360_pmu_chg_info *mpci = charger_get_data(chg_dev);
	int i, ret = 0;
	int adc_vals[MT6360_ADC_MAX];
	u32 ichg = 0, aicr = 0, mivr = 0, cv = 0, ieoc = 0;
	enum mt6360_charging_status chg_stat = MT6360_CHG_STATUS_READY;
	bool chg_en = false;
	u8 chg_stat1 = 0, chg_ctrl[2] = {0};

	dev_dbg(mpci->dev, "%s\n", __func__);
	ret = mt6360_charger_get_ichg(chg_dev, &ichg);
	ret = mt6360_charger_get_aicr(chg_dev, &aicr);
	ret = mt6360_get_mivr(mpci, &mivr);
	ret = mt6360_charger_get_cv(chg_dev, &cv);
	ret = mt6360_get_ieoc(mpci, &ieoc);
	ret = mt6360_get_charging_status(mpci, &chg_stat);
	ret = mt6360_is_charger_enabled(mpci, &chg_en);
	for (i = 0; i < MT6360_ADC_MAX; i++) {
		ret = iio_read_channel_processed(mpci->channels[i],
						 &adc_vals[i]);
		if (ret < 0) {
			dev_err(mpci->dev,
				"%s: read [%s] adc fail(%d)\n",
				__func__, mt6360_adc_chan_list[i], ret);
			return ret;
		}
	}
	ret = mt6360_pmu_reg_read(mpci->mpi, MT6360_PMU_CHG_STAT1);
	if (ret < 0)
		return ret;
	chg_stat1 = ret;

	ret = mt6360_pmu_reg_block_read(mpci->mpi, MT6360_PMU_CHG_CTRL1,
					2, chg_ctrl);
	if (ret < 0)
		return ret;
	dev_info(mpci->dev,
		 "%s: ICHG = %dmA, AICR = %dmA, MIVR = %dmV, IEOC = %dmA, CV = %dmV\n",
		 __func__, ichg / 1000, aicr / 1000, mivr / 1000, ieoc / 1000,
		 cv / 1000);
	dev_info(mpci->dev,
		 "%s: VBUS = %dmV, IBUS = %dmA, VSYS = %dmV, VBAT = %dmV, IBAT = %dmA\n",
		 __func__,
		 adc_vals[MT6360_ADC_VBUSDIV5] / 1000,
		 adc_vals[MT6360_ADC_IBUS] / 1000,
		 adc_vals[MT6360_ADC_VSYS] / 1000,
		 adc_vals[MT6360_ADC_VBAT] / 1000,
		 adc_vals[MT6360_ADC_IBAT] / 1000);
	dev_info(mpci->dev, "%s: CHG_EN = %d, CHG_STATUS = %s, CHG_STAT1 = 0x%02X\n",
		 __func__, chg_en, mt6360_chg_status_name[chg_stat], chg_stat1);
	dev_info(mpci->dev, "%s: CHG_CTRL1 = 0x%02X, CHG_CTRL2 = 0x%02X\n",
		 __func__, chg_ctrl[0], chg_ctrl[1]);
	return 0;
}

static int mt6360_charger_do_event(struct charger_device *chg_dev, u32 event,
				   u32 args)
{
	struct mt6360_pmu_chg_info *mpci = charger_get_data(chg_dev);

	dev_dbg(mpci->dev, "%s\n", __func__);
	switch (event) {
	case EVENT_EOC:
		charger_dev_notify(chg_dev, CHARGER_DEV_NOTIFY_EOC);
		break;
	case EVENT_RECHARGE:
		charger_dev_notify(chg_dev, CHARGER_DEV_NOTIFY_RECHG);
		break;
	default:
		break;
	}
	return 0;
}

static int mt6360_charger_plug_in(struct charger_device *chg_dev)
{
	struct mt6360_pmu_chg_info *mpci = charger_get_data(chg_dev);
	union power_supply_propval propval;
	int ret = 0;

	dev_dbg(mpci->dev, "%s\n", __func__);

	ret = mt6360_enable_wdt(mpci, true);
	if (ret < 0) {
		dev_err(mpci->dev, "%s: en wdt failed\n", __func__);
		return ret;
	}
	/* Replace CHG_EN by TE for avoid CV level too low trigger ieoc */
	/* TODO: First select cv, then chg_en, no need ? */
	ret = mt6360_charger_enable_te(chg_dev, true);
	if (ret < 0) {
		dev_err(mpci->dev, "%s: en te failed\n", __func__);
		return ret;
	}

	/* Workaround for ibus stuck in pe/pe20 pattern */
	ret = power_supply_get_property(mpci->psy,
					POWER_SUPPLY_PROP_CHARGE_TYPE,
					&propval);
	if (ret < 0) {
		dev_err(mpci->dev, "%s: get chg_type fail\n", __func__);
		return ret;
	}
	if (atomic_read(&mpci->pe_complete) &&
	    propval.intval != STANDARD_CHARGER) {
		ret = mt6360_enable_pump_express(mpci, true);
		if (ret < 0)
			dev_err(mpci->dev, "%s: trigger pe20 pattern fail\n",
				__func__);
	}
	return ret;
}

static int mt6360_charger_plug_out(struct charger_device *chg_dev)
{
	struct mt6360_pmu_chg_info *mpci = charger_get_data(chg_dev);
	int ret = 0;

	dev_dbg(mpci->dev, "%s\n", __func__);
	ret = mt6360_enable_wdt(mpci, false);
	if (ret < 0) {
		dev_err(mpci->dev, "%s: disable wdt failed\n", __func__);
		return ret;
	}
	ret = mt6360_charger_enable_te(chg_dev, false);
	if (ret < 0)
		dev_err(mpci->dev, "%s: disable te failed\n", __func__);
	return ret;
}

static int mt6360_enable_fod_oneshot(struct charger_device *chg_dev, bool en)
{
	struct mt6360_pmu_chg_info *mpci = charger_get_data(chg_dev);

	return (en ? mt6360_pmu_reg_set_bits : mt6360_pmu_reg_clr_bits)
		(mpci->mpi, MT6360_PMU_FOD_CTRL, MT6360_MASK_FOD_SWEN);
}

static int mt6360_get_fod_status(struct charger_device *chg_dev, u8 *status)
{
	int ret;
	struct mt6360_pmu_chg_info *mpci = charger_get_data(chg_dev);

	ret = mt6360_pmu_reg_read(mpci->mpi, MT6360_PMU_FOD_STAT);
	if (ret < 0)
		return ret;
	*status = ret & MT6360_MASK_FOD_ALL_STAT;
	return 0;
}

static int mt6360_is_typec_ot(struct charger_device *chg_dev, bool *ot)
{
	int ret;
	struct mt6360_pmu_chg_info *mpci = charger_get_data(chg_dev);

	ret = mt6360_pmu_reg_read(mpci->mpi, MT6360_PMU_CHG_STAT6);
	if (ret < 0)
		return ret;
	*ot = (ret & MT6360_MASK_TYPEC_OTP) ? true : false;
	return 0;
}

static const struct charger_ops mt6360_chg_ops = {
	/* cable plug in/out */
	.plug_in = mt6360_charger_plug_in,
	.plug_out = mt6360_charger_plug_out,
	/* enable */
	.enable = mt6360_charger_enable,
	/* charging current */
	.set_charging_current = mt6360_charger_set_ichg,
	.get_charging_current = mt6360_charger_get_ichg,
	.get_min_charging_current = mt6360_charger_get_min_ichg,
	/* charging voltage */
	.set_constant_voltage = mt6360_charger_set_cv,
	.get_constant_voltage = mt6360_charger_get_cv,
	/* charging input current */
	.set_input_current = mt6360_charger_set_aicr,
	.get_input_current = mt6360_charger_get_aicr,
	.get_min_input_current = mt6360_charger_get_min_aicr,
	/* set termination current */
	.set_eoc_current = mt6360_charger_set_ieoc,
	/* charging mivr */
	.set_mivr = mt6360_charger_set_mivr,
	/* charing termination */
	.enable_termination = mt6360_charger_enable_te,
	/* PE+/PE+20 */
	.send_ta_current_pattern = mt6360_set_pep_current_pattern,
	.set_pe20_efficiency_table = mt6360_set_pep20_efficiency_table,
	.send_ta20_current_pattern = mt6360_set_pep20_current_pattern,
	.reset_ta = mt6360_reset_ta,
	.enable_cable_drop_comp = mt6360_enable_cable_drop_comp,
	.run_aicl = mt6360_charger_run_aicc,
	/* Power path */
	.enable_powerpath = mt6360_charger_enable_power_path,
	.is_powerpath_enabled = mt6360_charger_is_power_path_enabled,
	/* safety timer */
	.enable_safety_timer = mt6360_charger_enable_safety_timer,
	.is_safety_timer_enabled = mt6360_charger_is_safety_timer_enabled,
	/* OTG */
	.enable_otg = mt6360_charger_enable_otg,
	.set_boost_current_limit = mt6360_charger_set_otg_current_limit,
	.enable_discharge = mt6360_charger_enable_discharge,
	/* Charger type detection */
	.enable_chg_type_det = mt6360_enable_chg_type_det,
	/* ADC */
	.get_vbus_adc = mt6360_charger_get_vbus,
	.get_ibus_adc = mt6360_charger_get_ibus,
	.get_tchg_adc = mt6360_charger_get_tchg,
	/* kick wdt */
	.kick_wdt = mt6360_charger_kick_wdt,
	/* misc */
	.safety_check = mt6360_charger_safety_check,
	.reset_eoc_state = mt6360_charger_reset_eoc_state,
	.is_charging_done = mt6360_charger_is_charging_done,
	.get_zcv = mt6360_charger_get_zcv,
	.dump_registers = mt6360_charger_dump_registers,
	/* event */
	.event = mt6360_charger_do_event,
	/* TypeC */
	.enable_fod_oneshot = mt6360_enable_fod_oneshot,
	.get_fod_status = mt6360_get_fod_status,
	.is_typec_ot = mt6360_is_typec_ot,
};

static const struct charger_properties mt6360_chg_props = {
	.alias_name = "mt6360_chg",
};

static irqreturn_t mt6360_pmu_chg_treg_evt_handler(int irq, void *data)
{
	struct mt6360_pmu_chg_info *mpci = data;
	int ret = 0;

	dev_err(mpci->dev, "%s\n", __func__);
	ret = mt6360_pmu_reg_read(mpci->mpi, MT6360_PMU_CHG_STAT1);
	if (ret < 0)
		return ret;
	if ((ret & MT6360_MASK_CHG_TREG) >> MT6360_SHFT_CHG_TREG)
		dev_err(mpci->dev,
			"%s: thermal regulation loop is active\n", __func__);
	return IRQ_HANDLED;
}

static irqreturn_t mt6360_pmu_chg_aicr_evt_handler(int irq, void *data)
{
	struct mt6360_pmu_chg_info *mpci = data;

	dev_warn(mpci->dev, "%s\n", __func__);
	return IRQ_HANDLED;
}

static irqreturn_t mt6360_pmu_chg_mivr_evt_handler(int irq, void *data)
{
	struct mt6360_pmu_chg_info *mpci = data;

	dev_dbg(mpci->dev, "%s\n", __func__);
	atomic_inc(&mpci->mivr_cnt);
	wake_up(&mpci->waitq);
	return IRQ_HANDLED;
}

static irqreturn_t mt6360_pmu_pwr_rdy_evt_handler(int irq, void *data)
{
#if 0
	struct mt6360_pmu_chg_info *mpci = data;
	bool pwr_rdy = false;
	int ret = 0;

	ret = mt6360_pmu_reg_read(mpci->mpi, MT6360_PMU_CHG_STAT1);
	if (ret < 0)
		return ret;
	pwr_rdy = (ret & MT6360_MASK_PWR_RDY_EVT);
	dev_info(mpci->dev, "%s: pwr_rdy = %d\n", __func__, pwr_rdy);
#endif
	return IRQ_HANDLED;
}

static irqreturn_t mt6360_pmu_chg_batsysuv_evt_handler(int irq, void *data)
{
	struct mt6360_pmu_chg_info *mpci = data;

	dev_warn(mpci->dev, "%s\n", __func__);
	return IRQ_HANDLED;
}

static irqreturn_t mt6360_pmu_chg_vsysuv_evt_handler(int irq, void *data)
{
	struct mt6360_pmu_chg_info *mpci = data;

	dev_warn(mpci->dev, "%s\n", __func__);
	return IRQ_HANDLED;
}

static irqreturn_t mt6360_pmu_chg_vsysov_evt_handler(int irq, void *data)
{
	struct mt6360_pmu_chg_info *mpci = data;

	dev_warn(mpci->dev, "%s\n", __func__);
	return IRQ_HANDLED;
}

static irqreturn_t mt6360_pmu_chg_vbatov_evt_handler(int irq, void *data)
{
	struct mt6360_pmu_chg_info *mpci = data;

	dev_warn(mpci->dev, "%s\n", __func__);
	return IRQ_HANDLED;
}

static irqreturn_t mt6360_pmu_chg_vbusov_evt_handler(int irq, void *data)
{
	struct mt6360_pmu_chg_info *mpci = data;
	struct chgdev_notify *noti = &(mpci->chg_dev->noti);
	bool vbusov_stat = false;
	int ret = 0;

	dev_warn(mpci->dev, "%s\n", __func__);
	ret = mt6360_pmu_reg_read(mpci->mpi, MT6360_PMU_CHG_STAT2);
	if (ret < 0)
		goto out;
	vbusov_stat = (ret & BIT(7));
	noti->vbusov_stat = vbusov_stat;
	dev_info(mpci->dev, "%s: stat = %d\n", __func__, vbusov_stat);
out:
	return IRQ_HANDLED;
}

static irqreturn_t mt6360_pmu_wd_pmu_det_handler(int irq, void *data)
{
	struct mt6360_pmu_chg_info *mpci = data;

	dev_info(mpci->dev, "%s\n", __func__);
	return IRQ_HANDLED;
}

static irqreturn_t mt6360_pmu_wd_pmu_done_handler(int irq, void *data)
{
	struct mt6360_pmu_chg_info *mpci = data;

	dev_info(mpci->dev, "%s\n", __func__);
	return IRQ_HANDLED;
}

static irqreturn_t mt6360_pmu_chg_tmri_handler(int irq, void *data)
{
	struct mt6360_pmu_chg_info *mpci = data;

	dev_warn(mpci->dev, "%s\n", __func__);
	return IRQ_HANDLED;
}

static irqreturn_t mt6360_pmu_chg_adpbadi_handler(int irq, void *data)
{
	struct mt6360_pmu_chg_info *mpci = data;

	dev_warn(mpci->dev, "%s\n", __func__);
	return IRQ_HANDLED;
}

static irqreturn_t mt6360_pmu_chg_rvpi_handler(int irq, void *data)
{
	struct mt6360_pmu_chg_info *mpci = data;

	dev_warn(mpci->dev, "%s\n", __func__);
	return IRQ_HANDLED;
}

static irqreturn_t mt6360_pmu_otpi_handler(int irq, void *data)
{
	struct mt6360_pmu_chg_info *mpci = data;

	dev_warn(mpci->dev, "%s\n", __func__);
	return IRQ_HANDLED;
}

static irqreturn_t mt6360_pmu_chg_aiccmeasl_handler(int irq, void *data)
{
	struct mt6360_pmu_chg_info *mpci = data;

	dev_dbg(mpci->dev, "%s\n", __func__);
	complete(&mpci->aicc_done);
	return IRQ_HANDLED;
}

static irqreturn_t mt6360_pmu_chgdet_donei_handler(int irq, void *data)
{
	struct mt6360_pmu_chg_info *mpci = data;

	dev_dbg(mpci->dev, "%s\n", __func__);
	return IRQ_HANDLED;
}

static irqreturn_t mt6360_pmu_wdtmri_handler(int irq, void *data)
{
	struct mt6360_pmu_chg_info *mpci = data;
	int ret;

	dev_warn(mpci->dev, "%s\n", __func__);
	/* Any I2C R/W can kick watchdog timer */
	ret = mt6360_pmu_reg_read(mpci->mpi, MT6360_PMU_CHG_CTRL1);
	if (ret < 0)
		dev_err(mpci->dev, "%s: kick wdt failed\n", __func__);
	return IRQ_HANDLED;
}

static irqreturn_t mt6360_pmu_ssfinishi_handler(int irq, void *data)
{
	struct mt6360_pmu_chg_info *mpci = data;

	dev_dbg(mpci->dev, "%s\n", __func__);
	return IRQ_HANDLED;
}

static irqreturn_t mt6360_pmu_chg_rechgi_handler(int irq, void *data)
{
	struct mt6360_pmu_chg_info *mpci = data;

	dev_dbg(mpci->dev, "%s\n", __func__);
	charger_dev_notify(mpci->chg_dev, CHARGER_DEV_NOTIFY_RECHG);
	return IRQ_HANDLED;
}

static irqreturn_t mt6360_pmu_chg_termi_handler(int irq, void *data)
{
	struct mt6360_pmu_chg_info *mpci = data;

	dev_dbg(mpci->dev, "%s\n", __func__);
	return IRQ_HANDLED;
}

static irqreturn_t mt6360_pmu_chg_ieoci_handler(int irq, void *data)
{
	struct mt6360_pmu_chg_info *mpci = data;
	bool ieoc_stat = false;
	int ret = 0;

	dev_dbg(mpci->dev, "%s\n", __func__);
	ret = mt6360_pmu_reg_read(mpci->mpi, MT6360_PMU_CHG_STAT4);
	if (ret < 0)
		goto out;
	ieoc_stat = (ret & BIT(7));
	if (!ieoc_stat)
		goto out;

	charger_dev_notify(mpci->chg_dev, CHARGER_DEV_NOTIFY_EOC);
out:
	return IRQ_HANDLED;
}

static irqreturn_t mt6360_pmu_pumpx_donei_handler(int irq, void *data)
{
	struct mt6360_pmu_chg_info *mpci = data;

	dev_info(mpci->dev, "%s\n", __func__);
	atomic_set(&mpci->pe_complete, 0);
	complete(&mpci->pumpx_done);
	return IRQ_HANDLED;
}

static irqreturn_t mt6360_pmu_bst_batuvi_handler(int irq, void *data)
{
	struct mt6360_pmu_chg_info *mpci = data;

	dev_warn(mpci->dev, "%s\n", __func__);
	return IRQ_HANDLED;
}

static irqreturn_t mt6360_pmu_bst_vbusovi_handler(int irq, void *data)
{
	struct mt6360_pmu_chg_info *mpci = data;

	dev_warn(mpci->dev, "%s\n", __func__);
	return IRQ_HANDLED;
}

static irqreturn_t mt6360_pmu_bst_olpi_handler(int irq, void *data)
{
	struct mt6360_pmu_chg_info *mpci = data;

	dev_warn(mpci->dev, "%s\n", __func__);
	return IRQ_HANDLED;
}

static irqreturn_t mt6360_pmu_attachi_handler(int irq, void *data)
{
	struct mt6360_pmu_chg_info *mpci = data;

	dev_dbg(mpci->dev, "%s\n", __func__);
#ifdef CONFIG_MT6360_PMU_CHARGER_TYPE_DETECT
	mutex_lock(&mpci->chgdet_lock);
	if (!mpci->bc12_en) {
		dev_err(mpci->dev, "%s: bc12 disabled, ignore irq\n",
			__func__);
		goto out;
	}
	mt6360_chgdet_post_process(mpci);
out:
	mutex_unlock(&mpci->chgdet_lock);
#endif /* CONFIG_MT6360_PMU_CHARGER_TYPE_DETECT */
	return IRQ_HANDLED;
}

static irqreturn_t mt6360_pmu_detachi_handler(int irq, void *data)
{
	struct mt6360_pmu_chg_info *mpci = data;

	dev_dbg(mpci->dev, "%s\n", __func__);
	return IRQ_HANDLED;
}

static irqreturn_t mt6360_pmu_qc30_stpdone_handler(int irq, void *data)
{
	struct mt6360_pmu_chg_info *mpci = data;

	dev_dbg(mpci->dev, "%s\n", __func__);
	return IRQ_HANDLED;
}

static irqreturn_t mt6360_pmu_qc_vbusdet_done_handler(int irq, void *data)
{
	struct mt6360_pmu_chg_info *mpci = data;

	dev_dbg(mpci->dev, "%s\n", __func__);
	return IRQ_HANDLED;
}

static irqreturn_t mt6360_pmu_hvdcp_det_handler(int irq, void *data)
{
	struct mt6360_pmu_chg_info *mpci = data;

	dev_dbg(mpci->dev, "%s\n", __func__);
	return IRQ_HANDLED;
}

static irqreturn_t mt6360_pmu_chgdeti_handler(int irq, void *data)
{
	struct mt6360_pmu_chg_info *mpci = data;

	dev_dbg(mpci->dev, "%s\n", __func__);
	return IRQ_HANDLED;
}

static irqreturn_t mt6360_pmu_dcdti_handler(int irq, void *data)
{
	struct mt6360_pmu_chg_info *mpci = data;

	dev_dbg(mpci->dev, "%s\n", __func__);
	return IRQ_HANDLED;
}

static irqreturn_t mt6360_pmu_chrdet_ext_evt_handler(int irq, void *data)
{
#ifdef CONFIG_MT6360_PMU_CHARGER_TYPE_DETECT
	struct mt6360_pmu_chg_info *mpci = data;
	int ret = 0;
	bool pwr_rdy = false;

	dev_info(mpci->dev, "%s\n", __func__);
	mutex_lock(&mpci->chgdet_lock);
	ret = mt6360_get_chrdet_ext_stat(mpci, &pwr_rdy);
	dev_info(mpci->dev, "%s: pwr_rdy = %d\n", __func__, pwr_rdy);
	if (ret < 0)
		goto out;
	if (mpci->pwr_rdy == pwr_rdy)
		goto out;
	mpci->pwr_rdy = pwr_rdy;
	mt6360_psy_online_changed(mpci);

#ifndef CONFIG_TCPC_CLASS
	mt6360_chgdet_pre_process(mpci, pwr_rdy);
#endif /* !CONFIG_TCPC_CLASS */
out:
	mutex_unlock(&mpci->chgdet_lock);
#endif /* CONFIG_MT6360_PMU_CHARGER_TYPE_DETECT */
	return IRQ_HANDLED;
}

static struct mt6360_pmu_irq_desc mt6360_pmu_chg_irq_desc[] = {
	MT6360_PMU_IRQDESC(chg_treg_evt),
	MT6360_PMU_IRQDESC(chg_aicr_evt),
	MT6360_PMU_IRQDESC(chg_mivr_evt),
	MT6360_PMU_IRQDESC(pwr_rdy_evt),
	MT6360_PMU_IRQDESC(chg_batsysuv_evt),
	MT6360_PMU_IRQDESC(chg_vsysuv_evt),
	MT6360_PMU_IRQDESC(chg_vsysov_evt),
	MT6360_PMU_IRQDESC(chg_vbatov_evt),
	MT6360_PMU_IRQDESC(chg_vbusov_evt),
	MT6360_PMU_IRQDESC(wd_pmu_det),
	MT6360_PMU_IRQDESC(wd_pmu_done),
	MT6360_PMU_IRQDESC(chg_tmri),
	MT6360_PMU_IRQDESC(chg_adpbadi),
	MT6360_PMU_IRQDESC(chg_rvpi),
	MT6360_PMU_IRQDESC(otpi),
	MT6360_PMU_IRQDESC(chg_aiccmeasl),
	MT6360_PMU_IRQDESC(chgdet_donei),
	MT6360_PMU_IRQDESC(wdtmri),
	MT6360_PMU_IRQDESC(ssfinishi),
	MT6360_PMU_IRQDESC(chg_rechgi),
	MT6360_PMU_IRQDESC(chg_termi),
	MT6360_PMU_IRQDESC(chg_ieoci),
	MT6360_PMU_IRQDESC(pumpx_donei),
	MT6360_PMU_IRQDESC(bst_batuvi),
	MT6360_PMU_IRQDESC(bst_vbusovi),
	MT6360_PMU_IRQDESC(bst_olpi),
	MT6360_PMU_IRQDESC(attachi),
	MT6360_PMU_IRQDESC(detachi),
	MT6360_PMU_IRQDESC(qc30_stpdone),
	MT6360_PMU_IRQDESC(qc_vbusdet_done),
	MT6360_PMU_IRQDESC(hvdcp_det),
	MT6360_PMU_IRQDESC(chgdeti),
	MT6360_PMU_IRQDESC(dcdti),
	MT6360_PMU_IRQDESC(chrdet_ext_evt),
};

static void mt6360_pmu_chg_irq_register(struct platform_device *pdev)
{
	struct mt6360_pmu_irq_desc *irq_desc;
	int i, ret;

	for (i = 0; i < ARRAY_SIZE(mt6360_pmu_chg_irq_desc); i++) {
		irq_desc = mt6360_pmu_chg_irq_desc + i;
		if (unlikely(!irq_desc->name))
			continue;
		ret = platform_get_irq_byname(pdev, irq_desc->name);
		if (ret < 0)
			continue;
		irq_desc->irq = ret;
		ret = devm_request_threaded_irq(&pdev->dev, irq_desc->irq, NULL,
						irq_desc->irq_handler,
						IRQF_TRIGGER_FALLING,
						irq_desc->name,
						platform_get_drvdata(pdev));
		if (ret < 0)
			dev_err(&pdev->dev,
				"request %s irq fail\n", irq_desc->name);
	}
}

static int mt6360_toggle_cfo(struct mt6360_pmu_chg_info *mpci)
{
	int ret = 0;

	mutex_lock(&mpci->mpi->io_lock);
	/* check if strobe mode */
	ret = mt6360_pmu_reg_read(mpci->mpi, MT6360_PMU_FLED_EN);
	if (ret < 0)
		goto out;
	if (ret & MT6360_MASK_STROBE_EN) {
		dev_err(mpci->dev, "%s: fled in strobe mode\n", __func__);
		goto out;
	}
	ret = mt6360_pmu_reg_clr_bits(mpci->mpi, MT6360_PMU_CHG_CTRL2,
				      MT6360_MASK_CFO_EN);
	if (ret < 0) {
		dev_err(mpci->dev, "%s: clear cfo fail\n", __func__);
		goto out;
	}
	ret = mt6360_pmu_reg_set_bits(mpci->mpi, MT6360_PMU_CHG_CTRL2,
				      MT6360_MASK_CFO_EN);
	if (ret < 0) {
		dev_err(mpci->dev, "%s: set cfo fail\n", __func__);
		goto out;
	}
out:
	mutex_unlock(&mpci->mpi->io_lock);
	return ret;
}

static int mt6360_chg_mivr_task_threadfn(void *data)
{
	struct mt6360_pmu_chg_info *mpci = data;
	u32 ibus;
	int ret;

	dev_info(mpci->dev, "%s ++\n", __func__);
	while (!kthread_should_stop()) {
		atomic_set(&mpci->mivr_cnt, 0);
		ret = wait_event_interruptible(mpci->waitq,
					      atomic_read(&mpci->mivr_cnt) > 0);
		if (ret < 0)
			continue;
		dev_dbg(mpci->dev, "%s: enter mivr thread\n", __func__);
		pm_stay_awake(mpci->dev);
		/* check real mivr stat or not */
		ret = mt6360_pmu_reg_read(mpci->mpi, MT6360_PMU_CHG_STAT1);
		if (ret < 0)
			goto loop_cont;
		if (!(ret & MT6360_MASK_MIVR_EVT)) {
			dev_dbg(mpci->dev, "%s: mivr stat not act\n", __func__);
			goto loop_cont;
		}
		/* read ibus adc */
		ret = mt6360_charger_get_ibus(mpci->chg_dev, &ibus);
		if (ret < 0) {
			dev_err(mpci->dev, "%s: get ibus adc fail\n", __func__);
			goto loop_cont;
		}
		/* if ibus adc value < 100mA), toggle cfo */
		if (ibus < 100000) {
			dev_dbg(mpci->dev, "%s: enter toggle cfo\n", __func__);
			ret = mt6360_toggle_cfo(mpci);
			if (ret < 0)
				dev_err(mpci->dev,
					"%s: toggle cfo fail\n", __func__);
		}
loop_cont:
		pm_relax(mpci->dev);
	}
	dev_info(mpci->dev, "%s --\n", __func__);
	return 0;
}

#if defined(CONFIG_MT6360_PMU_CHARGER_TYPE_DETECT)\
&& !defined(CONFIG_TCPC_CLASS)
static void mt6360_chgdet_work_handler(struct work_struct *work)
{
	int ret = 0;
	bool pwr_rdy = false;
	struct mt6360_pmu_chg_info *mpci =
		(struct mt6360_pmu_chg_info *)container_of(work,
		struct mt6360_pmu_chg_info, chgdet_work);

	/* Check PWR_RDY_STAT */
	ret = mt6360_get_chrdet_ext_stat(mpci, &pwr_rdy);
	if (ret < 0)
		return;
	/* power not good */
	if (!pwr_rdy)
		return;
	/* power good */
	/* Turn on USB charger detection */
	ret = mt6360_enable_usbchgen(mpci, true);
	if (ret < 0)
		dev_err(mpci->dev, "%s: en bc12 fail\n", __func__);
}
#endif /* CONFIG_MT6360_PMU_CHARGER_TYPE_DETECT && !CONFIG_TCPC_CLASS */

static const struct mt6360_pdata_prop mt6360_pdata_props[] = {
	MT6360_PDATA_VALPROP(ichg, struct mt6360_chg_platform_data,
			     MT6360_PMU_CHG_CTRL7, 2, 0xFC,
			     mt6360_trans_ichg_sel, 0),
	MT6360_PDATA_VALPROP(aicr, struct mt6360_chg_platform_data,
			     MT6360_PMU_CHG_CTRL3, 2, 0xFC,
			     mt6360_trans_aicr_sel, 0),
	MT6360_PDATA_VALPROP(mivr, struct mt6360_chg_platform_data,
			     MT6360_PMU_CHG_CTRL6, 1, 0xFE,
			     mt6360_trans_mivr_sel, 0),
	MT6360_PDATA_VALPROP(cv, struct mt6360_chg_platform_data,
			     MT6360_PMU_CHG_CTRL4, 1, 0xFE,
			     mt6360_trans_cv_sel, 0),
	MT6360_PDATA_VALPROP(ieoc, struct mt6360_chg_platform_data,
			     MT6360_PMU_CHG_CTRL9, 4, 0xF0,
			     mt6360_trans_ieoc_sel, 0),
	MT6360_PDATA_VALPROP(safety_timer, struct mt6360_chg_platform_data,
			     MT6360_PMU_CHG_CTRL12, 5, 0xE0, NULL, 0),
	MT6360_PDATA_VALPROP(ircmp_resistor, struct mt6360_chg_platform_data,
			     MT6360_PMU_CHG_CTRL18, 3, 0x38,
			     mt6360_trans_ircmp_r_sel, 0),
	MT6360_PDATA_VALPROP(ircmp_vclamp, struct mt6360_chg_platform_data,
			     MT6360_PMU_CHG_CTRL18, 0, 0x07,
			     mt6360_trans_ircmp_vclamp_sel, 0),
#if 0
	MT6360_PDATA_VALPROP(en_te, struct mt6360_chg_platform_data,
			     MT6360_PMU_CHG_CTRL2, 4, 0x10, NULL, 0),
	MT6360_PDATA_VALPROP(en_wdt, struct mt6360_chg_platform_data,
			     MT6360_PMU_CHG_CTRL13, 7, 0x80, NULL, 0),
#endif
	MT6360_PDATA_VALPROP(aicc_once, struct mt6360_chg_platform_data,
			     MT6360_PMU_CHG_CTRL14, 0, 0x04, NULL, 0),
};

static int mt6360_chg_apply_pdata(struct mt6360_pmu_chg_info *mpci,
				  struct mt6360_chg_platform_data *pdata)
{
	int ret;

	dev_dbg(mpci->dev, "%s ++\n", __func__);
	ret = mt6360_pdata_apply_helper(mpci->mpi, pdata, mt6360_pdata_props,
					ARRAY_SIZE(mt6360_pdata_props));
	if (ret < 0)
		return ret;
	dev_dbg(mpci->dev, "%s ++\n", __func__);
	return 0;
}

static const struct mt6360_val_prop mt6360_val_props[] = {
	MT6360_DT_VALPROP(ichg, struct mt6360_chg_platform_data),
	MT6360_DT_VALPROP(aicr, struct mt6360_chg_platform_data),
	MT6360_DT_VALPROP(mivr, struct mt6360_chg_platform_data),
	MT6360_DT_VALPROP(cv, struct mt6360_chg_platform_data),
	MT6360_DT_VALPROP(ieoc, struct mt6360_chg_platform_data),
	MT6360_DT_VALPROP(safety_timer, struct mt6360_chg_platform_data),
	MT6360_DT_VALPROP(ircmp_resistor, struct mt6360_chg_platform_data),
	MT6360_DT_VALPROP(ircmp_vclamp, struct mt6360_chg_platform_data),
	MT6360_DT_VALPROP(en_te, struct mt6360_chg_platform_data),
	MT6360_DT_VALPROP(en_wdt, struct mt6360_chg_platform_data),
	MT6360_DT_VALPROP(aicc_once, struct mt6360_chg_platform_data),
};

static int mt6360_chg_parse_dt_data(struct device *dev,
				    struct mt6360_chg_platform_data *pdata)
{
	struct device_node *np = dev->of_node;

	dev_dbg(dev, "%s ++\n", __func__);
	memcpy(pdata, &def_platform_data, sizeof(*pdata));
	mt6360_dt_parser_helper(np, (void *)pdata,
				mt6360_val_props, ARRAY_SIZE(mt6360_val_props));
	dev_dbg(dev, "%s --\n", __func__);
	return 0;
}

static int mt6360_enable_ilim(struct mt6360_pmu_chg_info *mpci, bool en)
{
	return (en ? mt6360_pmu_reg_set_bits : mt6360_pmu_reg_clr_bits)
		(mpci->mpi, MT6360_PMU_CHG_CTRL3, MT6360_MASK_ILIM_EN);
}

static int mt6360_chg_init_setting(struct mt6360_pmu_chg_info *mpci)
{
	struct mt6360_chg_platform_data *pdata = dev_get_platdata(mpci->dev);
	int ret = 0;

	dev_info(mpci->dev, "%s\n", __func__);
	ret = mt6360_select_input_current_limit(mpci, MT6360_IINLMTSEL_AICR);
	if (ret < 0) {
		dev_err(mpci->dev, "%s: select iinlmtsel by aicr fail\n",
			__func__);
		return ret;
	}
	usleep_range(5000, 6000);
	ret = mt6360_enable_ilim(mpci, false);
	if (ret < 0) {
		dev_err(mpci->dev, "%s: disable ilim fail\n", __func__);
		return ret;
	}
	/* disable wdt reduce 1mA power consumption */
	ret = mt6360_enable_wdt(mpci, false);
	if (ret < 0) {
		dev_err(mpci->dev, "%s: disable wdt fail\n", __func__);
		return ret;
	}
	/* Disable USB charger type detect, no matter use it or not */
	ret = mt6360_enable_usbchgen(mpci, false);
	if (ret < 0) {
		dev_err(mpci->dev, "%s: disable chg type detect fail\n",
			__func__);
		return ret;
	}
	/* unlock ovp limit for pump express, can be replaced by option */
	ret = mt6360_select_vinovp(mpci, 14500000);
	if (ret < 0) {
		dev_err(mpci->dev, "%s: unlimit vin for pump express\n",
			__func__);
		return ret;
	}
	/* Disable TE, set TE when plug in/out */
	ret = mt6360_pmu_reg_clr_bits(mpci->mpi, MT6360_PMU_CHG_CTRL2,
				      MT6360_MASK_TE_EN);
	if (ret < 0) {
		dev_err(mpci->dev, "%s: disable te fail\n", __func__);
		return ret;
	}
	/* Read ZCV */
	ret = mt6360_read_zcv(mpci);
	if (ret < 0) {
		dev_err(mpci->dev, "%s: read zcv fail\n", __func__);
		return ret;
	}
	/* enable AICC_EN if aicc_once = 0 */
	if (!pdata->aicc_once) {
		ret = mt6360_pmu_reg_set_bits(mpci->mpi, MT6360_PMU_CHG_CTRL14,
					      MT6360_MASK_RG_EN_AICC);
		if (ret < 0)
			dev_err(mpci->dev, "%s: enable aicc fail\n", __func__);
	}
	/* Check BATSYSUV occurred last time boot-on */
	ret = mt6360_pmu_reg_read(mpci->mpi, MT6360_PMU_CHG_STAT);
	if (ret < 0) {
		dev_err(mpci->dev, "%s: read BATSYSUV fail\n", __func__);
		return ret;
	}
	if (!(ret & MT6360_MASK_CHG_BATSYSUV)) {
		dev_warn(mpci->dev, "%s: BATSYSUV occurred\n", __func__);
		ret = mt6360_pmu_reg_set_bits(mpci->mpi, MT6360_PMU_CHG_STAT,
					      MT6360_MASK_CHG_BATSYSUV);
		if (ret < 0) {
			dev_err(mpci->dev,
				"%s: clear BATSYSUV fail\n", __func__);
		}
	}
	return ret;
}

static int mt6360_pmu_chg_probe(struct platform_device *pdev)
{
	struct mt6360_chg_platform_data *pdata = dev_get_platdata(&pdev->dev);
	struct mt6360_pmu_chg_info *mpci;
	struct iio_channel *channel;
	bool use_dt = pdev->dev.of_node;
	int i, ret = 0;

	dev_info(&pdev->dev, "%s\n", __func__);
	if (use_dt) {
		pdata = devm_kzalloc(&pdev->dev, sizeof(*pdata), GFP_KERNEL);
		if (!pdata)
			return -ENOMEM;
		ret = mt6360_chg_parse_dt_data(&pdev->dev, pdata);
		if (ret < 0) {
			dev_err(&pdev->dev, "parse dt fail\n");
			return ret;
		}
		pdev->dev.platform_data = pdata;
	}
	if (!pdata) {
		dev_err(&pdev->dev, "no platform data specified\n");
		return -EINVAL;
	}
	mpci = devm_kzalloc(&pdev->dev, sizeof(*mpci), GFP_KERNEL);
	if (!mpci)
		return -ENOMEM;
	mpci->dev = &pdev->dev;
	mpci->mpi = dev_get_drvdata(pdev->dev.parent);
	mpci->hidden_mode_cnt = 0;
	mutex_init(&mpci->hidden_mode_lock);
	mutex_init(&mpci->pe_lock);
	mutex_init(&mpci->aicr_lock);
	mutex_init(&mpci->chgdet_lock);
	mutex_init(&mpci->tchg_lock);
	mpci->tchg = 0;
#if defined(CONFIG_MT6360_PMU_CHARGER_TYPE_DETECT)\
&& !defined(CONFIG_TCPC_CLASS)
	INIT_WORK(&mpci->chgdet_work, mt6360_chgdet_work_handler);
#endif /* CONFIG_MT6360_PMU_CHARGER_TYPE_DETECT && !CONFIG_TCPC_CLASS */
	init_completion(&mpci->aicc_done);
	init_completion(&mpci->pumpx_done);
	atomic_set(&mpci->pe_complete, 0);
	atomic_set(&mpci->mivr_cnt, 0);
	init_waitqueue_head(&mpci->waitq);
	platform_set_drvdata(pdev, mpci);

	/* apply platform data */
	ret = mt6360_chg_apply_pdata(mpci, pdata);
	if (ret < 0) {
		dev_err(&pdev->dev, "apply pdata fail\n");
		return ret;
	}
	/* Initial Setting */
	ret = mt6360_chg_init_setting(mpci);
	if (ret < 0) {
		dev_err(&pdev->dev, "%s: init setting fail\n", __func__);
		return ret;
	}

	/* Get ADC iio channels */
	for (i = 0; i < MT6360_ADC_MAX; i++) {
		channel = devm_iio_channel_get(&pdev->dev,
					       mt6360_adc_chan_list[i]);
		if (IS_ERR(channel))
			return PTR_ERR(channel);
		mpci->channels[i] = channel;
	}
	/* Get chg type det power supply */
	mpci->psy = power_supply_get_by_name("charger");
	if (!mpci->psy) {
		dev_err(mpci->dev,
			"%s: get power supply failed\n", __func__);
		return -EINVAL;
	}
	/* charger class register */
	mpci->chg_dev = charger_device_register(pdata->chg_name, mpci->dev,
						mpci, &mt6360_chg_ops,
						&mt6360_chg_props);
	if (IS_ERR(mpci->chg_dev)) {
		dev_err(mpci->dev, "charger device register fail\n");
		return PTR_ERR(mpci->chg_dev);
	}
	/* TODO: is_polling_mode is not to use mt6360 irq to report event ? */

	/* irq register */
	mt6360_pmu_chg_irq_register(pdev);
	device_init_wakeup(&pdev->dev, true);
	/* mivr task */
	mpci->mivr_task = kthread_run(mt6360_chg_mivr_task_threadfn, mpci,
				      kasprintf(GFP_KERNEL, "mivr_thread.%s",
				      dev_name(mpci->dev)));
	ret = PTR_ERR_OR_ZERO(mpci->mivr_task);
	if (ret < 0) {
		dev_err(mpci->dev, "create mivr handling thread fail\n");
		return ret;
	}
	/* Schedule work for microB's BC1.2 */
#if defined(CONFIG_MT6360_PMU_CHARGER_TYPE_DETECT)\
&& !defined(CONFIG_TCPC_CLASS)
	schedule_work(&mpci->chgdet_work);
#endif /* CONFIG_MT6360_PMU_CHARGER_TYPE_DETECT && !CONFIG_TCPC_CLASS */
	dev_info(&pdev->dev, "%s: successfully probed\n", __func__);
	return 0;
}

static int mt6360_pmu_chg_remove(struct platform_device *pdev)
{
	struct mt6360_pmu_chg_info *mpci = platform_get_drvdata(pdev);

	dev_dbg(mpci->dev, "%s\n", __func__);
	if (mpci->mivr_task) {
		kthread_stop(mpci->mivr_task);
		atomic_inc(&mpci->mivr_cnt);
		wake_up(&mpci->waitq);
	}
	return 0;
}

static int __maybe_unused mt6360_pmu_chg_suspend(struct device *dev)
{
	return 0;
}

static int __maybe_unused mt6360_pmu_chg_resume(struct device *dev)
{
	return 0;
}

static SIMPLE_DEV_PM_OPS(mt6360_pmu_chg_pm_ops,
			 mt6360_pmu_chg_suspend, mt6360_pmu_chg_resume);

static const struct of_device_id __maybe_unused mt6360_pmu_chg_of_id[] = {
	{ .compatible = "mediatek,mt6360_pmu_chg", },
	{},
};
MODULE_DEVICE_TABLE(of, mt6360_pmu_chg_of_id);

static const struct platform_device_id mt6360_pmu_chg_id[] = {
	{ "mt6360_pmu_chg", 0 },
	{},
};
MODULE_DEVICE_TABLE(platform, mt6360_pmu_chg_id);

static struct platform_driver mt6360_pmu_chg_driver = {
	.driver = {
		.name = "mt6360_pmu_chg",
		.owner = THIS_MODULE,
		.pm = &mt6360_pmu_chg_pm_ops,
		.of_match_table = of_match_ptr(mt6360_pmu_chg_of_id),
	},
	.probe = mt6360_pmu_chg_probe,
	.remove = mt6360_pmu_chg_remove,
	.id_table = mt6360_pmu_chg_id,
};
module_platform_driver(mt6360_pmu_chg_driver);

MODULE_AUTHOR("CY_Huang <cy_huang@richtek.com>");
MODULE_DESCRIPTION("MT6360 PMU CHG Driver");
MODULE_LICENSE("GPL");
MODULE_VERSION(MT6360_PMU_CHG_DRV_VERSION);

/*
 * Version Note
 * 1.0.1_MTK
 * (1) fix dtsi parse attribute about en_te, en_wdt, aicc_once
 * (2) add charger class get vbus adc interface
 * (3) add initial setting about disable en_sdi, and check batsysuv.
 *
 * 1.0.0_MTK
 * (1) Initial Release
 */
