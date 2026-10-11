/* SPDX-License-Identifier: MIT */

/* amdgpu-wddm: the log stream behind amdgpu_wddm_stdio.h on the system Vulkan ICD line. The sink is resolved once,
 * on the first line, from AMDGPU_WDDM_LOG. Nothing here touches stdout or stderr of the process: this line does not
 * redirect the application's streams (see the header). */
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "util/amdgpu_wddm_stdio.h"

static INIT_ONCE amdgpu_wddm_log_once = INIT_ONCE_STATIC_INIT;
/* Null means no output. Otherwise the C runtime's stderr or the AMDGPU_WDDM_LOG file. */
static FILE *amdgpu_wddm_log_sink;

static FILE *
amdgpu_wddm_log_open_append(const char *path)
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
amdgpu_wddm_log_resolve(PINIT_ONCE once, PVOID parameter, PVOID *context)
{
   (void)once;
   (void)parameter;
   (void)context;

   /* An executable built from this tree keeps its stderr: the policy is for a DLL in someone else's process. A
    * module that cannot be told apart is treated as the DLL. */
   HMODULE self = NULL;
   if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          (LPCSTR)&amdgpu_wddm_log_resolve, &self) &&
       self == GetModuleHandleA(NULL)) {
      amdgpu_wddm_log_sink = stderr;
      return TRUE;
   }

   char value[MAX_PATH + 8];
   DWORD length = GetEnvironmentVariableA("AMDGPU_WDDM_LOG", value, sizeof(value));
   if (!length || length >= sizeof(value))
      return TRUE;
   if (!strcmp(value, "stderr")) {
      amdgpu_wddm_log_sink = stderr;
      return TRUE;
   }
   /* A file that cannot be opened means no output, as in the shell and the engine. */
   if (!strncmp(value, "file:", 5) && value[5])
      amdgpu_wddm_log_sink = amdgpu_wddm_log_open_append(value + 5);
   return TRUE;
}

FILE *
amdgpu_wddm_log_stream(void)
{
   InitOnceExecuteOnce(&amdgpu_wddm_log_once, amdgpu_wddm_log_resolve, NULL, NULL);
   return amdgpu_wddm_log_sink;
}

int
amdgpu_wddm_log(const char *format, ...)
{
   FILE *out = amdgpu_wddm_log_stream();
   if (!out)
      return 0;
   va_list args;
   va_start(args, format);
   int written = vfprintf(out, format, args);
   va_end(args);
   return written;
}
