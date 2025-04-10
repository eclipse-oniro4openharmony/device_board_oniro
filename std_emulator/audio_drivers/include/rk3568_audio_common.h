/*
 * Copyright (C) 2022 HiHope Open Source Organization .
 *
 * HDF is dual licensed: you can use it either under the terms of
 * the GPL, or the BSD license, at your option.
 * See the LICENSE file in the root of this repository for complete details.
 */

#ifndef RK3568_AUDIO_COMMON_H
#define RK3568_AUDIO_COMMON_H

typedef enum {
    AUDIO_DEVICE_SAMPLE_RATE_8000   = 8000,    /* 8kHz sample_rate */
    AUDIO_DEVICE_SAMPLE_RATE_12000  = 12000,   /* 12kHz sample_rate */
    AUDIO_DEVICE_SAMPLE_RATE_11025  = 11025,   /* 11.025kHz sample_rate */
    AUDIO_DEVICE_SAMPLE_RATE_16000  = 16000,   /* 16kHz sample_rate */
    AUDIO_DEVICE_SAMPLE_RATE_22050  = 22050,   /* 22.050kHz sample_rate */
    AUDIO_DEVICE_SAMPLE_RATE_24000  = 24000,   /* 24kHz sample_rate */
    AUDIO_DEVICE_SAMPLE_RATE_32000  = 32000,   /* 32kHz sample_rate */
    AUDIO_DEVICE_SAMPLE_RATE_44100  = 44100,   /* 44.1kHz sample_rate */
    AUDIO_DEVICE_SAMPLE_RATE_48000  = 48000,   /* 48kHz sample_rate */
    AUDIO_DEVICE_SAMPLE_RATE_64000  = 64000,   /* 64kHz sample_rate */
    AUDIO_DEVICE_SAMPLE_RATE_88200  = 88200,   /* 88.2kHz sample_rate */
    AUDIO_DEVICE_SAMPLE_RATE_96000  = 96000,   /* 96kHz sample_rate */
    AUDIO_DEVICE_SAMPLE_RATE_176400  = 176400, /* 176.4kHz sample_rate */
    AUDIO_DEVICE_SAMPLE_RATE_192000  = 192000, /* 192kHz sample_rate */
    AUDIO_DEVICE_SAMPLE_RATE_BUTT,
} AudioDeviceSampleRate;


#endif // __RK3568_AUDIO_COMMON_H__
