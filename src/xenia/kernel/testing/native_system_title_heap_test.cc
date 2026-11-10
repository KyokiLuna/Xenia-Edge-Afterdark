/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia contributors. All rights reserved.                    *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */
#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

#include "third_party/catch/include/catch.hpp"
#include "xenia/base/byte_order.h"
#include "xenia/kernel/native_system_title_heap_data.h"
#include "xenia/memory.h"

namespace xe::kernel::test {
namespace {
constexpr uint32_t kDataBlock = 0x81AA0000;
constexpr uint32_t kDataBytes = 0x10000;
constexpr uint32_t kRootCell = 0x81AA3EE0;
constexpr uint32_t kHeapCell = 0x81AABFF8;
constexpr uint32_t kRoot = 0x3002E000;
constexpr uint32_t kBacking = 0x20000000;
constexpr uint32_t kReserveBytes = 0x40000;
constexpr uint32_t kPageBytes = 0x1000;
using HeapData = NativeSystemTitleHeapData;
using Words = std::array<uint32_t, 6>;
constexpr Words kPristine{0xCCCC, 0, 0, 0, 0, 0};

// Actual Memory and public data-only APIs. No Kernel, native allocator call,
// Task, module, runtime readiness or guest execution authority is fabricated.
class TitleHeapDataFixture {
 public:
  explicit TitleHeapDataFixture(bool map_data = true) {
    REQUIRE(memory.Initialize());
    if (!map_data) {
      return;
    }
    auto* heap = memory.LookupHeap(kDataBlock);
    REQUIRE(heap);
    REQUIRE(heap->page_size() == kDataBytes);
    REQUIRE(heap->AllocFixed(kDataBlock, kDataBytes, kDataBytes,
                             kMemoryAllocationReserve,
                             kMemoryProtectRead | kMemoryProtectWrite));
    REQUIRE(heap->AllocFixed(kDataBlock, kDataBytes, kDataBytes,
                             kMemoryAllocationCommit,
                             kMemoryProtectRead | kMemoryProtectWrite));
    std::memset(memory.TranslateVirtual(kDataBlock), 0xA7, kDataBytes);
    Store(kRootCell, kRoot);
    StoreWords(kPristine);
  }
  void Store(uint32_t address, uint32_t value) {
    xe::store_and_swap<uint32_t>(memory.TranslateVirtual(address), value);
  }
  void StoreWords(const Words& words) {
    for (size_t i = 0; i < words.size(); ++i) {
      Store(kHeapCell + uint32_t(i * 4), words[i]);
    }
  }
  HeapData Read() { return ReadNativeSystemTitleHeapData(memory); }
  BaseHeap* BackingHeap() {
    auto* heap = memory.LookupHeap(kBacking);
    REQUIRE(heap);
    REQUIRE(heap->page_size() == kPageBytes);
    return heap;
  }
  void CommitBacking(uint32_t address, uint32_t length) {
    REQUIRE(BackingHeap()->AllocFixed(
        address, length, kPageBytes, kMemoryAllocationCommit,
        kMemoryProtectRead | kMemoryProtectWrite));
    std::memset(memory.TranslateVirtual(address), 0x5A, length);
  }
  void MakeBacking(uint32_t actual_bytes = kReserveBytes,
                   bool commit_all = true) {
    REQUIRE(BackingHeap()->AllocFixed(
        kBacking, actual_bytes, kPageBytes, kMemoryAllocationReserve,
        kMemoryProtectRead | kMemoryProtectWrite));
    if (commit_all) {
      CommitBacking(kBacking, actual_bytes);
    }
    StoreWords({0, kBacking + 0x40, kBacking, actual_bytes, 0x80000000, 1});
  }
  std::vector<uint8_t> Copy(uint32_t address, uint32_t bytes) const {
    const auto* begin = memory.TranslateVirtual<const uint8_t*>(address);
    return {begin, begin + bytes};
  }
  Memory memory;
};
}  // namespace

TEST_CASE("Prepared native Title metadata rejects stale and foreign owners",
          "[kernel][native_title_heap]") {
  const NativeSystemPreparedTitle title{19, 0x30030000, 7, 11};
  CHECK(SameNativeSystemPreparedTitle(title, title));
  // Reuse of the same hmodule cannot retarget an old foreground/load/thread.
  for (const auto& replacement :
       std::array{NativeSystemPreparedTitle{20, title.hmodule, 7, 11},
                  NativeSystemPreparedTitle{19, title.hmodule, 8, 11},
                  NativeSystemPreparedTitle{19, title.hmodule, 7, 12},
                  NativeSystemPreparedTitle{19, 0x30040000, 7, 11},
                  NativeSystemPreparedTitle{0, title.hmodule, 7, 11},
                  NativeSystemPreparedTitle{19, 0, 7, 11},
                  NativeSystemPreparedTitle{19, title.hmodule, 0, 11},
                  NativeSystemPreparedTitle{19, title.hmodule, 7, 0}}) {
    CHECK_FALSE(SameNativeSystemPreparedTitle(title, replacement));
    CHECK_FALSE(SameNativeSystemPreparedTitle(replacement, title));
  }
  CHECK_FALSE(SameNativeSystemPreparedTitle({}, {}));
  // Matching metadata by itself grants neither module nor caller authority.
}

TEST_CASE("Native Title heap reader copies big-endian data without writes",
          "[kernel][native_title_heap]") {
  TitleHeapDataFixture f;
  const auto before = f.Copy(kDataBlock, kDataBytes);
  const auto data = f.Read();
  REQUIRE(data.status == HeapData::Status::kRead);
  CHECK(data.root_hmodule == kRoot);
  CHECK(data.words == kPristine);
  CHECK(NativeSystemTitleHeapIsPristine(data, kRoot));
  CHECK_FALSE(
      NativeSystemTitleHeapHasBacking(f.memory, data, kRoot, kReserveBytes));
  CHECK(f.Copy(kDataBlock, kDataBytes) == before);
  f.Store(kHeapCell, 0);
  CHECK(data.words == kPristine);  // The observation is a local value copy.
  CHECK_FALSE(NativeSystemTitleHeapIsPristine(f.Read(), kRoot));
}

TEST_CASE("Native Title heap observations require real readable data",
          "[kernel][native_title_heap]") {
  SECTION("Unmapped root is rejected before any guest dereference") {
    TitleHeapDataFixture f(false);
    CHECK(f.memory.QueryRangeAccess(kRootCell, 4) ==
          memory::PageAccess::kNoAccess);
    const auto data = f.Read();
    CHECK(data.status == HeapData::Status::kRootUnreadable);
    CHECK(data.root_hmodule == 0);
    CHECK(data.words == Words{});
    CHECK_FALSE(NativeSystemTitleHeapIsPristine(data, kRoot));
    CHECK_FALSE(
        NativeSystemTitleHeapHasBacking(f.memory, data, kRoot, kReserveBytes));
  }
  SECTION("Data-only observation reads readonly pages without changing them") {
    TitleHeapDataFixture f;
    REQUIRE(f.memory.LookupHeap(kDataBlock)
                ->Protect(kDataBlock, kDataBytes, kMemoryProtectRead));
    const auto before = f.Copy(kDataBlock, kDataBytes);
    const auto data = f.Read();
    CHECK(data.status == HeapData::Status::kRead);
    CHECK(data.words == kPristine);
    CHECK(NativeSystemTitleHeapIsPristine(data, kRoot));
    CHECK(f.Copy(kDataBlock, kDataBytes) == before);
    // This is not permission for the native initializer to write these pages.
    REQUIRE(f.memory.LookupHeap(kDataBlock)
                ->Protect(kDataBlock, kDataBytes, kMemoryProtectNoAccess));
    CHECK(f.Read().status == HeapData::Status::kRootUnreadable);
  }
  // Root and heap0 occupy the same real64KiB page. No fake Memory is used to
  // manufacture independent kHeapUnreadable coverage within that page.
}

TEST_CASE("Native Title heap pristine guard refuses reuse and foreign roots",
          "[kernel][native_title_heap]") {
  TitleHeapDataFixture f;
  for (size_t word = 0; word < kPristine.size(); ++word) {
    CAPTURE(word);
    f.StoreWords(kPristine);
    f.Store(kHeapCell + uint32_t(word * 4), word == 0 ? 0 : 1);
    const auto before = f.Copy(kDataBlock, kDataBytes);
    CHECK_FALSE(NativeSystemTitleHeapIsPristine(f.Read(), kRoot));
    CHECK(f.Copy(kDataBlock, kDataBytes) == before);
  }
  f.StoreWords(kPristine);
  f.Store(kRootCell, kRoot + 4);
  const auto before = f.Copy(kDataBlock, kDataBytes);
  CHECK_FALSE(NativeSystemTitleHeapIsPristine(f.Read(), kRoot));
  CHECK_FALSE(NativeSystemTitleHeapIsPristine(f.Read(), 0));
  CHECK(f.Copy(kDataBlock, kDataBytes) == before);
}

TEST_CASE("Native Title heap backing checks actual committed reservation",
          "[kernel][native_title_heap]") {
  TitleHeapDataFixture f;
  f.MakeBacking(kReserveBytes, false);
  const auto data = f.Read();
  CHECK_FALSE(NativeSystemTitleHeapIsPristine(data, kRoot));
  CHECK_FALSE(
      NativeSystemTitleHeapHasBacking(f.memory, data, kRoot, kReserveBytes));
  f.CommitBacking(kBacking, kReserveBytes);
  const auto before_data = f.Copy(kDataBlock, kDataBytes);
  const auto before_backing = f.Copy(kBacking, kReserveBytes);
  REQUIRE(
      NativeSystemTitleHeapHasBacking(f.memory, data, kRoot, kReserveBytes));
  CHECK(f.Copy(kDataBlock, kDataBytes) == before_data);
  CHECK(f.Copy(kBacking, kReserveBytes) == before_backing);

  // This fixed native profile commits its whole allocation, not merely the
  // handle's first page. A hole or readonly tail cannot pass on that prefix.
  const auto tail = kBacking + kReserveBytes - kPageBytes;
  REQUIRE(f.BackingHeap()->Decommit(tail, kPageBytes));
  CHECK_FALSE(
      NativeSystemTitleHeapHasBacking(f.memory, data, kRoot, kReserveBytes));
  f.CommitBacking(tail, kPageBytes);
  REQUIRE(f.BackingHeap()->Protect(tail, kPageBytes, kMemoryProtectRead));
  CHECK_FALSE(
      NativeSystemTitleHeapHasBacking(f.memory, data, kRoot, kReserveBytes));
  REQUIRE(f.BackingHeap()->Protect(tail, kPageBytes,
                                   kMemoryProtectRead | kMemoryProtectWrite));
  CHECK(NativeSystemTitleHeapHasBacking(f.memory, data, kRoot, kReserveBytes));
  REQUIRE(f.BackingHeap()->Release(kBacking));
  CHECK_FALSE(
      NativeSystemTitleHeapHasBacking(f.memory, data, kRoot, kReserveBytes));
  CHECK(f.Copy(kDataBlock, kDataBytes) == before_data);
}

TEST_CASE("Native Title heap guards rounded reservations and descriptor bounds",
          "[kernel][native_title_heap]") {
  TitleHeapDataFixture f;
  // Actual NtAllocateVirtualMemory may round a16KiB request to64KiB granules.
  // These are ordinary real-Memory allocation bytes, not a fake native heap.
  constexpr uint32_t requested = 0x44000;
  constexpr uint32_t allocated = 0x50000;
  f.MakeBacking(allocated);
  const auto good = f.Read();
  const auto before = f.Copy(kDataBlock, kDataBytes);
  const auto backing_before = f.Copy(kBacking, allocated);
  REQUIRE(NativeSystemTitleHeapHasBacking(f.memory, good, kRoot, requested));
  CHECK_FALSE(
      NativeSystemTitleHeapHasBacking(f.memory, good, kRoot, kReserveBytes));
  CHECK_FALSE(
      NativeSystemTitleHeapHasBacking(f.memory, good, kRoot, requested + 1));
  CHECK_FALSE(NativeSystemTitleHeapHasBacking(f.memory, good, kRoot, 0x3C000));
  CHECK_FALSE(
      NativeSystemTitleHeapHasBacking(f.memory, good, kRoot + 4, requested));
  CHECK(f.Copy(kDataBlock, kDataBytes) == before);
  CHECK(f.Copy(kBacking, allocated) == backing_before);

  for (const auto& change : std::array{
           std::array<uint32_t, 2>{0, 0xCCCC}, std::array<uint32_t, 2>{1, 0},
           std::array<uint32_t, 2>{1, kBacking + 0x41},
           std::array<uint32_t, 2>{1, kBacking - 4},
           std::array<uint32_t, 2>{1, kBacking + allocated},
           std::array<uint32_t, 2>{2, 0},
           std::array<uint32_t, 2>{2, kBacking + 1},
           std::array<uint32_t, 2>{3, kReserveBytes},
           std::array<uint32_t, 2>{3, allocated + 0x10000},
           std::array<uint32_t, 2>{4, 0x80}, std::array<uint32_t, 2>{5, 0},
           std::array<uint32_t, 2>{5, 2}}) {
    CAPTURE(change[0], change[1]);
    auto changed = good.words;
    changed[change[0]] = change[1];
    f.StoreWords(changed);
    const auto bytes = f.Copy(kDataBlock, kDataBytes);
    CHECK_FALSE(
        NativeSystemTitleHeapHasBacking(f.memory, f.Read(), kRoot, requested));
    CHECK(f.Copy(kDataBlock, kDataBytes) == bytes);
  }
  auto changed = good.words;
  changed[1] = kBacking + 0x1040;
  changed[2] = kBacking + 0x1000;
  changed[3] = kReserveBytes;
  f.StoreWords(changed);
  // The span is readable/writable, but it is not this allocation's base/size.
  REQUIRE(f.memory.QueryRangeAccess(changed[2], changed[3]) ==
          memory::PageAccess::kReadWrite);
  CHECK_FALSE(NativeSystemTitleHeapHasBacking(f.memory, f.Read(), kRoot,
                                              kReserveBytes));
  changed = good.words;
  changed[1] = 0xFFFFF040;
  changed[2] = 0xFFFFF000;
  f.StoreWords(changed);
  CHECK_FALSE(
      NativeSystemTitleHeapHasBacking(f.memory, f.Read(), kRoot, requested));
}

TEST_CASE("Native Title heap backing refuses a committed physical alias",
          "[kernel][native_title_heap]") {
  TitleHeapDataFixture f;
  constexpr uint32_t physical_alias = 0xE1000000;
  auto* heap = f.memory.LookupHeap(physical_alias);
  REQUIRE(heap);
  REQUIRE(heap->heap_type() == HeapType::kGuestPhysical);
  REQUIRE(heap->AllocFixed(physical_alias, kReserveBytes, kPageBytes,
                           kMemoryAllocationReserve | kMemoryAllocationCommit,
                           kMemoryProtectRead | kMemoryProtectWrite));
  REQUIRE(f.memory.QueryRangeAccess(physical_alias, kReserveBytes) ==
          memory::PageAccess::kReadWrite);
  f.StoreWords(
      {0, physical_alias + 0x40, physical_alias, kReserveBytes, 0x80000000, 1});
  const auto before = f.Copy(kDataBlock, kDataBytes);
  CHECK_FALSE(NativeSystemTitleHeapHasBacking(f.memory, f.Read(), kRoot,
                                              kReserveBytes));
  CHECK(f.Copy(kDataBlock, kDataBytes) == before);
}
}  // namespace xe::kernel::test
