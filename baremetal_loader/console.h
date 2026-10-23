#pragma once
#pragma GCC visibility push(hidden)
#include <stdint.h>

#define FB_BASE		0x0UL
#define FB_W		1080
#define FB_H		2092
#define FB_STRIDE	(FB_W * 4)	/* a8r8g8b8, matches mainline simplefb */

#define RED	0xffff0000U
#define BLUE	0xff0044ccU
#define WHITE	0xffffffffU
#define YELLOW	0xffffff00U
#define GREEN	0xff00ff00U
#define BLACK	0xff000000U

extern volatile uint32_t *con_fb;
extern uint32_t con_w;
extern uint32_t con_h;
extern uint32_t con_stride;

void con_clear(uint32_t bg);
void con_skip(void);
void con_str(const char *label, const char *s);
void con_hex(const char *label, uint64_t v);
/* label, then "a=xxxxxxxx b=xxxxxxxx" style pairs of 32-bit values */
void con_hex2(const char *label, const char *a, uint32_t va, const char *b, uint32_t vb);
void decon_flush(void);
void uart_puts(const char *s);
void uart_putc(char c);
void *memset(void *s, int c, unsigned long n);
void *memcpy(void *dst, const void *src, unsigned long n);

static inline uint64_t get_cntvct(void)
{
	uint64_t v;
	__asm__ volatile("isb; mrs %0, cntvct_el0" : "=r"(v));
	return v;
}

static inline uint64_t get_cntfrq(void)
{
	uint64_t v;
	__asm__ volatile("mrs %0, cntfrq_el0" : "=r"(v));
	return v;
}

static inline void delay_ms(unsigned ms)
{
	uint64_t frq = get_cntfrq();
	if (frq < 1000000ULL || frq > 100000000ULL)
		frq = 24576000ULL;
	uint64_t end = get_cntvct() + (frq / 1000) * ms;
	while (get_cntvct() < end) {
		__asm__ volatile("yield");
	}
}
