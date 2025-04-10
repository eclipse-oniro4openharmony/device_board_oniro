/*
 * Copyright (c) 2022 Huawei Device Co., Ltd.
 *
 * HDF is dual licensed: you can use it either under the terms of
 * the GPL, or the BSD license, at your option.
 * See the LICENSE file in the root of this repository for complete details.
 */

#include <linux/iio/consumer.h>
#include <linux/platform_device.h>
#include "analog_headset.h"
#include "device_resource_if.h"
#include "osal_mem.h"
#include "securec.h"
#include "gpio_if.h"

#define HDF_LOG_TAG analog_headset_core

static struct HeadsetPdata *g_pdataInfo = NULL;
static struct HdfDeviceObject *g_hdfDevice = NULL;
static void InitHeadsetPdata(struct HeadsetPdata *pdata)
{
    if (pdata == NULL) {
        AUDIO_DEVICE_LOG_ERR("pdata is NULL!");
        return;
    }
    if (g_hdfDevice == NULL) {
        AUDIO_DEVICE_LOG_ERR("g_hdfDevice is NULL!");
        return;
    }

    pdata->device = g_hdfDevice;
    pdata->hsGpioFlags = 0;
    pdata->hsGpio = 0;
    pdata->hsInsertType = (pdata->hsGpioFlags & OF_GPIO_ACTIVE_LOW) ? HEADSET_IN_LOW : HEADSET_IN_HIGH;

    /* hook */
    pdata->hookGpio = 0;
    pdata->hookDownType = 0;
    pdata->isHookAdcMode = false;

    /* mic */
#ifdef CONFIG_MODEM_MIC_SWITCH
    pdata->micGpioFlags = 0;
    pdata->hsGpio = 0;
    pdata->hpMicIoValue = GPIO_VAL_LOW;
    pdata->mainMicIoValue = GPIO_VAL_HIGH;
#endif

    pdata->hsWakeup = true;
}

static int32_t GpioDirectionInput(struct device *dev, uint32_t gpio, const char *label)
{
    int32_t ret;

    if ((dev == NULL) || (label == NULL)) {
        AUDIO_DEVICE_LOG_ERR("dev or label is NULL.");
        return -EINVAL;
    }

    ret = GpioSetDir(gpio, GPIO_DIR_IN);
    if (ret < 0) {
        AUDIO_DEVICE_LOG_ERR("[GpioSetDir] failed.");
        return ret;
    }

    return ret;
}

static int32_t TraceInfo(const struct HeadsetPdata *pdata)
{
    if (pdata == NULL) {
        AUDIO_DEVICE_LOG_ERR("pdata is null");
        return -EINVAL;
    }

    AUDIO_DEVICE_LOG_DEBUG("hsGpioFlags = %d, isHookAdcMode = %s",
        pdata->hsGpioFlags, pdata->isHookAdcMode ? "true" : "false");
#ifdef CONFIG_MODEM_MIC_SWITCH
    AUDIO_DEVICE_LOG_DEBUG("micGpioFlags = %d, micSwitchGpio = %u, hpMicIoValue = %u, mainMicIoValue = %u.",
        pdata->micGpioFlags, pdata->micSwitchGpio, pdata->hpMicIoValue, pdata->mainMicIoValue);
#endif

    AUDIO_DEVICE_LOG_DEBUG("hsGpio = %u, hookGpio = %u, hookDownType = %u, hsWakeup = %s.",
        pdata->hsGpio, pdata->hookGpio, pdata->hookDownType, pdata->hsWakeup ? "true" : "false");

    return 0;
}

static int32_t LinuxReadMicConfig(struct device_node *node, struct HeadsetPdata *pdata)
{
#ifdef CONFIG_MODEM_MIC_SWITCH
    /* mic */
    int32_t ret;

    if ((node == NULL) || (pdata == NULL)) {
        AUDIO_DEVICE_LOG_ERR("node or pdata is NULL.");
        return -EINVAL;
    }

    ret = of_get_named_gpio_flags(node, "mic_switch_gpio", 0, &pdata->micGpioFlags);
    if (ret < 0) {
        AUDIO_DEVICE_LOG_DEBUG("Can not read property micSwitchGpio.");
    } else {
        pdata->hsGpio = ret;
        ret = of_property_read_u32(node, "hp_mic_io_value", &pdata->hpMicIoValue);
        if (ret < 0) {
            AUDIO_DEVICE_LOG_DEBUG("have not set hpMicIoValue ,so default set pull down low level.");
            pdata->hpMicIoValue = GPIO_VAL_LOW;
        }
        ret = of_property_read_u32(node, "main_mic_io_value", &pdata->mainMicIoValue);
        if (ret < 0) {
            AUDIO_DEVICE_LOG_DEBUG("have not set mainMicIoValue ,so default set pull down low level.");
            pdata->mainMicIoValue = GPIO_VAL_HIGH;
        }
    }
#endif

    return 0;
}

static int32_t LinuxReadConfig(struct device_node *node, struct HeadsetPdata *pdata)
{
    int32_t ret;
    int32_t wakeup;

    if ((node == NULL) || (pdata == NULL)) {
        AUDIO_DEVICE_LOG_ERR("node or pdata is NULL.");
        return -EINVAL;
    }

    /* headset */
    ret = of_get_named_gpio_flags(node, "headset_gpio", 0, &pdata->hsGpioFlags);
    if (ret < 0) {
        AUDIO_DEVICE_LOG_ERR("Can not read property hsGpio.");
        return ret;
    }
    pdata->hsGpio = ret;
    pdata->hsInsertType = (pdata->hsGpioFlags & OF_GPIO_ACTIVE_LOW) ? HEADSET_IN_LOW : HEADSET_IN_HIGH;

    /* hook */
    ret = of_get_named_gpio_flags(node, "hook_gpio", 0, &pdata->hookGpio);
    if (ret < 0) {
        AUDIO_DEVICE_LOG_WARNING("Can not read property hookGpio.");
        pdata->hookGpio = 0;
        /* adc mode */
        pdata->isHookAdcMode = true;
    } else {
        ret = of_property_read_u32(node, "hook_down_type", &pdata->hookDownType);
        if (ret < 0) {
            AUDIO_DEVICE_LOG_WARNING("have not set hookDownType,set >hook< insert type low level default.");
            pdata->hookDownType = 0;
        }
        pdata->isHookAdcMode = false;
    }
    /* mic */
    (void)LinuxReadMicConfig(node, pdata);

    ret = of_property_read_u32(node, "rockchip,headset_wakeup", &wakeup);
    if (ret < 0) {
        pdata->hsWakeup = true;
    } else {
        pdata->hsWakeup = (wakeup == 0) ? false : true;
    }

    return 0;
}

static int32_t ReadHookModeConfig(struct DeviceResourceIface *parser,
    const struct DeviceResourceNode *node, struct HeadsetPdata *pdata)
{
    int32_t ret;

    if ((pdata == NULL) || (node == NULL) || (parser == NULL)) {
        AUDIO_DEVICE_LOG_ERR("pdata, node or parser is NULL.");
        return HDF_ERR_INVALID_PARAM;
    }

    pdata->isHookAdcMode = true;
    ret = parser->GetUint32(node, "hook_gpio", &pdata->hookGpio, 0);
    if (ret != HDF_SUCCESS) {
        AUDIO_DEVICE_LOG_DEBUG("[GetUint32]-[hook_gpio] is null.");
        pdata->isHookAdcMode = false;
    }

    if (pdata->isHookAdcMode) { /* hook adc mode */
        AUDIO_DEVICE_LOG_DEBUG("headset have hook adc mode.");
        ret = parser->GetUint32(node, "adc_controller_no", &pdata->adcConfig.devNum, 0);
        if (ret != HDF_SUCCESS) {
            AUDIO_DEVICE_LOG_ERR("[GetUint32]-[adc_controller_no] failed.");
            return ret;
        }
        ret = parser->GetUint32(node, "adc_channel", &pdata->adcConfig.chanNo, 0);
        if (ret != HDF_SUCCESS) {
            AUDIO_DEVICE_LOG_ERR("[GetUint32]-[adc_channel] failed.");
            return ret;
        }
    } else { /* hook interrupt mode */
        ret = parser->GetUint32(node, "hook_down_type", &pdata->hookDownType, 0);
        if (ret != HDF_SUCCESS) {
            AUDIO_DEVICE_LOG_ERR("[GetUint32]-[hook_down_type] failed.");
            return ret;
        }
    }
    AUDIO_DEVICE_LOG_DEBUG("hook mode: %s.", pdata->isHookAdcMode ? "sar-adc" : "gpio-int");

    return HDF_SUCCESS;
}

static int32_t ReadMicConfig(struct DeviceResourceIface *parser,
    const struct DeviceResourceNode *node, struct HeadsetPdata *pdata)
{
#ifdef CONFIG_MODEM_MIC_SWITCH
    /* mic */
    int32_t ret;

    if ((pdata == NULL) || (parser == NULL) || (node == NULL)) {
        AUDIO_DEVICE_LOG_ERR("node or pdata is NULL.");
        return HDF_ERR_INVALID_PARAM;
    }

    ret = parser->GetUint32(node, "mic_switch_gpio", &pdata->hsGpio, 0);
    if (ret != HDF_SUCCESS) {
        AUDIO_DEVICE_LOG_ERR("[mic_switch_gpio] failed.");
        return ret;
    }

    ret = parser->GetUint32(node, "hp_mic_io_value", &pdata->hpMicIoValue, 0);
    if (ret != HDF_SUCCESS) {
        AUDIO_DEVICE_LOG_ERR("[hp_mic_io_value] failed.");
        return ret;
    }
    ret = parser->GetUint32(node, "main_mic_io_value", &pdata->mainMicIoValue, 1);
    if (ret != HDF_SUCCESS) {
        AUDIO_DEVICE_LOG_ERR("[main_mic_io_value] failed.");
        return ret;
    }
#endif

    return HDF_SUCCESS;
}

static int32_t ReadConfig(const struct DeviceResourceNode *node, struct HeadsetPdata *pdata)
{
    int32_t ret;
    int32_t temp;
    struct DeviceResourceIface *parser = DeviceResourceGetIfaceInstance(HDF_CONFIG_SOURCE);

    if ((pdata == NULL) || (node == NULL) || (parser == NULL)) {
        AUDIO_DEVICE_LOG_ERR("pdata, node or parser is NULL.");
        return HDF_FAILURE;
    }
    ret = parser->GetString(node, "dev_name", &pdata->devName, NULL);
    if (ret != HDF_SUCCESS) {
        AUDIO_DEVICE_LOG_ERR("[GetString]-[dev_name] failed.");
        return ret;
    }
    /* headset */
    ret = parser->GetUint32(node, "headset_gpio", &pdata->hsGpio, 0);
    if (ret != HDF_SUCCESS) {
        AUDIO_DEVICE_LOG_ERR("[GetUint32]-[headset_gpio] failed.");
        return ret;
    }
    ret = parser->GetUint32(node, "headset_gpio_flag", &pdata->hsGpioFlag, OF_GPIO_ACTIVE_LOW);
    if (ret != HDF_SUCCESS) {
        AUDIO_DEVICE_LOG_ERR("[GetUint32]-[headset_gpio_flag] failed.");
        return ret;
    }
    /* hook */
    ret = ReadHookModeConfig(parser, node, pdata);
    if (ret != HDF_SUCCESS) {
        AUDIO_DEVICE_LOG_ERR("[ReadHookModeConfig] failed.");
        return ret;
    }
    /* mic */
    (void)ReadMicConfig(parser, node, pdata);

    ret = parser->GetUint32(node, "headset_wakeup", &temp, 0);
    if (ret != HDF_SUCCESS) {
        AUDIO_DEVICE_LOG_WARNING("[GetUint32]-[headset_wakeup] failed.");
        temp = 1;
    }
    pdata->hsWakeup = (temp == 0) ? false : true;

    return HDF_SUCCESS;
}

static int32_t AnalogHeadsetInit(struct platform_device *pdev, struct HeadsetPdata *pdata)
{
    int32_t ret;

    if ((pdev == NULL) || (pdata == NULL)) {
        AUDIO_DEVICE_LOG_ERR("pdev or pdata is NULL.");
        return -EINVAL;
    }

    /* headset */
    ret = GpioSetDir(pdata->hsGpio, GPIO_DIR_IN);
    if (ret < 0) {
        AUDIO_DEVICE_LOG_ERR("[GpioSetDir]-[hsGpio] failed.");
        return ret;
    }

    /* hook */
    if (pdata->isHookAdcMode) {
        pdata->chan = iio_channel_get(&pdev->dev, NULL);
        if (IS_ERR(pdata->chan)) {
            pdata->chan = NULL;
            AUDIO_DEVICE_LOG_WARNING("have not set adc chan.");
        }
    } else {
        ret = GpioSetDir(pdata->hookGpio, GPIO_DIR_IN);
        if (ret < 0) {
            AUDIO_DEVICE_LOG_ERR("[GpioSetDir]-[hookGpio] failed.");
            return ret;
        }
    }

    if (pdata->chan != NULL) { /* hook adc mode */
        AUDIO_DEVICE_LOG_DEBUG("headset have hook adc mode.");
        ret = AnalogHeadsetAdcInit(pdev, pdata);
        if (ret < 0) {
            AUDIO_DEVICE_LOG_ERR("[AnalogHeadsetAdcInit] failed.");
            return ret;
        }
    } else { /* hook interrupt mode and not hook */
        AUDIO_DEVICE_LOG_DEBUG("headset have %s mode.", pdata->hookGpio ? "interrupt hook" : "no hook");
        ret = AnalogHeadsetGpioInit(pdev, pdata);
        if (ret < 0) {
            AUDIO_DEVICE_LOG_ERR("[AnalogHeadsetGpioInit] failed.");
            return ret;
        }
    }
    return ret;
}

static int AudioHeadsetProbe(struct platform_device *pdev)
{
    struct device_node *node = pdev->dev.of_node;
    struct HeadsetPdata *pdata;
    int32_t ret;

    AUDIO_DEVICE_LOG_INFO("enter.");
    pdata = (struct HeadsetPdata *)OsalMemCalloc(sizeof(*pdata));
    if (pdata == NULL) {
        AUDIO_DEVICE_LOG_ERR("[OsalMemCalloc] failed!");
        return HDF_ERR_MALLOC_FAIL;
    }
    InitHeadsetPdata(pdata);
    g_pdataInfo = pdata;

    ret = LinuxReadConfig(node, pdata);
    (void)TraceInfo(pdata);
    if (ret < 0) {
        AUDIO_DEVICE_LOG_ERR("[LinuxReadConfig] failed.");
        return ret;
    }

    ret = AnalogHeadsetInit(pdev, pdata);
    if (ret < 0) {
        AUDIO_DEVICE_LOG_ERR("[AnalogHeadsetInit] failed.");
        return ret;
    }
    AUDIO_DEVICE_LOG_INFO("success.");

    return ret;
}

static int AudioHeadsetRemove(struct platform_device *pdev)
{
    (void)pdev;
    return 0;
}

static int AudioHeadsetSuspend(struct platform_device *pdev, pm_message_t state)
{
    if (g_pdataInfo->chan != NULL) {
        return AnalogHeadsetAdcSuspend(pdev, state);
    }
    return 0;
}

static int AudioHeadsetResume(struct platform_device *pdev)
{
    if (g_pdataInfo->chan != NULL) {
        return AnalogHeadsetAdcResume(pdev);
    }
    return 0;
}

static const struct of_device_id g_headsetOfMatch[] = {
    { .compatible = "rockchip_headset", },
    {},
};
MODULE_DEVICE_TABLE(of, g_headsetOfMatch);

static struct platform_driver AudioHeadsetDriver = {
    .probe = AudioHeadsetProbe,
    .remove = AudioHeadsetRemove,
    .resume = AudioHeadsetResume,
    .suspend = AudioHeadsetSuspend,
    .driver = {
        .name = "rockchip_headset",
        .owner = THIS_MODULE,
        .of_match_table = of_match_ptr(g_headsetOfMatch),
    },
};

static int32_t HdfHeadsetBindDriver(struct HdfDeviceObject *device)
{
    if (device == NULL) {
        AUDIO_DEVICE_LOG_ERR("device is NULL.");
        return HDF_ERR_INVALID_PARAM;
    }

    g_hdfDevice = device;
    g_pdataInfo = NULL;

    return HDF_SUCCESS;
}

static int32_t HdfHeadsetInit(struct HdfDeviceObject *device)
{
    int32_t ret;
    static struct IDeviceIoService headsetService = {
        .object.objectId = 1,
    };
    const struct DeviceResourceNode *node = NULL;
    static struct HeadsetPdata pdata;

    AUDIO_DEVICE_LOG_INFO("enter.");
    platform_driver_register(&AudioHeadsetDriver);
    if (device == NULL) {
        AUDIO_DEVICE_LOG_ERR(" is NULL.");
        return HDF_ERR_INVALID_PARAM;
    }

    if (g_pdataInfo == NULL) {
        AUDIO_DEVICE_LOG_ERR("g_pdataInfo is NULL!");
        return HDF_ERR_MALLOC_FAIL;
    }

    node = device->property;
    ret = ReadConfig(node, &pdata);
    if (ret != HDF_SUCCESS) {
        AUDIO_DEVICE_LOG_ERR("[ReadConfig] failed.");
    }
    (void)TraceInfo(&pdata);

    g_pdataInfo->device = device;
    g_pdataInfo->ioService = headsetService;
    device->service = &g_pdataInfo->ioService;
    device->priv = (void *)g_pdataInfo;
    AUDIO_DEVICE_LOG_INFO("success.");

    return HDF_SUCCESS;
}

static void HdfHeadsetExit(struct HdfDeviceObject *device)
{
    struct HeadsetPdata *drvData = NULL;

    AUDIO_DEVICE_LOG_INFO("enter.");
    if (device == NULL) {
        AUDIO_DEVICE_LOG_ERR("device or device->service is NULL.");
        return;
    }

    platform_driver_unregister(&AudioHeadsetDriver);

    if ((device == NULL) || (device->priv == NULL)) {
        AUDIO_DEVICE_LOG_ERR("device or device->priv is NULL.");
        return;
    }
    drvData = (struct HeadsetPdata *)device->priv;
    if (drvData->chan != NULL) { // hook adc mode
        AnalogHeadsetAdcRelease(drvData);
    } else { // hook interrupt mode and not hook
        AnalogHeadsetGpioRelease(drvData);
    }
    OsalMemFree(drvData);
    device->priv = NULL;
    g_pdataInfo = NULL;
    g_hdfDevice = NULL;

    AUDIO_DEVICE_LOG_INFO("done.");
}

/* HdfDriverEntry definitions */
struct HdfDriverEntry g_headsetDevEntry = {
    .moduleVersion = 1,
    .moduleName = "AUDIO_ANALOG_HEADSET",
    .Bind = HdfHeadsetBindDriver,
    .Init = HdfHeadsetInit,
    .Release = HdfHeadsetExit,
};

HDF_INIT(g_headsetDevEntry);