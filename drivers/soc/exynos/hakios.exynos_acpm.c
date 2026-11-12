/*
 * The Exynos 9810's always-on power processor (APM), asked through its ACPM firmware: the S2MPS18 PMIC's registers
 * (over SPEEDY, which the APM owns) and the clocks' DVFS rates. For the Galaxy S9's drivers (the GPU's supply:
 * drivers/gpu/mali/hakios.mali.c; the touch screen's: hakios.s6sy761.c; ABOX's rate); sys/hakios.exynos_acpm.h says how to
 * call it.
 *
 * The boot loader has started the APM with its firmware; nothing is to be done first (Linux's driver maps the two
 * regions, reads the firmware's table and masks the interrupts it polls, nothing more). The AP and the APM share:
 *   the mailbox   0x14100000: INTGR0 0x08 rings the APM (bit 16 + channel), INTCR1 0x20 clears and INTSR1 0x28 shows
 *                 its answer pending for us (bit = channel); INTMR1 0x24 masks that interrupt
 *   the SRAM      0x02039000, 0x30000 bytes, of which the first three pages (below +0x3000) fault when touched: only
 *                 what lies above is mapped, and an offset below it in a table is refused. At +0x7F00 the firmware's
 *                 "initdata" -- +0x08 where the channel table is, +0x0C how many, +0x18 how many are the AP's, +0x60
 *                 two strings, its version and build time -- and every place in it an offset from the SRAM's start.
 *                 Every access 32 bits wide (the S9+ port: anything narrower is a bus error)
 *   a channel     0x48 bytes: +0x00 its id (the low byte; the rest is not ours), +0x08 its owner, +0x0C its type,
 *                 +0x10/+0x14/+0x18 where the index the APM reads at, the index we write at and the queue of our
 *                 requests are (the firmware's "rx": what it receives); +0x28/+0x2C/+0x30 the same for its answers (it
 *                 writes at +0x2C's, we read at +0x28's); +0x34 the queue's length, +0x38 its elements' size (16 or
 *                 more: four words are used), +0x44 whether the AP polls it. The two queues the wrong way round is
 *                 the one mistake the S9+ port warns of: the requests land in the answers' queue, it fills after four,
 *                 and the APM answers nothing more until the next restart
 * A request on a QUEUE (a length of 2 or more): any answer still in our queue first taken out of the way (all of them:
 * one request at a time, under the lock, so none of them is awaited -- left there, they fill the queue, and a full
 * answer queue stops the APM for the rest of the boot); four words written into the request queue at our index, the
 * index moved on, the doorbell rung; bits 21-16 of the first word a number of ours (1 to 63) that its answer carries
 * back, found between the APM's answer index and ours, copied out, and the queue emptied. 600 ms for an answer, three
 * sends (the PMIC is on a slow bus behind the APM; the S9+ port's tool waits 500 ms and has never needed more).
 * A request on a single-slot BUFFER (a length of 0 or 1, the firmware's type 2): the four words written at the request
 * queue's base, the doorbell rung, INTSR1's bit waited for (1 s), the answer read at the answer queue's base and the bit
 * cleared.
 *   the PMIC (channel 2)   word 0 = type << 8 | register (type: the S2MPS18's block, 0 top, 1 PMIC, 2 RTC -- what its
 *                          driver calls the i2c address), word 1 = function (0 read, 1 write, 2 update) | value << 8 |
 *                          mask << 16; the answer's word 1: the value [15:8], the status [31:24] (0 good, else SPEEDY
 *                          failed)
 *   DVFS (channel 0)       word 0 = the domain's index, word 1 = kHz, word 2 = 0 set, 1 get; a get's answer in word 1.
 *                          Channel 0 because that is where Samsung's kernel sends them: its acpm_dvfs client takes its
 *                          channel from the clock node's "acpm-ipc-channel", which the S9's device tree does not have,
 *                          and keeps 0. (The S9+ port's tool asks on 5, the acpm_dvfs node's -- the channel the APM
 *                          announces MIF changes on -- and finds its requests refused.) Not used for the GPU any more.
 * Sources: the phone's device tree (acpm_ipc, the mailbox, the SRAM, s2mps18mfd's acpm-ipc-channel 2) and Linux's
 * drivers/soc/samsung/acpm (acpm_ipc.c, acpm_mfd.c) and cal-if (acpm_dvfs.c, cal-if.c) of the S9's kernel; the mainline
 * Linux port to the S9+ (github.com/SheroAbi/galaxy-s9plus-mainline-linux: src/soc/s9p-acpm.c and extras/debug-tools'
 * s9p-acpm, what was measured on that phone: the faulting pages, 32-bit accesses, the queues' direction, the stale
 * answers, the timeout). All GPL: read for these facts, not one line copied.
 */
#include <pexpert/pexpert.h>
#include <kern/clock.h>
#include <kern/locks.h>
#include <libkern/libkern.h>
#include <mach/mach_time.h>
#include <mach/mach_types.h>
#include <sys/errno.h>
#include <sys/hakios.exynos_acpm.h>
#include <sys/hakios.exynos_soc.h>
#include <sys/hakios.xnu.h>
#include <kern/thread.h>
#include <stdarg.h>
#include <sys/systm.h>

#define MBOX_PA         EXYNOS_APM_MBOX_PA
#define MBOX_INTGR0     0x08
#define MBOX_INTCR1     0x20
#define MBOX_INTMR1     0x24
#define MBOX_INTSR1     0x28
#define SRAM_PA         EXYNOS_APM_SRAM_PA
#define SRAM_SIZE       EXYNOS_ACPM_SRAM_SIZE
#define SRAM_FIRST      EXYNOS_ACPM_SRAM_FIRST          /* below: pages that fault; not mapped, never read */
#define INITDATA        EXYNOS_ACPM_INITDATA

#define ID_CHANNELS     0x08
#define ID_NCHANNELS    0x0C
#define ID_AP_MAX       0x18
#define ID_BUILD        0x6C            /* three words more before build_info than the BSP's header has (0x007f0000 0x10 0x40), then "92dbdb1" */

#define CH_SIZE         0x48
#define CH_ID           0x00
#define CH_OWNER        0x08
#define CH_TYPE         0x0C
#define CH_IN_REAR      0x10    /* our requests: where the APM reads */
#define CH_IN_FRONT     0x14    /* ... where we write */
#define CH_IN_BASE      0x18
#define CH_OUT_REAR     0x28    /* its answers: where we read */
#define CH_OUT_FRONT    0x2C    /* ... where it writes */
#define CH_OUT_BASE     0x30
#define CH_QLEN         0x34
#define CH_ELEM         0x38
#define CH_AP_POLL      0x44

#define SEQ_SHIFT       16
#define SEQ_MASK        (0x3Fu << SEQ_SHIFT)
#define ROOM_US         20000   /* for a place in the request queue */
#define ANSWER_US       600000  /* for an answer, each send */
#define SENDS           3
#define BUFFER_US       1000000 /* for a buffer channel's answer */
#define POLL_US         50
#define CHANNELS_MOST   16

static vm_offset_t mbox, sram;          /* sram: SRAM_PA + SRAM_FIRST */
static struct channel {
	bool usable, buffer;
	uint32_t in_rear, in_front, in_base, out_rear, out_front, out_base, len, elem;
	uint32_t seq;
} channels[CHANNELS_MOST];
/*
 * One lock for everything here: a request, and the first call's setup, which the touch screen's, ABOX's and Mali's threads
 * may make at the same moment (each calls hakios_acpm_init() from its own thread): the second waits for the first's
 * answer instead of taking an unfinished one (acpm_tried set, acpm_ready not yet) for "no ACPM".
 */
static LCK_GRP_DECLARE(acpm_lock_group, "hakios-acpm");
static LCK_MTX_DECLARE(acpm_lock, &acpm_lock_group);
static bool acpm_ready, acpm_tried;

/* a moment `us` microseconds from now, by the clock: a loop bounded by it ends on time, however long delay() takes */
static uint64_t
deadline_us(uint64_t us)
{
	uint64_t interval;

	nanoseconds_to_absolutetime(us * NSEC_PER_USEC, &interval);
	return mach_absolute_time() + interval;
}

static inline uint32_t
rd(vm_offset_t base, uint32_t off)
{
	return *(volatile uint32_t *)(base + off);
}

static inline void
wr(vm_offset_t base, uint32_t off, uint32_t v)
{
	*(volatile uint32_t *)(base + off) = v;
}

/* an offset into the SRAM that may be touched: above the faulting pages, inside, 32-bit aligned */
static inline bool
sram_ok(uint32_t off, uint32_t bytes)
{
	return off >= SRAM_FIRST && off <= SRAM_SIZE && bytes <= SRAM_SIZE - off && (off & 3) == 0;
}

static inline uint32_t
srd(uint32_t off)
{
	return rd(sram, off - SRAM_FIRST);
}

static inline void
swr(uint32_t off, uint32_t v)
{
	wr(sram, off - SRAM_FIRST, v);
}

static bool
printable(uint32_t off, uint32_t len)
{
	for (uint32_t i = 0; i < len; i++) {
		uint8_t c = (uint8_t)(srd((off + i) & ~3u) >> (8 * ((off + i) & 3)));

		if (c == 0) {
			return i > 0;
		}
		if (c < 0x20 || c > 0x7e) {
			return false;
		}
	}
	return true;
}

static void
copy_string(uint32_t off, char *out, uint32_t len)
{
	uint32_t i;

	for (i = 0; i + 1 < len; i++) {
		char c = (char)(srd((off + i) & ~3u) >> (8 * ((off + i) & 3)));

		if (c == 0) {
			break;
		}
		out[i] = c;
	}
	out[i] = 0;
}

/* every answer in a queue channel's answer queue taken out (none is awaited), and its interrupt cleared */
static void
drain(struct channel *c, uint32_t id)
{
	uint32_t front = srd(c->out_front);

	if (front < c->len && srd(c->out_rear) != front) {
		swr(c->out_rear, front);
	}
	wr(mbox, MBOX_INTCR1, 1u << id);
}

static bool
acpm_setup(int (*say)(const char *, ...))
{
	uint32_t table, count, ap_max;
	char version[26], built[26];

	mbox = ml_io_map(MBOX_PA, 0x1000);
	sram = ml_io_map(SRAM_PA + SRAM_FIRST, SRAM_SIZE - SRAM_FIRST);
	if (mbox == 0 || sram == 0) {
		say("acpm: cannot map the mailbox (%#lx) or the APM's SRAM (%#lx)\n", MBOX_PA, SRAM_PA + SRAM_FIRST);
		return false;
	}
	if (!printable(INITDATA + ID_BUILD, 25)) {
		say("acpm: no firmware's table at SRAM +%#x (its version string is not text): not used\n", INITDATA);
		for (uint32_t o = 0; o < 0x80; o += 0x20) {
			say("acpm: SRAM +%#x: %08x %08x %08x %08x %08x %08x %08x %08x\n", INITDATA + o, srd(INITDATA + o),
			    srd(INITDATA + o + 4), srd(INITDATA + o + 8), srd(INITDATA + o + 12), srd(INITDATA + o + 16),
			    srd(INITDATA + o + 20), srd(INITDATA + o + 24), srd(INITDATA + o + 28));
		}
		return false;
	}
	copy_string(INITDATA + ID_BUILD, version, sizeof(version));
	copy_string(INITDATA + ID_BUILD + 25, built, sizeof(built));
	table = srd(INITDATA + ID_CHANNELS);
	count = srd(INITDATA + ID_NCHANNELS);
	ap_max = srd(INITDATA + ID_AP_MAX);
	say("acpm: firmware \"%s\" built \"%s\"; %u channels at SRAM +%#x, %u the AP's; mailbox INTMR1 %#x, INTSR1 %#x\n",
	    version, built, count, table, ap_max, rd(mbox, MBOX_INTMR1), rd(mbox, MBOX_INTSR1));
	if (ap_max > count || count > 64 || !sram_ok(table, count * CH_SIZE)) {
		say("acpm: the table is not where it may be read (above SRAM +%#x, inside): not used\n", SRAM_FIRST);
		return false;
	}
	for (uint32_t i = 0; i < ap_max; i++) {
		uint32_t at = table + i * CH_SIZE, id = srd(at + CH_ID) & 0xff;
		struct channel *c;
		struct channel n = { 0 };

		n.in_rear = srd(at + CH_IN_REAR);
		n.in_front = srd(at + CH_IN_FRONT);
		n.in_base = srd(at + CH_IN_BASE);
		n.out_rear = srd(at + CH_OUT_REAR);
		n.out_front = srd(at + CH_OUT_FRONT);
		n.out_base = srd(at + CH_OUT_BASE);
		n.len = srd(at + CH_QLEN);
		n.elem = srd(at + CH_ELEM);
		n.buffer = n.len <= 1;
		say("acpm: channel %u: owner %u, type %u, %s of %u x %u bytes, %s; requests at +%#x, answers at +%#x\n", id,
		    srd(at + CH_OWNER), srd(at + CH_TYPE), n.buffer ? "a single-slot buffer" : "a queue", n.len, n.elem,
		    srd(at + CH_AP_POLL) ? "polled" : "by interrupt", n.in_base, n.out_base);
		if (id >= CHANNELS_MOST) {
			continue;
		}
		if (n.buffer) {
			n.usable = sram_ok(n.in_base, 16) && sram_ok(n.out_base, 16);
		} else {
			n.usable = n.len <= 64 && n.elem >= 16 && n.elem <= 256 && sram_ok(n.in_rear, 4) &&
			    sram_ok(n.in_front, 4) && sram_ok(n.out_rear, 4) && sram_ok(n.out_front, 4) &&
			    sram_ok(n.in_base, n.len * n.elem) && sram_ok(n.out_base, n.len * n.elem);
		}
		if (!n.usable) {
			say("acpm: channel %u: a place in it is outside what may be read: not used\n", id);
			continue;
		}
		c = &channels[id];
		*c = n;
		if (!c->buffer) {
			drain(c, id);   /* answers nobody read (a kernel before this one, or none): out of the way */
		}
	}
	say("acpm: the PMIC's channel %s, DVFS's %s\n", channels[HAKIOS_ACPM_CH_PMIC].usable ? "usable" : "NOT usable",
	    channels[HAKIOS_ACPM_CH_DVFS].usable ? "usable" : "NOT usable");
	return channels[HAKIOS_ACPM_CH_PMIC].usable || channels[HAKIOS_ACPM_CH_DVFS].usable;
}

bool
hakios_acpm_init(int (*say)(const char *, ...))
{
	bool ready;

	if (!HAKIOS_SOC_HAS(APM_MBOX_PA)) {
		return false;
	}
	lck_mtx_lock(&acpm_lock);
	if (!acpm_tried) {
		acpm_tried = true;
		acpm_ready = acpm_setup(say);
	}
	ready = acpm_ready;
	lck_mtx_unlock(&acpm_lock);
	return ready;
}

/* the answer with our number, if it has come: copied out, and the answer queue emptied */
static bool
take_answer(struct channel *c, uint32_t id, uint32_t seq, uint32_t cmd[4])
{
	uint32_t rear = srd(c->out_rear), front = srd(c->out_front);
	bool found = false;

	if (rear >= c->len || front >= c->len || (rear == front && !(rd(mbox, MBOX_INTSR1) & (1u << id)))) {
		return false;
	}
	for (uint32_t i = rear; i != front; i = (i + 1) % c->len) {
		uint32_t slot = c->out_base + i * c->elem;

		if (((srd(slot) & SEQ_MASK) >> SEQ_SHIFT) == seq) {
			for (uint32_t w = 0; w < 4; w++) {
				cmd[w] = srd(slot + 4 * w);
			}
			found = true;
		}
	}
	drain(c, id);
	return found;
}

static int
request_queue(struct channel *c, uint32_t id, uint32_t cmd[4])
{
	uint32_t sent[4], front, next;
	uint64_t deadline;

	for (uint32_t w = 0; w < 4; w++) {
		sent[w] = cmd[w];
	}
	for (int send = 0; send < SENDS; send++) {
		drain(c, id);                   /* never into a queue with stale answers in it */
		front = srd(c->in_front);
		if (front >= c->len) {
			return EIO;
		}
		next = (front + 1) % c->len;
		for (deadline = deadline_us(ROOM_US); srd(c->in_rear) == next && mach_absolute_time() < deadline;) {
			delay(POLL_US);         /* the queue is full: the APM has not read our last requests yet */
		}
		if (srd(c->in_rear) == next) {
			continue;
		}
		c->seq = c->seq % 63 + 1;
		sent[0] = (sent[0] & ~SEQ_MASK) | c->seq << SEQ_SHIFT;
		for (uint32_t w = 0; w < 4; w++) {
			swr(c->in_base + front * c->elem + 4 * w, sent[w]);
		}
		__builtin_arm_dsb(0xf);         /* DSB SY: the words before the index, the index before the doorbell */
		swr(c->in_front, next);
		__builtin_arm_dsb(0xf);
		wr(mbox, MBOX_INTGR0, 1u << (16 + id));
		for (deadline = deadline_us(ANSWER_US); mach_absolute_time() < deadline;) {
			if (take_answer(c, id, c->seq, cmd)) {
				return 0;
			}
			delay(POLL_US);
		}
		if (take_answer(c, id, c->seq, cmd)) {
			return 0;
		}
	}
	return ETIMEDOUT;
}

static int
request_buffer(struct channel *c, uint32_t id, uint32_t cmd[4])
{
	uint64_t deadline;

	c->seq = c->seq % 63 + 1;
	cmd[0] = (cmd[0] & ~SEQ_MASK) | c->seq << SEQ_SHIFT;
	wr(mbox, MBOX_INTCR1, 1u << id);
	for (uint32_t w = 0; w < 4; w++) {
		swr(c->in_base + 4 * w, cmd[w]);
	}
	__builtin_arm_dsb(0xf);
	wr(mbox, MBOX_INTGR0, 1u << (16 + id));
	for (deadline = deadline_us(BUFFER_US); !(rd(mbox, MBOX_INTSR1) & (1u << id));) {
		if (mach_absolute_time() >= deadline) {
			return ETIMEDOUT;
		}
		delay(POLL_US);
	}
	for (uint32_t w = 0; w < 4; w++) {
		cmd[w] = srd(c->out_base + 4 * w);
	}
	wr(mbox, MBOX_INTCR1, 1u << id);
	return 0;
}

int
hakios_acpm_request(uint32_t id, uint32_t cmd[4])
{
	int error;

	if (!acpm_ready || id >= CHANNELS_MOST || !channels[id].usable) {
		return ENXIO;
	}
	lck_mtx_lock(&acpm_lock);
	error = channels[id].buffer ? request_buffer(&channels[id], id, cmd) : request_queue(&channels[id], id, cmd);
	lck_mtx_unlock(&acpm_lock);
	return error;
}

int
hakios_acpm_pmic_read(uint32_t type, uint32_t reg, uint8_t *value)
{
	uint32_t cmd[4] = { (type & 0xf) << 8 | (reg & 0xff), 0, 0, 0 };
	int error = hakios_acpm_request(HAKIOS_ACPM_CH_PMIC, cmd);

	if (error == 0 && (cmd[1] >> 24) != 0) {
		error = EIO;
	}
	if (error == 0) {
		*value = (uint8_t)(cmd[1] >> 8);
	}
	return error;
}

int
hakios_acpm_pmic_write(uint32_t type, uint32_t reg, uint8_t value)
{
	uint32_t cmd[4] = { (type & 0xf) << 8 | (reg & 0xff), 1u | (uint32_t)value << 8, 0, 0 };
	int error = hakios_acpm_request(HAKIOS_ACPM_CH_PMIC, cmd);

	return error == 0 && (cmd[1] >> 24) != 0 ? EIO : error;
}

int
hakios_acpm_pmic_update(uint32_t type, uint32_t reg, uint8_t value, uint8_t mask)
{
	uint32_t cmd[4] = { (type & 0xf) << 8 | (reg & 0xff), 2u | (uint32_t)value << 8 | (uint32_t)mask << 16, 0, 0 };
	int error = hakios_acpm_request(HAKIOS_ACPM_CH_PMIC, cmd);

	return error == 0 && (cmd[1] >> 24) != 0 ? EIO : error;
}

int
hakios_acpm_dvfs_get(uint32_t index, uint32_t *khz)
{
	uint32_t cmd[4] = { index, 0, 1, 0 };
	int error = hakios_acpm_request(HAKIOS_ACPM_CH_DVFS, cmd);

	if (error == 0) {
		*khz = cmd[1];
	}
	return error;
}

int
hakios_acpm_dvfs_set(uint32_t index, uint32_t khz)
{
	uint32_t cmd[4] = { index, khz, 0, 0 };

	return hakios_acpm_request(HAKIOS_ACPM_CH_DVFS, cmd);
}

/*
 * A DVFS domain's levels as the APM holds them, read only: its rate and voltage per level, the fastest first. Samsung's
 * kernel reads the same "fvmap" once at start (cal-if/fvmap.c fvmap_copy_from_sram, from acpm/acpm.c plugins_init) and
 * only reads it: the voltages in it are the ones the APM itself applies with each rate (ASV already applied by the boot;
 * Linux adds a margin to them only when its command line asks). A DVFS request names a rate, never a voltage.
 *   initdata +0x00   where the APM's plugins are (an offset into the SRAM), +0x04 how many (acpm_framework, framework.h)
 *   a plugin         0x30 bytes (struct plugin, fw_header/common.h, as the 32-bit APM lays it out): +0x04 its base (bit 0
 *                    a flag), +0x20 attached (a byte), +0x24 its size, +0x28 stays attached (a byte), +0x2C its name
 *   the DVFS plugin  the one whose name has "dvfs" in it; its map at base + the acpm node's fvmap_offset (0x6700 in the
 *                    phone's device tree) when the boot left it attached, at base + size when Linux loads it itself
 *   the map          a 24-byte header per domain, in the order of the DVFS indexes (struct fvmap_header, fvmap.h): +0x01
 *                    how many levels, +0x14 (16 bits) where they are from the map's start; a level is two words, the
 *                    rate (kHz) and the voltage (uV)
 * Returns how many levels it read into khz[] and uv[] (at most `most`), 0 when the map is not found or does not look
 * like one (each step said with `say`).
 */
#define ID_PLUGINS      0x00
#define ID_NPLUGINS     0x04
#define PL_SIZE         0x30
#define PL_BASE         0x04
#define PL_ATTACHED     0x20
#define PL_FWSIZE       0x24
#define PL_STAYS        0x28
#define PL_NAME         0x2C
#define FVMAP_OFFSET    EXYNOS_ACPM_FVMAP_OFFSET          /* the device tree's acpm { fvmap_offset = <26368>; } */
#define FV_HEADER       24
#define FV_NUM_LV       0x01
#define FV_O_RATEVOLT   0x14

static uint32_t
srd8(uint32_t off)
{
	return (srd(off & ~3u) >> (8 * (off & 3))) & 0xff;
}

static uint32_t
srd16(uint32_t off)
{
	return (srd(off & ~3u) >> (8 * (off & 2))) & 0xffff;
}

static bool
names_dvfs(const char *name)
{
	for (const char *p = name; p[0] && p[1] && p[2] && p[3]; p++) {
		if ((p[0] | 0x20) == 'd' && (p[1] | 0x20) == 'v' && (p[2] | 0x20) == 'f' && (p[3] | 0x20) == 's') {
			return true;
		}
	}
	return false;
}

/* where the DVFS plugin's map is in the SRAM; 0 when it is not found (said with `say`) */
static uint32_t
fvmap_at(int (*say)(const char *, ...))
{
	uint32_t plugins, count, map = 0;
	char name[24];

	plugins = srd(INITDATA + ID_PLUGINS);
	count = srd(INITDATA + ID_NPLUGINS);
	if (count == 0 || count > 32 || !sram_ok(plugins, count * PL_SIZE)) {
		say("acpm: fvmap: the plugins' table (+%#x, %u of them) is not where it may be read\n", plugins, count);
		return 0;
	}
	for (uint32_t i = 0; i < count && map == 0; i++) {
		uint32_t p = plugins + i * PL_SIZE, at_name = srd(p + PL_NAME), base = srd(p + PL_BASE) & ~1u;
		uint32_t attached = srd8(p + PL_ATTACHED), stays = srd8(p + PL_STAYS), size = srd(p + PL_FWSIZE);

		if (at_name == 0 || !sram_ok(at_name & ~3u, sizeof(name) + 4) || !printable(at_name, sizeof(name) - 1)) {
			continue;
		}
		copy_string(at_name, name, sizeof(name));
		if (!names_dvfs(name)) {
			continue;
		}
		map = base + (attached ? FVMAP_OFFSET : size);
		say("acpm: fvmap: plugin %u \"%s\": base +%#x, attached %u, stays %u, size %#x: the map at +%#x\n", i, name,
		    base, attached, stays, size, map);
	}
	if (map == 0) {
		say("acpm: fvmap: no plugin named for DVFS among %u\n", count);
	}
	return map;
}

uint32_t
hakios_acpm_dvfs_table(uint32_t index, uint32_t *khz, uint32_t *uv, uint32_t most, int (*say)(const char *, ...))
{
	uint32_t map, header, levels, at;

	if (!acpm_ready || (map = fvmap_at(say)) == 0) {
		return 0;
	}
	header = map + index * FV_HEADER;
	if (!sram_ok(header & ~3u, FV_HEADER + 4)) {
		say("acpm: fvmap: domain %u's header (+%#x) is not where it may be read\n", index, header);
		return 0;
	}
	levels = srd8(header + FV_NUM_LV);
	at = map + srd16(header + FV_O_RATEVOLT);
	if (levels == 0 || levels > most || !sram_ok(at, levels * 8)) {
		say("acpm: fvmap: domain %u: %u levels at +%#x: not a table (at most %u, inside the SRAM)\n", index, levels, at,
		    most);
		return 0;
	}
	for (uint32_t l = 0; l < levels; l++) {
		khz[l] = srd(at + 8 * l);
		uv[l] = srd(at + 8 * l + 4);
		if (khz[l] < 100000 || khz[l] > 4000000 || uv[l] < 300000 || uv[l] > 1500000 || (l > 0 && khz[l] >= khz[l - 1])) {
			say("acpm: fvmap: domain %u: level %u reads %u kHz %u uV: not a table (the fastest first, kHz and uV)\n",
			    index, l, khz[l], uv[l]);
			return 0;
		}
	}
	return levels;
}

/*
 * The APM asked to stop, as Samsung's kernel does before every power-off and restart (exynos_acpm_reboot() ->
 * acpm_enter_wfi(), drivers/soc/samsung/acpm/acpm.c:250-292): one request, word 0 the protocol's STOP bit (bit 22,
 * ACPM_IPC_PROTOCOL_STOP in include/soc/samsung/acpm_ipc_ctrl.h), on the channel of the acpm node itself (4 in the S9's
 * device tree; acpm_dvfs's is 5, s2mps18's 2). Samsung skips it on the 9810's first revision (EVT0) only. Its answer is
 * waited for as any other's; after it the APM answers nothing more, so nothing may come after it but PS_HOLD or a restart.
 */
#define ACPM_STOP       (1u << 22)

int
hakios_acpm_stop(void)
{
	uint32_t cmd[4] = { ACPM_STOP, 0, 0, 0 };

	return hakios_acpm_request(HAKIOS_ACPM_CH_ACPM, cmd);
}

/*
 * The other DVFS domains: the memory bus (MIF), the internal bus (INT, which also clocks the video codec's CLKCMU_MFC_BUS),
 * the display (DISP) and the rest. Their indexes are Samsung's (Apollo drivers/soc/samsung/cal-if/exynos9810/
 * acpm_dvfs_exynos9810.h:2-16: mif 0, int 1, cpucl0 2, cpucl1 3, g3d 4, intcam 5, fsys0 6, cam 7, disp 8, aud 9, iva 10,
 * score 11, cp 12; the devfreq nodes' dfs_id 0xb04000N carry the same N). Samsung's devfreq sets each with
 * cal_dfs_set_rate(dfs_id, kHz) -> exynos_acpm_set_rate(N, kHz) (exynos9810_bus_mif.c:216-224, cal-if.c:49-66), the
 * request hakios_acpm_dvfs_set() sends; the APM applies the level's voltage itself.
 * Samsung's limits, star2lte_kor_26.dts devfreq_* freq_info = <initial default_qos suspend min MAX reboot> (exynos-devfreq.c:
 * 959-970) and boot_info = <seconds kHz>: MIF max 2093000, boot 1794000 for 40 s (:9489-9490); INT max 667000, boot 667000
 * (:9511-9512); DISP max 640000, boot 640000 (:9553-9554). A rate asked here is never above Samsung's max nor the APM's top.
 *
 * Boot-args:
 *   bus_log=0   do not log the APM's tables at boot (default: one line per domain, read only)
 *   bus_max=1   MIF, INT and DISP to their top at boot, kept there (re-asked every 10 s, cheap)
 *   bus_max=2   MIF, INT and DISP at their top while frames are shown or video decoded (hakios_exynos_bus_busy()), back
 *               to Samsung's boot rates (1794000 / 667000 / 640000) after 5 s of neither
 *   bus_mask=N  which of the pinned domains bus_max moves, a bit per DVFS index (default 0x1a3: MIF 0, INT 1, INTCAM 5,
 *               CAM 7, DISP 8). S-Boot leaves DISP at 400 MHz, INTCAM and CAM at their slowest (630 / 590 MHz) and MIF
 *               at 1794 MHz; Samsung's devfreq starts them at initial_freq (DISP 640000, INTCAM and CAM 690000 --
 *               star2lte_kor_26.dts devfreq_intcam/_cam freq_info). bus_max=1 with all of them hung the S9+ at once
 *               (build 17, a purple screen, nothing logged): bus_mask=0x1a2 leaves MIF where S-Boot put it.
 */
#define BUS_DOMAINS_MOST        13
#define BUS_LEVELS_MOST         32
#define BUS_POLL_US             250000u
#define BUS_HOLD_MS             1000u   /* bus_max=2: busy if a frame or a decode came within this */
#define BUS_IDLE_MS             5000u   /* ... and back to the boot rates after this much without */
#define BUS_REASK_MS            10000u  /* bus_max=1: asked again this often (the APM may have been asked otherwise) */
#define CMU_MIF_PA              0x1B800000u     /* Apollo cal-if exynos9810 cmucal-sfr.c: CMU_MIF; PLL_CON0_PLL_MIF +0x120 */
#define CMU_MIF_PLL_CON0        0x120

static const char *const bus_names[BUS_DOMAINS_MOST] = {
	"MIF", "INT", "CPUCL0", "CPUCL1", "G3D", "INTCAM", "FSYS0", "CAM", "DISP", "AUD", "IVA", "SCORE", "CP",
};
static const struct bus_pin { uint32_t index, samsung_max, samsung_boot; } bus_pins[] = {
	{ 0, 2093000, 1794000 },        /* MIF  star2lte_kor_26.dts:9489-9490 */
	{ 1, 667000, 667000 },          /* INT  :9511-9512 */
	{ 8, 640000, 640000 },          /* DISP :9553-9554 */
	{ 5, 690000, 640000 },          /* INTCAM devfreq_intcam: freq_info initial/max 690000, boot_info 640000 */
	{ 7, 690000, 630000 },          /* CAM devfreq_cam: initial/max 690000, boot_info 630000 */
};
#define BUS_PINS        (sizeof(bus_pins) / sizeof(bus_pins[0]))
static uint32_t bus_top[BUS_DOMAINS_MOST];      /* the APM's fastest level of each domain, kHz (0: not read) */
static uint32_t bus_max, bus_log = 1, bus_level, bus_mask = 0x1a3;  /* bus_level: 0 not set, 1 the boot rates, 2 the top */
static uint64_t bus_busy_at;

static void
bus_say(const char *format, ...)
{
	char line[480];
	va_list ap;

	va_start(ap, format);
	vsnprintf(line, sizeof(line), format, ap);
	va_end(ap);
	printf("%s", line);
	loader_log_line(line);
}

/* a frame shown (hakios.mali.c hakios_mali_frame) or a picture decoded (hakios.exynos_mfc.c): any context, a stamp only */
void hakios_exynos_bus_busy(void);
void
hakios_exynos_bus_busy(void)
{
	__atomic_store_n(&bus_busy_at, mach_absolute_time(), __ATOMIC_RELAXED);
}

/* PLL_MIF as the CMU has it, MHz (the DRAM's bus at about half); 0 when it cannot be read */
static uint32_t
bus_mif_pll_mhz(void)
{
	static vm_offset_t cmu_mif;
	uint32_t con0, m, p, s;

	if (cmu_mif == 0 && (cmu_mif = ml_io_map(CMU_MIF_PA, 0x1000)) == 0) {
		return 0;
	}
	con0 = rd(cmu_mif, CMU_MIF_PLL_CON0);
	m = (con0 >> 16) & 0x3ff;
	p = (con0 >> 8) & 0x3f;
	s = con0 & 7;
	return p != 0 && (con0 & (1u << 31)) ? (uint32_t)(26ull * m / p >> s) : 0;
}

/* every domain's levels, one line each, read only; bus_top[] filled */
static void
bus_report(void)
{
	uint32_t map = fvmap_at(printf);

	if (map == 0) {
		return;
	}
	for (uint32_t d = 0; d < BUS_DOMAINS_MOST; d++) {
		uint32_t header = map + d * FV_HEADER, levels, init_lv, at, n = 0;
		char line[400];

		if (!sram_ok(header & ~3u, FV_HEADER + 4)) {
			break;
		}
		levels = srd8(header + FV_NUM_LV);
		init_lv = srd8(header + 0x08);  /* fvmap.h struct fvmap_header: init_lv, the level the boot left */
		at = map + srd16(header + 0x14);
		if (levels == 0 || levels > BUS_LEVELS_MOST || !sram_ok(at, levels * 8)) {
			bus_say("bus: DVFS %u (%s): %u levels at +%#x: not a table\n", d, bus_names[d], levels, at);
			continue;
		}
		n = (uint32_t)snprintf(line, sizeof(line), "bus: DVFS %u (%s): %u levels, the boot's level %u; kHz/uV:", d,
		    bus_names[d], levels, init_lv);
		for (uint32_t l = 0; l < levels && n < sizeof(line) - 24; l++) {
			uint32_t khz = srd(at + 8 * l), uv = srd(at + 8 * l + 4);

			if (l == 0 && khz >= 1000 && khz <= 4000000) {
				bus_top[d] = khz;
			}
			n += (uint32_t)snprintf(line + n, sizeof(line) - n, " %u/%u", khz, uv);
		}
		if (n < sizeof(line) - 2) {
			line[n] = '\n';
			line[n + 1] = 0;
		}
		if (bus_log) {
			bus_say("%s", line);
		}
	}
}

/* the pinned domains to their top (top true) or to Samsung's boot rates */
static void
bus_set(bool top, bool quiet)
{
	uint32_t before = bus_mif_pll_mhz();

	for (uint32_t i = 0; i < BUS_PINS; i++) {
		const struct bus_pin *b = &bus_pins[i];
		uint32_t want = b->samsung_boot;
		int e;

		if ((bus_mask & (1u << b->index)) == 0) {
			continue;
		}
		if (top) {
			want = bus_top[b->index] != 0 && bus_top[b->index] < b->samsung_max ? bus_top[b->index] : b->samsung_max;
		}
		e = hakios_acpm_dvfs_set(b->index, want);
		if (!quiet || e != 0) {
			bus_say("bus: %s (DVFS %u) asked for %u kHz (%s; the APM's top %u, Samsung's max %u): %d\n",
			    bus_names[b->index], b->index, want, top ? "the top" : "Samsung's boot rate", bus_top[b->index],
			    b->samsung_max, e);
		}
	}
	if (!quiet) {
		bus_say("bus: PLL_MIF %u MHz before, %u MHz after (the memory bus at about half)\n", before, bus_mif_pll_mhz());
	}
	bus_level = top ? 2 : 1;
}

static void
bus_thread(__unused void *arg, __unused wait_result_t wr)
{
	uint64_t hold, idle, reask, last_ask = 0;

	for (int tries = 0; !hakios_acpm_init(printf); tries++) {
		if (tries >= 40) {
			bus_say("bus: no ACPM: the DVFS domains are left as they are\n");
			return;
		}
		delay(BUS_POLL_US);
	}
	bus_report();
	if (bus_max == 0) {
		return;
	}
	nanoseconds_to_absolutetime((uint64_t)BUS_HOLD_MS * NSEC_PER_MSEC, &hold);
	nanoseconds_to_absolutetime((uint64_t)BUS_IDLE_MS * NSEC_PER_MSEC, &idle);
	nanoseconds_to_absolutetime((uint64_t)BUS_REASK_MS * NSEC_PER_MSEC, &reask);
	bus_say("bus: bus_max=%u, bus_mask %#x: the pinned domains (MIF, INT, INTCAM, CAM, DISP) %s\n", bus_max, bus_mask, bus_max == 1 ? "at their top from now on" :
	    "at their top while frames are shown or video decoded, Samsung's boot rates after 5 s of neither");
	for (;;) {
		uint64_t now = mach_absolute_time(), busy = __atomic_load_n(&bus_busy_at, __ATOMIC_RELAXED);

		if (bus_max == 1) {
			if (bus_level == 0 || now - last_ask >= reask) {
				bus_set(true, bus_level != 0);
				last_ask = now;
			}
		} else if (busy != 0 && now - busy < hold) {
			if (bus_level != 2) {
				bus_set(true, bus_level != 0);
			}
		} else if (bus_level == 2 && (busy == 0 || now - busy >= idle)) {
			bus_set(false, true);
		}
		delay(BUS_POLL_US);
	}
}

static void
hakios_exynos_bus_start(void)
{
	thread_t thread;

	if (!HAKIOS_SOC_HAS(APM_MBOX_PA)) {
		return;
	}
	(void)PE_parse_boot_argn("bus_max", &bus_max, sizeof(bus_max));
	(void)PE_parse_boot_argn("bus_log", &bus_log, sizeof(bus_log));
	(void)PE_parse_boot_argn("bus_mask", &bus_mask, sizeof(bus_mask));
	if ((bus_max == 0 && bus_log == 0) || bus_max > 2) {
		return;
	}
	if (kernel_thread_start(bus_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}

/*
 * The Galaxy S9's CPU clocks through ACPM's DVFS (Samsung's cal_dfs_set_rate -> exynos_acpm_set_rate: domain index,
 * kHz, FREQ_REQ). acpm_dvfs_exynos9810.h: dvfs_cpucl0 (the A55s) is index 2, dvfs_cpucl1 (the M3s) index 3.
 * boot-args cpufreq_lit=kHz, cpufreq_big=kHz; nothing without them. With cpufreq_gov=1 the governor
 * (drivers/power/exynos/hakios.exynos_cpufreq.c) sets both clusters' rates instead, and these two are not used.
 */
void hakios_exynos_dvfs_start(void);
void
hakios_exynos_dvfs_start(void)
{
	static const struct { const char *arg, *name; uint32_t index; } doms[] = {
		{ "cpufreq_lit", "the A55s (cpucl0)", 2 }, { "cpufreq_big", "the M3s (cpucl1)", 3 },
	};

	hakios_exynos_bus_start();
	if (hakios_exynos_cpufreq_gov_start()) {
		return;
	}
	for (uint32_t i = 0; i < sizeof(doms) / sizeof(doms[0]); i++) {
		uint32_t want = 0, before = 0, after = 0;
		int e1, e2, e3;

		if (!PE_parse_boot_argn(doms[i].arg, &want, sizeof(want)) || want == 0) {
			continue;
		}
		if (!hakios_acpm_init(printf)) {
			printf("dvfs: no ACPM: %s left as they are\n", doms[i].name);
			return;
		}
		e1 = hakios_acpm_dvfs_get(doms[i].index, &before);
		e2 = hakios_acpm_dvfs_set(doms[i].index, want);
		e3 = hakios_acpm_dvfs_get(doms[i].index, &after);
		{
			char line[160];

			snprintf(line, sizeof(line), "dvfs: %s: %u kHz (%d) -> asked %u kHz (%d) -> now %u kHz (%d)\n", doms[i].name,
			    before, e1, want, e2, after, e3);
			printf("%s", line);
			loader_log_line(line);
		}
	}
}
