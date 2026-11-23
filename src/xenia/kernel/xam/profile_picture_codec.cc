/** Xenia : Xbox 360 Emulator Research Project; BSD license, see LICENSE. */
#include "xenia/kernel/xam/profile_picture_codec.h"
#include <algorithm>
#include <array>
#include <new>
#include "xenia/kernel/xam/tile_service.h"
// Private writer, as kernel-only qualification must not depend on a GPU tool's
// global encoder implementation. PNG validation/decoding is shared TileService.
#define STBI_WRITE_NO_STDIO
#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#pragma clang diagnostic ignored "-Wunused-function"
#elif defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4505)
#endif
#include "third_party/stb/stb_image_write.h"
#ifdef __clang__
#pragma clang diagnostic pop
#elif defined(_MSC_VER)
#pragma warning(pop)
#endif
namespace xe::kernel::xam {
namespace {
constexpr size_t kMaximumPicturePng = 1u << 20;
std::error_code Bad() { return std::make_error_code(std::errc::bad_message); }
struct Encoded {
  std::vector<uint8_t> bytes;
  bool failed = false;
};
void Append(void* opaque, void* input, int count) noexcept {
  auto& out = *static_cast<Encoded*>(opaque);
  if (out.failed || count == 0) { return; }
  if (!input || count < 0 || size_t(count) > kMaximumPicturePng - out.bytes.size()) {
    out.failed = true; return;
  }
  try {
    const auto* data = static_cast<const uint8_t*>(input);
    out.bytes.insert(out.bytes.end(), data, data + size_t(count));
  } catch (...) { out.failed = true; }
}
std::vector<uint8_t> Encode(std::span<const uint8_t> rgba, uint32_t pixels,
                            std::error_code& error) {
  Encoded out;
  if (rgba.size() != size_t(pixels) * pixels * 4 ||
      !stbi_write_png_to_func(Append, &out, int(pixels), int(pixels), 4,
                              rgba.data(), int(pixels) * 4) || out.failed) {
    error = std::make_error_code(std::errc::io_error); return {};
  }
  error = ValidateProfilePicturePng(out.bytes, pixels);
  return error ? std::vector<uint8_t>{} : std::move(out.bytes);
}
TileService::TextureResult Decode(std::span<const uint8_t> bytes, uint32_t capacity) {
  return TileService::PrepareTexture(TileService::FromOwnedPng(
      std::vector<uint8_t>(bytes.begin(), bytes.end())), capacity * 4, capacity);
}
}  // namespace
std::error_code ValidateProfilePicturePng(std::span<const uint8_t> bytes, uint32_t pixels) {
  if ((pixels != 32 && pixels != 64) || bytes.empty() || bytes.size() > kMaximumPicturePng) {
    return Bad();
  }
  try {
    const auto decoded = Decode(bytes, pixels);
    return XFAILED(decoded.status) || decoded.width != pixels || decoded.height != pixels
        ? Bad() : std::error_code{};
  } catch (const std::bad_alloc&) {
    return std::make_error_code(std::errc::not_enough_memory);
  } catch (...) { return std::make_error_code(std::errc::io_error); }
}
std::optional<ProfilePicturePair> PrepareHostProfilePicturePair(
    std::span<const uint8_t> bytes, std::error_code& error) {
  error.clear();
  if (bytes.empty() || bytes.size() > kMaximumPicturePng) { error = Bad(); return {}; }
  try {
    const auto decoded = Decode(bytes, 64);
    if (XFAILED(decoded.status) || decoded.width != decoded.height ||
        (decoded.width != 32 && decoded.width != 64)) { error = Bad(); return {}; }
    const uint32_t source = decoded.width, target = source == 64 ? 32 : 64;
    std::vector<uint8_t> resized(size_t(target) * target * 4);
    // Bounded center-nearest resampling is a HOST fallback choice, not a claim
    // about the console encoder. Keep exact source PNG bytes at its real size.
    for (uint32_t y = 0; y != target; ++y) {
      for (uint32_t x = 0; x != target; ++x) {
        const uint32_t sx = ((2*x+1)*source)/(2*target), sy = ((2*y+1)*source)/(2*target);
        std::copy_n(decoded.rgba.data() + (size_t(sy)*source+sx)*4, 4,
                    resized.data() + (size_t(y)*target+x)*4);
      }
    }
    auto other = Encode(resized, target, error); if (error) { return {}; }
    ProfilePicturePair out;
    auto& original = source == 64 ? out.large : out.small;
    auto& derived = source == 64 ? out.small : out.large;
    original.assign(bytes.begin(), bytes.end()); derived = std::move(other);
    return out;
  } catch (const std::bad_alloc&) {
    error = std::make_error_code(std::errc::not_enough_memory); return {};
  } catch (...) {
    error = std::make_error_code(std::errc::io_error); return {};
  }
}
}  // namespace xe::kernel::xam
