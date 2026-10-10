/* Host thread scheduling only. No GPU, Vulkan loader or KMT calls. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
struct call { void (*fn)(void *); void *arg; };
static DWORD WINAPI worker(void *opaque)
{
    struct call *call = opaque;
    call->fn(call->arg);
    return 0;
}
int host_parallel(void (*fn)(void *), void *first, void *second)
{
    struct call calls[2] = {{fn, first}, {fn, second}};
    HANDLE threads[2] = {NULL, NULL};
    threads[0] = CreateThread(NULL, 0, worker, &calls[0], 0, NULL);
    threads[1] = CreateThread(NULL, 0, worker, &calls[1], 0, NULL);
    if (!threads[0] || !threads[1]) ExitProcess(98);
    DWORD wait = WaitForMultipleObjects(2, threads, TRUE, 3000);
    if (wait != WAIT_OBJECT_0) ExitProcess(99);
    CloseHandle(threads[0]);
    CloseHandle(threads[1]);
    return 0;
}
