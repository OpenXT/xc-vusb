//
// Copyright (c) Citrix Systems, Inc.
//
/// @file xenif.h xen ringbuffer and bus interface definitions.
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

typedef struct XEN_INTERFACE * PXEN_INTERFACE;
typedef struct USB_FDO_CONTEXT *PUSB_FDO_CONTEXT;

typedef VOID EVTCHN_HANDLER_CB(VOID *Context);
typedef EVTCHN_HANDLER_CB *PEVTCHN_HANDLER_CB;

PXEN_INTERFACE
AllocateXenInterface(
    PUSB_FDO_CONTEXT fdoContext);

VOID
DeallocateXenInterface(
    IN PXEN_INTERFACE Xen);

NTSTATUS
XenDeviceInitialize(
    IN PXEN_INTERFACE Xen,
    IN PEVTCHN_HANDLER_CB DpcCallback);

NTSTATUS
XenDeviceConnectBackend(
    IN PXEN_INTERFACE Xen);

VOID
XenScheduleDPC(
    IN PXEN_INTERFACE Xen);

VOID
XenDisconnectDPC(
    IN PXEN_INTERFACE Xen);

VOID
XenDeviceDisconnectBackend(
    IN PXEN_INTERFACE Xen);

//
// Request Processing.
// The functions that have a request parameter must consume the request.
//

_Requires_lock_held_(fdoContext->WdfDevice)
VOID
PutUrbOnRing(
    IN PUSB_FDO_CONTEXT fdoContext,
    IN PWDF_USB_CONTROL_SETUP_PACKET packet,
    IN WDFREQUEST Request,
    IN USBD_PIPE_TYPE PipeType,
    IN UCHAR EndpointAddress,
    IN BOOLEAN hasData,
    IN BOOLEAN shortOk);

_Requires_lock_held_(fdoContext->WdfDevice)
VOID
PutResetOrCycleUrbOnRing(
    IN PUSB_FDO_CONTEXT fdoContext,
    IN WDFREQUEST Request,
    IN BOOLEAN IsReset);

_Requires_lock_held_(fdoContext->WdfDevice)
VOID
PutIsoUrbOnRing(
    IN PUSB_FDO_CONTEXT fdoContext,
    IN PWDF_USB_CONTROL_SETUP_PACKET packet,
    IN WDFREQUEST Request,
    IN UCHAR EndpointAddress,
    IN BOOLEAN transferAsap,
    IN BOOLEAN ShortOK);

NTSTATUS
PutScratchOnRing(
    IN PUSB_FDO_CONTEXT fdoContext,
    IN PWDF_USB_CONTROL_SETUP_PACKET packet,
    IN ULONG TransferLength,
    IN USBD_PIPE_TYPE PipeType,
    IN UCHAR EndpointAddress,
    IN BOOLEAN isReset);

_Requires_lock_held_(fdoContext->WdfDevice)
BOOLEAN
WaitForScratchPadAccess(
    IN PUSB_FDO_CONTEXT fdoContext);

NTSTATUS
WaitForScratchCompletion(
    IN PUSB_FDO_CONTEXT fdoContext);

//
// Interrupt Processing
//

_Requires_lock_held_(fdoContext->WdfDevice)
BOOLEAN
XenDpc(
    IN PUSB_FDO_CONTEXT fdoContext,
    IN WDFCOLLECTION RequestCollection);

//
// Xen Low level response processing.
//

NTSTATUS
XenPostProcessIsoResponse(
    IN PURB Urb,
    IN NTSTATUS *usbdStatus,
    IN ULONG bytesTransferred,
    IN ULONG startFrame,
    IN PVOID isoPacketDescriptor,
    IN NTSTATUS Status);

void 
FreeShadowForRequest(
    IN PXEN_INTERFACE Xen,
    WDFREQUEST Request);

void
CompleteRequestsFromShadow(
    IN PUSB_FDO_CONTEXT fdoContext);

//
// Misc Xen information functions
//

ULONG
MaxIsoPackets(
        IN PXEN_INTERFACE Xen);

ULONG
MaxIsoSegments(
    IN PXEN_INTERFACE Xen);

ULONG
MaxSegments(
    IN PXEN_INTERFACE Xen);

ULONG
OnRingBuffer(
    IN PXEN_INTERFACE Xen);

BOOLEAN
IndirectGrefs(
    IN PXEN_INTERFACE Xen);

ULONG
AvailableRequests(
    IN PXEN_INTERFACE Xen);

NTSTATUS 
MapUsbifToUsbdStatus(
    IN BOOLEAN  ResetInProgress,
    IN LONG UsbIfStatus,
    IN PCHAR * OutUsbIfString,
    IN PCHAR * OutUsbdString);

BOOLEAN
XenCheckOperationalState(
    IN PXEN_INTERFACE Xen);
BOOLEAN
XenCheckOnline(
    IN PXEN_INTERFACE Xen);

