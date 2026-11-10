/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia contributors. All rights reserved.                    *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_GPU_SYSTEM_COMMAND_BUFFER_H_
#define XENIA_GPU_SYSTEM_COMMAND_BUFFER_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

#include "xenia/gpu/gpu_completion.h"

namespace xe::gpu {

// The observed guest record is copied byte-for-byte. Optional scaler blocks
// remain opaque; driver GPU address forms are not silently translated/masked.
struct SystemCommandDescriptor {
  static constexpr size_t kSize = 0x94;
  std::array<uint8_t, kSize> bytes{};
  static std::optional<SystemCommandDescriptor> Copy(
      std::span<const uint8_t> source);
  bool empty() const;
  uint32_t command_address() const;
  uint32_t command_dwords() const;
  uint32_t texture_address() const;
  uint32_t texture_format() const;
  uint32_t width() const;
  uint32_t height() const;
  bool has_texture() const;
  bool operator==(const SystemCommandDescriptor&) const = default;
};

// Host policy, not a recovered retail identifier encoding: publication
// generations and per-Get checkouts are distinct. Nonzero 32-bit checkout IDs
// are never reused in this object's lifetime; exhaustion fails closed.
class SystemCommandBuffer {
  struct Session;
  struct Publication;
  struct SwapUse;
  struct CheckoutEntry;

 public:
  struct IdentifierBinding {
    uint32_t address = 0;
    uint64_t generation = 0;
    bool operator==(const IdentifierBinding&) const = default;
  };
  struct OwnedSession {
    std::shared_ptr<void> device_lifetime;
    // Actual owned guest recording allocations, not module/code pins. The
    // caller validates raw command ranges against these before execution.
    GpuCompletionResources recording_memory;
    // The actual CP completion queue's epoch. A fence from another/replaced
    // backend cannot retire this session's use, even at the same numeric value.
    uint64_t backend_epoch = 0;
  };
  struct Checkout {
    SystemCommandDescriptor descriptor;
    uint32_t identifier = 0;
    uint64_t publication_generation = 0;
    IdentifierBinding binding;
  };
  class SwapLease {
   public:
    SwapLease() = default;
    explicit operator bool() const { return bool(use_); }
    const SystemCommandDescriptor& descriptor() const;
    uint32_t identifier() const;
    uint64_t owner_epoch() const;
    uint64_t use_id() const;
    uint64_t publication_generation() const;
    GpuCompletionResources resources() const;

   private:
    friend class SystemCommandBuffer;
    explicit SwapLease(std::shared_ptr<SwapUse> use) : use_(std::move(use)) {}
    std::shared_ptr<SwapUse> use_;
  };
  enum class BindStatus {
    kBound,
    kEmpty,
    kUnknownIdentifier,
    kDescriptorMismatch,
    kStaleBinding,
    kFailed,
    kNoOwnedSession,
  };
  struct BindResult {
    BindStatus status;
    SwapLease lease;
  };
  // Checked host-memory operations only: called under this state's mutex to
  // serialize ACK observation/stores with address reset. No guest execution,
  // allocation release, blocking GPU work or state reentry from these
  // callbacks.
  using AckReader = std::function<std::optional<uint32_t>(uint32_t address)>;
  // Compare-exchange the sampled logical guest DWORD: a blind store could
  // overwrite a concurrent CPU skip ACK before it was observed. The bridge
  // supplies aligned, endian-correct memory access; failure keeps Query busy.
  using AckWriter = std::function<bool(uint32_t address, uint32_t observed,
                                       uint32_t identifier)>;

  SystemCommandBuffer();
  bool BeginOwnedSession(OwnedSession session);
  void CloseOwnedSession();
  bool Publish(SystemCommandDescriptor descriptor,
               GpuCompletionResources texture_memory = {});
  IdentifierBinding SetIdentifierAddress(uint32_t address);
  IdentifierBinding identifier_binding() const;
  // Sample CPU skip ACKs before another Get can overwrite their writeback ID.
  // An empty publication returns ID0 without acquiring a GPU-resource checkout.
  std::optional<Checkout> Get(const AckReader& reader = {},
                              const AckWriter& writer = {});
  BindResult BindSwap(uint32_t identifier,
                      const SystemCommandDescriptor& supplied,
                      uint32_t acknowledgement_address);
  // One strict marker for each bound use. Repeated Swap uses cannot replace
  // another use's completion. Unsupported/Failed markers never become ACKs.
  bool AttachCompletion(const SwapLease& lease, GpuCompletion completion);
  void Poll(const AckReader& reader = {}, const AckWriter& writer = {});
  // Only mode0 was observed. Unknown modes fail closed, not always-ready.
  bool QueryReady(uint32_t mode = 0, const AckReader& reader = {},
                  const AckWriter& writer = {});
  size_t outstanding_checkouts() const;
  // Actual GPU stop/device destruction proof is required. No ACK is emitted.
  // Normal shutdown must stop queued CP requests before using this boundary
  // or destroying the state. Existing SwapLease copies remain independently
  // owned; the CP must give their resources to CaptureGpuCompletion.
  void ReleaseAfterGpuStopped();

 private:
  using Retired = std::vector<std::shared_ptr<CheckoutEntry>>;
  void PollLocked(const AckReader& reader, const AckWriter& writer,
                  Retired& retired);
  void CancelUnsubmittedLocked(Retired& retired);
  const uint64_t epoch_;
  mutable std::mutex mutex_;
  std::shared_ptr<Session> session_;
  std::shared_ptr<const Publication> publication_;
  IdentifierBinding binding_;
  uint64_t next_publication_ = 1;
  uint64_t next_identifier_ = 1;
  uint64_t next_use_ = 1;
  bool exhausted_ = false;
  std::unordered_map<uint32_t, std::shared_ptr<CheckoutEntry>> checkouts_;
};

}  // namespace xe::gpu

#endif  // XENIA_GPU_SYSTEM_COMMAND_BUFFER_H_
