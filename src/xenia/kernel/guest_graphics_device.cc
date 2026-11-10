/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia contributors. All rights reserved.                    *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/guest_graphics_device.h"

#include <algorithm>
#include <atomic>
#include <limits>
#include <utility>

namespace xe::kernel {
namespace {
std::atomic<uint64_t> next_publication{1};

uint64_t NewPublicationSerial() {
  uint64_t value = next_publication.load(std::memory_order_relaxed);
  while (value) {
    const uint64_t next =
        value == std::numeric_limits<uint64_t>::max() ? 0 : value + 1;
    if (next_publication.compare_exchange_weak(value, next,
                                               std::memory_order_relaxed)) {
      return value;
    }
  }
  return 0;
}
}  // namespace

struct GuestGraphicsDevice::Use {
  Use(std::shared_ptr<GuestGraphicsDevice> owner, Leases leases)
      : owner(std::move(owner)), leases(std::move(leases)) {}
  ~Use() { owner->EndCpuUse(); }
  std::shared_ptr<GuestGraphicsDevice> owner;
  Leases leases;
};

GuestGraphicsDevice::Identity GuestGraphicsDevice::UseLease::identity() const {
  return use_ ? use_->owner->identity_ : Identity{};
}

gpu::GpuCompletionResources GuestGraphicsDevice::UseLease::resources() const {
  if (!use_) {
    return {};
  }
  return {use_->leases.module, use_->leases.device_graph};
}

std::shared_ptr<GuestGraphicsDevice> GuestGraphicsDevice::Create(
    Identity identity, Leases leases, uint64_t backend_epoch) {
  // TITLE1 and SYSTEM2 are the supported kernel graphics process classes.
  if (!identity.kernel_epoch || !identity.module_hmodule ||
      (identity.module_hmodule & 3) || !identity.module_generation ||
      !identity.guest_device || (identity.guest_device & 3) ||
      (identity.process_type != 1 && identity.process_type != 2) ||
      !leases.module || !leases.device_graph) {
    return {};
  }
  const uint64_t serial = NewPublicationSerial();
  if (!serial) {
    return {};
  }
  return std::shared_ptr<GuestGraphicsDevice>(new GuestGraphicsDevice(
      identity, std::move(leases), backend_epoch, serial));
}

GuestGraphicsDevice::GuestGraphicsDevice(Identity identity, Leases leases,
                                         uint64_t backend_epoch,
                                         uint64_t serial)
    : identity_(identity),
      backend_epoch_(backend_epoch),
      publication_{serial, 0, identity},
      leases_(std::move(leases)) {}

GuestGraphicsDevice::~GuestGraphicsDevice() = default;

std::optional<GuestGraphicsDevice::Publication> GuestGraphicsDevice::Publish(
    uint32_t cell_address, const CellReader& reader,
    const CellCas& compare_exchange) {
  auto keep_alive = shared_from_this();
  std::lock_guard lock(mutex_);
  if (attempted_ || phase_ != Phase::kPrepared || !cell_address ||
      (cell_address & 3) || !reader || !compare_exchange) {
    return {};
  }
  attempted_ = true;
  publication_.cell_address = cell_address;
  // Arm retention before any host callback: a callback may throw after its
  // memory side effect. Unknown/forced unwind is rethrown, never swallowed.
  self_pin_ = keep_alive;
  phase_ = Phase::kQuarantined;
  poisoned_ = true;
  const auto observed = reader(cell_address);
  if (!observed || *observed != 0) {
    // No CAS was attempted and this owner cannot have published the cell.
    // Keep the graph until explicit retirement, without touching that cell.
    may_own_cell_ = false;
    phase_ = Phase::kRejected;
    poisoned_ = false;
    return {};
  }
  may_own_cell_ = true;
  if (!compare_exchange(cell_address, 0, identity_.guest_device)) {
    return {};  // Could have written: keep quarantine, never retry Publish.
  }
  poisoned_ = false;
  phase_ = Phase::kPublished;
  return publication_;
}

bool GuestGraphicsDevice::Matches(const Publication& publication) const {
  return attempted_ && publication.cell_address && publication == publication_;
}

GuestGraphicsDevice::UseLease GuestGraphicsDevice::AcquireUse(
    const Publication& publication) {
  auto keep_alive = shared_from_this();
  std::lock_guard lock(mutex_);
  ObserveFailedGpuUses();
  if (!Matches(publication) || phase_ != Phase::kPublished ||
      cpu_uses_ == std::numeric_limits<size_t>::max()) {
    return {};
  }
  auto use = std::make_shared<Use>(std::move(keep_alive), leases_);
  ++cpu_uses_;
  return UseLease(std::move(use));
}

void GuestGraphicsDevice::EndCpuUse() {
  std::lock_guard lock(mutex_);
  if (cpu_uses_) {
    --cpu_uses_;
  }
  // No callbacks or graph release from the last lease's destructor. The
  // owner explicitly polls retirement while the public cell is still alive.
}

void GuestGraphicsDevice::PruneCompleted() {
  std::erase_if(gpu_uses_, [&](const auto& token) {
    return token.backend_epoch() == backend_epoch_ && backend_epoch_ &&
           token.status() == gpu::GpuCompletionStatus::kComplete;
  });
}

void GuestGraphicsDevice::ObserveFailedGpuUses() {
  if (phase_ == Phase::kRetired || gpu_stopped_) {
    return;
  }
  for (const auto& token : gpu_uses_) {
    if (!backend_epoch_ || token.backend_epoch() != backend_epoch_ ||
        token.status() == gpu::GpuCompletionStatus::kFailed ||
        token.status() == gpu::GpuCompletionStatus::kUnsupported) {
      poisoned_ = true;
      phase_ = Phase::kQuarantined;
    }
  }
}

bool GuestGraphicsDevice::TrackGpuUse(const UseLease& use,
                                      gpu::GpuCompletion completion) {
  std::lock_guard lock(mutex_);
  if (!use.use_ || use.use_->owner.get() != this || !cpu_uses_ ||
      phase_ == Phase::kRetired || gpu_stopped_) {
    return false;
  }
  ObserveFailedGpuUses();
  PruneCompleted();
  // Keep even rejected markers: a foreign/unsupported marker cannot prove
  // the real work stopped. The graph is quarantined until actual GPU stop.
  const bool valid =
      backend_epoch_ && completion.backend_epoch() == backend_epoch_ &&
      completion.submission() &&
      completion.status() != gpu::GpuCompletionStatus::kFailed &&
      completion.status() != gpu::GpuCompletionStatus::kUnsupported;
  const bool previous_poison = poisoned_;
  const Phase previous_phase = phase_;
  // A failed allocation here must not lose a submitted GPU use and then
  // allow retirement based on an empty marker vector.
  poisoned_ = true;
  phase_ = Phase::kQuarantined;
  gpu_uses_.push_back(std::move(completion));
  if (valid && !previous_poison) {
    poisoned_ = false;
    phase_ = previous_phase;
  }
  return valid && !previous_poison;
}

bool GuestGraphicsDevice::BeginRetire(const Publication& publication) {
  std::lock_guard lock(mutex_);
  if (!Matches(publication)) {
    return false;
  }
  if (phase_ != Phase::kRetired && phase_ != Phase::kQuarantined) {
    phase_ = Phase::kRetiring;
  }
  return true;
}

GuestGraphicsDevice::RetireStatus GuestGraphicsDevice::RetireLocked(
    const CellReader& reader, const CellCas& compare_exchange, Leases& released,
    std::shared_ptr<GuestGraphicsDevice>& self_pin) {
  if (phase_ == Phase::kRetired) {
    return retired_foreign_ ? RetireStatus::kRetiredForeign
                            : RetireStatus::kRetired;
  }
  if (cpu_uses_) {
    return RetireStatus::kPendingCpu;
  }
  if (!gpu_stopped_) {
    ObserveFailedGpuUses();
    if (poisoned_) {
      phase_ = Phase::kQuarantined;
      return RetireStatus::kQuarantined;
    }
    if (std::any_of(gpu_uses_.begin(), gpu_uses_.end(), [](const auto& token) {
          return token.status() != gpu::GpuCompletionStatus::kComplete;
        })) {
      return RetireStatus::kPendingGpu;
    }
  }
  if (may_own_cell_) {
    if (!reader || !compare_exchange) {
      return RetireStatus::kCellUnavailable;
    }
    // Retain ownership on all exceptional/uncertain paths. If CAS reports
    // false, a checked read is required before concluding our pointer left.
    poisoned_ = true;
    phase_ = Phase::kQuarantined;
    const auto observed = reader(publication_.cell_address);
    if (!observed) {
      return RetireStatus::kCellUnavailable;
    }
    if (*observed != identity_.guest_device) {
      retired_foreign_ = true;
    } else if (!compare_exchange(publication_.cell_address,
                                 identity_.guest_device, 0)) {
      const auto after = reader(publication_.cell_address);
      if (!after || *after == identity_.guest_device) {
        poisoned_ = true;
        phase_ = Phase::kQuarantined;
        return after ? RetireStatus::kQuarantined
                     : RetireStatus::kCellUnavailable;
      }
      retired_foreign_ = true;
    }
    may_own_cell_ = false;
  }
  phase_ = Phase::kRetired;
  gpu_uses_.clear();
  released = std::move(leases_);
  self_pin = std::move(self_pin_);
  return retired_foreign_ ? RetireStatus::kRetiredForeign
                          : RetireStatus::kRetired;
}

GuestGraphicsDevice::RetireStatus GuestGraphicsDevice::PollRetirement(
    const Publication& publication, const CellReader& reader,
    const CellCas& compare_exchange) {
  auto keep_alive = shared_from_this();
  Leases released;
  std::shared_ptr<GuestGraphicsDevice> self_pin;
  std::lock_guard lock(mutex_);
  if (!Matches(publication) || phase_ == Phase::kPublished ||
      phase_ == Phase::kPrepared) {
    return RetireStatus::kRejected;
  }
  return RetireLocked(reader, compare_exchange, released, self_pin);
  // lock is destroyed before released/self_pin/keep_alive. Their guest-only
  // lease deleters therefore never run under this mutex.
}

GuestGraphicsDevice::RetireStatus GuestGraphicsDevice::RetireAfterGpuStopped(
    const Publication& publication, const CellReader& reader,
    const CellCas& compare_exchange, const GpuStopProof& proof) {
  auto keep_alive = shared_from_this();
  if (!proof || !BeginRetire(publication)) {
    return RetireStatus::kRejected;
  }
  if (!proof(identity_.kernel_epoch, backend_epoch_)) {
    return RetireStatus::kProofRejected;
  }
  Leases released;
  std::shared_ptr<GuestGraphicsDevice> self_pin;
  std::lock_guard lock(mutex_);
  if (!Matches(publication)) {
    return RetireStatus::kRejected;
  }
  gpu_stopped_ = true;
  return RetireLocked(reader, compare_exchange, released, self_pin);
}

GuestGraphicsDevice::Snapshot GuestGraphicsDevice::snapshot() const {
  std::lock_guard lock(mutex_);
  return {publication_,     phase_,     backend_epoch_, cpu_uses_,
          gpu_uses_.size(), attempted_, may_own_cell_,  gpu_stopped_};
}

}  // namespace xe::kernel
