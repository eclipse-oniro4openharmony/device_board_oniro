/*
 * Copyright (c) 2026 Oniro / Hybris Generic.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * MTK MT6789 + MT6366 render-side vendor hooks for the OHOS alsa_adapter.
 * Written as part of Phase 13B (native ALSA audio).
 *
 * Volume control uses `Lineout Volume` (loudspeaker) as the master integer
 * control. `Ext_Speaker_Amp Switch` is toggled at Start/Stop.
 */

#include <alsa/asoundlib.h>
#include "alsa_snd_render.h"
#include "common.h"

#define HDF_LOG_TAG HDF_AUDIO_HAL_RENDER

typedef struct _RENDER_DATA_ {
    struct AlsaMixerCtlElement ctrlVolume;
    long tempVolume;
} RenderData;

static int32_t RenderInitImpl(struct AlsaRender *renderIns)
{
    int32_t ret;
    struct AlsaMixerCtlElement elem;
    struct AlsaSoundCard *cardIns = (struct AlsaSoundCard *)renderIns;

    if (renderIns->priData != NULL) {
        return HDF_SUCCESS;
    }
    CHECK_NULL_PTR_RETURN_DEFAULT(renderIns);

    RenderData *priData = (RenderData *)OsalMemCalloc(sizeof(RenderData));
    if (priData == NULL) {
        AUDIO_FUNC_LOGE("Failed to allocate memory!");
        return HDF_FAILURE;
    }

    SndElementItemInit(&priData->ctrlVolume);
    priData->ctrlVolume.numid = SND_NUMID_LINEOUT_VOL;
    priData->ctrlVolume.name = SND_ELEM_LINEOUT_VOL;
    RenderSetPriData(renderIns, (RenderPriData)priData);

    SndElementItemInit(&elem);
    elem.numid = SND_NUMID_LINEOUT_VOL;
    elem.name = SND_ELEM_LINEOUT_VOL;
    elem.value = SND_OUT_PLAYBACK_DEFAULT_VOLUME;
    ret = SndElementWrite(cardIns, &elem);
    if (ret != HDF_SUCCESS) {
        AUDIO_FUNC_LOGE("write lineout volume fail!");
        return HDF_FAILURE;
    }

    /* Enable the DL1 -> ADDA DAC DAPM routes. This must happen before
     * snd_pcm_hw_params (which the adapter core calls after Init) because
     * the MT6789 ASoC driver otherwise rejects `hw_params` with the kernel
     * log `Playback_X: ASoC: no backend DAIs enabled`. Open-then-Init is
     * the earliest point we can reach these mixers from the alsa_adapter. */
    SndElementItemInit(&elem);
    elem.numid = SND_NUMID_ADDA_DL_CH1_DL1_CH1;
    elem.name = SND_ELEM_ADDA_DL_CH1_DL1_CH1;
    elem.value = "on";
    (void)SndElementWrite(cardIns, &elem);

    SndElementItemInit(&elem);
    elem.numid = SND_NUMID_ADDA_DL_CH2_DL1_CH2;
    elem.name = SND_ELEM_ADDA_DL_CH2_DL1_CH2;
    elem.value = "on";
    (void)SndElementWrite(cardIns, &elem);

    /* Power up the HP and speaker routes so DAPM keeps the DAC on. */
    SndElementItemInit(&elem);
    elem.numid = SND_NUMID_HPL_MUX;
    elem.name = SND_ELEM_HPL_MUX;
    elem.value = "Audio Playback";
    (void)SndElementWrite(cardIns, &elem);

    SndElementItemInit(&elem);
    elem.numid = SND_NUMID_HPR_MUX;
    elem.name = SND_ELEM_HPR_MUX;
    elem.value = "Audio Playback";
    (void)SndElementWrite(cardIns, &elem);

    /* Toggle Off -> On so DAPM fires its power-up sequencer and actually
     * engages the external speaker amplifier. Writing "on" when the kcontrol
     * is already "on" is a no-op for DAPM. See Phase 13B doc (13B.9). */
    SndElementItemInit(&elem);
    elem.numid = SND_NUMID_EXT_SPK_AMP_SWITCH;
    elem.name = SND_ELEM_EXT_SPK_AMP_SWITCH;
    elem.value = SND_OUT_CARD_OFF;
    (void)SndElementWrite(cardIns, &elem);
    SndElementItemInit(&elem);
    elem.numid = SND_NUMID_EXT_SPK_AMP_SWITCH;
    elem.name = SND_ELEM_EXT_SPK_AMP_SWITCH;
    elem.value = SND_OUT_CARD_ON;
    (void)SndElementWrite(cardIns, &elem);

    return HDF_SUCCESS;
}

static int32_t RenderSelectSceneImpl(struct AlsaRender *renderIns, enum AudioPortPin descPins,
    const struct PathDeviceInfo *deviceInfo)
{
    renderIns->descPins = descPins;
    return HDF_SUCCESS;
}

static int32_t RenderGetVolThresholdImpl(struct AlsaRender *renderIns, long *volMin, long *volMax)
{
    int32_t ret;
    struct AlsaSoundCard *cardIns = (struct AlsaSoundCard *)renderIns;
    RenderData *priData = RenderGetPriData(renderIns);
    CHECK_NULL_PTR_RETURN_DEFAULT(cardIns);
    CHECK_NULL_PTR_RETURN_DEFAULT(priData);

    ret = SndElementReadRange(cardIns, &priData->ctrlVolume, volMin, volMax);
    if (ret != HDF_SUCCESS) {
        AUDIO_FUNC_LOGE("SndElementReadRange fail!");
        return HDF_FAILURE;
    }
    return HDF_SUCCESS;
}

static int32_t RenderGetVolumeImpl(struct AlsaRender *renderIns, long *volume)
{
    int32_t ret;
    struct AlsaSoundCard *cardIns = (struct AlsaSoundCard *)renderIns;
    RenderData *priData = RenderGetPriData(renderIns);
    CHECK_NULL_PTR_RETURN_DEFAULT(cardIns);
    CHECK_NULL_PTR_RETURN_DEFAULT(priData);

    ret = SndElementReadInt(cardIns, &priData->ctrlVolume, volume);
    if (ret != HDF_SUCCESS) {
        AUDIO_FUNC_LOGE("Read volume fail!");
        return HDF_FAILURE;
    }
    return HDF_SUCCESS;
}

static int32_t RenderSetVolumeImpl(struct AlsaRender *renderIns, long volume)
{
    int32_t ret;
    struct AlsaSoundCard *cardIns = (struct AlsaSoundCard *)renderIns;
    RenderData *priData = RenderGetPriData(renderIns);
    CHECK_NULL_PTR_RETURN_DEFAULT(cardIns);
    CHECK_NULL_PTR_RETURN_DEFAULT(priData);

    ret = SndElementWriteInt(cardIns, &priData->ctrlVolume, volume);
    if (ret != HDF_SUCCESS) {
        AUDIO_FUNC_LOGE("Write volume fail!");
        return HDF_FAILURE;
    }
    /* Mirror on headphone volume for consistent user experience */
    struct AlsaMixerCtlElement hp;
    SndElementItemInit(&hp);
    hp.numid = SND_NUMID_HEADSET_VOL;
    hp.name = SND_ELEM_HEADSET_VOL;
    (void)SndElementWriteInt(cardIns, &hp, volume);
    return HDF_SUCCESS;
}

static bool RenderGetMuteImpl(struct AlsaRender *renderIns)
{
    return renderIns->muteState;
}

static int32_t RenderSetMuteImpl(struct AlsaRender *renderIns, bool muteFlag)
{
    int32_t ret;
    long vol, setVol;
    RenderData *priData = RenderGetPriData(renderIns);
    CHECK_NULL_PTR_RETURN_DEFAULT(renderIns);
    CHECK_NULL_PTR_RETURN_DEFAULT(priData);

    ret = renderIns->GetVolume(renderIns, &vol);
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
    renderIns->SetVolume(renderIns, setVol);
    renderIns->muteState = muteFlag;
    return HDF_SUCCESS;
}

static int32_t RenderStartImpl(struct AlsaRender *renderIns)
{
    int32_t ret;
    struct AlsaMixerCtlElement elem;
    struct AlsaSoundCard *cardIns = (struct AlsaSoundCard *)renderIns;

    /* Route headphone L/R to Audio Playback (harmless if speaker is in use;
     * the headset path is only physically live when the jack is plugged). */
    SndElementItemInit(&elem);
    elem.numid = SND_NUMID_HPL_MUX;
    elem.name = SND_ELEM_HPL_MUX;
    elem.value = "Audio Playback";
    (void)SndElementWrite(cardIns, &elem);

    SndElementItemInit(&elem);
    elem.numid = SND_NUMID_HPR_MUX;
    elem.name = SND_ELEM_HPR_MUX;
    elem.value = "Audio Playback";
    (void)SndElementWrite(cardIns, &elem);

    /* Re-assert the Off -> On toggle on every Start so DAPM re-runs its
     * power-up sequence even if the kcontrol was already "on" from Init. */
    SndElementItemInit(&elem);
    elem.numid = SND_NUMID_EXT_SPK_AMP_SWITCH;
    elem.name = SND_ELEM_EXT_SPK_AMP_SWITCH;
    elem.value = SND_OUT_CARD_OFF;
    (void)SndElementWrite(cardIns, &elem);
    SndElementItemInit(&elem);
    elem.numid = SND_NUMID_EXT_SPK_AMP_SWITCH;
    elem.name = SND_ELEM_EXT_SPK_AMP_SWITCH;
    elem.value = SND_OUT_CARD_ON;
    ret = SndElementWrite(cardIns, &elem);
    if (ret != HDF_SUCCESS) {
        AUDIO_FUNC_LOGE("enable Ext_Speaker_Amp Switch failed!");
        return HDF_FAILURE;
    }

    /* Close + reopen the PCM on every Start. Without this, OHOS playback is
     * silent on MT6789 even though hw_ptr advances, DAPM widgets are On,
     * mixer state is correct, and the codec backend reports "start". A fresh
     * snd_pcm_open (matching test_audio's lifecycle) is audible; keeping the
     * long-lived audio_host handle open across sessions is not. The root
     * cause lives in the MTK AFE / ASoC stream-session state machine and
     * isn't observable from userspace. Close+reopen here forces each Start
     * through the same "first open" codepath that we've verified works. The
     * SndElementWrite fd-leak fix in alsa_soundcard.c makes reopening cheap. */
    if (cardIns->pcmHandle != NULL) {
        snd_pcm_close(cardIns->pcmHandle);
        cardIns->pcmHandle = NULL;
        int32_t oret = snd_pcm_open(&cardIns->pcmHandle, cardIns->devName,
                                    SND_PCM_STREAM_PLAYBACK, 0);
        if (oret < 0) {
            AUDIO_FUNC_LOGE("snd_pcm_open re-open fail: %{public}s", snd_strerror(oret));
            return HDF_FAILURE;
        }
        (void)snd_pcm_nonblock(cardIns->pcmHandle, 0);
        /* Force hw_params/sw_params to be re-applied on next RenderRender by
         * clearing the adapter's params-cached flag (misnamed mmapFlag). */
        cardIns->mmapFlag = false;
    }
    return HDF_SUCCESS;
}

static int32_t RenderStopImpl(struct AlsaRender *renderIns)
{
    int32_t ret;
    struct AlsaMixerCtlElement elem;
    struct AlsaSoundCard *cardIns = (struct AlsaSoundCard *)renderIns;
    CHECK_NULL_PTR_RETURN_DEFAULT(renderIns);
    CHECK_NULL_PTR_RETURN_DEFAULT(cardIns);

    SndElementItemInit(&elem);
    elem.numid = SND_NUMID_EXT_SPK_AMP_SWITCH;
    elem.name = SND_ELEM_EXT_SPK_AMP_SWITCH;
    elem.value = SND_OUT_CARD_OFF;
    ret = SndElementWrite(cardIns, &elem);
    if (ret != HDF_SUCCESS) {
        AUDIO_FUNC_LOGE("disable Ext_Speaker_Amp Switch failed!");
    }

    if (renderIns->soundCard.pcmHandle != NULL) {
        snd_pcm_drain(renderIns->soundCard.pcmHandle);
    }
    return HDF_SUCCESS;
}

static int32_t RenderGetGainThresholdImpl(struct AlsaRender *renderIns, float *gainMin, float *gainMax)
{
    (void)renderIns;
    if (gainMin) *gainMin = 0.0f;
    if (gainMax) *gainMax = 0.0f;
    return HDF_SUCCESS;
}

static int32_t RenderGetGainImpl(struct AlsaRender *renderIns, float *volume)
{
    (void)renderIns;
    if (volume) *volume = 0.0f;
    return HDF_SUCCESS;
}

static int32_t RenderSetGainImpl(struct AlsaRender *renderIns, float volume)
{
    (void)renderIns;
    (void)volume;
    return HDF_SUCCESS;
}

static int32_t RenderGetChannelModeImpl(struct AlsaRender *renderIns, enum AudioChannelMode *mode)
{
    (void)renderIns;
    (void)mode;
    return HDF_SUCCESS;
}

static int32_t RenderSetChannelModeImpl(struct AlsaRender *renderIns, enum AudioChannelMode mode)
{
    (void)renderIns;
    (void)mode;
    return HDF_SUCCESS;
}

int32_t RenderOverrideFunc(struct AlsaRender *renderIns)
{
    struct AlsaSoundCard *cardIns = (struct AlsaSoundCard *)renderIns;

    if (cardIns->cardType == SND_CARD_PRIMARY) {
        renderIns->Init = RenderInitImpl;
        renderIns->SelectScene = RenderSelectSceneImpl;
        renderIns->Start = RenderStartImpl;
        renderIns->Stop = RenderStopImpl;
        renderIns->GetVolThreshold = RenderGetVolThresholdImpl;
        renderIns->GetVolume = RenderGetVolumeImpl;
        renderIns->SetVolume = RenderSetVolumeImpl;
        renderIns->GetGainThreshold = RenderGetGainThresholdImpl;
        renderIns->GetGain = RenderGetGainImpl;
        renderIns->SetGain = RenderSetGainImpl;
        renderIns->GetMute = RenderGetMuteImpl;
        renderIns->SetMute = RenderSetMuteImpl;
        renderIns->GetChannelMode = RenderGetChannelModeImpl;
        renderIns->SetChannelMode = RenderSetChannelModeImpl;
    }
    return HDF_SUCCESS;
}
