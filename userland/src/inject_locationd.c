/*
 * libhakios.locationd.dylib: locationd takes the S9+'s GNSS from gnssd (etc/inject.conf puts it into locationd; docs/gnssd.md).
 *
 * locationd 16.5 picks the device behind its GNSS controller in CLGnssDeviceFacade::create, from the hardware model and
 * nothing else: CLGnssDaemonDevice (Apple's gpsd, Mach service com.apple.gpsd) on the models that ship gpsd,
 * CLEurekaController (a Qualcomm baseband over QMI) or CLGnssUnknownDevice on the others. This phone runs as iPhone10,3,
 * which gets a baseband it has not got. Its receiver is served by gnssd on com.apple.gpsd, in gpsd's own protocol, so
 * here that one choice is made the gpsd one: the call of the "has gpsd" predicate in create (and nowhere else) answers
 * yes. Everything else about the hardware stays as locationd sees it. The rest of the route is locationd's own:
 * the new GNSS architecture (its preference UseNewGnssArchitecture, var/mobile/Library/Preferences/com.apple.locationd.plist),
 * START when a client wants GNSS and STOP when none does, and the fixes go to apps as the phone's own GPS.
 *
 * The address and the instruction are build 20F66's locationd. If the bytes there are not the expected ones (another
 * build) the library leaves locationd alone and says so, and locationd keeps to Wi-Fi.
 */
#include <libkern/OSCacheControl.h>
#include <mach-o/dyld.h>
#include <mach/mach.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/sysctl.h>
#include <unistd.h>

#define FACADE_CREATE_HAS_GPSD  0x1011eb0e0UL   /* CLGnssDeviceFacade::create: bl <the "has gpsd" predicate> */
#define BL_HAS_GPSD             0x97c71409u     /* bl 0x1003b0104 */
#define MOV_W0_1                0x52800020u     /* mov w0, #1 */
#define TEXT_BASE               0x100000000UL   /* locationd's unslid __TEXT */

extern const struct mach_header_64 *_NSGetMachExecuteHeader(void);

static void
say(const char *format, ...)
{
	char line[300];
	va_list ap;
	int n;

	va_start(ap, format);
	n = vsnprintf(line, sizeof(line), format, ap);
	va_end(ap);
	if (n > 0) write(2, line, (size_t)n < sizeof(line) ? (size_t)n : sizeof(line) - 1);
}

__attribute__((constructor)) static void
gnss_from_gnssd(void)
{
	char machine[32] = "";
	size_t len = sizeof(machine);
	uint32_t *insn = (uint32_t *)(FACADE_CREATE_HAS_GPSD - TEXT_BASE + (uintptr_t)_NSGetMachExecuteHeader());
	vm_address_t page = (vm_address_t)insn & ~(vm_address_t)(vm_page_size - 1);

	sysctlbyname("hw.machine", machine, &len, NULL, 0);
	if (strcmp(machine, "iPhone10,3") != 0) {
		say("libhakios.locationd: %s is not the identity this is for; locationd left alone\n", machine);
		return;
	}
	if (*insn != BL_HAS_GPSD) {
		say("libhakios.locationd: not build 20F66's locationd (%#x); left alone, no GNSS from gnssd\n", *insn);
		return;
	}
	if (vm_protect(mach_task_self(), page, vm_page_size, FALSE, VM_PROT_READ | VM_PROT_WRITE | VM_PROT_COPY) != KERN_SUCCESS) {
		say("libhakios.locationd: cannot write locationd's code; no GNSS from gnssd\n");
		return;
	}
	*insn = MOV_W0_1;
	vm_protect(mach_task_self(), page, vm_page_size, FALSE, VM_PROT_READ | VM_PROT_EXECUTE);
	sys_icache_invalidate(insn, 4);
	say("libhakios.locationd: GNSS device is gpsd's (com.apple.gpsd, served by gnssd)\n");
}
