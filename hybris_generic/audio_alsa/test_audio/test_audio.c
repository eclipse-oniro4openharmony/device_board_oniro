/*
 * Copyright (c) 2026 Oniro / Hybris Generic.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 *
 * test_audio — Phase 13B bisection tool.
 *
 * Exercises the OHOS-built /vendor/lib64/libasound.so directly against
 * /dev/snd/pcmC0D0p without audio_host / audio_server / HDF in the picture.
 *
 * What it does:
 *   1. Opens control hw:0 and programs the MT6789 DAPM route (same kcontrols
 *      as vendor_render.c::RenderInitImpl): ADDA_DL_CH{1,2} DL1_CH{1,2} on,
 *      HPL/HPR Mux = "Audio Playback", Ext_Speaker_Amp Switch = on, with the
 *      off→on toggle so the DAPM power sequencer fires.
 *   2. Sets Lineout Volume to 12 (matches SND_OUT_PLAYBACK_DEFAULT_VOLUME).
 *   3. Opens hw:0,0 playback, 44100/S16_LE/stereo, blocking mode,
 *      period_time=125ms, buffer_time=500ms, start_threshold=period_size.
 *   4. Writes a 440 Hz sine tone for ~3 s.
 *
 * Usage (on device, with audio_host stopped so the PCM is free):
 *     hdc shell "kill $(pidof audio_host) 2>/dev/null; sleep 1; \
 *                LD_LIBRARY_PATH=/vendor/lib64:/system/lib64 \
 *                /system/bin/test_audio"
 *
 * Expected outcomes:
 *   - Audible tone -> OHOS libasound.so + kernel path are functional, and
 *     the silence bug lives in the framework feed path (audio_server /
 *     audio_render_adapter / HiPlayer pacing).
 *   - Silent run   -> OHOS libasound.so itself is broken (musl build option
 *     divergence, mmap plugin load failure, etc.). Compare strace against
 *     a working host `aplay` run.
 */

#include "asoundlib.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define CARD        "hw:0"
#define PCM_DEVICE  "hw:0,0"
#define RATE        44100
#define CHANNELS    2
#define TONE_HZ     440
#define TONE_SEC    3
#define PERIOD_US   125000   /* 125 ms */
#define BUFFER_US   500000   /* 500 ms */

static int write_integer_numid(snd_ctl_t *ctl, unsigned int numid, long value)
{
    snd_ctl_elem_id_t *id;
    snd_ctl_elem_info_t *info;
    snd_ctl_elem_value_t *val;
    int err, i, count;

    snd_ctl_elem_id_alloca(&id);
    snd_ctl_elem_info_alloca(&info);
    snd_ctl_elem_value_alloca(&val);

    snd_ctl_elem_id_set_numid(id, numid);
    snd_ctl_elem_info_set_id(info, id);
    if ((err = snd_ctl_elem_info(ctl, info)) < 0) {
        fprintf(stderr, "elem_info numid=%u: %s\n", numid, snd_strerror(err));
        return err;
    }
    count = snd_ctl_elem_info_get_count(info);
    snd_ctl_elem_value_set_id(val, id);
    for (i = 0; i < count; i++) {
        switch (snd_ctl_elem_info_get_type(info)) {
        case SND_CTL_ELEM_TYPE_BOOLEAN:
            snd_ctl_elem_value_set_boolean(val, i, value);
            break;
        case SND_CTL_ELEM_TYPE_INTEGER:
            snd_ctl_elem_value_set_integer(val, i, value);
            break;
        case SND_CTL_ELEM_TYPE_ENUMERATED:
            snd_ctl_elem_value_set_enumerated(val, i, (unsigned int)value);
            break;
        default:
            fprintf(stderr, "numid=%u: unsupported elem type %d\n",
                    numid, snd_ctl_elem_info_get_type(info));
            return -EINVAL;
        }
    }
    if ((err = snd_ctl_elem_write(ctl, val)) < 0) {
        fprintf(stderr, "elem_write numid=%u val=%ld: %s\n",
                numid, value, snd_strerror(err));
        return err;
    }
    fprintf(stdout, "  numid=%-4u <- %ld  (%s)\n", numid, value,
            snd_ctl_elem_info_get_name(info));
    return 0;
}

static int toggle_on(snd_ctl_t *ctl, unsigned int numid)
{
    int err;
    if ((err = write_integer_numid(ctl, numid, 0)) < 0) return err;
    return write_integer_numid(ctl, numid, 1);
}

static int setup_dapm(void)
{
    snd_ctl_t *ctl = NULL;
    int err;

    fprintf(stdout, "Opening control %s...\n", CARD);
    if ((err = snd_ctl_open(&ctl, CARD, 0)) < 0) {
        fprintf(stderr, "snd_ctl_open(%s): %s\n", CARD, snd_strerror(err));
        return err;
    }

    fprintf(stdout, "Programming DAPM route:\n");
    /* ADDA_DL_CH1 DL1_CH1 = on (numid 211) */
    write_integer_numid(ctl, 211, 1);
    /* ADDA_DL_CH2 DL1_CH2 = on (numid 226) */
    write_integer_numid(ctl, 226, 1);
    /* HPL Mux = "Audio Playback" = item 2 (numid 311) */
    write_integer_numid(ctl, 311, 2);
    /* HPR Mux = "Audio Playback" = item 2 (numid 312) */
    write_integer_numid(ctl, 312, 2);
    /* Ext_Speaker_Amp Switch: toggle off->on so the DAPM power sequencer fires */
    toggle_on(ctl, 305);
    /* Lineout Volume = 12 (numid 286) */
    write_integer_numid(ctl, 286, 12);
    /* Headset Volume = 12 (numid 285) */
    write_integer_numid(ctl, 285, 12);

    snd_ctl_close(ctl);
    return 0;
}

static int set_hwparams(snd_pcm_t *handle, snd_pcm_hw_params_t *hwparams)
{
    unsigned int rrate = RATE;
    unsigned int period_time = PERIOD_US;
    unsigned int buffer_time = BUFFER_US;
    int err, dir = 0;

    snd_pcm_hw_params_any(handle, hwparams);
    if ((err = snd_pcm_hw_params_set_access(handle, hwparams,
                    SND_PCM_ACCESS_RW_INTERLEAVED)) < 0) {
        fprintf(stderr, "set_access: %s\n", snd_strerror(err)); return err;
    }
    if ((err = snd_pcm_hw_params_set_format(handle, hwparams,
                    SND_PCM_FORMAT_S16_LE)) < 0) {
        fprintf(stderr, "set_format: %s\n", snd_strerror(err)); return err;
    }
    if ((err = snd_pcm_hw_params_set_channels(handle, hwparams, CHANNELS)) < 0) {
        fprintf(stderr, "set_channels: %s\n", snd_strerror(err)); return err;
    }
    if ((err = snd_pcm_hw_params_set_rate_near(handle, hwparams, &rrate, &dir)) < 0) {
        fprintf(stderr, "set_rate_near: %s\n", snd_strerror(err)); return err;
    }
    /* Order matters: period_time BEFORE buffer_time (matches aplay). */
    dir = 0;
    if ((err = snd_pcm_hw_params_set_period_time_near(handle, hwparams,
                    &period_time, &dir)) < 0) {
        fprintf(stderr, "set_period_time: %s\n", snd_strerror(err)); return err;
    }
    dir = 0;
    if ((err = snd_pcm_hw_params_set_buffer_time_near(handle, hwparams,
                    &buffer_time, &dir)) < 0) {
        fprintf(stderr, "set_buffer_time: %s\n", snd_strerror(err)); return err;
    }
    if ((err = snd_pcm_hw_params(handle, hwparams)) < 0) {
        fprintf(stderr, "hw_params: %s\n", snd_strerror(err)); return err;
    }
    fprintf(stdout, "hw_params: rate=%u ch=%u period_us=%u buffer_us=%u\n",
            rrate, CHANNELS, period_time, buffer_time);
    return 0;
}

static int set_swparams(snd_pcm_t *handle,
                        snd_pcm_sframes_t period_size)
{
    snd_pcm_sw_params_t *sw;
    int err;
    snd_pcm_sw_params_alloca(&sw);
    if ((err = snd_pcm_sw_params_current(handle, sw)) < 0) {
        fprintf(stderr, "sw_params_current: %s\n", snd_strerror(err)); return err;
    }
    if ((err = snd_pcm_sw_params_set_start_threshold(handle, sw,
                    period_size)) < 0) {
        fprintf(stderr, "set_start_threshold: %s\n", snd_strerror(err)); return err;
    }
    if ((err = snd_pcm_sw_params_set_avail_min(handle, sw, period_size)) < 0) {
        fprintf(stderr, "set_avail_min: %s\n", snd_strerror(err)); return err;
    }
    if ((err = snd_pcm_sw_params(handle, sw)) < 0) {
        fprintf(stderr, "sw_params: %s\n", snd_strerror(err)); return err;
    }
    return 0;
}

int main(int argc, char **argv)
{
    snd_pcm_t *pcm = NULL;
    snd_pcm_hw_params_t *hw;
    snd_pcm_sframes_t period_size = 0, buffer_size = 0;
    int err, i;

    (void)argc; (void)argv;

    fprintf(stdout, "test_audio (phase13B libasound bisection)\n");

    if ((err = setup_dapm()) < 0) {
        fprintf(stderr, "DAPM setup failed, continuing anyway...\n");
    }

    fprintf(stdout, "Opening PCM %s (blocking)...\n", PCM_DEVICE);
    /* Blocking mode — NOT SND_PCM_NONBLOCK (see 13B fix note in phase13 doc). */
    if ((err = snd_pcm_open(&pcm, PCM_DEVICE, SND_PCM_STREAM_PLAYBACK, 0)) < 0) {
        fprintf(stderr, "snd_pcm_open(%s): %s\n", PCM_DEVICE, snd_strerror(err));
        return 1;
    }

    snd_pcm_hw_params_alloca(&hw);
    if (set_hwparams(pcm, hw) < 0) { snd_pcm_close(pcm); return 1; }

    snd_pcm_hw_params_get_period_size(hw, (snd_pcm_uframes_t *)&period_size, NULL);
    snd_pcm_hw_params_get_buffer_size(hw, (snd_pcm_uframes_t *)&buffer_size);
    fprintf(stdout, "period_size=%ld frames  buffer_size=%ld frames\n",
            (long)period_size, (long)buffer_size);

    if (set_swparams(pcm, period_size) < 0) { snd_pcm_close(pcm); return 1; }

    /* Build one period of a 440 Hz S16_LE stereo sine tone at ~50% amplitude. */
    size_t frames = (size_t)period_size;
    int16_t *buf = (int16_t *)malloc(frames * CHANNELS * sizeof(int16_t));
    if (!buf) { snd_pcm_close(pcm); return 1; }

    double phase = 0.0;
    const double step = 2.0 * M_PI * TONE_HZ / (double)RATE;
    const int16_t amp = 16000;

    int total_periods = (TONE_SEC * RATE) / (int)period_size;
    fprintf(stdout, "Writing %d periods (~%d s) of %d Hz sine...\n",
            total_periods, TONE_SEC, TONE_HZ);

    for (i = 0; i < total_periods; i++) {
        size_t f;
        for (f = 0; f < frames; f++) {
            int16_t s = (int16_t)(amp * sin(phase));
            buf[f * CHANNELS + 0] = s;
            buf[f * CHANNELS + 1] = s;
            phase += step;
            if (phase > 2.0 * M_PI) phase -= 2.0 * M_PI;
        }
        snd_pcm_sframes_t n = snd_pcm_writei(pcm, buf, frames);
        if (n < 0) {
            fprintf(stderr, "writei: %s (period %d)\n", snd_strerror((int)n), i);
            n = snd_pcm_recover(pcm, (int)n, 0);
            if (n < 0) {
                fprintf(stderr, "recover failed: %s\n", snd_strerror((int)n));
                break;
            }
            continue;
        }
        if ((i % 8) == 0) {
            snd_pcm_state_t st = snd_pcm_state(pcm);
            snd_pcm_sframes_t avail = snd_pcm_avail(pcm);
            fprintf(stdout, "  period %3d  wrote=%ld  state=%s  avail=%ld\n",
                    i, (long)n, snd_pcm_state_name(st), (long)avail);
        }
    }

    fprintf(stdout, "Draining...\n");
    snd_pcm_drain(pcm);
    snd_pcm_close(pcm);
    free(buf);
    fprintf(stdout, "done.\n");
    return 0;
}
