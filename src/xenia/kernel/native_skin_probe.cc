/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia contributors. All rights reserved.                    *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/native_skin_probe.h"

#include <initializer_list>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "third_party/tomlplusplus/toml.hpp"
#include "xenia/base/byte_order.h"
#include "xenia/base/logging.h"
#include "xenia/cpu/guest_call_abi.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/user_module.h"
#include "xenia/kernel/xthread.h"

namespace xe::kernel {
namespace {
constexpr uint32_t kLocatorCapacity = 128;
constexpr uint32_t kLocatorBytes = kLocatorCapacity * 2;

struct Layout {
  uint32_t locator_capacity, module_name_rva, section_name_rva,
      resource_name_rva, base_path_rva;
};

std::string_view PlanTag(std::string_view metadata) {
  constexpr std::string_view suffix = "\nverified_plan_sha256=";
  const auto position = metadata.rfind(suffix);
  return position == std::string_view::npos
             ? std::string_view{}
             : metadata.substr(position + suffix.size());
}

std::optional<Layout> ReadLayout(std::string_view metadata) {
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
  const auto* table = document["native_skin_layout"].as_table();
  if (!table || table->size() != 6 || !(*table)["version"].is_integer() ||
      (*table)["version"].value<int64_t>() != 1) {
    return {};
  }
  Layout result{};
  const std::pair<const char*, uint32_t*> fields[] = {
      {"locator_capacity", &result.locator_capacity},
      {"module_name_rva", &result.module_name_rva},
      {"section_name_rva", &result.section_name_rva},
      {"resource_name_rva", &result.resource_name_rva},
      {"base_path_rva", &result.base_path_rva}};
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
  return result.locator_capacity == kLocatorCapacity
             ? std::optional<Layout>{result}
             : std::nullopt;
}

// All allocations intentionally survive to kernel/memory teardown. No late
// destructor dereferences Memory, frees guest objects, or invokes PPC.
struct ResidentSkin {
  object_ref<UserModule> native_xam, resource;
  std::shared_ptr<KernelState::UserModuleExecutionLease> xam_lease,
      resource_lease;
  uint32_t locator = 0;
};
}  // namespace

NativeSkinProbeResult RunNativeSkinProbe(
    KernelState* kernel_state, const object_ref<UserModule>& native_xam,
    const NativeSkinProbeSpec& spec) {
  NativeSkinProbeResult result;
  if (!kernel_state || !native_xam || !XThread::IsInThread() ||
      !spec.attempted || !spec.validate_device_context) {
    return result;
  }
  auto* thread = XThread::GetCurrentThread();
  if (thread->kernel_state() != kernel_state ||
      native_xam->kernel_state() != kernel_state ||
      thread->guest_object<X_KTHREAD>()->process_type != X_PROCTYPE_SYSTEM ||
      !native_xam->is_system_app_module() ||
      native_xam->module_format() != UserModule::kModuleFormatXex ||
      native_xam.get() == kernel_state->GetExecutableModule().get()) {
    return result;
  }
  const auto runtime = kernel_state->GetGuestRuntime(native_xam->hmodule_ptr());
  if (!runtime) {
    return result;
  }
  const auto snapshot = runtime->GetSnapshot();
  if (snapshot.state != GuestRuntime::State::kReady ||
      snapshot.initialization_active || !snapshot.plan ||
      snapshot.plan->phases.empty() ||
      snapshot.plan->phases.front().functions.empty() ||
      snapshot.identity.hmodule != native_xam->hmodule_ptr()) {
    return result;
  }
  const auto tag =
      PlanTag(snapshot.plan->phases.front().functions.front().abi_metadata);
  const auto layout = ReadLayout(spec.resource_locator.abi_metadata);
  if (!layout || tag.size() != 64) {
    return result;
  }
  auto* memory = kernel_state->memory();
  auto* image = native_xam->xex_module();
  const auto image_range = [&](uint32_t address, uint32_t size, bool code) {
    if (!address || !size || address < image->base_address() ||
        uint64_t(address) + size >
            uint64_t(image->base_address()) + image->image_size() ||
        memory->QueryRangeAccess(address, size) ==
            xe::memory::PageAccess::kNoAccess) {
      return false;
    }
    for (uint64_t cursor = address; cursor < uint64_t(address) + size;) {
      xex2_section_type section;
      if (!image->GetPageSectionType(uint32_t(cursor), &section) ||
          section != (code ? XEX_SECTION_CODE : XEX_SECTION_READONLY_DATA)) {
        return false;
      }
      cursor = (cursor & ~uint64_t(0xFFF)) + 0x1000;
    }
    return true;
  };
  const auto valid_function = [&](const GuestRuntime::Function& function,
                                  GuestRuntime::ReturnKind kind,
                                  std::initializer_list<uint64_t> arguments) {
    return !(function.address & 3) && image_range(function.address, 4, true) &&
           kernel_state->GetUserModuleForAddress(function.address).get() ==
               native_xam.get() &&
           !function.identity.empty() &&
           PlanTag(function.abi_metadata) == tag &&
           function.return_kind == kind &&
           function.arguments == std::vector<uint64_t>(arguments);
  };
  using Return = GuestRuntime::ReturnKind;
  if (!valid_function(spec.resource_locator, Return::kHResult,
                      {0, 0, 0, 0, kLocatorCapacity}) ||
      !valid_function(spec.load_visuals, Return::kHResult, {0, 0}) ||
      !valid_function(spec.set_base_path, Return::kHResult, {0, 0}) ||
      !valid_function(spec.get_base_path, Return::kUInt32, {0}) ||
      (spec.initialize_locale_fonts &&
       !valid_function(*spec.initialize_locale_fonts, Return::kHResult, {0})) ||
      spec.readonly_strings.empty()) {
    return result;
  }
  for (const auto& check : spec.readonly_strings) {
    if ((check.address & 3) || check.nonzero ||
        !image_range(check.address, 4, false) ||
        xe::load_and_swap<uint32_t>(memory->TranslateVirtual(check.address)) !=
            check.expected) {
      return result;
    }
  }
  const auto checked_string_range = [&](uint32_t address, uint32_t size) {
    if (!image_range(address, size, false)) {
      return false;
    }
    const uint32_t first = address & ~uint32_t(3);
    const uint64_t end = uint64_t(address) + size;
    for (uint64_t cursor = first; cursor < end; cursor += 4) {
      bool covered = false;
      for (const auto& check : spec.readonly_strings) {
        covered |= check.address == cursor;
      }
      if (!covered) {
        return false;
      }
    }
    return true;
  };
  const auto source_address = [&](uint32_t rva) -> uint32_t {
    const uint64_t address = uint64_t(image->base_address()) + rva;
    return address <= UINT32_MAX ? uint32_t(address) : 0;
  };
  const auto read_utf16 = [&](uint32_t address,
                              bool source) -> std::optional<std::u16string> {
    if (!address || (address & 1)) {
      return {};
    }
    std::u16string value;
    for (uint32_t i = 0; i < kLocatorCapacity; ++i) {
      const uint64_t next = uint64_t(address) + i * 2;
      if (next + 2 > (uint64_t(1) << 32) ||
          memory->QueryRangeAccess(uint32_t(next), 2) ==
              xe::memory::PageAccess::kNoAccess ||
          (source && !checked_string_range(uint32_t(next), 2))) {
        return {};
      }
      const auto unit =
          xe::load_and_swap<uint16_t>(memory->TranslateVirtual(uint32_t(next)));
      if (!unit) {
        return value;
      }
      value += char16_t(unit);
    }
    return {};
  };
  std::string module_path;
  const uint32_t module_name = source_address(layout->module_name_rva);
  for (uint32_t i = 0; i < 256; ++i) {
    const uint64_t address = uint64_t(module_name) + i;
    if (address > UINT32_MAX || !checked_string_range(uint32_t(address), 1)) {
      return result;
    }
    const uint8_t unit = *memory->TranslateVirtual<uint8_t*>(uint32_t(address));
    if (!unit) {
      break;
    }
    if (unit < 0x20 || unit > 0x7E || i == 255) {
      return result;
    }
    module_path += char(unit);
  }
  const uint32_t section_name = source_address(layout->section_name_rva);
  const uint32_t resource_name = source_address(layout->resource_name_rva);
  const uint32_t base_path = source_address(layout->base_path_rva);
  const auto section = read_utf16(section_name, true);
  const auto resource = read_utf16(resource_name, true);
  const auto expected_base = read_utf16(base_path, true);
  if (module_path.empty() || module_path.front() != '\\' || !section ||
      section->empty() || section->size() > 8 || !resource ||
      resource->empty() || !expected_base || expected_base->empty()) {
    return result;
  }
  std::string section_ascii;
  for (const auto unit : *section) {
    if (unit < 0x20 || unit > 0x7E) {
      return result;
    }
    section_ascii += char(unit);
  }
  if (!spec.validate_device_context() ||
      spec.attempted->exchange(true, std::memory_order_acq_rel)) {
    return result;
  }
  result.attempted = true;
  auto resident = std::make_shared<ResidentSkin>();
  resident->native_xam = native_xam;
  // Retain before the common loader or a native call can produce side effects.
  kernel_state->RetainNativeResourceUntilTeardown(resident);
  resident->xam_lease =
      kernel_state->AcquireUserModuleExecutionLease(native_xam);
  if (!resident->xam_lease) {
    throw std::runtime_error("native skin could not lease XAM owner");
  }
  resident->resource = kernel_state->LoadUserModule(module_path, false, true);
  if (!resident->resource ||
      resident->resource->module_format() != UserModule::kModuleFormatXex ||
      !resident->resource->is_system_app_module()) {
    throw std::runtime_error("native skin requires an owned resource-only XEX");
  }
  resident->resource_lease =
      kernel_state->AcquireUserModuleExecutionLease(resident->resource);
  if (!resident->resource_lease) {
    throw std::runtime_error("native skin could not lease resource module");
  }
  if (!resident->resource->guest_xex_header() &&
      kernel_state->FinishLoadingUserModule(resident->resource, false, false) !=
          X_STATUS_SUCCESS) {
    throw std::runtime_error(
        "native skin resource module did not finish loading");
  }
  if (resident->resource->entry_point()) {
    throw std::runtime_error(
        "native skin resource module has executable entry");
  }
  const auto* resource_image = resident->resource->xex_module();
  const uint64_t resource_begin = resource_image->base_address();
  const uint64_t resource_end = resource_begin + resource_image->image_size();
  if (!resource_begin || resource_end > (uint64_t(1) << 32) ||
      resource_end <= resource_begin) {
    throw std::runtime_error("native skin resource image range is invalid");
  }
  for (uint64_t cursor = resource_begin; cursor < resource_end;) {
    xex2_section_type section_type;
    if (!resource_image->GetPageSectionType(uint32_t(cursor), &section_type) ||
        section_type != XEX_SECTION_READONLY_DATA) {
      throw std::runtime_error("native skin resource image is not data-only");
    }
    cursor = (cursor & ~uint64_t(0xFFF)) + 0x1000;
  }
  result.resource_hmodule = resident->resource->hmodule_ptr();
  uint32_t section_address = 0, section_size = 0;
  if (resident->resource->GetSection(section_ascii, &section_address,
                                     &section_size) != X_STATUS_SUCCESS ||
      !section_address || !section_size || section_address < resource_begin ||
      uint64_t(section_address) + section_size > resource_end ||
      memory->QueryRangeAccess(section_address, section_size) ==
          xe::memory::PageAccess::kNoAccess) {
    throw std::runtime_error("native skin resource section is unavailable");
  }
  resident->locator = memory->SystemHeapAlloc(kLocatorBytes);
  if (!resident->locator ||
      memory->QueryRangeAccess(resident->locator, kLocatorBytes) !=
          xe::memory::PageAccess::kReadWrite) {
    throw std::runtime_error("native skin locator allocation failed");
  }
  xe::store_and_swap<uint16_t>(memory->TranslateVirtual(resident->locator), 0);
  const auto invoke = [&](const char* name, uint32_t address,
                          std::initializer_list<uint32_t> values) {
    if (!spec.validate_device_context()) {
      throw std::runtime_error("native skin graphics ownership changed");
    }
    const auto current = runtime->GetSnapshot();
    if (current.state != GuestRuntime::State::kReady ||
        current.initialization_active ||
        current.identity.generation != snapshot.identity.generation) {
      throw std::runtime_error("native skin runtime generation changed");
    }
    std::vector<cpu::GuestCallArgument> arguments;
    for (const auto value : values) {
      arguments.push_back(cpu::GuestCallArgument::UInt32(value));
    }
    const auto call = cpu::ExecuteGuestCall(
        kernel_state->processor(), thread->thread_state(), address, arguments);
    XELOGI("Native skin probe: {} executed={} value={:08X} error={}", name,
           call.executed, uint32_t(call.value), uint32_t(call.error));
    if (!call.executed) {
      throw std::runtime_error("native skin execution interrupted");
    }
    return uint32_t(call.value);
  };
  XELOGI(
      "Native skin probe: resident resource={} hmodule={:08X} "
      "section={} bytes={}; no rollback or retry",
      module_path, result.resource_hmodule, section_ascii, section_size);
  if (invoke("format actual skin resource locator",
             spec.resource_locator.address,
             {result.resource_hmodule, section_name, resource_name,
              resident->locator, kLocatorCapacity}) != 0) {
    throw std::runtime_error("native skin resource locator failed");
  }
  const auto locator = read_utf16(resident->locator, false);
  if (!locator || locator->empty()) {
    throw std::runtime_error("native skin resource locator is not terminated");
  }
  if (invoke("XuiLoadVisualFromBinary", spec.load_visuals.address,
             {resident->locator, 0}) != 0 ||
      invoke("XuiVisualSetBasePath", spec.set_base_path.address,
             {base_path, 0}) != 0) {
    throw std::runtime_error("native skin visual registration failed");
  }
  const uint32_t actual_base =
      invoke("XuiVisualGetBasePath", spec.get_base_path.address, {0});
  const auto actual_base_string = read_utf16(actual_base, false);
  if (!actual_base_string || *actual_base_string != *expected_base) {
    throw std::runtime_error("native skin base path readback failed");
  }
  if (spec.initialize_locale_fonts) {
    if (invoke("initialize native locale/string-table/typeface stage",
               spec.initialize_locale_fonts->address,
               {result.resource_hmodule}) != 0) {
      throw std::runtime_error("native skin locale/font stage failed");
    }
    result.fonts_stage_completed = true;
  }
  result.ready = true;
  XELOGI(
      "Native skin probe: visual load/base path verified; "
      "fonts_stage_completed={} string_table_unverified=true; "
      "resource graph retained, control visuals/pixels unverified",
      result.fonts_stage_completed);
  return result;
}
}  // namespace xe::kernel
