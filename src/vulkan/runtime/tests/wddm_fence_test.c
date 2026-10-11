
/* Production functions precede this file in the generated test translation unit. */
static NTSTATUS mock_ShareObjects(unsigned n, const unsigned *object, void *attributes,
                                 unsigned access, HANDLE *out)
{
    OBJECT_ATTRIBUTES *oa = attributes;
    (void)n;
    if (concurrent_share) {
        atomic_fetch_add(&concurrent_arrived, 1);
        while (atomic_load(&concurrent_arrived) < 2) {}
    }
    lock_mock();
    shares++;
    last_access = access;
    last_security = oa->SecurityDescriptor;
    last_name[0] = 0;
    if (oa->ObjectName) {
        size_t count = oa->ObjectName->Length / sizeof(wchar_t);
        if (count > 127) count = 127;
        memcpy(last_name, oa->ObjectName->Buffer, count * sizeof(wchar_t));
        last_name[count] = 0;
    }
    NTSTATUS status = fail_share ? STATUS_NO_MEMORY : STATUS_SUCCESS;
    if (!fail_share) *out = new_nt(*object, access, (oa->Attributes & OBJ_INHERIT) != 0);
    unlock_mock();
    return status;
}
static NTSTATUS mock_OpenSyncObjectNtHandleFromName(D3DKMT_OPENSYNCOBJECTNTHANDLEFROMNAME *p)
{
    OBJECT_ATTRIBUTES *oa = p->pObjAttrib;
    named_opens++;
    CHECK(oa && oa->ObjectName);
    if (oa && oa->ObjectName) {
        CHECK(wcscmp(oa->ObjectName->Buffer, L"\\Sessions\\3\\BaseNamedObjects\\named-import") == 0);
    }
    if (p->dwDesiredAccess & ~named_allowed) return STATUS_ACCESS_DENIED;
    p->hNtHandle = new_nt(200, p->dwDesiredAccess, false);
    return STATUS_SUCCESS;
}
static struct vk_device device = {.wddm2_handle = 7};
static struct vk_wddm2_monitored_fence make_fence(void)
{
    struct vk_wddm2_monitored_fence f = {0};
    f.base.type = &vk_wddm2_monitored_fence_type;
    f.base.flags = VK_SYNC_IS_SHAREABLE;
    CHECK(f.base.type->init(&device, &f.base, 0x100000002ull) == VK_SUCCESS);
    CHECK(*f.value_map == 0x100000002ull);
    return f;
}
struct export_call { struct vk_sync *sync; HANDLE handle; VkResult result; };
static void export_worker(void *opaque)
{
    struct export_call *call = opaque;
    call->result = vk_sync_export_win32_handle(&device, call->sync, &call->handle);
}
int main(void)
{
    group = "caller ownership, caller-close, internal re-export, destroy";
    struct vk_wddm2_monitored_fence f = make_fence();
    HANDLE caller = new_nt(100, GENERIC_ALL, true), exported = NULL;
    CHECK(vk_sync_import_win32_handle(&device, &f.base, caller, NULL) == VK_SUCCESS);
    CHECK(f.shared_handle != caller);
    CHECK(live(caller));
    CHECK(CloseHandle(caller));
    CHECK(vk_sync_export_win32_handle(&device, &f.base, &exported) == VK_SUCCESS);
    CHECK(live(exported));
    if (live(exported)) CloseHandle(exported);
    unsigned bad_before = bad_closes;
    f.base.type->finish(&device, &f.base);
    CHECK(bad_closes == bad_before);
    /* Recycle caller's numerical value as a new unrelated object. Destroy must not close it. */
    f = make_fence(); caller = new_nt(101, GENERIC_ALL, false);
    CHECK(vk_sync_import_win32_handle(&device, &f.base, caller, NULL) == VK_SUCCESS);
    CloseHandle(caller);
    nt_handles[(uintptr_t)caller].alive = true;
    nt_handles[(uintptr_t)caller].object = 999;
    f.base.type->finish(&device, &f.base);
    CHECK(live(caller));
    if (live(caller)) CloseHandle(caller);

    group = "transactional duplicate and KMT-open failures";
    f = make_fence(); caller = new_nt(102, GENERIC_ALL, false);
    unsigned old = f.handle; HANDLE old_nt = f.shared_handle; uint64_t *old_map = f.value_map;
    fail_duplicate = true;
    CHECK(vk_sync_import_win32_handle(&device, &f.base, caller, NULL) != VK_SUCCESS);
    CHECK(f.handle == old && f.shared_handle == old_nt && f.value_map == old_map);
    CHECK(live(caller)); fail_duplicate = false;
    old = f.handle; old_nt = f.shared_handle; old_map = f.value_map;
    fail_open = true; unsigned before = closes;
    CHECK(vk_sync_import_win32_handle(&device, &f.base, caller, NULL) != VK_SUCCESS);
    CHECK(f.handle == old && f.shared_handle == old_nt && f.value_map == old_map);
    CHECK(live(caller)); CHECK(closes == before + 1); fail_open = false;
    if (live(caller)) CloseHandle(caller);
    f.base.type->finish(&device, &f.base);

    group = "duplicate OOM preserves destination and caller";
    for (unsigned error = 8; error <= 14; error += 6) {
        f = make_fence();
        caller = new_nt(103, GENERIC_ALL, false);
        old = f.handle; old_nt = f.shared_handle; old_map = f.value_map;
        duplicate_error = error; fail_duplicate = true;
        CHECK(vk_sync_import_win32_handle(&device, &f.base, caller, NULL) == VK_ERROR_OUT_OF_HOST_MEMORY);
        CHECK(f.handle == old && f.shared_handle == old_nt && f.value_map == old_map);
        CHECK(live(caller));
        fail_duplicate = false; duplicate_error = 6;
        f.base.type->finish(&device, &f.base);
        if (live(caller)) CloseHandle(caller);
    }
    group = "named import uses NT name opening and owns resulting handle";
    f = make_fence(); unsigned calls_before = named_opens;
    CHECK(vk_sync_import_win32_handle(&device, &f.base, NULL, L"Local\\named-import") == VK_SUCCESS);
    CHECK(named_opens == calls_before + 1);
    CHECK(live(f.shared_handle));
    HANDLE named = f.shared_handle;
    f.base.type->finish(&device, &f.base);
    CHECK(!live(named));

    group = "custom security, access, name, inheritance before first export";
    f = make_fence();
    CHECK(f.base.type->set_win32_export_params != NULL);
    if (f.base.type->set_win32_export_params) {
        int descriptor = 42; SECURITY_ATTRIBUTES sa = {sizeof(sa), &descriptor, true};
        CHECK(vk_sync_set_win32_export_params(&device, &f.base, &sa, 0x20001, L"Local\\test-fence") == VK_SUCCESS);
        CHECK(last_access == 0x20001); CHECK(last_security == &descriptor);
        CHECK(wcscmp(last_name, L"\\Sessions\\3\\BaseNamedObjects\\test-fence") == 0);
        CHECK(!nt_handles[(uintptr_t)f.shared_handle].inherit);
        exported = NULL;
        CHECK(vk_sync_export_win32_handle(&device, &f.base, &exported) == VK_SUCCESS);
        CHECK(live(exported));
        if (live(exported)) {
            CHECK(nt_handles[(uintptr_t)exported].inherit);
            CHECK(nt_handles[(uintptr_t)exported].access == 0x20001);
            CloseHandle(exported);
        }

    }
    f.base.type->finish(&device, &f.base);
    group = "transactional export attribute allocation failure";
    f = make_fence();
    if (f.base.type->set_win32_export_params) {
        old = f.handle; old_nt = f.shared_handle; old_map = f.value_map;
        fail_share = true;
        CHECK(vk_sync_set_win32_export_params(&device, &f.base, NULL, 0x9876, NULL) == VK_ERROR_OUT_OF_HOST_MEMORY);
        CHECK(f.handle == old && f.shared_handle == old_nt && f.value_map == old_map);
        fail_share = false;
    }
    f.base.type->finish(&device, &f.base);
    group = "nonshareable export attributes ignored";
    f = make_fence(); f.base.flags = 0;
    /* Baseline has NULL callback: record failure instead of dereferencing NULL. */
    if (f.base.type->set_win32_export_params) {
        unsigned share_before = shares;
        CHECK(vk_sync_set_win32_export_params(&device, &f.base, NULL, 0, L"ignored") == VK_SUCCESS);
        CHECK(shares == share_before);
    } else CHECK(f.base.type->set_win32_export_params != NULL);
    f.base.type->finish(&device, &f.base);

    group = "concurrent internal default exports have independent owned handles";
    f = make_fence();
    struct export_call a = {&f.base, NULL, -1}, b = {&f.base, NULL, -1};
    /* Only the new lazy route enters ShareObjects concurrently. Old retained handles
     * still run both exports and reach the CHECK about retained ownership. */
    concurrent_share = f.shared_handle == NULL;
    atomic_store(&concurrent_arrived, 0);
    CHECK(host_parallel(export_worker, &a, &b) == 0);
    concurrent_share = false;
    CHECK(a.result == VK_SUCCESS && b.result == VK_SUCCESS);
    CHECK(live(a.handle) && live(b.handle) && a.handle != b.handle);
    CHECK((f.base.flags & VK_SYNC_IS_SHARED) != 0);
    f.base.type->finish(&device, &f.base);
    CHECK(live(a.handle) && live(b.handle));
    if (live(a.handle)) CloseHandle(a.handle);
    if (live(b.handle)) CloseHandle(b.handle);
    printf("original ownership gate: %u checks, %u failures\n", checks, failures);

    group = "private destination imports a shared packet-only payload";
    f = make_fence(); f.base.flags = VK_SYNC_IS_TIMELINE;
    caller = new_nt(201, D3DDDI_SYNC_OBJECT_ALL_ACCESS, false);
    unsigned opened_before = open_calls;
    CHECK(vk_sync_import_win32_handle(&device, &f.base, caller, NULL) == VK_SUCCESS);
    CHECK(open_calls == opened_before + 1);
    CHECK(last_flags == 3);
    CHECK(f.value_map != NULL);
    CHECK(live(caller));
    f.base.type->finish(&device, &f.base); CloseHandle(caller);

    group = "mapped-only object retries INVALID_PARAMETER once, clears stale outputs";
    mapped_only = true; dirty_failed_outputs = true;
    f = make_fence(); caller = new_nt(202, D3DDDI_SYNC_OBJECT_ALL_ACCESS, false);
    opened_before = open_calls; unsigned packet_before=packet_attempts, mapped_before=mapped_attempts;
    CHECK(vk_sync_import_win32_handle(&device, &f.base, caller, NULL) == VK_SUCCESS);
    CHECK(open_calls == opened_before + 2);
    CHECK(packet_attempts == packet_before + 1 && mapped_attempts == mapped_before + 1);
    CHECK(retry_outputs_clean);
    CHECK(last_flags == 3 && f.value_map != NULL);
    CHECK(live(caller));
    f.base.type->finish(&device, &f.base); CloseHandle(caller);
    mapped_only = false; dirty_failed_outputs = false;

    group = "no retry or payload replacement for access, handle, memory or device errors";
    const NTSTATUS errors[] = {STATUS_ACCESS_DENIED, STATUS_INVALID_HANDLE, STATUS_NO_MEMORY, STATUS_DEVICE_REMOVED};
    const VkResult results[] = {VK_ERROR_INVALID_EXTERNAL_HANDLE, VK_ERROR_INVALID_EXTERNAL_HANDLE,
                               VK_ERROR_OUT_OF_HOST_MEMORY, VK_ERROR_DEVICE_LOST};
    for (unsigned i=0; i<ARRAY_SIZE(errors); i++) {
        f=make_fence(); caller=new_nt(203, D3DDDI_SYNC_OBJECT_ALL_ACCESS, false);
        old=f.handle; old_map=f.value_map; opened_before=open_calls; before=closes;
        open_error=errors[i];
        CHECK(vk_sync_import_win32_handle(&device, &f.base, caller, NULL) == results[i]);
        CHECK(open_calls == opened_before + 1);
        CHECK(f.handle == old && f.value_map == old_map && objects[old]);
        CHECK(live(caller) && closes == before + 1);
        open_error=0; f.base.type->finish(&device, &f.base); CloseHandle(caller);
    }

    group = "persistent invalid parameters get exactly two attempts";
    f=make_fence(); caller=new_nt(204, D3DDDI_SYNC_OBJECT_ALL_ACCESS, false);
    old=f.handle; opened_before=open_calls; before=closes; open_error=STATUS_INVALID_PARAMETER;
    CHECK(vk_sync_import_win32_handle(&device, &f.base, caller, NULL) == VK_ERROR_INVALID_EXTERNAL_HANDLE);
    CHECK(open_calls == opened_before + 2);
    CHECK(f.handle == old && objects[old] && live(caller) && closes == before + 1);
    open_error=0; f.base.type->finish(&device, &f.base); CloseHandle(caller);

    group = "successful KMT open without usable CPU value is rejected transactionally";
    for (unsigned misaligned=0; misaligned<2; misaligned++) {
        missing_cpu_map=!misaligned; misaligned_cpu_map=misaligned;
        f=make_fence(); caller=new_nt(205, D3DDDI_SYNC_OBJECT_ALL_ACCESS, false);
        old=f.handle; old_map=f.value_map; unsigned destroyed_before=destroyed;
        before=closes;
        CHECK(vk_sync_import_win32_handle(&device, &f.base, caller, NULL) == VK_ERROR_INVALID_EXTERNAL_HANDLE);
        CHECK(f.handle == old && f.value_map == old_map && objects[old]);
        CHECK(destroyed == destroyed_before + 1);
        CHECK(live(caller) && closes == before + 1);
        missing_cpu_map=false; misaligned_cpu_map=false;
        f.base.type->finish(&device, &f.base); CloseHandle(caller);
    }

    group = "named import obtains full access independently of the export handle mask";
    f=make_fence(); named_allowed=D3DDDI_SYNC_OBJECT_ALL_ACCESS;
    CHECK(vk_sync_set_win32_export_params(&device, &f.base, NULL, 0x120003, L"Local\\named-import") == VK_SUCCESS);
    CHECK(nt_handles[(uintptr_t)f.shared_handle].access == 0x120003);
    CHECK(vk_sync_import_win32_handle(&device, &f.base, NULL, L"Local\\named-import") == VK_SUCCESS);
    CHECK(nt_handles[(uintptr_t)f.shared_handle].access == D3DDDI_SYNC_OBJECT_ALL_ACCESS);
    CHECK(vk_sync_export_win32_handle(&device, &f.base, &exported) == VK_SUCCESS);
    CHECK(live(exported));
    if (live(exported)) {
        CHECK(nt_handles[(uintptr_t)exported].access == 0x120003);
        CloseHandle(exported);
    }
    f.base.type->finish(&device, &f.base); named_allowed=D3DDDI_SYNC_OBJECT_ALL_ACCESS;

    group = "restrictive object DACL refuses name-open without replacing the destination";
    f=make_fence(); old=f.handle; old_map=f.value_map;
    named_allowed=SYNCHRONIZE|3; opened_before=open_calls;
    CHECK(vk_sync_import_win32_handle(&device, &f.base, NULL, L"Local\\named-import") == VK_ERROR_INVALID_EXTERNAL_HANDLE);
    CHECK(open_calls == opened_before);
    CHECK(f.handle == old && f.value_map == old_map && objects[old]);
    f.base.type->finish(&device, &f.base); named_allowed=D3DDDI_SYNC_OBJECT_ALL_ACCESS;

    group = "insufficient unnamed access fails transactionally without rights escalation";
    f=make_fence(); caller=new_nt(206,SYNCHRONIZE|3,false);
    old=f.handle; old_map=f.value_map; opened_before=open_calls; before=closes;
    CHECK(vk_sync_import_win32_handle(&device, &f.base, caller, NULL) == VK_ERROR_INVALID_EXTERNAL_HANDLE);
    CHECK(open_calls == opened_before + 1);
    CHECK(f.handle == old && f.value_map == old_map && objects[old]);
    CHECK(live(caller) && closes == before + 1);
    f.base.type->finish(&device, &f.base); CloseHandle(caller);

    group = "unnamed handle re-export preserves granted access";
    f=make_fence(); caller=new_nt(206,D3DDDI_SYNC_OBJECT_ALL_ACCESS,false);
    CHECK(vk_sync_import_win32_handle(&device, &f.base, caller, NULL) == VK_SUCCESS);
    CHECK(vk_sync_export_win32_handle(&device, &f.base, &exported) == VK_SUCCESS);
    CHECK(live(exported));
    if (live(exported)) { CHECK(nt_handles[(uintptr_t)exported].access == D3DDDI_SYNC_OBJECT_ALL_ACCESS); CloseHandle(exported); }
    f.base.type->finish(&device, &f.base); CloseHandle(caller);

    group = "successful open without a local handle preserves the old payload";
    f=make_fence(); caller=new_nt(207, D3DDDI_SYNC_OBJECT_ALL_ACCESS, false);
    old=f.handle; unsigned destroyed_before=destroyed;
    missing_sync_handle=true;
    CHECK(vk_sync_import_win32_handle(&device, &f.base, caller, NULL) == VK_ERROR_INVALID_EXTERNAL_HANDLE);
    CHECK(f.handle == old && objects[old] && live(caller));
    CHECK(destroyed == destroyed_before);
    missing_sync_handle=false; f.base.type->finish(&device, &f.base); CloseHandle(caller);

    group = "optional legacy control closes its local payload before the real import";
    diagnostic_probe=true;
    for (unsigned refusal=0; refusal<2; refusal++) {
        mapped_only=refusal; f=make_fence(); f.base.flags=VK_SYNC_IS_TIMELINE;
        caller=new_nt(208,D3DDDI_SYNC_OBJECT_ALL_ACCESS,false);
        opened_before=open_calls; destroyed_before=destroyed;
        CHECK(vk_sync_import_win32_handle(&device, &f.base, caller, NULL) == VK_SUCCESS);
        CHECK(open_calls == opened_before + (refusal ? 3 : 2));
        CHECK(destroyed == destroyed_before + (refusal ? 1 : 2));
        CHECK(live(caller) && f.value_map != NULL);
        f.base.type->finish(&device, &f.base); CloseHandle(caller);
    }
    diagnostic_probe=false; mapped_only=false;

    group = "explicit export access still restricts a re-exported imported payload";
    f=make_fence(); caller=new_nt(209,D3DDDI_SYNC_OBJECT_ALL_ACCESS,false);
    if (f.base.type->set_win32_export_params) {
        CHECK(vk_sync_set_win32_export_params(&device, &f.base, NULL, SYNCHRONIZE|D3DDDI_SYNC_OBJECT_WAIT, NULL) == VK_SUCCESS);
        CHECK(vk_sync_import_win32_handle(&device, &f.base, caller, NULL) == VK_SUCCESS);
        CHECK(vk_sync_export_win32_handle(&device, &f.base, &exported) == VK_SUCCESS);
        CHECK(live(exported));
        if (live(exported)) {
            CHECK(nt_handles[(uintptr_t)exported].access == (SYNCHRONIZE|D3DDDI_SYNC_OBJECT_WAIT));
            CloseHandle(exported);
        }
    }
    f.base.type->finish(&device, &f.base); CloseHandle(caller);

    printf("%u checks, %u failures; mock graphics boundary, real host threads only\n", checks, failures);
    return failures ? 1 : 0;
}
