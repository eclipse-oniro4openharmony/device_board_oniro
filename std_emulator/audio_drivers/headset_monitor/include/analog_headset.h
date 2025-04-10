/*
 * Copyright (c) 2022 Huawei Device Co., Ltd.
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

#ifndef ANALOG_HEADSET_H
#define ANALOG_HEADSET_H

#include <linux/gpio.h>
#include <linux/of_gpio.h>
#include "analog_headset_ev.h"
#include "audio_device_log.h"
#include "hdf_base.h"
#include "hdf_device_desc.h"
#include "platform_if.h"

#define HDF_HEADSET_WORK_QUEUE_NAME    "hdf_headset_work_queue"
#define HEADSET                 0
#define HOOK                    1
#define HS_HOOK_COUNT           2

#define HEADSET_IN_HIGH         0x00000001
#define HEADSET_IN_LOW          0x00000000

#define HOOK_DOWN_HIGH          0x00000001
#define HOOK_DOWN_LOW           0x00000000

#define LEVEL_IN_HIGH           1
#define LEVEL_IN_LOW            0

#define DELAY_WORK_MS10         10
#define DELAY_WORK_MS50         50
#define DELAY_WORK_MS100        100
#define DELAY_WORK_MS200        200
#define DELAY_WORK_MS500        500

#define IRQ_CONFIRM_MS1         1
#define IRQ_CONFIRM_MS100       100
#define IRQ_CONFIRM_MS150       150

#define BIT_HEADSET_NULL        0
#define BIT_HEADSET             1
#define BIT_HEADSET_NO_MIC      2

#define GET_GPIO_REPEAT_TIMES   3
#define TIMER_EXPIRES_JIFFIES   100

struct AdcCtrlConfig {
    DevHandle handle;
    uint32_t devNum;
    uint32_t chanNo;
};

struct HeadsetPdata {
    struct IDeviceIoService ioService;
    struct HdfDeviceObject *device;
    uint32_t devType;
    const char *devName;
    /* heaset about */
    uint32_t hsGpio;
    enum of_gpio_flags hsGpioFlags;
    /* Headphones into the state level */
    uint32_t hsInsertType;
    uint32_t hsGpioFlag;
    bool isHookAdcMode;
    /* hook about */
    uint32_t hookGpio;
    /* Hook key down status */
    uint32_t hookDownType;
#ifdef CONFIG_MODEM_MIC_SWITCH
    /* mic about */
    enum of_gpio_flags micGpioFlags;
    uint32_t micSwitchGpio;
    uint32_t hpMicIoValue;
    uint32_t mainMicIoValue;
#endif
    struct iio_channel *chan;
    struct AdcCtrlConfig adcConfig;
    bool hsWakeup;
};

#define HOOK_KEY_CODE 226 // KEY_MEDIA

int32_t AnalogHeadsetGpioInit(struct platform_device *pdev, struct HeadsetPdata *pdata);
void AnalogHeadsetGpioRelease(struct HeadsetPdata *pdata);
int32_t AnalogHeadsetAdcInit(struct platform_device *pdev, struct HeadsetPdata *pdata);
void AnalogHeadsetAdcRelease(struct HeadsetPdata *pdata);
int AnalogHeadsetAdcSuspend(struct platform_device *pdev, pm_message_t state);
int AnalogHeadsetAdcResume(struct platform_device *pdev);
#endif
