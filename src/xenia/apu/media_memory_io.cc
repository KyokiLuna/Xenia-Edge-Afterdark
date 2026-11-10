/**
 * Xenia : Xbox 360 Emulator Research Project
 * Released under the BSD license - see LICENSE in the root for details.
 */
#include "xenia/apu/media_memory_io.h"
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <limits>
extern "C" {
#include "third_party/FFmpeg/libavformat/avio.h"
#include "third_party/FFmpeg/libavutil/error.h"
#include "third_party/FFmpeg/libavutil/mem.h"
}
namespace xe::apu {
MediaMemoryIo::MediaMemoryIo(std::vector<uint8_t> bytes)
    : bytes_(std::move(bytes)) {}
std::unique_ptr<MediaMemoryIo> MediaMemoryIo::Create(std::vector<uint8_t> bytes,
                                                     size_t maximum_bytes) {
  if (bytes.empty() || bytes.size() > maximum_bytes ||
      bytes.size() > size_t(INT64_MAX)) {
    return {};
  }
  auto result =
      std::unique_ptr<MediaMemoryIo>(new MediaMemoryIo(std::move(bytes)));
  auto buffer = static_cast<uint8_t*>(av_malloc(4096));
  if (!buffer) {
    return {};
  }
  result->context_ = avio_alloc_context(
      buffer, 4096, 0, result.get(),
      [](void* owner, uint8_t* data, int size) {
        return static_cast<MediaMemoryIo*>(owner)->Read(data, size);
      },
      nullptr,
      [](void* owner, int64_t offset, int whence) {
        return static_cast<MediaMemoryIo*>(owner)->Seek(offset, whence);
      });
  if (!result->context_) {
    av_free(buffer);
    return {};
  }
  return result;
}
MediaMemoryIo::~MediaMemoryIo() {
  if (context_) {
    // FFmpeg may replace this buffer while probing/seeking.
    av_freep(&context_->buffer);
    avio_context_free(&context_);
  }
}
int MediaMemoryIo::Read(uint8_t* destination, int count) {
  if (count <= 0 || !destination) {
    return AVERROR(EINVAL);
  }
  if (position_ == bytes_.size()) {
    return AVERROR_EOF;
  }
  const size_t copy = std::min(size_t(count), bytes_.size() - position_);
  std::memcpy(destination, bytes_.data() + position_, copy);
  position_ += copy;
  return int(copy);
}
int64_t MediaMemoryIo::Seek(int64_t offset, int whence) {
  whence &= ~AVSEEK_FORCE;
  if (whence == AVSEEK_SIZE) {
    return int64_t(bytes_.size());
  }
  int64_t base;
  switch (whence) {
    case SEEK_SET:
      base = 0;
      break;
    case SEEK_CUR:
      base = int64_t(position_);
      break;
    case SEEK_END:
      base = int64_t(bytes_.size());
      break;
    default:
      return AVERROR(EINVAL);
  }
  // Compare before signed addition, including INT64_MIN.
  if (offset < -base || offset > int64_t(bytes_.size()) - base) {
    return AVERROR(EINVAL);
  }
  position_ = size_t(base + offset);
  return int64_t(position_);
}
}  // namespace xe::apu
