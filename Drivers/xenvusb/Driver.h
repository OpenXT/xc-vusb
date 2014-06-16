//
// Copyright (c) Citrix Systems, Inc.
//
/// @file Driver.h USB Driver definitions.
//
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
// 
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
// 
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.
//
#define INITGUID

#include <ntddk.h>
#include <wdf.h>

#include "device.h"
#include "queue.h"
#include "trace.h"


#pragma warning(disable : 4127 ) //conditional expression is constant
#pragma warning(disable : 4200 ) // zero length array in struct

//
// WDFDRIVER Events
//
extern "C"
DRIVER_INITIALIZE DriverEntry;
EVT_WDF_OBJECT_CONTEXT_CLEANUP EvtDriverContextCleanup;

LONGLONG Uptime();

//
// light weight try finally
//
#ifndef TRY
#define TRY
#define LEAVE goto __tf_label__
#define FINALLY __tf_label__:
#endif

//
// Pooltags
//
#define XVU1 '1UVX' // scratchpad buffer.
#define XVU2 '2UVX' // AllocAndQueryPropertyString buffer.
#define XVU3 '3UVX' // USB_FDO_CONTEXT.ConfigData. (USB_CONFIG_INFO)
#define XVU4 '4UVX' // USB_CONFIG_INFO.m_configurationDescriptor
#define XVU5 '5UVX' // USB_CONFIG_INFO.m_interfaceDescriptors
#define XVU6 '6UVX' // USB_CONFIG_INFO.m_pipeDescriptors
#define XVU7 '7UVX' // GetOsDescriptorString compatids.
#define XVU8 '8UVX' // GetString USB_STRING.
#define XVU9 '9UVX' // XEN_INTERFACE.
#define XVUA 'AUVX' // usbif_shadow_ex_t array.
#define XVUB 'BUVX' // USHORT shadow free list array.
#define XVUC 'CUVX' // PutUrbOnRing indirectPageMemory.
#define XVUD 'DUVX' // PutIsoUrbOnRing iso packet buffer.
#define XVUE 'EUVX' // PutIsoUrbOnRing indirectPageMemory.
#define XVUF 'FUVX' // RootHubIfGetLocationString
#define XVUG 'GUVX' // RootHubIfFpAllocateWorkItem.
#define XVUH 'HUVX' // AllocateIrpWorkItem.
#define XVUI 'IUVX' // unused.
#define XVUJ 'JUVX' // unused.
#define XVUK 'KUVX' // unused.


extern BOOLEAN gVistaOrLater;
extern BOOLEAN gFakeNxprep;