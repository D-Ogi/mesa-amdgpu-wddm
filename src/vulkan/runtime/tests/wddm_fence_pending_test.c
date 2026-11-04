/* SPDX-License-Identifier: MIT
 *
 * BD-200 host check of VK_SYNC_WAIT_PENDING on the WDDM monitored fence
 * (vk_wddm2_monitored_fence.c). run_wddm_fence_pending.py builds one
 * translation unit from this file, the production functions of the CPU wait
 * and an inert kernel boundary, and runs it with real host threads.
 *
 * A wait for pending is a wait for the submission of a signal, not for its
 * completion (vk_sync.h, VK_SYNC_FEATURE_TIMELINE). The submit thread and the
 * zero-timeout check of vk_queue_submit use it. The code before BD-200 waited
 * for the fence value, that is for the GPU, and Quake II RTX lost 11 % in the
 * on-demand submit mode (revert 3b660ba0).
 *
 * In this model the GPU never completes anything: the fence values stay where
 * a case puts them. A signal handed to the kernel is a call of
 * vk_wddm2_monitored_fence_note_submitted (the winsys) or of the production
 * CPU signal.
 *
 * Negative control: --baseline 3b660ba0 builds the same cases against the
 * fence source before the change. Its wait ignores the flag, so the pending
 * cases fail and the runner returns 1.
 */

static uint64_t
now_ms(void)
{
   return os_time_get_nano() / 1000000ull;
}

struct waiter {
   struct vk_device *device;
   struct vk_sync_wait waits[2];
   uint32_t count;
   enum vk_sync_wait_flags flags;
   uint64_t abs_timeout_ns;
   volatile LONG done;
   VkResult result;
   uint64_t end_ms;
};

static int
waiter_main(void *arg)
{
   struct waiter *w = arg;
   w->result = vk_wddm2_monitored_fence_wait_many(w->device, w->count, w->waits, w->flags, w->abs_timeout_ns);
   w->end_ms = now_ms();
   InterlockedExchange(&w->done, 1);
   return 0;
}

static bool
wait_done(struct waiter *w, unsigned ms)
{
   /* Sleep(1) can take a whole timer tick (15.6 ms): count time, not calls. */
   const uint64_t end = now_ms() + ms;
   do {
      if (InterlockedCompareExchange(&w->done, 0, 0))
         return true;
      Sleep(1);
   } while (now_ms() < end);
   return InterlockedCompareExchange(&w->done, 0, 0) != 0;
}

/* A wait that a case could not end (the baseline ignores the flag) ends at
 * completion, so the case can join its thread. */
static void
release(struct waiter *w)
{
   if (wait_done(w, 1))
      return;
   host_lost = false;
   for (uint32_t i = 0; i < w->count; i++)
      p_atomic_set(container_of(w->waits[i].sync, struct vk_wddm2_monitored_fence, base)->value_map,
                   w->waits[i].wait_value);
   wait_done(w, 4000);
}

static void
start(thrd_t *t, struct waiter *w)
{
   CHECK(thrd_create(t, waiter_main, w) == thrd_success);
}

static void
test_zero_timeout(void)
{
   group = "zero-timeout";
   reset_mock();
   struct mock_fence *f = new_fence(0, 0);
   struct vk_sync_wait w = {&f->fence.base, 0, 5};

   vk_wddm2_monitored_fence_note_submitted(&f->fence, 5);
   CHECK(vk_wddm2_monitored_fence_wait_many(&device, 1, &w, VK_SYNC_WAIT_PENDING, 0) == VK_SUCCESS);
   CHECK(vk_wddm2_monitored_fence_wait_many(&device, 1, &w, VK_SYNC_WAIT_COMPLETE, 0) == VK_TIMEOUT);
   CHECK(kernel_waits == 0);

   w.wait_value = 6;
   CHECK(vk_wddm2_monitored_fence_wait_many(&device, 1, &w, VK_SYNC_WAIT_PENDING, 0) == VK_TIMEOUT);
   CHECK(kernel_waits == 0);

   /* A completed value is also pending. */
   *f->fence.value_map = 6;
   CHECK(vk_wddm2_monitored_fence_wait_many(&device, 1, &w, VK_SYNC_WAIT_PENDING, 0) == VK_SUCCESS);
}

static void
test_cpu_signal_is_pending(void)
{
   group = "cpu-signal";
   reset_mock();
   struct mock_fence *f = new_fence(0, 0);
   struct vk_sync_wait w = {&f->fence.base, 0, 3};

   CHECK(vk_wddm2_monitored_fence_signal(&device, &f->fence.base, 3) == VK_SUCCESS);
   /* The kernel mock keeps the CPU signal queued, as behind a GPU wait. */
   CHECK(*f->fence.value_map == 0);
   CHECK(vk_wddm2_monitored_fence_wait_many(&device, 1, &w, VK_SYNC_WAIT_PENDING, 0) == VK_SUCCESS);
}

static void
test_max_kept(void)
{
   group = "max";
   reset_mock();
   struct mock_fence *f = new_fence(0, 0);
   vk_wddm2_monitored_fence_note_submitted(&f->fence, 7);
   vk_wddm2_monitored_fence_note_submitted(&f->fence, 5);
   struct vk_sync_wait w = {&f->fence.base, 0, 7};
   CHECK(vk_wddm2_monitored_fence_wait_many(&device, 1, &w, VK_SYNC_WAIT_PENDING, 0) == VK_SUCCESS);
}

/* A blocked wait for pending ends at the submission, while the GPU has done nothing. */
static void
test_blocking_wake(void)
{
   group = "blocking-wake";
   reset_mock();
   struct mock_fence *a = new_fence(0, 0), *b = new_fence(0, 0);
   struct waiter w = {&device, {{&a->fence.base, 0, 5}}, 1, VK_SYNC_WAIT_PENDING, UINT64_MAX};
   thrd_t t;
   start(&t, &w);
   Sleep(50);
   CHECK(!InterlockedCompareExchange(&w.done, 0, 0));

   /* A submission for another fence wakes the wait but does not end it. */
   vk_wddm2_monitored_fence_note_submitted(&b->fence, 9);
   Sleep(50);
   CHECK(!InterlockedCompareExchange(&w.done, 0, 0));

   const uint64_t submit_ms = now_ms();
   vk_wddm2_monitored_fence_note_submitted(&a->fence, 5);
   CHECK(wait_done(&w, 2000));
   CHECK(w.result == VK_SUCCESS);
   CHECK(w.end_ms - submit_ms < 200);
   CHECK(*a->fence.value_map == 0);
   release(&w);
   thrd_join(t, NULL);
}

static void
test_wait_any(void)
{
   group = "wait-any";
   reset_mock();
   struct mock_fence *a = new_fence(0, 0), *b = new_fence(0, 0);
   struct waiter w = {&device, {{&a->fence.base, 0, 4}, {&b->fence.base, 0, 4}}, 2,
                      VK_SYNC_WAIT_PENDING | VK_SYNC_WAIT_ANY, UINT64_MAX};
   thrd_t t;
   start(&t, &w);
   Sleep(30);
   vk_wddm2_monitored_fence_note_submitted(&b->fence, 4);
   CHECK(wait_done(&w, 2000));
   CHECK(w.result == VK_SUCCESS);
   release(&w);
   thrd_join(t, NULL);

   /* Without ANY both have to be pending. */
   struct vk_sync_wait both[2] = {{&a->fence.base, 0, 4}, {&b->fence.base, 0, 4}};
   CHECK(vk_wddm2_monitored_fence_wait_many(&device, 2, both, VK_SYNC_WAIT_PENDING, 0) == VK_TIMEOUT);
   vk_wddm2_monitored_fence_note_submitted(&a->fence, 4);
   CHECK(vk_wddm2_monitored_fence_wait_many(&device, 2, both, VK_SYNC_WAIT_PENDING, 0) == VK_SUCCESS);
}

static void
test_deadline(void)
{
   group = "deadline";
   reset_mock();
   struct mock_fence *f = new_fence(0, 0);
   struct vk_sync_wait w = {&f->fence.base, 0, 2};
   const uint64_t begin = now_ms();
   VkResult r = vk_wddm2_monitored_fence_wait_many(&device, 1, &w, VK_SYNC_WAIT_PENDING,
                                                   os_time_get_nano() + 100000000ull);
   const uint64_t took = now_ms() - begin;
   CHECK(r == VK_TIMEOUT);
   CHECK(took >= 90 && took < 1000);
}

/* A payload that another process can signal: this process cannot see that
 * submission, so the wait is for completion, as before. */
static void
test_shared_waits_for_completion(void)
{
   group = "shared";
   reset_mock();
   struct mock_fence *f = new_fence(VK_SYNC_IS_SHAREABLE, 0);
   struct vk_sync_wait w = {&f->fence.base, 0, 5};
   vk_wddm2_monitored_fence_note_submitted(&f->fence, 5);
   CHECK(vk_wddm2_monitored_fence_wait_many(&device, 1, &w, VK_SYNC_WAIT_PENDING, 0) == VK_TIMEOUT);
   CHECK(vk_wddm2_monitored_fence_wait_many(&device, 1, &w, VK_SYNC_WAIT_PENDING,
                                            os_time_get_nano() + 30000000ull) == VK_TIMEOUT);
   CHECK(kernel_waits == 1);
   *f->fence.value_map = 5;
   CHECK(vk_wddm2_monitored_fence_wait_many(&device, 1, &w, VK_SYNC_WAIT_PENDING, 0) == VK_SUCCESS);

   struct mock_fence *g = new_fence(0, 1 /* imported */);
   struct vk_sync_wait wg = {&g->fence.base, 0, 5};
   vk_wddm2_monitored_fence_note_submitted(&g->fence, 5);
   CHECK(vk_wddm2_monitored_fence_wait_many(&device, 1, &wg, VK_SYNC_WAIT_PENDING, 0) == VK_TIMEOUT);
}

/* BD-179 holds for the pending wait too: after each slice the wait reads the
 * execution state of the device. The kernel device is used here, because on a
 * hosted device the fence poll itself already asks the host. */
static void
test_lost_in_slice(void)
{
   group = "lost-slice";
   reset_mock();
   vk_wddm2_monitored_fence_wait_slice_ns = 20000000ull;
   struct mock_fence *f = new_fence(0, 0);
   struct waiter w = {&kdevice, {{&f->fence.base, 0, 5}}, 1, VK_SYNC_WAIT_PENDING, UINT64_MAX};
   thrd_t t;
   start(&t, &w);
   Sleep(60);
   CHECK(!InterlockedCompareExchange(&w.done, 0, 0));
   host_lost = true;
   CHECK(wait_done(&w, 2000));
   CHECK(w.result == VK_ERROR_DEVICE_LOST);
   release(&w);
   thrd_join(t, NULL);
   vk_wddm2_monitored_fence_wait_slice_ns = 1000000000ull;
}

struct traffic {
   struct vk_wddm2_monitored_fence *fence;
   volatile LONG stop;
};

static int
traffic_main(void *arg)
{
   struct traffic *t = arg;
   for (uint64_t v = 1; !InterlockedCompareExchange(&t->stop, 0, 0); v++) {
      vk_wddm2_monitored_fence_note_submitted(t->fence, v);
      Sleep(2);
   }
   return 0;
}

/* Submissions to other fences wake the wait more often than once per slice.
 * The liveness check still runs once per slice. */
static void
test_lost_with_traffic(void)
{
   group = "lost-traffic";
   reset_mock();
   vk_wddm2_monitored_fence_wait_slice_ns = 20000000ull;
   struct mock_fence *f = new_fence(0, 0), *other = new_fence(0, 0);
   struct traffic tr = {&other->fence, 0};
   thrd_t tt, t;
   CHECK(thrd_create(&tt, traffic_main, &tr) == thrd_success);
   struct waiter w = {&kdevice, {{&f->fence.base, 0, 5}}, 1, VK_SYNC_WAIT_PENDING, UINT64_MAX};
   start(&t, &w);
   Sleep(60);
   CHECK(!InterlockedCompareExchange(&w.done, 0, 0));
   host_lost = true;
   CHECK(wait_done(&w, 2000));
   CHECK(w.result == VK_ERROR_DEVICE_LOST);
   release(&w);
   thrd_join(t, NULL);
   InterlockedExchange(&tr.stop, 1);
   thrd_join(tt, NULL);
   vk_wddm2_monitored_fence_wait_slice_ns = 1000000000ull;
}

#ifdef HAVE_FENCE_MOVE
/* A move carries the submitted value with the payload. */
static void
test_move_carries_pending(void)
{
   group = "move";
   reset_mock();
   struct mock_fence *a = new_fence(0, 0), *b = new_fence(0, 0);
   vk_wddm2_monitored_fence_note_submitted(&a->fence, 6);
   CHECK(vk_wddm2_monitored_fence_move(&device, &b->fence.base, &a->fence.base) == VK_SUCCESS);
   struct vk_sync_wait wa = {&a->fence.base, 0, 6}, wb = {&b->fence.base, 0, 6};
   CHECK(vk_wddm2_monitored_fence_wait_many(&device, 1, &wb, VK_SYNC_WAIT_PENDING, 0) == VK_SUCCESS);
   CHECK(vk_wddm2_monitored_fence_wait_many(&device, 1, &wa, VK_SYNC_WAIT_PENDING, 0) == VK_TIMEOUT);
}
#endif

int
main(void)
{
   setvbuf(stdout, NULL, _IONBF, 0);
   test_zero_timeout();
   test_cpu_signal_is_pending();
   test_max_kept();
   test_blocking_wake();
   test_wait_any();
   test_deadline();
   test_shared_waits_for_completion();
   test_lost_in_slice();
   test_lost_with_traffic();
#ifdef HAVE_FENCE_MOVE
   test_move_carries_pending();
#endif
   printf("%ld checks, %ld failures\n", (long)checks, (long)failures);
   return failures != 0;
}
