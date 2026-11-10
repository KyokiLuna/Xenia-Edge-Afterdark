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
#include <future>
#include <memory>
#include <span>
#include <stdexcept>
#include <vector>

#include "third_party/catch/include/catch.hpp"
#include "xenia/kernel/native_frame_owner.h"
#include "xenia/memory.h"

namespace xe::kernel::test {
namespace {
using Session = NativeFrameOwnerSession;
using Phase = Session::Phase;

GuestRuntime::Plan ModelPlan() {
  return {"host-model initialization",
          {{"startup",
            {{"model entry",
              0x81001000,
              {},
              GuestRuntime::ReturnKind::kVoid,
              "host-only executor; no guest execution"}}}}};
}

class ModelGraph final : public NativeFrameGraphAuthority {
 public:
  explicit ModelGraph(Session::Owner identity) : owner(identity) {}
  ~ModelGraph() override {
    if (on_destroy) {
      on_destroy();
    }
  }
  bool Matches(const Session::Owner& identity) const override {
    if (on_match) {
      on_match();
    }
    return matches && identity == owner;
  }
  bool AbortUnsubmitted(const NativeFrameSource& source) override {
    if (on_abort) {
      on_abort();
    }
    return aborts && source.owner() == owner;
  }
  bool ValidateNativeSubmission(const NativeFrameSource& source,
                                const gpu::GpuCompletion&) const override {
    // This switch exercises the host state machine, not real CP provenance.
    return accepts_model_submission && source.owner() == owner;
  }
  bool RetireComplete(const NativeFrameSource& source,
                      const gpu::GpuCompletion&) override {
    if (on_retire) {
      on_retire();
    }
    return retires && source.owner() == owner;
  }
  bool RetireAfterKernelStop(const Session::Owner& identity) override {
    if (on_stop) {
      on_stop();
    }
    return stops && identity == owner;
  }
  const Session::Owner owner;
  bool matches = true, aborts = true, retires = true, stops = true;
  bool accepts_model_submission = false;
  std::function<void()> on_match, on_abort, on_retire, on_stop, on_destroy;
};

class Fixture {
 public:
  explicit Fixture(bool ready = true) {
    // These are opaque identity markers, never guest objects or dereferenced.
    // XThread integration supplies actual objects in production. These tests
    // cover the host admission model and never claim native/GPU execution.
    thread = {reinterpret_cast<const KernelState*>(&kernel_marker),
              reinterpret_cast<const XThread*>(&thread_marker), 7, 2};
    runtime = std::make_shared<GuestRuntime>(
        GuestRuntime::Identity{0x30001000, 9}, std::make_shared<int>(1));
    if (ready) {
      MakeReady();
    }
    session = Session::CreateProvisional(thread, 3, runtime);
    REQUIRE(session);
  }
  ~Fixture() {
    queue.ReleaseAfterGpuStopped();
    graph.reset();
    if (session) {
      session->RetireAfterKernelStop([](const auto&) { return true; });
    }
  }
  void MakeReady() {
    REQUIRE(runtime->Initialize(ModelPlan(), [](const auto&) {
      return GuestRuntime::ExecutionResult{true, 0, {}};
    }) == GuestRuntime::InitStatus::kReady);
  }
  void Bind() {
    graph = std::make_shared<ModelGraph>(session->owner());
    REQUIRE(session->BindGraph(thread, graph));
  }
  std::shared_ptr<const NativeFrameSource> Freeze() {
    auto update = session->BeginUpdate(thread);
    REQUIRE(update);
    auto source = session->Freeze(thread, update, primary);
    REQUIRE(source);
    CHECK_FALSE(update);
    return source;
  }
  int kernel_marker = 0, thread_marker = 0;
  Session::ThreadIdentity thread;
  std::shared_ptr<GuestRuntime> runtime;
  std::shared_ptr<Session> session;
  std::shared_ptr<ModelGraph> graph;
  gpu::GpuCompletionQueue queue;
  const std::array<uint8_t, 8> primary{1, 2, 3, 4, 5, 6, 7, 8};
};

TEST_CASE("Native frame owner requires exact SYSTEM and runtime identity",
          "[kernel][native_frame_owner]") {
  Fixture fixture;
  auto thread = fixture.thread;
  thread.process_type = 1;
  CHECK_FALSE(Session::CreateProvisional(thread, 3, fixture.runtime));
  thread = fixture.thread;
  thread.thread = nullptr;
  CHECK_FALSE(Session::CreateProvisional(thread, 3, fixture.runtime));
  CHECK_FALSE(Session::CreateProvisional(fixture.thread, 0, fixture.runtime));
  CHECK_FALSE(Session::CreateProvisional(fixture.thread, 3, {}));
  auto second = Session::CreateProvisional(fixture.thread, 3, fixture.runtime);
  REQUIRE(second);
  CHECK(second->owner().session_id != fixture.session->owner().session_id);
  CHECK(fixture.session->owner().module_hmodule == 0x30001000);
  CHECK(fixture.session->owner().module_generation == 9);
  CHECK(fixture.session->owner().valid());
  const auto check_wrong = [&](auto wrong) {
    CHECK_FALSE(fixture.session->MatchesThread(wrong));
    CHECK_FALSE(fixture.session->BeginUpdate(wrong));
    CHECK_FALSE(fixture.session->AllocationOwner(wrong));
  };
  thread = fixture.thread;
  ++thread.thread_id;
  check_wrong(thread);
  thread = fixture.thread;
  thread.kernel = nullptr;
  check_wrong(thread);
  thread = fixture.thread;
  thread.thread = nullptr;
  check_wrong(thread);
  thread = fixture.thread;
  thread.process_type = 1;
  check_wrong(thread);
}

TEST_CASE("Native frame provisional allocation precedes real runtime readiness",
          "[kernel][native_frame_owner]") {
  Fixture fixture(false);
  auto update = fixture.session->BeginUpdate(fixture.thread);
  REQUIRE(update);
  CHECK(fixture.session->AllocationOwner(fixture.thread) ==
        fixture.session->owner());
  auto graph = std::make_shared<ModelGraph>(fixture.session->owner());
  CHECK_FALSE(fixture.session->BindGraph(fixture.thread, graph));
  CHECK_FALSE(fixture.session->Freeze(fixture.thread, update, fixture.primary));
  CHECK(update);
  REQUIRE(fixture.runtime->Initialize(ModelPlan(), [&](const auto&) {
    CHECK(fixture.session->AllocationOwner(fixture.thread).has_value());
    return GuestRuntime::ExecutionResult{true, 0, {}};
  }) == GuestRuntime::InitStatus::kReady);
  REQUIRE(fixture.session->BindGraph(fixture.thread, graph));
  CHECK(fixture.session->GetSnapshot().phase == Phase::kBound);
  REQUIRE(fixture.session->Freeze(fixture.thread, update, fixture.primary));
  CHECK_FALSE(fixture.session->AllocationOwner(fixture.thread));
}

TEST_CASE("Native frame freeze seals bytes and consumes the exact outer update",
          "[kernel][native_frame_owner]") {
  Fixture fixture;
  fixture.Bind();
  auto update = fixture.session->BeginUpdate(fixture.thread);
  REQUIRE(update);
  CHECK_FALSE(fixture.session->BeginUpdate(fixture.thread));
  std::vector<uint8_t> bytes(fixture.primary.begin(), fixture.primary.end());
  const auto source =
      fixture.session->FreezeCurrentUpdate(fixture.thread, bytes);
  REQUIRE(source);
  bytes[0] = 0xFF;
  CHECK(source->primary()[0] == 1);
  CHECK(source->owner() == fixture.session->owner());
  CHECK(source->serial() != 0);
  CHECK_FALSE(update);
  CHECK_FALSE(fixture.session->AllocationOwner(fixture.thread));
  CHECK_FALSE(fixture.session->BeginUpdate(fixture.thread));
  CHECK_FALSE(fixture.session->AttachPhysicalPin(fixture.thread, source, {}));
  REQUIRE(fixture.session->AbortUnsubmitted(fixture.thread, source));
  CHECK(fixture.session->GetSnapshot().phase == Phase::kBound);
  CHECK_FALSE(fixture.session->AllocationOwner(fixture.thread));
  auto next = fixture.session->BeginUpdate(fixture.thread);
  REQUIRE(next);
  update.Reset();  // The consumed old guard cannot close a newer update.
  CHECK(next);
  CHECK(fixture.session->AllocationOwner(fixture.thread).has_value());
  const auto second = fixture.session->Freeze(fixture.thread, next, bytes);
  REQUIRE(second);
  CHECK(second->serial() != source->serial());
  CHECK_FALSE(fixture.session->AbortUnsubmitted(fixture.thread, source));
  CHECK(fixture.session->GetSnapshot().phase == Phase::kFrozen);
}

TEST_CASE("Native frame invalid seal does not consume the active update",
          "[kernel][native_frame_owner]") {
  Fixture fixture;
  fixture.Bind();
  auto update = fixture.session->BeginUpdate(fixture.thread);
  REQUIRE(update);
  CHECK_FALSE(fixture.session->FreezeCurrentUpdate(fixture.thread, {}));
  CHECK_FALSE(fixture.session->FreezeCurrentUpdate(
      fixture.thread, std::span(fixture.primary).first(3)));
  const std::vector<uint8_t> oversized(0x40004);
  CHECK_FALSE(fixture.session->FreezeCurrentUpdate(fixture.thread, oversized));
  CHECK(update);
  CHECK(fixture.session->AllocationOwner(fixture.thread).has_value());
  auto wrong = fixture.thread;
  ++wrong.thread_id;
  CHECK_FALSE(fixture.session->FreezeCurrentUpdate(wrong, fixture.primary));
  CHECK(update);
  Session::Update empty;
  CHECK_FALSE(fixture.session->Freeze(fixture.thread, empty, fixture.primary));
  REQUIRE(fixture.session->Freeze(fixture.thread, update, fixture.primary));
}

TEST_CASE("Native frame graph callbacks and release run outside admission lock",
          "[kernel][native_frame_owner]") {
  Fixture fixture;
  fixture.Bind();
  const auto source = fixture.Freeze();
  unsigned calls = 0;
  fixture.graph->on_abort = [&] {
    const auto snapshot = fixture.session->GetSnapshot();
    CHECK(snapshot.operation_active);
    CHECK(snapshot.phase == Phase::kFrozen);
    CHECK_FALSE(fixture.session->BeginUpdate(fixture.thread));
    ++calls;
  };
  REQUIRE(fixture.session->AbortUnsubmitted(fixture.thread, source));
  CHECK(calls == 1);
  bool destroyed = false;
  fixture.graph->on_stop = [&] {
    CHECK(fixture.session->GetSnapshot().phase == Phase::kQuarantined);
  };
  fixture.graph->on_destroy = [&] {
    CHECK(fixture.session->GetSnapshot().phase == Phase::kRetired);
    destroyed = true;
  };
  fixture.graph.reset();
  REQUIRE(fixture.session->RetireAfterKernelStop([&](const auto& owner) {
    CHECK(owner == fixture.session->owner());
    CHECK(fixture.session->GetSnapshot().operation_active);
    return true;
  }));
  CHECK(destroyed);
  CHECK_FALSE(fixture.session->BeginUpdate(fixture.thread));
}

TEST_CASE("Native frame observer and title completion cannot reopen producer",
          "[kernel][native_frame_owner]") {
  Fixture fixture;
  fixture.Bind();
  const auto source = fixture.Freeze();
  const auto title_marker = fixture.queue.TrackSubmitted(1);
  fixture.queue.CompleteThrough(1);
  fixture.session->ObserveCompletion(title_marker);
  CHECK(fixture.session->GetSnapshot().phase == Phase::kFrozen);
  CHECK_FALSE(fixture.session->BeginUpdate(fixture.thread));
  // Even completed real queue markers lack native-frame provenance by default.
  CHECK_FALSE(fixture.session->AttachGpu(fixture.thread, source, title_marker));
  CHECK(fixture.session->GetSnapshot().phase == Phase::kQuarantined);
  CHECK(fixture.session->GetSnapshot().submission_attempted);
  CHECK_FALSE(fixture.session->GetSnapshot().gpu_attached);
  CHECK_FALSE(fixture.session->AbortUnsubmitted(fixture.thread, source));
  CHECK_FALSE(fixture.session->RetireComplete(fixture.thread, source));
  fixture.session->Revoke();
  CHECK_FALSE(fixture.session->RetireAfterKernelStop(
      [](const auto&) { return false; }));
  CHECK(fixture.session->GetSnapshot().submission_attempted);
  REQUIRE(
      fixture.session->RetireAfterKernelStop([](const auto&) { return true; }));
  CHECK_FALSE(fixture.session->GetSnapshot().submission_attempted);
}

TEST_CASE("Native frame submitted model needs owner-thread explicit retirement",
          "[kernel][native_frame_owner]") {
  Fixture fixture;
  fixture.Bind();
  fixture.graph->accepts_model_submission = true;
  const auto source = fixture.Freeze();
  const auto marker = fixture.queue.TrackSubmitted(1);
  REQUIRE(fixture.session->AttachGpu(fixture.thread, source, marker));
  CHECK(fixture.session->GetSnapshot().submission_attempted);
  CHECK_FALSE(fixture.session->AbortUnsubmitted(fixture.thread, source));
  REQUIRE(fixture.session->BeginRetire(fixture.thread, source));
  CHECK_FALSE(fixture.session->RetireComplete(fixture.thread, source));
  fixture.queue.CompleteThrough(1);
  fixture.session->ObserveCompletion(marker);
  CHECK(fixture.session->GetSnapshot().completion ==
        gpu::GpuCompletionStatus::kComplete);
  CHECK(fixture.session->GetSnapshot().phase == Phase::kRetiring);
  CHECK(fixture.session->GetSnapshot().submission_attempted);
  CHECK_FALSE(fixture.session->BeginUpdate(fixture.thread));
  auto wrong = fixture.thread;
  ++wrong.thread_id;
  CHECK_FALSE(fixture.session->RetireComplete(wrong, source));
  fixture.graph->on_retire = [&] {
    CHECK(fixture.session->GetSnapshot().operation_active);
    CHECK_FALSE(fixture.session->AllocationOwner(fixture.thread));
  };
  REQUIRE(fixture.session->RetireComplete(fixture.thread, source));
  CHECK(fixture.session->GetSnapshot().phase == Phase::kBound);
  CHECK_FALSE(fixture.session->GetSnapshot().submission_attempted);
  CHECK_FALSE(fixture.session->AllocationOwner(fixture.thread));
  REQUIRE(fixture.session->BeginUpdate(fixture.thread));
}

TEST_CASE("Native frame revoked parked update needs actual stopped proof",
          "[kernel][native_frame_owner]") {
  Fixture fixture;
  fixture.Bind();
  auto parked_update = fixture.session->BeginUpdate(fixture.thread);
  REQUIRE(parked_update);
  fixture.session->Revoke();  // No stack unwinding or guard destruction needed.
  CHECK_FALSE(parked_update);
  CHECK_FALSE(fixture.session->AllocationOwner(fixture.thread));
  CHECK_FALSE(fixture.session->BeginUpdate(fixture.thread));
  CHECK_FALSE(fixture.session->RetireAfterKernelStop(
      [](const auto&) { return false; }));
  CHECK(fixture.session->GetSnapshot().phase == Phase::kQuarantined);
  const std::weak_ptr<Session> retained = fixture.session;
  fixture.session.reset();
  CHECK_FALSE(retained.expired());
  auto owner = retained.lock();
  REQUIRE(owner);
  REQUIRE(owner->RetireAfterKernelStop([](const auto&) { return true; }));
  owner.reset();
  CHECK(retained.expired());
  parked_update.Reset();
}

TEST_CASE("Native frame runtime retirement closes allocation admission",
          "[kernel][native_frame_owner]") {
  Fixture fixture;
  fixture.Bind();
  auto update = fixture.session->BeginUpdate(fixture.thread);
  REQUIRE(update);
  REQUIRE(fixture.runtime->RetireAndDrain(false) ==
          GuestRuntime::DrainStatus::kDrained);
  CHECK_FALSE(fixture.session->AllocationOwner(fixture.thread));
  CHECK_FALSE(fixture.session->Freeze(fixture.thread, update, fixture.primary));
  update.Reset();
  CHECK_FALSE(fixture.session->BeginUpdate(fixture.thread));
  CHECK(fixture.session->GetSnapshot().phase == Phase::kQuarantined);
}

TEST_CASE(
    "Native frame exception poisons admission without destructive cleanup",
    "[kernel][native_frame_owner]") {
  Fixture fixture;
  fixture.Bind();
  const auto source = fixture.Freeze();
  fixture.graph->on_abort = [] { throw std::runtime_error("uncertain graph"); };
  CHECK_THROWS_AS(fixture.session->AbortUnsubmitted(fixture.thread, source),
                  std::runtime_error);
  CHECK(fixture.session->GetSnapshot().phase == Phase::kQuarantined);
  CHECK(fixture.session->GetSnapshot().source_serial == source->serial());
  CHECK_FALSE(fixture.session->BeginUpdate(fixture.thread));
}

TEST_CASE("Native frame racing revoke cannot reopen after an admitted callback",
          "[kernel][native_frame_owner]") {
  Fixture fixture;
  fixture.Bind();
  const auto source = fixture.Freeze();
  std::promise<void> entered, release;
  auto released = release.get_future().share();
  fixture.graph->on_abort = [&] {
    entered.set_value();
    released.wait();
  };
  auto attempt = std::async(std::launch::async, [&] {
    return fixture.session->AbortUnsubmitted(fixture.thread, source);
  });
  const bool reached = entered.get_future().wait_for(std::chrono::seconds(2)) ==
                       std::future_status::ready;
  unsigned second_proofs = 0;
  bool overlapped_retire = false;
  if (reached) {
    fixture.session->Revoke();
    // Revocation closes admission, but does not drain the callback currently
    // outside the mutex. A later operation must not steal its completion.
    CHECK(fixture.session->GetSnapshot().operation_active);
    overlapped_retire =
        fixture.session->RetireAfterKernelStop([&](const auto&) {
          ++second_proofs;
          return true;
        });
  }
  release.set_value();
  const bool aborted = attempt.get();
  REQUIRE(reached);
  CHECK_FALSE(aborted);
  CHECK_FALSE(overlapped_retire);
  CHECK(second_proofs == 0);
  CHECK(fixture.session->GetSnapshot().phase == Phase::kQuarantined);
  CHECK_FALSE(fixture.session->GetSnapshot().operation_active);
  CHECK_FALSE(fixture.session->BeginUpdate(fixture.thread));
  REQUIRE(
      fixture.session->RetireAfterKernelStop([](const auto&) { return true; }));
}

TEST_CASE("Native stopped proof cannot overlap or replace a revoked operation",
          "[kernel][native_frame_owner]") {
  Fixture fixture;
  fixture.Bind();
  const auto source = fixture.Freeze();
  std::promise<void> entered, release;
  auto released = release.get_future().share();
  std::atomic<unsigned> graph_stops{0};
  std::atomic<bool> callback_was_active{false};
  fixture.graph->on_stop = [&] { ++graph_stops; };
  auto first = std::async(std::launch::async, [&] {
    return fixture.session->RetireAfterKernelStop([&](const auto&) {
      fixture.session->Revoke();  // Recursive revocation must preserve token.
      callback_was_active = fixture.session->GetSnapshot().operation_active;
      entered.set_value();
      released.wait();
      return true;
    });
  });
  const bool reached = entered.get_future().wait_for(std::chrono::seconds(2)) ==
                       std::future_status::ready;
  bool second = false;
  unsigned second_proofs = 0;
  if (reached) {
    fixture.session->Revoke();
    second = fixture.session->RetireAfterKernelStop([&](const auto&) {
      ++second_proofs;
      return true;
    });
    CHECK(graph_stops == 0);
    CHECK(fixture.session->GetSnapshot().source_serial == source->serial());
    CHECK(fixture.session->GetSnapshot().operation_active);
  }
  release.set_value();
  const bool retired = first.get();
  REQUIRE(reached);
  CHECK(callback_was_active);
  CHECK_FALSE(second);
  CHECK(second_proofs == 0);
  CHECK(retired);
  CHECK(graph_stops == 1);
  CHECK(fixture.session->GetSnapshot().phase == Phase::kRetired);
  CHECK_FALSE(fixture.session->GetSnapshot().operation_active);
}

TEST_CASE(
    "Interior output lookup retains exact physical parent through Source",
    "[kernel][native_frame_owner][native_output_allocation]") {
  Memory memory;
  REQUIRE(memory.Initialize());
  Fixture fixture;
  fixture.Bind();
  auto* heap = static_cast<PhysicalHeap*>(memory.LookupHeap(0xE1000000));
  REQUIRE(heap);
  uint32_t address = 0;
  uint64_t allocation_id = 0;
  REQUIRE(heap->AllocRangeOwned(
      0xE1000000, 0xE1400000, 0x6000, 0x1000,
      kMemoryAllocationReserve | kMemoryAllocationCommit,
      kMemoryProtectRead | kMemoryProtectWrite, true, fixture.session->owner(),
      &address, &allocation_id));
  const uint32_t physical = memory.GetPhysicalAddress(address);
  const memory::PhysicalRange interior{physical + 0x1000, 1};
  auto first = memory.PinOwnedPhysicalRanges(
      fixture.session->owner(), std::span(&interior, 1));
  REQUIRE(first);
  REQUIRE(first.pin->allocations().size() == 1);
  const auto parent = first.pin->allocations().front();
  CHECK(parent.allocation_id == allocation_id);
  CHECK(parent.owner == fixture.session->owner());
  CHECK(parent.physical_address == physical);
  CHECK(parent.virtual_address == address);
  CHECK(parent.length == 0x6000);
  const memory::PhysicalRange remaining{
      interior.address,
      uint32_t(uint64_t(parent.physical_address) + parent.length -
               interior.address)};
  REQUIRE(remaining.length == 0x5000);
  auto full = memory.PinOwnedPhysicalRanges(
      fixture.session->owner(), std::span(&remaining, 1));
  REQUIRE(full);
  CHECK(full.pin->allocations().front().allocation_id == allocation_id);
  // The one-byte lookup reveals real retained parent extent. No logical
  // surface geometry or arbitrary address-range bound supplies ownership.
  const auto source = fixture.Freeze();
  REQUIRE(fixture.session->AttachPhysicalPin(fixture.thread, source,
                                             first.pin));
  first.pin.reset();
  full.pin.reset();
  CHECK(memory.ReleaseOwnedPhysicalAllocation(fixture.session->owner(),
                                                allocation_id) ==
        memory::PhysicalLeaseStatus::kPinned);
  REQUIRE(fixture.session->AbortUnsubmitted(fixture.thread, source));
  CHECK(memory.ReleaseOwnedPhysicalAllocation(fixture.session->owner(),
                                                allocation_id) ==
        memory::PhysicalLeaseStatus::kSuccess);
}

TEST_CASE(
    "A real foreign output allocation pin cannot attach to current Source",
    "[kernel][native_frame_owner][native_output_allocation]") {
  Memory memory;
  REQUIRE(memory.Initialize());
  Fixture fixture;
  fixture.Bind();
  const auto source = fixture.Freeze();
  auto foreign = fixture.session->owner();
  ++foreign.session_id;
  auto* heap = static_cast<PhysicalHeap*>(memory.LookupHeap(0xE1000000));
  REQUIRE(heap);
  uint32_t address = 0;
  uint64_t allocation_id = 0;
  REQUIRE(heap->AllocRangeOwned(
      0xE1000000, 0xE1400000, 0x3000, 0x1000,
      kMemoryAllocationReserve | kMemoryAllocationCommit,
      kMemoryProtectRead | kMemoryProtectWrite, true, foreign, &address,
      &allocation_id));
  const memory::PhysicalRange range{memory.GetPhysicalAddress(address), 1};
  auto pin = memory.PinOwnedPhysicalRanges(foreign, std::span(&range, 1));
  REQUIRE(pin);
  CHECK(memory.PinOwnedPhysicalRanges(fixture.session->owner(),
                                       std::span(&range, 1)).status ==
        memory::PhysicalLeaseStatus::kWrongOwner);
  CHECK_FALSE(fixture.session->AttachPhysicalPin(fixture.thread, source,
                                                  pin.pin));
  CHECK(fixture.session->GetSnapshot().physical_pins == 0);
  CHECK(fixture.session->GetSnapshot().source_serial == source->serial());
  CHECK_FALSE(fixture.session->GetSnapshot().submission_attempted);
  REQUIRE(fixture.session->AbortUnsubmitted(fixture.thread, source));
  pin.pin.reset();
  CHECK(memory.ReleaseOwnedPhysicalAllocation(foreign, allocation_id) ==
        memory::PhysicalLeaseStatus::kSuccess);
}
}  // namespace
}  // namespace xe::kernel::test
