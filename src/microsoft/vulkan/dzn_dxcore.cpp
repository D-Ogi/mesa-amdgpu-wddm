/*
 * Copyright © Microsoft Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include "dzn_dxcore.h"
#include <directx/dxcore.h>
#include <dxguids/dxguids.h>

void
dzn_query_memory_info(IUnknown* unk, d3d12_memory_info* output){
   IDXCoreAdapter* adapter = NULL;
   HRESULT hr = unk->QueryInterface(
      __uuidof(IDXCoreAdapter),
      reinterpret_cast<void**>(&adapter));

   if(SUCCEEDED(hr)){

      DXCoreAdapterMemoryBudget local_info, nonlocal_info;
      DXCoreAdapterMemoryBudgetNodeSegmentGroup local_node_segment = { 0, DXCoreSegmentGroup::Local };
      DXCoreAdapterMemoryBudgetNodeSegmentGroup nonlocal_node_segment = { 0, DXCoreSegmentGroup::NonLocal };
      adapter->QueryState(DXCoreAdapterState::AdapterMemoryBudget, &local_node_segment, &local_info);
      adapter->QueryState(DXCoreAdapterState::AdapterMemoryBudget, &nonlocal_node_segment, &nonlocal_info);

      output->budget_local = local_info.budget;
      output->budget_nonlocal = nonlocal_info.budget;
      output->budget = local_info.budget + nonlocal_info.budget;
      output->usage_local = local_info.currentUsage;
      output->usage_nonlocal = nonlocal_info.currentUsage;
      output->usage = local_info.currentUsage + nonlocal_info.currentUsage;
      return;
   }
}