/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 the HakiOS authors (Scion).
 *
 * metal_tri: a triangle through the Metal API on the Metal-on-Vulkan plug-in (src/metalvk/, METAL=vk), on the phone, checked pixel by
 * pixel. The first step of Metal for applications on the Galaxy S9 (docs/metal.md): MTLDevice, library, render pipeline, command queue,
 * render pass, read back, on metal2vulkan's runtime over PanVK on the Mali-G72.
 *
 *   metal_tri [--bundle DIR] [--metallib FILE] [--size N] [--out FILE.ppm] [--surface]
 *
 * The device: MTLCreateSystemDefaultDevice() first, Metal's own discovery (the kernel names the plug-in with boot-arg metal=N). When it
 * answers nil, the plug-in at --bundle (default /redwood/lib/HakiosMetal.bundle) is loaded and its class made the way Metal makes it,
 * -initWithAcceleratorPort: on the IOAcceleratorES service; so this runs without any boot-arg, in this process only, and nothing else on
 * the phone is shown a device. The bundle registers its class only in a process it is shown to (src/hakios_metal_shown.c): put
 * "metal_tri" in HakiosMetal.clients beside it.
 *
 * The shaders: tools/metal/air/tri_vertex.ll and tri_fragment.ll, AIR written by hand and put in a library by tools/metal/mkmetallib.py
 * (no Xcode needed); default /redwood/share/metal_tri.metallib. The vertex stage makes the three corners from the vertex id (bottom
 * left red, bottom right green, top blue), so the draw needs no buffer.
 *
 * What is checked, on a N x N BGRA8 target cleared to (0.1, 0.1, 0.3): every pixel whose centre is clearly inside the triangle is
 * not the clear colour and its colour is the barycentric mix of the corners within 8/255 per channel; every pixel clearly outside is
 * the clear colour exactly; pixels near an edge are not judged. --surface renders into a texture over an IOSurface (what a
 * CAMetalLayer's drawable is) and reads the surface's own memory, instead of a Shared texture's getBytes:. The picture is written as a
 * PPM with --out.
 */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>
#import <UIKit/UIKit.h>
#import <IOSurface/IOSurfaceRef.h>
#include <IOKit/IOKitLib.h>
#include <execinfo.h>
#include <math.h>
#include <signal.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const double clear_rgb[3] = { 0.1, 0.1, 0.3 };

/* the triangle of tri_vertex.ll in normalised device coordinates, y up */
static const double corner[3][2] = { { -0.8, -0.8 }, { 0.8, -0.8 }, { 0.0, 0.8 } };

/* what Metal sends a plug-in's device class (MetalPluginClassName) */
@interface NSObject (HakiosMetalPlugin)
- (id)initWithAcceleratorPort:(unsigned)port;
@end

static id<MTLDevice>
make_device(const char *bundle_path)
{
	id<MTLDevice> device = MTLCreateSystemDefaultDevice();
	if (device != nil) {
		printf("metal_tri: device by Metal's own discovery: %s\n", device.name.UTF8String);
		return device;
	}
	printf("metal_tri: MTLCreateSystemDefaultDevice() is nil (no metal=N): the plug-in at %s, by hand\n", bundle_path);
	NSBundle *bundle = [NSBundle bundleWithPath:@(bundle_path)];
	NSError *error = nil;
	if (bundle == nil || ![bundle loadAndReturnError:&error]) {
		printf("metal_tri: FAIL: the bundle did not load: %s\n", error.description.UTF8String ?: "no bundle");
		return nil;
	}
	Class cls = NSClassFromString(@"HakiosMetalDevice");
	if (cls == Nil) {
		printf("metal_tri: FAIL: no class HakiosMetalDevice (is metal_tri in HakiosMetal.clients beside the bundle?)\n");
		return nil;
	}
	io_service_t service = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("IOAcceleratorES"));
	if (service == IO_OBJECT_NULL) {
		printf("metal_tri: FAIL: no IOAcceleratorES service\n");
		return nil;
	}
	/*
	 * Sent as an init (the category below), as Metal sends it: the receiver is consumed and the result returned retained. The plug-in
	 * returns its frontend's device in place of the object allocated here and keeps that one on purpose (HakiosMetalVK.mm); a caller
	 * that released it as well (a cast function pointer under ARC does) would free a _MTLDevice whose -init never ran, which faults.
	 */
	device = [[cls alloc] initWithAcceleratorPort:service];
	IOObjectRelease(service);
	if (device == nil) {
		printf("metal_tri: FAIL: -initWithAcceleratorPort: gave nil (the plug-in's log says why)\n");
		return nil;
	}
	printf("metal_tri: device by the plug-in's own class: %s\n", device.name.UTF8String);
	return device;
}

/* barycentric coordinates of the pixel centre (px, py), y down, on an n x n target */
static void
barycentric(double px, double py, unsigned n, double w[3])
{
	double x = (px + 0.5) / n * 2 - 1, y = 1 - (py + 0.5) / n * 2;
	double x0 = corner[0][0], y0 = corner[0][1], x1 = corner[1][0], y1 = corner[1][1], x2 = corner[2][0], y2 = corner[2][1];
	double d = (y1 - y2) * (x0 - x2) + (x2 - x1) * (y0 - y2);
	w[0] = ((y1 - y2) * (x - x2) + (x2 - x1) * (y - y2)) / d;
	w[1] = ((y2 - y0) * (x - x2) + (x0 - x2) * (y - y2)) / d;
	w[2] = 1 - w[0] - w[1];
}

/* METAL_TRI_STEPS=1: each call said before it is made, so that a fault names the call it is in */
static void
step(const char *what)
{
	if (getenv("METAL_TRI_STEPS") != NULL) {
		printf("      -> %s\n", what);
	}
}

/* a fault in the runtime or the driver: where, by image and symbol, then the default action */
static void
fault(int sig)
{
	void *frames[48];
	int n = backtrace(frames, 48);
	dprintf(2, "metal_tri: signal %d, called from:\n", sig);
	backtrace_symbols_fd(frames, n, 2);
	signal(sig, SIG_DFL);
	raise(sig);
}

/* the library and the render pipeline of tri_vertex and tri_fragment, into BGRA8Unorm */
static id<MTLRenderPipelineState>
make_pipeline(id<MTLDevice> device, const char *metallib)
{
	NSError *error = nil;
	step("newLibraryWithURL:");
	id<MTLLibrary> library = [device newLibraryWithURL:[NSURL fileURLWithPath:@(metallib)] error:&error];
	if (library == nil) {
		printf("metal_tri: FAIL: newLibraryWithURL: %s: %s\n", metallib, error.description.UTF8String);
		return nil;
	}
	step("functionNames");
	printf("ok    library: %s\n", [[library.functionNames componentsJoinedByString:@" "] UTF8String]);
	step("newFunctionWithName: and the pipeline descriptor");
	MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
	pd.vertexFunction = [library newFunctionWithName:@"tri_vertex"];
	pd.fragmentFunction = [library newFunctionWithName:@"tri_fragment"];
	pd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
	if (pd.vertexFunction == nil || pd.fragmentFunction == nil) {
		printf("metal_tri: FAIL: tri_vertex or tri_fragment is not in the library\n");
		return nil;
	}
	CFAbsoluteTime t0 = CFAbsoluteTimeGetCurrent();
	step("newRenderPipelineStateWithDescriptor:error:");
	id<MTLRenderPipelineState> pipeline = [device newRenderPipelineStateWithDescriptor:pd error:&error];
	if (pipeline == nil) {
		printf("metal_tri: FAIL: newRenderPipelineStateWithDescriptor: %s\n", error.description.UTF8String);
		return nil;
	}
	printf("ok    render pipeline (AIR to SPIR-V to the GPU's code) in %.0f ms\n", (CFAbsoluteTimeGetCurrent() - t0) * 1000);
	return pipeline;
}

/* one render pass into target: a clear and the triangle; committed and waited for */
static bool
render(id<MTLCommandQueue> queue, id<MTLRenderPipelineState> pipeline, id<MTLTexture> target, const double clear[3], bool quiet)
{
	step("render pass, command buffer, encoder");
	MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
	rp.colorAttachments[0].texture = target;
	rp.colorAttachments[0].loadAction = MTLLoadActionClear;
	rp.colorAttachments[0].storeAction = MTLStoreActionStore;
	rp.colorAttachments[0].clearColor = MTLClearColorMake(clear[0], clear[1], clear[2], 1);
	id<MTLCommandBuffer> cb = [queue commandBuffer];
	id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
	[enc setRenderPipelineState:pipeline];
	[enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
	[enc endEncoding];
	CFAbsoluteTime t0 = CFAbsoluteTimeGetCurrent();
	step("commit and wait");
	[cb commit];
	[cb waitUntilCompleted];
	bool done = cb.status == MTLCommandBufferStatusCompleted;
	if (!quiet || !done) {
		printf("%s render pass: status %ld in %.1f ms%s%s\n", done ? "ok   " : "FAIL ", (long)cb.status,
		    (CFAbsoluteTimeGetCurrent() - t0) * 1000, cb.error ? ": " : "", cb.error ? cb.error.description.UTF8String : "");
	}
	return done;
}

/* a BGRA8 texture of n x n, over the IOSurface when one is given */
static id<MTLTexture>
make_target(id<MTLDevice> device, unsigned n, IOSurfaceRef ios)
{
	MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:n height:n
	    mipmapped:NO];
	td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
	td.storageMode = MTLStorageModeShared;
	return ios != NULL ? [device newTextureWithDescriptor:td iosurface:ios plane:0] : [device newTextureWithDescriptor:td];
}

static IOSurfaceRef
make_surface(unsigned n)
{
	NSDictionary *props = @{ (id)kIOSurfaceWidth: @(n), (id)kIOSurfaceHeight: @(n), (id)kIOSurfaceBytesPerElement: @4,
		(id)kIOSurfaceBytesPerRow: @(n * 4), (id)kIOSurfacePixelFormat: @((unsigned)'BGRA') };
	return IOSurfaceCreate((__bridge CFDictionaryRef)props);
}

/* the surface's rows into pixels (n x n x 4, tightly packed) */
static void
read_surface(IOSurfaceRef ios, unsigned char *pixels, unsigned n)
{
	IOSurfaceLock(ios, kIOSurfaceLockReadOnly, NULL);
	const unsigned char *base = IOSurfaceGetBaseAddress(ios);
	size_t row = IOSurfaceGetBytesPerRow(ios);
	for (unsigned y = 0; y < n; y++) {
		memcpy(pixels + (size_t)y * n * 4, base + y * row, (size_t)n * 4);
	}
	IOSurfaceUnlock(ios, kIOSurfaceLockReadOnly, NULL);
}

/*
 * Every pixel whose centre is clearly inside the triangle has the barycentric mix of the corners within 8/255 per channel; every pixel
 * clearly outside is the clear colour exactly; pixels near an edge are not judged. Returns the number wrong.
 */
static unsigned
judge(const unsigned char *pixels, unsigned n, const double clear[3], unsigned *inside, unsigned *outside, bool say)
{
	unsigned bad_in = 0, bad_out = 0;
	unsigned char want_clear[3] = { (unsigned char)lround(clear[2] * 255), (unsigned char)lround(clear[1] * 255),
		(unsigned char)lround(clear[0] * 255) };   /* B G R */
	*inside = *outside = 0;
	for (unsigned y = 0; y < n; y++) {
		for (unsigned x = 0; x < n; x++) {
			const unsigned char *p = pixels + ((size_t)y * n + x) * 4;
			double w[3];
			barycentric(x, y, n, w);
			double margin = 2.5 / n;                 /* about a pixel and a quarter in barycentric terms */
			if (w[0] > margin && w[1] > margin && w[2] > margin) {
				(*inside)++;
				double want[3] = { w[2] * 255, w[1] * 255, w[0] * 255 };     /* B G R from (r, g, b) = (w0, w1, w2) */
				bool ok = true;
				for (int c = 0; c < 3; c++) {
					ok = ok && fabs(p[c] - want[c]) <= 8;
				}
				if (!ok && bad_in++ < 4 && say) {
					printf("      inside (%u,%u): BGRA %u %u %u %u, want about %.0f %.0f %.0f 255\n", x, y, p[0], p[1], p[2], p[3],
					    want[0], want[1], want[2]);
				}
			} else if (w[0] < -margin || w[1] < -margin || w[2] < -margin) {
				(*outside)++;
				if ((p[0] != want_clear[0] || p[1] != want_clear[1] || p[2] != want_clear[2]) && bad_out++ < 4 && say) {
					printf("      outside (%u,%u): BGRA %u %u %u %u, want the clear colour %u %u %u 255\n", x, y, p[0], p[1], p[2],
					    p[3], want_clear[0], want_clear[1], want_clear[2]);
				}
			}
		}
	}
	return bad_in + bad_out;
}

/* ------------------------------------------------------------------------------------------------------------------ the application */

/*
 * Run from MetalTri.app (a bundle whose identifier is com.hakios.metaltri), with no arguments: a window whose layer shows, as its
 * contents, an IOSurface the Metal device has just rendered into, two surfaces in turn, a frame every half second with the clear colour
 * changing, every frame judged as above and the tally written under it. So the pixels on the panel are Metal's, through the window
 * server's OpenGL ES compositor, which takes the surface as any layer's contents. (A CAMetalLayer and its drawables are the next step:
 * they need the device registered with Metal, MTLAddDevice, which discovery does with boot-arg metal=N.)
 * The plug-in, its runtime, the Vulkan driver and the library are taken from the application's own directory (HakiosMetal.bundle,
 * HakiosMetal.icd naming libvulkan_panfrost.dylib there, HakiosMetal.clients naming metal_tri, metal_tri.metallib), so that the test
 * needs nothing installed in the system image.
 */
@interface MTApp : UIResponder <UIApplicationDelegate>
@property (strong) UIWindow *window;
@end

@interface MTView : UIViewController
@end

static struct {
	id<MTLDevice> device;
	id<MTLCommandQueue> queue;
	id<MTLRenderPipelineState> pipeline;
	IOSurfaceRef surface[2];
	id<MTLTexture> target[2];
	unsigned n, frame, good, bad;
	unsigned char *pixels;
	CALayer *picture;
	UILabel *label;
	NSString *status;
} app;

@implementation MTView
- (void)viewDidLoad
{
	[super viewDidLoad];
	self.view.backgroundColor = UIColor.blackColor;
	CGRect b = self.view.bounds;
	CGFloat side = MIN(b.size.width, b.size.height) - 32;
	app.picture = [CALayer layer];
	app.picture.frame = CGRectMake((b.size.width - side) / 2, 120, side, side);
	app.picture.magnificationFilter = kCAFilterNearest;
	[self.view.layer addSublayer:app.picture];
	app.label = [[UILabel alloc] initWithFrame:CGRectMake(16, 140 + side, b.size.width - 32, b.size.height - 160 - side)];
	app.label.numberOfLines = 0;
	app.label.textColor = UIColor.whiteColor;
	app.label.font = [UIFont monospacedSystemFontOfSize:15 weight:UIFontWeightRegular];
	[self.view addSubview:app.label];
	UILabel *title = [[UILabel alloc] initWithFrame:CGRectMake(16, 50, b.size.width - 32, 60)];
	title.numberOfLines = 2;
	title.textColor = UIColor.whiteColor;
	title.font = [UIFont boldSystemFontOfSize:20];
	title.text = @"Metal on the Mali-G72\nmetal2vulkan over PanVK";
	[self.view addSubview:title];
	app.label.text = app.status;
	if (app.pipeline != nil) {
		[NSTimer scheduledTimerWithTimeInterval:0.5 target:self selector:@selector(tick:) userInfo:nil repeats:YES];
		[self tick:nil];
	}
}

- (void)tick:(NSTimer *)timer
{
	unsigned i = app.frame++ & 1;
	double hue = (app.frame % 12) / 12.0, clear[3];
	for (int c = 0; c < 3; c++) {           /* a dim colour going round the wheel */
		double h = fmod(hue + c / 3.0, 1.0);
		clear[c] = 0.08 + 0.22 * (0.5 + 0.5 * cos(h * 2 * M_PI));
		clear[c] = round(clear[c] * 255) / 255;
	}
	CFAbsoluteTime t0 = CFAbsoluteTimeGetCurrent();
	bool done = render(app.queue, app.pipeline, app.target[i], clear, true);
	double ms = (CFAbsoluteTimeGetCurrent() - t0) * 1000;
	unsigned inside = 0, outside = 0, wrong = 1;
	if (done) {
		read_surface(app.surface[i], app.pixels, app.n);
		wrong = judge(app.pixels, app.n, clear, &inside, &outside, false);
	}
	if (done && wrong == 0) {
		app.good++;
	} else {
		app.bad++;
	}
	[CATransaction begin];
	[CATransaction setDisableActions:YES];
	app.picture.contents = (__bridge id)app.surface[i];
	[CATransaction commit];
	app.label.text = [NSString stringWithFormat:@"%@\n\nframe %u: %s in %.1f ms\n%u px inside, %u outside judged, %u wrong\nframes right %u, wrong %u",
	    app.status, app.frame, done ? "rendered" : "FAILED", ms, inside, outside, wrong, app.good, app.bad];
	if (app.frame <= 2 || app.frame % 20 == 0) {
		NSLog(@"metal_tri: frame %u: %s, %.1f ms, %u inside %u outside judged, %u wrong; right %u wrong %u", app.frame,
		    done ? "rendered" : "FAILED", ms, inside, outside, wrong, app.good, app.bad);
	}
}
@end

@implementation MTApp
- (BOOL)application:(UIApplication *)application didFinishLaunchingWithOptions:(NSDictionary *)options
{
	NSString *here = NSBundle.mainBundle.bundlePath;
	app.n = 512;
	app.device = make_device([here stringByAppendingPathComponent:@"HakiosMetal.bundle"].UTF8String);
	if (app.device != nil) {
		app.pipeline = make_pipeline(app.device, [here stringByAppendingPathComponent:@"metal_tri.metallib"].UTF8String);
		app.queue = [app.device newCommandQueue];
		for (int i = 0; i < 2 && app.pipeline != nil; i++) {
			app.surface[i] = make_surface(app.n);
			app.target[i] = app.surface[i] ? make_target(app.device, app.n, app.surface[i]) : nil;
			if (app.target[i] == nil) {
				app.pipeline = nil;
			}
		}
		app.pixels = calloc(app.n, (size_t)app.n * 4);
	}
	app.status = app.pipeline != nil ? [NSString stringWithFormat:@"device: %@\n%ux%u BGRA8 IOSurfaces, rendered by Metal, shown as a layer's contents",
	    app.device.name, app.n, app.n] : @"no Metal device or pipeline: see the log (metal_tri:, HakiosMetal:, native-metal:)";
	NSLog(@"metal_tri: %@", app.status);
	self.window = [[UIWindow alloc] initWithFrame:UIScreen.mainScreen.bounds];
	self.window.rootViewController = [MTView new];
	[self.window makeKeyAndVisible];
	return YES;
}
@end

int
main(int argc, char *argv[])
{
	setvbuf(stdout, NULL, _IOLBF, 0);
	signal(SIGSEGV, fault);
	signal(SIGBUS, fault);
	signal(SIGABRT, fault);
	@autoreleasepool {
		if (argc == 1 && [NSBundle.mainBundle.bundleIdentifier isEqualToString:@"com.hakios.metaltri"]) {
			return UIApplicationMain(argc, argv, nil, NSStringFromClass([MTApp class]));
		}
		const char *bundle_path = "/redwood/lib/HakiosMetal.bundle", *metallib = "/redwood/share/metal_tri.metallib", *out = NULL;
		unsigned n = 256;
		bool surface = false;
		for (int i = 1; i < argc; i++) {
			if (strcmp(argv[i], "--bundle") == 0 && i + 1 < argc) {
				bundle_path = argv[++i];
			} else if (strcmp(argv[i], "--metallib") == 0 && i + 1 < argc) {
				metallib = argv[++i];
			} else if (strcmp(argv[i], "--size") == 0 && i + 1 < argc) {
				n = (unsigned)atoi(argv[++i]);
			} else if (strcmp(argv[i], "--out") == 0 && i + 1 < argc) {
				out = argv[++i];
			} else if (strcmp(argv[i], "--surface") == 0) {
				surface = true;
			} else {
				fprintf(stderr, "usage: metal_tri [--bundle DIR] [--metallib FILE] [--size N] [--out FILE.ppm] [--surface]\n");
				return 2;
			}
		}
		if (n < 16 || n > 4096) {
			n = 256;
		}
		id<MTLDevice> device = make_device(bundle_path);
		if (device == nil) {
			return 1;
		}
		id<MTLRenderPipelineState> pipeline = make_pipeline(device, metallib);
		if (pipeline == nil) {
			return 1;
		}
		IOSurfaceRef ios = surface ? make_surface(n) : NULL;
		id<MTLTexture> target = (!surface || ios != NULL) ? make_target(device, n, ios) : nil;
		if (target == nil) {
			printf("metal_tri: FAIL: no %ux%u BGRA8 %s\n", n, n, surface ? "texture over an IOSurface" : "Shared texture");
			return 1;
		}
		if (!render([device newCommandQueue], pipeline, target, clear_rgb, false)) {
			return 1;
		}
		unsigned char *pixels = calloc(n, (size_t)n * 4);
		if (surface) {
			read_surface(ios, pixels, n);
		} else {
			[target getBytes:pixels bytesPerRow:(size_t)n * 4 fromRegion:MTLRegionMake2D(0, 0, n, n) mipmapLevel:0];
		}
		unsigned inside, outside, wrong = judge(pixels, n, clear_rgb, &inside, &outside, true);
		bool pass = inside > 0 && wrong == 0;
		printf("%s triangle: %ux%u %s, %u pixels inside and %u outside judged, %u wrong\n", pass ? "ok   " : "FAIL ", n, n,
		    surface ? "read from the IOSurface's own memory" : "read with getBytes:", inside, outside, wrong);
		if (out != NULL) {
			FILE *f = fopen(out, "wb");
			if (f != NULL) {
				fprintf(f, "P6\n%u %u\n255\n", n, n);
				for (size_t i = 0; i < (size_t)n * n; i++) {
					unsigned char rgb[3] = { pixels[i * 4 + 2], pixels[i * 4 + 1], pixels[i * 4] };
					fwrite(rgb, 1, 3, f);
				}
				fclose(f);
				printf("      picture: %s\n", out);
			}
		}
		free(pixels);
		if (ios != NULL) {
			CFRelease(ios);
		}
		printf("metal_tri: %s\n", pass ? "PASS" : "FAIL");
		return pass ? 0 : 1;
	}
}
