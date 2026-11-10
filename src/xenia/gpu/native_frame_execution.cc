/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia contributors. All rights reserved.                    *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */
#include "xenia/gpu/native_frame_execution.h"

#include <algorithm>
#include <bit>
#include <bitset>
#include <cmath>
#include <cstring>
#include <utility>

#include "xenia/base/native_system_ui_trace.h"
#include "xenia/gpu/native_vertex_access_analysis.h"
#include "xenia/gpu/register_file.h"
#include "xenia/gpu/texture_address.h"
#include "xenia/gpu/texture_util.h"

namespace xe::gpu {
namespace {
bool Valid(memory::PhysicalRange range) {
  return range.length && range.address < 0x20000000 &&
         range.length <= 4 * 1024 * 1024 &&
         range.length <= 0x20000000 - range.address;
}
bool Overlaps(memory::PhysicalRange a, memory::PhysicalRange b) {
  return uint64_t(a.address) < uint64_t(b.address) + b.length &&
         uint64_t(b.address) < uint64_t(a.address) + a.length;
}
bool ValidOutputLayout(const NativeFrameOutputLayout& layout,
                       memory::PhysicalRange output,
                       const NativeFrameCommandPlan& plan,
                       std::span<const memory::PhysicalRange> inputs,
                       const NativeFrameSubmissionIdentity& identity) {
  if (layout.origin == NativeFrameOutputLayout::Origin::kPm4Resolve ||
      layout.origin == NativeFrameOutputLayout::Origin::kNativeOverlayResolve) {
    if (layout.original_texture_fetch || !layout.pm4_resolve ||
        layout.pm4_resolve->identity() != identity) {
      return false;
    }
    const auto checked =
        DeriveNativePm4OutputLayout(plan, output, inputs, layout.pm4_resolve);
    return checked && checked->width == layout.width &&
           checked->height == layout.height && checked->pitch == layout.pitch &&
           checked->required_bytes == layout.required_bytes &&
           checked->texture_fetch == layout.texture_fetch &&
           checked->origin == layout.origin &&
           checked->overlay_state == layout.overlay_state &&
           checked->overlay_presentation == layout.overlay_presentation;
  }
  if (layout.origin != NativeFrameOutputLayout::Origin::kOriginalXgHeader ||
      !layout.original_texture_fetch || layout.pm4_resolve ||
      layout.overlay_state || layout.overlay_presentation) {
    return false;
  }
  if (!ValidateNativeFrameOutputStorage(layout, output)) {
    return false;
  }
  if (layout.original_texture_fetch) {
    auto normalized = *layout.original_texture_fetch;
    // Only base_address may differ, including for E/F CPU aliases whose
    // physical address includes the physical heap's page bias.
    normalized[1] = (normalized[1] & 0xFFF) | output.address;
    if (normalized != layout.texture_fetch) {
      return false;
    }
  }
  return true;
}
}  // namespace

bool ValidateNativeFrameOutputStorage(const NativeFrameOutputLayout& layout,
                                      memory::PhysicalRange output) {
  const bool original =
      layout.origin == NativeFrameOutputLayout::Origin::kOriginalXgHeader;
  const bool overlay =
      layout.origin == NativeFrameOutputLayout::Origin::kNativeOverlayResolve;
  if ((original && (layout.overlay_state || layout.overlay_presentation)) ||
      (layout.overlay_state && !layout.overlay_state->valid()) ||
      (overlay && (!layout.overlay_state || !layout.overlay_presentation ||
                   !layout.overlay_presentation->valid())) ||
      (!overlay && layout.overlay_presentation)) {
    return false;
  }
  if (!Valid(output) || (output.address & 0xFFF) ||
      (original
           ? (!layout.original_texture_fetch || layout.pm4_resolve ||
              layout.width != 640 || layout.height != 480 ||
              layout.pitch != 640)
           : ((layout.origin != NativeFrameOutputLayout::Origin::kPm4Resolve &&
               !overlay) || layout.original_texture_fetch ||
              !layout.pm4_resolve)) ||
      !layout.width || !layout.height ||
      layout.width > xenos::kTexture2DCubeMaxWidthHeight ||
      layout.height > xenos::kTexture2DCubeMaxWidthHeight ||
      layout.pitch < layout.width || (layout.pitch & 31) ||
      (layout.pitch >> 5) > 0x1FF) {
    return false;
  }
  xenos::xe_gpu_texture_fetch_t fetch{};
  std::memcpy(&fetch, layout.texture_fetch.data(), sizeof(fetch));
  if (fetch.type != xenos::FetchConstantType::kTexture ||
      fetch.format != xenos::TextureFormat::k_8_8_8_8 ||
      fetch.dimension != xenos::DataDimension::k2DOrStacked || fetch.stacked ||
      !fetch.tiled || fetch.endianness != xenos::Endian::kNone ||
      fetch.sign_x != xenos::TextureSign::kUnsigned ||
      fetch.sign_y != xenos::TextureSign::kUnsigned ||
      fetch.sign_z != xenos::TextureSign::kUnsigned ||
      fetch.sign_w != xenos::TextureSign::kUnsigned || fetch.num_format ||
      fetch.exp_adjust || fetch.swizzle != 0x60A || fetch.mip_min_level ||
      fetch.mip_max_level || fetch.mip_address || fetch.packed_mips ||
      fetch.size_2d.stack_depth ||
      uint32_t(fetch.size_2d.width) + 1 != layout.width ||
      uint32_t(fetch.size_2d.height) + 1 != layout.height ||
      (uint32_t(fetch.pitch) << 5) != layout.pitch ||
      (uint32_t(fetch.base_address) << 12) != output.address) {
    return false;
  }
  // For this 4-byte/block tiled format every complete 32x32 tile occupies
  // 4096 bytes. Bounds above keep the address calculation below INT32_MAX;
  // widen the final addition before comparing with the actual owned output.
  const int32_t last_tile = texture_address::Tiled2D(
      int32_t((layout.width - 1) & ~31u), int32_t((layout.height - 1) & ~31u),
      layout.pitch, 2);
  if (last_tile < 0) {
    return false;
  }
  const uint64_t required = uint64_t(last_tile) + 32 * 32 * 4;
  return required == layout.required_bytes && required <= output.length;
}

std::optional<NativeFrameOutputLayout> DeriveNativePm4OutputLayout(
    const NativeFrameCommandPlan& plan, memory::PhysicalRange output,
    std::span<const memory::PhysicalRange> inputs,
    const std::shared_ptr<const NativeFramePm4ResolveProof>& proof) {
  if (!proof || !proof->identity().valid() || !Valid(output) ||
      (output.address & 0xFFF) || !plan.has_final_copy() ||
      plan.inventory().draws.empty()) {
    return {};
  }
  const auto word = [&](uint32_t at) {
    const auto& bytes = proof->descriptor();
    return uint32_t(bytes[at]) << 24 | uint32_t(bytes[at + 1]) << 16 |
           uint32_t(bytes[at + 2]) << 8 | uint32_t(bytes[at + 3]);
  };
  const auto mapping = [](const NativeFramePm4ResolveProof::Mapping& m,
                          uint32_t observed) {
    using Kind = NativeFramePm4ResolveProof::Mapping::Kind;
    if (m.observed != observed || m.physical >= 0x20000000 ||
        (m.observed & 3) || (m.physical & 3)) {
      return false;
    }
    if (m.kind == Kind::kCanonicalPhysical) {
      return observed < 0x20000000 && observed == m.physical;
    }
    // Actual alias -> physical mapping is sealed by Session, not inferred
    // from these address bits. In particular E/F may add a 0x1000 page bias.
    return m.kind == Kind::kPhysicalHeapAlias && observed >= 0xA0000000 &&
           observed < 0xFFD00000;
  };
  const auto covered = [&](memory::PhysicalRange range) {
    return Valid(range) &&
           std::any_of(inputs.begin(), inputs.end(),
                       [&](memory::PhysicalRange input) {
                         return range.address >= input.address &&
                                uint64_t(range.address) + range.length <=
                                    uint64_t(input.address) + input.length;
                       });
  };
  using Origin = NativeFramePm4ResolveProof::OutputOrigin;
  const bool overlay = proof->output_origin() == Origin::kNativeOverlay;
  std::optional<NativeOverlayState> overlay_state;
  if (proof->overlay_input_state() && !proof->overlay_input_state()->valid()) {
    return {};
  }
  if (const auto& program = plan.native_overlay_program()) {
    overlay_state = program->Apply(proof->overlay_input_state().value_or(
        NativeOverlayState{}));
    if (!overlay_state->valid()) return {};
  }
  uint32_t logical_width = word(20), logical_height = word(24);
  std::optional<NativeOverlayPresentation> presentation;
  uint32_t observed_output = word(8);
  if (overlay) {
    // The native nonmodal helper deliberately omits the optional XG header.
    // Retain those genuine zero bytes; only the separately sealed same-frame
    // declaration and this program's complete geometry name an output.
    if (!plan.native_overlay_program() || !overlay_state ||
        !std::all_of(proof->descriptor().begin() + 8,
                     proof->descriptor().end(), [](uint8_t b) { return !b; })) {
      return {};
    }
    const auto current = plan.native_overlay_program()->Apply({});
    if (!current.valid() || !current.Has(NativeOverlayState::kGeometryKnown)) {
      return {};  // No stale/guessed window or surface geometry.
    }
    const uint32_t x = current.start >> 16, y = current.start & 0xFFFF;
    const uint32_t right = current.end >> 16, bottom = current.end & 0xFFFF;
    if (right <= x || bottom <= y || current.offset_x != x ||
        current.offset_y != y || (x & 255) || (y & 1)) {
      return {};  // Native renderer includes placement phase in the image.
    }
    logical_width = right - x;
    logical_height = bottom - y;
    observed_output = current.surface_address;
    const auto& info = proof->display_information();
    const auto u16 = [&](size_t at) {
      return uint32_t(info[at]) << 8 | uint32_t(info[at + 1]);
    };
    const auto u32 = [&](size_t at) {
      return uint32_t(info[at]) << 24 | uint32_t(info[at + 1]) << 16 |
             uint32_t(info[at + 2]) << 8 | uint32_t(info[at + 3]);
    };
    // Current real VdGetCurrentDisplayInformation writer provides this
    // identity scaler profile. Refuse unknown scaling rather than deriving
    // a canvas from the unrelated game swap texture or raw surface pitch.
    const uint32_t canvas_width = u16(0), canvas_height = u16(2);
    if (!canvas_width || !canvas_height || u32(8) || u32(12) ||
        u32(16) != canvas_width || u32(20) != canvas_height ||
        u32(24) != canvas_width || u32(28) != canvas_height ||
        u16(0x48) != canvas_width || u16(0x4A) != canvas_height ||
        current.control1 != (uint32_t(info[4] != 0) << 24)) {
      return {};
    }
    presentation = NativeOverlayPresentation{
        0, 0, int32_t(x), int32_t(y), canvas_width, canvas_height,
        logical_width, logical_height, 255};
    if (!presentation->valid() || current.pitch != overlay_state->pitch ||
        current.surface_address != overlay_state->surface_address ||
        current.start != overlay_state->start || current.end != overlay_state->end) {
      return {};
    }
  } else if (proof->output_origin() != Origin::kVdTexture ||
             word(12) != uint32_t(xenos::TextureFormat::k_8_8_8_8)) {
    return {};
  }
  if (!mapping(proof->primary_mapping(), word(0)) ||
      !mapping(proof->output_mapping(), observed_output) ||
      proof->output_mapping().physical != output.address ||
      (observed_output & 0xFFF) || word(4) != plan.words().size() ||
      (word(4) & 0xFF000000)) {
    return {};
  }
  const memory::PhysicalRange primary{proof->primary_mapping().physical,
                                      uint32_t(plan.words().size() * 4)};
  if (!covered(primary) || Overlaps(primary, output)) {
    return {};
  }
  const auto& draw = plan.inventory().draws.back();
  if (proof->copy_packet_dword_offset() != draw.packet_dword_offset) {
    return {};
  }
  RegisterFile regs;
  std::bitset<RegisterFile::kRegisterCount> known;
  for (const auto& write : plan.register_writes()) {
    if (write.packet_dword_offset > draw.packet_dword_offset) {
      break;
    }
    regs.values[write.index] = write.value;
    known.set(write.index);
  }
  for (uint32_t reg :
       {XE_GPU_REG_RB_SURFACE_INFO, XE_GPU_REG_RB_MODECONTROL,
        XE_GPU_REG_RB_COPY_CONTROL, XE_GPU_REG_RB_COPY_DEST_BASE,
        XE_GPU_REG_RB_COPY_DEST_INFO, XE_GPU_REG_RB_COPY_DEST_PITCH,
        XE_GPU_REG_RB_COPY_SURFACE_SLICE, XE_GPU_REG_PA_SU_VTX_CNTL,
        XE_GPU_REG_PA_SC_WINDOW_OFFSET, XE_GPU_REG_PA_SC_WINDOW_SCISSOR_TL,
        XE_GPU_REG_PA_SC_WINDOW_SCISSOR_BR, XE_GPU_REG_PA_SC_SCREEN_SCISSOR_TL,
        XE_GPU_REG_PA_SC_SCREEN_SCISSOR_BR}) {
    if (!known[reg]) {
      return {};
    }
  }
  const auto vertices = NativeDrawReadInventory::VertexReads(draw, nullptr);
  if (vertices.error != NativeDrawReadInventory::Error::kNone ||
      vertices.reads.size() != 1 || !vertices.reads[0].resolve_rectangle ||
      vertices.reads[0].byte_count != 24) {
    return {};
  }
  const auto& read = vertices.reads[0];
  if (proof->rectangle().address != read.physical_address ||
      proof->rectangle().length != 24 ||
      proof->rectangle_endian() != read.endian || read.endian > 3 ||
      !covered(proof->rectangle()) || Overlaps(proof->rectangle(), output)) {
    return {};
  }
  // Pure metadata from the same sealed proof and already-known RF. Capture
  // raw words before any geometry rejection; their existing endian/correction
  // can be decoded offline. No extra floating-point work or live guest read.
  if (cvars::native_system_ui_trace) {
    const auto& raw = proof->rectangle_words();
    const auto source_pitch = regs.Get<reg::RB_SURFACE_INFO>();
    const auto dest_pitch = regs.Get<reg::RB_COPY_DEST_PITCH>();
    const auto& identity = proof->identity();
    static xe::NativeSystemUiTraceLimiter limiter;
    limiter.Emit(
        "gpu", "pm4_layout_boundary",
        {{"kernel_epoch", identity.owner.kernel_epoch},
         {"runtime_module_generation", identity.owner.module_generation},
         {"owner_session_id", identity.owner.session_id},
         {"root_hmodule", identity.owner.module_hmodule},
         {"source_serial", identity.source_serial},
         {"copy_packet_dword", proof->copy_packet_dword_offset()},
         {"rectangle_address", proof->rectangle().address},
         {"rectangle_length", proof->rectangle().length},
         {"rectangle_endian", read.endian},
         {"raw_rectangle_word0", raw[0]},
         {"raw_rectangle_word1", raw[1]},
         {"raw_rectangle_word2", raw[2]},
         {"raw_rectangle_word3", raw[3]},
         {"raw_rectangle_word4", raw[4]},
         {"raw_rectangle_word5", raw[5]},
         {"descriptor_width", word(20)},
         {"descriptor_height", word(24)},
         {"descriptor_texture_format", word(12)},
         {"source_pitch", uint32_t(source_pitch.surface_pitch)},
         {"source_msaa", uint32_t(source_pitch.msaa_samples)},
         {"dest_pitch", uint32_t(dest_pitch.copy_dest_pitch)},
         {"dest_height", uint32_t(dest_pitch.copy_dest_height)},
         {"surface_info_raw", regs.values[XE_GPU_REG_RB_SURFACE_INFO]},
         {"copy_dest_pitch_raw", regs.values[XE_GPU_REG_RB_COPY_DEST_PITCH]},
         {"copy_dest_info_raw", regs.values[XE_GPU_REG_RB_COPY_DEST_INFO]},
         {"window_offset_raw", regs.values[XE_GPU_REG_PA_SC_WINDOW_OFFSET]},
         {"window_scissor_tl_raw",
          regs.values[XE_GPU_REG_PA_SC_WINDOW_SCISSOR_TL]},
         {"window_scissor_br_raw",
          regs.values[XE_GPU_REG_PA_SC_WINDOW_SCISSOR_BR]},
         {"screen_scissor_tl_raw",
          regs.values[XE_GPU_REG_PA_SC_SCREEN_SCISSOR_TL]},
         {"screen_scissor_br_raw",
          regs.values[XE_GPU_REG_PA_SC_SCREEN_SCISSOR_BR]},
         {"output_address", output.address},
         {"output_length", output.length}});
  }
  std::array<float, 6> coordinates;
  for (size_t i = 0; i < coordinates.size(); ++i) {
    coordinates[i] =
        std::bit_cast<float>(xenos::GpuSwap(proof->rectangle_words()[i],
                                            xenos::Endian(read.endian))) +
        0.5f;
    if (!std::isfinite(coordinates[i])) {
      return {};
    }
  }
  const float x0 = std::min({coordinates[0], coordinates[2], coordinates[4]});
  const float x1 = std::max({coordinates[0], coordinates[2], coordinates[4]});
  const float y0 = std::min({coordinates[1], coordinates[3], coordinates[5]});
  const float y1 = std::max({coordinates[1], coordinates[3], coordinates[5]});
  // Set declares logical texture size, not the padded resolve rectangle.
  // D3DDevice_Resolve rounds right/bottom to 8 before emitting its vertices;
  // GetResolveInfo repeats that alignment after rasterization/scissor. Retain
  // the prior exact-logical public-ISA form too: both integer forms have the
  // same effective rectangle. Do not accept arbitrary over/under rectangles
  // merely because float ceil would put them in the same alignment bucket.
  const uint32_t width = logical_width, height = logical_height;
  if (!width || !height || width > xenos::kTexture2DCubeMaxWidthHeight ||
      height > xenos::kTexture2DCubeMaxWidthHeight) {
    return {};
  }
  const uint32_t resolve_width = (width + xenos::kResolveAlignmentPixels - 1) &
                                 ~uint32_t(xenos::kResolveAlignmentPixels - 1);
  const uint32_t resolve_height =
      (height + xenos::kResolveAlignmentPixels - 1) &
      ~uint32_t(xenos::kResolveAlignmentPixels - 1);
  if (x0 != 0 || y0 != 0 ||
      (x1 != float(width) && x1 != float(resolve_width)) ||
      (y1 != float(height) && y1 != float(resolve_height))) {
    return {};
  }
  NativeFrameOutputLayout layout;
  layout.origin = overlay ? NativeFrameOutputLayout::Origin::kNativeOverlayResolve
                          : NativeFrameOutputLayout::Origin::kPm4Resolve;
  layout.overlay_state = overlay_state;
  layout.overlay_presentation = presentation;
  layout.width = width;
  layout.height = height;
  const auto pitch = regs.Get<reg::RB_COPY_DEST_PITCH>();
  layout.pitch = pitch.copy_dest_pitch;
  if (overlay && (!overlay_state || layout.pitch != overlay_state->pitch ||
                  output.address != overlay_state->surface_address)) {
    return {};
  }
  const auto source = regs.Get<reg::RB_SURFACE_INFO>();
  const auto control = regs.Get<reg::RB_COPY_CONTROL>();
  const auto info = regs.Get<reg::RB_COPY_DEST_INFO>();
  const auto tl = regs.Get<reg::PA_SC_WINDOW_SCISSOR_TL>();
  const auto br = regs.Get<reg::PA_SC_WINDOW_SCISSOR_BR>();
  const auto screen_tl = regs.Get<reg::PA_SC_SCREEN_SCISSOR_TL>();
  const auto screen_br = regs.Get<reg::PA_SC_SCREEN_SCISSOR_BR>();
  if ((!overlay && (word(20) != layout.width || word(24) != layout.height)) ||
      layout.pitch < resolve_width || (layout.pitch & 31) ||
      (layout.pitch >> 5) > 0x1FF ||
      (source.surface_pitch & ~uint32_t(xenos::kResolveAlignmentPixels - 1)) <
          resolve_width ||
      source.msaa_samples != xenos::MsaaSamples::k1X ||
      regs.Get<reg::RB_MODECONTROL>().edram_mode != xenos::EdramMode::kCopy ||
      control.copy_command != xenos::CopyCommand::kConvert ||
      control.copy_src_select || control.color_clear_enable ||
      control.depth_clear_enable ||
      regs.values[XE_GPU_REG_RB_COPY_DEST_BASE] != output.address ||
      info.copy_dest_format != xenos::ColorFormat::k_8_8_8_8 ||
      uint32_t(info.copy_dest_endian) || info.copy_dest_array ||
      info.copy_dest_slice || uint32_t(info.copy_dest_number) ||
      info.copy_dest_exp_bias || !info.copy_dest_swap ||
      pitch.copy_dest_height != layout.height ||
      regs.values[XE_GPU_REG_RB_COPY_SURFACE_SLICE] ||
      regs.values[XE_GPU_REG_PA_SC_WINDOW_OFFSET] ||
      regs.Get<reg::PA_SU_VTX_CNTL>().pix_center !=
          xenos::PixelCenter::kD3DZero ||
      tl.tl_x || tl.tl_y || br.br_x < layout.width || br.br_y < layout.height ||
      screen_tl.tl_x || screen_tl.tl_y || screen_br.br_x < 0 ||
      screen_br.br_y < 0 || uint32_t(screen_br.br_x) < layout.width ||
      uint32_t(screen_br.br_y) < layout.height) {
    return {};
  }
  // With origin 0 and these integer endpoints, covering the logical rectangle
  // before GetResolveInfo's align8 gives the same effective bounds for either
  // admitted form. Source pitch is floored to 8 by GetResolveInfo, and must not
  // clip that result. Check its actual 32bpp tiled write bound independently
  // of the conservative complete-tile sample footprint below.
  const uint32_t written_extent = texture_util::GetTiledAddressUpperBound2D(
      resolve_width, resolve_height, layout.pitch, 2);
  if (!written_extent || written_extent > output.length) {
    return {};
  }
  layout.required_bytes =
      uint32_t(texture_address::Tiled2D(int32_t((layout.width - 1) & ~31u),
                                        int32_t((layout.height - 1) & ~31u),
                                        layout.pitch, 2)) +
      32 * 32 * 4;
  if (layout.required_bytes > output.length) {
    return {};
  }
  xenos::xe_gpu_texture_fetch_t fetch{};
  fetch.type = xenos::FetchConstantType::kTexture;
  fetch.format = xenos::TextureFormat::k_8_8_8_8;
  fetch.dimension = xenos::DataDimension::k2DOrStacked;
  fetch.tiled = true;
  fetch.endianness = xenos::Endian::kNone;
  fetch.sign_x = fetch.sign_y = fetch.sign_z = fetch.sign_w =
      xenos::TextureSign::kUnsigned;
  fetch.swizzle = 0x60A;
  fetch.base_address = output.address >> 12;
  fetch.pitch = layout.pitch >> 5;
  fetch.size_2d.width = layout.width - 1;
  fetch.size_2d.height = layout.height - 1;
  std::memcpy(layout.texture_fetch.data(), &fetch, sizeof(fetch));
  layout.pm4_resolve = proof;
  if (!ValidateNativeFrameOutputStorage(layout, output)) {
    return {};
  }
  return layout;
}

NativeFrameExecutionRequest::NativeFrameExecutionRequest(
    std::shared_ptr<const NativeFrameCommandPlan> plan,
    NativeFrameSubmissionIdentity identity, uint64_t backend_epoch,
    memory::PhysicalRange output_range,
    std::vector<memory::PhysicalRange> input_ranges,
    std::vector<Shader> shaders,
    std::vector<std::shared_ptr<const memory::PhysicalRangePin>> pins,
    std::shared_ptr<NativeFrameExecutionAuthority> authority,
    std::optional<NativeFrameOutputLayout> output_layout,
    std::shared_ptr<const NativeFrameInputSnapshot> input_snapshot)
    : plan_(std::move(plan)),
      identity_(identity),
      backend_epoch_(backend_epoch),
      output_range_(output_range),
      output_layout_(std::move(output_layout)),
      input_ranges_(std::move(input_ranges)),
      shaders_(std::move(shaders)),
      pins_(std::move(pins)),
      authority_(std::move(authority)),
      input_snapshot_(std::move(input_snapshot)) {}

bool NativeFrameExecutionRequest::ResourcesValid() const {
  const bool control = plan_ && plan_->is_overlay_control();
  if (!identity_.valid() || !backend_epoch_ || !plan_ || !authority_ ||
      (control
           ? (output_range_.address || output_range_.length || output_layout_ ||
              !shaders_.empty() || !plan_->inventory().draws.empty() ||
              plan_->has_final_copy() || plan_->has_cache_flush() ||
              !plan_->native_overlay_program())
           : !Valid(output_range_)) ||
      input_ranges_.empty() || input_ranges_.size() > 512 || pins_.empty() ||
      pins_.size() > 512 ||
      shaders_.size() != plan_->inventory().shaders.size() ||
      (output_layout_ &&
       (!plan_->has_final_copy() ||
        !ValidOutputLayout(*output_layout_, output_range_, *plan_,
                           input_ranges_, identity_)))) {
    return false;
  }
  if (input_snapshot_ &&
      (!input_snapshot_->valid() || input_snapshot_->identity() != identity_ ||
       input_snapshot_->plan() != plan_ ||
       input_snapshot_->output_range().address != output_range_.address ||
       input_snapshot_->output_range().length != output_range_.length)) {
    return false;
  }
  for (const auto& pin : pins_) {
    if (!pin || !pin->valid() || pin->allocations().empty()) {
      return false;
    }
    for (const auto& allocation : pin->allocations()) {
      if (allocation.owner != identity_.owner) {
        return false;
      }
    }
  }
  const auto owned = [&](memory::PhysicalRange range) {
    if (!Valid(range)) {
      return false;
    }
    for (const auto& pin : pins_) {
      for (const auto& allocation : pin->allocations()) {
        if (range.address >= allocation.physical_address &&
            uint64_t(range.address) + range.length <=
                uint64_t(allocation.physical_address) + allocation.length) {
          return true;
        }
      }
    }
    return false;
  };
  const auto& title = plan_->title_input();
  if (title &&
      (!input_snapshot_ || input_snapshot_->title_input() != title ||
       !title->valid() || title->identity().backend_epoch != backend_epoch_ ||
       Overlaps(title->range(), output_range_))) {
    return false;
  }
  const auto imported = [&](memory::PhysicalRange range) {
    return title && title->Covers(range);
  };
  const auto covered = [&](memory::PhysicalRange range) {
    return owned(range) || imported(range);
  };
  if (!control && !owned(output_range_)) {
    return false;
  }
  for (const auto& range : input_ranges_) {
    if (!covered(range) || Overlaps(range, output_range_)) {
      return false;
    }
  }
  if (input_snapshot_) {
    if (output_layout_) {
      if ((output_layout_->origin !=
               NativeFrameOutputLayout::Origin::kPm4Resolve &&
           output_layout_->origin !=
               NativeFrameOutputLayout::Origin::kNativeOverlayResolve) ||
          !output_layout_->pm4_resolve) {
        return false;
      }
      std::array<uint32_t, 6> words;
      if (!input_snapshot_->Read(
              output_layout_->pm4_resolve->rectangle(),
              std::span(reinterpret_cast<uint8_t*>(words.data()),
                        sizeof(words))) ||
          words != output_layout_->pm4_resolve->rectangle_words()) {
        return false;
      }
    }
    for (const auto& block : input_snapshot_->blocks()) {
      if (!owned(block.range) || Overlaps(block.range, output_range_) ||
          (title && Overlaps(block.range, title->range()))) {
        return false;
      }
    }
    for (const auto range : input_ranges_) {
      if (!input_snapshot_->Covers(range)) {
        return false;
      }
    }
  }
  for (size_t i = 0; i < shaders_.size(); ++i) {
    const auto& shader = shaders_[i];
    const auto& load = plan_->inventory().shaders[i];
    if (shader.load.stage != load.stage ||
        shader.load.packet_dword_offset != load.packet_dword_offset ||
        shader.load.dword_count != load.dword_count ||
        shader.load.is_inline != load.is_inline ||
        shader.load.inline_dword_offset != load.inline_dword_offset ||
        shader.load.raw_address != load.raw_address ||
        shader.load.physical_address != load.physical_address ||
        shader.code.size() != size_t(load.dword_count) * 4 ||
        !shader.metadata.metadata_analyzed ||
        shader.metadata.error != NativeShaderReadAnalysis::Error::kNone ||
        shader.metadata.stage != load.stage) {
      return false;
    }
    if (input_snapshot_ && !load.is_inline) {
      std::array<uint8_t, 256> bytes;
      for (size_t offset = 0; offset < shader.code.size();
           offset += bytes.size()) {
        const size_t length =
            std::min(bytes.size(), shader.code.size() - offset);
        if (!input_snapshot_->Read(
                {load.physical_address + uint32_t(offset), uint32_t(length)},
                std::span(bytes).first(length)) ||
            !std::equal(bytes.begin(), bytes.begin() + length,
                        shader.code.begin() + offset)) {
          return false;
        }
      }
    }
    if (load.is_inline) {
      if (load.inline_dword_offset > plan_->words().size() ||
          load.dword_count > plan_->words().size() - load.inline_dword_offset) {
        return false;
      }
      for (uint32_t word = 0; word < load.dword_count; ++word) {
        const size_t byte = size_t(word) * 4;
        const uint32_t value = uint32_t(shader.code[byte]) << 24 |
                               uint32_t(shader.code[byte + 1]) << 16 |
                               uint32_t(shader.code[byte + 2]) << 8 |
                               shader.code[byte + 3];
        if (value != plan_->words()[load.inline_dword_offset + word]) {
          return false;
        }
      }
    } else if (!owned({load.physical_address, load.dword_count * 4}) ||
               (title && Overlaps({load.physical_address, load.dword_count * 4},
                                  title->range()))) {
      return false;
    }
  }
  if (input_snapshot_) {
    for (const auto& draw : plan_->inventory().draws) {
      const auto selected = [&](std::optional<uint32_t> index,
                                xenos::ShaderType stage) -> const Shader* {
        return index && *index < shaders_.size() &&
                       shaders_[*index].load.stage == stage
                   ? &shaders_[*index]
                   : nullptr;
      };
      const auto* vertex =
          selected(draw.vertex_shader, xenos::ShaderType::kVertex);
      const auto* pixel =
          selected(draw.pixel_shader, xenos::ShaderType::kPixel);
      const auto vertices = NativeDrawReadInventory::VertexReads(
          draw, vertex ? &vertex->metadata : nullptr);
      const auto textures = NativeDrawReadInventory::TextureReads(
          draw, pixel ? &pixel->metadata : nullptr);
      const auto constants = NativeDrawReadInventory::ConstantReads(
          draw, vertex ? &vertex->metadata : nullptr,
          pixel ? &pixel->metadata : nullptr);
      const auto bounds = NativeVertexAccessAnalysis::Analyze(
          draw, vertex ? std::span<const uint8_t>(vertex->code)
                       : std::span<const uint8_t>{});
      if (vertices.error != NativeDrawReadInventory::Error::kNone ||
          textures.error != NativeDrawReadInventory::Error::kNone ||
          constants.error != NativeDrawReadInventory::Error::kNone ||
          !constants.used_constant_values_known ||
          bounds.error != NativeVertexAccessAnalysis::Error::kNone ||
          !bounds.vertex_input_bounds_verified) {
        return false;
      }
      if (draw.title_input != title) {
        return false;
      }
      for (const auto& read : vertices.reads) {
        if (!owned({read.physical_address, read.byte_count}) ||
            (title && Overlaps({read.physical_address, read.byte_count},
                               title->range())) ||
            !input_snapshot_->Covers(
                {read.physical_address, read.byte_count})) {
          return false;
        }
      }
      for (const auto& read : textures.reads) {
        if ((read.from_title_input
                 ? !imported({read.physical_address, read.byte_count})
                 : (!owned({read.physical_address, read.byte_count}) ||
                    (title && Overlaps({read.physical_address, read.byte_count},
                                       title->range())))) ||
            !input_snapshot_->Covers(
                {read.physical_address, read.byte_count})) {
          return false;
        }
      }
    }
  }
  return true;
}

bool NativeFrameOperationsMatchPlan(const NativeFrameCommandPlan& plan,
                                    const NativeFrameExecutionResult& result) {
  const uint64_t draws =
      uint64_t(result.draw_commands) + result.indexed_draw_commands;
  if (plan.is_overlay_control()) {
    return plan.inventory().draws.empty() && plan.inventory().shaders.empty() &&
           !plan.has_final_copy() && !plan.has_cache_flush() &&
           plan.native_overlay_program() && !draws && !result.copy_operations &&
           !result.clear_operations && !result.written_range.address &&
           !result.written_range.length;
  }
  return !plan.inventory().draws.empty() &&
         draws == plan.inventory().draws.size() - 1 && result.copy_operations &&
         result.written_range.length;
}

bool NativeFrameExecutionRequest::IsAdmitted() const {
  return ResourcesValid() && authority_->IsAdmitted(*this);
}
bool NativeFrameExecutionRequest::MarkSubmissionAttempted() const {
  return ResourcesValid() && authority_->MarkSubmissionAttempted(*this);
}
bool NativeFrameExecutionRequest::IsPresentationOwnerLive() const noexcept {
  try {
    return ResourcesValid() && authority_->IsPresentationOwnerLive(*this);
  } catch (...) {
    return false;
  }
}
std::unique_ptr<NativeFrameReadbackAdmission>
NativeFrameExecutionRequest::AcquireCompletedReadback(
    const GpuCompletion& native_completion) const {
  const auto tag = native_completion.native_frame_identity();
  if (!ResourcesValid() || plan_->is_overlay_control() ||
      native_completion.status() != GpuCompletionStatus::kComplete ||
      native_completion.backend_epoch() != backend_epoch_ || !tag ||
      *tag != identity_) {
    return {};
  }
  // The authority implementation is default-deny until a real Session owner
  // reserves its operation across exact-source validation and the whole copy.
  return authority_->AcquireCompletedReadback(*this, native_completion);
}

}  // namespace xe::gpu
