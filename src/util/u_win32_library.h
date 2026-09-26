/*
 * Copyright (c) 2026 D-Ogi
 * SPDX-License-Identifier: MIT
 */
#ifndef U_WIN32_LIBRARY_H
#define U_WIN32_LIBRARY_H
#include <windows.h>
#include <string.h>

/* An ICD must not enter app-local DXVK/vkd3d while initializing native WDDM.
 * Use a full system path: a bare name can return an already-loaded proxy DLL.
 */
static inline HMODULE
util_load_system_library(const wchar_t *name)
{
   wchar_t path[MAX_PATH];
   UINT len = GetSystemDirectoryW(path, MAX_PATH);
   size_t name_len = lstrlenW(name);
   if (!len || len >= MAX_PATH || name_len + len + 2 > MAX_PATH)
      return NULL;
   path[len++] = L'\\';
   memcpy(path + len, name, (name_len + 1) * sizeof(wchar_t));
   return LoadLibraryW(path);
}
#endif
