/*
 * HakiosMetalVK.dylib, the half of HakiosMetal.bundle (METAL=vk) that opens the device: it links the Metal API (the frontend,
 * NativeBridgeMetalFrontend.mm) and through it libmetal2vulkan.dylib and the Vulkan driver, so it is loaded only when a process that was
 * shown the device first asks it for something only the runtime can answer (HakiosMetalVK.mm, the bundle's own executable, says when).
 *
 * The Vulkan driver is an installable client driver (ICD) opened by its path, there being no Vulkan loader on iOS (tools/metal/bridge/icd.rs):
 *   1. the environment's HAKIOS_VK_ICD, if set;
 *   2. else the first line of HakiosMetal.icd beside the bundle (in the directory that holds HakiosMetal.bundle);
 *   3. else /redwood/lib/libhakios.vk.dylib: gfxstream's guest Vulkan driver for the Pixel (AVF), being built on gpu/gfxstream-vulkan.
 *      On the Galaxy S9 the driver is PanVK (METAL_ICD=/redwood/lib/libvulkan_panfrost.dylib, tools/mk-panvk.sh): it gives the
 *      Mali-G72 (v6) Vulkan 1.0 unless told otherwise, which hakios_metal_vk_open() does for that driver only (docs/metal.md).
 * It has to export vk_icdGetInstanceProcAddr or vkGetInstanceProcAddr, and give a Vulkan 1.2 device with timeline semaphores (the
 * runtime's floor).
 */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <CoreFoundation/CoreFoundation.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "NativeBridgeMetalFrontend.h"
#include "bridge/NativeMetalObjects.h"
#include "../hakios_metal_shown.h"

#define DEFAULT_ICD "/redwood/lib/libhakios.vk.dylib"

/*
 * Shader priming's progress, which the frontend's telemetry reads (AdrenoMetal's plug-in defines these in PrimeShaders.h, its warming of
 * QuartzCore's pipelines at boot). Scion does not prime yet: they stay zero.
 */
extern "C" {
volatile uint32_t native_prime_compiling, native_prime_libraries_done, native_prime_libraries_total, native_prime_made, native_prime_rate;
}

static void (*say)(const char *line);

static void
sayf(const char *format, ...) __attribute__((format(printf, 1, 2)));

static void
sayf(const char *format, ...)
{
	char line[768];
	va_list ap;

	va_start(ap, format);
	vsnprintf(line, sizeof(line), format, ap);
	va_end(ap);
	if (say != NULL) {
		say(line);
	}
}

/* a selector the frontend has not got, once per class and selector: the next step's list */
static void
missing(const char *cls, const char *selector)
{
	static NSMutableSet *seen;
	NSString *name = [NSString stringWithFormat:@"-[%s %s]", cls, selector];

	@synchronized([NSProcessInfo class]) {
		if (seen == nil) {
			seen = [NSMutableSet new];
		}
		if ([seen containsObject:name]) {
			return;
		}
		[seen addObject:name];
	}
	sayf("asked and not implemented: %s", name.UTF8String);
}

/* the Vulkan driver's path, by the order above */
static void
driverPath(char *path, size_t size)
{
	const char *environment = getenv("HAKIOS_VK_ICD");
	char file[1024];
	FILE *f;

	if (environment != NULL && environment[0] != '\0') {
		strlcpy(path, environment, size);
		return;
	}
	if (hakios_metal_beside_bundle("HakiosMetal.icd", file, sizeof(file)) && (f = fopen(file, "r")) != NULL) {
		bool named = fgets(path, (int)size, f) != NULL;

		fclose(f);
		path[strcspn(path, " \t\r\n#")] = '\0';
		if (named && path[0] == '/') {
			return;
		}
		sayf("%s names no driver (an absolute path on its first line): the default", file);
	}
	strlcpy(path, DEFAULT_ICD, size);
}

/*
 * A writable directory for the translated shaders (the runtime will not open a device without one): the user's cache directory, its temp
 * directory, then /tmp and /private/var/tmp, each with m2v appended; the first that can be written.
 */
static bool
cacheDirectory(char *path, size_t size)
{
	char candidates[4][512] = {};

	if (confstr(_CS_DARWIN_USER_CACHE_DIR, candidates[0], sizeof(candidates[0]) - 16) == 0) {
		candidates[0][0] = '\0';
	}
	if (confstr(_CS_DARWIN_USER_TEMP_DIR, candidates[1], sizeof(candidates[1]) - 16) == 0) {
		candidates[1][0] = '\0';
	}
	strlcpy(candidates[2], "/tmp/", sizeof(candidates[2]));
	strlcpy(candidates[3], "/private/var/tmp/", sizeof(candidates[3]));
	for (unsigned i = 0; i < 4; i++) {
		if (candidates[i][0] == '\0') {
			continue;
		}
		strlcat(candidates[i], "m2v", sizeof(candidates[i]));
		mkdir(candidates[i], 0700);
		if (access(candidates[i], W_OK) == 0) {
			strlcpy(path, candidates[i], size);
			return true;
		}
	}
	return false;
}

/*
 * Open the Vulkan device and make DEVICE (an object of the bundle's class HakiosMetalDevice, which Metal or the program holds already)
 * the Metal device on it. On failure WHY says why. Called once per process, under the bundle's lock.
 */
extern "C" bool
hakios_metal_vk_open(id device, uint64_t registryID, void (*logger)(const char *), char *why, size_t size)
{
	char driver[1024], cache[512], text[512] = "";
	bool made = false;

	say = logger;
	native_bridge_metal_log_hook = logger;
	driverPath(driver, sizeof(driver));
	if (access(driver, R_OK) != 0) {
		snprintf(why, size, "no Vulkan driver at %s (HAKIOS_VK_ICD or HakiosMetal.icd name another)", driver);
		return false;
	}
	if (!cacheDirectory(cache, sizeof(cache))) {
		snprintf(why, size, "no writable directory for the shader cache");
		return false;
	}
	/*
	 * Surfaces this GPU reads or writes must be plain rows: Apple's lossless compression is a layout of Apple's GPUs (AdrenoMetal
	 * found the render server and every client compressing unless told not to). Asked of every process the device is opened in.
	 */
	CFPreferencesSetAppValue(CFSTR("CAEnableIOSurfaceCompression"), kCFBooleanFalse, kCFPreferencesCurrentApplication);
	if (strcmp(getprogname(), "backboardd") == 0) {
		setenv("CA_DISABLE_FRAMEBUFFER_COMPRESSION", "1", 1);
		CFPreferencesSetAppValue(CFSTR("CAEnableFramebufferCompression"), kCFBooleanFalse, kCFPreferencesCurrentApplication);
	}
	/*
	 * PanVK, Mesa's Vulkan driver for the Mali (the Galaxy S9's G72, Bifrost v6; tools/mk-panvk.sh): Mesa refuses a v6 GPU unless
	 * PAN_I_WANT_A_BROKEN_VULKAN_DRIVER is set, and reports Vulkan 1.0 there, below the runtime's floor of 1.2, although every 1.2
	 * feature the runtime asks for (timeline semaphores, buffer device addresses) is offered as an extension; Mesa's own
	 * MESA_VK_VERSION_OVERRIDE raises the number (docs/metal.md). Set only for that driver, only in a process the device is opened in,
	 * and never over a value the environment already has.
	 */
	const char *base = strrchr(driver, '/');
	if (strncmp(base != NULL ? base + 1 : driver, "libvulkan_panfrost", 18) == 0) {
		setenv("PAN_I_WANT_A_BROKEN_VULKAN_DRIVER", "1", 0);
		setenv("MESA_VK_VERSION_OVERRIDE", "1.2", 0);
	}
	@try {
		made = native_bridge_metal_adopt(device, driver, cache, registryID, missing);
	} @catch (NSException *exception) {
		snprintf(why, size, "the device threw: %s", exception.description.UTF8String ?: "?");
		return false;
	}
	nmb_icd_describe(text, sizeof(text));
	if (!made) {
		snprintf(why, size, "no device on %s: %s", text, nmb_last_error());
		return false;
	}
	snprintf(why, size, "%s, on %s, shaders cached in %s, runtime %s", [(id<MTLDevice>)device name].UTF8String ?: "?", text, cache,
	    nmb_runtime_build());
	return true;
}
