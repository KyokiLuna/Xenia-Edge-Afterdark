/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/hid/keyboard/keyboard_input_driver.h"

#include <cstring>

#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/hid/input_system.h"
#include "xenia/ui/virtual_key.h"
#include "xenia/ui/window.h"

#define XE_HID_KEYBOARD_BINDING(button, description, cvar_name, \
                                cvar_default_value)             \
  DEFINE_string(cvar_name, cvar_default_value,                  \
                "List of keys to bind to " description          \
                ", separated by spaces",                        \
                "HID.Key")
#include "keyboard_binding_table.inc"
#undef XE_HID_KEYBOARD_BINDING

DEFINE_bool(keyboard_passthrough, false,
            "When true, the keyboard surfaces raw keystrokes to the running "
            "title alongside any connected controller. When false (default), "
            "the keyboard is bindable to a guest slot via the Controllers "
            "menu and emulates a gamepad.",
            "HID");

namespace xe {
namespace hid {
namespace keyboard {

// Map ui::VirtualKey (Windows VK_ numbering) to USB HID usage page 0x07.
static uint8_t VirtualKeyToHIDUsage(ui::VirtualKey vk_enum) {
  const uint32_t vk = static_cast<uint32_t>(vk_enum);

  // Letters: contiguous in both VK and HID space.
  if (vk >= 'A' && vk <= 'Z') {
    return uint8_t(vk - 'A' + 0x04);
  }

  // Digits 1-9 (0 is irregular: 0x27).
  if (vk >= '1' && vk <= '9') {
    return uint8_t(vk - '1' + 0x1E);
  }

  // F1-F12 (VK_F1 = 0x70).
  if (vk >= 0x70 && vk <= 0x7B) {
    return uint8_t(vk - 0x70 + 0x3A);
  }
  // F13-F24 (VK_F13 = 0x7C).
  if (vk >= 0x7C && vk <= 0x87) {
    return uint8_t(vk - 0x7C + 0x68);
  }

  // Numpad 1-9 (VK_NUMPAD1 = 0x61; 0 is irregular: 0x62).
  if (vk >= 0x61 && vk <= 0x69) {
    return uint8_t(vk - 0x61 + 0x59);
  }

  switch (vk) {
    case 0x10:
    case 0xA0:
      return 0xE1;  // Shift / Left Shift.
    case 0xA1:
      return 0xE5;  // Right Shift.
    case 0x11:
    case 0xA2:
      return 0xE0;  // Control / Left Control.
    case 0xA3:
      return 0xE4;  // Right Control.
    case 0x12:
    case 0xA4:
      return 0xE2;  // Alt / Left Alt.
    case 0xA5:
      return 0xE6;  // Right Alt.
    case 0x5B:
      return 0xE3;  // Left Super.
    case 0x5C:
      return 0xE7;  // Right Super.

    case '0':
      return 0x27;
    case 0x0D:
      return 0x28;  // VK_RETURN
    case 0x1B:
      return 0x29;  // VK_ESCAPE
    case 0x08:
      return 0x2A;  // VK_BACK
    case 0x09:
      return 0x2B;  // VK_TAB
    case 0x20:
      return 0x2C;  // VK_SPACE
    case 0xBD:
      return 0x2D;  // VK_OEM_MINUS
    case 0xBB:
      return 0x2E;  // VK_OEM_PLUS
    case 0xDB:
      return 0x2F;  // VK_OEM_4
    case 0xDD:
      return 0x30;  // VK_OEM_6
    case 0xDC:
      return 0x31;  // VK_OEM_5
    case 0xBA:
      return 0x33;  // VK_OEM_1
    case 0xDE:
      return 0x34;  // VK_OEM_7
    case 0xC0:
      return 0x35;  // VK_OEM_3
    case 0xBC:
      return 0x36;  // VK_OEM_COMMA
    case 0xBE:
      return 0x37;  // VK_OEM_PERIOD
    case 0xBF:
      return 0x38;  // VK_OEM_2
    case 0x14:
      return 0x39;  // VK_CAPITAL
    case 0x2C:
      return 0x46;  // VK_SNAPSHOT
    case 0x91:
      return 0x47;  // VK_SCROLL
    case 0x13:
      return 0x48;  // VK_PAUSE
    case 0x2D:
      return 0x49;  // VK_INSERT
    case 0x24:
      return 0x4A;  // VK_HOME
    case 0x21:
      return 0x4B;  // VK_PRIOR
    case 0x2E:
      return 0x4C;  // VK_DELETE
    case 0x23:
      return 0x4D;  // VK_END
    case 0x22:
      return 0x4E;  // VK_NEXT
    case 0x27:
      return 0x4F;  // VK_RIGHT
    case 0x25:
      return 0x50;  // VK_LEFT
    case 0x28:
      return 0x51;  // VK_DOWN
    case 0x26:
      return 0x52;  // VK_UP
    case 0x90:
      return 0x53;  // VK_NUMLOCK
    case 0x6F:
      return 0x54;  // VK_DIVIDE
    case 0x6A:
      return 0x55;  // VK_MULTIPLY
    case 0x6D:
      return 0x56;  // VK_SUBTRACT
    case 0x6B:
      return 0x57;  // VK_ADD
    case 0x60:
      return 0x62;  // VK_NUMPAD0
    case 0x6E:
      return 0x63;  // VK_DECIMAL
    case 0x5D:
      return 0x65;  // VK_APPS
    default:
      return 0x00;
  }
}

static bool IsPassthroughEnabled() { return cvars::keyboard_passthrough; }

// No prefix (both flags false) matches either caps state.
static bool MatchesCaps(bool lowercase, bool uppercase, bool is_capital) {
  if (lowercase == uppercase) {
    return true;
  }
  return uppercase ? is_capital : !is_capital;
}

void KeyboardInputDriver::ParseKeyBinding(
    ui::VirtualKey output_key, const std::string_view description,
    const std::string_view source_tokens) {
  for (const std::string_view source_token :
       utf8::split(source_tokens, " ", true)) {
    KeyBinding key_binding;
    key_binding.output_key = output_key;

    std::string_view token = source_token;

    if (utf8::starts_with(token, "_")) {
      key_binding.lowercase = true;
      token = token.substr(1);
    } else if (utf8::starts_with(token, "^")) {
      key_binding.uppercase = true;
      token = token.substr(1);
    }

    if (utf8::starts_with(token, "0x")) {
      token = token.substr(2);
      key_binding.input_key = static_cast<ui::VirtualKey>(
          string_util::from_string<uint16_t>(token, true));
    } else if (token.size() == 1 && ((token[0] >= 'A' && token[0] <= 'Z') ||
                                     (token[0] >= '0' && token[0] <= '9'))) {
      key_binding.input_key = static_cast<ui::VirtualKey>(token[0]);
    }

    if (key_binding.input_key == ui::VirtualKey::kNone) {
      XELOGW(
          "keyboard HID: failed to parse binding \"{}\" for controller input "
          "{}.",
          source_token, description);
      continue;
    }

    key_bindings_.push_back(key_binding);
    XELOGI("keyboard HID: \"{}\" binds key 0x{:X} to controller input {}.",
           source_token, static_cast<uint16_t>(key_binding.input_key),
           description);
  }
}

KeyboardInputDriver::KeyboardInputDriver(xe::ui::Window* window,
                                         size_t window_z_order)
    : InputDriver(window, window_z_order), window_input_listener_(*this) {
#define XE_HID_KEYBOARD_BINDING(button, description, cvar_name,        \
                                cvar_default_value)                    \
  ParseKeyBinding(xe::ui::VirtualKey::kXInputPad##button, description, \
                  cvars::cvar_name);
#include "keyboard_binding_table.inc"
#undef XE_HID_KEYBOARD_BINDING

  native_text_available_ = window->SupportsNativeTextCommits();
  window->AddInputListener(&window_input_listener_, window_z_order);
  window->AddListener(&window_input_listener_);
  // AddListener does not replay a focus event. Seed the already-focused case
  // after attaching, on the UI thread, rather than awaiting a future
  // transition.
  native_text_.SetFocused(window->HasFocus());
}

KeyboardInputDriver::~KeyboardInputDriver() {
  window()->RemoveListener(&window_input_listener_);
  window()->RemoveInputListener(&window_input_listener_);
}

X_STATUS KeyboardInputDriver::Setup() { return X_STATUS_SUCCESS; }

X_RESULT KeyboardInputDriver::GetCapabilities(uint32_t user_index,
                                              uint32_t flags,
                                              X_INPUT_CAPABILITIES* out_caps) {
  if (IsPassthroughEnabled()) {
    out_caps->type = X_INPUT_DEVTYPE::XINPUT_DEVTYPE_KEYBOARD;
    out_caps->sub_type = X_INPUT_DEVSUBTYPE::XINPUT_DEVSUBTYPE_USB_KEYBOARD;
    return X_ERROR_SUCCESS;
  }

  out_caps->type = X_INPUT_DEVTYPE::XINPUT_DEVTYPE_GAMEPAD;
  out_caps->sub_type = X_INPUT_DEVSUBTYPE::XINPUT_DEVSUBTYPE_GAMEPAD;
  out_caps->flags = 0;
  out_caps->gamepad.buttons = 0xFFFF;
  out_caps->gamepad.left_trigger = 0xFF;
  out_caps->gamepad.right_trigger = 0xFF;
  out_caps->gamepad.thumb_lx = (int16_t)0xFFFFu;
  out_caps->gamepad.thumb_ly = (int16_t)0xFFFFu;
  out_caps->gamepad.thumb_rx = (int16_t)0xFFFFu;
  out_caps->gamepad.thumb_ry = (int16_t)0xFFFFu;
  out_caps->vibration.left_motor_speed = 0;
  out_caps->vibration.right_motor_speed = 0;
  return X_ERROR_SUCCESS;
}

X_RESULT KeyboardInputDriver::GetState(uint32_t user_index,
                                       X_INPUT_STATE* out_state) {
  if (IsPassthroughEnabled()) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  auto global_lock = global_critical_region_.Acquire();
  const auto native = native_text_.State(native_text_generation_);
  const bool native_claimed =
      native.owner_revision && native.user < XUserMaxUserCount;
  uint16_t buttons = 0;
  uint8_t left_trigger = 0;
  uint8_t right_trigger = 0;
  int16_t thumb_lx = 0;
  int16_t thumb_ly = 0;
  int16_t thumb_rx = 0;
  int16_t thumb_ry = 0;

  if (window()->HasFocus() && !native_claimed) {
    for (const KeyBinding& b : key_bindings_) {
      if (b.is_pressed) {
        switch (b.output_key) {
          case ui::VirtualKey::kXInputPadA:
            buttons |= X_INPUT_GAMEPAD_A;
            break;
          case ui::VirtualKey::kXInputPadY:
            buttons |= X_INPUT_GAMEPAD_Y;
            break;
          case ui::VirtualKey::kXInputPadB:
            buttons |= X_INPUT_GAMEPAD_B;
            break;
          case ui::VirtualKey::kXInputPadX:
            buttons |= X_INPUT_GAMEPAD_X;
            break;
          case ui::VirtualKey::kXInputPadGuide:
            buttons |= X_INPUT_GAMEPAD_GUIDE;
            break;
          case ui::VirtualKey::kXInputPadDpadLeft:
            buttons |= X_INPUT_GAMEPAD_DPAD_LEFT;
            break;
          case ui::VirtualKey::kXInputPadDpadRight:
            buttons |= X_INPUT_GAMEPAD_DPAD_RIGHT;
            break;
          case ui::VirtualKey::kXInputPadDpadDown:
            buttons |= X_INPUT_GAMEPAD_DPAD_DOWN;
            break;
          case ui::VirtualKey::kXInputPadDpadUp:
            buttons |= X_INPUT_GAMEPAD_DPAD_UP;
            break;
          case ui::VirtualKey::kXInputPadRThumbPress:
            buttons |= X_INPUT_GAMEPAD_RIGHT_THUMB;
            break;
          case ui::VirtualKey::kXInputPadLThumbPress:
            buttons |= X_INPUT_GAMEPAD_LEFT_THUMB;
            break;
          case ui::VirtualKey::kXInputPadBack:
            buttons |= X_INPUT_GAMEPAD_BACK;
            break;
          case ui::VirtualKey::kXInputPadStart:
            buttons |= X_INPUT_GAMEPAD_START;
            break;
          case ui::VirtualKey::kXInputPadLShoulder:
            buttons |= X_INPUT_GAMEPAD_LEFT_SHOULDER;
            break;
          case ui::VirtualKey::kXInputPadRShoulder:
            buttons |= X_INPUT_GAMEPAD_RIGHT_SHOULDER;
            break;
          case ui::VirtualKey::kXInputPadLTrigger:
            left_trigger = 0xFF;
            break;
          case ui::VirtualKey::kXInputPadRTrigger:
            right_trigger = 0xFF;
            break;
          case ui::VirtualKey::kXInputPadLThumbLeft:
            thumb_lx += SHRT_MIN;
            break;
          case ui::VirtualKey::kXInputPadLThumbRight:
            thumb_lx += SHRT_MAX;
            break;
          case ui::VirtualKey::kXInputPadLThumbDown:
            thumb_ly += SHRT_MIN;
            break;
          case ui::VirtualKey::kXInputPadLThumbUp:
            thumb_ly += SHRT_MAX;
            break;
          case ui::VirtualKey::kXInputPadRThumbUp:
            thumb_ry += SHRT_MAX;
            break;
          case ui::VirtualKey::kXInputPadRThumbDown:
            thumb_ry += SHRT_MIN;
            break;
          case ui::VirtualKey::kXInputPadRThumbRight:
            thumb_rx += SHRT_MAX;
            break;
          case ui::VirtualKey::kXInputPadRThumbLeft:
            thumb_rx += SHRT_MIN;
            break;
          default:
            assert_unhandled_case(b.output_key);
        }
      }
    }
  }

  out_state->gamepad.buttons = buttons;
  out_state->gamepad.left_trigger = left_trigger;
  out_state->gamepad.right_trigger = right_trigger;
  out_state->gamepad.thumb_lx = thumb_lx;
  out_state->gamepad.thumb_ly = thumb_ly;
  out_state->gamepad.thumb_rx = thumb_rx;
  out_state->gamepad.thumb_ry = thumb_ry;
  {
    std::lock_guard<xe_mutex> guard(state_lock_);
    if (std::memcmp(&out_state->gamepad, &last_gamepad_,
                    sizeof(last_gamepad_)) != 0) {
      last_gamepad_ = out_state->gamepad;
      ++packet_number_;
    }
    out_state->packet_number = packet_number_;
  }

  if (IsPassthroughEnabled()) {
    std::memset(out_state, 0, sizeof(*out_state));
  }

  return X_ERROR_SUCCESS;
}

X_RESULT KeyboardInputDriver::SetState(uint32_t user_index,
                                       X_INPUT_VIBRATION* vibration) {
  return X_ERROR_SUCCESS;
}

X_RESULT KeyboardInputDriver::GetKeystroke(uint32_t user_index, uint32_t flags,
                                           X_INPUT_KEYSTROKE* out_keystroke) {
  X_RESULT result = X_ERROR_EMPTY;

  ui::VirtualKey xinput_virtual_key = ui::VirtualKey::kNone;
  uint16_t unicode = 0;
  uint16_t keystroke_flags = 0;
  uint8_t hid_code = 0;

  // Pop from the queue. A release is sent only for a press the title received,
  // and repeats of a press it didn't receive aren't a new press.
  auto global_lock = global_critical_region_.Acquire();
  if (native_controller_keystroke_generation_) {
    return X_ERROR_EMPTY;
  }
  KeyEvent evt;
  do {
    if (key_events_.empty()) {
      // No keys!
      return X_ERROR_EMPTY;
    }
    evt = key_events_.front();
    key_events_.pop();
  } while (evt.transition ? evt.prev_state && !delivered_.count(evt.virtual_key)
                          : !delivered_.erase(evt.virtual_key));

  if (!IsPassthroughEnabled()) {
    for (const KeyBinding& b : key_bindings_) {
      if (b.input_key == evt.virtual_key &&
          MatchesCaps(b.lowercase, b.uppercase, evt.is_capital)) {
        xinput_virtual_key = b.output_key;
      }
    }
  } else {
    // Passthrough: surface the raw key plus modifier/unicode/HID-usage.
    xinput_virtual_key = evt.virtual_key;
    hid_code = VirtualKeyToHIDUsage(evt.virtual_key);
    if (evt.unicode) {
      unicode = evt.unicode;
      keystroke_flags |= 0x1000;  // XINPUT_KEYSTROKE_VALIDUNICODE
    }

    if (evt.is_capital) {
      keystroke_flags |= 0x0008;  // XINPUT_KEYSTROKE_SHIFT
    }
    if (evt.is_ctrl_pressed) {
      keystroke_flags |= 0x0010;  // XINPUT_KEYSTROKE_CTRL
    }
    if (evt.is_alt_pressed) {
      keystroke_flags |= 0x0020;  // XINPUT_KEYSTROKE_ALT
    }
  }

  if (xinput_virtual_key != ui::VirtualKey::kNone) {
    if (evt.transition == true) {
      keystroke_flags |= 0x0001;  // XINPUT_KEYSTROKE_KEYDOWN
      if (!delivered_.insert(evt.virtual_key).second) {
        keystroke_flags |= 0x0004;  // XINPUT_KEYSTROKE_REPEAT
      }
    } else if (evt.transition == false) {
      keystroke_flags |= 0x0002;  // XINPUT_KEYSTROKE_KEYUP
    }

    result = X_ERROR_SUCCESS;
  }

  out_keystroke->virtual_key = uint16_t(xinput_virtual_key);
  out_keystroke->unicode = unicode;
  out_keystroke->flags = keystroke_flags;
  out_keystroke->user_index = user_index;
  out_keystroke->hid_code = hid_code;

  // X_ERROR_EMPTY if no new keys
  // X_ERROR_DEVICE_NOT_CONNECTED if no device
  // X_ERROR_SUCCESS if key
  return result;
}

void KeyboardInputDriver::KeyboardWindowInputListener::OnKeyDown(
    ui::KeyEvent& e) {
  driver_.OnKey(e, true);
}

void KeyboardInputDriver::KeyboardWindowInputListener::OnKeyUp(
    ui::KeyEvent& e) {
  driver_.OnKey(e, false);
}

void KeyboardInputDriver::KeyboardWindowInputListener::OnKeyChar(
    ui::KeyEvent& e) {
  driver_.OnChar(e);
}

void KeyboardInputDriver::KeyboardWindowInputListener::OnTextInput(
    ui::TextInputEvent& e) {
  driver_.OnText(e);
}

void KeyboardInputDriver::KeyboardWindowInputListener::OnGotFocus(
    ui::UISetupEvent&) {
  auto lock = driver_.global_critical_region_.Acquire();
  driver_.native_text_.SetFocused(true);
  if (driver_.native_text_.State(driver_.native_text_generation_)
          .owner_revision) {
    driver_.QuarantinePressedKeys();
  }
  for (auto it = driver_.quarantined_.begin();
       it != driver_.quarantined_.end();) {
    if (driver_.window()->QueryPhysicalKeyState(*it) ==
        ui::Window::PhysicalKeyState::kReleased) {
      driver_.keys_down_.erase(*it);
      it = driver_.quarantined_.erase(it);
    } else {
      ++it;
    }
  }
}

void KeyboardInputDriver::KeyboardWindowInputListener::OnLostFocus(
    ui::UISetupEvent& e) {
  driver_.native_text_.SetFocused(false);
  driver_.ClearPressedKeys();
}

void KeyboardInputDriver::ClearPressedKeys() {
  // Releases happening to another focused window aren't seen as KeyUp here;
  // clear all bindings on focus loss to avoid stuck keys on refocus.
  auto global_lock = global_critical_region_.Acquire();
  for (auto& b : key_bindings_) {
    b.is_pressed = false;
  }
  // Keystroke readers get the releases too.
  const auto native = native_text_.State(native_text_generation_);
  for (const auto& [virtual_key, is_capital] : keys_down_) {
    if (native.owner_revision) {
      quarantined_.insert(virtual_key);
    }
    KeyEvent release;
    release.virtual_key = virtual_key;
    release.is_capital = is_capital;
    if (!native_controller_keystroke_generation_) {
      key_events_.push(release);
    }
    native_text_.PushKey(uint16_t(virtual_key), 0,
                         VirtualKeyToHIDUsage(virtual_key), false, false);
  }
  keys_down_.clear();
}

void KeyboardInputDriver::DiscardKeystrokes() {
  auto global_lock = global_critical_region_.Acquire();
  // Releases of presses the title received are kept so they still pair.
  std::queue<KeyEvent> releases;
  for (; !key_events_.empty(); key_events_.pop()) {
    const KeyEvent& evt = key_events_.front();
    if (!evt.transition && delivered_.count(evt.virtual_key)) {
      releases.push(evt);
    }
  }
  key_events_.swap(releases);
}

void KeyboardInputDriver::ResetKeystrokes() {
  auto global_lock = global_critical_region_.Acquire();
  key_events_ = {};
  delivered_.clear();
}

void KeyboardInputDriver::SetNativeControllerKeystrokeGeneration(
    uint64_t generation) {
  auto lock = global_critical_region_.Acquire();
  if (native_controller_keystroke_generation_ == generation) {
    return;
  }
  key_events_ = {};
  delivered_.clear();
  native_controller_keystroke_generation_ = generation;
  // Actual held keys/mapped gamepad state remain intact for native191. Text
  // availability is deliberately unchanged; this is not a Unicode/IME claim.
}

void KeyboardInputDriver::QuarantinePressedKeys() {
  for (const auto& [key, capital] : keys_down_) {
    quarantined_.insert(key);
  }
  for (auto& binding : key_bindings_) {
    binding.is_pressed = false;
  }
  key_events_ = {};
  delivered_.clear();
}

void KeyboardInputDriver::SetNativeTextGeneration(uint64_t generation) {
  auto lock = global_critical_region_.Acquire();
  if (native_text_generation_ == generation) {
    return;
  }
  if (native_text_.State(native_text_generation_).owner_revision) {
    QuarantinePressedKeys();
  }
  native_text_generation_ = generation;
  native_text_.SetGeneration(native_text_available_ ? generation : 0);
}

bool KeyboardInputDriver::SetNativeTextOwner(uint64_t generation,
                                             uint64_t revision, uint32_t user,
                                             bool enabled) {
  auto lock = global_critical_region_.Acquire();
  const auto before = native_text_.State(generation);
  const bool accepted =
      native_text_.SetOwner(generation, revision, user, enabled);
  if (accepted && before.invalidation_epoch !=
                      native_text_.State(generation).invalidation_epoch) {
    QuarantinePressedKeys();
  }
  return accepted;
}

NativeTextState KeyboardInputDriver::GetNativeTextState(
    uint64_t generation) const {
  return native_text_.State(generation);
}

void KeyboardInputDriver::SetNativeTextHostModal(uint64_t generation,
                                                 bool blocked) {
  auto lock = global_critical_region_.Acquire();
  const auto before = native_text_.State(generation).invalidation_epoch;
  native_text_.SetHostModal(generation, blocked);
  if (before != native_text_.State(generation).invalidation_epoch) {
    QuarantinePressedKeys();
  }
}

void KeyboardInputDriver::ResetNativeText(uint64_t generation) {
  auto lock = global_critical_region_.Acquire();
  native_text_.Reset(generation);
  if (generation == native_text_generation_ &&
      native_text_.State(generation).owner_revision) {
    QuarantinePressedKeys();
  }
}

X_RESULT KeyboardInputDriver::GetNativeTextKeystroke(
    uint64_t generation, uint32_t user_index, X_INPUT_KEYSTROKE* output) {
  return native_text_.Pop(generation, user_index, output);
}

void KeyboardInputDriver::OnKey(ui::KeyEvent& e, bool is_down) {
  auto global_lock = global_critical_region_.Acquire();

  if (quarantined_.contains(e.virtual_key())) {
    if (!is_down) {
      quarantined_.erase(e.virtual_key());
      keys_down_.erase(e.virtual_key());
    }
    return;
  }
  const auto native = native_text_.State(native_text_generation_);
  const bool native_claimed =
      native.owner_revision && native.user < XUserMaxUserCount;
  // Ignore releases and repeats of keys whose press another listener handled.
  // A held key keeps the shift state of its press so it maps to one binding
  // throughout.
  auto it = keys_down_.find(e.virtual_key());
  if (it == keys_down_.end()) {
    if (!is_down || e.prev_state()) {
      return;
    }
    it = keys_down_.emplace(e.virtual_key(), e.is_shift_pressed()).first;
  }
  const bool is_capital = it->second;
  if (!is_down) {
    keys_down_.erase(it);
  }

  KeyEvent key;
  key.virtual_key = e.virtual_key();
  key.transition = is_down;
  key.prev_state = e.prev_state();
  key.repeat_count = e.repeat_count();
  key.is_capital = is_capital;
  key.is_ctrl_pressed = e.is_ctrl_pressed();
  key.is_alt_pressed = e.is_alt_pressed();

  if (!native_controller_keystroke_generation_ && !native_claimed) {
    key_events_.push(key);
  }
  uint16_t native_modifiers = (key.is_capital ? 0x0008 : 0) |
                              (key.is_ctrl_pressed ? 0x0010 : 0) |
                              (key.is_alt_pressed ? 0x0020 : 0);
  const uint16_t native_key = uint16_t(key.virtual_key);
  const bool text_key = native_key == 0x20 ||
                        (native_key >= '0' && native_key <= '9') ||
                        (native_key >= 'A' && native_key <= 'Z') ||
                        (native_key >= 0x60 && native_key <= 0x6F) ||
                        (native_key >= 0xBA && native_key <= 0xE2);
  const bool shortcut = key.is_ctrl_pressed != key.is_alt_pressed;
  native_text_.PushKey(uint16_t(key.virtual_key), native_modifiers,
                       VirtualKeyToHIDUsage(key.virtual_key), is_down,
                       is_down && key.prev_state, text_key && !shortcut,
                       e.text_input_id());
  if (native_claimed) {
    e.set_handled(true);
    return;
  }
  for (auto& key_binding : key_bindings_) {
    if (key_binding.input_key != key.virtual_key) {
      continue;
    }
    if (is_down) {
      if (MatchesCaps(key_binding.lowercase, key_binding.uppercase,
                      is_capital)) {
        key_binding.is_pressed = true;
      }
    } else {
      // Clear all variants on release so a shift/caps change mid-press
      // can't leave a binding stuck.
      key_binding.is_pressed = false;
    }
  }
}

void KeyboardInputDriver::OnChar(ui::KeyEvent& e) {
  // WM_CHAR convention: OnKeyChar's virtual_key carries the unicode codepoint.
  auto global_lock = global_critical_region_.Acquire();
  const auto native = native_text_.State(native_text_generation_);
  if (native.owner_revision) {
    e.set_handled(true);
    return;
  }
  if (IsPassthroughEnabled() && !key_events_.empty()) {
    auto& last = key_events_.back();
    if (last.transition && last.unicode == 0) {
      last.unicode = static_cast<uint16_t>(e.virtual_key());
    }
  }
}

void KeyboardInputDriver::OnText(ui::TextInputEvent& e) {
  auto lock = global_critical_region_.Acquire();
  const auto native = native_text_.State(native_text_generation_);
  if (!native.owner_revision || native.user >= XUserMaxUserCount) {
    return;
  }
  e.set_handled(true);
  native_text_.CommitText(e.source_key(), e.text(), e.host_repeat(),
                          e.composed());
}

InputType KeyboardInputDriver::GetInputType() const {
  return cvars::keyboard_passthrough ? InputType::Keyboard
                                     : InputType::Controller;
}

std::vector<InputDeviceInfo> KeyboardInputDriver::EnumerateDevices() {
  // Passthrough surfaces keystrokes via GetKeystroke directly; it doesn't
  // route through the slot binding table.
  if (IsPassthroughEnabled()) {
    return {};
  }
  InputDeviceInfo info{};
  info.driver_slot = 0;
  info.stable_id = "keyboard";
  info.display_name = "Keyboard";
  return {info};
}

}  // namespace keyboard
}  // namespace hid
}  // namespace xe
