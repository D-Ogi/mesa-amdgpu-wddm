/* SPDX-License-Identifier: MIT
 * Standalone Windows regression: cl /O2 /W4 /WX compile_stack_win32.c
 * The real helper is included to inject Win32 allocation failure without a
 * production environment switch or a second implementation of its logic.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <float.h>

static volatile LONG fail_create;
static volatile LONG fail_convert;
static LPVOID test_create_fiber(SIZE_T commit, SIZE_T reserve, DWORD flags, LPFIBER_START_ROUTINE func, LPVOID data)
{
   if (fail_create) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return NULL; }
   return CreateFiberEx(commit, reserve, flags, func, data);
}
static LPVOID test_convert(LPVOID data, DWORD flags)
{
   if (fail_convert) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return NULL; }
   return ConvertThreadToFiberEx(data, flags);
}
#define CreateFiberEx test_create_fiber
#define ConvertThreadToFiberEx test_convert
#include "../u_compile_stack.c"
#undef CreateFiberEx
#undef ConvertThreadToFiberEx

static DWORD tls_slot, fls_slot;
static volatile LONG failures;
static __declspec(thread) int compiler_tls;
#define CHECK(expr) do { if (!(expr)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #expr); InterlockedIncrement(&failures); } } while (0)

struct task { DWORD tid; unsigned calls; };

static __declspec(noinline) unsigned
use_stack(unsigned depth)
{
   volatile unsigned char bytes[96 * 1024];
   for (unsigned i = 0; i < sizeof(bytes); i += 4096)
      bytes[i] = (unsigned char)(depth + i / 4096);
   unsigned result = depth ? use_stack(depth - 1) : 0;
   for (unsigned i = 0; i < sizeof(bytes); i += 4096)
      result += bytes[i];
   return result;
}

static void nested(void *opaque)
{
   struct task *t = opaque;
   CHECK(t->tid == GetCurrentThreadId());
   CHECK(compiler_tls == 37);
   CHECK(TlsGetValue(tls_slot) == t);
   CHECK(util_compile_stack_available() >= UTIL_COMPILE_STACK_MINIMUM);
   ++t->calls;
}

static void compile(void *opaque)
{
   struct task *t = opaque;
   unsigned int control = 0;
   _controlfp_s(&control, 0, 0);
   CHECK((control & _MCW_RC) == _RC_DOWN);
   CHECK(t->tid == GetCurrentThreadId());
   CHECK(util_compile_stack_available() >= UTIL_COMPILE_STACK_MINIMUM);
   CHECK(use_stack(8) > 0);
   CHECK(util_call_on_compile_stack("nested", nested, t));
   ++t->calls;
   _controlfp_s(&control, _RC_UP, _MCW_RC);
}

static __declspec(noinline) DWORD WINAPI small_thread(void *existing_fiber)
{
   volatile unsigned char occupied[16 * 1024];
   occupied[0] = 0x42;
   struct task task = {GetCurrentThreadId(), 0};
   void *fiber = existing_fiber ? ConvertThreadToFiberEx(&task,
      (uintptr_t)existing_fiber == 1 ? FIBER_FLAG_FLOAT_SWITCH : 0) : NULL;
   CHECK(!existing_fiber || fiber);
   TlsSetValue(tls_slot, &task);
   FlsSetValue(fls_slot, &task);
   compiler_tls = 37;
   unsigned int control = 0;
   _controlfp_s(&control, _RC_DOWN, _MCW_RC);
   CHECK(util_compile_stack_available() < 64 * 1024);
   SetLastError(0x7654);
   for (unsigned i = 0; i < 4; ++i)
      CHECK(util_call_on_compile_stack("96KiB frames", compile, &task));
   CHECK(GetLastError() == 0x7654);
   _controlfp_s(&control, 0, 0);
   CHECK((control & _MCW_RC) == _RC_DOWN);
   CHECK(task.calls == 8);
   CHECK(compiler_tls == 37 && TlsGetValue(tls_slot) == &task);
   CHECK(FlsGetValue(fls_slot) == &task);
   CHECK((bool)IsThreadAFiber() == (existing_fiber != NULL));
   if (fiber) { CHECK(GetCurrentFiber() == fiber); CHECK(GetFiberData() == &task); ConvertFiberToThread(); }
   CHECK(occupied[0] == 0x42);
   return 0;
}

static DWORD WINAPI negative_thread(void *arg)
{
   (void)arg;
   return use_stack(1);
}

int main(int argc, char **argv)
{
   SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
   if (argc == 2 && !strcmp(argv[1], "--negative")) {
      HANDLE t = CreateThread(NULL, 64 * 1024, negative_thread, NULL, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
      if (!t) return 2;
      WaitForSingleObject(t, 10000);
      return 3; /* A real stack overflow must have terminated this child. */
   }
   SetEnvironmentVariableA("MESA_COMPILE_STACK_STATS", "1");
   tls_slot = TlsAlloc(); fls_slot = FlsAlloc(NULL);
   CHECK(tls_slot != TLS_OUT_OF_INDEXES && fls_slot != FLS_OUT_OF_INDEXES);
   HANDLE threads[6];
   for (unsigned i = 0; i < 6; ++i) {
      threads[i] = CreateThread(NULL, 64 * 1024, small_thread, (void *)(uintptr_t)(i % 3), STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
      CHECK(threads[i]);
   }
   CHECK(WaitForMultipleObjects(6, threads, TRUE, 30000) == WAIT_OBJECT_0);
   for (unsigned i = 0; i < 6; ++i) CloseHandle(threads[i]);
   /* Fail closed without calling the callback or leaving the caller a fiber. */
   struct task task = {GetCurrentThreadId(), 0};
   fail_create = 1;
   CHECK(!util_call_on_compile_stack("allocation failure", nested, &task));
   CHECK(task.calls == 0 && !IsThreadAFiber());
   fail_create = 0; fail_convert = 1;
   CHECK(!util_call_on_compile_stack("conversion failure", nested, &task));
   CHECK(task.calls == 0 && !IsThreadAFiber());
   fail_convert = 0;
   TlsFree(tls_slot); FlsFree(fls_slot);
   printf("compile-stack: %ld failures; 64KiB callers, nesting, concurrency, existing fiber, TLS/FLS restoration, OOM\n", failures);
   return failures ? 1 : 0;
}
