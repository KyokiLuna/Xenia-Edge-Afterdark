/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia contributors. All rights reserved.                    *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */
#pragma once
#include <array>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
namespace xe::kernel {
class KernelState;
// Console slot presets are volatile native policy, not profile creation or a
// claim of Live presence. All raw bits are retained for the four actual slots.
class NativeCurrentUserPresets {
 public:
  std::optional<uint32_t> Get(uint32_t slot) const;
  bool Set(uint32_t slot, uint32_t raw_state);
  void Stop();

 private:
  mutable std::mutex mutex_;
  std::array<uint32_t, 4> slots_{};
  bool stopped_ = false;
};
std::shared_ptr<NativeCurrentUserPresets> CurrentUserPresetsForKernel(
    KernelState*);
}  // namespace xe::kernel
