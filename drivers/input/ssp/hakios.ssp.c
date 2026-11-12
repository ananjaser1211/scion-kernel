#define HAKIOS_SOC_FIXED
/*
 * Sensor hub driver for the Galaxy S9 (starlte, Exynos 9810).
 *
 * The accelerometer, gyroscope, magnetometer, barometer, light and proximity sensors sit behind one sensor hub: the MCU
 * of the Broadcom BCM4775 B0 GNSS chip ("ssp,BCM4773" in Samsung's device tree), reached over one SPI bus and four
 * GPIOs. On Android the proprietary lhd daemon powers the chip, loads the hub firmware into its RAM through the chip's
 * ROM and carries Samsung's sensor protocol (SSP) to the kernel. This file does all three.
 *
 * Depends on: the Exynos SPI driver (sys/hakios.exynos_spi.h), PMIC access through ACPM (sys/hakios.exynos_acpm.h),
 * the protocol layers in hakios.bcm_shub.h (tested on a host by tools/bcm_shub_test.c) and the hub firmware patch
 * fw/bcm_sensorhub_a1.bin in the small image (devices/starlte/firmware/sensorhub/). Runs only when the SoC base
 * address is the Exynos 9810's (0x10000000).
 *
 * Boot args. Nothing is touched without shub=N; the others apply only to the steps that use them.
 *   shub=N   run steps 1..N (N is capped at 6); each step includes the ones before it:
 *            1  read only: CHUB power domain and clocks (nothing of CHUB is read unless both are on, since a block
 *               whose domain or clock is off can stop the bus), LDO37, the SPI's clocks and pins, the hub's five pins
 *            2  power up: LDO37 on if off, SPI set up, AP_SLEEP high, NSTDBY low 20 ms then high (the chip leaves
 *               standby and starts in its ROM); MCU_REQ / MCU_RESP tried once; read what the chip sends
 *            3  ROM: autobaud burst, transport layer sync, version request (RPC 0x00, answered by RPC 0x03): the
 *               chip's ASIC and ROM versions
 *            4  firmware: read and check the patch, write it record by record (RPC 0x05, reliable packets, each
 *               acknowledged), start it (RPC 0x2E at PatchAddress 0x00800161), sync again, read the application's
 *               version
 *            5  SSP: wait for the hub's "sensor init done", WHOAMI (0x55), send the sensor formation (from the device
 *               tree), read the sensors found (SENSOR_SCANNING), the firmware revision and the accelerometer range
 *            6  enable the sensors in shub_sensors and read samples for as long as the phone runs. The USB console's
 *               "sensors" shows the last of each; /dev/hakios-sensors hands them to user space (sys/hakios.sensors.h;
 *               the reader is userland/src/inject.c in backboardd, which turns them into HID events)
 *   shub_sensors=<mask>  sensors step 6 enables, a bitmap of Samsung's sensor numbers (sensor_list.h: 0 accel, 1 gyro,
 *            4 magnetometer, 5 pressure, 7 proximity, 9 light). Default 0x2b3, those six. Only sensors the hub
 *            found are enabled.
 *   shub_ms=<n>          sample period in ms (default 20; light, proximity and pressure use 200)
 *   shub_div=<n>         USI_CHUB00 clock divider ratio, 0-15 (default 15: at most 1/16 of CHUB's bus clock, divided
 *            by 4 again in the SPI, about 5.5 MHz; Samsung runs it up to 25 MHz). 255 leaves it as found.
 *   shub_chub_on=1       at step 2, power the CHUB domain on if step 1 found it off, using the PMU's chub_on sequence
 *            from Samsung's flexpmu tables. Default: stop.
 *   shub_keep=0          power the hub off again (NSTDBY low) when the steps end or one fails. Default 1: leave it
 *            running, which the sensors need.
 *   shub_log=1           log the first bytes of every SPI frame (very loud). The first 40 frames are always logged.
 *   shub_hello=1         wait for MCU_RESP before every exchange, as Samsung's driver does. Default: raise MCU_REQ,
 *            wait 1 ms, then transfer: on the S9 MCU_RESP reads 0 whatever the chip does, and the chip answers
 *            without it.
 *   shub_req_rpc=<n>     RPC that carries SSP to the hub. Default 0x20: inferred from the hub's answers in RPC 0x21,
 *            not observed (lhd builds it). shub_req_len=0 sends the SSP bytes without the u16 length before them.
 *   shub_acc_pos=<0-7>   the accelerometer's (and gyroscope's) position in the formation of step 5. Default: the device
 *            tree's, 4 on the S9 (starlte), 7 on the S9+ (star2lte).
 *   shub_acc_cal=X,Y,Z   the accelerometer's factory offsets, sent to the hub before the sensors go on, as Samsung does
 *            (factory/accel_lsm6dsl.c set_accel_cal(): SSP 0xCE, s16 x, y, z). The numbers are the three little-endian
 *            s32 of /efs/FactoryApp/calibration_data, in counts at the 8 g range (4096 = 1 g), in the hub's output axes
 *            (so taken with the right shub_acc_pos). Default: none.
 *   shub_gyro_cal=X,Y,Z  the same for the gyroscope (gyro_lsm6dsl.c set_gyro_cal(): SSP 0xCD, s32 x, y, z), the three
 *            little-endian s32 of /efs/FactoryApp/gyro_cal_data. Default: none.
 */
#include <kern/clock.h>
#include <kern/locks.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_time.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/conf.h>
#include <sys/param.h>
#include <sys/errno.h>
#include <sys/hakios.exynos_acpm.h>
#include <sys/hakios.exynos_spi.h>
#include <sys/hakios.input.h>
#include <sys/hakios.sensors.h>
#include <sys/proc.h>
#include <sys/systm.h>
#include <sys/uio.h>
#include <miscfs/devfs/devfs.h>
#include <sys/hakios.exynos_soc.h>
#include "hakios.bcm_shub.h"
#include <sys/hakios.log.h>
#include <sys/hakios.xnu.h>

/*
 * Hardware, from Samsung's device tree for the S9 (exynos9810-starlte_eur_open_26.dts, nodes spi@13AC0000 and
 * BCM4773@0) and its drivers:
 *
 * SPI      spi@13AC0000 = USI_CHUB00, "samsung,exynos-spi", 64-byte FIFO, mode 3 (spi-cpol, spi-cpha), at most 25 MHz,
 *          one chip select (gph0-3: the controller's own nSS, function 2). Its USI mode word usi@13A13000
 *          (usi_00_chub): 2 = SPI. Driven by drivers/soc/exynos/hakios.exynos_spi.c.
 * pins     gph0 of CHUB's GPIO block (pinctrl@13A80000, bank +0x00): -0 SCLK, -1 MOSI, -3 nSS (function 2), -2 MISO
 *          (function 2, pull-down) ("sensorhub_mosi_sck_ssn", "sensorhub_miso"); -4 MCU_RESP input (the chip's
 *          "awake"), -5 MCU_REQ output (the AP's "wake up"). -7 belongs to the MST driver: its bits are never written.
 *          HOST_REQ: gpa1-7 of ALIVE's GPIO block (0x14050000, bank +0x40), the chip's "I have data". Samsung uses a
 *          level-high interrupt; here it is polled.
 *          AP_SLEEP: gpa0-5 (bank +0x20, "ssp-batch-wake-irq"), an output, high while the AP runs (ssp_dev.c).
 *          SHUB_INT: gpa4-0 (bank +0xC0), gyro time stamps for the camera. Unused here.
 *          NSTDBY: gpg0-2 of PERIC0's GPIO block (0x10430000, bank +0x80), the "gps" node's pin (ssp-gps-pwr-en).
 *          Low holds the chip in standby. Samsung's kernel leaves it low; lhd drives it.
 * supply   vdd_mcu_1p8 = LDO37 of the S2MPS18 (CTRL 0x6E, on = bits 7:6 set), reached through ACPM. Always-on in
 *          Samsung's tree. The sensors' own supplies are the hub's business.
 * clocks   CMU_CHUB 0x13A00000: GATE_CLK_CHUB_USI00 +0x2010, RSTNSYNC_CLK_CHUB_USI00 +0x2090, USI_CHUB00 IPCLK +0x20A8,
 *          Q-channel +0x305C, DIV_CLK_CHUB_USI00 +0x1808, MUX_CLK_CHUB_USI00 +0x100C (cmucal-sfr.c). Samsung's table
 *          runs the IP clock at 177.666 MHz (DIV 1 from CHUB's bus clock, cmucal-vclk.c). CHUB's bus clock comes from
 *          CMU_TOP (0x1A240000): GATE_CLKCMU_CHUB_BUS +0x2014, DIV +0x1810, MUX +0x1014.
 * CHUB domain  PMU 0x14060000: CHUB_CONFIGURATION +0x4080, CHUB_STATUS +0x4084 ([3:0] 0xF = on). Samsung's S9 never
 *          switches it (its tree has no power domain for it), so it is on as the boot left it; if it is off, this
 *          driver stops unless shub_chub_on=1.
 *
 * Sources: Samsung's GPL kernel for the S9, Scion-Project/android_kernel_samsung_exynos9810 at e5906667068e, read for
 * these facts, not copied: drivers/sensorhub/brcm/ (ssp.h, ssp_dev.c, ssp_bbd.c, ssp_i2c.c, ssp_data.c, ssp_sysfs.c,
 * sensor_list.h, bbdpl/bcm_gps_spi.c and .h, bbd.c, bbd_rpc_lh.c, bcm_gps_regs.c, q_os/bbd_patch_file_star.h),
 * drivers/spi/spi-s3c64xx.c, drivers/pinctrl/samsung/pinctrl-exynos.c, drivers/soc/samsung/cal-if/exynos9810/
 * (cmucal-sfr.c, cmucal-vclk.c, cmucal-vclklut.c, cmucal-node.c, flexpmu_cal_local_exynos9810.h),
 * include/linux/mfd/samsung/s2mps18-private.h, arch/arm64/boot/dts/exynos/exynos9810-starlte_eur_open_26.dts;
 * exynos-linux/samsung-gtactive3wifi docs/re/sensorhub.md (facts only).
 *
 * The ROM half of the firmware download is known only from the Tab Active3's description of lhd (same chip family,
 * same SoC; its patch is a different build of the same firmware).
 */

#define PMU_PA                  EXYNOS_PMU_PA
#define  CHUB_CONFIGURATION     0x4080
#define  CHUB_STATUS            0x4084
#define CMU_TOP_PA              EXYNOS_CMU_TOP_PA
#define  TOP_CHUB_MUX           0x1014
#define  TOP_CHUB_DIV           0x1810
#define  TOP_CHUB_GATE          0x2014
#define CMU_CHUB_PA             EXYNOS_CMU_CHUB_PA
#define  CHUB_BUS_USER_MUX      0x0100  /* PLL_CON0_MUX_CLKCMU_CHUB_BUS_USER: bit 4: CMU_TOP's clock, else oscillator */
#define  CHUB_USI00_MUX         0x100C
#define  CHUB_USI00_DIV         0x1808
#define  CHUB_GPIO_QCH          0x3028
#define GPIO_CHUB_PA            EXYNOS_GPIO_CHUB_PA
#define  BANK_GPH0              0x00
#define  PIN_MCU_RESP           4
#define  PIN_MCU_REQ            5
#define GPIO_ALIVE_PA           EXYNOS_GPIO_ALIVE_PA
#define  BANK_GPA0              0x20
#define  BANK_GPA1              0x40
#define  BANK_GPA4              0xC0
#define  PIN_AP_SLEEP           5       /* gpa0 */
#define  PIN_HOST_REQ           7       /* gpa1 */
#define  PIN_SHUB_INT           0       /* gpa4 */
#define GPIO_PERIC0_PA          EXYNOS_GPIO_PERIC0_PA
#define  BANK_GPG0              0x80
#define  PIN_NSTDBY             2       /* gpg0 */
#define LDO37_CTRL              0x6E
#define LDO_ON                  0xC0

#define GATE_MANUAL             (1u << 20)
#define GATE_ON                 (1u << 21)

/*
 * Sensor formation values sent in step 5, from the device tree (ssp-acc-position, ssp-mag-position; Samsung's
 * set_sensor_position() sends the accelerometer's twice, for the gyroscope of the same LSM6DSL). The hub turns the
 * chips' axes into Android's with them, so a wrong one turns every motion sample (and iOS's orientation) by 90 degrees
 * or mirrors it. The S9 (exynos9810-starlte_*_26.dts) mounts the LSM6DSL at position 4, the S9+
 * (exynos9810-star2lte_kor_26.dts and _eur_open_26.dts) at 7; the magnetometer is at 5 on both.
 * shub_acc_pos=<0-7> overrides the accelerometer's.
 */
#define ACC_POSITION_S9         4
#define ACC_POSITION_S9PLUS     7
#define MAG_POSITION            5
#define PATCH_ENTRY             0x00800161u     /* <PatchAddress=> of bbd_patch_file_star.h */
#define PATCH_FILE              "bcm_sensorhub_a1.bin"

/* Timings: from bcm_gps_spi.c and ssp_*.c, and for lhd from the Tab Active3's notes */
#define HELLO_POLLS             100
#define NSTDBY_LOW_MS           20
#define AFTER_POWER_MS          50
#define AUTOBAUD_BYTES          36
#define AUTOBAUD_MS             100
#define SYNC_TRIES              6
#define SYNC_EVERY_MS           100
#define AUTOBAUD_TRIES          5
#define VERSION_MS              250
#define VERSION_TRIES           5
#define RELIABLE_MS             500
#define RELIABLE_TRIES          10
#define START_MS                40
#define SSP_MS                  400
#define INIT_DONE_MS            1000
#define ACK_EVERY               100

static struct hakios_exynos_spi bus = {
	.name = "shub-spi",
	.regs_pa = EXYNOS_CHUB_USI00_SPI_PA,                  /* spi@13AC0000: USI_CHUB00 */
	.usi_mode_pa = EXYNOS_CHUB_USI00_MODE_PA,              /* usi@13A13000, usi_00_chub */
	.cmu_pa = CMU_CHUB_PA, .cmu_size = 0x4000,
	.gates = { 0x2010, 0x2090, 0x20A8 }, .ngates = 3,
	.qch = 0x305C, .div = CHUB_USI00_DIV, .mux = CHUB_USI00_MUX,
	.div_ratio = 15, .ipclk_mhz = 355,
	.pins_pa = GPIO_CHUB_PA + BANK_GPH0,    /* gph0-0..3: function 2; -2 (MISO) pull-down */
	.pins_mask = 0xffffu, .pins_want = 0x2222u,
	.pud_mask = 0xffffu, .pud_want = 0x0100u,
	.fifo_bytes = 64,
	.mode = 3,                              /* CPOL 1, CPHA 1 */
};

static vm_offset_t pmu, cmu_top, cmu_chub, gpio_chub, alive, peric0;
static uint32_t steps, keep = 1, log_frames, div_ratio = 15, chub_on_ok;
static uint32_t want_sensors = 0x2b3, period_ms = 20, req_rpc = SHUB_RPC_SENSOR_REQ, req_len = 1;
static uint32_t acc_position = ACC_POSITION_S9; /* hakios_ssp_init(): by HAKIOS_DEVICE, or shub_acc_pos= */
static bool acc_cal_given, gyro_cal_given;      /* shub_acc_cal=, shub_gyro_cal= */
static int32_t acc_cal[3], gyro_cal[3];
static bool acc_cal_subtract;                   /* the hub took no 0xCE: the offsets are taken off here instead */
static bool powered;
static const char *state = "off";

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

static void
sleep_ms(uint32_t ms)
{
	delay_for_interval(ms, NSEC_PER_MSEC);
}

static uint64_t
deadline_ms(uint32_t ms)
{
	uint64_t interval;

	nanoseconds_to_absolutetime((uint64_t)ms * NSEC_PER_MSEC, &interval);
	return mach_absolute_time() + interval;
}

static uint64_t
now_ns(void)
{
	uint64_t ns;

	absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
	return ns;
}

static uint32_t
pin_con(vm_offset_t gpio, uint32_t bank, unsigned pin)
{
	return (rd(gpio, bank) >> (4 * pin)) & 0xf;
}

static uint32_t
pin_dat(vm_offset_t gpio, uint32_t bank, unsigned pin)
{
	return (rd(gpio, bank + 4) >> pin) & 1;
}

static void
pin_set(vm_offset_t gpio, uint32_t bank, unsigned pin, bool high)
{
	uint32_t dat = rd(gpio, bank + 4);

	wr(gpio, bank + 4, high ? dat | 1u << pin : dat & ~(1u << pin));
}

/* Set the level before switching the pin to output, so it never drives the opposite level. */
static void
pin_output(vm_offset_t gpio, uint32_t bank, unsigned pin, bool high)
{
	pin_set(gpio, bank, pin, high);
	wr(gpio, bank, (rd(gpio, bank) & ~(0xfu << (4 * pin))) | 1u << (4 * pin));
}

static void
pin_input(vm_offset_t gpio, uint32_t bank, unsigned pin)
{
	wr(gpio, bank, rd(gpio, bank) & ~(0xfu << (4 * pin)));
}

static bool
host_req(void)
{
	return pin_dat(alive, BANK_GPA1, PIN_HOST_REQ) != 0;
}

static bool
mcu_resp(void)
{
	return pin_dat(gpio_chub, BANK_GPH0, PIN_MCU_RESP) != 0;
}

static void
mcu_req(bool high)
{
	pin_set(gpio_chub, BANK_GPH0, PIN_MCU_REQ, high);
}

static void
look_pins(const char *when)
{
	say("shub: %s: gph0 CON %08x PUD %08x DAT %02x (SPI on -0..-3: CON [15:0] 2222); MCU_RESP gph0-4 function %x level "
	    "%u, MCU_REQ gph0-5 function %x level %u\n", when, rd(gpio_chub, BANK_GPH0), rd(gpio_chub, BANK_GPH0 + 8),
	    rd(gpio_chub, BANK_GPH0 + 4), pin_con(gpio_chub, BANK_GPH0, PIN_MCU_RESP), pin_dat(gpio_chub, BANK_GPH0,
	    PIN_MCU_RESP), pin_con(gpio_chub, BANK_GPH0, PIN_MCU_REQ), pin_dat(gpio_chub, BANK_GPH0, PIN_MCU_REQ));
	say("shub: %s: HOST_REQ gpa1-7 function %x level %u; AP_SLEEP gpa0-5 function %x level %u; SHUB_INT gpa4-0 function "
	    "%x level %u; NSTDBY gpg0-2 function %x level %u\n", when, pin_con(alive, BANK_GPA1, PIN_HOST_REQ),
	    pin_dat(alive, BANK_GPA1, PIN_HOST_REQ), pin_con(alive, BANK_GPA0, PIN_AP_SLEEP),
	    pin_dat(alive, BANK_GPA0, PIN_AP_SLEEP), pin_con(alive, BANK_GPA4, PIN_SHUB_INT),
	    pin_dat(alive, BANK_GPA4, PIN_SHUB_INT), pin_con(peric0, BANK_GPG0, PIN_NSTDBY),
	    pin_dat(peric0, BANK_GPG0, PIN_NSTDBY));
}

static bool
chub_clocked(void)
{
	uint32_t g = rd(cmu_top, TOP_CHUB_GATE);

	return (rd(pmu, CHUB_STATUS) & 0xf) == 0xf && (!(g & GATE_MANUAL) || (g & GATE_ON));
}

/* Read only. Returns false, having read nothing of CHUB, when its domain is off or not clocked. */
static bool
look(void)
{
	uint32_t status = rd(pmu, CHUB_STATUS) & 0xf;
	uint8_t ldo = 0;
	int e;

	say("shub: 1: CHUB's power domain: CONFIGURATION %x STATUS %x (0xf: on); CMU_TOP's clock for CHUB's bus: mux %08x "
	    "divider %08x gate %08x\n", rd(pmu, CHUB_CONFIGURATION), status, rd(cmu_top, TOP_CHUB_MUX),
	    rd(cmu_top, TOP_CHUB_DIV), rd(cmu_top, TOP_CHUB_GATE));
	e = hakios_acpm_pmic_read(HAKIOS_ACPM_PMIC_PMIC, LDO37_CTRL, &ldo);
	say("shub: 1: the hub's supply, LDO37 (vdd_mcu_1p8): CTRL %02x%s\n", ldo, e ? " (not read: ACPM)" :
	    (ldo & LDO_ON) == LDO_ON ? " (on)" : " (off)");
	say("shub: 1: NSTDBY gpg0-2 function %x level %u; HOST_REQ gpa1-7 function %x level %u; AP_SLEEP gpa0-5 function %x "
	    "level %u\n", pin_con(peric0, BANK_GPG0, PIN_NSTDBY), pin_dat(peric0, BANK_GPG0, PIN_NSTDBY),
	    pin_con(alive, BANK_GPA1, PIN_HOST_REQ), pin_dat(alive, BANK_GPA1, PIN_HOST_REQ),
	    pin_con(alive, BANK_GPA0, PIN_AP_SLEEP), pin_dat(alive, BANK_GPA0, PIN_AP_SLEEP));
	if (!chub_clocked()) {
		say("shub: 1: CHUB is %s: nothing of it is read%s\n", status != 0xf ? "unpowered" : "not clocked by CMU_TOP",
		    status != 0xf ? " (shub_chub_on=1 lets step 2 power it)" : "");
		return false;
	}
	say("shub: 1: CMU_CHUB: bus user mux %08x, USI00 mux %08x divider %08x, GPIO Q-channel %08x\n",
	    rd(cmu_chub, CHUB_BUS_USER_MUX), rd(cmu_chub, CHUB_USI00_MUX), rd(cmu_chub, CHUB_USI00_DIV),
	    rd(cmu_chub, CHUB_GPIO_QCH));
	if (!hakios_exynos_spi_map(&bus, say)) {
		return false;
	}
	look_pins("1");
	return true;
}

/* The PMU's chub_on sequence (flexpmu_cal_local_exynos9810.h): domain on, then resets, clamps, sweeper in order. */
static bool
chub_power_on(void)
{
	static const struct { uint16_t off; uint8_t v; } seq[] = {
		{ 0x4b98, 1 }, { 0x4b38, 1 }, { 0x4c10, 1 }, { 0x4b54, 1 }, { 0x4c30, 0 }, { 0x4b54, 0 },
		{ 0x4b38, 0 }, { 0x4c10, 0 }, { 0x4c30, 1 },
	};
	uint64_t deadline = deadline_ms(100);

	wr(pmu, CHUB_CONFIGURATION, (rd(pmu, CHUB_CONFIGURATION) & ~0xfu) | 0xf);
	while ((rd(pmu, CHUB_STATUS) & 0xf) != 0xf) {
		if (mach_absolute_time() > deadline) {
			say("shub: 2: CHUB's domain did not come on (STATUS %x)\n", rd(pmu, CHUB_STATUS));
			return false;
		}
	}
	for (unsigned i = 0; i < sizeof(seq) / sizeof(seq[0]); i++) {
		/* the CM4's own reset (0x4b98) stays asserted: nothing runs on CHUB's core */
		if (seq[i].off == 0x4b98 && seq[i].v == 0) {
			continue;
		}
		wr(pmu, seq[i].off, (rd(pmu, seq[i].off) & ~1u) | seq[i].v);
		delay(10);
	}
	say("shub: 2: CHUB's domain powered on (STATUS %x)\n", rd(pmu, CHUB_STATUS));
	return true;
}

static void
power_off(const char *why)
{
	if (!powered) {
		return;
	}
	pin_set(peric0, BANK_GPG0, PIN_NSTDBY, false);
	mcu_req(false);
	powered = false;
	state = "off";
	say("shub: the hub in standby again (NSTDBY low): %s\n", why);
}

/*
 * Order: LDO37, SPI, pin directions, then NSTDBY low for NSTDBY_LOW_MS and high; the chip starts in its ROM.
 */
static bool
power_on(void)
{
	uint8_t ldo = 0;

	if (!chub_clocked()) {
		if (!chub_on_ok || (rd(pmu, CHUB_STATUS) & 0xf) == 0xf || !chub_power_on() || !chub_clocked()) {
			say("shub: 2: CHUB is off; stopping\n");
			return false;
		}
		if (!hakios_exynos_spi_map(&bus, say)) {
			return false;
		}
	}
	if (hakios_acpm_pmic_read(HAKIOS_ACPM_PMIC_PMIC, LDO37_CTRL, &ldo) == 0 && (ldo & LDO_ON) != LDO_ON) {
		uint8_t back = 0;

		(void)hakios_acpm_pmic_update(HAKIOS_ACPM_PMIC_PMIC, LDO37_CTRL, LDO_ON, LDO_ON);
		(void)hakios_acpm_pmic_read(HAKIOS_ACPM_PMIC_PMIC, LDO37_CTRL, &back);
		say("shub: 2: LDO37 was off (%02x): on now (%02x); 20 ms\n", ldo, back);
		sleep_ms(20);
	}
	bus.div_ratio = div_ratio;
	hakios_exynos_spi_power(&bus);
	hakios_exynos_spi_reset(&bus);
	say("shub: 2: SPI set up: USI00 divider %08x (ratio %u)\n", rd(cmu_chub, CHUB_USI00_DIV),
	    (rd(cmu_chub, CHUB_USI00_DIV) & 0xf) + 1);
	pin_input(gpio_chub, BANK_GPH0, PIN_MCU_RESP);
	pin_output(gpio_chub, BANK_GPH0, PIN_MCU_REQ, false);
	pin_input(alive, BANK_GPA1, PIN_HOST_REQ);
	pin_output(alive, BANK_GPA0, PIN_AP_SLEEP, true);
	pin_output(peric0, BANK_GPG0, PIN_NSTDBY, false);
	powered = true;
	state = "powered";
	sleep_ms(NSTDBY_LOW_MS);
	pin_set(peric0, BANK_GPG0, PIN_NSTDBY, true);
	sleep_ms(AFTER_POWER_MS);
	look_pins("2");
	return true;
}

static uint8_t txf[SHUB_SSI_FRAME + 8], rxf[SHUB_SSI_FRAME + 8];
static struct shub_tl_rx tlrx;
static uint8_t last_status;
static uint32_t frames_logged, hello_fails, spi_errors, fc_retries;
static uint64_t bytes_in, bytes_out;

static void handle_packet(const uint8_t *raw, size_t len);
static bool pay_ack(void);

static void
log_frame(const char *dir, const uint8_t *p, size_t n)
{
	char line[3 * 24 + 1];
	size_t k = 0;

	if (frames_logged >= 40 && log_frames == 0) {
		return;
	}
	frames_logged++;
	for (size_t i = 0; i < n && i < 24; i++) {
		k += (size_t)snprintf(line + k, sizeof(line) - k, "%02x ", p[i]);
	}
	line[k] = 0;
	say("shub: spi %s %zu: %s%s\n", dir, n, line, n > 24 ? "..." : "");
}

static bool
xfer(uint32_t len)
{
	uint32_t e;

	hakios_exynos_spi_select(&bus, true);
	e = hakios_exynos_spi_transfer(&bus, txf, rxf, len);
	hakios_exynos_spi_select(&bus, false);
	if (e != 0) {
		spi_errors++;
		return false;
	}
	return true;
}

static void
feed(const uint8_t *p, size_t n)
{
	bytes_in += n;
	for (size_t i = 0; i < n; i++) {
		if (shub_tl_byte(&tlrx, p[i]) == SHUB_TL_PACKET) {
			handle_packet(tlrx.buf, tlrx.len);
		}
	}
}

/*
 * MCU_RESP is not waited for unless shub_hello=1: on the S9 it reads 0 whatever the chip does (pulled up or down, hub
 * running or not), while everything else works when MCU_REQ is raised and the transfer follows 1 ms later. The
 * console's "nohello 0|1" changes it at run time.
 */
static uint32_t no_hello = 1;

static bool
hello(void)
{
	mcu_req(true);
	if (no_hello) {
		delay(1000);
		return true;
	}
	for (int count = 0; !mcu_resp(); count++) {
		if (count > HELLO_POLLS) {
			mcu_req(false);
			hello_fails++;
			return false;
		}
		sleep_ms(1);
		if (count % 20 == 19) {
			mcu_req(false);
			delay(1000);
			mcu_req(true);
			delay(1000);
		}
	}
	return true;
}

/* Read one frame and feed it to the transport layer. */
static bool
ssi_rx(void)
{
	uint16_t len, len2;

	memset(txf, 0, 3);
	txf[0] = SHUB_SSI_RX;
	if (!xfer(3)) {
		return false;
	}
	last_status = rxf[0];
	len = shub_le16(rxf + 1);
	if (len == 0) {
		len = 254;      /* zero length: read anyway, as bcm_ssi_rx() does */
	}
	if (len > SHUB_SSI_FRAME - 3) {
		len = SHUB_SSI_FRAME - 3;
	}
	memset(txf, 0, (size_t)len + 3);
	txf[0] = SHUB_SSI_RX;
	if (!xfer((uint32_t)len + 3)) {
		return false;
	}
	last_status = rxf[0];
	len2 = shub_le16(rxf + 1);
	if (len2 < len) {
		len = len2;
	}
	log_frame("rx", rxf, (size_t)len + 3);
	feed(rxf + 3, len);
	return true;
}

/* Send m bytes in a write frame, resending the remainder until the chip has taken all of it (three tries). Bytes the
 * chip returns meanwhile go to the transport layer. */
static bool
ssi_tx(const uint8_t *p, size_t m)
{
	uint8_t pending[SHUB_SSI_PAYLOAD];
	size_t left = m;

	if (m > sizeof(pending)) {
		return false;
	}
	memcpy(pending, p, m);
	if (shub_ssi_room(last_status) != 0 && m > shub_ssi_room(last_status)) {
		delay(1000);    /* as bcm_rxtx_work_func(): chip short of buffer room; wait 1 ms, then send anyway */
	}
	for (int tries = 0; tries < 3 && left > 0; tries++) {
		uint16_t nread, taken;

		txf[0] = SHUB_SSI_TX;
		txf[1] = (uint8_t)left;
		txf[2] = (uint8_t)(left >> 8);
		memcpy(txf + 3, pending, left);
		txf[3 + left] = 0;
		txf[4 + left] = 0;
		if (!xfer((uint32_t)left + 5)) {
			return false;
		}
		log_frame("tx", txf, left + 5);
		last_status = rxf[0];
		nread = shub_le16(rxf + 1);
		taken = shub_le16(rxf + 3 + left);
		if (nread > 0) {
			feed(rxf + 3, nread < left ? nread : left);
		}
		bytes_out += taken <= left ? taken : 0;
		if (taken >= left) {
			left = 0;
			break;
		}
		fc_retries++;
		memmove(pending, pending + taken, left - taken);
		left -= taken;
	}
	return left == 0;
}

/*
 * One SSI exchange: raise MCU_REQ and, with shub_hello=1, wait for MCU_RESP (up to 100 ms, pulsing MCU_REQ every 20
 * polls). While HOST_REQ is high read one frame at a time ([0x50][0 0] -> [status][len], then len + 3 bytes). Send
 * the bytes in write frames ([0x58][len][bytes][0 0]); the chip returns in the last two how many it took. Then drop
 * MCU_REQ. After bcm_gps_spi.c: bcm477x_hello/_bye, bcm_ssi_rx/_tx.
 */
static bool
exchange(const uint8_t *p, size_t n)
{
	bool ok = true;
	int frames = 0;

	if (!hello()) {
		return false;
	}
	while (ok && host_req() && frames++ < 64) {
		ok = ssi_rx() && pay_ack();
	}
	for (size_t at = 0; ok && at < n;) {
		size_t chunk = n - at > SHUB_SSI_PAYLOAD - 8 ? SHUB_SSI_PAYLOAD - 8 : n - at;

		ok = ssi_tx(p + at, chunk) && pay_ack();
		at += chunk;
		while (ok && host_req() && frames++ < 64) {
			ok = ssi_rx() && pay_ack();
		}
	}
	mcu_req(false);
	return ok;
}

/*
 * Read what the chip has for `ms`, or until done() is true, checking HOST_REQ every idle_us: 0.5 ms while the firmware
 * loads and answers are awaited, 4 ms once the sensors run (samples come every 20 ms or slower).
 */
static uint32_t idle_us = 500;

static void
pump(uint32_t ms, bool (*done)(void))
{
	uint64_t deadline = deadline_ms(ms);

	do {
		if (done != NULL && done()) {
			return;
		}
		if (host_req()) {
			(void)exchange(NULL, 0);
		} else if (idle_us >= 1000) {
			delay_for_interval(idle_us, NSEC_PER_USEC);
		} else {
			delay(idle_us);
		}
	} while (mach_absolute_time() < deadline);
}

static struct {
	uint8_t tx_seq, tx_rel;                 /* next to send */
	uint8_t rx_seq, rx_rel;                 /* last received */
	bool rx_rel_any;
	uint32_t unacked;                       /* payload packets received and not yet acknowledged */
	uint8_t rel_acked;                      /* last RELIABLE_ACK from the hub */
	bool rel_ack_seen, rel_nacked;
	bool synced;
	uint32_t packets, bad, internal, dup, out_of_order, rpcs, unknown_rpcs;
} tl;

static struct {
	bool seen;
	uint32_t asic, rom, patch;
} hubver;

static uint8_t sbuf[2 * SHUB_TL_MAX + 64];

static bool
send_packet(uint16_t flags, const uint8_t *payload, uint16_t size, bool reliable)
{
	struct shub_pkt p = { .seq = tl.tx_seq, .flags = flags, .payload = payload, .size = size };
	size_t n;

	if (!(flags & SHUB_F_INTERNAL)) {
		if (tl.unacked > 0) {
			p.flags |= SHUB_F_ACK;
			p.detail[0] = tl.rx_seq;
			tl.unacked = 0;
		}
		if (reliable) {
			p.flags |= SHUB_F_RELIABLE;
			p.detail[1] = tl.tx_rel;
		}
	}
	n = shub_tl_build(sbuf, sizeof(sbuf), &p);
	if (n == 0) {
		return false;
	}
	tl.tx_seq++;
	return exchange(sbuf, n);
}

/*
 * Acknowledgement owed to the hub. Recorded while a frame is parsed, because the frame buffer is still in use, and
 * sent by exchange() once the read is done. A later one replaces an earlier one: each acknowledges the latest packet.
 */
static struct {
	bool want;
	uint16_t flags;
	uint8_t rel;
} owed;

static void
send_ack(uint16_t flags, uint8_t rel)
{
	owed.want = true;
	owed.flags = flags;
	owed.rel = rel;
}

static bool
pay_ack(void)
{
	struct shub_pkt p = { .seq = tl.tx_seq, .flags = (uint16_t)(SHUB_F_ACK | owed.flags) };
	uint8_t b[32];
	size_t n;

	if (!owed.want) {
		return true;
	}
	owed.want = false;
	p.detail[0] = tl.rx_seq;
	p.detail[2] = owed.rel;
	p.detail[3] = owed.rel;
	tl.unacked = 0;
	n = shub_tl_build(b, sizeof(b), &p);
	if (n == 0) {
		return false;
	}
	tl.tx_seq++;
	return ssi_tx(b, n);
}

static void deliver_rpcs(const uint8_t *p, size_t n);

static void
handle_internal(const struct shub_pkt *p)
{
	tl.internal++;
	if (p->size >= 8 && p->payload[0] == SHUB_TLI_SEQ_RESPONSE) {
		tl.tx_seq = (uint8_t)(p->payload[2] + 1);
		tl.tx_rel = (uint8_t)(p->payload[4] + 1);
		tl.rx_seq = p->payload[5];
		tl.rx_rel = (uint8_t)(p->payload[6] - 1);
		tl.rx_rel_any = true;
		tl.synced = true;
		say("shub: sync answered: %02x %02x %02x %02x %02x %02x %02x %02x (next seq %u, next reliable %u)\n",
		    p->payload[0], p->payload[1], p->payload[2], p->payload[3], p->payload[4], p->payload[5], p->payload[6],
		    p->payload[7], tl.tx_seq, tl.tx_rel);
	} else if (tl.internal <= 8) {
		say("shub: an internal packet, type %u, %u bytes\n", p->size ? p->payload[0] : 0xff, p->size);
	}
}

static void
handle_packet(const uint8_t *raw, size_t len)
{
	struct shub_pkt p;
	int r = shub_tl_parse(raw, len, &p);

	if (r != SHUB_TL_OK) {
		if (tl.bad++ < 8) {
			say("shub: a packet refused (%s, %zu bytes)\n", r == SHUB_TL_CRC ? "CRC" : r == SHUB_TL_LENGTH ? "length" :
			    "short", len);
		}
		return;
	}
	tl.packets++;
	if (p.flags & SHUB_F_RELIABLE_ACK) {
		tl.rel_acked = p.detail[2];
		tl.rel_ack_seen = true;
	}
	if (p.flags & SHUB_F_RELIABLE_NACK) {
		tl.rel_nacked = true;
	}
	if (p.flags & SHUB_F_INTERNAL) {
		handle_internal(&p);
		return;
	}
	tl.rx_seq = p.seq;
	if (p.flags & SHUB_F_RELIABLE) {
		uint8_t rel = p.detail[1];

		if (tl.rx_rel_any && rel == tl.rx_rel) {
			tl.dup++;
			send_ack(SHUB_F_RELIABLE_ACK, rel);
			return;
		}
		if (tl.rx_rel_any && rel != (uint8_t)(tl.rx_rel + 1)) {
			tl.out_of_order++;
			send_ack(SHUB_F_RELIABLE_NACK, tl.rx_rel);
			return;
		}
		tl.rx_rel = rel;
		tl.rx_rel_any = true;
		send_ack(SHUB_F_RELIABLE_ACK, rel);
	} else if (p.size > 0 && ++tl.unacked >= ACK_EVERY) {
		send_ack(0, 0);
	}
	if (p.size > 0) {
		deliver_rpcs(p.payload, p.size);
	}
}

static bool
synced(void)
{
	return tl.synced;
}

/*
 * ROM entry: 36 bytes of 0x80 (autobaud), 100 ms, then the transport layer's sync: an INTERNAL "sequence request"
 * every 100 ms, six times, the whole thing up to five times. The version is a reliable RPC 0x00, answered by RPC 0x03
 * [u32 ASIC][u32 ROM][u32 patch level] (the Tab Active3's notes on lhd, section 6.3).
 */
static bool
autobaud_and_sync(const char *who)
{
	uint8_t burst[AUTOBAUD_BYTES];

	memset(burst, 0x80, sizeof(burst));
	tl.synced = false;
	for (int a = 0; a < AUTOBAUD_TRIES; a++) {
		if (!exchange(burst, sizeof(burst))) {
			say("shub: %s: the autobaud burst not sent (MCU_RESP %s)\n", who, hello_fails ? "never came" : "came");
		}
		pump(AUTOBAUD_MS, NULL);
		for (int s = 1; s <= SYNC_TRIES; s++) {
			uint8_t req[2] = { SHUB_TLI_SEQ_REQUEST, (uint8_t)s };

			(void)send_packet(SHUB_F_INTERNAL | SHUB_F_IGNORE_SEQID, req, 2, false);
			pump(SYNC_EVERY_MS, synced);
			if (tl.synced) {
				return true;
			}
		}
		say("shub: %s: no answer to the sync (try %d of %d; %llu bytes read, %u packets, %u refused, %llu garbage; "
		    "status %02x, HOST_REQ %u, MCU_RESP %u)\n", who, a + 1, AUTOBAUD_TRIES, bytes_in, tl.packets, tl.bad,
		    (unsigned long long)tlrx.garbage, last_status, host_req(), mcu_resp());
	}
	return false;
}

/* Send a reliable packet, resending until it is acknowledged or the tries run out. */
static bool
send_reliable(const uint8_t *payload, uint16_t size)
{
	uint8_t rel = tl.tx_rel;

	for (int t = 0; t < RELIABLE_TRIES; t++) {
		uint64_t deadline = deadline_ms(RELIABLE_MS);

		tl.rel_ack_seen = false;
		tl.rel_nacked = false;
		tl.tx_rel = rel;
		if (!send_packet(0, payload, size, true)) {
			continue;
		}
		while (mach_absolute_time() < deadline) {
			if (tl.rel_ack_seen && tl.rel_acked == rel) {
				tl.tx_rel = (uint8_t)(rel + 1);
				return true;
			}
			if (tl.rel_nacked) {
				break;
			}
			if (host_req()) {
				(void)exchange(NULL, 0);
			} else {
				delay(200);
			}
		}
	}
	tl.tx_rel = (uint8_t)(rel + 1);
	return false;
}

static struct shub_ssp_rx ssprx;
static void ssp_stream(void);

static void
text_rpc(const char *what, const uint8_t *d, uint16_t n)
{
	char text[160];
	size_t k = 0;

	for (uint16_t i = 0; i < n && k + 1 < sizeof(text); i++) {
		text[k++] = d[i] >= 0x20 && d[i] < 0x7f ? (char)d[i] : '.';
	}
	text[k] = 0;
	say("shub: hub %s: %s\n", what, text);
}

static void
deliver_rpcs(const uint8_t *p, size_t n)
{
	size_t at = 0;
	uint16_t id, len;
	const uint8_t *d;
	int r;

	while ((r = shub_rpc_next(p, n, &at, &id, &d, &len)) == 1) {
		tl.rpcs++;
		switch (id) {
		case SHUB_RPC_VERSION:
			if (len >= 12) {
				hubver.asic = shub_le32(d);
				hubver.rom = shub_le32(d + 4);
				hubver.patch = shub_le32(d + 8);
				hubver.seen = true;
			}
			break;
		case SHUB_RPC_SENSOR_RSP:
			if (len >= 2 && shub_le16(d) <= len - 2) {
				shub_ssp_rx_add(&ssprx, d + 2, shub_le16(d));
				ssp_stream();
			}
			break;
		case SHUB_RPC_LOG:
		case SHUB_RPC_LOG2:
			if (len >= 2) {
				text_rpc("log", d + 2, (uint16_t)(len - 2));
			}
			break;
		case SHUB_RPC_EXCEPTION:
		case SHUB_RPC_EXCEPTION_TEXT:
			text_rpc("EXCEPTION", d, len);
			break;
		default:
			if (tl.unknown_rpcs++ < 16) {
				say("shub: RPC %x, %u bytes: %02x %02x %02x %02x\n", id, len, len > 0 ? d[0] : 0, len > 1 ? d[1] : 0,
				    len > 2 ? d[2] : 0, len > 3 ? d[3] : 0);
			}
			break;
		}
	}
	if (r < 0 && tl.bad++ < 8) {
		say("shub: a packet's RPCs cut short (%zu bytes)\n", n);
	}
}

static bool
version_seen(void)
{
	return hubver.seen;
}

static bool
ask_version(const char *who)
{
	static const uint8_t req[2] = { SHUB_RPC_VERSION_REQ, 0 };

	hubver.seen = false;
	for (int t = 0; t < VERSION_TRIES; t++) {
		if (!send_reliable(req, sizeof(req))) {
			say("shub: %s: the version request not acknowledged (try %d)\n", who, t + 1);
		}
		pump(VERSION_MS, version_seen);
		if (hubver.seen) {
			say("shub: %s: ASIC %08x, ROM %08x, patch level %08x\n", who, hubver.asic, hubver.rom, hubver.patch);
			return true;
		}
	}
	say("shub: %s: no version\n", who);
	return false;
}

/*
 * Each write record goes as RPC 0x05 [u32 address][u16 n][data] in a reliable packet, acknowledged (RELIABLE_ACK)
 * before the next. Then RPC 0x2E [u32 PatchAddress] as a plain packet, 40 ms, autobaud and sync again, and the version
 * request again: it now returns the application's.
 */
static bool
load_patch(void)
{
	const uint8_t *data, *patch = NULL;
	uint64_t size = 0;
	struct shub_patch_info info;
	size_t at = 0;
	uint32_t addr, sent = 0;
	uint16_t n;
	uint8_t pay[SHUB_TL_MAX];
	uint64_t t0 = now_ns();

	for (int tries = 0; tries < 20 && patch == NULL; tries++) {
		patch = hakios_image_file("fw", PATCH_FILE, &size, NULL);
		if (patch == NULL) {
			sleep_ms(500);
		}
	}
	if (patch == NULL) {
		say("shub: 4: no fw/%s in the small image (devices/starlte/firmware/sensorhub/, userland/build.sh "
		    "DEVICE=starlte)\n", PATCH_FILE);
		return false;
	}
	if (!shub_patch_check(patch, (size_t)size, &info)) {
		say("shub: 4: fw/%s (%llu bytes) is not a patch of writes only\n", PATCH_FILE, size);
		return false;
	}
	say("shub: 4: fw/%s: %u writes, %u bytes, %08x-%08x; sending\n", PATCH_FILE, info.records, info.bytes, info.lo,
	    info.hi);
	state = "loading";
	while (shub_patch_next(patch, (size_t)size, &at, &addr, &data, &n) == 1) {
		uint8_t body[4 + 2 + SHUB_PATCH_MAX_RECORD];
		size_t k;

		shub_put32(body, addr);
		body[4] = (uint8_t)n;
		body[5] = (uint8_t)(n >> 8);
		memcpy(body + 6, data, n);
		k = shub_rpc_put(pay, SHUB_RPC_WRITE_MEM, body, (uint16_t)(6 + n));
		if (!send_reliable(pay, (uint16_t)k)) {
			say("shub: 4: write %u of %u (%08x, %u bytes) not acknowledged; stopping\n", sent + 1, info.records, addr,
			    n);
			return false;
		}
		sent++;
		if (sent % 100 == 0) {
			say("shub: 4: %u of %u written\n", sent, info.records);
		}
	}
	say("shub: 4: all %u written in %llu ms; starting it at %08x\n", sent, (now_ns() - t0) / NSEC_PER_MSEC, PATCH_ENTRY);
	{
		uint8_t entry[4];

		shub_put32(entry, PATCH_ENTRY);
		at = shub_rpc_put(pay, SHUB_RPC_START_APP, entry, 4);
		(void)send_packet(0, pay, (uint16_t)at, false);
	}
	sleep_ms(START_MS);
	memset(&tl, 0, sizeof(tl));
	memset(&tlrx, 0, sizeof(tlrx));
	if (!autobaud_and_sync("4 (the application)")) {
		return false;
	}
	return ask_version("4 (the application)");
}

/*
 * SSP: 9-byte instructions go in RPC 0x20 [u16 n][bytes]; the hub's answers and data frames come in RPC 0x21
 * (ssp_bbd.c, ssp_i2c.c, ssp_data.c). Reliable packets from the hub are acknowledged at once, plain ones every
 * ACK_EVERY packets.
 */
static struct {
	uint32_t init_done, frames, frame_errors, unexpected;
	bool answered;
	uint16_t answer_opts;
	uint8_t answer[16];
	uint16_t answer_len;
	uint64_t found;                 /* SENSOR_SCANNING's bitmap */
	uint32_t fw_rev;
	uint8_t accel_range;
	uint64_t enabled;
} ssp;

static LCK_GRP_DECLARE(shub_lck_grp, "hakios_ssp");
static LCK_MTX_DECLARE(shub_lock, &shub_lck_grp);       /* last[], the queue: hub thread and readers */

static struct {
	uint8_t data[24];
	uint8_t size;
	uint64_t count, hub_time, at_ns;
} last[SSP_SENSORS];

static void post_sample(const struct shub_sample *s);

static void
on_sample(void *ctx, const struct shub_sample *s)
{
	(void)ctx;
	lck_mtx_lock(&shub_lock);
	memcpy(last[s->sensor].data, s->data, s->size);
	last[s->sensor].size = s->size;
	last[s->sensor].count++;
	last[s->sensor].hub_time = s->time;
	last[s->sensor].at_ns = now_ns();
	lck_mtx_unlock(&shub_lock);
	post_sample(s);
}

static void
on_text(void *ctx, const uint8_t *p, size_t n)
{
	(void)ctx;
	text_rpc("says", p, (uint16_t)(n > 150 ? 150 : n));
}

static void
ssp_stream(void)
{
	uint16_t opts;
	const uint8_t *d;
	size_t n, used;
	int k;

	while ((k = shub_ssp_rx_peek(&ssprx, &opts, &d, &n, &used)) != SSP_RX_NONE) {
		if (k == SSP_RX_DATA) {
			struct shub_frame_counts c = { 0 };
			int r = shub_ssp_frame(d, n, &c, NULL, on_sample, on_text);

			ssp.frames++;
			ssp.init_done += c.init_done;
			if (r != SSP_FRAME_OK && ssp.frame_errors++ < 8) {
				size_t at = c.stopped_at;

				char hex[3 * 64 + 1];
				size_t k = 0;

				for (size_t i = 0; i < n && i < 64; i++) {
					k += (size_t)snprintf(hex + k, sizeof(hex) - k, "%02x ", d[i]);
				}
				hex[k] = 0;
				say("shub: a data frame of %zu bytes stopped at byte %zu (%s): %s\n", n, at,
				    r == SSP_FRAME_SHORT ? "cut short" : "a record or sensor it does not know", hex);
			}
		} else {
			ssp.answered = true;
			ssp.answer_opts = opts;
			ssp.answer_len = (uint16_t)(n < sizeof(ssp.answer) ? n : sizeof(ssp.answer));
			memcpy(ssp.answer, d, ssp.answer_len);
			ssprx.want_read = 0;
		}
		shub_ssp_rx_consume(&ssprx, used);
	}
}

static bool
ssp_answered(void)
{
	return ssp.answered;
}

static bool
ssp_send(const uint8_t *p, uint16_t n)
{
	uint8_t pay[16 + 2 + 64];
	uint8_t body[2 + 64];
	size_t k;

	if (n > 64) {
		return false;
	}
	if (req_len) {
		body[0] = (uint8_t)n;
		body[1] = (uint8_t)(n >> 8);
		memcpy(body + 2, p, n);
		k = shub_rpc_put(pay, (uint16_t)req_rpc, body, (uint16_t)(n + 2));
	} else {
		k = shub_rpc_put(pay, (uint16_t)req_rpc, p, n);
	}
	return send_packet(0, pay, (uint16_t)k, false);
}

/*
 * One SSP transaction, after ssp_spi_sync() in ssp_i2c.c. Send the 9-byte header. READ: wait for the answer and copy
 * len bytes into buf. WRITE: wait for the hub's answer, then send len bytes from buf. With len 0 only the header
 * is sent.
 */
static bool
ssp_cmd(uint8_t cmd, uint16_t type, uint32_t arg, uint8_t *buf, uint16_t len)
{
	uint8_t h[9];

	shub_ssp_header(h, cmd, len, type, arg);
	ssp.answered = false;
	ssprx.want_read = type == SSP_AP2HUB_READ ? len : 0;
	if (!ssp_send(h, sizeof(h))) {
		return false;
	}
	if (len == 0) {
		return true;
	}
	pump(SSP_MS, ssp_answered);
	if (!ssp.answered || ssp.answer_opts != type) {
		say("shub: SSP %02x: %s (options %x)\n", cmd, ssp.answered ? "another answer" : "no answer",
		    ssp.answered ? ssp.answer_opts : 0);
		ssprx.want_read = 0;
		return false;
	}
	if (type == SSP_AP2HUB_READ) {
		memcpy(buf, ssp.answer, len < ssp.answer_len ? len : ssp.answer_len);
		return true;
	}
	return ssp_send(buf, len);
}

static bool
init_seen(void)
{
	return ssp.init_done > 0;
}

/* Send the AP's time to the hub: header and 8 bytes in one packet, no go-ahead awaited (as bbd_send_timestamp()). */
static void
send_time(void)
{
	uint8_t m[9 + 8];
	uint64_t ns = now_ns();

	shub_ssp_header(m, SSP_CURRENT_TIMESTAMP, 8, SSP_AP2HUB_WRITE, 0);
	for (int i = 0; i < 8; i++) {
		m[9 + i] = (uint8_t)(ns >> (8 * i));
	}
	(void)ssp_send(m, sizeof(m));
}

static bool
ssp_init(void)
{
	uint8_t b[8] = { 0 };

	pump(INIT_DONE_MS, init_seen);
	say("shub: 5: %s\n", ssp.init_done ? "the hub's sensors are set up (record 0x0A)" :
	    "no \"sensor init done\" in 1 s (Samsung's driver goes on too)");
	if (!ssp_cmd(SSP_WHOAMI, SSP_AP2HUB_READ, 0, b, 1) || b[0] != SSP_WHOAMI_ANSWER) {
		say("shub: 5: WHOAMI: %02x (not 0x55): stopping\n", b[0]);
		return false;
	}
	say("shub: 5: WHOAMI 0x55\n");
	b[0] = (uint8_t)acc_position;
	b[1] = (uint8_t)acc_position;
	b[2] = MAG_POSITION;
	if (!ssp_cmd(SSP_SENSOR_FORMATION, SSP_AP2HUB_WRITE, 0, b, 3)) {
		say("shub: 5: the formation not taken\n");
	} else {
		say("shub: 5: the formation: accelerometer and gyroscope at %u, magnetometer at %u (%s)\n", acc_position,
		    MAG_POSITION, HAKIOS_DEVICE);
	}
	memset(b, 0, sizeof(b));
	if (ssp_cmd(SSP_SENSOR_SCANNING, SSP_AP2HUB_READ, 0, b, 8)) {
		ssp.found = (uint64_t)shub_le32(b) | (uint64_t)shub_le32(b + 4) << 32;
	}
	if (ssp_cmd(SSP_FIRMWARE_REV, SSP_AP2HUB_READ, 0, b, 4)) {
		ssp.fw_rev = shub_le32(b);
	}
	if (ssp_cmd(SSP_GET_ACCEL_RANGE, SSP_AP2HUB_READ, 0, b, 1)) {
		ssp.accel_range = b[0];
	}
	say("shub: 5: the sensors found: %016llx; firmware revision %u; accelerometer range %u\n", ssp.found, ssp.fw_rev,
	    ssp.accel_range);
	send_time();
	return ssp.found != 0;
}

static bool
sensor_on(uint8_t id, uint32_t ms)
{
	uint8_t b[10];

	shub_put32(b + 1, ms);
	b[0] = id;
	shub_put32(b + 5, 0);           /* no batching: every sample as it comes */
	b[9] = 0;
	if (!ssp_cmd(SSP_SENSOR_ADD, SSP_AP2HUB_WRITE, 0, b, sizeof(b))) {
		return false;
	}
	ssp.enabled |= 1ull << id;
	return true;
}

/*
 * The factory calibration, as Samsung's sync_sensor_state() (ssp_debug.c) sends it once the hub is up and before any
 * sensor is added: the gyroscope's (SSP 0xCD, 12 bytes: s32 x, y, z), then the accelerometer's (SSP 0xCE, 6 bytes: s16
 * x, y, z; the hub takes them off its samples). Only what shub_acc_cal= / shub_gyro_cal= give. If the hub does not take
 * the accelerometer's, convert() takes them off the samples to user space instead.
 */
static void
send_calibration(void)
{
	uint8_t b[12];

	if (gyro_cal_given) {
		if (!(ssp.found & (1ull << SSP_GYRO))) {
			say("shub: 6: no gyroscope found: its calibration not sent\n");
		} else {
			for (int i = 0; i < 3; i++) {
				shub_put32(b + 4 * i, (uint32_t)gyro_cal[i]);
			}
			say("shub: 6: gyroscope calibration %d %d %d: %s\n", gyro_cal[0], gyro_cal[1], gyro_cal[2],
			    ssp_cmd(SSP_SET_GYRO_CAL, SSP_AP2HUB_WRITE, 0, b, 12) ? "taken" : "NOT taken (not applied)");
		}
	}
	if (acc_cal_given) {
		bool taken;

		for (int i = 0; i < 3; i++) {
			b[2 * i] = (uint8_t)acc_cal[i];
			b[2 * i + 1] = (uint8_t)((uint32_t)acc_cal[i] >> 8);
		}
		taken = (ssp.found & (1ull << SSP_ACCEL)) && ssp_cmd(SSP_SET_ACCEL_CAL, SSP_AP2HUB_WRITE, 0, b, 6);
		acc_cal_subtract = !taken;
		say("shub: 6: accelerometer calibration %d %d %d: %s\n", acc_cal[0], acc_cal[1], acc_cal[2],
		    taken ? "taken by the hub" : "NOT taken: subtracted from the samples to user space instead");
	}
}

/* "X,Y,Z" (a boot-arg's text, empty if absent) into v[3]; false if absent or not three numbers */
static bool
parse_triple(const char *name, const char *text, int32_t v[3])
{
	const char *p = text;

	if (text[0] == 0) {
		return false;
	}
	for (int i = 0; i < 3; i++) {
		int sign = 1;
		int32_t n = 0;
		bool digits = false;

		if (*p == '-') {
			sign = -1;
			p++;
		} else if (*p == '+') {
			p++;
		}
		while (*p >= '0' && *p <= '9' && n < 100000000) {
			n = n * 10 + (*p++ - '0');
			digits = true;
		}
		if (!digits || (i < 2 && *p++ != ',') || (i == 2 && *p != 0)) {
			say("shub: %s=%s: not X,Y,Z: ignored\n", name, text);
			return false;
		}
		v[i] = sign * n;
	}
	return true;
}

#define QUEUE 256
static struct hakios_sensor_sample queue[QUEUE];
static uint32_t q_head, q_tail, q_lost;
static bool q_waiting;
static int q_readers;

/*
 * Set by the console's "sensors fake X Y Z S": for S seconds the accelerometer samples handed to user space read X, Y,
 * Z (counts, Android's axes and sign; 4096 is 1 g at the 8 g range) instead of the hub's, to try iOS's rotation on a
 * phone nobody is holding. The console's "sensors" still shows the hub's own samples.
 */
static volatile int32_t fake_v[3];
static volatile uint64_t fake_until_ns;

/* Convert a sample to the units of sys/hakios.sensors.h. False for sensors that are not passed to user space. */
static bool
convert(const struct shub_sample *s, struct hakios_sensor_sample *o)
{
	const uint8_t *d = s->data;

	memset(o, 0, sizeof(*o));
	o->time = mach_absolute_time();
	o->hub_ns = s->time;
	switch (s->sensor) {
	case SSP_ACCEL:
		/* Samsung's HAL: 4096 LSB per g at the 8 g range. scale is the range read in ssp_init(), 8 if not read. */
		o->type = HAKIOS_SENSOR_ACCEL;
		o->v[0] = (int16_t)shub_le16(d);
		o->v[1] = (int16_t)shub_le16(d + 2);
		o->v[2] = (int16_t)shub_le16(d + 4);
		o->scale = ssp.accel_range ? ssp.accel_range : 8;
		if (acc_cal_subtract) {
			o->v[0] -= acc_cal[0];
			o->v[1] -= acc_cal[1];
			o->v[2] -= acc_cal[2];
		}
		if (now_ns() < fake_until_ns) {
			o->v[0] = fake_v[0];
			o->v[1] = fake_v[1];
			o->v[2] = fake_v[2];
		}
		return true;
	case SSP_GYRO:
		o->type = HAKIOS_SENSOR_GYRO;
		o->v[0] = (int32_t)shub_le32(d);
		o->v[1] = (int32_t)shub_le32(d + 4);
		o->v[2] = (int32_t)shub_le32(d + 8);
		return true;
	case SSP_MAG:
		o->type = HAKIOS_SENSOR_MAG;
		o->v[0] = (int16_t)shub_le16(d);
		o->v[1] = (int16_t)shub_le16(d + 2);
		o->v[2] = (int16_t)shub_le16(d + 4);
		o->v[3] = d[6];
		return true;
	case SSP_PRESSURE:
		o->type = HAKIOS_SENSOR_PRESSURE;
		o->v[0] = (int32_t)shub_le32(d);
		o->v[1] = (int16_t)shub_le16(d + 4);
		return true;
	case SSP_LIGHT:
		o->type = HAKIOS_SENSOR_LIGHT;
		o->v[0] = (int32_t)shub_le32(d);      /* lux */
		o->v[1] = (int32_t)shub_le32(d + 4);  /* colour temperature */
		return true;
	case SSP_PROXIMITY:
		o->type = HAKIOS_SENSOR_PROXIMITY;
		o->v[0] = d[0];                         /* 1: something near */
		o->v[1] = shub_le16(d + 1);
		return true;
	default:
		return false;
	}
}

static void
post_sample(const struct shub_sample *s)
{
	struct hakios_sensor_sample o;

	if (!convert(s, &o)) {
		return;
	}
	lck_mtx_lock(&shub_lock);
	if (q_readers > 0) {
		if (q_head - q_tail >= QUEUE) {
			q_tail++;
			q_lost++;
		}
		queue[q_head++ % QUEUE] = o;
		if (q_waiting) {
			q_waiting = false;
			wakeup(&q_head);
		}
	}
	lck_mtx_unlock(&shub_lock);
}

static int
sensors_open(__unused dev_t dev, __unused int flags, __unused int devtype, __unused proc_t p)
{
	lck_mtx_lock(&shub_lock);
	q_readers++;
	q_tail = q_head;
	lck_mtx_unlock(&shub_lock);
	return 0;
}

static int
sensors_close(__unused dev_t dev, __unused int flags, __unused int devtype, __unused proc_t p)
{
	lck_mtx_lock(&shub_lock);
	if (q_readers > 0) {
		q_readers--;
	}
	lck_mtx_unlock(&shub_lock);
	return 0;
}

static int
sensors_read(__unused dev_t dev, struct uio *uio, __unused int ioflag)
{
	int error = 0;

	lck_mtx_lock(&shub_lock);
	while (q_head == q_tail) {
		q_waiting = true;
		error = msleep(&q_head, &shub_lock, PZERO | PCATCH, HAKIOS_SENSORS_NAME, NULL);
		if (error != 0) {
			lck_mtx_unlock(&shub_lock);
			return error;
		}
	}
	while (q_head != q_tail && uio_resid(uio) >= (user_ssize_t)sizeof(struct hakios_sensor_sample)) {
		struct hakios_sensor_sample o = queue[q_tail++ % QUEUE];

		lck_mtx_unlock(&shub_lock);
		error = uiomove((caddr_t)&o, sizeof(o), uio);
		lck_mtx_lock(&shub_lock);
		if (error != 0) {
			break;
		}
	}
	lck_mtx_unlock(&shub_lock);
	return error;
}

static const struct cdevsw sensors_cdevsw = {
	.d_open = sensors_open,
	.d_close = sensors_close,
	.d_read = sensors_read,
	.d_write = eno_rdwrt,
	.d_ioctl = eno_ioctl,
	.d_stop = eno_stop,
	.d_reset = eno_reset,
	.d_select = eno_select,
	.d_mmap = eno_mmap,
	.d_strategy = eno_strat,
	.d_type = 0,
};

static void
make_device(void)
{
	int major = cdevsw_add(-1, &sensors_cdevsw);

	if (major < 0 || devfs_make_node(makedev(major, 0), DEVFS_CHAR, UID_ROOT, GID_WHEEL, 0666,
	    HAKIOS_SENSORS_NAME) == NULL) {
		say("shub: /dev/%s could not be made\n", HAKIOS_SENSORS_NAME);
		return;
	}
	say("shub: /dev/%s made\n", HAKIOS_SENSORS_NAME);
}

static volatile uint64_t console_want_on, console_want_off;
static volatile bool console_cmd_pending;
static char console_cmd[64];
static void console_do(void);

static void
run(void)
{
	static const struct { uint8_t id; uint32_t ms; } slow[] = { { SSP_LIGHT, 200 }, { SSP_PROXIMITY, 200 },
		                                                   { SSP_PRESSURE, 200 } };
	uint32_t frames_at_time = 0;

	send_calibration();
	for (uint8_t id = 0; id < SSP_SENSORS; id++) {
		uint32_t ms = period_ms;

		if (id >= 32 || !(want_sensors & (1u << id)) || !(ssp.found & (1ull << id))) {
			continue;
		}
		for (unsigned i = 0; i < sizeof(slow) / sizeof(slow[0]); i++) {
			if (slow[i].id == id) {
				ms = slow[i].ms;
			}
		}
		say("shub: 6: sensor %u on, every %u ms: %s\n", id, ms, sensor_on(id, ms) ? "taken" : "NOT taken");
	}
	state = "running";
	idle_us = 4000;
	for (;;) {
		uint64_t on = console_want_on, off = console_want_off;

		if (on | off) {
			console_want_on = 0;
			console_want_off = 0;
			for (uint8_t id = 0; id < SSP_SENSORS; id++) {
				uint8_t b[5] = { id, 0, 0, 0, 0 };

				if (on & (1ull << id)) {
					say("shub: sensor %u on: %s\n", id, sensor_on(id, period_ms) ? "taken" : "NOT taken");
				}
				if ((off & (1ull << id)) && ssp_cmd(SSP_SENSOR_REMOVE, SSP_AP2HUB_WRITE, 0, b, 5)) {
					ssp.enabled &= ~(1ull << id);
					say("shub: sensor %u off\n", id);
				}
			}
		}
		if (console_cmd_pending) {
			console_do();
		}
		pump(100, NULL);
		if (ssp.frames - frames_at_time >= 3000) {
			frames_at_time = ssp.frames;
			send_time();
		}
	}
}

/* Run steps 2 to `steps`; also used again by the console's "retry". Returns false if a step stopped. */
static bool
from_step2(void)
{
	if (!power_on()) {
		return false;
	}
	if (!hello()) {
		say("shub: 2: MCU_RESP did not answer MCU_REQ in 100 ms (HOST_REQ %u)\n", host_req());
	} else {
		say("shub: 2: MCU_RESP answered; HOST_REQ %u\n", host_req());
		mcu_req(false);
	}
	(void)exchange(NULL, 0);
	say("shub: 2: %llu bytes read from the chip, status %02x\n", bytes_in, last_status);
	if (steps < 3) {
		return true;
	}
	state = "ROM";
	memset(&tl, 0, sizeof(tl));
	memset(&tlrx, 0, sizeof(tlrx));
	if (!autobaud_and_sync("3") || !ask_version("3")) {
		return false;
	}
	if (steps < 4 || !load_patch()) {
		return steps < 4;
	}
	state = "application";
	if (steps < 5 || !ssp_init()) {
		return steps < 5;
	}
	state = "SSP";
	if (steps >= 6) {
		run();
	}
	return true;
}

static void
shub_thread(void *param, __unused wait_result_t wr)
{
	steps = (uint32_t)(uintptr_t)param;
	say("shub: the S9's sensor hub, %u steps\n", steps);
	pmu = ml_io_map(PMU_PA, 0x5000);
	cmu_top = ml_io_map(CMU_TOP_PA, 0x4000);
	cmu_chub = ml_io_map(CMU_CHUB_PA, 0x4000);
	gpio_chub = ml_io_map(GPIO_CHUB_PA, 0x1000);
	alive = ml_io_map(GPIO_ALIVE_PA, 0x1000);
	peric0 = ml_io_map(GPIO_PERIC0_PA, 0x1000);
	if (!pmu || !cmu_top || !cmu_chub || !gpio_chub || !alive || !peric0) {
		say("shub: a block could not be mapped\n");
		goto out;
	}
	if (!hakios_acpm_init(say)) {
		say("shub: no ACPM: the hub's supply cannot be read\n");
	}
	if (!look() && !chub_on_ok) {
		goto out;
	}
	if (steps < 2) {
		goto done;
	}
	(void)from_step2();
done:
	if (keep == 0) {
		power_off("the steps are done (shub_keep=0)");
	}
out:
	say("shub: done: %llu bytes read, %llu sent, %u packets, %u refused, %u SPI errors, %u flow-control retries, %u "
	    "times MCU_RESP did not come; the console's \"sensors\" can look and try again\n", bytes_in, bytes_out,
	    tl.packets, tl.bad, spi_errors, fc_retries, hello_fails);
	if (!pmu || !alive) {
		thread_terminate(current_thread());
	}
	for (;;) {
		if (console_cmd_pending) {
			console_do();
		}
		sleep_ms(100);
	}
}

/*
 * Bring-up experiments from the console, run on the hub thread because ACPM and the SPI belong to it. The commands are
 * listed above hakios_ssp_console().
 */
static void
console_do(void)
{
	char a[sizeof(console_cmd)];
	uint8_t v = 0;

	strlcpy(a, console_cmd, sizeof(a));
	console_cmd_pending = false;    /* clear first: "retry" can end in run(), which also polls console_cmd_pending */

	if (strncmp(a, "pins", 4) == 0) {
		(void)look();
		say("sensors: MCU_RESP %u HOST_REQ %u now\n", mcu_resp(), host_req());
	} else if (strcmp(a, "ldos") == 0) {
		char line[200];
		size_t k = 0;

		/* S2MPS18 LDOn CTRL is 0x49 + n for n >= 3; LDO1 is 0x49, LDO2 0x4A (second one, 0x4B, not read). Read only. */
		for (unsigned n = 1; n <= 45; n++) {
			uint8_t reg = (uint8_t)(n == 1 ? 0x49 : n == 2 ? 0x4A : 0x49 + n);

			v = 0;
			(void)hakios_acpm_pmic_read(HAKIOS_ACPM_PMIC_PMIC, reg, &v);
			k += (size_t)snprintf(line + k, sizeof(line) - k, " %u:%02x", n, v);
			if (n % 15 == 0) {
				say("sensors: LDO CTRL%s\n", line);
				k = 0;
			}
		}
	} else if (strncmp(a, "ldo ", 4) == 0) {
		/* "ldo 0|1" is LDO37; "ldo N 0|1" is one of the LDOs that Samsung's tree keeps always-on and no other consumer
		 * uses (30, 32, 37, 42). Linux turns them on at boot; this boot may not have. */
		unsigned n = 37;
		bool on;
		const char *p = a + 4;

		if (p[0] >= '0' && p[0] <= '9' && p[1] >= '0' && p[1] <= '9') {
			n = (unsigned)(p[0] - '0') * 10 + (unsigned)(p[1] - '0');
			p += 2;
			while (*p == ' ') {
				p++;
			}
		}
		on = *p == '1';
		if (n != 30 && n != 32 && n != 37 && n != 42) {
			say("sensors: LDO%u is not one this may switch (30, 32, 37, 42)\n", n);
		} else {
			(void)hakios_acpm_pmic_update(HAKIOS_ACPM_PMIC_PMIC, (uint8_t)(0x49 + n), on ? LDO_ON : 0, LDO_ON);
			(void)hakios_acpm_pmic_read(HAKIOS_ACPM_PMIC_PMIC, (uint8_t)(0x49 + n), &v);
			say("sensors: LDO%u CTRL %02x; MCU_RESP %u HOST_REQ %u\n", n, v, mcu_resp(), host_req());
		}
	} else if (strncmp(a, "nstdby ", 7) == 0) {
		pin_output(peric0, BANK_GPG0, PIN_NSTDBY, a[7] == '1');
		say("sensors: NSTDBY %u; MCU_RESP %u HOST_REQ %u\n", pin_dat(peric0, BANK_GPG0, PIN_NSTDBY), mcu_resp(),
		    host_req());
	} else if (strncmp(a, "req ", 4) == 0) {
		pin_output(gpio_chub, BANK_GPH0, PIN_MCU_REQ, a[4] == '1');
		delay(2000);
		say("sensors: MCU_REQ %u; MCU_RESP %u HOST_REQ %u\n", pin_dat(gpio_chub, BANK_GPH0, PIN_MCU_REQ), mcu_resp(),
		    host_req());
	} else if (strcmp(a, "hello") == 0) {
		bool ok = hello();

		say("sensors: hello %s; MCU_RESP %u HOST_REQ %u\n", ok ? "answered" : "NOT answered", mcu_resp(), host_req());
		mcu_req(false);
	} else if (strncmp(a, "r ", 2) == 0 || strncmp(a, "w ", 2) == 0) {
		/* "r BLOCK OFF", "w BLOCK OFF VALUE": BLOCK is gpio (CHUB's GPIO), alive or peric0 (their GPIO blocks), all
		 * writable, or cmu (CMU_CHUB) or pmu, read only */
		char block[12] = "";
		const char *p = a + 2;
		size_t k = 0;
		uint32_t off = 0, val = 0;
		vm_offset_t base = 0;
		bool writable = false;

		while (*p != ' ' && *p != 0 && k + 1 < sizeof(block)) {
			block[k++] = *p++;
		}
		block[k] = 0;
		while (*p == ' ') {
			p++;
		}
		off = (uint32_t)strtoul(p, (char **)&p, 16);
		while (*p == ' ') {
			p++;
		}
		val = (uint32_t)strtoul(p, NULL, 16);
		if (strcmp(block, "gpio") == 0) {
			base = gpio_chub, writable = true;
		} else if (strcmp(block, "alive") == 0) {
			base = alive, writable = true;
		} else if (strcmp(block, "peric0") == 0) {
			base = peric0, writable = true;
		} else if (strcmp(block, "cmu") == 0) {
			base = cmu_chub;
		} else if (strcmp(block, "pmu") == 0) {
			base = pmu;
		}
		if (base == 0 || off >= 0x1000 || (off & 3)) {
			say("sensors: r/w gpio|alive|peric0|cmu|pmu OFF(hex, under 0x1000) [VALUE]\n");
		} else {
			if (a[0] == 'w' && writable) {
				wr(base, off, val);
			}
			say("sensors: %s +%03x = %08x%s; MCU_RESP %u HOST_REQ %u\n", block, off, rd(base, off),
			    a[0] == 'w' && !writable ? " (read only)" : "", mcu_resp(), host_req());
		}
	} else if (strncmp(a, "tap ", 4) == 0 || strncmp(a, "swipe ", 6) == 0) {
		/*
		 * "tap X Y", "swipe X1 Y1 X2 Y2": inject a finger into the touch queue (hakios_touch_post), positions in
		 * thousandths of the screen's width and height. For opening an app to look at its rotation with nobody at
		 * the phone.
		 */
		long v[4] = { 0, 0, 0, 0 };
		const char *p = a + (a[0] == 't' ? 4 : 6);
		int steps = a[0] == 't' ? 1 : 12;

		for (int i = 0; i < 4 && *p != 0; i++) {
			v[i] = strtol(p, (char **)&p, 10);
			while (*p == ' ') {
				p++;
			}
		}
		if (a[0] == 't') {
			v[2] = v[0];
			v[3] = v[1];
		}
		for (int i = 0; i < 4; i++) {
			v[i] = v[i] < 0 ? 0 : v[i] > 1000 ? 65535 : v[i] * 65535 / 1000;
		}
		hakios_touch_post(HAKIOS_TOUCH_DOWN, 0, (uint32_t)v[0], (uint32_t)v[1]);
		for (int i = 1; i <= steps; i++) {
			sleep_ms(16);
			hakios_touch_post(HAKIOS_TOUCH_MOVE, 0, (uint32_t)(v[0] + (v[2] - v[0]) * i / steps),
			    (uint32_t)(v[1] + (v[3] - v[1]) * i / steps));
		}
		sleep_ms(a[0] == 't' ? 60 : 16);
		hakios_touch_post(HAKIOS_TOUCH_UP, 0, (uint32_t)v[2], (uint32_t)v[3]);
		say("sensors: %s done\n", a[0] == 't' ? "tap" : "swipe");
	} else if (strncmp(a, "nohello ", 8) == 0) {
		no_hello = a[8] == '1';
		say("sensors: MCU_RESP %s\n", no_hello ? "not waited for" : "waited for");
	} else if (strcmp(a, "raw") == 0) {
		/* read one frame with MCU_REQ raised, whatever MCU_RESP says, to see what the chip clocks out */
		mcu_req(true);
		delay(2000);
		memset(txf, 0, 8);
		txf[0] = SHUB_SSI_RX;
		(void)xfer(8);
		mcu_req(false);
		say("sensors: raw read: %02x %02x %02x %02x %02x %02x %02x %02x (MCU_RESP %u)\n", rxf[0], rxf[1], rxf[2], rxf[3],
		    rxf[4], rxf[5], rxf[6], rxf[7], mcu_resp());
	} else if (strcmp(a, "retry") == 0) {
		if (steps < 2) {
			steps = 6;
		}
		say("sensors: steps 2 to %u again\n", steps);
		(void)from_step2();
	}
}

void
hakios_ssp_init(void)
{
	uint32_t n = 0;
	thread_t thread;

	if (!PE_parse_boot_argn("shub", &n, sizeof(n)) || n == 0) {
		return;
	}
	if (pe_arm_get_soc_base_phys() != EXYNOS_SOC_BASE_PA) {
		return;                 /* Exynos 9810 only */
	}
	if (n > 6) {
		n = 6;
	}
	PE_parse_boot_argn("shub_keep", &keep, sizeof(keep));
	PE_parse_boot_argn("shub_log", &log_frames, sizeof(log_frames));
	PE_parse_boot_argn("shub_div", &div_ratio, sizeof(div_ratio));
	PE_parse_boot_argn("shub_chub_on", &chub_on_ok, sizeof(chub_on_ok));
	PE_parse_boot_argn("shub_sensors", &want_sensors, sizeof(want_sensors));
	PE_parse_boot_argn("shub_ms", &period_ms, sizeof(period_ms));
	PE_parse_boot_argn("shub_req_rpc", &req_rpc, sizeof(req_rpc));
	PE_parse_boot_argn("shub_req_len", &req_len, sizeof(req_len));
	acc_position = strcmp(HAKIOS_DEVICE, "star2lte") == 0 ? ACC_POSITION_S9PLUS : ACC_POSITION_S9;
	PE_parse_boot_argn("shub_acc_pos", &acc_position, sizeof(acc_position));
	{
		char text[48] = "";

		if (PE_parse_boot_arg_str("shub_acc_cal", text, sizeof(text))) {
			acc_cal_given = parse_triple("shub_acc_cal", text, acc_cal);
		}
		text[0] = 0;
		if (PE_parse_boot_arg_str("shub_gyro_cal", text, sizeof(text))) {
			gyro_cal_given = parse_triple("shub_gyro_cal", text, gyro_cal);
		}
	}
	if (acc_position > 7) {
		acc_position = strcmp(HAKIOS_DEVICE, "star2lte") == 0 ? ACC_POSITION_S9PLUS : ACC_POSITION_S9;
	}
	{
		uint32_t h = 0;

		if (PE_parse_boot_argn("shub_hello", &h, sizeof(h))) {
			no_hello = h == 0;
		}
	}
	if (period_ms < 5) {
		period_ms = 5;
	}
	if (n >= 6) {
		make_device();          /* before the hub thread starts, so backboardd finds it at launch */
	}
	if (kernel_thread_start(shub_thread, (void *)(uintptr_t)n, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}

static const char *
sensor_name(uint8_t id)
{
	switch (id) {
	case SSP_ACCEL: return "accelerometer";
	case SSP_GYRO: return "gyroscope";
	case SSP_MAG_UNCAL: return "magnetometer (uncalibrated)";
	case SSP_MAG_RAW: return "magnetometer (raw)";
	case SSP_MAG: return "magnetometer";
	case SSP_PRESSURE: return "barometer";
	case SSP_PROXIMITY: return "proximity";
	case SSP_LIGHT: return "light";
	case SSP_PROXIMITY_RAW: return "proximity (raw)";
	case SSP_LIGHT_CCT: return "light (CCT)";
	default: return "sensor";
	}
}

/*
 * USB console "sensors" command (called from hakios.dwc3.c):
 *   sensors                the hub's state, the sensors found and on, and the last sample of each
 *   sensors on N | off N   turn sensor N (Samsung's number) on, at shub_ms, or off
 *   sensors fake X Y Z S   for S seconds, accelerometer samples to user space read X Y Z (counts, Android's axes;
 *                          4096 = 1 g); S outside 1..3599 means 10
 *   sensors tap X Y | swipe X1 Y1 X2 Y2
 *                          synthetic touch, run on the hub thread
 *   sensors pins | ldos | ldo [N] 0|1 | nstdby 0|1 | req 0|1 | hello | nohello 0|1 | raw | r BLOCK OFF |
 *           w BLOCK OFF V | retry
 *                          bring-up experiments, run on the hub thread (console_do())
 */
void
hakios_ssp_console(const char *arg)
{
	uint64_t t = now_ns();

	if (steps == 0) {
		say("sensors: the sensor hub is not started (shub=N)\n");
		return;
	}
	if (strncmp(arg, "tap ", 4) == 0 || strncmp(arg, "swipe ", 6) == 0 || strncmp(arg, "pins", 4) == 0 ||
	    strncmp(arg, "ldo", 3) == 0 || strncmp(arg, "r ", 2) == 0 ||
	    strncmp(arg, "w ", 2) == 0 || strncmp(arg, "nohello ", 8) == 0 || strcmp(arg, "raw") == 0 || strncmp(arg, "nstdby ", 7) == 0 ||
	    strncmp(arg, "req ", 4) == 0 || strcmp(arg, "hello") == 0 || strcmp(arg, "retry") == 0) {
		if (console_cmd_pending) {
			say("sensors: busy with \"%s\"\n", console_cmd);
			return;
		}
		strlcpy(console_cmd, arg, sizeof(console_cmd));
		console_cmd_pending = true;
		return;
	}
	if (strncmp(arg, "fake ", 5) == 0) {
		long v[4] = { 0, 0, 0, 10 };
		const char *p = arg + 5;

		for (int i = 0; i < 4 && *p != 0; i++) {
			v[i] = strtol(p, (char **)&p, 10);
			while (*p == ' ') {
				p++;
			}
		}
		fake_v[0] = (int32_t)v[0];
		fake_v[1] = (int32_t)v[1];
		fake_v[2] = (int32_t)v[2];
		fake_until_ns = now_ns() + (uint64_t)(v[3] > 0 && v[3] < 3600 ? v[3] : 10) * NSEC_PER_SEC;
		say("sensors: the accelerometer says %d %d %d to user space for %ld s\n", fake_v[0], fake_v[1], fake_v[2],
		    v[3]);
		return;
	}
	if (strncmp(arg, "on ", 3) == 0 || strncmp(arg, "off ", 4) == 0) {
		bool on = arg[1] == 'n';
		uint32_t id = 0;
		const char *p = arg + (on ? 3 : 4);

		while (*p >= '0' && *p <= '9') {
			id = id * 10 + (uint32_t)(*p++ - '0');
		}
		if (id >= SSP_SENSORS) {
			say("sensors: no sensor %u\n", id);
			return;
		}
		if (on) {
			console_want_on |= 1ull << id;
		} else {
			console_want_off |= 1ull << id;
		}
		say("sensors: sensor %u %s asked\n", id, on ? "on" : "off");
		return;
	}
	say("sensors: hub %s; ROM %08x/%08x, application's version %08x; found %016llx, on %016llx; %u data frames (%u "
	    "bad), %llu bytes in, %llu out, %u SPI errors, %u queued to user space lost\n", state, hubver.asic, hubver.rom,
	    hubver.patch, ssp.found, ssp.enabled, ssp.frames, ssp.frame_errors, bytes_in, bytes_out, spi_errors, q_lost);
	lck_mtx_lock(&shub_lock);
	for (uint8_t id = 0; id < SSP_SENSORS; id++) {
		char vals[96];
		const uint8_t *d = last[id].data;
		uint64_t age = last[id].count ? (t - last[id].at_ns) / NSEC_PER_MSEC : 0;

		if (last[id].count == 0) {
			continue;
		}
		switch (id) {
		case SSP_ACCEL:
		case SSP_MAG_RAW:
			snprintf(vals, sizeof(vals), "x %d y %d z %d", (int16_t)shub_le16(d), (int16_t)shub_le16(d + 2),
			    (int16_t)shub_le16(d + 4));
			break;
		case SSP_MAG:
			snprintf(vals, sizeof(vals), "x %d y %d z %d accuracy %u", (int16_t)shub_le16(d),
			    (int16_t)shub_le16(d + 2), (int16_t)shub_le16(d + 4), d[6]);
			break;
		case SSP_GYRO:
			snprintf(vals, sizeof(vals), "x %d y %d z %d", (int32_t)shub_le32(d), (int32_t)shub_le32(d + 4),
			    (int32_t)shub_le32(d + 8));
			break;
		case SSP_PRESSURE:
			snprintf(vals, sizeof(vals), "pressure %d temperature %d", (int32_t)shub_le32(d),
			    (int16_t)shub_le16(d + 4));
			break;
		case SSP_LIGHT:
		case SSP_LIGHT_CCT:
			snprintf(vals, sizeof(vals), "lux %u cct %d r %u g %u b %u w %u", shub_le32(d), (int32_t)shub_le32(d + 4),
			    shub_le16(d + 8), shub_le16(d + 10), shub_le16(d + 12), shub_le16(d + 14));
			break;
		case SSP_PROXIMITY:
			snprintf(vals, sizeof(vals), "%s adc %u", d[0] ? "NEAR" : "far", shub_le16(d + 1));
			break;
		default:
			snprintf(vals, sizeof(vals), "%u bytes: %02x %02x %02x %02x", last[id].size, d[0], d[1], d[2], d[3]);
			break;
		}
		say("sensors: %2u %-14s %s (%llu samples, the last %llu ms ago, hub time %llu)\n", id, sensor_name(id), vals,
		    last[id].count, age, last[id].hub_time);
	}
	lck_mtx_unlock(&shub_lock);
}
