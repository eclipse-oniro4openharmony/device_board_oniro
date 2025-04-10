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

#ifndef RK809_CODEC_IMPL_H
#define RK809_CODEC_IMPL_H

#include <linux/platform_device.h>
#include <linux/types.h>
#include "audio_dai_if.h"
#include "audio_codec_if.h"
#include "osal_mem.h"
#include "osal_time.h"
#include "osal_io.h"
#include "securec.h"

#ifdef __cplusplus
#if __cplusplus
extern "C" {
#endif
#endif /* __cplusplus */

struct Rk809ChipData {
    struct CodecData codec;
    struct DaiData dai;
    struct HdfDeviceObject *hdev;
    struct platform_device *pdev;
    struct regmap *regmap;
};

typedef enum {
    RK809_SRT_00 = 0x00,
    RK809_SRT_01 = 0x01,
    RK809_SRT_02 = 0x02,
    RK809_SRT_03 = 0x03,
} RK809SampleRateTimes;

typedef enum {
    RK809_PREMODE_1 = 0x03,
    RK809_PREMODE_2 = 0x06,
    RK809_PREMODE_3 = 0x0C,
    RK809_PREMODE_4 = 0x18,
} RK809PLLInputCLKPreDIV;

typedef enum {
    RK809_VDW_16BITS = 0x0F,
    RK809_VDW_24BITS = 0x17,
} RK809_VDW;

typedef enum {
    UPDATE_LREG = 0,
    UPDATE_RREG = 1,
} Update_Dest;

struct RK809DaiParamsVal {
    uint32_t frequencyVal;
    uint32_t DataWidthVal;
};
struct Rk809ChipData* GetCodecDevice(void);
int32_t Rk809DeviceInit(struct AudioCard *audioCard, const struct CodecDevice *device);
int32_t Rk809DeviceRegRead(uint32_t reg, uint32_t *val);
int32_t Rk809DeviceRegWrite(uint32_t reg, uint32_t value);
int32_t RK809CodecReadReg(const struct CodecDevice *codec, uint32_t reg, uint32_t *val);
int32_t Rk809CodecWriteReg(const struct CodecDevice *codec, uint32_t reg, uint32_t value);
int32_t Rk809RegBitsUpdate(struct AudioMixerControl regAttr);
int32_t Rk809DaiDeviceInit(struct AudioCard *card, const struct DaiDevice *device);
int32_t Rk809DaiStartup(const struct AudioCard *card, const struct DaiDevice *device);
int32_t Rk809DaiHwParams(const struct AudioCard *card, const struct AudioPcmHwParams *param);
int32_t Rk809NormalTrigger(const struct AudioCard *card, int cmd, const struct DaiDevice *device);
int32_t RK809CodecDaiReadReg(const struct DaiDevice *dai, uint32_t reg, uint32_t *value);
int32_t RK809CodecDaiWriteReg(const struct DaiDevice *dai, uint32_t reg, uint32_t value);

#ifdef __cplusplus
#if __cplusplus
}
#endif
#endif

#endif
