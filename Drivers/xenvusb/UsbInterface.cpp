//
// Copyright (c) Citrix Systems, Inc.
//
/// @file UsbInterface.cpp USB Bus Interface Support and Hub interface support.
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
#include "driver.h"
#include "RootHubPdo.h"
#include <hubbusif.h>
#include <wdmguid.h>
#include <HubFpIf.h>
//
/// @todo Bandwidth calculations are currently bogus.
//
ULONG
FdoTotalBandWidth()
{
    return 400000; // usb 2.0 (1.1  would be 12000)
}

ULONG
FdoConsumedBandwidth()
{
    return 0; // this is a lie.
}

VOID 
FdoInterfaceReference(
    PVOID Context)
{
  PUSB_FDO_CONTEXT fdoContext = (PUSB_FDO_CONTEXT) Context;

  TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DEVICE,
      __FUNCTION__": Context %p\n",
      Context);
  InterlockedIncrement(&fdoContext->busInterfaceReferenceCount);
}

VOID 
FdoInterfaceDereference(
    PVOID Context)
{ 
  PUSB_FDO_CONTEXT fdoContext = (PUSB_FDO_CONTEXT) Context;

  TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DEVICE,
      __FUNCTION__": Context %p\n",
      Context);
  //
  // could validate that fdo->Isa() == XUFD;
  //
  if (fdoContext->busInterfaceReferenceCount > 0)
  {
      InterlockedDecrement(&fdoContext->busInterfaceReferenceCount);
  }
}

/* 

Routine Description:

    Service Returns the Highest USBDI Interface Version supported 
    by the port driver.

    Released Interface Versions are:

    Win98Gold,usbd              0x00000102
    Win98SE,usbd                0x00000200
    Win2K,usbd                  0x00000300
    Win98M (Millenium),usbd     0x00000400   

    Usbport                     0x00000500

    IRQL = ANY
    
Arguments:

    VersionInformation - Ptr to USBD_VERSION_INFORMATION 
    HcdCapabilities - Ptr to ULONG that will be filled in with 
                the Host controller (port) driver capability flags.
*/
VOID
FdoGetUSBDIVersion(
    IN PVOID Context,
    IN OUT PUSBD_VERSION_INFORMATION VersionInformation,
    IN OUT PULONG HcdCapabilities)
{ 
    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DEVICE,
        __FUNCTION__": Context %p\n",
        Context);

  VersionInformation->USBDI_Version = USBDI_VERSION; // was  0x00000102;
  //
  // This is actually port dependent. Some ports are 2.0 some are 1.1
  //
  VersionInformation->Supported_USB_Version = 0x00020000; // v 2.0 was 0x00010001;  // v1.1??
  *HcdCapabilities = 0;
}

/* 

Routine Description:

    Returns the current 32 bit USB frame number.  The function 
    replaces the USBD_QueryBusTime Service.

    IRQL = ANY
    
Arguments:

NOTE: unimplemented

*/
NTSTATUS
FdoQueryBusTime(
    IN PVOID Context,   
    IN OUT PULONG CurrentUsbFrame)
{

  PUSB_FDO_CONTEXT fdoContext = (PUSB_FDO_CONTEXT) Context;

    TraceEvents(TRACE_LEVEL_VERBOSE, TRACE_DEVICE,
        __FUNCTION__": Context %p\n",
        Context);

  NTSTATUS Status = STATUS_SUCCESS;

  fdoContext->ScratchPad.FrameNumber++;
  *CurrentUsbFrame = fdoContext->ScratchPad.FrameNumber;

  TraceEvents(TRACE_LEVEL_VERBOSE, TRACE_DEVICE,
        __FUNCTION__": Context %p, CurrentUsbFrame %x\n",
    Context, *CurrentUsbFrame);

  return Status;
}

/*
Routine Description:

    Service exported for Real-Time Thread support.  Allows a driver
    to submit a request without going thru IoCallDriver or allocating 
    an Irp.  

    Additionally the request is scheduled while at high IRQL. The driver
    forfeits any packet level error information when calling this function.

    IRQL = ANY
    
Arguments:

    BusContext - Handle returned from get_bus_interface

    Urb - 

    NOTE: unimplemented
*/
NTSTATUS
FdoSubmitIsoOutUrb(
    IN PVOID Context,
    IN PURB )
{
    TraceEvents(TRACE_LEVEL_WARNING, TRACE_DEVICE,
        __FUNCTION__": Context %p\n",
        Context);

    return STATUS_UNSUCCESSFUL;
}

/* 

Routine Description:

    IRQL = ANY
    
Arguments:

NOTE: unimplemented

*/
NTSTATUS
FdoQueryBusInformation(
    IN PVOID Context,   
    IN ULONG Level,
    IN OUT PVOID BusInformationBuffer,
    IN OUT PULONG BusInformationBufferLength,
    OUT PULONG BusInformationActualLength)
{
    PUSB_FDO_CONTEXT fdoContext = (PUSB_FDO_CONTEXT) Context;

    TraceEvents(TRACE_LEVEL_WARNING, TRACE_DEVICE,
        __FUNCTION__": Context %p\n",
        Context);

    NTSTATUS Status = STATUS_SUCCESS;
    do 
    {
        if (BusInformationBuffer == NULL)
        {
            Status = STATUS_INVALID_PARAMETER;
            break;
        }
        if (BusInformationBufferLength == NULL)
        {
            Status = STATUS_INVALID_PARAMETER;
            break;
        }

        switch (Level)
        {
        case 0:
            {
                PUSB_BUS_INFORMATION_LEVEL_0 busInfo = 
                    (PUSB_BUS_INFORMATION_LEVEL_0) BusInformationBuffer;
                if ((*BusInformationBufferLength) < sizeof (USB_BUS_INFORMATION_LEVEL_0))
                {
                    Status = STATUS_BUFFER_TOO_SMALL;
                    break;
                }
                busInfo->TotalBandwidth = FdoTotalBandWidth();
                busInfo->ConsumedBandwidth = FdoConsumedBandwidth();
                if (BusInformationActualLength)
                {
                    *BusInformationActualLength = sizeof(USB_BUS_INFORMATION_LEVEL_0);
                }
                *BusInformationBufferLength = sizeof (USB_BUS_INFORMATION_LEVEL_0);
            }
            break;
        case 1:
            {
                PUSB_BUS_INFORMATION_LEVEL_1 busInfo = 
                    (PUSB_BUS_INFORMATION_LEVEL_1) BusInformationBuffer;

                if ((*BusInformationBufferLength) < sizeof (USB_BUS_INFORMATION_LEVEL_1))
                {
                    Status = STATUS_BUFFER_TOO_SMALL;
                    break;
                }
                //
                // see if the name will fit
                //
                UNICODE_STRING name;
                WdfStringGetUnicodeString(fdoContext->hcdsymlink, &name);
                ULONG bytesNeeded = sizeof(USB_BUS_INFORMATION_LEVEL_1) + 
                    name.Length;
                if (BusInformationActualLength)
                {
                    *BusInformationActualLength = bytesNeeded;
                }
                if ((*BusInformationBufferLength) < bytesNeeded)
                {
                    Status = STATUS_BUFFER_TOO_SMALL;
                    break;
                }
                *BusInformationBufferLength = bytesNeeded;
                //
                // copy the fixed length data
                //              
                busInfo->TotalBandwidth = FdoTotalBandWidth();
                busInfo->ConsumedBandwidth = FdoConsumedBandwidth();
                busInfo->ControllerNameLength = name.Length;
                if (name.Length)
                {
                    RtlCopyMemory(busInfo->ControllerNameUnicodeString,
                        name.Buffer,
                        name.Length);
                }
            }
            break;

        default:
            if (BusInformationActualLength)
            {
                *BusInformationActualLength = 0;
            }
            Status = STATUS_UNSUCCESSFUL;
            break;
        }
    } while (0);


    TraceEvents(TRACE_LEVEL_WARNING, TRACE_DEVICE,
        __FUNCTION__": Context %p; Level %d Status %x\n",
        Context, 
        Level,
        Status);

    return Status;
}

NTSTATUS
FdoEnumLogEntry(
    PVOID  Context,
    ULONG,
    ULONG,
    ULONG,
    ULONG)
{
    TraceEvents(TRACE_LEVEL_WARNING, TRACE_DEVICE,
        __FUNCTION__": Context %p\n",
        Context);

    return STATUS_UNSUCCESSFUL;
}
//
// @todo this is really asking about the controller - so just lie.
//
BOOLEAN
FdoIsDeviceHighSpeed(
    IN PVOID  Context)
{
    //
    // XXX lie 
    //
    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DEVICE,
        __FUNCTION__": Context %p\n",
        Context);

    return TRUE;
}

//
// V3 DDI
//
NTSTATUS
FdoQueryBusTimeEx(
    IN PVOID Context,   
    OUT PULONG)
{
    TraceEvents(TRACE_LEVEL_WARNING, TRACE_DEVICE,
        __FUNCTION__": Context %p\n",
        Context);

    return STATUS_UNSUCCESSFUL;
}

//
// HcdiOptionFlags is "don't touch" according to the docs.
//
NTSTATUS
#pragma warning(suppress: 6101)
FdoQueryControllerType(
    __in_opt PVOID Context,
    __out_opt PULONG HcdiOptionFlags,
    __out_opt PUSHORT PciVendorId,
    __out_opt PUSHORT PciDeviceId,
    __out_opt PUCHAR PciClass,
    __out_opt PUCHAR PciSubClass,
    __out_opt PUCHAR PciRevisionId,
    __out_opt PUCHAR PciProgIf)
{
    UNREFERENCED_PARAMETER(HcdiOptionFlags);
    UNREFERENCED_PARAMETER(PciVendorId);
    UNREFERENCED_PARAMETER(PciDeviceId);
    UNREFERENCED_PARAMETER(PciRevisionId);

    if (PciClass)
    {
        *PciClass = PCI_CLASS_SERIAL_BUS_CTLR;
    }
    if (PciSubClass)
    {
        *PciSubClass = PCI_SUBCLASS_SB_USB;
    }
    if (PciProgIf)
    {
        *PciProgIf = 0;
    }

    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DEVICE,
        __FUNCTION__": Context %p\n",
        Context);

    return STATUS_SUCCESS;
}

/**
 * @brief process a WDM IRP_MJ_PNP/IRP_MN_QUERY_INTERFACE IRP before the framework processes it.
 * The framework does not support Driver Interfaces with versioned interface structures, it
 * instead expects the Interface GUID to be versioned. The USB Driver Interface (USBDI) does
 * exactly that: one guid multiple versions of the interface structure. So our controller device
 * object has to preprocess the IRP_MN_QUERY_INTERFACE IRP in order to support the USBDI.
 *
 * This function must either handle the IRP in the legacy WDM fashion by either completing the IRP
 * by calling IoCompleteRequest() or deferring the IRP by calling IoMarkIrpPending() and not calling 
 * IoCompleteRequest(), or return the IRP to the framework by calling WdfDeviceWdmDispatchPreprocessedIrp().
 * 
 * *Note* it cannot do both. *Note Also* the return value rules for WDM Irps.
 *
 * @param[in] Device. The handle to the WDFDEVICE object for the controller.
 * @param[in] Irp. The IRP_MN_QUERY_INTERFACE IRP.
 *
 * @retval NTSTATUS STATUS_PENDING iff FdoPreProcessQueryInterface() has called IoMarkIrpPending() and
 * has not called IoCompleteRequest for this IRP.
 * @retval NTSTATUS STATUS_SUCCESS (or any other value for which NT_SUCCESS() equals TRUE) indicates
 * that the IRP has been completed with the *same* NTSTATUS value and has been successfully processed
 * by FdoPreProcessQueryInterface().
 * @retval NTSTATUS STATUS_UNSUCCESSFUL (or any value for which NT_SUCCESS() equals FALSE) indicates
 * that the IRP has been completed with the *same* NTSTATUS value and has been processed with an 
 * error condition by FdoPreProcessQueryInterface().
 * @retval NTSTATUS whatever value the call to WdfDeviceWdmDispatchPreprocessedIrp() returned.
 * 
 */
NTSTATUS
FdoPreProcessQueryInterface(
    IN WDFDEVICE Device,
    IN PIRP Irp)
{
    NTSTATUS Status = Irp->IoStatus.Status;
    PIO_STACK_LOCATION IoStack = IoGetCurrentIrpStackLocation(Irp);
    PUSB_FDO_CONTEXT fdoContext = DeviceGetFdoContext(Device);

    if (InlineIsEqualGUID( USB_BUS_INTERFACE_USBDI_GUID,
        *IoStack->Parameters.QueryInterface.InterfaceType))
    {
        BOOLEAN provideInterface=FALSE;
        switch (IoStack->Parameters.QueryInterface.Version)
        {
        case USB_BUSIF_USBDI_VERSION_0:
            if (IoStack->Parameters.QueryInterface.Size >=
                sizeof(USB_BUS_INTERFACE_USBDI_V0))
            {
                PUSB_BUS_INTERFACE_USBDI_V0 UsbdiInt = 
                  (PUSB_BUS_INTERFACE_USBDI_V0) IoStack->Parameters.QueryInterface.Interface;

                UsbdiInt->Size = sizeof(USB_BUS_INTERFACE_USBDI_V0);
                UsbdiInt->Version = USB_BUSIF_USBDI_VERSION_0;
                UsbdiInt->BusContext = fdoContext;
                UsbdiInt->InterfaceReference = FdoInterfaceReference;
                UsbdiInt->InterfaceDereference = FdoInterfaceDereference;
                UsbdiInt->GetUSBDIVersion = FdoGetUSBDIVersion;
                UsbdiInt->QueryBusTime = FdoQueryBusTime;
                UsbdiInt->SubmitIsoOutUrb = FdoSubmitIsoOutUrb;
                UsbdiInt->QueryBusInformation = FdoQueryBusInformation;                
                
                provideInterface = TRUE;
                Irp->IoStatus.Information = sizeof(USB_BUS_INTERFACE_USBDI_V0);
                InterlockedIncrement(&fdoContext->busInterfaceReferenceCount);
                Status = STATUS_SUCCESS;
            }
            break;
        case USB_BUSIF_USBDI_VERSION_1:
            if (IoStack->Parameters.QueryInterface.Size >=
                sizeof(USB_BUS_INTERFACE_USBDI_V1))
            {
                PUSB_BUS_INTERFACE_USBDI_V1 UsbdiInt = 
                  (PUSB_BUS_INTERFACE_USBDI_V1) IoStack->Parameters.QueryInterface.Interface;

                UsbdiInt->Size = sizeof(USB_BUS_INTERFACE_USBDI_V1);
                UsbdiInt->Version = USB_BUSIF_USBDI_VERSION_1;
                UsbdiInt->BusContext = fdoContext;
                UsbdiInt->InterfaceReference = FdoInterfaceReference;
                UsbdiInt->InterfaceDereference = FdoInterfaceDereference;
                UsbdiInt->GetUSBDIVersion = FdoGetUSBDIVersion;
                UsbdiInt->QueryBusTime = FdoQueryBusTime;
                UsbdiInt->SubmitIsoOutUrb = FdoSubmitIsoOutUrb;
                UsbdiInt->QueryBusInformation = FdoQueryBusInformation;
                UsbdiInt->IsDeviceHighSpeed = FdoIsDeviceHighSpeed;
                
                provideInterface = TRUE;
                Irp->IoStatus.Information = 0;
                InterlockedIncrement(&fdoContext->busInterfaceReferenceCount);
                Status = STATUS_SUCCESS;
            }
            break;
        case USB_BUSIF_USBDI_VERSION_2:
            if (IoStack->Parameters.QueryInterface.Size >=
                sizeof(USB_BUS_INTERFACE_USBDI_V2))
            {
                PUSB_BUS_INTERFACE_USBDI_V2 UsbdiInt = 
                  (PUSB_BUS_INTERFACE_USBDI_V2) IoStack->Parameters.QueryInterface.Interface;

                UsbdiInt->Size = sizeof(USB_BUS_INTERFACE_USBDI_V2);
                UsbdiInt->Version = USB_BUSIF_USBDI_VERSION_2;
                UsbdiInt->BusContext = fdoContext;
                UsbdiInt->InterfaceReference = FdoInterfaceReference;
                UsbdiInt->InterfaceDereference = FdoInterfaceDereference;
                UsbdiInt->GetUSBDIVersion = FdoGetUSBDIVersion;
                UsbdiInt->QueryBusTime = FdoQueryBusTime;
                UsbdiInt->SubmitIsoOutUrb = FdoSubmitIsoOutUrb;
                UsbdiInt->QueryBusInformation = FdoQueryBusInformation;
                UsbdiInt->IsDeviceHighSpeed = FdoIsDeviceHighSpeed;
                UsbdiInt->EnumLogEntry = FdoEnumLogEntry;
                
                provideInterface = TRUE; 
                Irp->IoStatus.Information = 0;
                InterlockedIncrement(&fdoContext->busInterfaceReferenceCount);
                Status = STATUS_SUCCESS;
            }
            break;

            case USB_BUSIF_USBDI_VERSION_3:
            if (IoStack->Parameters.QueryInterface.Size >=
                sizeof(USB_BUS_INTERFACE_USBDI_V3))
            {
                PUSB_BUS_INTERFACE_USBDI_V3 UsbdiInt = 
                  (PUSB_BUS_INTERFACE_USBDI_V3) IoStack->Parameters.QueryInterface.Interface;

                UsbdiInt->Size = sizeof(USB_BUS_INTERFACE_USBDI_V3);
                UsbdiInt->Version = USB_BUSIF_USBDI_VERSION_3;
                UsbdiInt->BusContext = fdoContext;
                UsbdiInt->InterfaceReference = FdoInterfaceReference;
                UsbdiInt->InterfaceDereference = FdoInterfaceDereference;
                UsbdiInt->GetUSBDIVersion = FdoGetUSBDIVersion;
                UsbdiInt->QueryBusTime = FdoQueryBusTime;
                UsbdiInt->SubmitIsoOutUrb = FdoSubmitIsoOutUrb;
                UsbdiInt->QueryBusInformation = FdoQueryBusInformation;
                UsbdiInt->IsDeviceHighSpeed = FdoIsDeviceHighSpeed;
                UsbdiInt->EnumLogEntry = FdoEnumLogEntry;
                UsbdiInt->QueryBusTimeEx = FdoQueryBusTimeEx;
                UsbdiInt->QueryControllerType = FdoQueryControllerType;
                
                provideInterface = TRUE; // not yet supported
                Irp->IoStatus.Information = 0;
                InterlockedIncrement(&fdoContext->busInterfaceReferenceCount);
                Status = STATUS_SUCCESS;
            }
            break;

        default:
            provideInterface = FALSE;
        }
        if (provideInterface)
        {
            TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DEVICE,
                __FUNCTION__": %s USB_BUS_INTERFACE_USBDI version %d\n",
                fdoContext->FrontEndPath,
                IoStack->Parameters.QueryInterface.Version);
        }
        else
        {
            TraceEvents(TRACE_LEVEL_WARNING, TRACE_DEVICE,
                __FUNCTION__": %s USB_BUS_INTERFACE_USBDI version %d XXX NOT IMPLEMENTED\n",
                fdoContext->FrontEndPath,
                IoStack->Parameters.QueryInterface.Version);
        }
    }
    Irp->IoStatus.Status = Status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return Status;
}

//
// Implementation of the HUB interfaces.
//

VOID 
RootHubIfInterfaceReference(
    PVOID Context)
{
  PUSB_HUB_PDO_CONTEXT hubContext =(PUSB_HUB_PDO_CONTEXT) Context;

  TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DEVICE,
      __FUNCTION__": Context %p\n",
      Context);
  InterlockedIncrement(&hubContext->BusInterfaceReferenceCount);
}

VOID 
RootHubIfInterfaceDereference(
    PVOID Context)
{ 
  PUSB_HUB_PDO_CONTEXT hubContext =(PUSB_HUB_PDO_CONTEXT) Context;

  TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DEVICE,
      __FUNCTION__": Context %p\n",
      Context);
  //
  // could validate that fdo->Isa() == XUFD;
  //
  if (hubContext->BusInterfaceReferenceCount > 0)
  {
      InterlockedDecrement(&hubContext->BusInterfaceReferenceCount);
  }
}

/**
 * @brief indicates to the port driver that a new USB device has arrived.
 * Which actually means that the hub driver has created a PDO for a new
 * USB device.
 *
 * @param[in] BusContext. Contains a handle to the bus context returned in the 
 * BusContext member of the structure by an IRP_MN_QUERY_INTERFACE request. 
 * The port driver provides this information when the hub driver queries 
 * for the interface.
 *
 * @param[out] DeviceHandle. On return, contains a handle to the new device 
 * structure created by this routine.
 *
 * @param[in] HubDeviceHandle. Contains a handle to the hub PDO device. 
 *
 * @param[in] PortStatus. According to the existing documentation:
 * "On return, contains the status of the port." But that sense 
 * doesn't it make as it is an input parameter not an output parameter.
 *
 * @param[in] PortNumber. The port number for the new device.
 *
 * @returns NTSTATUS indicating success or failure.
 */
NTSTATUS
USB_BUSIFFN
RootHubIfCreateUsbDevice(
    _In_ PVOID BusContext,
    _Outptr_ PUSB_DEVICE_HANDLE *NewDeviceHandle,
    _In_ PUSB_DEVICE_HANDLE HubDeviceHandle,
    _In_ USHORT PortStatus,
    _In_ USHORT PortNumber)
{
    UNREFERENCED_PARAMETER(HubDeviceHandle);

    PUSB_HUB_PDO_CONTEXT hubContext =(PUSB_HUB_PDO_CONTEXT) BusContext;
    hubContext->PortDevice.DeviceHandleRefCount++;
    // we don't care, 1 hub 1 controller 1 port 1 device.
    *NewDeviceHandle = (PUSB_DEVICE_HANDLE) &hubContext->PortDevice; 
    hubContext->PortDevice.HubPortNumber = PortNumber;
    hubContext->PortDevice.HubPortStatus = PortStatus;
    
    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DEVICE,
        __FUNCTION__": Context %p Port %x Status %x DeviceHandleRefCount %d\n",
        BusContext,
        PortNumber,
        PortStatus,
        hubContext->PortDevice.DeviceHandleRefCount);

    return STATUS_SUCCESS;
}

/**
 * @brief initializes a new USB device.
 * The hub driver calls the InitializeUsbDevice routine to assign a device 
 * address to a new USB device. This routine scans the list of device addresses 
 * that are recorded in the parent hub's device extension until it finds an 
 * unused address and assigns that unused address to the new device. 
 * There are 128 available addresses (0 - 127), as specified by version 1.1 
 * of the Universal Serial Bus specification. The address remains valid 
 * until the device is removed by a call to the RemoveUsbDevice routine, 
 * or until the device is reset or the system powered down. On the next 
 * enumeration, the device might be assigned a different address. For more 
 * information about how USB addresses are assigned, see the section of 
 * the Universal Serial Bus Specification that describes the Set Address 
 * request. 
 * 
 * Actually we don't do anything.
 *
 * @param[in] BusContext. Contains a handle to the bus context returned in the 
 * BusContext member of the structure by an IRP_MN_QUERY_INTERFACE request. 
 * The port driver provides this information when the hub driver queries 
 * for the interface.
 * 
 * @param[in,out] DeviceHandle. Contains a handle to the device created 
 * by CreateUsbDevice.
 *
 * @returns NTSTATUS indicating success or failure.
 */
NTSTATUS
USB_BUSIFFN
RootHubIfInitializeUsbDevice(
    _In_ PVOID BusContext,
    _Inout_ PUSB_DEVICE_HANDLE DeviceHandle)
{
    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DEVICE,
        __FUNCTION__"\n");
    UNREFERENCED_PARAMETER(BusContext);
    UNREFERENCED_PARAMETER(DeviceHandle);

    return STATUS_SUCCESS;
}

//
// V6 or later only V1 functions. (really?)
//
/**
 * @brief indicates to the port driver that a new USB device has arrived.
 * Which actually means that the hub driver has created a PDO for a new
 * USB device.
 *
 * @param[in] BusContext. Contains a handle to the bus context returned in the 
 * BusContext member of the structure by an IRP_MN_QUERY_INTERFACE request. 
 * The port driver provides this information when the hub driver queries 
 * for the interface.
 *
 * @param[out] DeviceHandle. On return, contains a handle to the new device 
 * structure created by this routine.
 *
 * @param[in] HubDeviceHandle. Contains a handle to the hub PDO device. 
 *
 * @param[in] PortStatus. According to the existing documentation:
 * "On return, contains the status of the port." But that sense 
 * doesn't it make as it is an input parameter not an output parameter.
 *
 * @param[in] PortNumber. The port number for the new device.
 *
 * @param[out] CdErrorInfo. Create Device error information.
 * There is no documentation for this output parameter.
 *
 * @param[in] TtPortNumber. Undocumented.
 *
 * @returns NTSTATUS indicating success or failure.
 */
_Function_class_(USB_BUSIFFN_CREATE_USB_DEVICE_EX)
NTSTATUS
USB_BUSIFFN
RootHubIfCreateUsbDeviceEx(
    _In_ PVOID BusContext,
    _Outptr_ PUSB_DEVICE_HANDLE *NewDeviceHandle,
    _In_ PUSB_DEVICE_HANDLE HubDeviceHandle,
    _In_ USHORT PortStatus,
    _In_ USHORT PortNumber,
    _Out_ PUSB_CD_ERROR_INFORMATION CdErrorInfo,
    _In_ USHORT TtPortNumber)
{
    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DEVICE,
        __FUNCTION__"\n");

    PUSB_HUB_PDO_CONTEXT hubContext =(PUSB_HUB_PDO_CONTEXT) BusContext;

    if (CdErrorInfo)
    {
        XXX_TODO("CdErrorInfo unknown usage");
        RtlZeroMemory(CdErrorInfo, sizeof(USB_CD_ERROR_INFORMATION));
    }

    NTSTATUS Status = RootHubIfCreateUsbDevice(
        BusContext,
        NewDeviceHandle,
        HubDeviceHandle,
        PortStatus,
        PortNumber);

    if (NT_SUCCESS(Status))
    {
        hubContext->PortDevice.TtPortNumber = TtPortNumber; // ??
    }
    return Status;
}

//
// And V7 has yet another CreateUsbDevice
//
_Function_class_(USB_BUSIFFN_CREATE_USB_DEVICE_V7)
NTSTATUS
USB_BUSIFFN
RootHubIfCreateUsbDeviceV7 (
    _In_ PVOID BusContext,
    _Outptr_ PUSB_DEVICE_HANDLE *NewDeviceHandle,
    _In_ PUSB_DEVICE_HANDLE HsHubDeviceHandle,
    _In_ USHORT PortStatus,
    _In_ PUSB_PORT_PATH PortPath,
    _Out_ PUSB_CD_ERROR_INFORMATION CdErrorInfo,
    _In_ USHORT TtPortNumber,
    _In_ PDEVICE_OBJECT PdoDeviceObject,
    _In_ PUNICODE_STRING PhysicalDeviceObjectName)
{
    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DEVICE,
        __FUNCTION__"\n");

    UNREFERENCED_PARAMETER(PdoDeviceObject);
    UNREFERENCED_PARAMETER(PhysicalDeviceObjectName);
    UNREFERENCED_PARAMETER(PortPath);

    PUSB_HUB_PDO_CONTEXT hubContext =(PUSB_HUB_PDO_CONTEXT) BusContext;
    NTSTATUS Status = RootHubIfCreateUsbDeviceEx(BusContext,
        NewDeviceHandle,
        HsHubDeviceHandle,
        PortStatus,
        hubContext->Port,
        CdErrorInfo,
        TtPortNumber);
    return Status;
}

/**
 * @brief initializes a new USB device.
 * See RootHubIfInitializeUsbDevice().
 *
 * @param[in] BusContext. Contains a handle to the bus context returned in the 
 * BusContext member of the structure by an IRP_MN_QUERY_INTERFACE request. 
 * The port driver provides this information when the hub driver queries 
 * for the interface.
 * 
 * @param[in,out] DeviceHandle. Contains a handle to the device created 
 * by CreateUsbDevice.
 *
 * @param[out] IdErrInfo. Undocumented Initialize Device Error Information.
 *
 * @returns NTSTATUS indicating success or failure.
 */
_Function_class_(USB_BUSIFFN_INITIALIZE_USB_DEVICE_EX)
NTSTATUS
USB_BUSIFFN
RootHubIfInitializeUsbDeviceEx (
    _In_ PVOID BusContext,
    _Inout_ PUSB_DEVICE_HANDLE DeviceHandle,
    _Out_ PUSB_ID_ERROR_INFORMATION IdErrInfo)
{
    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DEVICE,
        __FUNCTION__"\n");

    if (IdErrInfo)
    {
        RtlZeroMemory(IdErrInfo, sizeof(USB_ID_ERROR_INFORMATION));
    }

    NTSTATUS Status = RootHubIfInitializeUsbDevice(
        BusContext,
        DeviceHandle);

    return Status;
}

//
// V1 If functions
//

/**
 * @brief removes a USB device.
 *
 * @param[in] BusContext. Contains a handle to the bus context returned in the 
 * BusContext member of the structure by an IRP_MN_QUERY_INTERFACE request. 
 * The port driver provides this information when the hub driver queries 
 * for the interface.
 *
 * @param[in,out] DeviceHandle. Contains, on input, a pointer to a handle to 
 * the device created by CreateUsbDevice. If successful, this routine frees 
 * the memory that this member points to. (Or not. We didn't allocate memory.)
 *
 * @param[in] Flags. Contains flag values that indicate how the port driver 
 * should interpret the call to RemoveUsbDevice. Flag values are:
 * Flag                  | Meaning
 * ----------------------|----------------
 * USBD_KEEP_DEVICE_DATA | Do not  free the device handle, after removing the device. 
 * USBD_MARK_DEVICE_BUSY | Stop accepting requests for the device. Handle is valid.
 * 0                     | Handle is freed and device is removed.
 *
 * @returns NTSTATUS indicating success or failure.
 */
_Function_class_(USB_BUSIFFN_REMOVE_USB_DEVICE)
NTSTATUS
USB_BUSIFFN
RootHubIfRemoveUsbDevice (
    _In_ PVOID BusContext,
    _Inout_ PUSB_DEVICE_HANDLE DeviceHandle,
    _In_ ULONG Flags)
{
    PUSB_HUB_PDO_CONTEXT hubContext =(PUSB_HUB_PDO_CONTEXT) BusContext;

    TraceEvents(TRACE_LEVEL_WARNING, TRACE_DEVICE,
        __FUNCTION__" %s Device %p Flags %x DeviceHandleRefCount %d\n",
        GetFdoContextFromHubDevice(hubContext->WdfDevice)->FrontEndPath,
        hubContext->WdfDevice,
        Flags,
        hubContext->PortDevice.DeviceHandleRefCount);

    if (&hubContext->PortDevice != DeviceHandle)
    {
        return STATUS_DEVICE_NOT_CONNECTED;
    }
    hubContext->PortDevice.PortRemoveFlags = Flags;
    if (Flags == 0)
    {
        // we really don't care.
        if (hubContext->PortDevice.DeviceHandleRefCount > 0)
        {
            hubContext->PortDevice.DeviceHandleRefCount--;
        }
    }
    return STATUS_SUCCESS;

}

/**
 * @brief give usbhub the basic descriptors for the usb device.
 * UsbHub will send a 'big enough in most cases' buffer for the variable length
 * Config Descriptor. If the actual config descriptor is bigger than that, just return the
 * invariant portion. Hub will look at that and send a bigger buffer.
 *
 */
_Function_class_(USB_BUSIFFN_GET_USB_DESCRIPTORS)
NTSTATUS
USB_BUSIFFN
RootHubIfGetUsbDescriptors (
    _In_ PVOID BusContext,
    _Inout_ PUSB_DEVICE_HANDLE DeviceHandle,
    _Out_writes_bytes_to_(*DeviceDescriptorBufferLength,*DeviceDescriptorBufferLength) PUCHAR DeviceDescriptorBuffer,
    _Inout_ PULONG DeviceDescriptorBufferLength,
    _Out_writes_bytes_to_(*ConfigDescriptorBufferLength, *ConfigDescriptorBufferLength) PUCHAR ConfigDescriptorBuffer,
    _Inout_ PULONG ConfigDescriptorBufferLength)
{
    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DEVICE,
        __FUNCTION__": BusContext %p DeviceHandle %p DeviceDescriptorBuffer %p Length %d ConfigDescriptorBuffer %p Length %d\n",
        BusContext,
        DeviceHandle,
        DeviceDescriptorBuffer,
        *DeviceDescriptorBufferLength,
        ConfigDescriptorBuffer,
        *ConfigDescriptorBufferLength);

    UNREFERENCED_PARAMETER(DeviceHandle);

    PUSB_HUB_PDO_CONTEXT hubContext =(PUSB_HUB_PDO_CONTEXT) BusContext;
    PUSB_FDO_CONTEXT fdoContext = DeviceGetFdoContext(hubContext->Parent);    
    //
    // win8: DeviceDescriptorBuffer can be null.
    //
    if (DeviceDescriptorBuffer)
    {
        if (*DeviceDescriptorBufferLength < fdoContext->DeviceDescriptor.bLength)
        {
            TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
                __FUNCTION__": %s DeviceDescriptorBufferLength %d too small (%d)\n",
                fdoContext->FrontEndPath,
                *DeviceDescriptorBufferLength,
                fdoContext->DeviceDescriptor.bLength);

            return STATUS_DEVICE_DATA_ERROR;
        }
        RtlCopyMemory(DeviceDescriptorBuffer, &fdoContext->DeviceDescriptor, fdoContext->DeviceDescriptor.bLength);
        *DeviceDescriptorBufferLength = fdoContext->DeviceDescriptor.bLength;
    }
    //
    // win8.1: ConfigDescriptorBuffer can be null.
    //
    if (ConfigDescriptorBuffer)
    {
        if (fdoContext->ConfigurationDescriptor == NULL)
        {
            TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
                __FUNCTION__": %s No config descriptor!\n",
                fdoContext->FrontEndPath);

            return STATUS_DEVICE_DATA_ERROR;
        }
        ULONG copybytes = fdoContext->ConfigurationDescriptor->wTotalLength;

        if (*ConfigDescriptorBufferLength < sizeof(USB_CONFIGURATION_DESCRIPTOR))
        {
            TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
                __FUNCTION__": %s ConfigDescriptorBufferLength %d too small (%d)\n",
                fdoContext->FrontEndPath,
                *ConfigDescriptorBufferLength,
                sizeof(USB_CONFIGURATION_DESCRIPTOR));

            return STATUS_DEVICE_DATA_ERROR;
        }

        if (*ConfigDescriptorBufferLength < fdoContext->ConfigurationDescriptor->wTotalLength)
        {
            copybytes = sizeof(USB_CONFIGURATION_DESCRIPTOR);
        }
        RtlCopyMemory(ConfigDescriptorBuffer, fdoContext->ConfigurationDescriptor, 
            copybytes);
        *ConfigDescriptorBufferLength = copybytes;
    }
    return STATUS_SUCCESS;
}

_Function_class_(USB_BUSIFFN_RESTORE_DEVICE)
NTSTATUS
USB_BUSIFFN
RootHubIfRestoreUsbDevice (
    _In_ PVOID BusContext,
    _Inout_ PUSB_DEVICE_HANDLE OldDeviceHandle,
    _Inout_ PUSB_DEVICE_HANDLE NewDeviceHandle)
{
    UNREFERENCED_PARAMETER(OldDeviceHandle);
    UNREFERENCED_PARAMETER(NewDeviceHandle);

    PUSB_HUB_PDO_CONTEXT hubContext =(PUSB_HUB_PDO_CONTEXT) BusContext;
    TraceEvents(TRACE_LEVEL_WARNING, TRACE_DEVICE,
        __FUNCTION__": %s\n",
        GetFdoContextFromHubDevice(hubContext->WdfDevice)->FrontEndPath);
    return STATUS_SUCCESS;
}

_Function_class_(USB_BUSIFFN_GET_POTRTHACK_FLAGS)
NTSTATUS
USB_BUSIFFN
RootHubIfGetPortHackFlags (
    _In_ PVOID BusContext,
    _Inout_ PULONG Flags)
{
    PUSB_HUB_PDO_CONTEXT hubContext =(PUSB_HUB_PDO_CONTEXT) BusContext;
    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DEVICE,
        __FUNCTION__": %s\n",
        GetFdoContextFromHubDevice(hubContext->WdfDevice)->FrontEndPath);
    *Flags = 0;
    return STATUS_SUCCESS;
}

/**
 * @brief retrieves information about a USB device.
 *
 * @param[in] BusContext. Contains a handle to the bus context returned in the 
 * BusContext member of the structure by an IRP_MN_QUERY_INTERFACE request. 
 * The port driver provides this information when the hub driver queries 
 * for the interface.
 *
 * @param[in] DeviceHandle. Contains a handle to the device created by 
 * RootHubIfCreateUsbDevice().
 *
 * @param[out] DeviceInformationBuffer. Pointer to a buffer that contains 
 * the device data formatted in a structure of type USB_DEVICE_INFORMATION_0.
 *
 * @param[in] DeviceInformationBufferLength. Indicates the length in bytes 
 * of the buffer pointed to by DeviceInformationBuffer. 
 *
 * @param[in,out] LengthOfDataCopied. Indicates the length in bytes of 
 * the data that was returned. 
 *
 * @returns NTSTATUS value indicating success or failure.
 */
_Function_class_(USB_BUSIFFN_GET_DEVICE_INFORMATION)
NTSTATUS
USB_BUSIFFN
RootHubIfQueryDeviceInformation (
    _In_ PVOID BusContext,
    _In_ PUSB_DEVICE_HANDLE DeviceHandle,
    _Out_writes_bytes_to_(DeviceInformationBufferLength,*LengthOfDataCopied) PVOID DeviceInformationBuffer,
    _In_ ULONG DeviceInformationBufferLength,
    _Inout_ PULONG LengthOfDataCopied)
{
    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DEVICE,
        __FUNCTION__": BusContext %p DeviceHandle %p\n",
        BusContext,
        DeviceHandle);

    PUSB_HUB_PDO_CONTEXT hubContext =(PUSB_HUB_PDO_CONTEXT) BusContext;
    PUSB_FDO_CONTEXT fdoContext = DeviceGetFdoContext(hubContext->Parent);
    PUSB_DEVICE_INFORMATION_0 usbInfo = (PUSB_DEVICE_INFORMATION_0) DeviceInformationBuffer;

    if (DeviceInformationBufferLength < sizeof(USB_DEVICE_INFORMATION_0))
    {
        TraceEvents(TRACE_LEVEL_WARNING, TRACE_DEVICE,
            __FUNCTION__": %s DeviceInformationBufferLength %d < %d\n",
            fdoContext->FrontEndPath,
            DeviceInformationBufferLength,
            sizeof(USB_DEVICE_INFORMATION_0));

        return STATUS_INVALID_DEVICE_REQUEST;
    }
    if (usbInfo == NULL)
    {
        TraceEvents(TRACE_LEVEL_WARNING, TRACE_DEVICE,
            __FUNCTION__": %s DeviceInformationBuffer is null\n",
            fdoContext->FrontEndPath);

        return STATUS_INVALID_DEVICE_REQUEST;
    }
    usbInfo->InformationLevel = 0;

    PUSB_CONFIG_INFO configInfo;
    if (DeviceHandle == BusContext)
    {
        usbInfo->DeviceDescriptor = hubContext->HubConfig.DeviceDescriptor;
        usbInfo->CurrentConfigurationValue = 1;
        usbInfo->PortNumber = 0;
        usbInfo->DeviceSpeed = UsbHighSpeed; // ?? UsbFullSpeed?
        configInfo = &hubContext->HubConfig.ConfigInfo;
    }
    else if (DeviceHandle != &hubContext->PortDevice)
    {
        TraceEvents(TRACE_LEVEL_WARNING, TRACE_DEVICE,
            __FUNCTION__": %s Invalid Port device handle %p\n",
            fdoContext->FrontEndPath,
            DeviceHandle);

        return STATUS_INVALID_DEVICE_REQUEST;
    }
    else if (!fdoContext->PortConnected)
    {
        TraceEvents(TRACE_LEVEL_WARNING, TRACE_DEVICE,
            __FUNCTION__": %s port not connected\n",
            fdoContext->FrontEndPath);
        return STATUS_INVALID_DEVICE_REQUEST;
    }
    else
    {
        usbInfo->DeviceDescriptor = fdoContext->DeviceDescriptor;
        usbInfo->CurrentConfigurationValue = fdoContext->CurrentConfigValue;
        usbInfo->PortNumber =  1; 
        usbInfo->DeviceSpeed = fdoContext->DeviceSpeed;
        configInfo = fdoContext->ConfigData;
    }

    ULONG endpoints = configInfo->m_numEndpoints;
    ULONG additionalPipeEntries = endpoints ? endpoints -1 : 0;
    size_t sizeNeeded = sizeof(USB_DEVICE_INFORMATION_0) + 
        additionalPipeEntries * sizeof(USB_PIPE_INFORMATION_0);
    
    usbInfo->ActualLength = (ULONG) sizeNeeded;    
    usbInfo->DeviceAddress = 1;  // faked
    usbInfo->HubAddress = 1;  // faked
    usbInfo->DeviceType = Usb20Device; // faked
    usbInfo->NumberOfOpenPipes = endpoints;    
    *LengthOfDataCopied = (ULONG) sizeNeeded;

    if ((ULONG) sizeNeeded > DeviceInformationBufferLength)
    {
        TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DEVICE,
            __FUNCTION__": %s sizeNeeded %d > DeviceInformationBufferLength %d\n",
            fdoContext->FrontEndPath,
            sizeNeeded,
            DeviceInformationBufferLength);

        return STATUS_BUFFER_TOO_SMALL;
    }
    for (ULONG index = 0; index < endpoints; index++)
    {
        usbInfo->PipeList[index].EndpointDescriptor = 
            *configInfo->m_pipeDescriptors[index].endpoint;
        usbInfo->PipeList[index].ScheduleOffset = 0; // ??
    }
    return STATUS_SUCCESS;
}

//
// V2 If functions
//
_Function_class_(USB_BUSIFFN_INITIALIZE_20HUB)
NTSTATUS
USB_BUSIFFN
RootHubIfInitialize20Hub (
    _In_ PVOID BusContext,
    _In_ PUSB_DEVICE_HANDLE HubDeviceHandle,
    _In_ ULONG TtCount)
{
    TraceEvents(TRACE_LEVEL_WARNING, TRACE_DEVICE,
        __FUNCTION__"\n");

    UNREFERENCED_PARAMETER(HubDeviceHandle);
    UNREFERENCED_PARAMETER(TtCount);

    PUSB_HUB_PDO_CONTEXT hubContext =(PUSB_HUB_PDO_CONTEXT) BusContext;
    UNREFERENCED_PARAMETER(hubContext);

    return STATUS_SUCCESS;
}

_Function_class_(USB_BUSIFFN_GET_DEVICE_BUSCONTEXT)
PVOID
USB_BUSIFFN
RootHubIfGetDeviceBusContext (
    _In_ PVOID BusContext,
    _In_ PVOID DeviceHandle)
{
    UNREFERENCED_PARAMETER(DeviceHandle);
    TraceEvents(TRACE_LEVEL_WARNING, TRACE_DEVICE,
        __FUNCTION__"\n");

    PUSB_HUB_PDO_CONTEXT hubContext =(PUSB_HUB_PDO_CONTEXT) BusContext;
    UNREFERENCED_PARAMETER(hubContext);

    return NULL; // ???
}

/**
 * @brief returns the symbolic name of the root hub device.
 * Strips the leading "\\??\USB\" from the name and just returns
 * the last suffix. Or perhaps just "ROOT_HUB".
 */
_Function_class_(USB_BUSIFFN_GET_ROOTHUB_SYM_NAME)
NTSTATUS
USB_BUSIFFN
RoottHubIfGetRootHubSymbolicName (
    _In_ PVOID BusContext,
    _Inout_updates_bytes_to_(HubSymNameBufferLength, *HubSymNameActualLength) PVOID HubSymNameBuffer,
    _In_ ULONG HubSymNameBufferLength,
    _Out_ PULONG HubSymNameActualLength)
{
    TraceEvents(TRACE_LEVEL_WARNING, TRACE_DEVICE,
        __FUNCTION__"\n");

    PUSB_HUB_PDO_CONTEXT hubContext =(PUSB_HUB_PDO_CONTEXT) BusContext;
    PUSB_FDO_CONTEXT fdoContext = DeviceGetFdoContext(hubContext->Parent);
    PWCHAR buffer = (PWCHAR) HubSymNameBuffer;
    
    UNICODE_STRING hub;
    WdfStringGetUnicodeString(fdoContext->hubsymlink, &hub);
    //
    // Find the last part of the string.
    //
    ULONG length = hub.Length/sizeof(WCHAR);
    ULONG index = length;
    while (index )
    {
        if (hub.Buffer[--index] == L'\\')
        {
            break;
        }
    }
    if (index == 0)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    if (length >= index)
    {
        // wtf?
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    ULONG lengthNeeded = length - index; // in chars, includes null.
    if (lengthNeeded > HubSymNameBufferLength)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    *HubSymNameActualLength = lengthNeeded * sizeof(WCHAR);
    lengthNeeded--;
    RtlCopyMemory(buffer, &hub.Buffer[index+1],
        lengthNeeded * sizeof(WCHAR));
    buffer[lengthNeeded] = 0;

    return STATUS_SUCCESS;
}

/**
 * @brief returns information for all of the ports on the indicated hub.
 *
 * @param[in] BusContext. Contains a handle to the bus context returned in the 
 * BusContext member of the structure by an IRP_MN_QUERY_INTERFACE request. 
 * The port driver provides this information when the hub driver queries 
 * for the interface.
 *
 * @param[in] HubPhysicalDeviceObject.
 *
 * @param[in,out] HubInformationBuffer. Pointer to a structure of type 
 * USB_EXTHUB_INFORMATION_0. On input, the caller must set the 
 * InformationLevel member of USB_EXTHUB_INFORMATION_0 to 0. 
 * The port driver initializes the other members of the structure. 
 * On return, HubInformationBuffer points to the returned data. 
 *
 * @param[in] HubInformationBufferLength. Contains the length in 
 * bytes of the buffer pointed to by HubInformationBuffer.
 *
 * @param[in,out] LengthOfDataReturned. Contains the amount, in bytes, 
 * of the data returned. 
 *
 * @returns NTSTATUS value indicating success or failure.
 *
 */
_Function_class_(USB_BUSIFFN_GET_EXTENDED_HUB_INFO)
NTSTATUS
USB_BUSIFFN
RootHubIfGetExtendedHubInformation (
    _In_ PVOID BusContext,
    _In_ PDEVICE_OBJECT HubPhysicalDeviceObject,
    _Inout_updates_bytes_to_(HubInformationBufferLength, *LengthOfDataCopied) PVOID HubInformationBuffer,
    _In_ ULONG HubInformationBufferLength,
    _Out_ PULONG LengthOfDataCopied)
{
    TraceEvents(TRACE_LEVEL_WARNING, TRACE_DEVICE,
        __FUNCTION__":\n");

    UNREFERENCED_PARAMETER(HubPhysicalDeviceObject);
    UNREFERENCED_PARAMETER(BusContext);

    PUSB_EXTHUB_INFORMATION_0 hubInfo = (PUSB_EXTHUB_INFORMATION_0)
        HubInformationBuffer;

    if (HubInformationBufferLength < sizeof(USB_EXTHUB_INFORMATION_0))
    {
        return STATUS_BUFFER_TOO_SMALL;
    }
    if (hubInfo->InformationLevel != 0)
    {
        return STATUS_NOT_SUPPORTED;
    }
    hubInfo->NumberOfPorts = 1;
    hubInfo->Port[0].PhysicalPortNumber = 1;
    hubInfo->Port[0].PortLabelNumber = 0;
    hubInfo->Port[0].PortAttributes = USB_PORTATTR_SHARED_USB2; // @todo anything else here?
    hubInfo->Port[0].PidOverride = 0;
    hubInfo->Port[0].VidOverride = 0;
    *LengthOfDataCopied = FIELD_OFFSET(USB_EXTHUB_INFORMATION_0, Port[0]); // length of valid data.
    return STATUS_SUCCESS;
}


/**
 * @brief enables the selective-suspend facility on the indicated bus.
 *
 * @param[in] BusContext. Contains a handle to the bus context returned in the 
 * BusContext member of the structure by an IRP_MN_QUERY_INTERFACE request. 
 * The port driver provides this information when the hub driver queries 
 * for the interface.
 *
 * @param[in] Enable. Specifies whether selective suspend is enabled on the bus 
 * indicated by BusContext. A value of TRUE enables selective suspend; 
 * FALSE disables it. 
 *
 * @returns NTSTATUS value indicating success or failure.
 *
 * @todo selective suspend is not implemented.
 */
_Function_class_(USB_BUSIFFN_CONTROLLER_SELECTIVE_SUSPEND)
NTSTATUS
USB_BUSIFFN
RootHubIfControllerSelectiveSuspend (
    _In_ PVOID BusContext,
    _In_ BOOLEAN Enable)
{
    XXX_TODO("Not Implemented!");

    PUSB_HUB_PDO_CONTEXT hubContext =(PUSB_HUB_PDO_CONTEXT) BusContext;
    PUSB_FDO_CONTEXT fdoContext = DeviceGetFdoContext(hubContext->Parent);

    TraceEvents(TRACE_LEVEL_WARNING, TRACE_DEVICE,
        __FUNCTION__": %s %s\n",
        fdoContext->FrontEndPath,
        Enable ? "Enable" : "Disable"); // @todo too high.

    UNREFERENCED_PARAMETER(BusContext);

    if (Enable)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    return STATUS_SUCCESS;
}

/**
 * @brief retrieves information about the host controller.
 *
 * @param[in] BusContext. Contains a handle to the bus context returned in the 
 * BusContext member of the structure by an IRP_MN_QUERY_INTERFACE request. 
 * The port driver provides this information when the hub driver queries 
 * for the interface.
 *
 * @param[in,out] ControllerInformationBuffer. Pointer to a buffer that contains both 
 * the input and the output data, formatted as a USB_CONTROLLER_INFORMATION_0 
 * structure. On input, the caller must fill in the InformationLevel member of 
 * USB_CONTROLLER_INFORMATION_0, and then copy the contents of the structure 
 * into the buffer at ControllerInformationBuffer. InformationLevel must be 
 * set to 0. On completion of the routine, ControllerInformationBuffer points 
 * to a buffer containing a fully initialized USB_CONTROLLER_INFORMATION_0 
 * structure.
 *
 * @param[in] ControllerInformationBufferLength. Indicates the length of the buffer 
 * that the caller allocated at ControllerInformationBuffer. 
 *
 * @param[in,out] LengthOfDataReturned . Indicates the amount of data in bytes that 
 * was actually returned. 
 *
 * @returns NTSTATUS value indicating success or failure.
 */
_Function_class_(USB_BUSIFFN_GET_CONTROLLER_INFORMATION)
NTSTATUS
USB_BUSIFFN
RootHubIfGetControllerInformation (
    _In_ PVOID BusContext,
    _Inout_updates_bytes_to_(ControllerInformationBufferLength, *LengthOfDataCopied) PVOID ControllerInformationBuffer,
    _In_ ULONG ControllerInformationBufferLength,
    _Inout_ PULONG LengthOfDataCopied)
{

    PUSB_HUB_PDO_CONTEXT hubContext =(PUSB_HUB_PDO_CONTEXT) BusContext;
    PUSB_FDO_CONTEXT fdoContext = DeviceGetFdoContext(hubContext->Parent);

    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DEVICE,
        __FUNCTION__": %s\n",
        fdoContext->FrontEndPath);

    PUSB_CONTROLLER_INFORMATION_0 pControllerInfo = 
        (PUSB_CONTROLLER_INFORMATION_0) ControllerInformationBuffer;
    if (ControllerInformationBufferLength < sizeof(USB_CONTROLLER_INFORMATION_0))
    {
        TraceEvents(TRACE_LEVEL_WARNING, TRACE_DEVICE,
            __FUNCTION__": %s ControllerInformationBufferLength %d too small\n",
            fdoContext->FrontEndPath,
            ControllerInformationBufferLength);
        return STATUS_BUFFER_TOO_SMALL;
    }
    if (pControllerInfo->InformationLevel != 0)
    {
        TraceEvents(TRACE_LEVEL_WARNING, TRACE_DEVICE,
            __FUNCTION__": %s pControllerInfo->InformationLevel %d not supported\n",
            fdoContext->FrontEndPath,
            pControllerInfo->InformationLevel);
        return STATUS_NOT_SUPPORTED;
    }
    pControllerInfo->ActualLength = sizeof(USB_CONTROLLER_INFORMATION_0);
    pControllerInfo->IsHighSpeedController = TRUE;
    pControllerInfo->SelectiveSuspendEnabled = FALSE; // @todo support selective suspend.
    *LengthOfDataCopied = sizeof(USB_CONTROLLER_INFORMATION_0);
    return STATUS_SUCCESS;
}

//
// V3 If functions
//
_Function_class_(USB_BUSIFFN_ROOTHUB_INIT_NOTIFY)
NTSTATUS
USB_BUSIFFN
RootHubIfRootHubInitNotification (
    _In_ PVOID BusContext,
    _In_ PVOID CallbackContext,
    _In_ PRH_INIT_CALLBACK CallbackRoutine)
{
    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DEVICE,
        __FUNCTION__": CallbackRoutine %p\n",
        CallbackRoutine);

    NTSTATUS Status = STATUS_SUCCESS;
    PUSB_HUB_PDO_CONTEXT hubContext =(PUSB_HUB_PDO_CONTEXT) BusContext;
    UNREFERENCED_PARAMETER(hubContext);

    if (CallbackRoutine)
    {
        CallbackRoutine(CallbackContext);
    }
    else
    {
        Status = STATUS_CANCELLED;
    }

    return Status;
}

//
// V4 If functions
//

/**
 * @brief Flush all pending IO for the port indicated by the 
 * DeviceHandle parameter.
 *
 * @param[in] BusContext. Contains a handle to the bus context returned in the 
 * BusContext member of the structure by an IRP_MN_QUERY_INTERFACE request. 
 * The port driver provides this information when the hub driver queries 
 * for the interface.
 *
 * @param[in] DeviceHandle. Contains a handle to the device created by 
 * RootHubIfCreateUsbDevice().
 *
 * @todo functionality not implemented.
 */
_Function_class_(USB_BUSIFFN_FLUSH_TRANSFERS)
VOID
USB_BUSIFFN
RootHubIfFlushTransfers (
    _In_ PVOID BusContext,
    _In_ PVOID DeviceHandle)
{
    UNREFERENCED_PARAMETER(DeviceHandle);

    PUSB_HUB_PDO_CONTEXT hubContext =(PUSB_HUB_PDO_CONTEXT) BusContext;
    PUSB_FDO_CONTEXT fdoContext = DeviceGetFdoContext(hubContext->Parent);
    TraceEvents(TRACE_LEVEL_WARNING, TRACE_DEVICE,
        __FUNCTION__": %s\n",
        fdoContext->FrontEndPath);
}

//
// V5 If functions
//
_Function_class_(USB_BUSIFFN_SET_DEVHANDLE_DATA)
VOID
USB_BUSIFFN
RootHubIfSetDeviceHandleData (
    _In_ PVOID BusContext,
    _In_ PVOID DeviceHandle,
    _In_ PDEVICE_OBJECT UsbDevicePdo)
{
    UNREFERENCED_PARAMETER(DeviceHandle);
    UNREFERENCED_PARAMETER(UsbDevicePdo);

    PUSB_HUB_PDO_CONTEXT hubContext =(PUSB_HUB_PDO_CONTEXT) BusContext;
    PUSB_FDO_CONTEXT fdoContext = DeviceGetFdoContext(hubContext->Parent);
    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DEVICE,
        __FUNCTION__": %s\n",
        fdoContext->FrontEndPath);
}

//
// V6 If functions
//
_Function_class_(USB_BUSIFFN_SET_DEVICE_HANDLE_IDLE_READY_STATE)
ULONG
USB_BUSIFFN
RootHubIfSetDeviceHandleIdleReadyState (
    _In_ PVOID BusContext,
    _In_ PUSB_DEVICE_HANDLE DeviceHandle,
    _In_ ULONG NewIdleReadyState)
{
    UNREFERENCED_PARAMETER(DeviceHandle);

    PUSB_HUB_PDO_CONTEXT hubContext =(PUSB_HUB_PDO_CONTEXT) BusContext;
    PUSB_FDO_CONTEXT fdoContext = DeviceGetFdoContext(hubContext->Parent);
    TraceEvents(TRACE_LEVEL_WARNING, TRACE_DEVICE,
        __FUNCTION__": %s\n",
        fdoContext->FrontEndPath);

    return NewIdleReadyState;
}

_Function_class_(USB_BUSIFFN_DEREF_DEVICE_HANDLE)
VOID
USB_BUSIFFN
RootHubIfDerefDeviceHandle (
    _In_ PVOID BusContext,
    _In_ PUSB_DEVICE_HANDLE DeviceHandle,
    _In_ PVOID Object,
    _In_ ULONG Tag)
{
    TraceEvents(TRACE_LEVEL_VERBOSE, TRACE_DEVICE,
        __FUNCTION__": BusContext %p DeviceHandle %p\n",
        BusContext,
        DeviceHandle);

    UNREFERENCED_PARAMETER(Object);
    UNREFERENCED_PARAMETER(Tag);

    PUSB_HUB_PDO_CONTEXT hubContext =(PUSB_HUB_PDO_CONTEXT) BusContext;    

    if (DeviceHandle == &hubContext->PortDevice)
    {
        if (hubContext->PortDevice.DeviceHandleRefCount)
        {
            hubContext->PortDevice.DeviceHandleRefCount--;
        }
    }
}

_Function_class_(USB_BUSIFFN_REF_DEVICE_HANDLE)
NTSTATUS
USB_BUSIFFN
RootHubIfRefDeviceHandle (
    _In_ PVOID BusContext,
    _In_ PUSB_DEVICE_HANDLE DeviceHandle,
    _In_ PVOID Object,
    _In_ ULONG Tag)
{
    TraceEvents(TRACE_LEVEL_VERBOSE, TRACE_DEVICE,
        __FUNCTION__"\n");

    UNREFERENCED_PARAMETER(DeviceHandle);
    UNREFERENCED_PARAMETER(Object);
    UNREFERENCED_PARAMETER(Tag);

    PUSB_HUB_PDO_CONTEXT hubContext =(PUSB_HUB_PDO_CONTEXT) BusContext;
    
    if (DeviceHandle == &hubContext->PortDevice)
    {
        hubContext->PortDevice.DeviceHandleRefCount++;
    }
    return STATUS_SUCCESS;
}

_Function_class_(USB_BUSIFFN_GET_DEVICE_ADDRESS)
NTSTATUS
USB_BUSIFFN
RootHubIfGetDeviceAddress (
    _In_ PVOID BusContext,
    _In_ PUSB_DEVICE_HANDLE DeviceHandle,
    _Out_ PUSHORT DeviceAddress)
{
    UNREFERENCED_PARAMETER(DeviceHandle);

    PUSB_HUB_PDO_CONTEXT hubContext =(PUSB_HUB_PDO_CONTEXT) BusContext;
    PUSB_FDO_CONTEXT fdoContext = DeviceGetFdoContext(hubContext->Parent);
    TraceEvents(TRACE_LEVEL_WARNING, TRACE_DEVICE,
        __FUNCTION__": %s\n",
        fdoContext->FrontEndPath);
    *DeviceAddress = 1;

    return STATUS_SUCCESS;
}

_Function_class_(USB_BUSIFFN_WAIT_ASYNC_POWERUP)
NTSTATUS
USB_BUSIFFN
RootHubIfWaitAsyncPowerUp (
    _In_ PVOID BusContext)
{
    PUSB_HUB_PDO_CONTEXT hubContext =(PUSB_HUB_PDO_CONTEXT) BusContext;
    PUSB_FDO_CONTEXT fdoContext = DeviceGetFdoContext(hubContext->Parent);
    TraceEvents(TRACE_LEVEL_WARNING, TRACE_DEVICE,
        __FUNCTION__": %s\n",
        fdoContext->FrontEndPath);
    return STATUS_SUCCESS;
}

/**
 * @brief a hook into WMI for reporting performance stats.
 * The data structure is USB_DEVICE_PERFORMANCE_INFO_0.
 * @todo actually collect stats. Currently 0's are reported.
 */
_Function_class_(USB_BUSIFFN_GET_DEVICE_PERFORMANCE_INFO)
NTSTATUS
USB_BUSIFFN
RootHubIfGetDevicePerformanceInfo (
    _In_ PVOID BusContext,
    _In_ PUSB_DEVICE_HANDLE DeviceHandle,
    _Out_writes_bytes_to_(DeviceInformationBufferLength,*LengthOfDataCopied) PVOID DeviceInformationBuffer,
    _In_ ULONG DeviceInformationBufferLength,
    _Inout_ PULONG LengthOfDataCopied)
{
    UNREFERENCED_PARAMETER(DeviceHandle);

    TraceEvents(TRACE_LEVEL_VERBOSE, TRACE_DEVICE,
        __FUNCTION__"\n");
    PUSB_DEVICE_PERFORMANCE_INFO_0 perf = (PUSB_DEVICE_PERFORMANCE_INFO_0) DeviceInformationBuffer;
    PUSB_HUB_PDO_CONTEXT hubContext =(PUSB_HUB_PDO_CONTEXT) BusContext;
    PUSB_FDO_CONTEXT fdoContext = GetFdoContextFromHubDevice(hubContext->WdfDevice);

    if (DeviceInformationBufferLength < sizeof(USB_DEVICE_PERFORMANCE_INFO_0))
    {
        return STATUS_INVALID_PARAMETER;
    }
    if (DeviceInformationBuffer == NULL)
    {
        return STATUS_INVALID_PARAMETER;
    }
    *perf = fdoContext->perfInfo;
    *LengthOfDataCopied = sizeof(USB_DEVICE_PERFORMANCE_INFO_0);

    return STATUS_SUCCESS;
}

_Function_class_(USB_BUSIFFN_TEST_POINT)
NTSTATUS
USB_BUSIFFN
RootHubIfHubTestPoint (
    _In_ PVOID BusContext,
    _In_ PVOID DeviceHandle,
    _In_ ULONG Opcode,
    _In_ PVOID TestData)
{
    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DEVICE,
        __FUNCTION__"\n");

    UNREFERENCED_PARAMETER(DeviceHandle);
    UNREFERENCED_PARAMETER(Opcode);
    UNREFERENCED_PARAMETER(TestData);

    PUSB_HUB_PDO_CONTEXT hubContext =(PUSB_HUB_PDO_CONTEXT) BusContext;
    UNREFERENCED_PARAMETER(hubContext);
    return STATUS_SUCCESS;
}

_Function_class_(USB_BUSIFFN_SET_DEVICE_FLAG)
VOID
USB_BUSIFFN
RootHubIfSetDeviceFlag (
    _In_ PVOID BusContext,
    _In_ GUID *DeviceFlagGuid,
    _In_ PVOID ValueData,
    _In_ ULONG ValueLength)
{
    UNREFERENCED_PARAMETER(DeviceFlagGuid);
    UNREFERENCED_PARAMETER(ValueData);
    UNREFERENCED_PARAMETER(ValueLength);

    PUSB_HUB_PDO_CONTEXT hubContext =(PUSB_HUB_PDO_CONTEXT) BusContext;
    PUSB_FDO_CONTEXT fdoContext = DeviceGetFdoContext(hubContext->Parent);
    TraceEvents(TRACE_LEVEL_WARNING, TRACE_DEVICE,
        __FUNCTION__": %s\n",
        fdoContext->FrontEndPath);
}

_Function_class_(USB_BUSIFFN_SET_BUS_WAKE_MODE)
VOID
USB_BUSIFFN
RootHubIfSetBusSystemWakeMode (
    _In_ PVOID BusContext,
    _In_ ULONG Mode)
{
    UNREFERENCED_PARAMETER(Mode);

    PUSB_HUB_PDO_CONTEXT hubContext =(PUSB_HUB_PDO_CONTEXT) BusContext;
    PUSB_FDO_CONTEXT fdoContext = DeviceGetFdoContext(hubContext->Parent);
    TraceEvents(TRACE_LEVEL_WARNING, TRACE_DEVICE,
        __FUNCTION__": %s\n",
        fdoContext->FrontEndPath);
}

_Function_class_(USB_BUSIFFN_CALC_PIPE_BANDWIDTH)
ULONG
USB_BUSIFFN
RootHubIfCaculatePipeBandwidth (
    _In_ PVOID BusContext,
    _In_ PUSBD_PIPE_INFORMATION PipeInfo,
    _In_ USB_DEVICE_SPEED DeviceSpeed)
{
    UNREFERENCED_PARAMETER(PipeInfo);
    UNREFERENCED_PARAMETER(DeviceSpeed);

    PUSB_HUB_PDO_CONTEXT hubContext =(PUSB_HUB_PDO_CONTEXT) BusContext;
    PUSB_FDO_CONTEXT fdoContext = DeviceGetFdoContext(hubContext->Parent);
    TraceEvents(TRACE_LEVEL_WARNING, TRACE_DEVICE,
        __FUNCTION__": %s\n",
        fdoContext->FrontEndPath);
    return 0;
}

_Function_class_(USB_BUSIFFN_RELEASE_SEMAPHORE)
VOID
USB_BUSIFFN
RootHubIfReleaseBusSemaphore (
    _In_ PVOID BusContext)
{
    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DEVICE,
        __FUNCTION__"\n");

    PUSB_HUB_PDO_CONTEXT hubContext =(PUSB_HUB_PDO_CONTEXT) BusContext;
    UNREFERENCED_PARAMETER(hubContext);
}

_Function_class_(USB_BUSIFFN_ACQUIRE_SEMAPHORE)
VOID
USB_BUSIFFN
RootHubIfAcquireBusSemaphore (
    _In_ PVOID BusContext)
{
    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DEVICE,
        __FUNCTION__"\n");

    PUSB_HUB_PDO_CONTEXT hubContext =(PUSB_HUB_PDO_CONTEXT) BusContext;
    UNREFERENCED_PARAMETER(hubContext);
}

_Function_class_(USB_BUSIFFN_IS_ROOT)
BOOLEAN
USB_BUSIFFN
RootHubIfHubIsRoot (
    _In_ PVOID BusContext,
    _In_ PVOID DeviceObject)
{
    PUSB_HUB_PDO_CONTEXT hubContext =(PUSB_HUB_PDO_CONTEXT) BusContext;
    PDEVICE_OBJECT pdoDevice = WdfDeviceWdmGetDeviceObject(hubContext->WdfDevice);
    
    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DEVICE,
        __FUNCTION__": pdoDevice %p DeviceObject %p\n",
        pdoDevice,
        DeviceObject);    

    if (pdoDevice == DeviceObject)
    {
        return TRUE;
    }
    return FALSE;
}

//
// V7 IF Functions
//

_Function_class_(USB_BUSIFFN_GET_CONTAINER_ID_FOR_PORT)
NTSTATUS
USB_BUSIFFN
RootHubIfGetContainerIdForPort (
    _In_ PVOID BusContext,
    _In_ USHORT PortNumber,
    _Out_ LPGUID ContainerId)
{
    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DEVICE,
        __FUNCTION__"\n");

    UNREFERENCED_PARAMETER(PortNumber);

    PUSB_HUB_PDO_CONTEXT hubContext =(PUSB_HUB_PDO_CONTEXT) BusContext;
    *ContainerId = hubContext->PortDevice.ContainerId;
    return STATUS_SUCCESS;
}

_Function_class_(USB_BUSIFFN_SET_CONTAINER_ID_FOR_PORT)
VOID
USB_BUSIFFN
RootHubIfSetContainerIdForPort (
    _In_ PVOID BusContext,
    _In_ USHORT PortNumber,
    _In_ LPGUID ContainerId)
{
    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DEVICE,
        __FUNCTION__"\n");

    UNREFERENCED_PARAMETER(PortNumber);

    PUSB_HUB_PDO_CONTEXT hubContext =(PUSB_HUB_PDO_CONTEXT) BusContext;
    hubContext->PortDevice.ContainerId = *ContainerId;
}

_Function_class_(USB_BUSIFFN_ABORT_ALL_DEVICE_PIPES) 
NTSTATUS
USB_BUSIFFN
RootHubIfAbortAllDevicePipes (
    _In_ PVOID BusContext,
    _In_ PUSB_DEVICE_HANDLE DeviceHandle)
{
    UNREFERENCED_PARAMETER(DeviceHandle);

    PUSB_HUB_PDO_CONTEXT hubContext =(PUSB_HUB_PDO_CONTEXT) BusContext;
    PUSB_FDO_CONTEXT fdoContext = DeviceGetFdoContext(hubContext->Parent);
    TraceEvents(TRACE_LEVEL_WARNING, TRACE_DEVICE,
        __FUNCTION__": %s\n",
        fdoContext->FrontEndPath);
    return STATUS_SUCCESS;
}

_Function_class_(USB_BUSIFFN_SET_DEVICE_ERRATA_FLAG)
VOID
USB_BUSIFFN
RootHubIfSetDeviceErrataFlag (
    _In_ PVOID BusContext,
    _In_ PUSB_DEVICE_HANDLE DeviceHandle,
    _In_ ULONG DeviceErrataFlag)
{
    UNREFERENCED_PARAMETER(DeviceHandle);
    UNREFERENCED_PARAMETER(DeviceErrataFlag);

    PUSB_HUB_PDO_CONTEXT hubContext =(PUSB_HUB_PDO_CONTEXT) BusContext;
    PUSB_FDO_CONTEXT fdoContext = DeviceGetFdoContext(hubContext->Parent);
    TraceEvents(TRACE_LEVEL_WARNING, TRACE_DEVICE,
        __FUNCTION__": %s\n",
        fdoContext->FrontEndPath);
}

//
// V8 If functions
//
_Function_class_(USB_BUSIFFN_GET_DEBUG_PORT_NUMBER)
USHORT
USB_BUSIFFN
RootHubIfGetDebugPortNumber (
    _In_ PVOID BusContext)
{
    PUSB_HUB_PDO_CONTEXT hubContext =(PUSB_HUB_PDO_CONTEXT) BusContext;
    PUSB_FDO_CONTEXT fdoContext = DeviceGetFdoContext(hubContext->Parent);
    TraceEvents(TRACE_LEVEL_WARNING, TRACE_DEVICE,
        __FUNCTION__": %s\n",
        fdoContext->FrontEndPath);

    return 0;
}

//
// USB_BUS_INTERFACE_HUB_MINIDUMP functions
//

VOID 
RootHubIfMinidumpReference(
    PVOID Context)
{
  PUSB_HUB_PDO_CONTEXT hubContext =(PUSB_HUB_PDO_CONTEXT) Context;

  TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DEVICE,
      __FUNCTION__": Context %p\n",
      Context);
  InterlockedIncrement(&hubContext->MinidumpInterfaceReferenceCount);
}

VOID 
RootHubIfMinidumpDereference(
    PVOID Context)
{ 
  PUSB_HUB_PDO_CONTEXT hubContext =(PUSB_HUB_PDO_CONTEXT) Context;

  TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DEVICE,
      __FUNCTION__": Context %p\n",
      Context);

  if (hubContext->MinidumpInterfaceReferenceCount > 0)
  {
      InterlockedDecrement(&hubContext->MinidumpInterfaceReferenceCount);
  }
}

VOID
USB_BUSIFFN
RootHubIfSetUsbPortMiniDumpFlags(
    IN PVOID flags)
{
  TraceEvents(TRACE_LEVEL_WARNING, TRACE_DEVICE,
      __FUNCTION__": Flags %p\n",
      flags);
}

//
// USB_BUS_INTERFACE_HUB_SELECTIVE_SUSPEND
// 

VOID 
RootHubIfSSReference(
    PVOID Context)
{
  PUSB_HUB_PDO_CONTEXT hubContext =(PUSB_HUB_PDO_CONTEXT) Context;

  TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DEVICE,
      __FUNCTION__": Context %p\n",
      Context);
  InterlockedIncrement(&hubContext->SelectiveSuspendInterfaceReferenceCount);
}

VOID 
RootHubIfSSDereference(
    PVOID Context)
{ 
  PUSB_HUB_PDO_CONTEXT hubContext =(PUSB_HUB_PDO_CONTEXT) Context;

  TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DEVICE,
      __FUNCTION__": Context %p\n",
      Context);

  if (hubContext->SelectiveSuspendInterfaceReferenceCount > 0)
  {
      InterlockedDecrement(&hubContext->SelectiveSuspendInterfaceReferenceCount);
  }
}

NTSTATUS
USB_BUSIFFN
RootHubIfSSResumeHub(
    IN PDEVICE_OBJECT PDO)
{
  TraceEvents(TRACE_LEVEL_WARNING, TRACE_DEVICE,
      __FUNCTION__": PDO %p\n",
      PDO);
    return STATUS_SUCCESS;
}

NTSTATUS
USB_BUSIFFN
RootHubIfSSSuspendHub(
    IN PDEVICE_OBJECT PDO)
{
  TraceEvents(TRACE_LEVEL_WARNING, TRACE_DEVICE,
      __FUNCTION__": PDO %p\n",
      PDO);
    return STATUS_UNSUCCESSFUL;
}


//
// USB_BUS_INTERFACE_HUB_FORWARD_PROGRESS SUPPORT (PRELIMINARY)
// 

VOID 
RootHubIfFpReference(
    PVOID Context)
{
  PUSB_HUB_PDO_CONTEXT hubContext =(PUSB_HUB_PDO_CONTEXT) Context;

  InterlockedIncrement(&hubContext->FowardProgressInterfaceReferenceCount);
}

VOID 
RootHubIfFpDereference(
    PVOID Context)
{ 
  PUSB_HUB_PDO_CONTEXT hubContext =(PUSB_HUB_PDO_CONTEXT) Context;

  if (hubContext->FowardProgressInterfaceReferenceCount > 0)
  {
      InterlockedDecrement(&hubContext->FowardProgressInterfaceReferenceCount);
  }
}

PIO_WORKITEM
USB_BUSIFFN
RootHubIfFpAllocateWorkItem(
    _In_opt_ PDEVICE_OBJECT Pdo)
{
    ULONG workItemSize = IoSizeofWorkItem();
    PIO_WORKITEM workitem = (PIO_WORKITEM) ExAllocatePoolWithTag(
        NonPagedPoolNx,
        workItemSize,
        XVUG);

    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DEVICE,
        __FUNCTION__": Pdo %p\n",
        Pdo);

    if (workitem)
    {
        RtlZeroMemory(workitem, workItemSize);
        if (Pdo)
        {
            IoInitializeWorkItem(Pdo, workitem);
        }
    }
    return workitem;
}

VOID
USB_BUSIFFN
RootHubIfFpQueueWorkItem(
    IN PDEVICE_OBJECT Device, // ??? device object??
    IN PIO_WORKITEM  IoWorkItem,
    IN PIO_WORKITEM_ROUTINE  WorkerRoutine,
    IN WORK_QUEUE_TYPE  QueueType,
    IN PVOID  Context,
    IN BOOLEAN flag)
{
    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DEVICE,
        __FUNCTION__": Device %p IoWorkItem %p WorkerRoutine %p QueueType %d Context %p flag %d\n",
        Device,
        IoWorkItem,
        WorkerRoutine,
        QueueType,
        Context,
        flag);
    //
    // This is incomplete. Flag is important. The meaning is in DeferIrpProcessing
    // which calls FilterQueueWorkItem setting "flag" TRUE.
    //
    if (!flag)
    {
        IoQueueWorkItem(IoWorkItem, WorkerRoutine, QueueType, Context);
    }
    else
    {
        //
        // 
        IoQueueWorkItem(IoWorkItem, WorkerRoutine, QueueType, Context);
    }
}

VOID
USB_BUSIFFN
RootHubIfFpFreeWorkItem(
    IN PIO_WORKITEM IoWorkItem)
{	
    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DEVICE,
        __FUNCTION__": IoWorkItem %p\n",
        IoWorkItem);

    IoUninitializeWorkItem(IoWorkItem);
    ExFreePool(IoWorkItem);
}

//
// Guaranteed Forward Progress support.
// This is not currently fully implemented.
//
typedef struct _IRP_WORK_ITEM
{
    PIO_WORKITEM IoWorkItem;
    DIRP_CALLBACK_FUNC Func;
    PIRP Irp;
    BOOLEAN Flag;
} IRP_WORK_ITEM, *PIRP_WORK_ITEM;

_Function_class_(IO_WORKITEM_ROUTINE)
VOID
ProcessIrpWorkItem(
    PDEVICE_OBJECT device,
    PVOID context)
{
    PIRP_WORK_ITEM item = (PIRP_WORK_ITEM) context;
    
    TraceEvents(TRACE_LEVEL_WARNING, TRACE_DEVICE,
        __FUNCTION__": device %p context %p\n",
        device,
        context);

    // usbport derefs the dev.
    item->Func(device, item->Irp);
    ObDereferenceObject(device);
    RootHubIfFpFreeWorkItem(item->IoWorkItem);
    ExFreePool(item);
}

//
// returns allocated IRP_WORK_ITEM with IRP_WORK_ITEM.IoWorkItem set to an allocated IO_WORK_ITEM.
//
PIRP_WORK_ITEM
AllocateIrpWorkItem()
{
    PIRP_WORK_ITEM irpItem =  (PIRP_WORK_ITEM) ExAllocatePoolWithTag(NonPagedPool, 
        sizeof(IRP_WORK_ITEM), XVUH);
    if (irpItem)
    {
        RtlZeroMemory(irpItem, sizeof(IRP_WORK_ITEM));
        irpItem->IoWorkItem = RootHubIfFpAllocateWorkItem(NULL);
        if (!irpItem->IoWorkItem)
        {
            ExFreePool(irpItem);
            irpItem = NULL;
        }
    }
    return irpItem;
}

//
// This is supposed to guarantee IRP forward progress, but
// it currently doesn't.
//
VOID
USB_BUSIFFN
RootHubIfFpDeferIrpProcessing(    
    PDEVICE_OBJECT Device,
    DIRP_CALLBACK_FUNC Func,
    PIRP Irp)
{
    TraceEvents(TRACE_LEVEL_WARNING, TRACE_DEVICE,
        __FUNCTION__": Device %p Func %p Irp %p\n",
        Device,
        Func,
        Irp);

    PIRP_WORK_ITEM workitem = AllocateIrpWorkItem();
    if (!workitem)
    {
        TraceEvents(TRACE_LEVEL_WARNING, TRACE_DEVICE,
            __FUNCTION__": can't allocate irp work item, GFP not implemented\n");
        // die for now! need to go into EmergencyWorkItem mode. TBD
        // note that for a complete implementation IoMarkIrpPending
        // and ObReferenceObject need to be done for this path too.
        XXX_TODO("Implement Guaranteed Forward Progress");

        Irp->IoStatus.Status = STATUS_INSUFFICIENT_RESOURCES;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
    }
    else
    {
        IoMarkIrpPending(Irp);
        ObReferenceObject(Device);
        IoInitializeWorkItem(Device, workitem->IoWorkItem);
        workitem->Func = Func;
        workitem->Irp = Irp;

        RootHubIfFpQueueWorkItem(Device, 
            workitem->IoWorkItem, 
            ProcessIrpWorkItem, 
            DelayedWorkQueue, 
            workitem, 
            TRUE);
    }
}


//
// Root Hub Interface Support
//
NTSTATUS
RootHubPreProcessQueryInterface(
    IN WDFDEVICE Device,
    IN PIRP Irp)
{
    NTSTATUS Status = Irp->IoStatus.Status;
    PIO_STACK_LOCATION IoStack = IoGetCurrentIrpStackLocation(Irp);
    PUSB_HUB_PDO_CONTEXT hubContext = DeviceGetHubPdoContext(Device);  
    PUSB_FDO_CONTEXT fdoContext = DeviceGetFdoContext(hubContext->Parent);  
    BOOLEAN provideInterface=FALSE;
    PUSB_BUS_INTERFACE_HUB_V8 hubif = (PUSB_BUS_INTERFACE_HUB_V8) 
        IoStack->Parameters.QueryInterface.Interface;

    if (InlineIsEqualGUID( USB_BUS_INTERFACE_HUB_GUID,
        *IoStack->Parameters.QueryInterface.InterfaceType))
    {
        hubif->Version = IoStack->Parameters.QueryInterface.Version;
        //
        // sanity test versions 6-8.
        //
        switch(IoStack->Parameters.QueryInterface.Version)
        {
        case USB_BUSIF_HUB_VERSION_8:
            if (IoStack->Parameters.QueryInterface.Size == sizeof(USB_BUS_INTERFACE_HUB_V8))
            {
                provideInterface=TRUE;
                hubif->Size = sizeof(USB_BUS_INTERFACE_HUB_V8);
                hubif->Version = USB_BUSIF_HUB_VERSION_8;
                break;
            }
            break;
        case USB_BUSIF_HUB_VERSION_7:
            // minimum level required by win8.
            if (IoStack->Parameters.QueryInterface.Size == sizeof(USB_BUS_INTERFACE_HUB_V7))
            {
                provideInterface=TRUE;
                hubif->Size = sizeof(USB_BUS_INTERFACE_HUB_V7);
                hubif->Version = USB_BUSIF_HUB_VERSION_7;
                break;
            }
            break;
        case USB_BUSIF_HUB_VERSION_6:
            if (IoStack->Parameters.QueryInterface.Size == sizeof(USB_BUS_INTERFACE_HUB_V6))
            {
                provideInterface=TRUE;
                hubif->Size = sizeof(USB_BUS_INTERFACE_HUB_V6);
                hubif->Version = USB_BUSIF_HUB_VERSION_6;
                break;
            }
            break;
        }

        if (provideInterface)
        {
            switch (IoStack->Parameters.QueryInterface.Version)
            {
            case USB_BUSIF_HUB_VERSION_8:
                //
                // v8 hub drver functions:
                //
                hubif->GetDebugPortNumber = RootHubIfGetDebugPortNumber;
                //
                // fall through.
                //
            case USB_BUSIF_HUB_VERSION_7:
                //
                // v7 hub driver functions:
                //
                hubif->CreateUsbDeviceV7 = RootHubIfCreateUsbDeviceV7;
                hubif->GetContainerIdForPort = RootHubIfGetContainerIdForPort;
                hubif->SetContainerIdForPort = RootHubIfSetContainerIdForPort;
                hubif->AbortAllDevicePipes = RootHubIfAbortAllDevicePipes;
                hubif->SetDeviceErrataFlag = RootHubIfSetDeviceErrataFlag;
                //
                // fall through
                //
            case USB_BUSIF_HUB_VERSION_6:
                //
                // v6 hub driver functions:
                //
                hubif->HubIsRoot = RootHubIfHubIsRoot;
                hubif->AcquireBusSemaphore = RootHubIfAcquireBusSemaphore;
                hubif->ReleaseBusSemaphore = RootHubIfReleaseBusSemaphore;
                hubif->CaculatePipeBandwidth = RootHubIfCaculatePipeBandwidth;
                hubif->SetBusSystemWakeMode = RootHubIfSetBusSystemWakeMode;
                hubif->SetDeviceFlag = RootHubIfSetDeviceFlag;
                hubif->HubTestPoint = RootHubIfHubTestPoint;
                hubif->GetDevicePerformanceInfo = RootHubIfGetDevicePerformanceInfo;
                hubif->WaitAsyncPowerUp = RootHubIfWaitAsyncPowerUp;
                hubif->GetDeviceAddress = RootHubIfGetDeviceAddress;
                hubif->RefDeviceHandle = RootHubIfRefDeviceHandle;
                hubif->DerefDeviceHandle = RootHubIfDerefDeviceHandle;
                hubif->SetDeviceHandleIdleReadyState = RootHubIfSetDeviceHandleIdleReadyState;
                //
                // v5 hub driver functions:
                //
                hubif->SetDeviceHandleData = RootHubIfSetDeviceHandleData;
                //
                // v4 hub driver functions:
                //
                hubif->FlushTransfers = RootHubIfFlushTransfers;
                //
                // v3 hub driver functions:
                //
                hubif->RootHubInitNotification = RootHubIfRootHubInitNotification;
                //
                // v2 hub driver functions:
                //
                hubif->GetControllerInformation = RootHubIfGetControllerInformation;
                hubif->ControllerSelectiveSuspend = RootHubIfControllerSelectiveSuspend;
                hubif->GetExtendedHubInformation = RootHubIfGetExtendedHubInformation;
                hubif->GetRootHubSymbolicName = RoottHubIfGetRootHubSymbolicName;
                hubif->GetDeviceBusContext = RootHubIfGetDeviceBusContext;
                hubif->Initialize20Hub = RootHubIfInitialize20Hub;
                //
                //
                // V1 hub driver functions: (not exactly.)
                //
                hubif->CreateUsbDevice = RootHubIfCreateUsbDeviceEx;
                hubif->InitializeUsbDevice = RootHubIfInitializeUsbDeviceEx;
                hubif->GetUsbDescriptors = RootHubIfGetUsbDescriptors;
                hubif->RemoveUsbDevice = RootHubIfRemoveUsbDevice;
                hubif->RestoreUsbDevice = RootHubIfRestoreUsbDevice;
                hubif->GetPortHackFlags = RootHubIfGetPortHackFlags;
                hubif->QueryDeviceInformation = RootHubIfQueryDeviceInformation;
                //
                // Standard bus interface.
                //
                hubif->BusContext = hubContext;
                hubif->InterfaceDereference = RootHubIfInterfaceDereference;
                hubif->InterfaceReference = RootHubIfInterfaceReference;

                InterlockedIncrement(&hubContext->BusInterfaceReferenceCount);
                break;

            default:
                break;
            }
        }
        //
        // log all hub interfaces for now.
        //
        TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DEVICE,
            __FUNCTION__": %s USB_BUS_INTERFACE_HUB version %d %s\n",
            fdoContext->FrontEndPath,
            IoStack->Parameters.QueryInterface.Version,
            provideInterface? "Implemented" : "Not Implemented");
    }    
    else  if (InlineIsEqualGUID( USB_BUS_INTERFACE_HUB_MINIDUMP_GUID,
        *IoStack->Parameters.QueryInterface.InterfaceType))
    {
        // USB_BUS_INTERFACE_HUB_MINIDUMP has one function:
        // SetUsbPortMiniDumpFlags

        if ((IoStack->Parameters.QueryInterface.Size == sizeof(USB_BUS_INTERFACE_HUB_MINIDUMP)) &&
            (IoStack->Parameters.QueryInterface.Version == USB_BUSIF_HUB_MIDUMP_VERSION_0))
        {
            PUSB_BUS_INTERFACE_HUB_MINIDUMP minidump = (PUSB_BUS_INTERFACE_HUB_MINIDUMP)
                IoStack->Parameters.QueryInterface.Interface;

            minidump->Size = sizeof(USB_BUS_INTERFACE_HUB_MINIDUMP);
            minidump->Version = USB_BUSIF_HUB_MIDUMP_VERSION_0;
            minidump->BusContext = hubContext;
            minidump->InterfaceDereference = RootHubIfMinidumpReference;
            minidump->InterfaceDereference = RootHubIfMinidumpDereference;
            minidump->SetUsbPortMiniDumpFlags = RootHubIfSetUsbPortMiniDumpFlags;

            TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DEVICE,
                __FUNCTION__": USB_BUS_INTERFACE_HUB_MINIDUMP_GUID supported size %d functions %d\n",
                minidump->Size, 3);
            
            InterlockedIncrement(&hubContext->MinidumpInterfaceReferenceCount);
            provideInterface = TRUE;
        }
        else
        {
            TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DEVICE,
                __FUNCTION__": %s USB_BUS_INTERFACE_HUB_MINIDUMP_GUID unsupported\n",
                fdoContext->FrontEndPath);
        }
    }
    else  if (InlineIsEqualGUID( USB_BUS_INTERFACE_HUB_SS_GUID,
        *IoStack->Parameters.QueryInterface.InterfaceType))
    {
        //
        // USB_BUS_INTERFACE_HUB_SELECTIVE_SUSPEND has two functions:
        // SuspendHub and ResumeHub.
        //
        if ((IoStack->Parameters.QueryInterface.Size == sizeof(USB_BUS_INTERFACE_HUB_SELECTIVE_SUSPEND)) &&
            (IoStack->Parameters.QueryInterface.Version == USB_BUSIF_HUB_SS_VERSION_0))
        {
            PUSB_BUS_INTERFACE_HUB_SELECTIVE_SUSPEND hubSuspend = (PUSB_BUS_INTERFACE_HUB_SELECTIVE_SUSPEND)
                IoStack->Parameters.QueryInterface.Interface;

            hubSuspend->Size = sizeof(USB_BUS_INTERFACE_HUB_SELECTIVE_SUSPEND);
            hubSuspend->Version = USB_BUSIF_HUB_SS_VERSION_0;
            hubSuspend->InterfaceDereference = RootHubIfSSDereference;
            hubSuspend->InterfaceReference = RootHubIfSSReference;
            hubSuspend->BusContext = hubContext;
            hubSuspend->ResumeHub = RootHubIfSSResumeHub;
            hubSuspend->SuspendHub = RootHubIfSSSuspendHub;

            TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DEVICE,
                __FUNCTION__": USB_BUS_INTERFACE_HUB_SS_GUID supported size %d functions %d\n",
                hubSuspend->Size, 4);
            
            InterlockedIncrement(&hubContext->SelectiveSuspendInterfaceReferenceCount);
            provideInterface = TRUE;
        }
        else
        {
            TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DEVICE,
                __FUNCTION__": %s USB_BUS_INTERFACE_HUB_SS_GUID unsupported\n",
                fdoContext->FrontEndPath);
        }
    }
    else  if (InlineIsEqualGUID( USB_BUS_INTERFACE_USBDI_GUID,
        *IoStack->Parameters.QueryInterface.InterfaceType))
    {
        // this is the controller interface, ignored here, processed by the controller device.
        TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DEVICE,
                __FUNCTION__": USB_BUS_INTERFACE_USBDI_GUID passed down to controller\n");
    }
    else if ( InlineIsEqualGUID( GUID_PNP_LOCATION_INTERFACE,
        *IoStack->Parameters.QueryInterface.InterfaceType))
    {
        TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DEVICE,
            __FUNCTION__": GUID_PNP_LOCATION_INTERFACE unsupported\n");
    }
    else if ( InlineIsEqualGUID( USB_BUS_INTERFACE_HUB_FORWARD_PROGRESS_GUID,
        *IoStack->Parameters.QueryInterface.InterfaceType))
    {
        // required by win8 but nulling out all the non-generic callbacks appears to be ok.

        if ((IoStack->Parameters.QueryInterface.Size == sizeof(USB_BUS_INTERFACE_HUB_FORWARD_PROGRESS)) &&
            (IoStack->Parameters.QueryInterface.Version == 0))
        {
            PUSB_BUS_INTERFACE_HUB_FORWARD_PROGRESS fp = (PUSB_BUS_INTERFACE_HUB_FORWARD_PROGRESS)
                IoStack->Parameters.QueryInterface.Interface;

            RtlZeroMemory(fp, sizeof(USB_BUS_INTERFACE_HUB_FORWARD_PROGRESS));
            fp->Size = sizeof(USB_BUS_INTERFACE_HUB_FORWARD_PROGRESS);
            fp->Version = 0;
            fp->BusContext = hubContext;
            fp->InterfaceDereference = RootHubIfFpDereference;
            fp->InterfaceReference = RootHubIfFpReference;
            //
            fp->AllocateWorkItem = RootHubIfFpAllocateWorkItem;
            fp->QueueWorkItem = RootHubIfFpQueueWorkItem;
            fp->FreeWorkItem = RootHubIfFpFreeWorkItem;
            fp->DeferIrpProcessing = RootHubIfFpDeferIrpProcessing;

            TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DEVICE,
                __FUNCTION__": %s USB_BUS_INTERFACE_HUB_FORWARD_PROGRESS_GUID supported size %d functions %d\n",
                fdoContext->FrontEndPath,
                fp->Size, 6);

            provideInterface = TRUE;
            InterlockedIncrement(&hubContext->FowardProgressInterfaceReferenceCount);
        }
        else
        {
            TraceEvents(TRACE_LEVEL_WARNING, TRACE_DEVICE,
                __FUNCTION__": %s USB_BUS_INTERFACE_HUB_FORWARD_PROGRESS_GUID unsupported size %d (%d) Version %d (%d)\n",
                fdoContext->FrontEndPath,
                IoStack->Parameters.QueryInterface.Size, 
                sizeof(USB_BUS_INTERFACE_HUB_FORWARD_PROGRESS),
                IoStack->Parameters.QueryInterface.Version, 0);
        }
    }
    else
    {
        TraceEvents(TRACE_LEVEL_WARNING, TRACE_DEVICE,
            __FUNCTION__": %s GUID unsupported: \n"
            " %08.8x %04.4x %04.4x %02.2x %02.2x %02.2x %02.2x %02.2x %02.2x %02.2x %02.2\n", 
            fdoContext->FrontEndPath,
            IoStack->Parameters.QueryInterface.InterfaceType->Data1,
            IoStack->Parameters.QueryInterface.InterfaceType->Data2,
            IoStack->Parameters.QueryInterface.InterfaceType->Data3,
            IoStack->Parameters.QueryInterface.InterfaceType->Data4[0],
            IoStack->Parameters.QueryInterface.InterfaceType->Data4[1],
            IoStack->Parameters.QueryInterface.InterfaceType->Data4[2],
            IoStack->Parameters.QueryInterface.InterfaceType->Data4[3],
            IoStack->Parameters.QueryInterface.InterfaceType->Data4[4],
            IoStack->Parameters.QueryInterface.InterfaceType->Data4[5],
            IoStack->Parameters.QueryInterface.InterfaceType->Data4[6],
            IoStack->Parameters.QueryInterface.InterfaceType->Data4[7]);

        TraceEvents(TRACE_LEVEL_WARNING, TRACE_DEVICE,
                __FUNCTION__": %s Interface Size %x Version %d\n",
                fdoContext->FrontEndPath,
                IoStack->Parameters.QueryInterface.Size,
                IoStack->Parameters.QueryInterface.Version);
    }
    //
    // test again and either complete or re-dispatch.
    //
    if (provideInterface)
    {
        Irp->IoStatus.Status = STATUS_SUCCESS;
        Status = STATUS_SUCCESS;
        Irp->IoStatus.Information = 0;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
    }
    else
    {
        IoSkipCurrentIrpStackLocation(Irp);
        Status = WdfDeviceWdmDispatchPreprocessedIrp(Device, Irp);
    }
    return Status;
}

NTSTATUS 
RootHubIfGetLocationString(
  _Inout_  PVOID Context,
  _Out_    PWCHAR *LocationStrings)
{
    PUSB_HUB_PDO_CONTEXT hubContext = DeviceGetHubPdoContext(Context);
    PUSB_FDO_CONTEXT fdoContext = DeviceGetFdoContext(hubContext->Parent);  
    // this is a multi-string so it needs two Nuls at the end.
    static WCHAR location[]=L"ROOT_HUB\0";

    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DEVICE,
                __FUNCTION__": %s device %p\n",
                fdoContext->FrontEndPath,
                hubContext->WdfDevice);

    PWCHAR string = (PWCHAR) ExAllocatePoolWithTag(PagedPool,
        sizeof(location), XVUF);

    if (string)
    {
        RtlCopyMemory(string, location, sizeof(location));
        *LocationStrings = string;
        return STATUS_SUCCESS;
    }

    return STATUS_INSUFFICIENT_RESOURCES;

}
