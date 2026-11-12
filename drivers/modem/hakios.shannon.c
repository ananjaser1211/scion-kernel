/*
 * The Galaxy S9/S9+'s cellular modem: Samsung's Shannon, the "CP" inside the Exynos 9810, brought up one step at a time
 * (shannon=N, off unless asked). docs/cellular-star2lte.md is the plan and the evidence; this file is its step 1.
 *
 * What Android does on a cold boot: Samsung's cbd (vendor/bin/cbd, "cbd -d -tss310 -bm -mm -P .../RADIO -n
 * /mnt/vendor/efs", start_shannon310_boot) through /dev/umts_boot0, and Apollo's modem_v1 behind each ioctl
 * (docs/cellular-star2lte.md has the annotated sequence):
 *   1. waits for vendor.cbd.rfs_check_done (the RIL's RFS side validated NV), reads RADIO's TOC, opens
 *      /mnt/vendor/efs/nv_data.bin (creating it 0xFF-filled, the NV entry's size, if missing);
 *   2. IOCTL_MODEM_STATUS, IOCTL_MODEM_RESET (a no-op while the CP is offline);
 *   3. IOCTL_SECURITY_REQ(mode 2, 0, 0) "non-secure mode", with cbd's own CPU affinity taken off CPU 0: the kernel
 *      runs cal_cp_init first (CP_CTRL_NS |= PWRON, CP_CTRL_S |= CP_START, RESET_SEQUENCER_STATUS == 5), then
 *      SMC 0x82000700(2, 0, 0); CP_NOT_WORKING (0xb) is taken as success for mode 2;
 *   4. IOCTL_MODEM_XMIT_BOOT for TOC entries 1..4 (BOOT, MAIN, VSS once per boot, NV from the NV file), each
 *      in 0xf800-byte chunks to boot_base + (address & 0x3fffffff): BOOT at +0, MAIN +0x10000, VSS +0x7900000,
 *      NV +0x4900000. TOC and OFFSET are not sent;
 *   5. IOCTL_SECURITY_REQ(mode 0, BOOT's size, MAIN's size) "secure mode", off CPU 0: SMC 0x82000700(0 | base only if
 *      shm_ipc found a CP memory map at boot -- on a cold boot it has not, so 0, BOOT size, MAIN size);
 *   6. ds_detect; IOCTL_MODEM_ON (mailbox words cleared, AP status word, AP2CP_CFG = 1, 300 ms);
 *   7. IOCTL_MODEM_BOOT_ON (CP status word non-zero within 100 tries); IOCTL_MODEM_DL_START (the IPC map reset, the
 *      boot magic, AP status 1); the UDL handshake on umts_boot0 (stage done, then command 0xf);
 *      IOCTL_MODEM_BOOT_OFF (waits for the CP's init); IOCTL_MODEM_GET_CP_BOOTLOG.
 *
 * The steps here:
 *   shannon=1  read only: CP_CTRL_NS/_S through the read SMC, the four PMU words Samsung's exynos_print_cp_status reads
 *              at any time (CP_STAT, CENTRAL_SEQ_CP conf and status, RESET_SEQUENCER_STATUS), the mailbox's INTMR0 (the
 *              one word Linux reads at boot, mbox_request_irq, before the CP is on), then RADIO's table of contents and
 *              the CP's memory map (firmware headers, nothing of the phone's identity). Each MMIO read is said before it
 *              is made, so a fault names its register. shannon_regs=1 adds the CP SYS_PWR_REGs and the other mailbox
 *              words; shannon_peek=1 the first page of the CP's memory (what the boot loader left there).
 *              NOT read before the CP is initialised: AP2CP_CFG (0x14820000). It is in the CP's block, and a read with
 *              the CP never initialised is a synchronous external abort (build 35: panic in log_cp_registers, FAR at
 *              that page's offset 0). Samsung's kernel touches it only in ss310ap_on, after the security request has
 *              run cal_cp_init; so does this driver (step 3, RESET_SEQUENCER_STATUS == 5 and the verify call passed).
 *   shannon=2  ... and cbd's 3 to 5: cal_cp_init and SMC(2, 0, 0), the four images, SMC(0, BOOT, MAIN), both SMCs
 *              made off CPU 0 as cbd makes them; the answers logged by name. The CP is not started. shannon_nv=1 loads
 *              EFS:/nv_data.bin into NV (its size is logged, never its contents); without it NV is 0xFF, what cbd's
 *              create_empty_nv writes, and the CP is expected to rebuild its defaults.
 *   shannon=3  ... and cbd's 8 to 11: MODEM_ON (AP2CP_CFG), BOOT_ON (the CP's status), DL_START (the IPC map and its
 *              magic, AP status 1) and the UDL handshake (0x900d -> 0xa00d, 0x9f00 -> 0xaf00 over the RAW queues), then
 *              60 s of the CP's mailbox, each message logged with its command (PHONE_START, INIT_START, ...). Nothing
 *              is answered yet: PIF_INIT_DONE needs the SBD rings, the next step. shannon_udl_irq=1 also rings the
 *              mailbox after each UDL word (Linux does not).
 * Other arguments: shannon_sysrev=N (the board revision the AP reports, 0 if not given: Samsung reads it from eight GPIOs),
 * shannon_blsize=N (the boot loader's size for the SMC, BOOT's TOC size if not given, as cbd), shannon_toc=1 (the TOC at
 * +0 and BOOT after it, the layout shm_ipc.c's memory-map probe reads; not what cbd does), shannon_base=1 (the region's
 * base ORed into the verify call's mode, as Linux does only when its boot-time probe found a memory map).
 *
 * sysctl kern.hakios.shannon_state: 0 nothing, 1 read, 2 loaded and the SMC said CP_NO_ERROR, 3 the CP's status came up,
 * 4 the CP sent PHONE_START or INIT_START. No kern.hakios.cellular_* here yet: those are the redwood modem's
 * (hakios.mpss.c), and SpringBoard's hook (userland/src/inject_sb.c) reads them; this driver publishes them once it has
 * a registration to say.
 *
 * Never logged: anything of EFS, the NV image, IMEI, IMSI, ICCID, numbers or cell identities.
 */
#include <kern/clock.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <libkern/crc.h>
#include <libkern/OSByteOrder.h>
#include <mach/mach_time.h>
#include <mach/mach_types.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#include <stdarg.h>
#include <string.h>
#include <sys/errno.h>
#include <sys/param.h>
#include <sys/sysctl.h>
#include <sys/systm.h>
#include <sys/hakios.input.h>
#include <sys/hakios.physmap.h>
#include <sys/hakios.xnu.h>

void hakios_shannon_init(void);

#define PMU_PA                  0x14060000u     /* the 9810's PMU (alive block) */
#define PMU_SIZE                0x8000u
#define PMU_CP_STAT             0x0038u
#define PMU_CENTRAL_SEQ_CP_CONF 0x0280u
#define PMU_CENTRAL_SEQ_CP_STAT 0x0284u         /* [23:16]: 0x80 reset asserted, 0 running */
#define PMU_RESET_SEQ_STATUS    0x0504u         /* [2:0] == 5: the CP has been initialised (cal_cp_status) */
#define PMU_CP_SYS_PWR_FIRST    0x1320u         /* RESET_AHEAD_CP .. CP_DUMP_PC _SYS_PWR_REG, seven words */

#define GICD_PENDING_PA         0x10101000u     /* pmucal: 0x10100000 + 0x1238, bit 28, waited on after the CP's init */
#define GICD_PENDING_OFF        0x0238u

#define MBOX_PA                 0x14170000u     /* mcu_ipc@14170000, "mcu_ipc_cp" */
#define MBOX_SIZE               0x1000u
#define MBOX_INTGR0             0x08u
#define MBOX_INTCR0             0x0Cu
#define MBOX_INTMR0             0x10u
#define MBOX_INTSR0             0x14u           /* the CP's interrupts to the AP: bits 16 + n */
#define MBOX_INTGR1             0x1Cu           /* the AP's to the CP: bit n */
#define MBOX_INTMR1             0x24u
#define MBOX_INTSR1             0x28u
#define MBOX_ISSR(n)            (0x80u + 4u * (n))
#define MBOX_WORDS              64

#define MBX_AP2CP_MSG           0               /* the device tree's mif_pdata */
#define MBX_CP2AP_MSG           1
#define MBX_AP2CP_STATUS        2
#define MBX_CP2AP_STATUS        3
#define INT_AP2CP_MSG           0
#define IRQ_CP2AP_MSG           0

#define AP2CP_CFG_PA            0x14820000u
#define AP2CP_CFG_DONE          0x1u

#define CP_PA                   0xF0000000ull   /* modem_if: the loader keeps it out */
#define CP_SIZE                 0x9200000ull
#define CP_VIEW                 0x40000000u     /* the same memory as the CP addresses it (RADIO's TOC) */
#define CP_IPC_OFFSET           0x8000000u      /* the boot region ends here; the IPC region starts */

#define SMC_CP                  0x82000700u     /* Samsung's SiP call for the CP: verify (mode 0..2), READ_CTRL, WRITE_CTRL */
#define SMC_CP_READ_CTRL        3u
#define SMC_CP_WRITE_CTRL       4u
#define SMC_SSS_CLK             0x82001011u     /* the security engine's clock around the verify call */
#define CP_CTRL_S               0u
#define CP_CTRL_NS              1u

#define CMD_VALID               0x0040u
#define CMD_FIELD               0x003Fu
#define INT_VALID               0x0080u

static const char *const smc_errors[] = {
	"CP_NO_ERROR", "CP_NOT_ALIGN_64KB", "CP_MEM_TOO_BIG", "CP_FLAG_OUT_RANGE", "CP_WRONG_TZASC_REGION_NUM",
	"CP_WRONG_BL_SIZE", "CP_MEM_OUT_OF_RANGE", "CP_NOT_ALIGN_16B", "CP_MEM_IN_SECURE_DRAM", "CP_ASP_ENABLE_FAIL",
	"CP_ASP_DISABLE_FAIL", "CP_NOT_WORKING", "CP_ALREADY_WORKING", "CP_ALREADY_DUMP_MODE", "CP_NOT_VALID_MAGIC",
	"CP_SHOULD_BE_DISABLE", "CP_ALREADY_ENABLE_CPMEM_ON", "CP_ALREADY_SET_WND", "CP_FAIL_TO_SET_WND",
	"CP_INVALID_CP_BASE", "CP_CORRUPTED_CP_MEM_INFO",
};

static const char *const cp_commands[] = {
	"0", "INIT_START", "INIT_END", "REQ_ACTIVE", "RES_ACTIVE", "REQ_TIME_SYNC", "KERNEL_PANIC", "CRASH_RESET",
	"PHONE_START", "CRASH_EXIT", "CP_DEEP_SLEEP", "NV_REBUILDING", "EMER_DOWN", "PIF_INIT_DONE", "SILENT_NV_REBUILD",
	"NORMAL_POWER_OFF",
};

struct toc_entry {                              /* RADIO's table of contents, 32 bytes an entry */
	char            name[12];
	uint32_t        offset;                 /* in the partition */
	uint32_t        address;                /* where the CP sees it */
	uint32_t        size;
	uint32_t        crc;
	uint32_t        id;
};

#define TOC_MAX                 16
#define MEMMAP_OFFSET           0xA0u           /* the CP memory map, in BOOT */

static struct toc_entry toc[TOC_MAX];
static unsigned int toc_count;
static volatile uint8_t *pmu, *mbox, *ap2cp_cfg;
static volatile uint8_t *cp_mem;                /* the CP's memory, mapped by step 2 */
static int shannon_state;

SYSCTL_DECL(_kern_hakios);
SYSCTL_INT(_kern_hakios, OID_AUTO, shannon_state, CTLFLAG_RD | CTLFLAG_LOCKED, &shannon_state, 0,
    "the Shannon modem: 0 nothing, 1 read, 2 verified, 3 its status up, 4 it spoke");

static int say(const char *format, ...) __printflike(1, 2);

static int
say(const char *format, ...)
{
	char line[256];
	va_list list;
	int length;

	va_start(list, format);
	length = vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	loader_log_line(line);
	printf("%s", line);
	return length;
}

static inline uint32_t
rd(volatile uint8_t *base, uint32_t offset)
{
	return *(volatile uint32_t *)(base + offset);
}

static inline void
wr(volatile uint8_t *base, uint32_t offset, uint32_t value)
{
	*(volatile uint32_t *)(base + offset) = value;
	__builtin_arm_dsb(15);
}

static uint64_t
smc(uint64_t function, uint64_t first, uint64_t second, uint64_t third)
{
	register uint64_t x0 __asm__("x0") = function;
	register uint64_t x1 __asm__("x1") = first;
	register uint64_t x2 __asm__("x2") = second;
	register uint64_t x3 __asm__("x3") = third;

	__asm__ volatile ("smc #0" : "+r" (x0), "+r" (x1), "+r" (x2), "+r" (x3) : :
            "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "memory");
	return x0;
}

/* CP_CTRL_S or _NS through EL3, as pmu-cp.c's exynos_smc_read: two halves, each in the answer's top 16 bits. */
static bool
cp_ctrl_read(uint32_t which, uint32_t *value)
{
	uint32_t low = (uint32_t)smc(SMC_CP, SMC_CP_READ_CTRL, 0, which);
	uint32_t high;

	if ((low & 0xffff) != 0) {
		say("shannon: CP_CTRL_%s: the read SMC answered %#x\n", which == CP_CTRL_S ? "S" : "NS", low & 0xffff);
		return false;
	}
	high = (uint32_t)smc(SMC_CP, SMC_CP_READ_CTRL, 1, which);
	if ((high & 0xffff) != 0) {
		say("shannon: CP_CTRL_%s: the read SMC (high half) answered %#x\n", which == CP_CTRL_S ? "S" : "NS", high & 0xffff);
		return false;
	}
	*value = (high >> 16) << 16 | (low >> 16);
	return true;
}

/* pmucal_smc_write with a mask: the bits set, the rest kept. */
static bool
cp_ctrl_set(uint32_t which, uint32_t bits)
{
	uint32_t before, after = 0;
	uint64_t answer;

	if (!cp_ctrl_read(which, &before)) {
		return false;
	}
	answer = smc(SMC_CP, SMC_CP_WRITE_CTRL, before | bits, which);
	(void)cp_ctrl_read(which, &after);
	say("shannon: CP_CTRL_%s %#x -> %#x (write answered %#llx)\n", which == CP_CTRL_S ? "S" : "NS", before, after, answer);
	return (int32_t)answer <= 0;           /* pmu-cp.c: "ret > 0" is the failure */
}

static bool
wait_field(volatile uint8_t *base, uint32_t offset, uint32_t mask, uint32_t want, uint32_t ms, const char *name)
{
	for (uint32_t waited = 0; ; waited++) {
		uint32_t value = rd(base, offset);

		if ((value & mask) == want) {
			say("shannon: %s = %#x after %u ms\n", name, value, waited);
			return true;
		}
		if (waited >= ms) {
			say("shannon: %s = %#x, not %#x under %#x, after %u ms\n", name, value, want, mask, ms);
			return false;
		}
		delay(1000);
	}
}

static void
map_registers(void)
{
	if (pmu == NULL) {
		pmu = (volatile uint8_t *)ml_io_map(PMU_PA, PMU_SIZE);
		mbox = (volatile uint8_t *)ml_io_map(MBOX_PA, MBOX_SIZE);
	}
}

static bool
cp_initialised(void)
{
	return pmu != NULL && (rd(pmu, PMU_RESET_SEQ_STATUS) & 7) == 5;
}

/* One MMIO read, said first: if it faults, the last line in the log names it. */
static uint32_t
peek(const char *when, const char *name, volatile uint8_t *base, uint32_t pa, uint32_t offset)
{
	uint32_t value;

	say("shannon: %s: read %s (%#x)\n", when, name, pa + offset);
	value = rd(base, offset);
	say("shannon: %s:   %s = %#x\n", when, name, value);
	return value;
}

static void
log_cp_registers(const char *when)
{
	uint32_t ns = 0, s = 0, regs = 0;
	static const char *const pwr_names[] = {
		"RESET_AHEAD_CP_SYS_PWR_REG", "CLEANY_BUS_CP_SYS_PWR_REG", "LOGIC_RESET_CP_SYS_PWR_REG",
		"TCXO_GATE_CP_SYS_PWR_REG", "CP_DISABLE_ISO_SYS_PWR_REG", "CP_RESET_ISO_SYS_PWR_REG", "CP_DUMP_PC_SYS_PWR_REG",
	};

	(void)PE_parse_boot_argn("shannon_regs", &regs, sizeof(regs));
	say("shannon: %s: CP_CTRL_NS and CP_CTRL_S through SMC %#x READ_CTRL\n", when, SMC_CP);
	if (cp_ctrl_read(CP_CTRL_NS, &ns)) {
		say("shannon: %s:   CP_CTRL_NS = %#x\n", when, ns);
	}
	if (cp_ctrl_read(CP_CTRL_S, &s)) {
		say("shannon: %s:   CP_CTRL_S = %#x\n", when, s);
	}
	(void)peek(when, "PMU CP_STAT", pmu, PMU_PA, PMU_CP_STAT);
	(void)peek(when, "PMU CENTRAL_SEQ_CP_CONFIGURATION", pmu, PMU_PA, PMU_CENTRAL_SEQ_CP_CONF);
	(void)peek(when, "PMU CENTRAL_SEQ_CP_STATUS", pmu, PMU_PA, PMU_CENTRAL_SEQ_CP_STAT);
	(void)peek(when, "PMU RESET_SEQUENCER_STATUS", pmu, PMU_PA, PMU_RESET_SEQ_STATUS);
	(void)peek(when, "mailbox INTMR0", mbox, MBOX_PA, MBOX_INTMR0);
	if (regs) {
		for (uint32_t i = 0; i < 7; i++) {
			(void)peek(when, pwr_names[i], pmu, PMU_PA, PMU_CP_SYS_PWR_FIRST + 4 * i);
		}
		(void)peek(when, "mailbox INTSR0", mbox, MBOX_PA, MBOX_INTSR0);
		(void)peek(when, "mailbox INTMR1", mbox, MBOX_PA, MBOX_INTMR1);
		(void)peek(when, "mailbox INTSR1", mbox, MBOX_PA, MBOX_INTSR1);
		for (uint32_t i = 0; i < 4; i++) {
			char name[24];

			snprintf(name, sizeof(name), "mailbox word %u", i);
			(void)peek(when, name, mbox, MBOX_PA, MBOX_ISSR(i));
		}
	}
	if (ap2cp_cfg != NULL) {
		(void)peek(when, "AP2CP_CFG", ap2cp_cfg, AP2CP_CFG_PA, 0);
	} else {
		say("shannon: %s: AP2CP_CFG not read (only once the CP is initialised and verified: step 3)\n", when);
	}
}

static const struct toc_entry *
toc_find(const char *name)
{
	for (unsigned int i = 0; i < toc_count; i++) {
		if (strncmp(toc[i].name, name, sizeof(toc[i].name)) == 0) {
			return &toc[i];
		}
	}
	return NULL;
}

/* RADIO's table of contents and the memory map in BOOT; false if the partition is not one. */
static bool
read_radio(void)
{
	static uint8_t page[4096];
	uint64_t got = 0;
	int waited, error = 0;
	const struct toc_entry *boot;

	for (waited = 0; waited < 120; waited++) {
		error = hakios_ufs_read_partition("RADIO", 0, page, sizeof(page), &got);
		if (error == 0) {
			break;
		}
		IOSleep(500);
	}
	if (error != 0) {
		say("shannon: the RADIO partition cannot be read (%d) after %d ms: is ufs= given?\n", error, waited * 500);
		return false;
	}
	toc_count = 0;
	for (unsigned int i = 0; i < TOC_MAX; i++) {
		struct toc_entry entry;

		memcpy(&entry, page + i * sizeof(entry), sizeof(entry));
		if (entry.name[0] == 0) {
			break;
		}
		entry.name[sizeof(entry.name) - 1] = 0;
		toc[toc_count++] = entry;
		say("shannon: RADIO %-6s at %#x, %#x bytes, for the CP's %#x (+%#x), crc %#x, id %u\n", entry.name, entry.offset,
		    entry.size, entry.address, entry.address >= CP_VIEW ? entry.address - CP_VIEW : 0, entry.crc, entry.id);
	}
	if (toc_count == 0 || strcmp(toc[0].name, "TOC") != 0 || (boot = toc_find("BOOT")) == NULL || toc_find("MAIN") == NULL) {
		say("shannon: RADIO does not start with a Shannon TOC (TOC, BOOT, MAIN): nothing more\n");
		return false;
	}
	if (boot->offset + MEMMAP_OFFSET + 64 <= sizeof(page)) {
		const uint32_t *map = (const uint32_t *)(const void *)(page + boot->offset + MEMMAP_OFFSET);
		uint32_t tag = OSSwapInt32(map[0] + 0x30);   /* shm_ipc.c: ntohl(table_id_ver + 0x30): "MEM0" */

		if (memcmp(&tag, "MEM", 3) == 0 && map[3] <= 5) {
			say("shannon: the CP's memory map: DRAM %#x, more %#x, %u parts\n", map[1], map[2], map[3]);
			for (uint32_t i = 0; i < map[3]; i++) {
				uint32_t name = OSSwapInt32(map[4 + 3 * i]);

				say("shannon:   %.3s at +%#x, %#x bytes\n", (const char *)&name + 1, map[5 + 3 * i], map[6 + 3 * i]);
			}
		} else {
			say("shannon: no MEM table at BOOT+%#x: the device tree's layout applies (CP 0x7900000, IPC at +0x8000000)\n",
			    MEMMAP_OFFSET);
		}
	}
	return true;
}

/* An entry of RADIO into the CP's memory, straight from the partition. */
static bool
load_part(volatile uint8_t *cp, uint64_t at, uint64_t from, uint64_t length, const char *name)
{
	const uint64_t chunk = 1u << 20;
	uint64_t done = 0, ns;
	uint64_t began = mach_absolute_time();

	if (at + length > CP_IPC_OFFSET) {
		say("shannon: %s (%#llx bytes at +%#llx) would run past the boot region: not loaded\n", name, length, at);
		return false;
	}
	while (done < length) {
		uint64_t n = MIN(chunk, length - done);
		/* 0 is success; the last argument is the partition's size, not a count (hakios.ufs.c; build 38 stopped on that) */
		int error = hakios_ufs_read_partition("RADIO", from + done, (uint8_t *)(uintptr_t)(cp + at + done), n, NULL);

		if (error != 0) {
			say("shannon: %s: RADIO read of %#llx bytes at %#llx failed (%d)\n", name, n, from + done, error);
			return false;
		}
		done += n;
	}
	__builtin_arm_dsb(15);
	{
		/* the first bytes again, through a kernel buffer, compared with what landed in the CP's memory */
		uint8_t check[64];
		uint64_t n = MIN((uint64_t)sizeof(check), length);
		bool same = hakios_ufs_read_partition("RADIO", from, check, n, NULL) == 0;

		for (uint64_t i = 0; same && i < n; i++) {
			same = check[i] == cp[at + i];
		}
		if (!same) {
			say("shannon: %s: the first %llu bytes in the CP's memory differ from RADIO's: stopping\n", name, n);
			return false;
		}
	}
	absolutetime_to_nanoseconds(mach_absolute_time() - began, &ns);
	say("shannon: %s: %#llx bytes to +%#llx in %llu ms, crc32 %#x\n", name, length, at, ns / 1000000,
	    crc32(0, (const void *)(uintptr_t)(cp + at), (size_t)length));
	return true;
}

/*
 * cbd makes both security requests with CPU 0 taken out of its affinity (std_security_req: sched_setaffinity with
 * bit 0 cleared, restored after). No thread is bound here: binding to a processor that is not up yet panicked build
 * 37 in thread_setrun. Instead the call is made with interrupts off (so the thread cannot move between the check and
 * the SMC; EL3 runs the call with them masked anyway) on whatever core the thread is on, if its MPIDR's affinity is not
 * 0; on core 0 the thread sleeps 10 ms and looks again, up to 2 s.
 */
static uint64_t
security_request(uint32_t mode, uint32_t size_boot, uint32_t size_main)
{
	uint64_t answer = ~0ull, mpidr = 0;
	int tries;

	say("shannon: security SMC %#x(mode %#x, %#x, %#x): looking for a core other than CPU 0\n", SMC_CP, mode, size_boot,
	    size_main);
	for (tries = 0; tries < 200; tries++) {
		boolean_t enabled = ml_set_interrupts_enabled(FALSE);

		mpidr = __builtin_arm_rsr64("MPIDR_EL1");
		if ((mpidr & 0xffffffull) != 0) {
			(void)smc(SMC_SSS_CLK, 0, 0, 0);
			answer = smc(SMC_CP, mode, size_boot, size_main);
			(void)smc(SMC_SSS_CLK, 1, 0, 0);
			(void)ml_set_interrupts_enabled(enabled);
			break;
		}
		(void)ml_set_interrupts_enabled(enabled);
		IOSleep(10);
	}
	if (tries == 200) {
		say("shannon: the thread stayed on CPU 0 for 2 s: the security call was not made\n");
		return ~0ull;
	}
	say("shannon: security SMC %#x(mode %#x, %#x, %#x) from MPIDR %#llx answered %#llx (%s)\n", SMC_CP, mode, size_boot,
	    size_main, mpidr, answer, answer < sizeof(smc_errors) / sizeof(smc_errors[0]) ? smc_errors[answer] : "unknown");
	return answer;
}

/* cbd's steps 3 to 5 (the header): the CP's init and the non-secure request, the images, the secure request. */
static bool
load_and_verify(void)
{
	static const char *const stages[] = { "BOOT", "MAIN", "VSS", "NV" };
	const struct toc_entry *boot = toc_find("BOOT"), *main_part = toc_find("MAIN");
	uint32_t with_toc = 0, nv_wanted = 0, with_base = 0, blsize = boot->size;
	volatile uint8_t *cp;
	uint64_t answer;

	cp_mem = NULL;

	(void)PE_parse_boot_argn("shannon_toc", &with_toc, sizeof(with_toc));
	(void)PE_parse_boot_argn("shannon_nv", &nv_wanted, sizeof(nv_wanted));
	(void)PE_parse_boot_argn("shannon_blsize", &blsize, sizeof(blsize));
	(void)PE_parse_boot_argn("shannon_base", &with_base, sizeof(with_base));

	/* 3: IOCTL_SECURITY_REQ(2, 0, 0): on the 9810, cal_cp_init first if the CP has never been initialised */
	if ((rd(pmu, PMU_RESET_SEQ_STATUS) & 7) != 5) {
		volatile uint8_t *gicd = (volatile uint8_t *)ml_io_map(GICD_PENDING_PA, 0x1000);

		say("shannon: the CP has not been initialised: its PMU sequence (cal_cp_init)\n");
		(void)cp_ctrl_set(CP_CTRL_NS, 1u << 1);
		(void)cp_ctrl_set(CP_CTRL_S, 1u << 3);
		delay(1024);
		if (!wait_field(pmu, PMU_RESET_SEQ_STATUS, 7, 5, 100, "RESET_SEQUENCER_STATUS")) {
			say("shannon: the CP's init did not complete: stopping before any image is loaded\n");
			return false;
		}
		if (gicd != NULL) {
			(void)wait_field(gicd, GICD_PENDING_OFF, 1u << 28, 1u << 28, 100, "the GIC's pending word (0x10101238)");
		}
	} else {
		say("shannon: the CP was initialised before (RESET_SEQUENCER_STATUS %#x)\n", rd(pmu, PMU_RESET_SEQ_STATUS));
	}
	answer = security_request(2, 0, 0);
	if (answer != 0 && answer != 11) {
		say("shannon: the non-secure request failed (cbd takes only 0 or CP_NOT_WORKING): no image loaded\n");
		return false;
	}
	log_cp_registers("after the non-secure request");

	/* 4: IOCTL_MODEM_XMIT_BOOT, TOC entries BOOT, MAIN, VSS, NV, each to (address & 0x3fffffff) */
	cp = cp_mem = (volatile uint8_t *)hakios_physmap_wc(CP_PA, CP_SIZE);
	if (cp == NULL) {
		say("shannon: the CP's memory (%#llx, %#llx bytes) could not be mapped\n", CP_PA, CP_SIZE);
		return false;
	}
	for (unsigned int i = 0; i < sizeof(stages) / sizeof(stages[0]); i++) {
		const struct toc_entry *entry = toc_find(stages[i]);
		uint32_t at;

		if (entry == NULL) {
			say("shannon: RADIO has no %s entry: stopping\n", stages[i]);
			return false;
		}
		at = entry->address & 0x3fffffffu;
		if ((uint64_t)at + entry->size > CP_IPC_OFFSET) {
			say("shannon: %s (%#x bytes at +%#x) runs past the boot region: stopping\n", entry->name, entry->size, at);
			return false;
		}
		if (strcmp(entry->name, "NV") == 0) {
			volatile uint8_t *to = cp + at;
			const uint8_t *data = NULL;
			uint64_t size = 0;

			if (nv_wanted && hakios_partition_file("EFS", "/efs", "/nv_data.bin", entry->size, &data, &size) == 0 &&
			    data != NULL && size == entry->size) {
				for (uint64_t k = 0; k < size; k++) {
					to[k] = data[k];
				}
				say("shannon: NV: the phone's own nv_data.bin, %llu bytes, to +%#x (contents not shown)\n", size, at);
			} else {
				for (uint32_t k = 0; k < entry->size; k++) {
					to[k] = 0xff;
				}
				say("shannon: NV: %#x bytes of 0xFF at +%#x, cbd's create_empty_nv (%s)\n", entry->size, at,
				    !nv_wanted ? "shannon_nv=1 not given" : data == NULL ? "EFS:/nv_data.bin not read" :
				    "EFS:/nv_data.bin is not the NV entry's size");
			}
			__builtin_arm_dsb(15);
			continue;
		}
		if (entry == boot && with_toc) {
			if (!load_part(cp, 0, 0, boot->offset + boot->size, "TOC+BOOT (shannon_toc=1)")) {
				return false;
			}
			continue;
		}
		if (!load_part(cp, at, entry->offset, entry->size, entry->name)) {
			return false;
		}
	}

	/* 5: IOCTL_SECURITY_REQ(0, BOOT's size, MAIN's size) */
	answer = security_request(with_base ? (uint32_t)CP_PA : 0, blsize, main_part->size);
	log_cp_registers("after the secure request");
	return answer == 0;
}

/*
 * cbd's steps 10 and 11 (docs/cellular-star2lte.md): IOCTL_MODEM_DL_START and the UDL handshake, over the legacy IPC
 * map at the IPC region (shmem_4mb_phys_map without LEGACY_QOS): magic +0, access +4, FMT tx head/tail +8/+0xc, FMT
 * rx +0x10/+0x14, RAW tx +0x18/+0x1c, RAW rx +0x20/+0x24; FMT tx/rx buffers at +0x1000/+0x2000 (4 KB each), RAW tx at
 * +0x3000 (2084864 bytes), RAW rx at +0x200000 (2 MB). cbd writes a 4-byte word to umts_boot0; the io device puts a
 * SIPC5 header on it (config 0xFC once DL_START has made the link aligned, channel 215, length 8) and xmit_udl puts
 * the frame in the RAW tx queue, with no interrupt; the answer comes back in the RAW rx queue.
 */
#define IPC_MAGIC               0x00u
#define IPC_ACCESS              0x04u
#define IPC_RAW_TX_HEAD         0x18u
#define IPC_RAW_TX_TAIL         0x1Cu
#define IPC_RAW_RX_HEAD         0x20u
#define IPC_RAW_RX_TAIL         0x24u
#define IPC_RAW_TX_BUFF         0x3000u
#define IPC_RAW_TX_SIZE         2084864u
#define IPC_RAW_RX_BUFF         0x200000u
#define IPC_RAW_RX_SIZE         0x200000u
#define MEM_BOOT_MAGIC          0x424F4F54u
#define SIPC5_CH_BOOT0          215u

static bool
dl_start(volatile uint8_t *ipc)
{
	say("shannon: DL_START: writing the IPC map at %#llx (outside what the secure call closes, if Samsung's layout holds)\n",
	    CP_PA + CP_IPC_OFFSET);
	for (uint32_t offset = 0x08; offset <= 0x24; offset += 4) {
		wr(ipc, offset, 0);
	}
	wr(ipc, IPC_MAGIC, MEM_BOOT_MAGIC);
	say("shannon: DL_START: the IPC map reset, magic %#x (reads %#x)\n", MEM_BOOT_MAGIC, rd(ipc, IPC_MAGIC));
	return rd(ipc, IPC_MAGIC) == MEM_BOOT_MAGIC;
}

static void
raw_tx(volatile uint8_t *ipc, uint32_t word)
{
	uint8_t frame[8] = { 0xFC, SIPC5_CH_BOOT0, 8, 0, (uint8_t)word, (uint8_t)(word >> 8), (uint8_t)(word >> 16),
		             (uint8_t)(word >> 24) };
	uint32_t head = rd(ipc, IPC_RAW_TX_HEAD);

	for (uint32_t i = 0; i < sizeof(frame); i++) {
		ipc[IPC_RAW_TX_BUFF + (head + i) % IPC_RAW_TX_SIZE] = frame[i];
	}
	__builtin_arm_dsb(15);
	wr(ipc, IPC_RAW_TX_HEAD, (head + sizeof(frame)) % IPC_RAW_TX_SIZE);
}

/* One UDL request and its answer, as std_udl_req_resp: 30 s at most. */
static bool
udl_req_resp(volatile uint8_t *ipc, uint32_t request, uint32_t expect, bool ring)
{
	say("shannon: UDL request %#x (expecting %#x)\n", request, expect);
	raw_tx(ipc, request);
	if (ring) {
		wr(mbox, MBOX_ISSR(MBX_AP2CP_MSG), INT_VALID | 0x01u /* MASK_SEND_RAW */);
		wr(mbox, MBOX_INTGR1, 1u << INT_AP2CP_MSG);
	}
	for (int ms = 0; ms < 30000; ms += 10) {
		uint32_t head = rd(ipc, IPC_RAW_RX_HEAD), tail = rd(ipc, IPC_RAW_RX_TAIL);

		if (head != tail) {
			uint8_t hdr[8];
			uint32_t length, word;

			for (uint32_t i = 0; i < sizeof(hdr); i++) {
				hdr[i] = ipc[IPC_RAW_RX_BUFF + (tail + i) % IPC_RAW_RX_SIZE];
			}
			length = hdr[2] | (uint32_t)hdr[3] << 8;
			word = hdr[4] | (uint32_t)hdr[5] << 8 | (uint32_t)hdr[6] << 16 | (uint32_t)hdr[7] << 24;
			if (hdr[0] & 0x04) {
				length = (length + 3) & ~3u;
			}
			say("shannon: UDL answer after %d ms: frame %02x %02x len %u, word %#x (rx head %#x tail %#x)\n", ms,
			    hdr[0], hdr[1], hdr[2] | hdr[3] << 8, word, head, tail);
			wr(ipc, IPC_RAW_RX_TAIL, (tail + (length ? length : (head - tail) % IPC_RAW_RX_SIZE)) % IPC_RAW_RX_SIZE);
			return word == expect;
		}
		IOSleep(10);
	}
	say("shannon: no UDL answer in 30 s (raw tx head %#x tail %#x, rx head %#x tail %#x, CP status %#x)\n",
	    rd(ipc, IPC_RAW_TX_HEAD), rd(ipc, IPC_RAW_TX_TAIL), rd(ipc, IPC_RAW_RX_HEAD), rd(ipc, IPC_RAW_RX_TAIL),
	    rd(mbox, MBOX_ISSR(MBX_CP2AP_STATUS)));
	return false;
}

static void
boot_handshake(void)
{
	uint32_t ring = 0;
	volatile uint8_t *ipc;

	if (cp_mem == NULL) {
		say("shannon: no mapping of the CP's memory: no handshake\n");
		return;
	}
	(void)PE_parse_boot_argn("shannon_udl_irq", &ring, sizeof(ring));
	ipc = cp_mem + CP_IPC_OFFSET;
	if (!dl_start(ipc)) {
		say("shannon: the IPC map does not hold the magic: no handshake\n");
		return;
	}
	/* modem_boot_confirm: AP status 1 (after DL_START in IOCTL_MODEM_DL_START) */
	wr(mbox, MBOX_ISSR(MBX_AP2CP_STATUS), (rd(mbox, MBOX_ISSR(MBX_AP2CP_STATUS)) & ~(0xfu << 1)) | 1u << 1);
	say("shannon: AP status word %#x (boot confirmed)\n", rd(mbox, MBOX_ISSR(MBX_AP2CP_STATUS)));
	if (udl_req_resp(ipc, 0x900d, 0xa00d, ring != 0) && udl_req_resp(ipc, 0x9f00, 0xaf00, ring != 0)) {
		say("shannon: the UDL handshake is done (stage done, then 0xf): BOOT_OFF waits for the CP's init\n");
	} else {
		say("shannon: the UDL handshake did not complete\n");
	}
}

/* Steps 3 and 4: the AP's status, the CP let go, its status awaited; then its messages, logged. */
static void
start_and_listen(void)
{
	uint32_t sysrev = 0, last_status, last_message = 0;
	uint64_t ap;

	(void)PE_parse_boot_argn("shannon_sysrev", &sysrev, sizeof(sysrev));
	if (!cp_initialised()) {
		say("shannon: RESET_SEQUENCER_STATUS %#x is not 5: the CP is not initialised, AP2CP_CFG is not touched; not started\n",
		    rd(pmu, PMU_RESET_SEQ_STATUS));
		return;
	}
	say("shannon: mapping AP2CP_CFG (%#x): the CP is initialised\n", AP2CP_CFG_PA);
	ap2cp_cfg = (volatile uint8_t *)ml_io_map(AP2CP_CFG_PA, 0x1000);
	if (ap2cp_cfg == NULL) {
		say("shannon: AP2CP_CFG could not be mapped; not started\n");
		return;
	}
	for (int i = 0; i < MBOX_WORDS; i++) {
		wr(mbox, MBOX_ISSR(i), 0);
	}
	ap = (uint64_t)(sysrev & 0xff) << 6 | 0u << 14 /* ds_det: one SIM */ | 1u << 5 /* pda_active */;
	wr(mbox, MBOX_ISSR(MBX_AP2CP_STATUS), (uint32_t)ap);
	say("shannon: the AP's status word %#x (sys_rev %u, one SIM, pda_active)\n", (uint32_t)ap, sysrev);
	(void)peek("start", "AP2CP_CFG", ap2cp_cfg, AP2CP_CFG_PA, 0);
	say("shannon: writing AP2CP_CFG = 1\n");
	wr(ap2cp_cfg, 0, AP2CP_CFG_DONE);
	say("shannon: AP2CP_CFG = 1 (reads %#x): the CP runs\n", rd(ap2cp_cfg, 0));
	IOSleep(300);
	for (int i = 0; i < 200 && ((rd(mbox, MBOX_ISSR(MBX_CP2AP_STATUS)) >> 1) & 0xf) == 0; i++) {
		IOSleep(10);           /* ss310ap_boot_on: 100 tries of 10-20 ms */
	}
	if (((rd(mbox, MBOX_ISSR(MBX_CP2AP_STATUS)) >> 1) & 0xf) != 0) {
		shannon_state = 3;
	}
	say("shannon: the CP's status word %#x after 2 s: %s\n", rd(mbox, MBOX_ISSR(MBX_CP2AP_STATUS)),
	    shannon_state >= 3 ? "it is up" : "nothing (Samsung's kernel gives up here: -EACCES)");
	log_cp_registers("after the start");
	if (shannon_state >= 3) {
		boot_handshake();       /* cbd's DL_START and HANDSHAKE COMMAND */
	}
	last_status = rd(mbox, MBOX_ISSR(MBX_CP2AP_STATUS));
	for (int tick = 0; tick < 60 * 100; tick++) {
		uint32_t pending = rd(mbox, MBOX_INTSR0) >> 16, status = rd(mbox, MBOX_ISSR(MBX_CP2AP_STATUS));

		if (pending & (1u << IRQ_CP2AP_MSG)) {
			uint32_t message = rd(mbox, MBOX_ISSR(MBX_CP2AP_MSG));

			wr(mbox, MBOX_INTCR0, (1u << IRQ_CP2AP_MSG) << 16);
			if ((message & INT_VALID) && (message & CMD_VALID)) {
				uint32_t command = message & CMD_FIELD;

				say("shannon: %d.%02d s: the CP says %s (%#x)\n", tick / 100, tick % 100,
				    command < sizeof(cp_commands) / sizeof(cp_commands[0]) ? cp_commands[command] : "?", message);
				if (command == 8 || command == 1) {
					shannon_state = 4;
				}
			} else if (message != last_message) {
				say("shannon: %d.%02d s: the CP's message word %#x (data, not a command)\n", tick / 100, tick % 100, message);
			}
			last_message = message;
		}
		if (pending & ~(1u << IRQ_CP2AP_MSG)) {
			say("shannon: %d.%02d s: the CP's interrupts %#x\n", tick / 100, tick % 100, pending);
			wr(mbox, MBOX_INTCR0, (pending & ~(1u << IRQ_CP2AP_MSG)) << 16);
		}
		if (status != last_status) {
			say("shannon: %d.%02d s: the CP's status word %#x -> %#x\n", tick / 100, tick % 100, last_status, status);
			last_status = status;
		}
		IOSleep(10);
	}
	say("shannon: 60 s listened to; nothing answered (PIF_INIT_DONE on SBD rings and INIT_END are the next step)\n");
}

static void
shannon_thread(void *arg, __unused wait_result_t wr_result)
{
	uint32_t step = (uint32_t)(uintptr_t)arg, peek = 0;

	say("shannon: thread up; mapping the PMU (%#x) and the mailbox (%#x)\n", PMU_PA, MBOX_PA);
	map_registers();
	if (pmu == NULL || mbox == NULL) {
		say("shannon: the PMU or the mailbox could not be mapped\n");
		return;
	}
	log_cp_registers("as found");
	(void)PE_parse_boot_argn("shannon_peek", &peek, sizeof(peek));
	if (peek) {
		volatile uint8_t *first = (volatile uint8_t *)ml_io_map((vm_offset_t)CP_PA, 0x1000);

		if (first != NULL) {
			say("shannon: the CP's memory as found: +0 %#x %#x %#x %#x, +0x200 %#x %#x, +0x2a0 %#x\n", rd(first, 0),
			    rd(first, 4), rd(first, 8), rd(first, 12), rd(first, 0x200), rd(first, 0x204), rd(first, 0x2a0));
		}
	}
	if (!read_radio()) {
		return;
	}
	shannon_state = 1;
	if (step < 2) {
		say("shannon: done (step 1, read only)\n");
		return;
	}
	if (!load_and_verify()) {
		say("shannon: stopped at the verify call; the CP is not started\n");
		return;
	}
	shannon_state = 2;
	if (step < 3) {
		say("shannon: done (step 2): loaded and verified, not started\n");
		return;
	}
	start_and_listen();
}

void
hakios_shannon_init(void)
{
	uint32_t step = 0;
	thread_t thread;

	if (!PE_parse_boot_argn("shannon", &step, sizeof(step)) || step < 1 || step > 3) {
		return;
	}
	say("shannon=%u: %s\n", step, step == 1 ? "the modem's registers and RADIO's headers, read only" :
	    step == 2 ? "RADIO loaded into the CP's memory and verified by EL3, not started" :
	    "loaded, verified, started, and listened to for 60 s");
	if (kernel_thread_start(shannon_thread, (void *)(uintptr_t)step, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	} else {
		say("shannon: the thread could not be started\n");
	}
}
