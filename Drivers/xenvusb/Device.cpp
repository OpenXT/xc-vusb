//
// Copyright (c) Citrix Systems, Inc.
//
/// @file Device.cpp USB FDO Device implementation.
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
#include "UsbConfig.h"
#include <wdmguid.h>
#include <devguid.h>
#include <initguid.h>
#include "RootHubPdo.h"

struct USB_FDO_INTERRUPT_CONTEXT
{
    PUSB_FDO_CONTEXT    FdoContext;
    ULONGLONG           IsrEntered;
    ULONGLONG           IsrActive; //!< this was for us.
};
typedef struct USB_FDO_INTERRUPT_CONTEXT * PUSB_FDO_INTERRUPT_CONTEXT;

// --XT-- WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(USB_FDO_INTERRUPT_CONTEXT, DeviceGetInterruptContext)

EVT_WDF_DEVICE_PREPARE_HARDWARE FdoEvtDevicePrepareHardware;
EVT_WDF_DEVICE_RELEASE_HARDWARE FdoEvtDeviceReleaseHardware;
EVT_WDF_DEVICE_D0_ENTRY FdoEvtDeviceD0Entry;
EVT_WDF_DEVICE_D0_ENTRY_POST_INTERRUPTS_ENABLED  FdoEvtDeviceD0EntryPostInterruptsEnabled;
EVT_WDF_DEVICE_D0_EXIT FdoEvtDeviceD0Exit;
EVT_WDF_DEVICE_SURPRISE_REMOVAL FdoEvtDeviceSurpriseRemoval;
EVT_WDF_DEVICE_CONTEXT_CLEANUP FdoEvtDeviceContextCleanup;
// --XT-- EVT_WDF_DPC  FdoEvtDeviceDpcFunc;
// --XT-- EVT_WDF_INTERRUPT_ISR  FdoEvtDeviceIsrFunc;
// --XT-- EVT_WDF_INTERRUPT_ENABLE  FdoEvtDeviceInterruptEnable;
// --XT-- EVT_WDF_INTERRUPT_DISABLE  FdoEvtDeviceInterruptDisable;
EVT_WDF_TIMER  FdoEvtTimerFunc;
EVT_WDF_DEVICE_FILE_CREATE  FdoEvtDeviceFileCreate;
EVT_WDF_FILE_CLOSE  FdoEvtFileClose;

// --XT-- New callback type for the DPC
EVTCHN_HANDLER_CB FdoEvtDeviceDpcFunc;

NTSTATUS LateSetup(IN WDFDEVICE);
NTSTATUS XenConfigure(IN PUSB_FDO_CONTEXT);
NTSTATUS XenDeconfigure(IN PUSB_FDO_CONTEXT);
VOID CleanupDisconnectedDevice(PUSB_FDO_CONTEXT fdoContext);

PCHAR
DbgDevicePowerString(
    IN WDF_POWER_DEVICE_STATE Type);

NTSTATUS
InitScratchpad(
    IN PUSB_FDO_CONTEXT fdoContext);

VOID
DeleteScratchpad(
    IN PUSB_FDO_CONTEXT fdoContext);

NTSTATUS
SetPdoDescriptors(
    IN PWDFDEVICE_INIT DeviceInit,
    USB_DEVICE_DESCRIPTOR& descriptor,
    PUSB_CONFIGURATION_DESCRIPTOR config,
    PUSB_INTERFACE_DESCRIPTOR interfaceDescriptor,
    POS_COMPAT_ID compatIds);
/**
 * @brief Wraps WDFDEVICE lock acquire operations.
 * Records the lock owner for debugging. Sanity tests are DBG only.
 *
 * @param[in] fdoContext. The context for the FDO device.
 *
 */
_Acquires_lock_(fdoContext->WdfDevice)
VOID
AcquireFdoLock(
    IN PUSB_FDO_CONTEXT fdoContext)
{
    PETHREAD caller = PsGetCurrentThread();
    if (!HTSASSERT(fdoContext->lockOwner != caller))
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DRIVER,
            __FUNCTION__": Assertion failure lockowner %p != caller %p\n",
            fdoContext->lockOwner,
            caller);
    }
    WdfObjectAcquireLock(fdoContext->WdfDevice);
    if (!HTSASSERT(fdoContext->lockOwner == NULL))
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DRIVER,
            __FUNCTION__": Assertion failure lockowner %p == NULL\n",
            fdoContext->lockOwner);
    }
    fdoContext->lockOwner = PsGetCurrentThread();
}

/**
 * @brief Wraps WDFDEVICE lock release operations.
 * Resets the lock owner to NULL. Sanity tests are DBG only.
 *
 * @param[in] fdoContext. The context for the FDO device.
 */
_Requires_lock_held_(fdoContext->WdfDevice)
VOID
ReleaseFdoLock(
    IN PUSB_FDO_CONTEXT fdoContext)
{
    PETHREAD caller = PsGetCurrentThread();
    if (!HTSASSERT(caller == fdoContext->lockOwner))
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DRIVER,
            __FUNCTION__": Assertion failure caller %p == lockOwner %p\n",
            caller,
            fdoContext->lockOwner);
    }
    fdoContext->lockOwner = NULL;
    WdfObjectReleaseLock(fdoContext->WdfDevice);
}

/**
 * @brief Called by the framework when a new PDO has arrived that this driver manages.
 * The device in question is not operational at this point in time.
 * 
 * @param[in] Driver handle to WDFDRIVER object created by DriverEntry()
 * @param[in,out] DeviceInit device init object provided by framework.
 * 
 * @returns NTSTATUS value indicating success or failure.
 * 
 */
NTSTATUS
FdoEvtDeviceAdd(
    _In_    WDFDRIVER       Driver,    
    _Inout_ PWDFDEVICE_INIT DeviceInit 
    )
{
    UNREFERENCED_PARAMETER(Driver);
    WDF_OBJECT_ATTRIBUTES   attributes;
    NTSTATUS status;

    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DEVICE,
        __FUNCTION__"\n");
    
    WdfDeviceInitSetDeviceType(DeviceInit, FILE_DEVICE_BUS_EXTENDER);
    WdfDeviceInitSetExclusive(DeviceInit, FALSE);
    WdfDeviceInitSetIoType(DeviceInit, WdfDeviceIoDirect);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, USB_FDO_CONTEXT);
    attributes.EvtCleanupCallback = FdoEvtDeviceContextCleanup;
    
    //
    // Device state callbacks.
    //
    WDF_PNPPOWER_EVENT_CALLBACKS    pnpPowerCallbacks;
    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnpPowerCallbacks);
    pnpPowerCallbacks.EvtDevicePrepareHardware = FdoEvtDevicePrepareHardware;
    pnpPowerCallbacks.EvtDeviceReleaseHardware = FdoEvtDeviceReleaseHardware;
    pnpPowerCallbacks.EvtDeviceD0Entry = FdoEvtDeviceD0Entry;
    pnpPowerCallbacks.EvtDeviceD0EntryPostInterruptsEnabled = FdoEvtDeviceD0EntryPostInterruptsEnabled;
    pnpPowerCallbacks.EvtDeviceD0Exit  = FdoEvtDeviceD0Exit;
    pnpPowerCallbacks.EvtDeviceSurpriseRemoval = FdoEvtDeviceSurpriseRemoval;
    
    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &pnpPowerCallbacks);
    //
    // establish a request context
    //
    WDF_OBJECT_ATTRIBUTES   requestAttributes;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&requestAttributes, FDO_REQUEST_CONTEXT);
    WdfDeviceInitSetRequestAttributes(DeviceInit, &requestAttributes);

    //
    // static verifier seems to have a rule that the FDO must call 
    // WdfFdoInitSetDefaultChildListConfig if any component in the driver has
    // dynamic child devices, and the roothub has one if it is not operating in 
    // connect usb hub mode.
    //
    WDF_CHILD_LIST_CONFIG  config;
    WDF_CHILD_LIST_CONFIG_INIT(&config,
        sizeof(PDO_INDENTIFICATION_DESCRIPTION),
        FdoEvtChildListCreateDevice);

    WdfFdoInitSetDefaultChildListConfig(DeviceInit,
        &config,
        WDF_NO_OBJECT_ATTRIBUTES);
    //
    // add a preprocess callback for QueryInterface to support multi-version USBDI intefaces
    //
    UCHAR MinorFunctionTable[1] = {IRP_MN_QUERY_INTERFACE};

    status = WdfDeviceInitAssignWdmIrpPreprocessCallback(
        DeviceInit,
        FdoPreProcessQueryInterface,
        IRP_MJ_PNP,
        MinorFunctionTable,
        1);
    if (!NT_SUCCESS(status))
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
            __FUNCTION__": WdfDeviceInitAssignWdmIrpPreprocessCallback failed error %x\n",
            status);
        return status;
    }

    //
    // Add create/close handlers
    //
    WDF_OBJECT_ATTRIBUTES   fileAttributes;
    WDF_OBJECT_ATTRIBUTES_INIT(&fileAttributes);
    fileAttributes.SynchronizationScope = WdfSynchronizationScopeNone;
    WDF_FILEOBJECT_CONFIG FileObjectConfig;
    WDF_FILEOBJECT_CONFIG_INIT(
        &FileObjectConfig,
        FdoEvtDeviceFileCreate,
        FdoEvtFileClose,
        WDF_NO_EVENT_CALLBACK);

    WdfDeviceInitSetFileObjectConfig(
        DeviceInit,
        &FileObjectConfig,
        &fileAttributes);
        
    WDFDEVICE device;
    status = WdfDeviceCreate(&DeviceInit, &attributes, &device);
    if (!NT_SUCCESS(status))
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
            __FUNCTION__": WdfDeviceCreate failed error %x\n",
            status);
        return status;
    }

    PUSB_FDO_CONTEXT fdoContext = DeviceGetFdoContext(device);
    RtlZeroMemory(fdoContext, sizeof(USB_FDO_CONTEXT));
    fdoContext->WdfDevice = device;
    KeInitializeEvent(&fdoContext->resetCompleteEvent, SynchronizationEvent, FALSE);
    //
    // allocate the dpc request collection.
    //
    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.ParentObject = device;
    status = WdfCollectionCreate(&attributes,
        &fdoContext->RequestCollection);

    if (!NT_SUCCESS(status))
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
        __FUNCTION__": WdfCollectionCreate failed\n");
        return status;
    };
    //
    // The FDO is the USB Controller, create a device interface for that.
    //
    status = WdfDeviceCreateDeviceInterface(
        device,
        &GUID_DEVINTERFACE_USB_HOST_CONTROLLER,
        NULL);

    if (!NT_SUCCESS(status)) 
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
            __FUNCTION__": WdfDeviceCreateDeviceInterface for device %p error %x\n",
            device,
            status);
        return status;
    }

    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);    
    attributes.ParentObject = device;
    status = WdfStringCreate(NULL, &attributes, &fdoContext->hcdsymlink);
    if (!NT_SUCCESS(status)) 
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
            __FUNCTION__": WdfStringCreate for device %p error %x\n",
            device,
            status);
        return status;
    }

    status = WdfDeviceRetrieveDeviceInterfaceString(device,
        &GUID_DEVINTERFACE_USB_HOST_CONTROLLER,
        NULL,
        fdoContext->hcdsymlink);
    if (!NT_SUCCESS(status)) 
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
            __FUNCTION__": WdfStringCreate for device %p error %x\n",
            device,
            status);
        return status;
    }
    //
    // Some of our resources are independent of the device state and 
    // can be allocated/initialized here.
    //
    status = InitScratchpad(fdoContext);

    if (!NT_SUCCESS(status)) 
    {
        return status;
    }
    //
    // Initialize the I/O Package and any Queues
    //
    status = FdoQueueInitialize(device);
    if (!NT_SUCCESS(status)) 
    {
        return status;
    }

    //
    // --XT-- All of the WDF ISR and DPC setup code was removed
    // here. The DPC is now setup through the Xen interface in the
    // previous call. Note the event channel is setup but not active
    // until the backend is connected.
    //

    //
    // Allocate a watchdog timer for our Xen interface.
    //
    WDF_TIMER_CONFIG  timerConfig;
    WDF_OBJECT_ATTRIBUTES  timerAttributes;

    WDF_TIMER_CONFIG_INIT(
        &timerConfig,
        FdoEvtTimerFunc); 

    WDF_OBJECT_ATTRIBUTES_INIT(&timerAttributes);
    timerAttributes.ParentObject = device;
    timerAttributes.ExecutionLevel = WdfExecutionLevelPassive;

    status = WdfTimerCreate(
        &timerConfig,
        &timerAttributes,
        &fdoContext->WatchdogTimer);

    if (!NT_SUCCESS(status)) 
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
        __FUNCTION__": WdfTimerCreate error %x\n",
        status);
        return status;
    }

    //
    // Create a collection of work items.
    //
    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.ParentObject = device;
    status = WdfCollectionCreate(&attributes,
        &fdoContext->FreeWorkItems);
    if (!NT_SUCCESS(status)) 
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
        __FUNCTION__": WdfCollectionCreate error %x\n",
        status);
        return status;
    }

    for (ULONG index = 0; index < INIT_WORK_ITEM_COUNT; index++)
    {
        WDFWORKITEM workitem = NewWorkItem(fdoContext,
            NULL,
            0,0,0,0);
        if (workitem)
        {
            status = WdfCollectionAdd(fdoContext->FreeWorkItems, workitem);
            if (!NT_SUCCESS(status))
            {
                TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
                    __FUNCTION__": WdfCollectionAdd for workitem index %d error %x\n",
                    index,
                    status);
                
                WdfObjectDelete(workitem);
                return status;
            }
        }
    }

    PNP_BUS_INFORMATION  busInformation;
    busInformation.BusNumber = 0;
    busInformation.BusTypeGuid = GUID_BUS_TYPE_USB;
    busInformation.LegacyBusType = PNPBus;

    WdfDeviceSetBusInformationForChildren(
        device,
        &busInformation);

    if (NT_SUCCESS(status)) {
        status = LateSetup(device);
    }

    return status;
}

/**
 * @brief The device object is being deleted. 
 * This callback frees any resources allocated in FdoEvtDeviceAdd().
 * 
 * @param[in] Device handle to WDFOBJECT created by FdoEvtDeviceAdd().
 * 
 */
VOID
FdoEvtDeviceContextCleanup (
    _In_ WDFOBJECT       Device)
{    
    PUSB_FDO_CONTEXT fdoContext = DeviceGetFdoContext(Device);

    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DEVICE,
        __FUNCTION__"\n");
    CleanupDisconnectedDevice(fdoContext);

    //
    // scratchpad cleanup.
    //
    DeleteScratchpad(fdoContext);
    //
    // xen interface cleanup
    //
    if (fdoContext->Xen)
    {
        DeallocateXenInterface(fdoContext->Xen);
    }
    if (fdoContext->CompatIds)
    {
        ExFreePool(fdoContext->CompatIds);
        fdoContext->CompatIds = NULL;
    }
}


/**
 * @brief A newly arrived device needs to be initialized and made operational.
 * Connects the new device object to its hardware resources.
 * 
 * @param[in] Device handle to the WDFDEVICE created by FdoEvtDeviceAdd().
 * @oaram[in] ResourcesTranslated translated resource list for the device.
 * 
 * @returns NSTATUS value indicating success or failure. 
 * 
 */
NTSTATUS
FdoEvtDevicePrepareHardware (
    _In_ WDFDEVICE,
    WDFCMRESLIST,
    _In_ WDFCMRESLIST)
{    
    // --XT-- PUSB_FDO_CONTEXT fdoContext = DeviceGetFdoContext(Device);

    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DEVICE,
        __FUNCTION__"\n");
    //
    // map the hardware resources
    //
    
    // --XT-- Removed call to MapXenDeviceRegisters and args to this call.

    return STATUS_SUCCESS;
}

/**
 * @brief Cleanup *AFTER* a device is no longer accessible.
 * 
 * @param[in] Device handle to the WDFDEVICE created by FdoEvtDeviceAdd().
 * 
 * @returns NTSTATUS value indicating success or failure. Should never fail.
 * 
 */
NTSTATUS
FdoEvtDeviceReleaseHardware(
    IN  WDFDEVICE    Device,
    IN  WDFCMRESLIST )
{    
    PUSB_FDO_CONTEXT fdoContext = DeviceGetFdoContext(Device);

    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DEVICE,
        __FUNCTION__"\n");
    FreeUsbConfigData(fdoContext);
    return STATUS_SUCCESS;
}

/**
 * @brief Transition to fully powered state.
 * 
 * @param[in] Device handle to the WDFDEVICE created by FdoEvtDeviceAdd().
 * @param[in] PreviousState
 * 
 * @returns NTSTATUS value indicating success or failure. 
 * 
 */
NTSTATUS
FdoEvtDeviceD0Entry(
    IN  WDFDEVICE Device,
    IN  WDF_POWER_DEVICE_STATE PreviousState)
{
    UNREFERENCED_PARAMETER(Device);
    // PUSB_FDO_CONTEXT fdoContext = DeviceGetFdoContext(Device);    
    NTSTATUS status = STATUS_SUCCESS;

    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DEVICE,
        __FUNCTION__": PreviousState %s\n",
        DbgDevicePowerString(PreviousState));

    XXX_TODO("STUB");
    return status;
}

NTSTATUS LateSetup(IN WDFDEVICE Device)
{
    PUSB_FDO_CONTEXT fdoContext = DeviceGetFdoContext(Device);
    NTSTATUS status = STATUS_SUCCESS;
    //
    // set up the XEN connection.
    //
    if (!fdoContext->NxprepBoot)
    {   
        status = XenConfigure(fdoContext);
        if (!NT_SUCCESS(status))
        {
            TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
                __FUNCTION__": Device %p xen configuration error %x",
                fdoContext->WdfDevice,
                status);
            return status;
        }
        //
        // get the USB device config data.
        //
        status = GetUsbConfigData(fdoContext);
        if (!NT_SUCCESS(status))
        {
            TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
                __FUNCTION__": %s GetUsbConfigData error %x\n",
                fdoContext->FrontEndPath,
                status);
        }
        else if (fdoContext->BlacklistDevice)
        {
            TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
                __FUNCTION__": %s Device is blacklisted. No PDO.\n",
                fdoContext->FrontEndPath);
        }
        else
        {
            fdoContext->PortConnected = TRUE;
        }
    }
    if (fdoContext->PortConnected  || fdoContext->NxprepBoot)
    {
        //
        // create a child device.
        //
        status = CreateRootHubPdo(fdoContext);
        if (NT_SUCCESS(status) && !fdoContext->NxprepBoot)
        {
            WdfTimerStart(fdoContext->WatchdogTimer, WDF_REL_TIMEOUT_IN_SEC(2));
        }
    }
    
    TraceEvents(NT_SUCCESS(status) ? TRACE_LEVEL_INFORMATION : TRACE_LEVEL_ERROR,
        TRACE_DEVICE,
        __FUNCTION__": %s Device %p status %x\n",
        fdoContext->FrontEndPath, 
        fdoContext->WdfDevice,
        status);

    return status;
}

/**
 * @brief Transition to fully powered state with interrupts enabled. 
 * This is the first chance we have to interact with a fully functional device,
 * so collect the device configuration so we can create a USB pdo that can be 
 * enumerated by PnP.
 *
 * Allow failures to "succeed" (return STATUS_SUCCESS) so that the
 * dreaded Yellow Bang does not occur. Instead, this device will appear
 * normally in Windows Device Manager, however it will not instantiate
 * its child RootHub PDO.
 * 
 * @param[in] Device handle to the WDFDEVICE created by FdoEvtDeviceAdd().
 * 
 * @returns NTSTATUS value indicating success or failure.
 * 
 */
NTSTATUS
FdoEvtDeviceD0EntryPostInterruptsEnabled(
    IN WDFDEVICE device,
    IN WDF_POWER_DEVICE_STATE)
{
    NTSTATUS status = STATUS_SUCCESS;
    PUSB_FDO_CONTEXT fdoContext = DeviceGetFdoContext(device);
    BOOLEAN online = XenCheckOnline(fdoContext->Xen);

    if (!online)
    {
        /* Backend device disappeared behind our back (while guest was in S3, for example), clean it up.. */
        CleanupDisconnectedDevice(fdoContext);
        return STATUS_SUCCESS;
    }
	
    if (!fdoContext->XenConfigured)
    {
        status = XenConfigure(fdoContext);
        if (!NT_SUCCESS(status))
        {
            return status;
        }
        WdfTimerStart(fdoContext->WatchdogTimer, WDF_REL_TIMEOUT_IN_SEC(2));
    }

    return status;
}

NTSTATUS
XenConfigure(
    IN PUSB_FDO_CONTEXT fdoContext)
{
    NTSTATUS status;
    if (fdoContext->XenConfigured)
    {
        TraceEvents(TRACE_LEVEL_WARNING, TRACE_DEVICE,
            __FUNCTION__": xen connection already configured");
        return STATUS_SUCCESS;
    }

    if (fdoContext->Xen)
    {
        DeallocateXenInterface(fdoContext->Xen);
    }

    fdoContext->Xen = AllocateXenInterface(fdoContext);
    if (!fdoContext->Xen)
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
            __FUNCTION__": AllocateXenInterface failed\n");
        return STATUS_UNSUCCESSFUL;
    }

    status = XenDeviceInitialize(fdoContext->Xen, FdoEvtDeviceDpcFunc);
    if (!NT_SUCCESS(status))
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
            __FUNCTION__": failed to initialize xen device");
        return status;
    }

    status = XenDeviceConnectBackend(fdoContext->Xen);
    if (!NT_SUCCESS(status))
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
            __FUNCTION__": failed to connect backend");
        return status;
    }

    fdoContext->XenConfigured = TRUE;
    return STATUS_SUCCESS;
}

NTSTATUS
XenDeconfigure(IN PUSB_FDO_CONTEXT fdoContext)
{
    if (!fdoContext->XenConfigured)
    {
        return STATUS_SUCCESS;
    }
    fdoContext->XenConfigured = FALSE;
    XenDisconnectDPC(fdoContext->Xen);
    XenDeviceDisconnectBackend(fdoContext->Xen);
    return STATUS_SUCCESS;
}

/**
 * @brief handles child device processing on device removal.
 * 
 * @param[in] Device handle to the WDFDEVICE created by FdoEvtDeviceAdd().
 * 
 */
VOID RemoveAllChildDevices(
    IN WDFDEVICE Device)
{
    WdfFdoLockStaticChildListForIteration(Device);

    WDFDEVICE  hChild = NULL;

    while ((hChild = WdfFdoRetrieveNextStaticChild(
        Device, 
        hChild,
        WdfRetrieveAddedChildren)) != NULL) 
    {
        NTSTATUS Status = WdfPdoMarkMissing(hChild);
        if (!NT_SUCCESS(Status))
        {
            TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
                __FUNCTION__": Device %p WdfPdoMarkMissing %p error %x\n",
                Device,
                hChild,
                Status);
            XXX_TODO("What to do with a pdo we can't mark missing?\n");
        }
        else
        {
            TraceEvents(TRACE_LEVEL_WARNING, TRACE_DEVICE,
                __FUNCTION__": Device %p WdfPdoMarkMissing %p\n",
                Device,
                hChild);

            WDF_DEVICE_STATE    deviceState;
            WDF_DEVICE_STATE_INIT (&deviceState);
            deviceState.Removed = WdfTrue;

            WdfDeviceSetDeviceState(hChild,
                &deviceState);
        }
    }

    WdfFdoUnlockStaticChildListFromIteration(Device);
}

/**
 * @brief common processing to "unplug" a device from the Windows perspective.
 * Must be called with lock held.
 *
 * @param[in] fdoContext. The context for the FDO device.
 *
 */
_Requires_lock_held_(fdoContext->WdfDevice)
VOID
FdoUnplugDevice(
    IN PUSB_FDO_CONTEXT fdoContext)
{
    if (!fdoContext->DeviceUnplugged)
    {
        TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DEVICE,
            __FUNCTION__": %s\n",
            fdoContext->FrontEndPath);
        fdoContext->DeviceUnplugged = TRUE;        
        if (fdoContext->ConfigBusy)
        {

            KeSetEvent(&fdoContext->ScratchPad.CompletionEvent, IO_NO_INCREMENT, FALSE);
        }
        if (!fdoContext->CtlrDisconnected)
        {            
            ReleaseFdoLock(fdoContext);
            // --XT-- WdfDpcCancel(fdoContext->WdfDpc, TRUE);
            //
            // --XT-- Should disable the DPC here
            //
            XenDisconnectDPC(fdoContext->Xen);
            RemoveAllChildDevices(fdoContext->WdfDevice);
            AcquireFdoLock(fdoContext);
        }
    }
    DrainRequestQueue(fdoContext, TRUE);
}

VOID
CleanupDisconnectedDevice(
    PUSB_FDO_CONTEXT fdoContext)
{    
    AcquireFdoLock(fdoContext);
    if (fdoContext->CtlrDisconnected)
    {
        ReleaseFdoLock(fdoContext);
        return;
    }
    fdoContext->CtlrDisconnected = TRUE; 
    
    ReleaseFdoLock(fdoContext);
    WdfTimerStop(fdoContext->WatchdogTimer, TRUE);
    // --XT-- WdfDpcCancel(fdoContext->WdfDpc, TRUE);
    //
    // --XT-- This also looks like a reasonable place to turn off the event channel.
    //
    XenDisconnectDPC(fdoContext->Xen);

    AcquireFdoLock(fdoContext);

    FdoUnplugDevice(fdoContext);

    BOOLEAN completeRequest = TRUE;

    if (fdoContext->IdleRequest)
    {
        if (RequestGetRequestContext(fdoContext->IdleRequest)->RequestCompleted)
        {
            // should never happen if fdoContext->IdleRequest is not NULL.

            TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
                __FUNCTION__": Device %p Request %p marked completed\n",
                fdoContext->WdfDevice,
                fdoContext->IdleRequest);
            completeRequest = FALSE;
        }
        else if (RequestGetRequestContext(fdoContext->IdleRequest)->CancelSet)
        {
            NTSTATUS Status = WdfRequestUnmarkCancelable(fdoContext->IdleRequest);
            if (!NT_SUCCESS(Status))
            {
                if (Status == STATUS_CANCELLED)
                {
                    //
                    // don't complete the request here it is owned by the
                    // cancel routine.
                    XXX_TODO("Trace level probably too high");

                    TraceEvents(TRACE_LEVEL_WARNING, TRACE_DEVICE,
                        __FUNCTION__": Device %p Request %p Cancelled\n",
                        fdoContext->WdfDevice,
                        fdoContext->IdleRequest);
                    completeRequest = FALSE;
                }
                else
                {
                    //
                    // note but ignore. 
                    //
                    TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
                        __FUNCTION__": Device %p Request %p WdfRequestUnmarkCancelable error %x\n",
                        fdoContext->WdfDevice,
                        fdoContext->IdleRequest,
                        Status);
                }
            }
        }
        WDFREQUEST idleRequest = fdoContext->IdleRequest;
        fdoContext->IdleRequest = NULL;
        if (completeRequest)
        {
            RequestGetRequestContext(idleRequest)->RequestCompleted = 0;
            ReleaseFdoLock(fdoContext);
            WdfRequestComplete(idleRequest, STATUS_CANCELLED);
            AcquireFdoLock(fdoContext);
        }
    }
    ReleaseFdoLock(fdoContext);
    if (fdoContext->XenConfigured)
    {
        CompleteRequestsFromShadow(fdoContext);
    }
}
/**
 * @brief Transition out of fully powered state.
 * This callback is invoked on unplug after FdoEvtDeviceSurpriseRemoval() or
 * on resource rebalance or device disable or shutdown.
 *
 * Plus take advantage of this opportunity to dump a bunch of stats on this device.
 * 
 * @param[in] Device handle to the WDFDEVICE created by FdoEvtDeviceAdd().
 * 
 * @returns NTSTATUS value indicating success or failure. 
 * 
 */
NTSTATUS
FdoEvtDeviceD0Exit(
    IN  WDFDEVICE device,
    IN  WDF_POWER_DEVICE_STATE)
{  
    PUSB_FDO_CONTEXT fdoContext = DeviceGetFdoContext(device);
    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DEVICE,
        __FUNCTION__": %s Device %p\n",
        fdoContext->FrontEndPath, 
        fdoContext->WdfDevice);

    WdfTimerStop(fdoContext->WatchdogTimer, TRUE);
    XenDeconfigure(fdoContext);

    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DEVICE,
        __FUNCTION__": %s\n"
        "    Direct Transfers: %I64d errors: %I64d largest: %d\n"
        "    Indirect Transfers: %I64d errors: %I64d largest: %d\n",
        fdoContext->FrontEndPath, 
        fdoContext->totalDirectTransfers,
        fdoContext->totalDirectErrors,
        fdoContext->largestDirectTransfer,
        fdoContext->totalIndirectTransfers,
        fdoContext->totalIndirectErrors,
        fdoContext->largestIndirectTransfer);


    //
    // --XT-- Removed tracing of 2 interrupt related values.
    //
    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DEVICE,
        __FUNCTION__": %s\n"
        "    DPC overlap %I64d DPC requeue %I64d\n"
        "    DPC max passes %d DPC max processed %d DPC drain queue requests %d\n",
        fdoContext->FrontEndPath, 
        fdoContext->totalDpcOverLapCount,
        fdoContext->totalDpcReQueueCount,
        fdoContext->maxDpcPasses,
        fdoContext->maxRequestsProcessed,
        fdoContext->maxRequeuedRequestsProcessed);

    // @todo anything else that needs undoing?
    return STATUS_SUCCESS;
}

/**
 * @brief Notification that the device has been unplugged.
 * This callback is invoked before FdoEvtDeviceD0Exit and FdoEvtDeviceReleaseHardware.
 * 
 * @param[in] Device handle to the WDFDEVICE created by FdoEvtDeviceAdd().
 * 
 */
VOID FdoEvtDeviceSurpriseRemoval(
  IN  WDFDEVICE Device)
{
    PUSB_FDO_CONTEXT fdoContext = DeviceGetFdoContext(Device);

    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DEVICE,
        __FUNCTION__": %s Device %p\n",
        fdoContext->FrontEndPath, 
        fdoContext->WdfDevice);

    CleanupDisconnectedDevice(fdoContext);
}

//
// Dynamic Bus Support - currently not needed except that
// static verifier is convinced we need it.
// Theoretically this would be a root hub PDO.
//
NTSTATUS FdoEvtChildListCreateDevice(
    _In_  WDFCHILDLIST ChildList,
    _In_  PWDF_CHILD_IDENTIFICATION_DESCRIPTION_HEADER IdentificationDescription,
    _In_  PWDFDEVICE_INIT ChildInit)
{
    PUSB_FDO_CONTEXT fdoContext = DeviceGetFdoContext(WdfChildListGetDevice(ChildList));
    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DEVICE,
        __FUNCTION__": %s\n",
        fdoContext->FrontEndPath);

    return CreateRootHubPdoWithDeviceInit(
        fdoContext,
        IdentificationDescription,
        ChildInit);
}

//
// Backend IO processing (isr and dpc). XXX move to own file.
//

//
// --XT-- Removed FdoEvtDeviceInterruptEnable
//

//
// --XT-- Removed FdoEvtDeviceInterruptDisable
//

/**
 * @brief DPC callback.
 * Calls the Xen interface api to process the ringbuffer until the ringbuffer is empty,
 * then completes all requests provided by the Xen api.
 *
 * @todo consider completion within the XenDpc() loop. However this would release the device
 * lock.
 *
 * @param[in] Dpc The WDFDPC handle.
 *  
 */
VOID
FdoEvtDeviceDpcFunc(
    IN VOID *Context)
{
    // --XT-- FDO context passed directly now.
    PUSB_FDO_CONTEXT fdoContext = (PUSB_FDO_CONTEXT)Context;
    //
    // this stuff needs to be done at DPC level in order to complete irps.
    //
    ULONG passes = 0;
    AcquireFdoLock(fdoContext);

    if (fdoContext->InDpc)
    {
        fdoContext->DpcOverLapCount++;
        ReleaseFdoLock(fdoContext);
        return;
    }
    fdoContext->InDpc = TRUE;

    BOOLEAN moreWork;
    do
    {
        moreWork = XenDpc(fdoContext, fdoContext->RequestCollection);
        passes++;
        if (fdoContext->DpcOverLapCount)
        {
            fdoContext->totalDpcOverLapCount += fdoContext->DpcOverLapCount;
            fdoContext->DpcOverLapCount = 0;
            moreWork = TRUE;
        }
        if (moreWork & (passes >= KeQueryActiveProcessorCount(NULL)))
        {
            //
            // reschedule the dpc to prevent starvation.
            //
            // --XT-- WdfDpcEnqueue(fdoContext->WdfDpc);
            //
            // --XT-- Schedule through the Xen interface now.
            //
            XenScheduleDPC(fdoContext->Xen);
            TraceEvents(TRACE_LEVEL_VERBOSE, TRACE_DPC,
                __FUNCTION__": enqueue dpc at %d passes\n",
                passes);
            fdoContext->totalDpcReQueueCount++;
            break;
        }

    } while (moreWork);
 
    fdoContext->InDpc = FALSE; // allow another dpc instance to run and add to the collection.
    if (passes > fdoContext->maxDpcPasses)
    {
        fdoContext->maxDpcPasses = passes;
    }
    //
    // complete all queued Irps. Note that this section is re-entrant.
    // Note also that all of these requests have been "uncanceled" and ahve
    // been marked as completed in their request contexts and that the
    // additional reference on the request object has been removed.
    // The collection can be safely completed with no race conditions.
    //
    ULONG responseCount = 0;
    WDFREQUEST Request;
    while ((Request = (WDFREQUEST) WdfCollectionGetFirstItem(fdoContext->RequestCollection)) != NULL)
    {
        WdfCollectionRemoveItem(fdoContext->RequestCollection, 0);

        TraceEvents(TRACE_LEVEL_VERBOSE, TRACE_DPC,
            __FUNCTION__": complete Request %p Status %x\n",
            Request,
            WdfRequestWdmGetIrp(Request)->IoStatus.Status);
        
        ReleaseFdoLock(fdoContext);

        WdfRequestCompleteWithPriorityBoost(Request,
            WdfRequestWdmGetIrp(Request)->IoStatus.Status,
            IO_SOUND_INCREMENT);
        
        AcquireFdoLock(fdoContext);

        responseCount++;
    }

    if (responseCount > fdoContext->maxRequestsProcessed)
    {
        fdoContext->maxRequestsProcessed = responseCount;
    }
    //
    // fire up any queued requests
    //    
    DrainRequestQueue(fdoContext, FALSE);

    ReleaseFdoLock(fdoContext);

    // --XT-- this trace was way too noisy, made it verbose.
    TraceEvents(TRACE_LEVEL_VERBOSE, TRACE_DPC,
        __FUNCTION__": exit responses processed %d passes %d\n",
        responseCount,
        passes);
}

//
// --XT-- Removed FdoEvtDeviceIsrFunc
//


/**
 * @brief Watchdog timer
 * checks the operational state of the xen interface and initiates surprise removal if
 * the interface is not operational and the device is not unplugged.
 * 
 * @param[in] Timer handle to timer allocated by FdoDeviceAdd()
 * 
 */
VOID
FdoEvtTimerFunc(
    IN WDFTIMER Timer)
{
    PUSB_FDO_CONTEXT fdoContext = DeviceGetFdoContext(WdfTimerGetParentObject(Timer));

    // XenCheckOperationalState waits on an event. Must be called at < DISPATCH_LEVEL.
    BOOLEAN operational = XenCheckOperationalState(fdoContext->Xen);

    AcquireFdoLock(fdoContext);
    if (!fdoContext->DeviceUnplugged)
    {
        if (operational)
        {
            // restart the timer.
            WdfTimerStart(Timer, WDF_REL_TIMEOUT_IN_SEC(1));
        }
        else
        {
            TraceEvents(TRACE_LEVEL_WARNING, TRACE_DEVICE,
                __FUNCTION__": %s Device %p unplug detected by watchdog\n",
                fdoContext->FrontEndPath, 
                fdoContext->WdfDevice);

            FdoUnplugDevice(fdoContext);
            ReleaseFdoLock(fdoContext);
            return;
        }
    }
    ReleaseFdoLock(fdoContext);
    //
    // @todo run the dpc - if this fixes anything fix the bug!
    //
    if (!fdoContext->DeviceUnplugged)
    {
        //
        // --XT-- Now passing the FDO context directly.
        //
        FdoEvtDeviceDpcFunc(fdoContext);
    }
}

/**
 * @brief handles CreateFile operations for the usb controller.
 * This function basically exists only to log that a create occurred.
 *
 * @param[in] Device A handle to a framework device object. 
 * @param[in] Request A handle to a framework request object that represents a file creation request
 * @param[in] FileObject A handle to a framework file object that describes a file that is being opened for the specified request. 
 * This parameter is NULL if the driver has specified WdfFileObjectNotRequired for the FileObjectClass member of the WDF_FILEOBJECT_CONFIG structure. 
 */
VOID
FdoEvtDeviceFileCreate (
    IN WDFDEVICE  Device,
    IN WDFREQUEST  Request,
    IN WDFFILEOBJECT  FileObject)
{
    PUSB_FDO_CONTEXT fdoContext = DeviceGetFdoContext(Device);
    UNREFERENCED_PARAMETER(FileObject);

    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DEVICE,
        __FUNCTION__": %s Device %p\n",
        fdoContext->FrontEndPath,
        fdoContext->WdfDevice);

    WdfRequestComplete(Request, STATUS_SUCCESS);
}

/**
 * @brief handles operations that must be performed when all of an application's 
 * accesses to a device have been closed.
 *
 * @param[in] FileObject The handle to the FileObject.
 */
VOID
FdoEvtFileClose (
    IN WDFFILEOBJECT  FileObject)
{
    PUSB_FDO_CONTEXT fdoContext = DeviceGetFdoContext(WdfFileObjectGetDevice(FileObject));

    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DEVICE,
        __FUNCTION__": %s Device %p\n",
        fdoContext->FrontEndPath,
        fdoContext->WdfDevice);
}



//
// Workitem support.
// Maintain a collection of workitems, 2 to start with, and
// hand them out on demand. If the collection is empty, allocate
// a new work item.
//

/**
 * @brief generic workitem callback function 
 * calls the specific callback function in the work item context.
 * 
 * @param[in] WorkItem handle to the workitem object.
 * 
 */
VOID
EvtFdoDeviceGenericWorkItem (
    IN WDFWORKITEM  WorkItem)
{
    PUSB_FDO_WORK_ITEM_CONTEXT  context = WorkItemGetContext(WorkItem);
    //
    // NULL indicates somebody screwed up, log it and forget it.
    //
    if (context->CallBack)
    {
        context->CallBack(WorkItem);
    }
    else
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
            __FUNCTION__": %s Device %p WorkItem %p NULL callback\n",            
            context->FdoContext->FrontEndPath,
            context->FdoContext->WdfDevice,
            WorkItem);
    }
    context->CallBack = NULL;
    //
    // put this workitem into our collection.
    //
    AcquireFdoLock(context->FdoContext);
    NTSTATUS Status = WdfCollectionAdd(context->FdoContext->FreeWorkItems, WorkItem);
    ReleaseFdoLock(context->FdoContext);

    if (!NT_SUCCESS(Status))
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
            __FUNCTION__": %s Device %p WdfCollectionAdd error %x deleting workitem %p\n",
            context->FdoContext->FrontEndPath,
            context->FdoContext->WdfDevice,
            Status,
            WorkItem);
        // oh well delete it
        WdfObjectDelete(WorkItem);
    }
}


/**
 * @brief Allocate a workitem from or for our collection of workitems.
 * Must be called with the device lock held.
 * 
 * @param[in] Device handle to the WDFDEVICE created by FdoEvtDeviceAdd.
 * @param[in] callback only optional if this is add device doing an allocation!
 * @param[in] Param0 arbitrary context data
 * @param[in] Param1 arbitrary context data
 * @param[in] Param2 arbitrary context data
 * @param[in] Param3 arbitrary context data
 * 
 * @returns a WDFWORKITEM handle or NULL on failure.
 * 
 */
WDFWORKITEM
NewWorkItem(
    IN PUSB_FDO_CONTEXT fdoContext,
    IN PFN_WDF_WORKITEM  callback, 
    IN ULONG_PTR Param0,
    IN ULONG_PTR Param1,
    IN ULONG_PTR Param2,
    IN ULONG_PTR Param3)
{
    //
    // First try to get a workitem from our collection of them.
    //    
    WDFWORKITEM  WorkItem = (WDFWORKITEM) WdfCollectionGetFirstItem(fdoContext->FreeWorkItems);
    if (WorkItem == NULL)
    {
        //
        // ok - allocate a new one.
        //
        TraceEvents(TRACE_LEVEL_VERBOSE, TRACE_DEVICE,
            __FUNCTION__": Device %p FreeWorkItems is empty, init count %d\n",
            fdoContext->WdfDevice,
            INIT_WORK_ITEM_COUNT);

        NTSTATUS  status = STATUS_SUCCESS;
        WDF_OBJECT_ATTRIBUTES  attributes;
        WDF_WORKITEM_CONFIG  workitemConfig;

        WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
        WDF_OBJECT_ATTRIBUTES_SET_CONTEXT_TYPE(
            &attributes,
            USB_FDO_WORK_ITEM_CONTEXT);

        attributes.ParentObject = fdoContext->WdfDevice;

        WDF_WORKITEM_CONFIG_INIT(
            &workitemConfig,
            EvtFdoDeviceGenericWorkItem);

        status = WdfWorkItemCreate(
            &workitemConfig,
            &attributes,
            &WorkItem);

        if (!NT_SUCCESS(status)) 
        {
            TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
                __FUNCTION__": WdfWorkItemCreate error %x\n",
                status);
            return NULL;
        }
    }
    else
    {
        // note that WdfCollectionGetFirstItem doesn't remove it from the collection.
        WdfCollectionRemove(fdoContext->FreeWorkItems, WorkItem);
    }
    if (WorkItem)
    {
        // initialize it.
        PUSB_FDO_WORK_ITEM_CONTEXT  context = WorkItemGetContext(WorkItem);
        context->FdoContext = fdoContext;
        context->CallBack = callback;
        context->Params[0] = Param0;
        context->Params[1] = Param1;
        context->Params[2] = Param2;
        context->Params[3] = Param3;
    }
    return WorkItem;
}

/** 
 * @brief deallocate callback item.
 *  Must be called with lock held.
 * *Note* this is called only to deal with error cases. The normal
 * callback function re-adds the work item to the collection.
 * 
 * @param[in] WorkItem allocated WorkItem to be freed.
 * 
 */
VOID
FreeWorkItem(
    IN WDFWORKITEM WorkItem)
{
    PUSB_FDO_WORK_ITEM_CONTEXT  context = WorkItemGetContext(WorkItem);
    NTSTATUS Status = WdfCollectionAdd(context->FdoContext->FreeWorkItems,
        WorkItem);

    if (!NT_SUCCESS(Status))
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
            __FUNCTION__": Device %p WdfCollectionAdd error %x, deleting instead.\n",
            context->FdoContext->WdfDevice,
            Status);
        WdfObjectDelete(WorkItem);
    }
}


//
// file local helper functions.
//
NTSTATUS
InitScratchpad(
    IN PUSB_FDO_CONTEXT fdoContext)
{
    NTSTATUS status;
    KeInitializeEvent(&fdoContext->ScratchPad.CompletionEvent, NotificationEvent, FALSE);
    
    fdoContext->ScratchPad.Buffer = ExAllocatePoolWithTag(NonPagedPool, PAGE_SIZE, XVU1);
    if (!fdoContext->ScratchPad.Buffer)
    {
            status =  STATUS_NO_MEMORY;
            TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
                __FUNCTION__": Device %p ExAllocatePoolWithTag failed\n",
                fdoContext->WdfDevice);
            return status;
    }
    RtlZeroMemory(fdoContext->ScratchPad.Buffer, PAGE_SIZE);

    fdoContext->ScratchPad.Mdl = IoAllocateMdl(fdoContext->ScratchPad.Buffer,
        PAGE_SIZE,
        FALSE,
        FALSE,
        NULL);
    if (!fdoContext->ScratchPad.Mdl)
    {
        status =  STATUS_NO_MEMORY;
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
            __FUNCTION__": device %p IoAllocateMdl failed\n",
            fdoContext->WdfDevice);
        return status;
    }
    MmBuildMdlForNonPagedPool(fdoContext->ScratchPad.Mdl);

    return STATUS_SUCCESS;
}

VOID
DeleteScratchpad(
    IN PUSB_FDO_CONTEXT fdoContext)
{
    if (fdoContext->ScratchPad.Buffer)
    {
        ExFreePool(fdoContext->ScratchPad.Buffer);
        fdoContext->ScratchPad.Buffer = NULL;
    }

    if (fdoContext->ScratchPad.Mdl)
    {
        IoFreeMdl(fdoContext->ScratchPad.Mdl);
        fdoContext->ScratchPad.Mdl = NULL;
    }
}

PCHAR
DbgDevicePowerString(
    IN WDF_POWER_DEVICE_STATE Type)
{
    switch (Type)
    {
    case WdfPowerDeviceInvalid:
        return "WdfPowerDeviceInvalid";
    case WdfPowerDeviceD0:
        return "WdfPowerDeviceD0";
    case PowerDeviceD1:
        return "WdfPowerDeviceD1";
    case WdfPowerDeviceD2:
        return "WdfPowerDeviceD2";
    case WdfPowerDeviceD3:
        return "WdfPowerDeviceD3";
    case WdfPowerDeviceD3Final:
        return "WdfPowerDeviceD3Final";
    case WdfPowerDevicePrepareForHibernation:
        return "WdfPowerDevicePrepareForHibernation";
    case WdfPowerDeviceMaximum:
        return "PowerDeviceMaximum";
    default:
        return "UnKnown Device Power State";
    }
}









