/** Xenia : Xbox 360 Emulator Research Project; BSD license, see LICENSE. */
#ifndef XENIA_BASE_SNAPSHOT_IO_H_
#define XENIA_BASE_SNAPSHOT_IO_H_
#include <algorithm>
#include <atomic>
#include <span>
#include "xenia/base/filesystem.h"
namespace xe::filesystem {
// Production-used checked transfer loop. Writer is the actual owned stage (or
// an isolated typed fault wrapper around that stage). Invocations occur outside
// Mutation/GCRC; real stage WriteResult owns its own common admission guard.
// Never invent a complete count after partial/error or zero progress.
template <typename Writer>
FileIoResult WriteSnapshotBytes(Writer& writer, std::span<const uint8_t> bytes,
                               const std::atomic<bool>* canceled = nullptr) {
  size_t count = 0;
  while (count != bytes.size()) {
    if (canceled && canceled->load(std::memory_order_acquire)) {
      return {std::make_error_code(std::errc::operation_canceled), count};
    }
    const auto part = bytes.subspan(count,
        std::min<size_t>(bytes.size() - count, 1024 * 1024));
    const auto result = writer.WriteResult(count, part);
    if (result.bytes > part.size()) {
      return {std::make_error_code(std::errc::io_error), count};
    }
    count += result.bytes;
    if (result.error) { return {result.error, count}; }
    if (!result.bytes) {
      return {std::make_error_code(std::errc::io_error), count};
    }
  }
  return {{}, count};
}
// One irreversible namespace publication followed by its durability boundary.
// Production passes native unlink/fsync operations over already validated owned
// dirfds under the common Mutation guard. No guest/provider callback is used.
// The optional isolated error can only make a successful real flush uncertain;
// it cannot skip native work, manufacture success, or mint snapshot tokens.
struct SnapshotRemovalCommit {
  std::error_code error;
  bool published = false, durable = false;
};
template <typename Remove, typename Flush>
SnapshotRemovalCommit CommitSnapshotRemoval(
    Remove&& remove, Flush&& flush, std::error_code post_flush_error = {}) {
  auto error = remove();
  if (error) {
    return {error, false, false};
  }
  error = flush();
  if (!error) {
    error = post_flush_error;
  }
  return {error, true, !error};
}
}  // namespace xe::filesystem
#endif
