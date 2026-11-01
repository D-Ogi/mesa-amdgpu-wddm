"""amdgpu-wddm: build and run the host test of the run-time driver configuration files (driconf_profile_test.c).

    python src/util/tests/driconf_profile_test.py --build <Windows meson build dir> --clang-cl <clang-cl.exe>
        [--source <this Mesa tree>] [--work <empty work dir>]

The test compiles xmlconfig.c, u_driconf_file.c and u_ere.c of this tree with U_DRICONF_FILE_TEST, once as the
release flavour and once as the lab flavour (AMDGPU_WDDM_LAB_FLAVOUR), against the generated static table
(driconf_static.h) and libmesa_util of a Windows build of the same tree, and runs both. The build dir must be a build
of the same .conf files: the static table decides the expected values of the static cases.
"""
import argparse
import os
import shutil
import subprocess
import sys
import tempfile

ap = argparse.ArgumentParser()
ap.add_argument("--build", required=True)
ap.add_argument("--clang-cl", required=True)
ap.add_argument("--source", default=os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", "..")))
ap.add_argument("--work")
ap.add_argument("--radv-defaults", action="store_true", help="also test generated RADV parser and device defaults")
ap.add_argument("--radv-defaults-baseline", action="store_true",
                help="negative control: restore the old post-parse heap override in the extracted caller")
a = ap.parse_args()
if a.radv_defaults_baseline and not a.radv_defaults:
    ap.error("--radv-defaults-baseline requires --radv-defaults")

src, build = os.path.abspath(a.source), os.path.abspath(a.build)
work = os.path.abspath(a.work) if a.work else tempfile.mkdtemp(prefix="driconf-profile-")
os.makedirs(work, exist_ok=True)
libs = [os.path.join(build, p) for p in (r"src\util\libmesa_util.a", r"src\c11\impl\libmesa_util_c11.a",
                                         r"src\util\blake3\libblake3.a")]
zlib = os.path.join(build, r"subprojects\zlib-1.3.1\libz.a")
if os.path.exists(zlib):
    libs.append(zlib)
for p in [os.path.join(build, r"src\util\driconf_static.h")] + libs:
    if not os.path.exists(p):
        sys.exit("missing " + p)

includes = []
for d in (r"src\util", "include", "src"):
    includes += ["-I" + os.path.join(build, d), "-I" + os.path.join(src, d)]
common = ["/nologo", "/MT", "/utf-8", "/W3", "/clang:-std=c11", "/O1", "/Z7", "-DNO_REGEX", "-DWITH_XMLCONFIG=0",
          "-DU_DRICONF_FILE_TEST", "-D_CRT_SECURE_NO_WARNINGS", "-D_CRT_NONSTDC_NO_DEPRECATE", "-DWIN32_LEAN_AND_MEAN",
          "-D_USE_MATH_DEFINES", "-DNOMINMAX", "-DHAVE_STRUCT_TIMESPEC", "-D__STDC_FORMAT_MACROS",
          "-DSYSCONFDIR=\"c:/etc\"", "-DDATADIR=\"c:/share\"", "-Wno-unused-parameter"] + includes
files = [os.path.join(src, r"src\util\tests\driconf_profile_test.c"), os.path.join(src, r"src\util\xmlconfig.c"),
         os.path.join(src, r"src\util\u_driconf_file.c"), os.path.join(src, r"src\util\u_ere.c")]

if a.radv_defaults:
    sys.path.insert(0, os.path.join(src, "src", "util"))
    sys.path.insert(0, os.path.join(src, "src", "amd", "vulkan"))
    import drirc_gen
    import radv_drirc_gen
    generated = os.path.join(work, "radv_drirc.c")
    drirc_gen.drirc_generate(generated, os.path.join(work, "radv_drirc.h"),
                             "radv", radv_drirc_gen.declare_options())
    # Compile the actual production caller so the platform/APU selection is
    # tested as well as the generated parser. Unrelated device state is modelled.
    with open(os.path.join(src, "src", "amd", "vulkan", "radv_physical_device.c"), encoding="utf-8") as f:
        device_source = f.read()
    start = device_source.index("static void\nradv_init_dri_options(")
    end = device_source.index("\nstatic VkResult", start)
    with open(os.path.join(work, "radv_init_actual.h"), "w", encoding="utf-8") as f:
        caller = device_source[start:end]
        if a.radv_defaults_baseline:
            # Old code used the declared false default, then bypassed the parser
            # policy with getenv. NULL selects that same declared default here.
            begin_defaults = caller.index("   const struct radv_drirc_defaults defaults = {")
            end_defaults = caller.index("   radv_parse_dri_options(", begin_defaults)
            caller = caller[:begin_defaults] + caller[end_defaults:]
            caller = caller.replace("}, &defaults);", "}, NULL);")
            close = caller.rindex("}")
            caller = caller[:close] + '''#ifdef _WIN32
   if (!pdev->info.has_dedicated_vram && !getenv("radv_enable_unified_heap_on_apu"))
      drirc->performance.enable_unified_heap_on_apu = true;
#endif
''' + caller[close:]
        f.write(caller)
    common += ["-I" + work, "-DTEST_RADV_HEAP_DEFAULTS"]
    files += [generated, os.path.join(src, "src", "util", "tests", "radv_heap_defaults_test.c")]

failed = False
for flavour, defines in (("release", []), ("lab", ["-DAMDGPU_WDDM_LAB_FLAVOUR=1"])):
    out = os.path.join(work, flavour)
    os.makedirs(out, exist_ok=True)
    exe = os.path.join(out, "driconf_profile_test.exe")
    cmd = [a.clang_cl] + common + defines + files + ["/Fo" + out + "\\", "/Fe" + exe, "/link"] + libs + \
          ["advapi32.lib", "synchronization.lib", "ws2_32.lib", "ole32.lib", "user32.lib", "shell32.lib", "dbghelp.lib"]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode:
        print(r.stdout[-6000:], r.stderr[-3000:])
        sys.exit(f"{flavour}: build failed")
    data = os.path.join(out, "profiles")
    shutil.rmtree(data, ignore_errors=True)
    os.makedirs(data)
    r = subprocess.run([exe, data], capture_output=True, text=True)
    print(f"== {flavour}")
    print(r.stdout.rstrip())
    print("-- log lines")
    print(r.stderr.rstrip())
    failed |= r.returncode != 0
sys.exit(1 if failed else 0)
