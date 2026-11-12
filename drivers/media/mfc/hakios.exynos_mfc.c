#define HAKIOS_SOC_FIXED
/*
 * Hardware H.264 and HEVC decoding on the Exynos 9810 video codec block (MFC v10), for the iOS side. VideoToolbox keeps its
 * software decoder when this driver is absent or has failed. Nothing else depends on this driver.
 *
 * Depends on: the PMU power domain, the CMU_TOP, CMU_BUSC and CMU_MFC clocks, the two SysMMUs, SMC for the DTZPC protection,
 * ACPM for the optional rate requests, the firmware file (step 3), 128 MB the boot loader keeps out of XNU's memory, and
 * SPI 356 when mfc_irq=1.
 *
 * Boot args. mfc=N runs steps 1 through N; without it nothing is touched.
 *   mfc=1  look: the domain's and the clocks' state, read only
 *   mfc=2  block: power domain, clocks and Q-channels on; the IP version and SysMMU state read
 *   mfc=3  firmware: mfc_fw.bin from the VENDOR partition (or mfc_fw_part=) into the block's memory; RISC started; SYS_INIT
 *   mfc=4  self-test: a stream decoded at boot, each frame's CRC checked against ffmpeg's
 *   mfc=5  device: /dev/hakios-mfc for user space (sys/hakios.mfc.h), one client at a time
 *   mfc_zc=1        with 5: frames decoded straight into the client's IOSurfaces (property HakiosMFCFrame, set by mediaserverd's
 *                   MiniIOSurface.cpp). Surfaces are carved from the top of the 128 MB, the instance's own buffers from the bottom.
 *   mfc_irq=1       with 4 or more: wait for the block's interrupt instead of polling
 *   mfc_int_khz=N, mfc_mif_khz=N   ask ACPM for the INT and MIF rates, in kHz
 *   mfc_fw_part=    the partition holding mfc_fw.bin (default VENDOR)
 *
 * Any failed step leaves the block alone for the rest of the boot: /dev/hakios-mfc is not created, or open returns EIO.
 *
 * Hardware facts come from Samsung's GPL BSP for the S9 (Scion-Project/android_kernel_samsung_exynos9810, drivers/media/platform/
 * exynos/mfc: s5p_mfc_regs_v10.h, s5p_mfc_cal.c, s5p_mfc_cmd.c, s5p_mfc_inst.c, s5p_mfc_reg.c, s5p_mfc_irq.c; drivers/soc/samsung/
 * cal-if/exynos9810: flexpmu_cal_local, cmucal-sfr, cmucal-node; device tree nodes mfc0@178D0000, pd-mfc@14064068, sysmmu@17880000
 * and @178A0000). Those sources were read for these facts; no code was copied. Licensed under the APSL, as the kernel's are.
 *
 * Copyright (c) 2026 the HakiOS contributors. APSL 2.0.
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/conf.h>
#include <sys/errno.h>
#include <sys/ioccom.h>
#include <sys/proc.h>
#include <sys/kauth.h>
#include <miscfs/devfs/devfs.h>
#include <kern/clock.h>
#include <kern/locks.h>
#include <kern/thread.h>
#include <kern/sched_prim.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#include <IOKit/IOLib.h>
#include <libkern/OSAtomic.h>
#include <libkern/crc.h>
#include <stdarg.h>
#include <string.h>
#include <mach/vm_param.h>
#include <sys/hakios.exynos_soc.h>
#include <sys/hakios.exynos9810_mem.h>
#include <sys/hakios.exynos_acpm.h>
#include <sys/hakios.input.h>
#include <sys/hakios.mfc.h>
#include <sys/hakios.physmap.h>
#include <sys/hakios.log.h>

/* a file of the small image (drivers/storage/hakios.image.c), as hakios.bcm4361_bt.c and hakios.ssp.c take theirs */
extern const uint8_t *hakios_image_file(const char *dir, const char *name, uint64_t *size, int *disk_out);

extern vm_offset_t ml_io_map(vm_offset_t, vm_size_t);
extern boolean_t pmap_valid_page(ppnum_t);
extern void hakios_exynos_bus_busy(void);  /* drivers/soc/exynos/hakios.exynos_acpm.c: a stamp only */


/* say() output also goes to the log the boot loader keeps across a return to it; TWRP shows a run's log from there */
#define MFC_PA                  EXYNOS_MFC_PA
#define MFC_SIZE                0x10000UL
#define PMU_PA                  EXYNOS_PMU_PA
#define PMU_SIZE                0x5000UL
#define PMU_CONFIG              EXYNOS_MFC_PMU_CONFIG
#define PMU_STATUS              EXYNOS_MFC_PMU_STATUS
#define PD_ON                   0xFu
#define CMU_TOP_PA              EXYNOS_CMU_TOP_PA
#define CMU_BUSC_PA             EXYNOS_CMU_BUSC_PA
#define CMU_MFC_PA              EXYNOS_CMU_MFC_PA
#define CMU_SIZE                0x8000UL
#define TOP_MUX                 EXYNOS_MFC_TOP_MUX
#define TOP_DIV                 EXYNOS_MFC_TOP_DIV
#define TOP_GATE                EXYNOS_MFC_TOP_GATE
#define TOP_BUSY                (1u << 16)
#define GATE_MANUAL             (1u << 20)
#define GATE_ON                 (1u << 21)
#define MFC_BUS_USER            0x0100
#define MFC_QCH_FIRST           EXYNOS_MFC_MFC_QCH_FIRST
#define MFC_QCH_LAST            EXYNOS_MFC_MFC_QCH_LAST
#define QCH_REQ                 (1u << 1)
#define SYSMMU0_PA              EXYNOS_MFC_SYSMMU0_PA
#define SYSMMU1_PA              EXYNOS_MFC_SYSMMU1_PA
#define SYSMMU_SIZE             0x9000UL
#define MMU_CTRL                0x0000
#define MMU_VERSION             0x0014
#define DTZPC_SMC               0x82000410u
#define DTZPC_RESTORE           1
#define DTZPC_MFC               EXYNOS_MFC_DTZPC_MFC

/*
 * Register offsets from MFC_PA, names and values as in s5p_mfc_regs_v10.h. The 0xF000 range is the firmware's
 * command and context area; the commands and their answers are described before the H2R_ and R2H_ values below.
 */
#define RISC_ON                 0x0000
#define RISC2HOST_INT           0x003C
#define HOST2RISC_INT           0x0044
#define RISC_BASE_ADDRESS       0x0054
#define MFC_RESET               0x1070
#define HOST2RISC_CMD           0x1100
#define RISC2HOST_CMD           0x1104
#define MFC_OFF                 0x7120
#define FW_VERSION              0xF000
#define INSTANCE_ID             0xF008
#define CODEC_TYPE              0xF00C
#define CONTEXT_MEM_ADDR        0xF014
#define CONTEXT_MEM_SIZE        0xF018
#define PIXEL_FORMAT            0xF020
#define MFC_VERSION             0xF028
#define DEC_TIMEOUT_VALUE       0xF03C
#define RET_INSTANCE_ID         0xF070
#define ERROR_CODE              0xF074
#define D_CRC_CTRL              0xF0B0
#define D_DEC_OPTIONS           0xF0B4
#define D_SEI_ENABLE            0xF0C4
#define D_MIN_NUM_DPB           0xF0F0
#define D_MIN_FIRST_PLANE       0xF0F4
#define D_MIN_SECOND_PLANE      0xF0F8
#define D_MIN_NUM_MV            0xF100
#define D_MIN_SCRATCH           0xF108
#define D_NUM_DPB               0xF130
#define D_NUM_MV                0xF134
#define D_FIRST_STRIDE          0xF138
#define D_SECOND_STRIDE         0xF13C
#define D_FIRST_SIZE            0xF144
#define D_SECOND_SIZE           0xF148
#define D_MV_SIZE               0xF150
#define D_INIT_OPTIONS          0xF154
#define D_FIRST_DPB0            0xF160
#define D_SECOND_DPB0           0xF260
#define D_MV0                   0xF460
#define D_SCRATCH_ADDR          0xF560
#define D_SCRATCH_SIZE          0xF564
#define D_NAL_OPTIONS           0xF5AC
#define D_CPB_ADDR              0xF5B0
#define D_CPB_SIZE              0xF5B4
#define D_AVAILABLE_UPPER       0xF5B8
#define D_AVAILABLE_LOWER       0xF5BC
#define D_CPB_OFFSET            0xF5C0
#define D_SLICE_IF              0xF5C4
#define D_PICTURE_TAG           0xF5C8
#define D_STREAM_SIZE           0xF5D0
#define D_DYNAMIC_UPPER         0xF5D4
#define D_DYNAMIC_LOWER         0xF5D8
#define D_DISPLAY_WIDTH         0xF600
#define D_DISPLAY_HEIGHT        0xF604
#define D_DISPLAY_STATUS        0xF608
#define D_DISPLAY_FIRST         0xF60C
#define D_DISPLAY_SECOND        0xF610
#define D_DISPLAY_CROP1         0xF61C
#define D_DISPLAY_CROP2         0xF620
#define D_DECODED_STATUS        0xF644
#define D_DECODED_FIRST         0xF648
#define D_DECODED_NAL_SIZE      0xF664
#define D_RET_TAG_TOP           0xF674
#define D_USED_LOWER            0xF724

/*
 * Command protocol. The host writes a command to HOST2RISC_CMD and sets HOST2RISC_INT to 1. The RISC sets RISC2HOST_INT when it
 * is done, leaves its answer in RISC2HOST_CMD and any error in ERROR_CODE.
 *
 *   start       after RISC_ON, FW_STATUS_RET; then CONTEXT_MEM_ADDR and _SIZE, then SYS_INIT.
 *   instance    CODEC_TYPE, CONTEXT_MEM_ADDR and _SIZE (2 MB for H.264 and HEVC), OPEN_INSTANCE. RET_INSTANCE_ID is its number.
 *   header      D_DEC_OPTIONS, PIXEL_FORMAT 0 (NV12), D_SEI_ENABLE, the stream (D_STREAM_SIZE, D_CPB_ADDR, D_CPB_SIZE,
 *               D_CPB_OFFSET), SEQ_HEADER. SEQ_DONE_RET gives the size, D_MIN_NUM_DPB, D_MIN_NUM_MV, D_MIN_SCRATCH, the
 *               D_MIN_*_PLANE sizes, and D_DECODED_NAL_SIZE: how much of the stream was used.
 *   buffers     D_NUM_DPB, plane sizes and strides, scratch, D_MV_SIZE and D_NUM_MV, one MV buffer each. D_INIT_OPTIONS bit 3
 *               gives frame buffers one per frame ("dynamic DPB"). INIT_BUFFERS, then INIT_BUFFERS_RET.
 *   frame       the stream. A free slot's planes are at D_FIRST_DPB0 + 4 x slot and D_SECOND_DPB0 + 4 x slot, its bit in
 *               D_DYNAMIC_LOWER; the slot and those still referenced are in D_AVAILABLE_LOWER; the picture tag in
 *               D_PICTURE_TAG; NAL_START. FRAME_DONE_RET: D_DISPLAY_STATUS bits 2:0 (1 decoded and shown, 2 shown only),
 *               D_DISPLAY_FIRST and _SECOND hold the planes shown, the tag comes back at D_RET_TAG_TOP, and D_USED_LOWER has
 *               the slots still referenced. LAST_FRAME drains the decoder.
 */
#define H2R_SYS_INIT            1
#define H2R_OPEN_INSTANCE       2
#define H2R_SEQ_HEADER          3
#define H2R_INIT_BUFFERS        4
#define H2R_NAL_START           5
#define H2R_CLOSE_INSTANCE      6
#define H2R_LAST_FRAME          9
#define R2H_MASK                0x1FFFFu
#define R2H_SYS_INIT            1
#define R2H_OPEN_INSTANCE       2
#define R2H_SEQ_DONE            3
#define R2H_INIT_BUFFERS        4
#define R2H_CLOSE_INSTANCE      6
#define R2H_COMPLETE_SEQ        9
#define R2H_FW_STATUS           12
#define R2H_FRAME_DONE          13
#define R2H_FIELD_DONE          14
#define R2H_ERR                 32
#define CODEC_H264              0
#define CODEC_HEVC              17
#define CODEC_VP9               18

#define FW_CODE                 (1u << 20)
#define DEV_CTX                 0x8000u
#define INST_CTX                (2u << 20)
#define OTHER_CTX               0x8000u
#define CPB_SIZE                (6u << 20)
#define MAX_SLOTS               32
#define ROUND_UP(x, a)          (((x) + (uint32_t)(a) - 1) & ~((uint32_t)(a) - 1))

/*
 * Hardware facts (see the source list at the top of the file).
 *
 *   registers   0x178D0000, 64 KB. SPI 356 is the interrupt (MFC_INTID). Without mfc_irq the driver polls RISC2HOST_INT for up
 *               to 2 s: 100 reads 200 us apart, then 1 ms sleeps.
 *   domain      pd-mfc. PMU MFC_CONFIGURATION (0x14064068) bits 3:0 = 0xF turn it on; MFC_STATUS is 0x1406406C. After power-up,
 *               the DTZPC protection is restored with SMC 0x82000410 (1, 0x17810204, 0), as the BSP does for G3D, ISPPRE and AUD.
 *   clocks      CMU_TOP at 0x1A240000: the CLKCMU_MFC_BUS mux at 0x109C bits 2:0, divider at 0x1898 bits 3:0, gate at 0x20A4
 *               (bit 20 manual, bit 21 on). CMU_MFC at 0x17800000: PLL_CON0_MUX_CLKCMU_MFC_BUS_USER at +0x100 (bit 4 selects
 *               CMU_TOP's clock rather than the oscillator's; bit 7 is busy), Q-channels at +0x300C to +0x3058 (bit 1 requests
 *               the clock), their state at +0x4000 more. The bus side is in CMU_BUSC at 0x1A200000: +0x30EC, +0x30F8, +0x314C.
 *   rate        not a DVFS domain of its own. The boot's rate is logged; mfc_int_khz and mfc_mif_khz ask ACPM for the INT and
 *               MIF rates.
 *   SysMMUs     0x17880000 and 0x178A0000. MMU_CTRL +0 (bit 0: translating), version at +0x14. After a power-up they are taken
 *               as off (pass-through) and read to confirm. The addresses below are physical and 32 bits wide.
 *   firmware    a 1 MB image at RISC_BASE_ADDRESS, the device context (32 KB) right after it. The BSP's normal path copies the
 *               image and starts the RISC with no SMC and no signature check. Only its DRM path has those; this driver does not
 *               use that path.
 *   memory      128 MB of the low bank, which XNU does not get when mfc= is on the command line (the loader's extra_ram_claim,
 *               sys/hakios.exynos9810_mem.h). Mapped write-combined, so no cache maintenance. Layout: see mfc_alloc and carve.
 */

/* Writes are followed by a full data synchronization barrier. PA() gives the physical address of a pointer into mem. */
static volatile uint8_t *regs, *pmu, *cmu_top, *cmu_busc, *cmu_mfc, *mmu0, *mmu1;
static uint8_t *mem;                    /* the 128 MB, write-combined */
static uint32_t steps;
static int dead;                        /* a step failed: the block is not used again this boot */
static int fw_up;
static lck_mtx_t *mfc_lock;

static inline uint32_t rd(volatile uint8_t *b, uint32_t off) { return *(volatile uint32_t *)(b + off); }
static inline void wr(volatile uint8_t *b, uint32_t off, uint32_t v) { *(volatile uint32_t *)(b + off) = v; __builtin_arm_dsb(15); }
#define R(off)          rd(regs, (off))
#define W(off, v)       wr(regs, (off), (v))
#define PA(p)           ((uint32_t)(EXYNOS9810_MEM_MFC_PA + (uint64_t)((uint8_t *)(p) - mem)))

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

static bool
wait_bits(volatile uint8_t *b, uint32_t off, uint32_t mask, uint32_t want, uint32_t us)
{
	for (uint32_t i = 0; i < us; i += 10) {
		if ((rd(b, off) & mask) == want) {
			return true;
		}
		IODelay(10);
	}
	return (rd(b, off) & mask) == want;
}

/* Memory: a bump allocator over the 128 MB. An instance's allocations are released together when it closes. */

static uint32_t mem_used, mem_base_used;

/*
 * Zero-copy frames are carved from the top of the 128 MB, for client surfaces. carved[] is kept sorted by address, highest
 * first. A surface's lifetime belongs to the client's pool, not to this driver.
 */
#define CARVE_MAX 64
static struct { uint32_t at, bytes; } carved[CARVE_MAX];
static uint32_t n_carved;
static lck_mtx_t *carve_lock;
static uint32_t zero_copy;

static uint32_t
carve_floor(void)
{
	return n_carved != 0 ? carved[n_carved - 1].at : (uint32_t)EXYNOS9810_MEM_SIZE(MFC);
}

int
hakios_mfc_carve(uint64_t bytes, uint64_t *phys)
{
	uint32_t top = (uint32_t)EXYNOS9810_MEM_SIZE(MFC), need, i;

	if (mem == NULL || carve_lock == NULL || !zero_copy || bytes == 0 || bytes > EXYNOS9810_MEM_SIZE(MFC)) {
		return -1;
	}
	need = ROUND_UP((uint32_t)bytes, 4096);
	lck_mtx_lock(carve_lock);
	for (i = 0; i < n_carved; i++) {
		if (top - (carved[i].at + carved[i].bytes) >= need) {
			break;
		}
		top = carved[i].at;
	}
	if (n_carved == CARVE_MAX || top < need || (i == n_carved && top - need < ROUND_UP(mem_used, 4096))) {
		lck_mtx_unlock(carve_lock);
		return -1;
	}
	memmove(&carved[i + 1], &carved[i], (n_carved - i) * sizeof(carved[0]));
	carved[i].at = top - need;
	carved[i].bytes = need;
	n_carved++;
	*phys = EXYNOS9810_MEM_MFC_PA + carved[i].at;
	lck_mtx_unlock(carve_lock);
	return 0;
}

int
hakios_mfc_carved(uint64_t phys)
{
	return zero_copy && phys >= EXYNOS9810_MEM_MFC_PA && phys < EXYNOS9810_MEM_MFC_END;
}

void
hakios_mfc_uncarve(uint64_t phys)
{
	if (carve_lock == NULL) {
		return;
	}
	lck_mtx_lock(carve_lock);
	for (uint32_t i = 0; i < n_carved; i++) {
		if (EXYNOS9810_MEM_MFC_PA + carved[i].at == phys) {
			memmove(&carved[i], &carved[i + 1], (n_carved - i - 1) * sizeof(carved[0]));
			n_carved--;
			break;
		}
	}
	lck_mtx_unlock(carve_lock);
}

extern int hakios_surface_mfc_frame(uint32_t sid, uint64_t *y, uint64_t *uv, uint32_t *y_stride, uint32_t *uv_stride, uint32_t *y_bytes, uint32_t *uv_bytes);

static void *
mfc_alloc(uint32_t bytes)
{
	uint32_t at = ROUND_UP(mem_used, 4096);

	if (mem == NULL) {
		return NULL;
	}
	if (carve_lock != NULL) {
		lck_mtx_lock(carve_lock);
	}
	bool room = (uint64_t)at + bytes <= carve_floor();

	if (carve_lock != NULL) {
		lck_mtx_unlock(carve_lock);
	}
	if (!room) {
		return NULL;
	}
	mem_used = at + bytes;
	return mem + at;
}

/* Firmware commands. */

static uint32_t last_error;             /* ERROR_CODE as read with the last answer */

/*
 * mfc_irq=1: the block's interrupt instead of polling. The handler takes the answer and clears the block's interrupt. The waiter
 * still reads RISC2HOST_INT every 10 ms, so an interrupt that never arrives is counted in irq_missed, not treated as a dead session.
 */
#define MFC_INTID       EXYNOS_MFC_MFC_INTID      /* the device tree's SPI 356 */
extern kern_return_t gic_v3_spi_attach(uint32_t intid, bool edge, void (*handler)(void *context, uint32_t intid), void *context);
static uint32_t irq_wanted;
static bool irq_on;
static volatile uint32_t irq_answer, irq_error;
static uint64_t irq_count, irq_missed;

static void
mfc_interrupt(void *context __unused, uint32_t intid __unused)
{
	if (R(RISC2HOST_INT) != 0) {
		irq_error = R(ERROR_CODE);
		irq_answer = R(RISC2HOST_CMD) & R2H_MASK;
		W(RISC2HOST_CMD, 0);
		W(RISC2HOST_INT, 0);
		irq_count++;
		thread_wakeup((event_t)&irq_answer);
	}
}

/* What the RISC answers, or 0 after `ms`. */
static uint32_t
wait_answer(uint32_t ms)
{
	if (irq_on) {
		uint64_t deadline;

		clock_interval_to_deadline(ms, NSEC_PER_MSEC, &deadline);
		for (;;) {
			uint32_t answer;

			assert_wait_timeout((event_t)&irq_answer, THREAD_UNINT, 10, NSEC_PER_MSEC);
			if (irq_answer != 0) {
				clear_wait(current_thread(), THREAD_AWAKENED);
			} else {
				thread_block(THREAD_CONTINUE_NULL);
			}
			if (irq_answer != 0) {
				answer = irq_answer;
				last_error = irq_error;
				irq_answer = 0;
				return answer;
			}
			if (R(RISC2HOST_INT) != 0) {
				irq_missed++;
				answer = R(RISC2HOST_CMD) & R2H_MASK;
				last_error = R(ERROR_CODE);
				W(RISC2HOST_CMD, 0);
				W(RISC2HOST_INT, 0);
				/* The handler may have run between the check above and these writes; its answer is the same answer. */
				irq_answer = 0;
				return answer;
			}
			if (mach_absolute_time() > deadline) {
				return 0;
			}
		}
	}
	for (uint32_t i = 0; i < ms * 2; i++) {
		if (R(RISC2HOST_INT) != 0) {
			uint32_t answer = R(RISC2HOST_CMD) & R2H_MASK;

			last_error = R(ERROR_CODE);
			W(RISC2HOST_CMD, 0);
			W(RISC2HOST_INT, 0);
			return answer;
		}
		if (i < 100) {
			IODelay(200);
		} else {
			IOSleep(1);
		}
	}
	return 0;
}

static void
command(uint32_t cmd)
{
	W(RISC2HOST_CMD, 0);
	W(HOST2RISC_CMD, cmd);
	W(HOST2RISC_INT, 1);
}

static int
expect(uint32_t cmd, uint32_t want, const char *what)
{
	uint32_t answer;

	command(cmd);
	answer = wait_answer(2000);
	if (answer != want) {
		say("exynos_mfc: %s: answer %u, wanted %u, error %#x%s\n", what, answer, want, last_error,
		    answer == 0 ? " (nothing in 2 s: the block is left alone from now on)" : "");
		if (answer == 0) {
			dead = 1;
		}
		return EIO;
	}
	return 0;
}

/* An instance. */

static struct {
	int open;
	uint32_t codec, inst, width, height, crop_x, crop_y, crop_w, crop_h, stride, plane0, plane1, slots, mv_count, mv_size, scratch;
	uint8_t *cpb, *slot[MAX_SLOTS];
	bool zc;                        /* frames are in the client's surfaces */
	uint32_t rows;                  /* zero copy: the rows the client's surfaces must have */
	uint32_t slot_y[MAX_SLOTS], slot_uv[MAX_SLOTS]; /* each slot's latest plane addresses */
	uint32_t used;                  /* slots the firmware still references */
	uint32_t pending;               /* slots of frames decoded but not yet shown; D_USED_LOWER does not report these */
	bool buffers;
	uint32_t pending_offset;
	uint32_t mem_mark;
} I;

static void
give_stream(uint32_t bytes, uint32_t offset)
{
	W(D_STREAM_SIZE, bytes);
	W(D_CPB_ADDR, PA(I.cpb));
	W(D_CPB_SIZE, CPB_SIZE);
	W(D_CPB_OFFSET, offset);
}

static int
inst_open(uint32_t codec)
{
	uint32_t size = (codec == CODEC_H264 || codec == CODEC_HEVC) ? INST_CTX : OTHER_CTX;
	uint8_t *ctx;
	int error;

	I.mem_mark = mem_used;
	ctx = mfc_alloc(size);
	I.cpb = mfc_alloc(CPB_SIZE);
	if (ctx == NULL || I.cpb == NULL) {
		mem_used = I.mem_mark;
		return ENOMEM;
	}
	bzero(ctx, size);
	W(CODEC_TYPE, codec);
	W(CONTEXT_MEM_ADDR, PA(ctx));
	W(CONTEXT_MEM_SIZE, size);
	W(D_CRC_CTRL, 0);
	if ((error = expect(H2R_OPEN_INSTANCE, R2H_OPEN_INSTANCE, "OPEN_INSTANCE")) != 0) {
		mem_used = I.mem_mark;
		return error;
	}
	I.inst = R(RET_INSTANCE_ID);
	I.codec = codec;
	I.open = 1;
	I.buffers = false;
	I.used = 0;
	I.pending = 0;
	return 0;
}

static void
inst_close(void)
{
	if (!I.open) {
		return;
	}
	W(INSTANCE_ID, I.inst);
	(void)expect(H2R_CLOSE_INSTANCE, R2H_CLOSE_INSTANCE, "CLOSE_INSTANCE");
	I.open = 0;
	mem_used = I.mem_mark;
}

/* Decode the header at I.cpb: the stream size and the buffers wanted. How much of the stream it took is returned in *consumed. */
static int
inst_header(uint32_t bytes, uint32_t *consumed)
{
	uint32_t options;
	int error;

	/* D_DEC_OPTIONS: concealment mode 4 at bit 8, realloc at bit 13, parse everything including the PPS at bit 15 */
	options = (0x4u << 8) | (1u << 13) | (1u << 15);
	W(D_DEC_OPTIONS, options);
	W(PIXEL_FORMAT, 0);
	W(D_SEI_ENABLE, 1u << 2);
	give_stream(bytes, 0);
	W(INSTANCE_ID, I.inst);
	if ((error = expect(H2R_SEQ_HEADER, R2H_SEQ_DONE, "SEQ_HEADER")) != 0) {
		return error;
	}
	I.width = R(D_DISPLAY_WIDTH);
	I.height = R(D_DISPLAY_HEIGHT);
	*consumed = R(D_DECODED_NAL_SIZE);
	if (I.width == 0 || I.height == 0 || I.width > 4096 || I.height > 4096) {
		say("exynos_mfc: the header gives %ux%u, error %#x\n", I.width, I.height, last_error);
		return EINVAL;
	}
	/* Crop: CROP1 holds right << 16 | left, CROP2 holds bottom << 16 | top. */
	uint32_t c1 = R(D_DISPLAY_CROP1), c2 = R(D_DISPLAY_CROP2);
	I.crop_x = c1 & 0xFFFF;
	I.crop_w = I.width - (c1 & 0xFFFF) - (c1 >> 16);
	I.crop_y = c2 & 0xFFFF;
	I.crop_h = I.height - (c2 & 0xFFFF) - (c2 >> 16);
	I.stride = ROUND_UP(I.width, 16);
	uint32_t rows = ROUND_UP(I.height, 16), min0 = R(D_MIN_FIRST_PLANE), min1 = R(D_MIN_SECOND_PLANE);
	I.plane0 = I.stride * ((I.codec == CODEC_H264) ? ROUND_UP(rows, 32) : rows);
	I.plane1 = I.stride * (rows / 2 + 16);
	I.plane0 = ROUND_UP(MAX(I.plane0, min0), 256);
	I.plane1 = ROUND_UP(MAX(I.plane1, min1), 256);
	I.mv_count = R(D_MIN_NUM_MV);
	I.scratch = ROUND_UP(R(D_MIN_SCRATCH), 256);
	I.slots = R(D_MIN_NUM_DPB) + 3;
	if (I.slots > MAX_SLOTS) {
		I.slots = MAX_SLOTS;
	}
	if (I.codec == CODEC_HEVC) {
		I.mv_size = (((I.width + 63) / 64) * ((I.height + 63) / 64)) * 256 + 512;
	} else {
		I.mv_size = ((I.width + 15) / 16) * ((((I.height + 15) / 16 + 1) / 2) * 2) * 64 + 1024;
	}
	I.mv_size = ROUND_UP(I.mv_size, 16);
	I.rows = MAX(I.plane0 / I.stride + (I.plane0 % I.stride != 0), 2 * (I.plane1 / I.stride + (I.plane1 % I.stride != 0)));
	say("exynos_mfc: the stream: %ux%u (crop %u,%u %ux%u), %u DPBs wanted (taking %u), %u MVs of %u, scratch %u, planes %u + %u, took %u of %u bytes, "
	    "error %#x\n", I.width, I.height, I.crop_x, I.crop_y, I.crop_w, I.crop_h, R(D_MIN_NUM_DPB), I.slots, I.mv_count, I.mv_size, I.scratch,
	    I.plane0, I.plane1, *consumed, bytes, last_error);
	return 0;
}

static int
inst_buffers(void)
{
	uint32_t codec_bytes = I.scratch + I.mv_count * I.mv_size;
	uint8_t *codec = mfc_alloc(codec_bytes);
	uint32_t at;
	int error;

	if (codec == NULL) {
		return ENOMEM;
	}
	if (I.zc) {
		/* Zero copy: the frames are the client's surfaces, so only check that enough of them fit. */
		uint32_t frame = ROUND_UP(I.stride * I.rows + I.stride * (I.rows / 2), 4096);

		lck_mtx_lock(carve_lock);
		uint32_t room = carve_floor() - ROUND_UP(mem_used, 4096);
		lck_mtx_unlock(carve_lock);
		if (room / frame < I.slots + 2) {
			/* Not enough room for the client's frame pool in the 128 MB; decode into the instance's own frames and copy out. */
			say("exynos_mfc: no room for %u frames of %u bytes in %u: copies\n", I.slots + 2, frame, room);
			I.zc = false;
		}
	}
	for (uint32_t i = 0; !I.zc && i < I.slots; i++) {
		I.slot[i] = mfc_alloc(I.plane0 + I.plane1);
		if (I.slot[i] == NULL) {
			if (i <= R(D_MIN_NUM_DPB)) {
				say("exynos_mfc: no memory for %u frames of %u bytes\n", I.slots, I.plane0 + I.plane1);
				return ENOMEM;
			}
			I.slots = i;
			break;
		}
	}
	W(D_NUM_DPB, I.slots);
	W(D_FIRST_SIZE, I.plane0);
	W(D_SECOND_SIZE, I.plane1);
	W(D_SCRATCH_ADDR, PA(codec));
	W(D_SCRATCH_SIZE, I.scratch);
	W(D_MV_SIZE, I.mv_size);
	W(D_INIT_OPTIONS, 1u << 3);     /* bit 3: dynamic DPB, a frame buffer per frame */
	W(D_FIRST_STRIDE, I.stride);
	W(D_SECOND_STRIDE, I.stride);
	W(D_NUM_MV, I.mv_count);
	at = PA(codec) + I.scratch;
	for (uint32_t i = 0; i < I.mv_count; i++, at += I.mv_size) {
		W(D_MV0 + 4 * i, at);
	}
	W(PIXEL_FORMAT, 0);
	W(INSTANCE_ID, I.inst);
	if ((error = expect(H2R_INIT_BUFFERS, R2H_INIT_BUFFERS, "INIT_BUFFERS")) != 0) {
		return error;
	}
	I.buffers = true;
	I.used = 0;
	I.pending = 0;
	return 0;
}

/*
 * Decode one access unit at I.cpb + offset, or with bytes == 0 and last set, drain the decoder. The result goes in *r:
 * whether a frame is to be shown, its planes and its picture tag.
 */
struct mfc_result {
	bool shown, finished;
	int slot, shown_slot;
	uint8_t *y, *uv;
	uint32_t tag, consumed, status;
};

static int
inst_frame(uint32_t bytes, uint32_t offset, uint32_t tag, bool last, uint32_t y_pa, uint32_t uv_pa, struct mfc_result *r)
{
	uint32_t slot, answer;

	bzero(r, sizeof(*r));
	r->slot = r->shown_slot = -1;
	for (slot = 0; slot < I.slots && ((I.used | I.pending) & (1u << slot)); slot++) {
	}
	if (slot == I.slots) {
		/* Frames the firmware may never show (after an error) are given up here. If no slot is free even then, the decoder is out of buffers. */
		say("exynos_mfc: every frame buffer is in use (referenced %#x, to be shown %#x): those to be shown are given up\n", I.used, I.pending);
		I.pending = 0;
		for (slot = 0; slot < I.slots && (I.used & (1u << slot)); slot++) {
		}
		if (slot == I.slots) {
			return ENOSPC;
		}
	}
	W(D_FIRST_SIZE, I.plane0);
	W(D_SECOND_SIZE, I.plane1);
	if (!I.zc) {
		y_pa = PA(I.slot[slot]);
		uv_pa = PA(I.slot[slot] + I.plane0);
	}
	I.slot_y[slot] = y_pa;
	I.slot_uv[slot] = uv_pa;
	r->slot = (int)slot;
	W(D_FIRST_DPB0 + 4 * slot, y_pa);
	W(D_SECOND_DPB0 + 4 * slot, uv_pa);
	W(D_DYNAMIC_LOWER, 1u << slot);
	W(D_DYNAMIC_UPPER, 0);
	W(D_AVAILABLE_LOWER, I.used | (1u << slot));
	W(D_AVAILABLE_UPPER, 0);
	W(D_SLICE_IF, 0);
	W(D_NAL_OPTIONS, 0);
	W(DEC_TIMEOUT_VALUE, 200000000);
	W(D_PICTURE_TAG, tag);
	give_stream(bytes, offset);
	W(INSTANCE_ID, I.inst);
	command(last ? H2R_LAST_FRAME : H2R_NAL_START);
	answer = wait_answer(2000);
	if (answer == 0) {
		say("exynos_mfc: a frame: no answer in 2 s: the block is left alone from now on\n");
		dead = 1;
		return EIO;
	}
	if (answer == R2H_COMPLETE_SEQ) {
		r->finished = true;
		return 0;
	}
	if (answer != R2H_FRAME_DONE && answer != R2H_FIELD_DONE) {
		say("exynos_mfc: a frame: answer %u, error %#x\n", answer, last_error);
		return (answer == R2H_ERR) ? EBADMSG : EIO;
	}
	I.used = R(D_USED_LOWER);
	r->status = R(D_DISPLAY_STATUS);
	/*
	 * A decoded frame that is not yet shown can drop out of D_USED_LOWER. The Samsung BSP never releases such a slot before it
	 * is shown, so this driver keeps it in I.pending as well. Not verified to matter: no test run failed without this.
	 */
	{
		uint32_t decoded = R(D_DECODED_STATUS) & 7, at = R(D_DECODED_FIRST);

		if (decoded == 0 || decoded == 1) {
			for (uint32_t i = 0; i < I.slots; i++) {
				if (I.slot_y[i] == at) {
					I.pending |= 1u << i;
					break;
				}
			}
		}
	}
	if ((r->status >> 4) & 3) {
		/* The stream changed size in-band (D_DISPLAY_STATUS bits 5:4). The buffers are the old size's, so the client must open again. */
		say("exynos_mfc: the stream changes size in-band (status %#x): for the client to open again\n", r->status);
		return ESTALE;
	}
	r->consumed = R(D_DECODED_NAL_SIZE);
	r->tag = R(D_RET_TAG_TOP);
	if ((r->status & 7) == 1 || (r->status & 7) == 2) {
		uint32_t y = R(D_DISPLAY_FIRST), uv = R(D_DISPLAY_SECOND);

		if (y < EXYNOS9810_MEM_MFC_PA || y >= EXYNOS9810_MEM_MFC_END || uv < EXYNOS9810_MEM_MFC_PA || uv >= EXYNOS9810_MEM_MFC_END) {
			say("exynos_mfc: a frame shown at %#x and %#x: not ours\n", y, uv);
			return EIO;
		}
		r->y = mem + (y - EXYNOS9810_MEM_MFC_PA);
		r->uv = mem + (uv - EXYNOS9810_MEM_MFC_PA);
		r->shown = true;
		for (uint32_t i = 0; i < I.slots; i++) {
			if (I.slot_y[i] == y) {
				I.pending &= ~(1u << i);
				r->shown_slot = (int)i;
				break;
			}
		}
	}
	return 0;
}

/* Steps. */

static void
look(const char *when)
{
	say("exynos_mfc: %s: pd-mfc config %#x, status %#x (0xf: on); CMU_TOP CLKCMU_MFC_BUS mux %#x, divider %#x, gate %#x; "
	    "CMU_BUSC Q-channels D0 %#x, D1 %#x, P %#x\n", when, rd(pmu, PMU_CONFIG), rd(pmu, PMU_STATUS), rd(cmu_top, TOP_MUX),
	    rd(cmu_top, TOP_DIV), rd(cmu_top, TOP_GATE), rd(cmu_busc, 0x30EC), rd(cmu_busc, 0x30F8), rd(cmu_busc, 0x314C));
}

/*
 * The memory bus's clock as the CMU has it, read only (12 Nov: MIF and INT were never set by this kernel and the APM's
 * DVFS "get" answers 0, so the rate S-Boot left was unknown; Samsung's star2 tree boots MIF at 1794000 kHz for 40 s,
 * devfreq_mif boot_info, then keeps it between 208000 and 2093000). Apollo cal-if exynos9810: CMU_MIF 0x1B800000,
 * PLL_CON0_PLL_MIF +0x120 (PLL_1050X: P [13:8], M [25:16], S [2:0], ENABLE 31, STABLE 29; rates 4264/3731/2576/1418 MHz,
 * the bus at about half), CLKMUX_MIF_DDRPHY2X +0x1000 (the PLL, or CMU_TOP's CLKCMU_MIF_SWITCH for the lower levels),
 * DIV_CLK_MIF_PRE +0x1810, DIV_CLK_MIF_BUSD +0x1800; CMU_TOP's MUX_CLKCMU_MIF_SWITCH +0x10a8.
 */
#define CMU_MIF_PA      0x1B800000ULL
static void
mif_look(const char *when)
{
	static volatile uint8_t *cmu_mif;
	uint32_t con0, m, p, s;

	if (cmu_mif == NULL) {
		cmu_mif = (volatile uint8_t *)ml_io_map(CMU_MIF_PA, 0x2000);
		if (cmu_mif == NULL) {
			return;
		}
	}
	con0 = rd(cmu_mif, 0x120);
	m = (con0 >> 16) & 0x3ff;
	p = (con0 >> 8) & 0x3f;
	s = con0 & 7;
	say("exynos_mfc: %s: the memory bus: PLL_MIF CON0 %#x (M %u, P %u, S %u: %u MHz, %s, %s), DDRPHY2X mux %#x, MIF_PRE "
	    "divider %#x, BUSD divider %#x; CMU_TOP MIF_SWITCH mux %#x\n", when, con0, m, p, s,
	    p ? (uint32_t)(26ULL * m / ((uint64_t)p << s)) : 0, con0 & (1u << 31) ? "enabled" : "off",
	    con0 & (1u << 29) ? "stable" : "not stable", rd(cmu_mif, 0x1000), rd(cmu_mif, 0x1810), rd(cmu_mif, 0x1800),
	    rd(cmu_top, 0x10a8));
}

static bool
block_on(void)
{
	uint64_t answer;
	uint32_t khz = 0, want;

	if ((rd(pmu, PMU_STATUS) & PD_ON) != PD_ON) {
		wr(pmu, PMU_CONFIG, rd(pmu, PMU_CONFIG) | PD_ON);
		if (!wait_bits(pmu, PMU_STATUS, PD_ON, PD_ON, 20000)) {
			say("exynos_mfc: 2: the domain did not come on: status %#x\n", rd(pmu, PMU_STATUS));
			return false;
		}
		answer = smc(DTZPC_SMC, DTZPC_RESTORE, DTZPC_MFC, 0);
		say("exynos_mfc: 2: the domain on; SMC %#x (1, %#x), the DTZPC restore, returned %#llx\n", DTZPC_SMC, DTZPC_MFC, answer);
		if ((uint32_t)answer != 0) {
			say("exynos_mfc: 2: the protection was not restored: nothing of the block is touched\n");
			return false;
		}
	} else {
		say("exynos_mfc: 2: the domain was on already\n");
	}
	wr(cmu_top, TOP_GATE, rd(cmu_top, TOP_GATE) | GATE_MANUAL | GATE_ON);
	cmu_mfc = (volatile uint8_t *)ml_io_map(CMU_MFC_PA, CMU_SIZE);
	/* Select CMU_TOP's clock (bit 4) for the bus mux, then wait until it is no longer busy (bit 7). */
	wr(cmu_mfc, MFC_BUS_USER, rd(cmu_mfc, MFC_BUS_USER) | (1u << 4));
	if (!wait_bits(cmu_mfc, MFC_BUS_USER, 1u << 7, 0, 1000)) {
		say("exynos_mfc: 2: the bus's mux stays busy (%#x)\n", rd(cmu_mfc, MFC_BUS_USER));
		return false;
	}
	for (uint32_t q = MFC_QCH_FIRST; q <= MFC_QCH_LAST; q += 4) {
		wr(cmu_mfc, q, rd(cmu_mfc, q) | QCH_REQ);
	}
	IODelay(100);
	mif_look("2 (as found)");
	/* Rates are left alone unless a boot arg asks; CLKCMU_MFC_BUS is in the INT domain. */
	if (PE_parse_boot_argn("mfc_int_khz", &want, sizeof(want)) && want != 0) {
		int e = hakios_acpm_dvfs_set(1, want);
		say("exynos_mfc: 2: INT asked for %u kHz: %d\n", want, e);
	}
	if (PE_parse_boot_argn("mfc_mif_khz", &want, sizeof(want)) && want != 0) {
		int e = hakios_acpm_dvfs_set(0, want);
		say("exynos_mfc: 2: MIF asked for %u kHz: %d\n", want, e);
	}
	mif_look("2 (after any request)");
	(void)hakios_acpm_dvfs_get(1, &khz);
	say("exynos_mfc: 2: INT at %u kHz; ", khz);
	khz = 0;
	(void)hakios_acpm_dvfs_get(0, &khz);
	say("MIF at %u kHz\n", khz);
	look("2");
	say("exynos_mfc: 2: CMU_MFC: BUS_USER %#x, PLL_CON2 %#x, BUSP divider %#x; Q-channels MFC %#x (state %#x), SysMMU %#x, %#x\n",
	    rd(cmu_mfc, MFC_BUS_USER), rd(cmu_mfc, 0x108), rd(cmu_mfc, 0x1804), rd(cmu_mfc, 0x302C), rd(cmu_mfc, 0x702C), rd(cmu_mfc, 0x3050),
	    rd(cmu_mfc, 0x3054));
	regs = (volatile uint8_t *)ml_io_map(MFC_PA, MFC_SIZE);
	mmu0 = (volatile uint8_t *)ml_io_map(SYSMMU0_PA, SYSMMU_SIZE);
	mmu1 = (volatile uint8_t *)ml_io_map(SYSMMU1_PA, SYSMMU_SIZE);
	say("exynos_mfc: 2: the SysMMUs: CTRL %#x (version %#x) and %#x (version %#x)\n", rd(mmu0, MMU_CTRL), rd(mmu0, MMU_VERSION),
	    rd(mmu1, MMU_CTRL), rd(mmu1, MMU_VERSION));
	if ((rd(mmu0, MMU_CTRL) | rd(mmu1, MMU_CTRL)) & 5) {
		/* CTRL 0 lets addresses through. ABOX's SysMMU needed the same. */
		wr(mmu0, MMU_CTRL, 0);
		wr(mmu1, MMU_CTRL, 0);
		say("exynos_mfc: 2: they were not passing addresses through: now CTRL %#x, %#x\n", rd(mmu0, MMU_CTRL), rd(mmu1, MMU_CTRL));
	}
	say("exynos_mfc: 2: the block: version %#x, firmware register %#x\n", R(MFC_VERSION), R(FW_VERSION));
	return true;
}

static bool
firmware_start(void)
{
	const uint8_t *data = NULL;
	uint64_t size = 0;
	uint32_t answer;
	uint8_t *code;
	char partition[24] = "VENDOR";

	PE_parse_boot_arg_str("mfc_fw_part", partition, sizeof(partition));
	/*
	 * The loader keeps this range out of XNU's memory when mfc= is on the command line. If XNU owns a page in it, the range is
	 * not ours and is left alone.
	 */
	if (pmap_valid_page((ppnum_t)atop_64(EXYNOS9810_MEM_MFC_PA)) || pmap_valid_page((ppnum_t)atop_64(EXYNOS9810_MEM_MFC_END - 4096))) {
		say("exynos_mfc: 3: %#llx-%#llx is XNU's RAM (a loader without mfc='s keep-out?): not taken\n", EXYNOS9810_MEM_MFC_PA,
		    EXYNOS9810_MEM_MFC_END);
		return false;
	}
	mem = (uint8_t *)hakios_physmap_wc(EXYNOS9810_MEM_MFC_PA, EXYNOS9810_MEM_SIZE(MFC));
	if (mem == NULL) {
		say("exynos_mfc: 3: mfc's memory could not be mapped\n");
		return false;
	}
	*(volatile uint32_t *)(mem + EXYNOS9810_MEM_SIZE(MFC) - 4) = 0x4D464321;
	if (*(volatile uint32_t *)(mem + EXYNOS9810_MEM_SIZE(MFC) - 4) != 0x4D464321) {
		say("exynos_mfc: 3: mfc's memory does not read back\n");
		return false;
	}
	/*
	 * The storage may come up late, and hakios_partition_file remembers a failure for the rest of the run. So first wait (up to 60 s)
	 * until the partition answers a read.
	 */
	static uint8_t probe[4096];
	uint64_t got = 0;
	int waited;

	/*
	 * fw/mfc_fw.bin of the small image first (DEVICE=star*lte builds put the vendor's there: a phone whose VENDOR
	 * partition is another ROM's, or wiped, has none), then the phone's own VENDOR.
	 */
	data = hakios_image_file("fw", "mfc_fw.bin", &size, NULL);
	if (data != NULL && (size == 0 || size > FW_CODE)) {
		say("exynos_mfc: 3: fw/mfc_fw.bin is %llu bytes (at most %u): not used\n", size, (unsigned)FW_CODE);
		data = NULL;
	}
	if (data != NULL) {
		say("exynos_mfc: 3: the firmware from fw/mfc_fw.bin of the small image\n");
	} else {
		for (waited = 0; waited < 120 && hakios_ufs_read_partition(partition, 0, probe, sizeof(probe), &got) != 0; waited++) {
			IOSleep(500);
		}
		say("exynos_mfc: 3: the %s partition answers after %d ms more (%llu bytes)\n", partition, waited * 500, got);
		if (hakios_partition_file(partition, "/vendor", "/firmware/mfc_fw.bin", FW_CODE, &data, &size) != 0 || data == NULL) {
			say("exynos_mfc: 3: mfc_fw.bin is neither in fw/ nor on the phone's %s partition: no hardware video\n",
			    partition);
			return false;
		}
	}
	code = mfc_alloc(FW_CODE + DEV_CTX);
	bzero(code, FW_CODE + DEV_CTX);
	memcpy(code, data, size);
	__builtin_arm_dsb(15);
	say("exynos_mfc: 3: mfc_fw.bin, %llu bytes, at %#x\n", size, PA(code));
	/* Reset: clear the firmware-register window, pulse MFC_RESET, then point RISC_BASE_ADDRESS at the image. */
	W(RISC2HOST_CMD, 0);
	W(HOST2RISC_CMD, 0);
	W(FW_VERSION, 0);
	for (uint32_t i = 0; i < 1024; i++) {
		W(0xF000 + 4 * i, 0);
	}
	W(MFC_RESET, 0x1FFF);
	W(MFC_RESET, 0);
	W(RISC_BASE_ADDRESS, PA(code));
	W(RISC2HOST_CMD, 0);
	W(RISC2HOST_INT, 0);
	W(RISC_ON, 1);
	W(MFC_OFF, 0);
	answer = wait_answer(2000);
	if (answer != R2H_FW_STATUS) {
		say("exynos_mfc: 3: the RISC started: answer %u, wanted %u, error %#x; RISC_ON %#x\n", answer, R2H_FW_STATUS, last_error, R(RISC_ON));
		return false;
	}
	W(CONTEXT_MEM_ADDR, PA(code) + FW_CODE);
	W(CONTEXT_MEM_SIZE, DEV_CTX);
	if (expect(H2R_SYS_INIT, R2H_SYS_INIT, "SYS_INIT") != 0) {
		return false;
	}
	say("exynos_mfc: 3: the firmware runs: its date %02x-%02x-%02x (kind %c), the block's version %#x\n",
	    (R(FW_VERSION) >> 16) & 0xFF, (R(FW_VERSION) >> 8) & 0xFF, R(FW_VERSION) & 0xFF, (R(FW_VERSION) >> 24) & 0xFF, R(MFC_VERSION));
	mem_base_used = mem_used;
	fw_up = 1;
	return true;
}

/*
 * Step 4: self-test. test_stream is 6 frames of H.264 baseline, 176 x 144 (ffmpeg's testsrc2, encoded with x264 for this test).
 * test_units are the access unit boundaries in test_stream. test_crc holds each frame's CRC as ffmpeg decodes it, tags 100 to 105.
 */
static const uint8_t test_stream[] = {
#include "hakios.exynos_mfc_test.h"
};
static const uint32_t test_units[] = { 0, 2629, 3245, 4002, 4640, 5258, sizeof(test_stream) };
static const uint32_t test_crc[6] = { 0xd1ffb670, 0xc0a84809, 0x4c045182, 0xbbd7939f, 0x82230b84, 0xcd52fb70 };

/* CRC32 over the cropped luma rows, then the cropped chroma rows. */
static uint32_t
frame_crc(const uint8_t *y, const uint8_t *uv)
{
	static uint8_t row[4096];
	uint32_t crc = 0;

	for (uint32_t i = 0; i < I.crop_h; i++) {
		memcpy(row, y + (i + I.crop_y) * I.stride + I.crop_x, I.crop_w);
		crc = crc32(crc, row, I.crop_w);
	}
	for (uint32_t i = 0; i < I.crop_h / 2; i++) {
		memcpy(row, uv + (i + I.crop_y / 2) * I.stride + (I.crop_x & ~1u), I.crop_w);
		crc = crc32(crc, row, I.crop_w);
	}
	return crc;
}

static void
self_test(void)
{
	struct mfc_result r;
	uint32_t consumed = 0, unit = 0, shown = 0, right = 0;
	uint64_t begun, ended, ns;
	int error;

	if ((error = inst_open(CODEC_H264)) != 0) {
		say("exynos_mfc: 4: no instance: %d\n", error);
		return;
	}
	say("exynos_mfc: 4: instance %u\n", I.inst);
	memcpy(I.cpb, test_stream, sizeof(test_stream));
	__builtin_arm_dsb(15);
	if ((error = inst_header(test_units[1], &consumed)) != 0 || (error = inst_buffers()) != 0) {
		say("exynos_mfc: 4: the header and the buffers: %d\n", error);
		inst_close();
		return;
	}
	clock_get_uptime(&begun);
	for (unit = 0; unit < 6; unit++) {
		uint32_t offset = test_units[unit], bytes = test_units[unit + 1] - offset;

		if (unit == 0) {
			offset += consumed;
			bytes -= consumed;
		}
		error = inst_frame(bytes, offset, 100 + unit, false, 0, 0, &r);
		if (error != 0) {
			break;
		}
		if (r.shown) {
			uint32_t crc = frame_crc(r.y, r.uv), expected = (r.tag >= 100 && r.tag < 106) ? test_crc[r.tag - 100] : 0;

			shown++;
			right += crc == expected;
			say("exynos_mfc: 4: frame tag %u: CRC %#x, %s (status %#x, %u bytes taken)\n", r.tag, crc, crc == expected ? "right" : "WRONG",
			    r.status, r.consumed);
		} else {
			say("exynos_mfc: 4: unit %u: decoded, nothing shown yet (status %#x)\n", unit, r.status);
		}
	}
	for (uint32_t drain = 0; error == 0 && drain < 8; drain++) {
		error = inst_frame(0, 0, 0, true, 0, 0, &r);
		if (error != 0 || r.finished) {
			break;
		}
		if (r.shown) {
			uint32_t crc = frame_crc(r.y, r.uv), expected = (r.tag >= 100 && r.tag < 106) ? test_crc[r.tag - 100] : 0;

			shown++;
			right += crc == expected;
			say("exynos_mfc: 4: frame tag %u (drained): CRC %#x, %s\n", r.tag, crc, crc == expected ? "right" : "WRONG");
		}
	}
	clock_get_uptime(&ended);
	absolutetime_to_nanoseconds(ended - begun, &ns);
	say("exynos_mfc: 4: %u frames shown, %u with the right CRC, of 6, in %llu us (error %d)\n", shown, right, ns / 1000, error);
	inst_close();
	if (right != 6) {
		say("exynos_mfc: 4: not what ffmpeg decodes: no device for user space\n");
		dead = 1;
	}
}

/* Step 5: the device node for user space. */

static int mfc_opens;

static int
mfc_open(dev_t dev __unused, int flags __unused, int type __unused, proc_t p __unused)
{
	int error = 0;

	lck_mtx_lock(mfc_lock);
	if (dead || !fw_up) {
		error = EIO;
	} else if (mfc_opens != 0) {
		error = EBUSY;                  /* one client at a time; VideoToolbox's next session stays in software */
	} else {
		mfc_opens = 1;
	}
	lck_mtx_unlock(mfc_lock);
	return error;
}

static int
mfc_close(dev_t dev __unused, int flags __unused, int type __unused, proc_t p __unused)
{
	lck_mtx_lock(mfc_lock);
	if (!dead) {
		inst_close();
	}
	mfc_opens = 0;
	if (irq_on) {
		say("exynos_mfc: interrupts so far: %llu, answers they missed %llu\n", irq_count, irq_missed);
	}
	lck_mtx_unlock(mfc_lock);
	return 0;
}

/* Copy the cropped picture out to the client's NV12 planes. */
static int
frame_out(const struct mfc_result *r, struct hakios_mfc_decode *d)
{
	uint32_t w = I.crop_w, h = I.crop_h;
	int error = 0;

	if (d->y == 0 || d->uv == 0 || d->y_stride < w || d->uv_stride < w) {
		return EINVAL;
	}
	if (w > d->room_width || h > d->room_height) {
		return ERANGE;          /* the client's planes are for a smaller picture */
	}
	for (uint32_t i = 0; i < h && error == 0; i++) {
		error = copyout(r->y + (i + I.crop_y) * I.stride + I.crop_x, (user_addr_t)d->y + i * d->y_stride, w);
	}
	for (uint32_t i = 0; i < h / 2 && error == 0; i++) {
		error = copyout(r->uv + (i + I.crop_y / 2) * I.stride + (I.crop_x & ~1u), (user_addr_t)d->uv + i * d->uv_stride, w);
	}
	return error;
}

static int
mfc_ioctl(dev_t dev __unused, u_long cmd, caddr_t data, int flag __unused, proc_t p __unused)
{
	int error = 0;

	lck_mtx_lock(mfc_lock);
	if (dead || !fw_up) {
		lck_mtx_unlock(mfc_lock);
		return EIO;
	}
	switch (cmd) {
	case HAKIOS_MFC_OPEN: {
		struct hakios_mfc_open *o = (struct hakios_mfc_open *)(void *)data;
		uint32_t consumed = 0;

		I.zc = false;
		/* A second open from the client. After a drain the firmware's instance has finished, so closing it is safe. */
		inst_close();
		if (dead) {
			error = EIO;
			break;
		}
		if (o->codec != HAKIOS_MFC_H264 && o->codec != HAKIOS_MFC_HEVC) {
			error = ENOTSUP;
			break;
		}
		if ((error = inst_open(o->codec == HAKIOS_MFC_H264 ? CODEC_H264 : CODEC_HEVC)) != 0) {
			break;
		}
		if (o->header_bytes == 0 || o->header_bytes > CPB_SIZE - 1024 || (error = copyin((user_addr_t)o->header, I.cpb, o->header_bytes)) != 0) {
			error = error != 0 ? error : EINVAL;
			inst_close();
			break;
		}
		__builtin_arm_dsb(15);
		I.zc = zero_copy && (o->flags & HAKIOS_MFC_ZERO_COPY) != 0;
		if ((error = inst_header(o->header_bytes, &consumed)) != 0) {
			inst_close();
			break;
		}
		/* Zero copy needs the picture to start at the surface's first byte, so no crop offset. */
		I.zc = I.zc && I.crop_x == 0 && I.crop_y == 0;
		if ((error = inst_buffers()) != 0) {
			inst_close();
			break;
		}
		o->width = I.crop_w;
		o->height = I.crop_h;
		o->flags = I.zc ? HAKIOS_MFC_ZERO_COPY : 0;
		o->stride = I.stride;
		o->rows = I.rows;
		break;
	}
	case HAKIOS_MFC_DECODE: {
		struct hakios_mfc_decode *d = (struct hakios_mfc_decode *)(void *)data;
		struct mfc_result r;

		hakios_exynos_bus_busy();       /* bus_max=2: MIF and INT at their top while video is decoded */
		if (!I.open || !I.buffers) {
			error = ENXIO;
			break;
		}
		d->shown = 0;
		if (d->bytes > CPB_SIZE - 1024) {
			error = EFBIG;
			break;
		}
		if (d->bytes != 0 && (error = copyin((user_addr_t)d->stream, I.cpb, d->bytes)) != 0) {
			break;
		}
		__builtin_arm_dsb(15);
		uint64_t y = 0, uv = 0;

		if (I.zc) {
			uint32_t y_stride = 0, uv_stride = 0, y_bytes = 0, uv_bytes = 0;

			/*
			 * Zero copy: the surface must be a frame of this memory, with the strides and sizes the instance expects. Otherwise
			 * ENXIO; the client can open again for copies.
			 */
			if (hakios_surface_mfc_frame(d->surface, &y, &uv, &y_stride, &uv_stride, &y_bytes, &uv_bytes) != 0 || y_stride != I.stride ||
			    uv_stride != I.stride || y_bytes < I.plane0 || uv_bytes < I.plane1 || (y & 0xFF) != 0 || (uv & 0xFF) != 0 ||
			    y < EXYNOS9810_MEM_MFC_PA || uv + uv_bytes > EXYNOS9810_MEM_MFC_END) {
				static int told;

				if (told++ < 4) {
					say("exynos_mfc: surface %u is not a frame of the block's memory of %u bytes a row, %u + %u bytes: at %#llx + %#llx, %u and %u a row, %u + %u bytes\n", d->surface,
					    I.stride, I.plane0, I.plane1, y, uv, y_stride, uv_stride, y_bytes, uv_bytes);
				}
				error = ENXIO;
				break;
			}
		}
		error = inst_frame(d->bytes, 0, d->tag, d->bytes == 0, (uint32_t)y, (uint32_t)uv, &r);
		if (error != 0) {
			break;
		}
		d->finished = r.finished;
		d->slot = r.slot;
		d->shown_slot = r.shown_slot;
		d->used = I.used | I.pending;
		if (r.shown) {
			d->shown = 1;
			d->shown_tag = r.tag;
			d->width = I.crop_w;
			d->height = I.crop_h;
			if (!I.zc) {
				error = frame_out(&r, d);
			} else if (r.shown_slot < 0) {
				error = EIO;
			}
		}
		break;
	}
	default:
		error = ENOTTY;
		break;
	}
	lck_mtx_unlock(mfc_lock);
	return error;
}

static const struct cdevsw mfc_cdevsw = {
	.d_open = mfc_open, .d_close = mfc_close, .d_read = eno_rdwrt, .d_write = eno_rdwrt, .d_ioctl = mfc_ioctl, .d_stop = eno_stop,
	.d_reset = eno_reset, .d_ttys = NULL, .d_select = eno_select, .d_mmap = eno_mmap, .d_strategy = eno_strat,
	.d_reserved_1 = eno_getc, .d_reserved_2 = eno_putc, .d_type = 0,
};

static void
mfc_thread(void *arg __unused, wait_result_t w __unused)
{
	/* The VENDOR partition comes up with the storage, a little after the kernel's drivers start. */
	IOSleep(3000);
	if (steps >= 2 && !block_on()) {
		dead = 1;
		goto out;
	}
	if (steps >= 3 && !firmware_start()) {
		dead = 1;
		goto out;
	}
	if (steps >= 4) {
		lck_mtx_lock(mfc_lock);
		self_test();
		lck_mtx_unlock(mfc_lock);
	}
	(void)PE_parse_boot_argn("mfc_irq", &irq_wanted, sizeof(irq_wanted));
	if (steps >= 4 && !dead && irq_wanted) {
		kern_return_t attached = gic_v3_spi_attach(MFC_INTID, false, mfc_interrupt, NULL);

		irq_on = attached == KERN_SUCCESS;
		say("exynos_mfc: the interrupt, %u: %s (%d)\n", MFC_INTID, irq_on ? "attached" : "not attached", attached);
	}
	if (steps >= 5 && !dead) {
		int major = cdevsw_add(-1, &mfc_cdevsw);

		if (major < 0 || devfs_make_node(makedev(major, 0), DEVFS_CHAR, UID_ROOT, GID_WHEEL, 0666, "hakios-mfc") == NULL) {
			say("exynos_mfc: 5: no /dev/hakios-mfc\n");
		} else {
			say("exynos_mfc: 5: /dev/hakios-mfc\n");
		}
	}
out:
	say("exynos_mfc: mfc=%u done%s\n", steps, dead ? ": THE BLOCK IS LEFT ALONE, video stays in software" : "");
	thread_terminate(current_thread());
}

void
hakios_exynos_mfc_init(void)
{
	thread_t thread;
	static lck_grp_t *group;

	if (!PE_parse_boot_argn("mfc", &steps, sizeof(steps)) || steps == 0) {
		return;
	}
	group = lck_grp_alloc_init("hakios_mfc", LCK_GRP_ATTR_NULL);
	mfc_lock = lck_mtx_alloc_init(group, LCK_ATTR_NULL);
	carve_lock = lck_mtx_alloc_init(group, LCK_ATTR_NULL);
	(void)PE_parse_boot_argn("mfc_zc", &zero_copy, sizeof(zero_copy));
	pmu = (volatile uint8_t *)ml_io_map(PMU_PA, PMU_SIZE);
	cmu_top = (volatile uint8_t *)ml_io_map(CMU_TOP_PA, CMU_SIZE);
	cmu_busc = (volatile uint8_t *)ml_io_map(CMU_BUSC_PA, CMU_SIZE);
	look("1");
	if (steps >= 2 && kernel_thread_start(mfc_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
