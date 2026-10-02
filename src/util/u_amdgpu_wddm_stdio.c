/* SPDX-License-Identifier: MIT */

/* amdgpu-wddm: the streams behind amdgpu_wddm_stdio.h. This file also gets that header forced in, so it reaches the
 * C runtime's own streams through __acrt_iob_func and never names stdout or stderr. */
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "util/amdgpu_wddm_stdio.h"

static INIT_ONCE amdgpu_wddm_stdio_once = INIT_ONCE_STATIC_INIT;
/* Null: the C runtime's own streams. Otherwise NUL or the AMDGPU_WDDM_LOG file, for both streams. */
static FILE *amdgpu_wddm_stdio_sink;

static FILE *
amdgpu_wddm_stdio_open_append(const char *path)
{
   /* Append only and shared: the D3D12 shell and the engine may write to the same file, and so may other
    * processes; FILE_APPEND_DATA keeps each write whole at the end of the file. */
   HANDLE handle = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
   if (handle == INVALID_HANDLE_VALUE)
      return NULL;
   int fd = _open_osfhandle((intptr_t)handle, _O_APPEND | _O_WRONLY);
   if (fd < 0) {
      CloseHandle(handle);
      return NULL;
   }
   FILE *file = _fdopen(fd, "a");
   if (!file) {
      _close(fd);
      return NULL;
   }
   setvbuf(file, NULL, _IONBF, 0);
   return file;
}

static BOOL CALLBACK
amdgpu_wddm_stdio_resolve(PINIT_ONCE once, PVOID parameter, PVOID *context)
{
   (void)once;
   (void)parameter;
   (void)context;

   /* An executable built from this tree keeps its streams: the policy is for a DLL in someone else's process. */
   HMODULE self = NULL;
   if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)&amdgpu_wddm_stdio_resolve, &self) ||
       self == GetModuleHandleA(NULL))
      return TRUE;

   char value[MAX_PATH + 8];
   DWORD length = GetEnvironmentVariableA("AMDGPU_WDDM_LOG", value, sizeof(value));
   bool valid = length && length < sizeof(value);
   if (valid && !strcmp(value, "stderr"))
      return TRUE;
   if (valid && !strncmp(value, "file:", 5) && value[5])
      amdgpu_wddm_stdio_sink = amdgpu_wddm_stdio_open_append(value + 5);
   /* An explicit Mesa debug variable asks for its output on stderr, as an explicit VKD3D_DEBUG does in the engine:
    * RADV_DEBUG (psocachestats and the rest), MESA_SHADER_CACHE_SHOW_STATS, MESA_LOG and the compiler dumps. Only
    * the file sink takes precedence. */
   static const char *const explicit_debug[] = {
      "RADV_DEBUG", "ACO_DEBUG", "NIR_DEBUG", "MESA_DEBUG", "MESA_LOG", "MESA_SHADER_CACHE_SHOW_STATS",
   };
   for (unsigned i = 0; !amdgpu_wddm_stdio_sink && i < sizeof(explicit_debug) / sizeof(explicit_debug[0]); i++) {
      if (GetEnvironmentVariableA(explicit_debug[i], NULL, 0))
         return TRUE;
   }
   /* A file that cannot be opened means no output, as in the shell and the engine. */
   if (!amdgpu_wddm_stdio_sink)
      amdgpu_wddm_stdio_sink = fopen("NUL", "w");
   return TRUE;
}

FILE *
amdgpu_wddm_stdio(int fd)
{
   InitOnceExecuteOnce(&amdgpu_wddm_stdio_once, amdgpu_wddm_stdio_resolve, NULL, NULL);
   if (amdgpu_wddm_stdio_sink)
      return amdgpu_wddm_stdio_sink;
   return __acrt_iob_func(fd == 1 ? 1 : 2);
}

int
amdgpu_wddm_stdio_puts(const char *text)
{
   FILE *out = amdgpu_wddm_stdio(1);
   return fputs(text, out) < 0 || fputc('\n', out) == EOF ? EOF : 0;
}

int
amdgpu_wddm_stdio_putchar(int c)
{
   return fputc(c, amdgpu_wddm_stdio(1));
}
