#!/usr/bin/env python3
"""Compile the production query/BC250 memory blocks with inert host mocks."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

p = argparse.ArgumentParser()
p.add_argument('--out', type=Path, required=True)
p.add_argument('--cc', default='gcc')
p.add_argument('--legacy-overwrite', action='store_true')
a = p.parse_args()
a.out.mkdir(parents=True, exist_ok=True)
source = Path(__file__).resolve().parents[1] / 'radv_wddm2_winsys.c'
s = source.read_text()
start = s.index('   /* GTT size */')
end = s.index('   /* Nodes information */', start)
query = s[start:end]
start = s.index('   status = query_adapter_info(ws, KMTQAITYPE_UMDRIVERPRIVATE')
end = s.index('   info->drm_major', start)
parse = s[start:end]
assert 'segment_status = query_adapter_info' in query
assert 'radv_wddm2_try_bc250(ws, &dev, &mem, segment_status, compiler_compat_mode)' in s
assert s.index('   /* GTT size */') < s.index('if (radv_wddm2_try_bc250(ws,')
assert s.index('   mem->vram.total_heap_size') < s.index('   ac_fill_memory_info(info, dev, mem);')
if a.legacy_overwrite:
    first = parse.index('   if (!NT_SUCCESS(segment_status))')
    last = parse.index('\n   }', first) + len('\n   }')
    parse = parse[:first] + '   mem->gtt.total_heap_size = bc250_rd64(blob, BC250_OFF_MEMORY + 2 * 32);' + parse[last:]
code = r'''
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
typedef int32_t NTSTATUS;
#define NT_SUCCESS(s) ((s)>=0)
#define KMTQAITYPE_GETSEGMENTSIZE 1
#define KMTQAITYPE_UMDRIVERPRIVATE 2
#define BC250_CAPS_MAGIC 0x42433235u
#define BC250_OFF_DEVICE 16
#define BC250_OFF_MEMORY 464
struct heap { uint64_t total_heap_size; };
struct drm_amdgpu_memory_info { struct heap vram,cpu_accessible_vram,gtt; };
struct drm_amdgpu_info_device { unsigned char bytes[448]; };
typedef struct {uint64_t SharedSystemMemorySize;} D3DKMT_SEGMENTSIZEINFO;
static NTSTATUS os_status,blob_status;
static uint64_t os_size;
static unsigned warnings,queries,checks,failed;
static bool valid_blob;
static char warning[256];
static uint32_t bc250_rd32(const uint8_t*p,uint32_t o){uint32_t v;memcpy(&v,p+o,4);return v;}
static uint64_t bc250_rd64(const uint8_t*p,uint32_t o){uint64_t v;memcpy(&v,p+o,8);return v;}
static NTSTATUS query_adapter_info(void*ws,int kind,void*out,size_t n){
 (void)ws;(void)n;
 if(kind==1){queries++;((D3DKMT_SEGMENTSIZEINFO*)out)->SharedSystemMemorySize=os_size;return os_status;}
 uint8_t *b=out;memset(b,0,n);uint32_t magic=valid_blob?BC250_CAPS_MAGIC:0,ver=3;
 uint64_t local=0x2e5800000ull,captured=0xefa35000ull;
 memcpy(b,&magic,4);memcpy(b+4,&ver,4);memcpy(b+464,&local,8);memcpy(b+496,&local,8);memcpy(b+528,&captured,8);
 return blob_status;
}
static int mock_fprintf(FILE*f,const char*fmt,...){(void)f;va_list ap;va_start(ap,fmt);vsnprintf(warning,sizeof(warning),fmt,ap);va_end(ap);warnings++;return 0;}
#define fprintf mock_fprintf
static bool parse_caps(void*ws,struct drm_amdgpu_info_device*dev,struct drm_amdgpu_memory_info*mem,NTSTATUS segment_status){
 uint8_t blob[1472];NTSTATUS status;(void)segment_status;
PARSE
 return true;
}
static struct drm_amdgpu_memory_info run(bool *accepted){
 void *ws=NULL;struct drm_amdgpu_memory_info mem={0};struct drm_amdgpu_info_device dev={0};NTSTATUS segment_status;
QUERY
 *accepted=parse_caps(ws,&dev,&mem,segment_status);return mem;
}
#undef fprintf
#define CHECK(x) do{checks++;if(!(x)){printf("FAIL CHECK line%d: %s\n",__LINE__,#x);failed++;}}while(0)
static void testcase(NTSTATUS status,uint64_t size,bool valid,NTSTATUS bs){
 os_status=status;os_size=size;valid_blob=valid;blob_status=bs;warnings=queries=0;warning[0]=0;
 bool accepted=false;struct drm_amdgpu_memory_info m=run(&accepted);
 bool ours=valid&&NT_SUCCESS(bs);CHECK(accepted==ours);CHECK(queries==1);
 uint64_t expected=NT_SUCCESS(status)?size:(ours?0xefa35000ull:0);
 CHECK(m.gtt.total_heap_size==expected);
 CHECK(warnings==(unsigned)(ours&&!NT_SUCCESS(status)));
 if(ours){CHECK(m.vram.total_heap_size==0x2e5800000ull);CHECK(m.cpu_accessible_vram.total_heap_size==m.vram.total_heap_size);}
 if(ours&&!NT_SUCCESS(status)){CHECK(strstr(warning,"0xc0000001")!=NULL);CHECK(strlen(warning)>0 && strchr(warning,'\n')==warning+strlen(warning)-1);}
}
int main(void){
 (void)mock_fprintf;
 testcase(0,0x100000000ull,true,0);testcase(0,0,true,0);testcase(1,42,true,0);
 testcase((NTSTATUS)0xc0000001u,0xffffffffffffffffull,true,0);
 testcase(0,123,false,0);testcase((NTSTATUS)0xc0000001u,999,false,0);
 testcase(0,123,true,(NTSTATUS)0xc0000001u);testcase((NTSTATUS)0xc0000001u,999,true,(NTSTATUS)0xc0000001u);
 printf("%u checks, %u failures\n",checks,failed);return failed?1:0;
}
'''.replace('PARSE', parse).replace('QUERY', query)
c = a.out/'memory-test.c'
e = a.out/'memory-test.exe'
c.write_text(code)
build = subprocess.run([a.cc,'-std=c11','-Wall','-Wextra','-Werror','-Wno-missing-braces','-Wno-missing-field-initializers',str(c),'-o',str(e)],capture_output=True,text=True)
(a.out/'build.log').write_text(build.stdout+build.stderr)
if build.returncode:
    raise SystemExit('BUILD FAILURE: '+build.stderr)
r = subprocess.run([str(e)],capture_output=True,text=True,timeout=10)
(a.out/'run.log').write_text(r.stdout+r.stderr)
(a.out/'receipt.json').write_text(json.dumps({'source_sha256':hashlib.sha256(source.read_bytes()).hexdigest(),'legacy_overwrite':a.legacy_overwrite,'build_exit':build.returncode,'run_exit':r.returncode,'output':r.stdout},indent=2)+'\n')
print(r.stdout,end='')
raise SystemExit(r.returncode)
