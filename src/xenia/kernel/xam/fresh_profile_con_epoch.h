/** Xenia : Xbox 360 Emulator Research Project; BSD license, see LICENSE. */
#ifndef XENIA_KERNEL_XAM_FRESH_PROFILE_CON_EPOCH_H_
#define XENIA_KERNEL_XAM_FRESH_PROFILE_CON_EPOCH_H_
#include <memory>
#include <mutex>
#include <span>
#include <system_error>
#include <vector>
#include "xenia/base/managed_volume_authority.h"
#include "xenia/base/snapshot_publication_set.h"
#include "xenia/kernel/console_signing_identity.h"
#include "xenia/vfs/stfs_metadata.h"
namespace xe::filesystem { class ProfileArtifactAdmission; }
namespace xe::kernel { class NativeProfileSavePublication; }
namespace xe::kernel::xam {
class ContentManager;
class FreshProfileConEpochTestAccess;
// Explicit LEGACY profile transition, not an existing native-map importer or
// the native 86DA0 create/close protocol. Native metadata must be copied from
// its actual complete 93D6 producer before that buffer is freed. This operation
// owns no guest pointer, key, Task, callback, sign-in or STFS control
// completion.
//
// The new canonical map's directory FAT creation is DECLARED NEW at this epoch,
// never recovered from host ctime/birth or an old aggregate. Original member
// files, identities, bytes and host metadata are not changed. The exact old
// aggregate (including license/unknown tail) is retained in the compact map
// and in the publisher's durable changed-metadata recovery copy.
class FreshProfileConEpoch final {
 public:
  ~FreshProfileConEpoch();
  FreshProfileConEpoch(const FreshProfileConEpoch&) = delete;
  filesystem::SnapshotPublicationReceipt Commit(
      const std::atomic<bool>* canceled = nullptr);
  const filesystem::SnapshotPublicationReceipt& receipt() const;
  // Format/layout facts only, not a native Session or mutable allocation grant.
  struct Reopened {
    std::shared_ptr<const vfs::StfsLogicalSnapshot> logical;
    std::vector<vfs::StfsSnapshotMetadata> metadata;
    std::vector<uint8_t> original_legacy_header;
    uint64_t declared_creation = 0;
  };

  // A canonical baseline bound to the SAME already-issued Header/member
  // session. It is not detached Reopened data, a stage overlay, an imported
  // native map, a descriptor ACK, or an intermediate-flush publication.
  // Old immutable logical bytes survive loss of currentness; they never
  // become permission for another Header/session or a successor writer.
  class Live final {
    struct Data;
   public:
    class Guard final {
     public:
      ~Guard() = default;
      Guard(const Guard&) = delete;
      const filesystem::HostStorageSnapshotBatch& batch() const { return *batch_; }
     private:
      friend class FreshProfileConEpoch;
      explicit Guard(std::shared_ptr<const Data> owner) : owner_(std::move(owner)) {}
      // Release the SAME real filesystem guard BEFORE dropping external
      // Header/PIS/session/admission owners. No bare batch can escape custody.
      const std::shared_ptr<const Data> owner_;
      std::unique_ptr<filesystem::HostStorageSnapshotBatch> batch_;
    };
    ~Live();
    Live(const Live&) = delete;
    const Reopened& facts() const;
    const std::shared_ptr<const filesystem::HostStorageSnapshotToken>& header_before() const;
    // Complete physical member and exact external Header join under ONE
    // existing native batch. Genuine IO carrier only, outside GCRC/metadata
    // locks. No path recapture, stage write, publication or native completion.
    std::unique_ptr<Guard> Validate(std::error_code&) const;
   private:
    friend class FreshProfileConEpoch;
    explicit Live(std::shared_ptr<Data>);
    std::shared_ptr<Data> data_;
  };

 private:
  friend class ContentManager;
  friend class ::xe::kernel::NativeProfileSavePublication;
  friend class FreshProfileConEpochTestAccess;
  // Only CM's real selected source/profile ownership may supply this binding.
  // TestAccess uses an actual isolated MVA/member reservation, never raw roots.
  struct Source {
    std::shared_ptr<const filesystem::ManagedVolumeAuthority> volume;
    std::shared_ptr<filesystem::SnapshotPublicationSet> members;
    std::filesystem::path package;
  };
  static std::unique_ptr<FreshProfileConEpoch> Prepare(
      Source, std::span<const uint8_t> actual_complete_native_metadata,
      uint64_t declared_new_creation_filetime, const ConsoleSigningIdentity&,
      std::error_code&, const std::atomic<bool>* canceled = nullptr);
  static std::unique_ptr<Reopened> Open(
      Source, std::span<const uint8_t> trusted_public_certificate,
      std::error_code&);
  // The caller supplies its actual CM/MVA/admission tuple, never a raw
  // Reopened/span. Header CheckWithSnapshotGuard proves exact set/session.
  static std::unique_ptr<Live> BindCurrent(
      Source, std::shared_ptr<filesystem::ProfileArtifactAdmission>,
      std::span<const uint8_t> trusted_public_certificate, std::error_code&);
  static std::unique_ptr<Live::Guard> ValidateLive(
      const Live&, std::error_code&);
  struct Data;
  explicit FreshProfileConEpoch(std::unique_ptr<Data>);
  std::unique_ptr<Data> data_;
};
}  // namespace xe::kernel::xam
#endif
