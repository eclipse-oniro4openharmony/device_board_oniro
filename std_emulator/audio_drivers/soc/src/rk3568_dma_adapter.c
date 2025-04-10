/*
 * Copyright (C) 2022 HiHope Open Source Organization .
 *
 * HDF is dual licensed: you can use it either under the terms of
 * the GPL, or the BSD license, at your option.
 * See the LICENSE file in the root of this repository for complete details.
 */
 
#include <linux/slab.h>
#include "gpio_if.h"
#include "audio_core.h"
#include "audio_platform_base.h"
#include "audio_dma_base.h"
#include "rk3568_dma_ops.h"
#include "osal_io.h"
#include "osal_mem.h"
#include "audio_driver_log.h"

#define HDF_LOG_TAG rk3568_platform_adapter

struct AudioDmaOps g_dmaDeviceOps = {
    .DmaBufAlloc = Rk3568DmaBufAlloc,
    .DmaBufFree = Rk3568DmaBufFree,
    .DmaRequestChannel = Rk3568DmaRequestChannel,
    .DmaConfigChannel = Rk3568DmaConfigChannel,
    .DmaPrep = Rk3568DmaPrep,
    .DmaSubmit = Rk3568DmaSubmit,
    .DmaPending = Rk3568DmaPending,
    .DmaPause = Rk3568DmaPause,
    .DmaResume = Rk3568DmaResume,
    .DmaPointer = Rk3568PcmPointer,
};

/* HdfDriverEntry implementations */
static int32_t PlatformDriverBind(struct HdfDeviceObject *device)
{
    struct PlatformHost *platformHost = NULL;

    if (device == NULL) {
        AUDIO_DEVICE_LOG_ERR("input para is NULL.");
        return HDF_FAILURE;
    }

    platformHost = (struct PlatformHost *)OsalMemCalloc(sizeof(*platformHost));
    if (platformHost == NULL) {
        AUDIO_DEVICE_LOG_ERR("malloc host fail!");
        return HDF_FAILURE;
    }

    platformHost->device = device;
    device->service = &platformHost->service;

    AUDIO_DEVICE_LOG_DEBUG("success!");
    return HDF_SUCCESS;
}

static int32_t PlatformGetServiceName(const struct HdfDeviceObject *device, struct PlatformData *platformData)
{
    const struct DeviceResourceNode *node = NULL;
    struct DeviceResourceIface *drsOps = NULL;
    int32_t ret;

    if (device == NULL || platformData == NULL) {
        AUDIO_DEVICE_LOG_ERR("para is NULL.");
        return HDF_FAILURE;
    }

    node = device->property;
    if (node == NULL) {
        AUDIO_DEVICE_LOG_ERR("node is NULL.");
        return HDF_FAILURE;
    }

    drsOps = DeviceResourceGetIfaceInstance(HDF_CONFIG_SOURCE);
    if (drsOps == NULL || drsOps->GetString == NULL) {
        AUDIO_DEVICE_LOG_ERR("get drsops object instance fail!");
        return HDF_FAILURE;
    }

    ret = drsOps->GetString(node, "serviceName", &platformData->drvPlatformName, 0);
    if (ret != HDF_SUCCESS) {
        AUDIO_DEVICE_LOG_ERR("read serviceName fail!");
        return ret;
    }
    AUDIO_DEVICE_LOG_DEBUG("success!");

    return HDF_SUCCESS;
}

static int32_t PlatformDriverInit(struct HdfDeviceObject *device)
{
    int32_t ret;
    struct PlatformData *platformData = NULL;
    struct PlatformHost *platformHost = NULL;

    if (device == NULL) {
        AUDIO_DEVICE_LOG_ERR("device is NULL.");
        return HDF_ERR_INVALID_OBJECT;
    }
    platformHost = (struct PlatformHost *)device->service;
    if (platformHost == NULL) {
        AUDIO_DEVICE_LOG_ERR("platformHost is NULL");
        return HDF_FAILURE;
    }

    platformData = (struct PlatformData *)OsalMemCalloc(sizeof(*platformData));
    if (platformData == NULL) {
        AUDIO_DEVICE_LOG_ERR("malloc PlatformData fail!");
        return HDF_FAILURE;
    }

    ret = PlatformGetServiceName(device, platformData);
    if (ret !=  HDF_SUCCESS) {
        OsalMemFree(platformData);
        return ret;
    }

    platformData->PlatformInit = AudioDmaDeviceInit;
    platformData->ops = &g_dmaDeviceOps;
    if (AudioDmaGetConfigInfo(device, platformData) !=  HDF_SUCCESS) {
        OsalMemFree(platformData);
        return HDF_FAILURE;
    }

    OsalMutexInit(&platformData->renderBufInfo.buffMutex);
    OsalMutexInit(&platformData->captureBufInfo.buffMutex);
    ret = AudioSocRegisterPlatform(device, platformData);
    if (ret !=  HDF_SUCCESS) {
        OsalMemFree(platformData);
        return ret;
    }

    platformHost->priv = platformData;
    AUDIO_DEVICE_LOG_DEBUG("success.\n");
    return HDF_SUCCESS;
}

static void PlatformDriverRelease(struct HdfDeviceObject *device)
{
    struct PlatformData *platformData = NULL;
    struct PlatformHost *platformHost = NULL;
    if (device == NULL) {
        AUDIO_DEVICE_LOG_ERR("device is NULL");
        return;
    }

    platformHost = (struct PlatformHost *)device->service;
    if (platformHost == NULL) {
        AUDIO_DEVICE_LOG_ERR("platformHost is NULL");
        return;
    }

    platformData = (struct PlatformData *)platformHost->priv;
    if (platformData != NULL) {
        OsalMutexDestroy(&platformData->renderBufInfo.buffMutex);
        OsalMutexDestroy(&platformData->captureBufInfo.buffMutex);
        OsalMemFree(platformData);
    }

    OsalMemFree(platformHost);
    AUDIO_DEVICE_LOG_DEBUG("success.\n");
    return;
}

/* HdfDriverEntry definitions */
struct HdfDriverEntry g_platformDriverEntry = {
    .moduleVersion = 1,
    .moduleName = "DMA_RK3568",
    .Bind = PlatformDriverBind,
    .Init = PlatformDriverInit,
    .Release = PlatformDriverRelease,
};
HDF_INIT(g_platformDriverEntry);
