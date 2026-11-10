/**
 * Xenia : Xbox 360 Emulator Research Project
 * Released under the BSD license - see LICENSE in the root for details.
 */
#include "xenia/kernel/util/guest_io_buffer.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <vector>
#include "third_party/catch/include/catch.hpp"
#include "xenia/base/memory.h"
#include "xenia/kernel/util/native_wait_snapshot.h"
#include "xenia/memory.h"

namespace xe::kernel::util::test {
TEST_CASE("Guest I/O read pages use backing host offsets on16KiB hosts",
          "[kernel][file_control][memory][portable]") {
  std::vector<uint32_t> visits, pages;
  REQUIRE(internal::VisitGuestIoHostPages(
      0xE1002000, 0x2000, 0xE0000000, 0x1000, 0x4000, [&](uint32_t address) {
        visits.push_back(address);
        pages.push_back((address - 0xE0000000 + 0x1000) / 0x4000);
      }));
  CHECK((visits == std::vector<uint32_t>{0xE1002000, 0xE1003000}));
  CHECK((pages == std::vector<uint32_t>{0x400, 0x401}));
  visits.clear();
  REQUIRE(internal::VisitGuestIoHostPages(
      0xE1000000, 0x8000, 0xE0000000, 0, 0x4000,
      [&](uint32_t address) { visits.push_back(address); }));
  CHECK((visits == std::vector<uint32_t>{0xE1000000, 0xE1004000}));
  visits.clear();
  REQUIRE(internal::VisitGuestIoHostPages(
      0xE1003000, 0x4000, 0xE0000000, 0x1000, 0x4000,
      [&](uint32_t address) { visits.push_back(address); }));
  CHECK((visits == std::vector<uint32_t>{0xE1003000}));
  visits.clear();
  REQUIRE(internal::VisitGuestIoHostPages(
      0xFFFFE000, 0x2000, 0xE0000000, 0x1000, 0x4000,
      [&](uint32_t address) { visits.push_back(address); }));
  CHECK((visits == std::vector<uint32_t>{0xFFFFE000, 0xFFFFF000}));
}

TEST_CASE("Invalid guest I/O page coordinates invoke no provider",
          "[kernel][file_control][memory][portable]") {
  uint32_t visits = 0;
  const auto visit = [&](uint32_t) { ++visits; };
  CHECK_FALSE(internal::VisitGuestIoHostPages(0xE1002000, 0x2000, 0xE2000000,
                                              0x1000, 0x4000, visit));
  CHECK_FALSE(internal::VisitGuestIoHostPages(0xFFFFFFF8, 16, 0xE0000000,
                                              0x1000, 0x4000, visit));
  CHECK_FALSE(internal::VisitGuestIoHostPages(0xE1002000, 0x2000, 0xE0000000,
                                              0x1000, 0, visit));
  CHECK_FALSE(internal::VisitGuestIoHostPages(0xE1002000, 0x2000, 0xE0000000,
                                              0x1000, 0x4001, visit));
  CHECK(
      internal::VisitGuestIoHostPages(0, 0, 0xE0000000, 0x1000, 0x4000, visit));
  CHECK(visits == 0);
}
namespace {
struct VirtualFixture {
  static constexpr uint32_t kAddress = 0x90028000;
  Memory memory;
  VirtualFixture() {
    REQUIRE(memory.Initialize());
    auto* heap = memory.LookupHeap(kAddress);
    REQUIRE(heap->AllocFixed(kAddress - 0x1000, 0x3000, 0x1000,
                             kMemoryAllocationReserve,
                             kMemoryProtectRead | kMemoryProtectWrite));
    REQUIRE(heap->AllocFixed(kAddress, 0x1000, 0x1000, kMemoryAllocationCommit,
                             kMemoryProtectRead | kMemoryProtectWrite));
    std::memset(memory.TranslateVirtual(kAddress), 0xA5, 0x1000);
  }
};
}  // namespace

TEST_CASE(
    "Guest I/O copies validate complete spans and preserve rejected buffers",
    "[kernel][file_control][memory]") {
  VirtualFixture fixture;
  auto& memory = fixture.memory;
  constexpr uint32_t address = VirtualFixture::kAddress;
  const std::array<uint8_t, 5> input{1, 2, 3, 4, 5};
  REQUIRE(GuestIoRangeValid(memory, address, 0x1000, true));
  CHECK_FALSE(GuestIoRangeValid(memory, address, 0x1001, true));
  CHECK_FALSE(GuestIoRangeValid(memory, 0, 1, true));
  CHECK_FALSE(GuestIoRangeValid(memory, 0xFFFFFFFE, 8, true));
  REQUIRE(WriteGuestIoBuffer(memory, address + 32, input) == X_STATUS_SUCCESS);
  CHECK(memory.TranslateVirtual(address)[31] == 0xA5);
  CHECK(memory.TranslateVirtual(address)[37] == 0xA5);
  std::array<uint8_t, 5> output{};
  REQUIRE(ReadGuestIoBuffer(memory, address + 32, output) == X_STATUS_SUCCESS);
  CHECK(output == input);
  output.fill(0xC7);
  const auto before = output;
  CHECK(ReadGuestIoBuffer(memory, address + 0xFFC, output) ==
        X_STATUS_ACCESS_VIOLATION);
  CHECK(output == before);
  CHECK(WriteGuestIoBuffer(memory, address + 0xFFC, input) ==
        X_STATUS_ACCESS_VIOLATION);
  CHECK(std::all_of(memory.TranslateVirtual(address + 0xFFC),
                    memory.TranslateVirtual(address + 0x1000),
                    [](uint8_t value) { return value == 0xA5; }));
  REQUIRE(
      memory.LookupHeap(address)->Protect(address, 0x1000, kMemoryProtectRead));
  REQUIRE(ReadGuestIoBuffer(memory, address + 32, output) == X_STATUS_SUCCESS);
  CHECK(output == input);
  CHECK(WriteGuestIoBuffer(memory, address + 32, before) ==
        X_STATUS_ACCESS_VIOLATION);
  CHECK(ReadGuestIoBuffer(memory, address + 32, output) == X_STATUS_SUCCESS);
  CHECK(output == input);
}

TEST_CASE("Guest I/O status stores exact big endian count and failure status",
          "[kernel][file_control][memory]") {
  VirtualFixture fixture;
  auto& memory = fixture.memory;
  constexpr uint32_t address = VirtualFixture::kAddress + 32;
  REQUIRE(WriteGuestIoStatus(memory, address, X_STATUS_DISK_FULL, 0x12345678) ==
          X_STATUS_SUCCESS);
  const std::array<uint8_t, 8> expected{0xC0, 0x00, 0x00, 0x7F,
                                        0x12, 0x34, 0x56, 0x78};
  CHECK(std::memcmp(memory.TranslateVirtual(address), expected.data(),
                    expected.size()) == 0);
  CHECK(memory.TranslateVirtual(address)[-1] == 0xA5);
  CHECK(memory.TranslateVirtual(address)[8] == 0xA5);
  REQUIRE(WriteGuestIoStatus(memory, address, X_STATUS_PENDING, 0) ==
          X_STATUS_SUCCESS);
  CHECK(xe::load_and_swap<uint32_t>(memory.TranslateVirtual(address)) ==
        X_STATUS_PENDING);
  CHECK(xe::load_and_swap<uint32_t>(memory.TranslateVirtual(address + 4)) == 0);
  CHECK(WriteGuestIoStatus(memory, VirtualFixture::kAddress + 0xFFC,
                           X_STATUS_SUCCESS, 8) == X_STATUS_ACCESS_VIOLATION);
  CHECK(xe::load_and_swap<uint32_t>(memory.TranslateVirtual(
            VirtualFixture::kAddress + 0xFFC)) == 0xA5A5A5A5);
}

namespace {
struct PhysicalFixture {
  static constexpr uint32_t kAddress = 0xE1000000;
  Memory memory;
  uint32_t physical = 0;
  void* read_callback = nullptr;
  void* invalidation_callback = nullptr;
  bool provided = false, saw_unlocked_provider = false, saw_new_bytes = false;
  uint32_t providers = 0, invalidations = 0;
  std::array<uint8_t, 8> expected{1, 2, 3, 4, 5, 6, 7, 8};
  PhysicalFixture() {
    REQUIRE(memory.Initialize());
    memory.SetPhysicalAliasSkipHostProtect(true);
    REQUIRE(memory.LookupHeap(kAddress)->AllocFixed(
        kAddress, 0x2000, 0x1000,
        kMemoryAllocationReserve | kMemoryAllocationCommit,
        kMemoryProtectRead | kMemoryProtectWrite));
    physical = memory.GetPhysicalAddress(kAddress);
    REQUIRE(physical != UINT32_MAX);
    std::memset(memory.TranslatePhysical(physical), 0x31, 0x2000);
    read_callback = memory.RegisterPhysicalMemoryReadCallback(
        [](void* data, uint32_t start, uint32_t size,
           Memory::PhysicalAccess access) {
          auto& fixture = *static_cast<PhysicalFixture*>(data);
          if (uint64_t(start) + size <= fixture.physical ||
              start >= fixture.physical + 0x1000) {
            return;
          }
          ++fixture.providers;
          fixture.saw_unlocked_provider |=
              !global_critical_region::is_held_by_current_thread();
          if (!fixture.provided && access != Memory::PhysicalAccess::kDiscard) {
            std::memset(fixture.memory.TranslatePhysical(fixture.physical),
                        0xA5, 0x1000);
            fixture.provided = true;
          }
        },
        this);
    invalidation_callback = memory.RegisterPhysicalMemoryInvalidationCallback(
        [](void* data, uint32_t start, uint32_t size, bool) {
          auto& fixture = *static_cast<PhysicalFixture*>(data);
          if (uint64_t(start) + size > fixture.physical &&
              start < fixture.physical + 0x1000) {
            ++fixture.invalidations;
            fixture.saw_new_bytes |=
                std::memcmp(
                    fixture.memory.TranslatePhysical(fixture.physical + 32),
                    fixture.expected.data(), fixture.expected.size()) == 0;
          }
          return std::pair<uint32_t, uint32_t>{0, UINT32_MAX};
        },
        this);
    memory.EnablePhysicalMemoryAccessCallbacks(physical, 0x1000, true, true);
  }
  ~PhysicalFixture() {
    memory.UnregisterPhysicalMemoryInvalidationCallback(invalidation_callback);
    memory.UnregisterPhysicalMemoryReadCallback(read_callback);
  }
};
}  // namespace
TEST_CASE(
    "Guest I/O through physical aliases preserves provided neighbor bytes",
    "[kernel][file_control][memory][physical]") {
  PhysicalFixture fixture;
  SECTION("Read input obtains the real watched provider bytes") {
    std::array<uint8_t, 8> output{};
    REQUIRE(ReadGuestIoBuffer(fixture.memory, PhysicalFixture::kAddress + 32,
                              output) == X_STATUS_SUCCESS);
    CHECK(std::all_of(output.begin(), output.end(),
                      [](uint8_t value) { return value == 0xA5; }));
    CHECK(fixture.providers != 0);
    CHECK(fixture.saw_unlocked_provider);
  }
  SECTION(
      "Write output invalidates after copy and retains partial-page edges") {
    REQUIRE(WriteGuestIoBuffer(fixture.memory, PhysicalFixture::kAddress + 32,
                               fixture.expected) == X_STATUS_SUCCESS);
    CHECK(fixture.saw_unlocked_provider);
    CHECK(fixture.invalidations != 0);
    CHECK(fixture.saw_new_bytes);
    const auto* bytes = fixture.memory.TranslatePhysical(fixture.physical);
    CHECK(bytes[0] == 0xA5);
    CHECK(bytes[31] == 0xA5);
    CHECK(bytes[40] == 0xA5);
    CHECK(bytes[0xFFF] == 0xA5);
  }
  SECTION("IOSB uses the same protected physical path") {
    REQUIRE(WriteGuestIoStatus(fixture.memory, PhysicalFixture::kAddress + 32,
                               X_STATUS_CANCELLED, 16) == X_STATUS_SUCCESS);
    const auto* bytes = fixture.memory.TranslatePhysical(fixture.physical);
    CHECK(xe::load_and_swap<uint32_t>(bytes + 32) == X_STATUS_CANCELLED);
    CHECK(xe::load_and_swap<uint32_t>(bytes + 36) == 16);
    CHECK(bytes[31] == 0xA5);
    CHECK(bytes[40] == 0xA5);
    CHECK(fixture.invalidations != 0);
  }
}

TEST_CASE(
    "Nested global I/O context is rejected without providers or lock loss",
    "[kernel][file_control][memory][physical][lifetime]") {
  PhysicalFixture fixture;
  std::array<uint8_t, 8> output;
  output.fill(0xC7);
  const auto before = output;
  auto outer = global_critical_region::AcquireDirect();
  {
    auto inner = global_critical_region::AcquireDirect();
    REQUIRE(global_critical_region::is_held_by_current_thread());
    CHECK(GuestIoRangeValid(fixture.memory, PhysicalFixture::kAddress + 32, 8,
                            true));  // Nonwaiting metadata query.
    CHECK(ReadGuestIoBuffer(fixture.memory, PhysicalFixture::kAddress + 32,
                            output) == X_STATUS_INVALID_DEVICE_REQUEST);
    CHECK(output == before);
    CHECK(WriteGuestIoBuffer(fixture.memory, PhysicalFixture::kAddress + 32,
                             fixture.expected) ==
          X_STATUS_INVALID_DEVICE_REQUEST);
    CHECK(WriteGuestIoStatus(fixture.memory, PhysicalFixture::kAddress + 32,
                             X_STATUS_SUCCESS,
                             8) == X_STATUS_INVALID_DEVICE_REQUEST);
    CHECK(fixture.providers == 0);
    CHECK(fixture.invalidations == 0);
    CHECK(xe::load_and_swap<uint32_t>(fixture.memory.TranslatePhysical(
              fixture.physical + 32)) == 0x31313131);
    CHECK(global_critical_region::is_held_by_current_thread());
  }
  CHECK(global_critical_region::is_held_by_current_thread());
  outer.unlock();
  CHECK_FALSE(global_critical_region::is_held_by_current_thread());
  // Prove the same real watch would invoke a provider once admission is valid.
  REQUIRE(ReadGuestIoBuffer(fixture.memory, PhysicalFixture::kAddress + 32,
                            output) == X_STATUS_SUCCESS);
  CHECK(fixture.providers != 0);
  CHECK(fixture.saw_unlocked_provider);
}

// Actual Memory/bounds/watch behavior, no Kernel or native Task is modeled.
TEST_CASE(
    "Native wait snapshot preserves unaligned header and complete BE array "
    "backing",
    "[kernel][memory][native_wait_snapshot]") {
  VirtualFixture f;
  std::array<uint8_t, 256> expected{}, output{};
  for (size_t i = 0; i < expected.size(); ++i) {
    expected[i] = uint8_t(i);
  }
  std::memcpy(f.memory.TranslateVirtual(VirtualFixture::kAddress + 1),
              expected.data(), expected.size());
  auto lock = global_critical_region::AcquireDirect();
  REQUIRE(xe::kernel::internal::ReadNativeWaitSnapshot(
      f.memory, VirtualFixture::kAddress + 1, output));
  CHECK(output == expected);
  CHECK(xe::load_and_swap<uint32_t>(output.data() + 1) == 0x01020304);
}
TEST_CASE(
    "Native wait snapshot refuses complete invalid spans without partial "
    "output",
    "[kernel][memory][native_wait_snapshot]") {
  VirtualFixture f;
  std::array<uint8_t, 16> output;
  output.fill(0xA5);
  const auto before = output;
  for (uint32_t address :
       {0u, 0xFFFFFFF8u, VirtualFixture::kAddress - 0x1000,
        VirtualFixture::kAddress + 0xFF8, VirtualFixture::kAddress + 0x1000}) {
    CHECK_FALSE(xe::kernel::internal::ReadNativeWaitSnapshot(f.memory, address,
                                                             output));
    CHECK(output == before);
  }
  std::array<uint8_t, 257> large;
  large.fill(0xA5);
  const auto large_before = large;
  CHECK_FALSE(xe::kernel::internal::ReadNativeWaitSnapshot(
      f.memory, VirtualFixture::kAddress, large));
  CHECK(large == large_before);
}
TEST_CASE(
    "Native wait physical snapshot reads canonical backing without invoking "
    "providers",
    "[kernel][memory][native_wait_snapshot]") {
  PhysicalFixture f;
  std::array<uint8_t, 16> output{};
  auto lock = global_critical_region::AcquireDirect();
  REQUIRE(xe::kernel::internal::ReadNativeWaitSnapshot(
      f.memory, PhysicalFixture::kAddress + 3, output));
  CHECK(std::all_of(output.begin(), output.end(),
                    [](uint8_t b) { return b == 0x31; }));
  CHECK(f.providers == 0);
  CHECK_FALSE(f.provided);
  CHECK(f.invalidations == 0);
  CHECK(std::all_of(f.memory.TranslatePhysical(f.physical),
                    f.memory.TranslatePhysical(f.physical) + 0x1000,
                    [](uint8_t b) { return b == 0x31; }));
}

}  // namespace xe::kernel::util::test
