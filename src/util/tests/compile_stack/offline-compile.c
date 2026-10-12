/* SPDX-License-Identifier: MIT
 * Real SPIR-V -> NIR -> ACO compilation, no GPU, no winsys/device emulation.
 * The public Vulkan/D3D integration tests are separate.
 */
#include <windows.h>
#include <stdio.h>
#include "radv_shader.h"
#include "radv_pipeline_compute.h"
#include "radv_pipeline_graphics.h"
#include "radv_pipeline.h"
#include "ac_gpu_info.h"
#include "compiler/glsl_types.h"
#include "util/u_compile_stack.h"
#include "vk_debug_report.h"

struct task {
   struct radv_compiler_info *ci;
   char *spirv;
   uint32_t size;
   struct radv_shader_binary *binary;
   bool direct;
   bool graphics;
};

static __declspec(noinline) void compile_graphics(void *opaque)
{
   struct task *t = opaque;
   /* Match the stage-array footprint in radv_shader_object_init_graphics;
    * execute the actual graphics compiler, without creating/uploading a GPU object.
    */
   struct radv_shader_stage stages[MESA_VULKAN_SHADER_STAGES] = {0};
   struct radv_shader_binary *binaries[MESA_VULKAN_SHADER_STAGES] = {0};
   struct radv_shader_debug_info debug[MESA_VULKAN_SHADER_STAGES] = {0};
   struct radv_shader_debug_info gs_debug = {0};
   struct radv_shader_binary *gs_binary = NULL;
   struct radv_graphics_state_key gfx = {0};
   for (unsigned i = 0; i < MESA_VULKAN_SHADER_STAGES; ++i) {
      stages[i].stage = MESA_SHADER_NONE;
      stages[i].next_stage = MESA_SHADER_NONE;
   }
   stages[MESA_SHADER_FRAGMENT].stage = MESA_SHADER_FRAGMENT;
   stages[MESA_SHADER_FRAGMENT].entrypoint = "main";
   stages[MESA_SHADER_FRAGMENT].spirv.data = t->spirv;
   stages[MESA_SHADER_FRAGMENT].spirv.size = t->size;
   gfx.ps.color_outputs_need_epilog = true;
   gfx.ps.depth_output_needs_epilog = true;
   gfx.ps.stencil_output_needs_epilog = true;
   gfx.ps.sample_mask_output_needs_epilog = true;
   gfx.dynamic_rasterization_samples = true;
   gfx.dynamic_provoking_vtx_mode = true;
   gfx.smooth_lines_may_be_enabled = true;
   gfx.rs.polygon_mode_unknown = true;
   for (unsigned i = 0; i < MAX_RTS; ++i) gfx.ps.epilog.color_map[i] = i;
   radv_graphics_shaders_compile(t->ci, NULL, stages, &gfx, false, NULL, false, debug,
                                 binaries, &gs_debug, &gs_binary);
   t->binary = binaries[MESA_SHADER_FRAGMENT];
   for (unsigned i = 0; i < MESA_VULKAN_SHADER_STAGES; ++i) {
      if (i != MESA_SHADER_FRAGMENT) free(binaries[i]);
      ralloc_free(stages[i].nir);
   }
   free(gs_binary);
}

static LONG CALLBACK fault(EXCEPTION_POINTERS *e)
{
   void *frames[32];
   unsigned n = CaptureStackBackTrace(0, 32, frames, NULL);
   uintptr_t base = (uintptr_t)GetModuleHandleW(NULL);
   fprintf(stderr, "exception %08lx at RVA %llx\n", e->ExceptionRecord->ExceptionCode,
           (unsigned long long)((uintptr_t)e->ExceptionRecord->ExceptionAddress - base));
   for (unsigned i = 0; i < n; ++i)
      fprintf(stderr, "frame %u RVA %llx\n", i, (unsigned long long)((uintptr_t)frames[i] - base));
   return EXCEPTION_CONTINUE_SEARCH;
}

static __declspec(noinline) void compile(void *opaque)
{
   struct task *t = opaque;
   struct radv_shader_stage *s = calloc(1, sizeof(*s));
   struct radv_shader_debug_info dbg = {0};
   if (!s) return;
   s->stage = MESA_SHADER_COMPUTE;
   s->next_stage = MESA_SHADER_NONE;
   s->entrypoint = "main";
   s->spirv.data = t->spirv;
   s->spirv.size = t->size;
   t->binary = radv_compile_cs(t->ci, s, false, &dbg);
   radv_pipeline_stage_finish(s);
   free(s);
}

static DWORD WINAPI run(void *opaque)
{
   struct task *t = opaque;
   util_compile_stack_func func = t->graphics ? compile_graphics : compile;
   if (t->direct) func(t);
   else if (!util_call_on_compile_stack(t->graphics ? "real SPIR-V NIR ACO graphics" : "real SPIR-V NIR ACO compute", func, t)) return 2;
   return t->binary ? 0 : 3;
}

int main(int argc, char **argv)
{
   if (argc < 2) return 2;
   AddVectoredExceptionHandler(1, fault);
   SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
   SetEnvironmentVariableA("MESA_COMPILE_STACK_STATS", "1");
   FILE *f = fopen(argv[1], "rb");
   if (!f) return 2;
   fseek(f, 0, SEEK_END); long size = ftell(f); rewind(f);
   if (size <= 0 || size > 1048576) return 2;
   char *spirv = malloc(size);
   if (!spirv || fread(spirv, 1, size, f) != (size_t)size) return 2;
   fclose(f);
   struct radeon_info *ri = calloc(1, sizeof(*ri));
   struct radv_compiler_info *ci = calloc(1, sizeof(*ci));
   if (!ri || !ci) return 2;
   ri->gfx_level = GFX10;
   ri->family = CHIP_NAVI10; /* gfx101x compiler fixture, no hardware assertion */
   ac_fill_compiler_info(ri, NULL, false);
   ci->ac = &ri->compiler_info;
   ci->key.cs_wave_size = ci->key.ge_wave_size = ci->key.ps_wave_size = ci->key.rt_wave_size = 32;
   ci->subgroup_size = 64; ci->min_subgroup_size = 32; ci->max_subgroup_size = 64;
   ci->cache_disabled = true;
   struct vk_debug_report report = {0};
   list_inithead(&report.callbacks);
   ci->debug.debug_report = &report;
   ci->spirv_caps.Shader = true;
   ci->spirv_caps.PhysicalStorageBufferAddresses = true;
   ci->hw.address32_hi = 0xffff8000;
   radv_get_nir_options(ci);
   glsl_type_singleton_init_or_ref();
   bool direct = false, graphics = false;
   for (int i = 2; i < argc; ++i) {
      if (!strcmp(argv[i], "--unprotected")) direct = true;
      if (!strcmp(argv[i], "--graphics")) graphics = true;
   }
   struct task small = {ci, spirv, (uint32_t)size, NULL, direct, graphics};
   struct task control = {ci, spirv, (uint32_t)size, NULL, true, graphics};
   HANDLE t = CreateThread(NULL, 8 * 1024 * 1024, run, &control, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
   if (!t || WaitForSingleObject(t, 60000) != WAIT_OBJECT_0 || !control.binary) return 3;
   CloseHandle(t);
   t = CreateThread(NULL, 64 * 1024, run, &small, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
   if (!t || WaitForSingleObject(t, 60000) != WAIT_OBJECT_0 || !small.binary) return 4;
   CloseHandle(t);
   bool same = small.binary->total_size == control.binary->total_size &&
               !memcmp(small.binary, control.binary, small.binary->total_size);
   printf("real-compile: binary_size=%u identical_to_8MiB_control=%u\n", small.binary->total_size, same);
   free(small.binary); free(control.binary); free(spirv); free(ci); free(ri);
   glsl_type_singleton_decref();
   return same ? 0 : 5;
}
