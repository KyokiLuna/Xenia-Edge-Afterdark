/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia contributors. All rights reserved.                    *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */
#ifndef XENIA_KERNEL_NATIVE_ALLOCATION_COHORT_H_
#define XENIA_KERNEL_NATIVE_ALLOCATION_COHORT_H_
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include "xenia/base/physical_range_leases.h"
namespace xe::kernel {
// Host metadata only. Creating a cohort does not install kernel attribution,
// prove Task identity, tag allocations, stop CPU writers or grant GPU work.
// NativeSystemAllocationAuthority alone connects it to actual caller scopes.
class NativeAllocationCohort {
 public:
  explicit NativeAllocationCohort(memory::PhysicalAllocationOwner owner)
      : owner_(owner) {}
  const memory::PhysicalAllocationOwner& owner() const { return owner_; }
  bool accepting() const {
    return owner_.valid() && !closed_.load(std::memory_order_acquire);
  }
  void Close() noexcept { closed_.store(true, std::memory_order_release); }
  uint64_t ReserveSourceSerial() {
    if (!accepting()) {
      return 0;
    }
    auto value = next_source_.load(std::memory_order_relaxed);
    while (value && value != UINT64_MAX) {
      if (next_source_.compare_exchange_weak(value, value + 1,
                                             std::memory_order_relaxed)) {
        return value;
      }
    }
    Close();
    return 0;
  }
  // One update through Source retirement across ALL delegated Sessions. A
  // frozen/uncertain Session retains the token; replacement Task cannot write.
  // This is a host admission gate, not proof of all guest-writer exclusion.
  std::shared_ptr<void> AcquireWriter() {
    std::lock_guard lock(mutex_);
    if (!accepting() || !writer_.expired()) {
      return {};
    }
    auto token = std::make_shared<uint8_t>(0);
    writer_ = token;
    return token;
  }

 private:
  const memory::PhysicalAllocationOwner owner_;
  std::atomic<bool> closed_{false};
  std::atomic<uint64_t> next_source_{1};
  std::mutex mutex_;
  std::weak_ptr<void> writer_;
};
}  // namespace xe::kernel
#endif  // XENIA_KERNEL_NATIVE_ALLOCATION_COHORT_H_
