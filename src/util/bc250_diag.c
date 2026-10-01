/* SPDX-License-Identifier: MIT */
#include "bc250_diag.h"

#include <stdarg.h>

#ifdef _WIN32
#include <windows.h>
#endif

bool bc250_diag_enabled;

#ifdef _WIN32
static volatile LONG bc250_diag_error_lines;
#endif

void
bc250_diag_error(const char *format, ...)
{
   va_list ap;
   va_start(ap, format);
   if (bc250_diag_enabled) {
      vfprintf(stderr, format, ap);
   } else {
#ifdef _WIN32
      if (InterlockedIncrement(&bc250_diag_error_lines) <= 64) {
         char line[512];
         vsnprintf(line, sizeof(line), format, ap);
         OutputDebugStringA(line);
      }
#endif
   }
   va_end(ap);
}
