/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia contributors. All rights reserved.                    *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */
#include "xenia/kernel/native_current_user_presets.h"
#include <map>
#include "xenia/kernel/kernel_state.h"
namespace xe::kernel {
namespace {
std::mutex registry_mutex;
std::map<KernelState*, std::weak_ptr<NativeCurrentUserPresets>> registry;
}  // namespace
std::optional<uint32_t> NativeCurrentUserPresets::Get(uint32_t slot) const {
  std::lock_guard lock(mutex_);
  if (stopped_ || slot >= slots_.size()) {
    return {};
  }
  return slots_[slot];
}
bool NativeCurrentUserPresets::Set(uint32_t slot, uint32_t raw_state) {
  std::lock_guard lock(mutex_);
  if (stopped_ || slot >= slots_.size()) {
    return false;
  }
  slots_[slot] = raw_state;
  return true;
}
void NativeCurrentUserPresets::Stop() {
  std::lock_guard lock(mutex_);
  stopped_ = true;
}
std::shared_ptr<NativeCurrentUserPresets> CurrentUserPresetsForKernel(
    KernelState* kernel) {
  if (!kernel) {
    return {};
  }
  {
    std::lock_guard lock(registry_mutex);
    if (auto previous = registry[kernel].lock()) {
      return previous;
    }
  }
  auto value = std::make_shared<NativeCurrentUserPresets>();
  struct Retirement {
    Retirement(KernelState* owner,
               std::shared_ptr<NativeCurrentUserPresets> state)
        : kernel(owner), value(std::move(state)) {}
    KernelState* kernel;
    std::shared_ptr<NativeCurrentUserPresets> value;
    ~Retirement() {
      value->Stop();
      std::lock_guard lock(registry_mutex);
      const auto found = registry.find(kernel);
      if (found != registry.end() && found->second.lock() == value) {
        registry.erase(found);
      }
    }
  };
  // Retain before publishing and never hold the registry mutex while entering
  // Kernel retirement registration. A racing unused candidate stays stopped
  // only at teardown and cannot erase another published store.
  kernel->RetainNativeResourceUntilTeardown(
      std::make_shared<Retirement>(kernel, value));
  std::lock_guard lock(registry_mutex);
  if (auto previous = registry[kernel].lock()) {
    return previous;
  }
  registry[kernel] = value;
  return value;
}
}  // namespace xe::kernel
