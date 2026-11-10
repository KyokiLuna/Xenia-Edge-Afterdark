/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia contributors. All rights reserved.                    *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/native_hud_controller_probe.h"

#include <initializer_list>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

#include "third_party/tomlplusplus/toml.hpp"
#include "xenia/base/byte_order.h"
#include "xenia/base/logging.h"
#include "xenia/cpu/processor.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/user_module.h"
#include "xenia/kernel/xthread.h"

namespace xe::kernel {
namespace {

struct ControllerLayout {
  uint32_t owner_size;
  uint32_t owner_canvas;
  uint32_t owner_dc;
  uint32_t owner_owns_dc;
  uint32_t owner_controller;
  uint32_t controller_size;
  uint32_t controller_xui;
  uint32_t controller_owner;
  uint32_t controller_user;
  uint32_t initial_user;
};

struct AudioLayout {
  uint32_t engine_global_rva;
  uint32_t engine_size;
  uint32_t engine_refcount;
  uint32_t engine_voice_core;
  uint32_t engine_render_wrapper;
  uint32_t engine_worker_count;
  uint32_t expected_system_worker_count;
  uint32_t wrapper_driver;
  uint32_t driver_handle;
};

std::string_view PlanTag(std::string_view metadata) {
  constexpr std::string_view prefix = "\nverified_plan_sha256=";
  const auto position = metadata.rfind(prefix);
  return position == std::string_view::npos
             ? std::string_view{}
             : metadata.substr(position + prefix.size());
}

std::optional<ControllerLayout> ReadLayout(std::string_view metadata) {
  constexpr std::string_view suffix = "\nverified_plan_sha256=";
  const auto end = metadata.rfind(suffix);
  if (end == std::string_view::npos) {
    return {};
  }
  toml::table document;
  try {
    document = toml::parse(metadata.substr(0, end));
  } catch (const toml::parse_error&) {
    return {};
  }
  const auto* table = document["controller_layout"].as_table();
  if (!table) {
    return {};
  }
  ControllerLayout layout{};
  const std::pair<const char*, uint32_t*> fields[] = {
      {"owner_size", &layout.owner_size},
      {"owner_canvas", &layout.owner_canvas},
      {"owner_dc", &layout.owner_dc},
      {"owner_owns_dc", &layout.owner_owns_dc},
      {"owner_controller", &layout.owner_controller},
      {"controller_size", &layout.controller_size},
      {"controller_xui", &layout.controller_xui},
      {"controller_owner", &layout.controller_owner},
      {"controller_user", &layout.controller_user},
      {"initial_user", &layout.initial_user},
  };
  for (const auto& [key, output] : fields) {
    if (!(*table)[key].is_integer()) {
      return {};
    }
    const auto value = (*table)[key].value<int64_t>();
    if (!value || *value < 0 || uint64_t(*value) > UINT32_MAX) {
      return {};
    }
    *output = uint32_t(*value);
  }
  const auto valid_offset = [](uint32_t offset, uint32_t size) {
    return !(offset & 3) && offset <= size && size - offset >= 4;
  };
  if (layout.owner_size < 4 || layout.owner_size > 0x10000 ||
      layout.controller_size < 4 || layout.controller_size > 0x10000 ||
      layout.initial_user > 0xFF) {
    return {};
  }
  for (const auto offset : {layout.owner_canvas, layout.owner_dc,
                            layout.owner_owns_dc, layout.owner_controller}) {
    if (!valid_offset(offset, layout.owner_size)) {
      return {};
    }
  }
  for (const auto offset : {layout.controller_xui, layout.controller_owner,
                            layout.controller_user}) {
    if (!valid_offset(offset, layout.controller_size)) {
      return {};
    }
  }
  return layout;
}

std::optional<AudioLayout> ReadAudioLayout(std::string_view metadata) {
  constexpr std::string_view suffix = "\nverified_plan_sha256=";
  const auto end = metadata.rfind(suffix);
  if (end == std::string_view::npos) {
    return {};
  }
  toml::table document;
  try {
    document = toml::parse(metadata.substr(0, end));
  } catch (const toml::parse_error&) {
    return {};
  }
  const auto* table = document["native_audio_layout"].as_table();
  if (!table) {
    return {};
  }
  AudioLayout layout{};
  const std::pair<const char*, uint32_t*> fields[] = {
      {"engine_global_rva", &layout.engine_global_rva},
      {"engine_size", &layout.engine_size},
      {"engine_refcount", &layout.engine_refcount},
      {"engine_voice_core", &layout.engine_voice_core},
      {"engine_render_wrapper", &layout.engine_render_wrapper},
      {"engine_worker_count", &layout.engine_worker_count},
      {"expected_system_worker_count", &layout.expected_system_worker_count},
      {"wrapper_driver", &layout.wrapper_driver},
      {"driver_handle", &layout.driver_handle},
  };
  for (const auto& [key, output] : fields) {
    if (!(*table)[key].is_integer()) {
      return {};
    }
    const auto value = (*table)[key].value<int64_t>();
    if (!value || *value < 0 || uint64_t(*value) > UINT32_MAX) {
      return {};
    }
    *output = uint32_t(*value);
  }
  const auto valid_offset = [](uint32_t offset, uint32_t size) {
    return !(offset & 3) && offset <= size && size - offset >= 4;
  };
  if (layout.engine_size < 4 || layout.engine_size > 0x10000 ||
      layout.expected_system_worker_count != 0 ||
      !valid_offset(layout.wrapper_driver, 0x10000) ||
      !valid_offset(layout.driver_handle, 0x10000)) {
    return {};
  }
  for (const auto offset :
       {layout.engine_refcount, layout.engine_voice_core,
        layout.engine_render_wrapper, layout.engine_worker_count}) {
    if (!valid_offset(offset, layout.engine_size)) {
      return {};
    }
  }
  return layout;
}

}  // namespace

NativeHudControllerProbeResult RunNativeHudControllerProbe(
    KernelState* kernel_state, const object_ref<UserModule>& native_xam,
    uint32_t dc, const NativeHudControllerProbeSpec& spec) {
  NativeHudControllerProbeResult result;
  result.status = X_E_INVALIDARG;
  if (!kernel_state || !native_xam || !dc || !spec.attempted ||
      !XThread::IsInThread()) {
    return result;
  }
  auto* thread = XThread::GetCurrentThread();
  if (thread->kernel_state() != kernel_state ||
      native_xam->kernel_state() != kernel_state ||
      thread->guest_object<X_KTHREAD>()->process_type != X_PROCTYPE_SYSTEM ||
      native_xam->module_format() != UserModule::kModuleFormatXex ||
      !native_xam->is_system_app_module() ||
      native_xam.get() == kernel_state->GetExecutableModule().get()) {
    return result;
  }
  const auto runtime = kernel_state->GetGuestRuntime(native_xam->hmodule_ptr());
  if (!runtime) {
    return result;
  }
  const auto snapshot = runtime->GetSnapshot();
  if (snapshot.state != GuestRuntime::State::kReady ||
      snapshot.initialization_active ||
      snapshot.identity.hmodule != native_xam->hmodule_ptr() ||
      !snapshot.plan || snapshot.plan->phases.empty() ||
      snapshot.plan->phases.front().functions.empty()) {
    return result;
  }
  const auto tag =
      PlanTag(snapshot.plan->phases.front().functions.front().abi_metadata);
  const auto layout = ReadLayout(spec.allocate_owner.abi_metadata);
  const auto audio_layout = ReadAudioLayout(spec.initialize_audio.abi_metadata);
  if (tag.size() != 64 || !layout || !audio_layout) {
    return result;
  }
  auto* image = native_xam->xex_module();
  auto* memory = kernel_state->memory();
  const auto image_range = [&](uint32_t address, bool code) {
    xex2_section_type section;
    return image->loaded() && address && !(address & 3) &&
           address >= image->base_address() &&
           uint64_t(address) + 4 <=
               uint64_t(image->base_address()) + image->image_size() &&
           image->GetPageSectionType(address, &section) &&
           (code ? section == XEX_SECTION_CODE
                 : (section == XEX_SECTION_DATA ||
                    section == XEX_SECTION_READONLY_DATA)) &&
           memory->QueryRangeAccess(address, 4) !=
               xe::memory::PageAccess::kNoAccess;
  };
  const auto valid_function = [&](const GuestRuntime::Function& function,
                                  GuestRuntime::ReturnKind kind,
                                  std::initializer_list<uint64_t> arguments) {
    return image_range(function.address, true) && !function.identity.empty() &&
           PlanTag(function.abi_metadata) == tag &&
           function.return_kind == kind &&
           function.arguments == std::vector<uint64_t>(arguments);
  };
  using Return = GuestRuntime::ReturnKind;
  if (spec.allocate_owner.arguments.size() != 2 ||
      spec.allocate_owner.arguments[0] != layout->owner_size ||
      spec.allocate_owner.arguments[1] > UINT32_MAX ||
      !valid_function(spec.allocate_owner, Return::kUInt32,
                      {layout->owner_size, spec.allocate_owner.arguments[1]}) ||
      !valid_function(spec.construct_owner, Return::kUInt32, {0}) ||
      !valid_function(spec.initialize_borrowed_dc, Return::kUInt32, {0, 0}) ||
      !valid_function(spec.load_background, Return::kHResult, {0}) ||
      !valid_function(spec.register_sound_class, Return::kHResult, {}) ||
      !valid_function(spec.initialize_audio, Return::kHResult, {0}) ||
      spec.controller_global.nonzero || spec.controller_global.expected ||
      !image_range(spec.controller_global.address, false) ||
      spec.audio_engine_global.nonzero || spec.audio_engine_global.expected ||
      !image_range(spec.audio_engine_global.address, false) ||
      uint64_t(image->base_address()) + audio_layout->engine_global_rva !=
          spec.audio_engine_global.address) {
    return result;
  }
  const auto read = [&](uint32_t address) {
    return xe::load_and_swap<uint32_t>(memory->TranslateVirtual(address));
  };
  if (read(spec.controller_global.address) ||
      read(spec.audio_engine_global.address)) {
    result.status = X_HRESULT_FROM_WIN32(X_ERROR_BUSY);
    return result;
  }
  const uint32_t get_user = native_xam->GetProcAddressByOrdinal(0x3C9);
  if (!image_range(get_user, true)) {
    return result;
  }
  if (spec.attempted->exchange(true, std::memory_order_acq_rel)) {
    result.status = X_HRESULT_FROM_WIN32(X_ERROR_BUSY);
    return result;
  }
  const auto invoke = [&](const char* name, uint32_t address,
                          std::initializer_list<uint32_t> values) {
    std::vector<cpu::GuestCallArgument> arguments;
    for (const uint32_t value : values) {
      arguments.push_back(cpu::GuestCallArgument::UInt32(value));
    }
    const auto call = cpu::ExecuteGuestCall(
        kernel_state->processor(), thread->thread_state(), address, arguments);
    result.executed = call.executed;
    result.call_error = call.error;
    XELOGI("Native HUD controller probe: {} executed={} value={:08X} error={}",
           name, call.executed, uint32_t(call.value), uint32_t(call.error));
    return call;
  };
  const auto writable = [&](uint32_t address, uint32_t size) {
    if (!address || (address & 3) ||
        uint64_t(address) + size > (uint64_t(1) << 32)) {
      return false;
    }
    return memory->QueryRangeAccess(address, size) ==
           xe::memory::PageAccess::kReadWrite;
  };
  const auto readable = [&](uint32_t address, uint32_t size) {
    if (!address || (address & 3) ||
        uint64_t(address) + size > (uint64_t(1) << 32)) {
      return false;
    }
    return memory->QueryRangeAccess(address, size) !=
           xe::memory::PageAccess::kNoAccess;
  };
  result.status = X_E_FAIL;
  auto call = invoke("XamHudGetUserIndex(absent)", get_user, {});
  if (!call.executed || uint32_t(call.value) != 0xFF) {
    return result;
  }
  call = invoke("register native XuiSoundXAudio class",
                spec.register_sound_class.address, {});
  if (!call.executed) {
    return result;
  }
  if (int32_t(call.value) < 0) {
    result.status = uint32_t(call.value);
    return result;
  }
  result.audio_attempted = true;
  call = invoke("initialize real native UI audio engine",
                spec.initialize_audio.address, {0});
  if (!call.executed) {
    return result;
  }
  if (call.value != 0) {
    // The native initializer may have published partially constructed state.
    // Do not inspect that global or make further guest calls after failure.
    result.status = int32_t(call.value) < 0 ? uint32_t(call.value) : X_E_FAIL;
    return result;
  }
  result.audio_engine = read(spec.audio_engine_global.address);
  if (!readable(result.audio_engine, audio_layout->engine_size) ||
      !read(result.audio_engine + audio_layout->engine_refcount) ||
      read(result.audio_engine + audio_layout->engine_worker_count) !=
          audio_layout->expected_system_worker_count) {
    return result;
  }
  const uint32_t voice_core =
      read(result.audio_engine + audio_layout->engine_voice_core);
  const uint32_t render_wrapper =
      read(result.audio_engine + audio_layout->engine_render_wrapper);
  if (!readable(voice_core, sizeof(uint32_t)) ||
      !readable(render_wrapper, audio_layout->wrapper_driver + 4)) {
    return result;
  }
  const uint32_t driver = read(render_wrapper + audio_layout->wrapper_driver);
  if (!readable(driver, audio_layout->driver_handle + 4)) {
    return result;
  }
  const uint32_t driver_handle = read(driver + audio_layout->driver_handle);
  // This is the current common XAudioRegisterRenderDriverClient handle
  // contract, not an address or private native object layout.
  if ((driver_handle & 0xFFFF0000) != 0x41550000) {
    return result;
  }
  result.audio_ready = true;
  XELOGI(
      "Native HUD controller probe: resident audio engine={:08X} "
      "voice_core={:08X} render_wrapper={:08X} driver={:08X} handle={:08X} "
      "system_workers={}",
      result.audio_engine, voice_core, render_wrapper, driver, driver_handle,
      audio_layout->expected_system_worker_count);
  call =
      invoke("allocate native UI owner", spec.allocate_owner.address,
             {layout->owner_size, uint32_t(spec.allocate_owner.arguments[1])});
  result.owner = uint32_t(call.value);
  if (!call.executed || !result.owner) {
    result.status = call.executed
                        ? X_HRESULT_FROM_WIN32(X_ERROR_NOT_ENOUGH_MEMORY)
                        : X_E_FAIL;
    return result;
  }
  if (!writable(result.owner, layout->owner_size)) {
    return result;
  }
  call = invoke("construct derived native UI owner",
                spec.construct_owner.address, {result.owner});
  if (!call.executed || uint32_t(call.value) != result.owner ||
      read(result.owner + layout->owner_owns_dc) != 0) {
    return result;
  }
  call = invoke("initialize native owner with borrowed DC",
                spec.initialize_borrowed_dc.address, {result.owner, dc});
  if (!call.executed || call.value != 1 ||
      read(result.owner + layout->owner_dc) != dc ||
      read(result.owner + layout->owner_owns_dc) != 0 ||
      !read(result.owner + layout->owner_canvas)) {
    return result;
  }
  if (spec.skin) {
    result.skin_attempted = true;
    const auto skin = RunNativeSkinProbe(kernel_state, native_xam, *spec.skin);
    result.skin_ready = skin.ready;
    if (!skin.attempted || !skin.ready) {
      // Audio/owner initialization already ran. The caller's resident contract
      // forbids destructive cleanup or later native UI after this failure.
      throw std::runtime_error(
          "native HUD skin bootstrap did not become ready");
    }
  }
  result.loader_attempted = true;
  call = invoke("load actual native hudbkgnd scene",
                spec.load_background.address, {result.owner});
  if (!call.executed) {
    return result;
  }
  if (call.value != 0) {
    // A normally returned loader error may still follow publication and an
    // internal DestroyObject. Do not dereference that global or call UI again.
    result.status = int32_t(call.value) < 0 ? uint32_t(call.value) : X_E_FAIL;
    return result;
  }
  result.controller = read(spec.controller_global.address);
  if (!writable(result.controller, layout->controller_size) ||
      read(result.owner + layout->owner_controller) != result.controller ||
      read(result.controller + layout->controller_owner) != result.owner ||
      !read(result.controller + layout->controller_xui)) {
    return result;
  }
  result.user_index = read(result.controller + layout->controller_user);
  if (result.user_index != layout->initial_user) {
    return result;
  }
  call = invoke("XamHudGetUserIndex(published)", get_user, {});
  if (!call.executed || uint32_t(call.value) != result.user_index) {
    return result;
  }
  result.ready = true;
  result.status = X_E_SUCCESS;
  XELOGI(
      "Native HUD controller probe: resident owner={:08X} controller={:08X} "
      "audio_engine={:08X} user={:02X}; no GPU submission or input/pixel "
      "acceptance",
      result.owner, result.controller, result.audio_engine, result.user_index);
  return result;
}

}  // namespace xe::kernel
