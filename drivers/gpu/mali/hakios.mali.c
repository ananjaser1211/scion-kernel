#define HAKIOS_SOC_FIXED
/*
 * Mali-G72 MP18 on the Galaxy S9 (starlte, Exynos 9810): bring-up in nine steps, then the job interface that
 * HakiosMali (drivers/gpu/mali/hakios.mali.cpp) gives Mesa's panfrost. The steps are listed above step_look().
 * Each step's lines go to the loader's log as well as the kernel log. Nothing is touched unless boot-arg mali=N is
 * set, and then only on the starlte SoC.
 *
 * Depends on the Exynos 9810's PMU and CMU_G3D; on the S2MPS18 PMIC (BUCK6, vdd_g3d) through ACPM
 * (hakios.exynos_acpm.c); on the TrustZone protection controller, reached by SMC to EL3 (Carbon at EL2 passes unknown
 * SMCs on to EL3); on the kernel's pmap for physical address checks; and on the TMU zone sensor for the governor
 * (hakios.exynos_tmu.c).
 *
 * Boot-args:
 *   mali=N              run the first N steps (9: all, then serve). 0 or unset: the GPU is left alone.
 *   mali_mhz=MHZ        GPU clock: 260 (default), 455 or 572 (or mali_max's level).
 *   mali_max=MHZ        a level above 572 MHz, only if the APM's own G3D table (DVFS 4) has one at or below MHZ with a
 *                       voltage BUCK6 can give (950 mV at most): added as the top level, its M/P/S solved for PLL_1018X.
 *                       Samsung's star2 tree stops at 572 (gpu_max_clock_limit); this is beyond it. Default: none.
 *   mali_dvs=1          with mali_gov=1: BUCK6 follows the level, at the APM's voltage for it + mali_dvs_margin (uV,
 *                       default 25000), never below mali_dvs_floor (uV, default 800000: gpu_default_voltage) nor above
 *                       950 mV; raised before a switch up, lowered after a switch down. Default 0: 950 mV throughout.
 *   mali_pll=0          leave PLL_G3D as the boot set it; the step stops if it is not running.
 *   mali_acpm=0         do not raise the rail. The GPU is then touched at the boot's voltage, which hung the S9+ (test only).
 *   mali_fastmap=0      walk the page tables through the GPU's caches; each buffer entered is then an MMU update of AS 0.
 *   mali_trace=1        one line per job: "tl: j SEQ f|v q QUEUED b BEGAN e ENDED", in microseconds.
 *   mali_gov=1          the clock follows the load (see gov_window()). Needs mali_pll and mali_acpm.
 *   mali_gov_frame=MHZ  clock floor while frames are shown (default 572; 0: none).
 *   mali_nap_us=US      poll interval for a job still running after its first 200 us, clamped to 10..1000 (default 50).
 *
 * The user client is opened by backboardd, which does not run as root. Nothing here or in hakios.mali.cpp needs root.
 *
 * Origin. The reset, cores, address space 0, first jobs, page tables, buffer addresses, job queue, waits and the report
 * follow HakiOS's Mali driver for the MT6768 (Mali-G52): kernel/bsd/dev/redwood_mali.c of
 * github.com/oosexgang/hakios-mt6768 at commit 93ea250, Apple Public Source License 2.0. Register names and meanings
 * are from Linux panfrost (drivers/gpu/drm/panfrost/panfrost_regs.h). The power, clock and rail sequence follows the
 * S9+ mainline port (github.com/SheroAbi/galaxy-s9plus-mainline-linux, GPL-2.0) and Samsung's GPL-2.0 Exynos 9810
 * kernel. Those were read for facts and not copied.
 */
#include <kern/clock.h>
#include <kern/locks.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/hakios.exynos_acpm.h>
#include <sys/hakios.input.h>
#include <sys/sysctl.h>
#include <stdarg.h>
#include <sys/systm.h>
#include <vm/vm_kern.h>
#include <sys/hakios.exynos_soc.h>
#include "hakios.mali_va.h"
#include <sys/hakios.log.h>
#include <sys/hakios.xnu.h>

extern vm_offset_t phystokv(uint64_t pa);
extern boolean_t pmap_valid_address(uint64_t addr);             /* memory the kernel manages */
extern int hakios_mfc_carved(uint64_t phys);                    /* the video decoder's (hakios.exynos_mfc.c, mfc_zc; stubs.c) */
extern int hakios_scanout_carved(uint64_t phys);                /* the DPU's pool (hakios.exynos_dpu.c; stubs.c) */
extern void hakios_exynos_bus_busy(void);                      /* drivers/soc/exynos/hakios.exynos_acpm.c */
extern void HakiosMaliStart(void);                              /* drivers/gpu/mali/hakios.mali.cpp */
extern uint32_t gMaliClients, gMaliBuffers, gMaliSubmits, gMaliFailed, gMaliShown;   /* hakios.mali.cpp */
extern struct mali_made { uint64_t address, size; uint32_t asked, flags; } gMaliMade[32];

/*
 * Power, clock and supply. The G3D power domain is pd-embedded_g3d in the PMU. Its status reads 0xF when it is on.
 * A read of the GPU while the domain is off is a bus hang, not an error. PLL_G3D is in CMU_G3D, outside the domain, so
 * it keeps its settings when the domain is off. The supply is BUCK6 (vdd_g3d) of the S2MPS18, reached through ACPM.
 * S-Boot leaves BUCK6 at 644 mV with the GPU at 260 MHz. On the S9+ that combination gave DATA_INVALID_FAULTs every few
 * seconds, and two kernels that reset and read the GPU before the rail was raised never came back (the SoC hung).
 * So step 2 raises B6OUT to 950 mV and reads it back before the GPU's registers are first touched.
 */

#define STARLTE_SOC_BASE        EXYNOS_SOC_BASE_PA    /* the loader's platform "starlte" */

#define PMU_PHYS                EXYNOS_PMU_PA    /* the PMU */
#define PMU_SIZE                0x5000
#define PMU_G3D_CONFIGURATION   0x4040          /* 0x14064040: [3:0] = 0xF asks for pd-embedded_g3d on, 0 off */
#define PMU_G3D_STATUS          0x4044          /* 0x14064044: [3:0] = 0xF when it is on */
#define G3D_ON                  0xfu
#define G3D_WAIT_US             100000u         /* the S9+ port waits 10 ms */
/*
 * The G3D's low-power settings from Samsung's pmucal_lpm_init, written before the domain goes off (offset, mask, value).
 * Without them the S9+ domain stopped half off (STATUS 0x002F000E) and the next GPU access was an SError.
 */
static const struct { uint32_t at, mask, value; const char *name; } g3d_lpm[] = {
	{ 0x4690, 0x3u, 0x2u, "MEMORY_EMBEDDED_G3D_OPTION" },
	{ 0x4680, 0x2u, 0x0u, "PWR_EMBEDDED_G3D_OPTION" },
	{ 0x4684, 0xfffu, 0x363u, "PWR_EMBEDDED_G3D_DURATION" },
	{ 0x15a8, 0x1u, 0x1u, "SCI_CRPPORTPWRDN" },
	{ 0x15ac, 0x1u, 0x1u, "PWR_EMBEDDED_G3D" },
};

#define CMU_G3D_PHYS            EXYNOS_CMU_G3D_PA
#define CMU_G3D_SIZE            0x2000          /* the bus dividers are at +0x1800 */
#define PLL_LOCKTIME            0x000
#define MUX_G3D_USER            0x100           /* PLL_CON0_MUX_CLKCMU_EMBEDDED_G3D_USER: MUX_SEL set on, cleared off */
#define PLL_CON0                0x140           /* PLL_G3D */
#define PLL_CON2                0x148           /* PLL_CON2_LOCK while the PLL locks and the domain is on, _OFF after */
#define MUX_CLK_G3D_BUSD        0x1000
#define DIV_CLK_G3D_BUSD        0x1800
#define DIV_CLK_G3D_BUSP        0x1804
#define CLK_CON_BUSY            (1u << 16)
#define MUX_SEL                 (1u << 4)       /* PLL_CON0: the PLL's output, not the oscillator; +0x100: CMU's clock */
#define MUX_BUSY                (1u << 7)       /* PLL_CON0: the mux still moving */
#define PLL_ENABLE              (1u << 31)
#define PLL_STABLE              (1u << 29)
#define PLL_PMS                 ((0x3ffu << 16) | (0x3fu << 8) | 7u)
#define PLL_CON2_LOCK           0x30000003u
#define PLL_CON2_OFF            0x10000001u
#define PLL_FIN_KHZ             26000ull
#define PLL_LOCK_FACTOR         150u            /* PLL_LOCKTIME = P x 150 */
#define PLL_STABLE_US           20000u
#define MUX_BUSY_US             1000u
/*
 * The rates (mali_mhz): the S9+ port's three, as M, P, S on the 26 MHz oscillator. Each must fit PLL_1018X's limits
 * (26 / P from 2 to 8 MHz, 26 x M / P from 600 to 1200 MHz; Samsung's pll_spec.c). The S9+ port's own values for 455
 * and 572 MHz put the VCO below 600 MHz, and 455's reference at 1 MHz, below 2. These differ for the same rates.
 */
#define G3D_RATES_MOST          4
static struct g3d_rate { uint32_t mhz, m, p, s, uv; } g3d_rates[G3D_RATES_MOST] = {   /* uv: the APM's, 0 unknown */
	{ 260, 160, 4, 2, 0 },                  /* S-Boot's own: the VCO at 1040 MHz */
	{ 455, 140, 4, 1, 0 },                  /* 910 */
	{ 572, 176, 4, 1, 0 },                  /* 1144 */
};
static unsigned g3d_nrates = 3;                 /* a fourth only with mali_max (g3d_apm_levels()) */
static uint32_t mali_max, mali_dvs, mali_dvs_margin = 25000, mali_dvs_floor = 800000;
static uint32_t mali_mhz = 260;
static bool pll_quiet;                          /* the governor's switches: a line only when one fails */
static uint32_t mali_pll = 1;                   /* mali_pll=0: PLL_G3D as the boot left it */

#define DTZPC_SMC               0x82000410u     /* Samsung's SiP call, SMC_CMD_PREPARE_PD_ONOFF */
#define DTZPC_SAVE              0u              /* before the domain's power goes */
#define DTZPC_RESTORE           1u              /* after it has come */
#define DTZPC_G3D               EXYNOS_MALI_DTZPC_G3D

#define MALI_PHYS               EXYNOS_MALI_PA
#define MALI_SIZE               0x5000
#define MALI_G72                0x6221u         /* GPU_ID >> 16 */

SYSCTL_DECL(_kern_hakios);              /* the node is drivers/core/hakios.platform.c's */

static vm_offset_t pmu, cmu_g3d, mali;
static uint32_t reached, gpu_id, shader_mask, l2_mask, last_step, step;
static bool powered_here;               /* step 2 switched the domain on: off again if the steps end short */
static bool gpu_answered;               /* step 4 read a G72: a soft reset before the power goes */
/*
 * The supply, through ACPM (hakios.exynos_acpm.c): BUCK6 (vdd_g3d). Only B6OUT is written, never above 0x68 (950 mV,
 * the regulator's maximum in the phone's device tree), and it is read back. B6CTRL is read for the log. The rail is put
 * back as found when this driver switches the domain off again. mali_acpm=0 does none of this.
 */
#define B6CTRL                  0x2a            /* [7:6]: 11 always on, 10 the G3D power pin's (g3d_en), 00 off */
#define B6OUT                   0x2b            /* 300 mV + 6.25 mV a step */
#define B6OUT_950MV             0x68
#define B6OUT_MOST              0x68
#define RAIL_SETTLE_US          200
#define RAIL_TRIES              10              /* 100 ms apart: ACPM may not answer early in boot */
static uint32_t mali_acpm = 1;
static bool acpm_ok, rail_changed, b6out_known;  /* b6out_known: B6OUT was read before it was written */
static uint8_t b6out_found;

SYSCTL_UINT(_kern_hakios, OID_AUTO, mali, CTLFLAG_RD | CTLFLAG_LOCKED, &reached, 0,
    "how far the Mali came up: 9 is the GPU there for programs (drivers/gpu/mali/hakios.mali.c)");
/*
 * What a user program reports about the GPU (written by userland/src/mali_test.c). Also logged once a minute, so it
 * survives in the kept end of the log.
 */
static uint32_t user_said;
SYSCTL_UINT(_kern_hakios, OID_AUTO, mali_user, CTLFLAG_RW | CTLFLAG_LOCKED | CTLFLAG_ANYBODY, &user_said, 0,
    "what user space found: 1 a job ran, 2 no service, 3 no client, 4 no buffer, 5 no mapping, 6 the job did nothing");

/*
 * kern.hakios.say: a line from a program, into the kernel's log (a job's own output is kept nowhere):
 * sysctlbyname("kern.hakios.say", NULL, NULL, line, strlen(line)).
 */
#define KEPT_LINES 96
static char kept_lines[KEPT_LINES][200];
static uint32_t kept_next;
static LCK_GRP_DECLARE(kept_lock_group, "hakios-said");
static LCK_MTX_DECLARE(kept_lock, &kept_lock_group);

/* A line from the kernel itself, kept and said again every minute as kern.hakios.say's "!" ones are. */
void
hakios_keep(const char *format, ...)
{
	char line[200];
	va_list ap;

	va_start(ap, format);
	vsnprintf(line, sizeof(line), format, ap);
	va_end(ap);
	printf("%s\n", line);
	lck_mtx_lock(&kept_lock);
	strlcpy(kept_lines[kept_next % KEPT_LINES], line, sizeof(kept_lines[0]));
	kept_next++;
	lck_mtx_unlock(&kept_lock);
}

static int
say_line(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2, struct sysctl_req *req)
{
	char line[512];
	size_t length = req->newlen;

	if (req->newptr == USER_ADDR_NULL) {
		return 0;
	}
	if (length > sizeof(line) - 1) {
		length = sizeof(line) - 1;
	}
	if (SYSCTL_IN(req, line, length) != 0) {
		return EINVAL;
	}
	line[length] = 0;
	printf("%s%s", line, length != 0 && line[length - 1] == '\n' ? "" : "\n");
	if (line[0] == '!') {                   /* kept, and said again every minute (mali_thread) */
		lck_mtx_lock(&kept_lock);
		strlcpy(kept_lines[kept_next % KEPT_LINES], line + 1, sizeof(kept_lines[0]));
		kept_next++;
		lck_mtx_unlock(&kept_lock);
	}
	return 0;
}
SYSCTL_PROC(_kern_hakios, OID_AUTO, say, CTLTYPE_STRING | CTLFLAG_WR | CTLFLAG_LOCKED | CTLFLAG_ANYBODY, NULL, 0,
    say_line, "A", "");

/*
 * gl_test's result: who drew it (1 the Mali, 2 the processor) times 1000, plus the share of the picture inside the
 * triangle in percent; and the middle pixel, red in the low byte.
 */
static uint32_t gl_said, gl_middle;
/*
 * sysctl kern.hakios.bind_cpu=N: the calling thread runs on logical processor N from now on (-1: on any). The
 * scheduler of this kernel does not tell big cores from small ones. On the X6826B, CoreAnimation's render thread spent
 * 14 to 18 ms a frame unbound. The Metal driver binds its render thread and its worker to the big cores.
 */
extern void *cpu_to_processor(int cpu);
extern void *thread_bind(void *processor);
extern int thread_block(void *continuation);
extern unsigned int real_ncpus;

static int
bind_cpu(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2, struct sysctl_req *req)
{
	int cpu = -1, error;

	error = SYSCTL_IN(req, &cpu, sizeof(cpu));
	if (error != 0 || req->newptr == USER_ADDR_NULL) {
		return error;
	}
	if (cpu >= (int)real_ncpus) {
		return EINVAL;
	}
	(void)thread_bind(cpu < 0 ? NULL : cpu_to_processor(cpu));
	(void)thread_block(NULL);             /* yield, so the new binding takes effect now */
	return 0;
}
SYSCTL_PROC(_kern_hakios, OID_AUTO, bind_cpu, CTLTYPE_INT | CTLFLAG_WR | CTLFLAG_LOCKED | CTLFLAG_ANYBODY, NULL, 0,
    bind_cpu, "I", "bind the calling thread to a logical processor");

SYSCTL_UINT(_kern_hakios, OID_AUTO, mali_gl, CTLFLAG_RW | CTLFLAG_LOCKED | CTLFLAG_ANYBODY, &gl_said, 0, "");
SYSCTL_UINT(_kern_hakios, OID_AUTO, mali_gl_middle, CTLFLAG_RW | CTLFLAG_LOCKED | CTLFLAG_ANYBODY, &gl_middle, 0, "");
SYSCTL_UINT(_kern_hakios, OID_AUTO, mali_id, CTLFLAG_RD | CTLFLAG_LOCKED, &gpu_id, 0, "GPU_ID");

static inline uint32_t
rd(vm_offset_t base, uint32_t at)
{
	return *(volatile uint32_t *)(base + at);
}

static inline void
wr(vm_offset_t base, uint32_t at, uint32_t value)
{
	*(volatile uint32_t *)(base + at) = value;
	__asm__ volatile ("dsb sy");
}

/* Until (register & mask) == want, for at most `us` microseconds by the clock (delay() may sleep far longer than asked). */
static bool
wait_for(vm_offset_t base, uint32_t at, uint32_t mask, uint32_t want, uint32_t us)
{
	uint64_t deadline;

	nanoseconds_to_absolutetime((uint64_t)us * NSEC_PER_USEC, &deadline);
	deadline += mach_absolute_time();
	while ((rd(base, at) & mask) != want) {
		if (mach_absolute_time() >= deadline) {
			return (rd(base, at) & mask) == want;
		}
	}
	return true;
}

/* `us` microseconds, by the clock */
static void
pause_us(uint32_t us)
{
	uint64_t deadline;

	nanoseconds_to_absolutetime((uint64_t)us * NSEC_PER_USEC, &deadline);
	deadline += mach_absolute_time();
	while (mach_absolute_time() < deadline) {
	}
}

/*
 * The GPU's registers are at 0x17500000 (0x5000 bytes; the device tree's mali@17500000). A Bifrost GPU needs no
 * firmware. GPU_ID >> 16 reads 0x6221 on this phone: architecture 6.2, product 1, the G72. Interrupts are SPI 282 (job),
 * 281 (MMU) and 283 (GPU), GIC INTIDs 314, 313 and 315 on the GIC-400. They are not used; the driver polls.
 * Coherency is none. The GPU does not look into the processors' caches, so the CPU cleans its cache around what the
 * GPU reads and writes in kernel memory (job pages, page tables). Program buffers are write-combined.
 */

/* The register offsets and bit names are panfrost's. */

#define GPU_ID              0x000
#define GPU_L2_FEATURES     0x004
#define GPU_CORE_FEATURES   0x008
#define GPU_TILER_FEATURES  0x00c
#define GPU_MEM_FEATURES    0x010
#define GPU_MMU_FEATURES    0x014       /* [7:0] bits of GPU address, [15:8] bits of physical address */
#define GPU_AS_PRESENT      0x018
#define GPU_JS_PRESENT      0x01c
#define GPU_INT_RAWSTAT     0x020
#define GPU_INT_CLEAR       0x024
#define GPU_INT_MASK        0x028
#define GPU_CMD             0x030
#define THREAD_MAX_THREADS  0x0a0
#define SHADER_PRESENT      0x100
#define SHADER_PRESENT_HI   0x104
#define TILER_PRESENT       0x110
#define L2_PRESENT          0x120
#define SHADER_READY        0x140
#define TILER_READY         0x150
#define L2_READY            0x160
#define SHADER_PWRON        0x180
#define TILER_PWRON         0x190
#define L2_PWRON            0x1a0
#define COHERENCY_FEATURES  0x300
#define COHERENCY_ENABLE    0x304
#define COHERENCY_NONE      31u
#define THREAD_TLS_ALLOC    0x310
#define IRQ_RESET_COMPLETED (1u << 8)
#define GPU_CMD_SOFT_RESET  1u
#define GPU_CMD_HARD_RESET  2u
#define GPU_JM_CONFIG       0xf00
#define GPU_SHADER_CONFIG   0xf04
#define GPU_TILER_CONFIG    0xf08
#define GPU_L2_MMU_CONFIG   0xf0c
#define L2_MMU_LIMIT_EXTERNAL (0xfu << 24)    /* outstanding external reads [25:24], writes [27:26]: 0, no limit */
#define RESET_WAIT_US       100000
#define CORES_WAIT_US       50000

#define JOB_INT_RAWSTAT     0x1000
#define JOB_INT_CLEAR       0x1004
#define JOB_INT_MASK        0x1008
#define JS1                 0x1880      /* job slot 1, where compute-like jobs go */
#define JS_STATUS           0x24
#define JS_HEAD_NEXT_LO     0x40
#define JS_HEAD_NEXT_HI     0x44
#define JS_AFFINITY_NEXT_LO 0x50
#define JS_AFFINITY_NEXT_HI 0x54
#define JS_CONFIG_NEXT      0x58
#define JS_COMMAND_NEXT     0x60
#define JS_CONFIG           ((3u << 8) | (3u << 12) | (8u << 16))  /* AS 0, caches flushed both ends */

#define AS0                 0x2400
#define AS_TRANSTAB_LO      0x00
#define AS_TRANSTAB_HI      0x04
#define AS_MEMATTR_LO       0x08
#define AS_MEMATTR_HI       0x0c
#define AS_COMMAND          0x18
#define AS_FAULTSTATUS      0x1c
#define AS_STATUS           0x28
#define AS_ACTIVE           1u
#define AS_WAIT_US          50000
#define AS_TRANSCFG_LO      0x30
#define AS_TRANSCFG_HI      0x34

/*
 * A reset as Linux panfrost makes it (panfrost_gpu_soft_reset(), panfrost_gpu_init_quirks()): interrupts masked, the
 * reset's own interrupt cleared, a soft reset, and a hard one (GPU_CMD = 2) if that does not finish. Afterwards two
 * settings are written again, since a reset clears them: COHERENCY_ENABLE = none, and, as panfrost does on a G72,
 * the L2_MMU_CONFIG limits on outstanding external reads and writes cleared. False if no reset finished.
 */
static bool
gpu_reset(void)
{
	wr(mali, GPU_INT_MASK, 0);
	wr(mali, GPU_INT_CLEAR, IRQ_RESET_COMPLETED);
	wr(mali, GPU_CMD, GPU_CMD_SOFT_RESET);
	if (!wait_for(mali, GPU_INT_RAWSTAT, IRQ_RESET_COMPLETED, IRQ_RESET_COMPLETED, RESET_WAIT_US)) {
		say("mali: the soft reset did not finish (GPU_INT_RAWSTAT %#x): a hard reset\n", rd(mali, GPU_INT_RAWSTAT));
		wr(mali, GPU_CMD, GPU_CMD_HARD_RESET);
		if (!wait_for(mali, GPU_INT_RAWSTAT, IRQ_RESET_COMPLETED, IRQ_RESET_COMPLETED, RESET_WAIT_US)) {
			return false;
		}
	}
	wr(mali, GPU_INT_CLEAR, 0xffffffffu);
	wr(mali, COHERENCY_ENABLE, COHERENCY_NONE);
	wr(mali, GPU_L2_MMU_CONFIG, rd(mali, GPU_L2_MMU_CONFIG) & ~L2_MMU_LIMIT_EXTERNAL);
	return true;
}

/*
 * The cores, powered in panfrost's order (panfrost_gpu_power_on()): the L2 first, then the shader cores, then the
 * tiler, each waited for. False, and which one, if one did not come up.
 */
static bool
gpu_cores_on(const char **which)
{
	uint32_t tiler = rd(mali, TILER_PRESENT);

	wr(mali, L2_PWRON, l2_mask);
	if (!wait_for(mali, L2_READY, l2_mask, l2_mask, CORES_WAIT_US)) {
		*which = "the L2";
		return false;
	}
	wr(mali, SHADER_PWRON, shader_mask);
	if (!wait_for(mali, SHADER_READY, shader_mask, shader_mask, CORES_WAIT_US)) {
		*which = "the shader cores";
		return false;
	}
	wr(mali, TILER_PWRON, tiler);
	if (!wait_for(mali, TILER_READY, tiler, tiler, CORES_WAIT_US)) {
		*which = "the tiler";
		return false;
	}
	return true;
}

/*
 * The smallest job there is, in a page of the kernel's own: a job header that says "write value", and its payload:
 * where to write, what kind of value (a 32-bit immediate), and the value. The target is the same page, 0x800 further
 * on. `gpu` is the address the GPU knows the page by: its physical address in identity mode, or wherever the page
 * tables map it. The processor's caches are cleaned on both sides, since the GPU does not look into them.
 */
static bool
run_write_job(vm_offset_t page, uint64_t gpu, const char *what)
{
	volatile uint32_t *job = (volatile uint32_t *)page, *target = (volatile uint32_t *)(page + 0x800);
	bool ok;

	bzero((void *)page, PAGE_SIZE);
	job[4] = (2u << 1) | 1u | (1u << 16);   /* write value; 64-bit pointers; job index 1 */
	job[8] = (uint32_t)(gpu + 0x800);
	job[9] = (uint32_t)((gpu + 0x800) >> 32);
	job[10] = 6;                            /* a 32-bit immediate */
	job[12] = 0xc0ffee01u;
	FlushPoC_DcacheRegion(page, PAGE_SIZE);

	wr(mali, JOB_INT_MASK, 0);
	wr(mali, JOB_INT_CLEAR, 0xffffffffu);
	wr(mali, JS1 + JS_HEAD_NEXT_LO, (uint32_t)gpu);
	wr(mali, JS1 + JS_HEAD_NEXT_HI, (uint32_t)(gpu >> 32));
	wr(mali, JS1 + JS_AFFINITY_NEXT_LO, shader_mask);
	wr(mali, JS1 + JS_AFFINITY_NEXT_HI, 0);
	wr(mali, JS1 + JS_CONFIG_NEXT, JS_CONFIG);
	wr(mali, JS1 + JS_COMMAND_NEXT, 1);     /* start */
	(void)wait_for(mali, JOB_INT_RAWSTAT, (1u << 1) | (1u << 17), 1u << 1, 100000);

	FlushPoC_DcacheRegion(page, PAGE_SIZE);
	ok = *target == 0xc0ffee01u;
	say("mali: %u: %s: interrupts %#x, slot status %#x, MMU fault %#x, job status %#x, the word %#x%s\n", step, what,
	    rd(mali, JOB_INT_RAWSTAT), rd(mali, JS1 + JS_STATUS), rd(mali, AS0 + AS_FAULTSTATUS), job[0], *target,
	    ok ? " -- the GPU ran it" : " -- NOT WRITTEN");
	wr(mali, JOB_INT_CLEAR, 0xffffffffu);
	return ok;
}

static void
as0_update(uint64_t transtab, uint32_t transcfg)
{
	(void)wait_for(mali, AS0 + AS_STATUS, AS_ACTIVE, 0, AS_WAIT_US);   /* no command while one runs (panfrost) */
	wr(mali, AS0 + AS_TRANSTAB_LO, (uint32_t)transtab);
	wr(mali, AS0 + AS_TRANSTAB_HI, (uint32_t)(transtab >> 32));
	wr(mali, AS0 + AS_TRANSCFG_LO, transcfg);
	wr(mali, AS0 + AS_TRANSCFG_HI, 0);
	wr(mali, AS0 + AS_MEMATTR_LO, 0x88888888u);     /* every attribute: write-back, the GPU's own caches */
	wr(mali, AS0 + AS_MEMATTR_HI, 0x88888888u);
	wr(mali, AS0 + AS_COMMAND, 1);                  /* update */
	(void)wait_for(mali, AS0 + AS_STATUS, AS_ACTIVE, 0, AS_WAIT_US);
}

/*
 * Page tables in the format Linux panfrost uses for Bifrost: AArch64 stage-1 tables with 4 KB pages (AS_TRANSCFG
 * mode 6). They translate 35 bits of GPU address in three levels: pt_top is indexed by bits 34..30, the next table by
 * 29..21, the last by 20..12. An entry holds the page's address, the access flag, inner shareable, read/write (AP 01),
 * memory attribute 0, and low bits 11 ("page").
 *
 * Where the walk starts. The G52 had 35 bits of GPU address, so INA_BITS (AS_TRANSCFG [10:6]) is 55 - 35 = 20 and the
 * walk starts at pt_top. The G72 is expected to have 48 bits. Both Linux drivers walk four levels from level 0 on such
 * a GPU: Samsung's kbase always, and panfrost with INA_BITS = 55 - 48. Nothing shows that a G72 honours INA_BITS = 20
 * with a walk from level 1. So on a GPU of more than 39 bits, the tables get a level 0 as well: one more page (pt_root),
 * whose entry 0 points at pt_top. Every GPU address here is below 2^35, so its other entries stay empty. AS_TRANSTAB
 * points at pt_root and INA_BITS = 55 - the GPU's bits. pt_transtab is what AS_TRANSTAB holds in either case.
 */
#define PTE_TABLE           3ull
#define PTE_PAGE            3ull
#define PTE_AF              (1ull << 10)
#define PTE_SH_INNER        (3ull << 8)
#define PTE_AP_RW           (1ull << 6)
#define PTE_ADDRESS         0x0000fffffffff000ull
#define GPU_VA_BITS         35u
#define GPU_PA_BITS_NEEDED  36u         /* the S9's memory: 0x80000000-0xFFFFFFFF and 0x880000000-0x8FFFFFFFF */
#define TRANSCFG_AARCH64    6u                          /* [3:0]: AArch64 tables, 4 KB pages (2 is identity) */
#define TRANSCFG_INA(bits)  ((55u - (bits)) << 6)       /* [10:6]: 55 - the bits of input address */
#define TRANSCFG_WALK_WB    ((2u << 24) | (1u << 30))   /* the walk's reads write-back, read-allocate (kbase's) */
#define TRANSCFG_WALK_NC    (1u << 24)                  /* ... not cached */
#define PT_FOUR_LEVELS_BITS 40u                         /* from here on, a GPU's walk is given level 0 as well */
/*
 * Boot-arg mali_fastmap (1 unless =0). The GPU walks its tables uncached, so an entry the processor wrote and cleaned
 * is what the next walk reads. A newly entered buffer then needs no MMU command, because an empty entry is one no TLB
 * holds. Before this, each new buffer was an UPDATE of address space 0 plus a wait: 2.4 ms a buffer, about 320 buffers
 * every five seconds in the window server. Removing a buffer still updates.
 */
static int mali_fastmap = 1;

static uint64_t *pt_top, *pt_root;      /* pt_root: level 0, on a GPU of more than 39 bits (above) */
static uint64_t pt_top_phys, pt_root_phys, pt_transtab;
static uint32_t pt_gpu_bits = GPU_VA_BITS;  /* the GPU's own, from step 4 (48 at most is used) */

static inline bool
pt_four_levels(void)
{
	return pt_gpu_bits >= PT_FOUR_LEVELS_BITS;
}

/* AS_TRANSCFG for the tables: AArch64, 4 KB pages, the input bits the walk starts from, how it reads */
static inline uint32_t
pt_transcfg(void)
{
	return TRANSCFG_AARCH64 | TRANSCFG_INA(pt_four_levels() ? pt_gpu_bits : GPU_VA_BITS) |
	       (mali_fastmap ? TRANSCFG_WALK_NC : TRANSCFG_WALK_WB);
}
static uint32_t pt_refused;             /* addresses not taken into, or not followed through, the tables */

/*
 * The kernel's address of the table an entry points to. NULL for an empty entry, and for an address that is not memory
 * the kernel manages (reported the first few times). The check comes before phystokv(), which panics on an address
 * outside kernel memory, and before the cache is cleaned over the table. PixOS on another phone panicked cleaning the
 * cache over an unchecked address. All tables are the kernel's own pages, so this should not refuse. If it does, an
 * entry was overwritten.
 */
static uint64_t *
table_of(uint64_t entry)
{
	uint64_t phys = entry & PTE_ADDRESS;

	if (!(entry & 1)) {
		return NULL;
	}
	if (phys == 0 || !pmap_valid_address(phys)) {
		if (pt_refused++ < 4) {
			printf("mali: a table entry %#llx points outside the kernel's memory: not followed\n", entry);
		}
		return NULL;
	}
	return (uint64_t *)phystokv(phys);
}

static uint64_t *
pt_page(uint64_t *phys)
{
	vm_offset_t page = 0;

	if (kmem_alloc(kernel_map, &page, PAGE_SIZE, KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		return NULL;
	}
	*phys = kvtophys(page);
	if (*phys == 0 || !pmap_valid_address(*phys)) {
		kmem_free(kernel_map, page, PAGE_SIZE);
		return NULL;
	}
	FlushPoC_DcacheRegion(page, PAGE_SIZE);  /* its zeroes in memory, where the GPU's walk reads them */
	return (uint64_t *)page;
}

/* The table below `entry`, made if there is none yet. */
static uint64_t *
pt_below(uint64_t *entry)
{
	uint64_t phys;
	uint64_t *table;

	if (*entry & 1) {
		return table_of(*entry);
	}
	table = pt_page(&phys);
	if (table == NULL) {
		return NULL;
	}
	*entry = phys | PTE_TABLE;
	FlushPoC_DcacheRegion((vm_offset_t)entry, sizeof(*entry));
	return table;
}

/* One 4 KB page of physical memory at GPU address `gpu` (with the lock held, or before anyone else runs). */
static bool
map_page(uint64_t gpu, uint64_t phys)
{
	uint64_t *middle, *bottom, *entry;

	if (gpu >= (1ull << GPU_VA_BITS)) {
		return false;
	}
	if (phys == 0 || (!pmap_valid_address(phys) && !hakios_scanout_carved(phys) && !hakios_mfc_carved(phys))) {
		/*
		 * The GPU is given memory the kernel manages and nothing else (not a register block, not firmware memory),
		 * except the DPU's scan-out pool, where the display surfaces the window server draws into are.
		 */
		if (pt_refused++ < 4) {
			printf("mali: physical address %#llx is not memory the kernel manages: not entered at %#llx\n", phys, gpu);
		}
		return false;
	}
	if (pt_top == NULL && (pt_top = pt_page(&pt_top_phys)) == NULL) {
		return false;
	}
	if (pt_four_levels() && pt_root == NULL) {
		/* level 0 above it: entry 0 covers GPU addresses 0 to 2^39, and every one here is below 2^35 */
		if ((pt_root = pt_page(&pt_root_phys)) == NULL) {
			return false;
		}
		pt_root[0] = pt_top_phys | PTE_TABLE;
		FlushPoC_DcacheRegion((vm_offset_t)pt_root, sizeof(pt_root[0]));
	}
	pt_transtab = pt_root != NULL ? pt_root_phys : pt_top_phys;
	if ((middle = pt_below(&pt_top[(gpu >> 30) & 31])) == NULL || (bottom = pt_below(&middle[(gpu >> 21) & 511])) == NULL) {
		return false;
	}
	entry = &bottom[(gpu >> 12) & 511];
	*entry = (phys & PTE_ADDRESS) | PTE_AF | PTE_SH_INNER | PTE_AP_RW | PTE_PAGE;
	FlushPoC_DcacheRegion((vm_offset_t)entry, sizeof(*entry));
	return true;
}

/* The entries of `size` bytes from GPU address `gpu` emptied, as far as there are tables (with the lock held). */
static void
unmap_pages(uint64_t gpu, uint64_t size)
{
	if (pt_top == NULL) {
		return;
	}
	for (uint64_t at = gpu; at < gpu + size; at += 4096) {        /* the GPU's pages, not the kernel's 16 KB */
		uint64_t *table = table_of(pt_top[(at >> 30) & 31]), *entry;

		if (table == NULL) {
			continue;
		}
		table = table_of(table[(at >> 21) & 511]);
		if (table == NULL) {
			continue;
		}
		entry = &table[(at >> 12) & 511];
		*entry = 0;
		FlushPoC_DcacheRegion((vm_offset_t)entry, sizeof(*entry));
	}
}

/* PLL_G3D as it is, said; its PLL_CON0 back. */
static uint32_t
pll_say(const char *when)
{
	uint32_t con0 = rd(cmu_g3d, PLL_CON0);
	uint32_t m = (con0 >> 16) & 0x3ff, p = (con0 >> 8) & 0x3f, s = con0 & 7;
	uint32_t khz = p != 0 ? (uint32_t)(PLL_FIN_KHZ * m / ((uint64_t)p << s)) : 0;

	say("mali: %u: PLL_G3D %s: PLL_CON0 %#x: M %u, P %u, S %u, %u.%03u MHz from 26 MHz; %s, %s; the G3D on %s%s; "
	    "PLL_CON2 %#x\n", step, when, con0, m, p, s, khz / 1000, khz % 1000, (con0 & PLL_ENABLE) ? "enabled" :
	    "not enabled", (con0 & PLL_STABLE) ? "stable" : "NOT STABLE", (con0 & MUX_SEL) ? "the PLL" : "the oscillator",
	    (con0 & MUX_BUSY) ? " (the mux still moving)" : "", rd(cmu_g3d, PLL_CON2));
	return con0;
}

static bool
pll_is(const struct g3d_rate *r)
{
	uint32_t con0 = rd(cmu_g3d, PLL_CON0);

	return ((con0 >> 16) & 0x3ff) == r->m && ((con0 >> 8) & 0x3f) == r->p && (con0 & 7) == r->s &&
	       (con0 & (PLL_ENABLE | PLL_STABLE | MUX_SEL)) == (PLL_ENABLE | PLL_STABLE | MUX_SEL);
}

/*
 * PLL_G3D to rate `r`, in the order the S9+ port uses (s9p-g3d.c). PLL_CON2 = 0x30000003 (S-Boot's 0x10000001 keeps the
 * PLL from locking), then the lock time. The G3D moves to the oscillator and the mux is waited for. The PLL is stopped
 * and its dividers cleared, M/P/S set, then ENABLE. STABLE is waited for, and the G3D moves back to the PLL with the mux
 * waited for. On failure the PLL is put back as it was found.
 */
static bool
pll_switch(const struct g3d_rate *r)
{
	uint32_t old_con0 = rd(cmu_g3d, PLL_CON0), old_locktime = rd(cmu_g3d, PLL_LOCKTIME), v;
	const char *failed;

	if (!pll_quiet) {
		say("mali: %u: PLL_G3D to %u MHz (M %u, P %u, S %u): PLL_CON2 = %#x, PLL_LOCKTIME = %u, the G3D on the "
		    "oscillator, the PLL relocked, the G3D back on it\n", step, r->mhz, r->m, r->p, r->s, PLL_CON2_LOCK,
		    r->p * PLL_LOCK_FACTOR);
	}
	wr(cmu_g3d, PLL_CON2, PLL_CON2_LOCK);
	wr(cmu_g3d, PLL_LOCKTIME, r->p * PLL_LOCK_FACTOR);
	wr(cmu_g3d, PLL_CON0, rd(cmu_g3d, PLL_CON0) & ~MUX_SEL);
	if (!wait_for(cmu_g3d, PLL_CON0, MUX_BUSY, 0, MUX_BUSY_US)) {
		failed = "the mux did not move to the oscillator";
		goto restore;
	}
	v = rd(cmu_g3d, PLL_CON0) & ~(PLL_ENABLE | PLL_PMS);
	wr(cmu_g3d, PLL_CON0, v);
	v |= r->m << 16 | r->p << 8 | r->s;
	wr(cmu_g3d, PLL_CON0, v);
	wr(cmu_g3d, PLL_CON0, v | PLL_ENABLE);
	if (!wait_for(cmu_g3d, PLL_CON0, PLL_STABLE, PLL_STABLE, PLL_STABLE_US)) {
		failed = "the PLL did not lock";
		goto restore;
	}
	wr(cmu_g3d, PLL_CON0, rd(cmu_g3d, PLL_CON0) | MUX_SEL);
	if (!wait_for(cmu_g3d, PLL_CON0, MUX_BUSY, 0, MUX_BUSY_US) || !pll_is(r)) {
		failed = "the mux did not move back to the PLL";
		goto restore;
	}
	if (!pll_quiet) {
		(void)pll_say("switched");
	}
	return true;

restore:
	(void)pll_say(failed);
	say("mali: %u: %s: PLL_G3D put back as it was found (PLL_CON0 %#x)\n", step, failed, old_con0);
	wr(cmu_g3d, PLL_CON0, rd(cmu_g3d, PLL_CON0) & ~MUX_SEL);
	if (wait_for(cmu_g3d, PLL_CON0, MUX_BUSY, 0, MUX_BUSY_US)) {
		wr(cmu_g3d, PLL_CON0, rd(cmu_g3d, PLL_CON0) & ~PLL_ENABLE);
		wr(cmu_g3d, PLL_LOCKTIME, old_locktime);
		wr(cmu_g3d, PLL_CON0, old_con0 & ~MUX_SEL);
		if (!(old_con0 & PLL_ENABLE) || wait_for(cmu_g3d, PLL_CON0, PLL_STABLE, PLL_STABLE, PLL_STABLE_US)) {
			wr(cmu_g3d, PLL_CON0, old_con0);
			(void)wait_for(cmu_g3d, PLL_CON0, MUX_BUSY, 0, MUX_BUSY_US);
		}
	}
	(void)pll_say("put back");
	return false;
}

/* The rail's voltage, said; its B6OUT in `out`. False if ACPM did not answer. */
static bool
rail_read(const char *when, uint8_t *out)
{
	uint8_t ctrl = 0, vsel = 0;
	int error = hakios_acpm_pmic_read(HAKIOS_ACPM_PMIC_PMIC, B6CTRL, &ctrl);

	if (error == 0) {
		error = hakios_acpm_pmic_read(HAKIOS_ACPM_PMIC_PMIC, B6OUT, &vsel);
	}
	if (error != 0) {
		say("mali: %u: ACPM: BUCK6 (vdd_g3d) %s: NOT READ, error %d\n", step, when, error);
		return false;
	}
	say("mali: %u: ACPM: BUCK6 (vdd_g3d) %s: B6CTRL %#x ([7:6] = %u: %s), B6OUT %#x = %u mV\n", step, when, ctrl,
	    (ctrl >> 6) & 3, (ctrl >> 6) == 3 ? "always on" : (ctrl >> 6) == 2 ? "the G3D power pin's" : (ctrl >> 6) == 1 ?
	    "on in standby" : "OFF", vsel, 300 + vsel * 25 / 4);
	*out = vsel;
	return true;
}

/*
 * B6OUT = `vsel`, read back. This is the only PMIC register the driver writes, and never above 950 mV. Tried
 * RAIL_TRIES times, 100 ms apart, since the S9+ port found ACPM not answering early in boot.
 */
static bool
rail_set(uint8_t vsel)
{
	uint8_t now = 0;

	if (vsel > B6OUT_MOST) {
		return false;
	}
	for (int attempt = 1; attempt <= RAIL_TRIES; attempt++) {
		int error = hakios_acpm_pmic_write(HAKIOS_ACPM_PMIC_PMIC, B6OUT, vsel);

		if (error == 0) {
			error = hakios_acpm_pmic_read(HAKIOS_ACPM_PMIC_PMIC, B6OUT, &now);
		}
		if (error == 0 && now == vsel) {
			pause_us(RAIL_SETTLE_US);
			return true;
		}
		say("mali: %u: ACPM: B6OUT = %#x, attempt %d of %d: %s %#x\n", step, vsel, attempt, RAIL_TRIES,
		    error ? "error" : "it reads", error ? (unsigned)error : now);
		delay_for_interval(100, NSEC_PER_MSEC);
	}
	return false;
}

/* One SMC to EL3 (through Carbon at EL2): x0 back. */
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

/*
 * Power the G3D domain off again, undoing step 2. A GPU that answered is reset first, so nothing runs when the power
 * goes. If the protection cannot be saved, or the domain does not go off, it is switched on again and left on: a
 * half-off domain must not be read. BUCK6 is put back as found once the domain is off.
 */
static void
g3d_off(void)
{
	uint64_t answer;

	if (gpu_answered) {
		say("mali: before the power goes: a reset\n");
		if (!gpu_reset()) {
			say("mali: the reset did not finish (GPU_INT_RAWSTAT %#x): the power goes anyway\n", rd(mali, GPU_INT_RAWSTAT));
		}
		gpu_answered = false;
	}
	if ((rd(pmu, PMU_G3D_STATUS) & G3D_ON) != G3D_ON) {
		/* No save: the protection controller is inside the domain and cannot be saved without power. */
		say("mali: the G3D domain is not on (status %#x): no DTZPC save; its configuration [3:0] = 0\n",
		    rd(pmu, PMU_G3D_STATUS));
		wr(pmu, PMU_G3D_CONFIGURATION, rd(pmu, PMU_G3D_CONFIGURATION) & ~G3D_ON);
		(void)wait_for(pmu, PMU_G3D_STATUS, G3D_ON, 0, G3D_WAIT_US);
		goto clocks;
	}
	say("mali: switching the G3D domain off again, as it was found: first Samsung's low-power settings for it\n");
	for (unsigned i = 0; i < sizeof(g3d_lpm) / sizeof(g3d_lpm[0]); i++) {
		uint32_t was = rd(pmu, g3d_lpm[i].at);

		wr(pmu, g3d_lpm[i].at, (was & ~g3d_lpm[i].mask) | g3d_lpm[i].value);
		say("mali: PMU +%#x %s: %#x -> %#x\n", g3d_lpm[i].at, g3d_lpm[i].name, was, rd(pmu, g3d_lpm[i].at));
	}
	answer = smc(DTZPC_SMC, DTZPC_SAVE, DTZPC_G3D, 0);
	say("mali: SMC %#x (%u, %#x): the DTZPC save, returned %#llx\n", DTZPC_SMC, DTZPC_SAVE, DTZPC_G3D, answer);
	if ((uint32_t)answer != 0) {
		say("mali: the protection was not saved: the domain is left ON\n");
		powered_here = false;
		return;
	}
	wr(pmu, PMU_G3D_CONFIGURATION, rd(pmu, PMU_G3D_CONFIGURATION) & ~G3D_ON);
	if (!wait_for(pmu, PMU_G3D_STATUS, G3D_ON, 0, G3D_WAIT_US)) {
		say("mali: the domain did not go off: status %#x after %u ms; switching it on again\n", rd(pmu, PMU_G3D_STATUS),
		    G3D_WAIT_US / 1000);
		wr(pmu, PMU_G3D_CONFIGURATION, rd(pmu, PMU_G3D_CONFIGURATION) | G3D_ON);
		if (wait_for(pmu, PMU_G3D_STATUS, G3D_ON, G3D_ON, G3D_WAIT_US)) {
			answer = smc(DTZPC_SMC, DTZPC_RESTORE, DTZPC_G3D, 0);
			say("mali: on again; the DTZPC restore returned %#llx; the domain is left ON\n", answer);
		} else {
			say("mali: NOT ON AGAIN EITHER: status %#x; nothing of the G3D is to be touched\n", rd(pmu, PMU_G3D_STATUS));
		}
		powered_here = false;
		return;
	}
clocks:
	wr(cmu_g3d, MUX_G3D_USER, rd(cmu_g3d, MUX_G3D_USER) & ~MUX_SEL);
	wr(cmu_g3d, PLL_CON0, rd(cmu_g3d, PLL_CON0) & ~MUX_SEL);
	wr(cmu_g3d, PLL_CON2, PLL_CON2_OFF);
	say("mali: the G3D domain: configuration %#x, status %#x; CMU_G3D +0x100 %#x, PLL_CON0 %#x, PLL_CON2 %#x\n",
	    rd(pmu, PMU_G3D_CONFIGURATION), rd(pmu, PMU_G3D_STATUS), rd(cmu_g3d, MUX_G3D_USER), rd(cmu_g3d, PLL_CON0),
	    rd(cmu_g3d, PLL_CON2));
	if (rail_changed) {
		say("mali: BUCK6 back as it was found: B6OUT %#x (%u mV)\n", b6out_found, 300 + b6out_found * 25 / 4);
		if (b6out_found <= B6OUT_MOST && rail_set(b6out_found)) {
			rail_changed = false;
		} else {
			say("mali: B6OUT not put back (it reads otherwise, or ACPM did not answer)\n");
		}
	}
	powered_here = false;
}

/*
 * The steps. mali=N runs the first N and stops; 0 or unset leaves the GPU alone. Each step says what it will touch
 * before it does. A step that does not find what it needs ends the sequence.
 *   1  reads   the PMU's G3D configuration and status; CMU_G3D's PLL, muxes and bus dividers; BUCK6 through ACPM.
 *              Nothing is written, and the GPU itself is not read: the rail is as S-Boot left it.
 *   2  writes  B6OUT = 950 mV, read back (or the sequence ends). PLL_G3D to the rate (mali_mhz). If the domain is
 *              off, then its power-on.
 *   3  asks    if step 2 switched the domain on: the DTZPC restore by SMC to EL3, then the bus dividers.
 *   4  reads   GPU_ID (must be a G72), MMU_FEATURES (at least 35 bits of GPU address, 36 of physical), the core masks.
 *   5  writes  a reset (soft, hard if that does not finish); COHERENCY_ENABLE = none; L2_MMU_CONFIG limits cleared.
 *   6  writes  the L2, the shader cores and the tiler powered up, each waited for.
 *   7  runs    one job, in identity mode (address space 0, no tables): it writes a word into memory.
 *   8  runs    the same job through the GPU's own page tables.
 *   9  serves  HakiosMali starts: programs can use the GPU.
 * sysctl kern.hakios.mali reports the last step done. 9 means the GPU is there for programs.
 */

/* 1: what the boot has left, read only; not the GPU itself. */
static bool
step_look(void)
{
	uint32_t status;

	say("mali: 1: mapping the PMU (%#lx), CMU_G3D (%#lx) and the GPU (%#lx); nothing is written in this step\n", PMU_PHYS,
	    CMU_G3D_PHYS, MALI_PHYS);
	pmu = ml_io_map(PMU_PHYS, PMU_SIZE);
	cmu_g3d = ml_io_map(CMU_G3D_PHYS, CMU_G3D_SIZE);
	mali = ml_io_map(MALI_PHYS, MALI_SIZE);
	if (pmu == 0 || cmu_g3d == 0 || mali == 0) {
		say("mali: 1: cannot map them\n");
		return false;
	}
	say("mali: 1: reading the PMU's G3D configuration (%#lx) and status (%#lx)\n", PMU_PHYS + PMU_G3D_CONFIGURATION,
	    PMU_PHYS + PMU_G3D_STATUS);
	status = rd(pmu, PMU_G3D_STATUS);
	say("mali: 1: G3D configuration %#x, status %#x: the domain is %s\n", rd(pmu, PMU_G3D_CONFIGURATION), status,
	    (status & G3D_ON) == G3D_ON ? "on" : (status & G3D_ON) == 0 ? "off" : "NEITHER ON NOR OFF");
	say("mali: 1: reading CMU_G3D (outside the domain: it answers with the domain off)\n");
	(void)pll_say("as found");
	say("mali: 1: CMU_G3D +0x100 %#x (the G3D on %s), MUX_CLK_G3D_BUSD %#x, DIV_CLK_G3D_BUSD %#x, DIV_CLK_G3D_BUSP %#x\n",
	    rd(cmu_g3d, MUX_G3D_USER), (rd(cmu_g3d, MUX_G3D_USER) & MUX_SEL) ? "CMU's clock" : "the oscillator",
	    rd(cmu_g3d, MUX_CLK_G3D_BUSD), rd(cmu_g3d, DIV_CLK_G3D_BUSD), rd(cmu_g3d, DIV_CLK_G3D_BUSP));
	say("mali: 1: the GPU's own registers are not read before step 2 has raised its supply (S-Boot's 260 MHz at 644 mV "
	    "hung the S9+'s SoC when its GPU was read and reset)\n");
	if (mali_acpm && (acpm_ok = hakios_acpm_init(say))) {
		b6out_known = rail_read("as found", &b6out_found);
	} else if (mali_acpm) {
		say("mali: 1: no ACPM: the rail cannot be raised, and step 2 will stop there\n");
	}
	return true;
}

/*
 * The APM's own G3D levels (fvmap, DVFS index 4: Apollo cal-if/exynos9810/acpm_dvfs_exynos9810.h:7, the mali node's
 * g3d_cmu_cal_id 0xb040004 at star2lte_kor_26.dts:5323): read once, said, each of our rates given the voltage the APM
 * applies with it (or with the slowest of its levels at least as fast), and with mali_max a level above 572 MHz added
 * when the APM has one. Samsung's star2 tree: gpu_dvfs_table 572/546/455/338/299/260 MHz, gpu_max_clock_limit 572000
 * (dts:5330, :5336), gpu_default_voltage 800000 (:5340); BUCK6 (vdd_g3d) regulator-max-microvolt 950000 (:9055).
 */
#define G3D_DVFS                4
#define G3D_SAMSUNG_MAX_MHZ     572
#define G3D_UV_MOST             950000u
#define G3D_APM_LEVELS          24

/* M, P, S for `mhz` on PLL_1018X (Apollo pll_spec.c:169-178: 26 / P 2..8 MHz, M 64..1023, S 0..6, VCO 600..1200 MHz) */
static bool
g3d_mps(uint32_t mhz, struct g3d_rate *r)
{
	for (int s = 6; s >= 0; s--) {                  /* the highest VCO that fits first */
		uint64_t vco = (uint64_t)mhz * 1000 << s;       /* kHz */

		if (vco < 600000 || vco > 1200000) {
			continue;
		}
		for (uint32_t p = 4; p <= 13; p++) {
			if ((vco * p) % 26000 == 0 && vco * p / 26000 >= 64 && vco * p / 26000 <= 1023) {
				r->mhz = mhz;
				r->m = (uint32_t)(vco * p / 26000);
				r->p = p;
				r->s = (uint32_t)s;
				return true;
			}
		}
	}
	return false;
}

static void
g3d_apm_levels(void)
{
	static bool done;
	uint32_t khz[G3D_APM_LEVELS], uv[G3D_APM_LEVELS], n, len = 0;
	char line[400];

	if (done || !acpm_ok) {
		return;
	}
	done = true;
	line[0] = 0;
	n = hakios_acpm_dvfs_table(G3D_DVFS, khz, uv, G3D_APM_LEVELS, say);
	if (n == 0) {
		say("mali: 2: the APM's G3D table (DVFS %u) was not read: the three rates, BUCK6 at 950 mV\n", G3D_DVFS);
		return;
	}
	for (uint32_t l = 0; l < n && len < sizeof(line) - 24; l++) {
		len += (uint32_t)snprintf(line + len, sizeof(line) - len, " %u/%u", khz[l], uv[l]);
	}
	say("mali: 2: the APM's G3D table (DVFS %u), kHz/uV:%s; its fastest %u MHz (Samsung's limit %u MHz)\n", G3D_DVFS,
	    line, khz[0] / 1000, G3D_SAMSUNG_MAX_MHZ);
	for (unsigned i = 0; i < g3d_nrates; i++) {
		for (uint32_t l = 0; l < n && khz[l] >= g3d_rates[i].mhz * 1000; l++) {
			g3d_rates[i].uv = uv[l];        /* the fastest first: the last that is fast enough */
		}
	}
	if (mali_max > G3D_SAMSUNG_MAX_MHZ && g3d_nrates < G3D_RATES_MOST) {
		struct g3d_rate r = { 0 };
		uint32_t l = 0;

		while (l < n && khz[l] > mali_max * 1000) {
			l++;
		}
		if (l >= n || khz[l] <= G3D_SAMSUNG_MAX_MHZ * 1000) {
			say("mali: 2: mali_max=%u: the APM has no G3D level above %u MHz up to it: nothing added\n", mali_max,
			    G3D_SAMSUNG_MAX_MHZ);
		} else if (uv[l] > G3D_UV_MOST) {
			say("mali: 2: mali_max=%u: the APM's %u kHz wants %u uV, above BUCK6's 950 mV: not added\n", mali_max,
			    khz[l], uv[l]);
		} else if (khz[l] % 1000 != 0 || !g3d_mps(khz[l] / 1000, &r)) {
			say("mali: 2: mali_max=%u: no PLL_1018X M/P/S gives %u kHz exactly: not added\n", mali_max, khz[l]);
		} else {
			r.uv = uv[l];
			g3d_rates[g3d_nrates++] = r;
			say("mali: 2: mali_max=%u: %u MHz added above Samsung's 572 (M %u, P %u, S %u: the VCO at %u MHz; the APM's "
			    "voltage %u uV)\n", mali_max, r.mhz, r.m, r.p, r.s, r.mhz << r.s, r.uv);
		}
	}
	for (unsigned i = 0; i < g3d_nrates; i++) {
		say("mali: 2: %u MHz: the APM's voltage %u uV\n", g3d_rates[i].mhz, g3d_rates[i].uv);
	}
}

/* 2: the supply, the clock, power. */
static bool
step_power(void)
{
	const struct g3d_rate *rate = NULL;
	uint32_t status = rd(pmu, PMU_G3D_STATUS) & G3D_ON;

	if (status != G3D_ON && status != 0) {
		say("mali: 2: the domain is neither on nor off (status %#x): left alone\n", rd(pmu, PMU_G3D_STATUS));
		return false;
	}
	/* the rail first, before the clock moves and before anything of the GPU is touched */
	if (!mali_acpm) {
		say("mali: 2: mali_acpm=0: BUCK6 left as found -- the GPU will be touched at the boot's voltage\n");
	} else if (!acpm_ok) {
		say("mali: 2: no ACPM: BUCK6 cannot be raised; the GPU is not touched at the boot's voltage\n");
		return false;
	} else if (!b6out_known && !(b6out_known = rail_read("as found, read again", &b6out_found))) {
		say("mali: 2: B6OUT cannot be read: not written either (ACPM does not answer); the GPU is not touched at the "
		    "boot's voltage\n");
		return false;
	} else if (b6out_found >= B6OUT_950MV) {
		say("mali: 2: B6OUT is %#x (%u mV) already: not written\n", b6out_found, 300 + b6out_found * 25 / 4);
	} else {
		uint8_t now = 0;

		say("mali: 2: ACPM: B6OUT = %#x (950 mV, was %#x = %u mV), read back, then %u us\n", B6OUT_950MV, b6out_found,
		    300 + b6out_found * 25 / 4, RAIL_SETTLE_US);
		rail_changed = true;
		if (!rail_set(B6OUT_950MV)) {
			say("mali: 2: BUCK6 is not at 950 mV: the GPU is not touched at the boot's voltage\n");
			(void)rail_read("now", &now);
			return false;
		}
		(void)rail_read("now", &now);
	}
	/* the clock: PLL_G3D at the rate asked for */
	g3d_apm_levels();
	for (unsigned i = 0; i < g3d_nrates; i++) {
		if (g3d_rates[i].mhz == mali_mhz) {
			rate = &g3d_rates[i];
		}
	}
	if (rate == NULL) {
		say("mali: 2: mali_mhz=%u is not one of the rates (260, 455, 572, or mali_max's): 260\n", mali_mhz);
		rate = &g3d_rates[0];
	}
	if (pll_is(rate)) {
		say("mali: 2: PLL_G3D runs at %u MHz already, the G3D on it: not touched\n", rate->mhz);
	} else if (!mali_pll) {
		uint32_t con0 = pll_say("left as found (mali_pll=0)");

		if ((con0 & (PLL_ENABLE | PLL_STABLE)) != (PLL_ENABLE | PLL_STABLE)) {
			say("mali: 2: PLL_G3D does not run, and mali_pll=0 leaves it so: the GPU would have no clock\n");
			return false;
		}
	} else if (!pll_switch(rate)) {
		return false;
	}
	if (status == G3D_ON) {
		say("mali: 2: the domain is on already: no power-on; it is left on whatever happens\n");
		return true;
	}
	/* the power-on: PLL_CON2 and PLL_CON0's MUX_SEL as the switch left them (or the boot, with mali_pll=0) */
	say("mali: 2: the power-on: PLL_CON2 = %#x (was %#x), PLL_CON0 |= MUX_SEL, CMU_G3D +0x100 |= bit 4 (was %#x)\n",
	    PLL_CON2_LOCK, rd(cmu_g3d, PLL_CON2), rd(cmu_g3d, MUX_G3D_USER));
	powered_here = true;            /* from the first write on, the way back is g3d_off() */
	wr(cmu_g3d, PLL_CON2, PLL_CON2_LOCK);
	wr(cmu_g3d, PLL_CON0, rd(cmu_g3d, PLL_CON0) | MUX_SEL);
	wr(cmu_g3d, MUX_G3D_USER, rd(cmu_g3d, MUX_G3D_USER) | MUX_SEL);
	say("mali: 2: writing the PMU's G3D configuration [3:0] = 0xf (was %#x), then waiting up to %u ms for its status\n",
	    rd(pmu, PMU_G3D_CONFIGURATION), G3D_WAIT_US / 1000);
	wr(pmu, PMU_G3D_CONFIGURATION, (rd(pmu, PMU_G3D_CONFIGURATION) & ~G3D_ON) | G3D_ON);
	if (!wait_for(pmu, PMU_G3D_STATUS, G3D_ON, G3D_ON, G3D_WAIT_US)) {
		say("mali: 2: the domain did not come up: status %#x after %u ms\n", rd(pmu, PMU_G3D_STATUS), G3D_WAIT_US / 1000);
		return false;
	}
	say("mali: 2: the domain is on: status %#x\n", rd(pmu, PMU_G3D_STATUS));
	return true;
}

/*
 * Step 3, after our power-on: EL3 restores the G3D's TrustZone protection (DTZPC restore), then the bus dividers are
 * set. Without the restore, the first GPU access is refused by the bus with an SError.
 */
static bool
step_dtzpc(void)
{
	uint64_t answer;

	if (!powered_here) {
		say("mali: 3: the boot left the domain on: its protection and bus dividers stand (the S9+ port restores them only "
		    "after a power-on of its own); nothing done\n");
		return true;
	}
	say("mali: 3: about to make SMC %#x (%u, %#x, 0): the DTZPC restore, after the G3D's power-on; Carbon, at EL2, passes "
	    "it on to EL3\n", DTZPC_SMC, DTZPC_RESTORE, DTZPC_G3D);
	answer = smc(DTZPC_SMC, DTZPC_RESTORE, DTZPC_G3D, 0);
	say("mali: 3: SMC %#x returned %#llx\n", DTZPC_SMC, answer);
	if ((uint32_t)answer != 0) {
		say("mali: 3: not 0: %s; the GPU is not read (it would be an SError)\n", (uint32_t)answer == 0xffffffffu ?
		    "the call is not known to whoever answered it (SMCCC's NOT_SUPPORTED)" : "a refusal");
		return false;
	}
	say("mali: 3: the bus dividers: MUX_CLK_G3D_BUSD = 0, DIV_CLK_G3D_BUSD = 0 (the PLL's rate), DIV_CLK_G3D_BUSP = 3 (a "
	    "quarter of it); were %#x, %#x, %#x\n", rd(cmu_g3d, MUX_CLK_G3D_BUSD), rd(cmu_g3d, DIV_CLK_G3D_BUSD),
	    rd(cmu_g3d, DIV_CLK_G3D_BUSP));
	wr(cmu_g3d, MUX_CLK_G3D_BUSD, 0);
	wr(cmu_g3d, DIV_CLK_G3D_BUSD, 0);
	wr(cmu_g3d, DIV_CLK_G3D_BUSP, 3);
	if (!wait_for(cmu_g3d, DIV_CLK_G3D_BUSP, CLK_CON_BUSY, 0, MUX_BUSY_US)) {
		say("mali: 3: DIV_CLK_G3D_BUSP still busy: %#x\n", rd(cmu_g3d, DIV_CLK_G3D_BUSP));
		return false;
	}
	return true;
}

/* 4: which GPU, and what it has. */
static bool
step_gpu(void)
{
	uint32_t mmu, va_bits, pa_bits, shaders_hi, as_present, js_present, tiler;

	say("mali: 4: reading GPU_ID at %#lx, the first read of the GPU: should the run end here, the domain says on and the "
	    "GPU does not answer (its supply, its clock, or its protection)\n", MALI_PHYS);
	gpu_id = rd(mali, GPU_ID);
	say("mali: 4: GPU_ID %#x: architecture %u.%u revision %u, product %u, version r%up%u, status %u\n", gpu_id,
	    gpu_id >> 28, (gpu_id >> 24) & 0xf, (gpu_id >> 20) & 0xf, (gpu_id >> 16) & 0xf, (gpu_id >> 12) & 0xf,
	    (gpu_id >> 4) & 0xff, gpu_id & 0xf);
	if ((gpu_id >> 16) != MALI_G72) {
		if (((gpu_id >> 16) & 0xf00f) != 0x6001) {
			say("mali: 4: not a Mali-G72 (GPU_ID >> 16 should be %#x): nothing more is done with it\n", MALI_G72);
			return false;
		}
		say("mali: 4: a G72 of another revision than the %#x expected: going on\n", MALI_G72);
	}
	mmu = rd(mali, GPU_MMU_FEATURES);
	va_bits = mmu & 0xff;
	pa_bits = (mmu >> 8) & 0xff;
	say("mali: 4: MMU_FEATURES %#x: %u bits of GPU address, %u of physical (the page tables want %u and %u; the G52 "
	    "the driver was written for had 0x2823)\n", mmu, va_bits, pa_bits, GPU_VA_BITS, GPU_PA_BITS_NEEDED);
	if (va_bits < GPU_VA_BITS) {
		say("mali: 4: fewer than %u bits of GPU address: the page tables here start at bit 34; not going on\n", GPU_VA_BITS);
		return false;
	}
	if (pa_bits < GPU_PA_BITS_NEEDED) {
		say("mali: 4: fewer than %u bits of physical address, and this phone's memory reaches 0x8FFFFFFFF: not going on\n",
		    GPU_PA_BITS_NEEDED);
		return false;
	}
	pt_gpu_bits = va_bits > 48 ? 48 : va_bits;
	say("mali: 4: the GPU's tables will be walked %s; AS_TRANSCFG %#x\n", pt_four_levels() ?
	    "from level 0, four levels, as kbase and panfrost have a GPU of this many bits walk them" :
	    "from level 1, three levels, as on the G52", pt_transcfg());
	shader_mask = rd(mali, SHADER_PRESENT);
	shaders_hi = rd(mali, SHADER_PRESENT_HI);
	tiler = rd(mali, TILER_PRESENT);
	l2_mask = rd(mali, L2_PRESENT);
	as_present = rd(mali, GPU_AS_PRESENT);
	js_present = rd(mali, GPU_JS_PRESENT);
	say("mali: 4: shader cores %#x (%u of them; above bit 31 %#x), tiler %#x, L2 %#x; address spaces %#x, job slots %#x\n",
	    shader_mask, __builtin_popcount(shader_mask), shaders_hi, tiler, l2_mask, as_present, js_present);
	say("mali: 4: L2_FEATURES %#x, CORE_FEATURES %#x, TILER_FEATURES %#x, MEM_FEATURES %#x, THREAD_MAX_THREADS %u, "
	    "THREAD_TLS_ALLOC %u, COHERENCY_FEATURES %#x\n", rd(mali, GPU_L2_FEATURES), rd(mali, GPU_CORE_FEATURES),
	    rd(mali, GPU_TILER_FEATURES), rd(mali, GPU_MEM_FEATURES), rd(mali, THREAD_MAX_THREADS), rd(mali, THREAD_TLS_ALLOC),
	    rd(mali, COHERENCY_FEATURES));
	if (shader_mask == 0 || shaders_hi != 0) {
		say("mali: 4: shader cores %#x:%08x: the jobs are given 32 bits of affinity; not going on\n", shaders_hi, shader_mask);
		return false;
	}
	if (!(as_present & 1) || (js_present & 3) != 3 || tiler == 0 || l2_mask == 0) {
		say("mali: 4: no address space 0, job slots 0 and 1, tiler or L2: not going on\n");
		return false;
	}
	gpu_answered = true;
	return true;
}

/* 5: a reset, no coherency, and panfrost's setting after a reset. */
static bool
step_reset(void)
{
	say("mali: 5: JM_CONFIG %#x, SHADER_CONFIG %#x, TILER_CONFIG %#x, L2_MMU_CONFIG %#x before the reset\n",
	    rd(mali, GPU_JM_CONFIG), rd(mali, GPU_SHADER_CONFIG), rd(mali, GPU_TILER_CONFIG), rd(mali, GPU_L2_MMU_CONFIG));
	say("mali: 5: writing GPU_INT_MASK = 0, GPU_CMD = 1 (soft reset; 2, hard, if it does not finish), then "
	    "COHERENCY_ENABLE = %u (none) and L2_MMU_CONFIG &= ~%#x\n", COHERENCY_NONE, L2_MMU_LIMIT_EXTERNAL);
	if (!gpu_reset()) {
		say("mali: 5: GPU_ID %#x, but it did not finish a reset (GPU_INT_RAWSTAT %#x)\n", gpu_id, rd(mali, GPU_INT_RAWSTAT));
		return false;
	}
	say("mali: 5: reset; COHERENCY_ENABLE reads %#x, L2_MMU_CONFIG %#x\n", rd(mali, COHERENCY_ENABLE),
	    rd(mali, GPU_L2_MMU_CONFIG));
	return true;
}

/* 6: the cores. */
static bool
step_cores(void)
{
	uint32_t tiler = rd(mali, TILER_PRESENT);
	const char *which = NULL;

	say("mali: 6: writing L2_PWRON %#x, then SHADER_PWRON %#x, then TILER_PWRON %#x, each waited for\n", l2_mask,
	    shader_mask, tiler);
	if (!gpu_cores_on(&which)) {
		say("mali: 6: %s did not power up: L2 ready %#x, shaders %#x, tiler %#x\n", which, rd(mali, L2_READY),
		    rd(mali, SHADER_READY), rd(mali, TILER_READY));
		return false;
	}
	say("mali: 6: GPU_ID %#x (Mali-G72 r%up%u), %u shader cores %#x, tiler %#x, L2 %#x: all powered\n", gpu_id,
	    (gpu_id >> 12) & 0xf, (gpu_id >> 4) & 0xff, __builtin_popcount(shader_mask), shader_mask, tiler, l2_mask);
	return true;
}

/*
 * Steps 7 and 8 run the same job twice. The first run is in identity mode, which needs no tables, so it shows whether
 * the job manager works at all. The second maps the page in the GPU's tables, which shows whether they work. Address
 * space 0 stays on the tables afterwards, since drawing needs them. The page's entry is removed when the steps end.
 */
#define FIRST_JOB_GPU       0x00100000ull

static vm_offset_t job_page;

static bool
step_job_identity(void)
{
	if (kmem_alloc(kernel_map, &job_page, PAGE_SIZE, KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		job_page = 0;
		say("mali: 7: no page for the first jobs\n");
		return false;
	}
	say("mali: 7: address space 0 in identity mode (AS_TRANSCFG 2); one job on slot 1 at %#llx\n", kvtophys(job_page));
	as0_update(0, 2);                       /* identity: a GPU address is the physical one */
	return run_write_job(job_page, kvtophys(job_page), "first job, identity mode");
}

static bool
step_job_tables(void)
{
	if (!map_page(FIRST_JOB_GPU, kvtophys(job_page))) {
		say("mali: 8: no memory for page tables\n");
		return false;
	}
	say("mali: 8: the page at GPU address %#llx in the GPU's tables (AS_TRANSTAB %#llx, %s levels, AS_TRANSCFG %#x), "
	    "address space 0 on them; the same job again\n", FIRST_JOB_GPU, pt_transtab, pt_four_levels() ? "four" : "three",
	    pt_transcfg());
	as0_update(pt_transtab, pt_transcfg());
	return run_write_job(job_page, FIRST_JOB_GPU, "second job, through the GPU's page tables");
}

/* 9: there for programs. */
static bool
step_serve(void)
{
	say("mali: 9: HakiosMali started: programs can draw with the GPU (Mesa's panfrost; drivers/gpu/mali/hakios.mali.cpp)\n");
	reached = 9;                            /* (the user client asks hakios_mali_ready()) */
	HakiosMaliStart();
	return true;
}

static const struct {
	const char *name;
	bool (*make)(void);
} steps[] = {
	{ "what the boot left, read only", step_look },
	{ "the supply, the clock, power", step_power },
	{ "after a power-on: the DTZPC restore, the bus dividers", step_dtzpc },
	{ "GPU_ID and what the GPU has", step_gpu },
	{ "a reset", step_reset },
	{ "the cores", step_cores },
	{ "a job in identity mode", step_job_identity },
	{ "a job through page tables", step_job_tables },
	{ "HakiosMali", step_serve },
};
#define STEPS ((uint32_t)(sizeof(steps) / sizeof(steps[0])))

/*
 * The panfrost interface that Mesa's panfrost needs in order to draw. HakiosMali (IOKit) answers it, and this is the
 * hardware half: parameters, GPU addresses, pages entered into and taken out of the tables, and jobs. Every client
 * shares address space 0, and one job runs at a time. Overlapping jobs and per-client address spaces are not done.
 */
static LCK_GRP_DECLARE(mali_lock_group, "hakios-mali");
static LCK_MTX_DECLARE(mali_lock, &mali_lock_group);
/*
 * Held while a job runs, and by anything that must not change the address space under one (an UPDATE when entries are
 * taken out). Taken before mali_lock, never after. mali_lock is not held for the length of a job, because making a
 * buffer used to wait for the running job to finish (2.5 ms a buffer).
 */
static LCK_MTX_DECLARE(mali_job_lock, &mali_lock_group);

/*
 * Waits for a job without keeping a core busy: a short poll, then sleeps of mali_nap_us at a time. The wait used to
 * spin (delay(10) in a loop) for as long as the GPU worked, which took a whole core of the four during each frame.
 */
static int mali_wait_event;
static uint32_t mali_nap_us = 50;       /* boot-arg mali_nap_us (10 to 1000) */
static LCK_MTX_DECLARE(mali_wait_lock, &mali_lock_group);

static bool
wait_job(uint32_t at, uint32_t mask, uint32_t want, uint32_t ms)
{
	uint64_t deadline;

	for (int i = 0; i < 20; i++) {
		if ((rd(mali, at) & mask) == want) {
			return true;
		}
		delay(10);
	}
	clock_interval_to_deadline(ms, NSEC_PER_MSEC, &deadline);
	while (mach_absolute_time() < deadline) {
		if ((rd(mali, at) & mask) == want) {
			return true;
		}
		uint64_t nap;

		clock_interval_to_deadline(mali_nap_us, NSEC_PER_USEC, &nap);
		lck_mtx_lock(&mali_wait_lock);
		(void)lck_mtx_sleep_deadline(&mali_wait_lock, LCK_SLEEP_DEFAULT, (event_t)&mali_wait_event, THREAD_UNINT, nap);
		lck_mtx_unlock(&mali_wait_lock);
	}
	return (rd(mali, at) & mask) == want;
}

#define MMU_INT_RAWSTAT     0x2000
#define MMU_INT_CLEAR       0x2004
#define AS_LOCKADDR_LO      0x10
#define AS_LOCKADDR_HI      0x14
#define AS_COMMAND_LOCK     2
#define AS_COMMAND_FLUSH_PT 4
#define JS0                 0x1800      /* fragment jobs; everything else goes to slot 1 */
/* GPU addresses for programs' buffers: 32 MB to 4 GB, given out and taken back (hakios.mali_va.h). */
static struct mali_va mali_va;          /* with mali_lock held */

/*
 * The last buffers given GPU addresses, and whether each has been taken out of the tables since. A translation fault is
 * a job writing into a freed buffer or past the end of one, and this says which. A freed address can be given out
 * again, so the newest buffer at an address is the one reported. Protected by mali_lock.
 */
#define MALI_RECENT 4096
static struct {
	uint64_t address, size;
	uint32_t freed, kind;                   /* kind: 0 not said, 1 a panfrost BO, 2 an IOSurface */
	uint32_t width, height, row_bytes, format, offset;
} mali_recent[MALI_RECENT];
static uint32_t mali_recent_next;

/* What a buffer just made is, for describe_address. */
void
hakios_mali_tag(uint64_t address, uint32_t kind, uint32_t width, uint32_t height, uint32_t row_bytes,
    uint32_t format, uint32_t offset)
{
	lck_mtx_lock(&mali_lock);
	for (int i = 0; i < MALI_RECENT; i++) {
		if (mali_recent[i].address == address && mali_recent[i].size != 0 && !mali_recent[i].freed) {
			mali_recent[i].kind = kind;
			mali_recent[i].width = width;
			mali_recent[i].height = height;
			mali_recent[i].row_bytes = row_bytes;
			mali_recent[i].format = format;
			mali_recent[i].offset = offset;
		}
	}
	lck_mtx_unlock(&mali_lock);
}

static void
say_kind(int i)
{
	if (mali_recent[i].kind == 2) {
		printf("mali:     an IOSurface %ux%u, %u bytes a row, format %#x, from byte %u\n", mali_recent[i].width,
		    mali_recent[i].height, mali_recent[i].row_bytes, mali_recent[i].format, mali_recent[i].offset);
	} else {
		printf("mali:     %s\n", mali_recent[i].kind == 1 ? "a panfrost BO" : "made by nothing that said");
	}
}

static void
describe_address(uint64_t address)
{
	int below = -1;

	/* newest first: an address given out again belongs to its last buffer */
	for (uint32_t n = 0; n < MALI_RECENT; n++) {
		int i = (int)((mali_recent_next - 1 - n) % MALI_RECENT);

		if (mali_recent[i].size == 0) {
			continue;
		}
		if (address >= mali_recent[i].address && address < mali_recent[i].address + mali_recent[i].size) {
			printf("mali:   the fault is %#llx into a buffer of %#llx bytes at %#llx, %s\n",
			    address - mali_recent[i].address, mali_recent[i].size, mali_recent[i].address,
			    mali_recent[i].freed ? "FREED already" : "still there (a page not entered?)");
			say_kind(i);
			return;
		}
		if (mali_recent[i].address <= address && (below < 0 || mali_recent[i].address > mali_recent[below].address ||
		    (mali_recent[i].address == mali_recent[below].address && !mali_recent[i].freed))) {
			below = i;
		}
	}
	if (below >= 0) {
		printf("mali:   the fault is %#llx past the end of a buffer of %#llx bytes at %#llx (%s)\n",
		    address - (mali_recent[below].address + mali_recent[below].size), mali_recent[below].size,
		    mali_recent[below].address, mali_recent[below].freed ? "freed" : "live");
		say_kind(below);
	} else {
		printf("mali:   the fault is in no buffer of the last %u\n", MALI_RECENT);
	}
}

/* The last job that did not finish. The periodic report in mali_thread prints it. */
static struct {
	uint32_t count, slot, raw, status, mmu, fault, fault_address;
	uint64_t chain, transtab, walk[3];      /* the tables the GPU had, and their entries for the fault's address */
} last_failure;

bool
hakios_mali_map(uint64_t gpu, uint64_t phys)
{
	bool done;

	lck_mtx_lock(&mali_lock);
	done = map_page(gpu, phys);
	lck_mtx_unlock(&mali_lock);
	return done;
}

bool
hakios_mali_ready(void)
{
	return reached >= STEPS;
}

/* Linux's DRM_PANFROST_PARAM_*, in its order, from the registers they come from. */
bool
hakios_mali_parameter(uint32_t which, uint64_t *value)
{
	static const uint16_t simple[] = {
		[5] = 0xe00,    /* STACK_PRESENT (none on Bifrost: reads 0) */
		[6] = 0x018, [7] = 0x01c, [8] = 0x004, [9] = 0x008, [10] = 0x00c, [11] = 0x010, [12] = 0x014,
		[13] = 0x0ac, [14] = 0x0a0, [15] = 0x0a4, [16] = 0x0a8, [17] = 0x300,
		[18] = 0x0b0, [19] = 0x0b4, [20] = 0x0b8, [21] = 0x0bc,
	};

	if (!hakios_mali_ready()) {
		return false;
	}
	switch (which) {
	case 0:
		*value = gpu_id >> 16;
		return true;
	case 1:
		*value = gpu_id & 0xffff;
		return true;
	case 2:
	case 3:
	case 4:
		*value = rd(mali, 0x100 + (which - 2) * 0x10) | ((uint64_t)rd(mali, 0x104 + (which - 2) * 0x10) << 32);
		return true;
	case 38:                                /* NR_CORE_GROUPS: as Linux panfrost counts them, one per L2 */
		*value = __builtin_popcount(l2_mask) != 0 ? (uint64_t)__builtin_popcount(l2_mask) : 1;
		return true;
	case 39:
		/*
		 * THREAD_TLS_ALLOC (0x310) reads 0 on the G52. Panfrost then sized a spilling shader's stack by
		 * THREAD_MAX_THREADS, but the G72 gives each core more stacks than that and wrote past the end: every write
		 * fault of the uber shaders, up to 0x15870 past a 0x60000 buffer. Never less than the THREAD_MAX_THREADS
		 * fallback, whatever the register says.
		 */
		*value = rd(mali, THREAD_TLS_ALLOC);
		if (*value < rd(mali, THREAD_MAX_THREADS)) {
			*value = rd(mali, THREAD_MAX_THREADS);
		}
		{
			static int said;
			if (!said++) {
				printf("mali: THREAD_TLS_ALLOC %#x, THREAD_MAX_THREADS %#x, SHADER_PRESENT %#x\n", rd(mali, THREAD_TLS_ALLOC),
				    rd(mali, THREAD_MAX_THREADS), rd(mali, SHADER_PRESENT));
			}
		}
		return true;
	case 40:
		/*
		 * AFBC_FEATURES: 0. Mesa reads 0 as AFBC SUPPORTED, not as none (Mesa 25.0.7 pan_props.c,
		 * panfrost_query_afbc(): architecture 5 or later and the register 0), as Linux panfrost answers
		 * on a GPU without the register. So panfrost renders into AFBC textures here (a G72 has AFBC;
		 * seen under drm-shim as the G72, tools/mali-drmshim-check.sh: the triangle's render target is
		 * written AFBC). PAN_MESA_DEBUG=noafbc in a program's environment turns it off, for telling
		 * an AFBC fault from any other on the phone.
		 */
		*value = 0;
		return true;
	default:
		if (which >= 22 && which <= 37) {
			*value = rd(mali, 0x0c0 + (which - 22) * 4);
			return true;
		}
		if (which < sizeof(simple) / sizeof(simple[0]) && simple[which] != 0) {
			*value = rd(mali, simple[which]);
			return true;
		}
		return false;
	}
}

/*
 * `size` bytes of GPU addresses between 32 MB and 4 GB (hakios.mali_va.h). `executable`: a buffer the GPU may run code
 * from (Linux's flags without PANFROST_BO_NOEXEC). Such a buffer never crosses a 16 MB line, which is panfrost's rule
 * and Mesa assumes it (a Bifrost blend shader's address is 32 bits). 0: none left, or a size of nothing.
 */
uint64_t
hakios_mali_address(uint64_t size, bool executable)
{
	uint64_t address;

	lck_mtx_lock(&mali_lock);
	address = mali_va_alloc(&mali_va, size, executable);
	if (address != 0) {
		mali_recent[mali_recent_next % MALI_RECENT].address = address;
		mali_recent[mali_recent_next % MALI_RECENT].size = size;
		mali_recent[mali_recent_next % MALI_RECENT].freed = 0;
		mali_recent[mali_recent_next % MALI_RECENT].kind = 0;
		mali_recent_next++;
	}
	lck_mtx_unlock(&mali_lock);
	return address;
}

void
hakios_mali_unmap(uint64_t gpu, uint64_t size)
{
	lck_mtx_lock(&mali_lock);
	for (int i = 0; i < MALI_RECENT; i++) {
		if (mali_recent[i].address == gpu && mali_recent[i].size != 0) {
			mali_recent[i].freed = 1;
		}
	}
	unmap_pages(gpu, size);
	lck_mtx_unlock(&mali_lock);
	lck_mtx_lock(&mali_job_lock);                   /* an entry taken out: the TLB may hold it */
	lck_mtx_lock(&mali_lock);
	as0_update(pt_transtab, pt_transcfg());
	/* only now, with nothing of the buffer left in the tables or the MMU, are its addresses free to be given again */
	(void)mali_va_free(&mali_va, gpu, size);
	lck_mtx_unlock(&mali_lock);
	lck_mtx_unlock(&mali_job_lock);
}

/*
 * After entries change, the GPU's MMU must forget what it cached of the tables. The address space is updated as a whole,
 * since an UPDATE drops everything the MMU cached for it. A region lock with a table flush was not enough: a fragment
 * job faulted on a page whose entry had been written and cleaned to memory. Jobs run one at a time to their end, so
 * the GPU is idle whenever this runs.
 */
void
hakios_mali_mapped(void)
{
	if (mali_fastmap) {
		return;         /* see TRANSCFG_WALK_NC */
	}
	lck_mtx_lock(&mali_job_lock);
	lck_mtx_lock(&mali_lock);
	as0_update(pt_transtab, pt_transcfg());
	lck_mtx_unlock(&mali_lock);
	lck_mtx_unlock(&mali_job_lock);
}

/* The entries of each level for GPU address `gpu`, as far as there are tables (with the lock held). */
static void
walk_tables(uint64_t gpu, uint64_t walk[3])
{
	uint64_t *table = pt_top;
	static const unsigned shift[3] = { 30, 21, 12 };

	walk[0] = walk[1] = walk[2] = 0;
	for (int level = 0; level < 3 && table != NULL; level++) {
		walk[level] = table[(gpu >> shift[level]) & (level == 0 ? 31 : 511)];
		table = level < 2 ? table_of(walk[level]) : NULL;
	}
}

/*
 * Runs one job chain, from GPU address `chain`, to its end before returning. Fragment jobs (`fragment`) go on slot 0,
 * the rest on slot 1. Returns 0 when it finished; otherwise the slot status, a fault, or 0xff for no answer within a
 * second. After a failure the GPU is reset, so the next job finds it working.
 */
uint32_t
hakios_mali_submit(uint64_t chain, bool fragment)
{
	uint32_t slot = fragment ? 0 : 1, js = fragment ? JS0 : JS1, raw, status = 0;
	bool finished;

	lck_mtx_lock(&mali_job_lock);
	lck_mtx_lock(&mali_lock);
	wr(mali, JOB_INT_CLEAR, (1u << slot) | (1u << (slot + 16)));
	wr(mali, js + JS_HEAD_NEXT_LO, (uint32_t)chain);
	wr(mali, js + JS_HEAD_NEXT_HI, (uint32_t)(chain >> 32));
	wr(mali, js + JS_AFFINITY_NEXT_LO, shader_mask);
	wr(mali, js + JS_AFFINITY_NEXT_HI, 0);
	wr(mali, js + JS_CONFIG_NEXT, JS_CONFIG);
	wr(mali, js + JS_COMMAND_NEXT, 1);
	lck_mtx_unlock(&mali_lock);
	finished = wait_job(JOB_INT_RAWSTAT, (1u << slot) | (1u << (slot + 16)), 1u << slot, 1000);
	lck_mtx_lock(&mali_lock);
	if (!finished) {
		raw = rd(mali, JOB_INT_RAWSTAT);
		status = (raw & (1u << (slot + 16))) ? rd(mali, js + JS_STATUS) : 0xff;
		last_failure.count++;
		last_failure.slot = slot;
		last_failure.raw = raw;
		last_failure.status = rd(mali, js + JS_STATUS);
		last_failure.mmu = rd(mali, MMU_INT_RAWSTAT);
		last_failure.fault = rd(mali, AS0 + AS_FAULTSTATUS);
		last_failure.fault_address = rd(mali, AS0 + 0x20);
		last_failure.chain = chain;
		last_failure.transtab = rd(mali, AS0 + AS_TRANSTAB_LO) | ((uint64_t)rd(mali, AS0 + AS_TRANSTAB_HI) << 32);
		walk_tables(last_failure.fault_address | ((uint64_t)rd(mali, AS0 + 0x24) << 32), last_failure.walk);
		printf("mali: a job on slot %u did not finish: interrupts %#x, status %#x, MMU %#x fault %#x at %#x\n", slot, raw,
		    last_failure.status, last_failure.mmu, last_failure.fault, last_failure.fault_address);
		describe_address(last_failure.fault_address | ((uint64_t)rd(mali, AS0 + 0x24) << 32));
		const char *which = NULL;

		(void)gpu_reset();                      /* a reset, then everything back as it was */
		if (!gpu_cores_on(&which)) {
			printf("mali: after the reset, %s did not power up again\n", which);
		}
		as0_update(pt_transtab, pt_transcfg());
		wr(mali, MMU_INT_CLEAR, 0xffffffffu);
	}
	wr(mali, JOB_INT_CLEAR, (1u << slot) | (1u << (slot + 16)));
	lck_mtx_unlock(&mali_lock);
	lck_mtx_unlock(&mali_job_lock);
	return status;
}

/*
 * Jobs in a queue, run by a thread of their own. A submission returns at once with the job's number, and callers wait
 * for that number (hakios_mali_wait). Jobs run one at a time, in the order given, which panfrost needs: a pass's vertex
 * and tiler chain comes before its fragment chain, and a pass comes before any pass that reads its output. Without the
 * queue each submission ran to its end before returning. A frame with blur passes then alternated between the CPU and
 * the GPU at every pass, which cost 11 to 17 ms of CPU time per frame.
 */
#define MALI_QUEUE      64
static struct {
	uint64_t chain, seq, queued;
	bool fragment;
} mali_queue[MALI_QUEUE];
/* boot-arg mali_trace=1: a line for every job, "tl: j SEQ f|v q QUEUED b BEGAN e ENDED" in us of mach_absolute_time */
static uint32_t mali_trace;
static unsigned mali_queue_head, mali_queue_count;
static uint64_t mali_queued_seq, mali_done_seq;
static bool mali_worker_started;
static LCK_MTX_DECLARE(mali_queue_lock, &mali_lock_group);

/*
 * Counters for the last second, read by hakios_mali_second() for the display's fps_log (MiniIOMobileFramebuffer.cpp):
 * time the GPU ran jobs, job count, time they waited in the queue. Protected by mali_queue_lock.
 */
static uint64_t sec_busy, sec_queued;
static uint32_t sec_jobs;

/* PLL_G3D's rate as its registers say, in MHz; 26 when the G3D is on the oscillator */
static uint32_t
pll_mhz(void)
{
	uint32_t con0 = rd(cmu_g3d, PLL_CON0), m = (con0 >> 16) & 0x3ff, p = (con0 >> 8) & 0x3f, s = con0 & 7;

	if (!(con0 & MUX_SEL)) {
		return (uint32_t)(PLL_FIN_KHZ / 1000);
	}
	return p != 0 ? (uint32_t)(PLL_FIN_KHZ * m / ((uint64_t)p << s) / 1000) : 0;
}

/*
 * mali_gov=1: the GPU's clock follows its load, as drivers/power/exynos/hakios.exynos_cpufreq.c does for the cores.
 * Each GOV_WINDOW_MS, the share of that window the GPU spent running jobs is measured. The clock goes to the lowest of
 * the three rates at which that load would be at most GOV_TARGET%. It goes up at once, and down only after GOV_DOWN
 * windows in a row. It never exceeds the G3D temperature cap. The switch is pll_switch(), run between jobs and under
 * mali_job_lock, because the G3D runs from the 26 MHz oscillator while the PLL switches. Three failed switches in a
 * row stop the governor, and the clock stays where it is.
 */
#define GOV_WINDOW_MS   16
#define GOV_TARGET      50              /* percent. Low, because a frame waits for its jobs */
#define GOV_DOWN        5
#define GOV_IDLE_MS     50              /* asleep with no jobs: a window ends anyway, the clock may come down */
#define GOV_CAP1_MC     85000           /* the G3D's sensor: 455 MHz at most from here */
#define GOV_CAP2_MC     95000           /* 260 MHz from here */
#define GOV_HYST_MC     5000
#define GOV_NOT_READ_MC (-273000)
#define GOV_LEVELS      g3d_nrates
extern int32_t hakios_exynos_tmu_zone_mc(unsigned zone);       /* 2: the G3D's (hakios.exynos_tmu.c; stubs.c) */
static uint32_t mali_gov;
/*
 * mali_gov_frame=<MHz> (also kern.hakios.mali_gov_frame): the clock floor while frames are coming. A frame is one given
 * to the display (hakios_mali_frame(), present() in MiniIOMobileFramebuffer.cpp) within the last GOV_FRAME_MS. The GPU
 * then runs at least at the lowest rate that is this fast, whatever the load says, and still under the temperature cap.
 * The render thread waits for a frame's jobs. A GPU at 260 MHz that is busy a third of the time would finish a frame
 * later than one at 572 MHz, though the load alone would keep it at 260. 0: no floor.
 */
#define GOV_FRAME_MS    100
static uint32_t gov_frame_mhz = 572;
static uint64_t gov_frame_last;
SYSCTL_UINT(_kern_hakios, OID_AUTO, mali_gov_frame, CTLFLAG_RW | CTLFLAG_LOCKED | CTLFLAG_KERN, &gov_frame_mhz, 0,
    "the GPU's clock while frames come, MHz at least (mali_gov=1; 0: none)");

void hakios_mali_frame(void);
void
hakios_mali_frame(void)
{
	__atomic_store_n(&gov_frame_last, mach_absolute_time(), __ATOMIC_RELAXED);
	hakios_exynos_bus_busy();       /* bus_max=2: the memory bus at its top while frames come (hakios.exynos_acpm.c) */
}
static bool gov_on;
static unsigned gov_level, gov_cap = G3D_RATES_MOST - 1, gov_down, gov_failed;
static bool gov_dvs;                    /* mali_dvs=1 and every level's voltage known: BUCK6 follows the level */

/* B6OUT for a level under mali_dvs: the APM's voltage + the margin, rounded up, between the floor and 950 mV */
static uint8_t
gov_vsel(unsigned level)
{
	uint32_t uv = g3d_rates[level].uv + mali_dvs_margin, vsel;

	uv = uv < mali_dvs_floor ? mali_dvs_floor : uv;
	vsel = uv <= 300000 ? 0 : (uv - 300000 + 6249) / 6250;
	return (uint8_t)(vsel > B6OUT_MOST ? B6OUT_MOST : vsel);
}
static uint64_t gov_since, gov_busy;
static uint32_t gov_switches, gov_ups, gov_downs, gov_capped;

/* which of the rates PLL_G3D runs at; GOV_LEVELS when none */
static unsigned
gov_level_now(void)
{
	for (unsigned i = 0; i < GOV_LEVELS; i++) {
		if (pll_is(&g3d_rates[i])) {
			return i;
		}
	}
	return GOV_LEVELS;
}

static void
gov_start(void)
{
	if (!mali_gov) {
		return;
	}
	if (!mali_pll || !mali_acpm || !acpm_ok || (gov_level = gov_level_now()) >= GOV_LEVELS) {
		say("mali: mali_gov=1: not started (mali_pll=%u, mali_acpm=%u, ACPM %s, PLL_G3D %s one of the three rates)\n",
		    mali_pll, mali_acpm, acpm_ok ? "answers" : "does not answer", gov_level < GOV_LEVELS ? "at" : "NOT at");
		return;
	}
	if (!PE_parse_boot_argn("mali_gov_frame", &gov_frame_mhz, sizeof(gov_frame_mhz)) && g3d_nrates > 3) {
		gov_frame_mhz = g3d_rates[g3d_nrates - 1].mhz;  /* mali_max: frames at the top */
	}
	gov_cap = GOV_LEVELS - 1;
	if (mali_dvs) {
		gov_dvs = true;
		for (unsigned i = 0; i < GOV_LEVELS; i++) {
			gov_dvs = gov_dvs && g3d_rates[i].uv != 0;
		}
		if (!gov_dvs) {
			say("mali: mali_dvs=1: not every level has the APM's voltage: BUCK6 stays at 950 mV\n");
		} else if (!rail_set(gov_vsel(gov_level))) {
			gov_dvs = false;
			(void)rail_set(B6OUT_950MV);
			say("mali: mali_dvs=1: BUCK6 not set for %u MHz: it stays at 950 mV, no DVS\n", g3d_rates[gov_level].mhz);
		} else {
			for (unsigned i = 0; i < GOV_LEVELS; i++) {
				say("mali: mali_dvs=1: %u MHz at B6OUT %#x = %u mV (the APM's %u uV + %u, at least %u)\n",
				    g3d_rates[i].mhz, gov_vsel(i), 300 + gov_vsel(i) * 25 / 4, g3d_rates[i].uv, mali_dvs_margin,
				    mali_dvs_floor);
			}
		}
	}
	gov_on = true;
	gov_since = mach_absolute_time();
	say("mali: mali_gov=1: the GPU's clock follows its load from %u MHz: %u%% of a %u ms window wanted at most; no more "
	    "than %u MHz from %d C on the G3D's sensor, %u MHz from %d C\n", g3d_rates[gov_level].mhz, GOV_TARGET, GOV_WINDOW_MS,
	    g3d_rates[1].mhz, GOV_CAP1_MC / 1000, g3d_rates[0].mhz, GOV_CAP2_MC / 1000);
}

static void
gov_set(unsigned level)
{
	bool switched;

	if (level == gov_level || level >= GOV_LEVELS) {
		return;
	}
	/* mali_dvs: the voltage up before the clock goes up (a failed raise: no switch) */
	if (gov_dvs && level > gov_level && gov_vsel(level) > gov_vsel(gov_level) && !rail_set(gov_vsel(level))) {
		say("mali: mali_dvs: BUCK6 not raised for %u MHz: no switch\n", g3d_rates[level].mhz);
		return;
	}
	lck_mtx_lock(&mali_job_lock);
	pll_quiet = true;
	switched = pll_switch(&g3d_rates[level]);
	pll_quiet = false;
	lck_mtx_unlock(&mali_job_lock);
	/* ... and down only after the clock has come down */
	if (switched && gov_dvs && level < gov_level && gov_vsel(level) < gov_vsel(gov_level)) {
		(void)rail_set(gov_vsel(level));
	}
	if (switched) {
		gov_ups += level > gov_level;
		gov_downs += level < gov_level;
		gov_level = level;
		gov_switches++;
		gov_failed = 0;
		return;
	}
	if ((gov_level = gov_level_now()) >= GOV_LEVELS || ++gov_failed >= 3) {
		gov_on = false;
		if (gov_dvs) {
			gov_dvs = false;
			(void)rail_set(B6OUT_950MV);    /* wherever the clock stopped, the voltage for any of them */
		}
		say("mali: mali_gov: PLL_G3D did not switch to %u MHz (%u times in a row; it runs at %u MHz): the governor stops\n",
		    g3d_rates[level].mhz, gov_failed, pll_mhz());
	}
}

/* `busy` more of the GPU's time at `now` (mach_absolute_time); a window that has ended decides. No locks held. */
static void
gov_window(uint64_t now, uint64_t busy)
{
	uint64_t window, elapsed;
	unsigned want, cap;
	uint32_t load;
	int32_t mc;

	if (!gov_on) {
		return;
	}
	gov_busy += busy;
	nanoseconds_to_absolutetime((uint64_t)GOV_WINDOW_MS * NSEC_PER_MSEC, &window);
	elapsed = now - gov_since;
	if (elapsed < window) {
		return;
	}
	load = (uint32_t)(gov_busy >= elapsed ? 100 : gov_busy * 100 / elapsed);
	gov_since = now;
	gov_busy = 0;
	mc = hakios_exynos_tmu_zone_mc(2);
	cap = GOV_LEVELS - 1;
	if (mc != GOV_NOT_READ_MC) {
		if (mc >= GOV_CAP1_MC || (gov_cap <= 1 && mc >= GOV_CAP1_MC - GOV_HYST_MC)) {
			cap = 1;
		}
		if (mc >= GOV_CAP2_MC || (gov_cap == 0 && mc >= GOV_CAP2_MC - GOV_HYST_MC)) {
			cap = 0;
		}
	}
	if (cap != gov_cap) {
		say("mali: mali_gov: the G3D at %d.%d C: %u MHz at most now\n", mc / 1000, mc % 1000 / 100, g3d_rates[cap].mhz);
		gov_capped += cap < gov_cap;
		gov_cap = cap;
	}
	for (want = 0; want + 1 < GOV_LEVELS &&
	    (uint64_t)load * g3d_rates[gov_level].mhz > (uint64_t)GOV_TARGET * g3d_rates[want].mhz; want++) {
	}
	/* (mali_gov_frame: frames coming, the floor) */
	if (gov_frame_mhz != 0) {
		uint64_t last = __atomic_load_n(&gov_frame_last, __ATOMIC_RELAXED), span;

		nanoseconds_to_absolutetime((uint64_t)GOV_FRAME_MS * NSEC_PER_MSEC, &span);
		if (last != 0 && now - last < span) {
			unsigned floor = 0;

			while (floor + 1 < GOV_LEVELS && g3d_rates[floor].mhz < gov_frame_mhz) {
				floor++;
			}
			want = want < floor ? floor : want;
		}
	}
	want = want > cap ? cap : want;
	if (want > gov_level || gov_level > cap) {
		gov_down = 0;
		gov_set(want);
	} else if (want < gov_level) {
		if (++gov_down >= GOV_DOWN) {
			gov_down = 0;
			gov_set(want);
		}
	} else {
		gov_down = 0;
	}
}

/*
 * fps_log (MiniIOMobileFramebuffer.cpp), once a second: the GPU's time running jobs since the last call, the jobs, their
 * time in the queue before they ran (ns), the clock (MHz) and the governor's switches; the G3D's temperature
 * (millidegrees). 0 when the GPU is not there for programs.
 */
int hakios_mali_second(uint64_t *busy_ns, uint32_t *jobs, uint64_t *queued_ns, uint32_t *mhz, uint32_t *switches,
    int32_t *g3d_mc);

int
hakios_mali_second(uint64_t *busy_ns, uint32_t *jobs, uint64_t *queued_ns, uint32_t *mhz, uint32_t *switches,
    int32_t *g3d_mc)
{
	uint64_t busy, queued;

	if (reached < STEPS) {
		return 0;
	}
	lck_mtx_lock(&mali_queue_lock);
	busy = sec_busy;
	queued = sec_queued;
	*jobs = sec_jobs;
	sec_busy = sec_queued = 0;
	sec_jobs = 0;
	lck_mtx_unlock(&mali_queue_lock);
	absolutetime_to_nanoseconds(busy, busy_ns);
	absolutetime_to_nanoseconds(queued, queued_ns);
	*mhz = pll_mhz();
	*switches = gov_on ? gov_switches : 0;
	*g3d_mc = hakios_exynos_tmu_zone_mc(2);
	return gov_on ? 2 : 1;
}

static void
mali_worker(__unused void *parameter, __unused wait_result_t result)
{
	for (;;) {
		uint64_t chain, seq;
		bool fragment;

		lck_mtx_lock(&mali_queue_lock);
		while (mali_queue_count == 0) {
			uint64_t deadline;

			if (!gov_on || gov_level == 0) {
				lck_mtx_sleep(&mali_queue_lock, LCK_SLEEP_DEFAULT, (event_t)&mali_queue_count, THREAD_UNINT);
				continue;
			}
			clock_interval_to_deadline(GOV_IDLE_MS, NSEC_PER_MSEC, &deadline);
			if (lck_mtx_sleep_deadline(&mali_queue_lock, LCK_SLEEP_DEFAULT, (event_t)&mali_queue_count, THREAD_UNINT,
			    deadline) == THREAD_TIMED_OUT && mali_queue_count == 0) {
				lck_mtx_unlock(&mali_queue_lock);
				gov_window(mach_absolute_time(), 0);    /* (idle: the clock may come down) */
				lck_mtx_lock(&mali_queue_lock);
			}
		}
		chain = mali_queue[mali_queue_head].chain;
		seq = mali_queue[mali_queue_head].seq;
		uint64_t queued = mali_queue[mali_queue_head].queued, job_began = mach_absolute_time();
		fragment = mali_queue[mali_queue_head].fragment;
		lck_mtx_unlock(&mali_queue_lock);

		{
			/* how busy the GPU is: every five seconds, its share of the time and by which slot */
			static uint64_t busy[2], since, jobs[2];
			uint64_t began = mach_absolute_time(), now, ns;

			if (hakios_mali_submit(chain, fragment) != 0) {
				gMaliFailed++;
			}
			now = mach_absolute_time();
			if (mali_trace) {
				uint64_t q, b, e;

				absolutetime_to_nanoseconds(queued, &q);
				absolutetime_to_nanoseconds(began, &b);
				absolutetime_to_nanoseconds(now, &e);
				printf("tl: j %llu %c q %llu b %llu e %llu\n", seq, fragment ? 'f' : 'v', q / 1000, b / 1000, e / 1000);
			}
			absolutetime_to_nanoseconds(now - began, &ns);
			busy[fragment] += ns;
			jobs[fragment]++;
			if (since == 0) {
				since = now;
			} else {
				absolutetime_to_nanoseconds(now - since, &ns);
				if (ns >= 5ull * NSEC_PER_SEC) {
					printf("mali: GPU busy %llu%% of %llu s: vertex and tiler %llu jobs %llu ms (%llu us each), "
					    "fragment %llu jobs %llu ms (%llu us each)\n", (busy[0] + busy[1]) * 100 / ns,
					    ns / NSEC_PER_SEC, jobs[0], busy[0] / 1000000, jobs[0] ? busy[0] / 1000 / jobs[0] : 0,
					    jobs[1], busy[1] / 1000000, jobs[1] ? busy[1] / 1000 / jobs[1] : 0);
					busy[0] = busy[1] = jobs[0] = jobs[1] = 0;
					since = now;
				}
			}
		}

		uint64_t ended = mach_absolute_time();

		lck_mtx_lock(&mali_queue_lock);
		mali_queue_head = (mali_queue_head + 1) % MALI_QUEUE;
		mali_queue_count--;
		mali_done_seq = seq;
		sec_jobs++;
		sec_busy += ended - job_began;
		sec_queued += job_began - queued;
		lck_mtx_unlock(&mali_queue_lock);
		wakeup((caddr_t)&mali_done_seq);
		wakeup((caddr_t)&mali_queue_head);       /* (room in the queue) */
		gov_window(ended, ended - job_began);
	}
}

/* A job chain into the queue: its number. */
uint64_t
hakios_mali_enqueue(uint64_t chain, bool fragment)
{
	uint64_t seq;

	lck_mtx_lock(&mali_queue_lock);
	if (!mali_worker_started) {
		thread_t thread;

		if (kernel_thread_start(mali_worker, NULL, &thread) == KERN_SUCCESS) {
			thread_set_thread_name(thread, "the Mali's jobs");
			thread_deallocate(thread);
			mali_worker_started = true;
		}
	}
	while (mali_queue_count == MALI_QUEUE) {
		lck_mtx_sleep(&mali_queue_lock, LCK_SLEEP_DEFAULT, (event_t)&mali_queue_head, THREAD_UNINT);
	}
	seq = ++mali_queued_seq;
	unsigned at = (mali_queue_head + mali_queue_count) % MALI_QUEUE;

	mali_queue[at].chain = chain;
	mali_queue[at].fragment = fragment;
	mali_queue[at].seq = seq;
	mali_queue[at].queued = mach_absolute_time();
	mali_queue_count++;
	lck_mtx_unlock(&mali_queue_lock);
	wakeup((caddr_t)&mali_queue_count);
	return seq;
}

/* Wait until job `seq` is done: true when it is; with timeout 0 only a look. */
bool
hakios_mali_wait(uint64_t seq, uint64_t timeout_ns)
{
	uint64_t deadline;
	bool done;

	if (timeout_ns > 10ull * NSEC_PER_SEC) {
		timeout_ns = 10ull * NSEC_PER_SEC;     /* (an "infinite" one, INT64_MAX, would wrap) */
	}
	nanoseconds_to_absolutetime(timeout_ns, &deadline);
	deadline += mach_absolute_time();
	lck_mtx_lock(&mali_queue_lock);
	while (mali_done_seq < seq && timeout_ns != 0 && mach_absolute_time() < deadline) {
		lck_mtx_sleep_deadline(&mali_queue_lock, LCK_SLEEP_DEFAULT, (event_t)&mali_done_seq, THREAD_UNINT, deadline);
	}
	done = mali_done_seq >= seq;
	lck_mtx_unlock(&mali_queue_lock);
	return done;
}

/* Runs the steps, then reports once a minute what the GPU has done. */
static void
mali_thread(__unused void *parameter, __unused wait_result_t result)
{
	say("mali: the Galaxy S9's Mali-G72: steps 1 to %u of %u (mali=%u)\n", last_step, STEPS, last_step);
	for (step = 1; step <= last_step; step++) {
		say("mali: step %u: %s\n", step, steps[step - 1].name);
		if (!steps[step - 1].make()) {
			say("mali: step %u: FAILED; the steps after it are not made\n", step);
			break;
		}
		reached = step;
		say("mali: step %u: done\n", step);
	}
	if (job_page != 0) {
		/* After step 9, programs may already be using the GPU, so the tables change under the locks. */
		lck_mtx_lock(&mali_job_lock);
		lck_mtx_lock(&mali_lock);
		if (pt_top != NULL) {
			unmap_pages(FIRST_JOB_GPU, PAGE_SIZE);  /* removed before the page is freed, so the GPU has nothing to point at */
			as0_update(pt_transtab, pt_transcfg());
		}
		lck_mtx_unlock(&mali_lock);
		lck_mtx_unlock(&mali_job_lock);
		kmem_free(kernel_map, job_page, PAGE_SIZE);
		job_page = 0;
	}
	if (reached < STEPS) {
		if (powered_here) {
			g3d_off();
		} else if (reached >= 2) {
			say("mali: the G3D domain was on when found: left on%s\n", rail_changed ? ", and BUCK6 at 950 mV under it" : "");
		} else if (rail_changed && (rd(pmu, PMU_G3D_STATUS) & G3D_ON) == 0) {
			say("mali: BUCK6 back as it was found: B6OUT %#x\n", b6out_found);
			if (rail_set(b6out_found)) {
				rail_changed = false;
			}
		} else if (rail_changed) {
			/* The boot left the domain on and step 2 stopped at the clock. BUCK6 stays at 950 mV under the running GPU. */
			say("mali: the G3D domain is on: BUCK6 left at 950 mV under it\n");
		}
		say("mali: came up to step %u of %u: not there for programs\n", reached, STEPS);
		return;
	}
	say("mali: came up to step %u of %u: the GPU is there for programs (HakiosMali)\n", reached, STEPS);
	gov_start();
	for (;;) {
		delay(60 * 1000 * 1000);
		printf("mali: step %u of %u; user space says %u%s; %u clients, %u buffers, %u job chains (%u failed), "
		    "%u pictures shown; %llu MB of the GPU's 4 GB of addresses in use\n", reached, STEPS, user_said,
		    user_said == 1 ? " -- a program ran a job on the GPU" : "", gMaliClients, gMaliBuffers, gMaliSubmits,
		    gMaliFailed, gMaliShown, mali_va.taken * MALI_VA_GRANULE >> 20);
		lck_mtx_lock(&kept_lock);
		for (uint32_t i = kept_next > KEPT_LINES ? kept_next - KEPT_LINES : 0; i < kept_next; i++) {
			printf("kept: %s\n", kept_lines[i % KEPT_LINES]);
		}
		lck_mtx_unlock(&kept_lock);
		if (gl_said != 0) {
			printf("mali: gl_test drew on %s; %u%% of the picture inside the triangle (26%% by geometry); "
			    "the middle pixel red %u green %u blue %u alpha %u\n", gl_said / 1000 == 1 ? "THE MALI" : "the processor",
			    gl_said % 1000, gl_middle & 0xff, (gl_middle >> 8) & 0xff, (gl_middle >> 16) & 0xff, gl_middle >> 24);
		}
		if (last_failure.count != 0) {
			printf("mali: the last of %u unfinished: slot %u, chain at %#llx, interrupts %#x, slot status %#x, "
			    "MMU interrupts %#x, fault status %#x at address %#x\n", last_failure.count, last_failure.slot,
			    last_failure.chain, last_failure.raw, last_failure.status, last_failure.mmu, last_failure.fault,
			    last_failure.fault_address);
			printf("mali: the GPU's tables at %#llx (ours at %#llx); for the fault's address: %#llx, %#llx, %#llx\n",
			    last_failure.transtab, pt_transtab, last_failure.walk[0], last_failure.walk[1], last_failure.walk[2]);
			for (uint32_t i = 0; i < gMaliBuffers && i < 32; i++) {
				printf("mali:   buffer %u at %#llx..%#llx (asked %#x, flags %#x)%s\n", i, gMaliMade[i].address,
				    gMaliMade[i].address + gMaliMade[i].size, gMaliMade[i].asked, gMaliMade[i].flags,
				    last_failure.fault_address >= gMaliMade[i].address &&
				    last_failure.fault_address < gMaliMade[i].address + gMaliMade[i].size ? "  <- the fault" : "");
			}
		}
	}
}

/* Called from hakios_input_init() (drivers/input/hakios.input.c) at boot. The steps run in a thread. */
void
hakios_mali_init(void)
{
	thread_t thread;

	(void)PE_parse_boot_argn("mali_fastmap", &mali_fastmap, sizeof(mali_fastmap));
	(void)PE_parse_boot_argn("mali_trace", &mali_trace, sizeof(mali_trace));
	(void)PE_parse_boot_argn("mali_pll", &mali_pll, sizeof(mali_pll));
	(void)PE_parse_boot_argn("mali_mhz", &mali_mhz, sizeof(mali_mhz));
	(void)PE_parse_boot_argn("mali_acpm", &mali_acpm, sizeof(mali_acpm));
	(void)PE_parse_boot_argn("mali_gov", &mali_gov, sizeof(mali_gov));
	(void)PE_parse_boot_argn("mali_max", &mali_max, sizeof(mali_max));
	(void)PE_parse_boot_argn("mali_dvs", &mali_dvs, sizeof(mali_dvs));
	(void)PE_parse_boot_argn("mali_dvs_margin", &mali_dvs_margin, sizeof(mali_dvs_margin));
	(void)PE_parse_boot_argn("mali_dvs_floor", &mali_dvs_floor, sizeof(mali_dvs_floor));
	(void)PE_parse_boot_argn("mali_nap_us", &mali_nap_us, sizeof(mali_nap_us));
	mali_nap_us = mali_nap_us < 10 ? 10 : mali_nap_us > 1000 ? 1000 : mali_nap_us;
	if (!PE_parse_boot_argn("mali", &last_step, sizeof(last_step)) || last_step == 0) {
		return;
	}
	if (pe_arm_get_soc_base_phys() != STARLTE_SOC_BASE) {
		say("mali: mali= is for the Galaxy S9 (the loader's platform starlte, registers from %#lx), and this is not it "
		    "(%#lx): nothing touched\n", STARLTE_SOC_BASE, (unsigned long)pe_arm_get_soc_base_phys());
		return;
	}
	if (last_step > STEPS) {
		last_step = STEPS;
	}
	if (kernel_thread_start(mali_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
