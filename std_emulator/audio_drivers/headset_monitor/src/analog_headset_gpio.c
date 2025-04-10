/*
 * Copyright (c) 2022 Huawei Device Co., Ltd.
 *
 * HDF is dual licensed: you can use it either under the terms of
 * the GPL, or the BSD license, at your option.
 * See the LICENSE file in the root of this repository for complete details.
 */

#include <linux/debugfs.h>
#include <linux/device.h>
#ifdef CONFIG_HAS_EARLYSUSPEND
#include <linux/earlysuspend.h>
#endif
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/extcon-provider.h>
#include <linux/input.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/platform_device.h>
#include "analog_headset.h"
#include "analog_headset_base.h"
#include "audio_device_log.h"
#include "hdf_workqueue.h"
#include "osal_time.h"
#include "osal_timer.h"
#include "osal_mem.h"
#include "osal_mutex.h"
#include "gpio_if.h"

#define HDF_LOG_TAG analog_headset_gpio
#define HEADSET_IN              1
#define HEADSET_OUT             0
#define HOOK_DOWN               1
#define HOOK_UP                 0
#define ENABLE_FLAG             1
#define DISABLE_FLAG            0

#define HEADSET_TIMER_INTERVAL  1 /* unit in ms, about 100 jiffies. */

/* headset private data */
struct HeadsetPriv {
    struct input_dev *inDev;
    struct HeadsetPdata *pdata;
    uint32_t hsStatus : 1;
    uint32_t hookStatus : 1;
    uint32_t ishookIrq : 1;
    int32_t curHsStatus;
    HdfWorkQueue workQueue;
    HdfWork hDelayedWork[HS_HOOK_COUNT];
    struct extcon_dev *edev;
    struct OsalMutex mutexLk[HS_HOOK_COUNT];
    OsalTimer hsTimer;
    unsigned char *keycodes;
    bool isMic;
};

static struct HeadsetPriv *g_hsInfo = NULL;

#ifdef CONFIG_MODEM_MIC_SWITCH
#define HP_MIC 0
#define MAIN_MIC 1

void ModemMicSwitch(int value)
{
    struct HeadsetPriv *hs = g_hsInfo;
    struct HeadsetPdata *pdata = NULL;

    if ((hs == NULL) || (hs->pdata == NULL)) {
        AUDIO_DEVICE_LOG_ERR("hs or hs->pdata is NULL.");
        return;
    }
    pdata = hs->pdata;
    if (value == HP_MIC) {
        GpioWrite(pdata->micSwitchGpio, pdata->hpMicIoValue);
    } else if (value == MAIN_MIC) {
        GpioWrite(pdata->micSwitchGpio, pdata->mainMicIoValue);
    } else {
        ; // do nothing.
    }
}

void ModemMicRelease(void)
{
    struct HeadsetPriv *hs = g_hsInfo;
    struct HeadsetPdata *pdata = NULL;

    if ((hs == NULL) || (hs->pdata == NULL)) {
        AUDIO_DEVICE_LOG_ERR("hs or hs->pdata is NULL.");
        return;
    }
    pdata = hs->pdata;
    if (hs->curHsStatus == BIT_HEADSET) {
        GpioWrite(pdata->micSwitchGpio, pdata->hpMicIoValue);
    } else {
        GpioWrite(pdata->micSwitchGpio, pdata->mainMicIoValue);
    }
}
#endif

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
    hs->hookStatus = HOOK_UP;
    hs->ishookIrq = DISABLE_FLAG;
    hs->curHsStatus = BIT_HEADSET_NULL;
    hs->isMic = false;
    AUDIO_DEVICE_LOG_DEBUG("LINE = %d: isMic = %s.", hs->isMic ? "true" : "false");
}

static int ReadGpio(uint16_t gpio)
{
    int32_t i;
    uint16_t level;
    int32_t ret;

    for (i = 0; i < GET_GPIO_REPEAT_TIMES; i++) {
        ret = GpioRead(gpio, &level);
        if (ret < 0) {
            AUDIO_DEVICE_LOG_DEBUG("get pin level again, pin = %d, i = %d.", gpio, i);
            OsalMSleep(IRQ_CONFIRM_MS1);
            continue;
        }
        break;
    }
    if (level < 0) {
        AUDIO_DEVICE_LOG_ERR("get pin level err.");
    }
    return level;
}

static int32_t HeadsetInterrupt(uint16_t gpio, void * data)
{
    struct HeadsetPriv *hs = g_hsInfo;

    if (hs == NULL) {
        AUDIO_DEVICE_LOG_ERR("hs is NULL.");
        return -EINVAL;
    }

    (void)gpio;
    (void)data;
    (void)HdfAddDelayedWork(&hs->workQueue, &hs->hDelayedWork[HEADSET], DELAY_WORK_MS50);
    return IRQ_HANDLED;
}

static int32_t HookInterrupt(uint16_t gpio, void * data)
{
    struct HeadsetPriv *hs = g_hsInfo;

    if (hs == NULL) {
        AUDIO_DEVICE_LOG_ERR("hs is NULL.");
        return -EINVAL;
    }

    (void)gpio;
    (void)data;
    (void)HdfAddDelayedWork(&hs->workQueue, &hs->hDelayedWork[HOOK], DELAY_WORK_MS100);
    return IRQ_HANDLED;
}

static int32_t CheckState(struct HeadsetPriv *hs, bool *beChange)
{
    int32_t level = 0;
    int32_t level2 = 0;
    struct HeadsetPdata *pdata = NULL;
    static uint32_t oldStatus = 0;

    if ((hs == NULL) || (hs->pdata == NULL) || (beChange == NULL)) {
        AUDIO_DEVICE_LOG_ERR("hs, pdata or beChange is NULL.");
        return HDF_ERR_INVALID_PARAM;
    }
    pdata = hs->pdata;

    level = ReadGpio(pdata->hsGpio);
    if (level < 0) {
        return HDF_FAILURE;
    }
    OsalMSleep(IRQ_CONFIRM_MS100);
    level2 = ReadGpio(pdata->hsGpio);
    if (level2 < 0) {
        return HDF_FAILURE;
    }
    if (level2 != level) {
        return HDF_FAILURE;
    }
    oldStatus = hs->hsStatus;
    if (pdata->hsInsertType == HEADSET_IN_HIGH) {
        hs->hsStatus = level ? HEADSET_IN : HEADSET_OUT;
    } else {
        hs->hsStatus = level ? HEADSET_OUT : HEADSET_IN;
    }

    if (oldStatus == hs->hsStatus) {
        AUDIO_DEVICE_LOG_WARNING("oldStatus == hs->hsStatus.");
        *beChange = false;
        return HDF_SUCCESS;
    }
    *beChange = true;
    AUDIO_DEVICE_LOG_DEBUG("(headset in is %s)headset status is %s.",
        pdata->hsInsertType ? "high level" : "low level",
        hs->hsStatus ? "in" : "out");

    return HDF_SUCCESS;
}

static int32_t ReportCurrentState(struct HeadsetPriv *hs)
{
    struct HeadsetPdata *pdata = NULL;
    uint32_t type;
    bool bePlugIn;

    if ((hs == NULL) || (hs->pdata == NULL)) {
        AUDIO_DEVICE_LOG_ERR("hs or pdata is NULL.");
        return HDF_ERR_INVALID_PARAM;
    }
    pdata = hs->pdata;

    if (hs->hsStatus == HEADSET_IN) {
        hs->curHsStatus = BIT_HEADSET_NO_MIC;
        type = (pdata->hsInsertType == HEADSET_IN_HIGH) ? IRQF_TRIGGER_FALLING : IRQF_TRIGGER_RISING;
        if (pdata->hookGpio) {
            /* Start the timer, wait for press the hook-key, use OsalTimerStartOnce replace
               'del_timer(&t), t.expires = jiffies + TIMER_EXPIRES_JIFFIES, add_timer(&t)' */
            OsalTimerStartOnce(&hs->hsTimer);
            return HDF_SUCCESS;
        }
    } else {
        hs->hookStatus = HOOK_UP;
        if (hs->ishookIrq == ENABLE_FLAG) {
            AUDIO_DEVICE_LOG_DEBUG("disable hsHook irq.");
            hs->ishookIrq = DISABLE_FLAG;
            GpioDisableIrq(hs->pdata->hookGpio);
        }
        hs->curHsStatus = BIT_HEADSET_NULL;
    }
    bePlugIn = (hs->curHsStatus != BIT_HEADSET_NULL) ? true : false;
    (void)ExtconSetStateSync(hs, KEY_JACK_HEADPHONE, bePlugIn);
    AUDIO_DEVICE_LOG_DEBUG("curHsStatus = %d(0: NULL, 1: HEADSET, 2:HEADPHONE).", hs->curHsStatus);

    return HDF_SUCCESS;
}

static void HeadsetObserveWork(void *arg)
{
    int32_t ret;
    struct HeadsetPriv *hs = (struct HeadsetPriv *)arg;
    bool beChange = false;

    if (hs == NULL) {
        AUDIO_DEVICE_LOG_ERR("hs is NULL.");
        return;
    }

    (void)OsalMutexLock(&hs->mutexLk[HEADSET]);
    ret = CheckState(hs, &beChange);
    if (ret != HDF_SUCCESS) {
        AUDIO_DEVICE_LOG_ERR("[CheckState] failed.");
        (void)OsalMutexUnlock(&hs->mutexLk[HEADSET]);
        return;
    }
    if (!beChange) {
        AUDIO_DEVICE_LOG_ERR("read headset io level old status == now status = %u.", hs->hsStatus);
        (void)OsalMutexUnlock(&hs->mutexLk[HEADSET]);
        return;
    }

    ret = ReportCurrentState(hs);
    if (ret != HDF_SUCCESS) {
        AUDIO_DEVICE_LOG_DEBUG("[ReportCurentState] failed.");
    }
    (void)OsalMutexUnlock(&hs->mutexLk[HEADSET]);
}

static void HookWorkCallback(void *arg)
{
    int32_t level;
    struct HeadsetPriv *hs = (struct HeadsetPriv *)arg;
    struct HeadsetPdata *pdata = NULL;
    static uint32_t oldStatus = HOOK_UP;

    if ((hs == NULL) || (hs->pdata == NULL)) {
        AUDIO_DEVICE_LOG_ERR("hs or hs->pdata is NULL.");
        return;
    }
    pdata = hs->pdata;
    (void)OsalMutexLock(&hs->mutexLk[HOOK]);
    if (hs->hsStatus == HEADSET_OUT) {
        AUDIO_DEVICE_LOG_DEBUG("Headset is out.");
        (void)OsalMutexUnlock(&hs->mutexLk[HOOK]);
        return;
    }
    level = ReadGpio(pdata->hookGpio);
    if (level < 0) {
        AUDIO_DEVICE_LOG_ERR("[ReadGpio] failed.");
        (void)OsalMutexUnlock(&hs->mutexLk[HOOK]);
        return;
    }
    oldStatus = hs->hookStatus;
    AUDIO_DEVICE_LOG_DEBUG("Hook_work -- level = %d.", level);
    if (level == 0) {
        hs->hookStatus = (pdata->hookDownType == HOOK_DOWN_HIGH) ? HOOK_UP : HOOK_DOWN;
    } else if (level > 0) {
        hs->hookStatus = (pdata->hookDownType == HOOK_DOWN_HIGH) ? HOOK_DOWN : HOOK_UP;
    } else {
        ; // do nothing.
    }
    if (oldStatus == hs->hookStatus) {
        AUDIO_DEVICE_LOG_DEBUG("oldStatus == hs->hookStatus.");
        (void)OsalMutexUnlock(&hs->mutexLk[HOOK]);
        return;
    }
    AUDIO_DEVICE_LOG_DEBUG("Hook_work -- level = %d  hook status is %s.", level,
        hs->hookStatus ? "key down" : "key up");

    InputReportKeySync(hs, HOOK_KEY_CODE, hs->hookStatus);
    (void)OsalMutexUnlock(&hs->mutexLk[HOOK]);
}

static void HeadsetTimerCallback(uintptr_t arg)
{
    struct HeadsetPriv *hs = (struct HeadsetPriv *)arg;
    struct HeadsetPdata *pdata = NULL;
    int32_t level;
    bool bePlugIn;

    if (hs == NULL) {
        AUDIO_DEVICE_LOG_ERR("hs is NULL.");
        return;
    }

    pdata = hs->pdata;
    if (hs->hsStatus == HEADSET_OUT) {
        AUDIO_DEVICE_LOG_DEBUG("Headset is out.");
        return;
    }
    level = ReadGpio(pdata->hookGpio);
    if (level < 0) {
        AUDIO_DEVICE_LOG_ERR("[ReadGpio] failed.");
        return;
    }
    if ((level > 0 && pdata->hookDownType == HOOK_DOWN_LOW) ||
        (level == 0 && pdata->hookDownType == HOOK_DOWN_HIGH)) {
        hs->isMic = true;
        GpioEnableIrq(hs->pdata->hookGpio);
        hs->ishookIrq = ENABLE_FLAG;
        hs->hookStatus = HOOK_UP;
    } else {
        hs->isMic = false;
    }
    AUDIO_DEVICE_LOG_DEBUG("isMic = %s.", hs->isMic ? "true" : "false");
    hs->curHsStatus = hs->isMic ? BIT_HEADSET : BIT_HEADSET_NO_MIC;
    bePlugIn = hs->isMic;
    (void)ExtconSetStateSync(hs, KEY_JACK_HEADSET, bePlugIn);
    AUDIO_DEVICE_LOG_DEBUG("hs->curHsStatus = %d(0: NULL, 1: HEADSET, 2:HEADPHONE).", hs->curHsStatus);
}

#ifdef CONFIG_HAS_EARLYSUSPEND
static void HeadsetEarlyResume(struct early_suspend *h)
{
    (void)HdfAddDelayedWork(&g_hsInfo->workQueue, &g_hsInfo->hDelayedWork[HEADSET], DELAY_WORK_MS10);
    AUDIO_DEVICE_LOG_DEBUG("done.");
}

static struct early_suspend g_hsEarlySuspend;
#endif

static int AnalogHskeyOpen(struct input_dev *dev)
{
    AUDIO_DEVICE_LOG_DEBUG("enter.");
    (void)dev;
    return 0;
}

static void AnalogHskeyClose(struct input_dev *dev)
{
    AUDIO_DEVICE_LOG_DEBUG("enter.");
    (void)dev;
}

static const unsigned int g_hsCable[] = {
    KEY_JACK_HEADSET,
    KEY_JACK_HEADPHONE,
    EXTCON_NONE,
};

static int32_t CreateAndRegisterEdev(struct device *dev, struct HeadsetPriv *hs)
{
    int32_t ret;

    AUDIO_DEVICE_LOG_INFO("enter.");
    if ((hs == NULL) || (dev == NULL)) {
        AUDIO_DEVICE_LOG_ERR("hs or dev is NULL.");
        return -EINVAL;
    }

    hs->edev = devm_extcon_dev_allocate(dev, g_hsCable);
    if (IS_ERR(hs->edev)) {
        AUDIO_DEVICE_LOG_ERR("failed to allocate extcon device.");
        ret = -ENOMEM;
        return ret;
    }
    ret = devm_extcon_dev_register(dev, hs->edev);
    if (ret < 0) {
        AUDIO_DEVICE_LOG_ERR("extcon_dev_register() failed: %d.", ret);
        return ret;
    }

    return ret;
}

static int32_t InitWorkData(struct HeadsetPriv *hs)
{
    if (hs == NULL) {
        AUDIO_DEVICE_LOG_ERR("hs is NULL.");
        return HDF_ERR_INVALID_PARAM;
    }

    if (HdfWorkQueueInit(&hs->workQueue, HDF_HEADSET_WORK_QUEUE_NAME) != HDF_SUCCESS) {
        AUDIO_DEVICE_LOG_ERR("Init work queue failed");
        return HDF_FAILURE;
    }
    HdfDelayedWorkInit(&hs->hDelayedWork[HEADSET], HeadsetObserveWork, hs);
    HdfDelayedWorkInit(&hs->hDelayedWork[HOOK], HookWorkCallback, hs);

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
    if (!hs->inDev) {
        AUDIO_DEVICE_LOG_ERR("failed to allocate input device.");
        return -ENOMEM;
    }
    hs->inDev->name = pdev->name;
    hs->inDev->open = AnalogHskeyOpen;
    hs->inDev->close = AnalogHskeyClose;
    hs->inDev->dev.parent = &pdev->dev;

    hs->inDev->id.vendor = INPUT_DEVID_VENDOR;
    hs->inDev->id.product = INPUT_DEVID_PRODUCT;
    hs->inDev->id.version = INPUT_DEVID_VERSION;
    /* Register the input device */
    ret = input_register_device(hs->inDev);
    if (ret) {
        AUDIO_DEVICE_LOG_ERR("failed to register input device.");
        return ret;
    }
    input_set_capability(hs->inDev, EV_KEY, HOOK_KEY_CODE);

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
        return -EINVAL;
    }

    pdata = hs->pdata;
    if (pdata->hsGpio) {
        irqType = GPIO_IRQ_TRIGGER_RISING | GPIO_IRQ_TRIGGER_FALLING | GPIO_IRQ_USING_THREAD;
        ret = GpioSetIrq(pdata->hsGpio, irqType, HeadsetInterrupt, NULL);
        if (ret != HDF_SUCCESS) {
            AUDIO_DEVICE_LOG_ERR("[GpioSetIrq] failed.");
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
        AUDIO_DEVICE_LOG_ERR("failed init headset, please full hsGpio function in board.");
        return -EEXIST;
    }
    if (pdata->hookGpio) {
        irqType = GPIO_IRQ_TRIGGER_RISING | GPIO_IRQ_TRIGGER_FALLING | GPIO_IRQ_USING_THREAD;
        ret = GpioSetIrq(pdata->hookGpio, irqType, HookInterrupt, NULL);
        if (ret != HDF_SUCCESS) {
            AUDIO_DEVICE_LOG_ERR("[GpioSetIrq] failed.");
            return ret;
        }
        GpioDisableIrq(hs->pdata->hookGpio);
    }

    return 0;
}

int32_t AnalogHeadsetGpioInit(struct platform_device *pdev, struct HeadsetPdata *pdata)
{
    int32_t ret;
    struct HeadsetPriv *hs;
    AUDIO_DEVICE_LOG_INFO("enter.");
    hs = (struct HeadsetPriv *)OsalMemCalloc(sizeof(*hs));
    if (hs == NULL) {
        AUDIO_DEVICE_LOG_ERR("failed to allocate driver data.");
        return HDF_ERR_MALLOC_FAIL;
    }
    g_hsInfo = hs;
    InitHeadsetPriv(hs, pdata);
    ret = CreateAndRegisterEdev(&pdev->dev, hs);
    if (ret < 0) {
        AUDIO_DEVICE_LOG_ERR("[CreateAndRegisterEdev] failed.");
        return ret;
    }
    (void)OsalMutexInit(&hs->mutexLk[HEADSET]);
    (void)OsalMutexInit(&hs->mutexLk[HOOK]);
    ret = InitWorkData(hs);
    if (ret != HDF_SUCCESS) {
        AUDIO_DEVICE_LOG_ERR("[InitWorkData] failed");
        return ret;
    }
    ret = OsalTimerCreate(&hs->hsTimer, HEADSET_TIMER_INTERVAL, HeadsetTimerCallback, (uintptr_t)hs);
    if (ret != HDF_SUCCESS) {
        AUDIO_DEVICE_LOG_ERR("[OsalTimerCreate] failed[%d]", ret);
        return ret;
    }
    /* Create and register the input driver */
    ret = CreateAndRegisterInputDevice(pdev, hs);
    if (ret != 0) {
        AUDIO_DEVICE_LOG_ERR("[CreateAndRegisterInputDevice] failed");
        return ret;
    }
    ret = CreateAndRegisterHdfInputDevice((void *)hs, pdata->device);
    if (ret != 0) {
        AUDIO_DEVICE_LOG_DEBUG("[CreateAndRegisterHdfInputDevice] failed");
    }
#ifdef CONFIG_HAS_EARLYSUSPEND
    g_hsEarlySuspend.suspend = NULL;
    g_hsEarlySuspend.resume = HeadsetEarlyResume;
    g_hsEarlySuspend.level = ~0x0;
    register_early_suspend(&g_hsEarlySuspend);
#endif
    ret = SetHeadsetIrqEnable(&pdev->dev, hs);
    if (ret != 0) {
        AUDIO_DEVICE_LOG_ERR("[SetHeadsetIrqEnable] failed");
        return ret;
    }
    (void)HdfAddDelayedWork(&hs->workQueue, &hs->hDelayedWork[HEADSET], DELAY_WORK_MS500);
    AUDIO_DEVICE_LOG_INFO("success.");
    return 0;
}

void AnalogHeadsetGpioRelease(struct HeadsetPdata *pdata)
{
    struct HeadsetPriv *hs = g_hsInfo;

    (void)pdata;
    if (hs == NULL) {
        AUDIO_DEVICE_LOG_ERR("hs is NULL.");
        return;
    }

    (void)OsalMutexLock(&hs->mutexLk[HEADSET]);
    g_hsInfo = NULL;
    OsalMutexUnlock(&hs->mutexLk[HEADSET]);

    OsalTimerDelete(&hs->hsTimer);
    HdfWorkDestroy(&hs->hDelayedWork[HEADSET]);
    HdfWorkDestroy(&hs->hDelayedWork[HOOK]);
    HdfWorkQueueDestroy(&hs->workQueue);
    DestroyHdfInputDevice();
    OsalMutexDestroy(&hs->mutexLk[HEADSET]);
    OsalMutexDestroy(&hs->mutexLk[HOOK]);
    OsalMemFree(hs);
    hs = NULL;
}