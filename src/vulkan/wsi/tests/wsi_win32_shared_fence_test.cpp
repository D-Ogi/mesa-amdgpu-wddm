/* SPDX-License-Identifier: MIT */
#include "../wsi_win32_shared_fence.h"
#include <cstdint>
#include <cstdio>
#include <cstring>

static unsigned checks, failures;
static void check(bool ok, const char *what)
{
   ++checks;
   if (!ok) { ++failures; std::printf("FAIL %s\n", what); }
}

/* Models the measured r6 object, not a fabricated failing HRESULT: Open succeeds
 * but the returned D3D12 view has no monitored value, while the Vulkan view is 0.
 * A runtime-created fence retains a shared payload and has working queue methods.
 */
struct Mock {
   unsigned fail_step = 0, step = 0;
   unsigned made_fence = 0, released_fence = 0, made_timeline = 0, destroyed_timeline = 0;
   unsigned made_handle = 0, closed_handle = 0, published = 0, opens = 0;
   bool fence = false, handle = false, timeline = false, imported = false;
   bool bad_created_value = false, bad_imported_value = false;
   uint64_t value = 0;
   int enter() { return ++step == fail_step ? -int(step) : 0; }
   int create_fence() { int r = enter(); if (!r) { fence = true; ++made_fence; } return r; }
   int check_fence() { int r = enter(); return r ? r : (bad_created_value ? -100 : 0); }
   int share_fence() { int r = enter(); if (!r) { handle = true; ++made_handle; } return r; }
   int create_timeline() { int r = enter(); if (!r) { timeline = true; ++made_timeline; } return r; }
   int import_fence() {
      check(fence && timeline && handle, "permanent import has live inputs");
      int r = enter(); if (!r) imported = true; return r;
   }
   void close_handle() { if (handle) ++closed_handle; handle = false; }
   int check_timeline() {
      check(!handle && imported, "import keeps payload after application handle closes");
      int r = enter(); return r ? r : (bad_imported_value ? -101 : check_fence());
   }
   void publish() {
      check(fence && timeline && imported, "publish only complete pair");
      ++published;
      fence = timeline = false; // caller takes both references
   }
   void discard() {
      if (timeline) ++destroyed_timeline;
      if (fence) ++released_fence;
      fence = timeline = false;
   }
   int old_open() { ++opens; return 0; }
   uint64_t old_completed() const { return UINT64_MAX; }
   int old_signal(uint64_t) const { return int(0x80070057u); }
   int queue_signal(uint64_t v) { if (!published) return -1; value = v; return 0; }
   uint64_t vk_completed() const { return imported ? value : 0; }
   void vk_signal(uint64_t v) { value = v; }
   int queue_wait(uint64_t v) const { return published && value >= v ? 0 : -1; }
};

int main(int argc, char **argv)
{
   const bool old = argc == 2 && !std::strcmp(argv[1], "--old-r6-control");
   if (argc != 1 && !old) return 2;
   Mock bad;
   check(bad.old_open() == 0, "r6 OpenSharedHandle succeeded");
   check(bad.old_completed() == UINT64_MAX && bad.vk_completed() == 0,
         "r6 disagrees on completed value without device removal");
   check(bad.old_signal(1) == int(0x80070057u), "r6 queue Signal E_INVALIDARG");
   if (old) {
      // An HRESULT-only admission, as in the old initializer, fails the contract.
      check(bad.old_completed() == 0, "old initializer admitted an unusable dependency");
      check(bad.old_signal(1) == 0, "old initialized pair cannot signal");
   } else {
      Mock ok;
      check(wsi_win32_create_shared_fence(ok) == 0, "new pair initializes");
      check(ok.published == 1 && ok.opens == 0, "runtime owns fence; no bad open path");
      check(ok.closed_handle == 1 && ok.made_handle == 1, "handle closes exactly once");
      check(!ok.released_fence && !ok.destroyed_timeline, "caller retains both payload owners");
      for (uint64_t v = 1; v != 9; v += 2) {
         check(ok.queue_signal(v) == 0 && ok.vk_completed() == v, "D3D12 to Vulkan timeline");
         ok.vk_signal(v + 1);
         check(ok.queue_wait(v + 1) == 0, "Vulkan to D3D12 timeline");
      }
      for (unsigned stage = 1; stage <= 7; ++stage) {
         Mock m; m.fail_step = stage;
         check(wsi_win32_create_shared_fence(m) == -int(stage), "first error preserved");
         check(!m.published, "partial pair never published");
         check(m.made_fence == m.released_fence, "failure releases fence");
         check(m.made_timeline == m.destroyed_timeline, "failure destroys timeline");
         check(m.made_handle == m.closed_handle, "failure closes handle");
      }
      for (unsigned view = 0; view != 2; ++view) {
         Mock m; m.bad_created_value = view == 0; m.bad_imported_value = view == 1;
         check(wsi_win32_create_shared_fence(m) < 0 && !m.published, "bad initial value rejected");
         check(m.made_fence == m.released_fence && m.made_timeline == m.destroyed_timeline &&
               m.made_handle == m.closed_handle, "bad value cleanup is balanced");
      }
   }
   std::printf("shared fence: %u checks, %u failures\n", checks, failures);
   return failures ? 1 : 0;
}
