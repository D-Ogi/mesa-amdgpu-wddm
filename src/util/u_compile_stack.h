/* SPDX-License-Identifier: MIT */
#ifndef U_COMPILE_STACK_H
#define U_COMPILE_STACK_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Windows applications (including DWM) can call a driver on a 64 KiB stack.
 * Run compiler transactions synchronously, on the same OS thread, with at least
 * this much stack available before entering the compiler's first stack frame.
 * Non-Windows callers keep their existing execution model. Callbacks must
 * return normally (no exception/unwind across the fiber boundary). Ordinary
 * TLS and thread identity are shared; application FLS is restored on return
 * but is not inherited by the temporary compiler fiber.
 */
#define UTIL_COMPILE_STACK_RESERVE (8u * 1024u * 1024u)
#define UTIL_COMPILE_STACK_MINIMUM (6u * 1024u * 1024u)

typedef void (*util_compile_stack_func)(void *);

/* False means that the callback was NOT called. Callers must propagate OOM;
 * falling back to the original, insufficient stack is never safe.
 */
bool util_call_on_compile_stack(const char *label, util_compile_stack_func func, void *data);

/* Returns the remaining reserved stack, excluding a guard/emergency allowance.
 * Exposed for the runtime invariant and host regression tests.
 */
size_t util_compile_stack_available(void);

#ifdef __cplusplus
}
#endif
#endif
