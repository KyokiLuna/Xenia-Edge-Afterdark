/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia contributors. All rights reserved.                    *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <array>
#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>

#include "third_party/catch/include/catch.hpp"
#include "xenia/gpu/command_processor_callback_queue.h"

namespace xe::gpu::test {
namespace {

TEST_CASE("CP callback queue preserves FIFO and owner-thread reentrancy",
          "[graphics][callback_queue]") {
  CommandProcessorCallbackQueue queue;
  std::vector<unsigned> order;
  SECTION("No older queued callback permits nested inline execution") {
    queue.Submit(
        [&] {
          order.push_back(1);
          queue.Submit([&] { order.push_back(2); }, true);
          order.push_back(3);
        },
        true);
    CHECK(order == std::vector<unsigned>{1, 2, 3});
    CHECK_FALSE(queue.HasPending());
    CHECK_FALSE(queue.RunOne());
  }
  SECTION("An older callback keeps nested submissions behind it") {
    queue.Submit(
        [&] {
          order.push_back(1);
          queue.Submit([&] { order.push_back(4); }, true);
          order.push_back(2);
        },
        false);
    queue.Submit([&] { order.push_back(3); }, false);
    REQUIRE(queue.RunOne());
    CHECK(order == std::vector<unsigned>{1, 2});
    REQUIRE(queue.HasPending());
    REQUIRE(queue.RunOne());
    REQUIRE(queue.RunOne());
    CHECK(order == std::vector<unsigned>{1, 2, 3, 4});
    CHECK_FALSE(queue.HasPending());
    CHECK_FALSE(queue.RunOne());
  }
}

TEST_CASE("CP callbacks and capture destruction run outside the queue lock",
          "[graphics][callback_queue]") {
  CommandProcessorCallbackQueue queue;
  unsigned destroyed = 0;
  unsigned cleanup_callbacks = 0;
  auto resource = std::shared_ptr<int>(new int(1), [&](int* value) {
    // This would deadlock if callback destruction retained the queue mutex.
    queue.Submit([&] { ++cleanup_callbacks; }, false);
    ++destroyed;
    delete value;
  });
  bool owner_thread = false;
  SECTION("Queued capture") {}
  SECTION("Inline capture") { owner_thread = true; }
  queue.Submit([owned = std::move(resource)] { (void)owned; }, owner_thread);
  if (!owner_thread) {
    CHECK(destroyed == 0);
    REQUIRE(queue.RunOne());
  }
  CHECK(destroyed == 1);
  CHECK(cleanup_callbacks == 0);
  REQUIRE(queue.HasPending());
  REQUIRE(queue.RunOne());
  CHECK(cleanup_callbacks == 1);
  CHECK_FALSE(queue.HasPending());
}

TEST_CASE("CP callback exceptions leave the queue unlocked and FIFO intact",
          "[graphics][callback_queue]") {
  CommandProcessorCallbackQueue queue;
  unsigned callbacks = 0;
  SECTION("Queued throwing callback") {
    queue.Submit([] { throw std::runtime_error("callback"); }, false);
    queue.Submit([&] { ++callbacks; }, false);
    CHECK_THROWS_AS(queue.RunOne(), std::runtime_error);
    REQUIRE(queue.HasPending());
    REQUIRE(queue.RunOne());
  }
  SECTION("Inline throwing callback") {
    CHECK_THROWS_AS(
        queue.Submit([] { throw std::runtime_error("callback"); }, true),
        std::runtime_error);
    queue.Submit([&] { ++callbacks; }, true);
  }
  SECTION("Empty callback remains an invocation error") {
    queue.Submit({}, false);
    CHECK_THROWS_AS(queue.RunOne(), std::bad_function_call);
    queue.Submit([&] { ++callbacks; }, true);
  }
  CHECK(callbacks == 1);
  CHECK_FALSE(queue.HasPending());
  CHECK_FALSE(queue.RunOne());
}

TEST_CASE("Joined CP teardown discards captures outside live queue state locks",
          "[graphics][callback_queue]") {
  auto queue = std::make_unique<CommandProcessorCallbackQueue>();
  auto* live_during_destruction = queue.get();
  unsigned callbacks = 0;
  unsigned destroyed = 0;
  bool discarded_queue_was_empty = false;
  auto resource = std::shared_ptr<int>(new int(1), [&](int* value) {
    discarded_queue_was_empty = !live_during_destruction->HasPending();
    // The queue is alive during capture disposal, but cleanup must not start
    // another GPU callback after the worker and its backend have stopped.
    live_during_destruction->Submit([&] { ++callbacks; }, true);
    ++destroyed;
    delete value;
  });
  queue->Submit(
      [&, owned = std::move(resource)] {
        (void)owned;
        ++callbacks;
      },
      false);
  queue.reset();
  CHECK(destroyed == 1);
  CHECK(discarded_queue_was_empty);
  CHECK(callbacks == 0);
}

TEST_CASE("Concurrent CP producers preserve every callback and producer order",
          "[graphics][callback_queue]") {
  CommandProcessorCallbackQueue queue;
  constexpr unsigned kProducers = 4;
  constexpr unsigned kPerProducer = 256;
  std::array<std::vector<unsigned>, kProducers> received;
  std::atomic<bool> start{false};
  std::atomic<bool> observed_pending{false};
  std::atomic<bool> consumer_done{false};
  std::atomic<bool> observed_idle{false};
  std::atomic<bool> stop_observer{false};
  std::atomic<unsigned> producers_left{kProducers};
  unsigned seed_calls = 0;
  queue.Submit([&] { ++seed_calls; }, false);
  std::thread observer([&] {
    while (!stop_observer.load(std::memory_order_acquire)) {
      if (queue.HasPending()) {
        observed_pending.store(true, std::memory_order_release);
      } else if (consumer_done.load(std::memory_order_acquire)) {
        observed_idle.store(true, std::memory_order_release);
      }
      std::this_thread::yield();
    }
  });
  std::vector<std::thread> producers;
  for (unsigned producer = 0; producer < kProducers; ++producer) {
    producers.emplace_back([&, producer] {
      while (!start.load(std::memory_order_acquire)) {
        std::this_thread::yield();
      }
      for (unsigned sequence = 0; sequence < kPerProducer; ++sequence) {
        queue.Submit(
            [&, producer, sequence] { received[producer].push_back(sequence); },
            false);
      }
      producers_left.fetch_sub(1, std::memory_order_release);
    });
  }
  const auto observer_deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!observed_pending.load(std::memory_order_acquire) &&
         std::chrono::steady_clock::now() < observer_deadline) {
    std::this_thread::yield();
  }
  start.store(true, std::memory_order_release);
  const auto drain_deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (
      (producers_left.load(std::memory_order_acquire) || queue.HasPending()) &&
      std::chrono::steady_clock::now() < drain_deadline) {
    if (!queue.RunOne()) {
      std::this_thread::yield();
    }
  }
  for (auto& producer : producers) {
    producer.join();
  }
  consumer_done.store(true, std::memory_order_release);
  const auto idle_deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!observed_idle.load(std::memory_order_acquire) &&
         std::chrono::steady_clock::now() < idle_deadline) {
    std::this_thread::yield();
  }
  stop_observer.store(true, std::memory_order_release);
  observer.join();
  CHECK(observed_pending.load());
  CHECK(observed_idle.load());
  CHECK(seed_calls == 1);
  CHECK_FALSE(queue.HasPending());
  CHECK_FALSE(queue.RunOne());
  for (const auto& values : received) {
    REQUIRE(values.size() == kPerProducer);
    for (unsigned sequence = 0; sequence < kPerProducer; ++sequence) {
      CHECK(values[sequence] == sequence);
    }
  }
}

}  // namespace
}  // namespace xe::gpu::test
