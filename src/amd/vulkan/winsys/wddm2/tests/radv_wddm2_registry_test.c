/* SPDX-License-Identifier: MIT */
/*
 * Copyright 2026 amdgpu-wddm contributors
 *
 * Host test of radv_wddm2_registry.h: the registry view of the HKLM reads and the image file name of the
 * per-application keys. Build it as a 32-bit and as a 64-bit image (clang-cl --target=i686-pc-windows-msvc and
 * x86_64-pc-windows-msvc, with advapi32.lib); the registry cases read values that every Windows installation has, so
 * no administrator rights and no registry writes are necessary. The long-path case copies the test into a directory
 * deeper than MAX_PATH next to it and runs the copy; it is skipped when the host cannot start a process from there.
 *
 * Usage: radv_wddm2_registry_test [--negative-control]
 * The negative control runs every case with each expectation inverted; it must fail every case.
 */
#define _CRT_SECURE_NO_WARNINGS /* wcscat and _snwprintf on buffers sized for the longest path */
#include "../radv_wddm2_registry.h"

#include <stdio.h>
#include <wchar.h>

static int failures;
static int checks;
static bool invert;

#define CHECK(cond)                                                                                \
   do {                                                                                            \
      checks++;                                                                                    \
      if (!(cond) != invert) {                                                                     \
         failures++;                                                                               \
         printf("FAIL %s:%d: %s\n", __func__, __LINE__, #cond);                                    \
      }                                                                                            \
   } while (0)

#define CURRENT_VERSION "SOFTWARE\\Microsoft\\Windows\\CurrentVersion"

static bool
ends_with(const char *s, const char *tail)
{
   const size_t a = strlen(s), b = strlen(tail);
   return a >= b && !_stricmp(s + a - b, tail);
}

/* The 64-bit view on a 32-bit image, no view flag on a 64-bit image. */
static void
test_view(void)
{
   CHECK(radv_wddm2_registry_view() == (sizeof(void *) == 4 ? (DWORD)RRF_SUBKEY_WOW6464KEY : 0u));
}

/* ProgramFilesDir has one value in each view: "...\Program Files" in the 64-bit view and
 * "...\Program Files (x86)" in the 32-bit view. The reader must give the 64-bit value on both images. */
static void
test_view_value(void)
{
   char buf[MAX_PATH], view32[MAX_PATH];
   DWORD size = sizeof(view32);
   const char *v = radv_wddm2_read_hklm_sz(CURRENT_VERSION, "ProgramFilesDir", buf, sizeof(buf));
   CHECK(v == buf);
   CHECK(v && !ends_with(v, "(x86)"));
   CHECK(v && ends_with(v, "\\Program Files"));
   CHECK(RegGetValueA(HKEY_LOCAL_MACHINE, CURRENT_VERSION, "ProgramFilesDir",
                      RRF_RT_REG_SZ | RRF_SUBKEY_WOW6432KEY, NULL, view32, &size) == ERROR_SUCCESS &&
         ends_with(view32, "(x86)"));
}

/* An absent value is NULL; a value of another type is "invalid", never a guess. */
static void
test_absent_invalid(void)
{
   char buf[64];
   CHECK(radv_wddm2_read_hklm_sz(CURRENT_VERSION, "radv-wddm2-registry-test-no-such-value", buf, sizeof(buf)) ==
         NULL);
   CHECK(radv_wddm2_read_hklm_sz("SOFTWARE\\radv-wddm2-registry-test-no-such-key", "x", buf, sizeof(buf)) == NULL);
   const char *v = radv_wddm2_read_hklm_sz("SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", "InstallDate", buf,
                                           sizeof(buf));
   CHECK(v && !strcmp(v, "invalid"));
}

static void
test_exe_name(void)
{
   char name[MAX_PATH], tiny[8];
   CHECK(radv_wddm2_exe_name(name, sizeof(name)) && !_stricmp(name, "radv_wddm2_registry_test.exe"));
   CHECK(!radv_wddm2_exe_name(tiny, sizeof(tiny)) && tiny[0] == 0);
   CHECK(!radv_wddm2_exe_name(name, 0));
}

/* The child: the image name in a directory deeper than MAX_PATH. */
static int
long_path_child(void)
{
   char name[MAX_PATH];
   wchar_t path[32768];
   const DWORD n = GetModuleFileNameW(NULL, path, 32768);
   const bool deep = n > MAX_PATH && n < 32768;
   const bool ok = deep && radv_wddm2_exe_name(name, sizeof(name)) && !_stricmp(name, "radv_wddm2_registry_test.exe");
   printf("child: path length %lu, name %s\n", (unsigned long)n, ok ? name : "(wrong)");
   return ok ? 0 : 1;
}

static void
test_long_path(void)
{
   static wchar_t self[32768], dir[32768], copy[32768], cmd[33000];
   const DWORD n = GetModuleFileNameW(NULL, self, 32768);
   wchar_t *slash = n && n < 32768 ? wcsrchr(self, L'\\') : NULL;
   CHECK(slash != NULL);
   if (!slash)
      return;
   _snwprintf(dir, 32768, L"\\\\?\\%.*ls\\registry_test_long_path", (int)(slash - self), self);
   const size_t base = wcslen(dir);
   CreateDirectoryW(dir, NULL);
   unsigned depth = 0;
   while (wcslen(dir) < base + MAX_PATH) {
      wcscat(dir, L"\\directory_name_of_forty_characters_long");
      CreateDirectoryW(dir, NULL);
      depth++;
   }
   _snwprintf(copy, 32768, L"%ls\\radv_wddm2_registry_test.exe", dir);
   _snwprintf(cmd, 33000, L"\"%ls\" --long-path-child", copy);
   const bool copied = CopyFileW(self, copy, FALSE);
   CHECK(copied);
   if (copied) {
      STARTUPINFOW si = {.cb = sizeof(si)};
      PROCESS_INFORMATION pi;
      if (CreateProcessW(copy, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
         DWORD code = 1;
         WaitForSingleObject(pi.hProcess, 30000);
         GetExitCodeProcess(pi.hProcess, &code);
         CloseHandle(pi.hThread);
         CloseHandle(pi.hProcess);
         CHECK(code == 0);
      } else {
         printf("SKIP long_path: CreateProcessW error %lu\n", GetLastError());
      }
      DeleteFileW(copy);
   }
   for (; depth > 0; depth--) {
      RemoveDirectoryW(dir);
      *wcsrchr(dir, L'\\') = 0;
   }
   RemoveDirectoryW(dir);
}

static const struct {
   const char *name;
   void (*fn)(void);
} tests[] = {
   {"view", test_view},
   {"view_value", test_view_value},
   {"absent_invalid", test_absent_invalid},
   {"exe_name", test_exe_name},
   {"long_path", test_long_path},
};

int
main(int argc, char **argv)
{
   if (argc > 1 && !strcmp(argv[1], "--long-path-child"))
      return long_path_child();
   invert = argc > 1 && !strcmp(argv[1], "--negative-control");
   int failed_tests = 0;
   for (unsigned i = 0; i < sizeof(tests) / sizeof(tests[0]); i++) {
      const int before = failures;
      tests[i].fn();
      const bool ok = failures == before;
      failed_tests += !ok;
      printf("%s %s\n", ok ? "PASS" : "FAIL", tests[i].name);
   }
   printf("registry: %u tests, %d checks, %d failed checks, %u-bit image%s\n",
          (unsigned)(sizeof(tests) / sizeof(tests[0])), checks, failures, (unsigned)(sizeof(void *) * 8),
          invert ? " (negative control)" : "");
   return failed_tests ? 1 : 0;
}
