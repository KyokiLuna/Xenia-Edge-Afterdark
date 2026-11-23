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
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include "xenia/cpu/ppc/ppc_context.h"
#include "xenia/kernel/native_content_storage_source.h"
#include "xenia/xbox.h"

namespace xe::kernel {
class KernelState;
class NativeSystemCallbackDispatcher;
class NativeMediaService;
class NativeContentResolve;
class NativeSystemUiService;
namespace test { class NativeContentStorageDriverTestAccess; }

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

  uint64_t generation() const { return generation_; }
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
  // Namespace translation metadata issued only by an original successful
  // processing return. A native ID is not the registry's stable device number.
  // This receipt owns no host directory, MVA, guest Task or Kernel resource.
  class ContentProcessingReceipt final {
   public:
    uint32_t native_id() const { return native_id_; }
    const std::shared_ptr<const filesystem::ConsoleStorageVolume>& volume() const {
      return device_.source_volume;
    }
    const std::string& guest_volume_root() const { return device_.guest_volume_root; }
    const std::shared_ptr<const NativeOpticalStorageProof>& optical_source() const {
      return device_.optical_source_proof;
    }
   private:
    friend class NativeContentStorageDriver;
    ContentProcessingReceipt() = default;
    std::shared_ptr<const uint8_t> issuer_;
    uint64_t driver_generation_ = 0, cookie_ = 0;
    uint32_t native_id_ = 0;
    NativeContentStorageDevice device_;
  };
  // Cached metadata only; callers must separately admit the EXACT returned
  // source through ContentSourceRegistry, and recheck this binding before use.
  // These methods never reinterpret a caller-supplied path or stable number.
  std::optional<ContentProcessingReceipt> CaptureContentProcessing(
      uint32_t native_id) const noexcept;
  std::optional<ContentProcessingReceipt> CaptureContentProcessing(
      const std::shared_ptr<const filesystem::ConsoleStorageVolume>&) const noexcept;
  // Snapshot of actual successfully processed rows; not native table order or
  // a native default-device result. The original cookie remains in each row.
  std::vector<ContentProcessingReceipt> CaptureContentProcessing() const;
  bool ContentProcessingCurrent(const ContentProcessingReceipt&) const noexcept;
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
  friend class NativeMediaService;
  friend class NativeContentResolve;
  // Narrow nonwaiting metadata gate. The only consumer already owns GCRC and
  // a retained driver, and publishes a source-bound VFS alias or an exact
  // guest device selection before releasing it. No IO, provider, callback or
  // guest execution may occur while it is held; bounded guest memory stores
  // under the same existing namespace/allocator guard are permitted.
  class ContentProcessingAdmission final {
   public:
    ~ContentProcessingAdmission() = default;
    ContentProcessingAdmission(ContentProcessingAdmission&&) noexcept = default;
   private:
    friend class NativeContentStorageDriver;
    explicit ContentProcessingAdmission(std::mutex& mutex)
        : lock_(mutex, std::try_to_lock) {}
    std::unique_lock<std::mutex> lock_;
  };
  std::optional<ContentProcessingAdmission> TryAdmitContentProcessing(
      const ContentProcessingReceipt&, std::error_code&) const noexcept;
  friend class NativeSystemUiService;
  friend class test::NativeContentStorageDriverTestAccess;
  // Service-only, after strict producer/callback drain and its factual whole
  // CPU/IO/GPU stop. No native Close, completion or device action is inferred.
  bool RetireAfterActualKernelStop(KernelState* expected_kernel) noexcept;
  // Actual processing output, never a native-number/path relabeling. This
  // value owns only common denial metadata, not a device/backing/Kernel lease.
  struct SelectedMediaProcessingReceipt {
    const NativeContentStorageDriver* issuer=nullptr;
    uint64_t driver_generation=0,cookie=0;
    uint32_t native_id=0;
    std::shared_ptr<const xam::SelectedMediaSourceProof> source;
  };
  std::optional<SelectedMediaProcessingReceipt> CaptureSelectedMediaProcessing(
      std::string_view actual_native_path) const noexcept;
  bool SelectedMediaProcessingCurrent(const SelectedMediaProcessingReceipt&) const noexcept;
  struct State;
  struct Call;
  explicit NativeContentStorageDriver(Config);
  std::shared_ptr<Call> AcquireCall();
  std::shared_ptr<Call> AcquireConfigUse(bool stopping_allowed = false) const noexcept;
  bool IsCurrent() const;
  // Same host-owned path producer, under the caller's memory/State guards.
  uint32_t GetOrCreateGuestPathLocked(const std::string& path);
  void Publish(NativeContentStorageSnapshot) noexcept;
  bool QueueWake() noexcept;
  void ProtocolFailed() noexcept;
  void OnProcessingEntry(const cpu::ppc::PPCContext&);
  void OnProcessingReturn(const cpu::ppc::PPCContext&);
  void StopFormatBindings() noexcept;
  std::shared_ptr<const NativeOpticalStorageProof> CaptureOpticalSource(
      std::string_view actual_native_root) const;
  bool OpticalSourceCurrent(const NativeOpticalStorageProof&) const;
  // Existing namespace protection is already held at the final bounded HOST
  // output cut. No Config-use owner, backing lock or provider may be acquired
  // or released by this metadata-only currentness check.
  bool OpticalSourceCurrentLocked(const NativeOpticalStorageProof&) const;
  // Historical comparison metadata only; never dereferenced after retirement.
  KernelState* const kernel_identity_;
  const uint64_t generation_;
  Config config_;
  std::unique_ptr<State> state_;
};
}  // namespace xe::kernel
#endif  // XENIA_KERNEL_NATIVE_CONTENT_STORAGE_DRIVER_H_
