/*
 * Scan-out on the Exynos 9810 DPU (Galaxy S9): frames are shown from buffers of their own, not from the buffer the DECON
 * is reading.
 *
 * MiniIOMobileFramebuffer.cpp draws each frame into a buffer and calls in here to have the DECON show that buffer from
 * its next refresh on. Otherwise the frame is drawn into the buffer the DECON is reading, which tears. This runs only
 * with boot-arg fb_flip=1 or fb_dpp=1 (read by MiniIOMobileFramebuffer.cpp); with neither, or with fps_log=1 alone, the
 * DECON is only read.
 *
 * Boot state (Samsung's S-Boot sets up the display; Silicium's UEFI and this kernel only write into the buffer): DECON0
 * (0x16030000) in MIPI command mode, frames started by the panel's TE (HW trigger), one window on one DMA channel. That
 * channel's IDMA reads the 1440 x 2960 buffer at 0xCC000000 (the boot framebuffer), through the DPP and DSC to DSIM0 and
 * the S6E3HA8. The DECON reads that buffer once every refresh while the processor writes the next frame into it, so what
 * reaches the panel is part one frame, part the next.
 *
 * Two modes, both off by default:
 *
 *   fb_flip=1   The same channel and scaling on the processor, but into one of three buffers of the panel's size (the
 *               boot framebuffer and two more). When a frame is drawn, its buffer's address is written to the channel's
 *               IDMA_IN_BASE_ADDR and a shadow update is requested (SHADOW_REG_UPDATE_REQ, as Samsung's driver does after
 *               every frame: all windows and the global bit). The DECON takes the new address at the start of its next
 *               frame (the next TE) and clears the request. Only then is the next frame flipped, and a buffer that was
 *               shown before is drawn into again only after that. No buffer is written while the DECON reads it, and
 *               frames go to the panel at most once per refresh.
 *   fb_dpp=1    Scaling in hardware. The window moves to channel VGF0 (IDMA_VGF0, channel 1 in DATA_PATH_CONTROL_1), one
 *               of the two of the six DPPs that has a scaler (SCALED_IMG_SIZE; VGF0 and VGF1 only, and VGF1 is the one
 *               with HDR). It reads the iPhone's 1125 x 2436 picture from three buffers of that size and scales it to the
 *               panel's 1440 x 2960 (8 taps across, 4 down, polyphase, 16 phases). The processor's part is a copy, not a
 *               scaling, and the flips are as above. Before the window moves, VGF0 is set up from scratch and its registers
 *               forced in (SFR_UPDATE_FORCE; it is idle). After, three refreshes are waited for and the configuration errors
 *               of its IDMA and DPP are read. On any error, or if the DECON does not take the change, the window goes back to
 *               the boot loader's channel and the boot buffer.
 *
 *   fb_psr=<ms> The panel has its own memory in command mode. On a screen unchanged for this long (50 to 5000; 0, the
 *               default, is off), the TE trigger is masked (HW_TRIG_MASKED): no refresh, no read of DRAM over DSI. A new
 *               frame unmasks it. Samsung's decon_reg_set_trigger() masks it after every frame.
 *
 *   fb_planes   (with fb_dpp and fb_zc; MiniIOMobileFramebuffer.cpp's boot-arg, on by default) Two layers on two windows:
 *               the window server's surface on the boot loader's window through VGF0, blended by its alpha, over a
 *               layer CoreAnimation detached on window 4 through VGF1 (hakios_scanout_planes(), below), instead of the
 *               processor blending them.
 *
 * The scaler's coefficients are not Samsung's. They are Catmull-Rom (a cubic through the four nearest pixels), computed
 * at start for each of the nine phases the registers take. The other seven of the hardware's sixteen phases are mirrors
 * of these. Units are 1/512, and each row sums to 512.
 *
 * Registers and fields (from Samsung's dpu_9810; see Sources):
 *   DECON0 0x16030000: GLOBAL_CONTROL 0x000 (bit 8 command mode, bit 4 running), SHADOW_REG_UPDATE_REQ 0x060 (bit 31
 *     global, bits 0-5 the windows; cleared by the hardware when taken), HW_SW_TRIG_CONTROL 0x070 (bit 0 HW trigger,
 *     bit 4 HW trigger masked, bit 8 SW trigger), DATA_PATH_CONTROL_0 0x214 (bit 4w window w on),
 *     DATA_PATH_CONTROL_1 0x218 (bits 4w-4w+2: window w's channel), FRAME_COUNT 0x2A0, WIN_START/END_POSITION
 *     0x1008/0x100C + 0x30 w.
 *   DMA channel numbers (DATA_PATH_CONTROL_1) to IDMA blocks: 0 VG0, 1 VGF0, 2 VGF1, 3 G1, 4 VG1, 5 G0; the blocks
 *     in the order G0, G1, VG0, VG1, VGF0, VGF1 at 0x1000 steps from 0x16071000 (IDMA) and 0x16021000 (DPP).
 *   IDMA: ENABLE 0x000 (bit 4 SFR update force, bit 2 busy), IRQ 0x004 (status bits 21 config error, 19 read slave
 *     error, 17 deadlock), IN_CON 0x008 (bits 11-15 format, 3 block, 6-7 AFBC, 8-10 rotation), OUT_CON 0x00C,
 *     SRC_SIZE 0x010 (width in pixels = the stride, height), SRC_OFFSET 0x014, IMG_SIZE 0x018, IN_BASE_ADDR_Y/C
 *     0x040/0x044 (32 bits: the buffers must be below 4 GB), DEADLOCK_NUM 0x050, BUS_CON 0x054, DYNAMIC_GATING_EN
 *     0x058, IN_REQ_DEST 0x068, CFG_ERR_STATE 0x870 (11 bits).
 *   DPP: ENABLE 0x000 (bit 4 SFR update force), IRQ 0x004 (bit 21 config error), IN_CON 0x008 (bits 0-2 format, 0 is
 *     ARGB8888; bit 3 per-pixel alpha), IMG_SIZE 0x018, SCALED_IMG_SIZE 0x02C, MAIN_H_RATIO/V_RATIO 0x044/0x048
 *     (source / destination in 12.20), the coefficients: vertical (4 taps) at 0x200 + 4 phase + 0x24 tap, horizontal
 *     (8 taps) at 0x290 + 4 phase + 0x24 tap, phases 0-8, 11 bits signed, the luma bank and +0x200 the chroma one (both
 *     written, as Samsung does), the start positions YH, YV, CH, CV 0x5B0-0x5BC ((source - destination) / (2
 *     destination) in 12.20, two's complement), DYNAMIC_GATING_EN 0xA54, CFG_ERR_STATE 0xD08 (5 bits).
 *   Limits (dpp.h, dpp_drv.c): sizes 16-4096, scaling from 1/2 to 8 times; 1125 -> 1440 and 2436 -> 2960 are within.
 *
 * Sources: Samsung's kernel for the S9, github.com/Scion-Project/android_kernel_samsung_exynos9810 (lineage-24.0),
 * drivers/video/fbdev/exynos/dpu_9810: regs-decon.h, regs-dpp.h, decon_reg.c (decon_reg_config_win_channel,
 * decon_reg_set_trigger, decon_reg_wait_for_update_timeout), dpp_reg.c (dpp_reg_set_h_coef/v_coef, the ratio and
 * initial-phase formulas, dpp_reg_set_format), dpp_drv.c (the ratio, dpp_check_scale_ratio), dpp.h, helper.c
 * (dpu_dma_type_to_channel); and the device tree of the same tree (exynos9810-starlte_eur_open_26.dts: decon_f@16030000,
 * dpp@16021000-16026000 with their IDMA at 16071000-16076000). GPL: read for these facts, nothing copied.
 */
#include <stdarg.h>
#include <kern/clock.h>
#include <kern/thread_call.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/systm.h>
#include <sys/hakios.exynos9810_mem.h>
#include <sys/hakios.exynos_soc.h>
#include <sys/hakios.log.h>
#include <sys/hakios.xnu.h>
extern void IOSleep(unsigned milliseconds);

/* the display driver's side (MiniIOMobileFramebuffer.cpp); stand-ins in drivers/core/hakios.stubs.c */
int hakios_scanout_probe(uint64_t fb_phys, uint32_t width, uint32_t height, uint32_t rowbytes);
int hakios_scanout_memory(uint64_t bytes, unsigned count, uint64_t *phys);
int hakios_scanout_attach(int mode, const uint64_t *phys, unsigned count, uint32_t width, uint32_t height,
    uint32_t rowbytes, unsigned shown);
int hakios_scanout_flip(unsigned index);
int hakios_scanout_latched(void);
void hakios_scanout_detach(const char *why);
uint32_t hakios_scanout_refreshes(void);
int hakios_scanout_carve(uint64_t bytes, uint64_t *phys);
void hakios_scanout_uncarve(uint64_t phys);
int hakios_scanout_carved(uint64_t phys);
int hakios_scanout_show(uint64_t phys, uint32_t rowbytes);
int hakios_scanout_planes(uint64_t upper, uint32_t upper_rowbytes, uint64_t lower, uint32_t lower_rowbytes, uint32_t x,
    uint32_t y, uint32_t w, uint32_t h);

#define DECON_PA                EXYNOS_DECON_PA
#define DPP_PA                  EXYNOS_DPP_PA
#define DMA_PA                  EXYNOS_DPU_DMA_PA
#define BLOCK_SIZE              0x7000          /* the common block and the six channels' */

#define GLOBAL_CONTROL          0x000
#define  GC_CMD_MODE            (1u << 8)
#define  GC_RUN                 (1u << 4)
#define SHADOW_REQ              0x060
#define  SHADOW_ALL             (0x80000000u | 0x3fu)
#define TRIG_CONTROL            0x070
#define  HW_TRIG_EN             (1u << 0)
#define  HW_TRIG_MASKED         (1u << 4)
#define  SW_TRIG_EN             (1u << 8)
#define DATA_PATH_0             0x214
#define DATA_PATH_1             0x218
#define FRAME_COUNT             0x2A0
#define WIN_START(w)            (0x1008 + 0x30 * (w))
#define WIN_END(w)              (0x100C + 0x30 * (w))
#define WINDOWS                 6

#define IDMA_ENABLE             0x000
#define  SFR_UPDATE_FORCE       (1u << 4)
#define  OP_BUSY                (1u << 2)
#define IDMA_IRQ                0x004
#define  IDMA_IRQ_ERRORS        ((1u << 21) | (1u << 19) | (1u << 17))
#define IDMA_IN_CON             0x008
#define  IN_CON_KEEP            ((0xffu << 19) | (0x1fu << 11))     /* IC_MAX and the format */
#define  IN_CON_ODD             ((7u << 8) | (3u << 6) | (1u << 3)) /* rotation and flips, AFBC, blocking */
#define IDMA_OUT_CON            0x00C
#define IDMA_SRC_SIZE           0x010
#define IDMA_SRC_OFFSET         0x014
#define IDMA_IMG_SIZE           0x018
#define IDMA_BASE_Y             0x040
#define IDMA_BASE_C             0x044
#define IDMA_DEADLOCK           0x050
#define IDMA_BUS_CON            0x054
#define IDMA_GATING             0x058
#define IDMA_REQ_DEST           0x068
#define IDMA_CFG_ERR            0x870

#define DPP_ENABLE              0x000
#define DPP_IRQ                 0x004
#define  DPP_IRQ_ERRORS         (1u << 21)
#define DPP_IN_CON              0x008
#define  DPP_IN_KEEP            ((1u << 3) | 7u)                    /* per-pixel alpha, the format */
#define DPP_IMG_SIZE            0x018
#define DPP_SCALED_SIZE         0x02C
#define DPP_H_RATIO             0x044
#define DPP_V_RATIO             0x048
#define DPP_V_COEF(n, s, x)     (0x200 + 4 * (n) + 0x24 * (s) + 0x200 * (x))
#define DPP_H_COEF(n, s, x)     (0x290 + 4 * (n) + 0x24 * (s) + 0x200 * (x))
#define DPP_YH_POS              0x5B0
#define DPP_YV_POS              0x5B4
#define DPP_CH_POS              0x5B8
#define DPP_CV_POS              0x5BC
#define DPP_GATING              0xA54
#define DPP_CFG_ERR             0xD08

#define IDMA_VGF0               4               /* the blocks' order: G0 G1 VG0 VG1 VGF0 VGF1 */
#define CHANNEL_VGF0            1

static const int type_of_channel[8] = { 2, 4, 5, 1, 3, 0, -1, -1 };
static const char *const type_name[6] = { "G0", "G1", "VG0", "VG1", "VGF0", "VGF1" };

static vm_offset_t decon, dpp, dma;
static int probed;                      /* 0 until probed; 1 if the DECON is as expected; -1 if not (nothing written) */
static unsigned win, boot_type;         /* the boot loader's window and its channel's block */
static uint32_t boot_base, boot_data_path_1;
static int mode;                        /* 0 the boot loader's, 1 flip on its channel, 2 VGF0's scaler */
static unsigned channel_type;           /* the block whose base address the flips write */
static uint64_t buffers[4];
static unsigned nbuffers;

static inline uint32_t rd(vm_offset_t base, uint32_t offset) { return *(volatile uint32_t *)(base + offset); }
static inline void wr(vm_offset_t base, uint32_t offset, uint32_t value) { *(volatile uint32_t *)(base + offset) = value; }
static inline vm_offset_t idma(unsigned type) { return dma + 0x1000 * (type + 1); }
static inline vm_offset_t dppc(unsigned type) { return dpp + 0x1000 * (type + 1); }

/* the shadow update taken (all of it: the DECON's request register reads 0); up to `ms` milliseconds */
static int
wait_taken(unsigned ms)
{
	for (unsigned i = 0; i <= ms; i++) {
		if ((rd(decon, SHADOW_REQ) & SHADOW_ALL) == 0) {
			return 0;
		}
		IOSleep(1);
	}
	return -1;
}

static void
dump_channel(const char *what, unsigned type)
{
	vm_offset_t i = idma(type), d = dppc(type);

	say("exynos_dpu: %s %s: IDMA enable %x irq %x in %x out %x src %x offset %x img %x base %08x/%08x deadlock %x "
	    "bus %x gating %x dest %x cfg-err %x; DPP enable %x irq %x in %x img %x scaled %x ratio %x/%x cfg-err %x\n",
	    what, type_name[type], rd(i, IDMA_ENABLE), rd(i, IDMA_IRQ), rd(i, IDMA_IN_CON), rd(i, IDMA_OUT_CON),
	    rd(i, IDMA_SRC_SIZE), rd(i, IDMA_SRC_OFFSET), rd(i, IDMA_IMG_SIZE), rd(i, IDMA_BASE_Y), rd(i, IDMA_BASE_C),
	    rd(i, IDMA_DEADLOCK), rd(i, IDMA_BUS_CON), rd(i, IDMA_GATING), rd(i, IDMA_REQ_DEST), rd(i, IDMA_CFG_ERR),
	    rd(d, DPP_ENABLE), rd(d, DPP_IRQ), rd(d, DPP_IN_CON), rd(d, DPP_IMG_SIZE),
	    type >= IDMA_VGF0 ? rd(d, DPP_SCALED_SIZE) : 0, type >= 2 ? rd(d, DPP_H_RATIO) : 0,
	    type >= 2 ? rd(d, DPP_V_RATIO) : 0, rd(d, DPP_CFG_ERR));
}

/*
 * Is the display what this file expects: the boot framebuffer at 0xCC000000, 1440 x 2960, and a DECON0 running in
 * command mode with one window on, whose channel reads that buffer at that size? Reads only. 0 if it is.
 */
int
hakios_scanout_probe(uint64_t fb_phys, uint32_t width, uint32_t height, uint32_t rowbytes)
{
	uint32_t gc, trig, path0, path1, frames, base, img, on = 0;
	int type;

	if (!HAKIOS_SOC_HAS(DECON_PA)) {
		return -1;
	}

	if (probed != 0) {
		return probed > 0 ? 0 : -1;
	}
	probed = -1;
	if (fb_phys != 0xCC000000ULL || width != 1440 || height != 2960 || rowbytes != 1440 * 4) {
		say("exynos_dpu: the boot framebuffer is at %#llx, %ux%u, %u bytes a row: not the S9's (0xcc000000, "
		    "1440x2960): the DECON is left alone\n", fb_phys, width, height, rowbytes);
		return -1;
	}
	decon = ml_io_map(DECON_PA, 0x2000);
	dpp = ml_io_map(DPP_PA, BLOCK_SIZE);
	dma = ml_io_map(DMA_PA, BLOCK_SIZE);
	if (decon == 0 || dpp == 0 || dma == 0) {
		say("exynos_dpu: cannot map the DECON, the DPP or the DMA\n");
		return -1;
	}
	gc = rd(decon, GLOBAL_CONTROL);
	trig = rd(decon, TRIG_CONTROL);
	path0 = rd(decon, DATA_PATH_0);
	path1 = rd(decon, DATA_PATH_1);
	frames = rd(decon, FRAME_COUNT);
	for (unsigned w = 0; w < WINDOWS; w++) {
		if (path0 & (1u << (4 * w))) {
			on++;
			win = w;
		}
	}
	say("exynos_dpu: DECON0: control %x (%s mode, %s), trigger %x (HW %s%s, SW %s), shadow request %x, windows %x "
	    "(%u on), channels %x, frame count %u\n", gc, (gc & GC_CMD_MODE) ? "command" : "video",
	    (gc & GC_RUN) ? "running" : "NOT running", trig, (trig & HW_TRIG_EN) ? "on" : "off",
	    (trig & HW_TRIG_MASKED) ? " but masked" : "", (trig & SW_TRIG_EN) ? "on" : "off", rd(decon, SHADOW_REQ),
	    path0, on, path1, frames);
	if (on != 1) {
		say("exynos_dpu: %u windows on, not one: the DECON is left alone\n", on);
		return -1;
	}
	type = type_of_channel[(path1 >> (4 * win)) & 7];
	if (type < 0) {
		say("exynos_dpu: window %u is on channel %u, which is no IDMA's: the DECON is left alone\n", win,
		    (path1 >> (4 * win)) & 7);
		return -1;
	}
	boot_type = (unsigned)type;
	say("exynos_dpu: window %u: %x to %x, on channel %u (%s)\n", win, rd(decon, WIN_START(win)),
	    rd(decon, WIN_END(win)), (path1 >> (4 * win)) & 7, type_name[boot_type]);
	dump_channel("the boot loader's", boot_type);
	base = rd(idma(boot_type), IDMA_BASE_Y);
	img = rd(idma(boot_type), IDMA_IMG_SIZE);
	if (base != (uint32_t)fb_phys || img != (height << 16 | width)) {
		say("exynos_dpu: the channel reads %08x, %ux%u, not the boot framebuffer: the DECON is left alone\n", base,
		    img & 0x1fff, img >> 16 & 0x1fff);
		return -1;
	}
	if (!(gc & GC_CMD_MODE) || !(gc & GC_RUN)) {
		say("exynos_dpu: the DECON is not running in command mode: left alone\n");
		return -1;
	}
	boot_base = base;
	boot_data_path_1 = path1;
	channel_type = boot_type;
	probed = 1;
	say("exynos_dpu: probe: as expected (window %u on %s reading the boot framebuffer, command mode, running; the "
	    "boot channel's IDMA enable reads %x, not checked): flips and the scaler may be asked for\n", win,
	    type_name[boot_type], rd(idma(boot_type), IDMA_ENABLE));
	/* the pool slots are listed in scan_slots[] order (below); layout in sys/hakios.exynos9810_mem.h */
	say("exynos_dpu: memory: scan-out buffers %#llx-%#llx, fb_zc's pool four slots of 12 MB at %#llx %#llx %#llx %#llx "
	    "(all below %#llx)\n", EXYNOS9810_MEM_DPU_SCAN_PA, EXYNOS9810_MEM_DPU_SCAN_END - 1, EXYNOS9810_MEM_DPU_POOL2_PA,
	    EXYNOS9810_MEM_DPU_POOL0_PA, EXYNOS9810_MEM_DPU_POOL1_PA, EXYNOS9810_MEM_DPU_POOL1_PA + (12ULL << 20),
	    EXYNOS9810_MEM_DISPLAY_LIMIT);
	return 0;
}

/*
 * Where the scan-out's buffers go. The DECON's DMA takes 32-bit addresses, and none of this kernel's memory is below
 * 4 GB on the S9 (the loader gives XNU the bank from 0x880000000 up; IOKit's "below 4 GB" never succeeds there). The
 * buffers go in the lower bank, in the gap of the phone's device tree between the moved payload and the panic log
 * (0xD0000000 - 0xD3FFFFFF) and secure_camera (0xE1900000): no reserved-memory node, outside XNU's RAM, and used by
 * neither payload layout (devices/starlte/README.md, the memory table). Not 0xF0000000 and up, which holds the Shannon
 * modem's region (modem_if, cp_ram_logging) and then ABOX's. `count` buffers of `bytes` each from 0xD4000000, at least
 * 64 KB apart; physical addresses, the caller maps them (write-combined). 0 if they fit.
 */
#define SCAN_MEMORY_PA          EXYNOS9810_MEM_DPU_SCAN_PA      /* 0xD4000000 (sys/hakios.exynos9810_mem.h) */
#define SCAN_MEMORY_END         EXYNOS9810_MEM_DPU_SCAN_END     /* 0xD7000000 */
/*
 * The buffers above take 34 MB at most (the fb_flip buffers of the panel's size) of their 48 MB. fb_zc has a pool of
 * its own: the window server's display surfaces are made from it (MiniIOSurface.cpp), so VGF0 scans out the surface
 * CoreAnimation drew the frame into, and nothing is copied (MiniIOMobileFramebuffer.cpp). Slots are 12 MB, one surface
 * each: the iPhone's 1125 x 2436 at 4544 or 4608 bytes a row is 11 MB. Four slots sit in the three free pieces of the
 * gap below 0xE0000000 (sys/hakios.exynos9810_mem.h). Anything drawn from 0xE0000000 up does not reach the panel: a slot
 * at 0xDF800000 that ran over it showed the bottom quarter of every other frame black. CoreAnimation keeps two display
 * surfaces, which take the first two slots, 0xDEC00000 and 0xD7000000: both are pieces the display has scanned out of
 * before.
 */
#define SCAN_SLOT               (12ULL << 20)
static const uint64_t scan_slots[] = {
	EXYNOS9810_MEM_DPU_POOL2_PA,                    /* 0xDEC00000, the one slot below 0xE0000000 there */
	EXYNOS9810_MEM_DPU_POOL0_PA,                    /* 0xD7000000, between the scan-out buffers and the camera's */
	EXYNOS9810_MEM_DPU_POOL1_PA,                    /* 0xDA400000 and 0xDB000000, between UFS's and Wi-Fi's */
	EXYNOS9810_MEM_DPU_POOL1_PA + SCAN_SLOT,
};
#define SCAN_SLOTS              ((unsigned)(sizeof(scan_slots) / sizeof(scan_slots[0])))
_Static_assert(EXYNOS9810_MEM_DPU_POOL2_PA + SCAN_SLOT <= EXYNOS9810_MEM_DPU_POOL2_END &&
    EXYNOS9810_MEM_DPU_POOL0_PA + SCAN_SLOT <= EXYNOS9810_MEM_DPU_POOL0_END &&
    EXYNOS9810_MEM_DPU_POOL1_PA + 2 * SCAN_SLOT <= EXYNOS9810_MEM_DPU_POOL1_END,
    "exynos_dpu: every slot inside its piece of the pool (and so below 0xE0000000)");
_Static_assert(SCAN_SLOTS == 4, "exynos_dpu: the probe's line names four slots");
_Static_assert(SCAN_SLOTS >= 2 && SCAN_SLOTS <= 32, "exynos_dpu: the pool holds CoreAnimation's two surfaces, and pool_used's bits");
static uint32_t pool_used;              /* a bit for every slot given out (SCAN_SLOTS <= 32) */

/* the slot `phys` is in, or SCAN_SLOTS */
static unsigned
scan_slot_of(uint64_t phys)
{
	for (unsigned k = 0; k < SCAN_SLOTS; k++) {
		if (phys >= scan_slots[k] && phys < scan_slots[k] + SCAN_SLOT) {
			return k;
		}
	}
	return SCAN_SLOTS;
}
static uint32_t scan_rowbytes;          /* the buffers' row length as attached: a flip back to them restores it */
static uint32_t shown_rowbytes;         /* what the IDMA reads now */

int
hakios_scanout_memory(uint64_t bytes, unsigned count, uint64_t *phys)
{
	uint64_t step = (bytes + 0xFFFFULL) & ~0xFFFFULL;

	if (probed <= 0) {
		say("exynos_dpu: memory: not the S9's DECON as expected: no buffers of its own\n");
		return -1;
	}
	if (SCAN_MEMORY_PA + step * count > SCAN_MEMORY_END) {
		say("exynos_dpu: memory: %u buffers of %llu bytes do not fit between %#llx and %#llx\n", count, bytes,
		    SCAN_MEMORY_PA, SCAN_MEMORY_END);
		return -1;
	}
	for (unsigned k = 0; k < count; k++) {
		phys[k] = SCAN_MEMORY_PA + step * k;
	}
	say("exynos_dpu: memory: %u buffers of %llu bytes at %#llx to %#llx (of %#llx to %#llx)\n", count, bytes,
	    SCAN_MEMORY_PA, SCAN_MEMORY_PA + step * count, SCAN_MEMORY_PA, SCAN_MEMORY_END);
	return 0;
}

/*
 * fb_zc: a slot of the pool for a surface of `bytes` (physically contiguous, below 4 GB, outside XNU's RAM: the caller
 * maps it write-combined). 0 and its address in `phys`; -1 when the DPU is not the S9's or no slot is free. Called by
 * MiniIOSurface with its own lock held: nothing here sleeps.
 */
int
hakios_scanout_carve(uint64_t bytes, uint64_t *phys)
{
	if (probed <= 0 || bytes == 0 || bytes > SCAN_SLOT) {
		return -1;
	}
	for (unsigned k = 0; k < SCAN_SLOTS; k++) {
		uint32_t used = __atomic_load_n(&pool_used, __ATOMIC_RELAXED);

		while ((used & (1u << k)) == 0) {
			if (__atomic_compare_exchange_n(&pool_used, &used, used | (1u << k), 0, __ATOMIC_SEQ_CST,
			    __ATOMIC_SEQ_CST)) {
				*phys = scan_slots[k];
				return 0;
			}
		}
	}
	return -1;
}

void
hakios_scanout_uncarve(uint64_t phys)
{
	if (hakios_scanout_carved(phys)) {
		unsigned k = scan_slot_of(phys);

		__atomic_fetch_and(&pool_used, ~(1u << k), __ATOMIC_SEQ_CST);
	}
}

/*
 * Is `phys` in the pool? (The Mali's tables take it, though the kernel does not manage it: drivers/gpu/mali/
 * hakios.mali.c.)
 */
int
hakios_scanout_carved(uint64_t phys)
{
	return probed > 0 && scan_slot_of(phys) < SCAN_SLOTS;
}

/*
 * fb_psr state: psr_ms the idle limit in ms (0 off), psr_last_frame the time of the last frame, and psr_masked
 * whether the TE trigger is masked.
 */
static uint32_t psr_ms;
static thread_call_t psr_call;
static uint64_t psr_last_frame;
static bool psr_masked;
static void plane_off(void);            /* fb_planes (below): the lower window off at the next update */

/* a frame is coming: unmask the trigger before the shadow update is requested, or that update is never taken */
static void
psr_wake(void)
{
	if (psr_ms == 0) {
		return;
	}
	__atomic_store_n(&psr_last_frame, mach_absolute_time(), __ATOMIC_RELEASE);
	if (__atomic_exchange_n(&psr_masked, false, __ATOMIC_ACQ_REL)) {
		wr(decon, TRIG_CONTROL, rd(decon, TRIG_CONTROL) & ~HW_TRIG_MASKED);
		__asm__ volatile ("dsb sy" ::: "memory");
	}
}

static void
psr_tick(thread_call_param_t p0 __unused, thread_call_param_t p1 __unused)
{
	uint64_t now = mach_absolute_time(), idle, deadline, ns;

	if (psr_ms == 0) {
		return;
	}
	idle = now - __atomic_load_n(&psr_last_frame, __ATOMIC_ACQUIRE);
	absolutetime_to_nanoseconds(idle, &ns);
	if (mode == 2 && !__atomic_load_n(&psr_masked, __ATOMIC_ACQUIRE) && ns >= (uint64_t)psr_ms * NSEC_PER_MSEC &&
	    rd(decon, SHADOW_REQ) == 0) {
		__atomic_store_n(&psr_masked, true, __ATOMIC_RELEASE);
		wr(decon, TRIG_CONTROL, rd(decon, TRIG_CONTROL) | HW_TRIG_MASKED);
		__asm__ volatile ("dsb sy" ::: "memory");
		/* a frame that arrived while masking may have seen the trigger unmasked: unmask again, so its update goes out */
		if (rd(decon, SHADOW_REQ) != 0) {
			__atomic_store_n(&psr_masked, false, __ATOMIC_RELEASE);
			wr(decon, TRIG_CONTROL, rd(decon, TRIG_CONTROL) & ~HW_TRIG_MASKED);
		}
	}
	clock_interval_to_deadline(psr_ms < 500 ? psr_ms : 500, NSEC_PER_MSEC, &deadline);
	thread_call_enter_delayed(psr_call, deadline);
}

/*
 * fb_zc: VGF0 scans out a surface of the pool from the next refresh on. rowbytes is the row length (the display's size
 * stays as attached). As for hakios_scanout_flip(), the caller has waited for any earlier shadow update to be taken.
 */
int
hakios_scanout_show(uint64_t phys, uint32_t rowbytes)
{
	vm_offset_t i;

	if (mode != 2 || (phys & 0xff) || (rowbytes & 3) || !hakios_scanout_carved(phys) ||
	    rowbytes / 4 < (rd(idma(channel_type), IDMA_IMG_SIZE) & 0x3fff) || rowbytes / 4 > 0xffff) {
		return -1;
	}
	psr_wake();
	i = idma(channel_type);
	if (rowbytes != shown_rowbytes) {
		wr(i, IDMA_SRC_SIZE, (rd(i, IDMA_SRC_SIZE) & 0xffff0000u) | (rowbytes / 4));
		shown_rowbytes = rowbytes;
	}
	wr(i, IDMA_BASE_Y, (uint32_t)phys);
	wr(i, IDMA_BASE_C, (uint32_t)phys);
	plane_off();                            /* (fb_planes: one layer again) */
	__asm__ volatile ("dsb sy" ::: "memory");
	wr(decon, SHADOW_REQ, rd(decon, SHADOW_REQ) | SHADOW_ALL);
	return 0;
}

uint32_t
hakios_scanout_refreshes(void)
{
	return probed > 0 ? rd(decon, FRAME_COUNT) : 0;
}

int
hakios_scanout_latched(void)
{
	return probed > 0 && (rd(decon, SHADOW_REQ) & SHADOW_ALL) == 0;
}

int
hakios_scanout_flip(unsigned index)
{
	vm_offset_t i;

	if (mode == 0 || index >= nbuffers) {
		return -1;
	}
	psr_wake();
	i = idma(channel_type);
	if (mode == 2 && shown_rowbytes != scan_rowbytes) {
		/* returning from a pool surface, whose rows may be shorter */
		wr(i, IDMA_SRC_SIZE, (rd(i, IDMA_SRC_SIZE) & 0xffff0000u) | (scan_rowbytes / 4));
		shown_rowbytes = scan_rowbytes;
	}
	wr(i, IDMA_BASE_Y, (uint32_t)buffers[index]);
	wr(i, IDMA_BASE_C, (uint32_t)buffers[index]);
	plane_off();                            /* (fb_planes: a frame drawn whole) */
	__asm__ volatile ("dsb sy" ::: "memory");
	wr(decon, SHADOW_REQ, rd(decon, SHADOW_REQ) | SHADOW_ALL);
	return 0;
}

/* Catmull-Rom's weight of the pixel `k` (-1 .. 2) for a point `t` / 16 past pixel 0, in 1/512 */
static int
cubic(int k, int t)
{
	/* in 1/(2 * 16^3) units: w(x) for |x| <= 1: 3|x|^3 - 5|x|^2 + 2; 1 < |x| < 2: -|x|^3 + 5|x|^2 - 8|x| + 4 */
	int x = k * 16 - t, a = x < 0 ? -x : x;
	int64_t w;

	if (a <= 16) {
		w = 3LL * a * a * a - 5LL * 16 * a * a + 2LL * 16 * 16 * 16;
	} else if (a < 32) {
		w = -1LL * a * a * a + 5LL * 16 * a * a - 8LL * 16 * 16 * a + 4LL * 16 * 16 * 16;
	} else {
		w = 0;
	}
	/* (w / (2 * 4096)) * 512, rounded */
	return (int)((w * 512 + (w >= 0 ? 4096 : -4096)) / 8192);
}

/* the nine phases of the four taps (pixels -1 .. 2), each row 512 */
static void
coefficients(int16_t table[9][4])
{
	for (int t = 0; t < 9; t++) {
		int sum = 0;

		for (int k = 0; k < 4; k++) {
			table[t][k] = (int16_t)cubic(k - 1, t);
			sum += table[t][k];
		}
		/* the rounding remainder goes to the middle tap: pixel 0, or pixel 1 at phase 8 */
		table[t][t < 8 ? 1 : 2] += (int16_t)(512 - sum);
	}
}

/* the scaler's taps: Catmull-Rom, both banks */
static void
setup_taps(vm_offset_t d)
{
	int16_t c[9][4];

	coefficients(c);
	for (unsigned x = 0; x < 2; x++) {
		for (unsigned n = 0; n < 9; n++) {
			/* 8 taps across: pixel 0 is tap 3, so the four are taps 2 to 5; 4 down: pixel 0 is tap 1 */
			for (unsigned s = 0; s < 8; s++) {
				int v = s >= 2 && s <= 5 ? c[n][s - 2] : 0;

				wr(d, DPP_H_COEF(n, s, x), (uint32_t)v & 0x7ff);
			}
			for (unsigned s = 0; s < 4; s++) {
				wr(d, DPP_V_COEF(n, s, x), (uint32_t)c[n][s] & 0x7ff);
			}
		}
	}
}

/* the scaler's ratios and start positions for sw x sh to dw x dh */
static void
setup_ratio(vm_offset_t d, uint32_t sw, uint32_t sh, uint32_t dw, uint32_t dh)
{
	wr(d, DPP_H_RATIO, (uint32_t)(((uint64_t)sw << 20) / dw));
	wr(d, DPP_V_RATIO, (uint32_t)(((uint64_t)sh << 20) / dh));
	wr(d, DPP_YH_POS, (uint32_t)(int32_t)((((int64_t)sw - dw) << 20) / (2 * (int64_t)dw)));
	wr(d, DPP_YV_POS, (uint32_t)(int32_t)((((int64_t)sh - dh) << 20) / (2 * (int64_t)dh)));
	wr(d, DPP_CH_POS, (uint32_t)(int32_t)((((int64_t)sw - dw) << 20) / (2 * (int64_t)dw)));
	wr(d, DPP_CV_POS, (uint32_t)(int32_t)((((int64_t)sh - dh) << 20) / (2 * (int64_t)dh)));
}

static void
setup_scaler(vm_offset_t d, uint32_t sw, uint32_t sh, uint32_t dw, uint32_t dh)
{
	int16_t c[9][4];

	coefficients(c);
	setup_taps(d);
	setup_ratio(d, sw, sh, dw, dh);
	say("exynos_dpu: VGF0 scaler: %ux%u to %ux%u, ratio %x/%x, phases %d/%d, Catmull-Rom taps at half a pixel "
	    "%d %d %d %d\n", sw, sh, dw, dh, rd(d, DPP_H_RATIO), rd(d, DPP_V_RATIO), (int32_t)rd(d, DPP_YH_POS),
	    (int32_t)rd(d, DPP_YV_POS), c[8][0], c[8][1], c[8][2], c[8][3]);
}

/*
 * fb_planes: two of the DECON's windows (MiniIOMobileFramebuffer.cpp, planeShow()). CoreAnimation hands the display a
 * layer it detached from its picture (a CAMetalLayer's drawable) under the window server's surface, which is clear over
 * it. The window server's surface stays where fb_zc puts it, on the boot loader's window (5, the top one) and VGF0, now
 * blended by its premultiplied alpha: the DPP takes the alpha of every pixel and the window's function is the user-defined
 * one with Cr = Cf + (1 - Af) Cb, Ar = Af + (1 - Af) Ab. The detached layer, copied by the processor into a buffer of
 * the scan-out's at its place on the display, goes on a window below it (PLANE_WIN) through the other channel that has
 * a scaler, VGF1, enlarged to its rectangle on the panel and copied there as it is (function COPY). Both windows change
 * at the same shadow update, so no refresh shows one layer's new picture with the other's old one. hakios_scanout_show()
 * and hakios_scanout_flip() take the lower window away in the update they ask for.
 *
 * The first time, VGF1 is set up and tried unseen: the lower window is turned on under the top one while that is still
 * opaque (alpha per frame, 255), three refreshes are waited for and VGF1's error state is read, as attach_vgf0() does.
 * Only then is the top window made to blend. Any error, and the planes are never asked for again (-1: the caller blends
 * on the processor, as before).
 *
 * Registers (Samsung's dpu_9810, regs-decon.h, decon_reg.c: decon_reg_set_win_bnd_function, decon_reg_set_win_enable;
 * regs-dpp.h, dpp_reg.c: dpp_reg_set_alpha_sel, dma_reg_set_buf_offset): WIN_CONTROL_0 0x1000 + 0x30 w (alpha 1 at
 * bits 24-31, alpha 0 at 16-23, the function at 8-11: 1 COPY, 0xD user-defined; the alpha-mult source at 0-1, 2 the
 * foreground's alpha), WIN_CONTROL_1 0x1004 + 0x30 w (the coefficients of the FG colour, BG colour, FG alpha and BG
 * alpha at bits 24, 16, 8 and 0: 1 one, 3 one minus the foreground's alpha), WIN_START_TIME_CONTROL 0x1018 + 0x30 w;
 * DATA_PATH_CONTROL_0 bits 4w (window on) and 4w+1 (window filled with a colour); the blender's background colour
 * BLENDER_BG_IMAGE_COLOR_0/1 0x208/0x20C (alpha at 16, red at 0; green at 16, blue at 0); DPP_IN_CON bit 3 (the alpha
 * per pixel); the HDR block's DPP_VGRF_HDR_CON 0x600 (VGF1 has one: off); IDMA_SRC_OFFSET (y at 16, x at 0).
 */
#define PLANE_WIN               4
#define IDMA_VGF1               5
#define CHANNEL_VGF1            2
#define WIN_CONTROL_0(w)        (0x1000 + 0x30 * (w))
#define WIN_CONTROL_1(w)        (0x1004 + 0x30 * (w))
#define WIN_START_TIME(w)       (0x1018 + 0x30 * (w))
#define  WIN_OPAQUE_COPY        (0xffu << 24 | 0xffu << 16 | 0x1u << 8 | 2u)
#define  WIN_OPAQUE_USER        (0xffu << 24 | 0xffu << 16 | 0xdu << 8 | 2u)
#define  WIN_PREMULTIPLIED      (1u << 24 | 3u << 16 | 1u << 8 | 3u)
#define WIN_BITS(w)             (3u << (4 * (w)))
#define BG_COLOR_0              0x208
#define BG_COLOR_1              0x20C
#define DPP_ALPHA_PIXEL         (1u << 3)
#define DPP_HDR_CON             0x600

static int planes;                      /* 0 not set up yet, 1 set up and tried, -1 failed: not again */
static bool plane_on;                   /* the lower window is on, or will be at the update asked for */
static uint32_t saved_ctl0, saved_ctl1, saved_dpp_in, saved_bg0, saved_bg1;
static uint32_t plane_scaled[4];        /* the lower layer's size and its size on the panel, as VGF1's scaler has them */

/* (fb_planes) the lower window off at the next shadow update, which the caller asks for */
static void
plane_off(void)
{
	if (plane_on) {
		wr(decon, DATA_PATH_0, rd(decon, DATA_PATH_0) & ~WIN_BITS(PLANE_WIN));
		plane_on = false;
	}
}

/* VGF1 to read w x h at x, y of `lower` (rowbytes a row, `rows` rows), enlarged to px0, py0 - px1, py1 of the panel */
static void
plane_place(uint64_t lower, uint32_t rowbytes, uint32_t rows, uint32_t x, uint32_t y, uint32_t w, uint32_t h,
    uint32_t px0, uint32_t py0, uint32_t px1, uint32_t py1)
{
	vm_offset_t i = idma(IDMA_VGF1), d = dppc(IDMA_VGF1);
	uint32_t dw = px1 - px0, dh = py1 - py0;

	wr(i, IDMA_SRC_SIZE, rows << 16 | (rowbytes / 4));
	wr(i, IDMA_SRC_OFFSET, y << 16 | x);
	wr(i, IDMA_IMG_SIZE, h << 16 | w);
	wr(i, IDMA_BASE_Y, (uint32_t)lower);
	wr(i, IDMA_BASE_C, (uint32_t)lower);
	wr(d, DPP_IMG_SIZE, h << 16 | w);
	wr(d, DPP_SCALED_SIZE, dh << 16 | dw);
	if (plane_scaled[0] != w || plane_scaled[1] != h || plane_scaled[2] != dw || plane_scaled[3] != dh) {
		setup_ratio(d, w, h, dw, dh);
		plane_scaled[0] = w;
		plane_scaled[1] = h;
		plane_scaled[2] = dw;
		plane_scaled[3] = dh;
	}
	wr(decon, WIN_START(PLANE_WIN), py0 << 16 | px0);
	wr(decon, WIN_END(PLANE_WIN), (py1 - 1) << 16 | (px1 - 1));
	if (!plane_on) {
		wr(decon, DATA_PATH_0, (rd(decon, DATA_PATH_0) & ~WIN_BITS(PLANE_WIN)) | 1u << (4 * PLANE_WIN));
		plane_on = true;
	}
}

/* The first time: VGF1 set up, tried under the still opaque top window, and the top window made to blend. 0 if it works. */
static int
plane_setup(uint64_t lower, uint32_t rowbytes, uint32_t rows, uint32_t x, uint32_t y, uint32_t w, uint32_t h,
    uint32_t px0, uint32_t py0, uint32_t px1, uint32_t py1)
{
	vm_offset_t vi = idma(IDMA_VGF0), vd = dppc(IDMA_VGF0), i = idma(IDMA_VGF1), d = dppc(IDMA_VGF1);
	uint32_t path0 = rd(decon, DATA_PATH_0), path1 = rd(decon, DATA_PATH_1), frames, errors_i, errors_d, irq_i, irq_d;

	if (win <= PLANE_WIN || channel_type != IDMA_VGF0 || (path0 & WIN_BITS(PLANE_WIN)) || (rd(i, IDMA_ENABLE) & OP_BUSY)) {
		say("exynos_dpu: fb_planes: not set up (window %u on %s, window %u %s, VGF1 %s): the layers are blended on the "
		    "processor\n", win, type_name[channel_type], PLANE_WIN, (path0 & WIN_BITS(PLANE_WIN)) ? "on" : "off",
		    (rd(i, IDMA_ENABLE) & OP_BUSY) ? "busy" : "idle");
		return -1;
	}
	for (unsigned k = 0; k < WINDOWS; k++) {
		if (((path1 >> (4 * k)) & 7) == CHANNEL_VGF1 && (path0 & (1u << (4 * k)))) {
			say("exynos_dpu: fb_planes: VGF1 is window %u's already: not set up\n", k);
			return -1;
		}
	}
	dump_channel("fb_planes, before,", IDMA_VGF1);
	/* IDMA: VGF0's format and bus settings */
	wr(i, IDMA_IN_CON, rd(vi, IDMA_IN_CON) & IN_CON_KEEP);
	wr(i, IDMA_OUT_CON, rd(vi, IDMA_OUT_CON));
	wr(i, IDMA_DEADLOCK, rd(vi, IDMA_DEADLOCK));
	wr(i, IDMA_BUS_CON, rd(vi, IDMA_BUS_CON));
	wr(i, IDMA_GATING, rd(vi, IDMA_GATING));
	wr(i, IDMA_REQ_DEST, rd(vi, IDMA_REQ_DEST));
	/* DPP: ARGB, the alpha per frame (the window copies), no HDR; the scaler's taps, as VGF0's */
	wr(d, DPP_IN_CON, (rd(d, DPP_IN_CON) & ~DPP_IN_KEEP) | (rd(vd, DPP_IN_CON) & 7));
	wr(d, DPP_HDR_CON, 0);
	setup_taps(d);
	wr(d, DPP_GATING, rd(vd, DPP_GATING));
	memset(plane_scaled, 0, sizeof(plane_scaled));
	/* the window: VGF1, copied as it is, at the top window's start time */
	wr(decon, WIN_CONTROL_0(PLANE_WIN), WIN_OPAQUE_COPY);
	wr(decon, WIN_START_TIME(PLANE_WIN), rd(decon, WIN_START_TIME(win)));
	wr(decon, DATA_PATH_1, (path1 & ~(7u << (4 * PLANE_WIN))) | (uint32_t)CHANNEL_VGF1 << (4 * PLANE_WIN));
	plane_on = false;
	plane_place(lower, rowbytes, rows, x, y, w, h, px0, py0, px1, py1);
	wr(i, IDMA_ENABLE, rd(i, IDMA_ENABLE) | SFR_UPDATE_FORCE);
	wr(d, DPP_ENABLE, rd(d, DPP_ENABLE) | SFR_UPDATE_FORCE);
	__asm__ volatile ("dsb sy" ::: "memory");
	frames = rd(decon, FRAME_COUNT);
	psr_wake();
	wr(decon, SHADOW_REQ, rd(decon, SHADOW_REQ) | SHADOW_ALL);
	if (wait_taken(100) != 0) {
		say("exynos_dpu: fb_planes: the DECON did not take the lower window in 100 ms\n");
		goto off;
	}
	for (unsigned ms = 0; ms < 100 && rd(decon, FRAME_COUNT) - frames < 3; ms++) {
		psr_wake();
		IOSleep(1);
	}
	errors_i = rd(i, IDMA_CFG_ERR) & 0x7ff;
	errors_d = rd(d, DPP_CFG_ERR) & 0x1f;
	irq_i = rd(i, IDMA_IRQ) & IDMA_IRQ_ERRORS;
	irq_d = rd(d, DPP_IRQ) & DPP_IRQ_ERRORS;
	dump_channel("fb_planes, tried,", IDMA_VGF1);
	say("exynos_dpu: fb_planes: VGF1 after %u refreshes under the opaque window %u: IDMA configuration errors %x, DPP's %x, "
	    "IDMA error status %x, DPP's %x; window %u control %x/%x, start time %x, background %x/%x\n",
	    rd(decon, FRAME_COUNT) - frames, win, errors_i, errors_d, irq_i, irq_d, win, rd(decon, WIN_CONTROL_0(win)),
	    rd(decon, WIN_CONTROL_1(win)), rd(decon, WIN_START_TIME(win)), rd(decon, BG_COLOR_0), rd(decon, BG_COLOR_1));
	if (errors_i || errors_d || irq_i || irq_d) {
		goto off;
	}
	/* the top window blends by the alpha of its pixels, over a black background where no window is */
	saved_ctl0 = rd(decon, WIN_CONTROL_0(win));
	saved_ctl1 = rd(decon, WIN_CONTROL_1(win));
	saved_dpp_in = rd(vd, DPP_IN_CON);
	saved_bg0 = rd(decon, BG_COLOR_0);
	saved_bg1 = rd(decon, BG_COLOR_1);
	wr(decon, BG_COLOR_0, saved_bg0 & ~0x3ffu);
	wr(decon, BG_COLOR_1, 0);
	wr(decon, WIN_CONTROL_1(win), WIN_PREMULTIPLIED);
	wr(decon, WIN_CONTROL_0(win), WIN_OPAQUE_USER);
	wr(vd, DPP_IN_CON, saved_dpp_in | DPP_ALPHA_PIXEL);
	say("exynos_dpu: fb_planes: a detached layer goes on window %u through VGF1 under window %u, which blends by the alpha "
	    "of its pixels\n", PLANE_WIN, win);
	return 0;
off:
	plane_off();
	__asm__ volatile ("dsb sy" ::: "memory");
	wr(decon, SHADOW_REQ, rd(decon, SHADOW_REQ) | SHADOW_ALL);
	(void)wait_taken(100);
	say("exynos_dpu: fb_planes: VGF1 not usable: the layers are blended on the processor\n");
	return -1;
}

/* (fb_planes) the top window as the boot loader had it, the lower one off: at the update the caller asks for */
static void
plane_restore(void)
{
	plane_off();
	if (planes > 0) {
		wr(decon, WIN_CONTROL_0(win), saved_ctl0);
		wr(decon, WIN_CONTROL_1(win), saved_ctl1);
		wr(dppc(IDMA_VGF0), DPP_IN_CON, saved_dpp_in);
		wr(decon, BG_COLOR_0, saved_bg0);
		wr(decon, BG_COLOR_1, saved_bg1);
		planes = 0;
	}
}

/*
 * fb_planes: from the next refresh on, `upper` (a surface of the pool, the display's size, upper_rowbytes a row) on the
 * top window, and under it w x h at x, y of `lower` (a buffer of the scan-out's, the display's size, lower_rowbytes a
 * row) at its place, both enlarged to the panel. As for hakios_scanout_show(), the caller has waited for any earlier
 * shadow update to be taken. 0 if asked for; -1 if this one cannot be (its rectangle), -2 if none can (the windows could
 * not be set up): the caller blends on the processor.
 */
int
hakios_scanout_planes(uint64_t upper, uint32_t upper_rowbytes, uint64_t lower, uint32_t lower_rowbytes, uint32_t x,
    uint32_t y, uint32_t w, uint32_t h)
{
	vm_offset_t ui;
	uint32_t img, sw, sh, pw, ph, px0, py0, px1, py1, dw, dh;

	if (planes < 0) {
		return -2;
	}
	if (mode != 2 || channel_type != IDMA_VGF0) {
		return -1;
	}
	ui = idma(channel_type);
	img = rd(ui, IDMA_IMG_SIZE);
	sw = img & 0x3fff;
	sh = img >> 16 & 0x3fff;
	pw = (rd(decon, WIN_END(win)) & 0x3fff) + 1;
	ph = (rd(decon, WIN_END(win)) >> 16 & 0x3fff) + 1;
	if ((upper & 0xff) || (upper_rowbytes & 3) || !hakios_scanout_carved(upper) || upper_rowbytes / 4 < sw ||
	    upper_rowbytes / 4 > 0xffff || (lower & 0xff) || lower == 0 || (lower >> 32) || (lower_rowbytes & 3) ||
	    lower_rowbytes / 4 > 0xffff || sw == 0 || sh == 0 || (uint64_t)x + w > sw || (uint64_t)y + h > sh ||
	    lower_rowbytes / 4 < x + w) {
		return -1;
	}
	px0 = (uint32_t)((uint64_t)x * pw / sw);
	py0 = (uint32_t)((uint64_t)y * ph / sh);
	px1 = (uint32_t)(((uint64_t)x + w) * pw / sw);
	py1 = (uint32_t)(((uint64_t)y + h) * ph / sh);
	dw = px1 - px0;
	dh = py1 - py0;
	/* (dpp.h's limits: 16 pixels at least, scaled from half to eight times) */
	if (w < 16 || h < 16 || dw < 16 || dh < 16 || w > 2 * dw || h > 2 * dh || dw > 8 * w || dh > 8 * h) {
		return -1;
	}
	if (planes == 0) {
		planes = plane_setup(lower, lower_rowbytes, sh, x, y, w, h, px0, py0, px1, py1) == 0 ? 1 : -1;
		if (planes < 0) {
			return -2;
		}
	}
	psr_wake();
	if (upper_rowbytes != shown_rowbytes) {
		wr(ui, IDMA_SRC_SIZE, (rd(ui, IDMA_SRC_SIZE) & 0xffff0000u) | (upper_rowbytes / 4));
		shown_rowbytes = upper_rowbytes;
	}
	wr(ui, IDMA_BASE_Y, (uint32_t)upper);
	wr(ui, IDMA_BASE_C, (uint32_t)upper);
	plane_place(lower, lower_rowbytes, sh, x, y, w, h, px0, py0, px1, py1);
	__asm__ volatile ("dsb sy" ::: "memory");
	wr(decon, SHADOW_REQ, rd(decon, SHADOW_REQ) | SHADOW_ALL);
	return 0;
}

/* back to the boot loader's channel and buffer */
void
hakios_scanout_detach(const char *why)
{
	if (probed <= 0 || mode == 0) {
		return;
	}
	say("exynos_dpu: back to the boot loader's window (%s)\n", why);
	psr_wake();
	plane_restore();
	psr_ms = 0;
	if (mode == 2) {
		wr(decon, DATA_PATH_1, boot_data_path_1);
	}
	wr(idma(boot_type), IDMA_BASE_Y, boot_base);
	wr(idma(boot_type), IDMA_BASE_C, boot_base);
	__asm__ volatile ("dsb sy" ::: "memory");
	wr(decon, SHADOW_REQ, rd(decon, SHADOW_REQ) | SHADOW_ALL);
	if (wait_taken(100) != 0) {
		say("exynos_dpu: the DECON has not taken it after 100 ms (shadow request %x)\n", rd(decon, SHADOW_REQ));
	}
	mode = 0;
	channel_type = boot_type;
}

/*
 * VGF0 set up to read `phys` (width x height, rowbytes a row) and scale it to the boot window's size; then the window
 * is moved to it.
 */
static int
attach_vgf0(uint64_t phys, uint32_t width, uint32_t height, uint32_t rowbytes)
{
	vm_offset_t bi = idma(boot_type), bd = dppc(boot_type), i = idma(IDMA_VGF0), d = dppc(IDMA_VGF0);
	uint32_t path1 = boot_data_path_1, in_con = rd(bi, IDMA_IN_CON), dpp_in = rd(bd, DPP_IN_CON);
	uint32_t pw = rd(decon, WIN_END(win)) & 0x3fff, ph = rd(decon, WIN_END(win)) >> 16 & 0x3fff;
	uint32_t frames, errors_i, errors_d, irq_i, irq_d;

	pw += 1;
	ph += 1;
	for (unsigned w = 0; w < WINDOWS; w++) {
		if (((path1 >> (4 * w)) & 7) == CHANNEL_VGF0 && (rd(decon, DATA_PATH_0) & (1u << (4 * w)))) {
			say("exynos_dpu: VGF0 is window %u's already: not taken\n", w);
			return -1;
		}
	}
	if (boot_type == IDMA_VGF0 || (in_con & IN_CON_ODD) || (dpp_in & 7) != 0 || (rowbytes & 3) || rowbytes / 4 < width ||
	    width < 16 || height < 16 || pw > 4096 || ph > 4096 || width > 2 * pw || height > 2 * ph ||
	    ph > 8 * height || pw > 8 * width || phys >> 32 || (phys & 0xff)) {
		say("exynos_dpu: VGF0 cannot take it: in %x, DPP in %x, %ux%u (%u bytes a row) at %#llx to %ux%u\n", in_con,
		    dpp_in, width, height, rowbytes, phys, pw, ph);
		return -1;
	}
	if (rd(i, IDMA_ENABLE) & OP_BUSY) {
		say("exynos_dpu: VGF0's IDMA is busy: not taken\n");
		return -1;
	}
	dump_channel("before,", IDMA_VGF0);
	/* IDMA: the boot channel's format and bus settings; this buffer and its sizes */
	wr(i, IDMA_IN_CON, in_con & IN_CON_KEEP);
	wr(i, IDMA_OUT_CON, rd(bi, IDMA_OUT_CON));
	wr(i, IDMA_SRC_SIZE, height << 16 | (rowbytes / 4));
	wr(i, IDMA_SRC_OFFSET, 0);
	wr(i, IDMA_IMG_SIZE, height << 16 | width);
	wr(i, IDMA_BASE_Y, (uint32_t)phys);
	wr(i, IDMA_BASE_C, (uint32_t)phys);
	wr(i, IDMA_DEADLOCK, rd(bi, IDMA_DEADLOCK));
	wr(i, IDMA_BUS_CON, rd(bi, IDMA_BUS_CON));
	wr(i, IDMA_GATING, rd(bi, IDMA_GATING));
	wr(i, IDMA_REQ_DEST, rd(bi, IDMA_REQ_DEST));
	/* DPP: the boot channel's ARGB input format, the sizes, the scaler */
	wr(d, DPP_IN_CON, (rd(d, DPP_IN_CON) & ~DPP_IN_KEEP) | (dpp_in & DPP_IN_KEEP));
	wr(d, DPP_IMG_SIZE, height << 16 | width);
	wr(d, DPP_SCALED_SIZE, ph << 16 | pw);
	setup_scaler(d, width, height, pw, ph);
	wr(d, DPP_GATING, rd(bd, DPP_GATING));
	/* VGF0 is idle, so its registers are taken now rather than at a frame of its own */
	wr(i, IDMA_ENABLE, rd(i, IDMA_ENABLE) | SFR_UPDATE_FORCE);
	wr(d, DPP_ENABLE, rd(d, DPP_ENABLE) | SFR_UPDATE_FORCE);
	dump_channel("set up,", IDMA_VGF0);
	if (wait_taken(100) != 0) {
		say("exynos_dpu: a shadow update is still pending (%x): not taken\n", rd(decon, SHADOW_REQ));
		return -1;
	}
	/* move the window to VGF0 */
	frames = rd(decon, FRAME_COUNT);
	mode = 2;
	channel_type = IDMA_VGF0;
	wr(decon, DATA_PATH_1, (path1 & ~(7u << (4 * win))) | (uint32_t)CHANNEL_VGF0 << (4 * win));
	__asm__ volatile ("dsb sy" ::: "memory");
	wr(decon, SHADOW_REQ, rd(decon, SHADOW_REQ) | SHADOW_ALL);
	if (wait_taken(100) != 0) {
		hakios_scanout_detach("the DECON did not take the window's new channel in 100 ms");
		return -1;
	}
	/* wait for three refreshes on the new channel, then read VGF0's error state */
	for (unsigned ms = 0; ms < 100 && rd(decon, FRAME_COUNT) - frames < 3; ms++) {
		IOSleep(1);
	}
	errors_i = rd(i, IDMA_CFG_ERR) & 0x7ff;
	errors_d = rd(d, DPP_CFG_ERR) & 0x1f;
	irq_i = rd(i, IDMA_IRQ) & IDMA_IRQ_ERRORS;
	irq_d = rd(d, DPP_IRQ) & DPP_IRQ_ERRORS;
	dump_channel("after the switch,", IDMA_VGF0);
	say("exynos_dpu: VGF0 after %u refreshes: IDMA configuration errors %x, DPP's %x, IDMA error status %x, DPP's %x\n",
	    rd(decon, FRAME_COUNT) - frames, errors_i, errors_d, irq_i, irq_d);
	if (errors_i || errors_d || irq_i || irq_d) {
		hakios_scanout_detach("VGF0 reports errors");
		return -1;
	}
	return 0;
}

/*
 * The display driver hands over its buffers (physical addresses; `count` of them, `shown` the one already drawn) and
 * asks for mode 1 (flips on the boot loader's channel; buffers of the panel's size) or 2 (VGF0 scales). 0 if taken.
 */
int
hakios_scanout_attach(int want, const uint64_t *phys, unsigned count, uint32_t width, uint32_t height, uint32_t rowbytes,
    unsigned shown)
{
	say("exynos_dpu: attach: %s asked for, %u buffers of %ux%u (%u bytes a row), the first at %#llx, %u drawn\n",
	    want == 2 ? "VGF0's scaler (fb_dpp)" : want == 1 ? "flips (fb_flip)" : "nothing", count, width, height, rowbytes,
	    count ? phys[0] : 0, shown);
	if (probed <= 0 || mode != 0 || count < 2 || count > 4 || shown >= count) {
		say("exynos_dpu: attach: refused (probe %d, mode %d, %u buffers, buffer %u drawn)\n", probed, mode, count, shown);
		return -1;
	}
	for (unsigned k = 0; k < count; k++) {
		if (phys[k] >> 32 || (phys[k] & 0xff)) {
			say("exynos_dpu: buffer %u at %#llx: the IDMA takes 32-bit addresses (aligned): not taken\n", k, phys[k]);
			return -1;
		}
		buffers[k] = phys[k];
	}
	nbuffers = count;
	if (want == 1) {
		if (width != (rd(idma(boot_type), IDMA_IMG_SIZE) & 0x1fff) || rowbytes != (rd(idma(boot_type), IDMA_SRC_SIZE) &
		    0xffff) * 4) {
			say("exynos_dpu: flips need buffers like the boot one (%u wide, %u bytes a row; given %u, %u)\n",
			    rd(idma(boot_type), IDMA_IMG_SIZE) & 0x1fff, (rd(idma(boot_type), IDMA_SRC_SIZE) & 0xffff) * 4, width,
			    rowbytes);
			return -1;
		}
		mode = 1;
		channel_type = boot_type;
		say("exynos_dpu: flips on %s between %u buffers (%08llx ...), each taken at a refresh\n", type_name[boot_type],
		    count, phys[0]);
		if (buffers[shown] != boot_base) {
			hakios_scanout_flip(shown);
		}
		return 0;
	}
	if (want == 2 && attach_vgf0(phys[shown], width, height, rowbytes) == 0) {
		scan_rowbytes = shown_rowbytes = rowbytes;
		say("exynos_dpu: VGF0 scales %ux%u to the panel; flips between %u buffers\n", width, height, count);
		if (PE_parse_boot_argn("fb_psr", &psr_ms, sizeof(psr_ms)) && psr_ms >= 50 && psr_ms <= 5000) {
			psr_last_frame = mach_absolute_time();
			psr_call = thread_call_allocate(psr_tick, NULL);
			psr_tick(NULL, NULL);
			say("exynos_dpu: fb_psr: the panel left to itself after %u ms without a frame\n", psr_ms);
		} else {
			psr_ms = 0;
		}
		return 0;
	}
	say("exynos_dpu: attach: %s not taken: the boot loader's window and buffer stay\n",
	    want == 2 ? "VGF0's scaler" : "this mode");
	nbuffers = 0;
	return -1;
}
