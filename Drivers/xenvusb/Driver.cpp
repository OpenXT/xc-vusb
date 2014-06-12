//
// Copyright (c) Citrix Systems, Inc., All rights reserved.
//
/// @file Driver.cpp USB Driver Entry.
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
#include "driver.h"
#include "resource.h"
#include "Device.h"
#include <initguid.h> 
#include <ndisguid.h>
#include <wdmguid.h>
#include <string.h>

/** \mainpage XCE Virtual USB Driver.

The XCE Virtual USB Driver (xenvusb.sys) implements a paravirtual usb device
using XEN ringbuffer and xenstore interfaces for cross domain data transfer.

This is the second major version of this driver. This version differs from 
the original version as follows:
    1. KMDF based.
    2. Win8 WDK based.
    3. Controller/Root Hub/Port Device arhcitecture providing a more complete emulation
    of the standard Microsoft USB device stack.
    4. End to end power management.

Device architecture:
    The Xen bus driver enumerates devices on a virtual bus referred to as the _xen bus_
    and creates PDOs for those devices. If the Xen bus driver enumerates a vusb device,
    the PDO for that device will be connected, via the Windows PnP manager, to this driver
    in its role as a standard windows PnP function driver.

    The files Device.cpp and Device.h implement standard KMDF based function driver operations.
    The function driver emulates a virtual usb controller. There is one virtual usb controller for 
    each physical USB device on the platform that is connected to this VM. USB devices cannot be
    shared, so on a platform with multiple VMs running, USB devices can be connected to any one
    of the VMs or to _DOM0_. 
    
    The virtual usb controller implemented by this driver creates a child PDO device that emulates a
    root hub. The files RootHubPdo.cpp and RootHubPdo.h implement a _raw pdo_ that provide root hub 
    functionality in the standard Microsoft USB device architecture.

    The virtual root hub in turn creates its own child pdo device that represents the physical usb
    device connected to this VM. The files DevicePdo.cpp and DevicePdo.h implement a standard PDO device.
    This pdo device provides USB PnP enumeration descriptors to Windows PnP, allowing standard USB device
    drivers to connect seamlessly into the virtual usb subsystem imp0lement by the XCE Virtual USB Driver.


    \tableofcontents
*/


BOOLEAN gVistaOrLater = FALSE; //!< XP is different.
BOOLEAN gFakeNxprep = FALSE;   //!< for debugging nxprep 

//
/// driver entry is an "init" segment so the strings local to it get discarded so we need a local string
/// that will not get discarded.
//
PCHAR name="XenVusb";

// #define ALPHA_DBG

#ifdef ALLOC_PRAGMA
#pragma alloc_text (INIT, DriverEntry)
#endif
/** 
 * @brief Entry point for the driver.
 * Set up global variables and connect to the framework.
 * If successful creates a WDFDRIVER for this instantiation of the driver.
 * 
 * @param[in] DriverObject DriverObject created by OS.
 * @param[in] RegistryPath registry path to services key for driver.
 *
 * @returns NTSTATUS value for success or failure. Should never fail.
 *
 */
extern "C"
NTSTATUS
DriverEntry(
    _In_ PDRIVER_OBJECT  DriverObject, 
    _In_ PUNICODE_STRING RegistryPath )
{
    WDF_DRIVER_CONFIG config;
    NTSTATUS status;
    WDF_OBJECT_ATTRIBUTES attributes;
    //
    // Setup standard logging state for this driver.
    //
    gDriverName = name;
    gVistaOrLater = RtlIsNtDdiVersionAvailable(NTDDI_VISTA);

#ifdef ALPHA_DBG
    gDebugLevel = TRACE_LEVEL_INFORMATION;
    gDebugFlag = TRACE_DRIVER|TRACE_DEVICE|TRACE_QUEUE|TRACE_URB|TRACE_ISR|TRACE_DPC;
#else
    gDebugLevel = TRACE_LEVEL_WARNING;
    gDebugFlag = TRACE_DRIVER|TRACE_DEVICE|TRACE_QUEUE|TRACE_URB;
#endif
    GetDebugSettings(RegistryPath);

#if DBG
    CHAR * buildType = "Debug";
#else
    CHAR * buildType = "Release";
#endif

    TraceEvents(TRACE_LEVEL_ERROR, TRACE_DRIVER,
        "Xenclient Enterprise Virtual USB Controller Version %s.\n",
        VER_PRODUCTVERSION_STR);
    TraceEvents(TRACE_LEVEL_ERROR, TRACE_DRIVER,
        "%s build created on %s at %s\n",
        buildType,
        __DATE__, __TIME__);
    TraceEvents(TRACE_LEVEL_ERROR, TRACE_DRIVER,
        "DebugLevel %x DebugFlag %x\n", 
        gDebugLevel, gDebugFlag);

    //
    // Setup a cleanup callback for the WDFDRIVER object we are creating. 
    // Currently this is being done only to log that the driver
    // is being unloaded. Alternatively register for EvtDriverUnload, but that
    // will not be called if DriverEntry returns an error.
    //
    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.EvtCleanupCallback = EvtDriverContextCleanup;
    //
    // Setup a callback for DeviceAdd events.
    //
    WDF_DRIVER_CONFIG_INIT(&config,
                           FdoEvtDeviceAdd);
    //
    // And now register with KMDF and create a WDFDRIVER object.
    //
    status = WdfDriverCreate(DriverObject,
                             RegistryPath,
                             &attributes,
                             &config,
                             WDF_NO_HANDLE);

    if (!NT_SUCCESS(status)) 
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DRIVER, "WdfDriverCreate failed %x\n", 
            status);
        return status;
    }
    return status;
}

/** 
 * @brief Perform any cleanup operations required on driver unload.
 * 
 * @param[in] DriverObject WDFDRIVER created by DriverEntry
 * 
 */
VOID
EvtDriverContextCleanup(
    _In_ WDFOBJECT DriverObject
    )
{
    UNREFERENCED_PARAMETER(DriverObject);

    TraceEvents(TRACE_LEVEL_WARNING, TRACE_DRIVER, __FUNCTION__": Driver unload\n");
}

/*
 * GetTheCurrentTime()
 *
 * Get the current time, in milliseconds (KeQuerySystemTime returns units of
 * 100ns each).
 */
ULONG GetTheCurrentTime()
{
    LARGE_INTEGER Time;

    KeQuerySystemTime(&Time);

    return (ULONG)(Time.QuadPart / (10 * 1000));
}

//
// How long has the system been up, in seconds.
//
LONGLONG Uptime()
{
    LARGE_INTEGER Ticks;
    ULONG Increment = KeQueryTimeIncrement();
    KeQueryTickCount(&Ticks);
    return (Ticks.QuadPart * Increment)/10000000L;
}
