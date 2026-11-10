/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia contributors. All rights reserved.                    *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */
#include "xenia/base/physical_range_leases.h"

#include <algorithm>
#include <atomic>
#include <map>
#include <new>
#include <set>
#include <utility>

#include "xenia/base/mutex.h"

namespace xe::memory {
namespace {
constexpr uint32_t kPhysicalSize = 0x20000000;
std::atomic<uint64_t> next_allocation_id{1};
uint64_t NextAllocationId() {
  auto value = next_allocation_id.load(std::memory_order_relaxed);
  while (value && value != UINT64_MAX) {
    if (next_allocation_id.compare_exchange_weak(value, value + 1,
                                                 std::memory_order_relaxed)) {
      return value;
    }
  }
  return 0;
}
bool ValidRange(uint32_t address, uint32_t length) {
  return length && address < kPhysicalSize && length <= kPhysicalSize - address;
}
bool Overlaps(const PhysicalAllocationIdentity& identity, uint32_t address,
              uint32_t length) {
  return uint64_t(identity.physical_address) + identity.length > address &&
         uint64_t(address) + length > identity.physical_address;
}
}  // namespace

struct PhysicalRangeLeaseState {
  struct Entry {
    PhysicalAllocationIdentity identity;
    size_t pins = 0;
  };
  std::atomic<bool> alive{true};
  size_t pin_count = 0;
  std::map<uint32_t, Entry> entries;
};

PhysicalRangePin::PhysicalRangePin(
    std::shared_ptr<PhysicalRangeLeaseState> state,
    std::vector<PhysicalAllocationIdentity> allocations)
    : state_(std::move(state)), allocations_(std::move(allocations)) {}

PhysicalRangePin::~PhysicalRangePin() {
  if (!armed_) {
    return;
  }
  auto lock = global_critical_region::Acquire();
  for (const auto& identity : allocations_) {
    auto entry = state_->entries.find(identity.physical_address);
    if (entry != state_->entries.end() &&
        entry->second.identity.allocation_id == identity.allocation_id &&
        entry->second.pins) {
      --entry->second.pins;
      --state_->pin_count;
    }
  }
}

bool PhysicalRangePin::valid() const {
  return armed_ && state_->alive.load(std::memory_order_acquire);
}

PhysicalRangeLeaseRegistry::PhysicalRangeLeaseRegistry()
    : state_(std::make_shared<PhysicalRangeLeaseState>()) {}
PhysicalRangeLeaseRegistry::~PhysicalRangeLeaseRegistry() {
  auto lock = global_critical_region::Acquire();
  StopLocked();
}

uint64_t PhysicalRangeLeaseRegistry::RegisterLocked(
    PhysicalAllocationIdentity identity) {
  if (!state_->alive.load(std::memory_order_relaxed) ||
      !identity.owner.valid() ||
      !ValidRange(identity.physical_address, identity.length)) {
    return 0;
  }
  for (const auto& [base, entry] : state_->entries) {
    if (Overlaps(entry.identity, identity.physical_address, identity.length)) {
      return 0;
    }
  }
  identity.allocation_id = NextAllocationId();
  if (!identity.allocation_id) {
    return 0;
  }
  try {
    state_->entries.emplace(identity.physical_address,
                            PhysicalRangeLeaseState::Entry{identity, 0});
  } catch (const std::bad_alloc&) {
    return 0;
  }
  return identity.allocation_id;
}

PhysicalRangePinResult PhysicalRangeLeaseRegistry::PinLocked(
    const PhysicalAllocationOwner& owner,
    std::span<const PhysicalRange> ranges) {
  PhysicalRangePinResult result;
  if (!state_->alive.load(std::memory_order_relaxed)) {
    result.status = PhysicalLeaseStatus::kStopped;
    return result;
  }
  if (!owner.valid()) {
    result.status = PhysicalLeaseStatus::kInvalidOwner;
    return result;
  }
  if (ranges.empty() || ranges.size() > 4096) {
    return result;
  }
  std::vector<PhysicalAllocationIdentity> identities;
  std::set<uint64_t> seen;
  uint32_t intersections = 0;
  try {
    for (size_t i = 0; i < ranges.size(); ++i) {
      result.failed_range = uint32_t(i);
      const auto& range = ranges[i];
      if (!ValidRange(range.address, range.length)) {
        return result;
      }
      uint32_t cursor = range.address;
      const uint32_t end = range.address + range.length;
      while (cursor < end) {
        if (++intersections > 65536) {
          result.status = PhysicalLeaseStatus::kLimitExceeded;
          return result;
        }
        auto entry = state_->entries.upper_bound(cursor);
        if (entry == state_->entries.begin()) {
          result.status = PhysicalLeaseStatus::kUnowned;
          return result;
        }
        --entry;
        const auto& identity = entry->second.identity;
        if (cursor >= uint64_t(identity.physical_address) + identity.length) {
          result.status = PhysicalLeaseStatus::kUnowned;
          return result;
        }
        if (identity.owner != owner) {
          result.status = PhysicalLeaseStatus::kWrongOwner;
          return result;
        }
        if (seen.insert(identity.allocation_id).second) {
          if (identities.size() == 4096) {
            result.status = PhysicalLeaseStatus::kLimitExceeded;
            return result;
          }
          identities.push_back(identity);
        }
        cursor = std::min(end, identity.physical_address + identity.length);
      }
    }
    auto pin = std::shared_ptr<PhysicalRangePin>(
        new PhysicalRangePin(state_, std::move(identities)));
    // No allocation or callback after incrementing the counts. An unarmed
    // token destroyed by an allocation exception cannot consume another pin.
    for (const auto& identity : pin->allocations_) {
      ++state_->entries.find(identity.physical_address)->second.pins;
      ++state_->pin_count;
    }
    pin->armed_ = true;
    result.pin = std::move(pin);
  } catch (const std::bad_alloc&) {
    result.status = PhysicalLeaseStatus::kAllocationFailed;
    return result;
  }
  result.status = PhysicalLeaseStatus::kSuccess;
  result.failed_range = UINT32_MAX;
  return result;
}

PhysicalLeaseStatus PhysicalRangeLeaseRegistry::FindLocked(
    const PhysicalAllocationOwner& owner, uint64_t allocation_id,
    PhysicalAllocationIdentity* identity) const {
  if (!state_->alive.load(std::memory_order_relaxed)) {
    return PhysicalLeaseStatus::kStopped;
  }
  if (!owner.valid()) {
    return PhysicalLeaseStatus::kInvalidOwner;
  }
  for (const auto& [base, entry] : state_->entries) {
    if (entry.identity.allocation_id != allocation_id) {
      continue;
    }
    if (entry.identity.owner != owner) {
      return PhysicalLeaseStatus::kWrongOwner;
    }
    if (entry.pins) {
      return PhysicalLeaseStatus::kPinned;
    }
    *identity = entry.identity;
    return PhysicalLeaseStatus::kSuccess;
  }
  return PhysicalLeaseStatus::kStaleAllocation;
}

bool PhysicalRangeLeaseRegistry::HasPinsLocked(uint32_t address,
                                               uint32_t length) const {
  if (!state_->pin_count) {
    return false;
  }
  for (const auto& [base, entry] : state_->entries) {
    if (entry.pins && Overlaps(entry.identity, address, length)) {
      return true;
    }
  }
  return false;
}

bool PhysicalRangeLeaseRegistry::LookupLocked(
    uint32_t physical_address, PhysicalAllocationIdentity* identity) const {
  const auto entry = state_->entries.find(physical_address);
  if (entry == state_->entries.end()) {
    return false;
  }
  *identity = entry->second.identity;
  return true;
}

void PhysicalRangeLeaseRegistry::ForgetLocked(uint32_t address,
                                              uint32_t length) {
  for (auto it = state_->entries.begin(); it != state_->entries.end();) {
    if (!it->second.pins && Overlaps(it->second.identity, address, length)) {
      it = state_->entries.erase(it);
    } else {
      ++it;
    }
  }
}

bool PhysicalRangeLeaseRegistry::ClearLocked() {
  if (HasPinsLocked(0, kPhysicalSize)) {
    return false;
  }
  state_->entries.clear();
  return true;
}

void PhysicalRangeLeaseRegistry::StopLocked() {
  state_->alive.store(false, std::memory_order_release);
  state_->entries.clear();
  state_->pin_count = 0;
}
}  // namespace xe::memory
