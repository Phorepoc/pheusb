/* Copyright The Phorepoc Authors
 * SPDX-License-Identifier: BSD-2-Clause */

/* ure_test.c -- host test of phe_usb_ure.c (with FreeBSD's chip code, phe_usb_ure_bsd.inc) against a register model of
 * an RTL8153: the two register spaces (PLA, USB) behind the vendor control request, byte enables, the OCP window to the
 * PHY, and the status bits the init sequences poll. Checks the attach (chip version, the station address), the init (RX +
 * TX on, the RX filter), the link poll, the RX aggregation parser and the TX framing on the bulk pipes.
 *
 *   cc -DPHE_USB_URE_HOST -I ../../os/src/usbhost -I ../../os/third_party/freebsd-usb/sys/dev/usb/net ure_test.c \
 *      ../../os/src/usbhost/phe_usb_ure.c && ./a.out */
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include "phe_usb_ure_compat.h"
#include "phe_usb_ure.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %d: %s\n", __LINE__, #c); fails++; } } while (0)

int phe_usb_printf(const char *f, ...) { va_list a; va_start(a, f); vprintf(f, a); va_end(a); return 0; }
int pause(const char *w, int t) { (void)w; (void)t; return 0; }

/* ---- the chip model ---- */
static unsigned char pla[0x10000], usb[0x10000];
static unsigned short ocp[0x10000];
static unsigned int nctl, nwrites;
static const unsigned char kmac[6] = { 0x00, 0xe0, 0x4c, 0x68, 0x01, 0x53 };

static unsigned int rd32(unsigned char *m, unsigned int a) { return m[a] | m[a + 1] << 8 | m[a + 2] << 16 | (unsigned)m[a + 3] << 24; }

static void
model_reset(void)
{
	memset(pla, 0, sizeof(pla)); memset(usb, 0, sizeof(usb)); memset(ocp, 0, sizeof(ocp));
	pla[URE_PLA_TCR1] = 0x20; pla[URE_PLA_TCR1 + 1] = 0x5c;		/* version 0x5c20 (an RTL8153) */
	memcpy(pla + URE_PLA_BACKUP, kmac, 6);
	pla[URE_PLA_BOOT_CTRL] = URE_AUTOLOAD_DONE & 0xff; pla[URE_PLA_BOOT_CTRL + 1] = URE_AUTOLOAD_DONE >> 8;
	pla[URE_PLA_OOB_CTRL] = URE_LINK_LIST_READY;
	ocp[URE_OCP_PHY_STATUS] = URE_PHY_STAT_LAN_ON;
	ocp[0xe000] = 0x0100;						/* ure_enable_aldps(false)'s wait bit */
}

/* the OCP window: PLA_OCP_GPHY_BASE selects addr & 0xf000, PLA 0xb000-0xbfff is the 4 KB page */
static void
pla_write(unsigned int a, unsigned int byte)
{
	pla[a] = (unsigned char)byte;
	nwrites++;
	if (a == URE_PLA_CR && (byte & URE_CR_RST))
		pla[a] &= ~URE_CR_RST;					/* the reset completes at once */
}

static unsigned int ocp_base(void) { return (pla[URE_PLA_OCP_GPHY_BASE] | pla[URE_PLA_OCP_GPHY_BASE + 1] << 8) & 0xf000; }

int
phe_usb_ctrl(phe_usb_dev *d, unsigned char rt, unsigned char req, unsigned short value, unsigned short index, void *buf,
    unsigned short len, unsigned int *act)
{
	unsigned char *b = buf, *m;
	unsigned int i, sel = index & 0xff00, en = index & 0xff;
	(void)d;
	nctl++;
	if (req != 0x05 || (rt != 0x40 && rt != 0xc0))
		return 4;
	m = sel == URE_MCU_TYPE_PLA ? pla : usb;
	if (rt == 0xc0) {						/* read: whole words */
		for (i = 0; i < len; i++) {
			unsigned int a = value + i;
			if (m == pla && a >= 0xb000 && a < 0xc000) {
				unsigned short w = ocp[ocp_base() | ((a & 0xffe) & 0x0fff)];
				b[i] = (a & 1) ? w >> 8 : w & 0xff;
			} else
				b[i] = m[a];
		}
	} else {							/* write: byte enables (bit n = byte n of each dword) */
		for (i = 0; i < len; i++) {
			unsigned int a = value + i;
			if (i >= 8 || !(en & (1u << i)))				/* byte enables: bit n = byte n */
				continue;
			if (m == pla && a >= 0xb000 && a < 0xc000) {
				unsigned int o = ocp_base() | (a & 0x0ffe);
				ocp[o] = (a & 1) ? (ocp[o] & 0xff) | b[i] << 8 : (ocp[o] & 0xff00) | b[i];
				nwrites++;
			} else if (m == pla)
				pla_write(a, b[i]);
			else
				usb[a] = b[i], nwrites++;
		}
	}
	if (act)
		*act = len;
	return 0;
}

/* ---- the USB device: an RTL8153's vendor configuration ---- */
static const unsigned char cdesc[] = {
	9, 2, 39, 0, 1, 1, 0, 0xa0, 0x32,
	9, 4, 0, 0, 3, 0xff, 0xff, 0, 0,
	7, 5, 0x81, 2, 0x00, 0x04, 0,
	7, 5, 0x02, 2, 0x00, 0x04, 0,
	7, 5, 0x83, 3, 0x02, 0x00, 8,
};
static int devtag;
int
phe_usb_dev_info(phe_usb_dev *d, struct phe_usb_devinfo *i)
{
	(void)d;
	memset(i, 0, sizeof(*i));
	i->vid = 0x0bda; i->pid = 0x8153; i->speed = 2; i->superspeed = 1;
	i->cdesc = cdesc; i->cdesclen = sizeof(cdesc);
	return 0;
}

struct phe_usb_pipe { unsigned int ep; void *buf; unsigned int len; phe_usb_done_fn done; void *ctx; int busy; };
static struct phe_usb_pipe pin, pout;
static unsigned char sent[4096];
static unsigned int sentlen;
phe_usb_pipe *phe_usb_pipe_open(phe_usb_dev *d, int i, unsigned int e, unsigned int b)
	{ (void)d; (void)i; (void)b; struct phe_usb_pipe *p = (e & 0x80) ? &pin : &pout; p->ep = e; return p; }
void phe_usb_pipe_close(phe_usb_pipe *p) { p->busy = 0; }
int phe_usb_pipe_start(phe_usb_pipe *p, void *b, unsigned int l, int z, phe_usb_done_fn f, void *c)
{
	(void)z;
	if (p->busy)
		return -1;
	p->buf = b; p->len = l; p->done = f; p->ctx = c; p->busy = 1;
	if (p == &pout) {						/* OUT completes at once */
		memcpy(sent, b, l < sizeof(sent) ? l : sizeof(sent)); sentlen = l;
		p->busy = 0; f(c, 0, l);
	}
	return 0;
}
void phe_usb_pipe_abort(phe_usb_pipe *p) { p->busy = 0; }
int phe_usb_pipe_clear_stall(phe_usb_pipe *p) { (void)p; return 0; }

/* ---- the sink ---- */
static int st_present, st_link, nrx;
static unsigned char st_mac[6], lastrx[1600];
static unsigned int lastrxlen;
static void st(void *c, int p, int l, const unsigned char *m) { (void)c; st_present = p; st_link = l; memcpy(st_mac, m, 6); }
static void rx(void *c, const unsigned char *f, unsigned int n) { (void)c; nrx++; memcpy(lastrx, f, n); lastrxlen = n; }
static int npoll;
static void pl(void *c) { (void)c; npoll++; }
static const struct phe_usb_net_sink sink = { st, rx, pl, 0 };

static void
put32(unsigned char *p, unsigned int v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }

int
main(void)
{
	unsigned char agg[256], fr[64];
	unsigned int i, o;

	model_reset();
	phe_usb_ure_set_sink(&sink);
	phe_usb_ure_notify(&devtag, 1);
	phe_usb_ure_service();						/* the set-up: attach_post + init */
	CHECK(st_present == 1 && st_link == 0);
	CHECK(memcmp(st_mac, kmac, 6) == 0);
	CHECK(memcmp(pla + URE_PLA_IDR, kmac, 6) == 0);			/* ure_init programmed the MAC */
	CHECK((pla[URE_PLA_CR] & (URE_CR_RE | URE_CR_TE)) == (URE_CR_RE | URE_CR_TE));
	CHECK((rd32(pla, URE_PLA_RCR) & (URE_RCR_APM | URE_RCR_AB | URE_RCR_AM)) == (URE_RCR_APM | URE_RCR_AB | URE_RCR_AM));
	CHECK(rd32(pla, URE_PLA_MAR0) == 0xffffffff && rd32(pla, URE_PLA_MAR4) == 0xffffffff);
	CHECK((ocp[URE_OCP_BASE_MII + 0] & 0x1200) == 0x1200);		/* autonegotiation restarted */
	printf("init: %u control requests, %u register bytes written\n", nctl, nwrites);
	CHECK(phe_usb_ure_tx(fr, 60) == -1);				/* no link yet */

	/* the link comes up: the next ~250 service passes see it */
	pla[URE_PLA_PHYSTATUS] |= URE_PHYSTATUS_LINK;
	for (i = 0; i < 260; i++)
		phe_usb_ure_service();
	CHECK(st_link == 1 && npoll > 0);
	CHECK(pin.busy == 1 && pin.ep == 0x81);				/* an IN transfer is running */

	/* RX: two frames in one aggregation buffer (header 24 bytes, frame + FCS, 8-aligned), then a bogus length */
	memset(agg, 0, sizeof(agg));
	o = 0;
	put32(agg + o, 64); o += 24; memset(agg + o, 0xa1, 60); o += 64;	/* 60-byte frame + 4 FCS */
	put32(agg + o, 70); o += 24; memset(agg + o, 0xb2, 66); o += 72;	/* 66 + 4, padded to 72 */
	memcpy(pin.buf, agg, o);
	pin.busy = 0; pin.done(pin.ctx, 0, o);
	CHECK(nrx == 2 && lastrxlen == 66 && lastrx[0] == 0xb2);
	CHECK(phe_usb_ure_rx_parse(agg, 20) == -1);				/* a short header */
	put32(agg, 0x7fff);
	CHECK(phe_usb_ure_rx_parse(agg, 100) == -1);				/* the length FreeBSD refuses */

	/* TX: a 42-byte ARP-sized frame goes out padded to 60, with the first + last segment flags */
	for (i = 0; i < 42; i++)
		fr[i] = (unsigned char)i;
	CHECK(phe_usb_ure_tx(fr, 42) == 0);
	CHECK(sentlen == 8 + 60);
	CHECK(rd32(sent, 0) == (60u | URE_TKPKT_TX_FS | URE_TKPKT_TX_LS) && rd32(sent, 4) == 0);
	CHECK(sent[8 + 41] == 41 && sent[8 + 42] == 0 && sent[8 + 59] == 0);

	/* unplug */
	phe_usb_ure_notify(&devtag, 0);
	CHECK(st_present == 0);
	CHECK(phe_usb_ure_tx(fr, 60) == -1);
	printf(fails ? "ure_test: %d FAILED\n" : "ure_test: PASS\n", fails);
	return fails != 0;
}
