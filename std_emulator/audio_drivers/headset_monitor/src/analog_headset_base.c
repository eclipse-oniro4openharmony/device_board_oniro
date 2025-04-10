/*
 * Copyright (c) 2022 Huawei Device Co., Ltd.
 *
 * HDF is dual licensed: you can use it either under the terms of
 * the GPL, or the BSD license, at your option.
 * See the LICENSE file in the root of this repository for complete details.
 */

#include "analog_headset_base.h"
#include "analog_headset_ev.h"
#include "audio_device_log.h"
#include "osal_mem.h"
#include "securec.h"
#include "event_hub.h"

#define EV_TYPE_KEY_INDEX       0
#define KEY_CODE_JACK_INDEX     3
#define IRQ_CONFIRM_MS1         1
#define USING_LINUX_INPUT_DEVICE

InputDevice *g_hdfInDev = NULL;
void InputSetCapability(InputDevice *hdfInDev)
{
    if (hdfInDev == NULL) {
        AUDIO_DEVICE_LOG_ERR("hdfInDev is NULL.");
        return;
    }

    hdfInDev->abilitySet.eventType[EV_TYPE_KEY_INDEX] = SET_BIT(EV_KEY);
    hdfInDev->abilitySet.keyCode[KEY_CODE_JACK_INDEX] = SET_BIT(KEY_JACK_HOOK);
}

void SetStateSync(unsigned int id, bool state)
{
    InputDevice *hdfInDev = g_hdfInDev;
    if (hdfInDev == NULL) {
        AUDIO_DEVICE_LOG_ERR("hdfInDev is NULL.");
        return;
    }

    ReportKey(hdfInDev, id, state);
    ReportSync(hdfInDev);
}

static InputDevice *HdfInputDeviceInstance(void *hs, struct HdfDeviceObject *device)
{
#ifdef USING_LINUX_INPUT_DEVICE
    (void)hs;
    (void)device;
    return NULL;
#else
    int32_t ret;
    InputDevIdentify inputDevId;
    InputDevice *hdfInDev = NULL;
    static char *tempStr = "analog_headset_input_device";

    AUDIO_DEVICE_LOG_INFO("enter.");
    if ((hs == NULL) || (device == NULL)) {
        AUDIO_DEVICE_LOG_ERR("hs or device is NULL.");
        return NULL;
    }

    hdfInDev = (InputDevice *)OsalMemCalloc(sizeof(InputDevice));
    if (hdfInDev == NULL) {
        AUDIO_DEVICE_LOG_ERR("instance input device failed");
        return NULL;
    }

    hdfInDev->hdfDevObj = device;
    hdfInDev->pvtData = hs;
    hdfInDev->devType = INDEV_TYPE_KEY;
    hdfInDev->devName = tempStr;

    inputDevId.vendor = INPUT_DEVID_VENDOR;
    inputDevId.product = INPUT_DEVID_PRODUCT;
    inputDevId.version = INPUT_DEVID_VERSION;
    hdfInDev->attrSet.id = inputDevId;

    ret = strncpy_s(hdfInDev->attrSet.devName, DEV_NAME_LEN, tempStr, DEV_NAME_LEN);
    if (ret != 0) {
        OsalMemFree(hdfInDev);
        hdfInDev = NULL;
        AUDIO_DEVICE_LOG_ERR("strncpy devName failed");
        return NULL;
    }

    return hdfInDev;
#endif
}

int32_t CreateAndRegisterHdfInputDevice(void *hs, struct HdfDeviceObject *device)
{
    int32_t ret;
    InputDevice *hdfInDev = NULL;

    AUDIO_DEVICE_LOG_INFO("enter.");
    if (hs == NULL) {
        AUDIO_DEVICE_LOG_ERR("hs is NULL.");
        return HDF_ERR_INVALID_PARAM;
    }

    hdfInDev = HdfInputDeviceInstance(hs, device);
    if (hdfInDev == NULL) {
        AUDIO_DEVICE_LOG_ERR("[HdfInputDeviceInstance] failed.");
        return HDF_FAILURE;
    }
    ret = RegisterInputDevice(hdfInDev);
    if (ret != HDF_SUCCESS) {
        OsalMemFree(hdfInDev);
        hdfInDev = NULL;
        AUDIO_DEVICE_LOG_ERR("[RegisterInputDevice] failed.");
        /* Theoretically, the return fails. In fact, two reporting systems are used.
           The registration of the input device is unsuccessful, and another system is still available. */
        return HDF_SUCCESS;
    }

    InputSetCapability(hdfInDev);
    g_hdfInDev = hdfInDev;
    AUDIO_DEVICE_LOG_INFO("done.");

    return HDF_SUCCESS;
}

void DestroyHdfInputDevice(void)
{
    if (g_hdfInDev != NULL) {
        UnregisterInputDevice(g_hdfInDev);
        g_hdfInDev = NULL;
    }
}

int32_t GpioGetValue(uint16_t gpio)
{
    int32_t ret;
    uint16_t level = 0;
    ret = GpioRead(gpio, &level);
    return (ret == HDF_SUCCESS) ? level : ret;
}

int32_t SetIrqType(uint16_t gpio, uint16_t irqType, GpioIrqFunc func, void *arg)
{
    int32_t ret;

    ret = GpioUnsetIrq(gpio, arg);
    if (ret != HDF_SUCCESS) {
        AUDIO_DEVICE_LOG_ERR("[GpioUnsetIrq] failed.");
        return ret;
    }
    OsalMSleep(IRQ_CONFIRM_MS1);
    ret = GpioSetIrq(gpio, irqType, func, arg);
    if (ret != HDF_SUCCESS) {
        AUDIO_DEVICE_LOG_ERR("[GpioSetIrq] failed.");
    }

    return ret;
}