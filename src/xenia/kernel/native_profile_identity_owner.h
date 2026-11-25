/** Xenia : Xbox 360 Emulator Research Project; BSD license, see LICENSE. */
#pragma once
#include <array>
#include <cstdint>
#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <system_error>
#include <utility>
#include "xenia/kernel/guest_runtime.h"
namespace xe::cpu { struct GuestCallResult; }
namespace xe::gpu { class NativeFramePresentationGuard; class NativeFrameExecutionRequest; struct NativeFrameExecutionResult; }
namespace xe::kernel {
class KernelState;
struct NativeCompletedFrameRecord;
class XThread;
class NativeImportDomain;
class GuestRuntimeCallExecutor;
class NativeSystemAllocationAuthority;
class NativeSystemTaskContinuation;
class NativeSystemUiService;
class NativeSystemUiStartupCatalog;
class NativeInitialEmptyProfileWitness;
class NativeNotificationPresentationObserver;
class NativeSystemGraphicsTransport;
class NativeIdentityTileWork;
class NativeIdentityTilePublication;
class NativeIdentityTileObserverSlot;
class NativeAvatarService;
class NativeProfileIdentityRequest;
class NativeProfileSaveOwner;
class NativeXmsgProvenance;
namespace xam {
class HostRoster;
class ProfileManager;
class NativeOriginalLogonObservation;
class ProfileRosterIntent;
class ProfileRosterNativeBinding;
struct ProfileRosterResult;
}
// Existing SYSTEM service owns exactly one selected native identity family.
// Common ProfileManager owns all roster/account/model state. This class owns
// only original request/caller and actual native execution/consumer genealogy.
// A Task/UID/dequeue/copy/normal modal return is never a publication receipt.
class NativeProfileIdentityOwner final
    : public std::enable_shared_from_this<NativeProfileIdentityOwner> {
 public:
  enum class Disposition { kUnavailableBeforeNative, kRejectedBeforeEntry,
                           kOwned, kActualReturned, kExecutionUncertain };
  struct Result {
    Disposition disposition=Disposition::kUnavailableBeforeNative;
    uint32_t value=0;
    bool may_use_legacy() const {return disposition==Disposition::kUnavailableBeforeNative;}
  };
  static Result TryLogonOnOriginalCaller(KernelState*,uint16_t ordinal,
                                         std::span<const uint64_t>,
      const xam::NativeOriginalLogonObservation* observation=nullptr) noexcept;
  // Authenticated actual PPC entry/own-return hooks. Default unrelated work
  // remains unchanged; no process, TLS, currentTask or native cache is made up.
  static uint64_t GuardEntry(void*,uint64_t) noexcept;
  static void ObserveReturn(void*,uint64_t) noexcept;
  // Diagnostic-only HIR barriers: actual loaded registers, no guest reads or
  // authority. Each site includes its fixed original instruction word.
  struct TaskDiagnosticSite {uint32_t function,address,word;};
  static constexpr std::array<TaskDiagnosticSite,12> kTaskDiagnosticSites{{
    {0x8199E3E0,0x8199E478,0x4E800421},{0x8199E3E0,0x8199E47C,0x90610088},
    {0x8199E148,0x8199E174,0x4E800421},{0x8199E148,0x8199E178,0x7C7C1B78},
    {0x8199E3E0,0x8199F098,0x91610074},{0x8199E148,0x8199E234,0x9161005C},
    {0x816B9A18,0x816B9A68,0x914B0008},{0x816B9A18,0x816B9A70,0x914B000C},
    {0x816B9BB8,0x816B9C9C,0x557E07FA},{0x816B9BB8,0x816B9CAC,0x7D405828},
    {0x816B9890,0x816B98B4,0x2F030002},{0x816B9890,0x816B98C0,0x2F030002}
  }};
  static void ObserveTaskDiagnosticInstruction(void*,uint32_t site) noexcept;
  // Exact common native instruction cuts, not callback-address permissions.
  static constexpr std::array<TaskDiagnosticSite,10> kTaskExecutionSites{{
    {0x816B9BB8,0x816B9C14,0x2B0B0001},
    {0x816B9D68,0x816B9E1C,0x817E0008},
    {0x816B8B08,0x816B8B5C,0x7D6903A6},
    {0x816B8B08,0x816B8B64,0x907F0038},
    {0x816B7DE8,0x816B7E9C,0x7D6903A6},
    {0x816B7DE8,0x816B7EA4,0x817F0008},
    {0x816BA3C0,0x816BA3E4,0x4E800421},
    {0x816BA3C0,0x816BA3E8,0x3D608000},
    {0x816BA3C0,0x816BA420,0x4E800421},
    {0x816BA3C0,0x816BA424,0x7C7E1B78}
  }};
  static void ObserveTaskExecutionInstruction(void*,uint32_t site) noexcept;
  static void ObserveContextReference(void*,uint32_t event) noexcept;
  // Observer-only loaded operands. Original calls/returns retain their native
  // ABI; opaque D3D return words are never interpreted as HRESULT success.
  static constexpr std::array<TaskDiagnosticSite,17> kImageObservationSites{{
    {0x816B0B50,0x816B0B84,0x4807CA0D},
    {0x81798270,0x81798294,0x480A12FD},
    {0x81798270,0x81798298,0x38600000},
    {0x81798740,0x81798770,0x480A14A9},
    {0x81798740,0x81798774,0x38600000},
    {0x81798788,0x817987C0,0x480A19C1},
    {0x81798788,0x817987C4,0x38600000},
    {0x816C8D68,0x816C8DA0,0x4BFFFCA9},
    {0x817DD2E0,0x817DD348,0x482962AD},
    {0x817DD2E0,0x817DD34C,0x7C7B1B78},
    {0x816C8D68,0x816C8DDC,0x2C030000},
    {0x81839590,0x818396A4,0x7CBDF92E},
    {0x81839C18,0x81839C54,0x917F0030},
    {0x8183A180,0x8183A214,0x917F0030},
    {0x816921C8,0x81692368,0x93D90010},
    {0x81839780,0x81839B08,0x957E0004},
    {0x81839C60,0x8183A048,0x957E0004}
  }};
  static void ObserveImageInstruction(void*,uint32_t site) noexcept;

  // Value-only factual observation. The caller supplies its independently
  // authenticated actual native Task; these values confer no request, roster,
  // source/read/writer, backing-pointer or future-execution authority.
  struct TaskExecutionObservation {
    uint64_t kernel=0,domain=0,root_generation=0,birth_generation=0,invocation=0,module_generation=0;
    uint32_t task=0,thread=0,pcr=0,dispatcher_sp=0,body_sp=0;
    uint32_t callback=0,argument=0,module=0,callback_lr=0;
  };
  static std::optional<TaskExecutionObservation> CurrentTaskExecution(
      void* actual_context,uint32_t authenticated_actual_task) noexcept;
  // One value observation from the genuine allocator own-return. Consumed
  // only by the common XMsg return adapter before original PPC resumes; no
  // second Task counter/registry and no future request/read authority.
  struct TaskAllocationReturnObservation {
    struct RootFact {uint64_t kernel=0,domain=0,generation=0;};
    struct FrameFact {uint32_t thread=0,pcr=0,sp=0,lr=0,function=0;};
    RootFact root;FrameFact allocator,producer;
    uint32_t native_status=0,task=0;
    uint64_t birth_generation=0,birth_origin=0;
    bool validated_birth=false;
  };

  // Only an already retained actual ThreadState cell is consulted under the
  // native publisher/listener locks: bounded scalars/atomics, no lookup/IO,
  // host mutex, allocation or shared/native-resource final release.
  static void BeforeNativeListenerUnlock(void*) noexcept;
  static uint64_t PublishPreboundNativeCache(void*) noexcept;
  static void BeforeCompletionPublication(void*) noexcept;
  static void ObserveRenderIterationEnd(void*) noexcept;
  static std::shared_ptr<NativeIdentityTileWork> CaptureTileWork(
      KernelState*,uint32_t type,uint32_t title,uint64_t item,uint32_t user,
      uint32_t output,uint32_t size_address,uint32_t capacity,std::error_code&);
  class CountedCreate final {
   public:
    CountedCreate(CountedCreate&&) noexcept;
    ~CountedCreate();
    bool admitted() const noexcept {return admitted_;}
    void Complete(uint32_t status,uint32_t actual_body) noexcept;
   private:
    friend class NativeProfileIdentityOwner;
    struct Data;
    CountedCreate(std::shared_ptr<Data>,bool);
    std::shared_ptr<Data> data_;
    bool admitted_=false;
  };
  class CountedDelete final {
   public:
    CountedDelete(CountedDelete&&) noexcept;
    ~CountedDelete();
    void DeleteReturned(bool actual_execution) noexcept;
    void BeforeFree() noexcept;
    void FreeReturned(bool actual_execution) noexcept;
   private:
    friend class NativeProfileIdentityOwner;
    struct Data;
    explicit CountedDelete(std::shared_ptr<Data>);
    std::shared_ptr<Data> data_;
  };
  static CountedCreate BeforeCountedCreate(KernelState*,void*,uint32_t type,
      uint32_t attributes,uint32_t bytes,uint32_t output) noexcept;
  static CountedDelete BeforeCountedDelete(KernelState*,void*,uint32_t body,
      uint32_t allocation,uint32_t type,uint32_t deletion,uint32_t freeing) noexcept;
  // Observes only the actual GetNext Object Manager calls. Under GCRC the
  // primitive writes prebound scalar facts; binding runs after its lock scope.
  // Declared before primitive resources, this receipt's destructor releases
  // every owning local before dropping the conservative HLE-frame counter.
  class ListenerObjectUse final {
   public:
    ListenerObjectUse(ListenerObjectUse&&) noexcept;
    ~ListenerObjectUse();
    void ObserveCountedReference(uint32_t actual_body) noexcept;
    void ReferenceReturned(uint32_t actual_status) noexcept;
    void DereferenceReturned() noexcept;
   private:
    friend class NativeProfileIdentityOwner;
    struct Data;
    explicit ListenerObjectUse(std::shared_ptr<Data>,void* frame_state=nullptr);
    std::shared_ptr<Data> data_;
    void* frame_state_=nullptr;
  };
  static ListenerObjectUse BeforeListenerHandleReference(KernelState*,void*,
      uint32_t handle,uint32_t type,uint32_t output) noexcept;
  static ListenerObjectUse BeforeListenerObjectDereference(KernelState*,void*,
      uint32_t actual_body) noexcept;
  // Observes an existing caller-owned native Task reference. No new native
  // reference/allocation/source is minted. Complete requires real entry and
  // own-return; an executed guard refusal is not a native count receipt.
  class TaskCloseUse final {
   public:
    TaskCloseUse(TaskCloseUse&&) noexcept;
    ~TaskCloseUse();
    bool admitted() const noexcept {return admitted_;}
    bool observed() const noexcept {return bool(data_);}
    bool Complete(bool actual_executed,uint32_t actual_value) noexcept;
   private:
    friend class NativeProfileIdentityOwner;
    struct Data;
    TaskCloseUse(std::shared_ptr<Data>,void*,bool);
    std::shared_ptr<Data> data_;
    void* frame_state_=nullptr;
    bool admitted_=false,finished_=false;
  };
  static TaskCloseUse BeforeNativeTaskClose(KernelState*,XThread&,uint32_t tagged_target) noexcept;
  void BeginStop() noexcept;
  // Stop quiescence may retain an unjoined entered operation until actual
  // CPU/GPU-stop tombstoning; it never means native/host identity committed.
  // Optional caller-owned numeric snapshot of the ORIGINAL stop checks.
  // No resource identity, native completion or retention permission is issued.
  struct StopObservation {
    uint64_t evaluated=0,truth=0,row_evaluated=0,row_truth=0;
    uint32_t result_available=0,result=0,exception=0,first_failed=0,row_class=0;
    std::array<uint32_t,5> visited{}; // counted,context refs,task refs,retained,active
  };
  bool Drained(StopObservation* observation=nullptr) const noexcept;
 private:
  static uint64_t GuardEntryCore(void*,uint64_t) noexcept;
  friend class NativeAvatarService;
  friend class NativeIdentityTilePublication;
  static void ObserveImageEntry(void*,uint32_t) noexcept;
  static void ObserveImageReturn(void*,uint32_t) noexcept;
  static void ObserveImageAllocationFree(void*) noexcept;
  bool BindImageObserverSlot(const std::shared_ptr<NativeIdentityTileObserverSlot>&) noexcept;
  void ObserveImageTileWrite(const std::shared_ptr<NativeIdentityTileObserverSlot>&,
      const std::array<std::pair<uint32_t,uint32_t>,2>&,uint32_t phase,
      const NativeIdentityTileWork*) noexcept;
  friend class NativeXmsgProvenance;
  static std::optional<TaskAllocationReturnObservation> TakeTaskAllocationReturnAtNativeCut(
      void* actual_context) noexcept;
  // Permission to release exactly our own actual getter reference. This is
  // independent of execution Birth and carries no read/source/publication
  // authority. Only the retained Avatar producer can prepare it, before PPC.
  struct TaskReferenceFrame {uint32_t function=0,pcr=0,sp=0,lr=0;};
  enum class TaskReferenceAcquireKind {kWork,kRetainedUnload};
  class TaskReference final {
   private:
    friend class NativeProfileIdentityOwner;
    struct Data;
    explicit TaskReference(std::shared_ptr<Data> data):data_(std::move(data)){}
    std::shared_ptr<Data> data_;
  };
  class TaskReferenceAcquire final {
   public:
    TaskReferenceAcquire(TaskReferenceAcquire&&) noexcept;
    ~TaskReferenceAcquire();
    bool admitted() const noexcept {return admitted_;}
    std::shared_ptr<TaskReference> reference() const noexcept;
    bool Complete(bool executed,uint64_t actual_tag,uint32_t actual_current) noexcept;
   private:
    friend class NativeProfileIdentityOwner;
    struct Data;
    TaskReferenceAcquire(std::shared_ptr<Data>,void*,bool);
    void Withdraw() noexcept;
    std::shared_ptr<Data> data_;
    void* frame_state_=nullptr;
    bool admitted_=false,finished_=false;
  };
  static TaskReferenceAcquire BeforeNativeTaskReferenceAcquire(KernelState*,XThread&,
      TaskReferenceFrame,uint32_t actual_current,std::shared_ptr<void> retained_producer,
      TaskReferenceAcquireKind) noexcept;
  static TaskCloseUse BeforeNativeTaskReferenceClose(KernelState*,XThread&,uint32_t,
      const std::shared_ptr<TaskReference>&,TaskReferenceFrame actual_call,TaskReferenceFrame callback,
      std::shared_ptr<void> retained_producer) noexcept;
  static TaskCloseUse BeforeNativeTaskCloseImpl(KernelState*,XThread&,uint32_t,
      const std::shared_ptr<TaskReference>&,const TaskReferenceFrame*,const TaskReferenceFrame*,
      std::shared_ptr<void>) noexcept;
  friend class GuestRuntimeCallExecutor;
  friend class NativeSystemTaskContinuation;
  friend class NativeSystemUiService;
  // Optional fresh constructor provenance, never a native Logon completion.
  void ObserveInitialEmptyProfileBaseline(XThread&, NativeInitialEmptyProfileWitness&&) noexcept;
  friend class NativeSystemGraphicsTransport;
  friend class xam::ProfileManager;
  void RetainPreparationCleanup(const std::shared_ptr<xam::ProfileRosterIntent>&) noexcept;
  xam::ProfileRosterResult SubmitPreparedUntil(
      std::shared_ptr<xam::ProfileRosterIntent>,std::chrono::steady_clock::time_point) noexcept;
  struct Config {
    std::shared_ptr<const NativeSystemUiStartupCatalog> catalog;
    std::shared_ptr<NativeImportDomain> domain;
    std::shared_ptr<NativeSystemAllocationAuthority> allocation;
    std::shared_ptr<NativeNotificationPresentationObserver> renderer;
    std::shared_ptr<const xam::ProfileRosterNativeBinding> factory_binding;
  };
  static bool ClaimKernelBeforeHeap(KernelState&,const GuestRuntimeCallExecutor&,
      const std::shared_ptr<const xam::ProfileRosterNativeBinding>&);
  static std::shared_ptr<NativeProfileIdentityOwner> CreateBeforeWorkers(Config,std::string*);
  // Always-present execution role. No identity plan, PM claim, read scope,
  // renderer or profile publication is installed by this factory.
  static std::shared_ptr<NativeProfileIdentityOwner> CreateExecutionBeforeWorkers(Config,std::string*);
  bool HasProfileIdentityAuthority() const noexcept;

  static std::shared_ptr<NativeProfileIdentityOwner> Lookup(KernelState*);
  // Advisory snapshot only; exact host Bootstrap phase, never guard IO.
  void ReportFirstRefusalOnBootstrap(XThread&) noexcept;
  // External coordinator snapshot only; permanent scalar cells, even empty.
  void ReportFirstRefusalOnCoordinator() const noexcept;
  void ReportAlignmentOnHost(const char* boundary) const noexcept;
  // Advisory single-writer progress receipts on the existing OS Bootstrap.
  // No guest read, owner lock, call, completion or admission authority.
  enum class HandoffStage : uint32_t {
    kOwnersCaptured=1,kAlignmentReport,kAlignmentReported,kProfileReport,
    kProfileReported,kAvatarReport,kAvatarReported,kStartupUnavailable,
    kStepGate,kStepCall,kStepAdmission,kRefusalReport,kRefusalReported,
    kReap,kReaped,kPreentry,kPaint,kPaintReturned,kContinuation,
    kContinuationReturned,kStepReturnedTrue,kStepReturnedFalse,
    kMediaStep,kMediaReturnedTrue,kMediaReturnedFalse,kProgressReturned
  };
  void ObserveBootstrapHandoff(HandoffStage,bool begin_turn=false) noexcept;
  struct SharedScheduleObservation {
    bool admitted=false;
    std::function<void(const cpu::GuestCallResult&)> returned;
  };
  static SharedScheduleObservation ObserveSharedContinuationSchedule(
      NativeSystemTaskContinuation&,XThread&,std::span<const uint32_t>) noexcept;
  bool BindContinuation(std::shared_ptr<NativeSystemTaskContinuation>);
  bool OwnsScheduledWork(uint64_t serial) const noexcept;
  static bool OwnsIngressCall(GuestRuntimeCallExecutor*, XThread&, uint16_t,
                              std::span<const uint32_t>);
  // Shared facade dispatch after genuine preparation, outside all PM/source
  // locks. Never Login-then-refresh or replay/fallback after native entry.
  xam::ProfileRosterResult SubmitPrepared(
      std::shared_ptr<xam::ProfileRosterIntent>) noexcept;
  bool EnqueuePrepared(std::shared_ptr<xam::ProfileRosterIntent>,
      const NativeProfileIdentityRequest*,XThread*,uint32_t input,uint32_t flags,
      uint32_t overlap,bool bootstrap_alignment,std::error_code&,
      bool* retryable_bootstrap = nullptr,
      const xam::NativeOriginalLogonObservation* observation=nullptr,
      std::chrono::steady_clock::time_point preparation_deadline={});
  GuestRuntime::ExecutionResult ExecutePreparedOn(XThread&);
  GuestRuntime::ExecutionResult RunScheduledBody(XThread&,uint64_t serial,
      const std::function<GuestRuntime::ExecutionResult(XThread&)>&);
  bool CancelQueued(const std::shared_ptr<xam::ProfileRosterIntent>&);
  struct FrameQueryAdmission {
    bool managed=false,admitted=false;
    std::shared_ptr<const gpu::NativeFramePresentationGuard> guard;
  };
  static FrameQueryAdmission AdmitNativeFrameIteration(
      XThread&,uint32_t return_address,bool source_retired) noexcept;
  static FrameQueryAdmission CaptureNativeFrameGuard(XThread&) noexcept;
  static void ObserveNativeImageFrameCaptured(XThread&,const gpu::NativeFrameExecutionRequest&,
      const NativeCompletedFrameRecord&) noexcept;
  static void ObserveNativeImageFrameCompleted(const gpu::NativeFrameExecutionRequest&,
      const gpu::NativeFrameExecutionResult&) noexcept;
  // Same existing normal-return retirement on the actual OS Bootstrap,
  // outside all native/owner locks. No admission or completion authority.
  bool ReapReturnedOnBootstrap(XThread&,bool include_expected = true) noexcept;
  bool StepOnBootstrap(XThread&) noexcept;
  // Runs the genuine retained periodic service stop before host/native drain.
  bool StopPeriodicOnBootstrap(XThread&) noexcept;
  bool StopOnBootstrap(XThread&) noexcept;
  // Exact strong-owner coverage only; never normal Drained/native Close proof.
  bool RetainedForActualKernelStop(StopObservation* observation=nullptr) const noexcept;
  bool RetireAfterActualKernelStop() noexcept;
  static GuestRuntime::ExecutionResult ExecuteBootstrapInputAuxiliary(
      GuestRuntimeCallExecutor&,XThread&,const GuestRuntime::Function&,
      std::span<const uint32_t>);
  uint64_t EnterFiniteInputBoundary(void*,uint32_t) noexcept;
  uint64_t EnterFiniteSystemThreadBoundary(void*,uint32_t) noexcept;
  static uint64_t EnterPreboundNativePublisher(void*) noexcept;
  static void ReturnPreboundNativePublisher(void*) noexcept;
  struct State;
  friend class NativeProfileSaveOwner;
  // Candidate references only. The independently authenticated SAVE issuer
  // must use PM's ACTUAL selected envelope and revalidate request/getter/job/
  // model/source currentness; neither this snapshot nor Task facts grant IO.
  struct SaveSelectionCandidates {
    std::shared_ptr<const xam::HostRoster> current;
    std::shared_ptr<xam::ProfileRosterIntent> active_intent;
    uint64_t job_source=0;
  };
  std::optional<SaveSelectionCandidates> CaptureSaveSelectionCandidates() const noexcept;
  static bool ObserveExecutionEntry(void*,uint32_t) noexcept;
  static bool ObserveExecutionReturn(void*,uint32_t) noexcept;
  explicit NativeProfileIdentityOwner(std::shared_ptr<State>);
  std::shared_ptr<State> state_;
};
} // namespace xe::kernel
