/* SPDX-License-Identifier: MIT */
/*
 * Copyright 2026 amdgpu-wddm contributors
 *
 * Host test of radv_wddm2_mem_overflow.h (BD-096): the memory overflow policy switch and the
 * MakeResident flags of each policy. No GPU, no Vulkan, no Windows header.
 *
 * Usage: radv_wddm2_mem_overflow_test [--negative-control]
 * The negative control runs every case with each expectation inverted; it must fail every case.
 */
#include "../radv_wddm2_mem_overflow.h"

#include <stdio.h>
#include <string.h>

static int failures;
static int checks;
static bool invert;

#define CHECK(cond)                                                                                \
   do {                                                                                            \
      checks++;                                                                                    \
      if (!(cond) != invert) {                                                                     \
         failures++;                                                                               \
         printf("FAIL %s:%d: %s\n", __func__, __LINE__, #cond);                                    \
      }                                                                                            \
   } while (0)

static void
test_parse(void)
{
   enum radv_wddm2_mem_overflow p = RADV_WDDM2_MEM_OVERFLOW_LEGACY;
   CHECK(radv_wddm2_mem_overflow_parse("allow", &p) && p == RADV_WDDM2_MEM_OVERFLOW_ALLOW);
   CHECK(radv_wddm2_mem_overflow_parse("ALLOW", &p) && p == RADV_WDDM2_MEM_OVERFLOW_ALLOW);
   CHECK(radv_wddm2_mem_overflow_parse("Strict", &p) && p == RADV_WDDM2_MEM_OVERFLOW_STRICT);
   /* The legacy flags are not a user value. */
   CHECK(!radv_wddm2_mem_overflow_parse("legacy", &p));
   CHECK(!radv_wddm2_mem_overflow_parse("legacy-mustsucceed", &p));
   CHECK(!radv_wddm2_mem_overflow_parse("allow ", &p));
   CHECK(!radv_wddm2_mem_overflow_parse("", &p));
   CHECK(!radv_wddm2_mem_overflow_parse(NULL, &p));
   CHECK(!strcmp(radv_wddm2_mem_overflow_name(RADV_WDDM2_MEM_OVERFLOW_STRICT), "strict"));
}

static void
test_choose(void)
{
   struct radv_wddm2_mem_overflow_choice c;

   c = radv_wddm2_mem_overflow_choose(NULL, NULL, NULL, NULL);
   CHECK(c.policy == RADV_WDDM2_MEM_OVERFLOW_ALLOW && c.source == RADV_WDDM2_MEM_OVERFLOW_SOURCE_DEFAULT &&
         !c.invalid);
   CHECK(RADV_WDDM2_MEM_OVERFLOW_DEFAULT == RADV_WDDM2_MEM_OVERFLOW_ALLOW);
   c = radv_wddm2_mem_overflow_choose("", "", "", "");
   CHECK(c.policy == RADV_WDDM2_MEM_OVERFLOW_ALLOW && c.source == RADV_WDDM2_MEM_OVERFLOW_SOURCE_DEFAULT);

   /* Precedence: environment, then the per-application key, then the global key. */
   c = radv_wddm2_mem_overflow_choose(NULL, "strict", "allow", "allow");
   CHECK(c.policy == RADV_WDDM2_MEM_OVERFLOW_STRICT && c.source == RADV_WDDM2_MEM_OVERFLOW_SOURCE_ENV);
   c = radv_wddm2_mem_overflow_choose(NULL, NULL, "strict", "allow");
   CHECK(c.policy == RADV_WDDM2_MEM_OVERFLOW_STRICT &&
         c.source == RADV_WDDM2_MEM_OVERFLOW_SOURCE_APP_REGISTRY);
   c = radv_wddm2_mem_overflow_choose(NULL, "", "allow", "strict");
   CHECK(c.policy == RADV_WDDM2_MEM_OVERFLOW_ALLOW &&
         c.source == RADV_WDDM2_MEM_OVERFLOW_SOURCE_APP_REGISTRY);
   c = radv_wddm2_mem_overflow_choose(NULL, NULL, NULL, "STRICT");
   CHECK(c.policy == RADV_WDDM2_MEM_OVERFLOW_STRICT && c.source == RADV_WDDM2_MEM_OVERFLOW_SOURCE_REGISTRY);

   /* An invalid value gives the default and says so; it does not fall through to a lower source. */
   c = radv_wddm2_mem_overflow_choose(NULL, "never", NULL, "strict");
   CHECK(c.policy == RADV_WDDM2_MEM_OVERFLOW_ALLOW && c.source == RADV_WDDM2_MEM_OVERFLOW_SOURCE_ENV &&
         c.invalid);
   c = radv_wddm2_mem_overflow_choose(NULL, NULL, "invalid", NULL);
   CHECK(c.policy == RADV_WDDM2_MEM_OVERFLOW_ALLOW && c.invalid);
   /* The user cannot ask for the legacy flags through the user switch. */
   c = radv_wddm2_mem_overflow_choose(NULL, "legacy", NULL, NULL);
   CHECK(c.policy == RADV_WDDM2_MEM_OVERFLOW_ALLOW && c.invalid);

   /* Only the exact bisect value "1" selects the legacy flags, and it wins over everything. */
   c = radv_wddm2_mem_overflow_choose("1", "strict", "strict", "strict");
   CHECK(c.policy == RADV_WDDM2_MEM_OVERFLOW_LEGACY && c.source == RADV_WDDM2_MEM_OVERFLOW_SOURCE_BISECT);
   c = radv_wddm2_mem_overflow_choose("0", NULL, NULL, NULL);
   CHECK(c.policy == RADV_WDDM2_MEM_OVERFLOW_ALLOW && c.source == RADV_WDDM2_MEM_OVERFLOW_SOURCE_DEFAULT);
   c = radv_wddm2_mem_overflow_choose("11", NULL, NULL, NULL);
   CHECK(c.policy == RADV_WDDM2_MEM_OVERFLOW_ALLOW);
}

static void
test_bits(void)
{
   struct radv_wddm2_make_resident_bits b;

   /* allow: may go over the current budget (CantTrimFurther), never puts the device in error. */
   b = radv_wddm2_mem_overflow_bits(RADV_WDDM2_MEM_OVERFLOW_ALLOW);
   CHECK(b.cant_trim_further == 1 && b.must_succeed == 0);
   /* strict: fails over the current budget, never puts the device in error. */
   b = radv_wddm2_mem_overflow_bits(RADV_WDDM2_MEM_OVERFLOW_STRICT);
   CHECK(b.cant_trim_further == 0 && b.must_succeed == 0);
   /* legacy: exactly the flags of the ICDs before BD-096. */
   b = radv_wddm2_mem_overflow_bits(RADV_WDDM2_MEM_OVERFLOW_LEGACY);
   CHECK(b.cant_trim_further == 0 && b.must_succeed == 1);
   /* No user value can set MustSucceed. */
   b = radv_wddm2_mem_overflow_bits(radv_wddm2_mem_overflow_choose(NULL, "strict", NULL, NULL).policy);
   CHECK(b.must_succeed == 0);
   b = radv_wddm2_mem_overflow_bits(radv_wddm2_mem_overflow_choose(NULL, "allow", NULL, NULL).policy);
   CHECK(b.must_succeed == 0);
}

static void
test_app_key(void)
{
   char buf[128];
   CHECK(radv_wddm2_mem_overflow_app_key("llama-server.exe", buf, sizeof(buf)) &&
         !strcmp(buf, "SOFTWARE\\amdgpu-wddm\\Vulkan\\Applications\\llama-server.exe"));
   CHECK(!radv_wddm2_mem_overflow_app_key("", buf, sizeof(buf)));
   CHECK(!radv_wddm2_mem_overflow_app_key(NULL, buf, sizeof(buf)));
   CHECK(!radv_wddm2_mem_overflow_app_key("a\\b.exe", buf, sizeof(buf)));
   CHECK(!radv_wddm2_mem_overflow_app_key("a/b.exe", buf, sizeof(buf)));
   CHECK(!radv_wddm2_mem_overflow_app_key("llama-server.exe", buf, 20));
}

static const struct {
   const char *name;
   void (*fn)(void);
} tests[] = {
   {"parse", test_parse},
   {"choose", test_choose},
   {"bits", test_bits},
   {"app_key", test_app_key},
};

int
main(int argc, char **argv)
{
   int failed_tests = 0;
   invert = argc > 1 && !strcmp(argv[1], "--negative-control");
   for (unsigned i = 0; i < sizeof(tests) / sizeof(tests[0]); i++) {
      const int before = failures;
      tests[i].fn();
      if (failures != before) {
         failed_tests++;
         printf("FAIL %s\n", tests[i].name);
      } else {
         printf("PASS %s\n", tests[i].name);
      }
   }
   printf("mem overflow: %u tests, %d checks, %d failed checks%s\n",
          (unsigned)(sizeof(tests) / sizeof(tests[0])), checks, failures, invert ? " (negative control)" : "");
   return failed_tests ? 1 : 0;
}
