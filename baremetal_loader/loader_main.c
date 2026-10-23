/*
 * loader_main.c: Bare-Metal XNU Loader for Google Pixel Fold (felix, Tensor G2)
 * Based on the HackiOS xnuloader architecture (OOSEX GANG).
 */

#include <stdint.h>
#include <stddef.h>
#include "console.h"
#include "identity_felix.h"
#include "ufsinit.h"

static uint64_t g_virt_base = 0xfffffff007004000ULL;
static uint64_t g_phys_base = 0xA0004000ULL;
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

extern const uint8_t _binary_test_launchd_bin_start[];
extern const uint8_t _binary_test_launchd_bin_end[];

extern void secondary_entry(void);

static volatile uint64_t g_rdw_pa = 0;
static volatile uint64_t g_ufs_buf_pa = 0;
static volatile uint64_t g_ufs_dma_desc_pa = 0;
static volatile uint32_t g_ufs_hardware_ready = 0;
static volatile uint64_t g_userdata_start_lba = 0;
static volatile uint32_t g_core1_alive = 0;

void secondary_core_main(void)
{
	g_core1_alive = 0xCAFEBABE;
	volatile uint8_t *rdw = (volatile uint8_t *)g_rdw_pa;
	uint32_t last_answered = 0;

	for (;;) {
		uint32_t req = *(volatile uint32_t *)(rdw + 0x80);
		if (req != 0 && req != last_answered) {
			uint64_t lba  = *(volatile uint64_t *)(rdw + 0x88);
			uint64_t cnt  = *(volatile uint64_t *)(rdw + 0x90);

			int64_t status = 0;
			if (!g_ufs_hardware_ready) {
				status = (int64_t)0xBAAD000100000000ULL;
			} else if (g_ufs_buf_pa == 0) {
				status = (int64_t)0xBAAD000300000000ULL;
			} else {
				uint64_t phys_lba = g_userdata_start_lba + lba;
				int64_t scsi_rc = ufs_scsi_read(g_ufs_dma_desc_pa, 0, phys_lba, (uint32_t)cnt, (void *)g_ufs_buf_pa);
				if (scsi_rc != 0) {
					status = scsi_rc;
				}
			}

			/* Flush buffer from Core 1 cache if any, then memory barrier */
			clean_dcache(g_ufs_buf_pa, g_ufs_buf_pa + cnt * 4096);
			__asm__ volatile("dmb sy" ::: "memory");

			/* Success: acknowledge request */
			*(volatile int64_t *)(rdw + 0x108) = status;
			*(volatile uint64_t *)(rdw + 0x110) += 1;
			*(volatile uint64_t *)(rdw + 0x120) += cnt * 4096;
			__asm__ volatile("dmb sy" ::: "memory");
			*(volatile uint32_t *)(rdw + 0x100) = req;
			last_answered = req;
		}
		__asm__ volatile("yield");
	}
}

static int psci_cpu_on(uint64_t mpidr, uint64_t entry_pa, uint64_t context_id)
{
	register uint64_t x0 __asm__("x0") = 0xC4000003; /* PSCI_CPU_ON_64 */
	register uint64_t x1 __asm__("x1") = mpidr;
	register uint64_t x2 __asm__("x2") = entry_pa;
	register uint64_t x3 __asm__("x3") = context_id;
	__asm__ volatile(
		"smc #0\n"
		: "+r"(x0)
		: "r"(x1), "r"(x2), "r"(x3)
		: "memory"
	);
	return (int)x0;
}


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
nvram_chrp_header(uint8_t *h, uint8_t sig, uint32_t bytes, const char *name)
{
	uint16_t blocks = (uint16_t)(bytes / 16), sum;

	memset(h, 0, 16);
	h[0] = sig;
	h[2] = (uint8_t)blocks;
	h[3] = (uint8_t)(blocks >> 8);
	memcpy(h + 4, name, str_len(name));
	/* checksum over signature, length and name, folded to 8 bits */
	sum = h[0];
	for (int i = 2; i < 16; i++) {
		sum += h[i];
	}
	while (sum > 0xff) {
		sum = (sum & 0xff) + (sum >> 8);
	}
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
		else if (esr == 10) con_str("STAGE", "10: arm_init() entry (bootArgs copied)");
		else if (esr == 11) con_str("STAGE", "11: PE_init_platform complete");
		else if (esr == 30) con_str("STAGE", "30: timer configured");
		else if (esr == 31) con_str("STAGE", "31: wfe_timeout configured");
		else if (esr == 32) con_str("STAGE", "32: wfe_timeout init");
		else if (esr == 33) con_str("STAGE", "33: misc apple regs configured");
		else if (esr == 34) con_str("STAGE", "34: chosen node parsed");
		else if (esr == 35) con_str("STAGE", "35: gT0Sz configured");
		else if (esr == 12) con_str("STAGE", "12: cpu topology parsed");
		else if (esr == 40) con_str("STAGE", "40: boot cpu number verified");
		else if (esr == 41) con_str("STAGE", "41: cpu stacks configured");
		else if (esr == 42) con_str("STAGE", "42: thread bootstrap complete");
		else if (esr == 43) con_str("STAGE", "43: current thread established");
		else if (esr == 17) con_str("STAGE", "17: arm_vm_init complete");
		else if (esr == 18) con_str("STAGE", "18: serial init complete");
		else if (esr == 19) con_str("STAGE", "19: startup init upto 4 complete");
		else if (esr == 20) con_str("STAGE", "20: screen console created!");
		else con_str("STAGE", "Checkpoint reached");
		con_hex("SP", sp);
		con_hex("LR", lr);
		con_hex("PAR", far);
	} else {
		/* True exception / panic: Red background */
		con_clear(RED);
		con_str("PANIC", "XNU Exception / Panic Intercepted!");
		con_hex2("INFO", "code=", (uint32_t)code, "log_len=", (uint32_t)log_len);
		con_hex("ESR", esr);
		con_hex("ELR", elr);
		con_hex("FAR", far);
		con_hex("LR", lr);
		con_hex("SP", sp);
		if (g_virt_base && esr >= g_virt_base && esr < g_virt_base + 0x2000000ULL) {
			const char *msg = (const char *)(esr - g_virt_base + g_phys_base);
			con_str("MSG", msg);
		}
	}
	decon_flush();

	for (;;) {
		*(volatile uint32_t *)0x10060000UL = 0;
		*(volatile uint32_t *)0x10070000UL = 0;
		delay_ms(50);
		decon_flush();
	}
}



static void
ramfb_init_qemu(uint64_t fb, uint32_t w, uint32_t h, uint32_t stride)
{
	static struct {
		uint64_t addr;
		uint32_t fourcc, flags, width, height, stride;
	} __attribute__((packed)) cfg;
	static struct {
		uint32_t control, length;
		uint64_t address;
	} dma;
	struct {
		uint32_t size;
		uint16_t select, reserved;
		char name[56];
	} file;
	uint32_t count, select = 0;

	*(volatile uint16_t *)(0x09020000ULL + 8) = __builtin_bswap16(0x19);
	for (uint32_t i = 0; i < sizeof(count); i++)
		((uint8_t *)&count)[i] = *(volatile uint8_t *)0x09020000ULL;
	count = __builtin_bswap32(count);
	for (uint32_t i = 0; i < count && i < 256 && select == 0; i++) {
		for (uint32_t j = 0; j < sizeof(file); j++)
			((uint8_t *)&file)[j] = *(volatile uint8_t *)0x09020000ULL;
		if (str_eq(file.name, "etc/ramfb"))
			select = __builtin_bswap16(file.select);
	}
	if (select == 0) return;

	cfg.addr = __builtin_bswap64(fb);
	cfg.fourcc = __builtin_bswap32(0x34325258); /* 'XR24' */
	cfg.flags = 0;
	cfg.width = __builtin_bswap32(w);
	cfg.height = __builtin_bswap32(h);
	cfg.stride = __builtin_bswap32(stride);

	dma.control = __builtin_bswap32((select << 16) | 0x08 | 0x10);
	dma.length = __builtin_bswap32(sizeof(cfg));
	dma.address = __builtin_bswap64((uint64_t)(uintptr_t)&cfg);
	*(volatile uint64_t *)(0x09020000ULL + 16) = __builtin_bswap64((uint64_t)(uintptr_t)&dma);
}

/* ------------------------------------------------------------------------ */
/* Main Loader Entry Point                                                  */
/* ------------------------------------------------------------------------ */

void
felix_baremetal_main(uint64_t fdt_addr)
{
	(void)fdt_addr;

	uint64_t midr = rsr("midr_el1");
	int qemu = (((midr >> 4) & 0xfff) == 0xd0b);

	if (qemu) {
		ramfb_init_qemu(FB_OUTER_BASE, FB_W, FB_H, FB_STRIDE);
	} else {
		/* 1. Disable AP Watchdog Timers immediately on physical hardware */
		*(volatile uint32_t *)0x10060000UL = 0; // Cluster 0 WDT
		*(volatile uint32_t *)0x10070000UL = 0; // Cluster 1 WDT
	}

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

	uint64_t vm_lo = ~0ULL, vm_hi = 0, entry_va = 0;

	/* 3. Parse segments first to dynamically detect virtual base and entry */
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

	uint64_t virt_base = vm_lo;
	g_virt_base = virt_base;

	/*
	 * Block alignment:
	 * 16KB page kernels (Sweet, 0xfffffe00...) use 32MB L2 blocks (0x2000000).
	 * 4KB page kernels (Redwood, 0xfffffff0...) use 2MB L2 blocks (0x200000).
	 */
	uint64_t blk_align = (virt_base == 0xfffffe0007004000ULL) ? 0x2000000ULL : 0x200000ULL;
	uint64_t phys_base = 0xA0000000ULL + (virt_base & (blk_align - 1));
	g_phys_base = phys_base;
	uint64_t win_size  = 0x50000000ULL; /* 1.25 GB */
	uint64_t slide     = phys_base - virt_base;

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
	con_hex2("SEGMENTS", "copied=", seg_copied, "cmds=", mh->ncmds);
	/* 4. RDWBLOCK, UFS DMA Buffer, RAMDisk & Device Tree placement */
	uint64_t rdw_pa = (kern_hi + 0x3ffffULL) & ~0x3ffffULL;
	uint64_t rdw_size = 0x4000ULL; /* 16 KB page */

	uint64_t ufs_buf_pa = rdw_pa + rdw_size;
	uint64_t ufs_buf_size = 0x40000ULL; /* 256 KB */

	/* Initialize RDWBLOCK descriptor for bcm2837 redwood_ufs */
	volatile uint8_t *rdw = (volatile uint8_t *)rdw_pa;
	for (uint64_t i = 0; i < rdw_size; i++) rdw[i] = 0;

	/* Header */
	*(volatile uint64_t *)(rdw + 0x00) = 0x4b434f4c42574452ULL; /* 'RDWBLOCK' */
	*(volatile uint32_t *)(rdw + 0x08) = 3;                      /* version = 3 */
	*(volatile uint32_t *)(rdw + 0x0c) = 0x2b00;                 /* size = 0x2b00 */
	*(volatile uint32_t *)(rdw + 0x10) = 1;                      /* 1 partition */
	*(volatile uint32_t *)(rdw + 0x14) = 256 * 1024;             /* buffer_size in bytes (256 KB) */
	*(volatile uint64_t *)(rdw + 0x18) = ufs_buf_pa;             /* buffer_pa */

	/* Partition 0: 'userdata' at offset 0x180 */
	volatile uint8_t *part0 = rdw + 0x180;
	const char *pname = "userdata";
	for (int i = 0; pname[i]; i++) part0[i] = pname[i];
	*(volatile uint32_t *)(part0 + 0x24) = 4096;                 /* block_size = 4096 bytes */
	*(volatile uint64_t *)(part0 + 0x28) = 4194304ULL;           /* 4,194,304 blocks (16 GB) */
	*(volatile uint64_t *)(part0 + 0x30) = 0;                    /* start_lba = 0 */
	*(volatile uint16_t *)(part0 + 0x38) = 0;                    /* unit = 0 */
	*(volatile uint8_t *)(part0 + 0x3a) = 0;                     /* lun = 0 */
	*(volatile uint8_t *)(part0 + 0x3b) = 0;                     /* flags = 0 (creates block node) */

	con_hex2("RDWBLOCK", "pa=", (uint32_t)rdw_pa, "buf_pa=", (uint32_t)ufs_buf_pa);

	uint64_t ufs_dma_desc_pa = ufs_buf_pa + ufs_buf_size;
	uint64_t ufs_dma_desc_size = 0x4000ULL; /* 16 KB for UTRL/UTMRL/UCD */

	uint64_t ramdisk_pa = ufs_dma_desc_pa + ufs_dma_desc_size;
	uint64_t ramdisk_size = 0x100000ULL; /* 1 MB */

	/* Copy launchd binary into the RAMDisk so mockfs mounts it as /sbin/launchd */
	uint64_t launchd_sz = (uint64_t)(_binary_test_launchd_bin_end - _binary_test_launchd_bin_start);
	volatile uint8_t *rd_ptr = (volatile uint8_t *)ramdisk_pa;
	for (uint64_t i = 0; i < ramdisk_size; i++) rd_ptr[i] = 0;
	for (uint64_t i = 0; i < launchd_sz && i < ramdisk_size; i++) rd_ptr[i] = _binary_test_launchd_bin_start[i];
	con_hex2("RAMDISK", "pa=", (uint32_t)ramdisk_pa, "launchd_sz=", (uint32_t)launchd_sz);

	uint64_t dt_pa = (ramdisk_pa + ramdisk_size + 0x3fffULL) & ~0x3fffULL;

	/* 5. Build Apple Device Tree */
	struct dt_writer w = { .base = (uint8_t *)dt_pa, .cur = (uint8_t *)dt_pa, .depth = 0 };
	uint64_t cntfrq = rsr("cntfrq_el0");

	uint64_t io_base = qemu ? 0x08000000ULL : 0x10000000ULL;
	uint64_t io_size = qemu ? 0x08000000ULL : 0x10000000ULL;
	uint64_t gic_dist = qemu ? 0x08000000ULL : 0x10400000ULL;
	uint64_t gic_redist = qemu ? 0x080A0000ULL : 0x10440000ULL;
	uint64_t uart_addr = qemu ? 0x09000000ULL : 0ULL;

	uint64_t ranges[3] = { 0, io_base, io_size };
	uint8_t seed[64];
	for (unsigned i = 0; i < sizeof(seed); i++) seed[i] = (uint8_t)(i * 37 + 0x42);

	dt_begin(&w, "device-tree");
	static const char root_compat[] = "D22AP\0iPhone10,3\0xiaomi,sweet\0qcom,sm7150\0AppleARM\0";
	dt_prop(&w, "compatible", root_compat, sizeof(root_compat));
	dt_prop_str(&w, "model", "iPhone10,3");
	dt_prop_str(&w, "target-type", "D22");
	dt_prop_str(&w, "target-sub-type", "D22AP");
	dt_prop_str(&w, "manufacturer", "Apple Inc.");
	dt_prop_str(&w, "platform-name", "t8015");
	dt_prop_padded(&w, "serial-number", "FELIX00PIXELFOLD", 32);
	dt_prop_padded(&w, "mlb-serial-number", "FELIX00PIXELFOLDMLB", 32);
	dt_prop_padded(&w, "model-number", "MQAC2", 32);
	dt_prop_padded(&w, "region-info", "LL/A", 32);
	dt_prop_padded(&w, "regulatory-model-number", "A1865", 32);
	dt_prop_u32(&w, "#address-cells", 2);
	dt_prop_u32(&w, "#size-cells", 2);

	const char *cmd = qemu ? "-v debug=0x8 serial=3 cpus=1" :
		"-v debug=0xc serial=3 cpus=1 keepsyms=1 -enable_kprintf_spam -noprogress wdt=-1 wdt_test=no_user_checkin sweetusb=0 sweetlog=0 rd=ufsroot0 ufsrw=0 rootrw=0 unentitled_ios_sim_launch=1 ignition_level=1 launchd_unsecure_cache=1";

	dt_begin(&w, "chosen");
	dt_prop_str(&w, "boot-args", cmd);
	dt_prop(&w, "random-seed", seed, sizeof(seed));
	dt_prop_u64(&w, "dram-base", phys_base);
	dt_prop_u64(&w, "dram-size", win_size);
	dt_prop_u32(&w, "debug-enabled", 1);
	dt_prop_u32(&w, "display-scale", 3);
	dt_prop_u32(&w, "display-rotation", 0);
	dt_prop_str(&w, "psci-method", qemu ? "hvc" : "smc");

	static const uint8_t manifest_hash[48] = {
		0x5c, 0xbe, 0x36, 0x01, 0x1d, 0x7d, 0x83, 0x15, 0x15, 0xcd, 0xf1, 0x12, 0x2a, 0x93, 0xff, 0xca,
		0xd4, 0x8b, 0x05, 0xe9, 0x19, 0x43, 0x88, 0xad, 0x12, 0xe9, 0x75, 0x2a, 0x1a, 0xe4, 0xa5, 0xa0,
		0xb9, 0x34, 0x00, 0x8b, 0x37, 0x98, 0x53, 0x01, 0x04, 0x1d, 0x7e, 0x08, 0x6f, 0xff, 0x52, 0x05
	};
	static const uint8_t no_behavior[16];
	dt_prop_u64(&w, "unique-chip-id", 0x000052445730001ULL);
	dt_prop_u32(&w, "chip-id", 0x8015);
	dt_prop_u32(&w, "board-id", 0x06);
	dt_prop(&w, "boot-manifest-hash", manifest_hash, sizeof(manifest_hash));
	dt_prop(&w, "software-behavior", no_behavior, sizeof(no_behavior));
	dt_prop_str(&w, "crypto-hash-method", "sha2-384");
	dt_prop_str(&w, "firmware-version", "felix-shim");
	dt_prop_str(&w, "system-firmware-version", "felix-shim");
	dt_prop_u32(&w, "certificate-production-status", 0);
	dt_prop_u32(&w, "certificate-security-mode", 1);
	dt_prop_u32(&w, "effective-production-status-ap", 0);
	dt_prop_u32(&w, "effective-security-mode-ap", 1);
	dt_prop_u32(&w, "effective-security-mode-sep", 1);
	dt_prop_u32(&w, "secure-boot", 0);
	dt_prop_u32(&w, "production-cert", 0);
	dt_prop_u32(&w, "development-cert", 1);
	dt_prop_u32(&w, "embedded-panic-log-size", 0x100000);

	static uint8_t nvram_image[NVRAM_BANK_SIZE];
	build_nvram_image(nvram_image);
	dt_prop_u32(&w, "nvram-bank-size", NVRAM_BANK_SIZE);
	dt_prop_u32(&w, "nvram-bank-count", 1);
	dt_prop_u32(&w, "nvram-current-bank", 0);
	dt_prop(&w, "nvram-proxy-data", nvram_image, sizeof(nvram_image));

	dt_begin(&w, "asmb");
	dt_prop_u64(&w, "lp-sip0", 0x6fULL);
	dt_prop_u32(&w, "lp-sip1", 1);
	dt_end(&w);

	dt_begin(&w, "memory-map");
	uint64_t ramdisk_range[2] = { ramdisk_pa, ramdisk_size };
	dt_prop(&w, "RAMDisk", ramdisk_range, sizeof(ramdisk_range));
	dt_end(&w);
	dt_end(&w); /* chosen */

	dt_begin(&w, "defaults");
	dt_prop_u32(&w, "kern.vm_compressor", 2);
	dt_prop_u32(&w, "no-effaceable-storage", 1);
	dt_end(&w);

	dt_begin(&w, "buttons");
	dt_prop_u32(&w, "home-button-type", 2);
	dt_prop_u32(&w, "opposed-power-vol-buttons", 1);
	dt_end(&w);

	if (!qemu) {
		dt_begin(&w, "sweet-ufs");
		dt_prop_str(&w, "compatible", "google,gs101-ufs");
		uint64_t ufs_reg[2] = { 0x14700000ULL, 0x3000ULL };
		dt_prop(&w, "reg", ufs_reg, sizeof(ufs_reg));
		dt_end(&w);

		dt_begin(&w, "sweet-fbcon");
		uint64_t fb_reg[2] = { FB_OUTER_BASE, (uint64_t)FB_H * (uint64_t)FB_STRIDE };
		dt_prop(&w, "reg", fb_reg, sizeof(fb_reg));
		dt_prop_u32(&w, "width", FB_W);
		dt_prop_u32(&w, "height", FB_H);
		dt_prop_u32(&w, "stride", FB_STRIDE);
		dt_end(&w);
	}

	dt_begin(&w, "product");
	dt_identity(&w, identity_product, sizeof(identity_product) / sizeof(identity_product[0]), identity_product_data);
	dt_end(&w);

	dt_begin(&w, "options");
	dt_end(&w);

	/* CPU cores */
	dt_begin(&w, "cpus");
	dt_prop_u32(&w, "#address-cells", 1);
	dt_prop_u32(&w, "#size-cells", 0);
	for (int i = 0; i < (qemu ? 1 : 8); i++) {
		char name[5] = { 'c', 'p', 'u', (char)('0' + i), 0 };
		dt_begin(&w, name);
		dt_prop_str(&w, "device_type", "cpu");
		static const char cpu_compat[] = "qcom,kryo470\0ARM,v8\0";
		dt_prop(&w, "compatible", cpu_compat, sizeof(cpu_compat));
		dt_prop_str(&w, "state", i == 0 ? "running" : "stopped");
		uint32_t reg_val = (uint32_t)(i << 8);
		dt_prop_u32(&w, "reg", reg_val);
		dt_prop_u32(&w, "cpu-id", (uint32_t)i);
		dt_prop_u32(&w, "cluster-id", 0);
		dt_prop_u32(&w, "cluster-type", 0);
		dt_prop_u32(&w, "cluster-core-id", (uint32_t)i);
		dt_prop_u32(&w, "l2-cache-id", (uint32_t)i);
		dt_prop_u32(&w, "l3-cache-id", 0);
		dt_prop_u32(&w, "timebase-frequency", (uint32_t)cntfrq);
		dt_prop_u32(&w, "fixed-frequency", (uint32_t)cntfrq);
		dt_prop_u32(&w, "clock-frequency", (uint32_t)(i >= 6 ? 2850000000U : (i >= 4 ? 2350000000U : 1800000000U)));
		dt_prop_u32(&w, "bus-frequency", 100000000U);
		dt_prop_u32(&w, "memory-frequency", 1866000000U);
		dt_prop_u32(&w, "peripheral-frequency", (uint32_t)cntfrq);
		dt_end(&w);
	}
	dt_end(&w); /* cpus */

	/* arm-io */
	dt_begin(&w, "arm-io");
	dt_prop_str(&w, "device_type", qemu ? "qemu-virt" : "felix");
	dt_prop(&w, "ranges", ranges, sizeof(ranges));
	dt_prop_u32(&w, "#address-cells", 2);
	dt_prop_u32(&w, "#size-cells", 2);

	/* wdt */
	dt_begin(&w, "wdt");
	dt_prop_u32(&w, "no-pmu", 1);
	dt_end(&w);

	/* GICv3 */
	dt_begin(&w, "gic");
	dt_prop_str(&w, "device_type", "interrupt-controller");
	dt_prop_str(&w, "compatible", "arm,gic-v3");
	dt_prop(&w, "interrupt-controller", NULL, 0);
	dt_prop_u32(&w, "virtual-timer-intid", qemu ? 27 : 27); /* Tensor G2 arch virtual timer PPI 11 = INTID 27 */
	uint64_t gic_reg[4] = { gic_dist - io_base, 0x10000, gic_redist - io_base, 0x100000 };
	dt_prop(&w, "reg", gic_reg, sizeof(gic_reg));
	dt_end(&w);

	if (uart_addr != 0) {
		dt_begin(&w, "uart0");
		dt_prop_str(&w, "device_type", "uart");
		dt_prop_str(&w, "compatible", "arm,pl011");
		uint64_t reg2[2] = { uart_addr - io_base, 0x1000 };
		dt_prop(&w, "reg", reg2, sizeof(reg2));
		dt_prop_u32(&w, "boot-console", 1);
		dt_end(&w);
	}

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
	ba->virtBase = virt_base;
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
	ba->deviceTreeP = virt_base + (dt_pa - phys_base); /* EXACT virtual address! */
	ba->deviceTreeLength = dt_len;

	memcpy(ba->commandLine, cmd, str_len(cmd) + 1);
	con_str("BOOTARGS", ba->commandLine);
	decon_flush();

	/* 7. Patch "XNULDRST" early exception slot */
	uint64_t target_chkpt = 0; /* 0: run full kernel! */
	uint8_t *p = (uint8_t *)kern_lo;
	int patched = 0;
	while (p + 16 <= (uint8_t *)kern_hi) {
		if (p[0] == 'X' && p[1] == 'N' && p[2] == 'U' && p[3] == 'L' &&
		    p[4] == 'D' && p[5] == 'R' && p[6] == 'S' && p[7] == 'T') {
			uint64_t *slot = (uint64_t *)p;
			slot[1] = (uint64_t)&loader_return_stub;
			slot[2] = target_chkpt;
			slot[3] = slide;
			patched = 1;
			break;
		}
		p += 8;
	}

	con_hex2("STUB", "patched=", patched, "chkpt=", (uint32_t)target_chkpt);
	con_hex("ENTRY", entry_pa);

	g_rdw_pa = rdw_pa;
	g_ufs_buf_pa = ufs_buf_pa;
	g_ufs_dma_desc_pa = ufs_dma_desc_pa;

	/* Initialize UFS Host Controller & PHY */
	con_str("UFS", "Starting hardware cold-init...");
	int ufs_rc = ufs_cold_init();
	con_hex2("UFS", "cold_init rc=", ufs_rc, "hcs=", *(volatile uint32_t *)0x14700030UL);

	if (ufs_rc == 0) {
		ufs_init_host_dma(ufs_dma_desc_pa);
		g_ufs_hardware_ready = 1;
		con_str("UFS", "Host DMA and link operational!");

		/* 1. Set fDeviceInit flag via Slot 0 Query Request */
		int64_t fdev_rc = ufs_query_set_fdeviceinit(ufs_dma_desc_pa);
		con_hex2("UFS FDEV SET", "rc=", (uint32_t)fdev_rc, "", 0);

		/* 2. Poll until fDeviceInit reads 0 (device internal tables loaded) */
		int64_t poll_rc = ufs_query_poll_fdeviceinit(ufs_dma_desc_pa);
		con_hex2("UFS FDEV POLL", "rc=", (uint32_t)poll_rc, "", 0);

		/* 3. Send START STOP UNIT to transition LUN 0 to ACTIVE power state */
		int64_t pwr_rc = ufs_scsi_start_stop(ufs_dma_desc_pa, 0, 1);
		con_hex2("UFS PWR ACTIVE", "rc=", (uint32_t)pwr_rc, "", 0);

		/* 4. Request Sense to clear Unit Attention */
		static uint8_t sense_buf[64] __attribute__((aligned(64)));
		ufs_scsi_request_sense(ufs_dma_desc_pa, 0, sense_buf);

		/* 5. Clear Unit Attention with TUR */
		for (int i = 0; i < 3; i++) {
			int64_t tur_rc = ufs_scsi_tur(ufs_dma_desc_pa, 0);
			con_hex2("UFS TUR", "try=", i, "rc=", (uint32_t)tur_rc);
			if (tur_rc == 0) break;
			delay_ms(10);
		}

		/* Read GPT header from LBA 1 to find userdata start LBA */
		static uint8_t gpt_buf[4096];
		int64_t gpt_rc = ufs_scsi_read(ufs_dma_desc_pa, 0, 1, 1, gpt_buf);
		con_hex2("UFS GPT", "rc=", (uint32_t)gpt_rc, "magic=", *(uint32_t *)gpt_buf);
		if (gpt_rc == 0 && *(uint32_t *)gpt_buf == 0x20494645) { /* 'EFI ' */
			uint64_t part_lba = *(uint64_t *)(gpt_buf + 0x48);
			uint32_t num_parts = *(uint32_t *)(gpt_buf + 0x50);
			uint32_t part_size = *(uint32_t *)(gpt_buf + 0x54);
			con_hex2("GPT PARTS", "start_lba=", (uint32_t)part_lba, "count=", num_parts);

			for (uint32_t p = 0; p < num_parts && p < 128; p++) {
				uint64_t ent_block = part_lba + (p * part_size) / 4096;
				uint32_t ent_off = (p * part_size) % 4096;
				static uint8_t ent_buf[4096];
				if (ufs_scsi_read(ufs_dma_desc_pa, 0, ent_block, 1, ent_buf) == 0) {
					uint8_t *ent = ent_buf + ent_off;
					uint16_t *pname16 = (uint16_t *)(ent + 0x38);
					if (pname16[0] == 'u' && pname16[1] == 's' && pname16[2] == 'e' &&
					    pname16[3] == 'r' && pname16[4] == 'd' && pname16[5] == 'a' &&
					    pname16[6] == 't' && pname16[7] == 'a') {
						g_userdata_start_lba = *(uint64_t *)(ent + 0x20);
						uint64_t end_lba = *(uint64_t *)(ent + 0x28);
						con_hex2("USERDATA", "start_lba=", (uint32_t)g_userdata_start_lba, "end_lba=", (uint32_t)end_lba);
						/* Update part0 block count with actual partition size */
						if (end_lba > g_userdata_start_lba) {
							*(volatile uint64_t *)(part0 + 0x28) = (end_lba - g_userdata_start_lba + 1);
						}
						break;
					}
				}
			}
		}
	}

	/*
	 * In honest UFS mode, XNU sweet_ufs directly drives the host controller.
	 * Tensor G2 (Exynos UFS) requires Vendor Specific register (VS + 0x40 = 0x14701140)
	 * to configure which slots accept SCSI vs Device Management.
	 * In loader we set slot 0 to Device Management, but XNU sweet_ufs issues SCSI
	 * commands (TUR 0x00, READ 0x28) exclusively on SLOT 0!
	 * Setting 0x14701140 to 0xFFFFFFFF makes slot 0 accept SCSI commands!
	 */
	if (!qemu) {
		*(volatile uint32_t *)(0x14701100UL + 0x40) = 0xFFFFFFFFu; /* All slots = SCSI */
		*(volatile uint32_t *)(0x14700000UL + 0x24) = 0;           /* REG_IE = 0 */
		*(volatile uint32_t *)(0x14700000UL + 0x58) = 0;           /* REG_UTRLDBR = 0 */
		*(volatile uint32_t *)(0x14700000UL + 0x20) = *(volatile uint32_t *)(0x14700000UL + 0x20); /* ack REG_IS */
	}

	con_str("READY", "Jumping to XNU kernel in 3s...");

	/* Clean caches across the entire loaded region */
	clean_dcache(kern_lo, top);

	delay_ms(3000);

	/* Re-arm DECON continuous HW trigger (0x3061) so XNU screen console updates continuously */
	decon_rearm_continuous();

	/* Jump to XNU! */
	jump_to_kernel(entry_pa, ba_pa);
}
