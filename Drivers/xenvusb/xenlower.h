//
// Copyright (c) Citrix Systems, Inc.
//
/// @file xenlower.h lowlevel bus access interface definitions.
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

#define XEN_LOWER_INTERFACE_VERSION 3
#define XEN_LOWER_MAX_PATH          128
#define INVALID_GRANT_REF           0xFFFFFFFF

#define wmb() KeMemoryBarrier()
#define mb() KeMemoryBarrier()

typedef struct XEN_LOWER *PXEN_LOWER;

typedef VOID RESUME_HANDLER_CB(PXEN_LOWER XenLower, VOID *Internal);
typedef RESUME_HANDLER_CB *PRESUME_HANDLER_CB;

//
// Xen lower services
//

PXEN_LOWER
XenLowerAlloc(
    VOID);

VOID
XenLowerFree(
    PXEN_LOWER XenLower);

BOOLEAN
XenLowerInit(
    PXEN_LOWER XenLower,
    PVOID XenUpper,
    PDEVICE_OBJECT Pdo);

BOOLEAN
XenLowerBackendInit(
    PXEN_LOWER XenLower);

PVOID
XenLowerGetUpper(
    PXEN_LOWER XenLower);

ULONG
XenLowerInterfaceVersion(
    PXEN_LOWER XenLower);

PCHAR
XenLowerGetFrontendPath(
    PXEN_LOWER XenLower);

PCHAR
XenLowerGetBackendPath(
    PXEN_LOWER XenLower);

BOOLEAN
XenLowerGetSring(
    PXEN_LOWER XenLower,
    uint32_t Pfn);

BOOLEAN
XenLowerConnectEvtChnDPC(
    PXEN_LOWER XenLower,
    PEVTCHN_HANDLER_CB DpcCallback,
    VOID *Context);

VOID
XenLowerScheduleEvtChnDPC(
    PXEN_LOWER XenLower);

VOID
XenLowerDisconnectEvtChnDPC(
    PXEN_LOWER XenLower);

NTSTATUS
XenLowerConnectBackend(
    PXEN_LOWER XenLower,
    PRESUME_HANDLER_CB ResumeCallback);

NTSTATUS
XenLowerResumeConnectBackend(
    PXEN_LOWER XenLower,
    VOID *Internal);

VOID
XenLowerDisonnectBackend(
    PXEN_LOWER XenLower);

//
// Xen lower vectors
//

NTSTATUS
XenLowerEvtChnNotify(
    PVOID Context);

grant_ref_t
XenLowerGntTblGetRef(VOID);

grant_ref_t
XenLowerGntTblGrantAccess(
    domid_t Domid,
    uint32_t Frame,
    int Readonly,
    grant_ref_t Ref);

BOOLEAN
XenLowerGntTblEndAccess(
    grant_ref_t Ref);

ULONG
XenLowerGetBackendState(
    PVOID Context);

BOOLEAN
XenLowerGetOnline(
    PVOID Context);
