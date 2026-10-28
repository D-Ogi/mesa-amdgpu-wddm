/* SPDX-License-Identifier: MIT */
/*
 * Copyright 2026 amdgpu-wddm contributors
 *
 * The present route of the 32-bit system Vulkan ICD. This line has the GDI route only: the DXGI route of the x64
 * system ICD (its radv_wddm2_wsi.c and radv_wddm2_wsi_route.h) is not ported to it. The ICD still reads the switch
 * of that route, AMDGPU_WDDM_VK_WSI in the environment, else the REG_SZ value WsiRoute of
 * HKLM\SOFTWARE\amdgpu-wddm\Vulkan in the 64-bit view, with the same names and the same precedence, so that its
 * log line says what was asked and why the route is GDI. tests/radv_wddm2_wsi_x86_route_test.c tests the rules.
 * No Windows header.
 */
#ifndef RADV_WDDM2_WSI_X86_ROUTE_H
#define RADV_WDDM2_WSI_X86_ROUTE_H

#include <stdbool.h>
#include <stddef.h>

/* The routes and sources by the names of the x64 ICD's log line (radv_wddm2_wsi_route.h). */
enum radv_wddm2_wsi_x86_asked {
   RADV_WDDM2_WSI_X86_ASKED_GDI = 0,
   RADV_WDDM2_WSI_X86_ASKED_DXGI = 1,
   RADV_WDDM2_WSI_X86_ASKED_DXGI_COMPOSITION = 2,
};

struct radv_wddm2_wsi_x86_route {
   enum radv_wddm2_wsi_x86_asked asked; /* the x64 default (dxgi) when nothing is set */
   const char *source;                  /* "default", "env" or "registry" */
   bool invalid;                        /* the source named a value that is not a route */
   const char *reason;                  /* why the route is GDI */
};

static inline const char *
radv_wddm2_wsi_x86_asked_name(enum radv_wddm2_wsi_x86_asked asked)
{
   switch (asked) {
   case RADV_WDDM2_WSI_X86_ASKED_GDI: return "gdi";
   case RADV_WDDM2_WSI_X86_ASKED_DXGI: return "dxgi";
   case RADV_WDDM2_WSI_X86_ASKED_DXGI_COMPOSITION: return "dxgi-composition";
   default: return "unknown";
   }
}

static inline bool
radv_wddm2_wsi_x86_ieq(const char *a, const char *b)
{
   for (;; a++, b++) {
      char ca = *a, cb = *b;
      if (ca >= 'A' && ca <= 'Z')
         ca = (char)(ca - 'A' + 'a');
      if (cb >= 'A' && cb <= 'Z')
         cb = (char)(cb - 'A' + 'a');
      if (ca != cb)
         return false;
      if (!ca)
         return true;
   }
}

/* env and reg are the raw values, NULL when absent; an empty string counts as absent. reg is "invalid" for a
 * value that is not a short REG_SZ (radv_wddm2_read_hklm_sz). The route is always GDI; reason says why:
 *   asked          GDI was asked for
 *   invalid-value  the source named no route (the x64 ICD then takes GDI as well)
 *   x86-gdi-only   a DXGI route was asked for, or is the default, and this ICD has none
 */
static inline struct radv_wddm2_wsi_x86_route
radv_wddm2_wsi_x86_route_choose(const char *env, const char *reg)
{
   struct radv_wddm2_wsi_x86_route r = {RADV_WDDM2_WSI_X86_ASKED_DXGI, "default", false, "x86-gdi-only"};
   const char *text = NULL;

   if (env && env[0]) {
      text = env;
      r.source = "env";
   } else if (reg && reg[0]) {
      text = reg;
      r.source = "registry";
   }
   if (!text)
      return r;
   if (radv_wddm2_wsi_x86_ieq(text, "gdi")) {
      r.asked = RADV_WDDM2_WSI_X86_ASKED_GDI;
      r.reason = "asked";
   } else if (radv_wddm2_wsi_x86_ieq(text, "dxgi")) {
      r.asked = RADV_WDDM2_WSI_X86_ASKED_DXGI;
   } else if (radv_wddm2_wsi_x86_ieq(text, "dxgi-composition")) {
      r.asked = RADV_WDDM2_WSI_X86_ASKED_DXGI_COMPOSITION;
   } else {
      r.asked = RADV_WDDM2_WSI_X86_ASKED_GDI;
      r.invalid = true;
      r.reason = "invalid-value";
   }
   return r;
}

#endif /* RADV_WDDM2_WSI_X86_ROUTE_H */
