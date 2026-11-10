/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia contributors. All rights reserved.                    *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */
#include "xenia/kernel/util/thread_tls.h"

#include "xenia/base/byte_order.h"
#include "xenia/base/mutex.h"
#include "xenia/memory.h"

namespace xe::kernel::internal {

std::optional<uint32_t> CheckedThreadTlsSlot(ThreadTlsStorage storage,
                                             uint32_t slot) noexcept {
  if (!storage.static_address || !storage.dynamic_address ||
      !storage.total_size || storage.dynamic_address < storage.static_address) {
    return {};
  }
  const uint64_t extension =
      uint64_t(storage.dynamic_address) - storage.static_address;
  if (extension >= storage.total_size ||
      uint64_t(storage.static_address) + storage.total_size > 0x100000000ull) {
    return {};
  }
  const uint64_t offset = uint64_t(slot) * 4;
  if (offset + 4 > uint64_t(storage.total_size) - extension) {
    return {};
  }
  const uint64_t address = uint64_t(storage.dynamic_address) + offset;
  if (address + 4 > 0x100000000ull) {
    return {};
  }
  return uint32_t(address);
}

bool ReadThreadTlsSlot(Memory& memory, ThreadTlsStorage storage, uint32_t slot,
                       uint32_t* value) {
  if (!value) {
    return false;
  }
  const auto address = CheckedThreadTlsSlot(storage, slot);
  if (!address) {
    return false;
  }
  auto guard = global_critical_region::AcquireDirect();
  if (memory.QueryRangeAccess(*address, 4) == memory::PageAccess::kNoAccess) {
    return false;
  }
  const uint32_t sampled =
      xe::load_and_swap<uint32_t>(memory.TranslateVirtual(*address));
  *value = sampled;
  return true;
}

bool WriteThreadTlsSlot(Memory& memory, ThreadTlsStorage storage, uint32_t slot,
                        uint32_t value) {
  const auto address = CheckedThreadTlsSlot(storage, slot);
  if (!address) {
    return false;
  }
  auto guard = global_critical_region::AcquireDirect();
  if (memory.QueryRangeAccess(*address, 4) != memory::PageAccess::kReadWrite) {
    return false;
  }
  xe::store_and_swap<uint32_t>(memory.TranslateVirtual(*address), value);
  return true;
}

}  // namespace xe::kernel::internal
