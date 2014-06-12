//
// Copyright (c) Citrix Systems, Inc., All rights reserved.
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
#include "usb.h"
#include <usbiodef.h>
#include "wdfusb.h"
#define _USBD_
#include <usbdlib.h>
#undef _USBD_
#include "usbbusif.h"

// Cannot include this
// #include <USBProtocolDefs.h>
// so the following are cutnpasted
/////////////////////////////////////////////////////////////////////
// STANDARD REQUESTS
#define GET_STATUS                          0
#define CLEAR_FEATURE                       1
#define SET_FEATURE                         3
#define SET_ADDRESS                         5
#define GET_DESCRIPTOR                      6
#define SET_DESCRIPTOR                      7
#define GET_CONFIGURATION                   8
#define SET_CONFIGURATION                   9
#define GET_INTERFACE                       10
#define SET_INTERFACE                       11
#define SYNCH_FRAME                         12    


#define USB_INTERFACE_CLASS_HID 0x03
#define USB_DESCRIPTOR_TYPE_HID 0x21 // aka HID_HID_DESCRIPTOR_TYPE

#define USB_CLASS_AUDIO     0x20  // for diagnostic purposes only
#define USB_INTERFACE_CLASS_VIDEO     0x0e
#define USB_INTERFACE_CLASS_AUDIO     0x01


typedef enum _XENUSBD_PIPE_COMMAND {
    XenUsbdPipeControl,
    XenUsbdPipeIsochronous,
    XenUsbdPipeBulk,
    XenUsbdPipeInterrupt,
    XenUsbdPipeReset,
    XenUsbdPipeAbort,
    XenUsbdGetCurrentFrame,
    XenUsbGetSpeed
} XENUSBD_PIPE_COMMAND;

//
// XXX Super?
//
typedef enum _XENUSB_SPEED {
    XenUsbSpeedLow = 1,
    XenUsbSpeedFull,
    XenUsbSpeedHigh
} XENUSB_SPEED;


//
// max string length is 128 WCHAR + NULL
//
#define USB_STRING_ARRAY_LENGTH (128+1)

struct USB_STRING 
{
    UCHAR bLength;
    UCHAR bDescriptorType;
    WCHAR sString[USB_STRING_ARRAY_LENGTH];
};
typedef USB_STRING * PUSB_STRING;

//
// Support for Microsoft OS Descriptor APIs see usb.h
//
// OS_STRING_DESCRIPTOR_INDEX                  0xEE
// MS_OS_STRING_SIGNATURE                      L"MSFT100"
struct OS_DESCRIPTOR_STRING
{
    union 
    {
        OS_STRING  osDescriptor;
        USB_STRING usbString;
    };
};
typedef OS_DESCRIPTOR_STRING *POS_DESCRIPTOR_STRING;

inline 
WDF_USB_CONTROL_SETUP_PACKET
formatOsFeaturePacket(
    UCHAR vendorCode,
    UCHAR interfaceNumber,
    UCHAR pageIndex,
    USHORT featureIndex,
    USHORT length)
{
    WDF_USB_CONTROL_SETUP_PACKET packet;
    
    RtlZeroMemory(&packet, sizeof(WDF_USB_CONTROL_SETUP_PACKET));
    packet.Packet.bm.Byte = 0xC0;
    packet.Packet.bRequest = vendorCode;
    packet.Packet.wValue.Bytes.HiByte = interfaceNumber;
    packet.Packet.wValue.Bytes.LowByte = pageIndex;
    packet.Packet.wIndex.Value = featureIndex;
    packet.Packet.wLength = length;
    return packet;
}

struct OS_FEATURE_HEADER
{
    ULONG  dwLength;
    USHORT bcdVersion; // 0x0100
    USHORT wIndex;     // 0x04
    UCHAR  bCount;
    UCHAR  reserved[7];
};

struct OS_COMPATID_FUNCTION
{
    UCHAR bFirstInterfaceNumber;
    UCHAR reserved;
    UCHAR compatibleID[8];
    UCHAR subCompatibleID[8];
    UCHAR reserved2[6];
};

struct OS_COMPAT_ID
{
    OS_FEATURE_HEADER header;
    OS_COMPATID_FUNCTION functions[1];
};
typedef OS_COMPAT_ID *POS_COMPAT_ID;

struct PIPE_DESCRIPTOR
{
    BOOLEAN valid;
    PUSB_INTERFACE_DESCRIPTOR interfaceDescriptor;
    PUSB_ENDPOINT_DESCRIPTOR  endpoint;
    BOOLEAN intInEndpoint;
    ULONG requestsQueued;
    ULONGLONG lastResponseTime; // KeQueryInterruptTime
    BOOLEAN abortInProgress;
    ULONG abortWaiters;
    KEVENT abortCompleteEvent;
};


struct USB_CONFIG_INFO
{
    PUSB_CONFIGURATION_DESCRIPTOR m_configurationDescriptor;
    ULONG m_numInterfaces;
    ULONG m_numEndpoints;
    //
    // This is an array of interface object pointers
    // one for each interface, alternate and concurrent.
    //
    PUSB_INTERFACE_DESCRIPTOR * m_interfaceDescriptors;
    //
    // A configuration supports up to 32 unique endpoints,
    // 16 in endpoints and 16 out endpoints. Alternate interfaces
    // cannot conflict with other concurrent interface endpoints.
    // m_pipeDescriptors is an array of m_numEndpoints PIPE_DESCRIPTOR objects,
    // one for each possible endpoint in each interface and interface alternate.
    //
    PIPE_DESCRIPTOR * m_pipeDescriptors;
};
typedef USB_CONFIG_INFO * PUSB_CONFIG_INFO;

#define URB_FROM_REQUEST(_r_) (PURB) URB_FROM_IRP(WdfRequestWdmGetIrp(_r_))
