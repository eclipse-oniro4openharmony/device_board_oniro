/* reboot-bl — issue reboot(LINUX_REBOOT_CMD_RESTART2, "bootloader") via raw
 * syscalls, no libc.  busybox `reboot` can't pass the RESTART2 argument, but
 * the MT6878 syscon-reboot-mode driver needs it to write the LK fastboot
 * magic into the watchdog reg (0x10007024).  Freestanding: aarch64 only. */
static inline long sys4(long nr, long a, long b, long c, long d)
{
    register long x8 __asm__("x8") = nr;
    register long x0 __asm__("x0") = a;
    register long x1 __asm__("x1") = b;
    register long x2 __asm__("x2") = c;
    register long x3 __asm__("x3") = d;
    __asm__ volatile("svc #0"
                     : "+r"(x0)
                     : "r"(x8), "r"(x1), "r"(x2), "r"(x3)
                     : "memory");
    return x0;
}

void _start(void)
{
    sys4(81, 0, 0, 0, 0);                       /* sync */
    sys4(142, 0xfee1dead, 672274793, 0xa1b2c3d4, /* reboot RESTART2 */
         (long)"bootloader");
    sys4(93, 1, 0, 0, 0);                       /* exit(1) — reboot failed */
    for (;;)
        ;
}
