// SPDX-License-Identifier: MIT
// Copyright (c) 2026 NativeMetal contributors. From AdrenoMetal (https://github.com/UniXNU/AdrenoMetal) at ce5e19e33e05, NativeBridgeMetalFrontend.h;
// MIT licence, its text in LICENSE.AdrenoMetal beside this file. Adopted for Scion's plug-in (HakiosMetal.bundle with METAL=vk,
// userland/src/metalvk/HakiosMetalVK.mm; changes by the HakiOS authors, MIT, each marked "Scion:").
// Entry points of the native Metal frontend over the bridge (NativeBridgeMetalFrontend.mm).
#pragma once
#include <stddef.h>
#include <stdint.h>
#ifdef __OBJC__
#import <Foundation/Foundation.h>
extern "C" {
// Opens the process's one device: loader exports vkGetInstanceProcAddr (Turnip), cache is a writable shader-cache directory, registryID is the
// accelerator service's registry entry. missing (may be null) is told the class and selector of every unimplemented call.
id native_bridge_metal_open(const char *loader, const char *cache, uint64_t registryID, void (*missing)(const char *, const char *));
// Scion: the same device for an object of the plug-in's own _MTLDevice subclass that Metal holds already (HakiosMetalVK.mm): this file's
// methods go onto that object's class and the object becomes the device. True when it is (or already was) the process's device.
bool native_bridge_metal_adopt(id object, const char *loader, const char *cache, uint64_t registryID, void (*missing)(const char *, const char *));
// Scion: told every line the frontend logs (may be null).
extern void (*native_bridge_metal_log_hook)(const char *);
// Bare AIR bitcode as a library (the bridge's fixtures; Metal itself accepts only metallibs).
id native_bridge_metal_library_with_air(const void *bytes, size_t length);
// One line for the plugin's status report: command buffers committed/failed locally, pipelines made, refusals, the last refusal's text.
size_t native_bridge_metal_status(char *out, size_t size);
// The pixel formats of the textures asked for (first eight distinct) and how many texture requests the runtime refused.
size_t native_bridge_metal_formats(char *out, size_t size);
// The process's first problem (a refusal or a missing call) into out, and how many problems there were.
uint64_t native_bridge_metal_first_problem(char *out, size_t size);
// Called once, on the thread that hit it, with the first problem's text (null: not called).
extern void (*native_bridge_metal_problem_hook)(const char *);
size_t native_bridge_metal_telemetry(char *out, size_t size);
// Structured cumulative artifact/driver-cache counters for offline qualification.
struct NmbShaderCacheStats;
bool native_bridge_metal_shader_cache_stats(struct NmbShaderCacheStats *out);
bool native_bridge_metal_work_stats(uint64_t out[10]);
// The same fixed CPU, memory and pipeline workload on the tablet and the host, so their times compare directly (int-145): compute, first-touch
// page faults, memset bandwidth, CommonCrypto SHA-256, then the first `pairs` vertex/fragment pairs of the given QuartzCore metallib made into
// BGRA8 render states and drawn once each (pipeline_timing_host's draw=1 order). The results become the telemetry's Y rows; also returned.
size_t native_bridge_metal_benchmark(id device, const char *metallib, unsigned pairs, char *out, size_t size);
}
#endif
