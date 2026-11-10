/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia contributors. All rights reserved.                    *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */
#pragma once
#include <cstdint>
#include <memory>
#include <string>
namespace xe::kernel {
class GuestRuntimeCallExecutor;
class NativeCurrentUserPresets;
// Mechanical checked binding evidence only. It is not complete profile/Live
// readiness and cannot open the notification/public feature family.
class NativeCurrentUserProviderBinding {
 public:
  static std::shared_ptr<NativeCurrentUserProviderBinding>
  InstallBeforeHeapAndWorkers(std::shared_ptr<GuestRuntimeCallExecutor>,
                              std::string* error);
  uint64_t root_generation() const;
  bool bindings_complete() const;

 private:
  struct State;
  explicit NativeCurrentUserProviderBinding(std::shared_ptr<State>);
  std::shared_ptr<State> state_;
};
}  // namespace xe::kernel
