//
// Copyright (c) Citrix Systems, Inc., All rights reserved.
//
/// @file UsbResponse.cpp USB high level response processing.
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
#include "UsbResponse.h"
#include "UsbConfig.h"

NTSTATUS
PostProcessSelectInterface(
    IN PUSB_FDO_CONTEXT fdoContext,
    PURB Urb);

NTSTATUS
PostProcessSelectConfig(
    IN PUSB_FDO_CONTEXT fdoContext,
    IN PURB Urb);

VOID
PostProcessScratch(
    IN PUSB_FDO_CONTEXT fdoContext, 
    IN NTSTATUS usbdStatus,
    IN PCHAR usbifStatusString,
    IN PCHAR usbdStatusString,
    IN ULONG BytesTransferred, 
    IN ULONG Data)
{
    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DPC,
        __FUNCTION__": Completing scratch pad request\n");

    fdoContext->ScratchPad.Status = usbdStatus;
    fdoContext->ScratchPad.BytesTransferred = BytesTransferred;

    if (fdoContext->ScratchPad.Request == XenUsbdGetCurrentFrame)
    {
        fdoContext->ScratchPad.FrameNumber = Data;
    }
    else
    {
        fdoContext->ScratchPad.Data = Data;
    }
    if (fdoContext->ScratchPad.Status != USBD_STATUS_SUCCESS)
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DPC,
            __FUNCTION__": %s Scratch request error %x usbif %s usbd %s\n",
            fdoContext->FrontEndPath,
            usbdStatus,
            usbifStatusString,
            usbdStatusString);
    }
    KeSetEvent(&fdoContext->ScratchPad.CompletionEvent, IO_NO_INCREMENT, FALSE);
}

//
// handle descriptor and interface operations here
// in order to provide enough of a usbport emulation
// to convince the usb function drivers above us to
// talk to us. In particular PipeHandles have to be
// provided upwards that map to endpoint addresses used
// by the backend DOM 0 driver.
//
NTSTATUS
PostProcessUrb(
    IN PUSB_FDO_CONTEXT fdoContext,
    IN PURB Urb,
    IN NTSTATUS *usbdStatus,
    IN ULONG bytesTransferred,
    IN ULONG startFrame,
    IN PVOID isoPacketDescriptor)
{
    NTSTATUS Status = USBD_SUCCESS(*usbdStatus) ? STATUS_SUCCESS : STATUS_UNSUCCESSFUL;
    if (*usbdStatus == USBD_STATUS_CANCELED)
    {
        //
        // special case this.
        // perhaps it would be better to select the NT status in the map backend to frontend
        // usb status conversion routine?
        //
        Status = STATUS_CANCELLED;
    }
    ULONG TraceLevel = NT_SUCCESS(Status) ? TRACE_LEVEL_VERBOSE : TRACE_LEVEL_INFORMATION;
    
    TraceEvents(TraceLevel, TRACE_URB,
        __FUNCTION__": %s Device %p Status %x UsbdStatus %x Function %s bytesTransferred %d\n",
        fdoContext->FrontEndPath,
        fdoContext->WdfDevice,
        Status,
        *usbdStatus,
        UrbFunctionToString(Urb->UrbHeader.Function),
        bytesTransferred);

    switch (Urb->UrbHeader.Function)
    {
        //
        // UsbBuildGetDescriptorRequest
        //
    case URB_FUNCTION_GET_DESCRIPTOR_FROM_DEVICE:
        {
            Urb->UrbControlDescriptorRequest.TransferBufferLength = bytesTransferred;
            if (NT_SUCCESS(Status))
            {
                //
                // capture the descriptor data on return
                //
                PVOID buffer = Urb->UrbControlDescriptorRequest.TransferBuffer;
                //
                // just copy the descriptor, there can only be one.
                //
                if (!buffer)
                {
                    //
                    // ugh first get the systemva then do the copy.
                    //
                    buffer = MmGetSystemAddressForMdlSafe(
                        Urb->UrbControlDescriptorRequest.TransferBufferMDL,
                        NormalPagePriority);
                    if (!buffer)
                    {
                        //
                        // XXX STUB error handling here!
                        // we preallocated the sysva for the mdl,
                        // so this should never happen
                        //
                        break;
                    }
                }
                switch (Urb->UrbControlDescriptorRequest.DescriptorType)
                {
                case USB_DEVICE_DESCRIPTOR_TYPE:
                    memcpy(&fdoContext->DeviceDescriptor, buffer, 
                        sizeof(fdoContext->DeviceDescriptor));
                    break;

                case USB_CONFIGURATION_DESCRIPTOR_TYPE:
                    if (bytesTransferred >= sizeof(USB_CONFIGURATION_DESCRIPTOR))
                    {
                        TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DPC,
                            __FUNCTION__": Config descriptor returned length %d\n",
                            bytesTransferred);                  
                    }
                    break;

                case USB_STRING_DESCRIPTOR_TYPE:
                    if (bytesTransferred >= sizeof(USB_STRING_DESCRIPTOR))
                    {
                        PUSB_STRING_DESCRIPTOR stringDescriptor =
                            (PUSB_STRING_DESCRIPTOR) buffer;
                        //
                        // stay away from wchar strings at raised IRQL.
                        //
                        TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DPC,
                            __FUNCTION__": usb string: length %d type %d\n",
                            stringDescriptor->bLength,
                            stringDescriptor->bDescriptorType);
                    }
                    else
                    {
                        //
                        // hmmmm... this ought to be an error?
                        //
                        TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DPC,
                            __FUNCTION__": usb string: bytesTransferred(%d) < USB_STRING_DESCRIPTOR(%d)\n",
                            bytesTransferred,
                            sizeof(USB_STRING_DESCRIPTOR));
                    }
                    break;
                case USB_INTERFACE_DESCRIPTOR_TYPE:
                case USB_ENDPOINT_DESCRIPTOR_TYPE:
                default:
                    //
                    // XXX unimplemented
                    //
                    break;
                }
            }
        }
        break;

        // UsbBuildSelectConfigurationRequest
    case URB_FUNCTION_SELECT_CONFIGURATION:
        //
        // Fill in the URB for the client
        //
        if (NT_SUCCESS(Status))
        {
            Status = PostProcessSelectConfig(fdoContext, Urb);
        }
        if (!NT_SUCCESS(Status))
        {
            if (fdoContext->ConfigBusy)
            {
                fdoContext->ConfigBusy = FALSE;
            }
            //
            // zero out the data?
            //
            RtlZeroMemory(Urb, Urb->UrbHeader.Length);
        }
        break;

        
    case URB_FUNCTION_SELECT_INTERFACE:
        //
        // capture the interface data on return and
        // provide a mapping from endpoint to pipehandle
        //

        /////////////////////////////////////////////////////////////////////////////////////
        //
        // It is valid for the device to set a pipe to stalled during the
        // select interface operation when an error is believed to have
        // occurred. This is true when there is only one interface available.
        //
        // Comment from the backend USB code:
        //
        // 9.4.10 (of the USB spec) says devices don't need this and are free to STALL the
        // request if the interface only has one alternate setting.
        //
        // So to handle this situation, we set the status code to indicate success
        // when these conditons are met.
        //
        /////////////////////////////////////////////////////////////////////////////////////
        if ((*usbdStatus == USBD_STATUS_STALL_PID) && (fdoContext->NumInterfaces == 1))
        {
            TraceEvents(TRACE_LEVEL_WARNING, TRACE_DPC,
                __FUNCTION__": USB pipe stalled during select config...clearing failure status code");
            *usbdStatus = USBD_STATUS_SUCCESS;
            Urb->UrbHeader.Status = *usbdStatus;
            Status = STATUS_SUCCESS;
        }

        if (NT_SUCCESS(Status))
        {
            Status = PostProcessSelectInterface(fdoContext, Urb);
        }
        if (!NT_SUCCESS(Status))
        {
            if (fdoContext->ConfigBusy)
            {
                fdoContext->ConfigBusy = FALSE;
            }
            //
            // zero out the data?
            //
            RtlZeroMemory(Urb, Urb->UrbHeader.Length);
        }
        break;

    case URB_FUNCTION_BULK_OR_INTERRUPT_TRANSFER:
    case URB_FUNCTION_CONTROL_TRANSFER:
    case URB_FUNCTION_CONTROL_TRANSFER_EX:
        Urb->UrbControlTransfer.TransferBufferLength = bytesTransferred;
        break;

    case URB_FUNCTION_ISOCH_TRANSFER:
        if (isoPacketDescriptor)
        {
            Status = XenPostProcessIsoResponse(Urb,
                usbdStatus,
                bytesTransferred,
                startFrame,
                isoPacketDescriptor,
                Status);
        }
        else
        {
            TraceEvents(TRACE_LEVEL_WARNING, TRACE_DPC,
                __FUNCTION__": No isoPacketDescriptor for ISO Urb %p - failing request\n",
                Urb);
            Status = STATUS_UNSUCCESSFUL;
        }
        fdoContext->ScratchPad.FrameNumber = startFrame + 
            Urb->UrbIsochronousTransfer.NumberOfPackets;
        break;

    case URB_FUNCTION_CLASS_DEVICE:
    case URB_FUNCTION_CLASS_INTERFACE:
    case URB_FUNCTION_CLASS_ENDPOINT:
    case URB_FUNCTION_VENDOR_DEVICE:
    case URB_FUNCTION_VENDOR_INTERFACE:
    case URB_FUNCTION_VENDOR_ENDPOINT:
        Urb->UrbControlVendorClassRequest.TransferBufferLength = bytesTransferred;
        break;

    case URB_FUNCTION_SYNC_RESET_PIPE_AND_CLEAR_STALL:

        TraceEvents(TRACE_LEVEL_WARNING, TRACE_DPC,
                __FUNCTION__": Clear stall completed with usb status %x\n",
            *usbdStatus);
        break;

    case URB_FUNCTION_GET_STATUS_FROM_ENDPOINT:
        TraceEvents(TRACE_LEVEL_WARNING, TRACE_DPC,
                __FUNCTION__": Get status from endpoint %x usb status %x\n",
            Urb->UrbControlGetStatusRequest.Index,
            *usbdStatus);
        if (NT_SUCCESS(Status))
        {
            PUSHORT endpointStatus = NULL;
            if (Urb->UrbControlGetStatusRequest.TransferBufferMDL)
            {
                endpointStatus = (PUSHORT) MmGetSystemAddressForMdlSafe(
                    Urb->UrbControlGetStatusRequest.TransferBufferMDL,
                    NormalPagePriority );
            }
            else if (Urb->UrbControlGetStatusRequest.TransferBuffer)
            {
                endpointStatus = (PUSHORT) Urb->UrbControlGetStatusRequest.TransferBuffer;
            }
            if (endpointStatus)
            {
                TraceEvents(TRACE_LEVEL_WARNING, TRACE_DPC,
                __FUNCTION__": Endpoint status %x\n",
                    *endpointStatus);
            }
            else
            {
                TraceEvents(TRACE_LEVEL_WARNING, TRACE_DPC,
                __FUNCTION__": Endpoint status buffer NULL!\n");
            }
        }
        break;

    default:
        //
        // Nothing to do for anyone else?
        //
        break;
    }
    return Status;
}

NTSTATUS
PostProcessSelectConfig(
    IN PUSB_FDO_CONTEXT fdoContext,
    IN PURB Urb)
{

    PUSB_CONFIGURATION_DESCRIPTOR config = Urb->UrbSelectConfiguration.ConfigurationDescriptor;
    NTSTATUS Status = STATUS_UNSUCCESSFUL;
    //
    // config can be null if the client driver has reset the configuration
    //
    if (config)
    {
        fdoContext->CurrentConfigValue = config->bConfigurationValue;
        SetConfigPointers(fdoContext);

        if (NT_VERIFY(fdoContext->InterfaceDescriptors))
        {
            Urb->UrbSelectConfiguration.ConfigurationHandle = fdoContext->ConfigurationDescriptor;
            Status = STATUS_SUCCESS;
        }
    }
    else
    {
        fdoContext->CurrentConfigValue = 0; // unconfigured		
        SetConfigPointers(fdoContext);
        Status = STATUS_SUCCESS;
    }

    ASSERT(fdoContext->ConfigBusy);
    fdoContext->ConfigBusy = FALSE;

    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DPC,
        __FUNCTION__": returns Status %x\n",
        Status);
    return Status;
}

NTSTATUS
PostProcessSelectInterface(
    IN PUSB_FDO_CONTEXT fdoContext,
    PURB Urb)
{
    NTSTATUS Status = STATUS_SUCCESS;

    ASSERT(fdoContext->ConfigBusy);
    fdoContext->ConfigBusy = FALSE;

    TraceEvents(TRACE_LEVEL_WARNING, TRACE_DPC,
        __FUNCTION__": interface %d %d complete\n",
        Urb->UrbSelectInterface.Interface.InterfaceNumber,
        Urb->UrbSelectInterface.Interface.AlternateSetting);
    return Status;
}

