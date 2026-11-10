/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia contributors. All rights reserved.                    *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_NATIVE_HUD_CONTROLLER_PROBE_H_
#define XENIA_KERNEL_NATIVE_HUD_CONTROLLER_PROBE_H_

#include <atomic>
#include <memory>

#include "xenia/cpu/guest_call_abi.h"
#include "xenia/kernel/guest_runtime_plan.h"
#include "xenia/kernel/native_skin_probe.h"
#include "xenia/kernel/xobject.h"
#include "xenia/xbox.h"

namespace xe::kernel {
class KernelState;
class UserModule;

struct NativeHudControllerProbeSpec {
  // Deferred entries validated against this resident XAM generation. The
  // allocator's ABI metadata also contains the controller_layout table. The
  // audio initializer contains the independently verified native_audio_layout.
  // Actual allocator template {0x48,0x16}/kUInt32 is size72, tag22.
  GuestRuntime::Function allocate_owner;
  GuestRuntime::Function construct_owner;  // {0}/kUInt32 -> same pointer.
  GuestRuntime::Function initialize_borrowed_dc;  // {0,0}/kUInt32 -> BOOL.
  GuestRuntime::Function load_background;         // {0}/kHResult.
  GuestRuntime::Function register_sound_class;    // {}/kHResult.
  GuestRuntime::Function initialize_audio;        // {0}/kHResult.
  GuestRuntimeWordCheck controller_global;        // Image-owned zero word.
  GuestRuntimeWordCheck audio_engine_global;      // Image-owned zero word.
  // Authentic visual/base-path and optional locale/font setup, after native
  // borrowed-DC owner initialization and before background scene creation.
  std::optional<NativeSkinProbeSpec> skin;
  // Copies of the same resident diagnostic share admission. No retry after a
  // partial call: the native loader can destroy an already-published scene on
  // its own failure path, leaving the native global stale.
  std::shared_ptr<std::atomic<bool>> attempted =
      std::make_shared<std::atomic<bool>>(false);
};

struct NativeHudControllerProbeResult {
  bool executed = false;
  bool ready = false;
  bool audio_attempted = false;
  bool audio_ready = false;
  bool skin_attempted = false;
  bool skin_ready = false;
  bool loader_attempted = false;
  X_HRESULT status = X_E_FAIL;
  cpu::GuestCallError call_error = cpu::GuestCallError::kNone;
  uint32_t owner = 0;
  uint32_t controller = 0;
  uint32_t audio_engine = 0;
  uint32_t user_index = 0xFF;
};

// The caller MUST establish kernel-resident lifetime for the complete native
// engine, renderer, DC, recording, resources and module before invoking this
// single-attempt diagnostic. This function never destroys/frees native objects,
// clears private globals, submits GPU work or claims controller input works.
// Every failure ends this native-UI attempt. After an attempted audio init or
// background load, failure is specifically unsafe for subsequent native calls
// even though memory remains resident. No native audio shutdown is attempted;
// the caller must drain host audio callbacks before releasing guest memory.
//
// Run synchronously on the owning SYSTEM thread with native XuiInit active and
// a live DC, outside RenderBegin. Only ready && executed permits later HUD08.
NativeHudControllerProbeResult RunNativeHudControllerProbe(
    KernelState* kernel_state, const object_ref<UserModule>& native_xam,
    uint32_t dc, const NativeHudControllerProbeSpec& spec);
}  // namespace xe::kernel

#endif  // XENIA_KERNEL_NATIVE_HUD_CONTROLLER_PROBE_H_
