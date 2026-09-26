/*
 * Copyright © 2021 Intel Corporation
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

#include "vk_async_event.h"

#include <assert.h>
#include <stdio.h>

#include "util/macros.h"

#ifndef _WIN32
#include <errno.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <unistd.h>
#else
#include <stdbool.h>
#include <windows.h>
#endif

VkResult
vk_async_event_create(HANDLE *event_out)
{
#ifdef _WIN32
   HANDLE event = CreateEventA(NULL, true, false, NULL);
   if (event == NULL)
      return VK_ERROR_UNKNOWN;
#else
   int event = eventfd(0, EFD_CLOEXEC);
   if (event < 0)
      return VK_ERROR_UNKNOWN;
#endif

   *event_out = (void *)(intptr_t)event;

   return VK_SUCCESS;
}

VkResult
vk_async_event_wait(HANDLE event, uint64_t rel_timeout_ns)
{
   /* Both poll() and WaitForSingleObject() take a relative timeout in
    * milliseconds as a 32-bit number.  For poll(), it's signed.
    */
   uint64_t rel_timeout_ms = DIV_ROUND_UP(rel_timeout_ns, 1000 * 1000);
   if (rel_timeout_ms > INT32_MAX)
      rel_timeout_ms = INT32_MAX;

#ifdef _WIN32
   DWORD ret = WaitForSingleObject(event, rel_timeout_ms);
   switch (ret) {
   case WAIT_TIMEOUT:
      return VK_TIMEOUT;
   case WAIT_OBJECT_0:
      return VK_SUCCESS;
   default:
      fprintf(stderr, "WaitForSingleObject failed: 0x%X\n", ret);
      return VK_ERROR_UNKNOWN;
   }
#else
   struct pollfd event_poll = {
      .fd = (intptr_t)event,
      .events = POLLIN,
   };
   int ret = poll(&event_poll, 1, rel_timeout_ms);
   if (ret < 0 && (errno == EINTR || errno == EAGAIN)) {
      /* Treat this as an early timeout.  The caller loops anyway */
      fprintf(stderr, "TIMEOUT\n");
      return VK_TIMEOUT;
   } else if (ret < 0) {
      return VK_ERROR_UNKNOWN;
   } else if (ret > 0) {
      assert(event_poll.revents & POLLIN);
      return VK_SUCCESS;
   } else { 
      /* No events */
      fprintf(stderr, "no event\n");
      return VK_TIMEOUT;
   }
#endif
}

void
vk_async_event_close(HANDLE event)
{
#ifdef _WIN32
   CloseHandle(event);
#else
   close((intptr_t)event);
#endif
}
