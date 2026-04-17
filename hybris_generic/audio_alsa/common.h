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

/* MT6789 + MT6366 codec mixer controls.
 *
 * Numids are set to 0 here so `SndElementWrite`/`SndElementReadInt`/
 * `SndElementReadRange` look the element up by *name* (via
 * `snd_ctl_elem_id_set_name` inside `SetElementInfo`). The kernel on the
 * Volla X23 and the Volla Tablet (mimir) ships slightly different codec
 * drivers: the X23 has a few extra mixer entries ("DAC In Mux",
 * "MTK_SPK_TYPE_GET", etc.) inserted before the speaker/headphone controls,
 * which shifts the numid of every subsequent control by +2. Name-based
 * lookup sidesteps this entirely and stays correct across kernel changes.
 * See phase13_audio_support.md.
 */

/* Lineout Volume -> main loudspeaker volume. stereo, range 0..18, dB step 1.00 */
#define SND_NUMID_LINEOUT_VOL        0
#define SND_ELEM_LINEOUT_VOL         "Lineout Volume"
#define SND_OUT_PLAYBACK_DEFAULT_VOLUME   "12"  /* min:0 max:18 */

/* Headset Volume -> headphone volume, same range */
#define SND_NUMID_HEADSET_VOL        0
#define SND_ELEM_HEADSET_VOL         "Headset Volume"

/* Ext_Speaker_Amp Switch -> external PA enable (BOOLEAN) */
#define SND_NUMID_EXT_SPK_AMP_SWITCH 0
#define SND_ELEM_EXT_SPK_AMP_SWITCH  "Ext_Speaker_Amp Switch"
#define SND_OUT_CARD_ON              "on"
#define SND_OUT_CARD_OFF             "off"

/* HPL / HPR Mux  (route selector for headphone L/R.
 * Items: 0=Open 1=LoudSPK Playback 2=Audio Playback 3=Test Mode
 *        4=HP Impedance 5=Loud DualSPK Playback */
#define SND_NUMID_HPL_MUX            0
#define SND_ELEM_HPL_MUX             "HPL Mux"
#define SND_NUMID_HPR_MUX            0
#define SND_ELEM_HPR_MUX             "HPR Mux"
#define SND_HPX_MUX_AUDIO_PLAYBACK   2

/* PGA1 / PGA2 Volume -> mic gain, range 0..4 dB step 6.00 */
#define SND_NUMID_PGA1_VOL           0
#define SND_ELEM_PGA1_VOL            "PGA1 Volume"
#define SND_NUMID_PGA2_VOL           0
#define SND_ELEM_PGA2_VOL            "PGA2 Volume"
#define SND_IN_CAPTURE_DEFAULT_VOLUME    "3"   /* min:0 max:4 */

/* PGA L / PGA R Mux -> mic source (Items: 0=None 1=AIN0 2=AIN1 3=AIN2) */
#define SND_NUMID_PGA_L_MUX          0
#define SND_ELEM_PGA_L_MUX           "PGA L Mux"
#define SND_NUMID_PGA_R_MUX          0
#define SND_ELEM_PGA_R_MUX           "PGA R Mux"
#define SND_PGA_MUX_AIN0             1

/* Mic Type Mux -> mic topology
 * Items: 0=Idle 1=ACC 2=DMIC 3=DCC 4=DCC_ECM_DIFF 5=DCC_ECM_SINGLE */
#define SND_NUMID_MIC_TYPE_MUX       0
#define SND_ELEM_MIC_TYPE_MUX        "Mic Type Mux"
#define SND_MIC_TYPE_ACC             1

/* DAPM route mixers — wire the ALSA FE (Playback_1 = DL1) to the ADDA DAC.
 * Without these the MT6789 ASoC core refuses `snd_pcm_hw_params` on any
 * playback device with "no backend DAIs enabled for Playback_X". The Android
 * MTK HAL sets up equivalent routes via its audio_policy path; in our native
 * ALSA path vendor_render.c enables them directly.
 *
 * The tablet speaker output physically goes ADDA -> Lineout -> Ext_Speaker_Amp,
 * so the ADDA_DL_CH* mixers drive tablet audibility. The Volla X23 uses a
 * different path: DL1 -> I2S3 -> AW883xx smart PA (see I2S3 mixers below).
 * Enabling both sets of routes is safe on both devices — on the tablet the
 * I2S3 path has no physical output; on the X23 the ADDA/Lineout path has no
 * physical speaker connection (the MT6366 Lineout pins drive the headphone
 * jack only). */
#define SND_NUMID_ADDA_DL_CH1_DL1_CH1   0
#define SND_ELEM_ADDA_DL_CH1_DL1_CH1    "ADDA_DL_CH1 DL1_CH1"
#define SND_NUMID_ADDA_DL_CH2_DL1_CH2   0
#define SND_ELEM_ADDA_DL_CH2_DL1_CH2    "ADDA_DL_CH2 DL1_CH2"

/* I2S3 backend route — feeds the AW883xx smart PA on the X23. Items for
 * I2S3_Out_Mux: 0=Normal (drive I2S3 TX pins), 1=Dummy_Widget. */
#define SND_ELEM_I2S3_OUT_MUX           "I2S3_Out_Mux"
#define SND_I2S3_OUT_MUX_NORMAL         "0"
#define SND_ELEM_I2S3_CH1_DL1_CH1       "I2S3_CH1 DL1_CH1"
#define SND_ELEM_I2S3_CH2_DL1_CH2       "I2S3_CH2 DL1_CH2"

/* AWINIC AW883xx smart PA — present on Volla X23 (MTK_SPK_AWINIC_AW883XX),
 * absent on Volla Tablet (mimir, different PA). Writing to these kcontrols
 * on the tablet fails silently via `(void)SndElementWrite(...)` and costs
 * nothing. On the X23 this is load-bearing: without aw_dev_0_switch=Enable
 * the speaker output is silent even with the ADDA→Lineout route correct.
 *   - aw_dev_0_prof:   Music (0) / Receiver (1)
 *   - aw_dev_0_switch: Disable (0) / Enable (1) */
#define SND_ELEM_AW_DEV_0_PROF          "aw_dev_0_prof"
#define SND_ELEM_AW_DEV_0_SWITCH        "aw_dev_0_switch"
#define SND_AW_PROF_MUSIC               "0"
#define SND_AW_SWITCH_ENABLE            "1"
#define SND_AW_SWITCH_DISABLE           "0"

#endif /* ALSA_SND_COMMON_H */
