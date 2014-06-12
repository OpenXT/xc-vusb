//
// Copyright (c) 2014 Citrix Systems, Inc.
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

#include <stdarg.h>
#pragma warning(push)
#pragma warning(disable:6011)
#include <ntstrsafe.h>
#pragma warning(pop)

typedef enum {
	XenTraceLevelDebug,     /* Debug messages */
	XenTraceLevelVerbose,   /* Tracing of normal operation */
	XenTraceLevelInfo,      /* General operational messages. */
	XenTraceLevelNotice,    /* Important operational messages. */
	XenTraceLevelWarning,   /* Something bad happened, trying to recover */
	XenTraceLevelError,     /* Something bad happened, reduced functionality */
	XenTraceLevelCritical,  /* Something bad happened, we're going to crash soon. */
	XenTraceLevelBugCheck,  /* Something very bad happened, crash immediately. */
	XenTraceLevels,         /* Number of levels with dispositions that can be modified*/
	XenTraceLevelProfile,   /* Messages for WPP */
	XenTraceLevelInternal   /* Reserved for internal use only */
} XEN_TRACE_LEVEL;

extern "C"
{
	void ___XenTrace(XEN_TRACE_LEVEL lvl,
		__in_ecount(module_size) PCSTR module,
		size_t module_size,
		PCSTR fmt,
		va_list args);
}

static inline void __XenTrace(XEN_TRACE_LEVEL level, ULONG flags, PCSTR fmt, ...) 
{
	va_list args;

    UNREFERENCED_PARAMETER(flags);

    // --XT-- XXX TODO This is a temporary fix to deal with the really
    // loud tracing. See the logging comment in xenif.cpp for more info.
    if (level < XenTraceLevelWarning)
    {
        return;
    }

	va_start(args, fmt);
	___XenTrace(level, XENTARGET,  sizeof(XENTARGET)-1, fmt, args);
	va_end(args);
}

#define TraceEvents(_lvl_, _flg_, format, ...) \
	__XenTrace((XEN_TRACE_LEVEL)_lvl_, _flg_, format, __VA_ARGS__)

#define TRACE_DRIVER 0x01
#define TRACE_DEVICE 0x02
#define TRACE_QUEUE  0x04
#define TRACE_URB    0x08
#define TRACE_ISR    0x10
#define TRACE_DPC    0x20

#define TRACE_LEVEL_FATAL       XenTraceLevelCritical
#define TRACE_LEVEL_ERROR       XenTraceLevelError
#define TRACE_LEVEL_WARNING     XenTraceLevelWarning
#define TRACE_LEVEL_INFORMATION XenTraceLevelInfo 
#define TRACE_LEVEL_VERBOSE     XenTraceLevelVerbose 

EXTERN_C ULONG gDebugLevel;
EXTERN_C ULONG gDebugFlag;
EXTERN_C PCHAR gDriverName;

void
GetDebugSettings(IN PUNICODE_STRING RegistryPath);

#if DBG
#define HTSASSERT(_exp) \
	((!(_exp)) ? \
	(RtlAssert(#_exp, __FILE__, __LINE__, NULL), FALSE) : \
	TRUE)
#else
#define HTSASSERT(_exp) (_exp)
#endif

#if DBG
#define STR2(x) #x
#define STR1(x) STR2(x)
#define LOC __FILE__ "("STR1(__LINE__)") : Warning Msg: " __FUNCTION__
#define XXX_TODO(hint) __pragma(message(LOC " XXX TODO: " hint))
#else
#define XXX_TODO(hint)
#endif