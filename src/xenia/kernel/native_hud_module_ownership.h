/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia contributors. All rights reserved.                    *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */
#ifndef XENIA_KERNEL_NATIVE_HUD_MODULE_OWNERSHIP_H_
#define XENIA_KERNEL_NATIVE_HUD_MODULE_OWNERSHIP_H_

#include <memory>

namespace xe::kernel {
struct NativeSystemUiServiceContext;

enum class NativeHudOriginPolicy {
  kInstalledFlash,
  kFirstRecordSystemRoot,
};

struct NativeHudModuleOwnership {
  bool established = false;
  // Temporary domain/module execution leases. No native Ref/Release is done
  // by this holder, and it must not be retained as a permanent HUD pin.
  std::shared_ptr<void> observation;
};

// The native FE initializer loads hud.xex into81AA0B5C. This is the HUD child,
// not the native XAM callback owner or its separate self-module cell81AA3EE0.
// Does not prove FF registration, visibility, a frame, or service readiness.
NativeHudModuleOwnership ObserveNativeHudModuleOwnership(
    const NativeSystemUiServiceContext&, NativeHudOriginPolicy);

}  // namespace xe::kernel
#endif  // XENIA_KERNEL_NATIVE_HUD_MODULE_OWNERSHIP_H_
