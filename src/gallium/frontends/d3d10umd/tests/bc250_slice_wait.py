"""Host test of the sliced Present-idle wait of the hosted UMD (bc250_slice_wait.h, K225).

Builds bc250_slice_wait_test.c with cl against bc250_slice_wait.h and runs it.
--negative builds it against a copy of the header that ends the wait after one
slice, as the single 10 s wait before K225 did; that build must compile and the
innocent case must then fail. The source check asserts that
Bc250WaitPresentIdle() uses the helper and no longer has the single 10 s wait.

Usage: python bc250_slice_wait.py --out DIR [--negative]
Run it from a Visual Studio developer shell (cl on PATH).
"""
import argparse
import pathlib
import subprocess
import sys

RULE = "if (waited >= total_ms)\n         return BC250_SLICE_WAIT_BOUND;"
ONE_SLICE = "if (waited >= slice_ms)\n         return BC250_SLICE_WAIT_BOUND;"


def source_check(umd):
    device = (umd / "Device.cpp").read_text().replace("\r\n", "\n")
    start = device.index("HRESULT Bc250WaitPresentIdle(Device *device)")
    end = device.index("\n}\n", start)
    wait = device[start:end]
    assert '#include "bc250_slice_wait.h"' in device, "Device.cpp does not include the helper"
    assert "bc250_slice_wait(event," in wait, "Bc250WaitPresentIdle() does not call the helper"
    assert "WaitForSingleObject(event,10000)" not in wait, "Bc250WaitPresentIdle() still has the single 10 s wait"
    header = (umd / "bc250_slice_wait.h").read_text().replace("\r\n", "\n")
    assert RULE in header, "the total-bound rule is gone from bc250_slice_wait.h"
    print("PASS source check: Bc250WaitPresentIdle() uses the sliced wait")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--out", type=pathlib.Path, required=True)
    parser.add_argument("--negative", action="store_true")
    args = parser.parse_args()
    umd = pathlib.Path(__file__).resolve().parents[1]
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    source_check(umd)

    include = umd
    if args.negative:
        include = out / "negative-include"
        include.mkdir(exist_ok=True)
        header = (umd / "bc250_slice_wait.h").read_text().replace("\r\n", "\n")
        assert header.count(RULE) == 1
        (include / "bc250_slice_wait.h").write_text(header.replace(RULE, ONE_SLICE))

    exe = out / ("bc250_slice_wait_negative.exe" if args.negative else "bc250_slice_wait_test.exe")
    obj = out / (exe.stem + ".obj")
    build = subprocess.run(["cl", "/nologo", "/std:c11", "/W4", "/WX", "/MD", "/I", str(include),
                            str(umd / "tests" / "bc250_slice_wait_test.c"), "/Fe:" + str(exe), "/Fo:" + str(obj)],
                           cwd=out, capture_output=True, text=True)
    (out / (exe.stem + "-build.log")).write_text(build.stdout + build.stderr)
    if build.returncode:
        print(build.stdout + build.stderr)
        print("FAIL build")
        return 1
    run = subprocess.run([str(exe)], cwd=out, capture_output=True, text=True, timeout=120)
    print(run.stdout, end="")
    if args.negative:
        innocent_failed = "FAIL innocent:" in run.stdout
        if run.returncode and innocent_failed:
            print("PASS negative control: the one-slice wait fails the innocent case")
            return 0
        print("FAIL negative control: the one-slice wait passed the innocent case")
        return 1
    return run.returncode


if __name__ == "__main__":
    sys.exit(main())
