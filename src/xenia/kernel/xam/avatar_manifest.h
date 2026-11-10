/** Xenia : Xbox 360 Emulator Research Project; BSD license, see LICENSE. */
#ifndef XENIA_KERNEL_XAM_AVATAR_MANIFEST_H_
#define XENIA_KERNEL_XAM_AVATAR_MANIFEST_H_
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <system_error>
namespace xe::kernel::xam {
inline constexpr size_t kAvatarManifestBytes = 1000;
enum class AvatarBodyType : uint8_t { kUnknown = 0, kMale = 1, kFemale = 2 };
// Exact 17559 native body-asset GUID comparisons, not a guessed flag/bitfield.
inline AvatarBodyType AvatarManifestBodyType(std::span<const uint8_t> bytes) {
  if (bytes.size()!=kAvatarManifestBytes) { return AvatarBodyType::kUnknown; }
  constexpr std::array<uint8_t,16> male = {
      0,0,0,2,0,0,0,1,0xC1,0xC8,0xF1,9,0xA1,0x9C,0xB2,0xE0};
  constexpr std::array<uint8_t,16> female = {
      0,0,0,2,0,1,0,2,0xC1,0xC8,0xF1,9,0xA1,0x9C,0xB2,0xE0};
  const auto body=bytes.subspan(0x120,16);
  if (std::equal(body.begin(),body.end(),male.begin())) { return AvatarBodyType::kMale; }
  if (std::equal(body.begin(),body.end(),female.begin())) { return AvatarBodyType::kFemale; }
  return AvatarBodyType::kUnknown;
}
// F3 SetManifest copies1000 bytes, sets the actual offline XUID at380, clears
// five bytes at388 and writes only63E80044. All remaining opaque bytes survive.
// This is a codec operation, not validation/readiness/durability authority.
inline std::error_code NormalizeAvatarManifest(std::span<uint8_t> bytes,
                                               uint64_t offline_xuid) {
  if (bytes.size()!=kAvatarManifestBytes || !offline_xuid) {
    return std::make_error_code(std::errc::invalid_argument);
  }
  for (size_t i=0;i<8;++i) { bytes[0x380+i]=uint8_t(offline_xuid>>(56-8*i)); }
  std::fill(bytes.begin()+0x388,bytes.begin()+0x38D,0);
  return {};
}
}  // namespace xe::kernel::xam
#endif
