/** Xenia : Xbox 360 Emulator Research Project; BSD license, see LICENSE. */
#include "xenia/kernel/native_profile_save_owner.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <limits>
#include <mutex>
#include <optional>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>
#include "third_party/mbedtls/include/mbedtls/sha256.h"
#include "xenia/base/mutex.h"
#include "xenia/base/string.h"
#include "xenia/cpu/processor.h"
#include "xenia/cpu/thread_state.h"
#include "xenia/cpu/xex_module.h"
#include "xenia/kernel/guest_runtime_call_executor.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/native_import_domain.h"
#include "xenia/kernel/native_profile_save_managed_gpd.h"
#include "xenia/kernel/native_profile_gpd_read_view.h"
#include "xenia/kernel/native_profile_save_publication.h"
#include "xenia/kernel/xfile.h"
#include "xenia/kernel/xstfs_device.h"
#include "xenia/vfs/folder_content_package.h"
#include "xenia/kernel/native_profile_save_managed_picture.h"
#include "xenia/kernel/native_profile_save_picture_stage.h"
#include "xenia/kernel/native_profile_save_gpd_stage.h"
#include "xenia/kernel/xam/native_folder_content.h"
#include "xenia/kernel/native_profile_save_source.h"
#include "xenia/kernel/util/object_table.h"
#include "xenia/kernel/native_profile_save_request.h"
#include "xenia/kernel/native_profile_save_serialization.h"
#include "xenia/kernel/native_profile_save_semantic_uses.h"
#include "xenia/base/xdbf_view.h"
#include "xenia/kernel/native_xmsg_provenance.h"
#include "xenia/kernel/native_system_allocation_authority.h"
#include "xenia/kernel/native_system_ui_service.h"
#include "xenia/kernel/native_system_ui_startup.h"
#include "xenia/kernel/xam/profile_gpd_model.h"
#include "xenia/kernel/xam/profile_manager.h"
#include "xenia/kernel/xam/xam_state.h"
#include "xenia/kernel/user_module.h"
#include "xenia/kernel/util/guest_io_buffer.h"
#include "xenia/kernel/util/native_wait_snapshot.h"
#include "xenia/kernel/xthread.h"
#include "xenia/memory.h"
#include "xenia/vfs/file_write_checkpoint.h"
#include "xenia/vfs/virtual_file_system.h"
#include "xenia/vfs/device.h"
#include "xenia/vfs/devices/host_path_device.h"
#include "xenia/vfs/host_storage_errors.h"
namespace xe::kernel {
namespace {
std::mutex installed_save_mutex;
std::unordered_map<KernelState*,std::shared_ptr<NativeProfileSaveOwner>> installed_save;
std::error_code Error(std::errc value) {return std::make_error_code(value);}
// Complete original public/receiver bodies, already pinned against the real
// 17559 image. This is family schema authentication, not a Task allowlist.
struct SaveCode {uint32_t at,bytes;const char* sha;};
constexpr SaveCode save_code[]={
 {0x8190EDF0,116,"0cfd8d718162fbdf2d037f34bf5cd50015bda1f92f3dacb54c4b075f0388c3e1"},
 {0x81682FB8,132,"f8366b7a6618166d413fd9be1209d05b9e419ad90d750c986569f604879ae0d2"},
 {0x816EEC38,188,"c8b00a42074265ce0d597ff609e9480391a6afbdcc03359aa3c49759da975ab9"},
 {0x8191F898,104,"90407c8e1097289a2d753050b9cc8b19c1f7048c009bcaf97b8373aea57058f7"},
 {0x81912580,488,"3d32f1ccd9cff4b2f5525012deec882378ae367c3c6530a95df63a891ceea63b"},
 {0x81916158,456,"d3ad9d2e8c2f879d2f6c94242c2799c0bb641728b55b7a8544fb87017e13e2ff"},
 {0x819286B8,136,"47b7d8e6b813da5880090574a3719f69230d4f5a0ef8140ce6cd53f3bc5244bf"},
 {0x81925EF8,80,"40b5737d12be77e2b3596543dcc4dc52d0cda79df1eca220ea9796bb8b2ecae9"},
 {0x8171BA28,916,"9fba5d04e10c492d6deca50a6f819a731dfa6d3f34ce4a9824fdad74f1373121"},
 {0x8171BDC0,24,"d79ce9f1155667ed5bc4d8e7970b8053eac6033fd2929df1e5ca8d131043ccbf"},
 {0x816EEB78,188,"0b6af2294c7e6fb7747029aaa683bf97ec1878f62f1d408726c478ddd230465d"},
 {0x81682F20,148,"d0292d24cfd1792bcee29ebdd7d16c27eb2b0af773f29a3a1f25fe71ad6075b9"},
 {0x81680FC0,488,"2deff63ceaaa4d0842948e0631bebb16e8a37ae2d40caa446cbb282e4a41e66c"},
 {0x81680E60,352,"10b58733eb31242af17937b7440c82cffc9ecca192036a71f724c017f17d552a"},
 {0x81680128,1948,"eb63ec067ccc888f7b4c8c8a884ea05567bf991474aaeb8c6c431e504550ce25"},
 {0x8191B8F8,284,"ea53b868b0f7d1670530966592c1faa3dea89b4ec5f6c68cfc3b45c7b944376f"},
 {0x81687C00,880,"9862d7d286364194cceba3ee13536d127657d3e94eae2becafa431c9ad8b02f2"},
 {0x8191C4D8,2164,"ad0eece885f7367284ef968f6909f21c8460709cf1a1d9ebb071962f702686c0"},
 {0x816EF470,192,"541e893d3e4e36cf77afb7ab20fe2043c47ea22314601ed0c319c4104120a2bf"},
 {0x816EF548,28,"33645d9503968b804682e5014748913290c9a0be69b3ea90df9ff603d4a85f90"},
 {0x816837A0,640,"4d33c5394bd83efa3080ae105b02e459023fe3bd212be2dc108d1af044909c78"},
 {0x8167C628,468,"f99c8b31e86466dd175f6f3afeb370a02047865da9cc023261d15cff3c15f047"},
 {0x81925248,340,"4162e4e3d0cd5f0b19bf769da8dea4ad9db09fda07bec383c331c87b19f385ca"},
 {0x81923900,128,"5dd4913474d1369c3fd10f7f91f82367c4ebb7a83d30b62510b3b3b6b3b5ad28"},
 {0x81722268,292,"a3e5683b60327a5b04439cef5efe682dd30e0b6fea72b89098e258e31640092b"},
 {0x81925110,308,"03375f5c23766e2709c64dcdc544556d5dd352e8c8cfafad662ebecac88e67ce"},
 {0x81925AC0,452,"8076821393f2106af7c27adf0c67aeb7d994a57e9c98f8407863de5bdaf19f42"},
 {0x819110E0,360,"3769adfdbdfd13ede914879af49e9f1ca83d327d92c6e413a5651bc4c0e0022d"},
 {0x819133B0,324,"9ed76b9a585fd1b1f4f770881a753feb0ad920ff989a06c204edc9afa20b1d24"},
 {0x81913D18,428,"06b07d806e464ad1af31994a9e512d60fc8e48e8c8e33efa865744452a4ef5b8"},
 {0x819104F8,292,"ffc00cab253e43fb36b6207adfea45514e6cb35da40ce4d84a38d54aebd0aa5f"},
 {0x81928528,152,"eddad1ee5194db05e5e040c6e92993bb0f573ea9fbfcef70070dfbfcdf56c89d"},
 {0x81928358,92,"058725b6b6fb13d77ba79dd7b4c58d724ca01403f1df484fc401cbe504f1dd27"},
 {0x819283B8,76,"a7a5b32ee7ae1750283cd3e0da104cd8396f0d7d0ca5e31b7719f90562b8dcac"},
 {0x819285D0,80,"15e03c8a7b2bf767ff24510d33eccb832a171af243d398d78696b1c4fb1cace0"},
 {0x81923B68,212,"57394dcd22bfe4e9355855d020e0544e133463d507d83275df39472dc7870e00"},
 {0x81923AD0,148,"72378303ca86326e7633851513db38dda46bc1c7779a3837bb7127ec9cfa89fa"},
 {0x81928750,164,"614b323ff5dcbd4eca470e0f815985a57f2f32476f6e5f5d96027028c310c469"},
 {0x81928408,16,"da07557dc059290375fef04f36d080fd66c7222589084f737097e18290394af4"},
 {0x81924010,280,"61dddfeb31b19bae26dea692711062e7d766dafeb0268449f9bceab5b8b80513"},
 {0x819237F8,132,"f89db538abd3037529fc2896ed1807c347345b69f5e8bd258834e8949c02a74b"},
 {0x8171BF40,484,"de858a19f03a811592397c62f97a9a8cdac49171f53e1a82550e18d505c4fc23"},
 {0x81670740,184,"d6649a87ff3b0cb78f0a258b7b81eb5467191de62a0a66962c9561a4e54ea692"},
 {0x816707F8,160,"970367f262ed69e47d604ccda29841291675604689f0d223f571bbc94d6db9ed"},
 {0x81670E08,180,"b70fbdd4ef3f54e75b41b31c08815c4a6160cb369a4e9d95ef5613aa1e762744"},
 {0x816B1A20,124,"2762910ca548303e573d7f389118ab2395a6056c59be602cd34f1fc8e9636a51"},
 {0x8190AAC0,2660,"6bd25579b635630451e500e3e7daf7c53bdf6458fc3a93cdd35ade5ed93d753e"},
 {0x81906808,112,"af4c23257cb94141b8b648020e732b2b19f52863fc6bac97c7554b2197661b33"},
 {0x818C97F8,252,"3ede835b749be139396bbd94de575598ab964f3e6a7f29a03bc9e4f7f66e01e1"},
 {0x8190B528,8,"478c71a6f49f0945eb272fd611ea787afd9c420a915b67b042dff49605443752"},
 {0x8190B530,16,"877afbc33e992dca47646cd12a4db939c93fc481b426b77011e7ef0877e19756"},
 {0x8191CF80,108,"625aef487c80a1a12b28d7a4f646c90bd7b901b4e798906929cf4d6fbb7c52f6"},
 {0x8191DDE8,656,"400aeb706bab21cdbc5ad7d57d8a5c6f2701c087d0bb9fdf566ca80d5117cab9"},
 {0x8191E360,120,"d36af2a938b119f9de85513d0b420accce6fd9820c1c67874e3320ed5c0a7d04"},
 {0x8190E310,120,"49319d40b5e628e7e84ec106fb0b075a449be914aad03339ea153e30561aaaac"},
 {0x819114B8,116,"97c2ea49856ce0e3ba22e9193635322c78bfa1d3f7d734bd5ba2378b288bccd4"},
 {0x81907D00,76,"c29382c8481dd5c9e4f0895960dd3971c2ea2f1d9e383fb392e7e3701163a27a"},
 {0x819101D0,348,"bdf64d9a6336a2eb5e50fe140be0d3c949599893f9f6e2c2ee16f342e73fa73d"},
 {0x81911D48,300,"4e959654a1d4a28baf1660a99d3efdfbe8537a04753de9d0602c217ce1c134a9"},
 {0x81928418,76,"0bc794407911313db59d529b435eca19aa9ae1e813c5795df861e2f5e24e23e3"},
 {0x819235F0,124,"7bfc9389b8341bafbc364761ac07052a1252df3369392e1a3ce6f3da5d69f776"},
 {0x8190F1C0,152,"ba84ef15b62d06d75d687f5f752c5f2df396ea9cd92a6d045e068987bf7d1697"},
 {0x81928468,12,"88be2f365b2113cedca53eb67e70a5af4193525188cc10c139016275cc7ebaee"},
 {0x81923670,8,"420a3989a01aa174590449d2f322907a20801a1c110bfa1a343d8c9bf2f92e7a"},
};
struct BodySite {uint32_t message,function,caller_lr,frame_bytes,getter_lr,release_lr;};
constexpr BodySite body_sites[]={
 {0xB0031,0x8191CF80,0x8190B06C,0x70,0x8191CFA4,0x8191CFE0},
 {0xB0037,0x8191DDE8,0x8190B08C,0x200,0x8191DE10,0x8191E06C},
 {0xB0033,0x8191DDE8,0x8191E3C0,0x200,0x8191DE10,0x8191E06C},
};
const BodySite* BodyFor(uint32_t message) {
 const auto found=std::find_if(std::begin(body_sites),std::end(body_sites),[&](const auto& b){return b.message==message;});
 return found==std::end(body_sites)?nullptr:found;
}
struct ProducerSite {uint32_t function,instruction,word,frame_bytes,message,descriptor_bytes,wrapper;};
constexpr ProducerSite producer_sites[]={
 {0x81670740,0x816707A4,0x48032985,0x90,0xB0037,0x2C,0x816A3128},
 {0x816707F8,0x81670844,0x480328E5,0x90,0xB0031,0x28,0x816A3128},
 {0x81670E08,0x81670E68,0x480322C1,0x80,0xB0033,0x18,0x816A3128},
 {0x816B1A20,0x816B1A50,0x4BFF1531,0x70,0xB0004,4,0x816A2F80},
};
std::optional<uint32_t> Dword(uint64_t raw) {
 const auto low=uint32_t(raw);
 return raw==low || raw==uint64_t(int64_t(int32_t(low)))?std::optional(low):std::nullopt;
}
uint32_t BigWord(std::span<const uint8_t> bytes,size_t offset=0) {
 return uint32_t(bytes[offset])<<24 | uint32_t(bytes[offset+1])<<16 |
        uint32_t(bytes[offset+2])<<8 | uint32_t(bytes[offset+3]);
}
std::optional<uint32_t> ReadWord(Memory& memory,uint32_t address) {
 std::array<uint8_t,4> bytes;
 return internal::ReadNativeWaitSnapshot(memory,address,bytes)?std::optional(BigWord(bytes)):std::nullopt;
}
struct SaveFrame {
 XThread* thread=nullptr;
 uint32_t id=0,pcr=0,sp=0,lr=0,function=0;
 bool operator==(const SaveFrame&) const=default;
 NativeXmsgProvenance::FrameFact Fact() const {return {id,pcr,sp,lr,function};}
};
struct CriticalSnapshot {
  // Actual CURRENT HLE representation: lock_count is plain host int32, while
  // recursion_count and owning_thread are BE. Do not invent a uniform wire
  // conversion or replace the existing primitive semantics in this owner.
  int32_t host_lock_count=0,recursion=0;
  uint32_t owner_kthread=0;
};
std::optional<CriticalSnapshot> ReadCriticalSnapshot(Memory& memory,uint32_t at) {
  if(!at)return {};
  std::array<uint8_t,28> bytes;
  if(!internal::ReadNativeWaitSnapshot(memory,at,bytes))return {};
  CriticalSnapshot result;std::memcpy(&result.host_lock_count,bytes.data()+0x10,4);
  result.recursion=int32_t(BigWord(bytes,0x14));result.owner_kthread=BigWord(bytes,0x18);
  return result;
}
std::optional<SaveFrame> ActualSaveFrame(KernelState& kernel,cpu::ppc::PPCContext& c,uint32_t at) {
 if(!XThread::IsInThread())return {};
 auto* thread=XThread::GetCurrentThread();const auto sp=Dword(c.r[1]),lr=Dword(c.lr),pcr=Dword(c.r[13]);
 if(!thread || !XThread::IsInThread(thread) || thread->kernel_state()!=&kernel ||
    c.kernel_state!=&kernel || c.processor!=kernel.processor() || c.thread_state!=thread->thread_state() ||
    thread->thread_state()->context()!=&c || !sp || !*sp || (*sp&15) || !lr ||
    !pcr || *pcr!=thread->pcr_ptr())return {};
 const auto process=thread->native_frame_thread_identity().process_type;
 if(process!=X_PROCTYPE_SYSTEM && process!=X_PROCTYPE_TITLE)return {};
 return SaveFrame{thread,thread->thread_id(),*pcr,*sp,*lr,at};
}
std::optional<uint32_t> OriginalKernelProcessType(Memory& memory,const SaveFrame& frame) {
  // Match the actual KeGetCurrentProcessType ABI used by BF40/BA28, including
  // the existing DPC override. The immutable native-frame owner type alone
  // is not the value returned by that original kernel getter.
  const auto active=ReadWord(memory,frame.pcr+offsetof(X_KPCR,prcb_data)+offsetof(X_KPRCB,dpc_active));
  if(!active)return {};
  std::array<uint8_t,1> type;
  if(*active) {
    if(!internal::ReadNativeWaitSnapshot(memory,frame.pcr+offsetof(X_KPCR,processtype_value_in_dpc),type))return {};
  }else {
    const auto current=ReadWord(memory,frame.pcr+offsetof(X_KPCR,prcb_data)+offsetof(X_KPRCB,current_thread));
    if(!current || *current!=frame.thread->guest_object() ||
        !internal::ReadNativeWaitSnapshot(memory,*current+offsetof(X_KTHREAD,process_type),type))return {};
  }
  return type[0];
}
bool HashSaveCode(std::span<const uint8_t> bytes,std::string_view expected) {
 std::array<uint8_t,32> hash;
 if(mbedtls_sha256(bytes.data(),bytes.size(),hash.data(),0))return false;
 constexpr char hex[]="0123456789abcdef";
 if(expected.size()!=64)return false;
 for(size_t n=0;n<hash.size();++n)
  if(expected[2*n]!=hex[hash[n]>>4] || expected[2*n+1]!=hex[hash[n]&15])return false;
 return true;
}
bool SameRequestBytes(const NativeProfileSaveRequest& a,const NativeProfileSaveRequest& b) {
 const auto x=a.descriptor(),y=b.descriptor();
 return a.message()==b.message() && x.size()==y.size() && std::equal(x.begin(),x.end(),y.begin()) &&
   a.large().address==b.large().address && a.large().declared_bytes==b.large().declared_bytes &&
   a.large().bytes==b.large().bytes && a.small().address==b.small().address &&
   a.small().declared_bytes==b.small().declared_bytes && a.small().bytes==b.small().bytes;
}
}  // namespace
struct NativeProfileSaveOwner::State {
  explicit State(Config value):config(std::move(value)),kernel(config.catalog->calls()->kernel()) {}
  Config config;
  KernelState* kernel=nullptr;
  std::shared_ptr<KernelState::UserModuleExecutionLease> root_code;
  NativeImportDomain::Operation resident_domain;
  static constexpr uint64_t kObserversClosed=uint64_t(1)<<63;
  // One atomic admission word closes the check/increment race at actual stop.
  // This counts HOST hook borrows only, never native work/Task/DB completion.
  std::atomic<uint64_t> observers{0};
  std::atomic<bool> retired{false};
  struct Observation {
    State* owner=nullptr;
    explicit Observation(State& state) noexcept {
      auto value=state.observers.load(std::memory_order_acquire);
      while(!(value&kObserversClosed) && value!=kObserversClosed-1) {
        if(state.observers.compare_exchange_weak(value,value+1,std::memory_order_acq_rel)) {
          owner=&state;break;
        }
      }
    }
    Observation(const Observation&)=delete;
    ~Observation(){if(owner)owner->observers.fetch_sub(1,std::memory_order_acq_rel);}
    explicit operator bool() const noexcept {return owner!=nullptr;}
  };
  // This serial is a HOST source-preparation boundary only. It must never be
  // acquired by a native callback holding a DB lock or mistaken for native
  // dirty-cache/callback/Task drainage. No source capability crosses this lock.
  std::mutex preparation;
  const std::shared_ptr<std::atomic<bool>> closed=std::make_shared<std::atomic<bool>>(false);
  std::vector<std::shared_ptr<NativeProfileSaveManagedGpd>> gpd;
  std::vector<std::shared_ptr<NativeProfileSaveManagedPicture>> pictures;
  save_detail::SemanticUses semantic_uses;
  // Typed participant of the EXISTING File/IO22 request, not a new scheduler,
  // native overlap/APC/Event owner or per-setting mutation ledger.
  class WriteUse final:public vfs::FileWriteCheckpointUse {
   public:
    WriteUse(object_ref<XFile> file_owner,vfs::File* file,std::shared_ptr<NativeProfileSaveManagedGpd> service,
        std::shared_ptr<NativeProfileSavePublication> publication,uint32_t title,uint32_t offset,
        std::vector<uint8_t> input,std::vector<uint8_t> header,std::shared_ptr<WriteOutcome> outcome)
        :file_owner_(std::move(file_owner)),file_(file),service_(std::move(service)),publication_(std::move(publication)),title_(title),offset_(offset),
         input_(std::move(input)),header_(std::move(header)),outcome_(std::move(outcome)) {}
    std::span<const uint8_t> input() const noexcept override {return input_;}
    X_STATUS ValidateTransfer(vfs::File& file,uint64_t offset,uint32_t length) const noexcept override {
      try {
        std::error_code error;
        if(&file!=file_ || offset!=offset_ || length!=input_.size() ||
            !NativeProfileSaveManagedGpd::MatchesOwnedPublication(file,publication_) ||
            !publication_->CurrentForStage(error))return error?vfs::HostStorageError(error):X_STATUS_ACCESS_DENIED;
        return X_STATUS_SUCCESS;
      }catch(...){return X_STATUS_UNSUCCESSFUL;}
    }
    void TransferRefused(X_STATUS status) noexcept override {
      outcome_->status.store(status,std::memory_order_relaxed);outcome_->bytes.store(0,std::memory_order_relaxed);
      outcome_->phase.store(WriteOutcome::Phase::kTransferRefused,std::memory_order_release);
    }
    X_STATUS AfterTransfer(vfs::File& file,X_STATUS status,size_t bytes) noexcept override {
      const auto finish=[&](WriteOutcome::Phase phase,X_STATUS value){
        outcome_->status.store(value,std::memory_order_relaxed);outcome_->bytes.store(uint32_t(bytes),std::memory_order_relaxed);
        outcome_->phase.store(phase,std::memory_order_release);return value;
      };
      try {
        if(&file!=file_ || XFAILED(status) || bytes!=input_.size())
          return finish(WriteOutcome::Phase::kBackendFailed,XFAILED(status)?status:X_STATUS_UNSUCCESSFUL);
        if(publication_->RequiresWholeOperationCheckpoint()) {
          if(const auto error=publication_->ObserveJoinedGpdAtOriginalWrite(title_,header_))
            return finish(WriteOutcome::Phase::kPublicationFailed,vfs::HostStorageError(error));
          // This is only the actual successful staged byte write. The original
          // outer operation still owes its whole Header/PNG/GPD checkpoint,
          // durable transaction, cache readiness and native result/notification.
          return finish(WriteOutcome::Phase::kStagedAwaitingWholeCheckpoint,status);
        }
        if(const auto error=publication_->SealManagedGpdAtOriginalDbCut(title_,header_))
          return finish(WriteOutcome::Phase::kPublicationFailed,vfs::HostStorageError(error));
        const auto result=publication_->PublishAtOriginalCommitCut();
        outcome_->transaction_state.store(uint32_t(result.state),std::memory_order_relaxed);
        outcome_->intent_durable.store(result.storage.intent_durable,std::memory_order_relaxed);
        outcome_->done_durable.store(result.storage.done_durable,std::memory_order_relaxed);
        outcome_->journal_attempted.store(!result.attempted_journal.empty() || !result.storage.journal.empty() ||
            result.storage.intent_durable || result.storage.done_durable ||
            result.state==xam::ProfileGpdTransactionAttempt::State::kPreparationUncertain,std::memory_order_relaxed);
        using Phase=xam::ProfileGpdTransactionAttempt::State;
        if(result.error || (result.state!=Phase::kCommitted && result.state!=Phase::kAlreadyCommitted))
          return finish(WriteOutcome::Phase::kPublicationFailed,
              vfs::HostStorageError(result.error?result.error:Error(std::errc::device_or_resource_busy)));
        // Already committed storage/model remains factually committed even if
        // actual File readiness later refuses. No rollback/synthetic success.
        const auto ready=service_->PromoteCommittedAtActualDbQuiescence(file,publication_,header_);
        return finish(XSUCCEEDED(ready)?WriteOutcome::Phase::kReady:WriteOutcome::Phase::kCommittedCachePending,ready);
      }catch(...){return finish(WriteOutcome::Phase::kPublicationFailed,X_STATUS_UNSUCCESSFUL);}
    }
   private:
    const object_ref<XFile> file_owner_; // Actual retained File, no pointer/handle ABA.
    vfs::File* const file_; // Stable backend owned by this exact XFile/IO22 use.
    const std::shared_ptr<NativeProfileSaveManagedGpd> service_;
    const std::shared_ptr<NativeProfileSavePublication> publication_;
    const uint32_t title_,offset_;
    const std::vector<uint8_t> input_,header_;
    const std::shared_ptr<WriteOutcome> outcome_;
  };
  class PictureWriteUse final:public vfs::FileWriteCheckpointUse {
   public:
    PictureWriteUse(object_ref<XFile> file_owner,vfs::File* file,std::shared_ptr<NativeProfileSavePublication> publication,
        filesystem::ProfileArtifactKind kind,uint32_t offset,std::vector<uint8_t> input,std::shared_ptr<WriteOutcome> outcome)
        :file_owner_(std::move(file_owner)),file_(file),publication_(std::move(publication)),kind_(kind),offset_(offset),
         input_(std::move(input)),outcome_(std::move(outcome)) {}
    std::span<const uint8_t> input() const noexcept override{return input_;}
    X_STATUS ValidateTransfer(vfs::File& file,uint64_t offset,uint32_t length) const noexcept override {
      try {std::error_code error;
        if(&file!=file_ || offset!=offset_ || length!=input_.size() ||
            !NativeProfileSavePictureStage::MatchesOwnedFile(file,publication_,kind_) ||
            !publication_->CurrentForStage(error))return error?vfs::HostStorageError(error):X_STATUS_ACCESS_DENIED;
        return X_STATUS_SUCCESS;
      }catch(...){return X_STATUS_UNSUCCESSFUL;}
    }
    void TransferRefused(X_STATUS status) noexcept override {
      outcome_->status.store(status,std::memory_order_relaxed);outcome_->bytes.store(0,std::memory_order_relaxed);
      outcome_->phase.store(WriteOutcome::Phase::kTransferRefused,std::memory_order_release);
    }
    X_STATUS AfterTransfer(vfs::File& file,X_STATUS status,size_t bytes) noexcept override {
      const bool complete=&file==file_ && XSUCCEEDED(status) && bytes==input_.size();
      const auto result=complete?status:XFAILED(status)?status:X_STATUS_UNSUCCESSFUL;
      outcome_->status.store(result,std::memory_order_relaxed);outcome_->bytes.store(uint32_t(bytes),std::memory_order_relaxed);
      outcome_->phase.store(complete?WriteOutcome::Phase::kStagedAwaitingWholeCheckpoint:
          WriteOutcome::Phase::kBackendFailed,std::memory_order_release);
      // Original B8F8 tests BOOL only. A successful short byte count is still
      // an actual failed host transfer; never let that become a sealed SAVE.
      return result;
    }
   private:
    const object_ref<XFile> file_owner_;vfs::File* const file_;
    const std::shared_ptr<NativeProfileSavePublication> publication_;
    const filesystem::ProfileArtifactKind kind_;const uint32_t offset_;
    const std::vector<uint8_t> input_;const std::shared_ptr<WriteOutcome> outcome_;
  };
  class HeaderWriteUse final:public vfs::FileWriteCheckpointUse {
   public:
    HeaderWriteUse(object_ref<XFile> owner,vfs::File* file,
        std::shared_ptr<NativeProfileSavePublication> publication,uint32_t offset,
        std::vector<uint8_t> input,std::shared_ptr<WriteOutcome> outcome)
        :file_owner_(std::move(owner)),file_(file),publication_(std::move(publication)),
         offset_(offset),input_(std::move(input)),outcome_(std::move(outcome)) {}
    std::span<const uint8_t> input() const noexcept override{return input_;}
    X_STATUS ValidateTransfer(vfs::File& file,uint64_t offset,uint32_t length) const noexcept override {
      try {
        if(&file!=file_ || offset!=offset_ || length!=input_.size())return X_STATUS_ACCESS_DENIED;
        return publication_->PrepareHeaderAtOriginalWrite(file);
      }catch(...){return X_STATUS_UNSUCCESSFUL;}
    }
    void TransferRefused(X_STATUS status) noexcept override {
      outcome_->status.store(status,std::memory_order_relaxed);outcome_->bytes.store(0,std::memory_order_relaxed);
      outcome_->phase.store(WriteOutcome::Phase::kTransferRefused,std::memory_order_release);
    }
    X_STATUS AfterTransfer(vfs::File& file,X_STATUS status,size_t bytes) noexcept override {
      const bool complete=&file==file_ && XSUCCEEDED(status) && bytes==input_.size();
      const auto result=complete?status:XFAILED(status)?status:X_STATUS_UNSUCCESSFUL;
      outcome_->status.store(result,std::memory_order_relaxed);outcome_->bytes.store(uint32_t(bytes),std::memory_order_relaxed);
      outcome_->phase.store(complete?WriteOutcome::Phase::kStagedAwaitingWholeCheckpoint:
          WriteOutcome::Phase::kBackendFailed,std::memory_order_release);
      // Factual byte result only: no seal, journal, cache-ready, notification,
      // guest result rewriting or implicit commit at raw Write/Flush/Destroy.
      return result;
    }
   private:
    const object_ref<XFile> file_owner_;
    vfs::File* const file_;
    const std::shared_ptr<NativeProfileSavePublication> publication_;
    const uint32_t offset_;
    const std::vector<uint8_t> input_;
    const std::shared_ptr<WriteOutcome> outcome_;
  };
  class WriteAuthority final:public vfs::FileWriteCheckpointAuthority {
   public:
    WriteAuthority(std::unique_ptr<vfs::FileWriteCheckpointUse> use,std::shared_ptr<WriteOutcome> outcome)
        :use_(std::move(use)),outcome_(std::move(outcome)) {}
    std::unique_ptr<vfs::FileWriteCheckpointUse> AcquireForActualWrite(vfs::File& file,X_STATUS& refusal) const noexcept override {
      try {
        (void)file;std::unique_ptr<vfs::FileWriteCheckpointUse> use;
        {std::lock_guard lock(mutex_);use=std::move(use_);}
        if(!use){refusal=X_STATUS_ACCESS_DENIED;return {};}
        // ValidateTransfer checks exact actual File+offset under its normal IO
        // lock. Every resource leaves this historical authority before return.
        outcome_->phase.store(WriteOutcome::Phase::kEntered,std::memory_order_release);refusal=X_STATUS_SUCCESS;return use;
      }catch(...){refusal=X_STATUS_UNSUCCESSFUL;return {};}
    }
   private:
    mutable std::mutex mutex_;
    mutable std::unique_ptr<vfs::FileWriteCheckpointUse> use_;
    const std::shared_ptr<WriteOutcome> outcome_;
  };
  // One actual native GetDB/Ensure/edit/release USE. B0031 uses the embedded
  // scope; B0037 owns only its actually reached public/personal key scopes in
  // this SAME Request. Cached DB/File lifetime is independent of use lifetime.
  // There is no new registry, thread, native Task, request or ambient authority.
  struct GpdUse {
    SaveFrame image_leaf,image_db,image_core,image_try,image_db_release,image_unlock,
              image_ensure,image_file_open,image_stream_open,image_edit;
    std::shared_ptr<const NativeProfileSaveManagedGpd::ReadReceipt> open_receipt;
    std::shared_ptr<NativeProfileGpdReadView> opened_view;
    // Exact actual File retained across one authenticated cached GetDB/Ensure
    // use; not a registry/object lifetime or constructor/source permission.
    object_ref<XFile> cached_before_file;

    uint32_t native_stream=0,native_stream_result=0,opened_handle=0,image_open_flags=0;
    bool image_ensure_entered=false,image_file_open_entered=false,image_stream_entered=false,
         image_stream_returned=false,image_open_captured=false,image_ensure_returned=false,
         native_before_header_valid=false;
    uint32_t image_ensure_result=0;
    uint64_t image_actual_key=0;
    SaveFrame image_insert;bool image_insert_entered=false,image_insert_returned=false;
    uint32_t image_insert_result=0;
    SaveFrame stream_write;
    uint32_t stream_write_buffer=0,stream_write_length=0,stream_write_offset=0;
    uint64_t stream_write_serial=0;
    bool stream_write_entered=false,stream_write_returned=false,stream_checkpoint_captured=false;
    std::shared_ptr<WriteOutcome> write_outcome;
    std::shared_ptr<NativeProfileSaveManagedGpd> write_service;
    std::shared_ptr<ManagedFileCut> pristine_noop_cut;bool pristine_noop_settling=false;
    uint32_t image_edit_result=0,image_bind_status=X_STATUS_UNSUCCESSFUL;
    bool image_bind_attempted=false;
    bool image_edit_entered=false,image_edit_returned=false,
         image_actual_input_matches=false,image_actual_autosave=false;
    std::vector<uint8_t> native_before_header_index;
    // Same actual completed stage epoch for a later native key's retained
    // cached File; the immutable original view remains identity metadata only.
    std::shared_ptr<const NativeProfileGpdCheckpoint> reused_checkpoint;
    uint32_t edit_entry=0,edit_buffer=0,edit_length=0,edit_kind=0;
    std::optional<uint32_t> native_first_failure;
    uint32_t normalized_title=0,image_db_output=0,image_db_expected=0,
             image_db_result=0,image_db_pointer=0,image_db_cs=0,image_db_resource=0;
    std::optional<CriticalSnapshot> image_db_return_lock;
    std::optional<CriticalSnapshot> image_try_before,image_try_after,image_unlock_before;
    uint64_t image_try_attempts=0;
    uint32_t image_core_result=0,image_owned_cs=0,image_owned_resource=0;
    bool image_core_entered=false,image_core_returned=false,image_try_pending=false,
         image_try_acquired=false,image_lock_retained=false,image_release_entered=false,
         image_release_returned=false,image_unlock_entered=false,image_unlock_returned=false;
    bool image_leaf_entered=false,image_leaf_returned=false,image_db_entered=false,image_db_returned=false;
  };
  struct Request {
    Request()=default;
    Request(const Request&)=delete;Request& operator=(const Request&)=delete;
    // gpd points into this stable Request or one owned stable ParentKeyUse.
    // Request is never relocated/copied while a native or host duty uses it.
    uint64_t invocation=0;
    struct HeaderChild {
      NativeXmsgProvenance::Capture provenance;
      NativeXmsgProvenance::PublicInvocationFact public_fact;
      std::shared_ptr<const NativeXmsgProvenance::DispatchFact> dispatch;
      SaveFrame producer,public_frame,worker;
      std::array<uint8_t,0x218> input{};
      std::vector<uint8_t> thumbnail,before;
      struct HeaderWrite {
        SaveFrame entry;bool returned=false;uint32_t result=0;
        std::shared_ptr<WriteOutcome> output;
      };
      std::optional<HeaderWrite> write;
      std::vector<std::shared_ptr<WriteOutcome>> write_outputs;
      uint32_t context=0,backing_handle=0,device_body=0,worker_result=0;
      object_ref<XFile> backing;
      object_ref<XStfsDevice> device;
      bool worker_entered=false,worker_returned=false,before_captured=false,
           uncertain=false;
    };
    std::shared_ptr<HeaderChild> header_child;
    struct CloseChild {
      enum class Kind {kFinalClose,kInPlaceFlush};
      Kind kind=Kind::kFinalClose;
      SaveFrame public_frame;
      uint32_t e60_body_sp=0,e60_backchain_sp=0,e60_saved_lr=0;
      uint32_t context_result=0,producer_result=0;
      bool producer_returned=false,native_skip=false;
      NativeXmsgProvenance::Capture provenance;
      NativeXmsgProvenance::PublicInvocationFact public_fact;
      std::shared_ptr<const NativeXmsgProvenance::DispatchFact> dispatch;
      SaveFrame producer,worker,context_close;
      std::array<uint8_t,0x44> input{};
      object_ref<XFile> backing;object_ref<XStfsDevice> device;
      std::shared_ptr<vfs::Device> member_device;
      std::shared_ptr<XFileIoCheckpoint> io_cut;
      std::shared_ptr<WriteOutcome> whole_outcome;
      bool whole_checkpoint_captured=false;
      std::optional<HeaderChild::HeaderWrite> write;
      std::vector<std::shared_ptr<WriteOutcome>> write_outputs;
      uint32_t context=0,worker_result=0;
      bool header_input_bound=false;
      X_STATUS control0=X_STATUS_PENDING,control1=X_STATUS_PENDING,control2=X_STATUS_PENDING,control3=X_STATUS_PENDING,control4=X_STATUS_PENDING;
      X_STATUS cut_status=X_STATUS_PENDING;
      bool worker_entered=false,worker_returned=false,context_entered=false,
           context_returned=false,control1_returned=false,uncertain=false;
    };
    std::shared_ptr<CloseChild> close_child;

    struct PictureFileCall {
      SaveFrame entry;
      filesystem::ProfileArtifactKind kind=filesystem::ProfileArtifactKind::kPublic64;
      uint32_t name=0,buffer=0,length=0,result=0;
      bool returned=false;
      std::shared_ptr<WriteOutcome> output;
    };
    std::optional<PictureFileCall> picture_call;
    std::vector<std::shared_ptr<WriteOutcome>> picture_write_outputs;
    uint32_t parent_picture_sp=0;
    struct PictureCopyCall {
      SaveFrame entry;filesystem::ProfileArtifactKind source,target;
      bool returned=false;uint32_t result=0;
      std::shared_ptr<WriteOutcome> last_output;
    };
    std::shared_ptr<PictureCopyCall> picture_copy;
    std::vector<std::shared_ptr<PictureCopyCall>> picture_copies;
    std::vector<uint32_t> picture_copy_sites;

    ProducerSite site{};
    SaveFrame public_frame,producer,inner,selection,body,getter,release,normalize;
    GpdUse initial_gpd;
    GpdUse* gpd=&initial_gpd;
    struct ParentKeyUse {
      enum class Kind {kPublic,kPersonal};
      Kind kind=Kind::kPublic;
      SaveFrame helper,setter,setting;
      uint32_t key_object=0,setting_descriptor=0,result=0,setter_result=0,setting_result=0;
      uint32_t value_address=0,value_bytes=0;
      bool helper_returned=false,setter_entered=false,setter_returned=false,
           setting_entered=false,setting_returned=false;
      GpdUse database;
      std::shared_ptr<CloseChild> main_flush;
      SaveFrame post_flush;
      bool post_flush_entered=false,post_flush_returned=false;
      bool post_skip_no_changes=false,post_skip_native_gate=false;
      bool pec_gate_seen=false,pec_expected=false;
      std::optional<uint32_t> post_gate_failure;
      std::shared_ptr<const NativeProfileGpdCheckpoint> completed_checkpoint;
      std::array<bool,3> edit_kinds{}; // SyncData, requested setting, SyncList.
    };
    std::vector<std::unique_ptr<ParentKeyUse>> parent_keys;
    ParentKeyUse* active_parent_key=nullptr;
    object_ref<XThread> caller;
    std::shared_ptr<KernelState::UserModuleExecutionLease> caller_code;
    NativeImportDomain::Operation domain;
    NativeXmsgProvenance::PublicInvocationFact public_fact;
    NativeXmsgProvenance::Capture provenance;
    std::optional<NativeProfileSaveRequest> input;
    std::shared_ptr<const NativeXmsgProvenance::DispatchFact> dispatch;
    // Captured from the actual selected getter after its original array lock
    // returned. This distinct counted PM issuer is NOT a native DB/File or
    // mutation capability; exact native source/view/pre-edit admission must
    // still agree before a publication or open authority can be created.
    std::shared_ptr<xam::ProfileRosterNativeWriter> selected_source;
    std::shared_ptr<save_detail::SemanticUses::Span> semantic_use;
    std::error_code source_error;
    std::shared_ptr<NativeProfileSavePublication> publication;
    // Real future IO/transaction integration must retain these exact request
    // duties. A journal-attempted publication cannot become settled by expiry.
    uint32_t host_io_duties=0;
    bool publication_settled=false,subscription_ended=false,
         source_retiring=false,source_retired=false,caller_retiring=false,caller_retired=false;
    uint64_t owned_input_bytes=0;
    // Issued only after both real parent key-helper/native DB/current-table
    // lifecycles finish. Initial BEFORE prefix bytes are NOT this receipt.
    // Producer integration remains mandatory before required-GPD activation.
    std::shared_ptr<const NativeProfileGpdCheckpoint> whole_gpd_cache;
    // Separate historical HOST recovery result. The failed native write
    // outcome is immutable; a later durable commit never fakes caller success
    // or native cache readiness. The same pending owner remains until joined.
    bool recovery_running=false,recovery_storage_committed=false,recovery_requires_restart=false;
    std::optional<xam::ProfileGpdTransactionAttempt> recovery_result;
    uint64_t native_profile_xuid=0,identity_job_source=0;
    uint32_t native_profile=0,native_array=0;
    uint32_t descriptor=0,inner_descriptor=0,body_descriptor=0,overlap=0,native_overlap=0,original_result=0;
    bool selection_entered=false,selection_returned=false;
    bool public_returned=false,inner_entered=false,inner_returned=false,uncertain=false;
    bool body_entered=false,body_returned=false,getter_entered=false,getter_returned=false,
         release_entered=false,release_returned=false;
    bool normalize_entered=false,normalize_returned=false;
    // Actual getter/DB/IO duties are added to THIS request, never a new family
    // envelope registry. A writer is not issued by this observation capsule.
  };
  // Called only inside the shared observer's EXISTING observation borrow,
  // after its single current-dispatch and code check. No duplicate lookup,
  // hash, borrow or native call is introduced for the parent branch.
  void ParentGpdEditEntry(void*,uint32_t,const std::shared_ptr<Request>&) noexcept;
  void ParentGpdEditReturn(void*,uint32_t,const std::shared_ptr<Request>&) noexcept;
  struct GpdCaller {
    bool parent=false;
    uint32_t function=0,body_bytes=0,db_return=0,holder_offset=0,
        ensure_return=0,ensure_db_register=0,descriptor_register=0,
        descriptor=0,release_return=0;
  };
  std::optional<GpdCaller> CurrentGpdCaller(const Request& r) {
    std::lock_guard held(requests_mutex);
    if(r.site.message==0xB0031 && r.gpd==&r.initial_gpd &&
        r.gpd->image_leaf_entered && r.gpd->image_leaf.function==0x819101D0)
      return GpdCaller{false,0x819101D0,0xA0,0x81910218,0x50,0x81910248,29,30,r.inner_descriptor,0x81910310};
    const auto* key=r.active_parent_key;
    if(r.site.message==0xB0037 && key && r.gpd==&key->database &&
        key->setter_entered && !key->setter_returned && !key->helper_returned &&
        key->setter==r.gpd->image_leaf && r.gpd->image_leaf_entered)
      return GpdCaller{true,0x81912580,0xB0,0x819125E8,0x54,0x81912618,26,28,key->setting_descriptor,0x819126B4};
    return {};
  }
  struct HostOpenUse {
    std::shared_ptr<State> state;
    std::shared_ptr<Request> request;
    bool admitted=false;
    HostOpenUse(std::shared_ptr<State> owner,std::shared_ptr<Request> value)
        :state(std::move(owner)),request(std::move(value)) {
      std::lock_guard lock(state->requests_mutex);
      if(!state->retired.load(std::memory_order_acquire) && !request->uncertain &&
          request->inner_entered && !request->inner_returned && request->semantic_use &&
          request->semantic_use->unchanged() &&
          ((request->site.message==0xB0031 && request->gpd->image_lock_retained &&
            request->gpd->image_stream_entered && !request->gpd->image_stream_returned) ||
           (request->site.message==0xB0037 &&
            ((request->picture_call && !request->picture_call->returned) ||
             (request->picture_copy && !request->picture_copy->returned) ||
              (request->active_parent_key && request->active_parent_key->setter_entered &&
               !request->active_parent_key->setter_returned && request->gpd->image_lock_retained &&
               request->gpd->image_stream_entered && !request->gpd->image_stream_returned)) &&
            request->publication && request->publication->parent_operation_)) &&
          request->host_io_duties!=UINT32_MAX) {++request->host_io_duties;admitted=true;}
    }
    ~HostOpenUse() {
      if(admitted) {std::lock_guard lock(state->requests_mutex);--request->host_io_duties;}
    }
    // Only ReadAuthority owns this duty; Request stores just a unique metadata
    // receipt, never the authority/duty. Thus no State→Request→State cycle.
  };
  struct HostHeaderOpenUse {
    std::shared_ptr<State> state;std::shared_ptr<Request> request;bool admitted=false;
    HostHeaderOpenUse(std::shared_ptr<State> s,std::shared_ptr<Request> r):state(std::move(s)),request(std::move(r)) {
      std::lock_guard held(state->requests_mutex);
      if(!state->retired.load() && !request->uncertain && request->header_child &&
          request->header_child->worker_entered && !request->header_child->worker_returned &&
          !request->header_child->uncertain && request->publication &&
          request->semantic_use && request->semantic_use->unchanged() && request->host_io_duties!=UINT32_MAX) {
        ++request->host_io_duties;admitted=true;
      }
    }
    ~HostHeaderOpenUse(){if(admitted){std::lock_guard held(state->requests_mutex);--request->host_io_duties;}}
  };
  struct HostCloseIoUse {
    std::shared_ptr<State> state;std::shared_ptr<Request> request;bool admitted=false;
    HostCloseIoUse(std::shared_ptr<State> s,std::shared_ptr<Request> r):state(std::move(s)),request(std::move(r)) {
      std::lock_guard held(state->requests_mutex);
      if(!state->retired.load(std::memory_order_acquire) && !request->uncertain &&
          request->close_child && !request->close_child->uncertain &&
          request->close_child->context_entered && !request->close_child->context_returned &&
          request->close_child->worker_entered && !request->close_child->worker_returned &&
          request->publication && request->semantic_use && request->semantic_use->unchanged() &&
          request->host_io_duties!=UINT32_MAX) {++request->host_io_duties;admitted=true;}
    }
    ~HostCloseIoUse(){if(admitted){std::lock_guard held(state->requests_mutex);--request->host_io_duties;}}
    // Owned only by the actual carrier participant; Request retains just its
    // scalar outcome. No State -> Request -> State ownership cycle.
  };
  std::shared_ptr<Request> CurrentHeader(cpu::ppc::PPCContext& c) {
    std::vector<std::shared_ptr<Request>> candidates;
    {std::lock_guard held(requests_mutex);for(const auto& r:requests)
      if(r->header_child && r->header_child->worker_entered && !r->header_child->worker_returned &&
          !r->uncertain && !r->header_child->uncertain && r->header_child->provenance)candidates.push_back(r);}
    std::shared_ptr<Request> found;
    for(const auto& r:candidates) {
      const auto fact=NativeXmsgProvenance::CurrentDispatch(&c,r->header_child->provenance);
      if(!fact || fact!=r->header_child->dispatch)continue;
      if(found)return {};found=r;
    }
    return found;
  }
  struct ContentUse {std::shared_ptr<Request> request;std::shared_ptr<Request::CloseChild> operation;};
  ContentUse CurrentContent(cpu::ppc::PPCContext& c) {
    struct Candidate {ContentUse use;NativeXmsgProvenance::Capture provenance;
      std::shared_ptr<const NativeXmsgProvenance::DispatchFact> dispatch;};
    std::vector<Candidate> candidates;
    {
      std::lock_guard held(requests_mutex);
      for(const auto& r:requests) {
        if(r->uncertain)continue;
        const auto add=[&](const std::shared_ptr<Request::CloseChild>& child) {
          if(child && child->provenance && !child->uncertain)
            candidates.push_back({{r,child},child->provenance,child->dispatch});
        };
        add(r->close_child);
        for(const auto& key:r->parent_keys)if(key)add(key->main_flush);
      }
    }
    ContentUse found;
    for(const auto& candidate:candidates) {
      const auto fact=NativeXmsgProvenance::CurrentDispatch(&c,candidate.provenance);
      if(!fact || (candidate.dispatch && fact!=candidate.dispatch))continue;
      if(found.request)return {};found=candidate.use;
    }
    return found;
  }
  std::shared_ptr<Request> CurrentClose(cpu::ppc::PPCContext& c) {
    const auto use=CurrentContent(c);
    return use.operation && use.operation->kind==Request::CloseChild::Kind::kFinalClose
        ?use.request:nullptr;
  }
  std::mutex requests_mutex,retirement_mutex;
  std::condition_variable changed;
  std::thread reaper;
  bool reaper_stop=false,reaper_failed=false;
  uint64_t invocation=0,owned_input_bytes=0;
  std::vector<std::shared_ptr<Request>> requests;
  bool CodeCurrent(uint32_t at) const {
    if(retired.load(std::memory_order_acquire) || !root_code || !resident_domain ||
       !config.catalog || !config.domain ||
       !config.domain->active() || !config.allocation ||
       !config.allocation->MatchesDomain(config.domain))return false;
    const auto& calls=config.catalog->calls();const auto module=calls?calls->module():nullptr;
    const auto& root=config.domain->root();
    if(!calls || !calls->kernel() || !module || !module->xex_module() || !module->native_import_ready() ||
       module->hmodule_ptr()!=root.hmodule || module->load_generation()!=root.generation ||
       calls->load_generation()!=root.generation)return false;
    const auto found=std::find_if(std::begin(save_code),std::end(save_code),[&](const auto& code) {
      return at>=code.at && uint64_t(at)<uint64_t(code.at)+code.bytes;
    });
    if(found==std::end(save_code))return false;
    auto lock=global_critical_region::AcquireDirect();auto& memory=*calls->kernel()->memory();
    if(memory.QueryRangeAccess(found->at,found->bytes)==memory::PageAccess::kNoAccess)return false;
    for(uint32_t p=found->at;p<found->at+found->bytes;p+=4) {
      xex2_section_type type;
      if(calls->kernel()->GetUserModuleForAddress(p).get()!=module.get() ||
         calls->kernel()->processor()->LookupModule(p)!=module->xex_module() ||
         !module->xex_module()->GetPageSectionType(p,&type) || type!=XEX_SECTION_CODE ||
         module->xex_module()->TranslateCode(p)!=memory.TranslateVirtual<const uint8_t*>(p))return false;
    }
    return HashSaveCode({memory.TranslateVirtual<const uint8_t*>(found->at),found->bytes},found->sha);
  }
  std::shared_ptr<Request> CurrentInner(cpu::ppc::PPCContext& c) {
    std::vector<std::shared_ptr<Request>> candidates;
    {std::lock_guard lock(requests_mutex);for(const auto& r:requests)
      if(r->inner_entered && !r->inner_returned && !r->uncertain)candidates.push_back(r);}
    std::shared_ptr<Request> found;
    for(const auto& r:candidates) {
      const auto fact=NativeXmsgProvenance::CurrentDispatch(&c,r->provenance);
      if(!fact || !r->dispatch || fact->capture!=r->dispatch->capture ||
         fact->provider_invocation!=r->dispatch->provider_invocation)continue;
      if(found)return {};found=r;
    }
    return found;
  }
  bool CallerCanReap(const Request& r) const {
    return r.public_returned && !r.caller_retiring && !r.caller_retired;
  }
  static bool GpdUseEnded(const GpdUse& use) {
    if(use.image_lock_retained || use.image_try_pending ||
        (use.image_leaf_entered && !use.image_leaf_returned) ||
        (use.image_db_entered && !use.image_db_returned) ||
        (use.image_core_entered && !use.image_core_returned) ||
        (use.image_ensure_entered && !use.image_ensure_returned) ||
        (use.image_stream_entered && !use.image_stream_returned) ||
        (use.image_edit_entered && !use.image_edit_returned) ||
        (use.image_insert_entered && !use.image_insert_returned) ||
        (use.stream_write_entered && !use.stream_write_returned) ||
        (use.image_release_entered && !use.image_release_returned) ||
        (use.image_unlock_entered && !use.image_unlock_returned))return false;
    return !use.image_db_returned || int32_t(use.image_db_result)<0 || use.image_release_returned;
  }
  static void ReleaseGpdUseHolders(GpdUse& use) {
    use.cached_before_file.reset();use.opened_view.reset();use.open_receipt.reset();
  }
  bool OriginalSourceUseEnded(const Request& r) const {
    if(r.host_io_duties || (r.picture_call && !r.picture_call->returned) ||
        (r.picture_copy && !r.picture_copy->returned))return false;
    if(r.header_child && (r.header_child->uncertain || !r.header_child->worker_returned))return false;
    if(r.close_child && (r.close_child->uncertain || !r.close_child->worker_returned ||
        (r.close_child->context_entered && !r.close_child->context_returned)))return false;
    // An unavailable subscription never issued host DB/source duties. Ending
    // these metadata bytes is not a declaration of native request completion.
    if(!r.provenance && !r.inner_entered && !r.selected_source && !r.publication)
      return r.public_returned;
    if(r.uncertain)return false;
    if(!r.inner_entered)return r.public_returned && r.subscription_ended;
    if(!r.inner_returned || (r.selection_entered && !r.selection_returned) ||
       (r.body_entered && !r.body_returned) ||
       (r.getter_entered && !r.getter_returned))return false;
    if(r.native_profile && !r.release_returned)return false;
    if(!GpdUseEnded(r.initial_gpd))return false;
    for(const auto& key:r.parent_keys)if(!key ||
        (key->main_flush && (key->main_flush->uncertain || !key->main_flush->worker_returned ||
          !key->main_flush->producer_returned ||
          (key->main_flush->context_entered && !key->main_flush->context_returned))) ||
        (key->post_flush_entered && !key->post_flush_returned) || !key->helper_returned ||
        (key->setter_entered && !key->setter_returned) ||
        (key->setting_entered && !key->setting_returned) || !GpdUseEnded(key->database))return false;
    return true;
  }
  bool SourceCanReap(const Request& r) const {
    return !r.source_retiring && !r.source_retired &&
        (!r.publication || r.publication_settled) && OriginalSourceUseEnded(r);
  }
  bool CanErase(const Request& r) const {
    return r.public_returned && r.caller_retired && r.source_retired && !r.host_io_duties;
  }
  void Reap() {
    std::vector<std::shared_ptr<Request>> observed,callers,sources,old;
    observed.reserve(128);callers.reserve(128);sources.reserve(128);old.reserve(128);
    {std::lock_guard lock(requests_mutex);for(const auto& r:requests)
      if(r->provenance && !r->inner_entered && !r->subscription_ended)observed.push_back(r);}
    for(const auto& r:observed) {
      const auto ended=NativeXmsgProvenance::Observe(r->provenance);
      if(!ended || (!ended->released && !ended->authority_revoked && !ended->unavailable && !ended->kernel_stopped))continue;
      std::lock_guard lock(requests_mutex);
      if(!r->inner_entered && !r->host_io_duties && !r->selected_source && !r->publication)
        r->subscription_ended=true;
    }
    std::vector<std::pair<std::shared_ptr<Request>,std::shared_ptr<ManagedFileCut>>> noops;noops.reserve(128);
    {std::lock_guard lock(requests_mutex);for(const auto& r:requests)
      if(r->gpd->pristine_noop_cut && !r->gpd->pristine_noop_settling && !r->host_io_duties && !r->publication_settled) {
        noops.emplace_back(r,r->gpd->pristine_noop_cut);r->gpd->pristine_noop_settling=true;
      }}
    for(auto& [r,cut]:noops) {
      const auto result=NativeProfileSaveOwner::PrepareManagedFileCut(cut);
      std::shared_ptr<ManagedFileCut> dropped;
      {std::lock_guard lock(requests_mutex);r->gpd->pristine_noop_settling=false;
        if(XSUCCEEDED(result) && r->gpd->pristine_noop_cut==cut) {
          r->publication_settled=true;dropped=std::move(r->gpd->pristine_noop_cut);
        }}
      dropped.reset(); // Actual retained File holders leave outside locks.
    }
    std::vector<std::shared_ptr<Request>> unbound;unbound.reserve(128);
    {std::lock_guard lock(requests_mutex);if(!reaper_stop)for(const auto& r:requests) {
      if(r->publication && !r->publication_settled && r->gpd->image_bind_attempted &&
          XFAILED(r->gpd->image_bind_status) && r->gpd->image_edit_returned &&
          r->inner_entered && r->inner_returned && OriginalSourceUseEnded(*r))unbound.push_back(r);
    }}
    for(const auto& r:unbound) {
      const auto error=r->publication->CancelUnboundAtActualRundown();
      if(!error){std::lock_guard lock(requests_mutex);r->publication_settled=true;}
      // The failed File never changed its immutable tuple. Any actual denied
      // native mutation taint stays in that File; no cache/layout is adopted.
    }
    std::vector<std::shared_ptr<Request>> recovery;recovery.reserve(128);
    {std::lock_guard lock(requests_mutex);if(!reaper_stop)for(const auto& r:requests) {
      const auto outcome=r->site.message==0xB0037 && r->close_child ?
          r->close_child->whole_outcome:r->gpd->write_outcome;
      if(!r->publication || r->publication_settled || r->recovery_running || r->recovery_storage_committed ||
          r->recovery_requires_restart || !outcome || !r->inner_entered || !r->inner_returned ||
          (r->site.message!=0xB0037 && !r->gpd->image_edit_returned) || !OriginalSourceUseEnded(*r))continue;
      const auto phase=outcome->phase.load(std::memory_order_acquire);
      if((phase!=WriteOutcome::Phase::kPublicationFailed && phase!=WriteOutcome::Phase::kCommittedCachePending) ||
          !outcome->journal_attempted.load(std::memory_order_relaxed))continue;
      // One owned HOST duty after original native locks/reference/IO have
      // genuinely returned. No native pointer, callback, new issuer or await.
      ++r->host_io_duties;r->recovery_running=true;recovery.push_back(r);
    }}
    for(const auto& r:recovery) {
      xam::ProfileGpdTransactionAttempt result;
      try {result=r->publication->RecoverAttemptedAtActualRundown();}
      catch(...){result.error=Error(std::errc::io_error);}
      using Phase=xam::ProfileGpdTransactionAttempt::State;
      std::lock_guard lock(requests_mutex);
      r->recovery_storage_committed=!result.error && (result.state==Phase::kCommitted || result.state==Phase::kAlreadyCommitted);
      r->recovery_requires_restart=result.state==Phase::kPreparationUncertain || result.state==Phase::kRetired;
      r->recovery_running=false;--r->host_io_duties;
      r->recovery_result=std::move(result);
      // Deliberately NOT publication_settled: the native cache may have taken
      // a failed reload/fallback. Its actual later checkpoint/close or factual
      // restart owns that duty; recovery cannot infer clean native layout.
    }
    {std::lock_guard lock(requests_mutex);for(const auto& r:requests) {
      if(CallerCanReap(*r)){r->caller_retiring=true;callers.push_back(r);}
      if(SourceCanReap(*r)){r->source_retiring=true;sources.push_back(r);}
    }}
    // No guest read, current-source eligibility or native cancel is needed to
    // release an OLD exact source on this host lane. Do not hold metadata/
    // GCRC/native locks across these genuine last-owner destructors.
    for(const auto& r:sources) {
      NativeXmsgProvenance::Release(r->provenance);
      if(r->header_child)NativeXmsgProvenance::Release(r->header_child->provenance);
      if(r->close_child)NativeXmsgProvenance::Release(r->close_child->provenance);
      for(const auto& key:r->parent_keys)if(key && key->main_flush)
        NativeXmsgProvenance::Release(key->main_flush->provenance);
      ReleaseGpdUseHolders(r->initial_gpd);
      for(const auto& key:r->parent_keys)if(key)ReleaseGpdUseHolders(key->database);
      r->publication.reset();r->selected_source.reset();
      std::lock_guard lock(requests_mutex);r->source_retired=true;r->source_retiring=false;
    }
    for(const auto& r:callers) {
      r->caller_code.reset();r->caller.reset();r->domain={};
      std::lock_guard lock(requests_mutex);r->caller_retired=true;r->caller_retiring=false;
    }
    {std::lock_guard lock(requests_mutex);
      for(const auto& r:requests)if(CanErase(*r))old.push_back(r);
      for(const auto& r:old) {
        const auto it=std::find(requests.begin(),requests.end(),r);
        if(it!=requests.end()) {
          // Exact retained budget, no expiry or newly sampled payload size.
          owned_input_bytes-=r->owned_input_bytes;requests.erase(it);
        }
      }
    }
    // Immutable copied packets/observations die here, outside the mutex.
  }
  void Reaper() noexcept try {
    for(;;) {
      {std::unique_lock lock(requests_mutex);
        changed.wait_for(lock,std::chrono::milliseconds(50),[&]{return reaper_stop;});
        if(reaper_stop)return;
      }
      Reap();
    }
  }catch(...){std::lock_guard lock(requests_mutex);reaper_failed=true;}
};
NativeProfileSaveOwner::NativeProfileSaveOwner(std::shared_ptr<State> state):state_(std::move(state)) {}
std::optional<std::vector<uint8_t>> NativeProfileSaveOwner::CaptureNativeHeaderBefore(
    Memory& memory,uint32_t context,std::error_code& error) {
  error.clear();
  // This exact worker admits H=971A; it does not establish arbitrary older or
  // larger context layouts. R=A000, context allocation R+192. Native control
  // bytes [9752,98AC) are NOT raw Header or alignment padding.
  constexpr uint32_t h=0x971A,r=0xA000,first=0x38,second=0x98AC;
  if(!context || (context&3) || uint64_t(context)+r+0x192>uint64_t(UINT32_MAX)+1) {
    error=Error(std::errc::invalid_argument);return {};
  }
  std::array<uint8_t,4> size;
  if(ReadOwnedPayload(memory,context+0x378,size)!=X_ERROR_SUCCESS) {
    error=Error(std::errc::bad_address);return {};
  }
  if(BigWord(size)!=h){error=Error(std::errc::operation_not_supported);return {};}
  std::vector<uint8_t> owned(r);
  if(ReadOwnedPayload(memory,context+first,std::span(owned).first(h))!=X_ERROR_SUCCESS ||
      ReadOwnedPayload(memory,context+second,std::span(owned).subspan(h))!=X_ERROR_SUCCESS) {
    error=Error(std::errc::bad_address);return {};
  }
  if(BigWord(owned)!=0x434F4E20 || BigWord(owned,0x340)!=h) {
    error=Error(std::errc::bad_message);return {};
  }
  return owned;
}
uint32_t NativeProfileSaveOwner::ReadOwnedPayload(Memory& memory,uint32_t at,std::span<uint8_t> out) {
 if(out.size()>UINT32_MAX || !util::GuestIoRangeValid(memory,at,uint32_t(out.size()),false))return X_ERROR_INVALID_PARAMETER;
 // Immediate HOST-owned RAM copy only. Each narrow read validates its actual
 // current backing; no providers/wait, translated pointer, pin or deferred
 // copy escapes. This is not an atomic snapshot across concurrent guest
 // writers. The native immutable-input contract and later exact staged-byte
 // comparison are independently required before any durable publication.
 for(size_t offset=0;offset<out.size();) {
   const auto count=std::min<size_t>(256,out.size()-offset);
   if(uint64_t(at)+offset>UINT32_MAX || !internal::ReadNativeWaitSnapshot(
       memory,uint32_t(uint64_t(at)+offset),out.subspan(offset,count)))return X_ERROR_INVALID_PARAMETER;
   offset+=count;
 }
 return util::GuestIoRangeValid(memory,at,uint32_t(out.size()),false) ? X_ERROR_SUCCESS : X_ERROR_INVALID_PARAMETER;
}
NativeProfileSaveOwner::~NativeProfileSaveOwner(){CloseNewAdmission();}
std::shared_ptr<NativeProfileSaveOwner> NativeProfileSaveOwner::Lookup(KernelState* kernel) {
  std::lock_guard lock(installed_save_mutex);
  const auto i=installed_save.find(kernel);return i==installed_save.end()?nullptr:i->second;
}
std::shared_ptr<NativeProfileSaveOwner> NativeProfileSaveOwner::CreateBeforeWorkers(Config config,std::string* error) {
  const auto fail=[&](const char* text)->std::shared_ptr<NativeProfileSaveOwner>{if(error)*error=text;return {};};
  if(!config.catalog || !config.catalog->calls() || !config.catalog->calls()->kernel() ||
     !config.domain || !config.domain->active() || !config.allocation ||
     !config.allocation->IsAccepting() || !config.allocation->MatchesDomain(config.domain))
    return fail("Missing actual profile runtime/domain/allocation lifetime");
  auto state=std::make_shared<State>(std::move(config));
  const auto module=state->config.catalog->calls()->module();
  if(!module)return fail("Missing actual profile native root");
  state->root_code=state->kernel->AcquireUserModuleExecutionLease(module);
  state->resident_domain=state->config.domain->Acquire();
  if(!state->root_code || !state->resident_domain)return fail("Missing resident native profile code lifetime");
  for(const auto& code:save_code)if(!state->CodeCurrent(code.at))
    return fail("Original profile producer/DB code is not the pinned17559 family");
  state->requests.reserve(128);
  auto owner=std::shared_ptr<NativeProfileSaveOwner>(new NativeProfileSaveOwner(state));
  {
    std::lock_guard lock(installed_save_mutex);
    const auto [it,inserted]=installed_save.try_emplace(state->kernel,owner);
    if(!inserted)return fail("Another profile SAVE owner is installed");
  }
  try {state->reaper=std::thread([state]{state->Reaper();});}
  catch(...) {
    {std::lock_guard lock(installed_save_mutex);const auto i=installed_save.find(state->kernel);
      if(i!=installed_save.end() && i->second==owner)installed_save.erase(i);}
    return fail("Profile SAVE host retirement tail could not start");
  }
  return owner;
}
std::error_code NativeProfileSaveOwner::PrepareLoadedModel(KernelState* kernel,
    const std::shared_ptr<xam::ProfileGpdModelAuthority>& model) {
  // Real model load/repair host lane only. This is not a new native wait and
  // it is deliberately NOT called by the present R loader or hook gateway.
  if(XThread::IsInThread() || global_critical_region::is_held_by_current_thread())
    return Error(std::errc::operation_would_block);
  const auto owner=Lookup(kernel);
  return owner?owner->PrepareSourcePolicies(model):Error(std::errc::no_such_device);
}
bool NativeProfileSaveOwner::Drained() const noexcept try {
  auto& s=*state_;std::unique_lock preparation(s.preparation,std::try_to_lock);
  if(!preparation.owns_lock())return false;
  std::lock_guard lock(s.requests_mutex);
  return !s.reaper_failed && s.requests.empty() &&
      !(s.observers.load(std::memory_order_acquire)&~State::kObserversClosed);
}catch(...){return false;}
bool NativeProfileSaveOwner::RetainedForActualKernelStop() const noexcept try {
  auto& s=*state_;std::unique_lock preparation(s.preparation,std::try_to_lock);
  if(!preparation.owns_lock() || !s.closed->load(std::memory_order_acquire) ||
     (s.observers.load(std::memory_order_acquire)&~State::kObserversClosed))return false;
  std::lock_guard lock(s.requests_mutex);
  // This permits OLD records to stay owned THROUGH factual CPU stop, not a
  // healthy DB/request drainage result. No queued/running host IO is abandoned.
  return std::all_of(s.requests.begin(),s.requests.end(),[](const auto& r){return !r->host_io_duties;});
}catch(...){return false;}
bool NativeProfileSaveOwner::RetireAfterActualKernelStop() noexcept try {
  if(XThread::IsInThread() || global_critical_region::is_held_by_current_thread())return false;
  const auto keep_alive=shared_from_this();auto& s=*state_;
  std::lock_guard retirement(s.retirement_mutex);
  if(s.retired.load(std::memory_order_acquire))return true;
  if(!s.closed->load(std::memory_order_acquire) || !s.kernel)return false;
  // Called ONLY at the service's already factual whole CPU/IO/GPU-stop cut.
  // This additional actual-thread check never substitutes for that contract.
  for(const auto& thread:s.kernel->object_table()->GetObjectsByType<XThread>(XObject::Type::Thread))
    if(thread->is_running())return false;
  std::unique_lock preparation(s.preparation,std::try_to_lock);if(!preparation.owns_lock())return false;
  const auto observed=s.observers.fetch_or(State::kObserversClosed,std::memory_order_acq_rel);
  if(observed&~State::kObserversClosed)return false;
  {std::lock_guard lock(s.requests_mutex);
    if(std::any_of(s.requests.begin(),s.requests.end(),[](const auto& r){return r->host_io_duties!=0;}))return false;
    s.reaper_stop=true;
  }
  s.changed.notify_all();if(s.reaper.joinable())s.reaper.join();
  std::vector<std::shared_ptr<State::Request>> old;
  std::vector<std::shared_ptr<NativeProfileSaveManagedGpd>> gpd;
  std::vector<std::shared_ptr<NativeProfileSaveManagedPicture>> pictures;
  {std::lock_guard lock(s.requests_mutex);s.retired.store(true,std::memory_order_release);
    s.semantic_uses.Unavailable(); // Factual stopped owner cannot leave old scalar uses live.
    old.swap(s.requests);s.owned_input_bytes=0;}
  gpd.swap(s.gpd);pictures.swap(s.pictures);preparation.unlock();
  {std::lock_guard lock(installed_save_mutex);const auto i=installed_save.find(s.kernel);
    if(i!=installed_save.end() && i->second==keep_alive)installed_save.erase(i);}
  // Uncertain/dirty/partial-journal duties are discarded ONLY with their
  // already-stopped Kernel. No completion/cancel/rollback/flush is reported.
  for(const auto& request:old) {
    NativeXmsgProvenance::Release(request->provenance);
    if(request->header_child)NativeXmsgProvenance::Release(request->header_child->provenance);
    if(request->close_child)NativeXmsgProvenance::Release(request->close_child->provenance);
    for(const auto& key:request->parent_keys)if(key && key->main_flush)
      NativeXmsgProvenance::Release(key->main_flush->provenance);
  }
  old.clear();gpd.clear();pictures.clear();
  s.root_code.reset();s.resident_domain={};s.config={};s.kernel=nullptr;return true;
}catch(...){return false;}
std::error_code NativeProfileSaveOwner::PrepareSourcePolicies(
    const std::shared_ptr<xam::ProfileGpdModelAuthority>& model) {
  if(global_critical_region::is_held_by_current_thread() || !model) {return Error(std::errc::operation_would_block);}
  auto& s=*state_;std::lock_guard serial(s.preparation);
  const auto calls=s.config.catalog?s.config.catalog->calls():nullptr;
  if(s.closed->load(std::memory_order_acquire) || !calls || !s.config.domain || !s.config.domain->active() ||
      !s.config.allocation || !s.config.allocation->IsAccepting()) {return Error(std::errc::operation_canceled);}
  std::error_code error;const auto read=model->CaptureRead(error);if(!read) {return error;}
  const auto loaded=model->CaptureLoads(error);if(!loaded) {return error;}
  auto* fs=calls->kernel()->file_system();if(!fs) {return Error(std::errc::no_such_device);}
  // Reserve HOST retention BEFORE registration. A partial preparation is a
  // closed/fenced prerequisite, never permission to bypass an earlier leaf.
  // No live policy is replaced merely because a new model uses the same XUID.
  if(loaded->size()>4096 || s.gpd.size()>4096-loaded->size() || s.pictures.size()>4096-6) {return Error(std::errc::no_buffer_space);}
  s.gpd.reserve(s.gpd.size()+loaded->size());s.pictures.reserve(s.pictures.size()+6);
  for(const auto& [title,file]:*loaded) {
    if(!file.readable()) {continue;} // Real native new-title/default creation is a distinct protocol.
    bool already=false;
    for(const auto& existing:s.gpd) {
      if(existing->title_==title && existing->source_->Matches(*model->storage())) {
        if((error=existing->JoinCommittedReadModel(model)))return error;
        already=true;break;
      }
    }
    if(already) {continue;}
    auto policy=NativeProfileSaveManagedGpd::CreateSource(model,title,error,s.closed);if(!policy) {return error;}
    s.gpd.push_back(policy);
    if(!fs->RegisterManagedFile(policy)) {policy->source_->RetireAdmission();return Error(std::errc::device_or_resource_busy);}
  }
  using Kind=filesystem::ProfileArtifactKind;
  constexpr Kind kinds[]={Kind::kPublic64,Kind::kPublic32,Kind::kPersonal64,Kind::kPersonal32,Kind::kAvatar64,Kind::kAvatar32};
  for(const auto kind:kinds) {
    bool already=false;
    for(const auto& existing:s.pictures) {
      if(existing->kind_==kind && existing->source_->Matches(*model->storage())) {
        if((error=existing->JoinCommittedReadModel(model)))return error;
        already=true;break;
      }
    }
    if(already) {continue;}
    auto policy=NativeProfileSaveManagedPicture::CreateSource(model,kind,error,s.closed);if(!policy) {return error;}
    s.pictures.push_back(policy);
    if(!fs->RegisterManagedFile(policy)) {policy->source_->RetireAdmission();return Error(std::errc::device_or_resource_busy);}
  }
  if(!model->IsCurrent(read->origin)) {return Error(std::errc::operation_canceled);}
  return {};
}
void NativeProfileSaveOwner::CloseNewAdmission() noexcept {
  // One scalar cut also closes every previously issued policy capability,
  // including an open queued before a concurrent source preparation finished.
  // No registry/IO/preparation mutex or native memory is borrowed here.
  state_->closed->store(true,std::memory_order_release);
  // Real request/DB/IO owners are joined independently. This is not native
  // cancel, File close, GPD clean, XMsg completion or Kernel-stop proof.
}
void NativeProfileSaveOwner::ObserveOriginalProducer(void* raw,uint32_t instruction) noexcept {
  // This is the actual pre-BL in the ORIGINAL public wrapper, not a host HLE
  // export or a guessed caller. It is deliberately unwired until the complete
  // family owner can also join source/DB/IO and exact rundown duties.
  if(!raw || global_critical_region::is_held_by_current_thread())return;
  auto& s=*state_;State::Observation observation(s);if(!observation)return;std::shared_ptr<State::Request> request;
  std::optional<NativeXmsgProvenance::Capture> capture;
  try {
    const auto site=std::find_if(std::begin(producer_sites),std::end(producer_sites),
        [&](const auto& value){return value.instruction==instruction;});
    if(site==std::end(producer_sites) || s.closed->load(std::memory_order_acquire) ||
       !s.config.allocation || !s.config.allocation->IsAccepting() || !s.CodeCurrent(site->function))return;
    auto& c=*static_cast<cpu::ppc::PPCContext*>(raw);auto* kernel=s.config.catalog->calls()->kernel();
    const auto frame=ActualSaveFrame(*kernel,c,site->function);if(!frame)return;
    auto& memory=*kernel->memory();
    if(ReadWord(memory,instruction)!=std::optional(site->word) ||
       uint64_t(frame->sp)+site->frame_bytes>UINT32_MAX || frame->lr<4)return;
    const uint32_t entry_sp=frame->sp+site->frame_bytes;
    // All four pinned wrappers save LR/backchain before this first call. The
    // descriptor is their actual stack+50; no synthetic wrapper or Task is
    // introduced for direct21F→2F80→Start.
    if(ReadWord(memory,frame->sp)!=std::optional(entry_sp) ||
       ReadWord(memory,entry_sp-8)!=std::optional(frame->lr) ||
       Dword(c.r[3])!=std::optional(0xFBu) || Dword(c.r[4])!=std::optional(site->message))return;
    const bool direct=site->wrapper==0x816A2F80;
    const auto argument=Dword(c.r[direct?6:5]),bytes=Dword(c.r[direct?7:6]),overlap=Dword(c.r[direct?5:7]);
    if(!argument || *argument!=frame->sp+0x50 || !bytes || *bytes!=site->descriptor_bytes || !overlap)return;
    std::vector<uint8_t> descriptor(site->descriptor_bytes);
    if(!internal::ReadNativeWaitSnapshot(memory,*argument,descriptor))return;
    const auto reader=[&](uint32_t at,std::span<uint8_t> out) {
      return ReadOwnedPayload(memory,at,out);
    };
    std::error_code error;
    auto input=NativeProfileSaveRequest::CaptureOriginalFb(site->message,descriptor,reader,error);
    if(!input || error)return;
    const auto caller=kernel->GetUserModuleForAddress(frame->lr-4);
    const auto module=kernel->GetUserModuleForAddress(site->function);
    if(!caller || caller->module_format()!=UserModule::kModuleFormatXex || !module ||
       module.get()!=s.config.catalog->calls()->module().get())return;
    request=std::make_shared<State::Request>();
    request->site=*site;request->producer=*frame;request->public_frame=*frame;
    request->public_frame.sp=entry_sp;request->caller=retain_object(frame->thread);
    request->caller_code=kernel->AcquireUserModuleExecutionLease(caller);
    request->domain=s.config.domain->Acquire();
    if(!request->caller_code || !request->domain)return;
    request->descriptor=*argument;request->overlap=*overlap;request->input=std::move(input);
    const uint64_t owned=descriptor.size()+request->input->large().bytes.size()+request->input->small().bytes.size();
    request->owned_input_bytes=owned;
    {
      std::lock_guard lock(s.requests_mutex);
      // These are HOST observation bounds, not new guest payload limits or
      // success. Refusal issues no source/view/writer authority.
      constexpr uint64_t budget=128ull<<20;
      if(s.closed->load(std::memory_order_acquire) || s.requests.size()>=128 ||
         s.invocation==UINT64_MAX || owned>budget || s.owned_input_bytes>budget-owned)return;
      request->invocation=s.invocation+1;
      s.requests.push_back(request); // fallible retention precedes counters/Begin
      s.invocation=request->invocation;s.owned_input_bytes+=owned;
    }
    NativeXmsgProvenance::ProducerFields fields;
    fields.root={s.config.allocation->owner().kernel_epoch,s.config.domain->id(),s.config.domain->root().generation};
    fields.public_invocation.invocation=request->invocation;
    fields.public_invocation.module={module->hmodule_ptr(),module->load_generation()};
    fields.public_invocation.caller_module={caller->hmodule_ptr(),caller->load_generation()};
    fields.public_invocation.function=site->function;fields.public_invocation.instruction=frame->lr-4;
    fields.public_invocation.frame=request->public_frame.Fact();
    fields.producer=frame->Fact();fields.producer_instruction=instruction;fields.wrapper=site->wrapper;
    fields.form=direct?NativeXmsgProvenance::ProducerForm::kDirectStart:
                       NativeXmsgProvenance::ProducerForm::kRequestWrapper;
    fields.app=0xFB;fields.message=site->message;fields.argument=*argument;
    fields.argument_bytes=*bytes;fields.overlap=*overlap;fields.copied_bytes_budget=*bytes;
    capture=NativeXmsgProvenance::Begin(&c,NativeXmsgProvenance::AuthenticatedProducer(fields));
    bool discard=false;
    {
      std::lock_guard lock(s.requests_mutex);
      if(request->uncertain || request->public_returned || request->provenance)discard=true;
      else if(!capture)request->uncertain=true;
      else {request->public_fact=fields.public_invocation;request->provenance=*capture;}
    }
    if(discard && capture)NativeXmsgProvenance::Release(*capture);
  } catch(...) {
    if(request){std::lock_guard lock(s.requests_mutex);request->uncertain=true;}
    // No guest side effect, callback, success or exception replacement. The
    // actual retained owner must settle this host observation at real rundown.
    if(capture)NativeXmsgProvenance::Release(*capture);
  }
}
// Included as authored methods in the existing NativeProfileSaveOwner TU.
void NativeProfileSaveOwner::ObserveOriginalPictureFileEntry(void* raw) noexcept {
  if(!raw || global_critical_region::is_held_by_current_thread())return;
  auto& s=*state_;State::Observation observation(s);if(!observation)return;
  std::shared_ptr<State::Request> r;
  try {
    if(!s.CodeCurrent(0x8191C4D8) || !s.CodeCurrent(0x8191B8F8))return;
    auto& c=*static_cast<cpu::ppc::PPCContext*>(raw);r=s.CurrentInner(c);
    if(!r || !r->input || r->site.message!=0xB0037)return;
    const auto frame=ActualSaveFrame(*s.kernel,c,0x8191B8F8);
    if(!frame || (frame->lr!=0x8191C824 && frame->lr!=0x8191C88C))return;
    const bool small=frame->lr==0x8191C88C;
    const auto& payload=small?r->input->small():r->input->large();
    const auto name=Dword(c.r[3]),buffer=Dword(c.r[4]),length=Dword(c.r[5]);
    if(!name || !*name || !buffer || !length || !*buffer || !*length ||
        *buffer!=payload.address || *length!=payload.declared_bytes ||
        Dword(c.r[6])!=std::optional(0u))return;
    SaveFrame body;std::shared_ptr<xam::ProfileRosterNativeWriter> source;
    std::shared_ptr<save_detail::SemanticUses::Span> semantic;
    {
      std::lock_guard lock(s.requests_mutex);
      if(r->uncertain || !r->body_entered || r->body_returned || !r->getter_returned ||
          r->release_entered || !r->selected_source || r->source_error ||
          !r->semantic_use || !r->semantic_use->unchanged() ||
          (r->picture_call && !r->picture_call->returned) || s.closed->load())return;
      body=r->body;source=r->selected_source;semantic=r->semantic_use;
    }
    auto& memory=*s.kernel->memory();
    if(body.function!=0x8191DDE8 || body.sp<0x940 || frame->sp!=body.sp-0x940 ||
        ReadWord(memory,frame->sp)!=std::optional(body.sp-0x200) ||
        ReadWord(memory,body.sp-0x200-8)!=std::optional(0x8191E060u) ||
        ReadWord(memory,frame->sp+0x754)!=std::optional(r->native_profile) ||
        ReadWord(memory,frame->sp+0x75C)!=std::optional(r->input->flags()) ||
        ReadWord(memory,frame->sp+0x794)!=std::optional(0u))return;
    const auto common=Dword(c.r[19]),mode=Dword(c.r[25]);
    if(!common || *common>1 || !mode || *mode!=(r->input->flags()&0xF) ||
        (!*common && *mode!=1 && *mode!=2))return;
    using Kind=filesystem::ProfileArtifactKind;
    const auto kind=*common?(small?Kind::kAvatar32:Kind::kAvatar64):
        *mode==1?(small?Kind::kPublic32:Kind::kPublic64):(small?Kind::kPersonal32:Kind::kPersonal64);
    std::vector<uint8_t> actual(*length);
    if(ReadOwnedPayload(memory,*buffer,actual)!=X_ERROR_SUCCESS || actual!=payload.bytes)return;
    std::shared_ptr<NativeProfileSavePublication> publication;
    {std::lock_guard lock(s.requests_mutex);publication=r->publication;}
    if(!publication) {
      NativeProfileSavePublication::SystemFileIssuer issuer;
      issuer.source=source;issuer.actual_semantic_use=semantic;issuer.domain=s.config.domain->id();
      issuer.root_generation=s.config.domain->root().generation;issuer.invocation=r->invocation;
      issuer.title=0xFFFE07D1;issuer.defer_until_whole_checkpoint=true;
      issuer.gpd_mode=(r->input->flags()&0x80) || !(r->input->flags()&0x30)
          ? NativeProfileSavePublication::SystemGpdMode::kNativeUnchangedNoWrite
          : NativeProfileSavePublication::SystemGpdMode::kNativeWriteRequired;
      issuer.required_artifacts={Kind::kPackageHeader};
      const auto pair=[&](Kind large,Kind little){issuer.required_artifacts.push_back(large);issuer.required_artifacts.push_back(little);};
      if(*common) {
        pair(Kind::kAvatar64,Kind::kAvatar32);
        if(r->input->flags()&0x10)pair(Kind::kPublic64,Kind::kPublic32);
        if(r->input->flags()&0x20)pair(Kind::kPersonal64,Kind::kPersonal32);
      }else if(*mode==1)pair(Kind::kPublic64,Kind::kPublic32);
      else pair(Kind::kPersonal64,Kind::kPersonal32);
      std::error_code error;
      publication=NativeProfileSavePublication::PrepareSystemParentOperation(std::move(issuer),error);
      if(!publication){std::lock_guard lock(s.requests_mutex);r->source_error=error;return;}
    }
    std::lock_guard lock(s.requests_mutex);
    if(r->uncertain || r->release_entered || !semantic->unchanged() ||
        (r->picture_call && !r->picture_call->returned))return;
    if(r->publication && r->publication!=publication){r->uncertain=true;return;}
    r->publication=std::move(publication);r->parent_picture_sp=frame->sp;
    r->picture_call=State::Request::PictureFileCall{*frame,kind,*name,*buffer,*length};
    // No filesystem effect or wait occurred here. The real first CreateFile
    // carries this producer into IO22; its owned call admits ALL parent roles
    // plus one Header/PIS05 before even a PNG stage exists.
  }catch(...){if(r){std::lock_guard lock(s.requests_mutex);r->uncertain=true;}}
}
void NativeProfileSaveOwner::ObserveOriginalPictureFileReturn(void* raw) noexcept {
  if(!raw || global_critical_region::is_held_by_current_thread())return;
  auto& s=*state_;State::Observation observation(s);if(!observation)return;
  try {
    if(!s.CodeCurrent(0x8191B8F8))return;
    auto& c=*static_cast<cpu::ppc::PPCContext*>(raw);const auto r=s.CurrentInner(c);
    const auto frame=ActualSaveFrame(*s.kernel,c,0x8191B8F8);const auto result=Dword(c.r[3]);
    if(!r || !frame || !result)return;
    std::lock_guard lock(s.requests_mutex);
    if(!r->picture_call || r->picture_call->returned || r->picture_call->entry!=*frame)return;
    r->picture_call->result=*result;r->picture_call->returned=true;
    // Historical native outcome only: the native helper checks BOOL, not its
    // transferred-count DWORD. Final parent publication still owes actual IO.
  }catch(...){/* No invented file return/count or native completion. */}
}
std::optional<std::vector<uint8_t>> NativeProfileSaveOwner::CaptureNativeGpdIndex(
    Memory& memory,uint32_t stream,uint32_t handle,std::error_code& error) {
  error.clear();
  if(global_critical_region::is_held_by_current_thread()) {error=Error(std::errc::operation_would_block);return {};}
  if(!stream || !handle || handle==UINT32_MAX ||
      uint64_t(stream)+0x10>uint64_t(UINT32_MAX)+1) {error=Error(std::errc::invalid_argument);return {};}
  std::array<uint8_t,16> control;
  if(!internal::ReadNativeWaitSnapshot(memory,stream,control) || BigWord(control)!=0x81630230 ||
      BigWord(control,12)!=handle || !BigWord(control,4) || BigWord(control,8)<24 ||
      BigWord(control,8)>xam::GpdSnapshotStore::kMaximumBytes) {error=Error(std::errc::bad_message);return {};}
  std::vector<uint8_t> bytes(BigWord(control,8));
  if(ReadOwnedPayload(memory,BigWord(control,4),bytes)!=X_ERROR_SUCCESS) {error=Error(std::errc::bad_address);return {};}
  const uint64_t declared=24+uint64_t(XdbfTableView::U32(bytes.data()+8))*18+
      uint64_t(XdbfTableView::U32(bytes.data()+16))*8;
  if(declared!=bytes.size()) {error=Error(std::errc::bad_message);return {};}
  return bytes;
}
void NativeProfileSaveOwner::ObserveOriginalParentKeyEntry(void* raw) noexcept {
  if(!raw || global_critical_region::is_held_by_current_thread())return;
  auto& s=*state_;State::Observation observation(s);if(!observation)return;
  std::shared_ptr<State::Request> r;
  try {
    if(!s.CodeCurrent(0x8191F898) || !s.CodeCurrent(0x8191C4D8))return;
    auto& c=*static_cast<cpu::ppc::PPCContext*>(raw);r=s.CurrentInner(c);
    const auto frame=ActualSaveFrame(*s.kernel,c,0x8191F898);
    if(!r || !r->input || r->site.message!=0xB0037 || !frame ||
        (frame->lr!=0x8191CB7C && frame->lr!=0x8191CC9C))return;
    using Key=State::Request::ParentKeyUse;const bool personal=frame->lr==0x8191CC9C;
    const auto flags=r->input->flags();const auto parent=r->parent_picture_sp;
    if((flags&0x80) || !(flags&(personal?0x20u:0x10u)) || !parent ||
        uint64_t(parent)+0x798>uint64_t(UINT32_MAX)+1 || frame->sp!=parent ||
        Dword(c.r[3])!=std::optional(r->native_profile) || Dword(c.r[4])!=std::optional(personal?0u:1u))return;
    const auto object=ReadWord(*s.kernel->memory(),parent+0x764);
    if(!object || !*object || uint64_t(*object)+0x10>uint64_t(UINT32_MAX)+1 ||
        Dword(c.r[5])!=object)return;
    const auto bytes=ReadWord(*s.kernel->memory(),*object+8),value=ReadWord(*s.kernel->memory(),*object+12);
    if(!bytes || !value || (*bytes && !*value))return;
    auto key=std::make_unique<Key>();key->kind=personal?Key::Kind::kPersonal:Key::Kind::kPublic;
    key->helper=*frame;key->key_object=*object;key->value_address=*value;key->value_bytes=*bytes;
    key->database.normalized_title=0xFFFE07D1;
    std::lock_guard held(s.requests_mutex);
    if(r->uncertain || r->source_error || !r->publication || !r->publication->parent_operation_ ||
        !r->getter_returned || r->release_entered || !r->semantic_use || !r->semantic_use->unchanged() ||
        (r->active_parent_key && !r->active_parent_key->helper_returned))return;
    for(const auto& old:r->parent_keys) {
      if(!old || old->kind==key->kind || !old->helper_returned || int32_t(old->result)<0 ||
          !State::GpdUseEnded(old->database))return;
    }
    if(personal && (flags&0x10) && (r->parent_keys.size()!=1 ||
        r->parent_keys.front()->kind!=Key::Kind::kPublic))return;
    if(!personal && !r->parent_keys.empty())return;
    auto* actual=key.get();r->parent_keys.push_back(std::move(key));
    r->active_parent_key=actual;r->gpd=&actual->database;r->whole_gpd_cache.reset();
    // No disk/canonical/PIS effect. Next genuine12580/GetDB/CS/Ensure must
    // establish its own actual native use, including second-key staged reuse.
  }catch(...){if(r){std::lock_guard held(s.requests_mutex);r->uncertain=true;}}
}
void NativeProfileSaveOwner::ObserveOriginalParentKeySetterEntry(void* raw) noexcept {
  if(!raw || global_critical_region::is_held_by_current_thread())return;
  auto& s=*state_;State::Observation observation(s);if(!observation)return;
  std::shared_ptr<State::Request> r;
  try {
    if(!s.CodeCurrent(0x81912580) || !s.CodeCurrent(0x8191F898))return;
    auto& c=*static_cast<cpu::ppc::PPCContext*>(raw);r=s.CurrentInner(c);if(!r || r->site.message!=0xB0037)return;
    const auto frame=ActualSaveFrame(*s.kernel,c,0x81912580);
    State::Request::ParentKeyUse* key=nullptr;
    {std::lock_guard held(s.requests_mutex);key=r->active_parent_key;}
    // Request owns this stable capsule through the actual current native use.
    // Read native control operands outside the host family metadata lock.
    if(!key || r->gpd!=&key->database || key->helper_returned || key->setter_entered ||
        !frame || key->helper.sp<0x80 || frame->sp!=key->helper.sp-0x80 || frame->lr!=0x8191F8F0 ||
        frame->id!=key->helper.id || frame->pcr!=key->helper.pcr ||
        Dword(c.r[3])!=std::optional(r->native_profile) || Dword(c.r[4])!=std::optional(0xFFFE07D1u) ||
        Dword(c.r[5])!=std::optional(1u) || Dword(c.r[6])!=std::optional(frame->sp+0x50) ||
        Dword(c.r[7])!=std::optional(0u))return;
    const auto id=key->kind==State::Request::ParentKeyUse::Kind::kPublic?0x4064000Fu:0x40640010u;
    const auto descriptor=frame->sp+0x50;
    if(ReadWord(*s.kernel->memory(),descriptor)!=std::optional(1u) ||
        ReadWord(*s.kernel->memory(),descriptor+0x10)!=std::optional(id) ||
        ReadWord(*s.kernel->memory(),descriptor+0x20)!=std::optional(key->value_bytes) ||
        ReadWord(*s.kernel->memory(),descriptor+0x24)!=std::optional(key->value_address))return;
    std::lock_guard held(s.requests_mutex);
    if(r->uncertain || r->active_parent_key!=key || key->helper_returned || key->setter_entered)return;
    key->setter=*frame;key->setting_descriptor=descriptor;key->setter_entered=true;
    key->database.image_leaf=*frame;key->database.image_leaf_entered=true;
    // Flags/source/thread early-return paths remain real12580 behavior. No
    // absent GetDB/setting hook may stand in for a successful native no-write.
  }catch(...){if(r){std::lock_guard held(s.requests_mutex);r->uncertain=true;}}
}
void NativeProfileSaveOwner::ObserveOriginalParentKeySettingEntry(void* raw) noexcept {
  if(!raw || global_critical_region::is_held_by_current_thread())return;
  auto& s=*state_;State::Observation observation(s);if(!observation)return;
  std::shared_ptr<State::Request> r;
  try {
    if(!s.CodeCurrent(0x81916158) || !s.CodeCurrent(0x81912580))return;
    auto& c=*static_cast<cpu::ppc::PPCContext*>(raw);r=s.CurrentInner(c);if(!r || r->site.message!=0xB0037)return;
    const auto frame=ActualSaveFrame(*s.kernel,c,0x81916158);
    State::Request::ParentKeyUse* key=nullptr;uint32_t db=0,handle=0,cs=0;
    std::shared_ptr<NativeProfileSavePublication> publication;
    std::shared_ptr<NativeProfileGpdReadView> view;
    std::shared_ptr<const NativeProfileGpdCheckpoint> reused;
    std::vector<uint8_t> before;std::optional<CriticalSnapshot> acquired;
    {
      std::lock_guard held(s.requests_mutex);key=r->active_parent_key;
      if(!key || !frame || r->gpd!=&key->database || !key->setter_entered || key->setter_returned ||
          key->setting_entered || key->helper_returned || !r->publication || r->source_error || r->uncertain ||
          !r->gpd->image_ensure_returned || int32_t(r->gpd->image_ensure_result)<0 ||
          !r->gpd->image_lock_retained || !r->gpd->native_before_header_valid || r->gpd->image_release_entered ||
          r->release_entered || !r->semantic_use || !r->semantic_use->unchanged())return;
      db=r->gpd->image_db_pointer;handle=r->gpd->opened_handle;cs=r->gpd->image_owned_cs;
      acquired=r->gpd->image_try_after;publication=r->publication;view=r->gpd->opened_view;
      reused=r->gpd->reused_checkpoint;before=r->gpd->native_before_header_index;
    }
    if(!acquired || !view || before.empty() || !handle || handle==UINT32_MAX || key->setter.sp<0xB0 ||
        frame->sp!=key->setter.sp-0xB0 || frame->lr!=0x81912678 || frame->id!=key->setter.id ||
        frame->pcr!=key->setter.pcr || Dword(c.r[3])!=std::optional(db) ||
        Dword(c.r[4])!=std::optional(key->setting_descriptor) || Dword(c.r[5])!=std::optional(1u) ||
        Dword(c.r[6])!=std::optional(1u))return;
    const auto held=ReadCriticalSnapshot(*s.kernel->memory(),cs);
    if(!held || held->owner_kthread!=frame->thread->guest_object() || held->recursion!=acquired->recursion || held->recursion<=0)return;
    const auto file=s.kernel->object_table()->LookupObject<XFile>(handle);if(!file || !file->file())return;
    std::shared_ptr<NativeProfileSaveManagedGpd> service;
    {
      std::unique_lock prepare(s.preparation,std::try_to_lock);if(!prepare.owns_lock())return;
      for(const auto& candidate:s.gpd)if(candidate->title_==0xFFFE07D1 &&
          candidate->source_->MatchesMetadata(*publication->WriterForStage().model->storage())) {
        if(service)return;service=candidate;
      }
    }
    if(!service)return;
    std::error_code error;X_STATUS status=X_STATUS_ACCESS_DENIED;
    if(reused) {
      // This second actual native use reuses the SAME already-bound stage.
      // Matching latest bytes/index/version is not a new before binding.
      const auto current=NativeProfileSaveManagedGpd::CaptureStagedFileCheckpointAtActualDbUse(
          *file->file(),publication,before,error);
      if(current && current==reused && NativeProfileSaveManagedGpd::MatchesOwnedPublication(*file->file(),publication))
        status=X_STATUS_SUCCESS;
    }else {
      error=publication->BindSystemGpdInputAtOriginalEdit(view,file->file()->retained_device());
      if(!error) {
        auto cut=std::make_shared<ManagedFileCut>();cut->kind=ManagedFileCut::Kind::kBindSystem;
        cut->file=file;cut->service=service;cut->expected=view;cut->publication=publication;
        cut->actual_native_header_index=before;status=PrepareManagedFileCut(cut);
      }
    }
    std::lock_guard lock(s.requests_mutex);
    if(r->uncertain || r->active_parent_key!=key || key->setting_entered || key->setter_returned ||
        !r->gpd->image_lock_retained || r->release_entered)return;
    key->setting=*frame;key->setting_entered=true;
    r->gpd->image_bind_attempted=true;r->gpd->image_bind_status=status;r->gpd->write_service=service;
    r->gpd->cached_before_file=file;
    if(error || XFAILED(status))r->source_error=error?error:Error(std::errc::operation_not_permitted);
    // No stage creation, disk IO or native wait. The independently issued
    // bound SYSTEM File adopts its stage on its ORIGINAL subsequent byte IO.
  }catch(...){if(r){std::lock_guard lock(s.requests_mutex);r->uncertain=true;}}
}
void NativeProfileSaveOwner::ObserveOriginalParentKeySettingReturn(void* raw) noexcept {
  if(!raw || global_critical_region::is_held_by_current_thread())return;
  auto& s=*state_;State::Observation observation(s);if(!observation)return;
  std::shared_ptr<State::Request> r;
  try {
    if(!s.CodeCurrent(0x81916158) || !s.CodeCurrent(0x81912580))return;
    auto& c=*static_cast<cpu::ppc::PPCContext*>(raw);r=s.CurrentInner(c);if(!r || r->site.message!=0xB0037)return;
    const auto frame=ActualSaveFrame(*s.kernel,c,0x81916158);const auto result=Dword(c.r[3]);
    State::Request::ParentKeyUse* key=nullptr;uint32_t stream=0,handle=0,cs=0;
    object_ref<XFile> file;std::shared_ptr<NativeProfileSavePublication> publication;
    std::optional<CriticalSnapshot> acquired;
    {
      std::lock_guard lock(s.requests_mutex);key=r->active_parent_key;
      if(!key || !frame || !result || !key->setting_entered || key->setting_returned || key->setting!=*frame ||
          !r->gpd->image_lock_retained || r->gpd->image_release_entered || r->release_entered)return;
      stream=r->gpd->native_stream;handle=r->gpd->opened_handle;cs=r->gpd->image_owned_cs;
      acquired=r->gpd->image_try_after;file=r->gpd->cached_before_file;publication=r->publication;
    }
    std::error_code error;std::shared_ptr<const NativeProfileGpdCheckpoint> completed;
    if(int32_t(*result)>=0) {
      const auto held=ReadCriticalSnapshot(*s.kernel->memory(),cs);
      if(!held || !acquired || held->owner_kthread!=frame->thread->guest_object() ||
          held->recursion!=acquired->recursion || held->recursion<=0 || !file || !file->file() ||
          ReadWord(*s.kernel->memory(),stream+12)!=std::optional(handle))error=Error(std::errc::operation_canceled);
      else {
        auto native=CaptureNativeGpdIndex(*s.kernel->memory(),stream,handle,error);
        if(native)completed=NativeProfileSaveManagedGpd::CaptureStagedFileCheckpointAtActualDbUse(
            *file->file(),publication,*native,error);
      }
      // This captures only an ALREADY completed full-byte epoch produced on
      // the real final stream IO carrier; no full-file read occurs under CS.
    }
    std::lock_guard lock(s.requests_mutex);
    if(r->active_parent_key!=key || key->setting_returned || r->gpd!=&key->database)return;
    key->setting_result=*result;key->setting_returned=true;
    if(error)r->source_error=error;
    if(!r->uncertain && !r->source_error && int32_t(*result)>=0 &&
        key->edit_kinds[1] && r->gpd->image_edit_returned && !r->gpd->native_first_failure && completed)
      key->completed_checkpoint=std::move(completed);
  }catch(...){if(r){std::lock_guard lock(s.requests_mutex);r->uncertain=true;}}
}
void NativeProfileSaveOwner::State::ParentGpdEditEntry(void* raw,uint32_t function,const std::shared_ptr<Request>& r) noexcept {
  if(function!=0x81925AC0 || !raw || global_critical_region::is_held_by_current_thread())return;
  auto& s=*this;
  try {
    if(!r || r->site.message!=0xB0037 || !s.CodeCurrent(0x81916158))return;
    auto& c=*static_cast<cpu::ppc::PPCContext*>(raw);
    const auto frame=ActualSaveFrame(*s.kernel,c,function);State::Request::ParentKeyUse* key=nullptr;
    uint32_t db=0,cs=0;std::optional<CriticalSnapshot> acquired;
    {
      std::lock_guard held(s.requests_mutex);key=r->active_parent_key;
      if(!key || !frame || !key->setting_entered || key->setting_returned || key->setter_returned ||
          !r->publication || r->uncertain || r->source_error || !r->gpd->image_bind_attempted ||
          XFAILED(r->gpd->image_bind_status) || !r->gpd->write_service || !r->gpd->image_lock_retained ||
          r->gpd->image_release_entered || r->release_entered || !r->semantic_use || !r->semantic_use->unchanged())return;
      db=r->gpd->image_db_pointer;cs=r->gpd->image_owned_cs;acquired=r->gpd->image_try_after;
    }
    if(key->setting.sp<0x4F0 || !acquired || frame->id!=key->setting.id || frame->pcr!=key->setting.pcr ||
        Dword(c.r[3])!=std::optional(db) || Dword(c.r[7])!=std::optional(0u) || Dword(c.r[8])!=std::optional(0u))return;
    const uint32_t body=key->setting.sp-0x4F0;
    uint32_t kind=UINT32_MAX,entry=0,buffer=0,length=0;uint64_t expected_key=0;
    if(frame->lr==0x81928738 && body>=0xB0 && frame->sp==body-0xB0 && s.CodeCurrent(0x819286B8) &&
        ReadWord(*s.kernel->memory(),frame->sp)==std::optional(body) &&
        ReadWord(*s.kernel->memory(),body-8)==std::optional(0x8191620Cu)) {
      kind=0;entry=frame->sp+0x58;buffer=frame->sp+0x70;length=24;expected_key=0x200000000ull;
    }else if(frame->lr==0x81916258 && frame->sp==body &&
        ReadWord(*s.kernel->memory(),body)==std::optional(key->setting.sp) &&
        ReadWord(*s.kernel->memory(),key->setting.sp-8)==std::optional(0x81912678u)) {
      kind=1;entry=body+0x70;buffer=body+0xB0;
      const auto actual=ReadWord(*s.kernel->memory(),body+0x4B0);if(!actual)return;
      const uint32_t payload=std::max(2u,std::min(key->value_bytes&~1u,100u));
      if(*actual!=24+payload)return;length=*actual;
      expected_key=key->kind==State::Request::ParentKeyUse::Kind::kPublic?0x4064000Full:0x40640010ull;
    }else if(frame->lr==0x81925F38 && body>=0x70 && frame->sp==body-0x70 && s.CodeCurrent(0x81925EF8) &&
        ReadWord(*s.kernel->memory(),frame->sp)==std::optional(body) &&
        ReadWord(*s.kernel->memory(),body-8)==std::optional(0x8191630Cu)) {
      kind=2;entry=frame->sp+0x50;expected_key=0x100000000ull;
      const auto count=ReadWord(*s.kernel->memory(),body+0x84),data=ReadWord(*s.kernel->memory(),body+0x88);
      if(!count || !data || *count>xam::GpdSnapshotStore::kMaximumBytes/16)return;
      length=*count*16;buffer=*data;if(length && !buffer)return;
    }else return;
    if(Dword(c.r[4])!=std::optional(entry) || Dword(c.r[5])!=std::optional(buffer) ||
        Dword(c.r[6])!=std::optional(length))return;
    std::array<uint8_t,10> native_entry;
    if(!internal::ReadNativeWaitSnapshot(*s.kernel->memory(),entry,native_entry) ||
        native_entry[0] || native_entry[1]!=3)return;
    uint64_t actual_key=0;for(size_t n=2;n<native_entry.size();++n)actual_key=actual_key<<8|native_entry[n];
    if(actual_key!=expected_key)return;
    const auto held=ReadCriticalSnapshot(*s.kernel->memory(),cs);
    if(!held || held->owner_kthread!=frame->thread->guest_object() || held->recursion!=acquired->recursion || held->recursion<=0)return;
    std::lock_guard lock(s.requests_mutex);
    if(r->active_parent_key!=key || r->gpd!=&key->database || key->setting_returned || key->edit_kinds[kind] ||
        (r->gpd->image_edit_entered && !r->gpd->image_edit_returned) ||
        (r->gpd->image_insert_entered && !r->gpd->image_insert_returned) ||
        (r->gpd->stream_write_entered && !r->gpd->stream_write_returned))return;
    // Each one of the THREE real non-looping16158 suboperations has its own
    // original25AC0 scope: increment SyncData, write setting, update SyncList.
    // Completed earlier scopes need no historical buffer/permission registry.
    key->edit_kinds[kind]=true;r->gpd->edit_kind=kind;r->gpd->edit_entry=entry;
    r->gpd->edit_buffer=buffer;r->gpd->edit_length=length;
    r->gpd->image_edit=*frame;r->gpd->image_edit_entered=true;r->gpd->image_edit_returned=false;
    r->gpd->image_edit_result=0;r->gpd->image_actual_key=actual_key;
    r->gpd->image_insert={};r->gpd->image_insert_entered=false;r->gpd->image_insert_returned=false;
    r->gpd->stream_write={};r->gpd->stream_write_entered=false;r->gpd->stream_write_returned=false;
    r->gpd->stream_checkpoint_captured=false;r->gpd->write_outcome.reset();
  }catch(...){if(r){std::lock_guard lock(s.requests_mutex);r->uncertain=true;}}
}
void NativeProfileSaveOwner::State::ParentGpdEditReturn(void* raw,uint32_t function,const std::shared_ptr<Request>& r) noexcept {
  if(function!=0x81925AC0 || !raw || global_critical_region::is_held_by_current_thread())return;
  auto& s=*this;
  try {
    if(!r || r->site.message!=0xB0037)return;
    auto& c=*static_cast<cpu::ppc::PPCContext*>(raw);
    const auto frame=ActualSaveFrame(*s.kernel,c,function);const auto result=Dword(c.r[3]);
    std::lock_guard held(s.requests_mutex);auto* key=r->active_parent_key;
    if(!key || !frame || !result || !key->setting_entered || key->setting_returned ||
        !r->gpd->image_edit_entered || r->gpd->image_edit_returned || r->gpd->image_edit!=*frame)return;
    r->gpd->image_edit_result=*result;r->gpd->image_edit_returned=true;
    if(int32_t(*result)<0 && !r->gpd->native_first_failure)r->gpd->native_first_failure=*result;
    // Do not close the parent byte epoch or cancel on an individual key edit.
    // Remaining actual sync/key/copy/Header/close operations still own duties.
  }catch(...){/* Unknown original edit return never creates a final checkpoint. */}
}
void NativeProfileSaveOwner::ObserveOriginalParentKeySetterReturn(void* raw) noexcept {
  if(!raw || global_critical_region::is_held_by_current_thread())return;
  auto& s=*state_;State::Observation observation(s);if(!observation)return;
  try {
    if(!s.CodeCurrent(0x81912580))return;
    auto& c=*static_cast<cpu::ppc::PPCContext*>(raw);const auto r=s.CurrentInner(c);if(!r || r->site.message!=0xB0037)return;
    const auto frame=ActualSaveFrame(*s.kernel,c,0x81912580);const auto result=Dword(c.r[3]);
    std::lock_guard held(s.requests_mutex);auto* key=r->active_parent_key;
    if(!key || !frame || !result || !key->setter_entered || key->setter_returned || key->setter!=*frame)return;
    key->setter_result=*result;key->setter_returned=true;key->database.image_leaf_returned=true;
  }catch(...){/* Actual unknown original return is retained. */}
}
void NativeProfileSaveOwner::ObserveOriginalParentKeyReturn(void* raw) noexcept {
  if(!raw || global_critical_region::is_held_by_current_thread())return;
  auto& s=*state_;State::Observation observation(s);if(!observation)return;
  try {
    if(!s.CodeCurrent(0x8191F898))return;
    auto& c=*static_cast<cpu::ppc::PPCContext*>(raw);const auto r=s.CurrentInner(c);if(!r || r->site.message!=0xB0037)return;
    const auto frame=ActualSaveFrame(*s.kernel,c,0x8191F898);const auto result=Dword(c.r[3]);
    std::lock_guard held(s.requests_mutex);auto* key=r->active_parent_key;
    if(!key || !frame || !result || key->helper_returned || key->helper!=*frame ||
        !key->setter_returned || key->setter_result!=*result)return;
    key->result=*result;key->helper_returned=true;
    if(r->uncertain || r->source_error || int32_t(*result)<0 || !r->semantic_use || !r->semantic_use->unchanged())return;
    const auto flags=r->input->flags();const size_t expected=((flags&0x10)?1u:0u)+((flags&0x20)?1u:0u);
    if((flags&0x80) || !expected || r->parent_keys.size()!=expected)return;
    for(const auto& actual:r->parent_keys)if(!actual || !actual->helper_returned ||
        int32_t(actual->result)<0 || !actual->setter_returned || !actual->setting_returned ||
        int32_t(actual->setting_result)<0 || !actual->completed_checkpoint || !State::GpdUseEnded(actual->database))return;
    // Last actual selected key owns the FINAL full bytes/index revision. Old
    // earlier-key snapshots stay historical and cannot revive after new writes.
    r->whole_gpd_cache=key->completed_checkpoint;
  }catch(...){/* No fake final native current-table/IO/cache fact. */}
}
void NativeProfileSaveOwner::ObserveOriginalPictureCopyEntry(void* raw) noexcept {
  if(!raw || global_critical_region::is_held_by_current_thread())return;
  auto& s=*state_;State::Observation observation(s);if(!observation)return;
  try {
    if(!s.CodeCurrent(0x8191C4D8) || !s.CodeCurrent(0x8171BDC0) || !s.CodeCurrent(0x8171BA28))return;
    auto& c=*static_cast<cpu::ppc::PPCContext*>(raw);const auto r=s.CurrentInner(c);if(!r || r->site.message!=0xB0037)return;
    const auto frame=ActualSaveFrame(*s.kernel,c,0x8171BA28);if(!frame)return;
    using Kind=filesystem::ProfileArtifactKind;Kind source,target;uint32_t required_flag=0;
    switch(frame->lr) {
      case 0x8191CAC4:source=Kind::kAvatar64;target=Kind::kPublic64;required_flag=0x10;break;
      case 0x8191CB38:source=Kind::kAvatar32;target=Kind::kPublic32;required_flag=0x10;break;
      case 0x8191CBE4:source=Kind::kAvatar64;target=Kind::kPersonal64;required_flag=0x20;break;
      case 0x8191CC58:source=Kind::kAvatar32;target=Kind::kPersonal32;required_flag=0x20;break;
      default:return;
    }
    std::lock_guard held(s.requests_mutex);
    if(r->uncertain || r->release_entered || r->body_returned || !r->publication || !r->publication->parent_operation_ ||
        !r->input || !r->semantic_use || !r->semantic_use->unchanged() || r->source_error ||
        !r->parent_picture_sp || frame->sp!=r->parent_picture_sp ||
        (r->picture_call && !r->picture_call->returned) || (r->picture_copy && !r->picture_copy->returned) ||
        r->picture_copy_sites.size()>=4 || std::find(r->picture_copy_sites.begin(),r->picture_copy_sites.end(),frame->lr)!=r->picture_copy_sites.end() ||
        Dword(c.r[19])!=std::optional(1u) || Dword(c.r[25])!=std::optional(r->input->flags()) ||
        !(r->input->flags()&required_flag) || Dword(c.r[3])!=std::optional(frame->sp+0x320) ||
        Dword(c.r[4])!=std::optional(frame->sp+0xA0) || Dword(c.r[5])!=std::optional(0u) ||
        Dword(c.r[6])!=std::optional(0u) || Dword(c.r[7])!=std::optional(0u) || Dword(c.r[8])!=std::optional(0u))return;
    auto copy=std::make_shared<State::Request::PictureCopyCall>();copy->entry=*frame;copy->source=source;copy->target=target;
    r->picture_copies.push_back(copy);r->picture_copy_sites.push_back(frame->lr);r->picture_copy=std::move(copy);
    // Four original forward callsites, no new worker/queue or host CopyFile.
    // The guest still owns every read/write/EOF/SetTimes/close/error/cancel.
  }catch(...){/* Unproved native copy owns no new source or destination. */}
}
void NativeProfileSaveOwner::ObserveOriginalPictureCopyReturn(void* raw) noexcept {
  if(!raw || global_critical_region::is_held_by_current_thread())return;
  auto& s=*state_;State::Observation observation(s);if(!observation)return;
  try {
    auto& c=*static_cast<cpu::ppc::PPCContext*>(raw);const auto r=s.CurrentInner(c);
    const auto frame=ActualSaveFrame(*s.kernel,c,0x8171BA28);const auto result=Dword(c.r[3]);
    if(!r || !frame || !result || *result>1)return;
    std::lock_guard held(s.requests_mutex);
    if(!r->picture_copy || r->picture_copy->returned || r->picture_copy->entry!=*frame)return;
    r->picture_copy->result=*result;r->picture_copy->returned=true;
    // BOOL alone is not a full IO/count/metadata/durable publication receipt.
  }catch(...){/* Unknown is never converted to a completed native copy. */}
}
void NativeProfileSaveOwner::ObserveOriginalHeaderProducer(void* raw) noexcept {
  if(!raw || global_critical_region::is_held_by_current_thread())return;
  auto& s=*state_;State::Observation observation(s);if(!observation)return;
  std::shared_ptr<State::Request> r;
  std::shared_ptr<State::Request::HeaderChild> child;
  try {
    if(!s.CodeCurrent(0x8191C4D8) || !s.CodeCurrent(0x816EF548) ||
        !s.CodeCurrent(0x816EF470))return;
    auto& c=*static_cast<cpu::ppc::PPCContext*>(raw);r=s.CurrentInner(c);
    if(!r || !r->input || r->site.message!=0xB0037)return;
    const auto frame=ActualSaveFrame(*s.kernel,c,0x816EF470);if(!frame)return;
    SaveFrame body;uint32_t profile=0;
    {
      std::lock_guard lock(s.requests_mutex);
      if(r->uncertain || r->header_child || !r->body_entered || r->body_returned ||
          !r->getter_returned || r->release_entered || !r->native_profile ||
          !r->selected_source || r->source_error || !r->semantic_use ||
          !r->semantic_use->unchanged() || s.closed->load(std::memory_order_acquire))return;
      body=r->body;profile=r->native_profile;
    }
    // Actual supplied-buffer DDE8/E05C -> C4D8/CA44 -> frameless F548 ->
    // F470/F4F0. F470 has EARLIER calls: current LR=F4D8, whereas its saved
    // public caller is CA48. Never equate these two independent facts.
    auto& memory=*s.kernel->memory();
    if(body.function!=0x8191DDE8 || body.sp<0x940 || frame->sp>UINT32_MAX-0x2A0 ||
        frame->sp+0x2A0!=body.sp-0x940 || frame->lr!=0x816EF4D8 ||
        ReadWord(memory,frame->sp)!=std::optional(frame->sp+0x2A0) ||
        ReadWord(memory,frame->sp+0x2A0-8)!=std::optional(0x8191CA48u) ||
        ReadWord(memory,body.sp-0x940)!=std::optional(body.sp-0x200) ||
        ReadWord(memory,body.sp-0x200-8)!=std::optional(0x8191E060u) ||
        ReadWord(memory,body.sp-0x940+0x754)!=std::optional(profile) ||
        ReadWord(memory,body.sp-0x940+0x75C)!=std::optional(r->input->flags()) ||
        ReadWord(memory,0x816EF4F0)!=std::optional(0x4BFB3C39u) ||
        Dword(c.r[3])!=std::optional(0xFEu) || Dword(c.r[4])!=std::optional(0x20012u) ||
        Dword(c.r[5])!=std::optional(frame->sp+0x50) ||
        Dword(c.r[6])!=std::optional(0x218u) || Dword(c.r[7])!=std::optional(0u))return;
    child=std::make_shared<State::Request::HeaderChild>();
    if(ReadOwnedPayload(memory,frame->sp+0x50,child->input)!=X_ERROR_SUCCESS ||
        BigWord(child->input)!=0xFE || BigWord(child->input,0x208)!=0x200 ||
        BigWord(child->input,0x214)!=0 || !BigWord(child->input,0x20C) ||
        !BigWord(child->input,0x210) || BigWord(child->input,0x210)>0x3D00)return;
    child->thumbnail.resize(BigWord(child->input,0x210));
    if(ReadOwnedPayload(memory,BigWord(child->input,0x20C),child->thumbnail)!=X_ERROR_SUCCESS)return;
    // This may be the genuine derived/read thumbnail rather than the caller's
    // full-size PNG. Exact pinned construction and the same live parent own
    // that transformation; no HOST-generated picture is substituted.
    child->producer=*frame;child->public_frame=*frame;
    child->public_frame.sp+=0x2A0;child->public_frame.lr=0x8191CA48;
    NativeXmsgProvenance::ProducerFields fields;
    fields.root={s.config.allocation->owner().kernel_epoch,s.config.domain->id(),s.config.domain->root().generation};
    fields.public_invocation.invocation=r->invocation;
    fields.public_invocation.module=r->public_fact.module;
    fields.public_invocation.caller_module=r->public_fact.module;
    fields.public_invocation.function=0x816EF470;
    fields.public_invocation.instruction=0x8191CA44;
    fields.public_invocation.frame=child->public_frame.Fact();
    fields.producer=frame->Fact();fields.producer_instruction=0x816EF4F0;
    fields.wrapper=0x816A3128;fields.form=NativeXmsgProvenance::ProducerForm::kRequestWrapper;
    fields.app=0xFE;fields.message=0x20012;fields.argument=frame->sp+0x50;
    fields.argument_bytes=0x218;fields.overlap=0;fields.copied_bytes_budget=0x218;
    child->public_fact=fields.public_invocation;
    // Retention precedes Begin so a failed/uncertain subscription cannot lose
    // the parent source or invent a later independent operation on retry.
    {
      std::lock_guard lock(s.requests_mutex);
      if(r->uncertain || r->header_child || r->release_entered || !r->semantic_use->unchanged())return;
      r->header_child=child;
    }
    const auto capture=NativeXmsgProvenance::Begin(&c,NativeXmsgProvenance::AuthenticatedProducer(fields));
    std::lock_guard lock(s.requests_mutex);
    if(!capture || r->uncertain || r->release_entered)child->uncertain=true;
    if(capture)child->provenance=*capture;
  }catch(...){if(child){std::lock_guard lock(s.requests_mutex);child->uncertain=true;}}
}
void NativeProfileSaveOwner::ObserveOriginalHeaderWorkerEntry(void* raw) noexcept {
  if(!raw || global_critical_region::is_held_by_current_thread())return;
  auto& s=*state_;State::Observation observation(s);if(!observation)return;
  std::shared_ptr<State::Request::HeaderChild> selected;
  try {
    if(!s.CodeCurrent(0x816837A0) || !s.CodeCurrent(0x81687C00))return;
    auto& c=*static_cast<cpu::ppc::PPCContext*>(raw);
    const auto frame=ActualSaveFrame(*s.kernel,c,0x816837A0);
    const auto argument=Dword(c.r[3]);
    if(!frame || frame->lr!=0x81687D90 || !argument || !*argument)return;
    std::vector<std::pair<std::shared_ptr<State::Request>,std::shared_ptr<State::Request::HeaderChild>>> candidates;
    {
      std::lock_guard lock(s.requests_mutex);
      for(const auto& r:s.requests)if(r->header_child && r->header_child->provenance &&
          !r->header_child->worker_entered && !r->header_child->uncertain && !r->uncertain)
        candidates.emplace_back(r,r->header_child);
    }
    for(const auto& [r,child]:candidates) {
      const auto fact=NativeXmsgProvenance::CurrentDispatch(&c,child->provenance);
      if(!fact || fact->root!=NativeXmsgProvenance::RootFact{s.config.allocation->owner().kernel_epoch,
          s.config.domain->id(),s.config.domain->root().generation} ||
          fact->public_invocation!=child->public_fact || fact->app!=0xFE || fact->message!=0x20012 ||
          fact->original_argument!=child->producer.sp+0x50 || fact->original_argument_bytes!=0x218 ||
          fact->original_overlap || fact->provider_argument2 ||
          fact->provider_module!=r->public_fact.module)continue;
      if(fact->delivery==NativeXmsgProvenance::Delivery::kDirectInProcess) {
        if(fact->task || fact->provider_message!=0x20012 || fact->provider_argument!=*argument ||
            *argument!=fact->original_argument)continue;
      }else if(!fact->task || !fact->envelope || fact->provider_message!=0x80000007 ||
          fact->provider_argument!=fact->envelope || fact->copied_argument!=*argument ||
          !fact->copied_bytes || fact->copied_bytes->size()!=child->input.size() ||
          !std::equal(fact->copied_bytes->begin(),fact->copied_bytes->end(),child->input.begin()))continue;
      std::array<uint8_t,0x218> actual;
      if(ReadOwnedPayload(*s.kernel->memory(),*argument,actual)!=X_ERROR_SUCCESS || actual!=child->input ||
          NativeXmsgProvenance::CurrentDispatch(&c,child->provenance)!=fact)continue;
      if(selected){std::lock_guard lock(s.requests_mutex);selected->uncertain=true;child->uncertain=true;return;}
      selected=child;
      std::lock_guard lock(s.requests_mutex);
      if(r->uncertain || child->uncertain || child->worker_entered || r->release_entered)return;
      child->worker=*frame;child->dispatch=fact;child->worker_entered=true;
    }
  }catch(...){if(selected){std::lock_guard lock(s.requests_mutex);selected->uncertain=true;}}
}
void NativeProfileSaveOwner::ObserveOriginalHeaderBeforeEdit(void* raw) noexcept {
  if(!raw || global_critical_region::is_held_by_current_thread())return;
  auto& s=*state_;State::Observation observation(s);if(!observation)return;
  std::shared_ptr<State::Request> selected;
  std::shared_ptr<State::Request::HeaderChild> child;
  try {
    if(!s.CodeCurrent(0x816837A0) || !s.CodeCurrent(0x8167C628))return;
    auto& c=*static_cast<cpu::ppc::PPCContext*>(raw);
    const auto frame=ActualSaveFrame(*s.kernel,c,0x816837A0);
    const auto context=Dword(c.r[30]),packet=Dword(c.r[28]);
    if(!frame || frame->lr!=0x81683940 || !context || !*context ||
        uint64_t(*context)+0xA192>uint64_t(UINT32_MAX)+1 || !packet || !*packet ||
        ReadWord(*s.kernel->memory(),0x81683940)!=std::optional(0x387E1752u))return;
    std::vector<std::pair<std::shared_ptr<State::Request>,std::shared_ptr<State::Request::HeaderChild>>> candidates;
    {std::lock_guard lock(s.requests_mutex);for(const auto& r:s.requests)
      if(r->header_child && r->header_child->worker_entered && !r->header_child->worker_returned &&
          !r->uncertain && !r->header_child->uncertain)candidates.emplace_back(r,r->header_child);}
    for(const auto& [r,candidate]:candidates) {
      const auto fact=NativeXmsgProvenance::CurrentDispatch(&c,candidate->provenance);
      if(!fact || fact!=candidate->dispatch || frame->id!=candidate->worker.id ||
          frame->pcr!=candidate->worker.pcr || candidate->worker.sp<0x510 ||
          frame->sp!=candidate->worker.sp-0x510 ||
          *packet!=(fact->delivery==NativeXmsgProvenance::Delivery::kDirectInProcess?
            fact->original_argument:fact->copied_argument))continue;
      if(selected){std::lock_guard lock(s.requests_mutex);selected->uncertain=true;r->uncertain=true;return;}
      selected=r;child=candidate;
    }
    if(!selected || !child || child->before_captured || s.closed->load(std::memory_order_acquire))return;
    auto& memory=*s.kernel->memory();
    const auto critical=ReadCriticalSnapshot(memory,*context+0x1C);
    const auto handle=ReadWord(memory,*context+0x14),device_body=ReadWord(memory,*context);
    if(!critical || critical->owner_kthread!=frame->thread->guest_object() || critical->recursion<=0 ||
        !handle || !*handle || *handle==UINT32_MAX || !device_body || !*device_body ||
        ReadWord(memory,frame->sp+0x70)!=context ||
        ReadWord(memory,*packet+0x210)!=std::optional(uint32_t(child->thumbnail.size())))return;
    std::vector<uint8_t> actual_thumbnail(child->thumbnail.size());
    if(ReadOwnedPayload(memory,BigWord(child->input,0x20C),actual_thumbnail)!=X_ERROR_SUCCESS ||
        actual_thumbnail!=child->thumbnail)return;
    auto backing=s.kernel->object_table()->LookupObject<XFile>(*handle);
    auto device=XStfsDevice::Lookup(*s.kernel,*device_body);
    const auto package=backing && backing->file()?backing->file()->folder_content_package():nullptr;
    if(!backing || !package || !device || device->context_address()!=*context ||
        device->context_size()!=0xA192 || !device->MatchesFolderBacking(*backing,*package))return;
    std::error_code error;auto before=CaptureNativeHeaderBefore(memory,*context,error);
    if(!before || !selected->selected_source || !selected->selected_source->Current(error) ||
        !selected->semantic_use || !selected->semantic_use->unchanged())return;
    std::shared_ptr<NativeProfileSavePublication> publication;
    {
      std::lock_guard lock(s.requests_mutex);
      if(selected->uncertain || child->uncertain || child->worker_returned || child->before_captured ||
          selected->release_entered || !selected->semantic_use->unchanged())return;
      child->context=*context;child->backing_handle=*handle;child->device_body=*device_body;
      child->before=std::move(*before);child->backing=std::move(backing);child->device=std::move(device);
      child->before_captured=true;publication=selected->publication;
    }
    if(!publication || !publication->parent_operation_)return;
    NativeProfileSavePublication::SystemHeaderIssuer issuer;
    issuer.operation.source=selected->selected_source;issuer.operation.actual_semantic_use=selected->semantic_use;
    issuer.operation.domain=s.config.domain->id();issuer.operation.root_generation=s.config.domain->root().generation;
    issuer.operation.invocation=selected->invocation;issuer.operation.title=0xFFFE07D1;
    issuer.actual_backing_file=child->backing;issuer.actual_package=package;
    issuer.actual_native_before_header=child->before;
    const auto bound=publication->BindSystemHeaderInputAtOriginalEdit(std::move(issuer));
    if(bound){publication->CloseOriginalByteMutation();std::lock_guard held(s.requests_mutex);selected->source_error=bound;}
    // Metadata-only intersection with the SAME admission made before PNG IO.
    // No new stage/session, host wait or native control success is issued here.
  }catch(...){if(child){std::lock_guard lock(s.requests_mutex);child->uncertain=true;}}
}
void NativeProfileSaveOwner::ObserveOriginalHeaderWriteEntry(void* raw) noexcept {
  if(!raw || global_critical_region::is_held_by_current_thread())return;
  auto& s=*state_;State::Observation observation(s);if(!observation)return;
  std::shared_ptr<State::Request> r;
  try {
    if(!s.CodeCurrent(0x8167C628))return;
    auto& c=*static_cast<cpu::ppc::PPCContext*>(raw);r=s.CurrentHeader(c);
    const bool at_close=!r;const auto content=at_close?s.CurrentContent(c):State::ContentUse{};if(at_close)r=content.request;const auto close=content.operation;if(!r)return;
    const auto frame=ActualSaveFrame(*s.kernel,c,0x8167C628);const auto context=Dword(c.r[3]);
    if(!frame || !context || *context>UINT32_MAX-0x1C)return;
    const auto lock=ReadCriticalSnapshot(*s.kernel->memory(),*context+0x1C);
    if(!lock || lock->owner_kthread!=frame->thread->guest_object() || lock->recursion<=0)return;
    std::lock_guard held(s.requests_mutex);
    if(r->uncertain || r->source_error || !r->publication ||
        !r->semantic_use || !r->semantic_use->unchanged())return;
    auto* write=at_close?&close->write:&r->header_child->write;
    if(at_close) {
      if(close->uncertain || !close->header_input_bound || close->context_returned ||
          frame->lr!=0x81680F40 || !close->e60_body_sp ||
          frame->sp!=close->e60_body_sp || frame->id!=close->worker.id ||
          frame->pcr!=close->worker.pcr || *context!=close->context)return;
    }else {
      const auto& child=r->header_child;
      if(child->uncertain || !child->before_captured || child->worker_returned || child->worker.sp<0x510 ||
          frame->lr!=0x8168397C || frame->sp!=child->worker.sp-0x510 || *context!=child->context)return;
    }
    if(*write && !(*write)->returned)return;
    *write=State::Request::HeaderChild::HeaderWrite{*frame};
  }catch(...){if(r){std::lock_guard held(s.requests_mutex);r->uncertain=true;}}
}
void NativeProfileSaveOwner::ObserveOriginalHeaderWriteReturn(void* raw) noexcept {
  if(!raw || global_critical_region::is_held_by_current_thread())return;
  auto& s=*state_;State::Observation observation(s);if(!observation)return;
  try {
    if(!s.CodeCurrent(0x8167C628))return;
    auto& c=*static_cast<cpu::ppc::PPCContext*>(raw);auto r=s.CurrentHeader(c);
    const bool at_close=!r;const auto content=at_close?s.CurrentContent(c):State::ContentUse{};if(at_close)r=content.request;const auto close=content.operation;
    const auto frame=ActualSaveFrame(*s.kernel,c,0x8167C628);const auto result=Dword(c.r[3]);
    if(!r || !frame || !result)return;
    std::lock_guard held(s.requests_mutex);
    auto& write=at_close?close->write:r->header_child->write;
    if(!write || write->returned || write->entry!=*frame)return;
    write->result=*result;write->returned=true;
    // Actual native return only. No ignored Control0/1 result, signature-call
    // return, short write or whole close is promoted to durability/readiness.
  }catch(...){/* Real native own-return remains required. */}
}
void NativeProfileSaveOwner::ObserveOriginalInplaceFlushProducer(void* raw) noexcept {
  if(!raw || global_critical_region::is_held_by_current_thread())return;
  auto& s=*state_;State::Observation observation(s);if(!observation)return;
  std::shared_ptr<State::Request::CloseChild> child;
  try {
    if(!s.CodeCurrent(0x816EEC38) || !s.CodeCurrent(0x8190EDF0) || !s.CodeCurrent(0x81912580))return;
    auto& c=*static_cast<cpu::ppc::PPCContext*>(raw);const auto r=s.CurrentInner(c);
    const auto frame=ActualSaveFrame(*s.kernel,c,0x816EEC38);
    if(!r || !frame || r->site.message!=0xB0037 ||
        uint64_t(r->native_profile)+0x54>uint64_t(UINT32_MAX)+1)return;
    State::Request::ParentKeyUse* key=nullptr;
    {std::lock_guard held(s.requests_mutex);key=r->active_parent_key;
      if(!key || key->main_flush || !key->post_flush_entered || key->post_flush_returned ||
          key->setter_returned || !key->setting_returned || !key->database.image_release_returned ||
          key->database.image_lock_retained || r->uncertain || r->source_error ||
          !r->publication || !r->semantic_use || !r->semantic_use->unchanged())return;}
    if(key->setter.sp<0x1E0 || frame->sp!=key->setter.sp-0x1E0 || frame->lr!=0x816EEC80 ||
        frame->id!=key->setter.id || frame->pcr!=key->setter.pcr ||
        ReadWord(*s.kernel->memory(),frame->sp)!=std::optional(key->setter.sp-0x130) ||
        ReadWord(*s.kernel->memory(),key->setter.sp-0x138)!=std::optional(0x8190EE4Cu) ||
        ReadWord(*s.kernel->memory(),key->setter.sp-0xB8)!=std::optional(0x8191275Cu) ||
        Dword(c.r[30])!=std::optional(r->native_profile+0x44) || Dword(c.r[31])!=std::optional(0u) ||
        Dword(c.r[3])!=std::optional(0xFEu) || Dword(c.r[4])!=std::optional(0x20018u) ||
        Dword(c.r[5])!=std::optional(frame->sp+0x50) || Dword(c.r[6])!=std::optional(0x44u) ||
        Dword(c.r[7])!=std::optional(0u))return;
    child=std::make_shared<State::Request::CloseChild>();child->kind=State::Request::CloseChild::Kind::kInPlaceFlush;
    if(ReadOwnedPayload(*s.kernel->memory(),frame->sp+0x50,child->input)!=X_ERROR_SUCCESS ||
        child->input[0x3F] || BigWord(child->input,0x40))return;
    std::array<uint8_t,16> root{};
    if(ReadOwnedPayload(*s.kernel->memory(),r->native_profile+0x44,root)!=X_ERROR_SUCCESS)return;
    const auto end=std::find(root.begin(),root.end(),uint8_t(0));
    if(end==root.begin() || end==root.end() ||
        !std::equal(root.begin(),end,child->input.begin()) ||
        std::any_of(child->input.begin()+(end-root.begin()),child->input.end(),[](uint8_t v){return v!=0;}))return;
    child->producer=*frame;child->public_frame=*frame;child->public_frame.sp+=0xB0;child->public_frame.lr=0x8190EE4C;
    NativeXmsgProvenance::ProducerFields fields;
    fields.root={s.config.allocation->owner().kernel_epoch,s.config.domain->id(),s.config.domain->root().generation};
    fields.public_invocation={r->invocation,r->public_fact.module,r->public_fact.module,
        0x816EEC38,0x8190EE48,child->public_frame.Fact()};
    fields.producer=frame->Fact();fields.producer_instruction=0x816EECA4;fields.wrapper=0x816A31C8;
    fields.form=NativeXmsgProvenance::ProducerForm::kRequestWrapper;fields.app=0xFE;fields.message=0x20018;
    fields.argument=frame->sp+0x50;fields.argument_bytes=0x44;fields.overlap=0;fields.copied_bytes_budget=0x44;
    child->public_fact=fields.public_invocation;
    {std::lock_guard held(s.requests_mutex);
      if(r->active_parent_key!=key || key->main_flush || key->post_flush_returned || r->uncertain)return;
      key->main_flush=child;}
    const auto capture=NativeXmsgProvenance::Begin(&c,NativeXmsgProvenance::AuthenticatedProducer(fields));
    std::lock_guard held(s.requests_mutex);if(!capture)child->uncertain=true;else child->provenance=*capture;
  }catch(...){if(child){std::lock_guard held(s.requests_mutex);child->uncertain=true;}}
}
void NativeProfileSaveOwner::ObserveOriginalInplaceFlushWorkerEntry(void* raw) noexcept {
  if(!raw || global_critical_region::is_held_by_current_thread())return;
  auto& s=*state_;State::Observation observation(s);if(!observation)return;
  try {
    if(!s.CodeCurrent(0x81682FB8) || !s.CodeCurrent(0x81687C00))return;
    auto& c=*static_cast<cpu::ppc::PPCContext*>(raw);const auto use=s.CurrentContent(c);
    const auto r=use.request;const auto child=use.operation;
    if(!r || !child || child->kind!=State::Request::CloseChild::Kind::kInPlaceFlush)return;
    const auto frame=ActualSaveFrame(*s.kernel,c,0x81682FB8);const auto argument=Dword(c.r[3]);
    const auto fact=NativeXmsgProvenance::CurrentDispatch(&c,child->provenance);
    if(!frame || frame->lr!=0x81687CAC || !argument || !*argument || !fact ||
        fact->app!=0xFE || fact->message!=0x20018 || fact->public_invocation!=child->public_fact ||
        fact->original_argument!=child->producer.sp+0x50 || fact->original_argument_bytes!=0x44 ||
        fact->original_overlap || fact->provider_argument2 || fact->provider_module!=r->public_fact.module)return;
    if(fact->delivery==NativeXmsgProvenance::Delivery::kDirectInProcess) {
      if(fact->task || fact->provider_message!=0x20018 || fact->provider_argument!=*argument || *argument!=fact->original_argument)return;
    }else if(!fact->task || !fact->envelope || fact->provider_message!=0x80000007 ||
        fact->provider_argument!=fact->envelope || fact->copied_argument!=*argument || !fact->copied_bytes ||
        fact->copied_bytes->size()!=child->input.size() ||
        !std::equal(fact->copied_bytes->begin(),fact->copied_bytes->end(),child->input.begin()))return;
    std::array<uint8_t,0x44> actual;
    if(ReadOwnedPayload(*s.kernel->memory(),*argument,actual)!=X_ERROR_SUCCESS || actual!=child->input ||
        NativeXmsgProvenance::CurrentDispatch(&c,child->provenance)!=fact)return;
    std::lock_guard held(s.requests_mutex);
    if(r->uncertain || child->uncertain || child->worker_entered)return;
    child->worker=*frame;child->dispatch=fact;child->worker_entered=true;
  }catch(...){/* No inferred worker/current source from a shared E60 address. */}
}
void NativeProfileSaveOwner::ObserveOriginalInplaceFlushContextEntry(void* raw) noexcept {
  if(!raw || global_critical_region::is_held_by_current_thread())return;
  auto& s=*state_;State::Observation observation(s);if(!observation)return;
  try {
    if(!s.CodeCurrent(0x81680E60) || !s.CodeCurrent(0x81682FB8))return;
    auto& c=*static_cast<cpu::ppc::PPCContext*>(raw);const auto use=s.CurrentContent(c);
    const auto r=use.request;const auto child=use.operation;
    const auto frame=ActualSaveFrame(*s.kernel,c,0x81680E60);const auto context=Dword(c.r[3]);
    if(!r || !child || child->kind!=State::Request::CloseChild::Kind::kInPlaceFlush ||
        !frame || !context || !*context || uint64_t(*context)+0xA192>uint64_t(UINT32_MAX)+1 ||
        !child->worker_entered || child->worker_returned || child->worker.sp<0x130 ||
        frame->sp!=child->worker.sp-0x70 || frame->lr!=0x81683000 ||
        frame->id!=child->worker.id || frame->pcr!=child->worker.pcr ||
        Dword(c.r[4])!=std::optional(child->worker.sp-0x20))return;
    const auto handle=ReadWord(*s.kernel->memory(),*context+0x14),body=ReadWord(*s.kernel->memory(),*context);
    if(!handle || !body)return;
    auto backing=s.kernel->object_table()->LookupObject<XFile>(*handle);auto device=XStfsDevice::Lookup(*s.kernel,*body);
    const auto package=backing && backing->file()?backing->file()->folder_content_package():nullptr;
    if(!backing || !device || !package || device->context_address()!=*context ||
        device->context_size()!=0xA192 || !device->MatchesFolderBacking(*backing,*package))return;
    std::error_code error;const auto [policy,issued]=r->publication->CaptureParentHeaderRead(error);
    if(!policy || !issued || XFAILED(package->ObserveHeaderPublication(issued)))return;
    auto member=device->CaptureMemberForOriginalUse(*backing,*package);if(!member)return;
    std::lock_guard held(s.requests_mutex);
    if(r->uncertain || child->uncertain || child->context_entered || child->native_skip)return;
    child->context=*context;child->context_close=*frame;child->backing=std::move(backing);
    child->device=std::move(device);child->member_device=std::move(member);child->context_entered=true;
    child->e60_body_sp=frame->sp-0xC0;child->e60_backchain_sp=frame->sp;child->e60_saved_lr=0x81683000;
    // The SAME Header/source reservation is used. Actual Control0 after the
    // native CS is acquired supplies pre-edit bytes, never a fresh path tuple.
  }catch(...){/* Unknown real source/context stays unissued. */}
}
void NativeProfileSaveOwner::ObserveOriginalInplaceFlushSkipped(void* raw) noexcept {
  if(!raw || global_critical_region::is_held_by_current_thread())return;
  auto& s=*state_;State::Observation observation(s);if(!observation)return;
  try {
    if(!s.CodeCurrent(0x81682FB8))return;
    auto& c=*static_cast<cpu::ppc::PPCContext*>(raw);const auto use=s.CurrentContent(c);
    const auto child=use.operation;const auto frame=ActualSaveFrame(*s.kernel,c,0x81682FB8);
    if(!use.request || !child || child->kind!=State::Request::CloseChild::Kind::kInPlaceFlush ||
        !frame || !child->worker_entered || child->worker.sp<0x70 ||
        frame->sp!=child->worker.sp-0x70 || frame->lr!=0x81682FD4 ||
        frame->id!=child->worker.id || frame->pcr!=child->worker.pcr || Dword(c.r[11])!=std::optional(2u) ||
        ReadWord(*s.kernel->memory(),0x81682FE8)!=std::optional(0x40820030u))return;
    std::lock_guard held(s.requests_mutex);
    if(use.request->uncertain || child->uncertain || child->context_entered || child->worker_returned)return;
    child->native_skip=true; // Actual read-only branch, not absent-hook inference.
  }catch(...){/* A missing entry does not prove this native branch. */}
}
void NativeProfileSaveOwner::ObserveOriginalInplaceFlushContextReturn(void* raw) noexcept {
  if(!raw || global_critical_region::is_held_by_current_thread())return;
  auto& s=*state_;State::Observation observation(s);if(!observation)return;
  try {
    auto& c=*static_cast<cpu::ppc::PPCContext*>(raw);const auto use=s.CurrentContent(c);
    const auto child=use.operation;const auto frame=ActualSaveFrame(*s.kernel,c,0x81680E60);const auto result=Dword(c.r[3]);
    if(!use.request || !child || child->kind!=State::Request::CloseChild::Kind::kInPlaceFlush || !frame || !result)return;
    std::lock_guard held(s.requests_mutex);
    if(!child->context_entered || child->context_returned || child->context_close!=*frame)return;
    child->context_result=*result;child->context_returned=true;
  }catch(...){/* Own-return and inner IO/control results remain distinct. */}
}
void NativeProfileSaveOwner::ObserveOriginalInplaceFlushWorkerReturn(void* raw) noexcept {
  if(!raw || global_critical_region::is_held_by_current_thread())return;
  auto& s=*state_;State::Observation observation(s);if(!observation)return;
  try {
    auto& c=*static_cast<cpu::ppc::PPCContext*>(raw);const auto use=s.CurrentContent(c);
    const auto child=use.operation;const auto frame=ActualSaveFrame(*s.kernel,c,0x81682FB8);const auto result=Dword(c.r[3]);
    if(!use.request || !child || child->kind!=State::Request::CloseChild::Kind::kInPlaceFlush || !frame || !result)return;
    std::lock_guard held(s.requests_mutex);
    if(!child->worker_entered || child->worker_returned || child->worker!=*frame)return;
    child->worker_result=*result;child->worker_returned=true;
  }catch(...){/* No inferred namespace/ref or native completion. */}
}
void NativeProfileSaveOwner::ObserveOriginalInplaceFlushProducerReturn(void* raw) noexcept {
  if(!raw || global_critical_region::is_held_by_current_thread())return;
  auto& s=*state_;State::Observation observation(s);if(!observation)return;
  try {
    if(!s.CodeCurrent(0x816EEC38))return;
    auto& c=*static_cast<cpu::ppc::PPCContext*>(raw);const auto r=s.CurrentInner(c);
    const auto frame=ActualSaveFrame(*s.kernel,c,0x816EEC38);const auto result=Dword(c.r[3]);if(!r || !frame || !result)return;
    std::lock_guard held(s.requests_mutex);const auto* key=r->active_parent_key;
    const auto child=key?key->main_flush:nullptr;
    if(!child || child->kind!=State::Request::CloseChild::Kind::kInPlaceFlush ||
        child->producer_returned || child->public_frame!=*frame)return;
    child->producer_result=*result;child->producer_returned=true;
    // Actual public X_RESULT, separate from the HRESULT EDF0 ignores.
  }catch(...){/* Freed generic transport authority is never revived. */}
}
void NativeProfileSaveOwner::ObserveOriginalParentPostFlushEntry(void* raw) noexcept {
  if(!raw || global_critical_region::is_held_by_current_thread())return;
  auto& s=*state_;State::Observation observation(s);if(!observation)return;
  try {
    if(!s.CodeCurrent(0x8190EDF0) || !s.CodeCurrent(0x81912580))return;
    auto& c=*static_cast<cpu::ppc::PPCContext*>(raw);const auto r=s.CurrentInner(c);
    const auto frame=ActualSaveFrame(*s.kernel,c,0x8190EDF0);if(!r || !frame)return;
    std::lock_guard held(s.requests_mutex);auto* key=r->active_parent_key;
    if(!key || key->post_flush_entered || key->post_skip_no_changes || key->setter_returned ||
        key->setter.sp<0xB0 || frame->sp!=key->setter.sp-0xB0 || frame->lr!=0x8191275C ||
        frame->id!=key->setter.id || frame->pcr!=key->setter.pcr ||
        Dword(c.r[3])!=std::optional(r->native_profile))return;
    key->post_flush=*frame;key->post_flush_entered=true;
  }catch(...){/* A retained DB is not an actual post-release flush use. */}
}
void NativeProfileSaveOwner::ObserveOriginalParentPostFlushGate(void* raw,uint32_t at) noexcept {
  if(!raw || global_critical_region::is_held_by_current_thread())return;
  auto& s=*state_;State::Observation observation(s);if(!observation)return;
  try {
    if(!s.CodeCurrent(at) || (at!=0x81912750 && at!=0x8190EE08 && at!=0x8190EE18 && at!=0x8190EE28 && at!=0x8190EE34))return;
    auto& c=*static_cast<cpu::ppc::PPCContext*>(raw);const auto r=s.CurrentInner(c);if(!r)return;
    const bool setter=at==0x81912750;const auto frame=ActualSaveFrame(*s.kernel,c,setter?0x81912580:0x8190EDF0);
    const auto value=Dword(c.r[setter?23:at==0x8190EE28?29:11]);if(!frame || !value)return;
    std::lock_guard held(s.requests_mutex);auto* key=r->active_parent_key;
    if(!key || key->setter_returned || key->helper_returned)return;
    if(setter) {
      if(key->setter.sp<0xB0 || frame->sp!=key->setter.sp-0xB0 || frame->id!=key->setter.id || frame->pcr!=key->setter.pcr)return;
      if(!*value && !key->post_flush_entered)key->post_skip_no_changes=true;
      return;
    }
    if(!key->post_flush_entered || key->post_flush_returned || key->post_flush.sp<0x80 ||
        frame->sp!=key->post_flush.sp-0x80 || frame->id!=key->post_flush.id || frame->pcr!=key->post_flush.pcr)return;
    if((at==0x8190EE08 && *value) || (at==0x8190EE18 && !*value))key->post_skip_native_gate=true;
    if(at==0x8190EE28 && int32_t(*value)<0)key->post_gate_failure=*value;
    if(at==0x8190EE34){key->pec_gate_seen=true;key->pec_expected=*value!=0;}
  }catch(...){/* No missing hook or unknown field is classified no-effect. */}
}
void NativeProfileSaveOwner::ObserveOriginalParentPostFlushReturn(void* raw) noexcept {
  if(!raw || global_critical_region::is_held_by_current_thread())return;
  auto& s=*state_;State::Observation observation(s);if(!observation)return;
  try {
    auto& c=*static_cast<cpu::ppc::PPCContext*>(raw);const auto r=s.CurrentInner(c);
    const auto frame=ActualSaveFrame(*s.kernel,c,0x8190EDF0);if(!r || !frame)return;
    std::lock_guard held(s.requests_mutex);auto* key=r->active_parent_key;
    if(!key || !key->post_flush_entered || key->post_flush_returned || key->post_flush!=*frame)return;
    key->post_flush_returned=true; // Native void result says nothing about either child flush.
  }catch(...){/* Actual unknown post-release use remains retained. */}
}

void NativeProfileSaveOwner::ObserveOriginalCloseProducer(void* raw) noexcept {
  if(!raw || global_critical_region::is_held_by_current_thread())return;
  auto& s=*state_;State::Observation observation(s);if(!observation)return;
  std::shared_ptr<State::Request> r;std::shared_ptr<State::Request::CloseChild> close;
  try {
    if(!s.CodeCurrent(0x816EEB78) || !s.CodeCurrent(0x8191C4D8))return;
    auto& c=*static_cast<cpu::ppc::PPCContext*>(raw);r=s.CurrentInner(c);
    if(!r || r->site.message!=0xB0037 || !r->publication || !r->publication->parent_operation_)return;
    const auto frame=ActualSaveFrame(*s.kernel,c,0x816EEB78);if(!frame)return;
    const auto parent=r->parent_picture_sp;
    if(!parent || parent<0xB0 || frame->sp!=parent-0xB0 || frame->lr!=0x816EEBC0 ||
        ReadWord(*s.kernel->memory(),frame->sp)!=std::optional(parent) ||
        ReadWord(*s.kernel->memory(),parent-8)!=std::optional(0x8191CCF4u) ||
        Dword(c.r[3])!=std::optional(0xFEu) || Dword(c.r[4])!=std::optional(0x20003u) ||
        Dword(c.r[5])!=std::optional(frame->sp+0x50) || Dword(c.r[6])!=std::optional(0x44u) ||
        Dword(c.r[7])!=std::optional(0u))return;
    close=std::make_shared<State::Request::CloseChild>();
    if(ReadOwnedPayload(*s.kernel->memory(),frame->sp+0x50,close->input)!=X_ERROR_SUCCESS ||
        close->input[0x3F] || BigWord(close->input,0x40))return;
    NativeXmsgProvenance::ProducerFields fields;
    fields.root={s.config.allocation->owner().kernel_epoch,s.config.domain->id(),s.config.domain->root().generation};
    fields.public_invocation={r->invocation,r->public_fact.module,r->public_fact.module,
      0x816EEB78,0x8191CCF0,{frame->id,frame->pcr,parent,0x8191CCF4,0x816EEB78}};
    fields.producer=frame->Fact();fields.producer_instruction=0x816EEBE4;fields.wrapper=0x816A31C8;
    fields.form=NativeXmsgProvenance::ProducerForm::kRequestWrapper;fields.app=0xFE;fields.message=0x20003;
    fields.argument=frame->sp+0x50;fields.argument_bytes=0x44;fields.overlap=0;fields.copied_bytes_budget=0x44;
    close->producer=*frame;close->public_fact=fields.public_invocation;
    {std::lock_guard held(s.requests_mutex);if(r->uncertain || r->close_child || r->release_entered)return;r->close_child=close;}
    const auto capture=NativeXmsgProvenance::Begin(&c,NativeXmsgProvenance::AuthenticatedProducer(fields));
    std::lock_guard held(s.requests_mutex);if(!capture)close->uncertain=true;else close->provenance=*capture;
  }catch(...){if(close){std::lock_guard held(s.requests_mutex);close->uncertain=true;}}
}
void NativeProfileSaveOwner::ObserveOriginalCloseWorkerEntry(void* raw) noexcept {
  if(!raw || global_critical_region::is_held_by_current_thread())return;
  auto& s=*state_;State::Observation observation(s);if(!observation)return;
  try {
    if(!s.CodeCurrent(0x81682F20) || !s.CodeCurrent(0x81687C00))return;
    auto& c=*static_cast<cpu::ppc::PPCContext*>(raw);const auto r=s.CurrentClose(c);if(!r)return;
    const auto frame=ActualSaveFrame(*s.kernel,c,0x81682F20);const auto argument=Dword(c.r[3]);
    const auto fact=NativeXmsgProvenance::CurrentDispatch(&c,r->close_child->provenance);
    if(!frame || frame->lr!=0x81687C98 || !argument || !*argument || !fact ||
        fact->app!=0xFE || fact->message!=0x20003 || fact->public_invocation!=r->close_child->public_fact ||
        fact->original_argument!=r->close_child->producer.sp+0x50 || fact->original_argument_bytes!=0x44 ||
        fact->original_overlap || fact->provider_argument2 || fact->provider_module!=r->public_fact.module)return;
    const bool direct=fact->delivery==NativeXmsgProvenance::Delivery::kDirectInProcess;
    if(direct) {
      if(fact->task || fact->provider_message!=0x20003 || fact->provider_argument!=*argument ||
          *argument!=fact->original_argument)return;
    }else if(!fact->task || !fact->envelope || fact->provider_message!=0x80000007 ||
        fact->provider_argument!=fact->envelope || fact->copied_argument!=*argument ||
        !fact->copied_bytes || fact->copied_bytes->size()!=0x44 ||
        !std::equal(fact->copied_bytes->begin(),fact->copied_bytes->end(),r->close_child->input.begin()))return;
    std::array<uint8_t,0x44> actual;
    if(ReadOwnedPayload(*s.kernel->memory(),*argument,actual)!=X_ERROR_SUCCESS || actual!=r->close_child->input)return;
    std::lock_guard held(s.requests_mutex);auto& close=r->close_child;
    if(r->uncertain || close->uncertain || close->worker_entered)return;
    close->worker=*frame;close->dispatch=fact;close->worker_entered=true;
  }catch(...){/* An unproved close never completes a source or transaction. */}
}
void NativeProfileSaveOwner::ObserveOriginalContextCloseEntry(void* raw) noexcept {
  if(!raw || global_critical_region::is_held_by_current_thread())return;
  auto& s=*state_;State::Observation observation(s);if(!observation)return;
  try {
    if(!s.CodeCurrent(0x81680FC0))return;
    auto& c=*static_cast<cpu::ppc::PPCContext*>(raw);const auto r=s.CurrentClose(c);if(!r)return;
    const auto frame=ActualSaveFrame(*s.kernel,c,0x81680FC0);const auto context=Dword(c.r[3]);
    const auto close=r->close_child;
    if(!frame || !context || !*context || uint64_t(*context)+0xA192>uint64_t(UINT32_MAX)+1 ||
        !close->worker_entered || close->worker.sp<0x190 || frame->sp!=close->worker.sp-0x190 ||
        frame->id!=close->worker.id || frame->pcr!=close->worker.pcr ||
        frame->lr!=0x81682FA0 || Dword(c.r[4])!=std::optional(0u))return;
    const auto handle=ReadWord(*s.kernel->memory(),*context+0x14),body=ReadWord(*s.kernel->memory(),*context);
    if(!handle || !body)return;
    auto backing=s.kernel->object_table()->LookupObject<XFile>(*handle);auto device=XStfsDevice::Lookup(*s.kernel,*body);
    const auto package=backing && backing->file()?backing->file()->folder_content_package():nullptr;
    if(!backing || !device || !package || device->context_address()!=*context ||
        !device->MatchesFolderBacking(*backing,*package))return;
    std::error_code error;const auto [policy,issued]=r->publication->CaptureParentHeaderRead(error);
    if(!policy || !issued || XFAILED(package->ObserveHeaderPublication(issued)))return;
    auto member=device->CaptureMemberForOriginalUse(*backing,*package);if(!member)return;
    std::lock_guard held(s.requests_mutex);
    if(r->uncertain || close->uncertain || close->context_entered)return;
    close->context=*context;close->context_close=*frame;close->backing=std::move(backing);
    close->device=std::move(device);close->member_device=std::move(member);close->context_entered=true;
    if(frame->sp<0x1B0){close->uncertain=true;return;}
    close->e60_body_sp=frame->sp-0x1B0;close->e60_backchain_sp=frame->sp-0xF0;
    close->e60_saved_lr=0x81681040;
  }catch(...){/* Retain unknown native context/close; no guessed cache cleanup. */}
}
void NativeProfileSaveOwner::ObserveControlEntry(KernelState* kernel,const XStfsDevice& device,uint32_t code) noexcept try {
  if((code!=0 && code!=2) || !kernel || global_critical_region::is_held_by_current_thread() || !XThread::IsInThread())return;
  const auto owner=Lookup(kernel);if(!owner)return;auto& s=*owner->state_;State::Observation observation(s);if(!observation)return;
  auto& c=*XThread::GetCurrentThread()->thread_state()->context();const auto content=s.CurrentContent(c);const auto r=content.request;const auto close=content.operation;if(!r || !close)return;
  const auto frame=ActualSaveFrame(*kernel,c,0x81680E60);const auto context=Dword(c.r[31]);
  std::shared_ptr<NativeProfileSavePublication> publication;
  {std::lock_guard held(s.requests_mutex);
    if(!frame || frame->lr!=(code==2?0x81680EB8u:0x81680EA8u) || !context || !close->context_entered || close->context_returned ||
        close->device.get()!=&device || *context!=close->context || !close->e60_body_sp || !close->e60_backchain_sp ||
        frame->sp!=close->e60_body_sp || frame->id!=close->worker.id ||
        frame->pcr!=close->worker.pcr || close->uncertain || r->uncertain ||
        r->source_error || !r->semantic_use || !r->semantic_use->unchanged() || !r->publication ||
        (code==0 && close->header_input_bound))return;
    publication=r->publication;
  }
  if(!s.CodeCurrent(0x81680E60) || !s.CodeCurrent(0x8167C628) ||
      ReadWord(*kernel->memory(),frame->sp)!=std::optional(close->e60_backchain_sp) ||
      ReadWord(*kernel->memory(),close->e60_backchain_sp-8)!=std::optional(close->e60_saved_lr))return;
  const auto lock=ReadCriticalSnapshot(*kernel->memory(),*context+0x1C);
  if(!lock || lock->owner_kthread!=frame->thread->guest_object() || lock->recursion<=0)return;
  if(code==2) {
    // Original native data/allocation cut. Closing new GPD/PNG admission is an
    // atomic-only ownership effect, not a successful map/control or IO drain.
    // Header descriptor/sign writes remain possible on the SAME reservation.
    if(close->kind==State::Request::CloseChild::Kind::kFinalClose)
      publication->CloseDataByteMutationAtOriginalAllocationCut();
    return; // In-place native Flush retains the SAME mutable data epoch.
  }
  // A thumbnail child has already supplied the genuine earlier pre-edit
  // before image. Never recapture its changed cached bytes as a new before.
  bool already_bound=false;
  {std::unique_lock serial(publication->mutex_,std::try_to_lock);if(!serial.owns_lock())return;
    if(publication->header_issuer_) {
      already_bound=publication->header_issuer_->actual_backing_file.get()==close->backing.get() &&
          publication->header_issuer_->actual_package==close->backing->file()->folder_content_package();
      if(!already_bound)return;
    }
  }
  std::error_code error;
  if(!already_bound) {
    auto before=CaptureNativeHeaderBefore(*kernel->memory(),*context,error);if(!before)return;
    NativeProfileSavePublication::SystemHeaderIssuer issuer;
    // Immutable issuer is already independently owned by this exact parent.
    {std::unique_lock serial(publication->mutex_,std::try_to_lock);if(!serial.owns_lock() || !publication->system_issuer_)return;
      issuer.operation=*publication->system_issuer_;}
    issuer.actual_backing_file=close->backing;issuer.actual_package=close->backing->file()->folder_content_package();
    issuer.actual_native_before_header=std::move(*before);
    error=publication->BindSystemHeaderInputAtOriginalEdit(std::move(issuer));
  }
  std::lock_guard held(s.requests_mutex);
  if(error){r->source_error=error;close->uncertain=true;}
  else close->header_input_bound=true;
}catch(...){/* Unknown pre-edit ownership never issues a late Header writer. */}
struct NativeProfileSaveOwner::ControlCheckpoint::Data {
  std::shared_ptr<State::HostCloseIoUse> duty;
  std::shared_ptr<NativeProfileSavePublication> publication;
  std::shared_ptr<filesystem::HostStorageRoot> member_root;
  object_ref<XFile> raw,gpd;
  std::shared_ptr<NativeProfileSaveManagedGpd> gpd_service;
  std::shared_ptr<const NativeProfileGpdCheckpoint> gpd_checkpoint;
  bool gpd_required=false;
  std::shared_ptr<WriteOutcome> outcome;
  X_STATUS prior_status=X_STATUS_UNSUCCESSFUL;
  std::atomic<bool> entered{false};
};
NativeProfileSaveOwner::ControlCheckpoint::ControlCheckpoint(std::unique_ptr<Data> data):data_(std::move(data)) {}
NativeProfileSaveOwner::ControlCheckpoint::~ControlCheckpoint()=default;
std::shared_ptr<NativeProfileSaveOwner::ControlCheckpoint>
NativeProfileSaveOwner::CaptureControlCheckpoint(KernelState* kernel,const XStfsDevice& device,uint32_t code) noexcept try {
  if(code!=1 || !kernel || global_critical_region::is_held_by_current_thread() || !XThread::IsInThread())return {};
  const auto owner=Lookup(kernel);if(!owner)return {};auto& s=*owner->state_;
  State::Observation observation(s);if(!observation)return {};
  auto& c=*XThread::GetCurrentThread()->thread_state()->context();const auto r=s.CurrentClose(c);if(!r)return {};
  const auto frame=ActualSaveFrame(*kernel,c,0x81680E60);const auto context=Dword(c.r[31]);
  std::shared_ptr<State::Request::CloseChild> close;
  {std::lock_guard held(s.requests_mutex);close=r->close_child;
    if(!frame || frame->lr!=0x81680FA8 || !context || !close || !close->context_entered ||
        close->context_returned || close->worker_returned || close->uncertain || r->uncertain || r->source_error ||
        close->whole_checkpoint_captured || close->device.get()!=&device || *context!=close->context ||
        close->context_close.sp<0x1B0 || frame->sp!=close->context_close.sp-0x1B0 ||
        frame->id!=close->context_close.id || frame->pcr!=close->context_close.pcr ||
        !r->publication || !r->publication->parent_operation_ || !r->semantic_use || !r->semantic_use->unchanged())return {};
  }
  if(!s.CodeCurrent(0x81680E60) || !s.CodeCurrent(0x81680FC0) ||
      ReadWord(*kernel->memory(),frame->sp)!=std::optional(close->context_close.sp-0xF0) ||
      ReadWord(*kernel->memory(),close->context_close.sp-0xF8)!=std::optional(0x81681040u) ||
      Dword(c.r[3])!=std::optional(device.guest_object()) || Dword(c.r[4])!=std::optional(1u))return {};
  const auto lock=ReadCriticalSnapshot(*kernel->memory(),*context+0x1C);
  if(!lock || lock->owner_kthread!=frame->thread->guest_object() || lock->recursion<=0)return {};
  auto duty=std::make_shared<State::HostCloseIoUse>(owner->state_,r);if(!duty->admitted)return {};
  const auto member=std::dynamic_pointer_cast<vfs::HostPathDevice>(close->member_device);
  const auto package=close->backing && close->backing->file()?close->backing->file()->folder_content_package():nullptr;
  if(!member || !member->storage() || !package || !close->device ||
      !close->device->MatchesFolderBacking(*close->backing,*package))return {};
  auto data=std::make_unique<ControlCheckpoint::Data>();data->duty=duty;
  auto outcome=std::make_shared<WriteOutcome>();
  // Fallible participant/control-block allocation precedes the one-shot family
  // publication. A rejected host allocation cannot consume a successful lane.
  auto checkpoint=std::shared_ptr<ControlCheckpoint>(new ControlCheckpoint(std::move(data)));
  {
    std::lock_guard held(s.requests_mutex);
    if(r->uncertain || r->source_error || close!=r->close_child || close->uncertain ||
        close->whole_checkpoint_captured || close->context_returned || close->worker_returned)return {};
    auto& data=*checkpoint->data_;
    data.publication=r->publication;data.member_root=member->storage();data.outcome=outcome;data.raw=close->backing;
    data.gpd_required=r->publication->system_issuer_->gpd_mode==NativeProfileSavePublication::SystemGpdMode::kNativeWriteRequired;
    data.gpd=r->gpd->cached_before_file;data.gpd_service=r->gpd->write_service;data.gpd_checkpoint=r->whole_gpd_cache;
    auto first=close->control0;
    if(first==X_STATUS_SUCCESS)first=close->control2;
    if(first==X_STATUS_SUCCESS)first=close->control3;
    if(first==X_STATUS_SUCCESS && !close->header_input_bound)first=X_STATUS_UNSUCCESSFUL;
    if(first==X_STATUS_SUCCESS && (data.gpd_required ?
        (!data.gpd || !data.gpd->file() || !data.gpd_service || !data.gpd_checkpoint) :
        (data.gpd || data.gpd_service || data.gpd_checkpoint)))first=X_STATUS_NOT_SUPPORTED;
    // Both E60 branches reach this same end-control: an unchanged descriptor
    // legitimately skips C628/control4/NtFlush. A performed write instead
    // requires its true native return AND every actual byte result.
    if(first==X_STATUS_SUCCESS && close->write) {
      if(!close->write->returned || int32_t(close->write->result)<0 || close->control4!=X_STATUS_SUCCESS)
        first=close->control4!=X_STATUS_SUCCESS?close->control4:X_STATUS_UNSUCCESSFUL;
    }
    const auto complete=[](const std::shared_ptr<WriteOutcome>& value) {
      return value && value->phase.load(std::memory_order_acquire)==WriteOutcome::Phase::kStagedAwaitingWholeCheckpoint &&
          value->status.load(std::memory_order_relaxed)==X_STATUS_SUCCESS;
    };
    if(first==X_STATUS_SUCCESS && r->header_child &&
        (r->header_child->uncertain || !r->header_child->worker_returned ||
         int32_t(r->header_child->worker_result)<0))first=X_STATUS_UNSUCCESSFUL;
    if(first==X_STATUS_SUCCESS && ((r->picture_call && !r->picture_call->returned) ||
        (r->picture_copy && !r->picture_copy->returned)))first=X_STATUS_UNSUCCESSFUL;
    if(first==X_STATUS_SUCCESS)for(const auto& copy:r->picture_copies)
      if(!copy || !copy->returned || !copy->result){first=X_STATUS_UNSUCCESSFUL;break;}
    if(first==X_STATUS_SUCCESS)for(const auto& output:r->picture_write_outputs)
      if(!complete(output)){first=X_STATUS_UNSUCCESSFUL;break;}
    if(first==X_STATUS_SUCCESS && r->header_child)for(const auto& output:r->header_child->write_outputs)
      if(!complete(output)){first=X_STATUS_UNSUCCESSFUL;break;}
    if(first==X_STATUS_SUCCESS)for(const auto& output:close->write_outputs)
      if(!complete(output)){first=X_STATUS_UNSUCCESSFUL;break;}
    if(first==X_STATUS_SUCCESS)for(const auto& key:r->parent_keys) {
      if(!key){first=X_STATUS_UNSUCCESSFUL;break;}
      if(key->post_skip_no_changes) {
        if(key->post_flush_entered || key->main_flush)first=X_STATUS_UNSUCCESSFUL;
        if(first!=X_STATUS_SUCCESS)break;
        continue; // Positive actual12750 no-changed-settings branch.
      }
      if(!key->post_flush_entered || !key->post_flush_returned || key->post_gate_failure) {
        first=X_STATUS_UNSUCCESSFUL;break;
      }
      if(key->post_skip_native_gate) {
        if(key->main_flush || key->pec_gate_seen)first=X_STATUS_UNSUCCESSFUL;
        if(first!=X_STATUS_SUCCESS)break;
        continue; // Positive actual EDF0 guest/no-mounted-root branch.
      }
      if(!key->pec_gate_seen || key->pec_expected) {
        // PEC is a distinct nested1000-byte/zero-context package. Its native
        // driver/map/cache duties may not be waived by EDF0's void return.
        first=X_STATUS_NOT_SUPPORTED;break;
      }
      const auto& flush=key->main_flush;
      if(!flush || flush->uncertain || !flush->producer_returned || flush->producer_result ||
          !flush->worker_entered || !flush->worker_returned || int32_t(flush->worker_result)<0) {
        first=X_STATUS_UNSUCCESSFUL;break;
      }
      if(flush->native_skip) {
        if(flush->context_entered)first=X_STATUS_UNSUCCESSFUL;
        if(first!=X_STATUS_SUCCESS)break;
        continue; // Actual82FE8 read-only branch, never missing-hook inference.
      }
      if(!flush->context_entered || !flush->context_returned || int32_t(flush->context_result)<0 ||
          !flush->control1_returned || flush->control1!=X_STATUS_SUCCESS || flush->cut_status!=X_STATUS_SUCCESS || !flush->header_input_bound ||
          flush->control0!=X_STATUS_SUCCESS || flush->control2!=X_STATUS_SUCCESS || flush->control3!=X_STATUS_SUCCESS) {
        first=X_STATUS_UNSUCCESSFUL;break;
      }
      if(flush->write && (!flush->write->returned || int32_t(flush->write->result)<0 || flush->control4!=X_STATUS_SUCCESS)) {
        first=X_STATUS_UNSUCCESSFUL;break;
      }
      for(const auto& output:flush->write_outputs)if(!complete(output)){first=X_STATUS_UNSUCCESSFUL;break;}
      if(first!=X_STATUS_SUCCESS)break;
    }
    // PENDING is an unresolved synchronous prerequisite, never a new async
    // control operation. Return real Busy rather than minting a completion.
    data.prior_status=first==X_STATUS_PENDING?vfs::HostStorageError(Error(std::errc::device_or_resource_busy)):first;
    close->whole_outcome=outcome;close->whole_checkpoint_captured=true;
  }
  return checkpoint;
}catch(...){return {};}
X_STATUS NativeProfileSaveOwner::ControlCheckpoint::AfterOriginalControl(X_STATUS backend) noexcept try {
  if(!data_ || data_->entered.exchange(true,std::memory_order_acq_rel))return X_STATUS_ACCESS_DENIED;
  auto& d=*data_;const auto finish=[&](WriteOutcome::Phase phase,X_STATUS status) {
    d.outcome->status.store(status,std::memory_order_relaxed);
    d.outcome->phase.store(phase,std::memory_order_release);return status;
  };
  if(global_critical_region::is_held_by_current_thread() || !d.duty || !d.duty->admitted)
    return finish(WriteOutcome::Phase::kTransferRefused,X_STATUS_INVALID_DEVICE_REQUEST);
  // Actual backend failure has priority. A class/receipt or prior successful
  // write cannot turn unsupported control1/2/4 into guest success.
  if(backend!=X_STATUS_SUCCESS)return finish(WriteOutcome::Phase::kBackendFailed,backend);
  if(d.prior_status!=X_STATUS_SUCCESS)return finish(WriteOutcome::Phase::kTransferRefused,d.prior_status);
  X_STATUS status=X_STATUS_UNSUCCESSFUL;
  auto cut=XFile::CaptureIoCheckpoint(*d.duty->state->kernel,*d.member_root,status);
  if(cut)status=cut->Status();
  {std::lock_guard held(d.duty->state->requests_mutex);
    auto& close=d.duty->request->close_child;close->io_cut=cut;close->cut_status=status;
  }
  if(status!=X_STATUS_SUCCESS)return finish(WriteOutcome::Phase::kTransferRefused,
      status==X_STATUS_PENDING?vfs::HostStorageError(Error(std::errc::device_or_resource_busy)):status);
  if(d.gpd_required) {
    std::unique_lock serial(d.publication->mutex_,std::try_to_lock);
    if(!serial.owns_lock())return finish(WriteOutcome::Phase::kTransferRefused,
        vfs::HostStorageError(Error(std::errc::device_or_resource_busy)));
    const auto latest=d.publication->staged_checkpoints_.find(d.publication->system_issuer_->title);
    if(!d.gpd_checkpoint || latest==d.publication->staged_checkpoints_.end() || latest->second!=d.gpd_checkpoint)
      return finish(WriteOutcome::Phase::kTransferRefused,X_STATUS_UNSUCCESSFUL);
  }
  // Existing source/semantic, physical seals, full-native serialization and
  // same05 journal checks remain authoritative. No retained-reader/DB death
  // wait is added, and no generic IO drain creates a native cache receipt.
  if(const auto error=d.publication->SealManagedPicturesAtOriginalFileCut())
    return finish(WriteOutcome::Phase::kTransferRefused,vfs::HostStorageError(error));
  if(const auto error=d.publication->SealJoinedSystemAtOriginalWholeCheckpoint())
    return finish(WriteOutcome::Phase::kTransferRefused,vfs::HostStorageError(error));
  const auto result=d.publication->PublishAtOriginalCommitCut();
  d.outcome->transaction_state.store(uint32_t(result.state),std::memory_order_relaxed);
  d.outcome->intent_durable.store(result.storage.intent_durable,std::memory_order_relaxed);
  d.outcome->done_durable.store(result.storage.done_durable,std::memory_order_relaxed);
  d.outcome->journal_attempted.store(!result.attempted_journal.empty() || !result.storage.journal.empty() ||
      result.storage.intent_durable || result.storage.done_durable ||
      result.state==xam::ProfileGpdTransactionAttempt::State::kPreparationUncertain,std::memory_order_relaxed);
  using Phase=xam::ProfileGpdTransactionAttempt::State;
  if(result.error || (result.state!=Phase::kCommitted && result.state!=Phase::kAlreadyCommitted))
    return finish(WriteOutcome::Phase::kPublicationFailed,
        result.error?vfs::HostStorageError(result.error):X_STATUS_UNSUCCESSFUL);
  // Only this actual native current-table/IO participant may request the
  // shared joint cache consumer. Storage durability alone, namespace removal,
  // 80FC0's void return and retained DB lifetime grant none of this authority.
  const auto ready=d.publication->PromoteJoinedAtActualNativeCheckpoint(d.raw,d.gpd,d.gpd_service,
      d.gpd_checkpoint?std::span<const uint8_t>(d.gpd_checkpoint->index_):std::span<const uint8_t>{});
  if(XFAILED(ready))return finish(WriteOutcome::Phase::kCommittedCachePending,ready);
  {std::lock_guard held(d.duty->state->requests_mutex);d.duty->request->publication_settled=true;}
  return finish(WriteOutcome::Phase::kReady,ready);
}catch(...){
  if(data_ && data_->outcome){data_->outcome->status.store(X_STATUS_UNSUCCESSFUL,std::memory_order_relaxed);
    data_->outcome->phase.store(WriteOutcome::Phase::kPublicationFailed,std::memory_order_release);
    // A thrown entered host operation supplies no complete journal receipt.
    // Retain its exact artifacts for factual restart; never label it canceled,
    // clean or eligible for a fresh retry by absence of a returned receipt.
    std::lock_guard held(data_->duty->state->requests_mutex);
    data_->duty->request->recovery_requires_restart=true;
  }
  return X_STATUS_UNSUCCESSFUL;
}
void NativeProfileSaveOwner::ControlCheckpoint::OriginalCarrierDidNotReturn(X_STATUS failure) noexcept try {
  if(!data_ || !data_->outcome)return;
  // The actual wrapper lost/rejected its host invocation. Keep any factual
  // completed storage receipt; this distinct failure does not undo a CAS or
  // invent that the backend, native cancellation or its own-return occurred.
  data_->outcome->status.store(XFAILED(failure)?failure:X_STATUS_UNSUCCESSFUL,std::memory_order_relaxed);
  data_->outcome->phase.store(WriteOutcome::Phase::kPublicationFailed,std::memory_order_release);
  std::lock_guard held(data_->duty->state->requests_mutex);
  data_->duty->request->recovery_requires_restart=true;
}catch(...){/* Incomplete receipt remains fenced, never successful/canceled. */}
void NativeProfileSaveOwner::ObserveControlReturn(KernelState* kernel,const XStfsDevice& device,
    uint32_t code,X_STATUS status) noexcept try {
  if(!kernel || global_critical_region::is_held_by_current_thread() || !XThread::IsInThread())return;
  const auto owner=Lookup(kernel);if(!owner)return;auto& s=*owner->state_;State::Observation observation(s);if(!observation)return;
  auto& c=*XThread::GetCurrentThread()->thread_state()->context();const auto content=s.CurrentContent(c);const auto r=content.request;const auto close=content.operation;if(!r || !close)return;
  const auto frame=ActualSaveFrame(*kernel,c,0x81680E60);const auto context=Dword(c.r[31]);
  const uint32_t expected_lr=code==0?0x81680EA8:code==2?0x81680EB8:code==3?0x81680ED0:
      code==4?0x81680F68:code==1?0x81680FA8:0;
  if(!expected_lr || !s.CodeCurrent(0x81680E60) || !frame || frame->lr!=expected_lr || !context ||
      !close->context_entered || close->context_returned || *context!=close->context || close->device.get()!=&device ||
      !close->e60_body_sp || !close->e60_backchain_sp || frame->sp!=close->e60_body_sp ||
      frame->id!=close->worker.id || frame->pcr!=close->worker.pcr ||
      ReadWord(*kernel->memory(),frame->sp)!=std::optional(close->e60_backchain_sp) ||
      ReadWord(*kernel->memory(),close->e60_backchain_sp-8)!=std::optional(close->e60_saved_lr) ||
      Dword(c.r[3])!=std::optional(device.guest_object()) || Dword(c.r[4])!=std::optional(code))return;
  const auto lock=ReadCriticalSnapshot(*kernel->memory(),*context+0x1C);
  if(!lock || lock->owner_kthread!=frame->thread->guest_object() || lock->recursion<=0)return;
  std::shared_ptr<XFileIoCheckpoint> cut;X_STATUS cut_status=X_STATUS_PENDING;
  bool observation_only=false;
  {std::lock_guard held(s.requests_mutex);observation_only=code==1 && !close->whole_checkpoint_captured;}
  if(observation_only && close->member_device) {
    const auto member=std::dynamic_pointer_cast<vfs::HostPathDevice>(close->member_device);
    if(member && member->storage())cut=XFile::CaptureIoCheckpoint(*kernel,*member->storage(),cut_status);
    if(cut)cut_status=cut->Status(); // No drain, wait, polling, Flush or fake acknowledgement.
  }
  std::lock_guard held(s.requests_mutex);
  if(close->uncertain || close->context_returned)return;
  if(code==0)close->control0=status;
  if(code==2)close->control2=status;
  if(code==3)close->control3=status;
  if(code==4)close->control4=status;
  if(code==1){close->control1=status;close->control1_returned=true;
    if(observation_only){close->io_cut=std::move(cut);close->cut_status=cut_status;}
  }
  // Current unsupported control2 is preserved even when native GetLastStatus
  // or a void close loses it. No native success or PIS publication is inferred.
}catch(...){/* Unknown is retained; real kernel result is never rewritten. */}
void NativeProfileSaveOwner::ObserveOriginalContextCloseReturn(void* raw) noexcept {
  if(!raw || global_critical_region::is_held_by_current_thread())return;
  auto& s=*state_;State::Observation observation(s);if(!observation)return;
  try {auto& c=*static_cast<cpu::ppc::PPCContext*>(raw);const auto r=s.CurrentClose(c);if(!r)return;
    const auto frame=ActualSaveFrame(*s.kernel,c,0x81680FC0);std::lock_guard held(s.requests_mutex);
    const auto close=r->close_child;if(!frame || !close->context_entered || close->context_returned || close->context_close!=*frame)return;
    close->context_returned=true; // No read of the possibly freed native context.
  }catch(...){/* No fabricated native cache/close own-return. */}
}
void NativeProfileSaveOwner::ObserveOriginalCloseWorkerReturn(void* raw) noexcept {
  if(!raw || global_critical_region::is_held_by_current_thread())return;
  auto& s=*state_;State::Observation observation(s);if(!observation)return;
  try {auto& c=*static_cast<cpu::ppc::PPCContext*>(raw);const auto r=s.CurrentClose(c);if(!r)return;
    const auto frame=ActualSaveFrame(*s.kernel,c,0x81682F20);const auto result=Dword(c.r[3]);
    std::lock_guard held(s.requests_mutex);const auto close=r->close_child;
    if(!frame || !result || !close->worker_entered || close->worker_returned || close->worker!=*frame)return;
    close->worker_result=*result;close->worker_returned=true;
    if(r->publication)r->publication->CloseOriginalByteMutation();
    // Original final native status stays immutable. Actual map/control/IO and
    // native-cache joins remain independent, including true failed/no-close.
  }catch(...){/* No guessed original Complete/notification/Task free. */}
}
void NativeProfileSaveOwner::ObserveOriginalHeaderWorkerReturn(void* raw) noexcept {
  if(!raw || global_critical_region::is_held_by_current_thread())return;
  auto& s=*state_;State::Observation observation(s);if(!observation)return;
  try {
    if(!s.CodeCurrent(0x816837A0))return;
    auto& c=*static_cast<cpu::ppc::PPCContext*>(raw);
    const auto frame=ActualSaveFrame(*s.kernel,c,0x816837A0);const auto result=Dword(c.r[3]);if(!frame || !result)return;
    std::lock_guard lock(s.requests_mutex);
    for(const auto& r:s.requests)if(r->header_child && r->header_child->worker_entered &&
        !r->header_child->worker_returned && r->header_child->worker==*frame) {
      r->header_child->worker_result=*result;r->header_child->worker_returned=true;
      // Actual context/CS/File close/free tails may already have executed.
      // Only copied before bytes survive; never reread context or infer a
      // clean cache, whole ContentClose success or durable parent completion.
    }
  }catch(...){/* No fabricated native own-return or source retirement. */}
}

void NativeProfileSaveOwner::ObserveOriginalFbEntry(void* raw) noexcept {
  if(!raw || global_critical_region::is_held_by_current_thread())return;
  auto& s=*state_;State::Observation observation(s);if(!observation)return;std::shared_ptr<State::Request> touched;
  try {
    if(!s.CodeCurrent(0x8190AAC0) || !s.CodeCurrent(0x81906808) ||
       !s.CodeCurrent(0x818C97F8) || !s.CodeCurrent(0x8190B528) || !s.CodeCurrent(0x8190B530))return;
    auto& c=*static_cast<cpu::ppc::PPCContext*>(raw);auto* kernel=s.config.catalog->calls()->kernel();
    const auto frame=ActualSaveFrame(*kernel,c,0x8190AAC0);if(!frame)return;
    if(Dword(c.r[3])!=std::optional(0x81B2A370u) || frame->lr!=0x816A2A24)return;
    auto& memory=*kernel->memory();
    if(ReadWord(memory,0x81B2A370)!=std::optional(0x8162F258u))return;
    // The actual static receiver has a full11-slot vtable, not the old F260
    // bounded window. A destructor/base table or modified receiver refuses.
    constexpr uint32_t expected[]={0x81909B18,0x81906878,0x819068A8,0x81964638,
      0x81964638,0x818C18F8,0x8190B530,0x81964638,0x81907C98,0x81907C40,0x8190B528};
    std::array<uint8_t,sizeof(expected)> table;
    if(!internal::ReadNativeWaitSnapshot(memory,0x8162F258,table))return;
    for(size_t n=0;n<std::size(expected);++n)if(BigWord(table,n*4)!=expected[n])return;
    std::vector<std::shared_ptr<State::Request>> candidates;
    {
      std::lock_guard lock(s.requests_mutex);
      for(const auto& r:s.requests)if(r->provenance && !r->uncertain && !r->inner_entered)candidates.push_back(r);
    }
    std::shared_ptr<const NativeXmsgProvenance::DispatchFact> selected;
    for(const auto& r:candidates) {
      const auto fact=NativeXmsgProvenance::CurrentDispatch(&c,r->provenance);
      const NativeXmsgProvenance::RootFact root{s.config.allocation->owner().kernel_epoch,
          s.config.domain->id(),s.config.domain->root().generation};
      if(!fact || fact->root!=root || fact->app!=0xFB || fact->message!=r->site.message ||
         fact->public_invocation!=r->public_fact || fact->provider_callback!=0x81906808 ||
         fact->provider_module!=r->public_fact.module || fact->provider.thread!=frame->id ||
         fact->provider.pcr!=frame->pcr || fact->provider.sp<0x80 ||
         frame->sp!=fact->provider.sp-0x80 ||
         fact->original_argument!=r->descriptor || fact->original_argument_bytes!=r->site.descriptor_bytes ||
         fact->original_overlap!=r->overlap || fact->provider_argument2!=0)continue;
      uint32_t descriptor=0,overlap=0;
      if(fact->delivery==NativeXmsgProvenance::Delivery::kDirectInProcess) {
        if(fact->provider_message!=r->site.message || fact->provider_argument!=r->descriptor ||
           fact->envelope || fact->task || fact->original_overlap)continue;
        descriptor=r->descriptor;
      } else {
        if(fact->provider_message!=0x80000007 || !fact->envelope || !fact->task ||
           fact->provider_argument!=fact->envelope || !fact->copied_bytes ||
           fact->copied_argument_bytes!=r->site.descriptor_bytes ||
           fact->copied_bytes->size()!=r->input->descriptor().size() ||
           !std::equal(fact->copied_bytes->begin(),fact->copied_bytes->end(),r->input->descriptor().begin()))continue;
        // The FB receiver, not generic provider arg2, unwraps these actual
        // original envelope members while that native invocation is live.
        // Envelope+10 is the original referenced EVENT member, not a length.
        // Extent comes only from the authentic public/copier provenance.
        std::array<uint8_t,0x10> envelope;
        if(!internal::ReadNativeWaitSnapshot(memory,fact->envelope,envelope) ||
           BigWord(envelope,0)!=0xFB || BigWord(envelope,4)!=r->site.message ||
           BigWord(envelope,8)!=fact->native_overlap || BigWord(envelope,12)!=fact->copied_argument)continue;
        descriptor=BigWord(envelope,12);overlap=BigWord(envelope,8);
      }
      if(Dword(c.r[4])!=std::optional(r->site.message) || Dword(c.r[5])!=std::optional(descriptor) ||
         Dword(c.r[6])!=std::optional(overlap))continue;
      std::vector<uint8_t> bytes(r->site.descriptor_bytes);
      if(!internal::ReadNativeWaitSnapshot(memory,descriptor,bytes))continue;
      const auto reader=[&](uint32_t at,std::span<uint8_t> out) {
        return ReadOwnedPayload(memory,at,out);
      };
      std::error_code error;const auto input=NativeProfileSaveRequest::CaptureOriginalFb(r->site.message,bytes,reader,error);
      if(!input || error || !SameRequestBytes(*input,*r->input))continue;
      const auto current=NativeXmsgProvenance::CurrentDispatch(&c,r->provenance);
      if(!current || current->provider_invocation!=fact->provider_invocation || current->capture!=fact->capture)continue;
      if(touched){ // An ambiguous family invocation cannot lend either source.
        std::lock_guard lock(s.requests_mutex);touched->uncertain=true;r->uncertain=true;return;
      }
      touched=r;selected=std::move(current);
    }
    if(!touched || !selected)return;
    const auto descriptor=Dword(c.r[5]),overlap=Dword(c.r[6]);if(!descriptor || !overlap)return;
    std::lock_guard lock(s.requests_mutex);
    if(touched->uncertain || touched->inner_entered)return;
    touched->inner=*frame;touched->inner_descriptor=*descriptor;touched->native_overlap=*overlap;
    touched->dispatch=std::move(selected);touched->inner_entered=true;
    // This is still only the request/receiver/schema capsule. No selected-row
    // writer, model pending, DB lock/view, File authority or publication issued.
  } catch(...) {if(touched){std::lock_guard lock(s.requests_mutex);touched->uncertain=true;}}
}
void NativeProfileSaveOwner::ObserveOriginalFbReturn(void* raw) noexcept {
  if(!raw || global_critical_region::is_held_by_current_thread())return;
  auto& s=*state_;State::Observation observation(s);if(!observation)return;
  try {
    if(!s.CodeCurrent(0x8190AAC0))return;
    auto& c=*static_cast<cpu::ppc::PPCContext*>(raw);
    const auto frame=ActualSaveFrame(*s.config.catalog->calls()->kernel(),c,0x8190AAC0);if(!frame)return;
    std::vector<std::shared_ptr<State::Request>> candidates;
    {std::lock_guard lock(s.requests_mutex);for(const auto& r:s.requests)
      if(r->inner_entered && !r->inner_returned && r->inner==*frame)candidates.push_back(r);}
    if(candidates.size()!=1)return;
    const auto& r=candidates.front();
    const auto current=NativeXmsgProvenance::CurrentDispatch(&c,r->provenance);
    if(!current || !r->dispatch || current->provider_invocation!=r->dispatch->provider_invocation ||
       current->capture!=r->dispatch->capture)return;
    const auto result=Dword(c.r[3]);if(!result)return;
    std::lock_guard lock(s.requests_mutex);
    if(r->inner_returned)return;
    r->original_result=*result;r->inner_returned=true;
    // Original Complete may already have run. This observation does not
    // repeat it, own APC/Event stores or prove any native cached DB clean.
  }catch(...){/* Existing duties remain retained/uncertain until real stop. */}
}
void NativeProfileSaveOwner::ObserveOriginalPublicReturn(void* raw,uint32_t function) noexcept {
  if(!raw || global_critical_region::is_held_by_current_thread())return;
  auto& s=*state_;State::Observation observation(s);if(!observation)return;
  try {
    if(!s.CodeCurrent(function))return;
    auto& c=*static_cast<cpu::ppc::PPCContext*>(raw);
    const auto frame=ActualSaveFrame(*s.config.catalog->calls()->kernel(),c,function);if(!frame)return;
    std::shared_ptr<State::Request> found;
    std::lock_guard lock(s.requests_mutex);
    for(const auto& r:s.requests)if(!r->public_returned && r->public_frame==*frame) {
      if(found){found->uncertain=true;r->uncertain=true;return;}found=r;
    }
    if(found)found->public_returned=true;
    // The reaper must drop actual caller-code/domain owners outside this
    // mutex after this real return; it never waits for original native APC.
    // That full reaper/DB duty join is deliberately not wired by this draft.
  }catch(...){/* No invented public return/completion. */}
}
void NativeProfileSaveOwner::ObserveOriginalSelectionEntry(void* raw) noexcept {
  if(!raw || global_critical_region::is_held_by_current_thread())return;
  auto& s=*state_;State::Observation observation(s);if(!observation)return;std::shared_ptr<State::Request> r;
  try {
    if(!s.CodeCurrent(0x8191E360))return;
    auto& c=*static_cast<cpu::ppc::PPCContext*>(raw);r=s.CurrentInner(c);
    if(!r || r->site.message!=0xB0033)return;
    const auto frame=ActualSaveFrame(*s.kernel,c,0x8191E360);
    if(!frame || r->inner.sp<0xD0 || frame->sp!=r->inner.sp-0xD0 || frame->lr!=0x8190B07C ||
        Dword(c.r[3])!=std::optional(0x81B2A494u) || Dword(c.r[4])!=std::optional(r->inner_descriptor))return;
    std::lock_guard lock(s.requests_mutex);
    if(r->uncertain || r->inner_returned || r->selection_entered){r->uncertain=true;return;}
    r->selection=*frame;r->selection_entered=true;
  }catch(...){if(r){std::lock_guard lock(s.requests_mutex);r->uncertain=true;}}
}
void NativeProfileSaveOwner::ObserveOriginalSelectionReturn(void* raw) noexcept {
  if(!raw || global_critical_region::is_held_by_current_thread())return;
  auto& s=*state_;State::Observation observation(s);if(!observation)return;std::shared_ptr<State::Request> r;
  try {
    if(!s.CodeCurrent(0x8191E360))return;
    auto& c=*static_cast<cpu::ppc::PPCContext*>(raw);r=s.CurrentInner(c);
    if(!r || r->site.message!=0xB0033)return;
    const auto frame=ActualSaveFrame(*s.kernel,c,0x8191E360);
    std::lock_guard lock(s.requests_mutex);
    if(!frame || !r->selection_entered || r->selection_returned || r->selection!=*frame)return;
    r->selection_returned=true;
    if(!r->body_entered || !r->body_returned)r->uncertain=true;
    // No native object, expanded stack descriptor or result is reread after
    // this genuine outer return. Original native completion still owns itself.
  }catch(...){if(r){std::lock_guard lock(s.requests_mutex);r->uncertain=true;}}
}
void NativeProfileSaveOwner::ObserveOriginalBodyEntry(void* raw,uint32_t function) noexcept {
  if(!raw || global_critical_region::is_held_by_current_thread())return;
  auto& s=*state_;State::Observation observation(s);if(!observation)return;std::shared_ptr<State::Request> r;
  try {
    if(!s.CodeCurrent(function))return;
    auto& c=*static_cast<cpu::ppc::PPCContext*>(raw);r=s.CurrentInner(c);if(!r)return;
    const auto site=BodyFor(r->site.message);if(!site || site->function!=function)return;
    const auto frame=ActualSaveFrame(*s.config.catalog->calls()->kernel(),c,function);
    const bool selected=r->site.message==0xB0033;
    SaveFrame selection;
    {std::lock_guard lock(s.requests_mutex);
      if(selected && (!r->selection_entered || r->selection_returned))return;
      selection=r->selection;
    }
    const uint32_t parent_sp=selected?selection.sp:r->inner.sp;
    const uint32_t depth=selected?0xA0u:0xD0u;
    if(!frame || parent_sp<depth || frame->sp!=parent_sp-depth ||
       frame->lr!=site->caller_lr || Dword(c.r[3])!=std::optional(0x81B2A494u))return;
    uint32_t descriptor=r->inner_descriptor;
    if(selected) {
      if(frame->sp>UINT32_MAX-0x7C)return;
      descriptor=frame->sp+0x50;
      std::array<uint8_t,44> expanded;
      if(Dword(c.r[4])!=std::optional(descriptor) ||
          !internal::ReadNativeWaitSnapshot(*s.kernel->memory(),descriptor,expanded) ||
          !r->input->MatchesOriginalSelectionExpansion(expanded))return;
    } else if(Dword(c.r[4])!=std::optional(descriptor))return;
    std::lock_guard lock(s.requests_mutex);
    if(r->uncertain || r->inner_returned || r->body_entered ||
        (selected && (!r->selection_entered || r->selection_returned || r->selection!=selection))){r->uncertain=true;return;}
    r->body=*frame;r->body_descriptor=descriptor;r->body_entered=true;
  }catch(...){if(r){std::lock_guard lock(s.requests_mutex);r->uncertain=true;}}
}
void NativeProfileSaveOwner::ObserveOriginalSemanticEntry(void* raw,uint32_t function) noexcept {
  if(!raw)return;
  auto& s=*state_;State::Observation observation(s);if(!observation)return;
  try {
    using Target=save_detail::SemanticUses::Target;
    Target target;unsigned argument=3;
    switch(function) {
      case 0x819110E0: case 0x819133B0: case 0x81913D18: case 0x819104F8:
        target=Target::kProfile;break;
      case 0x81928528:target=Target::kProfile;argument=5;break;
      case 0x819283B8: case 0x819285D0:target=Target::kDatabase;break;
      case 0x81928358:target=Target::kResource;argument=4;break;
      default:return;
    }
    if(global_critical_region::is_held_by_current_thread()) {
      s.semantic_uses.Unavailable();return;
    }
    if(!s.CodeCurrent(function)) {s.semantic_uses.Unavailable();return;}
    auto& c=*static_cast<cpu::ppc::PPCContext*>(raw);
    const auto frame=ActualSaveFrame(*s.kernel,c,function);const auto address=Dword(c.r[argument]);
    if(!frame || !address) {s.semantic_uses.Unavailable();return;}
    // ALL genuine original callers matter, including another thread. No
    // CurrentInner/Task/slot filter can hide interference with our bound use.
    s.semantic_uses.Enter({frame->id,frame->pcr,frame->sp,frame->lr,function},target,*address);
  }catch(...) {s.semantic_uses.Unavailable();}
}
void NativeProfileSaveOwner::ObserveOriginalSemanticReturn(void* raw,uint32_t function) noexcept {
  if(!raw)return;
  auto& s=*state_;State::Observation observation(s);if(!observation)return;
  try {
    switch(function) {
      case 0x819110E0: case 0x819133B0: case 0x81913D18: case 0x819104F8:
      case 0x81928528: case 0x819283B8: case 0x819285D0: case 0x81928358:break;
      default:return;
    }
    if(global_critical_region::is_held_by_current_thread()) {
      s.semantic_uses.Unavailable();return;
    }
    if(!s.CodeCurrent(function)) {s.semantic_uses.Unavailable();return;}
    auto& c=*static_cast<cpu::ppc::PPCContext*>(raw);const auto frame=ActualSaveFrame(*s.kernel,c,function);
    if(!frame) {s.semantic_uses.Unavailable();return;}
    // No object read at return: genuine destructor/release can already have
    // freed/rebound it. The exact prior ORIGINAL frame retires only metadata.
    s.semantic_uses.Leave({frame->id,frame->pcr,frame->sp,frame->lr,function});
  }catch(...) {s.semantic_uses.Unavailable();}
}
void NativeProfileSaveOwner::ObserveOriginalGetterEntry(void* raw) noexcept {
  if(!raw || global_critical_region::is_held_by_current_thread())return;
  auto& s=*state_;State::Observation observation(s);if(!observation)return;std::shared_ptr<State::Request> r;
  try {
    if(!s.CodeCurrent(0x8190E310))return;
    auto& c=*static_cast<cpu::ppc::PPCContext*>(raw);r=s.CurrentInner(c);if(!r)return;
    const auto site=BodyFor(r->site.message);if(!site || r->input->user_slot()>=4)return;
    SaveFrame body;
    {std::lock_guard lock(s.requests_mutex);if(!r->body_entered || r->body_returned || r->getter_entered)return;body=r->body;}
    auto* kernel=s.config.catalog->calls()->kernel();
    const auto frame=ActualSaveFrame(*kernel,c,0x8190E310);
    const auto singleton=ReadWord(*kernel->memory(),0x81A96838);
    if(!frame || body.sp<site->frame_bytes || frame->sp!=body.sp-site->frame_bytes ||
       frame->lr!=site->getter_lr || !singleton || !*singleton || uint64_t(*singleton)+0x34>UINT32_MAX ||
       Dword(c.r[3])!=std::optional(*singleton+0x34) ||
       Dword(c.r[4])!=std::optional(r->input->user_slot()))return;
    std::lock_guard lock(s.requests_mutex);
    if(r->uncertain || r->body_returned || r->getter_entered)return;
    // Begin BEFORE the original getter can take its array lock/reference.
    // An unchanged later field snapshot cannot repair overlapping mutation.
    r->semantic_use=s.semantic_uses.BeginGetter();
    r->getter=*frame;r->native_array=*singleton+0x34;r->getter_entered=true;
    // Original E310 owns its real array lock, reference increment and unlock.
    // No object is borrowed or native ref increment manufactured here.
  }catch(...){if(r){std::lock_guard lock(s.requests_mutex);r->uncertain=true;}}
}
void NativeProfileSaveOwner::ObserveOriginalGetterReturn(void* raw) noexcept {
  if(!raw || global_critical_region::is_held_by_current_thread())return;
  auto& s=*state_;State::Observation observation(s);if(!observation)return;std::shared_ptr<State::Request> r;
  std::shared_ptr<xam::ProfileRosterNativeWriter> source;
  try {
    if(!s.CodeCurrent(0x8190E310))return;
    auto& c=*static_cast<cpu::ppc::PPCContext*>(raw);r=s.CurrentInner(c);if(!r)return;
    auto* kernel=s.config.catalog->calls()->kernel();const auto frame=ActualSaveFrame(*kernel,c,0x8190E310);
    {
      std::lock_guard lock(s.requests_mutex);
      if(!frame || !r->getter_entered || r->getter_returned || r->getter!=*frame || r->body_returned)return;
    }
    const auto profile=Dword(c.r[3]);if(!profile || (*profile&3))return;
    uint64_t profile_xuid=0,job_source=0;std::error_code error;
    const bool unchanged=*profile && s.semantic_uses.BindProfile(r->semantic_use,*profile);
    if(*profile) {
      if(uint64_t(*profile)+0x6C>uint64_t(UINT32_MAX)+1)return;
      std::array<uint8_t,0x14> prefix;
      if(!internal::ReadNativeWaitSnapshot(*kernel->memory(),*profile,prefix))return;
      profile_xuid=uint64_t(BigWord(prefix,0))<<32 | BigWord(prefix,4);
      const auto refs=ReadWord(*kernel->memory(),*profile+0x68);
      if(!profile_xuid || BigWord(prefix,0x10)!=r->input->user_slot() || !refs || int32_t(*refs)<=0)return;
      // +0 is the mask1/storage profile identity; +8 is separate online
      // identity. Neither equality nor this native reference grants source.
      // The distinct issuer below checks PM's ACTUAL selected envelope against
      // genuinely retained current/exact active intent and its full model /
      // Account / storage / source / managed / sign-in / version tuple.
      if(unchanged && !s.closed->load(std::memory_order_acquire)) {
        const auto identity=NativeProfileIdentityOwner::Lookup(kernel);
        const auto candidates=identity?identity->CaptureSaveSelectionCandidates():std::nullopt;
        if(candidates) {
          job_source=candidates->job_source;
          source=xam::ProfileRosterNativeWriter::CaptureFromActualGetter(
              *kernel->xam_state()->profile_manager(),candidates->current,candidates->active_intent,
              uint8_t(r->input->user_slot()),profile_xuid,error);
        } else {error=Error(std::errc::operation_not_permitted);}
      } else {error=Error(std::errc::operation_canceled);}
      // This existing factory is metadata-only/try-admitted and issues no
      // model pending or native File/open capability. Actual managed source /
      // DB use MUST agree at pre-edit; otherwise no storage effect is allowed.
    }
    const auto current=NativeXmsgProvenance::CurrentDispatch(&c,r->provenance);
    if(!current || !r->dispatch || current->provider_invocation!=r->dispatch->provider_invocation)return;
    {
      std::lock_guard lock(s.requests_mutex);
      if(r->uncertain || r->getter_returned || r->body_returned)return;
      r->native_profile=*profile;r->native_profile_xuid=profile_xuid;r->identity_job_source=job_source;
      if(s.closed->load(std::memory_order_acquire))r->source_error=Error(std::errc::operation_canceled);
      else {r->source_error=error;r->selected_source=std::move(source);}
      r->getter_returned=true;
    }
    if(!*profile)s.semantic_uses.Retire(r->semantic_use);
    // A null native result is an ordinary original getter failure, not a
    // fabricated empty profile or reason to provision an online identity.
  }catch(...){if(r){std::lock_guard lock(s.requests_mutex);r->uncertain=true;}}
  // Any refused local source token drops after the family mutex; its exact
  // counted PM release never retargets a replacement or waits on native IO.
}
void NativeProfileSaveOwner::ObserveOriginalProfileReleaseEntry(void* raw) noexcept {
  if(!raw || global_critical_region::is_held_by_current_thread())return;
  auto& s=*state_;State::Observation observation(s);if(!observation)return;std::shared_ptr<State::Request> r;
  try {
    if(!s.CodeCurrent(0x819114B8))return;
    auto& c=*static_cast<cpu::ppc::PPCContext*>(raw);r=s.CurrentInner(c);if(!r)return;
    const auto site=BodyFor(r->site.message);if(!site)return;
    SaveFrame body;uint32_t profile=0;
    {
      std::lock_guard lock(s.requests_mutex);
      if(!r->getter_returned || !r->native_profile || r->body_returned || r->release_entered)return;
      body=r->body;profile=r->native_profile;
    }
    const auto frame=ActualSaveFrame(*s.config.catalog->calls()->kernel(),c,0x819114B8);
    if(!frame || body.sp<site->frame_bytes || frame->sp!=body.sp-site->frame_bytes ||
       frame->lr!=site->release_lr || Dword(c.r[3])!=std::optional(profile))return;
    std::lock_guard lock(s.requests_mutex);
    if(r->uncertain || r->release_entered || r->body_returned)return;
    r->release=*frame;r->release_entered=true;
    s.semantic_uses.Retire(r->semantic_use);
    // Original Release can destroy/free the object. From this real entry,
    // future native-object reads/issuance must refuse; owned File/source and
    // staged/durable duties remain separately retained through their joins.
  }catch(...){if(r){std::lock_guard lock(s.requests_mutex);r->uncertain=true;}}
}
void NativeProfileSaveOwner::ObserveOriginalProfileReleaseReturn(void* raw) noexcept {
  if(!raw || global_critical_region::is_held_by_current_thread())return;
  auto& s=*state_;State::Observation observation(s);if(!observation)return;std::shared_ptr<State::Request> r;
  try {
    if(!s.CodeCurrent(0x819114B8))return;
    auto& c=*static_cast<cpu::ppc::PPCContext*>(raw);r=s.CurrentInner(c);if(!r)return;
    const auto frame=ActualSaveFrame(*s.config.catalog->calls()->kernel(),c,0x819114B8);
    std::lock_guard lock(s.requests_mutex);
    if(!frame || !r->release_entered || r->release_returned || r->release!=*frame)return;
    r->release_returned=true;
    // DO NOT reread profile+68 (or any object member) here: real final Release
    // may already have run destructor104F8 and free6B10. This is only the exact
    // original reference-own-return receipt, not DB/File/publication success.
  }catch(...){if(r){std::lock_guard lock(s.requests_mutex);r->uncertain=true;}}
}
void NativeProfileSaveOwner::ObserveOriginalBodyReturn(void* raw,uint32_t function) noexcept {
  if(!raw || global_critical_region::is_held_by_current_thread())return;
  auto& s=*state_;State::Observation observation(s);if(!observation)return;std::shared_ptr<State::Request> r;
  try {
    if(!s.CodeCurrent(function))return;
    auto& c=*static_cast<cpu::ppc::PPCContext*>(raw);r=s.CurrentInner(c);if(!r)return;
    const auto frame=ActualSaveFrame(*s.config.catalog->calls()->kernel(),c,function);
    std::lock_guard lock(s.requests_mutex);
    if(!frame || !r->body_entered || r->body_returned || r->body!=*frame)return;
    r->body_returned=true;
    if(!r->getter_returned || (r->native_profile && !r->release_returned))r->uncertain=true;
    // The same selected_source count stays owned until the full native owner
    // has joined actual DB/File/transaction duties. Body return alone cannot
    // cancel a partial journal, promote a cache view or drop those resources.
  }catch(...){if(r){std::lock_guard lock(s.requests_mutex);r->uncertain=true;}}
}
void NativeProfileSaveOwner::ObserveOriginalImageTitleEntry(void* raw) noexcept {
  if(!raw || global_critical_region::is_held_by_current_thread())return;
  auto& s=*state_;State::Observation observation(s);if(!observation)return;std::shared_ptr<State::Request> r;
  try {
    if(!s.CodeCurrent(0x81907D00))return;
    auto& c=*static_cast<cpu::ppc::PPCContext*>(raw);r=s.CurrentInner(c);
    if(!r || r->site.message!=0xB0031)return;
    SaveFrame body;
    {std::lock_guard lock(s.requests_mutex);if(!r->getter_returned || !r->native_profile ||
        r->release_entered || r->body_returned || r->normalize_entered)return;body=r->body;}
    auto* kernel=s.config.catalog->calls()->kernel();const auto frame=ActualSaveFrame(*kernel,c,0x81907D00);
    const auto singleton=ReadWord(*kernel->memory(),0x81A96838);
    if(!frame || body.sp<0x70 || frame->sp!=body.sp-0x70 || frame->lr!=0x8191CFC4 ||
       !singleton || !*singleton || Dword(c.r[3])!=singleton ||
       Dword(c.r[4])!=std::optional(r->input->raw_title()))return;
    std::lock_guard lock(s.requests_mutex);
    if(r->uncertain || r->release_entered || r->normalize_entered)return;
    r->normalize=*frame;r->normalize_entered=true;
  }catch(...){if(r){std::lock_guard lock(s.requests_mutex);r->uncertain=true;}}
}
void NativeProfileSaveOwner::ObserveOriginalImageTitleReturn(void* raw) noexcept {
  if(!raw || global_critical_region::is_held_by_current_thread())return;
  auto& s=*state_;State::Observation observation(s);if(!observation)return;std::shared_ptr<State::Request> r;
  try {
    if(!s.CodeCurrent(0x81907D00))return;
    auto& c=*static_cast<cpu::ppc::PPCContext*>(raw);r=s.CurrentInner(c);
    if(!r || r->site.message!=0xB0031)return;
    const auto frame=ActualSaveFrame(*s.config.catalog->calls()->kernel(),c,0x81907D00);
    const auto title=Dword(c.r[3]);
    std::lock_guard lock(s.requests_mutex);
    if(!frame || !title || !r->normalize_entered || r->normalize_returned || r->normalize!=*frame ||
       r->release_entered || r->body_returned)return;
    r->gpd->normalized_title=*title;r->normalize_returned=true;
    // Actual return only, even zero. Raw0 is not HOST dashboard/foreground;
    // FFFE07D2's real native title-manager rule is not reproduced by HLE.
  }catch(...){if(r){std::lock_guard lock(s.requests_mutex);r->uncertain=true;}}
}
void NativeProfileSaveOwner::ObserveOriginalImageLeafEntry(void* raw) noexcept {
  if(!raw || global_critical_region::is_held_by_current_thread())return;
  auto& s=*state_;State::Observation observation(s);if(!observation)return;std::shared_ptr<State::Request> r;
  try {
    if(!s.CodeCurrent(0x819101D0))return;
    auto& c=*static_cast<cpu::ppc::PPCContext*>(raw);r=s.CurrentInner(c);
    if(!r || r->site.message!=0xB0031)return;
    SaveFrame body;uint32_t title=0,profile=0;
    {std::lock_guard lock(s.requests_mutex);if(!r->normalize_returned || r->gpd->image_leaf_entered ||
        r->body_returned || r->release_entered)return;body=r->body;title=r->gpd->normalized_title;profile=r->native_profile;}
    auto* kernel=s.config.catalog->calls()->kernel();const auto frame=ActualSaveFrame(*kernel,c,0x819101D0);
    if(!frame || !profile || body.sp<0x70 || frame->sp!=body.sp-0x70 || frame->lr!=0x8191CFD4 ||
       Dword(c.r[3])!=std::optional(profile) || Dword(c.r[4])!=std::optional(r->inner_descriptor))return;
    std::array<uint8_t,0x28> descriptor;
    if(!internal::ReadNativeWaitSnapshot(*kernel->memory(),r->inner_descriptor,descriptor) || BigWord(descriptor,4)!=title)return;
    const auto original=r->input->descriptor();
    if(original.size()!=descriptor.size())return;
    for(size_t at=0;at<descriptor.size();++at)if((at<4 || at>=8) && descriptor[at]!=original[at])return;
    std::lock_guard lock(s.requests_mutex);
    if(r->uncertain || r->release_entered || r->gpd->image_leaf_entered)return;
    r->gpd->image_leaf=*frame;r->gpd->image_leaf_entered=true;
    // Only the proved original title normalization may differ from ingress.
    // No cached-table edit, managed stage, pending or writer IO starts here.
  }catch(...){if(r){std::lock_guard lock(s.requests_mutex);r->uncertain=true;}}
}
void NativeProfileSaveOwner::ObserveOriginalImageDbEntry(void* raw) noexcept {
  if(!raw || global_critical_region::is_held_by_current_thread())return;
  auto& s=*state_;State::Observation observation(s);if(!observation)return;std::shared_ptr<State::Request> r;
  try {
    if(!s.CodeCurrent(0x81911D48) || !s.CodeCurrent(0x81928418) || !s.CodeCurrent(0x819235F0))return;
    auto& c=*static_cast<cpu::ppc::PPCContext*>(raw);r=s.CurrentInner(c);
    if(!r)return;const auto caller=s.CurrentGpdCaller(*r);if(!caller)return;
    SaveFrame leaf;uint32_t title=0,profile=0;
    {std::lock_guard lock(s.requests_mutex);if(!r->gpd->image_leaf_entered || r->gpd->image_leaf_returned ||
        r->gpd->image_db_entered || r->release_entered)return;leaf=r->gpd->image_leaf;title=r->gpd->normalized_title;profile=r->native_profile;}
    auto* kernel=s.config.catalog->calls()->kernel();const auto frame=ActualSaveFrame(*kernel,c,0x81911D48);
    if(!frame || leaf.sp<caller->body_bytes || frame->sp!=leaf.sp-caller->body_bytes || frame->lr!=caller->db_return ||
       Dword(c.r[3])!=std::optional(profile) || Dword(c.r[4])!=std::optional(title) ||
       Dword(c.r[5])!=std::optional(frame->sp+caller->holder_offset) || Dword(c.r[6])!=std::optional(0u) ||
       ReadWord(*kernel->memory(),frame->sp+caller->holder_offset)!=std::optional(0u))return;
    const uint32_t offset=title==0xFFFE07D1?0x134u:0xD8u;
    if(uint64_t(profile)+offset>UINT32_MAX)return;
    std::lock_guard lock(s.requests_mutex);
    if(r->uncertain || r->release_entered || r->gpd->image_db_entered)return;
    r->gpd->image_db=*frame;r->gpd->image_db_output=frame->sp+caller->holder_offset;
    r->gpd->image_db_expected=profile+offset;r->gpd->image_db_entered=true;
    // Pending original acquisition scope only. GetDB enters its real profile
    // lock and actual DB acquire; a later HRESULT0 alone will NOT establish
    // a CS acquisition, dirty-cache cleanliness or permission to write.
  }catch(...){if(r){std::lock_guard lock(s.requests_mutex);r->uncertain=true;}}
}
void NativeProfileSaveOwner::ObserveOriginalImageEnsureEntry(void* raw,uint32_t function) noexcept {
  if(!raw || global_critical_region::is_held_by_current_thread())return;
  auto& s=*state_;State::Observation observation(s);if(!observation)return;
  std::shared_ptr<State::Request> r;
  try {
    if(function!=0x81928750 && function!=0x81928408)return;
    if(!s.CodeCurrent(function))return;
    auto& c=*static_cast<cpu::ppc::PPCContext*>(raw);r=s.CurrentInner(c);
    if(!r)return;const auto caller=s.CurrentGpdCaller(*r);if(!caller)return;
    SaveFrame leaf;uint32_t db=0,title=0;
    std::shared_ptr<xam::ProfileRosterNativeWriter> source;
    {std::lock_guard lock(s.requests_mutex);if(!r->gpd->image_db_returned || int32_t(r->gpd->image_db_result)<0 ||
        !r->gpd->image_lock_retained || r->gpd->image_ensure_entered || r->gpd->image_release_entered || r->release_entered)return;
      leaf=r->gpd->image_leaf;db=r->gpd->image_db_pointer;title=r->gpd->normalized_title;source=r->selected_source;}
    const auto frame=ActualSaveFrame(*s.kernel,c,function);
    if(!frame || leaf.sp<caller->body_bytes || frame->sp!=leaf.sp-caller->body_bytes || frame->lr!=caller->ensure_return ||
        Dword(c.r[3])!=std::optional(db) || function!=(title==0xFFFE07D1?0x81928750u:0x81928408u))return;
    // A genuine cached Ensure may perform no new open. Retain only the real
    // recognized immutable File tuple selected by THIS authenticated getter/
    // GetDB/held-CS use. The original Ensure still runs; complete native table
    // equality and same actual File/source are required at its return.
    object_ref<XFile> cached;std::shared_ptr<NativeProfileGpdReadView> cached_view;
    std::shared_ptr<const NativeProfileGpdCheckpoint> staged;
    uint32_t cached_stream=0,cached_handle=0;std::error_code cached_error;
    const auto stream=ReadWord(*s.kernel->memory(),db+0x2C);
    if(source && source->Current(cached_error) && stream && *stream) {
      std::array<uint8_t,16> prefix;
      if(internal::ReadNativeWaitSnapshot(*s.kernel->memory(),*stream,prefix) && BigWord(prefix)==0x81630230) {
        cached_handle=BigWord(prefix,12);
        if(cached_handle && cached_handle!=UINT32_MAX) {
          cached=s.kernel->object_table()->LookupObject<XFile>(cached_handle);
          if(cached && cached->file())cached_view=NativeProfileSaveManagedGpd::TryCaptureCurrentFileRead(
              *cached->file(),source->writer().model,title,cached_error);
          if(caller->parent && cached && cached->file() && r->publication &&
              NativeProfileSaveManagedGpd::MatchesOwnedPublication(*cached->file(),r->publication)) {
            auto index=CaptureNativeGpdIndex(*s.kernel->memory(),*stream,cached_handle,cached_error);
            if(index)staged=NativeProfileSaveManagedGpd::CaptureStagedFileCheckpointAtActualDbUse(
                *cached->file(),r->publication,*index,cached_error);
            if(staged) {
              std::lock_guard held(s.requests_mutex);
              for(const auto& prior:r->parent_keys)if(prior && &prior->database!=r->gpd &&
                  prior->helper_returned && prior->database.cached_before_file.get()==cached.get() &&
                  prior->database.opened_view)cached_view=prior->database.opened_view;
            }
          }
          if(cached_view)cached_stream=*stream;
        }
      }
    }
    std::lock_guard lock(s.requests_mutex);
    if(r->uncertain || r->gpd->image_ensure_entered || !r->gpd->image_lock_retained || r->release_entered)return;
    r->gpd->image_ensure=*frame;r->gpd->image_ensure_entered=true;
    if(cached_view && r->semantic_use && r->semantic_use->unchanged()) {
      r->gpd->cached_before_file=std::move(cached);r->gpd->opened_view=std::move(cached_view);
      r->gpd->native_stream=cached_stream;r->gpd->opened_handle=cached_handle;
      r->gpd->reused_checkpoint=std::move(staged);
    }
  }catch(...){if(r){std::lock_guard lock(s.requests_mutex);r->uncertain=true;}}
}
void NativeProfileSaveOwner::ObserveOriginalImageEnsureReturn(void* raw) noexcept {
  if(!raw || global_critical_region::is_held_by_current_thread())return;
  auto& s=*state_;State::Observation observation(s);if(!observation)return;
  std::shared_ptr<State::Request> r;
  try {
    if(!s.CodeCurrent(0x819101D0) || !s.CodeCurrent(0x81923B68) || !s.CodeCurrent(0x81923AD0))return;
    auto& c=*static_cast<cpu::ppc::PPCContext*>(raw);r=s.CurrentInner(c);
    if(!r)return;const auto caller=s.CurrentGpdCaller(*r);if(!caller)return;
    const auto frame=ActualSaveFrame(*s.kernel,c,caller->function);const auto result=Dword(c.r[3]);
    SaveFrame leaf;uint32_t db=0,stream=0,handle=0;
    object_ref<XFile> cached;std::shared_ptr<xam::ProfileRosterNativeWriter> source;bool new_open=false;
    std::shared_ptr<const NativeProfileSaveManagedGpd::ReadReceipt> receipt;
    std::shared_ptr<NativeProfileGpdReadView> expected;
    std::shared_ptr<const NativeProfileGpdCheckpoint> reused;
    {std::lock_guard lock(s.requests_mutex);
      if(!r->gpd->image_ensure_entered || r->gpd->image_ensure_returned || !r->gpd->image_lock_retained ||
          r->gpd->image_release_entered || r->release_entered)return;
      leaf=r->gpd->image_leaf;db=r->gpd->image_db_pointer;stream=r->gpd->native_stream;handle=r->gpd->opened_handle;
      receipt=r->gpd->open_receipt;expected=r->gpd->opened_view;
      cached=r->gpd->cached_before_file;source=r->selected_source;new_open=r->gpd->image_stream_entered;
      reused=r->gpd->reused_checkpoint;
    }
    // Exact original virtual+8 return at10248 BEFORE the native edit begins.
    if(!frame || !result || leaf.sp<caller->body_bytes || frame->sp!=leaf.sp-caller->body_bytes || frame->lr!=caller->ensure_return ||
        Dword(c.r[caller->ensure_db_register])!=std::optional(db) ||
        Dword(c.r[caller->descriptor_register])!=std::optional(caller->descriptor))return;
    std::vector<uint8_t> native;std::error_code error;
    if(int32_t(*result)>=0) {
      if((!receipt && !(cached && !new_open)) || !source || !expected || !stream || !handle || handle==UINT32_MAX ||
          ReadWord(*s.kernel->memory(),db+0x2C)!=std::optional(stream)) {
        error=Error(std::errc::operation_not_supported);
      } else {
        std::array<uint8_t,16> prefix;
        if(!internal::ReadNativeWaitSnapshot(*s.kernel->memory(),stream,prefix) ||
            BigWord(prefix)!=0x81630230 || BigWord(prefix,12)!=handle) {error=Error(std::errc::bad_message);}
        else if(caller->parent && reused) {
          // This is the SAME owned staged File, not a committed BEFORE view.
          // Genuine cached Ensure may retain it without reopening or rebinding.
          auto current_index=CaptureNativeGpdIndex(*s.kernel->memory(),stream,handle,error);
          const auto file=s.kernel->object_table()->LookupObject<XFile>(handle);
          const auto current=current_index && file && file.get()==cached.get() && file->file() ?
              NativeProfileSaveManagedGpd::CaptureStagedFileCheckpointAtActualDbUse(
                  *file->file(),r->publication,*current_index,error):nullptr;
          if(!current || current!=reused) {if(!error)error=Error(std::errc::state_not_recoverable);}
          else native=std::move(*current_index);
        }
        else {
          const auto bytes=expected->token_->bytes();
          uint64_t table=UINT64_MAX;
          if(bytes.size()>=24)table=24+uint64_t(XdbfTableView::U32(bytes.data()+8))*18+
                                       uint64_t(XdbfTableView::U32(bytes.data()+16))*8;
          if(table>bytes.size() || table>UINT32_MAX || BigWord(prefix,8)!=table || !BigWord(prefix,4))
            error=Error(std::errc::bad_message);
          else {
            const auto file=s.kernel->object_table()->LookupObject<XFile>(handle);
            const auto current=file && file->file()?
                (!new_open && cached && file.get()==cached.get() ?
                  NativeProfileSaveManagedGpd::TryCaptureCurrentFileRead(*file->file(),source->writer().model,r->gpd->normalized_title,error):
                  receipt ? NativeProfileSaveManagedGpd::TryCaptureOpenedRead(*file->file(),receipt,error):nullptr):nullptr;
            if(!current || current!=expected) {if(!error)error=Error(std::errc::bad_file_descriptor);}
            else {
              native.resize(size_t(table));
              if(ReadOwnedPayload(*s.kernel->memory(),BigWord(prefix,4),native)!=X_ERROR_SUCCESS)
                error=Error(std::errc::bad_address);
              else error=save_detail::ValidateNativeGpdSerialization(bytes,bytes,native);
            }
          }
        }
      }
    }
    std::lock_guard lock(s.requests_mutex);
    if(r->uncertain || r->gpd->image_ensure_returned || !r->gpd->image_lock_retained || r->release_entered)return;
    r->gpd->image_ensure_result=*result;r->gpd->image_ensure_returned=true;
    if(error)r->source_error=error;
    if(int32_t(*result)>=0 && !error) {
      r->gpd->native_before_header_index=std::move(native);r->gpd->native_before_header_valid=true;
    }
    // This proves this actual current immutable File/table use, either a
    // genuine new open or THIS retained cached File on the authenticated
    // getter/GetDB/CS path. It is no enduring object/config witness or writer;
    // the independent actual SYSTEM pre-edit issuer still owns admission.
  }catch(...){if(r){std::lock_guard lock(s.requests_mutex);r->uncertain=true;}}
}
void NativeProfileSaveOwner::ObserveOriginalImageEditEntry(void* raw,uint32_t function) noexcept {
  if(!raw || global_critical_region::is_held_by_current_thread())return;
  auto& s=*state_;State::Observation observation(s);if(!observation)return;
  std::shared_ptr<State::Request> r;
  try {
    if(function!=0x81925AC0 && function!=0x81925110)return;
    if(!s.CodeCurrent(function))return;
    auto& c=*static_cast<cpu::ppc::PPCContext*>(raw);r=s.CurrentInner(c);
    if(!r)return;if(r->site.message==0xB0037){s.ParentGpdEditEntry(raw,function,r);return;}
    if(r->site.message!=0xB0031 || !s.CodeCurrent(0x819101D0))return;
    SaveFrame leaf;uint32_t db=0;std::optional<CriticalSnapshot> acquired;
    {std::lock_guard lock(s.requests_mutex);
      if(!r->gpd->image_ensure_returned || int32_t(r->gpd->image_ensure_result)<0 ||
          !r->gpd->image_lock_retained || r->gpd->image_edit_entered || r->gpd->image_release_entered || r->release_entered)return;
      leaf=r->gpd->image_leaf;db=r->gpd->image_db_pointer;acquired=r->gpd->image_try_after;}
    const auto& payload=r->input->large();const bool remove=payload.null_pointer();
    const auto key=r->input->image_key();const auto frame=ActualSaveFrame(*s.kernel,c,function);
    if(!frame || !key || !acquired || leaf.sp<0xA0 || frame->sp!=leaf.sp-0xA0 ||
        frame->id!=leaf.id || frame->pcr!=leaf.pcr ||
        frame->lr!=(remove?0x819102D8u:0x819102D0u) || function!=(remove?0x81925110u:0x81925AC0u) ||
        Dword(c.r[3])!=std::optional(db) || Dword(c.r[4])!=std::optional(frame->sp+0x58) ||
        Dword(c.r[29])!=std::optional(db) || Dword(c.r[30])!=std::optional(r->inner_descriptor) ||
        Dword(c.r[27])!=std::optional(r->native_profile))return;
    if(!remove && (Dword(c.r[5])!=std::optional(payload.address) ||
        Dword(c.r[6])!=std::optional(payload.declared_bytes) || Dword(c.r[7])!=std::optional(0u) ||
        Dword(c.r[8])!=std::optional(0u)))return;
    std::array<uint8_t,10> native_key;std::array<uint8_t,1> autosave;
    if(!internal::ReadNativeWaitSnapshot(*s.kernel->memory(),frame->sp+0x58,native_key) ||
        !internal::ReadNativeWaitSnapshot(*s.kernel->memory(),db+0x31,autosave) ||
        native_key[0] || native_key[1]!=2)return;
    uint64_t actual_key=0;for(size_t at=2;at<native_key.size();++at)actual_key=actual_key<<8|native_key[at];
    if(actual_key!=*key)return;
    const auto current_lock=ReadCriticalSnapshot(*s.kernel->memory(),r->gpd->image_owned_cs);
    if(!current_lock || current_lock->owner_kthread!=frame->thread->guest_object() ||
        current_lock->recursion!=acquired->recursion || current_lock->recursion<=0)return;
    // ORIGINAL descriptor remains native-owned. Snapshot only at this genuine
    // pre-edit use; later RAM reuse cannot replace the immutable ingress plan.
    std::array<uint8_t,0x28> descriptor;
    if(!internal::ReadNativeWaitSnapshot(*s.kernel->memory(),r->inner_descriptor,descriptor) ||
        BigWord(descriptor,4)!=r->gpd->normalized_title)return;
    const auto original=r->input->descriptor();bool same=original.size()==descriptor.size();
    for(size_t at=0;same && at<descriptor.size();++at)
      if((at<4 || at>=8) && descriptor[at]!=original[at])same=false;
    if(same && !remove && payload.declared_bytes) {
      std::vector<uint8_t> actual(payload.declared_bytes);
      same=ReadOwnedPayload(*s.kernel->memory(),payload.address,actual)==X_ERROR_SUCCESS && actual==payload.bytes;
    }
    std::shared_ptr<xam::ProfileRosterNativeWriter> source;
    std::shared_ptr<NativeProfileGpdReadView> before_view;
    std::shared_ptr<const NativeProfileSaveManagedGpd::ReadReceipt> receipt;
    std::vector<uint8_t> before_header;uint32_t handle=0,title=0;
    object_ref<XFile> cached;bool new_open=false;
    {
      std::lock_guard lock(s.requests_mutex);
      if(r->uncertain || r->gpd->image_edit_entered || !r->gpd->image_lock_retained || r->release_entered)return;
      r->gpd->image_edit=*frame;r->gpd->image_edit_entered=true;r->gpd->image_actual_key=actual_key;
      r->gpd->image_actual_input_matches=same;r->gpd->image_actual_autosave=autosave[0]!=0;
      if(!same)r->source_error=Error(std::errc::operation_canceled);
      if(!same || r->source_error || !r->gpd->native_before_header_valid || !r->semantic_use ||
          !r->semantic_use->unchanged() || !r->selected_source || !r->gpd->opened_view ||
          (!r->gpd->open_receipt && !(r->gpd->cached_before_file && !r->gpd->image_stream_entered)) ||
          !r->gpd->opened_handle || r->gpd->opened_handle==UINT32_MAX || r->publication)return;
      source=r->selected_source;before_view=r->gpd->opened_view;receipt=r->gpd->open_receipt;
      cached=r->gpd->cached_before_file;new_open=r->gpd->image_stream_entered;
      before_header=r->gpd->native_before_header_index;handle=r->gpd->opened_handle;title=r->gpd->normalized_title;
    }
    // The real SYSTEM-loaded FB producer, actual selected getter, owned native
    // CS/use, immutable ingress and complete before-index now agree. TITLE is
    // still its genuine original caller when synchronous: no fake Task/process
    // or generic read-to-writer conversion is introduced.
    const auto file=s.kernel->object_table()->LookupObject<XFile>(handle);
    std::error_code error;const auto current=file && file->file()?
        (!new_open && cached && file.get()==cached.get() ?
          NativeProfileSaveManagedGpd::TryCaptureCurrentFileRead(*file->file(),source->writer().model,title,error):
          receipt ? NativeProfileSaveManagedGpd::TryCaptureOpenedRead(*file->file(),receipt,error):nullptr):nullptr;
    if(!current || current!=before_view){std::lock_guard lock(s.requests_mutex);
      r->source_error=error?error:Error(std::errc::bad_file_descriptor);return;}
    std::shared_ptr<NativeProfileSaveManagedGpd> service;
    {std::unique_lock prepare(s.preparation,std::try_to_lock);
      if(!prepare.owns_lock()){std::lock_guard lock(s.requests_mutex);r->source_error=Error(std::errc::device_or_resource_busy);return;}
      for(const auto& candidate:s.gpd)if(candidate->title_==title &&
          candidate->source_->MatchesMetadata(*source->writer().model->storage())){
        if(service){std::lock_guard lock(s.requests_mutex);r->source_error=Error(std::errc::permission_denied);return;}
        service=candidate;
      }
    }
    if(!service){std::lock_guard lock(s.requests_mutex);r->source_error=Error(std::errc::no_such_device);return;}
    NativeProfileSavePublication::SystemFileIssuer issuer;
    issuer.source=source;issuer.actual_before_file=before_view;issuer.requested_device=file->file()->retained_device();
    issuer.domain=s.config.domain->id();issuer.root_generation=s.config.domain->root().generation;
    issuer.invocation=r->invocation;issuer.title=title;
    issuer.actual_semantic_use=r->semantic_use;
    const auto publication=NativeProfileSavePublication::PrepareSystemFile(std::move(issuer),error);
    if(!publication){std::lock_guard lock(s.requests_mutex);r->source_error=error;return;}
    // Retain the exact publication in the existing request registry BEFORE
    // binding any real File. No original native memory/ref/Complete is owned.
    {std::lock_guard lock(s.requests_mutex);r->publication=publication;
      if(r->uncertain || !r->semantic_use->unchanged() || r->release_entered || !r->gpd->image_lock_retained){
        r->source_error=Error(std::errc::operation_canceled);return;}}
    auto cut=std::make_shared<ManagedFileCut>();cut->kind=ManagedFileCut::Kind::kBindSystem;
    cut->file=file;cut->service=std::move(service);cut->expected=std::move(before_view);
    cut->publication=publication;cut->actual_native_header_index=std::move(before_header);
    {std::lock_guard lock(s.requests_mutex);r->gpd->write_service=cut->service;}
    const auto status=PrepareManagedFileCut(cut);
    {std::lock_guard lock(s.requests_mutex);r->gpd->image_bind_attempted=true;r->gpd->image_bind_status=status;
      if(XFAILED(status))r->source_error=Error(std::errc::operation_not_permitted);}
    // No physical stage, write or extra observer wait occurs here. The SAME
    // existing File/IO22 mutation owns first stage creation/adoption and real
    // bytes. Final original checkpoint/header/durability/outcome/21F integration
    // remains mandatory before these observations/imports can be activated.
  }catch(...){if(r){std::lock_guard lock(s.requests_mutex);r->uncertain=true;}}
}
void NativeProfileSaveOwner::ObserveOriginalImageEditReturn(void* raw,uint32_t function) noexcept {
  if(!raw || global_critical_region::is_held_by_current_thread())return;
  auto& s=*state_;State::Observation observation(s);if(!observation)return;
  std::shared_ptr<State::Request> r;
  try {
    if(function!=0x81925AC0 && function!=0x81925110)return;
    if(!s.CodeCurrent(function))return;
    auto& c=*static_cast<cpu::ppc::PPCContext*>(raw);r=s.CurrentInner(c);
    if(!r)return;if(r->site.message==0xB0037){s.ParentGpdEditReturn(raw,function,r);return;}
    if(r->site.message!=0xB0031)return;
    const auto frame=ActualSaveFrame(*s.kernel,c,function);const auto result=Dword(c.r[3]);
    std::shared_ptr<NativeProfileSavePublication> publication;std::shared_ptr<NativeProfileSaveManagedGpd> service;
    std::shared_ptr<NativeProfileGpdReadView> expected;std::vector<uint8_t> before;
    uint32_t stream=0,handle=0;bool may_noop=false;
    {std::lock_guard lock(s.requests_mutex);
      if(!frame || !result || !r->gpd->image_edit_entered || r->gpd->image_edit_returned || r->gpd->image_edit!=*frame ||
          !r->gpd->image_lock_retained || r->gpd->image_release_entered || r->release_entered)return;
      r->gpd->image_edit_result=*result;r->gpd->image_edit_returned=true;publication=r->publication;
      if(publication)publication->CloseOriginalByteMutation();
      may_noop=publication && int32_t(*result)>=0 && !r->gpd->stream_write_entered &&
          r->gpd->image_bind_attempted && XSUCCEEDED(r->gpd->image_bind_status) && r->semantic_use && r->semantic_use->unchanged();
      if(may_noop){service=r->gpd->write_service;expected=r->gpd->opened_view;before=r->gpd->native_before_header_index;
        stream=r->gpd->native_stream;handle=r->gpd->opened_handle;}
    }
    if(may_noop && service && expected && !before.empty()) {
      std::array<uint8_t,16> prefix;
      if(!internal::ReadNativeWaitSnapshot(*s.kernel->memory(),stream,prefix) ||
          BigWord(prefix)!=0x81630230 || BigWord(prefix,12)!=handle || BigWord(prefix,8)!=before.size())return;
      std::vector<uint8_t> actual(before.size());
      if(ReadOwnedPayload(*s.kernel->memory(),BigWord(prefix,4),actual)!=X_ERROR_SUCCESS || actual!=before)return;
      const auto file=s.kernel->object_table()->LookupObject<XFile>(handle);if(!file)return;
      auto cut=std::make_shared<ManagedFileCut>();cut->kind=ManagedFileCut::Kind::kCancelPristine;
      cut->file=file;cut->service=service;cut->expected=expected;cut->publication=publication;
      cut->actual_native_header_index=std::move(actual);
      {std::lock_guard lock(s.requests_mutex);
        if(r->uncertain || !r->semantic_use->unchanged() || r->release_entered)return;
        r->gpd->pristine_noop_cut=cut;}
      // Attempt the same try-only metadata cut before the next native edit can
      // run. Registry owns it first; a busy result remains for the HOST reaper.
      const auto settled=PrepareManagedFileCut(cut);
      std::shared_ptr<ManagedFileCut> dropped;
      if(XSUCCEEDED(settled)){std::lock_guard lock(s.requests_mutex);
        if(r->gpd->pristine_noop_cut==cut){r->publication_settled=true;dropped=std::move(r->gpd->pristine_noop_cut);}}
      dropped.reset();s.changed.notify_all();
    }
    // Original HRESULT/notification/DB/getter cleanup are unchanged. A no-op
    // settles only after actual no-stage/no-journal File/model cancellation;
    // failed try admission remains an exact retained metadata retry packet.
  }catch(...){if(r){std::lock_guard lock(s.requests_mutex);r->uncertain=true;}}
}
void NativeProfileSaveOwner::ObserveOriginalImageInsertEntry(void* raw) noexcept {
  if(!raw || global_critical_region::is_held_by_current_thread())return;
  auto& s=*state_;State::Observation observation(s);if(!observation)return;
  std::shared_ptr<State::Request> r;
  try {
    if(!s.CodeCurrent(0x81925248) || !s.CodeCurrent(0x81925AC0))return;
    auto& c=*static_cast<cpu::ppc::PPCContext*>(raw);r=s.CurrentInner(c);
    if(!r)return;const auto caller=s.CurrentGpdCaller(*r);if(!caller)return;
    SaveFrame edit;uint32_t db=0;std::optional<CriticalSnapshot> acquired;
    {std::lock_guard lock(s.requests_mutex);
      if(r->uncertain || !r->gpd->image_edit_entered || r->gpd->image_edit_returned ||
          r->gpd->image_edit.function!=0x81925AC0 || r->gpd->image_insert_entered ||
          !r->gpd->image_lock_retained || r->gpd->image_release_entered || r->release_entered)return;
      edit=r->gpd->image_edit;db=r->gpd->image_db_pointer;acquired=r->gpd->image_try_after;
    }
    const auto frame=ActualSaveFrame(*s.kernel,c,0x81925248);
    const auto& input=r->input->large();
    const uint32_t original_entry=caller->parent?r->gpd->edit_entry:edit.sp+0x58;
    const uint32_t original_buffer=caller->parent?r->gpd->edit_buffer:input.address;
    const uint32_t original_length=caller->parent?r->gpd->edit_length:input.declared_bytes;
    if(!frame || edit.sp<0xC0 || frame->sp!=edit.sp-0xC0 ||
        frame->id!=edit.id || frame->pcr!=edit.pcr ||
        (frame->lr!=0x81925B44 && frame->lr!=0x81925C1C) ||
        Dword(c.r[3])!=std::optional(db) || Dword(c.r[4])!=std::optional(original_entry) ||
        Dword(c.r[5])!=std::optional(original_buffer) || Dword(c.r[6])!=std::optional(original_length))return;
    const auto actual_lock=ReadCriticalSnapshot(*s.kernel->memory(),r->gpd->image_owned_cs);
    if(!actual_lock || !acquired || actual_lock->owner_kthread!=frame->thread->guest_object() ||
        actual_lock->recursion!=acquired->recursion || actual_lock->recursion<=0)return;
    std::lock_guard lock(s.requests_mutex);
    if(r->uncertain || r->gpd->image_insert_entered || !r->gpd->image_lock_retained || r->release_entered)return;
    r->gpd->image_insert=*frame;r->gpd->image_insert_entered=true;
  }catch(...){if(r){std::lock_guard lock(s.requests_mutex);r->uncertain=true;}}
}
void NativeProfileSaveOwner::ObserveOriginalImageInsertReturn(void* raw) noexcept {
  if(!raw || global_critical_region::is_held_by_current_thread())return;
  auto& s=*state_;State::Observation observation(s);if(!observation)return;
  std::shared_ptr<State::Request> r;
  try {
    if(!s.CodeCurrent(0x81925248))return;
    auto& c=*static_cast<cpu::ppc::PPCContext*>(raw);r=s.CurrentInner(c);
    if(!r)return;const auto caller=s.CurrentGpdCaller(*r);if(!caller)return;
    const auto frame=ActualSaveFrame(*s.kernel,c,0x81925248);const auto result=Dword(c.r[3]);
    std::lock_guard lock(s.requests_mutex);
    if(!frame || !result || !r->gpd->image_insert_entered || r->gpd->image_insert_returned ||
        r->gpd->image_insert!=*frame || !r->gpd->image_lock_retained || r->release_entered)return;
    r->gpd->image_insert_result=*result;r->gpd->image_insert_returned=true;
  }catch(...){if(r){std::lock_guard lock(s.requests_mutex);r->uncertain=true;}}
}
void NativeProfileSaveOwner::ObserveOriginalStreamWriteEntry(void* raw) noexcept {
  if(!raw || global_critical_region::is_held_by_current_thread())return;
  auto& s=*state_;State::Observation observation(s);if(!observation)return;
  std::shared_ptr<State::Request> r;
  try {
    if(!s.CodeCurrent(0x81923900) || !s.CodeCurrent(0x81722268))return;
    auto& c=*static_cast<cpu::ppc::PPCContext*>(raw);r=s.CurrentInner(c);
    if(!r)return;const auto caller=s.CurrentGpdCaller(*r);if(!caller)return;
    SaveFrame edit,insert;bool insert_live=false;uint32_t stream=0,db=0,handle=0;
    {std::lock_guard lock(s.requests_mutex);
      if(r->uncertain || !r->publication || !r->gpd->write_service || !r->gpd->image_bind_attempted ||
          XFAILED(r->gpd->image_bind_status) || !r->gpd->image_edit_entered || r->gpd->image_edit_returned ||
          !r->gpd->image_lock_retained || r->gpd->stream_write_entered || r->release_entered ||
          !r->semantic_use || !r->semantic_use->unchanged())return;
      edit=r->gpd->image_edit;stream=r->gpd->native_stream;db=r->gpd->image_db_pointer;handle=r->gpd->opened_handle;
      insert=r->gpd->image_insert;insert_live=r->gpd->image_insert_entered && !r->gpd->image_insert_returned;
    }
    const auto frame=ActualSaveFrame(*s.kernel,c,0x81923900);
    const auto buffer=Dword(c.r[5]),length=Dword(c.r[6]),offset=Dword(c.r[4]);
    if(!frame || !buffer || !length || !offset ||
        frame->id!=edit.id || frame->pcr!=edit.pcr || Dword(c.r[3])!=std::optional(stream) ||
        ReadWord(*s.kernel->memory(),db+0x2C)!=std::optional(stream) ||
        ReadWord(*s.kernel->memory(),stream+0x0C)!=std::optional(handle))return;
    const auto kind=save_detail::ClassifyImageWriteCheckpoint(edit.function,edit.sp,frame->sp,frame->lr,
        Dword(c.r[23])==std::optional(0u),insert_live,insert.sp,insert.lr);
    const bool final_header=kind==save_detail::ImageWriteCheckpoint::kHeader;
    const bool final_inplace=kind==save_detail::ImageWriteCheckpoint::kInPlace;
    if(!final_header && !final_inplace)return;
    if(final_header && (*offset || ReadWord(*s.kernel->memory(),stream+4)!=buffer ||
        ReadWord(*s.kernel->memory(),stream+8)!=length))return;
    const uint32_t original_buffer=caller->parent?r->gpd->edit_buffer:r->input->large().address;
    const uint32_t original_length=caller->parent?r->gpd->edit_length:r->input->large().declared_bytes;
    if(final_inplace && (*buffer!=original_buffer || *length!=original_length))return;
    std::lock_guard lock(s.requests_mutex);
    if(r->uncertain || r->gpd->stream_write_entered || !r->gpd->image_lock_retained || r->release_entered)return;
    r->gpd->stream_write=*frame;r->gpd->stream_write_buffer=*buffer;r->gpd->stream_write_length=*length;
    r->gpd->stream_write_offset=*offset;r->gpd->stream_write_entered=true;++r->gpd->stream_write_serial;
  }catch(...){if(r){std::lock_guard lock(s.requests_mutex);r->uncertain=true;}}
}
std::shared_ptr<const vfs::FileWriteCheckpointAuthority>
NativeProfileSaveOwner::PrepareWriteCheckpointForActualCut(XFile& file,
    std::shared_ptr<NativeProfileSaveManagedGpd> service,std::shared_ptr<NativeProfileSavePublication> publication,
    uint32_t title,uint32_t offset,std::vector<uint8_t> input,std::vector<uint8_t> header,
    std::shared_ptr<WriteOutcome>& outcome,std::error_code& error) {
  error.clear();
  if(!service || !publication || !title || input.size()>xam::GpdSnapshotStore::kMaximumBytes ||
      uint64_t(offset)+input.size()>xam::GpdSnapshotStore::kMaximumBytes ||
      header.size()<24 || header.size()>xam::GpdSnapshotStore::kMaximumBytes ||
      !file.file_ || !NativeProfileSaveManagedGpd::MatchesOwnedPublication(*file.file_,publication)){
    error=Error(std::errc::invalid_argument);return {};
  }
  auto owned_outcome=std::make_shared<WriteOutcome>();
  auto use=std::make_unique<State::WriteUse>(retain_object(&file),file.file_,std::move(service),std::move(publication),title,offset,
      std::move(input),std::move(header),owned_outcome);
  auto authority=std::make_shared<State::WriteAuthority>(std::move(use),owned_outcome);
  outcome=std::move(owned_outcome);return authority;
}
std::shared_ptr<const vfs::FileWriteCheckpointAuthority>
NativeProfileSaveOwner::PrepareHeaderWriteForActualCut(XFile& file,
    std::shared_ptr<NativeProfileSavePublication> publication,uint32_t offset,
    std::vector<uint8_t> input,std::shared_ptr<WriteOutcome>& outcome,std::error_code& error) {
  error.clear();
  if(!publication || !publication->header_issuer_ || !file.file_ || !file.is_synchronous_ || file.is_alertable_ ||
      publication->header_issuer_->actual_backing_file.get()!=&file ||
      input.size()>filesystem::content_mutation_wire::kMaximumMetadataBytes ||
      uint64_t(offset)+input.size()>publication->header_issuer_->actual_native_before_header.size()) {
    error=Error(std::errc::invalid_argument);return {};
  }
  std::error_code current;if(!publication->CurrentForStage(current)){error=current;return {};}
  auto owned_outcome=std::make_shared<WriteOutcome>();
  auto use=std::make_unique<State::HeaderWriteUse>(retain_object(&file),file.file_,std::move(publication),offset,
      std::move(input),owned_outcome);
  auto authority=std::make_shared<State::WriteAuthority>(std::move(use),owned_outcome);
  outcome=std::move(owned_outcome);return authority;
}
std::shared_ptr<const vfs::FileWriteCheckpointAuthority> NativeProfileSaveOwner::PreparePictureWriteForActualCut(
    XFile& file,std::shared_ptr<NativeProfileSavePublication> publication,filesystem::ProfileArtifactKind kind,
    uint32_t offset,std::vector<uint8_t> input,std::shared_ptr<WriteOutcome>& outcome,std::error_code& error) {
  error.clear();
  if(!publication || !publication->parent_operation_ || !file.file_ || !file.is_synchronous_ || file.is_alertable_ ||
      input.empty() || input.size()>xam::GpdSnapshotStore::kMaximumBytes ||
      !NativeProfileSavePictureStage::MatchesOwnedFile(*file.file_,publication,kind)) {
    error=Error(std::errc::invalid_argument);return {};
  }
  if(!publication->CurrentForStage(error))return {};
  auto owned=std::make_shared<WriteOutcome>();
  auto use=std::make_unique<State::PictureWriteUse>(retain_object(&file),file.file_,std::move(publication),kind,offset,std::move(input),owned);
  auto authority=std::make_shared<State::WriteAuthority>(std::move(use),owned);outcome=std::move(owned);return authority;
}
std::optional<X_STATUS> NativeProfileSaveOwner::CaptureParentPictureCopyWrite(KernelState* kernel,XFile& file,
    uint32_t buffer,uint32_t length,std::shared_ptr<const vfs::FileWriteCheckpointAuthority>& output) {
  if(global_critical_region::is_held_by_current_thread())return {};
  const auto owner=Lookup(kernel);if(!owner || !kernel || !XThread::IsInThread())return {};
  auto& s=*owner->state_;State::Observation observation(s);if(!observation)return X_STATUS_CANCELLED;
  auto& c=*XThread::GetCurrentThread()->thread_state()->context();const auto r=s.CurrentInner(c);
  if(!r || r->site.message!=0xB0037)return {};
  SaveFrame copy;filesystem::ProfileArtifactKind source,target;
  {std::lock_guard held(s.requests_mutex);
    if(!r->picture_copy || r->picture_copy->returned)return {};
    if(r->uncertain || r->source_error || r->release_entered || !r->publication || !r->semantic_use ||
        !r->semantic_use->unchanged() || output)return X_STATUS_CANCELLED;
    const auto& prior=r->picture_copy->last_output;
    if(prior && prior->phase.load(std::memory_order_acquire)!=WriteOutcome::Phase::kStagedAwaitingWholeCheckpoint)
      return X_STATUS_CANCELLED;
    copy=r->picture_copy->entry;source=r->picture_copy->source;target=r->picture_copy->target;
  }
  const auto frame=ActualSaveFrame(*kernel,c,0x8171BA28);
  if(!frame || !s.CodeCurrent(0x8171BA28) || copy.sp<0x150 || frame->sp!=copy.sp-0x150 ||
      frame->id!=copy.id || frame->pcr!=copy.pcr || frame->lr!=0x8171BC84 ||
      ReadWord(*kernel->memory(),frame->sp)!=std::optional(copy.sp) ||
      ReadWord(*kernel->memory(),copy.sp-8)!=std::optional(copy.lr) ||
      Dword(c.r[4])!=std::optional(0u) || Dword(c.r[5])!=std::optional(0u) || Dword(c.r[6])!=std::optional(0u) ||
      Dword(c.r[7])!=std::optional(frame->sp+0x70) || Dword(c.r[8])!=std::optional(buffer) ||
      Dword(c.r[9])!=std::optional(length) || Dword(c.r[10])!=std::optional(frame->sp+0x78) ||
      !length || length>0x10000)return X_STATUS_ACCESS_DENIED;
  std::array<uint8_t,8> offset;
  if(!internal::ReadNativeWaitSnapshot(*kernel->memory(),frame->sp+0x78,offset) || BigWord(offset) ||
      ReadWord(*kernel->memory(),frame->sp+0x70)!=std::optional(uint32_t(X_STATUS_SUCCESS)) ||
      ReadWord(*kernel->memory(),frame->sp+0x74)!=std::optional(length))return X_STATUS_ACCESS_DENIED;
  const auto source_handle=ReadWord(*kernel->memory(),frame->sp+0x60),target_handle=ReadWord(*kernel->memory(),frame->sp+0x64);
  const auto caller_handle=Dword(c.r[3]);
  if(!source_handle || !target_handle || !caller_handle || *target_handle!=*caller_handle)return X_STATUS_INVALID_HANDLE;
  const auto source_file=kernel->object_table()->LookupObject<XFile>(*source_handle);
  // Exact actual source/target backends already opened by this original
  // scoped guest operation. Its source is a read-only view of the prior PNG
  // output; this does not hash/reread bytes or convert another source by path.
  constexpr uint32_t mutation_access=vfs::FileAccess::kGenericWrite|vfs::FileAccess::kGenericAll|0x000D0156;
  if(!source_file || !source_file->file() || (source_file->file_access()&mutation_access) ||
      !NativeProfileSavePictureStage::MatchesOwnedFile(*source_file->file(),r->publication,source) ||
      !file.file() || !NativeProfileSavePictureStage::MatchesOwnedFile(*file.file(),r->publication,target))return X_STATUS_ACCESS_DENIED;
  std::vector<uint8_t> input(length);
  if(ReadOwnedPayload(*kernel->memory(),buffer,input)!=X_ERROR_SUCCESS)return X_STATUS_ACCESS_VIOLATION;
  std::error_code error;std::shared_ptr<WriteOutcome> outcome;
  auto authority=PreparePictureWriteForActualCut(file,r->publication,target,BigWord(offset,4),std::move(input),outcome,error);
  if(!authority)return vfs::HostStorageError(error);
  {std::lock_guard held(s.requests_mutex);
    if(r->uncertain || !r->picture_copy || r->picture_copy->returned || r->picture_copy->entry!=copy)return X_STATUS_CANCELLED;
    // Retain only this actual current synchronous transfer. Successful short
    // reads can cause any number of legitimate loop iterations; no arbitrary
    // historical-transfer cap rejects them or accumulates unbounded receipts.
    // The genuine existing package IO cut preserves actual earlier failures;
    // each of the four copy own-returns independently retains its native BOOL.
    r->picture_copy->last_output=std::move(outcome);
  }
  output=std::move(authority);return X_STATUS_SUCCESS;
}
std::optional<X_STATUS> NativeProfileSaveOwner::CaptureParentPictureWrite(KernelState* kernel,XFile& file,
    uint32_t buffer,uint32_t length,std::shared_ptr<const vfs::FileWriteCheckpointAuthority>& output) {
  if(global_critical_region::is_held_by_current_thread())return {};
  const auto owner=Lookup(kernel);if(!owner || !kernel || !XThread::IsInThread())return {};
  auto& s=*owner->state_;State::Observation observation(s);if(!observation)return X_STATUS_CANCELLED;
  auto& c=*XThread::GetCurrentThread()->thread_state()->context();const auto r=s.CurrentInner(c);
  if(!r || r->site.message!=0xB0037 || !r->publication || !r->publication->parent_operation_)return {};
  SaveFrame call;filesystem::ProfileArtifactKind kind;uint32_t original_buffer=0,original_length=0;
  {std::lock_guard held(s.requests_mutex);
    if(!r->picture_call || r->picture_call->returned)return {};
    if(r->uncertain || r->source_error || r->picture_call->output || output ||
        !r->semantic_use || !r->semantic_use->unchanged())return X_STATUS_CANCELLED;
    call=r->picture_call->entry;kind=r->picture_call->kind;
    original_buffer=r->picture_call->buffer;original_length=r->picture_call->length;
  }
  const auto frame=ActualSaveFrame(*kernel,c,0x81722268);
  if(!s.CodeCurrent(0x81722268) || !frame || call.sp<0x120 || frame->sp!=call.sp-0x120 ||
      frame->id!=call.id || frame->pcr!=call.pcr || frame->lr!=0x81722324 ||
      ReadWord(*kernel->memory(),frame->sp+0x78)!=std::optional(0x8191B9DCu) ||
      Dword(c.r[4])!=std::optional(0u) ||
      Dword(c.r[5])!=std::optional(0u) || Dword(c.r[6])!=std::optional(0u) ||
      Dword(c.r[7])!=std::optional(frame->sp+0x50) || Dword(c.r[8])!=std::optional(buffer) ||
      Dword(c.r[9])!=std::optional(length) || Dword(c.r[10])!=std::optional(0u) ||
      buffer!=original_buffer || length!=original_length)return X_STATUS_ACCESS_DENIED;
  std::vector<uint8_t> input(length);
  if(ReadOwnedPayload(*kernel->memory(),buffer,input)!=X_ERROR_SUCCESS)return X_STATUS_ACCESS_VIOLATION;
  const auto& original=kind==filesystem::ProfileArtifactKind::kAvatar64 || kind==filesystem::ProfileArtifactKind::kPublic64 ||
      kind==filesystem::ProfileArtifactKind::kPersonal64?r->input->large():r->input->small();
  if(input!=original.bytes)return X_STATUS_CANCELLED;
  std::error_code error;std::shared_ptr<WriteOutcome> outcome;
  auto authority=PreparePictureWriteForActualCut(file,r->publication,kind,0,std::move(input),outcome,error);
  if(!authority)return vfs::HostStorageError(error);
  {std::lock_guard held(s.requests_mutex);
    if(r->uncertain || !r->picture_call || r->picture_call->entry!=call || r->picture_call->returned || r->picture_call->output)
      return X_STATUS_CANCELLED;
    if(r->picture_write_outputs.size()>=2)return X_STATUS_INSUFFICIENT_RESOURCES;
    r->picture_write_outputs.push_back(outcome);r->picture_call->output=std::move(outcome);
  }
  output=std::move(authority);return X_STATUS_SUCCESS;
}
std::optional<X_STATUS> NativeProfileSaveOwner::CaptureParentHeaderWrite(KernelState* kernel,XFile& file,
    uint32_t buffer,uint32_t length,std::shared_ptr<const vfs::FileWriteCheckpointAuthority>& output) {
  if(global_critical_region::is_held_by_current_thread())return {};
  const auto owner=Lookup(kernel);if(!owner || !kernel || !XThread::IsInThread())return {};
  auto& s=*owner->state_;State::Observation observation(s);if(!observation)return X_STATUS_CANCELLED;
  auto& c=*XThread::GetCurrentThread()->thread_state()->context();auto r=s.CurrentHeader(c);
  const bool at_close=!r;const auto content=at_close?s.CurrentContent(c):State::ContentUse{};if(at_close)r=content.request;const auto close=content.operation;
  if(!r || !r->publication || !r->publication->parent_operation_)return {};
  SaveFrame write;uint32_t context=0,handle=0;object_ref<XFile> backing;
  {std::lock_guard held(s.requests_mutex);
    const auto& entry=at_close?close->write:r->header_child->write;
    if(!entry)return {};
    if(r->uncertain || entry->returned || entry->output || r->source_error ||
        !r->semantic_use || !r->semantic_use->unchanged() || output)return X_STATUS_CANCELLED;
    if(at_close) {
      if(close->uncertain || !close->header_input_bound || close->context_returned)return X_STATUS_CANCELLED;
      context=close->context;backing=close->backing;
    }else {
      const auto& child=r->header_child;if(child->uncertain || !child->before_captured || child->worker_returned)return X_STATUS_CANCELLED;
      context=child->context;backing=child->backing;
    }
    write=entry->entry;
  }
  const auto frame=ActualSaveFrame(*kernel,c,0x81722268);
  const auto original_handle=ReadWord(*kernel->memory(),context+0x14);if(original_handle)handle=*original_handle;
  // H971A has no extra bytes: this recovered C628 lane issues one prefix
  // write. It hashes the retained alignment bytes without rewriting them.
  if(!s.CodeCurrent(0x81722268) || !frame || write.sp<0x140 || frame->sp!=write.sp-0x140 ||
      frame->id!=write.id || frame->pcr!=write.pcr || frame->lr!=0x81722324 ||
      ReadWord(*kernel->memory(),frame->sp+0x78)!=std::optional(0x8167C740u) ||
      Dword(c.r[3])!=std::optional(handle) || Dword(c.r[4])!=std::optional(0u) ||
      Dword(c.r[5])!=std::optional(0u) || Dword(c.r[6])!=std::optional(0u) ||
      Dword(c.r[7])!=std::optional(frame->sp+0x50) || Dword(c.r[8])!=std::optional(buffer) ||
      Dword(c.r[9])!=std::optional(length) || Dword(c.r[10])!=std::optional(0u) ||
      buffer!=context+0x38 || length<0x344 || length>0x971A ||
      !file.is_synchronous_ || file.is_alertable_ || backing.get()!=&file)return X_STATUS_ACCESS_DENIED;
  const auto lock=ReadCriticalSnapshot(*kernel->memory(),context+0x1C);
  if(!lock || lock->owner_kthread!=frame->thread->guest_object() || lock->recursion<=0)return X_STATUS_ACCESS_DENIED;
  std::vector<uint8_t> input(length);
  if(ReadOwnedPayload(*kernel->memory(),buffer,input)!=X_ERROR_SUCCESS)return X_STATUS_ACCESS_VIOLATION;
  std::error_code error;std::shared_ptr<WriteOutcome> outcome;
  auto authority=PrepareHeaderWriteForActualCut(file,r->publication,0,std::move(input),outcome,error);
  if(!authority)return vfs::HostStorageError(error);
  {std::lock_guard held(s.requests_mutex);
    auto& entry=at_close?close->write:r->header_child->write;
    auto& outputs=at_close?close->write_outputs:r->header_child->write_outputs;
    if(r->uncertain || !entry || entry->entry!=write || entry->returned || entry->output)return X_STATUS_CANCELLED;
    if(outputs.size()>=2)return X_STATUS_INSUFFICIENT_RESOURCES;
    outputs.push_back(outcome);entry->output=std::move(outcome);
  }
  output=std::move(authority);return X_STATUS_SUCCESS;
}
std::optional<X_STATUS> NativeProfileSaveOwner::CaptureWriteCheckpoint(KernelState* kernel,XFile& file,
    uint32_t buffer,uint32_t length,std::shared_ptr<const vfs::FileWriteCheckpointAuthority>& output) noexcept try {
  // No new permission or buffer observation under an inherited host GCRC.
  // Missing checkpoint remains unavailable and cannot count as native SAVE.
  if(global_critical_region::is_held_by_current_thread())return {};
  const auto copy=CaptureParentPictureCopyWrite(kernel,file,buffer,length,output);if(copy)return copy;
  const auto picture=CaptureParentPictureWrite(kernel,file,buffer,length,output);if(picture)return picture;
  const auto header=CaptureParentHeaderWrite(kernel,file,buffer,length,output);if(header)return header;
  const auto owner=Lookup(kernel);if(!owner || !kernel || !XThread::IsInThread())return {};
  auto& s=*owner->state_;State::Observation observation(s);if(!observation)return X_STATUS_CANCELLED;
  auto* thread=XThread::GetCurrentThread();auto& c=*thread->thread_state()->context();
  const auto r=s.CurrentInner(c);if(!r)return {};const auto caller=s.CurrentGpdCaller(*r);if(!caller)return {};
  SaveFrame stream;uint32_t native_stream=0,handle=0,offset=0,title=0;
  std::shared_ptr<NativeProfileSavePublication> publication;
  std::shared_ptr<NativeProfileSaveManagedGpd> service;
  {
    std::lock_guard lock(s.requests_mutex);
    if(!r->gpd->stream_write_entered || r->gpd->stream_write_returned)return {};
    if(output || r->uncertain || r->gpd->stream_checkpoint_captured || !r->gpd->image_lock_retained ||
        r->release_entered || !r->publication || !r->gpd->write_service || !r->semantic_use ||
        !r->semantic_use->unchanged() || r->source_error || s.closed->load(std::memory_order_acquire))return X_STATUS_CANCELLED;
    stream=r->gpd->stream_write;native_stream=r->gpd->native_stream;handle=r->gpd->opened_handle;
    offset=r->gpd->stream_write_offset;title=r->gpd->normalized_title;publication=r->publication;service=r->gpd->write_service;
    if(buffer!=r->gpd->stream_write_buffer || length!=r->gpd->stream_write_length)return X_STATUS_ACCESS_DENIED;
  }
  // Exact genuine STREAM Write→Win32 WriteFile(null overlap)→NtWriteFile FF.
  // No equivalent host filename, Task, slot or separately supplied File qualifies.
  if(!s.CodeCurrent(0x81923900) || !s.CodeCurrent(0x81722268))return X_STATUS_ACCESS_DENIED;
  const auto frame=ActualSaveFrame(*kernel,c,0x81722268);
  if(!frame || stream.sp<0x100 || frame->sp!=stream.sp-0x100 || frame->lr!=0x81722324 ||
      frame->id!=stream.id || frame->pcr!=stream.pcr || Dword(c.r[3])!=std::optional(handle) ||
      Dword(c.r[4])!=std::optional(0u) || Dword(c.r[5])!=std::optional(0u) ||
      Dword(c.r[6])!=std::optional(0u) || Dword(c.r[7])!=std::optional(frame->sp+0x50) ||
      Dword(c.r[8])!=std::optional(buffer) || Dword(c.r[9])!=std::optional(length) ||
      Dword(c.r[10])!=std::optional(0u) || !file.is_synchronous_ || file.is_alertable_ ||
      !file.file_ || length>xam::GpdSnapshotStore::kMaximumBytes)return X_STATUS_ACCESS_DENIED;
  const auto actual=kernel->object_table()->LookupObject<XFile>(handle);
  if(!actual || actual.get()!=&file || !NativeProfileSaveManagedGpd::MatchesOwnedPublication(*file.file_,publication))return X_STATUS_ACCESS_DENIED;
  std::array<uint8_t,16> prefix;
  if(!internal::ReadNativeWaitSnapshot(*kernel->memory(),native_stream,prefix) || BigWord(prefix)!=0x81630230 ||
      BigWord(prefix,12)!=handle || !BigWord(prefix,4) || BigWord(prefix,8)<24 ||
      BigWord(prefix,8)>xam::GpdSnapshotStore::kMaximumBytes)return X_STATUS_ACCESS_DENIED;
  std::vector<uint8_t> owned_input(length),header(BigWord(prefix,8));
  if(ReadOwnedPayload(*kernel->memory(),buffer,owned_input)!=X_ERROR_SUCCESS ||
      ReadOwnedPayload(*kernel->memory(),BigWord(prefix,4),header)!=X_ERROR_SUCCESS)return X_STATUS_ACCESS_VIOLATION;
  std::error_code error;if(!publication->CurrentForStage(error))return vfs::HostStorageError(error);
  std::shared_ptr<WriteOutcome> outcome;
  auto authority=PrepareWriteCheckpointForActualCut(file,service,publication,title,offset,
      std::move(owned_input),std::move(header),outcome,error);
  if(!authority)return vfs::HostStorageError(error);
  {std::lock_guard lock(s.requests_mutex);
    if(r->uncertain || r->gpd->stream_checkpoint_captured || !r->semantic_use->unchanged() ||
        !r->gpd->image_lock_retained || r->release_entered)return X_STATUS_CANCELLED;
    r->gpd->stream_checkpoint_captured=true;r->gpd->write_outcome=std::move(outcome);
  }
  // The common NtWrite closure carries this one-shot owning value. No State
  // observation borrow, translated guest buffer, guest output or callback
  // owner survives this original caller capture.
  output=std::move(authority);return X_STATUS_SUCCESS;
}catch(...){return X_STATUS_UNSUCCESSFUL;}
void NativeProfileSaveOwner::ObserveOriginalStreamWriteReturn(void* raw) noexcept {
  if(!raw || global_critical_region::is_held_by_current_thread())return;
  auto& s=*state_;State::Observation observation(s);if(!observation)return;
  std::shared_ptr<State::Request> r;
  try {
    if(!s.CodeCurrent(0x81923900))return;
    auto& c=*static_cast<cpu::ppc::PPCContext*>(raw);r=s.CurrentInner(c);if(!r)return;
    const auto frame=ActualSaveFrame(*s.kernel,c,0x81923900);const auto value=Dword(c.r[3]);
    std::lock_guard lock(s.requests_mutex);
    if(!frame || !value || !r->gpd->stream_write_entered || r->gpd->stream_write_returned || r->gpd->stream_write!=*frame)return;
    r->gpd->stream_write_returned=true;
    if(r->gpd->write_outcome && r->gpd->write_outcome->phase.load(std::memory_order_acquire)==WriteOutcome::Phase::kReady) {
      if(int32_t(*value)>=0)r->publication_settled=true;
      else r->uncertain=true; // Storage readiness is not invented native success.
    }
  }catch(...){if(r){std::lock_guard lock(s.requests_mutex);r->uncertain=true;}}
}
void NativeProfileSaveOwner::ObserveOriginalImageFileOpenEntry(void* raw) noexcept {
  if(!raw || global_critical_region::is_held_by_current_thread())return;
  auto& s=*state_;State::Observation observation(s);if(!observation)return;
  std::shared_ptr<State::Request> r;
  try {
    if(!s.CodeCurrent(0x81924010))return;
    auto& c=*static_cast<cpu::ppc::PPCContext*>(raw);r=s.CurrentInner(c);
    if(!r)return;const auto caller=s.CurrentGpdCaller(*r);if(!caller)return;
    SaveFrame ensure;uint32_t db=0,cs=0;
    {std::lock_guard lock(s.requests_mutex);if(!r->gpd->image_ensure_entered || r->gpd->image_file_open_entered ||
        !r->gpd->image_lock_retained || r->gpd->image_release_entered || r->release_entered)return;
      ensure=r->gpd->image_ensure;db=r->gpd->image_db_pointer;cs=r->gpd->image_owned_cs;}
    const auto frame=ActualSaveFrame(*s.kernel,c,0x81924010);
    const uint32_t depth=ensure.function==0x81928750?0x70u:0u;
    const uint32_t lr=depth?0x819287C8u:0x81910248u;
    const auto flags=ReadWord(*s.kernel->memory(),db+0x24);
    if(!frame || ensure.sp<depth || frame->sp!=ensure.sp-depth || frame->lr!=lr || !flags ||
        Dword(c.r[3])!=std::optional(db) || Dword(c.r[4])!=std::optional(db+0x3C) ||
        Dword(c.r[5])!=std::optional(0xC0000000u) || Dword(c.r[6])!=flags ||
        ReadWord(*s.kernel->memory(),db+4)!=std::optional(cs))return;
    std::lock_guard lock(s.requests_mutex);
    if(r->uncertain || r->gpd->image_file_open_entered || !r->gpd->image_lock_retained || r->release_entered)return;
    r->gpd->image_file_open=*frame;r->gpd->image_open_flags=*flags;r->gpd->image_file_open_entered=true;
    // Original24010 still allocates/constructs the real stream and performs
    // its normal open/header reads. No fake file handle or header is supplied.
  }catch(...){if(r){std::lock_guard lock(s.requests_mutex);r->uncertain=true;}}
}
void NativeProfileSaveOwner::ObserveOriginalImageStreamEntry(void* raw) noexcept {
  if(!raw || global_critical_region::is_held_by_current_thread())return;
  auto& s=*state_;State::Observation observation(s);if(!observation)return;
  std::shared_ptr<State::Request> r;
  try {
    if(!s.CodeCurrent(0x819237F8) || !s.CodeCurrent(0x81924010))return;
    auto& c=*static_cast<cpu::ppc::PPCContext*>(raw);r=s.CurrentInner(c);
    if(!r)return;const auto caller=s.CurrentGpdCaller(*r);if(!caller)return;
    SaveFrame open;uint32_t db=0,flags=0;
    {std::lock_guard lock(s.requests_mutex);if(!r->gpd->image_file_open_entered || r->gpd->image_stream_entered ||
        !r->gpd->image_lock_retained || r->gpd->image_release_entered || r->release_entered)return;
      open=r->gpd->image_file_open;db=r->gpd->image_db_pointer;flags=r->gpd->image_open_flags;}
    const auto frame=ActualSaveFrame(*s.kernel,c,0x819237F8);const auto stream=Dword(c.r[3]);
    if(!frame || open.sp<0x90 || frame->sp!=open.sp-0x90 || frame->lr!=0x819240BC ||
        !stream || !*stream || (*stream&3) || uint64_t(*stream)+0x10>uint64_t(UINT32_MAX)+1 ||
        Dword(c.r[4])!=std::optional(db+0x3C) || Dword(c.r[5])!=std::optional(0xC0000000u) ||
        Dword(c.r[6])!=std::optional(flags) || ReadWord(*s.kernel->memory(),db+0x2C)!=stream)return;
    std::array<uint8_t,16> prefix;
    if(!internal::ReadNativeWaitSnapshot(*s.kernel->memory(),*stream,prefix) ||
        BigWord(prefix)!=0x81630230 || BigWord(prefix,4) || BigWord(prefix,8) || BigWord(prefix,12)!=UINT32_MAX)return;
    std::lock_guard lock(s.requests_mutex);
    if(r->uncertain || r->gpd->image_stream_entered || !r->gpd->image_lock_retained || r->release_entered)return;
    r->gpd->image_stream_open=*frame;r->gpd->native_stream=*stream;r->gpd->image_stream_entered=true;
    // The bound original caller just constructed THIS new stream and supplied
    // its initial invalid handle. A similar address or cached stream is not it.
  }catch(...){if(r){std::lock_guard lock(s.requests_mutex);r->uncertain=true;}}
}
std::optional<X_STATUS> NativeProfileSaveOwner::CaptureParentHeaderOpen(KernelState* kernel,
    std::string_view path,vfs::FileOpenContext& context) {
  if(global_critical_region::is_held_by_current_thread())return {};
  const auto owner=Lookup(kernel);if(!owner || !kernel || !XThread::IsInThread())return {};
  auto& s=*owner->state_;State::Observation observation(s);if(!observation)return X_STATUS_CANCELLED;
  auto& c=*XThread::GetCurrentThread()->thread_state()->context();const auto r=s.CurrentHeader(c);
  if(!r || !r->publication || !r->publication->parent_operation_)return {};
  const auto frame=ActualSaveFrame(*kernel,c,0x81680128);
  const auto& child=r->header_child;
  if(!s.CodeCurrent(0x81680128) || !frame || child->worker.sp<0x920 ||
      frame->sp!=child->worker.sp-0x920 || (frame->lr!=0x81680554 && frame->lr!=0x81680598) ||
      ReadWord(*kernel->memory(),frame->sp)!=std::optional(child->worker.sp-0x510) ||
      ReadWord(*kernel->memory(),child->worker.sp-0x510-8)!=std::optional(0x816838ACu) ||
      Dword(c.r[3])!=std::optional(frame->sp+0x50) ||
      Dword(c.r[5])!=std::optional(frame->sp+0x80) || Dword(c.r[6])!=std::optional(frame->sp+0x60) ||
      Dword(c.r[7])!=std::optional(3u) || Dword(c.r[8])!=std::optional(0x20u))return X_STATUS_ACCESS_DENIED;
  auto* fs=kernel->file_system();std::shared_ptr<vfs::Device> device;std::string translated,relative;
  {auto graph=global_critical_region::AcquireDirect();
    if(XFAILED(fs->TranslateSymbolicPath(path,translated)) || !(device=fs->AcquirePathDevice(translated)))return X_STATUS_OBJECT_PATH_NOT_FOUND;
    const auto& mount=device->mount_path();
    if(translated.size()<mount.size() || !xe::utf8::equal_case(std::string_view(translated).substr(0,mount.size()),mount) ||
        (translated.size()>mount.size() && !mount.ends_with('\\') && !mount.ends_with(':') && translated[mount.size()]!='\\'))return X_STATUS_ACCESS_DENIED;
    relative=translated.substr(mount.size());while(!relative.empty() && relative.front()=='\\')relative.erase(relative.begin());
  }
  if(context.managed_authority || (context.expected_device && context.expected_device!=device))return X_STATUS_ACCESS_DENIED;
  std::error_code error;
  auto duty=std::make_shared<State::HostHeaderOpenUse>(owner->state_,r);if(!duty->admitted)return X_STATUS_CANCELLED;
  auto authority=r->publication->IssueParentHeaderReadForActualOpen(device,relative,duty,error);
  if(!authority)return vfs::HostStorageError(error);
  context.expected_device=std::move(device);context.managed_authority=std::move(authority);return X_STATUS_SUCCESS;
}
std::optional<X_STATUS> NativeProfileSaveOwner::CaptureReadSource(KernelState* kernel,XFile& file) noexcept try {
  if(global_critical_region::is_held_by_current_thread())return {};
  const auto owner=Lookup(kernel);if(!owner || !kernel || !XThread::IsInThread())return {};
  auto& s=*owner->state_;State::Observation observation(s);if(!observation)return X_STATUS_CANCELLED;
  auto* thread=XThread::GetCurrentThread();auto& c=*thread->thread_state()->context();
  const auto frame=ActualSaveFrame(*kernel,c,0x816837A0);if(!frame)return {};
  const auto r=s.CurrentHeader(c);if(!r || !r->publication || !r->publication->parent_operation_)return {};
  std::unique_lock posted(file.posted_lock_,std::try_to_lock);
  if(!posted.owns_lock() || file.posted_running_ || file.posted_executing_ || !file.posted_.empty())return vfs::HostStorageError(Error(std::errc::device_or_resource_busy));
  std::unique_lock io(file.file_lock_,std::try_to_lock);
  if(!io.owns_lock())return vfs::HostStorageError(Error(std::errc::device_or_resource_busy));
  const auto package=file.file_?file.file_->folder_content_package():nullptr;if(!package)return {};
  std::error_code error;auto [policy,issued]=r->publication->CaptureParentHeaderRead(error);
  if(!policy || !issued)return vfs::HostStorageError(error);
  // The exact raw backend proves its CM policy/source/before tuple. This only
  // attaches read observation under the existing try-only File cut; the later
  // native context+CS+fullR issuer independently owns mutation adoption.
  return package->ObserveHeaderPublication(issued);
}catch(...){return X_STATUS_UNSUCCESSFUL;}
std::optional<X_STATUS> NativeProfileSaveOwner::CaptureParentPictureCopyOpen(KernelState* kernel,
    std::string_view path,vfs::FileOpenContext& context) {
  if(global_critical_region::is_held_by_current_thread())return {};
  const auto owner=Lookup(kernel);if(!owner || !kernel || !XThread::IsInThread())return {};
  auto& s=*owner->state_;State::Observation observation(s);if(!observation)return X_STATUS_CANCELLED;
  auto& c=*XThread::GetCurrentThread()->thread_state()->context();const auto r=s.CurrentInner(c);
  if(!r || r->site.message!=0xB0037)return {};
  SaveFrame copy;filesystem::ProfileArtifactKind source,target;
  {std::lock_guard held(s.requests_mutex);
    if(!r->picture_copy || r->picture_copy->returned)return {};
    if(r->uncertain || r->source_error || r->release_entered || !r->publication || !r->semantic_use || !r->semantic_use->unchanged())
      return X_STATUS_CANCELLED;
    copy=r->picture_copy->entry;source=r->picture_copy->source;target=r->picture_copy->target;
  }
  const auto frame=ActualSaveFrame(*kernel,c,0x8171BA28);
  if(!frame || !s.CodeCurrent(0x8171BA28) || copy.sp<0x150 || frame->sp!=copy.sp-0x150 ||
      frame->id!=copy.id || frame->pcr!=copy.pcr || ReadWord(*kernel->memory(),frame->sp)!=std::optional(copy.sp) ||
      ReadWord(*kernel->memory(),copy.sp-8)!=std::optional(copy.lr))return X_STATUS_ACCESS_DENIED;
  const bool reading=frame->lr==0x8171BADC;
  if(!reading && frame->lr!=0x8171BB94)return {};
  if(Dword(c.r[3])!=std::optional(frame->sp+(reading?0x60u:0x64u)) ||
      Dword(c.r[4])!=std::optional(reading?0x80100000u:0x40110000u) ||
      Dword(c.r[5])!=std::optional(frame->sp+0x88) || Dword(c.r[6])!=std::optional(frame->sp+0x70) ||
      ReadWord(*kernel->memory(),frame->sp+0x8C)!=std::optional(frame->sp+0x80) ||
      ReadWord(*kernel->memory(),frame->sp+0x84)!=std::optional(copy.sp+(reading?0x320u:0xA0u)))return X_STATUS_ACCESS_DENIED;
  std::array<uint8_t,1> first_path;
  if(!internal::ReadNativeWaitSnapshot(*kernel->memory(),copy.sp+(reading?0x320u:0xA0u),first_path))return X_STATUS_ACCESS_VIOLATION;
  // Matching BA28 uses genuine KeGetCurrentProcessType and the ORIGINAL
  // counted string's first byte: SYSTEM absolute paths use root0; all other
  // native cases use ObDosDevices(-3). Neither is a path-selected writer.
  const auto process=OriginalKernelProcessType(*kernel->memory(),*frame);if(!process)return X_STATUS_ACCESS_DENIED;
  const uint32_t expected_root=*process==X_PROCTYPE_SYSTEM && first_path[0]=='\\'?0u:0xFFFFFFFDu;
  if(ReadWord(*kernel->memory(),frame->sp+0x88)!=std::optional(expected_root))return X_STATUS_ACCESS_DENIED;
  if(reading) {
    if(Dword(c.r[7])!=std::optional(1u) || Dword(c.r[8])!=std::optional(0x60u))return X_STATUS_ACCESS_DENIED;
  }else if(Dword(c.r[7])!=std::optional(frame->sp+0xC8) || Dword(c.r[8])!=std::optional(0x80u) ||
      Dword(c.r[9])!=std::optional(0u) || Dword(c.r[10])!=std::optional(5u) ||
      ReadWord(*kernel->memory(),frame->sp+0x54)!=std::optional(0x64u))return X_STATUS_ACCESS_DENIED;
  auto* fs=kernel->file_system();std::string translated,relative;std::shared_ptr<vfs::Device> device;
  {auto graph=global_critical_region::AcquireDirect();
    if(XFAILED(fs->TranslateSymbolicPath(path,translated)) || !(device=fs->AcquirePathDevice(translated)))return X_STATUS_OBJECT_PATH_NOT_FOUND;
    const auto& mount=device->mount_path();
    if(translated.size()<mount.size() || !xe::utf8::equal_case(std::string_view(translated).substr(0,mount.size()),mount) ||
        (translated.size()>mount.size() && !mount.ends_with('\\') && !mount.ends_with(':') && translated[mount.size()]!='\\'))return X_STATUS_ACCESS_DENIED;
    relative=translated.substr(mount.size());while(!relative.empty() && relative.front()=='\\')relative.erase(relative.begin());
  }
  if(context.managed_authority || (context.expected_device && context.expected_device!=device))return X_STATUS_ACCESS_DENIED;
  std::shared_ptr<NativeProfileSaveManagedPicture> service;
  {std::unique_lock prepare(s.preparation,std::try_to_lock);if(!prepare.owns_lock())return X_STATUS(0xC00000E8);
    for(const auto& candidate:s.pictures)if(candidate->kind_==(reading?source:target) &&
        candidate->source_->MatchesMetadata(*r->selected_source->writer().model->storage())) {
      if(service)return X_STATUS_ACCESS_DENIED;service=candidate;
    }
  }
  if(!service)return X_STATUS_NOT_SUPPORTED;
  auto duty=std::make_shared<State::HostOpenUse>(owner->state_,r);if(!duty->admitted)return X_STATUS_CANCELLED;
  std::error_code error;auto authority=reading?
      service->IssueParentStageReadForActualOpen(r->publication,device,relative,duty,error):
      service->IssueParentForActualOpen(r->publication,device,relative,duty,error);
  if(!authority)return vfs::HostStorageError(error);
  context.expected_device=std::move(device);context.managed_authority=std::move(authority);return X_STATUS_SUCCESS;
}
std::optional<X_STATUS> NativeProfileSaveOwner::CaptureParentPictureOpen(KernelState* kernel,
    std::string_view path,vfs::FileOpenContext& context) {
  if(global_critical_region::is_held_by_current_thread())return {};
  const auto owner=Lookup(kernel);if(!owner || !kernel || !XThread::IsInThread())return {};
  auto& s=*owner->state_;State::Observation observation(s);if(!observation)return X_STATUS_CANCELLED;
  auto* thread=XThread::GetCurrentThread();auto& c=*thread->thread_state()->context();
  const auto r=s.CurrentInner(c);if(!r)return {};
  State::Request::PictureFileCall call;std::shared_ptr<NativeProfileSavePublication> publication;
  {
    std::lock_guard lock(s.requests_mutex);
    if(r->site.message!=0xB0037 || !r->picture_call || r->picture_call->returned)return {};
    if(r->uncertain || r->release_entered || !r->publication || !r->publication->parent_operation_ ||
        !r->semantic_use || !r->semantic_use->unchanged() || s.closed->load())return X_STATUS_CANCELLED;
    call=*r->picture_call;publication=r->publication;
  }
  if(!s.CodeCurrent(0x8171BF40) || !s.CodeCurrent(0x8191B8F8))return X_STATUS_ACCESS_DENIED;
  const auto frame=ActualSaveFrame(*kernel,c,0x8171BF40);
  if(!frame || call.entry.sp<0x160 || frame->sp!=call.entry.sp-0x160 ||
      frame->id!=call.entry.id || frame->pcr!=call.entry.pcr || frame->lr!=0x8171C094 ||
      Dword(c.r[3])!=std::optional(frame->sp+0x60) || Dword(c.r[4])!=std::optional(0x40100080u) ||
      Dword(c.r[5])!=std::optional(frame->sp+0x78) || Dword(c.r[6])!=std::optional(frame->sp+0x70) ||
      Dword(c.r[7])!=std::optional(0u) || Dword(c.r[8])!=std::optional(0u) ||
      Dword(c.r[9])!=std::optional(0u) || Dword(c.r[10])!=std::optional(5u) ||
      ReadWord(*kernel->memory(),frame->sp+0x54)!=std::optional(0x60u))return X_STATUS_ACCESS_DENIED;
  std::array<uint8_t,12> attrs;std::array<uint8_t,8> name;
  if(!internal::ReadNativeWaitSnapshot(*kernel->memory(),frame->sp+0x78,attrs) ||
      !internal::ReadNativeWaitSnapshot(*kernel->memory(),frame->sp+0x68,name) ||
      BigWord(attrs,4)!=frame->sp+0x68 || BigWord(attrs,8)!=0x40 ||
      BigWord(name,4)!=call.name)return X_STATUS_ACCESS_DENIED;
  std::array<uint8_t,1> first_path;
  if(!internal::ReadNativeWaitSnapshot(*kernel->memory(),call.name,first_path))return X_STATUS_ACCESS_VIOLATION;
  const auto process=OriginalKernelProcessType(*kernel->memory(),*frame);if(!process)return X_STATUS_ACCESS_DENIED;
  const uint32_t expected_root=*process==X_PROCTYPE_SYSTEM && first_path[0]=='\\'?0u:0xFFFFFFFDu;
  if(BigWord(attrs)!=expected_root)return X_STATUS_ACCESS_DENIED;
  auto* fs=kernel->file_system();std::shared_ptr<vfs::Device> device;std::string translated,relative;
  {auto graph=global_critical_region::AcquireDirect();
    if(XFAILED(fs->TranslateSymbolicPath(path,translated)) || !(device=fs->AcquirePathDevice(translated)))
      return X_STATUS_OBJECT_PATH_NOT_FOUND;
    const auto& mount=device->mount_path();
    if(translated.size()<mount.size() || !xe::utf8::equal_case(std::string_view(translated).substr(0,mount.size()),mount) ||
       (translated.size()>mount.size() && !mount.ends_with('\\') && !mount.ends_with(':') && translated[mount.size()]!='\\'))
      return X_STATUS_ACCESS_DENIED;
    relative=translated.substr(mount.size());while(!relative.empty() && relative.front()=='\\')relative.erase(relative.begin());
  }
  const auto* leaf=NativeProfileSavePictureStage::Leaf(call.kind);
  if(!leaf || !xe::utf8::equal_case(relative,leaf) || context.managed_authority ||
      (context.expected_device && context.expected_device!=device))return X_STATUS_ACCESS_DENIED;
  std::shared_ptr<NativeProfileSaveManagedPicture> policy;
  {std::unique_lock prepare(s.preparation,std::try_to_lock);
    if(!prepare.owns_lock())return vfs::HostStorageError(Error(std::errc::device_or_resource_busy));
    for(const auto& candidate:s.pictures)if(candidate->kind_==call.kind &&
        candidate->source_->MatchesMetadata(*publication->WriterForStage().model->storage())) {
      if(policy)return X_STATUS_ACCESS_DENIED;policy=candidate;
    }
  }
  if(!policy)return X_STATUS_NOT_SUPPORTED;
  auto duty=std::make_shared<State::HostOpenUse>(owner->state_,r);
  if(!duty->admitted)return X_STATUS_CANCELLED;
  std::error_code error;auto authority=policy->IssueParentForActualOpen(publication,device,relative,duty,error);
  if(!authority)return vfs::HostStorageError(error);
  context.expected_device=std::move(device);context.managed_authority=std::move(authority);return X_STATUS_SUCCESS;
}
std::optional<X_STATUS> NativeProfileSaveOwner::CaptureOpenSource(KernelState* kernel,
    std::string_view path,vfs::FileOpenContext& context) noexcept try {
  const auto header=CaptureParentHeaderOpen(kernel,path,context);if(header)return header;
  const auto copy=CaptureParentPictureCopyOpen(kernel,path,context);if(copy)return copy;
  const auto parent=CaptureParentPictureOpen(kernel,path,context);if(parent)return parent;
  const auto owner=Lookup(kernel);if(!owner || !kernel || !XThread::IsInThread())return {};
  auto& s=*owner->state_;State::Observation observation(s);if(!observation)return X_STATUS_CANCELLED;
  auto* thread=XThread::GetCurrentThread();auto& c=*thread->thread_state()->context();
  auto r=s.CurrentInner(c);if(!r)return {};const auto caller=s.CurrentGpdCaller(*r);if(!caller)return {};
  SaveFrame stream;uint32_t db=0,cs=0,flags=0,profile=0;int32_t recursion=0;uint64_t xuid=0;
  std::shared_ptr<xam::ProfileRosterNativeWriter> source;
  {std::lock_guard lock(s.requests_mutex);
    if(!r->gpd->image_stream_entered || r->gpd->image_stream_returned)return {};
    if(r->uncertain || !r->semantic_use || !r->semantic_use->unchanged() ||
        r->gpd->image_open_captured || !r->gpd->image_lock_retained || r->gpd->image_release_entered ||
        r->release_entered || s.closed->load(std::memory_order_acquire))return X_STATUS_CANCELLED;
    stream=r->gpd->image_stream_open;db=r->gpd->image_db_pointer;cs=r->gpd->image_owned_cs;
    flags=r->gpd->image_open_flags;source=r->selected_source;profile=r->native_profile;xuid=r->native_profile_xuid;
    if(!r->gpd->image_try_after)return X_STATUS_ACCESS_DENIED;
    recursion=r->gpd->image_try_after->recursion;
  }
  // Only the genuine stream→BF40→NtCreateFile(D2) call. These are ORIGINAL
  // operands and frame facts; no host path or current-Task value grants access.
  if(!s.CodeCurrent(0x8171BF40) || !s.CodeCurrent(0x819237F8))return X_STATUS_ACCESS_DENIED;
  const auto frame=ActualSaveFrame(*kernel,c,0x8171BF40);
  if(!frame || stream.sp<0x130 || frame->id!=stream.id || frame->pcr!=stream.pcr ||
      frame->sp!=stream.sp-0x130 || frame->lr!=0x8171C094 ||
      Dword(c.r[3])!=std::optional(frame->sp+0x60) || Dword(c.r[4])!=std::optional(0xC0100080u) ||
      Dword(c.r[5])!=std::optional(frame->sp+0x78) || Dword(c.r[6])!=std::optional(frame->sp+0x70) ||
      Dword(c.r[7])!=std::optional(0u) || Dword(c.r[8])!=std::optional(0u) ||
      Dword(c.r[9])!=std::optional(flags) || Dword(c.r[10])!=std::optional(1u) ||
      ReadWord(*kernel->memory(),frame->sp+0x54)!=std::optional(0x60u))return X_STATUS_ACCESS_DENIED;
  const auto process=OriginalKernelProcessType(*kernel->memory(),*frame);
  std::array<uint8_t,1> first;
  if(!process || !internal::ReadNativeWaitSnapshot(*kernel->memory(),db+0x3C,first))return X_STATUS_ACCESS_DENIED;
  const uint32_t original_root=*process==2u && first[0]=='\\'?0u:0xFFFFFFFDu;
  std::array<uint8_t,12> attrs;std::array<uint8_t,8> name;std::array<uint8_t,16> import;
  if(!internal::ReadNativeWaitSnapshot(*kernel->memory(),frame->sp+0x78,attrs) ||
      !internal::ReadNativeWaitSnapshot(*kernel->memory(),frame->sp+0x68,name) ||
      !internal::ReadNativeWaitSnapshot(*kernel->memory(),0x81A723C4,import) ||
      BigWord(attrs)!=original_root || BigWord(attrs,4)!=frame->sp+0x68 || BigWord(attrs,8)!=0x40 ||
      BigWord(name,4)!=db+0x3C || BigWord(import)!=0x010000D2 || BigWord(import,4)!=0x020000D2 ||
      BigWord(import,8)!=0x7D6903A6 || BigWord(import,12)!=0x4E800420 ||
      ReadWord(*kernel->memory(),db+4)!=std::optional(cs))return X_STATUS_ACCESS_DENIED;
  const auto lock_state=ReadCriticalSnapshot(*kernel->memory(),cs);
  if(!lock_state || lock_state->owner_kthread!=thread->guest_object() || lock_state->recursion!=recursion ||
      recursion<=0 || !source || ReadWord(*kernel->memory(),db+0x24)!=std::optional(flags))return X_STATUS_ACCESS_DENIED;
  std::array<uint8_t,0x14> identity;
  if(!internal::ReadNativeWaitSnapshot(*kernel->memory(),profile,identity) ||
      (uint64_t(BigWord(identity))<<32|BigWord(identity,4))!=xuid ||
      BigWord(identity,0x10)!=r->input->user_slot())return X_STATUS_ACCESS_DENIED;
  std::error_code error;if(!source->Current(error))return vfs::HostStorageError(error);
  const auto read=source->before();
  std::shared_ptr<NativeProfileSaveManagedGpd> policy;
  {std::unique_lock prepare(s.preparation,std::try_to_lock);
    if(!prepare.owns_lock())return vfs::HostStorageError(Error(std::errc::device_or_resource_busy));
    for(const auto& candidate:s.gpd)if(candidate->title_==r->gpd->normalized_title &&
        candidate->source_->MatchesMetadata(*read.origin.storage)) {
      if(policy)return X_STATUS_ACCESS_DENIED;policy=candidate;
    }
  }
  if(!policy)return vfs::HostStorageError(Error(std::errc::no_such_device));
  auto* fs=kernel->file_system();std::shared_ptr<vfs::Device> device;std::string translated,relative;
  {auto graph=global_critical_region::AcquireDirect();
    if(XFAILED(fs->TranslateSymbolicPath(path,translated)) || !(device=fs->AcquirePathDevice(translated)))
      return X_STATUS_OBJECT_PATH_NOT_FOUND;
    const auto& mount=device->mount_path();
    if(translated.size()<mount.size() || !xe::utf8::equal_case(std::string_view(translated).substr(0,mount.size()),mount) ||
       (translated.size()>mount.size() && !mount.ends_with('\\') && !mount.ends_with(':') && translated[mount.size()]!='\\'))
      return X_STATUS_ACCESS_DENIED;
    relative=translated.substr(mount.size());while(!relative.empty() && relative.front()=='\\')relative.erase(relative.begin());
  }
  if(context.managed_authority || (context.expected_device &&
      (context.expected_device!=device || context.expected_device.owner_before(device) || device.owner_before(context.expected_device))))
    return X_STATUS_ACCESS_DENIED;
  auto receipt=NativeProfileSaveManagedGpd::IssueReadReceipt();
  auto duty=std::make_shared<State::HostOpenUse>(owner->state_,r);
  if(!duty->admitted)return X_STATUS_CANCELLED;
  const auto authority=caller->parent ?
      policy->IssueParentReadForActualOpen(r->publication,device,relative,duty,receipt,error):
      policy->IssueReadForActualOpen(read,device,relative,error,duty,receipt);
  if(!authority)return vfs::HostStorageError(error);
  {std::lock_guard guard(s.requests_mutex);
    if(r->uncertain || !r->semantic_use || !r->semantic_use->unchanged() ||
       r->gpd->image_open_captured || !r->gpd->image_lock_retained || r->gpd->image_stream_returned ||
       r->release_entered || s.closed->load(std::memory_order_acquire))return X_STATUS_CANCELLED;
    r->gpd->open_receipt=std::move(receipt);r->gpd->image_open_captured=true;
  }
  // Context is moved ONCE into the existing actual IO22 owned closure. The
  // private authority retains its true host-open duty there (including queued
  // drop/throw); File retains only the metadata receipt and immutable tuple.
  context.expected_device=std::move(device);context.managed_authority=authority;return X_STATUS_SUCCESS;
}catch(...){return X_STATUS_UNSUCCESSFUL;}
void NativeProfileSaveOwner::ObserveOriginalImageStreamReturn(void* raw) noexcept {
  if(!raw || global_critical_region::is_held_by_current_thread())return;
  auto& s=*state_;State::Observation observation(s);if(!observation)return;
  std::shared_ptr<State::Request> r;
  try {
    if(!s.CodeCurrent(0x819237F8))return;
    auto& c=*static_cast<cpu::ppc::PPCContext*>(raw);r=s.CurrentInner(c);
    if(!r)return;const auto caller=s.CurrentGpdCaller(*r);if(!caller)return;
    const auto frame=ActualSaveFrame(*s.kernel,c,0x819237F8);const auto result=Dword(c.r[3]);
    uint32_t stream=0;std::shared_ptr<const NativeProfileSaveManagedGpd::ReadReceipt> receipt;
    {std::lock_guard lock(s.requests_mutex);
      if(!frame || !result || !r->gpd->image_stream_entered || r->gpd->image_stream_returned ||
          r->gpd->image_stream_open!=*frame || !r->gpd->image_lock_retained || r->release_entered)return;
      stream=r->gpd->native_stream;receipt=r->gpd->open_receipt;
    }
    const auto handle=ReadWord(*s.kernel->memory(),stream+0x0C);if(!handle)return;
    std::shared_ptr<NativeProfileGpdReadView> view;std::error_code error;
    if(int32_t(*result)>=0 && *handle && *handle!=UINT32_MAX && receipt) {
      // Strong typed lookup makes this actual File stable during the metadata
      // check. Unique open receipt rejects numeric-handle reuse. No File ref
      // survives this observer to delay original native Close or stage sealing.
      const auto file=s.kernel->object_table()->LookupObject<XFile>(*handle);
      if(file && file->file())view=NativeProfileSaveManagedGpd::TryCaptureOpenedRead(*file->file(),receipt,error);
      else error=Error(std::errc::bad_file_descriptor);
    } else if(int32_t(*result)>=0) {error=Error(std::errc::operation_not_permitted);}
    std::lock_guard lock(s.requests_mutex);
    if(r->uncertain || r->gpd->image_stream_returned || !r->gpd->image_lock_retained || r->release_entered)return;
    r->gpd->native_stream_result=*result;r->gpd->opened_handle=*handle;r->gpd->opened_view=std::move(view);
    if(error)r->source_error=error;
    r->gpd->image_stream_returned=true;
    // Existing/raw or unrecognized files remain unsupported. No backend swap,
    // manufactured size failure, false clean flag or completed SAVE follows.
  }catch(...){if(r){std::lock_guard lock(s.requests_mutex);r->uncertain=true;}}
}

void NativeProfileSaveOwner::ObserveOriginalImageCoreEntry(void* raw) noexcept {
  if(!raw || global_critical_region::is_held_by_current_thread())return;
  auto& s=*state_;State::Observation observation(s);if(!observation)return;std::shared_ptr<State::Request> r;
  try {
    if(!s.CodeCurrent(0x819235F0) || !s.CodeCurrent(0x81928418))return;
    auto& c=*static_cast<cpu::ppc::PPCContext*>(raw);r=s.CurrentInner(c);
    if(!r)return;const auto caller=s.CurrentGpdCaller(*r);if(!caller)return;
    SaveFrame db;uint32_t expected=0;uint64_t xuid=0;
    {std::lock_guard lock(s.requests_mutex);if(!r->gpd->image_db_entered || r->gpd->image_db_returned ||
        r->gpd->image_core_entered || r->release_entered)return;
      db=r->gpd->image_db;expected=r->gpd->image_db_expected;xuid=r->native_profile_xuid;}
    auto* kernel=s.config.catalog->calls()->kernel();const auto frame=ActualSaveFrame(*kernel,c,0x819235F0);
    if(!frame || db.sp<0x110 || frame->sp!=db.sp-0x110 || frame->lr!=0x81928434 ||
       Dword(c.r[3])!=std::optional(expected))return;
    std::array<uint8_t,0x3C> prefix;
    if(!internal::ReadNativeWaitSnapshot(*kernel->memory(),expected,prefix))return;
    const auto cs=BigWord(prefix,4),resource=BigWord(prefix,0x38);
    if(!cs)return; // Native NULL path is a real failure, never an acquired use.
    if(resource) {
      if(uint64_t(resource)+0x28>uint64_t(UINT32_MAX)+1 || cs!=resource+8)return;
      std::array<uint8_t,0x28> record;
      if(!internal::ReadNativeWaitSnapshot(*kernel->memory(),resource,record) ||
         (uint64_t(BigWord(record))<<32|BigWord(record,4))!=xuid || !BigWord(record,0x24))return;
    } else if(uint64_t(expected)+8>UINT32_MAX || cs!=expected+8) {return;}
    std::lock_guard lock(s.requests_mutex);
    if(r->uncertain || r->gpd->image_core_entered || r->release_entered)return;
    r->gpd->image_core=*frame;r->gpd->image_owned_cs=cs;r->gpd->image_owned_resource=resource;r->gpd->image_core_entered=true;
    // Actual configured-pointer/key observation only. The separately observed
    // native config/reference generation and real File tuple are still needed.
  }catch(...){if(r){std::lock_guard lock(s.requests_mutex);r->uncertain=true;}}
}
void NativeProfileSaveOwner::ObserveOriginalImageTryEntry(void* raw) noexcept {
  if(!raw || global_critical_region::is_held_by_current_thread())return;
  auto& s=*state_;State::Observation observation(s);if(!observation)return;std::shared_ptr<State::Request> r;
  try {
    if(!s.CodeCurrent(0x819235F0))return;
    auto& c=*static_cast<cpu::ppc::PPCContext*>(raw);r=s.CurrentInner(c);
    if(!r)return;const auto caller=s.CurrentGpdCaller(*r);if(!caller)return;
    SaveFrame core;uint32_t cs=0,db=0;
    {std::lock_guard lock(s.requests_mutex);if(!r->gpd->image_core_entered || r->gpd->image_core_returned ||
        r->gpd->image_try_pending || r->gpd->image_try_acquired || r->release_entered)return;
      core=r->gpd->image_core;cs=r->gpd->image_owned_cs;db=r->gpd->image_db_expected;}
    auto* kernel=s.config.catalog->calls()->kernel();const auto frame=ActualSaveFrame(*kernel,c,0x819235F0);
    if(!frame || core.sp<0x70 || frame->sp!=core.sp-0x70 ||
       Dword(c.r[3])!=std::optional(cs) || Dword(c.r[31])!=std::optional(db) ||
       ReadWord(*kernel->memory(),frame->sp)!=std::optional(core.sp) ||
       ReadWord(*kernel->memory(),db+4)!=std::optional(cs))return;
    const auto before=ReadCriticalSnapshot(*kernel->memory(),cs);if(!before)return;
    // This is the original pre-BL23628, before the REAL Try import. It has no
    // native wait, false return or lock effect; failed iterations stay pending
    // only until their own genuine2362C and can never create an acquired use.
    std::lock_guard lock(s.requests_mutex);
    if(r->uncertain || r->gpd->image_try_pending || r->gpd->image_try_acquired ||
       r->gpd->image_core_returned || r->gpd->image_try_attempts==UINT64_MAX)return;
    r->gpd->image_try=*frame;r->gpd->image_try_before=before;r->gpd->image_try_pending=true;++r->gpd->image_try_attempts;
  }catch(...){if(r){std::lock_guard lock(s.requests_mutex);r->uncertain=true;}}
}
void NativeProfileSaveOwner::ObserveOriginalImageTryReturn(void* raw) noexcept {
  if(!raw || global_critical_region::is_held_by_current_thread())return;
  auto& s=*state_;State::Observation observation(s);if(!observation)return;std::shared_ptr<State::Request> r;
  try {
    if(!s.CodeCurrent(0x819235F0))return;
    auto& c=*static_cast<cpu::ppc::PPCContext*>(raw);r=s.CurrentInner(c);
    if(!r)return;const auto caller=s.CurrentGpdCaller(*r);if(!caller)return;
    SaveFrame attempted;uint32_t cs=0,db=0;std::optional<CriticalSnapshot> before;
    {std::lock_guard lock(s.requests_mutex);if(!r->gpd->image_try_pending || r->gpd->image_try_acquired ||
        r->gpd->image_core_returned || r->release_entered)return;
      attempted=r->gpd->image_try;before=r->gpd->image_try_before;cs=r->gpd->image_owned_cs;db=r->gpd->image_db_expected;}
    auto* kernel=s.config.catalog->calls()->kernel();const auto frame=ActualSaveFrame(*kernel,c,0x819235F0);
    const auto result=Dword(c.r[3]);
    if(!frame || !before || !result || *result>1 || frame->id!=attempted.id || frame->pcr!=attempted.pcr ||
       frame->sp!=attempted.sp || frame->lr!=0x8192362C || Dword(c.r[31])!=std::optional(db) ||
       ReadWord(*kernel->memory(),db+4)!=std::optional(cs))return;
    std::optional<CriticalSnapshot> after;
    if(*result==1) {
      after=ReadCriticalSnapshot(*kernel->memory(),cs);
      const auto self=frame->thread->guest_object();
      // Other waiters may change lock_count, so never use its sampled delta as
      // an acquisition count. The actual successful primitive result and its
      // owned recursion are the relevant original effect.
      if(!after || after->owner_kthread!=self || after->recursion<=0)return;
      if(before->owner_kthread==self) {
        if(before->recursion<=0 || before->recursion==INT32_MAX || after->recursion!=before->recursion+1)return;
      } else if(after->recursion!=1) {return;}
    }
    std::lock_guard lock(s.requests_mutex);
    if(r->uncertain || !r->gpd->image_try_pending || r->gpd->image_try_acquired || r->gpd->image_core_returned)return;
    r->gpd->image_try_pending=false;r->gpd->image_try_after=after;
    if(*result==1) {r->gpd->image_try_acquired=true;r->gpd->image_lock_retained=true;}
    // Success is retained NOW, before core/Configure/GetDB own-return. This is
    // one observed original primitive use, not dirty-cache or writer authority.
  }catch(...){if(r){std::lock_guard lock(s.requests_mutex);r->uncertain=true;}}
}
void NativeProfileSaveOwner::ObserveOriginalImageCoreReturn(void* raw) noexcept {
  if(!raw || global_critical_region::is_held_by_current_thread())return;
  auto& s=*state_;State::Observation observation(s);if(!observation)return;std::shared_ptr<State::Request> r;
  try {
    if(!s.CodeCurrent(0x819235F0))return;
    auto& c=*static_cast<cpu::ppc::PPCContext*>(raw);r=s.CurrentInner(c);
    if(!r)return;const auto caller=s.CurrentGpdCaller(*r);if(!caller)return;
    const auto frame=ActualSaveFrame(*s.config.catalog->calls()->kernel(),c,0x819235F0);
    const auto result=Dword(c.r[3]);
    std::lock_guard lock(s.requests_mutex);
    if(!frame || !result || !r->gpd->image_core_entered || r->gpd->image_core_returned ||
       r->gpd->image_core!=*frame || r->gpd->image_try_pending || r->release_entered)return;
    if(int32_t(*result)>=0) {
      if(*result || !r->gpd->image_try_acquired || !r->gpd->image_lock_retained || !r->gpd->image_try_after)return;
    } else if(r->gpd->image_try_acquired || r->gpd->image_lock_retained) {return;}
    r->gpd->image_core_result=*result;r->gpd->image_core_returned=true;
    // Core's actual0 convention differs from Try's1. A failed/cancelled core
    // never gets an invented use and cannot be mistaken for successful flush.
  }catch(...){if(r){std::lock_guard lock(s.requests_mutex);r->uncertain=true;}}
}
void NativeProfileSaveOwner::ObserveOriginalImageReleaseEntry(void* raw) noexcept {
  if(!raw || global_critical_region::is_held_by_current_thread())return;
  auto& s=*state_;State::Observation observation(s);if(!observation)return;std::shared_ptr<State::Request> r;
  try {
    if(!s.CodeCurrent(0x8190F1C0))return;
    auto& c=*static_cast<cpu::ppc::PPCContext*>(raw);r=s.CurrentInner(c);
    if(!r)return;const auto caller=s.CurrentGpdCaller(*r);if(!caller)return;
    SaveFrame leaf;uint32_t profile=0,output=0,db=0;
    {std::lock_guard lock(s.requests_mutex);if(!r->gpd->image_db_returned || int32_t(r->gpd->image_db_result)<0 ||
        !r->gpd->image_core_returned || !r->gpd->image_lock_retained || r->gpd->image_release_entered || r->release_entered)return;
      leaf=r->gpd->image_leaf;profile=r->native_profile;output=r->gpd->image_db_output;db=r->gpd->image_db_pointer;}
    auto* kernel=s.config.catalog->calls()->kernel();const auto frame=ActualSaveFrame(*kernel,c,0x8190F1C0);
    if(!frame || leaf.sp<caller->body_bytes || frame->sp!=leaf.sp-caller->body_bytes ||
       (frame->lr!=caller->release_return && !(caller->parent && frame->lr==0x8191273C)) ||
       Dword(c.r[3])!=std::optional(profile) || Dword(c.r[4])!=std::optional(output) ||
       Dword(c.r[5])!=std::optional(0u) || ReadWord(*kernel->memory(),output)!=std::optional(db))return;
    std::lock_guard lock(s.requests_mutex);
    if(r->uncertain || r->gpd->image_release_entered || r->release_entered)return;
    r->gpd->image_db_release=*frame;r->gpd->image_release_entered=true;
    // Original noncached virtual+4 and actual unlock still execute. Cached
    // DB+34=1/r5=0 skips that close; do not claim it happened. Entry is not
    // cleanliness, durable success, cancellation or a released native lock.
  }catch(...){if(r){std::lock_guard lock(s.requests_mutex);r->uncertain=true;}}
}
void NativeProfileSaveOwner::ObserveOriginalImageUnlockEntry(void* raw) noexcept {
  if(!raw || global_critical_region::is_held_by_current_thread())return;
  auto& s=*state_;State::Observation observation(s);if(!observation)return;std::shared_ptr<State::Request> r;
  try {
    if(!s.CodeCurrent(0x81923670) || !s.CodeCurrent(0x81928468) || !s.CodeCurrent(0x8190F1C0))return;
    auto& c=*static_cast<cpu::ppc::PPCContext*>(raw);r=s.CurrentInner(c);
    if(!r)return;const auto caller=s.CurrentGpdCaller(*r);if(!caller)return;
    SaveFrame release;uint32_t db=0,cs=0;std::optional<CriticalSnapshot> acquired;
    {std::lock_guard lock(s.requests_mutex);if(!r->gpd->image_release_entered || r->gpd->image_release_returned ||
        !r->gpd->image_lock_retained || r->gpd->image_unlock_entered || r->release_entered)return;
      release=r->gpd->image_db_release;db=r->gpd->image_db_pointer;cs=r->gpd->image_owned_cs;acquired=r->gpd->image_try_after;}
    auto* kernel=s.config.catalog->calls()->kernel();const auto frame=ActualSaveFrame(*kernel,c,0x81923670);
    if(!frame || release.sp<0x70 || frame->sp!=release.sp-0x70 || frame->lr!=0x8190F238 ||
       Dword(c.r[3])!=std::optional(db) || ReadWord(*kernel->memory(),db+4)!=std::optional(cs))return;
    const auto before=ReadCriticalSnapshot(*kernel->memory(),cs);
    if(!before || !acquired || before->owner_kthread!=frame->thread->guest_object() ||
       before->recursion!=acquired->recursion || before->recursion<=0)return;
    std::lock_guard lock(s.requests_mutex);
    if(r->uncertain || !r->gpd->image_lock_retained || r->gpd->image_unlock_entered || r->release_entered)return;
    r->gpd->image_unlock=*frame;r->gpd->image_unlock_before=before;r->gpd->image_unlock_entered=true;
    // Original23670 is frameless: it loads actualCS then tails to the REAL
    // Leave import. There is no synthetic wrapper epilogue or early discharge.
  }catch(...){if(r){std::lock_guard lock(s.requests_mutex);r->uncertain=true;}}
}
void NativeProfileSaveOwner::ObserveOriginalImageUnlockReturn(void* raw) noexcept {
  if(!raw || global_critical_region::is_held_by_current_thread())return;
  auto& s=*state_;State::Observation observation(s);if(!observation)return;std::shared_ptr<State::Request> r;
  try {
    if(!s.CodeCurrent(0x81923670) || !s.CodeCurrent(0x8190F1C0))return;
    auto& c=*static_cast<cpu::ppc::PPCContext*>(raw);r=s.CurrentInner(c);
    if(!r)return;const auto caller=s.CurrentGpdCaller(*r);if(!caller)return;
    const auto frame=ActualSaveFrame(*s.config.catalog->calls()->kernel(),c,0x81923670);
    std::lock_guard lock(s.requests_mutex);
    if(!frame || !r->gpd->image_unlock_entered || r->gpd->image_unlock_returned || !r->gpd->image_lock_retained ||
       r->gpd->image_unlock!=*frame || r->release_entered)return;
    r->gpd->image_unlock_returned=true;r->gpd->image_lock_retained=false;
    // Count only the real own-return after Leave. Do NOT reread the old CS or
    // require owner0: its real wake may already have let another thread take
    // it. This retires only this exact prior use, never the replacement user.
    // No durability/clean-cache outcome follows from a void Leave return.
  }catch(...){if(r){std::lock_guard lock(s.requests_mutex);r->uncertain=true;}}
}
void NativeProfileSaveOwner::ObserveOriginalImageReleaseReturn(void* raw) noexcept {
  if(!raw || global_critical_region::is_held_by_current_thread())return;
  auto& s=*state_;State::Observation observation(s);if(!observation)return;std::shared_ptr<State::Request> r;
  try {
    if(!s.CodeCurrent(0x8190F1C0))return;
    auto& c=*static_cast<cpu::ppc::PPCContext*>(raw);r=s.CurrentInner(c);
    if(!r)return;const auto caller=s.CurrentGpdCaller(*r);if(!caller)return;
    auto* kernel=s.config.catalog->calls()->kernel();const auto frame=ActualSaveFrame(*kernel,c,0x8190F1C0);
    uint32_t output=0;{std::lock_guard lock(s.requests_mutex);output=r->gpd->image_db_output;}
    if(ReadWord(*kernel->memory(),output)!=std::optional(0u))return;
    std::lock_guard lock(s.requests_mutex);
    if(!frame || !r->gpd->image_release_entered || r->gpd->image_release_returned || r->gpd->image_db_release!=*frame ||
       !r->gpd->image_unlock_returned || r->gpd->image_lock_retained || r->release_entered)return;
    r->gpd->image_release_returned=true;
    // The original holder word is now genuinely NULL. Keep the independent
    // selected-source/request/File/transaction duties until their real joins.
  }catch(...){if(r){std::lock_guard lock(s.requests_mutex);r->uncertain=true;}}
}

void NativeProfileSaveOwner::ObserveOriginalImageDbReturn(void* raw) noexcept {
  if(!raw || global_critical_region::is_held_by_current_thread())return;
  auto& s=*state_;State::Observation observation(s);if(!observation)return;std::shared_ptr<State::Request> r;
  try {
    if(!s.CodeCurrent(0x81911D48))return;
    auto& c=*static_cast<cpu::ppc::PPCContext*>(raw);r=s.CurrentInner(c);
    if(!r)return;const auto caller=s.CurrentGpdCaller(*r);if(!caller)return;
    auto* kernel=s.config.catalog->calls()->kernel();const auto frame=ActualSaveFrame(*kernel,c,0x81911D48);
    SaveFrame entered;uint32_t output=0,expected=0,title=0;
    {std::lock_guard lock(s.requests_mutex);if(!r->gpd->image_db_entered || r->gpd->image_db_returned ||
        r->release_entered)return;entered=r->gpd->image_db;output=r->gpd->image_db_output;expected=r->gpd->image_db_expected;title=r->gpd->normalized_title;}
    const auto result=Dword(c.r[3]);if(!frame || *frame!=entered || !result)return;
    // The output is read only at its actual original caller-frame return,
    // never on a host worker or after that native frame/ref was released.
    const auto database=ReadWord(*kernel->memory(),output);if(!database)return;
    uint32_t cs=0,resource=0;std::optional<CriticalSnapshot> current_lock;
    if(int32_t(*result)>=0) {
      if(*database!=expected || uint64_t(expected)+0x3C>uint64_t(UINT32_MAX)+1)return;
      std::array<uint8_t,0x3C> prefix;
      if(!internal::ReadNativeWaitSnapshot(*kernel->memory(),expected,prefix) ||
         BigWord(prefix,0)!=(title==0xFFFE07D1?0x81630370u:0x8163034Cu))return;
      cs=BigWord(prefix,4);resource=BigWord(prefix,0x38);
      current_lock=ReadCriticalSnapshot(*kernel->memory(),cs);
      if(!current_lock || current_lock->recursion<=0 ||
         current_lock->owner_kthread!=frame->thread->guest_object())return;
      {std::lock_guard lock(s.requests_mutex);
        if(!r->gpd->image_core_returned || r->gpd->image_core_result || !r->gpd->image_try_acquired ||
           !r->gpd->image_lock_retained || r->gpd->image_owned_cs!=cs || r->gpd->image_owned_resource!=resource ||
           !r->gpd->image_try_after || current_lock->recursion!=r->gpd->image_try_after->recursion)return;}

    } else if(*database) {return;}
    std::lock_guard lock(s.requests_mutex);
    if(r->uncertain || r->gpd->image_db_returned || r->release_entered)return;
    if(int32_t(*result)>=0 && !s.semantic_uses.BindDatabase(r->semantic_use,*database,resource))
      r->source_error=Error(std::errc::operation_canceled);
    r->gpd->image_db_result=*result;r->gpd->image_db_pointer=*database;
    r->gpd->image_db_cs=cs;r->gpd->image_db_resource=resource;r->gpd->image_db_returned=true;
    r->gpd->image_db_return_lock=current_lock;
    // Actual primitive before/after owner+recursion receipts and configured
    // source/view proof are still REQUIRED. This metadata is not that grant.
  }catch(...){if(r){std::lock_guard lock(s.requests_mutex);r->uncertain=true;}}
}
void NativeProfileSaveOwner::ObserveOriginalImageLeafReturn(void* raw) noexcept {
  if(!raw || global_critical_region::is_held_by_current_thread())return;
  auto& s=*state_;State::Observation observation(s);if(!observation)return;std::shared_ptr<State::Request> r;
  try {
    if(!s.CodeCurrent(0x819101D0))return;
    auto& c=*static_cast<cpu::ppc::PPCContext*>(raw);r=s.CurrentInner(c);
    if(!r || r->site.message!=0xB0031)return;
    const auto frame=ActualSaveFrame(*s.config.catalog->calls()->kernel(),c,0x819101D0);
    std::lock_guard lock(s.requests_mutex);
    if(!frame || !r->gpd->image_leaf_entered || r->gpd->image_leaf_returned || r->gpd->image_leaf!=*frame)return;
    r->gpd->image_leaf_returned=true;
    if(r->gpd->image_db_entered && !r->gpd->image_db_returned)r->uncertain=true;
    if(r->gpd->image_db_returned && int32_t(r->gpd->image_db_result)>=0 &&
       (!r->gpd->image_release_returned || r->gpd->image_lock_retained))r->uncertain=true;
    if(r->gpd->image_edit_entered && !r->gpd->image_edit_returned)r->uncertain=true;
    // Early native validation failure may never enter GetDB. Do not invent
    // an acquired DB, release, flush, cancellation or successful publication.
  }catch(...){if(r){std::lock_guard lock(s.requests_mutex);r->uncertain=true;}}
}
X_STATUS NativeProfileSaveOwner::PrepareManagedFileCut(const std::shared_ptr<ManagedFileCut>& cut) {
  if(global_critical_region::is_held_by_current_thread() || !cut || !cut->file || !cut->service ||
      (cut->kind!=ManagedFileCut::Kind::kBindSystem && cut->kind!=ManagedFileCut::Kind::kCancelPristine) ||
      !cut->file->is_synchronous_ || cut->file->is_alertable_)
    return X_STATUS_INVALID_DEVICE_REQUEST;
  // Same real queue/File locks as actual IO, but nonwaiting and metadata-only.
  // No observer borrow crosses a scheduler wait; the real family issuer has
  // already authenticated its native source and exact before File tuple.
  std::unique_lock posted(cut->file->posted_lock_,std::try_to_lock);
  if(!posted.owns_lock() || cut->file->posted_running_ || cut->file->posted_executing_ || !cut->file->posted_.empty())
    return vfs::HostStorageError(Error(std::errc::device_or_resource_busy));
  std::unique_lock io(cut->file->file_lock_,std::try_to_lock);
  if(!io.owns_lock())return vfs::HostStorageError(Error(std::errc::device_or_resource_busy));
  auto* backend=cut->file->file_;if(!backend)return X_STATUS_INVALID_HANDLE;
  return cut->kind==ManagedFileCut::Kind::kBindSystem ?
      cut->service->BindSystemFileBeforeActualEdit(*backend,cut->expected,cut->publication,std::move(cut->actual_native_header_index)) :
      cut->service->CancelPristineSystemBindingAtActualNoop(*backend,cut->expected,cut->publication,cut->actual_native_header_index);
}
X_STATUS NativeProfileSaveOwner::RunManagedFileCut(const std::shared_ptr<ManagedFileCut>& cut) {
  if(global_critical_region::is_held_by_current_thread() || !cut || !cut->file || !cut->service ||
      (cut->kind==ManagedFileCut::Kind::kBindSystem || cut->kind==ManagedFileCut::Kind::kCancelPristine) ||
      !cut->file->is_synchronous_ || cut->file->is_alertable_) {return X_STATUS_INVALID_DEVICE_REQUEST;}
  X_STATUS result=X_STATUS_UNSUCCESSFUL;
  // IO22 owns the closure and the exact XFile/backend until genuine return or
  // queue drop. The retained caller stack still owns only this checked result.
  const auto invoked=cut->file->RunSynchronousIo([cut,&result] {
    // Match the actual PostIo admission state, then the same file_lock_ as
    // ordinary retained read/write/query. Never wait with one of these locks
    // for a different worker or queue runner. Future enqueue waits this cut.
    std::unique_lock posted(cut->file->posted_lock_,std::try_to_lock);
    if(!posted.owns_lock() || cut->file->posted_running_ || cut->file->posted_executing_ ||
        !cut->file->posted_.empty()) {result=vfs::HostStorageError(Error(std::errc::device_or_resource_busy));return;}
    std::unique_lock io(cut->file->file_lock_,std::try_to_lock);
    if(!io.owns_lock()) {result=vfs::HostStorageError(Error(std::errc::device_or_resource_busy));return;}
    auto* backend=cut->file->file_;
    if(!backend) {result=X_STATUS_INVALID_HANDLE;return;}
    // The stable backend independently authenticates the exact issued owner,
    // source, physical leaf, pristine stage and complete native table layout.
    // No File/XFile/backend pointer, position, wait event or port is replaced.
    result=cut->kind==ManagedFileCut::Kind::kAdopt
        ?cut->service->UpgradeAtActualDbQuiescence(*backend,cut->expected,cut->open,cut->actual_native_header_index)
        :cut->service->PromoteCommittedAtActualDbQuiescence(*backend,cut->publication,cut->actual_native_header_index);
  });
  return invoked==HostCallOutcome::kReturned?result:HostCallFailureStatus(invoked);
}
// Factory/host retirement are private and UNWIRED; actual request/DB/File and
// checked IO duties must be joined before activation. No standalone application.
}  // namespace xe::kernel
