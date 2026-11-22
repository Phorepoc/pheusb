/* Copyright The Phorepoc Authors
 * SPDX-License-Identifier: BSD-2-Clause */

/* phe_usb_hid.h -- a HID client for the kernel: USB keyboards (boot protocol) and pointers (mice and tablets, from their
 * report descriptor), as plain events for the board to turn into its OS's input. Written from the USB HID 1.11
 * specification (boot protocol, appendix B; the short-item report descriptor, section 6.2.2) and FreeBSD's usb_hid.c usage
 * of it. Everything runs on the USB thread (phe_usb_service()).
 *
 * The client takes every HID interface no other user has: class 3 with protocol 1 (keyboard) or a report descriptor with
 * Generic Desktop X and Y (mouse = relative, tablet / touch = absolute). Keys are HID usages of the Keyboard page (0x07),
 * modifiers included (0xE0-0xE7). */
#ifndef PHE_USB_HID_H
#define PHE_USB_HID_H

struct phe_usb_hid_sink {
	void (*key)(void *ctx, unsigned int usage, int down);
	/* a relative pointer: motion and the button state (bit 0 = left, 1 = right, 2 = middle) */
	void (*rel)(void *ctx, int dx, int dy, unsigned int buttons);
	/* an absolute pointer: the position in [0, max] on each axis and the button state */
	void (*abs)(void *ctx, unsigned int x, unsigned int xmax, unsigned int y, unsigned int ymax, unsigned int buttons);
	void *ctx;
};

/* install the sink (once, before or after the host starts); NULL = stop delivering */
void phe_usb_hid_set_sink(const struct phe_usb_hid_sink *sink);

/* from phe_usb_api.c: a device came or went; and the per-pass service (the HID set-up runs here) */
void phe_usb_hid_notify(void *dev, int attached);
void phe_usb_hid_service(void);

/* the report-descriptor parser, exposed for the host test: the bit positions of X, Y and the first 3 buttons in the input
 * report with id rid (0 = the descriptor has no report ids) */
struct phe_usb_hid_ptr {
	int rid;				/* report id, or 0 */
	int xpos, xsize, ypos, ysize;		/* bit offset and size in the report (after the id byte) */
	int bpos, bcount;			/* buttons: first bit, count (<= 3 used) */
	int wpos, wsize;			/* wheel, or wsize 0 */
	int rel;				/* 1 = relative (mouse), 0 = absolute */
	long xmax, ymax;			/* logical maxima (absolute) */
};
int phe_usb_hid_parse(const unsigned char *desc, unsigned int len, struct phe_usb_hid_ptr *out);	/* 0 = found X and Y */
/* extract an unsigned / sign-extended field */
unsigned int phe_usb_hid_field(const unsigned char *rep, unsigned int len, int pos, int size);
int phe_usb_hid_sfield(const unsigned char *rep, unsigned int len, int pos, int size);

#endif
