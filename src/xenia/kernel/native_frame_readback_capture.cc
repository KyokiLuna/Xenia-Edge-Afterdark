/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia contributors. All rights reserved.                    *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */
#include "xenia/kernel/native_frame_readback_capture.h"
#include <algorithm>
#include <bit>
#include <bitset>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include "third_party/fmt/include/fmt/format.h"
#include "xenia/base/filesystem.h"
#include "xenia/base/mutex.h"
#include "xenia/gpu/command_processor.h"
#include "xenia/gpu/native_draw_read_inventory.h"
#include "xenia/gpu/register_file.h"
#include "xenia/gpu/texture_address.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/native_frame_lease_probe.h"
#include "xenia/kernel/native_frame_owner.h"
#include "xenia/memory.h"
#if !XE_PLATFORM_WIN32
#include <sys/stat.h>
#include <unistd.h>
#endif
namespace xe::kernel {
namespace {
using Inventory = gpu::NativeDrawReadInventory;
using Plan = gpu::NativeFrameCommandPlan;
bool Contains(memory::PhysicalRange outer, memory::PhysicalRange inner) {
  return inner.length && inner.address >= outer.address &&
         uint64_t(inner.address) + inner.length <=
             uint64_t(outer.address) + outer.length;
}
using File = std::unique_ptr<FILE, decltype(&std::fclose)>;
File OpenPrivate(const std::filesystem::path& path) {
  File file(filesystem::OpenFile(path, "wbx"), &std::fclose);
#if !XE_PLATFORM_WIN32
  if (file && ::fchmod(::fileno(file.get()), S_IRUSR | S_IWUSR)) {
    file.reset();
  }
#endif
  return file;
}
bool Close(File& file) {
  if (!file || std::fflush(file.get())) {
    return false;
  }
  return std::fclose(file.release()) == 0;
}
}  // namespace
std::optional<NativeFrameReadbackLayout> SnapshotNativeFrameReadbackLayout(
    Memory* guest_memory, const gpu::NativeFrameCommandPlan& plan,
    std::span<const memory::PhysicalRange> inputs,
    const NativeCompletedFrameRecord& record) {
  if (!guest_memory || !record.native_end_completed ||
      !record.output_cpu_address ||
      guest_memory->GetPhysicalAddress(record.output_cpu_address) !=
          record.output_physical_address ||
      (record.output_physical_address & 0xFFF) ||
      record.output_physical_address >= 0x20000000 ||
      !record.output_allocation_bytes ||
      record.output_allocation_bytes >
          0x20000000 - record.output_physical_address ||
      !plan.has_final_copy() || plan.inventory().draws.empty()) {
    return {};
  }
  NativeFrameReadbackLayout layout;
  layout.texture_fetch = record.output_texture_fetch;
  layout.original_texture_fetch = record.output_texture_fetch;
  gpu::xenos::xe_gpu_texture_fetch_t fetch{};
  std::memcpy(&fetch, layout.texture_fetch.data(), sizeof(fetch));
  layout.width = uint32_t(fetch.size_2d.width) + 1;
  layout.height = uint32_t(fetch.size_2d.height) + 1;
  layout.pitch = uint32_t(fetch.pitch) << 5;
  layout.physical_address = record.output_physical_address;
  // Deliberately bounded current format. Unsupported layouts remain explicit
  // blockers rather than being reinterpreted as ordinary linear RGBA bytes.
  if (fetch.type != gpu::xenos::FetchConstantType::kTexture ||
      fetch.format != gpu::xenos::TextureFormat::k_8_8_8_8 ||
      fetch.dimension != gpu::xenos::DataDimension::k2DOrStacked ||
      fetch.stacked || !fetch.tiled ||
      fetch.sign_x != gpu::xenos::TextureSign::kUnsigned ||
      fetch.sign_y != gpu::xenos::TextureSign::kUnsigned ||
      fetch.sign_z != gpu::xenos::TextureSign::kUnsigned ||
      fetch.sign_w != gpu::xenos::TextureSign::kUnsigned || fetch.num_format ||
      fetch.exp_adjust || ((layout.texture_fetch[1] >> 6) & 3) != 0 ||
      ((layout.texture_fetch[3] >> 1) & 0xFFF) != 0x60A ||
      (fetch.base_address << 12) !=
          (record.output_cpu_address & ~uint32_t(0xFFF)) ||
      fetch.mip_min_level || fetch.mip_max_level || fetch.mip_address ||
      fetch.size_2d.stack_depth || layout.width != 640 ||
      layout.height != 480 || layout.pitch != 640) {
    return {};
  }
  const auto& draw = plan.inventory().draws.back();
  gpu::RegisterFile regs;
  std::bitset<gpu::RegisterFile::kRegisterCount> known;
  for (const auto& write : plan.register_writes()) {
    if (write.packet_dword_offset > draw.packet_dword_offset) {
      break;
    }
    regs.values[write.index] = write.value;
    known.set(write.index);
  }
  for (uint32_t reg :
       {gpu::XE_GPU_REG_RB_SURFACE_INFO, gpu::XE_GPU_REG_RB_MODECONTROL,
        gpu::XE_GPU_REG_RB_COPY_CONTROL, gpu::XE_GPU_REG_RB_COPY_DEST_BASE,
        gpu::XE_GPU_REG_RB_COPY_DEST_INFO, gpu::XE_GPU_REG_RB_COPY_DEST_PITCH,
        gpu::XE_GPU_REG_RB_COPY_SURFACE_SLICE, gpu::XE_GPU_REG_PA_SU_VTX_CNTL,
        gpu::XE_GPU_REG_PA_SC_WINDOW_OFFSET,
        gpu::XE_GPU_REG_PA_SC_WINDOW_SCISSOR_TL,
        gpu::XE_GPU_REG_PA_SC_WINDOW_SCISSOR_BR,
        gpu::XE_GPU_REG_PA_SC_SCREEN_SCISSOR_TL,
        gpu::XE_GPU_REG_PA_SC_SCREEN_SCISSOR_BR}) {
    if (!known[reg]) {
      return {};
    }
  }
  const auto source_surface = regs.Get<gpu::reg::RB_SURFACE_INFO>();
  const auto control = regs.Get<gpu::reg::RB_COPY_CONTROL>();
  const auto info = regs.Get<gpu::reg::RB_COPY_DEST_INFO>();
  const auto pitch = regs.Get<gpu::reg::RB_COPY_DEST_PITCH>();
  const auto tl = regs.Get<gpu::reg::PA_SC_WINDOW_SCISSOR_TL>();
  const auto br = regs.Get<gpu::reg::PA_SC_WINDOW_SCISSOR_BR>();
  const auto screen_tl = regs.Get<gpu::reg::PA_SC_SCREEN_SCISSOR_TL>();
  const auto screen_br = regs.Get<gpu::reg::PA_SC_SCREEN_SCISSOR_BR>();
  if (regs.Get<gpu::reg::RB_MODECONTROL>().edram_mode !=
          gpu::xenos::EdramMode::kCopy ||
      control.copy_command != gpu::xenos::CopyCommand::kConvert ||
      control.copy_src_select || control.color_clear_enable ||
      control.depth_clear_enable ||
      source_surface.surface_pitch != layout.pitch ||
      source_surface.msaa_samples != gpu::xenos::MsaaSamples::k1X ||
      regs.values[gpu::XE_GPU_REG_RB_COPY_DEST_BASE] !=
          layout.physical_address ||
      info.copy_dest_format != gpu::xenos::ColorFormat::k_8_8_8_8 ||
      uint32_t(info.copy_dest_endian) || info.copy_dest_array ||
      info.copy_dest_slice || uint32_t(info.copy_dest_number) ||
      info.copy_dest_exp_bias || !info.copy_dest_swap ||
      pitch.copy_dest_pitch != layout.pitch ||
      pitch.copy_dest_height != layout.height ||
      regs.values[gpu::XE_GPU_REG_RB_COPY_SURFACE_SLICE] ||
      regs.values[gpu::XE_GPU_REG_PA_SC_WINDOW_OFFSET] ||
      regs.Get<gpu::reg::PA_SU_VTX_CNTL>().pix_center !=
          gpu::xenos::PixelCenter::kD3DZero ||
      tl.tl_x || tl.tl_y || br.br_x != layout.width ||
      br.br_y != layout.height || screen_tl.tl_x || screen_tl.tl_y ||
      screen_br.br_x < 0 || screen_br.br_y < 0 ||
      uint32_t(screen_br.br_x) < layout.width ||
      uint32_t(screen_br.br_y) < layout.height) {
    return {};
  }
  layout.copy_dest_info = info.value;
  layout.copy_dest_pitch = pitch.value;
  const auto vertices = Inventory::VertexReads(draw, nullptr);
  if (vertices.error != Inventory::Error::kNone || vertices.reads.size() != 1 ||
      !vertices.reads[0].resolve_rectangle ||
      vertices.reads[0].byte_count != 24) {
    return {};
  }
  const auto& read = vertices.reads[0];
  const memory::PhysicalRange rectangle{read.physical_address, read.byte_count};
  if (!std::any_of(inputs.begin(), inputs.end(),
                   [&](auto range) { return Contains(range, rectangle); })) {
    return {};
  }
  {
    auto lock = global_critical_region::Acquire();
    std::memcpy(layout.rectangle_words.data(),
                guest_memory->TranslatePhysical(rectangle.address), 24);
  }
  std::array<float, 6> coordinates;
  for (size_t i = 0; i < coordinates.size(); ++i) {
    layout.rectangle_words[i] = gpu::xenos::GpuSwap(
        layout.rectangle_words[i], gpu::xenos::Endian(read.endian));
    coordinates[i] = std::bit_cast<float>(layout.rectangle_words[i]) + 0.5f;
    if (!std::isfinite(coordinates[i])) {
      return {};
    }
  }
  const float x0 = std::min({coordinates[0], coordinates[2], coordinates[4]});
  const float x1 = std::max({coordinates[0], coordinates[2], coordinates[4]});
  const float y0 = std::min({coordinates[1], coordinates[3], coordinates[5]});
  const float y1 = std::max({coordinates[1], coordinates[3], coordinates[5]});
  if (x0 != 0.0f || y0 != 0.0f || x1 != float(layout.width) ||
      y1 != float(layout.height)) {
    return {};
  }
  uint64_t required = 0;
  for (uint32_t y = 0; y < layout.height; ++y) {
    for (uint32_t x = 0; x < layout.width; ++x) {
      const int32_t offset =
          gpu::texture_address::Tiled2D(x, y, layout.pitch, 2);
      if (offset < 0) {
        return {};
      }
      required = std::max(required, uint64_t(offset) + 4);
    }
  }
  if (required > record.output_allocation_bytes) {
    return {};
  }
  layout.required_bytes = uint32_t(required);
  fetch.base_address = record.output_physical_address >> 12;
  std::memcpy(layout.texture_fetch.data(), &fetch, sizeof(fetch));
  return layout;
}

std::optional<NativeFrameReadbackLayout> SnapshotNativeFrameReadbackLayout(
    KernelState* kernel, const gpu::NativeFrameExecutionRequest& request,
    const NativeCompletedFrameRecord& record) {
  if (!kernel || !request.IsAdmitted() || !request.output_layout() ||
      request.output_range().address != record.output_physical_address ||
      request.output_range().length != record.output_allocation_bytes) {
    return {};
  }
  auto layout = SnapshotNativeFrameReadbackLayout(
      kernel->memory(), *request.plan(), request.input_ranges(), record);
  const auto& output = *request.output_layout();
  if (!layout || layout->texture_fetch != output.texture_fetch ||
      !output.original_texture_fetch ||
      layout->original_texture_fetch != *output.original_texture_fetch ||
      layout->width != output.width || layout->height != output.height ||
      layout->pitch != output.pitch ||
      layout->required_bytes != output.required_bytes ||
      !request.IsAdmitted()) {
    return {};
  }
  return layout;
}

bool SaveNativeFrameReadback(const std::filesystem::path& capture_path,
                             const NativeFrameReadbackLayout& layout,
                             const gpu::NativeFrameExecutionRequest& request,
                             const gpu::NativeFrameReadbackResult& readback) {
  const auto tag = readback.native_completion.native_frame_identity();
  const auto& output = request.output_layout();
  if (readback.status != gpu::NativeFrameReadbackResult::Status::kComplete ||
      !output || output->texture_fetch != layout.texture_fetch ||
      !output->original_texture_fetch ||
      *output->original_texture_fetch != layout.original_texture_fetch ||
      output->width != layout.width || output->height != layout.height ||
      output->pitch != layout.pitch ||
      output->required_bytes != layout.required_bytes ||
      readback.native_completion.status() !=
          gpu::GpuCompletionStatus::kComplete ||
      readback.native_completion.backend_epoch() != request.backend_epoch() ||
      !tag || *tag != request.identity() ||
      readback.copied_range.address != layout.physical_address ||
      readback.copied_range.length != readback.bytes.size() ||
      readback.bytes.size() < layout.required_bytes ||
      !Contains(request.output_range(), readback.copied_range)) {
    return false;
  }
  auto raw_path = capture_path;
  raw_path += ".readback.bin";
  auto json_path = capture_path;
  json_path += ".readback.json";
  auto raw = OpenPrivate(raw_path);
  if (!raw ||
      std::fwrite(readback.bytes.data(), 1, readback.bytes.size(), raw.get()) !=
          readback.bytes.size() ||
      !Close(raw)) {
    return false;
  }
  auto json = OpenPrivate(json_path);
  if (!json) {
    return false;
  }
  const auto& owner = request.identity().owner;
  if (std::fprintf(
          json.get(),
          "{\"version\":1,\"gpu_readback_complete\":true,\"full_rectangle_"
          "validated\":true,"
          "\"kernel_epoch\":%llu,\"session\":%llu,\"source\":%llu,\"backend_"
          "epoch\":%llu,\"submission\":%llu,"
          "\"width\":%u,\"height\":%u,\"pitch\":%u,\"tiled\":true,\"format\":6,"
          "\"endian\":0,\"channels\":\"BGRA\","
          "\"physical_address\":%u,\"bytes\":%u,\"required_bytes\":%u,\"copy_"
          "dest_info\":%u,\"copy_dest_pitch\":%u,"
          "\"texture_fetch\":[%u,%u,%u,%u,%u,%u],"
          "\"original_texture_fetch\":[%u,%u,%u,%u,%u,%u],"
          "\"rectangle_float_words\":[%u,"
          "%u,%u,%u,%u,%u],"
          "\"pixels_verified\":false,\"composition_verified\":false,\"input_"
          "verified\":false}\n",
          static_cast<unsigned long long>(owner.kernel_epoch),
          static_cast<unsigned long long>(owner.session_id),
          static_cast<unsigned long long>(request.identity().source_serial),
          static_cast<unsigned long long>(request.backend_epoch()),
          static_cast<unsigned long long>(
              readback.native_completion.submission()),
          layout.width, layout.height, layout.pitch,
          readback.copied_range.address, readback.copied_range.length,
          layout.required_bytes, layout.copy_dest_info, layout.copy_dest_pitch,
          layout.texture_fetch[0], layout.texture_fetch[1],
          layout.texture_fetch[2], layout.texture_fetch[3],
          layout.texture_fetch[4], layout.texture_fetch[5],
          layout.original_texture_fetch[0], layout.original_texture_fetch[1],
          layout.original_texture_fetch[2], layout.original_texture_fetch[3],
          layout.original_texture_fetch[4], layout.original_texture_fetch[5],
          layout.rectangle_words[0], layout.rectangle_words[1],
          layout.rectangle_words[2], layout.rectangle_words[3],
          layout.rectangle_words[4], layout.rectangle_words[5]) <= 0) {
    return false;
  }
  return Close(json);
}
namespace {
bool SameOutputLayout(const gpu::NativeFrameOutputLayout& a,
                      const gpu::NativeFrameOutputLayout& b) {
  return a.origin == b.origin && a.width == b.width && a.height == b.height &&
         a.pitch == b.pitch && a.required_bytes == b.required_bytes &&
         a.texture_fetch == b.texture_fetch &&
         a.original_texture_fetch == b.original_texture_fetch &&
         a.overlay_state == b.overlay_state &&
         a.overlay_presentation == b.overlay_presentation;
}
bool CaptureStorageValid(const NativeFrameOutputCapture& capture) {
  const auto& layout = capture.layout;
  if (!capture.identity.valid() || !capture.backend_epoch ||
      !capture.native_submission ||
      !gpu::ValidateNativeFrameOutputStorage(layout, capture.output_range) ||
      capture.written_range.address != capture.output_range.address ||
      !Contains(capture.output_range, capture.written_range) ||
      capture.written_range.length < layout.required_bytes ||
      capture.written_range.length > 16 * 1024 * 1024) {
    return false;
  }
  if (gpu::NativeFrameOutputOriginUsesPm4Resolve(layout.origin)) {
    const auto expected_origin =
        layout.origin == gpu::NativeFrameOutputLayout::Origin::kNativeOverlayResolve
            ? gpu::NativeFramePm4ResolveProof::OutputOrigin::kNativeOverlay
            : gpu::NativeFramePm4ResolveProof::OutputOrigin::kVdTexture;
    return layout.pm4_resolve && !layout.original_texture_fetch &&
           layout.pm4_resolve->identity() == capture.identity &&
           layout.pm4_resolve->output_origin() == expected_origin;
  }
  return layout.origin ==
             gpu::NativeFrameOutputLayout::Origin::kOriginalXgHeader &&
         layout.original_texture_fetch && !layout.pm4_resolve;
}
bool PrimaryTriggerValid(const NativeFrameOutputCapture& capture,
                         const NativeFrameOutputBDownTrigger& trigger) {
  return gpu::NativeFrameOutputOriginUsesPm4Resolve(capture.layout.origin) &&
         trigger.input_generation && trigger.domain_id &&
         trigger.root_load_generation && trigger.foreground_generation &&
         trigger.visibility_sequence && trigger.press_sequence &&
         trigger.binding_epoch &&
         trigger.root_hmodule == capture.identity.owner.module_hmodule &&
         trigger.slot < 4 && trigger.native_user < 4 && trigger.device_handle &&
         (trigger.buttons & 0x2000);
}
bool PrimaryCaptureValid(const NativeFrameOutputCapture& capture) {
  if (capture.b_down_trigger && capture.exact_source_diagnostic)
    return false;
  if (!capture.b_down_trigger && !capture.exact_source_diagnostic) {
    return capture.primary_bytes.empty() &&
           capture.primary_identity == gpu::NativeFrameSubmissionIdentity{};
  }
  return capture.primary_identity == capture.identity &&
         !capture.primary_bytes.empty() &&
         capture.primary_bytes.size() <= 128 * 1024 &&
         !(capture.primary_bytes.size() & 3) &&
         (capture.b_down_trigger
              ? PrimaryTriggerValid(capture, *capture.b_down_trigger)
              : gpu::NativeFrameOutputOriginUsesPm4Resolve(
                    capture.layout.origin) &&
                    capture.exact_source_diagnostic->source_serial &&
                    capture.exact_source_diagnostic->source_serial ==
                        capture.identity.source_serial &&
                    capture.exact_source_diagnostic->title_input.valid() &&
                    capture.exact_source_diagnostic->title_input
                            .backend_epoch == capture.backend_epoch);
}
bool InputRangeValid(memory::PhysicalRange range) {
  return range.length && range.address < 0x20000000 &&
         range.length <= 0x20000000 - range.address;
}
bool InputRangesOverlap(memory::PhysicalRange a, memory::PhysicalRange b) {
  return uint64_t(a.address) < uint64_t(b.address) + b.length &&
         uint64_t(b.address) < uint64_t(a.address) + a.length;
}
bool InputBytesMatch(const NativeFrameOutputInputs &inputs,
                     memory::PhysicalRange range,
                     std::span<const uint8_t> bytes) {
  if (!InputRangeValid(range) || bytes.size() != range.length) {
    return false;
  }
  uint64_t cursor = range.address, end = cursor + range.length;
  for (const auto &block : inputs.blocks) {
    const uint64_t block_end =
        uint64_t(block.range.address) + block.range.length;
    if (block_end <= cursor) {
      continue;
    }
    if (block.range.address > cursor) {
      return false;
    }
    const size_t count = size_t(std::min(block_end, end) - cursor);
    const size_t offset = block.offset + size_t(cursor - block.range.address);
    if (!std::equal(bytes.begin() + size_t(cursor - range.address),
                    bytes.begin() + size_t(cursor - range.address) + count,
                    inputs.bytes.begin() + offset)) {
      return false;
    }
    cursor += count;
    if (cursor == end) {
      return true;
    }
  }
  return false;
}
bool SameTitleLayout(const gpu::NativeSystemTitleInput::Layout &a,
                     const gpu::NativeSystemTitleInput::Layout &b) {
  return a.swap.foreground_generation == b.swap.foreground_generation &&
         a.swap.title_hmodule == b.swap.title_hmodule &&
         a.swap.title_load_generation == b.swap.title_load_generation &&
         a.swap.fetch == b.swap.fetch && a.swap.width == b.swap.width &&
         a.swap.height == b.swap.height && a.fetch == b.fetch &&
         a.range.address == b.range.address &&
         a.range.length == b.range.length && a.width == b.width &&
         a.height == b.height && a.pitch == b.pitch &&
         a.row_pitch_bytes == b.row_pitch_bytes &&
         a.data_extent_bytes == b.data_extent_bytes && a.tiled == b.tiled &&
         a.endianness == b.endianness;
}
bool ExecutionInputsValid(const NativeFrameOutputCapture &capture,
                          const NativeFrameOutputInputs &inputs) {
  using Snapshot = gpu::NativeFrameInputSnapshot;
  using Inputs = NativeFrameOutputInputs;
  if ((!capture.b_down_trigger && !capture.exact_source_diagnostic) ||
      !PrimaryCaptureValid(capture) ||
      (capture.exact_source_diagnostic && !inputs.title) ||
      inputs.identity != capture.identity ||
      inputs.backend_epoch != capture.backend_epoch ||
      inputs.output_range.address != capture.output_range.address ||
      inputs.output_range.length != capture.output_range.length ||
      !InputRangeValid(inputs.primary_range) ||
      inputs.primary_range.length != capture.primary_bytes.size() ||
      inputs.blocks.empty() || inputs.blocks.size() > Snapshot::kMaxBlocks ||
      inputs.shaders.size() > Inputs::kMaxShaderLoads || inputs.bytes.empty() ||
      inputs.bytes.size() > Inputs::kMaxBytes ||
      inputs.snapshot_bytes > Snapshot::kMaxBytes) {
    return false;
  }
  uint64_t cursor = 0, native_bytes = 0, physical_end = 0;
  for (const auto &block : inputs.blocks) {
    if (!InputRangeValid(block.range) ||
        block.range.length > Snapshot::kMaxReadBytes ||
        block.range.address < physical_end || block.offset != cursor ||
        block.range.length > inputs.bytes.size() - cursor ||
        InputRangesOverlap(block.range, inputs.output_range)) {
      return false;
    }
    cursor += block.range.length;
    native_bytes += block.range.length;
    physical_end = uint64_t(block.range.address) + block.range.length;
  }
  // All complete byte ranges have been checked before this reads the payload.
  if (native_bytes > Snapshot::kMaxBytes ||
      !InputBytesMatch(inputs, inputs.primary_range, capture.primary_bytes)) {
    return false;
  }
  const auto word = [&](uint32_t at) {
    const auto *p = capture.primary_bytes.data() + size_t(at) * 4;
    return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 |
           p[3];
  };
  size_t shader_index = 0;
  const uint32_t words = uint32_t(capture.primary_bytes.size() / 4);
  for (uint32_t at = 0; at < words;) {
    const uint32_t header = word(at), type = header >> 30;
    const uint32_t size = type == 2   ? 1
                          : type == 1 ? 3
                                      : ((header >> 16) & 0x3FFF) + 2;
    if (size > words - at) {
      return false;
    }
    const uint32_t opcode = (header >> 8) & 0x7F;
    if (type == 3 && (opcode == gpu::xenos::PM4_IM_LOAD ||
                      opcode == gpu::xenos::PM4_IM_LOAD_IMMEDIATE)) {
      if (shader_index == inputs.shaders.size() || size < 3) {
        return false;
      }
      const auto &shader = inputs.shaders[shader_index++];
      const auto &load = shader.load;
      const bool in = opcode == gpu::xenos::PM4_IM_LOAD_IMMEDIATE;
      const uint32_t operand = word(at + 1), declared = word(at + 2);
      if (load.packet_dword_offset != at || load.is_inline != in ||
          uint32_t(load.stage) > 1 || uint32_t(load.stage) != (operand & 3) ||
          !load.dword_count || load.dword_count > Inputs::kMaxShaderBytes / 4 ||
          declared != load.dword_count ||
          size != (in ? load.dword_count + 3 : 3) ||
          load.inline_dword_offset != (in ? at + 3 : 0) ||
          load.raw_address != (in ? 0 : operand & ~3u) ||
          load.physical_address !=
              (in ? 0 : gpu::xenos::CpuToGpu(operand & ~3u)) ||
          shader.offset != cursor || shader.length != load.dword_count * 4 ||
          shader.length > inputs.bytes.size() - cursor) {
        return false;
      }
      const std::span<const uint8_t> code(inputs.bytes.data() + cursor,
                                          shader.length);
      if (in ? !std::equal(code.begin(), code.end(),
                           capture.primary_bytes.begin() + size_t(at + 3) * 4)
             : !InputBytesMatch(inputs, {load.physical_address, shader.length},
                                code)) {
        return false;
      }
      cursor += shader.length;
    }
    at += size;
  }
  if (shader_index != inputs.shaders.size()) {
    return false;
  }
  uint64_t title_bytes = 0;
  if (inputs.title) {
    const auto &title = *inputs.title;
    const auto &id = title.identity;
    const auto &layout = title.layout;
    const auto canonical = gpu::NativeSystemTitleInput::GetLayout(layout.swap);
    const uint64_t foreground =
        capture.b_down_trigger ? capture.b_down_trigger->foreground_generation
                               : capture.exact_source_diagnostic->title_input
                                     .foreground_generation;
    if (!id.valid() || id.backend_epoch != inputs.backend_epoch ||
        id.foreground_generation != foreground ||
        (capture.exact_source_diagnostic &&
         id != capture.exact_source_diagnostic->title_input) ||
        id.foreground_generation != layout.swap.foreground_generation ||
        id.title_hmodule != layout.swap.title_hmodule ||
        id.title_load_generation != layout.swap.title_load_generation ||
        !canonical || !SameTitleLayout(layout, *canonical) ||
        title.offset != cursor || title.length != layout.range.length ||
        !title.length ||
        title.length > gpu::NativeSystemTitleInput::kMaxRawBytes ||
        title.length > inputs.bytes.size() - cursor ||
        InputRangesOverlap(layout.range, inputs.output_range) ||
        InputRangesOverlap(layout.range, inputs.primary_range)) {
      return false;
    }
    for (const auto &block : inputs.blocks) {
      if (InputRangesOverlap(block.range, layout.range)) {
        return false;
      }
    }
    title_bytes = title.length;
    cursor += title.length;
  }
  return cursor == inputs.bytes.size() &&
         native_bytes + title_bytes == inputs.snapshot_bytes;
}
}  // namespace

std::optional<NativeFrameOutputCapture> SnapshotNativeFrameOutputCapture(
    const gpu::NativeFrameExecutionRequest& request,
    const gpu::NativeFrameExecutionResult& execution) {
  const auto tag = execution.completion.native_frame_identity();
  if (execution.status != gpu::NativeFrameExecutionResult::Status::kSubmitted ||
      !execution.title_state_restored || !request.output_layout() || !tag ||
      *tag != request.identity() ||
      execution.completion.status() != gpu::GpuCompletionStatus::kComplete ||
      execution.completion.backend_epoch() != request.backend_epoch() ||
      !execution.completion.submission()) {
    return {};
  }
  NativeFrameOutputCapture capture;
  capture.identity = request.identity();
  capture.backend_epoch = request.backend_epoch();
  capture.native_submission = execution.completion.submission();
  capture.layout = *request.output_layout();
  capture.output_range = request.output_range();
  capture.written_range = execution.written_range;
  if (!CaptureStorageValid(capture)) {
    return {};
  }
  if (gpu::NativeFrameOutputOriginUsesPm4Resolve(capture.layout.origin)) {
    if (!request.plan()) {
      return {};
    }
    const auto derived = gpu::DeriveNativePm4OutputLayout(
        *request.plan(), request.output_range(), request.input_ranges(),
        capture.layout.pm4_resolve);
    if (!derived || !SameOutputLayout(*derived, capture.layout)) {
      return {};
    }
  }
  return capture;
}

bool AttachNativeFrameOutputPrimary(
    NativeFrameOutputCapture& capture, const NativeFrameSource& source,
    const NativeFrameOutputBDownTrigger& trigger) {
  const gpu::NativeFrameSubmissionIdentity primary_identity{source.owner(),
                                                            source.serial()};
  const auto primary = source.primary();
  if (!CaptureStorageValid(capture) || !PrimaryCaptureValid(capture) ||
      capture.b_down_trigger || capture.exact_source_diagnostic ||
      primary_identity != capture.identity || primary.empty() ||
      primary.size() > 128 * 1024 || (primary.size() & 3) ||
      !PrimaryTriggerValid(capture, trigger)) {
    return false;
  }
  // Allocate before publishing any metadata. On allocation failure the caller's
  // existing diagnostic-only catch path leaves no partially attached primary.
  std::vector<uint8_t> bytes(primary.begin(), primary.end());
  capture.primary_bytes = std::move(bytes);
  capture.primary_identity = primary_identity;
  capture.b_down_trigger = trigger;
  return true;
}

bool AttachNativeFrameOutputExactSourceDiagnostic(
    NativeFrameOutputCapture &capture, const NativeFrameSource &source,
    const NativeFrameOutputExactSourceDiagnosticTrigger &trigger) {
  const gpu::NativeFrameSubmissionIdentity identity{source.owner(),
                                                    source.serial()};
  const auto primary = source.primary();
  if (!CaptureStorageValid(capture) || !PrimaryCaptureValid(capture) ||
      capture.b_down_trigger || capture.exact_source_diagnostic ||
      !trigger.source_serial || trigger.source_serial != source.serial() ||
      !trigger.title_input.valid() ||
      trigger.title_input.backend_epoch != capture.backend_epoch ||
      !source.title_input() || !source.title_input()->valid() ||
      source.title_input()->identity() != trigger.title_input ||
      identity != capture.identity || primary.empty() ||
      primary.size() > 128 * 1024 || (primary.size() & 3) ||
      !gpu::NativeFrameOutputOriginUsesPm4Resolve(capture.layout.origin))
    return false;
  std::vector<uint8_t> bytes(primary.begin(), primary.end());
  capture.primary_bytes = std::move(bytes);
  capture.primary_identity = identity;
  capture.exact_source_diagnostic = trigger;
  return true;
}

bool AttachNativeFrameOutputInputs(
    NativeFrameOutputCapture &capture,
    const gpu::NativeFrameExecutionRequest &request) {
  const auto &snapshot = request.input_snapshot();
  if (!CaptureStorageValid(capture) ||
      (!capture.b_down_trigger && !capture.exact_source_diagnostic) ||
      !PrimaryCaptureValid(capture) || capture.inputs ||
      (capture.exact_source_diagnostic &&
       (!snapshot || !snapshot->title_input() ||
        snapshot->title_input()->identity() !=
            capture.exact_source_diagnostic->title_input)) ||
      request.identity() != capture.identity ||
      request.backend_epoch() != capture.backend_epoch || !request.plan() ||
      !snapshot || !snapshot->valid() ||
      snapshot->identity() != request.identity() ||
      snapshot->plan() != request.plan() ||
      snapshot->output_range().address != request.output_range().address ||
      snapshot->output_range().length != request.output_range().length ||
      request.shaders().size() > NativeFrameOutputInputs::kMaxShaderLoads ||
      request.shaders().size() != request.plan()->inventory().shaders.size()) {
    return false;
  }
  NativeFrameOutputInputs inputs;
  inputs.identity = request.identity();
  inputs.backend_epoch = request.backend_epoch();
  inputs.primary_range = snapshot->primary_range();
  inputs.output_range = snapshot->output_range();
  inputs.snapshot_bytes = snapshot->byte_size();
  // Bound the complete allocation before copying any accepted input.
  uint64_t total = inputs.snapshot_bytes;
  for (const auto &shader : request.shaders()) {
    if (shader.code.empty() ||
        shader.code.size() > NativeFrameOutputInputs::kMaxShaderBytes) {
      return false;
    }
    total += shader.code.size();
  }
  if (total > NativeFrameOutputInputs::kMaxBytes) {
    return false;
  }
  inputs.bytes.reserve(size_t(total));
  for (const auto &block : snapshot->blocks()) {
    inputs.blocks.push_back({block.range, uint32_t(inputs.bytes.size())});
    inputs.bytes.insert(inputs.bytes.end(), block.bytes.begin(),
                        block.bytes.end());
  }
  for (const auto &shader : request.shaders()) {
    inputs.shaders.push_back({shader.load, uint32_t(inputs.bytes.size()),
                              uint32_t(shader.code.size())});
    inputs.bytes.insert(inputs.bytes.end(), shader.code.begin(),
                        shader.code.end());
  }
  if (const auto &title = snapshot->title_input()) {
    if (!title->valid() || title != request.plan()->title_input()) {
      return false;
    }
    inputs.title = NativeFrameOutputInputs::Title{
        title->identity(), title->layout(), uint32_t(inputs.bytes.size()),
        uint32_t(title->bytes().size())};
    inputs.bytes.insert(inputs.bytes.end(), title->bytes().begin(),
                        title->bytes().end());
  }
  if (inputs.bytes.size() != total || !ExecutionInputsValid(capture, inputs)) {
    return false;
  }
  capture.inputs = std::move(inputs);
  return true;
}

bool CompleteNativeFrameOutputCapture(NativeFrameOutputCapture& capture,
                                      gpu::NativeFrameReadbackResult readback) {
  capture.readback_complete = false;
  capture.bytes.clear();
  capture.readback_status = uint32_t(readback.status);
  const auto tag = readback.native_completion.native_frame_identity();
  const bool valid =
      CaptureStorageValid(capture) &&
      readback.status == gpu::NativeFrameReadbackResult::Status::kComplete &&
      readback.native_completion.status() ==
          gpu::GpuCompletionStatus::kComplete &&
      tag && *tag == capture.identity &&
      readback.native_completion.backend_epoch() == capture.backend_epoch &&
      readback.native_completion.submission() == capture.native_submission &&
      readback.copied_range.address == capture.written_range.address &&
      readback.copied_range.length == capture.written_range.length &&
      readback.bytes.size() == readback.copied_range.length;
  if (valid) {
    capture.readback_identity = *tag;
    capture.readback_backend_epoch = readback.native_completion.backend_epoch();
    capture.readback_native_submission =
        readback.native_completion.submission();
    capture.copied_range = readback.copied_range;
    capture.bytes = std::move(readback.bytes);
  }
  // Complete flags never publish while this helper still holds a token.
  readback.native_completion = {};
  capture.readback_complete = valid;
  return valid;
}

bool AssociateNativeFrameOutputCapture(
    NativeFrameOutputCapture& capture,
    gpu::NativeFramePresentationStatus presentation) {
  capture.image_associated =
      presentation.identity == capture.identity &&
      presentation.backend_epoch == capture.backend_epoch &&
      presentation.image_version &&
      presentation.native_write_submission == capture.native_submission;
  if (capture.image_associated) {
    capture.image_version = presentation.image_version;
    capture.presentation_backend_epoch =
        presentation.presentation_backend_epoch;
    capture.paint_submission = presentation.composite_submission;
    capture.presentation_status = uint32_t(presentation.status);
    capture.paint_completion_status =
        uint32_t(presentation.composite_completion.status());
    capture.image_ready = presentation.image_ready;
    capture.presenter_accepted = presentation.presenter_accepted;
  }
  presentation.composite_completion = {};
  return capture.image_associated;
}

bool SaveNativeFrameOutputCapture(const std::filesystem::path& capture_path,
                                  const NativeFrameOutputCapture& capture) {
  if (!capture_path.is_absolute() || !CaptureStorageValid(capture) ||
      !PrimaryCaptureValid(capture) || !capture.readback_complete ||
      (capture.exact_source_diagnostic && !capture.inputs) ||
      (capture.inputs && !ExecutionInputsValid(capture, *capture.inputs)) ||
      !capture.image_associated || !capture.image_version ||
      capture.readback_status !=
          uint32_t(gpu::NativeFrameReadbackResult::Status::kComplete) ||
      capture.readback_identity != capture.identity ||
      capture.readback_backend_epoch != capture.backend_epoch ||
      capture.readback_native_submission != capture.native_submission ||
      capture.copied_range.address != capture.written_range.address ||
      capture.copied_range.length != capture.written_range.length ||
      capture.bytes.size() != capture.copied_range.length) {
    return false;
  }
  auto raw_path = capture_path;
  raw_path += ".readback.bin";
  auto metadata_path = capture_path;
  metadata_path += ".readback.json";
  auto raw = OpenPrivate(raw_path);
  if (!raw ||
      std::fwrite(capture.bytes.data(), 1, capture.bytes.size(), raw.get()) !=
          capture.bytes.size() ||
      !Close(raw)) {
    return false;
  }
  if (capture.b_down_trigger || capture.exact_source_diagnostic) {
    auto primary_path = capture_path;
    primary_path += ".primary.bin";
    auto primary = OpenPrivate(primary_path);
    if (!primary ||
        std::fwrite(capture.primary_bytes.data(), 1,
                    capture.primary_bytes.size(),
                    primary.get()) != capture.primary_bytes.size() ||
        !Close(primary)) {
      return false;
    }
  }
  if (capture.inputs) {
    auto inputs_path = capture_path;
    inputs_path += ".inputs.bin";
    auto inputs = OpenPrivate(inputs_path);
    if (!inputs ||
        std::fwrite(capture.inputs->bytes.data(), 1,
                    capture.inputs->bytes.size(),
                    inputs.get()) != capture.inputs->bytes.size() ||
        !Close(inputs)) {
      return false;
    }
  }
  auto metadata = OpenPrivate(metadata_path);
  if (!metadata) {
    return false;
  }
  const auto& layout = capture.layout;
  const auto& owner = capture.identity.owner;
  const auto array = [](const auto& values) {
    std::string result = "[";
    for (const auto value : values) {
      if (result.size() != 1) {
        result += ',';
      }
      result += std::to_string(value);
    }
    return result + ']';
  };
  std::string text = fmt::format(
      "{{\"version\":2,\"raw_write_complete\":true,"
      "\"gpu_readback_complete\":true,\"output_layout_origin\":{},"
      "\"output_layout_origin_name\":\"{}\",\"kernel_epoch\":{},"
      "\"runtime_module_generation\":{},\"session\":{},\"root_hmodule\":{},"
      "\"source\":{},\"backend_epoch\":{},\"submission\":{},"
      "\"width\":{},\"height\":{},\"pitch\":{},\"tiled\":true,"
      "\"format\":6,\"endian\":0,\"channels\":\"BGRA\","
      "\"physical_address\":{},\"bytes\":{},\"required_bytes\":{},"
      "\"texture_fetch\":{},\"original_texture_fetch\":{},",
      uint32_t(layout.origin),
      layout.origin == gpu::NativeFrameOutputLayout::Origin::kNativeOverlayResolve
          ? "native_overlay_resolve"
          : layout.origin == gpu::NativeFrameOutputLayout::Origin::kPm4Resolve
                ? "pm4_resolve"
                : "original_xg_header",
      owner.kernel_epoch, owner.module_generation, owner.session_id,
      owner.module_hmodule, capture.identity.source_serial,
      capture.backend_epoch, capture.native_submission, layout.width,
      layout.height, layout.pitch, capture.copied_range.address,
      capture.copied_range.length, layout.required_bytes,
      array(layout.texture_fetch),
      layout.original_texture_fetch ? array(*layout.original_texture_fetch)
                                    : "null");
  if (layout.pm4_resolve) {
    const auto& proof = *layout.pm4_resolve;
    text += fmt::format(
        "\"copy_packet_dword\":{},\"rectangle_address\":{},"
        "\"rectangle_length\":{},\"rectangle_endian\":{},"
        "\"rectangle_raw_words\":{},",
        proof.copy_packet_dword_offset(), proof.rectangle().address,
        proof.rectangle().length, proof.rectangle_endian(),
        array(proof.rectangle_words()));
    if (layout.origin ==
        gpu::NativeFrameOutputLayout::Origin::kNativeOverlayResolve) {
      // Preserve the real absent VD output fields and captured native canvas.
      // These are raw proof bytes, not an original XG header or visibility.
      text += fmt::format(
          "\"pm4_output_origin\":\"native_overlay\","
          "\"vd_descriptor_raw_bytes\":{},"
          "\"display_information_raw_bytes\":{},",
          array(proof.descriptor()), array(proof.display_information()));
      const auto state = [&](const auto& value) {
        return fmt::format(
            "{{\"known_mask\":{},\"enable\":{},\"control1\":{},"
            "\"surface_address\":{},\"pitch\":{},\"offset_x\":{},"
            "\"offset_y\":{},\"start\":{},\"end\":{},"
            "\"alpha_outside\":{},\"alpha_inside\":{},"
            "\"alpha_control\":{}}}",
            value.known_mask, value.enable, value.control1,
            value.surface_address, value.pitch, value.offset_x, value.offset_y,
            value.start, value.end, value.alpha_outside, value.alpha_inside,
            value.alpha_control);
      };
      text += fmt::format(
          "\"overlay_input_requested_state\":{},"
          "\"overlay_output_requested_state\":{},",
          proof.overlay_input_state() ? state(*proof.overlay_input_state())
                                      : "null",
          layout.overlay_state ? state(*layout.overlay_state) : "null");
      if (layout.overlay_presentation) {
        const auto& presentation = *layout.overlay_presentation;
        text += fmt::format(
            "\"overlay_presentation\":{{\"canvas_x\":{},\"canvas_y\":{},"
            "\"canvas_width\":{},\"canvas_height\":{},\"x\":{},\"y\":{},"
            "\"width\":{},\"height\":{},\"opacity\":{}}},",
            presentation.canvas_x, presentation.canvas_y,
            presentation.canvas_width, presentation.canvas_height,
            presentation.x, presentation.y, presentation.width,
            presentation.height, uint32_t(presentation.opacity));
      } else {
        text += "\"overlay_presentation\":null,";
      }
    }
  }
  if (capture.b_down_trigger) {
    const auto& trigger = *capture.b_down_trigger;
    text += fmt::format(
        "\"primary_capture\":{{\"write_complete\":true,\"bytes\":{},"
        "\"truncated\":false,\"content\":\"source_primary_pm4\","
        "\"byte_order\":\"big_endian_dwords\",\"kernel_epoch\":{},"
        "\"runtime_module_generation\":{},\"session\":{},\"root_hmodule\":{},"
        "\"source\":{},\"trigger\":{{"
        "\"kind\":\"observed_b_down_before_source_freeze\","
        "\"input_generation\":{},\"domain_id\":{},\"root_load_generation\":{},"
        "\"foreground_generation\":{},\"visibility_sequence\":{},"
        "\"press_sequence\":{},\"binding_epoch\":{},\"root_hmodule\":{},"
        "\"slot\":{},\"native_user\":{},\"device_handle\":{},"
        "\"binding_identity\":{},\"packet\":{},\"buttons\":{}}}}},",
        capture.primary_bytes.size(), owner.kernel_epoch,
        owner.module_generation, owner.session_id, owner.module_hmodule,
        capture.identity.source_serial, trigger.input_generation,
        trigger.domain_id, trigger.root_load_generation,
        trigger.foreground_generation, trigger.visibility_sequence,
        trigger.press_sequence, trigger.binding_epoch, trigger.root_hmodule,
        trigger.slot, trigger.native_user, trigger.device_handle,
        trigger.binding_identity, trigger.packet, trigger.buttons);
  } else if (capture.exact_source_diagnostic) {
    text += fmt::format(
        "\"primary_capture\":{{\"write_complete\":true,\"bytes\":{},"
        "\"truncated\":false,\"content\":\"source_primary_pm4\","
        "\"byte_order\":\"big_endian_dwords\",\"kernel_epoch\":{},"
        "\"runtime_module_generation\":{},\"session\":{},\"root_hmodule\":{},"
        "\"source\":{},\"trigger\":{{\"kind\":\"explicit_exact_source_"
        "diagnostic\","
        "\"source\":{},\"foreground_generation\":{},\"title_backend_epoch\":{},"
        "\"title_hmodule\":{},\"title_load_generation\":{},\"title_swap_"
        "serial\":{},"
        "\"technical_acceptance\":false,\"feature_acceptance\":false}}}},",
        capture.primary_bytes.size(), owner.kernel_epoch,
        owner.module_generation, owner.session_id, owner.module_hmodule,
        capture.identity.source_serial,
        capture.exact_source_diagnostic->source_serial,
        capture.exact_source_diagnostic->title_input.foreground_generation,
        capture.exact_source_diagnostic->title_input.backend_epoch,
        capture.exact_source_diagnostic->title_input.title_hmodule,
        capture.exact_source_diagnostic->title_input.title_load_generation,
        capture.exact_source_diagnostic->title_input.swap_serial);
  } else {
    text += "\"primary_capture\":null,";
  }
  if (capture.inputs) {
    const auto &inputs = *capture.inputs;
    uint32_t native_bytes = 0, shader_bytes = 0;
    for (const auto &block : inputs.blocks) {
      native_bytes += block.range.length;
    }
    for (const auto &shader : inputs.shaders) {
      shader_bytes += shader.length;
    }
    text += fmt::format(
        "\"input_capture\":{{\"schema\":1,\"write_complete\":true,"
        "\"truncated\":false,\"content\":\"immutable_execution_inputs\","
        "\"byte_order\":\"raw_storage\",\"kernel_epoch\":{},"
        "\"runtime_module_generation\":{},\"session\":{},\"root_hmodule\":{},"
        "\"source\":{},\"backend_epoch\":{},\"primary_address\":{},"
        "\"primary_bytes\":{},\"output_address\":{},\"output_bytes\":{},"
        "\"snapshot_bytes\":{},\"native_bytes\":{},\"shader_bytes\":{},"
        "\"title_bytes\":{},\"bytes\":{},\"blocks\":[",
        owner.kernel_epoch, owner.module_generation, owner.session_id,
        owner.module_hmodule, capture.identity.source_serial,
        inputs.backend_epoch, inputs.primary_range.address,
        inputs.primary_range.length, inputs.output_range.address,
        inputs.output_range.length, inputs.snapshot_bytes, native_bytes,
        shader_bytes, inputs.title ? inputs.title->length : 0,
        inputs.bytes.size());
    for (size_t i = 0; i < inputs.blocks.size(); ++i) {
      const auto &block = inputs.blocks[i];
      text += fmt::format(
          "{}{{\"physical_address\":{},\"bytes\":{},\"offset\":{}}}",
          i ? "," : "", block.range.address, block.range.length, block.offset);
    }
    text += "],\"shaders\":[";
    for (size_t i = 0; i < inputs.shaders.size(); ++i) {
      const auto &shader = inputs.shaders[i];
      const auto &load = shader.load;
      text += fmt::format(
          "{}{{\"load_index\":{},\"stage\":{},\"packet_dword\":{},"
          "\"dword_count\":{},\"inline\":{},\"inline_dword\":{},"
          "\"raw_address\":{},\"physical_address\":{},\"offset\":{},"
          "\"bytes\":{}}}",
          i ? "," : "", i, uint32_t(load.stage), load.packet_dword_offset,
          load.dword_count, load.is_inline, load.inline_dword_offset,
          load.raw_address, load.physical_address, shader.offset,
          shader.length);
    }
    text += "],\"title\":";
    if (inputs.title) {
      const auto &title = *inputs.title;
      const auto &id = title.identity;
      const auto &layout = title.layout;
      text += fmt::format(
          "{{\"backend_epoch\":{},\"foreground_generation\":{},"
          "\"title_hmodule\":{},\"title_load_generation\":{},"
          "\"swap_serial\":{},\"swap_width\":{},\"swap_height\":{},"
          "\"swap_fetch\":{},\"texture_fetch\":{},\"width\":{},"
          "\"height\":{},\"pitch\":{},\"row_pitch_bytes\":{},"
          "\"data_extent_bytes\":{},\"tiled\":{},\"endian\":{},"
          "\"physical_address\":{},\"bytes\":{},\"offset\":{}}}",
          id.backend_epoch, id.foreground_generation, id.title_hmodule,
          id.title_load_generation, id.swap_serial, layout.swap.width,
          layout.swap.height, array(layout.swap.fetch), array(layout.fetch),
          layout.width, layout.height, layout.pitch, layout.row_pitch_bytes,
          layout.data_extent_bytes, layout.tiled, uint32_t(layout.endianness),
          layout.range.address, title.length, title.offset);
    } else {
      text += "null";
    }
    text += "},";
  } else {
    text += "\"input_capture\":null,";
  }
  text += fmt::format(
      "\"image_associated\":true,\"image_version\":{},"
      "\"native_write_submission\":{},\"image_ready\":{},"
      "\"presenter_accepted\":{},\"presentation_status\":{},"
      "\"presentation_backend_epoch\":{},\"paint_submission\":{},"
      "\"paint_completion_status\":{},\"pixels_verified\":false,"
      "\"composition_verified\":false,\"input_verified\":false}}\n",
      capture.image_version, capture.native_submission, capture.image_ready,
      capture.presenter_accepted, capture.presentation_status,
      capture.presentation_backend_epoch, capture.paint_submission,
      capture.paint_completion_status);
  return text.size() <= 512 * 1024 &&
         std::fwrite(text.data(), 1, text.size(), metadata.get()) ==
             text.size() &&
         Close(metadata);
}

}  // namespace xe::kernel
