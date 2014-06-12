//
// Copyright (c) 2014 Citrix Systems, Inc.
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
//
// for now define our own bogus version of USB_BUS_INTERFACE_HUB_FORWARD_PROGRESS_GUID
// d74bd695 5916 4a8a 9a d6 5b 20 7f 45 30 
//
DEFINE_GUID( USB_BUS_INTERFACE_HUB_FORWARD_PROGRESS_GUID, \
            0xd74bd695L, 0x5916, 0x4a8a, 0x9a, 0xd6, 0x5b, 0x20, 0x7f, 0x45, 0x30, 0x65);


typedef NTSTATUS
    (USB_BUSIFFN *PUSB_BUSIFFN_FP) (
        PVOID Param1
    );

typedef PIO_WORKITEM 
    (USB_BUSIFFN *FP_ALLOCATE_WORK_ITEM) (
        PDEVICE_OBJECT pdo
        );

typedef VOID
    (USB_BUSIFFN * FP_QUEUE_WORKITEM) (
        PDEVICE_OBJECT Device,
        PIO_WORKITEM IoWorkItem,
        PIO_WORKITEM_ROUTINE WorkerRoutine,
        WORK_QUEUE_TYPE QueueType,
        PVOID Context,
        BOOLEAN flag); // If TRUE called internally! else called by Hub.

typedef VOID
    (USB_BUSIFFN * FP_FREE_WORK_ITEM) (
    PIO_WORKITEM IoWorkItem);

_Function_class_(DIRP_CALLBACK_FUNC)
typedef VOID
    (USB_BUSIFFN * DIRP_CALLBACK_FUNC) (
        PDEVICE_OBJECT Device,
        PIRP Irp);

typedef VOID
    (USB_BUSIFFN * FP_DEFER_IRP_PROCESSING) (
        PDEVICE_OBJECT Device,
        DIRP_CALLBACK_FUNC Func,
        PIRP Irp);

typedef struct _USB_BUS_INTERFACE_HUB_FORWARD_PROGRESS {

    USHORT Size;
    USHORT Version;
    
    PVOID BusContext;
    PINTERFACE_REFERENCE InterfaceReference;
    PINTERFACE_DEREFERENCE InterfaceDereference;
    
    // interface specific entries go here

    FP_ALLOCATE_WORK_ITEM AllocateWorkItem;
    FP_FREE_WORK_ITEM FreeWorkItem;
    FP_QUEUE_WORKITEM QueueWorkItem;
    FP_DEFER_IRP_PROCESSING DeferIrpProcessing;

} USB_BUS_INTERFACE_HUB_FORWARD_PROGRESS, *PUSB_BUS_INTERFACE_HUB_FORWARD_PROGRESS;
