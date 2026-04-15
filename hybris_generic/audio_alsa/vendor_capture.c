/*
 * Copyright (c) 2026 Oniro / Hybris Generic.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * MTK MT6789 + MT6366 capture-side vendor hooks for the OHOS alsa_adapter.
 * Volume uses `PGA1 Volume` as the master capture gain. `Mic Type Mux`
 * selects analog-mic topology (ACC) and `PGA L/R Mux` routes to AIN0.
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

static int32_t CaptureSelectSceneImpl(struct AlsaCapture *captureIns, enum AudioPortPin descPins,
    const struct PathDeviceInfo *deviceInfo)
{
    captureIns->descPins = descPins;
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

static int32_t CaptureStartImpl(struct AlsaCapture *captureIns)
{
    struct AlsaMixerCtlElement elem;
    struct AlsaSoundCard *cardIns = (struct AlsaSoundCard *)captureIns;
    CHECK_NULL_PTR_RETURN_DEFAULT(captureIns);

    /* Select analog ACC mic topology and route PGA L/R to AIN0. */
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
