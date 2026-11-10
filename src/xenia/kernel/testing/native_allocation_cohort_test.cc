/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia contributors. All rights reserved.                    *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */
#include "xenia/kernel/native_allocation_cohort.h"
#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
#include <vector>
#include "third_party/catch/include/catch.hpp"
#include "xenia/memory.h"
namespace xe::kernel::test {
TEST_CASE("Pre-Task tagged backing remains the same exact cohort allocation",
          "[kernel][native_allocation_cohort]") {
  Memory memory;
  REQUIRE(memory.Initialize());
  const memory::PhysicalAllocationOwner owner{7, 11, 23, 0x30001000};
  NativeAllocationCohort cohort(owner);
  auto* heap = static_cast<PhysicalHeap*>(memory.LookupHeap(0xE1000000));
  uint32_t address = 0;
  uint64_t allocation = 0;
  REQUIRE(
      heap->AllocRangeOwned(0xE1000000, 0xE1400000, 0x2000, 0x1000,
                            kMemoryAllocationReserve | kMemoryAllocationCommit,
                            kMemoryProtectRead | kMemoryProtectWrite, true,
                            cohort.owner(), &address, &allocation));
  const std::array ranges{
      memory::PhysicalRange{memory.GetPhysicalAddress(address), 0x2000}};
  // Creating later frame identities cannot mutate the preexisting owner or
  // allocator identity. No fake native Task, guest GPU object or Kernel here.
  REQUIRE(cohort.ReserveSourceSerial() == 1);
  auto pin = memory.PinOwnedPhysicalRanges(cohort.owner(), ranges);
  REQUIRE(pin);
  REQUIRE(pin.pin->allocations().size() == 1);
  REQUIRE(pin.pin->allocations()[0].allocation_id == allocation);
  REQUIRE(pin.pin->allocations()[0].owner == owner);
  REQUIRE(memory.ReleaseOwnedPhysicalAllocation(owner, allocation) ==
          memory::PhysicalLeaseStatus::kPinned);
  auto wrong = owner;
  ++wrong.session_id;
  REQUIRE(memory.PinOwnedPhysicalRanges(wrong, ranges).status ==
          memory::PhysicalLeaseStatus::kWrongOwner);
  pin.pin.reset();
  REQUIRE(memory.ReleaseOwnedPhysicalAllocation(owner, allocation) ==
          memory::PhysicalLeaseStatus::kSuccess);
  uint32_t unowned = 0;
  REQUIRE(heap->AllocRange(0xE1000000, 0xE1400000, 0x2000, 0x1000,
                           kMemoryAllocationReserve | kMemoryAllocationCommit,
                           kMemoryProtectRead | kMemoryProtectWrite, true,
                           &unowned));
  const std::array unowned_range{
      memory::PhysicalRange{memory.GetPhysicalAddress(unowned), 0x2000}};
  REQUIRE(memory.PinOwnedPhysicalRanges(cohort.owner(), unowned_range).status ==
          memory::PhysicalLeaseStatus::kUnowned);
  REQUIRE(heap->Release(unowned));
}
TEST_CASE(
    "One cohort writer survives entry exit and closes replacement admission",
    "[kernel][native_allocation_cohort]") {
  NativeAllocationCohort cohort({7, 11, 23, 0x30001000});
  auto frozen_source_hold = cohort.AcquireWriter();
  REQUIRE(frozen_source_hold);
  REQUIRE_FALSE(cohort.AcquireWriter());
  const auto first = cohort.ReserveSourceSerial();
  frozen_source_hold.reset();
  auto next_task_hold = cohort.AcquireWriter();
  REQUIRE(next_task_hold);
  REQUIRE(cohort.ReserveSourceSerial() > first);
  cohort.Close();
  next_task_hold.reset();
  REQUIRE_FALSE(cohort.AcquireWriter());
  REQUIRE(cohort.ReserveSourceSerial() == 0);
}
TEST_CASE(
    "Concurrent delegated source namespaces and writer reservations stay "
    "unique",
    "[kernel][native_allocation_cohort]") {
  NativeAllocationCohort cohort({7, 11, 23, 0x30001000});
  std::mutex result_mutex;
  std::set<uint64_t> serials;
  std::atomic<unsigned> inside{0}, maximum{0};
  std::vector<std::thread> threads;
  for (unsigned worker = 0; worker < 4; ++worker) {
    threads.emplace_back([&] {
      for (unsigned attempt = 0; attempt < 100; ++attempt) {
        const auto serial = cohort.ReserveSourceSerial();
        {
          std::lock_guard lock(result_mutex);
          serials.insert(serial);
        }
        if (auto hold = cohort.AcquireWriter()) {
          const auto current = inside.fetch_add(1) + 1;
          auto previous = maximum.load();
          while (current > previous &&
                 !maximum.compare_exchange_weak(previous, current)) {
          }
          std::this_thread::yield();
          inside.fetch_sub(1);
        }
      }
    });
  }
  for (auto& thread : threads) {
    thread.join();
  }
  REQUIRE(serials.size() == 400);
  REQUIRE_FALSE(serials.contains(0));
  REQUIRE(maximum.load() == 1);
  cohort.Close();
  REQUIRE_FALSE(cohort.accepting());
}
}  // namespace xe::kernel::test
