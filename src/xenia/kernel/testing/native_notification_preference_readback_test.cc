/** Xenia : Xbox 360 Emulator Research Project; BSD license, see LICENSE. */
#include "xenia/kernel/native_notification_preference_readback.h"
#include <array>
#include <cstring>
#include "third_party/catch/include/catch.hpp"
#include "xenia/base/byte_order.h"
#include "xenia/base/mutex.h"
#include "xenia/memory.h"
namespace xe::kernel::test {
namespace {
using Readback = NativeNotificationPreferenceReadback;
struct PreferenceMemory {
  Memory memory;
  static constexpr uint32_t base = 0x90080000;
  PreferenceMemory() {
    REQUIRE(memory.Initialize());
    REQUIRE(memory.LookupHeap(base)->AllocFixed(
        base, 0x1000, 0x1000,
        kMemoryAllocationReserve | kMemoryAllocationCommit,
        kMemoryProtectRead | kMemoryProtectWrite));
  }
  void Returned(
      const NativeNotificationPreferenceCoordinator::Options& values) {
    const auto addresses = Readback::Arguments(base);
    for (unsigned i = 0; i < 4; ++i) {
      xe::store_and_swap<uint32_t>(memory.TranslateVirtual(addresses[i]),
                                   values[i]);
    }
  }
};
TEST_CASE(
    "Preference output proof preserves arbitrary DWORDs and bounds ambiguity",
    "[kernel][notification_preferences]") {
  PreferenceMemory f;
  REQUIRE(Readback::Prepare(f.memory, f.base, 0) == X_STATUS_SUCCESS);
  CHECK(Readback::Read(f.memory, f.base, 0).kind == Readback::Kind::kAmbiguous);
  // These are test-owned simulated returned stores, not native PPC evidence.
  const NativeNotificationPreferenceCoordinator::Options values{
      0x6E6F7000, 0, UINT32_MAX, 0x87654321};
  f.Returned(values);
  CHECK(Readback::Read(f.memory, f.base, 0).kind == Readback::Kind::kAmbiguous);
  REQUIRE(Readback::Prepare(f.memory, f.base, 1) == X_STATUS_SUCCESS);
  f.Returned(values);
  const auto actual = Readback::Read(f.memory, f.base, 1);
  REQUIRE(actual.kind == Readback::Kind::kWritten);
  CHECK(actual.options == values);
  CHECK(Readback::Prepare(f.memory, f.base, 2) == X_STATUS_INVALID_PARAMETER);
  const auto addresses = Readback::Arguments(f.base);
  for (unsigned i = 1; i < 4; ++i) {
    CHECK(addresses[i] - addresses[i - 1] == 12);
  }
}
TEST_CASE(
    "Preference readback detects guard damage and refuses outer critical "
    "region",
    "[kernel][notification_preferences]") {
  PreferenceMemory f;
  REQUIRE(Readback::Prepare(f.memory, f.base, 0) == X_STATUS_SUCCESS);
  f.Returned({0, 1, 2, 3});
  REQUIRE(Readback::Read(f.memory, f.base, 0).kind == Readback::Kind::kWritten);
  auto* bytes = f.memory.TranslateVirtual(f.base);
  bytes[12] ^= 1;
  CHECK(Readback::Read(f.memory, f.base, 0).kind == Readback::Kind::kInvalid);
  const std::array<uint8_t, Readback::kBytes> before = [&] {
    std::array<uint8_t, Readback::kBytes> result;
    std::memcpy(result.data(), bytes, result.size());
    return result;
  }();
  {
    auto held = global_critical_region::AcquireDirect();
    CHECK(Readback::Prepare(f.memory, f.base, 1) ==
          X_STATUS_INVALID_DEVICE_REQUEST);
    CHECK(Readback::Read(f.memory, f.base, 0).status ==
          X_STATUS_INVALID_DEVICE_REQUEST);
    CHECK(global_critical_region::is_held_by_current_thread());
  }
  CHECK(std::memcmp(before.data(), bytes, before.size()) == 0);
  REQUIRE(
      f.memory.LookupHeap(f.base)->Protect(f.base, 0x1000, kMemoryProtectRead));
  CHECK(Readback::Prepare(f.memory, f.base, 1) == X_STATUS_ACCESS_VIOLATION);
  CHECK(Readback::Prepare(f.memory, UINT32_MAX - 15, 0) ==
        X_STATUS_INVALID_PARAMETER);
}
TEST_CASE(
    "Preference cancellation and stop retain actual output ownership through "
    "return",
    "[kernel][notification_preferences]") {
  using Coordinator = NativeNotificationPreferenceCoordinator;
  auto console = std::make_shared<int>(1);
  Coordinator coordinator(console);
  REQUIRE(coordinator.BeginNativeTransition(std::make_shared<int>(2)));
  REQUIRE(coordinator.FallbackDrained());
  REQUIRE(coordinator.MarkNativeMayRun());
  auto output = std::make_shared<PreferenceMemory>();
  std::weak_ptr<PreferenceMemory> retained = output;
  auto operation =
      coordinator.TryAcquireNative(Coordinator::NativeCall::kGet, output);
  REQUIRE(operation);
  REQUIRE(operation->CancelBeforeEntry());
  operation.reset();
  REQUIRE_FALSE(retained.expired());
  operation =
      coordinator.TryAcquireNative(Coordinator::NativeCall::kGet, output);
  REQUIRE(operation);
  REQUIRE(Readback::Prepare(output->memory, output->base, 0) ==
          X_STATUS_SUCCESS);
  REQUIRE(operation->MarkEntering());
  coordinator.BeginStop();
  output->Returned({2, 0, UINT32_MAX, 1});
  const auto read = Readback::Read(output->memory, output->base, 0);
  REQUIRE(read.kind == Readback::Kind::kWritten);
  output.reset();
  CHECK_FALSE(retained.expired());
  REQUIRE(operation->CompleteReadback(read.options, {}));
  // Completion drains coordinator admission; this healthy operation object
  // still owns its invocation/output until the caller releases the scope.
  CHECK_FALSE(retained.expired());
  CHECK_FALSE(coordinator.ObservedProjection());
  CHECK(coordinator.SafelyDrained());
  CHECK_FALSE(coordinator.snapshot().uncertain);
  operation.reset();
  CHECK(retained.expired());
}
}  // namespace
}  // namespace xe::kernel::test
