/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia contributors. All rights reserved.                    *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */
#ifndef XENIA_BASE_PHYSICAL_RANGE_LEASES_H_
#define XENIA_BASE_PHYSICAL_RANGE_LEASES_H_

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace xe {
class Memory;
namespace memory {

// Opaque host admission identity. No process type, guest pointer or allocation
// flag can manufacture the authority to tag an allocation for another owner.
struct PhysicalAllocationOwner {
  uint64_t kernel_epoch = 0;
  uint64_t module_generation = 0;
  uint64_t session_id = 0;
  uint32_t module_hmodule = 0;
  bool valid() const {
    return kernel_epoch && module_generation && session_id && module_hmodule;
  }
  bool operator==(const PhysicalAllocationOwner&) const = default;
};

struct PhysicalRange {
  uint32_t address = 0;
  uint32_t length = 0;
};

enum class PhysicalLeaseStatus {
  kSuccess,
  kInvalidOwner,
  kInvalidRange,
  kUnowned,
  kWrongOwner,
  kNotCommitted,
  kPinned,
  kStaleAllocation,
  kAllocationFailed,
  kStopped,
  kLimitExceeded,
};

struct PhysicalAllocationIdentity {
  uint64_t allocation_id = 0;
  PhysicalAllocationOwner owner;
  uint32_t physical_address = 0;
  uint32_t length = 0;
  uint32_t virtual_address = 0;
};

class PhysicalRangeLeaseRegistry;
struct PhysicalRangeLeaseState;

// Only the Memory allocator can issue this token after checking real backing.
// Holds complete parent allocations (including arenas), not just requested
// subranges. Prevents allocator mutations, not CPU writes or GPU execution.
// Destruction performs no heap mutation, callbacks, guest code or GPU waits.
class PhysicalRangePin {
 public:
  ~PhysicalRangePin();
  PhysicalRangePin(const PhysicalRangePin&) = delete;
  PhysicalRangePin& operator=(const PhysicalRangePin&) = delete;
  bool valid() const;
  const std::vector<PhysicalAllocationIdentity>& allocations() const {
    return allocations_;
  }

 private:
  friend class PhysicalRangeLeaseRegistry;
  PhysicalRangePin(std::shared_ptr<PhysicalRangeLeaseState> state,
                   std::vector<PhysicalAllocationIdentity> allocations);
  std::shared_ptr<PhysicalRangeLeaseState> state_;
  const std::vector<PhysicalAllocationIdentity> allocations_;
  bool armed_ = false;
};

struct PhysicalRangePinResult {
  PhysicalLeaseStatus status = PhysicalLeaseStatus::kInvalidRange;
  uint32_t failed_range = UINT32_MAX;
  std::shared_ptr<const PhysicalRangePin> pin;
  explicit operator bool() const {
    return status == PhysicalLeaseStatus::kSuccess && bool(pin);
  }
};

// Internal Memory authority. All *Locked operations require the existing
// global critical region; no registry mutex is nested around heap callbacks.
class PhysicalRangeLeaseRegistry {
 public:
  PhysicalRangeLeaseRegistry();
  ~PhysicalRangeLeaseRegistry();
  PhysicalRangeLeaseRegistry(const PhysicalRangeLeaseRegistry&) = delete;
  PhysicalRangeLeaseRegistry& operator=(const PhysicalRangeLeaseRegistry&) =
      delete;

 private:
  friend class xe::Memory;
  uint64_t RegisterLocked(PhysicalAllocationIdentity identity);
  PhysicalRangePinResult PinLocked(const PhysicalAllocationOwner& owner,
                                   std::span<const PhysicalRange> ranges);
  PhysicalLeaseStatus FindLocked(const PhysicalAllocationOwner& owner,
                                 uint64_t allocation_id,
                                 PhysicalAllocationIdentity* identity) const;
  bool LookupLocked(uint32_t physical_address,
                    PhysicalAllocationIdentity* identity) const;
  bool HasPinsLocked(uint32_t address, uint32_t length) const;
  void ForgetLocked(uint32_t address, uint32_t length);
  bool ClearLocked();
  void StopLocked();
  std::shared_ptr<PhysicalRangeLeaseState> state_;
};

}  // namespace memory
}  // namespace xe
#endif  // XENIA_BASE_PHYSICAL_RANGE_LEASES_H_
