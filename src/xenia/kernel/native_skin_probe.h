/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia contributors. All rights reserved.                    *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_NATIVE_SKIN_PROBE_H_
#define XENIA_KERNEL_NATIVE_SKIN_PROBE_H_

#include <atomic>
#include <functional>
#include <memory>
#include <optional>

#include "xenia/kernel/guest_runtime_plan.h"
#include "xenia/kernel/xobject.h"

namespace xe::kernel {
class KernelState;
class UserModule;

struct NativeSkinProbeSpec {
  // Deferred, same-plan entries. The locator metadata carries the strict
  // native_skin_layout, and readonly_strings must cover every source string.
  GuestRuntime::Function resource_locator;  // {0,0,0,0,128}/kHResult.
  GuestRuntime::Function load_visuals;      // {0,0}/kHResult.
  GuestRuntime::Function set_base_path;     // {0,0}/kHResult.
  GuestRuntime::Function get_base_path;     // {0}/kUInt32, borrowed UTF16*.
  // Independently audited locale/string-table/typeface composite. Its success
  // does not prove the internally ignored string-table load succeeded.
  std::optional<GuestRuntime::Function> initialize_locale_fonts;
  std::vector<GuestRuntimeWordCheck> readonly_strings;
  std::function<bool()> validate_device_context;
  std::shared_ptr<std::atomic<bool>> attempted =
      std::make_shared<std::atomic<bool>>(false);
};

struct NativeSkinProbeResult {
  bool attempted = false;
  bool ready = false;
  bool fonts_stage_completed = false;
  uint32_t resource_hmodule = 0;
};

// Call after authentic borrowed-DC owner initialization, before creating its
// background/HUD. The caller must keep the entire native engine/renderer/DC
// kernel-resident on ALL outcomes. This function retains real resource/module
// leases and scratch before native side effects, never rolls back or retries,
// and throws on any attempted partial failure. It submits no GPU work and does
// not establish successful control visuals, font pixels or a visible guide.
NativeSkinProbeResult RunNativeSkinProbe(
    KernelState* kernel_state, const object_ref<UserModule>& native_xam,
    const NativeSkinProbeSpec& spec);
}  // namespace xe::kernel

#endif  // XENIA_KERNEL_NATIVE_SKIN_PROBE_H_
