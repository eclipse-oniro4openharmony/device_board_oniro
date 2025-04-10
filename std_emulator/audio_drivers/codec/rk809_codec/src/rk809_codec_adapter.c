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

#include <linux/device.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/mfd/rk808.h>
#include "rk817_codec.h"
#include "rk809_codec_impl.h"
#include "audio_codec_if.h"
#include "audio_codec_base.h"
#include "audio_driver_log.h"

#define HDF_LOG_TAG "rk809_codec_adapter"

struct CodecData g_rk809Data = {
    .Init = Rk809DeviceInit,
    .Read = RK809CodecReadReg,
    .Write = Rk809CodecWriteReg,
};

struct AudioDaiOps g_rk809DaiDeviceOps = {
    .Startup = Rk809DaiStartup,
    .HwParams = Rk809DaiHwParams,
    .Trigger = Rk809NormalTrigger,
};

struct DaiData g_rk809DaiData = {
    .DaiInit = Rk809DaiDeviceInit,
    .Read = RK809CodecDaiReadReg,
    .Write = RK809CodecDaiWriteReg,
    .ops = &g_rk809DaiDeviceOps,
};

static struct Rk809ChipData *g_chip;
struct Rk809ChipData* GetCodecDevice(void)
{
    return g_chip;
}

/* HdfDriverEntry implementations */
static int32_t Rk809DriverBind(struct HdfDeviceObject *device)
{
    struct CodecHost *codecHost;
    if (device == NULL) {
        AUDIO_DRIVER_LOG_ERR("input para is NULL.");
        return HDF_FAILURE;
    }

    codecHost = (struct CodecHost *)OsalMemCalloc(sizeof(*codecHost));
    if (codecHost == NULL) {
        AUDIO_DRIVER_LOG_ERR("malloc codecHost fail!");
        return HDF_FAILURE;
    }
    codecHost->device = device;
    device->service = &codecHost->service;

    return HDF_SUCCESS;
}

static int32_t Rk809DriverInit(struct HdfDeviceObject *device)
{
    int32_t ret;
    struct regmap_config codecRegmapCfg = getCodecRegmap();
    struct platform_device *codeDev = GetCodecPlatformDevice();
    struct rk808 *rk808 = NULL;
    if (!codeDev) {
        AUDIO_DEVICE_LOG_ERR("codeDev not ready");
        return HDF_FAILURE;
    }
    g_chip = devm_kzalloc(&codeDev->dev, sizeof(struct Rk809ChipData), GFP_KERNEL);
    if (!g_chip) {
        AUDIO_DEVICE_LOG_ERR("devm_kzalloc for g_chip failed!");
        return HDF_ERR_MALLOC_FAIL;
    }
    g_chip->codec = g_rk809Data;
    g_chip->dai = g_rk809DaiData;
    platform_set_drvdata(codeDev, g_chip);
    g_chip->pdev = codeDev;
    rk808 = dev_get_drvdata(g_chip->pdev->dev.parent);
    if (!rk808) {
        return HDF_FAILURE;
    }

    g_chip->regmap = devm_regmap_init_i2c(rk808->i2c, &codecRegmapCfg);
    if (IS_ERR(g_chip->regmap)) {
        AUDIO_DEVICE_LOG_ERR("failed to allocate regmap: %ld\n", PTR_ERR(g_chip->regmap));
        return HDF_FAILURE;
    }

    if (CodecDaiGetPortConfigInfo(device, &g_rk809DaiData) != HDF_SUCCESS) {
        return HDF_FAILURE;
    }

    if (CodecGetConfigInfo(device, &g_rk809Data) !=  HDF_SUCCESS) {
        return HDF_FAILURE;
    }
    if (CodecSetConfigInfoOfControls(&g_rk809Data,  &g_rk809DaiData) != HDF_SUCCESS) {
        return HDF_FAILURE;
    }
    ret = CodecGetServiceName(device, &(g_rk809Data.drvCodecName));
    if (ret != HDF_SUCCESS) {
        return ret;
    }
    ret = CodecGetDaiName(device,  &(g_rk809DaiData.drvDaiName));
    if (ret != HDF_SUCCESS) {
        return HDF_FAILURE;
    }
    OsalMutexInit(&g_rk809Data.mutex);
    OsalMutexInit(&g_rk809DaiData.mutex);
    ret = AudioRegisterCodec(device, &g_rk809Data, &g_rk809DaiData);
    if (ret != HDF_SUCCESS) {
        return ret;
    }
    return HDF_SUCCESS;
}

static void RK809DriverRelease(struct HdfDeviceObject *device)
{
    struct CodecHost *codecHost;
    if (device == NULL) {
        AUDIO_DRIVER_LOG_ERR("device is NULL");
        return;
    }

    if (g_chip) {
        platform_set_drvdata(g_chip->pdev, NULL);
        if (g_chip->regmap) {
            regmap_exit(g_chip->regmap);
        }
        devm_kfree(&g_chip->pdev->dev, g_chip);
    }
    OsalMutexDestroy(&g_rk809Data.mutex);
    OsalMutexDestroy(&g_rk809DaiData.mutex);

    if (device->priv != NULL) {
        OsalMemFree(device->priv);
    }
    codecHost = (struct CodecHost *)device->service;
    if (codecHost == NULL) {
        HDF_LOGE("CodecDriverRelease: codecHost is NULL");
        return;
    }
    OsalMemFree(codecHost);
}

/* HdfDriverEntry definitions */
struct HdfDriverEntry g_Rk809DriverEntry = {
    .moduleVersion = 1,
    .moduleName = "CODEC_RK809",
    .Bind = Rk809DriverBind,
    .Init = Rk809DriverInit,
    .Release = RK809DriverRelease,
};

HDF_INIT(g_Rk809DriverEntry);
