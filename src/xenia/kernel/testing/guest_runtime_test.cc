/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <stdexcept>
#include <vector>

#include "third_party/catch/include/catch.hpp"
#include "xenia/kernel/guest_runtime.h"

namespace xe::kernel::test {
namespace {

using Runtime = GuestRuntime;
constexpr uint32_t kModule = 0x90010000;

Runtime::Plan VerifiedPlan() {
  // Synthetic executor inputs, not a list of inferred console constructors.
  return {"test verified plan",
          {{"heap startup",
            {{"heap",
              0x81001000,
              {0x1234},
              Runtime::ReturnKind::kUInt32,
              "verified test ABI: r3 argument, uint32 result"},
             {"second table entry",
              0x81001000,
              {},
              Runtime::ReturnKind::kVoid,
              "verified test ABI: void(void)"}}},
           {"module constructors",
            {{"constructor",
              0x81002000,
              {0x5678, 0x9ABC},
              Runtime::ReturnKind::kHResult,
              "verified test ABI: two GPR arguments, HRESULT"}}}}};
}

Runtime::ExecutionResult Success(uint64_t value = 0) {
  return {true, value, {}};
}

TEST_CASE(
    "Guest runtime resources are not readiness and ordered entries run once",
    "[kernel][guest_runtime]") {
  auto owner = std::make_shared<int>(1);
  Runtime runtime({kModule, 1}, owner);
  const auto plan = VerifiedPlan();
  CHECK(runtime.GetSnapshot().state == Runtime::State::kResourcesLoaded);
  CHECK_FALSE(runtime.GetSnapshot().plan);
  std::vector<Runtime::Function> observed;
  const auto execute = [&](const Runtime::Function& function) {
    const auto active = runtime.GetSnapshot();  // No mutex over executor.
    CHECK(active.state == Runtime::State::kInitializing);
    CHECK(active.initialization_active);
    CHECK(active.current_function.has_value());
    observed.push_back(function);
    return Success(0x123456789ABCDEF0);
  };
  REQUIRE(runtime.Initialize(plan, execute) == Runtime::InitStatus::kReady);
  const auto ready = runtime.GetSnapshot();
  REQUIRE(ready.plan);
  CHECK(*ready.plan == plan);
  CHECK(ready.completed_functions == 3);
  CHECK(ready.total_functions == 3);
  CHECK_FALSE(ready.initialization_active);
  CHECK_FALSE(ready.current_function);
  CHECK_FALSE(ready.failure);
  REQUIRE(observed.size() == 3);
  CHECK(observed[0] == plan.phases[0].functions[0]);
  CHECK(observed[1] == plan.phases[0].functions[1]);
  CHECK(observed[2] == plan.phases[1].functions[0]);
  CHECK(observed[0].address == observed[1].address);  // Not deduplicated.
  CHECK(runtime.Initialize(plan, execute) ==
        Runtime::InitStatus::kAlreadyReady);
  auto changed = plan;
  changed.phases[0].functions[0].abi_metadata = "different ABI";
  CHECK(runtime.Initialize(changed, execute) ==
        Runtime::InitStatus::kPlanMismatch);
  CHECK(observed.size() == 3);
}

TEST_CASE("Guest runtime cannot become ready from an empty or unverified plan",
          "[kernel][guest_runtime]") {
  Runtime runtime({kModule, 1}, std::make_shared<int>(1));
  unsigned calls = 0;
  auto execute = [&](const auto&) {
    ++calls;
    return Success();
  };
  CHECK(runtime.Initialize({}, execute) == Runtime::InitStatus::kInvalidPlan);
  auto unknown = VerifiedPlan();
  unknown.phases[0].functions[0].return_kind =
      Runtime::ReturnKind::kUnspecified;
  CHECK(runtime.Initialize(unknown, execute) ==
        Runtime::InitStatus::kInvalidPlan);
  unknown = VerifiedPlan();
  unknown.phases[0].functions[0].address = 0;
  CHECK(runtime.Initialize(unknown, execute) ==
        Runtime::InitStatus::kInvalidPlan);
  CHECK(runtime.Initialize(VerifiedPlan(), {}) ==
        Runtime::InitStatus::kInvalidPlan);
  CHECK(runtime.GetSnapshot().state == Runtime::State::kResourcesLoaded);
  CHECK(calls == 0);
  Runtime no_owner({kModule, 1}, nullptr);
  CHECK(no_owner.Initialize(VerifiedPlan(), execute) ==
        Runtime::InitStatus::kFailed);
  Runtime no_generation({kModule, 0}, std::make_shared<int>(1));
  CHECK(no_generation.Initialize(VerifiedPlan(), execute) ==
        Runtime::InitStatus::kFailed);
  CHECK(calls == 0);
}

TEST_CASE("Partial guest initialization poisons only its owned generation",
          "[kernel][guest_runtime]") {
  auto owner = std::make_shared<int>(1);
  std::weak_ptr<int> weak_owner = owner;
  Runtime runtime({kModule, 1}, owner);
  owner.reset();
  unsigned calls = 0;
  auto execute = [&](const auto&) -> Runtime::ExecutionResult {
    return ++calls == 2
               ? Runtime::ExecutionResult{false, 0x8007000E, "heap failure"}
               : Success();
  };
  REQUIRE(runtime.Initialize(VerifiedPlan(), execute) ==
          Runtime::InitStatus::kFailed);
  auto failed = runtime.GetSnapshot();
  CHECK(failed.state == Runtime::State::kFailed);
  CHECK(failed.completed_functions == 1);
  REQUIRE(failed.failure);
  CHECK((failed.failure->position == Runtime::Position{0, 1}));
  CHECK(failed.failure->return_value == 0x8007000E);
  CHECK(failed.failure->detail == "heap failure");
  CHECK(runtime.Initialize(VerifiedPlan(), execute) ==
        Runtime::InitStatus::kFailed);
  CHECK(calls == 2);
  CHECK_FALSE(weak_owner.expired());
  CHECK(runtime.RetireAndDrain() == Runtime::DrainStatus::kDrained);
  CHECK(weak_owner.expired());
  Runtime replacement({kModule, 2}, std::make_shared<int>(2));
  CHECK(replacement.Initialize(VerifiedPlan(), [](const auto&) {
    return Success();
  }) == Runtime::InitStatus::kReady);
  CHECK(replacement.GetSnapshot().identity.generation == 2);
  CHECK(failed.identity.generation == 1);  // A saved snapshot never retargets.
  CHECK(runtime.Initialize(VerifiedPlan(), execute) ==
        Runtime::InitStatus::kRetiring);
}

TEST_CASE("Guest executor exceptions poison before propagating",
          "[kernel][guest_runtime]") {
  Runtime runtime({kModule, 1}, std::make_shared<int>(1));
  CHECK_THROWS_AS(
      runtime.Initialize(VerifiedPlan(),
                         [](const auto&) -> Runtime::ExecutionResult {
                           throw std::runtime_error("execution interrupted");
                         }),
      std::runtime_error);
  const auto failed = runtime.GetSnapshot();
  CHECK(failed.state == Runtime::State::kFailed);
  CHECK_FALSE(failed.initialization_active);
  CHECK(failed.completed_functions == 0);
  REQUIRE(failed.failure);
  CHECK((failed.failure->position == Runtime::Position{0, 0}));
  unsigned calls = 0;
  CHECK(runtime.Initialize(VerifiedPlan(), [&](const auto&) {
    ++calls;
    return Success();
  }) == Runtime::InitStatus::kFailed);
  CHECK(calls == 0);
}

TEST_CASE("Guest runtime reentry and self retirement never wait on themselves",
          "[kernel][guest_runtime]") {
  auto owner = std::make_shared<int>(1);
  std::weak_ptr<int> weak_owner = owner;
  Runtime runtime({kModule, 1}, owner);
  owner.reset();
  const auto plan = VerifiedPlan();
  unsigned calls = 0;
  SECTION("Recursive initialization is rejected") {
    auto execute = [&](const auto&) {
      ++calls;
      CHECK(runtime.Initialize(plan, [](const auto&) { return Success(); }) ==
            Runtime::InitStatus::kReentrant);
      return Success();
    };
    CHECK(runtime.Initialize(plan, execute) == Runtime::InitStatus::kReady);
    CHECK(calls == 3);
  }
  SECTION("Self retirement closes admission before deferring drain") {
    auto execute = [&](const auto&) {
      ++calls;
      CHECK(runtime.RetireAndDrain() == Runtime::DrainStatus::kSelfCall);
      CHECK_FALSE(weak_owner.expired());
      CHECK(runtime.GetSnapshot().state == Runtime::State::kRetiring);
      return Success();
    };
    CHECK(runtime.Initialize(plan, execute) == Runtime::InitStatus::kRetiring);
    CHECK(calls == 1);
    CHECK(weak_owner.expired());
    CHECK(runtime.RetireAndDrain() == Runtime::DrainStatus::kDrained);
  }
}

TEST_CASE(
    "Concurrent initialization and nonblocking retirement preserve ownership",
    "[kernel][guest_runtime]") {
  auto owner = std::make_shared<int>(1);
  std::weak_ptr<int> weak_owner = owner;
  Runtime runtime({kModule, 1}, owner);
  owner.reset();
  auto source = VerifiedPlan();
  const auto original = source;
  std::promise<void> entered, release;
  auto released = release.get_future().share();
  std::atomic<unsigned> calls{0};
  auto worker = std::async(std::launch::async, [&] {
    return runtime.Initialize(source, [&](const auto&) {
      if (calls.fetch_add(1) == 0) {
        entered.set_value();
        released.wait();
      }
      return Success();
    });
  });
  const bool started = entered.get_future().wait_for(std::chrono::seconds(1)) ==
                       std::future_status::ready;
  if (!started) {
    release.set_value();
    worker.wait();
    REQUIRE(started);
  }
  source.phases[0].functions[0].arguments[0] = 0xFFFFFFFF;
  CHECK(*runtime.GetSnapshot().plan == original);
  auto contender = std::async(std::launch::async, [&] {
    return runtime.Initialize(source, [](const auto&) { return Success(); });
  });
  const bool rejected =
      contender.wait_for(std::chrono::seconds(1)) == std::future_status::ready;
  CHECK(rejected);
  if (rejected) {
    CHECK(contender.get() == Runtime::InitStatus::kBusy);
  }
  auto preflight = std::async(std::launch::async,
                              [&] { return runtime.RetireAndDrain(false); });
  const bool deferred =
      preflight.wait_for(std::chrono::seconds(1)) == std::future_status::ready;
  CHECK(deferred);
  CHECK_FALSE(weak_owner.expired());
  release.set_value();
  CHECK(worker.get() == Runtime::InitStatus::kRetiring);
  CHECK(preflight.get() == Runtime::DrainStatus::kWouldBlock);
  if (!rejected) {
    CHECK(contender.get() == Runtime::InitStatus::kBusy);
  }
  CHECK(calls == 1);
  CHECK(weak_owner.expired());
  CHECK(runtime.RetireAndDrain() == Runtime::DrainStatus::kDrained);
}

TEST_CASE("Native retirement drains execution and releases owner outside mutex",
          "[kernel][guest_runtime]") {
  Runtime* address = nullptr;
  std::atomic<bool> deleter_reentered{false};
  std::atomic<bool> deleter_state_valid{false};
  auto owner = std::shared_ptr<int>(new int(1), [&](int* value) {
    deleter_state_valid =
        address->GetSnapshot().state == Runtime::State::kRetiring;
    deleter_reentered = true;
    delete value;
  });
  Runtime runtime({kModule, 1}, owner);
  address = &runtime;
  owner.reset();
  std::promise<void> entered, release;
  auto released = release.get_future().share();
  auto plan = VerifiedPlan();
  plan.phases.resize(1);
  plan.phases[0].functions.resize(1);
  auto worker = std::async(std::launch::async, [&] {
    return runtime.Initialize(plan, [&](const auto&) {
      entered.set_value();
      released.wait();
      return Success();
    });
  });
  const bool started = entered.get_future().wait_for(std::chrono::seconds(1)) ==
                       std::future_status::ready;
  if (!started) {
    release.set_value();
    worker.wait();
    REQUIRE(started);
  }
  auto drain =
      std::async(std::launch::async, [&] { return runtime.RetireAndDrain(); });
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(1);
  while (runtime.GetSnapshot().state != Runtime::State::kRetiring &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::yield();
  }
  CHECK(runtime.GetSnapshot().state == Runtime::State::kRetiring);
  CHECK(drain.wait_for(std::chrono::milliseconds(30)) ==
        std::future_status::timeout);
  CHECK_FALSE(deleter_reentered.load());
  release.set_value();
  CHECK(worker.get() == Runtime::InitStatus::kRetiring);
  CHECK(drain.get() == Runtime::DrainStatus::kDrained);
  CHECK(deleter_reentered.load());
  CHECK(deleter_state_valid.load());
}

TEST_CASE(
    "Guest initialization survives erasure of its last external runtime owner",
    "[kernel][guest_runtime]") {
  auto runtime = std::make_shared<Runtime>(Runtime::Identity{kModule, 1},
                                           std::make_shared<int>(1));
  std::weak_ptr<Runtime> weak = runtime;
  auto* address = runtime.get();
  unsigned calls = 0;
  CHECK(address->Initialize(VerifiedPlan(), [&](const auto&) {
    runtime.reset();
    CHECK_FALSE(weak.expired());
    ++calls;
    return Success();
  }) == Runtime::InitStatus::kReady);
  CHECK(calls == 3);
  CHECK(weak.expired());
}

TEST_CASE("Guest initialization owns an executor erased by its callback",
          "[kernel][guest_runtime]") {
  Runtime runtime({kModule, 1}, std::make_shared<int>(1));
  Runtime::Executor stored;
  unsigned calls = 0;
  stored = [&](const auto&) {
    stored = {};
    ++calls;
    return Success();
  };
  CHECK(runtime.Initialize(VerifiedPlan(), stored) ==
        Runtime::InitStatus::kReady);
  CHECK_FALSE(stored);
  CHECK(calls == 3);
}

}  // namespace
}  // namespace xe::kernel::test
