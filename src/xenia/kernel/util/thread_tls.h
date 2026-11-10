/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia contributors. All rights reserved.                    *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */
#ifndef XENIA_KERNEL_UTIL_THREAD_TLS_H_
#define XENIA_KERNEL_UTIL_THREAD_TLS_H_

#include <cstdint>
#include <optional>

namespace xe {
class Memory;
namespace kernel::internal {

// Host thread storage metadata, not a native TLS slot or Task authority.
struct ThreadTlsStorage {
  uint32_t static_address;
  uint32_t dynamic_address;
  uint32_t total_size;
};

std::optional<uint32_t> CheckedThreadTlsSlot(ThreadTlsStorage,
                                             uint32_t slot) noexcept;
bool ReadThreadTlsSlot(Memory&, ThreadTlsStorage, uint32_t slot,
                       uint32_t* value);
bool WriteThreadTlsSlot(Memory&, ThreadTlsStorage, uint32_t slot,
                        uint32_t value);

}  // namespace kernel::internal
}  // namespace xe
#endif  // XENIA_KERNEL_UTIL_THREAD_TLS_H_
