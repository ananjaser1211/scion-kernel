/*
 * The Galaxy S9+ (SM-G965F, star2lte) and the Galaxy Note9 (SM-N960F, crownlte): the S9's SoC, board and memory map. Every
 * address above is the same in their device trees (Samsung's BSP, exynos9810-star2lte_eur_open_26.dts and
 * exynos9810-crownlte_eur_open_26.dts: the GIC, the timer, the PMU, both cluster watchdogs, reserved-memory's modem_if,
 * cp_ram_logging, abox_rmem, exynos_ss and log_pstore, secure_camera), and the panel is 1440 x 2960 on all three, its buffer
 * at 0xCC000000 (the S9+'s seen there by the WoA port for it, ntdevlabs/exynos9810-woa; the Note9's not seen by anyone).
 * Their RAM (6 GB; the Note9 6 or 8 GB) is UEFI's to report: Silicium's RamManagerLib reads its size from what S-Boot
 * left at NS_IRAM + 0x848, and the kernel's window is the largest free block, at most MAX_MEM_SIZE. So only the name
 * differs, and with it what the drivers that look at /arm-io's device_type do (the MAX77705's battery). The framebuffer
 * cannot tell them from the S9: the device's own image says which it is (loader_platform=, devices/<name>/mkimage.sh),
 * and without that they run as the S9. Written, not seen: nobody has run this on an S9+ or a Note9.
 */
static const struct platform platform_star2lte = {
	.name = "star2lte", .io_base = 0x10000000, .io_size = 0x10000000,
	.gic_dist = 0x10101000, .gic_cpu = 0x10102000, .uart = 0,
	.cores = 8, .core_mpidr = (const uint32_t[]){ 0x000, 0x001, 0x002, 0x003, 0x100, 0x101, 0x102, 0x103 },
	.small_cores = 0x0f, .timer_hz = 26000000, .fb_base = 0xCC000000,
	.ram_log = 0xFED14000, .ram_log_size = 0x4000, .ram_dump = 0xFED10000, .ram_dump_size = 0x4000,
	.log_other_cpu = 1, .recovery_pmu = 0x14060000, .pmu_swreset = 0x400, .data_part = "USERDATA",
	.exynos_wdt = 0x10060000, .exynos_wdt_other = 0x10050000, .exynos_wdt_pmu_bit = 23,
	.parked_cores = 0x2, .affinity_unreliable = 1,
	.payload_home = 0xD0000000,
	.keep_out = { { 0xF0000000, 0x0B200000, "CP: modem_if, cp_ram_logging" }, { 0xFB200000, 0x02800000, "ABOX: abox_rmem" } },
	/* Same Exynos9810 low-bank map as StarLTE; extra_ram_claim() tests every page before exposing it to XNU. */
	.extra_ram = exynos9810_extra_ram,
	.sibling_of = &platform_starlte,
};
