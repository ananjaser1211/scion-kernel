#!/bin/bash
# Build libmetal2vulkan.dylib: the Metal API on Vulkan, for the plug-in Metal loads (userland/src/metalvk/, METAL=vk in
# userland/build.sh; docs/metal-plan.md, "Metal on Vulkan").
#
#   tools/metal/mk-metal2vulkan.sh fetch   fetch and patch the sources, nothing more
#   tools/metal/mk-metal2vulkan.sh mac     the library for this Mac, the test program tools/metal/m2v_test.c, and the test run
#                                          on a Vulkan driver of the Mac (VK_ICD, default lavapipe)
#   tools/metal/mk-metal2vulkan.sh ios     the library for arm64 iOS 16 (install name /redwood/lib/libmetal2vulkan.dylib), and the
#                                          test program and its metallib for the guest (out/ios/m2v_test, m2v_test.metallib)
#
# What is in the library:
#   - metal2vulkan (github.com/steelbrain/metal2vulkan, GPL-2.0-or-later OR LGPL-3.0-or-later): its runtime (the Metal objects and
#     calls on any Vulkan 1.2 device, through ash), its translator (AIR to SPIR-V) and its AIR reader, at the pinned M2V_REV, with
#     AdrenoMetal's patches to it (bridge/patches/, 16 of them; all apply to this revision) and ours (tools/metal/m2v/*.patch, under
#     metal2vulkan's licence, when there are any). Fetched into xnu-deps/build/metal2vulkan/src, never committed.
#   - AdrenoMetal's C-ABI bridge over the runtime (tools/metal/bridge/, MIT, NativeMetal contributors), and icd.rs: the Vulkan
#     driver is an ICD opened by its path (vk_icdGetInstanceProcAddr or vkGetInstanceProcAddr), there being no Vulkan loader on iOS.
#   - the self-test's shaders (tools/metal/m2v_selftest.metal, m2v_constant.metal) as AIR, made with Xcode's Metal compiler
#     (`xcodebuild -downloadComponent MetalToolchain` once, if `xcrun metal` says the toolchain is missing).
# It stays a library of its own, never linked into a program statically, so that it can be replaced: the LGPL's condition.
#
# Why this revision. AdrenoMetal's bridge and patches were made against metal2vulkan as it was on 14 October 2026; its history was
# landed again on 26 October, and the revision below is the newest one of the new history whose files all 16 patches apply to
# (37 of the 38 files they touch are byte for byte the patches' bases; texture.rs differs and its hunks apply). At metal2vulkan's
# master of 29 October 13 of the 16 do not apply, and the bridge would have to follow the runtime's API.
#
# Rust: rustup, toolchain RUST_VERSION (metal2vulkan's own pin, rust-toolchain.toml) with the aarch64-apple-ios target, kept apart
# from the machine's own Rust: RUSTUP_HOME and CARGO_HOME under HAKIOS_RUST (default xnu-deps/rust), installed there when missing.
#
# Disk: the toolchain about 0.6 GB; the sources 0.1 GB; a build's intermediates about 1 GB each for mac and ios, removed after the
# build unless KEEP=1 (a later build then starts from nothing: some minutes).
#
#   M2V_REV, ADRENOMETAL_REV   the pins
#   M2V_PATCHES=0              without AdrenoMetal's patches (the bridge then does not build: it uses PaddedReadsPolicy::AliasWrites)
#   VK_ICD                     the mac test's driver (/opt/homebrew/lib/libvulkan_lvp.dylib; MoltenVK and KosmicKrisp work the same)
#   SDK                        the iOS SDK (default: THEOS's iPhoneOS16.5.sdk, else Xcode's)
set -e
M2V_REV=${M2V_REV:-720ef1ce9ae2a2068999a8675717418b9416283e}
ADRENOMETAL_REV=${ADRENOMETAL_REV:-ce5e19e33e05e1873d38d818e23cabfe0ac708c3}
RUST_VERSION=${RUST_VERSION:-1.98.1}
ROOT=$(cd "$(dirname "$(readlink -f "$0")")/../.." && pwd)
B=$ROOT/xnu-deps/build/metal2vulkan
RUST=${HAKIOS_RUST:-$ROOT/xnu-deps/rust}
WHAT=${1:-mac}
# Xcode's own tools: called from userland/build.sh, PATH starts with the kernel build's stand-ins (xnu-deps/host/bin: an xcrun that
# answers the kernel's SDK, a clang that targets it), and Homebrew's binutils may come before Xcode's strip and nm.
#
# On Linux (the WSL build host) there is no Xcode: only `ios` is built, linked by the kernel build's xnu-clang (cctools ld64) against
# THEOS's iPhoneOS16.5 SDK and stripped by its cctools strip. The self-test's shaders need Xcode's Metal compiler, so the library is
# built without them (nmb_selftest then has no fixtures), and m2v_test with its metallib is not made; the plug-in's own check on the
# phone is userland/src/metalvk/metal_tri.m, whose shaders are hand-written AIR (tools/metal/air, tools/metal/mkmetallib.py).
HOSTBIN=$ROOT/xnu-deps/host/bin
PATH=$(echo "$PATH" | tr ':' '\n' | grep -v '/xnu-deps/host/bin$' | paste -sd: -)
XCRUN=/usr/bin/xcrun
LINUX=
[ "$(uname)" = Darwin ] || LINUX=1
mkdir -p "$B"
cd "$B"

# ---- the sources: metal2vulkan's three crates (a sparse checkout) and AdrenoMetal's patches ----
if [ ! -d AdrenoMetal/.git ]; then
	git clone -q --filter=blob:none --no-checkout https://github.com/UniXNU/AdrenoMetal.git AdrenoMetal
fi
if [ "$(git -C AdrenoMetal rev-parse HEAD 2>/dev/null)" != "$ADRENOMETAL_REV" ]; then
	git -C AdrenoMetal fetch -q origin
	git -C AdrenoMetal checkout -q --detach "$ADRENOMETAL_REV"
fi
if [ ! -d src/.git ]; then
	git clone -q --filter=blob:none --no-checkout https://github.com/steelbrain/metal2vulkan.git src
fi
# `init --cone` first: git before 2.35 takes `set --cone` as a pattern named --cone, and the checkout then lacks the workspace's
# Cargo.toml at the top (the crates inherit their edition from it). A checkout made that way is put right here as well.
if [ ! -f src/Cargo.toml ] || [ "$(git -C src sparse-checkout list 2>/dev/null | head -1)" = --cone ]; then
	git -C src sparse-checkout init --cone
	git -C src sparse-checkout set crates/metal2vulkan crates/metal2vulkan-air crates/metal2vulkan-runtime
fi
PATCHES=$( { echo "$M2V_REV ${M2V_PATCHES:-1}"; [ "${M2V_PATCHES:-1}" = 0 ] || cat AdrenoMetal/bridge/patches/*.patch;
	cat "$ROOT"/tools/metal/m2v/*.patch 2>/dev/null || true; } | shasum -a 256 | cut -c1-16)
if [ "$(cat src/.hakios-patches 2>/dev/null)" != "$PATCHES" ]; then
	git -C src cat-file -e "$M2V_REV^{commit}" 2>/dev/null || git -C src fetch -q origin
	git -C src checkout -q -f --detach "$M2V_REV"
	git -C src clean -q -fdx
	if [ "${M2V_PATCHES:-1}" != 0 ]; then
		for p in AdrenoMetal/bridge/patches/*.patch; do patch -s -p1 --no-backup-if-mismatch -d src < "$p"; done
	fi
	for p in "$ROOT"/tools/metal/m2v/*.patch; do
		[ -f "$p" ] && patch -s -p1 --no-backup-if-mismatch -d src < "$p"
	done
	echo "$PATCHES" > src/.hakios-patches
	echo "mk-metal2vulkan.sh: metal2vulkan $M2V_REV, patches $PATCHES"
fi
[ "$WHAT" = fetch ] && exit 0

# ---- Rust, private to this tree ----
export RUSTUP_HOME=$RUST/rustup CARGO_HOME=$RUST/cargo RUSTUP_TOOLCHAIN=$RUST_VERSION
if [ ! -x "$CARGO_HOME/bin/cargo" ]; then
	mkdir -p "$RUST"
	curl -sSf --proto '=https' --tlsv1.2 -o "$RUST/rustup-init.sh" https://sh.rustup.rs
	sh "$RUST/rustup-init.sh" -y -q --no-modify-path --profile minimal --default-toolchain "$RUST_VERSION" --target aarch64-apple-ios
fi
CARGO=$CARGO_HOME/bin/cargo
"$CARGO_HOME/bin/rustup" target list --installed | grep -q aarch64-apple-ios || "$CARGO_HOME/bin/rustup" target add aarch64-apple-ios

# ---- the bridge, beside the sources (its manifest names them as ../src) ----
rm -rf bridge
cp -R "$ROOT/tools/metal/bridge" bridge
mkdir -p air out/include
cp bridge/NativeMetalObjects.h bridge/NativeMetalBridge.h out/include/
export M2V_RUNTIME_ID="metal2vulkan-${M2V_REV:0:12}-$PATCHES"

# the self-test's shaders as AIR bitcode, for the platform the library is for
air() {
	local sdk=$1 target=$2
	if [ -n "$LINUX" ]; then
		echo "mk-metal2vulkan.sh: no Xcode here: the library is built without the self-test's shaders"
		return
	fi
	for s in m2v_selftest m2v_constant; do
		$XCRUN -sdk "$sdk" metal -target "$target" -c -o "air/$s.$sdk.air" "$ROOT/tools/metal/$s.metal"
	done
	export M2V_FIXTURE_AIR=$B/air/m2v_selftest.$sdk.air M2V_FC_AIR=$B/air/m2v_constant.$sdk.air
}

case "$WHAT" in
mac)
	air macosx air64-apple-macos13.0
	(cd bridge && CARGO_TARGET_DIR="$B/target-mac" "$CARGO" build -q --locked --release)
	mkdir -p out/mac
	cp target-mac/release/libmetal2vulkan_bridge.dylib out/mac/libmetal2vulkan.dylib
	$XCRUN install_name_tool -id @rpath/libmetal2vulkan.dylib out/mac/libmetal2vulkan.dylib
	# the test: the self-test's three checks, then a compute kernel and a triangle of tools/metal/m2v_test.metal through the C ABI
	$XCRUN -sdk macosx metal -target air64-apple-macos13.0 -c -o air/m2v_test.air "$ROOT/tools/metal/m2v_test.metal"
	$XCRUN -sdk macosx metallib -o out/mac/m2v_test.metallib air/m2v_test.air
	clang -O1 -Wall -Wextra -Iout/include -o out/mac/m2v_test "$ROOT/tools/metal/m2v_test.c" \
		-Lout/mac -lmetal2vulkan -Wl,-rpath,"$B/out/mac"
	[ -n "$KEEP" ] || rm -rf target-mac
	ICD=${VK_ICD:-/opt/homebrew/lib/libvulkan_lvp.dylib}
	mkdir -p "$B/cache-mac"
	out/mac/m2v_test "$ICD" "$B/cache-mac" out/mac/m2v_test.metallib out/mac/triangle.ppm
	;;
ios)
	if [ -z "$SDK" ]; then
		SDK=${THEOS:-$HOME/theos}/sdks/iPhoneOS16.5.sdk
		[ -d "$SDK" ] || SDK=$($XCRUN --sdk iphoneos --show-sdk-path)
	fi
	air iphoneos air64-apple-ios16.0
	if [ -n "$LINUX" ]; then
		# rustc's Apple targets take the SDK from SDKROOT where there is no xcrun (the build scripts are built for Linux and do
		# not read it), and link with the C compiler they are given: the kernel build's, which drives cctools' ld64
		(cd bridge && IPHONEOS_DEPLOYMENT_TARGET=16.0 M2V_INSTALL_NAME=/redwood/lib/libmetal2vulkan.dylib SDKROOT="$SDK" \
			CARGO_TARGET_AARCH64_APPLE_IOS_LINKER="$HOSTBIN/xnu-clang" \
			CARGO_TARGET_AARCH64_APPLE_IOS_RUSTFLAGS="-C link-arg=-isysroot -C link-arg=$SDK" \
			CARGO_TARGET_DIR="$B/target-ios" "$CARGO" build -q --locked --release --target aarch64-apple-ios)
		STRIP="$HOSTBIN/strip" NM="$HOSTBIN/nm"
	else
		# The SDK for the library's own link only (SDKROOT would reach the build scripts, which are built for this Mac): rustc names
		# xcrun's iPhoneOS SDK first, and the later -isysroot is the one the linker takes.
		(cd bridge && IPHONEOS_DEPLOYMENT_TARGET=16.0 M2V_INSTALL_NAME=/redwood/lib/libmetal2vulkan.dylib \
			CARGO_TARGET_AARCH64_APPLE_IOS_RUSTFLAGS="-C link-arg=-isysroot -C link-arg=$SDK" \
			CARGO_TARGET_DIR="$B/target-ios" "$CARGO" build -q --locked --release --target aarch64-apple-ios)
		STRIP="$XCRUN strip" NM="$XCRUN nm"
	fi
	mkdir -p out/ios
	# Xcode's or cctools' strip (a GNU strip found first on PATH turns a Mach-O into nonsense); -x keeps the exported symbols
	$STRIP -x -o out/ios/libmetal2vulkan.dylib target-ios/aarch64-apple-ios/release/libmetal2vulkan_bridge.dylib
	[ -n "$KEEP" ] || rm -rf target-ios
	echo "$M2V_RUNTIME_ID" > out/ios/built-from
	# what userland/build.sh METAL=vk compares to tell this library from one built of other sources (the same command there)
	(cd "$ROOT/userland" && cat ../tools/metal/mk-metal2vulkan.sh ../tools/metal/bridge/* ../tools/metal/*.metal \
		../tools/metal/m2v_test.c ../tools/metal/m2v/*.patch 2>/dev/null | sha256sum) > out/ios/built-from-tree
	# the same test as on the Mac, for the guest (userland/build.sh METAL=vk puts it in /redwood/bin, its metallib in /redwood/share;
	# userland/rc.jobs.metalvk runs it on the guest's Vulkan driver); its shaders need Xcode, so not on Linux
	rm -f out/ios/m2v_test out/ios/m2v_test.metallib
	if [ -z "$LINUX" ]; then
		$XCRUN -sdk iphoneos metal -target air64-apple-ios16.0 -c -o air/m2v_test.iphoneos.air "$ROOT/tools/metal/m2v_test.metal"
		$XCRUN -sdk iphoneos metallib -o out/ios/m2v_test.metallib air/m2v_test.iphoneos.air
		$XCRUN clang -target arm64-apple-ios16.0 -isysroot "$SDK" -O1 -Wall -Wextra -Iout/include -o out/ios/m2v_test \
			"$ROOT/tools/metal/m2v_test.c" out/ios/libmetal2vulkan.dylib
	fi
	echo "mk-metal2vulkan.sh: $B/out/ios/libmetal2vulkan.dylib ($(wc -c < out/ios/libmetal2vulkan.dylib) bytes," \
		"$($NM -gU out/ios/libmetal2vulkan.dylib | grep -c ' _nmb_') nmb_* functions)"
	;;
*)
	echo "usage: $0 fetch|mac|ios" >&2
	exit 1
	;;
esac
