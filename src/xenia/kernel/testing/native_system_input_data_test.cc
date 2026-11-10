/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia contributors. All rights reserved.                    *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>

#include "third_party/catch/include/catch.hpp"
#include "xenia/base/byte_order.h"
#include "xenia/base/mutex.h"
#include "xenia/kernel/native_system_input_data.h"
#include "xenia/memory.h"

namespace xe::kernel::test {
namespace {
using Status = NativeSystemInputData::Status;
constexpr uint32_t kBlockBytes = 0x10000;
constexpr std::array<uint32_t, 2> kBlocks{0x81A80000, 0x81AA0000};
constexpr uint32_t kUserTable = 0x81AA3D90;
constexpr uint32_t kEvent = 0x81A854F8;
constexpr uint32_t kTimer = 0x81A854B0;
constexpr uint32_t kDpc = 0x81A854D8;
constexpr uint32_t kFailureFlag = 0x81AAC4B0;
constexpr std::array<uint32_t, 4> kEventWords{0x01000000, 0, 0x81A85500,
                                              0x81A85500};
constexpr std::array<uint32_t, 10> kTimerWords{
    0x08000200, 0, 0x81A854B8, 0x81A854B8, 0, 0, 0, 0, 0, 0};
constexpr std::array<uint32_t, 7> kDpcWords{0x00130002, 0, 0, 0x816F27E0,
                                            0,          0, 0};

// Actual Memory/fixed data addresses, without Kernel, module, Task or event
// objects. Valid bytes here grant no catalog/code/import execution authority.
class InputDataFixture {
 public:
  InputDataFixture() {
    REQUIRE(memory.Initialize());
    for (const auto block : kBlocks) {
      auto* heap = memory.LookupHeap(block);
      REQUIRE(heap);
      REQUIRE(heap->page_size() == kBlockBytes);
      REQUIRE(heap->AllocFixed(block, kBlockBytes, kBlockBytes,
                               kMemoryAllocationReserve,
                               kMemoryProtectRead | kMemoryProtectWrite));
      REQUIRE(heap->AllocFixed(block, kBlockBytes, kBlockBytes,
                               kMemoryAllocationCommit,
                               kMemoryProtectRead | kMemoryProtectWrite));
    }
    Restore();
  }
  void Store(uint32_t address, uint32_t value) {
    xe::store_and_swap<uint32_t>(memory.TranslateVirtual(address), value);
  }
  template <size_t N>
  void StoreWords(uint32_t address, const std::array<uint32_t, N>& words) {
    for (size_t i = 0; i < N; ++i) {
      Store(address + uint32_t(i * 4), words[i]);
    }
  }
  void Restore() {
    for (const auto block : kBlocks) {
      std::memset(memory.TranslateVirtual(block), 0xA7, kBlockBytes);
    }
    Store(0x81A82FF4, kUserTable);
    Store(0x81A83014, 0x81A82FF8);
    Store(0x81A82FF8, 0x816F27F8);
    Store(0x81AACBF0, 0);
    Store(0x81A85C88, 0xFE);
    std::memset(memory.TranslateVirtual(kUserTable), 0, 0x100);
    StoreWords(kTimer, kTimerWords);
    StoreWords(kDpc, kDpcWords);
    StoreWords(kEvent, kEventWords);
    Store(kFailureFlag, 0);
  }
  std::array<uint8_t, 2 * kBlockBytes> Snapshot() const {
    std::array<uint8_t, 2 * kBlockBytes> bytes;
    for (size_t i = 0; i < kBlocks.size(); ++i) {
      std::copy_n(memory.TranslateVirtual<const uint8_t*>(kBlocks[i]),
                  kBlockBytes, bytes.begin() + i * kBlockBytes);
    }
    return bytes;
  }
  void Protect(uint32_t block, uint32_t protection) {
    auto* heap = memory.LookupHeap(block);
    REQUIRE(heap);
    REQUIRE(heap->Protect(block, kBlockBytes, protection));
  }
  Memory memory;
};
}  // namespace

TEST_CASE(
    "Native input data observes pristine real-memory bytes without writes",
    "[native_input_data]") {
  InputDataFixture f;
  const auto before = f.Snapshot();
  const auto initial = ReadNativeSystemInputBootstrapData(f.memory);
  REQUIRE(initial.status == Status::kValid);
  REQUIRE((initial.known & 31) == 31);
  REQUIRE(initial.user_table == kUserTable);
  REQUIRE(initial.class_table == 0x81A82FF8);
  REQUIRE(initial.class0_callback == 0x816F27F8);
  REQUIRE(initial.voice_gate == 0);
  REQUIRE(initial.voice_user == 0xFE);
  REQUIRE(initial.event == kEventWords);
  REQUIRE(initial.timer == kTimerWords);
  REQUIRE(initial.dpc == kDpcWords);
  REQUIRE(initial.failure_flag == 0);
  REQUIRE(ReadNativeSystemInputBindingData(f.memory).status == Status::kValid);
  REQUIRE(f.Snapshot() == before);
}

TEST_CASE("Native input data rejects changed pointers and voice profile",
          "[native_input_data]") {
  InputDataFixture f;
  const std::array cases{
      std::array<uint32_t, 3>{0x81A82FF4, kUserTable + 0x40,
                              uint32_t(Status::kUserTable)},
      std::array<uint32_t, 3>{0x81A83014, 0x81A82FFC,
                              uint32_t(Status::kClassTable)},
      std::array<uint32_t, 3>{0x81A82FF8, 0x816F27E0,
                              uint32_t(Status::kClassTable)},
      std::array<uint32_t, 3>{0x81AACBF0, 1,
                              uint32_t(Status::kVoiceUnsupported)},
      std::array<uint32_t, 3>{0x81A85C88, 0,
                              uint32_t(Status::kVoiceUnsupported)}};
  for (const auto& change : cases) {
    CAPTURE(change[0], change[1]);
    f.Restore();
    f.Store(change[0], change[1]);
    const auto before = f.Snapshot();
    REQUIRE(uint32_t(ReadNativeSystemInputBootstrapData(f.memory).status) ==
            change[2]);
    REQUIRE(uint32_t(ReadNativeSystemInputBindingData(f.memory).status) ==
            change[2]);
    REQUIRE(f.Snapshot() == before);
  }
}

TEST_CASE("Native input bootstrap requires all native user records pristine",
          "[native_input_data]") {
  InputDataFixture f;
  for (uint32_t offset : {0u, 0x48u, 0xFFu}) {
    CAPTURE(offset);
    f.Restore();
    *f.memory.TranslateVirtual<uint8_t*>(kUserTable + offset) = 1;
    const auto before = f.Snapshot();
    REQUIRE(ReadNativeSystemInputBootstrapData(f.memory).status ==
            Status::kRecordsNotPristine);
    REQUIRE(ReadNativeSystemInputBindingData(f.memory).status ==
            Status::kValid);
    REQUIRE(f.Snapshot() == before);
  }
}

TEST_CASE(
    "Native input bootstrap checks every initial event timer and DPC word",
    "[native_input_data]") {
  InputDataFixture f;
  const auto check = [&](uint32_t address, auto words, Status expected) {
    for (size_t i = 0; i < words.size(); ++i) {
      CAPTURE(address, i);
      f.Restore();
      f.Store(address + uint32_t(i * 4), words[i] ^ 1);
      const auto before = f.Snapshot();
      REQUIRE(ReadNativeSystemInputBootstrapData(f.memory).status == expected);
      REQUIRE(ReadNativeSystemInputBindingData(f.memory).status ==
              Status::kValid);
      REQUIRE(f.Snapshot() == before);
    }
  };
  check(kEvent, kEventWords, Status::kEventNotPristine);
  check(kTimer, kTimerWords, Status::kTimerNotPristine);
  check(kDpc, kDpcWords, Status::kDpcNotPristine);
  f.Restore();
  f.Store(kFailureFlag, 1);
  const auto before = f.Snapshot();
  REQUIRE(ReadNativeSystemInputBootstrapData(f.memory).status ==
          Status::kFailureFlag);
  REQUIRE(ReadNativeSystemInputBindingData(f.memory).status == Status::kValid);
  REQUIRE(f.Snapshot() == before);
}

TEST_CASE("Native input binding observation permits native mutable data roles",
          "[native_input_data]") {
  InputDataFixture f;
  f.Store(kUserTable + 0x40, 0x10001);
  f.Store(kUserTable + 0x48, 1);
  f.Store(kUserTable + 0x4C, 0x10000);
  f.Store(kEvent + 4, 1);
  f.Store(kTimer + 4, 1);
  f.Store(kDpc + 4, 0x81A854D8);
  f.Store(kFailureFlag, 1);
  const auto before = f.Snapshot();
  REQUIRE(ReadNativeSystemInputBindingData(f.memory).status == Status::kValid);
  REQUIRE(ReadNativeSystemInputBootstrapData(f.memory).status !=
          Status::kValid);
  REQUIRE(f.Snapshot() == before);
}

TEST_CASE("Native input observer rejects inaccessible actual native pages",
          "[native_input_data]") {
  InputDataFixture f;
  SECTION("First fields are unavailable") {
    f.Protect(kBlocks[0], 0);
    const auto result = ReadNativeSystemInputBootstrapData(f.memory);
    REQUIRE(result.status == Status::kUnreadable);
    REQUIRE(result.known == 0);
    REQUIRE(ReadNativeSystemInputBindingData(f.memory).status ==
            Status::kUnreadable);
  }
  SECTION("Later voice and user-record page is unavailable") {
    f.Protect(kBlocks[1], 0);
    const auto result = ReadNativeSystemInputBootstrapData(f.memory);
    REQUIRE(result.status == Status::kUnreadable);
    REQUIRE((result.known & 7) == 7);
    REQUIRE((result.known & 8) == 0);
    REQUIRE(ReadNativeSystemInputBindingData(f.memory).status ==
            Status::kUnreadable);
  }
}

TEST_CASE("Native input observers read-only under the catalog memory lock",
          "[native_input_data]") {
  InputDataFixture f;
  const auto before = f.Snapshot();
  f.Protect(kBlocks[0], kMemoryProtectRead);
  f.Protect(kBlocks[1], kMemoryProtectRead);
  auto catalog_lock = global_critical_region::AcquireDirect();
  REQUIRE(ReadNativeSystemInputBootstrapData(f.memory).status ==
          Status::kValid);
  REQUIRE(ReadNativeSystemInputBindingData(f.memory).status == Status::kValid);
  REQUIRE(f.Snapshot() == before);
}
}  // namespace xe::kernel::test
