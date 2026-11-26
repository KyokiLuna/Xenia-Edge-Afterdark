/** Xenia : Xbox 360 Emulator Research Project; BSD license, see LICENSE. */
#pragma once
#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <vector>
#include "xenia/kernel/xam/profile_gpd_transaction.h"
#include "xenia/kernel/xam/profile_roster.h"
#include "xenia/kernel/native_profile_save_semantic_uses.h"
#include "xenia/kernel/xfile.h"
#include "xenia/kernel/native_profile_save_managed_gpd.h"
#include "xenia/vfs/folder_content_package.h"
namespace xe::vfs {class Device;class FolderContentPackage;}
namespace xe::kernel::xam {class NativeFolderContent;}
namespace xe::kernel {
class NativeProfileSaveOwner;
class NativeProfileSaveManagedGpd;
class NativeProfileSaveManagedPicture;
class NativeProfileGpdReadView;
class NativeProfileSaveGpdStage;
class NativeProfileGpdCheckpoint;
class NativeProfileSavePictureStage;
namespace xam::test { class NativeProfileSavePublicationTestAccess; }
// The storage/model participant of ONE original native SAVE. It does not
// authenticate native ingress, issue a selected-row token, intercept a guest
// operation or declare a native DB clean. The concrete request registry owns
// it through genuine original request/DB/IO/completion/cancel rundown.
//
// Crucially the SAME model pending slot is native-issued from the start. Host
// Flush/Submit/Attempt cannot publish its HOST serialization, even after the
// weak owner expires. Only this actual owner may adopt the sealed native
// layout and execute the genuine existing journal/model/VFS transaction.
class NativeProfileSavePublication final
    : public std::enable_shared_from_this<NativeProfileSavePublication> {
 private:
  friend class NativeProfileSaveOwner;
  friend class NativeProfileSaveManagedGpd;
  friend class NativeProfileSaveManagedPicture;
  friend class NativeProfileSaveGpdStage;
  friend class NativeProfileSavePictureStage;
  friend class xam::test::NativeProfileSavePublicationTestAccess;
  // Only the real family owner mints this distinct SYSTEM storage issuer
  // after authenticating domain/operation/getter/File+source. RW bits, read
  // captures, scalars and generic Tasks cannot create this private capability.
  enum class SystemGpdMode {kNativeWriteRequired,kNativeUnchangedNoWrite};
  struct SystemFileIssuer {
    // Exact actual native parent control-flow mode, not a cold byte comparison.
    // No-write is issued only when matching C4D8 skips BOTH F898 key writers.
    SystemGpdMode gpd_mode=SystemGpdMode::kNativeWriteRequired;
    std::shared_ptr<xam::ProfileRosterNativeWriter> source;
    std::shared_ptr<NativeProfileGpdReadView> actual_before_file;
    std::shared_ptr<vfs::Device> requested_device;
    std::shared_ptr<const save_detail::SemanticUses::Span> actual_semantic_use;
    uint64_t domain=0,root_generation=0,invocation=0;
    uint32_t title=0;
    std::vector<filesystem::ProfileArtifactKind> required_artifacts;
    // Narrows publication to a later genuine whole-operation checkpoint.
    // This never grants IO/source/writer authority or invents that checkpoint.
    bool defer_until_whole_checkpoint=false;
  };
  struct SystemGpdFileInput {
    std::shared_ptr<NativeProfileGpdReadView> actual_before_file;
    std::shared_ptr<vfs::Device> requested_device;
  };
  struct SystemHeaderIssuer;
  // Actual early parent issuer. No physical File is required before its first
  // original open; the selected writer/semantic span and native output roles
  // have already been authenticated. Physical source/one05 admission is made
  // on that SAME existing first File IO lane, before a PNG stage is created.
  static std::shared_ptr<NativeProfileSavePublication> PrepareSystemParentOperation(
      SystemFileIssuer,std::error_code&);
  std::error_code PrepareParentArtifactsAtOriginalIo();
  std::pair<std::shared_ptr<xam::NativeFolderContent>,std::shared_ptr<filesystem::ContentMutationOwner::ProfileHeaderPublication>>
  CaptureParentHeaderRead(std::error_code&);
  std::error_code BindSystemHeaderInputAtOriginalEdit(SystemHeaderIssuer);
  std::shared_ptr<const vfs::ManagedFileOpenAuthority> IssueParentHeaderReadForActualOpen(
      std::shared_ptr<vfs::Device>,std::string,std::shared_ptr<void>,std::error_code&);
  std::shared_ptr<const filesystem::SnapshotPublicationSet> CaptureParentReadMembers(
      const xam::ProfileGpdReadCapture&,std::error_code&);
  struct SystemHeaderIssuer {
    SystemFileIssuer operation; // GPD input deliberately absent until its real use.
    object_ref<XFile> actual_backing_file;
    std::shared_ptr<vfs::FolderContentPackage> actual_package;
    // Captured on the genuine native971A BEFORE-edit boundary. Neither
    // parseability nor this owned value grants an operation by itself.
    std::vector<uint8_t> actual_native_before_header;
  };
  struct SealedGpd {
    // Owned exact staged file and original native stream table/index prefix.
    // The actual native owner supplies these only after its real DB/IO cut;
    // parseability/this struct itself never grants source or completion rights.
    std::vector<uint8_t> bytes, native_header_index;
  };
  static std::shared_ptr<NativeProfileSavePublication> Prepare(
      std::shared_ptr<xam::ProfileRosterNativeWriter>,
      xam::ProfileGpdMutation, std::error_code&);
  static std::shared_ptr<NativeProfileSavePublication> PrepareSystemFile(SystemFileIssuer,std::error_code&);
  static bool CheckSystemOperation(const SystemFileIssuer&,std::error_code&);
  static bool FinishSystemPreparation(const std::shared_ptr<NativeProfileSavePublication>&,std::error_code&);
  static std::shared_ptr<NativeProfileSavePublication> PrepareSystemHeaderOperation(SystemHeaderIssuer,std::error_code&);
  std::error_code BindSystemGpdInputAtOriginalEdit(
      std::shared_ptr<NativeProfileGpdReadView>,std::shared_ptr<vfs::Device>);
  // Called on the genuine scoped byte-IO lane BEFORE the operation's first
  // stage write. Only the original CM/source-owned Header token may join;
  // keep the returned SAME session for stable raw File adoption, never a
  // second package writer or a post-hoc reacquisition at journal time.
  std::shared_ptr<filesystem::ContentMutationOwner::ProfileHeaderPublication>
  AcquireHeaderAtOriginalIo(std::shared_ptr<const filesystem::HostStorageSnapshotToken>, std::error_code&);
  std::shared_ptr<filesystem::ContentMutationOwner::ProfileHeaderPublication>
  AcquireHeaderWithSerialHeld(std::shared_ptr<const filesystem::HostStorageSnapshotToken>, std::error_code&);
  // Under the SAME actual XFile byte-IO lock, before backend mutation. Captures
  // this raw File's exact CM before token only once, issues the same joined
  // session, and adopts it into the stable backend. This is no commit/flush or
  // native completion and may be called only by the genuine scoped issuer.
  X_STATUS PrepareHeaderAtOriginalWrite(vfs::File&);
  bool RequiresWholeOperationCheckpoint() const noexcept {
    return system_issuer_ && (system_issuer_->defer_until_whole_checkpoint ||
        !system_issuer_->required_artifacts.empty());
  }
  std::error_code ObserveJoinedGpdAtOriginalWrite(uint32_t,std::span<const uint8_t> native_header_index);
  // Real outer native IO/DB/close owner supplies quiescence. Revalidate each
  // last complete write against its actual physical seal; no cached byte
  // snapshot or passed flag alone can turn an intermediate write into commit.
  std::error_code SealJoinedSystemAtOriginalWholeCheckpoint();
  std::error_code SealSystemOutputAtOriginalCheckpoint(std::map<uint32_t,SealedGpd>);
  // Complete packet only: every semantically changed GPD is required; no
  // unrequested title, missing table, changed unknown record or partial pair
  // can publish. Exact native layout survives rather than HOST reserialization.
  std::error_code SealAtOriginalDbCut(std::map<uint32_t, SealedGpd>);
  // Caller must use the genuine outside-host-lock IO carrier and retain this
  // owner. This function does not invent wait permission or run nested PPC.
  xam::ProfileGpdTransactionAttempt PublishAtOriginalCommitCut();
  // HOST recovery only after genuine original request/DB/byte-use rundown.
  // Advances ONLY an actually attempted/uncertain/durable SAME transaction;
  // never starts an unjournaled failure, rewrites native result/notification,
  // or declares native cached File readiness. Unknown cache stays fenced.
  xam::ProfileGpdTransactionAttempt RecoverAttemptedAtActualRundown();
  // Only before a journal preparation was attempted, after actual native
  // views/uses have drained. Failure/expiry/destruction never implies cancel.
  std::error_code CancelAtOriginalQuiescence();
  // Managed File validated same unchanged cache/no-stage tuple; serial held.
  std::error_code CancelPristineAtActualNoopWithSerialHeld();
  // Genuine failed PRE-EDIT File binding plus complete original request/DB
  // rundown. No File was ever adopted and no stage/journal/bytes existed;
  // this releases only the exact unpublished model intent, never a cache.
  std::error_code CancelUnboundAtActualRundown();
  // Called only after the exact stable File actually switched from the sealed
  // stage to its matching committed tuple. Durability alone cannot release a
  // joined native cache milestone or let another HOST layout race promotion.
  std::error_code RequireActualCacheCheckpoint();
  class CacheCheckpoint final {
   public:
    ~CacheCheckpoint();
    void Apply() noexcept;
   private:
    friend class NativeProfileSavePublication;
    explicit CacheCheckpoint(std::unique_ptr<xam::ProfileGpdModelAuthority::NativeCacheCheckpointGuard> guard)
        : guard_(std::move(guard)) {}
    std::unique_ptr<xam::ProfileGpdModelAuthority::NativeCacheCheckpointGuard> guard_;
  };
  // Caller holds this SAME publication and actual File/stage locks. Every
  // try/admission/allocation occurs before its no-fail File readiness cut.
  std::unique_ptr<CacheCheckpoint> PrepareActualCacheCheckpoint(
      uint32_t,const std::shared_ptr<NativeProfileSaveGpdStage>&,std::error_code&);
  // The genuine whole native current-table/IO participant supplies these real
  // stable Files and its owned FINAL native GPD prefix. This private consumer
  // supplies no issuer itself. It prepares BOTH cache swaps and the SAME model
  // release before a no-fail joint Apply; cached DBs/FDs may remain alive.
  X_STATUS PromoteJoinedAtActualNativeCheckpoint(object_ref<XFile> actual_raw,
      object_ref<XFile> actual_gpd,const std::shared_ptr<NativeProfileSaveManagedGpd>&,
      std::span<const uint8_t> actual_native_gpd_index);
  const xam::ProfileGpdWriter& WriterForStage() const { return source_->writer(); }
  const xam::ProfileGpdMutationOrigin& OriginForStage() const { return source_->origin(); }
  bool CurrentForStage(std::error_code& error) const {
    if(!original_byte_use_open_.load(std::memory_order_acquire) ||
        (system_issuer_ && (!system_issuer_->actual_semantic_use || !system_issuer_->actual_semantic_use->unchanged()))) {
      error=std::make_error_code(std::errc::operation_canceled);return false;
    }
    return source_->Current(error);
  }
  // The genuine original allocation/whole-IO cut closes only this epoch's
  // GPD/PNG mutation. Header remains writable for the native descriptor/sign
  // tail. This supplies no IO drain or map/control success by itself.
  void CloseDataByteMutationAtOriginalAllocationCut() noexcept {
    data_byte_use_open_.store(false,std::memory_order_release);
  }
  bool CurrentForDataStage(std::error_code& error) const {
    if(!data_byte_use_open_.load(std::memory_order_acquire)) {
      error=std::make_error_code(std::errc::operation_canceled);return false;
    }
    return CurrentForStage(error);
  }
  // Actual original edit own-return withdraws only FUTURE byte mutation.
  // It does not cancel pending storage, retire cache/DB or fabricate success.
  void CloseOriginalByteMutation() noexcept {
    data_byte_use_open_.store(false,std::memory_order_release);
    original_byte_use_open_.store(false,std::memory_order_seq_cst);
    if(const auto header=header_mutation_session_.load(std::memory_order_seq_cst))header->CloseMutation();
  }

  std::shared_ptr<NativeProfileSaveGpdStage> CreateManagedGpdStage(uint32_t,std::error_code&);
  std::error_code SealManagedGpdAtOriginalDbCut(uint32_t,std::span<const uint8_t> actual_native_header_index);
  std::shared_ptr<NativeProfileSavePictureStage> CreateManagedPictureStage(
      filesystem::ProfileArtifactKind,std::error_code&);
  // Actual native File owners must have genuinely closed before this cut.
  // Each physical PNG is fsynced/decoded/compared to the same allowed plan;
  // successful individual seals stay owned across a later member failure.
  std::error_code SealManagedPicturesAtOriginalFileCut();
  NativeProfileSavePublication() = default;
  std::mutex mutex_;
  std::shared_ptr<xam::ProfileRosterNativeWriter> source_;
  std::optional<SystemFileIssuer> system_issuer_;
  std::optional<SystemHeaderIssuer> header_issuer_;
  // Immutable one-time GPD input publication. A Header-first operation binds
  // it only on its genuine later GetDB/CS/File before-edit use; ordinary read
  // captures cannot acquire an independent writer from this metadata.
  std::atomic<std::shared_ptr<const SystemGpdFileInput>> system_file_input_;
  std::shared_ptr<xam::ProfileGpdPendingMutation> pending_;
  std::shared_ptr<xam::ProfileGpdTransaction> transaction_;
  // Actual early stage admission. A session+initialization error is retained
  // for its exact physical artifact; failure/destruction never commits it.
  std::shared_ptr<filesystem::ProfileArtifactAdmission> joined_artifacts_;
  std::error_code joined_artifact_error_;
  std::shared_ptr<const filesystem::HostStorageSnapshotToken> header_before_at_io_;
  bool header_adopted_=false;
  std::shared_ptr<xam::NativeFolderContent> parent_header_policy_;
  bool parent_operation_=false;
  // Guarded by serial: one Header/main reservation is created before first
  // stage effects. Failure retains the same created/uncertain admission.
  bool parent_admission_attempted_=false;
  // Strong lifetime is held by joined_artifacts_, never reset/replaced during
  // this publication. The observer sees only this lock-free pointer and the
  // CM session's own atomic-only CloseMutation cut, never a mutex/ref drop.
  // Install(ptr)->load(open) and close(open)->load(ptr) are ALL seq_cst: their
  // single total order forbids both loads seeing the old values. Acquire/
  // release on two independent atomics would permit a lost close.
  using HeaderMutationSession=filesystem::ContentMutationOwner::ProfileHeaderPublication;
  static_assert(std::atomic<HeaderMutationSession*>::is_always_lock_free);
  std::atomic<HeaderMutationSession*> header_mutation_session_{nullptr};
  std::map<uint32_t,std::shared_ptr<NativeProfileSaveGpdStage>> stages_;
  std::map<uint32_t,std::shared_ptr<const NativeProfileGpdCheckpoint>> staged_checkpoints_;
  std::map<filesystem::ProfileArtifactKind,std::shared_ptr<NativeProfileSavePictureStage>> picture_stages_;
  std::map<filesystem::ProfileArtifactKind,std::vector<uint8_t>> picture_seals_;
  bool sealed_ = false, canceled_ = false, committed_ = false;
  bool cache_checkpoint_complete_=false;
  std::atomic<bool> original_byte_use_open_{true};
  std::atomic<bool> data_byte_use_open_{true};
};
}  // namespace xe::kernel
