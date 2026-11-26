/** Xenia : Xbox 360 Emulator Research Project; BSD license, see LICENSE. */
#pragma once
#include <atomic>
#include <memory>
#include <string>
#include <span>
#include "xenia/vfs/file_open_context.h"
#include "xenia/vfs/managed_file_service.h"
namespace xe::kernel {
class NativeProfileSaveOwner;
class NativeProfileGpdReadFile;
class NativeProfileSavePublication;
class NativeProfileSaveGpdStage;
class NativeProfileGpdReadView;
class NativeProfileGpdCheckpoint;
class NativeProfileSaveSource;
namespace xam {class ProfileGpdModelAuthority;struct ProfileGpdReadCapture;}
namespace xam::test {class NativeProfileSavePublicationTestAccess;}
// One stable source-bound existing ManagedFileRegistry GPD leaf policy, with
// distinct bounded immutable committed-read and native write-stage authorities.
// No ambient TLS/Task/slot/path/access inference or raw-open fallback. The
// concrete owner authenticates actual request/DB/source uses before issuance;
// an immutable read capability can never become writer permission.
class NativeProfileSaveManagedGpd final : public vfs::ManagedFileService,
    public std::enable_shared_from_this<NativeProfileSaveManagedGpd> {
 public:
  const std::filesystem::path& backing_path() const override {return path_;}
  std::shared_ptr<const filesystem::HostStorageDirectoryIdentity> backing_directory() const override;
  bool retiring() const override;
  X_STATUS Open(std::shared_ptr<vfs::Device>,std::string,vfs::FileDisposition,
      uint32_t,bool,vfs::File**,vfs::FileAction*) override;
  X_STATUS OpenWithContext(std::shared_ptr<vfs::Device>,std::string,vfs::FileDisposition,
      uint32_t,bool,vfs::File**,vfs::FileAction*,const vfs::FileOpenContext&) override;
 private:
  friend class NativeProfileSaveOwner;
  friend class NativeProfileSavePublication;
  friend class xam::test::NativeProfileSavePublicationTestAccess;
  friend class NativeProfileGpdReadFile;
  // Unique HOST metadata for one actually issued open. It carries no Kernel,
  // native object/Task, source, model, writer or request owner by itself.
  class ReadReceipt final {
   private:
    friend class NativeProfileSaveManagedGpd;
    ReadReceipt()=default;
  };
  static std::shared_ptr<const ReadReceipt> IssueReadReceipt();
  static std::shared_ptr<NativeProfileGpdReadView> TryCaptureOpenedRead(
      const vfs::File&,const std::shared_ptr<const ReadReceipt>&,std::error_code&);
  // Metadata-only current immutable File tuple. The family still authenticates
  // the actual selected getter/GetDB/held-CS/native table before using it;
  // finding such a File never grants native object or writer permission.
  static std::shared_ptr<NativeProfileGpdReadView> TryCaptureCurrentFileRead(
      const vfs::File&,const std::shared_ptr<xam::ProfileGpdModelAuthority>&,uint32_t title,std::error_code&);
  class ReadAuthority;
  X_STATUS OpenCommitted(std::shared_ptr<vfs::Device>,std::string,vfs::FileDisposition,
      uint32_t,bool,vfs::File**,vfs::FileAction*,const vfs::FileOpenContext&);
  std::error_code JoinCommittedReadModel(const std::shared_ptr<xam::ProfileGpdModelAuthority>&);
  class OpenAuthority final : public vfs::ManagedFileOpenAuthority {
   private:
    friend class NativeProfileSaveManagedGpd;
    OpenAuthority(std::weak_ptr<NativeProfileSaveManagedGpd> service,
        std::shared_ptr<NativeProfileSavePublication> publication,
        std::shared_ptr<NativeProfileSaveGpdStage> stage,
        std::shared_ptr<vfs::Device> requested,std::string path)
        : service_(std::move(service)),publication_(std::move(publication)),stage_(std::move(stage)),
          requested_(std::move(requested)),path_(std::move(path)) {}
    const std::weak_ptr<NativeProfileSaveManagedGpd> service_;
    // Owns only the actual request's HOST duties across the queued open; the
    // original native envelope/Task/overlap/callback remain native-owned.
    const std::shared_ptr<NativeProfileSavePublication> publication_;
    const std::shared_ptr<NativeProfileSaveGpdStage> stage_;
    const std::shared_ptr<vfs::Device> requested_;
    const std::string path_;
    mutable std::atomic<bool> consumed_{false};
  };
  static std::shared_ptr<NativeProfileSaveManagedGpd> Create(
      std::shared_ptr<NativeProfileSaveGpdStage>,std::error_code&);
  static std::shared_ptr<NativeProfileSaveManagedGpd> CreateSource(
      std::shared_ptr<xam::ProfileGpdModelAuthority>,uint32_t title,std::error_code&,
      std::shared_ptr<const std::atomic<bool>> family_closed={});
  std::shared_ptr<const vfs::ManagedFileOpenAuthority> IssueReadForActualOpen(
      xam::ProfileGpdReadCapture,std::shared_ptr<vfs::Device>,std::string actual_path,std::error_code&,
      std::shared_ptr<void> actual_host_io_duty={},
      std::shared_ptr<const ReadReceipt> actual_open_receipt={},
      std::shared_ptr<const filesystem::SnapshotPublicationSet> issued_members={});
  std::shared_ptr<const vfs::ManagedFileOpenAuthority> IssueParentReadForActualOpen(
      const std::shared_ptr<NativeProfileSavePublication>&,std::shared_ptr<vfs::Device>,
      std::string,std::shared_ptr<void>,std::shared_ptr<const ReadReceipt>,std::error_code&);
  // Observes the actual recognized backend after its real open, never a
  // numeric handle lookup or a fresh source/writer issuer.
  static std::shared_ptr<NativeProfileGpdReadView> CaptureOwnedReadView(const vfs::File&);
  // The actual DB-use owner, not a read capability, authorizes this cut. The
  // existing File/XFile/Entry and handle remain stable. A distinct one-shot
  // native writer authority may change only its exact before view to the
  // pristine corresponding physical stage; no legacy raw backend is swapped.
  // The owned native cache prefix is checked before any authority is consumed.
  X_STATUS UpgradeAtActualDbQuiescence(vfs::File&,
      const std::shared_ptr<NativeProfileGpdReadView>& expected_view,
      const vfs::FileOpenContext&,std::span<const uint8_t> actual_native_header_index);
  // Nonwaiting, metadata-only original PRE-EDIT cut. The actual family issuer
  // has already bound this exact SYSTEM operation/source/File and before index.
  // No stage or filesystem effect occurs here. The SAME existing File/IO22
  // mutation later creates/adopts the exact stage in its actual host IO lane.
  X_STATUS BindSystemFileBeforeActualEdit(vfs::File&,
      const std::shared_ptr<NativeProfileGpdReadView>&,
      const std::shared_ptr<NativeProfileSavePublication>&,
      std::vector<uint8_t> actual_before_header_index);
  // Called only with the actual recognized File mutex owned by its real IO.
  // It never infers permission from requested RW bits or a generic native Task.
  static X_STATUS AdoptBoundSystemStage(NativeProfileGpdReadFile&);
  // Genuine no-op own-return, same unchanged native prefix, no stage/journal
  // and closed original byte-use. Refusal retains the exact binding/pending.
  X_STATUS CancelPristineSystemBindingAtActualNoop(vfs::File&,
      const std::shared_ptr<NativeProfileGpdReadView>&,
      const std::shared_ptr<NativeProfileSavePublication>&,std::span<const uint8_t>);

  static bool MatchesOwnedPublication(const vfs::File&,
      const std::shared_ptr<NativeProfileSavePublication>&);
  // Called only by the independently authenticated next native GetDB/Ensure/
  // held-CS use. Reuses the SAME actual staged File and last completed byte
  // epoch; it never recaptures old committed bytes or grants new mutation.
  static std::shared_ptr<const NativeProfileGpdCheckpoint>
  CaptureStagedFileCheckpointAtActualDbUse(const vfs::File&,
      const std::shared_ptr<NativeProfileSavePublication>&,
      std::span<const uint8_t> actual_native_header_index,std::error_code&);
  // A genuine native no-change branch may have no File write and no stage.
  // Capture only its already-bound immutable before image at the independently
  // authenticated held-CS use. No disk read, stage creation or IO is invented.
  static std::shared_ptr<const NativeProfileGpdCheckpoint>
  CapturePristineBoundFileCheckpointAtActualDbUse(const vfs::File&,
      const std::shared_ptr<NativeProfileSavePublication>&,
      std::span<const uint8_t> actual_native_header_index,std::error_code&);
  // Metadata-only retained-cache lineage, NOT canonical/source currentness or
  // durability. It may describe the same cached before File after its own
  // checkpoint. The whole consumer must separately join actual native return,
  // unchanged-member publication/map authority and any real epoch rotation.
  static bool MatchesPristineBoundBefore(const vfs::File&,
      const std::shared_ptr<NativeProfileSavePublication>&,
      const std::shared_ptr<const NativeProfileGpdCheckpoint>&,
      std::span<const uint8_t> actual_native_header_index,std::error_code&);
  // Caller holds SAME publication serial and actual File mutex, both try-only.
  // This checks owned metadata only and neither creates nor refreshes a grant.
  static bool CheckPristineBoundFileWithSerialAndFileHeld(
      const NativeProfileGpdReadFile&,
      const std::shared_ptr<NativeProfileSavePublication>&,
      std::span<const uint8_t>,std::error_code&);

  class PreparedCachePromotion final {
   public:
    ~PreparedCachePromotion();
   private:
    friend class NativeProfileSaveManagedGpd;
    friend class NativeProfileSavePublication;
    friend class xam::test::NativeProfileSavePublicationTestAccess;
    struct Data;
    explicit PreparedCachePromotion(std::unique_ptr<Data>);
    bool MatchesPublication(const std::shared_ptr<NativeProfileSavePublication>&,
        uint32_t,const std::shared_ptr<NativeProfileSaveGpdStage>&) const noexcept;
    void Apply() noexcept;
    // Unlock consumed File/stage only; retain old holders until destruction
    // after the aggregate model guard has also unwound.
    void ReleaseLocksAfterApply() noexcept;
    std::unique_ptr<Data> data_;
  };
  // Caller owns SAME publication serial and real File/DB/IO lifetime. All
  // fallible source/layout/native-table work precedes Apply. Keep the result
  // through the aggregate model/Header/GPD no-fail cut, then destroy it AFTER
  // all aggregate locks; old read/stage holders must not fall under them.
  // Joint callers use nonwaiting locks. The existing nonjoined wrapper alone
  // retains its original waiting File/stage acquisition semantics.
  std::unique_ptr<PreparedCachePromotion> PrepareCommittedCacheWithSerialHeld(
      vfs::File&,const std::shared_ptr<NativeProfileSavePublication>&,
      std::span<const uint8_t> actual_native_header_index,std::error_code&,
      bool original_nonjoined_wait=false);
  static void ApplyPreparedCache(PreparedCachePromotion&) noexcept;

  // After the actual same transaction committed, a clean native cache already
  // has these exact offsets. Promote its stable File to the corresponding
  // immutable committed tuple; no fake size failure, guest-memory rewrite or
  // new writer is involved. Failure never changes an already committed result.
  X_STATUS PromoteCommittedAtActualDbQuiescence(vfs::File&,
      const std::shared_ptr<NativeProfileSavePublication>&,
      std::span<const uint8_t> actual_native_header_index);
  X_STATUS OpenRead(const std::shared_ptr<const ReadAuthority>&,
      const std::shared_ptr<vfs::Device>&,const std::string&,vfs::FileDisposition,
      uint32_t,vfs::File**,vfs::FileAction*,const vfs::FileOpenContext&);
  // Values come from the genuine original NtCreateFile request/source binding
  // BEFORE IO offload; this method itself is not that authentication proof.
  std::shared_ptr<const vfs::ManagedFileOpenAuthority> IssueForActualOpen(
      const std::shared_ptr<NativeProfileSavePublication>&,
      std::shared_ptr<vfs::Device>,std::string actual_path,std::error_code&);
  NativeProfileSaveManagedGpd(std::shared_ptr<NativeProfileSaveSource>,uint32_t title,std::filesystem::path path);
  const std::shared_ptr<NativeProfileSaveSource> source_;
  const uint32_t title_;
  const std::filesystem::path path_;
};
}  // namespace xe::kernel
