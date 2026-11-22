/* Copyright The Phorepoc Authors
 * SPDX-License-Identifier: BSD-2-Clause */

/* phe_usb_api.c -- phe_usb_api.h on FreeBSD's stack (the kshim, unmodified): the device list as attach / detach events,
 * control transfers and bulk / interrupt pipes. Everything runs on the USB thread. */
#include <bsd_global.h>

#include "phe_usb_port.h"
#include "phe_usb_api.h"
#include "phe_usb_hid.h"
#ifdef PHE_USB_URE
#include "phe_usb_ure.h"
#endif

#define PHE_USB_MAX_DEV 32

static phe_usb_notify_fn notify_fn;
static void *notify_ctx;
static void (*work_hook)(void);
static struct usb_device *known[PHE_USB_MAX_DEV];
static struct mtx pipe_mtx;
static int pipe_mtx_ready;

void
phe_usb_set_notify(phe_usb_notify_fn fn, void *ctx)
{
	notify_ctx = ctx;
	notify_fn = fn;
}

void
phe_usb_set_work_hook(void (*fn)(void))
{
	work_hook = fn;
}

static int
is_listed(struct usb_device *u)
{
	return (u->parent_hub != NULL && u->ddesc.bDeviceClass != UDCLASS_HUB && u->state == USB_STATE_CONFIGURED &&
	    u->cdesc != NULL);
}

static void
scan_devices(void)
{
	devclass_t dc = devclass_find("usbus");
	struct usb_device *now[PHE_USB_MAX_DEV];
	int n = 0, i, j, unit;

	if (dc == NULL)
		return;
	for (unit = 0; unit < DEVCLASS_MAXUNIT; unit++) {
		device_t bdev = devclass_get_device(dc, unit);
		struct usb_bus *bus = bdev ? device_get_ivars(bdev) : NULL;
		if (bus == NULL)
			continue;
		for (i = 0; i < (int)bus->devices_max && n < PHE_USB_MAX_DEV; i++) {
			struct usb_device *u = bus->devices[i];
			if (u != NULL && is_listed(u))
				now[n++] = u;
		}
	}
	/* gone first, so a reused address is a removal followed by an attachment */
	for (i = 0; i < PHE_USB_MAX_DEV; i++) {
		struct usb_device *k = known[i];
		if (k == NULL)
			continue;
		for (j = 0; j < n && now[j] != k; j++)
			;
		if (j == n) {
			known[i] = NULL;
			phe_usb_hid_notify(k, 0);
#ifdef PHE_USB_URE
			phe_usb_ure_notify(k, 0);
#endif
			if (notify_fn)
				notify_fn(notify_ctx, (phe_usb_dev *)k, 0);
		}
	}
	for (j = 0; j < n; j++) {
		for (i = 0; i < PHE_USB_MAX_DEV && known[i] != now[j]; i++)
			;
		if (i < PHE_USB_MAX_DEV)
			continue;
		for (i = 0; i < PHE_USB_MAX_DEV && known[i] != NULL; i++)
			;
		if (i == PHE_USB_MAX_DEV)
			continue;
		known[i] = now[j];
		phe_usb_hid_notify(now[j], 1);
#ifdef PHE_USB_URE
		phe_usb_ure_notify(now[j], 1);
#endif
		if (notify_fn)
			notify_fn(notify_ctx, (phe_usb_dev *)now[j], 1);
	}
}

/* called by phe_usb_service() after the stack has run */
void
phe_usb_api_service(void)
{
	scan_devices();
	phe_usb_hid_service();
#ifdef PHE_USB_URE
	phe_usb_ure_service();
#endif
	if (work_hook)
		work_hook();
}

int
phe_usb_dev_info(phe_usb_dev *dev, struct phe_usb_devinfo *info)
{
	struct usb_device *u = (struct usb_device *)dev;

	info->bus = device_get_unit(u->bus->bdev);
	info->addr = u->address;
	info->speed = (u->speed <= USB_SPEED_LOW) ? 0 : (u->speed == USB_SPEED_FULL) ? 1 : 2;
	info->vid = UGETW(u->ddesc.idVendor);
	info->pid = UGETW(u->ddesc.idProduct);
	info->ddesc = (const unsigned char *)&u->ddesc;
	info->cdesc = (const unsigned char *)u->cdesc;
	info->cdesclen = u->cdesc ? UGETW(u->cdesc->wTotalLength) : 0;
	info->superspeed = u->speed >= USB_SPEED_SUPER;
	return (0);
}

int
phe_usb_ctrl(phe_usb_dev *dev, unsigned char rtype, unsigned char req, unsigned short value, unsigned short index,
    void *buf, unsigned short len, unsigned int *actual)
{
	struct usb_device_request r;
	uint16_t act = 0;
	usb_error_t e;

	r.bmRequestType = rtype;
	r.bRequest = req;
	USETW(r.wValue, value);
	USETW(r.wIndex, index);
	USETW(r.wLength, len);
	e = usbd_do_request_flags((struct usb_device *)dev, NULL, &r, buf, (rtype & UT_READ) ? USB_SHORT_XFER_OK : 0, &act,
	    1000);
	if (actual)
		*actual = act;
	return ((int)e);
}

static int
iface_index(struct usb_device *u, int iface_number)
{
	int i;

	for (i = 0; i < 32; i++) {
		struct usb_interface *ifc = usbd_get_iface(u, i);
		if (ifc != NULL && ifc->idesc != NULL && ifc->idesc->bInterfaceNumber == iface_number)
			return (i);
	}
	return (-1);
}

int
phe_usb_set_alt(phe_usb_dev *dev, int iface_number, int alt)
{
	struct usb_device *u = (struct usb_device *)dev;
	int i = iface_index(u, iface_number);

	return (i < 0 ? USB_ERR_INVAL : (int)usbd_set_alt_interface_index(u, i, alt));
}

/*------------------------------------------------------------------------*
 * pipes: one usb_xfer per endpoint, driven by the usual three-state callback
 *------------------------------------------------------------------------*/

struct phe_usb_pipe {
	struct usb_xfer *xfer;
	struct usb_device *udev;
	unsigned int ep;
	int in;
	void *buf;
	unsigned int len;
	int zlp;
	int busy;
	phe_usb_done_fn done;
	void *ctx;
};

static void
pipe_finish(struct phe_usb_pipe *p, int err, unsigned int actual)
{
	phe_usb_done_fn done = p->done;

	p->busy = 0;
	if (done)
		done(p->ctx, err, actual);
}

static void
pipe_callback(struct usb_xfer *xfer, usb_error_t error)
{
	struct phe_usb_pipe *p = usbd_xfer_softc(xfer);	/* priv_sc (usbd_xfer_get_priv is the fifo pointer) */
	int actlen;

	switch (USB_GET_STATE(xfer)) {
	case USB_ST_TRANSFERRED:
		usbd_xfer_status(xfer, &actlen, NULL, NULL, NULL);
		if (p->in && actlen > 0)
			usbd_copy_out(usbd_xfer_get_frame(xfer, 0), 0, p->buf, actlen);
		if (p->busy)
			pipe_finish(p, 0, (unsigned int)actlen);
		return;
	case USB_ST_SETUP:
		if (!p->busy)
			return;
		if (!p->in)
			usbd_copy_in(usbd_xfer_get_frame(xfer, 0), 0, p->buf, p->len);
		usbd_xfer_set_frame_len(xfer, 0, p->len);
		if (!p->in && p->zlp)
			usbd_xfer_set_zlp(xfer);
		usbd_transfer_submit(xfer);
		return;
	default:
		if (p->busy)
			pipe_finish(p, error ? (int)error : USB_ERR_CANCELLED, 0);
		return;
	}
}

/* the endpoint type of address ep in the current alternate setting of interface index ifidx (0xFF = none) */
static uint8_t
ep_type(struct usb_device *u, int ifidx, unsigned int ep)
{
	struct usb_interface *ifc = usbd_get_iface(u, ifidx);
	struct usb_descriptor *desc = NULL;
	int in_iface = 0;

	while ((desc = usb_desc_foreach(u->cdesc, desc)) != NULL) {
		if (desc->bDescriptorType == UDESC_INTERFACE) {
			struct usb_interface_descriptor *id = (void *)desc;
			in_iface = (id->bInterfaceNumber == ifc->idesc->bInterfaceNumber &&
			    id->bAlternateSetting == ifc->alt_index);
		} else if (in_iface && desc->bDescriptorType == UDESC_ENDPOINT) {
			struct usb_endpoint_descriptor *ed = (void *)desc;
			if (ed->bEndpointAddress == (uint8_t)ep)
				return (ed->bmAttributes & UE_XFERTYPE);
		}
	}
	return (0xFF);
}

phe_usb_pipe *
phe_usb_pipe_open(phe_usb_dev *dev, int iface, unsigned int ep, unsigned int bufsize)
{
	struct usb_device *u = (struct usb_device *)dev;
	struct usb_config cfg;
	struct phe_usb_pipe *p;
	uint8_t ifs[1], type;
	int idx = iface_index(u, iface);

	if (idx < 0)
		return (NULL);
	type = ep_type(u, idx, ep);
	if (type != UE_BULK && type != UE_INTERRUPT)
		return (NULL);
	if (!pipe_mtx_ready) {
		mtx_init(&pipe_mtx, "phe_usb_pipe", NULL, MTX_DEF);
		pipe_mtx_ready = 1;
	}
	p = malloc(sizeof(*p), 0, 0);
	if (p == NULL)
		return (NULL);
	memset(p, 0, sizeof(*p));
	memset(&cfg, 0, sizeof(cfg));
	cfg.type = type;
	cfg.endpoint = ep & 0x0F;
	cfg.direction = (ep & 0x80) ? UE_DIR_IN : UE_DIR_OUT;
	cfg.bufsize = bufsize;
	cfg.callback = &pipe_callback;
	cfg.frames = 1;
	cfg.flags.pipe_bof = 1;
	cfg.flags.short_xfer_ok = 1;
	p->in = (ep & 0x80) != 0;
	p->udev = u;
	p->ep = ep;
	ifs[0] = idx;
	if (usbd_transfer_setup(u, ifs, &p->xfer, &cfg, 1, p, &pipe_mtx) != 0) {
		free(p, 0);
		return (NULL);
	}
	return (p);
}

void
phe_usb_pipe_close(phe_usb_pipe *p)
{
	p->done = NULL;
	mtx_lock(&pipe_mtx);
	usbd_transfer_stop(p->xfer);
	mtx_unlock(&pipe_mtx);
	usbd_transfer_unsetup(&p->xfer, 1);
	free(p, 0);
}

int
phe_usb_pipe_start(phe_usb_pipe *p, void *buf, unsigned int len, int zlp, phe_usb_done_fn done, void *ctx)
{
	if (p->busy)
		return (-1);
	p->buf = buf;
	p->len = len;
	p->zlp = zlp;
	p->done = done;
	p->ctx = ctx;
	p->busy = 1;
	mtx_lock(&pipe_mtx);
	usbd_transfer_start(p->xfer);
	mtx_unlock(&pipe_mtx);
	return (0);
}

void
phe_usb_pipe_abort(phe_usb_pipe *p)
{
	mtx_lock(&pipe_mtx);
	usbd_transfer_stop(p->xfer);
	mtx_unlock(&pipe_mtx);
	if (p->busy)
		pipe_finish(p, USB_ERR_CANCELLED, 0);
}

int
phe_usb_pipe_clear_stall(phe_usb_pipe *p)
{
	struct usb_endpoint *e = usbd_get_ep_by_addr(p->udev, (uint8_t)p->ep);
	int r = phe_usb_ctrl((phe_usb_dev *)p->udev, UT_WRITE_ENDPOINT, UR_CLEAR_FEATURE, UF_ENDPOINT_HALT, p->ep, NULL, 0,
	    NULL);

	if (e != NULL)
		usbd_clear_data_toggle(p->udev, e);
	return (r);
}
