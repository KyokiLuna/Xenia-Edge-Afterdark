/**
 * Xenia : Xbox 360 Emulator Research Project
 * Released under the BSD license - see LICENSE in the root for details.
 */
#ifndef XENIA_APU_MEDIA_MEMORY_IO_H_
#define XENIA_APU_MEDIA_MEMORY_IO_H_
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>
struct AVIOContext;
namespace xe::apu {
// Owned, seekable custom AVIO. Never opens external files/URLs. Caller retains
// this object through avformat_close_input; the format context borrows
// context().
class MediaMemoryIo {
 public:
  static std::unique_ptr<MediaMemoryIo> Create(std::vector<uint8_t> bytes,
                                               size_t maximum_bytes);
  ~MediaMemoryIo();
  AVIOContext* context() const { return context_; }
  size_t size() const { return bytes_.size(); }
  // Also used by AVIO callbacks. Failed seek/read leaves the cursor unchanged.
  int Read(uint8_t* destination, int count);
  int64_t Seek(int64_t offset, int whence);

 private:
  explicit MediaMemoryIo(std::vector<uint8_t> bytes);
  std::vector<uint8_t> bytes_;
  size_t position_ = 0;
  AVIOContext* context_ = nullptr;
};
}  // namespace xe::apu
#endif
