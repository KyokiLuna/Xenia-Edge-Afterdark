/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia contributors. All rights reserved.                    *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_GPU_GPU_COMPLETION_H_
#define XENIA_GPU_GPU_COMPLETION_H_

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "xenia/base/physical_range_leases.h"

namespace xe::gpu {

enum class GpuCompletionStatus { kUnsupported, kPending, kComplete, kFailed };
// Guest allocation/module lifetime leases only. These must not own Vulkan /
// D3D / Metal handles whose destructors require a still-live host GPU device.
using GpuCompletionResources = std::vector<std::shared_ptr<void>>;

// A resource-retirement boundary, not successful rendering or guest completion.
// The epoch is immutable. Evidence is published once by a trusted backend after
// an actual stop boundary; readers need no live CP, queue or GPU device.
class GpuBackendStopProof {
 public:
  enum class Evidence {
    kNotStopped,
    kExplicitGpuStopped,
    kDeviceLostWaitIdle,
    kDeviceDestroyed,
  };

  GpuBackendStopProof() = default;
  uint64_t backend_epoch() const;
  Evidence evidence() const;
  bool stopped() const { return evidence() != Evidence::kNotStopped; }

 private:
  friend class GpuCompletionQueue;
  struct State {
    explicit State(uint64_t backend_epoch) : epoch(backend_epoch) {}
    const uint64_t epoch;
    std::atomic<Evidence> evidence{Evidence::kNotStopped};
  };
  explicit GpuBackendStopProof(std::shared_ptr<State> state)
      : state_(std::move(state)) {}
  std::shared_ptr<State> state_;
};

// Exact source identity attached by a checked native-frame submitter. This is
// provenance, not execution authority: the native owner still checks its graph,
// input leases, backend epoch, source and actual submission outcome separately.
struct NativeFrameSubmissionIdentity {
  memory::PhysicalAllocationOwner owner;
  uint64_t source_serial = 0;
  bool valid() const { return owner.valid() && source_serial; }
  bool operator==(const NativeFrameSubmissionIdentity&) const = default;
};

// A copyable, backend-independent marker. Reading status is safe off the CP
// thread and does not dereference the backend or poll a GPU API. It does not
// itself acknowledge any guest identifier or classify a graphics owner.
class GpuCompletion {
 public:
  using Observer = std::function<void(const GpuCompletion&)>;
  GpuCompletion() = default;
  GpuCompletionStatus status() const;
  uint64_t backend_epoch() const;
  // The exact queue's independent resource-retirement boundary. This remains
  // readable after queue destruction and cannot turn Failed into Complete.
  GpuBackendStopProof backend_stop_proof() const;
  uint64_t submission() const;
  std::optional<NativeFrameSubmissionIdentity> native_frame_identity() const;
  static GpuCompletion Unsupported(Observer observer = {});

 private:
  friend class GpuCompletionQueue;
  struct State {
    State(uint64_t epoch, uint64_t submission, GpuCompletionStatus status,
          std::optional<NativeFrameSubmissionIdentity> native_frame,
          GpuBackendStopProof stop_proof)
        : epoch(epoch),
          submission(submission),
          native_frame(std::move(native_frame)),
          stop_proof(std::move(stop_proof)),
          status(status) {}
    const uint64_t epoch;
    const uint64_t submission;
    const std::optional<NativeFrameSubmissionIdentity> native_frame;
    const GpuBackendStopProof stop_proof;
    std::atomic<GpuCompletionStatus> status;
  };
  explicit GpuCompletion(std::shared_ptr<State> state)
      : state_(std::move(state)) {}
  std::shared_ptr<State> state_;
};

// Owned and mutated only by one backend's CP thread. Record a marker only
// after successful submission; CompleteThrough accepts actual fence progress,
// never CPU decode progress, a timeout, or a memexport-only completion point.
// Dropping a token cannot release resources still held by this queue.
class GpuCompletionQueue {
 public:
  GpuCompletionQueue();
  GpuCompletionQueue(const GpuCompletionQueue&) = delete;
  GpuCompletionQueue& operator=(const GpuCompletionQueue&) = delete;
  // The owner must prove the GPU stopped, or transfer TakeFailedResources() to
  // a device-lifetime quarantine, before destroying this queue with resources.
  ~GpuCompletionQueue();

  GpuCompletion TrackSubmitted(uint64_t submission,
                               GpuCompletionResources resources = {},
                               GpuCompletion::Observer observer = {});
  // Only after an actual checked native submission. Ordinary title fences
  // deliberately have no source identity. This queue preserves even malformed
  // metadata without dropping submitted resources; admission belongs to the
  // submitter and retirement must reject mismatched/invalid identities.
  GpuCompletion TrackNativeFrameSubmitted(
      uint64_t submission, NativeFrameSubmissionIdentity identity,
      GpuCompletionResources resources = {},
      GpuCompletion::Observer observer = {});
  void CompleteThrough(uint64_t completed_submission);
  void FailPending();
  // Transfer failed guest leases to an owner that outlives actual GPU device
  // destruction when a wait cannot prove quiescence. Tokens remain Failed.
  GpuCompletionResources TakeFailedResources();
  // Explicit backend assertions only. Neither a stale completion counter nor
  // observing a lost-device flag is a resource-retirement boundary.
  void ReleaseAfterGpuStopped();
  // Call only after actual device-wide vkDeviceWaitIdle returns
  // VK_ERROR_DEVICE_LOST, with every queue host-synchronized and native
  // submissions stopped. One queue's wait cannot cover auxiliary queues.
  void ReleaseAfterDeviceLostWaitIdle();
  GpuBackendStopProof backend_stop_proof() const {
    return GpuBackendStopProof(stop_state_);
  }
  // Install in the real device's after-vkDestroyDevice guest-lease drain. It
  // retains only shared proof state, never the queue, Kernel or Vulkan objects.
  // Merely constructing or destroying this callback publishes no evidence.
  std::function<void()> MakeAfterDeviceDestroyedCallback() const;
  bool has_pending() const { return !pending_.empty(); }
  uint64_t backend_epoch() const { return epoch_; }

 private:
  struct Entry {
    GpuCompletion token;
    GpuCompletionResources resources;
    GpuCompletion::Observer observer;
  };
  static void Notify(Entry& entry);
  static void PublishStopProof(
      const std::shared_ptr<GpuBackendStopProof::State>& state,
      GpuBackendStopProof::Evidence evidence);
  void CleanupWithoutStopProof();
  GpuCompletion TrackSubmittedImpl(
      uint64_t submission,
      std::optional<NativeFrameSubmissionIdentity> native_frame,
      GpuCompletionResources resources, GpuCompletion::Observer observer);
  const uint64_t epoch_;
  const std::shared_ptr<GpuBackendStopProof::State> stop_state_;
  uint64_t completed_through_ = 0;
  bool failed_ = false;
  bool stopped_ = false;
  std::vector<Entry> pending_;
  std::vector<Entry> quarantine_;
};

}  // namespace xe::gpu

#endif  // XENIA_GPU_GPU_COMPLETION_H_
