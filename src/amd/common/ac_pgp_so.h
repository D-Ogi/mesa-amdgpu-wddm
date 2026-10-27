/* SPDX-License-Identifier: MIT */
#ifndef AC_PGP_SO_H
#define AC_PGP_SO_H
#include <stdint.h>
#include <stdbool.h>
#define BC250_PGP_SO_STREAMS 4u
#define BC250_PGP_SO_COMPONENTS_PER_STREAM 128u
#define BC250_PGP_SO_SOURCES 512u
#define BC250_PGP_SO_DESTINATIONS 512u
#define BC250_PGP_SO_END 0xffffffffu
#define BC250_PGP_SO_CAPTURE 1u
#define BC250_PGP_SO_LINK 2u
#define BC250_PGP_SO_FULL 3u
#define BC250_PGP_SO_RASTER_COMPONENTS 128u
#define BC250_PGP_SO_SYSTEM_CLIP0 0u
#define BC250_PGP_SO_SYSTEM_CLIP1 1u
#define BC250_PGP_SO_SYSTEM_LAYER 2u
#define BC250_PGP_SO_SYSTEM_VIEWPORT 3u
#define BC250_PGP_SO_SYSTEM_PRIMITIVE_ID 4u
#define BC250_PGP_SO_SYSTEM_PARAMS 5u

/* Sources are synthetic XFB buffer * 128 + byte offset / 4. Capture libraries
 * use one synthetic buffer per GS stream, each with stride 512. Real output
 * buffers, gaps and strides exist only in the final mapping. */
struct bc250_pgp_so_destination {
   uint32_t buffer;
   uint32_t offset;
   uint32_t next;
};

struct bc250_pgp_so_table {
   uint32_t destination_count;
   uint32_t rasterized_stream;
   uint32_t buffer_mask;
   uint32_t buffer_stream[4];
   uint32_t stride[4];
   uint32_t head[BC250_PGP_SO_SOURCES];
   struct bc250_pgp_so_destination destinations[BC250_PGP_SO_DESTINATIONS];
   /* Pixel input register * 4 + component -> canonical capture word.
    * END supplies zero. Only the selected geometry stream may supply a word. */
   uint32_t raster_source[BC250_PGP_SO_RASTER_COMPONENTS];
   /* NIR system-varying slot -> PARAM register, sharing the same 32-register
    * allocation as generic inputs. END means the pixel shader does not use it. */
   uint32_t raster_system_param[BC250_PGP_SO_SYSTEM_PARAMS];
};

static inline bool
bc250_pgp_so_table_valid(const struct bc250_pgp_so_table *t)
{
   if (!t || t->destination_count > BC250_PGP_SO_DESTINATIONS || (t->buffer_mask & ~15u))
      return false;
   if (t->rasterized_stream >= 4 && t->rasterized_stream != BC250_PGP_SO_END)
      return false;
   for (unsigned i = 0; i < BC250_PGP_SO_RASTER_COMPONENTS; i++) {
      const uint32_t source = t->raster_source[i];
      if (source != BC250_PGP_SO_END && (source >= BC250_PGP_SO_SOURCES ||
          source / BC250_PGP_SO_COMPONENTS_PER_STREAM != t->rasterized_stream))
         return false;
   }
   uint32_t referenced[16] = {0};
   uint32_t system_used = 0;
   for (unsigned i = 0; i < BC250_PGP_SO_SYSTEM_PARAMS; i++) {
      const uint32_t param = t->raster_system_param[i];
      if (param == BC250_PGP_SO_END)
         continue;
      if (param >= 32 || (system_used & (1u << param)))
         return false;
      system_used |= 1u << param;
   }
   uint32_t used = 0, count = 0;
   for (unsigned b = 0; b < 4; b++) {
      if (((t->buffer_mask & (1u << b)) && t->buffer_stream[b] >= 4) ||
          t->stride[b] > 2048 || (t->stride[b] & 3))
         return false;
      if ((t->buffer_mask & (1u << b)) && !t->stride[b])
         return false;
   }
   for (unsigned source = 0; source < BC250_PGP_SO_SOURCES; source++) {
      uint32_t index = t->head[source];
      while (index != BC250_PGP_SO_END) {
         if (index >= t->destination_count || (referenced[index / 32] & (1u << (index % 32))))
            return false;
         referenced[index / 32] |= 1u << (index % 32);
         count++;
         const struct bc250_pgp_so_destination *d = &t->destinations[index];
         if (d->buffer >= 4 || !(t->buffer_mask & (1u << d->buffer)) ||
             t->buffer_stream[d->buffer] != source / 128 || (d->offset & 3) ||
             d->offset > 508 || d->offset + 4 > t->stride[d->buffer] ||
             (d->next != BC250_PGP_SO_END && d->next <= index))
            return false;
         used |= 1u << d->buffer;
         index = d->next;
      }
   }
   return count == t->destination_count && used == t->buffer_mask;
}


#endif
