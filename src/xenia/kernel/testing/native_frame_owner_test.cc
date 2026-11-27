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
#include <string>
#include <vector>

#include "third_party/catch/include/catch.hpp"
#include "xenia/kernel/native_frame_owner.h"
#include "xenia/kernel/native_identity_image_journal.h"
#include "xenia/kernel/native_identity_image_frame.h"
#include "xenia/kernel/native_identity_default_image.h"
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

// These exercise the SAME Owner's production value journal. Supplied values
// stand for authenticated original native cuts; no native call, PNG decode,
// resource reference, command submission or GPU completion is manufactured.
using ImageJournal = identity_detail::NativeImageJournal;
using ImageInput = identity_detail::NativeImageInput;
using ImageTransfer = identity_detail::NativeDecodedImageTransfer;
using ImageRoot = identity_detail::NativeProfileIdentityTasks::Root;

ImageInput JournalInput(ImageRoot root, uint64_t source = 3,
                        uint64_t version = 17, uint64_t publication = 9) {
  return {identity_detail::EncodedImageSource{
              root, 0x42, 7, publication, source, version, 0x41000000, 512},
          {}};
}

ImageJournal::Object JournalObject(ImageJournal& journal,
                                  ImageJournal::Kind kind,
                                  uint32_t address) {
  const auto object = journal.BeginNativeObject(kind, address);
  REQUIRE(object);
  REQUIRE(journal.FinishNativeObject(*object, true));
  return *object;
}

ImageJournal::Object JournalWrapper(ImageJournal& journal,
                                   const ImageInput& input,
                                   uint32_t address = 0x42000000,
                                   uint32_t texture = 0x43000000) {
  const auto object =
      JournalObject(journal, ImageJournal::Kind::kWrapper, address);
  REQUIRE(journal.FinishWrapperAssignment(
      object, ImageTransfer{input, 33, texture}, true));
  return object;
}

TEST_CASE("Known native object construction revokes cross-kind reused DC birth",
          "[kernel][native_frame_owner][native_image_journal]") {
  const ImageRoot root{11, 12, 13};
  ImageJournal journal(root);
  const auto input = JournalInput(root);
  const auto wrapper = JournalWrapper(journal, input);
  const auto dc = JournalObject(journal, ImageJournal::Kind::kDc, 0x44000000);
  const auto selection = journal.BeginDcSelection(dc);
  REQUIRE(selection);
  REQUIRE(journal.SelectDcTexture(*selection, wrapper, 0x45000000,
                                 0x43000000, true, false));
  REQUIRE(journal.ObserveDrawCall(dc, selection->serial, 101, 102,
                                 0x45000000, 0x46000000, true));

  // Reuse is invalidated at genuine constructor ENTRY, before its output.
  const auto replacement = journal.BeginNativeObject(
      ImageJournal::Kind::kInterface, dc.address);
  REQUIRE(replacement);
  CHECK(replacement->birth != dc.birth);
  CHECK_FALSE(journal.Current(ImageJournal::Kind::kDc, dc.address));
  CHECK_FALSE(journal.ObserveDrawCall(dc, selection->serial, 101, 103,
                                    0x45000000, 0x46000000, true));
  CHECK_FALSE(journal.Current(ImageJournal::Kind::kInterface, dc.address));
  journal.RetireObject(dc);  // A delayed OLD token cannot retire the new birth.
  REQUIRE(journal.FinishNativeObject(*replacement, true));
  CHECK(journal.Current(ImageJournal::Kind::kInterface, dc.address) ==
        replacement);
  CHECK_FALSE(journal.FinishNativeObject(dc, true));

  const auto failed = journal.BeginNativeObject(ImageJournal::Kind::kBrush,
                                                dc.address);
  REQUIRE(failed);
  CHECK_FALSE(journal.FinishNativeObject(*failed, false));
  CHECK_FALSE(journal.Current(ImageJournal::Kind::kInterface, dc.address));
  CHECK_FALSE(journal.Current(ImageJournal::Kind::kBrush, dc.address));
  const auto retry =
      JournalObject(journal, ImageJournal::Kind::kDc, dc.address);
  CHECK(retry.birth != dc.birth);
  CHECK(retry.birth != failed->birth);
  CHECK_FALSE(journal.Copied(retry));
  journal.RetireObject(*replacement);
  CHECK(journal.Current(ImageJournal::Kind::kDc, dc.address) == retry);
}

TEST_CASE("Known cache winner keeps its complete desired-source lineage",
          "[kernel][native_frame_owner][native_image_journal]") {
  const ImageRoot root{21, 22, 23};
  ImageJournal journal(root);
  const auto desired = JournalInput(root);
  const auto image_interface =
      JournalObject(journal, ImageJournal::Kind::kInterface, 0x44000000);
  const auto brush =
      JournalObject(journal, ImageJournal::Kind::kBrush, 0x44000100);
  const auto winner = JournalWrapper(journal, desired);
  const auto unknown =
      JournalObject(journal, ImageJournal::Kind::kWrapper, 0x44000200);
  REQUIRE(journal.CopyOwnedPath(image_interface, desired, true));
  REQUIRE(journal.AdoptKnownWinner(brush, winner,
                                  *journal.OwnedPath(image_interface), true));
  const auto original = journal.Copied(brush);
  REQUIRE(original);
  CHECK(original->input == desired);

  SECTION("Different native winner never becomes requested source by relabel") {
    // Same pointer/length/version can still belong to a distinct source.
    const auto different = JournalInput(root, 4);
    const auto other = JournalWrapper(journal, different, 0x44000300,
                                      0x43000100);
    CHECK_FALSE(journal.AdoptKnownWinner(brush, other, desired, true));
    CHECK(journal.Copied(brush) == original);
    auto changed = desired;
    ++changed.provider->version;
    CHECK_FALSE(journal.AdoptKnownWinner(brush, winner, changed, true));
    changed = desired;
    ++changed.provider->publication;
    CHECK_FALSE(journal.AdoptKnownWinner(brush, winner, changed, true));
    CHECK(journal.Copied(brush) == original);
  }
  SECTION("Unknown or failed duplicate return cannot fabricate decoded bytes") {
    CHECK_FALSE(journal.AdoptKnownWinner(brush, unknown, desired, true));
    CHECK_FALSE(journal.AdoptKnownWinner(brush, winner, desired, false));
    CHECK_FALSE(journal.FinishWrapperAssignment(
        unknown, ImageTransfer{desired, 33, 0x43000000}, false));
    CHECK_FALSE(journal.Copied(unknown));
    CHECK(journal.Copied(brush) == original);
  }
  SECTION("Withdrawal is distinct from copied path and decoder output") {
    journal.BeforePathChange(image_interface);
    CHECK_FALSE(journal.OwnedPath(image_interface));
    journal.WithdrawResource(brush);
    CHECK_FALSE(journal.Copied(brush));
    CHECK(journal.Copied(winner) == original);
    CHECK_FALSE(journal.CopyOwnedPath(image_interface, desired, false));
    CHECK_FALSE(journal.OwnedPath(image_interface));
    REQUIRE(journal.CopyOwnedPath(image_interface, desired, true));
    REQUIRE(journal.AdoptKnownWinner(brush, winner, desired, true));
    CHECK(journal.Copied(brush) == original);
  }
}

TEST_CASE("Copied native DC texture survives encoded and wrapper retirement",
          "[kernel][native_frame_owner][native_image_journal]") {
  const ImageRoot root{31, 32, 33};
  ImageJournal journal(root);
  identity_detail::NativeEncodedImageEpoch encoded;
  REQUIRE(encoded.Allocate(root, 0x42, 0x41000000, 1024));
  const auto captured = encoded.Capture(0x42, 3, 17, 512,
                                       encoded.allocation(),
                                       encoded.publication());
  REQUIRE(captured);
  const ImageInput input{*captured, {}};
  REQUIRE(encoded.FinishOriginalDecode(*captured, captured->address,
                                       captured->size, true));
  const auto wrapper = JournalWrapper(journal, input);
  const auto dc = JournalObject(journal, ImageJournal::Kind::kDc, 0x44000000);
  const auto selection = journal.BeginDcSelection(dc);
  REQUIRE(selection);
  REQUIRE(journal.SelectDcTexture(*selection, wrapper, 0x45000000,
                                 0x43000000, true, false));
  // The native same-texture return can preserve a genuinely retained prior
  // source without a new pointer store, but it still has its own invocation.
  const auto unchanged = journal.BeginDcSelection(dc);
  REQUIRE(unchanged);
  REQUIRE(journal.SelectDcTexture(*unchanged, wrapper, 0x45000000,
                                 0x43000000, false, true));
  const auto unselected_dc =
      JournalObject(journal, ImageJournal::Kind::kDc, 0x44000100);
  CHECK_FALSE(journal.ObserveDrawCall(unselected_dc, unchanged->serial, 201,
                                    202, 0x45000000, 0x46000000, true));
  // Genuine copied native Texture AddRef ownership is separate from PNG use.
  encoded.Revoke();
  CHECK_FALSE(encoded.Current(*captured));
  journal.RetireObject(wrapper);
  CHECK_FALSE(journal.Copied(wrapper));
  const auto draw = journal.ObserveDrawCall(dc, unchanged->serial, 201, 202,
                                          0x45000000, 0x46000000, true);
  REQUIRE(draw);
  CHECK(draw->copied.input == input);
  CHECK(draw->copied.texture == 0x43000000);
  CHECK(draw->render == 201);
  CHECK(draw->call == 202);

  CHECK_FALSE(journal.ObserveDrawCall(dc, unchanged->serial + 1, 201, 203,
                                    0x45000000, 0x46000000, true));
  CHECK_FALSE(journal.ObserveDrawCall(dc, unchanged->serial, 0, 203,
                                    0x45000000, 0x46000000, true));
  CHECK_FALSE(journal.ObserveDrawCall(dc, unchanged->serial, 201, 0,
                                    0x45000000, 0x46000000, true));
  CHECK_FALSE(journal.ObserveDrawCall(dc, unchanged->serial, 201, 203,
                                    0x45000004, 0x46000000, true));
  CHECK_FALSE(journal.ObserveDrawCall(dc, unchanged->serial, 201, 203,
                                    0x45000000, 0, true));
  CHECK_FALSE(journal.ObserveDrawCall(dc, unchanged->serial, 201, 203,
                                    0x45000000, 0x46000000, false));
  auto foreign = dc;
  ++foreign.root.generation;
  CHECK_FALSE(journal.ObserveDrawCall(foreign, unchanged->serial, 201, 203,
                                    0x45000000, 0x46000000, true));
  REQUIRE(journal.NativeTextureOwned(draw->copied));
  journal.RevokeNativeTexture(draw->copied.texture);
  CHECK_FALSE(journal.NativeTextureOwned(draw->copied));
  CHECK_FALSE(journal.ObserveDrawCall(dc, unchanged->serial, 201, 203,
                                    0x45000000, 0x46000000, true));
  // Existing immutable frame/value receipts remain facts; no future native
  // object adoption can borrow this freed/reused texture address.
  CHECK(draw->copied.texture == 0x43000000);
  journal.WithdrawResource(dc);
  CHECK_FALSE(journal.ObserveDrawCall(dc, unchanged->serial, 201, 203,
                                    0x45000000, 0x46000000, true));
  // The prior value receipt remains factual; withdrawal grants no new draw.
  CHECK(draw->copied.input == input);
}

TEST_CASE("Overlapping or abandoned native DC selection cannot borrow readiness",
          "[kernel][native_frame_owner][native_image_journal]") {
  const ImageRoot root{41, 42, 43};
  ImageJournal journal(root);
  const auto input = JournalInput(root);
  const auto first = JournalWrapper(journal, input);
  const auto second = JournalWrapper(journal, JournalInput(root, 4, 18),
                                     0x42000100, 0x43000100);
  const auto dc = JournalObject(journal, ImageJournal::Kind::kDc, 0x44000000);
  const auto original = journal.BeginDcSelection(dc);
  REQUIRE(original);
  REQUIRE(journal.SelectDcTexture(*original, first, 0x45000000,
                                 0x43000000, true, false));
  const auto older = journal.BeginDcSelection(dc);
  const auto younger = journal.BeginDcSelection(dc);
  REQUIRE(older);
  REQUIRE(younger);
  CHECK_FALSE(journal.ObserveDrawCall(dc, original->serial, 301, 302,
                                    0x45000000, 0x46000000, true));
  CHECK_FALSE(journal.SelectDcTexture(*younger, second, 0x45000000,
                                     0x43000100, true, false));
  CHECK_FALSE(journal.SelectDcTexture(*older, first, 0x45000000,
                                     0x43000000, true, false));
  CHECK_FALSE(journal.Copied(dc));
  const auto recovery = journal.BeginDcSelection(dc);
  REQUIRE(recovery);
  // Previous matching texture alone is not a real store/retained old lineage.
  CHECK_FALSE(journal.SelectDcTexture(*recovery, first, 0x45000000,
                                     0x43000000, false, true));
  const auto fresh = journal.BeginDcSelection(dc);
  REQUIRE(fresh);
  REQUIRE(journal.SelectDcTexture(*fresh, first, 0x45000000,
                                 0x43000000, true, false));
  REQUIRE(journal.ObserveDrawCall(dc, fresh->serial, 301, 303,
                                 0x45000000, 0x46000000, true));

  const auto reset_pending = journal.BeginDcSelection(dc);REQUIRE(reset_pending);
  journal.WithdrawResource(dc);
  CHECK(journal.Current(ImageJournal::Kind::kDc,dc.address)==std::optional(dc));
  CHECK_FALSE(journal.Copied(dc));
  CHECK_FALSE(journal.SelectDcTexture(*reset_pending,first,0x45000000,0x43000000,false,true));
  const auto after_reset=journal.BeginDcSelection(dc);REQUIRE(after_reset);
  REQUIRE(journal.SelectDcTexture(*after_reset,first,0x45000000,0x43000000,true,false));

  const auto abandoned = journal.BeginDcSelection(dc);
  const auto attempted_recovery = journal.BeginDcSelection(dc);
  REQUIRE(abandoned);
  REQUIRE(attempted_recovery);
  CHECK_FALSE(journal.SelectDcTexture(*attempted_recovery, second, 0x45000000,
                                     0x43000100, true, false));
  CHECK_FALSE(journal.ObserveDrawCall(dc, attempted_recovery->serial, 301, 304,
                                    0x45000000, 0x46000000, true));
  // No synthetic return retires the abandoned invocation. Only real stop does.
  journal.RetireMetadataAfterActualStop();
  CHECK_FALSE(journal.Copied(dc));
  CHECK_FALSE(journal.BeginDcSelection(dc));
}

TEST_CASE("Native section image source stays distinct and exact through cache use",
          "[kernel][native_frame_owner][native_image_journal]") {
  const ImageRoot root{51, 52, 53};
  ImageJournal journal(root);
  const ImageInput section{
      {}, identity_detail::NativeSectionImageSource{
              root, 3, 0x42, 17, root.generation, 0x30000000, 0x81BE4480,
              0xEA39, 0x81BE6C9F, 522}};
  REQUIRE(section.valid());
  const auto wrapper = JournalWrapper(journal, section);
  const auto brush =
      JournalObject(journal, ImageJournal::Kind::kBrush, 0x44000000);
  REQUIRE(journal.AdoptKnownWinner(brush, wrapper, section, true));
  CHECK(journal.Copied(brush)->input.section == section.section);
  CHECK_FALSE(journal.Copied(brush)->input.provider);

  auto changed = section;
  ++changed.section->module_generation;
  CHECK_FALSE(changed.valid());
  CHECK_FALSE(journal.AdoptKnownWinner(brush, wrapper, changed, true));
  changed = section;
  changed.section->address = changed.section->section +
                             changed.section->section_size - 1;
  CHECK_FALSE(changed.valid());
  CHECK_FALSE(journal.AdoptKnownWinner(brush, wrapper, changed, true));
  changed = section;
  changed.provider = JournalInput(root).provider;
  CHECK_FALSE(changed.valid());
  CHECK_FALSE(journal.AdoptKnownWinner(brush, wrapper, changed, true));
  CHECK_FALSE(journal.AdoptKnownWinner(brush, wrapper, JournalInput(root), true));
  auto foreign = JournalInput(root);
  ++foreign.provider->root.domain;
  CHECK_FALSE(journal.FinishWrapperAssignment(
      wrapper, ImageTransfer{foreign, 34, 0x43000000}, true));
  CHECK(journal.Copied(brush)->input == section);
}

TEST_CASE("Actual stop closes all existing native image metadata generations",
          "[kernel][native_frame_owner][native_image_journal]") {
  const ImageRoot root{61, 62, 63};
  ImageJournal journal(root);
  const auto input = JournalInput(root);
  const auto wrapper = JournalWrapper(journal, input);
  const auto brush =
      JournalObject(journal, ImageJournal::Kind::kBrush, 0x44000000);
  const auto dc = JournalObject(journal, ImageJournal::Kind::kDc, 0x44000100);
  REQUIRE(journal.AdoptKnownWinner(brush, wrapper, input, true));
  const auto selection = journal.BeginDcSelection(dc);
  REQUIRE(selection);
  REQUIRE(journal.SelectDcTexture(*selection, wrapper, 0x45000000,
                                 0x43000000, true, false));
  const auto prior = journal.ObserveDrawCall(dc, selection->serial, 401, 402,
                                           0x45000000, 0x46000000, true);
  REQUIRE(prior);
  journal.RetireMetadataAfterActualStop();
  CHECK(journal.unavailable());
  CHECK_FALSE(journal.Current(ImageJournal::Kind::kWrapper, wrapper.address));
  CHECK_FALSE(journal.Copied(brush));
  CHECK_FALSE(journal.Copied(dc));
  CHECK_FALSE(journal.BeginNativeObject(ImageJournal::Kind::kDc, dc.address));
  CHECK_FALSE(journal.FinishWrapperAssignment(
      wrapper, ImageTransfer{input, 34, 0x43000000}, true));
  CHECK_FALSE(journal.SelectDcTexture(*selection, wrapper, 0x45000000,
                                     0x43000000, true, false));
  CHECK_FALSE(journal.ObserveDrawCall(dc, selection->serial, 401, 403,
                                    0x45000000, 0x46000000, true));
  CHECK(prior->copied.input == input);
}

// Exact ORIGINAL default-row observations are supplied by the Owner's genuine
// 8D68/Open/GetSection/GetBuffer/fallback cuts. The pure helper neither owns
// their native resources nor claims these scalar fixture calls execute them.
struct DefaultImageFacts {
  using DefaultRow = identity_detail::NativeDefaultImageRow;
  identity_detail::NativeDefaultRootSection current;
  identity_detail::NativeDefaultRowObservation row;
  identity_detail::NativeDefaultResourceOpenObservation open;
  identity_detail::NativeDefaultGetSectionObservation section;
  identity_detail::NativeDefaultGetBufferObservation buffer;
  identity_detail::NativeDefaultFallbackObservation fallback;

  explicit DefaultImageFacts(uint32_t index = 0) {
    constexpr ImageRoot root{71, 72, 73};
    constexpr uint32_t module = 0x30002000;
    constexpr std::array<std::u16string_view, 3> labels{
        u"defaultsystemimage.png", u"dashicon.png",
        u"ico_64x_licensestore.png"};
    constexpr std::array<uint32_t, 3> offsets{0x281F, 0x3893, 0x3546};
    constexpr std::array<uint32_t, 3> lengths{522, 7479, 2078};
    const bool xam = index < 2;
    current = {root, module, root.generation, xam ? "xam" : "shrdres",
               xam ? 0x81BE4480u : 0x81BAD980u,
               xam ? 0xEA39u : 0x36AA7u};
    row = {root, 701, index, 0x81A8271C + index * 0x10, module,
           root.generation, xam ? u"xam" : u"shrdres", labels[index]};
    open = {root, row.initializer, 702, row.row_output, module, 0x90060000,
            0, row.section, row.entry};
    section = {root, module, root.generation, current.name, current.address,
               current.size, 0};
    buffer = {root, row.initializer, open.invocation, row.row_output,
              open.stream, current.address + offsets[index], lengths[index],
              0};
    fallback = {root, module, root.generation, index, 74, 3,
                0xF123456789ABCDEFull, buffer.address, buffer.size, 0};
  }
  std::optional<DefaultRow> Observe() const {
    return DefaultRow::Observe(current, row, open, section, buffer);
  }
};

TEST_CASE("Original three default resource rows join distinct section sources",
          "[kernel][native_frame_owner][native_image_default]") {
  for (uint32_t index = 0; index < 3; ++index) {
    CAPTURE(index);
    DefaultImageFacts facts(index);
    const auto row = facts.Observe();
    REQUIRE(row);
    CHECK(row->index() == index);
    const auto original = row->AtOriginalFallback(facts.fallback);
    REQUIRE(original);
    CHECK(original->valid());
    CHECK(original->root == facts.current.root);
    CHECK(original->module == facts.current.module);
    CHECK(original->module_generation == facts.current.module_generation);
    CHECK(original->section == facts.current.address);
    CHECK(original->section_size == facts.current.size);
    CHECK(original->address == facts.buffer.address);
    CHECK(original->size == facts.buffer.size);
    CHECK(original->provider == facts.fallback.provider_birth);
    CHECK(original->source == facts.fallback.source);
    CHECK(original->version == 0xF123456789ABCDEFull);
    const ImageInput input{{}, *original};
    CHECK(input.valid());
    CHECK_FALSE(input.provider);  // No invented Provider allocation epoch.
    CHECK(input.section == original);
  }
}

TEST_CASE("Default row requires original root package entry and buffer join",
          "[kernel][native_frame_owner][native_image_default]") {
  DefaultImageFacts facts;
  REQUIRE(facts.Observe());
  SECTION("module-zero filesystem route is not a section-backed default") {
    facts.current.module = 0;
    facts.row.module = facts.open.module = facts.section.module = 0;
    CHECK_FALSE(facts.Observe());
  }
  SECTION("foreign or stale actual module cannot borrow the current root") {
    ++facts.section.module_generation;
    CHECK_FALSE(facts.Observe());
    facts.section.module_generation = facts.current.module_generation;
    ++facts.open.module;
    CHECK_FALSE(facts.Observe());
  }
  SECTION("another row or successful unrelated Open does not supply entry") {
    facts.row.index = 1;
    CHECK_FALSE(facts.Observe());
    facts.row.index = 0;
    ++facts.open.initializer;
    CHECK_FALSE(facts.Observe());
    facts.open.initializer = facts.row.initializer;
    ++facts.buffer.resource_open;
    CHECK_FALSE(facts.Observe());
  }
  SECTION("matching labels alone cannot supply unobserved section ownership") {
    facts.section = {};
    CHECK_FALSE(facts.Observe());
  }
  SECTION("resource label and original ANSI section must both match") {
    facts.open.entry = u"dashicon.png";
    CHECK_FALSE(facts.Observe());
    facts.open.entry = facts.row.entry;
    facts.section.name = "shrdres";
    CHECK_FALSE(facts.Observe());
  }
  SECTION("genuine failed original calls never become successful metadata") {
    facts.open.result = 0x80004005;
    CHECK_FALSE(facts.Observe());
    facts.open.result = 0;
    facts.section.result = 0xC0000001;
    CHECK_FALSE(facts.Observe());
    facts.section.result = 0;
    facts.buffer.result = 0x80004005;
    CHECK_FALSE(facts.Observe());
  }
  SECTION("actual stream output and GetBuffer must have the same owner") {
    facts.buffer.stream += 4;
    CHECK_FALSE(facts.Observe());
    facts.buffer.stream = facts.open.stream;
    facts.buffer.row_output += 0x10;
    CHECK_FALSE(facts.Observe());
  }
  SECTION("a nearby PNG address or truncated entry is not the original entry") {
    ++facts.buffer.address;
    CHECK_FALSE(facts.Observe());
    --facts.buffer.address;
    --facts.buffer.size;
    CHECK_FALSE(facts.Observe());
    ++facts.buffer.size;
    ++facts.current.address;
    ++facts.section.address;
    CHECK_FALSE(facts.Observe());
  }
  SECTION("current resource row identity must match actual GetSection result") {
    --facts.section.size;
    CHECK_FALSE(facts.Observe());
    facts.section.size = facts.current.size;
    ++facts.buffer.root.domain;
    CHECK_FALSE(facts.Observe());
  }
}

TEST_CASE("Original default fallback needs current module and exact selected row",
          "[kernel][native_frame_owner][native_image_default]") {
  // These transient copied labels die before the row's later fallback. The
  // resulting value owns only scalar genealogy, never borrowed string views.
  const auto row = [] {
    DefaultImageFacts facts;
    const std::u16string section = u"xam", entry = u"defaultsystemimage.png";
    const std::string ansi = "xam";
    facts.row.section = facts.open.section = section;
    facts.row.entry = facts.open.entry = entry;
    facts.current.name = facts.section.name = ansi;
    return facts.Observe();
  }();
  REQUIRE(row);
  DefaultImageFacts current;
  const auto original = row->AtOriginalFallback(current.fallback);
  REQUIRE(original);
  SECTION("failed absent or pending native lookup supplies no image source") {
    current.fallback.result = 0x8000FFFF;
    CHECK_FALSE(row->AtOriginalFallback(current.fallback));
    current.fallback.result = 0x3E5;
    CHECK_FALSE(row->AtOriginalFallback(current.fallback));
  }
  SECTION("root module removal and reload cannot revive a retained row value") {
    current.fallback.current_module = 0;
    CHECK_FALSE(row->AtOriginalFallback(current.fallback));
    current.fallback.current_module = current.current.module;
    ++current.fallback.current_module_generation;
    CHECK_FALSE(row->AtOriginalFallback(current.fallback));
    current.fallback.current_module_generation = current.current.module_generation;
    ++current.fallback.root.generation;
    CHECK_FALSE(row->AtOriginalFallback(current.fallback));
  }
  SECTION("real lookup result must match index pointer and full byte count") {
    ++current.fallback.index;
    CHECK_FALSE(row->AtOriginalFallback(current.fallback));
    --current.fallback.index;
    ++current.fallback.address;
    CHECK_FALSE(row->AtOriginalFallback(current.fallback));
    --current.fallback.address;
    --current.fallback.size;
    CHECK_FALSE(row->AtOriginalFallback(current.fallback));
  }
  SECTION("unknown provider birth or source cannot borrow default image bytes") {
    current.fallback.provider_birth = 0;
    CHECK_FALSE(row->AtOriginalFallback(current.fallback));
    current.fallback.provider_birth = 74;
    current.fallback.source = 0;
    CHECK_FALSE(row->AtOriginalFallback(current.fallback));
  }
  CHECK(original->source == 3);
  CHECK(original->version == 0xF123456789ABCDEFull);
}
}  // namespace
TEST_CASE("Retained native draw refuses a later same-source DC selection",
          "[kernel][native_frame_owner][native_image_journal]") {
 const ImageRoot root{81,82,83};ImageJournal journal(root);
 const auto first=JournalWrapper(journal,JournalInput(root,4,18));
 auto second_input=JournalInput(root,4,18);++second_input.provider->provider;
 const auto second=JournalWrapper(journal,second_input,0x42000100,0x43000100);
 const auto dc=JournalObject(journal,ImageJournal::Kind::kDc,0x44000000);
 const auto first_selection=journal.BeginDcSelection(dc);REQUIRE(first_selection);
 REQUIRE(journal.SelectDcTexture(*first_selection,first,0x45000000,0x43000000,true,false));
 const auto retained=journal.Copied(dc);REQUIRE(retained);
 const auto emitted=journal.ObserveRetainedDrawCall(dc,first_selection->serial,*retained,101,102,0x45000000,0x46000000,true);REQUIRE(emitted);
 const auto later=journal.BeginDcSelection(dc);REQUIRE(later);
 REQUIRE(journal.SelectDcTexture(*later,second,0x45000000,0x43000100,true,false));
 const auto now=journal.Copied(dc);REQUIRE(now);
 REQUIRE(now->input.source()==retained->input.source());
 REQUIRE(now->input!=retained->input);
 CHECK(emitted->copied==*retained);
 CHECK(journal.NativeTextureOwned(emitted->copied));
 CHECK_FALSE(journal.ObserveRetainedDrawCall(dc,first_selection->serial,*retained,101,102,0x45000000,0x46000000,true));
 // A latest serial must not substitute the different actual image transfer.
 CHECK_FALSE(journal.ObserveRetainedDrawCall(dc,later->serial,*retained,101,102,0x45000000,0x46000000,true));
 CHECK(journal.ObserveRetainedDrawCall(dc,later->serial,*now,103,104,0x45000000,0x46000000,true));
}

TEST_CASE("Render-role history preserves immutable actual draws through revoke and restart",
          "[kernel][native_frame_owner][native_image_journal][render_history]") {
 using History=identity_detail::NativeImageDrawHistory;
 const ImageRoot root{91,92,93};ImageJournal journal(root);
 // A non-render role owns no history. Allocating the value at real render
 // admission gives genuinely known empty storage; pointer absence is distinct.
 std::unique_ptr<History> history;
 CHECK_FALSE(history);
 history=std::make_unique<History>();
 CHECK(std::none_of(history->draws.begin(),history->draws.end(),[](const auto& row){return row.has_value();}));
 const auto wrapper=JournalWrapper(journal,JournalInput(root,5,19));
 const auto dc=JournalObject(journal,ImageJournal::Kind::kDc,0x44000000);
 const auto selection=journal.BeginDcSelection(dc);REQUIRE(selection);
 REQUIRE(journal.SelectDcTexture(*selection,wrapper,0x45000000,0x43000000,true,false));
 const auto copied=journal.Copied(dc);REQUIRE(copied);
 CHECK_FALSE(journal.ObserveRetainedDrawCall(dc,selection->serial,*copied,201,202,0x45000000,0x46000000,false));
 const auto draw=journal.ObserveRetainedDrawCall(dc,selection->serial,*copied,201,202,0x45000000,0x46000000,true);REQUIRE(draw);
 history->draws[0].emplace();history->draws[0]->draw=*draw;
 history->draws[0]->packet_address=0x47000000;
 history->draws[0]->packet_header=0xC0012200;
 const auto captured=history->draws; // SAME immutable value copy retained by frame guard.
 history->RevokeTexture(0x43000100);REQUIRE(history->draws[0]);
 history->RevokeTexture(copied->texture);CHECK_FALSE(history->draws[0]);
 REQUIRE(captured[0]);CHECK(captured[0]->draw==*draw);
 CHECK(captured[0]->packet_address==0x47000000);
 // CPU metadata retention never keeps a freed native texture valid. Mutable
 // journal ownership and a copied frame value are separate duties.
 journal.RevokeNativeTexture(copied->texture);
 CHECK_FALSE(journal.NativeTextureOwned(captured[0]->draw.copied));
 CHECK_FALSE(journal.ObserveRetainedDrawCall(dc,selection->serial,*copied,201,202,0x45000000,0x46000000,true));
 journal.RetireMetadataAfterActualStop();
 ImageRoot next=root;++next.generation;ImageJournal restarted(next);
 const auto next_wrapper=JournalWrapper(restarted,JournalInput(next,5,19));
 const auto next_dc=JournalObject(restarted,ImageJournal::Kind::kDc,dc.address);
 const auto next_selection=restarted.BeginDcSelection(next_dc);REQUIRE(next_selection);
 REQUIRE(restarted.SelectDcTexture(*next_selection,next_wrapper,0x45000000,0x43000000,true,false));
 CHECK_FALSE(restarted.ObserveRetainedDrawCall(dc,selection->serial,*copied,201,202,0x45000000,0x46000000,true));
 const auto next_copy=restarted.Copied(next_dc);REQUIRE(next_copy);
 CHECK(restarted.ObserveRetainedDrawCall(next_dc,next_selection->serial,*next_copy,203,204,0x45000000,0x46000000,true));
 history.reset();REQUIRE(captured[0]);CHECK(captured[0]->draw==*draw);
 // This fixture exercises real Journal/History value production and revoke,
 // not Owner role selection, GPU fences, native PPC or a frame acknowledgement.
}

}  // namespace xe::kernel::test
