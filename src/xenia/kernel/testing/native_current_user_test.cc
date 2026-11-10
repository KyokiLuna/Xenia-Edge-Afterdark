/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia contributors. All rights reserved.                    *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */
#include <array>
#include <string_view>
#include "third_party/catch/include/catch.hpp"
#include "xenia/kernel/native_current_user_presets.h"
#include "xenia/kernel/native_current_user_provider.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xam/xam_module.h"
#include "xenia/kernel/xam/xam_private.h"
#include "xenia/memory.h"

namespace xe::kernel::test {
TEST_CASE("Current user presets preserve all bits independently in four slots",
          "[kernel][native_current_user][ownership]") {
  NativeCurrentUserPresets first, second;
  const std::array<uint32_t, 4> states{0x00020000, 0x800F0001, 0xFFFFFFFF,
                                       0xA5100200};
  for (uint32_t slot = 0; slot < states.size(); ++slot) {
    REQUIRE(first.Get(slot));
    CHECK(*first.Get(slot) == 0);
    REQUIRE(first.Set(slot, states[slot]));
    CHECK(*second.Get(slot) == 0);
  }
  for (uint32_t slot = 0; slot < states.size(); ++slot) {
    CHECK(*first.Get(slot) == states[slot]);
  }
  for (uint32_t invalid : {4u, 0xFFu, UINT32_MAX}) {
    CHECK_FALSE(first.Set(invalid, 0));
    CHECK_FALSE(first.Get(invalid));
  }
  for (uint32_t slot = 0; slot < states.size(); ++slot) {
    CHECK(*first.Get(slot) == states[slot]);
  }
}

TEST_CASE("Stopped preset ownership cannot revive or mutate another store",
          "[kernel][native_current_user][ownership]") {
  NativeCurrentUserPresets retired, live;
  REQUIRE(retired.Set(3, 0xFFFFFFFF));
  REQUIRE(live.Set(3, 0x20000));
  retired.Stop();
  retired.Stop();
  for (uint32_t slot = 0; slot < 4; ++slot) {
    CHECK_FALSE(retired.Get(slot));
    CHECK_FALSE(retired.Set(slot, 0));
  }
  CHECK(*live.Get(3) == 0x20000);
  CHECK_FALSE(CurrentUserPresetsForKernel(nullptr));
  std::string error;
  CHECK_FALSE(NativeCurrentUserProviderBinding::InstallBeforeHeapAndWorkers(
      nullptr, &error));
  CHECK_FALSE(error.empty());
}

TEST_CASE("Preset ordinals use DWORD registers and a genuine void setter",
          "[kernel][native_current_user][abi]") {
  Memory memory;
  REQUIRE(memory.Initialize());
  cpu::ExportResolver resolver;
  xam::RegisterNotifyExports(&resolver, nullptr);
  xam::XamModule::RegisterExportTable(&resolver);
  const auto* setter = resolver.GetExportByOrdinal("xam.xex", 0x214);
  const auto* getter = resolver.GetExportByOrdinal("xam.xex", 0x215);
  for (const auto* entry : {setter, getter}) {
    REQUIRE(entry);
    REQUIRE(entry->is_implemented());
    REQUIRE_FALSE(entry->tags & cpu::ExportTag::kStub);
    REQUIRE(entry->function_data.trampoline);
  }
  CHECK(std::string_view(setter->name) == "XamSetUserPresetPresenceState");
  CHECK(std::string_view(getter->name) == "XamGetUserPresetPresenceState");
  // Invalid indices terminate before shared Kernel access. Poisoned unused
  // registers are data, never hidden output pointers or an extra qword half.
  for (uint32_t slot : {4u, 0xFFu, UINT32_MAX}) {
    PPCContext context{};
    context.virtual_membase = memory.virtual_membase();
    context.r[3] = 0xFEDCBA9800000000ull | slot;
    context.r[4] = 0x12345678A5F20001ull;
    context.r[5] = UINT64_MAX;
    const uint64_t original_slot = context.r[3];
    setter->function_data.trampoline(&context);
    CHECK(context.r[3] == original_slot);
    CHECK(context.r[4] == 0x12345678A5F20001ull);
    CHECK(context.r[5] == UINT64_MAX);
    getter->function_data.trampoline(&context);
    CHECK(context.r[3] == 0);
    CHECK(context.r[4] == 0x12345678A5F20001ull);
  }
}
}  // namespace xe::kernel::test
