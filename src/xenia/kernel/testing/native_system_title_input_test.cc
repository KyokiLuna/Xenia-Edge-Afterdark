/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia contributors. All rights reserved.                    *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */
#include <array>
#include <cstdint>
#include <cstring>
#include <functional>
#include <utility>
#include <vector>

#include "third_party/catch/include/catch.hpp"
#include "xenia/gpu/native_system_title_input.h"
#include "xenia/gpu/native_system_title_input_provider.h"
#include "xenia/kernel/testing/native_system_title_input_test_access.h"

namespace xe::gpu::test {
namespace {
using Input = NativeSystemTitleInput;
using Fetch = xenos::xe_gpu_texture_fetch_t;

Input::Swap MakeSwap(uint32_t width = 64, uint32_t height = 33,
                     uint32_t pitch = 64, bool tiled = true,
                     uint32_t address = 0x01000000) {
  Fetch fetch{};
  fetch.type = xenos::FetchConstantType::kTexture;
  fetch.pitch = pitch >> 5;
  fetch.tiled = tiled;
  fetch.format = xenos::TextureFormat::k_8_8_8_8;
  fetch.base_address = address >> 12;
  fetch.dimension = xenos::DataDimension::k2DOrStacked;
  fetch.size_2d.width = width - 1;
  fetch.size_2d.height = height - 1;
  fetch.swizzle = 0x688;
  Input::Swap swap;
  swap.foreground_generation = 17;
  swap.title_hmodule = 0x3001C000;
  swap.title_load_generation = 23;
  swap.width = width;
  swap.height = height;
  std::memcpy(swap.fetch.data(), &fetch, sizeof(fetch));
  return swap;
}

Input::Identity MakeIdentity(const Input::Swap& swap) {
  return {41, swap.foreground_generation, swap.title_hmodule,
          swap.title_load_generation, 7};
}

void ModifyFetch(Input::Swap& swap, const std::function<void(Fetch&)>& mutate) {
  Fetch fetch;
  std::memcpy(&fetch, swap.fetch.data(), sizeof(fetch));
  mutate(fetch);
  std::memcpy(swap.fetch.data(), &fetch, sizeof(fetch));
}
}  // namespace

TEST_CASE("Native title input derives exact linear raw extent and sampler",
          "[native_title_input]") {
  auto swap = MakeSwap(33, 2, 64, false);
  ModifyFetch(swap, [](Fetch& fetch) {
    fetch.clamp_x = xenos::ClampMode::kRepeat;
    fetch.clamp_y = xenos::ClampMode::kMirroredRepeat;
    fetch.clamp_z = xenos::ClampMode::kMirrorClampToEdge;
    fetch.request_size = 2;
    fetch.nearest_clamp_policy = 1;
  });
  const auto original = swap.fetch;
  const auto layout = Input::GetLayout(swap);
  REQUIRE(layout.has_value());
  // One256-byte row plus33 texels is388 bytes; actual GPU loads round to400.
  REQUIRE(layout->data_extent_bytes == 388);
  REQUIRE(layout->range.length == 400);
  REQUIRE(layout->row_pitch_bytes == 256);
  REQUIRE(layout->pitch == 64);
  REQUIRE(layout->width == 33);
  REQUIRE(layout->height == 2);
  REQUIRE_FALSE(layout->tiled);
  REQUIRE(swap.fetch == original);
  REQUIRE(layout->swap.fetch == original);
  Fetch normalized;
  std::memcpy(&normalized, layout->fetch.data(), sizeof(normalized));
  REQUIRE(normalized.clamp_x == xenos::ClampMode::kClampToEdge);
  REQUIRE(normalized.clamp_y == xenos::ClampMode::kClampToEdge);
  REQUIRE(normalized.clamp_z == xenos::ClampMode::kMirrorClampToEdge);
  REQUIRE(normalized.mag_filter == xenos::TextureFilter::kLinear);
  REQUIRE(normalized.min_filter == xenos::TextureFilter::kLinear);
  REQUIRE(normalized.mip_filter == xenos::TextureFilter::kPoint);
  REQUIRE(normalized.request_size == 2);
  REQUIRE(normalized.nearest_clamp_policy == 1);
  REQUIRE((uint32_t(normalized.base_address) << 12) == layout->range.address);
}

TEST_CASE("Native title input retains tiled extent endian and channel order",
          "[native_title_input]") {
  for (uint32_t endian = 0; endian < 4; ++endian) {
    for (const uint32_t swizzle : {0x688u, 0x60Au}) {
      auto swap = MakeSwap();
      ModifyFetch(swap, [&](Fetch& fetch) {
        fetch.endianness = xenos::Endian(endian);
        fetch.swizzle = swizzle;
      });
      const auto layout = Input::GetLayout(swap);
      REQUIRE(layout.has_value());
      // Four complete32x32 RGBA tiles cover a64x33 visible rectangle.
      REQUIRE(layout->range.length == 16384);
      REQUIRE(layout->row_pitch_bytes == 256);
      REQUIRE(layout->tiled);
      REQUIRE(layout->endianness == xenos::Endian(endian));
      REQUIRE((layout->fetch[3] >> 1 & 0xFFF) == swizzle);
    }
  }
}

TEST_CASE("Native title input bounds real storage and canonical physical base",
          "[native_title_input]") {
  const auto at_limit = Input::GetLayout(MakeSwap(2048, 1024, 2048, false));
  REQUIRE(at_limit.has_value());
  REQUIRE(at_limit->range.length == Input::kMaxRawBytes);
  REQUIRE_FALSE(Input::GetLayout(MakeSwap(2048, 1025, 2048, false)));
  REQUIRE_FALSE(Input::GetLayout(MakeSwap(8192, 8192, 8192, true)));
  const auto last_page =
      Input::GetLayout(MakeSwap(32, 32, 32, true, 0x1FFFF000));
  REQUIRE(last_page.has_value());
  REQUIRE(uint64_t(last_page->range.address) + last_page->range.length ==
          0x20000000);
  REQUIRE_FALSE(Input::GetLayout(MakeSwap(64, 32, 64, true, 0x1FFFF000)));
  REQUIRE_FALSE(Input::GetLayout(MakeSwap(64, 32, 64, true, 0)));
  REQUIRE_FALSE(Input::GetLayout(MakeSwap(64, 32, 64, true, 0xFE600000)));
  auto inconsistent = MakeSwap();
  ++inconsistent.width;
  REQUIRE_FALSE(Input::GetLayout(inconsistent));
  // Existing TextureCache supports width beyond pitch; retain its exact span.
  const auto overlapping_rows = Input::GetLayout(MakeSwap(128, 32, 64, false));
  REQUIRE(overlapping_rows.has_value());
  REQUIRE(overlapping_rows->data_extent_bytes == 8448);
  REQUIRE(overlapping_rows->range.length == 8448);
  auto empty = MakeSwap();
  empty.width = 0;
  REQUIRE_FALSE(Input::GetLayout(empty));
}

TEST_CASE(
    "Native title input preserves front-buffer formats and packed storage",
    "[native_title_input]") {
  for (const auto format :
       {xenos::TextureFormat::k_8_8_8_8,
        xenos::TextureFormat::k_8_8_8_8_AS_16_16_16_16,
        xenos::TextureFormat::k_2_10_10_10,
        xenos::TextureFormat::k_2_10_10_10_AS_16_16_16_16}) {
    auto swap = MakeSwap(33, 2, 64, false);
    ModifyFetch(swap, [&](Fetch& f) {
      f.format = format;
      f.sign_x = xenos::TextureSign::kGamma;
      f.sign_y = xenos::TextureSign::kSigned;
      f.num_format = 1;
      f.exp_adjust = 3;
      f.border_color = xenos::BorderColor::k_ABGR_White;
      f.aniso_filter = xenos::AnisoFilter::kMax_4_1;
      f.arbitrary_filter = xenos::ArbitraryFilter::k4x4Sym;
      f.lod_bias = 9;
      f.grad_exp_adjust_h = 2;
      f.grad_exp_adjust_v = -1;
      f.border_size = 1;
      f._pad_0_19 = 5;
      f.size_2d.stack_depth =
          27;  // Ignored by actual non-stacked key creation.
      f.vol_mag_filter = f.vol_min_filter = 1;
      f.mag_aniso_walk = f.min_aniso_walk = 1;
      f.force_bc_w_to_max = 1;
      f.tri_clamp = 2;
      f.aniso_bias = -1;
      f.clamp_z = xenos::ClampMode::kClampToBorder;
      f.mip_filter = xenos::TextureFilter::kLinear;
      f.mip_min_level = f.mip_max_level =
          5;  // No mip address: effective base only.
    });
    const auto layout = Input::GetLayout(swap);
    REQUIRE(layout.has_value());
    // Both guest format families store four bytes, including the expansion
    // alias.
    REQUIRE(layout->data_extent_bytes == 388);
    REQUIRE(layout->range.length == 400);
    auto expected = swap.fetch;
    Fetch normalized;
    std::memcpy(&normalized, expected.data(), sizeof(normalized));
    normalized.clamp_x = normalized.clamp_y = xenos::ClampMode::kClampToEdge;
    normalized.mag_filter = normalized.min_filter =
        xenos::TextureFilter::kLinear;
    std::memcpy(expected.data(), &normalized, sizeof(normalized));
    REQUIRE(layout->fetch == expected);  // Every unconsumed bit is preserved.
  }
  auto packed = MakeSwap(8, 8, 64, false);
  ModifyFetch(packed, [](Fetch& f) { f.packed_mips = 1; });
  const auto packed_layout = Input::GetLayout(packed);
  REQUIRE(packed_layout.has_value());
  // Packed base at(16,0): seven256-byte rows plus24 RGBA columns, not8*8*4.
  REQUIRE(packed_layout->data_extent_bytes == 1888);
  REQUIRE(packed_layout->range.length == 1888);
  packed = MakeSwap(8, 8, 64, true);
  ModifyFetch(packed, [](Fetch& f) { f.packed_mips = 1; });
  const auto packed_tiled = Input::GetLayout(packed);
  REQUIRE(packed_tiled.has_value());
  REQUIRE(packed_tiled->range.length == 4096);
}

TEST_CASE(
    "Native title input rejects multiple resources and non-front-buffer "
    "formats",
    "[native_title_input]") {
  const std::vector<std::function<void(Fetch&)>> unsupported{
      [](Fetch& f) { f.type = xenos::FetchConstantType::kInvalidTexture; },
      [](Fetch& f) { f.dimension = xenos::DataDimension::k3D; },
      [](Fetch& f) { f.dimension = xenos::DataDimension::kCube; },
      [](Fetch& f) { f.stacked = 1; },
      [](Fetch& f) { f.format = xenos::TextureFormat::k_16_16_16_16_FLOAT; },
      [](Fetch& f) {
        f.mip_address = 1;
        f.mip_max_level = 1;
      },
      [](Fetch& f) {
        f.mip_address = 1;
        f.mip_min_level = 1;
      },
  };
  for (size_t i = 0; i < unsupported.size(); ++i) {
    CAPTURE(i);
    auto swap = MakeSwap();
    ModifyFetch(swap, unsupported[i]);
    REQUIRE_FALSE(Input::GetLayout(swap));
  }
}

TEST_CASE("Native title input owns exact bytes and never writes on failed read",
          "[native_title_input]") {
  const auto swap = MakeSwap(33, 2, 64, false);
  const auto layout = Input::GetLayout(swap);
  REQUIRE(layout.has_value());
  std::vector<uint8_t> source(layout->range.length);
  for (size_t i = 0; i < source.size(); ++i) {
    source[i] = uint8_t(i * 7 + 3);
  }
  const auto expected = source;
  const auto input = NativeSystemTitleInputTestAccess::Create(
      MakeIdentity(swap), *layout, source);
  REQUIRE(input);
  REQUIRE(input->valid());
  source.assign(source.size(), 0xCC);
  REQUIRE(std::vector<uint8_t>(input->bytes().begin(), input->bytes().end()) ==
          expected);
  REQUIRE(input->Covers(layout->range));
  std::array<uint8_t, 8> destination;
  destination.fill(0xA5);
  REQUIRE(input->Read({layout->range.address + 17, 8}, destination));
  for (size_t i = 0; i < destination.size(); ++i) {
    REQUIRE(destination[i] == expected[17 + i]);
  }
  destination.fill(0xA5);
  const auto before = destination;
  REQUIRE_FALSE(input->Read({layout->range.address - 1, 8}, destination));
  REQUIRE(destination == before);
  REQUIRE_FALSE(input->Read(
      {layout->range.address + layout->range.length - 7, 8}, destination));
  REQUIRE(destination == before);
  REQUIRE_FALSE(input->Read({UINT32_MAX - 1, 8}, destination));
  REQUIRE(destination == before);
  REQUIRE_FALSE(input->Read({layout->range.address, 7}, destination));
  REQUIRE(destination == before);
  REQUIRE_FALSE(input->Covers({layout->range.address, 0}));
  REQUIRE_FALSE(input->Read(
      {layout->range.address, 8},
      std::span<uint8_t>(static_cast<uint8_t*>(nullptr), size_t(8))));
}

TEST_CASE("Native title input requires exact owner identity and complete bytes",
          "[native_title_input]") {
  const auto swap = MakeSwap();
  const auto layout = Input::GetLayout(swap);
  REQUIRE(layout.has_value());
  const auto identity = MakeIdentity(swap);
  const std::vector<uint8_t> bytes(layout->range.length, 0x27);
  const auto input =
      NativeSystemTitleInputTestAccess::Create(identity, *layout, bytes);
  REQUIRE(input);
  REQUIRE(input->MatchesIdentity(identity));
  for (size_t field = 0; field < 5; ++field) {
    auto stale = identity;
    switch (field) {
      case 0:
        ++stale.backend_epoch;
        break;
      case 1:
        ++stale.foreground_generation;
        break;
      case 2:
        ++stale.title_hmodule;
        break;
      case 3:
        ++stale.title_load_generation;
        break;
      case 4:
        ++stale.swap_serial;
        break;
    }
    REQUIRE_FALSE(input->MatchesIdentity(stale));
  }
  auto missing_backend = identity;
  missing_backend.backend_epoch = 0;
  REQUIRE_FALSE(NativeSystemTitleInputTestAccess::Create(missing_backend,
                                                         *layout, bytes));
  auto missing_serial = identity;
  missing_serial.swap_serial = 0;
  REQUIRE_FALSE(
      NativeSystemTitleInputTestAccess::Create(missing_serial, *layout, bytes));
  for (size_t field = 0; field < 3; ++field) {
    auto foreign = identity;
    if (field == 0) {
      ++foreign.foreground_generation;
    }
    if (field == 1) {
      ++foreign.title_hmodule;
    }
    if (field == 2) {
      ++foreign.title_load_generation;
    }
    REQUIRE_FALSE(
        NativeSystemTitleInputTestAccess::Create(foreign, *layout, bytes));
  }
  auto short_bytes = bytes;
  short_bytes.pop_back();
  REQUIRE_FALSE(
      NativeSystemTitleInputTestAccess::Create(identity, *layout, short_bytes));
  auto extra_bytes = bytes;
  extra_bytes.push_back(0);
  REQUIRE_FALSE(
      NativeSystemTitleInputTestAccess::Create(identity, *layout, extra_bytes));
  auto forged = *layout;
  ++forged.range.address;
  REQUIRE_FALSE(
      NativeSystemTitleInputTestAccess::Create(identity, forged, bytes));
  forged = *layout;
  ++forged.data_extent_bytes;
  REQUIRE_FALSE(
      NativeSystemTitleInputTestAccess::Create(identity, forged, bytes));
  forged = *layout;
  ++forged.row_pitch_bytes;
  REQUIRE_FALSE(
      NativeSystemTitleInputTestAccess::Create(identity, forged, bytes));
  forged = *layout;
  forged.fetch[3] ^= 2;
  REQUIRE_FALSE(
      NativeSystemTitleInputTestAccess::Create(identity, forged, bytes));
  forged = *layout;
  ++forged.width;
  REQUIRE_FALSE(
      NativeSystemTitleInputTestAccess::Create(identity, forged, bytes));
}
TEST_CASE("Native title provider consumes exact tickets and retains old bytes",
          "[native_title_input][native_title_input_provider]") {
  NativeSystemTitleInputProvider provider;
  const auto swap = MakeSwap(33, 2, 64, false);
  REQUIRE_FALSE(provider.enabled());
  REQUIRE(provider.Stage(swap) == 0);
  REQUIRE_FALSE(provider.Enable(0));
  REQUIRE(provider.Enable(41));
  REQUIRE(provider.Enable(41));
  REQUIRE_FALSE(provider.Enable(42));
  REQUIRE(provider.enabled());
  const auto layout = Input::GetLayout(swap);
  REQUIRE(layout.has_value());
  const auto mismatched_ticket = provider.Stage(swap);
  REQUIRE(mismatched_ticket != 0);
  REQUIRE_FALSE(provider.Take(mismatched_ticket, layout->range.address + 4096,
                              swap.width, swap.height));
  REQUIRE_FALSE(provider.Take(mismatched_ticket, layout->range.address,
                              swap.width, swap.height));
  const auto ticket = provider.Stage(swap);
  REQUIRE(ticket != 0);
  const auto capture =
      provider.Take(ticket, layout->range.address, swap.width, swap.height);
  REQUIRE(capture.has_value());
  REQUIRE_FALSE(
      provider.Take(ticket, layout->range.address, swap.width, swap.height));
  REQUIRE(capture->identity.backend_epoch == 41);
  REQUIRE(capture->identity.foreground_generation ==
          swap.foreground_generation);
  const auto first = NativeSystemTitleInputTestAccess::Create(
      capture->identity, capture->layout,
      std::vector<uint8_t>(capture->layout.range.length, 0xA5));
  REQUIRE(first);
  REQUIRE(provider.Publish(first));
  const auto held = provider.Latest(swap.foreground_generation);
  REQUIRE(held == first);
  REQUIRE_FALSE(provider.Latest(swap.foreground_generation + 1));
  const auto second_ticket = provider.Stage(swap);
  REQUIRE(second_ticket != 0);
  const auto second_capture = provider.Take(
      second_ticket, layout->range.address, swap.width, swap.height);
  REQUIRE(second_capture.has_value());
  REQUIRE(second_capture->identity.swap_serial > capture->identity.swap_serial);
  const auto second = NativeSystemTitleInputTestAccess::Create(
      second_capture->identity, second_capture->layout,
      std::vector<uint8_t>(second_capture->layout.range.length, 0xB6));
  REQUIRE(second);
  REQUIRE(provider.Publish(second));
  REQUIRE(provider.Latest(swap.foreground_generation) == second);
  REQUIRE(held->bytes()[0] == 0xA5);
  REQUIRE(second->bytes()[0] == 0xB6);
  REQUIRE(held->valid());
  REQUIRE_FALSE(provider.Publish(first));
}

TEST_CASE(
    "Native title provider rejects foreign layouts and stale publications",
    "[native_title_input][native_title_input_provider]") {
  NativeSystemTitleInputProvider provider;
  REQUIRE(provider.Enable(41));
  const auto swap = MakeSwap(33, 2, 64, false);
  const auto layout = Input::GetLayout(swap);
  REQUIRE(layout.has_value());
  const auto ticket = provider.Stage(swap);
  const auto capture =
      provider.Take(ticket, layout->range.address, swap.width, swap.height);
  REQUIRE(capture.has_value());
  auto different_swap = swap;
  ModifyFetch(different_swap, [](Fetch& f) { ++f.base_address; });
  const auto different_layout = Input::GetLayout(different_swap);
  REQUIRE(different_layout.has_value());
  const auto wrong_layout = NativeSystemTitleInputTestAccess::Create(
      capture->identity, *different_layout,
      std::vector<uint8_t>(different_layout->range.length, 0x51));
  REQUIRE(wrong_layout);
  REQUIRE_FALSE(provider.Publish(wrong_layout));
  auto foreign_identity = capture->identity;
  ++foreign_identity.backend_epoch;
  const auto foreign = NativeSystemTitleInputTestAccess::Create(
      foreign_identity, capture->layout,
      std::vector<uint8_t>(capture->layout.range.length, 0x62));
  REQUIRE(foreign);
  REQUIRE_FALSE(provider.Publish(foreign));
  const auto legitimate = NativeSystemTitleInputTestAccess::Create(
      capture->identity, capture->layout,
      std::vector<uint8_t>(capture->layout.range.length, 0x73));
  REQUIRE(legitimate);
  REQUIRE(provider.Publish(legitimate));
  REQUIRE(provider.Latest(swap.foreground_generation) == legitimate);

  const auto stale_ticket = provider.Stage(swap);
  const auto stale_capture = provider.Take(stale_ticket, layout->range.address,
                                           swap.width, swap.height);
  REQUIRE(stale_capture.has_value());
  const auto stale = NativeSystemTitleInputTestAccess::Create(
      stale_capture->identity, stale_capture->layout,
      std::vector<uint8_t>(stale_capture->layout.range.length, 0x84));
  auto invalid = swap;
  invalid.width = 0;
  REQUIRE(provider.Stage(invalid) == 0);
  REQUIRE_FALSE(provider.Latest(swap.foreground_generation));
  REQUIRE_FALSE(provider.Publish(stale));
  auto replacement = swap;
  ++replacement.foreground_generation;
  replacement.title_hmodule += 0x1000;
  ++replacement.title_load_generation;
  const auto old_pending = provider.Stage(swap);
  REQUIRE(old_pending != 0);
  const auto new_ticket = provider.Stage(replacement);
  REQUIRE(new_ticket != 0);
  REQUIRE(provider.Stage(swap) == 0);
  REQUIRE_FALSE(provider.Take(old_pending, layout->range.address, swap.width,
                              swap.height));
  const auto new_capture = provider.Take(new_ticket, layout->range.address,
                                         replacement.width, replacement.height);
  REQUIRE(new_capture.has_value());
  const auto new_input = NativeSystemTitleInputTestAccess::Create(
      new_capture->identity, new_capture->layout,
      std::vector<uint8_t>(new_capture->layout.range.length, 0x95));
  REQUIRE(new_input);
  provider.Close();
  REQUIRE_FALSE(provider.enabled());
  REQUIRE_FALSE(provider.Enable(41));
  REQUIRE(provider.Stage(replacement) == 0);
  REQUIRE_FALSE(provider.Publish(new_input));
  REQUIRE_FALSE(provider.Latest(replacement.foreground_generation));
  REQUIRE(
      legitimate->valid());  // A strong old byte lease survives cache closure.
  REQUIRE(legitimate->bytes()[0] == 0x73);
}

TEST_CASE("Native title provider bounds pending tickets without overwriting",
          "[native_title_input][native_title_input_provider]") {
  NativeSystemTitleInputProvider provider;
  REQUIRE(provider.Enable(41));
  const auto swap = MakeSwap(33, 2, 64, false);
  const auto layout = Input::GetLayout(swap);
  REQUIRE(layout.has_value());
  std::array<uint64_t, 64> tickets{};
  for (size_t i = 0; i < tickets.size(); ++i) {
    tickets[i] = provider.Stage(swap);
    REQUIRE(tickets[i] != 0);
    if (i) {
      REQUIRE(tickets[i] > tickets[i - 1]);
    }
  }
  REQUIRE(provider.Stage(swap) == 0);
  const auto first =
      provider.Take(tickets[0], layout->range.address, swap.width, swap.height);
  REQUIRE(first.has_value());
  const auto first_input = NativeSystemTitleInputTestAccess::Create(
      first->identity, first->layout,
      std::vector<uint8_t>(first->layout.range.length, 0xA1));
  REQUIRE(first_input);
  REQUIRE(provider.Publish(first_input));
  const auto resumed = provider.Stage(swap);
  REQUIRE(resumed > tickets.back());
  REQUIRE(provider.Stage(swap) == 0);
  uint64_t previous_serial = first->identity.swap_serial;
  for (size_t i = 1; i <= tickets.size(); ++i) {
    const auto ticket = i < tickets.size() ? tickets[i] : resumed;
    const auto capture =
        provider.Take(ticket, layout->range.address, swap.width, swap.height);
    REQUIRE(capture.has_value());
    REQUIRE(capture->identity.swap_serial > previous_serial);
    previous_serial = capture->identity.swap_serial;
    const auto input = NativeSystemTitleInputTestAccess::Create(
        capture->identity, capture->layout,
        std::vector<uint8_t>(capture->layout.range.length, uint8_t(i)));
    REQUIRE(input);
    REQUIRE(provider.Publish(input));
    REQUIRE_FALSE(
        provider.Take(ticket, layout->range.address, swap.width, swap.height));
  }
  REQUIRE(provider.Latest(swap.foreground_generation)->identity().swap_serial ==
          previous_serial);
}
TEST_CASE("Native title provider distinguishes never-swapped from unavailable",
          "[native_title_input][native_title_input_provider]") {
  using A = NativeSystemTitleInputProvider::Availability;
  NativeSystemTitleInputProvider provider;
  const auto swap = MakeSwap(33, 2, 64, false);
  const auto observe = [&](uint64_t epoch = 41, uint64_t foreground = 17,
                           uint32_t title = 0x3001C000, uint64_t load = 23) {
    return provider.ObserveAvailability(epoch, foreground, title, load);
  };
  CHECK(observe() == A::kUnknown);
  REQUIRE(provider.Enable(41));
  CHECK(observe() == A::kProvenNoInitialSwap);
  CHECK(observe(42) == A::kUnknown);
  CHECK(observe(41, 0) == A::kUnknown);
  CHECK(observe(41, 17, 0) == A::kUnknown);
  CHECK(observe(41, 17, 0x3001C000, 0) == A::kUnknown);
  const auto ticket = provider.Stage(swap);
  REQUIRE(ticket);
  CHECK_FALSE(provider.Latest(17));
  CHECK(observe() == A::kUnknown);  // Real pending Swap, not no initial Swap.
  const auto layout = Input::GetLayout(swap);
  REQUIRE(layout);
  const auto capture = provider.Take(ticket, layout->range.address, 33, 2);
  REQUIRE(capture);
  CHECK(observe() == A::kUnknown);  // Actual copy not yet completed.
  const auto input = NativeSystemTitleInputTestAccess::Create(
      capture->identity, capture->layout,
      std::vector<uint8_t>(capture->layout.range.length, 0x51));
  REQUIRE(input);
  REQUIRE(provider.Publish(input));
  CHECK(observe() == A::kReady);
  CHECK(observe(41, 18) == A::kUnknown);
  CHECK(observe(41, 17, 0x30020000) == A::kUnknown);
  CHECK(observe(41, 17, 0x3001C000, 24) == A::kUnknown);
  auto unsupported = swap;
  ModifyFetch(unsupported,
              [](Fetch& f) { f.dimension = xenos::DataDimension::k3D; });
  REQUIRE(provider.Stage(unsupported) == 0);
  CHECK_FALSE(provider.Latest(17));
  CHECK(observe() == A::kUnknown);  // Cache withdrawal never restores no-Swap.
  provider.Close();
  CHECK(observe() == A::kUnknown);
}

}  // namespace xe::gpu::test
