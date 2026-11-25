/** Xenia : Xbox 360 Emulator Research Project; BSD license, see LICENSE. */
#ifndef XENIA_VFS_FOLDER_CONTENT_PACKAGE_H_
#define XENIA_VFS_FOLDER_CONTENT_PACKAGE_H_
#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include "xenia/xbox.h"
#include "xenia/base/content_mutation_owner.h"
namespace xe::kernel { class NativeProfileSaveOwner; class NativeProfileSavePublication; }
namespace xe::kernel::xam { class NativeFolderContentTestAccess; }
namespace xe::vfs {
class Device;
class File;
// Native StfsCreate adapter supplied only by an actually opened, CM-issued
// raw folder-package File. This is not constructible from a guest packet,
// path, numeric device ID, catalog description or caller-filled header.
// The exact backing File and this source keep their OWN real lifetimes.
class FolderContentPackage {
 public:
  virtual ~FolderContentPackage() = default;
  struct MountRequest {
    uint64_t data_offset = 0, logical_geometry = 0;
    uint32_t context_bytes = 0;
    std::array<uint8_t, 36> descriptor{};
    uint8_t owner_option = 0, content_nonzero = 0, signature_class = 0;
  };
  // Bounded genuine preparation resource only. The actual kernel retains it
  // through exclusive namespace/output publication and destroys it outside
  // GCRC before native resume. No request/device registry or lasting lease.
  class MountAdmission {
   public:
    virtual ~MountAdmission() = default;
   protected:
    MountAdmission() = default;
  };
  // Physical source/header/member checks run in the caller's actual owned IO
  // scope, outside GCRC. Creates ONLY an unpublished real member Device. The
  // kernel installs the exact backing-XFile lifetime as the Device mount
  // resource, binds the actual parent's existing storage volume and managed
  // registry, initializes, validates that registered backing parent and
  // exclusively publishes through normal VFS. Preparation MUST leave the
  // Device storage-volume/mount-resource slots unset for this handoff.
  // No raw sectors, guest context/magic, aliases or native outputs are planted.
  virtual X_STATUS PrepareMemberDevice(std::string_view mount_name,
      const MountRequest&, std::shared_ptr<File> exact_backing,
      std::unique_ptr<Device>& output,
      std::unique_ptr<MountAdmission>& admission) = 0;
  // Cached source/owner closure only. Does not prove a physical observation.
  virtual bool retiring() const noexcept = 0;
  // Actual typed folder backend operation in an owned IO scope. Unsupported
  // raw-sector controls must refuse. Output is owned host storage, copied by
  // the kernel only at its real guest-copy boundary; no retained guest pointer.
  virtual X_STATUS Control(uint32_t code, std::span<uint8_t> output,
                            uint32_t* transferred) = 0;
 private:
  friend class xe::kernel::NativeProfileSaveOwner;
  friend class xe::kernel::NativeProfileSavePublication;
  friend class xe::kernel::xam::NativeFolderContentTestAccess;
  // Observation from THIS actually retained raw File, in the real owned IO
  // scope after independent native context/backing/selected-row validation.
  // The span must flatten the complete native [0,align_up(HeaderSize,0x1000))
  // image: context Header/fixed bytes plus its separate ctx+0x98AC remainder,
  // NOT intervening native control fields. H-only data is insufficient.
  // Returns the source-root CM Header token/spelling; no writer, stage,
  // adoption or native request authority is issued by matching bytes.
  virtual std::shared_ptr<const filesystem::HostStorageSnapshotToken> CaptureHeaderBefore(
      std::span<const uint8_t> actual_native_before_header, std::error_code& error) {
    error = std::make_error_code(std::errc::operation_not_supported); return {};
  }
  // Only the actual native family owner may make this cache transition after
  // authenticating the exact cached backing XFile/context and real IO/DB
  // quiescence. The span contains an owned HOST snapshot, never a deferred
  // translated guest pointer. Stage availability/matching bytes is not that cut.
  virtual X_STATUS AdoptHeaderPublication(
      std::shared_ptr<filesystem::ContentMutationOwner::ProfileHeaderPublication>,
      std::span<const uint8_t> actual_native_before_header) {
    return X_STATUS_NOT_SUPPORTED;
  }
  // Caller owns the genuine SAME joined model/cache checkpoint. A Header CAS
  // receipt alone cannot mint that duty or permit an earlier cache switch.
  virtual X_STATUS PromoteCommittedHeader(
      const std::shared_ptr<filesystem::ContentMutationOwner::ProfileHeaderPublication>&,
      std::shared_ptr<const filesystem::HostStorageSnapshotToken>) {
    return X_STATUS_NOT_SUPPORTED;
  }
 protected:
  FolderContentPackage() = default;
};
}  // namespace xe::vfs
#endif
