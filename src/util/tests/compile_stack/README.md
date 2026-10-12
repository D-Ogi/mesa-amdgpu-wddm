# Windows compiler stack regression

Windows callers can enter the driver on a 64 or 128 KiB stack. Compiler work must enter a separately
reserved stack before its first large frame. `util_call_on_compile_stack` uses a synchronous Win32 fiber
on the calling OS thread, with an 8 MiB reserve, 64 KiB initial commit and a runtime check for at least
6 MiB available. Nested calls reuse sufficient space. A failed allocation returns false without calling
the compiler; the driver reports out-of-memory. Mesa worker threads also request an explicit 8 MiB reserve.

Ordinary TLS and thread identity are unchanged. Floating-point environment is copied in and restored on
return. FLS belongs to the fiber: caller FLS is restored afterward, but callbacks executed by the compiler
must not depend on application FLS being present inside the temporary fiber. No exception may unwind across
the fiber boundary. These are integration constraints, including for runtime and Vulkan allocation callbacks.

The implementation uses the documented [CreateFiberEx stack parameters and FP flag](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-createfiberex),
[TLS/FLS fiber behavior](https://learn.microsoft.com/en-us/windows/win32/procthread/fibers), and
[_beginthreadex reserve flag](https://learn.microsoft.com/en-us/cpp/c-runtime-library/reference/beginthread-beginthreadex).

## Build and helper regression

Use a VS developer shell for the desired architecture. `build-tests.ps1 -Out <absolute-directory>
-Glslang <glslangValidator.exe>` builds the x64 or x86 clients and shaders. It does not install a driver.
Run `compile-stack.exe`: six concurrent small-stack callers exercise nine 96 KiB frames, nesting,
thread TLS, FLS restoration, both types of existing fiber, FP state and allocation/conversion failures.
Run `compile-stack.exe --negative` in a separate process: expected exit is 0xC00000FD. The positive test
also runs under Meson `with_tests` on MSVC. The test executable explicitly reserves 1 MiB for its main thread.

## Real compiler controls without a GPU

In a VS developer shell with ninja in PATH:

```powershell
python build-offline.py --build <existing-radv-build> --out <out>
python build-offline.py --build <existing-llvmpipe-umd-build> --out <out> --llvm
glslangValidator -V --target-env vulkan1.3 -o <out>/offline.spv offline.comp
<out>/offline-compile.exe <out>/offline.spv
<out>/offline-compile.exe <api-out>/stack.frag.spv --graphics
<out>/offline-compile.exe <api-out>/stack.frag.spv --graphics --unprotected
<out>/offline-llvm.exe --large
<out>/offline-llvm.exe
<out>/offline-llvm.exe --unprotected
```

The RADV control relinks the build's actual compiler objects and compares binary output between an 8 MiB
control and a 64 KiB caller. Its gfx101x fixture is not a hardware measurement. The graphics arm includes
the stage-array footprint of shader-object compilation; the unprotected graphics arm must overflow.
The compute case can fit the old stack and is not a required negative control. The LLVM control relinks
the llvmpipe arithmetic JIT suite; the large and protected small arms must pass, the unprotected arm must overflow.

With `MESA_COMPILE_STACK_STATS=1`, each stack switch reports a committed-page high-water bound. This is
page-rounded, at least the initial 64 KiB commit, and excludes guard/no-access pages. It is not an exact SP
sample. The remaining reserve is a diagnostic margin, not a guarantee for arbitrary shader complexity.
With tracing off the helper does not walk committed pages or print per-call diagnostics.

## Public API integration on the AMD adapter

Use `run-api-case.ps1` once per case. Supply `-Client`, `-Case`, `-Artifact`, its `-Sha256`, and a new `-Output`
directory. Each process has a 120-second default bound (170 maximum). The runner checks the artifact hash,
keeps stdout/stderr and result.json, and distinguishes UNAVAILABLE from PASS. No registry changes occur.

Vulkan cases are `shader-object`, `graphics`, `compute`, and `rt`. Artifact is the absolute ICD DLL path.
Each creates its device on a large control stack, disables the disk shader cache, then calls the public
compilation entry from a thread created with exactly 64 KiB reserved. Shader objects link vertex and
fragment stages. RT creates a ray-generation pipeline; this tests compilation, not ray execution.
The ICD must expose an AMD adapter; an absent adapter returns 77 and is not a pass.

D3D cases are `d3d11` for the hosted Zink UMD and `d3d11-cpu` for the llvmpipe UMD. The operator configures
the desktop router for `api-stack.exe` before each arm. Artifact is the selected installed UMD path;
the client refuses a different loaded path. It uses inbox D3D11/DXGI, compiles DXBC on the main thread,
then creates the shaders and draws on the 64 KiB thread. Readback must be opaque green, not the red clear.
Select the paired hosted ICD along with the Zink UMD and record both hashes in the lab manifest.

Run old artifacts first and preserve failures, then the candidate. Use `-ExpectStackOverflow` only for
the expected failing old arm. Do not require every old API case to overflow: the deterministic graphics
compiler control establishes the regression, and the lab must establish the real DWM/public API behavior.
This kit does not claim lab validation merely because the offline compiler controls pass.
