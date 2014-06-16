//
// Copyright (c) Citrix Systems, Inc.
//
/// @file Device.h USB FDO Device definitions.
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
#pragma once
#include "Trace.h"
#include "xenusb.h"
#include "xenif.h"
#include <usbioctl.h>
#include "DevicePdo.h"
#include <hubbusif.h>


#define NO_INTERFACE_LENGTH (ULONG) (sizeof(_URB_SELECT_CONFIGURATION) - sizeof(USBD_INTERFACE_INFORMATION))
   

struct SCRATCHPAD
{
    PVOID                        Buffer;
    PMDL                         Mdl;
    KEVENT                       CompletionEvent; 
    USBD_STATUS                  Status;
    ULONG                        BytesTransferred;
    WDF_USB_CONTROL_SETUP_PACKET Packet;
    XENUSBD_PIPE_COMMAND         Request;
    ULONG                        FrameNumber;
    ULONG                        Data; //!< response from scratch request
};

//
/// The device context performs the same job as
/// a WDM device extension in the driver frameworks
//
//
struct USB_FDO_CONTEXT
{
    WDFDEVICE                 WdfDevice;
    // --XT-- WDFINTERRUPT              WdfInterrupt;
    // --XT-- WDFDPC                    WdfDpc;
    CHAR                      FrontEndPath[128];
    //
    // Device state
    //
    USHORT                    Port;
    BOOLEAN                   PortAllocated;
    BOOLEAN                   PortConnected;
    BOOLEAN                   XenConfigured;
    BOOLEAN                   DeviceUnplugged;
    BOOLEAN                   CtlrDisconnected;
    BOOLEAN                   ResetInProgress;
    BOOLEAN                   NxprepBoot;
    BOOLEAN                   DeferredPdo; // wait for boot to get to service start before bringing up USB devices?
    BOOLEAN                   InDpc;
    LONG                      DpcOverLapCount;
    USB_DEVICE_SPEED          DeviceSpeed; //!< low-super.
    ULONG                     scratchFrameNumber;
    USB_DEVICE_PERFORMANCE_INFO_0 perfInfo; //!< WMI data.
    //
    // Lock state.
    //
    PETHREAD                 lockOwner;
    //
    // serialization of configuration
    //
    BOOLEAN                   ConfigBusy;
    //
    // idle notification support
    //
    WDFREQUEST                IdleRequest;
    //
    // workitems for passive level tasks.
    //
    WDFWORKITEM               ResetDeviceWorkItem;
    //
    /// Quirks database maintened by MSFT and us in
    /// HKLM\\CCS\\Control\\usbflags
    //
    WCHAR                     UsbInfoEntryName[24];
    BOOLEAN                   BlacklistDevice;   //!< this device should be disabled for this OS release.
    BOOLEAN                   FetchOsDescriptor; //!< this device supports os descriptor strings.
    BOOLEAN                   ResetDevice;       //!< this device supports reset without malfunctions.
    KEVENT                    resetCompleteEvent;
    
    USB_DEVICE_DESCRIPTOR     DeviceDescriptor;
    //
    /// an array of DeviceDescriptor.bNumConfigurations
    /// USB_CONFIG_INFO objects. These are fetched from the 
    /// device using a GET_DESCRIPTOR request where "The range 
    /// of values used for a descriptor index is from 0 to one 
    /// less than the number of descriptors of that type implemented 
    /// by the device." (m_deviceDescriptor.bNumConfigurations).
    ///
    /// Note that configs are read from the device by zero based index but that
    /// the PUSB_CONFIGURATION_DESCRIPTOR.bConfigurationValue is not
    /// the zero based index of a config. Instead the bConfigurationValue of
    /// a configuration descriptor is used in GET_CONFIGURATION and 
    /// SET_CONFIGURATION requests to identify the current
    /// configuration and select a specific configuration and ZERO is
    /// considered a special configuration value meaning set the device to
    /// 'unconfigured' or the device is currently unconfigured.
    //
    PUSB_CONFIG_INFO          ConfigData;
    UCHAR                     CurrentConfigValue; //!< 0 == unconfigured
    //
    /// some devices are not usb 2.0 compliant and use a bConfigurationValue
    /// of zero. For example the Broadcom 5880 fingerprint scanner on the Dell Latitude E6440.
    /// in order to support these devices bias the m_currentConfigValue by m_currentConfigOffset
    /// such that (m_currentConfigValue - m_currentConfigOffset) == bConfigurationValue.
    /// For compliant devices m_currentConfigOffset is zero. For non-compliant devices it is one.
    //
    UCHAR                     CurrentConfigOffset;
    //
    /// currently we support only one configuration
    /// The following are the default or currently selected configuration.
    //
    PUSB_CONFIGURATION_DESCRIPTOR ConfigurationDescriptor;
    ULONG                     NumInterfaces;
    ULONG                     NumEndpoints;
    //
    /// This is an array of interface object pointers
    /// one for each interface, alternate and concurrent.
    //
    PUSB_INTERFACE_DESCRIPTOR * InterfaceDescriptors;
    //
    /// A configuration supports up to 32 unique endpoints,
    /// 16 in endpoints and 16 out endpoints. Alternate interfaces
    /// cannot conflict with other concurrent interface endpoints.
    /// PipeDescriptors is an array of m_numEndpoints PIPE_DESCRIPTOR objects,
    /// one for each possible endpoint in each interface and interface alternate.
    //
    PIPE_DESCRIPTOR *         PipeDescriptors;
    
    PUSB_STRING               Manufacturer;
    PUSB_STRING               Product;
    PUSB_STRING               SerialNumber;
    POS_DESCRIPTOR_STRING     OsDescriptorString;
    POS_COMPAT_ID             CompatIds; 
    USHORT                    LangId;
    //
    /// scratch buffer. For internal URB requests.
    //
    SCRATCHPAD                ScratchPad;
    //
    /// a parallel queue for URBs from the child PDO.
    //
    WDFQUEUE                  UrbQueue;
    //
    /// a manual IO queue for hardware busy conditions.
    //
    WDFQUEUE                  RequestQueue;
    ULONG                     RequeuedCount;
    //
    // a watchdog timer for detecting Xen state changes.
    //
    WDFTIMER                  WatchdogTimer;
    //
    /// interface to Xen Ringbuffer.
    // /Allocated in AddDevice.
    //
    PXEN_INTERFACE            Xen;
    //
    /// DPC collection of processed Requests ready to complete.
    //
    WDFCOLLECTION             RequestCollection;
    //
    /// A collection of WDFWORKITEM objects.
    //
    WDFCOLLECTION             FreeWorkItems;
    //
    /// usb bus interface reference count (query interface not device interface.)
    //
    LONG                     busInterfaceReferenceCount;
    //
    /// The device controller interface symlink name.
    //
    WDFSTRING                hcdsymlink;
    //
    /// The child device hub interface symlink name.
    //
    WDFSTRING                hubsymlink;
    //
    // Transfer stats. Bulk Int and Iso xfers.
    //
    ULONGLONG                totalDirectTransfers;
    ULONGLONG                totalDirectErrors;
    ULONGLONG                totalIndirectTransfers;
    ULONGLONG                totalIndirectErrors;
    ULONG                    largestDirectTransfer;   // successful only
    ULONG                    largestIndirectTransfer; // successful only
    //
    // DPC stats.
    //
    ULONGLONG                totalDpcOverLapCount;
    ULONGLONG                totalDpcReQueueCount;
    ULONG                    maxDpcPasses;
    ULONG                    maxRequestsProcessed;
    ULONG                    maxRequeuedRequestsProcessed;
}; 
//
// This macro will generate an inline function called DeviceGetContext
// which will be used to get a pointer to the device context memory
// in a type safe manner.
//
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(USB_FDO_CONTEXT, DeviceGetFdoContext)

//
// Request Context
//
struct FDO_REQUEST_CONTEXT
{
    LONG CancelSet;
    LONG RequestCompleted;
};
typedef FDO_REQUEST_CONTEXT *PFDO_REQUEST_CONTEXT;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(FDO_REQUEST_CONTEXT, RequestGetRequestContext)


//
// Work Item Processing.
//

#define INIT_WORK_ITEM_COUNT 2 //!< pre-allocate this many work items.
#define WORK_ITEM_PARAMS 4

//
/// All WorkItems share a generic context.
//
struct USB_FDO_WORK_ITEM_CONTEXT
{
    PUSB_FDO_CONTEXT    FdoContext; //<! back pointer to device context.
    PFN_WDF_WORKITEM    CallBack;   //<! task specific worker function.
    ULONG_PTR           Params[WORK_ITEM_PARAMS];  //<! task specific parameters.
};
typedef USB_FDO_WORK_ITEM_CONTEXT *PUSB_FDO_WORK_ITEM_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(USB_FDO_WORK_ITEM_CONTEXT, WorkItemGetContext)


WDFWORKITEM
NewWorkItem(
    IN PUSB_FDO_CONTEXT fdoContext,
    IN OPTIONAL PFN_WDF_WORKITEM  callback,
    IN OPTIONAL ULONG_PTR Param0,
    IN OPTIONAL ULONG_PTR Param1,
    IN OPTIONAL ULONG_PTR Param2,
    IN OPTIONAL ULONG_PTR Param3);

VOID
FreeWorkItem(
    IN WDFWORKITEM WorkItem);

EVT_WDF_WORKITEM  EvtFdoDeviceGenericWorkItem;

EVT_WDF_DRIVER_DEVICE_ADD FdoEvtDeviceAdd; 

EVT_WDFDEVICE_WDM_IRP_PREPROCESS  FdoPreProcessQueryInterface;

EVT_WDF_CHILD_LIST_CREATE_DEVICE FdoEvtChildListCreateDevice;


NTSTATUS
SetPdoDescriptors(
    IN PWDFDEVICE_INIT DeviceInit,
    USB_DEVICE_DESCRIPTOR& descriptor,
    PUSB_CONFIGURATION_DESCRIPTOR config,
    PUSB_INTERFACE_DESCRIPTOR interfaceDescriptor,
    POS_COMPAT_ID compatIds);

void
StartRequestQueue(
    IN PUSB_FDO_CONTEXT fdoContext);

void
StopRequestQueue(
    IN PUSB_FDO_CONTEXT fdoContext);


PCHAR
DbgDevicePowerString(
    IN WDF_POWER_DEVICE_STATE Type);

VOID RemoveAllChildDevices(
    IN WDFDEVICE Device);

_Requires_lock_held_(fdoContext->WdfDevice)
VOID
FdoUnplugDevice(
    IN PUSB_FDO_CONTEXT fdoContext);

_Acquires_lock_(fdoContext->WdfDevice)
VOID
AcquireFdoLock(
    IN PUSB_FDO_CONTEXT fdoContext);

_Requires_lock_held_(fdoContext->WdfDevice)
VOID
ReleaseFdoLock(
    IN PUSB_FDO_CONTEXT fdoContext);


PCHAR UsbIoctlToString(
    ULONG IoControlCode);

VOID
ProcessDriverKeyNameRequest(
    IN PUSB_FDO_CONTEXT fdoContext,
    IN WDFREQUEST Request,
    IN size_t OutputBufferLength);

NTSTATUS
ProcessGetDescriptorFromNode(
    IN PUSB_FDO_CONTEXT fdoContext,
    IN PUSB_DESCRIPTOR_REQUEST descRequest,    
    PULONG DataLength);

PWCHAR
AllocAndQueryPropertyString(
    IN WDFDEVICE Device,
    IN DEVICE_REGISTRY_PROPERTY  DeviceProperty,
    OUT PULONG ResultLength);


