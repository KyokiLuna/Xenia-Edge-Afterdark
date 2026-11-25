/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia contributors. All rights reserved.                    *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */
#include "xenia/kernel/native_system_input_data.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <type_traits>

#include "third_party/catch/include/catch.hpp"
#include "xenia/base/byte_order.h"
#include "xenia/base/mutex.h"
#include "xenia/kernel/native_system_ui_startup.h"
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

namespace {
// Inert real-Memory metadata for rejection coverage only. This fixture has no
// Kernel, source module, original constructor, actual Task or readiness owner.
constexpr uint32_t kProfileQueue = 0x81A8E000;
void PutProfileInitializationMetadata(InputDataFixture& f) {
  f.Store(0x81AA3E90, kProfileQueue | 2);
  f.Store(kProfileQueue, 0x815F90A0);
  f.Store(kProfileQueue + 4, 1);
  f.Store(kProfileQueue + 8, 0);
  f.Store(kProfileQueue + 0x18, 1);
  f.Store(kProfileQueue + 0x1C, 1);
  for (uint32_t slot = 0; slot < 4; ++slot) {
    for (uint32_t kind = 0; kind < 2; ++kind) {
      const uint32_t buffer = 0x81A8E100 + (slot * 2 + kind) * 0x40;
      f.Store((kind ? 0x81AA3320 : 0x81AA28E0) + slot * 0x290 + 0x278,
              buffer);
      std::memset(f.memory.TranslateVirtual(buffer), 0, 0x22);
    }
  }
}
}
TEST_CASE("Profile initialization data rejects absent and Task-tagged serial queues",
          "[native_profile_data][real_memory]") {
  InputDataFixture f;
  PutProfileInitializationMetadata(f);
  const auto reference = GENERATE(0u, kProfileQueue, kProfileQueue | 1u,
                                  kProfileQueue | 3u);
  f.Store(0x81AA3E90, reference);
  const auto before = f.Snapshot();
  const auto observed = ReadNativeSystemProfileInitializationData(f.memory);
  CHECK(observed.status == NativeSystemProfileInitializationData::Status::kQueueTag);
  CHECK(observed.queue_reference == reference);
  CHECK(f.Snapshot() == before);
}
TEST_CASE("Profile init-only data rejects foreign vtable and already active queue state",
          "[native_profile_data][real_memory]") {
  InputDataFixture f;
  PutProfileInitializationMetadata(f);
  const auto cut = GENERATE(0u, 1u, 2u, 3u, 4u);
  if (cut == 0) f.Store(kProfileQueue, 0x815F9098); // native Task vtable
  if (cut == 1) f.Store(kProfileQueue + 4, 2); // legal later; not pristine init
  if (cut == 2) f.Store(kProfileQueue + 8, 0x81A8E400);
  if (cut == 3) f.Store(kProfileQueue + 0x18, 4);
  if (cut == 4) f.Store(kProfileQueue + 0x1C, 0);
  const auto before = f.Snapshot();
  CHECK(ReadNativeSystemProfileInitializationData(f.memory).status ==
        (cut == 0 ? NativeSystemProfileInitializationData::Status::kQueueObject
                  : NativeSystemProfileInitializationData::Status::kQueueState));
  CHECK(f.Snapshot() == before);
}
TEST_CASE("Profile initialization data preserves partial native buffer evidence",
          "[native_profile_data][real_memory]") {
  InputDataFixture f;
  PutProfileInitializationMetadata(f);
  const auto index = GENERATE(0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u);
  const uint32_t cell = (index & 1 ? 0x81AA3320 : 0x81AA28E0) +
                        (index / 2) * 0x290 + 0x278;
  SECTION("An allocation failed") { f.Store(cell, 0); }
  SECTION("A stale buffer aliases the serial queue") { f.Store(cell, kProfileQueue); }
  SECTION("A buffer extent was never committed") { f.Store(cell, 0x81AB0000); }
  SECTION("The original memset has not completed") {
    *f.memory.TranslateVirtual<uint8_t*>(0x81A8E100 + index * 0x40 + 0x21) = 1;
  }
  const auto before = f.Snapshot();
  CHECK(ReadNativeSystemProfileInitializationData(f.memory).status ==
        NativeSystemProfileInitializationData::Status::kBuffer);
  CHECK(f.Snapshot() == before);
}
namespace {
using EmptyProfileStatus = NativeSystemInitialEmptyProfileData::Status;
void PutInitialEmptyProfileMetadata(InputDataFixture& f) {
  PutProfileInitializationMetadata(f);
  f.Store(0x81A82FF0, 0x81AA28E0);
  f.Store(0x81A83028, 0x81A8302C);
  f.Store(0x81A8302C, 0x815FB150);
  f.Store(0x81AA28DC, 0);
  for (uint32_t slot = 0; slot < 4; ++slot) {
    f.Store(0x81AA3D74 + slot * 4, 0);
    for (const auto base : {0x81AA28E0u, 0x81AA3320u}) {
      for (const auto offset :
           {0u, 8u, 0xCu, 0x3Cu, 0x40u, 0x27Cu, 0x280u, 0x288u, 0x28Cu})
        f.Store(base + slot * 0x290 + offset, 0);
    }
  }
}
// Even a complete inert Memory fixture cannot manufacture the call/return
// witness. Positive native constructor execution remains a runtime test.
static_assert(
    !std::is_default_constructible_v<NativeInitialEmptyProfileWitness>);
static_assert(!std::is_copy_constructible_v<NativeInitialEmptyProfileWitness>);
static_assert(!std::is_constructible_v<NativeInitialEmptyProfileWitness,
                                       NativeSystemProfileInitializationData>);
static_assert(std::is_move_constructible_v<NativeInitialEmptyProfileWitness>);
}  // namespace
TEST_CASE(
    "Initial empty profile data distinguishes construction from zero rows",
    "[native_profile_data][real_memory]") {
  InputDataFixture f;
  PutInitialEmptyProfileMetadata(f);
  SECTION("Constructed metadata is a data observation, not a native witness") {
    // DB788 uses exactly0x22 = 34 bytes, not22 decimal or0x22B.
    *f.memory.TranslateVirtual<uint8_t*>(0x81A8E100 + 0x22) = 1;
    const auto before = f.Snapshot();
    const auto data = ReadNativeSystemInitialEmptyProfileData(f.memory);
    REQUIRE(data.status == EmptyProfileStatus::kValid);
    REQUIRE(data.initialization.queue_body == kProfileQueue);
    REQUIRE(data.initialization.buffers[7] == 0x81A8E2C0);
    REQUIRE(f.Snapshot() == before);
  }
  SECTION(
      "Raw zero rows with no real queue or allocated buffers are not "
      "initialized") {
    f.Store(0x81AA3E90, 0);
    for (uint32_t slot = 0; slot < 4; ++slot)
      for (const auto base : {0x81AA28E0u, 0x81AA3320u})
        f.Store(base + slot * 0x290 + 0x278, 0);
    const auto before = f.Snapshot();
    REQUIRE(ReadNativeSystemInitialEmptyProfileData(f.memory).status ==
            EmptyProfileStatus::kInitialization);
    REQUIRE(f.Snapshot() == before);
  }
}
TEST_CASE(
    "Initial empty profile data rejects active provider and row overrides",
    "[native_profile_data][real_memory]") {
  InputDataFixture f;
  PutInitialEmptyProfileMetadata(f);
  const auto cell = GENERATE(0x81A82FF0u, 0x81A83028u, 0x81A8302Cu);
  // A valid static vtable is insufficient when its active provider is changed.
  f.Store(cell, 0x81A8F000);
  const auto before = f.Snapshot();
  REQUIRE(ReadNativeSystemInitialEmptyProfileData(f.memory).status ==
          EmptyProfileStatus::kProvider);
  REQUIRE(f.Snapshot() == before);
}
TEST_CASE(
    "Initial empty profile data requires all primary and secondary identities "
    "absent",
    "[native_profile_data][real_memory]") {
  InputDataFixture f;
  for (const auto base : {0x81AA28E0u, 0x81AA3320u}) {
    for (uint32_t slot = 0; slot < 4; ++slot) {
      for (const auto offset :
           {0u, 8u, 0xCu, 0x3Cu, 0x40u, 0x27Cu, 0x280u, 0x288u, 0x28Cu}) {
        CAPTURE(base, slot, offset);
        PutInitialEmptyProfileMetadata(f);
        f.Store(base + slot * 0x290 + offset, 1);
        const auto before = f.Snapshot();
        REQUIRE(ReadNativeSystemInitialEmptyProfileData(f.memory).status ==
                EmptyProfileStatus::kIdentity);
        REQUIRE(f.Snapshot() == before);
      }
    }
  }
}
TEST_CASE(
    "Initial empty profile data rejects requested presence and publication",
    "[native_profile_data][real_memory]") {
  InputDataFixture f;
  PutInitialEmptyProfileMetadata(f);
  const auto cell =
      GENERATE(0x81AA28DCu, 0x81AA3D74u, 0x81AA3D78u, 0x81AA3D7Cu, 0x81AA3D80u);
  f.Store(cell, 1);
  const auto before = f.Snapshot();
  REQUIRE(ReadNativeSystemInitialEmptyProfileData(f.memory).status ==
          EmptyProfileStatus::kIdentity);
  REQUIRE(f.Snapshot() == before);
}
TEST_CASE(
    "Fresh initial baseline validates allocation metadata without rereading "
    "auth bytes",
    "[native_profile_data][real_memory]") {
  InputDataFixture f;
  PutInitialEmptyProfileMetadata(f);
  const auto captured = ReadNativeSystemInitialEmptyProfileData(f.memory);
  REQUIRE(captured.status == EmptyProfileStatus::kValid);
  SECTION("Fresh comparison retains actual original allocations") {
    // The constructor-only content check is not a later live auth reader.
    *f.memory.TranslateVirtual<uint8_t*>(captured.initialization.buffers[0]) =
        1;
    const auto before = f.Snapshot();
    REQUIRE(ReadNativeSystemInitialEmptyProfileData(f.memory,
                                                    &captured.initialization)
                .status == EmptyProfileStatus::kValid);
    REQUIRE(ReadNativeSystemInitialEmptyProfileData(f.memory).status ==
            EmptyProfileStatus::kInitialization);
    REQUIRE(f.Snapshot() == before);
  }
  SECTION(
      "A replaced buffer cannot inherit the earlier constructor observation") {
    f.Store(0x81AA28E0 + 0x278, 0x81A8E500);
    std::memset(f.memory.TranslateVirtual(0x81A8E500), 0, 0x22);
    const auto before = f.Snapshot();
    REQUIRE(ReadNativeSystemInitialEmptyProfileData(f.memory,
                                                    &captured.initialization)
                .status == EmptyProfileStatus::kChangedAllocation);
    REQUIRE(f.Snapshot() == before);
  }
  SECTION("An extra live queue holder is not an identity operation") {
    f.Store(kProfileQueue + 4, 2);
    const auto before = f.Snapshot();
    REQUIRE(ReadNativeSystemInitialEmptyProfileData(f.memory,
                                                    &captured.initialization)
                .status == EmptyProfileStatus::kValid);
    REQUIRE(ReadNativeSystemInitialEmptyProfileData(f.memory).status ==
            EmptyProfileStatus::kInitialization);
    REQUIRE(f.Snapshot() == before);
  }
  SECTION("A zero queue base reference cannot inherit earlier initialization") {
    f.Store(kProfileQueue + 4, 0);
    const auto before = f.Snapshot();
    REQUIRE(ReadNativeSystemInitialEmptyProfileData(f.memory,
                                                    &captured.initialization)
                .status == EmptyProfileStatus::kInitialization);
    REQUIRE(f.Snapshot() == before);
  }
  SECTION("Unavailable old metadata cannot authorize a fresh observation") {
    auto unavailable = captured.initialization;
    unavailable.status =
        NativeSystemProfileInitializationData::Status::kUnavailable;
    const auto before = f.Snapshot();
    REQUIRE(ReadNativeSystemInitialEmptyProfileData(f.memory, &unavailable)
                .status == EmptyProfileStatus::kInitialization);
    REQUIRE(f.Snapshot() == before);
  }
}
}  // namespace xe::kernel::test
