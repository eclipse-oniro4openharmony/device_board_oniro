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

#ifndef ALSA_SND_COMMON_H
#define ALSA_SND_COMMON_H

/* Auto-Mute Mode  */
#define     SND_NUMID_AUTO_MUTE_MODE      8
#define     SND_ELEM_AUTO_MUTE_MODE       "Auto-Mute Mode"

/* Capture Volume  */
#define     SND_NUMID_CAPTURE_VOL      9
#define     SND_ELEM_CAPTURE_VOL       "Capture Volume"
#define     SND_IN_CAPTURE_DEFAULT_VOLUME   "50"  /* min:0 max:63  */

/* Capture Switch  */
#define     SND_NUMID_CAPTURE_SWITCH      10
#define     SND_ELEM_CAPTURE_SWITCH       "Capture Switch"
#define     SND_IN_CARD_ON                       "on"
#define     SND_IN_CARD_OFF                      "off"

/* Master Playback Volume  */
#define     SND_NUMID_MASTER_PLAYBACK_VOL      13
#define     SND_ELEM_MASTER_PLAYBACK_VOL       "Master Playback Volume"
#define     SND_OUT_PLAYBACK_DEFAULT_VOLUME   "80"  /* min:0 max:87  */

/* Master Playback Switch  */
#define     SND_NUMID_MASTER_PLAYBACK_SWITCH      14
#define     SND_ELEM_MASTER_PLAYBACK_SWITCH       "Master Playback Switch"
#define     SND_OUT_CARD_ON                       "on"
#define     SND_OUT_CARD_OFF                      "off"


#endif /* ALSA_SND_COMMON_H */
