/**
 * Xenia : Xbox 360 Emulator Research Project
 * Released under the BSD license - see LICENSE in the root for details.
 */
#include "xenia/kernel/util/guest_io_buffer.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include "xenia/base/memory.h"
#include "xenia/base/mutex.h"
#include "xenia/memory.h"

namespace xe::kernel::util {
namespace {
bool Valid(Memory& memory, uint32_t address, size_t length, bool writable) {
  if (!length) {
    return true;
  }
  if (!address || length > UINT32_MAX ||
      uint64_t(address) + length > 0x100000000ull) {
    return false;
  }
  const auto access = memory.QueryRangeAccess(address, uint32_t(length));
  return writable ? access == memory::PageAccess::kReadWrite
                  : access != memory::PageAccess::kNoAccess;
}
PhysicalHeap* Physical(Memory& memory, uint32_t address) {
  auto* heap = memory.LookupHeap(address);
  return heap && heap->heap_type() == HeapType::kGuestPhysical
             ? static_cast<PhysicalHeap*>(heap)
             : nullptr;
}
struct BackingSpan {
  PhysicalHeap* physical = nullptr;
  uint64_t begin = 0, end = 0;
};
X_STATUS CheckWrites(Memory& memory, std::span<const GuestIoWrite> writes,
                     std::array<BackingSpan, 2>& backing) {
  for (size_t i = 0; i < writes.size(); ++i) {
    const auto& write = writes[i];
    if (!Valid(memory, write.address, write.bytes.size(), true)) {
      return X_STATUS_ACCESS_VIOLATION;
    }
    auto* physical = Physical(memory, write.address);
    const uint64_t begin =
        physical ? physical->GetPhysicalAddress(write.address) : write.address;
    backing[i] = {physical, begin, begin + write.bytes.size()};
    for (size_t j = 0; j < i; ++j) {
      if (!write.bytes.empty() && !writes[j].bytes.empty() &&
          bool(physical) == bool(backing[j].physical) &&
          begin < backing[j].end && backing[j].begin < backing[i].end) {
        return X_STATUS_INVALID_PARAMETER;
      }
    }
  }
  return X_STATUS_SUCCESS;
}
template <typename Store>
X_STATUS WriteBatch(Memory& memory, std::span<const GuestIoWrite> writes,
                    Store store) {
  if (writes.size() > 2) {
    return X_STATUS_INVALID_PARAMETER;
  }
  if (std::ranges::all_of(writes,
                          [](const auto& w) { return w.bytes.empty(); })) {
    return X_STATUS_SUCCESS;
  }
  if (global_critical_region::is_held_by_current_thread()) {
    return X_STATUS_INVALID_DEVICE_REQUEST;
  }
  auto lock = global_critical_region::AcquireDirect();
  std::array<BackingSpan, 2> backing{};
  auto status = CheckWrites(memory, writes, backing);
  if (XFAILED(status)) {
    return status;
  }
  for (size_t i = 0; i < writes.size(); ++i) {
    if (backing[i].physical && !writes[i].bytes.empty()) {
      auto* physical = backing[i].physical;
      lock.unlock();
      physical->ProvideReadWatchedEdgePages(writes[i].address,
                                            uint32_t(writes[i].bytes.size()));
      lock.lock();
      // A prior provider may revoke ANY destination, not just its own span.
      status = CheckWrites(memory, writes, backing);
      if (XFAILED(status)) {
        return status;
      }
    }
  }
  // All callbacks which may wait have finished. No mapping change can split
  // this publication while the one actual global region is held.
  status = CheckWrites(memory, writes, backing);
  if (XFAILED(status)) {
    return status;
  }
  for (size_t i = 0; i < writes.size(); ++i) {
    if (writes[i].bytes.empty()) {
      continue;
    }
    auto* output = backing[i].physical
                       ? memory.TranslatePhysical(uint32_t(backing[i].begin))
                       : memory.TranslateVirtual(writes[i].address);
    store(i, output);
    std::atomic_thread_fence(std::memory_order_release);
  }
  // Every byte is already committed before an invalidation callback can run.
  // Such observers may change mappings afterward; no rejected partial write
  // or borrowed translated destination crosses those calls.
  for (size_t i = 0; i < writes.size(); ++i) {
    if (backing[i].physical && !writes[i].bytes.empty()) {
      backing[i].physical->TriggerCallbacks(std::move(lock), writes[i].address,
                                            uint32_t(writes[i].bytes.size()),
                                            true, true, true, false, true);
      if (i + 1 != writes.size()) {
        lock = global_critical_region::AcquireDirect();
      }
    }
  }
  return X_STATUS_SUCCESS;
}

}  // namespace
bool GuestIoRangeValid(Memory& memory, uint32_t address, uint32_t length,
                       bool writable) {
  auto lock = global_critical_region::AcquireDirect();
  return Valid(memory, address, length, writable);
}
X_STATUS ReadGuestIoBuffer(Memory& memory, uint32_t address,
                           std::span<uint8_t> output) {
  if (output.empty()) {
    return X_STATUS_SUCCESS;
  }
  if (global_critical_region::is_held_by_current_thread()) {
    return X_STATUS_INVALID_DEVICE_REQUEST;
  }
  auto lock = global_critical_region::AcquireDirect();
  if (!Valid(memory, address, output.size(), false)) {
    return X_STATUS_ACCESS_VIOLATION;
  }
  if (auto* physical = Physical(memory, address)) {
    if (!internal::VisitGuestIoHostPages(
            address, uint32_t(output.size()), physical->heap_base(),
            physical->host_address_offset(), uint32_t(xe::memory::page_size()),
            [&](uint32_t page) {
              physical->ProvideReadWatchedPage(lock, page, false);
            })) {
      return X_STATUS_INVALID_PARAMETER;
    }
    if (!Valid(memory, address, output.size(), false)) {
      return X_STATUS_ACCESS_VIOLATION;
    }
  }
  std::memcpy(output.data(), memory.TranslateForRead(address), output.size());
  return X_STATUS_SUCCESS;
}
bool GuestIoRangesOverlap(Memory& memory, uint32_t first, uint32_t first_length,
                          uint32_t second, uint32_t second_length) {
  if (!first_length || !second_length) {
    return false;
  }
  auto lock = global_critical_region::AcquireDirect();
  auto* a = Physical(memory, first);
  auto* b = Physical(memory, second);
  if (bool(a) != bool(b)) {
    return false;
  }
  const uint64_t begin_a = a ? a->GetPhysicalAddress(first) : first;
  const uint64_t begin_b = b ? b->GetPhysicalAddress(second) : second;
  return begin_a < begin_b + second_length && begin_b < begin_a + first_length;
}
X_STATUS WriteGuestIoBuffers(Memory& memory,
                             std::span<const GuestIoWrite> writes) {
  return WriteBatch(memory, writes, [&](size_t i, uint8_t* output) {
    std::memcpy(output, writes[i].bytes.data(), writes[i].bytes.size());
  });
}
X_STATUS WriteGuestIoBuffer(Memory& memory, uint32_t address,
                            std::span<const uint8_t> input) {
  const GuestIoWrite write{address, input};
  return WriteGuestIoBuffers(memory, {&write, 1});
}
X_STATUS WriteGuestIoStatus(Memory& memory, uint32_t address, X_STATUS status,
                            uint32_t information) {
  const std::array<uint8_t, 8> shape{};
  const GuestIoWrite write{address, shape};
  return WriteBatch(memory, {&write, 1}, [&](size_t, uint8_t* output) {
    xe::store_and_swap<uint32_t>(output + 4, information);
    std::atomic_thread_fence(std::memory_order_release);
    xe::store_and_swap<uint32_t>(output, status);
  });
}
}  // namespace xe::kernel::util
