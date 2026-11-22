/* Copyright The Phorepoc Authors
 * SPDX-License-Identifier: BSD-2-Clause */

/* phe_usb_ure.c -- see phe_usb_ure.h. The chip code is FreeBSD's if_ure.c, lifted verbatim into phe_usb_ure_bsd.inc by
 * gen_ure.py; this file gives it the handful of usb_ether / ifnet calls it makes and runs the transfers the way if_ure's
 * bulk callbacks do (one IN transfer of the chip's RX aggregation buffer at a time, one OUT transfer of framed packets).
 * Only phe_usb_api.h is used (no FreeBSD stack types), so the whole driver also builds on a host against a chip model
 * (tests/ure). The MII part (autonegotiation restart) is IEEE 802.3 clause 22, written here: FreeBSD's goes through
 * miibus. */
#include <stdint.h>
#include <stdbool.h>
#include "phe_usb_api.h"
#include "phe_usb_ure.h"

int phe_usb_printf(const char *, ...);
int pause(const char *, int);
void *memset(void *, int, __SIZE_TYPE__);
#define ULOG(...) phe_usb_printf(__VA_ARGS__)

#include "phe_usb_ure_compat.h"

#define ETHER_ADDR_LEN		6
#define ETHER_HDR_LEN		14
#define ETHER_CRC_LEN		4
#define ETHER_VLAN_ENCAP_LEN	4
#define ETHER_IS_ZERO(a)	(((a)[0] | (a)[1] | (a)[2] | (a)[3] | (a)[4] | (a)[5]) == 0)
#define IFF_BROADCAST	0x2
#define IFF_PROMISC	0x100
#define IFF_ALLMULTI	0x200
#define IFF_DRV_RUNNING	0x40
#define IFF_DRV_OACTIVE	0x400
#define IFCAP_VLAN_HWTAGGING 0x8
#define MA_OWNED	0
#define mtx_assert(m, t)	((void)0)
#define hz		1000
#define UT_WRITE_VENDOR_DEVICE	0x40
#define UT_READ_VENDOR_DEVICE	0xc0
#define UR_SET_ADDRESS		0x05
enum { USB_SPEED_VARIABLE, USB_SPEED_LOW, USB_SPEED_FULL, USB_SPEED_HIGH, USB_SPEED_SUPER, USB_SPEED_SUPER_PLUS,
	USB_SPEED_SUPER_PLUS_X2, USB_SPEED_SUPER_PLUS_GEN2_X2 };
struct usb_device_request { uint8_t bmRequestType, bRequest; uint16_t wValue, wIndex, wLength; };
#define USETW(w, v)	((w) = (uint16_t)(v))
#define USETDW(b, v)	do { uint32_t _v = (v); (b)[0] = _v; (b)[1] = _v >> 8; (b)[2] = _v >> 16; (b)[3] = _v >> 24; } while (0)
#define UGETDW(b)	((uint32_t)(b)[0] | (uint32_t)(b)[1] << 8 | (uint32_t)(b)[2] << 16 | (uint32_t)(b)[3] << 24)
#define le32toh(x)	(x)
#define htole32(x)	(x)
#define bswap32(x)	__builtin_bswap32(x)
#define roundup(x, y)	((((x) + ((y) - 1)) / (y)) * (y))
#define DEVPRINTFN(...)	do { } while (0)
#define device_printf(dev, ...)	ULOG("[ure] " __VA_ARGS__)
#define uether_getsc(ue)	((struct ure_softc *)(ue)->ue_sc)
#define uether_getifp(ue)	((ue)->ue_ifp)
#define if_getdrvflags(ifp)	((ifp)->drvflags)
#define if_setdrvflagbits(ifp, set, clr) ((ifp)->drvflags = ((ifp)->drvflags | (set)) & ~(clr))
#define if_getflags(ifp)	((ifp)->flags)
#define if_getlladdr(ifp)	((ifp)->lladdr)
#define if_getmtu(ifp)		1500
#define if_getcapenable(ifp)	0
#define if_getcapabilities(ifp)	0
#define if_foreach_llmaddr(ifp, cb, arg) ((void)0)/* IFF_ALLMULTI is always set: ure_rxfilter never hashes */
#define usbd_get_speed(udev)	ure_speed
#define usbd_xfer_set_stall(x)	((void)((x) && (x)->pipe && phe_usb_pipe_clear_stall((x)->pipe)))
#define usbd_transfer_stop(x)	do { if ((x) && (x)->pipe) phe_usb_pipe_abort((x)->pipe); } while (0)
#define arc4rand(p, n, r)	ure_rand(p, n)

static int ure_speed;
static int ure_ifmedia_upd(if_t ifp);

static int
uether_do_request(struct usb_ether *ue, struct usb_device_request *req, void *buf, int timeout)
{
	(void)timeout;
	return (phe_usb_ctrl(ue->ue_udev, req->bmRequestType, req->bRequest, req->wValue, req->wIndex, buf, req->wLength,
	    0) ? -1 : 0);
}

static void
uether_pause(struct usb_ether *ue, unsigned int ticks)
{
	(void)ue;
	pause("ure", ticks ? (int)ticks : 1);
}

static void
ure_rand(unsigned char *p, int n)
{
	static uint32_t x = 0x2545F491u;
	while (n--) {
		x ^= x << 13; x ^= x >> 17; x ^= x << 5;
		*p++ = (unsigned char)x;
	}
}

#include "phe_usb_ure_bsd.inc"

/* ---- MII: autonegotiation (IEEE 802.3 clause 22 registers through the OCP window, as if_ure's miibus calls map them) ---- */


static struct ure_softc sc0;
static struct ifnet ifn;
static struct usb_xfer rx_x, tx_x;

static int
ure_ifmedia_upd(if_t ifp)
{
	struct ure_softc *sc = &sc0;

	(void)ifp;
	ure_ocp_reg_write(sc, URE_OCP_BASE_MII + MII_ANAR * 2, ANAR_10_100_ALL);
	ure_ocp_reg_write(sc, URE_OCP_BASE_MII + MII_100T2CR * 2, GTCR_ADV_1000);
	ure_ocp_reg_write(sc, URE_OCP_BASE_MII + MII_BMCR * 2, BMCR_AUTOEN | BMCR_STARTNEG);
	return (0);
}

/* ---- the device ---- */
static const struct { unsigned short vid, pid; } ure_ids[] = {	/* if_ure.c ure_devs[] (usbdevs numbers) */
	{ 0x13b1, 0x0041 }, { 0x2001, 0xb301 }, { 0x056e, 0x4017 }, { 0x17ef, 0x7205 }, { 0x17ef, 0x720c },
	{ 0x17ef, 0x3069 }, { 0x17ef, 0x3082 }, { 0x17ef, 0x720a }, { 0x17ef, 0x3054 }, { 0x17ef, 0x3062 },
	{ 0x17ef, 0xa387 }, { 0x17ef, 0xa359 }, { 0x045e, 0x07ab }, { 0x045e, 0x07c6 }, { 0x045e, 0x0c5e },
	{ 0x0955, 0x09ff }, { 0x0bda, 0x8050 }, { 0x0bda, 0x8053 }, { 0x0bda, 0x8152 }, { 0x0bda, 0x8153 },
	{ 0x0bda, 0x8156 }, { 0x04e8, 0xa101 }, { 0x2357, 0x0601 }, { 0x2357, 0x0602 },
};

#define TXQ	8
#define RXBUF	URE_8153_RX_BUFSZ
static const struct phe_usb_net_sink *sink;
static phe_usb_dev *dev;		/* the adapter (one) */
static int pending;			/* attached, set-up not run yet */
static int up, link_up, rx_busy, tx_busy, tick;
static unsigned int iface, ep_in, ep_out;
static unsigned char rxbuf[RXBUF] __attribute__((aligned(8)));
static unsigned char txbuf[URE_TX_BUFSZ] __attribute__((aligned(8)));
static unsigned char txq[TXQ][1520];
static unsigned short txlen[TXQ];
static unsigned int txhead, txtail;

void
phe_usb_ure_set_sink(const struct phe_usb_net_sink *s)
{
	sink = s;
}

/* the vendor-class interface (255/255) with a bulk IN and a bulk OUT endpoint, as ure_probe matches it */
static int
find_iface(const unsigned char *c, unsigned int len, unsigned int *ifc, unsigned int *in, unsigned int *out)
{
	unsigned int o = 0;
	int ours = 0;

	*in = *out = 0;
	while (o + 2 <= len && c[o] >= 2) {
		if (c[o + 1] == 4 && o + 9 <= len) {			/* INTERFACE */
			if (ours && *in && *out)
				return (1);
			ours = c[o + 3] == 0 && c[o + 5] == 0xff && c[o + 6] == 0xff;
			if (ours)
				*ifc = c[o + 2];
		} else if (c[o + 1] == 5 && ours && o + 7 <= len && (c[o + 3] & 3) == 2) {	/* bulk ENDPOINT */
			if (c[o + 2] & 0x80)
				*in = c[o + 2];
			else
				*out = c[o + 2];
		}
		o += c[o];
	}
	return (ours && *in && *out);
}

void
phe_usb_ure_notify(void *d, int attached)
{
	struct phe_usb_devinfo info;
	unsigned int i;

	if (!attached) {
		if (d == dev) {
			if (rx_x.pipe)
				phe_usb_pipe_close(rx_x.pipe);
			if (tx_x.pipe)
				phe_usb_pipe_close(tx_x.pipe);
			rx_x.pipe = tx_x.pipe = 0;
			dev = 0; up = link_up = pending = rx_busy = tx_busy = 0;
			if (sink && sink->state)
				sink->state(sink->ctx, 0, 0, sc0.sc_ue.ue_eaddr);
		}
		return;
	}
	if (dev || phe_usb_dev_info((phe_usb_dev *)d, &info) != 0)
		return;
	for (i = 0; i < sizeof(ure_ids) / sizeof(ure_ids[0]); i++)
		if (ure_ids[i].vid == info.vid && ure_ids[i].pid == info.pid)
			break;
	if (i == sizeof(ure_ids) / sizeof(ure_ids[0]) || !find_iface(info.cdesc, info.cdesclen, &iface, &ep_in, &ep_out))
		return;
	ure_speed = info.superspeed ? USB_SPEED_SUPER : info.speed == 2 ? USB_SPEED_HIGH : USB_SPEED_FULL;
	dev = (phe_usb_dev *)d;
	pending = 1;
	ULOG("[ure] %04x:%04x on bus %u addr %u, interface %u, bulk %02x/%02x, %s\n", info.vid, info.pid, info.bus, info.addr,
	    iface, ep_in, ep_out, info.superspeed ? "SuperSpeed" : info.speed == 2 ? "high speed" : "full speed");
}

/* if_ure's attach + init, in its order: ure_attach_post (chip version, the chip init, the station address), then
 * ure_init (reset, MAC, RX aggregation, the filter, RX + TX on, autonegotiation) */
static void
setup(void)
{
	struct ure_softc *sc = &sc0;
	unsigned int i;

	for (i = 0; i < sizeof(sc0); i++)
		((unsigned char *)&sc0)[i] = 0;
	sc->sc_ue.ue_udev = dev;
	sc->sc_ue.ue_ifp = &ifn;
	sc->sc_ue.ue_sc = sc;
	sc->sc_rx_xfer[0] = &rx_x;
	sc->sc_tx_xfer[0] = &tx_x;
	ifn.flags = IFF_BROADCAST | IFF_ALLMULTI;
	ifn.drvflags = 0;
	ure_attach_post(&sc->sc_ue);
	if (!sc->sc_flags) {
		ULOG("[ure] unknown chip version %04x: not used\n", sc->sc_ver);
		dev = 0;
		return;
	}
	for (i = 0; i < 6; i++)
		ifn.lladdr[i] = sc->sc_ue.ue_eaddr[i];
	rx_x.pipe = phe_usb_pipe_open(dev, (int)iface, ep_in, RXBUF);
	tx_x.pipe = phe_usb_pipe_open(dev, (int)iface, ep_out, URE_TX_BUFSZ);
	if (!rx_x.pipe || !tx_x.pipe) {
		ULOG("[ure] no bulk pipes\n");
		dev = 0;
		return;
	}
	ure_init(&sc->sc_ue);
	up = 1;
	ULOG("[ure] RTL%s (version %04x), MAC %02x:%02x:%02x:%02x:%02x:%02x\n",
	    (sc->sc_flags & URE_FLAG_8152) ? "8152" : (sc->sc_flags & URE_FLAG_8153) ? "8153" :
	    (sc->sc_flags & URE_FLAG_8153B) ? "8153B" : "8156", sc->sc_ver, ifn.lladdr[0], ifn.lladdr[1], ifn.lladdr[2],
	    ifn.lladdr[3], ifn.lladdr[4], ifn.lladdr[5]);
	if (sink && sink->state)
		sink->state(sink->ctx, 1, 0, ifn.lladdr);
}

int
phe_usb_ure_rx_parse(const unsigned char *buf, unsigned int actual)
{
	unsigned int off = 0, len;
	int n = 0;
	struct ure_rxpkt pkt;

	/* ure_bulk_read_callback's loop: header, length check, frame minus the FCS, 8-aligned */
	while (actual > 0) {
		if (actual < sizeof(pkt))
			return (-1);
		pkt.ure_pktlen = UGETDW(buf + off);
		off += sizeof(pkt);
		actual -= sizeof(pkt);
		len = le32toh(pkt.ure_pktlen) & URE_RXPKT_LEN_MASK;
		if (len >= URE_RXPKT_LEN_MASK || actual < len)
			return (-1);
		if (len >= ETHER_HDR_LEN + ETHER_CRC_LEN) {
			if (sink && sink->rx)
				sink->rx(sink->ctx, buf + off, len - ETHER_CRC_LEN);
			n++;
		}
		len = roundup(len, URE_RXPKT_ALIGN);
		if (len > actual)
			break;
		off += len;
		actual -= len;
	}
	return (n);
}

unsigned int
phe_usb_ure_tx_frame(unsigned char *out, const unsigned char *frame, unsigned int len)
{
	unsigned int i, n = len < 60 ? 60 : len;

	/* ure_bulk_write_callback: struct ure_txpkt {len | first | last, csum 0}, the frame, padded to 4 */
	USETDW(out, (n & URE_TXPKT_LEN_MASK) | URE_TKPKT_TX_FS | URE_TKPKT_TX_LS);
	USETDW(out + 4, 0);
	for (i = 0; i < n; i++)
		out[8 + i] = i < len ? frame[i] : 0;
	for (; i < roundup(n, URE_TXPKT_ALIGN); i++)
		out[8 + i] = 0;
	return (8 + roundup(n, URE_TXPKT_ALIGN));
}

static void rx_start(void);

static void
rx_done(void *ctx, int err, unsigned int actual)
{
	(void)ctx;
	rx_busy = 0;
	if (!dev)
		return;
	if (err) {
		phe_usb_pipe_clear_stall(rx_x.pipe);		/* as ure_bulk_read_callback: clear the stall, go again */
		return;
	}
	phe_usb_ure_rx_parse(rxbuf, actual);
}

static void
rx_start(void)
{
	if (!rx_busy && phe_usb_pipe_start(rx_x.pipe, rxbuf, RXBUF, 0, rx_done, 0) == 0)
		rx_busy = 1;
}

static void
tx_done(void *ctx, int err, unsigned int actual)
{
	(void)ctx; (void)actual;
	tx_busy = 0;
	if (err && dev)
		phe_usb_pipe_clear_stall(tx_x.pipe);
}

static void
tx_kick(void)
{
	unsigned int pos = 0;

	if (tx_busy || !link_up)
		return;
	/* pack queued frames into one transfer, as the write callback does */
	while (txtail != txhead && pos + 8 + 1520 <= URE_TX_BUFSZ) {
		unsigned int k = txtail % TXQ;
		pos += phe_usb_ure_tx_frame(txbuf + pos, txq[k], txlen[k]);
		txtail++;
	}
	if (pos && phe_usb_pipe_start(tx_x.pipe, txbuf, pos, 0, tx_done, 0) == 0)
		tx_busy = 1;
}

int
phe_usb_ure_tx(const unsigned char *frame, unsigned int len)
{
	unsigned int i, k;

	if (!up || !link_up || len > 1514 || txhead - txtail >= TXQ)
		return (-1);
	k = txhead % TXQ;
	for (i = 0; i < len; i++)
		txq[k][i] = frame[i];
	txlen[k] = (unsigned short)len;
	txhead++;
	tx_kick();
	return (0);
}

void
phe_usb_ure_service(void)
{
	if (pending) {
		pending = 0;
		setup();
	}
	if (!up || !dev)
		return;
	/* ure_tick, every ~250 ms: the PHY's link status */
	if (++tick >= 250) {
		int l;
		tick = 0;
		l = ure_get_link_status(&sc0);
		if (l != link_up) {
			link_up = l;
			ULOG("[ure] link %s\n", l ? "up" : "down");
			if (sink && sink->state)
				sink->state(sink->ctx, 1, l, ifn.lladdr);
		}
	}
	if (link_up) {
		rx_start();
		if (sink && sink->poll)
			sink->poll(sink->ctx);
		tx_kick();
	}
}
