"""Host test of the zink batch state return in zink_context_destroy().

Builds bc250_batch_list_test.c with cl against zink_bc250_batch_list.h and
runs it. --negative builds it against a copy of the header without the guard
that keeps a listed current state from being appended twice; that build must
compile and the test must then fail by its cycle check. The source check also
asserts that zink_context_destroy() uses the guarded helper and no longer
appends ctx->bs by hand.

Usage: python bc250_batch_list.py --out DIR [--negative]
Run it from a Visual Studio developer shell (cl on PATH).
"""
import argparse
import pathlib
import subprocess
import sys

GUARD = "if (current && !current_listed) {"
UNGUARDED = "if (current && (current_listed || !current_listed)) {"


def source_check(zink):
    context = (zink / "zink_context.c").read_text()
    start = context.index("zink_context_destroy(struct pipe_context *pctx)")
    end = context.index("\n}\n", start)
    destroy = context[start:end]
    assert '#include "zink_bc250_batch_list.h"' in context, "zink_context.c does not include the helper"
    assert "bc250_batch_return_states(" in destroy, "zink_context_destroy() does not call the helper"
    assert "screen->last_free_batch_state->next = ctx->bs" not in destroy, \
        "zink_context_destroy() still appends ctx->bs by hand"
    assert "if (ctx->bs && !bs_listed)" in destroy, "zink_context_destroy() clears a listed ctx->bs twice"
    header = (zink / "zink_bc250_batch_list.h").read_text()
    assert GUARD in header, "the guard is gone from zink_bc250_batch_list.h"
    print("PASS source check: zink_context_destroy() uses the guarded helper")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--out", type=pathlib.Path, required=True)
    parser.add_argument("--negative", action="store_true")
    args = parser.parse_args()
    zink = pathlib.Path(__file__).resolve().parents[1]
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    source_check(zink)

    include = zink
    if args.negative:
        include = out / "negative-include"
        include.mkdir(exist_ok=True)
        header = (zink / "zink_bc250_batch_list.h").read_text()
        assert header.count(GUARD) == 1
        (include / "zink_bc250_batch_list.h").write_text(header.replace(GUARD, UNGUARDED))

    exe = out / ("bc250_batch_list_negative.exe" if args.negative else "bc250_batch_list_test.exe")
    obj = out / (exe.stem + ".obj")
    build = subprocess.run(["cl", "/nologo", "/std:c11", "/W4", "/WX", "/MD", "/I", str(include),
                            str(zink / "tests" / "bc250_batch_list_test.c"), "/Fe:" + str(exe), "/Fo:" + str(obj)],
                           cwd=out, capture_output=True, text=True)
    (out / (exe.stem + "-build.log")).write_text(build.stdout + build.stderr)
    if build.returncode != 0:
        print(build.stdout + build.stderr)
        print("FAIL: build error (a negative control must fail by a check, not by the build)")
        return 2

    # Without the guard the D1 shape makes bs->next == bs, and the helper's own walk to the end of
    # the list then never ends: the same spin as DWM in lab trial D1. A run that does not end within
    # the bound is that spin; the last RUN line names the case.
    proc = subprocess.Popen([str(exe)], cwd=out, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    try:
        stdout, _ = proc.communicate(timeout=10)
        spun = False
    except subprocess.TimeoutExpired:
        proc.kill()
        stdout, _ = proc.communicate()
        spun = True
    runs = [line[5:] for line in stdout.splitlines() if line.startswith("RUN  ")]
    if spun:
        stdout += "FAIL %s: the return of the states did not end within 10 s (cycle)\n" % (runs[-1] if runs else "?")
    (out / (exe.stem + ".log")).write_text(stdout)
    print(stdout, end="")
    if args.negative:
        if (spun or proc.returncode != 0) and runs and runs[0] == "d1-current-in-batch-states-empty-screen" \
                and ("cycle" in stdout):
            print("PASS negative control: without the guard the D1 shape makes a cycle")
            return 0
        print("FAIL negative control: the unguarded helper was not caught")
        return 1
    if spun:
        return 1
    return proc.returncode


if __name__ == "__main__":
    sys.exit(main())
