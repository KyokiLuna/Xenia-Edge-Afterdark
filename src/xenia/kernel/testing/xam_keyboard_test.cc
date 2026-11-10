/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <algorithm>
#include <array>

#include "third_party/catch/include/catch.hpp"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xam/xam_keyboard.h"
#include "xenia/kernel/xam/xam_module.h"
#include "xenia/kernel/xam/xam_private.h"

namespace xe::kernel::xam::test {
namespace {
constexpr uint16_t kCanary = 0xA75B;

TEST_CASE(
    "Keyboard ABI does not read output while rejecting an overflowing capacity",
    "[kernel][xam][keyboard]") {
  Memory memory;
  REQUIRE(memory.Initialize());
  cpu::ExportResolver resolver;
  RegisterUIExports(&resolver, nullptr);
  XamModule::RegisterExportTable(&resolver);
  auto* entry = resolver.GetExportByOrdinal("xam.xex", 0x2C1);
  REQUIRE(entry != nullptr);
  CHECK(std::string_view(entry->name) == "XamShowKeyboardUI");
  REQUIRE(entry->function_data.trampoline != nullptr);
  PPCContext context{};
  context.virtual_membase = memory.virtual_membase();
  // All inputs are NULL. The output is deliberately unmapped, but it is an
  // output address, not a string to read during ABI argument decoding/tracing.
  context.r[8] = 0xFFFFFFFF;
  context.r[9] = 0xFFFFFFFF;
  entry->function_data.trampoline(&context);
  CHECK(uint32_t(context.r[3]) == X_ERROR_INVALID_PARAMETER);
}

TEST_CASE("Keyboard output reserves one UTF-16 unit for NUL",
          "[kernel][xam][keyboard]") {
  std::array<xe::be<uint16_t>, 8> buffer;
  std::fill(buffer.begin(), buffer.end(), kCanary);
  CHECK_FALSE(WriteKeyboardText(u"", {}));
  CHECK_FALSE(WriteKeyboardText(u"x", {buffer.data() + 1, 1}));
  CHECK(std::all_of(buffer.begin(), buffer.end(),
                    [](auto unit) { return unit == kCanary; }));
  REQUIRE(WriteKeyboardText(u"", {buffer.data() + 1, 1}));
  CHECK(buffer[0] == kCanary);
  CHECK(buffer[1] == 0);
  CHECK(buffer[2] == kCanary);
}

TEST_CASE("Keyboard accepted output is big-endian and leaves canaries intact",
          "[kernel][xam][keyboard]") {
  std::array<xe::be<uint16_t>, 8> buffer;
  std::fill(buffer.begin(), buffer.end(), kCanary);
  const std::u16string text = u"\u65E5\U0001F642";
  REQUIRE(text.size() == 3);
  REQUIRE(WriteKeyboardText(text, {buffer.data() + 1, 4}));
  CHECK(buffer[0] == kCanary);
  CHECK(buffer[1] == 0x65E5);
  CHECK(buffer[2] == 0xD83D);
  CHECK(buffer[3] == 0xDE42);
  CHECK(buffer[4] == 0);
  CHECK(buffer[5] == kCanary);
  const auto* bytes = reinterpret_cast<const uint8_t*>(buffer.data() + 1);
  CHECK(bytes[0] == 0x65);
  CHECK(bytes[1] == 0xE5);
  CHECK(bytes[2] == 0xD8);
  CHECK(bytes[3] == 0x3D);
  CHECK(bytes[4] == 0xDE);
  CHECK(bytes[5] == 0x42);
  CHECK(bytes[6] == 0);
  CHECK(bytes[7] == 0);
}

TEST_CASE("Overlong keyboard output never partly overwrites caller storage",
          "[kernel][xam][keyboard]") {
  std::array<xe::be<uint16_t>, 6> buffer;
  std::fill(buffer.begin(), buffer.end(), kCanary);
  CHECK_FALSE(WriteKeyboardText(u"a\U0001F642", {buffer.data() + 1, 3}));
  CHECK(std::all_of(buffer.begin(), buffer.end(),
                    [](auto unit) { return unit == kCanary; }));
}

TEST_CASE(
    "Keyboard cancellation reports both native result fields without copyback",
    "[kernel][xam][keyboard]") {
  std::array<xe::be<uint16_t>, 6> buffer;
  std::fill(buffer.begin(), buffer.end(), kCanary);
  const auto result = CompleteKeyboardText(xe::ui::TextEntryResult::kAborted,
                                           u"ignored", buffer);
  CHECK(result.result == X_ERROR_CANCELLED);
  CHECK(result.extended_error == X_ERROR_CANCELLED);
  CHECK(result.length == 0);
  CHECK(std::all_of(buffer.begin(), buffer.end(),
                    [](auto unit) { return unit == kCanary; }));
  const auto empty =
      CompleteKeyboardText(xe::ui::TextEntryResult::kAborted, u"ignored", {});
  CHECK(empty.result == X_ERROR_CANCELLED);
}

TEST_CASE("Keyboard successful completion has zero transferred count",
          "[kernel][xam][keyboard]") {
  std::array<xe::be<uint16_t>, 6> buffer;
  std::fill(buffer.begin(), buffer.end(), kCanary);
  const auto result = CompleteKeyboardText(xe::ui::TextEntryResult::kAccepted,
                                           u"\u65E5\U0001F642", buffer);
  CHECK(result.result == X_ERROR_SUCCESS);
  CHECK(result.extended_error == X_ERROR_SUCCESS);
  CHECK(result.length == 0);
  CHECK(buffer[0] == 0x65E5);
  CHECK(buffer[1] == 0xD83D);
  CHECK(buffer[2] == 0xDE42);
  CHECK(buffer[3] == 0);
  CHECK(buffer[4] == kCanary);
}
TEST_CASE("Keyboard scene cancellation preserves native extended status",
          "[kernel][xam][keyboard]") {
  std::array<xe::be<uint16_t>, 3> buffer;
  std::fill(buffer.begin(), buffer.end(), kCanary);
  const auto result = CompleteKeyboardText(xe::ui::TextEntryResult::kCanceled,
                                           u"ignored", buffer);
  CHECK(result.result == X_ERROR_FUNCTION_FAILED);
  CHECK(result.extended_error == X_ERROR_CANCELLED);
  CHECK(result.length == 0);
  CHECK(std::all_of(buffer.begin(), buffer.end(),
                    [](auto unit) { return unit == kCanary; }));
}
TEST_CASE("Zero-capacity keyboard acceptance performs no copyback",
          "[kernel][xam][keyboard]") {
  const auto result = CompleteKeyboardText(xe::ui::TextEntryResult::kAccepted,
                                           u"\u65E5\U0001F642", {});
  CHECK(result.result == X_ERROR_SUCCESS);
  CHECK(result.extended_error == X_ERROR_SUCCESS);
  CHECK(result.length == 0);
}
}  // namespace
}  // namespace xe::kernel::xam::test
