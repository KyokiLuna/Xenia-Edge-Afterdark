/** Xenia : Xbox 360 Emulator Research Project; BSD license, see LICENSE. */
#pragma once
#include <atomic>
#include <memory>
#include <optional>
#include <system_error>
#include <utility>
#include <mutex>
#include <span>
#include <vector>
#include "xenia/base/host_storage.h"
#include "xenia/base/profile_publication_intent.h"
namespace xe::vfs {class HostPathDevice;class File;enum class FileDisposition;enum class FileAction;}
namespace xe::kernel {
class NativeProfileSaveOwner;
class NativeProfileSavePublication;
class NativeProfileSaveManagedPicture;
class NativeProfileSavePictureFile;
class NativeProfileSavePictureStage;
namespace xam {class ProfileStorageLease;}
namespace xam::test {class NativeProfileSavePublicationTestAccess;}
// Immutable output of one SAME owned PNG stage after its real retained-stage
// Flush at the genuine IO carrier (including after the native File closes).
// This is byte/time input for a later map producer, never native allocation,
// FAT birth, descriptor acknowledgement, publication or completion authority.
// A retained checkpoint owns only its detached bytes and original token; it
// does not keep the mutable stage/native publication alive. The immutable
// before token retains its ordinary source dependency, never a new grant or
// permission to ignore explicit source retirement.
class NativeProfilePictureCheckpoint final {
 public:
  std::span<const uint8_t> bytes() const { return bytes_; }
  const auto& before() const { return before_; }
  filesystem::ProfileArtifactKind kind() const { return kind_; }
  const filesystem::RetainedFileMetadata& metadata() const { return metadata_; }
  // Metadata only: no I/O or path recapture. Actual logical stage/source epoch
  // and byte/time revision must match. The caller separately joins physical
  // availability/identity in its final full-member guard and native IO/counts.
  bool CheckCurrent(std::error_code&) const;
 private:
  friend class NativeProfileSavePictureStage;
  NativeProfilePictureCheckpoint(std::weak_ptr<NativeProfileSavePictureStage> stage,
      uint64_t revision,
      std::shared_ptr<const filesystem::HostStorageSnapshotToken> before,
      filesystem::ProfileArtifactKind kind, std::vector<uint8_t> bytes,
      filesystem::RetainedFileMetadata metadata)
      :stage_(std::move(stage)),revision_(revision),before_(std::move(before)),
       kind_(kind),bytes_(std::move(bytes)),metadata_(metadata) {}
  const std::weak_ptr<NativeProfileSavePictureStage> stage_;
  const uint64_t revision_;
  const std::shared_ptr<const filesystem::HostStorageSnapshotToken> before_;
  const filesystem::ProfileArtifactKind kind_;
  const std::vector<uint8_t> bytes_;
  const filesystem::RetainedFileMetadata metadata_;
};
// One real fixed native PNG output under the SAME original SAVE owner. This
// is not the CM thumbnail participant, a native request, or writer admission.
// Only the actual native producer/open binding may expose its File. Original
// profile bytes remain intact until the shared GPD/PNG/key journal commits.
class NativeProfileSavePictureStage final
    : public std::enable_shared_from_this<NativeProfileSavePictureStage> {
 private:
  friend class NativeProfileSaveOwner;
  friend class NativeProfileSavePublication;
  friend class NativeProfileSaveManagedPicture;
  friend class NativeProfileSavePictureFile;
  friend class NativeProfilePictureCheckpoint;
  friend class xam::test::NativeProfileSavePublicationTestAccess;
  static std::shared_ptr<NativeProfileSavePictureStage> Create(
      std::shared_ptr<NativeProfileSavePublication>,filesystem::ProfileArtifactKind,
      std::error_code&);
  static const char* Leaf(filesystem::ProfileArtifactKind);
  static bool MatchesOwnedFile(const vfs::File&,const std::shared_ptr<NativeProfileSavePublication>&,
      filesystem::ProfileArtifactKind);
  std::error_code ValidateRequestedView(const std::shared_ptr<vfs::HostPathDevice>&,
      const std::string&) const;
  uint32_t OpenManagedView(std::shared_ptr<vfs::HostPathDevice>,std::string,
      vfs::FileDisposition,uint32_t access,vfs::File**,vfs::FileAction*);
  filesystem::FileIoResult Read(size_t,std::span<uint8_t>);
  filesystem::FileIoResult Write(size_t,std::span<const uint8_t>);
  std::error_code Resize(size_t);
  std::error_code SetTimes(uint64_t creation,uint64_t access,uint64_t write);
  std::error_code Flush();
  static std::shared_ptr<const NativeProfilePictureCheckpoint>
  CaptureOwnedFileCheckpoint(const vfs::File&,
      const std::shared_ptr<NativeProfileSavePublication>&,
      filesystem::ProfileArtifactKind,std::error_code&);
  std::shared_ptr<const NativeProfilePictureCheckpoint> CaptureWriteCheckpoint(
      std::error_code&);
  bool CheckCheckpoint(const NativeProfilePictureCheckpoint&,std::error_code&);
  std::optional<filesystem::RetainedFileMetadata> Query(std::error_code&);
  // No still-open native File may later enter another write. The actual
  // request/DB/File owner must also prove its original close/rundown; this
  // active-File count alone never claims native request completion.
  std::optional<std::vector<uint8_t>> Seal(std::error_code&);
  void RetireAtActualQuiescence();
  bool retired() const {return retired_.load(std::memory_order_acquire);}
  std::error_code ReadAvailable() const;
  std::error_code WriteAvailable() const;
  NativeProfileSavePictureStage()=default;
  std::mutex mutex_;
  std::weak_ptr<NativeProfileSavePublication> publication_;
  std::shared_ptr<xam::ProfileStorageLease> storage_;
  filesystem::ProfileArtifactKind kind_=filesystem::ProfileArtifactKind::kPublic64;
  std::shared_ptr<const filesystem::HostStorageSnapshotToken> before_;
  std::vector<uint8_t> expected_,sealed_bytes_;
  std::shared_ptr<const filesystem::SnapshotRecoveryOwner> recovery_;
  std::unique_ptr<filesystem::HostStorageSnapshotStage> stage_;
  std::filesystem::path artifact_;
  uint64_t creation_timestamp_=0;
  // Advance BEFORE any admitted nonempty write/resize/nonzero time update:
  // a failed entered operation may have partial/unknown effects. Empty writes
  // and all-zero time requests do not invalidate a genuine unchanged epoch.
  uint64_t output_revision_=1;
  std::optional<uint64_t> flushed_revision_;
  // Genuine last File destruction may hold GCRC; no stage mutex is needed
  // to retire this completed HOST IO view. The genuine native Close own-return
  // remains an independent duty; this scalar count does not prove that return.
  std::atomic<size_t> managed_views_{0};
  // Immutable readers retain their own old bytes/device dependencies. Only
  // actual writable File views conflict with sealing this output epoch.
  std::atomic<size_t> managed_writer_views_{0};
  bool created_=false,sealed_=false;
  // Guest-visible transaction existence differs from the immutable original
  // target token: a newly created stage must support later native OPEN/read
  // for CopyFile without making the original profile file visible early.
  bool logical_exists_=false,produced_=false;
  // Scoped SYSTEM output is schema-validated as native output. The older
  // explicit image-plan path still requires its exact deep-owned bytes.
  bool system_output_=false;
  std::error_code initialization_error_;
  std::atomic<bool> retired_{false};
};
}  // namespace xe::kernel
