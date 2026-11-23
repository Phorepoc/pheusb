# pheusb

The USB host stack of Phorepoc OS. It is FreeBSD's USB stack (the core, the xHCI, EHCI and OHCI controller drivers, and
the serial drivers), imported unmodified and run inside the EKA2 kernel, behind the USB host interfaces of the 2010
Symbian source: the hub driver and USBDI logical devices that the user-side Function Driver Framework (FDF) opens.

Boards use it as a layer: they find their controller (on a PC, by PCI class), map it, and hand it to the stack. Nothing
here names a board or a chip.

## What's in here

```
os/third_party/freebsd-usb/    FreeBSD's USB stack at commit 043d6753ca7c, unmodified (check.sh verifies it against SHA256SUMS)
os/src/usbhost/                our side, BSD-2-Clause:
  phe_usb_port.h                 the seam between the stack and the kernel (memory, time, locks, barriers)
  phe_usb_hc.c                   the generic root bus and the controller attach (xHCI / EHCI / OHCI)
  phe_usb_api.{h,c}              the stack as a plain C service: devices as attach/detach events, control transfers, pipes
  phe_usb_hid.{h,c}              a HID client: boot-protocol keyboards, mice, tablets / touch screens, XInput gamepads
  ekern/                         the USB thread, its 1 ms tick and memory pool; the libc the stack needs
  ldd/                           the USBHUBDRIVER and USBDI logical devices of the 2010 user side
  build_bsd.sh                   builds the C side into one relocatable object (ARCH=x32, x64 or arm)
```

At build time `os/` is laid over [core](https://github.com/Phorepoc/core)'s `os/`, so the paths line up with core's.

## Building

You don't build this repository by itself. A board that lists it in its `board.conf` builds it with its kernel: the
board's kernel script runs `os/src/usbhost/build_bsd.sh` with its architecture and the controllers it has, and links
the object into the kernel image.

## License

This repository is under FreeBSD's licence (BSD-2-Clause, see `LICENSE`), as port work on FreeBSD's USB stack. FreeBSD's files keep their own headers (BSD-2-Clause, and BSD-3-Clause for a few). `NOTICE` names the copyright holders, and the licence texts are in `LICENSES/`.
Files adapted from other projects keep their authors' copyright (see `AUTHORS` and `NOTICE`). Contributions are under the same terms. Add your name to `AUTHORS` in your first pull request.
