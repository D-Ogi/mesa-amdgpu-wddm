/*
 * Copyright © 2020 Advanced Micro Devices, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

/* A collection of unit tests for u_process.c */

#include "util/detect_os.h"
#include "util/os_misc.h"
#include "util/u_process.h"
#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <limits.h>
#include <stdlib.h>

#if DETECT_OS_WINDOWS
#include <windows.h>
#if !defined(PATH_MAX)
#define PATH_MAX MAX_PATH
#endif
#endif

static bool error = false;

static void
expect_equal_str(const char *expected, const char *actual, const char *test)
{
   if (strcmp(expected, actual)) {
      fprintf (stderr, "Error: Test '%s' failed:\n\t"
               "Expected=\"%s\", Actual=\"%s\"\n",
               test, expected, actual);
      error = true;
   }
}

static void
test_util_get_process_name (void)
{
#if DETECT_OS_WINDOWS
   const char *expected = "process_test.exe";
#else
   const char *expected = "process_test";
#endif

   const char *name_override = os_get_option("MESA_PROCESS_NAME");
   if (name_override)
      expected = name_override;

   const char *name = util_get_process_name();
   expect_equal_str(expected, name, "util_get_process_name");
}

static void posixify_path(char *path) {
   /* Always using posix separator '/' to check path equal */
   char *p = path;
   for (; *p != '\0'; p += 1) {
      if (*p == '\\') {
         *p = '/';
      }
   }
}

/* This test gets the real path from Meson (BUILD_FULL_PATH env var),
 * and compares it to the output of util_get_process_exec_path.
 */
static void
test_util_get_process_exec_path (void)
{
   char path[PATH_MAX];
   if (util_get_process_exec_path(path, PATH_MAX) == 0) {
      error = true;
      return;
   }
   posixify_path(path);
   char* build_path = os_get_option_dup("BUILD_FULL_PATH");
   if (!build_path) {
      fprintf(stderr, "BUILD_FULL_PATH environment variable should be set\n");
      error = true;
      return;
   }
   posixify_path(build_path);
#ifdef __CYGWIN__
   int i = strlen(build_path) - 4;
   if ((i > 0) && (strcmp(&build_path[i], ".exe") == 0))
      build_path[i] = 0;
#endif
   expect_equal_str(build_path, path, "test_util_get_process_exec_path");
   free(build_path);
}

#if DETECT_OS_WINDOWS
/* In the copy of this test in a directory deeper than MAX_PATH: the process
 * name is still the file name, and a buffer too short for the path gives 0,
 * not a cut path.
 */
static void
test_long_path_child(void)
{
   expect_equal_str("process_test.exe", util_get_process_name(),
                    "util_get_process_name (long path)");
   char short_path[PATH_MAX];
   if (util_get_process_exec_path(short_path, sizeof(short_path)) != 0) {
      fprintf(stderr, "util_get_process_exec_path (long path): a cut path for a short buffer\n");
      error = true;
   }
   static char long_path[32768];
   if (util_get_process_exec_path(long_path, sizeof(long_path)) <= MAX_PATH) {
      fprintf(stderr, "util_get_process_exec_path (long path): no path longer than MAX_PATH\n");
      error = true;
   }
}

/* Copies this executable into a directory whose path is longer than MAX_PATH
 * and runs the copy with PROCESS_TEST_LONG_PATH_CHILD set. A host that cannot
 * start a process from such a path (long paths disabled) skips the test.
 */
static void
test_long_path(void)
{
   static wchar_t self[32768], dir[32768], copy[32768];
   DWORD n = GetModuleFileNameW(NULL, self, ARRAYSIZE(self));
   if (!n || n >= ARRAYSIZE(self)) {
      error = true;
      return;
   }
   wchar_t *slash = wcsrchr(self, L'\\');
   if (!slash) {
      error = true;
      return;
   }
   _snwprintf(dir, ARRAYSIZE(dir), L"\\\\?\\%.*ls\\process_test_long_path", (int)(slash - self), self);
   const size_t base = wcslen(dir);
   CreateDirectoryW(dir, NULL);
   unsigned depth = 0;
   while (wcslen(dir) < base + MAX_PATH) {
      wcscat(dir, L"\\directory_name_of_forty_characters_long");
      CreateDirectoryW(dir, NULL);
      depth++;
   }
   _snwprintf(copy, ARRAYSIZE(copy), L"%ls\\process_test.exe", dir);
   bool skip = false;
   if (!CopyFileW(self, copy, FALSE)) {
      fprintf(stderr, "long path: cannot copy the test (error %lu)\n", GetLastError());
      error = true;
   } else {
      STARTUPINFOW si = { .cb = sizeof(si) };
      PROCESS_INFORMATION pi;
      SetEnvironmentVariableA("PROCESS_TEST_LONG_PATH_CHILD", "1");
      if (CreateProcessW(copy, NULL, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
         DWORD code = 1;
         WaitForSingleObject(pi.hProcess, INFINITE);
         GetExitCodeProcess(pi.hProcess, &code);
         CloseHandle(pi.hThread);
         CloseHandle(pi.hProcess);
         if (code != 0) {
            fprintf(stderr, "long path: the copy failed (exit %lu)\n", code);
            error = true;
         }
      } else {
         fprintf(stderr, "long path: SKIP, cannot start a process from a long path (error %lu)\n",
                 GetLastError());
         skip = true;
      }
      SetEnvironmentVariableA("PROCESS_TEST_LONG_PATH_CHILD", NULL);
      DeleteFileW(copy);
   }
   for (; depth > 0; depth--) {
      RemoveDirectoryW(dir);
      *wcsrchr(dir, L'\\') = 0;
   }
   RemoveDirectoryW(dir);
   if (!skip && !error)
      printf("long path: ok\n");
}
#endif

int
main (void)
{
#if DETECT_OS_WINDOWS
   if (os_get_option("PROCESS_TEST_LONG_PATH_CHILD")) {
      test_long_path_child();
      return error ? 1 : 0;
   }
#endif
   test_util_get_process_name();
   test_util_get_process_exec_path();
#if DETECT_OS_WINDOWS
   /* The copy would see the name override instead of its file name. */
   if (!os_get_option("MESA_PROCESS_NAME"))
      test_long_path();
#endif

   return error ? 1 : 0;
}
