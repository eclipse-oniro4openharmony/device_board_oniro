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

/* Playback Path */
#define     SND_NUMID_PLAYBACK_PATH          1
#define     SND_ELEM_PLAYBACK_PATH           "Playback Path"
#define     SND_OUT_CARD_OFF                 "0"   /* close play path */
#define     SND_OUT_CARD_RCV                 "1"   /* speaker */
#define     SND_OUT_CARD_SPK                 "2"   /* speaker */
#define     SND_OUT_CARD_HP                  "3"   /* headphone */
#define     SND_OUT_CARD_HP_NO_MIC           "4"   /* headphone */
#define     SND_OUT_CARD_BT                  "5"   /* bluetooth (Don't set!!!) */
#define     SND_OUT_CARD_SPK_HP              "6"   /* speaker and headphone */
#define     SND_OUT_CARD_RING_SPK            "7"   /* speaker */
#define     SND_OUT_CARD_RING_HP             "8"   /* headphone */
#define     SND_OUT_CARD_RING_HP_NO_MIC      "9"        /* headphone */
#define     SND_OUT_CARD_RING_SPK_HP         "10"  /* speaker and headphone */

/* Capture MIC Path */
#define     SND_NUMID_CAPUTRE_MIC_PATH       2
#define     SND_ELEM_CAPUTRE_MIC_PATH        "Capture MIC Path"
#define     SND_IN_CARD_MIC_OFF              "0"  /* close capture path */
#define     SND_IN_CARD_MAIN_MIC             "1"  /* main mic */
#define     SND_IN_CARD_HANDS_FREE_MIC       "2"  /* hands free mic */
#define     SND_IN_CARD_BT_SCO_MIC           "3"  /* bluetooth sco mic (Don't set!!!) */

/* DACL Playback Volume  */
#define     SND_NUMID_DACL_PLAYBACK_VOL      3
#define     SND_ELEM_DACL_PLAYBACK_VOL       "DACL Playback Volume"

/* DACR Playback Volume  */
#define     SND_NUMID_DACR_PLAYBACK_VOL      4
#define     SND_ELEM_DACR_PLAYBACK_VOL       "DACR Playback Volume"

/* DACL Capture Volume  */
#define     SND_NUMID_DACL_CAPTURE_VOL       5
#define     SND_ELEM_DACL_CAPTURE_VOL        "DACL Capture Volume"

/* DACR Capture Volume  */
#define     SND_NUMID_DACR_CAPTURE_VOL       6
#define     SND_ELEM_DACR_CAPTURE_VOL        "DACR Capture Volume"

#endif /* ALSA_SND_COMMON_H */
