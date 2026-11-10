/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia contributors. All rights reserved.                    *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */
#ifndef XENIA_KERNEL_NATIVE_SYSTEM_TITLE_HEAP_DATA_H_
#define XENIA_KERNEL_NATIVE_SYSTEM_TITLE_HEAP_DATA_H_
#include <array>
#include <cstdint>
namespace xe {
class Memory;
namespace kernel {
// Host metadata for one already prepared ordinary executable. This is not
// native Task, allocator, caller-context or guest execution permission.
struct NativeSystemPreparedTitle {
  uint64_t foreground_generation = 0;
  uint32_t hmodule = 0;
  uint64_t load_generation = 0;
  uint32_t main_thread_id = 0;
  bool valid() const {
    return foreground_generation && hmodule && load_generation &&
           main_thread_id;
  }
  bool operator==(const NativeSystemPreparedTitle&) const = default;
};
bool SameNativeSystemPreparedTitle(const NativeSystemPreparedTitle& expected,
                                   const NativeSystemPreparedTitle& actual);
// Exact supplied17559 data-only observation. No pointers are dereferenced from
// a sample, no heap is created or destroyed, and no readiness is granted.
struct NativeSystemTitleHeapData {
  enum class Status { kRead, kRootUnreadable, kHeapUnreadable };
  Status status = Status::kRootUnreadable;
  uint32_t root_hmodule = 0;
  std::array<uint32_t, 6> words{};
};
NativeSystemTitleHeapData ReadNativeSystemTitleHeapData(Memory& memory);
bool NativeSystemTitleHeapIsPristine(const NativeSystemTitleHeapData& data,
                                     uint32_t root_hmodule);
// Used only AFTER the fixed native initializer returned0. Independently checks
// the actual backing extent; this data helper is not a substitute for that
// call, actual prepared Title identity or its retained module/Kernel lifetime.
bool NativeSystemTitleHeapHasBacking(Memory& memory,
                                     const NativeSystemTitleHeapData& data,
                                     uint32_t root_hmodule,
                                     uint32_t expected_reserve_bytes);
}  // namespace kernel
}  // namespace xe
#endif
