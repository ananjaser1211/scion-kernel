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

**A semantic difference, written down rather than changed.** The stand-in treats a sync object that was reset (no fence) as
signalled. Linux treats it as unsignalled, and waiting on it without `WAIT_FOR_SUBMIT` fails there. Panfrost's OpenGL ES path,
which runs today, relies on the stand-in as it is, so this was not changed for PanVK. A Vulkan program that resets a fence and
then waits on it without submitting would return at once here, where Linux would time out. vk_smoke does not do that, but a
conformance test would.

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
