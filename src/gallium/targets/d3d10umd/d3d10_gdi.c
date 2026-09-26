/**************************************************************************
 *
 * Copyright 2012-2021 VMware, Inc.
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
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
 * THE COPYRIGHT HOLDERS, AUTHORS AND/OR ITS SUPPLIERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
 * OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
 * USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 *
 **************************************************************************/


/* BC250 experimental native D3D -> Zink path. Not a system UMD yet.
 * A nonzero adapter LUID is mandatory to prevent accidental GPU selection.
 * The bounded test supplies it from DXGI enumeration of the BC250 adapter.
 */
#include <stdint.h>
#include <stdlib.h>
#include <errno.h>
#include "util/u_debug.h"
#include "pipe/p_screen.h"
#include "target-helpers/inline_debug_helper.h"
#include "zink/zink_public.h"

extern struct pipe_screen *d3d10_create_screen(void);

struct pipe_screen *
d3d10_create_screen(void)
{
   const char *value = debug_get_option("BC250_D3D_ZINK_LUID", "");
   char *end;
   errno = 0;
   uint64_t luid = strtoull(value, &end, 16);
   if (errno || end == value || *end || !luid) {
      debug_printf("BC250 D3D Zink: explicit adapter LUID required\n");
      return NULL;
   }
   struct pipe_screen *screen = zink_win32_create_screen(luid);
   if (!screen)
      return NULL;
   debug_printf("BC250 D3D renderer: %s\n", screen->get_name(screen));
   return debug_screen_wrap(screen);
}
