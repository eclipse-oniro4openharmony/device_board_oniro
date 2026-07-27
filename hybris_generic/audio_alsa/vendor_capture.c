/*
 * Copyright (c) 2026 Oniro / Hybris Generic.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * MTK capture-side vendor hooks for the OHOS alsa_adapter. Written against
 * MT6789+MT6366 (Volla X23 / Tablet); MT6878+MT6369 (Volla Phone Plinius)
 * support added with the ansuz port — the two codec generations expose
 * different mic-mux kcontrols, so each device's sequence is gated at
 * runtime on its controls being present (see common.h).
 * Volume uses `PGA1 Volume` as the master capture gain (same name on both).
 * mt6366: `Mic Type Mux` selects ACC and `PGA L/R Mux` routes to AIN0.
 * mt6369: AMIC via UL_SRC/PGA_x/ADC_x/MISO muxes, plus the UL9 AFE route
 * (Capture_1 == memif VUL9 on the mt6878 AFE).
 */

#include "alsa_snd_capture.h"
#include "common.h"

#define HDF_LOG_TAG HDF_AUDIO_HAL_CAPTURE

typedef struct _CAPTURE_DATA_ {
    struct AlsaMixerCtlElement ctrlVolume;
    long tempVolume;
} CaptureData;

static int32_t CaptureInitImpl(struct AlsaCapture *captureIns)
{
    int32_t ret;
    struct AlsaMixerCtlElement elem;
    struct AlsaSoundCard *cardIns = (struct AlsaSoundCard *)captureIns;

    if (captureIns->priData != NULL) {
        return HDF_SUCCESS;
    }
    CHECK_NULL_PTR_RETURN_DEFAULT(captureIns);

    CaptureData *priData = (CaptureData *)OsalMemCalloc(sizeof(CaptureData));
    if (priData == NULL) {
        AUDIO_FUNC_LOGE("Failed to allocate memory!");
        return HDF_FAILURE;
    }
    SndElementItemInit(&priData->ctrlVolume);
    priData->ctrlVolume.numid = SND_NUMID_PGA1_VOL;
    priData->ctrlVolume.name = SND_ELEM_PGA1_VOL;
    CaptureSetPriData(captureIns, (CapturePriData)priData);

    SndElementItemInit(&elem);
    elem.numid = SND_NUMID_PGA1_VOL;
    elem.name = SND_ELEM_PGA1_VOL;
    elem.value = SND_IN_CAPTURE_DEFAULT_VOLUME;
    ret = SndElementWrite(cardIns, &elem);
    if (ret != HDF_SUCCESS) {
        AUDIO_FUNC_LOGE("write PGA1 volume fail!");
        return HDF_FAILURE;
    }
    SndElementItemInit(&elem);
    elem.numid = SND_NUMID_PGA2_VOL;
    elem.name = SND_ELEM_PGA2_VOL;
    elem.value = SND_IN_CAPTURE_DEFAULT_VOLUME;
    (void)SndElementWrite(cardIns, &elem);

    return HDF_SUCCESS;
}

static int32_t CaptureSelectSceneImpl(struct AlsaCapture *captureIns, const struct AudioHwCaptureParam *handleData)
{
    /* MT6789 has a single analog capture path, so scene selection is a no-op;
     * the mic topology is programmed in CaptureStartImpl. */
    (void)captureIns;
    (void)handleData;
    return HDF_SUCCESS;
}

static int32_t CaptureGetVolThresholdImpl(struct AlsaCapture *captureIns, long *volMin, long *volMax)
{
    int32_t ret;
    struct AlsaSoundCard *cardIns = (struct AlsaSoundCard *)captureIns;
    CaptureData *priData = CaptureGetPriData(captureIns);
    CHECK_NULL_PTR_RETURN_DEFAULT(cardIns);
    CHECK_NULL_PTR_RETURN_DEFAULT(priData);
    ret = SndElementReadRange(cardIns, &priData->ctrlVolume, volMin, volMax);
    if (ret != HDF_SUCCESS) {
        AUDIO_FUNC_LOGE("SndElementReadRange fail!");
        return HDF_FAILURE;
    }
    return HDF_SUCCESS;
}

static int32_t CaptureGetVolumeImpl(struct AlsaCapture *captureIns, long *volume)
{
    int32_t ret;
    struct AlsaSoundCard *cardIns = (struct AlsaSoundCard *)captureIns;
    CaptureData *priData = CaptureGetPriData(captureIns);
    CHECK_NULL_PTR_RETURN_DEFAULT(cardIns);
    CHECK_NULL_PTR_RETURN_DEFAULT(priData);
    ret = SndElementReadInt(cardIns, &priData->ctrlVolume, volume);
    if (ret != HDF_SUCCESS) {
        AUDIO_FUNC_LOGE("Read capture volume fail!");
        return HDF_FAILURE;
    }
    return HDF_SUCCESS;
}

static int32_t CaptureSetVolumeImpl(struct AlsaCapture *captureIns, long volume)
{
    int32_t ret;
    struct AlsaSoundCard *cardIns = (struct AlsaSoundCard *)captureIns;
    CaptureData *priData = CaptureGetPriData(captureIns);
    CHECK_NULL_PTR_RETURN_DEFAULT(cardIns);
    CHECK_NULL_PTR_RETURN_DEFAULT(priData);
    ret = SndElementWriteInt(cardIns, &priData->ctrlVolume, volume);
    if (ret != HDF_SUCCESS) {
        AUDIO_FUNC_LOGE("Write capture volume fail!");
        return HDF_FAILURE;
    }
    struct AlsaMixerCtlElement pga2;
    SndElementItemInit(&pga2);
    pga2.numid = SND_NUMID_PGA2_VOL;
    pga2.name = SND_ELEM_PGA2_VOL;
    (void)SndElementWriteInt(cardIns, &pga2, volume);
    return HDF_SUCCESS;
}

static int32_t CaptureSetMuteImpl(struct AlsaCapture *captureIns, bool muteFlag)
{
    int32_t ret;
    long vol, setVol;
    CaptureData *priData = CaptureGetPriData(captureIns);
    CHECK_NULL_PTR_RETURN_DEFAULT(captureIns);
    CHECK_NULL_PTR_RETURN_DEFAULT(priData);
    ret = captureIns->GetVolume(captureIns, &vol);
    if (ret != HDF_SUCCESS) {
        AUDIO_FUNC_LOGE("GetVolume failed!");
        return HDF_FAILURE;
    }
    if (muteFlag) {
        priData->tempVolume = vol;
        setVol = 0;
    } else {
        setVol = priData->tempVolume;
    }
    captureIns->SetVolume(captureIns, setVol);
    captureIns->muteState = muteFlag;
    return HDF_SUCCESS;
}

/* mt6366 (X23 / tablet) analog mic front-end: ACC topology, PGA on AIN0. */
static void CaptureRouteMt6366(struct AlsaSoundCard *cardIns)
{
    struct AlsaMixerCtlElement elem;

    SndElementItemInit(&elem);
    elem.numid = SND_NUMID_MIC_TYPE_MUX;
    elem.name = SND_ELEM_MIC_TYPE_MUX;
    elem.value = "ACC";
    (void)SndElementWrite(cardIns, &elem);

    SndElementItemInit(&elem);
    elem.numid = SND_NUMID_PGA_L_MUX;
    elem.name = SND_ELEM_PGA_L_MUX;
    elem.value = "AIN0";
    (void)SndElementWrite(cardIns, &elem);

    SndElementItemInit(&elem);
    elem.numid = SND_NUMID_PGA_R_MUX;
    elem.name = SND_ELEM_PGA_R_MUX;
    elem.value = "AIN0";
    (void)SndElementWrite(cardIns, &elem);
}

/* mt6369 (Plinius) analog mic front-end + mt6878 AFE capture route.
 * Verified live on ansuz 2026-07-27 (speaker->mic loopback of a 440 Hz
 * tone). The UL9 route is load-bearing: without it snd_pcm_hw_params on
 * Capture_1 fails EINVAL with "no backend DAIs enabled for Capture_1". */
static void CaptureRouteMt6369(struct AlsaSoundCard *cardIns)
{
    struct AlsaMixerCtlElement elem;

    SndElementItemInit(&elem);
    elem.numid = 0;
    elem.name = SND_ELEM_UL_SRC_MUX;
    elem.value = SND_UL_SRC_AMIC;
    (void)SndElementWrite(cardIns, &elem);

    SndElementItemInit(&elem);
    elem.numid = 0;
    elem.name = SND_ELEM_PGA_L_MUX_6878;
    elem.value = "AIN0";
    (void)SndElementWrite(cardIns, &elem);

    SndElementItemInit(&elem);
    elem.numid = 0;
    elem.name = SND_ELEM_PGA_R_MUX_6878;
    elem.value = "AIN0";
    (void)SndElementWrite(cardIns, &elem);

    SndElementItemInit(&elem);
    elem.numid = 0;
    elem.name = SND_ELEM_ADC_L_MUX;
    elem.value = SND_ADC_L_PREAMP;
    (void)SndElementWrite(cardIns, &elem);

    SndElementItemInit(&elem);
    elem.numid = 0;
    elem.name = SND_ELEM_ADC_R_MUX;
    elem.value = SND_ADC_R_PREAMP;
    (void)SndElementWrite(cardIns, &elem);

    SndElementItemInit(&elem);
    elem.numid = 0;
    elem.name = SND_ELEM_MISO0_MUX;
    elem.value = SND_MISO0_UL1_CH1;
    (void)SndElementWrite(cardIns, &elem);

    SndElementItemInit(&elem);
    elem.numid = 0;
    elem.name = SND_ELEM_MISO1_MUX;
    elem.value = SND_MISO1_UL1_CH2;
    (void)SndElementWrite(cardIns, &elem);

    SndElementItemInit(&elem);
    elem.numid = 0;
    elem.name = SND_ELEM_UL9_CH1_ADDA_UL_CH1;
    elem.value = "on";
    (void)SndElementWrite(cardIns, &elem);

    SndElementItemInit(&elem);
    elem.numid = 0;
    elem.name = SND_ELEM_UL9_CH2_ADDA_UL_CH2;
    elem.value = "on";
    (void)SndElementWrite(cardIns, &elem);
}

static int32_t CaptureStartImpl(struct AlsaCapture *captureIns, const struct AudioHwCaptureParam *handleData)
{
    (void)handleData;
    struct AlsaSoundCard *cardIns = (struct AlsaSoundCard *)captureIns;
    CHECK_NULL_PTR_RETURN_DEFAULT(captureIns);

    /* The two codec generations share no mic-mux control names, so probe
     * one fingerprint control from each and program whichever is present. */
    if (SndKcontrolExists(cardIns, SND_ELEM_MIC_TYPE_MUX)) {
        CaptureRouteMt6366(cardIns);
    }
    if (SndKcontrolExists(cardIns, SND_ELEM_UL_SRC_MUX)) {
        CaptureRouteMt6369(cardIns);
    }

    return HDF_SUCCESS;
}

static int32_t CaptureStopImpl(struct AlsaCapture *captureIns)
{
    CHECK_NULL_PTR_RETURN_DEFAULT(captureIns);
    if (captureIns->soundCard.pcmHandle != NULL) {
        snd_pcm_drop(captureIns->soundCard.pcmHandle);
    }
    return HDF_SUCCESS;
}

static int32_t CaptureGetGainThresholdImpl(struct AlsaCapture *captureIns, float *gainMin, float *gainMax)
{
    (void)captureIns;
    if (gainMin) *gainMin = 0.0f;
    if (gainMax) *gainMax = 0.0f;
    return HDF_SUCCESS;
}

static int32_t CaptureGetGainImpl(struct AlsaCapture *captureIns, float *volume)
{
    (void)captureIns;
    if (volume) *volume = 0.0f;
    return HDF_SUCCESS;
}

static int32_t CaptureSetGainImpl(struct AlsaCapture *captureIns, float volume)
{
    (void)captureIns;
    (void)volume;
    return HDF_SUCCESS;
}

static bool CaptureGetMuteImpl(struct AlsaCapture *captureIns)
{
    return captureIns->muteState;
}

int32_t CaptureOverrideFunc(struct AlsaCapture *captureIns)
{
    if (captureIns == NULL) {
        return HDF_FAILURE;
    }
    struct AlsaSoundCard *cardIns = (struct AlsaSoundCard *)captureIns;

    if (cardIns->cardType == SND_CARD_PRIMARY) {
        captureIns->Init = CaptureInitImpl;
        captureIns->SelectScene = CaptureSelectSceneImpl;
        captureIns->Start = CaptureStartImpl;
        captureIns->Stop = CaptureStopImpl;
        captureIns->GetVolThreshold = CaptureGetVolThresholdImpl;
        captureIns->GetVolume = CaptureGetVolumeImpl;
        captureIns->SetVolume = CaptureSetVolumeImpl;
        captureIns->GetGainThreshold = CaptureGetGainThresholdImpl;
        captureIns->GetGain = CaptureGetGainImpl;
        captureIns->SetGain = CaptureSetGainImpl;
        captureIns->GetMute = CaptureGetMuteImpl;
        captureIns->SetMute = CaptureSetMuteImpl;
    }
    return HDF_SUCCESS;
}

/*
 * Map an audio scene to a PCM sub-device index for card-list selection.
 * The MT6789 exposes a single AFE capture PCM device; call audio is routed
 * through the Halium/Android RIL path rather than this ALSA adapter, so every
 * scene resolves to the default device (-1 => let the adapter pick card 0).
 */
int32_t CaptureGetSceneDev(enum AudioCategory scene)
{
    (void)scene;
    return -1;
}
