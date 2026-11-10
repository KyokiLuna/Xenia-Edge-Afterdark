/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia contributors. All rights reserved.                    *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_NATIVE_XUI_RENDER_PROBE_H_
#define XENIA_KERNEL_NATIVE_XUI_RENDER_PROBE_H_

#include <functional>
#include <optional>
#include <span>
#include <vector>

#include "xenia/kernel/guest_runtime_plan.h"
#include "xenia/kernel/xobject.h"
#include "xenia/xbox.h"

namespace xe::kernel {

class KernelState;
class UserModule;
struct NativeCompletedFrameRecord;

struct NativeXuiRecordProbeSpec {
  // Validated deferred entries, with dynamic operands initially zero:
  // create_resource {0}/kUInt32, set_buffer {0,0,0}/kVoid,
  // begin {0,0,0x10,0,0,0,0}/kVoid, end {0}/kHResult.
  // create_resource.abi_metadata must also contain the image-verified
  // record_layout table. This keeps private resource offsets in diagnostic
  // input rather than assuming one dashboard's object layout in this source.
  GuestRuntime::Function create_resource;
  GuestRuntime::Function set_buffer;
  GuestRuntime::Function begin;
  GuestRuntime::Function end;
};

struct NativeXuiTargetProbeSpec {
  // Validated deferred entries for the bounded 640x480 RGBA8/1x profile:
  // create_surface {640,480,0x18280186,0,0}/kUInt32,
  // get_viewport {0,0}/kVoid, get_color_write_mask {0}/kUInt32,
  // texture_header {640,480,1,0,0x18280106,0,0,0,0,0,0,0}/kUInt32.
  // The latter's twelve logical arguments are packed with GuestCallABI;
  // get_viewport and texture_header must match actual exports 8A6 and 8CF.
  // create_surface.abi_metadata supplies the image-verified target_layout.
  // Neither caller storage nor a module pin owns the native allocations.
  GuestRuntime::Function create_surface;
  GuestRuntime::Function get_viewport;
  GuestRuntime::Function get_color_write_mask;
  GuestRuntime::Function texture_header;
};

struct NativeXuiRenderProbeSpec {
  // The caller must validate the factory's complete code signature and ABI
  // against this loaded module generation. Its six-argument template must be
  // {0, 2, 0, 0, 0, 0}, kHResult; the probe binds its own output to argument6.
  GuestRuntime::Function factory;
  // Image-validated ownership preconditions must include distinct zero checks
  // for native XUI initialization, the renderer and its retained default DC.
  // The probe only reads these words; it must never manufacture native globals.
  std::vector<GuestRuntimeWordCheck> preflight;
  std::optional<NativeXuiRecordProbeSpec> record;
  // Optional real target/viewport/clear/resolve producer, requiring record.
  // The producer only records. Post-End execution requires a separate checked
  // allocation lease, backend authority and strict retirement.
  // Targets, command/texture storage and native owners remain alive through
  // capture and verified unpublished End cleanup. Any future GPU execution
  // needs separate allocation leases and strict retirement before Release.
  std::optional<NativeXuiTargetProbeSpec> target;
  // Optional checked preparation by an owning SYSTEM device diagnostic,
  // after factory validation and before InitShared. Requires retention of the
  // complete native graph. Failure/exception must not trigger native cleanup;
  // the callback owns its admission and never transfers device ownership.
  std::function<X_HRESULT(uint32_t device)> prepare_device;
  // An owning native background/controller may have process-lifetime globals
  // with no verified unpublish. Its explicit diagnostic must retain the whole
  // engine/DC/device/recording graph until full kernel/memory teardown on every
  // outcome, and must never retry partial initialization in this generation.
  bool retain_native_objects = false;
  // Synchronous private diagnostic copy after range/packet validation. The
  // callback must copy immediately and must not retain this borrowed span.
  std::function<bool(std::span<const uint8_t>)> capture_record;
  // Optional post-End owner/allocator coverage gate. The real producer fills
  // the physical allocation ranges; the callback must freeze before reading
  // dependencies. These numbers alone never authorize GPU submission; the
  // opt-in executor must obtain checked backing and exact-source authority.
  // Requires retain_native_objects, record and target so a failed seal never
  // falls through to native cleanup. Owns capture instead of capture_record.
  std::function<bool(const NativeCompletedFrameRecord&)> seal_record;
  // Optional actual-scene diagnostic inside owned XuiInit and the unpublished
  // recording, after the primitive's RenderEnd. It owns its own Begin/End and
  // either closes its scene before returning or transfers the whole native
  // graph to the retained SYSTEM owner. This callback cannot submit commands;
  // borrowed DC/brush handles are not standalone owners.
  std::function<X_HRESULT(uint32_t dc)> draw_scene;
};

// Explicit diagnostic only. Run synchronously on the caller's SYSTEM guest
// thread, after this resident module's runtime is Ready and its CPU-only XUI
// cycle has uninitialized. The caller excludes other native XUI operations and
// owns thread cancellation/join before full kernel/memory teardown. A failed
// execution/cleanup may leave native allocations resident: stop the isolated
// process rather than retrying or forcibly releasing unknown references.
// Without record, success covers the device/renderer/DC lifecycle. With record,
// it also covers owned native XuiInit, solid-rectangle draw recording and a
// bounded native PM4 draw stream. The target gate adds real target/viewport/
// clear/resolve recording. An explicitly opted-in seal callback may execute
// the frozen recording under its own checked leases/fences; recording success
// alone establishes neither GPU readiness nor rendered pixels.
X_HRESULT RunNativeXuiRenderProbe(KernelState* kernel_state,
                                  const object_ref<UserModule>& module,
                                  const NativeXuiRenderProbeSpec& spec);

}  // namespace xe::kernel

#endif  // XENIA_KERNEL_NATIVE_XUI_RENDER_PROBE_H_
