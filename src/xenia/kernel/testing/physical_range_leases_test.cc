/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia contributors. All rights reserved.                    *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */
#include <array>
#include <memory>
#include <vector>

#include "third_party/catch/include/catch.hpp"
#include "xenia/base/byte_stream.h"
#include "xenia/memory.h"

namespace xe::test {
namespace {
using Owner = memory::PhysicalAllocationOwner;
using Range = memory::PhysicalRange;
using Status = memory::PhysicalLeaseStatus;
constexpr Owner kOwner{1, 2, 3, 4};
constexpr uint32_t kRw = kMemoryProtectRead | kMemoryProtectWrite;
constexpr uint32_t kCommit = kMemoryAllocationReserve | kMemoryAllocationCommit;

struct Allocation {
  uint32_t address = 0;
  uint32_t physical = 0;
  uint64_t id = 0;
};

Allocation Allocate(Memory& memory, uint32_t bytes = 0x2000,
                    uint32_t alignment = 0x1000, const Owner& owner = kOwner) {
  Allocation result;
  auto* heap = static_cast<PhysicalHeap*>(memory.LookupHeap(0xE1000000));
  REQUIRE(heap->AllocRangeOwned(0xE1000000, 0xE1100000, bytes, alignment,
                                kCommit, kRw, true, owner, &result.address,
                                &result.id));
  result.physical = memory.GetPhysicalAddress(result.address);
  REQUIRE(result.id);
  REQUIRE(result.physical != UINT32_MAX);
  return result;
}

memory::PhysicalRangePinResult Pin(Memory& memory, const Allocation& allocation,
                                   uint32_t offset = 0, uint32_t bytes = 4,
                                   const Owner& owner = kOwner) {
  const std::array ranges = {Range{allocation.physical + offset, bytes}};
  return memory.PinOwnedPhysicalRanges(owner, ranges);
}

TEST_CASE("Owned physical pins cover actual backing arenas and repeated spans",
          "[memory][physical_leases]") {
  Memory memory;
  REQUIRE(memory.Initialize());
  const auto allocation = Allocate(memory, 0x1001, 0x10000);
  auto* alias = memory.LookupHeap(allocation.address);
  uint32_t actual_size = 0;
  REQUIRE(alias->QuerySize(allocation.address, &actual_size));
  CHECK(actual_size == 0x10000);
  const std::array ranges = {Range{allocation.physical + 16, 36},
                             Range{allocation.physical + 0x8000, 64},
                             Range{allocation.physical + 16, 36}};
  auto first = memory.PinOwnedPhysicalRanges(kOwner, ranges);
  REQUIRE(first);
  REQUIRE(first.pin->valid());
  REQUIRE(first.pin->allocations().size() == 1);
  CHECK(first.pin->allocations()[0].allocation_id == allocation.id);
  CHECK(first.pin->allocations()[0].length == actual_size);
  CHECK(first.pin->allocations()[0].virtual_address == allocation.address);
  CHECK(first.pin->allocations()[0].physical_address == allocation.physical);
  auto second = Pin(memory, allocation);
  REQUIRE(second);
  first.pin.reset();
  CHECK(memory.ReleaseOwnedPhysicalAllocation(kOwner, allocation.id) ==
        Status::kPinned);
  second.pin.reset();
  CHECK(memory.ReleaseOwnedPhysicalAllocation(kOwner, allocation.id) ==
        Status::kSuccess);
}

TEST_CASE("Parent and alternate physical aliases refuse pinned mutations",
          "[memory][physical_leases]") {
  Memory memory;
  REQUIRE(memory.Initialize());
  const auto allocation = Allocate(memory, 0x10000, 0x10000);
  auto pin = Pin(memory, allocation);
  REQUIRE(pin);
  auto* original = memory.LookupHeap(allocation.address);
  auto* parent = memory.GetPhysicalHeap();
  auto* alternate = static_cast<PhysicalHeap*>(memory.LookupHeap(0xA0000000));
  const uint32_t other_address =
      alternate->heap_base() + allocation.physical -
      alternate->GetPhysicalAddress(alternate->heap_base());
  uint32_t unchanged_protect = 0xDEADBEEF;
  CHECK_FALSE(original->Release(allocation.address));
  CHECK_FALSE(parent->Release(allocation.physical));
  CHECK_FALSE(alternate->Release(other_address));
  CHECK_FALSE(original->Decommit(allocation.address, 0x1000));
  CHECK_FALSE(parent->Decommit(allocation.physical, 0x1000));
  CHECK_FALSE(alternate->Decommit(other_address, 0x10000));
  CHECK_FALSE(original->Protect(allocation.address, 0x1000, kMemoryProtectRead,
                                &unchanged_protect));
  CHECK(unchanged_protect == 0xDEADBEEF);
  CHECK_FALSE(parent->Protect(allocation.physical, 0x1000, kMemoryProtectRead));
  CHECK_FALSE(alternate->Protect(other_address, 0x10000, kMemoryProtectRead));
  CHECK_FALSE(
      original->AllocFixed(allocation.address, 0x1000, 0x1000, kCommit, kRw));
  CHECK_FALSE(
      parent->AllocFixed(allocation.physical, 0x1000, 0x1000, kCommit, kRw));
  CHECK_FALSE(
      alternate->AllocFixed(other_address, 0x10000, 0x10000, kCommit, kRw));
  CHECK(memory.QueryRangeAccess(allocation.address, 0x10000) ==
        memory::PageAccess::kReadWrite);
  CHECK(parent->QueryRangeAccess(allocation.physical,
                                 allocation.physical + 0xFFFF) ==
        memory::PageAccess::kReadWrite);
  pin.pin.reset();
  CHECK(memory.ReleaseOwnedPhysicalAllocation(kOwner, allocation.id) ==
        Status::kSuccess);
}

TEST_CASE("Pin validation is owner checked bounded and atomic",
          "[memory][physical_leases]") {
  Memory memory;
  REQUIRE(memory.Initialize());
  const auto allocation = Allocate(memory);
  auto* heap = memory.LookupHeap(allocation.address);
  uint32_t unowned = 0;
  REQUIRE(heap->AllocRange(0xE1000000, 0xE1100000, 0x1000, 0x1000, kCommit, kRw,
                           true, &unowned));
  auto wrong = kOwner;
  ++wrong.session_id;
  CHECK(Pin(memory, allocation, 0, 4, wrong).status == Status::kWrongOwner);
  CHECK(Pin(memory, allocation, 0, 4, {}).status == Status::kInvalidOwner);
  CHECK(Pin(memory, allocation, 0, 0).status == Status::kInvalidRange);
  for (const Range range : {Range{0x1FFFFFFF, 2}, Range{UINT32_MAX, 1}}) {
    const std::array ranges = {range};
    CHECK(memory.PinOwnedPhysicalRanges(kOwner, ranges).status ==
          Status::kInvalidRange);
  }
  const std::array mixed = {Range{allocation.physical, 4},
                            Range{memory.GetPhysicalAddress(unowned), 4}};
  const auto failed = memory.PinOwnedPhysicalRanges(kOwner, mixed);
  CHECK(failed.status == Status::kUnowned);
  CHECK(failed.failed_range == 1);
  CHECK_FALSE(failed.pin);
  // Failed validation must not leave a partial pin on its first allocation.
  CHECK(memory.ReleaseOwnedPhysicalAllocation(kOwner, allocation.id) ==
        Status::kSuccess);
  CHECK(heap->Release(unowned));
}

TEST_CASE("Pinned free is an explicit refusal with no deferred side effect",
          "[memory][physical_leases]") {
  Memory memory;
  REQUIRE(memory.Initialize());
  const auto allocation = Allocate(memory);
  auto pin = Pin(memory, allocation);
  REQUIRE(pin);
  CHECK(memory.ReleaseOwnedPhysicalAllocation(kOwner, allocation.id) ==
        Status::kPinned);
  pin.pin.reset();
  CHECK(memory.QueryRangeAccess(allocation.address, 4) ==
        memory::PageAccess::kReadWrite);
  // Dropping a token never executes a physical free or callbacks.
  auto again = Pin(memory, allocation);
  REQUIRE(again);
  again.pin.reset();
  REQUIRE(memory.ReleaseOwnedPhysicalAllocation(kOwner, allocation.id) ==
          Status::kSuccess);
  CHECK(memory.ReleaseOwnedPhysicalAllocation(kOwner, allocation.id) ==
        Status::kStaleAllocation);
}

TEST_CASE("Unscoped title allocation stays usable beside a native pin",
          "[memory][physical_leases]") {
  Memory memory;
  REQUIRE(memory.Initialize());
  const auto allocation = Allocate(memory);
  auto pin = Pin(memory, allocation);
  REQUIRE(pin);
  auto* heap = memory.LookupHeap(allocation.address);
  uint32_t title = 0;
  REQUIRE(heap->AllocRange(0xE1000000, 0xE1100000, 0x1000, 0x1000, kCommit, kRw,
                           true, &title));
  REQUIRE(heap->Protect(title, 0x1000, kMemoryProtectRead));
  REQUIRE(heap->Release(title));
  CHECK(pin.pin->valid());
}

TEST_CASE("Standalone physical heap metadata has no backing-owner authority",
          "[memory][physical_leases]") {
  Memory memory;
  REQUIRE(memory.Initialize());
  const auto allocation = Allocate(memory);
  auto pin = Pin(memory, allocation);
  REQUIRE(pin);
  Memory* allocator_owner = nullptr;
  SECTION("standalone parent with no Memory") {}
  SECTION("independent VirtualHeap with physical guest semantics") {
    allocator_owner = &memory;
  }
  VirtualHeap parent;
  parent.Initialize(allocator_owner, nullptr, HeapType::kGuestPhysical, 0,
                    0x20000000, 0x1000);
  // Only reserve metadata, as in the existing base heap fixtures. Even at the
  // same physical number this independent table cannot mutate real backing.
  REQUIRE(parent.AllocFixed(allocation.physical, 0x1000, 0x1000,
                            kMemoryAllocationReserve, kMemoryProtectRead));
  parent.Reset();
  CHECK(parent.IsRangeUnallocated(allocation.physical, 0x1000));
  PhysicalHeap alias;
  alias.Initialize(&memory, nullptr, HeapType::kGuestPhysical, 0xA0000000,
                   0x20000000, 0x10000, &parent);
  uint32_t address = UINT32_MAX;
  uint64_t id = UINT64_MAX;
  CHECK_FALSE(alias.AllocRangeOwned(0xA1000000, 0xA1100000, 0x10000, 0x10000,
                                    kCommit, kRw, false, kOwner, &address,
                                    &id));
  CHECK(address == 0);
  CHECK(id == 0);
  CHECK(pin.pin->valid());
  CHECK(memory.ReleaseOwnedPhysicalAllocation(kOwner, allocation.id) ==
        Status::kPinned);
  CHECK(memory.QueryRangeAccess(allocation.address, 0x2000) ==
        memory::PageAccess::kReadWrite);
}

TEST_CASE("Owned allocation IDs reject real physical address reuse",
          "[memory][physical_leases]") {
  Memory memory;
  REQUIRE(memory.Initialize());
  const auto old = Allocate(memory);
  REQUIRE(memory.ReleaseOwnedPhysicalAllocation(kOwner, old.id) ==
          Status::kSuccess);
  const auto replacement = Allocate(memory);
  REQUIRE(replacement.address == old.address);
  CHECK(replacement.id != old.id);
  CHECK(memory.ReleaseOwnedPhysicalAllocation(kOwner, old.id) ==
        Status::kStaleAllocation);
  auto pin = Pin(memory, replacement);
  REQUIRE(pin);
  CHECK(pin.pin->allocations()[0].allocation_id == replacement.id);
}

TEST_CASE(
    "Unpinned decommit or recommit cannot retain old allocation authority",
    "[memory][physical_leases]") {
  Memory memory;
  REQUIRE(memory.Initialize());
  const auto allocation = Allocate(memory);
  auto* heap = memory.LookupHeap(allocation.address);
  SECTION("decommit") {
    REQUIRE(heap->Decommit(allocation.address, 0x1000));
    CHECK(Pin(memory, allocation).status == Status::kNotCommitted);
  }
  SECTION("fixed recommit") {
    REQUIRE(heap->AllocFixed(allocation.address, 0x2000, 0x1000, kCommit, kRw));
    CHECK(Pin(memory, allocation).status == Status::kUnowned);
  }
  CHECK(memory.ReleaseOwnedPhysicalAllocation(kOwner, allocation.id) ==
        Status::kStaleAllocation);
}

TEST_CASE("Tracked free cannot release a callback's replacement allocation",
          "[memory][physical_leases]") {
  Memory memory;
  REQUIRE(memory.Initialize());
  const auto old = Allocate(memory);
  struct Replacement {
    Memory* memory;
    bool attempted = false;
    bool allocated = false;
    Allocation allocation;
  } replacement{&memory};
  struct CallbackRegistration {
    Memory* memory;
    void* handle;
    ~CallbackRegistration() {
      memory->UnregisterPhysicalMemoryInvalidationCallback(handle);
    }
  };
  {
    CallbackRegistration callback{
        &memory,
        memory.RegisterPhysicalMemoryInvalidationCallback(
            [](void* context, uint32_t, uint32_t, bool) {
              auto& replacement = *static_cast<Replacement*>(context);
              // Allocating also invalidates. Only the outer free notification
              // replaces the released allocation, with a different real ID.
              if (!replacement.attempted) {
                replacement.attempted = true;
                auto* heap = static_cast<PhysicalHeap*>(
                    replacement.memory->LookupHeap(0xE1000000));
                auto& allocation = replacement.allocation;
                replacement.allocated = heap->AllocRangeOwned(
                    0xE1000000, 0xE1100000, 0x2000, 0x1000, kCommit, kRw, true,
                    kOwner, &allocation.address, &allocation.id);
                if (replacement.allocated) {
                  allocation.physical = replacement.memory->GetPhysicalAddress(
                      allocation.address);
                  *replacement.memory->TranslateVirtual<uint32_t*>(
                      allocation.address) = 0xAABBCCDD;
                }
              }
              return std::pair<uint32_t, uint32_t>{0, UINT32_MAX};
            },
            &replacement)};
    SECTION("typed release") {
      REQUIRE(memory.ReleaseOwnedPhysicalAllocation(kOwner, old.id) ==
              Status::kSuccess);
    }
    SECTION("ordinary physical release of a tracked allocation") {
      uint32_t released_size = 0;
      REQUIRE(
          memory.LookupHeap(old.address)->Release(old.address, &released_size));
      CHECK(released_size == 0x2000);
    }
  }
  REQUIRE(replacement.attempted);
  REQUIRE(replacement.allocated);
  const auto& current = replacement.allocation;
  REQUIRE(current.address == old.address);
  REQUIRE(current.physical == old.physical);
  CHECK(current.id != old.id);
  HeapAllocationInfo alias_info{}, parent_info{};
  REQUIRE(memory.LookupHeap(current.address)
              ->QueryRegionInfo(current.address, &alias_info));
  REQUIRE(memory.GetPhysicalHeap()->QueryRegionInfo(current.physical,
                                                    &parent_info));
  CHECK(alias_info.allocation_base == current.address);
  CHECK(alias_info.allocation_size == 0x2000);
  CHECK(parent_info.allocation_base == current.physical);
  CHECK(parent_info.allocation_size == 0x2000);
  REQUIRE(memory.QueryRangeAccess(current.address, 0x2000) ==
          memory::PageAccess::kReadWrite);
  CHECK(*memory.TranslateVirtual<uint32_t*>(current.address) == 0xAABBCCDD);
  auto pin = Pin(memory, current);
  REQUIRE(pin);
  CHECK(pin.pin->allocations()[0].allocation_id == current.id);
  CHECK(memory.ReleaseOwnedPhysicalAllocation(kOwner, old.id) ==
        Status::kStaleAllocation);
  CHECK(memory.ReleaseOwnedPhysicalAllocation(kOwner, current.id) ==
        Status::kPinned);
}

TEST_CASE("Reset and restore refuse before any pinned state or stream changes",
          "[memory][physical_leases]") {
  Memory memory;
  REQUIRE(memory.Initialize());
  const auto allocation = Allocate(memory);
  auto* word = memory.TranslateVirtual<uint32_t*>(allocation.address);
  *word = 0x12345678;
  auto pin = Pin(memory, allocation);
  REQUIRE(pin);
  std::array<uint8_t, 8> empty{};
  ByteStream stream(empty.data(), empty.size());
  CHECK_FALSE(memory.Reset());
  CHECK_FALSE(memory.Restore(&stream));
  CHECK_FALSE(memory.Save(&stream));
  CHECK(stream.offset() == 0);
  CHECK(*word == 0x12345678);
  auto* parent = memory.GetPhysicalHeap();
  parent->Reset();
  CHECK_FALSE(parent->Restore(&stream));
  CHECK(stream.offset() == 0);
  CHECK(pin.pin->valid());
  CHECK(parent->IsPageCommitted(allocation.physical));
  pin.pin.reset();
  REQUIRE(memory.Reset());
  CHECK(memory.ReleaseOwnedPhysicalAllocation(kOwner, allocation.id) ==
        Status::kStaleAllocation);
}

TEST_CASE(
    "Real heap restore restores bytes but never serialized host ownership",
    "[memory][physical_leases]") {
  Memory memory;
  REQUIRE(memory.Initialize());
  const auto allocation = Allocate(memory);
  auto* parent = memory.GetPhysicalHeap();
  auto* word = memory.TranslatePhysical<uint32_t*>(allocation.physical);
  *word = 0xAABBCCDD;
  // Initialize commits XPS/GPU backing as well as this test's allocation.
  // Save serializes every committed parent page, not only our owned span.
  size_t committed_bytes = 0;
  for (uint32_t address = parent->heap_base();
       address < parent->heap_base() + parent->heap_size();) {
    HeapAllocationInfo info{};
    REQUIRE(parent->QueryRegionInfo(address, &info));
    REQUIRE(info.region_size);
    REQUIRE(info.region_size <= parent->heap_size() - address);
    if (info.state & kMemoryAllocationCommit) {
      committed_bytes += info.region_size;
    }
    address += info.region_size;
  }
  const size_t table_bytes = parent->total_page_count() * sizeof(uint64_t);
  std::vector<uint8_t> saved(table_bytes + committed_bytes);
  ByteStream stream(saved.data(), saved.size());
  REQUIRE(parent->Save(&stream));
  REQUIRE(stream.offset() == saved.size());
  *word = 0;
  stream.set_offset(0);
  REQUIRE(parent->Restore(&stream));
  CHECK(stream.offset() == saved.size());
  CHECK(*word == 0xAABBCCDD);
  CHECK(Pin(memory, allocation).status == Status::kUnowned);
  CHECK(memory.ReleaseOwnedPhysicalAllocation(kOwner, allocation.id) ==
        Status::kStaleAllocation);
}

TEST_CASE("Physical pin outliving Memory becomes inert without a raw owner",
          "[memory][physical_leases]") {
  std::shared_ptr<const memory::PhysicalRangePin> retained;
  {
    Memory memory;
    REQUIRE(memory.Initialize());
    const auto allocation = Allocate(memory);
    auto result = Pin(memory, allocation);
    REQUIRE(result);
    retained = result.pin;
  }
  CHECK_FALSE(retained->valid());
  retained.reset();
}
}  // namespace
}  // namespace xe::test
