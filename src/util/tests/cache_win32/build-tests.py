# SPDX-License-Identifier: MIT
"""Builds and runs the Windows disk cache tests (cache_win32_test.cpp) against the util library of a Mesa
build directory, without a meson configuration that builds tests.

The compiler, its flags and the include directories are those of the build's own util library (compile_commands.json,
the entry of src/util/u_qsort.cpp), so the tests run the code that the driver DLL of that build links. The build must
be of the lab flavour (AMDGPU_WDDM_LAB_FLAVOUR): the tests put their caches in directories that MESA_SHADER_CACHE_DIR
names, which only the lab flavour reads.

Run it from an x64 Visual Studio developer environment (vcvars64.bat), which gives lld-link the system libraries:

    python build-tests.py --build <mesa build dir> --out <empty dir> [--run]

--run starts the tests in <out>/run with LOCALAPPDATA, TEMP and TMP there too, so that a test of the default directory
writes nothing into the profile of the account that runs it.
"""

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

MESA = Path(__file__).resolve().parents[4]

SOURCES = [
    MESA / 'src/gtest/src/gtest-all.cc',
    MESA / 'src/gtest/src/gtest_main.cc',
    MESA / 'src/util/tests/cache_win32_test.cpp',
]

# The util library and what it needs, as the driver DLL links them (build.ninja of a radv-mt build).
LIBRARIES = [
    'src/util/libmesa_util.a',
    'src/util/libmesa_util_clflush.a',
    'src/util/libmesa_util_clflushopt.a',
    'src/util/libmesa_util_simd.a',
    'src/util/blake3/libblake3.a',
    'src/c11/impl/libmesa_util_c11.a',
    'subprojects/zlib-1.3.1/libz.a',
    'subprojects/zstd-1.5.7/build/meson/lib/libzstd.a',
]
SYSTEM_LIBRARIES = ['synchronization.lib', 'kernel32.lib', 'user32.lib', 'shell32.lib', 'ole32.lib', 'advapi32.lib',
                    'dbghelp.lib']

# Arguments of the library's compile command that belong to that one compile.
DROPPED = re.compile(r'^(/showIncludes|/c|/Fd.*|/Fo.*|.*\.(c|cpp))$', re.IGNORECASE)


def split_command(command):
    """The arguments of a compile_commands.json command: double-quoted strings with \\" inside, or bare words."""
    args = []
    for token in re.findall(r'"(?:[^"\\]|\\.)*"|\S+', command):
        if token.startswith('"') and token.endswith('"'):
            token = token[1:-1].replace('\\"', '"')
        args.append(token)
    return args


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--build', required=True, type=Path, help='Mesa build directory (lab flavour)')
    parser.add_argument('--out', required=True, type=Path, help='directory for objects, the test program and runs')
    parser.add_argument('--run', action='store_true', help='run the tests after the build')
    parser.add_argument('--filter', default='CacheWin32*', help='--gtest_filter of the run')
    opts = parser.parse_args()

    build = opts.build.resolve()
    out = opts.out.resolve()
    if 'LIB' not in os.environ:
        sys.exit('LIB is not set: run from an x64 Visual Studio developer environment (vcvars64.bat)')

    entries = json.loads((build / 'compile_commands.json').read_text(encoding='utf-8'))
    template = next((e for e in entries if e['file'].replace('\\', '/').endswith('src/util/u_qsort.cpp')), None)
    if not template:
        sys.exit(f'{build}: no compile command of src/util/u_qsort.cpp')
    args = split_command(template['command'])
    compiler, flags = args[0], [a for a in args[1:] if not DROPPED.match(a)]
    if '-DAMDGPU_WDDM_LAB_FLAVOUR=1' not in flags:
        sys.exit(f'{build} is not a lab flavour build: the tests need MESA_SHADER_CACHE_DIR')

    version = subprocess.run([compiler, '--version'], capture_output=True, text=True).stdout.splitlines()
    print(f'compiler: {compiler}: {version[0] if version else "?"}')

    out.mkdir(parents=True, exist_ok=True)
    objects = []
    for source in SOURCES:
        obj = out / (source.stem + '.obj')
        includes = [f'-I{MESA / "src/gtest/include"}', f'-I{MESA / "src/gtest"}']
        cmd = [compiler, *flags, *includes, '/c', f'/Fo{obj}', str(source)]
        print(f'compile {source.name}', flush=True)
        if subprocess.run(cmd, cwd=build).returncode:
            sys.exit(f'compile of {source} failed')
        objects.append(str(obj))

    exe = out / 'cache_tests.exe'
    linker = str(Path(compiler).with_name('lld-link.exe'))
    cmd = [linker, '/nologo', '/DEBUG', '/SUBSYSTEM:CONSOLE', f'/OUT:{exe}', *objects,
           *[str(build / lib) for lib in LIBRARIES], *SYSTEM_LIBRARIES]
    print(f'link {exe.name}', flush=True)
    if subprocess.run(cmd, cwd=build).returncode:
        sys.exit('link failed')

    if not opts.run:
        return 0

    run = out / 'run'
    shutil.rmtree(run, ignore_errors=True)
    for sub in ('LocalAppData', 'tmp'):
        (run / sub).mkdir(parents=True)
    env = {k: v for k, v in os.environ.items() if not k.upper().startswith(('MESA_', 'XDG_'))}
    env.update(LOCALAPPDATA=str(run / 'LocalAppData'), TEMP=str(run / 'tmp'), TMP=str(run / 'tmp'))
    result = subprocess.run([str(exe), f'--gtest_filter={opts.filter}', f'--gtest_output=xml:{out / "results.xml"}'],
                            cwd=run, env=env)
    return result.returncode


if __name__ == '__main__':
    sys.exit(main())
