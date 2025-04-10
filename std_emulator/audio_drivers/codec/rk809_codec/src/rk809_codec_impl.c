/*
 * Copyright (c) 2022 Institute of Software, CAS.
 * Author : huangji@nj.iscas.ac.cn
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include <linux/regmap.h>
#include "gpio_if.h"
#include "linux/of_gpio.h"
#include "audio_driver_log.h"
#include "audio_stream_dispatch.h"
#include "audio_codec_base.h"
#include "audio_sapm.h"
#include "rk817_codec.h"
#include "rk809_codec_impl.h"

#define HDF_LOG_TAG "rk809_codec"

/* RK809 I2C Device address  */
#define RK809_I2C_DEV_ADDR     (0x20)
#define RK809_I2C_BUS_NUMBER   (0)      // i2c0
#define RK809_I2C_WAIT_TIMES   (10)     // ms
#define RK809_CODEC_REG_MAX    (0x4f)
#define RK809_REG_INDEX0       0
#define RK809_REG_INDEX1       1
#define RK809_REG_INDEX2       2
#define RK809_REG_INDEX3       3
#define RK809_REG_INDEX4       4
#define RK809_REG_INDEX5       5
#define RK809_REG_INDEX6       6
#define RK809_REG_INDEX7       7

struct Rk809TransferData {
    uint16_t i2cDevAddr;
    uint16_t i2cBusNumber;
    uint32_t CfgCtrlCount;
    struct AudioRegCfgGroupNode **RegCfgGroupNode;
    struct AudioKcontrol *Controls;
};

static const struct AudioSapmRoute g_audioRoutes[] = {
    { "SPKL", NULL, "SPKL PGA"},
    { "HPL", NULL, "HPL PGA"},
    { "HPR", NULL, "HPR PGA"},
    { "SPKL PGA", "Speaker1 Switch", "DAC1"},
    { "HPL PGA", "Headphone1 Switch", "DAC2"},
    { "HPR PGA", "Headphone2 Switch", "DAC3"},

    { "ADCL", NULL, "LPGA"},
    { "ADCR", NULL, "RPGA"},
    { "LPGA", "LPGA MIC Switch", "MIC1"},
    { "RPGA", "RPGA MIC Switch", "MIC2"},
};

struct RegDefaultVal {
        uint16_t reg;
        uint16_t val;
};

struct RegConfig {
    const struct RegDefaultVal *regVal;
    uint32_t size;
};

static const struct RegDefaultVal g_rk817RenderStartRegDefaults[] = {
    { RK817_CODEC_DDAC_POPD_DACST, 0x04 },
    { RK817_CODEC_DI2S_RXCMD_TSD, 0x20 },
};

static const struct RegConfig g_rk817RenderStartRegConfig = {
    .regVal = g_rk817RenderStartRegDefaults,
    .size = ARRAY_SIZE(g_rk817RenderStartRegDefaults),
};

static const struct RegDefaultVal g_rk817RenderStopRegDefaults[] = {
    { RK817_CODEC_DDAC_POPD_DACST, 0x06 },
    { RK817_CODEC_DI2S_RXCMD_TSD, 0x10 },
};

static const struct RegConfig g_rk817RenderStopRegConfig = {
    .regVal = g_rk817RenderStopRegDefaults,
    .size = ARRAY_SIZE(g_rk817RenderStopRegDefaults),
};

static const struct RegDefaultVal g_rk817CaptureStartRegDefaults[] = {
    { RK817_CODEC_DTOP_DIGEN_CLKE, 0xff },   // I2SRX_EN I2SRX_CKE ADC_EN  ADC_CKE
    { RK817_CODEC_DI2S_TXCR3_TXCMD, 0x88 },
};

static const struct RegConfig g_rk817CaptureStartRegConfig = {
    .regVal = g_rk817CaptureStartRegDefaults,
    .size = ARRAY_SIZE(g_rk817CaptureStartRegDefaults),
};

static const struct RegDefaultVal g_rk817CaptureStopRegDefaults[] = {
    { RK817_CODEC_DTOP_DIGEN_CLKE, 0x0f },
    { RK817_CODEC_DI2S_TXCR3_TXCMD, 0x40 },
};

static const struct RegConfig g_rk817CaptureStopRegConfig = {
    .regVal = g_rk817CaptureStopRegDefaults,
    .size = ARRAY_SIZE(g_rk817CaptureStopRegDefaults),
};

int32_t Rk809DeviceRegRead(uint32_t reg, uint32_t *val)
{
    struct Rk809ChipData *chip = GetCodecDevice();
    if (chip == NULL) {
        AUDIO_DRIVER_LOG_ERR("get codec device failed.");
        return HDF_FAILURE;
    }

    if (regmap_read(chip->regmap, reg, val)) {
        AUDIO_DRIVER_LOG_ERR("read register fail: [%x]", reg);
        return HDF_FAILURE;
    }
    return HDF_SUCCESS;
}

int32_t Rk809DeviceRegWrite(uint32_t reg, uint32_t value)
{
    struct Rk809ChipData *chip = GetCodecDevice();
    if (chip == NULL) {
        AUDIO_DRIVER_LOG_ERR("get codec device failed.");
        return HDF_FAILURE;
    }

    if (regmap_write(chip->regmap, reg, value)) {
        AUDIO_DRIVER_LOG_ERR("write register fail: [%x] = %x", reg, value);
        return HDF_FAILURE;
    }
    return HDF_SUCCESS;
}

int32_t RK809CodecDaiReadReg(const struct DaiDevice *dai, uint32_t reg, uint32_t *value)
{
    if (value == NULL) {
        AUDIO_DRIVER_LOG_ERR("param val is null.");
        return HDF_FAILURE;
    }
    if (reg > RK809_CODEC_REG_MAX) {
        AUDIO_DRIVER_LOG_ERR("codec dai read error reg: %x ", reg);
        return HDF_FAILURE;
    }

    if (Rk809DeviceRegRead(reg, value)) {
        AUDIO_DRIVER_LOG_ERR("codec dai read register fail: [%04x]", reg);
        return HDF_FAILURE;
    }

    return HDF_SUCCESS;
}
int32_t RK809CodecDaiWriteReg(const struct DaiDevice *dai, uint32_t reg, uint32_t value)
{
    if (reg > RK809_CODEC_REG_MAX) {
        AUDIO_DRIVER_LOG_ERR("codec dai write error reg: %x ", reg);
        return HDF_FAILURE;
    }

    if (Rk809DeviceRegWrite(reg, value)) {
        AUDIO_DRIVER_LOG_ERR("codec dai write register fail: [%04x] = %04x", reg, value);
        return HDF_FAILURE;
    }
    return HDF_SUCCESS;
}

static const RK809SampleRateTimes RK809GetSRT(const uint32_t rate)
{
    switch (rate) {
        case AUDIO_SAMPLE_RATE_8000:
            return RK809_SRT_00;
        case AUDIO_SAMPLE_RATE_16000:
            return RK809_SRT_01;
        case AUDIO_SAMPLE_RATE_32000:
        case AUDIO_SAMPLE_RATE_44100:
        case AUDIO_SAMPLE_RATE_48000:
            return RK809_SRT_02;
        case AUDIO_SAMPLE_RATE_96000:
            return RK809_SRT_03;
        default:
            AUDIO_DEVICE_LOG_DEBUG("unsupport samplerate %d\n", rate);
            return RK809_SRT_02;
    }
}

static const RK809PLLInputCLKPreDIV RK809GetPremode(const uint32_t rate)
{
    switch (rate) {
        case AUDIO_SAMPLE_RATE_8000:
            return RK809_PREMODE_1;
        case AUDIO_SAMPLE_RATE_16000:
            return RK809_PREMODE_2;
        case AUDIO_SAMPLE_RATE_32000:
        case AUDIO_SAMPLE_RATE_44100:
        case AUDIO_SAMPLE_RATE_48000:
            return RK809_PREMODE_3;
        case AUDIO_SAMPLE_RATE_96000:
            return RK809_PREMODE_4;
        default:
            AUDIO_DEVICE_LOG_DEBUG("unsupport samplerate %d\n", rate);
            return RK809_PREMODE_3;
    }
}

static const RK809_VDW RK809GetI2SDataWidth(const uint32_t bitWidth)
{
    switch (bitWidth) {
        case BIT_WIDTH16:
            return RK809_VDW_16BITS;
        case BIT_WIDTH24:
        case BIT_WIDTH32:
            return RK809_VDW_24BITS;
        default:
            AUDIO_DEVICE_LOG_ERR("unsupport sample bit width %d.\n", bitWidth);
            return RK809_VDW_16BITS;
    }
}

int32_t RK809UpdateRenderParams(struct DaiDevice *codecDai, struct AudioRegCfgGroupNode **regCfgGroup,
    struct RK809DaiParamsVal codecDaiParamsVal)
{
    int32_t ret;
    struct AudioMixerControl *regAttr = NULL;
    int32_t itemNum;

    if (regCfgGroup == NULL || regCfgGroup[AUDIO_DAI_PATAM_GROUP] == NULL
        || regCfgGroup[AUDIO_DAI_PATAM_GROUP]->regCfgItem == NULL) {
        AUDIO_DEVICE_LOG_ERR("input invalid parameter.");
        return HDF_FAILURE;
    }

    itemNum = regCfgGroup[AUDIO_DAI_PATAM_GROUP]->itemNum;
    regAttr = regCfgGroup[AUDIO_DAI_PATAM_GROUP]->regCfgItem;
    if (regAttr == NULL) {
        AUDIO_DEVICE_LOG_ERR("reg Cfg Item is null.");
        return HDF_FAILURE;
    }

    regAttr[RK809_REG_INDEX0].value = RK809GetPremode(codecDaiParamsVal.frequencyVal);
    ret = AudioDaiRegUpdate(codecDai, &regAttr[RK809_REG_INDEX0]);
    if (ret != HDF_SUCCESS) {
        AUDIO_DRIVER_LOG_ERR("set freq failed.");
        return HDF_FAILURE;
    }

    regAttr[RK809_REG_INDEX5].value = 0;
    ret = AudioDaiRegUpdate(codecDai, &regAttr[RK809_REG_INDEX5]);
    if (ret != HDF_SUCCESS) {
        AUDIO_DRIVER_LOG_ERR("set freq failed.");
        return HDF_FAILURE;
    }

    regAttr[RK809_REG_INDEX1].value = RK809GetSRT(codecDaiParamsVal.frequencyVal);
    ret = AudioDaiRegUpdate(codecDai, &regAttr[RK809_REG_INDEX1]);
    if (ret != HDF_SUCCESS) {
        AUDIO_DRIVER_LOG_ERR("set freq failed.");
        return HDF_FAILURE;
    }

    regAttr[RK809_REG_INDEX5].value = 0xf;
    ret = AudioDaiRegUpdate(codecDai, &regAttr[RK809_REG_INDEX5]);
    if (ret != HDF_SUCCESS) {
        AUDIO_DRIVER_LOG_ERR("set freq failed.");
        return HDF_FAILURE;
    }

    regAttr[RK809_REG_INDEX4].value = RK809GetI2SDataWidth(codecDaiParamsVal.DataWidthVal);
    ret = AudioDaiRegUpdate(codecDai, &regAttr[RK809_REG_INDEX4]);
    if (ret != HDF_SUCCESS) {
        AUDIO_DRIVER_LOG_ERR("set freq failed.");
        return HDF_FAILURE;
    }

    return HDF_SUCCESS;
}

int32_t RK809UpdateCaptureParams(struct DaiDevice *codecDai, struct AudioRegCfgGroupNode **regCfgGroup,
    struct RK809DaiParamsVal codecDaiParamsVal)
{
    int32_t ret;
    struct AudioMixerControl *regAttr = NULL;
    int32_t itemNum;

    if (regCfgGroup == NULL || regCfgGroup[AUDIO_DAI_PATAM_GROUP] == NULL
        || regCfgGroup[AUDIO_DAI_PATAM_GROUP]->regCfgItem == NULL) {
        AUDIO_DEVICE_LOG_ERR("input invalid parameter.");
        return HDF_FAILURE;
    }

    itemNum = regCfgGroup[AUDIO_DAI_PATAM_GROUP]->itemNum;
    regAttr = regCfgGroup[AUDIO_DAI_PATAM_GROUP]->regCfgItem;
    if (regAttr == NULL) {
        AUDIO_DEVICE_LOG_ERR("reg Cfg Item is null.");
        return HDF_FAILURE;
    }

    regAttr[RK809_REG_INDEX0].value = RK809GetPremode(codecDaiParamsVal.frequencyVal);
    ret = AudioDaiRegUpdate(codecDai, &regAttr[RK809_REG_INDEX0]);
    if (ret != HDF_SUCCESS) {
        AUDIO_DRIVER_LOG_ERR("set freq failed.");
        return HDF_FAILURE;
    }

    regAttr[RK809_REG_INDEX6].value = 0x0;
    ret = AudioDaiRegUpdate(codecDai, &regAttr[RK809_REG_INDEX6]);
    if (ret != HDF_SUCCESS) {
        AUDIO_DRIVER_LOG_ERR("set freq failed.");
        return HDF_FAILURE;
    }

    regAttr[RK809_REG_INDEX2].value = RK809GetSRT(codecDaiParamsVal.frequencyVal);
    ret = AudioDaiRegUpdate(codecDai, &regAttr[RK809_REG_INDEX2]);
    if (ret != HDF_SUCCESS) {
        AUDIO_DRIVER_LOG_ERR("set freq failed.");
        return HDF_FAILURE;
    }

    regAttr[RK809_REG_INDEX6].value = 0xf;
    ret = AudioDaiRegUpdate(codecDai, &regAttr[RK809_REG_INDEX6]);
    if (ret != HDF_SUCCESS) {
        AUDIO_DRIVER_LOG_ERR("set freq failed.");
        return HDF_FAILURE;
    }

    regAttr[RK809_REG_INDEX3].value = RK809GetI2SDataWidth(codecDaiParamsVal.DataWidthVal);
    ret = AudioDaiRegUpdate(codecDai, &regAttr[RK809_REG_INDEX3]);
    if (ret != HDF_SUCCESS) {
        AUDIO_DRIVER_LOG_ERR("set freq failed.");
        return HDF_FAILURE;
    }

    return HDF_SUCCESS;
}

int32_t RK809DaiParamsUpdate(struct DaiDevice *codecDai, enum AudioStreamType streamType,
    struct RK809DaiParamsVal codecDaiParamsVal)
{
    int32_t ret;
    struct AudioRegCfgGroupNode **regCfgGroup = NULL;

    if (codecDai == NULL || codecDai->devData == NULL) {
        AUDIO_DEVICE_LOG_ERR("input invalid parameter.");
        return HDF_FAILURE;
    }
    regCfgGroup = codecDai->devData->regCfgGroup;

    if (regCfgGroup == NULL || regCfgGroup[AUDIO_DAI_PATAM_GROUP] == NULL
        || regCfgGroup[AUDIO_DAI_PATAM_GROUP]->regCfgItem == NULL) {
        AUDIO_DEVICE_LOG_ERR("regCfgGroup is invalid.");
        return HDF_FAILURE;
    }

    if (streamType == AUDIO_RENDER_STREAM) {
        ret = RK809UpdateRenderParams(codecDai, regCfgGroup, codecDaiParamsVal);
        if (ret != HDF_SUCCESS) {
            AUDIO_DEVICE_LOG_ERR("RK809UpdateRenderParams failed.");
            return HDF_FAILURE;
        }
    } else if (streamType == AUDIO_CAPTURE_STREAM) {
        ret = RK809UpdateCaptureParams(codecDai, regCfgGroup, codecDaiParamsVal);
        if (ret != HDF_SUCCESS) {
            AUDIO_DEVICE_LOG_ERR("RK809UpdateCaptureParams failed.");
            return HDF_FAILURE;
        }
    } else {
        AUDIO_DEVICE_LOG_ERR("streamType is invalid.");
        return HDF_FAILURE;
    }
    return HDF_SUCCESS;
}

int32_t RK809CodecReadReg(const struct CodecDevice *codec, uint32_t reg, uint32_t *val)
{
    if (val == NULL) {
        AUDIO_DRIVER_LOG_ERR("param val is null.");
        return HDF_FAILURE;
    }
    if (reg > RK809_CODEC_REG_MAX) {
        AUDIO_DRIVER_LOG_ERR("read error reg: %x ", reg);
        return HDF_FAILURE;
    }

    if (Rk809DeviceRegRead(reg, val)) {
        AUDIO_DRIVER_LOG_ERR("read register fail: [%04x]", reg);
        return HDF_FAILURE;
    }
    return HDF_SUCCESS;
}

int32_t Rk809CodecWriteReg(const struct CodecDevice *codec, uint32_t reg, uint32_t value)
{
    if (reg > RK809_CODEC_REG_MAX) {
        AUDIO_DRIVER_LOG_ERR("write error reg: %x ", reg);
        return HDF_FAILURE;
    }
    if (Rk809DeviceRegWrite(reg, value)) {
        AUDIO_DRIVER_LOG_ERR("write register fail: [%04x] = %04x", reg, value);
        return HDF_FAILURE;
    }
    return HDF_SUCCESS;
}

int32_t Rk809DeviceInit(struct AudioCard *audioCard, const struct CodecDevice *device)
{
    int32_t ret;

    if (audioCard == NULL || device == NULL || device->devData == NULL ||
        device->devData->sapmComponents == NULL || device->devData->controls == NULL) {
        AUDIO_DRIVER_LOG_ERR("input para is NULL.");
        return HDF_ERR_INVALID_OBJECT;
    }

    ret = CodecDeviceInitRegConfig(device);
    if (ret != HDF_SUCCESS) {
        AUDIO_DEVICE_LOG_ERR("RK809RegDefaultInit failed.");
        return HDF_FAILURE;
    }

    if (AudioAddControls(audioCard, device->devData->controls, device->devData->numControls) != HDF_SUCCESS) {
        AUDIO_DRIVER_LOG_ERR("add controls failed.");
        return HDF_FAILURE;
    }

    if (AudioSapmNewComponents(audioCard, device->devData->sapmComponents,
        device->devData->numSapmComponent) != HDF_SUCCESS) {
        AUDIO_DRIVER_LOG_ERR("new components failed.");
        return HDF_FAILURE;
    }

    if (AudioSapmAddRoutes(audioCard, g_audioRoutes, HDF_ARRAY_SIZE(g_audioRoutes)) != HDF_SUCCESS) {
        AUDIO_DRIVER_LOG_ERR("add route failed.");
        return HDF_FAILURE;
    }

    if (AudioSapmNewControls(audioCard) != HDF_SUCCESS) {
        AUDIO_DRIVER_LOG_ERR("add sapm controls failed.");
        return HDF_FAILURE;
    }

    if (AudioSapmSleep(audioCard) != HDF_SUCCESS) {
        AUDIO_DRIVER_LOG_ERR("add sapm sleep modular failed.");
        return HDF_FAILURE;
    }

    AUDIO_DRIVER_LOG_DEBUG("success.");
    return HDF_SUCCESS;
}

int32_t Rk809DaiDeviceInit(struct AudioCard *card, const struct DaiDevice *device)
{
    if (device == NULL || device->devDaiName == NULL) {
        AUDIO_DEVICE_LOG_ERR("input para is NULL.");
        return HDF_FAILURE;
    }
    (void)card;
    AUDIO_DEVICE_LOG_DEBUG("success.");
    return HDF_SUCCESS;
}

int32_t Rk809DaiStartup(const struct AudioCard *card, const struct DaiDevice *device)
{
    (void)card;
    (void)device;

    AUDIO_DRIVER_LOG_DEBUG("success.");
    return HDF_SUCCESS;
}

int32_t Rk809FormatToBitWidth(enum AudioFormat format, uint32_t *bitWidth)
{
    if (bitWidth == NULL) {
        AUDIO_DRIVER_LOG_ERR("bitWidth is null.");
        return HDF_FAILURE;
    }
    switch (format) {
        case AUDIO_FORMAT_TYPE_PCM_16_BIT:
            *bitWidth = DATA_BIT_WIDTH16;
            break;

        case AUDIO_FORMAT_TYPE_PCM_24_BIT:
            *bitWidth = DATA_BIT_WIDTH24;
            break;

        default:
            AUDIO_DRIVER_LOG_ERR("format: %d is not support.", format);
            return HDF_FAILURE;
    }
    return HDF_SUCCESS;
}


int32_t Rk809DaiHwParams(const struct AudioCard *card, const struct AudioPcmHwParams *param)
{
    int32_t ret;
    uint32_t bitWidth;
    struct RK809DaiParamsVal codecDaiParamsVal;
    (void)card;

    if (param == NULL || param->cardServiceName == NULL || card == NULL ||
        card->rtd == NULL || card->rtd->codecDai == NULL || card->rtd->codecDai->devData == NULL ||
        card->rtd->codecDai->devData->regCfgGroup == NULL) {
        AUDIO_DRIVER_LOG_ERR("input para is NULL.");
        return HDF_FAILURE;
    }

    ret = Rk809FormatToBitWidth(param->format, &bitWidth);
    if (ret != HDF_SUCCESS) {
        return HDF_FAILURE;
    }

    codecDaiParamsVal.frequencyVal = param->rate;
    codecDaiParamsVal.DataWidthVal = bitWidth;

    AUDIO_DRIVER_LOG_DEBUG("channels count : %d .", param->channels);
    ret =  RK809DaiParamsUpdate(card->rtd->codecDai, param->streamType, codecDaiParamsVal);
    if (ret != HDF_SUCCESS) {
        AUDIO_DEVICE_LOG_ERR("RK809DaiParamsUpdate failed.");
        return HDF_FAILURE;
    }

    return HDF_SUCCESS;
}

int32_t RK809DeviceRegConfig(const struct RegConfig regConfig)
{
    int32_t index;
    int32_t ret;

    for (index = 0; index < regConfig.size; index++) {
        ret = Rk809DeviceRegWrite(regConfig.regVal[index].reg, regConfig.regVal[index].val);
        if (ret != HDF_SUCCESS) {
            AUDIO_DEVICE_LOG_ERR("Rk809DeviceRegWrite failed.");
            return HDF_FAILURE;
        }
    }

    return HDF_SUCCESS;
}


/* normal scene */
int32_t Rk809NormalTrigger(const struct AudioCard *card, int32_t cmd, const struct DaiDevice *device)
{
    int32_t ret;
    switch (cmd) {
        case AUDIO_DRV_PCM_IOCTL_RENDER_START:
        case AUDIO_DRV_PCM_IOCTL_RENDER_RESUME:
            ret = RK809DeviceRegConfig(g_rk817RenderStartRegConfig);
            if (ret != HDF_SUCCESS) {
                AUDIO_DEVICE_LOG_ERR("RK809DeviceRegConfig failed.");
                return HDF_FAILURE;
            }
            break;

        case AUDIO_DRV_PCM_IOCTL_RENDER_STOP:
        case AUDIO_DRV_PCM_IOCTL_RENDER_PAUSE:
            ret = RK809DeviceRegConfig(g_rk817RenderStopRegConfig);
            if (ret != HDF_SUCCESS) {
                AUDIO_DEVICE_LOG_ERR("RK809DeviceRegConfig failed.");
                return HDF_FAILURE;
            }
            break;

        case AUDIO_DRV_PCM_IOCTL_CAPTURE_START:
        case AUDIO_DRV_PCM_IOCTL_CAPTURE_RESUME:
            ret = RK809DeviceRegConfig(g_rk817CaptureStartRegConfig);
            if (ret != HDF_SUCCESS) {
                AUDIO_DEVICE_LOG_ERR("RK809DeviceRegConfig failed.");
                return HDF_FAILURE;
            }
            break;

        case AUDIO_DRV_PCM_IOCTL_CAPTURE_STOP:
        case AUDIO_DRV_PCM_IOCTL_CAPTURE_PAUSE:
            ret = RK809DeviceRegConfig(g_rk817CaptureStopRegConfig);
            if (ret != HDF_SUCCESS) {
                AUDIO_DEVICE_LOG_ERR("RK809DeviceRegConfig failed.");
                return HDF_FAILURE;
            }
            break;

        default:
            break;
    }

    return HDF_SUCCESS;
}
