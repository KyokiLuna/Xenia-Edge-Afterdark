/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia contributors. All rights reserved.                    *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */
#ifndef XENIA_KERNEL_XAM_XAM_LAUNCH_CONTRACT_H_
#define XENIA_KERNEL_XAM_XAM_LAUNCH_CONTRACT_H_

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>

#include "xenia/base/memory.h"

namespace xe::kernel::xam {

// The retail 17559 HUD passes this complete record to XamLoaderRebootToDash.
// Keep an owned copy: neither a guest pointer nor its originating kernel may
// survive the asynchronous title transition.
class DashboardRebootRequest {
 public:
  static constexpr size_t kBytes = 0x3FC;
  static std::optional<DashboardRebootRequest> Copy(
      std::span<const uint8_t> source) {
    if (source.size() != kBytes) {
      return std::nullopt;
    }
    DashboardRebootRequest request;
    std::copy(source.begin(), source.end(), request.bytes_.begin());
    return request;
  }

  std::span<const uint8_t> bytes() const { return bytes_; }
  uint32_t reason() const { return xe::load_and_swap<uint32_t>(&bytes_[4]); }
  uint32_t user_index() const {
    return xe::load_and_swap<uint32_t>(&bytes_[8]);
  }
  uint32_t notification_value() const {
    return (user_index() << 16) | reason();
  }
  uint32_t launch_flags(uint8_t right_trigger) const {
    return right_trigger >= 0x80 ? 0x1000 : reason() == 0x48 ? 0x100000 : 0;
  }
  bool notify_running_dashboard(bool is_dashboard, uint32_t dashboard_context,
                                uint8_t right_trigger) const {
    return is_dashboard && dashboard_context != 5 && right_trigger < 0x80;
  }

 private:
  std::array<uint8_t, kBytes> bytes_{};
};

// Exact 17559 XamLoaderShouldConfirmReboot predicate. Executable privilege 18
// permits leaving without confirmation, except the dashboard's context 6.
inline bool ShouldConfirmDashboardReboot(bool is_dashboard,
                                         uint32_t executable_system_flags,
                                         uint32_t dashboard_context) {
  return (!is_dashboard && !(executable_system_flags & (1u << 18))) ||
         dashboard_context == 6;
}

// The existing process-spawn launch interface transports opaque bytes as hex.
inline std::string EncodeLaunchData(std::span<const uint8_t> bytes) {
  constexpr char digits[] = "0123456789abcdef";
  std::string result(bytes.size() * 2, '\0');
  for (size_t i = 0; i < bytes.size(); ++i) {
    result[i * 2] = digits[bytes[i] >> 4];
    result[i * 2 + 1] = digits[bytes[i] & 15];
  }
  return result;
}

}  // namespace xe::kernel::xam

#endif  // XENIA_KERNEL_XAM_XAM_LAUNCH_CONTRACT_H_
