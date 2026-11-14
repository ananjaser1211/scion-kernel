# Metal on the Galaxy S9's Mali-G72: PanVK and Metal on Vulkan

This page is about one question: can Metal on the S9 run on the GPU through PanVK (Mesa's Vulkan driver for the Mali) with a
Metal-on-Vulkan layer on top, and what would it take? It starts with the licences, then what Metal needs on iOS 16.5 and where
our code plugs in, then what was built and checked on 7 November 2026, then a plan with estimates.

Read with it: [metal-plan.md](metal-plan.md) (the Metal plug-in, both of its builds, and the Metal-on-Vulkan runtime it already
carries), the S9 repository's `GPU.md` (the OpenGL ES path through Panfrost, and the Gallium route to Metal), and
[air.md](air.md) (Apple's shader bitcode into NIR).

**Short answer.** Yes, it is feasible on this GPU, and as of this page PanVK builds for our userland and runs on a Mac against a
stand-in GPU. It has not run on the phone. PanVK supports the G72 only behind Mesa's own "experimental" switch, and reports Vulkan
1.0 there. But it already offers as extensions the features a Metal-on-Vulkan runtime needs (timeline semaphores, buffer device
addresses, synchronization2, dynamic rendering). A separate S9 project has run this exact stack on the same GPU under iOS 26 (see
"What others have shown"). On iOS 16.5 the window server cannot use Metal either way: QuartzCore's `default.metallib` is not in the
image ([metal-plan.md](metal-plan.md#what-steps-1-and-2-found-qemu-27-october-2026-metal1-a-clean-image)). So this route speeds up
Metal clients (CoreImage, RenderBox, apps), not the compositor, which stays on OpenGL ES through Panfrost.

## Licences

Nothing below is committed to this repository unless the last column says so. Mesa and metal2vulkan are fetched at a pinned
release, patched and built on the build machine. Apple's files are never in the repository.

| Component | What it does here | Licence | How it is used |
|---|---|---|---|
| Mesa 25.0.7: PanVK (`src/panfrost/vulkan`), the Vulkan runtime, NIR, the Bifrost compiler, `pan_kmod` | the Vulkan driver for the Mali | MIT (Mesa's per-file SPDX; the Khronos headers are Apache-2.0 OR MIT) | fetched by `tools/mk-panvk.sh`, patched with `tools/mesa/*.patch` and `tools/mesa/panvk/*.patch`, built into `libvulkan_panfrost.dylib`; never committed |
| `tools/mesa/panvk/panvk-darwin.patch` | builds PanVK on Darwin over our libdrm stand-in | MIT (Mesa's, as the files it changes) | committed; written for Scion |
| `tools/mesa/hakios-drm` (the libdrm stand-in) | Linux DRM calls as IOKit calls to HakiosMali; device enumeration, sync objects, the no-op GPU | MIT (ours) | committed |
| `tools/vk/vk_smoke.c`, `tools/mk-panvk.sh`, `userland/rc.jobs.vksmoke` | the build and the first test | MIT (ours) | committed |
| Mesa's drm-shim (`src/panfrost/drm-shim/panfrost_noop.c`) | the idea of a GPU that runs nothing | MIT | not copied; the no-op backend uses the G72 values of our own `tools/mali-drmshim/hakios_mali_shim.c` |
| Mesa 26.2.6 PanVK | a newer PanVK (needed by Starbite for metal2vulkan; see below) | MIT | not built here yet (it needs a host `mesa_clc`: phase 2 of the plan) |
| [metal2vulkan](https://github.com/steelbrain/metal2vulkan) | the Metal API on any Vulkan 1.2 device, and its AIR-to-SPIR-V translator | GPL-2.0-or-later OR LGPL-3.0-or-later | already in the tree's `METAL=vk` build as a separate, replaceable `libmetal2vulkan.dylib` (`tools/metal/mk-metal2vulkan.sh`); never committed; our changes are patch files under its licence |
| AdrenoMetal's bridge and frontend (UniXNU) | the C ABI over metal2vulkan and the `_MTLDevice` subclass | MIT (NativeMetal contributors) | already committed in `tools/metal/bridge/` and `userland/src/metalvk/` with their notice |
| IOS-26-Hot's `HotMetal2` frontend (nithinyadavamaraboina-online/IOS-26-Hot) | another frontend over the same bridge, the one Starbite uses | MIT (dipa-studio) | the frontend that `tools/ios27/metal2/` carries is Starbite's copy of it (with its changes), MIT, with `LICENSE.IOS-26-Hot` beside it |
| Starbite's Metal and PanVK work (`tools/metal2`, `tools/mesa-ios` on the build Mac) | the same stack on the same GPU under iOS 26 | no licence file; its author's permission covers their own code with credit. Their loader files are GPL-2.0-only. | `tools/ios27/metal2/` (the frontend, the bridge's Rust sources and patches, the build scripts; MIT, `LICENSES/MIT-Starbite.txt`, credited in `NOTICE`), imported on 9 November 2026 now that its author's code is licensed for publication (`LICENSES.md`). The loader files stay GPL-2.0-only and `tools/mesa-ios` is not imported here; `tools/ios27/mesa-ios/` has Scion's patches. |
| star2lte port's `metal-wip` | AdrenoMetal frontend plus a metal2vulkan patch | GPL-family patch | summary only ([star2lte-absorb.md](star2lte-absorb.md)) |
| MoltenVK (Apache-2.0), KosmicKrisp (Mesa, MIT), lavapipe (Mesa, MIT) | Vulkan on Metal, or on the CPU: the **reverse** direction | as stated | not part of the phone's stack; Mac-only test drivers for `tools/metal/metalvk-plugin-test.sh` |
| blazer's native Metal (`native/metal/{isa,streams}`) | Metal on the Pixel 10's PowerVR | MIT; its AIR front end is external LGPL-3.0 | not used: another GPU |
| Apple's Metal.framework, `AGXMetalA11.bundle`, QuartzCore's metallibs | Apple's | proprietary | never in the repository; only in the user's own system image |

## What Metal needs on iOS 16.5, and where our code plugs in

There are two levels where Metal could be made to work:

1. **Apple's GPU driver interface.** `AGXMetalA11.bundle` is Apple's userland driver for the A11's GPU. It writes the GPU's own
   command formats and sends them to `AGXAccelerator` in the kernel. Pretending to be that driver would mean decoding Apple's
   command streams and running them on a Mali. That is not possible in any useful sense, and we do not try.
2. **The `MTLDevice` API.** Metal finds its device through the kernel. The `IOAcceleratorES` service names a plug-in bundle and a
   class (`MetalPluginName`, `MetalPluginClassName`). Metal loads that bundle from `/System/Library/Extensions`, makes the class
   (a subclass of the private `_MTLDevice`) with `-initWithAcceleratorPort:`, and from then on every Metal object comes from the
   plug-in. This is where our code goes: `HakiosMetal.bundle`. Its `METAL=vk` build is the frontend and bridge over
   metal2vulkan. The kernel names it only with boot-arg `metal=N`, which is off by default.

So **no Apple file is replaced**. The kernel stops advertising an AGX accelerator, so `AGXMetalA11.bundle` is never loaded. The
plug-in is our own bundle, which `userland/build.sh` installs (as a link from `/redwood/lib`). The installer needs to touch nothing
of Apple's.

What the system asks of the device, from the QEMU runs in [metal-plan.md](metal-plan.md):

- `MTLAddDevice` asserts that the class conforms to `MTLDevice` and the private `MTLDeviceSPI`.
- The first messages are `-featureProfile` and `-maxBufferLength`, then a command queue, which must answer the private
  `MTLCommandQueueSPI` (`-setCompletionQueue:`).
- CoreImage reads `dedicatedMemorySize` and `sharedMemorySize`.
- The window server (`backboardd`, CoreAnimation) then wants QuartzCore's `default.metallib`. iOS 16.5's image does not ship it, so
  CoreAnimation must stay on OpenGL ES. On iOS 26, where the library is in the image, Starbite composites the whole UI through Metal.

What the Metal runtime needs from Vulkan: metal2vulkan opens a Vulkan 1.2 device with timeline semaphores and one queue family with
graphics and compute (`tools/metal/bridge/icd.rs`, [metal-plan.md](metal-plan.md#how-the-plug-in-reaches-the-vulkan-driver)).

## PanVK on the G72

The G72 is **Bifrost architecture 6** (GPU_ID 0x6221), not v7: v7 is the G52, G76 and G51. From Mesa's source:

| | Mesa 25.0.7 (our pin) | Mesa 26.2.6 (latest release, 7 Nov 2026) |
|---|---|---|
| PanVK built for v6 | yes (`foreach arch : [6, 7, 10]`) | yes (`[6, 7, 10, 12, 13, 14]`) |
| v6 accepted | only with `PAN_I_WANT_A_BROKEN_VULKAN_DRIVER=1` ("not well-tested on v6") | the same |
| API version reported on v6 | 1.0 (1.1 on v10+) | 1.0 (1.4 on v10+) |
| `VK_KHR_timeline_semaphore`, `bufferDeviceAddress`, `synchronization2`, `dynamic_rendering` | exposed | exposed |
| `vulkanMemoryModel` | no | yes |
| `descriptorIndexing` | no | v9+ only |
| `shaderFloat16` | no | v10+ only |
| needs a host `mesa_clc` to build | no | yes (`libpan`'s OpenCL kernels) |

[metal-plan.md](metal-plan.md) said on 31 October that "Mesa's main branch gives v6 and v7 Vulkan 1.3". The 26.2.6 release does
not: it gives 1.0 below v10. What a v6 device lacks for a 1.2 *core* device is the version number, not timeline semaphores.
`MESA_VK_VERSION_OVERRIDE=1.2` makes PanVK report 1.2. That claims more than the driver passes in conformance tests, and Starbite
runs metal2vulkan this way (below). Whether every 1.2 core feature that metal2vulkan's shaders use is right on v6 is exactly what
phase 1 and phase 3 of the plan test.

**What PanVK asks of the system that Panfrost (OpenGL ES) did not.** These are the additions made to `tools/mesa/hakios-drm` for
this page:

- **Device enumeration.** `vk_instance.c` lists devices with `drmGetDevices2`, and PanVK `open()`s the render node it is given.
  The stand-in now lists one platform device, the Mali, whose render node is `/dev/null`. It opens the HakiosMali connection on
  the way, so `drmGetVersion` on that descriptor says "panfrost".
- **Sync objects.** Vulkan's common code adds `drmSyncobjSignal`, which is now implemented. Timelines are left to Vulkan's
  emulation (`vk_sync_timeline`): the stand-in reports no `DRM_CAP_SYNCOBJ_TIMELINE`. `drmSyncobjQuery`, `TimelineSignal`,
  `FDToHandle` and `HandleToFD` refuse with `ENOSYS`, as before.
- **A no-op GPU** (`HAKIOS_DRM_NOOP=1`). It makes up a Mali-G72 in the process, using the same values as the Linux drm-shim check
  `tools/mali-drmshim-check.sh`. Buffers live in process memory, and every job is "done" without running. Nothing changes unless
  the variable is set.

**A semantic difference, and where it bit (14 November).** The stand-in treated a sync object that was reset (no fence) as
signalled. Linux treats it as unsignalled, and waiting on it without `WAIT_FOR_SUBMIT` fails there. Since 14 November a wait
*with* `DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT`, which Vulkan's common code always gives, waits until a submission gives the
sync object a job; without the flag (Panfrost's OpenGL ES never gives it) a reset sync object still counts as signalled. PanVK's
JM queue also makes its own sync object ioctls (a submission's fence handed to the semaphores it signals through a sync file;
VkEvents), which went to the kernel and failed silently; the stand-in serves them now. Before that, every `VkFence` wait
returned at once, and a Metal texture read back with `-getBytes:` had 64-byte lines of zeroes in about half the runs (see
"Phase 2" below).

## What was built and run (7 November 2026, the build Mac)

- `tools/mk-panvk.sh` (iOS): Mesa 25.0.7, every patch in `tools/mesa`, plus `panvk-darwin.patch`, cross-built with the kernel's
  `xnu-clang` against the iPhoneOS 16.5 SDK. The result is `libvulkan_panfrost.dylib` (arm64 iOS, 10.3 MB stripped),
  exporting `vk_icdGetInstanceProcAddr`, `vk_icdNegotiateLoaderICDInterfaceVersion` and `vk_icdGetPhysicalDeviceProcAddr`, which
  is what `icd.rs` opens a driver by. It links only libSystem and IOKit. Mach-O has no undefined weak symbols that resolve to NULL
  when linked, so the entrypoint tables' optional references (about 4 800 `panvk_*`, `vk_*`, `wsi_*` names) are left to dyld,
  which makes them NULL. The script **fails the build** if any non-weak symbol is left that way. That check is how a function
  missing from the stand-in would show up before the phone.
- `tools/mk-panvk.sh mac`: the same driver built for this Mac (about 10.6 MB), with `vk_smoke` run against the no-op G72. The
  results are in the next section.
- Neither library is code-signed, as `libOSMesa.8.dylib` from `tools/mk-mesa.sh` is not. If the phone's image needs signatures
  for libraries, both need the same step.
- Not run: anything on the phone, and anything that needs the GPU to execute a job.

## vk_smoke on the Mac, against the no-op G72

`tools/mk-panvk.sh mac` ends by running `tools/vk/vk_smoke.c` twice with `HAKIOS_DRM_NOOP=1`. The exact output on 7 November
(Mesa's "panvk is not a conformant Vulkan implementation" warning left out):

```
vk_smoke: ICD interface: asked 5, the driver says 5 (0)
vk_smoke: 1 instance: ok, API 1.0 asked
vk_smoke: 2 device: ok, "Mali-G72", Vulkan 1.0.305, driver 0x6400007, vendor 0x13b5, device 0x62210000
vk_smoke: 3 extensions: ok, 67 of them; +KHR_timeline_semaphore +KHR_buffer_device_address +KHR_synchronization2 +KHR_dynamic_rendering +KHR_create_renderpass2 -KHR_maintenance4 -KHR_vulkan_memory_model -EXT_descriptor_indexing +KHR_8bit_storage +KHR_16bit_storage +KHR_shader_float16_int8 +KHR_push_descriptor +KHR_external_memory -EXT_external_memory_host -EXT_extended_dynamic_state +KHR_driver_properties +EXT_scalar_block_layout -KHR_separate_depth_stencil_layouts
vk_smoke: 4 features: ok, timelineSemaphore 1 (by the extension); 1.0: shaderInt64 1 shaderInt16 1 samplerAnisotropy 1 textureCompressionBC 0 fragmentStoresAndAtomics 0 independentBlend 1
vk_smoke: 5 logical: ok, queue family 0 of 1 (flags 0x7, 1 queues), timeline semaphores on
vk_smoke: 6 clear: submitted and waited for; not checked: no GPU ran it (HAKIOS_DRM_NOOP)
vk_smoke: 7 timeline: ok, signalled to 7 by a submit, the CPU's wait returned, the counter reads 7
vk_smoke: 8 compute: compiled for this GPU, dispatched and waited for; not checked: no GPU ran it (HAKIOS_DRM_NOOP)
vk_smoke: 6 of 8 steps passed, 2 not checkable without a GPU
```

The second run asks for Vulkan 1.2 with `MESA_VK_VERSION_OVERRIDE=1.2`, as metal2vulkan would. It is the same except:

```
vk_smoke: 2 device: ok, "Mali-G72", Vulkan 1.2.305, ...
vk_smoke: 4 features: ok, 1.2: timelineSemaphore 1 bufferDeviceAddress 1 descriptorIndexing 0 vulkanMemoryModel 0 shaderFloat16 0 shaderInt8 1 storageBuffer8BitAccess 1 scalarBlockLayout 1; 1.0: ...
```

What this shows, on our stand-in with the S9's G72 parameters:

- The ICD handshake works.
- `drmGetDevices2` gives the Mali, and PanVK accepts it behind its gate.
- The extension and feature lists are as listed above.
- A device with timeline semaphores is made. Timelines are emulated over our binary sync objects, and their counter is right.
- The hand-written SPIR-V compute shader goes through PanVK's SPIR-V to NIR to **Bifrost v6** compile into a pipeline.
- Command buffers are built and submitted, and the waits return.

What it does not show:

- Anything the GPU does: the no-op GPU writes nothing, so steps 6 and 8 are "not checked". Phase 1 checks them on the phone.
- That the kernel's HakiosMali takes PanVK's job chains. PanVK's JM backend submits as Panfrost does (`DRM_PANFROST_SUBMIT`,
  `requirements` FS for fragment), but with its own descriptors and tiler heap.

Two v6 gaps that matter for Metal show up here:

- `fragmentStoresAndAtomics 0`: no buffer writes from fragment shaders, which Metal allows.
- No `descriptorIndexing` and no `vulkanMemoryModel` at 25.0.7. 26.2.6 adds the memory model on v6.

Whether metal2vulkan's translated shaders need any of these is what `m2v_test` against the no-op G72 (phase 3) will say before
any phone run.

## What others have shown (facts, not code)

- **Starbite** (a private S9 project; its notes are on the build Mac only). On 1 November 2026, Mesa 25.0.7 PanVK with
  `PAN_I_WANT_A_BROKEN_VULKAN_DRIVER=1` ran on the same G72 under iOS 26 through their own kernel driver. Instance, device, a
  clear and an image-to-buffer copy read back 4096 of 4096 pixels. For metal2vulkan they moved to **Mesa 26.2.6 PanVK** with
  `MESA_VK_VERSION_OVERRIDE=1.2`, with IOS-26-Hot's frontend over the same bridge. With that, iOS 26's window server composites
  through Metal on the Mali at about 10 fps, later up to 24 fps once some daemons were disabled. The frame reached the panel by a
  CPU copy, which was the limit.
- Their problems are the ones to expect here:
  - PanVK keeps a per-batch TLS (stack) buffer in every command pool. On an MP18 that is about 2.6 MiB per render pass, which
    grew to hundreds of MiB in backboardd. One TLS buffer per device fixed it, since the kernel runs one chain at a time.
  - PanVK allocates a 137 MiB indirect-varying buffer per device. They reduced it.
  - GPU buffers were mapped write-combined, which made the owner's heap pages Normal-NC. LSE atomics on those pages take external
    aborts on the Exynos 9810. Cacheable (copy-back) mappings fixed it.
  - This is iOS 26, not 16.5. On 16.5 the window server cannot take Metal (above).
- **UniXNU** (iPadOS 17, Adreno 740): Metal on Vulkan, with Turnip under metal2vulkan (MIT bridge).
- **star2lte port** (`metal-wip`): the AdrenoMetal frontend plus a GPL-family metal2vulkan patch. Not default, not 60 fps.

## Against the Gallium route

`GPU.md` (31 October) chose the Gallium route for the S9: Apple's shaders through the AIR frontend into NIR and Panfrost's Bifrost
compiler, with the Metal object graph written over Gallium. Its reasons against PanVK were:

- Vulkan 1.0 at the pin. That is still true, but timeline semaphores and the rest are there as extensions, and the version can
  be overridden.
- Missing stand-in calls. Those are now added and checked at link time.
- No shader execution in the hakios-metal route. That is not relevant to PanVK.

The trade-off now:

- **PanVK plus metal2vulkan**: the whole Metal API exists already (metal2vulkan's runtime, the bridge and the frontend are in the
  tree and pass on three Mac Vulkan drivers). What is new is a Vulkan driver that is experimental on v6, plus memory tuning that
  Starbite found necessary. It is a separate driver from the OpenGL ES one, so a PanVK bug cannot break the lock screen.
- **Gallium**: one driver shared with the OpenGL ES path, but the Metal object graph over Gallium must still be written (about
  1 200 methods in Apple's own, [metal-plan.md](metal-plan.md#the-three-parts)). So far only a compute dispatch and a draw are
  wired (run M1 in `GPU.md`).

**Recommendation:** make PanVK plus metal2vulkan the S9's main Metal route. Keep the Gallium route's M1 run as the cheap check that
the Bifrost compiler takes AIR-derived NIR, since both routes rely on that.

## Plan

Every behaviour change stays off by default: PanVK is only in an image built with `PANVK_DIR=`, only `vk_smoke` and a plug-in
behind `metal=N` use it, and PanVK itself refuses the G72 without `PAN_I_WANT_A_BROKEN_VULKAN_DRIVER`.

| Phase | Work | Checked by | Effort |
|---|---|---|---|
| 0 (done, this page) | licences; feasibility; PanVK 25.0.7 built for iOS and the Mac; the stand-in's enumeration and sync objects; the no-op G72; `vk_smoke`; `PANVK_DIR=` and `rc.jobs.vksmoke` | the link check; vk_smoke on the Mac (no-op) | done |
| 1 | **the first phone run**: an image with `GL=1 PANVK_DIR=xnu-deps/build/panvk/out RC_JOBS=rc.jobs.vksmoke`, boot-args as run G1 of `GPU.md` (`mali=9`) | vk_smoke's eight lines on the G72; 8 of 8 is the pass. A fault in step 6 or 8 is the kernel's fault lines, as G1 | one flash, 30 min for the phone owner; any fix in the stand-in or kernel, 0.5-2 days |
| 2 | **Mesa 26.2.6 PanVK** beside the pinned 25.0.7 (OpenGL ES stays on 25.0.7): build host `mesa_clc` and `vtn_bindgen2` against Homebrew's LLVM 23 on the build Mac, plus SPIRV-LLVM-Translator and SPIRV-Tools from source; port `panvk-darwin.patch` | vk_smoke no-op on the Mac; `vulkanMemoryModel` reported | 1-2 days |
| 3 | **Metal on PanVK**: `METAL=vk METAL_ICD=/redwood/lib/libvulkan_panfrost.dylib`; `HakiosMetalVK.mm` sets `PAN_I_WANT_A_BROKEN_VULKAN_DRIVER=1` and `MESA_VK_VERSION_OVERRIDE=1.2` only when the driver it opens is PanVK, and only with `metal=N` | `m2v_test` on the Mac against the no-op G72 (compiles metal2vulkan's shaders for Bifrost); then `rc.jobs.metalvk` on the phone (`m2v_test`'s compute and triangle, then `metal_test`) | 1 day, then 1-2 flashes |
| 4 | **memory and surfaces**: one TLS buffer per device and a smaller varying buffer for the MP18 (Starbite's figures, our own code); IOSurfaces into PanVK through the stand-in's existing surface request (0x7e) as imported memory; cacheable mappings for anything the CPU also maps | a 10-minute `metal=2` run with HakiosMali's buffer totals flat; CoreImage on Metal | 3-5 days, several flashes |
| 5 | clients: `metal=2` (CoreImage, RenderBox, the camera's views, apps), the `asked and not implemented` lists as the work list; performance | per client | weeks, open-ended |

The window server on Metal is **not** in this plan for iOS 16.5. It needs QuartzCore's `default.metallib`, which is Apple's and is
not in the image. On a newer iOS base it would be phase 6.

## How to build and run

```
tools/mk-panvk.sh            # iOS: xnu-deps/build/panvk/out/{libvulkan_panfrost.dylib, vk_smoke}
tools/mk-panvk.sh mac        # this Mac: out/mac/..., and vk_smoke against the no-op G72
GL=1 PANVK_DIR=$PWD/xnu-deps/build/panvk/out RC_JOBS=rc.jobs.vksmoke userland/build.sh ...   # a test image (phase 1)
```

`vk_smoke [--mali-v6] [--api 1.2] [driver.dylib]`: `--mali-v6` sets PanVK's gate for this program only.

## On the phone (13 November 2026): phase 1 passes, and a Metal triangle draws on the Mali-G72

**Where it stands.** On the Galaxy S9+ (star2lte, iOS 16.5, kernels of builds 36 to 40, `mali=9`, no new boot-arg), every piece was run by
hand from `/var/root/metal` and `/var/jb/Applications/MetalTri.app` while the OpenGL ES compositor kept running:

- **`vk_smoke --mali-v6` on the real G72: 8 of 8 steps passed.** The clear read back 4096 of 4096 pixels `0xff4080ff`, the timeline
  semaphore counted to 7, and the compute shader wrote 4096 of 4096 words `2i+1`. PanVK's jobs went through HakiosMali beside the
  compositor's.
- **`metal_tri` (`userland/src/metalvk/metal_tri.m`) through the Metal API.** It opens the METAL=vk plug-in by hand
  (`-initWithAcceleratorPort:`), gets an `MTLDevice` called "Mali-G72", loads a library from a `.metallib` and builds a render pipeline (AIR
  to SPIR-V to Bifrost in 47 to 61 ms). Then come a queue, a render pass and a read-back. A 256x256 triangle had **0 wrong pixels**:
  19 602 inside and 43 486 outside were judged. It was read both with `getBytes:` and from an IOSurface's own memory (`--surface`).
- **`MetalTri.app`, the same program as an application.** Metal renders into two 512x512 IOSurfaces in turn, and the window server shows
  them as a layer's contents. Every frame is judged. On the panel: 29 frames, 29 right, about 5.6 ms a frame.

The shaders were written by hand as AIR (`tools/metal/air/tri_*.ll`, assembled with `llvm-as`). `tools/metal/mkmetallib.py` puts them in a
library, so nothing in this chain needs Xcode. PanVK (Mesa 25.0.7) and `libmetal2vulkan.dylib` (Rust 1.98.1, `aarch64-apple-ios`) were
both built on the Linux build host.

### What the iOS 27 port's Metal was

It ran on **this same GPU**: iOS 27.0.1 on the Galaxy S9 (`profiles/ios-27.conf`, [ios27/METAL.md](ios27/METAL.md)). The stack was
Starbite's:

- Mesa **26.2.6** PanVK on the Mali-G72, with `PAN_I_WANT_A_BROKEN_VULKAN_DRIVER=1` and `MESA_VK_VERSION_OVERRIDE=1.2`;
- metal2vulkan's runtime with the bridge;
- the HotMetal2 frontend (from IOS-26-Hot, after AdrenoMetal);

all of it in `StarbiteMetal.bundle`. The kernel's StarbiteAccelerator published `IOAcceleratorES` with `MetalPluginName` behind
boot-arg `sbmtl=1`, so apps got their device the ordinary way: `MTLCreateSystemDefaultDevice()` loaded the plug-in. There was no AGX
emulation and no MTLCompilerService; AIR was translated inside the process.

What ran there:

- `mtl27test`: the device "Mali-G72 MC18", the clear and the IOSurface render exact.
- PosterBoard got a device, which took 20 s to open.
- CoreAnimation in backboardd ran on Metal, with failures (XR10 sampling, no-copy buffers).

The compositor stayed on OpenGL ES there too. The Adreno port's Metal ([metal-plan.md](metal-plan.md): hakios-metal, and metal2vulkan
over Turnip) is a separate, earlier effort.

**Reused here:**

- the METAL=vk plug-in (`userland/src/metalvk/`: AdrenoMetal's frontend and the same bridge);
- metal2vulkan at the same pin, with the same patches;
- PanVK;
- the kernel's existing `metal=N` switch on `IOAcceleratorES` (`MiniIOMobileFramebuffer.cpp`).

**Not taken:** Mesa 26.2.6, which needs a host `mesa_clc`; 25.0.7 has been enough so far. `HakiosMetalVK.mm` now sets PanVK's two
variables itself, only for that driver and only in a process the device is shown to.

### The Astronomy wallpaper (AegirPoster)

- **Why it aborts.** AegirPoster throws `NSInvalidArgumentException: -[__NSArrayM insertObject:atIndex:]: object cannot be nil` during
  renderer setup. The throw stack, unslid against the 20F66 cache, goes from PosterKit through ClockKitUI
  (+0x2973c, +0x8ad0, +0x29a6c, +0x2a558, +0x2b1d4) into NanoUniverse (+0xa7b4, beside `NUNILoadMtlTextureFromMemory`; then +0x168b0,
  +0x15f3c, +0x14b74, +0x14b28), which throws. NanoUniverse puts a Metal object it never got (there is no device) into an array.
- **Why it loops.** PosterBoard's gallery snapshotter (`GALLERY-(AegirPoster)-(Earth-Full)` and others) starts the extension, the extension
  aborts, and the snapshot is retried. That was 23 aborts in 4 minutes after boot (two about every 10 s), plus the lock screen's.
- **What it needs from Metal:**
  - NanoUniverse's `default.metallib`, which is AIR that metal2vulkan can translate: 27 functions, the `aegir_*` earth, luna, saturn,
    gaseous, starfield, sprite and post shaders;
  - textures loaded from memory (its asset formats are still to be checked against what PanVK takes: the Mali has ASTC and ETC2, not
    PVRTC);
  - a CAMetalLayer in an extension process (drawables are IOSurfaces, which `--surface` shows work);
  - XR10 formats on the layer (iOS 27 needed `supportsExtendedXR10Formats`);
  - a device registered with Metal (discovery with `metal=2`) rather than opened by hand.
- **Stopping the loop without Metal: there is no proper way.** The retries are PosterBoard's own, and they end only when a snapshot
  succeeds. Hiding the extension (LaunchServices or PlugInKit) or planting snapshots would each be a hack. The cost is CPU: one short
  process every few seconds. Leave it until `metal=2` works for PosterBoard's extensions.

### Maps (VectorKit): the honest route

- **The blocker is the shaders, not the GPU.** On 20F66, VectorKit ships its map shaders (`DaVinci::ground_*`, ribbons, flyover and the
  rest) **only as Apple GPU binaries**: `VectorKit.framework/iphoneos.pipelinelib/pipelines.g11p` and the files beside it, `LPPM`
  archives, one per AGX generation. Its `functions` file is empty, and `pipelines.desc` names a `default.metallib` that is not in the
  image. The only AIR VectorKit has is `metal_libraries/AlloyCommonLibrary.metallib`, 13 clear and text functions. So no Metal plug-in can
  draw the map from what is on the phone. It is the same wall as QuartzCore's missing `default.metallib` for the compositor.
- **The route:**
  1. `metal=2`, and MTLAddDevice, so that Maps gets the device by discovery.
  2. The plug-in answers `newPipelineLibraryWithFilePath:` from `pipelines.desc`: pipeline names, function names, function-constant
     hashes, formats.
  3. The AIR for those functions comes from a VectorKit `default.metallib` that the user supplies from a matching release; the iOS
     16.4/16.5 Simulator runtime's VectorKit is the closest. It is put in place on the user's own machine, as `tools/ca-metallib.py` does
     for QuartzCore, and is never in this repository.
  4. Then the frontend's gaps that VectorKit hits: argument buffers (refused today), its tile textures and formats, and memory
     (Starbite's TLS and varying-buffer fixes, phase 4 above).
- **Effort.** Steps 1 and 2: 3 to 5 days. Step 3: as soon as the owner has the Simulator runtime, then 1 to 2 weeks of working through the
  `asked and not implemented` lists. Step 4: weeks. The blue dot and the tiles come back together, since both are drawn by VectorKit's
  Metal. AegirPoster is the nearer target, because its AIR is on the phone, and should go first.

### Opt-in, and what an image gets

Nothing above is in the default build, and nothing here changes the OpenGL ES compositor:

- PanVK is in an image only with `PANVK_DIR=`.
- The plug-in is the Metal-on-Vulkan one only with `METAL=vk`.
- The device is shown to a process only with `metal=N` or `HakiosMetal.clients`.
- `metal_tri` and `MetalTri.app` are hand tests. The command line is
  `metal_tri [--bundle DIR] [--metallib FILE] [--surface] [--out F.ppm]`. As an application (bundle identifier
  `com.hakios.metaltri`) it takes everything from its own directory.

Not yet ready for an image:

- build.sh does not build `metal_tri` or `metal_tri.metallib`.
- `tools/metal/mk-metal2vulkan.sh` has no Linux path. It assumes a Mac (`xcrun metal` for its self-test shaders, Xcode's `strip`), and on
  git 2.34 its sparse checkout needs `git sparse-checkout init --cone` first. The hand build used
  `CARGO_TARGET_AARCH64_APPLE_IOS_LINKER=xnu-clang`, with `SDKROOT` set to theos's iPhoneOS16.5 SDK.

## Phase 2, step 1: a Metal device for applications (14 November 2026)

**What changed** (commits `db0e84a` to `b4dd2e1` on `star2-bringup`):

- **The device opens the GPU at its first use.** `HakiosMetal.bundle/HakiosMetal` (`src/metalvk/HakiosMetalVK.mm`) links only
  the system's frameworks. `-initWithAcceleratorPort:` gives an object of its own `_MTLDevice` subclass. That object answers what
  `MTLAddDevice()` sends (`-initLimits`, `-initFeatureQueries`, checked in the 20F66 cache) and the fixed facts (`registryID`,
  unified memory, not low power, not removable, not headless). Every other `MTLDevice`/`MTLDeviceSPI` message goes through
  Objective-C's forwarding, and the first one loads `HakiosMetal.bundle/HakiosMetalVK.dylib`, which holds the frontend and
  `HakiosMetalVKOpen.mm`. That library links `libmetal2vulkan.dylib`, opens PanVK, and puts the frontend's methods on that same
  class (`native_bridge_metal_adopt`). The object Metal holds then *is* the device: a buffer's `-device` is identical to it. A
  process that only asks `MTLCreateSystemDefaultDevice()` maps no runtime and opens no Vulkan device. A device that cannot be
  opened answers nil or 0, and its reason is logged once.
- **Who is shown a device** (`src/hakios_metal_shown.c`):
  - `metal=1`: `metal_test` only;
  - `metal=2`: **applications and app extensions** (an executable in a `.app` or `.appex` bundle), no daemon;
  - `metal=3`: every process but backboardd (what `metal=2` was);
  - `metal=4`: every process.

  `-NAME` in a clients file keeps the device from that process. `/private/var/db/HakiosMetal.clients`, which is writable, is read
  beside the image's read-only `HakiosMetal.clients`. The `metal=` word is no longer matched inside another argument (such as
  `ca_metal=`).
- **Logging:** each process writes `HakiosMetal[process pid]: ...` lines to the console and to `/private/var/tmp/HakiosMetal.log`
  (up to 4 MiB). The frontend's own lines go there too.
- **Sync objects** (`tools/mesa/hakios-drm`, above): fixed. `-getBytes:` is now right in 12 runs of 12, where before about half
  of the runs were wrong.
- **Memory of an opened device.** HakiosMali wires every buffer whole, so there are two changes:
  - PanVK's tiler heap is 32 MB on Darwin (`tools/mesa/panvk/panvk-tiler-heap.patch`). Linux grows it on a fault from a small
    start.
  - metal2vulkan's pooled blocks are 8 MiB on iOS, not 64 MiB (`tools/metal/m2v/0003-ios-block-size.patch`).
- **Tools:** `metal_tri --register` (hands the device to Metal by `MTLAddDevice()`, as discovery does) and `--hold N` (keeps the
  device open, to measure it). `metal_libcheck LIB.metallib...` runs every function of a library through a pipeline.

**Measured on the phone (build 43's kernel, the new plug-in, PanVK and runtime from `/var/root/metal2`, no boot-arg):**

| | Before | After |
|---|---|---|
| Runtime loaded after `-initWithAcceleratorPort:` + `MTLAddDevice()` | yes (linked by the bundle) | no; it loads at the first real message (`-name`, `-maxThreadsPerThreadgroup`, ...), 250 to 760 ms to open |
| `metal_tri` (`getBytes:`) | 0 to 4384 wrong pixels in about half the runs, the image's build too | 0 wrong in 12 of 12; `--surface` 3 of 3; `vk_smoke` 8 of 8, three times |
| Footprint of a process with an open device (`metal_tri --hold`) | 132 MB (129 MB IOKit: two 64 MiB blocks) | 20 MB |
| Wired memory for one open device (`vm_stat`) | 258 MB | 49 MB |

**`metal_libcheck`** (function constants zero, every function paired):

| Library | Result |
|---|---|
| NanoUniverse (the Astronomy wallpaper) | **27 of 27** made |
| ClockKitUI | 3 of 3 kernels made |
| SiriUICore (Siri's orb) | 15 of 20 made. `SiriBlur` (a kernel) is refused: it needs non-uniform indexing of storage-image arrays, which PanVK v6 does not offer. The `sphere` and `compositeSphere` functions are refused for this check's own pairing and formats, not proven wrong. |

**Still to be seen on a boot with `metal=2`:**

- the console lines of SpringBoard, PosterBoard and its extensions, and Maps;
- SpringBoard's and backboardd's memory and CPU against build 43's (`footprint`, `ps`);
- the compositor's frames;
- whether AegirPoster's crash loop ends.

The daemons get no device at `metal=2`. `mediaserverd` keeps taking its device out unless `mediamtl=1` (`inject_media.c`); its
`cvadepth-iOS.metallib` is in the image now (`KEEP_METALLIB`).

### On the phone with `metal=2` (builds 44 to 48, 14 November 2026)

Each boot found the next wall. The fixes are all in `star2-bringup`.

| Build | What happened | Cause | Fix |
|---|---|---|---|
| 44 | PosterBoard ended every ~2 s: `OS_REASON_GUARD` (VM, `kGUARD_EXC_DEALLOC_GAP`) | see build 45 | `be308d4` forwards `_MTLDevice`'s own methods too (harmless, not the cause) |
| 45 | CollectionsPoster ended the same way, a second after its device opened | PanVK `munmap()`s IOKit mappings. IOKit gives a task one mapping per buffer, so a VkBuffer covering its whole memory shared the memory's mapping; `vkDestroyBuffer` removed it and `vkFreeMemory`'s `munmap()` fell on a hole. This is fatal in apps and extensions and silent in root tools. It was caught in `metal_tri` with VM guard delivery on | `ccc645c`: the stand-in counts each mapping's users; PanVK unmaps through `hakios_drm_unmap()` (`panvk-unmap.patch`) |
| 46 | SpringBoard died of `SIGBUS` ~10 s after start, after `mmap(..., size=16) failed` | Siri's orb draws indexed. PanVK (arch < 9) maps the range of an index buffer; the stand-in mapped whole buffers only | `cabfdd0`: a sub-range maps as the buffer's one mapping at that offset, and upstream's in-page offset is fixed |
| 46 | AegirPoster aborted on an uncaught exception right after opening its device | (unknown then) | `71346b0`: exceptions thrown after the device opens are logged with their caller |
| 47 | AegirPoster: `CAMetalLayerInvalid: unsupported extended range format` from ClockKitUI | the frontend told every process "no XR10" | `12ed728`: only backboardd is told no. XR10 textures, clears and CAMetalLayer drawables work |
| 47-48 | SpringBoard with the device | | stable; Siri's orb prewarm returns. Footprint 88 MB (44 MB without the device) |
| 48 | PosterBoard without the device aborts when the gallery starts AegirPoster (`attempt to insert nil object from objects[1]`: it puts its command queue in a CIContext's options) | PosterBoard needs the device | `-PosterBoard` taken out of `/private/var/db/HakiosMetal.clients`. The gallery opens, and so do the Astronomy editor and AegirPoster, which no longer crash |

**The Astronomy wallpaper is still black (build 48).** metal2vulkan refuses 29 of AegirPoster's render pipelines with `Unsupported Format,
NotImplemented at Color(0)`. These are every `aegir_*` fragment that *blends* into the layer's `BGR10_XR_sRGB` drawable (SrcAlpha/OneMinusSrcAlpha for
the earth, luna, sprites and stars; One/One for `aegir_post_fsh`). metal2vulkan computes such a blend in the shader. It reads the attachment either through
rasterization-order input attachments (`lower/fragment/input_blend.rs`) or under the pixel interlock (the storage form, `lower/fragment/fetch.rs`), and
PanVK v6 offers neither. Unblended XR10 pipelines are made. The way forward is a `tools/metal/m2v` patch: blend with the fixed function on the raw
10-bit codes through a UNORM view of the attachment.
- For an affine blend (destination factor = 1 - source factor) the XR bias is kept exactly.
- For an additive one (destination factor One) the source is encoded without the bias.
- For the `_sRGB` forms, blending the encoded values is not Metal's linear blend: that path is an approximation, and it would be said so.

Not yet seen: whether the GLES compositor shows a 10-bit (`BGR10_XR`) drawable right; that needs a pipeline that draws first.

## Phase 3, step 1: blends into extended-range drawables, and the compositor's view of them (14 November 2026)

**The blends** (`tools/metal/m2v/0004-xr10-unorm-blend.patch`, its header says how and why in full):

- On a device that has no framebuffer fetch, a `BGR10_XR` or `BGR10_XR_sRGB` color attachment is now rendered through the
  `A2R10G10B10_UNORM_PACK32` view of its word. That means neither rasterization-order attachment access nor the pixel interlock,
  which is PanVK v6. The view's fields are the word's own, so unblended writes, clears, loads and stores keep Metal's codes exactly.
  Every pipeline and pass on the device takes the view, so the pass and its pipelines agree on the format.
- The codes are affine in Metal's value, `u(v) = (384 + 510 v) / 1023`. The device's fixed-function blend of codes is therefore
  Metal's blend when the function writes the source with the right share of the 384 bias:
  - all of it for "over" (`SourceAlpha`/`OneMinusSourceAlpha`, a blend colour's pair, `One`/`Zero`);
  - none for "added" (destination factor `One`);
  - the source alpha's share for premultiplied (`One`/`OneMinusSourceAlpha`).
  
  Every other blend stays refused.
- The `_sRGB` forms blend the encoded codes, which is an **approximation**. It is exact for unblended writes and at alpha 0 or 1;
  in between, colours come out darker than Metal's where they differ (as on a GPU without sRGB blending).
- Checks on the phone (PanVK, Mali-G72):
  - `metal_tri --xr none|over|add`, on a texture and on a `w30r` IOSurface: every pixel within 1 code of Metal's store, and the
    clear exact. Before the patch, the blended pipelines were refused.
  - `metal_libcheck --format 554|555 [--blend over|add|premultiplied]`: NanoUniverse 27 of 27 in every case.

**The compositor does not show an extended-range drawable right.** `MetalTri.app` with `MTXRBlend`/`MTXRFormat` in its
`Info.plist` shows the triangle in a CAMetalLayer of `BGR10_XR`. The Metal side reads back exact (0 wrong pixels), but the panel
shows stripes and a lavender background.

- The cause is the kernel. `MiniIOSurface.cpp` makes every `w30r`, `l10r` and `w30R` surface as plain `BGRA`, because
  CoreAnimation's software renderer cannot draw into them. The window server then reads the drawable's ten-bit words as eight-bit
  BGRA.
- The fix (`NativeBridgeMetalFrontend.mm`, `drawable_xr_to_surface`): when such a drawable is presented
  (`presentDrawable:`, `presentDrawable:atTime:`), the frontend converts its codes into the BGRA bytes the surface claims to hold.
  Each byte is `(c - 384) / 510` clamped to `[0, 1]`, the sRGB-encoded value these layers hold. It runs on the CPU after the GPU
  has finished.
- A presented drawable's contents belong to CoreAnimation, so nothing of Metal's is lost.
- A client that calls `-present` on the drawable itself, rather than through the command buffer, is not converted yet.

**`setDepthClipMode:`** is now forwarded to the runtime (`nmb_render_set_depth_clip_mode`). CoreLocation's precise-location
prompt plug-in threw on it as an unrecognized selector. The alert it left on the lock screen then stopped taking touches.

### On the phone, builds 49 and 50 (14 November 2026)

- **AegirPoster** makes every one of its blended `aegir_*` pipelines (`ok=1` in `/var/tmp/m2v/pipelines.log`, attachments 555 and 10)
  and no command buffer is refused any more. When the Astronomy preview opens it runs about 100% CPU for about a minute (to about
  225 MB) and then goes idle. It has **presented no drawable** through `presentDrawable:`: the conversion's log line never appears. The
  gallery tile and the preview stay black. Next: find where that minute goes and whether the extension presents another way.
- **The Weather wallpaper's** gallery tile now draws (build 50). The location prompt's map circle draws (`setDepthClipMode:`).
- **MetalTri.app with its own plug-in** (`MTXRBlend over`): a blended `BGR10_XR` CAMetalLayer, every frame judged exact (largest error
  1 code) and shown in the right colours through the GLES compositor. Before the conversion at present it showed stripes on lavender.

## Phase 3, step 2: the window server on Metal (`camtl=1`), first tries

`camtl=1` and a `backboardd` line in `/private/var/db/HakiosMetal.clients`, then backboardd restarted; reverted the same way.

- **Build 49:**
  - The device opens in about 520 ms and CoreAnimation makes 126 pipelines from its pipeline library.
  - Then backboardd aborts at its first frame: QuartzCore says "Metal failed to specialize fragment function:
    function=fixed_frag_lph_cph spec=A2Xhf".
  - launchd counted 8 crashes in a row before the line was taken out.
- **Build 50** (refused specializations are now logged):
  - The cause is `constant type 51 is not qualified`. QuartzCore's uber shader takes a `uchar3` function constant, and the frontend
    refused every 3-lane vector type.
  - Fixed in `05e6bda`: lanes times the scalar, as the bridge packs it. That fix needs an image to be tried.
- **The GLES baseline**, under six home-screen page swipes:
  - backboardd CPU averages about 36% (peaks 67%), RSS about 129 MB;
  - the eagl frame line (every 20th frame) shows 4 to 31 ms, a median of about 9 ms.
- **What is not yet known:** Metal's numbers, which need a backboardd that survives its first frame.
- **Default and fallback:** GLES stays the default and the fallback. The Metal road stays behind `camtl=1` plus the clients line, until
  it has run a session without aborting.
- **Build 51** (`uchar3` constants qualified): the specializations are now made, among them `fixed_frag_lph_cph` with
  `fc_framebuffer_fetch=1`. Of the window server's pipelines, 100 are made and one is refused: `std_vert1_lph` with `luma_boost_lph`.
  The refusal is `Unsupported(FramebufferFetch)`, needing rasterization-order attachment access or the pixel interlock, neither of which
  PanVK v6 has. `luma_boost_lph` reads its color attachment, and QuartzCore aborts on the refusal ("Metal failed to build render
  pipeline: spec=PBGRAXm_TlubA2Xhf_Isrc"); launchd counted 4 crashes before the line was taken out.

  The device claims Apple GPU families 1 to 3, with "Programmable blending" listed among their waived rows, so CoreAnimation assumes
  framebuffer fetch. The ways on, each its own piece of work:
  - find what makes QuartzCore choose the luma-boost (EDR) pass and its fetch path, and have the device answer that query truthfully;
  - or give metal2vulkan a fetch without either Vulkan feature, for example a copy of the attachment bound as a texture for the draws
    that read it.

## Phase 4: framebuffer fetch without either ordered form, and the window server on Metal (14 November 2026)

**metal2vulkan 0005** (`tools/metal/m2v/0005-unordered-framebuffer-fetch.patch`, its header says how and why in full):

- PanVK v6 has neither `rasterizationOrderColorAttachmentAccess` nor `fragmentShaderPixelInterlock`. On such a device, and only
  under `FramebufferFetchPolicy::AcceptUnordered`, every pass now reads its colour attachments as input attachments, as on a device
  with ordered access, but without the rasterization-order flags. The runtime makes the compile layer's unordered fetch pipelines
  there, where it used to refuse them.
- Between draws the read is ordered. The draws' subpass already depends on itself, by region, from colour attachment writes to
  input attachment reads. A draw that reads input attachments records that barrier first whenever an earlier draw of its render pass
  instance wrote a colour since the last barrier, or the pass loaded by a clear.
- Within one draw the read is unordered: a fragment may see its pixel as it was before the same draw's earlier fragments. A pass
  that reads each pixel once, such as CoreAnimation's full-screen `luma_boost_lph`, gets Metal's value. The compile layer's precision
  note (`unordered-framebuffer-fetch`) says so once.
- A `BGR10_XR` attachment rendered through its unorm view (0004) is not fetched. It is refused by name, because the view's codes are
  not the raw texel that the read decodes.
- The bridge (`nmb_device_open`) passes `AcceptUnordered`. The default policy is unchanged.

**PanVK** (`tools/mesa/panvk/panvk-barrier-clears.patch`): on Bifrost, input attachments are texture reads through memory, and a
pass's clear stays in the tile until its batch ends. PanVK used to drop a batch that had no draw at a barrier and then turn every
clear into a preload, so a barrier before the first draw lost the clear. The barrier now writes the clears first, as
`CmdEndRendering` does for a pass without draws.

**`metal_tri --fetch`** (`tools/metal/air/tri_fetch.ll`: the corners' mix over 2 plus `[[color(0)]]` over 2, drawn twice into a
target cleared in BGRA8):

- Every pixel inside is within 0.9/255 of 3/4 mix + 1/4 clear, and every pixel outside is the clear exactly.
- Checked on a texture and on a texture over an IOSurface, 256 and 1024 square, five runs of five, per process and from build 52's
  image.
- Before the PanVK fix, the first draw read zeros instead of the clear (19602 of 19602 pixels wrong). Before 0005, the pipeline was
  refused.
- `metal_tri` plain and `--xr none|over|add` still pass.

**The window server on Metal (`camtl=1` plus the clients line), builds 52 and 53:**

- **Build 52:** all 337 of backboardd's pipelines are made, `luma_boost_lph` among them, and the home screen draws right. Then
  backboardd died of SIGSEGV twice: 13 s after its start, and at the first home swipe. Both were in `memmove` from
  `Buffer::copy_host_bytes` in `host_sync::copy_out`, symbolicated against the unstripped runtime. That is 0002's copy back of
  no-copy buffers the device cannot import. It took every 4 KiB that differed, which included the caller's newer bytes and memory
  that CoreAnimation maps read-only. **0006** (`0006-copied-host-copy-out-gpu-writes.patch`) copies back only the chunks the GPU
  changed, from a per-4 KiB digest taken at the copy in.
- **Build 53:** backboardd runs on Metal without a crash through two sets of six home swipes. Measured the same way as GLES on
  build 52 (`ps` once a second):

  | | GLES (build 52) | Metal (build 53) |
  |---|---|---|
  | backboardd CPU, idle | about 7% | about 7% |
  | backboardd CPU, during the swipes | 25 to 38% | 120 to 168% |
  | backboardd RSS | 108 MB | 159 MB at the start, 171 MB after the swipes |

- **The next wall is the copy in.** A 4 s spindump during swipes puts 138 of 399 display-thread samples in `-[NBCommandBuffer
  commit]`, and 116 of those in `host_sync::copy_in`, the 4 KiB comparison of every no-copy buffer the device could not import, at
  every commit. Making those buffers import (why HakiosMali or PanVK refuses them: alignment, read-only mappings) is the next
  piece. GLES stays the default.
- No frame-time figure for Metal yet. The plug-in streams no frame line for a Metal backboardd; the eagl line is the GLES path's.

**The Astronomy wallpaper (AegirPoster), build 53, with the new plug-in log lines** (5e5dd13: the first IOSurface texture, the
first commit, presents by command buffer and by drawable, large texture loads, a tally at commits 1, 100, 1000 and 10000):

- AegirPoster makes its drawable's texture (`1125x2313`, MTLPixelFormat 555, over a `BGRA` surface) and loads a 2048x2048 texture.
  It never says its first commit, so the commit tally never runs.
- launchd kills it again and again: "Extension is hanging on launch. Killing." (`XPC_EXIT_REASON_EXTENSION_CHECK_IN_TIMEOUT`).
- A spindump of the launch has 181 of 181 main-thread samples under `-[NBEncoder endEncoding]` → `RenderCommandEncoder::end_pass` →
  `capture_steps` → `Device::owned_kernel` → `vkCreateComputePipelines` → PanVK's `bi_register_allocate`.
- The first pass loads its XR drawable, so it keeps its draws for capture replay, and the replay's three kernels were compiled on
  the program's thread. PanVK takes about a minute over them.
- **0007** (`0007-owned-kernels-ahead.patch`) makes them on a thread of their own. Until they are made the pass uses the host's
  reach, as the module's own rule says for capture forms not yet made. It needs an image to be seen in AegirPoster.

## Phase 5: no-copy imports, and the Astronomy wallpaper checks in (14 November 2026)

**AegirPoster, builds 54 to 56:**

- With 0007 it checks in ("extension checked in as alive"), makes its drawable's texture over the surface, commits once (3 render
  passes, 5 draws, 1 blit) and presents it by the drawable itself (`-present`). Settings > Wallpaper > Add New Wallpaper >
  Astronomy opens the editor, and the gallery and editor are not killed any more.
- Builds 54 and 55 showed the editor black with a blue patch at the top left. The drawable is `BGR10_XR_sRGB`. The frontend made an
  XR drawable's surface BGRA only for `presentDrawable:` (in its completed handler), so the compositor showed the XR words as BGRA
  bytes. Since 06e56c1 a drawable presented by itself is converted too, after the work that writes it. On build 56 the patch is
  gone and the frame is black, as converted.
- **Still no Earth:** AegirPoster commits that one frame and no other (no commit 100), and its 2048x2048 texture loads come after
  the first commit. What it waits for before a second frame is not known yet.
- The capture-replay kernels that 0007 moved off the program's thread are still being made after 9 minutes of one core at 100%.
  Symbolicated against the unstripped PanVK: `vk_common_CreateComputePipelines` → `panvk_compile_shaders` →
  `bifrost_compile_shader_nir` → `bi_register_allocate`, which spills one node per pass of up to 2000 passes, each rebuilding the
  interference graph. Until they are made the pass keeps the host's reach, as before, but the extension burns a core that long.

**The window server's no-copy buffers:**

- **Why they were copied.** PanVK had no `VK_EXT_external_memory_host`, so every no-copy buffer was copied (0002, 0006), whether it
  was an IOSurface's or the process's own. Alignment and read-only mappings were not the cause.
- **IOSurfaces are now imported.** `tools/mesa/panvk/panvk-host-surfaces.patch` imports an IOSurface's pages as HakiosMali's buffer
  of the surface by its ID (`kHakiosMaliSurface`, as `pan_kmod_bo_import` already did on Darwin), in the coherent type. The
  frontend tells the driver each surface it imports (`hakios_drm_host_surface`, as AdrenoMetal does for Turnip). With metal_tri,
  a 1024x1024 render pass into an IOSurface took 49.0 ms copied and takes 3.4 ms imported; `--surface`, `--fetch` and
  `--xr over|add` pass.
- **That was not the bulk.** The frontend's new tally (every 1000th commit) shows backboardd on Metal making 137 to 423
  no-copy buffers of its own memory, against 7 to 88 over IOSurfaces. Over commits 101 to 1000 on build 55, copying them cost
  94 ms a commit in and 64 ms out (reading back the device's write-combined memory to find what the GPU changed), and the GPU's
  commit-to-completion averaged 198 ms.
- **Importing the process's own pages (build 56, backed out):** HakiosMali request 0x7b (`ae4f5b6`, kept) wires a
  client's pages as a GPU buffer, and a PanVK memory type that is cached and not coherent imported them (metal2vulkan 0008
  flushes and invalidates such imports). A no-copy blit test (3 x 4 MB) took 7.8 ms against 206 ms copied, but it lost data at
  cache-line granularity: lines the GPU wrote read back as the CPU's old bytes, and lines the CPU changed read as the old ones on
  the GPU. That happened even with every line cleaned and invalidated by hand around each job (DC CIVAC, trapped and done by
  the kernel under patch 1511). iOS's `sys_dcache_flush()` is a DSB alone, so the frontend's `flushSurfaces` does nothing either.
  The GPU does not yet see a process's cached pages coherently, and backboardd's buffers were mostly refused anyway (it still
  copied 32 MB in 100 commits on build 56). PanVK went back to importing IOSurfaces only. Until both are understood, those buffers
  stay copied.

**Numbers** (`ps` once a second, six home-page swipes, `/var/tmp/m5/swipe.sh`):

| | GLES | Metal, build 54 (all copied) | Metal, build 55 (surfaces imported) | Metal, build 56 |
|---|---|---|---|---|
| backboardd CPU, swipes | 6 to 74% | 115 to 166% | 18 to 133% | 82 to 144% |
| backboardd CPU, idle | 4 to 20% | about 150% (Clock icon page) | 8 to 10% | about 125% |
| RSS | 103 to 115 MB | 176 to 186 MB | 133 to 148 MB | 146 to 157 MB |
| commit to completion (GPU) | (eagl line, about 9 ms median) | not measured | 142 to 198 ms average | 102 ms average |

GLES stays the default. Metal is not yet within reach of it for the window server.

## Phase 6: the process's own pages, coherent and imported (14 November 2026, on build 57)

**Why the GPU did not see a process's pages (wall 1).** It was never the Mali or its page tables:

- `tools/metal/vk_hostcoh.c` imports 4 MB of a process's pages through PanVK. Each round the CPU writes a pattern, the GPU
  copies it into its own memory and then fills the pages with another value, and every 64-byte line is checked on both sides.
  With build 56's PanVK (`0x7b`, pages wired by HakiosMali), 10 rounds:
  - nothing done about the caches: 420409 of 655360 lines lost into the GPU, 410076 out of it;
  - PanVK's `vkFlushMappedMemoryRanges` and `vkInvalidateMappedMemoryRanges` (what metal2vulkan 0008 calls): 366973 and
    350598 lost, the same, because they call `sys_dcache_flush()`, which on iOS 16.5 is a DSB alone;
  - DC CIVAC over every line, by the program, before the submit and after the wait: **none lost**.
- So the GPU sees cached pages correctly once their lines are cleaned and invalidated (JS_CONFIG already flushes the GPU's
  L2 at both ends of a job). Phase 5's note that lines were lost even with DC CIVAC around each job did not hold up here.
- The program cannot do it itself at any useful speed. With the Exynos M3s up, `SCTLR_EL1.UCI` is clear (xnu patch 1511),
  so each DC CIVAC from EL0 traps and the kernel does it: about 0.67 us a line, 87 ms a round for 4 MB, slower than
  metal2vulkan's copy.
- **The fix is in the kernel.** HakiosMali request `0x7a` (`syncBuffer`) cleans and invalidates a range of a buffer's lines
  on the kernel's alias of each wired page, as patch 1511 does, without holding its lock. hakios-drm's `hakios_drm_sync()`
  asks for it. PanVK's flush and invalidate of its cached type now ask for it too (`panvk-host-memory.patch`), so
  metal2vulkan's 0001 and 0008 steps do what they say.
- PanVK imports a process's own pages only where the kernel has `0x7a`. It asks once, of no buffer: a new kernel answers
  ENOENT and build 57's ENOSYS. Without `0x7a`, those pages are refused and copied as before. On build 57, `vk_hostcoh` with
  that PanVK says "the host pages were not imported", as it should.
- A hand-built PanVK that falls back to the trapped DC CIVAC where `0x7a` is missing loses nothing through the Vulkan path
  (`vk_hostcoh ... vk`: 5 rounds of 65536 lines, 0 lost either way). It is not committed; it was only for looking ahead of a
  kernel.

**Why most of backboardd's buffers were refused (wall 2).** The same hand build logged each import with `mach_vm_region`.

- Of 353 own-page imports at backboardd's start, 185 were taken and 168 refused.
- **Every refused one was read-only to backboardd** (protection and maximum protection read):
  - CoreAnimation's images shared from other processes: VM tag 51, shared, mapped by 3 to 40 processes, 32 KB to 1.4 MB;
  - read-only private copies of its own: tag 7, up to 10 MB.
- HakiosMali wired only writable pages (`kIODirectionInOut`), so metal2vulkan copied the rest at every commit.
- Now such pages are wired for reading and entered for the GPU to read only (AP 11 in the Mali's AArch64 tables; a job that
  writes one takes a permission fault), and `0x7b` answers `kMaliUserReadOnly`.
- Alignment and size were never the reason: metal2vulkan imports the window of whole pages around the bytes.

**What the window server's commits cost now (wall 3, first look).** backboardd on Metal with the hand-built PanVK on build
57's kernel: writable pages imported, read-only ones still copied. The driver was named through `launchctl setenv
HAKIOS_VK_ICD`, plus a `backboardd` line in the clients file; both were taken back after. Six home swipes
(`/var/tmp/coh/m6.sh`, which wakes and unlocks the screen first; an idle screen gave 4% flat):

| | GLES (build 57) | Metal, build 57's kernel, writable pages imported |
|---|---|---|
| backboardd CPU, six swipes | 7 to 92% | 82 to 161% |
| RSS | 101 to 115 MB | 140 to 158 MB |
| commits 2 to 100 | | 132 ms apart; commit to completion 82 ms on average |
| copied in, not imported | | 61 MB in 99 commits (3.3 s) |

- A 5 s spindump during the swipes put 180 of the display thread's 250 samples in `Buffer::copy_host_bytes` under
  `host_sync::copy_in`, and 101 of the handlers thread's in the same function for the copy out.
- PanVK's own work (submit, the trapped flushes) was under 20 samples.
- The copy in compared the caller's bytes with the buffer's own memory and then took that memory's digest. That memory is
  write-combined and reads back at about 25 MB/s.
- **metal2vulkan 0009** (`0009-copy-in-by-caller-digest.patch`) compares the caller's bytes with the digest recorded when they
  last agreed, and only writes the buffer's own memory.
- With the read-only pages imported too, little should be left to copy at all. That needs the kernel.
- 0009 builds, but it has not run on the phone yet: backboardd's dyld ignores `DYLD_LIBRARY_PATH` (set through `launchctl
  setenv`; the plug-in still named the old runtime's patch hash), and the runtime's path is fixed in the plug-in, so it needs
  an image.

**Needs an image** (kernel and userland): HakiosMali `0x7a` and read-only `0x7b` in the kernel; PanVK with
`panvk-host-memory.patch` and the new hakios-drm; metal2vulkan with 0009. Then backboardd on Metal is measured again the same
way, against GLES. GLES stays the default until then.

### On the phone, build 58 (kernel and image at 6607f09)

- `vk_hostcoh ... 4 20 vk`: 4 MB imported in type 1, 20 rounds of 65536 lines, **0 lost** either way; 1.14 ms of
  CPU and 1.12 ms of GPU a round (the trapped EL0 DC CIVAC cost 87 ms).
- backboardd on Metal (`backboardd` line in the clients file, `camtl=1`): no copies at all (0 KB in, 0 KB out;
  138 no-copy buffers imported, 7 over IOSurfaces). Home screen, dock translucency and blurs draw correctly.

| | GLES (build 58) | Metal (build 58) | Metal (build 57, copies) |
|---|---|---|---|
| backboardd CPU, six swipes | 6–69% | 24–80% | 82–161% |
| CPU idle after | ~20% | 27–33% | ~125% |
| RSS | 105–113 MB | 79–86 MB | 140–158 MB |
| commit to completion | ~9 ms | 20 ms average (111 ms slowest, start-up) | 82 ms |

The window server on Metal now costs about what GLES costs and uses less memory. What is left: the commit to
completion latency, and about 10% more idle CPU.

## Phase 7: where the window server's GPU time goes, and the Astronomy wallpaper's kernels (14 November 2026, on build 58)

**How it was measured.** The kernel's own line every 5 s (`dmesg`, "mali: GPU busy"), spindumps symbolicated against the
unstripped metal2vulkan and PanVK, and a test PanVK that is not committed: hakios-drm kept, for each job chain, when it was
submitted and when it was done (a thread waiting the job numbers in order), and PanVK described the chain's batch (its job
types, a compute job's grid, the shader's name or its kinds of bindings, a copy's image: size, tiling, row pitch, usage). It was
given to backboardd through `launchctl setenv HAKIOS_VK_ICD` (the file must be named `libvulkan_panfrost*`, or the plug-in does
not set PanVK's switches and the device is refused) and taken back after. Test runtimes ran in `metal_tri` through
`DYLD_LIBRARY_PATH`, which a program in `/redwood/bin` honours and backboardd does not.

**What the kernel says** (jobs and GPU time in 5 s):

| | vertex, tiler, compute | fragment | GPU busy |
|---|---|---|---|
| GLES, home screen idle | 150 (about 95 us each) | 150 (about 510 us) | 1% |
| GLES, swipes | 1850 to 1900 (86 to 99 us) | the same number (460 to 545 us) | 18 to 23% |
| Metal, idle | 650 to 755 (about 1 ms each) | 150 to 200 (about 300 us) | 15% |
| Metal, swipes | 2630 to 4290 (310 to 430 us) | 845 to 1490 (0.8 to 1.5 ms) | 39 to 70% |

A GLES frame is one vertex/tiler chain and one fragment chain. A Metal frame was about 20 job chains, most of them compute.

**One idle frame on Metal** (the trace; durations on the GPU):

| what | job chains | GPU |
|---|---|---|
| the home wallpaper, a 4:2:0 YCbCr texture over an IOSurface: its planes copied into a staging buffer (`vk-meta-copy-buffer`) | 2 | 0.4 ms |
| and converted to `RGBA16F` texels (metal2vulkan's `ycbcr_convert` kernel, 139x145 groups of 8x8) | 1 | **9.4 ms** |
| the framebuffer (1125x2313 BGRA8, rows of 4608 or 4544 bytes) copied into a shadow image before the frame | 1 | 1.3 ms |
| and copied back out after it | 1 | 1.2 ms |
| a 1271x1823 BGRA8 texture over rows of 5088 bytes, copied into its shadow | 1 | 1.2 ms |
| smaller textures copied in (656x688, 192x192, ...) | 3 to 6 | 0.1 to 0.3 ms each |
| CoreAnimation's render passes (4) | 8 | about 2 ms |

About 16 ms of GPU for a frame that costs GLES 0.6 ms. The conversion kernel compiles to 863 instructions, 80 cycles of loads and
stores, one thread per core (BIFROST_MESA_DEBUG=shaderdb), and metal2vulkan converts at every encoder that samples such a texture,
because the surface's bytes are the CPU's to write unseen (`ycbcr.rs`). Under swipes, the display thread spent 72 of 250 samples
in `panvk_queue_submit` asleep in `hakios_mali_enqueue`: the kernel's queue of 64 job chains was full, and it sleeps with
HakiosMali's lock held. Idle, backboardd's display thread used 12% of a core, most of it in metal2vulkan's
`RenderCommandEncoder::draw` / `draw_resident` (CoreAnimation redraws its layer tree every frame).

**Why the framebuffers were shadows.**

- The bridge's `shadow.log` was never written: it matched the notice's id against `shadowed-texture`, and every notice's id is
  `metal2vulkan-degrade-<kind>`. Fixed (`1aeb783`); every other degradation kind is now written once to `degradations.log` beside
  it.
- The reason, then (`metal_tri --surface --size 1125 --row-bytes 4608`, a new test option, `4701f2a`): the runtime takes back the
  bridge's `PaddedReadsPolicy::AliasWrites` for a texture a pass renders where a tile store at the right or bottom edge could
  reach past the buffer: it assumes every driver stores 16x4-texel blocks there, and 2313 rows round up past the surface's last
  page. **metal2vulkan 0010** (`09afad9`): the block is a fact of the driver, and PanVK's Mali writes back each tile's pixels
  inside the framebuffer descriptor's width and height (Mesa's own linear layout pads no height and rounds rows only to 64
  bytes): 1x1 on PanVK, 16x4 elsewhere.
- With that, the next refusal: the driver's linear image lays 1125 BGRA pixels 4544 bytes apart, and PanVK refused a
  `DRM_FORMAT_MOD_LINEAR` image at the surface's own rows for host memory (only `VK_IMAGE_TILING_LINEAR` was importable).
  **panvk-host-memory.patch** (`a98eb2a`) takes the modifier too. The same refusal made every padded texture that only reads (the
  1271x1823 one) a shadow; with this PanVK alone, backboardd's per-frame copy of it is gone (trace, build 58's runtime).
- `metal_tri --surface --size 1125 --row-bytes 4608 --frames 30`, every byte of the surface set to 0xA5 first: in place, the
  triangle is right and **no byte between the rows or after the last row changed**. Per frame: as a shadow, a 0.66 ms fragment
  job and a 0.66 ms copy back (two job chains); in place, one 1.05 ms fragment job into the linear surface.
- In place, HakiosMali's submit cleans the whole surface's cache lines (`flushSurface`, 5 MB for that test surface) at every
  submission that names it, under its lock: the time between a pass's two submissions went from 23 us to about 416 us. A
  surface of the display's scan-out pool is write-combined and skipped, which the window server's framebuffers are expected to
  be; others pay it.

**The Astronomy wallpaper.**

- **The kernels' compile.** `bi_register_allocate` spilled one value per pass, building the interference graph again each time
  (301 and 73 of 501 samples in `bi_allocate_registers` and `lcra_add_node_interference`), up to 2000 passes.
  **panvk-bifrost-spill-batches.patch** (`e7e3964`): the first 8 failed passes spill one value each, as before; after that twice
  as many every 8 passes, up to 64 at a time, the best by the same measure among the values that interfere with the node that
  failed. Capture replay's largest kernel (made by `metal_tri --xr over` with the pass loading its target, a test build): **93 s**
  (4320 bytes of spills), against more than 10 minutes, unfinished when the test ended, with Mesa's allocator. A shader that fits
  after a few spills is unchanged. Only PanVK takes the patch; the GLES driver does not.
- **Why there is no second frame.** Opening Settings > Wallpaper > Add New Wallpaper starts the gallery's snapshotter, which starts
  AegirPoster again every few seconds (a new pid each time, 100% of a core: the capture kernels begin compiling again in each).
  Each makes its first commit (3 render passes, 5 draws, 1 blit; three 2048x2048 texture loads of 4 MB, 12 ms in all) and its
  main thread then waits in `-[CAMetalDrawable present]` → the frontend's `drawableXrBeforePresent` → `waitForResource` →
  `-[NBCommandBuffer waitUntilCompleted]` for the frame's GPU work, which does not end: meanwhile the kernel logged **fragment
  jobs on slot 0 that did not finish within its 1 s limit** (`status 0x8`, still active, no MMU fault), 15 of them, each followed
  by a GPU reset, and 86% GPU busy. The extension is stopped before its frame completes. A hung fragment job holds the one
  queue for every client: backboardd's slowest commit-to-completion went to 1657 ms meanwhile.
- PosterBoard was restarted afterwards to end the loop (the lock screen came back with its default wallpaper). Extensions do not
  take `launchctl setenv`, so a test PanVK cannot be given to AegirPoster without an image.

**Needs an image** (userland only, no kernel change): PanVK with `panvk-host-memory.patch` and
`panvk-bifrost-spill-batches.patch`; metal2vulkan with 0010 and the bridge's notice log; `metal_tri`. Then backboardd on Metal
is measured again as in phase 6, and `degradations.log` and `shadow.log` in `/var/tmp/com.apple.backboardd/m2v` say what is
still degraded. Expected from the trace: about 4 ms and 3 job chains less per frame (the framebuffer's two copies and the padded
texture's), the conversion left.

**Next walls.**

1. **The wallpaper's conversion, 9.4 ms of every frame.** Exact and cheap needs to know when the planes changed: the runtime
   converts again only where the surface's bytes may have changed since the last conversion, decided where the command buffer
   commits (the host-sync step, as 0002's copies), from what the frontend can know (the surface's seed, which a CPU writer's
   unlock changes) and what the runtime knows (its own writes); the kernel would have to change a surface's seed when the
   video decoder or another process's GPU job writes it, which it does not today. Making the kernel itself cheaper (bytes
   loaded a word at a time) helps every conversion but not this many.
2. **HakiosMali's queue and lock**: a submission that finds the 64-chain queue full sleeps with the lock every other request
   takes; the whole-surface flush at each submission; and one client's fragment job that never ends stalls every client for
   1 s and resets the GPU.
3. **The Astronomy wallpaper's hung fragment job**: which pass (the trace PanVK in AegirPoster needs an image, or a
   `HakiosMetal.icd` naming it), then whether the shader loops or is only slow.
4. metal2vulkan's per-draw recording on the display thread (idle CPU).

**Making the window server's Metal the default (proposal, not done).** It is not close enough yet: about 16 ms of GPU a frame
against GLES's 0.6 ms (about 11 ms expected with this phase's patches), idle 27 to 33% CPU against about 20%, and a hung job of
any client stalls it. When the conversion is solved and a frame's GPU time is within twice GLES's: `userland/src/inject.c` leaves
the device in backboardd by default in a METAL=vk image whose `HakiosMetal.icd` names PanVK, with boot-arg `camtl=0` to keep it on
GLES (today `camtl=1` is needed); and the plug-in opens the device at once in backboardd (at `-initWithAcceleratorPort:`, since
the window server uses it within a second anyway) and answers nil when it cannot, so that CoreAnimation takes its GLES path: today
a device that fails to open answers nil from its first use on, after CoreAnimation has chosen Metal, and backboardd ends (seen
here with a driver path the plug-in refused). The clients file keeps its `-backboardd` line as the switch back without a new image.

### On the phone, build 59 (userland at 66531a0, kernel as build 58)

- `metal_tri` passes; backboardd on Metal draws the home screen correctly; no crash reports.
- backboardd, six swipes: CPU 18–86%, idle after 18–31%, RSS 74–76 MB (build 58: 24–80%, 27–33%, 79–86 MB).
- Commit to completion still about 20 ms on average (214 ms slowest, at start-up): the wallpaper's YCbCr conversion
  every frame is the larger share and is not fixed yet. No copies of no-copy buffers.
- `shadow.log` is written now: 16 small textures over no-copy buffers (A8Unorm and RGBA8Unorm, rows padded to
  the caller's) still go through a shadow image.

## Phase 8: the wallpaper converted only when it changes, and HakiosMali's queue, lock and cleans (14 November 2026, on build 59)

**Where it starts** (build 59, backboardd on Metal; `/var/tmp/coh/m6.sh`, six home swipes, and the kernel's 5 s lines):

- backboardd CPU 7 to 76% in the swipes, 26 to 31% idle after them, RSS 81 to 82 MB.
- GPU idle on the home screen: 11% busy; 350 to 381 vertex, tiler and compute jobs in 5 s at 1.4 to 1.5 ms each (528 ms: mostly the
  wallpaper's conversion, ten or so a second), 150 to 166 fragment jobs at about 0.3 ms. Swipes: 28 to 57% busy.

**The wallpaper's conversion (wall 1).** The planes are converted again only where they may have changed, as the seed of their
IOSurface says, decided where the command buffer is submitted, in the queue's order.

- *The seed.* A processor's lock or unlock for writing changes it, as before. Now a device's write changes it too, in the kernel
  (xnu fork `457143df6f1f`, `MiniIOSurfaceWritten`, `hakios_surface_written`):
  - the video decoder's frame written into a client's surface (`exynos_mfc`, zero copy);
  - a HakiosMali job chain whose submission marks the surface's handle as written (`kMaliSubmitWritten`). Mesa's panfrost knows
    which buffers a batch writes and marks them (`tools/mesa/panfrost-written-bos.patch`), so an OpenGL ES program drawing into a
    surface changes its seed; one only sampling it does not;
  - request `0x79` (`hakios_drm_surface_written`): work a client submitted writes a surface.
  
  Each change comes after the chain is queued, so a reader that saw the new seed submits behind the write.
- *The runtime* (metal2vulkan **0011**, `surface_watch.rs`):
  - The frontend watches every surface it imports (`nmb_buffer_watch_surface`): the seed, and a call announcing this process's
    GPU writes.
  - An encoder's first conversion of a YCbCr texture over a watched surface records nothing where no command before it in the
    command buffer wrote the planes or the image. The command buffer notes the texture.
  - Its first batch converts, at its start, each noted texture whose image does not hold the planes as of the seed it reads then.
    That conversion goes in a command buffer of its own, between barriers from and to every command.
  - What an image holds (seed, `VkQueue`) is forgotten once a batch writes the image or the planes.
  - After each batch, the watched surfaces it wrote are announced through hakios-drm (as a device's write). Where the driver cannot
    announce them (an older kernel, or a surface the driver copied), a lock and unlock for writing changes the seed instead.
  - A use after a write of the planes in the same command buffer converts in its encoder, as before.
- *What the seed does not see*: a processor writing the pixels without the lock IOSurface asks for. The frontend's own
  `replaceRegion:` of a texture over a surface now takes it.
- **`metal_tri --ycbcr`** checks it (`tools/metal/air/tri_ycbcr.ll`, a hand-written AIR fragment function that `read()`s a
  `YCBCR8_420_2P` texture over a `420f` surface at its own pixel). The steps draw:
  - twice, with the planes unchanged;
  - after the processor wrote them under the lock;
  - after a render pass cleared an `R8Unorm` texture over the luma plane, in a command buffer of its own and then in the draw's own.
  
  On the phone (build 59's kernel; 0011's runtime, frontend and PanVK by `DYLD_LIBRARY_PATH` and `HAKIOS_VK_ICD`), every step reads
  the codes the planes hold then, with the image's runtime and with 0011's. At 2048x2048 a step takes 33 ms from commit to completion
  with the image's runtime, and with 0011's **2.4 ms where the planes were unchanged** and 33 ms where they changed. The step after
  the GPU's own write converts once more: its batch wrote the planes. `metal_tri --surface --size 1125 --row-bytes 4608`,
  `--surface --frames 30` and `--fetch` pass with the new runtime, frontend and PanVK.

**HakiosMali (wall 2)** (`drivers/gpu/mali`):

- *The queue and the lock.* A submission reads its buffer list, cleans the surfaces it names, and waits for room in a full queue of
  64 chains, all without the lock every request takes. `hakios_mali_enqueue_for()` never sleeps; `hakios_mali_room()` waits.
  Before, the display thread slept there with every other client's requests behind it. A buffer given back now waits for its last
  job without the lock too (it waited up to 2 s with it).
- *Cleans by seed.* A surface's lines are cleaned before a job only where a processor may have written them since the last clean:
  its seed moved past `cleanSeed`, which a device's write keeps in step. A framebuffer the GPU draws into frame after frame was
  cleaned whole (5 MB, about 400 us) at every submission that named it. After the GPU writes, the cleans at `HakiosMaliDrawn()` and
  `WAIT_BO` stay as they were. `mali_flush=2` cleans before every job, as before, to compare.
- *A hung client is contained.* A job that faults is seen at once: the wait asked for the done bit alone, so a faulting job also
  held the queue for the whole second. For a job still running after the second (status 0x8):
  - the GPU is reset under it, as before;
  - its client's other chains queued by then are not run (counted done);
  - after 3 hangs the client's submissions are answered with no job until it goes;
  - the minute's line counts them ("job chains of clients whose jobs hung not run").
  
  Each AegirPoster the snapshotter starts can still hang the queue for a second, once.
- The kernel builds with all of it (`make kernel`: no error in the changed files; the symbol-set steps fail as they always do).

**Not done here.**

- *Wall 3, the 16 shadowed small textures* (`shadow.log`: A8Unorm and RGBA8Unorm text and icon textures, 94x54 to 600x600, over
  no-copy buffers):
  - Each sits at an offset in its pages (32, 64, 256, 6912, ...), but PanVK asks every image for 4096-byte alignment
    (`panvk_GetImageMemoryRequirements2`).
  - 13 of the 16 have rows that are multiples of 16 bytes but not of 64 (224, 288, 352, 608, 672, 720, 2400), where Mesa's linear
    layout rounds rows to 64.
  - What the Mali's texture descriptor takes (base alignment, row stride) needs a measurement on the phone first (a `metal_tri`
    texture over a buffer at such an offset and pitch), then PanVK's requirements for host memory.
- *Wall 4, the Astronomy wallpaper's hung fragment job*: tracing it needs the trace PanVK in AegirPoster, which takes an image
  (extensions take no `launchctl setenv`, and `HakiosMetal.icd` is read-only). The kernel now contains the hang.
- *Wall 5, idle CPU in the per-draw recording*: not started.
- *Wall 6, Metal by default*: waits for the measurement this phase needs an image for.

**Needs an image** (kernel and userland):

- *Kernel*: the xnu fork at `457143df6f1f` (`profiles/ios-16.5.conf`), `drivers/gpu/mali`, `drivers/media/mfc`.
- *Userland* (`METAL=vk`):
  - metal2vulkan with 0011 and the bridge's `nmb_buffer_watch_surface`;
  - the plug-in with the new frontend;
  - PanVK with the new hakios-drm;
  - Mesa's OpenGL ES with `panfrost-written-bos.patch` and the new hakios-drm;
  - `metal_tri` with `share/metal_tri.metallib` (now with `tri_ycbcr`).

Then:

- `metal_tri --ycbcr --size 2048`.
- backboardd on Metal measured as above (`m6.sh`, the kernel's GPU busy lines). Expected: the idle compute jobs and about 9 ms of
  every frame's GPU time gone, fewer chains a frame.
- No "client ... job hung" lines but AegirPoster's.
- Video (`mfc_zc`) and the wallpaper still drawn right.

### On the phone, build 60 (kernel with xnu 457143df6f1f, userland at 200c03d)

- `metal_tri` and `metal_tri --ycbcr --size 2048` pass. Unchanged planes take 2.4 ms from commit to completion
  against 33 ms when converted. One step still converts when it need not: "again, unchanged" after a pass cleared
  the luma plane in the same command buffer (33 ms; the values are right).
- backboardd on Metal draws the home screen correctly. Six swipes:

| | GLES (build 58) | Metal (build 59) | Metal (build 60) |
|---|---|---|---|
| backboardd CPU, swipes | 6–69% | 18–86% | 6–80% |
| CPU idle after | ~20% | 18–31% | 26–32% |
| RSS | 105–113 MB | 74–76 MB | 77–78 MB |
| commit to completion | ~9 ms | ~20 ms | 12.9 ms (commits 101–1000) |
| GPU busy, idle (kernel, per 5 s) | 1% | 11–15% | 1% |
| GPU busy, swipes | 18–23% | 28–57% | 21–37% |

The idle conversion jobs are gone. A frame's GPU time is now within twice GLES's, which is Phase 7's condition
for making Metal the window server's default. Idle CPU stays about 8 points above GLES.

## Phase 9: the window server on Metal by default, its idle cleans, the Astronomy wallpaper's Earth, and textures over buffers at their offsets (14 November 2026, on build 60)

How it was tried without an image: the plug-in, `libhakios.dylib` and the test PanVKs were built from this tree and
pushed over SSH. The system volume is read-only and a bindfs over `/redwood/lib` is refused to a shell (EPERM), so the
plug-in's window-server path ran in a stand-in process named `backboardd` that loads the bundle and sends
`-initWithAcceleratorPort:` as Metal does; test PanVKs reached backboardd through `launchctl setenv HAKIOS_VK_ICD` (a
file named `libvulkan_panfrost*`) and a restart of backboardd, and test programs through the environment. Everything
was taken back after: backboardd runs the image's PanVK on Metal, the clients file is as it was.

### Metal is the window server's default where the device is Metal on PanVK

Phase 7's proposal, done:

- **Who is shown the device** (`userland/src/hakios_metal_shown.c`). backboardd is shown it at `metal=2` or `3` when the
  plug-in's Vulkan driver is PanVK: `HAKIOS_VK_ICD`, else the first line of `HakiosMetal.icd` beside the bundle, names a
  `libvulkan_panfrost*` file (`hakios_metal_vk_driver()`, moved here from the opener so both decide by the same path).
  `camtl=0`, or a line `-backboardd` in either clients file, keeps the window server on OpenGL ES; the second needs no
  boot, only backboardd's next start. Any other driver or build: shown only at `metal=4` or when a clients file names it.
- **The plug-in opens the device at once in the window server** (`src/metalvk/HakiosMetalVK.mm`), in
  `-initWithAcceleratorPort:`, and answers nil when it cannot be opened. Metal's discovery tests the object before
  `MTLAddDevice()` (in Metal of 20F66, the call to the `initWithAcceleratorPort:` stub is followed by `cbz x0` past the
  add), so a nil answer is no device, `MTLCreateSystemDefaultDevice()` is nil, and CoreAnimation takes its OpenGL ES path.
  Before, a device that failed answered nil from its first use on, after CoreAnimation had chosen Metal, and backboardd
  ended. Every other process still opens the device at its first real use.
- **`inject.c`** leaves the device in backboardd without a `camtl=` on the line when it answers YES to
  `-hakiosOpenedForWindowServer` (the plug-in's: opened, for the window server); any other device is taken out as before.
  `camtl=1` leaves any device (the experiment it always was); `camtl=0` takes it out.
- The stand-in `backboardd` (the image's `libhakios.dylib` injected, `camtl=1` on the line, so this tests the plug-in's
  side): with the clients file's `backboardd` line removed and the image's PanVK, shown by default, opened in 134 to
  224 ms inside `-initWithAcceleratorPort:`, `-hakiosOpenedForWindowServer` YES; with `HAKIOS_VK_ICD` naming a PanVK file
  that is not there, shown, the open fails in 2 ms, and `-initWithAcceleratorPort:` answers nil ("no device for the
  window server: CoreAnimation draws with OpenGL ES"); with another driver named, not shown; with `-backboardd`, not
  shown; with the line `backboardd`, shown and opened. The no-`camtl=` path of `inject.c` needs a boot without `camtl=`.
- **Defaults.** `devices/star2lte` (scion-star2lte `4ebaa72`, pinned by `devices/manifest.conf`) has `CONFIG_METAL=2`:
  the S9+'s line carries `metal=2`, and `camtl=1` is no longer needed. The installer (`build-installer.sh`, scion-installer
  `441f592`) builds PanVK (`tools/mk-panvk.sh`) and the plug-in on Vulkan (`METAL=vk
  METAL_ICD=/redwood/lib/libvulkan_panfrost.dylib`, metal2vulkan through `mk-metal2vulkan.sh` and its own Rust) whenever
  the S9+ is among the phones, and checks for `llvm-as` (and Xcode's Metal compiler on macOS); the S9 and the Note9 get
  the same image and no `metal=`, so their kernels name no plug-in. Its README says so. The installer's kernel pin moves
  to this phase's last commit once it is pushed.

### Idle CPU: the cleans of pages no processor can write (wall 5 of phase 8)

A 10 s spindump of backboardd idle on the home screen (build 60, Metal): the display thread busy in 94 of 500 samples,
**90 of them in the kernel in `HakiosMaliUserClient::syncBuffer`** (request `0x7a`), under `host_sync::sync` →
`vkFlushMappedMemoryRanges` → `hakios_drm_sync`, at each command buffer's first batch. metal2vulkan's per-draw recording was
not where the time went (a few samples).

A test PanVK (not committed) logged every clean by buffer, with each import's region (`mach_vm_region`): **about 180 MB a
second cleaned idle**, 95% of it three buffers, CoreAnimation's decoded images (9 MB, 6.9 MB and 1.7 MB, VM tag 7),
cleaned whole at every frame. Each is read-only for good (maximum protection read), private and held by nothing else
(share mode private, one reference): no processor can write those pages after the import, and the GPU maps them read only.

- `hakios_drm_host_pages_sealed()` (`tools/mesa/hakios-drm`) says so of a range of pages, region by region; anything shared
  or writable anywhere is not. A mapping made from such pages is read-only for good as well, and a copy made writable
  (`VM_PROT_COPY`) is other pages.
- `panvk-host-memory.patch`: such an import is cleaned once, when it is made, and its flushes and invalidates are nothing to
  do after.
- What is still cleaned at every frame: CoreAnimation's images shared with other processes (VM tag 51, about 3.5 MB a
  second) and the small writable buffers.

| backboardd, Metal | image's PanVK (build 60) | with the sealed pages | GLES |
|---|---|---|---|
| CPU idle (CPU time over 20 s, woken, 5 s settled) | 27.9–28.9% | 22.8–23.7% | ~20% |
| CPU, six swipes (`m6.sh`) | 6–80% | 8.5–64% | 6–69% |
| RSS | 66–78 MB | 75–80 MB | 105–113 MB |

The display thread's busy samples idle went from 94 to 39 of 500. What is left over GLES, about 3 points, is spread over
CoreAnimation's own work, the submissions and the remaining cleans.

### The Astronomy wallpaper: the fragment shaders built a 4 KiB table in thread-local storage at every pixel (wall 4)

**A harness, no image needed.** NanoUniverse (the framework AegirPoster draws with) renders in any process:
`NUNIScene -initWithSphereoids:currentDateBlock:` (bit 3 the Earth, bit 4 the Moon), `-setSnap:3` (the Earth in focus),
`+[NUNIRenderer sharedInstanceWithPixelFormat:textureSuffix:rendererStyle:]` with style 2 (`NUNIAegirRenderer`; 0 is the
classic renderer, 1 Globetrotter), and `-renderWithScene:viewport:commandBuffer:passDescriptor:` into a `BGR10_XR_sRGB`
texture (555, the only format its pipelines are made for). Its first command buffer is AegirPoster's: 3 render passes, 6
draws, 1 blit (`aegir_starfield_fsh`, `aegir_star_fsh`, `aegir_earth_fsh`, `aegir_luna_fsh`, `aegir_threshold_fsh`,
`aegir_post_fsh`). It reproduces the hang: at 390x844 the frame's fragment job ran past HakiosMali's 1 s and was killed
(`status 0x8`, "client ... job hung"), the read-back showing the tiles done until then. It is slow, not looping: at
195x422 the Earth's frame took 440 ms, at 96x208 110 ms, about 5 us a pixel; at 1170x2532 about 20 s.

**Why.** `BIFROST_MESA_DEBUG=shaderdb`: every one of those fragment shaders spends 280 to 364 cycles a pixel in loads and
stores, for 0.5 to 3.5 of texturing. `BIFROST_MESA_DEBUG=shaders`: 2304 `store_scratch` in one frame's shaders. Each begins
by storing a 4 KiB constant table (`scratch: 4096`, 256 vec4 stores of words like 0x00010001, 0x00020002: a noise
function's permutation table) into thread-local storage, then reads a few words of it. PanVK keeps such a constant array a
function variable, and `bifrost_preprocess_nir()` puts every array over 256 bytes in thread-local storage.

**`panvk-large-constants.patch`**: `nir_opt_large_constants` after the global variables became local and before
`pan_shader_preprocess()`; each `load_constant` becomes a `load_global_constant` at the shader's own constant data, whose
address is a sysval (`panvk_{graphics,compute}_sysvals::const_data`), set to the bound shader's as its push uniforms are
made (so the vertex and fragment shaders of a draw each have their own). The data is uploaded beside the binary and
serialized with it.

| the harness, Earth and Moon in focus | before | with `panvk-large-constants.patch` |
|---|---|---|
| `store_scratch` in the frame's shaders | 2304 | 0 |
| load/store cycles a pixel (shaderdb) | 280–364 | 10–108 |
| frame at 195x422 | about 450 ms | 8–9 ms |
| frame at 1170x2532 | about 20 s (killed at 1 s) | 100 ms with the camera off the Earth; 0.7 s with the Earth filling the view (the rest of the scene 70 ms) |

The Earth renders: a disc with its atmosphere's rim, lit from the sun's side as the scene's date puts it, the starfield
behind. The 0.7 s is under HakiosMali's limit but slow: the Earth's shader is 5755 instructions at one thread a lane with
246 global loads of its tables. In AegirPoster itself it is not seen yet: that needs the image (extensions take no
`launchctl setenv`).

### Textures over no-copy buffers at their offsets (wall 3)

**`metal_tri --texel-buffer OFFSET [--a8] [--blit] [--render]`** (new): the triangle drawn by `tri_ycbcr` from an RGBA8Unorm
(or A8Unorm) texture over page-aligned no-copy memory at OFFSET, rows `--row-bytes` apart, judged texel by texel, and every
other byte of the memory checked unchanged; `--blit` has the GPU write the texels first, `--render` renders into the
texture and judges its memory. With a test PanVK whose host-allocation images asked any alignment (`P9_ALIGN`, not
committed):

| RGBA8 rows 720, A8 rows 224 or 288 | offsets 16, 32, 48, 64, 96, 6912 | offsets 1 to 8 |
|---|---|---|
| sampled | right, in place | Metal refuses them (`OffsetAlignment ... 16`) |
| blitted into | right, nothing outside the texels written | |
| rendered to (RGBA8) | right, nothing outside the texels written | |

Rows that are not multiples of 64 bytes (224, 288, 720) were never the obstacle: at offset 0 such a texture was already
in place. The page each image asked for was: `panvk_GetImageMemoryRequirements2` answered 4096 bytes of every image.
`panvk-host-memory.patch` now answers 64 bytes for a linear image of host allocations (Mesa's own minimum for a linear
surface on v6) and **16 on a v6 GPU where the image is no storage image**; a storage image is written through an attribute
buffer, whose pointer has no lower 6 bits on v6 (`Attribute Buffer`, `v6.xml`), and that was not measured.

In backboardd (a restart each, `shadow.log` emptied first): **18 textures shadowed with the image's PanVK, 8 with this one**.
metal2vulkan makes every such image with storage and color-attachment usage (`0x1f`), so the RGBA8 ones at offset 32 keep
64 bytes and stay shadows; the A8 ones at offsets of 64, 4608, 6784, 14592 bytes are shadows for another reason of
`aliases()` (the image's subresource larger than the room left in the buffer, by its rounded height), and a 1x1 BGRA8 one
as before.

### Not done

- **`metal_tri --ycbcr`'s "again, unchanged" after a same-command-buffer clear** still converts (33 ms; the values are right).
  Skipping it is correct only where the in-encoder conversion of that batch came after its last write of the planes, and
  no other writer changed the surface between the batch's submission and the runtime's own announcement of its write: the
  image then holds the planes as of the seed after that announcement. `SurfaceWatches::submitted()` forgets every image over
  bytes the batch wrote; it would need, per batch, the conversions that followed the last write, and the seed before and
  after the announcement (exactly one apart). Left as it is: a conversion more is right, one less would not be.

### Needs an image (userland only; the kernel is build 60's)

- the plug-in (`HakiosMetalVK.mm`, `HakiosMetalVKOpen.mm`, `hakios_metal_shown.c`) and `libhakios.dylib` (`inject.c`);
- PanVK with the new `panvk-host-memory.patch` (sealed pages, image alignment), `panvk-large-constants.patch` and the new
  hakios-drm (`tools/mk-panvk.sh`: the changed patches re-extract Mesa and rebuild PanVK whole);
- Mesa's OpenGL ES with the new hakios-drm (`hakios_drm_host_pages_sealed` is unused there);
- `metal_tri` (`--texel-buffer`).

Built as build 60 was (`userland/build.sh ALONE=1 RC=rc.launchd DEVICE=star2lte REST=1 GL=1 NO_AIR=1 SYSTEM=1
PANVK_DIR=... M2V_DIR=... METAL=vk METAL_ICD=/redwood/lib/libvulkan_panfrost.dylib`), the boot line without `camtl=1`
(`metal=2` from the S9+'s configuration). Then:

- the boot says "the window server draws with Metal" (inject.c) and "opened now, for the window server" (the plug-in);
- with `camtl=0` once, and with `-backboardd` in `/private/var/db/HakiosMetal.clients`, it draws with OpenGL ES;
- `m6.sh` and `cpuidle` as above; `shadow.log` of backboardd;
- Settings > Wallpaper > Add New Wallpaper > Astronomy: no "job hung" line, and the Earth drawn.

### Next walls

1. The Astronomy wallpaper's Earth at 0.7 s a frame: 246 global loads of its tables at one thread a lane. A table could be
   a UBO pushed in part to the FAU, or the shader kept at two threads (its register pressure).
2. The textures still shadowed in backboardd: metal2vulkan asks storage usage of every texture over a buffer, which costs
   the 16-byte alignment; and A8 images whose rounded height runs past the buffer.
3. The cleans of CoreAnimation's images shared with other processes (3.5 MB a second idle): their producers write them
   once; knowing that needs the kernel (a seed of the memory, as an IOSurface has).
