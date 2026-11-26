/** Xenia : Xbox 360 Emulator Research Project; BSD license, see LICENSE. */
#pragma once
#include <atomic>
#include <memory>
#include <mutex>
#include <span>
#include <utility>
#include <vector>
#include "xenia/base/host_storage.h"
#include "xenia/kernel/xam/profile_gpd_transaction.h"
namespace xe::vfs { class HostPathDevice; class File; }
namespace xe::kernel {
namespace xam::test { class NativeProfileSavePublicationTestAccess; }
class NativeProfileSaveOwner;
class NativeProfileSaveManagedGpd;
class NativeProfileSavePublication;
class NativeProfileSaveManagedGpdFile;
class NativeProfileGpdReadFile;
class NativeProfileGpdReadView;
class NativeProfileSaveGpdStage;
// Immutable observation of one completed stage/table image OR the exact still
// pristine bound-before File. The latter owns no stage/write/flush receipt.
// This is byte-image ownership, never a per-setting permission or native completion.
// A later actual byte mutation invalidates currentness while old owned bytes
// remain readable. Only the real DB/IO owner can interpret its native cut.
class NativeProfileGpdCheckpoint final {
 private:
  friend class NativeProfileSaveGpdStage;
  friend class NativeProfileSaveManagedGpd;
  friend class NativeProfileSavePublication;
  friend class NativeProfileSaveOwner;
  friend class xam::test::NativeProfileSavePublicationTestAccess;
  enum class Kind { kStagedWrite, kPristineBoundBefore };
  NativeProfileGpdCheckpoint(std::weak_ptr<NativeProfileSaveGpdStage> stage,
                             uint64_t revision, std::vector<uint8_t> bytes,
                             std::vector<uint8_t> index)
      : kind_(Kind::kStagedWrite), stage_(std::move(stage)),
        revision_(revision),
        bytes_(std::move(bytes)),
        index_(std::move(index)) {}
  NativeProfileGpdCheckpoint(const vfs::File* file,
      std::weak_ptr<NativeProfileGpdReadView> view,
      std::weak_ptr<NativeProfileSavePublication> publication,
      std::weak_ptr<NativeProfileSaveManagedGpd> service,
      std::vector<uint8_t> bytes, std::vector<uint8_t> index)
      : kind_(Kind::kPristineBoundBefore), revision_(0),
        bytes_(std::move(bytes)), index_(std::move(index)), before_file_(file),
        before_view_(std::move(view)), before_publication_(std::move(publication)),
        before_service_(std::move(service)) {}
  const Kind kind_;
  const std::weak_ptr<NativeProfileSaveGpdStage> stage_;
  const uint64_t revision_;
  const std::vector<uint8_t> bytes_, index_;
  // Compared only against the caller's retained actual File, never dereferenced.
  // The independently locked unique ReadView prevents address-reuse aliasing.
  const vfs::File* const before_file_ = nullptr;
  const std::weak_ptr<NativeProfileGpdReadView> before_view_;
  const std::weak_ptr<NativeProfileSavePublication> before_publication_;
  const std::weak_ptr<NativeProfileSaveManagedGpd> before_service_;
};
// ONE actual bounded physical staged native GPD view. The existing recovery
// owner supplies the protected directory and normal HostStorage IO preserves
// reserve/admission/short-write/fsync behavior. The file is never the target;
// cancellation/failure preserves its explicit artifact instead of truncating
// original content. No path/name/parseable payload creates this capability.
class NativeProfileSaveGpdStage final
    : public std::enable_shared_from_this<NativeProfileSaveGpdStage> {
 private:
  friend class NativeProfileSaveOwner;
  friend class NativeProfileSaveManagedGpd;
  friend class NativeProfileSavePublication;
  friend class NativeProfileSaveManagedGpdFile;
  friend class NativeProfileGpdReadFile;
  friend class xam::test::NativeProfileSavePublicationTestAccess;
  static std::shared_ptr<NativeProfileSaveGpdStage> Create(
      std::shared_ptr<NativeProfileSavePublication>,
      xam::GpdSnapshotLoad, std::error_code&);
  // Called only by the exact issued managed-open context/factory, before
  // any raw disposition effect. Normal VFS paths do not infer this permission.
  std::error_code ValidateRequestedView(const std::shared_ptr<vfs::HostPathDevice>&,
      const std::string& actual_path) const;
  uint32_t OpenManagedView(std::shared_ptr<vfs::HostPathDevice> actual_requested,
      std::string actual_path,uint32_t access,vfs::File**);
  filesystem::FileIoResult Read(size_t,std::span<uint8_t>);
  filesystem::FileIoResult Write(size_t,std::span<const uint8_t>);
  std::error_code Resize(size_t);
  std::error_code Flush();
  std::optional<filesystem::RetainedFileMetadata> Query(std::error_code&);
  // Exact completed byte-write observation. It leaves native write admission
  // OPEN; only the real whole operation may later Seal and publish it.
  std::optional<std::vector<uint8_t>> SnapshotAtActualWriteCheckpoint(std::error_code&);
  std::shared_ptr<const NativeProfileGpdCheckpoint> CaptureWriteCheckpoint(
      std::span<const uint8_t> actual_native_index,std::error_code&);
  std::optional<std::vector<uint8_t>> ReadSnapshotLocked(bool flush,std::error_code&);
  std::optional<std::vector<uint8_t>> Seal(std::error_code&);
  // Called only by the real DB-use owner at proved clean actual quiescence.
  // A model-version change/failed parse/host filename is never this boundary.
  void RetireAtActualQuiescence();
  bool retired() const { return retired_.load(std::memory_order_acquire); }
  std::error_code ReadAvailable() const;
  std::error_code WriteAvailable() const;
  NativeProfileSaveGpdStage() = default;
  static constexpr uint64_t kMaximumBytes = xam::GpdSnapshotStore::kMaximumBytes;
  std::mutex mutex_;
  std::weak_ptr<NativeProfileSavePublication> publication_;
  std::shared_ptr<xam::ProfileStorageLease> storage_;
  xam::GpdSnapshotLoad before_;
  std::shared_ptr<const filesystem::SnapshotRecoveryOwner> recovery_;
  std::unique_ptr<filesystem::HostStorageSnapshotStage> stage_;
  // Real created stage stays identifiable even if a later write/flush fails.
  std::filesystem::path artifact_;
  bool created_ = false, sealed_ = false;
  // Actual completed seal receipt retained for exact header-validation/retry;
  // it never permits later staged writes or a different source generation.
  std::vector<uint8_t> sealed_bytes_;
  // Last XFile release can run under ObjectTable's GCRC. Retirement must not
  // acquire the stage IO mutex (Open takes that mutex before namespace GCRC).
  // A File enters destruction only after its real IO owners have drained.
  std::atomic<size_t> managed_views_{0};
  // Protected by mutex_. A native cached File claims a pristine seeded stage
  // exclusively before its first mutation; ordinary additional stage opens
  // then refuse rather than introducing an untracked second cache writer.
  bool mutation_attempted_ = false, cache_view_claimed_ = false;
  // Only nonempty entered stage byte writes/resizes advance this epoch. It is
  // needed because two original key helpers may use one still-open File and
  // its native cached index while the enclosing transaction remains pending.
  uint64_t data_revision_ = 1;
  uint64_t creation_timestamp_ = 0;
  std::error_code initialization_error_;
  std::atomic<bool> retired_{false};
};
}  // namespace xe::kernel
