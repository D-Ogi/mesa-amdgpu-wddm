/*
 * Copyright 2020 Lag Free Games, LLC
 * All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sub license, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT.
 * IN NO EVENT SHALL VMWARE AND/OR ITS SUPPLIERS BE LIABLE FOR
 * ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include "memstream.h"

#include <stdlib.h>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <windows.h>

/* Distinguishes the temporary files of one process. */
static volatile LONG u_memstream_counter;
#endif

bool
u_memstream_open(struct u_memstream *mem, char **bufp, size_t *sizep)
{
#ifdef _GAMING_XBOX
   int err = tmpfile_s(&mem->f);
   mem->bufp = bufp;
   mem->sizep = sizep;

   return err == 0;
#elif defined(_WIN32)
   /* The stream is a temporary file that the system deletes when its last handle closes, also when the process ends
    * without u_memstream_close. Its name comes from the process id and a counter, not from GetTempFileName: that
    * function has 65535 names per prefix and probes the existing ones on every call, and the files of a stream whose
    * deletion failed used to fill that namespace, after which every open cost 65535 failed CreateFile calls.
    */
   char path[MAX_PATH];
   DWORD dwResult = GetTempPathA(MAX_PATH, path);
   if (dwResult == 0 || dwResult >= MAX_PATH)
      return false;

   for (unsigned attempt = 0; attempt < 8; attempt++) {
      int len = snprintf(mem->temp, sizeof(mem->temp), "%smesa-memstream-%lu-%ld-%lu.tmp", path,
                         (unsigned long)GetCurrentProcessId(), (long)InterlockedIncrement(&u_memstream_counter),
                         (unsigned long)GetTickCount());
      if (len < 0 || (size_t)len >= sizeof(mem->temp))
         return false;

      HANDLE h = CreateFileA(mem->temp, GENERIC_READ | GENERIC_WRITE, 0, NULL, CREATE_NEW,
                             FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, NULL);
      if (h == INVALID_HANDLE_VALUE) {
         if (GetLastError() == ERROR_FILE_EXISTS)
            continue;
         return false;
      }

      int fd = _open_osfhandle((intptr_t)h, _O_RDWR | _O_BINARY);
      if (fd == -1) {
         CloseHandle(h);
         return false;
      }
      FILE *f = _fdopen(fd, "w+b");
      if (f == NULL) {
         _close(fd);
         return false;
      }

      *bufp = NULL;
      *sizep = 0;
      mem->f = f;
      mem->bufp = bufp;
      mem->sizep = sizep;
      return true;
   }

   return false;
#else
   FILE *const f = open_memstream(bufp, sizep);
   mem->f = f;
   return f != NULL;
#endif
}

#ifdef _WIN32
static void
u_memstream_update_buffer(struct u_memstream *mem)
{
   FILE *const f = mem->f;
   long size = ftell(f);
   if (size > 0) {
      /* reserve space for the null terminator as well */
      if (*mem->bufp == NULL || *mem->sizep < size + 1)
         *mem->bufp = realloc(*mem->bufp, size + 1);

      fseek(f, 0, SEEK_SET);
      fread(*mem->bufp, 1, size, f);
      (*mem->bufp)[size] = '\0';
   }
   *mem->sizep = size;
}
#endif /* _WIN32 */

void
u_memstream_close(struct u_memstream *mem)
{
   FILE *const f = mem->f;

#ifdef _WIN32
   u_memstream_update_buffer(mem);
#endif

   /* On Windows this also deletes the file (FILE_FLAG_DELETE_ON_CLOSE). */
   fclose(f);
}

int
u_memstream_flush(struct u_memstream *mem)
{
#ifndef _WIN32
   return fflush(mem->f);
#else
   u_memstream_update_buffer(mem);
   return 0;
#endif /* _WIN32 */
}
