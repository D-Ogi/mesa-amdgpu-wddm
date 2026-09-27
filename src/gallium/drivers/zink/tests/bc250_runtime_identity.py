"""Compile the actual runtime identity logger with minimal host stubs.

Requires MSVC cl on PATH. Usage: python bc250_runtime_identity.py NEW_OUT
"""
from pathlib import Path
import re
import subprocess
import sys

source=Path(__file__).resolve().parents[1]/"zink_resource.c"
s=source.read_text();start=s.index("static void\nbc250_audit_runtime(");end=s.index("\nstatic void\nzink_resource_destroy",start)
body=s[start:end]
out=Path(sys.argv[1]).resolve();out.mkdir(parents=True,exist_ok=False)
fixture=r"""
#include <stdint.h>
#include <stdio.h>
#include <stdbool.h>
#include <stddef.h>
struct pipe_resource { unsigned width0,height0,format,bind; };
struct object { bool bc250_runtime; uint64_t bc250_audit_id; };
struct zink_resource { struct pipe_resource base; struct object *obj; uint64_t bc250_audit_id; };
struct winsys_handle { uintptr_t handle; void *bc250_identity; uint64_t bc250_va; };
static bool enabled=true;
static int bc250_audit_lifetime_lock;
static uint64_t bc250_audit_runtime_sequence;
static bool debug_get_option_bc250_map_lifetime(void) { return enabled; }
static struct zink_resource *zink_resource(struct pipe_resource *p) { return (struct zink_resource *)p; }
static void simple_mtx_lock(int *p) { (void)p; }
static void simple_mtx_unlock(int *p) { (void)p; }
static uint64_t os_time_get_nano(void) { return 42; }
"""
main=r"""
int main(void) {
 struct object obj={true,22};
 struct zink_resource res={{1920,1200,105,0x40000a},&obj,11};
 struct winsys_handle handle={0x1234567887654321ull,(void *)(uintptr_t)0x1122334455667788ull,0x100020003ull};
 bc250_audit_runtime("import",&res.base,&handle);
 bc250_audit_runtime("destroy",&res.base,NULL);
 res.bc250_audit_id=33; obj.bc250_audit_id=44;
 bc250_audit_runtime("import",&res.base,&handle);
 obj.bc250_runtime=false; bc250_audit_runtime("destroy",&res.base,NULL);
 obj.bc250_runtime=true; enabled=false; bc250_audit_runtime("destroy",&res.base,NULL);
 enabled=true;res.obj=NULL;bc250_audit_runtime("destroy",&res.base,NULL);
 return bc250_audit_runtime_sequence==3 ? 0 : 10;
}
"""
(out/"control.c").write_text(fixture+body+main)
r=subprocess.run(["cl","/nologo","/O2","/MD","/W4","/WX","control.c","/Fe:control.exe"],cwd=out,capture_output=True,text=True)
(out/"build.log").write_text(r.stdout+r.stderr);assert r.returncode==0,r.stdout+r.stderr
r=subprocess.run([str(out/"control.exe")],capture_output=True,text=True);assert r.returncode==0
(out/"records.txt").write_text(r.stderr)
rows=[dict(re.findall(r"(\w+)=([^ ]+)",line)) for line in r.stderr.splitlines()]
assert len(rows)==3
assert [x["seq"] for x in rows]==["1","2","3"]
assert [x["event"] for x in rows]==["import","destroy","import"]
assert [x["resource_id"] for x in rows]==["11","11","33"]
assert [x["object_id"] for x in rows]==["22","22","44"]
assert rows[0]["resource"]==rows[2]["resource"]
assert int(rows[0]["allocation"],16)==0x1234567887654321
assert int(rows[0]["identity"],16)==0x1122334455667788
assert int(rows[0]["va"])==0x100020003
print("PASS: import/destroy, pointer reuse with distinct IDs, 64-bit handles, disabled/non-runtime/null-object silence")
