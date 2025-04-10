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

#ifndef ANALOG_HEADSET_EV_H
#define ANALOG_HEADSET_EV_H

#define HEADSET_JACK_INDEX     3   // 0, 1=touch, 2=no response, 3=jack, 4 not found
#define INDEV_TYPE_HEADSET     4
#define KEY_JACK_HEADSET       20  // EXTCON_JACK_MICROPHONE(20)
#define KEY_JACK_HEADPHONE     21  // EXTCON_JACK_HEADPHONE(21)
#define KEY_JACK_HOOK          226 // KEY_MEDIA (226)

/**
 * @brief Audio event type.
 *
 * referenc the 'drivers\peripheral\audio\interfaces\include\audio_events.h',
 * the value don't modify!
 *
 * @since 1.0
 */
typedef enum AudioEventType {
    HDF_AUDIO_DEVICE_ADD = 0x1,
    HDF_AUDIO_DEVICE_REMOVE = 0x2,
} EVENT_TYPE;

typedef enum AudioDeviceType {
    HDF_AUDIO_HEADPHONE = 0x2,
    HDF_AUDIO_HEADSET = 0x4,
} DEVICE_TYPE;

struct AudioEvent {
    uint32_t eventType;
    uint32_t deviceType;
};

enum JackInsertStatus {
    JACK_STATUS_IN = 0,
    JACK_STATUS_OUT,
};

struct JackNotifyInfo {
    uint16_t jackType;
    bool jackStatus;
};

#endif