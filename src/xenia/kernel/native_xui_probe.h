/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia contributors. All rights reserved.                    *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_NATIVE_XUI_PROBE_H_
#define XENIA_KERNEL_NATIVE_XUI_PROBE_H_

#include "xenia/xbox.h"

namespace xe::kernel {

class KernelState;

// Diagnostic only: the caller must explicitly enable the probe, retain the
// loaded system XAM image, and run this synchronously on a SYSTEM XHostThread.
// Calls actual loaded XAM exports, never DllMain or a renderer initializer.
// Success covers a native XuiElement lifecycle, not scenes or presentation.
X_HRESULT RunNativeXuiProbe(KernelState* kernel_state);

}  // namespace xe::kernel

#endif  // XENIA_KERNEL_NATIVE_XUI_PROBE_H_
