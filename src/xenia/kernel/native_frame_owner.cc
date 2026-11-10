/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia contributors. All rights reserved.                    *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */
#include "xenia/kernel/native_frame_owner.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <utility>

#include "xenia/base/memory.h"
#include "xenia/base/mutex.h"
#include "xenia/emulator.h"
#include "xenia/gpu/register_file.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/native_import_domain.h"
#include "xenia/kernel/native_frame_input_capture.h"
#include "xenia/kernel/native_frame_lease_probe.h"
#include "xenia/kernel/native_frame_readback_capture.h"
#include "xenia/kernel/native_system_allocation_authority.h"
#include "xenia/kernel/native_notification_presentation.h"
#include "xenia/kernel/user_module.h"
#include "xenia/kernel/xthread.h"
#include "xenia/memory.h"

namespace xe::kernel {
namespace {
constexpr uint32_t kSystemProcess = 2;
constexpr size_t kMaxPrimaryBytes = 0x40000;
std::atomic<uint64_t> next_session{1};
uint64_t NextSession() {
  auto value = next_session.load(std::memory_order_relaxed);
  while (value && value != UINT64_MAX) {
    if (next_session.compare_exchange_weak(value, value + 1,
                                           std::memory_order_relaxed)) {
      return value;
    }
  }
  return 0;  // Exhaustion never reuses an earlier tag.
}

bool SameCompletedNativeTicket(
    const gpu::GpuCompletion& candidate, const gpu::GpuCompletion& attached,
    const gpu::NativeFrameSubmissionIdentity& identity, uint64_t epoch) {
  const auto candidate_identity = candidate.native_frame_identity();
  const auto attached_identity = attached.native_frame_identity();
  return epoch && identity.valid() && candidate_identity && attached_identity &&
         *candidate_identity == identity && *attached_identity == identity &&
         candidate.status() == gpu::GpuCompletionStatus::kComplete &&
         attached.status() == gpu::GpuCompletionStatus::kComplete &&
         candidate.backend_epoch() == epoch &&
         attached.backend_epoch() == epoch && candidate.submission() &&
         candidate.submission() == attached.submission() &&
         candidate.backend_stop_proof().backend_epoch() == epoch &&
         attached.backend_stop_proof().backend_epoch() == epoch &&
         !candidate.backend_stop_proof().stopped() &&
         !attached.backend_stop_proof().stopped();
}
bool PlanMatchesSource(const NativeFrameSource& source,
                       const gpu::NativeFrameCommandPlan& plan) {
  if (source.title_input() != plan.title_input()) {
    return false;
  }
  const auto primary = source.primary();
  if (primary.size() != plan.words().size() * sizeof(uint32_t)) {
    return false;
  }
  for (size_t i = 0; i < plan.words().size(); ++i) {
    const size_t at = i * 4;
    const uint32_t word = uint32_t(primary[at]) << 24 |
                          uint32_t(primary[at + 1]) << 16 |
                          uint32_t(primary[at + 2]) << 8 | primary[at + 3];
    if (word != plan.words()[i]) {
      return false;
    }
  }
  return true;
}
}  // namespace

NativeFrameOwnerSession::NativeFrameOwnerSession(
    ThreadIdentity thread, Owner owner, std::shared_ptr<GuestRuntime> runtime)
    : thread_(thread), owner_(owner), runtime_(std::move(runtime)) {}

std::shared_ptr<NativeFrameOwnerSession>
NativeFrameOwnerSession::CreateProvisional(
    ThreadIdentity thread, uint64_t kernel_epoch,
    std::shared_ptr<GuestRuntime> runtime) {
  if (!thread.kernel || !thread.thread || !thread.thread_id ||
      thread.process_type != kSystemProcess || !kernel_epoch || !runtime) {
    return {};
  }
  const auto snapshot = runtime->GetSnapshot();
  if (!snapshot.identity.hmodule || !snapshot.identity.generation ||
      snapshot.state == GuestRuntime::State::kFailed ||
      snapshot.state == GuestRuntime::State::kRetiring) {
    return {};
  }
  const auto owner = ReserveAllocationOwner(kernel_epoch, snapshot.identity);
  if (!owner.valid()) {
    return {};
  }
  return std::shared_ptr<NativeFrameOwnerSession>(
      new NativeFrameOwnerSession(thread, owner, std::move(runtime)));
}

NativeFrameOwnerSession::Owner NativeFrameOwnerSession::ReserveAllocationOwner(
    uint64_t kernel_epoch, GuestRuntime::Identity runtime) {
  if (!kernel_epoch || !runtime.hmodule || !runtime.generation) {
    return {};
  }
  return {kernel_epoch, runtime.generation, NextSession(), runtime.hmodule};
}
std::shared_ptr<NativeFrameOwnerSession>
NativeFrameOwnerSession::CreateDelegated(
    const std::shared_ptr<NativeSystemFrameDelegation>& delegation,
    std::shared_ptr<GuestRuntime> runtime) {
  if (!delegation || !runtime || !delegation->IsAccepting()) {
    return {};
  }
  const auto state = runtime->GetSnapshot();
  const auto& owner = delegation->owner();
  if (!owner.valid() || state.state != GuestRuntime::State::kReady ||
      state.initialization_active ||
      state.identity.hmodule != owner.module_hmodule ||
      state.identity.generation != owner.module_generation) {
    return {};
  }
  auto session = std::shared_ptr<NativeFrameOwnerSession>(
      new NativeFrameOwnerSession({}, owner, std::move(runtime)));
  session->delegated_ = true;
  session->delegation_ = delegation;
  return session;
}
bool NativeFrameOwnerSession::MatchesThread(
    const ThreadIdentity& thread) const {
  if (!delegated_) {
    return thread == thread_ && thread.process_type == kSystemProcess;
  }
  const auto delegation = delegation_.lock();
  return delegation && delegation->MatchesCurrentThread(
                           thread.kernel, thread.thread, thread.thread_id,
                           thread.process_type);
}

bool NativeFrameOwnerSession::RuntimeAdmitted(bool provisional) const {
  if (delegated_) {
    const auto delegation = delegation_.lock();
    if (!delegation || !delegation->IsAccepting()) {
      return false;
    }
  }
  const auto snapshot = runtime_->GetSnapshot();
  return snapshot.identity.hmodule == owner_.module_hmodule &&
         snapshot.identity.generation == owner_.module_generation &&
         (provisional
              ? (snapshot.state == GuestRuntime::State::kResourcesLoaded ||
                 snapshot.state == GuestRuntime::State::kInitializing ||
                 snapshot.state == GuestRuntime::State::kReady)
              : (snapshot.state == GuestRuntime::State::kReady &&
                 !snapshot.initialization_active));
}

NativeFrameOwnerSession::Operation::~Operation() {
  session_->EndOperation(serial_);
}

uint64_t NativeFrameOwnerSession::StartOperationLocked() {
  if (active_operation_) {
    return 0;
  }
  if (next_operation_ == UINT64_MAX) {
    QuarantineLocked();
    return 0;
  }
  active_operation_ = next_operation_++;
  return active_operation_;
}

void NativeFrameOwnerSession::EndOperation(uint64_t serial) {
  if (!serial) {
    return;
  }
  std::lock_guard lock(mutex_);
  if (active_operation_ == serial) {
    active_operation_ = 0;
  }
}

NativeFrameOwnerSession::Update::Update(Update&& other) noexcept
    : session_(std::move(other.session_)),
      serial_(std::exchange(other.serial_, 0)) {}
NativeFrameOwnerSession::Update& NativeFrameOwnerSession::Update::operator=(
    Update&& other) noexcept {
  if (this != &other) {
    Reset();
    session_ = std::move(other.session_);
    serial_ = std::exchange(other.serial_, 0);
  }
  return *this;
}
NativeFrameOwnerSession::Update::~Update() { Reset(); }
NativeFrameOwnerSession::Update::operator bool() const {
  const auto session = session_.lock();
  return session && session->UpdateActive(serial_);
}
void NativeFrameOwnerSession::Update::Reset() {
  if (const auto session = session_.lock()) {
    session->EndUpdate(serial_);
  }
  serial_ = 0;
  session_.reset();
}
bool NativeFrameOwnerSession::UpdateActive(uint64_t serial) const {
  std::lock_guard lock(mutex_);
  return serial && active_update_ == serial && !active_operation_ &&
         (phase_ == Phase::kProvisional || phase_ == Phase::kBound);
}
void NativeFrameOwnerSession::EndUpdate(uint64_t serial) {
  std::shared_ptr<void> released_writer;
  std::shared_ptr<void> released_preparation;
  {
    std::lock_guard lock(mutex_);
    if (serial && active_update_ == serial) {
      active_update_ = 0;
      if (phase_ == Phase::kProvisional || phase_ == Phase::kBound) {
        frame_writer_.swap(released_writer);
      }
    }
  }
}

NativeFrameOwnerSession::Update NativeFrameOwnerSession::BeginUpdate(
    const ThreadIdentity& thread) {
  if (!MatchesThread(thread)) {
    return {};
  }
  std::shared_ptr<void> writer;
  if (delegated_) {
    const auto delegation = delegation_.lock();
    writer = delegation ? delegation->AcquireFrameWriter() : nullptr;
    if (!writer) {
      return {};
    }
  }
  std::shared_ptr<GraphAuthority> graph;
  Operation operation(shared_from_this());
  uint64_t serial;
  {
    std::lock_guard lock(mutex_);
    if ((phase_ != Phase::kProvisional && phase_ != Phase::kBound) ||
        active_operation_ || active_update_) {
      return {};
    }
    if (next_update_ == UINT64_MAX) {
      QuarantineLocked();
      return {};
    }
    operation.serial_ = StartOperationLocked();
    if (!operation.serial_) {
      return {};
    }
    serial = next_update_++;
    active_update_ = serial;
    frame_writer_ = std::move(writer);
    graph = graph_;
  }
  try {
    if (!RuntimeAdmitted(!graph) || (graph && !graph->Matches(owner_))) {
      Revoke();
      return {};
    }
  } catch (...) {
    Revoke();
    throw;
  }
  std::lock_guard lock(mutex_);
  if (active_operation_ != operation.serial_ || active_update_ != serial ||
      (phase_ != Phase::kProvisional && phase_ != Phase::kBound)) {
    return {};
  }
  return Update(weak_from_this(), serial);
}

std::optional<NativeFrameOwnerSession::Owner>
NativeFrameOwnerSession::AllocationOwner(const ThreadIdentity& thread) const {
  if (!MatchesThread(thread)) {
    return {};
  }
  const bool provisional_runtime = RuntimeAdmitted(true);
  const bool ready_runtime = RuntimeAdmitted(false);
  std::lock_guard lock(mutex_);
  if (!active_update_ || active_operation_ ||
      (phase_ != Phase::kProvisional && phase_ != Phase::kBound) ||
      (phase_ == Phase::kProvisional ? !provisional_runtime : !ready_runtime)) {
    return {};
  }
  return owner_;
}

bool NativeFrameOwnerSession::BindGraph(const ThreadIdentity& thread,
                                        std::shared_ptr<GraphAuthority> graph) {
  if (!MatchesThread(thread) || !graph || !RuntimeAdmitted(false)) {
    return false;
  }
  Operation operation(shared_from_this());
  {
    std::lock_guard lock(mutex_);
    if (phase_ != Phase::kProvisional || active_operation_ || graph_) {
      return false;
    }
    operation.serial_ = StartOperationLocked();
    if (!operation.serial_) {
      return false;
    }
    graph_ = graph;
  }
  try {
    if (!graph->Matches(owner_)) {
      Revoke();
      return false;
    }
  } catch (...) {
    Revoke();
    throw;
  }
  std::lock_guard lock(mutex_);
  if (phase_ != Phase::kProvisional || active_operation_ != operation.serial_) {
    return false;
  }
  phase_ = Phase::kBound;
  return true;
}

bool NativeFrameOwnerSession::SourceMatches(
    const std::shared_ptr<const Source>& source) const {
  return source && source_ == source && source->owner() == owner_;
}

std::shared_ptr<const NativeFrameOwnerSession::Source>
NativeFrameOwnerSession::Freeze(
    const ThreadIdentity& thread, Update& update,
    std::span<const uint8_t> primary,
    std::shared_ptr<const gpu::NativeSystemTitleInput> title_input) {
  if (!MatchesThread(thread) || !RuntimeAdmitted(false) || primary.empty() ||
      primary.size() > kMaxPrimaryBytes || primary.size() % 4 ||
      update.session_.lock().get() != this) {
    return {};
  }
  if (title_input && !title_input->valid()) {
    return {};
  }
  std::vector<uint8_t> immutable(primary.begin(), primary.end());
  std::shared_ptr<GraphAuthority> graph;
  std::shared_ptr<const Source> source;
  Operation operation(shared_from_this());
  {
    std::lock_guard lock(mutex_);
    if (phase_ != Phase::kBound || active_operation_ || !graph_ ||
        !update.serial_ || active_update_ != update.serial_) {
      return {};
    }
    if (next_source_ == UINT64_MAX) {
      QuarantineLocked();
      return {};
    }
    uint64_t source_serial = 0;
    if (delegated_) {
      const auto delegation = delegation_.lock();
      source_serial = delegation ? delegation->ReserveSourceSerial() : 0;
    } else {
      source_serial = next_source_++;
    }
    if (!source_serial) {
      QuarantineLocked();
      return {};
    }
    source = std::shared_ptr<const Source>(new Source(
        owner_, source_serial, std::move(immutable), std::move(title_input)));
    operation.serial_ = StartOperationLocked();
    if (!operation.serial_) {
      return {};
    }
    source_ = source;
    active_update_ = 0;
    update.serial_ = 0;
    update.session_.reset();
    phase_ = Phase::kFrozen;
    graph = graph_;
    quarantine_pin_ = shared_from_this();
  }
  try {
    if (!graph->Matches(owner_) ||
        (source->title_input() &&
         !graph->MatchesTitleInput(*source->title_input()))) {
      Revoke();
      return {};
    }
  } catch (...) {
    Revoke();
    throw;
  }
  std::lock_guard lock(mutex_);
  if (phase_ != Phase::kFrozen || active_operation_ != operation.serial_ ||
      !SourceMatches(source)) {
    return {};
  }
  return source;
}

bool NativeFrameOwnerSession::AttachPhysicalPin(
    const ThreadIdentity& thread, const std::shared_ptr<const Source>& source,
    std::shared_ptr<const memory::PhysicalRangePin> pin) {
  if (!MatchesThread(thread) || !pin || !pin->valid() ||
      pin->allocations().empty()) {
    return false;
  }
  for (const auto& allocation : pin->allocations()) {
    if (allocation.owner != owner_) {
      return false;
    }
  }
  std::lock_guard lock(mutex_);
  if (phase_ != Phase::kFrozen || active_operation_ || !SourceMatches(source) ||
      pins_.size() >= 512) {
    return false;
  }
  pins_.push_back(std::move(pin));
  return true;
}

class NativeFrameOwnerSession::ExecutionAuthority final
    : public gpu::NativeFrameExecutionAuthority {
 public:
  ExecutionAuthority(std::shared_ptr<NativeFrameOwnerSession> session,
                     std::shared_ptr<const Source> source,
                     std::shared_ptr<const gpu::NativeFrameCommandPlan> plan,
                     uint64_t preparation, uint64_t backend_epoch)
      : session_(std::move(session)),
        source_(std::move(source)),
        plan_(std::move(plan)),
        preparation_(preparation),
        backend_epoch_(backend_epoch) {}
  bool IsAdmitted(
      const gpu::NativeFrameExecutionRequest& request) const override {
    return session_->CheckExecutionRequest(source_, plan_, preparation_,
                                           backend_epoch_, request, false);
  }
  bool MarkSubmissionAttempted(
      const gpu::NativeFrameExecutionRequest& request) override {
    return session_->CheckExecutionRequest(source_, plan_, preparation_,
                                           backend_epoch_, request, true);
  }
  bool IsPresentationOwnerLive(
      const gpu::NativeFrameExecutionRequest& request) const override {
    return request.identity() ==
               gpu::NativeFrameSubmissionIdentity{session_->owner(),
                                                  source_->serial()} &&
           request.plan() == plan_ &&
           request.backend_epoch() == backend_epoch_ &&
           session_->IsPresentationOwnerLive(backend_epoch_);
  }
  std::unique_ptr<gpu::NativeFrameReadbackAdmission> AcquireCompletedReadback(
      const gpu::NativeFrameExecutionRequest& request,
      const gpu::GpuCompletion& completion) override {
    return session_->AcquireCompletedReadback(source_, plan_, backend_epoch_,
                                              request, completion);
  }

 private:
  const std::shared_ptr<NativeFrameOwnerSession> session_;
  const std::shared_ptr<const Source> source_;
  const std::shared_ptr<const gpu::NativeFrameCommandPlan> plan_;
  const uint64_t preparation_, backend_epoch_;
};

class NativeFrameOwnerSession::CompletedReadbackAdmission final
    : public gpu::NativeFrameReadbackAdmission {
 public:
  CompletedReadbackAdmission(std::shared_ptr<NativeFrameOwnerSession> session,
                             std::shared_ptr<const Source> source,
                             gpu::GpuCompletion completion, uint64_t operation)
      : session_(std::move(session)),
        source_(std::move(source)),
        completion_(std::move(completion)),
        operation_(operation) {}
  ~CompletedReadbackAdmission() override { Finish(Outcome::kGpuUncertain); }
  bool Finish(Outcome outcome) noexcept override {
    const auto operation = operation_.exchange(0, std::memory_order_acq_rel);
    return operation && session_->FinishCompletedReadback(operation, source_,
                                                          completion_, outcome);
  }

 private:
  const std::shared_ptr<NativeFrameOwnerSession> session_;
  const std::shared_ptr<const Source> source_;
  const gpu::GpuCompletion completion_;
  std::atomic<uint64_t> operation_;
};

std::unique_ptr<gpu::NativeFrameReadbackAdmission>
NativeFrameOwnerSession::AcquireCompletedReadback(
    const std::shared_ptr<const Source>& source,
    const std::shared_ptr<const gpu::NativeFrameCommandPlan>& plan,
    uint64_t backend_epoch, const gpu::NativeFrameExecutionRequest& request,
    const gpu::GpuCompletion& completion) {
  if (!source || !plan || !backend_epoch || request.plan() != plan ||
      request.identity() !=
          gpu::NativeFrameSubmissionIdentity{owner_, source->serial()} ||
      request.backend_epoch() != backend_epoch) {
    return {};
  }
  std::shared_ptr<GraphAuthority> graph;
  Operation operation(shared_from_this());
  {
    std::lock_guard lock(mutex_);
    if ((phase_ != Phase::kSubmitted && phase_ != Phase::kRetiring) ||
        active_operation_ || !SourceMatches(source) || !gpu_attached_ ||
        !submission_attempted_ || active_preparation_ || !graph_ ||
        !SameCompletedNativeTicket(completion, completion_, request.identity(),
                                   backend_epoch)) {
      return {};
    }
    operation.serial_ = StartOperationLocked();
    if (!operation.serial_) {
      return {};
    }
    graph = graph_;
  }
  try {
    // Reserve the operation before leaving the lock: retirement cannot reopen
    // this source while the graph is checked or while the GPU copy is pending.
    if (!RuntimeAdmitted(false) || !graph->Matches(owner_) ||
        (source->title_input() &&
         !graph->MatchesTitleInput(*source->title_input())) ||
        graph->backend_epoch() != backend_epoch ||
        !graph->ValidateNativeSubmission(*source, completion)) {
      Revoke();
      return {};
    }
  } catch (...) {
    Revoke();
    return {};
  }
  std::lock_guard lock(mutex_);
  if ((phase_ != Phase::kSubmitted && phase_ != Phase::kRetiring) ||
      active_operation_ != operation.serial_ || !SourceMatches(source) ||
      !gpu_attached_ || !submission_attempted_ ||
      !SameCompletedNativeTicket(completion, completion_, request.identity(),
                                 backend_epoch)) {
    return {};
  }
  auto admission = std::make_unique<CompletedReadbackAdmission>(
      shared_from_this(), source, completion, operation.serial_);
  operation.serial_ = 0;  // The typed admission now owns the operation.
  return admission;
}

bool NativeFrameOwnerSession::FinishCompletedReadback(
    uint64_t operation, const std::shared_ptr<const Source>& source,
    const gpu::GpuCompletion& completion,
    gpu::NativeFrameReadbackAdmission::Outcome outcome) noexcept {
  using Outcome = gpu::NativeFrameReadbackAdmission::Outcome;
  std::shared_ptr<GraphAuthority> graph;
  {
    std::lock_guard lock(mutex_);
    if (!operation || active_operation_ != operation) {
      return false;
    }
    if ((outcome != Outcome::kCanceledBeforeGpu &&
         outcome != Outcome::kGpuComplete) ||
        (phase_ != Phase::kSubmitted && phase_ != Phase::kRetiring) ||
        !SourceMatches(source) || !gpu_attached_ || !submission_attempted_ ||
        !graph_ ||
        !SameCompletedNativeTicket(completion, completion_,
                                   {owner_, source->serial()},
                                   completion.backend_epoch())) {
      // Revoke before releasing the scope. The old frame's Complete token
      // cannot retire new readback commands whose GPU use is uncertain.
      QuarantineLocked();
      active_operation_ = 0;
      return false;
    }
    graph = graph_;
  }
  bool valid = false;
  try {
    valid = RuntimeAdmitted(false) && graph->Matches(owner_) &&
            graph->backend_epoch() == completion.backend_epoch() &&
            graph->ValidateNativeSubmission(*source, completion);
  } catch (...) {
    // Completion is noexcept; failed owner validation preserves quarantine.
  }
  std::lock_guard lock(mutex_);
  if (active_operation_ != operation) {
    return false;
  }
  valid &= (phase_ == Phase::kSubmitted || phase_ == Phase::kRetiring) &&
           SourceMatches(source) && gpu_attached_ && submission_attempted_ &&
           SameCompletedNativeTicket(completion, completion_,
                                     {owner_, source->serial()},
                                     completion.backend_epoch());
  if (!valid) {
    QuarantineLocked();
  }
  active_operation_ = 0;
  return valid;
}

std::optional<gpu::NativeFrameOutputLayout>
NativeFrameOwnerSession::SnapshotOutputLayout(
    Memory* guest_memory, const ThreadIdentity& thread,
    const std::shared_ptr<const Source>& source,
    const std::shared_ptr<const gpu::NativeFrameCommandPlan>& plan,
    std::span<const memory::PhysicalRange> input_ranges,
    const NativeCompletedFrameRecord& record) {
  if (!guest_memory || !MatchesThread(thread) || !source || !plan ||
      !PlanMatchesSource(*source, *plan)) {
    return {};
  }
  Operation operation(shared_from_this());
  std::vector<std::shared_ptr<const memory::PhysicalRangePin>> pins;
  {
    std::lock_guard lock(mutex_);
    if (phase_ != Phase::kFrozen || active_operation_ ||
        !SourceMatches(source) || submission_attempted_ ||
        active_preparation_ || pins_.empty()) {
      return {};
    }
    operation.serial_ = StartOperationLocked();
    if (!operation.serial_) {
      return {};
    }
    pins = pins_;
  }
  // Retain and check the actual allocation pins before the helper's physical
  // vertex read. The operation prevents Abort/retirement/New Update until the
  // snapshot and final exact-Source check finish, including outside the lock.
  const auto covered = [&](memory::PhysicalRange range) {
    if (!range.length || range.address >= 0x20000000 ||
        range.length > 0x20000000 - range.address) {
      return false;
    }
    for (const auto& pin : pins) {
      if (!pin || !pin->valid()) {
        continue;
      }
      for (const auto& allocation : pin->allocations()) {
        if (allocation.owner == owner_ &&
            range.address >= allocation.physical_address &&
            uint64_t(range.address) + range.length <=
                uint64_t(allocation.physical_address) + allocation.length) {
          return true;
        }
      }
    }
    return false;
  };
  if (!covered(
          {record.output_physical_address, record.output_allocation_bytes}) ||
      input_ranges.empty()) {
    return {};
  }
  for (const auto range : input_ranges) {
    if (!covered(range)) {
      return {};
    }
  }
  const auto snapshot = SnapshotNativeFrameReadbackLayout(guest_memory, *plan,
                                                          input_ranges, record);
  if (!snapshot) {
    return {};
  }
  std::lock_guard lock(mutex_);
  if (phase_ != Phase::kFrozen || active_operation_ != operation.serial_ ||
      !SourceMatches(source) || submission_attempted_ || active_preparation_) {
    return {};
  }
  gpu::NativeFrameOutputLayout result;
  result.origin = gpu::NativeFrameOutputLayout::Origin::kOriginalXgHeader;
  result.width = snapshot->width;
  result.height = snapshot->height;
  result.pitch = snapshot->pitch;
  result.required_bytes = snapshot->required_bytes;
  result.texture_fetch = snapshot->texture_fetch;
  result.original_texture_fetch = snapshot->original_texture_fetch;
  return result;
}

bool NativeFrameOwnerSession::ValidateControlFrame(
    Memory* guest_memory, const ThreadIdentity& thread,
    const std::shared_ptr<const Source>& source,
    const std::shared_ptr<const gpu::NativeFrameCommandPlan>& plan,
    const NativeCompletedFrameRecord& record,
    const std::array<uint8_t, 148>& descriptor,
    const std::shared_ptr<const NativeNonmodalControlProvenance>& proof) {
  if (!guest_memory || !proof || !MatchesThread(thread) || !source || !plan ||
      !plan->is_overlay_control() || !PlanMatchesSource(*source, *plan) ||
      !record.native_end_completed || record.output_physical_address ||
      record.output_cpu_address || record.output_allocation_bytes ||
      std::any_of(record.output_texture_fetch.begin(),
                  record.output_texture_fetch.end(),
                  [](uint32_t value) { return value != 0; }) ||
      record.primary.size() != plan->words().size() * 4 ||
      !std::all_of(descriptor.begin() + 8, descriptor.end(),
                   [](uint8_t value) { return value == 0; })) {
    return false;
  }
  const auto control_is_current = [&]() {
    if (!delegated_ || !XThread::IsInThread() || proof->owner() != owner_ ||
        proof->thread_id() != thread.thread_id || !proof->domain_id() ||
        !proof->root_load_generation() || !proof->foreground_generation() ||
        !proof->native_task() || !proof->frame_serial() || !proof->frame_sp() ||
        (proof->frame_sp() & 15)) {
      return false;
    }
    const auto delegation = delegation_.lock();
    const auto authority = delegation ? delegation->authority() : nullptr;
    auto* current = XThread::GetCurrentThread();
    auto* kernel = authority ? authority->kernel() : nullptr;
    if (!authority || !authority->IsAccepting() || !kernel ||
        kernel != thread.kernel || current != thread.thread ||
        current->kernel_state() != kernel || !current->thread_state() ||
        !MatchesThread(thread)) {
      return false;
    }
    const auto* context = current->thread_state()->context();
    const auto pointer = [](uint64_t value) -> std::optional<uint32_t> {
      const uint32_t low = uint32_t(value);
      return value == low || value == uint64_t(int64_t(int32_t(low)))
                 ? std::optional(low)
                 : std::nullopt;
    };
    if (!context || context->r[15] || context->r[16] ||
        context->kernel_state != kernel ||
        context->thread_state != current->thread_state()) {
      return false;
    }
    const auto lr = pointer(context->lr), sp = pointer(context->r[1]);
    if (!lr || *lr != 0x81842BFCu || !sp || *sp > UINT32_MAX - 0x120 ||
        *sp + 0x120 != proof->frame_sp()) {
      return false;
    }
    const auto root = authority->root_module();
    const auto caller = kernel->ResolveNativeImportCaller(*lr);
    if (!root || root->hmodule_ptr() != owner_.module_hmodule ||
        authority->root_load_generation() != proof->root_load_generation() ||
        root->load_generation() != proof->root_load_generation() ||
        !caller.scoped || !caller.domain || !caller.domain->active() ||
        caller.domain->id() != proof->domain_id() ||
        !authority->MatchesDomain(caller.domain) ||
        caller.domain->root() !=
            NativeImportDomain::Member{owner_.module_hmodule,
                                       proof->root_load_generation()}) {
      return false;
    }
    auto* emulator = kernel->emulator();
    if (!emulator || !emulator->OwnsTitleExecution(kernel) ||
        kernel->memory() != guest_memory) {
      return false;
    }
    const auto foreground = emulator->title_execution_state().snapshot();
    const auto title = kernel->GetExecutableModule();
    if (foreground.phase != TitleExecutionState::Phase::kRunning ||
        foreground.generation != proof->foreground_generation() || !title) {
      return false;
    }
    // Counter is the existing recovered root data cell, under the retained
    // actual root lease. An address being in RAM is insufficient admission.
    constexpr uint32_t kNativeFrameCounter = 0x81AA1B10;
    auto memory_lock = global_critical_region::AcquireDirect();
    return kernel->GetUserModuleForAddress(kNativeFrameCounter).get() ==
               root.get() &&
           guest_memory->QueryRangeAccess(kNativeFrameCounter, 4) !=
               memory::PageAccess::kNoAccess &&
           xe::load_and_swap<uint32_t>(guest_memory->TranslateVirtual(
               kNativeFrameCounter)) == proof->native_frame_counter();
  };
  if (!control_is_current()) {
    return false;
  }
  const auto word = [&](uint32_t at) {
    return uint32_t(descriptor[at]) << 24 | uint32_t(descriptor[at + 1]) << 16 |
           uint32_t(descriptor[at + 2]) << 8 | descriptor[at + 3];
  };
  {
    auto memory_lock = global_critical_region::AcquireDirect();
    const uint32_t observed = word(0);
    auto* heap = guest_memory->LookupHeap(observed);
    if (observed >= 0x20000000 &&
        (!heap || heap->heap_type() != HeapType::kGuestPhysical)) {
      return false;
    }
    const uint32_t physical = observed < 0x20000000
                                  ? observed
                                  : guest_memory->GetPhysicalAddress(observed);
    if (physical != record.primary_physical_address ||
        uint64_t(word(4)) * 4 != record.primary.size()) {
      return false;
    }
  }
  std::lock_guard lock(mutex_);
  return phase_ == Phase::kFrozen && !active_operation_ &&
         SourceMatches(source) && !submission_attempted_ &&
         !active_preparation_ && !pins_.empty();
}

std::optional<gpu::NativeFrameOutputLayout>
NativeFrameOwnerSession::SnapshotPm4OutputLayout(
    Memory* guest_memory, const ThreadIdentity& thread,
    const std::shared_ptr<const Source>& source,
    const std::shared_ptr<const gpu::NativeFrameCommandPlan>& plan,
    std::span<const memory::PhysicalRange> inputs,
    const NativeCompletedFrameRecord& record,
    const std::array<uint8_t, 148>& descriptor,
    const std::shared_ptr<const gpu::NativeFrameInputSnapshot>& snapshot,
    const std::shared_ptr<const NativeNonmodalOutputProvenance>&
        output_provenance,
    const std::optional<gpu::NativeOverlayState>& overlay_input_state) {
  if (!guest_memory || !MatchesThread(thread) || !source || !plan ||
      !PlanMatchesSource(*source, *plan) || !record.native_end_completed ||
      !plan->has_final_copy() || plan->inventory().draws.empty() ||
      record.primary.size() != plan->words().size() * 4) {
    return {};
  }
  if (overlay_input_state && !overlay_input_state->valid()) {
    return {};
  }
  const bool native_overlay = bool(output_provenance);
  // The new origin is accepted only on the actual delegated Set invocation.
  // This does not manufacture a Task, replay its TLS or call any PPC helper.
  // All external authority/module/foreground checks precede the Session lock.
  const auto output_is_current = [&]() {
    if (!native_overlay) {
      return true;
    }
    if (!delegated_ || !XThread::IsInThread() ||
        output_provenance->owner() != owner_ ||
        output_provenance->thread_id() != thread.thread_id ||
        !output_provenance->domain_id() ||
        !output_provenance->root_load_generation() ||
        !output_provenance->foreground_generation() ||
        !output_provenance->native_task() ||
        !output_provenance->frame_serial() ||
        !output_provenance->frame_sp() ||
        (output_provenance->frame_sp() & 15)) {
      return false;
    }
    const auto delegation = delegation_.lock();
    const auto authority = delegation ? delegation->authority() : nullptr;
    auto* current = XThread::GetCurrentThread();
    auto* kernel = authority ? authority->kernel() : nullptr;
    if (!authority || !authority->IsAccepting() || !kernel ||
        kernel != thread.kernel || current != thread.thread ||
        current->kernel_state() != kernel || !current->thread_state() ||
        !MatchesThread(thread)) {
      return false;
    }
    const auto* context = current->thread_state()->context();
    const auto pointer = [](uint64_t value) -> std::optional<uint32_t> {
      const uint32_t low = uint32_t(value);
      return value == low || value == uint64_t(int64_t(int32_t(low)))
                 ? std::optional(low)
                 : std::nullopt;
    };
    if (!context || context->kernel_state != kernel ||
        context->thread_state != current->thread_state()) {
      return false;
    }
    const auto lr = pointer(context->lr), sp = pointer(context->r[1]);
    if (!lr || *lr != 0x81842BFCu || !sp || *sp > UINT32_MAX - 0x120 ||
        *sp + 0x120 != output_provenance->frame_sp()) {
      return false;
    }
    const auto root = authority->root_module();
    const auto caller = kernel->ResolveNativeImportCaller(*lr);
    if (!root || root->hmodule_ptr() != owner_.module_hmodule ||
        authority->root_load_generation() !=
            output_provenance->root_load_generation() ||
        root->load_generation() != output_provenance->root_load_generation() ||
        !caller.scoped || !caller.domain || !caller.domain->active() ||
        caller.domain->id() != output_provenance->domain_id() ||
        !authority->MatchesDomain(caller.domain) ||
        caller.domain->root() != NativeImportDomain::Member{
                                     owner_.module_hmodule,
                                     output_provenance->root_load_generation()}) {
      return false;
    }
    auto* emulator = kernel->emulator();
    if (!emulator || !emulator->OwnsTitleExecution(kernel) ||
        kernel->memory() != guest_memory || !source->title_input()) {
      return false;
    }
    const auto foreground = emulator->title_execution_state().snapshot();
    const auto title = kernel->GetExecutableModule();
    const auto& input = source->title_input()->identity();
    if (foreground.phase != TitleExecutionState::Phase::kRunning ||
        foreground.generation != output_provenance->foreground_generation() ||
        input.foreground_generation != foreground.generation || !title ||
        input.title_hmodule != title->hmodule_ptr() ||
        input.title_load_generation != title->load_generation()) {
      return false;
    }
    // Counter is the existing recovered root data cell, under the retained
    // actual root lease. An address being in RAM is insufficient admission.
    constexpr uint32_t kNativeFrameCounter = 0x81AA1B10;
    auto memory_lock = global_critical_region::AcquireDirect();
    return kernel->GetUserModuleForAddress(kNativeFrameCounter).get() ==
               root.get() &&
           guest_memory->QueryRangeAccess(kNativeFrameCounter, 4) !=
               memory::PageAccess::kNoAccess &&
           xe::load_and_swap<uint32_t>(
               guest_memory->TranslateVirtual(kNativeFrameCounter)) ==
               output_provenance->native_frame_counter();
  };
  if (!output_is_current()) {
    return {};
  }
  if (snapshot &&
      (!snapshot->valid() ||
       snapshot->identity() !=
           gpu::NativeFrameSubmissionIdentity{owner_, source->serial()} ||
       snapshot->plan() != plan ||
       snapshot->output_range().address != record.output_physical_address ||
       snapshot->output_range().length != record.output_allocation_bytes)) {
    return {};
  }
  Operation operation(shared_from_this());
  std::vector<std::shared_ptr<const memory::PhysicalRangePin>> pins;
  {
    std::lock_guard lock(mutex_);
    if (phase_ != Phase::kFrozen || active_operation_ ||
        !SourceMatches(source) || submission_attempted_ ||
        active_preparation_ || pins_.empty()) {
      return {};
    }
    operation.serial_ = StartOperationLocked();
    if (!operation.serial_) {
      return {};
    }
    pins = pins_;
  }
  const auto covered = [&](memory::PhysicalRange range) {
    if (!range.length || range.address >= 0x20000000 ||
        range.length > 0x20000000 - range.address) {
      return false;
    }
    for (const auto& pin : pins) {
      if (!pin || !pin->valid()) {
        continue;
      }
      for (const auto& allocation : pin->allocations()) {
        if (allocation.owner == owner_ &&
            range.address >= allocation.physical_address &&
            uint64_t(range.address) + range.length <=
                uint64_t(allocation.physical_address) + allocation.length) {
          return true;
        }
      }
    }
    return false;
  };
  const memory::PhysicalRange output{record.output_physical_address,
                                     record.output_allocation_bytes};
  const memory::PhysicalRange primary{record.primary_physical_address,
                                      uint32_t(record.primary.size())};
  if (!covered(output) || !covered(primary) || inputs.empty()) {
    return {};
  }
  for (const auto range : inputs) {
    if (!covered(range) &&
        !(source->title_input() && source->title_input()->Covers(range))) {
      return {};
    }
  }
  const auto& draw = plan->inventory().draws.back();
  const auto vertices =
      gpu::NativeDrawReadInventory::VertexReads(draw, nullptr);
  if (vertices.error != gpu::NativeDrawReadInventory::Error::kNone ||
      vertices.reads.size() != 1 || !vertices.reads[0].resolve_rectangle ||
      vertices.reads[0].byte_count != 24) {
    return {};
  }
  const auto& read = vertices.reads[0];
  const memory::PhysicalRange rectangle{read.physical_address, 24};
  if (!covered(rectangle) ||
      !std::any_of(inputs.begin(), inputs.end(),
                   [&](memory::PhysicalRange range) {
                     return rectangle.address >= range.address &&
                            uint64_t(rectangle.address) + rectangle.length <=
                                uint64_t(range.address) + range.length;
                   })) {
    return {};
  }
  using Proof = gpu::NativeFramePm4ResolveProof;
  const auto word = [&](uint32_t at) {
    return uint32_t(descriptor[at]) << 24 | uint32_t(descriptor[at + 1]) << 16 |
           uint32_t(descriptor[at + 2]) << 8 | uint32_t(descriptor[at + 3]);
  };
  const auto normalize =
      [&](uint32_t observed) -> std::optional<Proof::Mapping> {
    if (observed < 0x20000000) {
      return Proof::Mapping{Proof::Mapping::Kind::kCanonicalPhysical, observed,
                            observed};
    }
    auto* heap = guest_memory->LookupHeap(observed);
    if (!heap || heap->heap_type() != HeapType::kGuestPhysical) {
      return {};
    }
    // Real physical heap translation includes E/F's page bias. Never replace
    // this call with observed &0x1FFFFFFF or pretend it was an XG fetch header.
    const uint32_t physical = guest_memory->GetPhysicalAddress(observed);
    if (physical >= 0x20000000) {
      return {};
    }
    return Proof::Mapping{Proof::Mapping::Kind::kPhysicalHeapAlias, observed,
                          physical};
  };
  std::optional<Proof::Mapping> overlay_destination;
  if (native_overlay) {
    // Absence is authentic native optional-r5 state. Never synthesize a VD
    // texture header, address, dimensions or format from the output witness.
    if (word(8) || word(12) || word(20) || word(24) ||
        record.output_cpu_address) {
      return {};
    }
    const auto& program = plan->native_overlay_program();
    if (!program) {
      return {};
    }
    const auto requested =
        program->Apply(overlay_input_state.value_or(gpu::NativeOverlayState{}));
    if (!requested.valid() ||
        !requested.Has(gpu::NativeOverlayState::kGeometryKnown) ||
        requested.surface_address != output.address) {
      return {};
    }
    std::optional<uint32_t> destination;
    for (const auto& write : plan->register_writes()) {
      if (write.packet_dword_offset > draw.packet_dword_offset) {
        break;
      }
      if (write.index == gpu::XE_GPU_REG_RB_COPY_DEST_BASE) {
        destination = write.value;
      }
    }
    if (!destination || *destination != requested.surface_address ||
        *destination >= 0x20000000 || (*destination & 0xFFF)) {
      return {};
    }
    overlay_destination = Proof::Mapping{
        Proof::Mapping::Kind::kCanonicalPhysical, *destination, *destination};
  } else if (record.output_cpu_address != word(8)) {
    return {};
  }
  std::shared_ptr<const Proof> proof;
  {
    auto memory_lock = global_critical_region::AcquireDirect();
    const auto command = normalize(word(0));
    const auto destination =
        native_overlay ? overlay_destination : normalize(word(8));
    if (!command || !destination || command->physical != primary.address ||
        destination->physical != output.address ||
        (!snapshot && guest_memory->GetPhysicalHeap()->QueryRangeAccess(
                          rectangle.address, rectangle.address + 23) ==
                          memory::PageAccess::kNoAccess)) {
      return {};
    }
    std::array<uint32_t, 6> words;
    if (snapshot) {
      if (!snapshot->Read(rectangle,
                          std::span(reinterpret_cast<uint8_t*>(words.data()),
                                    sizeof(words)))) {
        return {};
      }
    } else {
      std::memcpy(words.data(),
                  guest_memory->TranslatePhysical(rectangle.address),
                  sizeof(words));
    }
    proof = std::shared_ptr<const Proof>(new Proof(
        {owner_, source->serial()}, descriptor, *command, *destination,
        draw.packet_dword_offset, rectangle, read.endian, words,
        native_overlay ? Proof::OutputOrigin::kNativeOverlay
                       : Proof::OutputOrigin::kVdTexture,
        native_overlay ? output_provenance->display_information()
                       : std::array<uint8_t, 0x58>{},
        overlay_input_state));
  }
  const auto result =
      gpu::DeriveNativePm4OutputLayout(*plan, output, inputs, proof);
  if (!result || !output_is_current()) {
    return {};
  }
  std::lock_guard lock(mutex_);
  if (phase_ != Phase::kFrozen || active_operation_ != operation.serial_ ||
      !SourceMatches(source) || submission_attempted_ || active_preparation_) {
    return {};
  }
  return result;
}

std::shared_ptr<const gpu::NativeFrameExecutionRequest>
NativeFrameOwnerSession::MintExecutionRequest(
    const ThreadIdentity& thread, const std::shared_ptr<const Source>& source,
    std::shared_ptr<const gpu::NativeFrameCommandPlan> plan,
    memory::PhysicalRange output,
    std::vector<memory::PhysicalRange> input_ranges,
    std::vector<gpu::NativeFrameExecutionRequest::Shader> shaders,
    std::optional<gpu::NativeFrameOutputLayout> output_layout,
    std::shared_ptr<const gpu::NativeFrameInputSnapshot> input_snapshot) {
  if (!MatchesThread(thread) || !source || !plan ||
      !PlanMatchesSource(*source, *plan)) {
    return {};
  }
  std::shared_ptr<GraphAuthority> graph;
  std::vector<std::shared_ptr<const memory::PhysicalRangePin>> pins;
  Operation operation(shared_from_this());
  uint64_t preparation;
  {
    std::lock_guard lock(mutex_);
    if (phase_ != Phase::kFrozen || active_operation_ ||
        !SourceMatches(source) || !graph_ || submission_attempted_ ||
        active_preparation_ || pins_.empty()) {
      return {};
    }
    if (next_preparation_ == UINT64_MAX) {
      QuarantineLocked();
      return {};
    }
    operation.serial_ = StartOperationLocked();
    if (!operation.serial_) {
      return {};
    }
    preparation = next_preparation_++;
    graph = graph_;
    pins = pins_;
  }
  uint64_t backend_epoch = 0;
  try {
    if (!RuntimeAdmitted(false) || !graph->Matches(owner_) ||
        (source->title_input() &&
         !graph->MatchesTitleInput(*source->title_input())) ||
        !(backend_epoch = graph->backend_epoch())) {
      return {};
    }
  } catch (...) {
    Revoke();
    throw;
  }
  if (source->title_input() &&
      (source->title_input() != plan->title_input() ||
       source->title_input()->identity().backend_epoch != backend_epoch)) {
    return {};
  }
  auto authority = std::make_shared<ExecutionAuthority>(
      shared_from_this(), source, plan, preparation, backend_epoch);
  const auto request = std::shared_ptr<const gpu::NativeFrameExecutionRequest>(
      new gpu::NativeFrameExecutionRequest(
          plan, {owner_, source->serial()}, backend_epoch, output,
          std::move(input_ranges), std::move(shaders), std::move(pins),
          std::move(authority), std::move(output_layout),
          std::move(input_snapshot)));
  if (!request->ResourcesValid()) {
    return {};
  }
  std::lock_guard lock(mutex_);
  if (phase_ != Phase::kFrozen || active_operation_ != operation.serial_ ||
      !SourceMatches(source) || submission_attempted_ || active_preparation_) {
    return {};
  }
  active_preparation_ = preparation;
  return request;
}

bool NativeFrameOwnerSession::IsPresentationOwnerLive(
    uint64_t backend_epoch) const {
  if (!backend_epoch) {
    return false;
  }
  const auto phase_live = [&] {
    return phase_ == Phase::kBound || phase_ == Phase::kFrozen ||
           phase_ == Phase::kSubmitted || phase_ == Phase::kRetiring;
  };
  std::shared_ptr<GraphAuthority> graph;
  {
    std::lock_guard lock(mutex_);
    if (!phase_live() || !graph_) {
      return false;
    }
    graph = graph_;
  }
  // No Source bytes or consumed preparation are consulted. Retaining this
  // graph keeps its control block alive; no Session lock spans its host checks.
  try {
    if (!RuntimeAdmitted(false) || !graph->Matches(owner_) ||
        graph->backend_epoch() != backend_epoch ||
        !graph->IsPresentationOwnerLive(owner_) || !RuntimeAdmitted(false) ||
        graph->backend_epoch() != backend_epoch) {
      return false;
    }
  } catch (...) {
    return false;
  }
  std::lock_guard lock(mutex_);
  return phase_live() && graph_ == graph;
}

bool NativeFrameOwnerSession::CheckExecutionRequest(
    const std::shared_ptr<const Source>& source,
    const std::shared_ptr<const gpu::NativeFrameCommandPlan>& plan,
    uint64_t preparation, uint64_t backend_epoch,
    const gpu::NativeFrameExecutionRequest& request, bool consume) {
  if (!preparation || !backend_epoch || !source ||
      request.identity() !=
          gpu::NativeFrameSubmissionIdentity{owner_, source->serial()} ||
      request.backend_epoch() != backend_epoch || request.plan() != plan) {
    return false;
  }
  std::shared_ptr<GraphAuthority> graph;
  Operation operation(shared_from_this());
  {
    std::lock_guard lock(mutex_);
    if (phase_ != Phase::kFrozen || active_operation_ ||
        !SourceMatches(source) || active_preparation_ != preparation ||
        submission_attempted_ || !graph_) {
      return false;
    }
    operation.serial_ = StartOperationLocked();
    if (!operation.serial_) {
      return false;
    }
    graph = graph_;
  }
  try {
    if (!RuntimeAdmitted(false) || !graph->Matches(owner_) ||
        (source->title_input() &&
         !graph->MatchesTitleInput(*source->title_input())) ||
        graph->backend_epoch() != backend_epoch ||
        (consume && !graph->CanExecuteNativeFrame(*source))) {
      Revoke();
      return false;
    }
  } catch (...) {
    Revoke();
    throw;
  }
  std::lock_guard lock(mutex_);
  if (phase_ != Phase::kFrozen || active_operation_ != operation.serial_ ||
      !SourceMatches(source) || active_preparation_ != preparation ||
      submission_attempted_) {
    return false;
  }
  if (consume) {
    // No CPU-only abort can release this frame after a consumer starts. A
    // failed submit still needs actual completion/stopped-GPU retirement.
    submission_attempted_ = true;
    active_preparation_ = 0;
  }
  return true;
}

std::shared_ptr<const NativeFrameOwnerSession::Source>
NativeFrameOwnerSession::FreezeCurrentUpdate(
    const ThreadIdentity& thread, std::span<const uint8_t> primary,
    std::shared_ptr<const gpu::NativeSystemTitleInput> title_input) {
  Update borrowed;
  {
    std::lock_guard lock(mutex_);
    borrowed.session_ = weak_from_this();
    borrowed.serial_ = active_update_;
  }
  try {
    auto source = Freeze(thread, borrowed, primary, std::move(title_input));
    // This is a view of the outer guard's admission, not a second owner of it.
    // Failed validation must not implicitly end the original update.
    borrowed.serial_ = 0;
    borrowed.session_.reset();
    return source;
  } catch (...) {
    borrowed.serial_ = 0;
    borrowed.session_.reset();
    throw;
  }
}

std::shared_ptr<const NativeFrameOwnerSession::Source>
NativeFrameOwnerSession::SourceForExecutionRequest(
    const ThreadIdentity& thread,
    const gpu::NativeFrameExecutionRequest& request) const {
  if (!MatchesThread(thread) || !request.IsAdmitted()) {
    return {};
  }
  std::lock_guard lock(mutex_);
  if (phase_ != Phase::kFrozen || active_operation_ || submission_attempted_ ||
      !active_preparation_ || !source_ ||
      request.identity() !=
          gpu::NativeFrameSubmissionIdentity{owner_, source_->serial()}) {
    return {};
  }
  return source_;
}

bool NativeFrameOwnerSession::AttachGpu(
    const ThreadIdentity& thread, const std::shared_ptr<const Source>& source,
    gpu::GpuCompletion completion) {
  if (!MatchesThread(thread) || !RuntimeAdmitted(false)) {
    return false;
  }
  std::shared_ptr<GraphAuthority> graph;
  Operation operation(shared_from_this());
  {
    std::lock_guard lock(mutex_);
    if (phase_ != Phase::kFrozen || active_operation_ ||
        !SourceMatches(source) || gpu_attached_) {
      return false;
    }
    operation.serial_ = StartOperationLocked();
    if (!operation.serial_) {
      return false;
    }
    // The caller may already have submitted. A failed ticket validation must
    // never make this graph eligible for a CPU-only stopped-owner proof.
    submission_attempted_ = true;
    graph = graph_;
  }
  try {
    if (!graph || !graph->Matches(owner_) || !completion.backend_epoch() ||
        !completion.submission() ||
        completion.status() == gpu::GpuCompletionStatus::kUnsupported ||
        completion.status() == gpu::GpuCompletionStatus::kFailed ||
        !graph->ValidateNativeSubmission(*source, completion)) {
      Revoke();
      return false;
    }
  } catch (...) {
    Revoke();
    throw;
  }
  std::lock_guard lock(mutex_);
  if (phase_ != Phase::kFrozen || active_operation_ != operation.serial_ ||
      !SourceMatches(source)) {
    return false;
  }
  completion_ = std::move(completion);
  observed_ = completion_.status();
  gpu_attached_ = true;
  phase_ = Phase::kSubmitted;
  return true;
}

bool NativeFrameOwnerSession::BeginRetire(
    const ThreadIdentity& thread, const std::shared_ptr<const Source>& source) {
  if (!MatchesThread(thread)) {
    return false;
  }
  std::lock_guard lock(mutex_);
  if (phase_ != Phase::kSubmitted || active_operation_ ||
      !SourceMatches(source)) {
    return false;
  }
  phase_ = Phase::kRetiring;
  return true;
}

void NativeFrameOwnerSession::ObserveCompletion(
    const gpu::GpuCompletion& completion) {
  std::lock_guard lock(mutex_);
  if (!gpu_attached_ ||
      completion.backend_epoch() != completion_.backend_epoch() ||
      completion.submission() != completion_.submission()) {
    return;
  }
  observed_ = completion_.status();
  if (observed_ == gpu::GpuCompletionStatus::kFailed ||
      observed_ == gpu::GpuCompletionStatus::kUnsupported) {
    QuarantineLocked();
  }
}

bool NativeFrameOwnerSession::AbortUnsubmitted(
    const ThreadIdentity& thread, const std::shared_ptr<const Source>& source) {
  return FinishFrame(thread, source, false);
}
bool NativeFrameOwnerSession::RetireComplete(
    const ThreadIdentity& thread, const std::shared_ptr<const Source>& source) {
  return FinishFrame(thread, source, true);
}
bool NativeFrameOwnerSession::FinishFrame(
    const ThreadIdentity& thread, const std::shared_ptr<const Source>& source,
    bool submitted) {
  if (!MatchesThread(thread) || !RuntimeAdmitted(false)) {
    return false;
  }
  std::shared_ptr<GraphAuthority> graph;
  gpu::GpuCompletion completion;
  Operation operation(shared_from_this());
  {
    std::lock_guard lock(mutex_);
    const bool phase_ok =
        submitted ? (phase_ == Phase::kSubmitted || phase_ == Phase::kRetiring)
                  : phase_ == Phase::kFrozen;
    if (!phase_ok || active_operation_ || !SourceMatches(source) ||
        gpu_attached_ != submitted || (!submitted && submission_attempted_) ||
        (submitted &&
         completion_.status() != gpu::GpuCompletionStatus::kComplete)) {
      return false;
    }
    operation.serial_ = StartOperationLocked();
    if (!operation.serial_) {
      return false;
    }
    graph = graph_;
    completion = completion_;
  }
  try {
    if (!graph || !graph->Matches(owner_) ||
        !(submitted ? graph->RetireComplete(*source, completion)
                    : graph->AbortUnsubmitted(*source))) {
      Revoke();
      return false;
    }
  } catch (...) {
    Revoke();
    throw;
  }
  std::vector<std::shared_ptr<const memory::PhysicalRangePin>> released_pins;
  std::shared_ptr<const Source> released_source;
  std::shared_ptr<NativeFrameOwnerSession> released_self;
  std::shared_ptr<void> released_writer;
  std::shared_ptr<void> released_preparation;
  {
    std::lock_guard lock(mutex_);
    if (phase_ == Phase::kQuarantined ||
        active_operation_ != operation.serial_ || !SourceMatches(source)) {
      return false;
    }
    pins_.swap(released_pins);
    source_.swap(released_source);
    quarantine_pin_.swap(released_self);
    completion_ = {};
    observed_ = gpu::GpuCompletionStatus::kUnsupported;
    gpu_attached_ = false;
    active_preparation_ = 0;
    if (submitted) {
      submission_attempted_ = false;
    }
    phase_ = Phase::kBound;
    frame_writer_.swap(released_writer);
    preparation_pins_.swap(released_preparation);
  }
  return true;
}

bool NativeFrameOwnerSession::MatchesCompletedRecord(
    const ThreadIdentity& thread, const std::shared_ptr<const Source>& source,
    uint32_t device, uint32_t dc) {
  if (!delegated_ || !MatchesThread(thread) || !RuntimeAdmitted(false)) {
    return false;
  }
  std::shared_ptr<GraphAuthority> graph;
  Operation operation(shared_from_this());
  {
    std::lock_guard lock(mutex_);
    if (phase_ != Phase::kFrozen || active_operation_ ||
        !SourceMatches(source) || submission_attempted_) {
      return false;
    }
    operation.serial_ = StartOperationLocked();
    graph = graph_;
  }
  if (!operation.serial_ || !graph ||
      !graph->MatchesCompletedRecord(*source, device, dc)) {
    return false;
  }
  std::lock_guard lock(mutex_);
  return phase_ == Phase::kFrozen && active_operation_ == operation.serial_ &&
         SourceMatches(source);
}

bool NativeFrameOwnerSession::HoldPreparationPins(
    const ThreadIdentity& thread, const std::shared_ptr<const Source>& source,
    std::shared_ptr<void> pins) {
  if (!pins || !MatchesThread(thread)) {
    return false;
  }
  std::lock_guard lock(mutex_);
  if (phase_ != Phase::kFrozen || active_operation_ || submission_attempted_ ||
      !SourceMatches(source) || preparation_pins_) {
    return false;
  }
  preparation_pins_ = std::move(pins);
  return true;
}

bool NativeFrameOwnerSession::RetireDelegatedIdle() {
  std::shared_ptr<GraphAuthority> released;
  {
    std::lock_guard lock(mutex_);
    if (!delegated_ || phase_ != Phase::kBound || active_update_ ||
        active_operation_ || source_ || !pins_.empty() || gpu_attached_ ||
        submission_attempted_ || frame_writer_) {
      return false;
    }
    phase_ = Phase::kRetired;
    graph_.swap(released);
  }
  return true;
}

void NativeFrameOwnerSession::QuarantineLocked() {
  if (phase_ != Phase::kRetired) {
    phase_ = Phase::kQuarantined;
    active_update_ = 0;
    active_preparation_ = 0;
    quarantine_pin_ = shared_from_this();
  }
}
void NativeFrameOwnerSession::Revoke() {
  std::lock_guard lock(mutex_);
  QuarantineLocked();
}

bool NativeFrameOwnerSession::RetireAfterKernelStop(
    const std::function<bool(const Owner&)>& proof) {
  if (!proof) {
    return false;
  }
  std::shared_ptr<GraphAuthority> graph;
  Operation operation(shared_from_this());
  {
    std::lock_guard lock(mutex_);
    if (phase_ == Phase::kRetired) {
      return true;
    }
    if (active_operation_) {
      return false;
    }
    QuarantineLocked();
    operation.serial_ = StartOperationLocked();
    if (!operation.serial_) {
      return false;
    }
    graph = graph_;
  }
  try {
    if (!proof(owner_) || (graph && !graph->RetireAfterKernelStop(owner_))) {
      Revoke();
      return false;
    }
  } catch (...) {
    Revoke();
    throw;
  }
  std::vector<std::shared_ptr<const memory::PhysicalRangePin>> released_pins;
  std::shared_ptr<const Source> released_source;
  std::shared_ptr<NativeFrameOwnerSession> released_self;
  std::shared_ptr<GraphAuthority> released_graph;
  std::shared_ptr<void> released_writer;
  std::shared_ptr<void> released_preparation;
  {
    std::lock_guard lock(mutex_);
    if (active_operation_ != operation.serial_ ||
        phase_ != Phase::kQuarantined) {
      return false;
    }
    pins_.swap(released_pins);
    source_.swap(released_source);
    quarantine_pin_.swap(released_self);
    graph_.swap(released_graph);
    frame_writer_.swap(released_writer);
    preparation_pins_.swap(released_preparation);
    completion_ = {};
    phase_ = Phase::kRetired;
    gpu_attached_ = false;
    submission_attempted_ = false;
    active_preparation_ = 0;
  }
  return true;
}

NativeFrameOwnerSession::Snapshot NativeFrameOwnerSession::GetSnapshot() const {
  std::lock_guard lock(mutex_);
  return {owner_,
          thread_,
          phase_,
          active_update_ != 0,
          active_operation_ != 0,
          gpu_attached_,
          source_ ? source_->serial() : 0,
          pins_.size(),
          observed_,
          submission_attempted_};
}
std::unique_ptr<NativeFrameInputCapture>
NativeFrameOwnerSession::BeginInputCapture(
    Memory* memory, const ThreadIdentity& thread,
    const std::shared_ptr<const Source>& source,
    const std::shared_ptr<const gpu::NativeFrameCommandPlan>& plan,
    memory::PhysicalRange primary, memory::PhysicalRange output) {
  const auto valid = [](memory::PhysicalRange range) {
    return range.length &&
           range.length <= gpu::NativeFrameInputSnapshot::kMaxReadBytes &&
           range.address < 0x20000000 &&
           range.length <= 0x20000000 - range.address;
  };
  if (!memory || !source || !plan || !MatchesThread(thread) ||
      !PlanMatchesSource(*source, *plan) || !valid(primary) ||
      (plan->is_overlay_control() ? (output.address || output.length)
                                  : !valid(output)) ||
      primary.length != source->primary().size() ||
      (uint64_t(primary.address) < uint64_t(output.address) + output.length &&
       uint64_t(output.address) < uint64_t(primary.address) + primary.length)) {
    return {};
  }
  if (source->title_input() &&
      (!source->title_input()->valid() ||
       source->title_input() != plan->title_input() ||
       (uint64_t(output.address) <
            uint64_t(source->title_input()->range().address) +
                source->title_input()->range().length &&
        uint64_t(source->title_input()->range().address) <
            uint64_t(output.address) + output.length))) {
    return {};
  }
  auto capture =
      std::unique_ptr<NativeFrameInputCapture>(new NativeFrameInputCapture(
          shared_from_this(), memory, thread, source, plan, primary, output));
  {
    std::lock_guard lock(mutex_);
    if (phase_ != Phase::kFrozen || active_operation_ ||
        !SourceMatches(source) || submission_attempted_ ||
        active_preparation_) {
      return {};
    }
    bool output_pinned = plan->is_overlay_control();
    for (const auto& pin : pins_) {
      if (!pin || !pin->valid()) {
        continue;
      }
      for (const auto& allocation : pin->allocations()) {
        output_pinned |=
            allocation.owner == owner_ &&
            output.address >= allocation.physical_address &&
            uint64_t(output.address) + output.length <=
                uint64_t(allocation.physical_address) + allocation.length;
      }
    }
    if (!output_pinned) {
      return {};
    }
    capture->operation_ = StartOperationLocked();
  }
  if (!capture->operation_ || !capture->Capture(primary)) {
    return {};
  }
  return capture;
}
}  // namespace xe::kernel
