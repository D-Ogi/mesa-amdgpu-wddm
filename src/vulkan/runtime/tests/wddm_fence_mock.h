/* Offline mock boundary for production Win32 monitored-fence host tests. */
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <limits.h>
#include <assert.h>
#include <stdatomic.h>
#define unlikely(x) (x)
#define container_of(p,t,m) ((t*)((char*)(p)-offsetof(t,m)))
#define ASSERTED
#define VK_SUCCESS 0
#define VK_ERROR_UNKNOWN -13
#define VK_ERROR_OUT_OF_HOST_MEMORY -1
#define VK_ERROR_FEATURE_NOT_PRESENT -8
#define VK_ERROR_INVALID_EXTERNAL_HANDLE -1000072003
#define VK_ERROR_DEVICE_LOST -4
#define STATUS_SUCCESS 0
#define STATUS_NO_MEMORY ((int)0xc0000017)
#define STATUS_DEVICE_REMOVED ((int)0xc00002b6)
#define STATUS_INVALID_PARAMETER ((int)0xc000000d)
#define STATUS_INVALID_HANDLE ((int)0xc0000008)
#define STATUS_ACCESS_DENIED ((int)0xc0000022)
#define NT_SUCCESS(s) ((s)>=0)
#define D3DDDI_MONITORED_FENCE 5
#define D3DDDI_SYNC_OBJECT_ALL_ACCESS 0x1f0003
#define D3DDDI_SYNC_OBJECT_SIGNAL 2u
#define D3DDDI_SYNC_OBJECT_WAIT 1u
#define SYNCHRONIZE 0x100000u
#define DUPLICATE_SAME_ACCESS 2
#define OBJ_INHERIT 2
#define OBJ_CASE_INSENSITIVE 0x40
enum vk_sync_flags { VK_SYNC_IS_TIMELINE = 1, VK_SYNC_IS_SHAREABLE = 2, VK_SYNC_IS_SHARED = 4 };
#define VK_SYNC_FEATURE_TIMELINE 1
#define VK_SYNC_FEATURE_GPU_WAIT 2
#define VK_SYNC_FEATURE_CPU_WAIT 4
#define VK_SYNC_FEATURE_CPU_SIGNAL 8
#define VK_SYNC_FEATURE_WAIT_ANY 16
#define VK_SYNC_FEATURE_WAIT_BEFORE_SIGNAL 32
#define VK_SYNC_FEATURE_WAIT_PENDING 64
#define BC250_WDDM_CALL(host,fn,arg) mock_##fn(arg)
#define WDDM2_DISPATCH(x) mock_##x
#define vk_error(dev,r) (r)
#define vk_errorf(dev,r,...) (r)
#define vk_device_set_lost(dev,...) VK_ERROR_DEVICE_LOST
typedef int VkResult,NTSTATUS,BOOL;
typedef unsigned long ULONG,DWORD;
typedef unsigned short USHORT;
typedef void *HANDLE,*PVOID;
typedef wchar_t *PWSTR;
typedef const wchar_t *LPCWSTR;
typedef struct {DWORD nLength;void *lpSecurityDescriptor;BOOL bInheritHandle;} SECURITY_ATTRIBUTES;
struct vk_device {uint32_t wddm2_handle;struct {void *dispatch;} bc250_host;};
struct vk_sync;struct vk_sync_wait;
enum vk_sync_wait_flags {VK_SYNC_WAIT_ANY=1};
struct vk_sync_type {
 size_t size;unsigned features;
 VkResult (*init)(struct vk_device*,struct vk_sync*,uint64_t);
 void (*finish)(struct vk_device*,struct vk_sync*);
 VkResult (*signal)(struct vk_device*,struct vk_sync*,uint64_t);
 VkResult (*get_value)(struct vk_device*,struct vk_sync*,uint64_t*);
 VkResult (*wait_many)(struct vk_device*,uint32_t,const struct vk_sync_wait*,enum vk_sync_wait_flags,uint64_t);
 VkResult (*export_win32_handle)(struct vk_device*,struct vk_sync*,void**);
 VkResult (*import_win32_handle)(struct vk_device*,struct vk_sync*,void*,const wchar_t*);
 VkResult (*set_win32_export_params)(struct vk_device*,struct vk_sync*,const void*,uint32_t,const wchar_t*);
};
struct vk_sync {const struct vk_sync_type *type;enum vk_sync_flags flags;};
struct mock_flags {
    union {
        struct {
            unsigned Shared:1, NtSecuritySharing:1, CrossAdapter:1, TopOfPipeline:1;
            unsigned NoSignal:1, NoWait:1, NoSignalMaxValueOnTdr:1, NoGPUAccess:1, Reserved:24;
        };
        unsigned Value;
    };
};
struct mock_monitored {uint64_t InitialFenceValue;unsigned EngineAffinity;uint64_t *FenceValueCPUVirtualAddress;uint64_t FenceValueGPUVirtualAddress;};
typedef struct {unsigned hDevice;struct {unsigned Type;struct mock_flags Flags;struct mock_monitored MonitoredFence;} Info;unsigned hSyncObject;} D3DKMT_CREATESYNCHRONIZATIONOBJECT2;
typedef struct {unsigned hSyncObject;} D3DKMT_DESTROYSYNCHRONIZATIONOBJECT;
typedef struct {HANDLE hNtHandle;unsigned hDevice;struct mock_flags Flags;unsigned hSyncObject;struct mock_monitored MonitoredFence;} D3DKMT_OPENSYNCOBJECTFROMNTHANDLE2;
static unsigned next_handle=1,next_object=1;
static atomic_uint checks,failures;
static const char *group="startup";
static atomic_flag mock_lock=ATOMIC_FLAG_INIT;
static atomic_uint concurrent_arrived;
static bool concurrent_share;
static unsigned named_opens;
static void lock_mock(void){while(atomic_flag_test_and_set(&mock_lock)) {}}
static void unlock_mock(void){atomic_flag_clear(&mock_lock);}
static struct {bool alive;unsigned object;unsigned access;bool inherit;} nt_handles[512];
static uint64_t values[512];static bool objects[512];
static bool fail_duplicate,fail_open,fail_share;
/* Contract models, not a claim about the untraced lab NTSTATUS. The mapped-only
 * model exercises the bounded retry used by the Windows runtime. */
static bool mapped_only, missing_cpu_map, misaligned_cpu_map, missing_sync_handle, dirty_failed_outputs;
static NTSTATUS open_error;
static unsigned open_calls, packet_attempts, mapped_attempts, destroyed;
static bool retry_outputs_clean = true;
static unsigned named_allowed = D3DDDI_SYNC_OBJECT_ALL_ACCESS;
static bool diagnostic_probe;
static char *mock_getenv(const char *name)
{
    if (!strcmp(name, "MESA_WDDM_FENCE_IMPORT_PROBE")) return diagnostic_probe ? "1" : NULL;
    return getenv(name);
}
#define getenv mock_getenv
static unsigned closes,bad_closes,shares,last_access,last_flags;
static void *last_security;static wchar_t last_name[128];
#define CHECK(c) do{checks++;if(!(c)){failures++;printf("FAIL CHECK %s line %u: %s\n",group,__LINE__,#c);}}while(0)
#undef assert
#define assert(c) CHECK(c)
#define GENERIC_ALL 0x10000000u


static unsigned GetCurrentProcessId(void){return 123;}
static BOOL ProcessIdToSessionId(unsigned p,unsigned long *s){(void)p;*s=3;return true;}
static HANDLE new_nt(unsigned object,unsigned access,bool inherit) {unsigned n=next_handle++;nt_handles[n].alive=true;nt_handles[n].object=object;nt_handles[n].access=access;nt_handles[n].inherit=inherit;return (HANDLE)(uintptr_t)n;}
static bool live(HANDLE h){unsigned n=(unsigned)(uintptr_t)h;return n<512&&nt_handles[n].alive;}
static HANDLE GetCurrentProcess(void){return (HANDLE)(uintptr_t)-1;}
static BOOL CloseHandle(HANDLE h){if(!live(h)){bad_closes++;return false;}nt_handles[(uintptr_t)h].alive=false;closes++;return true;}
static BOOL DuplicateHandle(HANDLE a,HANDLE h,HANDLE b,HANDLE *out,DWORD access,BOOL inherit,DWORD options){(void)a;(void)b;if(fail_duplicate||!live(h))return false;unsigned requested=access==GENERIC_ALL?D3DDDI_SYNC_OBJECT_ALL_ACCESS:access;unsigned available=nt_handles[(uintptr_t)h].access==GENERIC_ALL?D3DDDI_SYNC_OBJECT_ALL_ACCESS:nt_handles[(uintptr_t)h].access;if(!(options&DUPLICATE_SAME_ACCESS)&&(requested&~available))return false;lock_mock();*out=new_nt(nt_handles[(uintptr_t)h].object,(options&DUPLICATE_SAME_ACCESS)?nt_handles[(uintptr_t)h].access:access,inherit);unlock_mock();return true;}
static NTSTATUS mock_CreateSynchronizationObject2(D3DKMT_CREATESYNCHRONIZATIONOBJECT2 *p){if(p->Info.Type!=D3DDDI_MONITORED_FENCE||(p->Info.Flags.NtSecuritySharing&&!p->Info.Flags.Shared)||p->Info.Flags.Reserved)return STATUS_INVALID_PARAMETER;p->hSyncObject=next_object++;objects[p->hSyncObject]=true;values[p->hSyncObject]=p->Info.MonitoredFence.InitialFenceValue;p->Info.MonitoredFence.FenceValueCPUVirtualAddress=&values[p->hSyncObject];p->Info.MonitoredFence.FenceValueGPUVirtualAddress=p->Info.Flags.NoGPUAccess?0:0x200000;return 0;}
static NTSTATUS mock_DestroySynchronizationObject(const D3DKMT_DESTROYSYNCHRONIZATIONOBJECT *p){objects[p->hSyncObject]=false;destroyed++;return 0;}
static NTSTATUS mock_OpenSyncObjectFromNtHandle2(D3DKMT_OPENSYNCOBJECTFROMNTHANDLE2 *p)
{
    open_calls++;
    if (p->Flags.NoGPUAccess) packet_attempts++; else mapped_attempts++;
    if (fail_open) return STATUS_NO_MEMORY;
    if (open_error) return open_error;
    if (!live(p->hNtHandle) || !p->hDevice) return STATUS_INVALID_HANDLE;
    if (p->Flags.Reserved || p->Flags.NoWait || p->Flags.NoSignal) return STATUS_INVALID_PARAMETER;
    if (mapped_only && p->Flags.NoGPUAccess) {
        if (dirty_failed_outputs) {
            p->hSyncObject = 511;
            p->MonitoredFence.FenceValueCPUVirtualAddress = (void *)(uintptr_t)1;
            p->MonitoredFence.FenceValueGPUVirtualAddress = 1;
        }
        return STATUS_INVALID_PARAMETER;
    }
    if (mapped_only && (p->hSyncObject || p->MonitoredFence.FenceValueCPUVirtualAddress ||
                        p->MonitoredFence.FenceValueGPUVirtualAddress)) retry_outputs_clean=false;
    if (missing_sync_handle) return STATUS_SUCCESS;
    p->hSyncObject=next_object++;
    objects[p->hSyncObject]=true;
    p->MonitoredFence.FenceValueCPUVirtualAddress=missing_cpu_map ? NULL : &values[p->hSyncObject];
    if (misaligned_cpu_map) p->MonitoredFence.FenceValueCPUVirtualAddress=(void *)((char *)&values[p->hSyncObject]+1);
    p->MonitoredFence.FenceValueGPUVirtualAddress=p->Flags.NoGPUAccess ? 0 : 0x100000;
    last_flags=p->Flags.Shared|(p->Flags.NtSecuritySharing<<1);
    return STATUS_SUCCESS;
}
static NTSTATUS mock_ShareObjects(unsigned,const unsigned*,void*,unsigned,HANDLE*);
static VkResult vk_wddm2_monitored_fence_signal(struct vk_device*d,struct vk_sync*s,uint64_t v){(void)d;(void)s;(void)v;return 0;}
static VkResult vk_wddm2_monitored_fence_get_value(struct vk_device*d,struct vk_sync*s,uint64_t*v){(void)d;(void)s;(void)v;return 0;}
static VkResult vk_wddm2_monitored_fence_wait_many(struct vk_device*d,uint32_t n,const struct vk_sync_wait*w,enum vk_sync_wait_flags f,uint64_t t){(void)d;(void)n;(void)w;(void)f;(void)t;return 0;}

extern int host_parallel(void (*fn)(void*),void *a,void *b);

typedef wchar_t WCHAR;
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
typedef struct {unsigned dwDesiredAccess;void *pObjAttrib;HANDLE hNtHandle;} D3DKMT_OPENSYNCOBJECTNTHANDLEFROMNAME;
static NTSTATUS mock_OpenSyncObjectNtHandleFromName(D3DKMT_OPENSYNCOBJECTNTHANDLEFROMNAME*);

#define ERROR_NOT_ENOUGH_MEMORY 8
#define ERROR_OUTOFMEMORY 14
static unsigned duplicate_error = 6;
static unsigned GetLastError(void) { return duplicate_error; }



#include "util/u_atomic.h"
