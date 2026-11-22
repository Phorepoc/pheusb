// Copyright The Phorepoc Authors
// SPDX-License-Identifier: BSD-2-Clause

// phe_usb_hid_ekern.cpp -- the HID client's events (phe_usb_hid.h) as EKA2 raw events: keys through a HID-usage ->
// EStdKey table (the Keyboard/Keypad page, HID Usage Tables 1.12 section 10), pointers as EPointerMove and
// EButton1..3Down/Up in screen pixels. Kern::AddEvent runs on the USB thread (phe_usb_service), as the i8042 driver
// calls it from its DFC.
//
// The board starts it with PheUsbHidStart(width, height, remap): the screen the pointer maps to, and an optional hook that
// turns a standard key into the board's own (a PC board's F9 = the phone's power key, F10 = its Menu key).
#include <kernel/kernel.h>
#include <e32keys.h>

extern "C" {
#include "phe_usb_hid.h"
}

typedef TInt (*TPheUsbKeyRemap)(TInt aStdKey);

static TInt gW = 360, gH = 640;
static TPheUsbKeyRemap gRemap;
static TInt gX, gY;
static TUint gButtons;

static TInt StdKey(TUint aUsage)
	{
	if (aUsage >= 0x04 && aUsage <= 0x1D)
		return 'A' + (aUsage - 0x04);
	if (aUsage >= 0x1E && aUsage <= 0x26)
		return '1' + (aUsage - 0x1E);
	if (aUsage >= 0x3A && aUsage <= 0x45)
		return EStdKeyF1 + (aUsage - 0x3A);
	if (aUsage >= 0x59 && aUsage <= 0x61)
		return EStdKeyNkp1 + (aUsage - 0x59);
	switch (aUsage)
		{
	case 0x27: return '0';
	case 0x28: return EStdKeyEnter;
	case 0x29: return EStdKeyEscape;
	case 0x2A: return EStdKeyBackspace;
	case 0x2B: return EStdKeyTab;
	case 0x2C: return EStdKeySpace;
	case 0x2D: return EStdKeyMinus;
	case 0x2E: return EStdKeyEquals;
	case 0x2F: return EStdKeySquareBracketLeft;
	case 0x30: return EStdKeySquareBracketRight;
	case 0x31: return EStdKeyBackSlash;
	case 0x33: return EStdKeySemiColon;
	case 0x34: return EStdKeySingleQuote;
	case 0x36: return EStdKeyComma;
	case 0x37: return EStdKeyFullStop;
	case 0x38: return EStdKeyForwardSlash;
	case 0x39: return EStdKeyCapsLock;
	case 0x4A: return EStdKeyHome;
	case 0x4B: return EStdKeyPageUp;
	case 0x4C: return EStdKeyDelete;
	case 0x4D: return EStdKeyEnd;
	case 0x4E: return EStdKeyPageDown;
	case 0x4F: return EStdKeyRightArrow;
	case 0x50: return EStdKeyLeftArrow;
	case 0x51: return EStdKeyDownArrow;
	case 0x52: return EStdKeyUpArrow;
	case 0x58: return EStdKeyNkpEnter;
	case 0x62: return EStdKeyNkp0;
	case 0x63: return EStdKeyNkpFullStop;
	case 0x80: return EStdKeyIncVolume;
	case 0x81: return EStdKeyDecVolume;
	case 0xE0: return EStdKeyLeftCtrl;
	case 0xE1: return EStdKeyLeftShift;
	case 0xE2: return EStdKeyLeftAlt;
	case 0xE3: return EStdKeyLeftFunc;
	case 0xE4: return EStdKeyRightCtrl;
	case 0xE5: return EStdKeyRightShift;
	case 0xE6: return EStdKeyRightAlt;
	case 0xE7: return EStdKeyRightFunc;
	default:   return EStdKeyNull;
		}
	}

static void Add(TRawEvent::TType aType, TInt aA, TInt aB)
	{
	TRawEvent e;
	if (aType == TRawEvent::EKeyDown || aType == TRawEvent::EKeyUp)
		e.Set(aType, aA, 0);
	else
		e.Set(aType, aA, aB, 0);				// Z = 0 explicitly (Set(x, y) leaves it uninitialised)
	Kern::AddEvent(e);
	}

static TInt gTraced;

static void OnKey(void*, unsigned int aUsage, int aDown)
	{
	TInt k = StdKey(aUsage);
	if (gRemap && k != EStdKeyNull)
		k = gRemap(k);
	if (gTraced < 8)
		{
		++gTraced;
		Kern::Printf("[usbh] hid key %02x -> %x %s", aUsage, k, aDown ? "down" : "up");
		}
	if (k != EStdKeyNull)
		Add(aDown ? TRawEvent::EKeyDown : TRawEvent::EKeyUp, k, 0);
	}

static void Buttons(TUint aNow)
	{
	static const TRawEvent::TType KDown[3] = { TRawEvent::EButton1Down, TRawEvent::EButton3Down, TRawEvent::EButton2Down };
	static const TRawEvent::TType KUp[3]   = { TRawEvent::EButton1Up,   TRawEvent::EButton3Up,   TRawEvent::EButton2Up };
	for (TInt i = 0; i < 3; ++i)				// HID: bit 0 left, 1 right, 2 middle -> button 1, 3, 2
		if ((aNow ^ gButtons) & (1u << i))
			{
			Add((aNow & (1u << i)) ? KDown[i] : KUp[i], gX, gY);
			if (gTraced < 16)
				{
				++gTraced;
				Kern::Printf("[usbh] hid button %d %s at (%d,%d)", i + 1, (aNow & (1u << i)) ? "down" : "up", gX, gY);
				}
			}
	gButtons = aNow;
	}

static void MoveTo(TInt aX, TInt aY)
	{
	aX = aX < 0 ? 0 : (aX >= gW ? gW - 1 : aX);
	aY = aY < 0 ? 0 : (aY >= gH ? gH - 1 : aY);
	if (aX != gX || aY != gY)
		{
		gX = aX; gY = aY;
		Add(TRawEvent::EPointerMove, gX, gY);
		}
	}

static void OnRel(void*, int aDx, int aDy, unsigned int aButtons)
	{
	MoveTo(gX + aDx, gY + aDy);
	Buttons(aButtons);
	}

static void OnAbs(void*, unsigned int aX, unsigned int aXmax, unsigned int aY, unsigned int aYmax, unsigned int aButtons)
	{
	MoveTo(aXmax ? (TInt)((TUint64)aX * (gW - 1) / aXmax) : 0, aYmax ? (TInt)((TUint64)aY * (gH - 1) / aYmax) : 0);
	Buttons(aButtons);
	}

static const phe_usb_hid_sink KSink = { &OnKey, &OnRel, &OnAbs, 0 };

void PheUsbHidStart(TInt aWidth, TInt aHeight, TPheUsbKeyRemap aRemap)
	{
	if (aWidth > 0 && aHeight > 0)
		{ gW = aWidth; gH = aHeight; }
	gRemap = aRemap;
	gX = gW / 2; gY = gH / 2;
	phe_usb_hid_set_sink(&KSink);
	Kern::Printf("[usbh] hid: keyboards and pointers -> raw events, screen %dx%d", gW, gH);
	}
