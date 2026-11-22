/* Copyright The Phorepoc Authors
 * SPDX-License-Identifier: BSD-2-Clause */

/* phe_usb_ure.h -- RealTek RTL8152 / RTL8153 / RTL8153B USB Ethernet adapters for the kernel: FreeBSD's if_ure chip code
 * (lifted verbatim by gen_ure.py) on the plain phe_usb_api service, raw Ethernet frames in and out. No ifnet: the board's
 * client (a debug link, later an Ethernet LDD) gets whole frames. Everything runs on the USB thread (phe_usb_service()).
 * Built with URE=1 (build_bsd.sh, -DPHE_USB_URE). */
#ifndef PHE_USB_URE_H
#define PHE_USB_URE_H

struct phe_usb_net_sink {
	/* the adapter is up (initialised, MAC known) or gone; link = the PHY reports a link */
	void (*state)(void *ctx, int present, int link, const unsigned char mac[6]);
	/* one received frame (destination MAC first, no FCS) */
	void (*rx)(void *ctx, const unsigned char *frame, unsigned int len);
	/* every service pass while the adapter is up (~1 ms): the client's chance to send */
	void (*poll)(void *ctx);
	void *ctx;
};
void phe_usb_ure_set_sink(const struct phe_usb_net_sink *sink);

/* Queue one frame (60..1514 bytes; shorter ones are padded). 0 = queued, -1 = no adapter / no link / the queue is full.
 * USB thread only. */
int phe_usb_ure_tx(const unsigned char *frame, unsigned int len);

/* from phe_usb_api.c */
void phe_usb_ure_notify(void *dev, int attached);
void phe_usb_ure_service(void);

/* exposed for the host test: one bulk-IN buffer of the chip's RX aggregation format (struct ure_rxpkt + frame + FCS, each
 * 8-aligned) -> rx() per frame; returns the frame count, or -1 at the first malformed header (the rest is dropped, as
 * FreeBSD does). And the TX framing of one frame into out (needs len + 8 + 7 bytes); returns the bytes to send. */
int phe_usb_ure_rx_parse(const unsigned char *buf, unsigned int actual);
unsigned int phe_usb_ure_tx_frame(unsigned char *out, const unsigned char *frame, unsigned int len);

#endif
