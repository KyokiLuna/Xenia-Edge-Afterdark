/** Xenia : Xbox 360 Emulator Research Project; BSD license, see LICENSE. */
#ifndef XENIA_BASE_HOST_METADATA_WRITER_H_
#define XENIA_BASE_HOST_METADATA_WRITER_H_
#include <atomic>
#include <filesystem>
#include <memory>
#include <mutex>
#include <span>
#include <system_error>
#include <vector>
#include "xenia/base/snapshot_publisher.h"
namespace xe::filesystem {
struct HostMetadataWriteResult {
  std::error_code error;
  SnapshotPublicationReceipt publication;
  std::vector<std::filesystem::path> created_directories, attempted_directories;
  bool unchanged = false, durable = false;
  explicit operator bool() const { return !error && durable; }
};
// One explicitly owned host metadata root. Ordinary single-file snapshots,
// not guest storage, a transaction journal or a global root registry.
class HostMetadataWriter final {
 public:
  static constexpr uint64_t kMaximumBytes = 4ull << 20;
  static constexpr size_t kMaximumFailures = 16;
  // Always returns an owner, including initialization uncertainty. Keep it
  // alive so exact failed bootstrap/publication evidence is not discarded.
  static std::shared_ptr<HostMetadataWriter> Open(
      const std::filesystem::path&, std::error_code&);
  ~HostMetadataWriter();
  const std::filesystem::path& path() const { return path_; }
  const std::error_code& error() const { return error_; }
  std::shared_ptr<const HostStorageSnapshotToken> Capture(
      const std::filesystem::path&, uint64_t maximum, std::error_code&) const;
  HostMetadataWriteResult Write(const std::filesystem::path&,
      std::span<const uint8_t>, const std::atomic<bool>* canceled = nullptr,
      bool require_absent = false);
  // Expected tokens come from this explicit root; old bytes/identity are
  // rechecked by the common CAS, including a formerly absent slot.
  HostMetadataWriteResult Publish(
      std::shared_ptr<const HostStorageSnapshotToken>, std::span<const uint8_t>,
      const std::atomic<bool>* canceled = nullptr);
  // Bind a model's retained token to the intended actual parent directory;
  // this reopens no target file and never substitutes a newer byte snapshot.
  HostMetadataWriteResult PublishAt(const std::filesystem::path&,
      std::shared_ptr<const HostStorageSnapshotToken>, std::span<const uint8_t>,
      const std::atomic<bool>* canceled = nullptr);
  HostMetadataWriteResult Erase(std::shared_ptr<const HostStorageSnapshotToken>,
      const std::atomic<bool>* canceled = nullptr);
  HostMetadataWriteResult last_result() const;
  std::vector<HostMetadataWriteResult> retained_failures() const;
 private:
  HostMetadataWriter() = default;
  static bool Relative(const std::filesystem::path&);
  std::shared_ptr<HostStorageRoot> Directory(const std::filesystem::path&,
      bool create, HostMetadataWriteResult*, std::error_code&) const;
  bool Owns(const HostStorageSnapshotToken&) const;
  HostMetadataWriteResult PublishLocked(
      std::shared_ptr<const HostStorageSnapshotToken>, std::span<const uint8_t>,
      const std::atomic<bool>*, bool erase = false);
  HostMetadataWriteResult Remember(HostMetadataWriteResult);
  std::filesystem::path path_;
  std::shared_ptr<HostStorageRoot> root_, uncertain_ancestor_;
  std::unique_ptr<SnapshotRecoveryOwner> recovery_;
  std::error_code error_;
  mutable std::mutex mutex_;
  HostMetadataWriteResult last_;
  std::vector<HostMetadataWriteResult> failures_;
};
}  // namespace xe::filesystem
#endif
