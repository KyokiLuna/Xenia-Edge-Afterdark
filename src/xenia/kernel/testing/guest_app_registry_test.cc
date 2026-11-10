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
#include <thread>

#include "third_party/catch/include/catch.hpp"
#include "xenia/kernel/xam/app_manager.h"

namespace xe::kernel::xam::test {
namespace {

constexpr uint32_t kHud = 0xFF;
constexpr uint32_t kOwner = 0x10000;
constexpr uint32_t kCallback = 0x90001000;
constexpr uint32_t kNoResource = 0xFFFFFFFF;

TEST_CASE("Guest system-process requests require an owned system dispatch",
          "[sysapp]") {
  AppManager manager;
  auto owner = std::make_shared<int>(1);
  REQUIRE(manager.guest_apps().Prepare(kHud, kOwner, owner));
  uint32_t calls = 0;
  REQUIRE(manager.guest_apps().Register(
              kOwner, kHud, kCallback, kNoResource,
              [&](auto, auto, auto) {
                ++calls;
                return X_E_SUCCESS;
              },
              owner) == X_ERROR_SUCCESS);
  REQUIRE(
      manager.guest_apps().Activate(manager.guest_apps().Lookup(kHud, true)));
  CHECK(manager.DispatchMessageAsync(kHud, 1, 0xFFFFFFFF, 9) ==
        X_E_NOT_IMPLEMENTED);
  CHECK(manager.DispatchMessageAsync(kHud, 1, 0xFFFFFFFF, 9, 0x1234) ==
        X_E_NOT_IMPLEMENTED);
  CHECK(calls == 0);
  CHECK(manager.DispatchMessageSync(kHud, 1, 0xFFFFFFFF, 9) == X_E_SUCCESS);
  CHECK(calls == 1);
}

TEST_CASE("Guest SysApp lifecycle precedes ordinary message routing",
          "[sysapp]") {
  GuestAppRegistry registry;
  auto owner = std::make_shared<int>(1);
  REQUIRE(registry.Prepare(kHud, kOwner, owner));
  uint32_t observed_message = 0;
  uint32_t observed_arg1 = 0;
  uint32_t observed_arg2 = 0;
  REQUIRE(registry.Register(
              kOwner, kHud, kCallback, 0,
              [&](uint32_t message, uint32_t arg1, uint32_t arg2) {
                observed_message = message;
                observed_arg1 = arg1;
                observed_arg2 = arg2;
                return uint32_t(0x81234567);
              },
              owner) == X_ERROR_SUCCESS);
  CHECK_FALSE(registry.Lookup(kHud));
  CHECK(registry.Dispatch(kHud, 1, 2, 3) == X_E_INVALIDARG);
  auto callback = registry.Lookup(kHud, true);
  REQUIRE(callback);
  CHECK(callback->DispatchLifecycle(0x80000004, 0xFFFFFFFF, 0x12345678) ==
        0x81234567);
  CHECK(observed_message == 0x80000004);
  CHECK(observed_arg1 == 0xFFFFFFFF);
  CHECK(observed_arg2 == 0x12345678);
  REQUIRE(registry.Activate(callback));
  CHECK(registry.Dispatch(kHud, 0x123, 4, 5) == 0x81234567);
  CHECK(observed_message == 0x123);
  CHECK(registry.ResourceModule(0) == kOwner);
}

TEST_CASE("SysApp registration keeps verified resource update ordering",
          "[sysapp]") {
  GuestAppRegistry registry;
  auto owner = std::make_shared<int>(1);
  REQUIRE(registry.Prepare(kHud, kOwner, owner));
  REQUIRE(registry.Register(
              kOwner, kHud, kCallback, 0,
              [](auto, auto, auto) { return X_E_SUCCESS; },
              owner) == X_ERROR_SUCCESS);
  auto callback = registry.Lookup(kHud, true);
  REQUIRE(registry.Activate(callback));
  CHECK(registry.Register(
            kOwner + 1, kHud, kCallback + 4, 0,
            [](auto, auto, auto) { return X_E_FAIL; },
            owner) == X_ERROR_FUNCTION_FAILED);
  CHECK(registry.ResourceModule(0) == kOwner + 1);
  CHECK(registry.Lookup(kHud) == callback);
  // The resource slot is cleared even though a mismatching owner cannot
  // remove the descriptor's callback. That mismatch still returns success.
  CHECK(registry.Unregister(kOwner + 1, kHud, 0) == X_ERROR_SUCCESS);
  CHECK(registry.ResourceModule(0) == 0);
  CHECK(registry.Lookup(kHud) == callback);
  CHECK(registry.Unregister(kOwner, kHud, kNoResource) == X_ERROR_SUCCESS);
  CHECK_FALSE(registry.Lookup(kHud));
  CHECK(registry.Dispatch(kHud, 0, 0, 0) == X_E_FAIL);
}

TEST_CASE("SysApp descriptor and readiness failures remain distinct",
          "[sysapp]") {
  GuestAppRegistry registry;
  auto owner = std::make_shared<int>(1);
  CHECK(registry.HasDescriptor(0xEF));
  CHECK(registry.HasDescriptor(0xFE));
  CHECK_FALSE(registry.HasDescriptor(kHud));
  CHECK_FALSE(registry.Prepare(0x100FF, kOwner, owner));
  CHECK_FALSE(registry.Prepare(0xEE, kOwner, owner));
  CHECK(registry.Register(
            kOwner, kHud, kCallback, kNoResource,
            [](auto, auto, auto) { return X_E_SUCCESS; },
            owner) == X_ERROR_FUNCTION_FAILED);
  REQUIRE(registry.Prepare(kHud, kOwner, owner));
  CHECK(registry.Unregister(kOwner, kHud, kNoResource) ==
        X_ERROR_FUNCTION_FAILED);
  CHECK(registry.Dispatch(kHud, 0, 0, 0) == X_E_INVALIDARG);
  CHECK_FALSE(registry.Activate({}));
}

TEST_CASE("Zero callback storage does not occupy a SysApp registration",
          "[sysapp]") {
  GuestAppRegistry registry;
  auto owner = std::make_shared<int>(1);
  REQUIRE(registry.Prepare(kHud, kOwner, owner));
  CHECK(registry.Register(kOwner, kHud, 0, 64, {}, owner) == X_ERROR_SUCCESS);
  auto empty = registry.Lookup(kHud, true);
  REQUIRE(empty);
  CHECK(empty->Dispatch(0, 0, 0) == X_E_FAIL);
  CHECK_FALSE(registry.Activate(empty));
  CHECK(registry.ResourceModule(64) == 0);
  CHECK(registry.Register(
            kOwner, kHud, kCallback, kNoResource,
            [](auto, auto, auto) { return X_E_SUCCESS; },
            owner) == X_ERROR_SUCCESS);
  CHECK(empty->Dispatch(0, 0, 0) == X_E_FAIL);
}

TEST_CASE("Self-unregister and replacement never redirect an old request",
          "[sysapp]") {
  GuestAppRegistry registry;
  auto owner = std::make_shared<int>(1);
  REQUIRE(registry.Prepare(kHud, kOwner, owner));
  uint32_t replacement_calls = 0;
  X_RESULT unregister_result = X_ERROR_FUNCTION_FAILED;
  X_RESULT register_result = X_ERROR_FUNCTION_FAILED;
  REQUIRE(registry.Register(
              kOwner, kHud, kCallback, kNoResource,
              [&](auto, auto, auto) {
                unregister_result =
                    registry.Unregister(kOwner, kHud, kNoResource);
                register_result = registry.Register(
                    kOwner, kHud, kCallback + 4, kNoResource,
                    [&](auto, auto, auto) {
                      ++replacement_calls;
                      return X_E_SUCCESS;
                    },
                    owner);
                return uint32_t(0xABCDEF01);
              },
              owner) == X_ERROR_SUCCESS);
  auto original = registry.Lookup(kHud, true);
  REQUIRE(registry.Activate(original));
  CHECK(original->Dispatch(1, 2, 3) == 0xABCDEF01);
  CHECK(unregister_result == X_ERROR_SUCCESS);
  CHECK(register_result == X_ERROR_SUCCESS);
  CHECK(original->Dispatch(1, 2, 3) == X_E_FAIL);
  CHECK_FALSE(registry.Activate(original));
  CHECK(replacement_calls == 0);
  CHECK(registry.Dispatch(kHud, 1, 2, 3) == X_E_SUCCESS);
  CHECK(replacement_calls == 1);
}

TEST_CASE("Retired SysApp snapshots pin code but cannot start another call",
          "[sysapp]") {
  GuestAppRegistry registry;
  auto owner = std::make_shared<int>(1);
  std::weak_ptr<int> lifetime = owner;
  REQUIRE(registry.Prepare(kHud, kOwner, owner));
  uint32_t calls = 0;
  REQUIRE(registry.Register(
              kOwner, kHud, kCallback, 0,
              [&](auto, auto, auto) {
                ++calls;
                return X_E_SUCCESS;
              },
              owner) == X_ERROR_SUCCESS);
  auto pending = registry.Lookup(kHud, true);
  REQUIRE(registry.Activate(pending));
  owner.reset();
  registry.RetireModule(kOwner);
  CHECK_FALSE(lifetime.expired());
  CHECK_FALSE(registry.Lookup(kHud, true));
  CHECK(registry.ResourceModule(0) == 0);
  CHECK(pending->Dispatch(1, 2, 3) == X_E_FAIL);
  CHECK(calls == 0);
  pending.reset();
  CHECK(lifetime.expired());
}

TEST_CASE("Registry shutdown invalidates queued callback generations",
          "[sysapp]") {
  GuestAppRegistry registry;
  auto owner = std::make_shared<int>(1);
  REQUIRE(registry.Prepare(kHud, kOwner, owner));
  uint32_t calls = 0;
  REQUIRE(registry.Register(
              kOwner, kHud, kCallback, 0,
              [&](auto, auto, auto) {
                ++calls;
                return X_E_SUCCESS;
              },
              owner) == X_ERROR_SUCCESS);
  auto pending = registry.Lookup(kHud, true);
  REQUIRE(registry.Activate(pending));
  registry.Clear();
  CHECK(pending->Dispatch(1, 2, 3) == X_E_FAIL);
  CHECK(pending->DispatchLifecycle(0x80000005, 0, 0) == X_E_FAIL);
  CHECK_FALSE(registry.Activate(pending));
  CHECK_FALSE(registry.Lookup(kHud, true));
  CHECK_FALSE(registry.Prepare(kHud, kOwner, owner));
  CHECK(registry.Register(kOwner, kHud, kCallback, 0, {}, owner) ==
        X_ERROR_FUNCTION_FAILED);
  CHECK(calls == 0);
}

TEST_CASE("Destroying a SysApp registry retires externally held requests",
          "[sysapp]") {
  GuestAppRegistry::Snapshot pending;
  uint32_t calls = 0;
  {
    GuestAppRegistry registry;
    auto owner = std::make_shared<int>(1);
    REQUIRE(registry.Prepare(kHud, kOwner, owner));
    REQUIRE(registry.Register(
                kOwner, kHud, kCallback, 0,
                [&](auto, auto, auto) {
                  ++calls;
                  return X_E_SUCCESS;
                },
                owner) == X_ERROR_SUCCESS);
    pending = registry.Lookup(kHud, true);
    REQUIRE(registry.Activate(pending));
  }
  CHECK(pending->Dispatch(1, 2, 3) == X_E_FAIL);
  CHECK(calls == 0);
}

TEST_CASE("SysApp retirement does not hold the registry across guest code",
          "[sysapp]") {
  GuestAppRegistry registry;
  auto owner = std::make_shared<int>(1);
  REQUIRE(registry.Prepare(kHud, kOwner, owner));
  std::promise<void> entered;
  std::promise<void> release;
  auto released = release.get_future().share();
  REQUIRE(registry.Register(
              kOwner, kHud, kCallback, kNoResource,
              [&](auto, auto, auto) {
                entered.set_value();
                released.wait();
                return uint32_t(0x11223344);
              },
              owner) == X_ERROR_SUCCESS);
  auto pending = registry.Lookup(kHud, true);
  REQUIRE(registry.Activate(pending));
  auto invocation = std::async(
      std::launch::async, [pending] { return pending->Dispatch(1, 2, 3); });
  entered.get_future().wait();
  auto retirement =
      std::async(std::launch::async, [&] { registry.RetireModule(kOwner); });
  bool retired_before_release =
      retirement.wait_for(std::chrono::seconds(2)) == std::future_status::ready;
  release.set_value();
  retirement.get();
  CHECK(invocation.get() == 0x11223344);
  CHECK(retired_before_release);
  CHECK(pending->Dispatch(1, 2, 3) == X_E_FAIL);
}

TEST_CASE("SysApp shutdown drains calls before destroying guest UI state",
          "[sysapp]") {
  GuestAppRegistry registry;
  auto owner = std::make_shared<int>(1);
  REQUIRE(registry.Prepare(kHud, kOwner, owner));
  std::promise<void> entered;
  std::promise<void> release;
  auto released = release.get_future().share();
  std::atomic<bool> close_seen{false};
  std::atomic<bool> normal_finished{false};
  std::atomic<bool> close_before_normal_finish{false};
  std::atomic<bool> destroy_after_normal_finish{false};
  REQUIRE(registry.Register(
              kOwner, kHud, kCallback, kNoResource,
              [&](uint32_t message, auto, auto) {
                if (message == 1) {
                  entered.set_value();
                  released.wait();
                  close_before_normal_finish = close_seen.load();
                  normal_finished = true;
                } else if (message == 0x80000003) {
                  close_seen = true;
                } else if (message == 0x80000005) {
                  destroy_after_normal_finish = normal_finished.load();
                }
                return message;
              },
              owner) == X_ERROR_SUCCESS);
  auto callback = registry.Lookup(kHud, true);
  REQUIRE(registry.Activate(callback));
  auto invocation = std::async(
      std::launch::async, [callback] { return callback->Dispatch(1, 2, 3); });
  entered.get_future().wait();
  // The console notifies close before obtaining exclusive callback access.
  CHECK(callback->DispatchLifecycle(0x80000003, 0, 0) == 0x80000003);
  auto shutdown = std::async(std::launch::async,
                             [&] { return registry.BeginShutdown(callback); });
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (registry.Lookup(kHud) && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::yield();
  }
  bool no_normal_lookup = !registry.Lookup(kHud);
  bool waiting_for_active_call =
      shutdown.wait_for(std::chrono::milliseconds(1)) !=
      std::future_status::ready;
  release.set_value();
  CHECK(invocation.get() == 1);
  CHECK(shutdown.get());
  CHECK(no_normal_lookup);
  CHECK(waiting_for_active_call);
  CHECK(callback->Dispatch(2, 0, 0) == X_E_FAIL);
  CHECK_FALSE(registry.Activate(callback));
  CHECK(callback->DispatchLifecycle(0x80000005, 0, 0) == 0x80000005);
  CHECK(close_before_normal_finish.load());
  CHECK(destroy_after_normal_finish.load());
}

TEST_CASE("Module shutdown from its own callback requests deferred teardown",
          "[sysapp]") {
  GuestAppRegistry registry;
  auto owner = std::make_shared<int>(1);
  REQUIRE(registry.Prepare(kHud, kOwner, owner));
  bool shutdown_was_deferred = false;
  REQUIRE(registry.Register(
              kOwner, kHud, kCallback, kNoResource,
              [&](uint32_t message, auto, auto) {
                if (message == 1) {
                  shutdown_was_deferred = !registry.BeginModuleShutdown(kOwner);
                }
                return X_E_SUCCESS;
              },
              owner) == X_ERROR_SUCCESS);
  auto callback = registry.Lookup(kHud, true);
  REQUIRE(registry.Activate(callback));
  CHECK(callback->Dispatch(1, 0, 0) == X_E_SUCCESS);
  CHECK(shutdown_was_deferred);
  CHECK_FALSE(registry.Lookup(kHud));
  CHECK(callback->Dispatch(2, 0, 0) == X_E_FAIL);
  CHECK(registry.BeginModuleShutdown(kOwner));
  CHECK(callback->DispatchLifecycle(0x80000005, 0, 0) == X_E_SUCCESS);
}

TEST_CASE("Module shutdown drains an unregistered callback generation",
          "[sysapp]") {
  GuestAppRegistry registry;
  auto owner = std::make_shared<int>(1);
  REQUIRE(registry.Prepare(kHud, kOwner, owner));
  std::promise<void> entered;
  std::promise<void> release;
  auto released = release.get_future().share();
  REQUIRE(registry.Register(
              kOwner, kHud, kCallback, kNoResource,
              [&](auto, auto, auto) {
                entered.set_value();
                released.wait();
                return X_E_SUCCESS;
              },
              owner) == X_ERROR_SUCCESS);
  auto original = registry.Lookup(kHud, true);
  REQUIRE(registry.Activate(original));
  auto invocation = std::async(
      std::launch::async, [original] { return original->Dispatch(1, 0, 0); });
  entered.get_future().wait();
  auto unregister_result = registry.Unregister(kOwner, kHud, kNoResource);
  auto register_result = registry.Register(
      kOwner, kHud, kCallback + 4, kNoResource,
      [](auto, auto, auto) { return X_E_SUCCESS; }, owner);
  auto replacement = registry.Lookup(kHud);
  auto replacement_result =
      replacement ? replacement->Dispatch(2, 0, 0) : X_E_FAIL;
  auto shutdown = std::async(
      std::launch::async, [&] { return registry.BeginModuleShutdown(kOwner); });
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (registry.Lookup(kHud) && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::yield();
  }
  bool waiting_for_old_generation =
      shutdown.wait_for(std::chrono::milliseconds(1)) !=
      std::future_status::ready;
  release.set_value();
  CHECK(invocation.get() == X_E_SUCCESS);
  CHECK(shutdown.get());
  CHECK(unregister_result == X_ERROR_SUCCESS);
  CHECK(register_result == X_ERROR_SUCCESS);
  CHECK(replacement_result == X_E_SUCCESS);
  CHECK(waiting_for_old_generation);
  CHECK(original->Dispatch(3, 0, 0) == X_E_FAIL);
  REQUIRE(replacement);
  CHECK(replacement->Dispatch(3, 0, 0) == X_E_FAIL);
}

TEST_CASE("Incomplete SysApp execution poisons the generation before cleanup",
          "[sysapp]") {
  GuestAppRegistry registry;
  auto owner = std::make_shared<int>(1);
  REQUIRE(registry.Prepare(kHud, kOwner, owner));
  uint32_t calls = 0;
  REQUIRE(registry.Register(
              kOwner, kHud, kCallback, kNoResource,
              [&](auto, auto, auto) -> GuestAppRegistry::ExecutionResult {
                ++calls;
                // The incidental register value is deliberately success:
                // execution completion must be checked independently of its
                // HRESULT sign.
                return {false, X_E_SUCCESS};
              },
              owner) == X_ERROR_SUCCESS);
  const auto registration = registry.Lookup(kHud, true);
  REQUIRE(registration);
  CHECK(registration->DispatchLifecycle(0x80000004, 0, 0) == X_E_FAIL);
  CHECK(registration->execution_failed());
  CHECK_FALSE(registry.Activate(registration));
  CHECK(registration->DispatchLifecycle(0x80000003, 0, 0) == X_E_FAIL);
  CHECK(registration->DispatchLifecycle(0x80000005, 0, 0) == X_E_FAIL);
  CHECK(calls == 1);
  CHECK(registry.BeginModuleShutdown(kOwner));
  CHECK(calls == 1);
}

TEST_CASE("An activated SysApp execution failure cannot report ready again",
          "[sysapp]") {
  GuestAppRegistry registry;
  auto owner = std::make_shared<int>(1);
  REQUIRE(registry.Prepare(kHud, kOwner, owner));
  REQUIRE(registry.Register(
              kOwner, kHud, kCallback, kNoResource,
              [](auto, auto, auto) -> GuestAppRegistry::ExecutionResult {
                return {false, X_E_SUCCESS};
              },
              owner) == X_ERROR_SUCCESS);
  const auto registration = registry.Lookup(kHud, true);
  REQUIRE(registry.Activate(registration));
  CHECK(registry.Dispatch(kHud, 1, 0, 0) == X_E_FAIL);
  CHECK_FALSE(registry.Lookup(kHud));
  CHECK(registry.Lookup(kHud, true) == registration);
  CHECK_FALSE(registry.Activate(registration));
}

TEST_CASE("SysApp close can release an older call after admission stops",
          "[sysapp]") {
  GuestAppRegistry registry;
  auto owner = std::make_shared<int>(1);
  REQUIRE(registry.Prepare(kHud, kOwner, owner));
  std::promise<void> entered;
  auto started = entered.get_future();
  std::promise<void> close;
  auto closing = close.get_future().share();
  std::atomic<unsigned> destroyed{0};
  REQUIRE(registry.Register(
              kOwner, kHud, kCallback, kNoResource,
              [&](uint32_t message, auto, auto) {
                if (message == 1) {
                  entered.set_value();
                  closing.wait();
                } else if (message == 0x80000003) {
                  close.set_value();
                } else if (message == 0x80000005) {
                  ++destroyed;
                }
                return X_E_SUCCESS;
              },
              owner) == X_ERROR_SUCCESS);
  const auto registration = registry.Lookup(kHud, true);
  REQUIRE(registry.Activate(registration));
  auto old_call = std::async(std::launch::async,
                             [&] { return registry.Dispatch(kHud, 1, 0, 0); });
  started.wait();
  CHECK(registry.StopNormalAdmission(registration));
  CHECK(registration->Dispatch(2, 0, 0) == X_E_FAIL);
  CHECK(registration->DispatchLifecycle(0x80000003, 0, 0) == X_E_SUCCESS);
  CHECK(registry.BeginShutdown(registration));
  CHECK(old_call.get() == X_E_SUCCESS);
  CHECK(registration->DispatchLifecycle(0x80000005, 0, 0) == X_E_SUCCESS);
  CHECK(destroyed == 1);
}

TEST_CASE("Stopping admission preserves close-time self unregistration",
          "[sysapp]") {
  GuestAppRegistry registry;
  auto owner = std::make_shared<int>(1);
  REQUIRE(registry.Prepare(kHud, kOwner, owner));
  X_RESULT unregistered = X_ERROR_FUNCTION_FAILED;
  REQUIRE(registry.Register(
              kOwner, kHud, kCallback, kNoResource,
              [&](uint32_t message, auto, auto) {
                if (message == 0x80000003) {
                  unregistered = registry.Unregister(kOwner, kHud, kNoResource);
                }
                return X_E_SUCCESS;
              },
              owner) == X_ERROR_SUCCESS);
  const auto registration = registry.Lookup(kHud, true);
  REQUIRE(registry.Activate(registration));
  CHECK(registry.StopNormalAdmission(registration));
  CHECK_FALSE(registry.Lookup(kHud));
  CHECK(registration->Dispatch(1, 0, 0) == X_E_FAIL);
  CHECK(registration->DispatchLifecycle(0x80000003, 0, 0) == X_E_SUCCESS);
  CHECK(unregistered == X_ERROR_SUCCESS);
  CHECK_FALSE(registry.Lookup(kHud, true));
  CHECK(registry.BeginModuleShutdown(kOwner));
}

}  // namespace
}  // namespace xe::kernel::xam::test
