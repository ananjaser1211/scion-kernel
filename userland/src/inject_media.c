/*
 * libhakios.media.dylib: code of ours inside mediaserverd (see inject.c for how it gets there).
 *
 * mediaserverd is the audio server, and also the camera's: the capture stack of CMCapture lives
 * in the same process. Soon after it starts, that stack "prewarms" the Core Image filters of the
 * portrait mode, for which Core Image wants an OpenGL ES context; there is no GPU, the context
 * has no functions behind it, and the daemon jumps to address 0 -- every time launchd restarts
 * it. Nobody is going to take a portrait here: the prewarming is skipped.
 *
 * What the daemon is wanted for meanwhile is neither sound nor the camera: it is where
 * VideoToolbox decodes for everybody else (the service com.apple.coremedia.decompressionsession),
 * and a HEIC picture is HEVC inside. Without the daemon ImageIO has "no way to decode a HEIC",
 * and the stock wallpaper of Darwin userland 16, whose layers are four such pictures, is its gradient alone.
 * There is no hardware decoder here; VideoToolbox takes its software one (VCPHEVC.videocodec).
 *
 * The flashlight (only where the kernel has one: torch=3, /dev/hakios-torch): Control Center's
 * button and AVFlashlight reach FigFlashlight in here, which asks the capture stack's device vendor
 * (+[BWFigCaptureDeviceVendor sharedCaptureDeviceVendor]) for the camera device and its back
 * camera's stream, and sets the stream's "TorchLevel" (0 to 1). Without a camera there is no
 * vendor (it is nil), every message to it does nothing, and FigFlashlight says it has set the level
 * when nothing has been. So where there is no vendor, FigFlashlight -- and only FigFlashlight,
 * by where it calls from -- is given one of ours, with a device and a stream whose torch level goes
 * to the kernel (see flashlight_*() below). Everybody else still gets none, as before.
 *
 * The camera (only where the kernel has one: cam=5, /dev/hakios-camera): the vendor is made for
 * everybody, with the create function of our capture device (camera_device.c) in place of the
 * one H10ISP.mediacapture would give for the iPhone's ISP; the capture graph is built around it and
 * its frames reach Camera.app's preview (see camera_device.c). Its stream takes the torch level too,
 * so FigFlashlight would need nothing of its own then (not tried yet).
 *
 * What the capture stack asks of hardware this phone has none of is refused here: there is no ISP
 * processing session, so there can be no video stabilization (see video_stabilization_method below).
 *
 * CMCapture is not loaded yet when this library is: the methods are replaced when its image
 * arrives.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <CoreMedia/CoreMedia.h>
#include <CoreVideo/CoreVideo.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <mach-o/dyld.h>
#include <mach/mach_time.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/sysctl.h>
#include <execinfo.h>
#include <signal.h>
#include <sys/ucontext.h>
#include <unistd.h>

static void
say(const char *format, ...)
{
	char line[256];
	va_list ap;
	int n;

	va_start(ap, format);
	n = vsnprintf(line, sizeof(line), format, ap);
	va_end(ap);
	if (n > (int)sizeof(line) - 1) {
		n = sizeof(line) - 1;
	}
	write(2, line, (size_t)n);
}

extern const void *_dyld_get_shared_cache_range(size_t *length);

#define CACHE_BASE 0x180000000UL                /* where the cache was built to be, before the slide */

static uintptr_t slide;

/*
 * The daemon's last words. There is no crash reporter in this system: a mediaserverd that dies says only "exited due
 * to SIGSEGV" in launchd's log. Here a fatal signal says where: the PC, the LR, the fault address and the stack, each
 * as image+offset (dladdr) and, for the shared cache, unslid (`ipsw dyld a2s <cache> <address>` names it). Then the
 * signal is taken again as it would have been.
 */
static void
crash_addr(char *line, size_t room, int *k, const char *label, uintptr_t a)
{
	Dl_info info;

	if (*k >= (int)room - 96) {
		return;
	}
	if (a != 0 && dladdr((const void *)a, &info) != 0 && info.dli_fname != NULL) {
		const char *base = strrchr(info.dli_fname, '/');

		*k += snprintf(line + *k, room - (size_t)*k, " %s%s+%#lx(%#lx)", label, base ? base + 1 : info.dli_fname,
		    (unsigned long)(a - (uintptr_t)info.dli_fbase), (unsigned long)(a - slide));
	} else {
		*k += snprintf(line + *k, room - (size_t)*k, " %s%#lx", label, (unsigned long)a);
	}
}

static void
crashed(int number, siginfo_t *info, void *context)
{
	void *frames[40];
	char line[2400];
	int n = backtrace(frames, 40), k;
	ucontext_t *uc = context;
	uintptr_t pc = 0, lr = 0, far = 0;

	if (uc != NULL && uc->uc_mcontext != NULL) {
		pc = (uintptr_t)uc->uc_mcontext->__ss.__pc;
		lr = (uintptr_t)uc->uc_mcontext->__ss.__lr;
		far = (uintptr_t)uc->uc_mcontext->__es.__far;
	}
	k = snprintf(line, sizeof(line), "hakios: mediaserverd: SIGNAL %d (address %p, fault %#lx; the cache's slide %#lx):",
	    number, info != NULL ? info->si_addr : NULL, (unsigned long)far, (unsigned long)slide);
	crash_addr(line, sizeof(line), &k, "pc ", pc);
	crash_addr(line, sizeof(line), &k, "lr ", lr);
	k += snprintf(line + k, sizeof(line) - (size_t)k, "; the stack:");
	for (int i = 0; i < n; i++) {
		crash_addr(line, sizeof(line), &k, "", (uintptr_t)frames[i]);
	}
	if (k > (int)sizeof(line) - 2) {
		k = (int)sizeof(line) - 2;
	}
	line[k++] = '\n';
	write(2, line, (size_t)k);
	signal(number, SIG_DFL);
	raise(number);
}

static void
crash_watch(void)
{
	static const int fatal[] = { SIGSEGV, SIGBUS, SIGILL, SIGTRAP, SIGABRT, SIGFPE };
	struct sigaction sa;

	memset(&sa, 0, sizeof(sa));
	sa.sa_sigaction = crashed;
	sa.sa_flags = SA_SIGINFO;
	sigemptyset(&sa.sa_mask);
	for (size_t i = 0; i < sizeof(fatal) / sizeof(fatal[0]); i++) {
		sigaction(fatal[i], &sa, NULL);
	}
}

/* +[BWCoreImageFilterRenderer prewarmPortraitFilterVersion:semanticStyleFilters:] */
static void
prewarm_portrait_filter(__unused id self, __unused SEL selector, __unused int version, __unused BOOL styles)
{
}

/* ---- the flashlight ------------------------------------------------------------------------------- */

static int torch = -1;                  /* /dev/hakios-torch */
static bool camera;                     /* /dev/hakios-camera is there */
extern int hakios_camera_device_create(const void *allocator, const void *options, void **out);
static Class vendor_class, device_class, stream_class;
static IMP original_shared_vendor;
static unsigned torch_said;

/*
 * FigFlashlight's functions that ask for the vendor, unslid (20F66): flashlight_IsAvailable,
 * _NotifyForCurrentState and _setupDevice lie together, _teardownDevice apart.
 */
static const struct { uintptr_t from, to; } flashlight_code[] = {
	{ 0x1a1fb0830UL, 0x1a1fb0c5cUL }, { 0x1a1eee5c0UL, 0x1a1eee6e8UL },
};

/* +[BWFigCaptureDeviceVendor sharedCaptureDeviceVendor] */
static id
shared_vendor(id self, SEL selector)
{
	uintptr_t caller = (uintptr_t)__builtin_return_address(0) - slide;
	id vendor = ((id (*)(id, SEL))original_shared_vendor)(self, selector);
	static id ours, camera_vendor;
	static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

	if (vendor != NULL) {
		return vendor;
	}
	if (!camera && access("/dev/hakios-camera", F_OK) == 0) {
		camera = true;          /* (should the kernel have made it after mediaserverd started) */
		say("hakios: mediaserverd: the kernel has a camera (/dev/hakios-camera)\n");
	}
	if (camera) {
		pthread_mutex_lock(&lock);
		if (camera_vendor == NULL) {
			id made = ((id (*)(Class, SEL))objc_msgSend)(objc_getClass("BWFigCaptureDeviceVendor"), sel_registerName("alloc"));

			camera_vendor = ((id (*)(id, SEL, void *))objc_msgSend)(made, sel_registerName("initWithDeviceCreateFunction:"),
			    (void *)hakios_camera_device_create);
			say("hakios: mediaserverd: the camera's device vendor %s\n", camera_vendor ? "made (camera_device.c)" :
			    "CANNOT BE MADE");
		}
		pthread_mutex_unlock(&lock);
		return camera_vendor;
	}
	for (size_t i = 0; torch >= 0 && i < sizeof(flashlight_code) / sizeof(flashlight_code[0]); i++) {
		if (caller >= flashlight_code[i].from && caller < flashlight_code[i].to) {
			if (ours == NULL) {
				ours = class_createInstance(vendor_class, 0);
				say("hakios: mediaserverd: no camera, so no device vendor: FigFlashlight gets the flashlight's own\n");
			}
			return ours;
		}
	}
	return NULL;
}

static void
flashlight_set(uint32_t level)
{
	ssize_t written = write(torch, &level, sizeof(level));

	if (written != (ssize_t)sizeof(level) || torch_said < 40) {
		torch_said++;
		say("hakios: mediaserverd: the flashlight at %u of 1000%s\n", level, written == (ssize_t)sizeof(level) ? "" :
		    " (NOT TAKEN by /dev/hakios-torch)");
	}
}

/* the vendor's: one client, one device, always available to it */
static int
flashlight_register(__unused id self, __unused SEL selector, __unused int pid, __unused id description, __unused int priority,
    __unused BOOL steal, __unused BOOL share, __unused id handler)
{
	return 1;
}

static id
flashlight_copy_device(__unused id self, __unused SEL selector, __unused int client, __unused BOOL inform, int *error)
{
	if (error != NULL) {
		*error = 0;
	}
	return class_createInstance(device_class, 0);
}

static id
flashlight_copy_stream(__unused id self, __unused SEL selector, __unused int position, __unused int type, id device)
{
	return device != NULL && object_getClass(device) == device_class ? class_createInstance(stream_class, 0) : NULL;
}

static void
flashlight_take_back(__unused id self, __unused SEL selector, __unused id device, __unused int client, __unused BOOL inform)
{
	flashlight_set(0);
}

static BOOL
flashlight_active_equals(__unused id self, __unused SEL selector, id device)
{
	return device == NULL || object_getClass(device) == device_class;
}

/* the stream's -supportedProperties: none that FigFlashlight looks for ("HighCurrentTorchEnabled") */
static id
flashlight_stream_properties(__unused id self, __unused SEL selector)
{
	static CFDictionaryRef none;

	if (none == NULL) {
		none = CFDictionaryCreate(NULL, NULL, NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	}
	return (id)none;
}

/* the stream's -setProperty:value: -- "TorchLevel", 0 to 1, as thousandths to the kernel */
static int
flashlight_stream_set(__unused id self, __unused SEL selector, id key, id value)
{
	float level = 0;

	if (key != NULL && CFEqual((CFTypeRef)key, CFSTR("TorchLevel"))) {
		if (value != NULL && CFGetTypeID((CFTypeRef)value) == CFNumberGetTypeID()) {
			CFNumberGetValue((CFNumberRef)value, kCFNumberFloatType, &level);
		}
		level = level < 0 ? 0 : level > 1 ? 1 : level;
		flashlight_set((uint32_t)(level * 1000 + 0.5f));
	}
	return 0;
}

/*
 * Video stabilization, of which this hardware has none: the capture graph's VIS node
 * (-[FigCaptureVISPipeline _newVISNodeWithUpstreamOutput:...]) is made with the ISP's processing
 * session, and this device has no such session, so the node comes back nil, the VIS pipeline with
 * it, the movie file sink's tail pipeline with that, and captureSession_buildGraphWithConfiguration
 * tears the whole graph down -- the preview too, for all that the preview has nothing to do with
 * stabilizing a recording. The connection's stabilization method is where the whole stack reads the
 * wish from (+[FigVideoCaptureConnectionConfiguration videoStabilizationMethods:includeIris:] ->
 * cspc_getVideoStabilizationType -> the connections' FigCaptureVideoStabilizationType), and 0 there
 * means none: the graph is then built without a VIS pipeline anywhere
 * (+[FigCaptureVISPipeline visPipelineIsRequiredForFigCaptureVideoStabilizationType:] says no to
 * every type but 2 and 3), and recordings are as steady as the hand that holds the phone.
 */
static int
video_stabilization_method(id self, SEL selector)
{
	(void)self;
	(void)selector;
	return 0;
}

/*
 * The type the stack acts on. The connection configuration arrives from the application's
 * AVFoundation with the type in it, which is read straight out of the object and not computed from
 * the method above (-[FigCaptureSessionParsedConfiguration _computeVideoStabilizationTypes] only
 * fills in what the client left out), so the wish has to be refused here as well.
 */
static int
video_stabilization_type(id self, SEL selector)
{
	(void)self;
	(void)selector;
	return 0;                       /* kFigCaptureVideoStabilizationType_None */
}

/*
 * The microphone, which the capture stack has none of here. A capture session's sound comes through
 * CoreMedia's own capture audio session -- BWAudioSourceNode's start sets its properties and reads
 * its pull format, all of it through a brokered plug-in of the audio HAL -- and that plug-in is not
 * on this phone: the node's start comes back with 'nope', the HAL's word for "no such thing".
 *
 * Camera.app's session has an audio input in it, because it is a session for photographs and for
 * video both. The graph starts its source nodes late (startDeferredSourceNodesIfNeeded) and takes
 * the first error it gets for the whole graph's, so the session reported AVCaptureSessionRuntimeError
 * to the application, which took the session down and built it again, two seconds later, for ever:
 * the camera's preview came in bursts of five frames with the sensor powering down in between.
 *
 * The node is told that it started. It then produces no sound -- which is all a phone with no
 * capture audio has to offer -- and the rest of the graph, the preview and the photograph, runs.
 * Only that one error is swallowed: a microphone that is there and fails for another reason still
 * takes the session down, as it should. (The kernel's own microphone, adsp=1 mic=4, is a different
 * path: AVAudioRecorder and Voice Memos record through it, and nothing here touches it.)
 */
#define ERROR_NO_SUCH_THING     1852797029      /* 'nope' */

static IMP original_audio_source_start;

static BOOL
audio_source_start(id self, SEL selector, id *error)
{
	BOOL started = ((BOOL (*)(id, SEL, id *))original_audio_source_start)(self, selector, error);
	id made = error != NULL ? *error : NULL;
	static bool said;

	if (started || made == NULL || ((long (*)(id, SEL))objc_msgSend)(made, sel_registerName("code")) != ERROR_NO_SUCH_THING) {
		return started;
	}
	*error = NULL;                  /* (autoreleased by the capture stack: not ours to give back) */
	if (!said) {
		said = true;
		say("hakios: mediaserverd: there is no capture audio on this phone: the session's audio source gives nothing, "
		    "and the camera runs without it\n");
	}
	return true;
}

/*
 * A photograph's quality prioritization, "speed" at the most. Asked for "balanced" (Camera.app's) or
 * "quality", the capture device takes a scene for a low-light one whenever a frame was exposed at
 * least as long as the sensor's still image stabilization threshold
 * (-[BWFigVideoCaptureDevice _isLowLightSceneUsingFrameStatisticsByPortType:]) -- a threshold from
 * the sensor's tuning in CameraSetup.plist, 0 for a sensor it has no tuning for, as ours -- and
 * then asks the stream for a bracket of four frames for SIS fusion (captureType 4, BWSISNode), which
 * this port has nothing to fuse with: the photograph never came. With "speed" it asks for one frame
 * (-_resolveStillImageCaptureTypeFromStatisticsByPortType:..., captureType 1), which the stream
 * gives (camera_device.c, StillImageCaptureNow).
 */
static IMP original_quality_prioritization;

static int
quality_prioritization(id self, SEL selector)
{
	int asked = ((int (*)(id, SEL))original_quality_prioritization)(self, selector);
	static bool said;

	if (asked > 1 && !said) {
		said = true;
		say("hakios: mediaserverd: a photograph's quality prioritization %d taken as 1 (speed): one frame, no SIS bracket\n",
		    asked);
	}
	return asked > 1 ? 1 : asked;
}

/*
 * A photograph's noise reduction (BWGNRNode, between the still image coordinator and the photo
 * encoder) is a FigSampleBufferProcessor of the iPhone's, which is handed the picture and never
 * hands one back here: the photograph stopped there, with no error, and the request with it (the
 * shutter's second press asked for nothing more). The picture goes past it to the node's output as
 * it came, as with no noise reduction to do.
 */
static void
gnr_render(id self, SEL selector, void *sample_buffer, id input)
{
	id output = ((id (*)(id, SEL))objc_msgSend)(self, sel_registerName("output"));
	static bool said;

	(void)selector, (void)input;
	if (!said) {
		said = true;
		say("hakios: mediaserverd: a photograph goes past the noise reduction, which has nothing to do it with here\n");
	}
	if (output != nil) {
		((void (*)(id, SEL, void *))objc_msgSend)(output, sel_registerName("emitSampleBuffer:"), sample_buffer);
	}
}

static bool
is_node(id node, const char *name)
{
	return node != nil && strcmp(object_getClassName(node), name) == 0;
}

/*
 * How long a photograph's encoding took, from the picture handed to the photo encoder node to the one
 * it hands on (HEVC in software, see register_hevc_encoder below: the slowest step of a photograph).
 */
static void
time_photo_encoder(id output, CMSampleBufferRef sample_buffer)
{
	CVImageBufferRef image = CMSampleBufferGetImageBuffer(sample_buffer);
	static uint64_t began;
	static mach_timebase_info_data_t timebase;
	id connection, input;

	if (image == NULL || CVPixelBufferGetWidth(image) < 2000) {
		return;                 /* not a photograph */
	}
	if (is_node(((id (*)(id, SEL))objc_msgSend)(output, sel_registerName("node")), "BWPhotoEncoderNode")) {
		if (began != 0) {
			if (timebase.denom == 0) {
				mach_timebase_info(&timebase);
			}
			say("hakios: mediaserverd: a photograph of %zux%zu encoded in %.0f ms\n", CVPixelBufferGetWidth(image),
			    CVPixelBufferGetHeight(image), (double)(mach_absolute_time() - began) * timebase.numer / timebase.denom / 1e6);
		}
		began = 0;
		return;
	}
	connection = ((id (*)(id, SEL))objc_msgSend)(output, sel_registerName("connection"));
	input = connection != nil ? ((id (*)(id, SEL))objc_msgSend)(connection, sel_registerName("input")) : nil;
	if (input != nil && is_node(((id (*)(id, SEL))objc_msgSend)(input, sel_registerName("node")), "BWPhotoEncoderNode")) {
		began = mach_absolute_time();
	}
}

/*
 * A node's output given no sample buffer at all, which no node takes: the session's audio source
 * (-[BWAudioSourceNode _generateSamples]), whose audio unit never started here (see audio_source_start
 * above), fills the silence by a timer with buffers it fails to make and emits them all the same --
 * NULL. Live Photo's staging node added that to an array (-[BWIrisStagingNode renderSampleBuffer:forInput:])
 * and mediaserverd died of the NSInvalidArgumentException, every half a minute with Live Photo on, the
 * preview black for a second each time. A NULL is not passed on.
 */
static IMP original_emit_sample_buffer;

static void
emit_sample_buffer(id self, SEL selector, CMSampleBufferRef sample_buffer)
{
	static bool said;

	if (sample_buffer == NULL) {
		if (!said) {
			said = true;
			say("hakios: mediaserverd: a node's output was given no sample buffer (the silent audio source's): not passed on\n");
		}
		return;
	}
	time_photo_encoder(self, sample_buffer);
	((void (*)(id, SEL, CMSampleBufferRef))original_emit_sample_buffer)(self, selector, sample_buffer);
}

/*
 * The photograph's encoder. Camera.app asks for HEIC -- HEVC inside a HEIF file, which CMPhoto makes
 * through VideoToolbox -- and VideoToolbox here has no HEVC encoder but the iPhone's hardware ones
 * (com.apple.videotoolbox.videoencoder.hevc and .hevc.gva, behind its AVE): the photo encoder node
 * failed with kVTCouldNotFindVideoEncoderErr (-12908). Apple's software HEVC codec is in the system
 * all the same, VCPHEVC.videocodec, whose decoder VideoToolbox takes here for every HEIC picture (see
 * the top), and there is an encoder in it, HEVCVideoEncoder_CreateInstance, that Darwin never registers:
 * the codec's VCPHEVCRegisterEncoder is an empty function. It is registered here, as the codec
 * registers its decoder (VTRegisterVideoDecoderWithInfo, not hardware accelerated).
 */
static void
register_hevc_encoder(void)
{
	void *codec = dlopen("/System/Library/VideoCodecs/VCPHEVC.videocodec", RTLD_NOW | RTLD_LOCAL);
	void *vt = dlopen("/System/Library/Frameworks/VideoToolbox.framework/VideoToolbox", RTLD_NOW | RTLD_GLOBAL);
	void *create = codec != NULL ? dlsym(codec, "HEVCVideoEncoder_CreateInstance") : NULL;
	int (*register_encoder)(uint32_t, CFDictionaryRef, void *) = vt != NULL ? dlsym(vt, "VTRegisterVideoEncoderWithInfo") : NULL;
	CFStringRef *id_key = vt != NULL ? dlsym(vt, "kVTVideoEncoderList_EncoderID") : NULL;
	CFStringRef *name_key = vt != NULL ? dlsym(vt, "kVTVideoEncoderList_EncoderName") : NULL;
	CFStringRef *hardware_key = vt != NULL ? dlsym(vt, "kVTVideoEncoderList_IsHardwareAccelerated") : NULL;
	int status;

	if (create == NULL || register_encoder == NULL || id_key == NULL || name_key == NULL || hardware_key == NULL) {
		say("hakios: mediaserverd: no software HEVC encoder to register (%s)\n", codec == NULL ? "no VCPHEVC.videocodec" :
		    create == NULL ? "no HEVCVideoEncoder_CreateInstance" : "VideoToolbox without VTRegisterVideoEncoderWithInfo");
		return;
	}
	const void *keys[] = { *id_key, *name_key, *hardware_key };
	const void *values[] = { CFSTR("com.apple.videotoolbox.videoencoder.hevc.vcp"), CFSTR("HEVC (VCPHEVC, software)"),
		kCFBooleanFalse };
	CFDictionaryRef info = CFDictionaryCreate(NULL, keys, values, 3, &kCFTypeDictionaryKeyCallBacks,
	    &kCFTypeDictionaryValueCallBacks);

	status = register_encoder('hvc1', info, create);
	CFRelease(info);
	say("hakios: mediaserverd: the software HEVC encoder of VCPHEVC.videocodec registered for 'hvc1' (%d)\n", status);
}

struct method_spec {
	const char *selector, *types;
	IMP imp;
};

static Class
make_class(const char *name, const struct method_spec *methods, size_t count)
{
	Class cls = objc_allocateClassPair(objc_getClass("NSObject"), name, 0);

	for (size_t i = 0; i < count; i++) {
		class_addMethod(cls, sel_registerName(methods[i].selector), methods[i].imp, methods[i].types);
	}
	objc_registerClassPair(cls);
	return cls;
}

static void
flashlight_classes(void)
{
	static const struct method_spec vendor[] = {
		{ "registerClientWithPID:clientDescription:clientPriority:canStealFromClientsWithSamePriority:"
		  "deviceSharingWithOtherClientsAllowed:deviceAvailabilityChangedHandler:", "i@:i@iBB@", (IMP)flashlight_register },
		{ "copyDeviceForClient:informClientWhenDeviceAvailableAgain:error:", "@@:iB^i", (IMP)flashlight_copy_device },
		{ "copyStreamForFlashlightWithPosition:deviceType:forDevice:", "@@:ii@", (IMP)flashlight_copy_stream },
		{ "takeBackDevice:forClient:informClientWhenDeviceAvailableAgain:", "v@:@iB", (IMP)flashlight_take_back },
		{ "activeDeviceEquals:", "B@:@", (IMP)flashlight_active_equals },
	}, stream[] = {
		{ "supportedProperties", "@@:", (IMP)flashlight_stream_properties },
		{ "setProperty:value:", "i@:@@", (IMP)flashlight_stream_set },
	};

	vendor_class = make_class("HakiosFlashlightVendor", vendor, sizeof(vendor) / sizeof(vendor[0]));
	device_class = make_class("HakiosFlashlightDevice", NULL, 0);
	stream_class = make_class("HakiosFlashlightStream", stream, sizeof(stream) / sizeof(stream[0]));
}

/*
 * The class methods replaced, each as soon as its class exists. `expected` is the unslid address
 * of the code in Darwin userland 16.5 (20F66): a method that is not what it was is left alone. `original`, where
 * there is one, keeps what was there (the vendor's: given as it is wherever there is one, and where
 * there is none, ours only with the kernel's camera or flashlight).
 */
static struct replacement {
	const char *class_name, *selector;
	uintptr_t expected;
	IMP replacement, *original;
	bool instance;                  /* an instance method, not a class method */
	bool done;
} replacements[] = {
	{ "BWCoreImageFilterRenderer", "prewarmPortraitFilterVersion:semanticStyleFilters:", 0x1a2115c0cUL,
	  (IMP)prewarm_portrait_filter, NULL, false, false },
	{ "BWFigCaptureDeviceVendor", "sharedCaptureDeviceVendor", 0x1a1ef0d04UL,
	  (IMP)shared_vendor, &original_shared_vendor, false, false },
	{ "BWAudioSourceNode", "start:", 0x1a1eefa18UL, (IMP)audio_source_start, &original_audio_source_start, true, false },
	{ "FigVideoCaptureConnectionConfiguration", "videoStabilizationMethod", 0x1a1e99ef8UL,
	  (IMP)video_stabilization_method, NULL, true, false },
	{ "FigVideoCaptureConnectionConfiguration", "videoStabilizationType", 0x1a20d8f48UL,
	  (IMP)video_stabilization_type, NULL, true, false },
	{ "FigCaptureStillImageSettings", "qualityPrioritization", 0x1a1f533dcUL,
	  (IMP)quality_prioritization, &original_quality_prioritization, true, false },
	{ "BWGNRNode", "renderSampleBuffer:forInput:", 0x1a1ec83b8UL, (IMP)gnr_render, NULL, true, false },
	{ "BWNodeOutput", "emitSampleBuffer:", 0x1a1ea37e0UL, (IMP)emit_sample_buffer, &original_emit_sample_buffer, true, false },
};

/* ---- the Metal device leaves this process before the capture stack asks for it -------------------- */

/*
 * With the Metal properties on the accelerator service (boot-arg metal=1) MTLCreateSystemDefaultDevice()
 * answers in every process, and in mediaserverd the first to ask is the capture stack: CMCapture's
 * FigPreloadShadersInternal reaches -[_MTLDevice newLibraryWithURL:error:] through three functions of
 * AppleCVAPhoto, asking for System/Library/PrivateFrameworks/AppleCVAPhoto.framework/Resources/
 * cvadepth-iOS.metallib. That file is not in this image (sysimage/select-system.py drops .metallib under
 * the framework directories), so the URL is nil and Metal aborts on `url must not be nil' -- twenty-eight
 * times in one boot, launchd restarting the daemon after each (logs/135751-run.log, docs/metal.md 10.38).
 * AppleCVAPhoto itself survives a library it cannot make: it checks the result and logs "Error creating
 * metal library". What kills the process is Metal's own assertion, and that is reached only because there
 * IS a device to send the message to.
 *
 * So the device is taken out of this process, exactly as inject.c does for the window server: Metal keeps
 * every device it has made in one static NSMutableArray (0x1e1efe9a0, __DATA_DIRTY.__bss, so one per
 * process), and MTLCreateSystemDefaultDevice() is no more than "if that array holds exactly one object,
 * return it" -- an empty array answers nil without a single instruction of Metal's being touched. The
 * array is built on first use, so it has to be asked for first; that makes the device object, which is
 * what the capture stack's next instruction would have done anyway, so the process pays nothing new.
 * The device is retained and never released, for inject.c's reason: what our -dealloc would do to a GPU
 * is not measured.
 *
 * WHEN, and why not where inject.c does it. There the hook is -[CAWindowServer _detectDisplays], the last
 * Objective-C method on the way in and an address that can be checked. Here the way in is three C
 * functions of AppleCVAPhoto with no method of ours to stand on, so this runs from the add-image notifier
 * this library already registers, the first time Metal is in the process at all -- which is before any
 * code of the image that brought it has run. Being early cannot cost correctness: if the device can be
 * made, it is made and removed; if the accelerator service is not matched yet and the array is built
 * empty, nil is what the capture stack gets, and nil is the whole point.
 *
 * Boot-arg mediamtl=1 leaves the device where it is. It is a switch of its own and not inject.c's camtl=1
 * because the two processes do not risk the same thing: a window server that aborts is a crash loop and a
 * black screen, while mediaserverd aborting costs HEIC decoding, the camera and the sound. A boot may
 * reasonably want Metal in one and not the other. Either way the metallib has to be in the image first,
 * or the same assertion comes straight back.
 */
#define MTL_DEVICE_ARRAY        0x1e1efe9a0UL   /* Metal's static NSMutableArray *_deviceArray */

extern id objc_retain(id object);

static bool metal_left_alone;                   /* boot-arg mediamtl=1 */

static bool
boot_arg(const char *what)
{
	char arguments[1024] = "";
	size_t size = sizeof(arguments) - 1;

	return sysctlbyname("kern.bootargs", arguments, &size, NULL, 0) == 0 &&
	    strstr(arguments, what) != NULL;
}

static void
keep_metal_from_the_capture_stack(void)
{
	static bool done;
	unsigned long (*ask)(id, SEL) = (unsigned long (*)(id, SEL))objc_msgSend;
	void (*tell)(id, SEL) = (void (*)(id, SEL))objc_msgSend;
	id *slot = (id *)(MTL_DEVICE_ARRAY + slide);
	id (*create)(void);
	id device, array;
	unsigned long held;

	if (done || metal_left_alone) {
		return;
	}
	/* Metal is not in this process yet: the image that brings it has not arrived, and the notifier
	 * will be back here when it does. Nothing is printed for that -- a line per image would bury
	 * the log, and the constructor has already said that this is armed. */
	if ((create = (id (*)(void))dlsym(RTLD_DEFAULT, "MTLCreateSystemDefaultDevice")) == NULL) {
		return;
	}
	done = true;
	if ((device = create()) == NULL) {
		say("hakios: mediaserverd: no Metal device in this process, so the capture stack's shader "
		    "preload has nothing to ask\n");
		return;
	}
	if ((array = *slot) == NULL) {
		say("hakios: mediaserverd: a device, but nothing at %#lx; the device list is NOT emptied\n",
		    MTL_DEVICE_ARRAY);
		return;
	}
	if (!((BOOL (*)(id, SEL, Class))objc_msgSend)(array, sel_registerName("isKindOfClass:"),
	    objc_getClass("NSMutableArray"))) {
		say("hakios: mediaserverd: %#lx holds a %s, not Metal's device array; left alone\n",
		    MTL_DEVICE_ARRAY, object_getClassName(array));
		return;
	}
	held = ask(array, sel_registerName("count"));
	objc_retain(device);
	tell(array, sel_registerName("removeAllObjects"));
	say("hakios: mediaserverd: %s taken out of the capture stack (%lu in the list, %lu now); "
	    "MTLCreateSystemDefaultDevice() = %s\n", object_getClassName(device), held,
	    ask(array, sel_registerName("count")), create() != NULL ? "A DEVICE STILL" : "nil");
}

/*
 * The hardware video decoder (mfc_decoder.c, only where the kernel has one: mfc=5). The kernel makes /dev/hakios-mfc when its firmware is up, which
 * may be after this process starts; a thread waits a minute for it and goes.
 */
extern void hakios_mfc_decoder_register(void);

static void *
wait_for_mfc(void *unused)
{
	(void)unused;
	for (int i = 0; i < 60; i++) {
		if (access("/dev/hakios-mfc", F_OK) == 0) {
			hakios_mfc_decoder_register();
			break;
		}
		sleep(1);
	}
	return NULL;
}

static void
image_added(__unused const struct mach_header *header, __unused intptr_t image_slide)
{
	/* before anything of the image that was just added runs, and this is the only place in this
	 * library that is told about a new image at all */
	keep_metal_from_the_capture_stack();
	for (size_t i = 0; i < sizeof(replacements) / sizeof(replacements[0]); i++) {
		struct replacement *r = &replacements[i];
		Class cls;
		Method method;
		IMP original;

		if (r->done || (cls = objc_getClass(r->class_name)) == NULL) {
			continue;
		}
		r->done = true;
		method = r->instance ? class_getInstanceMethod(cls, sel_registerName(r->selector)) :
		    class_getClassMethod(cls, sel_registerName(r->selector));
		original = method ? method_getImplementation(method) : NULL;
		if (original == NULL || (uintptr_t)original != r->expected + slide) {
			say("hakios: %c[%s %s]: found %#lx, expected %#lx; left alone\n", r->instance ? '-' : '+', r->class_name,
			    r->selector,
			    original ? (uintptr_t)original - slide : 0, r->expected);
			continue;
		}
		if (r->original != NULL) {
			*r->original = original;
		}
		method_setImplementation(method, r->replacement);
		say("hakios: mediaserverd: %c[%s %s] replaced\n", r->instance ? '-' : '+', r->class_name, r->selector);
	}
}

/*
 * The front camera's focus. D22's AVCaptureSession.plist (CMCapture's FigCaptureSourceUtilities reads it here, in
 * mediaserverd) is the iPhone X's, whose front camera has a fixed focus: no hasAutoFocus, no AutoFocusSystem in its
 * formats, so AVFoundation says it cannot focus and Camera.app sends a tap there nowhere. The S9+'s front S5K3H1 has a
 * lens motor (an AK737x on USI09, exynos_camera.c) and camera_device.c focuses it as the back one, by contrast: the
 * plist is given to CMCapture with the front camera's focus as the back camera's (its window sizes and timeouts) and
 * AutoFocusSystem 1 (contrast detection) in each of its formats. Found by the loaders Foundation has for a file's
 * dictionary; the one used says so. cam_front_af=0: the plist as it is.
 */
static const char *const front_af_keys[] = {
	"hasAutoFocus", "FocusCenterWindowSize", "FocusPointOfInterestWindowSize", "ContinuousAutoFocusWindowHeightOverride",
	"ContinuousAutoFocusWindowWidthOverride", "smoothAutoFocusSupported", "stillImageCaptureNowAfterAutofocusTimeout",
	"stillImageContrastBasedAutofocusTimeout", "autoFocusPositionSensorMode",
};

static bool
capture_plist_path(CFTypeRef where, char *path, size_t size)
{
	CFStringRef s = NULL;
	bool ok;

	if (where == NULL) {
		return false;
	}
	if (CFGetTypeID(where) == CFURLGetTypeID()) {
		s = CFURLCopyFileSystemPath((CFURLRef)where, kCFURLPOSIXPathStyle);
	} else if (CFGetTypeID(where) == CFStringGetTypeID()) {
		s = CFRetain(where);
	}
	if (s == NULL) {
		return false;
	}
	ok = CFStringGetCString(s, path, (CFIndex)size, kCFStringEncodingUTF8);
	CFRelease(s);
	return ok && strstr(path, "AVCaptureSession.plist") != NULL;
}

static CFMutableDictionaryRef
device_with_id(CFArrayRef devices, const char *id)
{
	CFStringRef want = CFStringCreateWithCString(NULL, id, kCFStringEncodingUTF8);
	CFMutableDictionaryRef found = NULL;

	for (CFIndex i = 0; devices != NULL && i < CFArrayGetCount(devices) && found == NULL; i++) {
		CFDictionaryRef d = CFArrayGetValueAtIndex(devices, i);
		CFTypeRef u = CFGetTypeID(d) == CFDictionaryGetTypeID() ? CFDictionaryGetValue(d, CFSTR("uniqueID")) : NULL;

		found = u != NULL && CFEqual(u, want) ? (CFMutableDictionaryRef)d : NULL;
	}
	CFRelease(want);
	return found;
}

/* the plist with the front camera focusing (a new dictionary, +1), or NULL to keep it as it is */
static CFDictionaryRef
capture_plist_patched(CFDictionaryRef plist, const char *path)
{
	CFMutableDictionaryRef copy, back, front, multi, formats = NULL;
	CFArrayRef devices;
	CFNumberRef contrast;
	int one = 1, n = 0;

	if (plist == NULL || CFGetTypeID(plist) != CFDictionaryGetTypeID() || strstr(path, "/D22/") == NULL ||
	    boot_arg("cam_front_af=0")) {
		return NULL;
	}
	copy = (CFMutableDictionaryRef)CFPropertyListCreateDeepCopy(NULL, plist, kCFPropertyListMutableContainersAndLeaves);
	devices = copy ? CFDictionaryGetValue(copy, CFSTR("AVCaptureDevices")) : NULL;
	back = device_with_id(devices, "com.apple.avfoundation.avcapturedevice.built-in_video:0");
	front = device_with_id(devices, "com.apple.avfoundation.avcapturedevice.built-in_video:1");
	if (back == NULL || front == NULL) {
		say("hakios: camera: %s: no back or front camera in it: left as it is\n", path);
		if (copy != NULL) {
			CFRelease(copy);
		}
		return NULL;
	}
	for (size_t i = 0; i < sizeof(front_af_keys) / sizeof(front_af_keys[0]); i++) {
		CFStringRef k = CFStringCreateWithCString(NULL, front_af_keys[i], kCFStringEncodingUTF8);
		CFTypeRef v = CFDictionaryGetValue(back, k);

		if (v != NULL) {
			CFDictionarySetValue(front, k, v);
		}
		CFRelease(k);
	}
	contrast = CFNumberCreate(NULL, kCFNumberIntType, &one);
	multi = (CFMutableDictionaryRef)CFDictionaryGetValue(front, CFSTR("MultiStream"));
	if (multi != NULL && CFGetTypeID(multi) == CFDictionaryGetTypeID()) {
		formats = (CFMutableDictionaryRef)CFDictionaryGetValue(multi, CFSTR("FigCaptureStreamFormats"));
	}
	if (formats != NULL && CFGetTypeID(formats) == CFDictionaryGetTypeID()) {
		CFIndex count = CFDictionaryGetCount(formats);
		const void **values = calloc((size_t)count, sizeof(void *));

		CFDictionaryGetKeysAndValues(formats, NULL, values);
		for (CFIndex i = 0; i < count; i++) {
			if (values[i] != NULL && CFGetTypeID(values[i]) == CFDictionaryGetTypeID()) {
				CFDictionarySetValue((CFMutableDictionaryRef)values[i], CFSTR("AutoFocusSystem"), contrast);
				n++;
			}
		}
		free(values);
	}
	CFRelease(contrast);
	say("hakios: camera: %s: the front camera focuses (the back camera's focus keys; AutoFocusSystem 1 in %d formats)\n",
	    path, n);
	return copy;
}

static IMP original_dict_url_class, original_dict_file_class, original_dict_url_error_class;
static IMP original_dict_url_init, original_dict_file_init, original_dict_url_error_init;

static id
dict_url_class(id self, SEL selector, id url)
{
	id d = ((id (*)(id, SEL, id))original_dict_url_class)(self, selector, url);
	char path[512];
	CFDictionaryRef p;

	if (capture_plist_path((CFTypeRef)url, path, sizeof(path)) && (p = capture_plist_patched((CFDictionaryRef)d, path))) {
		return (id)CFAutorelease(p);
	}
	return d;
}

static id
dict_file_class(id self, SEL selector, id file)
{
	id d = ((id (*)(id, SEL, id))original_dict_file_class)(self, selector, file);
	char path[512];
	CFDictionaryRef p;

	if (capture_plist_path((CFTypeRef)file, path, sizeof(path)) && (p = capture_plist_patched((CFDictionaryRef)d, path))) {
		return (id)CFAutorelease(p);
	}
	return d;
}

static id
dict_url_error_class(id self, SEL selector, id url, id *error)
{
	id d = ((id (*)(id, SEL, id, id *))original_dict_url_error_class)(self, selector, url, error);
	char path[512];
	CFDictionaryRef p;

	if (capture_plist_path((CFTypeRef)url, path, sizeof(path)) && (p = capture_plist_patched((CFDictionaryRef)d, path))) {
		return (id)CFAutorelease(p);
	}
	return d;
}

static id
dict_url_init(id self, SEL selector, id url)
{
	id d = ((id (*)(id, SEL, id))original_dict_url_init)(self, selector, url);
	char path[512];
	CFDictionaryRef p;

	if (capture_plist_path((CFTypeRef)url, path, sizeof(path)) && (p = capture_plist_patched((CFDictionaryRef)d, path))) {
		CFRelease((CFTypeRef)d);
		return (id)p;
	}
	return d;
}

static id
dict_file_init(id self, SEL selector, id file)
{
	id d = ((id (*)(id, SEL, id))original_dict_file_init)(self, selector, file);
	char path[512];
	CFDictionaryRef p;

	if (capture_plist_path((CFTypeRef)file, path, sizeof(path)) && (p = capture_plist_patched((CFDictionaryRef)d, path))) {
		CFRelease((CFTypeRef)d);
		return (id)p;
	}
	return d;
}

static id
dict_url_error_init(id self, SEL selector, id url, id *error)
{
	id d = ((id (*)(id, SEL, id, id *))original_dict_url_error_init)(self, selector, url, error);
	char path[512];
	CFDictionaryRef p;

	if (capture_plist_path((CFTypeRef)url, path, sizeof(path)) && (p = capture_plist_patched((CFDictionaryRef)d, path))) {
		if (d != NULL) {
			CFRelease((CFTypeRef)d);
		}
		return (id)p;
	}
	return d;
}

static void
hook_one(const char *class_name, bool instance, const char *selector, IMP replacement, IMP *original)
{
	Class cls = objc_getClass(class_name);
	Method m = cls == NULL ? NULL : instance ? class_getInstanceMethod(cls, sel_registerName(selector)) :
	    class_getClassMethod(cls, sel_registerName(selector));

	if (m == NULL) {
		say("hakios: camera: %c[%s %s]: not there\n", instance ? '-' : '+', class_name, selector);
		return;
	}
	*original = method_setImplementation(m, replacement);
}

/* (Foundation is in mediaserverd before this library: it links it) */
static void
front_camera_focus_hooks(void)
{
	hook_one("NSDictionary", false, "dictionaryWithContentsOfURL:", (IMP)dict_url_class, &original_dict_url_class);
	hook_one("NSDictionary", false, "dictionaryWithContentsOfFile:", (IMP)dict_file_class, &original_dict_file_class);
	hook_one("NSDictionary", false, "dictionaryWithContentsOfURL:error:", (IMP)dict_url_error_class,
	    &original_dict_url_error_class);
	hook_one("NSDictionary", true, "initWithContentsOfURL:", (IMP)dict_url_init, &original_dict_url_init);
	hook_one("NSDictionary", true, "initWithContentsOfFile:", (IMP)dict_file_init, &original_dict_file_init);
	hook_one("NSDictionary", true, "initWithContentsOfURL:error:", (IMP)dict_url_error_init, &original_dict_url_error_init);
}

__attribute__((constructor))
static void
hakios_media_init(void)
{
	const char *name = getprogname();
	size_t length = 0;

	if (name == NULL || strcmp(name, "mediaserverd") != 0) {
		return;
	}
	slide = (uintptr_t)_dyld_get_shared_cache_range(&length) - CACHE_BASE;
	crash_watch();
	metal_left_alone = boot_arg("mediamtl=1");
	if (metal_left_alone) {
		say("hakios: mediaserverd: mediamtl=1: the capture stack may have the Metal device (it "
		    "needs AppleCVAPhoto's cvadepth-iOS.metallib in the image)\n");
	} else {
		say("hakios: mediaserverd: the Metal device leaves this process as soon as Metal is in "
		    "it (mediamtl=1 leaves it there)\n");
	}
	torch = open("/dev/hakios-torch", O_WRONLY);
	camera = access("/dev/hakios-camera", F_OK) == 0;
	if (torch >= 0) {
		flashlight_classes();
		say("hakios: mediaserverd: the kernel has a flashlight (/dev/hakios-torch)\n");
	}
	if (camera) {
		say("hakios: mediaserverd: the kernel has a camera (/dev/hakios-camera)\n");
		register_hevc_encoder();
		front_camera_focus_hooks();
	}
	if (boot_arg("mfc=")) {
		pthread_t waiter;

		pthread_create(&waiter, NULL, wait_for_mfc, NULL);
	}
	_dyld_register_func_for_add_image(image_added);         /* also called for every image already there */
}
