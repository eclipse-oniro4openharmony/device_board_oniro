/*
 * Copyright (c) 2026 Eclipse Oniro for OpenHarmony contributors.
 * SPDX-License-Identifier: Apache-2.0
 */

/* init-strace-wrap — bound over /system/bin/init via androidd's halium-debug
 * overlay.  Execs Halium's own /system/bin/strace on a copy of the real init
 * (/data/halium-debug/init.real), forwarding the original envp (androidd
 * seeds androidboot.* there).  Freestanding static aarch64, no libc. */

static inline long sys3(long nr, long a, long b, long c)
{
    register long x8 __asm__("x8") = nr;
    register long x0 __asm__("x0") = a;
    register long x1 __asm__("x1") = b;
    register long x2 __asm__("x2") = c;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2)
                     : "memory");
    return x0;
}

__asm__(".globl _start\n"
        "_start:\n"
        "  mov x0, sp\n"
        "  bl  c_start\n"
        "  brk #0\n");

void c_start(long *sp)
{
    long argc = sp[0];
    char **argv = (char **)(sp + 1);
    char **envp = argv + argc + 1;
    char *nargv[] = {
        (char *)"strace", (char *)"-f", (char *)"-tt", (char *)"-s",
        (char *)"256",    (char *)"-o",
        (char *)"/data/halium-debug/strace.log",
        (char *)"/data/halium-debug/init.real", (char *)"second_stage", 0
    };
    sys3(221, (long)"/system/bin/strace", (long)nargv, (long)envp);
    /* exec failed — leave a note in kmsg (fd 2 is /dev/kmsg via androidd) */
    static const char msg[] = "init-strace-wrap: exec strace failed\n";
    sys3(64, 2, (long)msg, sizeof msg - 1);
    /* fall back to the real init so the NS still comes up */
    char *rargv[] = { (char *)"init", (char *)"second_stage", 0 };
    sys3(221, (long)"/data/halium-debug/init.real", (long)rargv, (long)envp);
    sys3(93, 127, 0, 0);
}
