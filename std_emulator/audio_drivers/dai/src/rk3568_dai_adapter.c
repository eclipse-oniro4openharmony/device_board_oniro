/*
 * Copyright (C) 2022 HiHope Open Source Organization .
 *
 * HDF is dual licensed: you can use it either under the terms of
 * the GPL, or the BSD license, at your option.
 * See the LICENSE file in the root of this repository for complete details.
 */
#include <linux/io.h>
#include <linux/delay.h>
#include "audio_core.h"
#include "audio_host.h"
#include "audio_platform_base.h"
#include "osal_io.h"
#include "audio_driver_log.h"
#include "rk3568_dai_ops.h"
#include "audio_dai_base.h"

struct AudioDaiOps g_daiDeviceOps = {
    .Startup = Rk3568DaiStartup,
    .HwParams = Rk3568DaiHwParams,
    .Trigger = Rk3568NormalTrigger,
};

/* HdfDriverEntry implementations */
static int32_t DaiDriverBind(struct HdfDeviceObject *device)
{
    struct DaiHost *daiHost = NULL;
    AUDIO_DRIVER_LOG_DEBUG("entry!");

    if (device == NULL) {
        AUDIO_DEVICE_LOG_ERR("input para is NULL.");
        return HDF_FAILURE;
    }

    daiHost = (struct DaiHost *)OsalMemCalloc(sizeof(*daiHost));
    if (daiHost == NULL) {
        AUDIO_DEVICE_LOG_ERR("malloc host fail!");
        return HDF_FAILURE;
    }

    daiHost->device = device;
    device->service = &daiHost->service;

    AUDIO_DRIVER_LOG_DEBUG("success!");
    return HDF_SUCCESS;
}


static int32_t DaiGetServiceName(const struct HdfDeviceObject *device, struct DaiData *daiData)
{
    const struct DeviceResourceNode *node = NULL;
    struct DeviceResourceIface *drsOps = NULL;
    int32_t ret;
    AUDIO_DRIVER_LOG_DEBUG("entry!");

    if (device == NULL || daiData == NULL) {
        AUDIO_DEVICE_LOG_ERR("input para is nullptr.");
        return HDF_FAILURE;
    }

    node = device->property;
    if (node == NULL) {
        AUDIO_DEVICE_LOG_ERR("drs node is nullptr.");
        return HDF_FAILURE;
    }
    drsOps = DeviceResourceGetIfaceInstance(HDF_CONFIG_SOURCE);
    if (drsOps == NULL || drsOps->GetString == NULL) {
        AUDIO_DEVICE_LOG_ERR("invalid drs ops fail!");
        return HDF_FAILURE;
    }

    ret = drsOps->GetString(node, "serviceName", &daiData->drvDaiName, 0);
    if (ret != HDF_SUCCESS) {
        AUDIO_DEVICE_LOG_ERR("read serviceName fail!");
        return ret;
    }

    AUDIO_DRIVER_LOG_DEBUG("success!");
    return HDF_SUCCESS;
}

static int32_t DaiDriverInit(struct HdfDeviceObject *device)
{
    int32_t ret = 0;
    struct DaiData *daiData = NULL;
    struct DaiHost *daiHost = NULL;

    AUDIO_DRIVER_LOG_DEBUG("entry!");
    if (device == NULL) {
        AUDIO_DEVICE_LOG_ERR("device is nullptr.");
        return HDF_ERR_INVALID_OBJECT;
    }

    daiHost = (struct DaiHost *)device->service;
    if (daiHost == NULL) {
        AUDIO_DEVICE_LOG_ERR("daiHost is NULL");
        return HDF_FAILURE;
    }

    daiData = (struct DaiData *)OsalMemCalloc(sizeof(*daiData));
    if (daiData == NULL) {
        AUDIO_DEVICE_LOG_ERR("malloc DaiData fail!");
        return HDF_FAILURE;
    }
    daiData->Read = Rk3568DeviceReadReg,
    daiData->Write = Rk3568DeviceWriteReg,
    daiData->DaiInit = Rk3568DaiDeviceInit,
    daiData->ops = &g_daiDeviceOps,
    daiData->daiInitFlag = false;
    OsalMutexInit(&daiData->mutex);
    daiHost->priv = daiData;

    if (DaiGetConfigInfo(device, daiData) !=  HDF_SUCCESS) {
        AUDIO_DEVICE_LOG_ERR("get dai data fail.");
        OsalMemFree(daiData);
        return HDF_FAILURE;
    }

    if (DaiGetServiceName(device, daiData) !=  HDF_SUCCESS) {
        AUDIO_DEVICE_LOG_ERR("get service name fail.");
        OsalMemFree(daiData);
        return HDF_FAILURE;
    }

    ret = AudioSocRegisterDai(device, (void *)daiData);
    if (ret !=  HDF_SUCCESS) {
        AUDIO_DEVICE_LOG_ERR("register dai fail.");
        OsalMemFree(daiData);
        return ret;
    }

    AUDIO_DRIVER_LOG_DEBUG("success.\n");
    return HDF_SUCCESS;
}

static void DaiDriverRelease(struct HdfDeviceObject *device)
{
    struct DaiHost *daiHost = NULL;
    struct DaiData *daiData = NULL;

    AUDIO_DRIVER_LOG_DEBUG("entry!");
    if (device == NULL) {
        AUDIO_DEVICE_LOG_ERR("device is NULL");
        return;
    }

    daiHost = (struct DaiHost *)device->service;
    if (daiHost == NULL) {
        AUDIO_DEVICE_LOG_ERR("daiHost is NULL");
        return;
    }

    daiData = (struct DaiData *)daiHost->priv;
    if (daiData != NULL) {
        OsalMutexDestroy(&daiData->mutex);
        OsalMemFree(daiData);
    }

    OsalMemFree(daiHost);
    AUDIO_DRIVER_LOG_DEBUG("success!");
}

/* HdfDriverEntry definitions */
struct HdfDriverEntry g_daiDriverEntry = {
    .moduleVersion = 1,
    .moduleName = "DAI_RK3568",
    .Bind = DaiDriverBind,
    .Init = DaiDriverInit,
    .Release = DaiDriverRelease,
};
HDF_INIT(g_daiDriverEntry);
