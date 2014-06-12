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

/*
** Template for version resources.  Place this in your .rc file,
** editing the values for VER_FILETYPE, VER_FILESUBTYPE,
** VER_FILEDESCRIPTION_STR and VER_INTERNALNAME_STR as needed.
** See winver.h for possible values.
**
** Ntverp.h defines several global values that aren't normally
** changed except for official releases such as betas, sdk updates, etc.
**
** Common.ver has the actual version resource structure that all these
** #defines eventually initialize.
*/
#pragma once
//
// this defines all the missing constants
//
#ifndef VER_BUILD_STRING
#if DBG
#define VER_BUILD_STRING "Checked Build"
#else
#define VER_BUILD_STRING ""
#endif
#endif

#if AMD64
#define VER_PLATFORM_STRING "X64"
#else
#define VER_PLATFORM_STRING "X86"
#endif

#ifndef VER_FILEDESCRIPTION_STR
#define VER_FILEDESCRIPTION_STR     VER_INTERNAL_FILEDESCRIPTION_STR " " VER_PRODUCT_RELEASE_LEVEL_STR " " VER_BUILD_STRING " " VER_PLATFORM_STRING
#endif

#ifndef NT_INCLUDED

#include <winver.h>

//
// from ntverp.h
//

/* default is nodebug */
#if DBG
#define VER_DEBUG                   VS_FF_DEBUG
#else
#define VER_DEBUG                   0
#endif

/* default is prerelease */
#if ALPHA
#define VER_PRERELEASE              VS_FF_PRERELEASE
#elif BETA
#define VER_PRERELEASE              VS_FF_PRERELEASE
#else
#define VER_PRERELEASE              0
#endif



#define VER_FILEFLAGSMASK           VS_FFI_FILEFLAGSMASK
#define VER_FILEOS                  VOS_NT_WINDOWS32
#define VER_FILEFLAGS               (VER_PRERELEASE|VER_DEBUG)

#ifndef VER_COMPANYNAME_STR
#define VER_COMPANYNAME_STR         "Citrix, Inc."
#endif

/*-----------------------------------------------*/
/* the following lines are specific to this file */
/*-----------------------------------------------*/



/* VER_FILETYPE, VER_FILESUBTYPE, VER_FILEDESCRIPTION_STR
 * and VER_INTERNALNAME_STR must be defined before including COMMON.VER
 * The strings don't need a '\0', since common.ver has them.
 */
#ifndef VER_FILETYPE
#define	VER_FILETYPE	VFT_DRV
#endif
/* possible values:		VFT_UNKNOWN
				VFT_APP
				VFT_DLL
				VFT_DRV
				VFT_FONT
				VFT_VXD
				VFT_STATIC_LIB
*/
#ifndef VER_FILESUBTYPE
#define	VER_FILESUBTYPE	VFT2_DRV_INSTALLABLE
#endif
/* possible values		VFT2_UNKNOWN
				VFT2_DRV_PRINTER
				VFT2_DRV_KEYBOARD
				VFT2_DRV_LANGUAGE
				VFT2_DRV_DISPLAY
				VFT2_DRV_MOUSE
				VFT2_DRV_NETWORK
				VFT2_DRV_SYSTEM
				VFT2_DRV_INSTALLABLE
				VFT2_DRV_SOUND
				VFT2_DRV_COMM
*/

#ifndef VER_FILEVERSION_STR
#if DBG
#define VER_FILEVERSION_STR VER_PRODUCT_RELEASE_LEVEL_STR " Checked Build"
#else
#define VER_FILEVERSION_STR VER_PRODUCT_RELEASE_LEVEL_STR
#endif
#endif


#include "common.ver"
#endif
