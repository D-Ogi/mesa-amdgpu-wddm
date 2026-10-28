/* SPDX-License-Identifier: MIT */
/*
 * Copyright 2026 amdgpu-wddm contributors
 *
 * Per-application graphics settings in the system Vulkan ICD: the reader of the values that the
 * control application writes, by the contract of the D3D11 and D3D12 shells (bc250-win
 * driver/umd/app-settings/app-settings-core.h, docs/design/per-app-graphics-settings.md).
 *
 *   HKLM\SOFTWARE\amdgpu-wddm\Graphics                          the global values
 *   HKLM\SOFTWARE\amdgpu-wddm\Graphics\Applications\<image>     the values of one application: <image>
 *                                                               is the file name of the process image
 *                                                               (game.exe); key names are not
 *                                                               case-sensitive
 *
 * Precedence, highest first: the environment variable AMDGPU_WDDM_<NAME>, the application key, the
 * global key. No value from any source = the application decides. Every value is a REG_DWORD. A value
 * out of its range, a value of another type, or an environment string that is not a decimal number is
 * ignored with one log line, and the next source is read. A 32-bit process reads the 64-bit registry
 * view.
 *
 * This ICD applies four of the settings: FrameRateLimit, VSync and MaxFrameLatency in the present
 * paths (wsi_win32_present_policy.h), Anisotropy in the sampler descriptors. PerformanceOverlay and
 * RenderOnCpu are settings of the D3D shells only and are not read here.
 *
 * This part has no Windows header, so that the host test
 * (tests/radv_wddm2_app_settings_test.c) gives it doubles of the registry and the environment.
 * radv_wddm2_app_settings_read() in radv_wddm2_wsi.c gives it the system.
 */
#ifndef RADV_WDDM2_APP_SETTINGS_H
#define RADV_WDDM2_APP_SETTINGS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

enum radv_wddm2_setting {
   RADV_WDDM2_SETTING_FRAME_RATE_LIMIT,
   RADV_WDDM2_SETTING_VSYNC,
   RADV_WDDM2_SETTING_ANISOTROPY,
   RADV_WDDM2_SETTING_MAX_FRAME_LATENCY,
   RADV_WDDM2_SETTING_COUNT,
};

/* Where a value came from. NONE: no source has a valid value, the application decides. */
enum radv_wddm2_setting_source {
   RADV_WDDM2_SETTING_SOURCE_NONE,
   RADV_WDDM2_SETTING_SOURCE_GLOBAL,
   RADV_WDDM2_SETTING_SOURCE_APPLICATION,
   RADV_WDDM2_SETTING_SOURCE_ENVIRONMENT,
};

/* What a registry read found. WRONG_TYPE: the value exists but is not a 4-byte REG_DWORD. */
enum radv_wddm2_setting_read {
   RADV_WDDM2_SETTING_READ_ABSENT,
   RADV_WDDM2_SETTING_READ_VALUE,
   RADV_WDDM2_SETTING_READ_WRONG_TYPE,
};

enum radv_wddm2_setting_problem {
   RADV_WDDM2_SETTING_OUT_OF_RANGE,
   RADV_WDDM2_SETTING_WRONG_TYPE,
   RADV_WDDM2_SETTING_NOT_NUMBER,
};

struct radv_wddm2_setting_descriptor {
   const wchar_t *value; /* REG_DWORD value name in both keys */
   const char *name;     /* the same name, for log lines */
   const char *env;      /* the environment variable that wins over both keys */
   const char *range;    /* the accepted values, for the log line of an ignored value */
};

#define RADV_WDDM2_SETTINGS_MAX_APP 260

/* The inputs of radv_wddm2_settings_resolve(). Neither may fail in a way other than its result. */
struct radv_wddm2_setting_sources {
   void *context;
   /* application: NULL for the global key, else the image file name. */
   enum radv_wddm2_setting_read (*registry)(void *context, const wchar_t *application,
                                            const wchar_t *value, uint32_t *out);
   /* false: the variable is not set. text gets at most size-1 characters and a terminating zero. */
   bool (*environment)(void *context, const char *name, char *text, size_t size);
};

struct radv_wddm2_setting_value {
   uint32_t value;
   enum radv_wddm2_setting_source source;
};

struct radv_wddm2_setting_rejection {
   enum radv_wddm2_setting setting;
   enum radv_wddm2_setting_source source;
   enum radv_wddm2_setting_problem problem;
   uint32_t value; /* OUT_OF_RANGE: the value */
   char text[24];  /* NOT_NUMBER: the start of the environment string */
};

struct radv_wddm2_settings {
   struct radv_wddm2_setting_value values[RADV_WDDM2_SETTING_COUNT];
   struct radv_wddm2_setting_rejection rejections[RADV_WDDM2_SETTING_COUNT * 3];
   unsigned rejection_count;
   /* the image file name the application key was read for (empty = none) */
   wchar_t application[RADV_WDDM2_SETTINGS_MAX_APP];
   /* true: the process is one that never takes these settings (dwm.exe); every value is unset */
   bool excluded;
};

static inline const struct radv_wddm2_setting_descriptor *
radv_wddm2_setting_descriptor(enum radv_wddm2_setting s)
{
   static const struct radv_wddm2_setting_descriptor table[RADV_WDDM2_SETTING_COUNT] = {
      {L"FrameRateLimit", "FrameRateLimit", "AMDGPU_WDDM_FRAME_RATE_LIMIT", "0, 20-300"},
      {L"VSync", "VSync", "AMDGPU_WDDM_VSYNC", "0, 1"},
      {L"Anisotropy", "Anisotropy", "AMDGPU_WDDM_ANISOTROPY", "1, 2, 4, 8, 16"},
      {L"MaxFrameLatency", "MaxFrameLatency", "AMDGPU_WDDM_MAX_FRAME_LATENCY", "1-3"},
   };
   return &table[(unsigned)s < RADV_WDDM2_SETTING_COUNT ? (unsigned)s : 0u];
}

static inline const wchar_t *
radv_wddm2_settings_global_key(void)
{
   return L"SOFTWARE\\amdgpu-wddm\\Graphics";
}

static inline const wchar_t *
radv_wddm2_settings_applications_key(void)
{
   return L"SOFTWARE\\amdgpu-wddm\\Graphics\\Applications";
}

static inline bool
radv_wddm2_setting_valid(enum radv_wddm2_setting s, uint32_t v)
{
   switch (s) {
   case RADV_WDDM2_SETTING_FRAME_RATE_LIMIT:
      return v == 0 || (v >= 20 && v <= 300);
   case RADV_WDDM2_SETTING_VSYNC:
      return v <= 1;
   case RADV_WDDM2_SETTING_ANISOTROPY:
      return v == 1 || v == 2 || v == 4 || v == 8 || v == 16;
   case RADV_WDDM2_SETTING_MAX_FRAME_LATENCY:
      return v >= 1 && v <= 3;
   default:
      return false;
   }
}

static inline const char *
radv_wddm2_setting_source_name(enum radv_wddm2_setting_source s)
{
   switch (s) {
   case RADV_WDDM2_SETTING_SOURCE_GLOBAL:
      return "global";
   case RADV_WDDM2_SETTING_SOURCE_APPLICATION:
      return "application";
   case RADV_WDDM2_SETTING_SOURCE_ENVIRONMENT:
      return "environment";
   default:
      return "unset";
   }
}

/* A decimal number of 1 to 10 digits that fits 32 bits, nothing else (no sign, no space, no "0x"). */
static inline bool
radv_wddm2_setting_parse_decimal(const char *text, uint32_t *out)
{
   uint64_t v = 0;
   unsigned digits = 0;

   if (!text || !*text)
      return false;
   for (const char *p = text; *p; ++p) {
      if (*p < '0' || *p > '9' || ++digits > 10)
         return false;
      v = v * 10u + (unsigned)(*p - '0');
   }
   if (v > 0xFFFFFFFFull)
      return false;
   *out = (uint32_t)v;
   return true;
}

/* Bounded copies that always end with a zero. */
static inline void
radv_wddm2_settings_copy(char *out, size_t size, const char *in)
{
   size_t i = 0;

   if (!size)
      return;
   for (; in && in[i] && i + 1 < size; ++i)
      out[i] = in[i];
   out[i] = 0;
}

static inline void
radv_wddm2_settings_wcopy(wchar_t *out, size_t size, const wchar_t *in)
{
   size_t i = 0;

   if (!size)
      return;
   for (; in && in[i] && i + 1 < size; ++i)
      out[i] = in[i];
   out[i] = 0;
}

static inline void
radv_wddm2_settings_reject(struct radv_wddm2_settings *s, enum radv_wddm2_setting setting,
                           enum radv_wddm2_setting_source source,
                           enum radv_wddm2_setting_problem problem, uint32_t value, const char *text)
{
   struct radv_wddm2_setting_rejection *r;

   if (s->rejection_count >= sizeof(s->rejections) / sizeof(s->rejections[0]))
      return;
   r = &s->rejections[s->rejection_count++];
   r->setting = setting;
   r->source = source;
   r->problem = problem;
   r->value = value;
   r->text[0] = 0;
   if (text)
      radv_wddm2_settings_copy(r->text, sizeof(r->text), text);
}

/* One registry source of one setting: true when it gave the effective value. */
static inline bool
radv_wddm2_settings_take_registry(struct radv_wddm2_settings *s, enum radv_wddm2_setting setting,
                                  enum radv_wddm2_setting_source source,
                                  const struct radv_wddm2_setting_sources *from,
                                  const wchar_t *application)
{
   uint32_t v = 0;

   if (!from->registry)
      return false;
   switch (from->registry(from->context, application, radv_wddm2_setting_descriptor(setting)->value,
                          &v)) {
   case RADV_WDDM2_SETTING_READ_VALUE:
      if (radv_wddm2_setting_valid(setting, v)) {
         s->values[setting].value = v;
         s->values[setting].source = source;
         return true;
      }
      radv_wddm2_settings_reject(s, setting, source, RADV_WDDM2_SETTING_OUT_OF_RANGE, v, NULL);
      return false;
   case RADV_WDDM2_SETTING_READ_WRONG_TYPE:
      radv_wddm2_settings_reject(s, setting, source, RADV_WDDM2_SETTING_WRONG_TYPE, 0, NULL);
      return false;
   default:
      return false;
   }
}

static inline bool
radv_wddm2_settings_wieq(const wchar_t *a, const wchar_t *b)
{
   for (;; ++a, ++b) {
      wchar_t x = *a, y = *b;
      if (x >= L'A' && x <= L'Z')
         x = (wchar_t)(x - L'A' + L'a');
      if (y >= L'A' && y <= L'Z')
         y = (wchar_t)(y - L'A' + L'a');
      if (x != y)
         return false;
      if (!x)
         return true;
   }
}

/* The processes that never take these settings: the desktop compositor, as in the router's desktop
 * rule (bc250-win driver/umd/router/router-policy.h).
 */
static inline bool
radv_wddm2_settings_excluded(const wchar_t *application)
{
   return application && radv_wddm2_settings_wieq(application, L"dwm.exe");
}

/* The effective value of every setting into s. application: the image file name, or NULL or "" for
 * none (then only the environment and the global key count).
 */
static inline void
radv_wddm2_settings_resolve(const wchar_t *application,
                            const struct radv_wddm2_setting_sources *from,
                            struct radv_wddm2_settings *s)
{
   memset(s, 0, sizeof(*s));
   if (application && *application)
      radv_wddm2_settings_wcopy(s->application, RADV_WDDM2_SETTINGS_MAX_APP, application);
   if (radv_wddm2_settings_excluded(s->application)) {
      s->excluded = true;
      return;
   }
   for (unsigned i = 0; i < RADV_WDDM2_SETTING_COUNT; ++i) {
      const enum radv_wddm2_setting setting = (enum radv_wddm2_setting)i;
      char text[64] = {0};
      if (from->environment &&
          from->environment(from->context, radv_wddm2_setting_descriptor(setting)->env, text,
                            sizeof(text))) {
         uint32_t v = 0;
         if (!radv_wddm2_setting_parse_decimal(text, &v)) {
            radv_wddm2_settings_reject(s, setting, RADV_WDDM2_SETTING_SOURCE_ENVIRONMENT,
                                       RADV_WDDM2_SETTING_NOT_NUMBER, 0, text);
         } else if (!radv_wddm2_setting_valid(setting, v)) {
            radv_wddm2_settings_reject(s, setting, RADV_WDDM2_SETTING_SOURCE_ENVIRONMENT,
                                       RADV_WDDM2_SETTING_OUT_OF_RANGE, v, NULL);
         } else {
            s->values[i].value = v;
            s->values[i].source = RADV_WDDM2_SETTING_SOURCE_ENVIRONMENT;
            continue;
         }
      }
      if (s->application[0] &&
          radv_wddm2_settings_take_registry(s, setting, RADV_WDDM2_SETTING_SOURCE_APPLICATION, from,
                                            s->application))
         continue;
      radv_wddm2_settings_take_registry(s, setting, RADV_WDDM2_SETTING_SOURCE_GLOBAL, from, NULL);
   }
}

static inline bool
radv_wddm2_settings_has(const struct radv_wddm2_settings *s, enum radv_wddm2_setting setting)
{
   return s->values[setting].source != RADV_WDDM2_SETTING_SOURCE_NONE;
}

/* "amdgpu-wddm settings: ignored FrameRateLimit=5 from the application key (accepted: 0, 20-300)" */
static inline int
radv_wddm2_settings_format_rejection(const struct radv_wddm2_setting_rejection *r, char *out,
                                     size_t size)
{
   const struct radv_wddm2_setting_descriptor *d = radv_wddm2_setting_descriptor(r->setting);
   const char *where = r->source == RADV_WDDM2_SETTING_SOURCE_ENVIRONMENT   ? "the environment"
                       : r->source == RADV_WDDM2_SETTING_SOURCE_APPLICATION ? "the application key"
                                                                            : "the global key";
   switch (r->problem) {
   case RADV_WDDM2_SETTING_OUT_OF_RANGE:
      return snprintf(out, size, "amdgpu-wddm settings: ignored %s=%u from %s (accepted: %s)",
                      d->name, r->value, where, d->range);
   case RADV_WDDM2_SETTING_WRONG_TYPE:
      return snprintf(out, size, "amdgpu-wddm settings: ignored %s from %s: not a REG_DWORD",
                      d->name, where);
   default:
      return snprintf(out, size,
                      "amdgpu-wddm settings: ignored %s=\"%s\" from %s: not a decimal number", d->env,
                      r->text, where);
   }
}

/* "amdgpu-wddm settings: api=vulkan app=game.exe FrameRateLimit=60/application VSync=unset
 *  Anisotropy=16/global MaxFrameLatency=unset". A character of the image name outside ASCII is
 * written as '?'. not_applied: bits of the settings this process has but does not apply (the route
 * cannot), marked "/not-applied".
 */
static inline int
radv_wddm2_settings_format_effective(const struct radv_wddm2_settings *s, unsigned not_applied,
                                     char *out, size_t size)
{
   char app[RADV_WDDM2_SETTINGS_MAX_APP];
   size_t i = 0;
   int n;

   for (; s->application[i] && i + 1 < sizeof(app); ++i)
      app[i] = s->application[i] < 0x80 ? (char)s->application[i] : '?';
   app[i] = 0;
   n = snprintf(out, size, "amdgpu-wddm settings: api=vulkan app=%s", app[0] ? app : "-");
   if (s->excluded && n >= 0 && (size_t)n < size)
      return n + snprintf(out + n, size - n, " excluded");
   for (unsigned k = 0; k < RADV_WDDM2_SETTING_COUNT && n >= 0 && (size_t)n < size; ++k) {
      const struct radv_wddm2_setting_value *v = &s->values[k];
      const char *name = radv_wddm2_setting_descriptor((enum radv_wddm2_setting)k)->name;
      if (v->source == RADV_WDDM2_SETTING_SOURCE_NONE)
         n += snprintf(out + n, size - n, " %s=unset", name);
      else
         n += snprintf(out + n, size - n, " %s=%u/%s%s", name, v->value,
                       radv_wddm2_setting_source_name(v->source),
                       (not_applied & (1u << k)) ? "/not-applied" : "");
   }
   return n;
}

/* The settings of this process, read at the first call (radv_wddm2_wsi.c). */
const struct radv_wddm2_settings *radv_wddm2_app_settings(void);

#endif /* RADV_WDDM2_APP_SETTINGS_H */
