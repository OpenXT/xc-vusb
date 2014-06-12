//
// Copyright (c) Citrix Systems, Inc., All rights reserved.
//
/// @file RootHubPdo.h Hub PDO definitions
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

struct PORT_DEVICE_INFORMATION
{
    LONG                      DeviceHandleRefCount;
    USHORT                    HubPortNumber;
    USHORT                    HubPortStatus;
    ULONG                     DeviceErrataFlag;
    ULONG                     PortRemoveFlags;
    USHORT                    TtPortNumber;
    GUID                      ContainerId;
};

struct ROOT_HUB_CONFIG
{
    USB_DEVICE_DESCRIPTOR        DeviceDescriptor;
    USB_CONFIG_INFO              ConfigInfo;
    USB_CONFIGURATION_DESCRIPTOR ConfigurationDescriptor;
    USB_INTERFACE_DESCRIPTOR     InterfaceDescriptor;
    PUSB_INTERFACE_DESCRIPTOR    InterfaceArray[1];
    USB_ENDPOINT_DESCRIPTOR      EndpointDescriptor;
    PIPE_DESCRIPTOR              PipeDescriptor;
    USB_HUB_DESCRIPTOR           HubDescriptor;
};
//
/// Context for the Hub PDO.
/// The Hub PDO represents a root hub usb device and
/// provides information about each device connected to 
/// the hub. Currently each root hub has exactly one port.
//
//
struct USB_HUB_PDO_CONTEXT
{
    WDFDEVICE                 WdfDevice;
    USHORT                    Port;
    WDFDEVICE                 Parent;
    WDFQUEUE                  ParentQueue;
    WDFQUEUE                  UrbQueue;
    ROOT_HUB_CONFIG           HubConfig;
    //
    // Each HUB has exactly one port.
    // port state and features
    //
    WDFQUEUE                  StatusChangeQueue;
    USHORT                    ReportedStatus;
    USHORT                    PortFeatureStatus;
    USHORT                    PortFeatureChange; 
    BOOLEAN                   HubDisconnectReported;
    //
    /// Hub interface
    //
    LONG                      BusInterfaceReferenceCount;
    //
    /// Hub minidump interface
    //
    LONG                      MinidumpInterfaceReferenceCount;
    //
    /// Hub selective suspend interface
    //
    LONG                      SelectiveSuspendInterfaceReferenceCount;
    //
    /// Hub Forward Progress
    //
    LONG                      FowardProgressInterfaceReferenceCount;
    //
    /// The one port for this hub.
    //
    PORT_DEVICE_INFORMATION   PortDevice;
    //
    /// obsolete
    //
    WDFCHILDLIST              ChildList;
}; 
typedef USB_HUB_PDO_CONTEXT * PUSB_HUB_PDO_CONTEXT;
//
// This macro will generate an inline function called DeviceGetContext
// which will be used to get a pointer to the device context memory
// in a type safe manner.
//
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(USB_HUB_PDO_CONTEXT, DeviceGetHubPdoContext)

struct PDO_INDENTIFICATION_DESCRIPTION
{
    WDF_CHILD_IDENTIFICATION_DESCRIPTION_HEADER Header; 
    USHORT                                      Port;
};
typedef PDO_INDENTIFICATION_DESCRIPTION *PPDO_INDENTIFICATION_DESCRIPTION;

//
// given a HUB WdfDevice, produce the USB_FDO_CONTEXT for its parent.
//
#define GetFdoContextFromHubDevice(_HubWdfDevice_) DeviceGetFdoContext(DeviceGetHubPdoContext(_HubWdfDevice_)->Parent)

struct HUB_STATUS_CHANGE_CONTEXT
{
    PUCHAR Buffer;
    ULONG  Length;
};
typedef HUB_STATUS_CHANGE_CONTEXT *PHUB_STATUS_CHANGE_CONTEXT;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(HUB_STATUS_CHANGE_CONTEXT, RequestGetStatusChangeContext)


typedef enum _USB_PORT_FEATURE_SELECTOR {
    PORT_CONNECTION         = 0,
    PORT_ENABLE             = 1,
    PORT_SUSPEND            = 2,
    PORT_OVER_CURRENT       = 3,
    PORT_RESET              = 4,
    PORT_LINK_STATE         = 5,
    PORT_POWER              = 8,
    PORT_LOW_SPEED          = 9,
    C_PORT_CONNECTION       = 16,
    C_PORT_ENABLE           = 17,
    C_PORT_SUSPEND          = 18,
    C_PORT_OVER_CURRENT     = 19,
    C_PORT_RESET            = 20,
    PORT_TEST               = 21,
    PORT_INDICATOR          = 22,
    PORT_U1_TIMEOUT         = 23,
    PORT_U2_TIMEOUT         = 24,
    C_PORT_LINK_STATE       = 25,
    C_PORT_CONFIG_ERROR     = 26,
    PORT_REMOTE_WAKE_MASK   = 27,
    BH_PORT_RESET           = 28,
    C_BH_PORT_RESET         = 29,
    FORCE_LINKPM_ACCEPT     = 30
} USB_PORT_FEATURE_SELECTOR, *PUSB_PORT_FEATURE_SELECTOR;
#define C_HUB_LOCAL_POWER 0
#define C_HUB_OVER_CURRENT 1


extern USB_HUB_DESCRIPTOR RootHubDescriptor;

NTSTATUS
CreateRootHubPdo(
    IN PUSB_FDO_CONTEXT fdoContext);

NTSTATUS
CreateRootHubPdoWithDeviceInit(
    IN PUSB_FDO_CONTEXT fdoContext,
    _In_opt_ PWDF_CHILD_IDENTIFICATION_DESCRIPTION_HEADER IdentificationDescription,
    IN PWDFDEVICE_INIT pDeviceInit);

NTSTATUS
HubQueueInitialize(
    _In_ WDFDEVICE hDevice);

EVT_WDF_CHILD_LIST_CREATE_DEVICE  HubEvtChildListCreateDevice;


//
// Device Events
//
EVT_WDF_DEVICE_D0_ENTRY HubEvtDeviceD0Entry;
EVT_WDF_DEVICE_D0_EXIT HubEvtDeviceD0Exit;
EVT_WDF_DEVICE_SURPRISE_REMOVAL HubEvtDeviceSurpriseRemoval;

NTSTATUS 
RootHubIfGetLocationString(
  _Inout_  PVOID Context,
  _Out_    PWCHAR *LocationStrings);

// {DC253819-5174-4B3D-8471-36F48701FCB3}
DEFINE_GUID(XEN_VROOT_HUB_DEVICE, 
0xdc253819, 0x5174, 0x4b3d, 0x84, 0x71, 0x36, 0xf4, 0x87, 0x1, 0xfc, 0xb3);


NTSTATUS
RootHubPreProcessQueryInterface(
    IN WDFDEVICE Device,
    IN PIRP Irp);

VOID
HubCheckStatusChange(
    IN PUSB_HUB_PDO_CONTEXT hubContext);


