/* Copyright The Phorepoc Authors
 * SPDX-License-Identifier: BSD-2-Clause */

/* phe_usb_ure_compat.h -- the FreeBSD types if_urereg.h needs, and the usb_ether / ifnet calls the lifted if_ure.c code
 * (phe_usb_ure_bsd.inc) makes, on phe_usb_api.h. For phe_usb_ure.c and its host test only. */
#ifndef PHE_USB_URE_COMPAT_H
#define PHE_USB_URE_COMPAT_H
#include <stdint.h>
#include <stdbool.h>
#include "phe_usb_api.h"

typedef uint8_t u_int8_t;
typedef unsigned int u_int;
typedef unsigned long u_long;
#define __packed __attribute__((packed))
struct mtx { int unused; };
struct ifmedia { int unused; };
struct usb_xfer { phe_usb_pipe *pipe; };
struct ifnet { int flags, drvflags; unsigned char lladdr[8]; };
typedef struct ifnet *if_t;
struct usb_ether {
	void *ue_dev;
	phe_usb_dev *ue_udev;
	if_t ue_ifp;
	unsigned char ue_eaddr[8];		/* ure_attach_post reads 8 bytes (the PLA register is 8 wide) */
	void *ue_sc;
};
#include "if_urereg.h"

/* MII (IEEE 802.3 clause 22) */
#define MII_BMCR	0
#define MII_BMSR	1
#define MII_ANAR	4
#define MII_100T2CR	9
#define BMCR_AUTOEN	0x1000
#define BMCR_STARTNEG	0x0200
#define BMCR_PDOWN	0x0800
#define ANAR_10_100_ALL	0x01e1		/* 10 / 100, half + full, selector 802.3 */
#define GTCR_ADV_1000	0x0300		/* 1000BASE-T full + half */
#endif
