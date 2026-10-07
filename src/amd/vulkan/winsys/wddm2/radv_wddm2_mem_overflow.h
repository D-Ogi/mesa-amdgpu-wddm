/* SPDX-License-Identifier: MIT */
/*
 * Copyright 2026 amdgpu-wddm contributors
 *
 * radv_wddm2_mem_overflow.h - what a BC-250 Vulkan device does when a new allocation does not fit in
 * the process's video memory budget (BD-096). Plain C with no Windows or Vulkan header, so a host test
 * drives the production rules (tests/radv_wddm2_mem_overflow_test.c).
 *
 * Every allocation of this winsys is made resident at creation (radv_wddm2_bo.c, all_resident) with
 * D3DKMTMakeResident. The policy selects the D3DDDI_MAKERESIDENT_FLAGS of those calls. The flag text is
 * from d3dukmdt.h of WDK/SDK 10.0.26100 and the D3DDDI_MAKERESIDENT_FLAGS reference page:
 *
 *   CantTrimFurther  "When set, MakeResidentCb will succeed even if the request puts the application over
 *                     the current budget. MakeResidentCb will still fail if the request puts the application
 *                     over the maximum budget." For a UMD that has already trimmed what it can.
 *   MustSucceed      "When set, instructs MakeResidentCb to put the device in error if the resource cannot be
 *                     made resident." The page adds: "This flag may only be set if CantTrimFurther is also set."
 *
 * Policies:
 *   ALLOW   (default, user value "allow")  CantTrimFurther = 1, MustSucceed = 0. An allocation may take the
 *           process over its current budget; video memory manager then pages other memory out to make room,
 *           and the application runs slower instead of failing. It still fails, with
 *           VK_ERROR_OUT_OF_DEVICE_MEMORY, above the maximum budget. Never a device error.
 *   STRICT  (user value "strict")          CantTrimFurther = 0, MustSucceed = 0. An allocation that would take
 *           the process over its current budget fails with VK_ERROR_OUT_OF_DEVICE_MEMORY, so the
 *           application can choose a smaller working set (for example a smaller GPU offload). Never a
 *           device error.
 *   LEGACY  (not a user value)             CantTrimFurther = 0, MustSucceed = 1: the flags of every ICD before
 *           BD-096. A failed MakeResident puts the device in error, and the next submission returns
 *           STATUS_GRAPHICS_GPU_EXCEPTION_ON_DEVICE (VK_ERROR_DEVICE_LOST). Only the bisect switch
 *           BC250_MAKERESIDENT_LEGACY=1 selects it, for a negative control on the lab.
 *
 * Precedence (the first source that names a value wins; an empty string counts as absent):
 *   1. environment BC250_MAKERESIDENT_LEGACY=1          (bisect only)
 *   2. environment AMDGPU_WDDM_VK_MEM_OVERFLOW
 *   3. REG_SZ HKLM\SOFTWARE\amdgpu-wddm\Vulkan\Applications\<exe>\MemoryOverflow   (<exe> = file name of
 *      the process image, for example "llama-server.exe"; case does not matter)
 *   4. REG_SZ HKLM\SOFTWARE\amdgpu-wddm\Vulkan\MemoryOverflow
 *   5. the default, ALLOW
 * Values: "allow", "strict" (case does not matter). Another value selects the default and is reported as
 * invalid, with its source, on the device's log line.
 */
#ifndef RADV_WDDM2_MEM_OVERFLOW_H
#define RADV_WDDM2_MEM_OVERFLOW_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

enum radv_wddm2_mem_overflow {
   RADV_WDDM2_MEM_OVERFLOW_ALLOW = 0,
   RADV_WDDM2_MEM_OVERFLOW_STRICT = 1,
   RADV_WDDM2_MEM_OVERFLOW_LEGACY = 2,
};

#define RADV_WDDM2_MEM_OVERFLOW_DEFAULT RADV_WDDM2_MEM_OVERFLOW_ALLOW

enum radv_wddm2_mem_overflow_source {
   RADV_WDDM2_MEM_OVERFLOW_SOURCE_DEFAULT = 0,
   RADV_WDDM2_MEM_OVERFLOW_SOURCE_BISECT = 1,
   RADV_WDDM2_MEM_OVERFLOW_SOURCE_ENV = 2,
   RADV_WDDM2_MEM_OVERFLOW_SOURCE_APP_REGISTRY = 3,
   RADV_WDDM2_MEM_OVERFLOW_SOURCE_REGISTRY = 4,
};

struct radv_wddm2_mem_overflow_choice {
   enum radv_wddm2_mem_overflow policy;
   enum radv_wddm2_mem_overflow_source source;
   bool invalid; /* the source named a value that is not a policy; policy is the default */
};

/* The two D3DDDI_MAKERESIDENT_FLAGS bits a policy sets. */
struct radv_wddm2_make_resident_bits {
   unsigned cant_trim_further;
   unsigned must_succeed;
};

static inline const char *
radv_wddm2_mem_overflow_name(enum radv_wddm2_mem_overflow policy)
{
   switch (policy) {
   case RADV_WDDM2_MEM_OVERFLOW_ALLOW: return "allow";
   case RADV_WDDM2_MEM_OVERFLOW_STRICT: return "strict";
   case RADV_WDDM2_MEM_OVERFLOW_LEGACY: return "legacy-mustsucceed";
   default: return "unknown";
   }
}

static inline const char *
radv_wddm2_mem_overflow_source_name(enum radv_wddm2_mem_overflow_source source)
{
   switch (source) {
   case RADV_WDDM2_MEM_OVERFLOW_SOURCE_DEFAULT: return "default";
   case RADV_WDDM2_MEM_OVERFLOW_SOURCE_BISECT: return "env-bisect";
   case RADV_WDDM2_MEM_OVERFLOW_SOURCE_ENV: return "env";
   case RADV_WDDM2_MEM_OVERFLOW_SOURCE_APP_REGISTRY: return "registry-app";
   case RADV_WDDM2_MEM_OVERFLOW_SOURCE_REGISTRY: return "registry";
   default: return "unknown";
   }
}

static inline bool
radv_wddm2_mem_overflow_ieq(const char *a, const char *b)
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

/* Returns true and sets *out when text names a user policy. "legacy" is not a user value. */
static inline bool
radv_wddm2_mem_overflow_parse(const char *text, enum radv_wddm2_mem_overflow *out)
{
   if (!text)
      return false;
   if (radv_wddm2_mem_overflow_ieq(text, "allow")) {
      *out = RADV_WDDM2_MEM_OVERFLOW_ALLOW;
      return true;
   }
   if (radv_wddm2_mem_overflow_ieq(text, "strict")) {
      *out = RADV_WDDM2_MEM_OVERFLOW_STRICT;
      return true;
   }
   return false;
}

/* bisect, env, app_reg and reg are the raw values, NULL when absent. */
static inline struct radv_wddm2_mem_overflow_choice
radv_wddm2_mem_overflow_choose(const char *bisect, const char *env, const char *app_reg, const char *reg)
{
   struct radv_wddm2_mem_overflow_choice choice = {
      RADV_WDDM2_MEM_OVERFLOW_DEFAULT, RADV_WDDM2_MEM_OVERFLOW_SOURCE_DEFAULT, false,
   };
   const char *text = NULL;

   if (bisect && bisect[0] == '1' && !bisect[1]) {
      choice.policy = RADV_WDDM2_MEM_OVERFLOW_LEGACY;
      choice.source = RADV_WDDM2_MEM_OVERFLOW_SOURCE_BISECT;
      return choice;
   }
   if (env && env[0]) {
      text = env;
      choice.source = RADV_WDDM2_MEM_OVERFLOW_SOURCE_ENV;
   } else if (app_reg && app_reg[0]) {
      text = app_reg;
      choice.source = RADV_WDDM2_MEM_OVERFLOW_SOURCE_APP_REGISTRY;
   } else if (reg && reg[0]) {
      text = reg;
      choice.source = RADV_WDDM2_MEM_OVERFLOW_SOURCE_REGISTRY;
   }
   if (!text)
      return choice;
   if (!radv_wddm2_mem_overflow_parse(text, &choice.policy)) {
      choice.policy = RADV_WDDM2_MEM_OVERFLOW_DEFAULT;
      choice.invalid = true;
   }
   return choice;
}

static inline struct radv_wddm2_make_resident_bits
radv_wddm2_mem_overflow_bits(enum radv_wddm2_mem_overflow policy)
{
   struct radv_wddm2_make_resident_bits bits = {0, 0};
   switch (policy) {
   case RADV_WDDM2_MEM_OVERFLOW_STRICT:
      break;
   case RADV_WDDM2_MEM_OVERFLOW_LEGACY:
      bits.must_succeed = 1; /* exactly the flags of the ICDs before BD-096 */
      break;
   case RADV_WDDM2_MEM_OVERFLOW_ALLOW:
   default:
      bits.cant_trim_further = 1;
      break;
   }
   return bits;
}

/* The registry subkey of the per-application value: "SOFTWARE\amdgpu-wddm\Vulkan\Applications\<exe>".
 * exe is the base name of the process image (no directory). Returns false when exe is empty, holds a
 * path separator, or the key does not fit in buf.
 */
#define RADV_WDDM2_VK_KEY "SOFTWARE\\amdgpu-wddm\\Vulkan"
#define RADV_WDDM2_VK_APPS_KEY RADV_WDDM2_VK_KEY "\\Applications\\"
#define RADV_WDDM2_MEM_OVERFLOW_VALUE "MemoryOverflow"

static inline bool
radv_wddm2_mem_overflow_app_key(const char *exe, char *buf, size_t size)
{
   static const char prefix[] = RADV_WDDM2_VK_APPS_KEY;
   size_t used = 0;
   if (!exe || !exe[0] || !buf || !size)
      return false;
   for (const char *p = prefix; *p; p++) {
      if (used + 1 >= size)
         return false;
      buf[used++] = *p;
   }
   for (const char *p = exe; *p; p++) {
      if (*p == '\\' || *p == '/')
         return false;
      if (used + 1 >= size)
         return false;
      buf[used++] = *p;
   }
   buf[used] = 0;
   return true;
}

#ifdef __cplusplus
}
#endif

#endif /* RADV_WDDM2_MEM_OVERFLOW_H */
