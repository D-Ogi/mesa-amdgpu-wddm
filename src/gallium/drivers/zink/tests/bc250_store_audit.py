import argparse
import json
import pathlib
import re
import subprocess


def function(source, name):
    start = source.index(name + "(")
    start = source.rfind("\n", 0, start) + 1
    body = source.index("{", start)
    depth = 1
    end = body + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--out", type=pathlib.Path, required=True)
    args = parser.parse_args()
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    root = pathlib.Path(__file__).resolve().parents[1]
    resource = (root / "zink_resource.c").read_text()
    descriptors = (root / "zink_descriptors.c").read_text()
    preamble = r"""
#include <windows.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#define PIPE_BUFFER 0
static bool enabled;
static bool debug_get_option_bc250_map_lifetime(void) { return enabled; }
static SRWLOCK bc250_audit_lifetime_lock = SRWLOCK_INIT;
#define simple_mtx_lock AcquireSRWLockExclusive
#define simple_mtx_unlock ReleaseSRWLockExclusive
static uint64_t bc250_audit_event_sequence;
static volatile LONG64 next_id, ticks;
static uint64_t bc250_audit_id(void) { return InterlockedIncrement64(&next_id); }
static uint64_t os_time_get_nano(void) { return InterlockedIncrement64(&ticks); }
struct pipe_resource { unsigned target; };
struct pipe_transfer { struct pipe_resource *resource; struct { int width, x; } box; };
struct zink_transfer {
   struct { struct pipe_transfer b; } base;
   uint64_t bc250_audit_map_id;
   uintptr_t bc250_audit_map_ptr;
   struct pipe_resource *staging_res;
   unsigned offset;
};
"""
    code = preamble
    code += "\nuint64_t\n" + function(resource, "zink_bc250_audit_store_begin")
    code += "\nvoid\n" + function(resource, "zink_bc250_audit_store_end")
    code += "\nstatic void\n" + function(descriptors, "bc250_descriptor_copy")
    code += r"""
static DWORD WINAPI worker(void *arg)
{
   (void)arg;
   unsigned char dst[128] = {0}, src[16];
   memset(src, 0xa5, sizeof(src));
   struct pipe_resource resource = {PIPE_BUFFER};
   struct zink_transfer trans = {{{&resource, {128, 0}}}, 77, (uintptr_t)dst, NULL, 0};
   for (unsigned i = 0; i < 1000; i++) {
      unsigned offset = (i % 8) * 16;
      bc250_descriptor_copy(&trans.base.b, dst + offset, src, sizeof(src), "thread");
      assert(memcmp(dst + offset, src, sizeof(src)) == 0);
   }
   return 0;
}
int main(void)
{
   assert(zink_bc250_audit_store_begin(NULL, NULL, SIZE_MAX, "disabled", "none") == 0);
   zink_bc250_audit_store_end(0);
   assert(bc250_audit_event_sequence == 0);
   enabled = true;
   unsigned char dst[128] = {0};
   struct pipe_resource resource = {PIPE_BUFFER};
   struct zink_transfer trans = {{{&resource, {128, 0}}}, 42, (uintptr_t)dst, NULL, 0};
   uint64_t id;
#define CHECK(pointer, count, label) \
   id = zink_bc250_audit_store_begin(&trans.base.b, pointer, count, label, "bounds"); \
   zink_bc250_audit_store_end(id)
   CHECK(dst, 128, "exact");
   CHECK(dst + 128, 0, "end_zero");
   trans.staging_res = &resource;
   trans.offset = 4096;
   CHECK(dst, 16, "staging");
   trans.staging_res = NULL;
   trans.base.b.box.x = 128;
   CHECK(dst, 16, "direct_offset");
   trans.base.b.box.x = 0;
   CHECK(dst + 128, 1, "over_end");
   CHECK((void *)((uintptr_t)dst - 1), 1, "under_start");
   CHECK(dst + 1, SIZE_MAX, "overflow");
   resource.target = 2;
   CHECK(dst, 1, "image");
   resource.target = PIPE_BUFFER;
   trans.bc250_audit_map_id = 0;
   CHECK(dst, 1, "missing_id");
   trans.bc250_audit_map_id = 42;
   HANDLE threads[2];
   for (unsigned i = 0; i < 2; i++) {
      threads[i] = CreateThread(NULL, 0, worker, NULL, 0, NULL);
      assert(threads[i]);
   }
   assert(WaitForMultipleObjects(2, threads, TRUE, INFINITE) == WAIT_OBJECT_0);
   for (unsigned i = 0; i < 2; i++) CloseHandle(threads[i]);
   zink_bc250_audit_store_begin(&trans.base.b, dst, 1, "unfinished", "bounds");
   return 0;
}
"""
    (out / "control.c").write_text(code)
    subprocess.run(["cl", "/nologo", "/std:c11", "/W4", "/WX", "/MD",
                    "control.c", "/Fe:control.exe", "/Fo:control.obj"], cwd=out, check=True)
    result = subprocess.run([str(out / "control.exe")], cwd=out, capture_output=True, check=True)
    (out / "control.log").write_bytes(result.stderr)
    pending = {}
    complete = []
    invalid = []
    rows = result.stderr.decode().splitlines()
    for seq, line in enumerate(rows, 1):
        assert line.startswith("BC250 audit store "), line
        row = dict(re.findall(r"(\w+)=(\S+)", line))
        assert int(row["seq"]) == seq
        ident = row["store"]
        if row["event"] == "begin":
            assert ident not in pending
            pending[ident] = row
            if row["valid"] == "0":
                invalid.append(row["writer"])
        else:
            begin = pending.pop(ident)
            assert int(row["time_ns"]) > int(begin["time_ns"])
            complete.append(begin)
    assert invalid == ["over_end", "under_start", "overflow", "image", "missing_id"]
    assert len(complete) == 2009
    assert next(r for r in complete if r["writer"] == "staging")["mapped_offset"] == "4096"
    assert next(r for r in complete if r["writer"] == "direct_offset")["mapped_offset"] == "128"
    assert len(pending) == 1 and next(iter(pending.values()))["writer"] == "unfinished"
    thread_writes = [r for r in complete if r["writer"] == "thread"]
    assert len(thread_writes) == 2000
    assert all(r["valid"] == "1" and r["map"] == "77" for r in thread_writes)
    assert sum(int(r["bytes"]) for r in thread_writes) == 32000
    summary = dict(completed=len(complete), pending=len(pending), invalid=invalid,
                   memcpy_bytes=32000, disabled_silent=True, threads=2)
    (out / "validation.json").write_text(json.dumps(summary, indent=2) + "\n")
    print(json.dumps(summary))


if __name__ == "__main__":
    main()
