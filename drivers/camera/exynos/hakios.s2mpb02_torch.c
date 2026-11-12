#define HAKIOS_SOC_FIXED
/*
 * The Galaxy S9's flashlight (starlte, Exynos 9810): the rear flash LED, driven by the flash block of the S2MPB02 (the
 * camera's PMIC, 0x59 on the HSI2C of USI10 of PERIC1, 0x10920000), switched by a pin of the SoC. Only with
 * boot-arg torch=N on this phone (the SoC's registers at 0x10000000; the POCO's driver, hakios.torch.c, is another
 * phone's and is left out of the S9's kernel). Every address and number below belongs to this phone.
 *
 * The way Android lights it (Samsung's leds-s2mpb02.c, its GPIO way): set up once -- FLED_CTRL1 (0x12) bit 3 clear (not
 * the low-voltage mode), FLED_CUR1 (0x14) [3:0] the torch current (20 mA a step), FLED_TIME1 (0x15) [3:0] the torch
 * timer (0xF), FLED_CTRL1 [7:6] = 00 (the LED's on and off from the pins, not from here), FLED_CTRL2 (0x13) = 0x38 --
 * and then: on, the flash pin low and the torch pin high; off, both low. FLED_CTRL2 is the value Android leaves on
 * this phone: s2mpb02_led_setup() sets its [3:0] to 4 and then, with CONFIG_LEDS_IRIS_IRLED_SUPPORT (on in the S9's
 * defconfig: the iris scanner's IR LED is this chip's second channel), writes the whole register 0x38 ("remote mode
 * off, power-LED mode on, GPIO polarity"); bit 3, set there and clear in 0x04, is not to be guessed at. The pins:
 *   torch1   gpg2 pin 4 of PERIC0's GPIO (CON 0x104300C0 [19:16], DAT 0x104300C4 bit 4)
 *   flash1   gpg3 pin 5 of PERIC1's GPIO (CON 0x108300C0 [23:20], DAT 0x108300C4 bit 5)
 * The pins are the SoC's, so a restart -- how every run of this port ends -- puts the LED out whatever happened.
 * Android's flashlight is 60 mA (code 3); iOS's levels are thousandths of that, never more.
 *
 * Writing to a PMIC is not like writing anywhere else: the S2MPB02 makes the cameras' supplies (and the proximity
 * sensor's LED's) as well. Every write goes through one function that takes the flash block's four registers alone
 * (0x12-0x15), and is read back; a write that does not read back ends all writing for the run.
 *
 *   torch=1    a look, reading only: the bus as found, the chip's ID, the flash block's registers, the two pins
 *   torch=2    ... and the light on once for torch_secs (default 2), then off
 *   torch=3    ... the light for iOS until the run ends: /dev/hakios-torch takes a level of 0 to 1000 (a 32-bit number,
 *              0 off) and a read gives what is lit; on for torch_max_s at a time at most (default 30, never more than
 *              120), off and staying off while the battery is above 45 C
 *   torch_flash_ms=N after setup, test the rear camera's flash output for N ms (1-500; Star2 defaults to 120). The
 *              flash current is Apollo's device-tree code 12 (1.25 A), its hardware timer code 7, and both pins are
 *              forced low again before the normal iOS torch service starts.
 *   torch_code_max=N  the torch current code iOS's full level maps to (1-9, default 3: the S9's 60 mA). Star2's device
 *              tree gives Android's flashlight levels codes 2-9 (torch-sec1's torch_table); 9 is never exceeded.
 * The chip's controller is the camera driver's too: every message goes under hakios_s2mpb02_bus_lock().
 *
 * Sources: the phone's device tree (hsi2c@10920000, s2mpb02@59 and its flash node: torch1-gpio, flash1-gpio, the
 * currents and timers), Samsung's leds-s2mpb02.c and s2mpb02-private.h of the S9's kernel, Linux's pinctrl-exynos.c
 * (GPL: read for these facts, not copied).
 */
#include <kern/clock.h>
#include <kern/locks.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_time.h>
#include <mach/mach_types.h>
#include <miscfs/devfs/devfs.h>
#include <pexpert/pexpert.h>
#include <sys/conf.h>
#include <sys/hakios.exynos_hsi2c.h>
#include <sys/hakios.input.h>
#include <sys/systm.h>
#include <sys/uio.h>
#include <sys/hakios.exynos_soc.h>
#include <sys/hakios.log.h>
#include <sys/hakios.xnu.h>

#define CHIP            0x59
#define REG_ID          0x00
#define FLED_CTRL1      0x12
#define FLED_CTRL2      0x13
#define FLED_CUR1       0x14
#define FLED_TIME1      0x15
#define FLED_CTRL2_ANDROID 0x38         /* what Android leaves in FLED_CTRL2 on the S9 (the header) */
#define TORCH_CODE_MAX  3               /* 60 mA: Android's flashlight on the S9 */
#define TORCH_CODE_CAP  9               /* Star2's DT torch_table tops out at 9: torch_code_max never above it */
#define FLASH_CODE      12              /* Apollo star2: brightness 0xc, 1.25 A */
#define FLASH_TIME_CODE 7               /* Apollo star2: timeout 0x7 */
#define HOT_C           45

#define GPIO_PERIC0_PA  EXYNOS_GPIO_PERIC0_PA
#define GPIO_PERIC1_PA  EXYNOS_GPIO_PERIC1_PA
#define GPG2            0x0C0           /* in PERIC0's: torch1 is pin 4 */
#define GPG3            0x0C0           /* in PERIC1's: flash1 is pin 5 */
#define TORCH_PIN       4
#define FLASH_PIN       5

static struct hakios_hsi2c bus = {
	.name = "s2mpb02",
	.regs_pa = EXYNOS_PERIC1_USI10_HSI2C0_PA,                  /* hsi2c@10920000: USI10 of PERIC1 */
	.usi_mode_pa = EXYNOS_PERIC1_USI10_MODE_PA,              /* its mode word */
	.cmu_pa = EXYNOS_CMU_PERIC1_PA, .cmu_size = 0x4000,       /* CMU_PERIC1 */
	.gates = { 0x2048, 0x20AC, 0x2114, 0x2118 }, .ngates = 4,
	.qch = 0x3050, .div = 0x1828, .mux = 0x0120,       /* DIV_CLK_PERIC1_USI10_USI; MUX_CLKCMU_PERIC1_IP_USER */
	.pins_pa = EXYNOS_GPIO_PERIC1_PA + 0x40,                  /* PERIC1's GPIO, bank gpp6: SCL pin 0, SDA pin 1 */
	.pins_mask = 0xffu, .pins_want = 0x22u,
	.ipclk_mhz = 200, .bus_hz = 400000,
};

static vm_offset_t peric0, peric1;
static uint32_t lit;                    /* the level lit now, 0 to 1000 */
static uint64_t lit_since;
static uint32_t max_secs = 30;
static uint32_t torch_code_max = TORCH_CODE_MAX;   /* torch_code_max: the brightest torch code iOS's 1000 maps to (1-9) */
static bool broken;                     /* a write did not read back, or the bus failed: no more writing */
static lck_mtx_t *torch_lock;

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

/*
 * The S2MPB02's controller (hsi2c@10920000) is the camera driver's too (hakios.exynos_camera.c: the cameras' LDOs):
 * one transfer at a time, whichever driver makes it. Taken by both around every message to the chip.
 */
static LCK_GRP_DECLARE(s2mpb02_bus_grp, "hakios-s2mpb02-bus");
static LCK_MTX_DECLARE(s2mpb02_bus_mtx, &s2mpb02_bus_grp);

void hakios_s2mpb02_bus_lock(void);
void hakios_s2mpb02_bus_unlock(void);

void
hakios_s2mpb02_bus_lock(void)
{
	lck_mtx_lock(&s2mpb02_bus_mtx);
}

void
hakios_s2mpb02_bus_unlock(void)
{
	lck_mtx_unlock(&s2mpb02_bus_mtx);
}

static uint32_t
get_locked(uint8_t reg, uint8_t *value)
{
	return hakios_hsi2c_read_reg(&bus, CHIP, reg, value, 1);
}

static uint32_t
get(uint8_t reg, uint8_t *value)
{
	uint32_t error;

	hakios_s2mpb02_bus_lock();
	error = get_locked(reg, value);
	hakios_s2mpb02_bus_unlock();
	return error;
}

/* the flash block's registers only, every write read back */
static bool
put(uint8_t reg, uint8_t value)
{
	uint8_t buf[2] = { reg, value }, back = 0;
	bool failed;

	if (broken || reg < FLED_CTRL1 || reg > FLED_TIME1) {
		return false;
	}
	hakios_s2mpb02_bus_lock();
	failed = hakios_hsi2c_message(&bus, CHIP, false, buf, 2, true) != 0 || get_locked(reg, &back) != 0 || back != value;
	hakios_s2mpb02_bus_unlock();
	if (failed) {
		say("s2mpb02: register %#x: wrote %#x, read back %#x (bus %#x): no more writing for this run\n", reg, value, back,
		    bus.last_error);
		broken = true;
		return false;
	}
	return true;
}

static bool
put_bits(uint8_t reg, uint8_t mask, uint8_t value)
{
	uint8_t now;

	return get(reg, &now) == 0 && put(reg, (uint8_t)((now & ~mask) | (value & mask)));
}

/* a pin: an output (CON nibble 1) at the level given */
static void
pin(vm_offset_t bank, uint32_t number, bool high)
{
	uint32_t data = rd(bank, 4);

	wr(bank, 4, high ? data | (1u << number) : data & ~(1u << number));
	wr(bank, 0, (rd(bank, 0) & ~(0xfu << (4 * number))) | (1u << (4 * number)));
}

static void
dark(void)
{
	pin(peric0 + GPG2, TORCH_PIN, false);
	pin(peric1 + GPG3, FLASH_PIN, false);
	lit = 0;
}

static bool
light(uint32_t permille)
{
	uint32_t code = (torch_code_max * permille + 999) / 1000;

	if (permille == 0) {
		dark();
		return true;
	}
	if (code < 1) {
		code = 1;
	}
	if (code > torch_code_max) {
		code = torch_code_max;
	}
	if (!put_bits(FLED_CUR1, 0x0f, (uint8_t)code)) {
		dark();
		return false;
	}
	pin(peric1 + GPG3, FLASH_PIN, false);
	pin(peric0 + GPG2, TORCH_PIN, true);
	if (lit == 0) {
		lit_since = mach_absolute_time();
	}
	lit = permille;
	return true;
}

static bool
flash_test(uint32_t ms)
{
	if (ms == 0) return true;
	if (ms > 500) ms = 500;
	if (!put_bits(FLED_CUR1, 0xf0, FLASH_CODE << 4) || !put_bits(FLED_TIME1, 0xf0, FLASH_TIME_CODE << 4)) {
		dark();
		return false;
	}
	pin(peric0 + GPG2, TORCH_PIN, false);
	pin(peric1 + GPG3, FLASH_PIN, true);
	say("s2mpb02: camera flash test: %u ms at Apollo code %u (1.25 A)\n", ms, FLASH_CODE);
	IOSleep(ms);
	dark();
	return true;
}

/*
 * On the way back to the loader (loader_return(), a panic's way out too: one processor left, no lock taken): the light
 * off, if it is lit. dark() is the two pins only, no I2C; the chip keeps its registers, the pins are what light it.
 */
void
hakios_s2mpb02_torch_quiesce(void)
{
	if (peric0 == 0 || peric1 == 0 || lit == 0) {
		return;
	}
	dark();
	say("s2mpb02: at the run's end: the light off (torch and flash pins low)\n");
}

static bool
too_hot(void)
{
	int c = hakios_battery_temperature();

	return c != -273 && c > HOT_C;
}

static bool
configure(void)
{
	return put_bits(FLED_CTRL1, 0x08, 0x00) && put_bits(FLED_CUR1, 0x0f, 1) && put_bits(FLED_TIME1, 0x0f, 0x0f) &&
	       put_bits(FLED_CTRL1, 0xc0, 0x00) && put_bits(FLED_CTRL2, 0xff, FLED_CTRL2_ANDROID);
}

/* ---- /dev/hakios-torch ---- */

static int
torch_write(__unused dev_t dev, struct uio *uio, __unused int ioflag)
{
	uint32_t level;
	int error = uiomove((caddr_t)&level, sizeof(level), uio);

	if (error != 0) {
		return error;
	}
	if (level > 1000) {
		level = 1000;
	}
	lck_mtx_lock(torch_lock);
	if (level != 0 && too_hot()) {
		say("s2mpb02: the battery is above %d C: the light stays off\n", HOT_C);
		dark();
		error = EBUSY;
	} else if (!light(level)) {
		error = EIO;
	}
	lck_mtx_unlock(torch_lock);
	return error;
}

static int
torch_read(__unused dev_t dev, struct uio *uio, __unused int ioflag)
{
	uint32_t level = lit;

	return uiomove((caddr_t)&level, sizeof(level), uio);
}

static int
torch_open(__unused dev_t dev, __unused int flags, __unused int type, __unused struct proc *p)
{
	return 0;
}

static const struct cdevsw torch_cdevsw = {
	.d_open = torch_open, .d_close = torch_open, .d_read = torch_read, .d_write = torch_write,
	.d_ioctl = eno_ioctl, .d_stop = eno_stop, .d_reset = eno_reset, .d_select = eno_select,
	.d_mmap = eno_mmap, .d_strategy = eno_strat, .d_type = 0
};

static void
torch_thread(void *arg, __unused wait_result_t result)
{
	uint32_t steps = (uint32_t)(uintptr_t)arg, secs = 2, flash_ms = 0;
	uint8_t id = 0, regs[4] = { 0 };

	say("s2mpb02: 1: map the bus (USI10 of PERIC1) and the two pins' GPIO blocks\n");
	peric0 = ml_io_map(GPIO_PERIC0_PA, 0x1000);
	peric1 = ml_io_map(GPIO_PERIC1_PA, 0x1000);
	if (peric0 == 0 || peric1 == 0 || !hakios_hsi2c_map(&bus, say)) {
		goto out;
	}
	say("s2mpb02: 1: torch1 (gpg2-4): CON %x DAT %x; flash1 (gpg3-5): CON %x DAT %x\n",
	    (rd(peric0 + GPG2, 0) >> (4 * TORCH_PIN)) & 0xf, (rd(peric0 + GPG2, 4) >> TORCH_PIN) & 1,
	    (rd(peric1 + GPG3, 0) >> (4 * FLASH_PIN)) & 0xf, (rd(peric1 + GPG3, 4) >> FLASH_PIN) & 1);
	hakios_s2mpb02_bus_lock();
	hakios_hsi2c_power(&bus);
	hakios_hsi2c_reset(&bus);
	hakios_s2mpb02_bus_unlock();
	if (get(REG_ID, &id) != 0) {
		say("s2mpb02: 1: no answer at 0x%x (%x)\n", CHIP, bus.last_error);
		goto out;
	}
	for (int i = 0; i < 4; i++) {
		(void)get((uint8_t)(FLED_CTRL1 + i), &regs[i]);
	}
	say("s2mpb02: 1: ID %#x; FLED_CTRL1 %#x, CTRL2 %#x, CUR1 %#x, TIME1 %#x\n", id, regs[0], regs[1], regs[2], regs[3]);
	if (steps < 2) {
		goto out;
	}
	if (!configure()) {
		say("s2mpb02: 2: the flash block could not be set up\n");
		goto out;
	}
	PE_parse_boot_argn("torch_flash_ms", &flash_ms, sizeof(flash_ms));
	if (flash_ms && (too_hot() || !flash_test(flash_ms))) {
		say("s2mpb02: camera flash test refused or failed\n");
	}
	if (steps == 2) {
		PE_parse_boot_argn("torch_secs", &secs, sizeof(secs));
		secs = secs > 10 ? 10 : secs;
		say("s2mpb02: 2: the light on for %u s at Android's 60 mA\n", secs);
		light(1000);
		delay_for_interval(secs * 1000, NSEC_PER_MSEC);
		dark();
		say("s2mpb02: 2: off\n");
		goto out;
	}
	PE_parse_boot_argn("torch_max_s", &max_secs, sizeof(max_secs));
	PE_parse_boot_argn("torch_code_max", &torch_code_max, sizeof(torch_code_max));
	torch_code_max = torch_code_max == 0 ? TORCH_CODE_MAX : torch_code_max > TORCH_CODE_CAP ? TORCH_CODE_CAP : torch_code_max;
	max_secs = max_secs == 0 ? 30 : max_secs > 120 ? 120 : max_secs;
	{
		int major = cdevsw_add(-1, &torch_cdevsw);

		if (major < 0 || devfs_make_node(makedev(major, 0), DEVFS_CHAR, UID_ROOT, GID_WHEEL, 0666, "hakios-torch") == NULL) {
			say("s2mpb02: 3: /dev/hakios-torch cannot be made\n");
			goto out;
		}
	}
	say("s2mpb02: 3: /dev/hakios-torch: the flashlight for iOS, %u s at a time at most\n", max_secs);
	for (;;) {
		delay_for_interval(100, NSEC_PER_MSEC);
		lck_mtx_lock(torch_lock);
		if (lit != 0) {
			uint64_t on_ns;

			absolutetime_to_nanoseconds(mach_absolute_time() - lit_since, &on_ns);
			if (on_ns / NSEC_PER_SEC >= max_secs) {
				say("s2mpb02: on for %u s: off, until it is asked for anew\n", max_secs);
				dark();
			} else if (too_hot()) {
				say("s2mpb02: the battery is above %d C: off\n", HOT_C);
				dark();
			}
		}
		lck_mtx_unlock(torch_lock);
	}
out:
	thread_terminate(current_thread());
}

void
hakios_s2mpb02_torch_init(void)
{
	uint32_t steps = 0, camera = 0;
	thread_t thread;
	kern_return_t result;

	if (!PE_parse_boot_argn("torch", &steps, sizeof(steps)) || steps == 0) {
		return;
	}
	if (pe_arm_get_soc_base_phys() != EXYNOS_SOC_BASE_PA) {
		return;                 /* not the Exynos 9810's (the POCO's torch=N is its own driver's) */
	}
	if (PE_parse_boot_argn("cam_s9", &camera, sizeof(camera)) && camera != 0) {
		say("s2mpb02: cam_s9=%u shares this controller; camera discovery was started first\n", camera);
	}
	torch_lock = lck_mtx_alloc_init(lck_grp_alloc_init("s2mpb02", LCK_GRP_ATTR_NULL), LCK_ATTR_NULL);
	result = kernel_thread_start(torch_thread, (void *)(uintptr_t)steps, &thread);
	if (result == KERN_SUCCESS) {
		thread_deallocate(thread);
	} else {
		say("s2mpb02: torch worker could not start (%#x)\n", result);
	}
}
