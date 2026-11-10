/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia contributors. All rights reserved.                    *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */
#include <array>
#include <vector>

#include "third_party/catch/include/catch.hpp"
#include "xenia/kernel/xam/xam_launch_contract.h"
#include "xenia/kernel/xam/xam_module.h"
#include "xenia/kernel/xam/xam_private.h"

namespace xe::kernel::xam::test {

TEST_CASE("Native HUD reboot imports resolve to real implemented exports",
          "[kernel][xam][launch]") {
  cpu::ExportResolver resolver;
  RegisterInfoExports(&resolver, nullptr);
  XamModule::RegisterExportTable(&resolver);
  for (const auto ordinal : {0x838, 0x839}) {
    const auto* entry = resolver.GetExportByOrdinal("xam.xex", ordinal);
    REQUIRE(entry);
    CHECK(entry->get_type() == cpu::Export::Type::kFunction);
    CHECK(entry->is_implemented());
    CHECK((entry->tags & cpu::ExportTag::kStub) == 0);
    CHECK(entry->function_data.trampoline != nullptr);
  }
}

TEST_CASE("Dashboard reboot owns the full native record and original user",
          "[kernel][xam][launch]") {
  std::array<uint8_t, DashboardRebootRequest::kBytes> source;
  for (size_t i = 0; i < source.size(); ++i) {
    source[i] = uint8_t(i * 37);
  }
  xe::store_and_swap<uint32_t>(&source[4], 0x16);
  xe::store_and_swap<uint32_t>(&source[8], 3);
  const auto original = source;
  auto request = DashboardRebootRequest::Copy(source);
  REQUIRE(request);
  source.fill(0xCD);  // The caller's storage may disappear after dispatch.
  CHECK(std::equal(request->bytes().begin(), request->bytes().end(),
                   original.begin()));
  CHECK(request->reason() == 0x16);
  CHECK(request->user_index() == 3);
  CHECK(request->notification_value() == 0x00030016);
  CHECK(request->launch_flags(0) == 0);
  CHECK_FALSE(DashboardRebootRequest::Copy({}));
  CHECK_FALSE(DashboardRebootRequest::Copy(
      std::span<const uint8_t>(source).first(source.size() - 1)));
}

TEST_CASE("Native dashboard navigation distinguishes notify from restart",
          "[kernel][xam][launch]") {
  std::array<uint8_t, DashboardRebootRequest::kBytes> source{};
  xe::store_and_swap<uint32_t>(&source[4], 0x48);
  auto request = DashboardRebootRequest::Copy(source);
  REQUIRE(request);
  CHECK(request->notify_running_dashboard(true, 0, 0));
  CHECK_FALSE(request->notify_running_dashboard(false, 0, 0));
  CHECK_FALSE(request->notify_running_dashboard(true, 5, 0));
  CHECK(request->notify_running_dashboard(true, 6, 0x7F));
  CHECK_FALSE(request->notify_running_dashboard(true, 0, 0x80));
  CHECK(request->launch_flags(0) == 0x100000);
  CHECK(request->launch_flags(0x7F) == 0x100000);
  CHECK(request->launch_flags(0x80) == 0x1000);
}

TEST_CASE("Native reboot confirmation respects privilege and dash context",
          "[kernel][xam][launch]") {
  CHECK(ShouldConfirmDashboardReboot(false, 0, 0));
  CHECK(ShouldConfirmDashboardReboot(false, 1u << 17, 0));
  CHECK_FALSE(ShouldConfirmDashboardReboot(false, 1u << 18, 0));
  CHECK_FALSE(ShouldConfirmDashboardReboot(true, 0, 0));
  CHECK_FALSE(ShouldConfirmDashboardReboot(true, 1u << 18, 5));
  CHECK(ShouldConfirmDashboardReboot(true, 0, 6));
  CHECK(ShouldConfirmDashboardReboot(false, 1u << 18, 6));
  CHECK(ShouldConfirmDashboardReboot(true, 1u << 18, 6));
}

TEST_CASE("Spawn launch transport preserves every byte including zeros",
          "[kernel][xam][launch]") {
  std::vector<uint8_t> bytes(DashboardRebootRequest::kBytes);
  for (size_t i = 0; i < bytes.size(); ++i) {
    bytes[i] = uint8_t(i);
  }
  const auto encoded = EncodeLaunchData(bytes);
  REQUIRE(encoded.size() == bytes.size() * 2);
  CHECK(encoded.substr(0, 8) == "00010203");
  CHECK(encoded.substr(0xFE * 2, 8) == "feff0001");
  std::vector<uint8_t> decoded;
  for (size_t i = 0; i < bytes.size(); ++i) {
    decoded.push_back(
        uint8_t(std::stoul(encoded.substr(i * 2, 2), nullptr, 16)));
  }
  CHECK(decoded == bytes);
  CHECK(EncodeLaunchData({}).empty());
}

}  // namespace xe::kernel::xam::test
