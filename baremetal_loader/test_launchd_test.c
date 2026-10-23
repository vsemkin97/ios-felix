#define SYS_exit   1
#define SYS_read   3
#define SYS_write  4
#define SYS_open   5
#define SYS_close  6

void *memcpy(void *dst, const void *src, unsigned long n) {
    char *d = (char *)dst;
    const char *s = (const char *)src;
    while (n--) *d++ = *s++;
    return dst;
}

static long syscall3(long num, long a0, long a1, long a2) {
    register long x16 __asm__("x16") = num;
    register long x0 __asm__("x0") = a0;
    register long x1 __asm__("x1") = a1;
    register long x2 __asm__("x2") = a2;
    __asm__ volatile("svc #0x80" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x16) : "memory", "cc");
    return x0;
}

static void print_str(const char *str) {
    long len = 0;
    while (str[len]) len++;
    syscall3(SYS_write, 1, (long)str, len);
}

void _start(void) {
    /* Open /dev/console for stdin (0), stdout (1), stderr (2) */
    syscall3(SYS_open, (long)"/dev/console", 2, 0);
    syscall3(SYS_open, (long)"/dev/console", 2, 0);
    syscall3(SYS_open, (long)"/dev/console", 2, 0);

    print_str("\n\n======================================================\n");
    print_str("*** REAL USERLAND LAUNCHD RUNNING ON GOOGLE PIXEL FOLD! ***\n");
    print_str("======================================================\n");

    for (;;) {
        __asm__ volatile("wfe");
    }
}
