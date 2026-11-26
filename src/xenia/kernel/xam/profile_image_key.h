/** Xenia : Xbox 360 Emulator Research Project; BSD license, see LICENSE. */
#pragma once
#include <cstdint>
#include <optional>
namespace xe::kernel::xam {
// Actual17559 FB88 reader and101D0 writer image namespace2 key mapping.
// Scalar address selection only; no source/user/title/IO or writer authority.
constexpr std::optional<uint64_t> ProfileGpdImageKey(uint32_t type,uint64_t item) noexcept {
  switch(type) {
    case 0:return item;
    case 1:return uint64_t(0x8000);
    case 6:return uint64_t(0x8003);
    case 0xF:return uint64_t(0x8007);
    default:return {};
  }
}
}  // namespace xe::kernel::xam
