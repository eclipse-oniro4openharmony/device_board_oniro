/*
 * Copyright (C) 2026 Oniro / Hybris Generic.
 * Licensed under the Apache License, Version 2.0 (the "License").
 *
 * tcprelay — forward 127.0.0.1:<lport> (host netns) to <dst>:<dport>.
 *
 * The Waydroid container (W4, android_app_compat_plan.md) lives in its
 * own netns; its adbd listens on the veth address (192.168.240.2:5555).
 * `hdc fport` can only reach device-loopback, so this relay bridges
 * loopback → veth for the workstation adb/scrcpy rig:
 *
 *   tcprelay 15555 192.168.240.2 5555 &
 *   (workstation)  hdc fport tcp:15555 tcp:15555
 *                  adb connect 127.0.0.1:15555
 *
 * Build (same standalone recipe as waydroidd):
 *   $SDK/llvm/bin/clang --target=aarch64-linux-ohos -O2 -static \
 *       -o tcprelay tcprelay.c
 */

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static void forward(int a, int b)
{
    struct pollfd pfd[2] = { { a, POLLIN, 0 }, { b, POLLIN, 0 } };
    char buf[65536];
    int open_dirs = 2;

    while (open_dirs > 0) {
        if (poll(pfd, 2, -1) < 0) {
            if (errno == EINTR)
                continue;
            return;
        }
        for (int i = 0; i < 2; i++) {
            if (!(pfd[i].revents & (POLLIN | POLLHUP | POLLERR)))
                continue;
            ssize_t n = read(pfd[i].fd, buf, sizeof(buf));
            if (n <= 0) {
                shutdown(pfd[i ^ 1].fd, SHUT_WR);
                pfd[i].events = 0;
                pfd[i].fd = -pfd[i].fd - 1;   /* ignore from now on */
                open_dirs--;
                continue;
            }
            for (ssize_t off = 0; off < n; ) {
                ssize_t w = write(pfd[i ^ 1].fd, buf + off, n - off);
                if (w <= 0)
                    return;
                off += w;
            }
        }
    }
}

int main(int argc, char **argv)
{
    if (argc != 4) {
        fprintf(stderr, "usage: %s <listen-port> <dst-ip> <dst-port>\n",
                argv[0]);
        return 2;
    }

    signal(SIGCHLD, SIG_IGN);
    signal(SIGPIPE, SIG_IGN);

    int ls = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in la = { 0 }, da = { 0 };
    la.sin_family = AF_INET;
    la.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    la.sin_port = htons((unsigned short)atoi(argv[1]));
    da.sin_family = AF_INET;
    da.sin_port = htons((unsigned short)atoi(argv[3]));
    if (inet_pton(AF_INET, argv[2], &da.sin_addr) != 1) {
        fprintf(stderr, "bad dst ip %s\n", argv[2]);
        return 2;
    }

    if (bind(ls, (struct sockaddr *)&la, sizeof(la)) < 0 ||
        listen(ls, 8) < 0) {
        perror("bind/listen");
        return 1;
    }

    for (;;) {
        int c = accept(ls, NULL, NULL);
        if (c < 0)
            continue;
        pid_t pid = fork();
        if (pid == 0) {
            close(ls);
            int d = socket(AF_INET, SOCK_STREAM, 0);
            if (connect(d, (struct sockaddr *)&da, sizeof(da)) == 0) {
                setsockopt(c, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
                setsockopt(d, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
                forward(c, d);
            }
            _exit(0);
        }
        close(c);
    }
}
