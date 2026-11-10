/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia contributors. All rights reserved.                    *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/base/cvar.h"

// Shared by kernel, input and graphics diagnostics without a kernel dependency.
DEFINE_bool(native_system_ui_trace, false,
            "Emit bounded native UI input, request, and frame transition "
            "diagnostics.",
            "Kernel");
