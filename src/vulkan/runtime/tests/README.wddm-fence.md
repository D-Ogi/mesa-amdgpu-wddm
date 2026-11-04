# Win32 monitored-fence host checks

Host checks of src/vulkan/runtime/vk_wddm2_monitored_fence.c. They load no
graphics DLL and submit no GPU work. Full ICD builds and bounded lab tests
remain separate release gates.

## VK_SYNC_WAIT_PENDING (BD-200)

`run_wddm_fence_pending.py` compiles the production CPU wait (the fence poll,
the BD-179 liveness check, the pending wait, the CPU signal and `wait_many`)
with the real Mesa c11 threads, atomics and host bootstrap header against an
inert KMT boundary in which the GPU completes nothing. It runs the cases of
`wddm_fence_pending_test.c` on real host threads:

```text
python src/vulkan/runtime/tests/run_wddm_fence_pending.py --compiler <clang.exe> --out <output-directory>
python src/vulkan/runtime/tests/run_wddm_fence_pending.py --compiler <clang.exe> --out <baseline-output> --baseline a45cf31d
```

The first command must return 0. The baseline command must return 1: before
BD-200 the wait ignores the flag and waits for completion, so the pending
cases fail. The cases: a submitted value satisfies a zero-timeout wait while
the fence value is lower, and a completion wait does not; a CPU signal is
pending; the largest submitted value is kept; a blocked wait ends at the
submission (a submission to another fence does not end it); WAIT_ANY and
WAIT_ALL; the deadline; a shareable or imported payload waits for completion;
and a stopped adapter ends the wait within a slice, also while submissions to
other fences wake it. Execution is bounded to 60 seconds. Limits: the mock
does not model the kernel's ordering of queued signals, GPU completion or
another process's signals.
