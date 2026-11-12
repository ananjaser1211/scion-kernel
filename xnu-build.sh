#!/bin/bash
# Build XNU for redwood on Linux. Extra make args are passed through.
source "$(dirname "${BASH_SOURCE[0]}")/xnu-env.sh"
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
cd "$XNU_SRC" || exit 1
# The Wi-Fi driver's object is linked into the kernel when it is there; make does not see it
# change, so the linked kernel is thrown away when it has. Which object is the radio driver's:
# ath11k's (tools/mk-ath11k.sh) for the WCN6750 (CONFIG_HAKIOS_WIFI), else brcmfmac's
# (tools/mk-brcmfmac.sh) for the Galaxy S9's BCM4361 (CONFIG_HAKIOS_BRCMFMAC); none for a kernel
# with neither. The object's calls are answered by hakios.lx.c.
LX=$XNUDEPS/build/ath11k/out/ath11k.o
LX_BUILD=$ROOT/tools/mk-ath11k.sh
if [ -f config/MASTER.hakios ] && ! grep -q '^#  HAKIOS_DRIVERS = \[.* hakios_wifi ' config/MASTER.hakios; then
	LX=/nonexistent
	LX_BUILD=
	if grep -q '^#  HAKIOS_DRIVERS = \[.* hakios_brcmfmac ' config/MASTER.hakios; then
		LX=$XNUDEPS/build/brcmfmac/out/brcmfmac.o
		LX_BUILD=$ROOT/tools/mk-brcmfmac.sh
	fi
fi
if [ -n "$LX_BUILD" ] && [ ! -f "$LX" ]; then
	echo "xnu-build: building required external Wi-Fi object: $LX"
	"$LX_BUILD" || exit 1
fi
if [ -n "$LX_BUILD" ] && [ ! -f "$LX" ]; then
	echo "xnu-build: required Wi-Fi object was not produced: $LX" >&2
	exit 1
fi
STAMP=BUILD/obj/hakios-objects
# (macOS: its own stat, not tools/macos-bin/stat, which is GNU stat for the scripts written for it and may be absent)
mtime() { if [ "$XNU_HOST_OS" = Darwin ]; then /usr/bin/stat -f %m "$1"; else stat -c %Y "$1"; fi; }
if [ -f "$LX" ]; then now="$LX $(mtime "$LX")"; else LX= now=; fi
# the build tools of SETUP/ are Apple's: on macOS they build as they are, on Linux with a compatibility header
if [ "$XNU_HOST_OS" = Darwin ]; then HOSTCC=clang; else
HOSTCC="clang -include $XNUDEPS/hostinc/hostcompat.h -I$XNUDEPS/hostinc -Wno-unused-command-line-argument -Wno-error=implicit-function-declaration"
# Apple's three Mach-O headers they need are no longer in the repository: xnu-deps/setup.sh copies them from this tree,
# and a setup made before that (setup.sh is not run again while host/bin/ld64 is there) gets them here
mkdir -p "$XNUDEPS/hostinc/mach-o"
for f in loader.h nlist.h fat.h; do
	[ -f "$XNUDEPS/hostinc/mach-o/$f" ] || cp EXTERNAL_HEADERS/mach-o/$f "$XNUDEPS/hostinc/mach-o/"
done
fi
if [ -d BUILD/obj ] && [ "$(cat $STAMP 2>/dev/null)" != "$now" ]; then
	rm -f BUILD/obj/*/mach.*.unstripped
	echo "$now" > $STAMP
fi
exec make HAKIOS_OBJECTS="$LX" SDKROOT=iphoneos ARCH_CONFIGS=ARM64 MACHINE_CONFIGS=${MACHINE:-$(_scion_profile machine BCM2837)} KERNEL_CONFIGS=DEVELOPMENT \
  ARCH_STRING_FOR_CURRENT_MACHINE_CONFIG=${XNU_ARCH_STRING:-arm64} BUILD_WERROR=0 BUILD_LTO=0 DO_CTFMERGE=0 \
  HOST_CC="$HOSTCC" HOST_FLEX=flex HOST_BISON=bison HOST_GM4=m4 CC=xnu-clang CXX=xnu-clang++ LOGCOLORS=n $XNU_MAKE_ARGS "$@"
