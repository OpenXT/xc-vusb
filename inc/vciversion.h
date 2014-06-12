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
#define STRINGER(w, x, y, z) #w "." #x "." #y "." #z 
#define XSTRINGER(w, x, y, z) STRINGER(w, x, y, z)
#define STR(w) #w 
#define XSTR(w) STR(w)

#define VER_PRODUCTVERSION      DRV_VERS_MAJOR,DRV_VERS_MINOR,DRV_VERS_PATCH,DRV_VERS_BLD   
#ifdef DRV_DEV_BUILD
#define VER_PRODUCTVERSION_STR  "Dev." DRV_DEV_BUILD "." XSTRINGER(DRV_VERS_MAJOR,DRV_VERS_MINOR,DRV_VERS_PATCH,DRV_VERS_BLD)
#else 
#define VER_PRODUCTVERSION_STR  XSTRINGER(DRV_VERS_MAJOR,DRV_VERS_MINOR,DRV_VERS_PATCH,DRV_VERS_BLD)
#endif

#define VER_PRODUCTVERSION_NUMBER (\
    ((DRV_VERS_MAJOR & 0xf) << 27) + \
    ((DRV_VERS_MINOR & 0xf) << 23) + \
    ((DRV_VERS_PATCH & 0xff) << 15) + \
    (DRV_VERS_BLD & 0xffff))

#define VER_LEGALCOPYRIGHT_YEARS    "2012"
#define VER_COMPANYNAME_STR         "Citrix, Inc."
#define VER_LEGALCOPYRIGHT_STR      "Copyright © " VER_LEGALCOPYRIGHT_YEARS " " VER_COMPANYNAME_STR
#define VER_PRODUCTNAME_STR         "XenClient Enterprise Desktop Services"

#define VER_PRODUCT_RELEASE_LEVEL_STR VER_PRODUCTVERSION_STR DRV_VERS_REL_LVL
//
// this defines all the missing constants
//
#if DBG
#define VER_BUILD_STRING "Checked Build"
#else
#define VER_BUILD_STRING ""
#endif

#if AMD64
#define VER_PLATFORM_STRING "X64"
#else
#define VER_PLATFORM_STRING "X86"
#endif

#define VER_FILEDESCRIPTION_STR     VER_INTERNAL_FILEDESCRIPTION_STR " " VER_PRODUCT_RELEASE_LEVEL_STR " " VER_BUILD_STRING " " VER_PLATFORM_STRING

#define	VER_FILETYPE	            VFT_DRV

#ifndef VER_FILESUBTYPE
#define	VER_FILESUBTYPE	            VFT2_DRV_INSTALLABLE
#endif

#define VER_FILEFLAGSMASK           VS_FFI_FILEFLAGSMASK
#define VER_FILEOS                  VOS_NT_WINDOWS32


#ifdef VER_FILEVERSION_STR
#undef VER_FILEVERSION_STR
#endif

#define VER_FILEVERSION_STR         VER_PRODUCT_RELEASE_LEVEL_STR " " VER_BUILD_STRING

#include "verrc.h"
