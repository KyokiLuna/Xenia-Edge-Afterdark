/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia contributors. All rights reserved.                    *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_NATIVE_GUIDE_SCENE_PROBE_H_
#define XENIA_KERNEL_NATIVE_GUIDE_SCENE_PROBE_H_

#include <functional>
#include <memory>
#include <optional>

#include "xenia/cpu/guest_import_provider.h"
#include "xenia/kernel/guest_runtime_plan.h"
#include "xenia/kernel/xobject.h"
#include "xenia/xbox.h"

namespace xe::kernel {
class KernelState;
class UserModule;

struct NativeGuideHostProbeSpec {
  // Deferred image-validated (owner, &HUD_root)/HRESULT entry. Its private
  // owner_app_layout metadata describes the actual borrowed-DC UI owner.
  GuestRuntime::Function attach_app;
  // Returns the real owner from a successful resident controller probe.
  std::function<uint32_t()> owner;
  // Optional deferred (owner, owned_float4x4_out)/HRESULT helper from the same
  // validated plan. Apply its finite native result through the public render
  // view-transform setter before Begin; absence preserves the existing DC.
  std::optional<GuestRuntime::Function> view_transform = std::nullopt;
};

struct NativeGuideSceneProbeSpec {
  std::shared_ptr<const cpu::GuestImportProvider> import_provider;
  // Deferred, image-validated leaf helpers. Dynamic arguments start at zero:
  // bind_resource_module {0,0}/kVoid; clear_resource_module {0}/kVoid.
  GuestRuntime::Function bind_resource_module;
  GuestRuntime::Function clear_resource_module;
  // One image-validated zero word for the owned, otherwise unused native slot.
  std::vector<GuestRuntimeWordCheck> resource_preflight;
  // Actual native render/post-render message constructors, both with template
  // {0,0,0,0xFFFFFFFF,1}/kVoid. The probe supplies owned message/data/DC
  // values.
  GuestRuntime::Function render_message;
  GuestRuntime::Function post_render_message;
  // Optional same-plan native public395 (element, borrowed_visual_out)/HRESULT
  // readback for the bounded host tree. It never attaches or frees a visual.
  std::optional<GuestRuntime::Function> get_control_visual = std::nullopt;
  // Select through the real native setter after background/controller setup.
  // Its paired getter must confirm the requested actor before HUD admission.
  bool select_native_actor = false;
  // Bounded diagnostic animation warmup after an initial zero delta. Each
  // update runs genuine timers/layout and passes 1000/60 milliseconds to
  // AnimRun. Timer time remains native; this is not a real-time input pump.
  uint32_t animation_frames = 30;
  // Optional diagnostic timing: Delay on the owning XThread between updates,
  // then supply measured guest-uptime deltas instead of simulated ones. The
  // update count remains bounded, with a 1-second step / 10-second total guard.
  // This pumps no input and does not alter the guest clock.
  bool real_time_updates = false;
  // Attach the HUD through the actual native owner and paint its composite
  // canvas. Requires retain_scene and both device/graph ownership guards.
  // After warmup, inspect at most 256 native elements / 32 levels using public
  // queries. Show and own opacity are readbacks, not effective visibility.
  std::optional<NativeGuideHostProbeSpec> host;
  // Keep actual scene/module/cache owners through command capture and future
  // GPU retirement. The caller must arrange lifecycle cleanup after all uses.
  bool retain_scene = false;
  std::function<bool()> validate_device_context;
  std::function<bool(std::shared_ptr<void>)> hold_scene_owner;
};

// Explicit unpublished-recording diagnostic on the owning SYSTEM thread.
// The caller owns a live native XUI engine/DC with no active RenderBegin.
// Loads a fresh HUD through the explicit provider, creates/navigates its real
// scene through lifecycle08, and sends the actual native render messages.
// By default closes/destroys/unloads HUD and clears its native resource slot
// before return. The retained mode transfers the complete graph to its owning
// SYSTEM device through kernel teardown. Owns one Begin/End around scene paint;
// does not submit or present. Submitted work would need strict GPU retirement.
X_HRESULT RunNativeGuideSceneProbe(KernelState* kernel_state,
                                   const object_ref<UserModule>& native_xam,
                                   uint32_t dc, uint32_t user_index,
                                   const NativeGuideSceneProbeSpec& spec);
}  // namespace xe::kernel

#endif  // XENIA_KERNEL_NATIVE_GUIDE_SCENE_PROBE_H_
