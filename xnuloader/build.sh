#!/bin/bash
# Build the XNU loader as a freestanding AArch64 UEFI application.
set -e
cd "$(dirname "$0")"
# CHOOSER=1: the boot chooser for the Galaxy S9 (chooser.c; off, and no code in the loader, otherwise)
# the iPhone's identity for XNU, from the device tree of the builder's own IPSW (not in the repository)
[ -f identity.h ] || ./mkidentity.sh
# the boot screen's drawing, which the loader shares with the kernel: osfmk/console/scion_splash.h of the fork of XNU
# (tools/xnu-tree.py; SCION_SPLASH_DIR names the directory when this script is run from a copy)
SPLASH=${SCION_SPLASH_DIR:-$(python3 ../tools/xnu-tree.py added ios-16.5)/osfmk/console}

${CLANG:-clang} --target=aarch64-unknown-windows -ffreestanding -fno-builtin -fno-stack-protector -fshort-wchar \
  -mgeneral-regs-only -DCHOOSER=${CHOOSER:-0} -O0 -Wall -Wextra -Wno-unused-parameter -I"$SPLASH" -c loader.c -o loader.obj
# /base: UEFI loads an image at its preferred address when that is free. The PE default (0x140000000)
# is in the middle of the phone's largest block of RAM, which XNU needs in one piece.
${LLD_LINK:-lld-link} /subsystem:efi_application /entry:efi_main /nodefaultlib /base:0xA1000000 /out:loader.efi loader.obj
ls -la loader.efi
