"""Relink the actual Mesa build objects with a GPU-free compiler test main."""
import ctypes
import json
from pathlib import Path
import subprocess
import argparse

parser = argparse.ArgumentParser()
parser.add_argument('--build', type=Path, required=True)
parser.add_argument('--out', type=Path, required=True)
parser.add_argument('--llvm', action='store_true')
options = parser.parse_args()
root = options.out.resolve()
root.mkdir(parents=True, exist_ok=True)
source_root = Path(__file__).resolve().parent
llvm = options.llvm
build = options.build.resolve()
stem = "offline-llvm" if llvm else "offline-compile"
split = ctypes.windll.shell32.CommandLineToArgvW
split.argtypes = [ctypes.c_wchar_p, ctypes.POINTER(ctypes.c_int)]
split.restype = ctypes.POINTER(ctypes.c_wchar_p)

def argv(command):
    count = ctypes.c_int()
    ptr = split(command, ctypes.byref(count))
    result = [ptr[i] for i in range(count.value)]
    ctypes.windll.kernel32.LocalFree(ptr)
    return result

commands = json.loads((build / "compile_commands.json").read_text())
source = "bc250_lp_test_main.c" if llvm else "radv_pipeline_compute.c"
c = next(c for c in commands if c["file"].endswith(source))
args = argv(c["command"])
args = [a for a in args if not a.startswith("/Fo") and not a.endswith(source)]
args += ["/Fo" + str(root / (stem + ".obj")), str(source_root / (stem + ".c"))]
subprocess.run(args, cwd=build, check=True)
commands = json.loads(subprocess.check_output(["ninja", "-t", "compdb", "-x"], cwd=build))
c = next(c for c in commands if c["output"].endswith("bc250_lp_test_arit.exe" if llvm else "vulkan_radeon.dll"))
args = argv(c["command"])
args = [a for a in args if a.upper() != "/DLL" and not a.upper().startswith(("/OUT:", "/PDB:", "/IMPLIB:", "/DEF:"))]
args = [a for a in args if not (llvm and a.endswith("bc250_lp_test_main.c.obj"))]
args += [str(root / (stem + ".obj")), "/OUT:" + str(root / (stem + ".exe")),
         "/PDB:" + str(root / (stem + ".pdb")), "/STACK:8388608"]
rsp = root / (stem + "-link.rsp")
rsp.write_text("\n".join(subprocess.list2cmdline([a]) for a in args[1:]), encoding="utf-8")
subprocess.run([args[0], "@" + str(rsp)], cwd=build, check=True)
