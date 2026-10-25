/* SPDX-License-Identifier: MIT
 *
 * Automatic no-output16 rendering on GFX10 (BD-232, step 1: the frozen
 * Stage B VS/FS pair only).
 *
 * The case: a complete graphics pipeline that asks for 16 rasterization
 * samples, with no color, depth or stencil attachment. GFX10 runs at most 8
 * PS iterations, so the pipeline is created as a physical 8-sample pipeline.
 * Its fragment shader runs the unchanged body twice per physical sample ID
 * (logical IDs p and p+8, nir/radv_nir_no16.c), and the command state selects
 * 16 coverage samples with 8 PS iterations for that marked shader only.
 *
 * Every other pipeline and draw is unchanged: the pipeline key bit, the
 * shader marker and the command state stay zero, so their hashes, binaries
 * and packets are those of the driver without this file. A 16-sample request
 * that the transform does not admit is refused, at pipeline creation or at
 * the draw. It never runs as an ordinary body with 16 samples.
 *
 * Active by default in both flavours. The lab flavour reads RADV_NO16_AUTO
 * once per process as a diagnostic kill switch (radv_nir_no16.c): unset or
 * "stageb" is the default, "off" gives the behaviour without this path, and
 * the stageb-neg-* values are lab controls that must make the frozen analyzer
 * or the pipeline fail.
 */
#ifndef RADV_NO16_H
#define RADV_NO16_H

#include <stdbool.h>
#include <string.h>

enum radv_no16_auto_mode {
   RADV_NO16_AUTO_OFF = 0,
   RADV_NO16_AUTO_STAGEB,
   RADV_NO16_AUTO_NEG_UPPER,    /* the transform drops the second logical phase */
   RADV_NO16_AUTO_NEG_POSITION, /* the second phase uses the first phase's sample location */
   RADV_NO16_AUTO_NEG_STATE,    /* transformed shader with ordinary 8-sample coverage */
   RADV_NO16_AUTO_NEG_REFUSE,   /* the transform must refuse; pipeline creation fails */
};

static inline const char *
radv_no16_auto_mode_name(enum radv_no16_auto_mode mode)
{
   static const char *names[] = {"off", "stageb", "stageb-neg-upper", "stageb-neg-position",
                                 "stageb-neg-state", "stageb-neg-refuse"};
   return (unsigned)mode < sizeof(names) / sizeof(names[0]) ? names[mode] : "invalid";
}

#if defined(RADV_S16F8_PROBE) && RADV_S16F8_PROBE
/* Lab kill switch. An unset, empty or unknown value is the default. A set
 * RADV_S16F8_PROBE_MODE (the manual S16F8 probe programs the same registers)
 * turns the path off. */
static inline enum radv_no16_auto_mode
radv_no16_auto_parse(const char *value, const char *probe_mode)
{
   if (probe_mode && *probe_mode)
      return RADV_NO16_AUTO_OFF;
   if (!value || !*value)
      return RADV_NO16_AUTO_STAGEB;
   if (!strcmp(value, "off")) return RADV_NO16_AUTO_OFF;
   if (!strcmp(value, "stageb-neg-upper")) return RADV_NO16_AUTO_NEG_UPPER;
   if (!strcmp(value, "stageb-neg-position")) return RADV_NO16_AUTO_NEG_POSITION;
   if (!strcmp(value, "stageb-neg-state")) return RADV_NO16_AUTO_NEG_STATE;
   if (!strcmp(value, "stageb-neg-refuse")) return RADV_NO16_AUTO_NEG_REFUSE;
   return RADV_NO16_AUTO_STAGEB;
}

/* The process value of the kill switch, read at the first call. */
enum radv_no16_auto_mode radv_no16_auto_mode(void);
#else
static inline enum radv_no16_auto_mode
radv_no16_auto_mode(void)
{
   return RADV_NO16_AUTO_STAGEB;
}
#endif

/* Pipeline admission, from the complete pipeline state. Pure, so host tests
 * cover the table. */
struct radv_no16_pipeline_inputs {
   enum radv_no16_auto_mode mode;
   bool gfx10;            /* GFX10 or GFX10.3 */
   unsigned samples;      /* static rasterization samples, 0 if there is no multisample state */
   bool library;          /* a library, or a pipeline linked from libraries */
   bool dynamic_samples;
   bool sample_locations; /* custom sample locations, static or dynamic */
   bool vs_fs_only;       /* the stages are exactly vertex and fragment */
   bool rasterization;    /* static rasterizer state, no conservative mode, no rasterizer discard */
   bool attachments;      /* a color, depth or stencil attachment format */
};

/* Returns true when the pipeline is admitted. *reason is "samples" for every
 * pipeline that does not ask for 16 samples, and "mode" when the path is off. */
static inline bool
radv_no16_pipeline_admit(const struct radv_no16_pipeline_inputs *in, const char **reason)
{
   *reason = "none";
   if (in->samples != 16) *reason = "samples";
   else if (in->mode == RADV_NO16_AUTO_OFF) *reason = "mode";
   else if (!in->gfx10) *reason = "gfx-level";
   else if (in->library) *reason = "library";
   else if (in->dynamic_samples) *reason = "dynamic-samples";
   else if (in->sample_locations) *reason = "sample-locations";
   else if (!in->vs_fs_only) *reason = "stages";
   else if (!in->rasterization) *reason = "rasterization";
   else if (in->attachments) *reason = "attachments";
   return !strcmp(*reason, "none");
}

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
   unsigned api_samples;       /* rasterization samples of the draw (radv_get_rasterization_samples) */
   unsigned state_samples;     /* rasterization samples of the bound state, before line rules */
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
      /* An untransformed body never runs with 16 samples. */
      if (in->api_samples == 16 || in->state_samples == 16) {
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
