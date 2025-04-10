/*
 * Copyright (C) 2022 HiHope Open Source Organization .
 *
 * HDF is dual licensed: you can use it either under the terms of
 * the GPL, or the BSD license, at your option.
 * See the LICENSE file in the root of this repository for complete details.
 */
#include <sound/pcm_params.h>
#include <sound/dmaengine_pcm.h>
#include <linux/module.h>
#include <linux/mfd/syscon.h>
#include <linux/delay.h>
#include <linux/of_gpio.h>
#include <linux/of_device.h>
#include <linux/of_address.h>
#include <linux/clk.h>
#include <linux/clk/rockchip.h>
#include <linux/pm_runtime.h>
#include <linux/regmap.h>
#include <linux/reset.h>
#include <linux/spinlock.h>

#include "audio_host.h"
#include "audio_control.h"
#include "audio_dai_if.h"
#include "audio_dai_base.h"
#include "audio_driver_log.h"
#include "audio_stream_dispatch.h"
#include "osal_io.h"
#include "rk3568_dai_linux.h"
#include "rk3568_audio_common.h"
#include "audio_platform_base.h"
#include "rk3568_dai_ops.h"

#define HDF_LOG_TAG rk3568_dai_ops

static struct platform_device *GetPlatformDev(const struct DaiDevice *dai)
{
    struct device_node *dmaOfNode = NULL;

    if (dai == NULL || dai->devData == NULL || dai->devData->regConfig == NULL) {
        AUDIO_DEVICE_LOG_ERR("dai is null");
        return NULL;
    }

    dmaOfNode = of_find_node_by_path(dai->devData->regConfig->audioIdInfo.chipName);
    if (dmaOfNode == NULL) {
        AUDIO_DEVICE_LOG_ERR("get device node failed.");
        return NULL;
    }
    return of_find_device_by_node(dmaOfNode);
}

int32_t Rk3568DeviceReadReg(const struct DaiDevice *dai, uint32_t reg, uint32_t *val)
{
    struct platform_device *platformdev = GetPlatformDev(dai);
    struct rk3568_i2s_tdm_dev *i2sTdm = NULL;

    if (platformdev == NULL) {
        AUDIO_DEVICE_LOG_ERR("platformdev is null");
        return HDF_FAILURE;
    }
    (void)dai;
    i2sTdm = dev_get_drvdata(&platformdev->dev);
    if (i2sTdm == NULL) {
        AUDIO_DEVICE_LOG_ERR("i2sTdm is null");
        return HDF_FAILURE;
    }

    if (regmap_read(i2sTdm->regmap, reg, val)) {
        AUDIO_DEVICE_LOG_ERR("read register fail: [%04x]", reg);
        return HDF_FAILURE;
    }

    AUDIO_DEVICE_LOG_DEBUG("success");
    return HDF_SUCCESS;
}

int32_t Rk3568DeviceWriteReg(const struct DaiDevice *dai, uint32_t reg, uint32_t value)
{
    struct platform_device *platformdev = GetPlatformDev(dai);
    struct rk3568_i2s_tdm_dev *i2sTdm = NULL;
    (void)dai;

    if (platformdev == NULL) {
        AUDIO_DEVICE_LOG_ERR("platformdev is null");
        return HDF_FAILURE;
    }
    i2sTdm = dev_get_drvdata(&platformdev->dev);
    if (i2sTdm == NULL) {
        AUDIO_DEVICE_LOG_ERR("i2sTdm is null");
        return HDF_FAILURE;
    }

    if (regmap_write(i2sTdm->regmap, reg, value)) {
        AUDIO_DEVICE_LOG_ERR("write register fail: [%04x] = %04x", reg, value);
        return HDF_FAILURE;
    }

    AUDIO_DEVICE_LOG_DEBUG("success");
    return HDF_SUCCESS;
}

int32_t Rk3568DaiDeviceInit(struct AudioCard *card, const struct DaiDevice *dai)
{
    struct DaiData *data = NULL;
    (void)card;

    if (dai == NULL || dai->device == NULL || dai->devDaiName == NULL) {
        AUDIO_DEVICE_LOG_ERR("input para is NULL.");
        return HDF_ERR_INVALID_PARAM;
    }
    if (dai == NULL || dai->devData == NULL) {
        AUDIO_DEVICE_LOG_ERR("dai host is NULL.");
        return HDF_FAILURE;
    }
    data = dai->devData;
    if (DaiSetConfigInfoOfControls(data) != HDF_SUCCESS) {
        AUDIO_DEVICE_LOG_ERR("set config info fail.");
        return HDF_FAILURE;
    }

    if (dai->devData->daiInitFlag == true) {
        AUDIO_DRIVER_LOG_DEBUG("dai init complete!");
        return HDF_SUCCESS;
    }

    dai->devData->daiInitFlag = true;
    AUDIO_DEVICE_LOG_DEBUG("success");
    return HDF_SUCCESS;
}

int32_t Rk3568DaiStartup(const struct AudioCard *card, const struct DaiDevice *dai)
{
    (void)card;
    (void)dai;
    return HDF_SUCCESS;
}

int RK3568SetI2sFomartVal(const struct AudioPcmHwParams *param)
{
    int32_t val = 0;
    if (param == NULL) {
        AUDIO_DEVICE_LOG_ERR("input param is null.");
        return -1;
    }
    switch (param->format) {
        case AUDIO_FORMAT_TYPE_PCM_8_BIT:
            val |= I2S_TXCR_VDW(8); // 8-bit
            break;
        case AUDIO_FORMAT_TYPE_PCM_16_BIT:
            val |= I2S_TXCR_VDW(16); // 16-bit
            break;
        case AUDIO_FORMAT_TYPE_PCM_24_BIT:
            val |= I2S_TXCR_VDW(24); // 24-bit
            break;
        case AUDIO_FORMAT_TYPE_PCM_32_BIT:
            val |= I2S_TXCR_VDW(32); // 32-bit
            break;
        default:
            AUDIO_DEVICE_LOG_ERR("format error, format is 0x%x", param->format);
            return -1;
    }
    AUDIO_DEVICE_LOG_DEBUG("sucess");
    return val;
}

int32_t RK3568SetI2sChannels(struct rk3568_i2s_tdm_dev *i2sTdm, const struct AudioPcmHwParams *param)
{
    unsigned int regFmt, fmt;
    int32_t ret = 0;

    if (i2sTdm == NULL || param == NULL) {
        AUDIO_DEVICE_LOG_ERR("input param is null.");
        return HDF_FAILURE;
    }
    if (param->streamType == AUDIO_RENDER_STREAM) {
        regFmt = I2S_TXCR;
    } else {
        regFmt = I2S_RXCR;
    }

    regmap_read(i2sTdm->regmap, regFmt, &fmt);
    fmt &= I2S_TXCR_TFS_MASK;
    if (fmt == 0) { // I2S mode
        switch (param->channels) {
            case 8:               // channels is 8
                ret = I2S_CHN_8;
                break;
            case 6:               // channels is 6
                ret = I2S_CHN_6;
                break;
            case 4:               // channels is 4
                ret = I2S_CHN_4;
                break;
            case 2:               // channels is 2
                ret = I2S_CHN_2;
                break;
            default:
                ret = -EINVAL;
                break;
        }
    }
    AUDIO_DEVICE_LOG_DEBUG("success");
    return ret;
}

int32_t ConfigInfoSetToReg(struct rk3568_i2s_tdm_dev *i2sTdm, const struct AudioPcmHwParams *param,
    unsigned int div_bclk, unsigned int div_lrck, int32_t fmt)
{
    if (i2sTdm == NULL || param == NULL) {
        AUDIO_DEVICE_LOG_ERR("input para is null.");
        return HDF_FAILURE;
    }

    regmap_update_bits(i2sTdm->regmap, I2S_CLKDIV,
        I2S_CLKDIV_TXM_MASK | I2S_CLKDIV_RXM_MASK,
        I2S_CLKDIV_TXM(div_bclk) | I2S_CLKDIV_RXM(div_bclk));
    regmap_update_bits(i2sTdm->regmap, I2S_CKR,
        I2S_CKR_TSD_MASK | I2S_CKR_RSD_MASK,
        I2S_CKR_TSD(div_lrck) | I2S_CKR_RSD(div_lrck));

    if (param->streamType == AUDIO_RENDER_STREAM) {
        regmap_update_bits(i2sTdm->regmap, I2S_TXCR,
            I2S_TXCR_VDW_MASK | I2S_TXCR_CSR_MASK,
            fmt);
    } else {
        regmap_update_bits(i2sTdm->regmap, I2S_RXCR,
            I2S_RXCR_VDW_MASK | I2S_RXCR_CSR_MASK,
            fmt);
    }
    AUDIO_DEVICE_LOG_DEBUG("success");
    return HDF_SUCCESS;
}

// i2s_tdm->mclk_tx_freq ? i2s_tdm->mclk_rx_freq ?
int32_t RK3568I2sTdmSetMclk(struct rk3568_i2s_tdm_dev *i2sTdm, const struct AudioPcmHwParams *param)
{
    int32_t ret = 0;
    unsigned int mclkRate = 0;
    unsigned int bclkRate = 0;
    unsigned int bclkDiv = 0;
    unsigned int lrclkDiv = 0;
    int32_t fmt = 0;
    int32_t channels = 0;
    if (i2sTdm == NULL || param == NULL) {
        AUDIO_DEVICE_LOG_ERR("input param is null.");
        return HDF_FAILURE;
    }

    if (param->streamType == AUDIO_RENDER_STREAM) {
        ret = clk_set_rate(i2sTdm->mclk_tx, i2sTdm->mclk_tx_freq);
        if (ret) {
            AUDIO_DEVICE_LOG_ERR(" clk_set_rate ret = %d", ret);
            return ret;
        }
        mclkRate = clk_get_rate(i2sTdm->mclk_tx);
    } else if (param->streamType == AUDIO_CAPTURE_STREAM) {
        ret = clk_set_rate(i2sTdm->mclk_rx, i2sTdm->mclk_rx_freq);
        if (ret) {
            AUDIO_DEVICE_LOG_ERR(" clk_set_rate ret = %d", ret);
            return ret;
        }
        mclkRate = clk_get_rate(i2sTdm->mclk_rx);
    } else {
        AUDIO_DEVICE_LOG_ERR("streamType is invalid.");
        return HDF_FAILURE;
    }

    bclkRate = i2sTdm->bclk_fs * param->rate;
    bclkDiv = DIV_ROUND_CLOSEST(mclkRate, bclkRate);
    lrclkDiv = bclkRate / param->rate;
    fmt = RK3568SetI2sFomartVal(param);
    if (fmt < 0) {
        AUDIO_DEVICE_LOG_ERR(" RK3568SetI2sFomartVal fmt = %d", fmt);
        return HDF_FAILURE;
    }
    channels = RK3568SetI2sChannels(i2sTdm, param);
    if (channels < 0) {
        AUDIO_DEVICE_LOG_ERR(" RK3568SetI2sFomartVal channels = %d", channels);
        return HDF_FAILURE;
    }

    fmt |= channels;
    ret = ConfigInfoSetToReg(i2sTdm, param, bclkDiv, lrclkDiv, fmt);
    if (ret != HDF_SUCCESS) {
        AUDIO_DEVICE_LOG_ERR(" ConfigInfoSetToReg ret= %d", ret);
        return HDF_FAILURE;
    }

    AUDIO_DEVICE_LOG_DEBUG("success");
    return HDF_SUCCESS;
}

int32_t RK3568I2sTdmSetSysClk(struct rk3568_i2s_tdm_dev *i2sTdm, const struct AudioPcmHwParams *param)
{
    uint32_t sampleRate;
    uint32_t mclk_parent_freq = 0;

    if (i2sTdm == NULL || param == NULL) {
        AUDIO_DEVICE_LOG_ERR("input para is null.");
        return HDF_FAILURE;
    }
    sampleRate = param->rate;

    /* Put set mclk rate into rockchip_i2s_tdm_set_mclk() */
    switch (sampleRate) {
        case AUDIO_DEVICE_SAMPLE_RATE_8000:
        case AUDIO_DEVICE_SAMPLE_RATE_16000:
        case AUDIO_DEVICE_SAMPLE_RATE_24000:
        case AUDIO_DEVICE_SAMPLE_RATE_32000:
        case AUDIO_DEVICE_SAMPLE_RATE_48000:
        case AUDIO_DEVICE_SAMPLE_RATE_64000:
        case AUDIO_DEVICE_SAMPLE_RATE_96000:
            mclk_parent_freq = i2sTdm->bclk_fs * AUDIO_DEVICE_SAMPLE_RATE_192000;
            break;
        case AUDIO_DEVICE_SAMPLE_RATE_11025:
        case AUDIO_DEVICE_SAMPLE_RATE_22050:
        case AUDIO_DEVICE_SAMPLE_RATE_44100:
            mclk_parent_freq = i2sTdm->bclk_fs * AUDIO_DEVICE_SAMPLE_RATE_176400;
            break;
        default:
            AUDIO_DEVICE_LOG_ERR("Invalid LRCK freq: %u Hz\n", sampleRate);
            return HDF_FAILURE;
    }
    i2sTdm->mclk_tx_freq = mclk_parent_freq;
    i2sTdm->mclk_rx_freq = mclk_parent_freq;
    
    return HDF_SUCCESS;
}

int32_t Rk3568DaiHwParams(const struct AudioCard *card, const struct AudioPcmHwParams *param)
{
    int ret;
    uint32_t bitWidth;
    struct DaiDevice *dai = NULL;
    struct DaiData *data = DaiDataFromCard(card);
    struct platform_device *platformdev = NULL;
    struct rk3568_i2s_tdm_dev *i2sTdm = NULL;

    if (card == NULL || card->rtd == NULL || param == NULL || param->cardServiceName == NULL) {
        AUDIO_DEVICE_LOG_ERR("input para is NULL.");
        return HDF_ERR_INVALID_PARAM;
    }
    if (data == NULL) {
        AUDIO_DEVICE_LOG_ERR("data is nullptr.");
        return HDF_FAILURE;
    }
    dai = card->rtd->cpuDai;
    platformdev = GetPlatformDev(dai);
    if (platformdev == NULL) {
        AUDIO_DEVICE_LOG_ERR("platformdev is NULL.");
        return HDF_FAILURE;
    }
    data->pcmInfo.channels = param->channels;

    AUDIO_DEVICE_LOG_DEBUG("channels count : %d .", param->channels);
    if (AudioFormatToBitWidth(param->format, &bitWidth) != HDF_SUCCESS) {
        AUDIO_DEVICE_LOG_ERR("AudioFormatToBitWidth error");
        return HDF_FAILURE;
    }
    data->pcmInfo.bitWidth = bitWidth;
    data->pcmInfo.rate = param->rate;
    data->pcmInfo.streamType = param->streamType;

    i2sTdm = dev_get_drvdata(&platformdev->dev);
    if (i2sTdm == NULL) {
        AUDIO_DEVICE_LOG_ERR("i2sTdm is null");
        return HDF_FAILURE;
    }
    ret = RK3568I2sTdmSetSysClk(i2sTdm, param);
    if (ret != HDF_SUCCESS) {
        AUDIO_DEVICE_LOG_ERR("RK3568I2sTdmSetSysClk error");
        return HDF_FAILURE;
    }

    ret = RK3568I2sTdmSetMclk(i2sTdm, param);
    if (ret != HDF_SUCCESS) {
        AUDIO_DEVICE_LOG_ERR("RK3568I2sTdmSetMclk error");
        return HDF_FAILURE;
    }
    AUDIO_DEVICE_LOG_DEBUG("success");
    return HDF_SUCCESS;
}

static int GetTriggeredFlag(int cmd)
{
    int32_t triggerFlag = false;
    switch (cmd) {
        case AUDIO_DRV_PCM_IOCTL_RENDER_START:
        case AUDIO_DRV_PCM_IOCTL_RENDER_RESUME:
        case AUDIO_DRV_PCM_IOCTL_CAPTURE_START:
        case AUDIO_DRV_PCM_IOCTL_CAPTURE_RESUME:
            triggerFlag = true;
            break;

        case AUDIO_DRV_PCM_IOCTL_RENDER_STOP:
        case AUDIO_DRV_PCM_IOCTL_CAPTURE_STOP:
        case AUDIO_DRV_PCM_IOCTL_RENDER_PAUSE:
        case AUDIO_DRV_PCM_IOCTL_CAPTURE_PAUSE:
            triggerFlag = false;
            break;

        default:
            triggerFlag = false;
    }
    AUDIO_DEVICE_LOG_DEBUG("success");
    return triggerFlag;
}


static int GetStreamType(int cmd)
{
    enum AudioStreamType streamType = AUDIO_CAPTURE_STREAM;
    switch (cmd) {
        case AUDIO_DRV_PCM_IOCTL_RENDER_START:
        case AUDIO_DRV_PCM_IOCTL_RENDER_RESUME:
        case AUDIO_DRV_PCM_IOCTL_RENDER_STOP:
        case AUDIO_DRV_PCM_IOCTL_RENDER_PAUSE:
            streamType = AUDIO_RENDER_STREAM;
            break;
        case AUDIO_DRV_PCM_IOCTL_CAPTURE_START:
        case AUDIO_DRV_PCM_IOCTL_CAPTURE_RESUME:
        case AUDIO_DRV_PCM_IOCTL_CAPTURE_STOP:
        case AUDIO_DRV_PCM_IOCTL_CAPTURE_PAUSE:
            streamType = AUDIO_CAPTURE_STREAM;
            break;

        default:
            break;
    }
    AUDIO_DEVICE_LOG_DEBUG("success");
    return streamType;
}

static int32_t Rk3568TxAndRxStart(struct rk3568_i2s_tdm_dev *i2sTdm, enum AudioStreamType streamType)
{
    uint32_t val = 0;

    if (streamType == AUDIO_RENDER_STREAM) {
        if (i2sTdm->mclk_tx == NULL) {
            AUDIO_DEVICE_LOG_ERR("mclk tx is null");
            return HDF_FAILURE;
        }

        if (clk_prepare_enable(i2sTdm->mclk_tx) != HDF_SUCCESS) {
            AUDIO_DEVICE_LOG_ERR("clk_prepare_enable fail");
            return HDF_FAILURE;
        }

        if (regmap_update_bits(i2sTdm->regmap, I2S_DMACR, I2S_DMACR_TDE_ENABLE, I2S_DMACR_TDE_ENABLE)) {
            AUDIO_DEVICE_LOG_ERR("regmap_update_bits fail");
            return HDF_FAILURE;
        }
        i2sTdm->txStart = true;
    } else {
        if (i2sTdm->mclk_rx == NULL) {
            AUDIO_DEVICE_LOG_ERR("mclk rx is null");
            return HDF_FAILURE;
        }

        if (clk_prepare_enable(i2sTdm->mclk_rx) != HDF_SUCCESS) {
            AUDIO_DEVICE_LOG_ERR("clk_prepare_enable fail");
            return HDF_FAILURE;
        }

        if (regmap_update_bits(i2sTdm->regmap, I2S_DMACR, I2S_DMACR_RDE_ENABLE, I2S_DMACR_RDE_ENABLE)) {
            AUDIO_DEVICE_LOG_ERR("regmap_update_bits fail");
            return HDF_FAILURE;
        }

        if (regmap_read(i2sTdm->regmap, I2S_DMACR, &val)) {
            AUDIO_DEVICE_LOG_ERR("read register fail: [%04x]", I2S_DMACR);
            return HDF_FAILURE;
        }
        i2sTdm->rxStart = true;
    }

    if (regmap_update_bits(i2sTdm->regmap, I2S_XFER, I2S_XFER_TXS_START | I2S_XFER_RXS_START,
        I2S_XFER_TXS_START | I2S_XFER_RXS_START) != HDF_SUCCESS) {
        AUDIO_DEVICE_LOG_ERR("regmap_update_bits fail");
        return HDF_FAILURE;
    }

    if (regmap_read(i2sTdm->regmap, I2S_XFER, &val)) {
        AUDIO_DEVICE_LOG_ERR("read register fail: [%04x]", I2S_XFER);
        return HDF_FAILURE;
    }

    return HDF_SUCCESS;
}

static int32_t Rk3568TxAndRxStop(struct rk3568_i2s_tdm_dev *i2sTdm, enum AudioStreamType streamType)
{
    uint32_t val = 0;
    int retry = 10; // retry 10 times
    int ret;

    if (streamType == AUDIO_RENDER_STREAM) {
        ret = regmap_update_bits(i2sTdm->regmap, I2S_DMACR, I2S_DMACR_TDE_ENABLE, I2S_DMACR_TDE_DISABLE);
        if (ret != HDF_SUCCESS) {
            AUDIO_DEVICE_LOG_ERR("regmap_update_bits fail");
            return HDF_FAILURE;
        }
        i2sTdm->txStart = false;
    } else {
        ret = regmap_update_bits(i2sTdm->regmap, I2S_DMACR, I2S_DMACR_RDE_ENABLE, I2S_DMACR_RDE_DISABLE);
        if (ret != HDF_SUCCESS) {
            AUDIO_DEVICE_LOG_ERR("regmap_update_bits fail");
            return HDF_FAILURE;
        }
        i2sTdm->rxStart = false;
    }
    if (i2sTdm->txStart || i2sTdm->rxStart) {
        return HDF_SUCCESS;
    }

    ret = regmap_update_bits(i2sTdm->regmap, I2S_XFER,
        I2S_XFER_TXS_START | I2S_XFER_RXS_START, I2S_XFER_TXS_STOP | I2S_XFER_RXS_STOP);
    if (ret != HDF_SUCCESS) {
        AUDIO_DEVICE_LOG_ERR("regmap_update_bits fail");
        return HDF_FAILURE;
    }

    udelay(150);
    ret = regmap_update_bits(i2sTdm->regmap, I2S_CLR, I2S_CLR_TXC | I2S_CLR_RXC, I2S_CLR_TXC | I2S_CLR_RXC);
    if (ret != HDF_SUCCESS) {
        AUDIO_DEVICE_LOG_ERR("regmap_update_bits fail");
        return HDF_FAILURE;
    }

    ret = regmap_read(i2sTdm->regmap, I2S_CLR, &val);
    if (ret != HDF_SUCCESS) {
        AUDIO_DEVICE_LOG_ERR("regmap_read fail");
        return HDF_FAILURE;
    }

    /* Should wait for clear operation to finish */
    while (val && retry > 0) {
        ret = regmap_read(i2sTdm->regmap, I2S_CLR, &val);
        if (ret != HDF_SUCCESS) {
            AUDIO_DEVICE_LOG_ERR("regmap_read fail");
            return HDF_FAILURE;
        }

        retry--;
    }

    return HDF_SUCCESS;
}

static int32_t Rk3568TxAndRxSetReg(struct rk3568_i2s_tdm_dev *i2sTdm,
    enum AudioStreamType streamType, int on)
{
    int ret;

    if (i2sTdm == NULL || i2sTdm->regmap == NULL) {
        AUDIO_DEVICE_LOG_ERR("i2sTdm is null");
        return HDF_FAILURE;
    }

    if (on) {       // when start/resume
        ret = Rk3568TxAndRxStart(i2sTdm, streamType);
        if (ret != HDF_SUCCESS) {
            AUDIO_DEVICE_LOG_ERR("Rk3568TxAndRxStart is failed");
            return HDF_FAILURE;
        }
    } else {     // when stop/pause
        ret = Rk3568TxAndRxStop(i2sTdm, streamType);
        if (ret != HDF_SUCCESS) {
            AUDIO_DEVICE_LOG_ERR("Rk3568TxAndRxStop is failed");
            return HDF_FAILURE;
        }
    }

    AUDIO_DEVICE_LOG_DEBUG("success");
    return HDF_SUCCESS;
}

/* normal scene */
int32_t Rk3568NormalTrigger(const struct AudioCard *card, int cmd, const struct DaiDevice *device)
{
    struct platform_device *platformdev = GetPlatformDev(device);
    struct rk3568_i2s_tdm_dev *i2sTdm = NULL;
    int32_t triggerFlag = GetTriggeredFlag(cmd);
    enum AudioStreamType streamType = GetStreamType(cmd);
    int ret;

    if (platformdev == NULL) {
        AUDIO_DEVICE_LOG_ERR("platformdev is null");
        return HDF_FAILURE;
    }

    i2sTdm = dev_get_drvdata(&platformdev->dev);
    ret = Rk3568TxAndRxSetReg(i2sTdm, streamType, triggerFlag);
    if (ret != HDF_SUCCESS) {
        AUDIO_DEVICE_LOG_ERR("Rk3568TxAndRxSetReg is failed");
        return HDF_FAILURE;
    }

    AUDIO_DEVICE_LOG_DEBUG("success");
    return HDF_SUCCESS;
}
