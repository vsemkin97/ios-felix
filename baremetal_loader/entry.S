/*
 * entry.S: Standard Linux ARM64 Header, exception vectors, and low-level helpers
 * for HackiOS Bare-Metal Loader on Google Pixel Fold (felix, Tensor G2)
 */

	.section .text.head, "ax"
	.globl _head
_head:
	b	_start			/* code0 */
	.long	0			/* code1 */
	.quad	0			/* text_offset */
	.quad	_end			/* image_size */
	.quad	0xa			/* flags: LE, 4K pages, place anywhere */
	.quad	0			/* res2 */
	.quad	0			/* res3 */
	.quad	0			/* res4 */
	.ascii	"ARM\x64"		/* magic */
	.long	0			/* res5 (PE offset) */

	.text
	.globl _start
_start:
	msr	daifset, #0xf		/* Disable all interrupts */
	mov	x19, x0			/* Preserve DTB pointer from ABL */

	/* Check Current Exception Level */
	mrs	x21, CurrentEL
	lsr	x21, x21, #2
	cmp	x21, #2
	b.ne	.L_is_el1

	/*
	 * Running at EL2 (Google Tensor ABL entry or QEMU virt EL2).
	 * Configure EL2 system registers and drop to EL1h before running anything else.
	 */
	adrp	x1, vectors
	add	x1, x1, :lo12:vectors
	msr	vbar_el2, x1
	msr	vbar_el1, x1

	/* HCR_EL2: Set RW bit (bit 31) -> EL1 is AArch64; AMO/IMO/FMO=0 */
	mov	x2, #(1 << 31)
	msr	hcr_el2, x2

	/* Pass through CPU IDs */
	mrs	x2, midr_el1
	msr	vpidr_el2, x2
	mrs	x2, mpidr_el1
	msr	vmpidr_el2, x2

	/*
	 * SCTLR_EL1: ARMv8.2 RES1 bits: 11, 20, 22, 23, 28, 29 (0x30d00800).
	 * MMU, D-cache, I-cache disabled.
	 */
	mov	x2, #0x0800
	movk	x2, #0x30d0, lsl #16
	msr	sctlr_el1, x2

	/* Disable FP/SIMD traps to EL2 */
	msr	cptr_el2, xzr

	/* Disable CP15 traps to EL2 */
	msr	hstr_el2, xzr

	/* CNTHCTL_EL2: Enable EL1 physical timer and counter access (EL1PCTEN | EL1PCEN) */
	mov	x2, #3
	msr	cnthctl_el2, x2
	msr	cntvoff_el2, xzr
	msr	vttbr_el2, xzr

	/* ICC_SRE_EL2: Enable GICv3 system registers at EL1 (SRE bit 0 | Enable bit 3 = 9) */
	mrs	x2, icc_sre_el2
	mov	x3, #9
	orr	x2, x2, x3
	msr	icc_sre_el2, x2
	isb

	/* Drop to EL1h at .L_is_el1 */
	adrp	x2, .L_is_el1
	add	x2, x2, :lo12:.L_is_el1
	msr	elr_el2, x2
	mov	x2, #0x3c5		/* EL1h, DAIF masked */
	msr	spsr_el2, x2
	mov	x0, x19
	eret

.L_is_el1:
	/* Setup stack pointer FIRST at EL1h */
	msr	spsel, #1
	adrp	x1, _stack_top
	add	x1, x1, :lo12:_stack_top
	mov	sp, x1

	/* Setup exception vector base FIRST so all faults are caught cleanly */
	adrp	x1, vectors
	add	x1, x1, :lo12:vectors
	msr	vbar_el1, x1
	isb

	/* Zero .bss */
	adrp	x1, _bss_start
	add	x1, x1, :lo12:_bss_start
	adrp	x2, _bss_end
	add	x2, x2, :lo12:_bss_end
1:	cmp	x1, x2
	b.hs	2f
	str	xzr, [x1], #8
	b	1b

2:
	/* Call C entry point with preserved DTB pointer in x0 */
	mov	x0, x19
	bl	felix_baremetal_main

hang:
	wfe
	b	hang

/*
 * Secondary CPU entry point for PSCI CPU_ON.
 * Core 1 starts here, sets up stack, and enters the mailbox responder loop.
 */
	.balign	0x800
	.globl secondary_entry
secondary_entry:
	msr	daifset, #0xf		/* Disable interrupts */

	mrs	x21, CurrentEL
	lsr	x21, x21, #2
	cmp	x21, #2
	b.ne	.L_sec_el1

	/* At EL2: drop to EL1 */
	mov	x2, #(1 << 31)
	msr	hcr_el2, x2
	mov	x2, #3
	msr	cnthctl_el2, x2
	msr	cptr_el2, xzr
	msr	hstr_el2, xzr
	adrp	x2, .L_sec_el1
	add	x2, x2, :lo12:.L_sec_el1
	msr	elr_el2, x2
	mov	x2, #0x3c5		/* EL1h */
	msr	spsr_el2, x2
	eret

.L_sec_el1:
	msr	spsel, #1
	adrp	x1, _sec_stacks
	add	x1, x1, :lo12:_sec_stacks
	add	x1, x1, #0x2000		/* 8KB stack for Core 1 */
	mov	sp, x1

	bl	secondary_core_main

.L_sec_hang:
	wfe
	b	.L_sec_hang


/*
 * Exception vectors. Each slot stashes x29/x30, loads its slot number
 * and jumps to the common path, which saves x0-x28 and calls exc_handler(slot, regs).
 */
.macro vent slot
	.balign	0x80
	stp	x29, x30, [sp, #-16]!
	mov	x29, #\slot
	b	exc_common
.endm

	.balign	0x800
	.globl vectors
vectors:
	vent 0
	vent 1
	vent 2
	vent 3
	vent 4
	vent 5
	vent 6
	vent 7
	vent 8
	vent 9
	vent 10
	vent 11
	vent 12
	vent 13
	vent 14
	vent 15

exc_common:
	sub	sp, sp, #(30 * 8)
	stp	x0, x1, [sp, #0]
	stp	x2, x3, [sp, #16]
	stp	x4, x5, [sp, #32]
	stp	x6, x7, [sp, #48]
	stp	x8, x9, [sp, #64]
	stp	x10, x11, [sp, #80]
	stp	x12, x13, [sp, #96]
	stp	x14, x15, [sp, #112]
	stp	x16, x17, [sp, #128]
	stp	x18, x19, [sp, #144]
	stp	x20, x21, [sp, #160]
	stp	x22, x23, [sp, #176]
	stp	x24, x25, [sp, #192]
	stp	x26, x27, [sp, #208]
	str	x28, [sp, #224]
	mov	x0, x29
	mov	x1, sp
	bl	exc_handler
	ldp	x0, x1, [sp, #0]
	ldp	x2, x3, [sp, #16]
	ldp	x4, x5, [sp, #32]
	ldp	x6, x7, [sp, #48]
	ldp	x8, x9, [sp, #64]
	ldp	x10, x11, [sp, #80]
	ldp	x12, x13, [sp, #96]
	ldp	x14, x15, [sp, #112]
	ldp	x16, x17, [sp, #128]
	ldp	x18, x19, [sp, #144]
	ldp	x20, x21, [sp, #160]
	ldp	x22, x23, [sp, #176]
	ldp	x24, x25, [sp, #192]
	ldp	x26, x27, [sp, #208]
	ldr	x28, [sp, #224]
	add	sp, sp, #(30 * 8)
	ldp	x29, x30, [sp], #16
	eret

/*
 * jump_to_kernel(uint64_t entry_pa, uint64_t boot_args_pa)
 * Disables MMU, D-cache, and I-cache, and jumps to XNU kernel entry.
 */
.globl jump_to_kernel
jump_to_kernel:
	msr	daifset, #0xf
	/* Clean stack area before turning off D-cache */
	mov	x2, sp
	sub	x2, x2, #0x400
	bic	x2, x2, #63
	add	x3, x2, #0x4000
1:
	dc	civac, x2
	add	x2, x2, #64
	cmp	x2, x3
	b.lo	1b
	dsb	sy

	/* Turn off MMU, D-cache, and I-cache in SCTLR_EL1 */
	mrs	x2, sctlr_el1
	bic	x2, x2, #0x1        /* MMU off */
	bic	x2, x2, #0x4        /* D-cache off */
	bic	x2, x2, #0x1000     /* I-cache off */
	msr	sctlr_el1, x2
	isb
	ic	iallu
	dsb	sy
	isb
	tlbi	vmalle1
	dsb	sy
	isb

	/* Put boot_args_pa in x0 and branch to kernel entry point */
	mov	x19, x0             /* entry_pa */
	mov	x0, x1              /* boot_args_pa */
	br	x19

/*
 * clean_dcache(uint64_t start, uint64_t end)
 */
.globl clean_dcache
clean_dcache:
	bic	x0, x0, #63
1:
	dc	civac, x0
	add	x0, x0, #64
	cmp	x0, x1
	b.lo	1b
	dsb	sy
	ret

/*
 * loader_return_stub:
 * Called from XNU's early exception handler (Lloader_early_exception in start.s).
 */
.globl loader_return_stub
loader_return_stub:
	msr	daifset, #0xf

	/* Disable MMU and caches: restore clean physical addressing */
	mrs	x9, sctlr_el1
	bic	x9, x9, #0x1        /* MMU off */
	bic	x9, x9, #0x4        /* D-cache off */
	bic	x9, x9, #0x1000     /* I-cache off */
	msr	sctlr_el1, x9
	isb
	ic	iallu
	dsb	sy
	isb

	/* Setup stack pointer at physical address */
	adrp	x9, _stack_top
	add	x9, x9, :lo12:_stack_top
	mov	sp, x9
	b	on_xnu_exception

