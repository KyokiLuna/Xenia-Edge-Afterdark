/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <array>
#include <atomic>
#include <barrier>
#include <chrono>
#include <future>
#include <memory>
#include <thread>
#include <vector>

#include "third_party/catch/include/catch.hpp"
#include "xenia/base/byte_stream.h"
#include "xenia/gpu/graphics_interrupt_registry.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/xthread.h"

namespace xe::kernel::test {
namespace {

using Registry = gpu::GraphicsInterruptRegistry;
constexpr uint32_t kTitleCallback = 0x82001000;
constexpr uint32_t kSystemCallback = 0x81002000;
constexpr uint32_t kTitleModule = 0x90010000;
constexpr uint32_t kSystemModule = 0x90020000;

TEST_CASE("Graphics legacy selection and explicit owners remain separate",
          "[graphics][interrupt]") {
  Registry registry;
  auto title_pin = std::make_shared<int>(1);
  auto system_pin = std::make_shared<int>(2);
  registry.Set({kTitleCallback, 0x1234}, X_PROCTYPE_TITLE, kTitleModule,
               title_pin);
  const auto title = registry.CaptureOwner(X_PROCTYPE_TITLE);
  REQUIRE(title);
  registry.Set({kSystemCallback, 0x5678}, X_PROCTYPE_SYSTEM, kSystemModule,
               system_pin);
  const auto system = registry.CaptureOwner(X_PROCTYPE_SYSTEM);
  REQUIRE(system);
  uint32_t callback = 0;
  uint32_t data = 0;
  auto execute = [&](const auto& registration) {
    callback = registration.callback();
    data = registration.user_data();
  };
  REQUIRE(registry.DispatchLegacy(execute));
  CHECK(callback == kSystemCallback);
  CHECK(data == 0x5678);
  REQUIRE(registry.DispatchOwner(title, X_PROCTYPE_TITLE, title->generation(),
                                 execute));
  CHECK(callback == kTitleCallback);
  CHECK(data == 0x1234);
  registry.Set({}, X_PROCTYPE_SYSTEM);
  CHECK_FALSE(registry.DispatchLegacy(execute));
  CHECK_FALSE(registry.CaptureOwner(X_PROCTYPE_SYSTEM));
  CHECK(registry.CaptureOwner(X_PROCTYPE_TITLE) == title);
  CHECK(registry.DispatchOwner(title, X_PROCTYPE_TITLE, title->generation(),
                               execute));
  CHECK_FALSE(registry.DispatchOwner(system, X_PROCTYPE_SYSTEM,
                                     system->generation(), execute));
}

TEST_CASE("Delayed explicit graphics events cannot call a replacement",
          "[graphics][interrupt]") {
  Registry registry;
  auto pin = std::make_shared<int>(1);
  registry.Set({kTitleCallback, 1}, X_PROCTYPE_TITLE, kTitleModule, pin);
  const auto pending = registry.CaptureOwner(X_PROCTYPE_TITLE);
  REQUIRE(pending);
  // Existing unclassified PM4 work resolves the latest legacy registration at
  // delivery. Capturing an owner token is an opt-in contract, not inferred.
  auto legacy_delivery = [&]() {
    uint32_t callback = 0;
    registry.DispatchLegacy(
        [&](const auto& registration) { callback = registration.callback(); });
    return callback;
  };
  registry.Set({kTitleCallback + 4, 2}, X_PROCTYPE_TITLE, kTitleModule, pin);
  const auto replacement = registry.CaptureOwner(X_PROCTYPE_TITLE);
  REQUIRE(replacement);
  CHECK(replacement->generation() != pending->generation());
  unsigned calls = 0;
  auto execute = [&](const auto&) { ++calls; };
  CHECK_FALSE(registry.DispatchOwner(pending, X_PROCTYPE_TITLE,
                                     pending->generation(), execute));
  CHECK_FALSE(registry.DispatchOwner(replacement, X_PROCTYPE_SYSTEM,
                                     replacement->generation(), execute));
  CHECK_FALSE(registry.DispatchOwner(replacement, X_PROCTYPE_TITLE,
                                     pending->generation(), execute));
  CHECK(calls == 0);
  CHECK(legacy_delivery() == kTitleCallback + 4);
  CHECK(registry.DispatchOwner(replacement, X_PROCTYPE_TITLE,
                               replacement->generation(), execute));
  CHECK(calls == 1);
  registry.Set({kSystemCallback, 3}, X_PROCTYPE_SYSTEM);
  CHECK_FALSE(registry.CaptureOwner(X_PROCTYPE_SYSTEM));  // No code pin.
  CHECK(registry.DispatchLegacy(execute));
}

TEST_CASE("Graphics callback pairs are atomic during concurrent registration",
          "[graphics][interrupt]") {
  Registry registry;
  constexpr uint32_t kDataMask = 0x5A3CC3A5;
  registry.Set({kTitleCallback, kTitleCallback ^ kDataMask});
  std::barrier start(3);
  std::atomic<unsigned> mixed_pairs{0};
  std::atomic<unsigned> calls{0};
  auto writer = [&](uint32_t process, uint32_t base) {
    start.arrive_and_wait();
    for (uint32_t i = 0; i < 1000; ++i) {
      const uint32_t callback = base + 4 * i;
      registry.Set({callback, callback ^ kDataMask}, process);
    }
  };
  std::thread title(writer, X_PROCTYPE_TITLE, kTitleCallback);
  std::thread system(writer, X_PROCTYPE_SYSTEM, kSystemCallback);
  start.arrive_and_wait();
  for (unsigned i = 0; i < 3000; ++i) {
    registry.DispatchLegacy([&](const auto& registration) {
      if ((registration.callback() ^ kDataMask) != registration.user_data()) {
        ++mixed_pairs;
      }
      ++calls;
    });
  }
  title.join();
  system.join();
  CHECK(mixed_pairs == 0);
  CHECK(calls > 0);
}

TEST_CASE("Graphics module shutdown drains admitted callbacks and pins code",
          "[graphics][interrupt]") {
  Registry registry;
  auto pin = std::make_shared<int>(1);
  std::weak_ptr<int> lifetime = pin;
  registry.Set({kTitleCallback, 1}, X_PROCTYPE_TITLE, kTitleModule, pin);
  auto pending = registry.CaptureOwner(X_PROCTYPE_TITLE);
  REQUIRE(pending);
  pin.reset();
  std::promise<void> entered;
  std::promise<void> release;
  auto released = release.get_future().share();
  auto active = std::async(std::launch::async, [&]() {
    return registry.DispatchOwner(pending, X_PROCTYPE_TITLE,
                                  pending->generation(), [&](const auto&) {
                                    entered.set_value();
                                    released.wait();
                                  });
  });
  entered.get_future().wait();
  auto shutdown = std::async(std::launch::async, [&]() {
    return registry.BeginModuleShutdown(kTitleModule);
  });
  // Admission retirement is observable without assuming a worker start time.
  bool stopped = false;
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (!stopped && std::chrono::steady_clock::now() < deadline) {
    stopped = !registry.DispatchOwner(
        pending, X_PROCTYPE_TITLE, pending->generation(), [](const auto&) {});
    std::this_thread::yield();
  }
  const bool waited = shutdown.wait_for(std::chrono::milliseconds(0)) ==
                      std::future_status::timeout;
  release.set_value();  // Release even if an assertion below fails.
  CHECK(stopped);
  CHECK(waited);
  CHECK(active.get());
  CHECK(shutdown.get());
  registry.RetireModule(kTitleModule);
  CHECK_FALSE(registry.CaptureOwner(X_PROCTYPE_TITLE));
  CHECK_FALSE(registry.DispatchLegacy([](const auto&) {}));
  CHECK_FALSE(lifetime.expired());  // The queued token still pins code.
  pending.reset();
  CHECK(lifetime.expired());
}

TEST_CASE("A graphics callback can request its own deferred module unload",
          "[graphics][interrupt]") {
  Registry registry;
  auto pin = std::make_shared<int>(1);
  registry.Set({kTitleCallback, 1}, X_PROCTYPE_TITLE, kTitleModule, pin);
  auto token = registry.CaptureOwner(X_PROCTYPE_TITLE);
  REQUIRE(token);
  bool self_drained = true;
  REQUIRE(registry.DispatchOwner(
      token, X_PROCTYPE_TITLE, token->generation(), [&](const auto&) {
        self_drained = registry.BeginModuleShutdown(kTitleModule);
      }));
  CHECK_FALSE(self_drained);
  CHECK_FALSE(registry.DispatchOwner(token, X_PROCTYPE_TITLE,
                                     token->generation(), [](const auto&) {}));
  CHECK(registry.BeginModuleShutdown(kTitleModule));
  registry.RetireModule(kTitleModule);
}

TEST_CASE("Graphics self-unload checks all callbacks before waiting",
          "[graphics][interrupt]") {
  Registry registry;
  auto pin = std::make_shared<int>(1);
  // Two process registrations may contain callback code from the same module.
  registry.Set({kSystemCallback, 2}, X_PROCTYPE_SYSTEM, kTitleModule, pin);
  auto other = registry.CaptureOwner(X_PROCTYPE_SYSTEM);
  registry.Set({kTitleCallback, 1}, X_PROCTYPE_TITLE, kTitleModule, pin);
  auto self = registry.CaptureOwner(X_PROCTYPE_TITLE);
  REQUIRE(other);
  REQUIRE(self);
  std::promise<void> entered;
  std::promise<void> release;
  auto released = release.get_future().share();
  auto active = std::async(std::launch::async, [&]() {
    return registry.DispatchOwner(other, X_PROCTYPE_SYSTEM, other->generation(),
                                  [&](const auto&) {
                                    entered.set_value();
                                    released.wait();
                                  });
  });
  entered.get_future().wait();
  bool self_drained = true;
  auto unloading = std::async(std::launch::async, [&]() {
    return registry.DispatchOwner(
        self, X_PROCTYPE_TITLE, self->generation(), [&](const auto&) {
          self_drained = registry.BeginModuleShutdown(kTitleModule);
        });
  });
  const bool returned_without_other =
      unloading.wait_for(std::chrono::seconds(2)) == std::future_status::ready;
  release.set_value();
  CHECK(returned_without_other);
  CHECK(unloading.get());
  CHECK(active.get());
  CHECK_FALSE(self_drained);
  CHECK(registry.BeginModuleShutdown(kTitleModule));
}

TEST_CASE("Graphics state keeps the legacy two-DWORD layout and retires tokens",
          "[graphics][interrupt][save]") {
  Registry registry;
  auto pin = std::make_shared<int>(1);
  registry.Set({kTitleCallback, 0xABCDEF12}, X_PROCTYPE_TITLE, kTitleModule,
               pin);
  const auto old = registry.CaptureOwner(X_PROCTYPE_TITLE);
  REQUIRE(old);
  std::array<uint8_t, 16> bytes{};
  ByteStream stream(bytes.data(), bytes.size());
  registry.SaveLegacy(&stream);
  CHECK(stream.offset() == 8);
  stream.Write<uint32_t>(0xAABBCCDD);  // The following CP state stays aligned.
  stream.set_offset(0);
  CHECK(stream.Read<uint32_t>() == kTitleCallback);
  CHECK(stream.Read<uint32_t>() == 0xABCDEF12);
  CHECK(stream.Read<uint32_t>() == 0xAABBCCDD);
  stream.set_offset(0);
  registry.RestoreLegacy(&stream);
  CHECK(stream.offset() == 8);
  CHECK_FALSE(registry.DispatchOwner(old, X_PROCTYPE_TITLE, old->generation(),
                                     [](const auto&) {}));
  CHECK_FALSE(registry.CaptureOwner(X_PROCTYPE_TITLE));
  uint32_t callback = 0;
  REQUIRE(registry.DispatchLegacy([&](const auto& restored) {
    callback = restored.callback();
    CHECK(restored.process_type() == Registry::kUnclassified);
    CHECK(restored.generation() != old->generation());
    CHECK(restored.user_data() == 0xABCDEF12);
  }));
  CHECK(callback == kTitleCallback);
  registry.Set({0, 0x12345678});
  CHECK(registry.legacy_pair().user_data == 0x12345678);
}

TEST_CASE("Whole graphics stop drains generations cleared before the stop",
          "[graphics][interrupt][stop]") {
  Registry registry;
  registry.Set({kTitleCallback, 1}, X_PROCTYPE_TITLE, kTitleModule,
               std::make_shared<int>(1));
  const auto token = registry.CaptureOwner(X_PROCTYPE_TITLE);
  REQUIRE(token);
  std::promise<void> entered, release;
  auto released = release.get_future().share();
  auto active = std::async(std::launch::async, [&] {
    return registry.DispatchOwner(token, X_PROCTYPE_TITLE, token->generation(),
                                  [&](const auto&) {
                                    entered.set_value();
                                    released.wait();
                                  });
  });
  entered.get_future().wait();
  registry.Clear();
  auto shutdown =
      std::async(std::launch::async, [&] { return registry.StopAndWait(); });
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (!registry.IsStopped() && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::yield();
  }
  const bool sealed = registry.IsStopped();
  const bool waited = shutdown.wait_for(std::chrono::milliseconds(0)) ==
                      std::future_status::timeout;
  registry.Set({kSystemCallback, 2}, X_PROCTYPE_SYSTEM, kSystemModule,
               std::make_shared<int>(2));
  registry.RestoreLegacy({kTitleCallback + 4, 3});
  unsigned calls = 0;
  const bool late_legacy =
      registry.DispatchLegacy([&](const auto&) { ++calls; });
  const bool late_token =
      registry.DispatchOwner(token, X_PROCTYPE_TITLE, token->generation(),
                             [&](const auto&) { ++calls; });
  release.set_value();
  CHECK(sealed);
  CHECK(waited);
  CHECK(active.get());
  CHECK(shutdown.get());
  CHECK_FALSE(late_legacy);
  CHECK_FALSE(late_token);
  CHECK(calls == 0);
  CHECK_FALSE(registry.CaptureOwner(X_PROCTYPE_SYSTEM));
  CHECK(registry.legacy_pair().callback == 0);
  CHECK(registry.StopAndWait());
}

TEST_CASE("Whole graphics self-stop seals every owner before deferring drain",
          "[graphics][interrupt][stop]") {
  Registry registry;
  auto pin = std::make_shared<int>(1);
  registry.Set({kSystemCallback, 2}, X_PROCTYPE_SYSTEM, kSystemModule, pin);
  const auto other = registry.CaptureOwner(X_PROCTYPE_SYSTEM);
  registry.Set({kTitleCallback, 1}, X_PROCTYPE_TITLE, kTitleModule, pin);
  const auto self = registry.CaptureOwner(X_PROCTYPE_TITLE);
  REQUIRE(other);
  REQUIRE(self);
  std::promise<void> entered, release;
  auto released = release.get_future().share();
  auto active = std::async(std::launch::async, [&] {
    return registry.DispatchOwner(other, X_PROCTYPE_SYSTEM, other->generation(),
                                  [&](const auto&) {
                                    entered.set_value();
                                    released.wait();
                                  });
  });
  entered.get_future().wait();
  bool self_drained = true;
  auto stopping = std::async(std::launch::async, [&] {
    return registry.DispatchOwner(
        self, X_PROCTYPE_TITLE, self->generation(),
        [&](const auto&) { self_drained = registry.StopAndWait(); });
  });
  const bool returned_without_other =
      stopping.wait_for(std::chrono::seconds(2)) == std::future_status::ready;
  release.set_value();
  CHECK(returned_without_other);
  CHECK(stopping.get());
  CHECK(active.get());
  CHECK_FALSE(self_drained);
  CHECK(registry.IsStopped());
  CHECK_FALSE(registry.DispatchOwner(other, X_PROCTYPE_SYSTEM,
                                     other->generation(), [](const auto&) {}));
  CHECK(registry.StopAndWait());
}

TEST_CASE("Owned graphics DPC context survives deferred work and restores PCR",
          "[graphics][interrupt][dpc]") {
  const uint32_t process = GENERATE(X_PROCTYPE_TITLE, X_PROCTYPE_SYSTEM);
  X_KPCR pcr{};
  pcr.current_irql = 1;
  pcr.processtype_value_in_dpc = 0x7F;
  pcr.prcb_data.current_cpu = 5;
  std::vector<uint8_t> selected;
  bool callback_ran = false;
  bool deferred_ran = false;
  DPCImpersonationScope::Run(
      pcr, process, 2,
      [&](uint8_t cpu) {
        selected.push_back(cpu);
        pcr.prcb_data.current_cpu = cpu;
      },
      [&]() {
        CHECK(pcr.current_irql == 2);
        CHECK(pcr.prcb_data.dpc_active == 1);
        CHECK(pcr.processtype_value_in_dpc == process);
        CHECK(pcr.prcb_data.current_cpu == 2);
        callback_ran = true;
      },
      [&]() {
        CHECK(callback_ran);
        CHECK(pcr.current_irql == 2);
        CHECK(pcr.prcb_data.dpc_active == 1);
        CHECK(pcr.processtype_value_in_dpc == process);
        CHECK(pcr.prcb_data.current_cpu == 2);
        deferred_ran = true;
      });
  CHECK(deferred_ran);
  CHECK(selected == std::vector<uint8_t>{2, 5});
  CHECK(pcr.current_irql == 1);
  CHECK(pcr.prcb_data.dpc_active == 0);
  CHECK(pcr.processtype_value_in_dpc == 0x7F);
  CHECK(pcr.prcb_data.current_cpu == 5);
}

}  // namespace
}  // namespace xe::kernel::test
