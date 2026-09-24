/*
 * Copyright (c) 2026 Eclipse Oniro for OpenHarmony contributors.
 * SPDX-License-Identifier: Apache-2.0
 */

/* screencast_agent — uitest extension that serves the phone screen as an
 * MJPEG-over-HTTP stream, for mirroring the device onto a laptop/projector.
 *
 * uitest already ships a screen-copy engine (arkxtest addon/screen_copy.cpp:
 * DisplayManager::GetScreenshot polling, change detection, scale, libjpeg).
 * It is reachable only from an extension library that `uitest start-daemon
 * singleness` dlopen()s from /data/local/tmp/<name>; this is that library.
 *
 *   GET /        viewer page (black, fullscreen, keeps the aspect ratio)
 *   GET /stream  multipart/x-mixed-replace JPEG stream   [?scale=0.1..0.9]
 *
 * One stream client at a time; a new /stream request takes over.  Capture
 * runs only while a client is connected.  Uses nothing but libc, so it
 * builds with the plain SDK NDK (see README.md). */

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "extension_c_api.h"

using namespace OHOS::uitest;

namespace {
constexpr int LOG_INFO = 4;
constexpr int LOG_ERROR = 6;
constexpr int DEFAULT_PORT = 9000;
constexpr float DEFAULT_SCALE = 0.5f;
constexpr const char *CAPTURE = "copyScreen";
constexpr const char *BOUNDARY = "oniroframe";

LowLevelFunctions g_ll;
int g_listenPort = DEFAULT_PORT;
float g_defaultScale = DEFAULT_SCALE;
bool g_anyAddr = false;     /* default: loopback only, reached via hdc fport */

pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
int g_client = -1;          /* stream socket, owned under g_lock */
bool g_capturing = false;   /* touched by the accept thread only */
unsigned long g_frames = 0;

/* uitest's printLog lands in the LOG_APP hilog type, which hilog does not
 * show here, so log to a file next to the library instead. */
FILE *g_log = nullptr;

void Log(int level, const char *fmt, ...)
{
    if (g_log == nullptr) {
        return;
    }
    time_t now = time(nullptr);
    char ts[32];
    strftime(ts, sizeof(ts), "%m-%d %H:%M:%S", localtime(&now));
    fprintf(g_log, "%s %c ", ts, level >= LOG_ERROR ? 'E' : 'I');
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
}

bool SendAll(int fd, const void *buf, size_t len)
{
    auto p = static_cast<const char *>(buf);
    while (len > 0) {
        ssize_t n = send(fd, p, len, MSG_NOSIGNAL);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            return false;
        }
        p += n;
        len -= static_cast<size_t>(n);
    }
    return true;
}

bool SendStr(int fd, const char *s)
{
    return SendAll(fd, s, strlen(s));
}

/* Runs on uitest's encode thread; bytes are freed by uitest on return.  A
 * blocking send is fine: uitest keeps only the newest frame, so a slow
 * client just sees a lower frame rate, never a growing lag. */
void OnFrame(Text bytes)
{
    auto data = reinterpret_cast<const unsigned char *>(bytes.data);
    if (bytes.size < 2 || data[0] != 0xFF || data[1] != 0xD8) {
        Log(LOG_ERROR, "capture error: %.*s", static_cast<int>(bytes.size), bytes.data);
        return;
    }
    pthread_mutex_lock(&g_lock);
    int fd = g_client;
    if (fd >= 0) {
        char hdr[128];
        snprintf(hdr, sizeof(hdr), "Content-Type: image/jpeg\r\nContent-Length: %zu\r\n\r\n", bytes.size);
        /* The trailing boundary goes out right after the JPEG: browsers
         * render a part only once they see the next boundary. */
        char tail[64];
        snprintf(tail, sizeof(tail), "\r\n--%s\r\n", BOUNDARY);
        if (!SendStr(fd, hdr) || !SendAll(fd, bytes.data, bytes.size) || !SendStr(fd, tail)) {
            close(fd);
            g_client = -1;
        } else {
            g_frames++;
        }
    }
    pthread_mutex_unlock(&g_lock);
}

void StopCapture()
{
    if (g_capturing) {
        g_ll.stopCapture(Text{CAPTURE, strlen(CAPTURE)});
        g_capturing = false;
        Log(LOG_INFO, "capture stopped after %lu frames", g_frames);
    }
}

bool StartCapture(float scale)
{
    StopCapture(); /* restart so the new client gets a first frame at once */
    char opt[64];
    snprintf(opt, sizeof(opt), "{\"scale\":%.2f}", scale);
    g_frames = 0;
    if (g_ll.startCapture(Text{CAPTURE, strlen(CAPTURE)}, OnFrame, Text{opt, strlen(opt)}) != RETCODE_SUCCESS) {
        Log(LOG_ERROR, "startCapture(%s) failed", opt);
        return false;
    }
    g_capturing = true;
    Log(LOG_INFO, "capture started %s", opt);
    return true;
}

const char VIEWER[] =
    "<!doctype html><html><head><meta charset=utf-8><title>Phone screen</title>"
    "<meta name=viewport content=\"width=device-width,initial-scale=1\">"
    "<style>html,body{margin:0;height:100%;background:#000;overflow:hidden}"
    "img{display:block;width:100%;height:100%;object-fit:contain}</style></head>"
    "<body><img id=s alt=\"\"><script>"
    "const s=document.getElementById('s');"
    "function go(){s.src='/stream'+location.search+(location.search?'&':'?')+'t='+Date.now();}"
    "s.onerror=()=>setTimeout(go,1000);go();"
    "document.body.ondblclick=()=>document.documentElement.requestFullscreen();"
    "</script></body></html>";

float ParseScale(const char *req)
{
    const char *q = strstr(req, "scale=");
    if (q == nullptr) {
        return g_defaultScale;
    }
    float s = strtof(q + 6, nullptr);
    return (s > 0.05f && s < 1.0f) ? s : g_defaultScale;
}

void HandleConnection(int fd)
{
    char req[1024];
    ssize_t n = recv(fd, req, sizeof(req) - 1, 0);
    if (n <= 0) {
        close(fd);
        return;
    }
    req[n] = '\0';
    if (strncmp(req, "GET /stream", 11) == 0) {
        char hdr[256];
        snprintf(hdr, sizeof(hdr),
                 "HTTP/1.0 200 OK\r\nCache-Control: no-cache\r\nConnection: close\r\n"
                 "Content-Type: multipart/x-mixed-replace; boundary=%s\r\n\r\n--%s\r\n",
                 BOUNDARY, BOUNDARY);
        if (!SendStr(fd, hdr)) {
            close(fd);
            return;
        }
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        pthread_mutex_lock(&g_lock);
        int old = g_client;
        g_client = fd;
        pthread_mutex_unlock(&g_lock);
        if (old >= 0) {
            close(old);
        }
        Log(LOG_INFO, "stream client connected");
        StartCapture(ParseScale(req));
        return;
    }
    char hdr[160];
    snprintf(hdr, sizeof(hdr),
             "HTTP/1.0 200 OK\r\nContent-Type: text/html\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n",
             sizeof(VIEWER) - 1);
    SendStr(fd, hdr);
    SendAll(fd, VIEWER, sizeof(VIEWER) - 1);
    close(fd);
}
} // namespace

extern "C" RetCode UiTestExtension_OnInit(UiTestPort port, size_t argc, char **argv)
{
    g_log = fopen("/data/local/tmp/screencast.log", "w");
    if (port.initLowLevelFunctions(&g_ll) != RETCODE_SUCCESS) {
        return RETCODE_FAIL;
    }
    /* uitest start-daemon singleness [--extension-name X] [port] [scale] [wifi] */
    if (argc > 0 && atoi(argv[0]) > 0) {
        g_listenPort = atoi(argv[0]);
    }
    if (argc > 1) {
        float s = strtof(argv[1], nullptr);
        if (s > 0.05f && s < 1.0f) {
            g_defaultScale = s;
        }
    }
    g_anyAddr = argc > 2 && strcmp(argv[2], "wifi") == 0;
    return RETCODE_SUCCESS;
}

extern "C" RetCode UiTestExtension_OnRun()
{
    int srv = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in addr {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(g_listenPort));
    /* Loopback unless asked: on a shared (conference) network anyone could
     * otherwise watch the screen. */
    addr.sin_addr.s_addr = htonl(g_anyAddr ? INADDR_ANY : INADDR_LOOPBACK);
    if (srv < 0 || bind(srv, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0 || listen(srv, 4) != 0) {
        Log(LOG_ERROR, "cannot listen on port %d: %s", g_listenPort, strerror(errno));
        return RETCODE_FAIL;
    }
    Log(LOG_INFO, "listening on %s:%d, default scale %.2f", g_anyAddr ? "0.0.0.0" : "127.0.0.1", g_listenPort,
        g_defaultScale);
    for (;;) {
        pollfd pfd {srv, POLLIN, 0};
        int r = poll(&pfd, 1, 500);
        if (r > 0) {
            int fd = accept(srv, nullptr, nullptr);
            if (fd >= 0) {
                HandleConnection(fd);
            }
        }
        pthread_mutex_lock(&g_lock);
        bool idle = g_client < 0;
        pthread_mutex_unlock(&g_lock);
        if (idle) {
            StopCapture(); /* client went away: stop polling screenshots */
        }
    }
}
