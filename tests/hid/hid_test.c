/* Copyright The Phorepoc Authors
 * SPDX-License-Identifier: BSD-2-Clause */

/* hid_test.c -- host test of phe_usb_hid.c's report-descriptor parser and field extraction, unmodified, on the report
 * descriptors of QEMU's usb-tablet and usb-mouse and of a touch screen with a report id. The USB calls are stubs.
 *
 *   cc -DPHE_USB_HID_HOST -I ../../os/src/usbhost hid_test.c ../../os/src/usbhost/phe_usb_hid.c && ./a.out */
#include <stdio.h>
#include "phe_usb_api.h"
#include "phe_usb_hid.h"

/* the client's USB calls: never reached by the parser */
int phe_usb_dev_info(phe_usb_dev *d, struct phe_usb_devinfo *i) { (void)d; (void)i; return -1; }
int phe_usb_ctrl(phe_usb_dev *d, unsigned char a, unsigned char b, unsigned short c, unsigned short e, void *f, unsigned short g,
    unsigned int *h) { (void)d; (void)a; (void)b; (void)c; (void)e; (void)f; (void)g; (void)h; return -1; }
phe_usb_pipe *phe_usb_pipe_open(phe_usb_dev *d, int i, unsigned int e, unsigned int b) { (void)d; (void)i; (void)e; (void)b; return 0; }
void phe_usb_pipe_close(phe_usb_pipe *p) { (void)p; }
int phe_usb_pipe_start(phe_usb_pipe *p, void *b, unsigned int l, int z, phe_usb_done_fn f, void *c)
	{ (void)p; (void)b; (void)l; (void)z; (void)f; (void)c; return -1; }
int phe_usb_pipe_clear_stall(phe_usb_pipe *p) { (void)p; return 0; }

static int fails;

static unsigned int keys[16], downs[16], nkeys, rels, lastb;
static int sdx, sdy;
static void k(void *c, unsigned int u, int d) { (void)c; if (nkeys < 16) { keys[nkeys] = u; downs[nkeys++] = (unsigned int)d; } }
static void r(void *c, int dx, int dy, unsigned int b) { (void)c; rels++; sdx = dx; sdy = dy; lastb = b; }
static const struct phe_usb_hid_sink sinkt = { k, r, 0, 0 };
#define CHECK(c) do { if (!(c)) { printf("FAIL %d: %s\n", __LINE__, #c); fails++; } } while (0)

static const unsigned char tablet[] = {
	0x05, 0x01, 0x09, 0x02, 0xa1, 0x01, 0x09, 0x01, 0xa1, 0x00, 0x05, 0x09, 0x19, 0x01, 0x29, 0x03,
	0x15, 0x00, 0x25, 0x01, 0x95, 0x03, 0x75, 0x01, 0x81, 0x02, 0x95, 0x01, 0x75, 0x05, 0x81, 0x01,
	0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x15, 0x00, 0x26, 0xff, 0x7f, 0x35, 0x00, 0x46, 0xff, 0x7f,
	0x75, 0x10, 0x95, 0x02, 0x81, 0x02, 0x05, 0x01, 0x09, 0x38, 0x15, 0x81, 0x25, 0x7f, 0x35, 0x00,
	0x45, 0x00, 0x75, 0x08, 0x95, 0x01, 0x81, 0x06, 0xc0, 0xc0 };
static const unsigned char mouse[] = {
	0x05, 0x01, 0x09, 0x02, 0xa1, 0x01, 0x09, 0x01, 0xa1, 0x00, 0x05, 0x09, 0x19, 0x01, 0x29, 0x03,
	0x15, 0x00, 0x25, 0x01, 0x95, 0x03, 0x75, 0x01, 0x81, 0x02, 0x95, 0x01, 0x75, 0x05, 0x81, 0x01,
	0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x09, 0x38, 0x15, 0x81, 0x25, 0x7f, 0x75, 0x08, 0x95, 0x03,
	0x81, 0x06, 0xc0, 0xc0 };
/* a touch screen: a keyboard report (id 1) first, then the pointer as report id 2 with a 12-bit X/Y */
static const unsigned char touch[] = {
	0x05, 0x01, 0x09, 0x06, 0xa1, 0x01, 0x85, 0x01, 0x05, 0x07, 0x19, 0xe0, 0x29, 0xe7, 0x15, 0x00,
	0x25, 0x01, 0x75, 0x01, 0x95, 0x08, 0x81, 0x02, 0xc0,
	0x05, 0x0d, 0x09, 0x04, 0xa1, 0x01, 0x85, 0x02, 0x09, 0x22, 0xa1, 0x02, 0x09, 0x42, 0x15, 0x00,
	0x25, 0x01, 0x75, 0x01, 0x95, 0x01, 0x81, 0x02, 0x95, 0x07, 0x81, 0x03, 0x05, 0x01, 0x26, 0xff,
	0x0f, 0x75, 0x0c, 0x95, 0x01, 0x09, 0x30, 0x81, 0x02, 0x09, 0x31, 0x81, 0x02, 0xc0, 0xc0 };

int main(void)
{
	struct phe_usb_hid_ptr p;

	CHECK(phe_usb_hid_parse(tablet, sizeof(tablet), &p) == 0);
	CHECK(!p.rel && p.rid == 0 && p.bpos == 0 && p.bcount == 3);
	CHECK(p.xpos == 8 && p.xsize == 16 && p.ypos == 24 && p.ysize == 16 && p.xmax == 0x7fff && p.ymax == 0x7fff);
	CHECK(p.wpos == 40 && p.wsize == 8);
	{
		const unsigned char r[] = { 0x01, 0x34, 0x12, 0xff, 0x7f, 0x00 };	/* left button, x 0x1234, y 0x7fff */
		CHECK(phe_usb_hid_field(r, sizeof(r), p.bpos, 3) == 1);
		CHECK(phe_usb_hid_field(r, sizeof(r), p.xpos, p.xsize) == 0x1234);
		CHECK(phe_usb_hid_field(r, sizeof(r), p.ypos, p.ysize) == 0x7fff);
	}

	CHECK(phe_usb_hid_parse(mouse, sizeof(mouse), &p) == 0);
	CHECK(p.rel && p.xpos == 8 && p.xsize == 8 && p.ypos == 16 && p.ysize == 8 && p.wpos == 24);
	{
		const unsigned char r[] = { 0x02, 0xfb, 0x07, 0x00 };		/* right button, dx -5, dy 7 */
		CHECK(phe_usb_hid_field(r, sizeof(r), p.bpos, 3) == 2);
		CHECK(phe_usb_hid_sfield(r, sizeof(r), p.xpos, p.xsize) == -5);
		CHECK(phe_usb_hid_sfield(r, sizeof(r), p.ypos, p.ysize) == 7);
	}

	CHECK(phe_usb_hid_parse(touch, sizeof(touch), &p) == 0);
	CHECK(p.rid == 2 && !p.rel && p.xpos == 8 && p.xsize == 12 && p.ypos == 20 && p.ysize == 12 && p.xmax == 4095);
	CHECK(p.bpos == 0 && p.bcount == 1);				/* the tip switch is the button */
	{
		const unsigned char r[] = { 0x01, 0x23, 0x01, 0x80 };		/* after the id: tip, x 0x123, y 0x800 */
		CHECK(phe_usb_hid_field(r, sizeof(r), p.xpos, p.xsize) == 0x123);
		CHECK(phe_usb_hid_field(r, sizeof(r), p.ypos, p.ysize) == 0x800);
	}

	CHECK(phe_usb_hid_parse(touch, 20, &p) != 0);			/* truncated: no X/Y */

	/* XInput: A down, then d-pad up + A up, then the left stick hard right + RT */
	{
		unsigned char x[20] = { 0x00, 0x14 };
		unsigned int last = 0;
		phe_usb_hid_set_sink(&sinkt);
		x[3] = 0x10;					/* A */
		phe_usb_hid_xpad(x, 20, &last);
		CHECK(nkeys == 1 && keys[0] == 0x3F && downs[0] == 1);
		x[3] = 0; x[2] = 0x01;				/* A up, d-pad up down */
		phe_usb_hid_xpad(x, 20, &last);
		CHECK(nkeys == 3 && keys[1] == 0x52 && downs[1] == 1 && keys[2] == 0x3F && downs[2] == 0);
		x[2] = 0; x[6] = 0xff; x[7] = 0x7f; x[5] = 0xff;	/* stick x = 32767, RT full */
		rels = 0;
		phe_usb_hid_xpad(x, 20, &last);
		CHECK(rels == 1 && sdx > 4 && sdy == 0 && lastb == 1);
		phe_usb_hid_xpad_tick();
		CHECK(rels == 2 && sdx > 4 && lastb == 1);	/* the held stick repeats */
		x[6] = 0; x[7] = 0; x[5] = 0;
		phe_usb_hid_xpad(x, 20, &last);
		CHECK(lastb == 0 && sdx == 0);
		rels = 0;
		phe_usb_hid_xpad_tick();
		CHECK(rels == 0);				/* centred: no motion */
		x[1] = 0x03;					/* not an input report */
		nkeys = 0;
		phe_usb_hid_xpad(x, 20, &last);
		CHECK(nkeys == 0);
	}

	printf("hid: %s\n", fails ? "FAIL" : "PASS");
	return fails ? 1 : 0;
}
