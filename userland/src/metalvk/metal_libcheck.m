/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 the HakiOS authors (Scion).
 *
 * metal_libcheck: whether every function of a .metallib becomes the GPU's code on the Metal-on-Vulkan plug-in (src/metalvk/, METAL=vk),
 * before a program that needs that library is given a device. A system library (NanoUniverse's for the Astronomy wallpaper, SiriUICore's
 * for Siri's orb) is AIR that metal2vulkan translates when a pipeline is made; this makes one per function and says which are refused.
 *
 *   metal_libcheck [--bundle DIR] [--format PIXELFORMAT] LIBRARY.metallib...
 *
 * The device is made as metal_tri makes it (the plug-in's class, by hand: run as metal_libcheck, named in HakiosMetal.clients beside the
 * bundle). For each library: every kernel function is a compute pipeline; every fragment function is tried with the library's vertex
 * functions, the one whose name shares the longest beginning with it first, until one makes a pipeline with it (a colour attachment of
 * PIXELFORMAT, default 80, BGRA8Unorm, on each of the first four indices and a Depth32Float attachment); a vertex function that no
 * fragment function took is tried the same way with the fragment functions. A function that no partner pairs with is listed with the
 * last refusal; that may be the pairing, not the function. The function constants a function declares are all set to zero (a program
 * sets its own), so what is checked is the translation of the function, not one particular variant.
 */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <IOKit/IOKitLib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

@interface NSObject (HakiosMetalPlugin)
- (id)initWithAcceleratorPort:(unsigned)port;
@end

static id<MTLDevice>
make_device(const char *bundle_path)
{
	id<MTLDevice> device = MTLCreateSystemDefaultDevice();

	if (device != nil) {
		return device;
	}
	NSBundle *bundle = [NSBundle bundleWithPath:@(bundle_path)];
	NSError *error = nil;
	if (bundle == nil || ![bundle loadAndReturnError:&error]) {
		printf("metal_libcheck: the bundle at %s did not load: %s\n", bundle_path, error.description.UTF8String ?: "no bundle");
		return nil;
	}
	Class cls = NSClassFromString(@"HakiosMetalDevice");
	io_service_t service = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("IOAcceleratorES"));
	if (cls == Nil || service == IO_OBJECT_NULL) {
		printf("metal_libcheck: no class HakiosMetalDevice (is metal_libcheck in HakiosMetal.clients?) or no IOAcceleratorES\n");
		return nil;
	}
	/* an init, as Metal sends it: the plug-in keeps the allocation it does not return (see metal_tri.m) */
	device = [[cls alloc] initWithAcceleratorPort:service];
	IOObjectRelease(service);
	return device;
}

static size_t
common_prefix(NSString *a, NSString *b)
{
	size_t n = 0, la = a.length, lb = b.length;

	while (n < la && n < lb && [a characterAtIndex:n] == [b characterAtIndex:n]) {
		n++;
	}
	return n;
}

/* the frontend's own (NativeBridgeMetalFrontend.mm, its shader priming): a function with each of its function constants set to zero */
@interface NSObject (HakiosMetalLibrary)
- (id<MTLFunction>)nb_newFunctionWithZeroConstants:(NSString *)name;
@end

/* NAME of LIBRARY, its function constants (if it declares any) all zero where the plug-in can do that */
static id<MTLFunction>
function_named(id<MTLLibrary> library, NSString *name, NSError **error)
{
	id<MTLFunction> function = nil;

	if ([(id)library respondsToSelector:@selector(nb_newFunctionWithZeroConstants:)]) {
		function = [(id)library nb_newFunctionWithZeroConstants:name];
	}
	if (function == nil) {
		function = [library newFunctionWithName:name];
	}
	if (function == nil && error != NULL) {
		*error = [NSError errorWithDomain:@"metal_libcheck" code:1 userInfo:@{NSLocalizedDescriptionKey: @"the library refused it"}];
	}
	return function;
}

/* a render pipeline of VERTEX and FRAGMENT into the test's attachments, or nil and ERROR */
static id<MTLRenderPipelineState>
pair(id<MTLDevice> device, id<MTLFunction> vertex, id<MTLFunction> fragment, MTLPixelFormat format, NSError **error)
{
	MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];

	pd.vertexFunction = vertex;
	pd.fragmentFunction = fragment;
	for (unsigned i = 0; i < 4; i++) {
		pd.colorAttachments[i].pixelFormat = format;
	}
	pd.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
	return [device newRenderPipelineStateWithDescriptor:pd error:error];
}

/* FUNCTIONS ordered by how long a beginning of its name each shares with NAME, longest first */
static NSArray<id<MTLFunction>> *
nearest(NSArray<id<MTLFunction>> *functions, NSString *name)
{
	return [functions sortedArrayUsingComparator:^NSComparisonResult(id<MTLFunction> a, id<MTLFunction> b) {
		size_t pa = common_prefix(a.name, name), pb = common_prefix(b.name, name);
		return pa > pb ? NSOrderedAscending : pa < pb ? NSOrderedDescending : NSOrderedSame;
	}];
}

static const char *
reason(NSError *error)
{
	NSString *text = error.localizedDescription ?: @"no error given";

	return [[text stringByReplacingOccurrencesOfString:@"\n" withString:@" "] UTF8String];
}

/* the functions of one library through pipelines; how many were refused */
static unsigned
check(id<MTLDevice> device, const char *path, MTLPixelFormat format)
{
	NSError *error = nil;
	id<MTLLibrary> library = [device newLibraryWithURL:[NSURL fileURLWithPath:@(path)] error:&error];
	NSMutableArray<id<MTLFunction>> *vertex = [NSMutableArray new], *fragment = [NSMutableArray new], *kernel = [NSMutableArray new];
	unsigned made = 0, refused = 0;
	CFAbsoluteTime began = CFAbsoluteTimeGetCurrent();

	if (library == nil) {
		printf("FAIL  %s: no library: %s\n", path, reason(error));
		return 1;
	}
	for (NSString *name in [library.functionNames sortedArrayUsingSelector:@selector(compare:)]) {
		error = nil;
		id<MTLFunction> function = function_named(library, name, &error);

		if (function == nil) {
			printf("FAIL  %s: no function: %s\n", name.UTF8String, reason(error));
			refused++;
		} else if (function.functionType == MTLFunctionTypeVertex) {
			[vertex addObject:function];
		} else if (function.functionType == MTLFunctionTypeFragment) {
			[fragment addObject:function];
		} else if (function.functionType == MTLFunctionTypeKernel) {
			[kernel addObject:function];
		} else {
			printf("skip  %s: function type %lu\n", name.UTF8String, (unsigned long)function.functionType);
		}
	}
	printf("%s: %lu functions (%lu vertex, %lu fragment, %lu kernel)\n", path, (unsigned long)library.functionNames.count,
	    (unsigned long)vertex.count, (unsigned long)fragment.count, (unsigned long)kernel.count);
	for (id<MTLFunction> function in kernel) {
		error = nil;
		if ([device newComputePipelineStateWithFunction:function error:&error] != nil) {
			printf("ok    kernel   %s\n", function.name.UTF8String);
			made++;
		} else {
			printf("FAIL  kernel   %s: %s\n", function.name.UTF8String, reason(error));
			refused++;
		}
	}
	NSMutableSet<NSString *> *paired = [NSMutableSet new];
	for (id<MTLFunction> function in fragment) {
		const char *with = NULL;

		error = nil;
		for (id<MTLFunction> partner in nearest(vertex, function.name)) {
			if (pair(device, partner, function, format, &error) != nil) {
				with = partner.name.UTF8String;
				[paired addObject:partner.name];
				break;
			}
		}
		if (with != NULL) {
			printf("ok    fragment %s (with %s)\n", function.name.UTF8String, with);
			made++;
		} else {
			printf("FAIL  fragment %s: %s\n", function.name.UTF8String, vertex.count ? reason(error) : "no vertex function to pair");
			refused++;
		}
	}
	for (id<MTLFunction> function in vertex) {
		const char *with = NULL;

		if ([paired containsObject:function.name]) {
			printf("ok    vertex   %s\n", function.name.UTF8String);
			made++;
			continue;
		}
		error = nil;
		for (id<MTLFunction> partner in nearest(fragment, function.name)) {
			if (pair(device, function, partner, format, &error) != nil) {
				with = partner.name.UTF8String;
				break;
			}
		}
		if (with != NULL) {
			printf("ok    vertex   %s (with %s)\n", function.name.UTF8String, with);
			made++;
		} else {
			printf("FAIL  vertex   %s: %s\n", function.name.UTF8String, fragment.count ? reason(error) : "no fragment function to pair");
			refused++;
		}
	}
	printf("%s: %u made, %u refused, in %.1f s\n", path, made, refused, CFAbsoluteTimeGetCurrent() - began);
	return refused;
}

int
main(int argc, char *argv[])
{
	setvbuf(stdout, NULL, _IOLBF, 0);
	@autoreleasepool {
		const char *bundle_path = "/redwood/lib/HakiosMetal.bundle";
		MTLPixelFormat format = MTLPixelFormatBGRA8Unorm;
		unsigned refused = 0;
		int first = 1;

		for (; first < argc && strncmp(argv[first], "--", 2) == 0; first++) {
			if (strcmp(argv[first], "--bundle") == 0 && first + 1 < argc) {
				bundle_path = argv[++first];
			} else if (strcmp(argv[first], "--format") == 0 && first + 1 < argc) {
				format = (MTLPixelFormat)strtoul(argv[++first], NULL, 0);
			} else {
				break;
			}
		}
		if (first >= argc) {
			fprintf(stderr, "usage: metal_libcheck [--bundle DIR] [--format PIXELFORMAT] LIBRARY.metallib...\n");
			return 2;
		}
		id<MTLDevice> device = make_device(bundle_path);
		if (device == nil) {
			return 1;
		}
		printf("metal_libcheck: device %s\n", device.name.UTF8String);
		for (int i = first; i < argc; i++) {
			refused += check(device, argv[i], format);
		}
		return refused != 0;
	}
}
