COPYRIGHT = """\
/*
 * Copyright 2020 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sub license, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT.
 * IN NO EVENT SHALL VMWARE AND/OR ITS SUPPLIERS BE LIABLE FOR
 * ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */
"""

import argparse
import math
import os

from mako.template import Template


# We generate a static hash table for entry point lookup
# (vkGetProcAddress). We use a linear congruential generator for our hash
# function and a power-of-two size table. The prime numbers are determined
# experimentally.

TEMPLATE_H = Template(COPYRIGHT + """\
/* This file generated from ${filename}, don't edit directly. */

#ifndef VK_WDDM_DISPATCH_TABLE_H
#define VK_WDDM_DISPATCH_TABLE_H

#include <stdint.h>

 #ifdef _WIN32

 #ifndef NSTATUS
 #define NTSTATUS LONG
 #define STATUS_SUCCESS                  ((NTSTATUS)(0))
 #define STATUS_WAIT_0                   ((NTSTATUS)(0x00000000L)) 
 #define STATUS_OBJECT_NAME_INVALID	     ((NTSTATUS)(0xC0000033L))
 #define STATUS_DEVICE_REMOVED           ((NTSTATUS)(0xC00002B6L))
 #define STATUS_INVALID_HANDLE           ((NTSTATUS)(0xC0000008L))
 #define STATUS_ILLEGAL_INSTRUCTION      ((NTSTATUS)(0xC000001DL))
 #define STATUS_NOT_IMPLEMENTED          ((NTSTATUS)(0xC0000002L))
 #define STATUS_PENDING                  ((NTSTATUS)(0x00000103L))
 #define STATUS_ACCESS_DENIED            ((NTSTATUS)(0xC0000022L))
 #define STATUS_BUFFER_TOO_SMALL         ((NTSTATUS)(0xC0000023L))
 #define STATUS_OBJECT_TYPE_MISMATCH     ((NTSTATUS)(0xC0000024L))
 #define STATUS_GRAPHICS_ALLOCATION_BUSY ((NTSTATUS)(0xC01E0102L))
 #define STATUS_NOT_SUPPORTED            ((NTSTATUS)(0xC00000BBL))
 #define STATUS_TIMEOUT                  ((NTSTATUS)(0x00000102L))
 #define STATUS_INVALID_PARAMETER        ((NTSTATUS)(0xC000000DL))
 #define STATUS_NO_MEMORY                ((NTSTATUS)(0xC0000017L))
 #define STATUS_OBJECT_NAME_COLLISION    ((NTSTATUS)(0xC0000035L))
 #define STATUS_OBJECT_NAME_NOT_FOUND    ((NTSTATUS)(0xC0000034L))
 #define STATUS_UNSUCCESSFUL             ((NTSTATUS)(0xC0000001L))
 #define STATUS_INVALID_PARAMETER        ((NTSTATUS)(0xC000000DL))
 #define NT_SUCCESS(status)              (status >= 0)
 #endif

 #define UMDF_USING_NTSTATUS
 #include <windows.h>
 #else
 #include <assert.h>
 #include "wsl/winadapter.h"
 #define MAKEWORD(a,b) ((WORD)(((BYTE)(a))|(((WORD)((BYTE)(b)))<<8)))
 #endif

#include <d3dkmthk.h>
#include <stdio.h>
#include <vulkan/vulkan_core.h>

#ifdef __cplusplus
extern "C" {
#endif

struct vk_wddm2_dispatch_table {
% for e in entrypoints:
  PFND3DKMT_${e[0].upper()} ${e[0]};
% endfor
};

struct vk_wddm2_dispatch_table *
vk_wddm2_dispatch_table_get(void);

#define WDDM2_DISPATCH(FUNC) vk_wddm2_dispatch_table_get()->FUNC

void
print_hex_data(FILE *fp, const void *data, uint32_t size);

#ifdef __cplusplus
}
#endif

#endif /* VK_WDDM_DISPATCH_TABLE_H */
""")

TEMPLATE_C = Template(COPYRIGHT + """\
/* This file generated from ${filename}, don't edit directly. */

#include "vk_wddm2_dispatch_table.h"

#include "util/log.h"
#include "util/u_call_once.h"
#include "util/u_dl.h"

#include "string.h"

 #ifdef _WIN32
 #define DXCORE_LIBNAME "gdi32"
 #else
 #define DXCORE_LIBNAME "dxcore"
 #endif

static struct vk_wddm2_dispatch_table table;

<%def name="arg(e)">
% if e[1] is None:
CONST D3DKMT_${e[0].upper()} *arg
% else:
${e[1]}
% endif
</%def>

% for e in entrypoints:
static NTSTATUS
${e[0]}_not_supported(${arg(e)})
{
  fprintf(stderr, "D3DKMT${e[0]} is not supported\\n");
  return STATUS_NOT_SUPPORTED;
}

% endfor

static void
initialize_dispatch_table(void)
{
   struct util_dl_library *dxcore = util_dl_open(UTIL_DL_PREFIX DXCORE_LIBNAME UTIL_DL_EXT);
   if (!dxcore)
      fprintf(stderr, "Failed to load DXCore\\n");

   % for e in entrypoints:
   table.${e[0]} = dxcore ? (PFND3DKMT_${e[0].upper()})util_dl_get_proc_address(dxcore, "D3DKMT${e[0]}") : NULL;
   if (table.${e[0]} == NULL)
      table.${e[0]} = ${e[0]}_not_supported;
   % endfor
}

struct vk_wddm2_dispatch_table *
vk_wddm2_dispatch_table_get(void)
{
   static util_once_flag flag = UTIL_ONCE_FLAG_INIT;
   util_call_once(&flag, initialize_dispatch_table);

   return &table;
}

void
print_hex_data(FILE *fp, const void *data, uint32_t size)
{
    for (uint32_t offset = 0; offset < size; offset += sizeof(uint32_t)) {
        if (offset % 32 == 0) {
            if (offset != 0)
                fprintf(fp, "\\n");
            fprintf(fp, "  ");
        }

        fprintf(fp, "  0x%08x", *(uint32_t *)((char *)data + offset));
    }

    fprintf(fp, "\\n");
}

""")

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--out-c', help='Output C file.')
    parser.add_argument('--out-h', help='Output H file.')
    args = parser.parse_args()

    entrypoints = [
      ('QueryAdapterInfo', None),
      ('QueryStatistics', None),
      ('QueryVideoMemoryInfo', 'D3DKMT_QUERYVIDEOMEMORYINFO *arg'),
      ('OpenAdapterFromLuid', 'D3DKMT_OPENADAPTERFROMLUID *arg'),
      ('CloseAdapter', None),
      ('CreateDevice', 'D3DKMT_CREATEDEVICE *arg'),
      ('DestroyDevice', None),
      ('GetDeviceState', 'D3DKMT_GETDEVICESTATE *arg'),
      ('CreatePagingQueue', 'D3DKMT_CREATEPAGINGQUEUE *arg'),
      ('DestroyPagingQueue', 'D3DDDI_DESTROYPAGINGQUEUE *arg'),
      ('CreateContextVirtual', 'D3DKMT_CREATECONTEXTVIRTUAL *arg'),
      ('DestroyContext', None),
      ('CreateHwQueue', 'D3DKMT_CREATEHWQUEUE *arg'),
      ('DestroyHwQueue', None),
      ('SubmitCommand', None),
      ('SubmitCommandToHwQueue', None),
      ('CreateAllocation2', 'D3DKMT_CREATEALLOCATION *arg'),
      ('DestroyAllocation2', None),
      ('ReserveGpuVirtualAddress', 'D3DDDI_RESERVEGPUVIRTUALADDRESS *arg'),
      ('UpdateGpuVirtualAddress', None),
      ('MapGpuVirtualAddress', 'D3DDDI_MAPGPUVIRTUALADDRESS *arg'),
      ('FreeGpuVirtualAddress', None),
      ('MakeResident', 'D3DDDI_MAKERESIDENT *arg'),
      ('ShareObjects', 'UINT arg1, CONST D3DKMT_HANDLE *arg2, POBJECT_ATTRIBUTES arg3, DWORD arg4, HANDLE *arg5'),
      ('QueryResourceInfoFromNtHandle', 'D3DKMT_QUERYRESOURCEINFOFROMNTHANDLE *arg'),
      ('OpenResourceFromNtHandle', 'D3DKMT_OPENRESOURCEFROMNTHANDLE *arg'),
      ('Evict', 'D3DKMT_EVICT *arg'),
      ('Lock2', 'D3DKMT_LOCK2 *arg'),
      ('Unlock2', None),
      ('CreateSynchronizationObject2', 'D3DKMT_CREATESYNCHRONIZATIONOBJECT2 *arg'),
      ('DestroySynchronizationObject', None),
      ('OpenSyncObjectFromNtHandle2', 'D3DKMT_OPENSYNCOBJECTFROMNTHANDLE2 *arg'),
      ('WaitForSynchronizationObjectFromCpu', None),
      ('WaitForSynchronizationObjectFromGpu', None),
      ('SubmitWaitForSyncObjectsToHwQueue', None),
      ('SubmitSignalSyncObjectsToHwQueue', None),
      ('SignalSynchronizationObjectFromCpu', None),
      ('SignalSynchronizationObjectFromGpu', None),
      ('SignalSynchronizationObjectFromGpu2', None),
    ]

    # For outputting entrypoints.h we generate a anv_EntryPoint() prototype
    # per entry point.
    try:
        if args.out_h:
            with open(args.out_h, 'w', encoding='utf-8') as f:
                f.write(TEMPLATE_H.render(entrypoints=entrypoints,
                                          filename=os.path.basename(__file__)))
        if args.out_c:
            with open(args.out_c, 'w', encoding='utf-8') as f:
                f.write(TEMPLATE_C.render(entrypoints=entrypoints,
                                          filename=os.path.basename(__file__)))
    except Exception:
        # In the event there's an error, this imports some helpers from mako
        # to print a useful stack trace and prints it, then exits with
        # status 1, if python is run with debug; otherwise it just raises
        # the exception
        import sys
        from mako import exceptions
        print(exceptions.text_error_template().render(), file=sys.stderr)
        sys.exit(1)


if __name__ == '__main__':
    main()
