//
// Copyright (c) Citrix Systems, Inc.
//
/// @file UsbConfig.cpp USB URB and Scratchpad request and response processing implementation.
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
#include <hidport.h>
#include "UsbConfig.h"

//
// local function declarations
//
NTSTATUS
GetAllConfigDescriptors(
    IN PUSB_FDO_CONTEXT fdoContext);

NTSTATUS
GetConfigDescriptor(
    IN PUSB_FDO_CONTEXT fdoContext,
    IN PUSB_CONFIGURATION_DESCRIPTOR configDescriptor,
    IN ULONG length,
    IN UCHAR index);

NTSTATUS
GetDeviceDescriptor(
    IN PUSB_FDO_CONTEXT fdoContext);

_Requires_lock_held_(fdoContext->WdfDevice)
NTSTATUS
GetDescriptor(
    IN PUSB_FDO_CONTEXT fdoContext,
    IN WDF_USB_CONTROL_SETUP_PACKET * packet,
    IN UCHAR DescType,
    IN UCHAR Recipient,
    IN UCHAR DescIndex,
    IN USHORT Value,
    IN USHORT Length,
    IN ULONG Datalength);


NTSTATUS
GetDeviceSpeed(
    IN PUSB_FDO_CONTEXT fdoContext);

NTSTATUS
GetCompleteConfigDescriptor(
    IN PUSB_FDO_CONTEXT fdoContext,
    IN UCHAR index);

void
GetDeviceStrings(
    IN PUSB_FDO_CONTEXT fdoContext);

PUSB_STRING
GetString(
    IN PUSB_FDO_CONTEXT fdoContext,
    IN UCHAR index);

NTSTATUS
GetOsDescriptorString(
    IN PUSB_FDO_CONTEXT fdoContext);

NTSTATUS
GetCurrentConfiguration(
    IN PUSB_FDO_CONTEXT fdoContext);

NTSTATUS
SetCurrentConfiguration(
    IN PUSB_FDO_CONTEXT fdoContext,
    IN UCHAR configValue);

_Requires_lock_held_(fdoContext->WdfDevice)
NTSTATUS
SetCurrentConfigurationLocked(
    IN PUSB_FDO_CONTEXT fdoContext,
    IN UCHAR configValue,
    IN BOOLEAN SetInterface,
    IN USHORT InterfaceNumber,
    IN USHORT AlternateSetting);

void
SetConfigPointers(
    IN PUSB_FDO_CONTEXT fdoContext);


NTSTATUS
ParseConfig(
    IN PUSB_FDO_CONTEXT fdoContext,
    PUSB_CONFIG_INFO configInfo);

PCHAR
AttributesToEndpointTypeString(
    UCHAR attributes);

PCHAR UrbFunctionToString(
    USHORT Function);

//
// Implementation.
//

NTSTATUS
GetUsbConfigData(
    IN PUSB_FDO_CONTEXT fdoContext)
{
    NTSTATUS status;

    TRY
    {
        status = GetDeviceDescriptor(fdoContext);
        if (!NT_SUCCESS(status))
        {
            LEAVE;
        }

        if (fdoContext->ResetDevice)
        {
            ULONG count = 1;
            status = ResetDevice(fdoContext);
            while (!NT_SUCCESS(status))
            {
                TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
                    __FUNCTION__": %s reset device failed %x retry count %d\n",
                    fdoContext->FrontEndPath,
                    status,
                    count);
                if (count > 5)
                {
                    break;
                }
                //
                // delay for a while and retry.
                //
                LARGE_INTEGER Timeout;
                Timeout.QuadPart = WDF_REL_TIMEOUT_IN_MS( 200);
                KeDelayExecutionThread(
                    KernelMode,
                    FALSE,
                    &Timeout);

                count++;
                status = ResetDevice(fdoContext);
            }

            if (!NT_SUCCESS(status))
            {
                TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
                    __FUNCTION__": %s reset device: device failed. Set registry no reset value.\n",
                    fdoContext->FrontEndPath);
                //
                // update the registry settings for this device.
                //
                fdoContext->ResetDevice = FALSE;
                SetUsbInfo(fdoContext, fdoContext->FetchOsDescriptor);
                LEAVE;
            }
        }

        status = GetDeviceSpeed(fdoContext);
        if (!NT_SUCCESS(status))
        {
            //
            // if the interface is not supported lie and claim
            // we are high speed.
            //
            TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
                __FUNCTION__": %s GetDeviceSpeed error %x\n",
                fdoContext->FrontEndPath,
                status);
            fdoContext->DeviceSpeed = UsbHighSpeed;
        }

        status = GetAllConfigDescriptors(fdoContext);
        if (!NT_SUCCESS(status))
        {
            TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
                __FUNCTION__": %s Device %p failing on GetAllConfigDescriptors error %x\n",
                fdoContext->FrontEndPath,
                fdoContext->WdfDevice,
                status);
        }
    }
    FINALLY
    {
        if (!NT_SUCCESS(status))
        {
            //
            // to allow dummy devices in nxprep.
            //
            if (fdoContext->NxprepBoot)
            {
                status = STATUS_SUCCESS;
            }
        }
    }
    return status;
}

void
FreeUsbConfigData(
    IN PUSB_FDO_CONTEXT fdoContext)
{
    //
    // Strings
    //
    if (fdoContext->SerialNumber)
    {
        ExFreePool(fdoContext->SerialNumber);
        fdoContext->SerialNumber = NULL;
    }

    if (fdoContext->Product)
    {
        ExFreePool(fdoContext->Product);
        fdoContext->Product = NULL;
    }

    if (fdoContext->Manufacturer)
    {
        ExFreePool(fdoContext->Manufacturer);
        fdoContext->Manufacturer = NULL;
    }

    if (fdoContext->OsDescriptorString)
    {
        ExFreePool(fdoContext->OsDescriptorString);
        fdoContext->OsDescriptorString = NULL;
    }
    //
    // config data
    //
       
    if (fdoContext->ConfigData)
    {
        for (ULONG Index = 0;
            Index < fdoContext->DeviceDescriptor.bNumConfigurations;
            Index++)
        {
            if (fdoContext->ConfigData[Index].m_configurationDescriptor)
            {
                ExFreePool(fdoContext->ConfigData[Index].m_configurationDescriptor);
                fdoContext->ConfigData[Index].m_configurationDescriptor = NULL;
            }
            if (fdoContext->ConfigData[Index].m_interfaceDescriptors)
            {
                ExFreePool(fdoContext->ConfigData[Index].m_interfaceDescriptors);
                fdoContext->ConfigData[Index].m_interfaceDescriptors = NULL;
            }
            if (fdoContext->ConfigData[Index].m_pipeDescriptors)
            {
                ExFreePool(fdoContext->ConfigData[Index].m_pipeDescriptors);
                fdoContext->ConfigData[Index].m_pipeDescriptors = NULL;
            }
        }
        ExFreePool(fdoContext->ConfigData);
        fdoContext->ConfigData = NULL;
    }
}

/**
 * @brief implements the actual USB request transfer for IOCTL_USB_GET_DESCRIPTOR_FROM_NODE_CONNECTION.
 * The MSDN documentation claims that "USBD", which of course does not exist for our
 * implementation, fills in the bmRequest and bRequest fields of the USB packet.
 * Experimental evidence concurs.
 *
 * @param[in] fdoContext. The usual context for the device.
 * @param[in] descRequest. The USB_DESCRIPTOR_REQUEST from the application.
 * @param DataLength. The payload buffer length on input, the payload transferred on output.
 *
 * @returns NTSTATUS value indicating success or failure.
 */
NTSTATUS
ProcessGetDescriptorFromNode(
    IN PUSB_FDO_CONTEXT fdoContext,
    IN PUSB_DESCRIPTOR_REQUEST descRequest,    
    PULONG DataLength)
{
    AcquireFdoLock(fdoContext);    
    if (!WaitForScratchPadAccess(fdoContext))
    {
        ReleaseFdoLock(fdoContext);
        return STATUS_UNSUCCESSFUL;
    }
    WDF_USB_CONTROL_SETUP_PACKET setup;
    // hmmmm we fill in the bmRequest = 0x80
    // and bRequest = 0x06
    setup.Packet.bm.Byte = 0x80;
    setup.Packet.bRequest = 0x06;
    setup.Packet.wIndex.Value = descRequest->SetupPacket.wIndex;
    setup.Packet.wLength = descRequest->SetupPacket.wLength;
    setup.Packet.wValue.Value = descRequest->SetupPacket.wValue;

    NTSTATUS status = PutScratchOnRing(
        fdoContext,
        &setup,
        *DataLength,
        UsbdPipeTypeControl,
        USB_ENDPOINT_TYPE_CONTROL | USB_ENDPOINT_DIRECTION_MASK, //!< Control IN
        FALSE);

    if (!NT_SUCCESS(status))
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
            __FUNCTION__": %s putScratchOnRing failed %x\n",
            fdoContext->FrontEndPath,
            status);          
        fdoContext->ConfigBusy = FALSE;
        ReleaseFdoLock(fdoContext);
        return status;
    }

    ReleaseFdoLock(fdoContext);
    status = WaitForScratchCompletion(fdoContext);
    AcquireFdoLock(fdoContext);

    if (status != STATUS_SUCCESS)
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
            __FUNCTION__": %s wait failed %x\n",
            fdoContext->FrontEndPath,
            status);
        status = STATUS_UNSUCCESSFUL;
    }
    else if (fdoContext->ScratchPad.Status != 0) // XXX what is the correct constant?
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
            __FUNCTION__": %s usb status %x returned\n",
            fdoContext->FrontEndPath,
            fdoContext->ScratchPad.Status);
        status = STATUS_UNSUCCESSFUL;
    }
    else
    {
        // copy data back
        *DataLength = min((*DataLength), fdoContext->ScratchPad.BytesTransferred);
        RtlCopyMemory(descRequest->Data, fdoContext->ScratchPad.Buffer,
             *DataLength);
    }           
    fdoContext->ConfigBusy = FALSE;
    ReleaseFdoLock(fdoContext);
    return status;
}

_Requires_lock_held_(fdoContext->WdfDevice)
NTSTATUS
GetDescriptor(
    IN PUSB_FDO_CONTEXT fdoContext,
    IN WDF_USB_CONTROL_SETUP_PACKET * packet,
    IN UCHAR DescType,
    IN UCHAR Recipient,
    IN UCHAR DescIndex,
    IN USHORT Value,
    IN USHORT Length,
    IN ULONG Datalength)
{
    RtlZeroMemory(packet, sizeof(WDF_USB_CONTROL_SETUP_PACKET));
    packet->Packet.bm.Request.Dir = BMREQUEST_DEVICE_TO_HOST;
    packet->Packet.bm.Request.Type = BMREQUEST_STANDARD;
    packet->Packet.bm.Request.Recipient = Recipient;
    packet->Packet.bRequest = USB_REQUEST_GET_DESCRIPTOR;
    packet->Packet.wValue.Bytes.HiByte = DescType;
    packet->Packet.wValue.Bytes.LowByte = DescIndex;
    packet->Packet.wIndex.Value = Value;
    packet->Packet.wLength = Length;

    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DEVICE,
            __FUNCTION__": %s (%x) Recipient %x Value %x Index %x Length %x\n",
        DescriptorTypeToString(DescType),
        DescType,
        packet->Packet.bm.Request.Recipient,
        packet->Packet.wValue.Value,
        packet->Packet.wIndex.Value,
        packet->Packet.wLength);

        
    NTSTATUS status = PutScratchOnRing(
        fdoContext,
        packet,
        Datalength,
        UsbdPipeTypeControl,
        USB_ENDPOINT_TYPE_CONTROL | USB_ENDPOINT_DIRECTION_MASK, //!< Control IN
        FALSE); 

    if (!NT_SUCCESS(status))
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
            __FUNCTION__": %s putScratchOnRing failed %x\n",
            fdoContext->FrontEndPath,
            status);
        return status;
    }

    ReleaseFdoLock(fdoContext);
    status = WaitForScratchCompletion(fdoContext);
    AcquireFdoLock(fdoContext);

    if (status != STATUS_SUCCESS)
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
            __FUNCTION__": %s wait failed %x\n",
            fdoContext->FrontEndPath,
            status);
        status = STATUS_UNSUCCESSFUL;
    }
    else if (fdoContext->ScratchPad.Status != 0) // XXX what is the correct constant?
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
            __FUNCTION__": %s usb status %x returned\n",
            fdoContext->FrontEndPath,
            fdoContext->ScratchPad.Status);
        status = STATUS_UNSUCCESSFUL;
    }
    return status;
}

NTSTATUS
GetDeviceDescriptor(
    IN PUSB_FDO_CONTEXT fdoContext)
{
    AcquireFdoLock(fdoContext);

    NTSTATUS status = GetDescriptor(
        fdoContext,
        &fdoContext->ScratchPad.Packet,
        USB_DEVICE_DESCRIPTOR_TYPE,
        BMREQUEST_TO_DEVICE,
        0,
        0,
        sizeof(USB_DEVICE_DESCRIPTOR),
        256);

    if (!NT_SUCCESS(status))
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
            __FUNCTION__": %s GetDescriptor failed %x\n",
            fdoContext->FrontEndPath,
            status);
    }
    else if (fdoContext->ScratchPad.BytesTransferred < sizeof(USB_DEVICE_DESCRIPTOR))
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
            __FUNCTION__": %s expected %x bytes got %x\n",
            fdoContext->FrontEndPath,
            sizeof(USB_DEVICE_DESCRIPTOR),
            fdoContext->ScratchPad.BytesTransferred);
    }
    else
    {
        RtlCopyMemory(&fdoContext->DeviceDescriptor, fdoContext->ScratchPad.Buffer, sizeof(USB_DEVICE_DESCRIPTOR));

        TraceEvents(TRACE_LEVEL_WARNING, TRACE_DEVICE,
            __FUNCTION__": %s Vendor %4.4x Product %4.4x\n",
            fdoContext->FrontEndPath,
            fdoContext->DeviceDescriptor.idVendor,
            fdoContext->DeviceDescriptor.idProduct);


        TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DEVICE,
            __FUNCTION__":  Received %d bytes for descriptor\n"
            "    bLength %x bDescriptorType %x bcdUSB %x bDeviceClass %x\n"
            "    bDeviceSubClass %x bDeviceProtocol %x bMaxPacketSize0 %x\n"
            "    bcdDevice %x iManufacturer %x iProduct %x\n"
            "    iSerialNumber %x bNumConfigurations %x\n",
            fdoContext->ScratchPad.BytesTransferred,
            fdoContext->DeviceDescriptor.bLength,
            fdoContext->DeviceDescriptor.bDescriptorType,
            fdoContext->DeviceDescriptor.bcdUSB,
            fdoContext->DeviceDescriptor.bDeviceClass,
            fdoContext->DeviceDescriptor.bDeviceSubClass,
            fdoContext->DeviceDescriptor.bDeviceProtocol,
            fdoContext->DeviceDescriptor.bMaxPacketSize0,
            fdoContext->DeviceDescriptor.bcdDevice,
            fdoContext->DeviceDescriptor.iManufacturer,
            fdoContext->DeviceDescriptor.iProduct,
            fdoContext->DeviceDescriptor.iSerialNumber,
            fdoContext->DeviceDescriptor.bNumConfigurations);

        if (fdoContext->DeviceDescriptor.bNumConfigurations == 0)
        {
            TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
                __FUNCTION__": %s bNumConfigurations is zero\n",
                fdoContext->FrontEndPath); 
            status = STATUS_UNSUCCESSFUL;
        }
    }
    ReleaseFdoLock(fdoContext);

    if (NT_SUCCESS(status))
    {
        //
        // set the usbinfo string
        //
        status = RtlStringCbPrintfW(
            fdoContext->UsbInfoEntryName,
            sizeof(fdoContext->UsbInfoEntryName),
            L"usbflags\\%04.4x%04.4x%04.4x",
            fdoContext->DeviceDescriptor.idVendor,
            fdoContext->DeviceDescriptor.idProduct,
            fdoContext->DeviceDescriptor.bcdDevice);

        if (!NT_SUCCESS(status))
        {
            WCHAR deadBeef[] = L"usbflags\\DEADF00D";
            //
            // this should never happen!
            //
           TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
                __FUNCTION__": %s RtlStringCbPrintfW error %x\n",
                fdoContext->FrontEndPath,
               status);
           RtlCopyMemory(fdoContext->UsbInfoEntryName, deadBeef, sizeof(deadBeef));
        }
        //
        // allocate the array of config data based on the device descriptor
        //
        fdoContext->ConfigData = (PUSB_CONFIG_INFO) ExAllocatePoolWithTag(
            NonPagedPool,
            fdoContext->DeviceDescriptor.bNumConfigurations * sizeof(USB_CONFIG_INFO),
            XVU3);
        if (!fdoContext->ConfigData)
        {
            TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
                __FUNCTION__": %s no memory for config info\n",
                fdoContext->FrontEndPath);
            status = STATUS_INSUFFICIENT_RESOURCES;
        }
        else
        {
            RtlZeroMemory(fdoContext->ConfigData,
                fdoContext->DeviceDescriptor.bNumConfigurations * sizeof(USB_CONFIG_INFO));
        }
        //
        // fetch the registry flags for this usb device.
        //
        (void) GetUsbInfo(fdoContext);

    }
    return status;
}

NTSTATUS
GetDeviceSpeed(
    IN PUSB_FDO_CONTEXT fdoContext)
{
    AcquireFdoLock(fdoContext);

    PCHAR String = "Unknown Speed Value";

    RtlZeroMemory(&fdoContext->ScratchPad.Packet, sizeof(fdoContext->ScratchPad.Packet));
    fdoContext->ScratchPad.Request = XenUsbGetSpeed;

    NTSTATUS status = PutScratchOnRing(
        fdoContext,
        &fdoContext->ScratchPad.Packet,
        sizeof(ULONG),
        (USBD_PIPE_TYPE)XenUsbGetSpeed,
        USB_ENDPOINT_TYPE_CONTROL | USB_ENDPOINT_DIRECTION_MASK, //!< Control IN
        FALSE);

    ReleaseFdoLock(fdoContext);

    status = WaitForScratchCompletion(fdoContext);

    if (NT_SUCCESS(status))
    {
        ULONG debugLevel = TRACE_LEVEL_INFORMATION;
        switch ((XENUSB_SPEED) fdoContext->ScratchPad.Data)
        {
        case XenUsbSpeedLow:
            String = "XenUsbSpeedLow";
            fdoContext->DeviceSpeed = UsbLowSpeed;
            break;
        case XenUsbSpeedFull:
            String = "XenUsbSpeedFull";
            fdoContext->DeviceSpeed = UsbFullSpeed;
            break;
        case XenUsbSpeedHigh:
            String = "XenUsbSpeedHigh";
            fdoContext->DeviceSpeed = UsbHighSpeed;
            break;
        default: // ???
            debugLevel = TRACE_LEVEL_ERROR;
            fdoContext->DeviceSpeed = UsbHighSpeed;
            break;
        };
        
        TraceEvents(debugLevel, TRACE_DEVICE,
            __FUNCTION__": %s GetDeviceSpeed got %s (%d)\n",
            fdoContext->FrontEndPath,
            String,
            fdoContext->ScratchPad.Data);
    }
    else
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
            __FUNCTION__": %s waitForScratchCompletion failed %x\n",
            fdoContext->FrontEndPath,
            status);
    }
    return status;
}

//
// must be called at less than dispatch level with the device lock not held.
//
NTSTATUS
ResetDevice(
    IN PUSB_FDO_CONTEXT fdoContext)
{
    AcquireFdoLock(fdoContext);

    RtlZeroMemory(&fdoContext->ScratchPad.Packet, sizeof(fdoContext->ScratchPad.Packet));
    NTSTATUS status = PutScratchOnRing(
        fdoContext,
        NULL,
        0,
        UsbdPipeTypeControl,
        USB_ENDPOINT_TYPE_CONTROL,
        TRUE);

    ReleaseFdoLock(fdoContext);

    if (!NT_SUCCESS(status))
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
            __FUNCTION__": %s putScratchOnRing for failed %x\n",
            fdoContext->FrontEndPath,
            status);
    }
    else
    {
        status = WaitForScratchCompletion(fdoContext);
    }
    if (NT_SUCCESS(status))
    {
        if (fdoContext->ScratchPad.Status != 0)
        {
            TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
                __FUNCTION__": %s scratch Status %x\n",
                fdoContext->FrontEndPath,
                fdoContext->ScratchPad.Status);
            status = STATUS_UNSUCCESSFUL;
        }
    }
    TraceEvents(TRACE_LEVEL_WARNING, TRACE_DEVICE,
        __FUNCTION__": %s returns status %x\n",
        fdoContext->FrontEndPath,
        status);
    return status;
}

NTSTATUS
GetAllConfigDescriptors(
    IN PUSB_FDO_CONTEXT fdoContext)
{
    NTSTATUS status = STATUS_UNSUCCESSFUL;
    if (!fdoContext->ConfigData)
    {
        return status;
    }
    for (UCHAR Index = 0;
        Index < fdoContext->DeviceDescriptor.bNumConfigurations;
        Index++)
    {
        status = GetCompleteConfigDescriptor(fdoContext, Index);
        if (!NT_SUCCESS(status))
        {
            TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
                __FUNCTION__": %s Get config descriptor %d of %d failed\n",
                fdoContext->FrontEndPath,
                Index,
                fdoContext->DeviceDescriptor.bNumConfigurations);
            break;
        }
    }
    if (NT_SUCCESS(status))
    {
        //
        // get the strings.
        //
        GetDeviceStrings(fdoContext);
        //
        // make sure we have a default config active
        //
        status = GetCurrentConfiguration(fdoContext);

        if (NT_SUCCESS(status))
        {
            if (CurrentConfigValue(fdoContext) == 0)
            {
                PUSB_CONFIGURATION_DESCRIPTOR defaultDesc =
                    ConfigByIndex(fdoContext, 0);
                if (defaultDesc)
                {
                    //
                    // if this is a non-compliant device we
                    // need to offset bConfigurationValue
                    //
                    status = SetCurrentConfiguration(fdoContext,
                        defaultDesc->bConfigurationValue + fdoContext->CurrentConfigOffset);
                }
                else
                {
                    status = STATUS_UNSUCCESSFUL;
                }
            }
        }
        if (NT_SUCCESS(status))
        {
            SetConfigPointers(fdoContext);
        }
    }
    return status;
}

UCHAR CurrentConfigValue(
    IN PUSB_FDO_CONTEXT fdoContext)
{
    return (fdoContext->CurrentConfigValue - fdoContext->CurrentConfigOffset);
}

PUSB_CONFIGURATION_DESCRIPTOR
ConfigByIndex(
    IN PUSB_FDO_CONTEXT fdoContext,
    ULONG Index)
{
    PUSB_CONFIGURATION_DESCRIPTOR configDesc = NULL;
    if (fdoContext->ConfigData)
    {
        if (Index < fdoContext->DeviceDescriptor.bNumConfigurations)
        {
            configDesc = fdoContext->ConfigData[Index].m_configurationDescriptor;
        }
    }
    return configDesc;
}

PUSB_CONFIG_INFO
ConfigInfoByValue(
    IN PUSB_FDO_CONTEXT fdoContext,
    UCHAR Value)
{
    PUSB_CONFIG_INFO configInfo = NULL;
    if (fdoContext->ConfigData)
    {
        for (UCHAR Index = 0;
            Index < fdoContext->DeviceDescriptor.bNumConfigurations;
            Index++)
        {
            configInfo = &fdoContext->ConfigData[Index];
            if ((configInfo->m_configurationDescriptor) &&
                (configInfo->m_configurationDescriptor->bConfigurationValue == Value))
            {
                break;
            }
            configInfo = NULL;
        }
    }
    return configInfo;
}


PUSB_INTERFACE_DESCRIPTOR
GetDefaultInterface(
    IN PUSB_FDO_CONTEXT fdoContext)
{
    if (fdoContext->InterfaceDescriptors)
    {
        return fdoContext->InterfaceDescriptors[0]; // is this right?
    }
    else
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
            __FUNCTION__": %s Warning: no interfaces defined for this device %p\n",
            fdoContext->FrontEndPath,
            fdoContext->WdfDevice);
        return NULL;
    }
}

void
SetConfigPointers(
    IN PUSB_FDO_CONTEXT fdoContext)
{
    fdoContext->ConfigurationDescriptor = NULL;
    fdoContext->InterfaceDescriptors = NULL;
    fdoContext->PipeDescriptors = NULL;
    fdoContext->NumInterfaces = 0;
    fdoContext->NumEndpoints = 0;

    if (CurrentConfigValue(fdoContext))
    {
        PUSB_CONFIG_INFO info = ConfigInfoByValue(fdoContext,
            fdoContext->CurrentConfigValue);
        if (info)
        {
            fdoContext->ConfigurationDescriptor  = info->m_configurationDescriptor;
            fdoContext->InterfaceDescriptors = info->m_interfaceDescriptors;
            fdoContext->PipeDescriptors = info->m_pipeDescriptors;
            fdoContext->NumInterfaces = info->m_numInterfaces;
            fdoContext->NumEndpoints = info->m_numEndpoints;
        }
    }
}

//
// parse the config twice - once to check the config and count the endpoints.
// the second pass assigns endpoints to the endpoint pointer array, one entry for
// each endpoint in all interfaces in the configuration.
//
NTSTATUS
ParseConfig(
    IN PUSB_FDO_CONTEXT,
    PUSB_CONFIG_INFO configInfo)
{
    NTSTATUS Status = STATUS_SUCCESS;
    ASSERT(configInfo);
    ASSERT(configInfo->m_configurationDescriptor);
    UCHAR configValue = configInfo->m_configurationDescriptor->bConfigurationValue;

    PUCHAR descEnd = (PUCHAR)configInfo->m_configurationDescriptor + 
        configInfo->m_configurationDescriptor->wTotalLength;
    PUSB_COMMON_DESCRIPTOR commonDesc = (PUSB_COMMON_DESCRIPTOR)configInfo->m_configurationDescriptor;
    PUCHAR currentLocation = (PUCHAR) configInfo->m_configurationDescriptor;
    ULONG numInterfaces = 0;
    ULONG numEndpoints = 0;
    ULONG configDescriptors = 0;
    ULONG enumIndex = 0;
    PUSB_INTERFACE_DESCRIPTOR currentInterface = NULL;
    BOOLEAN isHidDevice = FALSE;
    BOOLEAN isHidDescriptorBeforeEndpoint = FALSE;    
    ULONG numHidEndPoints = 0;
    ULONG numHidEndpointsFound = 0;

    while ((PUCHAR)commonDesc + sizeof(USB_COMMON_DESCRIPTOR) < descEnd &&
        (PUCHAR)commonDesc + commonDesc->bLength <= descEnd)
    {
        switch (commonDesc->bDescriptorType)
        {
        case USB_CONFIGURATION_DESCRIPTOR_TYPE:
            if (configDescriptors != 0)
            {
                TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
                    __FUNCTION__ ": Config %d configuration descriptor unexpected\n",
                    configValue);
                Status = STATUS_UNSUCCESSFUL;
                break;
            }
            if (commonDesc->bLength != sizeof(USB_CONFIGURATION_DESCRIPTOR))
            {
                TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
                    __FUNCTION__ ": Config %d Error parsing configuration descriptor index %d\n",
                    configValue,
                    enumIndex);
                Status = STATUS_UNSUCCESSFUL;
                break;
            }
            currentLocation  += commonDesc->bLength;
            commonDesc = (PUSB_COMMON_DESCRIPTOR) currentLocation;
            configDescriptors++;
            enumIndex++;
            continue;

        case USB_INTERFACE_DESCRIPTOR_TYPE:
            if (commonDesc->bLength < sizeof(USB_INTERFACE_DESCRIPTOR))
            {
                TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
                    __FUNCTION__ ": Config %d Error parsing interface descriptor index %d bLength %d expected %d\n",
                    configValue,
                    commonDesc->bLength,
                    sizeof(USB_INTERFACE_DESCRIPTOR),
                    enumIndex);
                Status = STATUS_UNSUCCESSFUL;
                break;
            }
            currentInterface = (PUSB_INTERFACE_DESCRIPTOR) commonDesc;
            if (configInfo->m_interfaceDescriptors)
            {
                //
                // Add a pointer to this interface/alternate
                //
                configInfo->m_interfaceDescriptors[numInterfaces] = currentInterface;
                TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DEVICE,
                    __FUNCTION__ ": Config %d Found interface at %p\n",
                    configValue,
                    currentInterface);
                TraceEvents(TRACE_LEVEL_VERBOSE, TRACE_DEVICE,
                    "bLength %x bDescriptorType %x bInterfaceNumber %x\n"
                    "bAlternateSetting %x bNumEndpoints %x bInterfaceClass %x\n"
                    "bInterfaceSubClass %x bInterfaceProtocol %x iInterface %x\n",
                    configInfo->m_interfaceDescriptors[numInterfaces]->bLength,
                    configInfo->m_interfaceDescriptors[numInterfaces]->bDescriptorType,
                    configInfo->m_interfaceDescriptors[numInterfaces]->bInterfaceNumber,
                    configInfo->m_interfaceDescriptors[numInterfaces]->bAlternateSetting,
                    configInfo->m_interfaceDescriptors[numInterfaces]->bNumEndpoints,
                    configInfo->m_interfaceDescriptors[numInterfaces]->bInterfaceClass,
                    configInfo->m_interfaceDescriptors[numInterfaces]->bInterfaceSubClass,
                    configInfo->m_interfaceDescriptors[numInterfaces]->bInterfaceProtocol,
                    configInfo->m_interfaceDescriptors[numInterfaces]->iInterface);

                if (configInfo->m_interfaceDescriptors[numInterfaces]->bInterfaceClass == 
                    USB_INTERFACE_CLASS_HID)
                {
                    isHidDevice = TRUE;
                    isHidDescriptorBeforeEndpoint = FALSE;
                    numHidEndPoints = configInfo->m_interfaceDescriptors[numInterfaces]->bNumEndpoints;
                    numHidEndpointsFound = 0;
                    //
                    // each endpoint is seven bytes long. We could force
                    // the hid interface to be Draft 3 compliant - with the
                    // hid descriptor AFTER the endpoints.
                    //
                    TraceEvents(TRACE_LEVEL_VERBOSE, TRACE_DEVICE,
                        __FUNCTION__ ": Config %d HID Device class subclass %d protocol %d\n",
                        configValue,
                        // 1 == boot
                        configInfo->m_interfaceDescriptors[numInterfaces]->bInterfaceSubClass,
                        // 1 == keyboard 2 == mouse
                        configInfo->m_interfaceDescriptors[numInterfaces]->bInterfaceProtocol);
                }
                else
                {
                    isHidDevice = FALSE;
                }

            }
            numInterfaces++;
            enumIndex++;
            currentLocation  += commonDesc->bLength;
            commonDesc = (PUSB_COMMON_DESCRIPTOR) currentLocation;
            continue;

        case USB_DESCRIPTOR_TYPE_HID:
            if (configInfo->m_interfaceDescriptors)
            {
                if (!isHidDevice)
                {
                    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DEVICE,
                        __FUNCTION__ ": Config %d Hid descriptor found for non-hid device (ignoring it)\n",
                        configValue);
                }
                else if ((numHidEndpointsFound == 0) && numHidEndPoints)
                {
                    isHidDescriptorBeforeEndpoint = TRUE;
                }
                TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DEVICE,
                    __FUNCTION__ ": Config %d Found HID descriptor HID device was %s compliant\n",
                    configValue,
                    isHidDescriptorBeforeEndpoint ? "D4" : "D3");
            }
            currentLocation  += commonDesc->bLength;
            commonDesc = (PUSB_COMMON_DESCRIPTOR) currentLocation;
            enumIndex++;
            continue;

        case USB_ENDPOINT_DESCRIPTOR_TYPE:
            if (commonDesc->bLength < sizeof(USB_ENDPOINT_DESCRIPTOR)) // not equal?
            {
                TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
                    __FUNCTION__ ": Config %d Error parsing endpoint descriptor index %d bLength %d expected %d\n",
                    configValue,
                    enumIndex,
                    commonDesc->bLength,
                    sizeof(USB_ENDPOINT_DESCRIPTOR));
                Status = STATUS_UNSUCCESSFUL;
                break;
            }
            if (currentInterface == NULL)
            {
                TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
                    __FUNCTION__ ": Config %d Error parsing endpoint descriptor. currentInterface is NULL\n",
                    configValue);
                Status = STATUS_UNSUCCESSFUL;
                break;
            }
            if (configInfo->m_pipeDescriptors)
            {
                PUSB_ENDPOINT_DESCRIPTOR ea = (PUSB_ENDPOINT_DESCRIPTOR) commonDesc;
                configInfo->m_pipeDescriptors[numEndpoints].endpoint = ea;
                configInfo->m_pipeDescriptors[numEndpoints].interfaceDescriptor = currentInterface;
                configInfo->m_pipeDescriptors[numEndpoints].valid = FALSE;
                configInfo->m_pipeDescriptors[numEndpoints].intInEndpoint = (
                    (ea->bmAttributes & USB_ENDPOINT_TYPE_MASK) == USB_ENDPOINT_TYPE_INTERRUPT) &&
                    USB_ENDPOINT_DIRECTION_IN(ea->bEndpointAddress);
                configInfo->m_pipeDescriptors[numEndpoints].requestsQueued = 0;
                configInfo->m_pipeDescriptors[numEndpoints].lastResponseTime = 0;
                configInfo->m_pipeDescriptors[numEndpoints].abortInProgress = FALSE;
                configInfo->m_pipeDescriptors[numEndpoints].abortWaiters = 0;
                KeInitializeEvent(&configInfo->m_pipeDescriptors[numEndpoints].abortCompleteEvent,
                    NotificationEvent,
                    FALSE);


                TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DEVICE,
                    __FUNCTION__ ": Config %d Found %s endpoint %x at %p for interface %p (%d %d) Class %x SubClass %x Protocol %x IntIn %d\n",
                    configValue,
                    AttributesToEndpointTypeString(ea->bmAttributes),
                    ea->bEndpointAddress,
                    configInfo->m_pipeDescriptors[numEndpoints].endpoint,
                    configInfo->m_pipeDescriptors[numEndpoints].interfaceDescriptor,
                    currentInterface->bInterfaceNumber,
                    currentInterface->bAlternateSetting,
                    currentInterface->bInterfaceClass,
                    currentInterface->bInterfaceSubClass,
                    currentInterface->bInterfaceProtocol,
                    configInfo->m_pipeDescriptors[numEndpoints].intInEndpoint);

                TraceEvents(TRACE_LEVEL_VERBOSE, TRACE_DEVICE,
                    __FUNCTION__ ": bLength %x bDescriptorType %x bEndpointAddress %x\n"
                    "bmAttributes %x wMaxPacketSize %x bInterval %x\n",
                    ea->bLength,
                    ea->bDescriptorType,
                    ea->bEndpointAddress,
                    ea->bmAttributes,
                    ea->wMaxPacketSize,
                    ea->bInterval);

                if (isHidDevice)
                {
                    if (numHidEndpointsFound == 0 &&
                        !isHidDescriptorBeforeEndpoint)
                    {
                        TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DEVICE,
                            __FUNCTION__ ": Config %d D3 compliant HID device\n",
                            configValue);
                    }
                    numHidEndpointsFound++;
                }
            }
            currentLocation  += commonDesc->bLength;
            commonDesc = (PUSB_COMMON_DESCRIPTOR) currentLocation;
            numEndpoints++;
            enumIndex++;
            continue;
            //
            // Audio devices are bork'd. Why?
            //
        case (USB_CLASS_AUDIO | USB_INTERFACE_DESCRIPTOR_TYPE):
            {
                PUSB_INTERFACE_DESCRIPTOR audioInterface = (PUSB_INTERFACE_DESCRIPTOR) commonDesc;
                TraceEvents(TRACE_LEVEL_VERBOSE, TRACE_DEVICE,
                    __FUNCTION__ ": Config %d Audio Class Interface Descriptor bLength %d bDescriptorType %x bDescriptorSubtype %x\n",
                    configValue,
                    audioInterface->bLength,
                    audioInterface->bDescriptorType,
                    audioInterface->bInterfaceNumber);
            }
            currentLocation  += commonDesc->bLength;
            commonDesc = (PUSB_COMMON_DESCRIPTOR) currentLocation;
            enumIndex++;
            continue;

        case (USB_CLASS_AUDIO | USB_ENDPOINT_DESCRIPTOR_TYPE):
            {
                PUSB_ENDPOINT_DESCRIPTOR audioEndpoint = (PUSB_ENDPOINT_DESCRIPTOR) commonDesc;
                TraceEvents(TRACE_LEVEL_VERBOSE, TRACE_DEVICE,
                    __FUNCTION__ ": Config %d Audio Class Endpoint Descriptor bLength %d bDescriptorType %x\n"
                    "    bDescriptorSubtype %x bmAttributes %x\n",
                    configValue,
                    audioEndpoint->bLength,
                    audioEndpoint->bDescriptorType,
                    audioEndpoint->bEndpointAddress,
                    audioEndpoint->bEndpointAddress);
            }
            currentLocation  += commonDesc->bLength;
            commonDesc = (PUSB_COMMON_DESCRIPTOR) currentLocation;
            enumIndex++;
            continue;

        case USB_RESERVED_DESCRIPTOR_TYPE:
        case USB_CONFIG_POWER_DESCRIPTOR_TYPE:
        case USB_INTERFACE_POWER_DESCRIPTOR_TYPE:
        default:
            //
            // allow other descriptors as well.
            //
            TraceEvents(TRACE_LEVEL_VERBOSE, TRACE_DEVICE,
                __FUNCTION__ ": Config %d Custom descriptor type %x length %x \n",
                configValue,
                commonDesc->bDescriptorType,
                commonDesc->bLength);

            currentLocation  += commonDesc->bLength;
            commonDesc = (PUSB_COMMON_DESCRIPTOR) currentLocation;
            enumIndex++;
            continue;
        }
        break;
    }
    if (NT_SUCCESS(Status))
    {
        configInfo->m_numInterfaces = numInterfaces;
        configInfo->m_numEndpoints = numEndpoints;
        TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DEVICE,
            __FUNCTION__ ": Config %d parseConfig successfully parsed %d interfaces and %d endpoints\n",
            configValue,
            configInfo->m_numInterfaces,
            configInfo->m_numEndpoints);
    }
    return Status;
}


NTSTATUS
GetCompleteConfigDescriptor(
    IN PUSB_FDO_CONTEXT fdoContext,
    IN UCHAR index)
{
    if (!fdoContext->ConfigData)
    {
        return STATUS_UNSUCCESSFUL;
    }
    if (index >= fdoContext->DeviceDescriptor.bNumConfigurations)
    {
        return STATUS_UNSUCCESSFUL;
    }
    PUSB_CONFIG_INFO configInfo = &fdoContext->ConfigData[index];

    USB_CONFIGURATION_DESCRIPTOR configHeader;
    NTSTATUS status = GetConfigDescriptor(
        fdoContext,
        &configHeader,
        sizeof(configHeader),
        index);

    if (!NT_SUCCESS(status))
    {
        return status;
    }
    ULONG length = configHeader.wTotalLength;

    // allocate the space for the config interface and endpoint descriptors
    if (configInfo->m_configurationDescriptor)
    {
        ExFreePool(configInfo->m_configurationDescriptor);
    }
    configInfo->m_configurationDescriptor = (PUSB_CONFIGURATION_DESCRIPTOR)
        ExAllocatePoolWithTag(NonPagedPool, length, XVU4);
    if (!configInfo->m_configurationDescriptor)
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
            __FUNCTION__": %s allocation failure for m_configurationDescriptor\n",
            fdoContext->FrontEndPath);
        return STATUS_UNSUCCESSFUL;
    }
    status = GetConfigDescriptor(
        fdoContext,
        configInfo->m_configurationDescriptor,
        length,
        index);

    if (!NT_SUCCESS(status))
    {
        ExFreePool(configInfo->m_configurationDescriptor);
        configInfo->m_configurationDescriptor = NULL;
        return status;
    }
    //
    // first pass is to count the endpoints
    //
    status = ParseConfig(fdoContext, configInfo);
    if (!NT_SUCCESS(status))
    {
        ExFreePool(configInfo->m_configurationDescriptor);
        configInfo->m_configurationDescriptor = NULL;
        return status;
    }
    if (configInfo->m_numInterfaces == 0)
    {
        //
        // what is this?
        //
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
            __FUNCTION__": %s Config index %d No interfaces?\n",
            fdoContext->FrontEndPath,
            index);
        ExFreePool(configInfo->m_configurationDescriptor);
        configInfo->m_configurationDescriptor = NULL;
        return STATUS_UNSUCCESSFUL;
    }
    //
    // allocate interface pointers and pipe_descriptor pointers
    //
    configInfo->m_interfaceDescriptors = (PUSB_INTERFACE_DESCRIPTOR *)
        ExAllocatePoolWithTag(NonPagedPool,
            (configInfo->m_numInterfaces * sizeof(PUSB_INTERFACE_DESCRIPTOR *)),
            XVU5);
    if (!configInfo->m_interfaceDescriptors)
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
            __FUNCTION__": %s Config index %d allocation failure for interface pointer array\n",
            fdoContext->FrontEndPath,
            index);
        ExFreePool(configInfo->m_configurationDescriptor);
        configInfo->m_configurationDescriptor = NULL;
        return STATUS_UNSUCCESSFUL;
    }
    //
    // Allocate the endpoint pointer array - if there are any endpoints.
    // Note that there is no requirement for anything other than control
    // endpoints.
    //
    if (configInfo->m_numEndpoints)
    {
        configInfo->m_pipeDescriptors = (PIPE_DESCRIPTOR *)
            ExAllocatePoolWithTag(NonPagedPool,
            (configInfo->m_numEndpoints * sizeof(PIPE_DESCRIPTOR)),
            XVU6);
        if (!configInfo->m_pipeDescriptors)
        {
            TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
                __FUNCTION__": %s Config index %d allocation failure for PIPE_DESCRIPTOR array\n",
                fdoContext->FrontEndPath,
                index);
            ExFreePool(configInfo->m_configurationDescriptor);
            configInfo->m_configurationDescriptor = NULL;
            ExFreePool(configInfo->m_interfaceDescriptors);
            configInfo->m_interfaceDescriptors = NULL;
            return STATUS_UNSUCCESSFUL;
        }
    }
    //
    // reparse the config to fill in the dynamic data
    //
    status = ParseConfig(fdoContext, configInfo);
    if (!NT_SUCCESS(status))
    {
        ExFreePool(configInfo->m_configurationDescriptor);
        configInfo->m_configurationDescriptor = NULL;
        ExFreePool(configInfo->m_interfaceDescriptors);
        configInfo->m_interfaceDescriptors = NULL;
        if (configInfo->m_pipeDescriptors)
        {
            ExFreePool(configInfo->m_pipeDescriptors);
            configInfo->m_pipeDescriptors = NULL;
        }
        return status;
    }

    return status;
}

//
// The config descriptor is variable length
//
NTSTATUS
GetConfigDescriptor(
    IN PUSB_FDO_CONTEXT fdoContext,
    IN PUSB_CONFIGURATION_DESCRIPTOR configDescriptor,
    IN ULONG length,
    IN UCHAR index)
{
    if (!configDescriptor)
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
            __FUNCTION__": %s input buffer NULL!\n",
            fdoContext->FrontEndPath);
        return STATUS_UNSUCCESSFUL;
    }
    if (length < sizeof(USB_CONFIGURATION_DESCRIPTOR))
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
            __FUNCTION__": %s input length %x too small (%x)\n",
            fdoContext->FrontEndPath,
            length,
            sizeof(USB_CONFIGURATION_DESCRIPTOR));

        return STATUS_UNSUCCESSFUL;
    }
    if (length > 0xffff)
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
            __FUNCTION__": %s input length %x too big\n",
            fdoContext->FrontEndPath,
            length);
        return STATUS_UNSUCCESSFUL;
    }
    if (index >= fdoContext->DeviceDescriptor.bNumConfigurations)
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
            __FUNCTION__": %s input index %d bNumConfigurations %d\n",
            fdoContext->FrontEndPath,
            index,
            fdoContext->DeviceDescriptor.bNumConfigurations);
        return STATUS_UNSUCCESSFUL;
    }

    AcquireFdoLock(fdoContext);

    NTSTATUS status = GetDescriptor(
        fdoContext,
        &fdoContext->ScratchPad.Packet,
        USB_CONFIGURATION_DESCRIPTOR_TYPE,
        BMREQUEST_TO_DEVICE,
        index,
        0,
        (USHORT) length,
        length);
    do
    {
        if (!NT_SUCCESS(status))
        {
            TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
                __FUNCTION__": %s failed index %d %x\n",
                fdoContext->FrontEndPath,
                index,
                status);
            break;
        }

        if (fdoContext->ScratchPad.BytesTransferred < sizeof(USB_CONFIGURATION_DESCRIPTOR))
        {
            TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
                __FUNCTION__": %s expected at least %x bytes got %x\n",
                fdoContext->FrontEndPath,
                sizeof(USB_CONFIGURATION_DESCRIPTOR),
                fdoContext->ScratchPad.BytesTransferred);
            status = STATUS_UNSUCCESSFUL;
            break;
        }
        if (fdoContext->ScratchPad.BytesTransferred > length)
        {
            TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
                __FUNCTION__": %s expected no more than %x bytes got %x\n",
                fdoContext->FrontEndPath,
                length,
                fdoContext->ScratchPad.BytesTransferred);
            status = STATUS_UNSUCCESSFUL;
            break;
        }
        RtlCopyMemory(configDescriptor, fdoContext->ScratchPad.Buffer, fdoContext->ScratchPad.BytesTransferred);

        if (configDescriptor->bConfigurationValue == 0)
        {
            if ((index == 0) && 
                (fdoContext->DeviceDescriptor.bNumConfigurations == 1))
            {
                //
                // UGH! This device has a single non-compliant config with a
                // bConfigurationValue of zero, allow it to exist. 
                //
                TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
                    __FUNCTION__": %s Non compliant single config zero value config descriptor allowed\n",
                    fdoContext->FrontEndPath);
                fdoContext->CurrentConfigOffset = 1;
            }
            else
            {
                //
                // UGH! This device has a multiple non-compliant config with a
                // bConfigurationValue of zero, don't allow it to exist. 
                //
                TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
                    __FUNCTION__": %s Non compliant multi config zero value config descriptor not allowed\n",
                    fdoContext->FrontEndPath);
                status = STATUS_UNSUCCESSFUL;
            }
        }

        TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DEVICE,
            __FUNCTION__": index %d Received %d bytes for config descriptor\n"
            "    bLength %x bDescriptorType %x wTotalLength %x bNumInterfaces %x\n"
            "    bConfigurationValue %x iConfiguration %x bmAttributes %x\n"
            "    MaxPower %x\n",
            index,
            fdoContext->ScratchPad.BytesTransferred,
            configDescriptor->bLength,
            configDescriptor->bDescriptorType,
            configDescriptor->wTotalLength,
            configDescriptor->bNumInterfaces,
            configDescriptor->bConfigurationValue,
            configDescriptor->iConfiguration,
            configDescriptor->bmAttributes,
            configDescriptor->MaxPower);
        break;

    } while (1);

    ReleaseFdoLock(fdoContext);
    return status;
}

void
GetDeviceStrings(
    IN PUSB_FDO_CONTEXT fdoContext)
{  
    //
    // support Microsoft OS Descriptors, and fetch this string first.
    //
    GetOsDescriptorString(fdoContext);
    PUSB_STRING lang_id;

    fdoContext->LangId = 0;
    lang_id = GetString(fdoContext, 0);
    if (lang_id)
    {
        PUSHORT p = (PUSHORT)&lang_id->sString[0];
        fdoContext->LangId = (USHORT)*p;

        TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DEVICE,
            __FUNCTION__": Language ID: 0x%04.4x (English 0x0409) Length: %d (4)\n",
          *p,
          lang_id->bLength);
        ExFreePool (lang_id);
    }

    if (fdoContext->DeviceDescriptor.iSerialNumber)
    {
        fdoContext->SerialNumber = GetString(fdoContext,
            fdoContext->DeviceDescriptor.iSerialNumber);
        if (fdoContext->SerialNumber)
        {
            TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DEVICE,
                __FUNCTION__": serial number: %S\n",
                fdoContext->SerialNumber->sString);
        }
    }
    if (fdoContext->DeviceDescriptor.iProduct)
    {
        fdoContext->Product = GetString(
            fdoContext,
            fdoContext->DeviceDescriptor.iProduct);
        if (fdoContext->Product)
        {
            TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DEVICE,
                __FUNCTION__": product: %S\n",
                fdoContext->Product->sString);
        }
    }
    if (fdoContext->DeviceDescriptor.iManufacturer)
    {
        fdoContext->Manufacturer = GetString(fdoContext,
            fdoContext->DeviceDescriptor.iManufacturer);
        if (fdoContext->Manufacturer )
        {
            TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DEVICE,
                __FUNCTION__": manufacturer: %S\n",
                fdoContext->Manufacturer->sString);
        }
    }  
}

NTSTATUS
GetOsDescriptorString(
    IN PUSB_FDO_CONTEXT fdoContext)
{
    NTSTATUS Status = STATUS_UNSUCCESSFUL;
    BOOLEAN doReset = FALSE;
    BOOLEAN doSet = TRUE;
    do
    {
        if (!fdoContext->FetchOsDescriptor)
        {
            TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
                __FUNCTION__": %s this device does not support os descriptors\n",
                fdoContext->FrontEndPath);
            doSet = FALSE;
            break;
        }
        if (fdoContext->OsDescriptorString)
        {
            TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
                __FUNCTION__": %s m_osDescriptor already set\n",
                fdoContext->FrontEndPath);
            Status = STATUS_SUCCESS;
            break;
        }

        fdoContext->OsDescriptorString = (POS_DESCRIPTOR_STRING) GetString(
            fdoContext, OS_STRING_DESCRIPTOR_INDEX); 
        if (!fdoContext->OsDescriptorString)
        {
            TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DEVICE,
                __FUNCTION__": putScratchOnRing failed %x\n",
                Status);
            doReset = TRUE;
            break;
        }

        if (fdoContext->OsDescriptorString->osDescriptor.bLength != 0x12)
        {                       
            TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
                __FUNCTION__": %s OS Descriptor invalid length %x ignoring\n",
                fdoContext->FrontEndPath,
                fdoContext->OsDescriptorString->osDescriptor.bLength);
            break;
        }

        if (fdoContext->OsDescriptorString->osDescriptor.bDescriptorType != 0x03)
        {                      
            TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
                __FUNCTION__": %s OS Descriptor invalid type %x ignoring\n",
                fdoContext->FrontEndPath,
                fdoContext->OsDescriptorString->osDescriptor.bDescriptorType);
            break;
        }

        if (wcsncmp(MS_OS_STRING_SIGNATURE, fdoContext->OsDescriptorString->osDescriptor.MicrosoftString, 7))
        {            
            TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
                __FUNCTION__": %s OS Descriptor invalid String %C%C%C%C%C%C ignoring\n",
                fdoContext->FrontEndPath,
                fdoContext->OsDescriptorString->osDescriptor.MicrosoftString[0],
                fdoContext->OsDescriptorString->osDescriptor.MicrosoftString[1],
                fdoContext->OsDescriptorString->osDescriptor.MicrosoftString[2],
                fdoContext->OsDescriptorString->osDescriptor.MicrosoftString[3],
                fdoContext->OsDescriptorString->osDescriptor.MicrosoftString[4],
                fdoContext->OsDescriptorString->osDescriptor.MicrosoftString[5],
                fdoContext->OsDescriptorString->osDescriptor.MicrosoftString[6]);
            break;
        }
        TraceEvents(TRACE_LEVEL_WARNING, TRACE_DEVICE,
            __FUNCTION__": %s OS Descriptor String found for this device\n",
            fdoContext->FrontEndPath);        
        //
        //  fetch the feature descriptor
        //
        WDF_USB_CONTROL_SETUP_PACKET packet = formatOsFeaturePacket(
            fdoContext->OsDescriptorString->osDescriptor.bVendorCode,
            0,
            0,
            4,
            0x10);

        AcquireFdoLock(fdoContext);
        Status = PutScratchOnRing(
            fdoContext,
            &packet, 
            0x16, 
            UsbdPipeTypeControl, 
            USB_ENDPOINT_DIRECTION_MASK,
            FALSE);
        ReleaseFdoLock(fdoContext);

        if (!NT_SUCCESS(Status))
        {
            TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
                __FUNCTION__": %s putScratchOnRing failed %x\n",
                fdoContext->FrontEndPath,
                Status);
            break;
        }

        Status = WaitForScratchCompletion(fdoContext);
        if (Status != STATUS_SUCCESS)
        {
            TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
                __FUNCTION__": %s wait failed %x\n",
                fdoContext->FrontEndPath,
                Status);
            break;
        }

        if (fdoContext->ScratchPad.Status != 0) // XXX what is the correct constant?
        {
            TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
                __FUNCTION__": %s usb status %x returned\n",
                fdoContext->FrontEndPath,
                fdoContext->ScratchPad.Status);
            Status = STATUS_UNSUCCESSFUL;
            break;
        }

        POS_COMPAT_ID compatIds = (POS_COMPAT_ID) fdoContext->ScratchPad.Buffer;
        if (compatIds->header.dwLength > PAGE_SIZE)
        {
            TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
                __FUNCTION__": %s dwLength %d too big\n",
                fdoContext->FrontEndPath,
                compatIds->header.dwLength);
            Status = STATUS_UNSUCCESSFUL;
            break;
        }

        if (compatIds->header.bcdVersion != 0x100)
        {
            TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
                __FUNCTION__": %s bcdVersion %x not 0x0100\n",
                fdoContext->FrontEndPath,
                compatIds->header.bcdVersion);
            Status = STATUS_UNSUCCESSFUL;
            break;
        }

        if (compatIds->header.wIndex != 4)
        {
            TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
                __FUNCTION__": %s wIndex %x not 4\n",
                fdoContext->FrontEndPath,
                compatIds->header.wIndex);
            Status = STATUS_UNSUCCESSFUL;
            break;
        }

        USHORT length = (compatIds->header.bCount * (USHORT) sizeof(OS_COMPATID_FUNCTION)) +
            (USHORT) sizeof(OS_FEATURE_HEADER);
        if (length != compatIds->header.dwLength)
        {
            TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
                __FUNCTION__": %s computed length %d != reported length %d bCount %d size %d\n",
                fdoContext->FrontEndPath,
                length, 
                compatIds->header.dwLength,
                compatIds->header.bCount,
                sizeof(OS_COMPATID_FUNCTION));
            Status = STATUS_UNSUCCESSFUL;
            break;
        }
        //
        // get the whole thing
        //
        packet = formatOsFeaturePacket(
            fdoContext->OsDescriptorString->osDescriptor.bVendorCode,
            0,
            0,
            4,
            length);
        
        AcquireFdoLock(fdoContext);
        Status = PutScratchOnRing(
            fdoContext,
            &packet, 
            length, 
            UsbdPipeTypeControl, 
            USB_ENDPOINT_DIRECTION_MASK,
            FALSE);
        ReleaseFdoLock(fdoContext);

        if (!NT_SUCCESS(Status))
        {
            TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
                __FUNCTION__": %s putScratchOnRing (2) failed %x\n",
                fdoContext->FrontEndPath,
                Status);
            break;
        }

        Status = WaitForScratchCompletion(fdoContext);
        if (Status != STATUS_SUCCESS)
        {
            TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
                __FUNCTION__": %s wait (2) failed %x\n",
                fdoContext->FrontEndPath,
                Status);
            break;
        }

        if (fdoContext->ScratchPad.Status != 0) // XXX what is the correct constant?
        {
            TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
                __FUNCTION__": %s usb status %x returned (2)\n",
                fdoContext->FrontEndPath,
                fdoContext->ScratchPad.Status);
            Status = STATUS_UNSUCCESSFUL;
            break;
        }
                
        if (compatIds->header.bcdVersion != 0x100)
        {
            TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
                __FUNCTION__": %s compatID bcdVersion %x not 0x100\n",
                fdoContext->FrontEndPath,
                compatIds->header.bcdVersion);
            Status = STATUS_UNSUCCESSFUL;
            break;
        }
        if (compatIds->header.wIndex != 4)
        {
            TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
                __FUNCTION__": %s compatId wIndex %x not 4\n",
                fdoContext->FrontEndPath,
                compatIds->header.wIndex);
            Status = STATUS_UNSUCCESSFUL;
            break;
        }
        if (length != compatIds->header.dwLength)
        {
            TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
                __FUNCTION__": %s compatid dwLength %d not %d\n",
                fdoContext->FrontEndPath,
                compatIds->header.dwLength,
                length);
            Status = STATUS_UNSUCCESSFUL;
            break;
        }
        fdoContext->CompatIds = (POS_COMPAT_ID) ExAllocatePoolWithTag(NonPagedPool,
            length,
            XVU7);
        if (!fdoContext->CompatIds)
        {
            TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
                __FUNCTION__": %s allocation failure\n",
                fdoContext->FrontEndPath);
            Status = STATUS_UNSUCCESSFUL;
            break;
        }
        RtlCopyMemory(fdoContext->CompatIds, fdoContext->ScratchPad.Buffer, length);
                // yay! That was fun!
        TraceEvents(TRACE_LEVEL_WARNING, TRACE_DEVICE,
            __FUNCTION__": %s got compat IDs length %d count %d %s %s\n",
            fdoContext->FrontEndPath,
            fdoContext->CompatIds->header.dwLength,
            fdoContext->CompatIds->header.bCount,
            fdoContext->CompatIds->functions[0].compatibleID,
            fdoContext->CompatIds->functions[0].subCompatibleID);

    } while(0);

    if (!NT_SUCCESS(Status))
    {
        if (fdoContext->OsDescriptorString)
        {
            ExFreePool(fdoContext->OsDescriptorString);
            fdoContext->OsDescriptorString = NULL;
        }
        if (fdoContext->CompatIds)
        {
            ExFreePool(fdoContext->CompatIds);
            fdoContext->CompatIds = NULL;
        }

        if (doSet)
        {
            SetUsbInfo(fdoContext, FALSE);
        }
        //
        // reset the device?
        // 
        if (doReset)
        {
            TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
                __FUNCTION__": %s reset device after os compat failure\n",
                fdoContext->FrontEndPath);

            ResetDevice(fdoContext);
        }
    }
    return Status;
}

/**
 * Allocate and initialize a usb string.
 * @returns initialized USB string as specified by the index, or NULL.
 * Caller must free the string!
 */
PUSB_STRING
GetString(
    IN PUSB_FDO_CONTEXT fdoContext,
    UCHAR index)
{
    PUSB_STRING uString =
        (PUSB_STRING) ExAllocatePoolWithTag(NonPagedPool, sizeof(USB_STRING), XVU8);
    if (!uString)
    {
        TraceEvents(TRACE_LEVEL_WARNING, TRACE_DEVICE,
            __FUNCTION__": %s allocation failed\n",
            fdoContext->FrontEndPath);
        return NULL;
    }

    RtlZeroMemory(uString, sizeof(USB_STRING));

    AcquireFdoLock(fdoContext);
    do
    {

        USHORT ActualLength;

        NTSTATUS Status = GetDescriptor(
            fdoContext,
            &fdoContext->ScratchPad.Packet,
            USB_STRING_DESCRIPTOR_TYPE,
            BMREQUEST_TO_DEVICE,
            index,
            fdoContext->LangId,
            4,
            sizeof(USB_STRING));

        if (!NT_SUCCESS(Status))
        {
            TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DEVICE,
                __FUNCTION__": GetDescriptor length probe failed %x\n",
                Status);
            ExFreePool(uString);
            uString = NULL;
            break;
        }

        ActualLength = (USHORT) ((PUCHAR)fdoContext->ScratchPad.Buffer)[0];
        TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DEVICE,
            __FUNCTION__": GetDescriptor requesting string of length %d (0x%x)\n",
            ActualLength, ActualLength);

        Status = GetDescriptor(
            fdoContext,
            &fdoContext->ScratchPad.Packet,
            USB_STRING_DESCRIPTOR_TYPE,
            BMREQUEST_TO_DEVICE,
            index,
            fdoContext->LangId,
            ActualLength,
            sizeof(USB_STRING));

        if (!NT_SUCCESS(Status))
        {
            TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
                __FUNCTION__": %s actual length %d GetDescriptor failed %x\n",
                fdoContext->FrontEndPath,
                ActualLength,
                Status);
            //
            // ugh. Ok try using 255. Some devices are broken.
            // Note: some other devices are broken the other way.
            //
            Status = GetDescriptor(
                fdoContext,
                &fdoContext->ScratchPad.Packet,
                USB_STRING_DESCRIPTOR_TYPE,
                BMREQUEST_TO_DEVICE,
                index,
                fdoContext->LangId,
                0xff,
                sizeof(USB_STRING));

            if (!NT_SUCCESS(Status))
            {
                TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
                    __FUNCTION__": %s max length 255 GetDescriptor failed %x\n",
                    fdoContext->FrontEndPath,
                    Status);
                ExFreePool(uString);
                uString = NULL;
                break;
            }
        }

        if (fdoContext->ScratchPad.BytesTransferred < sizeof(USB_COMMON_DESCRIPTOR))
        {
            TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
                __FUNCTION__": %s expected %x bytes got %x\n",
                fdoContext->FrontEndPath,
                sizeof(fdoContext->DeviceDescriptor),
                fdoContext->ScratchPad.BytesTransferred);
            ExFreePool(uString);
            uString = NULL;
            break;
        }

        RtlCopyMemory(uString, fdoContext->ScratchPad.Buffer, fdoContext->ScratchPad.BytesTransferred);
        if (uString->bLength < 3)
        {
            TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
                __FUNCTION__": %s less that three bytes of data (%d)\n",
                fdoContext->FrontEndPath,
                uString->bLength);
            ExFreePool(uString);
            uString = NULL;
            break;
        }
        break;

    } while (1);

    ReleaseFdoLock(fdoContext);

    return uString;
}

NTSTATUS
GetCurrentConfiguration(
    IN PUSB_FDO_CONTEXT fdoContext)
{
    AcquireFdoLock(fdoContext);

    NTSTATUS Status = GetCurrentConfigurationLocked(fdoContext);

    ReleaseFdoLock(fdoContext);

    return Status;
}

_Requires_lock_held_(fdoContext->WdfDevice)
NTSTATUS
GetCurrentConfigurationLocked(
    IN PUSB_FDO_CONTEXT fdoContext)
{  
    RtlZeroMemory(&fdoContext->ScratchPad.Packet, sizeof(fdoContext->ScratchPad.Packet));
    fdoContext->ScratchPad.Packet.Packet.bm.Request.Dir = BMREQUEST_DEVICE_TO_HOST;
    fdoContext->ScratchPad.Packet.Packet.bm.Request.Type = BMREQUEST_STANDARD;
    fdoContext->ScratchPad.Packet.Packet.bm.Request.Recipient = 0;
    fdoContext->ScratchPad.Packet.Packet.bRequest = USB_REQUEST_GET_CONFIGURATION;
    fdoContext->ScratchPad.Packet.Packet.wLength = 1;
    fdoContext->ScratchPad.Request = XenUsbdPipeControl;

    NTSTATUS Status = PutScratchOnRing(
        fdoContext,
        &fdoContext->ScratchPad.Packet,
        sizeof(UCHAR),
        UsbdPipeTypeControl,
        USB_ENDPOINT_TYPE_CONTROL | USB_ENDPOINT_DIRECTION_MASK,
        FALSE);

    ReleaseFdoLock(fdoContext);
    Status = WaitForScratchCompletion(fdoContext);
    AcquireFdoLock(fdoContext);

    if (NT_SUCCESS(Status))
    {
        if (fdoContext->ScratchPad.BytesTransferred == 1)
        {
            PUCHAR byte0 = (PUCHAR) &fdoContext->ScratchPad.Data;
            fdoContext->CurrentConfigValue = *byte0;
            TraceEvents(TRACE_LEVEL_WARNING, TRACE_DEVICE,
                __FUNCTION__": %s CurrentConfiguration %d\n",
                fdoContext->FrontEndPath,
                fdoContext->CurrentConfigValue);
        }
        else
        {
            TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
                __FUNCTION__": %s %d m_scratchBytesTransferred unexpected\n",
                fdoContext->FrontEndPath,
                fdoContext->ScratchPad.BytesTransferred);

            if (fdoContext->ResetDevice)
            {
                fdoContext->ResetDevice = FALSE;
                //
                // This is most likely a reset failure catastrophe.
                // Indicate that this device should avoid initialization resets.
                //            
                ReleaseFdoLock(fdoContext);
                SetUsbInfo(fdoContext, fdoContext->FetchOsDescriptor);
                AcquireFdoLock(fdoContext);
            }
        }
    }
    else
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
            __FUNCTION__": %s failed %x\n",
            fdoContext->FrontEndPath,
            Status);
    }
    return Status;
}

NTSTATUS
SetCurrentConfiguration(
    IN PUSB_FDO_CONTEXT fdoContext,
    IN UCHAR configValue)
{
    NTSTATUS Status = STATUS_SUCCESS;

    if (fdoContext->CurrentConfigValue != configValue)
    {
        AcquireFdoLock(fdoContext);
        Status = SetCurrentConfigurationLocked(fdoContext, configValue, FALSE, 0, 0);        
        ReleaseFdoLock(fdoContext);
    }

    return Status;
}

//
// Must be called with lock held.
//
_Requires_lock_held_(fdoContext->WdfDevice)
NTSTATUS
SetCurrentConfigurationLocked(
    IN PUSB_FDO_CONTEXT fdoContext,
    IN UCHAR configValue,
    IN BOOLEAN SetInterface,
    IN USHORT InterfaceNumber,
    IN USHORT AlternateSetting)
{ 

    RtlZeroMemory(&fdoContext->ScratchPad.Packet, sizeof(fdoContext->ScratchPad.Packet));
    fdoContext->ScratchPad.Packet.Packet.bRequest = USB_REQUEST_SET_CONFIGURATION;
    fdoContext->ScratchPad.Packet.Packet.wValue.Bytes.LowByte = configValue;
    fdoContext->ScratchPad.Packet.Packet.wIndex.Value = 0;
    fdoContext->ScratchPad.Packet.Packet.wLength = 0;
    fdoContext->ScratchPad.Request = XenUsbdPipeControl;

    NTSTATUS Status = PutScratchOnRing(
        fdoContext,
        &fdoContext->ScratchPad.Packet,
        sizeof(UCHAR),
        UsbdPipeTypeControl,
        USB_ENDPOINT_TYPE_CONTROL,
        FALSE);

    ReleaseFdoLock(fdoContext);
    Status = WaitForScratchCompletion(fdoContext);
    AcquireFdoLock(fdoContext);

    if (NT_SUCCESS(Status))
    {
        fdoContext->CurrentConfigValue = configValue;  // XXX how to deal with non-compliant configs?
        TraceEvents(TRACE_LEVEL_WARNING, TRACE_DEVICE,
            __FUNCTION__":  %s %d\n",
            fdoContext->FrontEndPath,
            fdoContext->CurrentConfigValue);

        if (SetInterface)
        {
            //
            // select an interface too.
            //
            TraceEvents(TRACE_LEVEL_WARNING, TRACE_DEVICE,
                __FUNCTION__": %s SET_INTERFACE %d Alternate %d\n",
                fdoContext->FrontEndPath,
                InterfaceNumber,
                AlternateSetting);

            RtlZeroMemory(&fdoContext->ScratchPad.Packet, sizeof(fdoContext->ScratchPad.Packet));
            fdoContext->ScratchPad.Packet.Packet.bm.Request.Dir = BMREQUEST_HOST_TO_DEVICE;
            fdoContext->ScratchPad.Packet.Packet.bm.Request.Type = BMREQUEST_STANDARD;
            fdoContext->ScratchPad.Packet.Packet.bm.Request.Recipient = BMREQUEST_TO_INTERFACE;
            fdoContext->ScratchPad.Packet.Packet.bRequest = USB_REQUEST_SET_INTERFACE;
            fdoContext->ScratchPad.Packet.Packet.wValue.Value = AlternateSetting;
            fdoContext->ScratchPad.Packet.Packet.wIndex.Value = InterfaceNumber;
            fdoContext->ScratchPad.Packet.Packet.wLength = 0;

            Status = PutScratchOnRing(
                fdoContext,
                &fdoContext->ScratchPad.Packet,
                sizeof(UCHAR),
                UsbdPipeTypeControl,
                USB_ENDPOINT_TYPE_CONTROL,
                FALSE);

            ReleaseFdoLock(fdoContext);
            Status = WaitForScratchCompletion(fdoContext);
            AcquireFdoLock(fdoContext);

            if (!NT_SUCCESS(Status))
            {
                TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
                    __FUNCTION__": %s SET_INTERFACE failed %x\n",
                    fdoContext->FrontEndPath,
                    Status);
            }
        }
    }
    else
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
            __FUNCTION__": %s failed %x\n",
            fdoContext->FrontEndPath,
            Status);
    }
    return Status;
}

/**
 This function probes the device for a Microsoft OS String Descriptor 
 and for a Microsoft Extended Compat ID OS Feature Descriptor.
 If the Compat ID exists then it is prepended to the PnP enumerator
 for the device.

 See http://msdn.microsoft.com/en-us/windows/hardware/gg487321
 and the link to the documentation at: http://msdn.microsoft.com/en-us/windows/hardware/gg463179

 XXX there are registry locations used in the MSFT implementation that are not implemented
 here. Specifically:
  HKEY_LOCAL_MACHINE\\SYSTEM\\CurrentControlSet\\Control\\usbflags
    each subkey here is named after the VID/PID of an enumerated usb device.
      vvvvpppprrrr
          vvvv - 4 digit vendor
          pppp - 4 digit product
          rrrr - 4 digit revision
    each subkey contains:
       osvc REG_BINARY 
          0x0000 - no MSFT OS string descriptor
          0x01xx - valid response to OS string descriptor
              xx - bVendorCode hex value.

 refer to http://msdn.microsoft.com/en-us/library/ff537430(v=vs.85).aspx
 It might be a good idea to implement this and preserve the registry location as
 probing for the OS string can stall/hang a device that does not support the string.



 returns TRUE if a fetch should be performed, else false.
 TRUE is returned if no record exists for this device or if a record
 exists and indicates the device supports OsDescriptorString fetches.
 */

BOOLEAN
GetUsbInfo(
    IN PUSB_FDO_CONTEXT FdoContext)
{
    FdoContext->FetchOsDescriptor = TRUE; // default is fetch it.
    FdoContext->ResetDevice = FALSE;       // default is no reset.

    NTSTATUS Status = RtlCheckRegistryKey(
        RTL_REGISTRY_CONTROL,
        (PWSTR) FdoContext->UsbInfoEntryName);

    if (!NT_SUCCESS(Status))
    {
        return TRUE;
    }
    //
    // the device key exists, read the osvc value.
    //   
    USHORT Value = 0x0100;
    ULONG  Reset = FALSE;
    RTL_QUERY_REGISTRY_TABLE QueryTable[3]; // over allocated!
    RtlZeroMemory(QueryTable, sizeof(QueryTable));
    QueryTable[0].QueryRoutine = NULL;
    QueryTable[0].Flags = RTL_QUERY_REGISTRY_DIRECT;
    QueryTable[0].Name = L"osvc";
    QueryTable[0].EntryContext = &Value;
    QueryTable[0].DefaultType = REG_BINARY;
    QueryTable[0].DefaultData = &Value;
    QueryTable[0].DefaultLength = sizeof(Value);

    QueryTable[1].QueryRoutine = NULL;
    QueryTable[1].Flags = RTL_QUERY_REGISTRY_DIRECT;
    QueryTable[1].Name = L"ResetOnStart";
    QueryTable[1].EntryContext = &Reset;
    QueryTable[1].DefaultType = REG_DWORD;
    QueryTable[1].DefaultData = &Reset;
    QueryTable[1].DefaultLength = sizeof(Reset);

    Status = RtlQueryRegistryValues(
            RTL_REGISTRY_CONTROL,
            FdoContext->UsbInfoEntryName,
            QueryTable,
            NULL,
            NULL);
    if (NT_SUCCESS(Status))
    {
        if ((Value & 0xFF00) != 0x0100)
        {
            //
            // this is the only time we set fetch to false, if
            // there is an entry and it explicitly states: don't fetch.
            //
            FdoContext->FetchOsDescriptor = FALSE;
        }
        FdoContext->ResetDevice = Reset ? TRUE : FALSE;
        TraceEvents(TRACE_LEVEL_WARNING, TRACE_DEVICE,
            __FUNCTION__": %s Os Descriptors %s, Reset %s\n",
            FdoContext->FrontEndPath,
            FdoContext->FetchOsDescriptor ? "enabled" : "disabled",
            FdoContext->ResetDevice ? "enabled" : "disabled");
    }
    //
    // now check the XP blacklist value.
    //
    RTL_OSVERSIONINFOW VersionInformation;
    RtlZeroMemory(&VersionInformation, sizeof(VersionInformation));
    VersionInformation.dwOSVersionInfoSize = sizeof(RTL_OSVERSIONINFOW);
    VersionInformation.dwMajorVersion = 5;
    VersionInformation.dwMinorVersion = 1;
    Status = RtlGetVersion(&VersionInformation);
    if (!NT_SUCCESS(Status))
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
            __FUNCTION__": RtlGetVersion error %x\n",
            Status);
        return FALSE;
    }
    //
    // supposedly this never fails.
    // Convert from os Major/Minor in upper 16 bits to lower 16 bits.
    //
    // Supported values:
    //   0     all OS releases are supported
    //   0501  XP    or lower blacklisted.
    //   0502  W2K3  or lower blacklisted
    //   0600  Vista or lower blacklisted
    //   0601  Win7  or lower blacklisted
    //   0602  Win8  or lower blacklisted
    //   FFFF  all OS releases are blacklisted.
    //
    ULONG OsVersion = (VersionInformation.dwMajorVersion << 8) |
        (VersionInformation.dwMinorVersion & 0x00FF);
    ULONG BlackListOS = 0; // default no os is blacklisted.

    RtlZeroMemory(QueryTable, sizeof(QueryTable));
    QueryTable[0].QueryRoutine = NULL;
    QueryTable[0].Flags = RTL_QUERY_REGISTRY_DIRECT;
    QueryTable[0].Name = L"OsBlackList";
    QueryTable[0].EntryContext = &BlackListOS;
    QueryTable[0].DefaultType = REG_DWORD;
    QueryTable[0].DefaultData = &BlackListOS;
    QueryTable[0].DefaultLength = sizeof(BlackListOS);

    Status = RtlQueryRegistryValues(
        RTL_REGISTRY_CONTROL,
        FdoContext->UsbInfoEntryName,
        QueryTable,
        NULL,
        NULL);

    if (NT_SUCCESS(Status))
    {
        if (BlackListOS >= OsVersion)
        {
            TraceEvents(TRACE_LEVEL_WARNING, TRACE_DEVICE,
                __FUNCTION__": %s device is blacklisted for OS version %x (%x)\n",
                FdoContext->FrontEndPath,
                OsVersion,
                Value);
            FdoContext->BlacklistDevice = TRUE;
            //
            // XXX add a system errorlog entry.
            //
        }
    }
    return FdoContext->FetchOsDescriptor;
}


void
SetUsbInfo(
    IN PUSB_FDO_CONTEXT FdoContext,
    IN BOOLEAN enable)
{
    USHORT value = enable ? 0x0100 : 0x0000;
    ULONG resetSupport = FdoContext->ResetDevice ? 1 : 0;    
    FdoContext->FetchOsDescriptor = enable;

    NTSTATUS Status = RtlCheckRegistryKey(
        RTL_REGISTRY_CONTROL,
        L"usbflags");

    if (!NT_SUCCESS(Status))
    {
        Status = RtlCreateRegistryKey(            
            RTL_REGISTRY_CONTROL,
            L"usbflags");
        if (!NT_SUCCESS(Status))
        {
            TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
                __FUNCTION__": %s RtlCreateRegistryKey usbflags error %x\n",
                FdoContext->FrontEndPath,
                Status);
            return;
        }
    }

    Status = RtlCheckRegistryKey(
        RTL_REGISTRY_CONTROL,
        FdoContext->UsbInfoEntryName);

    if (!NT_SUCCESS(Status))
    {
        Status = RtlCreateRegistryKey(            
            RTL_REGISTRY_CONTROL,
            FdoContext->UsbInfoEntryName);
        if (!NT_SUCCESS(Status))
        {
            TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
                __FUNCTION__": %s RtlCreateRegistryKey %S error %x\n",
                FdoContext->FrontEndPath,
                FdoContext->UsbInfoEntryName,
                Status);
            return;
        }
    }

    Status = RtlWriteRegistryValue(
        RTL_REGISTRY_CONTROL,
        FdoContext->UsbInfoEntryName,
        L"osvc",
        REG_BINARY,
        &value,
        sizeof(value));
    if (!NT_SUCCESS(Status))
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
            __FUNCTION__": %s RtlWriteRegistryValue %S osvc error %x\n",
            FdoContext->FrontEndPath,
            FdoContext->UsbInfoEntryName,
            Status);
    }
    else
    {
        TraceEvents(TRACE_LEVEL_WARNING, TRACE_DEVICE,
            __FUNCTION__": %s device %S os descriptors %s\n",
            FdoContext->FrontEndPath,
            FdoContext->UsbInfoEntryName,
            enable ? "enabled" : "disabled");
    }    

    Status = RtlWriteRegistryValue(
        RTL_REGISTRY_CONTROL,
        FdoContext->UsbInfoEntryName,
        L"ResetOnStart",
        REG_DWORD,
        &resetSupport,
        sizeof(resetSupport));
    if (!NT_SUCCESS(Status))
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
            __FUNCTION__": %s RtlWriteRegistryValue %S ResetOnStart error %x\n",
            FdoContext->FrontEndPath,
            FdoContext->UsbInfoEntryName,
            Status);
    }
    else
    {
        FdoContext->ResetDevice = resetSupport ? TRUE : FALSE;
        TraceEvents(TRACE_LEVEL_WARNING, TRACE_DEVICE,
            __FUNCTION__": %s device %S ResetOnStart %s\n",
            FdoContext->FrontEndPath,
            FdoContext->UsbInfoEntryName,
            FdoContext->ResetDevice ? "enabled" : "disabled");
    }

}

//
// really minimal validation here
//
PUSB_ENDPOINT_DESCRIPTOR
PipeHandleToEndpointAddressDescriptor(
    IN PUSB_FDO_CONTEXT fdoContext,
    IN USBD_PIPE_HANDLE PipeHandle)
{
    PUSB_ENDPOINT_DESCRIPTOR endpoint = NULL;
    ULONG Index;
    for (Index = 0;
        Index < fdoContext->NumEndpoints;
        Index++)
    {
        if (NT_VERIFY(fdoContext->PipeDescriptors))
        {
            if (PipeHandle == &fdoContext->PipeDescriptors[Index])
            {
                endpoint = fdoContext->PipeDescriptors[Index].endpoint;
                break;
            }
        }
    }
    return endpoint;
}


PIPE_DESCRIPTOR *
EndpointAddressToPipeDescriptor(
    IN PUSB_FDO_CONTEXT fdoContext,
    IN UCHAR EndpointAddress)
{
    PIPE_DESCRIPTOR * pipe = NULL;
    ULONG Index;
    for (Index = 0;
        Index < fdoContext->NumEndpoints;
        Index++)
    {
        if (NT_VERIFY(fdoContext->PipeDescriptors))
        {
            if (EndpointAddress == fdoContext->PipeDescriptors[Index].endpoint->bEndpointAddress)
            {
                pipe = &fdoContext->PipeDescriptors[Index];
                break;
            }
        }
    }
    return pipe;
}



PUSB_INTERFACE_DESCRIPTOR
PipeHandleToInterfaceDescriptor(
    IN PUSB_FDO_CONTEXT fdoContext,
    IN USBD_PIPE_HANDLE PipeHandle)
{
    PUSB_INTERFACE_DESCRIPTOR interfaceDesc = NULL;
    ULONG Index;
    for (Index = 0;
        Index < fdoContext->NumEndpoints;
        Index++)
    {
        if (NT_VERIFY(fdoContext->PipeDescriptors))
        {
            if (PipeHandle == &fdoContext->PipeDescriptors[Index])
            {
                interfaceDesc = fdoContext->PipeDescriptors[Index].interfaceDescriptor;
                break;
            }
        }
    }
    return interfaceDesc;
}

//
// note the assumption that pipes for an interface are adjacent.
//
PIPE_DESCRIPTOR *
FindFirstPipeForInterface(
    IN PUSB_FDO_CONTEXT fdoContext,
    PUSB_CONFIG_INFO configInfo,
    PUSB_INTERFACE_DESCRIPTOR pInterfaceDescriptor)
{
    PIPE_DESCRIPTOR * pipeDescriptors = configInfo ?
        configInfo->m_pipeDescriptors : fdoContext->PipeDescriptors;

    ULONG numEndpoints = configInfo ?
        configInfo->m_numEndpoints : fdoContext->NumEndpoints;

    for (ULONG Index = 0;
        Index < numEndpoints;
        Index++)
    {
        if (NT_VERIFY(pipeDescriptors))
        {
            if (pipeDescriptors[Index].interfaceDescriptor == pInterfaceDescriptor)
            {
                return &pipeDescriptors[Index];
            }
        }
    }
    return NULL;
}

NTSTATUS
SetInterfaceDescriptorPipes(
    IN PUSB_FDO_CONTEXT fdoContext,
    PUSB_CONFIG_INFO configInfo,
    PUSB_INTERFACE_DESCRIPTOR pInterfaceDescriptor,
    PUSBD_INTERFACE_INFORMATION Interface)
{
    PIPE_DESCRIPTOR * pipeDesc = FindFirstPipeForInterface(
        fdoContext,
        configInfo,
        pInterfaceDescriptor);

    NTSTATUS Status = STATUS_SUCCESS;

    if (pipeDesc == NULL)
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_URB,
            __FUNCTION__": %s no pipe descriptor for interface\n",
            fdoContext->FrontEndPath);
        return STATUS_UNSUCCESSFUL;
    }
    for (ULONG pipeIndex = 0;
        pipeIndex < Interface->NumberOfPipes;
        pipeIndex++)
    {
        if (pipeDesc->interfaceDescriptor != pInterfaceDescriptor)
        {
            TraceEvents(TRACE_LEVEL_ERROR, TRACE_URB,
                __FUNCTION__": %s pipe index %d for %p %d %d not associated with interface %p %d %d\n",
                fdoContext->FrontEndPath,
                pipeIndex,
                pipeDesc->interfaceDescriptor,
                pipeDesc->interfaceDescriptor->bInterfaceNumber,
                pipeDesc->interfaceDescriptor->bAlternateSetting,
                pInterfaceDescriptor,
                pInterfaceDescriptor->bInterfaceNumber,
                pInterfaceDescriptor->bAlternateSetting);
            return STATUS_UNSUCCESSFUL;
        }

        PUSB_ENDPOINT_DESCRIPTOR endpoint = pipeDesc->endpoint;
        PUSBD_PIPE_INFORMATION pipe = &Interface->Pipes[pipeIndex];

        pipe->MaximumPacketSize = endpoint->wMaxPacketSize;
        pipe->EndpointAddress = endpoint->bEndpointAddress;
        pipe->Interval = endpoint->bInterval;
        pipe->PipeType = (USBD_PIPE_TYPE) (endpoint->bmAttributes & 0x03);
        pipe->PipeHandle = (USBD_PIPE_HANDLE) pipeDesc;
        
        TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_URB,
            __FUNCTION__": pipe %d maxpacket %d ea %x interval %d type %d handle %p\n",
            pipeIndex,
            pipe->MaximumPacketSize,
            pipe->EndpointAddress,
            pipe->Interval,
            pipe->PipeType,
            pipe->PipeHandle);

        pipeDesc++;
    }
    return Status;
}


PUSB_INTERFACE_DESCRIPTOR
FindInterface(
    IN PUSB_FDO_CONTEXT fdoContext,
    PUSB_CONFIG_INFO configInfo,
    UCHAR InterfaceNumber,
    UCHAR AlternateSetting)
{
    PUSB_INTERFACE_DESCRIPTOR handle = NULL;
    PUSB_INTERFACE_DESCRIPTOR * interfaces = configInfo ?
        configInfo->m_interfaceDescriptors : fdoContext->InterfaceDescriptors;
    ULONG numInterfaces = configInfo ?
        configInfo->m_numInterfaces : fdoContext->NumInterfaces;

    if (interfaces)
    {
        for (ULONG index = 0;
            index < numInterfaces;
            index++)
        {
            if ((interfaces[index]->bInterfaceNumber == InterfaceNumber) &&
                (interfaces[index]->bAlternateSetting == AlternateSetting))
            {
                handle = interfaces[index];
                break;
            }
        }
    }
    return handle;
}


PCHAR DescriptorTypeToString(
    UCHAR DescType)
{
    PCHAR String = "Unknown Descriptor Type";
    switch (DescType)
    {
    case USB_DEVICE_DESCRIPTOR_TYPE:
        String="USB_DEVICE_DESCRIPTOR_TYPE";
        break;

    case USB_CONFIGURATION_DESCRIPTOR_TYPE:
        String="USB_CONFIGURATION_DESCRIPTOR_TYPE";
        break;

    case USB_STRING_DESCRIPTOR_TYPE:
        String="USB_STRING_DESCRIPTOR_TYPE";
        break;

    case USB_INTERFACE_DESCRIPTOR_TYPE:
        String="USB_INTERFACE_DESCRIPTOR_TYPE";
        break;

    case USB_ENDPOINT_DESCRIPTOR_TYPE:
        String="USB_ENDPOINT_DESCRIPTOR_TYPE";
        break;

    case HID_HID_DESCRIPTOR_TYPE:
        String="HID_HID_DESCRIPTOR_TYPE";
        break;

    case HID_REPORT_DESCRIPTOR_TYPE:
        String="HID_REPORT_DESCRIPTOR_TYPE";
        break;

    case HID_PHYSICAL_DESCRIPTOR_TYPE:
        String="HID_PHYSICAL_DESCRIPTOR_TYPE";
        break;
    }
    return String;
}

//
// debugging functions
//
PCHAR UrbFunctionToString(
    USHORT Function)
{
    PCHAR String="Unknown URB function";

    switch(Function)
    {
    case URB_FUNCTION_SELECT_CONFIGURATION:
        String="URB_FUNCTION_SELECT_CONFIGURATION";
        break;

    case URB_FUNCTION_SELECT_INTERFACE:
        String="URB_FUNCTION_SELECT_INTERFACE";
        break;

    case URB_FUNCTION_ABORT_PIPE:
        String="URB_FUNCTION_ABORT_PIPE";
        break;

    case URB_FUNCTION_TAKE_FRAME_LENGTH_CONTROL:
        String="URB_FUNCTION_TAKE_FRAME_LENGTH_CONTROL";
        break;

    case URB_FUNCTION_RELEASE_FRAME_LENGTH_CONTROL:
        String="URB_FUNCTION_RELEASE_FRAME_LENGTH_CONTROL";
        break;

    case URB_FUNCTION_GET_FRAME_LENGTH:
        String="URB_FUNCTION_GET_FRAME_LENGTH";
        break;

    case URB_FUNCTION_SET_FRAME_LENGTH:
        String="URB_FUNCTION_SET_FRAME_LENGTH";
        break;

    case URB_FUNCTION_GET_CURRENT_FRAME_NUMBER:
        String="URB_FUNCTION_GET_CURRENT_FRAME_NUMBER";
        break;

    case URB_FUNCTION_CONTROL_TRANSFER:
        String="URB_FUNCTION_CONTROL_TRANSFER";
        break;

    case URB_FUNCTION_BULK_OR_INTERRUPT_TRANSFER:
        String="URB_FUNCTION_BULK_OR_INTERRUPT_TRANSFER";
        break;

    case URB_FUNCTION_ISOCH_TRANSFER:
        String="URB_FUNCTION_ISOCH_TRANSFER";
        break;

    case URB_FUNCTION_GET_DESCRIPTOR_FROM_DEVICE:
        String="URB_FUNCTION_GET_DESCRIPTOR_FROM_DEVICE";
        break;

    case URB_FUNCTION_SET_DESCRIPTOR_TO_DEVICE:
        String="URB_FUNCTION_SET_DESCRIPTOR_TO_DEVICE";
        break;

    case URB_FUNCTION_SET_FEATURE_TO_DEVICE:
        String="URB_FUNCTION_SET_FEATURE_TO_DEVICE";
        break;

    case URB_FUNCTION_SET_FEATURE_TO_INTERFACE:
        String="URB_FUNCTION_SET_FEATURE_TO_INTERFACE";
        break;

    case URB_FUNCTION_SET_FEATURE_TO_ENDPOINT:
        String="URB_FUNCTION_SET_FEATURE_TO_ENDPOINT";
        break;

    case URB_FUNCTION_CLEAR_FEATURE_TO_DEVICE:
        String="URB_FUNCTION_CLEAR_FEATURE_TO_DEVICE";
        break;

    case URB_FUNCTION_CLEAR_FEATURE_TO_INTERFACE:
        String="URB_FUNCTION_CLEAR_FEATURE_TO_INTERFACE";
        break;

    case URB_FUNCTION_CLEAR_FEATURE_TO_ENDPOINT:
        String="URB_FUNCTION_CLEAR_FEATURE_TO_ENDPOINT";
        break;

    case URB_FUNCTION_GET_STATUS_FROM_DEVICE:
        String="URB_FUNCTION_GET_STATUS_FROM_DEVICE";
        break;

    case URB_FUNCTION_GET_STATUS_FROM_INTERFACE:
        String="URB_FUNCTION_GET_STATUS_FROM_INTERFACE";
        break;

    case URB_FUNCTION_GET_STATUS_FROM_ENDPOINT:
        String="URB_FUNCTION_GET_STATUS_FROM_ENDPOINT";
        break;

    case URB_FUNCTION_RESERVED_0X0016:
        String="URB_FUNCTION_RESERVED_0X0016";
        break;

    case URB_FUNCTION_VENDOR_DEVICE:
        String="URB_FUNCTION_VENDOR_DEVICE";
        break;

    case URB_FUNCTION_VENDOR_INTERFACE:
        String="URB_FUNCTION_VENDOR_INTERFACE";
        break;

    case URB_FUNCTION_VENDOR_ENDPOINT:
        String="URB_FUNCTION_VENDOR_ENDPOINT";
        break;

    case URB_FUNCTION_CLASS_DEVICE:
        String="URB_FUNCTION_CLASS_DEVICE";
        break;

    case URB_FUNCTION_CLASS_INTERFACE:
        String="URB_FUNCTION_CLASS_INTERFACE";
        break;

    case URB_FUNCTION_CLASS_ENDPOINT:
        String="URB_FUNCTION_CLASS_ENDPOINT";
        break;

    case URB_FUNCTION_RESERVE_0X001D:
        String="URB_FUNCTION_RESERVE_0X001D";
        break;

// previously URB_FUNCTION_RESET_PIPE
    case URB_FUNCTION_SYNC_RESET_PIPE_AND_CLEAR_STALL:
        String="URB_FUNCTION_SYNC_RESET_PIPE_AND_CLEAR_STALL";
        break;

    case URB_FUNCTION_CLASS_OTHER:
        String="URB_FUNCTION_CLASS_OTHER";
        break;

    case URB_FUNCTION_VENDOR_OTHER:
        String="URB_FUNCTION_VENDOR_OTHER";
        break;

    case URB_FUNCTION_GET_STATUS_FROM_OTHER:
        String="URB_FUNCTION_GET_STATUS_FROM_OTHER";
        break;

    case URB_FUNCTION_CLEAR_FEATURE_TO_OTHER:
        String="URB_FUNCTION_CLEAR_FEATURE_TO_OTHER";
        break;

    case URB_FUNCTION_SET_FEATURE_TO_OTHER:
        String="URB_FUNCTION_SET_FEATURE_TO_OTHER";
        break;

    case URB_FUNCTION_GET_DESCRIPTOR_FROM_ENDPOINT:
        String="URB_FUNCTION_GET_DESCRIPTOR_FROM_ENDPOINT";
        break;

    case URB_FUNCTION_SET_DESCRIPTOR_TO_ENDPOINT:
        String="URB_FUNCTION_SET_DESCRIPTOR_TO_ENDPOINT";
        break;

    case URB_FUNCTION_GET_CONFIGURATION:
        String="URB_FUNCTION_GET_CONFIGURATION";
        break;

    case URB_FUNCTION_GET_INTERFACE:
        String="URB_FUNCTION_GET_INTERFACE";
        break;

    case URB_FUNCTION_GET_DESCRIPTOR_FROM_INTERFACE:
        String="URB_FUNCTION_GET_DESCRIPTOR_FROM_INTERFACE";
        break;

    case URB_FUNCTION_SET_DESCRIPTOR_TO_INTERFACE:
        String="URB_FUNCTION_SET_DESCRIPTOR_TO_INTERFACE";
        break;

        // USB 2.0 calls start at 0x0030

#if (_WIN32_WINNT >= 0x0501)

    case URB_FUNCTION_GET_MS_FEATURE_DESCRIPTOR:
        String="URB_FUNCTION_GET_MS_FEATURE_DESCRIPTOR";
        break;

    case URB_FUNCTION_SYNC_RESET_PIPE:
        String="URB_FUNCTION_SYNC_RESET_PIPE";
        break;

    case URB_FUNCTION_SYNC_CLEAR_STALL:
        String="URB_FUNCTION_SYNC_CLEAR_STALL";
        break;
#endif

#if (_WIN32_WINNT >= 0x0600)

    case URB_FUNCTION_CONTROL_TRANSFER_EX:
        String="URB_FUNCTION_CONTROL_TRANSFER_EX";
        break;
#endif

    // Reserve 0x002B-0x002F
    case URB_FUNCTION_RESERVE_0X002B:
        String="URB_FUNCTION_RESERVE_0X002B";
        break;

    case URB_FUNCTION_RESERVE_0X002C:
        String="URB_FUNCTION_RESERVE_0X002C";
        break;

    case URB_FUNCTION_RESERVE_0X002D:
        String="URB_FUNCTION_RESERVE_0X002D";
        break;

    case URB_FUNCTION_RESERVE_0X002E:
        String="URB_FUNCTION_RESERVE_0X002E";
        break;

    case URB_FUNCTION_RESERVE_0X002F:
        String="URB_FUNCTION_RESERVE_0X002F";
        break;
    }
    return String;

}

PCHAR UsbIoctlToString(
    ULONG IoControlCode)
{

    PCHAR String="Unknown URB function";

    switch (IoControlCode) 
    {
    case IOCTL_USB_HCD_GET_STATS_1:
        String="IOCTL_USB_HCD_GET_STATS_1";
        break;
    case IOCTL_USB_HCD_GET_STATS_2:
        String="IOCTL_USB_HCD_GET_STATS_2";
        break;
    case IOCTL_USB_HCD_DISABLE_PORT:
        String="IOCTL_USB_HCD_DISABLE_PORT";
        break;
    case IOCTL_USB_HCD_ENABLE_PORT:
        String="IOCTL_USB_HCD_ENABLE_PORT";
        break;
    case IOCTL_USB_DIAGNOSTIC_MODE_ON:
        String="IOCTL_USB_DIAGNOSTIC_MODE_ON";
        break;
    case IOCTL_USB_DIAGNOSTIC_MODE_OFF:
        String="IOCTL_USB_DIAGNOSTIC_MODE_OFF";
        break;
    case IOCTL_USB_GET_ROOT_HUB_NAME: // conflicts with IOCTL_USB_GET_NODE_INFORMATION
        String="IOCTL_USB_GET_ROOT_HUB_NAME";
        break;
    case IOCTL_GET_HCD_DRIVERKEY_NAME:
        String="IOCTL_GET_HCD_DRIVERKEY_NAME";
        break;
    //case IOCTL_USB_GET_NODE_INFORMATION:
    //    String="IOCTL_USB_GET_NODE_INFORMATION";
    //    break;
    case IOCTL_USB_GET_NODE_CONNECTION_INFORMATION:
        String="IOCTL_USB_GET_NODE_CONNECTION_INFORMATION";
        break;
    case IOCTL_USB_GET_DESCRIPTOR_FROM_NODE_CONNECTION:
        String="IOCTL_USB_GET_DESCRIPTOR_FROM_NODE_CONNECTION";
        break;
    case IOCTL_USB_GET_NODE_CONNECTION_NAME:
        String="IOCTL_USB_GET_NODE_CONNECTION_NAME";
        break;
    case IOCTL_USB_DIAG_IGNORE_HUBS_ON:
        String="IOCTL_USB_DIAG_IGNORE_HUBS_ON";
        break;
    case IOCTL_USB_DIAG_IGNORE_HUBS_OFF:
        String="IOCTL_USB_DIAG_IGNORE_HUBS_OFF";
        break;
    case IOCTL_USB_GET_NODE_CONNECTION_DRIVERKEY_NAME:
        String="IOCTL_USB_GET_NODE_CONNECTION_DRIVERKEY_NAME";
        break;
    case IOCTL_USB_GET_HUB_CAPABILITIES:
        String="IOCTL_USB_GET_HUB_CAPABILITIES";
        break;
    case IOCTL_USB_HUB_CYCLE_PORT:
        String="IOCTL_USB_HUB_CYCLE_PORT";
        break;
    case IOCTL_USB_GET_NODE_CONNECTION_ATTRIBUTES:
        String="IOCTL_USB_GET_NODE_CONNECTION_ATTRIBUTES";
        break;
    case IOCTL_USB_GET_NODE_CONNECTION_INFORMATION_EX:
        String="IOCTL_USB_GET_NODE_CONNECTION_INFORMATION_EX";
        break;
#if (_WIN32_WINNT >= 0x0600)
    case IOCTL_USB_RESET_HUB:
        String="IOCTL_USB_RESET_HUB";
        break;
    case IOCTL_USB_GET_HUB_CAPABILITIES_EX:
        String="IOCTL_USB_GET_HUB_CAPABILITIES_EX";
        break;
#endif
    default:
        break;
    }
    return String;
}

PCHAR
UsbFeatureSelectorString(
    USHORT featureSelector)
{
    
    PCHAR featureString = "UNKNOWN";
    switch (featureSelector)
    {
    case USB_FEATURE_ENDPOINT_STALL:
        featureString = "ENDPOINT_STALL";
        break;
    case USB_FEATURE_REMOTE_WAKEUP:
        featureString = "REMOTE_WAKEUP";
    case 2:
        featureString = "TEST_MODE";
    }
    return featureString;
}


PCHAR
AttributesToEndpointTypeString(
    UCHAR attributes)
{
    PCHAR string = "";
    switch (attributes & USB_ENDPOINT_TYPE_MASK)
    {
    case USB_ENDPOINT_TYPE_CONTROL:
        string = "Control";
        break;
    case USB_ENDPOINT_TYPE_ISOCHRONOUS:
        string = "Iso";
        break;
    case USB_ENDPOINT_TYPE_BULK:
        string = "Bulk";
        break;
    case USB_ENDPOINT_TYPE_INTERRUPT:
        string = "Interrupt";
        break;
    }
    return string;
}

VOID
DbgPrintBuffer(
    PVOID buffer,
    ULONG bytesTransferred,
    ULONG Level,
    ULONG Flag)
{
    if ((gDebugLevel >= Level) &&
        (gDebugFlag & Flag))
    {
        ULONG index = 0; 
        WDF_USB_CONTROL_SETUP_PACKET packet;
        while (index < bytesTransferred)
        {
            ULONG remainder = bytesTransferred - index;
            RtlCopyMemory(packet.Generic.Bytes,
                buffer,
                remainder < 8 ? remainder : 8);

            TraceEvents(TRACE_LEVEL_ERROR, TRACE_DRIVER,
                "    %03.3d: %02.2x %02.2x %02.2x %02.2x %02.2x %02.2x %02.2x %02.2x\n",
                index,
                packet.Generic.Bytes[0],
                packet.Generic.Bytes[1],
                packet.Generic.Bytes[2],
                packet.Generic.Bytes[3],
                packet.Generic.Bytes[4],
                packet.Generic.Bytes[5],
                packet.Generic.Bytes[6],
                packet.Generic.Bytes[7]);

            index += 8;
            buffer = ((PUCHAR)buffer) + 8;
        }
    }
}
