/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia contributors. All rights reserved.                    *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */
#include <algorithm>
#include <unordered_set>
#include "third_party/catch/include/catch.hpp"
#include "xenia/hid/input_system.h"
#include "xenia/hid/keyboard/keyboard_input_driver.h"
#include "xenia/ui/imgui_drawer.h"
#include "xenia/ui/physical_key_tracker.h"
#include "xenia/ui/ui_event.h"
#include "xenia/ui/window.h"
#include "xenia/ui/windowed_app_context.h"
namespace xe::hid::keyboard::test {
class NativeKeystrokeRoutingTestAccess {
 public:
  static void BindA(KeyboardInputDriver& driver) {
    driver.ParseKeyBinding(ui::VirtualKey::kXInputPadA, "test A", "0x41");
  }
  static void Key(KeyboardInputDriver& driver, bool down, bool repeat = false) {
    ui::KeyEvent event(nullptr, ui::VirtualKey::kA, 1, repeat, false, false,
                       false, false);
    driver.OnKey(event, down);
  }
  static size_t Queued(const KeyboardInputDriver& driver) {
    return driver.key_events_.size();
  }
  static bool RawAIsHeld(const KeyboardInputDriver& driver) {
    return std::any_of(driver.key_bindings_.begin(), driver.key_bindings_.end(),
                       [](const auto& binding) {
                         return binding.input_key == ui::VirtualKey::kA &&
                                binding.output_key ==
                                    ui::VirtualKey::kXInputPadA &&
                                binding.is_pressed;
                       });
  }
};
namespace {
class Context final : public ui::WindowedAppContext {
 protected:
  void NotifyUILoopOfPendingFunctions() override {}
  void PlatformQuitFromUIThread() override {}
};
class Window final : public ui::Window {
 public:
  explicit Window(Context& context, bool native_text = false)
      : ui::Window(context, "native keyboard transport", 1280, 720),
        native_text_(native_text) {}
  bool SupportsNativeTextCommits() const override { return native_text_; }
  PhysicalKeyState QueryPhysicalKeyState(ui::VirtualKey key) const override {
    ++physical_queries_;
    return physical_down_.contains(key) ? PhysicalKeyState::kPressed
                                        : PhysicalKeyState::kReleased;
  }
  void ReleaseOutsideFocusForTest(ui::VirtualKey key) {
    physical_down_.erase(key);
  }
  unsigned physical_queries() const { return physical_queries_; }
  void FocusForTest(bool focused) {
    WindowDestructionReceiver receiver(this);
    OnFocusUpdate(focused, receiver);
  }
  void KeyForTest(ui::VirtualKey key, bool down, uint64_t id,
                  bool repeat = false) {
    if (down) {
      physical_down_.insert(key);
    } else {
      physical_down_.erase(key);
    }
    WindowDestructionReceiver receiver(this);
    ui::KeyEvent event(this, key, 1, repeat, false, false, false, false, id);
    if (down) {
      OnKeyDown(event, receiver);
    } else {
      OnKeyUp(event, receiver);
    }
  }
  void LegacyCharForTest(uint16_t c) {
    WindowDestructionReceiver receiver(this);
    ui::KeyEvent event(this, static_cast<ui::VirtualKey>(c), 1, false, false,
                       false, false, false);
    OnKeyChar(event, receiver);
  }
  void CommitForTest(std::u32string text, uint64_t id, bool repeat = false,
                     bool composed = false) {
    WindowDestructionReceiver receiver(this);
    ui::TextInputEvent event(this, std::move(text), id, repeat, composed);
    OnTextInput(event, receiver);
  }
  ~Window() override { EnterDestructor(); }

 protected:
  bool OpenImpl() override { return true; }
  void RequestCloseImpl() override {}
  std::unique_ptr<ui::Surface> CreateSurfaceImpl(
      ui::Surface::TypeFlags) override {
    return nullptr;
  }
  void RequestPaintImpl() override {}

 private:
  bool native_text_;
  std::unordered_set<ui::VirtualKey> physical_down_;
  mutable unsigned physical_queries_ = 0;
};
}  // namespace
TEST_CASE(
    "Native controller generator transport preserves raw keyboard state "
    "without unused legacy queue growth",
    "[ui][input][native_keystroke]") {
  Context context;
  Window window(context);
  KeyboardInputDriver driver(&window, 0);
  NativeKeystrokeRoutingTestAccess::BindA(driver);
  driver.SetNativeControllerKeystrokeGeneration(41);
  NativeKeystrokeRoutingTestAccess::Key(driver, true);
  for (unsigned i = 0; i < 4096; ++i) {
    NativeKeystrokeRoutingTestAccess::Key(driver, true, true);
  }
  CHECK(NativeKeystrokeRoutingTestAccess::RawAIsHeld(driver));
  CHECK(NativeKeystrokeRoutingTestAccess::Queued(driver) == 0);
  CHECK_FALSE(driver.HasNativeTextInput());
  X_INPUT_KEYSTROKE stroke{};
  CHECK(driver.GetKeystroke(0, 1, &stroke) == X_ERROR_EMPTY);
  driver.SetNativeControllerKeystrokeGeneration(0);
  NativeKeystrokeRoutingTestAccess::Key(driver, false);
  CHECK(driver.GetKeystroke(0, 1, &stroke) ==
        X_ERROR_EMPTY);  // Never delivered its Down.
  NativeKeystrokeRoutingTestAccess::Key(driver, true);
  CHECK(driver.GetKeystroke(0, 1, &stroke) == X_ERROR_SUCCESS);
  CHECK(stroke.virtual_key == uint16_t(ui::VirtualKey::kXInputPadA));
}
TEST_CASE(
    "Inactive diagnostic transport keeps the ordinary keyboard keystroke queue",
    "[ui][input][native_keystroke]") {
  Context context;
  Window window(context);
  KeyboardInputDriver driver(&window, 0);
  NativeKeystrokeRoutingTestAccess::BindA(driver);
  driver.SetNativeControllerKeystrokeGeneration(0);
  NativeKeystrokeRoutingTestAccess::Key(driver, true);
  CHECK(NativeKeystrokeRoutingTestAccess::Queued(driver) == 1);
  X_INPUT_KEYSTROKE stroke{};
  CHECK(driver.GetKeystroke(0, 1, &stroke) == X_ERROR_SUCCESS);
}

namespace {
struct NativeTextFixture {
  Context context;
  Window window{context, true};
  KeyboardInputDriver driver{&window, 0};
  ui::ImGuiDrawer drawer{&window, 10};
  NativeTextFixture() {
    REQUIRE(window.Open());
    window.FocusForTest(true);
    window.AddInputListener(&drawer, 10);
    auto& io = drawer.GetIO();
    io.DisplaySize = ImVec2(1280, 720);
    io.DeltaTime = 1.0f / 60;
    io.ConfigInputTrickleEventQueue = false;
    unsigned char* pixels;
    int width, height;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    NativeKeystrokeRoutingTestAccess::BindA(driver);
    driver.SetNativeTextGeneration(81);
  }
  ~NativeTextFixture() { window.RemoveInputListener(&drawer); }
  void Enable(uint64_t revision = 1, bool enabled = true) {
    REQUIRE(driver.SetNativeTextOwner(81, revision, 1, enabled));
  }
};
}  // namespace

TEST_CASE(
    "Idle ImGui does not starve complete native BMP commits or duplicate chars",
    "[ui][input][native_text]") {
  NativeTextFixture f;
  f.Enable();
  f.drawer.GetIO().WantTextInput = false;
  f.window.KeyForTest(ui::VirtualKey::kA, true, 101);
  f.window.LegacyCharForTest(0x03A9);
  f.window.CommitForTest(U"\u03A9", 101);
  X_INPUT_KEYSTROKE out{};
  REQUIRE(f.driver.GetNativeTextKeystroke(81, 1, &out) == X_ERROR_SUCCESS);
  CHECK(uint16_t(out.virtual_key) == 'A');
  CHECK(uint16_t(out.unicode) == 0x03A9);
  CHECK(out.hid_code == 4);
  CHECK(out.user_index == 1);
  ImGui::NewFrame();
  CHECK(f.drawer.GetIO().InputQueueCharacters.Size == 1);
  if (f.drawer.GetIO().InputQueueCharacters.Size) {
    CHECK(f.drawer.GetIO().InputQueueCharacters[0] == 0x03A9);
  }
  ImGui::EndFrame();
  f.window.KeyForTest(ui::VirtualKey::kA, false, 101);
  REQUIRE(f.driver.GetNativeTextKeystroke(81, 1, &out) == X_ERROR_SUCCESS);
  CHECK(uint16_t(out.flags) == X_INPUT_KEYSTROKE_KEYUP);
}

TEST_CASE(
    "Native text claims quarantine mapped gamepad keys before delivery and "
    "after close",
    "[ui][input][native_text]") {
  NativeTextFixture f;
  f.window.KeyForTest(ui::VirtualKey::kA, true, 1);
  CHECK(NativeKeystrokeRoutingTestAccess::RawAIsHeld(f.driver));
  f.Enable(1, false);
  CHECK_FALSE(NativeKeystrokeRoutingTestAccess::RawAIsHeld(f.driver));
  X_INPUT_STATE state{};
  REQUIRE(f.driver.GetState(0, &state) == X_ERROR_SUCCESS);
  CHECK(uint16_t(state.gamepad.buttons) == 0);
  f.window.KeyForTest(ui::VirtualKey::kA, true, 1, true);
  f.Enable(1, true);
  f.window.CommitForTest(U"a", 1, true);
  X_INPUT_KEYSTROKE out{};
  CHECK(f.driver.GetNativeTextKeystroke(81, 1, &out) == X_ERROR_EMPTY);
  f.window.KeyForTest(ui::VirtualKey::kA, false, 1);
  f.window.KeyForTest(ui::VirtualKey::kA, true, 2);
  f.window.CommitForTest(U"a", 2);
  REQUIRE(f.driver.GetNativeTextKeystroke(81, 1, &out) == X_ERROR_SUCCESS);
  REQUIRE(f.driver.SetNativeTextOwner(81, 0, UINT32_MAX, false));
  f.window.KeyForTest(ui::VirtualKey::kA, true, 2, true);
  CHECK_FALSE(NativeKeystrokeRoutingTestAccess::RawAIsHeld(f.driver));
  f.window.KeyForTest(ui::VirtualKey::kA, false, 2);
  f.window.KeyForTest(ui::VirtualKey::kA, true, 3);
  CHECK(NativeKeystrokeRoutingTestAccess::RawAIsHeld(f.driver));
}

TEST_CASE(
    "Actual Window focus and host text priority reject stale native commits",
    "[ui][input][native_text]") {
  NativeTextFixture f;
  f.Enable();
  f.window.KeyForTest(ui::VirtualKey::kA, true, 1);
  const auto epoch = f.driver.GetNativeTextState(81).invalidation_epoch;
  f.window.FocusForTest(false);
  CHECK(f.driver.GetNativeTextState(81).invalidation_epoch > epoch);
  f.window.FocusForTest(true);
  f.window.CommitForTest(U"a", 1);
  X_INPUT_KEYSTROKE out{};
  CHECK(f.driver.GetNativeTextKeystroke(81, 1, &out) == X_ERROR_EMPTY);
  f.window.KeyForTest(ui::VirtualKey::kA, false, 1);
  f.window.KeyForTest(ui::VirtualKey::kA, true, 2);
  f.drawer.GetIO().WantTextInput = true;
  f.window.CommitForTest(U"a", 2);
  CHECK(f.driver.GetNativeTextKeystroke(81, 1, &out) == X_ERROR_EMPTY);
  f.window.KeyForTest(ui::VirtualKey::kA, false, 2);
  f.drawer.GetIO().WantTextInput = false;
  f.window.KeyForTest(ui::VirtualKey::kA, true, 3);
  f.window.CommitForTest(U"a", 3);
  REQUIRE(f.driver.GetNativeTextKeystroke(81, 1, &out) == X_ERROR_SUCCESS);
  CHECK(uint16_t(out.unicode) == 'a');
}

TEST_CASE("Windows VK modifier values produce the explicit USB keyboard usages",
          "[ui][input][native_text]") {
  NativeTextFixture f;
  f.Enable();
  struct Row {
    uint16_t vk;
    uint8_t hid;
  };
  constexpr Row rows[] = {{0xA0, 0xE1}, {0xA1, 0xE5}, {0xA2, 0xE0},
                          {0xA3, 0xE4}, {0xA4, 0xE2}, {0xA5, 0xE6},
                          {0x5B, 0xE3}, {0x5C, 0xE7}, {0xA6, 0},
                          {0xA7, 0},    {0xA8, 0},    {0xA9, 0}};
  uint64_t id = 1;
  for (const auto row : rows) {
    const auto vk = static_cast<ui::VirtualKey>(row.vk);
    f.window.KeyForTest(vk, true, id++);
    X_INPUT_KEYSTROKE out{};
    REQUIRE(f.driver.GetNativeTextKeystroke(81, 1, &out) == X_ERROR_SUCCESS);
    CHECK(uint16_t(out.virtual_key) == row.vk);
    CHECK(out.hid_code == row.hid);
    f.window.KeyForTest(vk, false, id++);
    REQUIRE(f.driver.GetNativeTextKeystroke(81, 1, &out) == X_ERROR_SUCCESS);
    CHECK(out.hid_code == row.hid);
  }
}

TEST_CASE(
    "InputSystem forwards exact native text leases and modal invalidation to "
    "real keyboard",
    "[ui][input][native_text]") {
  Context context;
  Window window(context, true);
  REQUIRE(window.Open());
  window.FocusForTest(true);
  InputSystem input(&window);
  input.AddDriver(std::make_unique<KeyboardInputDriver>(&window, 0));
  {
    auto lock = input.lock();
    REQUIRE(input.BeginNativeInputTransport(91, {}, [] {}));
    REQUIRE(input.HasNativeTextInput(91));
    REQUIRE(input.SetNativeTextOwner(91, 1, 1, true));
  }
  window.KeyForTest(ui::VirtualKey::kA, true, 1);
  window.CommitForTest(U"a", 1);
  X_INPUT_KEYSTROKE out{};
  {
    auto lock = input.lock();
    REQUIRE(input.GetNativeTextKeystroke(91, 1, &out) == X_ERROR_SUCCESS);
    CHECK(uint16_t(out.unicode) == 'a');
    CHECK(out.user_index == 1);
    const auto epoch = input.GetNativeTextState(91).invalidation_epoch;
    input.AddUIInputBlocker();
    CHECK_FALSE(input.GetNativeTextState(91).enabled);
    CHECK(input.GetNativeTextState(91).invalidation_epoch > epoch);
  }
  window.KeyForTest(ui::VirtualKey::kA, false, 1);
  window.KeyForTest(ui::VirtualKey::kB, true, 2);
  window.CommitForTest(U"b", 2);
  {
    auto lock = input.lock();
    CHECK(input.GetNativeTextKeystroke(91, 1, &out) ==
          X_ERROR_DEVICE_NOT_CONNECTED);
    input.RemoveUIInputBlocker();
    CHECK(input.GetNativeTextKeystroke(91, 1, &out) == X_ERROR_EMPTY);
    CHECK_FALSE(input.SetNativeTextOwner(90, 9, 1, true));
  }
  window.KeyForTest(ui::VirtualKey::kB, false, 2);
  window.KeyForTest(ui::VirtualKey::kB, true, 3);
  window.CommitForTest(U"b", 3);
  {
    auto lock = input.lock();
    REQUIRE(input.GetNativeTextKeystroke(91, 1, &out) == X_ERROR_SUCCESS);
    CHECK(uint16_t(out.unicode) == 'b');
  }
  std::shared_ptr<NativeDeviceNotificationRundown> stopped;
  {
    auto lock = input.lock();
    stopped = input.StopNativeDeviceNotifications(91);
    input.EndNativeInputTransport(91);
    CHECK_FALSE(input.GetNativeTextState(91).available);
  }
  REQUIRE(stopped);
  CHECK(stopped->Drain());
}

TEST_CASE("Focus regain admits the first fresh key released outside the window",
          "[ui][input][native_text]") {
  NativeTextFixture f;
  f.Enable();
  f.window.KeyForTest(ui::VirtualKey::kA, true, 1);
  f.window.CommitForTest(U"a", 1);
  X_INPUT_KEYSTROKE out{};
  REQUIRE(f.driver.GetNativeTextKeystroke(81, 1, &out) == X_ERROR_SUCCESS);
  f.window.FocusForTest(false);
  // Actual source changes while no Window KeyUp callback is delivered.
  f.window.ReleaseOutsideFocusForTest(ui::VirtualKey::kA);
  const auto queries = f.window.physical_queries();
  f.window.FocusForTest(true);
  CHECK(f.window.physical_queries() > queries);
  f.window.KeyForTest(ui::VirtualKey::kA, true, 2);
  f.window.CommitForTest(U"a", 2);
  REQUIRE(f.driver.GetNativeTextKeystroke(81, 1, &out) == X_ERROR_SUCCESS);
  CHECK(uint16_t(out.unicode) == 'a');
}

TEST_CASE(
    "Focus regain keeps truly held physical keys suppressed through release",
    "[ui][input][native_text]") {
  NativeTextFixture f;
  f.Enable();
  f.window.KeyForTest(ui::VirtualKey::kA, true, 1);
  f.window.CommitForTest(U"a", 1);
  f.window.FocusForTest(false);
  f.window.FocusForTest(true);  // The physical source still reports pressed.
  f.window.KeyForTest(ui::VirtualKey::kA, true, 2, true);
  f.window.CommitForTest(U"a", 2, true);
  X_INPUT_KEYSTROKE out{};
  CHECK(f.driver.GetNativeTextKeystroke(81, 1, &out) == X_ERROR_EMPTY);
  f.window.KeyForTest(ui::VirtualKey::kA, false, 2);
  f.window.KeyForTest(ui::VirtualKey::kA, true, 3);
  f.window.CommitForTest(U"a", 3);
  REQUIRE(f.driver.GetNativeTextKeystroke(81, 1, &out) == X_ERROR_SUCCESS);
}

TEST_CASE(
    "Keyboard text starts focused when attached after the window gained focus",
    "[ui][input][native_text]") {
  Context context;
  Window window(context, true);
  REQUIRE(window.Open());
  window.FocusForTest(true);
  KeyboardInputDriver driver(&window, 0);
  driver.SetNativeTextGeneration(93);
  REQUIRE(driver.SetNativeTextOwner(93, 1, 1, true));
  CHECK(driver.GetNativeTextState(93).focused);
  CHECK(driver.GetNativeTextState(93).enabled);
  window.KeyForTest(ui::VirtualKey::kA, true, 1);
  window.CommitForTest(U"a", 1);
  X_INPUT_KEYSTROKE out{};
  REQUIRE(driver.GetNativeTextKeystroke(93, 1, &out) == X_ERROR_SUCCESS);
}

TEST_CASE("Physical key tracker preserves all sources of one logical VK",
          "[ui][input][native_text]") {
  ui::PhysicalKeyTracker tracker;
  const auto enter = static_cast<ui::VirtualKey>(0x0D);
  const auto main = tracker.Down(36, enter);
  const auto pad = tracker.Down(104, enter);
  REQUIRE(main.deliver);
  REQUIRE(pad.deliver);
  CHECK_FALSE(main.physical_repeat);
  CHECK_FALSE(main.logical_repeat);
  CHECK_FALSE(pad.physical_repeat);
  CHECK(pad.logical_repeat);
  CHECK(main.id != pad.id);
  ui::PhysicalKeyTracker::Bitmap keys{};
  keys[36 >> 3] |= 1u << (36 & 7);
  // The most recently encountered source104 is released, but36 is still held.
  CHECK(tracker.Classify(enter, keys) ==
        ui::PhysicalKeyTracker::State::kPressed);
  CHECK_FALSE(tracker.Up(36).deliver);
  CHECK(tracker.IsHeld(enter));
  const auto last = tracker.Up(104);
  CHECK(last.deliver);
  CHECK(last.key == enter);
  CHECK_FALSE(tracker.IsHeld(enter));
}

TEST_CASE("Held hardware identity survives VK changes and focus reconciliation",
          "[ui][input][native_text]") {
  ui::PhysicalKeyTracker tracker;
  const auto original = tracker.Down(38, ui::VirtualKey::kA);
  ui::PhysicalKeyTracker::Bitmap pressed{};
  pressed[38 >> 3] |= 1u << (38 & 7);
  tracker.Reconcile(pressed);
  const auto repeated = tracker.Down(38, ui::VirtualKey::kB);
  CHECK(repeated.physical_repeat);
  CHECK(repeated.logical_repeat);
  CHECK(repeated.key == ui::VirtualKey::kA);
  CHECK(repeated.id == original.id);
  const auto release = tracker.Up(38);
  CHECK(release.deliver);
  CHECK(release.key == ui::VirtualKey::kA);
  const auto fresh = tracker.Down(38, ui::VirtualKey::kB);
  CHECK_FALSE(fresh.physical_repeat);
  CHECK_FALSE(fresh.logical_repeat);
  CHECK(fresh.key == ui::VirtualKey::kB);
  CHECK(fresh.id != original.id);
  CHECK(tracker.Classify(ui::VirtualKey::kA, pressed) ==
        ui::PhysicalKeyTracker::State::kReleased);
  CHECK(tracker.Classify(ui::VirtualKey::kB, pressed) ==
        ui::PhysicalKeyTracker::State::kPressed);
  tracker.Reconcile(
      {});  // Physical release happened while the window was unfocused.
  const auto recovered = tracker.Down(38, ui::VirtualKey::kA);
  CHECK_FALSE(recovered.logical_repeat);
  CHECK(recovered.id != fresh.id);
  CHECK_FALSE(tracker.Down(0, ui::VirtualKey::kA).deliver);
  CHECK_FALSE(tracker.Down(256, ui::VirtualKey::kA).deliver);
}

TEST_CASE("Same-VK hardware sources emit one native logical release",
          "[ui][input][native_text]") {
  NativeTextFixture f;
  f.Enable();
  ui::PhysicalKeyTracker tracker;
  const auto first = tracker.Down(38, ui::VirtualKey::kA);
  f.window.KeyForTest(first.key, true, first.id, first.logical_repeat);
  f.window.CommitForTest(U"a", first.id, first.logical_repeat);
  X_INPUT_KEYSTROKE out{};
  REQUIRE(f.driver.GetNativeTextKeystroke(81, 1, &out) == X_ERROR_SUCCESS);
  const auto second = tracker.Down(52, ui::VirtualKey::kA);
  f.window.KeyForTest(second.key, true, second.id, second.logical_repeat);
  f.window.CommitForTest(U"a", second.id, second.logical_repeat);
  const auto early = tracker.Up(38);
  CHECK_FALSE(early.deliver);
  CHECK(f.driver.GetNativeTextKeystroke(81, 1, &out) == X_ERROR_EMPTY);
  const auto final = tracker.Up(52);
  REQUIRE(final.deliver);
  f.window.KeyForTest(final.key, false, final.id);
  REQUIRE(f.driver.GetNativeTextKeystroke(81, 1, &out) == X_ERROR_SUCCESS);
  CHECK(uint16_t(out.flags) == X_INPUT_KEYSTROKE_KEYUP);
  CHECK(f.driver.GetNativeTextKeystroke(81, 1, &out) == X_ERROR_EMPTY);
}

TEST_CASE(
    "A physical key first observed held on focus regain cannot become a fresh "
    "press",
    "[ui][input][native_text]") {
  ui::PhysicalKeyTracker tracker;
  ui::PhysicalKeyTracker::Bitmap pressed{};
  pressed[52 >> 3] |= 1u << (52 & 7);
  tracker.Reconcile(pressed);
  const auto held = tracker.Down(52, ui::VirtualKey::kA);
  CHECK(held.physical_repeat);
  CHECK(held.logical_repeat);
  CHECK(held.key == ui::VirtualKey::kA);
  REQUIRE(tracker.Up(52).deliver);
  const auto fresh = tracker.Down(52, ui::VirtualKey::kA);
  CHECK_FALSE(fresh.physical_repeat);
  CHECK_FALSE(fresh.logical_repeat);
  CHECK(fresh.id != held.id);
}

}  // namespace xe::hid::keyboard::test
