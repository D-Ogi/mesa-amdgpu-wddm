"""Test the actual read-only host query case, including rejected ABI inputs."""
from pathlib import Path
import subprocess
import sys
root=Path(__file__).resolve().parents[5]
source=(root/"src/gallium/frontends/d3d10umd/Device.cpp").read_text()
a=source.index("   case BC250_HOST_AUDIT_PRESENT: {");b=source.index("   case BC250_HOST_PUBLISH_PROGRESS:",a)
header=(root/"src/util/bc250_host_bootstrap.h").as_posix()
out=Path(sys.argv[1]).resolve();out.mkdir(parents=True,exist_ok=False)
code='#include "'+header+'"\n'+r"""
#include <assert.h>
#define S_OK 0
#define E_INVALIDARG ((int32_t)0x80070057)
struct Device { uint32_t profilePresents; };
struct State { Device *device; uint64_t present_value; uint32_t present_sync; };
static int32_t query(State *s, uint32_t op, void *argument) {
 switch(op) {
"""+source[a:b]+r"""
 default:return E_INVALIDARG;
 }
}
int main() {
 Device dev={29}; State state={&dev,0x100000003ull,0x12345678};
 bc250_host_present_audit a={}; a.size=sizeof(a);a.version=1;
 assert(query(&state,BC250_HOST_AUDIT_PRESENT,&a)==S_OK);
 assert(a.completed==29 && a.signaled==0x100000003ull && a.sync==0x12345678);
 assert(dev.profilePresents==29 && state.present_value==0x100000003ull);
 a.completed=123;a.size--;
 assert(query(&state,BC250_HOST_AUDIT_PRESENT,&a)==E_INVALIDARG && a.completed==123);
 a.size=sizeof(a);a.version=2;
 assert(query(&state,BC250_HOST_AUDIT_PRESENT,&a)==E_INVALIDARG && a.completed==123);
 a.version=1;a.reserved=1;
 assert(query(&state,BC250_HOST_AUDIT_PRESENT,&a)==E_INVALIDARG && a.completed==123);
 return 0;
}
"""
(out/"control.cpp").write_text(code)
r=subprocess.run(["cl","/nologo","/O2","/MD","/W4","/WX","control.cpp","/Fe:control.exe"],cwd=out,capture_output=True,text=True)
(out/"build.log").write_text(r.stdout+r.stderr);assert r.returncode==0,r.stdout+r.stderr
subprocess.run([str(out/"control.exe")],check=True)
print("PASS: actual query case, 64-bit fence, read-only state, size/version/reserved rejection")
