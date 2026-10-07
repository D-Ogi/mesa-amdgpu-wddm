/* SPDX-License-Identifier: MIT
 *
 * Host test of bc250_slice_wait.h, the sliced Present-idle wait of the hosted
 * UMD (K225). A GPU thread plays the scheduler: it signals the event, marks
 * the work complete, marks the device lost, or does nothing, after a delay.
 * Built and run by bc250_slice_wait.py.
 */
#include "bc250_slice_wait.h"

#include <stdio.h>

static volatile LONG done_flag, lost_flag;
static unsigned checks, failures;

static void
check(int ok, const char *what, DWORD ms)
{
   checks++;
   failures += !ok;
   printf("%s %s (%lu ms)\n", ok ? "PASS" : "FAIL", what, (unsigned long)ms);
}

static int
poll(void *ctx)
{
   (void)ctx;
   if (InterlockedCompareExchange(&lost_flag, 0, 0))
      return -1;
   return InterlockedCompareExchange(&done_flag, 0, 0) ? 1 : 0;
}

enum action { SIGNAL, COMPLETE, LOSE, NOTHING };
struct gpu {
   HANDLE event;
   enum action action;
   DWORD delay_ms;
};

static DWORD WINAPI
gpu_thread(void *arg)
{
   struct gpu *g = arg;
   Sleep(g->delay_ms);
   if (g->action == SIGNAL)
      SetEvent(g->event);
   else if (g->action == COMPLETE)
      InterlockedExchange(&done_flag, 1);
   else if (g->action == LOSE)
      InterlockedExchange(&lost_flag, 1);
   return 0;
}

static enum bc250_slice_wait_result
run(enum action action, DWORD delay_ms, DWORD slice_ms, DWORD total_ms, DWORD *waited, DWORD *elapsed)
{
   struct gpu g = {CreateEventA(NULL, FALSE, FALSE, NULL), action, delay_ms};
   done_flag = lost_flag = 0;
   HANDLE t = CreateThread(NULL, 0, gpu_thread, &g, 0, NULL);
   ULONGLONG t0 = GetTickCount64();
   enum bc250_slice_wait_result r = bc250_slice_wait(g.event, slice_ms, total_ms, poll, NULL, waited);
   *elapsed = (DWORD)(GetTickCount64() - t0);
   WaitForSingleObject(t, INFINITE);
   CloseHandle(t);
   CloseHandle(g.event);
   return r;
}

int
main(void)
{
   DWORD waited, ms;
   enum bc250_slice_wait_result r;

   /* The D1 shape: the work completes after about 15 slices. */
   r = run(SIGNAL, 300, 20, 5000, &waited, &ms);
   check(r == BC250_SLICE_WAIT_DONE && ms >= 280, "innocent: the event fires after about 15 slices, DONE", ms);

   r = run(COMPLETE, 100, 20, 5000, &waited, &ms);
   check(r == BC250_SLICE_WAIT_DONE && ms >= 80 && ms < 1000,
         "complete: the poll sees the fence at its value without the event, DONE at the next slice", ms);

   r = run(LOSE, 100, 20, 5000, &waited, &ms);
   check(r == BC250_SLICE_WAIT_LOST && ms < 1000, "lost: the poll sees the loss, LOST at the next slice", ms);

   r = run(NOTHING, 0, 20, 300, &waited, &ms);
   check(r == BC250_SLICE_WAIT_BOUND && waited >= 300 && ms < 2000, "bound: a live device past the total, BOUND", ms);

   r = run(SIGNAL, 0, 20, 5000, &waited, &ms);
   check(r == BC250_SLICE_WAIT_DONE && waited == 0, "prompt: an event that fires at once, DONE with 0 waited", ms);

   printf("checks=%u failures=%u\n", checks, failures);
   return failures ? 1 : 0;
}
