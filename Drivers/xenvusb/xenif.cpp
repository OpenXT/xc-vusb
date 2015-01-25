//
// Copyright (c) Citrix Systems, Inc.
//
/// @file xenif.cpp xen ringbuffer and bus access implementation.
/// This module encapsulates and abstracts driver access to the xen
/// DOMU <-> DOM0 "bus".
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
#define __DRIVER_NAME "xenvusb"
#include "Driver.h"
#include "Device.h"
#include "UsbConfig.h"

#define VCI_BUILD

#include <xen.h>
#include <memory.h>
#include <grant_table.h>
#include <event_channel.h>
#include <ring.h>
#include <xenbus.h>
#include "usbxenif.h"
#include "xenif.h"
#include "UsbResponse.h"
#include "UsbRequest.h"
#include "xenlower.h"
#pragma warning(disable : 4127) //conditional expression is constant

// --XT-- XXX TODO the logging was just sort of bolted on to get things
// working. It really needs to be integrated to deal with noisy logging
// in a cleaner way (w/o introducing performance issues). The current
// solution involves a fucntion call for every line of logging.
XXX_TODO("--XT-- Better logging integration.");

#define XEN_BUS L"Xen"
#define RB_VERSION_REQUIRED "3"
#define MAX_ISO_PACKETS (PAGE_SIZE/sizeof(iso_packet_info))

//
/// local context for ringbuffer entry.
//
typedef struct 
{
    usbif_request_t req;                 //<! The associated ringbuffer request
    ULONG           Tag;                 //<! must be 'Shdw'
    BOOLEAN         InUse;               //<! Must be FALSE when unallocated.
    WDFREQUEST      Request;             //<! NULL if internal request
    PMDL            allocatedMdl;        //<! if not NULL an MDL that must be deallocated
    ULONG           length;              //<! ??? figure out if this is used!
    BOOLEAN         isReset;             //<! is this a reset request
    PVOID           isoPacketDescriptor; //<! allocated page for iso packets
    PMDL            isoPacketMdl;        //<! allocated iso packet MDL
    PVOID           indirectPageMemory;  //<! allocated indirect memory
} usbif_shadow_ex_t;

//
/// Device resources allocated for the xen bus interface.
//
struct XEN_BUS_PLATFORM_RESOURCE
{
    PHYSICAL_ADDRESS Address;
    PVOID            SystemVA;
    ULONG            OriginalLength;
    ULONG            Length;
    ULONG            Alloc;
    USHORT           Flags;
};

//
/// This keeps the interface to Xenbus private to this module.
/// Theoretically it could be a separate static or dynamic library component.
/// @todo add a pointer back to the "owner" (e.g. the WDFDEVICE)?
//
struct XEN_INTERFACE
{
    PUSB_FDO_CONTEXT          FdoContext;
    PXEN_LOWER                XenLower; // --XT-- the Xen lower context association

    //
    // Xenstore paths
    //
    CHAR FrontendPath[XEN_LOWER_MAX_PATH]; // --XT-- pulled out of vectors struct
    CHAR BackendPath[XEN_LOWER_MAX_PATH];  // --XT-- pulled out of vectors struct

    //
    // Xen ringbuffer and grant ref interface.
    //
    BOOLEAN                   NxPrepBoot;  // --XT-- always false for XT
    ULONG                     XenifVersion; //!< had better be 3
    ULONG                     DeviceGoneRequestCount;

    usbif_sring *             Sring; //!< shared ring
    usbif_front_ring_t        Ring;  //!< front ring
    ULONG                     RequestsOnRingbuffer; //!< data URBs only
    PMDL                      SringPage; // --XT-- added to track the mapped page
    
    ULONG                     MaxIsoSegments;
    ULONG                     MaxSegments;

    usbif_shadow_ex_t *       Shadows;
    ULONG                     ShadowArrayEntries;
    USHORT *                  ShadowFreeList;
    USHORT                    ShadowFree;
    USHORT                    ShadowMinFree;


    BOOLEAN                   IndirectGrefSupport; //!< has to be true!
};


//
// local declarations.
//
static BOOLEAN
InitXenDevice(
    IN PXEN_INTERFACE Xen,
    IN PUCHAR *ptr);

static BOOLEAN
ConnectXenDevice(
    IN PXEN_INTERFACE Xen,
    IN PUCHAR *ptr);


static BOOLEAN
PutGrantOnFreelist(
    IN PXEN_INTERFACE Xen,
    IN grant_ref_t grant);

static grant_ref_t
GetGrantFromFreelist(
    IN PXEN_INTERFACE Xen);

static usbif_shadow_ex_t *
GetShadowFromFreeList(
    IN PXEN_INTERFACE Xen);

static VOID
PutShadowOnFreelist(
    IN PXEN_INTERFACE Xen,
    IN usbif_shadow_ex_t *shadow);

static BOOLEAN
AllocateGrefs(
    IN PXEN_INTERFACE Xen,
    IN usbif_shadow_ex_t *shadow,
    IN PPFN_NUMBER pfnArray,
    IN ULONG PagesUsed);

static BOOLEAN
AllocateIndirectGrefs(
    IN PXEN_INTERFACE Xen,
    IN usbif_shadow_ex_t * Shadow,
    IN ULONG IndirectPagesNeeded,
    IN PMDL Mdl,
    IN PMDL IndirectPageMdl,
    IN ULONG PagesUsed,
    IN PPFN_NUMBER PacketPfnArray);

static VOID
PutOnRing(
    IN PXEN_INTERFACE Xen,
    IN usbif_shadow_ex_t *shadow);

static VOID
DecrementRingBufferRequests(
    IN PXEN_INTERFACE Xen);

static VOID
PutShadowOnFreelist(
    IN PXEN_INTERFACE Xen,
    IN usbif_shadow_ex_t *shadow);


//
// Implementation.
//

static PMDL
XenAllocatePage(VOID)
{
    PMDL mdl;
    PVOID buf;

    buf = ExAllocatePoolWithTag(NonPagedPool, PAGE_SIZE, XVU9);
    if (buf == NULL)
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
            __FUNCTION__ ": page allocation failed\n");
        return NULL;
    }

    mdl = (PMDL)ExAllocatePoolWithTag(NonPagedPool, MmSizeOfMdl(buf, PAGE_SIZE), XVU9);
    if (mdl == NULL)
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
            __FUNCTION__ ": MDL allocation failed\n");
        ExFreePoolWithTag(buf, XVU9);
        return NULL;
    }
  
    MmInitializeMdl(mdl, buf, PAGE_SIZE);
    MmBuildMdlForNonPagedPool(mdl);

    return mdl;
}

static VOID
XenFreePage(PMDL Mdl)
{
    PVOID buf = MmGetMdlVirtualAddress(Mdl);

    ExFreePoolWithTag(Mdl, XVU9);
    ExFreePoolWithTag(buf, XVU9);
}

static BOOLEAN
XenGetPaths(
    IN PXEN_INTERFACE Xen)
{
    NTSTATUS status;

    status = RtlStringCchCopyA(Xen->FrontendPath,
        sizeof(Xen->FrontendPath),
        XenLowerGetFrontendPath(Xen->XenLower));;
    if (status != STATUS_SUCCESS)
    {
        Xen->FrontendPath[0] = 0;
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
            __FUNCTION__ ": to copy front end path - status: 0x%x\n", status);
        return FALSE;
    }

    status = RtlStringCchCopyA(Xen->BackendPath,
        sizeof(Xen->BackendPath),
        XenLowerGetBackendPath(Xen->XenLower));;
    if (status != STATUS_SUCCESS)
    {
        Xen->BackendPath[0] = 0;
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
            __FUNCTION__ ": to copy back end path - status: 0x%x\n", status);
        return FALSE;
    }

    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DEVICE,
        __FUNCTION__ ": Fetched xenstore paths - Front: %s Back: %s\n",
        Xen->FrontendPath, Xen->BackendPath);

    return TRUE;
}

static BOOLEAN
XenCreateSring(
    IN PXEN_INTERFACE Xen)
{
    // Unfortunately this had to be yanked out of the xenpci bus driver
    // but it is needed for the init below.
    struct dummy_sring {
        RING_IDX req_prod, req_event;
        RING_IDX rsp_prod, rsp_event;
        uint8_t  pad[48];
    };

    PMDL ring = XenAllocatePage();
    PVOID address;

    if (!ring)
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
            __FUNCTION__ ": failed to allocate sring page.\n");
        return FALSE;
    }

    address = MmGetMdlVirtualAddress(ring);
    SHARED_RING_INIT((struct dummy_sring *)address);

    if (!XenLowerGetSring(Xen->XenLower, (uint32_t)*MmGetMdlPfnArray(ring)))
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
            __FUNCTION__ ": Xen config incomplete no ringbuffer\n");
        XenFreePage(ring);
        return FALSE;
    }

    Xen->Sring = (usbif_sring *)address;
    Xen->SringPage = ring;

    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DEVICE,
        __FUNCTION__ ": Setup shared ring: %p\n", Xen->Sring);

    return TRUE;
}

static VOID
XenInterfaceCleanup(
    IN PXEN_INTERFACE Xen)
{
    if (Xen->SringPage)
    {
        XenFreePage(Xen->SringPage);
    }

    if (Xen->Shadows)
    {
        ExFreePool(Xen->Shadows);
        Xen->Shadows = NULL;
    }

    if (Xen->ShadowFreeList)
    {
        ExFreePool(Xen->ShadowFreeList);
        Xen->ShadowFreeList = NULL;
    }
}

PXEN_INTERFACE
AllocateXenInterface(
    PUSB_FDO_CONTEXT fdoContext)
{
    PXEN_INTERFACE xen = (PXEN_INTERFACE) 
        ExAllocatePoolWithTag(NonPagedPool, sizeof(XEN_INTERFACE), XVU9);
    if (xen)
    {
        RtlZeroMemory(xen, sizeof(XEN_INTERFACE));
        xen->FdoContext = fdoContext;

        xen->XenLower = XenLowerAlloc();
        if (!xen->XenLower)
        {
            ExFreePool(xen);
            return NULL;
        }
    }
    return xen;
}

VOID
DeallocateXenInterface(
    IN PXEN_INTERFACE Xen)
{
    XenLowerFree(Xen->XenLower);
    // XXX TODO do we want to clean all this up on shutdown?
    XenInterfaceCleanup(Xen);
    ExFreePool(Xen);
}

NTSTATUS
XenDeviceInitialize(
    IN PXEN_INTERFACE Xen,
    IN PEVTCHN_HANDLER_CB DpcCallback)
{
    ULONG version;
    NTSTATUS status = STATUS_SUCCESS;
    PDEVICE_OBJECT pdo;
    BOOLEAN rc;
    ULONG i;

    do {
        pdo = WdfDeviceWdmGetPhysicalDevice(Xen->FdoContext->WdfDevice);
        if (!pdo)
        {
            TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
                __FUNCTION__ ": failed to get the PDO device\n");
            status = STATUS_UNSUCCESSFUL;
            break;
        }

        // Init XenLower
        rc = XenLowerInit(Xen->XenLower, Xen, pdo);
        if (!rc)
        {
            TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
                __FUNCTION__ ": failed to initialize the Xen Lower interface\n");
            status = STATUS_UNSUCCESSFUL;
            break;
        }

        // Init XenLower backend values
        rc = XenLowerBackendInit(Xen->XenLower);
        if (!rc)
        {
            TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
                __FUNCTION__ ": failed to initialize the Xen Lower backend values\n");
            status = STATUS_UNSUCCESSFUL;
            break;
        }

        // Get the interface version - only 3 is supported. This also sets up the
        // the frontend version information which is also 3.
        version = XenLowerInterfaceVersion(Xen->XenLower);
        if (version != XEN_LOWER_INTERFACE_VERSION)
        {
            TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
                __FUNCTION__ ": interface version %d unsupported\n",
                version);
            status = STATUS_NOT_IMPLEMENTED;
            break;
        }

        TraceEvents(TRACE_LEVEL_VERBOSE, TRACE_DEVICE,
            __FUNCTION__ ": version = %d\n",
            version);

        Xen->XenifVersion = version;
        Xen->IndirectGrefSupport = TRUE;
        Xen->MaxIsoSegments = USBIF_URB_MAX_ISO_SEGMENTS;
        Xen->MaxSegments = USBIF_URB_MAX_SEGMENTS_PER_REQUEST;

        Xen->ShadowFree = 0;

        // Somewhere around here the initial setup code called XenPci_XenConfigDevice
        // which setup the backend. This involved setting up all the values in the
        // "registers" reported in the memory resource. This made those values available
        // to the setup code to fetch. It then start the front-back state machine and
        // start connecting. The XT xenbus code does this rather differently.

        rc = XenGetPaths(Xen);
        if (!rc)
        {
            TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
                __FUNCTION__ ": failed to get xenstore paths\n");
            status = STATUS_UNSUCCESSFUL;
            break;
        }        

        //
        // Set the front end path in the caller's context.
        //
        status = RtlStringCchCopyA(Xen->FdoContext->FrontEndPath,
            sizeof(Xen->FdoContext->FrontEndPath),
            Xen->FrontendPath);
        if ((status == STATUS_SUCCESS) || (status == STATUS_BUFFER_OVERFLOW))
        {
            int port = 0;
            sscanf_s(Xen->FdoContext->FrontEndPath, "device/vusb/%d", &port);
            Xen->FdoContext->Port = (USHORT)port;

            TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DEVICE,
                __FUNCTION__ ": %s port %d returns success\n",
                Xen->FdoContext->FrontEndPath,
                Xen->FdoContext->Port);
        }
        else
        {
            Xen->FdoContext->FrontEndPath[0] = 0;
            TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
                __FUNCTION__": FrontEndPath invalid, error %x\n",
                status);
            break;
        }

        // The event channel is not setup here. It is setup later where the KMDF code in
        // Device.cpp would have hooked up the ISR to the shared interrupt. In this case
        // we will just directly wire the DPC routine into the EC processor ISR in our xenbus.

        // Grant refs are not allocate during initialization. They are fetched by the ring
        // processing code below via Vectors.GntTbl_GetRef. Removing those bits.

        rc = XenCreateSring(Xen);
        if (!rc)
        {
            TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
                __FUNCTION__ ": Xen config incomplete no ringbuffer\n");
            status = STATUS_UNSUCCESSFUL;
            break;
        }
        FRONT_RING_INIT(&Xen->Ring, Xen->Sring, PAGE_SIZE);

        Xen->ShadowArrayEntries = SHADOW_ENTRIES;
        Xen->Shadows = (usbif_shadow_ex_t *)ExAllocatePoolWithTag(NonPagedPool,
            sizeof(usbif_shadow_ex_t)* SHADOW_ENTRIES,
            XVUA);
        if (!Xen->Shadows)
        {
            TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
                __FUNCTION__ ": Xen config shadow allocation failure\n");
            status = STATUS_NO_MEMORY;
            break;
        }

        Xen->ShadowFreeList = (PUSHORT)ExAllocatePoolWithTag(NonPagedPool,
            sizeof(USHORT)* SHADOW_ENTRIES,
            XVUB);
        if (!Xen->ShadowFreeList)
        {        
            TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
                __FUNCTION__ ": Xen config shadow free list allocation failure\n");
            status = STATUS_NO_MEMORY;
            break;
        }

        //
        // set up the mapping from shadow request to request/respons through
        // the request.id field.
        //
        memset(Xen->Shadows, 0, sizeof(usbif_shadow_t)* SHADOW_ENTRIES);
        for (i = 0; i < SHADOW_ENTRIES; i++)
        {
            Xen->Shadows[i].req.id = i;
            Xen->Shadows[i].Tag = SHADOW_TAG;
            Xen->Shadows[i].InUse = TRUE;
            PutShadowOnFreelist(Xen, &Xen->Shadows[i]);
        }
        TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DEVICE,
            __FUNCTION__ ": Fetched shared ring and setup shadows\n");

        //
        // Setup the event channel DPC here. It will not be active
        // until the backend is connected.
        //
        rc = XenLowerConnectEvtChnDPC(Xen->XenLower,
            DpcCallback, Xen->FdoContext);
        if (!rc)
        {
            TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
                __FUNCTION__ ": failed to connect event channel to DPC\n");
            status = STATUS_UNSUCCESSFUL;
            break;
        }
        TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DEVICE,
            __FUNCTION__ ": Setup event channel DPC callback: %p\n", DpcCallback);

    } while (FALSE);

    if (status != STATUS_SUCCESS)
    {
        XenInterfaceCleanup(Xen);
    }

    return status;
}

RESUME_HANDLER_CB XenDeviceResumeConnectBackend;

NTSTATUS
XenDeviceConnectBackend(
    IN PXEN_INTERFACE Xen)
{
    NTSTATUS status;
    BOOLEAN rc;

    // Start the backend connect, wait for it to be initialized and
    // ready to proceed. Write the xenstore values and then connect.
    status = XenLowerConnectBackend(Xen->XenLower, XenDeviceResumeConnectBackend);
    if (!NT_SUCCESS(status))
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
            __FUNCTION__ ": failed to connect backend\n");
        return status;
    }

    // The connect routine re-reads backend values for resume or chaning
    // backend device domains. Update the upper lever path values here.
    rc = XenGetPaths(Xen);
    if (!rc)
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
            __FUNCTION__ ": failed to get xenstore paths\n");
        return STATUS_UNSUCCESSFUL;
    }
    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DEVICE,
        __FUNCTION__ ": Xen backend connected!\n");

    return STATUS_SUCCESS;
}

VOID
XenDeviceResumeConnectBackend(PXEN_LOWER, VOID*)
{
}

VOID
XenScheduleDPC(
    IN PXEN_INTERFACE Xen)
{
    XenLowerScheduleEvtChnDPC(Xen->XenLower);
}

VOID
XenDisconnectDPC(
    IN PXEN_INTERFACE Xen)
{
    XenLowerDisconnectEvtChnDPC(Xen->XenLower);
}

VOID
XenDeviceDisconnectBackend(
    IN PXEN_INTERFACE Xen)
{
    XenLowerDisonnectBackend(Xen->XenLower);
}

void 
FreeShadowForRequest(
    IN PXEN_INTERFACE Xen,
    WDFREQUEST Request)
{
    for (ULONG index = 0;
        index < SHADOW_ENTRIES;
        index++)
    {
        if (Request == Xen->Shadows[index].Request)
        {
            TraceEvents(TRACE_LEVEL_WARNING, TRACE_DEVICE,
                __FUNCTION__ ": Device %p Found backend Request %p shadow %p index %d requests on ringbuffer: %d\n",
                Xen->FdoContext->WdfDevice,
                Request,
                &Xen->Shadows[index],
                index,
                OnRingBuffer(Xen));
            //
            // reclaim any shadow resources
            //
            Xen->Shadows[index].Request = NULL;
            PutShadowOnFreelist(Xen, &Xen->Shadows[index]);
            DecrementRingBufferRequests(Xen);
            break;
        }
    }
}

static VOID
PutShadowOnFreelist(
    IN PXEN_INTERFACE Xen,
    IN usbif_shadow_ex_t *shadow)
{
    uint8_t index;
    ASSERT(shadow->InUse == TRUE);
    if (!shadow->InUse)
    {
        TraceEvents(TRACE_LEVEL_WARNING, TRACE_DEVICE,
            __FUNCTION__ ": shadow %d not in use!\n",
            shadow->req.id);
        return;
    }

    if (shadow->isoPacketMdl)
    {
        IoFreeMdl(shadow->isoPacketMdl);
        shadow->isoPacketMdl = NULL;
    }
    if (shadow->allocatedMdl)
    {
        IoFreeMdl(shadow->allocatedMdl);
        shadow->allocatedMdl = NULL;
    }
    if (shadow->isoPacketDescriptor)
    {
        ExFreePool(shadow->isoPacketDescriptor);
        shadow->isoPacketDescriptor = NULL;
    }
    //
    // Free the grant refs allocated to this request.
    //
    if (shadow->indirectPageMemory)
    {
        usbif_indirect_page_t * indirectPage = 
            (usbif_indirect_page_t *) shadow->indirectPageMemory;

        for (index = 0; index <shadow->req.nr_segments; index++)
        {
            for (uint32_t index2 = 0;
                index2 < indirectPage[index].nr_segments;
                index2++)
            {
                if (!PutGrantOnFreelist(Xen, indirectPage[index].gref[index2]))
                {
                    TraceEvents(TRACE_LEVEL_WARNING, TRACE_DEVICE,
                        __FUNCTION__": leaked grant ref %p for indirect data segment\n",
                        indirectPage[index].gref[index2]);
                }
            }
        }
        ExFreePool(shadow->indirectPageMemory);
        shadow->indirectPageMemory = NULL;
    }
    for(index = 0; index < shadow->req.nr_segments; index++)
    {
        if (!PutGrantOnFreelist(Xen, shadow->req.gref[index]))
        {
            TraceEvents(TRACE_LEVEL_WARNING, TRACE_DEVICE,
                __FUNCTION__": leaked grant ref %p for data segment\n",
                shadow->req.gref[index]);
        }
    }
    shadow->req.nr_segments = 0;
    shadow->Request = NULL;
    shadow->InUse = FALSE;

    ASSERT(Xen->ShadowFree < Xen->ShadowArrayEntries);
    if (Xen->ShadowFree >= Xen->ShadowArrayEntries)
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
            __FUNCTION__": ShadowFree %d >= ShadowArrayEntries %d!\n",
            Xen->ShadowFree,
            Xen->ShadowArrayEntries);
        return;
    }
    Xen->ShadowFreeList[Xen->ShadowFree] = (USHORT)shadow->req.id;
    Xen->ShadowFree++;
}

void
CompleteRequestsFromShadow(
    IN PUSB_FDO_CONTEXT fdoContext)
{
    ULONG RequestsProcessed = 0;

    AcquireFdoLock(fdoContext);;

    TraceEvents(TRACE_LEVEL_VERBOSE, TRACE_DEVICE,
        __FUNCTION__": Device %p %d requests on ringbuffer\n",
        fdoContext->WdfDevice,
        OnRingBuffer(fdoContext->Xen));
    
    for (ULONG index = 0;
        index < SHADOW_ENTRIES;
        index++)
    {
        WDFREQUEST Request = fdoContext->Xen->Shadows[index].Request;
        if (Request)
        {
            PFDO_REQUEST_CONTEXT requestContext = RequestGetRequestContext(Request);
            RequestsProcessed++;

            if (requestContext->RequestCompleted)
            {
                // this shouldn't happen!

                TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
                    __FUNCTION__": Device %p Request %p Completed! Should not happen.\n",
                    fdoContext->WdfDevice,
                    Request);
                WdfObjectDereference(Request);
                continue;
            }
            //
            // Remove the cancel state from this request and deal with the
            // error case.
            //
            if (requestContext->CancelSet)
            {
                requestContext->CancelSet = 0;
                NTSTATUS Status = WdfRequestUnmarkCancelable(Request);
                if (!NT_SUCCESS(Status))
                {
                    if (Status == STATUS_CANCELLED)
                    {
                        //
                        // don't complete the request here it is owned by the
                        // cancel routine.
                        //
                        TraceEvents(TRACE_LEVEL_WARNING, TRACE_DEVICE,
                            __FUNCTION__": Device %p Request %p Cancelled\n",
                            fdoContext->WdfDevice,
                            Request);
                        WdfObjectDereference(Request);
                        continue;
                    }
                    //
                    // note but ignore.
                    //
                    TraceEvents(TRACE_LEVEL_WARNING, TRACE_DEVICE,
                        __FUNCTION__": Device %p Request %p WdfRequestUnmarkCancelable error %x\n",
                        fdoContext->WdfDevice,
                        Request,
                        Status);
                }  
            }
            //
            // deallocate any resources acquired for this IRP.
            //
            requestContext->RequestCompleted = 1;
            FreeShadowForRequest(fdoContext->Xen, Request);            
            WdfObjectDereference(Request);

            ReleaseFdoLock(fdoContext);
            WdfRequestComplete(Request, STATUS_DEVICE_DOES_NOT_EXIST);
            AcquireFdoLock(fdoContext);;

            TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DEVICE,
                __FUNCTION__": Device %p completing on hardware Request %p\n",
                fdoContext->WdfDevice,
                Request);
        }
    }
    
    ReleaseFdoLock(fdoContext);

    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DEVICE,
        __FUNCTION__": Device %p removed %d requests from ringbuffer\n",
        fdoContext->WdfDevice,
        RequestsProcessed);
}

static usbif_shadow_ex_t *
GetShadowFromFreeList(
    IN PXEN_INTERFACE Xen)
{
    if (Xen->ShadowFree == 0)
    {
        return NULL;
    }
    Xen->ShadowFree--;
    if (Xen->ShadowFree < Xen->ShadowMinFree)
    {
        Xen->ShadowMinFree = Xen->ShadowFree;
    }
    ASSERT(Xen->Shadows[Xen->ShadowFreeList[Xen->ShadowFree]].InUse == FALSE);
    Xen->Shadows[Xen->ShadowFreeList[Xen->ShadowFree]].InUse = TRUE;
    Xen->Shadows[Xen->ShadowFreeList[Xen->ShadowFree]].req.nr_segments = 0;
    Xen->Shadows[Xen->ShadowFreeList[Xen->ShadowFree]].req.nr_packets = 0;
    Xen->Shadows[Xen->ShadowFreeList[Xen->ShadowFree]].req.flags = 0;
    Xen->Shadows[Xen->ShadowFreeList[Xen->ShadowFree]].req.length = 0;
    return &Xen->Shadows[Xen->ShadowFreeList[Xen->ShadowFree]];
}

static grant_ref_t
GetGrantFromFreelist(
    IN PXEN_INTERFACE Xen)
{
    UNREFERENCED_PARAMETER(Xen);

    return XenLowerGntTblGetRef();
}

static BOOLEAN
PutGrantOnFreelist(
    IN PXEN_INTERFACE Xen,
    IN grant_ref_t grant)
{
    UNREFERENCED_PARAMETER(Xen);

    //
    // N. B. The underlying implementation for the XT xenbus does nothing
    // with the keepref parameter. Since it is always FALSE, this is fine.
    // If it needs to be TRUE at any point, the underlying grant code will 
    // have to be modified.
    //
    return XenLowerGntTblEndAccess(grant);
}

/**
 * @brief allocates grantrefs for a data transfer for standard (not indirect) ringbuffer transfers.
 * 
 * _Note_ AllocateGrefs can be called more than once for a single transfer operation and uses
 * usbif_shadow_ex_t.req.nr_segments to track where it is in the usbif_shadow_ex_t.req.gref array
 * of allocated grefs.
 *
 * @param[in] Xen. The Xen context.
 * @param[in] shadow. Pointer to the usbif_shadow_ex_t allocated for this transfer.
 * @param[in] pfnArray. The PPFN_NUMBER array for the data transfer.
 * @param[in] PagesUsed. The size of pfnArray.
 *
 * @returns BOOLEAN success or failure.
 */
static BOOLEAN
AllocateGrefs(
    IN PXEN_INTERFACE Xen,
    IN usbif_shadow_ex_t *shadow,
    IN PPFN_NUMBER pfnArray,
    IN ULONG PagesUsed)
{
    ULONG index;

    TraceEvents(TRACE_LEVEL_VERBOSE, TRACE_DEVICE,
            __FUNCTION__ "==> PagesUsed: %d (0x%x)\n", PagesUsed, PagesUsed);

    for (index = 0;
        index < PagesUsed;
        index++)
    {
        PFN_NUMBER pfn = pfnArray[index];
        grant_ref_t gref = GetGrantFromFreelist(Xen);
        if (gref == INVALID_GRANT_REF)
        {
            TraceEvents(TRACE_LEVEL_WARNING, TRACE_DEVICE,
                __FUNCTION__ "GetGrantFromFreelist failed\n");
            return FALSE;
        }     
        shadow->req.gref[shadow->req.nr_segments] = gref;

        XenLowerGntTblGrantAccess(
            0,
            (uint32_t) pfn,
            0,
            shadow->req.gref[shadow->req.nr_segments]);

        shadow->req.nr_segments++;
        TraceEvents(TRACE_LEVEL_VERBOSE, TRACE_DEVICE,
            __FUNCTION__ "Mapped PFN(%d): %d (0x%x)\n", index, pfn, pfn);
    }

    TraceEvents(TRACE_LEVEL_VERBOSE, TRACE_DEVICE,
            __FUNCTION__ "<==\n");
    return TRUE;
}

/**
 * @brief allocates the grant refs for an transfer using the indirect gref 
 * mechanism.
 * This expands the ringbuffer per-transfer capacity by allocating g'refd memory
 * in the guest to hold additional grefs for data pages beyond the 
 * ringbuffer capacity of 17 pages.
 *
 * @param[in] Xen. The Xen interface context.
 * @param[in] Shadow. Pointer to an allocated usbif_shadow_ex_t object, AllocateIndirectGrefs()
 *  modifies the nr_segments field to record the number of allocated grefs in usbif_shadow_ex_t.indirectPageMemory
 * @param[in] IndirectPagesNeeded. How many indirect pages are required.
 * @param[in] Mdl. The Mdl for the data page.
 * @param[in] IndirectPageMdl. The MDL for the indirect pages.
 * @param[in] PagesUsed. Number of data pages being transferred.
 * @param[in] PacketPfnArray. Optional. If an Iso packet the first data page is the ISO packets.
 * 
 * @returns BOOLEAN success or failure.
 */
static BOOLEAN
AllocateIndirectGrefs(
    IN PXEN_INTERFACE Xen,
    IN usbif_shadow_ex_t * Shadow,
    IN ULONG IndirectPagesNeeded,
    IN PMDL Mdl,
    IN PMDL IndirectPageMdl,
    IN ULONG PagesUsed,
    IN PPFN_NUMBER PacketPfnArray)
{
    ASSERT(Shadow);
    Shadow->req.nr_segments = 0;
    //
    // req.nr_segments is the number of indirect gref pages we need, and that is
    //  ( PagesUsed / 1023 )
    // 
    PPFN_NUMBER pfnArray = MmGetMdlPfnArray(IndirectPageMdl);
    //
    // set up the descriptors for the indirect pages in the request.
    //
    if (!AllocateGrefs(Xen, Shadow, pfnArray, IndirectPagesNeeded))
    {
        return FALSE;
    }
    //
    // now build the descriptors for the data pages pointed to
    // by the indirect pages.
    //
    usbif_indirect_page_t * indirectPages = (usbif_indirect_page_t *) Shadow->indirectPageMemory;
    ASSERT(indirectPages != NULL);

    ULONG indirectIndex = 0;
    ULONG indirectArrayIndex = 0;
    ULONG pfnIndex = 0;
    pfnArray = MmGetMdlPfnArray(Mdl);
    indirectPages[0].nr_segments = 0;

    if (PacketPfnArray)
    {
        //
        // set up the descriptor for the iso packets - it is the first page 
        // of the first indirect page. We have to fill in pagesUsed + 1 pages;
        // The first gref of the first page points to the iso packet descriptor page.
        //
        indirectPages[0].gref[0] = GetGrantFromFreelist(Xen);
        if (indirectPages[0].gref[0] == INVALID_GRANT_REF)
        {
            return FALSE;
        }
        XenLowerGntTblGrantAccess(
            0,
            (uint32_t) PacketPfnArray[0], 
            0, 
            indirectPages[0].gref[0]);

        indirectPages[0].nr_segments = 1;
        indirectIndex = 1; 
    }
    //
    // now set up the data grefs.
    //
    while ( pfnIndex < PagesUsed )
    {
        indirectPages[indirectArrayIndex].gref[indirectIndex] = GetGrantFromFreelist(Xen);
        if (indirectPages[indirectArrayIndex].gref[indirectIndex]  == INVALID_GRANT_REF)
        {
            return FALSE;
        }

        XenLowerGntTblGrantAccess(          
            0, 
            (uint32_t) pfnArray[pfnIndex], 
            0, 
            indirectPages[indirectArrayIndex].gref[indirectIndex]);
        
        pfnIndex++;
        indirectPages[indirectArrayIndex].nr_segments++;
        indirectIndex++;
        if (indirectIndex == 1023)
        {
            indirectArrayIndex++;
            indirectPages[indirectArrayIndex].nr_segments = 0;
            indirectIndex = 0;
        }
    }
    return TRUE;
}

static VOID
PutRequest(
    IN PXEN_INTERFACE Xen,
    usbif_request_t *shadowReq)
{
    usbif_request_t *req = RING_GET_REQUEST(&Xen->Ring, Xen->Ring.req_prod_pvt);
    if (NT_VERIFY(req))
    {
        memcpy(req,
            shadowReq,
            sizeof(usbif_request_t));

        // XXX STUB: print level too high
        TraceEvents(TRACE_LEVEL_VERBOSE, TRACE_DEVICE,
            __FUNCTION__": req %p index %d id %I64d pipetype %x endpointId %x\n"
            "           offset %x length %x segments %x flags %x packets %x startframe %x\n",
            req,
            Xen->Ring.req_prod_pvt,
            req->id,
            req->type,
            req->endpoint,
            req->offset,
            req->length,
            req->nr_segments,
            req->flags,
            req->nr_packets,
            req->startframe);
        Xen->Ring.req_prod_pvt++;
        Xen->RequestsOnRingbuffer++;
    }
}

static VOID
PutOnRing(
    IN PXEN_INTERFACE Xen,
    IN usbif_shadow_ex_t *shadow)
{
    int notify;
    PutRequest(Xen, &shadow->req);
    RING_PUSH_REQUESTS_AND_CHECK_NOTIFY(&Xen->Ring, notify);
    // --XT-- Lower context is holding on the the EC port, arg not used.
    XenLowerEvtChnNotify(Xen->XenLower);
}

//
// One level up in the abstraction, deliver various USB requests
// to the Xen interface using "PutOnRing".
//

NTSTATUS
PutScratchOnRing(
    IN PUSB_FDO_CONTEXT fdoContext,
    IN PWDF_USB_CONTROL_SETUP_PACKET packet,
    IN ULONG TransferLength,
    IN USBD_PIPE_TYPE PipeType,
    IN UCHAR EndpointAddress,
    IN BOOLEAN isReset)
{
    ULONG offset = 0;
    PPFN_NUMBER pfnArray = NULL;
    ULONG pagesUsed = 0;
    UCHAR shortPacketOk = REQ_SHORT_PACKET_OK;

    if (AvailableRequests(fdoContext->Xen) == 0) //allow scratch requests to use the last entry
    {
        return STATUS_UNSUCCESSFUL;
    }
    if (isReset)
    {        
        if (packet)
        {
            TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
                __FUNCTION__": %s reset with packet?\n",
                fdoContext->FrontEndPath);
            return STATUS_INVALID_PARAMETER;
        }
        if (TransferLength)
        {
            TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
                __FUNCTION__": %s reset with data?\n",
                fdoContext->FrontEndPath);
            return STATUS_INVALID_PARAMETER;
        }
    }
    if (TransferLength)
    {
        pagesUsed =  ADDRESS_AND_SIZE_TO_SPAN_PAGES(
            fdoContext->ScratchPad.Buffer,
            TransferLength);
        offset = MmGetMdlByteOffset(fdoContext->ScratchPad.Mdl);
        pfnArray = MmGetMdlPfnArray(fdoContext->ScratchPad.Mdl);
    }       
    usbif_shadow_ex_t *shadow = GetShadowFromFreeList(fdoContext->Xen);
    ASSERT(shadow);
    ASSERT(shadow->Tag == SHADOW_TAG);
    if (isReset)
    {
        shadow->Request = NULL;
        shadow->req.endpoint = 0;
        shadow->req.type = 0;
        shadow->req.length = 0;
        shadow->req.offset = 0;
        shadow->req.nr_segments = 0;
        shadow->req.flags = RESET_TARGET_DEVICE;
        shadow->allocatedMdl = NULL;
        shadow->req.setup = 0L;
        shadow->isReset = TRUE;
    }
    else
    {
        shadow->Request = NULL;
        shadow->req.endpoint = EndpointAddress;
        shadow->req.type = (uint8_t) PipeType;
        shadow->req.length = (usbif_request_len_t) TransferLength;
        ASSERT(offset < 0x00010000);
        shadow->req.offset = (uint16_t) offset;
        ASSERT(pagesUsed < 0x100);
        shadow->req.nr_segments = 0;
        shadow->allocatedMdl = NULL;
        shadow->req.flags = shortPacketOk;
        RtlCopyMemory(&shadow->req.setup, packet, sizeof(shadow->req.setup));
    }
    if (!AllocateGrefs(fdoContext->Xen, shadow, pfnArray, pagesUsed))
    {
        PutShadowOnFreelist(fdoContext->Xen, shadow);
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
            __FUNCTION__": %s no grefs.\n",
            fdoContext->FrontEndPath);
        return STATUS_UNSUCCESSFUL;
    }
    PutOnRing(fdoContext->Xen, shadow);

    return STATUS_SUCCESS;
}

_Requires_lock_held_(fdoContext->WdfDevice)
BOOLEAN
WaitForScratchPadAccess(
    IN PUSB_FDO_CONTEXT fdoContext)
{
    
    ULONG Count = 0;
    while (fdoContext->ConfigBusy && Count < 5)
    {
        //
        // wait one second and try again
        //            
        ReleaseFdoLock(fdoContext);
        LARGE_INTEGER  delay;
        delay.QuadPart = WDF_REL_TIMEOUT_IN_SEC(1);
        KeDelayExecutionThread(
            KernelMode,
            FALSE,
            &delay);
        Count++;
        AcquireFdoLock(fdoContext);;

        if (fdoContext->DeviceUnplugged)
        {
            TraceEvents(TRACE_LEVEL_WARNING, TRACE_DEVICE,
                __FUNCTION__ ": Device %p While waiting for config access, already unplugged bail\n",
                fdoContext->WdfDevice);
            return FALSE;
        }
    }
    fdoContext->ConfigBusy = TRUE;
    return TRUE;
}

NTSTATUS
WaitForScratchCompletion(
    IN PUSB_FDO_CONTEXT fdoContext)
{
    ULONG WaitCounter = 0;
    LARGE_INTEGER Timeout;
    NTSTATUS Status = STATUS_SUCCESS;

    while ( WaitCounter < 5)
    {
        if (fdoContext->DeviceUnplugged)
        {
            break;
        }

        Timeout.QuadPart = WDF_REL_TIMEOUT_IN_SEC( 2 );
        Status = KeWaitForSingleObject(
            &fdoContext->ScratchPad.CompletionEvent,
            Executive,
            KernelMode,
            FALSE,
            &Timeout);

        if (Status == STATUS_TIMEOUT)
        {
            WaitCounter++;
            TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
                __FUNCTION__": %s wait timeout %d unplugged %d\n",
                fdoContext->FrontEndPath,
                WaitCounter,
                fdoContext->DeviceUnplugged);
            Status = STATUS_UNSUCCESSFUL;

            if (fdoContext->DeviceUnplugged)
            {
                break;
            }
        }
        else if (NT_SUCCESS(Status))
        {            
            if (fdoContext->DeviceUnplugged)
            {
                Status = STATUS_UNSUCCESSFUL;
            }
            break;
        }
        else
        {
            TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
                __FUNCTION__": %s wait failed %x\n",
                fdoContext->FrontEndPath,
                Status);
            Status = STATUS_UNSUCCESSFUL;
            break;
        }
    }
    KeClearEvent(&fdoContext->ScratchPad.CompletionEvent);
    if (!NT_SUCCESS(Status))
    {
        //
        // this device is busted.
        //
        AcquireFdoLock(fdoContext);
        FdoUnplugDevice(fdoContext);
        ReleaseFdoLock(fdoContext);
    }
    return Status;
}

//
// Put various requests onto the ring buffer.
//
_Requires_lock_held_(fdoContext->WdfDevice)
VOID
PutResetOrCycleUrbOnRing(
    IN PUSB_FDO_CONTEXT fdoContext,
    IN WDFREQUEST Request,
    IN BOOLEAN IsReset) // RESET_TARGET_DEVICE (T)  or CYCLE_PORT (F)
{
    usbif_shadow_ex_t *shadow = NULL;    
    NTSTATUS Status = STATUS_UNSUCCESSFUL;
    if (!Request)
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
            __FUNCTION__": %s reset with no Request?\n",
            fdoContext->FrontEndPath);
        return;
    }

    TRY
    {
        //
        // cycle or reset are allowed to consume the last request.
        // 
        if (!AvailableRequests(fdoContext->Xen))
        {
            TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
                __FUNCTION__": %s no available request\n",
                fdoContext->FrontEndPath);

            RequeueRequest(fdoContext, Request);
            Request = NULL;
            Status = STATUS_UNSUCCESSFUL;
            LEAVE;
        }

        shadow = GetShadowFromFreeList(fdoContext->Xen);
        ASSERT(shadow);
        if (!shadow)
        {
            TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
                __FUNCTION__": Device %p No shadows for request?\n",
                fdoContext->WdfDevice);

            Status = STATUS_UNSUCCESSFUL;
            LEAVE;
        }
        
        Status = WdfRequestMarkCancelableEx(Request,
            EvtFdoOnHardwareRequestCancelled);

        if (!NT_SUCCESS(Status))
        {
            TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
                __FUNCTION__": Device %p Request %p WdfRequestMarkCancelableEx error %x\n",
                fdoContext->WdfDevice,
                Request,
                Status);
            LEAVE;
        }
        RequestGetRequestContext(Request)->CancelSet = 1; 
        //
        // Add a reference to the Request when it is going to the ringbuffer.
        //
        WdfObjectReference(Request);
        fdoContext->ResetInProgress = TRUE;

        uint8_t ResetOrCycle = IsReset ? RESET_TARGET_DEVICE : CYCLE_PORT;
        shadow->Request = Request;
        shadow->req.endpoint = 0;
        shadow->req.type = 0;
        shadow->req.length = 0;
        shadow->req.offset = 0;
        shadow->req.nr_segments = 0;
        shadow->req.flags = ResetOrCycle;
        shadow->allocatedMdl = NULL;
        shadow->req.setup = 0L;
        shadow->isReset = TRUE;

        TraceEvents(TRACE_LEVEL_VERBOSE, TRACE_DEVICE,
            __FUNCTION__":request: id %I64d type %d endpoint %x length %x offset %x nr_segs %d\n",
            shadow->req.id,
            shadow->req.type,
            shadow->req.endpoint,
            shadow->req.length,
            shadow->req.offset,
            shadow->req.nr_segments);

        PutOnRing(fdoContext->Xen, shadow);
    }
    FINALLY
    {
        if (Status != STATUS_SUCCESS)
        {
            if (shadow)
            {          
                PutShadowOnFreelist(fdoContext->Xen, shadow);
            }
            if (Request)
            {
                //
                // Complete the request.
                //
                TraceEvents(TRACE_LEVEL_VERBOSE, TRACE_DEVICE,
                    __FUNCTION__": Device %p Request %p shadow %p Cleanup request status %x\n",
                    fdoContext->WdfDevice,
                    Request,
                    shadow,
                    Status);
                
                RequestGetRequestContext(Request)->RequestCompleted = 1; 
                ReleaseFdoLock(fdoContext);
                WdfRequestComplete(Request, Status);
                AcquireFdoLock(fdoContext);;
            }
        }
    }
}

/**
 * @brief Puts a request on the Xen ringbuffer.
 * *Must be called with lock held*
 * *Must Complete, requeue, or put the request on the ringbuffer.*
 * 
 */
_Requires_lock_held_(fdoContext->WdfDevice)
VOID
PutUrbOnRing(
    IN PUSB_FDO_CONTEXT fdoContext,
    IN PWDF_USB_CONTROL_SETUP_PACKET packet,
    IN WDFREQUEST Request,
    IN USBD_PIPE_TYPE PipeType,
    IN UCHAR EndpointAddress,
    IN BOOLEAN hasData,
    IN BOOLEAN shortOk)
{
    ULONG transferLength = 0;
    ULONG offset = 0;
    PMDL Mdl = NULL;
    ULONG pagesUsed  = 0;
    PPFN_NUMBER pfnArray = NULL;
    BOOLEAN mdlAllocated = FALSE;
    PVOID buffer;
    usbif_shadow_ex_t *shadow = NULL;
    PURB Urb = NULL;
    PMDL IndirectPageMdl = NULL;
    
    NTSTATUS Status = STATUS_UNSUCCESSFUL;

    TRY
    {
        if (!Request)
        {
            TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
                __FUNCTION__": %s Urb with no Request?\n",
                fdoContext->FrontEndPath);
            Status = STATUS_INVALID_PARAMETER;
            LEAVE;
        }        
        //
        // reserve one request for reset of all requests
        // 
        if (AvailableRequests(fdoContext->Xen) <= 1)
        {
            RequeueRequest(fdoContext, Request);
            Request = NULL;
            Status = STATUS_UNSUCCESSFUL;
            LEAVE;
        }

        Urb = (PURB) URB_FROM_IRP(WdfRequestWdmGetIrp(Request));  

        shadow = GetShadowFromFreeList(fdoContext->Xen);
        ASSERT(shadow);
        if (!shadow)
        {
            TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
                __FUNCTION__": Device %p No shadows for request?\n",
                fdoContext->WdfDevice);

            Status = STATUS_UNSUCCESSFUL;
            LEAVE;
        }
        if (shortOk)
        {
            if (USB_ENDPOINT_DIRECTION_OUT(EndpointAddress))
            {
                TraceEvents(TRACE_LEVEL_WARNING, TRACE_DEVICE,
                    __FUNCTION__": %s Out packet with shortok!\n",
                    fdoContext->FrontEndPath);
            }

            shadow->req.flags = REQ_SHORT_PACKET_OK;
        }

        if (hasData)
        {
            //
            // this is probably not the best way to do get the length, but it works
            // for all urbs.
            //
            transferLength = Urb->UrbBulkOrInterruptTransfer.TransferBufferLength;
        }

        if (transferLength)
        {
            //
            // we have to handle the case where the usb function driver has
            // sent us a request with no MDL.
            // The rules are one or the other, either a TransferBufferMDL
            // is supplied or a TransferBuffer is supplied, but not both.
            // If we allocate the MDL then we store it in TransferBufferMDL
            // and we have to deallocate it when we complete the request.
            // On completion if TransferBuffer and TransferBufferMDL then
            // deallocate the MDL.
            //
            Mdl = Urb->UrbBulkOrInterruptTransfer.TransferBufferMDL;
            buffer = Urb->UrbBulkOrInterruptTransfer.TransferBuffer;
            
            if (Mdl)
            {
                pagesUsed = ADDRESS_AND_SIZE_TO_SPAN_PAGES(MmGetMdlVirtualAddress(Mdl), transferLength);

            }
            else if (buffer)
            {
                //
                // we have to allocate it
                //
                Mdl = IoAllocateMdl(
                    buffer,
                    transferLength,
                    FALSE,
                    FALSE,
                    NULL);

                if (Mdl)
                {
                    MmBuildMdlForNonPagedPool(Mdl);
                    mdlAllocated = TRUE;
                }
                else
                {
                    TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
                        __FUNCTION__": %s cannot allocate mdl for transfer\n",
                        fdoContext->FrontEndPath);
                    Status = STATUS_UNSUCCESSFUL;
                    LEAVE;
                }

                pagesUsed =  ADDRESS_AND_SIZE_TO_SPAN_PAGES(
                    MmGetMdlVirtualAddress(Mdl),
                    transferLength);
            }
            else
            {
                // no buffer and no mdl? what?
                TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
                    __FUNCTION__": %s no buffer and no mdl for Urb %p function %s (%x)\n",
                    fdoContext->FrontEndPath,
                    Urb,
                    UrbFunctionToString(Urb->UrbHeader.Function),
                    Urb->UrbHeader.Function);
                Status = STATUS_UNSUCCESSFUL;
                LEAVE;
            }
            
            offset = MmGetMdlByteOffset(Mdl);

            if (pagesUsed > MaxSegments(fdoContext->Xen))
            {
                //
                // BULK Indirect support. 
                //
                if (PipeType != UsbdPipeTypeBulk)
                {
                    TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
                        __FUNCTION__": %s pagesUsed: %d greater than max allowed %d!\n",
                        fdoContext->FrontEndPath,
                        pagesUsed,
                        MaxSegments(fdoContext->Xen));

                    Status = STATUS_UNSUCCESSFUL;
                    LEAVE;
                }
                if (pagesUsed > MAX_PAGES_FOR_INDIRECT_REQUEST)
                {
                    TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
                        __FUNCTION__": %s pagesUsed: %d greater than MAX_INDIRECT_PAGES %d\n",
                        fdoContext->FrontEndPath,
                        pagesUsed,
                        MAX_INDIRECT_PAGES);

                    Status = STATUS_UNSUCCESSFUL;
                    LEAVE;
                }
                
                ULONG indirectPagesNeeded = INDIRECT_PAGES_REQUIRED(pagesUsed );
                ASSERT(indirectPagesNeeded <= MAX_INDIRECT_PAGES);
#pragma warning(push)
#pragma warning(disable: 28197)
                shadow->indirectPageMemory = ExAllocatePoolWithTag(
                    NonPagedPool,
                    (PAGE_SIZE * indirectPagesNeeded),
                    XVUC);
#pragma warning(pop)

                if (!shadow->indirectPageMemory )
                {
                    TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
                        __FUNCTION__": %s Request %d no memory for indirect pages, failing request\n",
                        fdoContext->FrontEndPath,
                        Request);

                    Status = STATUS_INSUFFICIENT_RESOURCES;
                    LEAVE;
                }           
                RtlZeroMemory(shadow->indirectPageMemory, (PAGE_SIZE * indirectPagesNeeded));

                IndirectPageMdl = IoAllocateMdl(shadow->indirectPageMemory,
                    (PAGE_SIZE * indirectPagesNeeded),
                    FALSE,
                    FALSE,
                    NULL);
                if (!IndirectPageMdl)
                {
                    TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
                        __FUNCTION__": %s Request %p can't allocate indirect MDL, failing request\n",
                        fdoContext->FrontEndPath,
                        Request);
                    Status = STATUS_INSUFFICIENT_RESOURCES;
                    LEAVE;
                }
                MmBuildMdlForNonPagedPool(IndirectPageMdl);

                if (!AllocateIndirectGrefs(
                    fdoContext->Xen,
                    shadow,
                    indirectPagesNeeded, 
                    Mdl,
                    IndirectPageMdl,
                    pagesUsed,
                    NULL))
                {
                    TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
                        __FUNCTION__": %s AllocateIndirectGrefs failed\n",
                        fdoContext->FrontEndPath);

                    Status = STATUS_INSUFFICIENT_RESOURCES;
                    LEAVE;
                }
                shadow->req.flags |= INDIRECT_GREF;
            }
            else
            {
                pfnArray = MmGetMdlPfnArray(Mdl);
                if (!AllocateGrefs(fdoContext->Xen, shadow, pfnArray, pagesUsed))
                {
                    RequeueRequest(fdoContext, Request);
                    Request = NULL;
                    Status = STATUS_UNSUCCESSFUL;
                    LEAVE;
                }
            }
        }
        //
        // If we get here this request is going to DOM0.
        // Mark the request as cancelable, unfortunately this step can fail.
        //        
        Status = WdfRequestMarkCancelableEx(Request,
            EvtFdoOnHardwareRequestCancelled);

        if (!NT_SUCCESS(Status))
        {
            TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
                __FUNCTION__": Device %p Request %p WdfRequestMarkCancelableEx error %x\n",
                fdoContext->WdfDevice,
                Request,
                Status);
            //
            // we own the request. Cleanup and complete below.
            //
            LEAVE;
        }
        RequestGetRequestContext(Request)->CancelSet = 1; 
        //
        // Add a reference to the Request when it is going to the ringbuffer.
        //
        WdfObjectReference(Request);

        Status = STATUS_SUCCESS;

        shadow->isReset = FALSE;
        shadow->Request = Request;
        ASSERT(Urb->UrbHeader.Function < 0x100);
        shadow->req.endpoint = EndpointAddress;
        shadow->req.type = (uint8_t) PipeType;
        shadow->req.length = (usbif_request_len_t) transferLength;
        ASSERT(offset < 0x00010000);
        shadow->req.offset = (uint16_t) offset;
        shadow->allocatedMdl = mdlAllocated ? Mdl : NULL; // remember we allocated this mdl.
        shadow->isoPacketDescriptor = NULL;
        RtlCopyMemory(&shadow->req.setup, packet, sizeof(shadow->req.setup));

        mdlAllocated = FALSE; // so we do not clean it up 

        if (shadow->indirectPageMemory)
        {
            fdoContext->totalIndirectTransfers++;
        }
        else
        {
            fdoContext->totalDirectTransfers++;
        }

        PutOnRing(fdoContext->Xen, shadow);
        //
        // DOM0 owns the request for now.
        //
        Request = NULL;
    }
    FINALLY
    {
        //
        // If request is not NULL it has to be completed here.
        // If Status is not STATUS_SUCCESS the allocated resources have
        // to be freed.
        //
        if (IndirectPageMdl)
        {
            IoFreeMdl(IndirectPageMdl);
        }
        if (Status != STATUS_SUCCESS)
        {
            if (shadow)
            {          
                PutShadowOnFreelist(fdoContext->Xen, shadow);
            }

            if (Mdl && mdlAllocated)
            {                
                IoFreeMdl(Mdl);
            }

            if (Request)
            {
                //
                // Complete the request.
                //
                TraceEvents(TRACE_LEVEL_VERBOSE, TRACE_DEVICE,
                    __FUNCTION__": Device %p Request %p shadow %p Mdl %p mdlAllocated %d Cleanup request status %x\n",
                    fdoContext->WdfDevice,
                    Request,
                    shadow,
                    Mdl,
                    mdlAllocated,
                    Status);
                
                RequestGetRequestContext(Request)->RequestCompleted = 1; 
                ReleaseFdoLock(fdoContext);
                WdfRequestComplete(Request, Status);
                AcquireFdoLock(fdoContext);;
            }
        }
        else
        {
            ASSERT(Request == NULL);
        }
        return;
    }
}


//
// Must be called with device lock held
//
//
_Requires_lock_held_(fdoContext->WdfDevice)
VOID
PutIsoUrbOnRing(
    IN PUSB_FDO_CONTEXT fdoContext,
    IN PWDF_USB_CONTROL_SETUP_PACKET packet,
    IN WDFREQUEST Request,
    IN UCHAR EndpointAddress,
    IN BOOLEAN transferAsap,
    IN BOOLEAN ShortOK) 
{
    ULONG transferLength = 0;
    ULONG offset = 0;
    PMDL Mdl = NULL;
    ULONG pagesUsed  = 0;
    PPFN_NUMBER pfnArray = NULL;
    BOOLEAN mdlAllocated = FALSE;
    PVOID buffer;
    ULONG numberOfPackets;
    iso_packet_info * packetBuffer = NULL;
    PMDL packetMdl = NULL; 
    usbif_shadow_ex_t *shadow = NULL;
    PURB Urb = NULL;
    PMDL IndirectPageMdl = NULL;
    //
    // reserve one request for cancellation of all requests
    // 
    NTSTATUS Status = STATUS_UNSUCCESSFUL;

    TRY
    {
        if (!Request)
        {
            LEAVE;
        }        
        Urb = (PURB) URB_FROM_IRP(WdfRequestWdmGetIrp(Request));

        if (AvailableRequests(fdoContext->Xen) <= 1)
        {
            RequeueRequest(fdoContext, Request);
            Request = NULL;
            Status = STATUS_UNSUCCESSFUL;
            LEAVE;
        }

        shadow = GetShadowFromFreeList(fdoContext->Xen);
        ASSERT(shadow);
        if (!shadow)
        {
            TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
                __FUNCTION__": Device %p No shadows for request?\n",
                fdoContext->WdfDevice);

            Status = STATUS_UNSUCCESSFUL;
            LEAVE;
        }

        transferLength = Urb->UrbIsochronousTransfer.TransferBufferLength;
        numberOfPackets = Urb->UrbIsochronousTransfer.NumberOfPackets; 

        packetBuffer = (iso_packet_info *)  ExAllocatePoolWithTag(NonPagedPool,
            PAGE_SIZE, XVUD);

        if (!packetBuffer)
        {
            TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
                __FUNCTION__": %s packet buffer allocation failed\n",
                fdoContext->FrontEndPath);

            Status = STATUS_UNSUCCESSFUL;
            LEAVE;
        }

        packetMdl = IoAllocateMdl(packetBuffer,
            PAGE_SIZE,
            FALSE,
            FALSE,
            NULL);
        if (!packetMdl)
        {
            TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
                __FUNCTION__": %s packet buffer mdl allocation failed\n",
                fdoContext->FrontEndPath);
            LEAVE;
        }
        MmBuildMdlForNonPagedPool(packetMdl);
        //
        // set up the transfer the packet descriptors 
        //
        //
        ULONG segLength32 = Urb->UrbIsochronousTransfer.TransferBufferLength / numberOfPackets;

        for (ULONG Index = 0; 
            Index < numberOfPackets;
            Index++)
        {
            //
            /// set the offsets and lengths.
            //
            packetBuffer[Index].offset = Urb->UrbIsochronousTransfer.IsoPacket[Index].Offset;
            packetBuffer[Index].length = (uint16_t) segLength32;
            packetBuffer[Index].status = 0;
        }

        TraceEvents(TRACE_LEVEL_VERBOSE, TRACE_DEVICE,
            __FUNCTION__": transferLength %d packets %d\n",
            transferLength,
            numberOfPackets);
        //
        // we have to handle the case where the usb function driver has
        // sent us a request with no MDL.
        // The rules are one or the other, either a TransferBufferMDL
        // is supplied or a TransferBuffer is supplied, but not both.
        // If we allocate the MDL then we store it in TransferBufferMDL
        // and we have to deallocate it when we complete the request.
        // On completion if TransferBuffer and TransferBufferMDL then
        // deallocate the MDL.
        //
        Mdl = Urb->UrbIsochronousTransfer.TransferBufferMDL;
        buffer = Urb->UrbIsochronousTransfer.TransferBuffer;
        if (buffer && Mdl)
        {
            //
            // This is not an error. Use the MDL.
            //
            TraceEvents(TRACE_LEVEL_VERBOSE, TRACE_DEVICE,
                __FUNCTION__": buffer and Mdl specified!\n");
        }
        else if (Mdl && !buffer)
        {
            // normal.
        }
        else if (buffer && !Mdl)
        {
            //
            // we have to allocate it
            //
            Mdl = IoAllocateMdl(
                buffer,
                transferLength,
                FALSE,
                FALSE,
                NULL);

            if (Mdl)
            {
                MmBuildMdlForNonPagedPool(Mdl);            
                mdlAllocated = TRUE;
            }
            else
            {
                TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
                    __FUNCTION__": %s cannot allocate mdl for transfer\n",
                    fdoContext->FrontEndPath);
                Status = STATUS_UNSUCCESSFUL;
                LEAVE;
            }
        }
        else
        {
            // no buffer and no mdl? what?            
            TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
                __FUNCTION__": %s no buffer and no mdl for Urb %p function %s (%x)\n",
                fdoContext->FrontEndPath,
                Urb,
                UrbFunctionToString(Urb->UrbHeader.Function),
                Urb->UrbHeader.Function);

            Status = STATUS_UNSUCCESSFUL;
            LEAVE;
        }
        //
        // get the offset into the data payload.
        //
        offset = MmGetMdlByteOffset(Mdl);

        PPFN_NUMBER packetPfnArray = MmGetMdlPfnArray(packetMdl);

        pagesUsed =  ADDRESS_AND_SIZE_TO_SPAN_PAGES(
            MmGetMdlVirtualAddress(Mdl), 
            transferLength);

        if (pagesUsed > MaxIsoSegments(fdoContext->Xen))
        {
            //
            // indirect gref required.
            //
            if (pagesUsed > MAX_PAGES_FOR_INDIRECT_ISO_REQUEST)
            {
                //
                // this should never happen.
                //
                TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
                    __FUNCTION__": %s pagesUsed: %d greater than MAX_INDIRECT_PAGES %d\n",
                    fdoContext->FrontEndPath,
                    pagesUsed,
                    MAX_PAGES_FOR_INDIRECT_ISO_REQUEST);

                Status = STATUS_INSUFFICIENT_RESOURCES;
                LEAVE;
            }

            ULONG indirectPagesNeeded = INDIRECT_PAGES_REQUIRED(pagesUsed + 1); // + 1 for the iso packet page
            ASSERT(indirectPagesNeeded <= MAX_INDIRECT_PAGES);
#pragma warning(push)
#pragma warning(disable: 28197)
            shadow->indirectPageMemory = ExAllocatePoolWithTag(
                NonPagedPool,
                (PAGE_SIZE * indirectPagesNeeded),
                XVUE);
#pragma warning(pop)

            if (!shadow->indirectPageMemory)
            {
                TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
                    __FUNCTION__": %s Request %d no memory for indirect pages, failing request\n",
                    fdoContext->FrontEndPath,
                    Request);

                Status = STATUS_INSUFFICIENT_RESOURCES;
                LEAVE;
            }           
            RtlZeroMemory(shadow->indirectPageMemory, (PAGE_SIZE * indirectPagesNeeded));

            IndirectPageMdl = IoAllocateMdl(shadow->indirectPageMemory,
                (PAGE_SIZE * indirectPagesNeeded),
                FALSE,
                FALSE,
                NULL);
            if (!IndirectPageMdl)
            {
                TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
                    __FUNCTION__": %s Request %p can't allocate indirect MDL, failing request\n",
                    fdoContext->FrontEndPath,
                    Request);
                Status = STATUS_INSUFFICIENT_RESOURCES;
                LEAVE;
            }
            MmBuildMdlForNonPagedPool(IndirectPageMdl);            

            if (!AllocateIndirectGrefs(
                fdoContext->Xen,
                shadow,
                indirectPagesNeeded, 
                Mdl,
                IndirectPageMdl,
                pagesUsed,
                packetPfnArray))
            {
                Status = STATUS_INSUFFICIENT_RESOURCES;
                LEAVE;
            }
            //
            // If we get here this request is going to DOM0.
            // Mark the request as cancelable, unfortunately this step can fail.
            //
            Status = WdfRequestMarkCancelableEx(Request,
                EvtFdoOnHardwareRequestCancelled);

            if (!NT_SUCCESS(Status))
            {
                TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
                    __FUNCTION__": Device %p Request %p WdfRequestMarkCancelableEx error %x\n",
                    fdoContext->WdfDevice,
                    Request,
                    Status);
                //
                // we own the request. Cleanup and complete below.
                //
                LEAVE;
            }
            RequestGetRequestContext(Request)->CancelSet = 1; 
            //
            // Add a reference to the Request when it is going to the ringbuffer.
            //
            WdfObjectReference(Request);

            //
            // Use INDIRECT_GREF
            //

            shadow->isReset = FALSE;
            shadow->Request = Request;
            shadow->req.endpoint = EndpointAddress;
            shadow->req.type = (uint8_t) UsbdPipeTypeIsochronous;
            shadow->req.length = (usbif_request_len_t) transferLength;

            ASSERT(offset < 0x00010000);
            shadow->req.offset = (uint16_t) offset;

            shadow->req.flags = INDIRECT_GREF | (ShortOK ? REQ_SHORT_PACKET_OK : 0) | (transferAsap ? ISO_FRAME_ASAP : 0);
            shadow->req.nr_packets = (uint16_t) numberOfPackets;
            shadow->req.startframe = Urb->UrbIsochronousTransfer.StartFrame;
            shadow->isoPacketDescriptor = packetBuffer;

            shadow->allocatedMdl = mdlAllocated ? Mdl : NULL;
            RtlCopyMemory(&shadow->req.setup, packet, sizeof(shadow->req.setup));
            fdoContext->totalIndirectTransfers++;

            PutOnRing(fdoContext->Xen, shadow);
            Request = NULL;
            LEAVE;
        }
              
        //
        // set up the descriptor for the iso packets
        //
        pfnArray = MmGetMdlPfnArray(Mdl);
        if (!AllocateGrefs(fdoContext->Xen,
            shadow,
            packetPfnArray,
            1))
        {
            TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
                __FUNCTION__": %s Request %p gref exhaustion, requeuing\n",
                fdoContext->FrontEndPath,
                Request);
            LEAVE;
        }
        //
        // set up the descriptors for the data buffer
        //
        if (!AllocateGrefs(fdoContext->Xen,
            shadow,
            pfnArray,
            pagesUsed))
        {
            TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
                __FUNCTION__": %s Request %p gref exhaustion, requeuing\n",
                fdoContext->FrontEndPath,
                Request);
            LEAVE;
        }
        //
        // If we get here this request is going to DOM0.
        // Mark the request as cancelable, unfortunately this step can fail.
        //        
        Status = WdfRequestMarkCancelableEx(Request,
            EvtFdoOnHardwareRequestCancelled);

        if (!NT_SUCCESS(Status))
        {
            TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
                __FUNCTION__": Device %p Request %p WdfRequestMarkCancelableEx error %x\n",
                fdoContext->WdfDevice,
                Request,
                Status);
            //
            // we own the request. Cleanup and complete below.
            //
            LEAVE;
        }
        RequestGetRequestContext(Request)->CancelSet = 1; 
        //
        // Add a reference to the Request when it is going to the ringbuffer.
        //
        WdfObjectReference(Request);
        //
        // payload fits directly into the xen usb request.
        //
        shadow->isReset = FALSE;
        shadow->Request = Request;
        ASSERT(Urb->UrbHeader.Function < 0x100);
        shadow->req.endpoint = EndpointAddress;
        shadow->req.type = (uint8_t) UsbdPipeTypeIsochronous;
        shadow->req.length = (usbif_request_len_t) transferLength;
        ASSERT(offset < 0x00010000);
        shadow->req.offset = (uint16_t) offset; // offset for first data packet!
        ASSERT(pagesUsed < 0x100);
        shadow->req.flags = (ShortOK ? REQ_SHORT_PACKET_OK : 0) | (transferAsap ? ISO_FRAME_ASAP : 0);
        shadow->req.nr_packets = (uint16_t) numberOfPackets;
        shadow->req.startframe = Urb->UrbIsochronousTransfer.StartFrame;
        shadow->isoPacketDescriptor = packetBuffer;

        shadow->allocatedMdl = mdlAllocated ? Mdl : NULL;

        RtlCopyMemory(&shadow->req.setup, packet, sizeof(shadow->req.setup));
        TraceEvents(TRACE_LEVEL_VERBOSE, TRACE_DEVICE,
                __FUNCTION__": request: id %I64d type %d endpoint %x length %x offset %x nr_segs %d\n",
            shadow->req.id,
            shadow->req.type,
            shadow->req.endpoint,
            shadow->req.length,
            shadow->req.offset,
            shadow->req.nr_segments);

        fdoContext->totalDirectTransfers++;

        PutOnRing(fdoContext->Xen, shadow);
        Request = NULL;
    }
    FINALLY
    {
        //
        // cleanup
        //
        if (IndirectPageMdl)
        {
            IoFreeMdl(IndirectPageMdl);
        }
        if (packetMdl)
        {                
            IoFreeMdl(packetMdl);
        }

        if (Status != STATUS_SUCCESS)
        {
            // error cleanup
            if (shadow)
            {
                PutShadowOnFreelist(fdoContext->Xen, shadow);
            }
            else
            {
                if (packetBuffer)
                {
                    ExFreePool(packetBuffer); 
                } 
                if (mdlAllocated)
                {
                    // have to get rid of this one                
                    IoFreeMdl(Mdl);
                }
            }           
            if (Request)
            {
                //
                // Complete the request.
                //
                TraceEvents(TRACE_LEVEL_VERBOSE, TRACE_DEVICE,
                    __FUNCTION__": Device %p Request %p shadow %p Mdl %p mdlAllocated %d Cleanup request status %x\n",
                    fdoContext->WdfDevice,
                    Request,
                    shadow,
                    Mdl,
                    mdlAllocated,
                    Status);
                
                RequestGetRequestContext(Request)->RequestCompleted = 1; 
                ReleaseFdoLock(fdoContext);
                WdfRequestComplete(Request, Status);
                AcquireFdoLock(fdoContext);;
            }
        }
        else
        { 
            ASSERT(Request == NULL);
        }
    }
}

//
// --XT-- Removed ISR and DPC processing.
//

static usbif_response_t *
GetResponse(
    IN PXEN_INTERFACE Xen,
    IN int index)
{
    usbif_response_t * rsp = RING_GET_RESPONSE(&Xen->Ring, index);
    if (NT_VERIFY(rsp))
    {
        TraceEvents(TRACE_LEVEL_VERBOSE, TRACE_DPC,
            __FUNCTION__": %p id %I64d status %x bytesTransferred %x\n",
            rsp,
            rsp->id,
            rsp->status,
            rsp->bytesTransferred);
    }
    return rsp;
}

static void
TraceUsbIfRequest(
    IN PUSB_FDO_CONTEXT fdoContext,
    IN usbif_request_t * request)
{
    WDF_USB_CONTROL_SETUP_PACKET packet;
    RtlCopyMemory(&packet, &request->setup, sizeof(packet));
    TraceEvents(TRACE_LEVEL_VERBOSE, TRACE_URB,
        "%s Device %p usbif_request %p\n"
        "  id %x type %x endpoint %x\n"
        "  offset %x length %x nr_segments %x\n"
        "  flags %x nr_packets %x startframe %x\n"
        "  bm %x brequest %x wValue %x wIndex %x wLength %x\n",
        fdoContext->FrontEndPath, fdoContext->WdfDevice, request,
        request->id, request->type, request->endpoint,
        request->offset, request->length, request->nr_segments,
        request->flags, request->nr_packets, request->startframe,
        packet.Packet.bm.Byte, packet.Packet.bRequest,
        packet.Packet.wValue.Value, packet.Packet.wIndex.Value,
        packet.Packet.wLength);
}


/**
 * @brief DPC handler for XEN interface.
 * Process all completed requests on the ringbuffer and hand them back to the caller as 
 * a WDFCOLLECTION of requests. The caller completes the requests.
 * __called with device lock held__
 *
 * @param[in] fdoContext the context for the USB Controller FDO.
 * @param[in] handle to a WDFCOLLECTION for requests processed by this function.
 *
 * @return TRUE if there are more entries on the ringbuffer, FALSE if the ringbuffer is empty.
 */
_Requires_lock_held_(fdoContext->WdfDevice)
BOOLEAN
XenDpc(
    IN PUSB_FDO_CONTEXT fdoContext,
    IN WDFCOLLECTION RequestCollection)
{
    RING_IDX index, rp;
    rp = fdoContext->Xen->Ring.sring->rsp_prod;
    ULONG responsesProcessed = 0;
    RING_IDX moreWork = FALSE;

    KeMemoryBarrier();
    for (index = fdoContext->Xen->Ring.rsp_cons; index != rp; index++)
    {
        NTSTATUS NtStatus = STATUS_SUCCESS;
        if (fdoContext->DeviceUnplugged)
        {
            //
            /// let somebody else clean up the mess.
            //
            TraceEvents(TRACE_LEVEL_ERROR, TRACE_DPC,
                __FUNCTION__": %s device %p DeviceUnplugged set.\n",
                fdoContext->FrontEndPath,
                fdoContext->WdfDevice);
            return FALSE;
        }

        responsesProcessed++;
        usbif_response_t *response =  GetResponse(fdoContext->Xen, index);

        usbif_shadow_ex_t *shadow = &fdoContext->Xen->Shadows[response->id];
        ASSERT(shadow->Tag == SHADOW_TAG);
        if (!shadow->InUse)
        {
            //
            // handled by cancel side.
            //
            ASSERT(shadow->Request == NULL);
            TraceEvents(TRACE_LEVEL_WARNING, TRACE_URB,
                __FUNCTION__": %s shadow %p request %p already processed\n",
                fdoContext->FrontEndPath,
                shadow,
                shadow->Request);
            continue;
        }

        WDFREQUEST Request = shadow->Request;

        PCHAR usbifStatusString = "UnknownUsbIf";
        PCHAR usbdStatusString = "";

        NTSTATUS usbdStatus = MapUsbifToUsbdStatus(
            shadow->isReset,
            response->status,
            &usbifStatusString,
            &usbdStatusString);

        if (usbdStatus == USBD_STATUS_DEVICE_GONE)
        {
            //
            // this device has been removed
            // fail all new IO requests!
            //
            fdoContext->Xen->DeviceGoneRequestCount++;
            fdoContext->PortConnected = FALSE;
            if ((fdoContext->Xen->DeviceGoneRequestCount % 100) == 1)
            {
                TraceEvents(TRACE_LEVEL_ERROR, TRACE_DPC,
                    __FUNCTION__": %s device unplugged request %p count %d detected by USBD_STATUS_DEVICE_GONE (%s)\n",
                    fdoContext->FrontEndPath,
                    Request,
                    fdoContext->Xen->DeviceGoneRequestCount,
                    usbifStatusString);
            }
        }
        //
        // deal with the cancel race here.
        //
        if (Request)
        {
            PFDO_REQUEST_CONTEXT requestContext = RequestGetRequestContext(Request);
            if (requestContext->CancelSet && !requestContext->RequestCompleted)
            {
                //
                // Remove the cancel state from this request.
                //
                requestContext->CancelSet = 0;
                NtStatus = WdfRequestUnmarkCancelable(Request);
                if (NtStatus == STATUS_CANCELLED)
                {
                    // defer to the cancel callback routine.
                    // @TODO debug print too high
                    TraceEvents(TRACE_LEVEL_WARNING, TRACE_URB,
                        __FUNCTION__": %s Request %p owned by cancel routine\n",
                        fdoContext->FrontEndPath,
                        Request);
                    WdfObjectDereference(Request);
                    Request = NULL;
                    continue;
                }
                else if (!NT_SUCCESS(NtStatus))
                {
                    TraceEvents(TRACE_LEVEL_ERROR, TRACE_DPC,
                        __FUNCTION__": Device %p Request %p WdfRequestUnmarkCancelable error %x\n",
                        fdoContext->WdfDevice,
                        Request,
                        NtStatus);

                    XXX_TODO("Ignoring bad status from WdfRequestUnmarkCancelable");
                }
            }
            if (requestContext->RequestCompleted)
            {
                // defer to the cancel callback routine.
                // @TODO debug print too high
                TraceEvents(TRACE_LEVEL_WARNING, TRACE_URB,
                    __FUNCTION__": %s Request %p already completed\n",
                    fdoContext->FrontEndPath,
                    Request);
                WdfObjectDereference(Request);
                Request = NULL;
                continue;
            }

            DecrementRingBufferRequests(fdoContext->Xen);
            //
            // The DPC owns the request.
            //
            shadow->Request = NULL;
            //
            // remove our hardware reference to the Request object.
            //
            WdfObjectDereference(Request);

            if (shadow->isReset)
            {
                //
                // resets always work.
                //
                TraceEvents(TRACE_LEVEL_VERBOSE, TRACE_DPC,
                    __FUNCTION__": %s reset completion\n",
                    fdoContext->FrontEndPath);
                NtStatus = STATUS_SUCCESS;
                fdoContext->ResetInProgress = FALSE;
                //
                // the Hub PDO needs to transition the port
                // to Enabled state if it is not already there.
                //
                KeSetEvent(&fdoContext->resetCompleteEvent, IO_NO_INCREMENT, FALSE);
            }
            else
            {
                PURB Urb = URB_FROM_REQUEST(Request);
                Urb->UrbHeader.Status = usbdStatus;

                if (response->status == USBIF_RSP_USB_INVALID)
                {
                    //
                    // debug this.
                    //
                    TraceUsbIfRequest(fdoContext, &shadow->req);
                }

                NtStatus = PostProcessUrb(
                    fdoContext,
                    Urb, 
                    &usbdStatus, 
                    response->bytesTransferred,
                    response->data,
                    shadow->isoPacketDescriptor);

                if (!NT_SUCCESS(NtStatus))
                {
                    if (shadow->indirectPageMemory)
                    {
                        fdoContext->totalIndirectErrors++;
                    }
                    else
                    {
                        fdoContext->totalDirectErrors++;
                    }
                }
                else
                {
                    if (shadow->indirectPageMemory)
                    {
                        if (shadow->req.length > fdoContext->largestIndirectTransfer)
                        {
                            fdoContext->largestIndirectTransfer = shadow->req.length;
                            TraceEvents(TRACE_LEVEL_WARNING, TRACE_DPC,
                                        __FUNCTION__": %s request length %d > largest ind xfer\n",
                                        fdoContext->FrontEndPath, shadow->req.length);
                        }
                    }
                    else if (shadow->req.length > fdoContext->largestDirectTransfer)
                    {
                        fdoContext->largestDirectTransfer = shadow->req.length;
                        TraceEvents(TRACE_LEVEL_WARNING, TRACE_DPC,
                                    __FUNCTION__": %s request length %d > largest dir xfer\n",
                                    fdoContext->FrontEndPath, shadow->req.length);
                    }
                }

                // --XT-- Originally shorting this logging out but that was a mess. Logged
                // warnings abovel and make this verbose.
                TraceEvents(TRACE_LEVEL_VERBOSE, TRACE_DPC,
                    __FUNCTION__": %s %s usbif status %x (%s) usbd status %x (%s) ntstatus %x\n"
                    "request type %x endpoint %x offset %d length %d nr_segments %d flags %x\n"
                    "nr_packets %d startframe %d indirectPageMemory %p\n",
                    fdoContext->FrontEndPath,
                    response->status ? "response error" : "response",
                    response->status,
                    usbifStatusString,
                    usbdStatus,
                    usbdStatusString,
                    NtStatus,
                    shadow->req.type,
                    shadow->req.endpoint,
                    shadow->req.offset,
                    shadow->req.length,
                    shadow->req.nr_segments,
                    shadow->req.flags,
                    shadow->req.nr_packets,
                    shadow->req.startframe,
                    shadow->indirectPageMemory);
            }
        }
        else
        {
            PostProcessScratch(fdoContext, 
                usbdStatus, 
                usbifStatusString,
                usbdStatusString,
                response->bytesTransferred, 
                response->data);
        }
        PutShadowOnFreelist(fdoContext->Xen, shadow);
        if (Request)
        {
            WdfRequestWdmGetIrp(Request)->IoStatus.Status = NtStatus;
            NtStatus = WdfCollectionAdd(RequestCollection, Request);
            if (!NT_SUCCESS(NtStatus))
            {
                // ugh, how about a collection that never fails
                // to add elements?
                // we have no choice except to complete the request
                // here. Possibly out of order.
                // NB: this never happens.
                RequestGetRequestContext(Request)->RequestCompleted = 1;
                ReleaseFdoLock(fdoContext);

                WdfRequestCompleteWithPriorityBoost(Request,
                    WdfRequestWdmGetIrp(Request)->IoStatus.Status,
                    IO_SOUND_INCREMENT);

                AcquireFdoLock(fdoContext);
                TraceEvents(TRACE_LEVEL_ERROR, TRACE_DPC,
                    __FUNCTION__": WdfCollectionAdd error %x\n",
                    NtStatus);
            }

        }
    }
    //
    // check for more work
    //
    fdoContext->Xen->Ring.rsp_cons = index;
    if (index != fdoContext->Xen->Ring.req_prod_pvt)
    {
        RING_FINAL_CHECK_FOR_RESPONSES(&fdoContext->Xen->Ring, moreWork);
    }
    else if (!fdoContext->DeviceUnplugged)
    {
        fdoContext->Xen->Ring.sring->rsp_event = index + 1;
        moreWork = (RING_IDX) FALSE;
    }
    if (moreWork)
    {
        return TRUE;
    }
    return FALSE;
}


//
// low level (Xen interface dependent) response processing.
//
NTSTATUS
XenPostProcessIsoResponse(
    IN PURB Urb,
    IN NTSTATUS *usbdStatus,
    IN ULONG bytesTransferred,
    IN ULONG startFrame,
    IN PVOID isoPacketDescriptor,
    IN NTSTATUS Status)
{    
    iso_packet_info * isoInfoArray = (iso_packet_info *) isoPacketDescriptor;

    if (NT_SUCCESS(Status))
    {
        ULONG numberOfPackets = Urb->UrbIsochronousTransfer.NumberOfPackets; 
        Urb->UrbIsochronousTransfer.ErrorCount = 0;
        ULONG totalBytes = 0;

        for (ULONG Index = 0;
            Index < numberOfPackets;
            Index++)
        {
            PCHAR usbifStatusString = "UnknownUsbIf";
            PCHAR usbdStatusString = "";

            MapUsbifToUsbdStatus(
                FALSE,
                isoInfoArray[Index].status,
                &usbifStatusString,
                &usbdStatusString);

            Urb->UrbIsochronousTransfer.IsoPacket[Index].Offset = 
                isoInfoArray[Index].offset;
            Urb->UrbIsochronousTransfer.IsoPacket[Index].Length = 
                isoInfoArray[Index].length;
            Urb->UrbIsochronousTransfer.IsoPacket[Index].Status = 
                isoInfoArray[Index].status;

            if (isoInfoArray[Index].status == USBD_STATUS_SUCCESS)
            {
                totalBytes += isoInfoArray[Index].length;
            }
            else
            {
                Urb->UrbIsochronousTransfer.ErrorCount++;
            }

            TraceEvents(TRACE_LEVEL_VERBOSE, TRACE_DPC,
                __FUNCTION__": packet %d offset %d length %d status %x %s %s\n",
                Index,
                Urb->UrbIsochronousTransfer.IsoPacket[Index].Offset,
                Urb->UrbIsochronousTransfer.IsoPacket[Index].Length,
                Urb->UrbIsochronousTransfer.IsoPacket[Index].Status, //@@@packetStatus,
                usbifStatusString,
                usbdStatusString);
        }
        if (totalBytes == 0)
        {
            if (!gVistaOrLater)
            {
                //
                // xp usbaudio will crash on an iso packet with zero total bytes.
                // as a workaround for this msft bug, set packet 0 to status success.
                // Note that an iso packet always has a non-zero length, so simply
                // converting the status is sufficient. Ignore the fact that we 
                // are producing garbage as data. This code path is a consequence of an unplug
                // operation, iso data is not reliable, and we can't fix usbaudio.
                //
                Urb->UrbIsochronousTransfer.IsoPacket[0].Status = USBD_STATUS_SUCCESS;
                if (Urb->UrbIsochronousTransfer.IsoPacket[0].Length)
                {
                    totalBytes += Urb->UrbIsochronousTransfer.IsoPacket[0].Length;
                }
                else
                {
                    totalBytes += Urb->UrbIsochronousTransfer.IsoPacket[1].Offset;
                }
                TraceEvents(TRACE_LEVEL_ERROR, TRACE_DPC,
                    __FUNCTION__": Urb %p succeed zero length ISO request xp usbaudio bug\n",
                    Urb);
            }
        }

        Urb->UrbIsochronousTransfer.TransferBufferLength = totalBytes;
        if (Urb->UrbIsochronousTransfer.ErrorCount != bytesTransferred)
        {
            TraceEvents(TRACE_LEVEL_VERBOSE, TRACE_DPC,
                __FUNCTION__": packet completion: Urb ErrorCount %d != response ErrorCount %d\n",
                Urb->UrbIsochronousTransfer.ErrorCount,
                bytesTransferred);
        }

        if (Urb->UrbIsochronousTransfer.TransferFlags & USBD_START_ISO_TRANSFER_ASAP)
        {
            Urb->UrbIsochronousTransfer.StartFrame = startFrame;
        }
        else if (startFrame != Urb->UrbIsochronousTransfer.StartFrame)
        {
            TraceEvents(TRACE_LEVEL_VERBOSE, TRACE_DPC,
                __FUNCTION__": packet completion: Urb.startFrame %d != response startframe %d\n",
                Urb->UrbIsochronousTransfer.StartFrame,
                startFrame);
        }
                

        // transfer the packet descriptors back?
        TraceEvents(
            TRACE_LEVEL_VERBOSE, 
            TRACE_DPC,
            __FUNCTION__": packet completion: StartFrame %d  errorCount %d numberOfPackets %d totalBytes %d\n",
            startFrame,
            Urb->UrbIsochronousTransfer.ErrorCount,
            numberOfPackets,
            totalBytes);
    }
    else
    {
        if (!gVistaOrLater)
        {
            Status = STATUS_SUCCESS;
        }
        TraceEvents(TRACE_LEVEL_WARNING, TRACE_DPC,
            __FUNCTION__": Urb %p usbd status %x status %x\n"
            "TransferFlags %x TransferBufferLength %d StartFrame %d NumberOfPackets %d\n",
            Urb,
            *usbdStatus,
            Status,
            Urb->UrbIsochronousTransfer.TransferFlags,
            Urb->UrbIsochronousTransfer.TransferBufferLength,
            Urb->UrbIsochronousTransfer.StartFrame,
            Urb->UrbIsochronousTransfer.NumberOfPackets);
    }

    return Status;
}

//
// Information about the Xen interface.
//

ULONG
MaxIsoSegments(
    IN PXEN_INTERFACE Xen)
{
    return Xen->MaxIsoSegments;
}

ULONG
MaxSegments(
    IN PXEN_INTERFACE Xen)
{
    return Xen->MaxSegments;
}

ULONG
MaxIsoPackets(
    IN PXEN_INTERFACE)
{
    return MAX_ISO_PACKETS;
}

ULONG
OnRingBuffer(
    IN PXEN_INTERFACE Xen)
{
    return Xen->RequestsOnRingbuffer;
}

static VOID
DecrementRingBufferRequests(
    IN PXEN_INTERFACE Xen)
{
    if (NT_VERIFY(Xen->RequestsOnRingbuffer))
    {
        Xen->RequestsOnRingbuffer--;
    }
    else
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DPC,
            __FUNCTION__": RequestsOnRingbuffer is zero!\n");
    }

}

BOOLEAN
IndirectGrefs(
    IN PXEN_INTERFACE Xen)
{
    return Xen->IndirectGrefSupport;
}

ULONG
AvailableRequests(
    IN PXEN_INTERFACE Xen)
{
    return Xen->ShadowFree;
}

//
// Error code translation (debug logging.)
//
NTSTATUS 
MapUsbifToUsbdStatus(
    IN BOOLEAN  ResetInProgress,
    IN LONG UsbIfStatus,
    IN PCHAR * OutUsbIfString,
    IN PCHAR * OutUsbdString)
{
    NTSTATUS UsbdStatus = USBD_STATUS_INTERNAL_HC_ERROR;
    PCHAR UsbifString = "Unknown UsbIfCode";
    PCHAR UsbdString = "USBD_STATUS_INTERNAL_HC_ERROR";

    switch(UsbIfStatus)
    {
    case 0:
        UsbdStatus = USBD_STATUS_SUCCESS;
        UsbifString = "USBIF_SUCCESS";
        UsbdString = "USBD_STATUS_SUCCESS";
        break;
    case USBIF_RSP_USB_CANCELED:
        UsbdStatus = USBD_STATUS_CANCELED;
        UsbifString = "USBIF_RSP_USB_CANCELED";
        UsbdString = "USBD_STATUS_CANCELED";
        break;
    case USBIF_RSP_USB_PENDING:
        UsbdStatus = USBD_STATUS_INTERNAL_HC_ERROR;
        UsbifString = "USBIF_RSP_USB_PENDING";
        UsbdString = "USBD_STATUS_INTERNAL_HC_ERROR";
        break;
    case USBIF_RSP_USB_PROTO:
        UsbdStatus = USBD_STATUS_INTERNAL_HC_ERROR;
        UsbifString = "USBIF_RSP_USB_PROTO";
        UsbdString = "USBD_STATUS_INTERNAL_HC_ERROR";
        break;
    case USBIF_RSP_USB_CRC:
        UsbdStatus = USBD_STATUS_CANCELED; // was USBD_STATUS_CRC;
        UsbifString = "USBIF_RSP_USB_CRC";
        UsbdString = "USBD_STATUS_CANCELED";
        break;
    case USBIF_RSP_USB_TIMEOUT:
        UsbdStatus = USBD_STATUS_STALL_PID; // USBD_STATUS_TIMEOUT;
        UsbifString = "USBIF_RSP_USB_TIMEOUT";
        UsbdString = "USBD_STATUS_STALL_PID";
        break;
    case USBIF_RSP_USB_STALLED:
        UsbdStatus = USBD_STATUS_STALL_PID;
        UsbifString = "USBIF_RSP_USB_STALLED";
        UsbdString = "USBD_STATUS_STALL_PID";
        break;
    case USBIF_RSP_USB_INBUFF:
        UsbdStatus = USBD_STATUS_BUFFER_OVERRUN;
        UsbifString = "USBIF_RSP_USB_INBUFF";
        UsbdString = "USBD_STATUS_BUFFER_OVERRUN";
        break;
    case USBIF_RSP_USB_OUTBUFF:
        UsbdStatus = USBD_STATUS_BUFFER_UNDERRUN;
        UsbifString = "USBIF_RSP_USB_OUTBUFF";
        UsbdString = "USBD_STATUS_BUFFER_UNDERRUN";
        break;
    case USBIF_RSP_USB_OVERFLOW: // stall? babble - for bulk, perhaps not for isoch or interrupt?
        UsbdStatus = USBD_STATUS_STALL_PID; // USBD_STATUS_BABBLE_DETECTED;
        UsbifString = "USBIF_RSP_USB_OVERFLOW";
        UsbdString = "USBD_STATUS_STALL_PID";
        break;
    case USBIF_RSP_USB_SHORTPKT:
        UsbdStatus = USBD_STATUS_ERROR_SHORT_TRANSFER;
        UsbifString = "USBIF_RSP_USB_SHORTPKT";
        UsbdString = "USBD_STATUS_ERROR_SHORT_TRANSFER";
        break;
    case USBIF_RSP_USB_DEVRMVD:
        UsbdStatus = USBD_STATUS_DEVICE_GONE;
        UsbifString = "USBIF_RSP_USB_DEVRMVD";
        UsbdString = "USBD_STATUS_DEVICE_GONE";
        break;
    case USBIF_RSP_USB_INVALID:
        UsbdStatus = USBD_STATUS_INVALID_URB_FUNCTION; // ?
        UsbifString = "USBIF_RSP_USB_INVALID";
        UsbdString = "USBD_STATUS_INVALID_URB_FUNCTION";
        break;
    case USBIF_RSP_USB_PARTIAL:
        UsbdStatus = USBD_STATUS_INTERNAL_HC_ERROR;
        UsbifString = "USBIF_RSP_USB_PARTIAL";
        UsbdString = "USBD_STATUS_INTERNAL_HC_ERROR";
        break;
    case USBIF_RSP_USB_RESET: // this is really a timeout
        UsbdStatus = USBD_STATUS_TIMEOUT;
        UsbifString = "USBIF_RSP_USB_RESET";
        UsbdString = "USBD_STATUS_TIMEOUT";
        break;
    case USBIF_RSP_USB_SHUTDOWN:
        UsbdStatus = USBD_STATUS_DEVICE_GONE; // was USBD_STATUS_ENDPOINT_HALTED
        UsbifString = "USBIF_RSP_USB_SHUTDOWN";
        UsbdString = "USBD_STATUS_DEVICE_GONE";
        break;
    case USBIF_RSP_USB_UNKNOWN: // backend unplug detected! (or not.)
        UsbdStatus = USBD_STATUS_ERROR_BUSY; // was USBD_STATUS_DEVICE_GONE;
        UsbifString = "USBIF_RSP_USB_UNKNOWN";
        UsbdString = "USBD_STATUS_ERROR_BUSY";
        break;
    }
    //
    // treat device gone errors as transient if ResetInProgress.
    //
    if ((UsbdStatus == USBD_STATUS_DEVICE_GONE) &&
        (ResetInProgress))
    {
        UsbdStatus = USBD_STATUS_CANCELED;
        UsbdString = "USBD_STATUS_CANCELED";
    }
    if (OutUsbIfString)
    {
        *OutUsbIfString = UsbifString;
    }
    if (OutUsbdString)
    {
        *OutUsbdString = UsbdString;
    }
    return UsbdStatus;
}

BOOLEAN
XenCheckOnline(
    IN PXEN_INTERFACE Xen)
{
    return XenLowerGetOnline(Xen->XenLower);
}

BOOLEAN
XenCheckOperationalState(
    IN PXEN_INTERFACE Xen)
{
    xenbus_state xenstate = (xenbus_state)XenLowerGetBackendState(Xen->XenLower);
    BOOLEAN Operational = TRUE;

    switch (xenstate)
    {
    case XenbusStateUnknown: //0
    case XenbusStateClosed:  //6
    case XenbusStateClosing: //5 
        Operational = FALSE;
    default:
        break;
    };
    return Operational;
}
