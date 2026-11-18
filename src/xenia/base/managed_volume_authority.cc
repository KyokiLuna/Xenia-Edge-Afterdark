/** Xenia : Xbox 360 Emulator Research Project; BSD license, see LICENSE. */
#include "xenia/base/managed_volume_authority.h"
#include "xenia/base/content_mutation_owner.h"
#include "xenia/base/managed_storage_bootstrap.h"
#include "xenia/base/cache_storage_owner.h"
#include <algorithm>
#include <array>
#include <functional>
#include <map>
#include <random>
#include <string_view>
#include "third_party/mbedtls/include/mbedtls/sha256.h"
#include "xenia/base/mutex.h"
namespace xe::filesystem {
namespace {
std::error_code Denied() {
  return std::make_error_code(std::errc::permission_denied);
}
std::string Key(std::string_view id, const std::filesystem::path& path,
                std::error_code& error) {
  const auto text = std::string(id) + "\n" + path.generic_string();
  std::array<uint8_t, 32> digest{};
  if (mbedtls_sha256(reinterpret_cast<const uint8_t*>(text.data()), text.size(),
                     digest.data(), 0)) {
    error = std::make_error_code(std::errc::io_error);
    return {};
  }
  constexpr char hex[] = "0123456789abcdef";
  std::string key;
  for (auto byte : digest) {
    key += hex[byte >> 4];
    key += hex[byte & 15];
  }
  return key;
}
std::shared_ptr<HostStorageRoot> Child(
    const std::shared_ptr<HostStorageRoot>& parent,
    const std::filesystem::path& name, bool& created, std::error_code& error,
    bool* denied_missing_creation = nullptr) {
  if (denied_missing_creation) {
    *denied_missing_creation = false;
  }
  auto root = parent->Subroot(name, false, error);
  if (!root && error == std::errc::no_such_file_or_directory) {
    error = parent->Create(name, true);
    if (denied_missing_creation &&
        (error == std::errc::permission_denied ||
         error == std::errc::read_only_file_system)) {
      *denied_missing_creation = true;
    }
    created = !error;
    if (!error || error == std::errc::file_exists) {
      root = parent->Subroot(name, false, error);
    }
  }
  return root;
}
std::span<const uint8_t> Bytes(const std::string& text) {
  return {reinterpret_cast<const uint8_t*>(text.data()), text.size()};
}
struct Binding {
  std::string source, content, parent, public_root;
  uint64_t content_directory = 0, parent_directory = 0, private_directory = 0,
           public_directory = 0;
};
void Number(std::vector<uint8_t>& bytes, uint64_t value) {
  for (int shift = 56; shift >= 0; shift -= 8) {
    bytes.push_back(uint8_t(value >> shift));
  }
}
void Text(std::vector<uint8_t>& bytes, std::string_view value) {
  Number(bytes, value.size());
  bytes.insert(bytes.end(), value.begin(), value.end());
}
std::vector<uint8_t> EncodeBinding(const Binding& b, std::error_code& error) {
  std::vector<uint8_t> out{'X', 'M', 'V', 'O', 'L', 'B', '2', '\n'};
  Text(out, b.source);
  Text(out, b.content);
  Number(out, b.content_directory);
  Text(out, b.parent);
  Number(out, b.parent_directory);
  Number(out, b.private_directory);
  Text(out, b.public_root);
  Number(out, b.public_directory);
  std::array<uint8_t, 32> hash{};
  if (mbedtls_sha256(out.data(), out.size(), hash.data(), 0)) {
    error = std::make_error_code(std::errc::io_error);
    return {};
  }
  out.insert(out.end(), hash.begin(), hash.end());
  return out;
}
Binding DecodeBinding(std::span<const uint8_t> bytes, std::error_code& error) {
  Binding b;
  constexpr std::array<uint8_t, 8> magic{'X', 'M', 'V', 'O',
                                         'L', 'B', '2', '\n'};
  if (bytes.size() < 8 + 8 * 8 + 32 || bytes.size() > 16384 ||
      !std::equal(magic.begin(), magic.end(), bytes.begin())) {
    error = std::make_error_code(std::errc::bad_message);
    return b;
  }
  std::array<uint8_t, 32> hash{};
  if (mbedtls_sha256(bytes.data(), bytes.size() - 32, hash.data(), 0) ||
      !std::equal(hash.begin(), hash.end(), bytes.end() - 32)) {
    error = std::make_error_code(std::errc::bad_message);
    return b;
  }
  auto remaining = bytes.subspan(8, bytes.size() - 40);
  auto number = [&]() {
    if (remaining.size() < 8) {
      error = std::make_error_code(std::errc::bad_message);
      return uint64_t(0);
    }
    uint64_t value = 0;
    for (auto byte : remaining.first(8)) {
      value = (value << 8) | byte;
    }
    remaining = remaining.subspan(8);
    return value;
  };
  auto text = [&](size_t maximum) {
    const auto size = number();
    if (error || !size || size > maximum || size > remaining.size()) {
      error = std::make_error_code(std::errc::bad_message);
      return std::string{};
    }
    const auto value = remaining.first(size_t(size));
    remaining = remaining.subspan(size_t(size));
    if (std::find(value.begin(), value.end(), 0) != value.end()) {
      error = std::make_error_code(std::errc::bad_message);
      return std::string{};
    }
    return std::string(value.begin(), value.end());
  };
  b.source = text(256);
  b.content = text(4096);
  b.content_directory = number();
  b.parent = text(4096);
  b.parent_directory = number();
  b.private_directory = number();
  b.public_root = text(4096);
  b.public_directory = number();
  if (!remaining.empty() || !b.content_directory || !b.parent_directory ||
      !b.private_directory || !b.public_directory ||
      !std::filesystem::path(b.parent).is_absolute() ||
      !std::filesystem::path(b.public_root).is_absolute()) {
    error = std::make_error_code(std::errc::bad_message);
  }
  return b;
}
// Source bootstrap controls are bounded declarations in the existing MVA
// binding, not a second device registry or content operation journal.
using BootDigest = std::array<uint8_t, 32>;
using BootNonce = std::array<uint8_t, 16>;
BootDigest BootHash(std::span<const uint8_t> bytes, std::error_code& error) {
  BootDigest out{};
  if (mbedtls_sha256(bytes.data(), bytes.size(), out.data(), 0)) {
    error = std::make_error_code(std::errc::io_error);
  }
  return out;
}
std::error_code BootConflict() {
  return std::make_error_code(std::errc::state_not_recoverable);
}
std::error_code BootPending() {
  return std::make_error_code(std::errc::operation_in_progress);
}
template <size_t N> bool BootNonzero(const std::array<uint8_t, N>& bytes) {
  return std::any_of(bytes.begin(), bytes.end(), [](auto b) { return b != 0; });
}
std::string BootHex(std::span<const uint8_t> bytes) {
  constexpr char hex[] = "0123456789abcdef";
  std::string out;
  for (auto b : bytes) { out += hex[b >> 4]; out += hex[b & 15]; }
  return out;
}
BootNonce BootRandom() {
  BootNonce out{}; std::random_device random;
  for (auto& byte : out) { byte = uint8_t(random()); }
  out[6] = uint8_t((out[6] & 15) | 0x40);
  out[8] = uint8_t((out[8] & 63) | 0x80);
  return out;
}
void BootRaw(std::vector<uint8_t>& out, std::span<const uint8_t> bytes) {
  out.insert(out.end(), bytes.begin(), bytes.end());
}
void BootSeal(std::vector<uint8_t>& out, std::error_code& error) {
  const auto digest = BootHash(out, error); BootRaw(out, digest);
}
struct BootReader {
  std::span<const uint8_t> remaining;
  std::error_code error;
  explicit BootReader(std::span<const uint8_t> input, std::string_view magic) {
    if (input.size() < 40 || input.size() > 65536 || magic.size() != 8 ||
        !std::equal(magic.begin(), magic.end(), input.begin())) {
      error = std::make_error_code(std::errc::bad_message); return;
    }
    const auto digest = BootHash(input.first(input.size() - 32), error);
    if (error || !std::equal(digest.begin(), digest.end(), input.end() - 32)) {
      error = std::make_error_code(std::errc::bad_message); return;
    }
    remaining = input.subspan(8, input.size() - 40);
  }
  uint64_t Number() {
    if (remaining.size() < 8) { error = BootConflict(); return 0; }
    uint64_t n = 0;
    for (auto b : remaining.first(8)) { n = (n << 8) | b; }
    remaining = remaining.subspan(8); return n;
  }
  template <size_t N> void Raw(std::array<uint8_t, N>& out) {
    if (remaining.size() < N) { error = BootConflict(); return; }
    std::copy_n(remaining.begin(), N, out.begin()); remaining = remaining.subspan(N);
  }
  std::string Text(size_t maximum, bool empty = false) {
    const auto n = Number();
    if (error || n > maximum || n > remaining.size() || (!empty && !n)) {
      error = BootConflict(); return {};
    }
    const auto bytes = remaining.first(size_t(n)); remaining = remaining.subspan(size_t(n));
    if (std::find(bytes.begin(), bytes.end(), 0) != bytes.end()) {
      error = BootConflict(); return {};
    }
    return std::string(bytes.begin(), bytes.end());
  }
  bool Done(std::error_code& out) {
    if (!remaining.empty() && !error) { error = BootConflict(); }
    if (error) { out = error; } return !error;
  }
};
struct BootRoot {
  std::string path;
  uint64_t directory = 0;
};
// Root slots: state parent, private parent, public authority, recovery,
// profile epoch owner, mutation root. Pending may declare absent components;
// commit records the actual native identities observed before publication.
struct BootPlan {
  uint64_t mode = 0, creates = 0, content_directory = 0, binding_directory = 0,
           epoch = 0, prior_marker_file = 0;
  uint32_t device = 0;
  std::string source, content;
  BootNonce uuid{}, nonce{};
  BootDigest prefix{};
  std::array<BootRoot, 6> roots{};
};
std::vector<uint8_t> BootEncodePlan(const BootPlan& p, std::error_code& error) {
  std::vector<uint8_t> out{'X','M','V','B','P','0','1','\n'};
  Number(out,p.mode); Number(out,p.creates); Number(out,p.device);
  Text(out,p.source); Text(out,p.content); Number(out,p.content_directory);
  Number(out,p.binding_directory); Number(out,p.epoch); Number(out,p.prior_marker_file);
  BootRaw(out,p.uuid); BootRaw(out,p.nonce); BootRaw(out,p.prefix);
  for (const auto& root : p.roots) { Text(out,root.path); Number(out,root.directory); }
  BootSeal(out,error); return out;
}
BootPlan BootDecodePlan(std::span<const uint8_t> bytes, std::error_code& error) {
  BootPlan p; BootReader in(bytes,"XMVBP01\n");
  p.mode=in.Number(); p.creates=in.Number(); const auto device=in.Number();
  p.device=uint32_t(device); p.source=in.Text(256); p.content=in.Text(4096);
  p.content_directory=in.Number(); p.binding_directory=in.Number();
  p.epoch=in.Number(); p.prior_marker_file=in.Number();
  in.Raw(p.uuid); in.Raw(p.nonce); in.Raw(p.prefix);
  for (auto& root:p.roots) { root.path=in.Text(4096); root.directory=in.Number(); }
  if (!in.Done(error)) { return {}; }
  if (p.mode < 1 || p.mode > 4 || p.creates > 31 || !device || device > UINT32_MAX ||
      !p.content_directory || !p.binding_directory || !p.epoch || !p.roots[0].directory ||
      !BootNonzero(p.uuid) || !BootNonzero(p.nonce) || !BootNonzero(p.prefix) ||
      !std::filesystem::path(p.content).is_absolute()) { error=BootConflict(); }
  for (const auto& root:p.roots) {
    if (!std::filesystem::path(root.path).is_absolute()) { error=BootConflict(); }
  }
  return p;
}
struct BootCommit {
  BootDigest pending{}, owner{};
  uint64_t marker_file = 0;
  std::array<uint64_t,6> directories{};
};
std::vector<uint8_t> BootEncodeCommit(const BootCommit& p, std::error_code& error) {
  std::vector<uint8_t> out{'X','M','V','B','C','0','1','\n'};
  BootRaw(out,p.pending); BootRaw(out,p.owner); Number(out,p.marker_file);
  for (auto directory:p.directories) { Number(out,directory); }
  BootSeal(out,error); return out;
}
BootCommit BootDecodeCommit(std::span<const uint8_t> bytes,std::error_code& error) {
  BootCommit p; BootReader in(bytes,"XMVBC01\n");
  in.Raw(p.pending); in.Raw(p.owner); p.marker_file=in.Number();
  for (auto& directory:p.directories) { directory=in.Number(); }
  in.Done(error);
  if (!BootNonzero(p.pending) || !BootNonzero(p.owner) || !p.marker_file ||
      std::any_of(p.directories.begin(),p.directories.end(),[](auto n){return !n;})) {
    error=BootConflict();
  }
  return p;
}
// Each component has a durable declaration followed by a native-ID/complete
// marker proof. Both are needed: a pathname or a checksum alone never adopts
// an unrecorded directory left between mkdir and proof publication.
struct BootComponent {
  uint64_t slot=0, phase=0, parent=0, directory=0;
  BootDigest pending{}, payload{};
  std::string target, staging;
  BootNonce nonce{};
};
std::vector<uint8_t> BootEncodeComponent(const BootComponent& p,std::error_code& error) {
  std::vector<uint8_t> out{'X','M','V','B','D','0','1','\n'};
  Number(out,p.slot);Number(out,p.phase);Number(out,p.parent);Number(out,p.directory);
  BootRaw(out,p.pending);BootRaw(out,p.payload);Text(out,p.target);Text(out,p.staging);
  BootRaw(out,p.nonce);BootSeal(out,error);return out;
}
BootComponent BootDecodeComponent(std::span<const uint8_t> bytes,std::error_code& error) {
  BootComponent p;BootReader in(bytes,"XMVBD01\n");
  p.slot=in.Number();p.phase=in.Number();p.parent=in.Number();p.directory=in.Number();
  in.Raw(p.pending);in.Raw(p.payload);p.target=in.Text(256);p.staging=in.Text(256);in.Raw(p.nonce);
  in.Done(error);
  if (p.slot>15 || p.phase>1 || !p.parent || !BootNonzero(p.pending) ||
      !BootNonzero(p.nonce) || (p.phase && (!p.directory || !BootNonzero(p.payload))) ||
      (!p.phase && (p.directory || BootNonzero(p.payload))) ||
      std::filesystem::path(p.target).has_parent_path() ||
      std::filesystem::path(p.staging).has_parent_path()) { error=BootConflict(); }
  return p;
}
std::string BootComponentLeaf(uint64_t slot) {
  constexpr char hex[]="0123456789ABCDEF";
  return std::string("bootstrap.component-")+hex[(slot>>4)&15]+hex[slot&15]+".v1";
}

}  // namespace
struct ManagedStorageBootstrap::Impl {
  std::shared_ptr<HostStorageRoot> source, binding;
  std::shared_ptr<const HostStorageIntentLease> source_lease;
  std::shared_ptr<const ManagedDeviceIdentity> identity;
  std::vector<std::unique_ptr<HostStorageBootstrapDirectory>> capsules;
  std::vector<std::shared_ptr<HostStorageRoot>> preserved_roots;
  std::vector<std::shared_ptr<const HostStorageGuestProjection>> validation_projections;
  std::vector<uint8_t> pending_wire, prefix_wire;
  BootPlan plan;
  BootDigest pending_digest{};
  bool committed=false;
  unsigned stop=0, boundary=0;
  std::error_code error;

  bool Boundary() {
    ++boundary;
    if (stop && boundary==stop) {
      error=std::make_error_code(std::errc::operation_canceled);return false;
    }
    return true;
  }
  static std::shared_ptr<HostStorageRoot> BindingRoot(
      const HostStorageRoot& source,std::string_view id,std::error_code& error) {
    const auto key=Key(id,source.path(),error);if (error) { return {}; }
    return source.Subroot(".xenia-device-binding-v1-"+key,false,error);
  }
  static std::vector<uint8_t> Read(const HostStorageRoot& root,
      const std::filesystem::path& leaf,bool required,std::error_code& error) {
    auto token=root.CaptureSnapshot(leaf,65536,error);
    if (!token) { return {}; }
    if (!token->exists()) {
      if (required) { error=BootConflict(); }return {};
    }
    if (token->bytes().empty()) { error=BootConflict();return {}; }
    return {token->bytes().begin(),token->bytes().end()};
  }
  // The gate validates a bounded typed binding, not every private journal.
  // Each normal owner subsequently proves its exact committed private parent;
  // MVA reopens all owners noncreating before publishing the device.
  static bool ReadBinding(const HostStorageRoot& source,std::string_view id,
      uint32_t device,BootPlan& plan,BootCommit& commit,
      std::shared_ptr<HostStorageRoot>& binding,std::vector<uint8_t>& pending,
      bool& committed,std::error_code& error) {
    error.clear();committed=false;
    binding=BindingRoot(source,id,error);
    if (!binding && error==std::errc::no_such_file_or_directory) {
      error.clear();return false;
    }
    if (!binding) { return true; }
    if (ValidateBindingContainer(binding)) { error=BootConflict();return true; }
    pending=Read(*binding,"bootstrap.pending.v1",false,error);
    if (error || pending.empty()) { return true; } // old complete B2 is legacy
    plan=BootDecodePlan(pending,error);if (error) { return true; }
    const auto source_id=source.IntentRootIdentity(error);
    const auto binding_id=binding->IntentRootIdentity(error);
    if (error || plan.source!=id || plan.device!=device ||
        plan.content!=source.path().generic_string() ||
        plan.content_directory!=source_id.directory ||
        plan.binding_directory!=binding_id.directory) {
      error=BootConflict();return true;
    }
    const auto prefix=Read(*binding,"bootstrap-source.v1",true,error);
    if (error || BootHash(prefix,error)!=plan.prefix) {
      if (!error) { error=BootConflict(); }return true;
    }
    auto original=ManagedDeviceIdentity::DescribeBootstrapWire(prefix,error);
    if (error || original.binding || original.uuid!=plan.uuid ||
        original.source!=plan.source || original.device!=plan.device ||
        original.location!=plan.content || original.directory!=plan.content_directory ||
        original.epoch!=plan.epoch || original.format_pending) {
      if (!error) { error=BootConflict(); }return true;
    }
    const auto complete=Read(*binding,"bootstrap.commit.v1",false,error);
    if (error || complete.empty()) { return true; }
    commit=BootDecodeCommit(complete,error);
    const auto owner=Read(*binding,"owner.v1",true,error);
    if (error || commit.pending!=BootHash(pending,error) ||
        commit.owner!=BootHash(owner,error)) {
      if (!error) { error=BootConflict(); }return true;
    }
    const auto b2=DecodeBinding(owner,error);
    if (error || b2.source!=plan.source || b2.content!=plan.content ||
        b2.content_directory!=plan.content_directory ||
        b2.parent!=plan.roots[0].path || b2.public_root!=plan.roots[2].path ||
        b2.parent_directory!=commit.directories[0] ||
        b2.private_directory!=commit.directories[1] ||
        b2.public_directory!=commit.directories[2]) {
      if (!error) { error=BootConflict(); }return true;
    }
    committed=true;return true;
  }
};
ManagedStorageBootstrap::ManagedStorageBootstrap(std::unique_ptr<Impl> impl)
    :impl_(std::move(impl)) {}
ManagedStorageBootstrap::~ManagedStorageBootstrap()=default;
unsigned ManagedStorageBootstrap::Boundaries(const ManagedVolumeAuthority& volume) {
  return volume.bootstrap_ && volume.bootstrap_->impl_?volume.bootstrap_->impl_->boundary:0;
}

std::error_code ManagedStorageBootstrap::ValidateBindingContainer(
    const std::shared_ptr<HostStorageRoot>& binding) {
  if (!binding) { return BootConflict(); }
  auto error=binding->PrivateDirectory();if (error) { return error; }
  const auto leaves=binding->ListBootstrapRecords(error);
  // Inventory accepts only the bounded native control grammar. A partial
  // record remains opaque evidence; it is never parsed as a committed record.
  static_cast<void>(leaves);return error;
}
std::error_code ManagedStorageBootstrap::BeforeNormalOpen(
    const HostStorageRoot& source,std::string_view id,uint32_t device) {
  if(CacheStorageOwner::IsCacheSource(id))return CacheStorageOwner::BeforeNormalOpen(source,id,device);
  BootPlan plan;BootCommit commit;std::shared_ptr<HostStorageRoot> binding;
  std::vector<uint8_t> pending;bool committed=false;std::error_code error;
  Impl::ReadBinding(source,id,device,plan,commit,binding,pending,committed,error);
  if (error) { return error; }
  if (!pending.empty() && !committed) { return BootPending(); }
  return {};
}
std::error_code ManagedStorageBootstrap::CheckNormal(
    const HostStorageRoot& source,const ManagedBootstrapSourceDescription& actual) {
  if(CacheStorageOwner::IsCacheSource(actual.source))return CacheStorageOwner::CheckNormal(source,actual);
  BootPlan plan;BootCommit commit;std::shared_ptr<HostStorageRoot> binding;
  std::vector<uint8_t> pending;bool committed=false;std::error_code error;
  const bool exists=Impl::ReadBinding(source,actual.source,actual.device,
      plan,commit,binding,pending,committed,error);
  if (error) { return error; }
  if (!actual.binding) {
    if (!pending.empty()) { return committed?BootConflict():BootPending(); }
    return {};
  }
  if (!exists || pending.empty()) { return BootConflict(); }
  const auto& tag=*actual.binding;
  if (tag.epoch!=plan.epoch || tag.binding_directory!=plan.binding_directory ||
      tag.nonce!=plan.nonce || tag.pending_digest!=BootHash(pending,error) ||
      tag.prefix_digest!=plan.prefix || actual.prefix_digest!=plan.prefix ||
      actual.uuid!=plan.uuid || actual.epoch<plan.epoch ||
      actual.directory!=plan.content_directory || actual.location!=plan.content) {
    return error?error:BootConflict();
  }
  if (!committed) { return BootPending(); }
  if (actual.marker_file && actual.marker_file!=commit.marker_file) {
    return BootConflict();
  }
  // Re-observe canonical complete controls through the same native barrier
  // implementation. No file/path replacement is made here.
  const auto complete=Impl::Read(*binding,"bootstrap.commit.v1",true,error);
  if (error) { return error; }
  const auto receipt=binding->PublishBootstrapRecord(
      "bootstrap.commit.v1",complete,BootHex(plan.nonce));
  return receipt.error ? receipt.error : (!receipt.durable?BootConflict():std::error_code{});
}
std::error_code ManagedStorageBootstrap::ValidateOwnerParent(
    const ManagedDeviceIdentity& identity,const HostStorageRoot& parent) {
  if (!identity.binding_required()) { return {}; }
  BootPlan plan;BootCommit commit;std::shared_ptr<HostStorageRoot> binding;
  std::vector<uint8_t> pending;bool committed=false;std::error_code error;
  Impl::ReadBinding(*identity.source(),identity.source_id(),identity.device_id(),
      plan,commit,binding,pending,committed,error);
  if (error || !committed || pending.empty()) { return error?error:BootPending(); }
  const auto actual=parent.IntentRootIdentity(error);
  if (!error && (parent.path().generic_string()!=plan.roots[1].path ||
                actual.directory!=commit.directories[1])) { error=BootConflict(); }
  return error;
}
bool ManagedStorageBootstrap::AllowsPending(const HostStorageRoot& source,
    const ManagedBootstrapSourceDescription& actual) const {
  if (!impl_ || impl_->error || impl_->committed || !impl_->source ||
      !impl_->source_lease || !source.IntentLeaseMatches(*impl_->source_lease) ||
      source.path()!=impl_->source->path()) { return false; }
  const auto& p=impl_->plan;
  if (actual.uuid!=p.uuid || actual.epoch!=p.epoch || actual.source!=p.source ||
      actual.device!=p.device || actual.location!=p.content ||
      actual.directory!=p.content_directory || actual.format_pending) { return false; }
  if (!actual.binding) { return actual.prefix_digest==p.prefix; }
  return actual.binding->epoch==p.epoch &&
      actual.binding->binding_directory==p.binding_directory &&
      actual.binding->nonce==p.nonce &&
      actual.binding->prefix_digest==p.prefix &&
      actual.binding->pending_digest==impl_->pending_digest &&
      actual.prefix_digest==p.prefix;
}
std::error_code ManagedStorageBootstrap::CheckPending(
    const ManagedDeviceIdentity& identity) const {
  if (!impl_ || impl_->identity.get()!=&identity || !impl_->source_lease ||
      !identity.source()->IntentLeaseMatches(*impl_->source_lease)) { return Denied(); }
  std::error_code error;const auto bytes=identity.BootstrapWire(error);
  auto actual=ManagedDeviceIdentity::DescribeBootstrapWire(bytes,error);
  return error?error:(AllowsPending(*identity.source(),actual)?std::error_code{}:Denied());
}
ManagedBootstrapCreationPermit::ManagedBootstrapCreationPermit(Purpose purpose,
    std::shared_ptr<const ManagedDeviceIdentity> identity,
    std::shared_ptr<const HostStorageRoot> parent,
    std::shared_ptr<const HostStorageIntentLease> lease,
    std::weak_ptr<ManagedStorageBootstrap> bootstrap,
    std::shared_ptr<HostStorageRoot> component)
    :purpose_(purpose),identity_(std::move(identity)),parent_(std::move(parent)),
     source_lease_(std::move(lease)),bootstrap_(std::move(bootstrap)),
     component_(std::move(component)) {}
std::error_code ManagedBootstrapCreationPermit::Consume(Purpose purpose,
    const ManagedDeviceIdentity& identity,const HostStorageRoot& parent,
    const HostStorageIntentLease& lease) {
  if (purpose!=purpose_ || &identity!=identity_.get() || !parent_ ||
      &lease!=source_lease_.get()) { return Denied(); }
  if (auto error=ManagedStorageBootstrap::CheckPermitBindings(
          identity,*parent_,parent,lease)) { return error; }
  bool absent=false;
  if (!consumed_.compare_exchange_strong(absent,true,std::memory_order_acq_rel)) { return Denied(); }
  return Check(identity);
}
std::error_code ManagedBootstrapCreationPermit::Check(const ManagedDeviceIdentity& identity) const {
  if (&identity!=identity_.get() || !consumed_.load(std::memory_order_acquire) ||
      !source_lease_ || !parent_ ||
      ManagedStorageBootstrap::CheckPermitBindings(identity,*parent_,*parent_,*source_lease_)) { return Denied(); }
  if (purpose_==Purpose::kFormatProfile) { return identity.CheckCurrent(); }
  auto owner=bootstrap_.lock();return owner?owner->CheckPending(identity):Denied();
}
std::shared_ptr<HostStorageRoot> ManagedBootstrapCreationPermit::ComponentRoot(
    const std::shared_ptr<HostStorageRoot>& named,std::error_code& error) const {
  error=Check(*identity_);if (error || !named) {if (!error) {error=Denied();}return {};}
  if (!component_) {return named;}
  const auto& expected=component_->directory_identity();
  const auto& actual=named->directory_identity();
  if (!expected || !actual || component_->path()!=named->path() ||
      !expected->SameEntry({},*actual,{})) {error=Denied();return {};}
  error=ManagedStorageBootstrap::CheckComponentCurrent(*component_);
  return error?nullptr:component_;
}
ManagedBootstrapGuestAdmissionPermit::ManagedBootstrapGuestAdmissionPermit(
    std::weak_ptr<ManagedStorageBootstrap> bootstrap,std::shared_ptr<const HostStorageRoot> source,
    std::shared_ptr<const HostStorageIntentLease> lease,std::shared_ptr<const ManagedDeviceIdentity> identity,
    std::shared_ptr<const HostStorageGuestProjection> projection,std::shared_ptr<const ContentMutationOwner> mutation)
    :bootstrap_(std::move(bootstrap)),source_(std::move(source)),lease_(std::move(lease)),
     identity_(std::move(identity)),projection_(std::move(projection)),mutation_(std::move(mutation)) {}
ManagedBootstrapGuestAdmissionPermit::ManagedBootstrapGuestAdmissionPermit(
    std::shared_ptr<CacheStorageOwner> cache,std::shared_ptr<const HostStorageRoot> source,
    std::shared_ptr<const HostStorageIntentLease> lease,std::shared_ptr<const ManagedDeviceIdentity> identity,
    std::shared_ptr<const HostStorageGuestProjection> projection)
    :source_(std::move(source)),lease_(std::move(lease)),identity_(std::move(identity)),
     projection_(std::move(projection)),cache_(std::move(cache)) {}
bool ManagedBootstrapGuestAdmissionPermit::FinalCurrent() const noexcept {
  return consumed_.load(std::memory_order_acquire) && identity_ && !identity_->retiring() &&
      (cache_?cache_->AdmissionCurrent():(mutation_ && mutation_->ready()));
}
std::error_code ManagedBootstrapGuestAdmissionPermit::Consume(
    const HostStorageRoot& source,const HostStorageGuestProjection& projection) {
  if (&source!=source_.get() || &projection!=projection_.get() ||
      consumed_.exchange(true,std::memory_order_acq_rel)) {return Denied();}
  if(cache_)return cache_->CheckGuestAdmission(*this);
  auto bootstrap=bootstrap_.lock();return bootstrap?bootstrap->CheckGuestAdmission(*this):Denied();
}
std::error_code ManagedStorageBootstrap::CheckGuestAdmission(
    const ManagedBootstrapGuestAdmissionPermit& permit) const {
  if (!impl_ || impl_->error || !impl_->committed || !permit.source_ || !permit.lease_ ||
      !permit.identity_ || !permit.mutation_ || !permit.projection_ ||
      permit.source_.get()!=impl_->source.get() || permit.lease_!=impl_->source_lease ||
      permit.identity_!=impl_->identity || permit.source_->Projection()!=permit.projection_ ||
      !permit.source_->IntentLeaseMatches(*permit.lease_) ||
      permit.mutation_->identity_!=permit.identity_ ||
      permit.mutation_->content_.get()!=permit.source_.get() ||
      permit.mutation_->source_lease_!=permit.lease_ || !permit.mutation_->ready()) {return Denied();}
  // Actual observing currentness (identity, recovery and native source lease)
  // runs outside HostStorageProtection/space locks before native release.
  return permit.mutation_->CheckCurrent();
}
std::error_code ManagedStorageBootstrap::CheckComponentCurrent(
    const HostStorageRoot& component) {
  return component.Validate();
}
std::error_code ManagedStorageBootstrap::CheckPermitBindings(
    const ManagedDeviceIdentity& identity,const HostStorageRoot& expected,
    const HostStorageRoot& actual,const HostStorageIntentLease& lease) {
  if (expected.path()!=actual.path() || !identity.source()->IntentLeaseMatches(lease)) { return Denied(); }
  std::error_code error;
  const auto expected_id=expected.IntentRootIdentity(error);if (error) { return error; }
  const auto actual_id=actual.IntentRootIdentity(error);
  return error?error:(expected_id==actual_id?std::error_code{}:Denied());
}
std::unique_ptr<ManagedBootstrapCreationPermit> ManagedStorageBootstrap::Permit(
    ManagedBootstrapCreationPermit::Purpose purpose,
    const std::shared_ptr<const HostStorageRoot>& parent) {
  if (!impl_ || impl_->error || impl_->committed || !parent || !impl_->identity ||
      CheckPending(*impl_->identity)) { return {}; }
  const auto leaf=purpose==ManagedBootstrapCreationPermit::Purpose::kProfile
      ? std::string(ProfileIntentOwner::kDirectoryPrefix)+BootHex(impl_->plan.uuid)+"-"+std::to_string(impl_->plan.epoch)
      : ".xenia-content-mutations-v1-"+BootHex(impl_->plan.uuid);
  std::shared_ptr<HostStorageRoot> component;
  for (const auto& capsule:impl_->capsules) {
    if (capsule->root()->path()==parent->path()/leaf) {component=capsule->root();break;}
  }
  return std::unique_ptr<ManagedBootstrapCreationPermit>(new ManagedBootstrapCreationPermit(
      purpose,impl_->identity,parent,impl_->source_lease,weak_from_this(),std::move(component)));
}

ManagedFormatGuestAdmissionPermit::ManagedFormatGuestAdmissionPermit(
    const ManagedDeviceIdentity::Handoff& handoff,
    std::shared_ptr<const ContentMutationOwner> mutation)
    : handoff_(handoff), identity_(handoff.identity), source_(handoff.recovery_source),
      observation_source_(handoff.observation_source), lease_(handoff.source_lease),
      projection_(handoff.projection), observation_projection_(handoff.observation_projection),
      mutation_(std::move(mutation)) {}
ManagedFormatGuestAdmissionPermit::ManagedFormatGuestAdmissionPermit(
    const ManagedDeviceIdentity::Handoff& handoff,std::shared_ptr<CacheStorageOwner> cache)
    :handoff_(handoff),identity_(handoff.identity),source_(handoff.recovery_source),
     observation_source_(handoff.observation_source),lease_(handoff.source_lease),
     projection_(handoff.projection),observation_projection_(handoff.observation_projection),
     cache_(std::move(cache)) {}
std::error_code ManagedFormatGuestAdmissionPermit::Consume(
    const HostStorageRoot& source, const HostStorageGuestProjection& projection) {
  if (&source!=source_.get() || &projection!=projection_.get() ||
      consumed_.exchange(true,std::memory_order_acq_rel)) {return Denied();}
  return cache_?CacheStorageOwner::CheckFormatGuestAdmission(*this):
      ManagedVolumeAuthority::CheckFormatGuestAdmission(*this);
}
bool ManagedFormatGuestAdmissionPermit::FinalCurrent() const {
  return consumed_.load(std::memory_order_acquire) && identity_ &&
      !identity_->retiring() && (cache_?cache_->AdmissionCurrent():(mutation_ && mutation_->ready()));
}
std::error_code ManagedVolumeAuthority::CheckFormatGuestAdmission(
    const ManagedFormatGuestAdmissionPermit& permit) {
  const auto& h=permit.handoff_;
  if (h.identity!=permit.identity_ || h.recovery_source!=permit.source_ ||
      h.source_lease!=permit.lease_ || h.projection!=permit.projection_ ||
      h.observation_source!=permit.observation_source_ ||
      h.observation_projection!=permit.observation_projection_ ||
      !permit.identity_ || !permit.mutation_ || !permit.mutation_->ready() ||
      permit.mutation_->identity_!=permit.identity_ ||
      permit.mutation_->content_!=permit.source_ ||
      permit.mutation_->source_lease_!=permit.lease_) {return Denied();}
  auto error=ManagedDeviceIdentity::CheckCompletedHandoff(h);
  return error?error:permit.mutation_->CheckCurrent();
}

bool ManagedStorageBootstrap::Open(ManagedVolumeAuthority& out,
    const ManagedVolumeAuthority::Options& options,
    const std::shared_ptr<HostStorageRoot>& anchor,bool inspect,unsigned stop) {
  auto bootstrap=std::shared_ptr<ManagedStorageBootstrap>(new ManagedStorageBootstrap(std::make_unique<Impl>()));
  out.bootstrap_=bootstrap;
  auto& b=*bootstrap->impl_;auto& error=b.error;
  b.source=out.content_;b.source_lease=out.source_lease_;b.stop=stop;
  auto fail=[&]() { out.error_=error?error:BootConflict();return true; };
  const auto live=b.source->IntentRootIdentity(error);if (error) { return fail(); }
  const auto key=Key(options.source_id,b.source->path(),error);if (error) { return fail(); }
  const auto inventory=anchor->ManagedVolumeEntries(key,error);if (error) { return fail(); }
  BootCommit commit;bool committed=false;
  auto binding=Impl::BindingRoot(*b.source,options.source_id,error);
  const bool has_binding=bool(binding);
  if (!binding && error!=std::errc::no_such_file_or_directory) { return fail(); }
  error.clear();
  if ((inventory && !(inventory&1)) || (has_binding && !(inventory&1))) {
    error=BootConflict();return fail();
  }
  if (has_binding) {
    if (auto e=ValidateBindingContainer(binding)) { error=e;return fail(); }
    b.pending_wire=Impl::Read(*binding,"bootstrap.pending.v1",false,error);
    if (error) { return fail(); }
    if (!b.pending_wire.empty()) {
      Impl::ReadBinding(*b.source,options.source_id,options.device_id,b.plan,
          commit,binding,b.pending_wire,committed,error);
      if (error) { return fail(); }
      b.binding=binding;b.pending_digest=BootHash(b.pending_wire,error);
      b.prefix_wire=Impl::Read(*binding,"bootstrap-source.v1",true,error);
      if (error) { return fail(); }
    }
  }
  // Capture physical bootstrap parents while the real source EX is current.
  // Unlike guest/operation views these native roots must survive a deliberate
  // source-generation retirement. Inspection borrows EX only for observation;
  // every description is dropped before InspectHandoff takes its own drain.
  if (inspect) {
    if (b.pending_wire.empty()) { out.bootstrap_.reset();return false; }
    if (!committed) { error=BootPending();return fail(); }
    b.source_lease=b.source->AcquireIntentLease(error,true);
    if (!b.source_lease) {return fail();}
  }
  auto source_anchor=b.source->RetainBootstrapNamespace(*b.source_lease,error);
  if (!source_anchor) {return fail();}
  auto anchor_lease=source_anchor->RebindFormatLease(*b.source_lease,error);
  if (!anchor_lease) {return fail();}
  if (binding) {
    auto retained=Impl::BindingRoot(*source_anchor,options.source_id,error);
    const auto& before=binding->directory_identity();
    const auto& after=retained?retained->directory_identity():nullptr;
    if (!retained || !before || !after || binding->path()!=retained->path() ||
        !before->SameEntry({},*after,{})) {
      if (!error) {error=BootConflict();}return fail();
    }
    binding=std::move(retained);b.binding=binding;
  }
  auto same_fs=[&](const std::shared_ptr<HostStorageRoot>& root) {
    if (!root || root->read_only() || root->is_guest_private()) { error=Denied();return false; }
    const auto id=root->IntentRootIdentity(error);
    if (!error && id.filesystem!=live.filesystem) { error=std::make_error_code(std::errc::cross_device_link); }
    return !error;
  };
  auto open_root=[&](const std::string& path,uint64_t directory) {
    auto root=HostStorageRoot::Open(path,false,error,0);
    if (!same_fs(root)) { return std::shared_ptr<HostStorageRoot>{}; }
    const auto id=root->IntentRootIdentity(error);
    if (!error && (root->path().generic_string()!=path || (directory && id.directory!=directory))) { error=BootConflict(); }
    return error?std::shared_ptr<HostStorageRoot>{}:root;
  };
  // Source-identical write parents use the independently pinned physical
  // anchor only after complete current native identity/pin proof. Their lease
  // is rebound from the SAME source EX description, never reacquired by name.
  // Public namespace roots remain independent and never pass through here.
  auto selected_parent=[&](std::shared_ptr<HostStorageRoot> selected) {
    if (!same_fs(selected)) {return std::shared_ptr<HostStorageRoot>{};}
    if (selected->path()!=b.source->path()) {return selected;}
    const auto selected_id=selected->IntentRootIdentity(error);
    const auto source_id=error?HostStorageRootIdentity{}:b.source->IntentRootIdentity(error);
    const auto& selected_pin=selected->directory_identity();
    const auto& source_pin=b.source->directory_identity();
    if (error || selected_id!=live || source_id!=live || !selected_pin || !source_pin ||
        !selected_pin->SameEntry({},*source_pin,{}) ||
        (b.source_lease && !b.source->IntentLeaseMatches(*b.source_lease))) {
      if (!error) {error=BootConflict();}return std::shared_ptr<HostStorageRoot>{};
    }
    return source_anchor;
  };
  std::shared_ptr<HostStorageRoot> state_parent,private_parent,public_root;
  std::shared_ptr<const SnapshotRecoveryOwner> recovery;
  if (!b.pending_wire.empty()) {
    // An existing declaration owns selection after restart. Options may agree
    // or omit an override, but cannot silently choose a different parent.
    state_parent=open_root(b.plan.roots[0].path,b.plan.roots[0].directory);
    if (state_parent) {state_parent=selected_parent(std::move(state_parent));}
    if (!state_parent) { return fail(); }
    if (options.state_parent_override &&
        options.state_parent_override->path()!=state_parent->path()) {
      error=BootConflict();return fail();
    }
    if (committed) {
      private_parent=open_root(b.plan.roots[1].path,commit.directories[1]);
      if (!private_parent) { return fail(); }
      public_root=open_root(b.plan.roots[2].path,commit.directories[2]);
      if (!public_root) { return fail(); }
    } else {
      if (b.plan.roots[1].directory) {
        private_parent=open_root(b.plan.roots[1].path,b.plan.roots[1].directory);
        if (!private_parent) { return fail(); }
      }
      if (b.plan.roots[2].directory) {
        public_root=open_root(b.plan.roots[2].path,b.plan.roots[2].directory);
        if (!public_root) { return fail(); }
      }
    }
  } else {
    if (inspect) {
      // Old complete bindings keep their existing explicit format inspection
      // lane. No bootstrap/migration side effect is authorized by inspection.
      out.bootstrap_.reset();return false;
    }
    b.plan.source=options.source_id;b.plan.device=options.device_id;
    b.plan.content=b.source->path().generic_string();b.plan.content_directory=live.directory;
    b.plan.nonce=BootRandom();
    // Acquire and retain the original canonical FD/flock before any pending
    // declaration. An escaped old identity/owner makes migration busy here.
    b.identity=ManagedDeviceIdentity::OpenInternal(b.source,options.source_id,
        options.device_id,error,false,false,b.source_lease);
    const bool existing_marker=bool(b.identity);
    if (!existing_marker && error!=std::errc::no_such_file_or_directory) { return fail(); }
    error.clear();
    if (existing_marker) {
      b.prefix_wire=b.identity->BootstrapWire(error);
      const auto original=b.identity->DescribeBootstrapCurrent(error);
      if (error || original.binding || original.format_pending) {
        if (!error) { error=BootConflict(); }return fail();
      }
      b.plan.prior_marker_file=original.marker_file;
    } else {
      if (has_binding || inventory) { error=BootConflict();return fail(); }
      b.prefix_wire=ManagedDeviceIdentity::PrepareBootstrapPrefix(
          *b.source,options.source_id,options.device_id,error);
    }
    auto prefix=ManagedDeviceIdentity::DescribeBootstrapWire(b.prefix_wire,error);
    if (error) { return fail(); }
    b.plan.uuid=prefix.uuid;b.plan.epoch=prefix.epoch;b.plan.prefix=BootHash(b.prefix_wire,error);
    if (error) { return fail(); }
    if (has_binding) {
      // Explicit migration of an already complete B2 producer. All prior
      // components must exist; a missing owner is never freshness evidence.
      const auto owner=Impl::Read(*binding,"owner.v1",true,error);
      const auto old=DecodeBinding(owner,error);
      if (error || !existing_marker || old.source!=options.source_id ||
          old.content!=b.plan.content || old.content_directory!=live.directory) {
        if (!error) { error=BootConflict(); }return fail();
      }
      state_parent=open_root(old.parent,old.parent_directory);
      if (!state_parent) { return fail(); }
      private_parent=state_parent->Subroot(".xenia-device-state-v1-"+key,false,error);
      if (!private_parent || private_parent->IntentRootIdentity(error).directory!=old.private_directory) {
        if (!error) { error=BootConflict(); }return fail();
      }
      public_root=open_root(old.public_root,old.public_directory);
      if (!public_root) { return fail(); }
      b.plan.mode=4;b.plan.creates=0;
    } else if (options.preserve_legacy_layout || existing_marker) {
      if (existing_marker && !options.preserve_legacy_layout && !options.existing_legacy) {
        error=Denied();return fail();
      }
      private_parent=options.preserve_legacy_layout?options.legacy_private_parent:options.existing_legacy->private_parent;
      public_root=options.preserve_legacy_layout?options.legacy_public_root:options.existing_legacy->public_root;
      recovery=options.preserve_legacy_layout?options.legacy_recovery:options.existing_legacy->recovery;
      if (!same_fs(private_parent) || !same_fs(public_root) || !recovery ||
          recovery->error() || recovery->CheckTarget(*b.source)) {
        if (!error) { error=Denied(); }return fail();
      }
      state_parent=private_parent;b.plan.mode=existing_marker?3:2;
      b.plan.creates=existing_marker?16:24; // profile=8, mutation=16
      auto shell=public_root->Subroot("Content",false,error);
      if (!shell) {
        if (existing_marker || error!=std::errc::no_such_file_or_directory) {return fail();}
        error.clear();b.plan.creates|=2; // ordinary child of explicit public folder
      } else {
        error=shell->CheckBootstrapBrowseDirectory(*b.source);if (error) {return fail();}
      }
    } else {
      b.plan.mode=1;b.plan.creates=31;
      // A default folder-backed source keeps its managed siblings within the
      // already authorized writable source. Explicit same-FS parent selection
      // remains honored; no probe creates a private directory at filesystem /.
      state_parent=options.state_parent_override?options.state_parent_override:anchor;
      if (options.legacy_public_root) {
        public_root=options.legacy_public_root;if (!same_fs(public_root)) {return fail();}
        b.plan.creates&=~uint64_t(2);
      }
    }
    state_parent=selected_parent(std::move(state_parent));
    if (!state_parent) {return fail();}
    if (private_parent) {
      private_parent=selected_parent(std::move(private_parent));
      if (!private_parent) {return fail();}
    }
    if (options.state_parent_override && options.state_parent_override->path()!=state_parent->path()) {
      error=BootConflict();return fail();
    }
    b.plan.roots[0]={state_parent->path().generic_string(),state_parent->IntentRootIdentity(error).directory};
    b.plan.roots[1]={private_parent?private_parent->path().generic_string():
        (state_parent->path()/(".xenia-device-state-v1-"+key)).generic_string(),
        private_parent?private_parent->IntentRootIdentity(error).directory:0};
    b.plan.roots[2]={public_root?public_root->path().generic_string():
        (state_parent->path()/(".xenia-device-volume-v1-"+key)).generic_string(),
        public_root?public_root->IntentRootIdentity(error).directory:0};
    const auto private_path=std::filesystem::path(b.plan.roots[1].path);
    b.plan.roots[3]={(private_path/SnapshotRecoveryOwner::kDirectory).generic_string(),0};
    if (recovery) {b.plan.roots[3].directory=recovery->root()->IntentRootIdentity(error).directory;}
    b.plan.roots[4]={(private_path/(std::string(ProfileIntentOwner::kDirectoryPrefix)+
        BootHex(b.plan.uuid)+"-"+std::to_string(b.plan.epoch))).generic_string(),0};
    b.plan.roots[5]={(private_path/(".xenia-content-mutations-v1-"+BootHex(b.plan.uuid))).generic_string(),0};
    if (error) { return fail(); }
    if (existing_marker) {
      // Validate exact old producer authorities before publishing a pending
      // tag. ExistingLegacy permits only the specifically absent new mutation
      // purpose; unknown partial root/link pairs still refuse unchanged.
      recovery=SnapshotRecoveryOwner::OpenExisting(private_parent);
      if (!recovery || recovery->error()) { error=recovery?recovery->error():Denied();return fail(); }
      auto old_profile_root=private_parent->Subroot(
          std::filesystem::path(b.plan.roots[4].path).filename(),false,error);
      if (!old_profile_root) {return fail();}
      auto mutation=private_parent->Subroot(std::filesystem::path(b.plan.roots[5].path).filename(),false,error);
      const bool has_mutation=bool(mutation);
      if (!mutation && error!=std::errc::no_such_file_or_directory) {return fail();}
      error.clear();
      const auto link=private_parent->CaptureSnapshot(
          "content-mutations-v1-"+BootHex(b.plan.uuid)+".owner",4096,error);
      if (!link || has_mutation!=link->exists() || (!has_mutation && !(b.plan.creates&16))) {
        if (!error) {error=BootConflict();}return fail();
      }
      std::vector<std::shared_ptr<const HostStorageRoot>> validation_private;
      if (b.plan.mode==4) {
        error=private_parent->PrivateDirectory();if (error) {return fail();}
        const auto state_id=private_parent->IntentRootIdentity(error);
        const auto text="Xenia managed-volume state\nversion=1\nsource="+options.source_id+
            "\ncontent="+b.plan.content+"\ndirectory="+std::to_string(live.directory)+
            "\nstate-directory="+std::to_string(state_id.directory)+"\n";
        const auto state_wire=Impl::Read(*private_parent,"owner.v1",true,error);
        if (error || !std::equal(state_wire.begin(),state_wire.end(),Bytes(text).begin(),Bytes(text).end())) {
          if (!error) {error=std::make_error_code(std::errc::bad_message);}return fail();
        }
        validation_private={private_parent,binding};
      } else {
        validation_private={recovery->root(),old_profile_root};
        if (mutation) {validation_private.push_back(mutation);}
      }
      // A temporary UNADMITTED namespace validates the old producer through
      // its ordinary owners without teaching them a disjointness bypass. The
      // original source EX lease excludes all normal metadata factories while
      // its canonical FD is transferred to/from this local validation cohort.
      auto validation_source=b.source->IndependentNamespaceView(error);
      if (!validation_source) {return fail();}
      // Reuse the existing native same-description rebind, not a new EX
      // acquisition or a path-equivalence lease bypass. The temporary
      // independent namespace must own its exact matching capability while
      // the original source lease remains retained for the whole migration.
      if (!b.source_lease || !b.source->IntentLeaseMatches(*b.source_lease)) {
        error=Denied();return fail();
      }
      auto validation_lease=validation_source->RebindFormatLease(*b.source_lease,error);
      if (!validation_lease || error) {return fail();}
      auto validation_projection=validation_source->InstallProjection(
          std::move(validation_private),{},false,error);
      if (!validation_projection) {return fail();}
      b.validation_projections.push_back(std::move(validation_projection));
      b.identity.reset();
      const bool validated=[&]() {
        auto identity=ManagedDeviceIdentity::OpenInternal(validation_source,options.source_id,
            options.device_id,error,false,false,validation_lease);
        if (!identity || error) {return false;}
        auto profile=ProfileIntentOwner::OpenInternal(private_parent,identity,false,validation_lease);
        if (!profile || profile->error()) {error=profile?profile->error():Denied();return false;}
        b.plan.roots[3].directory=recovery->root()->IntentRootIdentity(error).directory;
        b.plan.roots[4].directory=profile->root()->IntentRootIdentity(error).directory;
        if (has_mutation) {
          auto owner=ContentMutationOwner::OpenInternal(validation_source,identity,
              private_parent,recovery,validation_lease,false);
          if (!owner || !owner->ready()) {error=owner?owner->error():Denied();if (!error) {error=BootPending();}return false;}
          b.plan.roots[5].directory=mutation->IntentRootIdentity(error).directory;
          b.plan.creates&=~uint64_t(16);
        }
        return !error;
      }();
      if (!validated) {return fail();}
      b.identity=ManagedDeviceIdentity::OpenInternal(b.source,options.source_id,
          options.device_id,error,false,false,b.source_lease);
      if (!b.identity || error) {return fail();}
      const auto unchanged=b.identity->DescribeBootstrapCurrent(error);
      if (error || unchanged.marker_file!=b.plan.prior_marker_file ||
          b.identity->BootstrapWire(error)!=b.prefix_wire) {if (!error) {error=BootConflict();}return fail();}
      if (error) { return fail(); }
    }
    if (!binding) {
      auto capsule=source_anchor->BeginBootstrapDirectory(".xenia-device-binding-v1-"+key,
          BootHex(b.plan.nonce),error,anchor_lease);
      if (capsule) { b.capsules.push_back(std::move(capsule)); }
      if (error || b.capsules.empty()) { return fail(); }
      auto& exact=*b.capsules.back();
      b.plan.binding_directory=exact.native_directory();
      b.pending_wire=BootEncodePlan(b.plan,error);b.pending_digest=BootHash(b.pending_wire,error);
      if (error || !b.Boundary()) { return fail(); }
      auto written=exact.root()->PublishBootstrapRecord("bootstrap-source.v1",b.prefix_wire,BootHex(b.plan.nonce));
      if (written.error) { error=written.error;return fail(); }
      written=exact.root()->PublishBootstrapRecord("bootstrap.pending.v1",b.pending_wire,BootHex(b.plan.nonce));
      if (written.error) { error=written.error;return fail(); }
      if (!b.Boundary()) { return fail(); }
      auto published=source_anchor->PublishBootstrapDirectory(exact);binding=published.canonical_root;
      if (binding) { b.binding=binding;out.binding_root_=binding; }
      if (published.error || !published.durable || !binding) { error=published.error?published.error:BootConflict();return fail(); }
    } else {
      b.plan.binding_directory=binding->IntentRootIdentity(error).directory;
      b.pending_wire=BootEncodePlan(b.plan,error);b.pending_digest=BootHash(b.pending_wire,error);
      if (error) { return fail(); }
      auto written=binding->PublishBootstrapRecord("bootstrap-source.v1",b.prefix_wire,BootHex(b.plan.nonce));
      if (written.error) { error=written.error;return fail(); }
      written=binding->PublishBootstrapRecord("bootstrap.pending.v1",b.pending_wire,BootHex(b.plan.nonce));
      if (written.error) { error=written.error;return fail(); }
    }
    b.binding=binding;
    if (!b.Boundary()) { return fail(); }
  }
  if (private_parent) {
    private_parent=selected_parent(std::move(private_parent));
    if (!private_parent) {return fail();}
  }
  HostStorageBootstrapDirectory* binding_capsule=nullptr;
  for (const auto& capsule:b.capsules) {
    if (capsule->root()->path()==binding->path() &&
        capsule->native_directory()==b.plan.binding_directory) {
      binding_capsule=capsule.get();break;
    }
  }
  if (!binding_capsule && !inspect) {
    // The complete binding was validated above. A retained unrelated guest
    // may still own its old registered bootstrap protector after interruption.
    // Reclaim only that exact opaque native receipt under the SAME source EX
    // lease; a missing receipt grants no adoption authority.
    auto resumed=source_anchor->ResumeRetainedBootstrapDirectory(
        binding->path().filename(),*binding,error,anchor_lease);
    if (error) {return fail();}
    if (resumed) {
      binding_capsule=resumed.get();b.capsules.push_back(std::move(resumed));
      const auto observed=source_anchor->PublishBootstrapDirectory(*binding_capsule);
      if (observed.error || !observed.durable || !observed.canonical_root) {
        error=observed.error?observed.error:BootConflict();return fail();
      }
      binding=observed.canonical_root;b.binding=binding;
    }
  }
  if (binding_capsule) {
    // Generic ProtectFlat deliberately cannot change a bootstrap receipt's
    // role. Keep its existing protection until the common private-exclusion
    // settlement below, then acquire the ordinary flat binding authority.
    out.binding_root_=binding_capsule->root();
  } else {
    out.binding_root_=binding->ProtectFlat(error);
    if (!out.binding_root_) {return fail();}
  }
  const ManagedBootstrapReference reference{b.plan.epoch,b.plan.binding_directory,
      b.plan.nonce,b.pending_digest,b.plan.prefix};
  auto preserve=[&](const std::shared_ptr<HostStorageRoot>& parent,
                    const std::filesystem::path& relative,
                    uint64_t required_file,bool directory,
                    std::span<const uint8_t> required_bytes=std::span<const uint8_t>{}) {
    auto actual=parent->ObservePreparationArtifact(relative,error);
    if (error || (!actual && !required_file)) {return !error;}
    if (actual && required_file && actual->file_id!=required_file) {error=BootConflict();return false;}
    const auto parent_id=parent->IntentRootIdentity(error);if (error) {return false;}
    const auto file=actual?actual->file_id:required_file;
    std::vector<uint8_t> ticket{'X','M','V','B','S','0','1','\n'};
    BootRaw(ticket,b.pending_digest);Text(ticket,parent->path().generic_string());
    Number(ticket,parent_id.directory);Text(ticket,relative.generic_string());
    Number(ticket,file);Number(ticket,directory?1:0);
    const auto digest=BootHash(ticket,error);BootNonce nonce{};
    std::copy_n(digest.begin(),nonce.size(),nonce.begin());
    const auto name=BootHex(nonce);const auto destination="preserved-"+name+(directory?"-d":"-h");
    Text(ticket,destination);BootSeal(ticket,error);if (error) {return false;}
    if (!actual) {
      const auto recorded=Impl::Read(*binding,"bootstrap-preserve-"+name+".v1",true,error);
      auto preserved=error?std::optional<FileHandleInfo>{}:parent->ObservePreparationArtifact(destination,error);
      if (error || recorded!=ticket || !preserved || preserved->file_id!=file) {
        if (!error) {error=BootConflict();}return false;
      }
    }
    if (!required_bytes.empty()) {
      const auto token=parent->CaptureSnapshot(actual?relative:std::filesystem::path(destination),65536,error);
      if (!token || !token->exists() || !std::equal(required_bytes.begin(),required_bytes.end(),
                                                  token->bytes().begin(),token->bytes().end())) {
        if (!error) {error=BootConflict();}return false;
      }
    }
    if (!actual) {return true;}
    const auto recorded=binding->PublishBootstrapRecord("bootstrap-preserve-"+name+".v1",ticket,BootHex(BootRandom()));
    if (recorded.error) { error=recorded.error;return false; }
    if (!b.Boundary()) { return false; }
    error=parent->PreservePreparationArtifact(relative,actual->file_id,directory,*parent,destination);
    if (error) { return false; }
    if (directory) {
      auto root=parent->Subroot(destination,false,error);if (!root) { return false; }
      b.preserved_roots.push_back(std::move(root));
    }
    return b.Boundary();
  };
  const auto metadata_temporary=HostStorageRoot::MetadataBootstrapTemporary(BootHex(b.plan.nonce));
  if (!b.identity) {
    b.identity=ManagedDeviceIdentity::OpenInternal(b.source,options.source_id,
        options.device_id,error,false,false,b.source_lease,bootstrap);
    if (!b.identity && error==std::errc::no_such_file_or_directory && !committed &&
        !b.plan.prior_marker_file) {
      error.clear();
      if (!preserve(b.source,metadata_temporary,0,false)) { return fail(); }
      auto initial=b.prefix_wire;
      const auto suffix=ManagedDeviceIdentity::BootstrapSuffix(initial,reference,error);
      if (error) { return fail(); }BootRaw(initial,suffix);
      b.identity=ManagedDeviceIdentity::OpenInternal(b.source,options.source_id,
          options.device_id,error,true,false,b.source_lease,bootstrap,initial,BootHex(b.plan.nonce));
    }
    if (!b.identity || (error && !(inspect && error==std::errc::operation_in_progress))) {
      out.identity_=b.identity;return fail();
    }
  }
  out.identity_=b.identity;
  auto actual=b.identity->DescribeBootstrapCurrent(error);
  if (error) { return fail(); }
  if (inspect && !actual.format_pending) {
    error=std::make_error_code(std::errc::operation_not_permitted);return fail();
  }
  if (!actual.binding) {
    if (committed || actual.prefix_digest!=b.plan.prefix ||
        actual.marker_file!=b.plan.prior_marker_file) { error=BootConflict();return fail(); }
    // An earlier interrupted replacement may have left a partial/full NEW
    // control inode at the declared temporary name. Preserve it opaquely; it
    // is not the still-locked old canonical source and grants no tag authority.
    if (!preserve(b.source,metadata_temporary,0,false)) {return fail();}
    const auto appended=b.identity->AppendBootstrapReference(reference,b.source_lease,BootHex(b.plan.nonce));
    if (appended.error || !appended.durable) { error=appended.error?appended.error:BootConflict();return fail(); }
    if (!b.Boundary()) { return fail(); }
    actual=b.identity->DescribeBootstrapCurrent(error);
  }
  if (error || (!committed && !bootstrap->AllowsPending(*b.source,actual))) {
    if (!error) { error=BootConflict(); }return fail();
  }
  if (!committed && b.plan.prior_marker_file) {
    // The old locked marker inode is deliberately preserved, not an unlinkable
    // scratch file. A restart proves the same beforeimage and native inode.
    if (!preserve(b.source,metadata_temporary,b.plan.prior_marker_file,false,b.prefix_wire)) { return fail(); }
    const auto old=Impl::Read(*binding,"bootstrap-source.v1",true,error);
    if (error || old!=b.prefix_wire) { if (!error) {error=BootConflict();}return fail(); }
  }
  b.committed=committed;
  if (private_parent && (b.plan.mode==1 || b.plan.mode==4)) {
    // The complete binding already authenticated this exact private native
    // parent. Keep its exclusion even if a later owner payload is corrupt;
    // failed initialization must never enable an unfiltered guest fallback.
    auto validation_source=b.source->IndependentNamespaceView(error);
    if (!validation_source) {return fail();}
    auto exclusion=validation_source->InstallProjection({private_parent,out.binding_root_},{},false,error);
    if (!exclusion) {return fail();}
    b.validation_projections.push_back(std::move(exclusion));
  }
  std::map<std::filesystem::path,std::shared_ptr<const HostStorageIntentLease>> parent_leases;
  parent_leases.emplace(source_anchor->path(),anchor_lease);
  auto lease_for=[&](const std::shared_ptr<HostStorageRoot>& parent) {
    auto found=parent_leases.find(parent->path());
    if (found!=parent_leases.end()) {
      if (!parent->IntentLeaseMatches(*found->second)) { error=Denied();return std::shared_ptr<const HostStorageIntentLease>{}; }
      return found->second;
    }
    auto lease=parent->AcquireIntentLease(error,true);
    if (lease) { parent_leases.emplace(parent->path(),lease); }return lease;
  };
  using Fill=std::function<std::vector<uint8_t>(const std::shared_ptr<HostStorageRoot>&,bool,std::error_code&)>;
  auto component=[&](unsigned slot,const std::shared_ptr<HostStorageRoot>& parent,
                     const std::filesystem::path& target,const Fill& fill) {
    std::shared_ptr<HostStorageRoot> result;
    if (committed || !(b.plan.creates&(uint64_t(1)<<slot))) {
      result=parent->Subroot(target,false,error);if (!result) { return result; }
      const auto native=result->IntentRootIdentity(error);
      const unsigned root_slot=slot==0?1:slot==1?2:slot+1;
      const auto expected=committed?commit.directories[root_slot]:b.plan.roots[root_slot].directory;
      // Profile epoch directories may advance only through the real owned
      // format protocol. The ordinary factory validates their exact new wire.
      const bool later_profile=slot==3 && actual.epoch>b.plan.epoch;
      if (error || (!later_profile && expected && native.directory!=expected)) {
        if (!error) {error=BootConflict();}return std::shared_ptr<HostStorageRoot>{};
      }
      const auto proof=fill(result,false,error);static_cast<void>(proof);
      return error?std::shared_ptr<HostStorageRoot>{}:result;
    }
    const auto declaration_leaf=BootComponentLeaf(slot*2);
    const auto ready_leaf=BootComponentLeaf(slot*2+1);
    auto declaration=Impl::Read(*binding,declaration_leaf,false,error);
    if (error) { return result; }
    BootComponent declared;
    if (declaration.empty()) {
      declared.slot=slot;declared.parent=parent->IntentRootIdentity(error).directory;
      declared.pending=b.pending_digest;declared.target=target.generic_string();
      declared.nonce=BootRandom();
      declared.staging=HostStorageRoot::DirectoryBootstrapTemporary(target,BootHex(declared.nonce)).generic_string();
      if (error || declared.staging.empty()) { if (!error) {error=BootConflict();}return result; }
      declaration=BootEncodeComponent(declared,error);if (error) { return result; }
      const auto saved=binding->PublishBootstrapRecord(declaration_leaf,declaration,BootHex(BootRandom()));
      if (saved.error) { error=saved.error;return result; }
    } else { declared=BootDecodeComponent(declaration,error); }
    if (error || declared.phase || declared.slot!=slot || declared.target!=target.generic_string() ||
        declared.pending!=b.pending_digest ||
        declared.parent!=parent->IntentRootIdentity(error).directory ||
        declared.staging!=HostStorageRoot::DirectoryBootstrapTemporary(target,BootHex(declared.nonce)).generic_string()) {
      if (!error) {error=BootConflict();}return result;
    }
    if (!b.Boundary()) { return result; }
    const auto lease=lease_for(parent);if (!lease) { return result; }
    const auto ready_wire=Impl::Read(*binding,ready_leaf,false,error);if (error) { return result; }
    std::unique_ptr<HostStorageBootstrapDirectory> capsule;
    BootComponent ready=declared;
    if (!ready_wire.empty()) {
      ready=BootDecodeComponent(ready_wire,error);
      auto comparable=ready;comparable.phase=0;comparable.directory=0;comparable.payload={};
      if (error || ready.phase!=1 || BootEncodeComponent(comparable,error)!=declaration) {
        if (!error) {error=BootConflict();}return result;
      }
      capsule=parent->ResumeBootstrapDirectory(target,declared.staging,ready.directory,error,lease);
    } else {
      // A declared mkdir without its complete native-ID proof grants no
      // adoption authority. Preserve its exact observed inode/tree first.
      if (!preserve(parent,declared.staging,0,true)) { return result; }
      capsule=parent->BeginBootstrapDirectory(target,BootHex(declared.nonce),error,lease);
    }
    const bool captured=bool(capsule);
    if (capsule) { b.capsules.push_back(std::move(capsule)); }
    if (error || !captured) { return result; }
    auto& exact=*b.capsules.back();
    if (!b.Boundary()) { return result; }
    const auto payload=fill(exact.root(),ready_wire.empty(),error);
    if (error || payload.empty()) { if (!error) {error=BootConflict();}return result; }
    const auto digest=BootHash(payload,error);if (error) { return result; }
    if (ready_wire.empty()) {
      ready.phase=1;ready.directory=exact.native_directory();ready.payload=digest;
      const auto wire=BootEncodeComponent(ready,error);if (error) { return result; }
      const auto saved=binding->PublishBootstrapRecord(ready_leaf,wire,BootHex(BootRandom()));
      if (saved.error) { error=saved.error;return result; }
    } else if (ready.payload!=digest || ready.directory!=exact.native_directory()) {
      error=BootConflict();return result;
    }
    if (!b.Boundary()) { return result; }
    const auto published=parent->PublishBootstrapDirectory(exact);
    result=published.canonical_root;
    if (published.error || !published.durable || !result) {
      error=published.error?published.error:BootConflict();return result;
    }
    if (!b.Boundary()) { return result; }return result;
  };
  auto record=[&](const std::shared_ptr<HostStorageRoot>& root,
                  const std::filesystem::path& leaf,std::span<const uint8_t> bytes,bool create) {
    if (create) {
      const auto saved=root->PublishBootstrapRecord(leaf,bytes,BootHex(BootRandom()));
      if (saved.error) { error=saved.error;return false; }
    }
    const auto token=root->CaptureSnapshot(leaf,65536,error);
    if (!token || !token->exists() || !std::equal(bytes.begin(),bytes.end(),token->bytes().begin(),token->bytes().end())) {
      if (!error) {error=std::make_error_code(std::errc::bad_message);}return false;
    }
    const auto observed=root->ObserveIntentSnapshot(*token);
    if (observed.error || !observed.durable) {error=observed.error?observed.error:BootConflict();return false;}
    return true;
  };
  const auto private_leaf=std::filesystem::path(b.plan.roots[1].path).filename();
  if (!private_parent) {
    private_parent=component(0,state_parent,private_leaf,[&](const auto& root,bool create,std::error_code&) {
      const auto id=root->IntentRootIdentity(error);
      const auto text="Xenia managed-volume state\nversion=1\nsource="+options.source_id+
          "\ncontent="+b.plan.content+"\ndirectory="+std::to_string(live.directory)+
          "\nstate-directory="+std::to_string(id.directory)+"\n";
      auto bytes=Bytes(text);if (!error) {record(root,"owner.v1",bytes,create);}
      return std::vector<uint8_t>(bytes.begin(),bytes.end());
    });
    if (!private_parent || error) { return fail(); }
  } else if (b.plan.mode==4 || b.plan.mode==1) {
    // Reopen still validates the original exact state owner; root identity
    // alone is not sufficient to accept a corrupt complete binding.
    const auto id=private_parent->IntentRootIdentity(error);
    const auto text="Xenia managed-volume state\nversion=1\nsource="+options.source_id+
        "\ncontent="+b.plan.content+"\ndirectory="+std::to_string(live.directory)+
        "\nstate-directory="+std::to_string(id.directory)+"\n";
    if (error || !record(private_parent,"owner.v1",Bytes(text),false)) { return fail(); }
  }
  out.private_parent_=private_parent;out.state_parent_=state_parent;
  if (!public_root) {
    public_root=component(1,state_parent,std::filesystem::path(b.plan.roots[2].path).filename(),
        [&](const auto& root,bool create,std::error_code&) {
      if (create) { error=root->Create("Content",true); }
      auto shell=error?nullptr:root->Subroot("Content",false,error);
      std::vector<uint8_t> bytes{'X','M','V','P','U','B','1','\n'};
      if (shell) {
        Number(bytes,root->IntentRootIdentity(error).directory);
        Number(bytes,shell->IntentRootIdentity(error).directory);
      }
      return bytes;
    });
    if (!public_root || error) { return fail(); }
  }
  auto public_content=public_root->Subroot("Content",false,error);
  if (!public_content && !committed && !b.plan.prior_marker_file &&
      b.plan.roots[2].directory && error==std::errc::no_such_file_or_directory) {
    // Public browsing structure under an already explicit public authority,
    // not an identity/private owner. mkdir is atomic; no existing bytes are
    // replaced or inferred as private metadata on retry.
    if (!b.Boundary()) {return fail();}
    error=public_root->Create("Content",true);
    if (!error && !b.Boundary()) {return fail();}
    if (!error) {public_content=public_root->Subroot("Content",false,error);}
  }
  if (!public_content) {return fail();}
  error=public_content->CheckBootstrapBrowseDirectory(*b.source);if (error) {return fail();}
  error=public_content->FlushDirectory();if (!error) {error=public_root->FlushDirectory();}
  if (error) {return fail();}
  out.public_authority_=public_root;
  if (!recovery) {
    if (!committed && (b.plan.creates&4)) {
      constexpr std::string_view ownership="Xenia Edge recovery snapshots\nversion=1\npurpose=single-file-cas\n";
      auto recovery_root=component(2,private_parent,SnapshotRecoveryOwner::kDirectory,
          [&](const auto& root,bool create,std::error_code&) {
        const std::vector<uint8_t> bytes(ownership.begin(),ownership.end());record(root,"owner.v1",bytes,create);
        return std::vector<uint8_t>(bytes.begin(),bytes.end());
      });
      if (!recovery_root || error) { return fail(); }
    }
    HostStorageBootstrapDirectory* recovery_capsule=nullptr;
    for (const auto& capsule:b.capsules) {
      if (capsule->root()->path()==private_parent->path()/SnapshotRecoveryOwner::kDirectory) {
        recovery_capsule=capsule.get();break;
      }
    }
    recovery=SnapshotRecoveryOwner::OpenInternal(
        private_parent,false,0,recovery_capsule);
  }
  out.recovery_=recovery;
  if (!recovery || recovery->error() || recovery->CheckTarget(*b.source)) {
    error=recovery?recovery->error():Denied();if (!error) {error=Denied();}return fail();
  }
  const auto profile_leaf=std::string(ProfileIntentOwner::kDirectoryPrefix)+
      BootHex(b.plan.uuid)+"-"+std::to_string(actual.epoch);
  if (!committed && (b.plan.creates&8)) {
    const auto marker=ProfileIntentOwner::BootstrapMarker(*b.identity);
    auto root=component(3,private_parent,profile_leaf,
        [&](const auto& root,bool create,std::error_code&) {
      record(root,"owner.v1",marker,create);return marker;
    });
    if (!root || error) { return fail(); }
  }
  std::vector<uint8_t> mutation_marker;
  if (!committed && (b.plan.creates&16)) {
    auto root=component(4,private_parent,std::filesystem::path(b.plan.roots[5].path).filename(),
        [&](const auto& root,bool create,std::error_code&) {
      if (create) {
        error=root->Create("records",true);if (!error) {error=root->Create("staging",true);}
      }
      auto records=error?nullptr:root->Subroot("records",false,error);
      auto staging=error?nullptr:root->Subroot("staging",false,error);
      if (!records || !staging || error) {return std::vector<uint8_t>{};}
      mutation_marker=ContentMutationOwner::BootstrapMarker(*b.identity,*root,*records,*staging,error);
      if (!error) {record(records,"owner.v1",mutation_marker,create);}
      return mutation_marker;
    });
    if (!root || error) { return fail(); }
    const auto linked=private_parent->PublishBootstrapRecord("content-mutations-v1-"+BootHex(b.plan.uuid)+".owner",
        mutation_marker,BootHex(BootRandom()));
    if (linked.error) {error=linked.error;return fail();}
  }
  // A completed epoch at Prepared may not have created its new profile owner
  // yet. Inspection preserves the exact originally committed owner; creation
  // or validation of the current epoch remains BuildFormatVolume's typed job.
  const auto retained_profile=inspect?std::filesystem::path(b.plan.roots[4].path).filename():
      std::filesystem::path(profile_leaf);
  auto profile_native=private_parent->Subroot(retained_profile,false,error);
  if (inspect && profile_native &&
      profile_native->IntentRootIdentity(error).directory!=commit.directories[4]) {
    if (!error) {error=BootConflict();}return fail();
  }
  auto mutation_native=error?nullptr:private_parent->Subroot(
      std::filesystem::path(b.plan.roots[5].path).filename(),false,error);
  if (!profile_native || !mutation_native || error) {return fail();}
  // Projection admission is a native exclusion transaction, not a guest
  // mount or readiness claim. Settle complete private capsules first so the
  // existing owner factories keep their real disjointness checks.
  // Reconcile all preservation-only tickets as exact native observations.
  // This also seals orphan exclusions when a selected state parent is Content.
  for (const auto& leaf:binding->ListBootstrapRecords(error)) {
    if (!leaf.string().starts_with("bootstrap-preserve-") ||
        leaf.string().find(".partial-")!=std::string::npos) {continue;}
    const auto wire=Impl::Read(*binding,leaf,true,error);if (error) {return fail();}
    BootReader reader(wire,"XMVBS01\n");BootDigest pending{};reader.Raw(pending);
    const auto parent_path=reader.Text(4096);const auto parent_id=reader.Number();
    const auto original=reader.Text(256);const auto artifact_id=reader.Number();
    const auto directory=reader.Number();const auto destination=reader.Text(256);
    if (!reader.Done(error) || pending!=b.pending_digest || !parent_id ||
        !artifact_id || directory>1 || std::filesystem::path(original).has_parent_path() ||
        std::filesystem::path(destination).has_parent_path() || !destination.starts_with("preserved-")) {
      if (!error) {error=BootConflict();}return fail();
    }
    std::shared_ptr<HostStorageRoot> parent;
    for (const auto& candidate:{source_anchor,state_parent,private_parent}) {
      if (candidate->path().generic_string()==parent_path &&
          candidate->IntentRootIdentity(error).directory==parent_id) {parent=candidate;break;}
      if (error) {return fail();}
    }
    if (!parent) {error=BootConflict();return fail();}
    bool declared_artifact=parent->path()==b.source->path() && !directory &&
        original==metadata_temporary.generic_string();
    if (directory) {
      for (unsigned slot=0;slot<5;++slot) {
        const auto declared_wire=Impl::Read(*binding,BootComponentLeaf(slot*2),false,error);
        if (error) {return fail();}
        if (declared_wire.empty()) {continue;}
        const auto declared=BootDecodeComponent(declared_wire,error);if (error) {return fail();}
        if (declared.pending==b.pending_digest && declared.parent==parent_id &&
            declared.staging==original && declared.phase==0) {declared_artifact=true;break;}
      }
    }
    if (!declared_artifact) {error=BootConflict();return fail();}
    std::vector<uint8_t> prefix{'X','M','V','B','S','0','1','\n'};
    BootRaw(prefix,b.pending_digest);Text(prefix,parent_path);Number(prefix,parent_id);
    Text(prefix,original);Number(prefix,artifact_id);Number(prefix,directory);
    const auto digest=BootHash(prefix,error);BootNonce ticket_nonce{};
    std::copy_n(digest.begin(),ticket_nonce.size(),ticket_nonce.begin());
    const auto nonce=BootHex(ticket_nonce);
    if (error || leaf!=std::filesystem::path("bootstrap-preserve-"+nonce+".v1") ||
        destination!="preserved-"+nonce+(directory?"-d":"-h")) {if (!error) {error=BootConflict();}return fail();}
    auto preserved=parent->ObservePreparationArtifact(destination,error);
    if (!preserved || preserved->file_id!=artifact_id) {
      // A ticket may precede its exact preservation rename at process loss.
      // Complete only that identity-CAS; no unknown original is re-admitted.
      if (error) {return fail();}
      if (inspect) {error=BootConflict();return fail();} // inspection never repairs
      error=parent->PreservePreparationArtifact(original,artifact_id,directory!=0,*parent,destination);
      if (error) {return fail();}
    }
    if (directory) {
      auto root=parent->Subroot(destination,false,error);if (!root) {return fail();}
      b.preserved_roots.push_back(std::move(root));
    }
  }
  if (error) {return fail();}
  auto mounted=b.source->MountedFilesystemRoot(error);if (!mounted) {return fail();}
  const bool source_folder=mounted->IntentRootIdentity(error).directory!=live.directory;
  auto public_mounted=public_root->MountedFilesystemRoot(error);if (!public_mounted) {return fail();}
  const auto public_id=public_root->IntentRootIdentity(error);
  if (error || public_mounted->IntentRootIdentity(error).directory==public_id.directory) {
    if (!error) {error=Denied();}return fail();
  }
  // Never turn an actual filesystem root into a formatable public scope.
  // This folder remains native-only: exact complete component witnesses are
  // established before the unadmitted projection; actual owner recovery and
  // final binding commit still precede every GuestView below.
  public_root->SetFormatEligibility(true);
  HostStorageBootstrapDirectory* public_capsule=nullptr;
  for (const auto& capsule:b.capsules) {
    if (capsule->root()->path()==public_root->path() &&
        capsule->native_directory()==public_id.directory) {public_capsule=capsule.get();break;}
  }
  if (!public_capsule && !inspect && b.plan.mode==1 && (b.plan.creates&2)) {
    // Forward only an already-held exact parent lease. With an unrelated
    // external parent and no receipt, the native helper must remain a pure
    // observation; it acquires that parent's lease only if a receipt exists.
    std::shared_ptr<const HostStorageIntentLease> retained_parent_lease;
    if (const auto found=parent_leases.find(state_parent->path());
        found!=parent_leases.end()) {
      if (!state_parent->IntentLeaseMatches(*found->second)) {error=Denied();return fail();}
      retained_parent_lease=found->second;
    }
    auto retained=state_parent->ResumeRetainedBootstrapDirectory(
        std::filesystem::path(b.plan.roots[2].path).filename(),*public_root,error,
        std::move(retained_parent_lease));
    if (error) {return fail();}
    if (retained) {
      const auto declaration=Impl::Read(*binding,BootComponentLeaf(2),true,error);
      const auto ready=Impl::Read(*binding,BootComponentLeaf(3),true,error);
      const auto d=BootDecodeComponent(declaration,error),r=BootDecodeComponent(ready,error);
      if (error || d.pending!=b.pending_digest || r.pending!=b.pending_digest ||
          r.phase!=1 || r.directory!=public_id.directory || d.target!=retained->target().generic_string() ||
          d.staging!=retained->staging().generic_string()) {if (!error) {error=BootConflict();}return fail();}
      const auto published=state_parent->PublishBootstrapDirectory(*retained);
      if (published.error || !published.durable) {error=published.error?published.error:BootConflict();return fail();}
      public_root=published.canonical_root;public_root->SetFormatEligibility(true);
      public_capsule=retained.get();b.capsules.push_back(std::move(retained));
    }
  }
  std::vector<std::shared_ptr<const HostStorageRoot>> private_roots{out.binding_root_};
  if (b.plan.mode==1 || b.plan.mode==4) {private_roots.push_back(private_parent);}
  else {
    // The legacy parent may contain Content and ordinary user directories.
    // Only actual private purpose roots are excluded, never the whole parent.
    private_roots.insert(private_roots.end(),{recovery->root(),profile_native,mutation_native});
  }
  for (const auto& preserved:b.preserved_roots) {private_roots.push_back(preserved);}
  std::vector<HostStorageBootstrapDirectory*> settled_private;
  for (const auto& capsule:b.capsules) {
    const auto path=capsule->root()->path();
    if (capsule.get()==binding_capsule || path==private_parent->path() ||
        path==profile_native->path() || path==mutation_native->path()) {
      settled_private.push_back(capsule.get());
    }
  }
  if (inspect) {
    // A committed bootstrap plus the existing pending format is observation,
    // not a guest admission. Keep its authenticated exclusions AND common
    // SourceUnadmitted seal on the source InspectHandoff will reserve; otherwise
    // a Prepared restart would reset private owners as ordinary content.
    out.projection_=b.source->InstallBootstrapProjection(std::move(private_roots),
        {public_root},source_folder,{},nullptr,error);
    if (!out.projection_) {return fail();}
    b.validation_projections.clear();
    b.source_lease.reset(); // local anchor/cache dup descriptions die on return
    out.error_=BootPending();return true;
  }
  // Native V3 authenticates the exact public capsule family here; its private
  // protection remains live until atomic public admission, but is not mistaken
  // for a permanent private-tree exclusion in the common source projection.
  std::vector<std::shared_ptr<const HostStorageRoot>> public_namespaces{public_root};
  out.projection_=b.source->InstallBootstrapProjection(std::move(private_roots),
      std::move(public_namespaces),source_folder,settled_private,public_capsule,error);
  if (!out.projection_) {return fail();}
  // The replacement now retains every permanent private exclusion and the
  // SourceUnadmitted seal. Earlier validation projections may have captured
  // this exact public bootstrap receipt as private while it was unresolved;
  // they must retire before its authenticated public admission changes role.
  // Failure after this handoff still retains the new sealed projection.
  b.validation_projections.clear();
  if (binding_capsule) {
    out.binding_root_=binding_capsule->root()->ProtectFlat(error);
    if (!out.binding_root_) {return fail();}
  }
  if (!b.Boundary()) {return fail();} // metadata-only projection; no GuestView
  out.public_authority_=public_root->IndependentNamespaceView(error);
  if (!out.public_authority_) {return fail();}
  auto profile_permit=committed?nullptr:bootstrap->Permit(ManagedBootstrapCreationPermit::Purpose::kProfile,private_parent);
  if (!committed && !profile_permit) {error=Denied();return fail();}
  out.intents_=ProfileIntentOwner::OpenInternal(private_parent,b.identity,false,b.source_lease,profile_permit.get());
  if (!out.intents_ || out.intents_->error()) {error=out.intents_?out.intents_->error():Denied();return fail();}
  auto mutation_permit=committed?nullptr:bootstrap->Permit(ManagedBootstrapCreationPermit::Purpose::kContentMutation,private_parent);
  if (!committed && !mutation_permit) {error=Denied();return fail();}
  out.mutations_=ContentMutationOwner::OpenInternal(b.source,b.identity,private_parent,
      recovery,b.source_lease,false,mutation_permit.get());
  if (!out.mutations_ || out.mutations_->error()) {error=out.mutations_?out.mutations_->error():Denied();return fail();}
  std::array<std::shared_ptr<const HostStorageRoot>,6> roots{
      state_parent,private_parent,public_root,recovery->root(),out.intents_->root(),
      private_parent->Subroot(std::filesystem::path(b.plan.roots[5].path).filename(),false,error)};
  if (error || !roots[5]) {return fail();}
  BootCommit observed;
  observed.pending=b.pending_digest;
  actual=b.identity->DescribeBootstrapCurrent(error);observed.marker_file=actual.marker_file;
  for (unsigned i=0;i<roots.size();++i) {
    if (!roots[i] || roots[i]->path().generic_string()!=b.plan.roots[i].path) {
      // The profile is epoch-keyed; only a real committed format may move its
      // name. Its standard owner factory validated that exact new epoch wire.
      if (!(i==4 && committed && actual.epoch>b.plan.epoch && roots[i] &&
            roots[i]->path()==private_parent->path()/profile_leaf)) {
        error=BootConflict();return fail();
      }
    }
    observed.directories[i]=roots[i]->IntentRootIdentity(error).directory;
    if (error) {return fail();}
    if (committed && !(i==4 && actual.epoch>b.plan.epoch) &&
        observed.directories[i]!=commit.directories[i]) {error=BootConflict();return fail();}
    if (!committed && b.plan.roots[i].directory &&
        observed.directories[i]!=b.plan.roots[i].directory) {error=BootConflict();return fail();}
  }
  const auto b2=EncodeBinding({options.source_id,b.plan.content,b.plan.roots[0].path,
      b.plan.roots[2].path,live.directory,observed.directories[0],
      observed.directories[1],observed.directories[2]},error);
  if (error) {return fail();}
  const auto bound=binding->PublishBootstrapRecord("owner.v1",b2,BootHex(BootRandom()));
  if (bound.error) {error=bound.error;return fail();}
  observed.owner=BootHash(b2,error);
  if (error) {return fail();}
  if (!committed) {
    if (!b.Boundary()) {return fail();}
    const auto complete=BootEncodeCommit(observed,error);if (error) {return fail();}
    const auto published=binding->PublishBootstrapRecord("bootstrap.commit.v1",complete,BootHex(BootRandom()));
    if (published.error || !published.durable) {error=published.error?published.error:BootConflict();return fail();}
    committed=b.committed=true;commit=observed;
    if (!b.Boundary()) {return fail();}
    // A token-opened mutation owner was deliberately blocked while pending.
    // Final commit certifies the complete native structures, not fake journal
    // recovery. Actual recovery is mandatory before any guest/device admission.
    error=out.mutations_->Recover();if (error) {return fail();}
  }
  if (!out.mutations_->ready()) {error=BootPending();return fail();}
  error=b.identity->CheckCurrent();if (error) {return fail();}
  ManagedBootstrapGuestAdmissionPermit admission(bootstrap,b.source,b.source_lease,
      b.identity,out.projection_,out.mutations_);
  error=b.source->AdmitBootstrapProjection(admission);if (error) {return fail();}

  std::shared_ptr<HostStorageRoot> public_view;
  if (public_capsule) {
    public_view=state_parent->AdmitBootstrapPublicDirectory(*public_capsule,error,b.source.get());
  } else {
    public_view=public_root->IndependentNamespaceView(error);
    if (!public_view) {return fail();}
    error=public_view->AttachPublicProjection(*b.source);if (error) {return fail();}
    public_view=public_view->GuestView(error);
  }
  if (!public_view || error) {return fail();}
  out.public_root_=std::move(public_view);
  error=b.identity->AdmitBootstrapIdentity();if (error) {return fail();}
  // The committed projection retains exact private exclusions. All typed
  // owners/protections were acquired before releasing bootstrap capsules.
  b.capsules.clear();
  out.error_.clear();return true;
}

std::shared_ptr<const ManagedStorageContext> ManagedStorageContext::Capture(
    std::shared_ptr<HostStorageRoot> data_parent,
    std::shared_ptr<const SnapshotRecoveryOwner> legacy_recovery,
    std::shared_ptr<HostStorageRoot> state_parent_override,
    std::error_code& error) {
  error.clear();
  if (xe::global_critical_region::is_held_by_current_thread()) {
    error = std::make_error_code(std::errc::operation_not_permitted);
    return {};
  }
  if (!data_parent || data_parent->read_only() ||
      data_parent->is_guest_private() ||
      !data_parent->native_filesystem_authority() || !legacy_recovery ||
      legacy_recovery->error() || !legacy_recovery->root() ||
      legacy_recovery->root()->path() !=
          data_parent->path() / SnapshotRecoveryOwner::kDirectory) {
    error = Denied();
    return {};
  }
  auto result =
      std::shared_ptr<ManagedStorageContext>(new ManagedStorageContext);
  result->data_parent_ = std::move(data_parent);
  result->legacy_recovery_ = std::move(legacy_recovery);
  result->state_parent_override_ = std::move(state_parent_override);
  error = result->CheckCurrent();
  return error ? nullptr : result;
}
std::error_code ManagedStorageContext::CheckCurrent() const {
  if (xe::global_critical_region::is_held_by_current_thread()) {
    return std::make_error_code(std::errc::operation_not_permitted);
  }
  if (!data_parent_ || !legacy_recovery_ || !legacy_recovery_->root()) {
    return Denied();
  }
  if (auto error = data_parent_->Validate()) {
    return error;
  }
  if (auto error = legacy_recovery_->CheckTarget(*data_parent_)) {
    return error;
  }
  if (state_parent_override_) {
    if (state_parent_override_->read_only() ||
        state_parent_override_->is_guest_private() ||
        !state_parent_override_->native_filesystem_authority()) {
      return Denied();
    }
    if (auto error = state_parent_override_->Validate()) {
      return error;
    }
  }
  return {};
}
std::shared_ptr<const ManagedVolumeAuthority> ManagedVolumeAuthority::Open(
    Options options) {
  return OpenInternal(std::move(options), false);
}
std::shared_ptr<const ManagedVolumeAuthority>
ManagedVolumeAuthority::OpenInternal(Options options, bool inspect, unsigned bootstrap_stop) {
  auto result =
      std::shared_ptr<ManagedVolumeAuthority>(new ManagedVolumeAuthority);
  auto& error = result->error_;
  if (xe::global_critical_region::is_held_by_current_thread()) {
    error = std::make_error_code(std::errc::operation_not_permitted);
    return result;
  }
  auto content_anchor = std::move(options.content);
  result->content_ =
      content_anchor ? content_anchor->Subroot({}, false, error) : nullptr;
  if (!result->content_ || options.source_id.empty() ||
      options.source_id.size() > 256 ||
      options.source_id.find('\0') != std::string::npos || !options.device_id ||
      result->content_->read_only() || result->content_->is_guest_private()) {
    error = Denied();
    return result;
  }
  // One native cooperating-owner lease covers the entire initialization,
  // including all refusal/partial results. No Space guard crosses these calls.
  if (!inspect) {
    result->source_lease_ = result->content_->AcquireIntentLease(error, true);
    if (!result->source_lease_) { return result; }
  }
  static_cast<void>(result->content_->IntentRootIdentity(error));
  if (error) {
    return result;
  }
  if (ManagedStorageBootstrap::Open(*result,options,content_anchor,inspect,bootstrap_stop)) { return result; }
  return OpenLegacyInternal(std::move(options), std::move(content_anchor),
                            std::move(result), inspect);
}
std::shared_ptr<const ManagedVolumeAuthority>
ManagedVolumeAuthority::OpenLegacyInternal(
    Options options, std::shared_ptr<HostStorageRoot> content_anchor,
    std::shared_ptr<ManagedVolumeAuthority> result, bool inspect) {
  auto& error = result->error_;
  // Native identity observation belongs to this actual producer, whose
  // existing friendship grants precisely that authority, not to its caller.
  const auto source_identity = result->content_->IntentRootIdentity(error);
  if (error) { return result; }
  std::shared_ptr<const ManagedDeviceIdentity> inspected_identity;
  bool existing_marker = false;
  if (inspect) {
    // Explicit recovery resolves only an existing marker, without a guest
    // mount or an initial source lease that would self-conflict with drain.
    inspected_identity = ManagedDeviceIdentity::OpenInternal(
        result->content_, options.source_id, options.device_id, error,
        false, false);
    if (!inspected_identity ||
        (error && error != std::errc::operation_in_progress)) { return result; }
    existing_marker = true;
    error.clear();
  } else {
    existing_marker = ManagedDeviceIdentity::CheckExisting(
        result->content_, options.source_id, options.device_id, error);
    if (error) { return result; }
  }
  const auto key = Key(options.source_id, result->content_->path(), error);
  if (error) {
    return result;
  }
  const auto entries = content_anchor->ManagedVolumeEntries(key, error);
  if (error) {
    return result;
  }
  if ((entries && !(entries & 1)) || (entries && !existing_marker) ||
      (options.preserve_legacy_layout && entries)) {
    error = std::make_error_code(std::errc::state_not_recoverable);
    return result;
  }
  const bool reuse = (entries & 1) != 0;
  auto same_fs = [&](const std::shared_ptr<HostStorageRoot>& root) {
    if (!root) {
      error = Denied();
      return false;
    }
    const auto identity = root->IntentRootIdentity(error);
    if (!error && identity.filesystem != source_identity.filesystem) {
      error = std::make_error_code(std::errc::cross_device_link);
    }
    return !error;
  };
  auto mounted = result->content_->MountedFilesystemRoot(error);
  if (!mounted) {
    return result;
  }
  const auto mounted_identity = mounted->IntentRootIdentity(error);
  if (error) {
    return result;
  }
  const bool folder = source_identity.directory != mounted_identity.directory;
  bool known_legacy = false;
  std::shared_ptr<HostStorageRoot> binding_parent;
  if (!options.preserve_legacy_layout && existing_marker && !reuse) {
    // A pre-V1 source is not fresh ownership. Only an explicit known producer
    // bundle can preserve its existing redo/recovery/public locations.
    if (!options.existing_legacy) {
      error = Denied();
      return result;
    }
    const auto& old = *options.existing_legacy;
    if (!same_fs(old.private_parent) || !same_fs(old.public_root) ||
        !old.recovery || old.recovery->error() ||
        old.recovery->CheckTarget(*result->content_) ||
        (options.state_parent_override &&
         options.state_parent_override->path() != old.private_parent->path())) {
      if (!error) {
        error = Denied();
      }
      return result;
    }
    result->private_parent_ = old.private_parent;
    result->public_root_ = old.public_root;
    result->recovery_ = SnapshotRecoveryOwner::OpenExisting(old.private_parent);
    if (!result->recovery_ || result->recovery_->error() ||
        result->recovery_->root()->path() != old.recovery->root()->path()) {
      error = result->recovery_ ? result->recovery_->error() : Denied();
      if (!error) {
        error = Denied();
      }
      return result;
    }
    if (!result->public_root_->Subroot("Content", false, error)) {
      return result;
    }
    known_legacy = true;
  }
  if (options.preserve_legacy_layout || known_legacy) {
    if (!known_legacy) {
      result->private_parent_ = options.legacy_private_parent;
      result->public_root_ = options.legacy_public_root;
      result->recovery_ = options.legacy_recovery;
    }
    if (!same_fs(result->private_parent_) || !same_fs(result->public_root_) ||
        !result->recovery_ || result->recovery_->error()) {
      if (!error) {
        error = Denied();
      }
      return result;
    }
    error = result->recovery_->CheckTarget(*result->content_);
    if (error) {
      return result;
    }
    result->state_parent_ = result->private_parent_;
    result->content_->SetFormatEligibility(folder);
  } else {
    if (options.state_parent_override &&
        !same_fs(options.state_parent_override)) {
      return result;
    }
    auto parent =
        options.state_parent_override ? options.state_parent_override : mounted;
    if (!reuse) {
      // Absence at the explicit preferred parent is checked without creating
      // anything. Existing/unknown partial owners never become a fresh bundle.
      for (const auto& leaf : {".xenia-device-state-v1-" + key,
                               ".xenia-device-volume-v1-" + key}) {
        if (parent->Subroot(leaf, false, error)) {
          error = std::make_error_code(std::errc::state_not_recoverable);
          return result;
        }
        if (error != std::errc::no_such_file_or_directory) {
          return result;
        }
        error.clear();
      }
    }
    bool binding_created = false;
    const auto binding_leaf = ".xenia-device-binding-v1-" + key;
    auto binding_raw =
        reuse ? content_anchor->Subroot(binding_leaf, false, error)
              : Child(content_anchor, binding_leaf, binding_created, error);
    if (!binding_raw || (!reuse && !binding_created)) {
      if (!error) {
        error = std::make_error_code(std::errc::state_not_recoverable);
      }
      return result;
    }
    error = binding_raw->CheckStateContainer(false);
    if (error) {
      return result;
    }
    result->binding_root_ = binding_raw->ProtectFlat(error);
    if (!result->binding_root_) {
      return result;
    }
    Binding binding;
    if (reuse) {
      const auto stored =
          result->binding_root_->CaptureSnapshot("owner.v1", 16384, error);
      if (error || !stored || !stored->exists()) {
        if (!error) {
          error = std::make_error_code(std::errc::bad_message);
        }
        return result;
      }
      binding = DecodeBinding(stored->bytes(), error);
      if (error || binding.source != options.source_id ||
          binding.content != result->content_->path().generic_string() ||
          binding.content_directory != source_identity.directory) {
        if (!error) {
          error = std::make_error_code(std::errc::state_not_recoverable);
        }
        return result;
      }
      parent = HostStorageRoot::Open(binding.parent, false, error, 0);
      if (!parent || !same_fs(parent)) {
        return result;
      }
      const auto id = parent->IntentRootIdentity(error);
      if (error || parent->path().generic_string() != binding.parent ||
          id.directory != binding.parent_directory ||
          (options.state_parent_override &&
           options.state_parent_override->path() != parent->path())) {
        if (!error) {
          error = std::make_error_code(std::errc::state_not_recoverable);
        }
        return result;
      }
    }
    if (!same_fs(parent)) {
      return result;
    }
    const auto private_leaf = ".xenia-device-state-v1-" + key;
    bool created = false, denied_missing_creation = false;
    result->private_parent_ = reuse
                                  ? parent->Subroot(private_leaf, false, error)
                                  : Child(parent, private_leaf, created, error,
                                          &denied_missing_creation);
    if (!result->private_parent_ && !reuse && !options.state_parent_override &&
        denied_missing_creation) {
      parent = content_anchor;
      created = false;
      result->private_parent_ = Child(parent, private_leaf, created, error);
    }
    binding_parent = parent;
    result->state_parent_ = parent;
    if (!result->private_parent_ || (!reuse && !created)) {
      if (!error) {
        error = std::make_error_code(std::errc::state_not_recoverable);
      }
      return result;
    }
    error = result->private_parent_->CheckStateContainer(false);
    if (error) {
      return result;
    }
    const auto private_identity =
        result->private_parent_->IntentRootIdentity(error);
    if (error) {
      return result;
    }
    if (reuse && private_identity.directory != binding.private_directory) {
      error = std::make_error_code(std::errc::state_not_recoverable);
      return result;
    }
    bool public_created = false;
    const auto public_leaf = ".xenia-device-volume-v1-" + key;
    result->public_root_ =
        options.legacy_public_root ? options.legacy_public_root
        : reuse ? parent->Subroot(public_leaf, false, error)
                : Child(parent, public_leaf, public_created, error);
    if (!result->public_root_ || !same_fs(result->public_root_)) {
      return result;
    }
    if (!options.legacy_public_root) {
      error = result->public_root_->CheckStateContainer(true);
      if (error) {
        return result;
      }
      if (!reuse && !public_created) {
        error = std::make_error_code(std::errc::state_not_recoverable);
        return result;
      }
    }
    const auto public_identity =
        result->public_root_->IntentRootIdentity(error);
    if (error ||
        (reuse && (public_identity.directory != binding.public_directory ||
                   result->public_root_->path().generic_string() !=
                       binding.public_root))) {
      if (!error) {
        error = std::make_error_code(std::errc::state_not_recoverable);
      }
      return result;
    }
    std::vector<std::shared_ptr<const HostStorageRoot>> namespaces;
    if (!options.legacy_public_root) {
      namespaces.push_back(result->public_root_);
    }
    if (!inspect) {
      result->projection_ = result->content_->InstallProjection(
          {result->binding_root_, result->private_parent_}, std::move(namespaces),
          folder, error);
      if (!result->projection_) {
        return result;
      }
    }
    const std::string marker =
        "Xenia managed-volume state\nversion=1\nsource=" + options.source_id +
        "\ncontent=" + result->content_->path().generic_string() +
        "\ndirectory=" + std::to_string(source_identity.directory) +
        "\nstate-directory=" + std::to_string(private_identity.directory) +
        "\n";
    if (!reuse) {
      error = parent->FlushDirectory();
      if (error) {
        return result;
      }
      const auto written =
          result->private_parent_->WriteNewFile("owner.v1", Bytes(marker));
      error = written.error;
      if (!error && written.bytes != marker.size()) {
        error = std::make_error_code(std::errc::io_error);
      }
      if (error) {
        return result;
      }
    }
    const auto observed =
        result->private_parent_->CaptureSnapshot("owner.v1", 8192, error);
    if (error || !observed || !observed->exists() ||
        !std::equal(observed->bytes().begin(), observed->bytes().end(),
                    Bytes(marker).begin(), Bytes(marker).end())) {
      if (!error) {
        error = std::make_error_code(std::errc::bad_message);
      }
      return result;
    }
    auto content_shell = result->public_root_->Subroot("Content", false, error);
    if (!content_shell && !reuse &&
        error == std::errc::no_such_file_or_directory) {
      error = result->public_root_->Create("Content", true);
      if (!error) {
        content_shell = result->public_root_->Subroot("Content", false, error);
      }
    }
    if (!content_shell) {
      return result;
    }
    result->recovery_ =
        reuse ? SnapshotRecoveryOwner::OpenExisting(result->private_parent_)
              : SnapshotRecoveryOwner::Open(result->private_parent_);
    if (!result->recovery_ || result->recovery_->error()) {
      error = result->recovery_ ? result->recovery_->error() : Denied();
      return result;
    }
  }
  // Public guest generation is separately disposable; its actual authority
  // must survive an owned format without an arbitrary path reopen.
  result->public_authority_ = result->public_root_;
  if (inspect) {
    result->identity_ = std::move(inspected_identity);
    error = std::make_error_code(std::errc::operation_in_progress);
    return result;  // No intents, new epoch, public guest view or readiness.
  }
  result->public_root_ = result->public_root_->IndependentNamespaceView(error);
  if (!result->public_root_) {
    return result;
  }
  if (result->projection_) {
    error = result->public_root_->AttachPublicProjection(*result->content_);
    if (error) {
      return result;
    }
  }
  result->public_root_ = result->public_root_->GuestView(error);
  if (!result->public_root_) {
    return result;
  }
  result->identity_ =
      existing_marker
          ? ManagedDeviceIdentity::OpenExisting(
                result->content_, options.source_id, options.device_id, error)
          : ManagedDeviceIdentity::Open(result->content_, options.source_id,
                                        options.device_id, error);
  if (!result->identity_ || error) {
    return result;
  }
  result->intents_ = ProfileIntentOwner::OpenInternal(
      result->private_parent_, result->identity_,
      !existing_marker && !reuse && !known_legacy, result->source_lease_);
  if (!result->intents_ || result->intents_->error()) {
    error = result->intents_ ? result->intents_->error() : Denied();
    return result;
  }
  if (!result->source_lease_) {
    // The historical producer's real intent factory acquires this EX after
    // ordinary MDI metadata admission. Retain that SAME successful lease for
    // the coupled mutation owner; never mint a name-equivalent substitute.
    if (result->intents_->source() != result->content_ ||
        result->intents_->managed_identity() != result->identity_ ||
        !result->intents_->source_lease_ ||
        !result->content_->IntentLeaseMatches(*result->intents_->source_lease_)) {
      error = Denied(); return result;
    }
    result->source_lease_ = result->intents_->source_lease_;
  }
  result->mutations_ = ContentMutationOwner::OpenInternal(
      result->content_, result->identity_, result->private_parent_,
      result->recovery_, result->source_lease_, true);
  if (!result->mutations_ || result->mutations_->error()) {
    error = result->mutations_ ? result->mutations_->error() : Denied();
    return result;
  }
  if (!options.preserve_legacy_layout && !known_legacy && !reuse) {
    const auto private_identity =
        result->private_parent_->IntentRootIdentity(error);
    if (error) {
      return result;
    }
    const auto public_identity =
        result->public_root_->IntentRootIdentity(error);
    if (error) {
      return result;
    }
    const auto parent_identity = binding_parent->IntentRootIdentity(error);
    if (error) {
      return result;
    }
    const auto wire = EncodeBinding(
        {options.source_id, result->content_->path().generic_string(),
         binding_parent->path().generic_string(),
         result->public_root_->path().generic_string(),
         source_identity.directory, parent_identity.directory,
         private_identity.directory, public_identity.directory},
        error);
    if (error) {
      return result;
    }
    error = content_anchor->FlushDirectory();
    if (error) {
      return result;
    }
    // Final commit: every identity, source lease, recovery, intent and public
    // view is already ready. A partial directory never counts as committed.
    const auto written = result->binding_root_->WriteNewFile("owner.v1", wire);
    error = written.error;
    if (!error && written.bytes != wire.size()) {
      error = std::make_error_code(std::errc::io_error);
    }
  }
  return result;
}
std::array<uint8_t, 32> ManagedVolumeAuthority::FormatBindingDigest(
    const ManagedVolumeAuthority& volume, std::error_code& error) {
  error.clear();
  std::array<uint8_t, 32> digest{};
  if (!volume.content_ || !volume.private_parent_ || !volume.public_authority_ ||
      !volume.state_parent_ || !volume.identity_ || !volume.recovery_ ||
      volume.recovery_->error() || !volume.recovery_->root() ||
      volume.identity_->source() != volume.content_) {
    error = Denied(); return digest;
  }
  std::vector<uint8_t> wire{'X','M','V','F','B','1','\n'};
  Text(wire, volume.identity_->source_id());
  Number(wire, volume.identity_->device_id());
  wire.insert(wire.end(), volume.identity_->uuid().begin(),
              volume.identity_->uuid().end());
  std::vector<HostStorageRootIdentity> roots;
  for (const auto& root : {volume.content_, volume.private_parent_,
                           volume.public_authority_, volume.state_parent_,
                           volume.binding_root_, volume.recovery_->root()}) {
    if (!root) {
      Text(wire, ""); Number(wire, 0); roots.push_back({}); continue;
    }
    const auto identity = root->IntentRootIdentity(error);
    if (error) { return digest; }
    Text(wire, root->path().generic_string());
    Number(wire, identity.directory);  // Reboot-stable ID, not live mount/st_dev.
    roots.push_back(identity);
  }
  if (volume.binding_root_) {
    const auto stored = volume.binding_root_->CaptureSnapshot("owner.v1", 16384,
                                                              error);
    if (error || !stored || !stored->exists()) {
      if (!error) { error = std::make_error_code(std::errc::bad_message); }
      return digest;
    }
    const auto binding = DecodeBinding(stored->bytes(), error);
    if (error || binding.source != volume.identity_->source_id() ||
        binding.content != volume.content_->path().generic_string() ||
        binding.content_directory != roots[0].directory ||
        binding.private_directory != roots[1].directory ||
        binding.public_directory != roots[2].directory ||
        binding.public_root != volume.public_authority_->path().generic_string() ||
        binding.parent != volume.state_parent_->path().generic_string() ||
        binding.parent_directory != roots[3].directory) {
      if (!error) { error = std::make_error_code(std::errc::state_not_recoverable); }
      return digest;
    }
    Number(wire, stored->bytes().size());
    wire.insert(wire.end(), stored->bytes().begin(), stored->bytes().end());
  } else {
    Number(wire, 0);  // Explicit existing primary/legacy owner configuration.
  }
  if (mbedtls_sha256(wire.data(), wire.size(), digest.data(), 0)) {
    error = std::make_error_code(std::errc::io_error);
  }
  return digest;
}
std::unique_ptr<ManagedVolumeFormatHandoff>
ManagedVolumeAuthority::CaptureFormatHandoff() const {
  auto ticket = std::unique_ptr<ManagedVolumeFormatHandoff>(
      new ManagedVolumeFormatHandoff);
  ticket->identity_ = identity_;
  ticket->source_id_ = identity_ ? identity_->source_id() : std::string{};
  if (xe::global_critical_region::is_held_by_current_thread()) {
    ticket->error_ = std::make_error_code(std::errc::operation_not_permitted);
    return ticket;
  }
  if (!format_eligible()) {
    ticket->error_ = std::make_error_code(std::errc::operation_not_supported);
    return ticket;
  }
  ticket->binding_ = FormatBindingDigest(*this, ticket->error_);
  if (ticket->error_) { return ticket; }
  ticket->content_ = content_;
  ticket->private_parent_ = private_parent_;
  ticket->public_authority_ = public_authority_;
  ticket->state_parent_ = state_parent_;
  ticket->binding_root_ = binding_root_;
  ticket->old_public_view_ = public_root_;
  ticket->recovery_ = recovery_;
  ticket->projection_ = projection_;
  ticket->previous_intents_ = intents_;
  ticket->previous_mutations_ = mutations_;  // Weak; never blocks actual drain.
  return ticket;
}
std::unique_ptr<ManagedVolumeFormatHandoff>
ManagedVolumeAuthority::InspectFormatRecovery(Options options) {
  auto ticket = std::unique_ptr<ManagedVolumeFormatHandoff>(
      new ManagedVolumeFormatHandoff);
  const auto inspected = OpenInternal(std::move(options), true);
  if (!inspected || inspected->error_ != std::errc::operation_in_progress ||
      !inspected->identity_ || !inspected->content_->format_eligible()) {
    ticket->error_ = inspected && inspected->error_ ? inspected->error_
        : std::make_error_code(std::errc::operation_not_permitted);
    return ticket;
  }
  ticket->binding_ = FormatBindingDigest(*inspected, ticket->error_);
  if (ticket->error_) { return ticket; }
  ticket->identity_ = inspected->identity_;
  ticket->source_id_ = ticket->identity_->source_id();
  ticket->content_ = inspected->content_;
  ticket->private_parent_ = inspected->private_parent_;
  ticket->public_authority_ = inspected->public_authority_;
  ticket->state_parent_ = inspected->state_parent_;
  ticket->binding_root_ = inspected->binding_root_;
  ticket->recovery_ = inspected->recovery_;
  ticket->projection_ = inspected->projection_;
  ticket->operation_ = ManagedDeviceIdentity::InspectHandoff(
      ticket->identity_, ticket->binding_);
  ticket->error_ = ticket->operation_->error;
  if (!ticket->error_ && !inspected->bootstrap_) {
    ticket->error_ = ReconstructLegacyFormatInspection(*ticket);
    if (ticket->error_) {
      // No executable format capability escapes a failed legacy proof/seal.
      // Generic Resume deliberately retries retained native operations; this
      // prerequisite failure instead requires a fresh authenticated inspection.
      ticket->operation_.reset();
    }
  }
  return ticket;
}
std::error_code ManagedVolumeAuthority::ReconstructLegacyFormatInspection(
    ManagedVolumeFormatHandoff& ticket) {
  // The caller already validated the real legacy B2/explicit producer bundle
  // and reserved the actual existing format Handoff. No create, empty permit,
  // source-marker rewrite or guessed directory identity is admitted here.
  if (global_critical_region::is_held_by_current_thread() || !ticket.operation_) {
    return Denied();
  }
  auto& handoff = *ticket.operation_;
  if (handoff.error || !handoff.prepared_durable || !handoff.old_epoch ||
      handoff.phase < 1 || handoff.phase > 3 || handoff.binding != ticket.binding_ ||
      handoff.identity != ticket.identity_ || !ticket.content_ ||
      handoff.identity->source() != ticket.content_ || !handoff.source_lease ||
      !ticket.content_->IntentLeaseMatches(*handoff.source_lease) ||
      ticket.content_->has_guest_projection() || ticket.projection_ ||
      !ticket.private_parent_ || !ticket.public_authority_ || !ticket.state_parent_ ||
      !ticket.recovery_ || ticket.recovery_->error()) {
    return Denied();
  }
  std::error_code error;
  const auto binding = FormatTicketDigest(ticket, error);
  if (error || binding != ticket.binding_) {
    return error ? error : std::make_error_code(std::errc::state_not_recoverable);
  }
  error = ticket.recovery_->CheckTarget(*ticket.content_);
  if (error) { return error; }
  // These capabilities predate OpenInternal's fresh content_ child: the B2
  // producer resolved them from content_anchor/its authenticated actual parent,
  // or the explicit legacy owner supplied them. They cannot depend on that
  // newly-created logical source generation, and survive its owned retirement.
  const auto profile_leaf = std::string(ProfileIntentOwner::kDirectoryPrefix) +
      BootHex(ticket.identity_->uuid()) + "-" + std::to_string(handoff.old_epoch);
  auto profile = ticket.private_parent_->Subroot(profile_leaf, false, error);
  if (!profile) { return error; }
  profile = profile->ProtectProfileIntents(error);
  if (!profile) { return error; }
  const auto profile_marker = profile->CaptureSnapshot("owner.v1", 1024, error);
  const auto expected_profile = ProfileIntentOwner::FormatInspectionMarker(
      *ticket.identity_, handoff.old_epoch);
  if (error || !profile_marker || !profile_marker->exists() ||
      !std::equal(profile_marker->bytes().begin(), profile_marker->bytes().end(),
                  expected_profile.begin(), expected_profile.end())) {
    return error ? error : std::make_error_code(std::errc::bad_message);
  }
  const auto uuid = BootHex(ticket.identity_->uuid());
  auto mutation = ticket.private_parent_->Subroot(
      ".xenia-content-mutations-v1-" + uuid, false, error);
  if (!mutation) { return error; }
  auto records = mutation->Subroot("records", false, error);
  auto staging = error ? nullptr : mutation->Subroot("staging", false, error);
  if (!records || !staging || error) { return error ? error : Denied(); }
  error = mutation->PrivateDirectory();
  if (!error) { error = records->PrivateDirectory(); }
  if (!error) { error = staging->PrivateDirectory(); }
  if (error) { return error; }
  // The existing typed content journal policy applies to the flat records
  // directory. Its parent contains records/staging directories and remains an
  // exact native container excluded by the source projection below.
  records = records->ProtectContentMutations(error);
  if (!records) { return error; }
  const auto expected_mutation = ContentMutationOwner::BootstrapMarker(
      *ticket.identity_, *mutation, *records, *staging, error);
  if (error) { return error; }
  const auto owner = records->CaptureSnapshot("owner.v1", 4096, error);
  const auto link = error ? nullptr : ticket.private_parent_->CaptureSnapshot(
      "content-mutations-v1-" + uuid + ".owner", 4096, error);
  if (error || !owner || !owner->exists() || !link || !link->exists() ||
      !std::equal(owner->bytes().begin(), owner->bytes().end(),
                  expected_mutation.begin(), expected_mutation.end()) ||
      !std::equal(link->bytes().begin(), link->bytes().end(),
                  expected_mutation.begin(), expected_mutation.end())) {
    return error ? error : std::make_error_code(std::errc::bad_message);
  }
  std::shared_ptr<HostStorageRoot> current_profile;
  if (ticket.identity_->content_epoch() != handoff.old_epoch) {
    const auto epoch = ticket.identity_->content_epoch();
    if (handoff.old_epoch == UINT64_MAX || epoch != handoff.old_epoch + 1) {
      return std::make_error_code(std::errc::state_not_recoverable);
    }
    const auto leaf = std::string(ProfileIntentOwner::kDirectoryPrefix) +
        uuid + "-" + std::to_string(epoch);
    current_profile = ticket.private_parent_->Subroot(leaf, false, error);
    if (!current_profile) {
      // Prepared may have completed reset without creating the new owner yet.
      // Later durable phases certify existence and remain noncreating.
      if (handoff.phase != 1 || error != std::errc::no_such_file_or_directory) {
        return error ? error : Denied();
      }
      error.clear();
    } else {
      current_profile = current_profile->ProtectProfileIntents(error);
      if (!current_profile) { return error; }
      const auto token = current_profile->CaptureSnapshot("owner.v1", 1024, error);
      const auto marker = ProfileIntentOwner::FormatInspectionMarker(*ticket.identity_, epoch);
      if (error || !token || !token->exists() ||
          !std::equal(token->bytes().begin(), token->bytes().end(), marker.begin(), marker.end())) {
        return error ? error : std::make_error_code(std::errc::bad_message);
      }
    }
  }
  std::vector<std::shared_ptr<const HostStorageRoot>> private_roots;
  if (ticket.binding_root_) {
    // Fully authenticated B2 state directory is purpose-private as a whole.
    private_roots = {ticket.binding_root_, ticket.private_parent_};
  } else {
    // A primary/explicit legacy parent may itself contain ordinary Content.
    // Exclude only the validated existing purpose roots, never that parent.
    private_roots = {ticket.recovery_->root(), profile, mutation};
    if (current_profile) { private_roots.push_back(current_profile); }
  }
  const auto public_id = ticket.public_authority_->IntentRootIdentity(error);
  auto public_mount = error ? nullptr : ticket.public_authority_->MountedFilesystemRoot(error);
  if (!public_mount || error ||
      public_mount->IntentRootIdentity(error).directory == public_id.directory) {
    return error ? error : Denied();
  }
  ticket.public_authority_->SetFormatEligibility(true);
  const auto projection = ticket.content_->InstallBootstrapProjection(
      std::move(private_roots), {ticket.public_authority_},
      ticket.content_->format_eligible(), {}, nullptr, error);
  if (!projection || error) { return error ? error : Denied(); }
  // Exact proof and metadata-only pending seal join into this actual Handoff.
  // No GuestView or ready owner is produced until ordinary format recovery,
  // phase3 and the same typed native admission complete.
  ticket.projection_ = projection;
  handoff.observation_source = ticket.content_;
  handoff.observation_projection = projection;
  return {};
}
ManagedDeviceFormatReceipt ManagedVolumeAuthority::ExecuteFormat(
    ManagedVolumeFormatHandoff& ticket, const std::atomic<bool>* cancellation) {
  ManagedDeviceFormatReceipt receipt;
  if (xe::global_critical_region::is_held_by_current_thread()) {
    receipt.error = std::make_error_code(std::errc::operation_not_permitted);
    return receipt;
  }
  if (ticket.error_ || !ticket.identity_ || !ticket.content_) {
    receipt.error = ticket.error_ ? ticket.error_ : Denied(); return receipt;
  }
  if (!ticket.operation_ && ticket.old_public_view_ &&
      !ticket.old_public_view_->retiring()) {
    receipt.error = std::make_error_code(std::errc::device_or_resource_busy);
    ticket.error_ = receipt.error; return receipt;
  }
  if (!ticket.operation_ &&
      (ticket.canceled_.load(std::memory_order_acquire) ||
       (cancellation && cancellation->load(std::memory_order_acquire)))) {
    receipt.error = std::make_error_code(std::errc::operation_canceled);
    ticket.error_ = receipt.error; return receipt;
  }
  std::error_code binding_error;
  const auto observed_binding = FormatTicketDigest(ticket, binding_error);
  if (binding_error || observed_binding != ticket.binding_) {
    receipt.error = binding_error ? binding_error
        : std::make_error_code(std::errc::state_not_recoverable);
    ticket.error_ = receipt.error; return receipt;
  }
  if (!ticket.operation_) {
    // Real source exclusion is acquired here, after coordinator drops all old
    // MVA/intents holders. No weak-owner observation claims drain.
    ticket.operation_ = PrepareFormatHandoff(ticket, cancellation);
  }
  receipt = ManagedDeviceIdentity::ExecuteHandoff(*ticket.operation_);
  ticket.error_ = receipt.error;
  // Private new-epoch creator must not escape before actual owners and both
  // durable handoff phases exist. FromCompletedFormat is the sole exposure.
  receipt.current.reset();
  return receipt;
}
std::unique_ptr<ManagedDeviceIdentity::Handoff>
ManagedVolumeAuthority::PrepareFormatHandoff(
    ManagedVolumeFormatHandoff& ticket, const std::atomic<bool>* external) {
  return ManagedDeviceIdentity::PrepareHandoff(
      ticket.identity_, ticket.binding_, &ticket.canceled_, external);
}
std::array<uint8_t, 32> ManagedVolumeAuthority::FormatTicketDigest(
    const ManagedVolumeFormatHandoff& ticket, std::error_code& error) {
  ManagedVolumeAuthority view;
  view.identity_ = ticket.operation_ ? ticket.operation_->identity : ticket.identity_;
  view.content_ = view.identity_
      ? std::const_pointer_cast<HostStorageRoot>(view.identity_->source()) : nullptr;
  view.private_parent_ = ticket.private_parent_;
  view.public_authority_ = ticket.public_authority_;
  view.state_parent_ = ticket.state_parent_;
  view.binding_root_ = ticket.binding_root_;
  view.recovery_ = ticket.recovery_;
  return FormatBindingDigest(view, error);
}
std::shared_ptr<ManagedVolumeAuthority> ManagedVolumeAuthority::BuildFormatVolume(
    ManagedVolumeFormatHandoff& ticket, bool completed) {
  auto result = std::shared_ptr<ManagedVolumeAuthority>(new ManagedVolumeAuthority);
  auto& error = result->error_;
  if (xe::global_critical_region::is_held_by_current_thread()) {
    error = std::make_error_code(std::errc::operation_not_permitted); return result;
  }
  if (!ticket.identity_ || (completed && (!ticket.operation_ ||
      !ticket.operation_->creator_ready || ticket.operation_->error))) {
    error = Denied(); return result;
  }
  if (!completed && ticket.operation_ && ticket.operation_->phase) {
    error = std::make_error_code(std::errc::operation_in_progress); return result;
  }
  if (!completed && (!ticket.old_public_view_ ||
                     !ticket.old_public_view_->retiring())) {
    error = std::make_error_code(std::errc::device_or_resource_busy); return result;
  }
  result->identity_ = completed ? ticket.operation_->identity : ticket.identity_;
  error = result->identity_->CheckCurrent();
  if (error) { return result; }
  result->content_ = std::const_pointer_cast<HostStorageRoot>(
      result->identity_->source());
  result->private_parent_ = ticket.private_parent_;
  result->public_authority_ = ticket.public_authority_;
  result->state_parent_ = ticket.state_parent_;
  result->binding_root_ = ticket.binding_root_;
  result->recovery_ = ticket.recovery_;
  result->projection_ = completed && ticket.operation_->projection
                           ? ticket.operation_->projection : ticket.projection_;
  const auto current_binding = FormatBindingDigest(*result, error);
  if (error || current_binding != ticket.binding_) {
    if (!error) { error = std::make_error_code(std::errc::state_not_recoverable); }
    return result;
  }
  std::shared_ptr<const ProfileIntentOwner> previous;
  if (completed) {
    result->source_lease_ = ticket.operation_->source_lease;
  } else {
    // This is restoration only, not drain admission. A genuine existing
    // intent owner may share its unchanged-epoch source lease after refusal.
    previous = ticket.previous_intents_.lock();
    if (previous && (!previous->error() &&
        previous->managed_identity() == result->identity_ &&
        previous->source() == result->content_ &&
        previous->private_parent() == result->private_parent_)) {
      result->source_lease_ = previous->source_lease_;
    } else {
      previous.reset();
      if (ticket.operation_ && ticket.operation_->source_lease) {
        result->source_lease_ = ticket.operation_->source_lease;
      } else {
        result->source_lease_ = result->content_->AcquireIntentLease(error, true);
      }
    }
  }
  if (!result->source_lease_ || error ||
      !result->content_->IntentLeaseMatches(*result->source_lease_)) {
    if (!error) { error = Denied(); } return result;
  }
  error = result->recovery_->CheckTarget(*result->content_);
  if (error) { return result; }
  result->public_root_ = result->public_authority_->IndependentNamespaceView(error);
  if (!result->public_root_) { return result; }
  if (result->projection_) {
    error = result->public_root_->AttachRetainedPublicProjection(
        *result->content_, *result->projection_);
    if (error) { return result; }
  }
  // The projection caches a pure native scope. Failures/ClosePublishedView
  // may retire only this disposable child, never that retained scope itself.
  result->public_root_=result->public_root_->Subroot({},false,error);
  if (!result->public_root_) {return result;}
  if (!completed) {
    result->public_root_ = result->public_root_->GuestView(error);
    if (!result->public_root_) { return result; }
  }
  if (previous) {
    result->intents_ = std::move(previous);
  } else {
    // Later durable phases certify that owners already existed. They can be
    // validated/reopened, never recreated from marker bytes alone.
    const bool create = completed && ticket.operation_->phase == 1;
    std::unique_ptr<ManagedBootstrapCreationPermit> format_creation;
    if (create && result->identity_->binding_required()) {
      format_creation.reset(new ManagedBootstrapCreationPermit(
          ManagedBootstrapCreationPermit::Purpose::kFormatProfile,
          result->identity_,result->private_parent_,result->source_lease_));
    }
    result->intents_ = ProfileIntentOwner::OpenInternal(
        result->private_parent_, result->identity_, create, result->source_lease_,format_creation.get());
  }
  if (!result->intents_ || result->intents_->error()) {
    error = result->intents_ ? result->intents_->error() : Denied();
  }
  if (!error) {
    auto previous_mutations = completed ? nullptr : ticket.previous_mutations_.lock();
    if (previous_mutations && previous_mutations->identity_ == result->identity_ &&
        previous_mutations->content_ == result->content_ &&
        previous_mutations->parent_ == result->private_parent_) {
      result->mutations_ = std::move(previous_mutations);
    } else {
      // Format preserves private mutation history. Never invent a new owner
      // during later recovery phases or replay an unfinished older epoch.
      result->mutations_ = ContentMutationOwner::OpenInternal(
          result->content_, result->identity_, result->private_parent_,
          result->recovery_, result->source_lease_, false);
    }
    if (!result->mutations_ || !result->mutations_->ready()) {
      error = result->mutations_ ? result->mutations_->error() : Denied();
      if (!error) { error = std::make_error_code(std::errc::operation_in_progress); }
    }
  }
  return result;
}
std::shared_ptr<const ManagedVolumeAuthority>
ManagedVolumeAuthority::RestoreUnchangedFormat(ManagedVolumeFormatHandoff& ticket) {
  // Caller has not published Prepared; all data/identity bytes must still be
  // the original epoch. Restoration makes new views, never reactivates old.
  ticket.partial_.reset();
  auto restored = BuildFormatVolume(ticket, false);
  ticket.error_ = restored->error_;
  if (ticket.error_) {
    if (restored->public_root_) { restored->public_root_->Retire(); }
    ticket.partial_ = restored;
  } else {
    ticket.operation_.reset();
    ticket.previous_intents_ = restored->intents_;
    ticket.previous_mutations_ = restored->mutations_;
  }
  return restored;
}
std::shared_ptr<const ManagedVolumeAuthority>
ManagedVolumeAuthority::FromCompletedFormat(ManagedVolumeFormatHandoff& ticket) {
  return FromCompletedFormatInternal(ticket,0);
}
std::shared_ptr<const ManagedVolumeAuthority>
ManagedVolumeAuthority::FromCompletedFormatInternal(
    ManagedVolumeFormatHandoff& ticket,unsigned stop_after_admission) {
  auto result = BuildFormatVolume(ticket, true);
  if (!result->error_) {
    if (ticket.operation_->phase == 1) {
      result->error_ = ManagedDeviceIdentity::AdvanceHandoff(*ticket.operation_,
                                                            false);
    }
    if (!result->error_ && ticket.operation_->phase == 2) {
      result->error_ = ManagedDeviceIdentity::AdvanceHandoff(*ticket.operation_,
                                                            true);
    }
    if (!result->error_ && ticket.operation_->phase != 3) {
      result->error_ = std::make_error_code(std::errc::state_not_recoverable);
    }
  }
  if (!result->error_) {
    // Native owners and actual Recover precede both durable phase3 admission
    // and the first published guest. The permit cannot be made from a bool or
    // marker tag, and retires only this Handoff's exact inspection seal.
    ManagedFormatGuestAdmissionPermit admission(*ticket.operation_,result->mutations_);
    result->error_=result->content_->AdmitFormatProjection(admission);
    if (!result->error_ && stop_after_admission==1) {
      result->error_=std::make_error_code(std::errc::operation_canceled);
    }
    if (!result->error_) {
      result->public_root_=result->public_root_->GuestView(result->error_);
      if (!result->public_root_ && !result->error_) {result->error_=Denied();}
    }
  }
  ticket.error_ = result->error_;
  if (ticket.error_) {
    if (result->public_root_) { result->public_root_->Retire(); }
    ticket.partial_ = result;
  } else {
    ticket.partial_.reset();
    ticket.previous_intents_ = result->intents_;
    ticket.previous_mutations_ = result->mutations_;
    ticket.identity_ = result->identity_;
    ticket.content_ = result->content_;
    ticket.projection_ = result->projection_;
    // The ready bundle now owns the same source lease. Completed operation
    // metadata does not keep an extra format reservation alive indefinitely.
    ticket.operation_.reset();
  }
  return result;
}
std::shared_ptr<const ManagedVolumeAuthority> ManagedVolumeAuthority::ResumeFormat(
    ManagedVolumeFormatHandoff& ticket) {
  if (!ticket.operation_ ||
      xe::global_critical_region::is_held_by_current_thread()) {
    auto failed = std::shared_ptr<ManagedVolumeAuthority>(new ManagedVolumeAuthority);
    failed->error_ = std::make_error_code(std::errc::operation_not_permitted);
    return failed;
  }
  if (ticket.partial_) {
    if (ticket.partial_->public_root_) { ticket.partial_->public_root_->Retire(); }
    ticket.partial_.reset();  // Native/private lease destruction outside locks.
  }
  ticket.error_ = ManagedDeviceIdentity::ReopenHandoffForResume(*ticket.operation_);
  if (!ticket.error_) {
    std::error_code error;
    const auto observed = FormatTicketDigest(ticket, error);
    if (error || observed != ticket.binding_) {
      ticket.error_ = error ? error
          : std::make_error_code(std::errc::state_not_recoverable);
    }
  }
  if (!ticket.error_) {
    const auto receipt = ExecuteFormat(ticket);
    ticket.error_ = receipt.error;
  }
  if (!ticket.error_) { return FromCompletedFormat(ticket); }
  auto failed = std::shared_ptr<ManagedVolumeAuthority>(new ManagedVolumeAuthority);
  failed->error_ = ticket.error_;
  return failed;
}
bool ManagedVolumeAuthority::profile_writable() const {
  return !error_ && identity_ && intents_ && recovery_ && mutations_ &&
         mutations_->ready() &&
         !identity_->retiring() && !intents_->error() && !recovery_->error();
}
bool ManagedVolumeAuthority::format_eligible() const {
  return profile_writable() && mutations_->idle() && content_->format_eligible();
}
}  // namespace xe::filesystem
