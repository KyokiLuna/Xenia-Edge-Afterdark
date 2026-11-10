/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia contributors. All rights reserved.                    *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */
#include "xenia/kernel/native_system_title_heap_data.h"
#include "xenia/base/byte_order.h"
#include "xenia/base/mutex.h"
#include "xenia/memory.h"
namespace xe::kernel {
namespace {
constexpr uint32_t kRootSelf = 0x81AA3EE0, kHeap0 = 0x81AABFF8;
}
bool SameNativeSystemPreparedTitle(const NativeSystemPreparedTitle& expected,
                                   const NativeSystemPreparedTitle& actual) {
  return expected.valid() && actual.valid() && expected == actual;
}
NativeSystemTitleHeapData ReadNativeSystemTitleHeapData(Memory& memory) {
  auto lock = xe::global_critical_region::AcquireDirect();
  NativeSystemTitleHeapData result;
  if (memory.QueryRangeAccess(kRootSelf, 4) == memory::PageAccess::kNoAccess) {
    return result;
  }
  result.root_hmodule =
      xe::load_and_swap<uint32_t>(memory.TranslateVirtual(kRootSelf));
  if (memory.QueryRangeAccess(kHeap0, 24) == memory::PageAccess::kNoAccess) {
    result.status = NativeSystemTitleHeapData::Status::kHeapUnreadable;
    return result;
  }
  for (size_t i = 0; i < result.words.size(); ++i) {
    result.words[i] = xe::load_and_swap<uint32_t>(
        memory.TranslateVirtual(kHeap0 + uint32_t(i) * 4));
  }
  result.status = NativeSystemTitleHeapData::Status::kRead;
  return result;
}
bool NativeSystemTitleHeapIsPristine(const NativeSystemTitleHeapData& data,
                                     uint32_t root_hmodule) {
  return root_hmodule &&
         data.status == NativeSystemTitleHeapData::Status::kRead &&
         data.root_hmodule == root_hmodule &&
         data.words == std::array<uint32_t, 6>{0xCCCC, 0, 0, 0, 0, 0};
}
bool NativeSystemTitleHeapHasBacking(Memory& memory,
                                     const NativeSystemTitleHeapData& data,
                                     uint32_t root_hmodule,
                                     uint32_t expected_reserve_bytes) {
  auto lock = xe::global_critical_region::AcquireDirect();
  const auto& w = data.words;
  const uint64_t end = uint64_t(w[2]) + w[3];
  if (!root_hmodule ||
      data.status != NativeSystemTitleHeapData::Status::kRead ||
      data.root_hmodule != root_hmodule || w[0] || w[1] < 0x1000 ||
      (w[1] & 3) || !w[2] || (w[2] & 0xFFF) ||
      expected_reserve_bytes < 0x40000 || (expected_reserve_bytes & 0x3FFF) ||
      w[3] < expected_reserve_bytes ||
      uint64_t(w[3]) >
          ((uint64_t(expected_reserve_bytes) + 0xFFFF) & ~uint64_t(0xFFFF)) ||
      end > uint64_t(UINT32_MAX) + 1 || w[1] < w[2] || w[1] >= end ||
      !(w[4] & 0x80000000u) || w[5] != 1) {
    return false;
  }
  auto* heap = memory.LookupHeap(w[2]);
  HeapAllocationInfo info{};
  return heap && heap->heap_type() == HeapType::kGuestVirtual &&
         heap->QueryRegionInfo(w[2], &info) && info.allocation_base == w[2] &&
         info.allocation_size == w[3] &&
         (info.state & (kMemoryAllocationReserve | kMemoryAllocationCommit)) ==
             (kMemoryAllocationReserve | kMemoryAllocationCommit) &&
         memory.QueryRangeAccess(w[2], w[3]) == memory::PageAccess::kReadWrite;
}
}  // namespace xe::kernel
