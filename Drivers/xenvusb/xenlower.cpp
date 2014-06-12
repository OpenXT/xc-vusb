//
// Copyright (c) Citrix Systems, Inc., All rights reserved.
//
/// @file xenlower.cpp lowlevel bus access implementation.
/// This module is the lower edge interface to the underlying
/// xenbus and hypercalls.
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
#define INITGUID

#include <ntddk.h>
#include <wdf.h>
#pragma warning(push)
#pragma warning(disable:6011)
#include <ntstrsafe.h>
#pragma warning(pop)
#include <event_channel.h>
#include <grant_table.h>
#include <xenbus.h>
extern "C" {
#include <xsapi.h>
}
#include "xenlower.h"

#pragma warning(disable : 4127 ) //conditional expression is constant

// Me likey
#if DBG
#define STR2(x) #x
#define STR1(x) STR2(x)
#define LOC __FILE__ "("STR1(__LINE__)") : Warning Msg: " __FUNCTION__
#define XXX_TODO(hint) __pragma(message(LOC " XXX TODO: " hint))
#else
#define XXX_TODO(hint)
#endif

//#define DBG_GREF 1

#ifdef DBG_GREF
static VOID
_XenLowerTraceGref(PCSTR Caller, grant_ref_t greft, GRANT_REF gref)
{
    grant_ref_t greftun = xen_GRANT_REF(gref);

    TraceInfo(("%s: GRANT_REF greft: %d (0x%x) greftun: %d (0x%x) wrapped: %2.2x:%2.2x:%2.2x:%2.2x\n",
        Caller, greft, greft, greftun, greftun,
        gref.__wrapped_data[0], gref.__wrapped_data[1],
        gref.__wrapped_data[2], gref.__wrapped_data[3]));
}

#define XenLowerTraceGref(g1, g2) \
    _XenLowerTraceGref(__FUNCTION__, g1, g2)
#else
#define XenLowerTraceGref(g1, g2) do {} while (FALSE)
#endif

// Memory Allocators (so we don't have to pull in scsiboot.h)
extern "C" {
/* Memory allocation functions */
PVOID _XmAllocateMemory(size_t size, const char *caller);
PVOID _XmAllocateZeroedMemory(size_t size, const char *caller);

/* Allocate x bytes of non-paged pool.  Guaranteed to be page aligned
 if x >= PAGE_SIZE. */
#define XmAllocateMemory(x) _XmAllocateMemory((x), __FUNCTION__)

/* Like XmAllocateMemory(), but zero the memory on success. */
#define XmAllocateZeroedMemory(x) _XmAllocateZeroedMemory((x), __FUNCTION__)

/* XmFreeMemory(x) releases memory obtained via XmAllocateMemory or
 XmAllocatePhysMemory. */
VOID XmFreeMemory(PVOID ptr);
}

struct XEN_LOWER
{
    PVOID XenUpper;
    PDEVICE_OBJECT Pdo;
    CHAR FrontendPath[XEN_LOWER_MAX_PATH];
    CHAR BackendPath[XEN_LOWER_MAX_PATH];
    DOMAIN_ID BackendDomid;
    EVTCHN_PORT EvtchnPort;
    GRANT_REF SringGrantRef;
    PRESUME_HANDLER_CB ResumeCallback;
    struct SuspendHandler *LateSuspendHandler;
};

//
// Xen Lower helpers
//

static PCHAR
XenLowerReadXenstoreValue(
    PCHAR Path,
    PCHAR Value)
{
    ULONG plen, vlen;
    PCHAR path;
    PCHAR res = NULL;
    NTSTATUS status;

    plen = (ULONG)strlen(Path);
    vlen = (ULONG)strlen(Value);
    path = (PCHAR)XmAllocateMemory(plen + vlen + 2);
    if (path == NULL)
    {
        return NULL;
    }

    memcpy(path, Path, plen);
    path[plen] = '/';
    memcpy(path + plen + 1, Value, vlen + 1);
    status = xenbus_read(XBT_NIL, path, &res);
    XmFreeMemory(path);
    if (!NT_SUCCESS(status))
    {
        return NULL;
    }

    return res;
}

//
// Xen Lower services
//

PXEN_LOWER
XenLowerAlloc(
    VOID)
{
    PXEN_LOWER p =
        (PXEN_LOWER)ExAllocatePoolWithTag(NonPagedPool, sizeof(XEN_LOWER), '9UVX');
    if (p)
    {
        RtlZeroMemory(p, sizeof(XEN_LOWER));
    }
    return p;
}

VOID
XenLowerFree(
    PXEN_LOWER XenLower)
{
    if (!XenLower)
    {
        return;
    }

    if (XenLower->LateSuspendHandler)
    {
        EvtchnUnregisterSuspendHandler(XenLower->LateSuspendHandler);
    }

    if (!is_null_EVTCHN_PORT(XenLower->EvtchnPort))
    {
        EvtchnPortStop(XenLower->EvtchnPort);
        EvtchnClose(XenLower->EvtchnPort);
    }

    if (!is_null_GRANT_REF(XenLower->SringGrantRef))
    {
        (VOID)GnttabEndForeignAccess(XenLower->SringGrantRef);
    }

    ExFreePool(XenLower);
}

BOOLEAN
XenLowerInit(
    PXEN_LOWER XenLower,
    PVOID XenUpper,
    PDEVICE_OBJECT Pdo)
{
    PCHAR path;
    NTSTATUS status;

    XenLower->XenUpper = XenUpper;
    XenLower->Pdo = Pdo;

    //
    // Wait for xenbus to come up.  SMP guests sometimes try and
    // initialise xennet and xenvbd in parallel when they come back
    // from hibernation, and that causes problems.
    //

    if (!xenbus_await_initialisation())
    {
        TraceError((__FUNCTION__ ": xenbus_await_initialisation() failed?\n"));
        return FALSE;
    }

    path = xenbus_find_frontend(Pdo);
    if (path == NULL)
    {
        TraceError((__FUNCTION__
            ": xenbus_find_frontend() failed to return the front end path, fatal.\n"));
        return FALSE;
    }
    status = RtlStringCchCopyA(XenLower->FrontendPath,
        sizeof(XenLower->FrontendPath),
        path);
    XmFreeMemory(path);
    if (status != STATUS_SUCCESS)
    {
        XenLower->FrontendPath[0] = 0;
        TraceError((__FUNCTION__ ": Failed to copy front end path - status: 0x%x\n", status));
        return FALSE;
    }

    return TRUE;
}

BOOLEAN
XenLowerBackendInit(
    PXEN_LOWER XenLower)
{
    PCHAR path;
    NTSTATUS status;

    // Note this is split from the XenLowerInit so it can be called on the resume
    // path in case backend values change.

    XXX_TODO("--XT-- All the backend path handling assumes dom0 is the backend, this will change for device domains")
    // XXX TODO all the backend path handling assumes dom0 is the backend. This will
    // not necessarily be true with device domains. The changes to support this go
    // beyond this module though.
    path = XenLowerReadXenstoreValue(XenLower->FrontendPath, "backend");
    if (path == NULL)
    {
        TraceError((__FUNCTION__
            ": XenLowerReadXenstoreValue() failed to return the back end path, fatal.\n"));
        return FALSE;
    }
    status = RtlStringCchCopyA(XenLower->BackendPath,
        sizeof(XenLower->BackendPath),
        path);
    XmFreeMemory(path);
    if (status != STATUS_SUCCESS)
    {
        XenLower->BackendPath[0] = 0;
        TraceError((__FUNCTION__
            ": Failed to copy back end path - status: 0x%x\n", status));
        return FALSE;
    }

    status = xenbus_read_domain_id(XBT_NIL, XenLower->FrontendPath,
        "backend-id", &XenLower->BackendDomid);
    if (!NT_SUCCESS(status))
    {
        TraceWarning((__FUNCTION__
            ": Failed to read backend id from %s (%x), setting to dom0\n",
            XenLower->FrontendPath, status));
        XenLower->BackendDomid = DOMAIN_ID_0();
    }

    // XXX TODO for now we only support a dom0 backend so check that here. Later
    // when we support a device domain for vusb, other domids will be fine.
    XXX_TODO("--XT-- For now we only support a dom0 backend so check that here");
    if (unwrap_DOMAIN_ID(XenLower->BackendDomid) != unwrap_DOMAIN_ID(DOMAIN_ID_0()))
    {
        TraceError((XENTARGET
            ": cannot connect to backend Domid: %d, only dom0 supported currently\n",
            XenLower->BackendDomid));
        return FALSE;
    }

    TraceInfo((__FUNCTION__
        ": XenLower initialized - FrontendPath: %s  BackendPath: %s BackendDomid: %d\n",
        XenLower->FrontendPath, XenLower->BackendPath, unwrap_DOMAIN_ID(XenLower->BackendDomid)));

    return TRUE;
}

PVOID
XenLowerGetUpper(
    PXEN_LOWER XenLower)
{
    return XenLower->XenUpper;
}

ULONG
XenLowerInterfaceVersion(
    PXEN_LOWER XenLower)
{
    NTSTATUS status;
    PCHAR vstr;
    int version;

	vstr = XenLowerReadXenstoreValue(XenLower->BackendPath, "version");
    if (vstr == NULL)
    {
        TraceError((__FUNCTION__\
            ": XenLowerReadXenstoreValue() failed to return the vusb version.\n"));
        return 0;
    }

    sscanf_s(vstr, "%d", &version);
    XmFreeMemory(vstr);

    // Need to now write the version we support to the frontend
    status = xenbus_printf(XBT_NIL, XenLower->FrontendPath,
        "version", "%d", XEN_LOWER_INTERFACE_VERSION);
    if (!NT_SUCCESS(status))
    {
        TraceError((__FUNCTION__\
            ": xenbus_printf(frontend/version) failed.\n"));
        return 0;
    }

    TraceInfo((__FUNCTION__
        ": Read backend version: %d  -  Wrote frontend version: %d\n",
        version, XEN_LOWER_INTERFACE_VERSION));

    return (ULONG)version;
}

PCHAR
XenLowerGetFrontendPath(
    PXEN_LOWER XenLower)
{
    return XenLower->FrontendPath;
}

PCHAR
XenLowerGetBackendPath(
    PXEN_LOWER XenLower)
{
    return XenLower->BackendPath;
}

BOOLEAN
XenLowerGetSring(
    PXEN_LOWER XenLower,
    uint32_t Pfn)
{
    XenLower->SringGrantRef =
        GnttabGrantForeignAccess(XenLower->BackendDomid,
        (ULONG_PTR)Pfn,
        GRANT_MODE_RW);
    if (is_null_GRANT_REF(XenLower->SringGrantRef))
    {
        TraceError((__FUNCTION__\
            ": GnttabGrantForeignAccess() failed to return shared ring grant ref.\n"));
        return FALSE;
    }

    // Note the gref will be written to xenstore later in the second connect part
    // in anticipation of supporting suspend/resume.
    return TRUE;
}

BOOLEAN
XenLowerConnectEvtChnDPC(
    PXEN_LOWER XenLower,
    PEVTCHN_HANDLER_CB DpcCallback,
    VOID *Context)
{
    XenLower->EvtchnPort =
        EvtchnAllocUnboundDpc(XenLower->BackendDomid, DpcCallback, Context);

    if (is_null_EVTCHN_PORT(XenLower->EvtchnPort))
    {
        TraceError((__FUNCTION__ ": failed to allocate DPC for Event Channel.\n"));
        return FALSE;
    }

    return TRUE;
}

VOID
XenLowerScheduleEvtChnDPC(
    PXEN_LOWER XenLower)
{
    if (!is_null_EVTCHN_PORT(XenLower->EvtchnPort))
    {
        EvtchnRaiseLocally(XenLower->EvtchnPort);
    }
}

VOID
XenLowerDisconnectEvtChnDPC(
    PXEN_LOWER XenLower)
{
    if (!is_null_EVTCHN_PORT(XenLower->EvtchnPort))
    {
        EvtchnPortStop(XenLower->EvtchnPort);
        EvtchnClose(XenLower->EvtchnPort);
        XenLower->EvtchnPort = null_EVTCHN_PORT();
    }
}

static NTSTATUS
XenLowerConnectBackendInternal(
    PXEN_LOWER XenLower,
    SUSPEND_TOKEN Token)
{
    NTSTATUS status = STATUS_SUCCESS;
    XENBUS_STATE state;
    xenbus_transaction_t xbt;
    PCHAR fepath;

    if (is_null_EVTCHN_PORT(XenLower->EvtchnPort))
    {
        TraceError((__FUNCTION__ ": no event channel port, this routine must be called after event channel initialization\n"));
        return STATUS_UNSUCCESSFUL;
    }

    //---------------------------Backend Wait Ready-------------------------------//
    //
    // Wait for backend to get ready for initialization.
    //

    status = xenbus_change_state(XBT_NIL,
        XenLower->FrontendPath,
        "state",
        XENBUS_STATE_INITIALISING);
    if (!NT_SUCCESS(status))
    {
        TraceWarning((__FUNCTION__
            ": Failed to change front end state to XENBUS_STATE_INITIALISING(%d) status: 0x%x\n",
            XENBUS_STATE_INITIALISING, status));
        // Go on, best effort, chin up
    }

    TraceInfo((__FUNCTION__
        ": Front end state set to XENBUS_STATE_INITIALISING(%d)\n",
        XENBUS_STATE_INITIALISING));

    state = null_XENBUS_STATE();
    for ( ; ; )
    {
        // Turns out suspend tokens are not even used.
        state = XenbusWaitForBackendStateChange(XenLower->BackendPath,
            state, NULL, Token);

        if (same_XENBUS_STATE(state, XENBUS_STATE_INITWAIT))
        {
            break;
        }

        if (same_XENBUS_STATE(state, XENBUS_STATE_CLOSING) ||
            is_null_XENBUS_STATE(state))
        {
            TraceError((__FUNCTION__ ": backend '%s' went away before we could connect to it?\n",
                XenLower->BackendPath));

            status = STATUS_UNSUCCESSFUL;
            break;
        }
    }

    if (status != STATUS_SUCCESS)
    {
        return status;
    }

    TraceInfo((__FUNCTION__
        ": Back end state went to XENBUS_STATE_INITWAIT(%d)\n",
        XENBUS_STATE_INITWAIT));
    
    //----------------------------Backend Connect---------------------------------//    

    //
    // Communicate configuration to backend.
    //

    fepath = XenLower->FrontendPath;
    do {
        xenbus_transaction_start(&xbt);
        xenbus_write_grant_ref(xbt, fepath, "ring-ref", XenLower->SringGrantRef);
        xenbus_write_evtchn_port(xbt, fepath, "event-channel", XenLower->EvtchnPort);
        xenbus_change_state(xbt, fepath, "state", XENBUS_STATE_CONNECTED);
        status = xenbus_transaction_end(xbt, 0);
    } while (status == STATUS_RETRY);

    if (status != STATUS_SUCCESS)
    {
        TraceError((__FUNCTION__ ": failed to configure xenstore frontend values.\n"));
        return STATUS_UNSUCCESSFUL;
    }

    TraceInfo((__FUNCTION__
        ": Front end state set to XENBUS_STATE_CONNECTED(%d)\n",
        XENBUS_STATE_CONNECTED));

    //
    // Wait for backend to accept configuration and complete initialization.
    //

    state = null_XENBUS_STATE();
    for ( ; ; )
    {
        state = XenbusWaitForBackendStateChange(XenLower->BackendPath,
            state, NULL, Token);

        if (is_null_XENBUS_STATE(state) ||
            same_XENBUS_STATE(state, XENBUS_STATE_CLOSING) ||
            same_XENBUS_STATE(state, XENBUS_STATE_CLOSED))
        {

            TraceError((__FUNCTION__ ": Failed to connected '%s' <-> '%s' backend state: %d\n",
                XenLower->FrontendPath,
                XenLower->BackendPath,
                state));

            status = STATUS_UNSUCCESSFUL;
            break;
        }

        if (same_XENBUS_STATE(state, XENBUS_STATE_CONNECTED))
        {
            TraceNotice((__FUNCTION__ ": Connected '%s' <-> '%s' \n",
                XenLower->FrontendPath,
                XenLower->BackendPath));
            TraceInfo((__FUNCTION__
                ": Back end final state went to XENBUS_STATE_CONNECTED(%d)\n",
                XENBUS_STATE_CONNECTED));
            break;
        }
    }

    return status;
}

VOID
XenLowerResumeLate(
    PVOID Context,
    SUSPEND_TOKEN Token)
{
    PXEN_LOWER XenLower = (PXEN_LOWER)Context;

    XenLower->ResumeCallback(XenLower, &Token);
}

static VOID
XenLowerRegisterSuspendHandler(
    PXEN_LOWER XenLower,
    PRESUME_HANDLER_CB ResumeCallback)
{
    XenLower->ResumeCallback = ResumeCallback;

    XenLower->LateSuspendHandler =
        EvtchnRegisterSuspendHandler(XenLowerResumeLate,
            XenLower,
            "RestartVusbifLate",
            SUSPEND_CB_LATE);
    if (!XenLower->LateSuspendHandler)
    {
        TraceError((__FUNCTION__ ": failed to register suspend handler.\n"));
    }
}

NTSTATUS
XenLowerConnectBackend(
    PXEN_LOWER XenLower,
    PRESUME_HANDLER_CB ResumeCallback)
{
    NTSTATUS status;
    SUSPEND_TOKEN token;

    token = EvtchnAllocateSuspendToken("xenvusb");
    if (is_null_SUSPEND_TOKEN(token))
    {
        TraceError((__FUNCTION__ ": failed to get supsend token\n"));
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    status = XenLowerConnectBackendInternal(XenLower, token);

    // Register resume callback, already holding a suspend token.
    XenLowerRegisterSuspendHandler(XenLower, ResumeCallback);

    EvtchnReleaseSuspendToken(token);

    return status;
}

NTSTATUS
XenLowerResumeConnectBackend(
    PXEN_LOWER XenLower,
    VOID *Internal)
{
    SUSPEND_TOKEN *pt = (SUSPEND_TOKEN*)Internal;

    return XenLowerConnectBackendInternal(XenLower, *pt);
}

VOID
XenLowerDisonnectBackend(
    PXEN_LOWER XenLower)
{
    SUSPEND_TOKEN token;
    XENBUS_STATE festate;
    XENBUS_STATE bestate;

    if (strlen(XenLower->BackendPath) == 0)
    {
        TraceError((__FUNCTION__ ": shutting down an adapter %s which wasn't properly created?\n",
            XenLower->FrontendPath));
        return;        
    }

    // Give disconnect a go even if by some chance we cannot get a token.
    token = EvtchnAllocateSuspendToken("xenvusb-disconnect");

    // Wait for the backend to stabilise before we close it
    bestate = null_XENBUS_STATE();
    do {
        bestate = XenbusWaitForBackendStateChange(XenLower->BackendPath,
            bestate, NULL, null_SUSPEND_TOKEN());
    } while (same_XENBUS_STATE(bestate, XENBUS_STATE_INITIALISING));

    // Now close the frontend
    festate = XENBUS_STATE_CLOSING;
    while (!same_XENBUS_STATE(bestate, XENBUS_STATE_CLOSING) &&
           !same_XENBUS_STATE(bestate, XENBUS_STATE_CLOSED) &&
           !is_null_XENBUS_STATE(bestate))
    {
        xenbus_change_state(XBT_NIL, XenLower->FrontendPath, "state", festate);
        bestate = XenbusWaitForBackendStateChange(XenLower->BackendPath,
            bestate, NULL, null_SUSPEND_TOKEN());
    }

    festate = XENBUS_STATE_CLOSED;
    while (!same_XENBUS_STATE(bestate, XENBUS_STATE_CLOSED) &&
           !is_null_XENBUS_STATE(bestate))
    {
        xenbus_change_state(XBT_NIL, XenLower->FrontendPath, "state", festate);
        bestate = XenbusWaitForBackendStateChange(XenLower->BackendPath,
            bestate, NULL, null_SUSPEND_TOKEN());
    }

    // Unhook this here since there will be no reconnecting at this point.
    if (XenLower->LateSuspendHandler)
    {
        EvtchnUnregisterSuspendHandler(XenLower->LateSuspendHandler);
        XenLower->LateSuspendHandler = NULL;
    }

    // Clear the XenLower->BackendPath which sort of indicates shutdown or
    // not properly initialized.

    // --XT-- keep backend path, useful on resume to check whether device was
    //        unplugged during suspend
    // memset(&XenLower->BackendPath[0], 0, XEN_LOWER_MAX_PATH);

    if (!is_null_SUSPEND_TOKEN(token))
    {
        EvtchnReleaseSuspendToken(token);
    }
}

//
// Xen lower XENPCI_VECTORS vectors
//

NTSTATUS
XenLowerEvtChnNotify(
    PVOID Context)
{
    PXEN_LOWER XenLower = (PXEN_LOWER)Context;

    if (is_null_EVTCHN_PORT(XenLower->EvtchnPort))
    {
        TraceError((__FUNCTION__ ": no event channel port, cannot notory anything.\n"));
        return STATUS_UNSUCCESSFUL;
    }

    EvtchnNotifyRemote(XenLower->EvtchnPort);

    return STATUS_SUCCESS;
}

grant_ref_t
XenLowerGntTblGetRef(VOID)
{
    GRANT_REF gref;
    grant_ref_t greft;
    
    gref = GnttabGetGrantRef();
    greft = xen_GRANT_REF(gref);
    if (greft == xen_GRANT_REF(null_GRANT_REF()))
    {
        TraceError((__FUNCTION__ ": failed to get free grant ref.\n"));
        return INVALID_GRANT_REF;
    }

    XenLowerTraceGref(greft, gref);

    return greft;
}

grant_ref_t
XenLowerGntTblGrantAccess(
    uint16_t Domid,
    uint32_t Frame,
    int Readonly,
    grant_ref_t Ref)
{
    GRANT_MODE mode = (Readonly ? GRANT_MODE_RO : GRANT_MODE_RW);
    DOMAIN_ID domain = wrap_DOMAIN_ID(Domid);
    GRANT_REF gref = wrap_GRANT_REF(Ref, 0);

    if (Ref == INVALID_GRANT_REF)
    {
        TraceError((__FUNCTION__ ": invalid grant ref specified, cannot continue.\n"));
        return INVALID_GRANT_REF;
    }

    XenLowerTraceGref(Ref, gref);

    GnttabGrantForeignAccessRef(domain, (PFN_NUMBER)Frame, mode, gref);

    return Ref;
}

BOOLEAN
XenLowerGntTblEndAccess(
    grant_ref_t Ref)
{
    NTSTATUS status;
    GRANT_REF gref = wrap_GRANT_REF(Ref, 0);

    // Note that the only call this callback passes FALSE for keepref
    // which is good since this grant impl. does not support that currently.

    if (Ref == INVALID_GRANT_REF)
    {
        TraceError((__FUNCTION__ ": invalid grant ref specified, cannot continue.\n"));
        return FALSE;
    }

    XenLowerTraceGref(Ref, gref);

    status = GnttabEndForeignAccess(gref);
    if (!NT_SUCCESS(status))
    {
        TraceError((__FUNCTION__ ": failed to end grant access and return grant ref.\n"));
        return FALSE;
    }

    return TRUE;
}

ULONG
XenLowerGetBackendState(
    PVOID Context)
{
    PXEN_LOWER XenLower = (PXEN_LOWER)Context;
    PCHAR sstr;
    int state;

    sstr = XenLowerReadXenstoreValue(XenLower->BackendPath, "state");
    if (sstr == NULL)
    {
        TraceError((__FUNCTION__
            ": XenLowerReadXenstoreValue() failed to return the back end state.\n"));
        return XenbusStateUnknown;
    }

    sscanf_s(sstr, "%d", &state);
    XmFreeMemory(sstr);

    return (ULONG)state;
}

BOOLEAN
XenLowerGetOnline(
    PVOID Context)
{
    PXEN_LOWER XenLower = (PXEN_LOWER)Context;
    PCHAR sstr;
    int state;

    sstr = XenLowerReadXenstoreValue(XenLower->BackendPath, "online");
    if (sstr == NULL)
    {
        TraceError((__FUNCTION__
            ": XenLowerReadXenstoreValue() failed to return the back end state.\n"));
        return FALSE;
    }

    sscanf_s(sstr, "%d", &state);
    XmFreeMemory(sstr);

    return (state == 1);
}
