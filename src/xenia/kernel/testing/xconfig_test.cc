/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <array>
#include <cstdint>
#include <cstring>

#include "xenia/base/byte_order.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_xconfig.h"
#include "xenia/kernel/xconfig.h"
#include "xenia/memory.h"

#include "third_party/catch/include/catch.hpp"

namespace {

using xe::X_STATUS;
using xe::kernel::ConsoleMacAddress;

TEST_CASE("Console MAC address", "[xconfig]") {
  const auto mac = ConsoleMacAddress("E000000000000001");

  SECTION("It is in a console vendor's block") {
    REQUIRE(mac[0] == 0x00);
    REQUIRE(mac[1] == 0x1D);
    REQUIRE(mac[2] == 0xD8);
  }

  SECTION("A console keeps its address") {
    // Titles encrypt saves with it, so a new derivation would lose them.
    REQUIRE(mac == std::array<uint8_t, 6>{0x00, 0x1D, 0xD8, 0x78, 0x02, 0xF7});
  }

  SECTION("Another profile or machine is another console") {
    REQUIRE(ConsoleMacAddress("E000000000000002") != mac);
    REQUIRE(ConsoleMacAddress("machine") != mac);
  }
}

TEST_CASE("Unsupported XConfig reads fail without fabricating a value",
          "[xconfig][unsupported]") {
  using namespace xe::kernel;
  XConfig config;
  std::array<uint8_t, 4> output;
  output.fill(0xA5);
  const auto original = output;
  CHECK(config.GetSettingSize(XCONFIG_CONSOLE_CATEGORY, 0xA) == 0);
  CHECK(config.ReadSettingChecked(XCONFIG_CONSOLE_CATEGORY, 0xA, output) ==
        X_STATUS_NOT_SUPPORTED);
  CHECK(output == original);
  CHECK(config.ReadSettingChecked(XCONFIG_USER_CATEGORY, 0xFFFF, {}) ==
        X_STATUS_NOT_SUPPORTED);
  CHECK(config.ReadSettingChecked(XCONFIG_USER_CATEGORY, XCONFIG_USER_LANGUAGE,
                                  output) == X_STATUS_SUCCESS);
}

TEST_CASE("Unsupported guest XConfig publishes zero size and preserves payload",
          "[xconfig][unsupported][memory]") {
  using namespace xe::kernel;
  XConfig config;
  xe::Memory memory;
  REQUIRE(memory.Initialize());
  constexpr uint32_t base = 0x90030000;
  constexpr uint32_t required = base + 4, output = base + 32;
  REQUIRE(memory.LookupHeap(base)->AllocFixed(
      base, 0x1000, 0x1000,
      xe::kMemoryAllocationReserve | xe::kMemoryAllocationCommit,
      xe::kMemoryProtectRead | xe::kMemoryProtectWrite));
  std::memset(memory.TranslateVirtual(base), 0xA5, 0x1000);

  // Native 17559's one-byte 7/A read must take its negative-status branch,
  // rather than reading a caller stack byte left behind by success/size zero.
  CHECK(xboxkrnl::ReadXConfigGuest(config, memory, 7, 0xA, output, 1,
                                   required) == X_STATUS_NOT_SUPPORTED);
  CHECK(memory.TranslateVirtual(output)[0] == 0xA5);
  CHECK(xe::load_and_swap<uint16_t>(memory.TranslateVirtual(required)) == 0);
  CHECK(memory.TranslateVirtual(required - 1)[0] == 0xA5);
  CHECK(memory.TranslateVirtual(required + 2)[0] == 0xA5);

  xe::store_and_swap<uint16_t>(memory.TranslateVirtual(required), 0xFFFF);
  CHECK(xboxkrnl::ReadXConfigGuest(config, memory, 7, 0xA, 0, 0, required) ==
        X_STATUS_NOT_SUPPORTED);
  CHECK(xe::load_and_swap<uint16_t>(memory.TranslateVirtual(required)) == 0);
  CHECK(xboxkrnl::ReadXConfigGuest(config, memory, 7, 0xA, output, 1, 0) ==
        X_STATUS_NOT_SUPPORTED);
  CHECK(memory.TranslateVirtual(output)[0] == 0xA5);

  xe::store_and_swap<uint16_t>(memory.TranslateVirtual(required), 0xA5A5);
  REQUIRE(
      memory.LookupHeap(base)->Protect(base, 0x1000, xe::kMemoryProtectRead));
  CHECK(xboxkrnl::ReadXConfigGuest(config, memory, 7, 0xA, 0, 0, required) ==
        X_STATUS_ACCESS_VIOLATION);
  CHECK(xe::load_and_swap<uint16_t>(memory.TranslateVirtual(required)) ==
        0xA5A5);
  CHECK(memory.TranslateVirtual(output)[0] == 0xA5);
}

} // namespace
