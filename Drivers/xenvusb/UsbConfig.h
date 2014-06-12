//
// Copyright (c) Citrix Systems, Inc., All rights reserved.
//
/// @file UsbConfig.h USB URB and Scratchpad request and response processing definitions.
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

NTSTATUS
GetUsbConfigData(
    IN PUSB_FDO_CONTEXT fdoContext);

void
FreeUsbConfigData(
    IN PUSB_FDO_CONTEXT fdoContext);

void
SetConfigPointers(
    IN PUSB_FDO_CONTEXT fdoContext);

_Requires_lock_held_(fdoContext->WdfDevice)
NTSTATUS
SetCurrentConfigurationLocked(
    IN PUSB_FDO_CONTEXT fdoContext,
    IN UCHAR configValue,
    IN BOOLEAN SetInterface,
    IN USHORT InterfaceNumber,
    IN USHORT AlternateSetting);

_Requires_lock_held_(fdoContext->WdfDevice)
NTSTATUS
GetCurrentConfigurationLocked(
    IN PUSB_FDO_CONTEXT fdoContext);

PUSB_CONFIG_INFO
ConfigInfoByValue(
    IN PUSB_FDO_CONTEXT fdoContext,
    UCHAR Value);

UCHAR 
CurrentConfigValue(
    IN PUSB_FDO_CONTEXT fdoContext);

PUSB_CONFIGURATION_DESCRIPTOR
ConfigByIndex(
    IN PUSB_FDO_CONTEXT fdoContext,
    ULONG Index);

NTSTATUS
ResetDevice(
    IN PUSB_FDO_CONTEXT fdoContext);

BOOLEAN
GetUsbInfo(
    IN PUSB_FDO_CONTEXT FdoContext);

void
SetUsbInfo(
    IN PUSB_FDO_CONTEXT FdoContext,
    IN BOOLEAN enable);

PUSB_ENDPOINT_DESCRIPTOR
PipeHandleToEndpointAddressDescriptor(
    IN PUSB_FDO_CONTEXT fdoContext,
    IN USBD_PIPE_HANDLE PipeHandle);

PIPE_DESCRIPTOR *
EndpointAddressToPipeDescriptor(
    IN PUSB_FDO_CONTEXT fdoContext,
    IN UCHAR EndpointAddress);

PUSB_INTERFACE_DESCRIPTOR
PipeHandleToInterfaceDescriptor(
    IN PUSB_FDO_CONTEXT fdoContext,
    IN USBD_PIPE_HANDLE PipeHandle);

PIPE_DESCRIPTOR *
FindFirstPipeForInterface(
    IN PUSB_FDO_CONTEXT fdoContext,
    PUSB_CONFIG_INFO configInfo,
    PUSB_INTERFACE_DESCRIPTOR pInterfaceDescriptor);

PUSB_INTERFACE_DESCRIPTOR
FindInterface(
    IN PUSB_FDO_CONTEXT fdoContext,
    PUSB_CONFIG_INFO configInfo,
    UCHAR InterfaceNumber,
    UCHAR AlternateSetting);

NTSTATUS
SetInterfaceDescriptorPipes(
    IN PUSB_FDO_CONTEXT fdoContext,
    PUSB_CONFIG_INFO configInfo,
    PUSB_INTERFACE_DESCRIPTOR pInterfaceDescriptor,
    PUSBD_INTERFACE_INFORMATION Interface);

PUSB_INTERFACE_DESCRIPTOR
GetDefaultInterface(
    IN PUSB_FDO_CONTEXT fdoContext);


PCHAR UrbFunctionToString(
    USHORT Function);

PCHAR
UsbFeatureSelectorString(
    USHORT featureSelector);

PCHAR 
DescriptorTypeToString(
    IN UCHAR DescType);

VOID
DbgPrintBuffer(
    PVOID buffer,
    ULONG bytesTransferred,
    ULONG Level,
    ULONG Flag);
