/* Copyright The Phorepoc Authors
 * SPDX-License-Identifier: BSD-2-Clause */

/* phe_usb_hid.c -- see phe_usb_hid.h. Only phe_usb_api.h is used (no FreeBSD types), so the parser also builds on a host. */
#include "phe_usb_api.h"
#include "phe_usb_hid.h"

#ifndef PHE_USB_HID_HOST
int phe_usb_printf(const char *, ...);
#define HLOG(...) phe_usb_printf(__VA_ARGS__)
#else
#define HLOG(...) ((void)0)
#endif

#define HID_MAX 8
#define REP_MAX 64
static const struct phe_usb_hid_sink *sink;

enum { K_KBD = 1, K_PTR = 2, K_XPAD = 3 };
#define PROTO_XPAD 0x100

/* ---- the XInput gamepad (the wired Xbox 360 controller protocol, as built into handhelds): a 20-byte input report,
 * type 0x00 / length 0x14; bytes 2-3 the buttons, 4-5 the triggers, 6-13 the sticks (signed 16-bit, little endian).
 * Written from the protocol's public descriptions (the report layout is not a HID descriptor). Buttons become keys of
 * the Keyboard page, so a board maps them like a keyboard's; the left stick and the right trigger drive the pointer. */
static const unsigned short xpad_keys[16] = {
	0x52, 0x51, 0x50, 0x4F,		/* d-pad up, down, left, right -> the arrows */
	0x43, 0x29,			/* start -> F10 (a PC board's Menu key), back -> Escape */
	0, 0,				/* the stick clicks */
	0x81, 0x80,			/* LB, RB -> volume down, up */
	0x43, 0,			/* guide -> F10, - */
	0x3F, 0x40, 0x3E, 0x28		/* A -> F6 (OK), B -> F7 (right softkey), X -> F5 (left softkey), Y -> Enter */
};

static int xpad_dx, xpad_dy;
static unsigned int xpad_btn;	/* the stick's current motion per step, repeated by phe_usb_hid_xpad_tick() */

void
phe_usb_hid_xpad_tick(void)
{
	if ((xpad_dx || xpad_dy) && sink && sink->rel)
		sink->rel(sink->ctx, xpad_dx, xpad_dy, xpad_btn);
}

void
phe_usb_hid_xpad(const unsigned char *r, unsigned int n, unsigned int *last)
{
	unsigned int b, ch, i, prev = *last & 0xFFFF;
	int lx, ly, dx, dy;

	if (n < 14 || r[0] != 0x00 || r[1] < 0x14 || !sink)
		return;
	b = r[2] | (r[3] << 8);
	ch = b ^ prev;
	for (i = 0; i < 16; i++)
		if ((ch & (1u << i)) && xpad_keys[i] && sink->key)
			sink->key(sink->ctx, xpad_keys[i], (b >> i) & 1);
	/* the pointer: the left stick past a dead zone, quadratic speed; RT = button 1, LT = button 2 */
	lx = (short)(r[6] | (r[7] << 8));
	ly = (short)(r[8] | (r[9] << 8));
	dx = (lx > 6000 || lx < -6000) ? (int)((long)lx * (lx < 0 ? -lx : lx) / (32768L * 4096)) : 0;
	dy = (ly > 6000 || ly < -6000) ? -(int)((long)ly * (ly < 0 ? -ly : ly) / (32768L * 4096)) : 0;
	{
		unsigned int btn = (r[5] > 64 ? 1u : 0) | (r[4] > 64 ? 4u : 0);
		if ((dx || dy || btn != (*last >> 16)) && sink->rel)
			sink->rel(sink->ctx, dx, dy, btn);
		*last = b | (btn << 16);
		xpad_dx = dx;
		xpad_dy = dy;
		xpad_btn = btn;
	}
}

struct hid {
	phe_usb_dev *dev;
	int kind, iface, ep, mps, ready, failed;
	phe_usb_pipe *pipe;
	struct phe_usb_hid_ptr ptr;
	unsigned char rep[REP_MAX], last[8];
	unsigned int lastbuttons;
};

static struct hid hids[HID_MAX];

void
phe_usb_hid_set_sink(const struct phe_usb_hid_sink *s)
{
	sink = s;
}

/* ---- the report descriptor (HID 1.11 section 6.2.2: short items) ---- */

unsigned int
phe_usb_hid_field(const unsigned char *rep, unsigned int len, int pos, int size)
{
	unsigned int v = 0;
	int i;

	for (i = 0; i < size && i < 32; i++) {
		int b = pos + i;
		if ((unsigned int)(b >> 3) < len && (rep[b >> 3] >> (b & 7)) & 1)
			v |= 1u << i;
	}
	return (v);
}

int
phe_usb_hid_sfield(const unsigned char *rep, unsigned int len, int pos, int size)
{
	unsigned int v = phe_usb_hid_field(rep, len, pos, size);

	if (size > 0 && size < 32 && (v & (1u << (size - 1))))
		v |= ~0u << size;
	return ((int)v);
}

int
phe_usb_hid_parse(const unsigned char *d, unsigned int len, struct phe_usb_hid_ptr *o)
{
	unsigned int i = 0, page = 0, rsize = 0, rcount = 0, umin = 0, umax = 0, nu = 0, rid = 0;
	unsigned int usages[16];
	long lmax = 0;
	int bit = 0, have_x = 0, have_y = 0, done_rid = -1;
	struct phe_usb_hid_ptr p = { 0 };

	p.bpos = -1;
	while (i < len) {
		unsigned int b = d[i], sz = b & 3, tag = b & 0xFC, v = 0, k;
		if (b == 0xFE) {			/* long item: skip */
			if (i + 2 >= len)
				break;
			i += 3 + d[i + 1];
			continue;
		}
		if (sz == 3)
			sz = 4;
		if (i + 1 + sz > len)
			break;
		for (k = 0; k < sz; k++)
			v |= (unsigned int)d[i + 1 + k] << (8 * k);
		i += 1 + sz;
		switch (tag) {
		case 0x04: page = v; break;					/* usage page */
		case 0x74: rsize = v; break;					/* report size */
		case 0x94: rcount = v; break;					/* report count */
		case 0x24: lmax = (sz == 1) ? (long)(signed char)v : (sz == 2) ? (long)(short)v : (long)(int)v; break;
		case 0x84:							/* report id */
			if (have_x && have_y)
				goto out;					/* the pointer's report ends here */
			rid = v; bit = 0; p.bpos = -1;
			break;
		case 0x08:							/* usage */
			if (nu < 16)
				usages[nu++] = (sz == 4) ? v : (page << 16) | v;
			break;
		case 0x18: umin = (sz == 4) ? v : (page << 16) | v; break;	/* usage minimum */
		case 0x28: umax = (sz == 4) ? v : (page << 16) | v; break;	/* usage maximum */
		case 0x80:							/* input */
			for (k = 0; k < rcount; k++) {
				unsigned int u = 0;
				if (nu)
					u = usages[k < nu ? k : nu - 1];
				else if (umax >= umin && umin)
					u = umin + k;
				if (!(v & 1)) {					/* data, not constant */
					if (u == 0x10030 && !have_x) {
						p.xpos = bit; p.xsize = rsize; p.rel = (v & 4) != 0; p.xmax = lmax; have_x = 1;
						p.rid = rid;
					} else if (u == 0x10031 && !have_y) {
						p.ypos = bit; p.ysize = rsize; p.ymax = lmax; have_y = 1;
					} else if (u == 0x10038 && !p.wsize) {
						p.wpos = bit; p.wsize = rsize;
					} else if (((u >> 16) == 9 || u == 0xD0042) && p.bpos < 0) {	/* buttons, or a digitizer tip switch */
						p.bpos = bit; p.bcount = (int)rcount;
					}
				}
				bit += rsize;
			}
			(void)done_rid;
			nu = 0; umin = umax = 0;
			break;
		case 0x90: case 0xB0:						/* output, feature: not in the input report */
			nu = 0; umin = umax = 0;
			break;
		case 0xA0: case 0xC0:						/* collection, end collection */
			nu = 0; umin = umax = 0;
			break;
		default:
			break;
		}
	}
out:
	if (!have_x || !have_y)
		return (-1);
	if (p.bpos < 0) {
		p.bpos = 0; p.bcount = 0;
	}
	*o = p;
	return (0);
}

/* ---- the client ---- */

/* the nth HID (or XInput) interface of the configuration descriptor with an interrupt-IN endpoint: a composite device
 * (a laptop's keyboard + touchpad) has one per function */
static int
find_iface(const unsigned char *c, unsigned int len, int nth, int *iface, int *proto, int *ep, int *mps)
{
	unsigned int i = 0;
	int cur = -1, cproto = 0;

	while (i + 2 <= len && c[i] >= 2) {
		const unsigned char *d = c + i;
		if (d[1] == 4 && d[0] >= 9) {					/* interface */
			cur = (d[5] == 3 && d[3] == 0) ? d[2] : -1;		/* class HID, alternate setting 0 */
			cproto = d[7];
			if (d[5] == 0xFF && d[6] == 0x5D && d[7] == 0x01 && d[3] == 0) {	/* an XInput gamepad (vendor class 5D/01) */
				cur = d[2];
				cproto = PROTO_XPAD;
			}
		} else if (d[1] == 5 && d[0] >= 7 && cur >= 0 && (d[2] & 0x80) && (d[3] & 3) == 3) {
			if (nth-- == 0) {
				*iface = cur; *proto = cproto; *ep = d[2]; *mps = d[4] | (d[5] << 8);
				return (1);
			}
			cur = -1;						/* one endpoint per interface */
		}
		i += d[0];
	}
	return (0);
}

void
phe_usb_hid_notify(void *dev, int attached)
{
	struct phe_usb_devinfo info;
	int i, n;

	if (!attached) {
		for (i = 0; i < HID_MAX; i++)
			if (hids[i].dev == dev) {
				if (hids[i].pipe)
					phe_usb_pipe_close(hids[i].pipe);
				hids[i].dev = 0;
				hids[i].pipe = 0;
			}
		return;
	}
	for (i = 0; i < HID_MAX; i++)
		if (hids[i].dev == dev)
			return;							/* known already */
	if (phe_usb_dev_info((phe_usb_dev *)dev, &info) != 0)
		return;
	for (n = 0;; n++) {
		struct hid t;
		unsigned char *raw = (unsigned char *)&t;
		unsigned int k;
		int proto = 0;

		for (k = 0; k < sizeof(t); k++)
			raw[k] = 0;
		if (!find_iface(info.cdesc, info.cdesclen, n, &t.iface, &proto, &t.ep, &t.mps))
			break;
		for (i = 0; i < HID_MAX && hids[i].dev; i++)
			;
		if (i == HID_MAX)
			break;
		t.dev = (phe_usb_dev *)dev;
		t.kind = (proto == 1) ? K_KBD : (proto == PROTO_XPAD) ? K_XPAD : K_PTR;
		hids[i] = t;
	}
}

static void report_done(void *ctx, int err, unsigned int actual);

static void
deliver_kbd(struct hid *h, const unsigned char *r, unsigned int n)
{
	int i, j;

	if (n < 8 || !sink || !sink->key)
		return;
	if (r[2] == 1)
		return;								/* rollover (phantom state) */
	for (i = 0; i < 8; i++) {						/* modifiers */
		unsigned int m = 1u << i;
		if ((r[0] ^ h->last[0]) & m)
			sink->key(sink->ctx, 0xE0 + i, (r[0] & m) != 0);
	}
	for (i = 2; i < 8; i++) {						/* released: in the last report, not in this one */
		if (h->last[i] < 4)
			continue;
		for (j = 2; j < 8 && r[j] != h->last[i]; j++)
			;
		if (j == 8)
			sink->key(sink->ctx, h->last[i], 0);
	}
	for (i = 2; i < 8; i++) {						/* pressed */
		if (r[i] < 4)
			continue;
		for (j = 2; j < 8 && h->last[j] != r[i]; j++)
			;
		if (j == 8)
			sink->key(sink->ctx, r[i], 1);
	}
	for (i = 0; i < 8; i++)
		h->last[i] = r[i];
}

static void
deliver_ptr(struct hid *h, const unsigned char *r, unsigned int n)
{
	const struct phe_usb_hid_ptr *p = &h->ptr;
	unsigned int b = 0;

	if (p->rid) {
		if (n < 1 || r[0] != p->rid)
			return;
		r++; n--;
	}
	if (p->bcount)
		b = phe_usb_hid_field(r, n, p->bpos, p->bcount > 3 ? 3 : p->bcount);
	if (!sink)
		return;
	if (p->rel) {
		if (sink->rel)
			sink->rel(sink->ctx, phe_usb_hid_sfield(r, n, p->xpos, p->xsize), phe_usb_hid_sfield(r, n, p->ypos, p->ysize), b);
	} else if (sink->abs) {
		sink->abs(sink->ctx, phe_usb_hid_field(r, n, p->xpos, p->xsize), (unsigned int)p->xmax,
		    phe_usb_hid_field(r, n, p->ypos, p->ysize), (unsigned int)p->ymax, b);
	}
	h->lastbuttons = b;
}

static void
report_done(void *ctx, int err, unsigned int actual)
{
	struct hid *h = ctx;

	if (!h->dev || !h->pipe)
		return;
	if (err == 0) {
		if (h->kind == K_KBD)
			deliver_kbd(h, h->rep, actual);
		else if (h->kind == K_XPAD)
			phe_usb_hid_xpad(h->rep, actual, &h->lastbuttons);
		else
			deliver_ptr(h, h->rep, actual);
	} else if (err == PHE_USB_ERR_STALLED) {
		phe_usb_pipe_clear_stall(h->pipe);
	}
	/* the next report (a cancelled pipe comes back with an error on close: dev is cleared before that) */
	if (h->dev)
		phe_usb_pipe_start(h->pipe, h->rep, (unsigned int)(h->mps < REP_MAX ? h->mps : REP_MAX), 0, report_done, h);
}

/* HID class requests (HID 1.11 section 7.2) and GET_DESCRIPTOR(Report) */
static int
setup(struct hid *h)
{
	unsigned char desc[512];
	unsigned int got = 0;

	if (h->kind == K_XPAD) {
		/* no class requests: the pad reports as soon as its interrupt pipe is read */
	} else if (h->kind == K_KBD) {
		/* SET_IDLE(0): report on change only */
		(void)phe_usb_ctrl(h->dev, 0x21, 0x0A, 0, (unsigned short)h->iface, 0, 0, 0);
		/* SET_PROTOCOL(0) = the boot protocol: the fixed 8-byte report */
		(void)phe_usb_ctrl(h->dev, 0x21, 0x0B, 0, (unsigned short)h->iface, 0, 0, 0);
	} else {
		(void)phe_usb_ctrl(h->dev, 0x21, 0x0A, 0, (unsigned short)h->iface, 0, 0, 0);
		if (phe_usb_ctrl(h->dev, 0x81, 0x06, 0x2200, (unsigned short)h->iface, desc, sizeof(desc), &got) != 0 ||
		    phe_usb_hid_parse(desc, got, &h->ptr) != 0)
			return (-1);
	}
	h->pipe = phe_usb_pipe_open(h->dev, h->iface, (unsigned int)h->ep, REP_MAX);
	if (!h->pipe)
		return (-1);
	if (h->kind == K_XPAD)
		HLOG("[usbh] hid: XInput gamepad on interface %d, ep %02x\n", h->iface, h->ep);
	else if (h->kind == K_KBD)
		HLOG("[usbh] hid: keyboard on interface %d, ep %02x (boot protocol)\n", h->iface, h->ep);
	else
		HLOG("[usbh] hid: %s pointer on interface %d, ep %02x, report %d: x %d/%d y %d/%d buttons %d/%d max %ld,%ld\n",
		    h->ptr.rel ? "relative" : "absolute", h->iface, h->ep, h->ptr.rid, h->ptr.xpos, h->ptr.xsize, h->ptr.ypos,
		    h->ptr.ysize, h->ptr.bpos, h->ptr.bcount, h->ptr.xmax, h->ptr.ymax);
	return (phe_usb_pipe_start(h->pipe, h->rep, (unsigned int)(h->mps < REP_MAX ? h->mps : REP_MAX), 0, report_done, h));
}

void
phe_usb_hid_service(void)
{
	int i;

	for (i = 0; i < HID_MAX; i++) {
		struct hid *h = &hids[i];
		if (h->dev && !h->ready && !h->failed) {
			if (setup(h) == 0)
				h->ready = 1;
			else
				h->failed = 1;
		}
	}
	/* a held stick keeps the pointer moving (the pad reports only on change): every 10th pass, about 100 Hz */
	{
		static unsigned int pass;
		if (++pass % 10 == 0)
			phe_usb_hid_xpad_tick();
	}
}
