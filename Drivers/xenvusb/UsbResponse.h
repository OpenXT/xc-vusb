//
// Copyright (c) Citrix Systems, Inc., All rights reserved.
//
/// @file UsbResponse.h USB high level response processing.
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

VOID
PostProcessScratch(
    IN PUSB_FDO_CONTEXT fdoContext, 
    IN NTSTATUS usbdStatus,
    IN PCHAR usbifStatusString,
    IN PCHAR usbdStatusString,
    IN ULONG BytesTransferred, 
    IN ULONG Data);

NTSTATUS
PostProcessUrb(
    IN PUSB_FDO_CONTEXT fdoContext,
    IN PURB Urb,
    IN NTSTATUS *usbdStatus,
    IN ULONG bytesTransferred,
    IN ULONG startFrame,
    IN PVOID isoPacketDescriptor);

NTSTATUS
PostProcessSelectConfig(
    IN PUSB_FDO_CONTEXT fdoContext,
    IN PURB Urb);
