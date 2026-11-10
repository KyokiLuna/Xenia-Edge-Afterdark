/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia contributors. All rights reserved.                    *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */
#ifndef XENIA_GPU_NATIVE_SYSTEM_TITLE_INPUT_H_
#define XENIA_GPU_NATIVE_SYSTEM_TITLE_INPUT_H_

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include "xenia/base/physical_range_leases.h"
#include "xenia/gpu/xenos.h"

namespace xe::gpu::vulkan {
class VulkanCommandProcessor;
}
namespace xe::gpu::test {
class NativeSystemTitleInputTestAccess;
}
namespace xe::gpu {

// Immutable raw texture storage produced by the real title GPU path. Layout
// validation alone grants no GPU, Source, foreground or completion authority.
class NativeSystemTitleInput final {
 public:
  static constexpr uint32_t kMaxRawBytes = 8 * 1024 * 1024;
  struct Identity {
    uint64_t backend_epoch = 0;
    uint64_t foreground_generation = 0;
    uint32_t title_hmodule = 0;
    uint64_t title_load_generation = 0;
    uint64_t swap_serial = 0;
    bool valid() const noexcept;
    bool operator==(const Identity&) const = default;
  };
  struct Swap {
    uint64_t foreground_generation = 0;
    uint32_t title_hmodule = 0;
    uint64_t title_load_generation = 0;
    // Actual host-canonical GPU fetch after the kernel's physical translation.
    // This class never translates a CPU alias or guesses a physical address.
    std::array<uint32_t, 6> fetch{};
    uint32_t width = 0;
    uint32_t height = 0;
  };
  struct Layout {
    Swap swap;
    // Host-only sampler normalization: edge clamp XY and linear min/mag.
    std::array<uint32_t, 6> fetch{};
    // Actual TextureCache read coverage, including its16-byte load rounding.
    memory::PhysicalRange range;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t pitch = 0;
    uint32_t row_pitch_bytes = 0;
    // GetGuestTextureLayout base extent, including known tiled/packed padding.
    uint32_t data_extent_bytes = 0;
    bool tiled = false;
    xenos::Endian endianness = xenos::Endian::kNone;
  };

  static std::optional<Layout> GetLayout(const Swap& swap) noexcept;
  const Identity& identity() const noexcept { return identity_; }
  const Layout& layout() const noexcept { return layout_; }
  const std::array<uint32_t, 6>& fetch() const noexcept {
    return layout_.fetch;
  }
  const memory::PhysicalRange& range() const noexcept { return layout_.range; }
  std::span<const uint8_t> bytes() const noexcept { return bytes_; }
  bool valid() const noexcept { return valid_; }
  bool MatchesIdentity(const Identity& expected) const noexcept;
  bool Covers(memory::PhysicalRange range) const noexcept;
  // Failure does not modify destination. No guest RAM is read by this class.
  bool Read(memory::PhysicalRange range,
            std::span<uint8_t> destination) const noexcept;

 private:
  friend class vulkan::VulkanCommandProcessor;
  friend class test::NativeSystemTitleInputTestAccess;
  // The production caller may mint only after its actual GPU copy fence has
  // completed. There is deliberately no public bool/token factory shortcut.
  static std::shared_ptr<const NativeSystemTitleInput> Create(
      Identity identity, Layout layout, std::vector<uint8_t> bytes);
  NativeSystemTitleInput(Identity identity, Layout layout,
                         std::vector<uint8_t> bytes);
  static bool Validate(const Identity& identity, const Layout& layout,
                       std::span<const uint8_t> bytes) noexcept;

  const Identity identity_;
  const Layout layout_;
  const std::vector<uint8_t> bytes_;
  const bool valid_;
};
}  // namespace xe::gpu
#endif
