/*
 * Copyright (c) 2026 Oniro / Hybris Generic.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 */

#ifndef ALSA_SND_COMMON_H
#define ALSA_SND_COMMON_H

/* MT6789 + MT6366 codec mixer controls (captured via amixer -c 0 on Volla X23)
 * See phase13_audio_support.md (13B.3/13B.4).
 */

/* Lineout Volume -> main loudspeaker volume. stereo, range 0..18, dB step 1.00 */
#define SND_NUMID_LINEOUT_VOL        286
#define SND_ELEM_LINEOUT_VOL         "Lineout Volume"
#define SND_OUT_PLAYBACK_DEFAULT_VOLUME   "12"  /* min:0 max:18 */

/* Headset Volume -> headphone volume, same range */
#define SND_NUMID_HEADSET_VOL        285
#define SND_ELEM_HEADSET_VOL         "Headset Volume"

/* Ext_Speaker_Amp Switch -> external PA enable (BOOLEAN) */
#define SND_NUMID_EXT_SPK_AMP_SWITCH 305
#define SND_ELEM_EXT_SPK_AMP_SWITCH  "Ext_Speaker_Amp Switch"
#define SND_OUT_CARD_ON              "on"
#define SND_OUT_CARD_OFF             "off"

/* HPL / HPR Mux  (route selector for headphone L/R.
 * Items: 0=Open 1=LoudSPK Playback 2=Audio Playback 3=Test Mode
 *        4=HP Impedance 5=Loud DualSPK Playback */
#define SND_NUMID_HPL_MUX            311
#define SND_ELEM_HPL_MUX             "HPL Mux"
#define SND_NUMID_HPR_MUX            312
#define SND_ELEM_HPR_MUX             "HPR Mux"
#define SND_HPX_MUX_AUDIO_PLAYBACK   2

/* PGA1 / PGA2 Volume -> mic gain, range 0..4 dB step 6.00 */
#define SND_NUMID_PGA1_VOL           288
#define SND_ELEM_PGA1_VOL            "PGA1 Volume"
#define SND_NUMID_PGA2_VOL           289
#define SND_ELEM_PGA2_VOL            "PGA2 Volume"
#define SND_IN_CAPTURE_DEFAULT_VOLUME    "3"   /* min:0 max:4 */

/* PGA L / PGA R Mux -> mic source (Items: 0=None 1=AIN0 2=AIN1 3=AIN2) */
#define SND_NUMID_PGA_L_MUX          318
#define SND_ELEM_PGA_L_MUX           "PGA L Mux"
#define SND_NUMID_PGA_R_MUX          319
#define SND_ELEM_PGA_R_MUX           "PGA R Mux"
#define SND_PGA_MUX_AIN0             1

/* Mic Type Mux -> mic topology
 * Items: 0=Idle 1=ACC 2=DMIC 3=DCC 4=DCC_ECM_DIFF 5=DCC_ECM_SINGLE */
#define SND_NUMID_MIC_TYPE_MUX       315
#define SND_ELEM_MIC_TYPE_MUX        "Mic Type Mux"
#define SND_MIC_TYPE_ACC             1

/* DAPM route mixers — wire the ALSA FE (Playback_1 = DL1) to the ADDA DAC.
 * Without these the MT6789 ASoC core refuses `snd_pcm_hw_params` on any
 * playback device with "no backend DAIs enabled for Playback_X". The Android
 * MTK HAL sets up equivalent routes via its audio_policy path; in our native
 * ALSA path vendor_render.c enables them directly. */
#define SND_NUMID_ADDA_DL_CH1_DL1_CH1   211
#define SND_ELEM_ADDA_DL_CH1_DL1_CH1    "ADDA_DL_CH1 DL1_CH1"
#define SND_NUMID_ADDA_DL_CH2_DL1_CH2   226
#define SND_ELEM_ADDA_DL_CH2_DL1_CH2    "ADDA_DL_CH2 DL1_CH2"

#endif /* ALSA_SND_COMMON_H */
