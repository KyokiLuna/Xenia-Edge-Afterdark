/**
 * Xenia : Xbox 360 Emulator Research Project
 * Released under the BSD license - see LICENSE in the root for details.
 */
#ifndef XENIA_KERNEL_UTIL_NATIVE_WAIT_SNAPSHOT_H_
#define XENIA_KERNEL_UTIL_NATIVE_WAIT_SNAPSHOT_H_
#include <array>
#include <cstdint>
#include <cstring>
#include <span>
#include "xenia/base/mutex.h"
#include "xenia/memory.h"
namespace xe::kernel::internal {
// Reads current RAM backing ONLY. It never runs GPU data providers, waits for
// guest work, changes mappings/watches, or grants object/allocation authority.
// Caller-visible bytes may differ until a real provider synchronizes them.
// This is not an atomic snapshot across concurrent guest writers.
// Failure preserves output. At most256 bytes; complete metadata admission
// precedes any source read. Native callers may already hold recursive GCRC.
inline bool ReadNativeWaitSnapshot(Memory& memory, uint32_t address,
                                   std::span<uint8_t> output) {
  if (output.empty()) {
    return true;
  }
  if (!address || output.size() > 256 ||
      uint64_t(address) + output.size() > UINT64_C(0x100000000)) {
    return false;
  }
  auto lock = global_critical_region::AcquireDirect();
  auto* heap = memory.LookupHeap(address);
  if (!heap ||
      memory.LookupHeap(uint32_t(uint64_t(address) + output.size() - 1)) !=
          heap ||
      memory.QueryRangeAccess(address, uint32_t(output.size())) ==
          memory::PageAccess::kNoAccess) {
    return false;
  }
  if (heap->heap_type() == HeapType::kGuestPhysical) {
    auto* physical = static_cast<PhysicalHeap*>(heap);
    const uint32_t first = physical->GetPhysicalAddress(address);
    const uint32_t last = physical->GetPhysicalAddress(
        uint32_t(uint64_t(address) + output.size() - 1));
    if (first >= 0x20000000 || output.size() > 0x20000000 - first ||
        uint64_t(first) + output.size() - 1 != last ||
        physical->parent_heap()->QueryRangeAccess(
            first, uint32_t(uint64_t(first) + output.size() - 1)) ==
            memory::PageAccess::kNoAccess) {
      return false;
    }
  }
  std::array<uint8_t, 256> owned;
  std::memcpy(owned.data(), memory.TranslateForRead(address), output.size());
  std::memcpy(output.data(), owned.data(), output.size());
  return true;
}
}  // namespace xe::kernel::internal
#endif
