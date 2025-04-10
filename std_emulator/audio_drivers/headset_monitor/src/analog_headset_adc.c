/*
 * Copyright (c) 2022 Huawei Device Co., Ltd.
 *
 * HDF is dual licensed: you can use it either under the terms of
 * the GPL, or the BSD license, at your option.
 * See the LICENSE file in the root of this repository for complete details.
 */

#include <linux/device.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/extcon-provider.h>
#include <linux/iio/consumer.h>
#include <linux/input.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/platform_device.h>
#include "analog_headset.h"
#include "analog_headset_base.h"
#include "hdf_workqueue.h"
#include "osal_time.h"
#include "osal_mem.h"
#include "securec.h"
#include "gpio_if.h"
#include "osal_irq.h"

#define HDF_LOG_TAG analog_headset_adc
#define HOOK_ADC_SAMPLE_TIME 100

#define HOOK_LEVEL_HIGH 410 // 1V*1024/2.5
#define HOOK_LEVEL_LOW 204 // 0.5V*1024/2.5
#define HOOK_DEFAULT_VAL 1024

#define HEADSET_IN 1
#define HEADSET_OUT 0
#define HOOK_DOWN 1
#define HOOK_UP 0

#define HEADSET_TIMER 1
#define HOOK_TIMER 2

#define WAIT 2
#define BUSY 1
#define IDLE 0

/* headset private data */
struct HeadsetPriv {
    struct input_dev *inDev;
    struct HeadsetPdata *pdata;
    uint32_t hsStatus : 1;
    uint32_t hookStatus : 1;
    struct iio_channel *chan;
    /* headset interrupt working will not check hook key  */
    uint32_t hsIrqWorking;
    int32_t curHsStatus;
    HdfWorkQueue workQueue;
    HdfWork hDelayedWork[HS_HOOK_COUNT];
    struct extcon_dev *edev;
    unsigned char *keycodes;
    HdfWork hHookWork;
    /* ms */
    uint32_t hookTime;
    bool isMic;
};

static struct HeadsetPriv *g_hsInfo = NULL;

static int ExtconSetStateSync(struct HeadsetPriv *hs, unsigned int id, bool state)
{
    if (hs == NULL) {
        AUDIO_DEVICE_LOG_ERR("hs is NULL!");
        return -EINVAL;
    }

    extcon_set_state_sync(hs->edev, id, state);
    SetStateSync(id, state);
    AUDIO_DEVICE_LOG_DEBUG("id = %u, state = %s.", id, state ? "in" : "out");

    return 0;
}

static void InputReportKeySync(struct HeadsetPriv *hs, unsigned int code, int value)
{
    if (hs == NULL) {
        AUDIO_DEVICE_LOG_ERR("hs is NULL!");
        return;
    }

    input_report_key(hs->inDev, code, value);
    input_sync(hs->inDev);
    SetStateSync(code, value);
    AUDIO_DEVICE_LOG_DEBUG("code = %u, value = %s.", code, value ? "in" : "out");
}

static void InitHeadsetPriv(struct HeadsetPriv *hs, struct HeadsetPdata *pdata)
{
    if ((hs == NULL) || (pdata == NULL)) {
        AUDIO_DEVICE_LOG_ERR("hs or pdata is NULL!");
        return;
    }

    hs->pdata = pdata;
    hs->hsStatus = HEADSET_OUT;
    hs->hsIrqWorking = IDLE;
    hs->hookStatus = HOOK_UP;
    hs->hookTime = HOOK_ADC_SAMPLE_TIME;
    hs->curHsStatus = BIT_HEADSET_NULL;
    hs->isMic = false;
    AUDIO_DEVICE_LOG_DEBUG("isMic = %s.", hs->isMic ? "true" : "false");
    hs->chan = pdata->chan;
}

static int32_t CheckState(struct HeadsetPriv *hs, bool *beChange)
{
    struct HeadsetPdata *pdata = NULL;
    static uint32_t oldStatus = 0;
    int32_t i;
    int32_t ret;
    int16_t level = 0;

    if ((hs == NULL) || (hs->pdata == NULL) || (beChange == NULL)) {
        AUDIO_DEVICE_LOG_ERR("hs, pdata or beChange is NULL.");
        return HDF_ERR_INVALID_PARAM;
    }

    pdata = hs->pdata;
    OsalMSleep(IRQ_CONFIRM_MS150);
    for (i = 0; i < GET_GPIO_REPEAT_TIMES; i++) {
        ret = GpioRead(pdata->hsGpio, &level);
        if (ret < 0) {
            AUDIO_DEVICE_LOG_ERR("get pin level again, pin=%u, i=%d.", pdata->hsGpio, i);
            OsalMSleep(IRQ_CONFIRM_MS1);
            continue;
        }
        break;
    }
    if ((level < 0) || (ret < 0)) {
        AUDIO_DEVICE_LOG_ERR("get pin level err.");
        return HDF_FAILURE;
    }

    oldStatus = hs->hsStatus;
    switch (pdata->hsInsertType) {
        case HEADSET_IN_HIGH:
            hs->hsStatus = (level > 0) ? HEADSET_IN : HEADSET_OUT;
            break;
        case HEADSET_IN_LOW:
            hs->hsStatus = (level == 0) ? HEADSET_IN : HEADSET_OUT;
            break;
        default:
            AUDIO_DEVICE_LOG_ERR("[hsInsertType] error.");
            break;
    }
    if (oldStatus == hs->hsStatus) {
        *beChange = false;
        return HDF_SUCCESS;
    }

    *beChange = true;
    AUDIO_DEVICE_LOG_DEBUG("(headset in is %s)headset status is %s.",
        pdata->hsInsertType ? "high" : "low", hs->hsStatus ? "in" : "out");

    return HDF_SUCCESS;
}

static int32_t ReportCurrentState(struct HeadsetPriv *hs)
{
    struct HeadsetPdata *pdata = NULL;

    if ((hs == NULL) || (hs->pdata == NULL)) {
        AUDIO_DEVICE_LOG_ERR("hs or pdata is NULL.");
        return HDF_ERR_INVALID_PARAM;
    }

    pdata = hs->pdata;
    if (hs->hsStatus == HEADSET_IN) {
        if (pdata->chan != NULL) {
            /* detect hook key */
            (void)HdfAddDelayedWork(&hs->workQueue, &hs->hDelayedWork[HOOK], DELAY_WORK_MS200);
        } else {
            hs->isMic = false;
            AUDIO_DEVICE_LOG_DEBUG("isMic = %s.", hs->isMic ? "true" : "false");
            hs->curHsStatus = BIT_HEADSET_NO_MIC;
            (void)ExtconSetStateSync(hs, KEY_JACK_HEADPHONE, true);
            AUDIO_DEVICE_LOG_DEBUG("notice headset status = %d(0: NULL, 1: HEADSET, 2: HEADPHONE).", hs->curHsStatus);
        }
    } else {
        hs->curHsStatus = BIT_HEADSET_NULL;
        HdfCancelDelayedWorkSync(&hs->hHookWork);
        if (hs->isMic) {
            if (hs->hookStatus == HOOK_DOWN) {
                hs->hookStatus = HOOK_UP;
                InputReportKeySync(hs, HOOK_KEY_CODE, hs->hookStatus);
            }
            hs->isMic = false;
            AUDIO_DEVICE_LOG_DEBUG("isMic = %s.", hs->isMic ? "true" : "false");
        }

        // Need judge the type, it is not always microphone.
        (void)ExtconSetStateSync(hs, KEY_JACK_HEADSET, false);
        AUDIO_DEVICE_LOG_DEBUG("notice headset status = %d(0: NULL, 1: HEADSET, 2:HEADPHONE).", hs->curHsStatus);
    }

    return HDF_SUCCESS;
}

static int32_t HeadsetInterrupt(uint16_t gpio, void * data)
{
    int32_t ret;
    struct HeadsetPriv *hs = g_hsInfo;
    bool beChange = false;

    (void)data;
    if (hs == NULL) {
        AUDIO_DEVICE_LOG_ERR("hs is NULL.");
        return -EINVAL;
    }

    GpioDisableIrq(hs->pdata->hsGpio);
    if ((hs->hsIrqWorking == BUSY) ||
        (hs->hsIrqWorking == WAIT)) {
        AUDIO_DEVICE_LOG_DEBUG("hsIrqWorking is BUSY or WAIT.");
        return IRQ_HANDLED;
    }

    hs->hsIrqWorking = BUSY;
    ret = CheckState(hs, &beChange);
    if (ret != HDF_SUCCESS) {
        AUDIO_DEVICE_LOG_DEBUG("[CheckState] failed.");
        hs->hsIrqWorking = IDLE;
        GpioEnableIrq(hs->pdata->hsGpio);
        return IRQ_HANDLED;
    }
    if (!beChange) {
        hs->hsIrqWorking = IDLE;
        GpioEnableIrq(hs->pdata->hsGpio);
        return IRQ_HANDLED;
    }

    (void)ReportCurrentState(hs);
    hs->hsIrqWorking = IDLE;
    GpioEnableIrq(hs->pdata->hsGpio);
    return IRQ_HANDLED;
}

#ifdef TEST_FOR_CHANGE_IRQTYPE /* not actived. */
static int32_t HeadsetChangeIrqtype(int type, unsigned int irqType)
{
    int32_t ret;

    free_irq(g_hsInfo->irq[type], NULL);

    AUDIO_DEVICE_LOG_DEBUG("type is %s irqtype is %s.", type ? "hook" : "headset",
        (irqType == IRQF_TRIGGER_RISING) ? "RISING" : "FALLING");
    AUDIO_DEVICE_LOG_DEBUG("type is %s irqtype is %s.",
        type ? "hook" : "headset", (irqType == IRQF_TRIGGER_LOW) ? "LOW" : "HIGH");
    switch (type) {
        case HEADSET:
            ret = request_threaded_irq(g_hsInfo->irq[type], NULL, HeadsetInterrupt, irqType, "headset_input", NULL);
            if (ret < 0) {
                AUDIO_DEVICE_LOG_DEBUG("HeadsetChangeIrqtype: request irq failed.");
            }
            break;
        default:
            ret = -EINVAL;
            break;
    }
    return ret;
}
#endif

static void HookOnceWork(void *arg)
{
    int32_t ret;
    int32_t val;
    uint32_t type;
    struct HeadsetPriv *hs = (struct HeadsetPriv *)arg;

    if (hs == NULL) {
        AUDIO_DEVICE_LOG_ERR("hs is NULL.");
        return;
    }
    ret = iio_read_channel_raw(hs->chan, &val);
    if (ret < 0) {
        AUDIO_DEVICE_LOG_ERR("read HookOnceWork adc channel() error: %d.", ret);
    } else {
        AUDIO_DEVICE_LOG_DEBUG("HookOnceWork read adc value: %d.", val);
    }

    if (val >= 0 && val < HOOK_LEVEL_LOW) {
        hs->isMic = false;
    } else if (val >= HOOK_LEVEL_HIGH) {
        hs->isMic = true;
        (void)HdfAddDelayedWork(&hs->workQueue, &hs->hHookWork, DELAY_WORK_MS100);
    } else {
        ; // do nothing.
    }
    AUDIO_DEVICE_LOG_DEBUG("isMic = %s.", g_hsInfo->isMic ? "true" : "false");
    hs->curHsStatus = hs->isMic ? BIT_HEADSET : BIT_HEADSET_NO_MIC;

    if (hs->curHsStatus != BIT_HEADSET_NULL) {
        type = (hs->isMic) ? KEY_JACK_HEADSET : KEY_JACK_HEADPHONE;
        (void)ExtconSetStateSync(hs, type, true);
    }
    AUDIO_DEVICE_LOG_DEBUG("notice headset status = %d(0: NULL, 1: HEADSET, 2:HEADPHONE).", hs->curHsStatus);
}

static int32_t CheckInsertType(struct HeadsetPdata *pdata)
{
    int32_t i;
    int32_t ret;
    int16_t level = 0;

    for (i = 0; i < GET_GPIO_REPEAT_TIMES; i++) {
        ret = GpioRead(pdata->hsGpio, &level);
        if (ret < 0) {
            AUDIO_DEVICE_LOG_ERR("get pin level again, pin=%u, i=%d.", pdata->hsGpio, i);
            OsalMSleep(IRQ_CONFIRM_MS1);
            continue;
        }
        break;
    }

    if ((level < 0) || (ret < 0)) {
        AUDIO_DEVICE_LOG_ERR("get pin level err.");
        return HDF_FAILURE;
    }

    switch (pdata->hsInsertType) {
        case HEADSET_IN_HIGH:
            ret = (level > 0) ? HEADSET_IN : HEADSET_OUT;
            break;
        case HEADSET_IN_LOW:
            ret = (level == 0) ? HEADSET_IN : HEADSET_OUT;
            break;
        default:
            ret = HDF_FAILURE;
            AUDIO_DEVICE_LOG_ERR("[hsInsertType] error.");
            break;
    }

    return ret;
}

static void HookWorkCallback(void * arg)
{
    int32_t ret;
    int32_t val;
    struct HeadsetPriv *hs = (struct HeadsetPriv *)arg;
    static uint32_t oldStatus = HOOK_UP;
    static int32_t oldVal = -1; // Invalid initial value

    if ((hs == NULL) || (hs->pdata == NULL)) {
        AUDIO_DEVICE_LOG_ERR("hs or hs->pdata is NULL.");
        return;
    }

    ret = iio_read_channel_raw(hs->chan, &val);
    if (ret < 0) {
        AUDIO_DEVICE_LOG_ERR("read hook adc channel() error: %d.", ret);
        return;
    }

    ret = CheckInsertType(hs->pdata);
    if ((hs->hsStatus == HEADSET_OUT) || (hs->hsIrqWorking == BUSY) || (hs->hsIrqWorking == WAIT) ||
        (ret != HEADSET_IN)) {
        AUDIO_DEVICE_LOG_DEBUG("Headset is out or waiting for headset is in or out, after same time check HOOK key.");
        return;
    }
    oldStatus = hs->hookStatus;
    if (val < HOOK_LEVEL_LOW && val >= 0) {
        hs->hookStatus = HOOK_DOWN;
    } else if (val > HOOK_LEVEL_HIGH && val < HOOK_DEFAULT_VAL) {
        hs->hookStatus = HOOK_UP;
    } else {
        ; // do nothing.
    }
    if (oldVal != val) {
        AUDIO_DEVICE_LOG_DEBUG("HOOK status is %s , adc value = %d, hookTime = %u.",
            hs->hookStatus ? "down" : "up", val, hs->hookTime);
        oldVal = val;
    }
    if (oldStatus == hs->hookStatus) {
        (void)HdfAddDelayedWork(&hs->workQueue, &hs->hHookWork, DELAY_WORK_MS100);
        return;
    }

    ret = CheckInsertType(hs->pdata);
    if ((hs->hsStatus == HEADSET_OUT) || (hs->hsIrqWorking == BUSY) || (hs->hsIrqWorking == WAIT) ||
        (ret != HEADSET_IN)) {
        AUDIO_DEVICE_LOG_DEBUG("headset is out, HOOK status must discard.");
        return;
    }
    InputReportKeySync(hs, HOOK_KEY_CODE, hs->hookStatus);
    (void)HdfAddDelayedWork(&hs->workQueue, &hs->hHookWork, DELAY_WORK_MS100);
}

static int AnalogHskeyOpen(struct input_dev *dev)
{
    (void)dev;
    return 0;
}

static void AnalogHskeyClose(struct input_dev *dev)
{
    (void)dev;
}

static const unsigned int g_hsCable[] = {
    KEY_JACK_HEADSET,
    KEY_JACK_HEADPHONE,
    EXTCON_NONE,
};

static int32_t InitWorkData(struct HeadsetPriv *hs)
{
    struct HeadsetPdata *pdata = NULL;
    if ((hs == NULL) || (hs->pdata == NULL)) {
        AUDIO_DEVICE_LOG_ERR("hs or pdata is NULL.");
        return HDF_ERR_INVALID_PARAM;
    }
    pdata = hs->pdata;
    if (HdfWorkQueueInit(&hs->workQueue, HDF_HEADSET_WORK_QUEUE_NAME) != HDF_SUCCESS) {
        AUDIO_DEVICE_LOG_ERR("Init work queue failed");
        return HDF_FAILURE;
    }
    HdfDelayedWorkInit(&hs->hDelayedWork[HOOK], HookOnceWork, hs);
    if (pdata->chan != NULL) { // this is always true.
        HdfDelayedWorkInit(&hs->hHookWork, HookWorkCallback, hs);
    }

    return HDF_SUCCESS;
}

static int32_t CreateAndRegisterInputDevice(struct platform_device *pdev, struct HeadsetPriv *hs)
{
    int32_t ret;

    AUDIO_DEVICE_LOG_INFO("enter.");
    if ((hs == NULL) || (pdev == NULL)) {
        AUDIO_DEVICE_LOG_ERR("hs or pdev is NULL.");
        return -EINVAL;
    }

    hs->inDev = devm_input_allocate_device(&pdev->dev);
    if (hs->inDev == NULL) {
        AUDIO_DEVICE_LOG_ERR("failed to allocate input device.");
        ret = -ENOMEM;
        return ret;
    }

    hs->inDev->name = pdev->name;
    hs->inDev->open = AnalogHskeyOpen;
    hs->inDev->close = AnalogHskeyClose;
    hs->inDev->dev.parent = &pdev->dev;

    hs->inDev->id.vendor = INPUT_DEVID_VENDOR;
    hs->inDev->id.product = INPUT_DEVID_PRODUCT;
    hs->inDev->id.version = INPUT_DEVID_VERSION;
    // register the input device
    ret = input_register_device(hs->inDev);
    if (ret) {
        AUDIO_DEVICE_LOG_ERR("failed to register input device.");
        return ret;
    }
    input_set_capability(hs->inDev, EV_KEY, HOOK_KEY_CODE);
    AUDIO_DEVICE_LOG_INFO("%s: done.");

    return ret;
}

static int32_t SetHeadsetIrqEnable(struct device *dev, struct HeadsetPriv *hs)
{
    int32_t ret;
    struct HeadsetPdata *pdata = NULL;
    uint32_t irqType;
    uint32_t irq;

    if ((hs == NULL) || (hs->pdata == NULL) || (dev == NULL)) {
        AUDIO_DEVICE_LOG_ERR("hs, pdata or dev is NULL.");
        return HDF_ERR_INVALID_PARAM;
    }

    pdata = hs->pdata;
    if (pdata->hsGpio) {
        irqType = GPIO_IRQ_TRIGGER_RISING | GPIO_IRQ_TRIGGER_FALLING | GPIO_IRQ_USING_THREAD;
        ret = GpioSetIrq(pdata->hsGpio, irqType, HeadsetInterrupt, NULL);
        if (ret != HDF_SUCCESS) {
            AUDIO_DEVICE_LOG_ERR("failed headset adc probe ret=%d.", ret);
            return ret;
        }

        ret = GpioEnableIrq(pdata->hsGpio);
        if (ret != HDF_SUCCESS) {
            AUDIO_DEVICE_LOG_ERR("enable irq fail! ret:%d\n", ret);
            (void)GpioUnsetIrq(pdata->hsGpio, NULL);
            return ret;
        }

        if (pdata->hsWakeup) {
            irq = gpio_to_irq(pdata->hsGpio);
            enable_irq_wake(irq);
        }
    } else {
        AUDIO_DEVICE_LOG_ERR("failed init headset,please full hook_io_init function in board.");
        ret = -EEXIST;
        return ret;
    }

    return 0;
}

int32_t AnalogHeadsetAdcInit(struct platform_device *pdev, struct HeadsetPdata *pdata)
{
    int32_t ret;
    struct HeadsetPriv *hs;

    AUDIO_DEVICE_LOG_INFO("%s: enter.");
    hs = (struct HeadsetPriv *)OsalMemCalloc(sizeof(*hs));
    if (hs == NULL) {
        AUDIO_DEVICE_LOG_ERR("failed to allocate driver data.");
        return HDF_ERR_MALLOC_FAIL;
    }
    g_hsInfo = hs;
    InitHeadsetPriv(hs, pdata);
    hs->edev = devm_extcon_dev_allocate(&pdev->dev, g_hsCable);
    if (IS_ERR(hs->edev)) {
        AUDIO_DEVICE_LOG_ERR("failed to allocate extcon device.");
        return-ENOMEM;
    }
    ret = devm_extcon_dev_register(&pdev->dev, hs->edev);
    if (ret < 0) {
        AUDIO_DEVICE_LOG_ERR("extcon_dev_register() failed: %d.", ret);
        return ret;
    }
    ret = InitWorkData(hs);
    if (ret != HDF_SUCCESS) {
        AUDIO_DEVICE_LOG_ERR("[InitWorkData] failed");
        return ret;
    }
    // Create and register the input driver
    ret = CreateAndRegisterInputDevice(pdev, hs);
    if (ret != 0) {
        AUDIO_DEVICE_LOG_ERR("[CreateAndRegisterInputDevice] failed");
        return ret;
    }
    ret = CreateAndRegisterHdfInputDevice((void *)hs, pdata->device);
    if (ret != 0) {
        AUDIO_DEVICE_LOG_DEBUG("[CreateAndRegisterHdfInputDevice] failed");
    }
    ret = SetHeadsetIrqEnable(&pdev->dev, hs);
    if (ret != 0) {
        AUDIO_DEVICE_LOG_ERR("[SetHeadsetIrqEnable] failed");
        return ret;
    }
    AUDIO_DEVICE_LOG_INFO("%s: success.");
    return ret;
}

int AnalogHeadsetAdcSuspend(struct platform_device *pdev, pm_message_t state)
{
    AUDIO_DEVICE_LOG_DEBUG("%d enter.");
    (void)pdev;
    (void)state;

    return 0;
}

int AnalogHeadsetAdcResume(struct platform_device *pdev)
{
    AUDIO_DEVICE_LOG_DEBUG("%d enter.");
    (void)pdev;

    return 0;
}

void AnalogHeadsetAdcRelease(struct HeadsetPdata *pdata)
{
    struct HeadsetPriv *hs = g_hsInfo;
    if (hs == NULL) {
        AUDIO_DEVICE_LOG_ERR("hs is NULL.");
        return;
    }
    (void)pdata;
    HdfWorkDestroy(&hs->hDelayedWork[HOOK]);
    HdfCancelDelayedWorkSync(&hs->hHookWork);
    HdfWorkDestroy(&hs->hHookWork);
    HdfWorkQueueDestroy(&hs->workQueue);
    DestroyHdfInputDevice();
    OsalMemFree(hs);
    g_hsInfo = NULL;
}