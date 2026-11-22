/* Copyright The Phorepoc Authors
 * SPDX-License-Identifier: BSD-2-Clause */

/* phe_usb_api.h -- the stack as a plain C service for the kernel side (the USBDI / hub driver LDDs, ldd/usbhost_ldd.cpp),
 * without FreeBSD's types. Implemented in phe_usb_api.c. Every call except phe_usb_set_notify, phe_usb_set_work_hook and
 * phe_usb_dev_info runs on the USB thread only (the thread that calls phe_usb_service); the LDDs queue work to it. */
#ifndef PHE_USB_API_H
#define PHE_USB_API_H

typedef struct phe_usb_dev phe_usb_dev;		/* a FreeBSD usb_device */
typedef struct phe_usb_pipe phe_usb_pipe;	/* one endpoint's transfer */

struct phe_usb_devinfo {
	unsigned int bus;		/* usbus unit */
	unsigned int addr;		/* USB address */
	unsigned int speed;		/* 0 low, 1 full, 2 high (SuperSpeed counts as high) */
	unsigned short vid, pid;
	const unsigned char *ddesc;	/* the 18-byte device descriptor */
	const unsigned char *cdesc;	/* the active configuration descriptor (all interfaces) */
	unsigned int cdesclen;
	unsigned int superspeed;	/* 1 = SuperSpeed or faster (speed then still says 2) */
};

/* A device is reported attached once it is configured and its descriptors are known, removed when the stack drops it.
 * Hubs are the stack's own business and are not reported. Called on the USB thread from phe_usb_service(). */
typedef void (*phe_usb_notify_fn)(void *ctx, phe_usb_dev *dev, int attached);
void phe_usb_set_notify(phe_usb_notify_fn fn, void *ctx);
/* Called on the USB thread at the end of every service pass (the LDDs run their queued work there). */
void phe_usb_set_work_hook(void (*fn)(void));

int phe_usb_dev_info(phe_usb_dev *dev, struct phe_usb_devinfo *info);	/* 0 = ok */

/* A control transfer on endpoint 0, blocking. Returns 0, or a FreeBSD usb_error_t (the PHE_USB_ERR_* below). */
#define PHE_USB_ERR_STALLED 4
#define PHE_USB_ERR_IOERROR 7
#define PHE_USB_ERR_TIMEOUT 19
int phe_usb_ctrl(phe_usb_dev *dev, unsigned char rtype, unsigned char req, unsigned short value, unsigned short index,
    void *buf, unsigned short len, unsigned int *actual);
int phe_usb_set_alt(phe_usb_dev *dev, int iface_number, int alt);

/* A bulk or interrupt pipe of the interface with bInterfaceNumber iface (its current alternate setting), endpoint address
 * ep; transfers up to bufsize bytes. NULL if it is not there. */
phe_usb_pipe *phe_usb_pipe_open(phe_usb_dev *dev, int iface, unsigned int ep, unsigned int bufsize);
void phe_usb_pipe_close(phe_usb_pipe *p);
/* Start one transfer of len bytes (OUT: from buf; IN: into buf). done() runs on the USB thread with the FreeBSD error (0 =
 * ok) and the byte count. One transfer per pipe at a time. zlp: end an OUT transfer with a zero-length packet. */
typedef void (*phe_usb_done_fn)(void *ctx, int err, unsigned int actual);
int phe_usb_pipe_start(phe_usb_pipe *p, void *buf, unsigned int len, int zlp, phe_usb_done_fn done, void *ctx);
void phe_usb_pipe_abort(phe_usb_pipe *p);	/* done() is called with an error if a transfer was running */
int phe_usb_pipe_clear_stall(phe_usb_pipe *p);

#endif
