/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia contributors. All rights reserved.                    *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <atomic>
#include <memory>
#include <thread>
#include <vector>

#include "xenia/emulator_lifecycle.h"

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

namespace xe::kernel::test {
// Exercises the actual production host queue. This opaque owner is never
// dereferenced and supplies no Emulator/native execution or drain evidence.
class EmulatorLifecycleTestAccess {
 public:
  static bool Install(EmulatorLifecycle& queue) {
    return queue.Install(reinterpret_cast<Emulator*>(uintptr_t(1)));
  }
  static bool Bind(EmulatorLifecycle& queue) { return queue.BindWorker(); }
  static auto Take(EmulatorLifecycle& queue, uint64_t generation = 0) {
    return queue.Take(generation);
  }
  static void Reap(EmulatorLifecycle& queue) {
    auto retired = queue.RetirePendingOnWorker();
    retired.clear();
  }
  static bool Execute(EmulatorLifecycle& queue, EmulatorLifecycle::Work& work) {
    return queue.Execute(work);
  }
  static bool Finish(EmulatorLifecycle& queue, EmulatorLifecycle::Work& work) {
    return queue.Finish(work);
  }
  static bool RecordHostShutdownReturn(EmulatorLifecycle& queue,
                                       EmulatorLifecycle::Work& work) {
    return queue.MarkShutdownReturned(
        work, reinterpret_cast<Emulator*>(uintptr_t(1)));
  }
  static bool Approve(EmulatorLifecycle& queue) {
    return queue.ApproveFinalUiClose();
  }
  static bool Failed(EmulatorLifecycle& queue) { return queue.Failed(); }
  static bool PreparedWorker(EmulatorLifecycle& queue, uint64_t generation) {
    return queue.IsActivePreparedWorker(
        reinterpret_cast<Emulator*>(uintptr_t(1)), generation);
  }
};
namespace {
using Lifecycle = EmulatorLifecycle;
using Access = EmulatorLifecycleTestAccess;
TEST_CASE(
    "Lifecycle replacement retires copied pending work on the bound host "
    "worker",
    "[title][lifecycle][host-queue]") {
  Lifecycle queue([] {});
  REQUIRE(Access::Install(queue));
  REQUIRE(Access::Bind(queue));
  const auto worker = std::this_thread::get_id();
  std::thread::id destroyed;
  struct Payload {
    Lifecycle* queue;
    std::thread::id* destroyed;
    ~Payload() {
      (void)queue->closing();
      *destroyed = std::this_thread::get_id();
    }
  };
  auto payload = std::make_unique<Payload>();
  payload->queue = &queue;
  payload->destroyed = &destroyed;
  Lifecycle::Request first;
  first.SetWork([owned = std::move(payload)] {});
  REQUIRE(queue.Submit(std::move(first)));
  std::vector<uint8_t> original{1, 0, 3};
  std::vector<uint8_t> observed;
  Lifecycle::Request second;
  second.SetWork([copy = original, &observed] { observed = copy; });
  bool accepted = false;
  std::thread producer([&] { accepted = queue.Submit(std::move(second)); });
  producer.join();
  REQUIRE(accepted);
  original.assign(1, 9);
  CHECK(destroyed == std::thread::id{});
  Access::Reap(queue);
  CHECK(destroyed == worker);
  auto work = Access::Take(queue);
  REQUIRE(work);
  REQUIRE(Access::Execute(queue, *work));
  CHECK((observed == std::vector<uint8_t>{1, 0, 3}));
  REQUIRE(Access::Finish(queue, *work));
  work.reset();
}
TEST_CASE(
    "Early user close preserves active work and waits for the real host "
    "completion cut",
    "[title][lifecycle][host-queue]") {
  Lifecycle queue([] {});
  REQUIRE(Access::Install(queue));
  REQUIRE(Access::Bind(queue));
  Lifecycle::Request launch;
  int calls = 0;
  launch.SetWork([&] { ++calls; });
  REQUIRE(queue.Submit(std::move(launch)));
  auto active = Access::Take(queue);
  REQUIRE(active);
  CHECK(queue.DeferUserClose());
  CHECK(queue.DeferUserClose());
  Lifecycle::Request rejected;
  CHECK_FALSE(queue.Submit(std::move(rejected)));
  CHECK_FALSE(Access::Take(queue));
  CHECK_FALSE(Access::Approve(queue));
  REQUIRE(Access::Execute(queue, *active));
  CHECK(calls == 1);
  REQUIRE(Access::Finish(queue, *active));
  active.reset();
  auto quit = Access::Take(queue);
  REQUIRE(quit);
  CHECK_FALSE(Access::Execute(queue, *quit));
  CHECK_FALSE(Access::Finish(queue, *quit));
  CHECK_FALSE(Access::Approve(queue));
  CHECK(queue.DeferUserClose());
  // This fixture tests only the private host-return acknowledgment contract.
  // Native stop/GPU/title completion are never inferred or simulated here.
  REQUIRE(Access::RecordHostShutdownReturn(queue, *quit));
  REQUIRE(Access::Finish(queue, *quit));
  quit.reset();
  REQUIRE(Access::Approve(queue));
  CHECK_FALSE(queue.DeferUserClose());
}
TEST_CASE(
    "Lifecycle preparation admission is stop waitable and closes before "
    "teardown",
    "[title][lifecycle][host-queue]") {
  std::atomic<unsigned> wakes{0};
  Lifecycle queue([&] { ++wakes; });
  REQUIRE(Access::Install(queue));
  REQUIRE(Access::Bind(queue));
  {
    auto preparation = queue.AdmitProducer();
    REQUIRE(preparation);
    REQUIRE(queue.DeferUserClose());
    CHECK_FALSE(queue.AdmitProducer());
    CHECK_FALSE(Access::Take(queue));
    CHECK_FALSE(Access::Approve(queue));
  }
  CHECK(wakes.load() >= 2);
  auto quit = Access::Take(queue);
  REQUIRE(quit);
  REQUIRE(Access::RecordHostShutdownReturn(queue, *quit));
  REQUIRE(Access::Finish(queue, *quit));
  quit.reset();
}
TEST_CASE(
    "Stale lifecycle handoff cannot close a newer title or execute on "
    "another worker",
    "[title][lifecycle][host-queue]") {
  Lifecycle queue([] {});
  REQUIRE(Access::Install(queue));
  REQUIRE(Access::Bind(queue));
  Lifecycle::Request stale;
  stale.kind = Lifecycle::Kind::kQuit;
  stale.expected_generation = 7;
  REQUIRE(queue.Submit(std::move(stale)));
  CHECK(queue.closing());
  CHECK_FALSE(Access::Take(queue, 8));
  CHECK_FALSE(queue.closing());
  Access::Reap(queue);
  Lifecycle::Request next;
  int calls = 0;
  next.SetWork([&] { ++calls; });
  REQUIRE(queue.Submit(std::move(next)));
  bool foreign_took = false, foreign_bound = false;
  std::thread foreign([&] {
    foreign_took = bool(Access::Take(queue));
    foreign_bound = Access::Bind(queue);
  });
  foreign.join();
  CHECK_FALSE(foreign_took);
  CHECK_FALSE(foreign_bound);
  auto work = Access::Take(queue);
  REQUIRE(work);
  CHECK_FALSE(queue.AdmitProducer());
  REQUIRE(Access::Execute(queue, *work));
  REQUIRE(Access::Finish(queue, *work));
  work.reset();
  CHECK(calls == 1);
}
TEST_CASE(
    "Abandoned active lifecycle work never becomes successful shutdown "
    "evidence",
    "[title][lifecycle][host-queue]") {
  Lifecycle queue([] {});
  REQUIRE(Access::Install(queue));
  REQUIRE(Access::Bind(queue));
  Lifecycle::Request request;
  request.SetWork([] {});
  REQUIRE(queue.Submit(std::move(request)));
  {
    auto abandoned = Access::Take(queue);
    REQUIRE(abandoned);
  }
  CHECK(Access::Failed(queue));
  CHECK_FALSE(Access::Approve(queue));
  CHECK(queue.DeferUserClose());
  CHECK_FALSE(queue.final_ui_approved());
}
}  // namespace
}  // namespace xe::kernel::test

namespace xe::kernel::test {
namespace {
TEST_CASE(
    "Prepared lifecycle publication preserves payloads and waits for producer return",
    "[title][lifecycle][host-queue][prepared]") {
  std::atomic<unsigned> wakes{0};
  Lifecycle queue([&] { ++wakes; });
  REQUIRE(Access::Install(queue));
  REQUIRE(Access::Bind(queue));
  const auto worker = std::this_thread::get_id();
  std::thread::id retired_on;
  struct Payload {
    Lifecycle* queue;
    std::thread::id* retired_on;
    ~Payload() {
      (void)queue->closing();  // Destruction must occur outside its mutex.
      *retired_on = std::this_thread::get_id();
    }
  };
  auto old = std::make_unique<Payload>();
  old->queue = &queue; old->retired_on = &retired_on;
  Lifecycle::Request pending;
  pending.SetWork([owned = std::move(old)] {});
  REQUIRE(queue.Submit(std::move(pending)));
  std::vector<uint8_t> bytes{1, 0, 0xFF, 3}, observed;
  const auto expected = bytes;
  const auto before_publish = wakes.load();
  {
    auto producer = queue.AdmitProducer();
    REQUIRE(producer);
    Lifecycle::Request request;
    request.kind = Lifecycle::Kind::kReplace;
    request.expected_generation = 7;
    request.SetWork([copy = bytes, &observed] { observed = copy; });
    auto prepared = queue.Prepare(std::move(request), producer);
    REQUIRE(prepared);
    REQUIRE(*prepared);
    bytes.assign(1, 9);
    bool published = false;
    std::thread publisher([&] {
      published = queue.PublishPreparedWithoutWake(*prepared, producer);
    });
    publisher.join();
    REQUIRE(published);
    CHECK_FALSE(*prepared);
    CHECK_FALSE(queue.PublishPreparedWithoutWake(*prepared, producer));
    CHECK(wakes.load() == before_publish);
    CHECK(retired_on == std::thread::id{});
    CHECK_FALSE(Access::Take(queue, 7));
    CHECK_FALSE(Access::PreparedWorker(queue, 7));
  }
  // Only the actual Producer release wakes the already-existing worker.
  CHECK(wakes.load() == before_publish + 1);
  Access::Reap(queue);
  CHECK(retired_on == worker);
  auto work = Access::Take(queue, 7);
  REQUIRE(work);
  CHECK(Access::PreparedWorker(queue, 7));
  CHECK_FALSE(Access::PreparedWorker(queue, 8));
  bool foreign = true;
  std::thread other([&] { foreign = Access::PreparedWorker(queue, 7); });
  other.join();
  CHECK_FALSE(foreign);
  REQUIRE(Access::Execute(queue, *work));
  CHECK(observed == expected);
  REQUIRE(Access::Finish(queue, *work));
  work.reset();
  CHECK_FALSE(Access::PreparedWorker(queue, 7));
}

TEST_CASE(
    "Prepared lifecycle requests require the same live producer across moves",
    "[title][lifecycle][host-queue][prepared]") {
  Lifecycle queue([] {}), other([] {});
  REQUIRE(Access::Install(queue)); REQUIRE(Access::Bind(queue));
  REQUIRE(Access::Install(other)); REQUIRE(Access::Bind(other));
  unsigned calls = 0;
  {
    auto producer = queue.AdmitProducer();
    auto another = queue.AdmitProducer();
    auto foreign = other.AdmitProducer();
    REQUIRE(producer); REQUIRE(another); REQUIRE(foreign);
    Lifecycle::Request request;
    request.kind = Lifecycle::Kind::kReplace;
    request.expected_generation = 9;
    request.SetWork([&] { ++calls; });
    auto prepared = queue.Prepare(std::move(request), producer);
    REQUIRE(prepared);
    CHECK_FALSE(queue.PublishPreparedWithoutWake(*prepared, another));
    CHECK_FALSE(queue.PublishPreparedWithoutWake(*prepared, foreign));
    CHECK_FALSE(other.PublishPreparedWithoutWake(*prepared, foreign));
    CHECK(*prepared);
    auto moved = std::move(producer);
    CHECK_FALSE(producer);
    CHECK_FALSE(queue.PublishPreparedWithoutWake(*prepared, producer));
    REQUIRE(queue.PublishPreparedWithoutWake(*prepared, moved));
    CHECK_FALSE(queue.PublishPreparedWithoutWake(*prepared, moved));
    CHECK_FALSE(Access::Take(queue, 9));
  }
  auto work = Access::Take(queue, 9);
  REQUIRE(work);
  REQUIRE(Access::Execute(queue, *work));
  REQUIRE(Access::Finish(queue, *work));
  work.reset();
  CHECK(calls == 1);
  CHECK_FALSE(Access::Take(other, 9));
  CHECK_FALSE(Access::Failed(queue));
  CHECK_FALSE(Access::Failed(other));
}

TEST_CASE(
    "Prepared lifecycle publication rejects closure and untyped worker work",
    "[title][lifecycle][host-queue][prepared]") {
  Lifecycle queue([] {});
  REQUIRE(Access::Install(queue)); REQUIRE(Access::Bind(queue));
  unsigned calls = 0;
  {
    auto producer = queue.AdmitProducer();
    REQUIRE(producer);
    for (unsigned invalid = 0; invalid != 5; ++invalid) {
      Lifecycle::Request request;
      request.kind = invalid == 0 ? Lifecycle::Kind::kLaunch
                                  : Lifecycle::Kind::kReplace;
      request.expected_generation = invalid == 1 ? 0 : 11;
      if (invalid != 4) { request.SetWork([&] { ++calls; }); }
      if (invalid == 2) { request.prepare_ui = [] {}; }
      if (invalid == 3) { request.after_shutdown_ui = [] { return true; }; }
      CHECK_FALSE(queue.Prepare(std::move(request), producer));
    }
  }
  SECTION("A user quit wins before publication") {
    auto producer = queue.AdmitProducer();
    REQUIRE(producer);
    Lifecycle::Request request;
    request.kind = Lifecycle::Kind::kReplace;
    request.expected_generation = 11;
    request.SetWork([&] { ++calls; });
    auto prepared = queue.Prepare(std::move(request), producer);
    REQUIRE(prepared);
    REQUIRE(queue.DeferUserClose());
    CHECK_FALSE(queue.PublishPreparedWithoutWake(*prepared, producer));
    CHECK(*prepared);
    CHECK_FALSE(Access::Take(queue, 11));
    CHECK_FALSE(Access::Approve(queue));
    CHECK(calls == 0);
    // No native shutdown is acknowledged or simulated by this queue fixture.
  }
  SECTION("An ordinary replacement is not a prepared system handoff") {
    Lifecycle::Request request;
    request.kind = Lifecycle::Kind::kReplace;
    request.expected_generation = 11;
    request.SetWork([&] { ++calls; });
    REQUIRE(queue.Submit(std::move(request)));
    auto work = Access::Take(queue, 11);
    REQUIRE(work);
    CHECK_FALSE(Access::PreparedWorker(queue, 11));
    REQUIRE(Access::Execute(queue, *work));
    REQUIRE(Access::Finish(queue, *work));
    work.reset();
    CHECK(calls == 1);
  }
}
}  // namespace
}  // namespace xe::kernel::test
