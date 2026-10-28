/* SPDX-License-Identifier: MIT */
/*
 * Copyright 2026 amdgpu-wddm contributors
 *
 * Host test of radv_wddm2_app_settings.h (the reader of the per-application graphics settings in the
 * system Vulkan ICD) and wsi_win32_present_policy.h (what VSync, FrameRateLimit and MaxFrameLatency
 * change in a present): precedence, ranges, wrong types, environment strings, the excluded
 * process, the log lines, the present mode, the frame rate clock and the frame latency ring. No GPU,
 * no Vulkan device and no Windows header: the registry and the environment are doubles.
 *
 * Build and run (from the Mesa source root, with the Vulkan headers of include/):
 *   clang-cl /nologo /W3 /WX /Iinclude src/amd/vulkan/winsys/wddm2/tests/radv_wddm2_app_settings_test.c
 *            /Fe:radv_wddm2_app_settings_test.exe
 *   radv_wddm2_app_settings_test [--negative-control]
 * The negative control runs every case with one expectation inverted; it must fail, which shows
 * that a broken rule is reported.
 */
#include "../radv_wddm2_app_settings.h"
#include "../../../../../vulkan/wsi/wsi_win32_present_policy.h"

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

/* ---- doubles ---- */

struct fake_value {
   const wchar_t *application; /* NULL: the global key */
   const wchar_t *value;
   enum radv_wddm2_setting_read read;
   uint32_t data;
};

struct fake_env {
   const char *name;
   const char *text;
};

struct fake {
   const struct fake_value *values;
   unsigned value_count;
   const struct fake_env *env;
   unsigned env_count;
   unsigned registry_calls;
};

static bool
same_app(const wchar_t *a, const wchar_t *b)
{
   if (!a || !b)
      return a == b;
   return radv_wddm2_settings_wieq(a, b);
}

static enum radv_wddm2_setting_read
fake_registry(void *context, const wchar_t *application, const wchar_t *value, uint32_t *out)
{
   struct fake *f = (struct fake *)context;
   f->registry_calls++;
   for (unsigned i = 0; i < f->value_count; ++i) {
      const struct fake_value *v = &f->values[i];
      if (same_app(v->application, application) && !wcscmp(v->value, value)) {
         if (v->read == RADV_WDDM2_SETTING_READ_VALUE)
            *out = v->data;
         return v->read;
      }
   }
   return RADV_WDDM2_SETTING_READ_ABSENT;
}

static bool
fake_environment(void *context, const char *name, char *text, size_t size)
{
   struct fake *f = (struct fake *)context;
   for (unsigned i = 0; i < f->env_count; ++i) {
      if (!strcmp(f->env[i].name, name)) {
         radv_wddm2_settings_copy(text, size, f->env[i].text);
         return true;
      }
   }
   return false;
}

static void
resolve(struct fake *f, const wchar_t *app, struct radv_wddm2_settings *s)
{
   const struct radv_wddm2_setting_sources sources = {f, fake_registry, fake_environment};
   radv_wddm2_settings_resolve(app, &sources, s);
}

#define V(app, name, data) {app, name, RADV_WDDM2_SETTING_READ_VALUE, data}
#define WRONG(app, name) {app, name, RADV_WDDM2_SETTING_READ_WRONG_TYPE, 0}

/* ---- the reader ---- */

static void
test_nothing_set(void)
{
   struct fake f = {0};
   struct radv_wddm2_settings s;
   resolve(&f, L"game.exe", &s);
   for (unsigned i = 0; i < RADV_WDDM2_SETTING_COUNT; ++i)
      CHECK(!radv_wddm2_settings_has(&s, (enum radv_wddm2_setting)i));
   CHECK(s.rejection_count == 0);
   CHECK(!s.excluded);
}

static void
test_precedence(void)
{
   const struct fake_value values[] = {
      V(NULL, L"FrameRateLimit", 60),
      V(L"game.exe", L"FrameRateLimit", 144),
      V(NULL, L"VSync", 1),
      V(L"game.exe", L"Anisotropy", 8),
      V(NULL, L"Anisotropy", 16),
      V(NULL, L"MaxFrameLatency", 2),
   };
   const struct fake_env env[] = {{"AMDGPU_WDDM_MAX_FRAME_LATENCY", "1"}};
   struct fake f = {values, 6, env, 1, 0};
   struct radv_wddm2_settings s;
   resolve(&f, L"game.exe", &s);
   CHECK(s.values[RADV_WDDM2_SETTING_FRAME_RATE_LIMIT].value == 144);
   CHECK(s.values[RADV_WDDM2_SETTING_FRAME_RATE_LIMIT].source == RADV_WDDM2_SETTING_SOURCE_APPLICATION);
   CHECK(s.values[RADV_WDDM2_SETTING_VSYNC].value == 1);
   CHECK(s.values[RADV_WDDM2_SETTING_VSYNC].source == RADV_WDDM2_SETTING_SOURCE_GLOBAL);
   CHECK(s.values[RADV_WDDM2_SETTING_ANISOTROPY].value == 8);
   CHECK(s.values[RADV_WDDM2_SETTING_MAX_FRAME_LATENCY].value == 1);
   CHECK(s.values[RADV_WDDM2_SETTING_MAX_FRAME_LATENCY].source == RADV_WDDM2_SETTING_SOURCE_ENVIRONMENT);
   CHECK(s.rejection_count == 0);

   /* Another application: only the global values. Key names are not case-sensitive. */
   resolve(&f, L"other.exe", &s);
   CHECK(s.values[RADV_WDDM2_SETTING_FRAME_RATE_LIMIT].value == 60);
   CHECK(s.values[RADV_WDDM2_SETTING_ANISOTROPY].value == 16);
   resolve(&f, L"GAME.EXE", &s);
   CHECK(s.values[RADV_WDDM2_SETTING_FRAME_RATE_LIMIT].value == 144);

   /* No image name: the application key is never read. */
   f.registry_calls = 0;
   resolve(&f, NULL, &s);
   CHECK(s.values[RADV_WDDM2_SETTING_FRAME_RATE_LIMIT].value == 60);
   CHECK(f.registry_calls == RADV_WDDM2_SETTING_COUNT - 1); /* the environment gave one */
}

static void
test_ranges(void)
{
   struct {
      enum radv_wddm2_setting setting;
      uint32_t value;
      bool valid;
   } cases[] = {
      {RADV_WDDM2_SETTING_FRAME_RATE_LIMIT, 0, true},
      {RADV_WDDM2_SETTING_FRAME_RATE_LIMIT, 19, false},
      {RADV_WDDM2_SETTING_FRAME_RATE_LIMIT, 20, true},
      {RADV_WDDM2_SETTING_FRAME_RATE_LIMIT, 300, true},
      {RADV_WDDM2_SETTING_FRAME_RATE_LIMIT, 301, false},
      {RADV_WDDM2_SETTING_VSYNC, 0, true},
      {RADV_WDDM2_SETTING_VSYNC, 1, true},
      {RADV_WDDM2_SETTING_VSYNC, 2, false},
      {RADV_WDDM2_SETTING_ANISOTROPY, 0, false},
      {RADV_WDDM2_SETTING_ANISOTROPY, 1, true},
      {RADV_WDDM2_SETTING_ANISOTROPY, 3, false},
      {RADV_WDDM2_SETTING_ANISOTROPY, 16, true},
      {RADV_WDDM2_SETTING_ANISOTROPY, 32, false},
      {RADV_WDDM2_SETTING_MAX_FRAME_LATENCY, 0, false},
      {RADV_WDDM2_SETTING_MAX_FRAME_LATENCY, 1, true},
      {RADV_WDDM2_SETTING_MAX_FRAME_LATENCY, 3, true},
      {RADV_WDDM2_SETTING_MAX_FRAME_LATENCY, 4, false},
   };
   for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i)
      CHECK(radv_wddm2_setting_valid(cases[i].setting, cases[i].value) == cases[i].valid);
}

static void
test_rejections_fall_through(void)
{
   /* An out-of-range application value, then the global value; a wrong type, then nothing. */
   const struct fake_value values[] = {
      V(L"game.exe", L"FrameRateLimit", 5),
      V(NULL, L"FrameRateLimit", 30),
      WRONG(L"game.exe", L"VSync"),
      WRONG(NULL, L"Anisotropy"),
   };
   const struct fake_env env[] = {
      {"AMDGPU_WDDM_MAX_FRAME_LATENCY", "2x"},
      {"AMDGPU_WDDM_ANISOTROPY", "64"},
   };
   struct fake f = {values, 4, env, 2, 0};
   struct radv_wddm2_settings s;
   char line[256];
   resolve(&f, L"game.exe", &s);
   CHECK(s.values[RADV_WDDM2_SETTING_FRAME_RATE_LIMIT].value == 30);
   CHECK(s.values[RADV_WDDM2_SETTING_FRAME_RATE_LIMIT].source == RADV_WDDM2_SETTING_SOURCE_GLOBAL);
   CHECK(!radv_wddm2_settings_has(&s, RADV_WDDM2_SETTING_VSYNC));
   CHECK(!radv_wddm2_settings_has(&s, RADV_WDDM2_SETTING_ANISOTROPY));
   CHECK(!radv_wddm2_settings_has(&s, RADV_WDDM2_SETTING_MAX_FRAME_LATENCY));
   CHECK(s.rejection_count == 5);

   CHECK(radv_wddm2_settings_format_rejection(&s.rejections[0], line, sizeof(line)) > 0);
   CHECK(!strcmp(line, "amdgpu-wddm settings: ignored FrameRateLimit=5 from the application key "
                       "(accepted: 0, 20-300)"));
   CHECK(radv_wddm2_settings_format_rejection(&s.rejections[1], line, sizeof(line)) > 0);
   CHECK(!strcmp(line, "amdgpu-wddm settings: ignored VSync from the application key: not a REG_DWORD"));
   CHECK(radv_wddm2_settings_format_rejection(&s.rejections[2], line, sizeof(line)) > 0);
   CHECK(!strcmp(line, "amdgpu-wddm settings: ignored Anisotropy=64 from the environment "
                       "(accepted: 1, 2, 4, 8, 16)"));
   CHECK(radv_wddm2_settings_format_rejection(&s.rejections[4], line, sizeof(line)) > 0);
   CHECK(!strcmp(line, "amdgpu-wddm settings: ignored AMDGPU_WDDM_MAX_FRAME_LATENCY=\"2x\" from the "
                       "environment: not a decimal number"));
}

static void
test_parse_decimal(void)
{
   uint32_t v = 0;
   CHECK(radv_wddm2_setting_parse_decimal("0", &v) && v == 0);
   CHECK(radv_wddm2_setting_parse_decimal("4294967295", &v) && v == 4294967295u);
   CHECK(!radv_wddm2_setting_parse_decimal("4294967296", &v));
   CHECK(!radv_wddm2_setting_parse_decimal("00000000001", &v));
   CHECK(!radv_wddm2_setting_parse_decimal("", &v));
   CHECK(!radv_wddm2_setting_parse_decimal(" 1", &v));
   CHECK(!radv_wddm2_setting_parse_decimal("-1", &v));
   CHECK(!radv_wddm2_setting_parse_decimal("0x10", &v));
   CHECK(!radv_wddm2_setting_parse_decimal(NULL, &v));
}

static void
test_excluded_and_effective_line(void)
{
   const struct fake_value values[] = {V(NULL, L"FrameRateLimit", 60), V(NULL, L"Anisotropy", 16)};
   struct fake f = {values, 2, NULL, 0, 0};
   struct radv_wddm2_settings s;
   char line[512];

   resolve(&f, L"DWM.exe", &s);
   CHECK(s.excluded);
   CHECK(!radv_wddm2_settings_has(&s, RADV_WDDM2_SETTING_FRAME_RATE_LIMIT));
   CHECK(radv_wddm2_settings_format_effective(&s, 0, line, sizeof(line)) > 0);
   CHECK(!strcmp(line, "amdgpu-wddm settings: api=vulkan app=DWM.exe excluded"));

   resolve(&f, L"gäme.exe", &s);
   CHECK(!s.excluded);
   CHECK(radv_wddm2_settings_format_effective(&s, 1u << RADV_WDDM2_SETTING_ANISOTROPY, line,
                                              sizeof(line)) > 0);
   CHECK(!strcmp(line, "amdgpu-wddm settings: api=vulkan app=g?me.exe FrameRateLimit=60/global "
                       "VSync=unset Anisotropy=16/global/not-applied MaxFrameLatency=unset"));
}

/* ---- the present policy ---- */

static void
test_present_mode(void)
{
   struct wsi_win32_present_settings s = {0};
   CHECK(wsi_win32_settings_present_mode(NULL, VK_PRESENT_MODE_MAILBOX_KHR) == VK_PRESENT_MODE_MAILBOX_KHR);
   CHECK(wsi_win32_settings_present_mode(&s, VK_PRESENT_MODE_IMMEDIATE_KHR) == VK_PRESENT_MODE_IMMEDIATE_KHR);
   s.vsync_set = true;
   s.vsync = true;
   CHECK(wsi_win32_settings_present_mode(&s, VK_PRESENT_MODE_IMMEDIATE_KHR) == VK_PRESENT_MODE_FIFO_KHR);
   CHECK(wsi_win32_settings_present_mode(&s, VK_PRESENT_MODE_MAILBOX_KHR) == VK_PRESENT_MODE_FIFO_KHR);
   s.vsync = false;
   CHECK(wsi_win32_settings_present_mode(&s, VK_PRESENT_MODE_FIFO_KHR) == VK_PRESENT_MODE_IMMEDIATE_KHR);
   CHECK(wsi_win32_settings_present_mode(&s, VK_PRESENT_MODE_MAILBOX_KHR) == VK_PRESENT_MODE_IMMEDIATE_KHR);
}

static void
test_limiter(void)
{
   uint64_t last = 0;
   CHECK(wsi_win32_limiter_target(1000, 0, &last) == 0 && last == 0); /* no limit */
   CHECK(wsi_win32_limiter_target(1000, 100, &last) == 0 && last == 1000); /* first frame */
   CHECK(wsi_win32_limiter_target(1030, 100, &last) == 1100 && last == 1100); /* early: wait */
   CHECK(wsi_win32_limiter_target(1150, 100, &last) == 1200 && last == 1200);
   CHECK(wsi_win32_limiter_target(1250, 100, &last) == 1300 && last == 1300);
   /* late by less than one interval: the clock keeps its grid */
   last = 1200;
   CHECK(wsi_win32_limiter_target(1350, 100, &last) == 0 && last == 1300);
   /* late by more than one interval: the clock restarts at now, no burst */
   last = 1300;
   CHECK(wsi_win32_limiter_target(1700, 100, &last) == 0 && last == 1700);
   /* a clock that went back restarts too */
   CHECK(wsi_win32_limiter_target(10, 100, &last) == 0 && last == 10);
}

static void
test_latency_ring(void)
{
   struct wsi_win32_latency l = {0};
   uint32_t image = 99;
   uint64_t value = 0;

   CHECK(!wsi_win32_latency_target(&l, 1, &image, &value)); /* nothing yet */
   wsi_win32_latency_push(&l, 0, 10);
   CHECK(!wsi_win32_latency_target(&l, 1, &image, &value)); /* one present: none back */
   wsi_win32_latency_push(&l, 1, 20);
   CHECK(wsi_win32_latency_target(&l, 1, &image, &value) && image == 0 && value == 10);
   CHECK(!wsi_win32_latency_target(&l, 2, &image, &value));
   wsi_win32_latency_push(&l, 2, 30);
   wsi_win32_latency_push(&l, 0, 11);
   CHECK(wsi_win32_latency_target(&l, 3, &image, &value) && image == 0 && value == 10);
   CHECK(wsi_win32_latency_target(&l, 2, &image, &value) && image == 1 && value == 20);
   CHECK(wsi_win32_latency_target(&l, 1, &image, &value) && image == 2 && value == 30);
   wsi_win32_latency_push(&l, 1, 21); /* the ring wraps */
   CHECK(wsi_win32_latency_target(&l, 3, &image, &value) && image == 1 && value == 20);
   CHECK(!wsi_win32_latency_target(&l, 0, &image, &value)); /* no limit */
   CHECK(!wsi_win32_latency_target(&l, 4, &image, &value)); /* out of range */
}

int
main(int argc, char **argv)
{
   invert = argc > 1 && !strcmp(argv[1], "--negative-control");
   test_nothing_set();
   test_precedence();
   test_ranges();
   test_rejections_fall_through();
   test_parse_decimal();
   test_excluded_and_effective_line();
   test_present_mode();
   test_limiter();
   test_latency_ring();
   if (invert) {
      /* Every check must have failed. */
      printf("negative control: %d of %d checks failed\n", failures, checks);
      return failures == checks ? 1 : 0;
   }
   printf("%s: %d checks, %d failures\n", failures ? "FAIL" : "PASS", checks, failures);
   return failures ? 1 : 0;
}
