/* SPDX-License-Identifier: MIT */
/*
 * Copyright 2026 amdgpu-wddm contributors
 *
 * The registry and image name reads of the winsys: one REG_SZ value of HKLM, and the file name of the process
 * image for the per-application keys. tests/radv_wddm2_registry_test.c tests them on the host, as a 32-bit and a
 * 64-bit image. Include it after the Windows headers of the including file.
 */
#ifndef RADV_WDDM2_REGISTRY_H
#define RADV_WDDM2_REGISTRY_H

#include <windows.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

/* The 64-bit view on both images. The installer and the control application write HKLM\SOFTWARE\amdgpu-wddm in
 * the 64-bit view, so a 32-bit image that does not ask for that view reads the empty WOW6432Node copy. The same
 * view as radv_wddm2_wsi_read_route of the x64 system ICD and the app-settings reader of the D3D shells. 0 on x64
 * keeps that image as it was.
 */
static inline DWORD
radv_wddm2_registry_view(void)
{
   return sizeof(void *) == 4 ? RRF_SUBKEY_WOW6464KEY : 0;
}

/* One REG_SZ value of HKLM\subkey into buf. NULL when the value is absent, "invalid" when it is there but
 * not a short string (never guess a policy from it), as radv_wddm2_wsi_read_route does for WsiRoute. */
static inline const char *
radv_wddm2_read_hklm_sz(const char *subkey, const char *value, char *buf, DWORD size)
{
   const DWORD capacity = size;
   const LSTATUS status = RegGetValueA(HKEY_LOCAL_MACHINE, subkey, value, RRF_RT_REG_SZ | radv_wddm2_registry_view(),
                                       NULL, buf, &size);
   if (status == ERROR_SUCCESS) {
      buf[capacity - 1] = 0;
      return buf;
   }
   if (status == ERROR_FILE_NOT_FOUND || status == ERROR_PATH_NOT_FOUND)
      return NULL;
   return "invalid";
}

/* The file name of the process image (game.exe) into out. The path can be longer than MAX_PATH (a long-path-aware
 * process, an application in a deep directory): the buffer grows until the path fits, up to the longest path
 * Windows has. False when the path cannot be read or the name does not fit in size.
 */
static inline bool
radv_wddm2_exe_name(char *out, size_t size)
{
   if (!size)
      return false;
   out[0] = 0;
   for (DWORD capacity = MAX_PATH; capacity <= 65536; capacity *= 2) {
      char *path = (char *)malloc(capacity);
      if (!path)
         return false;
      const DWORD n = GetModuleFileNameA(NULL, path, capacity);
      if (n && n < capacity) {
         const char *name = path;
         for (const char *p = path; *p; p++) {
            if (*p == '\\' || *p == '/')
               name = p + 1;
         }
         const size_t length = strlen(name);
         const bool fits = length < size;
         if (fits)
            memcpy(out, name, length + 1);
         free(path);
         return fits;
      }
      free(path);
      if (!n)
         return false;
   }
   return false;
}

#endif /* RADV_WDDM2_REGISTRY_H */
