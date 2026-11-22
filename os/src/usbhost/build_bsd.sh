#!/usr/bin/env bash
# Copyright The Phorepoc Authors
# SPDX-License-Identifier: BSD-2-Clause
#
# build_bsd.sh <out-dir> -- the C side of the USB host: the unmodified FreeBSD USB stack on its kshim, our phe_usb_hc.c,
# phe_usb_api.c and phe_usb_libc.c, compiled for the board's kernel and collected into ONE relocatable object,
# <out-dir>/usbhost_bsd.o, with the kshim's SYSINIT tables generated as FreeBSD's stand/kshim/kshim.mk does.
# usbd_get_page is wrapped inside the object (phe_usb_hc.c).
# Env: ARCH=x32|x64|arm (default x32; x64 = LP64 amd64); HC="xhci ehci ohci" (the controller drivers to build); SERIAL=1 (ucom + uftdi + u3g on
#      phe_usb_tty.c, which the board provides: PHE_USB_TTY); FB (the import, default ../../third_party/freebsd-usb);
#      EXTRA_SRCS (board C files, e.g. a PHY provider); GCC, HOSTCC, USBHOST_DEFS.
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"
O="$1"; mkdir -p "$O"
FB="${FB:-$HERE/../../third_party/freebsd-usb}"
ARCHN="${ARCH:-x32}"
HC="${HC:-xhci}"
HOSTCC="${HOSTCC:-cc}"
U="$FB/sys/dev/usb"
case "$ARCHN" in
  x32) GCC="${GCC:-gcc}"; LD="${LD:-ld}"; OBJCOPY="${OBJCOPY:-objcopy}"; LDM="-m elf32_x86_64"
       ARCHF=( -mx32 -mno-red-zone -mgeneral-regs-only -ffreestanding -fno-strict-aliasing -fno-stack-protector
         -fno-tree-loop-distribute-patterns -O2 -fno-pic -fno-pie -fno-common ) ;;
  x64) GCC="${GCC:-gcc}"; LD="${LD:-ld}"; OBJCOPY="${OBJCOPY:-objcopy}"; LDM="-m elf_x86_64"
       # LP64 (-m64): the kernel's code model is large (its image and heaps live above 4 GiB)
       ARCHF=( -m64 -mcmodel=large -mno-red-zone -mgeneral-regs-only -ffreestanding -fno-strict-aliasing -fno-stack-protector
         -fno-tree-loop-distribute-patterns -O2 -fno-pic -fno-pie -fno-common ) ;;
  arm) GCC="${GCC:-arm-none-eabi-gcc}"; LD="${LD:-${GCC%gcc}ld}"; OBJCOPY="${OBJCOPY:-${GCC%gcc}objcopy}"; LDM=""
       ARCHF=( -march=armv7-a -marm -mfloat-abi=soft -fno-short-enums -mno-unaligned-access -ffreestanding
         -fno-strict-aliasing -O2 -fno-pic -fno-common ) ;;
  *) echo "ARCH=$ARCHN?" >&2; exit 1 ;;
esac
KSRCS=(
  "$FB/stand/kshim/bsd_kernel.c" "$FB/stand/kshim/bsd_busspace.c" "$U/controller/usb_controller.c"
  "$U/usb_core.c" "$U/usb_debug.c" "$U/usb_device.c" "$U/usb_dynamic.c" "$U/usb_error.c"
  "$U/usb_handle_request.c" "$U/usb_hid.c" "$U/usb_hub.c" "$U/usb_lookup.c" "$U/usb_msctest.c"
  "$U/usb_parse.c" "$U/usb_request.c" "$U/usb_transfer.c" "$U/usb_util.c"
  "$HERE/phe_usb_hc.c" "$HERE/phe_usb_api.c" "$HERE/phe_usb_hid.c"
)
HCDEFS=()
for h in $HC; do
  case "$h" in
    xhci) KSRCS+=( "$U/controller/xhci.c" ); HCDEFS+=( -DPHE_USB_XHCI ) ;;
    ehci) KSRCS+=( "$U/controller/ehci.c" "$U/controller/generic_ehci.c" ); HCDEFS+=( -DPHE_USB_EHCI ) ;;
    ohci) KSRCS+=( "$U/controller/ohci.c" ); HCDEFS+=( -DPHE_USB_OHCI ) ;;
  esac
done
GEN=()
if [ "${SERIAL:-0}" = 1 ]; then
  mkdir -p "$O/gen"
  ( cd "$O/gen" && awk -f "$FB/sys/tools/usbdevs2h.awk" "$U/usbdevs" -h )
  GEN=( -I"$O/gen" )
  KSRCS+=( "$U/serial/usb_serial.c" "$U/serial/uftdi.c" "$U/serial/u3g.c" )
fi
KSRCS+=( ${EXTRA_SRCS:-} )
SRCS=( "$FB/stand/usb/usb_busdma_loader.c" )
CFLAGS=( "${ARCHF[@]}" -nostdinc -isystem "$("$GCC" -print-file-name=include)" -D_STANDALONE -DHAVE_MALLOC
  -Dprintf=phe_usb_printf -Dsnprintf=phe_usb_snprintf
  -Wall -Wno-unused -Wno-pointer-sign -Wno-unused-but-set-variable
  -I"$FB/stand/kshim" -I"$FB/sys" -I"$HERE" -I"$HERE/kinc" ${PHE_USB_INC:-}
  -D'DECLARE_CLASS(x)=' -D'MPASS(x)=((void)0)'
  -DUSB_GLOBAL_INCLUDE_FILE='"bsd_global.h"' -D'__uintptr_t=unsigned long' -DUSB_MSCTEST_BULK_SIZE=65536
  "${HCDEFS[@]}" "${GEN[@]}" ${USBHOST_DEFS:-} )
objs=(); sysobjs=()
for s in "${KSRCS[@]}" "${SRCS[@]}"; do
  o="$O/$(basename "${s%.c}").o"; "$GCC" "${CFLAGS[@]}" -c "$s" -o "$o"; objs+=("$o")
  # UCOM_UNLOAD_DRAIN names its record sysuninit_var in every driver; the records are the same: weak, so the copies merge
  case "$s" in */serial/*) "$OBJCOPY" --weaken-symbol=sysuninit_var "$o" ;; esac
done
for s in "${KSRCS[@]}"; do
  o="$O/$(basename "${s%.c}").o"; "$OBJCOPY" -j .debug.sysinit -O binary "$o" "${o%.o}.osys"; sysobjs+=("${o%.o}.osys")
done
cat "${sysobjs[@]}" > "$O/sysinit.bin"
[ -x "$O/sysinit" ] || "$HOSTCC" -O2 -I"$FB/stand/kshim" -o "$O/sysinit" "$FB/stand/usb/tools/sysinit.c"
"$O/sysinit" -i "$O/sysinit.bin" -o "$O/sysinit_data.c" -k sysinit -s sysinit_data
"$O/sysinit" -i "$O/sysinit.bin" -o "$O/sysuninit_data.c" -R -k sysuninit -s sysuninit_data
for s in "$O/sysinit_data.c" "$O/sysuninit_data.c"; do
  o="${s%.c}.o"; "$GCC" "${CFLAGS[@]}" -c "$s" -o "$o"; objs+=("$o")
done
"$GCC" "${ARCHF[@]}" -Wall -c "$HERE/ekern/phe_usb_libc.c" -o "$O/phe_usb_libc.o"; objs+=("$O/phe_usb_libc.o")
"$LD" $LDM -r --wrap=usbd_get_page -o "$O/usbhost_bsd.o" "${objs[@]}"
"$OBJCOPY" --remove-section=.debug.sysinit "$O/usbhost_bsd.o"
echo "usbhost: $O/usbhost_bsd.o"
