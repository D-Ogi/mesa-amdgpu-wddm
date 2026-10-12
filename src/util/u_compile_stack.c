/* SPDX-License-Identifier: MIT */
#include "u_compile_stack.h"

#ifdef _WIN32
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <fenv.h>

struct compile_stack_call {
   util_compile_stack_func func;
   void *data;
   void *caller;
   bool executed;
   size_t committed_peak_bound;
   size_t reserve;
   fenv_t caller_fenv;
};

static INIT_ONCE trace_once = INIT_ONCE_STATIC_INIT;
static bool trace_enabled;

static BOOL CALLBACK
compile_stack_trace_init(PINIT_ONCE once, PVOID param, PVOID *context)
{
   char value[2];
   (void)once;
   (void)param;
   (void)context;
   trace_enabled = GetEnvironmentVariableA("MESA_COMPILE_STACK_STATS", value, sizeof(value)) == 1 && value[0] == '1';
   return TRUE;
}

size_t
util_compile_stack_available(void)
{
   char marker;
   MEMORY_BASIC_INFORMATION memory;
   /* VirtualQuery works for both thread and fiber stacks, without relying on
    * private TEB offsets or the executable's default stack size.
    */
   if (!VirtualQuery(&marker, &memory, sizeof(memory)))
      return 0;
   const uintptr_t current = (uintptr_t)&marker;
   const uintptr_t bottom = (uintptr_t)memory.AllocationBase;
   const size_t emergency = 64 * 1024;
   return current > bottom && current - bottom > emergency ? current - bottom - emergency : 0;
}

static VOID WINAPI
compile_stack_main(void *opaque)
{
   struct compile_stack_call *call = opaque;
   /* Check the effective stack, not just CreateFiberEx's requested size. This
    * also guards accidental regression to the caller executable's PE default.
    */
   if (fesetenv(&call->caller_fenv) == 0 &&
       util_compile_stack_available() >= UTIL_COMPILE_STACK_MINIMUM) {
      call->executed = true;
      call->func(call->data);
   }
   MEMORY_BASIC_INFORMATION memory;
   if (trace_enabled && VirtualQuery(&call, &memory, sizeof(memory))) {
      /* A fresh fiber's stack commits pages as the guard moves down. They are
       * not decommitted on function return. This is a conservative high-water
       * bound, rounded up to pages and at least the 64 KiB initial commit; it
       * is NOT a sampled SP advertised as an exact maximum.
       */
      const uintptr_t bottom = (uintptr_t)memory.AllocationBase;
      const uintptr_t top = (uintptr_t)memory.BaseAddress + memory.RegionSize;
      call->reserve = top - bottom;
      /* VirtualQuery starts its forward scan at the queried page. Querying
       * the current SP alone therefore misses the pages below it. Walk this
       * allocation from its bottom and count all committed, non-guard pages.
       */
      for (uintptr_t address = bottom; address < top;) {
         if (!VirtualQuery((void *)address, &memory, sizeof(memory)) ||
             (uintptr_t)memory.AllocationBase != bottom || !memory.RegionSize)
            break;
         if (memory.State == MEM_COMMIT && !(memory.Protect & (PAGE_GUARD | PAGE_NOACCESS)))
            call->committed_peak_bound += memory.RegionSize;
         address = (uintptr_t)memory.BaseAddress + memory.RegionSize;
      }
   }
   SwitchToFiber(call->caller);
   /* A fiber entry must not return: Windows would terminate the OS thread. */
   abort();
}

bool
util_call_on_compile_stack(const char *label, util_compile_stack_func func, void *data)
{
   const DWORD last_error = GetLastError();
   if (util_compile_stack_available() >= UTIL_COMPILE_STACK_MINIMUM) {
      func(data);
      return true;
   }

   InitOnceExecuteOnce(&trace_once, compile_stack_trace_init, NULL, NULL);

   const bool converted = !IsThreadAFiber();
   void *caller = converted ? ConvertThreadToFiberEx(NULL, FIBER_FLAG_FLOAT_SWITCH) : GetCurrentFiber();
   if (!caller)
      return false;

   struct compile_stack_call call = {0};
   call.func = func;
   call.data = data;
   call.caller = caller;
   if (fegetenv(&call.caller_fenv) != 0) {
      if (converted)
         ConvertFiberToThread();
      return false;
   }
   void *fiber = CreateFiberEx(64 * 1024, UTIL_COMPILE_STACK_RESERVE, FIBER_FLAG_FLOAT_SWITCH,
                              compile_stack_main, &call);
   if (fiber) {
      SwitchToFiber(fiber);
      DeleteFiber(fiber);
   }
   if (converted)
      ConvertFiberToThread();
   /* A new fiber starts with its own FP environment. Also restore explicitly
    * for an application fiber that was created without FLOAT_SWITCH on x86. */
   fesetenv(&call.caller_fenv);
   if (trace_enabled) {
      fprintf(stderr, "mesa-compile-stack: %s called=%u reserve=%zu committed_peak_bound=%zu margin=%zu thread=%lu\n",
              label, call.executed, call.reserve, call.committed_peak_bound,
              call.reserve - call.committed_peak_bound, (unsigned long)GetCurrentThreadId());
   }
   SetLastError(last_error);
   return call.executed;
}
#else

size_t
util_compile_stack_available(void)
{
   return (size_t)-1;
}

bool
util_call_on_compile_stack(const char *label, util_compile_stack_func func, void *data)
{
   (void)label;
   func(data);
   return true;
}
#endif
