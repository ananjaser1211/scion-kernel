#!/bin/bash
# Build PanVK, Mesa's Vulkan driver for the Mali (src/panfrost/vulkan), for the Darwin userland: the
# Vulkan device a Metal-on-Vulkan plug-in (docs/metal-plan.md, "Metal on Vulkan"; docs/metal.md) can run
# on, on the Galaxy S9's Mali-G72 (Bifrost, architecture 6). Off by default: nothing in an image uses
# it unless the image is built with VK=1 (userland/build.sh), and PanVK itself refuses a v6 GPU unless
# the process sets PAN_I_WANT_A_BROKEN_VULKAN_DRIVER=1 (Mesa's own gate).
#
# The same pinned Mesa as tools/mk-mesa.sh (25.0.7, MIT), the same patches (tools/mesa/*.patch) and the
# same stand-in for libdrm (tools/mesa/hakios-drm), plus tools/mesa/panvk/*.patch for what PanVK needs
# of a system without Linux's DRM. Built in a tree of its own (xnu-deps/build/panvk), so the OpenGL ES
# library of mk-mesa.sh is not touched.
#
#   tools/mk-panvk.sh          -> xnu-deps/build/panvk/out/libvulkan_panfrost.dylib
#                                 (iOS arm64, install name /redwood/lib/libvulkan_panfrost.dylib; it
#                                 exports vk_icdGetInstanceProcAddr and
#                                 vk_icdNegotiateLoaderICDInterfaceVersion, the entry
#                                 tools/metal/bridge/icd.rs opens a driver by)
#                                 and out/vk_smoke (tools/vk/vk_smoke.c, linked to it)
#   tools/mk-panvk.sh mac      -> the same for this Mac, in out/mac, and runs vk_smoke there with
#                                 HAKIOS_DRM_NOOP=1: a made-up Mali-G72 that runs nothing
#                                 (tools/mesa/hakios-drm/xf86drm.h), so everything up to the GPU --
#                                 instance, device, the Bifrost shader compiler, command building,
#                                 submission -- is gone through without the phone. What the GPU would
#                                 write is never written, so vk_smoke's read-backs fail there by design.
#
# Needs what mk-mesa.sh needs (meson, ninja, bison, flex, python3 with venv, llvm-ar, llvm-strip);
# glslang is not needed (PanVK's meta shaders are NIR built in C). JOBS (default 4) for ninja.
set -e
shopt -s nullglob
VERSION=25.0.7
SHA256=592272df3cf01e85e7db300c449df5061092574d099da275d19e97ef0510f8a6
ROOT=$(cd "$(dirname "$(readlink -f "$0")")/.." && pwd)
source "$ROOT/xnu-env.sh"
B=$ROOT/xnu-deps/build/panvk
SDK=$THEOS/sdks/iPhoneOS16.5.sdk
CC=$ROOT/xnu-deps/host/bin/xnu-clang
TARGET=${1:-ios}
case $TARGET in ios|mac) ;; *) echo "usage: $0 [ios|mac]" >&2; exit 2;; esac
mkdir -p "$B/out/mac"
cd "$B"

PATCHLIST=("$ROOT"/tools/mesa/*.patch "$ROOT"/tools/mesa/panvk/*.patch)
PATCHES=$(cat "${PATCHLIST[@]}" | sha256sum | cut -d' ' -f1)
if [ ! -d mesa-$VERSION ] || [ "$(cat mesa-$VERSION/.hakios-patches 2>/dev/null)" != "$PATCHES" ]; then
	if [ ! -f mesa-$VERSION.tar.xz ]; then
		# the tarball mk-mesa.sh already fetched, if it is there
		if [ -f "$ROOT/xnu-deps/build/mesa/mesa-$VERSION.tar.xz" ]; then
			cp "$ROOT/xnu-deps/build/mesa/mesa-$VERSION.tar.xz" .
		else
			curl -sL --fail -o mesa-$VERSION.tar.xz https://archive.mesa3d.org/mesa-$VERSION.tar.xz
		fi
	fi
	echo "$SHA256  mesa-$VERSION.tar.xz" | sha256sum -c --quiet
	rm -rf mesa-$VERSION build build-mac
	tar xJf mesa-$VERSION.tar.xz
	for p in "${PATCHLIST[@]}"; do
		patch -s --no-backup-if-mismatch -d mesa-$VERSION -p1 < "$p"
	done
	echo "$PATCHES" > mesa-$VERSION/.hakios-patches
fi
rm -rf mesa-$VERSION/src/hakios-drm
cp -r "$ROOT/tools/mesa/hakios-drm" mesa-$VERSION/src/hakios-drm
# the AIR frontend is not built here, but tools/mesa/air-frontend.patch's meson names its directory
rm -rf mesa-$VERSION/src/compiler/air
cp -r "$ROOT/tools/mesa/air" mesa-$VERSION/src/compiler/air
if [ ! -x venv/bin/python3 ]; then
	python3 -m venv venv
	venv/bin/pip -q install mako pyyaml packaging
fi
export PATH=$B/venv/bin:$PATH

cat > cross.ini <<INI
[binaries]
c = ['$CC', '-arch', 'arm64', '-isysroot', '$SDK']
cpp = ['$CC++', '-arch', 'arm64', '-isysroot', '$SDK', '-fuse-cxa-atexit', '-stdlib=libc++']
objc = ['$CC', '-arch', 'arm64', '-isysroot', '$SDK']
ar = 'llvm-ar'
ranlib = 'llvm-ranlib'
strip = 'llvm-strip'
pkg-config = 'false'

[host_machine]
system = 'darwin'
subsystem = 'ios'
cpu_family = 'aarch64'
cpu = 'arm64'
endian = 'little'

[properties]
needs_exe_wrapper = true
INI
OPTIONS=(--wrap-mode=nofallback --prefix=/redwood --libdir=lib --buildtype=release -Db_ndebug=true
	-Dplatforms= -Dgallium-drivers= -Dvulkan-drivers=panfrost -Dglx=disabled -Degl=disabled -Dgbm=disabled
	-Dosmesa=false -Dllvm=disabled -Dzlib=disabled -Dshared-glapi=disabled -Dgles1=disabled -Dgles2=disabled -Dopengl=false
	-Dzstd=disabled -Dxmlconfig=disabled -Dexpat=disabled -Dshader-cache=disabled -Dvalgrind=disabled
	-Dlibunwind=disabled -Dglvnd=disabled -Dvideo-codecs= -Dtools= -Dgallium-vdpau=disabled -Dgallium-va=disabled
	-Dgallium-xa=disabled -Dlmsensors=disabled -Dbuild-tests=false -Dvulkan-layers= -Dperfetto=false)
if [ "$TARGET" = ios ]; then
	BUILD=build OUT=out SMOKE_CC=("$CC" -arch arm64 -isysroot "$SDK")
	[ -f $BUILD/build.ninja ] || meson setup $BUILD mesa-$VERSION --cross-file cross.ini "${OPTIONS[@]}" > meson-setup.log
else
	BUILD=build-mac OUT=out/mac SMOKE_CC=(cc)
	[ -f $BUILD/build.ninja ] || CC=cc CXX=c++ meson setup $BUILD mesa-$VERSION "${OPTIONS[@]}" > meson-setup-mac.log
fi
ninja -j "${JOBS:-4}" -C $BUILD src/panfrost/vulkan/libvulkan_panfrost.dylib > ninja-$TARGET.log

llvm-strip -x -o $OUT/libvulkan_panfrost.dylib $BUILD/src/panfrost/vulkan/libvulkan_panfrost.dylib
install_name_tool -id /redwood/lib/libvulkan_panfrost.dylib $OUT/libvulkan_panfrost.dylib 2>/dev/null
[ "$TARGET" = mac ] && install_name_tool -id @rpath/libvulkan_panfrost.dylib $OUT/libvulkan_panfrost.dylib 2>/dev/null
nm -gU $OUT/libvulkan_panfrost.dylib | awk '{ print substr($3, 2) }' | sort > $OUT/exported.txt
# Left to dyld (panvk-darwin.patch: -undefined dynamic_lookup): only the entrypoint tables' weak
# references may be, which dyld makes NULL when nothing defines them. Anything else so left is a
# function the library needs and nothing gives -- most likely one hakios.drm.c lacks -- and would
# be found missing only on the phone; it fails the build here instead.
nm -m $OUT/libvulkan_panfrost.dylib | grep 'dynamically looked up' | grep -v 'weak external' \
	| sed 's/.*external _//; s/ .*//' > $OUT/missing.txt || true
if [ -s $OUT/missing.txt ]; then
	echo "mk-panvk.sh: needed and defined nowhere: $(tr '\n' ' ' < $OUT/missing.txt)" >&2
	exit 1
fi
nm -m $OUT/libvulkan_panfrost.dylib | grep -c 'dynamically looked up' > $OUT/weak-entrypoints.txt || true

# the smoke test: instance, the Mali, a device, a buffer and an image, a clear, a copy, a timeline
# semaphore, a compute dispatch; every step says what it got (tools/vk/vk_smoke.c)
"${SMOKE_CC[@]}" -O2 -Wall -I mesa-$VERSION/include -o $OUT/vk_smoke "$ROOT/tools/vk/vk_smoke.c" \
	$OUT/libvulkan_panfrost.dylib -Wl,-rpath,@executable_path
cat "${PATCHLIST[@]}" "$ROOT"/tools/mesa/hakios-drm/* | sha256sum > $OUT/built-from
echo "mk-panvk.sh: Mesa $VERSION PanVK ($TARGET): $B/$OUT/libvulkan_panfrost.dylib, $(stat -c %s $OUT/libvulkan_panfrost.dylib) bytes," \
	"$(grep -c '^vk_icd' $OUT/exported.txt) vk_icd* entry points, $(cat $OUT/weak-entrypoints.txt) weak entrypoints left to dyld"
if [ "$TARGET" = mac ]; then
	echo "mk-panvk.sh: vk_smoke on this Mac, HAKIOS_DRM_NOOP=1 (a Mali-G72 that runs nothing):"
	HAKIOS_DRM_NOOP=1 $OUT/vk_smoke --mali-v6 || true
	# and as a Metal-on-Vulkan runtime would open it: a 1.2 device (metal2vulkan's floor), the version
	# PanVK reports on v6 (1.0) overridden by Mesa's own MESA_VK_VERSION_OVERRIDE (docs/metal.md)
	echo "mk-panvk.sh: the same, asking for Vulkan 1.2 with MESA_VK_VERSION_OVERRIDE=1.2:"
	HAKIOS_DRM_NOOP=1 MESA_VK_VERSION_OVERRIDE=1.2 $OUT/vk_smoke --mali-v6 --api 1.2 || true
fi
