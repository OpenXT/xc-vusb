//
// Copyright (c) Citrix Systems, Inc., All rights reserved.
//
/// @file Queue.cpp USB FDO IO Queue implementation.
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
#include "usbioctl.h"
#include "UsbRequest.h"


//
/// This is the context that can be placed per queue
/// and would contain per queue information.
//
typedef struct _QUEUE_CONTEXT {

    ULONG PrivateDeviceData;  // just a placeholder

} QUEUE_CONTEXT, *PQUEUE_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(QUEUE_CONTEXT, QueueGetContext)

//
// Events from the IoQueue object
//
EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL FdoEvtIoDeviceControl;
EVT_WDF_IO_QUEUE_IO_INTERNAL_DEVICE_CONTROL UrbEvtIoInternalDeviceControl;
EVT_WDF_IO_QUEUE_IO_STOP FdoEvtIoStop;
EVT_WDF_WORKITEM  PassiveDrain;

PCHAR UsbControlCodeToString(
    IN ULONG ControlCode);

/**
 * @brief called by FdoEvtDeviceAdd() to intialize the queue objects for the
 * virtual usb controller.
 * The virtual usb controller does all of the URB and IOCTL processing for the
 * USB PDO.
 * Three queues are created: one for IOCTLs, one for URBs and one for URBs that 
 * could not be put on the ringbuffer.
 *
 * @param[in] Device handle to the WDFDEVICE object for the virtual usb host controller FDO.
 *
 * @returns NTSTATUS indicating success or failure.
 */
NTSTATUS
FdoQueueInitialize(
    _In_ WDFDEVICE Device)
{
    WDFQUEUE queue;
    NTSTATUS status;
    WDF_IO_QUEUE_CONFIG    queueConfig;
    PUSB_FDO_CONTEXT fdoContext = DeviceGetFdoContext(Device);    
    //
    // The default queue only processes IOCTL requests. These requests are targetd at the
    // HBA device and never go across the ringbuffer interface. It could also handle
    // internally dispatched URBs from the Hub driver.
    //
    // The UrbQueue handles InternalDeviceControl (URB) requests routed from the
    // child PDO.
    //
    // The RequestQueue holds requests that were blocked from being put onto the
    // ringbuffer interface due to resource exhaustion (no slots.) When this queue
    // is non-empty (not IDLE) the UrbQueue has to be in the stopped state. Typically
    // the RequestQueue will have either 0 or 1 requests queued, although it is possible to have
    // a depth > 1.
    //
    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(
         &queueConfig,
        WdfIoQueueDispatchParallel); // this could probably be serial.

    queueConfig.EvtIoDeviceControl = FdoEvtIoDeviceControl;
    queueConfig.EvtIoInternalDeviceControl = UrbEvtIoInternalDeviceControl;
    queueConfig.EvtIoStop = FdoEvtIoStop;

    status = WdfIoQueueCreate(
                 Device,
                 &queueConfig,
                 WDF_NO_OBJECT_ATTRIBUTES,
                 &queue);

    if( !NT_SUCCESS(status) ) 
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_QUEUE,
            __FUNCTION__": %s Default queue WdfIoQueueCreate failed %x", 
            fdoContext->FrontEndPath,
            status);
        return status;
    }
    //
    // The child URB queue
    //    
    WDF_IO_QUEUE_CONFIG_INIT(
        &queueConfig,
        WdfIoQueueDispatchParallel);

    queueConfig.EvtIoInternalDeviceControl = UrbEvtIoInternalDeviceControl;
    queueConfig.EvtIoStop = FdoEvtIoStop;

    status = WdfIoQueueCreate(Device,
        &queueConfig,
        WDF_NO_OBJECT_ATTRIBUTES,
        &fdoContext->UrbQueue);

    if( !NT_SUCCESS(status) ) 
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_QUEUE,
            __FUNCTION__": %s URB queue WdfIoQueueCreate failed %x", 
            fdoContext->FrontEndPath,
            status);
        return status;
    }
    //
    // Now configure the request queue as a manual dispatch queue used to handle hardware busy states.
    //
    WDF_IO_QUEUE_CONFIG_INIT(
        &queueConfig,
        WdfIoQueueDispatchManual);

    status = WdfIoQueueCreate(Device,
        &queueConfig,
        WDF_NO_OBJECT_ATTRIBUTES,
        &fdoContext->RequestQueue);

    if( !NT_SUCCESS(status) ) 
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_QUEUE, 
            __FUNCTION__ ": %s Manual queue WdfIoQueueCreate failed %x", 
            fdoContext->FrontEndPath,
            status);
    }
    return status;
}

/**
 * @brief Process an IOCTL_USB_GET_ROOT_HUB_NAME.
 * ** Must complete the Request **
 * The IOCTL_USB_GET_ROOT_HUB_NAME I/O control request is used with the 
 * USB_ROOT_HUB_NAME structure to retrieve the symbolic link name of the root hub.
 * On output, the AssociatedIrp.SystemBuffer member points to a USB_ROOT_HUB_NAME structure 
 * that contains the symbolic link name of the root hub. The leading "\xxx\ " text 
 * is not included in the retrieved string.
 *
 * @param[in] fdoContext. The context object for the device.
 * @param[in] Request. The handle to the IO Request.
 * @param[in] OutputBufferLength. The length in bytes of the output buffer.
 *
 */
VOID
ProcessRootHubNameRequest(
    IN PUSB_FDO_CONTEXT fdoContext,
    IN WDFREQUEST Request,
    IN size_t OutputBufferLength)
{
    UNICODE_STRING hub;
    ULONG length = (ULONG) OutputBufferLength;
    ULONG lengthNeeded = 0;
    NTSTATUS Status = STATUS_SUCCESS;
    ULONG_PTR Information = 0;

    if (!fdoContext->hubsymlink)
    {
        Status = STATUS_INVALID_DEVICE_REQUEST;
    }
    else
    {
        WdfStringGetUnicodeString(fdoContext->hubsymlink, &hub);
        //
        // delete the \??\ prefix from the returned string.
        //
        const ULONG prefixLength = 4 * sizeof(WCHAR);
        //
        // allow for a null at the end
        //
        lengthNeeded = hub.Length - prefixLength + sizeof(ULONG)+sizeof(WCHAR);

        if (hub.Length < prefixLength)
        {
            Status = STATUS_INVALID_DEVICE_REQUEST;
        }
        else
        {
            PUSB_ROOT_HUB_NAME name;

            Status = WdfRequestRetrieveOutputBuffer(Request,
                sizeof(USB_ROOT_HUB_NAME),
                (PVOID *)&name,
                NULL);

            if (!NT_SUCCESS(Status))
            {
                Status = STATUS_BUFFER_TOO_SMALL;
            }
            else
            {
                name->ActualLength = lengthNeeded;
                if (length >= lengthNeeded)
                {
                    Status = RtlStringCbCopyW(
                        name->RootHubName,
                        lengthNeeded,
                        &hub.Buffer[4]);
                    Information = lengthNeeded;
                }
                else
                {
                    name->RootHubName[0] = 0;
                    Information = sizeof(USB_ROOT_HUB_NAME);
                }
            }
        }
    }

    WdfRequestCompleteWithInformation(Request, Status, Information);

    TraceEvents(TRACE_LEVEL_WARNING, TRACE_QUEUE,
        __FUNCTION__": request completed with status %x size %d size needed %d output buffer length %d\n",
        Status,
        Information,
        lengthNeeded,
        length);
}

/**
 * @brief Process an IOCTL_INTERNAL_USB_GET_CONTROLLER_NAME.
 * ** Must complete the Request **
 * The IOCTL_INTERNAL_USB_GET_CONTROLLER_NAME I/O request queries the bus driver 
 * for the device name of the USB host controller. Confusingly, the data structure
 * used for this query is named "USB_HUB_NAME", rather than e.g. USB_HCD_NAME.
 * Upon successful completion, the HubName member of USB_HUB_NAME contains the name of the 
 * controller and the ActualLength member indicates the length of the controller name string. 
 * Note that ActualLength does not indicate the size of the entire USB_HUB_NAME structure. 
 * If the buffer supplied in Parameters.Others.Argument1 is not large enough to hold the string, 
 * the HubName value might show a truncated string.
 *
 * @param[in] fdoContext. The context object for the device.
 * @param[in] Request. The handle to the IO Request.
 * @param[in] OutputBufferLength. The length in bytes of the output buffer.
 *
 */
_Requires_lock_held_(fdoContext->WdfDevice)
VOID
ProcessControllerNameRequest(
    IN PUSB_FDO_CONTEXT fdoContext,
    IN WDFREQUEST Request,
    IN size_t OutputBufferLength)
{
    UNICODE_STRING hcd;
    WdfStringGetUnicodeString(fdoContext->hcdsymlink, &hcd);
    ULONG length = (ULONG) OutputBufferLength;
    NTSTATUS Status = STATUS_SUCCESS;
    ULONG_PTR Information = 0;    
    ULONG lengthNeeded = hcd.Length + sizeof(WCHAR);

    // note that WdfRequestRetrieveOutputBuffer enforces the minimum size constraints.
    PUSB_HUB_NAME name;
    Status = WdfRequestRetrieveOutputBuffer(Request,
        sizeof(USB_HUB_NAME),
        (PVOID *) &name,
        NULL);

    if (NT_SUCCESS(Status))
    {
        //
        // string must be null terminated.
        // copy as much as will fit in the input struct, but null terminate it.
        //
        ULONG copylength = (length - sizeof(ULONG));
        name->HubName[0] = 0; // null terminate always.
        //
        // returned length includes the struct itself and 
        // the full length of the string.
        //
        name->ActualLength = lengthNeeded + sizeof(ULONG);
        if (copylength > lengthNeeded)
        {
            copylength = lengthNeeded;
        }
        if (copylength) 
        {
            //
            // This request does not strip the prefix, unlike its cousin,
            // ProcessDriverKeyNameRequest(). Dept of grrr.....
            //
            Status = RtlStringCbCopyW(
                name->HubName,
                copylength,
                hcd.Buffer);
        }
        if (Status == STATUS_BUFFER_OVERFLOW)
        {
            Status = STATUS_SUCCESS;
        }
        Information = length;
    }

    TraceEvents(TRACE_LEVEL_WARNING, TRACE_QUEUE,
        __FUNCTION__": request completed with status %x size %d size needed %d output buffer length %d\n",
        Status,
        Information,
        lengthNeeded,
        length);

    RequestGetRequestContext(Request)->RequestCompleted = 1;  
    ReleaseFdoLock(fdoContext);
    WdfRequestCompleteWithInformation(Request, Status, Information);
    AcquireFdoLock(fdoContext);
}

/**
 * @brief Process an IOCTL_GET_HCD_DRIVERKEY_NAME.
 * ** Must complete the Request **
 * The IOCTL_GET_HCD_DRIVERKEY_NAME I/O control request retrieves the driver key name 
 * in the registry for a USB host controller driver.
 * The data structure used for this query is USB_HCD_DRIVERKEY_NAME.
 *
 * @param[in] fdoContext. The context object for the device.
 * @param[in] Request. The handle to the IO Request.
 * @param[in] OutputBufferLength. The length in bytes of the output buffer.
 *
 */
VOID
ProcessDriverKeyNameRequest(
    IN PUSB_FDO_CONTEXT fdoContext,
    IN WDFREQUEST Request,
    IN size_t OutputBufferLength)
{
    NTSTATUS Status = STATUS_UNSUCCESSFUL;
    ULONG length = (ULONG) OutputBufferLength;
    ULONG lengthNeeded = 0;
    ULONG_PTR Information = 0;

    PUSB_HCD_DRIVERKEY_NAME name;
    Status = WdfRequestRetrieveOutputBuffer(Request,
            sizeof(USB_HCD_DRIVERKEY_NAME),
            (PVOID *) &name,
            NULL);

    if (!NT_SUCCESS(Status))
    {
        Status = STATUS_BUFFER_TOO_SMALL;
    }
    else
    {
        ULONG stringLength = length - sizeof(ULONG);
        Information = sizeof(USB_HCD_DRIVERKEY_NAME);

        Status = WdfDeviceQueryProperty(
            fdoContext->WdfDevice,
            DevicePropertyDriverKeyName,
            stringLength,
            name->DriverKeyName,
            &lengthNeeded);

        if (Status == STATUS_BUFFER_TOO_SMALL)
        {
            lengthNeeded += sizeof(ULONG);
            name->ActualLength = lengthNeeded;
            name->DriverKeyName[0] = 0;
            Information = sizeof(USB_HCD_DRIVERKEY_NAME);
            Status = STATUS_SUCCESS; // ??
        }
        else if (NT_SUCCESS(Status))
        {
            lengthNeeded += sizeof(ULONG);
            name->ActualLength = lengthNeeded;
            Information = name->ActualLength;
        }
    }
    // C6102 Using lengthNeeded from failed function call at line ... 
    // claims that lengthNeeded is uninitialized. It clearly is initialized.
   #pragma warning(suppress: 6102) 
    TraceEvents(TRACE_LEVEL_WARNING, TRACE_QUEUE,
        __FUNCTION__": request completed with status %x size %d size needed %d output buffer length %d\n",
        Status,
        Information,
        lengthNeeded,
        length);

    WdfRequestCompleteWithInformation(Request, Status, Information);
}

_Requires_lock_held_(fdoContext->WdfDevice)
VOID
ProcessCyclePort(
    IN PUSB_FDO_CONTEXT fdoContext,
    IN WDFREQUEST Request,
    IN size_t OutputBufferLength)
{
    TraceEvents(TRACE_LEVEL_WARNING, TRACE_URB,
        __FUNCTION__": %s for Device %p\n", 
        fdoContext->FrontEndPath,
        fdoContext->WdfDevice);

    UNREFERENCED_PARAMETER(OutputBufferLength);

    PutResetOrCycleUrbOnRing(
        fdoContext,
        Request,
        FALSE);
}

PWCHAR
AllocAndQueryPropertyString(
    IN WDFDEVICE Device,
    IN DEVICE_REGISTRY_PROPERTY  DeviceProperty,
    OUT PULONG ResultLength)
{
        PWCHAR buffer = NULL;
        *ResultLength = 0;
        NTSTATUS Status = WdfDeviceQueryProperty(
            Device,
            DeviceProperty,
            0,
            NULL,
            ResultLength);

        if (Status != STATUS_BUFFER_TOO_SMALL)
        {
            TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
                __FUNCTION__": device %p unexpected error %x from pre-alloc WdfDeviceQueryProperty\n",
                Device,
                Status);
            return NULL;
        }
        // C6102		Using '*ResultLength' from failed function call.
#pragma warning(suppress: 6102)
        buffer = (PWCHAR) ExAllocatePoolWithTag(PagedPool, *ResultLength, '2UVX');
        if (!buffer)
        {
            return NULL;
        }

        Status = WdfDeviceQueryProperty(
            Device,
            DeviceProperty,
            *ResultLength,
            buffer,
            ResultLength);

        if (!NT_SUCCESS(Status))
        {
            ExFreePool(buffer);
            
            TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
                __FUNCTION__": device %p unexpected error %x from WdfDeviceQueryProperty\n",
                Device,
                Status);

            return NULL;
        }

        return buffer;
}


/**
 * @brief Process an IOCTL_INTERNAL_USB_GET_DEVICE_CONFIG_INFO.
 * ** Must complete the Request **
 * The IOCTL_INTERNAL_USB_GET_DEVICE_CONFIG_INFO  I/O control request contains
 * a HUB_DEVICE_CONFIG_INFO structure.
 * Upon successful completion, the HardwareIds, CompatibleIds, 
 * DeviceDescription USB_ID_STRING structures contained in the HUB_DEVICE_CONFIG_INFO 
 * structure points to string buffers allocated by the hub driver.
 *
 * ** Input Parameters ** for this request: Parameters.Others.Argument1 points to a HUB_DEVICE_CONFIG_INFO 
 * structure to receive the device configuration information.
 *
 * ** Output Parameters ** for this request: Parameters.Others.Argument1 points to a HUB_DEVICE_CONFIG_INFO 
 * structure containing the device configuration information.
 *
 * ** This function allocates paged pool for string buffers that must be freed
 * by the caller. **
 *
 * @param[in] fdoContext. The context object for the device.
 * @param[in] Request. The handle to the IO Request.
 * @param[in] OutputBufferLength. The length in bytes of the output buffer.
 *
 */
VOID
ProcessGetDeviceConfigInfo(
    IN PUSB_FDO_CONTEXT fdoContext,
    IN WDFREQUEST Request,
    IN size_t OutputBufferLength)
{
    UNREFERENCED_PARAMETER(OutputBufferLength);

    NTSTATUS Status = STATUS_SUCCESS;
    PHUB_DEVICE_CONFIG_INFO hubConfig;
    WDF_REQUEST_PARAMETERS parameters;

    WDF_REQUEST_PARAMETERS_INIT(&parameters);
    WdfRequestGetParameters(Request, &parameters);

    TRY
    {
        //
        // this is pretty horrible. Could put a real _try _except around the
        // naked pointer access.
        //
        hubConfig = (PHUB_DEVICE_CONFIG_INFO) parameters.Parameters.Others.Arg1;
        if (!hubConfig)
        {
            Status = STATUS_UNSUCCESSFUL;
            TraceEvents(TRACE_LEVEL_ERROR, TRACE_URB,
                __FUNCTION__": %s hubConfig NULL\n",
                fdoContext->FrontEndPath);
            LEAVE;
        }

        if (hubConfig->Length < sizeof(HUB_DEVICE_CONFIG_INFO))
        {
            Status = STATUS_UNSUCCESSFUL;
            TraceEvents(TRACE_LEVEL_ERROR, TRACE_URB,
                __FUNCTION__": %s hubConfig->Length %d < %d\n",
                fdoContext->FrontEndPath,
                hubConfig->Length,
                (ULONG) sizeof(HUB_DEVICE_CONFIG_INFO));
            LEAVE;
        }
        hubConfig->Version = 1;
        hubConfig->HubFlags.ul = 0;
        hubConfig->HubFlags.HubIsHighSpeed = TRUE;
        hubConfig->HubFlags.HubIsHighSpeedCapable = TRUE;
        hubConfig->HubFlags.HubIsRoot = TRUE;
        //
        // hardware ids
        //
        hubConfig->HardwareIds.LanguageId = 0x0409;
        hubConfig->HardwareIds.Buffer = AllocAndQueryPropertyString(
            fdoContext->WdfDevice,
            DevicePropertyHardwareID,
            &hubConfig->HardwareIds.LengthInBytes);

        if (!hubConfig->HardwareIds.Buffer)
        {
            TraceEvents(TRACE_LEVEL_ERROR, TRACE_URB,
                __FUNCTION__": %s HardwareIds.Buffer NULL\n",
                fdoContext->FrontEndPath);
            Status = STATUS_UNSUCCESSFUL;
            LEAVE;
        }        
        //
        // compat ids
        //
        hubConfig->CompatibleIds.LanguageId = 0x0409;
        hubConfig->CompatibleIds.Buffer = AllocAndQueryPropertyString(
            fdoContext->WdfDevice,
            DevicePropertyCompatibleIDs,
            &hubConfig->CompatibleIds.LengthInBytes);

        if (!hubConfig->CompatibleIds.Buffer)
        {
            TraceEvents(TRACE_LEVEL_ERROR, TRACE_URB,
                __FUNCTION__": %s CompatibleIds.Buffer NULL\n",
                fdoContext->FrontEndPath);
            ExFreePool(hubConfig->HardwareIds.Buffer);
            Status = STATUS_UNSUCCESSFUL;
            LEAVE;
        }
        //
        // DeviceDescription is optional.
        //
        hubConfig->DeviceDescription.Buffer = NULL;
        hubConfig->DeviceDescription.LengthInBytes = 0;
    }
    FINALLY
    {
        TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_QUEUE,
            __FUNCTION__": %s request completed with status %x\n",
            fdoContext->FrontEndPath,
            Status);
        //
        // acquiring and releasing the lock here is pointless
        // but is being done for consistency.
        //
        AcquireFdoLock(fdoContext);
        RequestGetRequestContext(Request)->RequestCompleted = 1;
        ReleaseFdoLock(fdoContext);
        WdfRequestCompleteWithInformation(Request, Status, 0);
    }
}


/**
 * @brief Process an USBUSER_GET_POWER_STATE_MAP.
 * ** Must complete the Request **
 * ** Processed on dispatch side and does not use Request Context **
 *
 * @param[in] fdoContext. The context object for the device.
 * @param[in] Request. The handle to the IO Request.
 * @param[in] OutputBufferLength. The length in bytes of the output buffer.
 *
 */
VOID
ProcessUsbPowerStateMap(
    IN PUSB_FDO_CONTEXT fdoContext,
    IN WDFREQUEST Request,
    IN size_t OutputBufferLength)
{
    UNREFERENCED_PARAMETER(fdoContext);
    UNREFERENCED_PARAMETER(OutputBufferLength);
    PUSBUSER_POWER_INFO_REQUEST usbPower = NULL;
    ULONG_PTR Information = 0;

    NTSTATUS Status = WdfRequestRetrieveOutputBuffer(Request,
            sizeof(USBUSER_POWER_INFO_REQUEST),
            (PVOID *) &usbPower,
            NULL);

    if (NT_SUCCESS(Status))
    {
        usbPower->PowerInformation.HcDeviceWake = WdmUsbPowerDeviceUnspecified;
        usbPower->PowerInformation.HcSystemWake = WdmUsbPowerDeviceUnspecified;
        usbPower->PowerInformation.RhDeviceWake = WdmUsbPowerDeviceUnspecified;
        usbPower->PowerInformation.RhSystemWake = WdmUsbPowerDeviceUnspecified;
        usbPower->PowerInformation.LastSystemSleepState = WdmUsbPowerSystemUnspecified;

        switch (usbPower->PowerInformation.SystemState)
        {
        case WdmUsbPowerSystemWorking:
            usbPower->PowerInformation.HcDevicePowerState = WdmUsbPowerDeviceD0;
            usbPower->PowerInformation.RhDevicePowerState = WdmUsbPowerDeviceD0;
            usbPower->PowerInformation.IsPowered = TRUE;
            usbPower->PowerInformation.CanWakeup = FALSE;
            break;
        case WdmUsbPowerSystemSleeping1:
        case WdmUsbPowerSystemSleeping2:
        case WdmUsbPowerSystemSleeping3:
        case WdmUsbPowerSystemHibernate:
        case WdmUsbPowerSystemShutdown:
        default:
            usbPower->PowerInformation.HcDevicePowerState = WdmUsbPowerDeviceD3;
            usbPower->PowerInformation.RhDevicePowerState = WdmUsbPowerDeviceD3;
            usbPower->PowerInformation.IsPowered = FALSE;
            usbPower->PowerInformation.CanWakeup = FALSE;
            break;
        }
        usbPower->Header.UsbUserStatusCode = UsbUserSuccess;
        usbPower->Header.ActualBufferLength = usbPower->Header.RequestBufferLength;
        Information = usbPower->Header.ActualBufferLength;
    }
    
    TraceEvents(TRACE_LEVEL_WARNING, TRACE_QUEUE,
        __FUNCTION__": request completed with status %x size %d\n",
        Status,
        Information); 

    WdfRequestCompleteWithInformation(Request, Status, Information);

}

/**
 * @brief Process an USBUSER_GET_POWER_STATE_MAP.
 * ** Must complete the Request **
 * ** Processed on dispatch side and does not use Request Context **
 *
 * @param[in] fdoContext. The context object for the device.
 * @param[in] Request. The handle to the IO Request.
 * @param[in] OutputBufferLength. The length in bytes of the output buffer.
 *
 */
VOID
ProcessUsbControllerInfo0(
    IN PUSB_FDO_CONTEXT fdoContext,
    IN WDFREQUEST Request,
    IN size_t OutputBufferLength)
{
    UNREFERENCED_PARAMETER(fdoContext);
    UNREFERENCED_PARAMETER(OutputBufferLength);
    PUSBUSER_CONTROLLER_INFO_0 usbController = NULL;
    ULONG_PTR Information = 0;

    NTSTATUS Status = WdfRequestRetrieveOutputBuffer(Request,
            sizeof(USBUSER_CONTROLLER_INFO_0),
            (PVOID *) &usbController,
            NULL);

    if (NT_SUCCESS(Status))
    {
        usbController->Info0.PciVendorId = 0x5853;
        usbController->Info0.PciDeviceId = 0x0001;
        usbController->Info0.PciRevision = 0x01;
        usbController->Info0.NumberOfRootPorts = 1;
        usbController->Info0.ControllerFlavor = EHCI_Generic;
        usbController->Info0.HcFeatureFlags = 0; // @TODO support USB_HC_FEATURE_FLAG_SEL_SUSPEND
        usbController->Header.UsbUserStatusCode = UsbUserSuccess;
        usbController->Header.ActualBufferLength = usbController->Header.RequestBufferLength;
        Information = usbController->Header.ActualBufferLength;
    }
    
    TraceEvents(TRACE_LEVEL_WARNING, TRACE_QUEUE,
        __FUNCTION__": request completed with status %x size %d\n",
        Status,
        Information); 

    WdfRequestCompleteWithInformation(Request, Status, Information);

}

/**
 * @brief Process an IOCTL_USB_USER_REQUEST.
 * ** Must guarantee the Request is completed. **
 * IOCTL_USB_USER_REQUEST I/O provides a set of user mode apis for communicating
 * directly with a host controller and its related root hub.
 * ** Processed on dispatch side and does not use Request Context **
 *
 * @TODO many subtypes are stubbed out. Some of these requests have direct analogs with 
 * other IOCTLs.
 *
 * @param[in] fdoContext. The context object for the device.
 * @param[in] Request. The handle to the IO Request.
 * @param[in] OutputBufferLength. The length in bytes of the output buffer.
 *
 */
VOID
ProcessUsbUserRequest(
    IN PUSB_FDO_CONTEXT fdoContext,
    IN WDFREQUEST Request,
    IN size_t OutputBufferLength)
{
    NTSTATUS Status = STATUS_SUCCESS;
    ULONG_PTR Information = 0;  
    UNREFERENCED_PARAMETER(OutputBufferLength);

    // note that WdfRequestRetrieveOutputBuffer enforces the minimum size constraints.
    PUSBUSER_REQUEST_HEADER userRequest;
    Status = WdfRequestRetrieveOutputBuffer(Request,
        sizeof(USBUSER_REQUEST_HEADER),
        (PVOID *) &userRequest,
        NULL);

    PCHAR userRequestString = "Unknown IOCTL_USB_USER_REQUEST type";
    ULONG UsbUserRequest = USBUSER_INVALID_REQUEST;

    if (NT_SUCCESS(Status))
    {
        userRequest->UsbUserStatusCode = UsbUserNotSupported;
        UsbUserRequest = userRequest->UsbUserRequest;
        Information = sizeof(USBUSER_REQUEST_HEADER);

        switch (userRequest->UsbUserRequest)
        {
        case USBUSER_GET_CONTROLLER_INFO_0:
            userRequestString = "USBUSER_GET_CONTROLLER_INFO_0";
            ProcessUsbControllerInfo0(fdoContext, Request, OutputBufferLength);
            Request = NULL;
            break;
        case USBUSER_GET_CONTROLLER_DRIVER_KEY:
            userRequestString = "USBUSER_GET_CONTROLLER_DRIVER_KEY";
            break;
        case USBUSER_PASS_THRU:
            userRequestString = "USBUSER_PASS_THRU";
            break;
        case USBUSER_GET_POWER_STATE_MAP:
            userRequestString = "USBUSER_GET_POWER_STATE_MAP";
            // required for USBVIEW
            ProcessUsbPowerStateMap(fdoContext, Request, OutputBufferLength);
            Request = NULL;
            break;
        case USBUSER_GET_BANDWIDTH_INFORMATION:
            userRequestString = "USBUSER_GET_BANDWIDTH_INFORMATION";
            break;
        case USBUSER_GET_BUS_STATISTICS_0:
            userRequestString = "USBUSER_GET_BUS_STATISTICS_0";
            break;
        case USBUSER_GET_ROOTHUB_SYMBOLIC_NAME:
            userRequestString = "USBUSER_GET_ROOTHUB_SYMBOLIC_NAME";
            break;
        case USBUSER_GET_USB_DRIVER_VERSION:
            userRequestString = "USBUSER_GET_USB_DRIVER_VERSION";
            break;
        case USBUSER_GET_USB2_HW_VERSION:
            userRequestString = "USBUSER_GET_USB2_HW_VERSION";
            break;
        case USBUSER_USB_REFRESH_HCT_REG:
            userRequestString = "USBUSER_USB_REFRESH_HCT_REG";
            break;
        case USBUSER_OP_SEND_ONE_PACKET:
            userRequestString = "USBUSER_OP_SEND_ONE_PACKET";
            break;
        case USBUSER_OP_RAW_RESET_PORT:
            userRequestString = "USBUSER_OP_RAW_RESET_PORT";
            break;
        case USBUSER_OP_OPEN_RAW_DEVICE:
            userRequestString = "USBUSER_OP_OPEN_RAW_DEVICE";
            break;
        case USBUSER_OP_CLOSE_RAW_DEVICE:
            userRequestString = "USBUSER_OP_CLOSE_RAW_DEVICE";
            break;
        case USBUSER_OP_SEND_RAW_COMMAND:
            userRequestString = "USBUSER_OP_SEND_RAW_COMMAND";
            break;
        case USBUSER_SET_ROOTPORT_FEATURE:
            userRequestString = "USBUSER_SET_ROOTPORT_FEATURE";
            break;
        case USBUSER_CLEAR_ROOTPORT_FEATURE:
            userRequestString = "USBUSER_CLEAR_ROOTPORT_FEATURE";
            break;
        case USBUSER_GET_ROOTPORT_STATUS:
            userRequestString = "USBUSER_GET_ROOTPORT_STATUS";
            break;
        case USBUSER_INVALID_REQUEST:
            userRequestString = "USBUSER_INVALID_REQUEST";
            userRequest->UsbUserStatusCode = UsbUserInvalidRequestCode;
            break;
        case USBUSER_OP_MASK_DEVONLY_API:
            userRequestString = "USBUSER_OP_MASK_DEVONLY_API";
            break;
        case USBUSER_OP_MASK_HCTEST_API:
            userRequestString = "USBUSER_OP_MASK_HCTEST_API";
            break;

        }
    }
    
    if (Request)
    {
        TraceEvents(TRACE_LEVEL_WARNING, TRACE_QUEUE,
            __FUNCTION__": %s request %s (%x) completed with status %x size %d \n",
            fdoContext->FrontEndPath,
            userRequestString,
            UsbUserRequest,
            Status,
            Information);

        WdfRequestCompleteWithInformation(Request, Status, Information);
    }
}


/**
 * @brief process IOCTL requests.
 * IOCTLs targeted at the usb controller are processed here.
 * @Note these requests are all processed on the dispatch side,
 * are not cancelable and do not use the request context.
 *
 * @param[in] Queue the IOCTL queue.
 * @param[in] Request the IOCTL request handle.
 * @param[in] OutputBufferLength size of the IOCTL output buffer.
 * @param[in] InputBufferLength size of the IOCTL input buffer
 * @param[in] IoControlCode the IOCTL function code.
 */
VOID
FdoEvtIoDeviceControl(
    _In_ WDFQUEUE Queue,
    _In_ WDFREQUEST Request,
    _In_ size_t OutputBufferLength,
    _In_ size_t InputBufferLength,
    _In_ ULONG IoControlCode)
{
    PUSB_FDO_CONTEXT fdoContext = DeviceGetFdoContext(WdfIoQueueGetDevice(Queue));

    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_QUEUE,
        __FUNCTION__": %s Queue %p, Request %p IoControlCode %d Function %d OUT\n",
        fdoContext->FrontEndPath,
        Queue,
        Request,
        IoControlCode,
        IoGetFunctionCodeFromCtlCode(IoControlCode));

    UNREFERENCED_PARAMETER(OutputBufferLength);
    UNREFERENCED_PARAMETER(InputBufferLength);

    NTSTATUS Status = STATUS_UNSUCCESSFUL;
    if (fdoContext->XenConfigured)
    {

        switch (IoControlCode)
        {
        case IOCTL_USB_DIAGNOSTIC_MODE_ON: // HCD 256
        case IOCTL_USB_DIAGNOSTIC_MODE_OFF:  // HCD 257
            Status = STATUS_SUCCESS;
            break;

        case IOCTL_USB_GET_ROOT_HUB_NAME:
            ProcessRootHubNameRequest(fdoContext,
                Request,
                OutputBufferLength);
            Request = NULL; // handled
            break;

        case IOCTL_GET_HCD_DRIVERKEY_NAME:
            ProcessDriverKeyNameRequest(fdoContext,
                Request,
                OutputBufferLength);
            Request = NULL; // handled
            break;

        case IOCTL_USB_USER_REQUEST:
            ProcessUsbUserRequest(fdoContext,
                Request,
                OutputBufferLength);
            Request = NULL;
            break;

        case IOCTL_USB_HCD_GET_STATS_1: //255
        case IOCTL_USB_HCD_GET_STATS_2: // 266
        case IOCTL_USB_HCD_DISABLE_PORT: //268
        case IOCTL_USB_HCD_ENABLE_PORT: // 269
        default:

            TraceEvents(TRACE_LEVEL_WARNING, TRACE_QUEUE,
                __FUNCTION__": HCD IOCTL %x Function %d %s unsupported\n",
                IoControlCode,
                IoGetFunctionCodeFromCtlCode(IoControlCode),
                UsbIoctlToString(IoControlCode));
            break;
        };
    }

    if (Request)
    {
        WdfRequestComplete(Request, Status);
    }

    return;
}



/**
 * @brief process URB requests for the USB PDO device.
 * URBs are simply forwarded down the stack from USB PDO to ROOT HUB to
 * the virutal usb controller and then processed here.
 *
 * Also some non URB internal device control requests can be sent from the hub device
 * to the controller device, for example IOCTL_INTERNAL_USB_CYCLE_PORT.
 *
 * @param[in] Queue the URB queue.
 * @param[in] Request the URB request handle.
 * @param[in] OutputBufferLength unused.
 * @param[in] InputBufferLength unused.
 * @param[in] IoControlCode the IOCTL function code - muse be IOCTL_INTERNAL_USB_SUBMIT_URB.
 *
 */
VOID
UrbEvtIoInternalDeviceControl(
    _In_ WDFQUEUE Queue,
    _In_ WDFREQUEST Request,
    _In_ size_t OutputBufferLength,
    _In_ size_t InputBufferLength,
    _In_ ULONG IoControlCode)
{
    KIRQL dispatchIrql = KeGetCurrentIrql();
    NTSTATUS Status = STATUS_UNSUCCESSFUL;

    UNREFERENCED_PARAMETER(OutputBufferLength);
    UNREFERENCED_PARAMETER(InputBufferLength);
    
    TraceEvents(TRACE_LEVEL_VERBOSE, TRACE_QUEUE, 
        __FUNCTION__": Queue %p, Request %p IoControlCode %d FunctionCode %d\n", 
        Queue, 
        Request,  
        IoControlCode,
        IoGetFunctionCodeFromCtlCode(IoControlCode));

    PUSB_FDO_CONTEXT fdoContext = DeviceGetFdoContext(WdfIoQueueGetDevice(Queue));
    PFDO_REQUEST_CONTEXT requestContext = RequestGetRequestContext(Request);
    RtlZeroMemory(requestContext, sizeof(FDO_REQUEST_CONTEXT));

    WDF_REQUEST_PARAMETERS  Parameters;
    WDF_REQUEST_PARAMETERS_INIT(&Parameters);
    WdfRequestGetParameters(Request, &Parameters);

    AcquireFdoLock(fdoContext);
    TRY
    {
        if ((!fdoContext->XenConfigured) ||
            (fdoContext->DeviceUnplugged))
        {   
            if ((gVistaOrLater) ||
                (IoControlCode != IOCTL_INTERNAL_USB_SUBMIT_URB))
            {
                Status = STATUS_DEVICE_DOES_NOT_EXIST;
            }
            else
            {
                PURB Urb = URB_FROM_REQUEST(Request);
                if (Urb->UrbHeader.Function == URB_FUNCTION_ISOCH_TRANSFER)
                {
                    Status = STATUS_SUCCESS;
                    Urb->UrbHeader.Status = USBD_STATUS_DEVICE_GONE;
                }
                else
                {
                    Status = STATUS_DEVICE_DOES_NOT_EXIST;
                }
            }
            LEAVE;
        }

        if (fdoContext->ConfigBusy)
        {
            //
            // This queue has to be stopped and the request queued internally.
            //
            RequeueRequest(fdoContext, Request);
            Request = NULL; // consumed!
            LEAVE;
        }

        switch (IoControlCode)
        {
        case IOCTL_INTERNAL_USB_SUBMIT_URB:
            {
                if (Queue != fdoContext->UrbQueue)
                {
                    // URBs have to go through the hub device.
                    Status = STATUS_INVALID_DEVICE_REQUEST;
                    break;
                }

                PURB Urb = URB_FROM_REQUEST(Request);
                if (Urb->UrbHeader.UsbdDeviceHandle == 0)
                {
                    //
                    // this is targeted at the hub device.
                    //
                    Status = STATUS_INVALID_DEVICE_REQUEST;
                    break;
                }
                if (fdoContext->ResetInProgress)
                {
                    RequeueRequest(fdoContext, Request);
                    Request = NULL; // consumed!
                    LEAVE;
                }

                if ((Urb->UrbHeader.Function == URB_FUNCTION_SELECT_INTERFACE) &&
                    (dispatchIrql >= DISPATCH_LEVEL))
                {
                    XXX_TODO("SELECT_INTERFACE at >= DISPATCH_LEVEL");
                    //
                    // This queue has to be stopped. This request
                    // has to be requeued to a passive level queue.
                    // That queue has to submit the select interface request
                    // and then the completion of that request has to 
                    // restart this queue.
                    //
                    LEAVE;
                }
                SubmitUrb(fdoContext, Request, Urb);
                Request = NULL; // consumed!
            }
            break;

        case IOCTL_INTERNAL_USB_GET_PORT_STATUS:
            //
            // Need to support for error recovery.
            //
            {
                PULONG portStatus = (PULONG) Parameters.Parameters.Others.Arg1;
                if (portStatus)
                {
                    if (fdoContext->DeviceUnplugged)
                    {
                        *portStatus = USBD_PORT_ENABLED; // enabled but not connected.
                        TraceEvents(TRACE_LEVEL_WARNING, TRACE_URB,
                            __FUNCTION__": IOCTL_INTERNAL_USB_GET_PORT_STATUS returning enabled and not connected\n");
                    }
                    else
                    {
                        *portStatus = USBD_PORT_ENABLED|USBD_PORT_CONNECTED;

                        TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_URB,
                            __FUNCTION__": IOCTL_INTERNAL_USB_GET_PORT_STATUS returning enabled and connected\n");
                    }
                    Status = STATUS_SUCCESS;
                }
                else
                {

                    TraceEvents(TRACE_LEVEL_WARNING, TRACE_URB,
                        __FUNCTION__": IOCTL_INTERNAL_USB_GET_PORT_STATUS invalid request\n");
                    Status = STATUS_INVALID_PARAMETER;
                }
            }
            break;

        case IOCTL_INTERNAL_USB_RESET_PORT:
            if (fdoContext->ResetInProgress)
            {
                TraceEvents(TRACE_LEVEL_WARNING, TRACE_URB,
                    __FUNCTION__": Reset already in progress, ignoring this request\n");
                Status = STATUS_SUCCESS; // ??
            }
            else
            {
                ProcessResetRequest(fdoContext, Request);
                Request = NULL; // owned by completion side.
            }
            break;

        case IOCTL_INTERNAL_USB_GET_BUS_INFO: // obsolete - support USB_BUSIFFN_QUERY_BUS_INFORMATION instead
            {
                PUSB_BUS_NOTIFICATION busNotification = 
                    (PUSB_BUS_NOTIFICATION) Parameters.Parameters.Others.Arg1;

                TraceEvents(TRACE_LEVEL_WARNING, TRACE_URB,
                    __FUNCTION__": IOCTL_INTERNAL_USB_GET_BUS_INFO for device %p\n",
                    fdoContext->WdfDevice);
                //
                // there appears to be no way to actually validate 
                // this structure.
                //
                busNotification->NotificationType = AcquireBusInfo;
                busNotification->TotalBandwidth = 12000; // ???
                busNotification->ConsumedBandwidth = 0; // LIAR!
                busNotification->ControllerNameLength = 0; // or support IOCTL_INTERNAL_USB_GET_CONTROLLER_NAME
                Status = STATUS_SUCCESS;
            }
            break;

        case IOCTL_INTERNAL_USB_SUBMIT_IDLE_NOTIFICATION:
            {
                //
                // mock up an idle callback
                //
                ProcessIdleNotificationRequest(fdoContext, Request);
                Request = NULL; // consumed!
            }
            break;

        case IOCTL_INTERNAL_USB_GET_PARENT_HUB_INFO:
            Status = STATUS_NOT_IMPLEMENTED;
            break;

        case IOCTL_INTERNAL_USB_GET_CONTROLLER_NAME:
            ProcessControllerNameRequest(
                fdoContext,
                Request,
                OutputBufferLength);
            Request = NULL; // consumed by ProcessControllerNameRequest.
            break;

        case IOCTL_INTERNAL_USB_CYCLE_PORT:
            ProcessCyclePort(
                fdoContext,
                Request,OutputBufferLength);
            Request = NULL; // consumed by ProcessCyclePort.
            break;

        case IOCTL_INTERNAL_USB_GET_DEVICE_CONFIG_INFO:
            
            ReleaseFdoLock(fdoContext);
            if (!HTSASSERT(KeGetCurrentIrql() == PASSIVE_LEVEL))
            {
                Status = STATUS_INVALID_DEVICE_REQUEST;
                break;
            }
            ProcessGetDeviceConfigInfo(
                fdoContext,
                Request,
                OutputBufferLength);

            AcquireFdoLock(fdoContext);
            Request = NULL;  // consumed by ProcessGetDeviceConfigInfo.
            break;

        case IOCTL_INTERNAL_USB_GET_TOPOLOGY_ADDRESS:
            // this should be processed by the hub device.
            Status = STATUS_NOT_IMPLEMENTED;
            break;

        case IOCTL_INTERNAL_USB_NOTIFY_IDLE_READY:
        case IOCTL_INTERNAL_USB_REQ_GLOBAL_SUSPEND:
        case IOCTL_INTERNAL_USB_REQ_GLOBAL_RESUME:
        case IOCTL_INTERNAL_USB_RECORD_FAILURE:
        case IOCTL_INTERNAL_USB_GET_DEVICE_HANDLE_EX:
        case IOCTL_INTERNAL_USB_GET_TT_DEVICE_HANDLE:
        case IOCTL_INTERNAL_USB_ENABLE_PORT:
        case IOCTL_INTERNAL_USB_GET_BUSGUID_INFO:
        case IOCTL_INTERNAL_USB_GET_DEVICE_HANDLE:
        default:
            //
            // other requests?
            //
            Status = STATUS_NOT_IMPLEMENTED;
        }

        if (Status == STATUS_NOT_IMPLEMENTED)
        {
            TraceEvents(TRACE_LEVEL_WARNING, TRACE_URB,
                __FUNCTION__": INTERNAL IOCTL %x %s (%d) unsupported\n",
                IoControlCode,
                UsbControlCodeToString(IoControlCode),
                IoGetFunctionCodeFromCtlCode(IoControlCode));
        }
    }

    FINALLY
    {
        if (Request)
        {
            requestContext->RequestCompleted = 1;
            ReleaseFdoLock(fdoContext);
            WdfRequestComplete(Request, Status);
        }
        else
        {
            ReleaseFdoLock(fdoContext);
        }
    }
    return;
}

/**
 * @brief completes, requeues, or suspends processing of a specified request because 
 * the request's I/O queue is being stopped.
 * These requests are the ones that are "on hardware" so Stop cannot complete them.
 * 
 * @param[in] Queue A handle to the framework queue object that is associated with the I/O request.
 * @param[in] Request A handle to a framework request object. 
 * @param[in] ActionFlags A bitwise OR of one or more WDF_REQUEST_STOP_ACTION_FLAGS-typed flags 
 *   that identify the reason that the callback function is being called and whether the request is cancelable
 */
VOID
FdoEvtIoStop(
    _In_ WDFQUEUE Queue,
    _In_ WDFREQUEST Request,
    _In_ ULONG ActionFlags)
{
    TraceEvents(TRACE_LEVEL_INFORMATION, 
                TRACE_QUEUE, 
                __FUNCTION__": Queue %p, Request %p ActionFlags %x\n", 
                Queue, Request, ActionFlags);

    PUSB_FDO_CONTEXT fdoContext = DeviceGetFdoContext(WdfIoQueueGetDevice(Queue));
    PFDO_REQUEST_CONTEXT requestContext = RequestGetRequestContext(Request);

    AcquireFdoLock(fdoContext);
    //
    // We are assuming that the Request object has a valid Irp and
    // that WdfRequestUnmarkCancelable cannot fail. The docs claim
    // that if WdfRequestStopRequestCancelable is set then WdfRequestUnmarkCancelable
    // must be called.
    //
    if (ActionFlags & WdfRequestStopRequestCancelable)
    {
        NTSTATUS Status = WdfRequestUnmarkCancelable(Request);
        if (Status == STATUS_CANCELLED)
        {
            ReleaseFdoLock(fdoContext);
            return;
        }
        requestContext->CancelSet = 0;
    }
    WdfRequestStopAcknowledge(Request, FALSE);
    ReleaseFdoLock(fdoContext);

    return;
}
/**
 *  @brief Stops the URB dispatch Queue and puts the request on the queue.
 *  *Must be called with Lock held*.
 *  *Must Queue or Complete the Request*.
 *
 * @param[in] fdoContext A pointer to the USB_FDO_CONTEXT for the virtual usb controller device.
 * @param[in] Request A handle to the framework request object that needs to be requeued.
 *
 **/
_Requires_lock_held_(fdoContext->WdfDevice)
VOID
RequeueRequest(
    IN PUSB_FDO_CONTEXT fdoContext,
    IN WDFREQUEST Request)
{
    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_URB,
            __FUNCTION__": Device %p Request %p\n",
            fdoContext->WdfDevice,
            Request);

    // @TODO - this function should just not be called with the lock held.
    ReleaseFdoLock(fdoContext);
    //
    // This call will run the queue, and can process cancelled requests, which
    // will in turn end up trying to acquire the lock, recursively.
    //
    WdfIoQueueStop(fdoContext->UrbQueue, NULL, NULL);
    //
    // requeue the request to the RequestQueue, if didn't come from 
    // the requeust queue use WdfRequestForwardToIoQueue, otherwise
    // use WdfRequestRequeue, for reasons known only to the framework authors
    // a bugcheck will occur if forward's target queue is the same queue
    // the request started from.
    //
    NTSTATUS Status;
    if (WdfRequestGetIoQueue(Request) != fdoContext->RequestQueue)
    {
        Status = WdfRequestForwardToIoQueue(Request, fdoContext->RequestQueue);
    }
    else
    {
        Status = WdfRequestRequeue(Request);
    }
    AcquireFdoLock(fdoContext);
    //
    // unfortunately the requeue can fail. All the failures are malfunctions.
    //
    if (!NT_SUCCESS(Status))
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_URB,
            __FUNCTION__": %s Device %p Error %x returned from WdfRequestForwardToIoQueue\n",
            fdoContext->FrontEndPath,
            fdoContext->WdfDevice,
            Status);
        ReleaseFdoLock(fdoContext);
        WdfRequestComplete(Request, Status);
        AcquireFdoLock(fdoContext);
    }
    else
    {
        // could just acquire the lock to inc the count. 
        fdoContext->RequeuedCount++;
    }
    return;
}

/**
 * @brief PASSIVE level worker routine to invoke DrainRequestQueue().
 * This is only used for the edge case where we had to requeue a URB_FUNCTION_SELECT_INTERFACE
 * operation.
 *
 * @param[in] WorkItem a handle to a WDFWORKITEM object for this worker routine.
 *
 */
void
PassiveDrain(
    IN WDFWORKITEM WorkItem)
{
    PUSB_FDO_WORK_ITEM_CONTEXT  context = WorkItemGetContext(WorkItem);
    
    TraceEvents(TRACE_LEVEL_WARNING, TRACE_DEVICE,
        __FUNCTION__":Device %p\n",
        context->FdoContext->WdfDevice);
    //
    // run the queue
    //
    AcquireFdoLock(context->FdoContext);
    DrainRequestQueue(context->FdoContext, TRUE);
    ReleaseFdoLock(context->FdoContext);
}

/**
 * @brief Drain the RequestQueue and restart the default queue iff drained.
 * *Must be called with the device lock held*
 * *Will release and re-acquire the device lock.*
 * @todo wouldn't it be cleaner to not call this with the lock held?
 *
 * @param[in] fdoContext A pointer to the USB_FDO_CONTEXT for the virtual usb controller device.
 * @param[in] FromPassiveLevel TRUE if invoked from PASSIVE_LEVEL i.e. from PassiveDrain() else FALSE.
 *
 */
_Requires_lock_held_(fdoContext->WdfDevice)
VOID
DrainRequestQueue(
    IN PUSB_FDO_CONTEXT fdoContext,
    IN BOOLEAN FromPassiveLevel)
{
    BOOLEAN queueEmpty = FALSE;
    BOOLEAN moreToDo = TRUE;
    WDFREQUEST Request;
    ULONG processed = 0;

    while (moreToDo)
    {
        NTSTATUS Status = 
            WdfIoQueueRetrieveNextRequest(
                fdoContext->RequestQueue,
                &Request);

        if (!NT_SUCCESS(Status))
        {
            moreToDo = FALSE;
            queueEmpty = TRUE; // this will cause a restart of default IO queue dispatching.
            break;
        }
        BOOLEAN RequeueThisRequest = FALSE;
        //
        // Status is STATUS_SUCCESS
        //
        if (fdoContext->DeviceUnplugged)
        {
            Status = STATUS_DEVICE_DOES_NOT_EXIST;
        }
        else
        {
            WDF_REQUEST_PARAMETERS  Parameters;
            WDF_REQUEST_PARAMETERS_INIT(&Parameters);

            WdfRequestGetParameters(Request, &Parameters);

            switch (Parameters.Parameters.DeviceIoControl.IoControlCode) 
            {
            case IOCTL_INTERNAL_USB_SUBMIT_URB:
                {                    
                    if (fdoContext->ResetInProgress)
                    {
                        RequeueThisRequest = TRUE;
                        break;
                    }

                    PURB Urb = (PURB) URB_FROM_REQUEST(Request);

                    if (!FromPassiveLevel &&
                        Urb->UrbHeader.Function == URB_FUNCTION_SELECT_INTERFACE)
                    {
                        TraceEvents(TRACE_LEVEL_WARNING, TRACE_QUEUE,
                            __FUNCTION__ ": Device %p Request %p URB_FUNCTION_SELECT_INTERFACE processing from DPC\n",
                            fdoContext->WdfDevice,
                            Request);

                        WDFWORKITEM worker = NewWorkItem(fdoContext,
                            PassiveDrain,
                            0,0,0,0);
                        if (worker)
                        {
                            RequeueThisRequest = TRUE;
                            WdfWorkItemEnqueue(worker);
                        }
                        else
                        {
                            TraceEvents(TRACE_LEVEL_WARNING, TRACE_QUEUE,
                                __FUNCTION__ ": Device %p Request %p URB_FUNCTION_SELECT_INTERFACE no workitem available\n",
                                fdoContext->WdfDevice,
                                Request);
                            Status = STATUS_INSUFFICIENT_RESOURCES;
                        }
                    }
                    else
                    {
                        fdoContext->RequeuedCount = 0; // test if we requeued a request while processing it.
                        SubmitUrb(fdoContext, Request, Urb);
                        Request = NULL; // Consumed.
                        if (fdoContext->RequeuedCount)
                        {
                            moreToDo = FALSE;
                        }
                    }
                }
                break;

            case IOCTL_INTERNAL_USB_SUBMIT_IDLE_NOTIFICATION:
                {
                    ProcessIdleNotificationRequest(fdoContext, Request);
                    Request = NULL; // Consumed.
                }
                break;
            default:

                Status = STATUS_INVALID_DEVICE_REQUEST;
                break;
            }
        }

        if (Request)
        {
            if (RequeueThisRequest)
            {
                fdoContext->RequeuedCount = 0;
                RequeueRequest(fdoContext, Request);
                if (fdoContext->RequeuedCount)
                {
                    moreToDo = FALSE;
                }
            }
            else
            {
                processed++;
                ReleaseFdoLock(fdoContext);
                WdfRequestComplete(Request, Status);
                AcquireFdoLock(fdoContext);
            }
        }
        else
        {
            processed++;
        }
    }
    if (queueEmpty)
    {
        ReleaseFdoLock(fdoContext);
        WdfIoQueueStart(fdoContext->UrbQueue);
        AcquireFdoLock(fdoContext);
    }
    if (processed > fdoContext->maxRequeuedRequestsProcessed)
    {
        fdoContext->maxRequeuedRequestsProcessed = processed;
    }
}

void
StopRequestQueue(
    IN PUSB_FDO_CONTEXT fdoContext)
{
    WdfIoQueueStop(fdoContext->UrbQueue, NULL, NULL);
}


PCHAR UsbControlCodeToString(
    ULONG ControlCode)
{
    PCHAR string = "Unknown Control Code";
    switch (ControlCode)
    {
    case IOCTL_INTERNAL_USB_RESET_PORT:
        string = "IOCTL_INTERNAL_USB_RESET_PORT";
        break;
    case IOCTL_INTERNAL_USB_GET_ROOTHUB_PDO:
        string = "IOCTL_INTERNAL_USB_GET_ROOTHUB_PDO";
        break;
    case IOCTL_INTERNAL_USB_GET_PORT_STATUS:
        string = "IOCTL_INTERNAL_USB_GET_PORT_STATUS";
        break;
    case IOCTL_INTERNAL_USB_ENABLE_PORT:
        string = "IOCTL_INTERNAL_USB_ENABLE_PORT";
        break;
    case IOCTL_INTERNAL_USB_GET_HUB_COUNT:
        string = "IOCTL_INTERNAL_USB_GET_HUB_COUNT";
        break;
    case IOCTL_INTERNAL_USB_CYCLE_PORT:
        string = "IOCTL_INTERNAL_USB_CYCLE_PORT";
        break;
    case IOCTL_INTERNAL_USB_GET_HUB_NAME:
        string = "IOCTL_INTERNAL_USB_GET_HUB_NAME";
        break;
    case IOCTL_INTERNAL_USB_GET_BUS_INFO:
        string = "IOCTL_INTERNAL_USB_GET_BUS_INFO";
        break;
    case IOCTL_INTERNAL_USB_GET_CONTROLLER_NAME:
        string = "IOCTL_INTERNAL_USB_GET_CONTROLLER_NAME";
        break;
    case IOCTL_INTERNAL_USB_GET_BUSGUID_INFO:
        string = "IOCTL_INTERNAL_USB_GET_BUSGUID_INFO";
        break;
    case IOCTL_INTERNAL_USB_GET_PARENT_HUB_INFO:
        string = "IOCTL_INTERNAL_USB_GET_PARENT_HUB_INFO";
        break;
    case IOCTL_INTERNAL_USB_SUBMIT_IDLE_NOTIFICATION:
        string = "IOCTL_INTERNAL_USB_SUBMIT_IDLE_NOTIFICATION";
        break;
    case IOCTL_INTERNAL_USB_GET_DEVICE_HANDLE:
        string = "IOCTL_INTERNAL_USB_GET_DEVICE_HANDLE";
        break;
#if (_WIN32_WINNT >= 0x0600)
    case IOCTL_INTERNAL_USB_NOTIFY_IDLE_READY:
        string = "IOCTL_INTERNAL_USB_NOTIFY_IDLE_READY";
        break;
    case IOCTL_INTERNAL_USB_REQ_GLOBAL_SUSPEND:
        string = "IOCTL_INTERNAL_USB_REQ_GLOBAL_SUSPEND";
        break;
    case IOCTL_INTERNAL_USB_REQ_GLOBAL_RESUME:
        string = "IOCTL_INTERNAL_USB_REQ_GLOBAL_RESUME";
        break;
    case IOCTL_INTERNAL_USB_RECORD_FAILURE:
        string = "IOCTL_INTERNAL_USB_RECORD_FAILURE";
        break;
    case IOCTL_INTERNAL_USB_GET_DEVICE_HANDLE_EX:
        string = "IOCTL_INTERNAL_USB_GET_DEVICE_HANDLE_EX";
        break;
    case IOCTL_INTERNAL_USB_GET_TT_DEVICE_HANDLE:
        string = "IOCTL_INTERNAL_USB_GET_TT_DEVICE_HANDLE";
        break;
    case IOCTL_INTERNAL_USB_GET_TOPOLOGY_ADDRESS:
        string = "IOCTL_INTERNAL_USB_GET_TOPOLOGY_ADDRESS";
        break;
    case IOCTL_INTERNAL_USB_GET_DEVICE_CONFIG_INFO:
        string = "IOCTL_INTERNAL_USB_GET_DEVICE_CONFIG_INFO";
        break;
    case IOCTL_USB_RESET_HUB:
        string = "IOCTL_USB_RESET_HUB";
        break;
    case IOCTL_USB_GET_HUB_CAPABILITIES_EX:
        string = "IOCTL_USB_GET_HUB_CAPABILITIES_EX";
        break;
#endif
    }
    return string;
}
