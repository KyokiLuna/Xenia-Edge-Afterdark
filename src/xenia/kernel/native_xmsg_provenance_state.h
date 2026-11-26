/** Xenia : Xbox 360 Emulator Research Project; BSD license, see LICENSE. */
#ifndef XENIA_KERNEL_NATIVE_XMSG_PROVENANCE_STATE_H_
#define XENIA_KERNEL_NATIVE_XMSG_PROVENANCE_STATE_H_
#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>
#include "xenia/cpu/ppc/ppc_context.h"
#include "xenia/kernel/native_xmsg_provenance.h"
namespace xe { class Memory; }
namespace xe::kernel {
// Implementation-private scoped request observations. This contains no native
// Task registry/refcount/source authority. Runtime admission/code ownership is
// required separately; constructing the metadata state cannot publish a service.
struct NativeXmsgProvenance::Capture::Record {
 using Frame=NativeXmsgProvenance::FrameFact;
 std::weak_ptr<State> owner;
 ProducerFields producer;
 std::weak_ptr<NativeProfileIdentityOwner::IdentityServiceOrigin> identity_service;
 bool identity_service_metadata_only=false;
 const void* producer_thread=nullptr;
 uint64_t capture=0,copy_serial=0,version=0;
 Frame wrapper,inner_wrapper,start,copier,schedule,callback,inprocess,shared,controls;
 bool wrapper_entered=false,wrapper_returned=false,inner_entered=false,inner_returned=false;
 bool start_entered=false,start_returned=false,copy_entered=false,copy_returned=false;
 bool inprocess_entered=false,inprocess_returned=false,shared_entered=false,shared_returned=false;
 bool controls_entered=false,controls_returned=false;uint32_t provider_object=0,next_control=0;
 bool schedule_entered=false,schedule_returned=false,callback_entered=false,callback_returned=false;
 std::atomic<bool> facts_available{true};
 // Initialized only by a genuine successful native metadata-only copy. A
 // separate positive CD588 handoff is required before any pump getter admits
 // it. Task free never changes this lane; actual envelope loss closes it and
 // no handoff can set it true again.
 std::atomic<bool> identity_envelope_available{false};
 std::optional<IdentityServiceEnvelope> accepted_identity_service;
 Frame identity_acceptance;
 void CloseAvailability() noexcept {
  facts_available.store(false,std::memory_order_release);
  identity_envelope_available.store(false,std::memory_order_release);
 }
 void Invalidate() noexcept {unavailable=true;CloseAvailability();}
 bool released=false,revoked=false,unavailable=false;
 uint32_t copy_output=0,envelope=0,copied_argument=0,native_overlap=0,parameters=0;
 uint32_t task=0;uint64_t birth_generation=0,birth_origin=0;
 Delivery delivery=Delivery::kDirectInProcess;
 std::shared_ptr<const std::vector<uint8_t>> copied_bytes;
 std::optional<NativeProfileIdentityOwner::TaskExecutionObservation> execution;
};

struct NativeXmsgProvenance::State {
 using Record=Capture::Record;
 using Frame=FrameFact;using Root=RootFact;
 struct Provider {
  Frame frame;const void* actual_thread=nullptr;
  std::weak_ptr<Record> record;
  uint64_t serial=0;
  uint32_t object=0,message=0,argument=0,bytes=0;
  std::shared_ptr<const DispatchFact> fact;
  bool returned=false,unavailable=false;
 };
 explicit State(Config value);
 ~State();
 explicit State(Root value):root(value){}
 Config config;
 KernelState* kernel=nullptr;
 const Root root;
 std::mutex mutex;
 std::mutex retirement_mutex;
 bool stopping=false,retired=false,provider_overflow=false;
 size_t hooks=0;
 uint64_t next_capture=1,next_copy=1,next_provider=1;
 std::vector<std::weak_ptr<Record>> records;
 std::vector<Provider> providers;

 bool CodeCurrent(uint32_t) const;
 // Native providers/producer code must belong to the installed domain. The
 // already-authenticated family's original caller may genuinely be TITLE;
 // its retained caller-code lease belongs to that family, not this metadata.
 std::optional<ModuleFact> ModuleAt(uint32_t,bool require_domain=true) const;
 void Prune();
 bool Live(const Record& r) const noexcept {return r.facts_available.load(std::memory_order_acquire) && !r.released && !r.revoked && !r.unavailable;}
 std::shared_ptr<Record> Unique(const auto& predicate) {
  std::shared_ptr<Record> result;
  for(const auto& weak:records)if(const auto r=weak.lock();r && Live(*r) && predicate(*r)){
   if(result){result->Invalidate();r->Invalidate();return {};}
   result=r;
  }
  return result;
 }

 bool TransferIdentityServiceEnvelope(Record&,const IdentityServiceEnvelope&,Frame,
     uint32_t pending_request,uint32_t pending_overlap) noexcept;
 std::optional<IdentityServiceEnvelope> AcceptedIdentityServiceEnvelope(const Record&) const noexcept;
 Provider* Innermost(const Frame&,const void* actual_thread);
 Provider* CurrentProvider(const Record&,const Frame&,const void* actual_thread);
 bool PublicArguments(const Record&,cpu::ppc::PPCContext&,bool start_form) const;
 bool Execution(const Record&,const NativeProfileIdentityOwner::TaskExecutionObservation&) const;
 void Revoke(uint32_t pointer) noexcept;
 static std::shared_ptr<const std::vector<uint8_t>> ReadCopiedPayload(
     Memory&,uint32_t address,uint32_t bytes);
 bool BindCopiedPayload(Record&,uint64_t expected_version,bool success,bool valid,
     uint32_t envelope,uint32_t copied_argument,
     std::shared_ptr<const std::vector<uint8_t>> payload) noexcept;
};
}  // namespace xe::kernel
#endif
