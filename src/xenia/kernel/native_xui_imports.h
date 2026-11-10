/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia contributors. All rights reserved.                    *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_NATIVE_XUI_IMPORTS_H_
#define XENIA_KERNEL_NATIVE_XUI_IMPORTS_H_

#include <memory>
#include <string>

#include "xenia/cpu/guest_import_provider.h"
#include "xenia/kernel/xobject.h"

namespace xe::kernel {
class KernelState;
class UserModule;

// Explicit native SYSTEM consumer only. Creates one coherent XUI/string/
// resource/control family from a separately loaded, resident Ready native XAM.
// The caller passes it only to a fresh system app, never a title or XAM itself.
// Missing family exports reject the consumer at import preflight; HLE service
// exports outside this family keep their existing implementations.
std::shared_ptr<const cpu::GuestImportProvider> CreateNativeXuiImportProvider(
    KernelState* kernel_state, const object_ref<UserModule>& native_xam,
    std::string* error = nullptr);

// Native system-domain profile. Keeps the coupled Task/Msg/SysApp/Notify/input,
// UI and cross-module allocation families in the same native XAM generation.
// Construction establishes binding identity only, not scheduler/driver/UI
// readiness. Register it in a NativeImportDomain before native child loads;
// never apply it globally to ordinary titles or an incompatible live module.
std::shared_ptr<const cpu::GuestImportProvider>
CreateNativeSystemUiImportProvider(KernelState* kernel_state,
                                   const object_ref<UserModule>& native_xam,
                                   std::string* error = nullptr);
}  // namespace xe::kernel

#endif  // XENIA_KERNEL_NATIVE_XUI_IMPORTS_H_
