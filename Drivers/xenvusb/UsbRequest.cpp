//
// Copyright (c) Citrix Systems, Inc.
//
/// @file UsbRequest.cpp USB high level request processing.
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
#include "Driver.h"
#include "Device.h"
#include "UsbConfig.h"
#include "usbioctl.h"
#include "xenif.h"
#include "UsbResponse.h"

_Requires_lock_held_(fdoContext->WdfDevice)
VOID
    ProcessGetDescriptor(
    IN UCHAR Recipient,
    IN PUSB_FDO_CONTEXT fdoContext,
    IN WDFREQUEST Request,
    IN PURB Urb);

_Requires_lock_held_(fdoContext->WdfDevice)
VOID
    ProcessSelectConfiguration(
    IN PUSB_FDO_CONTEXT fdoContext,
    IN WDFREQUEST Request,
    IN PURB Urb);

_Requires_lock_held_(fdoContext->WdfDevice)
VOID
    SetConfiguration(
    IN PUSB_FDO_CONTEXT fdoContext,
    IN WDFREQUEST Request,
    IN UCHAR configNumber);

_Requires_lock_held_(fdoContext->WdfDevice)
VOID
    ProcessSelectInterface(
    IN PUSB_FDO_CONTEXT fdoContext,
    IN WDFREQUEST Request,
    IN PURB Urb);

_Requires_lock_held_(fdoContext->WdfDevice)
VOID
ProcessBulkOrInterruptTransfer(
    IN PUSB_FDO_CONTEXT fdoContext,
    IN WDFREQUEST Request,
    IN PURB Urb);

_Requires_lock_held_(fdoContext->WdfDevice)
VOID
ProcessIsoRequest(
    IN PUSB_FDO_CONTEXT fdoContext,
    IN WDFREQUEST Request,
    IN PURB Urb);

_Requires_lock_held_(fdoContext->WdfDevice)
VOID
ProcessControlTransfer(
    IN PUSB_FDO_CONTEXT fdoContext,
    IN WDFREQUEST Request,
    IN PURB Urb);

_Requires_lock_held_(fdoContext->WdfDevice)
VOID
ProcessControlTransferEx(
    IN PUSB_FDO_CONTEXT fdoContext,
    IN WDFREQUEST Request,
    IN PURB Urb);

_Requires_lock_held_(fdoContext->WdfDevice)
VOID
ProcessClearOrSetFeature(
    IN PUSB_FDO_CONTEXT fdoContext,
    IN WDFREQUEST Request,
    IN PURB Urb,    
    IN UCHAR requestType,
    IN UCHAR request);

_Requires_lock_held_(fdoContext->WdfDevice)
VOID
ProcessClassOrVendorCommand(
    IN PUSB_FDO_CONTEXT fdoContext,
    IN WDFREQUEST Request,
    IN PURB Urb,
    IN ULONG Recipient,
    IN ULONG RequestType);

_Requires_lock_held_(fdoContext->WdfDevice)
VOID
ProcessGetStatus(
    IN PUSB_FDO_CONTEXT fdoContext,
    IN WDFREQUEST Request,
    IN PURB Urb,
    IN ULONG Recipient);

_Requires_lock_held_(fdoContext->WdfDevice)
VOID
ProcessSyncResetClearStall(
    IN PUSB_FDO_CONTEXT fdoContext,
    IN WDFREQUEST Request,
    IN PURB Urb);

_Requires_lock_held_(fdoContext->WdfDevice)
VOID
    ProcessAbortPipe(
    IN PUSB_FDO_CONTEXT fdoContext,
    IN WDFREQUEST Request,
    IN PURB Urb);

_Requires_lock_held_(fdoContext->WdfDevice)
VOID
    OsFeatureDescriptorCommand(
    IN PUSB_FDO_CONTEXT fdoContext,
    IN WDFREQUEST Request,
    IN PURB Urb);

EVT_WDF_WORKITEM AbortEndpointWorker;

EVT_WDF_WORKITEM AbortEndpointWaitWorker;

EVT_WDF_WORKITEM ResetDeviceWorker;

EVT_WDF_REQUEST_CANCEL  UsbIdleEvtRequestCancel;



/**
 * @brief Process a URB Request.
 * __Requirements__
 * *Must be called with FDO lock held.*
 * *Must send to DOM0 or re-queue or complete the Request*
 * Decodes the URB and completes it immediately if possible, otherwise invokes
 * URB specific routines for further processing.
 *
 * @param[in] fdoContext. The context for the FDO device.
 * @param[in] Request. The WDFREQUEST handle.
 * @param[in] Urb. The URB for the Request.
 *
 */
_Requires_lock_held_(fdoContext->WdfDevice)
VOID
SubmitUrb(
    IN PUSB_FDO_CONTEXT fdoContext,
    IN WDFREQUEST Request,
    IN PURB Urb)
{
    NTSTATUS Status = STATUS_UNSUCCESSFUL;

    TraceEvents(TRACE_LEVEL_VERBOSE, TRACE_URB,
        __FUNCTION__": %s Device %p Request %p URB %p\n",
        fdoContext->FrontEndPath,
        fdoContext->WdfDevice,
        Request,
        Urb);

    if (Urb->UrbHeader.Length < sizeof(_URB_HEADER))
    {
        RequestGetRequestContext(Request)->RequestCompleted = 1;
        ReleaseFdoLock(fdoContext);
        WdfRequestComplete(Request, STATUS_UNSUCCESSFUL);
        AcquireFdoLock(fdoContext);

        TraceEvents(TRACE_LEVEL_WARNING, TRACE_URB,
            __FUNCTION__": Bad URB header\n");

        return;
    }

    //
    // preset usb status to success!
    //
    Urb->UrbHeader.Status = USBD_STATUS_SUCCESS;

    PCHAR UrbFuncString = UrbFunctionToString(Urb->UrbHeader.Function);

    /* --XT-- This might be the source of so much of the info tracing, taking it out...
    ULONG traceLevel = TRACE_LEVEL_VERBOSE;
    if ((Urb->UrbHeader.Function != URB_FUNCTION_BULK_OR_INTERRUPT_TRANSFER) &&
        (Urb->UrbHeader.Function != URB_FUNCTION_ISOCH_TRANSFER) &&
        (Urb->UrbHeader.Function != URB_FUNCTION_CLASS_INTERFACE))
    {
        traceLevel = TRACE_LEVEL_INFORMATION;
    }*/
    TraceEvents(TRACE_LEVEL_VERBOSE, TRACE_URB,
        __FUNCTION__": Start processing Request %p URB Function %s requests on ringbuffer: %d\n",
        Request,
        UrbFuncString,
        OnRingBuffer(fdoContext->Xen));

    UCHAR Destination = BMREQUEST_TO_DEVICE;

    switch (Urb->UrbHeader.Function)
    {
    case URB_FUNCTION_GET_CURRENT_FRAME_NUMBER:
        {
            if (Urb->UrbHeader.Length < sizeof(_URB_GET_CURRENT_FRAME_NUMBER ))
            {
                Status = STATUS_UNSUCCESSFUL;
            }
            else
            {
                Urb->UrbGetCurrentFrameNumber.FrameNumber = fdoContext->ScratchPad.FrameNumber;
                Status = STATUS_SUCCESS;
            }
            RequestGetRequestContext(Request)->RequestCompleted = 1;
            ReleaseFdoLock(fdoContext);
            WdfRequestComplete(Request, Status);
            AcquireFdoLock(fdoContext);
            return;
        }
        break;

        //
        // UsbBuildGetDescriptorRequest
        //
    case URB_FUNCTION_GET_DESCRIPTOR_FROM_DEVICE:
        //
        // if this is a valid device config desc request just hand back ours.
        //
        if (fdoContext->ConfigData &&
            (Urb->UrbControlDescriptorRequest.DescriptorType == USB_CONFIGURATION_DESCRIPTOR_TYPE) &&
            (Urb->UrbControlDescriptorRequest.TransferBuffer != NULL) &&
            (Urb->UrbControlDescriptorRequest.TransferBufferLength >= sizeof(USB_CONFIGURATION_DESCRIPTOR)))
        {
            PUSB_CONFIGURATION_DESCRIPTOR config = 
                ConfigByIndex(fdoContext,
                Urb->UrbControlDescriptorRequest.Index);

            if (config)
            {
                ULONG configBytes = config->wTotalLength;
                ULONG bytesToCopy = min(Urb->UrbControlDescriptorRequest.TransferBufferLength, configBytes);

                RtlCopyMemory(Urb->UrbControlDescriptorRequest.TransferBuffer,
                    (PVOID) config,
                    bytesToCopy);            

                Urb->UrbControlDescriptorRequest.TransferBufferLength = bytesToCopy;

                TraceEvents(TRACE_LEVEL_VERBOSE, TRACE_URB,
                    __FUNCTION__": Device config descriptor %d processed locally %d bytes returned\n",
                    Urb->UrbControlDescriptorRequest.Index,
                    bytesToCopy);

                DbgPrintBuffer(Urb->UrbControlDescriptorRequest.TransferBuffer,
                    bytesToCopy,
                    TRACE_LEVEL_VERBOSE,
                    TRACE_URB);
                
                RequestGetRequestContext(Request)->RequestCompleted = 1;
                ReleaseFdoLock(fdoContext);
                WdfRequestComplete(Request, STATUS_SUCCESS);
                AcquireFdoLock(fdoContext);
                return;
            }
        }
        else if ((fdoContext->DeviceDescriptor.bLength) &&
            (Urb->UrbControlDescriptorRequest.DescriptorType == USB_DEVICE_DESCRIPTOR_TYPE) &&
            (Urb->UrbControlDescriptorRequest.TransferBuffer != NULL) &&
            (Urb->UrbControlDescriptorRequest.TransferBufferLength >= sizeof(USB_DEVICE_DESCRIPTOR)))
        {
            ULONG bytesToCopy = min(Urb->UrbControlDescriptorRequest.TransferBufferLength, 
                fdoContext->DeviceDescriptor.bLength);

            RtlCopyMemory(Urb->UrbControlDescriptorRequest.TransferBuffer,
                (PVOID) &fdoContext->DeviceDescriptor,
                bytesToCopy);

            Urb->UrbControlDescriptorRequest.TransferBufferLength = bytesToCopy;
            TraceEvents(TRACE_LEVEL_VERBOSE, TRACE_URB,
                __FUNCTION__": device descriptor processed locally %d bytes returned\n",
                bytesToCopy);

            DbgPrintBuffer(Urb->UrbControlDescriptorRequest.TransferBuffer,
                bytesToCopy,
                TRACE_LEVEL_VERBOSE,
                TRACE_URB);
            
            RequestGetRequestContext(Request)->RequestCompleted = 1;
            ReleaseFdoLock(fdoContext);
            WdfRequestComplete(Request, STATUS_SUCCESS);
            AcquireFdoLock(fdoContext);
            return;
        }
        //
        // otherwise fall through
        //
    case URB_FUNCTION_GET_DESCRIPTOR_FROM_INTERFACE:
    case URB_FUNCTION_GET_DESCRIPTOR_FROM_ENDPOINT:
        //
        // Just return our cached descriptor?
        //
        if (Urb->UrbControlDescriptorRequest.TransferBufferMDL)
        {
        }
        else if (Urb->UrbControlDescriptorRequest.TransferBuffer)
        {
        }
        else
        {
            RequestGetRequestContext(Request)->RequestCompleted = 1;
            ReleaseFdoLock(fdoContext);
            WdfRequestComplete(Request, STATUS_INVALID_PARAMETER);
            AcquireFdoLock(fdoContext);
            return;
        }
        switch (Urb->UrbHeader.Function)
        {
        case URB_FUNCTION_GET_DESCRIPTOR_FROM_DEVICE:
            Destination = BMREQUEST_TO_DEVICE;
            break;
        case URB_FUNCTION_GET_DESCRIPTOR_FROM_INTERFACE:
            Destination = BMREQUEST_TO_INTERFACE;
            break;
        case URB_FUNCTION_GET_DESCRIPTOR_FROM_ENDPOINT:
            Destination = BMREQUEST_TO_ENDPOINT;
        }

        ProcessGetDescriptor(Destination,
            fdoContext,
            Request,
            Urb);
        break;

    case URB_FUNCTION_SELECT_CONFIGURATION:
        if (fdoContext->ConfigBusy)
        {
            TraceEvents(TRACE_LEVEL_VERBOSE, TRACE_URB,
                __FUNCTION__": URB_FUNCTION_SELECT_CONFIGURATION busy\n");

            RequeueRequest(fdoContext, Request);
            return;
        }
        fdoContext->ConfigBusy = TRUE;

        ProcessSelectConfiguration(
            fdoContext,
            Request,
            Urb);

        break;

        // UsbBuildSelectInterfaceRequest
    case URB_FUNCTION_SELECT_INTERFACE:
        if (fdoContext->ConfigBusy)
        {
            TraceEvents(TRACE_LEVEL_WARNING, TRACE_URB,
                __FUNCTION__": URB_FUNCTION_SELECT_INTERFACE busy\n");

            RequeueRequest(fdoContext, Request);
            return;
        }
        fdoContext->ConfigBusy = TRUE;
        ProcessSelectInterface(
            fdoContext,
            Request,
            Urb);
        break;

        // UsbBuildInterruptOrBulkTransferRequest
    case URB_FUNCTION_BULK_OR_INTERRUPT_TRANSFER:
        //
        // use the hca to restart partial urbs
        //
        RtlZeroMemory(&Urb->UrbBulkOrInterruptTransfer.hca, sizeof(_URB_HCD_AREA));
        ProcessBulkOrInterruptTransfer(
            fdoContext,
            Request,
            Urb);
        break;

    case URB_FUNCTION_CONTROL_TRANSFER_EX:

        ProcessControlTransferEx(
            fdoContext,
            Request,
            Urb);
        break;

    case URB_FUNCTION_CONTROL_TRANSFER:

        ProcessControlTransfer(
            fdoContext,
            Request,
            Urb);
        break;

    case URB_FUNCTION_SET_FEATURE_TO_DEVICE:

        ProcessClearOrSetFeature(
            fdoContext,
            Request,
            Urb,            
            BMREQUEST_TO_DEVICE,
            USB_REQUEST_SET_FEATURE);
        break;

    case URB_FUNCTION_SET_FEATURE_TO_INTERFACE:

        ProcessClearOrSetFeature(
            fdoContext,
            Request,
            Urb,
            BMREQUEST_TO_INTERFACE,
            USB_REQUEST_SET_FEATURE);
        break;

    case URB_FUNCTION_SET_FEATURE_TO_ENDPOINT:

        ProcessClearOrSetFeature(
            fdoContext,
            Request,
            Urb,
            BMREQUEST_TO_ENDPOINT,
            USB_REQUEST_SET_FEATURE);
        break;

    case URB_FUNCTION_CLEAR_FEATURE_TO_DEVICE:

        ProcessClearOrSetFeature(
            fdoContext,
            Request,
            Urb,
            BMREQUEST_TO_DEVICE,
            USB_REQUEST_CLEAR_FEATURE);
        break;

    case URB_FUNCTION_CLEAR_FEATURE_TO_INTERFACE:

        ProcessClearOrSetFeature(
            fdoContext,
            Request,
            Urb,
            BMREQUEST_TO_INTERFACE,
            USB_REQUEST_CLEAR_FEATURE);
        break;

    case URB_FUNCTION_CLEAR_FEATURE_TO_ENDPOINT:

        ProcessClearOrSetFeature(
            fdoContext,
            Request,
            Urb,
            BMREQUEST_TO_ENDPOINT,
            USB_REQUEST_CLEAR_FEATURE);
        break;

    case URB_FUNCTION_SYNC_RESET_PIPE_AND_CLEAR_STALL: // aka URB_FUNCTION_RESET_PIPE

        ProcessSyncResetClearStall(
            fdoContext,
            Request,
            Urb);
        break;

    case URB_FUNCTION_ABORT_PIPE:
        //
        // Indicates that all outstanding requests for a pipe should be canceled. If set,
        // the URB is used with _URB_PIPE_REQUEST as the data structure. This general-purpose
        // request enables a client to cancel any pending transfers for the specified pipe.
        // Pipe state and endpoint state are unaffected. The abort request might complete
        // before all outstanding requests have completed. Do not assume that completion of
        // the abort request implies that all other outstanding requests have completed. 
        //
        ProcessAbortPipe(
            fdoContext,
            Request,
            Urb);
        break;

    case URB_FUNCTION_CLASS_DEVICE:

        ProcessClassOrVendorCommand(
            fdoContext,
            Request,
            Urb,
            BMREQUEST_TO_DEVICE,
            BMREQUEST_CLASS);
        break;

    case URB_FUNCTION_CLASS_INTERFACE:
        //
        // HACK! If the request might be a HID
        // SET_IDLE request then just complete it
        // with success. This is a test!
        //
        if (Urb->UrbControlVendorClassRequest.Request == 0x0a &&
            Urb->UrbControlVendorClassRequest.TransferBufferLength == 0)
        {
            //
            // we could look at the interface and actually decide if this really
            // is a HID. The interface is Urb->UrbControlVendorClassRequest.Index.
            // The device stalls anyway, so this is not the problem.
            //            
            TraceEvents(TRACE_LEVEL_VERBOSE, TRACE_URB,
                __FUNCTION__": This class request could be a HID SET_IDLE\n");

            ProcessClassOrVendorCommand(
                fdoContext,
                Request,
                Urb,
                BMREQUEST_TO_INTERFACE,
                BMREQUEST_CLASS);

        }
        else
        {
            ProcessClassOrVendorCommand(
                fdoContext,
                Request,
                Urb,
                BMREQUEST_TO_INTERFACE,
                BMREQUEST_CLASS);
        }
        break;

    case URB_FUNCTION_CLASS_ENDPOINT:

        ProcessClassOrVendorCommand(
            fdoContext,
            Request,
            Urb,
            BMREQUEST_TO_ENDPOINT,
            BMREQUEST_CLASS);
        break;

        //
        // should be supported
        //
    case URB_FUNCTION_VENDOR_DEVICE:

        ProcessClassOrVendorCommand(
            fdoContext,
            Request,
            Urb,
            BMREQUEST_TO_DEVICE,
            BMREQUEST_VENDOR);
        break;

    case URB_FUNCTION_VENDOR_INTERFACE:

        ProcessClassOrVendorCommand(
            fdoContext,
            Request,
            Urb,
            BMREQUEST_TO_INTERFACE,
            BMREQUEST_VENDOR);
        break;

    case URB_FUNCTION_VENDOR_ENDPOINT:

        ProcessClassOrVendorCommand(
            fdoContext,
            Request,
            Urb,
            BMREQUEST_TO_ENDPOINT,
            BMREQUEST_VENDOR);
        break;
        //
        // ???
        //
    case URB_FUNCTION_GET_STATUS_FROM_DEVICE:
        ProcessGetStatus(
            fdoContext,
            Request,
            Urb,
            BMREQUEST_TO_DEVICE);
        break;

    case URB_FUNCTION_GET_STATUS_FROM_INTERFACE:
        ProcessGetStatus(
            fdoContext,
            Request,
            Urb,
            BMREQUEST_TO_INTERFACE);
        break;

    case URB_FUNCTION_GET_STATUS_FROM_ENDPOINT:
        ProcessGetStatus(
            fdoContext,
            Request,
            Urb,
            BMREQUEST_TO_ENDPOINT);
        break;

    case URB_FUNCTION_GET_STATUS_FROM_OTHER:
        ProcessGetStatus(
            fdoContext,
            Request,
            Urb,
            BMREQUEST_TO_OTHER);
        break;

    case URB_FUNCTION_ISOCH_TRANSFER:

        RtlZeroMemory(&Urb->UrbIsochronousTransfer.hca, sizeof(_URB_HCD_AREA));
        ProcessIsoRequest(
            fdoContext,
            Request,
            Urb);
        break;

    case URB_FUNCTION_GET_MS_FEATURE_DESCRIPTOR:
        //
        // hmm.. just send this down and see what happens.
        // it is a MSFT defined, vendor implemented vendor request to
        // a device or interface.
        //
        OsFeatureDescriptorCommand(
            fdoContext,
            Request,
            Urb);
        break;

    default:
        
        RequestGetRequestContext(Request)->RequestCompleted = 1;
        ReleaseFdoLock(fdoContext);
        WdfRequestComplete(Request, STATUS_UNSUCCESSFUL);
        AcquireFdoLock(fdoContext);
        break;
    }
}

/**
 * @brief process BULK (or INT) URB transfer requests.
 * __Requirements inherited from caller:__
 * * FDO lock held by caller *
 * * Must send to DOM0 or re-queue or complete the Request *
 *
 * @param[in] fdoContext. The FDO context.
 * @param[in] Request. The WDFREQUEST handle.
 * @param[in] Urb. The URB for the BULK or INT transfer operation.
 *
 */
_Requires_lock_held_(fdoContext->WdfDevice)
VOID
ProcessBulkOrInterruptTransfer(
    IN PUSB_FDO_CONTEXT fdoContext,
    IN WDFREQUEST Request,
    IN PURB Urb)
{
    WDF_USB_CONTROL_SETUP_PACKET packet;

    TraceEvents(TRACE_LEVEL_VERBOSE, TRACE_URB,
        __FUNCTION__": Urb %p Request %p inflight requests %d\n",
        Urb,
        Request,
        OnRingBuffer(fdoContext->Xen));
    //
    // validate pipe handle. (It needs to be in m_defaultEndpoints).
    //
    if (Urb->UrbBulkOrInterruptTransfer.Hdr.Function !=
        URB_FUNCTION_BULK_OR_INTERRUPT_TRANSFER)
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_URB,
            __FUNCTION__": %s invalid Function %x\n",
            fdoContext->FrontEndPath,
            Urb->UrbBulkOrInterruptTransfer.Hdr.Function);
        
        RequestGetRequestContext(Request)->RequestCompleted = 1;
        ReleaseFdoLock(fdoContext);
        WdfRequestComplete(Request, STATUS_INVALID_PARAMETER);
        AcquireFdoLock(fdoContext);
        return;
    }

    if (Urb->UrbBulkOrInterruptTransfer.Hdr.Length !=
        sizeof(_URB_BULK_OR_INTERRUPT_TRANSFER))
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_URB,
            __FUNCTION__": %s invalid length %d expected %d\n",
            fdoContext->FrontEndPath,
            Urb->UrbBulkOrInterruptTransfer.Hdr.Length,
            sizeof(_URB_BULK_OR_INTERRUPT_TRANSFER));
        
        RequestGetRequestContext(Request)->RequestCompleted = 1;
        ReleaseFdoLock(fdoContext);
        WdfRequestComplete(Request, STATUS_INVALID_PARAMETER);
        AcquireFdoLock(fdoContext);
        return;
    }

    PUSB_ENDPOINT_DESCRIPTOR endpoint =
        PipeHandleToEndpointAddressDescriptor(fdoContext,
        Urb->UrbBulkOrInterruptTransfer.PipeHandle);
    if (endpoint)
    {
        TraceEvents(TRACE_LEVEL_VERBOSE, TRACE_URB,
            __FUNCTION__":  found ea %x direction %s\n",
            (endpoint->bEndpointAddress & ~USB_ENDPOINT_DIRECTION_MASK),
            USB_ENDPOINT_DIRECTION_IN(endpoint->bEndpointAddress) ? "In" : "Out");
    }
    else
    {
        RequestGetRequestContext(Request)->RequestCompleted = 1;
        ReleaseFdoLock(fdoContext);
        WdfRequestComplete(Request, STATUS_INVALID_PARAMETER);
        AcquireFdoLock(fdoContext);
        return;
    }

    USBD_PIPE_TYPE PipeType = UsbdPipeTypeBulk;
    switch (endpoint->bmAttributes & USB_ENDPOINT_TYPE_MASK)
    {
    case USB_ENDPOINT_TYPE_CONTROL:
    case USB_ENDPOINT_TYPE_ISOCHRONOUS:
        //
        // cant do this.
        //
        RequestGetRequestContext(Request)->RequestCompleted = 1;
        ReleaseFdoLock(fdoContext);
        WdfRequestComplete(Request, STATUS_INVALID_PARAMETER);
        AcquireFdoLock(fdoContext);
        return;

    case USB_ENDPOINT_TYPE_INTERRUPT:
        PipeType = UsbdPipeTypeInterrupt;

    case USB_ENDPOINT_TYPE_BULK:
        //
        // direction is implied by endpoint address
        //
        RtlZeroMemory(&packet, sizeof(WDF_USB_CONTROL_SETUP_PACKET));
        PutUrbOnRing(
            fdoContext,
            &packet,
            Request,
            PipeType,
            endpoint->bEndpointAddress,
            TRUE,
            Urb->UrbBulkOrInterruptTransfer.TransferFlags & USBD_SHORT_TRANSFER_OK ? TRUE : FALSE);
        break;
    }
}

/**
 * @brief process ISO URB transfer requests.
 * __Requirements inherited from caller:__
 * * FDO lock held by caller *
 * * Must send to DOM0 or re-queue or complete the Request *
 *
 * __Other Stuff__
 * Rules we know about:
 *  Packets must be contiguous. 
 *    In other words, IsoPacket[n+1].Offset must be equal to IsoPacket[n].Offset + IsoPacket[n].Length. (not enforced)
 *  In high-speed transmissions, the number of packets must be a multiple of eight. (not enforced)
 *  The only supported flag is USB_START_ISO_TRANSFER_ASAP?
 *
 * @param[in] fdoContext. The FDO context.
 * @param[in] Request. The WDFREQUEST handle.
 * @param[in] Urb. The URB for the ISO transfer operation.
 *
 */
_Requires_lock_held_(fdoContext->WdfDevice)
VOID
ProcessIsoRequest(
    IN PUSB_FDO_CONTEXT fdoContext,
    IN WDFREQUEST Request,
    IN PURB Urb)
{
    WDF_USB_CONTROL_SETUP_PACKET packet;
    TraceEvents(TRACE_LEVEL_VERBOSE, TRACE_URB,
        __FUNCTION__": Urb %p Request %p\n",
        Urb,
        Request);
    //
    // validate 
    //
    if (Urb->UrbIsochronousTransfer.Hdr.Function != 
        URB_FUNCTION_ISOCH_TRANSFER)
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_URB,
            __FUNCTION__": %s invalid Function %x\n",
            fdoContext->FrontEndPath,
            Urb->UrbIsochronousTransfer.Hdr.Function);
        
        RequestGetRequestContext(Request)->RequestCompleted = 1;
        ReleaseFdoLock(fdoContext);
        WdfRequestComplete(Request, STATUS_INVALID_PARAMETER);
        AcquireFdoLock(fdoContext);
        return;
    }

    if (Urb->UrbIsochronousTransfer.Hdr.Length < 
        sizeof(_URB_ISOCH_TRANSFER))
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_URB,
            __FUNCTION__": %s invalid header length %d expected at least %d\n",
            fdoContext->FrontEndPath,
            Urb->UrbIsochronousTransfer.Hdr.Length,
            sizeof(_URB_BULK_OR_INTERRUPT_TRANSFER));
        
        RequestGetRequestContext(Request)->RequestCompleted = 1;
        ReleaseFdoLock(fdoContext);
        WdfRequestComplete(Request, STATUS_INVALID_PARAMETER);
        AcquireFdoLock(fdoContext);
        return;
    }

    ULONG urbSize = GET_ISO_URB_SIZE(Urb->UrbIsochronousTransfer.NumberOfPackets);
    if (Urb->UrbIsochronousTransfer.Hdr.Length < 
        urbSize)
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_URB,
            __FUNCTION__": %s invalid length %d expected from NumberOfPackets %d\n",
            fdoContext->FrontEndPath,
            Urb->UrbIsochronousTransfer.Hdr.Length,
            urbSize);
        
        RequestGetRequestContext(Request)->RequestCompleted = 1;
        ReleaseFdoLock(fdoContext);
        WdfRequestComplete(Request, STATUS_INVALID_PARAMETER);
        AcquireFdoLock(fdoContext);
        return;
    }

    if (Urb->UrbIsochronousTransfer.NumberOfPackets > MaxIsoPackets(fdoContext->Xen))
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_URB,
            __FUNCTION__": %s NumberOfPackets (%d) > MAX_ISO_PACKETS (%d)\n",
            fdoContext->FrontEndPath,
            Urb->UrbIsochronousTransfer.NumberOfPackets,
            MaxIsoPackets(fdoContext->Xen));
        
        RequestGetRequestContext(Request)->RequestCompleted = 1;
        ReleaseFdoLock(fdoContext);
        WdfRequestComplete(Request, STATUS_INVALID_PARAMETER);
        AcquireFdoLock(fdoContext);
        return;
    }

    PUSB_ENDPOINT_DESCRIPTOR endpoint = 
        PipeHandleToEndpointAddressDescriptor(fdoContext, Urb->UrbIsochronousTransfer.PipeHandle);

    if (endpoint)
    {
        TraceEvents(TRACE_LEVEL_VERBOSE, TRACE_URB,
            __FUNCTION__": found ea %x direction %s\n",
            (endpoint->bEndpointAddress & ~USB_ENDPOINT_DIRECTION_MASK),
            USB_ENDPOINT_DIRECTION_IN(endpoint->bEndpointAddress) ? "In" : "Out");
    }
    else
    {
        RequestGetRequestContext(Request)->RequestCompleted = 1;
        ReleaseFdoLock(fdoContext);
        WdfRequestComplete(Request, STATUS_INVALID_PARAMETER);
        AcquireFdoLock(fdoContext);
        return;
    }

    switch (endpoint->bmAttributes & USB_ENDPOINT_TYPE_MASK)
    {
    default:
    case USB_ENDPOINT_TYPE_CONTROL:
    case USB_ENDPOINT_TYPE_INTERRUPT:
    case USB_ENDPOINT_TYPE_BULK:
        //
        // isoch on isoch only
        //
        RequestGetRequestContext(Request)->RequestCompleted = 1;
        ReleaseFdoLock(fdoContext);
        WdfRequestComplete(Request, STATUS_INVALID_PARAMETER);
        AcquireFdoLock(fdoContext);
        return;

    case USB_ENDPOINT_TYPE_ISOCHRONOUS:
        //
        // direction is implied by endpoint address
        //
        RtlZeroMemory(&packet, sizeof(WDF_USB_CONTROL_SETUP_PACKET));
        BOOLEAN transferAsap = (Urb->UrbIsochronousTransfer.TransferFlags & USBD_START_ISO_TRANSFER_ASAP) ? TRUE : FALSE;

        PutIsoUrbOnRing(
            fdoContext,
            &packet,
            Request,
            endpoint->bEndpointAddress,
            transferAsap,
            1); 
        break;    
    }
}

/**
 * @brief process Control pipe URB transfer requests.
 * __Requirements inherited from caller:__
 * * FDO lock held by caller *
 * * Must send to DOM0 or re-queue or complete the Request *
 *
 * @param[in] fdoContext. The FDO context.
 * @param[in] Request. The WDFREQUEST handle.
 * @param[in] Urb. The URB for the Control pipe transfer operation.
 *
 */
_Requires_lock_held_(fdoContext->WdfDevice)
VOID
ProcessControlTransfer(
    IN PUSB_FDO_CONTEXT fdoContext,
    IN WDFREQUEST Request,
    IN PURB Urb)
{
    WDF_USB_CONTROL_SETUP_PACKET packet;

    TraceEvents(TRACE_LEVEL_VERBOSE, TRACE_URB,
        __FUNCTION__": Urb %p Request %p\n",
        Urb,
        Request);
    //
    // validate pipe handle. (It needs to be in m_defaultEndpoints.)
    //
    if (Urb->UrbControlTransfer.Hdr.Function !=
        URB_FUNCTION_CONTROL_TRANSFER)
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_URB,
            __FUNCTION__": %s invalid Function %x\n",
            fdoContext->FrontEndPath,
            Urb->UrbControlTransfer.Hdr.Function);
        
        RequestGetRequestContext(Request)->RequestCompleted = 1;
        ReleaseFdoLock(fdoContext);
        WdfRequestComplete(Request, STATUS_INVALID_PARAMETER);
        AcquireFdoLock(fdoContext);
        return;
    }
    if (Urb->UrbControlTransfer.Hdr.Length !=
        sizeof(_URB_CONTROL_TRANSFER))
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_URB,
            __FUNCTION__": %s invalid length %d expected %d\n",
            fdoContext->FrontEndPath,
            Urb->UrbControlTransfer.Hdr.Length,
            sizeof(_URB_CONTROL_TRANSFER));
        
        RequestGetRequestContext(Request)->RequestCompleted = 1;
        ReleaseFdoLock(fdoContext);
        WdfRequestComplete(Request, STATUS_INVALID_PARAMETER);
        AcquireFdoLock(fdoContext);
        return;
    }
    UCHAR EndpointAddress = 0;
    if ((Urb->UrbControlTransfer.TransferFlags & USBD_DEFAULT_PIPE_TRANSFER) ||
        (Urb->UrbControlTransfer.PipeHandle == NULL)) // ???
    {
        // The client is supposed to set the USBD_DEFAULT_PIPE_TRANSFER flag but
        // KMDF at least doesn't bother and instead uses NULL as the clue
    }
    else
    {
        PUSB_ENDPOINT_DESCRIPTOR endpoint =
            PipeHandleToEndpointAddressDescriptor(
            fdoContext,
            Urb->UrbControlTransfer.PipeHandle);
        if (endpoint)
        {
            TraceEvents(TRACE_LEVEL_VERBOSE, TRACE_URB,
                __FUNCTION__": found ea %x direction %s\n",
                (endpoint->bEndpointAddress & ~USB_ENDPOINT_DIRECTION_MASK),
                USB_ENDPOINT_DIRECTION_IN(endpoint->bEndpointAddress) ? "In" : "Out");
            EndpointAddress = endpoint->bEndpointAddress;
        }
        else
        {
            TraceEvents(TRACE_LEVEL_ERROR, TRACE_URB,
                __FUNCTION__": %s invalid pipe %p and USBD_DEFAULT_PIPE_TRANSFER not set\n",
                fdoContext->FrontEndPath,
                Urb->UrbControlTransfer.PipeHandle);
            
            RequestGetRequestContext(Request)->RequestCompleted = 1;
            ReleaseFdoLock(fdoContext);
            WdfRequestComplete(Request, STATUS_INVALID_PARAMETER);
            AcquireFdoLock(fdoContext);
            return;
        }
    }

    if (Urb->UrbControlTransfer.TransferFlags & USBD_TRANSFER_DIRECTION_IN)
    {
        EndpointAddress |= USB_ENDPOINT_DIRECTION_MASK;
    }
    else
    {
        EndpointAddress &= (~USB_ENDPOINT_DIRECTION_MASK);
    }

    TraceEvents(TRACE_LEVEL_VERBOSE, TRACE_URB,
        __FUNCTION__": ea %x length %d\n",
        EndpointAddress,
        Urb->UrbControlTransfer.TransferBufferLength);

    RtlCopyMemory(packet.Generic.Bytes, Urb->UrbControlTransfer.SetupPacket, sizeof (packet));

    PutUrbOnRing(
        fdoContext,
        &packet,
        Request,
        UsbdPipeTypeControl,
        EndpointAddress,
        TRUE,
        Urb->UrbControlTransfer.TransferFlags & USBD_SHORT_TRANSFER_OK ? TRUE : FALSE);
}

 /**
 * @brief process Control pipe URB transfer requests using URB_CONTROL_TRANSFER_EX
 * __Requirements inherited from caller:__
 * * FDO lock held by caller *
 * * Must send to DOM0 or re-queue or complete the Request *
 * * @todo timeout is ignored. *
 *
 * @param[in] fdoContext. The FDO context.
 * @param[in] Request. The WDFREQUEST handle.
 * @param[in] Urb. The URB for the Control pipe transfer operation.
 *
 */
_Requires_lock_held_(fdoContext->WdfDevice)
VOID
ProcessControlTransferEx(
    IN PUSB_FDO_CONTEXT fdoContext,
    IN WDFREQUEST Request,
    IN PURB Urb)
{
    WDF_USB_CONTROL_SETUP_PACKET packet;

    TraceEvents(TRACE_LEVEL_VERBOSE, TRACE_URB,
        __FUNCTION__": Urb %p Request %p\n",
        Urb,
        Request);
    //
    // validate pipe handle. (It needs to be in m_defaultEndpoints.)
    //
    if (Urb->UrbControlTransfer.Hdr.Function !=
        URB_FUNCTION_CONTROL_TRANSFER_EX)
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_URB,
            __FUNCTION__": %s invalid Function %x\n",
            fdoContext->FrontEndPath,
            Urb->UrbControlTransfer.Hdr.Function);
        
        RequestGetRequestContext(Request)->RequestCompleted = 1;
        ReleaseFdoLock(fdoContext);
        WdfRequestComplete(Request, STATUS_INVALID_PARAMETER);
        AcquireFdoLock(fdoContext);
        return;
    }
    if (Urb->UrbControlTransfer.Hdr.Length !=
        sizeof(_URB_CONTROL_TRANSFER_EX))
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_URB,
            __FUNCTION__": %s invalid length %d expected %d\n",
            fdoContext->FrontEndPath,
            Urb->UrbControlTransferEx.Hdr.Length,
            sizeof(_URB_CONTROL_TRANSFER_EX));
        
        RequestGetRequestContext(Request)->RequestCompleted = 1;
        ReleaseFdoLock(fdoContext);
        WdfRequestComplete(Request, STATUS_INVALID_PARAMETER);
        AcquireFdoLock(fdoContext);
        return;
    }
    UCHAR EndpointAddress = 0;
    if ((Urb->UrbControlTransferEx.TransferFlags & USBD_DEFAULT_PIPE_TRANSFER) ||
        (Urb->UrbControlTransferEx.PipeHandle == NULL)) // ???
    {
        // The client is supposed to set the USBD_DEFAULT_PIPE_TRANSFER flag but
        // KMDF at least doesn't bother and instead uses NULL as the clue
    }
    else
    {
        PUSB_ENDPOINT_DESCRIPTOR endpoint =
            PipeHandleToEndpointAddressDescriptor(
            fdoContext,
            Urb->UrbControlTransferEx.PipeHandle);
        if (endpoint)
        {
            TraceEvents(TRACE_LEVEL_VERBOSE, TRACE_URB,
                __FUNCTION__": found ea %x direction %s\n",
                (endpoint->bEndpointAddress & ~USB_ENDPOINT_DIRECTION_MASK),
                USB_ENDPOINT_DIRECTION_IN(endpoint->bEndpointAddress) ? "In" : "Out");
            EndpointAddress = endpoint->bEndpointAddress;
        }
        else
        {
            TraceEvents(TRACE_LEVEL_ERROR, TRACE_URB,
                __FUNCTION__": %s invalid pipe %p and USBD_DEFAULT_PIPE_TRANSFER not set\n",
                fdoContext->FrontEndPath,
                Urb->UrbControlTransferEx.PipeHandle);

            RequestGetRequestContext(Request)->RequestCompleted = 1;
            ReleaseFdoLock(fdoContext);
            WdfRequestComplete(Request, STATUS_INVALID_PARAMETER);
            AcquireFdoLock(fdoContext);
            return;
        }
    }

    if (Urb->UrbControlTransferEx.TransferFlags & USBD_TRANSFER_DIRECTION_IN)
    {
        EndpointAddress |= USB_ENDPOINT_DIRECTION_MASK;
    }
    else
    {
        EndpointAddress &= (~USB_ENDPOINT_DIRECTION_MASK);
    }

    TraceEvents(TRACE_LEVEL_VERBOSE, TRACE_URB,
        __FUNCTION__": ea %x length %d\n",
        EndpointAddress,
        Urb->UrbControlTransferEx.TransferBufferLength);

    RtlCopyMemory(packet.Generic.Bytes, Urb->UrbControlTransferEx.SetupPacket, sizeof (packet));

    PutUrbOnRing(
        fdoContext,
        &packet,
        Request,
        UsbdPipeTypeControl,
        EndpointAddress,
        TRUE,
        Urb->UrbControlTransferEx.TransferFlags & USBD_SHORT_TRANSFER_OK ? TRUE : FALSE);
}

/**
 * @brief process Idle Notification Requests.
 * __Requirements inherited from caller:__
 * * FDO lock held by caller *
 * * Must send to DOM0 or re-queue or complete the Request *
 * __The IOCTL_INTERNAL_USB_SUBMIT_IDLE_NOTIFICATION I/O request is used by drivers to inform the 
 * USB bus driver that a device is idle and can be suspended.__
 *
 * @TODO we don't support end to end power management yet. The current implementation never calls
 * the callback function to inform the caller that the device can be set idle. The request will be
 * completed when the device is removed or cancelled.
 *
 * @param[in] fdoContext. The FDO context.
 * @param[in] Request. The WDFREQUEST handle.
 *
 */
_Requires_lock_held_(fdoContext->WdfDevice)
VOID
ProcessIdleNotificationRequest(
    IN PUSB_FDO_CONTEXT fdoContext,
    IN WDFREQUEST Request)
{
    PUSB_IDLE_CALLBACK_INFO callback = NULL;
    size_t bufferLength;

    NTSTATUS Status = WdfRequestRetrieveInputBuffer(Request,
        sizeof(USB_IDLE_CALLBACK_INFO),
        (PVOID *) &callback,
        &bufferLength);

    if (!NT_SUCCESS(Status))
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_URB,
            __FUNCTION__": %s WdfRequestRetrieveInputBuffer erro %x\n",
            fdoContext->FrontEndPath,
            Status);
        
        RequestGetRequestContext(Request)->RequestCompleted = 1;
        ReleaseFdoLock(fdoContext);
        WdfRequestComplete(Request, Status);
        AcquireFdoLock(fdoContext);
        return;
    }

    if (bufferLength != sizeof(USB_IDLE_CALLBACK_INFO))
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_URB,
            __FUNCTION__ ": %s in %d expected %d\n",
            fdoContext->FrontEndPath,
            bufferLength,
            sizeof(USB_IDLE_CALLBACK_INFO));
        
        RequestGetRequestContext(Request)->RequestCompleted = 1;
        ReleaseFdoLock(fdoContext);
        WdfRequestComplete(Request, STATUS_INVALID_PARAMETER);
        AcquireFdoLock(fdoContext);
        return;
    }
    //
    // This should never happen.
    //
    if (!callback)
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_URB,
            __FUNCTION__ ": %s no callback data!\n",
            fdoContext->FrontEndPath);
        
        RequestGetRequestContext(Request)->RequestCompleted = 1;
        ReleaseFdoLock(fdoContext);
        WdfRequestComplete(Request, STATUS_INVALID_PARAMETER);
        AcquireFdoLock(fdoContext);
        return;
    }

    if (callback->IdleCallback == NULL)
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_URB,
            __FUNCTION__ ": %s NULL IdleCallback\n",
            fdoContext->FrontEndPath);
        
        RequestGetRequestContext(Request)->RequestCompleted = 1;
        ReleaseFdoLock(fdoContext);
        WdfRequestComplete(Request, STATUS_INVALID_PARAMETER);
        AcquireFdoLock(fdoContext);
        return;
    }

    if (fdoContext->IdleRequest)
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_URB,
            __FUNCTION__ ": %s idle callback already queued\n",
            fdoContext->FrontEndPath);
        
        RequestGetRequestContext(Request)->RequestCompleted = 1;
        ReleaseFdoLock(fdoContext);
        WdfRequestComplete(Request, STATUS_INVALID_PARAMETER);
        AcquireFdoLock(fdoContext);
        return;
    }
    fdoContext->IdleRequest = Request;

    TraceEvents(TRACE_LEVEL_VERBOSE, TRACE_URB,
        __FUNCTION__ ": %s IDLE Request %p IdleCallback = %p; IdleContext = %p Device %p accepted and deferred\n",
        fdoContext->FrontEndPath,
        Request,
        callback->IdleCallback,
        callback->IdleContext,
        fdoContext->WdfDevice);
    //
    // take on the idle irp 
    // return pending
    // perhaps we ought to call the callback once
    // and then never again?
    //
    Status = WdfRequestMarkCancelableEx(
        Request,
        UsbIdleEvtRequestCancel);

    if (!NT_SUCCESS(Status))
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_URB,
            __FUNCTION__ ": %s WdfRequestMarkCancelableEx error %x\n",
            fdoContext->FrontEndPath,
            Status);

        fdoContext->IdleRequest = NULL;
        RequestGetRequestContext(Request)->RequestCompleted = 1;
        ReleaseFdoLock(fdoContext);
        WdfRequestComplete(Request, Status);
        AcquireFdoLock(fdoContext);
    }
    RequestGetRequestContext(Request)->CancelSet = 1;
}

/**
 * @brief process cancellation of the idle request.
 *
 * @param[in] Request. The WDFREQUEST handle for the idle request.
 *
 */
VOID
UsbIdleEvtRequestCancel (
    IN WDFREQUEST  Request)
{
    WDFQUEUE ioqueue = WdfRequestGetIoQueue(Request);
    WDFDEVICE device = WdfIoQueueGetDevice(ioqueue);
    PUSB_FDO_CONTEXT fdoContext = DeviceGetFdoContext(device);

    AcquireFdoLock(fdoContext);
    ASSERT(fdoContext->IdleRequest == Request); // DBG only.
    fdoContext->IdleRequest = NULL;
    RequestGetRequestContext(Request)->RequestCompleted = 1;  
    RequestGetRequestContext(Request)->CancelSet = 0;
    ReleaseFdoLock(fdoContext);
    WdfRequestComplete(Request, STATUS_CANCELLED);
}


/**
 * @brief process URB_FUNCTION_SELECT_CONFIGURATION  URB transfer requests.
 * __Requirements inherited from caller:__
 * * FDO lock held by caller *
 * * Must send to DOM0 or re-queue or complete the Request *
 * Selects an alternate setting for an interface.
 *
 * @param[in] fdoContext. The FDO context.
 * @param[in] Request. The WDFREQUEST handle.
 * @param[in] Urb. The URB for the Control pipe transfer operation.
 *
 */
_Requires_lock_held_(fdoContext->WdfDevice)
VOID
ProcessSelectConfiguration(
    IN PUSB_FDO_CONTEXT fdoContext,
    IN WDFREQUEST Request,
    IN PURB Urb)
{
    NTSTATUS Status = STATUS_UNSUCCESSFUL; BOOLEAN noInterfaces = FALSE;

    if (Urb->UrbHeader.Length < sizeof(_URB_SELECT_CONFIGURATION))
    {
        if (Urb->UrbHeader.Length < NO_INTERFACE_LENGTH)
        {
            TraceEvents(TRACE_LEVEL_ERROR, TRACE_URB,
                __FUNCTION__ ": %s Device %p  length %d less than minimum %d\n",
                fdoContext->FrontEndPath,
                fdoContext->WdfDevice,
                Urb->UrbHeader.Length,
                NO_INTERFACE_LENGTH);
            
            RequestGetRequestContext(Request)->RequestCompleted = 1;
            ReleaseFdoLock(fdoContext);
            WdfRequestComplete(Request, STATUS_INVALID_PARAMETER);
            AcquireFdoLock(fdoContext);
            fdoContext->ConfigBusy = FALSE;
            return;
        }
        noInterfaces = TRUE;
    }

    PUSB_CONFIGURATION_DESCRIPTOR config = Urb->UrbSelectConfiguration.ConfigurationDescriptor;
    if (config == NULL)
    {
        //
        // this is ?most likely? NOT an actual usb config operation, but instead a
        // driver install operation.
        //
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_URB,
            __FUNCTION__ ": %s NULL config ignored\n",
            fdoContext->FrontEndPath);
        
        RequestGetRequestContext(Request)->RequestCompleted = 1;
        ReleaseFdoLock(fdoContext);
        WdfRequestComplete(Request, STATUS_SUCCESS);
        AcquireFdoLock(fdoContext);
        fdoContext->ConfigBusy = FALSE;
        return;
    }
    //
    // @todo revisit smartcard config zero nonsense.
    // See http://www.usb.org/developers/devclass_docs/DWG_Smart-Card_USB-ICC_ICCD_rev10.pdf
    //
    if (config->bConfigurationValue == 0)
    {
        if (fdoContext->CurrentConfigOffset == 1)
        {
            TraceEvents(TRACE_LEVEL_ERROR, TRACE_URB,
                __FUNCTION__ ": %s config zero - treating as config 1\n",
                fdoContext->FrontEndPath);
        }
        else
        {
            TraceEvents(TRACE_LEVEL_ERROR, TRACE_URB,
                __FUNCTION__ ": %s config zero - treating as config 1\n",
                fdoContext->FrontEndPath);
            //
            // Handled on the completion side by PostProcessSelectConfig.
            //
            SetConfiguration(fdoContext, Request, 0);
            return;
        }
    }

    if (fdoContext->ConfigurationDescriptor == NULL)
    {
        //
        // get the default config first?
        //
        Status = GetCurrentConfigurationLocked(fdoContext);
        if (!NT_SUCCESS(Status))
        {
            TraceEvents(TRACE_LEVEL_ERROR, TRACE_URB,
                __FUNCTION__ ": %s current config is NULL?\n",
                fdoContext->FrontEndPath);
            
            RequestGetRequestContext(Request)->RequestCompleted = 1;
            ReleaseFdoLock(fdoContext);
            WdfRequestComplete(Request, STATUS_UNSUCCESSFUL);
            AcquireFdoLock(fdoContext);
            fdoContext->ConfigBusy = FALSE;
            return;
        }
        // this should never happen.
        if (fdoContext->ConfigurationDescriptor == NULL)
        {
            TraceEvents(TRACE_LEVEL_ERROR, TRACE_URB,
                __FUNCTION__ ": %s default config is NULL?\n",
                fdoContext->FrontEndPath);
            
            RequestGetRequestContext(Request)->RequestCompleted = 1;
            ReleaseFdoLock(fdoContext);
            WdfRequestComplete(Request, STATUS_UNSUCCESSFUL);
            AcquireFdoLock(fdoContext);
            fdoContext->ConfigBusy = FALSE;
            return;
        }
    }

    PUSB_CONFIG_INFO configInfo =
        ConfigInfoByValue(fdoContext,
        config->bConfigurationValue);
    if (!configInfo)
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_URB,
            __FUNCTION__ ": %s invalid config value %d\n",
            fdoContext->FrontEndPath,
            fdoContext->ConfigurationDescriptor->bConfigurationValue);
        
        RequestGetRequestContext(Request)->RequestCompleted = 1;
        ReleaseFdoLock(fdoContext);
        WdfRequestComplete(Request, STATUS_UNSUCCESSFUL);
        AcquireFdoLock(fdoContext);
        fdoContext->ConfigBusy = FALSE;
        return;
    }
    //
    // validate the interface descriptor list
    //
    BOOLEAN setInterface = FALSE;
    USHORT InterfaceNumber = 0;
    USHORT Alternate = 0;
    if (noInterfaces == FALSE)
    {
        UCHAR interfacesInRequest = 0;
        PUSBD_INTERFACE_INFORMATION Interface = &Urb->UrbSelectConfiguration.Interface;
        PUCHAR start = (PUCHAR) Interface;
        PUCHAR end = ((PUCHAR) Urb) + Urb->UrbHeader.Length;
        while (start < end)
        {
            if (Interface->Length)
            {
                start += Interface->Length;
            }
            else
            {
                TraceEvents(TRACE_LEVEL_ERROR, TRACE_URB,
                    __FUNCTION__ ": %s interface %d length is zero\n",
                    fdoContext->FrontEndPath,
                    interfacesInRequest);
                
                RequestGetRequestContext(Request)->RequestCompleted = 1;
                ReleaseFdoLock(fdoContext);
                WdfRequestComplete(Request, STATUS_INVALID_PARAMETER);
                AcquireFdoLock(fdoContext);
                fdoContext->ConfigBusy = FALSE;
                return;
            }
            interfacesInRequest++;
            Interface = (PUSBD_INTERFACE_INFORMATION) start;
        }
        if (interfacesInRequest > config->bNumInterfaces)
        {
            TraceEvents(TRACE_LEVEL_ERROR, TRACE_URB,
                __FUNCTION__ ": %s caller wants %d interfaces from a config with %d\n",
                fdoContext->FrontEndPath,
                interfacesInRequest,
                config->bNumInterfaces);
            
            RequestGetRequestContext(Request)->RequestCompleted = 1;
            ReleaseFdoLock(fdoContext);
            WdfRequestComplete(Request, STATUS_INVALID_PARAMETER);
            AcquireFdoLock(fdoContext);
            fdoContext->ConfigBusy = FALSE;
            return;
        }
        ULONG index = 0;
        setInterface = (interfacesInRequest == 1);
        Interface = &Urb->UrbSelectConfiguration.Interface;
        start = (PUCHAR) Interface;
        while (index < interfacesInRequest)
        {
            InterfaceNumber = Interface->InterfaceNumber;
            Alternate = Interface->AlternateSetting;
            //
            // validate that we have this interface/alternate
            //
            PUSB_INTERFACE_DESCRIPTOR pInterfaceDescriptor = FindInterface(
                fdoContext,
                configInfo,
                Interface->InterfaceNumber,
                Interface->AlternateSetting);
            if (!pInterfaceDescriptor)
            {
                TraceEvents(TRACE_LEVEL_ERROR, TRACE_URB,
                    __FUNCTION__ ": %s Invalid interface %d alternate %d requested\n",
                    fdoContext->FrontEndPath,
                    Interface->InterfaceNumber,
                    Interface->AlternateSetting);
                Interface->InterfaceHandle = NULL;
            }
            else
            {
                Interface->InterfaceHandle = (USBD_INTERFACE_HANDLE) pInterfaceDescriptor;
                ULONG sizeNeeded = sizeof(USBD_INTERFACE_INFORMATION) - sizeof(USBD_PIPE_INFORMATION);
                if (pInterfaceDescriptor->bNumEndpoints)
                {
                    sizeNeeded += (sizeof(USBD_PIPE_INFORMATION) * pInterfaceDescriptor->bNumEndpoints);
                }
                if (sizeNeeded > Interface->Length)
                {
                    TraceEvents(TRACE_LEVEL_ERROR, TRACE_URB,
                        __FUNCTION__ ": %s Invalid interface %d alternate %d request not enough space for %d pipes\n"
                        " needed %d have %d\n",
                        fdoContext->FrontEndPath,
                        Interface->InterfaceNumber,
                        Interface->AlternateSetting,
                        pInterfaceDescriptor->bNumEndpoints,
                        sizeNeeded,
                        Interface->Length);
                    
                    RequestGetRequestContext(Request)->RequestCompleted = 1;
                    ReleaseFdoLock(fdoContext);
                    WdfRequestComplete(Request, STATUS_UNSUCCESSFUL);
                    AcquireFdoLock(fdoContext);
                    fdoContext->ConfigBusy = FALSE;
                    return;
                }

                Interface->NumberOfPipes = pInterfaceDescriptor->bNumEndpoints;
                if (Interface->NumberOfPipes)
                {
                    Status = SetInterfaceDescriptorPipes(
                        fdoContext,
                        configInfo,
                        pInterfaceDescriptor,
                        Interface);
                    if (!NT_SUCCESS(Status))
                    {
                        TraceEvents(TRACE_LEVEL_ERROR, TRACE_URB,
                            __FUNCTION__ ": %s Invalid pipe configuration\n",
                            fdoContext->FrontEndPath);
                        
                        RequestGetRequestContext(Request)->RequestCompleted = 1;
                        ReleaseFdoLock(fdoContext);
                        WdfRequestComplete(Request, Status);
                        AcquireFdoLock(fdoContext);
                        fdoContext->ConfigBusy = FALSE;
                        return;
                    }
                }

                Interface->Class = pInterfaceDescriptor->bInterfaceClass;
                Interface->SubClass = pInterfaceDescriptor->bInterfaceSubClass;
                Interface->Protocol = pInterfaceDescriptor->bInterfaceProtocol;
                Interface->Reserved = 0;
            }

            Interface = (PUSBD_INTERFACE_INFORMATION) (start + Interface->Length);
            start = (PUCHAR) Interface;
            index++;
        }
    }

    Status = STATUS_SUCCESS;

    if (!config->bConfigurationValue && fdoContext->CurrentConfigOffset && 
        (config->bConfigurationValue == fdoContext->CurrentConfigValue))
    {
        //
        // for smartcards with their stupid config 0, do not set the config if it is already set.
        // 
        TraceEvents(TRACE_LEVEL_WARNING, TRACE_URB,
         __FUNCTION__": %s smart card zero config already set\n",
         fdoContext->FrontEndPath);
    }
    else
    {
        //
        // use the scratch pad to bypass xhci problem with zero length requests
        //
        Status = SetCurrentConfigurationLocked(
            fdoContext,
            config->bConfigurationValue,
            setInterface, InterfaceNumber, Alternate);

        if (Status == STATUS_PENDING)
        {
            //
            // this is bork'd. it is a sync interface.
            //
            TraceEvents(TRACE_LEVEL_ERROR, TRACE_URB,
                __FUNCTION__": %s SetCurrentConfigurationLocked returned STATUS_PENDING\n",
                fdoContext->FrontEndPath);
            Status = STATUS_SUCCESS;
        }
    }
    if (Status == STATUS_SUCCESS)
    {
        PostProcessSelectConfig(fdoContext, Urb);
    }
    
    RequestGetRequestContext(Request)->RequestCompleted = 1;
    ReleaseFdoLock(fdoContext);
    WdfRequestComplete(Request, Status);
    AcquireFdoLock(fdoContext);
    fdoContext->ConfigBusy = FALSE;
    return;
}


_Requires_lock_held_(fdoContext->WdfDevice)
VOID
SetConfiguration(
    IN PUSB_FDO_CONTEXT fdoContext,
    IN WDFREQUEST Request,
    IN UCHAR configNumber)
{
    WDF_USB_CONTROL_SETUP_PACKET packet;
    RtlZeroMemory(&packet, sizeof(WDF_USB_CONTROL_SETUP_PACKET));
    packet.Packet.bm.Request.Dir = BMREQUEST_HOST_TO_DEVICE;
    packet.Packet.bm.Request.Type = BMREQUEST_STANDARD;
    packet.Packet.bm.Request.Recipient = BMREQUEST_TO_DEVICE;
    packet.Packet.bRequest = USB_REQUEST_SET_CONFIGURATION;
    packet.Packet.wValue.Bytes.HiByte = 0;
    packet.Packet.wValue.Bytes.LowByte = configNumber;
    packet.Packet.wIndex.Value = 0;
    packet.Packet.wLength = 0;

    TraceEvents(TRACE_LEVEL_VERBOSE, TRACE_URB,
        __FUNCTION__ ": USB Request header RequestType %x Request %x Value %x Index %x Length %x Irp %p\n",
        packet.Packet.bm.Byte,
        packet.Packet.bRequest,
        packet.Packet.wValue.Value,
        packet.Packet.wIndex.Value,
        packet.Packet.wLength,
        Request);

    PutUrbOnRing(
        fdoContext,
        &packet,
        Request,
        UsbdPipeTypeControl,    // control
        USB_ENDPOINT_TYPE_CONTROL, // control OUT
        FALSE,
        FALSE);
}

//
/// As we only currently support one interface and its default setting
/// the only feature here is the client's setting of MaximumPacketSize on the
/// pipe. UsbStor adjusts from 512 to 0x10000 (64k). There is no need to send this
/// down to the backend.
///
/// This selects one alternate setting for a single interface. The only variable
/// length portion of the data structure is the number of pipes (one for each endpoint.)
//
_Requires_lock_held_(fdoContext->WdfDevice)
VOID
ProcessSelectInterface(
    IN PUSB_FDO_CONTEXT fdoContext,
    IN WDFREQUEST Request,
    IN PURB Urb)
{
    NTSTATUS Status = STATUS_SUCCESS;
    ULONG minimumSize = sizeof(_URB_SELECT_INTERFACE) - sizeof(USBD_PIPE_INFORMATION);

    if (Urb->UrbHeader.Length < minimumSize)
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_URB,
            __FUNCTION__ ": %s length %d less than minimum %d\n",
            fdoContext->FrontEndPath,
            Urb->UrbHeader.Length,
            minimumSize);

        fdoContext->ConfigBusy = FALSE;     
        RequestGetRequestContext(Request)->RequestCompleted = 1;   
        ReleaseFdoLock(fdoContext);
        WdfRequestComplete(Request, STATUS_INVALID_PARAMETER);
        AcquireFdoLock(fdoContext);
        return;
    }

    if (Urb->UrbSelectInterface.ConfigurationHandle != fdoContext->ConfigurationDescriptor)
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_URB,
            __FUNCTION__ ": %s invalid configuration handle %p expected %p\n",
            fdoContext->FrontEndPath,
            Urb->UrbSelectInterface.ConfigurationHandle,
            fdoContext->ConfigurationDescriptor);

        fdoContext->ConfigBusy = FALSE;       
        RequestGetRequestContext(Request)->RequestCompleted = 1; 
        ReleaseFdoLock(fdoContext);
        WdfRequestComplete(Request, STATUS_UNSUCCESSFUL);
        AcquireFdoLock(fdoContext);
        return;
    }

    //
    // validate the interface descriptor list ? later - for now
    // just trust the idiot above us.
    //
    PUSB_INTERFACE_DESCRIPTOR descriptor = FindInterface(
        fdoContext,
        NULL,
        Urb->UrbSelectInterface.Interface.InterfaceNumber,
        Urb->UrbSelectInterface.Interface.AlternateSetting);
    if (!descriptor)
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_URB,
            __FUNCTION__ ": %s invalid interface %d %d\n",
            fdoContext->FrontEndPath,
            Urb->UrbSelectInterface.Interface.InterfaceNumber,
            Urb->UrbSelectInterface.Interface.AlternateSetting);

        fdoContext->ConfigBusy = FALSE;    
        RequestGetRequestContext(Request)->RequestCompleted = 1;    
        ReleaseFdoLock(fdoContext);
        WdfRequestComplete(Request, STATUS_UNSUCCESSFUL);
        AcquireFdoLock(fdoContext);
        return;
    }

    //
    // validate that the input _URB_SELECT_INTERFACE has enough room for the
    // endpoints on this interface.
    //
    ULONG numEndpoints = descriptor->bNumEndpoints;
    ULONG sizeNeeded = minimumSize;
    if (numEndpoints > 0)
    {
        sizeNeeded += sizeof(USBD_PIPE_INFORMATION) * numEndpoints;
    }
    if (sizeNeeded > Urb->UrbSelectInterface.Hdr.Length)
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_URB,
            __FUNCTION__ ": %s URB size %d too small (%d)\n",
            fdoContext->FrontEndPath,
            Urb->UrbSelectInterface.Hdr.Length,
            sizeNeeded);

        fdoContext->ConfigBusy = FALSE;   
        RequestGetRequestContext(Request)->RequestCompleted = 1;     
        ReleaseFdoLock(fdoContext);
        WdfRequestComplete(Request, STATUS_UNSUCCESSFUL);
        AcquireFdoLock(fdoContext);
        return;
    }

    Urb->UrbSelectInterface.Interface.InterfaceHandle = (USBD_INTERFACE_HANDLE) descriptor;
    Urb->UrbSelectInterface.Interface.NumberOfPipes = numEndpoints;
    if (numEndpoints)
    {
        Status = SetInterfaceDescriptorPipes(
            fdoContext,
            NULL,
            descriptor,
            &Urb->UrbSelectInterface.Interface);

        if (!NT_SUCCESS(Status))
        {
            TraceEvents(TRACE_LEVEL_ERROR, TRACE_URB,
                __FUNCTION__ ": %s Invalid pipe configuration USBD Status %x\n",
                fdoContext->FrontEndPath,
                Status);

            Urb->UrbHeader.Status = Status;

            fdoContext->ConfigBusy = FALSE;    
            RequestGetRequestContext(Request)->RequestCompleted = 1;    
            ReleaseFdoLock(fdoContext);
            WdfRequestComplete(Request, STATUS_UNSUCCESSFUL);
            AcquireFdoLock(fdoContext);
            return;
        }
    }
    if (fdoContext->NumInterfaces == 1)
    {
        //
        // don't select an interface on a device with one interface.
        //
        fdoContext->ConfigBusy = FALSE;   
        RequestGetRequestContext(Request)->RequestCompleted = 1;     
        ReleaseFdoLock(fdoContext);
        WdfRequestComplete(Request, STATUS_SUCCESS);
        AcquireFdoLock(fdoContext);
        return;
    }
    //
    // put this down to the backend via the sync scratchpad interface.
    //
    WDF_USB_CONTROL_SETUP_PACKET packet;
    RtlZeroMemory(&packet, sizeof(WDF_USB_CONTROL_SETUP_PACKET));
    UCHAR Alternate = Urb->UrbSelectInterface.Interface.AlternateSetting;
    UCHAR Interface = Urb->UrbSelectInterface.Interface.InterfaceNumber;
    packet.Packet.bm.Request.Dir = BMREQUEST_HOST_TO_DEVICE;
    packet.Packet.bm.Request.Type = BMREQUEST_STANDARD;
    packet.Packet.bm.Request.Recipient = BMREQUEST_TO_INTERFACE;
    packet.Packet.bRequest = USB_REQUEST_SET_INTERFACE;
    packet.Packet.wValue.Value = Alternate;
    packet.Packet.wIndex.Value = Interface;
    packet.Packet.wLength = 0;

    TraceEvents(TRACE_LEVEL_VERBOSE, TRACE_URB,
        __FUNCTION__ ": %s Interface %x Alternate %x\n",
        fdoContext->FrontEndPath,
        packet.Packet.wIndex.Value,
        packet.Packet.wValue.Value);

    Status = PutScratchOnRing(
        fdoContext,
        &packet,
        0,
        UsbdPipeTypeControl,
        USB_ENDPOINT_TYPE_CONTROL,
        FALSE);

    do
    {
        if (!NT_SUCCESS(Status))
        {
            TraceEvents(TRACE_LEVEL_ERROR, TRACE_URB,
                __FUNCTION__ ": %s putScratchOnRing failed %x\n",
                fdoContext->FrontEndPath,
                Status);
            break;
        }

        ReleaseFdoLock(fdoContext);
        Status = WaitForScratchCompletion(fdoContext);
        AcquireFdoLock(fdoContext);
        fdoContext->ConfigBusy = FALSE;

        if (Status != STATUS_SUCCESS)
        {
            TraceEvents(TRACE_LEVEL_ERROR, TRACE_URB,
                __FUNCTION__ ": %s wait failed %x\n",
                fdoContext->FrontEndPath,
                Status);
            Status = STATUS_UNSUCCESSFUL;
            break;
        }
        if (fdoContext->ScratchPad.Status != 0) // XXX what is the correct constant?
        {
            TraceEvents(TRACE_LEVEL_ERROR, TRACE_URB,
                __FUNCTION__ ": %s usb status %x returned\n",
                fdoContext->FrontEndPath,
                fdoContext->ScratchPad.Status);

            Status = STATUS_UNSUCCESSFUL;
        }
        else
        {
            // @todo tone this down when MUTT is working!
            TraceEvents(TRACE_LEVEL_WARNING, TRACE_URB,
                __FUNCTION__ ": %s interface %d Alternate %d selected\n",
                fdoContext->FrontEndPath,
                Interface,
                Alternate);
        }
        break;

    } while(1);
    
    RequestGetRequestContext(Request)->RequestCompleted = 1;
    ReleaseFdoLock(fdoContext);
    WdfRequestComplete(Request, Status);
    AcquireFdoLock(fdoContext);
    return;
}

NTSTATUS
PostProcessSelectInterface(
    IN PUSB_FDO_CONTEXT fdoContext,
    IN WDFREQUEST,
    IN PURB Urb)
{
    NTSTATUS Status = STATUS_SUCCESS;

    ASSERT(fdoContext->ConfigBusy);
    fdoContext->ConfigBusy = FALSE;

    TraceEvents(TRACE_LEVEL_VERBOSE, TRACE_URB,
        __FUNCTION__ ": %s interface %d %d complete\n",
        fdoContext->FrontEndPath,
        Urb->UrbSelectInterface.Interface.InterfaceNumber,
        Urb->UrbSelectInterface.Interface.AlternateSetting);
    return Status;
}

_Requires_lock_held_(fdoContext->WdfDevice)
VOID
ProcessAbortPipe(
    IN PUSB_FDO_CONTEXT fdoContext,
    IN WDFREQUEST Request,
    IN PURB Urb)
{
    NTSTATUS Status = STATUS_UNSUCCESSFUL;
    WDF_USB_CONTROL_SETUP_PACKET packet;

    TraceEvents(TRACE_LEVEL_VERBOSE, TRACE_URB,
        __FUNCTION__ ":  Urb %p Request %p\n",
        Urb,
        Request);

    PUSB_ENDPOINT_DESCRIPTOR endpoint = 
        PipeHandleToEndpointAddressDescriptor(
        fdoContext,
        Urb->UrbPipeRequest.PipeHandle);
    if (endpoint)
    {
        TraceEvents(TRACE_LEVEL_VERBOSE, TRACE_URB,
            __FUNCTION__ ": found ea %x direction %s\n",
            (endpoint->bEndpointAddress & ~USB_ENDPOINT_DIRECTION_MASK),
            USB_ENDPOINT_DIRECTION_IN(endpoint->bEndpointAddress) ? "In" : "Out");

        Status = STATUS_SUCCESS;
    }
    else
    {        
        RequestGetRequestContext(Request)->RequestCompleted = 1;
        ReleaseFdoLock(fdoContext);
        WdfRequestComplete(Request, STATUS_INVALID_PARAMETER);
        AcquireFdoLock(fdoContext);
        return;
    }

    RtlZeroMemory(&packet, sizeof(WDF_USB_CONTROL_SETUP_PACKET));

    PutUrbOnRing(
        fdoContext,
        &packet,
        Request,
        (USBD_PIPE_TYPE)XenUsbdPipeAbort,
        endpoint->bEndpointAddress,
        FALSE,
        FALSE);

    return;
}

_Requires_lock_held_(fdoContext->WdfDevice)
VOID
ProcessClearOrSetFeature(
    IN PUSB_FDO_CONTEXT fdoContext,
    IN WDFREQUEST Request,
    IN PURB Urb,    
    IN UCHAR requestType,
    IN UCHAR request)
{
    if (Urb->UrbControlFeatureRequest.Hdr.Length !=
        sizeof(_URB_CONTROL_FEATURE_REQUEST))
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_URB,
            __FUNCTION__ ": %s invalid length %d expected %d\n",
            fdoContext->FrontEndPath,
            Urb->UrbControlTransfer.Hdr.Length,
            sizeof(_URB_CONTROL_FEATURE_REQUEST));
        
        RequestGetRequestContext(Request)->RequestCompleted = 1;
        ReleaseFdoLock(fdoContext);
        WdfRequestComplete(Request, STATUS_INVALID_PARAMETER);
        AcquireFdoLock(fdoContext);
        return;
    }

    TraceEvents(TRACE_LEVEL_VERBOSE, TRACE_URB,
        __FUNCTION__ ": target:%x %s %s index:%x\n",
        requestType,
        request == USB_REQUEST_CLEAR_FEATURE ? "CLEAR_FEATURE" : "SET_FEATURE",
        UsbFeatureSelectorString(Urb->UrbControlFeatureRequest.FeatureSelector),
        Urb->UrbControlFeatureRequest.Index);    

    WDF_USB_CONTROL_SETUP_PACKET packet;  

    RtlZeroMemory(&packet, sizeof(WDF_USB_CONTROL_SETUP_PACKET));
    packet.Packet.bm.Byte = requestType;
    packet.Packet.bRequest = request;
    packet.Packet.wValue.Value = Urb->UrbControlFeatureRequest.FeatureSelector;
    packet.Packet.wIndex.Value = Urb->UrbControlFeatureRequest.Index;

    PutUrbOnRing(
        fdoContext,
        &packet,
        Request,
        UsbdPipeTypeControl,    // control
        USB_ENDPOINT_TYPE_CONTROL, // control OUT
        FALSE,
        FALSE);
}

_Requires_lock_held_(fdoContext->WdfDevice)
VOID
ProcessSyncResetClearStall(
    IN PUSB_FDO_CONTEXT fdoContext,
    IN WDFREQUEST Request,
    IN PURB Urb)
{
    UCHAR EndpointAddress;
    WDF_USB_CONTROL_SETUP_PACKET packet;

    TraceEvents(TRACE_LEVEL_VERBOSE, TRACE_URB,
        __FUNCTION__ ": Urb %p Request %p\n",
        Urb,
        Request);
    //
    // request uses a USB_PIPE_REQUEST - the ea is in the pipe handle
    //

    PUSB_ENDPOINT_DESCRIPTOR endpoint =
        PipeHandleToEndpointAddressDescriptor(
        fdoContext,
        Urb->UrbPipeRequest.PipeHandle);

    if (!endpoint)
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_URB,
            __FUNCTION__ ": %s URB PipeHandle %p appears invalid\n",
            fdoContext->FrontEndPath,
            Urb->UrbPipeRequest.PipeHandle);
        
        RequestGetRequestContext(Request)->RequestCompleted = 1;
        ReleaseFdoLock(fdoContext);
        WdfRequestComplete(Request, STATUS_INVALID_PARAMETER);
        AcquireFdoLock(fdoContext);;
        return;
    }
    EndpointAddress = endpoint->bEndpointAddress;

    RtlZeroMemory(&packet, sizeof(WDF_USB_CONTROL_SETUP_PACKET));
    packet.Packet.bm.Request.Dir = BMREQUEST_HOST_TO_DEVICE;
    packet.Packet.bm.Request.Type = BMREQUEST_STANDARD;
    packet.Packet.bm.Request.Recipient = BMREQUEST_TO_ENDPOINT;
    packet.Packet.bRequest = USB_REQUEST_CLEAR_FEATURE;
    packet.Packet.wValue.Value = USB_FEATURE_ENDPOINT_STALL;
    packet.Packet.wIndex.Value = EndpointAddress;
    packet.Packet.wLength = 0;

    TraceEvents(TRACE_LEVEL_VERBOSE, TRACE_URB,
        __FUNCTION__ ": USB Request header RequestType %x Request %x Value %x Index %x Length %x Request %p\n",
        packet.Packet.bm.Byte,
        packet.Packet.bRequest,
        packet.Packet.wValue.Value,
        packet.Packet.wIndex.Value,
        packet.Packet.wLength,
        Request);

    PutUrbOnRing(
        fdoContext,
        &packet,
        Request,
        UsbdPipeTypeControl,    // control
        USB_ENDPOINT_TYPE_CONTROL, // control OUT
        FALSE,
        FALSE);
}

_Requires_lock_held_(fdoContext->WdfDevice)
VOID
ProcessClassOrVendorCommand(
    IN PUSB_FDO_CONTEXT fdoContext,
    IN WDFREQUEST Request,
    IN PURB Urb,
    IN ULONG Recipient,
    IN ULONG RequestType)
{
    WDF_USB_CONTROL_SETUP_PACKET packet;

    TraceEvents(TRACE_LEVEL_VERBOSE, TRACE_URB,
        __FUNCTION__ ": recipient %x Type %x Urb %p Irp %p length %x reserved %x setup byte 0 %x\n",
        Recipient,
        RequestType,
        Urb,
        Request,
        Urb->UrbControlVendorClassRequest.TransferBufferLength,
        Urb->UrbControlVendorClassRequest.RequestTypeReservedBits,
        Urb->UrbControlTransfer.SetupPacket[0]);

    if ((RequestType != BMREQUEST_CLASS) &&
        (RequestType != BMREQUEST_VENDOR))
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_URB,
            __FUNCTION__ ": %s invalid RequestType %x\n",
            fdoContext->FrontEndPath,
            RequestType);
        
        RequestGetRequestContext(Request)->RequestCompleted = 1;
        ReleaseFdoLock(fdoContext);
        WdfRequestComplete(Request, STATUS_INVALID_PARAMETER);
        AcquireFdoLock(fdoContext);;
        return;
    }
    if (Recipient > BMREQUEST_TO_ENDPOINT)
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_URB,
            __FUNCTION__ ": %s invalid Recipient %x\n",
            fdoContext->FrontEndPath,
            Recipient);
        
        RequestGetRequestContext(Request)->RequestCompleted = 1;
        ReleaseFdoLock(fdoContext);
        WdfRequestComplete(Request, STATUS_INVALID_PARAMETER);
        AcquireFdoLock(fdoContext);;
        return;
    }

    RtlZeroMemory(&packet, sizeof(WDF_USB_CONTROL_SETUP_PACKET));   
    UCHAR control = USB_ENDPOINT_TYPE_CONTROL; // OUT
    if (Urb->UrbControlVendorClassRequest.TransferFlags & USBD_TRANSFER_DIRECTION_IN)
    {
        packet.Packet.bm.Request.Dir = BMREQUEST_DEVICE_TO_HOST;
        control |= USB_ENDPOINT_DIRECTION_MASK; //IN
    }
    else
    {
        packet.Packet.bm.Request.Dir = BMREQUEST_HOST_TO_DEVICE;
    }
    packet.Packet.bm.Request.Type = RequestType;
    packet.Packet.bm.Request.Recipient = Recipient;
    packet.Packet.bm.Request.Reserved = 0; // hid hack
    packet.Packet.bRequest = Urb->UrbControlVendorClassRequest.Request;
    packet.Packet.wValue.Value = Urb->UrbControlVendorClassRequest.Value;
    packet.Packet.wIndex.Value = Urb->UrbControlVendorClassRequest.Index;
    packet.Packet.wLength = (USHORT) Urb->UrbControlVendorClassRequest.TransferBufferLength;

    TraceEvents(TRACE_LEVEL_VERBOSE, TRACE_URB,
        __FUNCTION__ ": bm:%x Req:%x Val:%x Ind:%x Len:%x\n",
        packet.Packet.bm.Byte,
        packet.Packet.bRequest,
        packet.Packet.wValue.Value,
        packet.Packet.wIndex.Value,
        packet.Packet.wLength);
    //
    // HID_SET_IDLE seems problematic (packet.Packet.bRequest == 0x0A).
    //
    BOOLEAN shortOk = packet.Packet.bm.Request.Dir == BMREQUEST_DEVICE_TO_HOST ? TRUE : FALSE;

    PutUrbOnRing(
        fdoContext,
        &packet,
        Request,
        UsbdPipeTypeControl,
        control,
        TRUE,
        shortOk );
}

_Requires_lock_held_(fdoContext->WdfDevice)
VOID
OsFeatureDescriptorCommand(
    IN PUSB_FDO_CONTEXT fdoContext,
    IN WDFREQUEST Request,
    IN PURB Urb)
{
    TraceEvents(TRACE_LEVEL_VERBOSE, TRACE_URB,
        __FUNCTION__ ": Urb %p Request %p length %x InterfaceNumber %d PageIndex %d  MS_FeatureDescriptorIndex %d\n",
        Urb,
        Request,
        Urb->UrbOSFeatureDescriptorRequest.TransferBufferLength,
        Urb->UrbOSFeatureDescriptorRequest.InterfaceNumber,
        Urb->UrbOSFeatureDescriptorRequest.MS_PageIndex,
        Urb->UrbOSFeatureDescriptorRequest.MS_FeatureDescriptorIndex);

    if (!fdoContext->OsDescriptorString)
    {
        TraceEvents(TRACE_LEVEL_VERBOSE, TRACE_URB,
            __FUNCTION__": MS_FEATURE_DESCRIPTOR InterfaceNumber %d PageIndex %d  MS_FeatureDescriptorIndex %d not supported for this device\n",
            Urb->UrbOSFeatureDescriptorRequest.InterfaceNumber,
            Urb->UrbOSFeatureDescriptorRequest.MS_PageIndex,
            Urb->UrbOSFeatureDescriptorRequest.MS_FeatureDescriptorIndex);
        //
        // not supported
        //      
        RequestGetRequestContext(Request)->RequestCompleted = 1;  
        ReleaseFdoLock(fdoContext);
        WdfRequestComplete(Request, STATUS_UNSUCCESSFUL);
        AcquireFdoLock(fdoContext);;
        return;
    }

    WDF_USB_CONTROL_SETUP_PACKET packet = formatOsFeaturePacket(
        fdoContext->OsDescriptorString->osDescriptor.bVendorCode,
        Urb->UrbOSFeatureDescriptorRequest.InterfaceNumber,
        Urb->UrbOSFeatureDescriptorRequest.MS_PageIndex,
        Urb->UrbOSFeatureDescriptorRequest.MS_FeatureDescriptorIndex,
        (USHORT) Urb->UrbControlVendorClassRequest.TransferBufferLength);    

    TraceEvents(TRACE_LEVEL_VERBOSE, TRACE_URB,
        __FUNCTION__": bm:%x Req:%x Val:%x Ind:%x Len:%x\n",
        packet.Packet.bm.Byte,
        packet.Packet.bRequest,
        packet.Packet.wValue.Value,
        packet.Packet.wIndex.Value,
        packet.Packet.wLength);

    PutUrbOnRing(
        fdoContext,
        &packet,
        Request,
        UsbdPipeTypeControl,
        USB_ENDPOINT_DIRECTION_MASK,
        TRUE,
        FALSE);
}

_Requires_lock_held_(fdoContext->WdfDevice)
VOID
ProcessGetDescriptor(
    IN UCHAR Recipient,
    IN PUSB_FDO_CONTEXT fdoContext,
    IN WDFREQUEST Request,
    IN PURB Urb)
{
    WDF_USB_CONTROL_SETUP_PACKET packet;
    RtlZeroMemory(&packet, sizeof(WDF_USB_CONTROL_SETUP_PACKET));

    packet.Packet.bm.Request.Dir = BMREQUEST_DEVICE_TO_HOST;
    packet.Packet.bm.Request.Type = BMREQUEST_STANDARD;
    packet.Packet.bm.Request.Recipient = Recipient;
    packet.Packet.bRequest = USB_REQUEST_GET_DESCRIPTOR;
    packet.Packet.wValue.Bytes.HiByte = Urb->UrbControlDescriptorRequest.DescriptorType;
    packet.Packet.wValue.Bytes.LowByte = Urb->UrbControlDescriptorRequest.Index;
    packet.Packet.wIndex.Value = Urb->UrbControlDescriptorRequest.LanguageId;
    packet.Packet.wLength = (USHORT) Urb->UrbControlDescriptorRequest.TransferBufferLength;


    TraceEvents(TRACE_LEVEL_VERBOSE, TRACE_DEVICE,
        __FUNCTION__": %s (%x) Recipient %x Value %x Index %x Length %x Request %p\n",
        DescriptorTypeToString(packet.Packet.wValue.Bytes.HiByte),
        packet.Packet.wValue.Bytes.HiByte,
        packet.Packet.bm.Request.Recipient,
        packet.Packet.wValue.Value,
        packet.Packet.wIndex.Value,
        packet.Packet.wLength,
        Request);

    PutUrbOnRing(
        fdoContext,
        &packet,
        Request,
        UsbdPipeTypeControl,    // control
        USB_ENDPOINT_TYPE_CONTROL | USB_ENDPOINT_DIRECTION_MASK, //!< control IN
        TRUE,
        TRUE);
}

_Requires_lock_held_(fdoContext->WdfDevice)
VOID
ProcessGetStatus(
    IN PUSB_FDO_CONTEXT fdoContext,
    IN WDFREQUEST Request,
    IN PURB Urb,
    IN ULONG Recipient)
{
    WDF_USB_CONTROL_SETUP_PACKET packet;

    TraceEvents(TRACE_LEVEL_VERBOSE, TRACE_URB,
        __FUNCTION__": recipient %x Urb %p Request %p length %x\n",
        Recipient,
        Urb,
        Request,
        Urb->UrbControlGetStatusRequest.TransferBufferLength);


    if (Recipient > BMREQUEST_TO_OTHER)
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_URB,
            __FUNCTION__": %s invalid Recipient %x\n",
            fdoContext->FrontEndPath,
            Recipient);
        
        RequestGetRequestContext(Request)->RequestCompleted = 1;
        ReleaseFdoLock(fdoContext);
        WdfRequestComplete(Request, STATUS_INVALID_PARAMETER);
        AcquireFdoLock(fdoContext);;
        return;
    }
    if (Urb->UrbControlGetStatusRequest.TransferBufferLength < 2)
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_URB,
            __FUNCTION__": %s invalid length %d\n",
            fdoContext->FrontEndPath,
            Urb->UrbControlGetStatusRequest.TransferBufferLength);
        
        RequestGetRequestContext(Request)->RequestCompleted = 1;
        ReleaseFdoLock(fdoContext);
        WdfRequestComplete(Request, STATUS_INVALID_PARAMETER);
        AcquireFdoLock(fdoContext);;
        return;
    }

    UCHAR control = USB_ENDPOINT_TYPE_CONTROL | USB_ENDPOINT_DIRECTION_MASK; // IN

    RtlZeroMemory(&packet, sizeof(WDF_USB_CONTROL_SETUP_PACKET));

    packet.Packet.bm.Request.Recipient = Recipient;
    packet.Packet.bm.Request.Type = BMREQUEST_STANDARD;
    packet.Packet.bm.Request.Dir = BMREQUEST_DEVICE_TO_HOST;
    packet.Packet.bRequest = USB_REQUEST_GET_STATUS;
    packet.Packet.wValue.Value = 0;
    packet.Packet.wIndex.Value = Urb->UrbControlGetStatusRequest.Index;
    packet.Packet.wLength = 2;

    TraceEvents(TRACE_LEVEL_VERBOSE, TRACE_URB,
        __FUNCTION__": bm:%x Req:%x Val:%x Ind:%x Len:%x\n",
        packet.Packet.bm.Byte,
        packet.Packet.bRequest,
        packet.Packet.wValue.Value,
        packet.Packet.wIndex.Value,
        packet.Packet.wLength);

    PutUrbOnRing(
        fdoContext,
        &packet,
        Request,
        UsbdPipeTypeControl,
        control,
        TRUE,
        FALSE);
}

//
// We reserve a request for resets.
//
_Requires_lock_held_(fdoContext->WdfDevice)
VOID
ProcessResetRequest(
    IN PUSB_FDO_CONTEXT fdoContext,
    IN WDFREQUEST Request)
{
    TraceEvents(TRACE_LEVEL_ERROR, TRACE_URB,
        __FUNCTION__": %s Device %p Request %p\n",
        fdoContext->FrontEndPath,
        fdoContext->WdfDevice,
        Request);

    PutResetOrCycleUrbOnRing(
        fdoContext,
        Request,
        TRUE);
}

/**
* @brief Called by the framework when a request has been cancelled and
* that request is currently in progress ("on the hardware" i.e. owned by DOM0.)
* This function is responsible for initiating an abort of the request.
* It does this by either queuing an AbortEndpointWorker request if there is
* an endpoint, or a ResetDeviceWorker if there isn't, or failing that completing
* the request itself.
*
* @param[in] Request the handle to the WdfRequest object to cancel.
*
*/
VOID
EvtFdoOnHardwareRequestCancelled(
    IN WDFREQUEST  Request)
{
    PUSB_FDO_CONTEXT fdoContext = DeviceGetFdoContext(
        WdfIoQueueGetDevice( WdfRequestGetIoQueue(Request)));

    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DEVICE,
        __FUNCTION__": %s Device %p Request %p OnRingbuffer %d\n",
        fdoContext->FrontEndPath,
        fdoContext->WdfDevice,
        Request,
        OnRingBuffer(fdoContext->Xen));

    AcquireFdoLock(fdoContext);
    //
    // Do not set request context CancelSet to zero until the Irp is completed,
    // otherwise the hardware completion handler will complete the request
    // thinking that it doesn't have a cancel handler.
    //
    if (fdoContext->DeviceUnplugged)
    {
        FreeShadowForRequest(fdoContext->Xen,
            Request);
        RequestGetRequestContext(Request)->RequestCompleted = 1; 
        RequestGetRequestContext(Request)->CancelSet = 0;
        ReleaseFdoLock(fdoContext);
        WdfRequestComplete(Request, STATUS_CANCELLED);
        // @todo there is still a race condition here
        WdfObjectDereference(Request);
        TraceEvents(TRACE_LEVEL_WARNING, TRACE_DEVICE,
            __FUNCTION__": unplugged cancellation of request %p\n",
            Request);
        return;
    }
    //
    // make sure this is a URB request.
    //
    do 
    {
        WDF_REQUEST_PARAMETERS Parameters;
        WDF_REQUEST_PARAMETERS_INIT(&Parameters);
        WdfRequestGetParameters(Request, &Parameters);

        if (Parameters.Type != WdfRequestTypeDeviceControlInternal)
        {
            TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
                __FUNCTION__": %s Parameters.Type %x expected %x\n",
                fdoContext->FrontEndPath,
                Parameters.Type,
                WdfRequestTypeDeviceControlInternal);
            break;
        }

        if (Parameters.Parameters.DeviceIoControl.IoControlCode != IOCTL_INTERNAL_USB_SUBMIT_URB)
        {
            TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
                __FUNCTION__": %s IoControlCode %x expected %x\n",
                fdoContext->FrontEndPath,
                Parameters.Parameters.DeviceIoControl.IoControlCode,
                IOCTL_INTERNAL_USB_SUBMIT_URB);
            break;
        }
        //
        // we have a urb so find an endpoint and do an abort endpoint,
        //
        PURB Urb = (PURB) URB_FROM_REQUEST(Request);
        PIPE_DESCRIPTOR * pipe = (PIPE_DESCRIPTOR *) Urb->UrbPipeRequest.PipeHandle;
        PUSB_ENDPOINT_DESCRIPTOR endpoint = 
            PipeHandleToEndpointAddressDescriptor(fdoContext, Urb->UrbPipeRequest.PipeHandle);
        PCHAR UrbFuncString = UrbFunctionToString(Urb->UrbHeader.Function);
        //
        // if there is no valid endpoint we can't abort it.
        //
        if (!endpoint)
        {
            TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
                __FUNCTION__": %s Device %p no endpoint for Request %p %s\n",
                fdoContext->FrontEndPath,
                fdoContext->WdfDevice,
                Request,
                UrbFuncString);
            break;
        }

        ULONG doAbort = TRUE;

        if (pipe->abortInProgress)
        {
            // let the current abort handle things.
            TraceEvents(TRACE_LEVEL_WARNING, TRACE_DEVICE,
                __FUNCTION__ ": %s Device %p Request %p EA %x abort in progress wait for abort completion.\n",
                fdoContext->FrontEndPath,
                fdoContext->WdfDevice,
                Request,
                endpoint->bEndpointAddress);
            doAbort = FALSE;
        }

        WDFWORKITEM worker = NewWorkItem(fdoContext,
            doAbort ? AbortEndpointWorker : AbortEndpointWaitWorker,
            (ULONG_PTR) endpoint->bEndpointAddress,
            (ULONG_PTR) Request,
            (ULONG_PTR) pipe, 
            0);

        if (worker)
        {
            pipe->abortInProgress = TRUE;
            if (doAbort)
            {
                KeClearEvent(&pipe->abortCompleteEvent);
                pipe->abortWaiters = 0;
            }
            else
            {
                pipe->abortWaiters++;
            }

            TraceEvents(TRACE_LEVEL_WARNING, TRACE_DEVICE,
                __FUNCTION__ ": %s Device %p queue %s for Request %p EA %x %s waiters %d\n",
                fdoContext->FrontEndPath,
                fdoContext->WdfDevice,
                doAbort ? "AbortEndpointWorker" : "AbortEndpointWaitWorker",
                Request,
                endpoint->bEndpointAddress,
                UrbFuncString,
                pipe->abortWaiters);

            WdfWorkItemEnqueue(worker);
            ReleaseFdoLock(fdoContext);
            return;
        }
        //
        // oh great no memory.
        // reserve more work items!
        //
        RequestGetRequestContext(Request)->RequestCompleted = 1;   
        RequestGetRequestContext(Request)->CancelSet = 0;
        ReleaseFdoLock(fdoContext);

        TraceEvents(TRACE_LEVEL_WARNING, TRACE_DEVICE,
            __FUNCTION__ ": %s Device %p no work items for abort pipe of cancelled Request %p\n",
            fdoContext->FrontEndPath,
            fdoContext->WdfDevice,
            Request);
        WdfRequestComplete(Request, STATUS_CANCELLED);
        return;

    } while (0);

    // just reset the entire device, but this should not happen.

    if (fdoContext->ResetInProgress)
    {
        XXX_TODO("like aborts, this needs to have a waitforresetworker for each request.");        
        RequestGetRequestContext(Request)->RequestCompleted = 1;  
        RequestGetRequestContext(Request)->CancelSet = 0;
        FreeShadowForRequest(fdoContext->Xen, Request);
        ReleaseFdoLock(fdoContext);
        WdfRequestComplete(Request, STATUS_CANCELLED);
        return;
    }
    //
    // this request has to be processed at passive level
    //
    WDFWORKITEM worker = NewWorkItem(fdoContext,
        ResetDeviceWorker,
        (ULONG_PTR) Request,
        0,0,0);

    if (worker)
    {
        fdoContext->ResetInProgress = TRUE;
        TraceEvents(TRACE_LEVEL_WARNING, TRACE_DEVICE,
            __FUNCTION__ ": %s Device %p queue ResetDeviceWorker for Request %p \n",
            fdoContext->FrontEndPath,
            fdoContext->WdfDevice,
            Request);
        WdfWorkItemEnqueue(worker);
        ReleaseFdoLock(fdoContext);
        return;
    }


    TraceEvents(TRACE_LEVEL_WARNING, TRACE_DEVICE,
        __FUNCTION__ ": %s Device %p no work items for reset device for cancelled Request %p\n",
        fdoContext->FrontEndPath,
        fdoContext->WdfDevice,
        Request);

    FreeShadowForRequest(fdoContext->Xen, Request);
    RequestGetRequestContext(Request)->RequestCompleted = 1;     
    RequestGetRequestContext(Request)->CancelSet = 0;
    ReleaseFdoLock(fdoContext);
    WdfRequestComplete(Request, STATUS_CANCELLED);
    return;
}


/**
* @brief Passive level worker routine for EvtFdoOnHardwareRequestCancelled().
* This version issues a reset instead of an abort as there is no endpoint to
* abort. Like the two other variations on cancel workres, this routine is responsible
* for completing the Request and cleaning up the resources associated with the
* request.
*
* @todo a call to WdfRequestIsCanceled (done by the dpc routine) after the
* request has been completed will bugcheck because the IRP in the request is 
* NULL, which is stupid but it is what it is. This is true even if the request
* itself is valid (as for example its reference count was incremented in a mistaken
* effort to avoid this race condition.) Grrrr.... instead we actually need to add
* a context for the 'on hardware requests' and use that to provide the equivalent
* to WdfRequestIsCanceled, only it is "WdfRequestHasValidIrp".
*
* @param[in] WorkItem a handle to a WDFWORKITEM object.
*
*/
VOID
ResetDeviceWorker(
    IN WDFWORKITEM WorkItem)
{    
    PUSB_FDO_WORK_ITEM_CONTEXT  context = WorkItemGetContext(WorkItem);
    AcquireFdoLock(context->FdoContext);    
    WDFREQUEST Request = (WDFREQUEST) context->Params[0];

    TraceEvents(TRACE_LEVEL_VERBOSE, TRACE_DEVICE,
        __FUNCTION__ ": %s Device %p requests on ringbuffer %d\n",
        context->FdoContext->FrontEndPath,
        context->FdoContext->WdfDevice,
        OnRingBuffer(context->FdoContext->Xen));    

    if (context->FdoContext->DeviceUnplugged)
    {
        //
        // don't reset shot devices, and cleanup the request.
        //
        TraceEvents(TRACE_LEVEL_WARNING, TRACE_DEVICE,
            __FUNCTION__ ": %s Device %p On entry, already unplugged don't reset\n",
            context->FdoContext->FrontEndPath,
            context->FdoContext->WdfDevice);

        FreeShadowForRequest(context->FdoContext->Xen, Request);
        context->FdoContext->ResetInProgress = FALSE;
        RequestGetRequestContext(Request)->RequestCompleted = 1; 
        RequestGetRequestContext(Request)->CancelSet = 0;
        ReleaseFdoLock(context->FdoContext);
        WdfRequestComplete(Request, STATUS_CANCELLED);
        return;
    }
    //
    // we need the scratchpad, so wait for it.
    //
    if (!WaitForScratchPadAccess(context->FdoContext))
    {
        // ugh. Just complete the request.
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
            __FUNCTION__ ": %s Device %p Config Busy, can't reset request %p\n",
            context->FdoContext->FrontEndPath,
            context->FdoContext->WdfDevice,
            Request);
    }
    else
    {
        ReleaseFdoLock(context->FdoContext);
        NTSTATUS Status = ResetDevice(context->FdoContext);
        AcquireFdoLock(context->FdoContext);

        if (!NT_SUCCESS(Status))
        {
            TraceEvents(TRACE_LEVEL_WARNING, TRACE_DEVICE,
                __FUNCTION__ ": %s Device %p reset failed %x\n",
                context->FdoContext->FrontEndPath,
                context->FdoContext->WdfDevice,
                Status);
        }
        else
        {
            TraceEvents(TRACE_LEVEL_WARNING, TRACE_DEVICE,
                __FUNCTION__ ": %s Device %p reset succeeded. Requests on ringbuffer: %d\n",
                context->FdoContext->FrontEndPath,
                context->FdoContext->WdfDevice,
                OnRingBuffer(context->FdoContext->Xen));
        }
        context->FdoContext->ConfigBusy = FALSE;
    }
    context->FdoContext->ResetInProgress = FALSE;
    FreeShadowForRequest(context->FdoContext->Xen, Request);
    RequestGetRequestContext(Request)->RequestCompleted = 1; 
    RequestGetRequestContext(Request)->CancelSet = 0;
    ReleaseFdoLock(context->FdoContext);
    WdfRequestComplete(Request, STATUS_CANCELLED);
}

/**
* @brief Passive level worker routine for EvtFdoOnHardwareRequestCancelled().
* This version does not issue the abort, one has already been issued,
* it just waits for the abort event to be signalled. The other abort
* worker, AbortEndpointWorker() issues the abort. Both versions are responsible
* completing the Request and cleaning up the resources associated with the
* request.
*
* @param[in] WorkItem a handle to a WDFWORKITEM object.
*
*/
VOID 
AbortEndpointWaitWorker(
    IN WDFWORKITEM WorkItem)
{    
    PUSB_FDO_WORK_ITEM_CONTEXT  context = WorkItemGetContext(WorkItem);
    AcquireFdoLock(context->FdoContext);

    UCHAR endpointAddress = (UCHAR) context->Params[0];
    WDFREQUEST Request = (WDFREQUEST) context->Params[1];
    PIPE_DESCRIPTOR * pipe = (PIPE_DESCRIPTOR *) context->Params[2];

    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DEVICE,
        __FUNCTION__ ": %s Device %p ea %x request %p pipe %p\n",
        context->FdoContext->FrontEndPath,
        context->FdoContext->WdfDevice,
        endpointAddress,
        Request,
        pipe);
    TRY
    {
        if (context->FdoContext->DeviceUnplugged)
        {
            //
            // don't abortpipe unplugged devices
            //
            TraceEvents(TRACE_LEVEL_WARNING, TRACE_DEVICE,
                __FUNCTION__ ": %s Device %p On entry, already unplugged don't abort\n",
                context->FdoContext->FrontEndPath,
                context->FdoContext->WdfDevice);
            LEAVE;
        }

        ASSERT(pipe);
        if (!pipe)
        {
            TraceEvents(TRACE_LEVEL_WARNING, TRACE_DEVICE,
                __FUNCTION__ ": %s Device %p No pipe for ea %x dev obj %p\n",
                context->FdoContext->FrontEndPath,
                context->FdoContext->WdfDevice,
                endpointAddress);
            LEAVE;
        }

        ASSERT(pipe->abortInProgress);
        if (!pipe->abortInProgress)
        {        
            TraceEvents(TRACE_LEVEL_WARNING, TRACE_DEVICE,
                __FUNCTION__ ": %s Device %p abortInProgress not set for ea %x\n",
                context->FdoContext->FrontEndPath,
                context->FdoContext->WdfDevice,
                endpointAddress);
            LEAVE;
        }
        //
        // wait for the abort owner to finish.
        // The abort worker has to guarantee that this event is signalled.
        //
        ReleaseFdoLock(context->FdoContext);
        NTSTATUS Status = KeWaitForSingleObject(&pipe->abortCompleteEvent, Executive, KernelMode, FALSE, NULL);  
        AcquireFdoLock(context->FdoContext);
        //
        // we don't really care what the status is, but trace a return value other than STATUS_SUCCESS;
        //
        if (Status != STATUS_SUCCESS)
        {
            TraceEvents(TRACE_LEVEL_WARNING, TRACE_DEVICE,
                __FUNCTION__ ": %s Device %p wait for abort complete error %x\n",
                context->FdoContext->FrontEndPath,
                context->FdoContext->WdfDevice,
                Status);
        }
        if (pipe->abortWaiters == 1)
        {
            KeClearEvent(&pipe->abortCompleteEvent);
            pipe->abortInProgress = FALSE;
        }
        pipe->abortWaiters--;

    }
    FINALLY
    {    
        FreeShadowForRequest(context->FdoContext->Xen, Request);
        RequestGetRequestContext(Request)->RequestCompleted = 1; 
        RequestGetRequestContext(Request)->CancelSet = 0;
        ReleaseFdoLock(context->FdoContext);
        WdfRequestComplete(Request, STATUS_CANCELLED);
    }
}

/**
* @brief Passive level worker routine for EvtFdoOnHardwareRequestCancelled().
* This version issues the abort and waits for it to complete.
*
* @param[in] WorkItem a handle to a WDFWORKITEM object.
*
*/
VOID
AbortEndpointWorker(
    IN WDFWORKITEM WorkItem)
{    
    PUSB_FDO_WORK_ITEM_CONTEXT  context = WorkItemGetContext(WorkItem);
    AcquireFdoLock(context->FdoContext);
    PIPE_DESCRIPTOR * pipe = (PIPE_DESCRIPTOR *) context->Params[2];

    UCHAR endpointAddress = (UCHAR) context->Params[0];
    WDFREQUEST Request = (WDFREQUEST) context->Params[1];

    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DEVICE,
        __FUNCTION__ ": %s Device %p ea %x request %p pipe %p\n",
        context->FdoContext->FrontEndPath,
        context->FdoContext->WdfDevice,
        endpointAddress,
        Request,
        pipe);    

    NTSTATUS Status = STATUS_SUCCESS;

    TRY
    {
        if (context->FdoContext->DeviceUnplugged)
        {
            //
            // don't abortpipe unplugged devices
            //
            TraceEvents(TRACE_LEVEL_WARNING, TRACE_DEVICE,
                __FUNCTION__ ": %s Device %p On entry, already unplugged don't abort\n",
                context->FdoContext->FrontEndPath,
                context->FdoContext->WdfDevice);
            LEAVE;
        }

        ASSERT(pipe);
        if (!pipe)
        {
            TraceEvents(TRACE_LEVEL_WARNING, TRACE_DEVICE,
                __FUNCTION__ ": %s Device %p No pipe for ea %x dev obj %p\n",
                context->FdoContext->FrontEndPath,
                context->FdoContext->WdfDevice,
                endpointAddress);
            LEAVE;
        }

        ASSERT(pipe->abortInProgress);
        if (!pipe->abortInProgress)
        {        
            TraceEvents(TRACE_LEVEL_WARNING, TRACE_DEVICE,
                __FUNCTION__ ": %s Device %p abortInProgress not set for ea %x pipe %p\n",
                context->FdoContext->FrontEndPath,
                context->FdoContext->WdfDevice,
                endpointAddress,
                pipe);
            pipe = NULL;
            LEAVE;
        }
        //
        // we need the scratchpad, so wait for it.
        //
        if (!WaitForScratchPadAccess(context->FdoContext))
        {
            TraceEvents(TRACE_LEVEL_WARNING, TRACE_DEVICE,
                __FUNCTION__ ": %s Device %p Config Busy, can't abort pipe\n",
                context->FdoContext->FrontEndPath,
                context->FdoContext->WdfDevice);
            pipe = NULL;
            LEAVE;
        }

        WDF_USB_CONTROL_SETUP_PACKET packet;
        RtlZeroMemory(&packet, sizeof(WDF_USB_CONTROL_SETUP_PACKET));

        TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DEVICE,
            __FUNCTION__ ": %s Device %p endpoint %x\n",
            context->FdoContext->FrontEndPath,
            context->FdoContext->WdfDevice,
            endpointAddress);

        Status = PutScratchOnRing(
            context->FdoContext,
            &packet,
            0,
            (USBD_PIPE_TYPE)XenUsbdPipeAbort,
            endpointAddress,
            FALSE);

        if (!NT_SUCCESS(Status))
        {
            TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
                __FUNCTION__ ": %s Device %p PutScratchOnRing error %x\n",
                context->FdoContext->FrontEndPath,
                Status);            
            context->FdoContext->ConfigBusy = FALSE;
            LEAVE;
        }

        ReleaseFdoLock(context->FdoContext);
        Status = WaitForScratchCompletion(context->FdoContext);
        AcquireFdoLock(context->FdoContext);          

        context->FdoContext->ConfigBusy = FALSE;

        if (Status != STATUS_SUCCESS)
        {
            TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
                __FUNCTION__ ": %s Device %p WaitForScratchCompletion failed %x unplugged %d\n",
                context->FdoContext->FrontEndPath,
                context->FdoContext,
                Status);
            if (context->FdoContext->DeviceUnplugged)
            {
                LEAVE;
            }
            //
            // screw it try a reset.
            //
            ReleaseFdoLock(context->FdoContext);
            Status = ResetDevice(context->FdoContext);
            AcquireFdoLock(context->FdoContext);
            LEAVE;
        }

        if (context->FdoContext->ScratchPad.Status != 0) // XXX what is the correct constant?
        {
            TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
                __FUNCTION__ ": %s Device %p usb status %x returned\n",
                context->FdoContext->FrontEndPath,
                context->FdoContext,
                context->FdoContext->ScratchPad.Status);
            Status = STATUS_UNSUCCESSFUL;
        }

        TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DEVICE,
            __FUNCTION__ ": %s Device %p abort pipe %p request for %x succeeded abortWaiters %d\n",
            context->FdoContext->FrontEndPath,
            context->FdoContext,
            pipe,
            endpointAddress,
            pipe->abortWaiters);

    } 
    FINALLY
    {
        if (pipe)
        {
            // if we failed make sure the waiters fail too.
            KeSetEvent(&pipe->abortCompleteEvent, 0, FALSE);
            if (!pipe->abortWaiters)
            {
                pipe->abortInProgress = FALSE;
                KeClearEvent(&pipe->abortCompleteEvent);
            }
        }
        FreeShadowForRequest(context->FdoContext->Xen, Request);
        RequestGetRequestContext(Request)->RequestCompleted = 1; 
        RequestGetRequestContext(Request)->CancelSet = 0;
        ReleaseFdoLock(context->FdoContext);
        WdfRequestComplete(Request, STATUS_CANCELLED);
    }
}
