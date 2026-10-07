/* SPDX-License-Identifier: MIT
 *
 * A CPU wait for GPU work in slices, with a liveness poll between them.
 *
 * A wait that runs past one slice is not a device loss. When another device
 * hangs the engine, the work of this device waits behind it until the
 * scheduler resets the engine and resubmits the render packets of the other
 * devices. Only the device of the hung packet goes into the error state
 * ("TDR changes in Windows 8", engine reset). That takes TdrDelay and the
 * reset: 14.6 s in BC-250 lab trial D1 of KMD 0.7.216.17 with TdrDelay 10,
 * longer than the single 10 s wait that this helper replaces (K225).
 *
 * Host test: tests/bc250_slice_wait.py.
 */
#ifndef BC250_SLICE_WAIT_H
#define BC250_SLICE_WAIT_H

#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

enum bc250_slice_wait_result {
   BC250_SLICE_WAIT_DONE,   /* the event fired, or the poll saw the work complete */
   BC250_SLICE_WAIT_LOST,   /* the poll saw a device loss */
   BC250_SLICE_WAIT_BOUND,  /* the total bound ended with the device alive and the work pending */
   BC250_SLICE_WAIT_FAILED, /* WaitForSingleObject failed */
};

/* Returns < 0 when the device is lost, > 0 when the work completed, 0 when
 * the work is pending on a live device. */
typedef int (*bc250_slice_poll)(void *ctx);

static inline enum bc250_slice_wait_result
bc250_slice_wait(HANDLE event, DWORD slice_ms, DWORD total_ms, bc250_slice_poll poll, void *ctx, DWORD *waited_ms)
{
   DWORD waited = 0;
   if (!slice_ms)
      slice_ms = 1;
   if (total_ms < slice_ms)
      total_ms = slice_ms;
   for (;;) {
      DWORD step = total_ms - waited < slice_ms ? total_ms - waited : slice_ms;
      DWORD r = WaitForSingleObject(event, step);
      if (r == WAIT_OBJECT_0) {
         *waited_ms = waited;
         return BC250_SLICE_WAIT_DONE;
      }
      if (r != WAIT_TIMEOUT) {
         *waited_ms = waited;
         return BC250_SLICE_WAIT_FAILED;
      }
      waited += step;
      *waited_ms = waited;
      int state = poll(ctx);
      if (state < 0)
         return BC250_SLICE_WAIT_LOST;
      if (state > 0)
         return BC250_SLICE_WAIT_DONE;
      if (waited >= total_ms)
         return BC250_SLICE_WAIT_BOUND;
   }
}

#ifdef __cplusplus
}
#endif

#endif
