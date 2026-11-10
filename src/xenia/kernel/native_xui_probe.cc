/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia contributors. All rights reserved.                    *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/native_xui_probe.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <iterator>
#include <stdexcept>
#include <string>

#include "xenia/base/byte_order.h"
#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/cpu/ppc/ppc_context.h"
#include "xenia/cpu/processor.h"
#include "xenia/gpu/gpu_completion.h"
#include "xenia/kernel/guest_runtime_executor.h"
#include "xenia/kernel/guest_runtime_plan.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/native_frame_execution_probe.h"
#include "xenia/kernel/native_frame_lease_probe.h"
#include "xenia/kernel/native_frame_owner.h"
#include "xenia/kernel/native_gpu_completion_probe.h"
#include "xenia/kernel/native_graphics_device_binding.h"
#include "xenia/kernel/native_guide_scene_probe.h"
#include "xenia/kernel/native_hud_controller_probe.h"
#include "xenia/kernel/native_record_capture.h"
#include "xenia/kernel/native_xui_imports.h"
#include "xenia/kernel/native_xui_render_probe.h"
#include "xenia/kernel/user_module.h"
#include "xenia/kernel/xthread.h"
#include "xenia/memory.h"

DECLARE_path(native_xui_runtime_plan);
DECLARE_bool(native_xui_render_probe);
DECLARE_bool(native_guide_scene_probe);
DECLARE_bool(native_guide_host_probe);
DECLARE_bool(native_guide_real_time_probe);
DECLARE_bool(native_frame_lease_probe);
DECLARE_bool(native_frame_execute_probe);
DECLARE_bool(native_hud_controller_probe);
DECLARE_bool(native_xui_target_probe);
DECLARE_bool(native_system_device_binding_probe);
DECLARE_bool(native_gpu_context_probe);
DECLARE_path(native_xui_record_capture);

namespace xe::kernel {
namespace {

// This rejects concurrent probes without blocking a scheduler fiber while
// another probe executes guest code. It is not the native engine's lock.
std::atomic_flag probe_active = ATOMIC_FLAG_INIT;

class ProbeOperation {
 public:
  ProbeOperation()
      : acquired_(!probe_active.test_and_set(std::memory_order_acquire)) {}
  ~ProbeOperation() {
    if (acquired_) {
      probe_active.clear(std::memory_order_release);
    }
  }
  bool acquired() const { return acquired_; }

 private:
  bool acquired_;
};

class ProbeStorage {
 public:
  explicit ProbeStorage(Memory* memory)
      : memory_(memory), address_(memory->SystemHeapAlloc(0x40)) {}
  ~ProbeStorage() {
    if (address_) {
      memory_->SystemHeapFree(address_);
    }
  }
  uint32_t address() const { return address_; }

 private:
  Memory* memory_;
  uint32_t address_;
};

struct ProbeExport {
  uint16_t ordinal;
  const char* name;
  uint32_t address = 0;
};

}  // namespace

X_HRESULT RunNativeXuiProbe(KernelState* kernel_state) {
  auto* thread = XThread::GetCurrentThread();
  if (!kernel_state || !thread ||
      thread->guest_object<X_KTHREAD>()->process_type != X_PROCTYPE_SYSTEM) {
    XELOGE("Native XUI probe: requires a SYSTEM guest thread");
    return X_E_INVALIDARG;
  }
  ProbeOperation operation;
  if (!operation.acquired()) {
    return X_HRESULT_FROM_WIN32(X_ERROR_BUSY);
  }

  // user_only avoids the HLE kernel module with the same basename. The caller
  // owns loading and unloading; this function must not run the XAM entrypoint.
  auto module = kernel_state->GetModule("\\SystemRoot\\xam.xex", true);
  if (!module) {
    module = kernel_state->GetModule("\\SystemRoot\\$flash_xam.xex", true);
  }
  if (!module || module->module_type() != XModule::ModuleType::kUserModule) {
    XELOGE("Native XUI probe: no loaded guest system XAM image");
    return X_E_NOTFOUND;
  }
  auto* user_module = static_cast<UserModule*>(module.get());
  if (user_module->module_format() != UserModule::kModuleFormatXex ||
      user_module == kernel_state->GetExecutableModule().get()) {
    XELOGE("Native XUI probe: guest XAM is not a separate loaded XEX image");
    return X_E_INVALIDARG;
  }
  const auto* image = user_module->xex_module();
  const uint32_t image_base = image->base_address();
  if (!image->loaded() || !image_base) {
    XELOGE("Native XUI probe: guest XAM image is not ready");
    return X_E_FAIL;
  }
  const uint64_t image_end = uint64_t(image_base) + image->image_size();
  if (image_end > 0x100000000ull) {
    XELOGE("Native XUI probe: guest XAM image is not ready");
    return X_E_FAIL;
  }

  // Resource-only attachment is not native CRT readiness. Require an explicit
  // image-validated ordered runtime plan before entering native XUI at all.
  if (cvars::native_xui_runtime_plan.empty()) {
    XELOGE(
        "Native XUI probe: native runtime plan required; resource-only XAM "
        "is not initialized");
    return X_E_NOT_IMPLEMENTED;
  }
  std::shared_ptr<NativeFrameOwnerSession> frame_owner;
  NativeFrameOwnerSession::Update frame_update;
  auto device_binding =
      std::make_shared<std::shared_ptr<NativeGraphicsDeviceBinding>>();
  if (cvars::native_frame_lease_probe || cvars::native_frame_execute_probe) {
    if (!cvars::native_guide_host_probe || !cvars::native_guide_scene_probe ||
        !cvars::native_hud_controller_probe ||
        !cvars::native_system_device_binding_probe ||
        !cvars::native_xui_target_probe ||
        cvars::native_xui_record_capture.empty()) {
      XELOGE("Native frame lease: retained guide/target and capture required");
      return X_E_INVALIDARG;
    }
    const auto runtime =
        kernel_state->CreateGuestRuntime(retain_object(user_module));
    frame_owner = NativeFrameOwnerSession::CreateProvisional(
        thread->native_frame_thread_identity(),
        NativeGraphicsDeviceBinding::ReserveEpoch(), runtime);
    if (!frame_owner || !thread->SetNativeFrameOwnerSession(frame_owner)) {
      return X_E_FAIL;
    }
    // Parked fibers need not unwind. Kernel teardown explicitly revokes and
    // retires this persistent control block after producers have stopped.
    struct SessionRetirement {
      SessionRetirement(
          std::shared_ptr<NativeFrameOwnerSession> value,
          std::shared_ptr<std::shared_ptr<NativeGraphicsDeviceBinding>> binding)
          : session(std::move(value)), binding(std::move(binding)) {}
      std::shared_ptr<NativeFrameOwnerSession> session;
      std::shared_ptr<std::shared_ptr<NativeGraphicsDeviceBinding>> binding;
      ~SessionRetirement() {
        session->Revoke();
        const bool retired = session->RetireAfterKernelStop([&](const auto&) {
          // Kernel has stopped producers. Any attempted native submission
          // additionally needs the real matching backend stop boundary.
          const auto snapshot = session->GetSnapshot();
          return (!snapshot.gpu_attached && !snapshot.submission_attempted) ||
                 (*binding && (*binding)->HasStoppedBackend());
        });
        const auto& owner = session->owner();
        XELOGI("Native frame lease: kernel stop epoch={} session={} retired={}",
               owner.kernel_epoch, owner.session_id, retired);
      }
    };
    kernel_state->RetainNativeResourceUntilTeardown(
        std::make_shared<SessionRetirement>(frame_owner, device_binding));
    frame_update =
        frame_owner->BeginUpdate(thread->native_frame_thread_identity());
    if (!frame_update) {
      return X_E_FAIL;
    }
    const auto& tag = frame_owner->owner();
    XELOGI(
        "Native frame lease: provisional owner epoch={} generation={} "
        "session={} module={:08X} before heap/CRT initialization",
        tag.kernel_epoch, tag.module_generation, tag.session_id,
        tag.module_hmodule);
  }
  ValidatedGuestRuntimePlan verified;
  const auto runtime_result =
      InitializeGuestRuntimeFromPlan(kernel_state, retain_object(user_module),
                                     cvars::native_xui_runtime_plan, &verified);
  if (XFAILED(runtime_result)) {
    return runtime_result;
  }

  // Actual XAM export ordinals, resolved from the installed image. These are
  // not host trampolines or hardcoded addresses from one dashboard version.
  std::array<ProbeExport, 6> exports = {{{0x340, "XuiInit"},
                                         {0x33A, "XuiFindClass"},
                                         {0x325, "XuiCreateObject"},
                                         {0x346, "XuiObjectFromHandle"},
                                         {0x326, "XuiDestroyObject"},
                                         {0x361, "XuiUninit"}}};
  for (auto& entry : exports) {
    entry.address = module->GetProcAddressByOrdinal(entry.ordinal);
    xex2_section_type section_type;
    if ((entry.address & 3) || entry.address < image_base ||
        uint64_t(entry.address) + 4 > image_end ||
        !image->GetPageSectionType(entry.address, &section_type) ||
        section_type != XEX_SECTION_CODE) {
      XELOGE(
          "Native XUI probe: missing/out-of-image export {} ordinal={:03X} "
          "address={:08X}",
          entry.name, entry.ordinal, entry.address);
      return X_E_NOTFOUND;
    }
    XELOGI("Native XUI probe: export {} ordinal={:03X} address={:08X}",
           entry.name, entry.ordinal, entry.address);
  }

  auto* memory = kernel_state->memory();
  ProbeStorage storage(memory);
  const uint32_t params = storage.address();
  if (!params) {
    return X_HRESULT_FROM_WIN32(X_ERROR_NOT_ENOUGH_MEMORY);
  }
  auto* bytes = memory->TranslateVirtual<uint8_t*>(params);
  std::memset(bytes, 0, 0x40);
  auto* words = reinterpret_cast<xe::be<uint32_t>*>(bytes);
  // The actual HUD's default XuiInit parameters: size12 and two zero DWORDs.
  words[0] = 12;
  constexpr char16_t kClassName[] = u"XuiElement";
  constexpr uint32_t kClassNameOffset = 0x10;
  auto* class_name =
      reinterpret_cast<xe::be<uint16_t>*>(bytes + kClassNameOffset);
  for (size_t i = 0; i < std::size(kClassName); ++i) {
    class_name[i] = kClassName[i];
  }
  constexpr uint32_t kHandleOffset = 0x30;
  constexpr uint32_t kObjectOffset = 0x34;

  auto* state = thread->thread_state();
  auto* context = state->context();
  auto invoke = [&](size_t index, std::initializer_list<uint32_t> args,
                    uint32_t* result) {
    size_t argument = 3;
    for (const uint32_t value : args) {
      context->r[argument++] = value;
    }
    const bool executed =
        kernel_state->processor()->Execute(state, exports[index].address);
    *result = static_cast<uint32_t>(context->r[3]);
    XELOGI("Native XUI probe: {} executed={} result={:08X}",
           exports[index].name, executed, *result);
    return executed;
  };

  uint32_t init_result = 0;
  if (!invoke(0, {params}, &init_result)) {
    return X_E_FAIL;
  }
  if (static_cast<int32_t>(init_result) < 0) {
    // Actual XuiInit unwinds its partial initialization itself. In particular,
    // native XuiAlloc calls the guest XAM allocator directly; absent native
    // heap setup can yield OOM even when the HLE allocation exports work.
    XELOGE(
        "Native XUI probe: native initialization prerequisite failed "
        "HRESULT={:08X}; renderer and object stages skipped",
        init_result);
    return init_result;
  }
  if (init_result != X_E_SUCCESS) {
    // S_FALSE (1) means an existing engine. Do not destroy another owner's
    // engine with XuiUninit, or treat prior initialization as this probe's.
    XELOGW(
        "Native XUI probe: engine already initialized or unexpected success "
        "status={:08X}; leaving its owner in control",
        init_result);
    return X_HRESULT_FROM_WIN32(X_ERROR_BUSY);
  }

  X_HRESULT result = X_E_FAIL;
  uint32_t class_result = 0;
  uint32_t create_result = 0;
  uint32_t query_result = 0;
  uint32_t destroy_result = 0;
  uint32_t handle = 0;
  uint32_t native_object = 0;
  bool valid_object = false;
  bool destroyed = false;
  if (invoke(1, {params + kClassNameOffset}, &class_result) && class_result) {
    if (invoke(2, {params + kClassNameOffset, params + kHandleOffset},
               &create_result)) {
      handle = words[kHandleOffset / 4];
      XELOGI("Native XUI probe: XuiElement handle={:08X} HRESULT={:08X}",
             handle, create_result);
      if (static_cast<int32_t>(create_result) < 0) {
        result = create_result;
      } else if (handle &&
                 invoke(3, {handle, params + kObjectOffset}, &query_result)) {
        native_object = words[kObjectOffset / 4];
        valid_object = static_cast<int32_t>(query_result) >= 0 && native_object;
        XELOGI("Native XUI probe: native object={:08X} HRESULT={:08X} valid={}",
               native_object, query_result, valid_object);
        if (valid_object) {
          result = X_E_SUCCESS;
        } else if (static_cast<int32_t>(query_result) < 0) {
          result = query_result;
        }
      }
    }
  } else {
    XELOGE("Native XUI probe: built-in XuiElement class unavailable");
  }

  // Destroy only a handle the real engine just resolved successfully. Partial
  // engine allocations remain the native XuiUninit implementation's concern.
  if (valid_object) {
    if (!invoke(4, {handle}, &destroy_result)) {
      result = X_E_FAIL;
    } else if (static_cast<int32_t>(destroy_result) < 0) {
      result = destroy_result;
    } else {
      destroyed = true;
    }
  }
  uint32_t uninit_result = 0;
  if (!invoke(5, {}, &uninit_result)) {
    result = X_E_FAIL;
  } else if (uninit_result != X_E_SUCCESS) {
    // This probe owned a newly initialized engine. S_FALSE here would mean
    // it lost that ownership before cleanup, not a successful lifecycle.
    result = static_cast<int32_t>(uninit_result) < 0 ? uninit_result : X_E_FAIL;
  }
  XELOGI(
      "Native XUI probe: complete HRESULT={:08X} object_cycle={} "
      "uninit={:08X}; no renderer or presentation requested",
      result, valid_object && destroyed, uninit_result);
  if (result == X_E_SUCCESS &&
      (cvars::native_xui_render_probe || cvars::native_guide_scene_probe ||
       cvars::native_hud_controller_probe || cvars::native_xui_target_probe ||
       cvars::native_system_device_binding_probe ||
       cvars::native_gpu_context_probe || cvars::native_guide_host_probe ||
       cvars::native_guide_real_time_probe || cvars::native_frame_lease_probe ||
       cvars::native_frame_execute_probe)) {
    const auto factory = verified.catalog.find("native-light-device-factory");
    if (factory == verified.catalog.end()) {
      XELOGE(
          "Native XUI probe: verified light-device factory catalog required");
      return X_E_INVALIDARG;
    }
    NativeXuiRenderProbeSpec spec;
    spec.factory = factory->second;
    spec.preflight = verified.contracts.at(factory->first).before;
    if (cvars::native_system_device_binding_probe &&
        (!cvars::native_guide_scene_probe ||
         !cvars::native_hud_controller_probe)) {
      XELOGE("Native SYSTEM device: controller and scene ownership required");
      return X_E_INVALIDARG;
    }
    if (cvars::native_guide_host_probe &&
        (!cvars::native_guide_scene_probe ||
         !cvars::native_hud_controller_probe ||
         !cvars::native_system_device_binding_probe)) {
      XELOGE(
          "Native guide host: retained controller, scene and SYSTEM device "
          "ownership required");
      return X_E_INVALIDARG;
    }
    if (cvars::native_guide_real_time_probe &&
        !cvars::native_guide_scene_probe) {
      XELOGE("Native guide frame clock: scene diagnostic required");
      return X_E_INVALIDARG;
    }
    auto resident_controller =
        std::make_shared<NativeHudControllerProbeResult>();
    const auto create = verified.catalog.find("native-command-resource-create");
    if (create != verified.catalog.end()) {
      const auto set =
          verified.catalog.find("native-command-resource-set-buffer");
      const auto begin = verified.catalog.find("native-light-record-begin");
      const auto end = verified.catalog.find("native-light-record-end");
      if (set == verified.catalog.end() || begin == verified.catalog.end() ||
          end == verified.catalog.end()) {
        XELOGE("Native XUI probe: incomplete verified recording catalog");
        return X_E_INVALIDARG;
      }
      spec.record = NativeXuiRecordProbeSpec{create->second, set->second,
                                             begin->second, end->second};
      if (!cvars::native_xui_record_capture.empty()) {
        spec.capture_record = [memory = kernel_state->memory(),
                               path = cvars::native_xui_record_capture](
                                  std::span<const uint8_t> data) {
          return CaptureNativeRecord(memory, path, data);
        };
      }
      if (frame_owner) {
        spec.capture_record = {};
        // A full in-process dashboard relaunch creates a different owner.
        // Keep its artifacts distinct and preserve exclusive capture creation.
        auto capture_path = cvars::native_xui_record_capture;
        const auto& owner = frame_owner->owner();
        capture_path += ".owner-" + std::to_string(owner.kernel_epoch) + "-" +
                        std::to_string(owner.session_id);
        spec.seal_record = [kernel_state, frame_owner,
                            path = std::move(capture_path)](
                               const NativeCompletedFrameRecord& completed) {
          return cvars::native_frame_execute_probe
                     ? RunNativeFrameExecutionProbe(kernel_state, frame_owner,
                                                    completed, path)
                     : RunNativeFrameLeaseProbe(kernel_state, frame_owner,
                                                completed, path);
        };
      }
    }
    if (cvars::native_xui_target_probe) {
      const auto surface = verified.catalog.find("native-color-surface-create");
      const auto viewport = verified.catalog.find("native-get-viewport");
      const auto mask = verified.catalog.find("native-get-color-write-mask-0");
      const auto header = verified.catalog.find("native-texture-header");
      if (!spec.record || surface == verified.catalog.end() ||
          viewport == verified.catalog.end() ||
          mask == verified.catalog.end() || header == verified.catalog.end()) {
        XELOGE(
            "Native XUI target: complete verified producer catalog required");
        return X_E_INVALIDARG;
      }
      spec.target = NativeXuiTargetProbeSpec{surface->second, viewport->second,
                                             mask->second, header->second};
    }
    if (cvars::native_system_device_binding_probe) {
      spec.prepare_device = [kernel_state, module = retain_object(user_module),
                             factory = factory->second, device_binding,
                             frame_owner](uint32_t device) {
        gpu::GpuCompletion completed;
        const auto status =
            RunNativeGpuCompletionProbe(kernel_state, module, &completed);
        if (status != X_E_SUCCESS) {
          return status;
        }
        std::string error;
        *device_binding = NativeGraphicsDeviceBinding::Create(
            kernel_state, module, factory, device, completed.backend_epoch(),
            &error, frame_owner ? frame_owner->owner().kernel_epoch : 0,
            completed.backend_stop_proof(), cvars::native_frame_execute_probe);
        if (!*device_binding) {
          XELOGE("Native SYSTEM device: {}", error);
          return X_E_FAIL;
        }
        if (frame_owner &&
            !frame_owner->BindGraph(
                XThread::GetCurrentThread()->native_frame_thread_identity(),
                (*device_binding)
                    ->CreateFrameAuthority(frame_owner->owner()))) {
          XELOGE("Native frame lease: actual device graph binding refused");
          return X_E_FAIL;
        }
        return X_E_SUCCESS;
      };
    }
    if (cvars::native_guide_scene_probe) {
      const auto bind = verified.catalog.find("native-hud-resource-slot-bind");
      const auto clear =
          verified.catalog.find("native-hud-resource-slot-clear");
      const auto render = verified.catalog.find("native-hud-render-message");
      const auto post = verified.catalog.find("native-hud-post-render-message");
      if (!spec.record || bind == verified.catalog.end() ||
          clear == verified.catalog.end() || render == verified.catalog.end() ||
          post == verified.catalog.end()) {
        XELOGE("Native XUI probe: complete verified scene catalog required");
        return X_E_INVALIDARG;
      }
      std::string error;
      auto provider = CreateNativeXuiImportProvider(
          kernel_state, retain_object(user_module), &error);
      if (!provider) {
        XELOGE("Native XUI probe: native family refused: {}", error);
        return X_E_INVALIDARG;
      }
      NativeGuideSceneProbeSpec scene;
      scene.import_provider = std::move(provider);
      scene.bind_resource_module = bind->second;
      scene.clear_resource_module = clear->second;
      scene.resource_preflight = verified.contracts.at(bind->first).before;
      scene.render_message = render->second;
      scene.post_render_message = post->second;
      scene.real_time_updates = cvars::native_guide_real_time_probe;
      const auto control_visual =
          verified.catalog.find("native-ui-skin-get-control-visual");
      if (control_visual != verified.catalog.end()) {
        scene.get_control_visual = control_visual->second;
      }
      if (cvars::native_guide_host_probe) {
        const auto attach =
            verified.catalog.find("native-hud-owner-attach-app");
        if (attach == verified.catalog.end()) {
          XELOGE("Native guide host: verified attachment helper required");
          return X_E_INVALIDARG;
        }
        scene.host = NativeGuideHostProbeSpec{
            attach->second, [resident_controller] {
              return resident_controller->executed && resident_controller->ready
                         ? resident_controller->owner
                         : 0;
            }};
        const auto view =
            verified.catalog.find("native-hud-owner-view-transform");
        if (view != verified.catalog.end()) {
          scene.host->view_transform = view->second;
        }
      }
      if (cvars::native_system_device_binding_probe) {
        scene.select_native_actor = true;
        scene.retain_scene = true;
        scene.validate_device_context = [device_binding] {
          return *device_binding &&
                 (*device_binding)->CheckCpuResourceContext();
        };
        scene.hold_scene_owner = [device_binding](std::shared_ptr<void> owner) {
          return *device_binding &&
                 (*device_binding)->HoldGraphOwner(std::move(owner));
        };
      }
      spec.draw_scene = [kernel_state, module = retain_object(user_module),
                         scene = std::move(scene)](uint32_t dc) {
        return RunNativeGuideSceneProbe(kernel_state, module, dc, 0, scene);
      };
    }
    if (cvars::native_hud_controller_probe) {
      const auto allocate =
          verified.catalog.find("native-hud-controller-allocate-owner");
      const auto construct =
          verified.catalog.find("native-hud-controller-construct-owner");
      const auto initialize =
          verified.catalog.find("native-hud-controller-initialize-borrowed-dc");
      const auto background =
          verified.catalog.find("native-hud-controller-load-background");
      const auto sound =
          verified.catalog.find("native-hud-controller-register-sound-class");
      const auto audio =
          verified.catalog.find("native-ui-audio-initialize-default");
      if (!spec.record || allocate == verified.catalog.end() ||
          construct == verified.catalog.end() ||
          initialize == verified.catalog.end() ||
          background == verified.catalog.end() ||
          sound == verified.catalog.end() || audio == verified.catalog.end() ||
          verified.contracts.at(allocate->first).before.size() != 1 ||
          verified.contracts.at(audio->first).before.size() != 1) {
        XELOGE(
            "Native XUI probe: complete verified resident controller catalog "
            "required");
        return X_E_INVALIDARG;
      }
      NativeHudControllerProbeSpec controller;
      controller.allocate_owner = allocate->second;
      controller.construct_owner = construct->second;
      controller.initialize_borrowed_dc = initialize->second;
      controller.load_background = background->second;
      controller.register_sound_class = sound->second;
      controller.initialize_audio = audio->second;
      controller.controller_global =
          verified.contracts.at(allocate->first).before.front();
      controller.audio_engine_global =
          verified.contracts.at(audio->first).before.front();
      const auto skin_locator =
          verified.catalog.find("native-ui-skin-resource-locator");
      if (skin_locator != verified.catalog.end()) {
        const auto load_visuals =
            verified.catalog.find("native-ui-skin-load-visuals");
        const auto set_base =
            verified.catalog.find("native-ui-skin-set-base-path");
        const auto get_base =
            verified.catalog.find("native-ui-skin-get-base-path");
        if (load_visuals == verified.catalog.end() ||
            set_base == verified.catalog.end() ||
            get_base == verified.catalog.end()) {
          XELOGE(
              "Native HUD skin: complete verified resource catalog required");
          return X_E_INVALIDARG;
        }
        NativeSkinProbeSpec skin;
        skin.resource_locator = skin_locator->second;
        skin.load_visuals = load_visuals->second;
        skin.set_base_path = set_base->second;
        skin.get_base_path = get_base->second;
        skin.readonly_strings =
            verified.contracts.at(skin_locator->first).before;
        skin.validate_device_context = [device_binding] {
          return *device_binding &&
                 (*device_binding)->CheckCpuResourceContext();
        };
        const auto fonts =
            verified.catalog.find("native-ui-skin-initialize-locale-fonts");
        if (fonts != verified.catalog.end()) {
          skin.initialize_locale_fonts = fonts->second;
        }
        controller.skin = std::move(skin);
      }
      spec.retain_native_objects = true;
      spec.draw_scene = [kernel_state, module = retain_object(user_module),
                         controller = std::move(controller),
                         resident_controller,
                         next_scene = std::move(spec.draw_scene)](uint32_t dc) {
        const auto result =
            RunNativeHudControllerProbe(kernel_state, module, dc, controller);
        XELOGI(
            "Native XUI resident controller: executed={} ready={} "
            "audio_attempted={} audio_ready={} loader_attempted={} "
            "skin_attempted={} skin_ready={} "
            "HRESULT={:08X}",
            result.executed, result.ready, result.audio_attempted,
            result.audio_ready, result.loader_attempted, result.skin_attempted,
            result.skin_ready, result.status);
        if (!result.executed || (result.audio_attempted && !result.ready) ||
            (result.loader_attempted && !result.ready)) {
          // A failed native background load may already have published and
          // destroyed its controller. Do not invoke UI/record cleanup or retry.
          throw std::runtime_error(
              "resident native controller ownership unproven");
        }
        if (!result.ready) {
          return result.status;
        }
        // This is the actual constructor/background-loader result. Native
        // objects are already retained with the full renderer/module graph.
        *resident_controller = result;
        return next_scene ? next_scene(dc) : X_E_SUCCESS;
      };
    }
    result =
        RunNativeXuiRenderProbe(kernel_state, retain_object(user_module), spec);
    if (result == X_E_SUCCESS && *device_binding &&
        !(*device_binding)->CheckCpuResourceContext()) {
      XELOGE(
          "Native SYSTEM device: CPU resource ownership changed during "
          "recording");
      return X_E_FAIL;
    }
    if (result == X_E_SUCCESS && cvars::native_gpu_context_probe) {
      result = RunNativeGpuContextAllocationProbe(kernel_state,
                                                  retain_object(user_module));
    }
    if (result == X_E_SUCCESS) {
      result =
          RunNativeGpuCompletionProbe(kernel_state, retain_object(user_module));
    }
    return result;
  }
  return result;
}

}  // namespace xe::kernel
