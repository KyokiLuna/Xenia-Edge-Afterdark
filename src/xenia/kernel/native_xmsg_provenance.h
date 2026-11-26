/** Xenia : Xbox 360 Emulator Research Project; BSD license, see LICENSE. */
#ifndef XENIA_KERNEL_NATIVE_XMSG_PROVENANCE_H_
#define XENIA_KERNEL_NATIVE_XMSG_PROVENANCE_H_

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "xenia/kernel/native_profile_identity_owner.h"

namespace xe::kernel {
class KernelState;
class NativeContentResolve;
class NativeProfileSaveOwner;
class NativeImportDomain;
class NativeSystemAllocationAuthority;
class NativeSystemUiService;
class NativeSystemUiStartupCatalog;
namespace test { class NativeXmsgProvenanceTestAccess; }

// Scoped observation of the genuine 17559 XMsg producer/copy/dispatch lifetime.
// This service never creates a guest packet/Task, owns native refs, substitutes
// a native outcome, writes completion/status/APC data, or grants family access.
class NativeXmsgProvenance final {
 public:
  struct RootFact {
    uint64_t kernel=0, domain=0, generation=0;
    bool operator==(const RootFact&) const=default;
  };
  struct FrameFact {
    uint32_t thread=0, pcr=0, sp=0, lr=0, function=0;
    bool operator==(const FrameFact&) const=default;
  };
  struct ModuleFact {
    uint32_t hmodule=0;
    uint64_t generation=0;
    bool operator==(const ModuleFact&) const=default;
  };
  // A family-correlated actual public invocation. Its number is NOT a native
  // allocation birth, dispatch invocation, or right to use another family.
  struct PublicInvocationFact {
    uint64_t invocation=0;
    ModuleFact module, caller_module;
    uint32_t function=0, instruction=0;
    FrameFact frame;
    bool operator==(const PublicInvocationFact&) const=default;
  };
  enum class ProducerForm { kRequestWrapper, kDirectStart };
  struct ProducerFields {
    RootFact root;
    PublicInvocationFact public_invocation;
    // Exact original pre-call cut. Request wrappers 816A3128/816A31C8
    // consume app/message/argument/bytes/overlap in r3..r7; the distinct genuine
    // Start wrapper816A2F80 consumes app/message/overlap/argument/bytes. It
    // tail-enters2CF0 and does not fabricate a request-wrapper invocation.
    FrameFact producer;
    uint32_t producer_instruction=0, wrapper=0;
    ProducerForm form=ProducerForm::kRequestWrapper;
    uint32_t app=0, message=0, argument=0, argument_bytes=0, overlap=0;
    // Bounded host observation budget, not a guest ABI validation or limit.
    // If copying cannot be observed, the original native call still executes.
    uint32_t copied_bytes_budget=0;
  };
  class AuthenticatedProducer final {
   private:
    friend class NativeContentResolve;
    friend class NativeProfileIdentityOwner;
    friend class NativeProfileSaveOwner;
    friend class test::NativeXmsgProvenanceTestAccess;
    friend class NativeXmsgProvenance;
    explicit AuthenticatedProducer(ProducerFields fields)
        : fields_(fields) {}
    AuthenticatedProducer(ProducerFields fields,
        std::shared_ptr<NativeProfileIdentityOwner::IdentityServiceOrigin> origin)
        :fields_(fields),identity_service_(std::move(origin)){}
    std::shared_ptr<NativeProfileIdentityOwner::IdentityServiceOrigin> identity_service_;
    ProducerFields fields_;
  };
  class Capture final {
   public:
    Capture()=default;
    Capture(const Capture&)=default;
    Capture& operator=(const Capture&)=default;
    Capture(Capture&&) noexcept=default;
    Capture& operator=(Capture&&) noexcept=default;
    explicit operator bool() const noexcept { return bool(record_); }
   private:
    friend class NativeXmsgProvenance;
    friend class test::NativeXmsgProvenanceTestAccess;
    struct Record;
    explicit Capture(std::shared_ptr<Record> record)
        : record_(std::move(record)) {}
    std::shared_ptr<Record> record_;
  };
  enum class Delivery { kDirectInProcess, kQueuedTask, kInlineTask };
  // Immutable value observation at the real, currently active registered
  // provider callback. Addresses are opaque guest values, never raw pointers.
  // Families still validate their own handler/schema/current copied bytes,
  // source identity, user/caller, cancellation and publication boundaries.
  struct DispatchFact {
    RootFact root;
    PublicInvocationFact public_invocation;
    // Host observation correlations, never native allocation generations.
    uint64_t capture=0, copy=0, provider_invocation=0;
    Delivery delivery=Delivery::kDirectInProcess;
    FrameFact producer, wrapper, start, copier, callback, provider;
    ModuleFact provider_module;
    uint32_t app=0, message=0, original_argument=0, original_argument_bytes=0;
    uint32_t original_overlap=0;
    uint32_t envelope=0, copied_argument=0, copied_argument_bytes=0;
    uint32_t native_overlap=0;
    // Original copy helper's owned payload snapshot, before Start fills the
    // envelope header. Envelope+18 Task output is mutable and not a payload.
    std::shared_ptr<const std::vector<uint8_t>> copied_bytes;
    uint32_t provider_object=0, provider_message=0;
    uint32_t provider_argument=0, provider_argument2=0; // opaque actual callback arg2
    uint32_t provider_callback=0;
    std::optional<NativeProfileIdentityOwner::TaskExecutionObservation> task;
  };
  // Only already-authenticated families can construct admission. A metadata
  // refusal does not stop or change the original native operation. On success
  // the family retains Capture only while it has a real host provenance duty.
  static std::optional<Capture> Begin(void* actual_context,
                                    AuthenticatedProducer) noexcept;
  // Returns only the innermost exact current provider invocation belonging to
  // this capture. Copied routes also recheck the SAME common Task execution
  // birth/invocation; direct synchronous calls require no synthetic Task.
  static std::shared_ptr<const DispatchFact> CurrentDispatch(
      void* actual_context, const Capture&) noexcept;
  // Preissued scalar revocation check for existing nonwaiting VFS admission.
  // True supplies NO source/user/writer authority or native completion fact.
  // It performs no lookup/lock/allocation/refcount/final-owner release.
  static bool FactsAvailable(const Capture&) noexcept;
  struct ObservationState {
    bool released=false, authority_revoked=false, unavailable=false, kernel_stopped=false;
  };
  // Metadata-only host rundown query. Revocation records original free ENTRY
  // or factual stopped-kernel withdrawal, never completed native free, cancel,
  // request, callback or Task. Families must still join actual host IO duties.
  static std::optional<ObservationState> Observe(const Capture&) noexcept;
  // Ends this family's subscription; it is not native cancel/close/completion.
  // Already copied value facts may survive, but confer no current authority.
  static void Release(const Capture&) noexcept;

  // Actual original function entry/own-return and narrowly fixed instruction
  // observations. They never deny entry, change registers or replace PPC.
  // The B9A18 own-return runs AFTER the common Task owner observer and consumes
  // that owner's private once-only exact-cut allocation fact. No reverse link
  // from the independently buildable Task owner to this component is needed.
  static bool HooksFunction(uint32_t) noexcept;
  static void ObserveEntry(void*, uint64_t function) noexcept;
  static void ObserveReturn(void*, uint64_t function) noexcept;
  struct InstructionSite { uint32_t function,address,word; };
  static std::span<const InstructionSite> InstructionSites() noexcept;
  static void ObserveInstruction(void*, uint32_t address) noexcept;

  void BeginStop() noexcept;
  bool Drained() const noexcept;
  // Metadata may remain retained across a factual CPU stop. This only proves
  // that no observer hook currently borrows Kernel/config; it is not native
  // Task, request, callback, provider or IO drainage.
  bool RetainedForActualKernelStop() const noexcept;
 private:
  friend class NativeSystemUiService;
  friend class NativeProfileIdentityOwner;
  friend class test::NativeXmsgProvenanceTestAccess;
  // Optional diagnostic values from ONE existing retained-stop predicate.
  // Scalar-only, caller-owned; no native/Kernel/config lifetime or authority.
  struct RetainedStopObservation {
    bool available=false,stopping=false,retired=false,result=false;
    uint64_t hooks=0;
  };
  bool RetainedForActualKernelStop(RetainedStopObservation*) const noexcept;
  // One source-neutral original TITLE producer, observed before its real Task
  // call. The common Task owner still issues the sole allocation Birth and
  // independently authenticates code, output and all later native lifecycle.
  struct IdentityServiceEnvelope {
    RootFact root;uint64_t capture=0,copy=0;
    uint32_t envelope=0,argument=0,overlap=0,task=0;
    uint64_t birth=0,origin=0;
  };
  static std::optional<IdentityServiceEnvelope> IdentityServiceRequest(
      const std::shared_ptr<NativeXmsgIdentityServiceLink>&) noexcept;
  // The real CD588 successful pending-pair handoff is distinct from the
  // temporary dispatch Task. These private methods never restore generic
  // Live/CurrentDispatch or permit a freed Task to execute again.
  static bool TransferIdentityServiceEnvelope(void*,
      const std::shared_ptr<NativeXmsgIdentityServiceLink>&,
      const std::shared_ptr<NativeProfileIdentityOwner::IdentityServiceOrigin>&,
      const IdentityServiceEnvelope&,FrameFact acceptance,
      uint32_t pending_request,uint32_t pending_overlap) noexcept;
  static std::optional<IdentityServiceEnvelope> AcceptedIdentityServiceEnvelope(
      const std::shared_ptr<NativeXmsgIdentityServiceLink>&) noexcept;
  static std::shared_ptr<const DispatchFact> IdentityServiceDispatch(
      void*,const std::shared_ptr<NativeXmsgIdentityServiceLink>&) noexcept;
  static void ReleaseIdentityService(const std::shared_ptr<NativeXmsgIdentityServiceLink>&) noexcept;
  static std::atomic<bool>* IdentityServiceAvailability(
      const std::shared_ptr<NativeXmsgIdentityServiceLink>&) noexcept;
  static std::atomic<bool>* IdentityServiceDispatchAvailability(
      const std::shared_ptr<NativeXmsgIdentityServiceLink>&) noexcept;
  struct TitleTaskProducerFact {
    RootFact root;
    FrameFact frame;
    uint64_t capture=0, copy=0;
    uint32_t actual_process=0, callback=0, argument=0, parameters=0, output=0;
  };
  static std::optional<TitleTaskProducerFact> CaptureTitleTaskProducer(
      void* actual_context,uint32_t actual_function) noexcept;
  struct Config {
    std::shared_ptr<const NativeSystemUiStartupCatalog> catalog;
    std::shared_ptr<NativeImportDomain> domain;
    std::shared_ptr<NativeSystemAllocationAuthority> allocation;
  };
  static std::shared_ptr<NativeXmsgProvenance> CreateBeforeWorkers(
      Config, std::string* error);
  static std::shared_ptr<NativeXmsgProvenance> Lookup(KernelState*);
  // Metadata-only factual whole-kernel-stopped revocation, never guest cleanup.
  bool RetireAfterActualKernelStop() noexcept;
  struct State;
  explicit NativeXmsgProvenance(std::shared_ptr<State>);
  std::shared_ptr<State> state_;
};
// Strong typed handle for this existing copied-envelope observation only.
// The Record holds a weak origin, so no Record/capsule ownership cycle exists.
class NativeXmsgIdentityServiceLink final {
 private:
  friend class NativeXmsgProvenance;
  NativeXmsgProvenance::Capture capture_;
};
}  // namespace xe::kernel
#endif
