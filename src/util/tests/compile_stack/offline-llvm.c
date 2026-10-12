/* SPDX-License-Identifier: MIT */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "util/u_compile_stack.h"
#define main llvm_control_main
#include "gallium/drivers/llvmpipe/lp_test_main.c"
#undef main

struct task { bool direct; int result; };
static __declspec(noinline) void compile(void *opaque)
{
   struct task *t = opaque;
   char *args[] = {"compile-stack-llvm", "0"};
   t->result = llvm_control_main(2, args);
}
static DWORD WINAPI run(void *opaque)
{
   struct task *t = opaque;
   if (t->direct) compile(t);
   else if (!util_call_on_compile_stack("real llvmpipe LLVM arithmetic", compile, t)) return 2;
   return t->result;
}
int main(int argc, char **argv)
{
   SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
   _set_error_mode(_OUT_TO_STDERR);
   SetEnvironmentVariableA("MESA_COMPILE_STACK_STATS", "1");
   bool large = argc > 1 && !strcmp(argv[1], "--large");
   struct task t = {argc > 1, 3};
   HANDLE thread = CreateThread(NULL, large ? 8 * 1024 * 1024 : 64 * 1024,
      run, &t, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
   if (!thread || WaitForSingleObject(thread, 90000) != WAIT_OBJECT_0) return 4;
   DWORD result = 5;
   if (!GetExitCodeThread(thread, &result)) return 5;
   CloseHandle(thread);
   printf("llvmpipe LLVM: stack=%s result=%lu\n", large ? "8MiB control" : "64KiB caller", result);
   return (int)result;
}
