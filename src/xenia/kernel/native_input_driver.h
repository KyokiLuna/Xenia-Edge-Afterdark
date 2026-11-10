/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia contributors. All rights reserved.                    *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */
#ifndef XENIA_KERNEL_NATIVE_INPUT_DRIVER_H_
#define XENIA_KERNEL_NATIVE_INPUT_DRIVER_H_

#include <array>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <utility>

#include "xenia/cpu/ppc/ppc_context.h"
#include "xenia/hid/input.h"
#include "xenia/hid/native_guide_button_edge.h"
#include "xenia/xbox.h"

namespace xe::hid {
class NativeDeviceNotificationRundown;
}
namespace xe::kernel {
class KernelState;
class NativeSystemCallbackDispatcher;
class NativeSystemUiVisibilityEvent;

// Physical-driver transport below native XAM's user binding, input counters,
// keystroke generation and UI ownership policy. Installation alone grants no
// native UI readiness. Unsupported raw/write packets remain explicit failures.
class NativeInputDriver
    : public std::enable_shared_from_this<NativeInputDriver> {
 public:
  enum class Callback : uint32_t {
    kUserBinding,
    kDeviceConfig,
    kSystemRequest,
    kFailedConnectionOrBind
  };
  struct Config {
    KernelState* kernel = nullptr;
    uint64_t generation = 0;
    std::shared_ptr<NativeSystemCallbackDispatcher> dispatcher;
    std::function<bool()> authorize_current_caller;
    std::function<bool(uint32_t)> validate_callback;
    std::function<bool()> allow_physical_input;
    // Exact native class0 code/data/import profile; no pristine-event check
    // after callbacks may have materialized it. No lock crosses guest entry.
    std::function<bool()> validate_device_context;
    std::function<void()> protocol_failed;
  };
  class Installation {
   public:
    ~Installation();
    Installation(const Installation&) = delete;
    Installation& operator=(const Installation&) = delete;

   private:
    friend class NativeInputDriver;
    explicit Installation(std::shared_ptr<NativeInputDriver> driver)
        : driver_(std::move(driver)) {}
    std::shared_ptr<NativeInputDriver> driver_;
    bool armed_ = false;
  };

  static std::shared_ptr<NativeInputDriver> Create(Config config);
  static std::unique_ptr<Installation> Install(
      const std::shared_ptr<NativeInputDriver>& driver);
  static std::shared_ptr<NativeInputDriver> Lookup(KernelState* kernel);
  static void BeginStopForKernel(KernelState* kernel);
  static bool DrainForKernel(KernelState* kernel);
  static bool ReleaseInputTransportAfterProducersStoppedForKernel(
      KernelState* kernel);
  ~NativeInputDriver();

  uint64_t generation() const { return config_.generation; }
  // Diagnostic correlation only, never native user/Kernel/Source authority.
  uint64_t trace_input_generation() const { return transport_generation_; }
  // Diagnostic POD only, observed through the existing successful native
  // controller read. It grants no input, Source, GPU or user authority.
  struct BDownCaptureObservation {
    uint64_t input_generation = 0, domain_id = 0, root_load_generation = 0;
    uint64_t foreground_generation = 0, visibility_sequence = 0;
    uint64_t press_sequence = 0, binding_epoch = 0;
    uint32_t root_hmodule = 0, slot = 0, native_user = UINT32_MAX;
    uint32_t device_handle = 0, binding_identity = 0;
    uint32_t packet = 0, buttons = 0;
    bool operator==(const BDownCaptureObservation&) const = default;
  };
  std::optional<BDownCaptureObservation> GetBDownCaptureObservation() noexcept;
  bool AuthorizeCurrentCaller() const;
  bool SetCallback(Callback kind, uint32_t guest_address);
  // Trusted bootstrap only; validates the exact installed native506 closure.
  // Visibility and per-user196 counts are not host admission predicates.
  bool EnableGuideIntent(
      std::function<bool()> guide_requests_allowed,
      std::function<NativeUiBackendRoute::Admission()> admit_ui);
  // Call after registration and on physical binding/hotplug changes. Callback
  // work is queued on the checked SYSTEM dispatcher, never executed here.
  bool RefreshDevices();
  // Coordinator-only after actual native NotifyStartup postconditions. Real
  // capability changes observed earlier stay pending; no synthetic event.
  bool ObserveNativeNotificationsInitialized();
  struct InitialBindingSnapshot {
    bool accepting = false, callbacks_registered = false;
    bool enumeration_observed = false, failed = false;
    uint32_t connected_mask = 0, bound_mask = 0;
    size_t pending_callbacks = 0;
    bool ready() const {
      return accepting && callbacks_registered && enumeration_observed &&
             !failed && !pending_callbacks && connected_mask == bound_mask;
    }
  };
  // Actual host enumeration + native output-byte adoption, never fabricated
  // user0/Task completion/Source or graphics readiness. No controller is valid.
  InitialBindingSnapshot initial_binding_snapshot() const;
  bool SetNativeUiOwner(bool owns_input);
  void BeginStop();
  bool Drain();
  // Caller must already have stopped the complete native guest producer set.
  // Callback drain alone does not authorize releasing TITLE input capture.
  bool ReleaseInputTransportAfterProducersStopped();

  static constexpr size_t kVirtualDeviceStatsBytes = 0x1C;
  X_STATUS GetDeviceStats(uint32_t handle, uint32_t selector,
                          std::array<uint8_t, kVirtualDeviceStatsBytes>* stats,
                          uint32_t* opaque);
  X_STATUS TakeFailedConnectionOrBind(uint32_t* actual_handle);
  X_STATUS ReadState(uint32_t handle, hid::X_INPUT_STATE* state);
  X_STATUS GetCapabilities(uint32_t handle, hid::X_INPUT_CAPABILITIES* caps);
  // Actual native selector context is metadata, not another guest argument.
  X_RESULT ReadText(uint32_t user, hid::X_INPUT_KEYSTROKE* stroke,
                    const cpu::ppc::PPCContext&);
  bool HasText(uint32_t user);
  void ObserveNativeUiVisibility(const NativeSystemUiVisibilityEvent&) noexcept;
  void RevokeNativeTextVisibility() noexcept;
  bool RefreshNativeTextOwner() noexcept;
  bool GetTextKeyLocks(uint32_t user, uint16_t* word);
  X_STATUS SetTextKeyLocks(uint32_t user, uint16_t value, uint16_t mask);
  X_STATUS GetLastTextInputTime(uint32_t user, uint64_t* time);

 private:
  struct Device {
    uint32_t handle = 0;
    uint32_t binding_identity = 0;
    uint32_t slot = 0;
    bool capture_b_sample_known = false, capture_b_down = false;
    bool connected = true;
    uint64_t announced_registration = 0;
    uint64_t native_user_registration = 0;
    uint32_t native_user = XUserIndexAny;
    bool binding_rejected = false;
    hid::X_INPUT_CAPABILITIES capabilities{};
    uint64_t capability_revision = 1, config_delivered_revision = 1;
    uint64_t config_announced_revision = 1, config_announced_epoch = 0;
  };
  struct Registration {
    uint32_t address = 0;
    uint64_t epoch = 1;
  };
  struct TextOwner {
    uint32_t user = UINT32_MAX, group = 0;
    uint64_t foreground = 0, binding_epoch = 0, revision = 0;
    bool operator==(const TextOwner&) const = default;
  };
  struct TextEndpoint {
    uint32_t handle = 0;
    bool present = false, pending = false, failed = false;
    uint32_t assigned_user = UINT32_MAX;
    TextOwner owner;
    uint64_t rejected_revision = 0;
  };
  bool ReadNativeTextUser(const cpu::ppc::PPCContext&, uint32_t* user,
                          uint64_t* foreground, bool* ui_task) const;
  void PumpTextBinding() noexcept;
  void ApplyHostTextOwner() noexcept;
  bool QueueTextBinding(const std::shared_ptr<TextEndpoint>&, TextOwner,
                        bool removed);
  class Poll {
   public:
    explicit Poll(NativeInputDriver* owner = nullptr) : owner_(owner) {}
    Poll(Poll&& other) noexcept
        : owner_(std::exchange(other.owner_, nullptr)) {}
    ~Poll();
    Poll(const Poll&) = delete;
    Poll& operator=(const Poll&) = delete;
    explicit operator bool() const { return owner_ != nullptr; }

   private:
    NativeInputDriver* owner_;
  };
  explicit NativeInputDriver(Config config, uint64_t transport_generation)
      : config_(std::move(config)),
        transport_generation_(transport_generation) {}
  Poll AcquirePoll();
  bool IsCurrent() const;
  bool QueueCallback(Callback kind, std::span<const uint32_t> arguments,
                     std::function<void(bool, uint32_t)> completed = {},
                     uint32_t address_override = 0,
                     std::function<bool()> eligible = {},
                     bool* accepted_out = nullptr, uint64_t trace_id = 0);
  bool AnnounceDevice(const std::shared_ptr<Device>& device, bool removed);
  bool AnnounceConfiguration(const std::shared_ptr<Device>&, uint64_t revision);
  bool DeviceCallbackEligible(const std::shared_ptr<Device>&,
                              bool removed) const;
  void ProtocolFailed(uint32_t handle, uint32_t reason) noexcept;
  void RecordFailedBinding(const std::shared_ptr<Device>&);
  bool AnnounceFailedBinding();
  std::shared_ptr<const hid::NativeGuideButtonEdge> CaptureGuideButtonEdge(
      uint32_t slot);
  hid::NativeGuideDispatch DispatchGuideButtonEdge(
      const std::shared_ptr<Device>&, uint32_t native_user,
      uint64_t binding_epoch, Registration system_registration,
      uint32_t native_owner, uint32_t native_task, uint64_t trace_id);
  bool GuideOwnerMatches(uint32_t owner, uint32_t task) const;

  const Config config_;
  const uint64_t transport_generation_;
  mutable std::mutex mutex_;
  std::mutex refresh_mutex_;
  std::condition_variable drained_;
  bool stopping_ = false;
  bool transport_released_ = false;
  bool notification_stop_published_ = false, notification_stop_failed_ = false;
  std::shared_ptr<hid::NativeDeviceNotificationRundown> notification_rundown_;
  bool transport_started_ = false;
  bool physical_text_allowed_ = true;
  bool guide_intent_enabled_ = false;
  bool enumeration_observed_ = false, protocol_failed_ = false;
  bool native_notifications_initialized_ = false;
  bool text_visibility_known_ = false, text_visible_ = false;
  bool text_visibility_revoked_ = false;
  uint64_t text_visibility_sequence_ = 0, text_foreground_ = 0;
  uint64_t text_owner_revision_ = 0;
  bool text_owner_readable_ = false, text_pump_active_ = false;
  uint16_t text_key_locks_ = 0;  // Virtual opaque lock word, not hardware LEDs.
  uint64_t output_capture_b_sequence_ = 0;
  std::optional<BDownCaptureObservation> output_capture_b_down_;
  std::optional<TextOwner> text_owner_;
  std::shared_ptr<TextEndpoint> text_endpoint_;
  std::array<std::array<uint64_t, 2>, XUserMaxUserCount> text_epochs_{};
  std::function<bool()> guide_requests_allowed_;
  std::function<NativeUiBackendRoute::Admission()> admit_ui_;
  size_t active_polls_ = 0;
  size_t pending_callbacks_ = 0;
  uint32_t failed_connection_or_bind_ = 0;
  bool failure_notification_pending_ = false, failure_callback_queued_ = false;
  std::array<Registration, 4> registrations_{};
  std::array<std::shared_ptr<Device>, XUserMaxUserCount> slots_{};
  std::map<uint32_t, std::shared_ptr<Device>> handles_;
};
}  // namespace xe::kernel

#endif  // XENIA_KERNEL_NATIVE_INPUT_DRIVER_H_
