/*
 * The Exynos 9810 phones' low bank (0x80000000-0xFFFFFFFF), where the window is not (it is the bank from 0x880000000):
 * where its free RAM may be XNU's as well (extra_ram_claim). Every range the device tree reserves there, every range the
 * kernel's drivers take for themselves, and the platform's keep-outs are outside these (devices/starlte/README.md,
 * Memory layout; scion/xnu-8796:bsd/sys/hakios.exynos9810_mem.h):
 *   0x80000000 + 2 MB          sec_debug_magic (4 KB) and a margin
 *   0x8F000000 - 0x97FFFFFF    the UEFI image, its stack, the DXE heap, and 80 MB more left to UEFI's allocator, which
 *                              stays alive beside the kernel (zswap_debug, 0x91300000, is in it)
 *   0xAF800000 + 32 MB         rkp_region
 *   0xB8000000 + 2 MB          tima_region
 *   0xC0000000 + 512 KB        seclog_mem
 *   0xCC000000 - 0xE36FFFFF    the panel's buffer; the moved payload and the panic log, the display's, the cameras', UFS's
 *                              and Wi-Fi's ranges (0xD0000000-0xE18FFFFF: hakios.exynos9810_mem.h); secure_camera
 *   0xF0000000 -               the CP's, ABOX's (keep_out), exynos_ss, fimc_is_lib
 * Within them only what UEFI's memory map calls free is taken: the secure holes (Silicium's RamManagerLib takes them out of
 * the map), the loader's image at 0xA1000000 and whatever UEFI has handed out stay where they are.
 */
static const uint64_t exynos9810_extra_ram[][2] = {
	{ 0x80200000, 0x8F000000 }, { 0x98000000, 0xAF800000 }, { 0xB1800000, 0xB8000000 }, { 0xB8200000, 0xC0000000 },
	{ 0xC0080000, 0xCC000000 }, { 0xE3700000, 0xF0000000 },
	/* The main window has already been claimed and is no longer ConventionalMemory; collect any other high-bank pieces. */
	{ 0x880000000, 0xA00000000 }, { 0, 0 },
};

static const struct platform platform_starlte = {
	/*
	 * Samsung Galaxy S9 (SM-G960F), Exynos 9810, under Project Silicium's starlte UEFI. Every value is from
	 * the phone's own device tree (devices/starlte/README.md): the SoC's registers from 0x10000000, a GIC-400
	 * (distributor 0x10101000, CPU interface 0x10102000), PSCI by SMC, the panel's buffer at 0xCC000000.
	 */
	.name = "starlte", .io_base = 0x10000000, .io_size = 0x10000000,
	.gic_dist = 0x10101000, .gic_cpu = 0x10102000, .uart = 0,
	/*
	 * Cores 0-3 are Cortex-A55s (MPIDR 0x000-0x003, 1.8 GHz at most), 4-7 Samsung's own Exynos M3s (0x100-0x103,
	 * 2.7 GHz), started by Carbon's PSCI. The M3s implement ARMv8.0 where the A55s implement 8.2: a kernel that is
	 * to use both boots on an M3 (bootcpu=4), so that what it says the processor can do is what every core can
	 * (devices/starlte/README.md).
	 */
	.cores = 8, .core_mpidr = (const uint32_t[]){ 0x000, 0x001, 0x002, 0x003, 0x100, 0x101, 0x102, 0x103 },
	.small_cores = 0x0f,
	/*
	 * The Exynos 9810's EL3 leaves CNTFRQ_EL0 at 0 (Silicium's S5E9810Pkg: HAS_CNTFRQ_EL0_EMPTY_ERRATA, the frequency
	 * given instead as 26 MHz; Samsung's device tree: the timer's clock-frequency 26000000), and only EL3 can write it.
	 */
	.timer_hz = 26000000,
	.fb_base = 0xCC000000,
	/* the ALIVE GPIO block's GPA0 data register, pin 4: Volume Down (drivers/input/hakios.exynos_keys.c reads the same pin) */
	.vol_down_reg = 0x14050000 + 0x020 + 0x04, .vol_down_bit = 4,
	/*
	 * Samsung's pstore (exynos-snapshot's log_pstore, 0xFED10000 + 32 KB), its second half: the pmsg zone, where the
	 * WoA port for the S9+ kept its notes across restarts and read them back in TWRP. Reserved in this port's UEFI
	 * memory map (devices/starlte/uefi).
	 */
	.ram_log = 0xFED14000, .ram_log_size = 0x4000,
	/*
	 * Its first half: ramoops' one dmesg record (Samsung sets record_size and pmsg_size to half of log_pstore each,
	 * the dmesg records coming first). The start of the kernel's log goes there.
	 */
	.ram_dump = 0xFED10000, .ram_dump_size = 0x4000,
	/*
	 * With the kernel on a core of its own (bootcpu=, the S9's default), this core watches it and writes the log on the
	 * way, and when the kernel hangs: into the RAM log, which survives the restart that follows.
	 */
	.log_other_cpu = 1,
	/*
	 * The PMU (system-controller@14060000 in the phone's device tree): its INFORM registers tell S-Boot where a restart
	 * is to go, and a run's restart goes to recovery, where its log is read (recovery_after_reset).
	 */
	.recovery_pmu = 0x14060000,
	/*
	 * The data volume (/private/var kept between runs, ufs_write=1 var_hfs=ufs_USERDATA_data): this phone has no
	 * darwin, and Android's USERDATA, given up for it, is the only partition a write may reach, the whole of it.
	 */
	.data_part = "USERDATA",
	/*
	 * The restart: the PMU's SWRESET (+0x400), 1 written, as Samsung's own kernel restarts the phone (exynos-reboot.c,
	 * exynos_reboot(), after sec_reboot() has set INFORM2/3). Only that kind of reset keeps the INFORM registers: S-Boot
	 * reads them as 0 after the watchdog's (platform_restart).
	 */
	.pmu_swreset = 0x400,
	/*
	 * Cluster 1's watchdog (watchdog_cl1@10060000, PMU bit 23), armed for the whole run as the mainline Linux port to the
	 * S9+ arms it: a hang of the whole SoC becomes a restart (exynos_wdt_start; wdt=0: not), into BOOT on this S9, as its
	 * reset clears INFORM3 (platform_restart). Cluster 0's (watchdog_cl0@10050000), which S-Boot also leaves running, is
	 * stopped.
	 */
	.exynos_wdt = 0x10060000, .exynos_wdt_other = 0x10050000, .exynos_wdt_pmu_bit = 23,
	/*
	 * S-Boot starts core 1 for its own display work (its log: "[boot_exynos] Powers up core(1)", then lines headed [1:) and
	 * has it off again by the hand-over (the PMU's CLUSTER0 core 1 STATUS 0x880000, as core 3's before its CPU_ON). The
	 * first run on this phone (29 October 2026) started the kernel on core 1, got nothing back, and took it for a core that
	 * never comes; that kernel had no patch 1504 yet, and read core 1's MPIDR (0x81000100) as an M3's number. With it, CPU_ON
	 * for core 1 answers 0 and the core is in the kernel 1.1 ms later (seen 3 November 2026). core1=1 lists it; without
	 * that it is left out, as it has always been. AFFINITY_INFO answers 0 ("on") of every core (Carbon's psci_affinity_info
	 * is a stub): that answer is not taken as a no.
	 */
	.parked_cores = 0x2,
	.affinity_unreliable = 1,
	/*
	 * The Shannon CP's memory (Samsung's device tree, reserved-memory: modem_if 0xF0000000 + 0x9200000, cp_ram_logging
	 * 0xF9200000 + 0x2000000), whose window EL3 fixes at cold boot, and ABOX's (abox_rmem 0xFB200000 + 0x2800000, for
	 * calliope): taken from UEFI's allocator at the start and never in XNU's window (choose_window). The payload lands
	 * at 0xF0000000 (the UEFI image's boot manager finds loader.efi there), so it is moved out first, to 0xD0000000:
	 * between the panel's buffer (0xCC000000 + 0x1043000) and secure_camera (0xE1900000), where the device tree reserves
	 * nothing (docs: devices/starlte, docs/modem-plan.md). The CP is not running while the loader moves it: nothing
	 * starts the modem before XNU (S-Boot does not; Android's cbd would).
	 */
	.payload_home = 0xD0000000,
	.keep_out = { { 0xF0000000, 0x0B200000, "CP: modem_if, cp_ram_logging" }, { 0xFB200000, 0x02800000, "ABOX: abox_rmem" } },
	/*
	 * The low bank's free RAM, about 900 MB, is the kernel's too (beside its 2 GB window from 0x880000000), in the ranges
	 * above; extra_ram=0 among the kernel's arguments: not.
	 */
	.extra_ram = exynos9810_extra_ram,
};
