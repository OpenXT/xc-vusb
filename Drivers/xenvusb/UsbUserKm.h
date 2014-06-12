//
// Copyright (c) 2014 Citrix Systems, Inc.
//
//
// @TODO defined in um\usbuser.h and can't be included in driver.
// Many subcategories of user mode USB requests. Support requirements
// unknown
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
#ifndef IOCTL_USB_USER_REQUEST
#define IOCTL_USB_USER_REQUEST USB_CTL(HCD_USER_REQUEST)
#include <pshpack1.h>
/*
    define error codes
*/
typedef enum _USB_USER_ERROR_CODE {

    UsbUserSuccess = 0,
    UsbUserNotSupported,
    UsbUserInvalidRequestCode,
    UsbUserFeatureDisabled,
    UsbUserInvalidHeaderParameter,
    UsbUserInvalidParameter,
    UsbUserMiniportError,
    UsbUserBufferTooSmall,
    UsbUserErrorNotMapped,
    UsbUserDeviceNotStarted,
    UsbUserNoDeviceConnected

} USB_USER_ERROR_CODE;

/*
    define USB USER request Codes
*/

/*
    The following APIS are enabled always
*/
#define USBUSER_GET_CONTROLLER_INFO_0           0x00000001
#define USBUSER_GET_CONTROLLER_DRIVER_KEY       0x00000002
#define USBUSER_PASS_THRU                       0x00000003
#define USBUSER_GET_POWER_STATE_MAP             0x00000004
#define USBUSER_GET_BANDWIDTH_INFORMATION       0x00000005
#define USBUSER_GET_BUS_STATISTICS_0            0x00000006
#define USBUSER_GET_ROOTHUB_SYMBOLIC_NAME       0x00000007
#define USBUSER_GET_USB_DRIVER_VERSION          0x00000008
#define USBUSER_GET_USB2_HW_VERSION             0x00000009
#define USBUSER_USB_REFRESH_HCT_REG             0x0000000a

/*
    The following APIs are only enabled when the
    devlopr key is set in the registry.
*/
#define USBUSER_OP_SEND_ONE_PACKET              0x10000001

/*
    The following APIs are only enabled when the
    root hub is disabled.
*/

#define USBUSER_OP_RAW_RESET_PORT               0x20000001
#define USBUSER_OP_OPEN_RAW_DEVICE              0x20000002
#define USBUSER_OP_CLOSE_RAW_DEVICE             0x20000003
#define USBUSER_OP_SEND_RAW_COMMAND             0x20000004

#define USBUSER_SET_ROOTPORT_FEATURE            0x20000005
#define USBUSER_CLEAR_ROOTPORT_FEATURE          0x20000006
#define USBUSER_GET_ROOTPORT_STATUS             0x20000007

#define USBUSER_INVALID_REQUEST                 0xFFFFFFF0

#define USBUSER_OP_MASK_DEVONLY_API             0x10000000
#define USBUSER_OP_MASK_HCTEST_API              0x20000000

typedef struct _USBUSER_REQUEST_HEADER {
    /*
        API Requested
    */
    ULONG UsbUserRequest;
    /*
        status code returned by port driver
    */
    USB_USER_ERROR_CODE UsbUserStatusCode;
    /*
        size of client input/output buffer
        we always use the same buffer for input
        and output
    */
    ULONG RequestBufferLength;
    /*
        size of buffer required to get all of the data
    */
    ULONG ActualBufferLength;

} USBUSER_REQUEST_HEADER, *PUSBUSER_REQUEST_HEADER;

typedef enum _WDMUSB_POWER_STATE {

    WdmUsbPowerNotMapped = 0,

    WdmUsbPowerSystemUnspecified = 100,
    WdmUsbPowerSystemWorking,
    WdmUsbPowerSystemSleeping1,
    WdmUsbPowerSystemSleeping2,
    WdmUsbPowerSystemSleeping3,
    WdmUsbPowerSystemHibernate,
    WdmUsbPowerSystemShutdown,

    WdmUsbPowerDeviceUnspecified = 200,
    WdmUsbPowerDeviceD0,
    WdmUsbPowerDeviceD1,
    WdmUsbPowerDeviceD2,
    WdmUsbPowerDeviceD3

} WDMUSB_POWER_STATE;

typedef struct _USB_POWER_INFO {

    /* input */
    WDMUSB_POWER_STATE SystemState;
    /* output */
    WDMUSB_POWER_STATE HcDevicePowerState;
    WDMUSB_POWER_STATE HcDeviceWake;
    WDMUSB_POWER_STATE HcSystemWake;

    WDMUSB_POWER_STATE RhDevicePowerState;
    WDMUSB_POWER_STATE RhDeviceWake;
    WDMUSB_POWER_STATE RhSystemWake;

    WDMUSB_POWER_STATE LastSystemSleepState;

    BOOLEAN CanWakeup;
    BOOLEAN IsPowered;

} USB_POWER_INFO, *PUSB_POWER_INFO;

typedef struct _USBUSER_POWER_INFO_REQUEST {

    USBUSER_REQUEST_HEADER Header;
    USB_POWER_INFO PowerInformation;

} USBUSER_POWER_INFO_REQUEST, *PUSBUSER_POWER_INFO_REQUEST;

/****************************************************
    API - Get Controller Information

    Return some information about the controller

    USBUSER_GET_CONTROLLER_INFO_0
****************************************************/

/* these flags indicate features of the HC */

#define USB_HC_FEATURE_FLAG_PORT_POWER_SWITCHING    0x00000001
#define USB_HC_FEATURE_FLAG_SEL_SUSPEND             0x00000002
#define USB_HC_FEATURE_LEGACY_BIOS                  0x00000004

typedef struct _USB_CONTROLLER_INFO_0 {

    ULONG PciVendorId;
    ULONG PciDeviceId;
    ULONG PciRevision;

    ULONG NumberOfRootPorts;

    USB_CONTROLLER_FLAVOR ControllerFlavor;

    ULONG HcFeatureFlags;

} USB_CONTROLLER_INFO_0 , *PUSB_CONTROLLER_INFO_0;

typedef struct _USBUSER_CONTROLLER_INFO_0 {

    USBUSER_REQUEST_HEADER Header;
    USB_CONTROLLER_INFO_0 Info0;

} USBUSER_CONTROLLER_INFO_0, *PUSBUSER_CONTROLLER_INFO_0;

/****************************************************
    API - Get Controller Driver Key

    Returns the driver key in the registry associated
    with this controller.

    The key is returned NULL terminated, KeyLength
    is the length of the key in bytes including the
    UNICODE_NULL

    USBUSER_GET_CONTROLLER_DRIVER_KEY

    API - Get Root Hub Name

    Returns the symbolic name for the root hub on the
    host controller. Length is the length of the name
    in bytes including the NULL.

    USBUSER_GET_ROOTHUB_SYMBOLIC_NAME

----------------------------------------------------

    The following structure is used to return unicode
    names from the port driver for both of these APIs.

****************************************************/

typedef struct _USB_UNICODE_NAME {

    ULONG Length;
    WCHAR String[1];

} USB_UNICODE_NAME, *PUSB_UNICODE_NAME;

typedef struct _USBUSER_CONTROLLER_UNICODE_NAME {

    USBUSER_REQUEST_HEADER Header;
    USB_UNICODE_NAME UnicodeName;

} USBUSER_CONTROLLER_UNICODE_NAME, *PUSBUSER_CONTROLLER_UNICODE_NAME;
#include <poppack.h>

#endif
