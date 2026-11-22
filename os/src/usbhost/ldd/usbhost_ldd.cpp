// Copyright The Phorepoc Authors
// SPDX-License-Identifier: BSD-2-Clause

// usbhost_ldd.cpp -- the two logical devices the 2010 USB host user side (FDF, the function drivers) talks to, written over
// the FreeBSD stack (phe_usb_api.h) instead of the closed usbhost.dll:
//   "USBHUBDRIVER" (RUsbHubDriver / RUsbDevice, d32usbdi_hubdriver.h): bus events (device attached / removed), a handle per
//       device, its descriptors, string descriptors, interface tokens. One client in the real driver (the Function Driver
//       Framework, SID 0x10282B48); here any client is accepted (the SID check is not made yet).
//   "USBDI" (RUsbInterface / RUsbPipe, d32usbdi.h): one channel per interface, opened with a token from the hub driver:
//       the interface descriptor, string descriptors, alternate settings, control transfers on endpoint 0, bulk and
//       interrupt pipes, and the shared chunk the zero-copy transfer strategy (usbdi_utils) lays its transfers out in.
// Both are DLogicalChannelBase drivers: requests run in the client's thread. Everything that touches the stack is queued to
// the USB thread (the DFC thread of phe_usb_ekern.cpp calling phe_usb_service()) and runs from its work hook; synchronous
// controls wait for it on a fast semaphore. Isochronous transfers and suspend / resume are not implemented.
#include <kernel/kern_priv.h>
#include <kernel/kernel.h>
#include <d32usbdi_hubdriver.h>
#include <d32usbtransfers.h>
#include <d32usbdi.h>

extern "C" {
#include "../phe_usb_api.h"
void phe_usb_kick(void);
void ekseam_puts(const char*);
}

// ---- small things ---------------------------------------------------------------------------------------------------

static volatile TInt gLock;
struct TLockGuard
	{
	TLockGuard()
		{
		iIrq = NKern::DisableAllInterrupts();
		while (__sync_lock_test_and_set(&gLock, 1))
			{}
		}
	~TLockGuard()
		{
		__sync_lock_release(&gLock);
		NKern::RestoreInterrupts(iIrq);
		}
	TInt iIrq;
	};

_LIT(KHubName, "USBHUBDRIVER");
_LIT(KUsbdiName, "USBDI");
static const TInt KMaxDev = 16;
static const TInt KMaxEvents = 32;
static const TInt KPipeBuf = 65536;

// FreeBSD usb_error_t -> the 2010 USB errors (d32usbdi_errors.h)
static TInt MapError(TInt aUsbErr)
	{
	switch (aUsbErr)
		{
	case 0:							return KErrNone;
	case PHE_USB_ERR_STALLED:		return KErrUsbStalled;
	case PHE_USB_ERR_TIMEOUT:		return KErrUsbTimeout;
	default:						return KErrUsbIOError;
		}
	}

// ---- the device table, kept by the USB thread -----------------------------------------------------------------------

struct SDev
	{
	TUint iHandle;							// 0 = free
	phe_usb_dev* iDev;						// valid while iPresent (USB thread only)
	TBool iPresent;
	TInt iOpen;								// RUsbDevice::Open count
	TUint8 iDd[18];
	TUint8* iCfg;
	TInt iCfgLen;
	TUint iSpeed, iBus;
	};
static SDev gDevs[KMaxDev];
static TUint gNextHandle = 1;

struct SEvent { TInt iType; TUint iHandle; };
static SEvent gEvents[KMaxEvents];
static TInt gEvHead, gEvCount;

static SDev* DevByHandle(TUint aHandle)		// any context; a descriptor copy outlives the device until its table slot is reused
	{
	if (!aHandle)
		return NULL;
	for (TInt i = 0; i < KMaxDev; ++i)
		if (gDevs[i].iHandle == aHandle)
			return &gDevs[i];
	return NULL;
	}

static void PostEvent(TInt aType, TUint aHandle)
	{
	TLockGuard g;
	if (gEvCount == KMaxEvents)
		return;								// overflow: the oldest stay
	gEvents[(gEvHead + gEvCount++) % KMaxEvents].iType = aType;
	gEvents[(gEvHead + gEvCount - 1) % KMaxEvents].iHandle = aHandle;
	}

// USB thread: a device appeared or went (phe_usb_set_notify)
static void OnDevice(void*, phe_usb_dev* aDev, int aAttached)
	{
	if (aAttached)
		{
		phe_usb_devinfo info;
		phe_usb_dev_info(aDev, &info);
		SDev* d = NULL;
		for (TInt i = 0; i < KMaxDev && !d; ++i)
			if (!gDevs[i].iPresent && !gDevs[i].iOpen)		// a gone device nobody holds open: reuse its slot
				d = &gDevs[i];
		if (!d || info.cdesclen == 0)
			return;
		NKern::ThreadEnterCS();
		TUint8* cfg = (TUint8*)Kern::Alloc(info.cdesclen);
		NKern::ThreadLeaveCS();
		if (!cfg)
			return;
		memcpy(cfg, info.cdesc, info.cdesclen);
		if (d->iCfg)
			{
			NKern::ThreadEnterCS();
			Kern::Free(d->iCfg);
			NKern::ThreadLeaveCS();
			}
		memcpy(d->iDd, info.ddesc, 18);
		d->iCfg = cfg;
		d->iCfgLen = info.cdesclen;
		d->iSpeed = info.speed;
		d->iBus = info.bus;
		d->iDev = aDev;
		d->iOpen = 0;
		d->iHandle = gNextHandle++;
		d->iPresent = ETrue;
		ekseam_puts("[usbh] hub driver: device attached\n");
		PostEvent(RUsbHubDriver::TBusEvent::EDeviceAttached, d->iHandle);
		}
	else
		{
		for (TInt i = 0; i < KMaxDev; ++i)
			if (gDevs[i].iPresent && gDevs[i].iDev == aDev)
				{
				gDevs[i].iPresent = EFalse;
				gDevs[i].iDev = NULL;
				ekseam_puts("[usbh] hub driver: device removed\n");
				PostEvent(RUsbHubDriver::TBusEvent::EDeviceRemoved, gDevs[i].iHandle);
				}
		}
	}

// ---- work for the USB thread ----------------------------------------------------------------------------------------

struct SWork
	{
	SWork* iNext;
	void (*iFn)(SWork&);
	TAny* iA;
	TAny* iB;
	TAny* iC;
	TInt iI;
	TInt iR;
	NFastSemaphore iSem;
	};
static SWork* gWorkHead;
static SWork* gWorkTail;
static void (*gDeliver)();

static void Enqueue(SWork& aW)
	{
		{
		TLockGuard g;
		aW.iNext = NULL;
		if (gWorkTail)
			gWorkTail->iNext = &aW;
		else
			gWorkHead = &aW;
		gWorkTail = &aW;
		}
	phe_usb_kick();
	}

// run aFn on the USB thread and wait for it (the client thread; the function sets iR)
static TInt RunSync(void (*aFn)(SWork&), TAny* aA = NULL, TAny* aB = NULL, TAny* aC = NULL, TInt aI = 0)
	{
	SWork w;
	w.iFn = aFn; w.iA = aA; w.iB = aB; w.iC = aC; w.iI = aI; w.iR = KErrGeneral;
	NKern::FSSetOwner(&w.iSem, 0);
	Enqueue(w);
	NKern::FSWait(&w.iSem);
	return w.iR;
	}
static void DoneSync(SWork& aW) { NKern::FSSignal(&aW.iSem); }

static void DeliverEvents();				// below (hub channel)

// the work hook: the USB thread, after every service pass
static void WorkHook()
	{
	for (;;)
		{
		SWork* w;
			{
			TLockGuard g;
			w = gWorkHead;
			if (w)
				{
				gWorkHead = w->iNext;
				if (!gWorkHead)
					gWorkTail = NULL;
				}
			}
		if (!w)
			break;
		w->iFn(*w);
		}
	DeliverEvents();
	}

// ---- the hub driver channel -----------------------------------------------------------------------------------------

class DHubChannel : public DLogicalChannelBase
	{
public:
	DHubChannel() : iClient(NULL), iWaitStatus(NULL), iWaitEvent(NULL), iStateStatus(NULL) {}
	~DHubChannel();
	virtual TInt DoCreate(TInt aUnit, const TDesC8* aInfo, const TVersion& aVer);
	virtual TInt Request(TInt aReqNo, TAny* a1, TAny* a2);
	void Deliver();							// USB thread
	TInt Control(TInt aFn, TAny* a1, TAny* a2);
	void CancelWait(TInt aReason);
public:
	DThread* iClient;
	TRequestStatus* iWaitStatus;
	RUsbHubDriver::TBusEvent* iWaitEvent;
	TRequestStatus* iStateStatus;
	};
static DHubChannel* gHub;

class DHubFactory : public DLogicalDevice
	{
public:
	DHubFactory() { iVersion = RUsbHubDriver::VersionRequired(); iParseMask = 0; iUnitsMask = 0; }
	virtual TInt Install() { return SetName(&KHubName); }
	virtual void GetCaps(TDes8& aDes) const { aDes.FillZ(aDes.MaxLength()); }
	virtual TInt Create(DLogicalChannelBase*& aChannel)
		{
		aChannel = new DHubChannel;
		return aChannel ? KErrNone : KErrNoMemory;
		}
	};

TInt DHubChannel::DoCreate(TInt, const TDesC8*, const TVersion& aVer)
	{
	if (!Kern::QueryVersionSupported(RUsbHubDriver::VersionRequired(), aVer))
		return KErrNotSupported;
	if (gHub)
		return KErrInUse;					// one client (the Function Driver Framework)
	iClient = &Kern::CurrentThread();
	iClient->Open();
	gHub = this;
	return KErrNone;
	}

DHubChannel::~DHubChannel()
	{
	if (gHub == this)
		gHub = NULL;
	if (iClient)
		Kern::SafeClose((DObject*&)iClient, NULL);
	}

// the client of a request: write what it asked for and complete it (a thread context in a critical section)
static void CompleteWith(DThread* aClient, TRequestStatus*& aStatus, TInt aReason)
	{
	Kern::RequestComplete(aClient, aStatus, aReason);
	}

void DHubChannel::Deliver()
	{
	if (!iWaitStatus)
		return;
	SEvent ev;
		{
		TLockGuard g;
		if (!gEvCount)
			return;
		ev = gEvents[gEvHead];
		gEvHead = (gEvHead + 1) % KMaxEvents;
		--gEvCount;
		}
	RUsbHubDriver::TBusEvent out;
	out.iEventType = (RUsbHubDriver::TBusEvent::TEvent)ev.iType;
	out.iDeviceHandle = ev.iHandle;
	out.iError = KErrNone;
	out.iReason = 0;
	TRequestStatus* st = iWaitStatus;
	iWaitStatus = NULL;
	NKern::ThreadEnterCS();
	Kern::ThreadRawWrite(iClient, iWaitEvent, &out, sizeof(out));
	CompleteWith(iClient, st, KErrNone);
	NKern::ThreadLeaveCS();
	}

static void DeliverEvents()
	{
	if (gHub)
		gHub->Deliver();
	}

void DHubChannel::CancelWait(TInt aReason)
	{
	TRequestStatus* st = iWaitStatus;
	iWaitStatus = NULL;
	if (st)
		{
		NKern::ThreadEnterCS();
		CompleteWith(iClient, st, aReason);
		NKern::ThreadLeaveCS();
		}
	}

// GET_DESCRIPTOR (string) on endpoint 0, run on the USB thread: iA = device, iB = buffer (255), iI = index | langid << 16
static void StringWork(SWork& aW)
	{
	SDev* d = (SDev*)aW.iA;
	TUint index = (TUint)aW.iI & 0xFFFF, lang = ((TUint)aW.iI >> 16) & 0xFFFF;
	if (!d->iPresent)
		aW.iR = KErrNotFound;
	else
		{
		TUint act = 0;
		TInt e = phe_usb_ctrl(d->iDev, 0x80, 6, (TUint16)((3 << 8) | index), (TUint16)lang, aW.iB, 255, &act);
		aW.iR = e ? MapError(e) : (TInt)act;	// the length read
		}
	DoneSync(aW);
	}

// the string descriptor of device d into the client's descriptor aTarget (a TDes8 of at least 255)
static TInt GetString(SDev* aD, TDes8* aTarget, TInt aIndex, TInt aLang)
	{
	TUint8* buf = (TUint8*)Kern::Alloc(256);
	if (!buf)
		return KErrNoMemory;
	TInt r = RunSync(StringWork, aD, buf, NULL, (aIndex & 0xFFFF) | ((aLang & 0xFFFF) << 16));
	if (r >= 0)
		{
		Kern::InfoCopy(*aTarget, buf, r);
		r = KErrNone;
		}
	Kern::Free(buf);
	return r;
	}

TInt DHubChannel::Control(TInt aFn, TAny* a1, TAny* a2)
	{
	switch (aFn)
		{
	case RUsbHubDriver::EStartHost:
	case RUsbHubDriver::EStopHost:
		return KErrNone;					// the stack is already running
	case RUsbDevice::EOpen:
		{
		TLockGuard g;
		SDev* d = DevByHandle((TUint)a1);
		if (!d || !d->iPresent)
			return KErrNotFound;
		++d->iOpen;
		return KErrNone;
		}
	case RUsbDevice::EClose:
		{
		TLockGuard g;
		SDev* d = DevByHandle((TUint)a1);
		if (d && d->iOpen > 0)
			--d->iOpen;
		return KErrNone;
		}
	case RUsbDevice::EGetDeviceDescriptor:
		{
		SDev* d = DevByHandle((TUint)a1);
		if (!d)
			return KErrNotFound;
		Kern::InfoCopy(*(TDes8*)a2, d->iDd, 18);
		return KErrNone;
		}
	case RUsbDevice::EGetConfigurationDescriptorSize:
		{
		SDev* d = DevByHandle((TUint)a1);
		if (!d)
			return KErrNotFound;
		TInt n = d->iCfgLen;
		kumemput32(a2, &n, sizeof(n));
		return KErrNone;
		}
	case RUsbDevice::EGetConfigurationDescriptor:
		{
		SDev* d = DevByHandle((TUint)a1);
		if (!d)
			return KErrNotFound;
		Kern::InfoCopy(*(TDes8*)a2, d->iCfg, d->iCfgLen);
		return KErrNone;
		}
	case RUsbDevice::EGetStringDescriptor:
		{
		SDev* d = DevByHandle((TUint)a1);
		if (!d)
			return KErrNotFound;
		RUsbDevice::TStringDescParams p;
		kumemget32(&p, a2, sizeof(p));
		return GetString(d, p.iTarget, p.iIndex, p.iLangId);
		}
	case RUsbDevice::EGetInterfaceToken:
		{
		SDev* d = DevByHandle((TUint)a1);
		if (!d || !d->iPresent)
			return KErrNotFound;
		RUsbDevice::TInterfaceTokenParameters p;
		kumemget32(&p, a2, sizeof(p));
		for (TInt o = 0; o + 2 <= d->iCfgLen && d->iCfg[o] >= 2; o += d->iCfg[o])
			if (d->iCfg[o + 1] == 4 && o + 9 <= d->iCfgLen && d->iCfg[o + 2] == p.iInterfaceNumber)
				{
				TUint32 token = (d->iHandle << 8) | (TUint32)p.iInterfaceNumber;
				kumemput32(p.iToken, &token, sizeof(token));
				return KErrNone;
				}
		return KErrNotFound;
		}
	case RUsbDevice::ESuspend:
	case RUsbDevice::EResume:
		return KErrNone;					// not implemented: the device stays active
	case RUsbDevice::ECancelDeviceStateChange:
		{
		TRequestStatus* st = iStateStatus;
		iStateStatus = NULL;
		if (st)
			{
			NKern::ThreadEnterCS();
			CompleteWith(iClient, st, KErrCancel);
			NKern::ThreadLeaveCS();
			}
		return KErrNone;
		}
	default:
		return KErrNotSupported;
		}
	}

TInt DHubChannel::Request(TInt aReqNo, TAny* a1, TAny* a2)
	{
	if (aReqNo == KMaxTInt)					// DoCancel(a1 = mask)
		{
		if ((TUint)a1 & RUsbHubDriver::ECancelWaitForBusEvent)
			CancelWait(KErrCancel);
		return KErrNone;
		}
	if (aReqNo >= 0)
		return Control(aReqNo, a1, a2);
	TRequestStatus* st = (TRequestStatus*)a1;
	TAny* args[2];
	kumemget32(args, a2, sizeof(args));
	switch (~aReqNo)
		{
	case RUsbHubDriver::EWaitForBusEvent:
		if (iWaitStatus)
			{
			NKern::ThreadEnterCS();
			CompleteWith(&Kern::CurrentThread(), st, KErrInUse);
			NKern::ThreadLeaveCS();
			return KErrNone;
			}
		iWaitEvent = (RUsbHubDriver::TBusEvent*)args[0];
		iWaitStatus = st;
		phe_usb_kick();						// events already queued are delivered by the USB thread
		return KErrNone;
	case RUsbDevice::EDeviceStateChange:
		iStateStatus = st;					// suspend / resume are not implemented: never completes
		return KErrNone;
	default:
		NKern::ThreadEnterCS();
		CompleteWith(&Kern::CurrentThread(), st, KErrNotSupported);
		NKern::ThreadLeaveCS();
		return KErrNone;
		}
	}

// ---- the interface channel ------------------------------------------------------------------------------------------

struct SPipe
	{
	phe_usb_pipe* iPipe;
	TUint iEp;
	DThread* iClient;						// of the running transfer
	TRequestStatus* iStatus;
	TInt iHeaderOff;
	TInt iDataLen;
	TBool iIn;
	};

class DUsbiChannel : public DLogicalChannelBase
	{
public:
	DUsbiChannel() : iHandle(0), iClient(NULL), iChunk(NULL), iChunkBase(0), iChunkSize(0), iSuspend(NULL)
		{ memclr(iPipes, sizeof(iPipes)); }
	~DUsbiChannel();
	virtual TInt DoCreate(TInt aUnit, const TDesC8* aInfo, const TVersion& aVer);
	virtual TInt Request(TInt aReqNo, TAny* a1, TAny* a2);
	TInt Control(TInt aFn, TAny* a1, TAny* a2);
	void IssueTransfer(TInt aPipe, TInt aHeader, TRequestStatus* aStatus);
public:
	enum { KMaxPipes = 8 };
	TUint iHandle;
	TInt iIface;
	TInt iSliceOff, iSliceLen;				// the interface's part of the configuration descriptor
	DThread* iClient;
	DChunk* iChunk;
	TLinAddr iChunkBase;
	TInt iChunkSize;
	SPipe iPipes[KMaxPipes];
	TRequestStatus* iSuspend;
	};

class DUsbiFactory : public DLogicalDevice
	{
public:
	DUsbiFactory() { iVersion = RUsbInterface::VersionRequired(); iParseMask = 0; iUnitsMask = 0; }
	virtual TInt Install() { return SetName(&KUsbdiName); }
	virtual void GetCaps(TDes8& aDes) const { aDes.FillZ(aDes.MaxLength()); }
	virtual TInt Create(DLogicalChannelBase*& aChannel)
		{
		aChannel = new DUsbiChannel;
		return aChannel ? KErrNone : KErrNoMemory;
		}
	};

TInt DUsbiChannel::DoCreate(TInt, const TDesC8* aInfo, const TVersion& aVer)
	{
	if (!Kern::QueryVersionSupported(RUsbInterface::VersionRequired(), aVer))
		return KErrNotSupported;
	TUint32 token = 0;
	TPtr8 tp((TUint8*)&token, 0, sizeof(token));
	if (!aInfo)
		return KErrArgument;
	Kern::KUDesGet(tp, *aInfo);
	if (tp.Length() != (TInt)sizeof(token))
		return KErrArgument;
	TLockGuard g;
	SDev* d = DevByHandle(token >> 8);
	if (!d || !d->iPresent)
		return KErrNotFound;
	iHandle = d->iHandle;
	iIface = token & 0xFF;
	// the slice: from the first descriptor of the interface through the descriptor before the next interface number
	TInt start = -1, end = d->iCfgLen;
	for (TInt o = 0; o + 2 <= d->iCfgLen && d->iCfg[o] >= 2; o += d->iCfg[o])
		if (d->iCfg[o + 1] == 4 && o + 9 <= d->iCfgLen)
			{
			if (d->iCfg[o + 2] == iIface && start < 0)
				start = o;
			else if (d->iCfg[o + 2] != iIface && start >= 0)
				{
				end = o;
				break;
				}
			}
	if (start < 0)
		return KErrNotFound;
	iSliceOff = start;
	iSliceLen = end - start;
	++d->iOpen;
	iClient = &Kern::CurrentThread();
	return KErrNone;
	}

static void ClosePipeWork(SWork& aW)
	{
	SPipe* p = (SPipe*)aW.iA;
	if (p->iPipe)
		{
		phe_usb_pipe_abort(p->iPipe);
		phe_usb_pipe_close(p->iPipe);
		p->iPipe = NULL;
		}
	DoneSync(aW);
	}

DUsbiChannel::~DUsbiChannel()
	{
	for (TInt i = 0; i < KMaxPipes; ++i)
		if (iPipes[i].iPipe)
			RunSync(ClosePipeWork, &iPipes[i]);
	if (iChunk)
		Kern::ChunkClose(iChunk);
	TLockGuard g;
	SDev* d = DevByHandle(iHandle);
	if (d && d->iOpen > 0)
		--d->iOpen;
	}

static void SetAltWork(SWork& aW)
	{
	SDev* d = (SDev*)aW.iA;
	aW.iR = d->iPresent ? (phe_usb_set_alt(d->iDev, (TInt)aW.iB, aW.iI) ? KErrArgument : KErrNone) : KErrNotFound;
	DoneSync(aW);
	}

static void OpenPipeWork(SWork& aW)
	{
	SDev* d = (SDev*)aW.iA;
	SPipe* p = (SPipe*)aW.iC;
	if (!d->iPresent)
		aW.iR = KErrNotFound;
	else
		{
		p->iPipe = phe_usb_pipe_open(d->iDev, (TInt)aW.iB, (TUint)aW.iI, KPipeBuf);
		aW.iR = p->iPipe ? KErrNone : KErrArgument;
		}
	DoneSync(aW);
	}

static void ClearStallWork(SWork& aW)
	{
	SPipe* p = (SPipe*)aW.iA;
	aW.iR = p->iPipe && phe_usb_pipe_clear_stall(p->iPipe) == 0 ? KErrNone : KErrGeneral;
	DoneSync(aW);
	}

static void AbortWork(SWork& aW)
	{
	SPipe* p = (SPipe*)aW.iA;
	if (p->iPipe)
		phe_usb_pipe_abort(p->iPipe);
	DoneSync(aW);
	}

TInt DUsbiChannel::Control(TInt aFn, TAny* a1, TAny* a2)
	{
	SDev* d = DevByHandle(iHandle);
	if (!d)
		return KErrNotFound;
	switch (aFn)
		{
	case RUsbInterface::EGetInterfaceDescriptorSize:
		kumemput32(a1, &iSliceLen, sizeof(TInt));
		return KErrNone;
	case RUsbInterface::EGetInterfaceDescriptor:
		Kern::InfoCopy(*(TDes8*)a1, d->iCfg + iSliceOff, iSliceLen);
		return KErrNone;
	case RUsbInterface::EGetStringDescriptor:
		{
		TUint32 v;
		kumemget32(&v, a2, sizeof(v));
		return GetString(d, (TDes8*)a1, v & 0xFFFF, v >> 16);
		}
	case RUsbInterface::ESelectAlternateInterface:
		return RunSync(SetAltWork, d, (TAny*)iIface, NULL, (TInt)a1);
	case RUsbInterface::EPermitRemoteWakeup:
		return KErrNone;
	case RUsbInterface::EGetDeviceSpeed:
		{
		TInt s = d->iSpeed;
		kumemput32(a1, &s, sizeof(s));
		return KErrNone;
		}
	case RUsbInterface::EGetBusId:
		{
		TUint32 b = d->iBus;
		kumemput32(a1, &b, sizeof(b));
		return KErrNone;
		}
	case RUsbInterface::EHcdPageSize:
		{
		TInt s = 4096;
		kumemput32(a1, &s, sizeof(s));
		return KErrNone;
		}
	case RUsbInterface::EGetSizeAndAlignment:
		{
		RUsbInterface::TTransferMemoryDetails t;
		kumemget32(&t, a1, sizeof(t));
		t.iAlignment = 4;					// the stack copies: no DMA alignment is needed
		kumemput32(a1, &t, sizeof(t));
		return KErrNone;
		}
	case RUsbInterface::EAllocChunk:
		{
		if (iChunk)
			return KErrAlreadyExists;
		RUsbInterface::TChunkRequestDetails cd;
		kumemget32(&cd, a1, sizeof(cd));
		TInt size = (cd.iRequestSize + 0xFFF) & ~0xFFF;
		TChunkCreateInfo info;
		info.iType = TChunkCreateInfo::ESharedKernelSingle;
		info.iMaxSize = size;
		info.iMapAttr = EMapAttrFullyBlocking;
		info.iOwnsMemory = ETrue;
		info.iDestroyedDfc = NULL;
		TUint32 mapAttr;
		TLinAddr kern;
		NKern::ThreadEnterCS();
		TInt r = Kern::ChunkCreate(info, iChunk, kern, mapAttr);
		if (r == KErrNone)
			r = Kern::ChunkCommit(iChunk, 0, size);
		if (r != KErrNone && iChunk)
			{
			Kern::ChunkClose(iChunk);
			iChunk = NULL;
			}
		NKern::ThreadLeaveCS();
		if (r != KErrNone)
			return r;
		iChunkBase = kern;
		iChunkSize = size;
		NKern::ThreadEnterCS();
		TInt h = Kern::MakeHandleAndOpen(NULL, iChunk);
		NKern::ThreadLeaveCS();
		if (h < 0)
			return h;
		TInt off = 0;
		kumemput32(cd.iChunkHandle, &h, sizeof(h));
		kumemput32(cd.iOffset, &off, sizeof(off));
		return KErrNone;
		}
	case RUsbInterface::EOpenPipe:
		{
		TUint ep = (TUint)a2 & 0xFF;
		TInt slot = -1;
		for (TInt i = 0; i < KMaxPipes && slot < 0; ++i)
			if (!iPipes[i].iPipe)
				slot = i;
		if (slot < 0)
			return KErrOverflow;
		TInt r = RunSync(OpenPipeWork, d, (TAny*)iIface, &iPipes[slot], (TInt)ep);
		if (r != KErrNone)
			return r;
		iPipes[slot].iEp = ep;
		iPipes[slot].iIn = (ep & 0x80) != 0;
		TUint32 h = slot + 1;
		kumemput32(a1, &h, sizeof(h));
		return KErrNone;
		}
	case RUsbPipe::EClose:
	case RUsbPipe::EAbort:
	case RUsbPipe::EClearRemoteStall:
	case RUsbPipe::EGetEndpointId:
		{
		TInt slot = (TInt)a1 - 1;
		if (slot < 0 || slot >= KMaxPipes || !iPipes[slot].iPipe)
			return KErrNotFound;
		if (aFn == RUsbPipe::EClose)
			return RunSync(ClosePipeWork, &iPipes[slot]);
		if (aFn == RUsbPipe::EAbort)
			return RunSync(AbortWork, &iPipes[slot]);
		if (aFn == RUsbPipe::EClearRemoteStall)
			return RunSync(ClearStallWork, &iPipes[slot]);
		TUint64 id = ((TUint64)d->iHandle << 8) | iPipes[slot].iEp;
		kumemput32(a2, &id, sizeof(id));
		return KErrNone;
		}
	default:
		return KErrNotSupported;
		}
	}

// ---- a bulk / interrupt transfer from the shared chunk, and endpoint 0 transfers ------------------------------------

// the header of a transfer in the chunk (zerocopymetadata.h): TTransferType, DataOffset, DataLength, DataMaxLength, ZlpStatus
static const TInt KHdrType = 0, KHdrDataOff = 4, KHdrDataLen = 8, KHdrDataMax = 12, KHdrZlp = 16;

static void TransferDone(void* aCtx, int aErr, unsigned int aActual)
	{
	SPipe* p = (SPipe*)aCtx;
	DUsbiChannel* ch = (DUsbiChannel*)p->iClient;		// (the channel is kept in iClient while a transfer runs)
	if (!p->iStatus)
		return;
	TRequestStatus* st = p->iStatus;
	p->iStatus = NULL;
	if (p->iIn && aErr == 0)
		*(TInt*)(ch->iChunkBase + p->iHeaderOff + KHdrDataLen) = (TInt)aActual;
	DThread* client = ch->iClient;
	NKern::ThreadEnterCS();
	Kern::RequestComplete(client, st, MapError(aErr));
	NKern::ThreadLeaveCS();
	}

static void IssueWork(SWork& aW)
	{
	DUsbiChannel* ch = (DUsbiChannel*)aW.iA;
	SPipe* p = (SPipe*)aW.iB;
	TRequestStatus* st = (TRequestStatus*)aW.iC;
	SDev* d = DevByHandle(ch->iHandle);
	if (!d || !d->iPresent)
		{
		NKern::ThreadEnterCS();
		Kern::RequestComplete(ch->iClient, st, KErrNotFound);
		NKern::ThreadLeaveCS();
		return;
		}
	TLinAddr hdr = ch->iChunkBase + aW.iI;
	TInt dataOff = *(TInt*)(hdr + KHdrDataOff);
	TInt len = p->iIn ? *(TInt*)(hdr + KHdrDataMax) : *(TInt*)(hdr + KHdrDataLen);
	TInt zlp = *(TInt*)(hdr + KHdrZlp);
	p->iStatus = st;
	p->iHeaderOff = aW.iI;
	p->iClient = (DThread*)ch;
	if (phe_usb_pipe_start(p->iPipe, (void*)(ch->iChunkBase + dataOff), len, !p->iIn && zlp == RUsbTransferDescriptor::EAlwaysSendZlp,
			TransferDone, p) != 0)
		{
		p->iStatus = NULL;
		NKern::ThreadEnterCS();
		Kern::RequestComplete(ch->iClient, st, KErrInUse);
		NKern::ThreadLeaveCS();
		}
	}

struct SEp0
	{
	DUsbiChannel* iCh;
	TRequestStatus* iStatus;
	TDes8* iRecv;						// the client's
	TUint8 iBuf[512];
	TInt iSendLen;
	RUsbInterface::TUsbTransferRequestDetails iD;
	};

static void Ep0Work(SWork& aW)
	{
	SEp0* e = (SEp0*)aW.iA;
	SDev* d = DevByHandle(e->iCh->iHandle);
	TInt r = KErrNotFound;
	TUint act = 0;
	TBool in = (e->iD.iRequestType & 0x80) != 0;
	if (d && d->iPresent)
		{
		TInt e2 = phe_usb_ctrl(d->iDev, e->iD.iRequestType, e->iD.iRequest, e->iD.iValue, e->iD.iIndex, e->iBuf,
				in ? sizeof(e->iBuf) : e->iSendLen, &act);
		r = MapError(e2);
		}
	NKern::ThreadEnterCS();
	if (in && r == KErrNone && e->iRecv)
		{
		TPtrC8 got(e->iBuf, act);
		Kern::ThreadDesWrite(e->iCh->iClient, e->iRecv, got, 0, NULL);
		}
	Kern::RequestComplete(e->iCh->iClient, e->iStatus, r);
	Kern::Free(e);
	Kern::Free(&aW);
	NKern::ThreadLeaveCS();
	}

TInt DUsbiChannel::Request(TInt aReqNo, TAny* a1, TAny* a2)
	{
	if (aReqNo == KMaxTInt)					// DoCancel
		{
		if (((TUint)a1 & RUsbInterface::ECancelSuspend) || ((TUint)a1 & RUsbInterface::ECancelWaitForResume))
			{
			TRequestStatus* st = iSuspend;
			iSuspend = NULL;
			if (st)
				{
				NKern::ThreadEnterCS();
				Kern::RequestComplete(iClient, st, KErrCancel);
				NKern::ThreadLeaveCS();
				}
			}
		return KErrNone;
		}
	if (aReqNo >= 0)
		return Control(aReqNo, a1, a2);
	TRequestStatus* st = (TRequestStatus*)a1;
	TAny* args[2];
	kumemget32(args, a2, sizeof(args));
	iClient = &Kern::CurrentThread();
	TInt r = KErrNotSupported;
	switch (~aReqNo)
		{
	case RUsbInterface::ESuspend:
		iSuspend = st;						// never completes: suspend is not implemented
		return KErrNone;
	case RUsbInterface::EEp0Transfer:
		{
		NKern::ThreadEnterCS();
		SEp0* e = (SEp0*)Kern::AllocZ(sizeof(SEp0));
		NKern::ThreadLeaveCS();
		if (!e)
			{ r = KErrNoMemory; break; }
		kumemget32(&e->iD, args[0], sizeof(e->iD));
		e->iCh = this;
		e->iStatus = st;
		e->iRecv = e->iD.iRecv;
		e->iSendLen = 0;
		if (e->iD.iSend)
			{
			TPtr8 sp(e->iBuf, 0, sizeof(e->iBuf));
			Kern::KUDesGet(sp, *e->iD.iSend);
			e->iSendLen = sp.Length();
			}
		NKern::ThreadEnterCS();
		SWork* w = (SWork*)Kern::AllocZ(sizeof(SWork));
		NKern::ThreadLeaveCS();
		if (!w)
			{ r = KErrNoMemory; break; }
		w->iFn = Ep0Work; w->iA = e;
		Enqueue(*w);
		return KErrNone;
		}
	case RUsbPipe::EIssueTransfer:
		{
		TInt slot = (TInt)args[0] - 1;
		if (slot < 0 || slot >= KMaxPipes || !iPipes[slot].iPipe || !iChunk)
			{ r = KErrNotFound; break; }
		NKern::ThreadEnterCS();
		SWork* w = (SWork*)Kern::AllocZ(sizeof(SWork));
		NKern::ThreadLeaveCS();
		if (!w)
			{ r = KErrNoMemory; break; }
		w->iFn = IssueWork; w->iA = this; w->iB = &iPipes[slot]; w->iC = st; w->iI = (TInt)args[1];
		Enqueue(*w);
		return KErrNone;
		}
	default:
		break;
		}
	NKern::ThreadEnterCS();
	Kern::RequestComplete(&Kern::CurrentThread(), st, r);
	NKern::ThreadLeaveCS();
	return KErrNone;
	}

// ---- installation ---------------------------------------------------------------------------------------------------

TInt UsbhostLddInstall()
	{
	DLogicalDevice* hub = new DHubFactory;
	DLogicalDevice* usbi = new DUsbiFactory;
	if (!hub || !usbi)
		return KErrNoMemory;
	phe_usb_set_notify(OnDevice, NULL);
	phe_usb_set_work_hook(WorkHook);
	TInt r = Kern::InstallLogicalDevice(hub);
	if (r == KErrNone)
		r = Kern::InstallLogicalDevice(usbi);
	Kern::Printf("[usbh] USBHUBDRIVER + USBDI logical devices installed -> %d", r);
	return r;
	}
