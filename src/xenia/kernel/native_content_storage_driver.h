/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */
#ifndef XENIA_KERNEL_NATIVE_CONTENT_STORAGE_DRIVER_H_
#define XENIA_KERNEL_NATIVE_CONTENT_STORAGE_DRIVER_H_

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <system_error>
#include <utility>

#include "xenia/cpu/ppc/ppc_context.h"
#include "xenia/kernel/native_content_storage_source.h"
#include "xenia/xbox.h"

namespace xe::kernel {
class KernelState;
class NativeSystemCallbackDispatcher;

// Physical snapshot/change transport beneath native XAM's storage policy.
// The native initializer owns its table, worker, mount state, IDs, source
// attachment and acknowledgement. This driver never writes their cells/events.
class NativeContentStorageDriver
    : public std::enable_shared_from_this<NativeContentStorageDriver> {
 public:
  static constexpr uint32_t kNativeCallback = 0x8167B700;
  // The supplied native consumer proves signed-negative empty termination,
  // not a unique console error value. This virtual provider uses NTSTATUS.
  static constexpr X_STATUS kNoMoreNotifications = 0x8000001A;
  struct Config {
    KernelState* kernel = nullptr;
    uint64_t generation = 0;
    std::shared_ptr<NativeSystemCallbackDispatcher> dispatcher;
    std::shared_ptr<NativeContentStorageSource> source;
    std::function<bool()> authorize_current_caller;
    std::function<bool(uint32_t)> validate_callback;
    std::function<bool()> validate_context;
    std::function<void()> protocol_failed;
  };
  class Installation {
   public:
    ~Installation();
    Installation(const Installation&) = delete;
    Installation& operator=(const Installation&) = delete;

   private:
    friend class NativeContentStorageDriver;
    explicit Installation(std::shared_ptr<NativeContentStorageDriver> owner)
        : owner_(std::move(owner)) {}
    std::shared_ptr<NativeContentStorageDriver> owner_;
    bool armed_ = false;
  };
  static std::shared_ptr<NativeContentStorageDriver> Create(Config);
  static std::unique_ptr<Installation> Install(
      const std::shared_ptr<NativeContentStorageDriver>&);
  static std::shared_ptr<NativeContentStorageDriver> Lookup(KernelState*);
  ~NativeContentStorageDriver();

  uint64_t generation() const { return config_.generation; }
  bool AuthorizeCurrentCaller() const;
  bool SetCallback(uint32_t address);
  // Transactionally publishes the supported24-byte output prefix before
  // consuming a change. Its ANSI path remains valid through Kernel teardown.
  X_STATUS GetNotification(uint32_t guest_output);
  // Only a genuinely processed normal HDD/USB receipt can bind a native ID.
  // Returned proof must still pass the common exact-generation reservation.
  // Host-operation context only: no GCRC, native CS, or guest critical region.
  // The known storage CS/current critical-region ownership is checked before IO;
  // future callers must release any other native locks before calling this API.
  // FE2000B routing/completion/cancellation is deliberately not implemented.
  std::shared_ptr<const xam::ManagedFormatSourceProof> CaptureFormatSource(
      uint32_t native_id, std::error_code&);
  static constexpr uint32_t kProcessingFunction = 0x8167E918;
  static void ObserveProcessingEntry(void*, uint64_t) noexcept;
  static void ObserveProcessingReturn(void*, uint64_t) noexcept;
  void BeginStop() noexcept;
  // Stop source producers and callback ingress before joining this owner.
  // False is an unproved rundown, never permission to free retained strings.
  bool Drain() noexcept;
  struct Snapshot {
    bool accepting = false, failed = false, callback_registered = false;
    uint64_t source_revision = 0, delivered_notifications = 0, empty_drains = 0;
    size_t pending_changes = 0, pending_callbacks = 0, active_calls = 0;
    // Recorded receipt candidates only; not format/readiness acceptance.
    size_t processed_binding_candidates = 0;
    bool format_binding_failed = false;
  };
  Snapshot snapshot() const;

 private:
  struct State;
  struct Call;
  explicit NativeContentStorageDriver(Config);
  std::shared_ptr<Call> AcquireCall();
  bool IsCurrent() const;
  void Publish(NativeContentStorageSnapshot) noexcept;
  bool QueueWake() noexcept;
  void ProtocolFailed() noexcept;
  void OnProcessingEntry(const cpu::ppc::PPCContext&);
  void OnProcessingReturn(const cpu::ppc::PPCContext&);
  void StopFormatBindings() noexcept;
  Config config_;
  std::unique_ptr<State> state_;
};
}  // namespace xe::kernel
#endif  // XENIA_KERNEL_NATIVE_CONTENT_STORAGE_DRIVER_H_
