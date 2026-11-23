/** Xenia : Xbox 360 Emulator Research Project; BSD license, see LICENSE. */
#ifndef XENIA_BASE_RETAINED_FILE_METADATA_H_
#define XENIA_BASE_RETAINED_FILE_METADATA_H_
#include <cstdint>
#include <optional>
#include <system_error>
namespace xe::filesystem {
class FileHandle;
// One native open-file observation. Creation time is deliberately absent:
// POSIX ctime is not birth time. The VFS retains its existing creation value.
struct RetainedFileMetadata {
  uint64_t size = 0, allocated_bytes = 0;
  uint64_t access_timestamp = 0, write_timestamp = 0, change_timestamp = 0;
};
// The exact FileHandle remains owned throughout this call. Its existing
// QueryInfo validates backend authority; the native descriptor observation
// must match that open identity. No pathname is reopened or refreshed.
std::optional<RetainedFileMetadata> QueryRetainedFileMetadata(
    const FileHandle& file, std::error_code& error);
}  // namespace xe::filesystem
#endif  // XENIA_BASE_RETAINED_FILE_METADATA_H_
