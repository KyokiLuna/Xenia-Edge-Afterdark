/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Emulator. All rights reserved.                        *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */
#ifndef XENIA_BASE_XDBF_VIEW_H_
#define XENIA_BASE_XDBF_VIEW_H_
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace xe {
// Read-only bounds for the wire tables used by both mutable XDBF loading and
// offline title summaries. Free-entry sizes may contain the EOF sentinel.
struct XdbfTableView {
  std::span<const uint8_t> entries;
  std::span<const uint8_t> free_entries;
  std::span<const uint8_t> data;
  uint32_t entries_used;
  uint32_t free_entries_used;
  static uint32_t U32(const uint8_t* p) {
    return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 |
           p[3];
  }
  static std::optional<XdbfTableView> Parse(std::span<const uint8_t> bytes) {
    if (bytes.size() < 24 || U32(bytes.data()) != 0x58444246) {
      return std::nullopt;
    }
    const uint32_t count = U32(bytes.data() + 8);
    const uint32_t used = U32(bytes.data() + 12);
    const uint32_t free_count = U32(bytes.data() + 16);
    const uint32_t free_used = U32(bytes.data() + 20);
    if (used > count || free_used > free_count ||
        count > (bytes.size() - 24) / 18) {
      return std::nullopt;
    }
    const size_t free_start = 24 + size_t(count) * 18;
    if (free_count > (bytes.size() - free_start) / 8) {
      return std::nullopt;
    }
    const size_t data_start = free_start + size_t(free_count) * 8;
    XdbfTableView view{bytes.subspan(24, size_t(count) * 18),
                       bytes.subspan(free_start, size_t(free_count) * 8),
                       bytes.subspan(data_start), used, free_used};
    for (size_t i = 0; i < used; ++i) {
      const auto* e = view.entries.data() + i * 18;
      const uint32_t offset = U32(e + 10), size = U32(e + 14);
      if (offset > view.data.size() || size > view.data.size() - offset) {
        return std::nullopt;
      }
    }
    return view;
  }
  std::optional<std::span<const uint8_t>> Find(uint16_t section,
                                               uint64_t id) const {
    for (size_t i = 0; i < entries_used; ++i) {
      const auto* e = entries.data() + i * 18;
      const uint16_t found_section = uint16_t(e[0]) << 8 | e[1];
      const uint64_t found_id = uint64_t(U32(e + 2)) << 32 | U32(e + 6);
      if (found_section == section && found_id == id) {
        return data.subspan(U32(e + 10), U32(e + 14));
      }
    }
    return std::nullopt;
  }
};
}  // namespace xe
#endif  // XENIA_BASE_XDBF_VIEW_H_
