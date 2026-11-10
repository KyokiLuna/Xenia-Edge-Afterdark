/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia contributors. All rights reserved.                    *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/native_xui_render_probe.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <exception>
#include <initializer_list>
#include <string_view>
#include <unordered_set>
#include <vector>

#include "third_party/tomlplusplus/toml.hpp"
#include "xenia/base/byte_order.h"
#include "xenia/base/logging.h"
#include "xenia/base/mutex.h"
#include "xenia/cpu/guest_call_abi.h"
#include "xenia/cpu/processor.h"
#include "xenia/gpu/register_file.h"
#include "xenia/gpu/xenos.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/native_frame_lease_probe.h"
#include "xenia/kernel/user_module.h"
#include "xenia/kernel/xthread.h"
#include "xenia/memory.h"

namespace xe::kernel {
namespace {

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

constexpr uint32_t kParamsSize = 0x7C;
constexpr uint32_t kDeviceOffset = 0x80;
constexpr uint32_t kDcOffset = 0x84;
constexpr uint32_t kWidthOffset = 0x88;
constexpr uint32_t kHeightOffset = 0x8C;
constexpr uint32_t kXuiParamsOffset = 0x90;
constexpr uint32_t kBrushOffset = 0xA0;
constexpr uint32_t kBufferOffset = 0xA4;
constexpr uint32_t kRectOffset = 0xB0;
constexpr uint32_t kSavedStateOffset = 0xC0;
constexpr uint32_t kStorageSize = 0xF0;
constexpr uint32_t kTargetViewportOffset = 0x100;
constexpr uint32_t kTargetViewportReadbackOffset = 0x120;
constexpr uint32_t kTargetResolveRectOffset = 0x140;
constexpr uint32_t kTargetTextureHeaderOffset = 0x160;
constexpr uint32_t kTargetStorageSize = 0x260;
constexpr uint32_t kRecordBufferSize = 0x40000;
constexpr uint32_t kMaxResolveBytes = 4 * 1024 * 1024;

struct TargetLayout {
  uint32_t resource_size;
  uint32_t resource_type_offset;
  uint32_t resource_type_mask;
  uint32_t resource_type_value;
  uint32_t resource_refcount_offset;
  uint32_t resource_owned_flags;
  uint32_t texture_header_size;
  uint32_t texture_fetch_offset;
};

std::optional<TargetLayout> ParseTargetLayout(std::string_view metadata) {
  metadata = metadata.substr(0, metadata.find("\nverified_plan_sha256="));
  try {
    const auto call = toml::parse(metadata);
    const auto layout = call["target_layout"];
    if (layout["version"].value<int64_t>() != 1) {
      return {};
    }
    TargetLayout result{};
    const auto field = [&](const char* key, uint32_t* output) {
      const auto node = layout[key];
      const auto value = node.value<int64_t>();
      if (!node.is_integer() || !value || *value < 0 ||
          uint64_t(*value) > UINT32_MAX) {
        return false;
      }
      *output = uint32_t(*value);
      return true;
    };
    if (!field("resource_size", &result.resource_size) ||
        !field("resource_type_offset", &result.resource_type_offset) ||
        !field("resource_type_mask", &result.resource_type_mask) ||
        !field("resource_type_value", &result.resource_type_value) ||
        !field("resource_refcount_offset", &result.resource_refcount_offset) ||
        !field("resource_owned_flags", &result.resource_owned_flags) ||
        !field("texture_header_size", &result.texture_header_size) ||
        !field("texture_fetch_offset", &result.texture_fetch_offset)) {
      return {};
    }
    const auto word_fits = [](uint32_t size, uint32_t offset) {
      return size >= 4 && !(offset & 3) && offset <= size - 4;
    };
    if (result.resource_size > 0x1000 ||
        !word_fits(result.resource_size, result.resource_type_offset) ||
        !word_fits(result.resource_size, result.resource_refcount_offset) ||
        result.resource_type_offset == result.resource_refcount_offset ||
        !result.resource_type_mask || !result.resource_type_value ||
        (result.resource_type_value & ~result.resource_type_mask) ||
        !result.resource_owned_flags ||
        (result.resource_owned_flags & result.resource_type_mask) ||
        result.texture_header_size <
            sizeof(gpu::xenos::xe_gpu_texture_fetch_t) ||
        result.texture_header_size >
            kTargetStorageSize - kTargetTextureHeaderOffset ||
        (result.texture_fetch_offset & 3) ||
        result.texture_fetch_offset >
            result.texture_header_size -
                sizeof(gpu::xenos::xe_gpu_texture_fetch_t)) {
      return {};
    }
    return result;
  } catch (const toml::parse_error&) {
    return {};
  }
}

struct RecordLayout {
  uint32_t resource_size;
  uint32_t resource_header_offset;
  uint32_t resource_last_use_offset;
  uint32_t resource_flags_offset;
  uint32_t external_buffer_flag;
  uint32_t header_size;
  uint32_t header_count_offset;
  uint32_t header_address_offset;
  uint32_t count_mask;
};

std::optional<RecordLayout> ParseRecordLayout(std::string_view metadata) {
  // The validated executor appends provenance after the original TOML call.
  metadata = metadata.substr(0, metadata.find("\nverified_plan_sha256="));
  try {
    const auto call = toml::parse(metadata);
    const auto layout = call["record_layout"];
    if (layout["version"].value<int64_t>() != 1) {
      return {};
    }
    RecordLayout result{};
    const auto field = [&](const char* key, uint32_t* output) {
      const auto value = layout[key].value<int64_t>();
      if (!value || *value < 0 || uint64_t(*value) > UINT32_MAX) {
        return false;
      }
      *output = uint32_t(*value);
      return true;
    };
    if (!field("resource_size", &result.resource_size) ||
        !field("resource_header_offset", &result.resource_header_offset) ||
        !field("resource_last_use_offset", &result.resource_last_use_offset) ||
        !field("resource_flags_offset", &result.resource_flags_offset) ||
        !field("external_buffer_flag", &result.external_buffer_flag) ||
        !field("header_size", &result.header_size) ||
        !field("header_count_offset", &result.header_count_offset) ||
        !field("header_address_offset", &result.header_address_offset) ||
        !field("count_mask", &result.count_mask)) {
      return {};
    }
    const auto word_fits = [](uint32_t size, uint32_t offset) {
      return size >= 4 && !(offset & 3) && offset <= size - 4;
    };
    if (result.resource_size > 0x1000 || result.header_size > 0x1000 ||
        !word_fits(result.resource_size, result.resource_header_offset) ||
        !word_fits(result.resource_size, result.resource_last_use_offset) ||
        !word_fits(result.resource_size, result.resource_flags_offset) ||
        !word_fits(result.header_size, result.header_count_offset) ||
        !word_fits(result.header_size, result.header_address_offset) ||
        !result.external_buffer_flag ||
        (result.external_buffer_flag & (result.external_buffer_flag - 1)) ||
        !result.count_mask || result.count_mask > 0xFFFFFF ||
        (result.count_mask & (result.count_mask + 1))) {
      return {};
    }
    return result;
  } catch (const toml::parse_error&) {
    return {};
  }
}

bool CheckRecordedDraws(
    Memory* memory, uint32_t resource, uint32_t buffer,
    const RecordLayout& layout,
    const std::function<bool(std::span<const uint8_t>, uint32_t)>& capture,
    bool require_target = false) {
  using namespace xe::gpu;
  const auto word = [&](uint32_t offset) {
    return xe::load_and_swap<uint32_t>(
        memory->TranslateVirtual(resource + offset));
  };
  uint32_t count = 0, commands = 0;
  std::vector<uint8_t> owned_commands;
  {
    // The seal caller already holds the real full-buffer backing pin. Keep the
    // allocator guard while reading headers/copying bytes, then release it
    // before parsing or invoking graph-authority callbacks.
    auto heap_lock = global_critical_region::Acquire();
    const uint32_t header = word(layout.resource_header_offset) & 0x1FFFFFFF;
    auto* physical = memory->GetPhysicalHeap();
    if ((header & 3) || !header ||
        uint64_t(header) + layout.header_size > 0x20000000ull ||
        physical->QueryRangeAccess(header, header + layout.header_size - 1) ==
            xe::memory::PageAccess::kNoAccess) {
      XELOGE("Native XUI record probe: invalid actual command header");
      return false;
    }
    const auto* bytes = memory->TranslatePhysical<const uint8_t*>(header);
    count = xe::load_and_swap<uint32_t>(bytes + layout.header_count_offset) &
            layout.count_mask;
    commands =
        xe::load_and_swap<uint32_t>(bytes + layout.header_address_offset) &
        0x1FFFFFFF;
    // This is an allocation's guest virtual alias, not an encoded GPU address.
    // In particular, the E window has a physical +0x1000 bias in Memory.
    const auto* buffer_heap = memory->LookupHeap(buffer);
    if (!buffer_heap || buffer_heap->heap_type() != HeapType::kGuestPhysical) {
      return false;
    }
    const uint32_t buffer_base = memory->GetPhysicalAddress(buffer);
    const uint64_t command_end = uint64_t(commands) + uint64_t(count) * 4;
    if (uint64_t(buffer_base) + kRecordBufferSize > 0x20000000ull || !count ||
        (commands & 3) || commands < buffer_base ||
        command_end > uint64_t(buffer_base) + kRecordBufferSize ||
        physical->QueryRangeAccess(commands, uint32_t(command_end - 1)) ==
            xe::memory::PageAccess::kNoAccess) {
      XELOGE("Native XUI record probe: command extent outside owned buffer");
      return false;
    }
    const auto* borrowed = memory->TranslatePhysical<const uint8_t*>(commands);
    owned_commands.assign(borrowed, borrowed + size_t(count) * 4);
  }
  const auto* commands_be = owned_commands.data();
  uint32_t packets = 0;
  uint32_t draws = 0;
  uint32_t target_draws = 0;
  // This is a narrow producer diagnostic, not a command-stream executor or
  // shader validator. The native clear may draw before the XUI projection is
  // installed; require at least one later color draw with the real projection.
  gpu::RegisterFile registers;
  std::array<bool, gpu::RegisterFile::kRegisterCount> known{};
  for (uint32_t cursor = 0; cursor < count;) {
    const uint32_t packet =
        xe::load_and_swap<uint32_t>(commands_be + uint64_t(cursor) * 4);
    const uint32_t type = packet >> 30;
    const uint32_t packet_size = type == 2   ? 1
                                 : type == 1 ? 3
                                             : ((packet >> 16) & 0x3FFF) + 2;
    if (packet_size > count - cursor) {
      XELOGE("Native XUI record probe: truncated PM4 packet at DWORD{}",
             cursor);
      return false;
    }
    if (require_target && type == 0) {
      const uint32_t index = packet & 0x7FFF;
      const bool one_register = (packet & 0x8000) != 0;
      for (uint32_t i = 1; i < packet_size; ++i) {
        const uint32_t target = index + (one_register ? 0 : i - 1);
        if (target >= gpu::RegisterFile::kRegisterCount) {
          return false;
        }
        registers.values[target] =
            xe::load_and_swap<uint32_t>(commands_be + uint64_t(cursor + i) * 4);
        known[target] = true;
      }
    } else if (type == 3) {
      const auto opcode = (packet >> 8) & 0x7F;
      using namespace xe::gpu::xenos;
      const bool draw = opcode == PM4_DRAW_INDX || opcode == PM4_DRAW_INDX_2 ||
                        opcode == PM4_DRAW_INDX_BIN ||
                        opcode == PM4_DRAW_INDX_2_BIN;
      draws += draw;
      if (require_target && draw && known[XE_GPU_REG_RB_MODECONTROL] &&
          (registers.values[XE_GPU_REG_RB_MODECONTROL] & 7) ==
              uint32_t(EdramMode::kColorDepth) &&
          known[XE_GPU_REG_RB_COLOR_MASK] &&
          (registers.values[XE_GPU_REG_RB_COLOR_MASK] & 0xF) == 0xF &&
          known[XE_GPU_REG_RB_SURFACE_INFO] &&
          (registers.values[XE_GPU_REG_RB_SURFACE_INFO] & 0x3FFF)) {
        bool finite_projection = true;
        for (uint32_t i = XE_GPU_REG_SHADER_CONSTANT_004_X;
             i <= XE_GPU_REG_SHADER_CONSTANT_007_W; ++i) {
          finite_projection &=
              known[i] &&
              std::isfinite(std::bit_cast<float>(registers.values[i]));
        }
        finite_projection &=
            known[XE_GPU_REG_SHADER_CONSTANT_004_X] &&
            known[XE_GPU_REG_SHADER_CONSTANT_005_Y] &&
            std::bit_cast<float>(
                registers.values[XE_GPU_REG_SHADER_CONSTANT_004_X]) > 0.0f &&
            std::bit_cast<float>(
                registers.values[XE_GPU_REG_SHADER_CONSTANT_005_Y]) < 0.0f;
        target_draws += finite_projection;
      }
    }
    ++packets;
    cursor += packet_size;
  }
  XELOGI(
      "Native XUI record probe: actual PM4 address={:08X} DWORDs={} packets={} "
      "draw_packets={}; bounded CPU recording, no GPU submission",
      commands, count, packets, draws);
  if (require_target) {
    XELOGI(
        "Native XUI target probe: color draw packets with finite projection "
        "and RGBA writes={}; diagnostic only, not execution permission",
        target_draws);
  }
  // Preserve the primary raw capture even when the producer's additional
  // diagnostic gate fails, so the failure remains inspectable.
  const bool captured =
      (!require_target || !capture) ? true : capture(owned_commands, commands);
  if (!draws || (require_target && !target_draws)) {
    return false;
  }
  // Exceptions must escape to the recording boundary, which retains the
  // complete native graph rather than continuing native teardown.
  return require_target ? captured
                        : (!capture || capture(owned_commands, commands));
}

class ProbeStorage {
 public:
  explicit ProbeStorage(Memory* memory, uint32_t size)
      : memory_(memory), address_(memory->SystemHeapAlloc(size)) {}
  ~ProbeStorage() {
    if (address_ && !retained_) {
      memory_->SystemHeapFree(address_);
    }
  }
  uint32_t address() const { return address_; }
  void RetainUntilTeardown() { retained_ = true; }
  void AllowFreeAfterVerifiedTeardown() { retained_ = false; }

 private:
  Memory* memory_;
  uint32_t address_;
  bool retained_ = false;
};

struct ProbeExport {
  uint16_t ordinal;
  const char* name;
  uint32_t address = 0;
};

}  // namespace

X_HRESULT RunNativeXuiRenderProbe(KernelState* kernel_state,
                                  const object_ref<UserModule>& module,
                                  const NativeXuiRenderProbeSpec& spec) {
  if (!kernel_state || !module || !XThread::IsInThread()) {
    XELOGE("Native XUI render probe: requires a SYSTEM guest thread");
    return X_E_INVALIDARG;
  }
  auto* thread = XThread::GetCurrentThread();
  if (!thread || thread->kernel_state() != kernel_state ||
      module->kernel_state() != kernel_state ||
      thread->guest_object<X_KTHREAD>()->process_type != X_PROCTYPE_SYSTEM ||
      module->module_format() != UserModule::kModuleFormatXex ||
      module->is_executable() ||
      module.get() == kernel_state->GetExecutableModule().get()) {
    XELOGE("Native XUI render probe: requires SYSTEM thread and separate XAM");
    return X_E_INVALIDARG;
  }
  const auto name = module->bounding_filename();
  if (name != "xam" && name != "$flash_xam" && module->name() != "xam" &&
      module->name() != "$flash_xam") {
    XELOGE("Native XUI render probe: module is not guest XAM");
    return X_E_INVALIDARG;
  }
  ProbeOperation operation;
  if (!operation.acquired()) {
    return X_HRESULT_FROM_WIN32(X_ERROR_BUSY);
  }

  // The registry owns the module generation until full teardown; neither the
  // legacy attachment compatibility getter nor resource loading proves this.
  auto runtime = kernel_state->GetGuestRuntime(module->hmodule_ptr());
  if (!runtime) {
    XELOGE("Native XUI render probe: no resident native runtime");
    return X_E_FAIL;
  }
  const auto snapshot = runtime->GetSnapshot();
  if (snapshot.state != GuestRuntime::State::kReady ||
      snapshot.initialization_active ||
      snapshot.identity.hmodule != module->hmodule_ptr()) {
    XELOGE("Native XUI render probe: native runtime is not Ready");
    return X_E_FAIL;
  }

  const auto* image = module->xex_module();
  const uint32_t image_base = image->base_address();
  const uint64_t image_end = uint64_t(image_base) + image->image_size();
  auto* memory = kernel_state->memory();
  if (!image->loaded() || !image_base || image_end > 0x100000000ull) {
    return X_E_FAIL;
  }
  const auto image_range = [&](uint32_t address, uint32_t size, bool code) {
    if (!size || (address & 3) || address < image_base ||
        uint64_t(address) + size > image_end ||
        memory->QueryRangeAccess(address, size) ==
            xe::memory::PageAccess::kNoAccess) {
      return false;
    }
    for (uint64_t current = address; current < uint64_t(address) + size;
         current = (current & ~uint64_t(0xFFF)) + 0x1000) {
      xex2_section_type section;
      if (!image->GetPageSectionType(uint32_t(current), &section) ||
          (code ? section != XEX_SECTION_CODE
                : (section != XEX_SECTION_DATA &&
                   section != XEX_SECTION_READONLY_DATA))) {
        return false;
      }
    }
    return true;
  };

  constexpr std::array<uint64_t, 6> kFactoryArguments = {0, 2, 0, 0, 0, 0};
  if (spec.factory.identity.empty() || spec.factory.abi_metadata.empty() ||
      spec.factory.return_kind != GuestRuntime::ReturnKind::kHResult ||
      spec.factory.arguments.size() != kFactoryArguments.size() ||
      !std::equal(kFactoryArguments.begin(), kFactoryArguments.end(),
                  spec.factory.arguments.begin()) ||
      !image_range(spec.factory.address, 4, true) ||
      kernel_state->GetUserModuleForAddress(spec.factory.address).get() !=
          module.get() ||
      spec.preflight.size() < 3 || spec.preflight.size() > 256) {
    XELOGE("Native XUI render probe: invalid validated factory/ownership spec");
    return X_E_INVALIDARG;
  }
  std::unordered_set<uint32_t> check_addresses;
  size_t zero_checks = 0;
  for (const auto& check : spec.preflight) {
    if (!image_range(check.address, 4, false) ||
        !check_addresses.insert(check.address).second) {
      return X_E_INVALIDARG;
    }
    zero_checks += !check.nonzero && check.expected == 0;
  }
  if (zero_checks < 3) {
    XELOGE("Native XUI render probe: missing three unowned-state checks");
    return X_E_INVALIDARG;
  }
  const auto check_preflight = [&] {
    for (const auto& check : spec.preflight) {
      if (!image_range(check.address, 4, false)) {
        return false;
      }
      const auto actual =
          xe::load_and_swap<uint32_t>(memory->TranslateVirtual(check.address));
      XELOGI(
          "Native XUI render probe: preflight address={:08X} actual={:08X} "
          "expected={:08X} nonzero={}",
          check.address, actual, check.expected, check.nonzero);
      if (check.nonzero ? !actual : actual != check.expected) {
        return false;
      }
    }
    return true;
  };
  if (!check_preflight()) {
    XELOGE("Native XUI render probe: existing native owner/precondition");
    return X_HRESULT_FROM_WIN32(X_ERROR_BUSY);
  }
  std::optional<RecordLayout> record_layout;
  std::optional<TargetLayout> target_layout;
  if (spec.draw_scene && !spec.record) {
    return X_E_INVALIDARG;
  }
  if (spec.retain_native_objects && (!spec.record || !spec.draw_scene)) {
    return X_E_INVALIDARG;
  }
  if (spec.target && !spec.record) {
    return X_E_INVALIDARG;
  }
  if (spec.prepare_device && !spec.retain_native_objects) {
    return X_E_INVALIDARG;
  }
  if (spec.seal_record &&
      (!spec.retain_native_objects || !spec.record || !spec.target)) {
    return X_E_INVALIDARG;
  }
  const auto validate = [&](const GuestRuntime::Function& function,
                            std::initializer_list<uint64_t> arguments,
                            GuestRuntime::ReturnKind kind) {
    return !function.identity.empty() && !function.abi_metadata.empty() &&
           function.return_kind == kind &&
           function.arguments.size() == arguments.size() &&
           std::equal(arguments.begin(), arguments.end(),
                      function.arguments.begin()) &&
           image_range(function.address, 4, true) &&
           kernel_state->GetUserModuleForAddress(function.address).get() ==
               module.get();
  };
  if (spec.record) {
    using Kind = GuestRuntime::ReturnKind;
    if (!validate(spec.record->create_resource, {0}, Kind::kUInt32) ||
        !validate(spec.record->set_buffer, {0, 0, 0}, Kind::kVoid) ||
        !validate(spec.record->begin, {0, 0, 0x10, 0, 0, 0, 0}, Kind::kVoid) ||
        !validate(spec.record->end, {0}, Kind::kHResult) ||
        !(record_layout =
              ParseRecordLayout(spec.record->create_resource.abi_metadata))) {
      XELOGE("Native XUI record probe: invalid validated entries/layout");
      return X_E_INVALIDARG;
    }
  }
  if (spec.target) {
    using Kind = GuestRuntime::ReturnKind;
    const auto& target = *spec.target;
    if (!validate(target.create_surface, {640, 480, 0x18280186, 0, 0},
                  Kind::kUInt32) ||
        !validate(target.get_viewport, {0, 0}, Kind::kVoid) ||
        !validate(target.get_color_write_mask, {0}, Kind::kUInt32) ||
        !validate(target.texture_header,
                  {640, 480, 1, 0, 0x18280106, 0, 0, 0, 0, 0, 0, 0},
                  Kind::kUInt32) ||
        !(target_layout =
              ParseTargetLayout(target.create_surface.abi_metadata))) {
      XELOGE("Native XUI target probe: invalid validated entries/layout");
      return X_E_INVALIDARG;
    }
  }

  enum ExportIndex {
    kPng,
    kInit,
    kCreateDc,
    kSize,
    kDestroyDc,
    kUninit,
    kRelease,
    kEngineInit,
    kEngineUninit,
    kCreateBrush,
    kSelectBrush,
    kFillRect,
    kBeginRender,
    kEndRender,
    kDestroyBrush,
    kAllocBuffer,
    kFreeBuffer,
    kReleaseResource,
    kSetTarget,
    kSetViewport,
    kGetViewport,
    kClear,
    kResolve,
    kTextureHeader,
    kOffsetTexture,
    kAllocPixels
  };
  std::array<ProbeExport, 26> exports = {{{0x29A, "XuiPNGTextureLoader"},
                                          {0x352, "XuiRenderInitShared"},
                                          {0x34C, "XuiRenderCreateDC"},
                                          {0x350, "XuiRenderGetBackBufferSize"},
                                          {0x34E, "XuiRenderDestroyDC"},
                                          {0x355, "XuiRenderUninit"},
                                          {0x8AB, "D3DDevice_Release"},
                                          {0x340, "XuiInit"},
                                          {0x361, "XuiUninit"},
                                          {0x39D, "XuiCreateSolidBrush"},
                                          {0x3B2, "XuiSelectBrush"},
                                          {0x3AD, "XuiFillRect"},
                                          {0x34B, "XuiRenderBegin"},
                                          {0x34F, "XuiRenderEnd"},
                                          {0x38C, "XuiDestroyBrush"},
                                          {0x1EB, "XamAllocEx"},
                                          {0x48A, "XMemFree"},
                                          {0x8AA, "D3DResource_Release"},
                                          {0x8D8, "D3DDevice_SetRenderTarget"},
                                          {0x8A5, "D3DDevice_SetViewport"},
                                          {0x8A6, "D3DDevice_GetViewport"},
                                          {0x8A4, "D3DDevice_Clear"},
                                          {0x8DB, "D3DDevice_Resolve"},
                                          {0x8CF, "XGSetTextureHeader"},
                                          {0x8D1, "XGOffsetResourceAddress"},
                                          {0x489, "XMemAlloc"}}};
  const size_t export_count = spec.target   ? exports.size()
                              : spec.record ? 18
                                            : 7;
  for (size_t index = 0; index < export_count; ++index) {
    auto& entry = exports[index];
    entry.address = module->GetProcAddressByOrdinal(entry.ordinal);
    if (!image_range(entry.address, 4, true) ||
        kernel_state->GetUserModuleForAddress(entry.address).get() !=
            module.get()) {
      XELOGE("Native XUI render probe: invalid export {} ordinal={:03X}",
             entry.name, entry.ordinal);
      return X_E_NOTFOUND;
    }
    XELOGI("Native XUI render probe: export {} ordinal={:03X} address={:08X}",
           entry.name, entry.ordinal, entry.address);
  }
  if (spec.target &&
      (spec.target->get_viewport.address != exports[kGetViewport].address ||
       spec.target->texture_header.address !=
           exports[kTextureHeader].address)) {
    return X_E_INVALIDARG;
  }

  const uint32_t storage_size = spec.target ? kTargetStorageSize : kStorageSize;
  ProbeStorage storage(memory, storage_size);
  const uint32_t scratch = storage.address();
  if (!scratch) {
    return X_HRESULT_FROM_WIN32(X_ERROR_NOT_ENOUGH_MEMORY);
  }
  auto* bytes = memory->TranslateVirtual<uint8_t*>(scratch);
  std::memset(bytes, 0, storage_size);
  auto* words = reinterpret_cast<xe::be<uint32_t>*>(bytes);
  static_assert(kParamsSize <= kDeviceOffset);
  words[0] = 640;
  words[1] = 480;

  auto* thread_state = thread->thread_state();
  const auto invoke_typed = [&](const char* name, uint32_t address,
                                std::span<const cpu::GuestCallArgument> args,
                                uint32_t* result) {
    XELOGI("Native XUI render probe: calling {} address={:08X}", name, address);
    storage.RetainUntilTeardown();
    const auto call = cpu::ExecuteGuestCall(kernel_state->processor(),
                                            thread_state, address, args);
    *result = uint32_t(call.value);
    XELOGI("Native XUI render probe: {} executed={} result={:08X}", name,
           call.executed, *result);
    return call.executed;
  };
  const auto invoke = [&](const char* name, uint32_t address,
                          std::initializer_list<uint32_t> arguments,
                          uint32_t* result) {
    std::array<cpu::GuestCallArgument, cpu::kGuestCallMaxArguments> packed{};
    if (arguments.size() > packed.size()) {
      return false;
    }
    size_t index = 0;
    for (const uint32_t argument : arguments) {
      packed[index++] = cpu::GuestCallArgument::UInt32(argument);
    }
    // A guest call may exit its host thread through POSIX forced unwind,
    // which need not affect std::uncaught_exceptions(). Arm ownership before
    // execution and release only after the complete native teardown gate.
    return invoke_typed(name, address,
                        {packed.data(), std::max(arguments.size(), size_t(8))},
                        result);
  };
  const auto invoke_export = [&](ExportIndex index,
                                 std::initializer_list<uint32_t> args,
                                 uint32_t* result) {
    return invoke(exports[index].name, exports[index].address, args, result);
  };
  const auto writable_object = [&](uint32_t address) {
    return address && !(address & 3) &&
           memory->QueryRangeAccess(address, 4) ==
               xe::memory::PageAccess::kReadWrite;
  };
  const auto incomplete = [&] {
    // A failed CPU execution may have partially published native objects.
    // Preserve scratch too rather than guess what the interrupted call kept.
    storage.RetainUntilTeardown();
    XELOGE(
        "Native XUI render probe: execution/ownership incomplete; retain "
        "allocations until isolated process teardown, no in-place retry");
    return X_E_FAIL;
  };

  uint32_t call_result = 0;
  if (!invoke(spec.factory.identity.c_str(), spec.factory.address,
              {0, 2, 0, 0, 0, scratch + kDeviceOffset}, &call_result)) {
    return incomplete();
  }
  const uint32_t device = words[kDeviceOffset / 4];
  XELOGI("Native XUI render probe: device={:08X} HRESULT={:08X}", device,
         call_result);
  if (int32_t(call_result) < 0) {
    return device ? incomplete() : call_result;
  }
  if (!writable_object(device)) {
    return incomplete();
  }
  if (spec.prepare_device) {
    if (call_result != X_E_SUCCESS) {
      return incomplete();
    }
    try {
      const X_HRESULT prepared = spec.prepare_device(device);
      if (prepared != X_E_SUCCESS) {
        storage.RetainUntilTeardown();
        XELOGE(
            "Native XUI render probe: owned device preparation failed "
            "HRESULT={:08X}; retaining native graph without cleanup",
            prepared);
        return int32_t(prepared) < 0 ? prepared : X_E_FAIL;
      }
    } catch (const std::exception& error) {
      XELOGE("Native XUI render probe: device preparation interrupted: {}",
             error.what());
      return incomplete();
    }
  }
  X_HRESULT result = call_result == X_E_SUCCESS ? X_E_SUCCESS : X_E_FAIL;
  bool renderer_owned = false;
  uint32_t dc = 0;
  bool engine_owned = false;
  uint32_t brush = 0;

  // Recheck immediately before InitShared, which replaces an old renderer on
  // success. Other native UI admission remains the caller's responsibility.
  if (!check_preflight()) {
    result = X_HRESULT_FROM_WIN32(X_ERROR_BUSY);
  } else if (!invoke_export(kInit, {device, scratch, exports[kPng].address},
                            &call_result)) {
    return incomplete();
  } else if (int32_t(call_result) < 0) {
    result = call_result;
  } else {
    renderer_owned = true;
    if (call_result != X_E_SUCCESS) {
      result = X_E_FAIL;
    }
    if (!invoke_export(kCreateDc, {scratch + kDcOffset}, &call_result)) {
      return incomplete();
    }
    dc = words[kDcOffset / 4];
    XELOGI("Native XUI render probe: DC={:08X} HRESULT={:08X}", dc,
           call_result);
    if (int32_t(call_result) < 0) {
      if (dc) {
        return incomplete();
      }
      result = call_result;
    } else if (!writable_object(dc)) {
      return incomplete();
    } else {
      if (call_result != X_E_SUCCESS) {
        result = X_E_FAIL;
      }
      if (!invoke_export(kSize,
                         {dc, scratch + kWidthOffset, scratch + kHeightOffset},
                         &call_result)) {
        return incomplete();
      }
      const uint32_t width = words[kWidthOffset / 4];
      const uint32_t height = words[kHeightOffset / 4];
      XELOGI("Native XUI render probe: size={}x{} HRESULT={:08X}", width,
             height, call_result);
      if (int32_t(call_result) < 0) {
        result = call_result;
      } else if (call_result != X_E_SUCCESS || width != 640 || height != 480) {
        result = X_E_FAIL;
      }
    }
  }

  if (result == X_E_SUCCESS && spec.record) {
    const auto& recording = *spec.record;
    const auto& layout = *record_layout;
    const auto invoke_function = [&](const GuestRuntime::Function& function,
                                     std::initializer_list<uint32_t> args) {
      return invoke(function.identity.c_str(), function.address, args,
                    &call_result);
    };
    const auto run_record = [&]() -> bool {
      words[kXuiParamsOffset / 4] = 12;
      if (!invoke_export(kEngineInit, {scratch + kXuiParamsOffset},
                         &call_result)) {
        return false;
      }
      if (int32_t(call_result) < 0) {
        result = call_result;
        return true;  // Actual XuiInit unwinds its own failed initialization.
      }
      if (call_result != 0) {
        return false;  // Existing/unknown engine ownership: do not Uninit it.
      }
      engine_owned = true;
      if (!spec.draw_scene) {
        if (!invoke_export(kCreateBrush, {0xFF00FF00, scratch + kBrushOffset},
                           &call_result)) {
          return false;
        }
        brush = words[kBrushOffset / 4];
        XELOGI("Native XUI record probe: solid brush={:08X} HRESULT={:08X}",
               brush, call_result);
        if (int32_t(call_result) < 0) {
          if (brush) {
            return false;
          }
          result = call_result;
          return true;
        }
        if (call_result != 0 || !writable_object(brush)) {
          return false;
        }
      }

      uint32_t resource = 0;
      uint32_t buffer = 0;
      uint32_t surface = 0;
      uint32_t pixels = 0;
      uint32_t pixel_bytes = 0;
      uint32_t target_fetch_address = 0;
      bool target_bound = false;
      bool resolve_recorded = false;
      bool recording_active = false;
      bool render_active = false;
      bool record_ended = false;
      const auto fresh_external_resource = [&] {
        if (memory->QueryRangeAccess(resource, layout.resource_size) !=
            xe::memory::PageAccess::kReadWrite) {
          return false;
        }
        const uint32_t last_use =
            xe::load_and_swap<uint32_t>(memory->TranslateVirtual(
                resource + layout.resource_last_use_offset));
        const uint32_t flags = xe::load_and_swap<uint32_t>(
            memory->TranslateVirtual(resource + layout.resource_flags_offset));
        return !last_use && (flags & layout.external_buffer_flag);
      };
      do {
        if (!invoke_function(recording.create_resource, {0})) {
          return false;
        }
        resource = call_result;
        XELOGI("Native XUI record probe: command resource={:08X}", resource);
        if (!resource) {
          result = X_HRESULT_FROM_WIN32(X_ERROR_NOT_ENOUGH_MEMORY);
          break;
        }
        if (!fresh_external_resource()) {
          return false;
        }
        // Actual native UI caller: physical attributes, direct allocation,
        // byte size, output DWORD. Keep allocator/free ownership paired.
        if (!invoke_export(kAllocBuffer,
                           {0xB0010000, 0x40000000, kRecordBufferSize,
                            scratch + kBufferOffset},
                           &call_result)) {
          return false;
        }
        buffer = words[kBufferOffset / 4];
        XELOGI(
            "Native XUI record probe: recording buffer={:08X} HRESULT={:08X}",
            buffer, call_result);
        if (int32_t(call_result) < 0) {
          if (buffer) {
            return false;
          }
          result = call_result;
          break;
        }
        const auto* buffer_heap = memory->LookupHeap(buffer);
        if (call_result != 0 || !buffer_heap ||
            buffer_heap->heap_type() != HeapType::kGuestPhysical ||
            memory->QueryRangeAccess(buffer, kRecordBufferSize) !=
                xe::memory::PageAccess::kReadWrite) {
          return false;
        }
        if (spec.target) {
          const auto& target = *spec.target;
          const auto& target_info = *target_layout;
          if (!invoke_function(target.get_color_write_mask, {device}) ||
              call_result != 0xF) {
            XELOGE("Native XUI target probe: fresh color-write mask is not F");
            return false;
          }
          if (!invoke_function(target.create_surface,
                               {640, 480, 0x18280186, 0, 0})) {
            return false;
          }
          surface = call_result;
          if (!surface) {
            result = X_HRESULT_FROM_WIN32(X_ERROR_NOT_ENOUGH_MEMORY);
            break;
          }
          if ((surface & 3) ||
              uint64_t(surface) + target_info.resource_size > 0x100000000ull ||
              memory->QueryRangeAccess(surface, target_info.resource_size) !=
                  xe::memory::PageAccess::kReadWrite) {
            return false;
          }
          const uint32_t type_flags =
              xe::load_and_swap<uint32_t>(memory->TranslateVirtual(
                  surface + target_info.resource_type_offset));
          const uint32_t references =
              xe::load_and_swap<uint32_t>(memory->TranslateVirtual(
                  surface + target_info.resource_refcount_offset));
          if ((type_flags & target_info.resource_type_mask) !=
                  target_info.resource_type_value ||
              (type_flags & target_info.resource_owned_flags) !=
                  target_info.resource_owned_flags ||
              references != 1) {
            return false;
          }
          XELOGI(
              "Native XUI target probe: owned native surface={:08X} "
              "flags={:08X} references={}; no GPU submission",
              surface, type_flags, references);
          const uint32_t header = scratch + kTargetTextureHeaderOffset;
          if (!invoke_function(
                  target.texture_header,
                  {640, 480, 1, 0, 0x18280106, 0, 0, 0, 0, header, 0, 0})) {
            return false;
          }
          pixel_bytes = call_result;
          if (!pixel_bytes || pixel_bytes > kMaxResolveBytes) {
            return false;
          }
          if (!invoke_export(kAllocPixels, {pixel_bytes, 0xB0010000},
                             &call_result)) {
            return false;
          }
          pixels = call_result;
          if (!pixels) {
            result = X_HRESULT_FROM_WIN32(X_ERROR_NOT_ENOUGH_MEMORY);
            break;
          }
          const auto* pixels_heap = memory->LookupHeap(pixels);
          if (!pixels_heap || uint64_t(pixels) + pixel_bytes > 0x100000000ull ||
              pixels_heap->heap_type() != HeapType::kGuestPhysical ||
              memory->QueryRangeAccess(pixels, pixel_bytes) !=
                  xe::memory::PageAccess::kReadWrite) {
            return false;
          }
          const uint32_t physical_pixels = memory->GetPhysicalAddress(pixels);
          if ((physical_pixels & 0xFFF) ||
              uint64_t(physical_pixels) + pixel_bytes > 0x20000000ull ||
              !invoke_export(kOffsetTexture, {header, pixels}, &call_result)) {
            return false;
          }
          target_fetch_address = scratch + kTargetTextureHeaderOffset +
                                 target_info.texture_fetch_offset;
          gpu::xenos::xe_gpu_texture_fetch_t fetch{};
          std::array<uint32_t, 6> fetch_words;
          for (size_t i = 0; i < fetch_words.size(); ++i) {
            fetch_words[i] = xe::load_and_swap<uint32_t>(
                bytes + kTargetTextureHeaderOffset +
                target_info.texture_fetch_offset + i * 4);
          }
          std::memcpy(&fetch, fetch_words.data(), sizeof(fetch));
          XELOGI(
              "Native XUI target probe: native texture header={:08X} "
              "pixels={:08X} physical={:08X} bytes={} "
              "fetch={:08X},{:08X},{:08X},{:08X},{:08X},{:08X}; "
              "type={} format={} dimension={} stacked={} width={} height={} "
              "pitch={} masked_cpu_alias={:08X}",
              header, pixels, physical_pixels, pixel_bytes, fetch_words[0],
              fetch_words[1], fetch_words[2], fetch_words[3], fetch_words[4],
              fetch_words[5], uint32_t(fetch.type), uint32_t(fetch.format),
              uint32_t(fetch.dimension), uint32_t(fetch.stacked),
              uint32_t(fetch.size_2d.width) + 1,
              uint32_t(fetch.size_2d.height) + 1, uint32_t(fetch.pitch) << 5,
              gpu::xenos::CpuToGpu(fetch.base_address << 12));
          if (fetch.type != gpu::xenos::FetchConstantType::kTexture ||
              fetch.format != gpu::xenos::TextureFormat::k_8_8_8_8 ||
              fetch.dimension != gpu::xenos::DataDimension::k2DOrStacked ||
              fetch.stacked || fetch.size_2d.width != 639 ||
              fetch.size_2d.height != 479 || (fetch.pitch << 5) < 640 ||
              // This is the native CPU resource header, not a ready GPU
              // fetch. XGOffsetResourceAddress retains the allocation alias;
              // native Resolve translates its E/F-window skew when recording
              // the final physical destination. Do not relocate it twice.
              (fetch.base_address << 12) != (pixels & ~uint32_t(0xFFF))) {
            return false;
          }
          auto* viewport = reinterpret_cast<xe::be<uint32_t>*>(
              bytes + kTargetViewportOffset);
          viewport[0] = 0;
          viewport[1] = 0;
          viewport[2] = 640;
          viewport[3] = 480;
          viewport[4] = std::bit_cast<uint32_t>(0.0f);
          viewport[5] = std::bit_cast<uint32_t>(1.0f);
          auto* resolve_rect = reinterpret_cast<xe::be<uint32_t>*>(
              bytes + kTargetResolveRectOffset);
          resolve_rect[0] = 0;
          resolve_rect[1] = 0;
          resolve_rect[2] = 640;
          resolve_rect[3] = 480;
        }
        // The exact native producer zeroes forty bytes and sets this DWORD.
        // This scratch block is an input, never an overwrite of boot globals.
        words[(kSavedStateOffset + 0x1C) / 4] = 0x10000;
        auto* rect = reinterpret_cast<xe::be<float>*>(bytes + kRectOffset);
        rect[0] = 32.0f;
        rect[1] = 32.0f;
        rect[2] = 160.0f;
        rect[3] = 112.0f;
        if (!invoke_function(recording.set_buffer,
                             {resource, buffer, kRecordBufferSize}) ||
            !invoke_function(recording.begin,
                             {device, resource, 0x10,
                              scratch + kSavedStateOffset, 0, 0, 0})) {
          return false;
        }
        recording_active = true;
        if (spec.target) {
          if (!invoke_export(kSetTarget, {device, 0, surface}, &call_result)) {
            return false;
          }
          target_bound = true;
          // Native SetViewport is a no-op without a target. Shared Begin
          // reads this device viewport, not InitShared's logical dimensions.
          if (!invoke_export(kSetViewport,
                             {device, scratch + kTargetViewportOffset},
                             &call_result) ||
              !invoke_function(
                  spec.target->get_viewport,
                  {device, scratch + kTargetViewportReadbackOffset}) ||
              std::memcmp(bytes + kTargetViewportOffset,
                          bytes + kTargetViewportReadbackOffset, 24)) {
            return false;
          }
          XELOGI(
              "Native XUI target probe: actual viewport 0,0,640,480 "
              "Z=0..1; native Begin will derive finite projection");
          using Arg = cpu::GuestCallArgument;
          const std::array clear_args = {
              Arg::UInt32(device),
              Arg::UInt32(0),
              Arg::UInt32(0),
              Arg::UInt32(0xF),
              Arg::UInt32(spec.draw_scene ? 0 : 0xFF000000),
              Arg::Float32(1.0f),
              Arg::UInt32(0)};
          // Shared XuiRenderBegin skips its backend clear. Float logical6
          // occupies f1 while stencil logical7 occupies r9, not r8.
          if (!invoke_typed(exports[kClear].name, exports[kClear].address,
                            clear_args, &call_result)) {
            return false;
          }
        }
        if (!spec.draw_scene) {
          if (!invoke_export(kBeginRender, {dc, 0xFF000000}, &call_result)) {
            return false;
          }
          if (call_result != 0) {
            result = int32_t(call_result) < 0 ? call_result : X_E_FAIL;
            break;
          }
          render_active = true;
          if (!invoke_export(kSelectBrush, {dc, brush}, &call_result)) {
            return false;
          }
          if (call_result != 0) {
            result = int32_t(call_result) < 0 ? call_result : X_E_FAIL;
            break;
          }
          if (!invoke_export(kFillRect, {dc, scratch + kRectOffset},
                             &call_result)) {
            return false;
          }
          if (call_result != 0) {
            result = int32_t(call_result) < 0 ? call_result : X_E_FAIL;
            break;
          }
        }
      } while (false);

      // Unwind completed owned stages, including a failed draw operation.
      // Failed execution itself cannot establish a safe unwind point.
      if (render_active) {
        if (!invoke_export(kEndRender, {dc}, &call_result) ||
            call_result != 0) {
          return false;
        }
      }
      // Actual scene/application paint methods may own their own Begin/End;
      // do not nest them inside the primitive's render interval.
      if (result == X_E_SUCCESS && recording_active && spec.draw_scene) {
        try {
          const auto scene_result = spec.draw_scene(dc);
          XELOGI("Native XUI record probe: scene callback HRESULT={:08X}",
                 scene_result);
          if (scene_result != 0) {
            result = int32_t(scene_result) < 0 ? scene_result : X_E_FAIL;
          }
        } catch (const std::exception& error) {
          XELOGE("Native XUI record probe: scene ownership interrupted: {}",
                 error.what());
          return false;
        }
      }
      if (recording_active) {
        if (result == X_E_SUCCESS && spec.target) {
          using Arg = cpu::GuestCallArgument;
          const std::array resolve_args = {
              Arg::UInt32(device),
              Arg::UInt32(0),
              Arg::UInt32(scratch + kTargetResolveRectOffset),
              Arg::UInt32(scratch + kTargetTextureHeaderOffset),
              Arg::UInt32(0),
              Arg::UInt32(0),
              Arg::UInt32(0),
              Arg::UInt32(0),
              Arg::Float32(0.0f),
              Arg::UInt32(0),
              Arg::UInt32(0)};
          // The actual resolve has f1 at logical9, then DWORDs at10/11.
          // GuestCallABI supplies stack homes +5C/+64, never +54/+5C.
          if (!invoke_typed(exports[kResolve].name, exports[kResolve].address,
                            resolve_args, &call_result)) {
            return false;
          }
          resolve_recorded = true;
        }
        if (!invoke_function(recording.end, {device})) {
          return false;
        }
        record_ended = call_result == 0;
        if (!record_ended) {
          result = int32_t(call_result) < 0 ? call_result : X_E_FAIL;
        }
      }
      auto initial_record_pin =
          std::make_shared<std::shared_ptr<const memory::PhysicalRangePin>>();
      if (record_ended && spec.seal_record) {
        const auto session = thread->native_frame_owner_session();
        const memory::PhysicalRange range{memory->GetPhysicalAddress(buffer),
                                          kRecordBufferSize};
        if (!session) {
          return false;
        }
        const auto pin = memory->PinOwnedPhysicalRanges(
            session->owner(),
            std::span<const memory::PhysicalRange>(&range, 1));
        if (!pin) {
          XELOGE(
              "Native frame lease: pre-copy record pin refused status={} "
              "physical={:08X} bytes={}",
              unsigned(pin.status), range.address, range.length);
          return false;
        }
        *initial_record_pin = pin.pin;
        kernel_state->RetainNativeResourceUntilTeardown(initial_record_pin);
      }
      const auto capture_record = [&](std::span<const uint8_t> primary,
                                      uint32_t primary_physical_address) {
        if (spec.seal_record) {
          if (result != X_E_SUCCESS || !record_ended || !resolve_recorded ||
              !pixels || !pixel_bytes) {
            return false;
          }
          const auto* record_heap = memory->LookupHeap(buffer);
          const auto* output_heap = memory->LookupHeap(pixels);
          if (!record_heap || !output_heap ||
              record_heap->heap_type() != HeapType::kGuestPhysical ||
              output_heap->heap_type() != HeapType::kGuestPhysical) {
            return false;
          }
          if (!target_fetch_address) {
            return false;
          }
          std::array<uint32_t, 6> target_fetch;
          for (size_t i = 0; i < target_fetch.size(); ++i) {
            target_fetch[i] =
                xe::load_and_swap<uint32_t>(memory->TranslateVirtual(
                    target_fetch_address + uint32_t(i) * 4));
          }
          const NativeCompletedFrameRecord completed{
              primary,
              device,
              dc,
              static_cast<const PhysicalHeap*>(record_heap)
                  ->GetPhysicalAddress(buffer),
              kRecordBufferSize,
              static_cast<const PhysicalHeap*>(output_heap)
                  ->GetPhysicalAddress(pixels),
              pixel_bytes,
              true,
              primary_physical_address,
              pixels,
              target_fetch};
          return spec.seal_record(completed);
        }
        return !spec.capture_record || spec.capture_record(primary);
      };
      const bool checked_record =
          record_ended && CheckRecordedDraws(memory, resource, buffer, layout,
                                             capture_record, bool(spec.target));
      if (record_ended && !checked_record) {
        if (result == X_E_SUCCESS) {
          result = X_E_FAIL;
        }
      }
      if (checked_record) {
        // The completed callback either checked/aborted this CPU-only frame or
        // transferred its own typed pins. Failed/unknown paths leave this
        // independent first-copy guard with Kernel teardown.
        initial_record_pin->reset();
      }
      if (spec.retain_native_objects) {
        XELOGI(
            "Native XUI record probe: kernel-resident record ended={} "
            "HRESULT={:08X}; retaining resource/buffer with UI owner, "
            "frame execution evidence is reported by the seal callback",
            record_ended, result);
        return true;
      }
      // Even unpublished targets must be detached before final Release. End
      // normally restores bindings, but explicitly close our borrowed slot.
      if (target_bound &&
          (!record_ended ||
           !invoke_export(kSetTarget, {device, 0, 0}, &call_result))) {
        return false;
      }
      if (resource) {
        // A fresh never-published resource has no prior GPU use to wait on.
        // Its external-buffer flag prevents Release from freeing our buffer.
        if (!fresh_external_resource() ||
            !invoke_export(kReleaseResource, {resource}, &call_result) ||
            call_result != 0) {
          return false;
        }
      }
      if (buffer &&
          !invoke_export(kFreeBuffer, {buffer, 0xB0010000}, &call_result)) {
        return false;
      }
      if (surface &&
          (!invoke_export(kReleaseResource, {surface}, &call_result) ||
           call_result != 0)) {
        return false;
      }
      if (pixels &&
          !invoke_export(kFreeBuffer, {pixels, 0xB0010000}, &call_result)) {
        return false;
      }
      XELOGI(
          "Native XUI record probe: record ended={} HRESULT={:08X}; no "
          "GPU execution or publication requested; target_gate={} "
          "resolve_recorded={}",
          record_ended, result, bool(spec.target), resolve_recorded);
      return true;
    };
    try {
      if (!run_record()) {
        return incomplete();
      }
    } catch (const std::exception& error) {
      // This includes host capture failures. Do not run any further native
      // cleanup after an exception; unknown exceptions and forced unwind
      // propagate while ProbeStorage remains conservatively retained.
      XELOGE("Native XUI record probe: recording interrupted: {}",
             error.what());
      return incomplete();
    }
  }

  if (spec.retain_native_objects) {
    storage.RetainUntilTeardown();
    XELOGI(
        "Native XUI render probe: kernel-resident HRESULT={:08X}; "
        "retaining complete native UI graph until kernel/memory teardown; "
        "frame execution evidence is reported separately",
        result);
    return result;
  }

  // DestroyDC releases both the caller's reference and, when matching, the
  // separately retained default-DC reference. RenderUninit alone does not.
  if (dc &&
      (!invoke_export(kDestroyDc, {dc}, &call_result) || call_result != 0)) {
    return incomplete();
  }
  // The selected solid brush is borrowed by the DC; destroy the DC first.
  if (brush && (!invoke_export(kDestroyBrush, {brush}, &call_result) ||
                call_result != 0)) {
    return incomplete();
  }
  if (engine_owned &&
      (!invoke_export(kEngineUninit, {}, &call_result) || call_result != 0)) {
    return incomplete();
  }
  if (renderer_owned &&
      (!invoke_export(kUninit, {}, &call_result) || call_result != 0)) {
    return incomplete();
  }
  // Release returns a reference count, not an HRESULT. A remaining owner is
  // evidence to retain, never permission to loop Release or force-free it.
  if (!invoke_export(kRelease, {device}, &call_result)) {
    return incomplete();
  }
  if (call_result != 0) {
    XELOGE("Native XUI render probe: device retained references={}",
           call_result);
    return incomplete();
  }
  if (!check_preflight()) {
    XELOGE("Native XUI render probe: ownership state not restored");
    return incomplete();
  }
  XELOGI(
      "Native XUI render probe: complete HRESULT={:08X}; actual device/DC "
      "lifecycle, record_gate={}; no GPU submission, publication or pixels",
      result, bool(spec.record));
  storage.AllowFreeAfterVerifiedTeardown();
  return result;
}

}  // namespace xe::kernel
