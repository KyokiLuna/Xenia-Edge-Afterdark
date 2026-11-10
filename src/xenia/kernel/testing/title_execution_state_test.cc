/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia contributors. All rights reserved.                    *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <atomic>
#include <thread>

#include "third_party/catch/include/catch.hpp"
#include "xenia/kernel/title_execution_state.h"

namespace xe::kernel::test {
namespace {

using State = TitleExecutionState;
constexpr uint32_t kTitle = 0x12345678;
constexpr uint32_t kModule = 0x81001000;
constexpr uint32_t kThread = 0x10001;

TEST_CASE("Title execution is inactive until its exact prepared thread starts",
          "[title][lifecycle]") {
  State state;
  CHECK(state.snapshot() == State::Snapshot{});
  CHECK(state.active_title_id() == 0);
  CHECK(state.BeginStopping() == 0);
  CHECK_FALSE(state.CompleteStop(0));
  CHECK_FALSE(state.CancelPreparing(0));

  const uint64_t generation = state.BeginPreparing();
  REQUIRE(generation != 0);
  CHECK(state.BeginPreparing() == 0);
  CHECK_FALSE(state.MarkRunning(generation, kModule, kThread));
  CHECK_FALSE(state.SetPreparedIdentity(0, kTitle, kModule, kThread));
  CHECK_FALSE(state.SetPreparedIdentity(generation, kTitle, 0, kThread));
  CHECK_FALSE(state.SetPreparedIdentity(generation, kTitle, kModule, 0));
  REQUIRE(state.SetPreparedIdentity(generation, kTitle, kModule, kThread));
  // Loaded metadata, launch notification, and a suspended main thread still
  // belong to Preparing. None exposes an active title.
  CHECK(state.snapshot().phase == State::Phase::kPreparing);
  CHECK(state.active_title_id() == 0);
  CHECK_FALSE(state.MarkRunning(generation + 1, kModule, kThread));
  CHECK_FALSE(state.MarkRunning(generation, kModule + 4, kThread));
  CHECK_FALSE(state.MarkRunning(generation, kModule, kThread + 1));
  REQUIRE(state.MarkRunning(generation, kModule, kThread));
  CHECK(state.active_title_id() == kTitle);
  CHECK(state.active_title_id(generation) == kTitle);
  CHECK(state.active_title_id(generation + 1) == 0);
  CHECK_FALSE(state.MarkRunning(generation, kModule, kThread));
}

TEST_CASE("A prepared title identity cannot be replaced within its generation",
          "[title][lifecycle]") {
  State state;
  const uint64_t generation = state.BeginPreparing();
  REQUIRE(state.SetPreparedIdentity(generation, kTitle, kModule, kThread));
  const auto prepared = state.snapshot();
  CHECK(state.SetPreparedIdentity(generation, kTitle, kModule, kThread));
  CHECK_FALSE(
      state.SetPreparedIdentity(generation, kTitle + 1, kModule, kThread));
  CHECK_FALSE(
      state.SetPreparedIdentity(generation, kTitle, kModule + 4, kThread));
  CHECK_FALSE(
      state.SetPreparedIdentity(generation, kTitle, kModule, kThread + 1));
  CHECK(state.snapshot() == prepared);
}

TEST_CASE("Stopping revokes title access before teardown completes",
          "[title][lifecycle]") {
  State state;
  const uint64_t generation = state.BeginPreparing();
  REQUIRE(state.SetPreparedIdentity(generation, kTitle, kModule, kThread));
  REQUIRE(state.MarkRunning(generation, kModule, kThread));
  CHECK_FALSE(state.CancelPreparing(generation));
  CHECK_FALSE(state.CompleteStop(generation));
  CHECK(state.BeginStopping(generation + 1) == 0);
  CHECK(state.active_title_id() == kTitle);
  REQUIRE(state.BeginStopping(generation) == generation);
  CHECK(state.BeginStopping() == generation);
  CHECK(state.BeginPreparing() == 0);
  CHECK(state.snapshot().hmodule == kModule);
  CHECK(state.active_title_id() == 0);
  CHECK_FALSE(state.MarkRunning(generation, kModule, kThread));
  CHECK_FALSE(state.CompleteStop(generation + 1));
  REQUIRE(state.CompleteStop(generation));
  const auto stopped = state.snapshot();
  CHECK(stopped.generation == generation);
  CHECK(stopped.phase == State::Phase::kUnloaded);
  CHECK(stopped.title_id == 0);
  CHECK(stopped.hmodule == 0);
  CHECK(stopped.main_thread_id == 0);
  CHECK_FALSE(state.CompleteStop(generation));
  CHECK(state.BeginStopping(generation) == 0);
}

TEST_CASE("Failed preparation invalidates delayed startup and permits retry",
          "[title][lifecycle]") {
  State state;
  const uint64_t first = state.BeginPreparing();
  SECTION("Failure before a main thread exists") {
    REQUIRE(state.CancelPreparing(first));
  }
  SECTION("Failure after the suspended main thread was assigned") {
    REQUIRE(state.SetPreparedIdentity(first, kTitle, kModule, kThread));
    REQUIRE(state.CancelPreparing(first));
  }
  SECTION("Teardown starts before the main thread is dispatched") {
    REQUIRE(state.SetPreparedIdentity(first, kTitle, kModule, kThread));
    REQUIRE(state.BeginStopping(first) == first);
    CHECK_FALSE(state.CancelPreparing(first));
    CHECK_FALSE(state.MarkRunning(first, kModule, kThread));
    REQUIRE(state.CompleteStop(first));
  }
  CHECK_FALSE(state.MarkRunning(first, kModule, kThread));
  const uint64_t second = state.BeginPreparing();
  REQUIRE(second > first);
  REQUIRE(state.SetPreparedIdentity(second, kTitle, kModule, kThread));
  CHECK_FALSE(state.CancelPreparing(first));
  CHECK(state.BeginStopping(first) == 0);
  CHECK_FALSE(state.CompleteStop(first));
  CHECK_FALSE(state.MarkRunning(first, kModule, kThread));
  REQUIRE(state.MarkRunning(second, kModule, kThread));
  CHECK(state.active_title_id(first) == 0);
  CHECK(state.active_title_id(second) == kTitle);
}

TEST_CASE("A new title epoch rejects a stopped kernel's reused addresses",
          "[title][lifecycle]") {
  State state;
  const uint64_t old = state.BeginPreparing();
  REQUIRE(state.SetPreparedIdentity(old, kTitle, kModule, kThread));
  REQUIRE(state.MarkRunning(old, kModule, kThread));
  REQUIRE(state.BeginStopping() == old);
  REQUIRE(state.CompleteStop(old));
  const uint64_t current = state.BeginPreparing();
  REQUIRE(current > old);
  REQUIRE(state.SetPreparedIdentity(current, kTitle, kModule, kThread));
  CHECK_FALSE(state.MarkRunning(old, kModule, kThread));
  CHECK(state.BeginStopping(old) == 0);
  REQUIRE(state.MarkRunning(current, kModule, kThread));
  CHECK(state.active_title_id(old) == 0);
  CHECK(state.active_title_id(current) == kTitle);
}

TEST_CASE("Missing execution info does not prevent an identified title epoch",
          "[title][lifecycle]") {
  State state;
  const uint64_t generation = state.BeginPreparing();
  REQUIRE(state.SetPreparedIdentity(generation, 0, kModule, kThread));
  REQUIRE(state.MarkRunning(generation, kModule, kThread));
  CHECK(state.snapshot().phase == State::Phase::kRunning);
  CHECK(state.active_title_id() == 0);
  REQUIRE(state.BeginStopping(generation) == generation);
  REQUIRE(state.CompleteStop(generation));
}

TEST_CASE("A racing title startup cannot reopen stopping admission",
          "[title][lifecycle]") {
  for (unsigned attempt = 0; attempt < 32; ++attempt) {
    State state;
    const uint64_t generation = state.BeginPreparing();
    REQUIRE(state.SetPreparedIdentity(generation, kTitle, kModule, kThread));
    std::atomic<bool> ready{false};
    std::atomic<bool> go{false};
    std::thread startup([&] {
      ready.store(true, std::memory_order_release);
      while (!go.load(std::memory_order_acquire)) {
        std::this_thread::yield();
      }
      state.MarkRunning(generation, kModule, kThread);
    });
    while (!ready.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }
    go.store(true, std::memory_order_release);
    const uint64_t stopped = state.BeginStopping(generation);
    startup.join();
    CHECK(stopped == generation);
    CHECK(state.snapshot().phase == State::Phase::kStopping);
    CHECK(state.active_title_id() == 0);
    CHECK_FALSE(state.MarkRunning(generation, kModule, kThread));
    REQUIRE(state.CompleteStop(generation));
  }
}

}  // namespace
}  // namespace xe::kernel::test
