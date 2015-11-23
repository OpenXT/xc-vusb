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
// turn on ISO support
//
#define XENVUSB_ISO 1
//
// usb specific error codes returned from the backend
//
#define USBIF_RSP_USB_ERROR	-10

#define USBIF_USB_CANCELED	-1
#define USBIF_RSP_USB_CANCELED	(USBIF_RSP_USB_ERROR + USBIF_USB_CANCELED)
#define USBIF_USB_PENDING	-2
#define USBIF_RSP_USB_PENDING	(USBIF_RSP_USB_ERROR + USBIF_USB_PENDING)
#define USBIF_USB_PROTO	-3
#define USBIF_RSP_USB_PROTO	(USBIF_RSP_USB_ERROR + USBIF_USB_PROTO)
#define USBIF_USB_CRC	-4
#define USBIF_RSP_USB_CRC	(USBIF_RSP_USB_ERROR + USBIF_USB_CRC)
#define USBIF_USB_TIMEOUT	-5
#define USBIF_RSP_USB_TIMEOUT	(USBIF_RSP_USB_ERROR + USBIF_USB_TIMEOUT)
#define USBIF_USB_STALLED	-6
#define USBIF_RSP_USB_STALLED	(USBIF_RSP_USB_ERROR + USBIF_USB_STALLED)
#define USBIF_USB_INBUFF	-7
#define USBIF_RSP_USB_INBUFF	(USBIF_RSP_USB_ERROR + USBIF_USB_INBUFF)
#define USBIF_USB_OUTBUFF	-8
#define USBIF_RSP_USB_OUTBUFF	(USBIF_RSP_USB_ERROR + USBIF_USB_OUTBUFF)
#define USBIF_USB_OVERFLOW	-9
#define USBIF_RSP_USB_OVERFLOW	(USBIF_RSP_USB_ERROR + USBIF_USB_OVERFLOW)
#define USBIF_USB_SHORTPKT	-10
#define USBIF_RSP_USB_SHORTPKT	(USBIF_RSP_USB_ERROR + USBIF_USB_SHORTPKT)
#define USBIF_USB_DEVRMVD	-11
#define USBIF_RSP_USB_DEVRMVD	(USBIF_RSP_USB_ERROR + USBIF_USB_DEVRMVD)
#define USBIF_USB_PARTIAL	-12
#define USBIF_RSP_USB_PARTIAL	(USBIF_RSP_USB_ERROR + USBIF_USB_PARTIAL)
#define USBIF_USB_INVALID	-13
#define USBIF_RSP_USB_INVALID	(USBIF_RSP_USB_ERROR + USBIF_USB_INVALID)
#define USBIF_USB_RESET	-14
#define USBIF_RSP_USB_RESET	(USBIF_RSP_USB_ERROR + USBIF_USB_RESET)
#define USBIF_USB_SHUTDOWN	-15
#define USBIF_RSP_USB_SHUTDOWN	(USBIF_RSP_USB_ERROR + USBIF_USB_SHUTDOWN)
#define USBIF_USB_UNKNOWN	-16
#define USBIF_RSP_USB_UNKNOWN	(USBIF_RSP_USB_ERROR + USBIF_USB_UNKNOWN)


//
// URB Requests
//

//
// STUB what is thre real limit here?
// STUB lots of operations have no data no transfer flags
//
#define USBIF_URB_MAX_SEGMENTS_PER_REQUEST 17 
#define USBIF_URB_MAX_ISO_SEGMENTS (USBIF_URB_MAX_SEGMENTS_PER_REQUEST - 1)
#define USBIF_URB_MAX_SEGMENTS_PER_REQUEST_V2 66
#define USBIF_URB_MAX_ISO_SEGMENTS_V2 (USBIF_URB_MAX_SEGMENTS_PER_REQUEST_V2 - 1)
//
// flags bits
//
#define REQ_SHORT_PACKET_OK 0x01
#define RESET_TARGET_DEVICE 0x02 // if set all other flags and data are ignored.
#define ISO_FRAME_ASAP      0x04 // if set start this ISO request on next available frame
#define INDIRECT_GREF       0x08 // if set this request uses indirect gref pages (see comments below).
#define CYCLE_PORT          0x10 // force re-enumeration of this device


typedef uint32_t usbif_request_len_t;

//
// INDIRECT GREF PAGES:
// 
// If flags & INDIRECT_GREF is true:
//   usbif_request.nr_segments indicates the number of valid gref descriptors
//   in usbif_request.gref[] but each page referred to in usbif_request.gref[]
//   is not the data for the URB but instead a usbif_indirect_page, defined as:
//
#define INDIRECT_GREF_PAGES 1023
struct usbif_indirect_page {
    uint32_t          nr_segments; // valid data grefs in this page.
    grant_ref_t       gref[INDIRECT_GREF_PAGES];  // the data grefs.
};
typedef struct usbif_indirect_page usbif_indirect_page_t;
//
// The indirect gref mechanism can support up to nr_segments (255) indirect pages of 1023 grefs each,
// for a theoretical maximum transfer size of 255 * 1023 * 4096 bytes.  (1,068,503,040).
// However as the usbif_request.gref[] array is itself limited to USBIF_URB_MAX_SEGMENTS_PER_REQUEST (17)
// grefs, the real limit is 17 * 1023 * 4096 bytes (71,233,536).
//
// Note that windows limits DMA transactions to slightly less than 67,108,864 (64MB) bytes (MDL limit.)
//
// XXX: the usbif_indirect_page could be an array of gref[1024] with the first gref of
// the first page dual purposed as the nr_segments for the entire set of indirect pages.
//
// For ISO requests the first gref is the array of nr_packets iso_packet_info requests.
//

struct usbif_request {
    uint64_t            id;           /* private guest value, echoed in resp */
    uint64_t            setup;        /* the setup packet 8 bytes            */ 
    uint8_t             type;         /* Control Bulk Int Iso                */
    uint8_t             endpoint;     /* endpoint address and direction      */
    uint16_t            offset;       /* offset into data                    */ 
    usbif_request_len_t length;       /* of data not including setup packet  */ 
    uint8_t             nr_segments;  /* number of segments                  */
    uint8_t             flags;        /* 0x01 == SHORT_PACKET_OK             */
#ifdef XENVUSB_ISO
    uint16_t            nr_packets;
    uint32_t            startframe;
#else
    uint16_t            pad;
#endif
    grant_ref_t         gref[USBIF_URB_MAX_SEGMENTS_PER_REQUEST];
    uint32_t            pad;
};
typedef struct usbif_request usbif_request_t;

#define INDIRECT_PAGES_REQUIRED(_data_pages_) (((_data_pages_ -1)/INDIRECT_GREF_PAGES) + 1)
#define MAX_INDIRECT_PAGES USBIF_URB_MAX_SEGMENTS_PER_REQUEST
#define MAX_PAGES_FOR_INDIRECT_REQUEST (MAX_INDIRECT_PAGES * INDIRECT_GREF_PAGES)
#define MAX_PAGES_FOR_INDIRECT_ISO_REQUEST (MAX_PAGES_FOR_INDIRECT_REQUEST - 1)

struct usbifv2_request {
    uint64_t            id;           /* private guest value, echoed in resp */
    uint64_t            setup;        /* the setup packet 8 bytes            */ 
    uint8_t             type;         /* Control Bulk Int Iso                */
    uint8_t             endpoint;     /* endpoint address and direction      */
    uint16_t            offset;       /* offset into data                    */ 
    usbif_request_len_t length;       /* of data not including setup packet  */ 
    uint8_t             nr_segments;  /* number of segments                  */
    uint8_t             flags;        /* 0x01 == SHORT_PACKET_OK             */
#ifdef XENVUSB_ISO
    uint16_t            nr_packets;
    uint32_t            startframe;
#else
    uint16_t            pad;
#endif
    grant_ref_t         gref[USBIF_URB_MAX_SEGMENTS_PER_REQUEST_V2];
};
typedef struct usbifv2_request usbifv2_request_t;

struct usbif_response {
    uint64_t            id;              /* copied from request */
    usbif_request_len_t bytesTransferred; /* for ISO packets - number of error packets */
#ifdef XENVUSB_ISO  
    uint32_t            data;      /* for ISO ASAP - the first frame sent */
                                   /* for get speed request - the speed. rename? */
#endif
    int16_t             status;          /* USBIF_RSP_???       */  
    uint32_t            pad;
};
typedef struct usbif_response usbif_response_t;

struct iso_packet_info {
    uint32_t            offset; // set by frontend - offset into buffer of this packet
    uint16_t            length; // <= 1024 - set to 0 by frontend. Set to valid length by backend for IN packets
    int16_t             status; // set to 0 by frontend. Set to USBIF_USB_??? by backend.
};

DEFINE_RING_TYPES(usbif, struct usbif_request, struct usbif_response);



#define SHADOW_TAG 'wdhS'
typedef struct 
{
    usbif_request_t req;
    ULONG Tag;  // must be 'Shdw'
    BOOLEAN InUse; // Must be FALSE when allocated.
    PIRP  Irp;  // NULL if internal request
    PMDL  allocatedMdl;
    ULONG length; // ???
    BOOLEAN isReset;
    PVOID  isoPacketDescriptor;
    PMDL   isoPacketMdl;
    PVOID indirectPageMemory;
} usbif_shadow_t;


#define USB_RING_SIZE __RING_SIZE((usbif_sring *)0, PAGE_SIZE)
#define SHADOW_ENTRIES  USB_RING_SIZE
#define MAX_GRANT_ENTRIES 512
#define MAX_SHADOW_ENTRIES SHADOW_ENTRIES

#define INVALID_SHADOW_FREE_LIST_INDEX (USHORT) -1
