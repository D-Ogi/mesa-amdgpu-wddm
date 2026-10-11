/* SPDX-License-Identifier: MIT */
#ifndef WSI_WIN32_SHARED_FENCE_H
#define WSI_WIN32_SHARED_FENCE_H

/* Transaction for a presenter-owned shared timeline. Ops implements the API
 * calls and owns temporary objects until publish. No queue work is submitted.
 * Success from opening an object is insufficient: both views must read the
 * initial value before the pair can become a dependency (BD-105).
 *
 * The D3D12 runtime creates the fence. Vulkan permanently imports its D3D12_FENCE
 * handle. Import does not transfer ownership of the application's NT handle.
 */
template <typename Ops>
static auto
wsi_win32_create_shared_fence(Ops &ops) -> decltype(ops.create_fence())
{
   auto result = ops.create_fence();
   if (result == 0) result = ops.check_fence();
   if (result == 0) result = ops.share_fence();
   if (result == 0) result = ops.create_timeline();
   if (result == 0) result = ops.import_fence();
   ops.close_handle();
   if (result == 0) result = ops.check_timeline();
   if (result == 0) ops.publish();
   ops.discard();
   return result;
}

#endif
