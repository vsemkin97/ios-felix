/*
 * Minimal text console on the ABL splash framebuffer. With the MMU off
 * every access is Device memory, so stay aligned (-mstrict-align).
 */
#include "console.h"
#include "font.h"

#define SCALE		3
#define CH_W		(8 * SCALE)
#define CH_H		(16 * SCALE)
#define MARGIN		24
#define LABEL_COLS	8

volatile uint32_t *con_fb = 0;
uint32_t con_w = FB_W;
uint32_t con_h = FB_H;
uint32_t con_stride = FB_STRIDE;
static uint32_t con_bg;
static int row;

static uint32_t ramoops_off = 0;
static int ramoops_inited = 0;

static void ramoops_init(void)
{
	ramoops_off = 0;
	ramoops_inited = 1;
}

static void bldr_putc(char c)
{
	volatile uint64_t *head_ptr = (volatile uint64_t *)0xfd800000UL;
	volatile uint64_t *size_ptr = (volatile uint64_t *)0xfd800008UL;
	volatile char *buf = (volatile char *)0xfd800010UL;

	uint64_t head = *head_ptr;
	uint64_t size = *size_ptr;

	if (size < 0x1000 || size > 0x200000UL) {
		size = 0x80000;
		*size_ptr = size;
		head = 0;
	}

	buf[head % size] = c;
	*head_ptr = head + 1;
}

static inline int is_qemu(void)
{
	uint64_t midr;
	__asm__ volatile("mrs %0, midr_el1" : "=r"(midr));
	return ((midr >> 4) & 0xfff) == 0xd0b;
}

void uart_putc(char c)
{
	if (is_qemu()) {
		*(volatile uint32_t *)0x09000000UL = (uint32_t)(uint8_t)c;
		return;
	}

	*(volatile uint32_t *)0x10A00020UL = (uint32_t)(uint8_t)c;

	if (!ramoops_inited)
		ramoops_init();

	/* Write to persistent ramoops console buffer at 0xfd3ff000 */
	volatile char *ramoops = (volatile char *)0xfd3ff000UL;
	if (ramoops_off < 0x1f0000) {
		ramoops[ramoops_off++] = c;
		ramoops[ramoops_off] = '\0';
	}

	/* Write to persistent ABL dmesg log buffer at 0xfd800000 */
	bldr_putc(c);
}

void uart_puts(const char *s)
{
	while (*s) {
		if (*s == '\n') uart_putc('\r');
		uart_putc(*s++);
	}
}

int strcmp(const char *a, const char *b)
{
	while (*a && *a == *b)
		a++, b++;
	return *(const unsigned char *)a - *(const unsigned char *)b;
}

void *memset(void *s, int c, unsigned long n)
{
	unsigned char *p = s;
	while (n--) *p++ = (unsigned char)c;
	return s;
}

void *memcpy(void *dst, const void *src, unsigned long n)
{
	unsigned char *d = dst;
	const unsigned char *s = src;
	while (n--) *d++ = *s++;
	return dst;
}

void decon_flush(void)
{
	if (is_qemu()) return;

	__asm__ volatile("dsb sy" ::: "memory");

	/* Clean & invalidate both framebuffers to PoC so DPU DMA reads fresh DRAM pixels */
	for (uintptr_t p = 0xfc000000UL; p < 0xfc000000UL + 0x8a0000UL; p += 64) {
		__asm__ volatile("dc civac, %0" :: "r"(p) : "memory");
	}
	for (uintptr_t p = 0xfac00000UL; p < 0xfac00000UL + 0xf80000UL; p += 64) {
		__asm__ volatile("dc civac, %0" :: "r"(p) : "memory");
	}
	__asm__ volatile("dsb sy" ::: "memory");

	volatile uint32_t *decon1 = (volatile uint32_t *)0x1C241000UL;
	volatile uint32_t *decon0 = (volatile uint32_t *)0x1C240000UL;

	/* 1. Window shadow mask (0x3f) */
	decon1[0x50 / 4] |= 0x3f;
	decon0[0x50 / 4] |= 0x3f;

	/* 2. DECON_CR enable & start (0x3) */
	decon1[0x20 / 4] |= 0x3;
	decon0[0x20 / 4] |= 0x3;

	/* 3. UPDATE_REQ global shadow commit (0x80100000) */
	decon1[0x50 / 4] |= 0x80100000;
	decon0[0x50 / 4] |= 0x80100000;

	/* Wait for DECON_CR bit 4 (running state) */
	for (int i = 0; i < 100000; i++) {
		if ((decon1[0x20 / 4] & 0x10) && (decon0[0x20 / 4] & 0x10)) break;
		__asm__ volatile("yield");
	}

	/* 4. Commit HW TE trigger: (TRIGCON & ~0x103) | 0x102 (ABL function 0x69780) */
	decon1[0x30 / 4] = (decon1[0x30 / 4] & ~0x103) | 0x102;
	decon0[0x30 / 4] = (decon0[0x30 / 4] & ~0x103) | 0x102;

	/* 5. Wait for DECON hardware to complete update (UPDATE_REQ becomes 0) */
	uint64_t frq = get_cntfrq();
	if (frq < 1000000ULL || frq > 100000000ULL) frq = 19200000ULL;
	uint64_t end = get_cntvct() + (frq / 20); // 50 ms
	while (get_cntvct() < end) {
		if (decon1[0x50 / 4] == 0)
			break;
		__asm__ volatile("yield");
	}

	/* 6. Leave HW TE trigger continuously armed: 0x3061 across Felix DECONs */
	decon1[0x30 / 4] = 0x3061;
	decon0[0x30 / 4] = 0x3061;

	/* 7. Fallback: if HW TE did not clear UPDATE_REQ, keep 0x3061 */
	if (decon1[0x50 / 4] != 0) {
		decon1[0x30 / 4] = 0x3061;
		decon0[0x30 / 4] = 0x3061;
	}

	__asm__ volatile("dsb sy; isb" ::: "memory");
}

void decon_rearm_continuous(void)
{
	if (is_qemu()) return;

	__asm__ volatile("dsb sy" ::: "memory");
	for (uintptr_t p = 0xfc000000UL; p < 0xfc000000UL + 0x8a0000UL; p += 64) {
		__asm__ volatile("dc civac, %0" :: "r"(p) : "memory");
	}
	for (uintptr_t p = 0xfac00000UL; p < 0xfac00000UL + 0xf80000UL; p += 64) {
		__asm__ volatile("dc civac, %0" :: "r"(p) : "memory");
	}
	__asm__ volatile("dsb sy" ::: "memory");

	/* Arm continuous hardware TE trigger 0x3061 across Felix DECON blocks (0x1C240000 / 0x1C241000) */
	*(volatile uint32_t *)0x1C241030UL = 0x3061;
	*(volatile uint32_t *)0x1C240030UL = 0x3061;

	/* Commit shadow update */
	*(volatile uint32_t *)0x1C241050UL |= 0x8010003f;
	*(volatile uint32_t *)0x1C240050UL |= 0x8010003f;

	__asm__ volatile("dsb sy; isb" ::: "memory");
}

void con_clear(uint32_t bg)
{
	con_fb = (volatile uint32_t *)0xfc000000UL;
	uint64_t bg64 = ((uint64_t)bg << 32) | (uint64_t)bg;

	/* Outer screen: 1080 * 2092 = 2,259,360 pixels */
	volatile uint64_t *fb_out = (volatile uint64_t *)0xfc000000UL;
	for (uint32_t i = 0; i < (1080 * 2092) / 2; i++)
		fb_out[i] = bg64;

	/* Inner screen: 1840 * 2208 = 4,062,720 pixels */
	volatile uint64_t *fb_in = (volatile uint64_t *)0xfac00000UL;
	for (uint32_t i = 0; i < (1840 * 2208) / 2; i++)
		fb_in[i] = bg64;

	con_bg = bg;
	row = 0;
}

static void put_char(int col, char ch, uint32_t fg)
{
	if (ch < 0x20 || ch > 0x7e)
		ch = '?';
	const unsigned char *g = font8x16[ch - 0x20];
	int x0 = MARGIN + col * CH_W, y0 = MARGIN + row * CH_H;
	if (x0 + CH_W > (int)con_w || y0 + CH_H > (int)con_h)
		return;

	for (int gy = 0; gy < 16; gy++)
		for (int gx = 0; gx < 8; gx++) {
			uint32_t c = (g[gy] & (0x80 >> gx)) ? fg : con_bg;
			for (int sy = 0; sy < SCALE; sy++) {
				volatile uint32_t *p_out = (volatile uint32_t *)0xfc000000UL
					+ (y0 + gy * SCALE + sy) * 1080 + x0 + gx * SCALE;
				volatile uint32_t *p_in = (volatile uint32_t *)0xfac00000UL
					+ (y0 + gy * SCALE + sy) * 1840 + x0 + gx * SCALE;
				for (int sx = 0; sx < SCALE; sx++) {
					p_out[sx] = c;
					p_in[sx] = c;
				}
			}
		}
}

static int put_str(int col, const char *s, uint32_t fg)
{
	while (*s)
		put_char(col++, *s++, fg);
	return col;
}

static void fmt_hex(char *buf, uint64_t v, int digits)
{
	static const char hex[] = "0123456789ABCDEF";
	for (int i = 0; i < digits; i++)
		buf[i] = hex[(v >> (4 * (digits - 1 - i))) & 0xf];
	buf[digits] = 0;
}

void con_skip(void)
{
	uart_puts("\r\n");
	row++;
}

void con_str(const char *label, const char *s)
{
	uart_puts("[SHIM] ");
	uart_puts(label);
	uart_puts(": ");
	uart_puts(s);
	uart_puts("\r\n");
	int c = put_str(0, label, YELLOW);
	if (c < LABEL_COLS) c = LABEL_COLS;
	put_str(c + 1, s, WHITE);
	row++;
	decon_flush();
}

void con_hex(const char *label, uint64_t v)
{
	char buf[17];
	fmt_hex(buf, v, 16);
	uart_puts("[SHIM] ");
	uart_puts(label);
	uart_puts(": 0x");
	uart_puts(buf);
	uart_puts("\r\n");
	con_str(label, buf);
}

void con_hex2(const char *label, const char *a, uint32_t va, const char *b, uint32_t vb)
{
	char buf[9];
	uart_puts("[SHIM] ");
	uart_puts(label);
	uart_puts(": ");
	uart_puts(a);
	fmt_hex(buf, va, 8);
	uart_puts(buf);
	if (b) {
		uart_puts(" ");
		uart_puts(b);
		fmt_hex(buf, vb, 8);
		uart_puts(buf);
	}
	uart_puts("\r\n");
	int col = put_str(0, label, YELLOW);
	col = col < LABEL_COLS ? LABEL_COLS : col + 1;
	col = put_str(col, a, YELLOW);
	fmt_hex(buf, va, 8);
	col = put_str(col, buf, WHITE);
	if (b) {
		col = put_str(col + 1, b, YELLOW);
		fmt_hex(buf, vb, 8);
		put_str(col, buf, WHITE);
	}
	row++;
	decon_flush();
}

uint64_t counter_freq(void)
{
	uint64_t frq;
	__asm__ volatile("mrs %0, cntfrq_el0" : "=r"(frq));
	return frq;
}
