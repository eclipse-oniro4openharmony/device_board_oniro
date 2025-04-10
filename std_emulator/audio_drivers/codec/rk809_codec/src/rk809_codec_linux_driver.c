/*
 * Copyright (c) 2021 Rockchip Electronics Co., Ltd.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/device.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/mfd/rk808.h>
#include "rk817_codec.h"
#include "audio_driver_log.h"

#define HDF_LOG_TAG "rk809_codec_linux_driver"

struct platform_device *rk817_pdev;
struct platform_device *GetCodecPlatformDevice(void)
{
    return rk817_pdev;
}

static const struct of_device_id rk817_codec_dt_ids[] = {
    { .compatible = "rockchip,rk817-codec" },
};
MODULE_DEVICE_TABLE(of, rk817_codec_dt_ids);

static int rk817_platform_probe(struct platform_device *pdev)
{
    rk817_pdev = pdev;
    dev_info(&pdev->dev, "got rk817-codec platform_device");
    return 0;
}

static int rk817_platform_remove(struct platform_device *pdev)
{
    panic("%s not support now", __func__);
}

static struct platform_driver rk817_codec_driver = {
    .driver = {
        .name = "rk817-codec",
        .of_match_table = rk817_codec_dt_ids,
    },
    .probe = rk817_platform_probe,
    .remove = rk817_platform_remove,
};

module_platform_driver(rk817_codec_driver);

static const struct reg_default rk817_reg_defaults[] = {
    { RK817_CODEC_DTOP_VUCTL, 0x003 },
    { RK817_CODEC_DTOP_VUCTIME, 0x00 },
    { RK817_CODEC_DTOP_LPT_SRST, 0x00 },
    { RK817_CODEC_DTOP_DIGEN_CLKE, 0x00 },
    { RK817_CODEC_AREF_RTCFG0, 0x00 },
    { RK817_CODEC_AREF_RTCFG1, 0x06 },
    { RK817_CODEC_AADC_CFG0, 0xc8 },
    { RK817_CODEC_AADC_CFG1, 0x00 },
    { RK817_CODEC_DADC_SR_ACL0, 0x00 },
    { RK817_CODEC_DADC_ALC1, 0x00 },
    { RK817_CODEC_DADC_ALC2, 0x00 },
    { RK817_CODEC_DADC_NG, 0x00 },
    { RK817_CODEC_DADC_HPF, 0x00 },
    { RK817_CODEC_DADC_RVOLL, 0xff },
    { RK817_CODEC_DADC_RVOLR, 0xff },
    { RK817_CODEC_AMIC_CFG0, 0x70 },
    { RK817_CODEC_AMIC_CFG1, 0x00 },
    { RK817_CODEC_DMIC_PGA_GAIN, 0x6F },
    { RK817_CODEC_DMIC_LMT1, 0x00 },
    { RK817_CODEC_DMIC_LMT2, 0x00 },
    { RK817_CODEC_DMIC_NG1, 0x00 },
    { RK817_CODEC_DMIC_NG2, 0x00 },
    { RK817_CODEC_ADAC_CFG0, 0x00 },
    { RK817_CODEC_ADAC_CFG1, 0x07 },
    { RK817_CODEC_DDAC_POPD_DACST, 0x82 },
    { RK817_CODEC_DDAC_VOLL, 0x00 },
    { RK817_CODEC_DDAC_VOLR, 0x00 },
    { RK817_CODEC_DDAC_SR_LMT0, 0x00 },
    { RK817_CODEC_DDAC_LMT1, 0x00 },
    { RK817_CODEC_DDAC_LMT2, 0x00 },
    { RK817_CODEC_DDAC_MUTE_MIXCTL, 0xa0 },
    { RK817_CODEC_DDAC_RVOLL, 0xff },
    { RK817_CODEC_DDAC_RVOLR, 0xff },
    { RK817_CODEC_AHP_ANTI0, 0x00 },
    { RK817_CODEC_AHP_ANTI1, 0x00 },
    { RK817_CODEC_AHP_CFG0, 0xe0 },
    { RK817_CODEC_AHP_CFG1, 0x1f },
    { RK817_CODEC_AHP_CP, 0x09 },
    { RK817_CODEC_ACLASSD_CFG1, 0x69 },
    { RK817_CODEC_ACLASSD_CFG2, 0x44 },
    { RK817_CODEC_APLL_CFG0, 0x04 },
    { RK817_CODEC_APLL_CFG1, 0x00 },
    { RK817_CODEC_APLL_CFG2, 0x30 },
    { RK817_CODEC_APLL_CFG3, 0x19 },
    { RK817_CODEC_APLL_CFG4, 0x65 },
    { RK817_CODEC_APLL_CFG5, 0x01 },
    { RK817_CODEC_DI2S_CKM, 0x01 },
    { RK817_CODEC_DI2S_RSD, 0x00 },
    { RK817_CODEC_DI2S_RXCR1, 0x00 },
    { RK817_CODEC_DI2S_RXCR2, 0x17 },
    { RK817_CODEC_DI2S_RXCMD_TSD, 0x00 },
    { RK817_CODEC_DI2S_TXCR1, 0x00 },
    { RK817_CODEC_DI2S_TXCR2, 0x17 },
    { RK817_CODEC_DI2S_TXCR3_TXCMD, 0x00 },
};

static bool rk817_volatile_register(struct device *dev, unsigned int reg)
{
    switch (reg) {
        case RK817_CODEC_DTOP_LPT_SRST:
            return true;
        default:
            return false;
    }
}

static bool rk817_codec_register(struct device *dev, unsigned int reg)
{
    switch (reg) {
        case RK817_CODEC_DTOP_VUCTL:
        case RK817_CODEC_DTOP_VUCTIME:
        case RK817_CODEC_DTOP_LPT_SRST:
        case RK817_CODEC_DTOP_DIGEN_CLKE:
        case RK817_CODEC_AREF_RTCFG0:
        case RK817_CODEC_AREF_RTCFG1:
        case RK817_CODEC_AADC_CFG0:
        case RK817_CODEC_AADC_CFG1:
        case RK817_CODEC_DADC_VOLL:
        case RK817_CODEC_DADC_VOLR:
        case RK817_CODEC_DADC_SR_ACL0:
        case RK817_CODEC_DADC_ALC1:
        case RK817_CODEC_DADC_ALC2:
        case RK817_CODEC_DADC_NG:
        case RK817_CODEC_DADC_HPF:
        case RK817_CODEC_DADC_RVOLL:
        case RK817_CODEC_DADC_RVOLR:
        case RK817_CODEC_AMIC_CFG0:
        case RK817_CODEC_AMIC_CFG1:
        case RK817_CODEC_DMIC_PGA_GAIN:
        case RK817_CODEC_DMIC_LMT1:
        case RK817_CODEC_DMIC_LMT2:
        case RK817_CODEC_DMIC_NG1:
        case RK817_CODEC_DMIC_NG2:
        case RK817_CODEC_ADAC_CFG0:
        case RK817_CODEC_ADAC_CFG1:
        case RK817_CODEC_DDAC_POPD_DACST:
        case RK817_CODEC_DDAC_VOLL:
        case RK817_CODEC_DDAC_VOLR:
        case RK817_CODEC_DDAC_SR_LMT0:
        case RK817_CODEC_DDAC_LMT1:
        case RK817_CODEC_DDAC_LMT2:
        case RK817_CODEC_DDAC_MUTE_MIXCTL:
        case RK817_CODEC_DDAC_RVOLL:
        case RK817_CODEC_DDAC_RVOLR:
        case RK817_CODEC_AHP_ANTI0:
        case RK817_CODEC_AHP_ANTI1:
        case RK817_CODEC_AHP_CFG0:
        case RK817_CODEC_AHP_CFG1:
        case RK817_CODEC_AHP_CP:
        case RK817_CODEC_ACLASSD_CFG1:
        case RK817_CODEC_ACLASSD_CFG2:
        case RK817_CODEC_APLL_CFG0:
        case RK817_CODEC_APLL_CFG1:
        case RK817_CODEC_APLL_CFG2:
        case RK817_CODEC_APLL_CFG3:
        case RK817_CODEC_APLL_CFG4:
        case RK817_CODEC_APLL_CFG5:
        case RK817_CODEC_DI2S_CKM:
        case RK817_CODEC_DI2S_RSD:
        case RK817_CODEC_DI2S_RXCR1:
        case RK817_CODEC_DI2S_RXCR2:
        case RK817_CODEC_DI2S_RXCMD_TSD:
        case RK817_CODEC_DI2S_TXCR1:
        case RK817_CODEC_DI2S_TXCR2:
        case RK817_CODEC_DI2S_TXCR3_TXCMD:
            return true;
        default:
            return false;
    }
}

static const struct regmap_config rk817_codec_regmap_config = {
    .name = "rk817-codec",
    .reg_bits = 8,
    .val_bits = 8,
    .reg_stride = 1,
    .max_register = 0x4f,
    .cache_type = REGCACHE_FLAT,
    .volatile_reg = rk817_volatile_register,
    .writeable_reg = rk817_codec_register,
    .readable_reg = rk817_codec_register,
    .reg_defaults = rk817_reg_defaults,
    .num_reg_defaults = ARRAY_SIZE(rk817_reg_defaults),
};

struct regmap_config getCodecRegmap(void)
{
    return rk817_codec_regmap_config;
}
