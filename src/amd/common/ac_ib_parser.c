/*
 * Copyright © 2023 Advanced Micro Devices, Inc.
 *
 * SPDX-License-Identifier: MIT
 */

#include "ac_debug.h"
#include <string.h>
#include <stdlib.h>

struct amd_ib_header {
   uint32_t ib_header_size;
   uint32_t len;
   uint32_t device_id;
   uint32_t flags;
   uint32_t ip_type;
};

static size_t
parse_header(FILE *f, enum amd_ip_type *ip_type, enum amd_gfx_level *gfx_level,
             enum radeon_family *family)
{
   uint32_t header_size;
   struct amd_ib_header header;
   size_t ret;

   ret = fread(&header_size, sizeof(header_size), 1, f);
   if (ret != 1)
      return 0;
   fseek(f, -sizeof(header_size), SEEK_CUR);

   ret = fread(&header, header_size, 1, f);
   if (ret != 1)
      return 0;

   switch (header.ip_type) {
   case 0:
      *ip_type = AMD_IP_SDMA;
      break;
   case 1:
      *ip_type = AMD_IP_GFX;
      break;
   default:
      fprintf(stderr, "Unsupport IP type %i\n", header.ip_type);
      break;
   }

   for (unsigned i = 0; i < CHIP_LAST; i++) {
      if (ac_get_pci_device_id(i) == header.device_id) {
         *family = i;
         *gfx_level = ac_get_gfx_level(i);
         break;
      }
   }

   return header.len;
}

static bool
parse_file(const char *filename, FILE *f, size_t size, enum amd_ip_type ip_type,
           enum amd_gfx_level gfx_level, enum radeon_family family)
{
   uint32_t *ib;

   if (size == 0) {
      fseek(f, 0, SEEK_END);
      size = ftell(f);
      fseek(f, 0, SEEK_SET);
   }

   ib = (uint32_t*)malloc(size);

   size_t ret = fread(ib, size, 1, f);
   if (ret != 1) {
      fprintf(stderr, "Can't read IB: %s\n", filename);
      free(ib);
      return false;
   }

   struct ac_ib_parser ib_parser = {
      .f = stdout,
      .ib = ib,
      .num_dw = size / 4,
      .gfx_level = gfx_level,
      .family = family,
      .ip_type = ip_type,
   };

   ac_parse_ib(&ib_parser, filename);
   free(ib);
   return true;
}

int main(int argc, char **argv)
{
   if (argc < 3) {
      fprintf(stderr, "Usage: [LLVM processor] [IB filenames]\n");
      return 1;
   }

   const char *gpu = argv[1];
   bool read_header = false;
   enum amd_gfx_level gfx_level = CLASS_UNKNOWN;
   enum radeon_family family = CHIP_UNKNOWN;
   enum amd_ip_type ip_type = AMD_IP_GFX;

   if (!strcmp(gpu, "auto")) {
      read_header = true;
   } else {
      for (unsigned i = 0; i < CHIP_LAST; i++) {
         if (!strcmp(gpu, ac_get_llvm_processor_name(i))) {
            family = i;
            gfx_level = ac_get_gfx_level(i);
            break;
         }
      }

      if (family == CHIP_UNKNOWN) {
         fprintf(stderr, "Unknown LLVM processor.\n");
         return 1;
      }
   }

   for (unsigned i = 2; i < argc; i++) {
      const char *filename = argv[i];

      FILE *f = fopen(filename, "r");
      if (!f) {
         fprintf(stderr, "Can't open IB: %s\n", filename);
         continue;
      }

      while (true) {
         size_t size = 0;
         if (read_header) {
            size = parse_header(f, &ip_type, &gfx_level, &family);
            if (size == 0)
               break;
         }
         if (!parse_file(filename, f, size, ip_type, gfx_level, family))
            break;
         if (!read_header)
            break;
      }

      fclose(f);
   }

   return 0;
}
