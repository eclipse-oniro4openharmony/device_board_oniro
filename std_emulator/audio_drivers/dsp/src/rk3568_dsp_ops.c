/*
 * Copyright (C) 2022 HiHope Open Source Organization .
 *
 * HDF is dual licensed: you can use it either under the terms of
 * the GPL, or the BSD license, at your option.
 * See the LICENSE file in the root of this repository for complete details.
 */

#include "rk3568_dsp_ops.h"
#include "spi_if.h"
#include "audio_dsp_if.h"
#include "audio_driver_log.h"

#define HDF_LOG_TAG rk3568_dsp_ops

int32_t DspDaiStartup(const struct AudioCard *card, const struct DaiDevice *device)
{
    (void)card;
    (void)device;
    return HDF_SUCCESS;
}

int32_t DspDaiHwParams(const struct AudioCard *card, const struct AudioPcmHwParams *param)
{
    (void)card;
    (void)param;
    AUDIO_DRIVER_LOG_DEBUG("success.");
    return HDF_SUCCESS;
}


int32_t DspDeviceInit(const struct DspDevice *device)
{
    (void)device;
    return HDF_SUCCESS;
}

int32_t DspDeviceReadReg(const struct DspDevice *device, const void *msgs, const uint32_t len)
{
    (void)device;
    (void)msgs;
    return HDF_SUCCESS;
}

int32_t DspDeviceWriteReg(const struct DspDevice *device, const void *msgs, const uint32_t len)
{
    (void)device;
    (void)msgs;
    return HDF_SUCCESS;
}

int32_t DspDaiDeviceInit(struct AudioCard *card, const struct DaiDevice *device)
{
    (void)card;
    (void)device;
    return HDF_SUCCESS;
}

int32_t DspDecodeAudioStream(const struct AudioCard *card, const uint8_t *buf, const struct DspDevice *device)
{
    (void)card;
    (void)buf;
    (void)device;
    AUDIO_DRIVER_LOG_DEBUG("decode run!!!");
    return HDF_SUCCESS;
}

int32_t DspEncodeAudioStream(const struct AudioCard *card, const uint8_t *buf, const struct DspDevice *device)
{
    (void)card;
    (void)buf;
    (void)device;
    AUDIO_DRIVER_LOG_DEBUG("encode run!!!");
    return HDF_SUCCESS;
}


int32_t DspEqualizerActive(const struct AudioCard *card, const uint8_t *buf, const struct DspDevice *device)
{
    (void)card;
    (void)buf;
    (void)device;
    AUDIO_DRIVER_LOG_DEBUG("equalizer run!!!");
    return HDF_SUCCESS;
}
