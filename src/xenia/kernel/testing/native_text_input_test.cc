/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia contributors. All rights reserved.                    *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */
#include <memory>
#include <queue>
#include <string>

#include "third_party/catch/include/catch.hpp"
#include "xenia/base/clock.h"
#include "xenia/hid/input_driver.h"
#include "xenia/hid/input_system.h"
#include "xenia/hid/native_text_input_queue.h"
#include "xenia/ui/virtual_key.h"

namespace xe::kernel::test {
namespace {
using namespace hid;

void EnableText(NativeTextInputQueue& queue, uint64_t generation = 10,
                uint32_t user = 1, uint64_t revision = 10) {
  queue.SetGeneration(generation);
  queue.SetFocused(true);
  REQUIRE(queue.SetOwner(generation, revision, user, true));
}

TEST_CASE("Native BMP commit is invisible until finalized and XAM owns repeats",
          "[kernel][native_input]") {
  NativeTextInputQueue q;
  EnableText(q);
  q.PushKey('A', 0x18, 4, true, false, true, 91);
  X_INPUT_KEYSTROKE out{};
  out.virtual_key = 0xCAFE;
  CHECK(q.Pop(10, 1, &out) == X_ERROR_EMPTY);
  CHECK(uint16_t(out.virtual_key) == 0xCAFE);
  REQUIRE(q.CommitText(91, U"a"));
  const auto time = q.State(10).last_input_time;
  REQUIRE(q.Pop(10, 1, &out) == X_ERROR_SUCCESS);
  CHECK(uint16_t(out.virtual_key) == 'A');
  CHECK(uint16_t(out.unicode) == 'a');
  CHECK(uint16_t(out.flags) == 0x1019);
  CHECK(out.user_index == 1);
  CHECK(out.hid_code == 4);
  q.PushKey('A', 0x18, 4, true, true, true, 91);
  CHECK_FALSE(q.CommitText(91, U"a", true));
  CHECK(q.State(10).last_input_time == time);
  CHECK(q.Pop(10, 1, &out) == X_ERROR_EMPTY);
  q.PushKey('A', 0, 4, false, false);
  REQUIRE(q.Pop(10, 1, &out) == X_ERROR_SUCCESS);
  CHECK(uint16_t(out.flags) == X_INPUT_KEYSTROKE_KEYUP);
}

TEST_CASE("Native commits correlate exact physical IDs across overlapping keys",
          "[kernel][native_input]") {
  NativeTextInputQueue q;
  EnableText(q);
  q.PushKey('A', 0, 4, true, false, true, 1);
  q.PushKey(0x10, 8, 0xE1, true, false);
  q.PushKey('B', 8, 5, true, false, true, 2);
  REQUIRE(q.CommitText(2, U"B"));
  X_INPUT_KEYSTROKE out{};
  CHECK(q.Pop(10, 1, &out) == X_ERROR_EMPTY);
  REQUIRE(q.CommitText(1, U"a"));
  REQUIRE(q.Pop(10, 1, &out) == X_ERROR_SUCCESS);
  CHECK(uint16_t(out.virtual_key) == 'A');
  CHECK(uint16_t(out.unicode) == 'a');
  REQUIRE(q.Pop(10, 1, &out) == X_ERROR_SUCCESS);
  CHECK(uint16_t(out.virtual_key) == 0x10);
  CHECK(uint16_t(out.unicode) == 0);
  REQUIRE(q.Pop(10, 1, &out) == X_ERROR_SUCCESS);
  CHECK(uint16_t(out.virtual_key) == 'B');
  CHECK(uint16_t(out.unicode) == 'B');
}

TEST_CASE("Native pending input belongs to its lease and adopted user",
          "[kernel][native_input]") {
  NativeTextInputQueue q;
  EnableText(q);
  q.PushKey('A', 0, 4, true, false, true, 1);
  REQUIRE(q.SetOwner(10, 11, 2, true));
  CHECK_FALSE(q.CommitText(1, U"a"));
  q.PushKey('B', 0, 5, true, false, true, 2);
  REQUIRE(q.CommitText(2, U"b"));
  X_INPUT_KEYSTROKE out{};
  out.virtual_key = 0xCAFE;
  CHECK(q.Pop(10, 1, &out) == X_ERROR_DEVICE_NOT_CONNECTED);
  CHECK(uint16_t(out.virtual_key) == 0xCAFE);
  REQUIRE(q.Pop(10, 2, &out) == X_ERROR_SUCCESS);
  CHECK(out.user_index == 2);
  CHECK(uint16_t(out.unicode) == 'b');
  q.SetGeneration(12);
  CHECK(q.Pop(10, 2, &out) == X_ERROR_DEVICE_NOT_CONNECTED);
  CHECK_FALSE(q.SetOwner(10, 20, 1, true));
}

TEST_CASE(
    "Text claim reserves mode before enable and stale claims cannot reopen",
    "[kernel][native_input]") {
  NativeTextInputQueue q;
  q.SetGeneration(13);
  q.SetFocused(true);
  REQUIRE(q.SetOwner(13, 41, 1, false));
  CHECK(q.State(13).available);
  CHECK_FALSE(q.State(13).enabled);
  CHECK(q.State(13).owner_revision == 41);
  q.PushKey('A', 0, 4, true, false, true, 1);
  REQUIRE(q.SetOwner(13, 41, 1, true));
  CHECK_FALSE(q.CommitText(1, U"a"));
  q.PushKey('B', 0, 5, true, false, true, 2);
  REQUIRE(q.CommitText(2, U"b"));
  REQUIRE(q.SetOwner(13, 0, UINT32_MAX, false));
  CHECK_FALSE(q.SetOwner(13, 41, 1, true));
  CHECK_FALSE(q.SetOwner(13, 42, 4, true));
  REQUIRE(q.SetOwner(13, 42, 1, true));
  X_INPUT_KEYSTROKE out{};
  CHECK(q.Pop(13, 1, &out) == X_ERROR_EMPTY);
  CHECK(q.Pop(13, 4, &out) == X_ERROR_INVALID_PARAMETER);
  CHECK(q.Pop(13, 1, nullptr) == X_ERROR_INVALID_PARAMETER);
}

TEST_CASE(
    "Text invalidation epochs are nonconsuming across focus and modal edges",
    "[kernel][native_input]") {
  NativeTextInputQueue q;
  EnableText(q);
  auto epoch = q.State(10).invalidation_epoch;
  q.PushKey('A', 0, 4, true, false, true, 1);
  q.SetFocused(false);
  CHECK(q.State(10).invalidation_epoch > epoch);
  epoch = q.State(10).invalidation_epoch;
  CHECK(q.State(10).invalidation_epoch == epoch);
  X_INPUT_KEYSTROKE out{};
  CHECK(q.Pop(10, 1, &out) == X_ERROR_DEVICE_NOT_CONNECTED);
  CHECK(q.State(10).invalidation_epoch == epoch);
  q.SetFocused(true);
  CHECK_FALSE(q.CommitText(1, U"a"));
  epoch = q.State(10).invalidation_epoch;
  q.SetHostModal(10, true);
  CHECK(q.State(10).invalidation_epoch > epoch);
  q.PushKey('M', 0, 16, true, false, true, 2);
  q.SetHostModal(10, false);
  CHECK_FALSE(q.CommitText(2, U"m"));
  CHECK(q.Pop(10, 1, &out) == X_ERROR_EMPTY);
  epoch = q.State(10).invalidation_epoch;
  q.Reset(9);
  CHECK(q.State(10).invalidation_epoch == epoch);
  q.Reset(10);
  CHECK(q.State(10).invalidation_epoch > epoch);
}

TEST_CASE("Unsupported whole commits never leak a BMP prefix",
          "[kernel][native_input]") {
  for (const auto& text :
       {std::u32string(U"ab"), std::u32string(U"\U0001F642"),
        std::u32string(1, char32_t(0xD83D)), std::u32string()}) {
    NativeTextInputQueue q;
    EnableText(q);
    q.PushKey('A', 0, 4, true, false, true, 1);
    const auto epoch = q.State(10).invalidation_epoch;
    CHECK_FALSE(q.CommitText(1, text));
    CHECK(q.State(10).invalidation_epoch > epoch);
    X_INPUT_KEYSTROKE out{};
    CHECK(q.Pop(10, 1, &out) == X_ERROR_EMPTY);
  }
  NativeTextInputQueue q;
  EnableText(q);
  q.PushKey('E', 0, 8, true, false, true, 2);
  CHECK_FALSE(q.CommitText(2, U"\u00E9", false, true));
  X_INPUT_KEYSTROKE out{};
  CHECK(q.Pop(10, 1, &out) == X_ERROR_EMPTY);
}

TEST_CASE("Uncommitted physical presses cannot become guessed text or releases",
          "[kernel][native_input]") {
  NativeTextInputQueue q;
  EnableText(q);
  q.PushKey(0xC0, 0, 0x35, true, false, true, 1);
  q.PushKey(0xC0, 0, 0x35, false, false);
  CHECK_FALSE(q.CommitText(1, U"x"));
  X_INPUT_KEYSTROKE out{};
  CHECK(q.Pop(10, 1, &out) == X_ERROR_EMPTY);
  q.PushKey('E', 0, 8, true, false, true, 2);
  REQUIRE(q.CommitText(2, U"\u03A9"));
  REQUIRE(q.Pop(10, 1, &out) == X_ERROR_SUCCESS);
  CHECK(uint16_t(out.unicode) == 0x03A9);
  CHECK(uint16_t(out.virtual_key) == 'E');
}

TEST_CASE("Native text overflow invalidates without a repeating unmatched edge",
          "[kernel][native_input]") {
  NativeTextInputQueue q;
  EnableText(q);
  const auto epoch = q.State(10).invalidation_epoch;
  for (uint16_t vk = 1; vk <= NativeTextInputQueue::kMaxPendingEdges + 1;
       ++vk) {
    q.PushKey(vk, 0, 0, true, false);
  }
  CHECK(q.State(10).invalidation_epoch > epoch);
  X_INPUT_KEYSTROKE out{};
  CHECK(q.Pop(10, 1, &out) == X_ERROR_EMPTY);
  q.PushKey(1, 0, 0, false, false);
  CHECK(q.Pop(10, 1, &out) == X_ERROR_EMPTY);
}

TEST_CASE("Native text activity time is actual common-clock input not polling",
          "[kernel][native_input]") {
  NativeTextInputQueue q;
  EnableText(q);
  CHECK(q.State(10).last_input_time == 0);
  const auto before = Clock::QueryGuestInterruptTime();
  q.PushKey('A', 0, 4, true, false, true, 1);
  REQUIRE(q.CommitText(1, U"a"));
  const auto accepted = q.State(10).last_input_time;
  CHECK(accepted >= before);
  CHECK(accepted <= Clock::QueryGuestInterruptTime());
  X_INPUT_KEYSTROKE out{};
  q.Pop(10, 1, &out);
  q.Pop(10, 1, &out);
  CHECK(q.State(10).last_input_time == accepted);
  q.PushKey('A', 0, 4, true, true, true, 1);
  q.CommitText(1, U"a", true);
  CHECK(q.State(10).last_input_time == accepted);
  REQUIRE(q.SetOwner(10, 11, 1, true));
  CHECK(q.State(10).last_input_time == 0);
}

class PhysicalFixtureDriver final : public InputDriver {
 public:
  explicit PhysicalFixtureDriver(InputType type = Controller)
      : InputDriver(nullptr, 0), type_(type) {}
  X_STATUS Setup() override { return X_STATUS_SUCCESS; }
  InputType GetInputType() const override { return type_; }
  X_RESULT GetCapabilities(uint32_t user, uint32_t,
                           X_INPUT_CAPABILITIES* caps) override {
    if (user || !connected) {
      return X_ERROR_DEVICE_NOT_CONNECTED;
    }
    *caps = {};
    caps->type =
        type_ == Controller ? XINPUT_DEVTYPE_GAMEPAD : XINPUT_DEVTYPE_KEYBOARD;
    caps->sub_type = type_ == Controller ? XINPUT_DEVSUBTYPE_GAMEPAD
                                         : XINPUT_DEVSUBTYPE_USB_KEYBOARD;
    caps->gamepad.thumb_lx = caps->gamepad.thumb_ly = 32767;
    caps->gamepad.thumb_rx = caps->gamepad.thumb_ry = 32767;
    return X_ERROR_SUCCESS;
  }
  X_RESULT GetState(uint32_t user, X_INPUT_STATE* state) override {
    if (user || !connected) {
      return X_ERROR_DEVICE_NOT_CONNECTED;
    }
    *state = {};
    state->packet_number = 20;
    state->gamepad.buttons = buttons;
    return X_ERROR_SUCCESS;
  }
  X_RESULT SetState(uint32_t, X_INPUT_VIBRATION*) override {
    return X_ERROR_SUCCESS;
  }
  X_RESULT GetKeystroke(uint32_t, uint32_t,
                        X_INPUT_KEYSTROKE* output) override {
    if (title.empty()) {
      return X_ERROR_EMPTY;
    }
    *output = title.front();
    title.pop();
    return X_ERROR_SUCCESS;
  }
  std::vector<InputDeviceInfo> EnumerateDevices() override {
    if (!connected) {
      return {};
    }
    InputDeviceInfo info{};
    info.driver_slot = 0;
    info.stable_id = type_ == Controller ? "fixture-pad" : "fixture-keyboard";
    info.display_name = "Fixture pad";
    return {info};
  }
  void DiscardKeystrokes() override {
    ++discards;
    title = {};
  }
  bool HasNativeTextInput() const override { return true; }
  void SetNativeTextGeneration(uint64_t generation) override {
    native.SetGeneration(generation);
    native.SetFocused(true);
    if (generation) {
      native.SetOwner(generation, 1, 0, true);
    }
  }
  void ResetNativeText(uint64_t generation) override {
    native.Reset(generation);
  }
  X_RESULT GetNativeTextKeystroke(uint64_t generation, uint32_t user,
                                  X_INPUT_KEYSTROKE* output) override {
    return native.Pop(generation, user, output);
  }
  void Key(char key) {
    X_INPUT_KEYSTROKE stroke{};
    stroke.virtual_key = key;
    stroke.flags = X_INPUT_KEYSTROKE_KEYDOWN;
    title.push(stroke);
    native.PushKey(key, 0, 4, true, false);
  }
  const InputType type_;
  bool connected = true;
  uint16_t buttons = X_INPUT_GAMEPAD_A;
  unsigned discards = 0;
  std::queue<X_INPUT_KEYSTROKE> title;
  NativeTextInputQueue native;
};

struct InputFixture {
  InputFixture() {
    auto driver = std::make_unique<PhysicalFixtureDriver>();
    physical = driver.get();
    input.AddDriver(std::move(driver));
    input.Setup();
  }
  InputSystem input{nullptr};
  PhysicalFixtureDriver* physical = nullptr;
};

TEST_CASE("Blocked HLE title polling preserves the native text audience",
          "[kernel][native_input]") {
  InputFixture fixture;
  auto& input = fixture.input;
  auto* physical = fixture.physical;
  REQUIRE(input.BeginNativeInputTransport(21));
  REQUIRE(input.SetNativeInputOwner(21, true));
  X_INPUT_KEYSTROKE stroke{};
  CHECK(input.GetNativeTextKeystroke(21, 0, &stroke) == X_ERROR_EMPTY);
  physical->Key('C');
  const auto discards = physical->discards;
  for (unsigned i = 0; i != 32; ++i) {
    CHECK(input.GetKeystroke(0, Controller, &stroke) == X_ERROR_EMPTY);
  }
  CHECK(physical->discards == discards);
  REQUIRE(input.GetNativeTextKeystroke(21, 0, &stroke) == X_ERROR_SUCCESS);
  CHECK(uint16_t(stroke.virtual_key) == 'C');
  X_INPUT_STATE state{};
  REQUIRE(input.GetState(0, Controller, &state) == X_ERROR_SUCCESS);
  CHECK(uint16_t(state.gamepad.buttons) == 0);
  REQUIRE(input.GetStateForUI(0, Controller, &state) == X_ERROR_SUCCESS);
  CHECK(uint16_t(state.gamepad.buttons) == X_INPUT_GAMEPAD_A);
  input.EndNativeInputTransport(21);
}

TEST_CASE("Host modal preemption drops its text and invalidates native repeat",
          "[kernel][native_input]") {
  InputFixture fixture;
  auto& input = fixture.input;
  REQUIRE(input.BeginNativeInputTransport(22));
  REQUIRE(input.SetNativeInputOwner(22, true));
  X_INPUT_KEYSTROKE stroke{};
  input.GetNativeTextKeystroke(22, 0, &stroke);
  input.AddUIInputBlocker();
  fixture.physical->Key('M');
  CHECK(input.GetNativeTextKeystroke(22, 0, &stroke) ==
        X_ERROR_DEVICE_NOT_CONNECTED);
  input.RemoveUIInputBlocker();
  CHECK(input.GetNativeTextKeystroke(22, 0, &stroke) == X_ERROR_EMPTY);
  fixture.physical->Key('N');
  REQUIRE(input.GetNativeTextKeystroke(22, 0, &stroke) == X_ERROR_SUCCESS);
  CHECK(uint16_t(stroke.virtual_key) == 'N');
  input.EndNativeInputTransport(22);
}

TEST_CASE("Old transport release cannot withdraw a replacement input owner",
          "[kernel][native_input]") {
  InputFixture fixture;
  auto& input = fixture.input;
  REQUIRE(input.BeginNativeInputTransport(23));
  input.EndNativeInputTransport(23);
  REQUIRE(input.BeginNativeInputTransport(24));
  REQUIRE(input.SetNativeInputOwner(24, true));
  CHECK_FALSE(input.BeginNativeInputTransport(25));
  input.EndNativeInputTransport(23);
  CHECK_FALSE(input.SetNativeInputOwner(23, false));
  X_INPUT_STATE state{};
  REQUIRE(input.GetState(0, Controller, &state) == X_ERROR_SUCCESS);
  CHECK(uint16_t(state.gamepad.buttons) == 0);
  input.EndNativeInputTransport(24);
  // A held close button stays masked until the physical release.
  REQUIRE(input.GetState(0, Controller, &state) == X_ERROR_SUCCESS);
  CHECK(uint16_t(state.gamepad.buttons) == 0);
  fixture.physical->buttons = 0;
  input.GetState(0, Controller, &state);
  fixture.physical->buttons = X_INPUT_GAMEPAD_A;
  REQUIRE(input.GetState(0, Controller, &state) == X_ERROR_SUCCESS);
  CHECK(uint16_t(state.gamepad.buttons) == X_INPUT_GAMEPAD_A);
}

// Real InputSystem plus scripted host-provider samples. No native XThread,
// kernel, guest user assignment or native keystroke execution is modeled here.
TEST_CASE("Native close masks held gamepad buttons across state audiences",
          "[kernel][native_input]") {
  InputFixture f;
  auto& input = f.input;
  auto* pad = f.physical;
  input.BindSlot(1, pad, 0, "fixture-pad", "Fixture pad");
  REQUIRE(input.BeginNativeInputTransport(31));
  pad->buttons = X_INPUT_GAMEPAD_B;
  REQUIRE(input.SetNativeInputOwner(31, true));
  X_INPUT_STATE state{};
  // The physical B must reach native UI before it closes, even while ordinary
  // HLE title input is neutralized by the native UI owner.
  REQUIRE(input.GetStateForNativeDriver(1, Controller, &state) ==
          X_ERROR_SUCCESS);
  CHECK(uint16_t(state.gamepad.buttons) == X_INPUT_GAMEPAD_B);
  REQUIRE(input.GetState(1, Controller, &state) == X_ERROR_SUCCESS);
  CHECK(uint16_t(state.gamepad.buttons) == 0);
  REQUIRE(input.SetNativeInputOwner(31, false));
  // A new A remains usable; reading either audience does not consume B's
  // suppression as though a read itself were a release.
  pad->buttons = X_INPUT_GAMEPAD_A | X_INPUT_GAMEPAD_B;
  REQUIRE(input.GetStateForNativeDriver(1, Controller, &state) ==
          X_ERROR_SUCCESS);
  CHECK(uint16_t(state.gamepad.buttons) == X_INPUT_GAMEPAD_A);
  REQUIRE(input.GetState(1, Controller, &state) == X_ERROR_SUCCESS);
  CHECK(uint16_t(state.gamepad.buttons) == X_INPUT_GAMEPAD_A);
  REQUIRE(input.GetStateForUI(1, Controller, &state) == X_ERROR_SUCCESS);
  CHECK(uint16_t(state.gamepad.buttons) ==
        (X_INPUT_GAMEPAD_A | X_INPUT_GAMEPAD_B));
  REQUIRE(input.GetStateForNativeDriver(1, Controller, &state) ==
          X_ERROR_SUCCESS);
  CHECK(uint16_t(state.gamepad.buttons) == X_INPUT_GAMEPAD_A);
  input.EndNativeInputTransport(31);
}

TEST_CASE("Only successful raw controller release rearms the closed slot",
          "[kernel][native_input]") {
  InputFixture f;
  auto& input = f.input;
  auto* pad = f.physical;
  auto keyboard = std::make_unique<PhysicalFixtureDriver>(Keyboard);
  auto* keyboard_ptr = keyboard.get();
  keyboard_ptr->buttons = 0;
  input.AddDriver(std::move(keyboard));
  REQUIRE(input.Setup() == X_STATUS_SUCCESS);
  input.BindSlot(1, pad, 0, "fixture-pad", "Fixture pad");
  input.BindSlot(1, keyboard_ptr, 0, "fixture-keyboard", "Fixture keyboard");
  REQUIRE(input.BeginNativeInputTransport(32));
  pad->buttons = X_INPUT_GAMEPAD_B;
  REQUIRE(input.SetNativeInputOwner(32, true));
  REQUIRE(input.SetNativeInputOwner(32, false));
  X_INPUT_STATE state{};
  // A successful unrelated keyboard sample must not clear a controller latch.
  REQUIRE(input.GetStateForUI(1, Keyboard, &state) == X_ERROR_SUCCESS);
  CHECK(uint16_t(state.gamepad.buttons) == 0);
  REQUIRE(input.GetStateForNativeDriver(1, Controller, &state) ==
          X_ERROR_SUCCESS);
  CHECK(uint16_t(state.gamepad.buttons) == 0);
  // Requested Controller is not proof of a controller sample: a mixed query
  // can succeed through the keyboard while the actual pad read is unknown.
  pad->connected = false;
  REQUIRE(input.GetStateForUI(1, Controller | Keyboard, &state) ==
          X_ERROR_SUCCESS);
  CHECK(uint16_t(state.gamepad.buttons) == 0);
  pad->connected = true;
  REQUIRE(input.GetStateForNativeDriver(1, Controller, &state) ==
          X_ERROR_SUCCESS);
  CHECK(uint16_t(state.gamepad.buttons) == 0);
  // A different successful neutral controller also cannot turn an unknown
  // sample from a still-bound controller into proof that all buttons are up.
  auto extra = std::make_unique<PhysicalFixtureDriver>();
  auto* extra_ptr = extra.get();
  extra_ptr->buttons = 0;
  input.AddDriver(std::move(extra));
  input.BindSlot(1, extra_ptr, 0, "fixture-extra", "Fixture extra");
  extra_ptr->connected = false;
  pad->buttons = 0;
  REQUIRE(input.GetStateForUI(1, Controller, &state) == X_ERROR_SUCCESS);
  pad->buttons = X_INPUT_GAMEPAD_B;
  REQUIRE(input.GetStateForNativeDriver(1, Controller, &state) ==
          X_ERROR_SUCCESS);
  CHECK(uint16_t(state.gamepad.buttons) == 0);
  extra_ptr->connected = true;
  // No title or native filtered read occurs during the actual known Up: only
  // the host raw poll observes all bound controllers. The next Down is new.
  pad->buttons = 0;
  REQUIRE(input.GetStateForUI(1, Controller, &state) == X_ERROR_SUCCESS);
  CHECK(uint16_t(state.gamepad.buttons) == 0);
  pad->buttons = X_INPUT_GAMEPAD_B;
  REQUIRE(input.GetStateForNativeDriver(1, Controller, &state) ==
          X_ERROR_SUCCESS);
  CHECK(uint16_t(state.gamepad.buttons) == X_INPUT_GAMEPAD_B);
  input.EndNativeInputTransport(32);
}

TEST_CASE("Held close button strokes cannot become title downs or repeats",
          "[kernel][native_input]") {
  InputFixture f;
  auto& input = f.input;
  auto* pad = f.physical;
  input.BindSlot(1, pad, 0, "fixture-pad", "Fixture pad");
  REQUIRE(input.BeginNativeInputTransport(33));
  pad->buttons = X_INPUT_GAMEPAD_B;
  REQUIRE(input.SetNativeInputOwner(33, true));
  REQUIRE(input.SetNativeInputOwner(33, false));
  const auto queue = [&](ui::VirtualKey key, uint16_t flags) {
    X_INPUT_KEYSTROKE event{};
    event.virtual_key = uint16_t(key);
    event.flags = flags;
    pad->title.push(event);
  };
  queue(ui::VirtualKey::kXInputPadB,
        X_INPUT_KEYSTROKE_KEYDOWN | X_INPUT_KEYSTROKE_REPEAT);
  X_INPUT_KEYSTROKE stroke{};
  stroke.virtual_key = 0xCAFE;
  REQUIRE(input.GetKeystroke(1, Controller, &stroke) == X_ERROR_EMPTY);
  CHECK(uint16_t(stroke.virtual_key) == 0);
  CHECK(uint16_t(stroke.flags) == 0);
  // Suppression is per actual button, rather than a blanket empty-input mode.
  pad->buttons = X_INPUT_GAMEPAD_A | X_INPUT_GAMEPAD_B;
  queue(ui::VirtualKey::kXInputPadA, X_INPUT_KEYSTROKE_KEYDOWN);
  REQUIRE(input.GetKeystroke(1, Controller, &stroke) == X_ERROR_SUCCESS);
  CHECK(uint16_t(stroke.virtual_key) == uint16_t(ui::VirtualKey::kXInputPadA));
  CHECK(stroke.user_index == 1);
  pad->buttons = X_INPUT_GAMEPAD_A;
  X_INPUT_STATE state{};
  REQUIRE(input.GetStateForUI(1, Controller, &state) == X_ERROR_SUCCESS);
  pad->buttons |= X_INPUT_GAMEPAD_B;
  queue(ui::VirtualKey::kXInputPadB, X_INPUT_KEYSTROKE_KEYDOWN);
  REQUIRE(input.GetKeystroke(1, Controller, &stroke) == X_ERROR_SUCCESS);
  CHECK(uint16_t(stroke.virtual_key) == uint16_t(ui::VirtualKey::kXInputPadB));
  input.EndNativeInputTransport(33);
}

TEST_CASE("Uninstalled native text transport preserves ordinary HLE input",
          "[kernel][native_input]") {
  InputFixture fixture;
  fixture.physical->Key('L');
  X_INPUT_KEYSTROKE stroke{};
  REQUIRE(fixture.input.GetKeystroke(0, Controller, &stroke) ==
          X_ERROR_SUCCESS);
  CHECK(uint16_t(stroke.virtual_key) == 'L');
  X_INPUT_STATE state{};
  REQUIRE(fixture.input.GetState(0, Controller, &state) == X_ERROR_SUCCESS);
  CHECK(uint16_t(state.gamepad.buttons) == X_INPUT_GAMEPAD_A);
  CHECK_FALSE(fixture.input.HasNativeTextInput(99));
}
}  // namespace
}  // namespace xe::kernel::test
