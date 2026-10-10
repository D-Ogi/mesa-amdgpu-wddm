# Win32 monitored-fence ownership host checks

This standalone gate compiles the production fence ownership helpers and dispatch
table, plus the three production `vk_sync` Win32 wrappers, against an inert KMT
and handle boundary. It does not load a graphics DLL or submit GPU work. A separate
Windows fixture creates and joins two temporary CPU threads for concurrent export.

Run from the repository root, supplying a portable Windows Clang compiler:

```text
python src/vulkan/runtime/tests/run_wddm_fence.py --compiler <clang.exe> --out <output-directory>
python src/vulkan/runtime/tests/run_wddm_fence.py --compiler <clang.exe> --out <baseline-output> --baseline ac5ea6cd
```

The baseline command must return 1 because runtime CHECKs expose the old defects;
a compiler failure is not a regression result. Both commands use the same test
fixture. The runner records source, header, wrapper, utility, fixture, compiler,
generated translation unit and executable hashes alongside the output. It bounds
compilation to 60 seconds and execution to 10 seconds. The thread fixture has its
own three-second join limit and exits the test process if a join fails.

Coverage includes caller-owned NT handles, closing and numerically reusing caller
handles, internal re-export and destruction, transactional duplication and KMT-open
failure, both Windows host-memory errors, named import, security descriptor/name/
access/inheritance export attributes, ignored nonshareable attributes, and concurrent
internal exports that remain usable after sync destruction. Production assertions
are recorded as CHECKs instead of aborting; absent old callbacks are checked before
calling them, so the baseline reaches the remaining tests. Repeated exports here
exercise internal `vk_sync` ownership only, not permission to violate the public
Vulkan once-per-semaphore/handle-type export rule (VUID 01127).

Limits: the test boundary models handles, KMT structures and result codes; it does
not establish Windows ABI compatibility, access enforcement, namespace behavior,
GPU fence behavior, or CTS conformance. Signal, value query and waits are outside
this ownership suite and are stubbed. Mesa atomic utilities are included directly.
Full production ICD builds and bounded lab tests remain separate release gates.
