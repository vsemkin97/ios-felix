/*
 * felix: Google Pixel Fold (Tensor G2 / GS201)
 * Platform description for xnuloader/loader.c.
 */
static const struct platform platform_felix = {
	.name = "felix",
	.io_base = 0x10000000, .io_size = 0x10000000,
	.gic_dist = 0x10400000, .gic_redist = 0x10440000,
	.uart = 0,
	.log_other_cpu = 1,
	.cores = 8, .core_shift = 8,
	.small_cores = 0x0f,
	.fb_base = 0xFC000000,
};
