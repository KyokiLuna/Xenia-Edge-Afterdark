/** Xenia : Xbox 360 Emulator Research Project; BSD license, see LICENSE. */
#ifndef XENIA_KERNEL_XAM_PROFILE_PICTURE_CODEC_H_
#define XENIA_KERNEL_XAM_PROFILE_PICTURE_CODEC_H_
#include <cstdint>
#include <optional>
#include <span>
#include <system_error>
#include <vector>
namespace xe::kernel::xam {
struct ProfilePicturePair {
  std::vector<uint8_t> large, small;
};
// Existing bounded TileService PNG/decompression validation remains the
// decoder. Native fixed-file producers must already supply the correct size;
// this performs no transformation or fake native decoder completion.
std::error_code ValidateProfilePicturePng(std::span<const uint8_t>, uint32_t pixels);
// Host fallback input only: a real32/64 square PNG. Preserve the exact original
// chosen bytes and derive the other real size. No native RNG/encoder call is
// claimed, and full-body GPD image bytes are outside this fixed-picture API.
std::optional<ProfilePicturePair> PrepareHostProfilePicturePair(
    std::span<const uint8_t>, std::error_code&);
}  // namespace xe::kernel::xam
#endif
