/*
 * Copyright (c) 2026 Eclipse Oniro for OpenHarmony contributors.
 * SPDX-License-Identifier: Apache-2.0
 *
 * test_audio_hal — exercise the Android (Halium) legacy audio HAL from an
 * OHOS process through libhybris.
 *
 * This is the phase-A0 go/no-go harness of
 * device/board/oniro/docs/hybris_generic/audio_hal_switch_plan.md: it proves
 * (or disproves) that /android/vendor/lib64/hw/audio.primary.<soc>.so can be
 * loaded and driven in-process before any of the OHOS audio HDI stack is
 * touched.  Seeded from third_party/libhybris/hybris/tests/test_audio.c and
 * extended with playback, capture + level measurement, parameter and routing
 * pokes.  Deliberately named *_hal so it does not collide with the direct-ALSA
 * `test_audio` from ../../audio_alsa/test_audio/.
 *
 * Run it with the Android library search paths exported, e.g.
 *   LD_LIBRARY_PATH=/system/lib64/libhybris:/system/lib64 \
 *   HYBRIS_LD_LIBRARY_PATH=/android/vendor/lib64:/android/vendor/lib64/hw:\
 * /android/system/lib64:/apex/com.android.vndk.v34/lib64 \
 *   test_audio_hal --info
 *
 * No assert() anywhere: NDEBUG is on in release builds and would silently drop
 * the side effects (a trap the display bring-up already paid for).
 */

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <hardware/audio.h>
#include <hardware/hardware.h>

/* Some MTK vendors register the module under "libaudio" instead of "audio". */
#define AUDIO_HARDWARE_MODULE_ID_ALT "libaudio"

#define DEFAULT_OUT_RATE     48000
#define DEFAULT_IN_RATE      48000
#define DEFAULT_TONE_HZ      440
#define DEFAULT_SECONDS      4
#define FALLBACK_CHUNK_BYTES 4096

static struct audio_hw_device *g_adev;
static struct hw_module_t *g_hwmod;

/* ------------------------------------------------------------------ utils */

static const char *fmt_name(audio_format_t f)
{
    switch (f) {
        case AUDIO_FORMAT_PCM_16_BIT: return "PCM_16_BIT";
        case AUDIO_FORMAT_PCM_8_BIT: return "PCM_8_BIT";
        case AUDIO_FORMAT_PCM_32_BIT: return "PCM_32_BIT";
        case AUDIO_FORMAT_PCM_8_24_BIT: return "PCM_8_24_BIT";
        case AUDIO_FORMAT_PCM_24_BIT_PACKED: return "PCM_24_BIT_PACKED";
        case AUDIO_FORMAT_PCM_FLOAT: return "PCM_FLOAT";
        default: return "other";
    }
}

static int bytes_per_sample(audio_format_t f)
{
    switch (f) {
        case AUDIO_FORMAT_PCM_8_BIT: return 1;
        case AUDIO_FORMAT_PCM_16_BIT: return 2;
        case AUDIO_FORMAT_PCM_24_BIT_PACKED: return 3;
        case AUDIO_FORMAT_PCM_32_BIT:
        case AUDIO_FORMAT_PCM_8_24_BIT:
        case AUDIO_FORMAT_PCM_FLOAT: return 4;
        default: return 2;
    }
}

static unsigned count_bits(uint32_t v)
{
    unsigned n = 0;
    while (v) {
        n += v & 1u;
        v >>= 1;
    }
    return n;
}

/* ------------------------------------------------------------- HAL loading */

static int open_hal(void)
{
    int ret;

    hw_get_module_by_class(AUDIO_HARDWARE_MODULE_ID, AUDIO_HARDWARE_MODULE_ID_PRIMARY,
                           (const hw_module_t **)&g_hwmod);
    if (g_hwmod == NULL) {
        fprintf(stderr, "hw_get_module_by_class(%s, %s) failed, trying %s\n",
                AUDIO_HARDWARE_MODULE_ID, AUDIO_HARDWARE_MODULE_ID_PRIMARY,
                AUDIO_HARDWARE_MODULE_ID_ALT);
        hw_get_module_by_class(AUDIO_HARDWARE_MODULE_ID_ALT, AUDIO_HARDWARE_MODULE_ID_PRIMARY,
                               (const hw_module_t **)&g_hwmod);
    }
    if (g_hwmod == NULL) {
        fprintf(stderr, "FATAL: no audio primary HAL module found\n");
        return -1;
    }

    fprintf(stdout, "module: id=%s name=%s author=%s module_api=0x%x hal_api=0x%x\n",
            g_hwmod->id ? g_hwmod->id : "(null)",
            g_hwmod->name ? g_hwmod->name : "(null)",
            g_hwmod->author ? g_hwmod->author : "(null)",
            g_hwmod->module_api_version, g_hwmod->hal_api_version);

    ret = audio_hw_device_open(g_hwmod, &g_adev);
    if (ret != 0 || g_adev == NULL) {
        fprintf(stderr, "FATAL: audio_hw_device_open failed: %d (%s)\n", ret, strerror(-ret));
        return -1;
    }

    fprintf(stdout, "device api version: 0x%04x (current 0x%04x, min 0x%04x)\n",
            g_adev->common.version, AUDIO_DEVICE_API_VERSION_CURRENT,
            AUDIO_DEVICE_API_VERSION_MIN);
    if (g_adev->common.version < AUDIO_DEVICE_API_VERSION_MIN) {
        fprintf(stderr, "WARNING: device api below minimum, calls may be unsafe\n");
    }

    if (g_adev->init_check != NULL) {
        ret = g_adev->init_check(g_adev);
        fprintf(stdout, "init_check: %d%s\n", ret, ret == 0 ? " (OK)" : " (FAILED)");
        if (ret != 0) {
            return -1;
        }
    }
    return 0;
}

static void close_hal(void)
{
    if (g_adev != NULL) {
        audio_hw_device_close(g_adev);
        g_adev = NULL;
    }
}

/* ------------------------------------------------------------------ --info */

static void print_dev_param(const char *key)
{
    char *v;

    if (g_adev->get_parameters == NULL) {
        return;
    }
    v = g_adev->get_parameters(g_adev, key);
    fprintf(stdout, "  get_parameters(\"%s\") = %s\n", key, v ? v : "(null)");
    free(v);
}

static int cmd_info(void)
{
    float volume;
    bool mute;

    if (g_adev->get_master_volume != NULL && g_adev->get_master_volume(g_adev, &volume) == 0) {
        fprintf(stdout, "master volume: %f\n", volume);
    }
    if (g_adev->get_master_mute != NULL && g_adev->get_master_mute(g_adev, &mute) == 0) {
        fprintf(stdout, "master mute: %d\n", (int)mute);
    }
    if (g_adev->get_mic_mute != NULL && g_adev->get_mic_mute(g_adev, &mute) == 0) {
        fprintf(stdout, "mic mute: %d\n", (int)mute);
    }

    print_dev_param("routing");
    print_dev_param("audio_mode");
    print_dev_param("connect");
    print_dev_param("SND_CARD_NAME");
    print_dev_param("AUDIO_HAL_VERSION");

    if (g_adev->get_supported_devices != NULL) {
        fprintf(stdout, "supported devices: 0x%x\n", g_adev->get_supported_devices(g_adev));
    }

    if (g_adev->get_microphones != NULL) {
        size_t count = 0;
        int ret = g_adev->get_microphones(g_adev, NULL, &count);
        fprintf(stdout, "get_microphones: ret=%d count=%zu\n", ret, count);
    }

    if (g_adev->dump != NULL) {
        fflush(stdout);
        fprintf(stdout, "---- adev->dump ----\n");
        fflush(stdout);
        g_adev->dump(g_adev, STDOUT_FILENO);
        fprintf(stdout, "---- end dump ----\n");
    }
    return 0;
}

/* ------------------------------------------------------------------ --play */

static int cmd_play(int seconds, int tone_hz, uint32_t out_device, uint32_t flags)
{
    struct audio_config config = {
        .sample_rate = DEFAULT_OUT_RATE,
        .channel_mask = AUDIO_CHANNEL_OUT_STEREO,
        .format = AUDIO_FORMAT_PCM_16_BIT,
    };
    struct audio_stream_out *out = NULL;
    size_t buf_bytes, frame_bytes, frames_per_buf, total_frames, done_frames;
    unsigned chans, bps;
    int16_t *buf;
    double phase = 0.0, step;
    int ret;
    char kv[64];

    ret = g_adev->open_output_stream(g_adev, 0 /* handle */, out_device,
                                     (audio_output_flags_t)flags, &config, &out, "");
    if (ret != 0 || out == NULL) {
        fprintf(stderr, "FATAL: open_output_stream failed: %d (%s)\n", ret, strerror(-ret));
        return -1;
    }

    /* The HAL is free to mutate config; report what we actually got. */
    config.sample_rate = out->common.get_sample_rate(&out->common);
    config.channel_mask = out->common.get_channels(&out->common);
    config.format = out->common.get_format(&out->common);
    chans = count_bits(config.channel_mask);
    bps = bytes_per_sample(config.format);
    frame_bytes = (size_t)chans * bps;
    buf_bytes = out->common.get_buffer_size(&out->common);
    if (buf_bytes == 0 || buf_bytes > (1u << 20)) {
        buf_bytes = FALLBACK_CHUNK_BYTES;
    }
    if (frame_bytes == 0) {
        fprintf(stderr, "FATAL: bogus frame size (chans=%u bps=%u)\n", chans, bps);
        goto out_close;
    }
    frames_per_buf = buf_bytes / frame_bytes;

    fprintf(stdout, "output stream: rate=%u channels=%u(mask 0x%x) format=%s buffer=%zuB (%zu frames) latency=%ums\n",
            config.sample_rate, chans, config.channel_mask, fmt_name(config.format),
            buf_bytes, frames_per_buf,
            out->get_latency ? out->get_latency(out) : 0);

    if (config.format != AUDIO_FORMAT_PCM_16_BIT) {
        fprintf(stderr, "FATAL: only PCM_16_BIT tone generation implemented\n");
        goto out_close;
    }

    /* Tell the HAL where to route.  AudioFlinger does this on patch apply. */
    if (out->common.set_parameters != NULL) {
        snprintf(kv, sizeof(kv), "routing=%u", out_device);
        ret = out->common.set_parameters(&out->common, kv);
        fprintf(stdout, "stream set_parameters(\"%s\") = %d\n", kv, ret);
    }
    if (out->set_volume != NULL) {
        ret = out->set_volume(out, 1.0f, 1.0f);
        fprintf(stdout, "set_volume(1.0, 1.0) = %d\n", ret);
    }

    buf = (int16_t *)malloc(frames_per_buf * frame_bytes);
    if (buf == NULL) {
        fprintf(stderr, "FATAL: out of memory\n");
        goto out_close;
    }

    step = 2.0 * M_PI * (double)tone_hz / (double)config.sample_rate;
    total_frames = (size_t)seconds * config.sample_rate;
    done_frames = 0;

    fprintf(stdout, "playing %d Hz for %d s on device 0x%x ...\n", tone_hz, seconds, out_device);
    while (done_frames < total_frames) {
        size_t n = frames_per_buf;
        size_t i, c;
        ssize_t written;

        if (n > total_frames - done_frames) {
            n = total_frames - done_frames;
        }
        for (i = 0; i < n; i++) {
            int16_t s = (int16_t)(16384.0 * sin(phase));
            phase += step;
            if (phase > 2.0 * M_PI) {
                phase -= 2.0 * M_PI;
            }
            for (c = 0; c < chans; c++) {
                buf[i * chans + c] = s;
            }
        }
        written = out->write(out, buf, n * frame_bytes);
        if (written < 0) {
            fprintf(stderr, "write failed: %zd (%s)\n", written, strerror((int)-written));
            break;
        }
        if ((size_t)written != n * frame_bytes) {
            fprintf(stdout, "short write: %zd of %zu\n", written, n * frame_bytes);
        }
        done_frames += n;
    }

    if (out->get_presentation_position != NULL) {
        uint64_t frames = 0;
        struct timespec ts = {0, 0};
        ret = out->get_presentation_position(out, &frames, &ts);
        fprintf(stdout, "get_presentation_position: ret=%d frames=%llu ts=%lld.%09ld\n",
                ret, (unsigned long long)frames, (long long)ts.tv_sec, ts.tv_nsec);
    }
    if (out->get_render_position != NULL) {
        uint32_t dsp = 0;
        ret = out->get_render_position(out, &dsp);
        fprintf(stdout, "get_render_position: ret=%d dsp_frames=%u\n", ret, dsp);
    }

    fprintf(stdout, "wrote %zu frames, entering standby\n", done_frames);
    if (out->common.standby != NULL) {
        fprintf(stdout, "standby: %d\n", out->common.standby(&out->common));
    }
    free(buf);
    g_adev->close_output_stream(g_adev, out);
    return 0;

out_close:
    g_adev->close_output_stream(g_adev, out);
    return -1;
}

/* ------------------------------------------------------------------- --rec */

struct level_acc {
    double sum_sq;
    double sum_sq_hp;
    int32_t peak;
    uint64_t samples;
    /* one-pole high-pass state (fc ~200 Hz) for the LF-dominance estimate */
    double hp_a;
    double hp_prev_in;
    double hp_prev_out;
};

static void level_init(struct level_acc *a, uint32_t rate)
{
    double rc = 1.0 / (2.0 * M_PI * 200.0);
    double dt = 1.0 / (double)rate;

    memset(a, 0, sizeof(*a));
    a->hp_a = rc / (rc + dt);
}

static void level_feed(struct level_acc *a, const int16_t *s, size_t n)
{
    size_t i;

    for (i = 0; i < n; i++) {
        double x = (double)s[i];
        double y = a->hp_a * (a->hp_prev_out + x - a->hp_prev_in);

        a->hp_prev_in = x;
        a->hp_prev_out = y;
        a->sum_sq += x * x;
        a->sum_sq_hp += y * y;
        if (s[i] > a->peak) {
            a->peak = s[i];
        }
        if (-(int32_t)s[i] > a->peak) {
            a->peak = -(int32_t)s[i];
        }
        a->samples++;
    }
}

static double dbfs(double lin)
{
    if (lin <= 0.0) {
        return -999.0;
    }
    return 20.0 * log10(lin / 32768.0);
}

static void level_report(const struct level_acc *a, const char *tag)
{
    double rms, rms_hp;

    if (a->samples == 0) {
        fprintf(stdout, "%s: no samples\n", tag);
        return;
    }
    rms = sqrt(a->sum_sq / (double)a->samples);
    rms_hp = sqrt(a->sum_sq_hp / (double)a->samples);
    fprintf(stdout,
            "%s: samples=%llu peak=%d (%.1f dBFS) rms=%.1f (%.1f dBFS) rms>200Hz=%.1f (%.1f dBFS) LF_share=%.0f%%\n",
            tag, (unsigned long long)a->samples, a->peak, dbfs((double)a->peak),
            rms, dbfs(rms), rms_hp, dbfs(rms_hp),
            rms > 0.0 ? 100.0 * (1.0 - (rms_hp * rms_hp) / (rms * rms)) : 0.0);
}

static int cmd_rec(const char *path, int seconds, audio_source_t source, uint32_t in_device,
                   uint32_t rate, uint32_t channel_mask)
{
    struct audio_config config = {
        .sample_rate = rate,
        .channel_mask = channel_mask,
        .format = AUDIO_FORMAT_PCM_16_BIT,
    };
    struct audio_stream_in *in = NULL;
    struct level_acc total, sec;
    size_t buf_bytes, frame_bytes, total_frames, done_frames, next_mark;
    unsigned chans, bps;
    int16_t *buf;
    int fd = -1;
    int ret;
    char kv[64];

    ret = g_adev->open_input_stream(g_adev, 0 /* handle */, in_device, &config, &in,
                                    AUDIO_INPUT_FLAG_NONE, "", source);
    if (ret != 0 || in == NULL) {
        fprintf(stderr, "FATAL: open_input_stream failed: %d (%s)\n", ret, strerror(-ret));
        return -1;
    }

    config.sample_rate = in->common.get_sample_rate(&in->common);
    config.channel_mask = in->common.get_channels(&in->common);
    config.format = in->common.get_format(&in->common);
    chans = count_bits(config.channel_mask);
    bps = bytes_per_sample(config.format);
    frame_bytes = (size_t)chans * bps;
    buf_bytes = in->common.get_buffer_size(&in->common);
    if (buf_bytes == 0 || buf_bytes > (1u << 20)) {
        buf_bytes = FALLBACK_CHUNK_BYTES;
    }
    if (frame_bytes == 0) {
        fprintf(stderr, "FATAL: bogus frame size (chans=%u bps=%u)\n", chans, bps);
        goto out_close;
    }

    fprintf(stdout, "input stream: rate=%u channels=%u(mask 0x%x) format=%s buffer=%zuB source=%d device=0x%x\n",
            config.sample_rate, chans, config.channel_mask, fmt_name(config.format),
            buf_bytes, (int)source, in_device);

    if (config.format != AUDIO_FORMAT_PCM_16_BIT) {
        fprintf(stderr, "FATAL: only PCM_16_BIT level analysis implemented\n");
        goto out_close;
    }

    if (in->common.set_parameters != NULL) {
        snprintf(kv, sizeof(kv), "routing=%u", in_device);
        ret = in->common.set_parameters(&in->common, kv);
        fprintf(stdout, "stream set_parameters(\"%s\") = %d\n", kv, ret);
    }
    if (in->set_gain != NULL) {
        ret = in->set_gain(in, 1.0f);
        fprintf(stdout, "set_gain(1.0) = %d\n", ret);
    }

    buf = (int16_t *)malloc(buf_bytes);
    if (buf == NULL) {
        fprintf(stderr, "FATAL: out of memory\n");
        goto out_close;
    }

    if (path != NULL && strcmp(path, "-") != 0) {
        fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) {
            fprintf(stderr, "WARNING: cannot open %s: %s\n", path, strerror(errno));
        }
    }

    level_init(&total, config.sample_rate);
    level_init(&sec, config.sample_rate);
    total_frames = (size_t)seconds * config.sample_rate;
    done_frames = 0;
    next_mark = config.sample_rate;

    fprintf(stdout, "recording %d s ...\n", seconds);
    while (done_frames < total_frames) {
        ssize_t got = in->read(in, buf, buf_bytes);
        size_t got_frames;

        if (got < 0) {
            fprintf(stderr, "read failed: %zd (%s)\n", got, strerror((int)-got));
            break;
        }
        if (got == 0) {
            fprintf(stderr, "read returned 0, aborting\n");
            break;
        }
        got_frames = (size_t)got / frame_bytes;
        level_feed(&total, buf, (size_t)got / sizeof(int16_t));
        level_feed(&sec, buf, (size_t)got / sizeof(int16_t));
        if (fd >= 0 && write(fd, buf, (size_t)got) < 0) {
            fprintf(stderr, "WARNING: file write failed: %s\n", strerror(errno));
        }
        done_frames += got_frames;
        if (done_frames >= next_mark) {
            char tag[32];
            snprintf(tag, sizeof(tag), "  t=%zus", next_mark / config.sample_rate);
            level_report(&sec, tag);
            level_init(&sec, config.sample_rate);
            next_mark += config.sample_rate;
        }
    }

    level_report(&total, "TOTAL");
    if (in->get_input_frames_lost != NULL) {
        fprintf(stdout, "frames lost: %u\n", in->get_input_frames_lost(in));
    }
    if (fd >= 0) {
        close(fd);
        fprintf(stdout, "raw s16le %uch @%u Hz written to %s\n", chans, config.sample_rate, path);
    }
    if (in->common.standby != NULL) {
        fprintf(stdout, "standby: %d\n", in->common.standby(&in->common));
    }
    free(buf);
    g_adev->close_input_stream(g_adev, in);
    return 0;

out_close:
    g_adev->close_input_stream(g_adev, in);
    return -1;
}

/* ------------------------------------------------------------------- main */

static void usage(const char *argv0)
{
    fprintf(stdout,
        "usage: %s <command> [options]\n"
        "  --info                                     open the HAL and dump its state\n"
        "  --play [sec] [hz] [outdev] [flags]         play a sine (default 4 440 0x2 0x1)\n"
        "  --rec <file|-> [sec] [source] [indev] [rate] [chmask]\n"
        "                                             record and report levels\n"
        "                                             (default 4 1 0x80000004 48000 0x10)\n"
        "  --params <k=v[;k=v]>                       adev set_parameters\n"
        "  --getparams <key>                          adev get_parameters\n"
        "  --mode <n>                                 adev set_mode before the command\n"
        "  --mic-mute <0|1>                           adev set_mic_mute\n"
        "\n"
        "  outdev:  0x2 speaker, 0x1 earpiece, 0x4 wired headset, 0x8 wired headphone\n"
        "  indev:   0x80000004 builtin mic, 0x80000010 wired headset mic\n"
        "  source:  1 MIC, 5 CAMCORDER, 6 VOICE_RECOGNITION, 7 VOICE_COMMUNICATION, 9 UNPROCESSED\n"
        "  flags:   0x1 PRIMARY, 0x2 DEEP_BUFFER, 0x1000 RAW, 0x10000 MMAP_NOIRQ\n",
        argv0);
}

int main(int argc, char **argv)
{
    int i;
    int mode = -1;
    int mic_mute = -1;
    int rc;

    if (argc < 2) {
        usage(argv[0]);
        return 1;
    }

    /* Pre-scan for modifiers that must be applied right after adev open. */
    for (i = 1; i < argc - 1; i++) {
        if (strcmp(argv[i], "--mode") == 0) {
            mode = atoi(argv[i + 1]);
        } else if (strcmp(argv[i], "--mic-mute") == 0) {
            mic_mute = atoi(argv[i + 1]);
        }
    }

    if (open_hal() != 0) {
        return 2;
    }

    if (mode >= 0 && g_adev->set_mode != NULL) {
        fprintf(stdout, "set_mode(%d) = %d\n", mode, g_adev->set_mode(g_adev, (audio_mode_t)mode));
    }
    if (mic_mute >= 0 && g_adev->set_mic_mute != NULL) {
        fprintf(stdout, "set_mic_mute(%d) = %d\n", mic_mute,
                g_adev->set_mic_mute(g_adev, mic_mute != 0));
    }

    rc = 0;
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--info") == 0) {
            rc = cmd_info();
        } else if (strcmp(argv[i], "--play") == 0) {
            int sec = (i + 1 < argc && argv[i + 1][0] != '-') ? atoi(argv[++i]) : DEFAULT_SECONDS;
            int hz = (i + 1 < argc && argv[i + 1][0] != '-') ? atoi(argv[++i]) : DEFAULT_TONE_HZ;
            uint32_t dev = (i + 1 < argc && argv[i + 1][0] != '-')
                           ? (uint32_t)strtoul(argv[++i], NULL, 0) : AUDIO_DEVICE_OUT_SPEAKER;
            uint32_t flags = (i + 1 < argc && argv[i + 1][0] != '-')
                             ? (uint32_t)strtoul(argv[++i], NULL, 0) : AUDIO_OUTPUT_FLAG_PRIMARY;
            rc = cmd_play(sec, hz, dev, flags);
        } else if (strcmp(argv[i], "--rec") == 0) {
            const char *path = (i + 1 < argc) ? argv[++i] : "-";
            int sec = (i + 1 < argc && argv[i + 1][0] != '-') ? atoi(argv[++i]) : DEFAULT_SECONDS;
            audio_source_t src = (i + 1 < argc && argv[i + 1][0] != '-')
                                 ? (audio_source_t)atoi(argv[++i]) : AUDIO_SOURCE_MIC;
            uint32_t dev = (i + 1 < argc && argv[i + 1][0] != '-')
                           ? (uint32_t)strtoul(argv[++i], NULL, 0) : AUDIO_DEVICE_IN_BUILTIN_MIC;
            uint32_t rate = (i + 1 < argc && argv[i + 1][0] != '-')
                            ? (uint32_t)strtoul(argv[++i], NULL, 0) : DEFAULT_IN_RATE;
            uint32_t mask = (i + 1 < argc && argv[i + 1][0] != '-')
                            ? (uint32_t)strtoul(argv[++i], NULL, 0) : AUDIO_CHANNEL_IN_MONO;
            rc = cmd_rec(path, sec, src, dev, rate, mask);
        } else if (strcmp(argv[i], "--params") == 0 && i + 1 < argc) {
            const char *kv = argv[++i];
            fprintf(stdout, "set_parameters(\"%s\") = %d\n", kv,
                    g_adev->set_parameters ? g_adev->set_parameters(g_adev, kv) : -ENOSYS);
        } else if (strcmp(argv[i], "--getparams") == 0 && i + 1 < argc) {
            print_dev_param(argv[++i]);
        } else if (strcmp(argv[i], "--mode") == 0 || strcmp(argv[i], "--mic-mute") == 0) {
            i++; /* already handled */
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            usage(argv[0]);
        } else {
            fprintf(stderr, "unknown argument: %s\n", argv[i]);
            usage(argv[0]);
            rc = -1;
        }
        if (rc != 0) {
            break;
        }
    }

    close_hal();
    fprintf(stdout, "done (rc=%d)\n", rc);
    return rc == 0 ? 0 : 3;
}
