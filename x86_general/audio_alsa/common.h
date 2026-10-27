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

/*
 * QEMU's ES1370 (card "AudioPCI") carries an AK4531 codec. Its controls are
 * looked up by name: numid 0 leaves the numid unpinned, so the kernel matches
 * name + index. QEMU stores codec writes without applying them, so on the
 * emulator these only have to exist for the adapter's Init/Start to succeed.
 */
#define     SND_NUMID_BY_NAME                  0

/* Master Playback Volume  */
#define     SND_ELEM_MASTER_PLAYBACK_VOL       "Master Playback Volume"
#define     SND_OUT_PLAYBACK_DEFAULT_VOLUME   "31"  /* min:0 max:31, 31 = 0 dB */

/* Master Playback Switch  */
#define     SND_ELEM_MASTER_PLAYBACK_SWITCH    "Master Playback Switch"
#define     SND_OUT_CARD_ON                       "on"
#define     SND_OUT_CARD_OFF                      "off"

/* PCM (index 0) is DAC2, the ALSA pcm device 0 the adapter plays to  */
#define     SND_ELEM_PCM_SWITCH                "PCM Switch"
#define     SND_ELEM_PCM_VOL                   "PCM Volume"
#define     SND_ELEM_PCM_PLAYBACK_SWITCH       "PCM Playback Switch"
#define     SND_PCM_DEFAULT_VOLUME            "25"  /* min:0 max:31, 25 = 0 dB */

/* Capture: the mic input routed to the ADC  */
#define     SND_ELEM_CAPTURE_VOL               "Mic Volume"
#define     SND_IN_CAPTURE_DEFAULT_VOLUME     "25"  /* min:0 max:31, 25 = 0 dB */
#define     SND_ELEM_CAPTURE_SWITCH            "Mic Switch"
#define     SND_ELEM_CAPTURE_ROUTE             "Mic Capture Switch"
#define     SND_IN_CARD_ON                       "on"
#define     SND_IN_CARD_OFF                      "off"

#endif /* ALSA_SND_COMMON_H */
