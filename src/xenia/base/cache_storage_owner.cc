/** Xenia : Xbox 360 Emulator Research Project; BSD license, see LICENSE. */
#include "xenia/base/cache_storage_owner.h"
#include "xenia/base/cache_storage_wire.h"
#include "xenia/base/managed_storage_bootstrap.h"
#include "xenia/base/mutex.h"
#include <algorithm>
#include <mutex>
#include <random>
namespace xe::filesystem {
namespace {
namespace wire=cache_storage_detail;
std::error_code Denied(){return std::make_error_code(std::errc::permission_denied);}
std::error_code Busy(){return std::make_error_code(std::errc::device_or_resource_busy);}
std::error_code Pending(){return std::make_error_code(std::errc::operation_in_progress);}
std::string FormatLeaf(size_t slot){return "cache-format-"+std::to_string(slot)+".v1";}
bool Equal(std::span<const uint8_t> a,std::span<const uint8_t> b){return std::equal(a.begin(),a.end(),b.begin(),b.end());}
size_t Index(CacheStorageOwner::Role role){return size_t(role);}
constexpr auto kBindingDirectory=".xenia-cache-binding-v1";
std::vector<uint8_t> ReadControl(const HostStorageRoot& root,const char* leaf,std::error_code& error) {
  const auto token=root.CaptureSnapshot(leaf,65536,error);
  if(!token || error || !token->exists()){
    if(!error)error=std::make_error_code(std::errc::operation_in_progress);return {};}
  return {token->bytes().begin(),token->bytes().end()};
}
struct BoundCache {
  wire::Binding binding;
  wire::Commit commit;
  wire::Digest purpose{};
  std::shared_ptr<HostStorageRoot> directory;
  size_t slot=3;
};
BoundCache ReadBoundCache(const HostStorageRoot& source,std::string_view id,uint32_t device,
    std::error_code& error) {
  BoundCache out;
  if(device!=1){error=Denied();return out;}
  out.directory=source.Subroot(kBindingDirectory,true,error);
  if(!out.directory)return out;
  const auto owner=ReadControl(*out.directory,"owner.v1",error);if(error)return out;
  out.binding=wire::DecodeBinding(owner,error);if(error)return out;
  out.purpose=wire::Hash(owner,error);if(error)return out;
  for(size_t i=0;i<3;++i)if(out.binding.slots[i].source_id==id){
    if(out.slot!=3){error=wire::Conflict();return out;}out.slot=i;
  }
  if(out.slot==3){error=wire::Conflict();return out;}
  const auto& slot=out.binding.slots[out.slot];
  const auto source_pin=source.directory_identity(),binding_pin=out.directory->directory_identity();
  if(source.path().generic_string()!=slot.source.path || !source_pin || !binding_pin ||
     !source_pin->MatchesNativeDirectory(slot.source.filesystem,slot.source.directory) ||
     !binding_pin->MatchesNativeDirectory(slot.source.filesystem,slot.binding_directory) ||
     !source.Observe().available || !out.directory->Observe().available){error=wire::Conflict();return out;}
  const auto complete=ReadControl(*out.directory,"bootstrap.commit.v1",error);if(error)return out;
  out.commit=wire::DecodeCommit(complete,error);
  if(error || out.commit.purpose!=out.purpose){if(!error)error=wire::Conflict();return out;}
  // A valid source record is not enough: the exact shared private root and
  // its same complete cache-purpose binding/commit must still physically exist.
  const auto table=HostStorageRoot::Open(out.binding.table.path,true,error);
  if(!table)return out;
  const auto table_pin=table->directory_identity();
  if(!table_pin || !table_pin->MatchesNativeDirectory(out.binding.table.filesystem,out.binding.table.directory) ||
     !Equal(ReadControl(*table,"owner.v1",error),owner) || error ||
     !Equal(ReadControl(*table,"bootstrap.commit.v1",error),complete) || error){
    if(!error)error=wire::Conflict();return out;}
  const auto recovery=HostStorageRoot::Open(out.binding.recovery.path,true,error);
  if(!recovery)return out;
  const auto recovery_pin=recovery->directory_identity();
  if(!recovery_pin || !recovery_pin->MatchesNativeDirectory(out.binding.recovery.filesystem,
      out.binding.recovery.directory) || !recovery->Observe().available){
    if(!error)error=wire::Conflict();return out;}
  return out;
}
}  // namespace
struct CacheStorageOwner::State {
  explicit State(CacheProductIdentity p,Options o):product(p),options(std::move(o)){
    for(auto& slot:slot_retired)slot=std::make_shared<std::atomic<bool>>(false);
  }
  const CacheProductIdentity product;
  Options options;
  mutable std::mutex mutex;
  std::error_code error;
  wire::Binding binding;
  wire::Digest purpose{};
  std::vector<uint8_t> binding_wire;
  std::array<std::shared_ptr<HostStorageRoot>,kRoleCount> sources;
  std::array<std::shared_ptr<const HostStorageRoot>,kRoleCount> anchors;
  std::array<std::atomic<std::shared_ptr<HostStorageRoot>>,kRoleCount> guest;
  std::array<std::shared_ptr<const HostStorageIntentLease>,kRoleCount> leases;
  std::array<std::shared_ptr<const ManagedDeviceIdentity>,kRoleCount> identities;
  std::array<std::shared_ptr<HostStorageRoot>,kRoleCount> bindings;
  std::array<std::shared_ptr<const HostStorageGuestProjection>,kRoleCount> projections;
  std::vector<std::shared_ptr<const HostStorageGuestProjection>> validation_projections;
  std::vector<std::unique_ptr<HostStorageBootstrapDirectory>> capsules;
  std::vector<std::shared_ptr<HostStorageRoot>> preserved;
  std::vector<std::filesystem::path> preserved_files;
  std::shared_ptr<HostStorageRoot> table_root;
  std::shared_ptr<const SnapshotRecoveryOwner> recovery;
  std::shared_ptr<const HostStorageSnapshotToken> table_snapshot;
  wire::Table table;
  const std::shared_ptr<std::atomic<bool>> retired=std::make_shared<std::atomic<bool>>(false);
  std::array<std::shared_ptr<std::atomic<bool>>,kRoleCount> slot_retired;
  std::atomic<bool> accepting{false},admission_open{false},format_active{false};
  enum class BootstrapStage {kProbe,kDeclared,kCommitted};
  BootstrapStage bootstrap_stage=BootstrapStage::kProbe;
  bool bootstrap_committed=false,native_closing=false,issuer_bound=false;
  std::atomic<bool> native_closed{false};
  std::weak_ptr<const uint8_t> registry_issuer;
  const std::shared_ptr<std::atomic<size_t>> metadata_files=std::make_shared<std::atomic<size_t>>(0);
  std::weak_ptr<CacheFormatHandoff::State> active_handoff;
  // Caller owns the sole operation mutex. Native observations occur outside
  // GCRC/model/registry locks; the wire is never interpreted as authority.
  std::error_code LoadTable() {
    std::error_code e;
    if(!table_root || !recovery || recovery->error())return Denied();
    const auto observed=table_root->CaptureSnapshot("cache-table.v1",4096,e);
    if(!observed || e || !observed->exists())return e?e:wire::Conflict();
    auto value=wire::DecodeTable(observed->bytes(),e);
    if(e || value.purpose!=purpose)return e?e:wire::Conflict();
    for(size_t i=0;i<kRoleCount;++i)if(!identities[i] ||
        value.uuid[i]!=identities[i]->uuid() || value.epoch[i]!=identities[i]->content_epoch())return wire::Conflict();
    table=std::move(value);table_snapshot=observed;return {};
  }
};
struct CacheStorageOwner::MetadataFileUse {
  explicit MetadataFileUse(std::shared_ptr<std::atomic<size_t>> value):count(std::move(value)){}
  ~MetadataFileUse(){if(admitted)count->fetch_sub(1,std::memory_order_acq_rel);}
  const std::shared_ptr<std::atomic<size_t>> count;
  bool admitted=false;
};
struct CacheStorageOwner::RegistrationAdmission::Impl {
  explicit Impl(std::shared_ptr<const CacheStorageOwner> value)
      :owner(std::move(value)),lock(owner->state_->mutex,std::try_to_lock){}
  // Unlock before releasing the exact owner of this mutex.
  const std::shared_ptr<const CacheStorageOwner> owner;
  std::unique_lock<std::mutex> lock;
};
CacheStorageOwner::RegistrationAdmission::RegistrationAdmission(std::unique_ptr<Impl> impl):impl_(std::move(impl)){}
CacheStorageOwner::RegistrationAdmission::~RegistrationAdmission()=default;
struct CacheFormatHandoff::State {
  std::shared_ptr<CacheStorageOwner> owner;
  size_t slot=0;
  wire::FormatPlan plan;
  std::vector<uint8_t> plan_wire;
  std::unique_ptr<ManagedDeviceIdentity::Handoff> native;
  std::unique_ptr<PreparedSnapshotPublication> prepared;
  const std::shared_ptr<std::atomic<bool>> fresh_slot_retired=std::make_shared<std::atomic<bool>>(false);
  bool owns_operation=false;
  ~State(){if(owns_operation && owner){
    // Native fields finish releasing before another operation/teardown can
    // observe this actual handoff as gone. No destructor invokes new IO.
    native.reset();prepared.reset();
    owner->state_->format_active.store(false,std::memory_order_release);
  }}
};
std::string CacheProductIdentity::key() const {
  constexpr char hex[]="0123456789ABCDEF";std::string value=kind_==Kind::kXbox360?"360-":"og-";
  for(int shift=28;shift>=0;shift-=4)value+=hex[(id_>>shift)&15];return value;
}
std::error_code CacheStorageOwner::PublishControl(const HostStorageRoot& root,
    const std::filesystem::path& leaf,std::span<const uint8_t> bytes,std::string nonce) {
  for(unsigned attempt=0;attempt<64;++attempt){
    const auto written=root.PublishBootstrapRecord(leaf,bytes,nonce);
    if(!written.error)return written.durable?std::error_code{}:wire::Conflict();
    if(written.error!=std::errc::file_exists)return written.error;
    // Only this exact declared temporary is observed. Its opaque bytes remain
    // untouched in the already bound private directory. A new deterministic
    // nonce avoids it; no glob scan, content adoption or unlink is performed.
    std::error_code error;
    const auto partial=root.CaptureSnapshot(leaf.string()+".partial-"+nonce,65536,error);
    if(!partial || error || !partial->exists())return error?error:wire::Conflict();
    std::vector<uint8_t> key;wire::Text(key,root.path().generic_string());wire::Text(key,leaf.string());
    wire::Text(key,nonce);wire::Number(key,partial->info().file_id);
    const auto hash=wire::Hash(key,error);if(error)return error;
    nonce=wire::Hex(std::span(hash).first(16));
  }
  return std::make_error_code(std::errc::no_space_on_device);
}
bool CacheStorageOwner::IsCacheSource(std::string_view source) {return source.starts_with("cache-v1:");}
std::error_code CacheStorageOwner::BeforeNormalOpen(const HostStorageRoot& source,
    std::string_view id,uint32_t device) {
  const auto path=source.path().native();
  if(std::find(path.begin(),path.end(),0)!=path.end())return std::make_error_code(std::errc::invalid_argument);
  std::error_code error;const auto bound=ReadBoundCache(source,id,device,error);if(error)return error;
  // Ordinary MDI Open(create) must never recreate a committed missing marker.
  // Observe its exact nofollow native inode without opening/borrowing the
  // marker's leased data FD. The actual shared decoder follows on that FD.
  const auto marker=source.ObservePreparationArtifact(ManagedDeviceIdentity::kMarker,error);
  if(error)return error;
  return marker && marker->regular && marker->links==1 && marker->file_id==bound.commit.marker_file[bound.slot]
      ? std::error_code{}:wire::Conflict();
}
std::error_code CacheStorageOwner::CheckNormal(const HostStorageRoot& source,
    const ManagedBootstrapSourceDescription& actual) {
  const auto path=source.path().native();
  if(std::find(path.begin(),path.end(),0)!=path.end())return std::make_error_code(std::errc::invalid_argument);
  std::error_code error;const auto bound=ReadBoundCache(source,actual.source,actual.device,error);
  if(error)return error;
  const auto& slot=bound.binding.slots[bound.slot];
  const auto prefix=ManagedDeviceIdentity::DescribeBootstrapWire(slot.prefix,error);
  const auto digest=wire::Hash(slot.prefix,error);
  if(error || !actual.binding || actual.uuid!=slot.uuid || actual.epoch<slot.initial_epoch ||
     actual.directory!=slot.source.directory || actual.location!=slot.source.path ||
     actual.marker_file!=bound.commit.marker_file[bound.slot] ||
     actual.prefix_digest!=digest || prefix.uuid!=slot.uuid || prefix.epoch!=slot.initial_epoch ||
     prefix.binding || prefix.format_pending || prefix.source!=slot.source_id ||
     prefix.device!=1 || prefix.directory!=slot.source.directory || prefix.location!=slot.source.path ||
     actual.binding->epoch!=slot.initial_epoch || actual.binding->nonce!=bound.binding.nonce ||
     actual.binding->binding_directory!=slot.binding_directory ||
     actual.binding->pending_digest!=bound.purpose || actual.binding->prefix_digest!=digest)
    return error?error:wire::Conflict();
  return {};
}
bool CacheStorageOwner::AllowsPending(const HostStorageRoot& source,std::string_view id,uint32_t device,
    const ManagedBootstrapSourceDescription* actual) const {
  if(device!=1 || state_->retired->load())return false;
  for(size_t i=0;i<kRoleCount;++i){
    const auto& slot=state_->binding.slots[i];
    if(state_->sources[i].get()!=&source || slot.source_id!=id || !state_->leases[i] ||
       !source.IntentLeaseMatches(*state_->leases[i]))continue;
    if(!actual)return true;
    // Probe is metadata-only: the existing MDI decoder still proves the actual
    // source id/location/native directory. No GuestView/registry access is
    // granted. Full cache-purpose binding must follow before factory admission.
    if(state_->bootstrap_stage==State::BootstrapStage::kProbe)return true;
    std::error_code error;const auto prefix=ManagedDeviceIdentity::DescribeBootstrapWire(slot.prefix,error);
    const auto hash=wire::Hash(slot.prefix,error);
    if(error || actual->uuid!=slot.uuid || actual->epoch!=slot.initial_epoch ||
       actual->source!=id || actual->device!=device || actual->directory!=slot.source.directory ||
       actual->location!=slot.source.path || actual->prefix_digest!=hash || actual->format_pending ||
       prefix.uuid!=slot.uuid || prefix.binding || prefix.format_pending)return false;
    return !actual->binding || (actual->binding->epoch==slot.initial_epoch &&
        actual->binding->binding_directory==slot.binding_directory && actual->binding->nonce==state_->binding.nonce &&
        actual->binding->pending_digest==state_->purpose && actual->binding->prefix_digest==hash);
  }
  return false;
}
std::shared_ptr<CacheStorageOwner> CacheStorageOwner::Open(CacheProductIdentity product,
    Options options,std::error_code& error) {
  return OpenInternal(product,std::move(options),0,error);
}
std::shared_ptr<CacheStorageOwner> CacheStorageOwner::OpenInternal(CacheProductIdentity product,
    Options options,unsigned stop_boundary,std::error_code& error) {
  auto out=std::shared_ptr<CacheStorageOwner>(new CacheStorageOwner(std::make_unique<State>(product,std::move(options))));
  auto& b=*out->state_;error.clear();b.capsules.reserve(4);
  const auto keep_capsule=[&](std::unique_ptr<HostStorageBootstrapDirectory> owned){
    auto* exact=owned.get();if(owned)b.capsules.push_back(std::move(owned));return exact;
  };
  const auto fail=[&]() {b.error=error?error:wire::Conflict();error=b.error;return out;};
  if(global_critical_region::is_held_by_current_thread()){
    error=std::make_error_code(std::errc::operation_would_block);return fail();}
  if(!product.id() || !product.launch_generation() ||
     (product.kind()!=CacheProductIdentity::Kind::kXbox360 && product.kind()!=CacheProductIdentity::Kind::kXboxOriginal) ||
     !b.options.private_parent || b.options.private_parent->read_only() || b.options.private_parent->is_guest_private()){
    error=std::make_error_code(std::errc::invalid_argument);return fail();}
  const auto clean_path=[](const HostStorageRoot& root){const auto path=root.path().native();
    return std::find(path.begin(),path.end(),0)==path.end();};
  if(!clean_path(*b.options.private_parent) || std::any_of(b.options.role_parents.begin(),b.options.role_parents.end(),
      [&](const auto& root){return !root || !clean_path(*root);})){error=std::make_error_code(std::errc::invalid_argument);return fail();}
  const auto describe=[&](const HostStorageRoot& root){
    const auto id=root.IntentRootIdentity(error);
    return wire::Directory{root.path().generic_string(),id.filesystem,id.directory};
  };
  b.binding.kind=uint64_t(product.kind());b.binding.product=product.id();
  b.binding.private_parent=describe(*b.options.private_parent);if(error)return fail();
  const auto space=HostStorageSpace::ForPath(b.options.private_parent->path(),kMinimumFreeBytes,error);
  if(!space || error || !space->NativeAuthorityFor(b.binding.private_parent.filesystem)){
    if(!error)error=Denied();return fail();}
  // The shared real-filesystem reserve is raised before any new directory or
  // control file. Revalidate the exact retained parent after that pool lookup.
  if(describe(*b.options.private_parent)!=b.binding.private_parent || error)return fail();
  std::vector<uint8_t> key;wire::Number(key,b.binding.kind);wire::Number(key,b.binding.product);
  wire::Encode(key,b.binding.private_parent);
  std::array<std::shared_ptr<const HostStorageIntentLease>,3> anchor_leases;
  const std::array<const char*,3> roles{"legacy","cache0","cache1"};
  // Validate the complete parent set BEFORE creating even the first product
  // child. Pairwise physical ancestry/alias overlap would let a slot reset
  // delete another slot's marker/data; mere unequal inode values do not prove
  // disjointness. Reuse the common retained-native ancestry proof.
  for(size_t i=0;i<kRoleCount;++i){
    const auto& parent=b.options.role_parents[i];
    if(!parent || parent->read_only() || parent->is_guest_private()) {error=Denied();return fail();}
    const auto parent_id=parent->IntentRootIdentity(error);if(error)return fail();
    if(parent_id.filesystem!=b.binding.private_parent.filesystem){
      error=std::make_error_code(std::errc::cross_device_link);return fail();}
    error=b.options.private_parent->IntentDisjointFrom(*parent);if(error)return fail();
    for(size_t j=0;j<i;++j){error=parent->IntentDisjointFrom(*b.options.role_parents[j]);if(error)return fail();}
  }
  for(size_t i=0;i<kRoleCount;++i){
    const auto& parent=b.options.role_parents[i];
    auto source=parent->Subroot(product.key(),false,error);
    if(!source && error==std::errc::no_such_file_or_directory){
      error=parent->Create(product.key(),true);
      if(error && error!=std::errc::file_exists)return fail();
      source=parent->Subroot(product.key(),false,error);
    }
    if(!source || error)return fail();
    for(size_t j=0;j<i;++j){error=source->IntentDisjointFrom(*b.sources[j]);if(error)return fail();}
    b.sources[i]=source;
    auto& slot=b.binding.slots[i];slot.source=describe(*source);
    slot.source_id="cache-v1:"+product.key()+":"+roles[i];
    if(error)return fail();wire::Encode(key,slot.source);wire::Text(key,slot.source_id);
    b.leases[i]=source->AcquireIntentLease(error,true);if(!b.leases[i] || error)return fail();
    b.anchors[i]=source->RetainBootstrapNamespace(*b.leases[i],error);if(!b.anchors[i] || error)return fail();
    anchor_leases[i]=b.anchors[i]->RebindFormatLease(*b.leases[i],error);if(!anchor_leases[i] || error)return fail();
    // Deny foreign live source aliases BEFORE writing any cache controls.
    // This explicitly configured parent contains CACHE PRIVATE METADATA ONLY.
    // Protect it before any lower recovery initializer can create a directory;
    // unknown interrupted lower stages remain preserved here, never guest data.
    auto validation_source=source->IndependentNamespaceView(error);if(!validation_source || error)return fail();
    auto pending=validation_source->InstallBootstrapProjection({b.options.private_parent},{},false,{},nullptr,error);
    if(!pending || error)return fail();b.validation_projections.push_back(std::move(pending));
    b.identities[i]=ManagedDeviceIdentity::OpenInternal(source,slot.source_id,1,error,
        false,false,b.leases[i],{}, {},{},out.get());
    if(!b.identities[i] && error!=std::errc::no_such_file_or_directory)return fail();
    if(b.identities[i] && error && error!=std::errc::operation_in_progress)return fail();
    error.clear();
  }
  const auto digest=wire::Hash(key,error);if(error)return fail();
  const auto leaf=".xenia-cache-state-v1-"+wire::Hex(digest);
  const auto final_path=b.options.private_parent->path()/leaf;
  std::string nonce=wire::Hex(std::span(digest).first(16));
  std::shared_ptr<HostStorageRoot> table_raw;
  std::vector<uint8_t> declared;
  HostStorageBootstrapDirectory* table_capsule=nullptr;
  std::vector<std::string> unclaimed_nonces;
  // The table and its recovery directory share this one exact parent EX
  // description. Reopening a second EX while a table capsule retains the
  // first would conflict with our own bootstrap, just like a foreign owner.
  const auto private_lease=b.options.private_parent->AcquireIntentLease(error,true);
  if(!private_lease || error)return fail();
  table_raw=b.options.private_parent->Subroot(leaf,false,error);
  if(!table_raw && error!=std::errc::no_such_file_or_directory)return fail();
  if(table_raw){declared=ReadControl(*table_raw,"owner.v1",error);if(error)return fail();}
  else{
    error.clear();
    for(unsigned attempt=0;attempt<64;++attempt){
      const auto temporary=HostStorageRoot::DirectoryBootstrapTemporary(leaf,nonce);
      const auto observed=b.options.private_parent->ObservePreparationArtifact(temporary,error);
      if(error)return fail();
      if(!observed){
        if(stop_boundary==1)b.options.private_parent->bootstrap_test_boundary_.store(1);
        table_capsule=keep_capsule(b.options.private_parent->BeginBootstrapDirectory(leaf,nonce,error,private_lease));
        if(!table_capsule || error)return fail();table_raw=table_capsule->root();break;
      }
      if(!observed->directory){error=wire::Conflict();return fail();}
      auto raw=b.options.private_parent->Subroot(temporary,false,error);if(!raw || error)return fail();
      const auto control=raw->CaptureSnapshot("owner.v1",65536,error);if(!control || error)return fail();
      if(control->exists()){
        // A complete recognized declaration is resumable only after exact
        // product/root/binding validation below. A foreign/future canonical
        // record is never overwritten or adopted from its name.
        declared.assign(control->bytes().begin(),control->bytes().end());
        auto parsed=wire::DecodeBinding(declared,error);if(error)return fail();
        if(parsed.table.directory!=observed->file_id || parsed.table.path!=final_path.generic_string()){
          error=wire::Conflict();return fail();}
        table_capsule=keep_capsule(b.options.private_parent->ResumeBootstrapDirectory(leaf,temporary,
            observed->file_id,error,private_lease));if(!table_capsule || error)return fail();
        table_raw=table_capsule->root();break;
      }
      // No declaration confers no ownership. Preserve the exact directory in
      // place as an opaque private exclusion; do not parse/delete its payload.
      b.preserved.push_back(raw);unclaimed_nonces.push_back(nonce);
      std::vector<uint8_t> next;wire::Text(next,nonce);wire::Number(next,observed->file_id);
      const auto next_hash=wire::Hash(next,error);if(error)return fail();
      nonce=wire::Hex(std::span(next_hash).first(16));
    }
    if(!table_raw){error=std::make_error_code(std::errc::no_space_on_device);return fail();}
  }
  const bool resume=!declared.empty();
  std::array<HostStorageBootstrapDirectory*,3> binding_capsules{};
  if(resume){
    auto actual=wire::DecodeBinding(declared,error);if(error)return fail();
    if(actual.kind!=b.binding.kind || actual.product!=b.binding.product ||
       actual.private_parent!=b.binding.private_parent || actual.table.path!=final_path.generic_string() ||
       actual.table.filesystem!=b.binding.private_parent.filesystem ||
       table_raw->IntentRootIdentity(error).directory!=actual.table.directory){
      if(!error)error=wire::Conflict();return fail();}
    for(size_t i=0;i<kRoleCount;++i)if(actual.slots[i].source!=b.binding.slots[i].source ||
        actual.slots[i].source_id!=b.binding.slots[i].source_id){error=wire::Conflict();return fail();}
    b.binding=std::move(actual);nonce=wire::Hex(b.binding.nonce);
    if(!table_capsule){
      table_capsule=keep_capsule(b.options.private_parent->ResumeRetainedBootstrapDirectory(leaf,*table_raw,error,private_lease));
      if(error)return fail();
    }
    for(const auto& preserved:b.binding.preserved){
      auto raw=HostStorageRoot::Open(preserved.path,false,error);if(!raw || error)return fail();
      if(describe(*raw)!=preserved || error){if(!error)error=wire::Conflict();return fail();}
      b.preserved.push_back(std::move(raw));
    }
  }else{
    // Missing source markers plus a genuinely new source are mandatory. An
    // existing/foreign marker can never be rebound to a new cache table.
    for(const auto& identity:b.identities)if(identity){error=wire::Conflict();return fail();}
    b.binding.table={final_path.generic_string(),b.binding.private_parent.filesystem,
                      table_raw->IntentRootIdentity(error).directory};if(error)return fail();
    const auto raw_nonce=std::span(digest).first(16);static_cast<void>(raw_nonce);
    for(size_t i=0;i<16;++i){
      const auto digit=[](char c){return c<='9'?c-'0':c-'a'+10;};
      b.binding.nonce[i]=uint8_t((digit(nonce[i*2])<<4)|digit(nonce[i*2+1]));
    }
    for(const auto& old_nonce:unclaimed_nonces)for(size_t i=0;i<kRoleCount;++i){
      const auto temporary=HostStorageRoot::DirectoryBootstrapTemporary(kBindingDirectory,old_nonce);
      const auto observed=b.anchors[i]->ObservePreparationArtifact(temporary,error);if(error)return fail();
      if(observed){if(!observed->directory){error=wire::Conflict();return fail();}
        auto raw=b.anchors[i]->Subroot(temporary,false,error);if(!raw || error)return fail();b.preserved.push_back(std::move(raw));}
    }
    for(const auto& raw:b.preserved)b.binding.preserved.push_back(describe(*raw));if(error)return fail();
  }
  for(size_t i=0;i<kRoleCount;++i){
    auto raw=b.anchors[i]->Subroot(kBindingDirectory,false,error);
    if(raw){
      if(!resume || raw->IntentRootIdentity(error).directory!=b.binding.slots[i].binding_directory){
        if(!error)error=wire::Conflict();return fail();}
      binding_capsules[i]=keep_capsule(b.anchors[i]->ResumeRetainedBootstrapDirectory(kBindingDirectory,*raw,error,anchor_leases[i]));
      if(error)return fail();b.bindings[i]=raw;
    }else if(error==std::errc::no_such_file_or_directory){
      error.clear();const auto temporary=HostStorageRoot::DirectoryBootstrapTemporary(kBindingDirectory,nonce);
      if(resume){binding_capsules[i]=keep_capsule(b.anchors[i]->ResumeBootstrapDirectory(kBindingDirectory,temporary,
          b.binding.slots[i].binding_directory,error,anchor_leases[i]));}
      else{binding_capsules[i]=keep_capsule(b.anchors[i]->BeginBootstrapDirectory(kBindingDirectory,nonce,error,anchor_leases[i]));}
      if(!binding_capsules[i] || error)return fail();b.bindings[i]=binding_capsules[i]->root();
    }else return fail();
    if(!resume){
      auto& slot=b.binding.slots[i];slot.binding_directory=b.bindings[i]->IntentRootIdentity(error).directory;
      slot.prefix=ManagedDeviceIdentity::PrepareBootstrapPrefix(*b.sources[i],slot.source_id,1,error);
      const auto prefix=ManagedDeviceIdentity::DescribeBootstrapWire(slot.prefix,error);
      if(error || prefix.binding || prefix.format_pending){if(!error)error=wire::Conflict();return fail();}
      slot.uuid=prefix.uuid;slot.initial_epoch=prefix.epoch;
    }
  }
  // Authenticate every detached prefix's full source/UUID/epoch/native-root
  // binding before any missing marker can be created from that wire.
  for(const auto& slot:b.binding.slots){
    const auto prefix=ManagedDeviceIdentity::DescribeBootstrapWire(slot.prefix,error);
    if(error || prefix.binding || prefix.format_pending || prefix.uuid!=slot.uuid ||
        prefix.epoch!=slot.initial_epoch || prefix.source!=slot.source_id || prefix.device!=1 ||
        prefix.directory!=slot.source.directory || prefix.location!=slot.source.path){
      if(!error)error=wire::Conflict();return fail();}
  }
  // A committed purpose must retain the exact existing recovery root. Only
  // a fresh bootstrap may initialize it; a missing/replaced root is never
  // reconstructed from its name or accepted as the prior backup authority.
  b.recovery=SnapshotRecoveryOwner::OpenInternal(
      b.options.private_parent,!resume,0,nullptr,private_lease);
  if(!b.recovery || b.recovery->error()){error=b.recovery?b.recovery->error():Denied();return fail();}
  const auto recovery_identity=describe(*b.recovery->root());if(error)return fail();
  if(resume && b.binding.recovery!=recovery_identity){error=wire::Conflict();return fail();}
  b.binding.recovery=recovery_identity;
  b.binding_wire=wire::EncodeBinding(b.binding,error);if(error)return fail();
  if(resume && !Equal(b.binding_wire,declared)){error=wire::Conflict();return fail();}
  b.purpose=wire::Hash(b.binding_wire,error);if(error)return fail();
  b.bootstrap_stage=State::BootstrapStage::kDeclared;
  // Each temporary validation namespace remains SourceUnadmitted. The
  // original source itself is still unprojected for its ONE final install.
  error=PublishControl(*table_raw,"owner.v1",b.binding_wire,nonce);if(error)return fail();
  for(const auto& binding:b.bindings){error=PublishControl(*binding,"owner.v1",b.binding_wire,nonce);if(error)return fail();}
  if(stop_boundary==2){error=std::make_error_code(std::errc::operation_canceled);return fail();}
  wire::Table initial;initial.purpose=b.purpose;
  for(size_t i=0;i<kRoleCount;++i){initial.uuid[i]=b.binding.slots[i].uuid;initial.epoch[i]=b.binding.slots[i].initial_epoch;}
  const auto initial_wire=wire::EncodeTable(initial,error);if(error)return fail();
  wire::Commit complete;complete.purpose=b.purpose;
  const auto prior_commit=table_raw->CaptureSnapshot("bootstrap.commit.v1",4096,error);
  if(!prior_commit || error)return fail();
  const bool committed=prior_commit->exists();
  if(committed){complete=wire::DecodeCommit(prior_commit->bytes(),error);
    if(error || complete.purpose!=b.purpose){if(!error)error=wire::Conflict();return fail();}}
  const auto existing_table=table_raw->CaptureSnapshot("cache-table.v1",4096,error);if(!existing_table || error)return fail();
  if(!existing_table->exists()){
    if(committed){error=wire::Conflict();return fail();}
    error=PublishControl(*table_raw,"cache-table.v1",initial_wire,nonce);if(error)return fail();
  }else if(!committed && !Equal(existing_table->bytes(),initial_wire)){
    error=wire::Conflict();return fail();
  }
  if(table_capsule){
    const auto published=b.options.private_parent->PublishBootstrapDirectory(*table_capsule);
    error=published.error;if(!published.canonical_root || error || !published.durable){if(!error)error=wire::Conflict();return fail();}
    table_raw=published.canonical_root;
  }
  for(size_t i=0;i<kRoleCount;++i)if(binding_capsules[i]){
    const auto published=b.anchors[i]->PublishBootstrapDirectory(*binding_capsules[i]);
    error=published.error;if(!published.canonical_root || error || !published.durable){if(!error)error=wire::Conflict();return fail();}
    b.bindings[i]=published.canonical_root;
  }
  b.table_root=table_raw;
  std::vector<std::shared_ptr<const HostStorageRoot>> exclusions{b.options.private_parent};
  exclusions.insert(exclusions.end(),b.bindings.begin(),b.bindings.end());
  for(const auto& root:b.preserved)if(root->path().parent_path()!=b.options.private_parent->path())exclusions.push_back(root);
  std::vector<HostStorageBootstrapDirectory*> settled;
  if(table_capsule)settled.push_back(table_capsule);
  for(auto* capsule:binding_capsules)if(capsule)settled.push_back(capsule);
  for(size_t i=0;i<kRoleCount;++i){
    auto projection=b.sources[i]->InstallBootstrapProjection(exclusions,{},true,
        i?std::span<HostStorageBootstrapDirectory* const>{}:std::span<HostStorageBootstrapDirectory* const>(settled),nullptr,error);
    if(!projection || error)return fail();b.projections[i]=std::move(projection);
  }
  // The final exact exclusion cohort has replaced all temporary seals. Only
  // these owned validation projections retire; failed initialization retains
  // them until actual teardown. No stale seal can deny a completed reopen.
  b.validation_projections.clear();
  b.table_root=table_raw->ProtectFlat(error);if(!b.table_root || error)return fail();
  for(auto& binding:b.bindings){binding=binding->ProtectFlat(error);if(!binding || error)return fail();}
  if(stop_boundary==3){error=std::make_error_code(std::errc::operation_canceled);return fail();}
  bool pending_format=false;
  for(size_t i=0;i<kRoleCount;++i){
    const auto& slot=b.binding.slots[i];
    if(!b.identities[i]){
      if(committed){error=wire::Conflict();return fail();}
      // A crash during marker creation may have left this one declared native
      // temporary. Its inode is observed, preserved by same-FS CAS, and never
      // interpreted as marker bytes. The destination is the already proven
      // private table directory; a name alone supplies no move authority.
      const auto temporary=HostStorageRoot::MetadataBootstrapTemporary(nonce);
      const auto partial=b.anchors[i]->ObservePreparationArtifact(temporary,error);
      if(error)return fail();
      if(partial){
        if(!partial->regular || partial->directory || partial->links!=1){error=wire::Conflict();return fail();}
        auto preserve=HostStorageRoot::Open(b.table_root->path(),false,error);
        if(!preserve || error || describe(*preserve)!=b.binding.table){
          if(!error)error=wire::Conflict();return fail();}
        const auto destination="preserved-marker-"+std::to_string(i)+"-"+nonce+"-"+std::to_string(partial->file_id);
        error=b.anchors[i]->PreservePreparationArtifact(temporary,partial->file_id,false,*preserve,destination);
        if(error)return fail();b.preserved_files.push_back(preserve->path()/destination);
      }
      std::vector<std::shared_ptr<const HostStorageRoot>> opaque;
      for(const auto& root:b.preserved)if(root->path().parent_path()==b.sources[i]->path())opaque.push_back(root);
      error=b.sources[i]->CheckCacheBootstrapSource(b.bindings[i].get(),0,opaque);
      if(error)return fail();
      ManagedBootstrapReference reference;reference.epoch=slot.initial_epoch;reference.binding_directory=slot.binding_directory;
      reference.nonce=b.binding.nonce;reference.pending_digest=b.purpose;reference.prefix_digest=wire::Hash(slot.prefix,error);
      auto initial=slot.prefix;const auto suffix=ManagedDeviceIdentity::BootstrapSuffix(initial,reference,error);
      if(error)return fail();wire::Raw(initial,suffix);
      if(!i && (stop_boundary==4 || stop_boundary==8))
        b.sources[i]->bootstrap_test_boundary_.store(stop_boundary==4?1:4);
      b.identities[i]=ManagedDeviceIdentity::OpenInternal(b.sources[i],slot.source_id,1,error,
          true,false,b.leases[i],{},initial,nonce,out.get());
      if(!b.identities[i] || error)return fail();
    }
    const auto actual=b.identities[i]->DescribeBootstrapCurrent(error);if(error)return fail();
    if(!actual.binding || actual.uuid!=slot.uuid || actual.prefix_digest!=wire::Hash(slot.prefix,error) ||
       actual.binding->nonce!=b.binding.nonce || actual.binding->pending_digest!=b.purpose ||
       actual.binding->binding_directory!=slot.binding_directory ||
       (committed && complete.marker_file[i]!=actual.marker_file)){
      if(!error)error=wire::Conflict();return fail();}
    complete.marker_file[i]=actual.marker_file;pending_format|=actual.format_pending;
    if(!i && stop_boundary==5){error=std::make_error_code(std::errc::operation_canceled);return fail();}
    if(!committed){
      std::vector<std::shared_ptr<const HostStorageRoot>> opaque;
      for(const auto& root:b.preserved)if(root->path().parent_path()==b.sources[i]->path())opaque.push_back(root);
      error=b.sources[i]->CheckCacheBootstrapSource(b.bindings[i].get(),actual.marker_file,opaque);
      if(error)return fail();
    }
  }
  const auto commit_wire=wire::EncodeCommit(complete,error);if(error)return fail();
  for(const auto& binding:b.bindings){error=PublishControl(*binding,"bootstrap.commit.v1",commit_wire,nonce);if(error)return fail();}
  error=PublishControl(*b.table_root,"bootstrap.commit.v1",commit_wire,nonce);if(error)return fail();
  b.bootstrap_committed=true;b.bootstrap_stage=State::BootstrapStage::kCommitted;
  if(stop_boundary==6){error=std::make_error_code(std::errc::operation_canceled);return fail();}
  if(pending_format){b.capsules.clear();b.leases={};error=Pending();return fail();}
  error=b.LoadTable();if(error)return fail();
  b.admission_open.store(true,std::memory_order_release);
  for(size_t i=0;i<kRoleCount;++i){
    ManagedBootstrapGuestAdmissionPermit admission(out,b.sources[i],b.leases[i],b.identities[i],b.projections[i]);
    error=b.sources[i]->AdmitBootstrapProjection(admission);if(error)return fail();
    error=b.identities[i]->AdmitBootstrapIdentity();if(error)return fail();
    auto guest=b.sources[i]->GuestView(error);if(!guest || error)return fail();
    if(!guest->BindCacheCancellation(b.retired,b.slot_retired[i])){error=Denied();return fail();}
    b.guest[i].store(std::move(guest),std::memory_order_release);
    if(!i && stop_boundary==7){error=std::make_error_code(std::errc::operation_canceled);return fail();}
  }
  b.capsules.clear();b.leases={};
  b.accepting.store(true,std::memory_order_release);b.error.clear();error.clear();return out;
}
CacheStorageOwner::CacheStorageOwner(std::unique_ptr<State> state):state_(std::move(state)){}
CacheStorageOwner::~CacheStorageOwner(){Retire();CloseNativeAfterIoDrained();}
const CacheProductIdentity& CacheStorageOwner::product() const {return state_->product;}
std::error_code CacheStorageOwner::error() const {std::lock_guard lock(state_->mutex);
  return state_->error?state_->error:(state_->retired->load()?std::make_error_code(std::errc::no_such_device):std::error_code{});}
std::vector<std::filesystem::path> CacheStorageOwner::preserved_paths() const {
  std::lock_guard lock(state_->mutex);auto out=state_->preserved_files;
  for(const auto& root:state_->binding.preserved)out.emplace_back(root.path);return out;
}
bool CacheStorageOwner::current_cached() const noexcept {
  return state_->accepting.load(std::memory_order_acquire) && !state_->retired->load(std::memory_order_acquire);
}
bool CacheStorageOwner::AdmissionCurrent() const noexcept {
  return state_->admission_open.load(std::memory_order_acquire) && !state_->retired->load(std::memory_order_acquire);
}
std::shared_ptr<CacheStorageOwner::MetadataFileUse> CacheStorageOwner::AcquireMetadataFile() const {
  if(!current_cached() || state_->format_active.load(std::memory_order_acquire))return {};
  auto use=std::make_shared<MetadataFileUse>(state_->metadata_files);
  use->count->fetch_add(1,std::memory_order_acq_rel);use->admitted=true;
  if(!current_cached() || state_->format_active.load(std::memory_order_acquire))return {};
  return use;
}
std::unique_ptr<CacheStorageOwner::RegistrationAdmission> CacheStorageOwner::TryAdmitRegistration(Role role,
    const std::shared_ptr<HostStorageRoot>& root,
    const std::shared_ptr<const ManagedDeviceIdentity>& identity,uint32_t sectors) const {
  auto held=std::make_unique<RegistrationAdmission::Impl>(shared_from_this());
  if(!held->lock.owns_lock())return {};const auto slot=Index(role);
  if(slot>=kRoleCount || !current_cached() || state_->format_active.load(std::memory_order_acquire) ||
      state_->native_closing || state_->native_closed || state_->guest[slot].load()!=root ||
      state_->identities[slot]!=identity || !root || root->retiring() || !identity || identity->retiring() ||
      state_->table.uuid[slot]!=identity->uuid() || state_->table.epoch[slot]!=identity->content_epoch() ||
      (slot?state_->table.logical.sectors_per_cluster[slot-1]:1)!=sectors)return {};
  return std::unique_ptr<RegistrationAdmission>(new RegistrationAdmission(std::move(held)));
}
bool CacheStorageOwner::BindRegistry(const std::shared_ptr<const uint8_t>& issuer) noexcept {
  if(!issuer || !current_cached())return false;
  std::unique_lock lock(state_->mutex,std::try_to_lock);if(!lock.owns_lock() || !current_cached())return false;
  if(state_->issuer_bound)return !state_->registry_issuer.expired() &&
      !state_->registry_issuer.owner_before(issuer) && !issuer.owner_before(state_->registry_issuer);
  state_->registry_issuer=issuer;state_->issuer_bound=true;return true;
}
void CacheStorageOwner::Retire() noexcept {
  state_->retired->store(true,std::memory_order_release);
  state_->accepting.store(false,std::memory_order_release);
  state_->admission_open.store(false,std::memory_order_release);
}
bool CacheStorageOwner::CloseNativeAfterIoDrained() noexcept {
  Retire();
  if(global_critical_region::is_held_by_current_thread())return false;
  // Declared before the lock: native owners are destroyed only after unlock.
  Options options;
  std::array<std::shared_ptr<HostStorageRoot>,kRoleCount> sources;
  std::array<std::shared_ptr<const HostStorageRoot>,kRoleCount> anchors;
  std::array<std::shared_ptr<HostStorageRoot>,kRoleCount> guests,bindings;
  std::array<std::shared_ptr<const HostStorageIntentLease>,kRoleCount> leases;
  std::array<std::shared_ptr<const ManagedDeviceIdentity>,kRoleCount> identities;
  std::array<std::shared_ptr<const HostStorageGuestProjection>,kRoleCount> projections;
  std::vector<std::unique_ptr<HostStorageBootstrapDirectory>> capsules;
  std::vector<std::shared_ptr<HostStorageRoot>> preserved;
  std::vector<std::shared_ptr<const HostStorageGuestProjection>> validation;
  std::shared_ptr<HostStorageRoot> table;
  std::shared_ptr<const SnapshotRecoveryOwner> recovery;
  std::shared_ptr<const HostStorageSnapshotToken> snapshot;
  std::unique_ptr<ManagedDeviceIdentity::Handoff> handoff;
  std::unique_ptr<PreparedSnapshotPublication> prepared;
  std::shared_ptr<CacheFormatHandoff::State> active;
  std::unique_lock lock(state_->mutex,std::try_to_lock);if(!lock.owns_lock())return false;
  if(state_->native_closed.load(std::memory_order_acquire))return true;
  if(state_->native_closing)return false;
  active=state_->active_handoff.lock();
  // A weak-expired handoff may still be in its real destructor. It publishes
  // format_active=false only after its native fields are actually released.
  if(!active && state_->format_active.load(std::memory_order_acquire))return false;
  state_->native_closing=true;
  for(const auto& id:state_->identities)if(id)id->Retire();
  if(active){handoff=std::move(active->native);prepared=std::move(active->prepared);
    active->owns_operation=false;}
  state_->active_handoff.reset();state_->format_active.store(false,std::memory_order_release);
  options=std::move(state_->options);sources=std::move(state_->sources);anchors=std::move(state_->anchors);
  leases=std::move(state_->leases);identities=std::move(state_->identities);
  bindings=std::move(state_->bindings);projections=std::move(state_->projections);
  for(size_t i=0;i<kRoleCount;++i)guests[i]=state_->guest[i].exchange({},std::memory_order_acq_rel);
  capsules=std::move(state_->capsules);preserved=std::move(state_->preserved);
  validation=std::move(state_->validation_projections);
  table=std::move(state_->table_root);recovery=std::move(state_->recovery);snapshot=std::move(state_->table_snapshot);
  lock.unlock();
  handoff.reset();prepared.reset();active.reset();capsules.clear();
  identities={};leases={};projections={};validation.clear();guests={};
  snapshot.reset();recovery.reset();table.reset();bindings={};preserved.clear();sources={};anchors={};options={};
  // A concurrent close observes closing until the last moved native marker/EX
  // owner above has genuinely gone. A bool never stands in for that rundown.
  state_->native_closed.store(true,std::memory_order_release);return true;
}
std::error_code CacheStorageOwner::CheckCurrent() const {
  if(global_critical_region::is_held_by_current_thread())return std::make_error_code(std::errc::operation_would_block);
  std::unique_lock lock(state_->mutex,std::try_to_lock);if(!lock.owns_lock())return Busy();
  if(!current_cached())return state_->error?state_->error:
      (state_->retired->load()?std::make_error_code(std::errc::no_such_device):Pending());
  for(const auto& identity:state_->identities){if(!identity)return Denied();if(auto e=identity->CheckCurrent())return e;}
  return state_->LoadTable();
}
std::shared_ptr<HostStorageRoot> CacheStorageOwner::CaptureRoot(Role role,std::error_code& error) const {
  error=CheckCurrent();if(error)return {};
  std::lock_guard lock(state_->mutex);const auto i=Index(role);
  auto root=i<kRoleCount?state_->guest[i].load(std::memory_order_acquire):nullptr;
  if(!current_cached() || !root || root->retiring()){
    error=std::make_error_code(std::errc::no_such_device);return {};}
  return root;
}
std::shared_ptr<const HostStorageRoot> CacheStorageOwner::NativeSource(Role role) const {
  std::lock_guard lock(state_->mutex);const auto i=Index(role);return i<kRoleCount?state_->sources[i]:nullptr;
}
std::shared_ptr<const ManagedDeviceIdentity> CacheStorageOwner::CaptureIdentity(Role role) const {
  std::lock_guard lock(state_->mutex);const auto i=Index(role);
  return i<kRoleCount && current_cached()?state_->identities[i]:nullptr;
}
CacheStorageOwner::LogicalTable CacheStorageOwner::table(std::error_code& error) const {
  error=CheckCurrent();if(error)return {};
  std::lock_guard lock(state_->mutex);if(!current_cached()){error=Pending();return {};}
  return state_->table.logical;
}
std::error_code CacheStorageOwner::ReadTable(uint64_t offset,std::span<uint8_t> out) const {
  // Validate the complete request before any output byte is touched. A table
  // overlap never claims unsupported neighboring sectors or wrapped IO.
  if(offset>kTableBytes || out.size()>kTableBytes-offset)return std::make_error_code(std::errc::operation_not_supported);
  std::error_code e;const auto snapshot=table(e);if(e)return e;
  std::copy_n(snapshot.bytes.begin()+size_t(offset),out.size(),out.begin());return {};
}
std::error_code CacheStorageOwner::FlushTable() const {
  if(global_critical_region::is_held_by_current_thread())return std::make_error_code(std::errc::operation_would_block);
  std::unique_lock lock(state_->mutex,std::try_to_lock);if(!lock.owns_lock())return Busy();
  if(!current_cached() || state_->format_active.load())return Pending();
  if(auto error=state_->LoadTable())return error;
  const auto observed=state_->table_root->ObserveIntentSnapshot(*state_->table_snapshot);
  return observed.error?observed.error:(observed.matched && observed.durable?std::error_code{}:wire::Conflict());
}
CacheStorageOwner::Receipt CacheStorageOwner::WriteTable(uint64_t offset,std::span<const uint8_t> bytes,
    const std::atomic<bool>* canceled) {
  Receipt receipt;
  if(global_critical_region::is_held_by_current_thread()){
    receipt.error=std::make_error_code(std::errc::operation_would_block);return receipt;}
  if(offset>kTableBytes || bytes.size()>kTableBytes-offset){receipt.error=std::make_error_code(std::errc::operation_not_supported);return receipt;}
  std::unique_lock lock(state_->mutex,std::try_to_lock);if(!lock.owns_lock()){receipt.error=Busy();return receipt;}
  if(!current_cached() || state_->format_active.load()){receipt.error=Pending();return receipt;}
  for(const auto& id:state_->identities){
    if(!id){receipt.error=Denied();return receipt;}
    if((receipt.error=id->CheckCurrent()))return receipt;
  }
  receipt.error=state_->LoadTable();if(receipt.error)return receipt;
  auto next=state_->table;std::copy(bytes.begin(),bytes.end(),next.logical.bytes.begin()+size_t(offset));
  const auto encoded=wire::EncodeTable(next,receipt.error);if(receipt.error)return receipt;
  const auto published=SnapshotPublisher::PublishCacheTable(*state_->recovery,state_->table_snapshot,encoded,canceled);
  receipt.error=published.error;receipt.table_published=published.published;receipt.table_durable=published.durable;
  if(published.published && (!published.current || !published.durable || published.error)){
    state_->accepting.store(false,std::memory_order_release);state_->error=receipt.error?receipt.error:wire::Conflict();
    state_->retired->store(true,std::memory_order_release);
  }
  if(!receipt.error && published.durable && published.current){
    state_->table=std::move(next);state_->table_snapshot=published.current;receipt.complete=true;
  }
  return receipt;
}
std::unique_ptr<CacheFormatHandoff> CacheStorageOwner::CaptureFormat(Role role,
    uint32_t sectors,const std::atomic<bool>* canceled) {
  auto operation=std::make_shared<CacheFormatHandoff::State>();
  operation->owner=shared_from_this();operation->slot=Index(role);
  auto ticket=std::unique_ptr<CacheFormatHandoff>(new CacheFormatHandoff(std::move(operation)));
  auto& h=*ticket->state_;auto& receipt=ticket->receipt_;
  if(global_critical_region::is_held_by_current_thread()){
    receipt.error=std::make_error_code(std::errc::operation_would_block);return ticket;}
  if(h.slot>=kRoleCount || (h.slot?!wire::Mountable(sectors):sectors!=0)){
    receipt.error=std::make_error_code(std::errc::invalid_argument);return ticket;}
  std::unique_lock lock(state_->mutex,std::try_to_lock);if(!lock.owns_lock()){receipt.error=Busy();return ticket;}
  bool no=false;
  if(!current_cached() || !state_->format_active.compare_exchange_strong(no,true,std::memory_order_acq_rel)){
    receipt.error=Busy();return ticket;}
  h.owns_operation=true;state_->active_handoff=ticket->state_;
  // No native utility initiator ABI has been proved yet. Conservatively require
  // ALL actual logical metadata File leases to close; no public bool/counter
  // exception pretends an initiating handle was authenticated.
  if(state_->metadata_files->load(std::memory_order_acquire)){receipt.error=Busy();return ticket;}
  if(canceled && canceled->load(std::memory_order_acquire)){
    receipt.error=std::make_error_code(std::errc::operation_canceled);return ticket;}
  for(const auto& identity:state_->identities){
    if(!identity){receipt.error=Denied();return ticket;}
    if((receipt.error=identity->CheckCurrent()))return ticket;
  }
  receipt.error=state_->LoadTable();if(receipt.error)return ticket;
  if(state_->table.epoch[h.slot]==UINT64_MAX){receipt.error=std::make_error_code(std::errc::value_too_large);return ticket;}
  const auto journal=state_->table_root->CaptureSnapshot(FormatLeaf(h.slot),16384,receipt.error);
  if(!journal || receipt.error)return ticket;
  if(journal->exists()){
    const auto prior=wire::DecodeFormat(journal->bytes(),receipt.error);
    if(receipt.error || prior.purpose!=state_->purpose || prior.slot!=h.slot){
      if(!receipt.error)receipt.error=wire::Conflict();return ticket;}
    const auto before=wire::DecodeTable(prior.before,receipt.error);
    const auto after=wire::DecodeTable(prior.intended,receipt.error);
    if(receipt.error || before.purpose!=state_->purpose || after.purpose!=state_->purpose ||
       before.epoch[h.slot]>state_->table.epoch[h.slot]){
      if(!receipt.error)receipt.error=wire::Conflict();return ticket;}
    if(before.epoch[h.slot]==state_->table.epoch[h.slot]){
      // An interrupted plan BEFORE native Prepared owns no deletion/publication
      // right. Prove its actual marker still has no pending native handoff and
      // the exact old table remains. Preserve its private stage/backup paths;
      // the replacement journal's normal durable backup preserves old evidence.
      const auto actual=state_->identities[h.slot]->DescribeBootstrapCurrent(receipt.error);
      if(receipt.error || actual.format_pending || prior.before_file!=state_->table_snapshot->info().file_id ||
          !Equal(state_->table_snapshot->bytes(),prior.before)){
        if(!receipt.error)receipt.error=wire::Conflict();return ticket;}
      state_->preserved_files.push_back(state_->table_root->path()/prior.stage);
      state_->preserved_files.push_back(state_->recovery->root()->path()/prior.recovery);
    }
  }
  auto next=state_->table;++next.epoch[h.slot];
  if(h.slot)next.logical.sectors_per_cluster[h.slot-1]=sectors;
  h.plan.purpose=state_->purpose;h.plan.slot=h.slot;h.plan.sectors=sectors;
  h.plan.before_file=state_->table_snapshot->info().file_id;
  h.plan.before.assign(state_->table_snapshot->bytes().begin(),state_->table_snapshot->bytes().end());
  h.plan.intended=wire::EncodeTable(next,receipt.error);if(receipt.error)return ticket;
  h.prepared=SnapshotPublisher::PrepareCacheTable(*state_->recovery,state_->table_snapshot,h.plan.intended,canceled);
  if(!h.prepared || h.prepared->receipt().error){receipt.error=h.prepared?h.prepared->receipt().error:Denied();return ticket;}
  const auto& prepared=h.prepared->receipt();
  h.plan.stage=prepared.stage.generic_string();h.plan.stage_file=h.prepared->stage_info().file_id;
  h.plan.recovery=prepared.recovery.generic_string();
  if(!h.prepared->recovery_ || !h.prepared->recovery_->exists()){
    receipt.error=wire::Conflict();return ticket;}
  h.plan.recovery_file=h.prepared->recovery_->info().file_id;
  h.plan_wire=wire::EncodeFormat(h.plan,receipt.error);if(receipt.error)return ticket;
  // An authenticated bounded intent records the REAL stage/backup identities
  // before the existing native MDI Prepared/pending-epoch engine can begin.
  // It is cache-purpose metadata; no second deletion or recovery engine.
  const auto saved=SnapshotPublisher::PublishCacheTable(*state_->recovery,journal,h.plan_wire,canceled);
  receipt.error=saved.error;
  if(receipt.error || !saved.durable || !saved.current){if(!receipt.error)receipt.error=wire::Conflict();return ticket;}
  const auto binding=wire::Hash(h.plan_wire,receipt.error);if(receipt.error)return ticket;
  h.native=ManagedDeviceIdentity::PrepareHandoff(state_->identities[h.slot],binding,
      canceled,state_->retired.get());
  if(!h.native){receipt.error=Denied();return ticket;}
  receipt.error=h.native->error;receipt.prepared_durable=h.native->prepared_durable;
  receipt.epoch=h.native->old_epoch+1;
  if(h.native->phase || h.native->prepared_durable){
    // Retire only the exact guest child. Its retained native physical source,
    // marker EX and private anchors remain usable by the owned transaction.
    state_->accepting.store(false,std::memory_order_release);
    state_->slot_retired[h.slot]->store(true,std::memory_order_release);
  }
  return ticket;
}
std::error_code CacheStorageOwner::CheckGuestAdmission(
    const ManagedBootstrapGuestAdmissionPermit& permit) const {
  const auto& b=*state_;
  if(!AdmissionCurrent() || !b.bootstrap_committed || b.native_closed ||
      !b.recovery || b.recovery->error() || !b.table_snapshot)return Denied();
  for(size_t i=0;i<kRoleCount;++i)if(permit.source_==b.sources[i]){
    if(permit.lease_!=b.leases[i] || !permit.lease_ || permit.identity_!=b.identities[i] ||
        permit.projection_!=b.projections[i] || !permit.identity_ ||
        permit.source_->Projection()!=permit.projection_ ||
        !permit.source_->IntentLeaseMatches(*permit.lease_))return Denied();
    auto error=permit.identity_->CheckCurrent();if(error)return error;
    error=b.recovery->CheckTarget(*b.table_root);if(error)return error;
    const auto actual=b.table_root->ObserveIntentSnapshot(*b.table_snapshot);
    return actual.error?actual.error:(actual.matched && actual.durable?std::error_code{}:wire::Conflict());
  }
  return Denied();
}
std::error_code CacheStorageOwner::CheckFormatGuestAdmission(
    const ManagedFormatGuestAdmissionPermit& permit) {
  if(!permit.cache_)return Denied();const auto& b=*permit.cache_->state_;
  const auto& h=permit.handoff_;
  if(!permit.cache_->AdmissionCurrent() || b.native_closed ||
      h.identity!=permit.identity_ || h.recovery_source!=permit.source_ ||
      h.source_lease!=permit.lease_ || h.projection!=permit.projection_ ||
      h.observation_source!=permit.observation_source_ ||
      h.observation_projection!=permit.observation_projection_ ||
      !b.recovery || b.recovery->error() || !b.table_snapshot)return Denied();
  for(size_t i=0;i<kRoleCount;++i)if(b.sources[i]==permit.source_){
    if(b.identities[i]!=permit.identity_ || b.projections[i]!=permit.projection_ ||
        b.table.epoch[i]!=h.old_epoch+1 || !permit.identity_ ||
        b.table.uuid[i]!=permit.identity_->uuid())return Denied();
    auto error=ManagedDeviceIdentity::CheckCompletedHandoff(h);if(error)return error;
    const auto actual=b.table_root->ObserveIntentSnapshot(*b.table_snapshot);
    return actual.error?actual.error:(actual.matched && actual.durable?std::error_code{}:wire::Conflict());
  }
  return Denied();
}
namespace {
std::error_code ValidateFormatPlan(const wire::FormatPlan& plan,
    const wire::Digest& purpose,size_t slot,wire::Table& before,wire::Table& after) {
  std::error_code error;
  before=wire::DecodeTable(plan.before,error);after=wire::DecodeTable(plan.intended,error);
  if(error || plan.purpose!=purpose || plan.slot!=slot || slot>=3 ||
      before.purpose!=purpose || after.purpose!=purpose ||
      plan.before_file==plan.stage_file || plan.before_file==plan.recovery_file ||
      plan.stage_file==plan.recovery_file || before.epoch[slot]==UINT64_MAX ||
      before.logical.bytes!=after.logical.bytes)return error?error:wire::Conflict();
  auto expected=before;++expected.epoch[slot];
  if(slot)expected.logical.sectors_per_cluster[slot-1]=uint32_t(plan.sectors);
  return expected==after?std::error_code{}:wire::Conflict();
}
}
std::vector<CacheStorageOwner::Role> CacheStorageOwner::PendingFormats(std::error_code& error) const {
  error.clear();std::vector<Role> roles;
  if(global_critical_region::is_held_by_current_thread()){
    error=std::make_error_code(std::errc::operation_would_block);return roles;}
  std::unique_lock lock(state_->mutex,std::try_to_lock);if(!lock.owns_lock()){error=Busy();return roles;}
  if(state_->retired->load() || state_->native_closed || !state_->bootstrap_committed){
    error=state_->error?state_->error:Denied();return roles;}
  for(size_t i=0;i<kRoleCount;++i){
    if(!state_->identities[i]){error=Denied();return {};}
    const auto actual=state_->identities[i]->DescribeBootstrapCurrent(error);if(error)return {};
    if(actual.format_pending)roles.push_back(Role(i));
  }
  return roles;
}
std::unique_ptr<CacheFormatHandoff> CacheStorageOwner::InspectRecovery(Role role) {
  auto operation=std::make_shared<CacheFormatHandoff::State>();
  operation->owner=shared_from_this();operation->slot=Index(role);
  auto ticket=std::unique_ptr<CacheFormatHandoff>(new CacheFormatHandoff(std::move(operation)));
  auto& h=*ticket->state_;auto& receipt=ticket->receipt_;
  if(global_critical_region::is_held_by_current_thread()){
    receipt.error=std::make_error_code(std::errc::operation_would_block);return ticket;}
  std::unique_lock lock(state_->mutex,std::try_to_lock);if(!lock.owns_lock()){receipt.error=Busy();return ticket;}
  bool no=false;
  if(h.slot>=kRoleCount || state_->retired->load() || state_->native_closed ||
      !state_->bootstrap_committed || !state_->identities[h.slot] ||
      !state_->table_root || !state_->recovery || state_->recovery->error()){
    receipt.error=Denied();return ticket;}
  if(!state_->format_active.compare_exchange_strong(no,true,std::memory_order_acq_rel)){
    receipt.error=Busy();return ticket;}
  h.owns_operation=true;state_->active_handoff=ticket->state_;
  if(state_->metadata_files->load(std::memory_order_acquire)){receipt.error=Busy();return ticket;}
  const auto journal=state_->table_root->CaptureSnapshot(FormatLeaf(h.slot),16384,receipt.error);
  if(!journal || receipt.error || !journal->exists()){
    if(!receipt.error)receipt.error=std::make_error_code(std::errc::no_such_file_or_directory);return ticket;}
  h.plan_wire.assign(journal->bytes().begin(),journal->bytes().end());
  h.plan=wire::DecodeFormat(h.plan_wire,receipt.error);if(receipt.error)return ticket;
  wire::Table before,after;receipt.error=ValidateFormatPlan(h.plan,state_->purpose,h.slot,before,after);
  if(receipt.error)return ticket;
  for(size_t i=0;i<kRoleCount;++i)if(before.uuid[i]!=state_->binding.slots[i].uuid){
    receipt.error=wire::Conflict();return ticket;}
  const auto binding=wire::Hash(h.plan_wire,receipt.error);if(receipt.error)return ticket;
  h.native=ManagedDeviceIdentity::InspectHandoff(state_->identities[h.slot],binding);
  if(!h.native){receipt.error=Denied();return ticket;}
  receipt.error=h.native->error;receipt.prepared_durable=h.native->prepared_durable;
  receipt.epoch=h.native->old_epoch+1;
  if(receipt.error || before.epoch[h.slot]!=h.native->old_epoch){
    if(!receipt.error)receipt.error=wire::Conflict();h.native.reset();return ticket;}
  state_->accepting.store(false,std::memory_order_release);
  state_->slot_retired[h.slot]->store(true,std::memory_order_release);
  return ticket;
}
CacheStorageOwner::Receipt CacheStorageOwner::ExecuteFormat(CacheFormatHandoff& ticket) {
  return FinishFormat(ticket,false);
}
CacheStorageOwner::Receipt CacheStorageOwner::ResumeFormat(CacheFormatHandoff& ticket) {
  return FinishFormat(ticket,true);
}
CacheStorageOwner::Receipt CacheStorageOwner::FinishFormat(CacheFormatHandoff& ticket,bool resume,unsigned stop_boundary) {
  auto& receipt=ticket.receipt_;
  if(!ticket.state_ || !ticket.state_->owner){receipt.error=Denied();return receipt;}
  auto& h=*ticket.state_;auto& owner=*h.owner;auto& b=*owner.state_;
  if(global_critical_region::is_held_by_current_thread()){
    receipt.error=std::make_error_code(std::errc::operation_would_block);return receipt;}
  std::unique_lock lock(b.mutex,std::try_to_lock);if(!lock.owns_lock()){receipt.error=Busy();return receipt;}
  if(receipt.complete)return receipt;
  if(!h.owns_operation || b.active_handoff.lock()!=ticket.state_ ||
      b.retired->load(std::memory_order_acquire) || b.native_closed || !h.native ||
      !b.table_root || !b.recovery || b.recovery->error()){
    receipt.error=Denied();return receipt;}
  // Original failure can be retried ONLY through the real native reopen path.
  // Its nonce/binding/old epoch are then re-observed, never cleared by a bool.
  if(resume){
    h.prepared.reset();receipt.error=ManagedDeviceIdentity::ReopenHandoffForResume(*h.native);
    if(receipt.error)return receipt;
  }else if(receipt.error)return receipt;
  const auto journal=b.table_root->CaptureSnapshot(FormatLeaf(h.slot),16384,receipt.error);
  if(!journal || receipt.error || !journal->exists() || !Equal(journal->bytes(),h.plan_wire)){
    if(!receipt.error)receipt.error=wire::Conflict();return receipt;}
  const auto digest=wire::Hash(h.plan_wire,receipt.error);
  wire::Table before,after;
  if(!receipt.error)receipt.error=ValidateFormatPlan(h.plan,b.purpose,h.slot,before,after);
  if(receipt.error || digest!=h.native->binding || before.epoch[h.slot]!=h.native->old_epoch){
    if(!receipt.error)receipt.error=wire::Conflict();return receipt;}
  for(size_t i=0;i<kRoleCount;++i)if(before.uuid[i]!=b.binding.slots[i].uuid){
    receipt.error=wire::Conflict();return receipt;}
  // Validate BOTH real staged replacement and durable old-byte backup before
  // starting native data reset. Replayed publication is accepted only by the
  // exact recorded stage inode, never just equal intended bytes.
  const auto backup=b.recovery->root()->CaptureSnapshot(h.plan.recovery,h.plan.before.size(),receipt.error);
  if(!backup || receipt.error || !backup->exists() || backup->info().file_id!=h.plan.recovery_file ||
      !Equal(backup->bytes(),h.plan.before)){
    if(!receipt.error)receipt.error=wire::Conflict();return receipt;}
  const auto backup_durable=b.recovery->root()->ObserveIntentSnapshot(*backup);
  if(backup_durable.error || !backup_durable.matched || !backup_durable.durable){
    receipt.error=backup_durable.error?backup_durable.error:wire::Conflict();return receipt;}
  receipt.preserved=b.recovery->root()->path()/h.plan.recovery;
  auto current=b.table_root->CaptureSnapshot("cache-table.v1",4096,receipt.error);
  if(!current || receipt.error || !current->exists()){
    if(!receipt.error)receipt.error=wire::Conflict();return receipt;}
  bool already_published=current->info().file_id==h.plan.stage_file && Equal(current->bytes(),h.plan.intended);
  if(!already_published){
    if(current->info().file_id!=h.plan.before_file || !Equal(current->bytes(),h.plan.before)){
      receipt.error=wire::Conflict();return receipt;}
    // An existing in-process stage already owns the real exclusive file.
    // Otherwise recover exactly the authenticated same inode/bytes.
    if(!h.prepared){
      auto stage=b.table_root->RecoverContentSnapshotStage(h.plan.stage,h.plan.stage_file,h.plan.intended,receipt.error);
      if(!stage || receipt.error)return receipt;
      const auto info=stage->QueryInfo(receipt.error);if(!info || receipt.error)return receipt;
      h.prepared=std::make_unique<PreparedSnapshotPublication>();
      auto& prepared=*h.prepared;prepared.stage_info_=*info;prepared.stage_=std::move(stage);
      prepared.intended_=h.plan.intended;prepared.recovery_=backup;
      prepared.purpose_=PrivateSnapshotPurpose::kCacheTable;
      prepared.receipt_.expected=current;prepared.receipt_.stage=h.plan.stage;
      prepared.receipt_.recovery=h.plan.recovery;prepared.receipt_.stage_created=true;
      prepared.receipt_.staged_count=h.plan.intended.size();prepared.receipt_.recovery_created=true;
      prepared.receipt_.recovery_count=h.plan.before.size();prepared.receipt_.recovery_durable=true;
      prepared.receipt_.phase=SnapshotPublicationPhase::kRecoveryVerify;
    }
  }
  const auto native=ManagedDeviceIdentity::ExecuteHandoff(*h.native);
  receipt.error=native.error;receipt.prepared_durable=h.native->prepared_durable;
  receipt.epoch_published|=native.epoch_published;receipt.epoch_observed|=native.epoch_observed;
  receipt.epoch_durable|=native.epoch_durable;receipt.data_started|=native.data_started;
  receipt.removed_entries+=native.removed_entries;receipt.epoch=native.content_epoch;
  if(receipt.error || !native.complete || !h.native->creator_ready || !h.native->recovery_source || !h.native->projection){
    if(!receipt.error)receipt.error=wire::Conflict();return receipt;}
  // Same exact native-created source as MVA's completed-format factory. This
  // private join does not manufacture a new root/view or accept a raw path.
  b.sources[h.slot]=std::const_pointer_cast<HostStorageRoot>(h.native->recovery_source);
  b.identities[h.slot]=h.native->identity;
  b.projections[h.slot]=h.native->projection;
  if(stop_boundary==1){receipt.error=std::make_error_code(std::errc::operation_canceled);return receipt;}
  if(!already_published){
    const auto published=SnapshotPublisher::Commit(*h.prepared);
    receipt.error=published.error;receipt.table_published|=published.published;
    receipt.table_durable|=published.durable;current=published.current;
    if(receipt.error || !published.published || !published.durable || !current){
      if(!receipt.error)receipt.error=wire::Conflict();return receipt;}
  }else{
    const auto observed=b.table_root->ObserveIntentSnapshot(*current);
    receipt.error=observed.error;receipt.table_published=true;receipt.table_durable=observed.durable;
    if(receipt.error || !observed.matched || !observed.durable){
      if(!receipt.error)receipt.error=wire::Conflict();return receipt;}
  }
  if(current->info().file_id!=h.plan.stage_file || !Equal(current->bytes(),h.plan.intended)){
    receipt.error=wire::Conflict();return receipt;}
  b.table_snapshot=current;b.table=after;
  // The exact canonical token/bytes and durable publication above now own
  // readiness. Release the successfully consumed stage's real exclusive File
  // before the later common table observation; keeping our renamed stage
  // open would truthfully conflict with that observation of cache-table.v1.
  // Every uncertain/error publication returned earlier and retains its owner.
  h.prepared.reset();
  if(stop_boundary==2){receipt.error=std::make_error_code(std::errc::operation_canceled);return receipt;}
  // Same common epoch journal: actual table readiness precedes OwnersReady
  // and Committed, which precede one-use fresh guest admission.
  if(h.native->phase==1)receipt.error=ManagedDeviceIdentity::AdvanceHandoff(*h.native,false);
  if(!receipt.error && h.native->phase==2 && stop_boundary==3){receipt.error=std::make_error_code(std::errc::operation_canceled);return receipt;}
  if(!receipt.error && h.native->phase==2)receipt.error=ManagedDeviceIdentity::AdvanceHandoff(*h.native,true);
  if(receipt.error || h.native->phase!=3){if(!receipt.error)receipt.error=wire::Conflict();return receipt;}
  if(stop_boundary==4){receipt.error=std::make_error_code(std::errc::operation_canceled);return receipt;}
  b.admission_open.store(true,std::memory_order_release);
  ManagedFormatGuestAdmissionPermit admission(*h.native,h.owner);
  receipt.error=b.sources[h.slot]->AdmitFormatProjection(admission);if(receipt.error)return receipt;
  if(stop_boundary==5){receipt.error=std::make_error_code(std::errc::operation_canceled);return receipt;}
  // The disposable child alone is retired on any later partial/stop path.
  // Never retire the actual completed Handoff source by disposing a view.
  auto child=b.sources[h.slot]->Subroot({},false,receipt.error);
  auto guest=child?child->GuestView(receipt.error):nullptr;
  if(!guest || receipt.error){if(!receipt.error)receipt.error=Denied();return receipt;}
  if(!guest->BindCacheCancellation(b.retired,h.fresh_slot_retired)){
    receipt.error=std::make_error_code(std::errc::operation_canceled);return receipt;}
  if(b.retired->load(std::memory_order_acquire)){receipt.error=std::make_error_code(std::errc::operation_canceled);return receipt;}
  b.slot_retired[h.slot]=h.fresh_slot_retired;
  b.guest[h.slot].store(std::move(guest),std::memory_order_release);
  b.error.clear();b.accepting.store(true,std::memory_order_release);receipt.complete=true;receipt.error.clear();
  h.native.reset();h.prepared.reset();h.owns_operation=false;
  b.active_handoff.reset();b.format_active.store(false,std::memory_order_release);return receipt;
}
CacheFormatHandoff::CacheFormatHandoff(std::shared_ptr<State> state):state_(std::move(state)){}
CacheFormatHandoff::~CacheFormatHandoff()=default;
}  // namespace xe::filesystem
