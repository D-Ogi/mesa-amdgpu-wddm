/* SPDX-License-Identifier: MIT
 *
 * amdgpu-wddm: the environment reads of the BC-250 switches (RADV_BC250_*, BC250_*), in one place.
 *
 * A read takes the name as a token: BC250_ENV_BOOL(RADV_BC250_SPLIT_MESH, false). The lab flavour
 * (AMDGPU_WDDM_LAB_FLAVOUR) expands it to the reader it replaces, debug_get_bool_option("RADV_BC250_SPLIT_MESH",
 * false), so a lab build reads the process environment as before. The release flavour reads no environment and
 * keeps no name: a read gives the value that the lab build gives with an empty user environment. For a name of
 * the DirectMesh preset (the _P forms, the BC250_PRESET_* values below) that is the preset value when a BC-250 is
 * present (bc250_env_preset(), which radv sets at instance creation from the PCI ids of the adapters, in a line
 * that has the DirectMesh route), else the code default. For every other name it is the code default.
 *
 * The lab build applies the same preset to the process environment (bc250_env_apply_directmesh_preset), so both
 * flavours take their preset values from this one table. tests/bc250_env_test.c holds every read site of the tree
 * (bc250_env_sites.h) and compares the two flavours for a BC-250 and for another adapter.
 */
#ifndef BC250_ENV_H
#define BC250_ENV_H

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The DirectMesh preset: the BC-250 base settings and every validated direct-path switch. The base configuration
 * passed Mesh CTS 3,558/3,558; additions require their own release validation. The switches themselves only act on
 * GFX1013. */
#define BC250_PRESET_RADV_BC250_NATIVE_TASK "0"
#define BC250_PRESET_BC250_EXPERIMENTAL_COMPUTE_CU_MODE "false"
#define BC250_PRESET_BC250_BALANCED_SLICES "true"
#define BC250_PRESET_BC250_SINGLE_PIECE "true"
#define BC250_PRESET_BC250_CACHE_PLAN "true"
#define BC250_PRESET_BC250_TRANSIENT_ARENA "true"
#define BC250_PRESET_BC250_PARALLEL_CULL "true"
#define BC250_PRESET_BC250_COMPACT_VERTICES "false"
/* BC250_EXPERIMENTAL_PRIVATE_GTT is not set: it moved every private allocation out of the VRAM carveout into
 * snooped GTT (a hang experiment, never shown to help). */
#define BC250_PRESET_BC250_OUTPUT_REGIONS "true"
/* BC250_EXPERIMENTAL_ZEROED_PRIVATE_GTT, RADV_BC250_SUBMIT_KNOWN_SIGNALS and RADV_BC250_KEEP_IB_KB are dropped from
 * this preset: they are read by the amdgpu winsys, which this port does not touch (PORT-NOTES.md). */
#define BC250_PRESET_RADV_BC250_MESH_ONCE "false"
#define BC250_PRESET_BC250_EXPERIMENTAL_DIRECT_SPLIT "true"
#define BC250_PRESET_BC250_EXPERIMENTAL_SKIP_INACTIVE_CHUNKS "true"
#define BC250_PRESET_RADV_BC250_SPLIT_MESH "true"
#define BC250_PRESET_RADV_BC250_EXPAND_PRIMITIVES "true"
#define BC250_PRESET_BC250_EXPERIMENTAL_CULL_COMPACT "true"
#define BC250_PRESET_BC250_EXPERIMENTAL_PACK_TRIANGLE_VERTICES "true"
#define BC250_PRESET_BC250_EXPERIMENTAL_POST_MESH_VGT_FLUSH "0"
#define BC250_PRESET_BC250_CAPTURE_RAW_IBS "false"
#define BC250_PRESET_RADV_BC250_EXPOSE_FAST_BINDING "1"
#define BC250_PRESET_RADV_BC250_LOCAL_BOS "1"
#define BC250_PRESET_RADV_BC250_PERF_PIECE_PRIMS "64"
#define BC250_PRESET_RADV_BC250_SCRATCH_REUSE "1"
#define BC250_PRESET_RADV_BC250_MESH_DIRECT_READ "full"
#define BC250_PRESET_RADV_BC250_MESH_AUTOCULL "1"
#define BC250_PRESET_RADV_BC250_MESH_AUTOCULL_WIDE "1"
/* Direct Mesh path. */
#define BC250_PRESET_RADV_BC250_MESH_SAFE_FAST "1"
#define BC250_PRESET_RADV_BC250_MESH_SAFE_PIECES "1"
#define BC250_PRESET_RADV_BC250_MESH_SAFE_OWNED "1"
#define BC250_PRESET_RADV_BC250_MESH_SAFE_LOCAL "1"
#define BC250_PRESET_RADV_BC250_MESH_SAFE_CHECK "1"
#define BC250_PRESET_RADV_BC250_MESH_SAFE_CORNERS "1"
#define BC250_PRESET_RADV_BC250_MESH_SAFE_AUTOCULL "1"
#define BC250_PRESET_RADV_BC250_MESH_SAFE_PARALLEL "1"
#define BC250_PRESET_RADV_BC250_MESH_SAFE_BARY "1"
#define BC250_PRESET_RADV_BC250_BARY_IO16 "1"
#define BC250_PRESET_RADV_BC250_MESH_SAFE_BARY_TINY "1"
#define BC250_PRESET_RADV_BC250_MESH_SAFE_BARY_AFFINE "1"
#define BC250_PRESET_RADV_BC250_MESH_SAFE_BARY_LAST "1"
#define BC250_PRESET_RADV_BC250_BARY_CORNER_ID "1"
#define BC250_PRESET_RADV_BC250_MESH_ALLOW_POS1 "1"
#define BC250_PRESET_RADV_BC250_MESH_SPLIT_ANY "1"
#define BC250_PRESET_RADV_BC250_MESH_NESTED_SLICE "1"
#define BC250_PRESET_RADV_BC250_MESH_SAFE_SPLIT_PIECES "1"
#define BC250_PRESET_RADV_BC250_MESH_SAFE_ADAPTIVE "1"
/* Additional direct routes; an explicit zero still overrides this preset in the lab flavour. */
#define BC250_PRESET_RADV_BC250_MESH_LDS_PLAN "1"
#define BC250_PRESET_RADV_BC250_MESH_LDS_COVER "1"
#define BC250_PRESET_RADV_BC250_MESH_MERGED_CHECK "1"
#define BC250_PRESET_RADV_BC250_MESH_PP_DIRECT "1"
#define BC250_PRESET_RADV_BC250_MESH_PIECES_65 "1"
#define BC250_PRESET_RADV_BC250_MESH_IDXPASS "1"
/* Never the raw route: a Mesh pipeline without a protected route is refused. */
#define BC250_PRESET_RADV_BC250_MESH_FAIL_CLOSED "1"
/* More shapes admitted to the safe routes (gates PP1, TG1, PX1). */
#define BC250_PRESET_RADV_BC250_MESH_DEAD_PAYLOAD "1"
#define BC250_PRESET_RADV_BC250_MESH_PIECE_PRIMID "1"
#define BC250_PRESET_RADV_BC250_MESH_SAFE_PIECES_EXT "1"
#define BC250_PRESET_RADV_BC250_TASK_GRID_FOLD "1"
/* Renumbered referenced vertices after culling instead of private corners (gate CM1). */
#define BC250_PRESET_RADV_BC250_MESH_SAFE_COMPACT "1"
/* Shared vertices on the owned route, private provoking corners (gates PS1, PS2). */
#define BC250_PRESET_RADV_BC250_MESH_PP_SHARE "1"
/* Skip adaptive checks whose result is already known (gate LC1). */
#define BC250_PRESET_RADV_BC250_MESH_LEAN_CHECK "1"
#define BC250_PRESET_RADV_BC250_EXPOSE_MULTIVIEW_MESH "1"
#define BC250_PRESET_RADV_BC250_MESH_SPLIT_STORES "1"
/* Multiview Mesh: the view layer is a per-vertex export (GFX10.1 has no layer in the primitive export), and
 * Task+Mesh draws run the Task stage once and the Mesh stage once per view. */
#define BC250_PRESET_RADV_BC250_MESH_MULTIVIEW_VTX "1"
#define BC250_PRESET_RADV_BC250_TASK_MULTIVIEW "1"
#define BC250_PRESET_RADV_BC250_TASK_TAIL "1"
#define BC250_PRESET_RADV_BC250_EXPOSE_DGC "1"
/* RADV_BC250_NGGC_WIDE and RADV_BC250_DCC_32B are not in the preset, although the reference's preset sets both.
 * Neither has anything to do with mesh shaders: the first raises the NGG culling admission threshold from 8 to 12
 * for every pipeline (radv_device.c), the second makes every colour surface on GFX1013 use a 32-byte DCC minimum
 * compressed block (ac_descriptors.c). They would therefore change rendering for every DXVK and vkd3d-proton title
 * on the board, on the reference's unpublished "GDDR6 and 24 CUs" claim, and a wrong DCC minimum block size is an
 * image-corruption class in front of the release gates. Both switches stay available in the lab flavour for a
 * measured A/B against the b29 baseline. */

/* Every name of the preset, in the order the lab build applies them. X(name) for each. */
#define BC250_DIRECTMESH_PRESET(X) \
   X(RADV_BC250_NATIVE_TASK) X(BC250_EXPERIMENTAL_COMPUTE_CU_MODE) X(BC250_BALANCED_SLICES) \
   X(BC250_SINGLE_PIECE) X(BC250_CACHE_PLAN) X(BC250_TRANSIENT_ARENA) X(BC250_PARALLEL_CULL) \
   X(BC250_COMPACT_VERTICES) X(BC250_OUTPUT_REGIONS) X(RADV_BC250_MESH_ONCE) \
   X(BC250_EXPERIMENTAL_DIRECT_SPLIT) X(BC250_EXPERIMENTAL_SKIP_INACTIVE_CHUNKS) X(RADV_BC250_SPLIT_MESH) \
   X(RADV_BC250_EXPAND_PRIMITIVES) X(BC250_EXPERIMENTAL_CULL_COMPACT) X(BC250_EXPERIMENTAL_PACK_TRIANGLE_VERTICES) \
   X(BC250_EXPERIMENTAL_POST_MESH_VGT_FLUSH) X(BC250_CAPTURE_RAW_IBS) X(RADV_BC250_EXPOSE_FAST_BINDING) \
   X(RADV_BC250_LOCAL_BOS) X(RADV_BC250_PERF_PIECE_PRIMS) X(RADV_BC250_SCRATCH_REUSE) \
   X(RADV_BC250_MESH_DIRECT_READ) X(RADV_BC250_MESH_AUTOCULL) X(RADV_BC250_MESH_AUTOCULL_WIDE) \
   X(RADV_BC250_MESH_SAFE_FAST) X(RADV_BC250_MESH_SAFE_PIECES) X(RADV_BC250_MESH_SAFE_OWNED) \
   X(RADV_BC250_MESH_SAFE_LOCAL) X(RADV_BC250_MESH_SAFE_CHECK) X(RADV_BC250_MESH_SAFE_CORNERS) \
   X(RADV_BC250_MESH_SAFE_AUTOCULL) X(RADV_BC250_MESH_SAFE_PARALLEL) X(RADV_BC250_MESH_SAFE_BARY) \
   X(RADV_BC250_BARY_IO16) X(RADV_BC250_MESH_SAFE_BARY_TINY) X(RADV_BC250_MESH_SAFE_BARY_AFFINE) \
   X(RADV_BC250_MESH_SAFE_BARY_LAST) X(RADV_BC250_BARY_CORNER_ID) X(RADV_BC250_MESH_ALLOW_POS1) \
   X(RADV_BC250_MESH_SPLIT_ANY) X(RADV_BC250_MESH_NESTED_SLICE) X(RADV_BC250_MESH_SAFE_SPLIT_PIECES) \
   X(RADV_BC250_MESH_SAFE_ADAPTIVE) X(RADV_BC250_MESH_LDS_PLAN) X(RADV_BC250_MESH_LDS_COVER) \
   X(RADV_BC250_MESH_MERGED_CHECK) X(RADV_BC250_MESH_PP_DIRECT) X(RADV_BC250_MESH_PIECES_65) \
   X(RADV_BC250_MESH_IDXPASS) X(RADV_BC250_MESH_FAIL_CLOSED) X(RADV_BC250_MESH_DEAD_PAYLOAD) \
   X(RADV_BC250_MESH_PIECE_PRIMID) X(RADV_BC250_MESH_SAFE_PIECES_EXT) X(RADV_BC250_TASK_GRID_FOLD) \
   X(RADV_BC250_MESH_SAFE_COMPACT) X(RADV_BC250_MESH_PP_SHARE) X(RADV_BC250_MESH_LEAN_CHECK) \
   X(RADV_BC250_EXPOSE_MULTIVIEW_MESH) X(RADV_BC250_MESH_SPLIT_STORES) X(RADV_BC250_MESH_MULTIVIEW_VTX) \
   X(RADV_BC250_TASK_MULTIVIEW) X(RADV_BC250_TASK_TAIL) X(RADV_BC250_EXPOSE_DGC)

/* True when the DirectMesh preset applies to this process: a BC-250 is present. radv sets it at instance
 * creation; it is never cleared, as the lab build never removes a preset value from the environment. */
bool bc250_env_preset(void);
void bc250_env_set_preset(void);

/* The lab build: put every preset value into the process environment that is not set there already, so that an
 * explicit setting of the user wins. */
void bc250_env_apply_directmesh_preset(void);

/* Both forms of each read. Only the forms below the flavour switch are used outside the test. The name argument is
 * only stringified (#) or pasted (##), so a macro of the same name never expands in its place. */
#define BC250_ENV_BOOL_LAB(n, d) debug_get_bool_option(#n, d)
#define BC250_ENV_BOOL_P_LAB(n, d) debug_get_bool_option(#n, d)
#define BC250_ENV_NUM_LAB(n, d) debug_get_num_option(#n, d)
#define BC250_ENV_NUM_P_LAB(n, d) debug_get_num_option(#n, d)
#define BC250_ENV_STR_LAB(n, d) debug_get_option(#n, d)
#define BC250_ENV_STR_P_LAB(n, d) debug_get_option(#n, d)
#define BC250_GETENV_LAB(n) getenv(#n)
#define BC250_GETENV_P_LAB(n) getenv(#n)
#define BC250_OS_OPTION_LAB(n) os_get_option(#n)
#define BC250_GET_ONCE_BOOL_OPTION_LAB(s, n, d) DEBUG_GET_ONCE_BOOL_OPTION(s, #n, d)
#define BC250_GET_ONCE_BOOL_OPTION_P_LAB(s, n, d) DEBUG_GET_ONCE_BOOL_OPTION(s, #n, d)
#define BC250_GET_ONCE_OPTION_LAB(s, n, d) DEBUG_GET_ONCE_OPTION(s, #n, d)

#define BC250_ENV_BOOL_REL(n, d) ((bool)(d))
#define BC250_ENV_BOOL_P_REL(n, d) debug_parse_bool_option(bc250_env_preset() ? BC250_PRESET_##n : NULL, d)
#define BC250_ENV_NUM_REL(n, d) ((int64_t)(d))
#define BC250_ENV_NUM_P_REL(n, d) debug_parse_num_option(bc250_env_preset() ? BC250_PRESET_##n : NULL, d)
#define BC250_ENV_STR_REL(n, d) ((const char *)(d))
#define BC250_ENV_STR_P_REL(n, d) (bc250_env_preset() ? BC250_PRESET_##n : (const char *)(d))
#define BC250_GETENV_REL(n) ((char *)NULL)
#define BC250_GETENV_P_REL(n) (bc250_env_preset() ? (char *)BC250_PRESET_##n : (char *)NULL)
#define BC250_OS_OPTION_REL(n) ((const char *)NULL)
#define BC250_GET_ONCE_BOOL_OPTION_REL(s, n, d) \
   static bool debug_get_option_##s(void) { return (d); }
#define BC250_GET_ONCE_BOOL_OPTION_P_REL(s, n, d) \
   static bool debug_get_option_##s(void) \
   { \
      static bool initialized = false; \
      static bool value; \
      if (unlikely(!p_atomic_read_relaxed(&initialized))) { \
         bool parsed_value = debug_parse_bool_option(bc250_env_preset() ? BC250_PRESET_##n : NULL, d); \
         p_atomic_set(&value, parsed_value); \
         p_atomic_set(&initialized, true); \
      } \
      return value; \
   }
#define BC250_GET_ONCE_OPTION_REL(s, n, d) \
   static const char *debug_get_option_##s(void) { return (d); }

/* Object-like names for the forms, so that the arguments reach the form unexpanded. */
#ifdef AMDGPU_WDDM_LAB_FLAVOUR
#define BC250_ENV_BOOL BC250_ENV_BOOL_LAB
#define BC250_ENV_BOOL_P BC250_ENV_BOOL_P_LAB
#define BC250_ENV_NUM BC250_ENV_NUM_LAB
#define BC250_ENV_NUM_P BC250_ENV_NUM_P_LAB
#define BC250_ENV_STR BC250_ENV_STR_LAB
#define BC250_ENV_STR_P BC250_ENV_STR_P_LAB
#define BC250_GETENV BC250_GETENV_LAB
#define BC250_GETENV_P BC250_GETENV_P_LAB
#define BC250_OS_OPTION BC250_OS_OPTION_LAB
#define BC250_GET_ONCE_BOOL_OPTION BC250_GET_ONCE_BOOL_OPTION_LAB
#define BC250_GET_ONCE_BOOL_OPTION_P BC250_GET_ONCE_BOOL_OPTION_P_LAB
#define BC250_GET_ONCE_OPTION BC250_GET_ONCE_OPTION_LAB
#else
#define BC250_ENV_BOOL BC250_ENV_BOOL_REL
#define BC250_ENV_BOOL_P BC250_ENV_BOOL_P_REL
#define BC250_ENV_NUM BC250_ENV_NUM_REL
#define BC250_ENV_NUM_P BC250_ENV_NUM_P_REL
#define BC250_ENV_STR BC250_ENV_STR_REL
#define BC250_ENV_STR_P BC250_ENV_STR_P_REL
#define BC250_GETENV BC250_GETENV_REL
#define BC250_GETENV_P BC250_GETENV_P_REL
#define BC250_OS_OPTION BC250_OS_OPTION_REL
#define BC250_GET_ONCE_BOOL_OPTION BC250_GET_ONCE_BOOL_OPTION_REL
#define BC250_GET_ONCE_BOOL_OPTION_P BC250_GET_ONCE_BOOL_OPTION_P_REL
#define BC250_GET_ONCE_OPTION BC250_GET_ONCE_OPTION_REL
#endif

#ifdef __cplusplus
}
#endif

#endif /* BC250_ENV_H */
