/*
 * Copyright (C) 2022 HiHope Open Source Organization .
 *
 * HDF is dual licensed: you can use it either under the terms of
 * the GPL, or the BSD license, at your option.
 * See the LICENSE file in the root of this repository for complete details.
 */
#include <sound/memalloc.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/notifier.h>
#include <linux/of.h>
#include <linux/of_dma.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/stat.h>
#include <linux/string.h>
#include <linux/sysfs.h>
#include <linux/suspend.h>

#include "audio_platform_base.h"
#include "audio_dma_base.h"
#include "osal_io.h"
#include "osal_uaccess.h"
#include "audio_driver_log.h"
#include "rk3568_dma_ops.h"

#define HDF_LOG_TAG rk3568_platform_ops

#define I2S_TXDR 0x24
#define I2S_RXDR 0x28

#define DMA_TX_CHANNEL 0
#define DMA_RX_CHANNEL 1

#define DMA_CHANNEL_MAX 2

struct DmaRuntimeData {
    struct dma_chan *dmaChn[DMA_CHANNEL_MAX];
    dma_cookie_t cookie[DMA_CHANNEL_MAX];
    struct device *dmaDev;
    char *i2sDtsTreePath;
    struct device_node *dmaOfNode;
    uint32_t i2sAddr;
    struct HdfDeviceObject *device;
};

static int32_t GetDmaDevice(struct PlatformData *data)
{
    struct DmaRuntimeData *dmaRtd = NULL;
    struct platform_device *platformdev = NULL;
    const char *i2s1DtsTreePath;

    if (data == NULL || data->regConfig == NULL) {
        AUDIO_DEVICE_LOG_ERR("PlatformData is null.");
        return HDF_FAILURE;
    }

    dmaRtd = (struct DmaRuntimeData *)data->dmaPrv;
    if (dmaRtd == NULL) {
        AUDIO_DEVICE_LOG_ERR("dmaRtd is null.");
        return HDF_FAILURE;
    }

    i2s1DtsTreePath = data->regConfig->audioIdInfo.chipName;
    dmaRtd->i2sAddr = data->regConfig->audioIdInfo.chipIdRegister;
    dmaRtd->dmaOfNode = of_find_node_by_path(i2s1DtsTreePath);
    if (dmaRtd->dmaOfNode == NULL) {
        AUDIO_DEVICE_LOG_ERR("get device node failed.");
        return HDF_FAILURE;
    }

    platformdev = of_find_device_by_node(dmaRtd->dmaOfNode);
    if (platformdev == NULL) {
        AUDIO_DEVICE_LOG_ERR("get platformdev failed.");
        return HDF_FAILURE;
    }

    dmaRtd->dmaDev = &platformdev->dev;
    return HDF_SUCCESS;
}

static int32_t GetDmaChannel(struct PlatformData *data)
{
    struct DmaRuntimeData *dmaRtd = NULL;
    struct device_node    *dmaOfNode = NULL;
    struct device *dmaDevice = NULL;
    struct property *dma_names = NULL;
    const char *dma_name = NULL;
    bool hasRender = false;
    bool hasCapture = false;
    static const char * const dmaChannelNames[] = {
        [DMA_TX_CHANNEL] = "tx", [DMA_RX_CHANNEL] = "rx",
    };

    dmaRtd = (struct DmaRuntimeData *)data->dmaPrv;
    if (dmaRtd == NULL) {
        AUDIO_DEVICE_LOG_ERR("dmaRtd is null.");
        return HDF_FAILURE;
    }

    dmaOfNode = dmaRtd->dmaOfNode;
    if (dmaOfNode == NULL) {
        AUDIO_DEVICE_LOG_ERR("dmaOfNode is null.");
        return HDF_FAILURE;
    }
    of_property_for_each_string(dmaOfNode, "dma-names", dma_names, dma_name) {
        if (strcmp(dma_name, "rx") == 0) {
            hasCapture = true;
        }
        if (strcmp(dma_name, "tx") == 0) {
            hasRender = true;
        }
    }

    dmaDevice = dmaRtd->dmaDev;
    if (dmaDevice == NULL) {
        AUDIO_DEVICE_LOG_ERR("dmaDevice is null.");
        return HDF_FAILURE;
    }
    if (hasRender) {
        dmaRtd->dmaChn[DMA_TX_CHANNEL] = dma_request_slave_channel(dmaDevice, dmaChannelNames[DMA_TX_CHANNEL]);
        if (dmaRtd->dmaChn[DMA_TX_CHANNEL] == NULL) {
            AUDIO_DEVICE_LOG_ERR("dma_request_slave_channel DMA_TX_CHANNEL failed");
            return HDF_FAILURE;
        }
    }
    if (hasCapture) {
        dmaRtd->dmaChn[DMA_RX_CHANNEL] = dma_request_slave_channel(dmaDevice, dmaChannelNames[DMA_RX_CHANNEL]);
        if (dmaRtd->dmaChn[DMA_RX_CHANNEL] == NULL) {
            AUDIO_DEVICE_LOG_ERR("dma_request_slave_channel DMA_RX_CHANNEL failed");
            return HDF_FAILURE;
        }
    }
    return HDF_SUCCESS;
}

static int32_t DmaRtdInit(struct PlatformData *data)
{
    struct DmaRuntimeData *dmaRtd = NULL;
    int ret;

    if (data == NULL || data->regConfig == NULL) {
        AUDIO_DEVICE_LOG_ERR("data is null.");
        return HDF_FAILURE;
    }

    dmaRtd = kzalloc(sizeof(*dmaRtd), GFP_KERNEL);
    if (!dmaRtd) {
        AUDIO_DEVICE_LOG_ERR("AudioRenderBuffInit: fail.");
        return HDF_FAILURE;
    }
    data->dmaPrv = dmaRtd;

    ret = GetDmaDevice(data);
    if (ret != HDF_SUCCESS) {
        AUDIO_DEVICE_LOG_ERR("GetDmaDevice: fail.");
        return HDF_FAILURE;
    }

    ret = GetDmaChannel(data);
    if (ret != HDF_SUCCESS) {
        AUDIO_DEVICE_LOG_ERR("GetDmaChannel: fail.");
        return HDF_FAILURE;
    }

    AUDIO_DEVICE_LOG_DEBUG("success.");
    return HDF_SUCCESS;
}

int32_t AudioDmaDeviceInit(const struct AudioCard *card, const struct PlatformDevice *platform)
{
    struct PlatformData *data = NULL;
    struct DmaRuntimeData *dmaRtd = NULL;
    int ret;
    (void)platform;

    if (card == NULL) {
        AUDIO_DEVICE_LOG_ERR("card is null.");
        return HDF_FAILURE;
    }

    data = PlatformDataFromCard(card);
    if (data == NULL) {
        AUDIO_DEVICE_LOG_ERR("PlatformDataFromCard failed.");
        return HDF_FAILURE;
    }

    if (data->platformInitFlag) {
        AUDIO_DRIVER_LOG_DEBUG("platform init complete!");
        return HDF_SUCCESS;
    }

    ret = DmaRtdInit(data);
    if (ret != HDF_SUCCESS) {
        AUDIO_DEVICE_LOG_ERR("DmaRtdInit failed.");
        return HDF_FAILURE;
    }

    dmaRtd = (struct DmaRuntimeData *)data->dmaPrv;
    if (dmaRtd == NULL) {
        AUDIO_DEVICE_LOG_ERR("dmaRtd is null.");
        return HDF_FAILURE;
    }
    dmaRtd->device = card->device;

    data->platformInitFlag = true;
    AUDIO_DEVICE_LOG_DEBUG("success.");
    return HDF_SUCCESS;
}

int32_t Rk3568DmaBufAlloc(struct PlatformData *data, const enum AudioStreamType streamType)
{
    uint32_t preallocBufSize;
    struct DmaRuntimeData *dmaRtd = NULL;
    struct device *dmaDevice = NULL;

    if (data == NULL) {
        AUDIO_DEVICE_LOG_ERR("data is null");
        return HDF_FAILURE;
    }

    dmaRtd = (struct DmaRuntimeData *)data->dmaPrv;
    if (dmaRtd == NULL) {
        AUDIO_DEVICE_LOG_ERR("dmaRtd is null.");
        return HDF_FAILURE;
    }

    dmaDevice = dmaRtd->dmaDev;
    if (dmaDevice == NULL) {
        AUDIO_DEVICE_LOG_ERR("dmaDevice is null");
        return HDF_FAILURE;
    }

    if (streamType == AUDIO_CAPTURE_STREAM) {
        if (data->captureBufInfo.virtAddr == NULL) {
            preallocBufSize = data->captureBufInfo.cirBufMax;
            dmaDevice->coherent_dma_mask = 0xffffffffUL;
            data->captureBufInfo.virtAddr = dma_alloc_wc(dmaDevice, preallocBufSize,
                (dma_addr_t *)&data->captureBufInfo.phyAddr, GFP_DMA | GFP_KERNEL);
        }
    } else if (streamType == AUDIO_RENDER_STREAM) {
        if (data->renderBufInfo.virtAddr == NULL) {
            preallocBufSize = data->renderBufInfo.cirBufMax;
            dmaDevice->coherent_dma_mask = 0xffffffffUL;
            data->renderBufInfo.virtAddr = dma_alloc_wc(dmaDevice, preallocBufSize,
                (dma_addr_t *)&data->renderBufInfo.phyAddr, GFP_DMA | GFP_KERNEL);
        }
    } else {
        AUDIO_DEVICE_LOG_ERR("stream Type is invalude.");
        return HDF_FAILURE;
    }
    
    AUDIO_DEVICE_LOG_DEBUG("success.");
    return HDF_SUCCESS;
}

int32_t Rk3568DmaBufFree(struct PlatformData *data, const enum AudioStreamType streamType)
{
    struct DmaRuntimeData *dmaRtd = NULL;
    struct device *dmaDevice = NULL;

    if (data == NULL || data->dmaPrv == NULL) {
        AUDIO_DEVICE_LOG_ERR("data is null");
        return HDF_FAILURE;
    }

    dmaRtd = (struct DmaRuntimeData *)data->dmaPrv;
    dmaDevice = dmaRtd->dmaDev;
    if (dmaDevice == NULL) {
        AUDIO_DEVICE_LOG_ERR("dmaDevice is null");
        return HDF_FAILURE;
    }

    if (streamType == AUDIO_CAPTURE_STREAM) {
        dma_free_wc(dmaDevice, data->captureBufInfo.cirBufMax, data->captureBufInfo.virtAddr,
                    data->captureBufInfo.phyAddr);
    } else if (streamType == AUDIO_RENDER_STREAM) {
        dma_free_wc(dmaDevice, data->renderBufInfo.cirBufMax, data->renderBufInfo.virtAddr,
                    data->renderBufInfo.phyAddr);
    } else {
        AUDIO_DEVICE_LOG_ERR("stream Type is invalude.");
        return HDF_FAILURE;
    }

    AUDIO_DEVICE_LOG_DEBUG("success");
    return HDF_SUCCESS;
}

int32_t  Rk3568DmaRequestChannel(const struct PlatformData *data, const enum AudioStreamType streamType)
{
    (void)data;
    AUDIO_DEVICE_LOG_DEBUG("sucess");
    return HDF_SUCCESS;
}

int32_t Rk3568DmaConfigChannel(const struct PlatformData *data, const enum AudioStreamType streamType)
{
    struct dma_chan *dmaChan = NULL;
    struct dma_slave_config slaveConfig;
    int32_t ret = 0;
    struct DmaRuntimeData *dmaRtd = NULL;

    if (data == NULL || data->dmaPrv == NULL) {
        AUDIO_DEVICE_LOG_ERR("data is null");
        return HDF_FAILURE;
    }

    dmaRtd = (struct DmaRuntimeData *)data->dmaPrv;
    (void)memset_s(&slaveConfig, sizeof(slaveConfig), 0, sizeof(slaveConfig));
    if (streamType == AUDIO_RENDER_STREAM) {
        dmaChan = (struct dma_chan *)dmaRtd->dmaChn[DMA_TX_CHANNEL];   // tx
        slaveConfig.direction = DMA_MEM_TO_DEV;
        slaveConfig.dst_addr_width = DMA_SLAVE_BUSWIDTH_4_BYTES;
        slaveConfig.dst_addr = dmaRtd->i2sAddr + I2S_TXDR;
        slaveConfig.dst_maxburst = 8; // Max Transimit 8 Byte
    } else {
        dmaChan = (struct dma_chan *)dmaRtd->dmaChn[DMA_RX_CHANNEL];
        slaveConfig.direction = DMA_DEV_TO_MEM;
        slaveConfig.src_addr_width = DMA_SLAVE_BUSWIDTH_4_BYTES;
        slaveConfig.src_addr = dmaRtd->i2sAddr + I2S_RXDR;
        slaveConfig.src_maxburst = 8; // Max Transimit 8 Byte
    }
    slaveConfig.device_fc = 0;
    slaveConfig.slave_id = 0;

    if (dmaChan == NULL) {
        AUDIO_DEVICE_LOG_ERR("dmaChan is null");
        return HDF_FAILURE;
    }
    ret = dmaengine_slave_config(dmaChan, &slaveConfig);
    if (ret != 0) {
        AUDIO_DEVICE_LOG_ERR("dmaengine_slave_config failed");
        return HDF_FAILURE;
    }
    AUDIO_DEVICE_LOG_DEBUG("success");
    return HDF_SUCCESS;
}

static int32_t BytesToFrames(uint32_t frameBits, uint32_t size, uint32_t *pointer)
{
    if (pointer == NULL || frameBits == 0) {
        AUDIO_DEVICE_LOG_ERR("input para is error.");
        return HDF_FAILURE;
    }
    *pointer = size / frameBits;
    return HDF_SUCCESS;
}

int32_t Rk3568PcmPointer(struct PlatformData *data, const enum AudioStreamType streamType, uint32_t *pointer)
{
    uint32_t bufSize;
    struct dma_chan *dmaChan = NULL;
    struct dma_tx_state dmaState;
    uint32_t currentPointer;
    int ret;
    struct DmaRuntimeData *dmaRtd = NULL;

    if (data == NULL || data->dmaPrv == NULL) {
        AUDIO_DEVICE_LOG_ERR("data is null");
        return HDF_FAILURE;
    }
    dmaRtd = (struct DmaRuntimeData *)data->dmaPrv;

    if (streamType == AUDIO_RENDER_STREAM) {
        dmaChan = dmaRtd->dmaChn[DMA_TX_CHANNEL];
        bufSize = data->renderBufInfo.cirBufSize;
        if (dmaChan == NULL) {
            AUDIO_DEVICE_LOG_ERR("dmaChan is null");
            return HDF_FAILURE;
        }
        dmaengine_tx_status(dmaChan, dmaRtd->cookie[DMA_TX_CHANNEL], &dmaState);

        if (dmaState.residue > 0) {
            currentPointer = bufSize - dmaState.residue;
            ret = BytesToFrames(data->renderPcmInfo.frameSize, currentPointer, pointer);
            if (ret != HDF_SUCCESS) {
                AUDIO_DEVICE_LOG_ERR("BytesToFrames is failed.");
                return HDF_FAILURE;
            }
        } else {
            *pointer = 0;
        }
    } else {
        dmaChan = dmaRtd->dmaChn[DMA_RX_CHANNEL];
        bufSize = data->captureBufInfo.cirBufSize;
        if (dmaChan == NULL) {
            AUDIO_DEVICE_LOG_ERR("dmaChan is null");
            return HDF_FAILURE;
        }
        dmaengine_tx_status(dmaChan, dmaRtd->cookie[DMA_RX_CHANNEL], &dmaState);

        if (dmaState.residue > 0) {
            currentPointer = bufSize - dmaState.residue;
            ret = BytesToFrames(data->capturePcmInfo.frameSize, currentPointer, pointer);
            if (ret != HDF_SUCCESS) {
                AUDIO_DEVICE_LOG_ERR("BytesToFrames is failed.");
                return HDF_FAILURE;
            }
        } else {
            *pointer = 0;
        }
    }

    return HDF_SUCCESS;
}

int32_t Rk3568DmaPrep(const struct PlatformData *data, const enum AudioStreamType streamType)
{
    (void)data;
    return HDF_SUCCESS;
}

static void RenderPcmDmaComplete(void *arg)
{
    struct PlatformData *data = NULL;
    struct DmaRuntimeData *dmaRtd = NULL;
    struct dma_chan *dmaChan = NULL;

    data = (struct PlatformData *)arg;
    if (data == NULL || data->dmaPrv == NULL) {
        AUDIO_DEVICE_LOG_ERR("data is null.");
        return;
    }

    dmaRtd = (struct DmaRuntimeData *)data->dmaPrv;
    dmaChan = dmaRtd->dmaChn[DMA_TX_CHANNEL];
    if (dmaChan == NULL) {
        AUDIO_DEVICE_LOG_ERR("dmaChan is null");
        return;
    }
    if (!AudioDmaTransferStatusIsNormal(data, AUDIO_RENDER_STREAM)) {
        dmaengine_terminate_async(dmaChan);
    }
}

static void CapturePcmDmaComplete(void *arg)
{
    struct AudioEvent reportMsg;
    struct PlatformData *data = NULL;
    struct DmaRuntimeData *dmaRtd = NULL;
    struct dma_chan *dmaChan = NULL;

    data = (struct PlatformData *)arg;
    if (data == NULL || data->dmaPrv == NULL) {
        AUDIO_DEVICE_LOG_ERR("data is null.");
        return;
    }
    dmaRtd = (struct DmaRuntimeData *)data->dmaPrv;
    dmaChan = dmaRtd->dmaChn[DMA_RX_CHANNEL];
    if (dmaChan == NULL) {
        AUDIO_DEVICE_LOG_ERR("dmaChan is null");
        return;
    }

    if (!AudioDmaTransferStatusIsNormal(data, AUDIO_CAPTURE_STREAM)) {
        dmaengine_terminate_async(dmaChan);
    }
    reportMsg.eventType = HDF_AUDIO_CAPTURE_THRESHOLD;
    reportMsg.deviceType = HDF_AUDIO_PRIMARY_DEVICE;
#ifdef CONFIG_AUDIO_ENABLE_CAP_THRESHOLD
    if (AudioCapSilenceThresholdEvent(dmaRtd->device, &reportMsg) != HDF_SUCCESS) {
        AUDIO_DRIVER_LOG_ERR("AudioCapSilenceThresholdEvent failed.");
    }
#endif
}

int32_t Rk3568DmaSubmit(const struct PlatformData *data, const enum AudioStreamType streamType)
{
    struct dma_async_tx_descriptor *desc = NULL;
    enum dma_transfer_direction direction;
    unsigned long flags = 3;
    struct dma_chan *dmaChan = NULL;
    struct DmaRuntimeData *dmaRtd = NULL;

    if (data == NULL || data->dmaPrv == NULL) {
        AUDIO_DEVICE_LOG_ERR("input para is null.");
        return HDF_FAILURE;
    }

    dmaRtd = (struct DmaRuntimeData *)data->dmaPrv;

    if (streamType == AUDIO_RENDER_STREAM) {
        direction = DMA_MEM_TO_DEV;
        dmaChan = dmaRtd->dmaChn[DMA_TX_CHANNEL];
        if (dmaChan == NULL) {
            AUDIO_DEVICE_LOG_ERR("dmaChan is null");
            return HDF_FAILURE;
        }
        desc = dmaengine_prep_dma_cyclic(dmaChan,
            data->renderBufInfo.phyAddr,
            data->renderBufInfo.cirBufSize,
            data->renderBufInfo.periodSize, direction, flags);
        if (!desc) {
            AUDIO_DEVICE_LOG_ERR("DMA_TX_CHANNEL desc create failed");
            return -ENOMEM;
        }
        desc->callback = RenderPcmDmaComplete;
        desc->callback_param = (void *)data;

        dmaRtd->cookie[DMA_TX_CHANNEL] = dmaengine_submit(desc);
    } else {
        direction = DMA_DEV_TO_MEM;
        dmaChan = dmaRtd->dmaChn[DMA_RX_CHANNEL];
        if (dmaChan == NULL) {
            AUDIO_DEVICE_LOG_ERR("dmaChan is null");
            return HDF_FAILURE;
        }

        desc = dmaengine_prep_dma_cyclic(dmaChan,
            data->captureBufInfo.phyAddr,
            data->captureBufInfo.cirBufSize,
            data->captureBufInfo.periodSize, direction, flags);
        if (!desc) {
            AUDIO_DEVICE_LOG_ERR("DMA_RX_CHANNEL desc create failed");
            return -ENOMEM;
        }
        desc->callback = CapturePcmDmaComplete;
        desc->callback_param = (void *)data;
        dmaRtd->cookie[DMA_RX_CHANNEL] = dmaengine_submit(desc);
    }

    AUDIO_DEVICE_LOG_DEBUG("success");
    return 0;
}

int32_t Rk3568DmaPending(struct PlatformData *data, const enum AudioStreamType streamType)
{
    struct dma_chan *dmaChan = NULL;
    struct DmaRuntimeData *dmaRtd = NULL;

    if (data == NULL || data->dmaPrv == NULL) {
        AUDIO_DEVICE_LOG_ERR("data is null");
        return HDF_FAILURE;
    }

    dmaRtd = (struct DmaRuntimeData *)data->dmaPrv;
    if (dmaRtd->dmaChn[0] == NULL) {
        AUDIO_DEVICE_LOG_ERR("dmaChan is null");
        return HDF_FAILURE;
    }

    AUDIO_DEVICE_LOG_DEBUG("streamType = %d", streamType);
    if (streamType == AUDIO_RENDER_STREAM) {
        dmaChan = dmaRtd->dmaChn[DMA_TX_CHANNEL];
    } else {
        dmaChan = dmaRtd->dmaChn[DMA_RX_CHANNEL];
    }
    if (dmaChan == NULL) {
        AUDIO_DEVICE_LOG_ERR("dmaChan is null");
        return HDF_FAILURE;
    }

    dma_async_issue_pending(dmaChan);
    AUDIO_DEVICE_LOG_DEBUG("dmaChan chan_id = %d.", dmaChan->chan_id);

    AUDIO_DEVICE_LOG_DEBUG("success");
    return HDF_SUCCESS;
}

int32_t Rk3568DmaPause(struct PlatformData *data, const enum AudioStreamType streamType)
{
    struct dma_chan *dmaChan = NULL;
    struct DmaRuntimeData *dmaRtd = NULL;

    if (data == NULL || data->dmaPrv == NULL) {
        AUDIO_DEVICE_LOG_ERR("data is null");
        return HDF_FAILURE;
    }

    dmaRtd = (struct DmaRuntimeData *)data->dmaPrv;
    if (dmaRtd->dmaChn[0] == NULL) {
        AUDIO_DEVICE_LOG_ERR("dmaChan is null");
        return HDF_FAILURE;
    }

    if (streamType == AUDIO_RENDER_STREAM) {
        dmaChan = dmaRtd->dmaChn[DMA_TX_CHANNEL];
    } else {
        dmaChan = dmaRtd->dmaChn[DMA_RX_CHANNEL];
    }
    // can not use dmaengine_pause function
    if (dmaChan == NULL) {
        AUDIO_DEVICE_LOG_ERR("dmaChan is null");
        return HDF_FAILURE;
    }
    dmaengine_terminate_async(dmaChan);

    AUDIO_DEVICE_LOG_DEBUG("success");
    return HDF_SUCCESS;
}
int32_t Rk3568DmaResume(const struct PlatformData *data, const enum AudioStreamType streamType)
{
    int ret;
    struct dma_chan *dmaChan = NULL;
    struct DmaRuntimeData *dmaRtd = NULL;

    if (data == NULL || data->dmaPrv == NULL) {
        AUDIO_DEVICE_LOG_ERR("data is null");
        return HDF_FAILURE;
    }

    dmaRtd = (struct DmaRuntimeData *)data->dmaPrv;
    if (dmaRtd->dmaChn[0] == NULL) {
        AUDIO_DEVICE_LOG_ERR("dmaRtd->dmaChn is null");
        return HDF_FAILURE;
    }

    if (streamType == AUDIO_RENDER_STREAM) {
        dmaChan = dmaRtd->dmaChn[DMA_TX_CHANNEL];
    } else {
        dmaChan = dmaRtd->dmaChn[DMA_RX_CHANNEL];
    }
    if (dmaChan == NULL) {
        AUDIO_DEVICE_LOG_ERR("dmaChan is null");
        return HDF_FAILURE;
    }

    // use start Operation function
    ret = Rk3568DmaSubmit(data, streamType);
    if (ret != HDF_SUCCESS) {
        AUDIO_DEVICE_LOG_ERR("call Rk3568DmaSubmit failed");
        return HDF_FAILURE;
    }
    dma_async_issue_pending(dmaChan);

    AUDIO_DEVICE_LOG_DEBUG("success");
    return HDF_SUCCESS;
}
