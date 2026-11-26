/** Xenia : Xbox 360 Emulator Research Project; BSD license, see LICENSE. */
#pragma once
#include <memory>
#include <atomic>
#include <optional>
#include <string_view>
#include <span>
#include <string>
#include <system_error>
#include <vector>
#include "xenia/kernel/xobject.h"
#include "xenia/vfs/file_open_context.h"
#include "xenia/xbox.h"
namespace xe {class Memory;namespace filesystem {enum class ProfileArtifactKind:uint64_t;}}
namespace xe::vfs {class File;class FileWriteCheckpointAuthority;}
namespace xe::kernel {
namespace test {class NativeProfileSaveRequestTestAccess;}
class KernelState;
class XFile;
class XStfsDevice;
class NativeProfileSaveManagedGpd;
class NativeProfileGpdReadView;
class NativeProfileSavePublication;
class NativeImportDomain;
class NativeSystemUiService;
class NativeSystemUiStartupCatalog;
class NativeSystemAllocationAuthority;
class NativeXmsgProvenance;
namespace xam {class ProfileManager;class ProfileGpdModelAuthority;namespace test {class NativeProfileSavePublicationTestAccess;}}
// One family owner for actual original profile SAVE requests and their bounded
// native DB/source/staging duties. The source-only preparation below fences
// real leaves before native first-open; it issues NO read or writer authority.
// Full native ingress/DB/commit integration is still being authored privately;
// this class is not registered, exported or activated by this source packet.
class NativeProfileSaveOwner final : public std::enable_shared_from_this<NativeProfileSaveOwner> {
 public:
  ~NativeProfileSaveOwner();
  // Actual IoCreateFile caller lane, before the EXISTING IO22 context move.
  // Unrelated calls contribute nothing; admitted calls carry their true host
  // IO duty inside the private authority, with no native/TLS/path-only grant.
  // Actual retained raw File read source, before the existing owned read IO.
  // This only observes a preissued Header scope; it never permits mutation.
  // One authenticated original E60 end-control owns this participant through
  // the EXISTING XStfsDevice synchronous IO carrier. No observer/native pointer
  // crosses the wait and no additional worker, wait, flush or completion is
  // created. An unsupported/failed real backend control never publishes.
  class ControlCheckpoint final {
   public:
    ~ControlCheckpoint();
    X_STATUS AfterOriginalControl(X_STATUS actual_backend_status) noexcept;
    void OriginalCarrierDidNotReturn(X_STATUS actual_kernel_failure) noexcept;
   private:
    friend class NativeProfileSaveOwner;
    struct Data;
    explicit ControlCheckpoint(std::unique_ptr<Data>);
    std::unique_ptr<Data> data_;
  };
  static std::shared_ptr<ControlCheckpoint> CaptureControlCheckpoint(
      KernelState*,const XStfsDevice&,uint32_t) noexcept;
  static void ObserveControlEntry(KernelState*,const XStfsDevice&,uint32_t) noexcept;
  static void ObserveControlReturn(KernelState*,const XStfsDevice&,uint32_t,X_STATUS) noexcept;
  static std::optional<X_STATUS> CaptureReadSource(KernelState*,XFile&) noexcept;
  static std::optional<X_STATUS> CaptureOpenSource(KernelState*,std::string_view,
      vfs::FileOpenContext&) noexcept;
  // Same original NtWriteFile caller lane. Only an authenticated final native
  // stream checkpoint contributes a one-shot owned IO participant; unrelated
  // writes remain ordinary File IO and no native completion is synthesized.
  static std::optional<X_STATUS> CaptureWriteCheckpoint(KernelState*,XFile&,
      uint32_t actual_buffer,uint32_t actual_length,
      std::shared_ptr<const vfs::FileWriteCheckpointAuthority>&) noexcept;
 private:
  friend class NativeSystemUiService;
  friend class test::NativeProfileSaveRequestTestAccess;
  friend class xam::ProfileManager;
  friend class xam::test::NativeProfileSavePublicationTestAccess;
  struct Config {
    std::shared_ptr<const NativeSystemUiStartupCatalog> catalog;
    std::shared_ptr<NativeImportDomain> domain;
    std::shared_ptr<NativeSystemAllocationAuthority> allocation;
  };
  static std::shared_ptr<NativeProfileSaveOwner> CreateBeforeWorkers(Config,std::string*);
  static std::shared_ptr<NativeProfileSaveOwner> Lookup(KernelState*);
  // NativeProfileSaveRequest::Read is a STATUS callback (success==0), never a
  // byte-count callback. The same actual RAM adapter is used at both ingress
  // observations and tested with real Memory backing, not a synthetic reader.
  static uint32_t ReadOwnedPayload(Memory&,uint32_t,std::span<uint8_t>);
  // Owned two-range snapshot of the actual 17559 existing-CON context. Pure
  // ABI/data capture only; the producer below separately proves parent/child,
  // current source, exact raw XFile/device and genuinely held native CS.
  static std::optional<std::vector<uint8_t>> CaptureNativeHeaderBefore(
      Memory&,uint32_t actual_context,std::error_code&);
  // Owned actual native cached GPD prefix at a separately authenticated held-CS
  // use. Byte capture/shape only; never a native/File/stage/writer grant.
  static std::optional<std::vector<uint8_t>> CaptureNativeGpdIndex(
      Memory&,uint32_t actual_stream,uint32_t expected_handle,std::error_code&);
  // Actual common model-load/repair completion calls this outside PM/GCRC/
  // model guards before a prepared native roster row becomes usable. Native
  // first-open may then receive an independently authenticated bounded cap.
  static std::error_code PrepareLoadedModel(KernelState*,
      const std::shared_ptr<xam::ProfileGpdModelAuthority>&);
  std::error_code PrepareSourcePolicies(const std::shared_ptr<xam::ProfileGpdModelAuthority>&);
  void CloseNewAdmission() noexcept;
  bool Drained() const noexcept;
  bool RetainedForActualKernelStop() const noexcept;
  bool RetireAfterActualKernelStop() noexcept;
  // Original fixed cuts only. These family observations consume the shared
  // XMsg capsule; they never duplicate Start/copy/Task/envelope tracking or
  // change a native register, packet, reference, completion or notification.
  // They remain UNWIRED until the actual getter/DB/IO/rundown owner is whole.
  void ObserveOriginalProducer(void* actual_context,uint32_t instruction) noexcept;
  // Nested genuine 2FB/C4D8 -> F548/F470 -> FE20012 -> 837A0 family.
  // These remain unregistered with the rest of this private SAVE unit.
  void ObserveOriginalPictureFileEntry(void* actual_context) noexcept;
  void ObserveOriginalPictureFileReturn(void* actual_context) noexcept;
  void ObserveOriginalPictureCopyEntry(void* actual_context) noexcept;
  void ObserveOriginalPictureCopyReturn(void* actual_context) noexcept;
  // Two actual C4D8 -> F898 ->12580 ->16158 key USES. Native cached DB/File
  // lifetime is retained; these never construct a parallel request/Task.
  void ObserveOriginalParentKeyEntry(void*) noexcept;
  void ObserveOriginalParentKeyReturn(void*) noexcept;
  void ObserveOriginalParentKeySetterEntry(void*) noexcept;
  void ObserveOriginalParentKeySetterReturn(void*) noexcept;
  void ObserveOriginalParentKeySettingEntry(void*) noexcept;
  void ObserveOriginalParentKeySettingReturn(void*) noexcept;
  void ObserveOriginalHeaderProducer(void* actual_context) noexcept;
  void ObserveOriginalHeaderWorkerEntry(void* actual_context) noexcept;
  void ObserveOriginalHeaderBeforeEdit(void* actual_context) noexcept;
  void ObserveOriginalHeaderWorkerReturn(void* actual_context) noexcept;
  void ObserveOriginalHeaderWriteEntry(void* actual_context) noexcept;
  void ObserveOriginalHeaderWriteReturn(void* actual_context) noexcept;
  // Each actual post-key FE20018 is an in-place flush, never final byte close.
  // These share exact native source/dispatch/Header consumers with20003.
  void ObserveOriginalInplaceFlushProducer(void*) noexcept;
  void ObserveOriginalInplaceFlushProducerReturn(void*) noexcept;
  void ObserveOriginalInplaceFlushWorkerEntry(void*) noexcept;
  void ObserveOriginalInplaceFlushWorkerReturn(void*) noexcept;
  void ObserveOriginalInplaceFlushContextEntry(void*) noexcept;
  void ObserveOriginalInplaceFlushContextReturn(void*) noexcept;
  void ObserveOriginalInplaceFlushSkipped(void*) noexcept;
  void ObserveOriginalParentPostFlushEntry(void*) noexcept;
  void ObserveOriginalParentPostFlushReturn(void*) noexcept;
  void ObserveOriginalParentPostFlushGate(void*,uint32_t) noexcept;
  void ObserveOriginalCloseProducer(void* actual_context) noexcept;
  void ObserveOriginalCloseWorkerEntry(void* actual_context) noexcept;
  void ObserveOriginalCloseWorkerReturn(void* actual_context) noexcept;
  void ObserveOriginalContextCloseEntry(void* actual_context) noexcept;
  void ObserveOriginalContextCloseReturn(void* actual_context) noexcept;
  // Actual native close/control observations never change a device result.
  void ObserveOriginalFbEntry(void* actual_context) noexcept;
  void ObserveOriginalFbReturn(void* actual_context) noexcept;
  void ObserveOriginalPublicReturn(void* actual_context,uint32_t actual_function) noexcept;
  void ObserveOriginalSelectionEntry(void* actual_context) noexcept;
  void ObserveOriginalSelectionReturn(void* actual_context) noexcept;
  void ObserveOriginalBodyEntry(void* actual_context,uint32_t actual_function) noexcept;
  void ObserveOriginalBodyReturn(void* actual_context,uint32_t actual_function) noexcept;
  // Deny-only covered semantic mutation spans, observed before genuine getter
  // acquisition. These metadata observations never grant object/source access.
  void ObserveOriginalSemanticEntry(void* actual_context,uint32_t actual_function) noexcept;
  void ObserveOriginalSemanticReturn(void* actual_context,uint32_t actual_function) noexcept;
  void ObserveOriginalGetterEntry(void* actual_context) noexcept;
  void ObserveOriginalGetterReturn(void* actual_context) noexcept;
  void ObserveOriginalProfileReleaseEntry(void* actual_context) noexcept;
  void ObserveOriginalProfileReleaseReturn(void* actual_context) noexcept;
  void ObserveOriginalImageTitleEntry(void* actual_context) noexcept;
  void ObserveOriginalImageTitleReturn(void* actual_context) noexcept;
  void ObserveOriginalImageLeafEntry(void* actual_context) noexcept;
  void ObserveOriginalImageLeafReturn(void* actual_context) noexcept;
  void ObserveOriginalImageDbEntry(void* actual_context) noexcept;
  void ObserveOriginalImageDbReturn(void* actual_context) noexcept;
  void ObserveOriginalImageEnsureEntry(void* actual_context,uint32_t actual_function) noexcept;
  void ObserveOriginalImageEnsureReturn(void* actual_context) noexcept;
  void ObserveOriginalImageEditEntry(void* actual_context,uint32_t actual_function) noexcept;
  void ObserveOriginalImageEditReturn(void* actual_context,uint32_t actual_function) noexcept;
  void ObserveOriginalImageInsertEntry(void* actual_context) noexcept;
  void ObserveOriginalImageInsertReturn(void* actual_context) noexcept;
  void ObserveOriginalStreamWriteEntry(void* actual_context) noexcept;
  void ObserveOriginalStreamWriteReturn(void* actual_context) noexcept;
  void ObserveOriginalImageFileOpenEntry(void* actual_context) noexcept;
  void ObserveOriginalImageStreamEntry(void* actual_context) noexcept;
  void ObserveOriginalImageStreamReturn(void* actual_context) noexcept;
  void ObserveOriginalImageCoreEntry(void* actual_context) noexcept;
  void ObserveOriginalImageTryEntry(void* actual_context) noexcept;
  void ObserveOriginalImageTryReturn(void* actual_context) noexcept;
  void ObserveOriginalImageCoreReturn(void* actual_context) noexcept;
  void ObserveOriginalImageReleaseEntry(void* actual_context) noexcept;
  void ObserveOriginalImageUnlockEntry(void* actual_context) noexcept;
  void ObserveOriginalImageUnlockReturn(void* actual_context) noexcept;
  void ObserveOriginalImageReleaseReturn(void* actual_context) noexcept;
  struct ManagedFileCut {
    enum class Kind {kAdopt,kPromote,kBindSystem,kCancelPristine};
    Kind kind=Kind::kAdopt;
    object_ref<XFile> file;
    std::shared_ptr<NativeProfileSaveManagedGpd> service;
    std::shared_ptr<NativeProfileGpdReadView> expected;
    std::shared_ptr<NativeProfileSavePublication> publication;
    vfs::FileOpenContext open;
    std::vector<uint8_t> actual_native_header_index;
    // Full owner also retains actual DB/getter/execution duties until this
    // owned closure returned/dropped. No translated guest pointer is carried.
  };
  // HOST-only owned IO cut after genuine native request/DB authentication.
  // Empty posted IO is only a prerequisite, never the authentication itself.
  static X_STATUS RunManagedFileCut(const std::shared_ptr<ManagedFileCut>&);
  // Nonwaiting original PRE-EDIT binding only; no RunBlockingIo or physical
  // storage operation is permitted by this metadata-only consumer.
  static X_STATUS PrepareManagedFileCut(const std::shared_ptr<ManagedFileCut>&);
  struct WriteOutcome {
    enum class Phase:uint32_t {kIssued,kEntered,kTransferRefused,kBackendFailed,kPublicationFailed,kStagedAwaitingWholeCheckpoint,kCommittedCachePending,kReady};
    std::atomic<Phase> phase{Phase::kIssued};
    std::atomic<uint32_t> status{X_STATUS_UNSUCCESSFUL},bytes{0},transaction_state{0};
    std::atomic<bool> intent_durable{false},done_durable{false},journal_attempted{false};
  };
  static std::shared_ptr<const vfs::FileWriteCheckpointAuthority> PreparePictureWriteForActualCut(
      XFile&,std::shared_ptr<NativeProfileSavePublication>,filesystem::ProfileArtifactKind,uint32_t,
      std::vector<uint8_t>,std::shared_ptr<WriteOutcome>&,std::error_code&);
  static std::optional<X_STATUS> CaptureParentPictureCopyWrite(KernelState*,XFile&,
      uint32_t,uint32_t,std::shared_ptr<const vfs::FileWriteCheckpointAuthority>&);
  static std::optional<X_STATUS> CaptureParentPictureWrite(KernelState*,XFile&,
      uint32_t,uint32_t,std::shared_ptr<const vfs::FileWriteCheckpointAuthority>&);
  static std::optional<X_STATUS> CaptureParentHeaderWrite(KernelState*,XFile&,uint32_t,uint32_t,
      std::shared_ptr<const vfs::FileWriteCheckpointAuthority>&);
  static std::optional<X_STATUS> CaptureParentHeaderOpen(KernelState*,std::string_view,vfs::FileOpenContext&);
  static std::optional<X_STATUS> CaptureParentPictureCopyOpen(KernelState*,std::string_view,vfs::FileOpenContext&);
  static std::optional<X_STATUS> CaptureParentPictureOpen(KernelState*,std::string_view,
      vfs::FileOpenContext&);
  static std::shared_ptr<const vfs::FileWriteCheckpointAuthority> PrepareWriteCheckpointForActualCut(
      XFile&,std::shared_ptr<NativeProfileSaveManagedGpd>,std::shared_ptr<NativeProfileSavePublication>,
      uint32_t title,uint32_t offset,std::vector<uint8_t> owned_input,std::vector<uint8_t> owned_header,
      std::shared_ptr<WriteOutcome>&,std::error_code&);
  // Actual authenticated native raw Header Write through the SAME XFile/IO22
  // request. The owned input is factual, not a public setter or synthetic
  // completion. Successful byte IO stays staged until the whole SAVE cut.
  static std::shared_ptr<const vfs::FileWriteCheckpointAuthority> PrepareHeaderWriteForActualCut(
      XFile&,std::shared_ptr<NativeProfileSavePublication>,uint32_t offset,
      std::vector<uint8_t> owned_input,std::shared_ptr<WriteOutcome>&,std::error_code&);
  struct State;
  explicit NativeProfileSaveOwner(std::shared_ptr<State>);
  std::shared_ptr<State> state_;
};
}  // namespace xe::kernel
