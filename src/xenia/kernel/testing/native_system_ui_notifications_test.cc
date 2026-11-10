/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia contributors. All rights reserved.                    *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */
#include <array>
#include <condition_variable>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

#include "third_party/catch/include/catch.hpp"
#include "xenia/kernel/native_system_ui_notifications.h"

namespace xe::kernel::test {
class NativeSystemUiNotificationTestAccess {
 public:
  using Observer = NativeSystemUiNotifications;
  static std::shared_ptr<Observer> Queue(
      std::function<void(const NativeSystemUiVisibilityEvent&)> deliver,
      std::function<void()> revoked = {}) {
    return Observer::CreateQueueForTest(std::move(deliver), std::move(revoked));
  }
  static Observer::UnlockObservation Reserve(
      const std::shared_ptr<Observer>& observer, bool visible,
      std::shared_ptr<void> lifetime = {}) {
    // Only host ordering/rundown is modeled. No Kernel, native body, module,
    // guest listener, foreground runtime or GPU authority is represented.
    NativeSystemUiVisibilityEvent event;
    event.visible_ = visible;
    return observer->Reserve(std::move(event), std::move(lifetime));
  }
};
namespace {
using Access = NativeSystemUiNotificationTestAccess;

TEST_CASE("Native visibility delivery follows native lock reservation order",
          "[native_notify][host_transport]") {
  std::vector<std::pair<uint64_t, bool>> events;
  auto observer = Access::Queue([&](const auto& event) {
    events.emplace_back(event.sequence(), event.visible());
  });
  auto first = Access::Reserve(observer, true);
  auto second = Access::Reserve(observer, false);
  second.Complete();
  CHECK(events.empty());
  CHECK(observer->snapshot().pending == 2);
  first.Complete();
  REQUIRE(events.size() == 2);
  CHECK(events[0] == std::pair<uint64_t, bool>{1, true});
  CHECK(events[1] == std::pair<uint64_t, bool>{2, false});
  first.Complete();
  second.Complete();
  CHECK(events.size() == 2);
  CHECK(observer->snapshot().pending == 0);
  CHECK(observer->snapshot().last_sequence == 2);
  CHECK_FALSE(observer->snapshot().visible);
}

TEST_CASE("Native visibility publishes state before entering host sinks",
          "[native_notify][host_transport]") {
  std::shared_ptr<NativeSystemUiNotifications> observer;
  bool saw_current = false;
  observer = Access::Queue([&](const auto& event) {
    const auto state = observer->snapshot();
    saw_current = state.known && state.visible == event.visible() &&
                  state.last_sequence == event.sequence();
  });
  auto first = Access::Reserve(observer, true);
  CHECK_FALSE(observer->snapshot().known);
  first.Complete();
  CHECK(saw_current);
}

TEST_CASE("Native visibility ring is bounded and never replays overflow",
          "[native_notify][host_transport]") {
  size_t delivered = 0, revoked = 0;
  auto observer =
      Access::Queue([&](const auto&) { ++delivered; }, [&] { ++revoked; });
  std::array<NativeSystemUiNotifications::UnlockObservation, 64> held;
  for (auto& entry : held) {
    entry = Access::Reserve(observer, true);
  }
  CHECK(observer->snapshot().pending == 64);
  auto overflow = Access::Reserve(observer, false);
  CHECK(revoked == 0);
  overflow.Complete();
  CHECK(revoked == 1);
  observer->BeginStop();
  CHECK(revoked == 1);
  CHECK(observer->snapshot().pending == 64);
  CHECK(observer->snapshot().poisoned);
  for (auto& entry : held) {
    entry.Complete();
  }
  CHECK(observer->snapshot().pending == 0);
  CHECK(delivered == 0);
  CHECK_FALSE(observer->Drain());
}

TEST_CASE("Abandoned native unlock cannot publish it or later visibility",
          "[native_notify][host_transport]") {
  size_t delivered = 0;
  auto observer = Access::Queue([&](const auto&) { ++delivered; });
  auto lifetime = std::make_shared<int>(42);
  std::weak_ptr<int> weak = lifetime;
  {
    auto first = Access::Reserve(observer, true, lifetime);
    auto second = Access::Reserve(observer, false);
    lifetime.reset();
    second.Complete();
    CHECK_FALSE(weak.expired());
  }
  CHECK(weak.expired());
  CHECK(observer->snapshot().poisoned);
  CHECK(observer->snapshot().pending == 0);
  CHECK(delivered == 0);
  CHECK_FALSE(observer->Drain());
}

TEST_CASE("Native visibility partial sink failure is never retried",
          "[native_notify][host_transport]") {
  size_t attempts = 0;
  auto observer = Access::Queue([&](const auto&) {
    ++attempts;
    throw std::runtime_error("host sink failed");
  });
  auto first = Access::Reserve(observer, true);
  auto second = Access::Reserve(observer, false);
  second.Complete();
  first.Complete();
  first.Complete();
  CHECK(attempts == 1);
  CHECK(observer->snapshot().poisoned);
  CHECK(observer->snapshot().pending == 0);
}

TEST_CASE("Native visibility stop retains admitted callback resources",
          "[native_notify][host_transport]") {
  std::mutex mutex;
  std::condition_variable changed;
  bool entered = false, release = false;
  auto lifetime = std::make_shared<int>(42);
  std::weak_ptr<int> weak = lifetime;
  auto observer = Access::Queue([&](const auto&) {
    std::unique_lock lock(mutex);
    entered = true;
    changed.notify_all();
    changed.wait(lock, [&] { return release; });
  });
  auto first = Access::Reserve(observer, true, lifetime);
  lifetime.reset();
  std::thread delivery(
      [token = std::move(first)]() mutable { token.Complete(); });
  {
    std::unique_lock lock(mutex);
    changed.wait(lock, [&] { return entered; });
  }
  observer->BeginStop();
  CHECK_FALSE(weak.expired());
  CHECK(observer->snapshot().pending == 1);
  auto rejected = Access::Reserve(observer, false);
  rejected.Complete();
  CHECK(observer->snapshot().pending == 1);
  {
    std::lock_guard lock(mutex);
    release = true;
  }
  changed.notify_all();
  CHECK(observer->Drain());
  delivery.join();
  CHECK(weak.expired());
  CHECK(observer->snapshot().pending == 0);
}
}  // namespace
}  // namespace xe::kernel::test
