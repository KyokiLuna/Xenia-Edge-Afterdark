/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia contributors. All rights reserved.                    *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/native_guide_scene_probe.h"

#include <array>
#include <atomic>
#include <cmath>
#include <initializer_list>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#include "third_party/tomlplusplus/toml.hpp"

#include "xenia/base/byte_order.h"
#include "xenia/base/clock.h"
#include "xenia/base/logging.h"
#include "xenia/cpu/guest_call_abi.h"
#include "xenia/cpu/processor.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/user_module.h"
#include "xenia/kernel/xam/app_manager.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_modules.h"
#include "xenia/kernel/xthread.h"

namespace xe::kernel {
namespace {

std::atomic_flag scene_probe_active = ATOMIC_FLAG_INIT;
constexpr uint32_t kHudAppId = 0xFF;
constexpr uint32_t kRootOffset = 0;
constexpr uint32_t kChildOffset = 4;
constexpr uint32_t kParentOffset = 8;
constexpr uint32_t kPropertyIdOffset = 0xC;
constexpr uint32_t kMessageOffset = 0x10;
constexpr uint32_t kRenderDataOffset = 0x30;
constexpr uint32_t kShowNameOffset = 0x50;
constexpr uint32_t kPropertyValueOffset = 0x60;
constexpr uint32_t kTreeHandleOffset = 0x80;
constexpr uint32_t kTreeIdOffset = 0x84;
constexpr uint32_t kTreeOpacityOffset = 0x88;
constexpr uint32_t kTreePropertyIdOffset = 0x8C;
constexpr uint32_t kTreePropertyValueOffset = 0x90;
constexpr uint32_t kViewTransformOffset = 0xB0;
// Reserve alignment slack after the tree's 32-byte property value. The
// 64-byte output remains owned through paint and any uncertain teardown.
constexpr uint32_t kScratchSize = 0x100;
constexpr size_t kTreeNodeLimit = 256;
constexpr uint32_t kTreeDepthLimit = 32;
constexpr size_t kTreeIdLimit = 128;

struct TreeId {
  std::string text;
  const char* status;
};

TreeId ReadTreeId(Memory* memory, uint32_t address) {
  if (!address) {
    return {{}, "unnamed"};
  }
  if (address & 1) {
    return {{}, "unsupported-unaligned"};
  }
  TreeId result{{}, "truncated-128-units"};
  constexpr char kHex[] = "0123456789ABCDEF";
  for (size_t i = 0; i < kTreeIdLimit; ++i) {
    const uint64_t next = uint64_t(address) + i * 2;
    if (next + 2 > uint64_t(UINT32_MAX) + 1 ||
        memory->QueryRangeAccess(uint32_t(next), 2) ==
            xe::memory::PageAccess::kNoAccess) {
      result.status = "unsupported-unreadable";
      return result;
    }
    const auto unit =
        xe::load_and_swap<uint16_t>(memory->TranslateVirtual(uint32_t(next)));
    if (!unit) {
      result.status = "terminated";
      return result;
    }
    // Preserve every UTF-16 unit without interpreting borrowed storage as a
    // host string. Escape non-ASCII/control units and log delimiters so one
    // malformed ID cannot inject another diagnostic line.
    if (unit >= 0x20 && unit <= 0x7E) {
      if (unit == '\\' || unit == '"') {
        result.text += '\\';
      }
      result.text += char(unit);
    } else {
      result.text += "\\u";
      for (int shift = 12; shift >= 0; shift -= 4) {
        result.text += kHex[(unit >> shift) & 0xF];
      }
    }
  }
  return result;
}

struct OwnerAppLayout {
  uint32_t size, canvas, dc, controller, attached_app, app_host;
};

std::string_view PlanTag(std::string_view metadata) {
  constexpr std::string_view key = "\nverified_plan_sha256=";
  const auto end = metadata.rfind(key);
  return end == std::string_view::npos ? std::string_view{}
                                       : metadata.substr(end + key.size());
}

std::optional<OwnerAppLayout> ReadOwnerAppLayout(std::string_view metadata) {
  const auto end = metadata.rfind("\nverified_plan_sha256=");
  if (end == std::string_view::npos) {
    return {};
  }
  toml::table document;
  try {
    document = toml::parse(metadata.substr(0, end));
  } catch (const toml::parse_error&) {
    return {};
  }
  const auto* table = document["owner_app_layout"].as_table();
  if (!table || table->size() != 7 || !(*table)["version"].is_integer() ||
      (*table)["version"].value<int64_t>() != 1) {
    return {};
  }
  OwnerAppLayout result{};
  const std::pair<const char*, uint32_t*> fields[] = {
      {"owner_size", &result.size},
      {"owner_canvas", &result.canvas},
      {"owner_dc", &result.dc},
      {"owner_controller", &result.controller},
      {"owner_attached_app", &result.attached_app},
      {"owner_app_host", &result.app_host},
  };
  for (const auto& [key, output] : fields) {
    const auto node = (*table)[key];
    if (!node.is_integer()) {
      return {};
    }
    const auto value = node.value<int64_t>();
    if (!value || *value < 0 || uint64_t(*value) > UINT32_MAX) {
      return {};
    }
    *output = uint32_t(*value);
  }
  if (result.size < 4 || result.size > 0x10000) {
    return {};
  }
  const std::array offsets{result.canvas, result.dc, result.controller,
                           result.attached_app, result.app_host};
  for (size_t i = 0; i < offsets.size(); ++i) {
    if (!offsets[i] || (offsets[i] & 3) || offsets[i] > result.size ||
        result.size - offsets[i] < 4) {
      return {};
    }
    for (size_t j = 0; j < i; ++j) {
      if (offsets[i] == offsets[j]) {
        return {};
      }
    }
  }
  return result;
}

bool Writable(Memory* memory, uint32_t address, uint32_t size) {
  const auto access = memory->QueryRangeAccess(address, size);
  return access == xe::memory::PageAccess::kReadWrite ||
         access == xe::memory::PageAccess::kExecuteReadWrite;
}

class SceneOperation {
 public:
  SceneOperation()
      : acquired_(!scene_probe_active.test_and_set(std::memory_order_acquire)) {
  }
  ~SceneOperation() {
    if (acquired_) {
      scene_probe_active.clear(std::memory_order_release);
    }
  }
  bool acquired() const { return acquired_; }

 private:
  bool acquired_;
};

class SceneScratch {
 public:
  explicit SceneScratch(Memory* memory)
      : memory_(memory), address_(memory->SystemHeapAlloc(kScratchSize)) {}
  ~SceneScratch() {
    if (address_ && !retained_) {
      memory_->SystemHeapFree(address_);
    }
  }
  uint32_t address() const { return address_; }
  void RetainUntilTeardown() { retained_ = true; }

 private:
  Memory* memory_;
  uint32_t address_;
  bool retained_ = false;
};

}  // namespace

X_HRESULT RunNativeGuideSceneProbe(KernelState* kernel_state,
                                   const object_ref<UserModule>& native_xam,
                                   uint32_t dc, uint32_t user_index,
                                   const NativeGuideSceneProbeSpec& spec) {
  if (!kernel_state || !native_xam || !dc || user_index >= 4 ||
      spec.animation_frames > 120 || !XThread::IsInThread() ||
      (spec.host && (!spec.retain_scene || !spec.validate_device_context ||
                     !spec.hold_scene_owner || !spec.host->owner))) {
    return X_E_INVALIDARG;
  }
  auto* thread = XThread::GetCurrentThread();
  auto provider = spec.import_provider;
  if (thread->kernel_state() != kernel_state ||
      thread->guest_object<X_KTHREAD>()->process_type != X_PROCTYPE_SYSTEM ||
      native_xam->kernel_state() != kernel_state ||
      !native_xam->is_system_app_module() || !provider ||
      native_xam->module_format() != UserModule::kModuleFormatXex ||
      provider->owner().hmodule != native_xam->hmodule_ptr()) {
    return X_E_INVALIDARG;
  }
  auto runtime = kernel_state->GetGuestRuntime(native_xam->hmodule_ptr());
  if (!runtime) {
    return X_E_FAIL;
  }
  const auto snapshot = runtime->GetSnapshot();
  if (snapshot.state != GuestRuntime::State::kReady ||
      snapshot.initialization_active ||
      snapshot.identity.generation != provider->owner().generation) {
    return X_E_FAIL;
  }
  SceneOperation operation;
  if (!operation.acquired()) {
    return X_HRESULT_FROM_WIN32(X_ERROR_BUSY);
  }
  auto* manager = kernel_state->app_manager();
  if (manager->guest_apps().HasDescriptor(kHudAppId) ||
      manager->guest_apps().ResourceModule(0) ||
      kernel_state->GetModule("\\SystemRoot\\hud.xex", true)) {
    // This diagnostic may not claim an existing scene or resource binding.
    return X_HRESULT_FROM_WIN32(X_ERROR_BUSY);
  }
  auto* image = native_xam->xex_module();
  auto* memory = kernel_state->memory();
  const auto owned = [&](uint32_t address, bool code) {
    xex2_section_type type;
    return address && !(address & 3) && address >= image->base_address() &&
           uint64_t(address) + 4 <=
               uint64_t(image->base_address()) + image->image_size() &&
           image->GetPageSectionType(address, &type) &&
           (code ? type == XEX_SECTION_CODE
                 : (type == XEX_SECTION_DATA ||
                    type == XEX_SECTION_READONLY_DATA)) &&
           memory->QueryRangeAccess(address, 4) !=
               xe::memory::PageAccess::kNoAccess;
  };
  const auto valid_helper = [&](const GuestRuntime::Function& function,
                                std::vector<uint64_t> arguments) {
    return owned(function.address, true) && !function.identity.empty() &&
           !function.abi_metadata.empty() &&
           function.return_kind == GuestRuntime::ReturnKind::kVoid &&
           function.arguments == arguments;
  };
  if (!valid_helper(spec.bind_resource_module, {0, 0}) ||
      !valid_helper(spec.clear_resource_module, {0}) ||
      !valid_helper(spec.render_message, {0, 0, 0, UINT32_MAX, 1}) ||
      !valid_helper(spec.post_render_message, {0, 0, 0, UINT32_MAX, 1}) ||
      spec.resource_preflight.size() != 1) {
    XELOGE("Native guide scene probe: validated deferred helpers required");
    return X_E_INVALIDARG;
  }
  std::optional<OwnerAppLayout> host_layout;
  if (spec.host) {
    const auto& attach = spec.host->attach_app;
    host_layout = ReadOwnerAppLayout(attach.abi_metadata);
    const auto tag = PlanTag(attach.abi_metadata);
    if (!host_layout || !owned(attach.address, true) ||
        kernel_state->GetUserModuleForAddress(attach.address).get() !=
            native_xam.get() ||
        attach.identity.empty() ||
        attach.return_kind != GuestRuntime::ReturnKind::kHResult ||
        attach.arguments != std::vector<uint64_t>{0, 0} || tag.size() != 64 ||
        !snapshot.plan || snapshot.plan->phases.empty() ||
        snapshot.plan->phases.front().functions.empty() ||
        tag !=
            PlanTag(
                snapshot.plan->phases.front().functions.front().abi_metadata) ||
        tag != PlanTag(spec.bind_resource_module.abi_metadata) ||
        tag != PlanTag(spec.render_message.abi_metadata) ||
        tag != PlanTag(spec.post_render_message.abi_metadata)) {
      XELOGE("Native guide host probe: image-validated owner API required");
      return X_E_INVALIDARG;
    }
    if (spec.host->view_transform) {
      const auto& view = *spec.host->view_transform;
      if (!owned(view.address, true) ||
          kernel_state->GetUserModuleForAddress(view.address).get() !=
              native_xam.get() ||
          view.identity.empty() ||
          view.return_kind != GuestRuntime::ReturnKind::kHResult ||
          view.arguments != std::vector<uint64_t>{0, 0} ||
          PlanTag(view.abi_metadata) != tag) {
        XELOGE(
            "Native guide host probe: same-plan native view helper required");
        return X_E_INVALIDARG;
      }
    }
  }
  uint32_t get_control_visual = 0;
  if (spec.get_control_visual) {
    const auto& visual = *spec.get_control_visual;
    const auto tag = PlanTag(visual.abi_metadata);
    const auto target = provider->Resolve(
        "xam.xex", 0x395, cpu::GuestImportProvider::Kind::kFunction);
    if (!owned(visual.address, true) ||
        kernel_state->GetUserModuleForAddress(visual.address).get() !=
            native_xam.get() ||
        visual.identity.empty() ||
        visual.return_kind != GuestRuntime::ReturnKind::kHResult ||
        visual.arguments != std::vector<uint64_t>{0, 0} || tag.size() != 64 ||
        !snapshot.plan || snapshot.plan->phases.empty() ||
        snapshot.plan->phases.front().functions.empty() ||
        tag !=
            PlanTag(
                snapshot.plan->phases.front().functions.front().abi_metadata) ||
        tag != PlanTag(spec.bind_resource_module.abi_metadata) ||
        tag != PlanTag(spec.render_message.abi_metadata) ||
        tag != PlanTag(spec.post_render_message.abi_metadata) ||
        target.disposition != cpu::GuestImportProvider::Disposition::kBound ||
        target.address != visual.address) {
      XELOGE(
          "Native guide tree probe: same-plan native control visual API "
          "required");
      return X_E_INVALIDARG;
    }
    get_control_visual = target.address;
  }
  const uint32_t native_slot = spec.resource_preflight.front().address;
  for (const auto& check : spec.resource_preflight) {
    if (check.nonzero || check.expected || !owned(check.address, false) ||
        xe::load_and_swap<uint32_t>(memory->TranslateVirtual(check.address))) {
      XELOGE("Native guide scene probe: native resource slot already owned");
      return X_HRESULT_FROM_WIN32(X_ERROR_BUSY);
    }
  }
  // These all belong to the same provider as HUD's imported object functions.
  constexpr std::array<uint16_t, 9> kOrdinals{0x35F, 0x322, 0x368, 0x32B, 0x320,
                                              0x34B, 0x34F, 0x365, 0x82A};
  std::array<uint32_t, kOrdinals.size()> exports{};
  for (size_t i = 0; i < kOrdinals.size(); ++i) {
    const auto target = provider->Resolve(
        "xam.xex", kOrdinals[i], cpu::GuestImportProvider::Kind::kFunction);
    if (target.disposition != cpu::GuestImportProvider::Disposition::kBound ||
        !owned(target.address, true)) {
      return X_E_INVALIDARG;
    }
    exports[i] = target.address;
  }
  // Public readbacks independently verify native attachment and Show state.
  // All are supplied by the same XUI object/class implementation as HUD.
  constexpr std::array<uint16_t, 5> kHostOrdinals{0x331, 0x348, 0x347, 0x379,
                                                  0x37A};
  std::array<uint32_t, kHostOrdinals.size()> host_exports{};
  if (spec.host) {
    for (size_t i = 0; i < kHostOrdinals.size(); ++i) {
      const auto target =
          provider->Resolve("xam.xex", kHostOrdinals[i],
                            cpu::GuestImportProvider::Kind::kFunction);
      if (target.disposition != cpu::GuestImportProvider::Disposition::kBound ||
          !owned(target.address, true)) {
        return X_E_INVALIDARG;
      }
      host_exports[i] = target.address;
    }
  }
  uint32_t set_view_transform = 0;
  if (spec.host && spec.host->view_transform) {
    const auto target = provider->Resolve(
        "xam.xex", 0x354, cpu::GuestImportProvider::Kind::kFunction);
    if (target.disposition != cpu::GuestImportProvider::Disposition::kBound ||
        !owned(target.address, true)) {
      XELOGE("Native guide host probe: native view setter unavailable");
      return X_E_INVALIDARG;
    }
    set_view_transform = target.address;
  }
  // These are public read-only element queries, not a private object-layout
  // walk. The class pointer is optional and remains opaque to the host.
  constexpr std::array<uint16_t, 4> kTreeOrdinals{0x330, 0x37D, 0x32E, 0x33E};
  std::array<uint32_t, kTreeOrdinals.size()> tree_exports{};
  if (spec.host) {
    for (size_t i = 0; i < kTreeOrdinals.size(); ++i) {
      const auto target =
          provider->Resolve("xam.xex", kTreeOrdinals[i],
                            cpu::GuestImportProvider::Kind::kFunction);
      if (target.disposition == cpu::GuestImportProvider::Disposition::kBound &&
          owned(target.address, true)) {
        tree_exports[i] = target.address;
      } else {
        XELOGI("Native guide tree probe: unsupported ordinal={:03X}",
               kTreeOrdinals[i]);
      }
    }
  }
  uint32_t get_actor = 0, set_actor = 0;
  if (spec.select_native_actor) {
    for (const auto [ordinal, output] :
         {std::pair{uint16_t(0x3C9), &get_actor},
          std::pair{uint16_t(0x46A), &set_actor}}) {
      const auto target = provider->Resolve(
          "xam.xex", ordinal, cpu::GuestImportProvider::Kind::kFunction);
      if (target.disposition != cpu::GuestImportProvider::Disposition::kBound ||
          !owned(target.address, true)) {
        return X_E_INVALIDARG;
      }
      *output = target.address;
    }
  }
  SceneScratch scratch(memory);
  if (!scratch.address()) {
    return X_HRESULT_FROM_WIN32(X_ERROR_NOT_ENOUGH_MEMORY);
  }
  const uint64_t view_address =
      (uint64_t(scratch.address()) + kViewTransformOffset + 15) & ~uint64_t(15);
  if (uint64_t(scratch.address()) + kScratchSize > uint64_t(UINT32_MAX) + 1 ||
      view_address + 64 > uint64_t(scratch.address()) + kScratchSize) {
    return X_E_FAIL;
  }
  auto* words = memory->TranslateVirtual<xe::be<uint32_t>*>(scratch.address());
  for (size_t i = 0; i < kScratchSize / 4; ++i) {
    words[i] = 0;
  }
  if (spec.host) {
    auto* name = memory->TranslateVirtual<xe::be<uint16_t>*>(scratch.address() +
                                                             kShowNameOffset);
    constexpr std::array<uint16_t, 5> kShowName{u'S', u'h', u'o', u'w', 0};
    for (size_t i = 0; i < kShowName.size(); ++i) {
      name[i] = kShowName[i];
    }
  }
  std::shared_ptr<KernelState::UserModuleExecutionLease> execution_lease;
  object_ref<UserModule> hud;
  xam::GuestAppRegistry::Snapshot registration;
  bool quarantined = false;
  uint32_t previous_actor = UINT32_MAX;
  uint32_t host_owner = 0, host_canvas = 0, host_controller = 0, host_app = 0,
           expected_attached_app = 0;
  bool host_context_ready = false;
  const auto read_word = [&](uint32_t address) {
    return xe::load_and_swap<uint32_t>(memory->TranslateVirtual(address));
  };
  const auto host_context_valid = [&](uint32_t attached_app) {
    if (!host_layout || !host_owner || (host_owner & 3) ||
        !Writable(memory, host_owner, host_layout->size)) {
      return false;
    }
    return read_word(host_owner + host_layout->canvas) == host_canvas &&
           read_word(host_owner + host_layout->dc) == dc &&
           read_word(host_owner + host_layout->controller) == host_controller &&
           read_word(host_owner + host_layout->app_host) == host_app &&
           read_word(host_owner + host_layout->attached_app) == attached_app &&
           host_controller && !(host_controller & 3) &&
           Writable(memory, host_controller, 4);
  };
  const auto quarantine = [&]() {
    if (quarantined) {
      return;
    }
    quarantined = true;
    scratch.RetainUntilTeardown();
    if (registration) {
      // Direct native Xui calls can unwind outside Registration::Invoke and
      // leave execution_failed unset. Close normal routing without waiting
      // or invoking destructive native lifecycle callbacks during unwind.
      manager->guest_apps().StopNormalAdmission(registration);
    }
    if (execution_lease) {
      kernel_state->RetainNativeResourceUntilTeardown(
          std::move(execution_lease));
    }
    if (hud) {
      kernel_state->RetainNativeResourceUntilTeardown(
          std::make_shared<object_ref<UserModule>>(hud));
    }
    if (registration) {
      kernel_state->RetainNativeResourceUntilTeardown(
          std::make_shared<xam::GuestAppRegistry::Snapshot>(registration));
    }
  };
  const auto invoke_typed =
      [&](const char* name, uint32_t address,
          std::span<const cpu::GuestCallArgument> arguments) {
        if ((spec.validate_device_context && !spec.validate_device_context()) ||
            (host_context_ready &&
             !host_context_valid(expected_attached_app))) {
          quarantine();
          throw std::runtime_error("native SYSTEM graphics ownership changed");
        }
        const auto call =
            cpu::ExecuteGuestCall(kernel_state->processor(),
                                  thread->thread_state(), address, arguments);
        if (!call.executed) {
          // Preserve all guest allocations and the loaded registration when an
          // interrupted native call provides no safe cleanup boundary. The
          // outer recording probe stops without tearing down its borrowed
          // renderer.
          quarantine();
          throw std::runtime_error("native guide scene execution interrupted");
        }
        const auto value = uint32_t(call.value);
        XELOGI("Native guide scene probe: {} returned={:08X}", name, value);
        return value;
      };
  const auto invoke = [&](const char* name, uint32_t address,
                          std::initializer_list<uint32_t> arguments) {
    std::array<cpu::GuestCallArgument, 8> packed{};
    if (arguments.size() > packed.size()) {
      throw std::runtime_error("native scene call ABI overflow");
    }
    size_t i = 0;
    for (const auto argument : arguments) {
      packed[i++] = cpu::GuestCallArgument::UInt32(argument);
    }
    return invoke_typed(name, address, packed);
  };
  const auto verify_host_show = [&](const char* stage) {
    // The authentic attachment helper ignores its Show-message result.
    // Query the real property through the same native class metadata.
    words[kPropertyIdOffset / 4] = 0;
    if (invoke("XuiObjectGetPropertyId(AppHost Show)", host_exports[1],
               {host_app, scratch.address() + kShowNameOffset,
                scratch.address() + kPropertyIdOffset}) != 0) {
      throw std::runtime_error("native AppHost Show property unavailable");
    }
    const uint32_t property = words[kPropertyIdOffset / 4];
    const uint32_t value = scratch.address() + kPropertyValueOffset;
    // Construct/Destruct are lifetime APIs, not HRESULT-returning calls.
    invoke("XUIElementPropVal_Construct(Show)", host_exports[3], {value});
    const auto show_result =
        invoke("XuiObjectGetProperty(AppHost Show)", host_exports[2],
               {host_app, property, 0, value});
    if (show_result != 0) {
      throw std::runtime_error("native AppHost Show query failed");
    }
    const uint32_t value_type = words[kPropertyValueOffset / 4];
    const uint32_t shown = words[kPropertyValueOffset / 4 + 1];
    if (value_type != 1 || shown > 1) {
      throw std::runtime_error("native AppHost Show type is not BOOL");
    }
    // A verified BOOL value owns no allocation. Interrupted or unexpected
    // values instead leave scratch retained; no speculative destructor.
    invoke("XUIElementPropVal_Destruct(Show)", host_exports[4], {value});
    XELOGI(
        "Native guide host probe: Show stage={} HRESULT={:08X} "
        "property={} type={} value={}",
        stage, show_result, property, value_type, shown);
    if (!shown) {
      throw std::runtime_error("native AppHost did not remain shown");
    }
  };
  const auto inspect_tree = [&](uint32_t canvas, uint32_t hud_root) {
    if (!tree_exports[0] || !tree_exports[1] || !tree_exports[2]) {
      XELOGI(
          "Native guide tree probe: unsupported required native queries; "
          "tree not inspected");
      return;
    }
    struct Node {
      uint32_t handle, parent, depth;
    };
    std::vector<Node> pending;
    pending.reserve(kTreeNodeLimit);
    pending.push_back({canvas, 0, 0});
    std::unordered_set<uint32_t> visited;
    visited.reserve(kTreeNodeLimit);
    visited.insert(canvas);
    size_t inspected = 0, unsupported = 0, truncated = 0;
    size_t visual_queries = 0, visual_attached = 0, visual_missing = 0,
           visual_not_control = 0, visual_unsupported = 0;
    bool saw_hud = false;
    XELOGI(
        "Native guide tree probe: begin canvas={:08X} HUD={:08X} "
        "max_nodes={} max_depth={}; sequential readbacks, own opacity only, "
        "no effective visibility or pixel inference",
        canvas, hud_root, kTreeNodeLimit, kTreeDepthLimit);
    for (size_t index = 0; index < pending.size(); ++index) {
      const Node node = pending[index];
      if (!invoke("XuiHandleIsValid(tree)", exports[2], {node.handle})) {
        ++unsupported;
        XELOGI(
            "Native guide tree probe: unsupported invalid handle={:08X} "
            "parent={:08X} depth={}",
            node.handle, node.parent, node.depth);
        continue;
      }
      words[kTreeHandleOffset / 4] = 0;
      const auto parent_result =
          invoke("XuiElementGetParent(tree)", host_exports[0],
                 {node.handle, scratch.address() + kTreeHandleOffset});
      const uint32_t actual_parent = words[kTreeHandleOffset / 4];
      if (parent_result || actual_parent != node.parent) {
        ++unsupported;
        XELOGI(
            "Native guide tree probe: unsupported parent handle={:08X} "
            "depth={} HRESULT={:08X} actual={:08X} expected={:08X}",
            node.handle, node.depth, parent_result, actual_parent, node.parent);
        continue;
      }
      ++inspected;
      saw_hud |= node.handle == hud_root;
      bool node_unsupported = false;

      words[kTreeIdOffset / 4] = 0;
      const auto id_result =
          invoke("XuiElementGetId(tree)", tree_exports[2],
                 {node.handle, scratch.address() + kTreeIdOffset});
      const TreeId id = id_result
                            ? TreeId{{}, "unsupported-HRESULT"}
                            : ReadTreeId(memory, words[kTreeIdOffset / 4]);
      if (id_result || std::string_view(id.status).starts_with("unsupported")) {
        node_unsupported = true;
      } else if (std::string_view(id.status).starts_with("truncated")) {
        ++truncated;
      }

      words[kTreeOpacityOffset / 4] = 0;
      const auto opacity_result =
          invoke("XuiElementGetOpacity(tree)", tree_exports[1],
                 {node.handle, scratch.address() + kTreeOpacityOffset});
      std::string opacity = "unsupported-HRESULT";
      if (!opacity_result) {
        const float value = xe::load_and_swap<float>(
            memory->TranslateVirtual(scratch.address() + kTreeOpacityOffset));
        if (std::isfinite(value)) {
          opacity = std::to_string(value);
        } else {
          opacity = "unsupported-nonfinite";
          node_unsupported = true;
        }
      } else {
        node_unsupported = true;
      }

      std::string_view show = "unsupported-property";
      words[kTreePropertyIdOffset / 4] = 0;
      const auto property_result =
          invoke("XuiObjectGetPropertyId(tree Show)", host_exports[1],
                 {node.handle, scratch.address() + kShowNameOffset,
                  scratch.address() + kTreePropertyIdOffset});
      uint32_t show_result = property_result, value_type = 0;
      if (!property_result) {
        const uint32_t property = words[kTreePropertyIdOffset / 4];
        const uint32_t value = scratch.address() + kTreePropertyValueOffset;
        invoke("XUIElementPropVal_Construct(tree Show)", host_exports[3],
               {value});
        show_result = invoke("XuiObjectGetProperty(tree Show)", host_exports[2],
                             {node.handle, property, 0, value});
        value_type = words[kTreePropertyValueOffset / 4];
        const uint32_t shown = words[kTreePropertyValueOffset / 4 + 1];
        if (!show_result && value_type == 1 && shown <= 1) {
          // Hidden false is an ordinary, useful readback, not a failed query.
          show = shown ? "true" : "false";
        } else {
          show = show_result ? "unsupported-HRESULT" : "unsupported-type";
        }
        // Get completed normally. Destruct the constructed variant through
        // its public API even for an unsupported type/result: a valid string
        // variant may own memory. Interrupted execution instead throws before
        // this point, retains scratch and performs no further native calls.
        invoke("XUIElementPropVal_Destruct(tree Show)", host_exports[4],
               {value});
      }
      node_unsupported |= show.starts_with("unsupported");
      const uint32_t object_class =
          tree_exports[3] ? invoke("XuiGetObjectClass(tree opaque)",
                                   tree_exports[3], {node.handle})
                          : 0;
      node_unsupported |= tree_exports[3] && !object_class;
      if (get_control_visual) {
        words[kTreeHandleOffset / 4] = 0;
        const auto visual_result =
            invoke("XuiControlGetVisual(tree)", get_control_visual,
                   {node.handle, scratch.address() + kTreeHandleOffset});
        const uint32_t visual = words[kTreeHandleOffset / 4];
        ++visual_queries;
        std::string_view visual_status;
        if (visual_result == 0x8030000A) {
          visual_status = "not-a-control";
          ++visual_not_control;
        } else if (visual_result == 0x80300017) {
          visual_status = "no-visual";
          ++visual_missing;
        } else if (visual_result == X_E_SUCCESS && visual &&
                   invoke("XuiHandleIsValid(control visual)", exports[2],
                          {visual})) {
          visual_status = "attached-valid";
          ++visual_attached;
        } else {
          visual_status = visual_result ? "unsupported-HRESULT"
                                        : "unsupported-invalid-success";
          ++visual_unsupported;
          node_unsupported = true;
        }
        XELOGI(
            "Native guide tree probe: visual node={} handle={:08X} "
            "HRESULT={:08X} visual={:08X} status={}; borrowed handle, "
            "no ownership or pixel inference",
            inspected, node.handle, visual_result, visual, visual_status);
      }
      XELOGI(
          "Native guide tree probe: node={} depth={} handle={:08X} "
          "parent={:08X} id=\"{}\" id_status={} id_HRESULT={:08X} "
          "Show={} Show_HRESULT={:08X} Show_type={} own_opacity={} "
          "opacity_HRESULT={:08X} class={:08X} class_status={} HUD_root={}",
          inspected, node.depth, node.handle, actual_parent, id.text, id.status,
          id_result, show, show_result, value_type, opacity, opacity_result,
          object_class,
          !tree_exports[3]
              ? "unsupported"
              : (object_class ? "opaque-not-dereferenced" : "unsupported-null"),
          node.handle == hud_root);
      unsupported += node_unsupported;

      // These handles are borrowed. A completed custom property getter could
      // have changed the tree; do not follow links from a retired/reparented
      // node. No timer, animation or layout pump runs during this walk.
      if (!invoke("XuiHandleIsValid(tree after queries)", exports[2],
                  {node.handle})) {
        ++unsupported;
        XELOGI("Native guide tree probe: unsupported retired handle={:08X}",
               node.handle);
        continue;
      }
      words[kTreeHandleOffset / 4] = 0;
      if (invoke("XuiElementGetParent(tree after queries)", host_exports[0],
                 {node.handle, scratch.address() + kTreeHandleOffset}) != 0 ||
          uint32_t(words[kTreeHandleOffset / 4]) != actual_parent) {
        ++unsupported;
        XELOGI(
            "Native guide tree probe: unsupported changed parent "
            "handle={:08X}",
            node.handle);
        continue;
      }
      words[kTreeHandleOffset / 4] = 0;
      const auto child_result =
          invoke("XuiElementGetFirstChild(tree)", exports[3],
                 {node.handle, scratch.address() + kTreeHandleOffset});
      if (child_result) {
        ++unsupported;
        XELOGI(
            "Native guide tree probe: unsupported children handle={:08X} "
            "HRESULT={:08X}",
            node.handle, child_result);
        continue;
      }
      uint32_t child = words[kTreeHandleOffset / 4];
      if (child && node.depth == kTreeDepthLimit) {
        ++truncated;
        XELOGI(
            "Native guide tree probe: truncated depth_limit={} "
            "parent={:08X} first_child={:08X}",
            kTreeDepthLimit, node.handle, child);
        continue;
      }
      while (child) {
        if (visited.contains(child)) {
          XELOGI(
              "Native guide tree probe: invalid repeated/cyclic handle="
              "{:08X} parent={:08X}",
              child, node.handle);
          // The host walk is bounded, but native layout/render may recurse
          // over these same links. Do not paint an inconsistent graph after
          // detecting a real cycle or a node shared by multiple parents.
          throw std::runtime_error("native guide tree is repeated/cyclic");
        }
        if (pending.size() == kTreeNodeLimit) {
          ++truncated;
          XELOGI(
              "Native guide tree probe: truncated node_limit={} "
              "parent={:08X} next_child={:08X}",
              kTreeNodeLimit, node.handle, child);
          break;
        }
        if (!invoke("XuiHandleIsValid(tree sibling)", exports[2], {child})) {
          ++unsupported;
          XELOGI(
              "Native guide tree probe: unsupported sibling={:08X} "
              "parent={:08X}",
              child, node.handle);
          break;
        }
        words[kTreeHandleOffset / 4] = 0;
        const auto sibling_parent_result =
            invoke("XuiElementGetParent(tree sibling)", host_exports[0],
                   {child, scratch.address() + kTreeHandleOffset});
        if (sibling_parent_result ||
            uint32_t(words[kTreeHandleOffset / 4]) != node.handle) {
          ++unsupported;
          XELOGI(
              "Native guide tree probe: unsupported sibling parent "
              "handle={:08X} HRESULT={:08X} actual={:08X} expected={:08X}",
              child, sibling_parent_result,
              uint32_t(words[kTreeHandleOffset / 4]), node.handle);
          break;
        }
        visited.insert(child);
        pending.push_back({child, node.handle, node.depth + 1});
        words[kTreeHandleOffset / 4] = 0;
        const auto next_result =
            invoke("XuiElementGetNext(tree)", tree_exports[0],
                   {child, scratch.address() + kTreeHandleOffset});
        if (next_result) {
          ++unsupported;
          XELOGI(
              "Native guide tree probe: unsupported next sibling "
              "handle={:08X} HRESULT={:08X}",
              child, next_result);
          break;
        }
        child = words[kTreeHandleOffset / 4];
      }
    }
    XELOGI(
        "Native guide tree probe: end inspected={} scheduled={} "
        "unsupported={} truncations={} HUD_seen={}; own values only",
        inspected, pending.size(), unsupported, truncated, saw_hud);
    if (get_control_visual) {
      XELOGI(
          "Native guide tree probe: control_visual queries={} "
          "attached_valid={} no_visual={} not_control={} unsupported={}; "
          "borrowed handles not released, no effective visibility inference",
          visual_queries, visual_attached, visual_missing, visual_not_control,
          visual_unsupported);
    }
  };

  try {
    if (spec.host) {
      host_owner = spec.host->owner();
      if (!host_owner || (host_owner & 3) ||
          !Writable(memory, host_owner, host_layout->size)) {
        throw std::runtime_error("native guide owner is not readable/writable");
      }
      host_canvas = read_word(host_owner + host_layout->canvas);
      host_controller = read_word(host_owner + host_layout->controller);
      host_app = read_word(host_owner + host_layout->app_host);
      if (!host_canvas || !host_app || host_canvas == host_app ||
          !host_context_valid(0)) {
        throw std::runtime_error("native guide owner is not a fresh host");
      }
      host_context_ready = true;
      if (!invoke("XuiHandleIsValid(owner canvas)", exports[2],
                  {host_canvas}) ||
          !invoke("XuiHandleIsValid(AppHost)", exports[2], {host_app})) {
        throw std::runtime_error("native guide owner handles are invalid");
      }
      XELOGI(
          "Native guide host probe: owner={:08X} canvas={:08X} "
          "AppHost={:08X} controller={:08X} DC={:08X} attached=0",
          host_owner, host_canvas, host_app, host_controller, dc);
    }
    if (spec.select_native_actor) {
      previous_actor = invoke("XamHudGetUserIndex(before)", get_actor, {});
      if (previous_actor != 0xFE && previous_actor >= 4) {
        throw std::runtime_error(
            "native HUD controller is not ready for an actor");
      }
      invoke("XamHudSetUserIndex(requested)", set_actor, {user_index});
      if (invoke("XamHudGetUserIndex(requested)", get_actor, {}) !=
          user_index) {
        throw std::runtime_error(
            "native HUD actor selection was not confirmed");
      }
    }
    if (host_context_ready && !host_context_valid(expected_attached_app)) {
      throw std::runtime_error("native guide owner changed before HUD load");
    }
    const auto load_result = kernel_state->LoadGuestSystemApp(
        kHudAppId, "\\SystemRoot\\hud.xex", user_index, 0, provider);
    registration = manager->guest_apps().Lookup(kHudAppId, true);
    hud = registration ? kernel_state->GetUserModuleForAddress(
                             registration->callback_address())
                       : nullptr;
    if (int32_t(load_result) < 0) {
      if (registration ||
          kernel_state->GetModule("\\SystemRoot\\hud.xex", true)) {
        throw std::runtime_error("failed native guide load retained an owner");
      }
      return load_result;
    }
    if (!registration || !hud ||
        hud->hmodule_ptr() != registration->owner_hmodule()) {
      throw std::runtime_error("native guide scene lost loaded owner");
    }
    execution_lease = kernel_state->AcquireUserModuleExecutionLease(hud);
    if (!execution_lease) {
      throw std::runtime_error(
          "native guide scene could not lease loaded owner");
    }
    if (load_result != 0) {
      // The loader activates any nonnegative04 result. An unexpected success
      // still owns HUD and must close it before outer renderer teardown.
      const auto cleanup = kernel_state->UnloadGuestSystemApp(
          kHudAppId, std::move(execution_lease));
      if (cleanup != 0 || registration->execution_failed() ||
          manager->guest_apps().Lookup(kHudAppId, true) ||
          kernel_state->GetModule("\\SystemRoot\\hud.xex", true)) {
        throw std::runtime_error(
            "native guide nonzero load could not clean up");
      }
      return X_E_FAIL;
    }
    const uint32_t hud_hmodule = hud->hmodule_ptr();
    auto* ldr = memory->TranslateVirtual<X_LDR_DATA_TABLE_ENTRY*>(hud_hmodule);
    const uint32_t load_count = ldr->load_count;
    const auto guest_unload = xboxkrnl::xeXexUnloadImage(hud_hmodule);
    const bool lease_protected =
        guest_unload == X_STATUS_ACCESS_DENIED &&
        uint32_t(ldr->load_count) == load_count &&
        kernel_state->GetModule("\\SystemRoot\\hud.xex", true).get() ==
            hud.get();
    XELOGI(
        "Native guide scene probe: leased guest unload status={:08X} "
        "load_count={}->{} preserved={}",
        guest_unload, load_count, uint32_t(ldr->load_count), lease_protected);
    if (!lease_protected) {
      throw std::runtime_error(
          "native guide execution lease did not protect owner");
    }
    invoke("bind HUD native resource slot", spec.bind_resource_module.address,
           {0, hud->hmodule_ptr()});
    if (xe::load_and_swap<uint32_t>(memory->TranslateVirtual(native_slot)) !=
        hud->hmodule_ptr()) {
      throw std::runtime_error(
          "native guide resource binding was not established");
    }
    if ((spec.validate_device_context && !spec.validate_device_context()) ||
        (host_context_ready && !host_context_valid(expected_attached_app))) {
      throw std::runtime_error("native SYSTEM device lost before HUD08");
    }
    const auto open_result = registration->DispatchLifecycle(
        0x80000008, dc, scratch.address() + kRootOffset);
    if (registration->execution_failed()) {
      throw std::runtime_error(
          "native guide lifecycle08 execution interrupted");
    }

    const uint32_t root = words[kRootOffset / 4];
    uint32_t paint_root = root;
    XELOGI("Native guide scene probe: lifecycle08 HRESULT={:08X} root={:08X}",
           open_result, root);
    X_HRESULT result = open_result == 0 ? X_E_FAIL : open_result;
    if (open_result == 0 && root &&
        invoke("XuiHandleIsValid(root)", exports[2], {root})) {
      words[kChildOffset / 4] = 0;
      const uint32_t child_result =
          invoke("XuiElementGetFirstChild", exports[3],
                 {root, scratch.address() + kChildOffset});
      const uint32_t child = words[kChildOffset / 4];
      XELOGI(
          "Native guide scene probe: first child HRESULT={:08X} "
          "handle={:08X}",
          child_result, child);
      if (child_result != 0) {
        result = int32_t(child_result) < 0 ? child_result : X_E_FAIL;
      } else if (child &&
                 invoke("XuiHandleIsValid(scene)", exports[2], {child})) {
        if (spec.host) {
          words[kParentOffset / 4] = 0;
          if (invoke("XuiElementGetParent(unattached HUD)", host_exports[0],
                     {root, scratch.address() + kParentOffset}) != 0 ||
              words[kParentOffset / 4] != 0) {
            throw std::runtime_error("native HUD root already has a parent");
          }
          const auto attached = invoke(
              "attach HUD to native AppHost", spec.host->attach_app.address,
              {host_owner, scratch.address() + kRootOffset});
          XELOGI("Native guide host probe: attach HRESULT={:08X} HUD={:08X}",
                 attached, root);
          if (attached != 0 || !host_context_valid(root) ||
              uint32_t(words[kRootOffset / 4]) != root) {
            throw std::runtime_error(
                "native HUD attachment ownership unproven");
          }
          expected_attached_app = root;
          words[kParentOffset / 4] = 0;
          const auto parent_result =
              invoke("XuiElementGetParent(attached HUD)", host_exports[0],
                     {root, scratch.address() + kParentOffset});
          const uint32_t parent = words[kParentOffset / 4];
          XELOGI(
              "Native guide host probe: parent HRESULT={:08X} "
              "actual={:08X} expected_AppHost={:08X}",
              parent_result, parent, host_app);
          if (parent_result != 0 || parent != host_app) {
            throw std::runtime_error("native HUD parent readback failed");
          }

          verify_host_show("attached");
          paint_root = host_canvas;
          XELOGI(
              "Native guide host probe: composite paint root={:08X} "
              "HUD={:08X}; attachment/host Show verified, no pixel acceptance",
              paint_root, root);
        }
        // Native frame order is timers -> layout -> AnimRun(delta ms). New
        // animations consume their first delta as zero. Advance the actual
        // animation engine with bounded simulated or measured deltas; timers
        // keep their own native tick_count and no private visibility/time is
        // set.
        constexpr float kDeltaMilliseconds = 1000.0f / 60.0f;
        const uint32_t update_start = Clock::QueryGuestUptimeMillis();
        uint32_t previous_update = update_start;
        for (uint32_t frame = 0; frame <= spec.animation_frames; ++frame) {
          float delta = frame ? kDeltaMilliseconds : 0.0f;
          if (spec.real_time_updates && frame) {
            // Delay on the owning XThread, then pass measured guest uptime to
            // AnimRun. These are diagnostic bounds, not native timing rules.
            if (thread->Delay(0, 0, uint64_t(-166667ll)) != X_STATUS_SUCCESS) {
              throw std::runtime_error("native guide frame delay interrupted");
            }
            const uint32_t now = Clock::QueryGuestUptimeMillis();
            if (now < previous_update || now < update_start ||
                now - previous_update > 1000 || now - update_start > 10000) {
              throw std::runtime_error(
                  "native guide frame clock out of bounds");
            }
            delta = float(now - previous_update);
            previous_update = now;
          }
          if (invoke("XuiTimersRun(update)", exports[7], {}) != 0 ||
              invoke("XuiElementLayoutTree(update)", exports[8],
                     {paint_root}) != 0) {
            throw std::runtime_error("native guide frame update failed");
          }
          const std::array tick{cpu::GuestCallArgument::Float32(delta)};
          if (invoke_typed("XuiAnimRun(update)", exports[4], tick) != 0) {
            throw std::runtime_error("native guide animation tick failed");
          }
        }
        const uint32_t update_end = Clock::QueryGuestUptimeMillis();
        if (spec.real_time_updates &&
            (update_end < update_start || update_end - update_start > 10000)) {
          throw std::runtime_error(
              "native guide total frame time out of bounds");
        }
        XELOGI(
            "Native guide scene probe: native updates={} clock_mode={} "
            "simulated_ms={} actual_guest_elapsed_ms={}; "
            "timer clock unchanged, no input pump",
            spec.animation_frames + 1,
            spec.real_time_updates ? "measured-after-Delay" : "simulated-delta",
            spec.real_time_updates ? 0.0f
                                   : spec.animation_frames * kDeltaMilliseconds,
            uint32_t(update_end - update_start));
        if (spec.host) {
          verify_host_show("paint");
          inspect_tree(paint_root, root);
          if (spec.host->view_transform) {
            const uint32_t matrix_address = uint32_t(view_address);
            auto* matrix_words =
                memory->TranslateVirtual<xe::be<uint32_t>*>(matrix_address);
            for (size_t i = 0; i < 16; ++i) {
              // The native helper must produce every element. An unwritten
              // output must not pass merely because scratch began as zero.
              matrix_words[i] = 0x7FC00000;
            }
            const auto view_result =
                invoke("build authentic owner view transform",
                       spec.host->view_transform->address,
                       {host_owner, matrix_address});
            if (view_result != X_E_SUCCESS) {
              throw std::runtime_error("native owner view transform failed");
            }
            std::array<float, 16> matrix;
            for (size_t i = 0; i < matrix.size(); ++i) {
              matrix[i] = xe::load_and_swap<float>(
                  memory->TranslateVirtual(matrix_address + uint32_t(i * 4)));
              if (!std::isfinite(matrix[i])) {
                throw std::runtime_error(
                    "native owner view transform is incomplete or nonfinite");
              }
            }
            const auto set_result =
                invoke("XuiRenderSetViewTransform(owner)", set_view_transform,
                       {dc, matrix_address});
            if (set_result != X_E_SUCCESS) {
              throw std::runtime_error("native render view transform failed");
            }
            XELOGI(
                "Native guide host probe: authentic view HRESULT={:08X} "
                "set={:08X} finite_matrix=[{},{},{},{};{},{},{},{};"
                "{},{},{},{};{},{},{},{}]",
                view_result, set_result, matrix[0], matrix[1], matrix[2],
                matrix[3], matrix[4], matrix[5], matrix[6], matrix[7],
                matrix[8], matrix[9], matrix[10], matrix[11], matrix[12],
                matrix[13], matrix[14], matrix[15]);
          } else {
            XELOGI(
                "Native guide host probe: no owner view helper in plan; "
                "preserving current borrowed DC view transform");
          }
        }
        result = invoke("XuiRenderBegin(scene)", exports[5], {dc, 0xFF000000});
        if (result == 0) {
          // HUD also lays out the tree after Begin, before its render message.
          if (invoke("XuiElementLayoutTree(paint)", exports[8], {paint_root}) !=
              0) {
            throw std::runtime_error("native guide paint layout failed");
          }
          const uint32_t message = scratch.address() + kMessageOffset;
          const uint32_t data = scratch.address() + kRenderDataOffset;
          invoke("build native render message", spec.render_message.address,
                 {message, data, dc, UINT32_MAX, 1});
          result = invoke("XuiSendMessage(render)", exports[0],
                          {paint_root, message});
          // The actual caller pairs post-render after every normally returned
          // render call, including a failure. Preserve its first failure.
          invoke("build native post-render message",
                 spec.post_render_message.address,
                 {message, data, dc, UINT32_MAX, 1});
          const auto post_result = invoke("XuiBubbleMessage(post-render)",
                                          exports[1], {paint_root, message});
          if (result == 0) {
            result = post_result;
          }
          // End also after returned draw errors. Interrupted execution or a
          // failed End does not establish a safe renderer unwind boundary.
          if (invoke("XuiRenderEnd(scene)", exports[6], {dc}) != 0) {
            throw std::runtime_error("native guide scene RenderEnd failed");
          }
        }
      }
    }

    if (spec.retain_scene) {
      if (result != X_E_SUCCESS ||
          (spec.host && !host_context_valid(expected_attached_app))) {
        throw std::runtime_error(
            "native resident scene did not reach a safe recording boundary");
      }
      struct ResidentScene {
        object_ref<UserModule> xam_module, hud_module;
        std::shared_ptr<KernelState::UserModuleExecutionLease> lease;
        xam::GuestAppRegistry::Snapshot registration;
        uint32_t root, paint_root, host_owner, app_host, dc, scratch;
      };
      scratch.RetainUntilTeardown();
      auto owner = std::make_shared<ResidentScene>(ResidentScene{
          native_xam, hud, execution_lease, registration, root, paint_root,
          host_owner, host_app, dc, scratch.address()});
      // Retention is an owned static recording, not public app admission.
      // TITLE callers must not enter native HUD outside the SYSTEM owner and
      // its device/context checks after this diagnostic returns.
      manager->guest_apps().StopNormalAdmission(registration);
      kernel_state->RetainNativeResourceUntilTeardown(owner);
      if (spec.hold_scene_owner && !spec.hold_scene_owner(owner)) {
        throw std::runtime_error(
            "native scene graph ownership could not be retained");
      }
      XELOGI(
          "Native guide scene probe: resident root={:08X} HRESULT={:08X}; "
          "scene/cache/module retained through record capture and retirement",
          root, result);
      return result == 0 || int32_t(result) < 0 ? result : X_E_FAIL;
    }

    // The common loader performs03, stops/drains admission, then05 and DLL
    // detach/unregistration. Retain both snapshots until the native slot
    // clears.
    const auto unload_result = kernel_state->UnloadGuestSystemApp(
        kHudAppId, std::move(execution_lease));
    if (registration->execution_failed() ||
        manager->guest_apps().Lookup(kHudAppId, true) ||
        kernel_state->GetModule("\\SystemRoot\\hud.xex", true)) {
      throw std::runtime_error(
          "native guide scene cleanup did not retire owner");
    }
    if (xe::load_and_swap<uint32_t>(memory->TranslateVirtual(native_slot)) !=
        hud->hmodule_ptr()) {
      throw std::runtime_error("native guide resource slot changed ownership");
    }
    invoke("clear HUD native resource slot", spec.clear_resource_module.address,
           {0});
    if (xe::load_and_swap<uint32_t>(memory->TranslateVirtual(native_slot))) {
      throw std::runtime_error("native guide resource slot did not clear");
    }
    if (unload_result != 0) {
      result = int32_t(unload_result) < 0 ? unload_result : X_E_FAIL;
    }
    if (spec.select_native_actor && previous_actor != UINT32_MAX) {
      invoke("XamHudSetUserIndex(restore)", set_actor, {previous_actor});
      if (invoke("XamHudGetUserIndex(restored)", get_actor, {}) !=
          previous_actor) {
        throw std::runtime_error("native HUD actor restore failed");
      }
    }
    XELOGI(
        "Native guide scene probe: complete HRESULT={:08X}; unpublished, no "
        "presentation or controller acceptance",
        result);
    return result == 0 || int32_t(result) < 0 ? result : X_E_FAIL;
  } catch (...) {
    quarantine();
    throw;
  }
}

}  // namespace xe::kernel
