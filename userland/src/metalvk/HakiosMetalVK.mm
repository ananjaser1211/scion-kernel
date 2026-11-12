/*
 * HakiosMetal, Metal on Vulkan (userland/build.sh METAL=vk; docs/metal-plan.md, "Metal on Vulkan"): the plug-in Metal loads when
 * the kernel names it (boot-arg metal=N), whose device is metal2vulkan's runtime on a Vulkan driver of Scion's.
 *
 * The bundle is made of three parts:
 *   this file      the class Metal takes (HakiosMetalDevice, MetalPluginClassName), which finds the Vulkan driver and opens the device
 *   NativeBridgeMetalFrontend.mm
 *                  the Metal API: AdrenoMetal's frontend (MIT, NativeMetal contributors), a dynamic subclass of _MTLDevice and an object of
 *                  its own for every Metal object, each call handed to the bridge
 *   libmetal2vulkan.dylib (/redwood/lib)
 *                  the bridge (AdrenoMetal's, MIT) over metal2vulkan's runtime and translator (GPL-2.0-or-later OR LGPL-3.0-or-later),
 *                  built by tools/metal/mk-metal2vulkan.sh; linked, never copied in, so that it can be replaced
 *
 * What Metal does, as with the other build of this plug-in (src/HakiosMetal.m): it reads MetalPluginName and MetalPluginClassName off
 * IOAcceleratorES (the kernel's metal=1, MiniIOMobileFramebuffer.cpp), opens /System/Library/Extensions/HakiosMetal.bundle, takes the
 * class and sends it -initWithAcceleratorPort:. That init returns the frontend's device in place of itself (as AdrenoMetal's plug-in
 * does): an object of the frontend's class NativeBridgeMetalDevice, a subclass of _MTLDevice that declares MTLDevice and MTLDeviceSPI.
 *
 * The Vulkan driver is an installable client driver (ICD) opened by its path, there being no Vulkan loader on iOS (tools/metal/bridge/icd.rs):
 *   1. the environment's HAKIOS_VK_ICD, if set;
 *   2. else the first line of HakiosMetal.icd beside the bundle (in the directory that holds HakiosMetal.bundle);
 *   3. else /redwood/lib/libhakios.vk.dylib: gfxstream's guest Vulkan driver for the Pixel (AVF), being built on gpu/gfxstream-vulkan.
 *      On the Galaxy S9 the driver is PanVK (METAL_ICD=/redwood/lib/libvulkan_panfrost.dylib, tools/mk-panvk.sh): it gives the
 *      Mali-G72 (v6) Vulkan 1.0 unless told otherwise, which openDevice() below does for that driver only (docs/metal.md).
 * It has to export vk_icdGetInstanceProcAddr or vkGetInstanceProcAddr, and give a Vulkan 1.2 device with timeline semaphores (the
 * runtime's floor). Without a driver at that path there is no device: init returns nil, which Metal takes as no plug-in, and every
 * program keeps the paths it has without Metal (the window server its OpenGL ES).
 *
 * Who is shown the device is the same as for the other build (src/hakios_metal_shown.c): metal=1 metal_test only, metal=2 all but
 * backboardd, metal=3 all, and HakiosMetal.clients beside the bundle.
 */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <objc/runtime.h>
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <dlfcn.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
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

static void
say(NSString *text)
{
	NSLog(@"HakiosMetal: %@", text);
}

static void
sayLine(const char *line)
{
	say([NSString stringWithUTF8String:line]);
}

/* a selector the frontend has not got, once per class and selector: the next step's list, as the other build's log is */
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
	say([NSString stringWithFormat:@"asked and not implemented: %@", name]);
}

/* the Vulkan driver's path, by the order above */
static bool
driverPath(char *path, size_t size)
{
	const char *environment = getenv("HAKIOS_VK_ICD");
	char file[1024];
	FILE *f;

	if (environment != NULL && environment[0] != '\0') {
		strlcpy(path, environment, size);
		return true;
	}
	if (hakios_metal_beside_bundle("HakiosMetal.icd", file, sizeof(file)) && (f = fopen(file, "r")) != NULL) {
		bool named = fgets(path, (int)size, f) != NULL;

		fclose(f);
		path[strcspn(path, " \t\r\n#")] = '\0';
		if (named && path[0] == '/') {
			return true;
		}
		say([NSString stringWithFormat:@"%s names no driver (an absolute path on its first line): the default", file]);
	}
	strlcpy(path, DEFAULT_ICD, size);
	return true;
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

static id
openDevice(unsigned port)
{
	static id device;
	static bool tried;
	char driver[1024], cache[512], text[512] = "";
	uint64_t registryID = 0;

	@synchronized([NSProcessInfo class]) {
		if (tried) {
			return device;
		}
		tried = true;
		if (!driverPath(driver, sizeof(driver)) || access(driver, R_OK) != 0) {
			say([NSString stringWithFormat:@"no Vulkan driver at %s: no device (HAKIOS_VK_ICD or HakiosMetal.icd name another)", driver]);
			return nil;
		}
		if (!cacheDirectory(cache, sizeof(cache))) {
			say(@"no writable directory for the shader cache: no device");
			return nil;
		}
		IORegistryEntryGetRegistryEntryID(port, &registryID);
		/*
		 * Surfaces this GPU reads or writes must be plain rows: Apple's lossless compression is a layout of Apple's GPUs (AdrenoMetal
		 * found the render server and every client compressing unless told not to). Asked of every process the device is shown to.
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
		 * MESA_VK_VERSION_OVERRIDE raises the number (docs/metal.md). Set only for that driver, only in a process shown the device, and
		 * never over a value the environment already has.
		 */
		const char *base = strrchr(driver, '/');
		if (strncmp(base != NULL ? base + 1 : driver, "libvulkan_panfrost", 18) == 0) {
			setenv("PAN_I_WANT_A_BROKEN_VULKAN_DRIVER", "1", 0);
			setenv("MESA_VK_VERSION_OVERRIDE", "1.2", 0);
		}
		@try {
			device = native_bridge_metal_open(driver, cache, registryID, missing);
		} @catch (NSException *exception) {
			say([NSString stringWithFormat:@"the device threw: %@", exception]);
			device = nil;
		}
		nmb_icd_describe(text, sizeof(text));
		if (device == nil) {
			say([NSString stringWithFormat:@"no device on %s: %s", text, nmb_last_error()]);
			return nil;
		}
		say([NSString stringWithFormat:@"a device for the accelerator port %u: %@, on %s, shaders cached in %s, runtime %s", port,
		    [(id<MTLDevice>)device name], text, cache, nmb_runtime_build()]);
		return device;
	}
}

/*
 * The one method of HakiosMetalDevice, a class made at load time as a subclass of _MTLDevice (which is not in the SDK's Metal.tbd, so it
 * cannot be subclassed in source): written here on NSObject and installed on that class as -initWithAcceleratorPort:.
 *
 * Its ownership is the init's that Metal calls ([[cls alloc] initWithAcceleratorPort:]): the result is returned retained (the frontend's
 * device, not the object Metal allocated). The allocated object is a _MTLDevice that was never initialised, and it is deliberately not
 * released: -[_MTLDevice dealloc] on an instance whose -init never ran is not something to bet on, and an init returning another object
 * would release it. So the method is not of the init family as ARC sees it (its name), and it keeps that one empty object, once per
 * process (the device is made once).
 */
@interface HakiosMetalVKDeviceMethods : NSObject
- (id)deviceForAcceleratorPort:(unsigned)port NS_RETURNS_RETAINED;
@end

@implementation HakiosMetalVKDeviceMethods
- (id)deviceForAcceleratorPort:(unsigned)port
{
	return port != 0 ? openDevice(port) : nil;
}
@end

__attribute__((constructor)) static void
registerDevice(void)
{
	Class base, device;
	Method init;

	if (!hakios_metal_shown(sayLine)) {
		return;
	}
	if ((base = objc_getClass("_MTLDevice")) == Nil) {
		say(@"this system has no _MTLDevice: no device class is registered");
		return;
	}
	if (objc_getClass("HakiosMetalDevice") != Nil || (device = objc_allocateClassPair(base, "HakiosMetalDevice", 0)) == Nil) {
		say(@"could not make the class HakiosMetalDevice");
		return;
	}
	init = class_getInstanceMethod([HakiosMetalVKDeviceMethods class], @selector(deviceForAcceleratorPort:));
	class_addMethod(device, @selector(initWithAcceleratorPort:), method_getImplementation(init), method_getTypeEncoding(init));
	/* what MTLAddDevice asserts of the class it is given (docs/metal-plan.md, step 1); the device it gets back declares the same */
	class_addProtocol(device, @protocol(MTLDevice));
	if (Protocol *spi = objc_getProtocol("MTLDeviceSPI")) {
		class_addProtocol(device, spi);
	}
	objc_registerClassPair(device);
	say(@"the class HakiosMetalDevice is registered (Metal on Vulkan: metal2vulkan through libmetal2vulkan.dylib)");
}
