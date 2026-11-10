/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia contributors. All rights reserved.                    *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */
#ifndef XENIA_KERNEL_XAM_SYSTEM_FONT_H_
#define XENIA_KERNEL_XAM_SYSTEM_FONT_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>

namespace xe::kernel::xam {
// Compatibility bounds for host installation, not claimed retail limits.
inline constexpr size_t kMaximumSystemFontBytes = 32 * 1024 * 1024;
struct SystemFontInfo {
  std::array<uint8_t, 256> signature{};
  std::array<uint8_t, 256> base_signature{};
  uint32_t signed_bytes = 0, glyph_pages = 0;
  bool is_base = false;
};
// Pure host validation of the supported xttf1.0 container. Authenticates signed
// metadata with retail key39/XBOX360XTT and every 4KiB glyph page against its
// signed xchk table. No guest state, font rendering, file IO or input changes.
// Output is assigned only on success. It is descriptive, not file authority.
bool ValidateSystemFont(std::span<const uint8_t>, SystemFontInfo&);
// Bounded raw recovery filename (<SHA256>.xtt), not font authentication.
// No file IO/guest state. Failure returns no name and grants no authority.
std::optional<std::string> SystemFontRecoveryBackupName(
    std::span<const uint8_t>);
// Recognizes only the exact historical generated body, for preserving a repair
// backup. This is never an authenticity or installation-success predicate.
bool IsGeneratedSystemFontSubstitute(std::span<const uint8_t>);
}  // namespace xe::kernel::xam
#endif
