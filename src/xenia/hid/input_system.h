/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_HID_INPUT_SYSTEM_H_
#define XENIA_HID_INPUT_SYSTEM_H_

#include <array>
#include <atomic>
#include <bitset>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <vector>
#include "xenia/base/mutex.h"
#include "xenia/hid/input.h"
#include "xenia/hid/input_driver.h"
#include "xenia/hid/native_guide_button_edge.h"
#include "xenia/hid/portal/portal.h"
#include "xenia/xbox.h"

namespace xe {
namespace ui {
class Window;
}  // namespace ui
}  // namespace xe

namespace xe {
namespace hid {

// Kernel-free host callback rundown. A token is prepared before subscription
// publication. Stop seals it without waiting under the input lock; the owner
// drains entered callbacks later, outside all InputSystem/Driver locks.
class NativeDeviceNotificationRundown final {
 public:
  // Requires a stopped subscription. Declines from this token's callback or
  // any nested callback invocation still inside it, rather than self-waiting.
  bool Drain() noexcept;

 private:
  friend class InputSystem;
  struct State;
  explicit NativeDeviceNotificationRundown(std::shared_ptr<State> state);
  std::shared_ptr<State> state_;
};

// Pure CPU reservation for the gap before an already-selected host dialog's
// UI-thread constructor installs its normal blocker. No provider/Kernel raw
// pointer or callback survives in the token. Old-generation releases are inert.
class UiInputReservation final {
 public:
  ~UiInputReservation();
  void Release() noexcept;

 private:
  friend class InputSystem;
  struct State {
    explicit State(uint64_t value) : generation(value) {}
    const uint64_t generation;
    std::atomic<uint32_t> count{0};
    std::atomic<uint32_t> edges{0};
  };
  explicit UiInputReservation(std::shared_ptr<State>);
  std::shared_ptr<State> state_;
  std::atomic<bool> released_{false};
};

class InputSystem {
 public:
  explicit InputSystem(xe::ui::Window* window);
  ~InputSystem();

  xe::ui::Window* window() const { return window_; }

  X_STATUS Setup();

  void AddDriver(std::unique_ptr<InputDriver> driver);
  void ClearDrivers() { drivers_.clear(); }
  size_t driver_count() const { return drivers_.size(); }

  X_RESULT GetCapabilities(uint32_t user_index, uint32_t flags,
                           X_INPUT_CAPABILITIES* out_caps);
  // ui_active here and in GetKeystroke holds the input for UI outside the input
  // blockers, such as xam's.
  X_RESULT GetState(uint32_t user_index, uint32_t flags,
                    X_INPUT_STATE* out_state, bool ui_active = false);
  // GetState variant for UI that bypasses the input blocker
  X_RESULT GetStateForUI(uint32_t user_index, uint32_t flags,
                         X_INPUT_STATE* out_state);
  X_RESULT SetState(uint32_t user_index, X_INPUT_VIBRATION* vibration);
  X_RESULT GetKeystroke(uint32_t user_index, uint32_t flags,
                        X_INPUT_KEYSTROKE* out_keystroke,
                        bool ui_active = false);

  // The service coordinator holds lock() for these operations. Transport
  // lifetime and actual native UI ownership are separate: native XAM keeps
  // its own input counters and selects its TITLE/SYSTEM audience internally.
  using NativeGuideCapture =
      std::function<std::shared_ptr<const NativeGuideButtonEdge>(uint32_t)>;
  using NativeDevicesChangedCallback = std::function<void()>;
  bool BeginNativeInputTransport(
      uint64_t generation, NativeGuideCapture guide_capture = {},
      NativeDevicesChangedCallback devices_changed = {});
  // Caller holds lock(). Closes only this transport generation, without
  // allocating or waiting. Queued callbacks become inert; the returned token
  // retains and drains every already-entered callback, including its local
  // ownership before Driver Poll entry and after Poll exit. The owner must
  // keep it and call Drain outside input/Driver locks before Kernel teardown.
  // A stopped subscription cannot be reopened by an older/same generation.
  // Repeat closes return the same token until a later transport replaces it.
  std::shared_ptr<NativeDeviceNotificationRundown>
  StopNativeDeviceNotifications(uint64_t generation);
  uint64_t native_input_generation() const {
    return native_input_generation_.load();
  }
  uint64_t native_guide_generation() const {
    return native_guide_generation_.load();
  }
  bool EnableNativeGuideTransport(uint64_t generation);
  // These two methods acquire/release lock internally. No callback or native
  // enqueue crosses the InputSystem lock. Capture is metadata-only, not PPC.
  std::shared_ptr<const NativeGuideButtonEdge> CaptureNativeGuideButtonEdge(
      uint32_t slot, uint64_t expected_generation);
  NativeGuideDispatch DispatchNativeGuideButtonEdge(
      const std::shared_ptr<const NativeGuideButtonEdge>& edge,
      uint64_t expected_generation);
  // Caller holds lock(). Host-modal priority and its held-release masks apply
  // below native191, whose actual counters still select TITLE/SYSTEM readers.
  X_RESULT GetStateForNativeDriver(uint32_t slot, uint32_t flags,
                                   X_INPUT_STATE* state);
  // Same transformation for an already sampled controller. Caller holds
  // lock(); raw host modal/menu input stays separate from native Guide intent.
  void ApplyNativeHostModalSuppression(uint32_t slot, X_INPUT_STATE* state);
  void EndNativeInputTransport(uint64_t generation);
  bool SetNativeInputOwner(uint64_t generation, bool owns_input);
  X_RESULT GetNativeTextKeystroke(uint64_t generation, uint32_t user_index,
                                  X_INPUT_KEYSTROKE* output);
  bool HasNativeTextInput(uint64_t generation) const;
  bool SetNativeTextOwner(uint64_t generation, uint64_t owner_revision,
                          uint32_t user, bool enabled);
  NativeTextState GetNativeTextState(uint64_t generation) const;
  void ResetNativeText(uint64_t generation);

  // Block/unblock input to the game (for UI dialogs)
  void AddUIInputBlocker();
  void RemoveUIInputBlocker();
  bool IsUIInputBlocked() const {
    const auto state = host_ui_input_reservations_.load();
    return ui_input_blockers_.load() > 0 ||
           (state && state->generation == native_input_generation_.load() &&
            state->count.load() > 0);
  }
  // Caller holds lock(). Only the current real native transport generation.
  std::shared_ptr<UiInputReservation> ReserveHostUiInput(uint64_t generation);

  // Called when a title launches.
  void ResetKeystrokes() {
    for (auto& driver : drivers_) {
      driver->ResetKeystrokes();
    }
  }

  bool GetVibrationCvar();
  void ToggleVibration();

  const std::bitset<XUserMaxUserCount> GetConnectedSlots() const {
    return connected_slots;
  }

  uint32_t GetLastUsedSlot() const { return last_used_slot; }

  Portal* GetPortal() { return portal_.get(); }

  std::unique_lock<xe_unlikely_mutex> lock();

  // A device feeding a guest controller slot, alone or with others.
  struct SlotBinding {
    InputDriver* driver = nullptr;  // null = empty or detached
    uint8_t driver_slot = 0;
    // Retained while detached so the same device reattaches to this slot.
    std::string stable_id;
    // Cached at bind time; UI uses it for the bound state only.
    std::string display_name;
    // 0 = report whatever the driver detected (e.g. SDL's XInput subtype
    // translation); non-zero = force this XINPUT_DEVSUBTYPE_* value when the
    // guest reads capabilities.
    uint8_t subtype_override = 0;
  };

  // One enumerated device + its current binding state.
  struct EnumeratedDevice {
    InputDriver* driver;
    InputDeviceInfo info;
    int bound_slot;  // -1 if not bound
  };

  std::vector<EnumeratedDevice> EnumerateDevices();
  // Adds the device to the slot alongside any already bound there.
  void BindSlot(uint32_t guest_slot, InputDriver* driver, uint8_t driver_slot,
                std::string stable_id, std::string display_name);
  void UnbindDevice(InputDriver* driver, uint8_t driver_slot);
  // Pass subtype = 0 to clear the override and use the device's detected type.
  void SetSubtypeOverride(InputDriver* driver, uint8_t driver_slot,
                          uint8_t subtype);
  const std::vector<SlotBinding>& GetSlotBindings(uint32_t guest_slot) const {
    return slot_bindings_[guest_slot];
  }

  // Serializes slot bindings for --slot_bindings_passthrough=; empty when
  // nothing to forward. Format documented at the definition.
  std::string SerializeSlotBindingsForPassthrough() const;

  // Invoked whenever the binding table changes — explicit bind/unbind, or
  // ReconcileBindings demoting/auto-binding due to hotplug. Fires on the
  // calling thread with lock_ held, so subscribers must defer any work that
  // re-enters InputSystem (CallInUIThreadDeferred — never sync, since lock_
  // is a non-recursive spinlock).
  using BindingsChangedCallback = std::function<void()>;
  void SetBindingsChangedCallback(BindingsChangedCallback cb) {
    bindings_changed_cb_ = std::move(cb);
  }

  // Drivers call this after a hotplug add/remove; schedules ReconcileBindings
  // to run on the UI thread (which takes lock_) so the SDL event thread can
  // post the notification without holding any of our locks.
  void NotifyDevicesChanged();

 private:
  typedef std::pair<uint16_t, uint16_t> joystick_value;

  const std::string controller_slot_state_change_message[2] = {
      "Controller disconnected from slot {}.",
      "New controller connected to slot {}."};

  uint32_t UiInputBlockingEdges() const {
    const auto state = host_ui_input_reservations_.load();
    return ui_input_blocking_edges_.load() +
           (state && state->generation == native_input_generation_.load()
                ? state->edges.load()
                : 0);
  }
  void UpdateUsedSlot(InputDriver* driver, uint8_t slot, bool connected);
  static void MergeState(X_INPUT_STATE* state, const X_INPUT_STATE& other);
  static void MergeCapabilities(X_INPUT_CAPABILITIES* caps,
                                const X_INPUT_CAPABILITIES& other);
  void AdjustDeadzoneLevels(const uint8_t slot, X_INPUT_GAMEPAD* gamepad);
  X_INPUT_VIBRATION ModifyVibrationLevel(X_INPUT_VIBRATION* vibration);

  // Detach gone devices, reattach by stable_id, auto-bind new devices to the
  // first free guest slot. Cheap; safe to call from the polling path.
  void ReconcileBindings();
  // A connected device's binding, and the slot it's in when guest_slot isn't
  // null. Null if the device isn't bound.
  SlotBinding* FindBinding(InputDriver* driver, uint8_t driver_slot,
                           uint32_t* guest_slot = nullptr);
  void NotifyBindingsChanged();
  // Always defers the independent native subscriber through the UI queue.
  // The deferred closure contains only a weak generation token, never this.
  void QueueNativeDeviceNotification();

  // Seeds slot_bindings_ as detached entries from the passthrough cvar;
  // ReconcileBindings reattaches them by stable_id as devices appear.
  void LoadSlotBindingsFromPassthrough();

  xe::ui::Window* window_ = nullptr;

  std::vector<std::unique_ptr<InputDriver>> drivers_;

  std::unique_ptr<Portal> portal_;

  std::bitset<XUserMaxUserCount> connected_slots = {};
  std::array<std::vector<SlotBinding>, XUserMaxUserCount> slot_bindings_{};
  // True while a NotifyDevicesChanged() reconcile is pending on the UI
  // thread; coalesces bursts of hotplug events into a single reconcile.
  std::atomic<bool> reconcile_pending_{false};
  // Devices the user has explicitly unbound this session; suppresses
  // re-auto-binding by ReconcileBindings until BindSlot or app restart.
  std::set<std::string> dismissed_ids_;

  BindingsChangedCallback bindings_changed_cb_;
  std::array<std::pair<joystick_value, joystick_value>, XUserMaxUserCount>
      controllers_max_joystick_value = {};
  uint32_t last_used_slot = 0;

  xe_unlikely_mutex lock_;

  // Reference count for UI elements blocking game input
  std::atomic<int> ui_input_blockers_{0};
  std::atomic<std::shared_ptr<UiInputReservation::State>>
      host_ui_input_reservations_;
  // Times blocking started or ended, added to packet numbers so a game sees a
  // new state at both edges. Bumped after the count changes so the last read
  // of each phase has a new number.
  std::atomic<uint32_t> ui_input_blocking_edges_{0};
  std::atomic<uint64_t> native_input_generation_{0};
  std::atomic<bool> native_input_owner_{false};
  NativeGuideCapture native_guide_capture_;
  // Accessed atomically because driver hotplug may arrive off the UI thread.
  std::atomic<std::shared_ptr<NativeDeviceNotificationRundown>>
      native_device_notifications_;
  // Only the optional notification subscription requires increasing tokens;
  // legacy transport users without a subscriber retain their existing ABI.
  uint64_t native_device_notifications_closed_through_ = 0;
  std::atomic<uint64_t> native_guide_generation_{0};

  // Buttons that should be masked from game input until released (per slot).
  // This prevents button presses used to close UI dialogs from being
  // seen by the game immediately after the dialog closes.
  std::array<uint16_t, XUserMaxUserCount> consumed_buttons_{};
};

}  // namespace hid
}  // namespace xe

#endif  // XENIA_HID_INPUT_SYSTEM_H_
