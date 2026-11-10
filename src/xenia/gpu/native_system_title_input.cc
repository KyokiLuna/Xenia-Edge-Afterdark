/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia contributors. All rights reserved.                    *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */
#include "xenia/gpu/native_system_title_input.h"

#include <cstring>
#include <utility>

#include "xenia/gpu/texture_util.h"

namespace xe::gpu {
namespace {
constexpr uint32_t kPhysicalSize = 0x20000000;

bool SameSwap(const NativeSystemTitleInput::Swap& a,
              const NativeSystemTitleInput::Swap& b) noexcept {
  return a.foreground_generation == b.foreground_generation &&
         a.title_hmodule == b.title_hmodule &&
         a.title_load_generation == b.title_load_generation &&
         a.fetch == b.fetch && a.width == b.width && a.height == b.height;
}

bool SameLayout(const NativeSystemTitleInput::Layout& a,
                const NativeSystemTitleInput::Layout& b) noexcept {
  return SameSwap(a.swap, b.swap) && a.fetch == b.fetch &&
         a.range.address == b.range.address &&
         a.range.length == b.range.length && a.width == b.width &&
         a.height == b.height && a.pitch == b.pitch &&
         a.row_pitch_bytes == b.row_pitch_bytes &&
         a.data_extent_bytes == b.data_extent_bytes && a.tiled == b.tiled &&
         a.endianness == b.endianness;
}
}  // namespace

bool NativeSystemTitleInput::Identity::valid() const noexcept {
  return backend_epoch && foreground_generation && title_hmodule &&
         title_load_generation && swap_serial;
}

std::optional<NativeSystemTitleInput::Layout> NativeSystemTitleInput::GetLayout(
    const Swap& swap) noexcept {
  if (!swap.foreground_generation || !swap.title_hmodule ||
      !swap.title_load_generation || !swap.width || !swap.height ||
      swap.width > xenos::kTexture2DCubeMaxWidthHeight ||
      swap.height > xenos::kTexture2DCubeMaxWidthHeight) {
    return std::nullopt;
  }
  xenos::xe_gpu_texture_fetch_t fetch;
  std::memcpy(&fetch, swap.fetch.data(), sizeof(fetch));
  const uint32_t pitch = uint32_t(fetch.pitch) << 5;
  const uint32_t base = uint32_t(fetch.base_address) << 12;
  const auto base_format = GetBaseFormat(fetch.format);
  // Match the base-format families of normal front buffers accepted by VdSwap.
  // Preserve each semantic alias in the fetch. Format
  // expansion is sampling interpretation, not larger guest storage: use the
  // same GetBaseFormat remap as TextureCache::BindingInfoFromFetchConstant.
  if (fetch.type != xenos::FetchConstantType::kTexture ||
      fetch.dimension != xenos::DataDimension::k2DOrStacked || fetch.stacked ||
      (base_format != xenos::TextureFormat::k_8_8_8_8 &&
       base_format != xenos::TextureFormat::k_2_10_10_10) ||
      uint32_t(fetch.size_2d.width) + 1 != swap.width ||
      uint32_t(fetch.size_2d.height) + 1 != swap.height || !base ||
      base >= kPhysicalSize) {
    return std::nullopt;
  }
  uint32_t width_minus_1, height_minus_1, depth_minus_1;
  uint32_t base_page, mip_page, mip_min, mip_max;
  texture_util::GetSubresourcesFromFetchConstant(
      fetch, &width_minus_1, &height_minus_1, &depth_minus_1, &base_page,
      &mip_page, &mip_min, &mip_max);
  // One effective base resource only. Unused raw mip fields and non-stacked
  // stack_depth are preserved exactly as TextureCache ignores them. A packed
  // base tail is supported through the authoritative helper below; additional
  // effective mip levels, arrays and volumes need separate read coverage.
  if (width_minus_1 + 1 != swap.width || height_minus_1 + 1 != swap.height ||
      depth_minus_1 || base_page != fetch.base_address || mip_page || mip_min ||
      mip_max) {
    return std::nullopt;
  }
  const auto storage = texture_util::GetGuestTextureLayout(
      xenos::DataDimension::k2DOrStacked, fetch.pitch, swap.width, swap.height,
      1, fetch.tiled, base_format, fetch.packed_mips, true, 0);
  const uint32_t data_extent = storage.base.level_data_extent_bytes;
  // TextureCache::LoadTextureData/RequestGuestRanges and Vulkan source
  // descriptors align this extent to16 for real 128-bit shader loads.
  const uint64_t read_extent = (uint64_t(data_extent) + 15) & ~UINT64_C(15);
  if (!data_extent || read_extent > kMaxRawBytes ||
      read_extent > kPhysicalSize - base || storage.array_size != 1 ||
      storage.max_level || storage.mips_total_extent_bytes) {
    return std::nullopt;
  }
  // Only sampler policy changes. Storage/address/endian/swizzle are untouched.
  // Request-size hints do not change TextureKey/GetGuestTextureLayout storage;
  // nearest-clamp policy is irrelevant to this explicit linear sampler.
  fetch.clamp_x = fetch.clamp_y = xenos::ClampMode::kClampToEdge;
  fetch.mag_filter = fetch.min_filter = xenos::TextureFilter::kLinear;
  Layout result;
  result.swap = swap;
  std::memcpy(result.fetch.data(), &fetch, sizeof(fetch));
  result.range = {base, uint32_t(read_extent)};
  result.width = swap.width;
  result.height = swap.height;
  result.pitch = pitch;
  result.row_pitch_bytes = storage.base.row_pitch_bytes;
  result.data_extent_bytes = data_extent;
  result.tiled = fetch.tiled != 0;
  result.endianness = fetch.endianness;
  return result;
}

bool NativeSystemTitleInput::Validate(const Identity& identity,
                                      const Layout& layout,
                                      std::span<const uint8_t> bytes) noexcept {
  if (!identity.valid() ||
      identity.foreground_generation != layout.swap.foreground_generation ||
      identity.title_hmodule != layout.swap.title_hmodule ||
      identity.title_load_generation != layout.swap.title_load_generation ||
      bytes.size() != layout.range.length || bytes.empty()) {
    return false;
  }
  const auto expected = GetLayout(layout.swap);
  return expected && SameLayout(layout, *expected);
}

std::shared_ptr<const NativeSystemTitleInput> NativeSystemTitleInput::Create(
    Identity identity, Layout layout, std::vector<uint8_t> bytes) {
  if (!Validate(identity, layout, bytes)) {
    return {};
  }
  return std::shared_ptr<const NativeSystemTitleInput>(
      new NativeSystemTitleInput(identity, std::move(layout),
                                 std::move(bytes)));
}

NativeSystemTitleInput::NativeSystemTitleInput(Identity identity, Layout layout,
                                               std::vector<uint8_t> bytes)
    : identity_(identity),
      layout_(std::move(layout)),
      bytes_(std::move(bytes)),
      valid_(Validate(identity_, layout_, bytes_)) {}

bool NativeSystemTitleInput::MatchesIdentity(
    const Identity& expected) const noexcept {
  return valid_ && identity_ == expected;
}

bool NativeSystemTitleInput::Covers(
    memory::PhysicalRange range) const noexcept {
  return valid_ && range.length && range.address >= layout_.range.address &&
         uint64_t(range.address) + range.length <=
             uint64_t(layout_.range.address) + layout_.range.length;
}

bool NativeSystemTitleInput::Read(
    memory::PhysicalRange range,
    std::span<uint8_t> destination) const noexcept {
  if (!destination.data() || destination.size() != range.length ||
      !Covers(range)) {
    return false;
  }
  std::memcpy(destination.data(),
              bytes_.data() + (range.address - layout_.range.address),
              range.length);
  return true;
}
}  // namespace xe::gpu
