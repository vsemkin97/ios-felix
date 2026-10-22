/*
 * loader_main.c: Bare-Metal XNU Loader for Google Pixel Fold (felix, Tensor G2)
 * Based on the HackiOS xnuloader architecture (OOSEX GANG).
 */

#include <stdint.h>
#include <stddef.h>
#include "console.h"
#include "identity_felix.h"

#define KERNEL_VIRT_BASE    0xfffffff007004000ULL
#define PAGE_SIZE_MAX       0x4000ULL
#define BOOT_LINE_LENGTH    608
#define NVRAM_BANK_SIZE     0x2000u

#define FB_OUTER_BASE       0xFC000000ULL

#define rsr(r) ({ uint64_t _v; __asm__ volatile("mrs %0, " r : "=r"(_v)); _v; })

#pragma GCC visibility push(hidden)

extern uint8_t _binary_kernel_macho_start[] __attribute__((visibility("hidden")));
extern uint8_t _binary_kernel_macho_end[] __attribute__((visibility("hidden")));

void clean_dcache(uint64_t start, uint64_t end) __attribute__((visibility("hidden")));
void jump_to_kernel(uint64_t entry_pa, uint64_t boot_args_pa) __attribute__((visibility("hidden")));
void loader_return_stub(void) __attribute__((visibility("hidden")));

static size_t
str_len(const char *s)
{
	size_t n = 0;
	while (*s++) n++;
	return n;
}

static int
str_eq(const char *a, const char *b)
{
	while (*a && *b && *a == *b) {
		a++; b++;
	}
	return *a == *b;
}

/* ------------------------------------------------------------------------ */
/* Structures                                                               */
/* ------------------------------------------------------------------------ */

struct boot_video {
	uint64_t v_baseAddr;
	uint64_t v_display;
	uint64_t v_rowBytes;
	uint64_t v_width;
	uint64_t v_height;
	uint64_t v_depth;
};

struct boot_args {
	uint16_t revision;
	uint16_t version;
	uint64_t virtBase;
	uint64_t physBase;
	uint64_t memSize;
	uint64_t topOfKernelData;
	struct boot_video video;
	uint32_t machineType;
	uint64_t deviceTreeP;
	uint32_t deviceTreeLength;
	char commandLine[BOOT_LINE_LENGTH];
	uint64_t bootFlags;
	uint64_t memSizeActual;
};

#define MH_MAGIC_64      0xFEEDFACFu
#define LC_SEGMENT_64    0x19
#define LC_UNIXTHREAD    0x5

struct mach_header_64 {
	uint32_t magic, cputype, cpusubtype, filetype, ncmds, sizeofcmds, flags, reserved;
};

struct load_command {
	uint32_t cmd, cmdsize;
};

struct segment_command_64 {
	uint32_t cmd, cmdsize;
	char segname[16];
	uint64_t vmaddr, vmsize, fileoff, filesize;
	uint32_t maxprot, initprot, nsects, flags;
};

/* ------------------------------------------------------------------------ */
/* Apple Device Tree Writer                                                 */
/* ------------------------------------------------------------------------ */

struct dt_writer {
	uint8_t *base, *cur;
	uint32_t *counts[16];
	int depth;
};

static void
dt_prop(struct dt_writer *w, const char *name, const void *data, uint32_t len)
{
	memset(w->cur, 0, 32);
	memcpy(w->cur, name, str_len(name));
	*(uint32_t *)(w->cur + 32) = len;
	memset(w->cur + 36, 0, (len + 3) & ~3u);
	if (data && len) {
		memcpy(w->cur + 36, data, len);
	}
	w->cur += 36 + ((len + 3) & ~3u);
	w->counts[w->depth - 1][0]++;
}

static void
dt_prop_str(struct dt_writer *w, const char *name, const char *value)
{
	dt_prop(w, name, value, (uint32_t)str_len(value) + 1);
}

static void
dt_prop_u32(struct dt_writer *w, const char *name, uint32_t value)
{
	dt_prop(w, name, &value, sizeof(value));
}

static void
dt_prop_u64(struct dt_writer *w, const char *name, uint64_t value)
{
	dt_prop(w, name, &value, sizeof(value));
}

static void
dt_prop_padded(struct dt_writer *w, const char *name, const char *value, uint32_t size)
{
	uint8_t field[64];
	memset(field, 0, sizeof(field));
	memcpy(field, value, str_len(value));
	dt_prop(w, name, field, size);
}

static void
dt_identity(struct dt_writer *w, const struct identity_prop *props, uint32_t count, const unsigned char *data)
{
	for (uint32_t i = 0; i < count; i++) {
		if (str_eq(props[i].name, "artwork-display-gamut")) {
			dt_prop(w, props[i].name, "sRGB", 5);
			continue;
		}
		dt_prop(w, props[i].name, data + props[i].offset, props[i].length);
	}
}

static void
dt_begin(struct dt_writer *w, const char *name)
{
	if (w->depth > 0) {
		w->counts[w->depth - 1][1]++;
	}
	w->counts[w->depth++] = (uint32_t *)w->cur;
	w->counts[w->depth - 1][0] = 0;
	w->counts[w->depth - 1][1] = 0;
	w->cur += 8;
	dt_prop_str(w, "name", name);
}

static void
dt_end(struct dt_writer *w)
{
	w->depth--;
}

static void
nvram_chrp_header(uint8_t *h, uint8_t type, uint32_t size, const char *name)
{
	uint32_t sum = 0;
	h[0] = type;
	h[1] = 0;
	h[2] = (uint8_t)(size >> 12);
	h[3] = (uint8_t)(size >> 4);
	memset(h + 4, 0, 12);
	memcpy(h + 4, name, str_len(name));
	for (int i = 0; i < 16; i++) {
		if (i != 1) sum += h[i];
	}
	while (sum > 0xff) sum = (sum & 0xff) + (sum >> 8);
	h[1] = (uint8_t)sum;
}

static void
build_nvram_image(uint8_t *image)
{
	uint32_t lo = 1, hi = 0, generation = 1;
	memset(image, 0, NVRAM_BANK_SIZE);
	nvram_chrp_header(image, 0x5a, 0x20, "nvram");
	nvram_chrp_header(image + 0x20, 0x70, NVRAM_BANK_SIZE - 0x20, "common");
	memcpy(image + 20, &generation, 4);
	for (uint32_t i = 20; i < NVRAM_BANK_SIZE; i++) {
		lo = (lo + image[i]) % 65521;
		hi = (hi + lo) % 65521;
	}
	uint32_t csum = (hi << 16) | lo;
	memcpy(image + 16, &csum, 4);
}

/* ------------------------------------------------------------------------ */
/* Exception Handlers                                                       */
/* ------------------------------------------------------------------------ */

void
exc_handler(uint64_t slot, uint64_t *regs)
{
	/* Disable AP watchdogs immediately */
	*(volatile uint32_t *)0x10060000UL = 0;
	*(volatile uint32_t *)0x10070000UL = 0;

	con_clear(RED);
	uint32_t s = slot & 3;
	if (s == 0) con_str("PANIC", "SYNCHRONOUS ABORT");
	else if (s == 1) con_str("PANIC", "IRQ");
	else if (s == 2) con_str("PANIC", "FIQ");
	else con_str("PANIC", "SERROR / SYSTEM ERROR");

	con_hex2("SLOT", "n=", (uint32_t)slot, "cpu=", (uint32_t)rsr("mpidr_el1"));
	con_hex("ESR", rsr("esr_el1"));
	con_hex("ELR", rsr("elr_el1"));
	con_hex("FAR", rsr("far_el1"));
	con_hex("SPSR", rsr("spsr_el1"));
	con_hex("X0", regs[0]);
	con_hex("X1", regs[1]);
	con_hex("X30", regs[29]);
	decon_flush();

	for (;;) {
		__asm__ volatile("wfe");
	}
}

void
on_xnu_exception(uint64_t log_pa, uint64_t log_len, uint64_t code,
                 uint64_t esr, uint64_t elr, uint64_t far, uint64_t lr, uint64_t sp)
{
	/* Disable AP watchdogs immediately */
	*(volatile uint32_t *)0x10060000UL = 0;
	*(volatile uint32_t *)0x10070000UL = 0;

	if (code == 2) {
		/* Checkpoint reached! Dark green background */
		con_clear(0xFF003F1F);
		con_str("DARWIN", "XNU Execution Active!");
		con_hex2("CHECKPOINT", "num=", (uint32_t)esr, "code=", (uint32_t)code);
		if (esr == 1) con_str("STAGE", "01: _start_first_cpu entry");
		else if (esr == 2) con_str("STAGE", "02: VBAR and OSLAR configured");
		else if (esr == 3) con_str("STAGE", "03: Bootstrap pagetables ready");
		else if (esr == 4) con_str("STAGE", "04: TTBR0/TTBR1/TCR armed");
		else if (esr == 5) con_str("STAGE", "05: MMU enabled (SCTLR_EL1)");
		else if (esr == 6) con_str("STAGE", "06: common_start ret reached!");
		else if (esr == 7) con_str("STAGE", "07: arm_init() C entry reached!");
		else if (esr == 10) con_str("STAGE", "10: arm_init() C entry reached!");
		else con_str("STAGE", "Checkpoint reached");
		con_hex("SP", sp);
		con_hex("LR", lr);
		con_hex("PAR", far);
	} else {
		/* True exception: Red background */
		con_clear(RED);
		con_str("PANIC", "XNU Early Exception Intercepted!");
		con_hex2("INFO", "code=", (uint32_t)code, "log_len=", (uint32_t)log_len);
		con_hex("ESR", esr);
		con_hex("ELR", elr);
		con_hex("FAR", far);
		con_hex("LR", lr);
		con_hex("SP", sp);
	}
	decon_flush();

	for (;;) {
		*(volatile uint32_t *)0x10060000UL = 0;
		*(volatile uint32_t *)0x10070000UL = 0;
		delay_ms(50);
		decon_flush();
	}
}

static inline uint64_t smc(uint64_t fn, uint64_t a1, uint64_t a2, uint64_t a3)
{
	register uint64_t x0 __asm__("x0") = fn;
	register uint64_t x1 __asm__("x1") = a1;
	register uint64_t x2 __asm__("x2") = a2;
	register uint64_t x3 __asm__("x3") = a3;
	__asm__ volatile("smc #0"
			 : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3)
			 :
			 : "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11",
			   "x12", "x13", "x14", "x15", "x16", "x17", "memory");
	return x0;
}

void
display_daemon_main(uint64_t cpu_id)
{
	(void)cpu_id;
	for (;;) {
		*(volatile uint32_t *)0x10060000UL = 0;
		*(volatile uint32_t *)0x10070000UL = 0;
		delay_ms(50);
		decon_flush();
	}
}

/* ------------------------------------------------------------------------ */
/* Main Loader Entry Point                                                  */
/* ------------------------------------------------------------------------ */

void
felix_baremetal_main(uint64_t fdt_addr)
{
	(void)fdt_addr;

	/* 1. Disable AP Watchdog Timers immediately to eliminate resets */
	*(volatile uint32_t *)0x10060000UL = 0; // Cluster 0 WDT
	*(volatile uint32_t *)0x10070000UL = 0; // Cluster 1 WDT

	/* 2. Visual Takeover: Dark blue console on Pixel Fold AMOLED */
	con_clear(0xFF001F3F);
	con_str("DARWIN", "HackiOS Bare-Metal Loader");
	con_str("TARGET", "Google Pixel Fold (felix, Tensor G2)");
	con_str("STATUS", "Display takeover active!");
	decon_flush();

	uint8_t *kernel_macho = _binary_kernel_macho_start;
	size_t kernel_size = _binary_kernel_macho_end - _binary_kernel_macho_start;

	struct mach_header_64 *mh = (struct mach_header_64 *)kernel_macho;
	if (mh->magic != MH_MAGIC_64) {
		con_clear(RED);
		con_hex("FATAL MAGIC", mh->magic);
		decon_flush();
		for (;;) { __asm__ volatile("wfe"); }
	}

	con_hex2("KERNEL", "size=", (uint32_t)kernel_size, "ncmds=", mh->ncmds);

	/* Memory Layout: Must match 2MB block offset of KERNEL_VIRT_BASE (0x4000) */
	uint64_t phys_base = 0xA0000000ULL + (KERNEL_VIRT_BASE & (0x200000ULL - 1));
	uint64_t win_size  = 0x50000000ULL; /* 1.25 GB */
	uint64_t slide     = phys_base - KERNEL_VIRT_BASE;

	uint64_t vm_lo = ~0ULL, vm_hi = 0, entry_va = 0;

	/* 3. Parse segments */
	const struct load_command *lc = (const struct load_command *)(mh + 1);
	for (uint32_t i = 0; i < mh->ncmds; i++, lc = (const void *)((const uint8_t *)lc + lc->cmdsize)) {
		if (lc->cmd == LC_SEGMENT_64) {
			const struct segment_command_64 *sg = (const void *)lc;
			if (sg->vmsize != 0) {
				if (sg->vmaddr < vm_lo) vm_lo = sg->vmaddr;
				if (sg->vmaddr + sg->vmsize > vm_hi) vm_hi = sg->vmaddr + sg->vmsize;
			}
		} else if (lc->cmd == LC_UNIXTHREAD) {
			entry_va = ((const uint64_t *)((const uint8_t *)lc + 16))[32];
		}
	}

	uint64_t entry_pa = entry_va + slide;
	uint64_t kern_lo = vm_lo + slide;
	uint64_t kern_hi = (vm_hi + slide + PAGE_SIZE_MAX - 1) & ~(PAGE_SIZE_MAX - 1);

	con_hex2("LOAD PA", "lo=", (uint32_t)kern_lo, "hi=", (uint32_t)kern_hi);

	/* 4. Copy segments using fast 64-bit aligned transfers */
	lc = (const struct load_command *)(mh + 1);
	uint32_t seg_copied = 0;
	for (uint32_t i = 0; i < mh->ncmds; i++, lc = (const void *)((const uint8_t *)lc + lc->cmdsize)) {
		if (lc->cmd == LC_SEGMENT_64) {
			const struct segment_command_64 *sg = (const void *)lc;
			if (sg->vmsize == 0) continue;
			uint64_t dst_pa = sg->vmaddr + slide;
			/* Zero BSS region of segment */
			volatile uint64_t *d = (volatile uint64_t *)dst_pa;
			for (uint64_t j = 0; j < (sg->vmsize + 7) / 8; j++) d[j] = 0;
			/* Copy file data */
			volatile const uint64_t *s = (volatile const uint64_t *)(kernel_macho + sg->fileoff);
			for (uint64_t j = 0; j < (sg->filesize + 7) / 8; j++) d[j] = s[j];
			seg_copied++;
		}
	}
	con_hex2("SEGS", "copied=", seg_copied, "top=", (uint32_t)kern_hi);

	/* 4. Device Tree placement */
	uint64_t dt_pa = (kern_hi + 0x3fffULL) & ~0x3fffULL;

	/* 5. Build Apple Device Tree */
	struct dt_writer w = { .base = (uint8_t *)dt_pa, .cur = (uint8_t *)dt_pa, .depth = 0 };
	uint64_t cntfrq = rsr("cntfrq_el0");

	uint64_t ranges[3] = { 0, 0x10000000ULL, 0x10000000ULL };
	uint8_t seed[64];
	for (unsigned i = 0; i < sizeof(seed); i++) seed[i] = (uint8_t)(i * 37 + 0x42);

	dt_begin(&w, "device-tree");
	dt_identity(&w, identity_root, sizeof(identity_root) / sizeof(identity_root[0]), identity_root_data);
	dt_prop_str(&w, "platform-name", "t8015");
	dt_prop_padded(&w, "serial-number", "FELIX00PIXELFOLD", 32);
	dt_prop_padded(&w, "mlb-serial-number", "FELIX00PIXELFOLDMLB", 32);
	dt_prop_padded(&w, "model-number", "MQAC2", 32);
	dt_prop_padded(&w, "region-info", "LL/A", 32);
	dt_prop_padded(&w, "regulatory-model-number", "A1865", 32);
	dt_prop_u32(&w, "#address-cells", 2);
	dt_prop_u32(&w, "#size-cells", 2);

	dt_begin(&w, "chosen");
	dt_prop(&w, "random-seed", seed, sizeof(seed));
	dt_prop_u64(&w, "dram-base", phys_base);
	dt_prop_u64(&w, "dram-size", win_size);
	dt_prop_u32(&w, "debug-enabled", 1);
	dt_prop_u32(&w, "display-scale", 3);
	dt_prop_u32(&w, "display-rotation", 0);
	dt_prop_str(&w, "psci-method", "smc");

	static const uint8_t manifest_hash[48] = { 0x66, 0x65, 0x6c, 0x69, 0x78 };
	static const uint8_t no_behavior[16];
	dt_prop_u64(&w, "unique-chip-id", 0x000052445730001ULL);
	dt_prop_u32(&w, "chip-id", 0x8015);
	dt_prop_u32(&w, "board-id", 0x06);
	dt_prop(&w, "boot-manifest-hash", manifest_hash, sizeof(manifest_hash));
	dt_prop(&w, "software-behavior", no_behavior, sizeof(no_behavior));
	dt_prop_str(&w, "firmware-version", "iBoot-8422.100.650");
	dt_prop_u32(&w, "certificate-production-status", 1);
	dt_prop_u32(&w, "certificate-security-mode", 1);
	dt_prop_u32(&w, "effective-production-status-ap", 1);
	dt_prop_u32(&w, "effective-security-mode-ap", 1);
	dt_prop_u32(&w, "secure-boot", 1);
	dt_prop_u32(&w, "production-cert", 1);
	dt_prop_u32(&w, "development-cert", 0);
	dt_prop_u32(&w, "embedded-panic-log-size", 0x100000);

	static uint8_t nvram_image[NVRAM_BANK_SIZE];
	build_nvram_image(nvram_image);
	dt_prop_u32(&w, "nvram-bank-size", NVRAM_BANK_SIZE);
	dt_prop_u32(&w, "nvram-bank-count", 1);
	dt_prop_u32(&w, "nvram-current-bank", 0);
	dt_prop(&w, "nvram-proxy-data", nvram_image, sizeof(nvram_image));

	dt_begin(&w, "memory-map");
	dt_end(&w);
	dt_end(&w); /* chosen */

	dt_begin(&w, "defaults");
	dt_prop_u32(&w, "kern.vm_compressor", 2);
	dt_end(&w);

	dt_begin(&w, "product");
	dt_identity(&w, identity_product, sizeof(identity_product) / sizeof(identity_product[0]), identity_product_data);
	dt_end(&w);

	dt_begin(&w, "options");
	dt_end(&w);

	/* 8 CPU cores */
	dt_begin(&w, "cpus");
	for (int i = 0; i < 8; i++) {
		char name[5] = { 'c', 'p', 'u', (char)('0' + i), 0 };
		dt_begin(&w, name);
		dt_prop_str(&w, "device_type", "cpu");
		dt_prop_str(&w, "state", i == 0 ? "running" : "waiting");
		dt_prop_u32(&w, "reg", (uint32_t)(i << 8)); /* Tensor G2 MPIDR: 0x00, 0x100..0x700 */
		dt_prop_u32(&w, "cpu-id", (uint32_t)i);
		dt_prop_u64(&w, "timebase-frequency", cntfrq);
		dt_prop_u64(&w, "fixed-frequency", cntfrq);
		dt_prop_u64(&w, "clock-frequency", 1800000000ULL);
		dt_prop_u64(&w, "bus-frequency", 100000000ULL);
		dt_prop_u64(&w, "memory-frequency", 100000000ULL);
		dt_prop_u64(&w, "peripheral-frequency", 24000000ULL);
		if (i < 4) dt_prop_u32(&w, "small-core", 1);
		dt_end(&w);
	}
	dt_end(&w); /* cpus */

	/* arm-io */
	dt_begin(&w, "arm-io");
	dt_prop_str(&w, "device_type", "felix");
	dt_prop(&w, "ranges", ranges, sizeof(ranges));
	dt_prop_u32(&w, "#address-cells", 2);
	dt_prop_u32(&w, "#size-cells", 2);

	/* GICv3 on Tensor G2: Dist=0x10400000, Redist=0x10440000 */
	dt_begin(&w, "gic");
	dt_prop_str(&w, "device_type", "interrupt-controller");
	dt_prop_str(&w, "compatible", "arm,gic-v3");
	uint64_t gic_reg[4] = { 0x00400000, 0x10000, 0x00440000, 0x100000 };
	dt_prop(&w, "reg", gic_reg, sizeof(gic_reg));
	dt_end(&w);

	dt_end(&w); /* arm-io */
	dt_end(&w); /* device-tree */

	uint32_t dt_len = (uint32_t)(w.cur - w.base);
	uint64_t ba_pa = (dt_pa + dt_len + PAGE_SIZE_MAX - 1) & ~(PAGE_SIZE_MAX - 1);
	uint64_t top = ba_pa + PAGE_SIZE_MAX;
	con_hex2("DEVTREE", "pa=", (uint32_t)dt_pa, "len=", dt_len);
	con_hex2("BOOTARGS", "pa=", (uint32_t)ba_pa, "top=", (uint32_t)top);

	/* 6. Build boot_args */
	struct boot_args *ba = (struct boot_args *)ba_pa;
	memset(ba, 0, sizeof(*ba));
	ba->revision = 2;
	ba->version = 2;
	ba->virtBase = KERNEL_VIRT_BASE;
	ba->physBase = phys_base;
	ba->memSize = win_size;
	ba->memSizeActual = win_size;
	ba->topOfKernelData = top;
	ba->video.v_baseAddr = FB_OUTER_BASE;
	ba->video.v_display = 0; /* verbose mode */
	ba->video.v_rowBytes = FB_STRIDE;
	ba->video.v_width = FB_W;
	ba->video.v_height = FB_H;
	ba->video.v_depth = 32;
	ba->deviceTreeP = KERNEL_VIRT_BASE + (dt_pa - phys_base); /* EXACT virtual address! */
	ba->deviceTreeLength = dt_len;

	const char *cmd = "-v debug=0x8 serial=3 cpus=1";
	memcpy(ba->commandLine, cmd, str_len(cmd) + 1);
	con_str("BOOTARGS", ba->commandLine);
	decon_flush();

	/* 7. Patch "XNULDRST" early exception slot */
	uint8_t *p = (uint8_t *)kern_lo;
	int patched = 0;
	while (p + 16 <= (uint8_t *)kern_hi) {
		if (p[0] == 'X' && p[1] == 'N' && p[2] == 'U' && p[3] == 'L' &&
		    p[4] == 'D' && p[5] == 'R' && p[6] == 'S' && p[7] == 'T') {
			uint64_t *slot = (uint64_t *)p;
			slot[1] = (uint64_t)&loader_return_stub;
			slot[2] = 0; /* Checkpoint 0: NO CHECKPOINTS - boot kernel directly! */
			slot[3] = slide;
			patched = 1;
			break;
		}
		p += 8;
	}

	/* 8. Restore common_start at 0xfffffff0074614a8:
	 * add x0, x20, x22 (0x8b160280)
	 * sub x0, x0, x23  (0xcb170000)
	 * ret              (0xd65f03c0)
	 */
	*(volatile uint32_t *)(0xfffffff0074614a8ULL + slide) = 0x8b160280;
	*(volatile uint32_t *)(0xfffffff0074614acULL + slide) = 0xcb170000;
	*(volatile uint32_t *)(0xfffffff0074614b0ULL + slide) = 0xd65f03c0;

	/* 9. Patch _start_first_cpu at 0xfffffff0074612e4:
	 * Right after _bootstrap_pagetables is built, map 0x80000000 (loader_return_stub)
	 * into TTBR0 Page 2 entry 0, then jump directly to common_start.
	 */
	volatile uint32_t *map80 = (volatile uint32_t *)(0xfffffff0074612e4ULL + slide);
	map80[0] = 0xd280c02a; /* mov  x10, #0x601 */
	map80[1] = 0xf2b0000a; /* movk x10, #0x8000, lsl #16 */
	map80[2] = 0xf910032a; /* str  x10, [x25, #0x2000] (Page 2[0] = 0x80000601) */
	map80[3] = 0x1400000d; /* b    common_start (0xfffffff007461324) */

	/* 10. Patch _LowExceptionVectorBase at 0xfffffff00745f7d0:
	 * NOP out the 4 instructions that disabled MMU while PC was in TTBR1.
	 * Now exceptions cleanly branch to loader_return_stub at 0x800018a0 with MMU ON.
	 */
	volatile uint32_t *exc_patch = (volatile uint32_t *)(0xfffffff00745f7d0ULL + slide);
	exc_patch[0] = 0xd503201f; /* nop (was mrs x9, SCTLR_EL1) */
	exc_patch[1] = 0xd503201f; /* nop (was and x9, x9, #~1) */
	exc_patch[2] = 0xd503201f; /* nop (was msr SCTLR_EL1, x9) */
	exc_patch[3] = 0xd503201f; /* nop (was isb) */

	/* 11. Patch _loader_jump at 0xfffffff007472a6c:
	 * Replace "bl _PE_parse_boot_argn" with "mov w0, #0" so early checkpoints
	 * do not crash on NULL PE_state.bootArgs before PE_init_platform runs.
	 */
	*(volatile uint32_t *)(0xfffffff007472a6cULL + slide) = 0x52800000; /* mov w0, #0 */

	/* 12. Pre-initialize PE_state.bootArgs pointer and loader_l1/l2 tables */
	uint64_t ba_va = KERNEL_VIRT_BASE + (ba_pa - phys_base);
	*(volatile uint64_t *)(0xfffffff007252900ULL + slide) = ba_va;

	uint64_t loader_l2_pa = 0xfffffff007e7c000ULL + slide;
	uint64_t *l1 = (uint64_t *)(0xfffffff007e7d000ULL + slide);
	uint64_t *l2 = (uint64_t *)(0xfffffff007e7c000ULL + slide);
	l1[2] = loader_l2_pa | 0x3;
	l2[0] = 0x80000000ULL | 0x601;

	con_hex2("STUB", "patched=", patched, "chkpt=", 0);
	con_hex("ENTRY", entry_pa);
	con_str("READY", "Jumping to XNU kernel in 2s...");

	/* Start Core 1 as background display refresh daemon (20 FPS DECON flush) */
	extern void secondary_entry(void) __attribute__((visibility("hidden")));
	uint64_t psci_ret = smc(0xc4000003UL, 1 << 8, (uintptr_t)secondary_entry, 1);
	con_hex2("CORE 1", "psci=", (uint32_t)psci_ret, "mpidr=", 0x100);

	/* Clean caches across the entire loaded region */
	clean_dcache(kern_lo, top);

	delay_ms(2000);

	/* Jump to XNU! */
	jump_to_kernel(entry_pa, ba_pa);
}
