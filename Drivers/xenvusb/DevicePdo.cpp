//
// Copyright (c) Citrix Systems, Inc., All rights reserved.
//
/// @file DevicePdo.cpp USB Device PDO IO Queue implementation.
//
#include "driver.h"
#include "usbioctl.h"
#include "UsbRequest.h"
#include "RootHubPdo.h"



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
EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL DevicePdoEvtIoDeviceControl;
EVT_WDF_IO_QUEUE_IO_INTERNAL_DEVICE_CONTROL DevicePdoEvtIoInternalDeviceControl;
EVT_WDF_IO_QUEUE_IO_STOP DevicePdoEvtIoStop;

/**
 * @brief Initialize the USB Device PDO queue.
 * The USB Device PDO uses a very simple model: forward all URBs to the parent.
 * 
 * @param[in] Device The handle to the PDO device.
 *
 * @returns NTSTATUS value indicating success or failure.
 *
 */
NTSTATUS
DevicePdoQueueInitialize(
    _In_ WDFDEVICE Device)
{
    WDFQUEUE queue;
    NTSTATUS status;
    WDF_IO_QUEUE_CONFIG    queueConfig;

    PAGED_CODE();
    //
    // Configure the default queue to handle IOCTL and URB requests.
    //
    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(
         &queueConfig,
        WdfIoQueueDispatchParallel);

    queueConfig.EvtIoDeviceControl = DevicePdoEvtIoDeviceControl;
    queueConfig.EvtIoInternalDeviceControl = DevicePdoEvtIoInternalDeviceControl;
    queueConfig.EvtIoStop = DevicePdoEvtIoStop;

    status = WdfIoQueueCreate(
                 Device,
                 &queueConfig,
                 WDF_NO_OBJECT_ATTRIBUTES,
                 &queue);
    return status;
}


/**
 * @brief process IOCTL requests.
 * No user mode IOCTLs are defined for USB device PDOs. Instead they
 * are all supposed to be processed by opening either the HUB or Controller device.
 *
 * @param[in] Queue the IOCTL queue.
 * @param[in] Request the IOCTL request handle.
 * @param[in] OutputBufferLength size of the IOCTL output buffer.
 * @param[in] InputBufferLength size of the IOCTL input buffer
 * @param[in] IoControlCode the IOCTL function code.
 */
VOID
DevicePdoEvtIoDeviceControl(
    _In_ WDFQUEUE Queue,
    _In_ WDFREQUEST Request,
    _In_ size_t OutputBufferLength,
    _In_ size_t InputBufferLength,
    _In_ ULONG IoControlCode
    )
{
    
    TraceEvents(TRACE_LEVEL_VERBOSE, TRACE_QUEUE, 
        __FUNCTION__": Queue %p, Request %p IoControlCode %d\n", 
        Queue, 
        Request, 
        IoControlCode);

    UNREFERENCED_PARAMETER(OutputBufferLength);
    UNREFERENCED_PARAMETER(InputBufferLength);

    WdfRequestComplete(Request, STATUS_UNSUCCESSFUL);
}


/**
 * @brief process URB requests for the USB devuce.
 * URBs are simply forwarded down the stack from USB PDO to ROOT HUB to
 * the virutal usb controller..
 *
 * @param[in] Queue the URB queue.
 * @param[in] Request the URB request handle.
 * @param[in] OutputBufferLength unused.
 * @param[in] InputBufferLength unused.
 * @param[in] IoControlCode the IOCTL function code - muse be IOCTL_INTERNAL_USB_SUBMIT_URB.
 *
 */
VOID
DevicePdoEvtIoInternalDeviceControl(
    _In_ WDFQUEUE Queue,
    _In_ WDFREQUEST Request,
    _In_ size_t OutputBufferLength,
    _In_ size_t InputBufferLength,
    _In_ ULONG IoControlCode
    )
{
    NTSTATUS Status = STATUS_UNSUCCESSFUL;
    
    TraceEvents(TRACE_LEVEL_VERBOSE, TRACE_QUEUE, 
        __FUNCTION__": Queue %p, Request %p IoControlCode %d\n", 
        Queue, 
        Request, 
        IoControlCode);

    UNREFERENCED_PARAMETER(OutputBufferLength);
    UNREFERENCED_PARAMETER(InputBufferLength);

    PUSB_DEVICE_PDO_CONTEXT pdoContext = DeviceGetDevicePdoContext(WdfIoQueueGetDevice(Queue)); 
    
    WDF_REQUEST_FORWARD_OPTIONS forwardOptions;
    WDF_REQUEST_FORWARD_OPTIONS_INIT(&forwardOptions);

    Status = WdfRequestForwardToParentDeviceIoQueue(
        Request,
        pdoContext->ParentQueue,
        &forwardOptions);

    if (!NT_SUCCESS(Status))
    {
        char * errorString = "Unexpected error code";
        char * infoString = "";

        switch (Status)
        {
        case STATUS_INFO_LENGTH_MISMATCH:
            errorString = "STATUS_INFO_LENGTH_MISMATCH";
            infoString = "size of forwardOptions invalid";
            break;

        case STATUS_INVALID_PARAMETER:
            errorString = "STATUS_INVALID_PARAMETER";
            infoString = "forwardOptions invalid member";
            break;

        case STATUS_INVALID_DEVICE_REQUEST:
            errorString = "STATUS_INVALID_DEVICE_REQUEST";
            infoString = "assorted queue processing malfunctions";
            break;

        case STATUS_WDF_BUSY:
            errorString = "STATUS_WDF_BUSY";
            infoString = "parent not accepting new requests";
            XXX_TODO("Handle WDF_BUSY condition in device pdo");
            break;

        default:
            break;
        };

        TraceEvents(TRACE_LEVEL_ERROR, TRACE_QUEUE,
            __FUNCTION__": Device %p Request %p ParentQueue %p Parent Device %p WdfRequestForwardToParentDeviceIoQueue error %x %s %s\n",
            pdoContext->WdfDevice,
            Request,
            pdoContext->ParentQueue,
            pdoContext->Parent,
            Status,
            errorString,
            infoString);

        WdfRequestComplete(Request, STATUS_UNSUCCESSFUL);
    }

    return;
}

VOID
DevicePdoEvtIoStop(
    _In_ WDFQUEUE Queue,
    _In_ WDFREQUEST Request,
    _In_ ULONG ActionFlags
)
{
    TraceEvents(TRACE_LEVEL_INFORMATION, 
                TRACE_QUEUE, 
                __FUNCTION__": Queue 0x%p, Request 0x%p ActionFlags %d", 
                Queue, Request, ActionFlags);

    return;
}
