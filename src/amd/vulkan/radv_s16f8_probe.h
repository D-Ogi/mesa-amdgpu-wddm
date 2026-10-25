/* SPDX-License-Identifier: MIT
 * Isolated S16F8 diagnostic. Not a capability implementation.
 * Requires an explicit build define AND process opt-in. Never use in a release build.
 */
#ifndef RADV_S16F8_PROBE_H
#define RADV_S16F8_PROBE_H
#include <stdlib.h>
#include <string.h>
#if defined(RADV_S16F8_PROBE) && RADV_S16F8_PROBE && (!defined(AMDGPU_WDDM_LAB_FLAVOUR) || !AMDGPU_WDDM_LAB_FLAVOUR)
#error S16F8 diagnostics require the lab build flavour
#endif
static inline unsigned
radv_s16f8_probe_mode(void)
{
#if defined(RADV_S16F8_PROBE) && RADV_S16F8_PROBE
   const char *mode = getenv("RADV_S16F8_PROBE_MODE");
   if (mode && !strcmp(mode, "control8")) return 1;
   if (mode && !strcmp(mode, "pal8")) return 2;
   if (mode && !strcmp(mode, "exposed16")) return 3;
#endif
   return 0;
}

#if defined(RADV_S16F8_PROBE) && RADV_S16F8_PROBE
#include <stdbool.h>
#include <stdio.h>

/* Lab-only automatic no-output16 path for the frozen Stage B VS/FS pair
 * (RADV_NO16_AUTO). An application pipeline with 16 rasterization samples and
 * no attachments is created as a physical 8-sample pipeline. Its fragment
 * shader runs the unchanged body twice per physical sample ID (logical IDs p
 * and p+8, radv_nir_no16.c). The command state then selects 16 coverage
 * samples with 8 PS iterations for that marked shader only.
 *
 * Off by default. Any other value of the variable, or a set
 * RADV_S16F8_PROBE_MODE, keeps the mode off. The mode is read once per process
 * (radv_no16_auto_mode, radv_nir_no16.c), so no pipeline can mix shaders of two
 * modes. The negative modes are lab controls that must make the frozen analyzer
 * fail or the pipeline fail.
 */
enum radv_no16_auto_mode {
   RADV_NO16_AUTO_OFF = 0,
   RADV_NO16_AUTO_STAGEB,
   RADV_NO16_AUTO_NEG_UPPER,    /* the transform drops the second logical phase */
   RADV_NO16_AUTO_NEG_POSITION, /* the second phase uses the first phase's sample location */
   RADV_NO16_AUTO_NEG_STATE,    /* transformed shader with ordinary 8-sample coverage */
   RADV_NO16_AUTO_NEG_REFUSE,   /* the transform must refuse; pipeline creation fails */
};

static inline enum radv_no16_auto_mode
radv_no16_auto_parse(const char *value, const char *probe_mode)
{
   if (!value || !*value)
      return RADV_NO16_AUTO_OFF;
   if (probe_mode && *probe_mode)
      return RADV_NO16_AUTO_OFF; /* never combined with the manual S16F8 probe */
   if (!strcmp(value, "stageb")) return RADV_NO16_AUTO_STAGEB;
   if (!strcmp(value, "stageb-neg-upper")) return RADV_NO16_AUTO_NEG_UPPER;
   if (!strcmp(value, "stageb-neg-position")) return RADV_NO16_AUTO_NEG_POSITION;
   if (!strcmp(value, "stageb-neg-state")) return RADV_NO16_AUTO_NEG_STATE;
   if (!strcmp(value, "stageb-neg-refuse")) return RADV_NO16_AUTO_NEG_REFUSE;
   return RADV_NO16_AUTO_OFF;
}

static inline const char *
radv_no16_auto_mode_name(enum radv_no16_auto_mode mode)
{
   static const char *names[] = {"off", "stageb", "stageb-neg-upper", "stageb-neg-position",
                                 "stageb-neg-state", "stageb-neg-refuse"};
   return (unsigned)mode < sizeof(names) / sizeof(names[0]) ? names[mode] : "invalid";
}

/* The process value of RADV_NO16_AUTO, read at the first call. */
enum radv_no16_auto_mode radv_no16_auto_mode(void);

/* Command-time decision for one draw. Pure, so host tests cover the table. */
enum radv_no16_draw {
   RADV_NO16_DRAW_DEFAULT = 0, /* unchanged driver behaviour */
   RADV_NO16_DRAW_COVERAGE16,  /* 16 coverage samples, 8 PS iterations, standard16 locations */
   RADV_NO16_DRAW_PHYSICAL8,   /* negative control: transformed shader, ordinary 8-sample state */
   RADV_NO16_DRAW_REFUSE,      /* no draw; the command buffer records an error */
};

struct radv_no16_draw_inputs {
   enum radv_no16_auto_mode mode;
   bool gfx10;                 /* GFX10 or GFX10.3, as the S16F8 probe */
   bool fs_logical16;          /* the bound fragment shader carries the transform marker */
   unsigned api_samples;       /* rasterization samples of the bound state */
   unsigned ps_iter_samples;
   bool sample_locations_enable;
   bool conservative;
   unsigned color_att_count;
   bool ds_attachment;
   bool triangles;             /* triangle primitives, polygon mode fill */
   bool vrs;                   /* a coarse shading rate or a VRS attachment can apply */
};

static inline enum radv_no16_draw
radv_no16_draw_decide(const struct radv_no16_draw_inputs *in, const char **reason)
{
   *reason = "none";
   if (in->mode == RADV_NO16_AUTO_OFF)
      return RADV_NO16_DRAW_DEFAULT;
   if (!in->fs_logical16) {
      /* An untransformed body must never run with 16 samples in this mode. */
      if (in->api_samples == 16) {
         *reason = "unmarked-16-sample-draw";
         return RADV_NO16_DRAW_REFUSE;
      }
      return RADV_NO16_DRAW_DEFAULT;
   }
   if (!in->gfx10) *reason = "gfx-level";
   else if (in->api_samples != 8) *reason = "physical-samples";
   else if (in->ps_iter_samples != 8) *reason = "ps-iter-samples";
   else if (in->sample_locations_enable) *reason = "sample-locations";
   else if (in->conservative) *reason = "conservative";
   else if (in->color_att_count) *reason = "color-attachment";
   else if (in->ds_attachment) *reason = "depth-stencil-attachment";
   else if (!in->triangles) *reason = "topology";
   else if (in->vrs) *reason = "vrs";
   if (strcmp(*reason, "none"))
      return RADV_NO16_DRAW_REFUSE;
   return in->mode == RADV_NO16_AUTO_NEG_STATE ? RADV_NO16_DRAW_PHYSICAL8 : RADV_NO16_DRAW_COVERAGE16;
}

static inline const char *
radv_no16_draw_name(enum radv_no16_draw draw)
{
   static const char *names[] = {"default", "coverage16", "physical8", "refuse"};
   return (unsigned)draw < sizeof(names) / sizeof(names[0]) ? names[draw] : "invalid";
}
#endif
#endif
