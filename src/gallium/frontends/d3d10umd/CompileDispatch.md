# D3D compiler stack ownership

`CreateDevice` reserves an 8 MiB compiler worker stack and two events. Failure returns `E_OUTOFMEMORY` before
device creation. Compiler-capable device DDIs reuse those resources until `DestroyDevice` completes.
They do not allocate a fiber or convert the current runtime thread to a fiber. Destroy always calls the full
device teardown on the worker before it joins the thread and releases the events.

This follows the [D3D error rules](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/handling-errors):
`SetShader`, `DestroyDevice` and `QueryGetData` cannot use `E_OUTOFMEMORY` for stack setup. `ResourceMap` also has a
restricted error set. Resource and compiler allocation errors inside each DDI still follow that DDI's contract.
The dispatcher removes its own late allocation failure; it does not audit all allocation failures in Mesa.

Runtime callbacks are synchronous. Each callback runs on the original calling thread, with that thread's TLS
and FLS. The worker waits while the caller pumps callbacks. A callback can reenter a device DDI: the worker pumps
that nested call before the original callback returns. The mailbox data lives on the two existing stacks.
The caller and worker copy the floating-point environment for each work item, then restore their own state.
No C++ exception may cross this boundary, as with the existing DDI ABI.

Each device owns its worker and lock. D3D10 device entries are serialized by the runtime, apart from the
[free-threaded CalcPrivate functions](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/retroactively-requiring-free-threaded-calcprivate-ddis).
Those small queries stay on the caller. Other entries without compiler work also stay on the caller, with the
callback scope installed. Independent devices have independent workers. Destruction must not race a live entry.

Only the base DXGI Present callback is read: a D3D10 device does not grant access to newer DXGI callback slots.
Kernel and core-layer tables retain their existing layout; NULL callbacks stay NULL. The hosted runtime-device
scope is set on both participating threads. A worker never calls a runtime callback from its own OS thread.

## Host regression

Use an x64 or x86 VS developer PowerShell with the WDK and SDK 10.0.26100 include paths in `INCLUDE`:

```powershell
./test-compile-dispatch.ps1 -Out <output-directory>
```

The test instantiates the production DDI wrappers with WDK signatures and deterministic bodies. It injects
failure at all four dispatcher setup allocations through `CreateDevice`, and checks that no handles leak.
After successful creation it denies every further dispatcher allocation. `SetShader`, `ResourceMap`,
`QueryGetData` and `DestroyDevice` must still run, with correct outputs and no added error callback.
The bodies consume 96 KiB below an entry from a 64 KiB runtime thread. The test checks synchronous callback
reentry, callback thread identity, TLS/FLS, FP state, NULL callbacks, and two concurrent devices that migrate
across new runtime threads. It does not substitute for running actual Mesa device bodies on the adapter.

The cost of a worker handoff on the selected DDIs remains to be measured on the adapter. DWM, both UMD routes,
WoW64 production binaries, D3D12 and WGL frontend stack coverage still need their own validation. This test is
not an HLK pass and does not establish a Microsoft minimum caller-stack size.
