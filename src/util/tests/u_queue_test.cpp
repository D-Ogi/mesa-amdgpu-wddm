/* SPDX-License-Identifier: MIT
 *
 * util_queue_finish() on a queue that runs several threads, and the util_barrier contract it relies on:
 * util_barrier_wait() returns true to one thread of each round, and that thread may destroy the barrier at once,
 * while the threads it released are still leaving it.
 *
 * The mutex and condvar barrier that Windows, macOS and Haiku use returned true to every thread, and each of them
 * destroyed the barrier: on Windows the first one deleted a critical section that the others still had to re-enter,
 * and the next lock of it faulted. The disk cache's queue, which starts up to four threads, met that on every
 * disk_cache_destroy() after a burst of writes.
 */

#include <gtest/gtest.h>

#include <atomic>
#include <thread>
#include <vector>

#include "util/u_queue.h"
#include "util/u_thread.h"

namespace {

void
count_job(void *job, void *gdata, int thread_index)
{
   static_cast<std::atomic<unsigned> *>(job)->fetch_add(1);
}

} /* namespace */

TEST(UtilQueue, FinishWithSeveralThreads)
{
   struct util_queue queue;
   ASSERT_TRUE(util_queue_init(&queue, "test", 64, 4, 0, NULL));

   /* Threads start on demand: start all four, so that the finish barrier spans four. */
   util_queue_adjust_num_threads(&queue, 4, false);
   ASSERT_EQ(queue.num_threads, 4u);

   std::atomic<unsigned> done(0);
   for (unsigned round = 1; round <= 100; round++) {
      for (unsigned i = 0; i < 16; i++)
         util_queue_add_job(&queue, &done, NULL, count_job, NULL, 0);
      util_queue_finish(&queue);
      ASSERT_EQ(done.load(), round * 16);
   }

   util_queue_destroy(&queue);
}

TEST(UtilBarrier, OneSerialThreadPerRound)
{
   const unsigned num_threads = 4, num_rounds = 1000;
   util_barrier barrier;
   util_barrier_init(&barrier, num_threads);

   std::atomic<unsigned> serial(0);
   std::vector<std::thread> threads;
   for (unsigned i = 0; i < num_threads; i++) {
      threads.emplace_back([&] {
         for (unsigned round = 0; round < num_rounds; round++) {
            if (util_barrier_wait(&barrier))
               serial++;
         }
      });
   }
   for (std::thread &thread : threads)
      thread.join();

   util_barrier_destroy(&barrier);
   EXPECT_EQ(serial.load(), num_rounds);
}

TEST(UtilBarrier, SerialThreadDestroys)
{
   const unsigned num_threads = 4;
   for (unsigned round = 0; round < 200; round++) {
      util_barrier barrier;
      util_barrier_init(&barrier, num_threads);

      std::atomic<unsigned> destroyed(0);
      std::vector<std::thread> threads;
      for (unsigned i = 0; i < num_threads; i++) {
         threads.emplace_back([&] {
            if (util_barrier_wait(&barrier)) {
               util_barrier_destroy(&barrier);
               destroyed++;
            }
         });
      }
      for (std::thread &thread : threads)
         thread.join();

      ASSERT_EQ(destroyed.load(), 1u) << "round " << round;
   }
}
