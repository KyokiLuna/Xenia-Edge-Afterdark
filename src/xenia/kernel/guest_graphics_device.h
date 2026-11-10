/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia contributors. All rights reserved.                    *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_GUEST_GRAPHICS_DEVICE_H_
#define XENIA_KERNEL_GUEST_GRAPHICS_DEVICE_H_

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

#include "xenia/gpu/gpu_completion.h"

namespace xe::kernel {

// Owns publication of a caller-validated real guest device. This class never
// constructs a device, interprets native private fields, or executes guest
// code. The caller must classify and lease every CPU/GPU consumer before use.
class GuestGraphicsDevice
    : public std::enable_shared_from_this<GuestGraphicsDevice> {
  struct Use;

 public:
  struct Identity {
    uint64_t kernel_epoch = 0;
    uint32_t module_hmodule = 0;
    uint64_t module_generation = 0;
    uint32_t guest_device = 0;
    uint32_t process_type = 0;
    bool operator==(const Identity&) const = default;
  };
  struct Leases {
    std::shared_ptr<void> module;
    std::shared_ptr<void> device_graph;
  };
  struct Publication {
    uint64_t serial = 0;
    uint32_t cell_address = 0;
    Identity identity;
    bool operator==(const Publication&) const = default;
  };
  enum class Phase {
    kPrepared,
    kPublished,
    kRetiring,
    kQuarantined,
    kRejected,
    kRetired,
  };
  struct Snapshot {
    Publication publication;
    Phase phase = Phase::kPrepared;
    uint64_t backend_epoch = 0;
    size_t cpu_uses = 0;
    size_t gpu_uses = 0;
    bool publication_attempted = false;
    bool cell_may_be_owned = false;
    bool gpu_stopped = false;
  };
  enum class RetireStatus {
    kRejected,
    kPendingCpu,
    kPendingGpu,
    kQuarantined,
    kCellUnavailable,
    kProofRejected,
    kRetired,
    kRetiredForeign,
  };
  class UseLease {
   public:
    UseLease() = default;
    explicit operator bool() const { return bool(use_); }
    Identity identity() const;
    gpu::GpuCompletionResources resources() const;

   private:
    friend class GuestGraphicsDevice;
    explicit UseLease(std::shared_ptr<Use> use) : use_(std::move(use)) {}
    std::shared_ptr<Use> use_;
  };

  // Logical guest DWORD access only. The integration supplies checked,
  // aligned, endian-correct host memory operations. Called under this mutex:
  // no PPC, blocking work, resource destruction or state reentry is allowed.
  // A false CAS may be an uncertain outcome; it never alone releases leases.
  using CellReader = std::function<std::optional<uint32_t>(uint32_t)>;
  using CellCas = std::function<bool(uint32_t, uint32_t, uint32_t)>;
  // Called outside the mutex, after closing admission. The owner must prove
  // the exact kernel/backend stopped and that no more GPU work can be queued.
  // This is not a timeout, an observer, or CPU command-decode progress.
  using GpuStopProof = std::function<bool(uint64_t, uint64_t)>;

  // Guest-only leases: neither may own host Vulkan/D3D/Metal handles or execute
  // PPC in its deleter. Epoch0 permits CPU-only ownership; any GPU attachment
  // then quarantines rather than inventing a backend/completion identity.
  static std::shared_ptr<GuestGraphicsDevice> Create(Identity identity,
                                                     Leases leases,
                                                     uint64_t backend_epoch);
  ~GuestGraphicsDevice();

  // One checked publication attempt. The only store is CAS(0, real_device).
  // An uncertain CAS/exception keeps a self-pin, closed admission and graph
  // quarantine. On failure, snapshot().publication identifies that attempt
  // for explicit retirement. No failed attempt may be retried/rebound.
  std::optional<Publication> Publish(uint32_t cell_address,
                                     const CellReader& reader,
                                     const CellCas& compare_exchange);
  UseLease AcquireUse(const Publication& publication);
  // Keep this CPU lease alive until the real strict marker is attached. It
  // may attach after BeginRetire, since that operation is already admitted.
  bool TrackGpuUse(const UseLease& use, gpu::GpuCompletion completion);
  bool BeginRetire(const Publication& publication);
  RetireStatus PollRetirement(const Publication& publication,
                              const CellReader& reader,
                              const CellCas& compare_exchange);
  RetireStatus RetireAfterGpuStopped(const Publication& publication,
                                     const CellReader& reader,
                                     const CellCas& compare_exchange,
                                     const GpuStopProof& proof);
  Snapshot snapshot() const;

 private:
  GuestGraphicsDevice(Identity identity, Leases leases, uint64_t backend_epoch,
                      uint64_t serial);
  bool Matches(const Publication& publication) const;
  void EndCpuUse();
  void PruneCompleted();
  void ObserveFailedGpuUses();
  RetireStatus RetireLocked(const CellReader& reader,
                            const CellCas& compare_exchange, Leases& released,
                            std::shared_ptr<GuestGraphicsDevice>& self_pin);

  const Identity identity_;
  const uint64_t backend_epoch_;
  mutable std::mutex mutex_;
  Publication publication_;
  Phase phase_ = Phase::kPrepared;
  Leases leases_;
  size_t cpu_uses_ = 0;
  std::vector<gpu::GpuCompletion> gpu_uses_;
  bool attempted_ = false;
  bool may_own_cell_ = false;
  bool poisoned_ = false;
  bool gpu_stopped_ = false;
  bool retired_foreign_ = false;
  // Publication survives an accidentally dropped host handle. Integration
  // retains an external registry reference so it can retire after GPU stop.
  std::shared_ptr<GuestGraphicsDevice> self_pin_;
};

}  // namespace xe::kernel

#endif  // XENIA_KERNEL_GUEST_GRAPHICS_DEVICE_H_
