"""Execute the production image mapper with mocked allocation/Vulkan boundaries."""
import argparse
import pathlib
import subprocess
from bc250_store_audit import function

parser = argparse.ArgumentParser()
parser.add_argument('--out', type=pathlib.Path, required=True)
parser.add_argument('--baseline', action='store_true')
args = parser.parse_args()
root = pathlib.Path(__file__).resolve().parents[1]
source = (root / 'zink_resource.c').read_text()
mapper = function(source, 'zink_image_map')
if args.baseline:
    restriction = ' ||\n       (pres->flags & PIPE_RESOURCE_FLAG_DONT_MAP_DIRECTLY)'
    assert mapper.count(restriction) == 1, 'baseline requires exactly one production restriction'
    mapper = mapper.replace(restriction, '', 1)
out = args.out.resolve()
out.mkdir(parents=True, exist_ok=True)
code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define PIPE_MAP_READ 1
#define PIPE_MAP_WRITE 2
#define PIPE_MAP_UNSYNCHRONIZED 4
#define PIPE_MAP_DEPTH_ONLY 8
#define PIPE_MAP_STENCIL_ONLY 16
#define TC_TRANSFER_MAP_THREADED_UNSYNC 32
#define ZINK_MAP_TEMPORARY 64
#define PIPE_RESOURCE_FLAG_DONT_MAP_DIRECTLY 128
#define PIPE_RESOURCE_FLAG_SPARSE 256
#define ZINK_ALLOC_SPARSE 1
#define VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT 1
#define VK_MEMORY_PROPERTY_HOST_COHERENT_BIT 2
#define PIPE_BUFFER 0
#define PIPE_TEXTURE_3D 3
#define PIPE_USAGE_STAGING 1
#define PIPE_USAGE_STREAM 2
#define PIPE_BIND_LINEAR 1
#define PIPE_FORMAT_S8_UINT 1
#define VK_SUCCESS 0
#define MESA_TRACE_FUNC() ((void)0)
#define VKSCR(n) mock_##n
enum pipe_format { FORMAT_A8 = 1 };
struct pipe_screen { int unused; };
struct pipe_context { struct pipe_screen *screen; };
struct pipe_box { int x,y,z,width,height,depth; };
struct pipe_resource { unsigned flags,target,bind,usage,width0,height0,depth0,last_level,array_size; enum pipe_format format; struct pipe_resource *next; };
struct pipe_transfer { unsigned level,stride,usage; uint64_t layer_stride; };
struct zink_transfer { struct { struct pipe_transfer b; } base; struct pipe_resource *staging_res; unsigned offset,depthPitch; };
struct mock_bo { unsigned placement; };
struct zink_resource_object { bool host_visible,coherent,is_buffer; unsigned image,modifier_aspect; uint64_t size,offset; struct { unsigned present_count; } *dt; struct { struct mock_bo base; } *bo; };
struct zink_resource { struct { struct pipe_resource b; } base; struct zink_resource_object *obj; struct zink_resource *transient; bool unflushed_transient,linear,valid,modifiers; unsigned aspect,fb_bind_count; };
struct zink_context { struct pipe_context base; bool bc250_audit,rp_loadop_changed; };
struct zink_screen { struct pipe_screen base; unsigned dev; struct { struct { struct { unsigned propertyFlags; } memoryTypes[1]; } mem_props; } info; };
struct mem_alloc_info { unsigned aflags; };
struct util_format_description { struct { unsigned height,width,bits; } block; };
typedef struct { unsigned aspect,level,layer; } VkImageSubresource;
typedef struct { uint64_t rowPitch,depthPitch,arrayPitch,offset; } VkSubresourceLayout;
typedef struct { unsigned unused; } VkMappedMemoryRange;
typedef uint64_t VkDeviceSize;
static struct zink_screen screen;
static struct zink_context ctx;
static struct zink_resource staging;
static struct zink_resource_object staging_obj;
static unsigned allocations,copies,waits,direct_maps,staging_maps,destroyed;
static bool fail_allocate,fail_map,borrowed_unmappable;
static uint8_t backing[768*640], packed[704*640];
static struct zink_context *zink_context(struct pipe_context *p) { return (void*)p; }
static struct zink_screen *zink_screen(struct pipe_screen *p) { return (void*)p; }
static struct zink_resource *zink_resource(struct pipe_resource *p) { return (void*)p; }
static uint64_t zink_bo_get_offset(void *p) { (void)p; return 0; }
static uint64_t zink_bo_get_size(void *p) { (void)p; return sizeof backing; }
#define bc250_audit_map_begin(...) 1
#define bc250_audit_map_bucket(...) ((void)0)
#define bc250_audit_map_result(...) ((void)0)
#define zink_is_swapchain(...) false
#define zink_kopper_reads_next_image(...) false
#define zink_kopper_acquire(...) ((void)0)
#define zink_fb_clears_apply_or_discard(...) ((void)0)
#define zink_fb_clears_apply_region(...) ((void)0)
#define zink_resource_usage_is_unflushed_write(...) true
#define zink_resource_usage_wait(...) ((void)0)
#define zink_resource_has_usage(...) false
#define mesa_loge(...) ((void)0)
static unsigned util_format_get_stride(enum pipe_format f,unsigned w) { (void)f; return w; }
static uint64_t util_format_get_2d_size(enum pipe_format f,unsigned s,unsigned h) { (void)f; return (uint64_t)s*h; }
static enum pipe_format util_format_get_depth_only(enum pipe_format f) { return f; }
static const struct util_format_description *util_format_description(enum pipe_format f) { static const struct util_format_description d={{1,1,8}}; (void)f; return &d; }
static struct zink_transfer *create_transfer(struct zink_context *c, struct pipe_resource *r,unsigned u,const struct pipe_box *b) { (void)c;(void)r;(void)b; struct zink_transfer*t=calloc(1,sizeof*t); t->base.b.usage=u;return t; }
static void destroy_transfer(struct zink_context *c,struct zink_transfer*t) { (void)c;destroyed++; free(t); }
static struct pipe_resource *zink_resource_create(struct pipe_screen *s,const struct pipe_resource *t) { (void)s;allocations++; if(fail_allocate)return NULL; staging.base.b=*t;staging.obj=&staging_obj;return &staging.base.b; }
static void zink_transfer_copy_bufimage(struct zink_context*c,struct zink_resource*d,struct zink_resource*s,struct zink_transfer*t) { (void)c;(void)s;assert(d==&staging);copies++;for(unsigned y=0;y<640;y++)memcpy(packed+y*704,backing+y*768,704);assert(t->base.b.stride==704); }
static void zink_fence_wait(struct pipe_context *p) { (void)p;waits++; }
static void *map_resource(struct zink_screen*s,struct zink_resource*r) { (void)s;if(r==&staging){staging_maps++;return fail_map?NULL:packed;}direct_maps++;return borrowed_unmappable?NULL:backing; }
static void mock_GetImageSubresourceLayout(unsigned d,unsigned i,const VkImageSubresource*s,VkSubresourceLayout*l) { (void)d;(void)i;(void)s;*l=(VkSubresourceLayout){768,sizeof backing,sizeof backing,0}; }
static VkMappedMemoryRange zink_resource_init_mem_range(struct zink_screen*s,struct zink_resource_object*o,unsigned x,unsigned n) { (void)s;(void)o;(void)x;(void)n;return (VkMappedMemoryRange){0}; }
static int mock_FlushMappedMemoryRanges(unsigned d,unsigned n,const VkMappedMemoryRange*r) { (void)d;(void)n;(void)r;return VK_SUCCESS; }
'''
code += '\nstatic void\n' + function(source, 'update_obj_info')
code += '\nstatic void *\n' + mapper
code += r'''
static unsigned failures,checks;
#define CHECK(x) do { checks++; if(!(x)){failures++;fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x);} } while(0)
static void run(bool flag,bool linear,bool visible,bool allocation_fail,bool mapping_fail,unsigned usage) {
 struct zink_resource_object obj={0}; struct zink_resource res={0};
 struct { struct mock_bo base; } bo={{0}}; obj.bo=(void*)&bo;
 struct mem_alloc_info ai={0};screen.info.mem_props.memoryTypes[0].propertyFlags=(visible?VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT:0)|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
 res.base.b=(struct pipe_resource){.flags=flag?PIPE_RESOURCE_FLAG_DONT_MAP_DIRECTLY:0,.target=2,.format=FORMAT_A8,.width0=704,.height0=640,.depth0=1,.array_size=1};
 update_obj_info(&screen,&obj,&res.base.b,&ai);
 CHECK(obj.host_visible==visible);res.obj=&obj;res.linear=linear;
 allocations=copies=waits=direct_maps=staging_maps=destroyed=0;
 fail_allocate=allocation_fail;fail_map=mapping_fail;borrowed_unmappable=flag;
 ctx.base.screen=&screen.base;
 struct pipe_transfer *transfer=NULL;struct pipe_box box={0,0,0,704,640,1};
 void *p=zink_image_map(&ctx.base,&res.base.b,0,usage,&box,&transfer);
 bool expected_staging=flag||!linear||!visible;
 if(expected_staging) {
  CHECK(direct_maps==0);CHECK(allocations==1);
  if(allocation_fail||mapping_fail){CHECK(p==NULL);CHECK(transfer==NULL);CHECK(destroyed==1);}
  else {CHECK(p==packed);CHECK(copies==!!(usage&PIPE_MAP_READ));CHECK(waits==!!(usage&PIPE_MAP_READ));CHECK(staging_maps==1);CHECK(transfer&&transfer->stride==704);
   if(p && (usage&PIPE_MAP_READ))for(unsigned y=0;y<640;y++)CHECK(!memcmp((uint8_t*)p+y*704,backing+y*768,704));
   if(usage&PIPE_MAP_WRITE){CHECK(res.valid);if(p){uint8_t old=backing[0];((uint8_t*)p)[0]=(uint8_t)(old^255);CHECK(backing[0]==old);}}}
 } else {CHECK(p==backing);CHECK(direct_maps==1);CHECK(allocations==0);CHECK(transfer&&transfer->stride==768);}
 if(transfer)destroy_transfer(&ctx,(void*)transfer);
 CHECK(destroyed==1);
}
int main(void) {
 for(unsigned y=0;y<640;y++)for(unsigned x=0;x<768;x++)backing[y*768+x]=(uint8_t)(x*17+y*31);
 run(true,true,true,false,false,PIPE_MAP_READ);run(false,true,true,false,false,PIPE_MAP_READ);
 run(false,false,true,false,false,PIPE_MAP_READ);run(false,true,false,false,false,PIPE_MAP_READ);
 run(true,true,true,true,false,PIPE_MAP_READ);run(true,true,true,false,true,PIPE_MAP_READ);
 run(true,true,true,false,false,PIPE_MAP_WRITE);
 printf("image map production path: %u checks, %u failures\n",checks,failures);
 return failures?1:0;
}
'''
c = out / 'image-map.c'
c.write_text(code)
exe = out / 'image-map.exe'
subprocess.run(['clang-cl', '/nologo', '/TC', '/W3', str(c), '/Fe:' + str(exe),
                '/Fo:' + str(out / 'image-map.obj')], check=True)
raise SystemExit(subprocess.run([str(exe)]).returncode)
