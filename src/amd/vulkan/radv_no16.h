/* SPDX-License-Identifier: MIT
 *
 * 16-sample rendering without attachments on GFX10 (BD-232).
 *
 * The case: a graphics pipeline with 16 static rasterization samples and no
 * color, depth or stencil attachment (D3D12 SampleDesc.Count 16 or
 * ForcedSampleCount 16 with no outputs; VulkanOn12.md requires the counts 1,
 * 4, 8 and 16 there). The GFX10 scan converter has 16 coverage samples, but a
 * pixel shader runs at most 8 iterations per pixel. So:
 *
 *  - NATIVE16: when the shader does not run once per sample, the draw uses
 *    16 coverage samples with 1, 2, 4 or 8 PS iterations (EQAA 16s 8z 8f, as
 *    RadeonSI si_state.c programs it). The shader runs once.
 *  - SPLIT16: when the shader runs once per sample, the draw is recorded two
 *    times, each as an ordinary 8-sample draw with 8 PS iterations. Pass 0 has
 *    the standard 16-sample locations 0-7, pass 1 the locations 8-15. The
 *    shader adds 8 times the pass index to its sample index, so each of the
 *    16 logical samples runs one real hardware invocation, with real helper
 *    lanes, derivatives, discard and subgroups. Stages before the rasterizer
 *    run two times (Vulkan allows it: a vertex shader runs at least once per
 *    vertex, shaders.adoc "Vertex Shader Execution"). The memory writes of an
 *    NGG vertex shader run in pass 0 only (NGG_STATE_NO16_PASS), so they
 *    happen once. Refused at creation: memory writes in the tessellation or
 *    geometry stages, a vertex shader memory write whose returned value is
 *    used, transform feedback, task/mesh shaders, and a fragment shader
 *    interlock (rasterizer-ordered views).
 *
 * Every other pipeline and draw is unchanged: the pipeline key bit, the
 * shader marker and the command state stay zero.
 *
 * Active by default in both flavours. The lab flavour reads RADV_NO16_AUTO
 * once per process (nir/radv_nir_no16.c): unset or "on" is the default, "off"
 * gives the behaviour without this path (no 16 samples in the limit), and the
 * neg-* values are lab controls that must make the measurement fail.
 */
#ifndef RADV_NO16_H
#define RADV_NO16_H

#include <stdbool.h>
#include <string.h>

enum radv_no16_auto_mode {
   RADV_NO16_AUTO_OFF = 0,
   RADV_NO16_AUTO_ON,
   RADV_NO16_AUTO_NEG_UPPER,    /* SPLIT16 records pass 0 only */
   RADV_NO16_AUTO_NEG_POSITION, /* SPLIT16 pass 1 uses the locations of pass 0 */
   RADV_NO16_AUTO_NEG_BASE,     /* SPLIT16 pass 1 does not add 8 to the sample index */
   RADV_NO16_AUTO_NEG_REFUSE,   /* every 16-sample pipeline is refused at creation */
};

static inline const char *
radv_no16_auto_mode_name(enum radv_no16_auto_mode mode)
{
   static const char *names[] = {"off", "on", "neg-upper", "neg-position", "neg-base", "neg-refuse"};
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
      return RADV_NO16_AUTO_ON;
   if (!strcmp(value, "off")) return RADV_NO16_AUTO_OFF;
   if (!strcmp(value, "neg-upper")) return RADV_NO16_AUTO_NEG_UPPER;
   if (!strcmp(value, "neg-position")) return RADV_NO16_AUTO_NEG_POSITION;
   if (!strcmp(value, "neg-base")) return RADV_NO16_AUTO_NEG_BASE;
   if (!strcmp(value, "neg-refuse")) return RADV_NO16_AUTO_NEG_REFUSE;
   return RADV_NO16_AUTO_ON;
}

/* The process value of the kill switch, read at the first call. */
enum radv_no16_auto_mode radv_no16_auto_mode(void);
#else
static inline enum radv_no16_auto_mode
radv_no16_auto_mode(void)
{
   return RADV_NO16_AUTO_ON;
}
#endif

/* The two GFX levels whose scan converter and EQAA state this file programs. */
static inline bool
radv_no16_gfx_level_ok(bool gfx10, bool gfx10_3)
{
   return gfx10 || gfx10_3;
}

/* Shader marker (radv_shader_info.ps.no16) and command state of a draw. */
enum radv_no16_draw {
   RADV_NO16_DRAW_DEFAULT = 0, /* unchanged driver behaviour */
   RADV_NO16_DRAW_NATIVE16,    /* one draw, 16 coverage samples, at most 8 PS iterations */
   RADV_NO16_DRAW_SPLIT16,     /* two 8-sample draws, logical samples 0-7 and 8-15 */
   RADV_NO16_DRAW_REFUSE,      /* no draw; the command buffer records an error */
};

static inline const char *
radv_no16_draw_name(enum radv_no16_draw draw)
{
   static const char *names[] = {"default", "native16", "split16", "refuse"};
   return (unsigned)draw < sizeof(names) / sizeof(names[0]) ? names[draw] : "invalid";
}

/* Pipeline admission, from the multisample, rasterization and rendering
 * state. Pure, so host tests cover the table. */
struct radv_no16_pipeline_inputs {
   enum radv_no16_auto_mode mode;
   bool gfx_level_ok;     /* radv_no16_gfx_level_ok */
   unsigned samples;      /* static rasterization samples, 0 if there is no multisample state */
   bool dynamic_samples;
   bool sample_locations; /* custom sample locations, static or dynamic */
   bool conservative;     /* conservative rasterization, static or dynamic */
   bool attachments;      /* a color, depth or stencil attachment format, or no rendering state */
};

/* Returns true when the pipeline is admitted. *reason is "samples" for every
 * pipeline that does not ask for 16 samples, and "mode" when the path is off. */
static inline bool
radv_no16_pipeline_admit(const struct radv_no16_pipeline_inputs *in, const char **reason)
{
   *reason = "none";
   if (in->samples != 16) *reason = "samples";
   else if (in->mode == RADV_NO16_AUTO_OFF) *reason = "mode";
   else if (in->mode == RADV_NO16_AUTO_NEG_REFUSE) *reason = "forced-refusal";
   else if (!in->gfx_level_ok) *reason = "gfx-level";
   else if (in->dynamic_samples) *reason = "dynamic-samples";
   else if (in->sample_locations) *reason = "sample-locations";
   else if (in->conservative) *reason = "conservative";
   else if (in->attachments) *reason = "attachments";
   return !strcmp(*reason, "none");
}

/* Fragment shader decision of an admitted pipeline. Pure. */
struct radv_no16_shader_inputs {
   bool per_sample;           /* the shader runs once per sample (uses_sample_shading, or 16 PS iterations) */
   unsigned ps_iter_samples;  /* static PS iterations of the pipeline sample shading state, 1 if none */
   bool pre_raster_memory;    /* TCS, TES or GS writes memory, or the pre-raster stages are not visible */
   bool vs_memory_result;     /* the vertex shader uses the value that a memory write returns */
   bool xfb;                  /* transform feedback */
   bool task_mesh;            /* task or mesh stages */
   bool interlock;            /* fragment shader interlock (rasterizer-ordered views) */
};

static inline enum radv_no16_draw
radv_no16_shader_decide(const struct radv_no16_shader_inputs *in, const char **reason)
{
   *reason = "none";
   if (!in->per_sample && in->ps_iter_samples <= 8)
      return RADV_NO16_DRAW_NATIVE16;
   if (in->pre_raster_memory) *reason = "pre-raster-memory";
   else if (in->vs_memory_result) *reason = "vertex-memory-result";
   else if (in->xfb) *reason = "xfb";
   else if (in->task_mesh) *reason = "task-mesh";
   else if (in->interlock) *reason = "interlock";
   return strcmp(*reason, "none") ? RADV_NO16_DRAW_REFUSE : RADV_NO16_DRAW_SPLIT16;
}

/* Command-time decision for one draw. Pure, so host tests cover the table. */
struct radv_no16_draw_inputs {
   enum radv_no16_auto_mode mode;
   bool gfx_level_ok;
   enum radv_no16_draw marker; /* radv_shader_info.ps.no16 of the bound fragment shader */
   unsigned state_samples;     /* rasterization samples of the bound state */
   unsigned rast_samples;      /* the same after the line rules (radv_get_rasterization_samples) */
   bool sample_locations_enable;
   bool conservative;
   unsigned color_att_count;
   bool ds_attachment;
   bool vrs;                   /* a coarse shading rate or a VRS attachment can apply */
   bool generated_commands;    /* vkCmdExecuteGeneratedCommandsEXT */
};

static inline enum radv_no16_draw
radv_no16_draw_decide(const struct radv_no16_draw_inputs *in, const char **reason)
{
   *reason = "none";
   if (in->mode == RADV_NO16_AUTO_OFF)
      return RADV_NO16_DRAW_DEFAULT;
   if (in->marker != RADV_NO16_DRAW_NATIVE16 && in->marker != RADV_NO16_DRAW_SPLIT16) {
      /* Only a marked shader runs with 16 samples. */
      if (in->state_samples == 16) {
         *reason = "unmarked-16-sample-draw";
         return RADV_NO16_DRAW_REFUSE;
      }
      return RADV_NO16_DRAW_DEFAULT;
   }
   if (!in->gfx_level_ok) *reason = "gfx-level";
   else if (in->state_samples != 16) *reason = "samples";
   else if (in->rast_samples != 16) *reason = "line-samples";
   else if (in->sample_locations_enable) *reason = "sample-locations";
   else if (in->conservative) *reason = "conservative";
   else if (in->color_att_count) *reason = "color-attachment";
   else if (in->ds_attachment) *reason = "depth-stencil-attachment";
   else if (in->vrs) *reason = "vrs";
   else if (in->generated_commands && in->marker == RADV_NO16_DRAW_SPLIT16) *reason = "generated-commands";
   return strcmp(*reason, "none") ? RADV_NO16_DRAW_REFUSE : in->marker;
}

/* Number of recorded passes of a draw. */
static inline unsigned
radv_no16_draw_passes(enum radv_no16_draw draw, enum radv_no16_auto_mode mode)
{
   if (draw != RADV_NO16_DRAW_SPLIT16)
      return 1;
   return mode == RADV_NO16_AUTO_NEG_UPPER ? 1 : 2;
}

/* Hardware rasterization samples of a draw whose bound state has state_samples. */
static inline unsigned
radv_no16_physical_samples(enum radv_no16_draw draw, unsigned state_samples)
{
   return draw == RADV_NO16_DRAW_SPLIT16 ? 8 : state_samples;
}

/* Standard 16-sample locations (Vulkan primsrast.adoc standard-sample-locations,
 * identical to the D3D standard pattern) in sample-index order, in 1/16 pixel
 * units from the pixel corner. */
static const unsigned char radv_no16_standard16[16][2] = {
   {9, 9}, {7, 5}, {5, 10}, {12, 7}, {3, 6}, {10, 13}, {13, 11}, {11, 3},
   {6, 14}, {8, 1}, {4, 2}, {2, 12}, {0, 8}, {15, 4}, {14, 15}, {1, 0},
};

/* Signed hardware offset (-8..7) from the pixel center of standard16 sample s, axis a. */
static inline int
radv_no16_standard16_offset(unsigned s, unsigned a)
{
   return (int)radv_no16_standard16[s & 15][a & 1] - 8;
}

/* The logical sample of hardware sample h in pass p of a SPLIT16 draw. The
 * position negative gives pass 1 the locations of pass 0. */
static inline unsigned
radv_no16_split_location_sample(unsigned pass, unsigned h, enum radv_no16_auto_mode mode)
{
   return (mode == RADV_NO16_AUTO_NEG_POSITION ? 0 : pass * 8) + (h & 7);
}

/* The value the shader adds to its hardware sample index in pass p. */
static inline unsigned
radv_no16_split_sample_base(unsigned pass, enum radv_no16_auto_mode mode)
{
   return mode == RADV_NO16_AUTO_NEG_BASE ? 0 : pass * 8;
}

#endif
