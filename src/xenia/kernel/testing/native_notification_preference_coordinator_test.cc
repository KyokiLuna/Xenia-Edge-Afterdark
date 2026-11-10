/** Xenia : Xbox 360 Emulator Research Project; BSD license, see LICENSE. */
#include "xenia/kernel/native_notification_preference_coordinator.h"

#include <atomic>
#include <memory>
#include <thread>

#include "third_party/catch/include/catch.hpp"

namespace xe::kernel::test {
namespace {
using Coordinator = NativeNotificationPreferenceCoordinator;
using Phase = Coordinator::Phase;
using Call = Coordinator::NativeCall;
std::shared_ptr<void> Owner() { return std::make_shared<int>(1); }
void EnterNative(Coordinator& c) {
  REQUIRE(c.BeginNativeTransition(Owner()));
  REQUIRE(c.FallbackDrained());
  REQUIRE(c.MarkNativeMayRun());
}
void Observe(Coordinator& c, Coordinator::Options options = {1, 1, 1, 1}) {
  auto operation = c.TryAcquireNative(Call::kGet, Owner());
  REQUIRE(operation);
  REQUIRE(operation->MarkEntering());
  Coordinator::PersistenceObservation persisted;
  persisted.retail_flags = Coordinator::DisabledOptionBits(options);
  REQUIRE(operation->CompleteReadback(options, persisted));
}
}  // namespace

TEST_CASE("Preference handover drains already admitted fallback operations",
          "[notification][preferences][lifecycle]") {
  Coordinator c(Owner());
  auto fallback = c.TryAcquireFallback();
  REQUIRE(fallback);
  std::atomic<bool> transition_started{false}, release{false};
  std::thread writer([operation = std::move(fallback), &transition_started,
                      &release]() mutable {
    transition_started.store(true);
    while (!release.load()) {
      std::this_thread::yield();
    }
    operation.reset();
  });
  while (!transition_started.load()) {
    std::this_thread::yield();
  }
  const bool began = c.BeginNativeTransition(Owner());
  const bool new_fallback_denied = !c.TryAcquireFallback();
  const bool native_waited = !c.MarkNativeMayRun();
  release.store(true);
  writer.join();
  REQUIRE(began);
  CHECK(new_fallback_denied);
  CHECK(native_waited);
  REQUIRE(c.FallbackDrained());
  REQUIRE(c.MarkNativeMayRun());
  CHECK_FALSE(c.TryAcquireFallback());
}

TEST_CASE(
    "Preference transition cancellation cannot reopen after native may run",
    "[notification][preferences][cancellation]") {
  Coordinator c(Owner());
  REQUIRE(c.BeginNativeTransition(Owner()));
  REQUIRE(c.CancelTransitionBeforeNative());
  CHECK(c.TryAcquireFallback());
  EnterNative(c);
  CHECK_FALSE(c.CancelTransitionBeforeNative());
  CHECK_FALSE(c.TryAcquireFallback());
  CHECK_FALSE(c.TryAcquireNative(Call::kSetThenReadback, Owner()));
  auto read = c.TryAcquireNative(Call::kGet, Owner());
  REQUIRE(read);
  REQUIRE(read->CancelBeforeEntry());
  CHECK_FALSE(read->MarkEntering());
  CHECK(c.TryAcquireNative(Call::kGet, Owner()));
}

TEST_CASE("Native preferences project actual cache after failed persistence",
          "[notification][preferences][persistence]") {
  Coordinator c(Owner());
  EnterNative(c);
  Observe(c);
  auto write = c.TryAcquireNative(Call::kSetThenReadback, Owner());
  REQUIRE(write);
  REQUIRE(write->MarkEntering());
  CHECK_FALSE(c.ObservedProjection());
  CHECK_FALSE(c.TryAcquireNative(Call::kGet, Owner()));
  CHECK_FALSE(write->CancelBeforeEntry());
  Coordinator::PersistenceObservation failed;
  failed.retail_flags = 0;  // Previous durable all-enabled options.
  failed.update_observed = true;
  failed.update_error = std::make_error_code(std::errc::no_space_on_device);
  failed.published = false;
  failed.durable = false;
  const Coordinator::Options actual{0, 2, 0, 1};
  REQUIRE(write->CompleteReadback(actual, failed));
  const auto projection = c.ObservedProjection();
  REQUIRE(projection);
  CHECK(projection->native_options == actual);
  CHECK_FALSE(projection->observed_flags_match);
  CHECK(projection->persistence.update_error == failed.update_error);
  CHECK_FALSE(projection->persistence.durable);
  CHECK_FALSE(c.TryAcquireFallback());
  CHECK_FALSE(write->CompleteReadback({1, 1, 1, 1}, {}));
}

TEST_CASE(
    "Published not durable preference outcome is never promoted by readback",
    "[notification][preferences][durability]") {
  Coordinator c(Owner());
  EnterNative(c);
  Coordinator::PersistenceObservation persisted;
  persisted.retail_flags = 0x80000020;
  persisted.update_observed = true;
  persisted.update_error = std::make_error_code(std::errc::io_error);
  persisted.published = true;
  persisted.durable = false;
  auto read = c.TryAcquireNative(Call::kGet, Owner());
  REQUIRE(read);
  REQUIRE(read->MarkEntering());
  REQUIRE(read->CompleteReadback({1, 1, 0, 1}, persisted));
  REQUIRE(c.ObservedProjection());
  CHECK(c.ObservedProjection()->observed_flags_match);
  CHECK(c.ObservedProjection()->persistence.published);
  CHECK_FALSE(c.ObservedProjection()->persistence.durable);
  CHECK(c.ObservedProjection()->persistence.update_error);
  CHECK(Coordinator::DisabledOptionBits({1, 1, 0, 1}) == 0x20);
  CHECK((Coordinator::OptionsFromRetailFlags(0x80000020) ==
         Coordinator::Options{1, 1, 0, 1}));
}

TEST_CASE(
    "Returned missing owner or outputs permits getter retry without replay",
    "[notification][preferences][readback]") {
  Coordinator c(Owner());
  EnterNative(c);
  Observe(c);
  auto write = c.TryAcquireNative(Call::kSetThenReadback, Owner());
  REQUIRE(write);
  REQUIRE(write->MarkEntering());
  Coordinator::PersistenceObservation observed;
  observed.update_observed = true;
  observed.update_error =
      std::make_error_code(std::errc::read_only_file_system);
  REQUIRE(write->CompleteWithoutReadback(observed));
  CHECK_FALSE(c.ObservedProjection());
  CHECK(c.snapshot().phase == Phase::kNativeMayRun);
  REQUIRE(c.snapshot().latest_persistence);
  CHECK(c.snapshot().latest_persistence->update_error == observed.update_error);
  CHECK_FALSE(c.TryAcquireNative(Call::kSetThenReadback, Owner()));
  CHECK_FALSE(c.TryAcquireFallback());
  Observe(c, {0, 1, 0, 1});
  REQUIRE(c.ObservedProjection());
  CHECK((c.ObservedProjection()->native_options ==
         Coordinator::Options{0, 1, 0, 1}));
}

TEST_CASE("Uncertain preference execution retains the actual caller payload",
          "[notification][preferences][ownership]") {
  std::weak_ptr<void> lifetime;
  {
    Coordinator c(Owner());
    EnterNative(c);
    auto owner = Owner();
    lifetime = owner;
    auto read = c.TryAcquireNative(Call::kGet, std::move(owner));
    REQUIRE(read);
    REQUIRE(read->MarkEntering());
    read.reset();  // No normal-return/output observation, never fake success.
    CHECK_FALSE(lifetime.expired());
    CHECK(c.snapshot().uncertain);
    CHECK_FALSE(c.TryAcquireFallback());
    CHECK_FALSE(c.TryAcquireNative(Call::kGet, Owner()));
    c.BeginStop();
    CHECK_FALSE(c.SafelyDrained());
  }  // Simulated owner destruction, NOT a real Kernel-stop proof.
  CHECK(lifetime.expired());
}

TEST_CASE("Observed internal setter invalidates old preference projection",
          "[notification][preferences][internal-caller]") {
  for (bool quiescing : {false, true}) {
    Coordinator unexpected(Owner());
    if (quiescing) {
      REQUIRE(unexpected.BeginNativeTransition(Owner()));
    }
    Coordinator::PersistenceObservation observed;
    observed.retail_flags = 0x12345678;
    observed.update_observed = true;
    observed.update_error = std::make_error_code(std::errc::no_space_on_device);
    observed.published = true;
    observed.durable = false;
    CHECK_FALSE(unexpected.ObserveNativeSetterPersistence(observed));
    CHECK_FALSE(unexpected.TryAcquireFallback());
    const auto snapshot = unexpected.snapshot();
    CHECK(snapshot.uncertain);
    REQUIRE(snapshot.latest_persistence);
    CHECK(snapshot.latest_persistence->retail_flags == observed.retail_flags);
    CHECK(snapshot.latest_persistence->update_observed);
    CHECK(snapshot.latest_persistence->update_error == observed.update_error);
    CHECK(snapshot.latest_persistence->published);
    CHECK_FALSE(snapshot.latest_persistence->durable);
    unexpected.BeginStop();
    CHECK_FALSE(unexpected.SafelyDrained());
  }
  Coordinator c(Owner());
  EnterNative(c);
  Observe(c);
  REQUIRE(c.ObservedProjection());
  Coordinator::PersistenceObservation failure;
  failure.update_observed = true;
  failure.update_error =
      std::make_error_code(std::errc::device_or_resource_busy);
  REQUIRE(c.ObserveNativeSetterPersistence(failure));
  CHECK_FALSE(c.ObservedProjection());
  CHECK_FALSE(c.TryAcquireFallback());
  REQUIRE(c.snapshot().latest_persistence);
  CHECK(c.snapshot().latest_persistence->update_error == failure.update_error);
  // This fixture supplies a later owned observation; it does not execute PPC
  // or assert atomicity with the internal native setter.
  Observe(c, {0, 0, 1, 1});
  REQUIRE(c.ObservedProjection());
  CHECK((c.ObservedProjection()->native_options ==
         Coordinator::Options{0, 0, 1, 1}));
}

TEST_CASE("Stop rejects unentered controls and drains returned active calls",
          "[notification][preferences][stop]") {
  Coordinator c(Owner());
  EnterNative(c);
  auto read = c.TryAcquireNative(Call::kGet, Owner());
  REQUIRE(read);
  SECTION("Before actual native entry") {
    c.BeginStop();
    CHECK_FALSE(read->MarkEntering());
    CHECK_FALSE(c.SafelyDrained());
    REQUIRE(read->CancelBeforeEntry());
  }
  SECTION("After actual entry no fallback or cancellation is manufactured") {
    REQUIRE(read->MarkEntering());
    c.BeginStop();
    CHECK_FALSE(read->CancelBeforeEntry());
    CHECK_FALSE(c.SafelyDrained());
    REQUIRE(read->CompleteReadback({1, 1, 1, 1}, {}));
    CHECK_FALSE(c.ObservedProjection());
  }
  CHECK(c.SafelyDrained());
  CHECK_FALSE(c.TryAcquireFallback());
  CHECK_FALSE(c.TryAcquireNative(Call::kGet, Owner()));
  CHECK_FALSE(c.CancelTransitionBeforeNative());
}
}  // namespace xe::kernel::test
