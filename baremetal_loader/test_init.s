.globl start
.globl _main
start:
_main:
    /* SYS_write(1, msg, len) */
    adr x1, msg
    mov x0, #1
    mov x2, #60
    mov x16, #4
    svc #0x80

    /* Spin safely */
1:  wfe
    b 1b

msg:
    .ascii "\n\n*** HELLO FROM REAL LAUNCHD USERSPACE ON PIXEL FOLD! ***\n\n\0"
