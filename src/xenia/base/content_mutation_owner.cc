/**
 * Xenia : Xbox 360 Emulator Research Project
 * Released under the BSD license - see LICENSE in the root for details.
 */
#include "xenia/base/snapshot_publication_set.h"
#include "xenia/base/content_mutation_owner.h"
#include <algorithm>
#include <array>
#include <new>
#include <random>
#include <set>
#include "xenia/base/mutex.h"
#include "xenia/base/managed_storage_bootstrap.h"
namespace xe::filesystem {
namespace {
namespace wire = content_mutation_wire;
namespace package_wire = content_package_mutation_wire;
std::string UuidText(const ManagedDeviceIdentity::Uuid& uuid) {
  constexpr char hex[] = "0123456789abcdef";
  std::string text;
  for (auto byte : uuid) { text += hex[byte >> 4]; text += hex[byte & 15]; }
  return text;
}
std::error_code Error(std::errc value) { return std::make_error_code(value); }
bool Equal(std::span<const uint8_t> a, std::span<const uint8_t> b) {
  return std::equal(a.begin(), a.end(), b.begin(), b.end());
}
std::string Nonce() {
  constexpr char hex[] = "0123456789abcdef";
  std::random_device random;
  std::string out;
  for (unsigned i = 0; i != 4; ++i) {
    const uint32_t value = random();
    for (unsigned j = 0; j != 8; ++j) { out += hex[(value >> (4*j)) & 15]; }
  }
  return out;
}
void Number(std::vector<uint8_t>& out, uint64_t n) {
  for (int shift = 56; shift >= 0; shift -= 8) { out.push_back(uint8_t(n >> shift)); }
}
std::filesystem::path Journal(std::string_view nonce, bool done = false) {
  return "content-" + std::string(nonce) + (done ? ".done" : ".intent");
}
std::filesystem::path PreparationJournal(std::string_view nonce) {
  return "content-" + std::string(nonce) + ".plan";
}
std::filesystem::path AbortJournal(std::string_view nonce) {
  return "content-" + std::string(nonce) + ".abort";
}
std::filesystem::path PreservationJournal(std::string_view nonce, bool header) {
  return "content-" + std::string(nonce) + (header ? ".keep1" : ".keep0");
}
bool HasPreparation(const wire::Record& r) {
  return std::any_of(r.preparation_digest.begin(),r.preparation_digest.end(),[](uint8_t b) { return b != 0; });
}
std::filesystem::path StreamJournal(std::string_view nonce) {
  return "content-" + std::string(nonce) + ".stream";
}
bool SameStreamAuthority(const wire::Record& admitted, const wire::Record& complete) {
  if (!admitted.streamed || !admitted.stream_pending || !complete.streamed || complete.stream_pending ||
      admitted.identity != complete.identity || admitted.nonce != complete.nonce ||
      admitted.package != complete.package || admitted.target != complete.target ||
      admitted.preparation_digest != complete.preparation_digest ||
      admitted.stage != complete.stage || admitted.stage_file != complete.stage_file ||
      admitted.directory_stage != complete.directory_stage || admitted.intended != complete.intended ||
      admitted.initial_tree.size() != complete.initial_tree.size()) { return false; }
  std::map<std::filesystem::path, const ContentPackageInitialEntry*> rows;
  for (const auto& row : admitted.initial_tree) { rows.emplace(row.entry.relative, &row); }
  for (const auto& row : complete.initial_tree) {
    const auto found = rows.find(row.entry.relative);
    if (found == rows.end()) { return false; }
    const auto& expected = found->second->entry;
    if (row.entry.file != expected.file || row.entry.mode != expected.mode ||
        row.entry.directory != expected.directory ||
        (!row.entry.directory && row.entry.bytes != expected.bytes)) { return false; }
  }
  return true;
}
struct Active {
  std::atomic<uint64_t>& value;
  explicit Active(std::atomic<uint64_t>& v) : value(v) { value.fetch_add(1, std::memory_order_acq_rel); }
  ~Active() { value.fetch_sub(1, std::memory_order_acq_rel); }
};
}
std::vector<uint8_t> ContentMutationOwner::BootstrapMarker(
    const ManagedDeviceIdentity& identity, const HostStorageRoot& root,
    const HostStorageRoot& records, const HostStorageRoot& staging,
    std::error_code& error) {
  error.clear();
  if (xe::global_critical_region::is_held_by_current_thread()) {
    error = Error(std::errc::operation_not_permitted); return {};
  }
  // Byte-for-byte XCMOWN1 producer, shared by the ordinary factory and the
  // native component initializer. Native IDs describe actual retained roots;
  // the resulting detached bytes confer no creation/publication authority.
  std::vector<uint8_t> marker{'X','C','M','O','W','N','1','\n'};
  marker.insert(marker.end(), identity.uuid().begin(), identity.uuid().end());
  Number(marker, identity.directory_file_id());
  Number(marker, identity.device_id());
  const auto& source = identity.source_id();
  Number(marker, source.size()); marker.insert(marker.end(), source.begin(), source.end());
  for (const auto* owned : {&root, &records, &staging}) {
    const auto id = owned->IntentRootIdentity(error);
    if (error) { return {}; } Number(marker, id.directory);
  }
  const auto digest = wire::Digest(marker, error);
  if (error) { return {}; }
  marker.insert(marker.end(), digest.begin(), digest.end());
  return marker;
}
std::error_code ContentMutationOwner::CheckCurrent() const {
  if (xe::global_critical_region::is_held_by_current_thread()) {
    return Error(std::errc::operation_not_permitted);
  }
  if (error_) { return error_; }
  if (!identity_ || !content_ || !records_ || !staging_ || !source_lease_ ||
      identity_->source().get() != content_.get() ||
      !content_->IntentLeaseMatches(*source_lease_)) {
    return Error(std::errc::permission_denied);
  }
  auto error = identity_->CheckCurrent();
  if (!error) { error = recovery_->CheckTarget(*content_); }
  return error;
}
std::shared_ptr<ContentMutationOwner> ContentMutationOwner::OpenInternal(
    std::shared_ptr<HostStorageRoot> content,
    std::shared_ptr<const ManagedDeviceIdentity> identity,
    std::shared_ptr<HostStorageRoot> parent,
    std::shared_ptr<const SnapshotRecoveryOwner> recovery,
    std::shared_ptr<const HostStorageIntentLease> source_lease,
    bool initialize, ManagedBootstrapCreationPermit* creation) {
  auto out = std::shared_ptr<ContentMutationOwner>(new ContentMutationOwner);
  out->content_ = std::move(content); out->identity_ = std::move(identity);
  out->parent_ = std::move(parent); out->recovery_ = std::move(recovery);
  out->source_lease_ = std::move(source_lease);
  auto& error = out->error_;
  if (xe::global_critical_region::is_held_by_current_thread() ||
      !out->content_ || !out->identity_ || !out->parent_ || !out->recovery_ ||
      out->identity_->source().get() != out->content_.get() ||
      !out->source_lease_ ||
      !out->content_->IntentLeaseMatches(*out->source_lease_)) {
    error = Error(std::errc::permission_denied); return out;
  }
  if (creation) {
    error = creation->Consume(ManagedBootstrapCreationPermit::Purpose::kContentMutation,
                              *out->identity_, *out->parent_, *out->source_lease_);
    if (!error) { error = out->identity_->CheckBootstrapCurrent(*creation); }
    // The capsule producer publishes both complete component and link before
    // opening this owner. A permit never enables this legacy staging path.
    initialize = false;
  } else {
    error = out->identity_->CheckCurrent();
    if (!error && out->identity_->binding_required()) {
      error = ManagedStorageBootstrap::ValidateOwnerParent(*out->identity_, *out->parent_);
      initialize = false;
    }
  }
  if (!error) { error = out->recovery_->CheckTarget(*out->content_); }
  if (error) { return out; }
  out->wire_identity_ = {out->identity_->uuid(), out->identity_->content_epoch(),
      out->identity_->directory_file_id(), out->identity_->device_id(),
      out->identity_->source_id()};
  const auto key = UuidText(out->identity_->uuid());
  const auto directory = ".xenia-content-mutations-v1-" + key;
  const auto link_name = "content-mutations-v1-" + key + ".owner";
  auto link = out->parent_->CaptureSnapshot(link_name, 4096, error);
  if (!link) { return out; }
  auto root = out->parent_->Subroot(directory, false, error);
  bool created = false;
  if (!root && error == std::errc::no_such_file_or_directory &&
      !link->exists() && initialize) {
    error = out->parent_->Create(directory, true);
    if (error) { return out; }
    created = true;
    root = out->parent_->Subroot(directory, false, error);
  }
  if (!root || (created == link->exists())) {
    if (!error) { error = Error(std::errc::state_not_recoverable); }
    return out;
  }
  // Never adopt a partial existing directory merely because its link is gone.
  if (!created && !link->exists()) {
    error = Error(std::errc::state_not_recoverable); return out;
  }
  if (creation) {
    // Preserve the producer's exact canonical native component capability;
    // the consumed permit proves it against this ordinary named resolution.
    root=creation->ComponentRoot(root,error);
    if (!root || error) { return out; }
  }
  error = root->PrivateDirectory();
  if (!error) { error = root->IntentDisjointFrom(*out->content_); }
  if (error) { return out; }
  for (const auto* leaf : {"records", "staging"}) {
    if (created) { error = root->Create(leaf, true); if (error) { return out; } }
    auto child = root->Subroot(leaf, false, error);
    if (!child) { return out; }
    error = child->PrivateDirectory();
    if (error) { return out; }
    if (std::string_view(leaf) == "records") {
      out->records_ = child->ProtectContentMutations(error);
    } else {
      // This actual private child is a native construction namespace, not a
      // guest-relative view of Content. Keep its exact retained dependency,
      // identity, readonly policy and retirement chain, while source guest
      // exclusions continue to protect it through the shared native registry.
      auto native_stage = child->IndependentNamespaceView(error);
      // Native staging remains private while its operation still depends on
      // the exact live content generation, including after physical anchoring.
      out->staging_ = native_stage ? native_stage->WithDependency(out->content_, error)
                                  : nullptr;
      if (!out->staging_ && !error) {
        error = Error(std::errc::state_not_recoverable);
      }
    }
    if (error) { return out; }
  }
  const auto marker = BootstrapMarker(*out->identity_, *root,
                                       *out->records_, *out->staging_, error);
  if (error) { return out; }
  if (created) {
    const auto written = out->records_->WriteNewFile("owner.v1", marker);
    error = written.error;
    if (!error && written.bytes != marker.size()) { error = Error(std::errc::io_error); }
    if (!error) { error = root->FlushDirectory(); }
    if (!error) { error = out->parent_->FlushDirectory(); }
    if (!error) {
      // External link is the final initialization commit. Missing committed
      // children refuse noncreating; failed creation stays private/uncertain.
      const auto linked = out->parent_->WriteNewFile(link_name, marker);
      error = linked.error;
      if (!error && linked.bytes != marker.size()) { error = Error(std::errc::io_error); }
    }
  } else if (!Equal(link->bytes(), marker)) {
    error = Error(std::errc::state_not_recoverable);
  }
  if (error) { return out; }
  const auto actual = out->records_->CaptureSnapshot("owner.v1", 4096, error);
  if (!error && (!actual || !actual->exists() || !Equal(actual->bytes(), marker))) {
    error = Error(std::errc::state_not_recoverable);
  }
  // A pending owner is a private retained initializer result, never a ready
  // catalog authority. The coordinator commits its durable binding under the
  // same source exclusion, then calls ordinary Recover before publication.
  // Operational CheckCurrent remains strict; no token is retained or replayed.
  if (!error && !creation) { error = out->RecoverLocked(); }
  out->blocked_.store(creation || bool(error), std::memory_order_release);
  return out;
}
bool ContentMutationOwner::Visible(const std::filesystem::path& package) const {
  return ready() && content_->ContentNamespaceReady(package);
}
std::unique_ptr<HostStorageContentTree> ContentMutationOwner::CapturePackage(
    const std::filesystem::path& package, std::error_code& error,
    std::shared_ptr<const HostStorageRoot> admission_root) const {
  if (admission_root && (admission_root->retiring() ||
      !content_ || admission_root->path() != content_->path())) {
    error = Error(std::errc::no_such_device); return {};
  }
  if (!wire::ValidPackage(package) || !Visible(package)) {
    error = Error(std::errc::permission_denied); return {};
  }
  error = CheckCurrent();
  if (error) { return {}; }
  auto captured = content_->CaptureContentTree(package, error);
  if (captured) { captured->BindIdentity(identity_, source_lease_, std::move(admission_root)); }
  return captured;
}
std::shared_ptr<const HostStorageSnapshotToken> ContentMutationOwner::CaptureMetadata(
    Kind kind, const std::filesystem::path& package,
    const std::filesystem::path& exact, std::error_code& error) const {
  wire::Record slot; slot.kind = kind; slot.package = package; slot.target = exact;
  if (!ready() || !wire::ValidPackage(package) || wire::Target(slot).empty()) {
    error = Error(std::errc::permission_denied); return {};
  }
  error = CheckCurrent();
  if (!error) { error = content_->CheckContentPathCase(package); }
  if (!error) { error = content_->CheckContentPathCase(exact); }
  if (error) { return {}; }
  auto token = content_->CaptureSnapshot(exact, wire::kMaximumMetadataBytes, error, true);
  if (!token && error == std::errc::no_such_file_or_directory &&
      (kind == Kind::kHeader || kind == Kind::kCreate)) {
    error = EnsureParents(exact.parent_path());
    if (!error) {
      token = content_->CaptureSnapshot(exact, wire::kMaximumMetadataBytes, error, true);
    }
  }
  return token;
}
std::error_code ContentMutationOwner::EnsureParents(
    const std::filesystem::path& relative) const {
  std::error_code error;
  std::filesystem::path current;
  for (const auto& part : relative) {
    current /= part;
    auto root = content_->Subroot(current, false, error);
    if (!root && error == std::errc::no_such_file_or_directory) {
      error = content_->Create(current, true);
      if (error && error != std::errc::file_exists) { return error; }
      root = content_->Subroot(current, false, error);
      if (root) { error = content_->FlushDirectory(current.parent_path()); }
    }
    if (!root || error) { return error; }
  }
  return {};
}
std::error_code ContentMutationOwner::AppendRecord(
    const std::filesystem::path& path, std::span<const uint8_t> bytes) {
  std::string temporary;
  for (unsigned i = 0; i != 8 && !wire::ValidNonce(temporary); ++i) { temporary = Nonce(); }
  if (!wire::ValidNonce(temporary)) { return Error(std::errc::io_error); }
  unsigned stop = 0;
  if (!record_stop_suffix_.empty() && path.string().ends_with(record_stop_suffix_)) {
    stop = record_stop_boundary_; record_stop_suffix_.clear(); record_stop_boundary_ = 0;
  }
  return records_->AppendContentRecord(path,bytes,temporary,stop);
}
std::error_code ContentMutationOwner::BeginPreparation(wire::Record& record,
    const HostStorageSnapshotToken& before, std::span<const uint8_t> header, std::vector<uint8_t>& encoded) {
  const auto peer = content_->CheckContentPreparationPeer(*staging_);
  if (peer) { return peer; }
  wire::Preparation plan{record.identity,record.nonce,record.package,record.target,
                          uint64_t(header.size()),{}};
  std::error_code error;
  plan.header_digest = wire::Digest(header,error);
  if (!error && before.exists()) {
    plan.before_file = before.info().file_id; plan.before_bytes = before.bytes().size();
    plan.before_digest = wire::Digest(before.bytes(),error);
  }
  if (!error) { encoded = wire::EncodePreparation(plan,error); }
  if (!error) { error = AppendRecord(PreparationJournal(plan.nonce),encoded); }
  if (!error) {
    record.preparation_digest = wire::Digest(encoded,error);
    record.stage = wire::PreparationStage(plan);
  }
  return error;
}
std::vector<std::filesystem::path> ContentMutationOwner::PreservedArtifacts() const {
  std::lock_guard operation(operation_mutex_);
  return preserved_artifacts_;
}
std::error_code ContentMutationOwner::PreserveArtifact(wire::Preservation ticket,
    const std::shared_ptr<HostStorageRoot>& source, unsigned stop_boundary) {
  std::error_code error;
  const auto space = source->Observe();
  if (space.error || !space.available || space.read_only ||
      std::min(space.usable_bytes,preparation_space_ceiling_.value_or(UINT64_MAX)) < (1u << 16)) {
    return space.error ? space.error : Error(space.read_only ? std::errc::read_only_file_system : std::errc::no_space_on_device);
  }
  const auto leaf = PreservationJournal(ticket.nonce,ticket.header);
  const auto existing = records_->CaptureSnapshot(leaf,4096,error);
  if (!existing) { return error; }
  if (existing->exists()) {
    wire::Preservation prior;
    if (!wire::DecodePreservation(existing->bytes(),prior,error) ||
        prior.identity != ticket.identity || prior.nonce != ticket.nonce ||
        prior.header != ticket.header || prior.source != ticket.source ||
        prior.preparation_digest != ticket.preparation_digest ||
        (ticket.file && (prior.file != ticket.file || prior.directory != ticket.directory))) {
      return error ? error : Error(std::errc::state_not_recoverable);
    }
    ticket = std::move(prior);
  } else {
    const auto observed = source->ObservePreparationArtifact(ticket.source,error);
    if (error) { return error; }
    if (!observed) { return {}; }
    if (observed->directory != !ticket.header || (!observed->directory && !observed->regular)) {
      return Error(std::errc::state_not_recoverable);
    }
    ticket.file = observed->file_id; ticket.directory = observed->directory;
    const auto encoded = wire::EncodePreservation(ticket,error);
    if (!error) { error = AppendRecord(leaf,encoded); }
    if (error) { return error; }
  }
  if (stop_boundary == 1) { return Error(std::errc::operation_canceled); }
  const auto destination = wire::PreservationDestination(ticket);
  error = source->PreservePreparationArtifact(ticket.source,ticket.file,ticket.directory,
                                               *staging_,destination);
  if (!error) {
    const auto path = staging_->path()/destination;
    if (std::find(preserved_artifacts_.begin(),preserved_artifacts_.end(),path) == preserved_artifacts_.end()) {
      preserved_artifacts_.push_back(path);
    }
  }
  if (!error && stop_boundary == 2) { return Error(std::errc::operation_canceled); }
  return error;
}
std::error_code ContentMutationOwner::CheckPreparationBefore(const wire::Preparation& plan,
    const HostStorageSnapshotToken& before) const {
  if (before.exists() != bool(plan.before_file)) { return Error(std::errc::state_not_recoverable); }
  std::error_code error;
  if (before.exists() && (before.info().file_id != plan.before_file ||
      before.bytes().size() != plan.before_bytes || wire::Digest(before.bytes(),error) != plan.before_digest)) {
    return error ? error : Error(std::errc::state_not_recoverable);
  }
  return error;
}
std::error_code ContentMutationOwner::ObservePreparationBackup(const wire::Preparation& plan) {
  if (!plan.before_file) { return {}; }
  // SnapshotPublisher may have created an incomplete backup before the full
  // intent exists. Plan-only abort keeps the exact semantic Header untouched;
  // the named backup is only retained opaque evidence, never restore authority.
  const auto relative = "snapshot-"+plan.nonce+".bin";
  std::error_code error;
  const auto token = recovery_->root()->CaptureSnapshot(relative,wire::kMaximumMetadataBytes,error);
  if (!token) { return error; }
  if (!token->exists()) { return {}; }
  const auto observed = recovery_->root()->ObserveIntentSnapshot(*token);
  if (observed.error || !observed.durable) { return observed.error ? observed.error : Error(std::errc::io_error); }
  const auto path = recovery_->root()->path()/relative;
  if (std::find(preserved_artifacts_.begin(),preserved_artifacts_.end(),path) == preserved_artifacts_.end()) {
    preserved_artifacts_.push_back(path);
  }
  return {};
}
std::error_code ContentMutationOwner::AbortPreparation(const wire::Preparation& plan,
    std::span<const uint8_t> encoded, const std::shared_ptr<HostStorageRoot>& reserved,
    const std::shared_ptr<HostStorageRoot>& package_reserved, unsigned stop_boundary) {
  std::error_code error;
  if (plan.identity != wire_identity_) { return Error(std::errc::state_not_recoverable); }
  // A canonical promotion always wins, including a rename whose final barrier
  // failed. The preparation-only path cannot roll back a publication duty.
  for (const auto& leaf : {Journal(plan.nonce),StreamJournal(plan.nonce)}) {
    const auto record = records_->CaptureSnapshot(leaf,
        wire::kMaximumRecordBytes,error);
    if (!record) { return error; }
    if (record->exists()) { return Error(std::errc::operation_in_progress); }
  }
  auto target = reserved->CaptureSnapshot(plan.target,wire::kMaximumMetadataBytes,error,true);
  if (!target) { return error; }
  error = CheckPreparationBefore(plan,*target);
  if (error) { return error; }
  const auto package = package_reserved->ObservePreparationArtifact(plan.package,error);
  if (error || package) { return error ? error : Error(std::errc::state_not_recoverable); }
  const auto digest = wire::Digest(encoded,error);
  wire::Preservation directory{plan.identity,plan.nonce,false,0,true,
                               "package-"+plan.nonce+".tmp",digest};
  if (!error) { error = PreserveArtifact(directory,staging_,stop_boundary); }
  wire::Preservation header{plan.identity,plan.nonce,true,0,false,
                            wire::PreparationStage(plan),digest};
  if (!error) { error = PreserveArtifact(header,reserved,stop_boundary); }
  if (!error) { error = ObservePreparationBackup(plan); }
  const auto terminal = error ? std::vector<uint8_t>{} : wire::Done(encoded,error);
  if (!error) { error = AppendRecord(AbortJournal(plan.nonce),terminal); }
  if (!error) {
    reserved->ReleaseContentNamespace(); package_reserved->ReleaseContentNamespace();
    pending_reservations_.erase(plan.target); pending_reservations_.erase(plan.package);
  }
  return error;
}
std::error_code ContentMutationOwner::RecoverPreparations(
    const std::vector<std::filesystem::path>& names,
    const std::set<std::filesystem::path>& present,
    std::map<std::filesystem::path,uint64_t>& known) {
  std::error_code error;
  std::map<std::string,std::pair<wire::Preparation,std::vector<uint8_t>>> plans;
  auto same_source = [&](const wire::Identity& identity) {
    return identity.uuid == wire_identity_.uuid && identity.source == wire_identity_.source &&
        identity.device == wire_identity_.device && identity.content_directory == wire_identity_.content_directory &&
        identity.epoch <= wire_identity_.epoch;
  };
  for (const auto& name : names) {
    const auto text = name.string();
    if (text.find(".partial-") != std::string::npos) {
      preserved_artifacts_.push_back(records_->path()/name); continue;
    }
    if (!text.ends_with(".plan")) { continue; }
    const auto token = records_->CaptureSnapshot(name,4096,error);
    wire::Preparation plan;
    if (!token || !wire::DecodePreparation(token->bytes(),plan,error)) { return error; }
    if (name != PreparationJournal(plan.nonce) || !same_source(plan.identity)) { return Error(std::errc::state_not_recoverable); }
    plans.emplace(plan.nonce,std::make_pair(plan,std::vector<uint8_t>(token->bytes().begin(),token->bytes().end())));
  }
  // Before any preparation-only preservation, verify an orphan semantic
  // Header still matches the exact before snapshot. Completed aborts may be
  // followed by a legitimate later package/update and must not freeze it.
  for (const auto& [nonce,pair] : plans) {
    if (present.contains(Journal(nonce)) || present.contains(StreamJournal(nonce)) ||
        present.contains(AbortJournal(nonce))) { continue; }
    const auto& plan = pair.first;
    auto view = content_;
    if (const auto old = pending_reservations_.find(plan.target);
        old != pending_reservations_.end() && old->second.nonce == nonce) { view = old->second.view; }
    const auto before = view->CaptureSnapshot(plan.target,wire::kMaximumMetadataBytes,error,true);
    if (!before) { return error; }
    error = CheckPreparationBefore(plan,*before);
    if (error) { return error; }
  }
  // Validate canonical promotions BEFORE any opaque orphan preservation.
  for (const auto& name : names) {
    const auto text = name.string();
    if (text.find(".partial-") != std::string::npos ||
        (!text.ends_with(".stream") && !text.ends_with(".intent"))) { continue; }
    const auto token = records_->CaptureSnapshot(name,
        std::max(wire::kMaximumRecordBytes,package_wire::kMaximumRecordBytes),error);
    if (!token) { return error; }
    constexpr std::string_view magic = "XCPKG03\n";
    if (token->bytes().size() >= magic.size() && std::equal(magic.begin(),magic.end(),token->bytes().begin())) { continue; }
    wire::Record record;
    if (!wire::Decode(token->bytes(),record,error)) { return error; }
    const auto found = plans.find(record.nonce);
    if (HasPreparation(record)) {
      if (found == plans.end() || !wire::MatchesPreparation(found->second.first,found->second.second,record,error)) {
        return error ? error : Error(std::errc::state_not_recoverable);
      }
    } else if (found != plans.end()) { return Error(std::errc::state_not_recoverable); }
  }
  // Every preserved destination has one exact durable ticket, even after an
  // abort. Replaying a rename/barrier is independent from publication success.
  for (const auto& name : names) {
    const auto text = name.string();
    if (!text.ends_with(".keep0") && !text.ends_with(".keep1")) { continue; }
    const auto token = records_->CaptureSnapshot(name,4096,error);
    wire::Preservation ticket;
    if (!token || !wire::DecodePreservation(token->bytes(),ticket,error)) { return error; }
    if (name != PreservationJournal(ticket.nonce,ticket.header) || !same_source(ticket.identity)) {
      return Error(std::errc::state_not_recoverable);
    }
    const auto found = plans.find(ticket.nonce);
    if (present.contains(Journal(ticket.nonce)) || present.contains(StreamJournal(ticket.nonce))) {
      return Error(std::errc::state_not_recoverable);
    }
    if (ticket.header || std::any_of(ticket.preparation_digest.begin(),ticket.preparation_digest.end(),[](uint8_t b){return b != 0;})) {
      if (found == plans.end() || ticket.identity != found->second.first.identity ||
          ticket.preparation_digest != wire::Digest(found->second.second,error) ||
          (ticket.header && ticket.source != wire::PreparationStage(found->second.first))) {
        return error ? error : Error(std::errc::state_not_recoverable);
      }
    }
    auto source = ticket.header ? content_ : staging_;
    // Header namespace permits from an in-process failed operation must be
    // reused, never transiently released to acquire another one.
    if (ticket.header && found != plans.end()) {
      const auto old = pending_reservations_.find(found->second.first.target);
      if (old != pending_reservations_.end() && old->second.nonce == ticket.nonce) { source = old->second.view; }
    }
    error = PreserveArtifact(ticket,source);
    if (error) { return error; }
    known.emplace(wire::PreservationDestination(ticket),ticket.file);
  }
  for (const auto& [nonce,pair] : plans) {
    const auto& plan = pair.first; const auto& encoded = pair.second;
    if (present.contains(AbortJournal(nonce))) {
      if (present.contains(Journal(nonce)) || present.contains(StreamJournal(nonce))) { return Error(std::errc::state_not_recoverable); }
      const auto terminal = records_->CaptureSnapshot(AbortJournal(nonce),256,error);
      if (!terminal || !wire::CheckDone(terminal->bytes(),encoded,error)) { return error; }
      const auto observed = records_->ObserveIntentSnapshot(*terminal);
      if (observed.error || !observed.durable) { return observed.error ? observed.error : Error(std::errc::io_error); }
      error = ObservePreparationBackup(plan);
      if (error) { return error; }
      for (const auto& slot : {plan.target,plan.package}) {
        const auto old = pending_reservations_.find(slot);
        if (old != pending_reservations_.end() && old->second.nonce == nonce) {
          old->second.view->ReleaseContentNamespace(); pending_reservations_.erase(old);
        }
      }
      continue;
    }
    if (present.contains(Journal(nonce)) || present.contains(StreamJournal(nonce))) { continue; }
    if (plan.identity != wire_identity_) { return Error(std::errc::state_not_recoverable); }
    std::shared_ptr<HostStorageRoot> reserved, package_reserved;
    for (const auto& slot : {plan.target,plan.package}) {
      const auto old = pending_reservations_.find(slot);
      auto& view = slot == plan.target ? reserved : package_reserved;
      if (old != pending_reservations_.end() && old->second.nonce == nonce) { view = old->second.view; }
      if (!view) {
        std::optional<FileHandleInfo> expected;
        if (slot == plan.target) {
          const auto before = content_->CaptureSnapshot(plan.target,wire::kMaximumMetadataBytes,error,true);
          if (!before) { return error; }
          error = CheckPreparationBefore(plan,*before);
          if (error) { return error; }
          if (before->exists()) { expected=before->info(); }
        }
        view = content_->ReserveContentNamespace(slot,expected,slot == plan.package,error);
      }
      if (!view) { return error; }
      pending_reservations_[slot] = {nonce,view};
    }
    error = AbortPreparation(plan,encoded,reserved,package_reserved);
    if (error) { return error; }
    // Newly written tickets were not in the original inventory.
    for (const bool header : {false,true}) {
      const auto ticket = records_->CaptureSnapshot(PreservationJournal(nonce,header),4096,error);
      if (!ticket) { return error; }
      if (ticket->exists()) {
        wire::Preservation saved;
        if (!wire::DecodePreservation(ticket->bytes(),saved,error)) { return error; }
        known.emplace(wire::PreservationDestination(saved),saved.file);
      }
    }
  }
  for (const auto& name : names) {
    const auto text = name.string();
    if (text.ends_with(".abort") && !plans.contains(text.substr(8,32))) { return Error(std::errc::bad_message); }
  }
  return {};
}
struct ContentMutationOwner::ProfileThumbnailParticipant::Data {
  std::shared_ptr<ContentMutationOwner> owner;
  std::weak_ptr<SnapshotPublicationSet> members;
  std::shared_ptr<HostStorageRoot> reservation;
  std::shared_ptr<const HostStorageSnapshotToken> expected;
  bool active = false;
  ~Data() {
    if (reservation) {
      reservation->Retire();
      reservation->ReleaseRetiredSnapshotReservation();
    }
    if (active) { owner->active_.fetch_sub(1, std::memory_order_acq_rel); }
  }
};
ContentMutationOwner::ProfileThumbnailParticipant::ProfileThumbnailParticipant(
    std::shared_ptr<Data> data) : data_(std::move(data)) {}
ContentMutationOwner::ProfileThumbnailParticipant::~ProfileThumbnailParticipant() = default;
const std::shared_ptr<const HostStorageSnapshotToken>&
ContentMutationOwner::ProfileThumbnailParticipant::expected() const { return data_->expected; }
std::error_code ContentMutationOwner::ProfileThumbnailParticipant::CheckCurrent(
    const SnapshotPublicationSet& members) const {
  const auto held = data_->members.lock();
  if (!data_->active || !held || held.get() != &members || !data_->reservation ||
      data_->reservation->retiring()) { return Error(std::errc::operation_canceled); }
  return data_->owner->CheckCurrent();
}
std::shared_ptr<ContentMutationOwner::ProfileThumbnailParticipant>
ContentMutationOwner::PrepareProfileThumbnail(const std::filesystem::path& package,
    const std::shared_ptr<SnapshotPublicationSet>& members, std::error_code& error) {
  error.clear();
  if (global_critical_region::is_held_by_current_thread() || !members) {
    error = Error(std::errc::operation_not_permitted); return {};
  }
  // This join is only for the actual canonical local profile package, never
  // arbitrary content metadata. The store separately authenticates its XUID,
  // managed source binding and complete intended record before journal IO.
  std::vector<std::string> parts;
  for (const auto& part : package) { parts.push_back(part.generic_string()); }
  const auto fold = [](std::string value) {
    for (auto& c : value) { if (c >= 'a' && c <= 'z') { c -= 'a' - 'A'; } }
    return value;
  };
  const auto xuid_leaf = [](std::string_view value) {
    return value.size() == 16 && value != "0000000000000000" &&
        std::all_of(value.begin(), value.end(), [](char c) {
          return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
        });
  };
  if (!wire::ValidPackage(package) || parts.size() != 4 || !xuid_leaf(parts[0]) ||
      fold(parts[0]) != fold(parts[3]) || fold(parts[1]) != "FFFE07D1" ||
      parts[2] != "00010000") {
    error = Error(std::errc::invalid_argument); return {};
  }
  std::unique_lock operation(operation_mutex_, std::try_to_lock);
  if (!operation.owns_lock()) { error = Error(std::errc::device_or_resource_busy); return {}; }
  error = CheckCurrent();
  if (!error && (!ready() || active_.load(std::memory_order_acquire))) {
    error = Error(std::errc::device_or_resource_busy);
  }
  if (error) { return {}; }
  try {
    auto data = std::make_shared<ProfileThumbnailParticipant::Data>();
    data->owner = shared_from_this(); data->members = members;
    active_.fetch_add(1, std::memory_order_acq_rel); data->active = true;
    const auto exact = CaptureMetadata(Kind::kThumbnail, package,
                                        package / "__thumbnail.png", error);
    if (!exact) { return {}; }
    auto native_profile = content_->Subroot(package, false, error);
    if (!native_profile) { return {}; }
    auto before = native_profile->CaptureSnapshot("__thumbnail.png",
        wire::kMaximumMetadataBytes, error, true);
    if (!before) { return {}; }
    if (before->exists() != exact->exists() || before->bytes().size() != exact->bytes().size() ||
        !std::equal(before->bytes().begin(), before->bytes().end(), exact->bytes().begin()) ||
        (before->exists() && (before->info().filesystem_id != exact->info().filesystem_id ||
          before->info().file_id != exact->info().file_id || before->info().size != exact->info().size))) {
      error = Error(std::errc::state_not_recoverable); return {};
    }
    data->reservation = native_profile->ReserveSnapshotFile(before, error);
    if (!data->reservation) { return {}; }
    if (!data->reservation->IsSnapshotReservationOf(*native_profile)) {
      error = Error(std::errc::state_not_recoverable); return {};
    }
    auto actual = data->reservation->CaptureSnapshot("__thumbnail.png",
        wire::kMaximumMetadataBytes, error, true);
    if (!actual) { return {}; }
    // Allocate the public holder BEFORE attaching. A later allocation failure
    // must not strand an attached active admission with no returned holder.
    auto participant = std::shared_ptr<ProfileThumbnailParticipant>(new ProfileThumbnailParticipant(data));
    // Attach validates actual source ancestry + native directory identity,
    // retains this typed admission, and issues an original-view observation.
    data->expected = members->AttachThumbnail(content_, data->reservation,
                                                actual, data, error);
    if (!data->expected) { return {}; }
    return participant;
  } catch (const std::bad_alloc&) {
    error = Error(std::errc::not_enough_memory); return {};
  }
}
bool ContentMutationOwner::InstallNativeHeaderFormat(
    std::shared_ptr<const NativeHeaderFormat> format, std::error_code& error) {
  error.clear();
  if (global_critical_region::is_held_by_current_thread() || !format) {
    error = Error(std::errc::operation_not_permitted); return false;
  }
  std::unique_lock lock(operation_mutex_, std::try_to_lock);
  if (!lock.owns_lock() || active_.load(std::memory_order_acquire)) {
    error = Error(std::errc::device_or_resource_busy); return false;
  }
  error = CheckCurrent(); if (error) { return false; }
  if (!ready()) { error = Error(std::errc::operation_canceled); return false; }
  if (native_header_format_) {
    if (!native_header_format_->SameBinding(*format)) {
      error = Error(std::errc::permission_denied); return false;
    }
    return true;
  }
  native_header_format_ = std::move(format); return true;
}
// The external header stays in its ACTUAL Headers directory. It is never
// projected into a profile-flat token, admitted with ../, or committed by File
// Write/Flush/Destroy. The PIS is the sole later journal/publication caller.
struct ContentMutationOwner::ProfileHeaderPublication::Data {
  std::shared_ptr<ContentMutationOwner> owner;
  std::weak_ptr<SnapshotPublicationSet> members;
  std::filesystem::path package, target, artifact;
  std::shared_ptr<HostStorageRoot> parent, reservation;
  std::shared_ptr<const HostStorageSnapshotToken> before;
  std::shared_ptr<const NativeHeaderFormat> format;
  mutable std::mutex mutex;
  std::unique_ptr<HostStorageSnapshotStage> stage;
  std::unique_ptr<PreparedSnapshotPublication> prepared;
  std::shared_ptr<const std::vector<uint8_t>> sealed;
  std::optional<RetainedFileMetadata> sealed_metadata;
  std::error_code initialization_error, seal_error;
  std::atomic<bool> mutation_open{true};
  bool active = false, created = false;
  ~Data() {
    // Stage destruction never removes its real/uncertain artifact. The outer
    // owner must retain this duty through native File/IO and journal rundown.
    prepared.reset(); stage.reset();
    if (reservation) {
      reservation->Retire(); reservation->ReleaseRetiredSnapshotReservation();
    }
    if (active) { owner->active_.fetch_sub(1, std::memory_order_acq_rel); }
  }
  std::error_code Available() const {
    if (global_critical_region::is_held_by_current_thread()) {
      return Error(std::errc::operation_would_block);
    }
    if (initialization_error) { return initialization_error; }
    const auto held = members.lock();
    if (!active || !owner || !reservation || reservation->retiring() ||
        !held || !held->base() || held->base()->retiring()) {
      return Error(std::errc::operation_canceled);
    }
    const auto error = owner->CheckCurrent();
    // Validate includes the real WithDependency chain; retiring() is cached
    // parent/own metadata only. This also governs sealed cached stage reads.
    return error ? error : reservation->Validate();
  }
};
ContentMutationOwner::ProfileHeaderPublication::ProfileHeaderPublication(
    std::shared_ptr<Data> data) : data_(std::move(data)) {}
ContentMutationOwner::ProfileHeaderPublication::~ProfileHeaderPublication() = default;
const std::filesystem::path& ContentMutationOwner::ProfileHeaderPublication::package() const { return data_->package; }
const std::filesystem::path& ContentMutationOwner::ProfileHeaderPublication::target() const { return data_->target; }
const std::filesystem::path& ContentMutationOwner::ProfileHeaderPublication::stage_artifact() const { return data_->artifact; }
const std::shared_ptr<const HostStorageSnapshotToken>&
ContentMutationOwner::ProfileHeaderPublication::expected() const { return data_->before; }
void ContentMutationOwner::ProfileHeaderPublication::CloseMutation() noexcept {
  data_->mutation_open.store(false, std::memory_order_release);
}
FileIoResult ContentMutationOwner::ProfileHeaderPublication::Read(
    size_t offset, std::span<uint8_t> bytes) {
  // Check BEFORE locking: no storage mutex wait is permitted under GCRC.
  if (global_critical_region::is_held_by_current_thread()) {
    return {Error(std::errc::operation_would_block), 0};
  }
  std::lock_guard lock(data_->mutex);
  if (auto error = data_->Available()) { return {error, 0}; }
  if (data_->sealed) {
    if (offset >= data_->sealed->size()) { return {}; }
    const auto count = std::min(bytes.size(), data_->sealed->size() - offset);
    std::copy_n(data_->sealed->data() + offset, count, bytes.data());
    return {{}, count};
  }
  if (!data_->stage) { return {Error(std::errc::operation_canceled), 0}; }
  return data_->stage->ReadResult(offset, bytes);
}
FileIoResult ContentMutationOwner::ProfileHeaderPublication::Write(
    size_t offset, std::span<const uint8_t> bytes) {
  if (global_critical_region::is_held_by_current_thread()) {
    return {Error(std::errc::operation_would_block), 0};
  }
  std::lock_guard lock(data_->mutex);
  if (auto error = data_->Available()) { return {error, 0}; }
  if (!data_->mutation_open.load(std::memory_order_acquire) || !data_->stage) {
    return {Error(std::errc::device_or_resource_busy), 0};
  }
  // The raw package's owned extent is independent of Header size. This
  // participant supports in-place native metadata writes only; geometry and
  // unowned/payload ranges cannot be invented by length extension.
  const auto size = data_->before->bytes().size();
  if (offset > size || bytes.size() > size - offset) {
    return {Error(std::errc::operation_not_supported), 0};
  }
  return data_->stage->WriteAllResult(offset, bytes);
}
std::error_code ContentMutationOwner::ProfileHeaderPublication::Resize(size_t size) {
  if (global_critical_region::is_held_by_current_thread()) { return Error(std::errc::operation_would_block); }
  std::lock_guard lock(data_->mutex);
  if (auto error = data_->Available()) { return error; }
  if (!data_->mutation_open.load(std::memory_order_acquire) || !data_->stage) {
    return Error(std::errc::device_or_resource_busy);
  }
  return size == data_->before->bytes().size() ? std::error_code{}
      : Error(std::errc::operation_not_supported);
}
std::error_code ContentMutationOwner::ProfileHeaderPublication::Flush() {
  if (global_critical_region::is_held_by_current_thread()) { return Error(std::errc::operation_would_block); }
  std::lock_guard lock(data_->mutex);
  if (auto error = data_->Available()) { return error; }
  if (data_->seal_error) { return data_->seal_error; }
  // Sealed data was already fsynced. Neither branch publishes public bytes.
  return data_->sealed ? std::error_code{} : data_->stage
      ? data_->stage->FlushResult() : Error(std::errc::operation_canceled);
}
std::optional<RetainedFileMetadata> ContentMutationOwner::ProfileHeaderPublication::Query(
    std::error_code& error) {
  if (global_critical_region::is_held_by_current_thread()) { error = Error(std::errc::operation_would_block); return {}; }
  std::lock_guard lock(data_->mutex);
  error = data_->Available(); if (error) { return {}; }
  if (data_->sealed_metadata) { return data_->sealed_metadata; }
  if (!data_->stage) { error = Error(std::errc::operation_canceled); return {}; }
  return data_->stage->QueryMetadata(error);
}
std::unique_ptr<HostStorageContentLayout>
ContentMutationOwner::ProfileHeaderPublication::CaptureMemberLayout(std::error_code& error) const {
  if (global_critical_region::is_held_by_current_thread()) {
    error = Error(std::errc::operation_would_block); return {};
  }
  error = data_->Available(); if (error) { return {}; }
  const auto members = data_->members.lock();
  if (!members) { error = Error(std::errc::operation_canceled); return {}; }
  return members->CaptureContentLayoutForCM(*data_->owner->content_, data_->package, error);
}
std::shared_ptr<const std::vector<uint8_t>>
ContentMutationOwner::ProfileHeaderPublication::Seal(std::error_code& error) {
  if (global_critical_region::is_held_by_current_thread()) { error = Error(std::errc::operation_would_block); return {}; }
  // Closing future admission precedes waiting for the real current byte IO.
  CloseMutation();
  std::lock_guard lock(data_->mutex);
  error = data_->Available(); if (error) { return {}; }
  if (data_->sealed) { return data_->sealed; }
  if (data_->seal_error) { error = data_->seal_error; return {}; }
  try {
    const auto before = data_->stage ? data_->stage->QueryInfo(error) : std::nullopt;
    if (!before || !before->regular || before->links != 1 ||
        before->size != data_->before->bytes().size()) {
      if (!error) { error = Error(std::errc::state_not_recoverable); }
    }
    std::shared_ptr<std::vector<uint8_t>> bytes;
    if (!error) {
      bytes = std::make_shared<std::vector<uint8_t>>(size_t(before->size));
      for (size_t done = 0; done < bytes->size();) {
        const auto part = std::span(*bytes).subspan(done,
            std::min(size_t(1 << 20), bytes->size() - done));
        const auto actual = data_->stage->ReadResult(done, part);
        if (actual.bytes > part.size()) { error = Error(std::errc::io_error); break; }
        done += actual.bytes;
        if (actual.error || !actual.bytes) { error = actual.error ? actual.error : Error(std::errc::io_error); break; }
      }
    }
    if (!error) {
      error = data_->format ? data_->format->ValidateNativeChange(
          data_->package, data_->before->bytes(), *bytes)
          : Error(std::errc::operation_not_supported);
    }
    if (!error) { error = data_->stage->FlushResult(); }
    if (!error) { error = data_->reservation->FlushDirectory(); }
    const auto after = error ? std::nullopt : data_->stage->QueryInfo(error);
    if (!error && (!after || after->filesystem_id != before->filesystem_id ||
        after->file_id != before->file_id || after->size != before->size ||
        !after->regular || after->links != 1)) { error = Error(std::errc::state_not_recoverable); }
    const auto metadata = error ? std::nullopt : data_->stage->QueryMetadata(error);
    if (!error && !metadata) { error = Error(std::errc::io_error); }
    if (!error) { data_->sealed_metadata = metadata; data_->sealed = std::move(bytes); }
  } catch (const std::bad_alloc&) { error = Error(std::errc::not_enough_memory); }
    catch (...) { error = Error(std::errc::io_error); }
  data_->seal_error = error;
  return error ? nullptr : data_->sealed;
}
std::error_code ContentMutationOwner::ProfileHeaderPublication::CheckCurrent(
    const SnapshotPublicationSet& members) const {
  if (global_critical_region::is_held_by_current_thread()) { return Error(std::errc::operation_would_block); }
  const auto actual = data_->members.lock();
  if (!actual || actual.get() != &members) { return Error(std::errc::permission_denied); }
  return data_->Available();
}
std::error_code ContentMutationOwner::ProfileHeaderPublication::CheckWithSnapshotGuard(
    const SnapshotPublicationSet& members, const HostStorageSnapshotBatch& batch,
    const std::shared_ptr<const HostStorageSnapshotToken>& token) const {
  // The caller ALREADY owns the filesystem guard. Do not call the ordinary
  // CheckCurrent/CheckTarget path here: those would acquire it recursively.
  if (global_critical_region::is_held_by_current_thread()) { return Error(std::errc::operation_would_block); }
  const auto held = data_->members.lock();
  if (!held || held.get() != &members || !data_->active || !data_->owner ||
      !data_->owner->ready() || !data_->reservation || data_->reservation->retiring() ||
      members.base()->retiring() || !data_->owner->source_lease_ || !data_->owner->content_->IntentLeaseMatches(*data_->owner->source_lease_)) {
    return Error(std::errc::operation_canceled);
  }
  // Never wait for an IO/session lock while retaining the profile batch.
  std::unique_lock lock(data_->mutex, std::try_to_lock);
  if (!lock.owns_lock()) { return Error(std::errc::device_or_resource_busy); }
  if (data_->initialization_error) { return data_->initialization_error; }
  // Only our actual before/published token may be joined; external record
  // text and a token from another root/operation never supply this authority.
  const bool before = token && token == data_->before;
  const bool published = token && data_->prepared &&
      token == data_->prepared->receipt().current;
  if (!before && !published) { return Error(std::errc::permission_denied); }
  return data_->reservation->ValidateExternalContentHeaderWithSnapshotGuard(
      batch, *data_->owner->content_, data_->package, data_->target, *token);
}
const PreparedSnapshotPublication*
ContentMutationOwner::ProfileHeaderPublication::PrepareCanonical(
    const SnapshotPublicationSet& members, std::error_code& error) {
  error = CheckCurrent(members); if (error) { return nullptr; }
  std::lock_guard lock(data_->mutex);
  if (!data_->sealed) { error = Error(std::errc::operation_in_progress); return nullptr; }
  if (!data_->prepared) {
    data_->prepared = SnapshotPublisher::PrepareInternal(*data_->owner->recovery_,
        data_->before, *data_->sealed, nullptr, {}, {},
        PrivateSnapshotPurpose::kOrdinary, std::move(data_->stage));
  }
  error = data_->prepared->receipt().error;
  return data_->prepared.get();
}
SnapshotPublicationReceipt
ContentMutationOwner::ProfileHeaderPublication::CommitCanonical(
    const SnapshotPublicationSet& members) {
  if (const auto error = CheckCurrent(members)) { SnapshotPublicationReceipt r; r.error = error; return r; }
  std::lock_guard lock(data_->mutex);
  if (!data_->prepared) { SnapshotPublicationReceipt r; r.error = Error(std::errc::operation_in_progress); return r; }
  auto receipt = SnapshotPublisher::Commit(*data_->prepared);
  if (receipt.published) {
    // The canonical namespace reservation stays owned through the SAME outer
    // journal/Done/cache cut. The renamed stage's exclusive File lease must
    // end now, or durability/final validation would conflict with our own
    // child lease. Publication is real even when its barrier is uncertain.
    data_->prepared->stage_.reset();
  }
  return receipt;
}
std::shared_ptr<ContentMutationOwner::ProfileHeaderPublication>
ContentMutationOwner::PrepareProfileHeader(const std::filesystem::path& package,
    std::shared_ptr<const HostStorageSnapshotToken> actual_header,
    const std::shared_ptr<SnapshotPublicationSet>& members, std::error_code& error) {
  error.clear();
  if (global_critical_region::is_held_by_current_thread() || !members ||
      !members->base() || members->base()->read_only() || !actual_header ||
      actual_header->root() != content_ || !actual_header->ascii_folded_leaf()) {
    error = Error(std::errc::operation_not_permitted); return {};
  }
  std::vector<std::string> parts;
  for (const auto& p : package) { parts.push_back(p.generic_string()); }
  const auto fold = [](std::string value) {
    for (auto& c : value) { if (c >= 'a' && c <= 'z') { c -= 'a' - 'A'; } }
    return value;
  };
  if (!wire::ValidPackage(package) || parts.size() != 4 ||
      parts[0] == "0000000000000000" || fold(parts[0]) != fold(parts[3]) ||
      fold(parts[1]) != "FFFE07D1" || parts[2] != "00010000" ||
      !content_ || !content_->directory_identity() ||
      !members->base()->directory_identity() ||
      !content_->directory_identity()->SameEntry(package,
          *members->base()->directory_identity(), {})) {
    error = Error(std::errc::permission_denied); return {};
  }
  std::unique_lock operation(operation_mutex_, std::try_to_lock);
  if (!operation.owns_lock()) { error = Error(std::errc::device_or_resource_busy); return {}; }
  error = CheckCurrent();
  if (!error && (!ready() || active_.load(std::memory_order_acquire))) {
    error = Error(std::errc::device_or_resource_busy);
  }
  if (!error && !native_header_format_) { error = Error(std::errc::operation_not_supported); }
  if (!error) { error = native_header_format_->ValidateCurrent(package, actual_header->bytes()); }
  if (error) { return {}; }
  std::shared_ptr<ProfileHeaderPublication> result;
  try {
    auto data = std::make_shared<ProfileHeaderPublication::Data>();
    data->owner = shared_from_this(); data->members = members; data->package = package;
    data->format = native_header_format_;
    wire::Record slot; slot.kind = Kind::kHeader; slot.package = package;
    // Preserve the real spelling selected by the SAME source resolver. The
    // closed grammar must still derive Headers/Type/Package.header from this
    // canonical package; an arbitrary external token/path cannot join.
    slot.target = actual_header->relative(); data->target = wire::Target(slot);
    if (data->target.empty()) { error = Error(std::errc::invalid_argument); return {}; }
    active_.fetch_add(1, std::memory_order_acq_rel); data->active = true;
    const auto exact = CaptureMetadata(Kind::kHeader, package, data->target, error);
    if (!exact) { return {}; }
    if (exact->exists() != actual_header->exists() || !Equal(exact->bytes(), actual_header->bytes()) ||
        (exact->exists() && (exact->info().filesystem_id != actual_header->info().filesystem_id ||
          exact->info().file_id != actual_header->info().file_id || exact->info().size != actual_header->info().size))) {
      error = Error(std::errc::state_not_recoverable); return {};
    }
    // Cold projection/upgrade is a separate real CM producer. An absent or
    // legacy descriptor cannot become a writable native CON by this issuer.
    if (!exact->exists() || exact->bytes().size() < 0x971A) {
      error = Error(std::errc::operation_not_supported); return {};
    }
    auto header_parent = content_->Subroot(data->target.parent_path(), false, error);
    if (!header_parent) { return {}; }
    // Carry the actual admitted profile/source generation into physical CAS,
    // not merely a preparatory cached Current check. This existing dependency
    // also refuses a removed/retired source at the real HostStorage effect.
    data->parent = header_parent->WithDependency(members->base(), error);
    if (!data->parent) { return {}; }
    auto before = data->parent->CaptureSnapshot(data->target.filename(),
        wire::kMaximumMetadataBytes, error, true);
    if (!before) { return {}; }
    if (before->exists() != exact->exists() || !Equal(before->bytes(), exact->bytes()) ||
        before->info().filesystem_id != exact->info().filesystem_id ||
        before->info().file_id != exact->info().file_id || before->info().size != exact->info().size) {
      error = Error(std::errc::state_not_recoverable); return {};
    }
    data->reservation = data->parent->ReserveSnapshotFile(before, error);
    if (!data->reservation) { return {}; }
    data->before = data->reservation->CaptureSnapshot(data->target.filename(),
        wire::kMaximumMetadataBytes, error, true);
    if (!data->before) { return {}; }
    result = std::shared_ptr<ProfileHeaderPublication>(new ProfileHeaderPublication(data));
    // Return the actual created/uncertain session on a later IO failure. Its
    // path is retained; destruction never deletes or reports an abort/commit.
    for (unsigned n = 0; n < 8; ++n) {
      data->artifact = ".xenia-stage-" + Nonce() + ".tmp";
      data->stage = data->reservation->CreateSnapshotStage(data->artifact, data->created, error);
      if (data->stage || data->created || error != std::errc::file_exists) { break; }
    }
    if (!data->stage && !error) { error = Error(std::errc::io_error); }
    if (!error) {
      const auto write = data->stage->WriteAllResult(0, data->before->bytes());
      error = write.error;
      if (!error && write.bytes != data->before->bytes().size()) { error = Error(std::errc::io_error); }
    }
    data->initialization_error = error;
    return result;
  } catch (const std::bad_alloc&) { error = Error(std::errc::not_enough_memory); }
    catch (...) { error = Error(std::errc::io_error); }
  if (result) { result->data_->initialization_error = error; }
  return result;
}
ContentMutationOwner::Receipt ContentMutationOwner::WriteMetadata(
    Kind kind, const std::filesystem::path& package,
    std::shared_ptr<const HostStorageSnapshotToken> expected,
    std::span<const uint8_t> bytes, const std::atomic<bool>* cancel) {
  if (kind != Kind::kHeader && kind != Kind::kThumbnail) {
    return {Error(std::errc::invalid_argument)};
  }
  return Mutate(kind, package, std::move(expected), bytes, cancel);
}
ContentMutationOwner::Receipt ContentMutationOwner::Create(
    const std::filesystem::path& package,
    std::shared_ptr<const HostStorageSnapshotToken> expected,
    std::span<const uint8_t> bytes, const std::atomic<bool>* cancel) {
  return Mutate(Kind::kCreate, package, std::move(expected), bytes, cancel);
}
ContentMutationOwner::Receipt ContentMutationOwner::CreateInitial(
    const std::filesystem::path& package,
    std::shared_ptr<const HostStorageSnapshotToken> expected,
    std::span<const uint8_t> header,
    std::span<const ContentPackageInitialMember> members,
    const std::atomic<bool>* cancel) {
  Receipt refused;
  refused.error=wire::ValidateInitialMembers(members);
  if (!refused.error && (!expected || expected->exists())) {
    refused.error=Error(std::errc::file_exists);
  }
  if (refused.error) { return refused; }
  return Mutate(Kind::kCreate,package,std::move(expected),header,cancel,{},members);
}
ContentMutationOwner::Receipt ContentMutationOwner::CreateStream(
    const std::filesystem::path& package,
    std::shared_ptr<const HostStorageSnapshotToken> expected,
    std::span<const uint8_t> header, ContentPackageStreamSource& source,
    const std::atomic<bool>* canceled, std::atomic<uint64_t>* progress) {
  return StreamInternal(package, std::move(expected), header, source, canceled, progress);
}
ContentMutationOwner::Receipt ContentMutationOwner::StreamInternal(
    const std::filesystem::path& package,
    std::shared_ptr<const HostStorageSnapshotToken> expected,
    std::span<const uint8_t> header, ContentPackageStreamSource& source,
    const std::atomic<bool>* canceled, std::atomic<uint64_t>* progress,
    uint64_t stop_after_bytes, std::optional<uint64_t> space_ceiling,
    std::atomic<bool>* cancel_after_authentication, unsigned preparation_boundary) {
  Receipt receipt;
  if (xe::global_critical_region::is_held_by_current_thread()) {
    receipt.error = Error(std::errc::operation_not_permitted); return receipt;
  }
  std::lock_guard operation(operation_mutex_); Active active(active_);
  auto& error = receipt.error;
  error = CheckCurrent();
  if (!error && (!ready() || active_.load(std::memory_order_acquire) != 1)) {
    error = Error(std::errc::device_or_resource_busy);
  }
  wire::Record record;
  record.identity = wire_identity_; record.kind = Kind::kCreate;
  record.package = package; record.streamed = record.stream_pending = true;
  if (expected) { record.target = expected->relative(); }
  if (!error && (!expected || expected->root().get() != content_.get() ||
      wire::Target(record).empty() || header.empty() || header.size() > wire::kMaximumMetadataBytes)) {
    error = Error(std::errc::invalid_argument);
  }
  if (!error && expected->exists()) { error = Error(std::errc::file_exists); }
  if (!error && canceled && canceled->load(std::memory_order_acquire)) { error = Error(std::errc::operation_canceled); }
  if (error) { return receipt; }
  // A bounded metadata snapshot, never a detached aggregate payload.
  const auto supplied = source.members();
  if (supplied.size() >= package_wire::kMaximumEntries) {
    error = Error(std::errc::value_too_large); return receipt;
  }
  const std::vector<ContentPackageStreamMember> members(supplied.begin(), supplied.end());
  error = wire::ValidateStreamMembers(members);
  if (!error) { error = source.CheckCurrent(); }
  if (error) { return receipt; }
  uint64_t allocation = uint64_t(header.size()) * 4 + (16u << 16);
  for (const auto& member : members) { allocation += member.size + 8192 + member.relative.native().size(); }
  const auto space = content_->Observe();
  if (space.error || !space.available || space.read_only ||
      std::min(space.usable_bytes, space_ceiling.value_or(UINT64_MAX)) < allocation) {
    error = space.error ? space.error : Error(space.read_only ? std::errc::read_only_file_system : std::errc::no_space_on_device);
    return receipt;
  }
  const auto names = records_->ListContentMutationFiles(error);
  if (!error && (maximum_records_ < 12 || names.size() > maximum_records_ - 12)) {
    error = Error(std::errc::no_space_on_device);
  }
  if (error) { return receipt; }
  auto reserved = content_->ReserveContentNamespace(record.target, {}, false, error);
  if (!reserved) { return receipt; }
  auto current = reserved->CaptureSnapshot(record.target, wire::kMaximumMetadataBytes, error, true);
  if (!current || current->exists()) {
    if (!error) { error = Error(std::errc::file_exists); }
    reserved->ReleaseContentNamespace(); return receipt;
  }
  error = EnsureParents(package.parent_path());
  auto package_reserved = error ? nullptr : content_->ReserveContentNamespace(package, {}, true, error);
  if (!package_reserved) { reserved->ReleaseContentNamespace(); return receipt; }
  auto release = [&] { reserved->ReleaseContentNamespace(); package_reserved->ReleaseContentNamespace(); };
  auto retain = [&] {
    blocked_.store(true, std::memory_order_release);
    pending_reservations_[record.target] = {record.nonce, reserved};
    pending_reservations_[record.package] = {record.nonce, package_reserved};
  };
  for (unsigned i = 0; i != 8 && !wire::ValidNonce(record.nonce); ++i) { record.nonce = Nonce(); }
  if (!wire::ValidNonce(record.nonce)) { error = Error(std::errc::io_error); release(); return receipt; }
  receipt.nonce = record.nonce;
  const auto leaf = "package-" + record.nonce + ".tmp";
  std::vector<uint8_t> preparation;
  error = content_->CheckContentPreparationPeer(*staging_);
  if (error) { release(); return receipt; }
  // Close admission and retain real namespace permits before a durable plan
  // or any allocation after it; unwinding can never leave ready=true.
  retain();
  error = BeginPreparation(record,*current,header,preparation);
  // A canonical plan may exist even if its parent barrier returned an error.
  // It grants no publication authority, but must be reconciled before reuse.
  if (error) {
    // No dependent artifacts have been permitted yet. A partial/canonical
    // control record remains private for Recover, without a leaked live permit.
    release(); pending_reservations_.erase(record.target); pending_reservations_.erase(record.package);
    return receipt;
  }
  auto abort_preparation = [&](unsigned boundary = 0) {
    wire::Preparation plan;
    std::error_code parse;
    if (!wire::DecodePreparation(preparation,plan,parse)) { error = parse; retain(); return; }
    const auto original = error;
    const auto aborted = AbortPreparation(plan,preparation,reserved,package_reserved,boundary);
    if (aborted) { error = aborted; retain(); }
    else {
      error = original; receipt.aborted_before_publication = true;
      receipt.preserved_artifacts = preserved_artifacts_;
      blocked_.store(false,std::memory_order_release);
    }
  };
  if (preparation_boundary == 5) { error = Error(std::errc::operation_canceled); return receipt; }
  error = staging_->Create(leaf, true);
  if (error) { abort_preparation(); return receipt; }
  if (preparation_boundary == 8) { error = Error(std::errc::operation_canceled); return receipt; }
  auto staged = staging_->Subroot(leaf, false, error);
  if (staged) {
    record.directory_stage = staged->IntentRootIdentity(error).directory;
    for (const auto& member : members) {
      if (error) { break; }
      if (member.directory) { error = staged->Create(member.relative, true); }
      else { error = staged->WriteNewFile(member.relative, {}, false).error; }
      if (!error && preparation_boundary == 4) {
        error = Error(std::errc::operation_canceled); return receipt;
      }
    }
  }
  if (!error) { record.initial_tree = staging_->CaptureInitialContentTree(leaf, error); }
  if (!error && record.initial_tree.size() != members.size() + 1) { error = Error(std::errc::state_not_recoverable); }
  std::map<std::filesystem::path, uint64_t> declared;
  for (const auto& member : members) { declared.emplace(member.relative, member.size); }
  for (auto& row : record.initial_tree) {
    if (row.entry.directory) { continue; }
    const auto found = declared.find(row.entry.relative);
    if (found == declared.end() || row.entry.bytes) { error = Error(std::errc::state_not_recoverable); break; }
    row.entry.bytes = found->second; row.digest = {};
  }
  // The durable plan authenticates only these slots. Ordinary failure moves
  // observed data intact to preservation; a stream record is still required
  // before any input payload write or authenticated cleanup.
  if (error || !staged) { staged.reset(); abort_preparation(); return receipt; }
  auto prepared = SnapshotPublisher::PrepareInternal(*recovery_, current, header, nullptr, record.nonce, {});
  if (prepared->receipt().error) {
    error = prepared->receipt().error; prepared.reset(); staged.reset();
    abort_preparation(); return receipt;
  }
  if (preparation_boundary == 6) { error = Error(std::errc::operation_canceled); return receipt; }
  record.intended.assign(header.begin(), header.end());
  record.stage = prepared->receipt().stage; record.stage_file = prepared->stage_info().file_id;
  const auto admission = wire::Encode(record, error);
  if (!error) {
    error = AppendRecord(StreamJournal(record.nonce), admission);
  }
  if (error) { retain(); return receipt; }
  receipt.stream_admitted_durable = true;
  if (progress) { progress->store(0, std::memory_order_release); }
  uint64_t transferred = 0;
  std::vector<uint8_t> buffer(1u << 20);
  for (size_t index = 0; index != members.size() && !error; ++index) {
    const auto& member = members[index];
    if (member.directory) { continue; }
    auto file = staged->OpenFile(member.relative, FileAccess::kFileWriteData, error);
    if (!file) { break; }
    const auto opened = file->QueryInfo(error);
    const auto admitted = std::find_if(record.initial_tree.begin(), record.initial_tree.end(),
        [&](const auto& row) { return row.entry.relative == member.relative; });
    if (!opened || error || admitted == record.initial_tree.end() || !opened->regular ||
        opened->links != 1 || opened->size || opened->file_id != admitted->entry.file) {
      if (!error) { error = Error(std::errc::state_not_recoverable); } break;
    }
    for (uint64_t offset = 0; offset != member.size && !error;) {
      if (canceled && canceled->load(std::memory_order_acquire)) { error = Error(std::errc::operation_canceled); break; }
      error = source.CheckCurrent();
      if (error) { break; }
      const auto count = size_t(std::min<uint64_t>(buffer.size(), member.size - offset));
      auto part = std::span(buffer).first(count);
      const auto read = source.Read(index, offset, part);
      if (read.error || read.bytes != count) { error = read.error ? read.error : Error(std::errc::io_error); break; }
      if (canceled && canceled->load(std::memory_order_acquire)) { error = Error(std::errc::operation_canceled); break; }
      const auto written = file->WriteAllResult(size_t(offset), part);
      transferred += written.bytes;
      if (progress) { progress->fetch_add(written.bytes, std::memory_order_release); }
      if (written.error || written.bytes != count) { error = written.error ? written.error : Error(std::errc::io_error); break; }
      offset += count;
      if (transferred >= stop_after_bytes) {
        error = Error(std::errc::operation_canceled); retain(); return receipt;
      }
    }
    if (!error) { error = file->FlushResult(); }
  }
  if (!error && canceled && canceled->load(std::memory_order_acquire)) { error = Error(std::errc::operation_canceled); }
  if (!error) { error = source.CheckCurrent(); }
  if (error) {
    const auto original = error; prepared.reset(); staged.reset();
    const auto rollback = RollbackStream(record, admission, reserved, package_reserved);
    if (rollback) { error = rollback; retain(); }
    else { error = original; blocked_.store(false, std::memory_order_release); }
    return receipt;
  }
  auto complete = record; complete.stream_pending = false;
  complete.initial_tree = staging_->CaptureInitialContentTree(leaf, error);
  if (!error && !SameStreamAuthority(record, complete)) { error = Error(std::errc::state_not_recoverable); }
  // Private deny-only seam: the complete native tree authentication above ran
  // normally. Tests can only raise cancellation in its completion window.
  if (cancel_after_authentication) { cancel_after_authentication->store(true, std::memory_order_release); }
  const auto encoded = error ? std::vector<uint8_t>{} : wire::Encode(complete, error);
  // Hashing and encoding can be long. They do not constitute durable complete
  // preparation; cancellation must roll back, and source withdrawal must
  // prevent publication.
  if (!error && canceled && canceled->load(std::memory_order_acquire)) { error = Error(std::errc::operation_canceled); }
  if (!error) { error = source.CheckCurrent(); }
  if (!error && canceled && canceled->load(std::memory_order_acquire)) { error = Error(std::errc::operation_canceled); }
  if (error) {
    const auto original = error; prepared.reset(); staged.reset();
    // Authentication conflicts remain a preserved third state; an ordinary
    // cancellation can discard only its earlier authenticated mutable stage.
    if (original == std::errc::operation_canceled) {
      const auto rollback = RollbackStream(record, admission, reserved, package_reserved);
      if (rollback) { error = rollback; retain(); }
      else { error = original; blocked_.store(false, std::memory_order_release); }
    } else { retain(); }
    return receipt;
  }
  error = AppendRecord(Journal(record.nonce),encoded);
  if (error) { retain(); return receipt; }
  // Durable complete-tree intent is now the normal publication obligation.
  // Cancellation after this point cannot discard an already prepared commit.
  auto result = Advance(complete, encoded, reserved, package_reserved, std::move(prepared), false);
  result.stream_admitted_durable = true;
  if (result.complete) { blocked_.store(false, std::memory_order_release); }
  return result;
}
std::error_code ContentMutationOwner::RollbackStream(const wire::Record& record,
    std::span<const uint8_t> encoded, const std::shared_ptr<HostStorageRoot>& reserved,
    const std::shared_ptr<HostStorageRoot>& package_reserved) {
  if (!record.streamed || !record.stream_pending || record.identity != wire_identity_) {
    return Error(std::errc::state_not_recoverable);
  }
  std::error_code error;
  const auto current = reserved->CaptureSnapshot(record.target, wire::kMaximumMetadataBytes, error, true);
  if (!current || current->exists()) { return error ? error : Error(std::errc::state_not_recoverable); }
  error = staging_->DiscardStreamContentDirectory("package-" + record.nonce + ".tmp",
                                                 record.directory_stage, record.initial_tree);
  if (!error) { error = reserved->DiscardStreamHeaderStage(record.stage, record.stage_file, record.intended); }
  if (!error) {
    const auto done = wire::Done(encoded, error);
    if (!error) {
      error = AppendRecord(Journal(record.nonce, true), done);
    }
  }
  if (!error) {
    reserved->ReleaseContentNamespace(); package_reserved->ReleaseContentNamespace();
    pending_reservations_.erase(record.target); pending_reservations_.erase(record.package);
  }
  return error;
}
ContentMutationOwner::Receipt ContentMutationOwner::Mutate(
    Kind kind, const std::filesystem::path& package,
    std::shared_ptr<const HostStorageSnapshotToken> expected,
    std::span<const uint8_t> bytes, const std::atomic<bool>* cancel,
    std::error_code post_stage_parent_error,
    std::span<const ContentPackageInitialMember> initial,
    unsigned stop_boundary, std::optional<uint64_t> initial_space_ceiling) {
  Receipt receipt;
  if (xe::global_critical_region::is_held_by_current_thread()) {
    receipt.error = Error(std::errc::operation_not_permitted); return receipt;
  }
  std::lock_guard operation(operation_mutex_); Active active(active_);
  auto& error = receipt.error;
  error = CheckCurrent();
  if (!error && active_.load(std::memory_order_acquire) != 1) {
    error = Error(std::errc::device_or_resource_busy);
  }
  if (!error && !ready()) { error = Error(std::errc::operation_in_progress); }
  wire::Record record;
  record.identity = wire_identity_; record.kind = kind; record.package = package;
  if (expected) { record.target = expected->relative(); }
  if (error || !expected || expected->root().get() != content_.get() ||
      wire::Target(record).empty() || bytes.size() > wire::kMaximumMetadataBytes ||
      (kind != Kind::kThumbnail && bytes.empty())) {
    if (!error) { error = Error(std::errc::invalid_argument); } return receipt;
  }
  if (cancel && cancel->load(std::memory_order_acquire)) {
    error = Error(std::errc::operation_canceled); return receipt;
  }
  if (!initial.empty()) {
    error=wire::ValidateInitialMembers(initial);
    if (!error && (kind!=Kind::kCreate || expected->exists())) { error=Error(std::errc::invalid_argument); }
    if (error) { return receipt; }
  }
  if (kind == Kind::kCreate) {
    uint64_t bound=uint64_t(bytes.size())*4+uint64_t(expected->bytes().size())*2+(16u << 16);
    for (const auto& member:initial) { bound+=member.bytes.size()+8192+member.relative.native().size(); }
    const auto space=content_->Observe();
    const auto usable=std::min(space.usable_bytes,
        initial_space_ceiling.value_or(UINT64_MAX));
    if (space.error || !space.available || space.read_only || usable<bound) {
      error=space.error ? space.error : Error(space.read_only ? std::errc::read_only_file_system : std::errc::no_space_on_device);
      return receipt;
    }
  }
  const auto records = records_->ListContentMutationFiles(error);
  if (error) { return receipt; }
  const size_t required_records = kind == Kind::kCreate ? 12 : 4;
  if (maximum_records_ < required_records || records.size() > maximum_records_ - required_records) {
    // Refuse finite exhaustion before any stage or Prepared; never admit an
    // intent into the last free slot and strand its required Done marker.
    error = Error(std::errc::no_space_on_device); return receipt;
  }
  // The caller's load predates any parent creation and must be valid already.
  std::optional<FileHandleInfo> before;
  if (expected->exists()) { before = expected->info(); }
  std::shared_ptr<HostStorageRoot> package_reservation;
  if (kind != Kind::kCreate) {
    package_reservation = content_->ReserveContentIo(package, error);
    if (!package_reservation) { return receipt; }
  }
  auto reserved = (package_reservation ? package_reservation : content_)->ReserveContentNamespace(
      record.target, before, false, error);
  if (!reserved) {
    if (package_reservation) { package_reservation->ReleaseContentNamespace(); }
    return receipt;
  }
  auto release = [&] {
    reserved->ReleaseContentNamespace();
    if (package_reservation) { package_reservation->ReleaseContentNamespace(); }
  };
  auto current = reserved->CaptureSnapshot(record.target, wire::kMaximumMetadataBytes, error, true);
  if (!current || current->exists() != expected->exists() ||
      (current->exists() && (current->info().file_id != expected->info().file_id ||
                            !Equal(current->bytes(), expected->bytes())))) {
    if (!error) { error = Error(std::errc::state_not_recoverable); }
    release(); return receipt;
  }
  std::shared_ptr<HostStorageRoot> staged_directory;
  for (unsigned attempt = 0; attempt != 8 && !wire::ValidNonce(record.nonce); ++attempt) {
    record.nonce = Nonce();
  }
  if (!wire::ValidNonce(record.nonce)) {
    error = Error(std::errc::io_error); release(); return receipt;
  }
  receipt.nonce = record.nonce;
  std::vector<uint8_t> preparation;
  auto retain = [&] {
    blocked_.store(true,std::memory_order_release);
    pending_reservations_[record.target] = {record.nonce,reserved};
    if (package_reservation) { pending_reservations_[record.package] = {record.nonce,package_reservation}; }
  };
  auto abort_preparation = [&] {
    wire::Preparation plan; std::error_code parse;
    if (preparation.empty() || !wire::DecodePreparation(preparation,plan,parse)) {
      if (parse) { error = parse; } retain(); return;
    }
    const auto original = error;
    const auto aborted = AbortPreparation(plan,preparation,reserved,package_reservation);
    if (aborted) { error = aborted; retain(); }
    else {
      error = original; receipt.aborted_before_publication = true;
      receipt.preserved_artifacts = preserved_artifacts_;
      blocked_.store(false,std::memory_order_release);
    }
  };
  if (kind == Kind::kCreate) {
    error = EnsureParents(package.parent_path());
    if (!error) { package_reservation = content_->ReserveContentNamespace(package, {}, true, error); }
    if (!package_reservation) { release(); return receipt; }
    error = content_->CheckContentPreparationPeer(*staging_);
    if (error) { release(); return receipt; }
    retain();
    error = BeginPreparation(record,*current,bytes,preparation);
    if (error) {
      release(); pending_reservations_.erase(record.target); pending_reservations_.erase(record.package);
      return receipt;
    }
    if (stop_boundary == 5) { error = Error(std::errc::operation_canceled); return receipt; }
    const auto leaf = "package-" + record.nonce + ".tmp";
    error = staging_->Create(leaf, true);
    if (!error && stop_boundary == 8) { error = Error(std::errc::operation_canceled); return receipt; }
    if (!error) { staged_directory = staging_->Subroot(leaf, false, error); }
    if (staged_directory) {
      record.directory_stage = staged_directory->IntentRootIdentity(error).directory;
      for (const auto& member:initial) {
        if (error) { break; }
        if (cancel && cancel->load(std::memory_order_acquire)) {
          error=Error(std::errc::operation_canceled); break;
        }
        if (member.directory) { error=staged_directory->Create(member.relative,true); }
        else {
          const auto written=staged_directory->WriteNewFile(member.relative,member.bytes,true,cancel);
          error=written.error;
          if (!error && written.bytes!=member.bytes.size()) { error=Error(std::errc::io_error); }
        }
        // A stop seam follows real filesystem creation/write, with only the
        // durable preparation plan available on restart.
        if (!error && stop_boundary==4) { error=Error(std::errc::operation_canceled); return receipt; }
        if (!error && stop_boundary==7) { error=Error(std::errc::operation_canceled); }
      }
      if (!error && !initial.empty()) {
        record.initial_tree=staging_->CaptureInitialContentTree(leaf,error);
        if (!error && record.initial_tree.size()!=initial.size()+1) { error=Error(std::errc::state_not_recoverable); }
        std::map<std::filesystem::path,const ContentPackageInitialMember*> intended;
        for (const auto& member:initial) { intended.emplace(member.relative,&member); }
        for (const auto& row:record.initial_tree) {
          if (error || row.entry.relative.empty()) { continue; }
          const auto found=intended.find(row.entry.relative);
          if (found==intended.end() || row.entry.directory!=found->second->directory) {
            error=Error(std::errc::state_not_recoverable); break;
          }
          if (!row.entry.directory && (row.entry.bytes!=found->second->bytes.size() ||
              row.digest!=wire::Digest(found->second->bytes,error))) {
            if (!error) { error=Error(std::errc::state_not_recoverable); } break;
          }
        }
      }
      if (!error) { error = staging_->FlushDirectory(); }
    }
    if (error || !staged_directory) {
      staged_directory.reset(); abort_preparation(); return receipt;
    }
  } else {
    // The package I/O reservation is already active. Reopen through its
    // retained view so only this actual permit is inherited; the unreserved
    // root correctly refuses the same path, as it must for foreign callers.
    auto directory = package_reservation->Subroot(package, false, error);
    if (!directory) { release(); return receipt; }
  }
  auto prepared = SnapshotPublisher::PrepareInternal(
      *recovery_, current, bytes, cancel, record.nonce, post_stage_parent_error);
  if (prepared->receipt().error) {
    receipt.metadata = prepared->receipt(); error = receipt.metadata.error;
    if (kind == Kind::kCreate) {
      prepared.reset(); staged_directory.reset(); abort_preparation(); return receipt;
    }
    if (staged_directory || receipt.metadata.stage_created || receipt.metadata.recovery_created) {
      // Retain uncertain, unrecorded staging. It is not a completed operation
      // and cannot be recovered by guessing intended bytes after the fact.
      blocked_.store(true, std::memory_order_release);
      pending_reservations_[record.target] = {record.nonce, reserved};
      if (package_reservation) { pending_reservations_[record.package] = {record.nonce, package_reservation}; }
    } else { release(); }
    return receipt;
  }
  if (kind == Kind::kCreate && stop_boundary == 6) { error = Error(std::errc::operation_canceled); return receipt; }
  record.before_exists = current->exists();
  record.before_file = current->exists() ? current->info().file_id : 0;
  record.before.assign(current->bytes().begin(), current->bytes().end());
  record.intended.assign(prepared->intended().begin(), prepared->intended().end());
  record.stage = prepared->receipt().stage;
  record.stage_file = prepared->stage_info().file_id;
  record.recovery = prepared->receipt().recovery;
  if (record.before_exists) { record.recovery_file = prepared->recovery_->info().file_id; }
  const auto encoded = wire::Encode(record, error);
  if (!error && kind == Kind::kCreate && cancel && cancel->load(std::memory_order_acquire)) {
    error = Error(std::errc::operation_canceled);
    prepared.reset(); staged_directory.reset(); abort_preparation(); return receipt;
  }
  if (!error) {
    error = AppendRecord(Journal(record.nonce), encoded);
  }
  if (error) {
    // A short journal may exist. Never release a namespace with uncertain
    // durable multi-file obligations or delete the only exact raw backup.
    blocked_.store(true, std::memory_order_release);
    pending_reservations_[record.target] = {record.nonce, std::move(reserved)};
    if (package_reservation) { pending_reservations_[record.package] = {record.nonce, std::move(package_reservation)}; }
    return receipt;
  }
  // Cancellation after durable Prepared cannot erase its recovery obligation.
  // Finish this transaction; subsequent admission observes cancellation normally.
  // The private stop seam follows genuine durable preparation, never invents IO.
  if (stop_boundary==1) {
    receipt.prepared_durable=true;
    receipt.error=Error(std::errc::operation_canceled);
    blocked_.store(true,std::memory_order_release);
    pending_reservations_[record.target]={record.nonce,std::move(reserved)};
    pending_reservations_[record.package]={record.nonce,std::move(package_reservation)};
    return receipt;
  }
  auto result = Advance(record, encoded, std::move(reserved), std::move(package_reservation),
                        std::move(prepared), false,stop_boundary);
  if (result.complete) { blocked_.store(false,std::memory_order_release); }
  return result;
}
ContentMutationOwner::Receipt ContentMutationOwner::Advance(
    const wire::Record& record, std::span<const uint8_t> encoded,
    std::shared_ptr<HostStorageRoot> reserved,
    std::shared_ptr<HostStorageRoot> package_reserved,
    std::unique_ptr<PreparedSnapshotPublication> prepared, bool recovery,
    unsigned stop_boundary) {
  Receipt receipt;
  receipt.nonce = record.nonce; receipt.prepared_durable = true;
  receipt.observed = recovery;
  auto& error = receipt.error;
  if (!record.initial_tree.empty()) {
    auto final=package_reserved->Subroot(record.package,false,error);
    HostStorageDurableObservation observed;
    if (final) {
      observed=package_reserved->ObserveInitialContentDirectory(
          record.package,record.directory_stage,record.initial_tree);
    } else if (error==std::errc::no_such_file_or_directory) {
      error.clear();
      observed=staging_->ObserveInitialContentDirectory(
          "package-"+record.nonce+".tmp",record.directory_stage,record.initial_tree);
    }
    if (!error) { error=observed.error; }
    if (!error && (!observed.matched || !observed.durable)) { error=Error(std::errc::io_error); }
  }
  // The exact retained backup remains part of the completed transaction even
  // when the target already contains the intended inode after a restart.
  if (!error && record.before_exists) {
    const auto backup = recovery_->root()->CaptureSnapshot(
        record.recovery, wire::kMaximumMetadataBytes, error);
    if (!error && (!backup || !backup->exists() ||
        backup->info().file_id != record.recovery_file ||
        !Equal(backup->bytes(), record.before))) {
      error = Error(std::errc::state_not_recoverable);
    }
  }
  auto current = error ? nullptr : reserved->CaptureSnapshot(record.target, wire::kMaximumMetadataBytes, error, true);
  const bool after = current && current->exists() &&
      current->info().file_id == record.stage_file && Equal(current->bytes(), record.intended);
  if (!error && after) {
    const auto observed = reserved->ObserveIntentSnapshot(*current);
    error = observed.error; receipt.durable = observed.durable;
  } else if (!error && current && current->exists() == record.before_exists &&
      (!record.before_exists || (current->info().file_id == record.before_file &&
                                Equal(current->bytes(), record.before)))) {
    if (!prepared) {
      prepared = std::make_unique<PreparedSnapshotPublication>();
      prepared->receipt_.expected = current;
      prepared->receipt_.stage = record.stage;
      prepared->receipt_.recovery = record.recovery;
      prepared->intended_ = record.intended;
      prepared->stage_ = reserved->RecoverContentSnapshotStage(
          record.stage, record.stage_file, record.intended, error);
      if (!error && record.before_exists) {
        prepared->recovery_ = recovery_->root()->CaptureSnapshot(
            record.recovery, wire::kMaximumMetadataBytes, error);
        if (!error && (!prepared->recovery_->exists() ||
            prepared->recovery_->info().file_id != record.recovery_file ||
            !Equal(prepared->recovery_->bytes(), record.before))) {
          error = Error(std::errc::state_not_recoverable);
        }
      }
    }
    if (!error) {
      receipt.metadata = SnapshotPublisher::Commit(*prepared);
      error = receipt.metadata.error;
      receipt.metadata_published = receipt.metadata.published;
      receipt.durable = receipt.metadata.durable;
    }
  } else if (!error) { error = Error(std::errc::state_not_recoverable); }
  if (!error && stop_boundary==2) { error=Error(std::errc::operation_canceled); }
  if (!error && record.kind == Kind::kCreate) {
    auto final = package_reserved->Subroot(record.package, false, error);
    if (final) {
      const auto observed = record.initial_tree.empty()
          ? package_reserved->ObserveContentDirectory(record.package,record.directory_stage)
          : package_reserved->ObserveInitialContentDirectory(record.package,record.directory_stage,record.initial_tree);
      error = observed.error; receipt.durable = receipt.durable && observed.durable;
    } else if (error == std::errc::no_such_file_or_directory) {
      auto staged = staging_->Subroot("package-" + record.nonce + ".tmp", false, error);
      if (staged) {
        error = record.initial_tree.empty()
            ? package_reserved->PublishContentDirectory(*staged,record.directory_stage,
                record.package,receipt.directory_published)
            : package_reserved->PublishInitialContentDirectory(*staged,record.directory_stage,
                record.package,receipt.directory_published,record.initial_tree);
      }
    }
  }
  if (!error && stop_boundary==3) { error=Error(std::errc::operation_canceled); }
  if (!error && receipt.durable) {
    const auto done = wire::Done(encoded, error);
    if (!error) {
      error = AppendRecord(Journal(record.nonce, true), done);
    }
  }
  receipt.complete = !error && receipt.durable;
  if (receipt.complete) {
    reserved->ReleaseContentNamespace();
    if (package_reserved) { package_reserved->ReleaseContentNamespace(); }
    pending_reservations_.erase(record.target);
    pending_reservations_.erase(record.package);
  } else {
    blocked_.store(true, std::memory_order_release);
    pending_reservations_[record.target] = {record.nonce, std::move(reserved)};
    if (package_reserved) { pending_reservations_[record.package] = {record.nonce, std::move(package_reserved)}; }
  }
  return receipt;
}
std::error_code ContentMutationOwner::RecoverLocked() {
  auto error = CheckCurrent();
  if (error) { return error; }
  // Existing in-process permits stay active across recovery. A failed read or
  // malformed journal never creates a window for raw VFS/package admission.
  const auto names = records_->ListContentMutationFiles(error);
  if (error) { return error; }
  std::set<std::filesystem::path> present(names.begin(), names.end());
  std::map<std::filesystem::path, uint64_t> known_directories;
  preserved_artifacts_.clear();
  error = RecoverPreparations(names,present,known_directories);
  if (error) { return error; }
  for (const auto& name : names) {
    if (!name.string().ends_with(".stream")) { continue; }
    const auto token = records_->CaptureSnapshot(name, wire::kMaximumRecordBytes, error);
    wire::Record record;
    if (!token || !wire::Decode(token->bytes(), record, error)) { return error; }
    if (!record.streamed || !record.stream_pending || name != StreamJournal(record.nonce) ||
        record.identity.uuid != wire_identity_.uuid || record.identity.source != wire_identity_.source ||
        record.identity.device != wire_identity_.device ||
        record.identity.content_directory != wire_identity_.content_directory ||
        record.identity.epoch > wire_identity_.epoch) { return Error(std::errc::state_not_recoverable); }
    if (present.contains(Journal(record.nonce))) { continue; }
    if (present.contains(Journal(record.nonce, true))) {
      const auto done = records_->CaptureSnapshot(Journal(record.nonce, true), 256, error);
      if (!done || !wire::CheckDone(done->bytes(), token->bytes(), error)) { return error; }
      const auto observed = records_->ObserveIntentSnapshot(*done);
      if (observed.error || !observed.durable) { return observed.error ? observed.error : Error(std::errc::io_error); }
      for (const auto& slot : {record.target, record.package}) {
        const auto old = pending_reservations_.find(slot);
        if (old != pending_reservations_.end() && old->second.nonce == record.nonce) {
          old->second.view->ReleaseContentNamespace(); pending_reservations_.erase(old);
        }
      }
      continue;
    }
    if (record.identity != wire_identity_) { return Error(std::errc::state_not_recoverable); }
    std::shared_ptr<HostStorageRoot> reserved, package_reserved;
    if (const auto old = pending_reservations_.find(record.target);
        old != pending_reservations_.end() && old->second.nonce == record.nonce) { reserved = old->second.view; }
    if (const auto old = pending_reservations_.find(record.package);
        old != pending_reservations_.end() && old->second.nonce == record.nonce) { package_reserved = old->second.view; }
    if (!reserved) { reserved = content_->ReserveContentNamespace(record.target, {}, false, error); }
    if (!reserved) { return error; }
    if (!package_reserved) { package_reserved = content_->ReserveContentNamespace(record.package, {}, true, error); }
    if (!package_reserved) { pending_reservations_[record.target] = {record.nonce, reserved}; return error; }
    pending_reservations_[record.target] = {record.nonce, reserved};
    pending_reservations_[record.package] = {record.nonce, package_reserved};
    error = RollbackStream(record, token->bytes(), reserved, package_reserved);
    if (error) { return error; }
  }
  for (const auto& name : names) {
    const auto text = name.string();
    if (text == "owner.v1" || text.ends_with(".stream") || text.ends_with(".plan") ||
        text.ends_with(".abort") || text.ends_with(".keep0") || text.ends_with(".keep1") ||
        text.find(".partial-") != std::string::npos) { continue; }
    if (text.ends_with(".done") || text.ends_with(".retire")) {
      if (!present.contains(Journal(text.substr(8, 32))) &&
          !(text.ends_with(".done") && present.contains(StreamJournal(text.substr(8, 32))))) {
        return Error(std::errc::bad_message);
      }
      continue;
    }
    const auto intent = records_->CaptureSnapshot(name,
        std::max(wire::kMaximumRecordBytes, package_wire::kMaximumRecordBytes), error);
    if (!intent) { return error; }
    constexpr std::string_view package_magic = "XCPKG03\n";
    if (intent->bytes().size() >= package_magic.size() &&
        std::equal(package_magic.begin(), package_magic.end(), intent->bytes().begin())) {
      package_wire::Record package;
      if (!package_wire::Decode(intent->bytes(), package, error)) { return error; }
      if (name != Journal(package.nonce)) { return Error(std::errc::bad_message); }
      error = RecoverPackageIntent(package, intent->bytes(), present, known_directories);
      if (error) { return error; }
      continue;
    }
    if (present.contains("content-" + text.substr(8, 32) + ".retire")) {
      return Error(std::errc::bad_message);
    }
    wire::Record record;
    if (!intent || !wire::Decode(intent->bytes(), record, error)) { return error; }
    if (name != Journal(record.nonce) || record.identity.uuid != wire_identity_.uuid ||
        record.identity.source != wire_identity_.source || record.identity.device != wire_identity_.device ||
        record.identity.content_directory != wire_identity_.content_directory ||
        record.identity.epoch > wire_identity_.epoch) { return Error(std::errc::state_not_recoverable); }
    if (record.stream_pending) { return Error(std::errc::bad_message); }
    if (record.streamed) {
      const auto admitted = records_->CaptureSnapshot(StreamJournal(record.nonce), wire::kMaximumRecordBytes, error);
      wire::Record admission;
      if (!admitted || !admitted->exists() || !wire::Decode(admitted->bytes(), admission, error) ||
          !SameStreamAuthority(admission, record)) {
        return error ? error : Error(std::errc::state_not_recoverable);
      }
    }
    if (record.kind == Kind::kCreate) {
      known_directories.emplace("package-" + record.nonce + ".tmp", record.directory_stage);
    }
    const auto done_name = Journal(record.nonce, true);
    if (present.contains(done_name)) {
      const auto done = records_->CaptureSnapshot(done_name, 256, error);
      if (!done || !wire::CheckDone(done->bytes(), intent->bytes(), error)) { return error; }
      const auto observed = records_->ObserveIntentSnapshot(*done);
      if (observed.error || !observed.durable) {
        return observed.error ? observed.error : Error(std::errc::io_error);
      }
      for (const auto& slot : {record.target, record.package}) {
        const auto pending = pending_reservations_.find(slot);
        if (pending != pending_reservations_.end()) {
          if (pending->second.nonce != record.nonce) { continue; }
          pending->second.view->ReleaseContentNamespace();
          pending_reservations_.erase(pending);
        }
      }
      continue;
    }
    if (record.identity.epoch != wire_identity_.epoch) { return Error(std::errc::state_not_recoverable); }
    std::shared_ptr<HostStorageRoot> package_reserved, reserved;
    if (const auto old = pending_reservations_.find(record.package);
        old != pending_reservations_.end() && old->second.nonce == record.nonce) {
      package_reserved = old->second.view;
    }
    if (const auto old = pending_reservations_.find(record.target);
        old != pending_reservations_.end() && old->second.nonce == record.nonce) {
      reserved = old->second.view;
    }
    if (record.kind != Kind::kCreate && !package_reserved) {
      package_reserved = content_->ReserveContentIo(record.package, error);
      if (!package_reserved) { return error; }
    }
    auto current = (reserved ? reserved : package_reserved ? package_reserved : content_)->CaptureSnapshot(
        record.target, wire::kMaximumMetadataBytes, error, true);
    if (!current) {
      if (package_reserved) { pending_reservations_[record.package] = {record.nonce, package_reserved}; }
      return error;
    }
    if (!reserved) {
      std::optional<FileHandleInfo> info;
      if (current->exists()) { info = current->info(); }
      reserved = (package_reserved ? package_reserved : content_)->ReserveContentNamespace(
          record.target, info, false, error);
    }
    if (!reserved) {
      if (package_reserved) { pending_reservations_[record.package] = {record.nonce, package_reserved}; }
      return error;
    }
    if (record.kind == Kind::kCreate && !package_reserved) {
      std::optional<FileHandleInfo> directory_info;
      auto final = content_->Subroot(record.package, false, error);
      if (final) {
        const auto id = final->IntentRootIdentity(error);
        directory_info = FileHandleInfo{0, 1, true, false, id.filesystem, id.directory};
      } else if (error == std::errc::no_such_file_or_directory) { error.clear(); }
      if (!error) {
        package_reserved = content_->ReserveContentNamespace(record.package, directory_info, true, error);
      }
      if (!package_reserved) {
        pending_reservations_[record.target] = {record.nonce, std::move(reserved)};
        return error;
      }
    }
    const auto result = Advance(record, intent->bytes(), std::move(reserved),
                                std::move(package_reserved), {}, true);
    if (result.error) { return result.error; }
  }
  const auto staged = staging_->ListContentStageDirectories(error);
  if (error) { return error; }
  for (const auto& [leaf, directory] : staged) {
    const auto found = known_directories.find(leaf);
    if (found == known_directories.end() || found->second != directory) {
      // Legacy private setup has no payload ownership proof. Record an exact
      // observation and preserve it opaquely; it may never become a package.
      const auto text = leaf.string();
      if (!text.starts_with("package-") || text.size() != 44) { return Error(std::errc::state_not_recoverable); }
      const auto nonce = text.substr(8,32);
      if (present.contains(Journal(nonce)) || present.contains(StreamJournal(nonce)) ||
          present.contains(PreparationJournal(nonce))) { return Error(std::errc::state_not_recoverable); }
      wire::Preservation ticket{wire_identity_,nonce,false,directory,true,leaf,{}};
      error = PreserveArtifact(ticket,staging_);
      if (error) { return error; }
    }
  }
  if (!pending_reservations_.empty()) {
    return Error(std::errc::state_not_recoverable);
  }
  return {};
}
std::error_code ContentMutationOwner::Recover() {
  if (xe::global_critical_region::is_held_by_current_thread()) { return Error(std::errc::operation_not_permitted); }
  std::lock_guard operation(operation_mutex_); Active active(active_);
  blocked_.store(true, std::memory_order_release);
  const auto error = RecoverLocked();
  blocked_.store(bool(error), std::memory_order_release);
  return error;
}
ContentMutationOwner::FlushReservation::~FlushReservation() {
  view_->ReleaseContentNamespace();
  owner_->active_.fetch_sub(1, std::memory_order_acq_rel);
}
std::unique_ptr<ContentMutationOwner::FlushReservation> ContentMutationOwner::BeginFlush(
    const std::filesystem::path& package, std::error_code& error) {
  if (xe::global_critical_region::is_held_by_current_thread()) {
    error = Error(std::errc::operation_not_permitted); return {};
  }
  std::lock_guard operation(operation_mutex_);
  if (!wire::ValidPackage(package) || !idle()) {
    error = Error(std::errc::device_or_resource_busy); return {};
  }
  error = CheckCurrent();
  if (error) { return {}; }
  auto view = content_->ReserveContentIo(package, error);
  if (!view) { return {}; }
  active_.fetch_add(1, std::memory_order_acq_rel);
  return std::unique_ptr<FlushReservation>(new FlushReservation(
      shared_from_this(), std::move(view), package));
}
std::error_code ContentMutationOwner::Flush(
    FlushReservation& reservation, const std::filesystem::path& header) {
  if (xe::global_critical_region::is_held_by_current_thread()) { return Error(std::errc::operation_not_permitted); }
  std::lock_guard operation(operation_mutex_);
  if (!ready() || reservation.owner_.get() != this) {
    return Error(std::errc::operation_in_progress);
  }
  auto error = CheckCurrent();
  wire::Record slot; slot.kind = Kind::kHeader;
  slot.package = reservation.package_; slot.target = header;
  if (!error && wire::Target(slot).empty()) { error = Error(std::errc::invalid_argument); }
  if (!error) { error = reservation.view_->FlushTree(reservation.package_); }
  if (!error) {
    const auto metadata = content_->CaptureSnapshot(header, wire::kMaximumMetadataBytes, error, true);
    if (!metadata && error == std::errc::no_such_file_or_directory) {
      error.clear();
    } else if (!error && metadata->exists()) {
      const auto observed = content_->ObserveIntentSnapshot(*metadata);
      error = observed.error;
      if (!error && !observed.durable) { error = Error(std::errc::io_error); }
    }
  }
  if (!error) {
    error = content_->FlushDirectory(header.parent_path());
    if (error == std::errc::no_such_file_or_directory) { error.clear(); }
  }
  if (!error) { error = CheckCurrent(); }
  return error;
}
ContentMutationOwner::PackageReceipt ContentMutationOwner::Delete(
    std::unique_ptr<HostStorageContentTree> expected,
    std::shared_ptr<const HostStorageSnapshotToken> header,
    const std::atomic<bool>* canceled) {
  return DeleteInternal(std::move(expected), std::move(header), canceled, false);
}
ContentMutationOwner::PackageReceipt ContentMutationOwner::Truncate(
    std::unique_ptr<HostStorageContentTree> expected,
    std::shared_ptr<const HostStorageSnapshotToken> header,
    const std::atomic<bool>* canceled) {
  return DeleteInternal(std::move(expected), std::move(header), canceled,
                        false, false, package_wire::Kind::kTruncate);
}
ContentMutationOwner::PackageReceipt ContentMutationOwner::DeleteInternal(
    std::unique_ptr<HostStorageContentTree> expected,
    std::shared_ptr<const HostStorageSnapshotToken> header,
    const std::atomic<bool>* canceled, bool stop_after_prepared,
    bool stop_after_retire, package_wire::Kind kind, PackageBoundary boundary) {
  PackageReceipt receipt;
  if (xe::global_critical_region::is_held_by_current_thread()) {
    receipt.error = Error(std::errc::operation_not_permitted); return receipt;
  }
  std::lock_guard lock(operation_mutex_); Active active(active_);
  auto& error = receipt.error;
  error = CheckCurrent();
  if (!error && (!ready() || active_.load(std::memory_order_acquire) != 1)) {
    error = Error(std::errc::device_or_resource_busy);
  }
  package_wire::Record record;
  record.identity = wire_identity_; record.kind = kind;
  if (kind != package_wire::Kind::kDelete && kind != package_wire::Kind::kTruncate) {
    error = Error(std::errc::operation_not_supported);
  }
  if (!error && (!expected || expected->identity().get() != identity_.get() ||
      !header || header->root().get() != content_.get())) {
    error = Error(std::errc::permission_denied);
  }
  if (!error && expected->admission_retired()) { error = Error(std::errc::no_such_device); }
  if (error) { return receipt; }
  record.package = expected->relative(); record.before = expected->entries();
  if (kind == package_wire::Kind::kTruncate) {
    if (record.before.empty() || !record.before.front().directory) {
      error = Error(std::errc::operation_not_supported); return receipt;
    }
    for (const auto& entry : record.before) {
      if (entry.relative.empty() || entry.relative.has_parent_path()) { continue; }
      auto name = entry.relative.string();
      for (auto& c : name) { if (c >= 'A' && c <= 'Z') { c += 'a' - 'A'; } }
      if (name != "__thumbnail.png") { continue; }
      if (entry.directory) { error = Error(std::errc::operation_not_supported); return receipt; }
      record.preserved_leaf = entry.relative;
    }
  }
  record.header_target = header->relative();
  record.header_exists = header->exists();
  record.header_file = header->exists() ? header->info().file_id : 0;
  record.header.assign(header->bytes().begin(), header->bytes().end());
  // Validate package/semantic Header before any private staging or public IO.
  wire::Record semantic;
  semantic.package = record.package; semantic.target = record.header_target;
  semantic.kind = wire::Kind::kHeader;
  if (wire::Target(semantic).empty()) {
    error = Error(std::errc::invalid_argument); return receipt;
  }
  if (canceled && canceled->load(std::memory_order_acquire)) {
    error = Error(std::errc::operation_canceled); return receipt;
  }
  const auto names = records_->ListContentMutationFiles(error);
  if (!error && (maximum_records_ < 6 || names.size() > maximum_records_ - 6)) {
    error = Error(std::errc::no_space_on_device);
  }
  if (error) { return receipt; }
  auto package_view = content_->ReserveContentNamespace(
      record.package, expected->top_info(), true, error);
  if (!package_view) { return receipt; }
  std::optional<FileHandleInfo> header_info;
  if (header->exists()) { header_info = header->info(); }
  auto header_view = package_view->ReserveContentNamespace(
      record.header_target, header_info, false, error);
  auto release = [&] {
    if (header_view) { header_view->ReleaseContentNamespace(); }
    package_view->ReleaseContentNamespace();
  };
  if (!header_view) { release(); return receipt; }
  auto current = header_view->CaptureSnapshot(record.header_target,
      wire::kMaximumMetadataBytes, error, true);
  if (!current || current->exists() != record.header_exists ||
      (current->exists() && (current->info().file_id != record.header_file ||
                            !Equal(current->bytes(), record.header)))) {
    if (!error) { error = Error(std::errc::state_not_recoverable); }
    release(); return receipt;
  }
  // A consumed opaque proof pins real native descriptors/source lease, but
  // cannot leave those unlinked files allocated after completed retirement.
  auto fresh = package_view->CaptureContentTree(record.package, error);
  if (!fresh || fresh->entries() != record.before) {
    if (!error) { error = Error(std::errc::state_not_recoverable); }
    release(); return receipt;
  }
  fresh.reset();
  if (expected->admission_retired()) {
    error = Error(std::errc::no_such_device); release(); return receipt;
  }
  if (canceled && canceled->load(std::memory_order_acquire)) {
    error = Error(std::errc::operation_canceled); release(); return receipt;
  }
  record.nonce = Nonce(); receipt.nonce = record.nonce;
  if (!wire::ValidNonce(record.nonce)) {
    error = Error(std::errc::io_error); release(); return receipt;
  }
  const auto operation_leaf = "package-" + record.nonce + ".tmp";
  error = staging_->Create(operation_leaf, true);
  const bool created = !error;
  auto operation = !error ? staging_->Subroot(operation_leaf, false, error) : nullptr;
  if (operation) {
    record.op_directory = operation->IntentRootIdentity(error).directory;
    if (!error) { error = staging_->FlushDirectory(); }
  }
  std::vector<uint8_t> encoded;
  if (!error) { encoded = package_wire::Encode(record, error); }
  // Cached original guest-generation cancellation is the final pre-Prepared
  // admission boundary. Afterwards the durable obligation owns forward drain;
  // it never recaptures a replacement device by numeric/string ID.
  if (!error && expected->admission_retired()) { error = Error(std::errc::no_such_device); }
  expected.reset();  // Drop all captured payload FDs before actual retirement.
  if (!error) {
    error = AppendRecord(Journal(record.nonce), encoded);
  }
  if (error || stop_after_prepared) {
    if (created) {
      // Retain incomplete private setup/intent and close public admission.
      // A restart must validate the actual typed journal, never adopt by name.
      blocked_.store(true, std::memory_order_release);
      pending_reservations_[record.package] = {record.nonce, package_view};
      pending_reservations_[record.header_target] = {record.nonce, header_view};
    } else { release(); }
    receipt.prepared_durable = !error;
    if (!error) { error = Error(std::errc::operation_in_progress); }
    return receipt;
  }
  // Prepared is irrevocable. Cancellation now joins actual retirement; it
  // cannot manufacture rollback while files may already be quarantined.
  return AdvancePackage(record, encoded, std::move(package_view),
                       std::move(header_view), false, stop_after_retire, boundary);
}
ContentMutationOwner::PackageReceipt ContentMutationOwner::AdvancePackage(
    const package_wire::Record& record, std::span<const uint8_t> encoded,
    std::shared_ptr<HostStorageRoot> package_view,
    std::shared_ptr<HostStorageRoot> header_view, bool recovering,
    bool stop_after_retire, PackageBoundary boundary) {
  PackageReceipt receipt;
  receipt.nonce = record.nonce; receipt.prepared_durable = true;
  auto& error = receipt.error;
  const auto retire_leaf = "content-" + record.nonce + ".retire";
  auto retire = records_->CaptureSnapshot(retire_leaf, 256, error);
  auto operation = !error ? staging_->Subroot("package-" + record.nonce + ".tmp", false, error) : nullptr;
  if (!retire || !operation) { if (!error) { error = Error(std::errc::io_error); } }
  if (!error && operation->IntentRootIdentity(error).directory != record.op_directory) {
    if (!error) { error = Error(std::errc::state_not_recoverable); }
  }
  if (!error && retire->exists() &&
      !package_wire::CheckRetire(retire->bytes(), encoded, error)) {
    if (!error) { error = Error(std::errc::bad_message); }
  }
  if (!error && !retire->exists()) {
    error = record.kind == package_wire::Kind::kDelete
        ? package_view->QuarantineContentPackage(record, *operation)
        : package_view->QuarantineContentTruncate(record, *operation);
    receipt.quarantined = !error;
  } else if (!error) {
    const auto observed = records_->ObserveIntentSnapshot(*retire);
    error = observed.error;
    if (!error && !observed.durable) { error = Error(std::errc::io_error); }
    receipt.retirement_durable = !error;
  }
  if (!error && !retire->exists() && boundary == PackageBoundary::kQuarantined) {
    error = Error(std::errc::operation_in_progress);
  }
  if (!error && record.kind == package_wire::Kind::kTruncate) {
    auto header = header_view->CaptureSnapshot(record.header_target,
        wire::kMaximumMetadataBytes, error, true);
    if (!header || header->exists() != record.header_exists ||
        (header->exists() && (header->info().file_id != record.header_file ||
                             !Equal(header->bytes(), record.header)))) {
      if (!error) { error = Error(std::errc::state_not_recoverable); }
    } else if (header->exists()) {
      const auto observed = header_view->ObserveIntentSnapshot(*header);
      error = observed.error;
      if (!error && !observed.durable) { error = Error(std::errc::io_error); }
    } else { error = header_view->FlushDirectory(record.header_target.parent_path()); }
  } else if (!error) {
    auto header = header_view->CaptureSnapshot(record.header_target,
        wire::kMaximumMetadataBytes, error, true);
    if (!header) { if (!error) { error = Error(std::errc::io_error); } }
    else if (header->exists()) {
      if (!record.header_exists || header->info().file_id != record.header_file ||
          !Equal(header->bytes(), record.header)) {
        error = Error(std::errc::state_not_recoverable);
      } else {
        // The common Delete ABI deliberately accepts a flat token. Retain a
        // genuine subroot of the reserved Header parent, then reload that same
        // exact inode/raw bytes; never relax its flat-file authority contract.
        auto parent = header_view->Subroot(record.header_target.parent_path(), false, error);
        auto flat = parent ? parent->CaptureSnapshot(record.header_target.filename(),
            wire::kMaximumMetadataBytes, error, true) : nullptr;
        if (!flat || !flat->exists() || flat->info().file_id != record.header_file ||
            !Equal(flat->bytes(), record.header)) {
          if (!error) { error = Error(std::errc::state_not_recoverable); }
        }
        if (!error) {
          const auto deleted = SnapshotPublisher::Delete(*recovery_, flat);
          error = deleted.error;
          if (!error && (!deleted.published || !deleted.durable)) { error = Error(std::errc::io_error); }
          receipt.header_removed = deleted.published;
        }
      }
    } else {
      // Journal authority, not an ordinary absent token, authenticates this
      // requested absent end state. This is observing, never fresh CAS success.
      error = header_view->FlushDirectory(record.header_target.parent_path());
      receipt.observed = !error;
    }
  }
  if (!error && !retire->exists() && boundary == PackageBoundary::kHeaderCommitted) {
    error = Error(std::errc::operation_in_progress);
  }
  if (!error && !retire->exists()) {
    const auto bytes = package_wire::Retire(encoded, error);
    if (!error) {
      error = AppendRecord(retire_leaf, bytes);
    }
    if (!error) { retire = records_->CaptureSnapshot(retire_leaf, 256, error); }
    if (!error && (!retire || !retire->exists() ||
        !package_wire::CheckRetire(retire->bytes(), encoded, error))) {
      if (!error) { error = Error(std::errc::bad_message); }
    }
    receipt.retirement_durable = !error;
  }
  // Existing test friend may stop at this REAL durable boundary. The same
  // native quarantine, Header CAS and Retire write/verification ran above;
  // this never fabricates a marker, partial unlink or successful completion.
  if (!error && stop_after_retire) { error = Error(std::errc::operation_in_progress); }
  if (!error) {
    error = record.kind == package_wire::Kind::kDelete
        ? package_view->RetireContentPackage(record, *operation)
        : package_view->RetireContentTruncate(record, *operation);
    receipt.space_retired = !error;
  }
  if (!error) {
    const auto observed = staging_->ObserveContentDirectory(
        "package-" + record.nonce + ".tmp", record.op_directory);
    error = observed.error;
    if (!error && !observed.durable) { error = Error(std::errc::io_error); }
  }
  if (!error) {
    const auto bytes = package_wire::Done(encoded, retire->bytes(), error);
    if (!error) {
      error = AppendRecord(Journal(record.nonce, true), bytes);
    }
  }
  if (!error) { error = CheckCurrent(); }
  receipt.observed = receipt.observed || recovering;
  receipt.complete = !error;
  if (receipt.complete) {
    package_view->ReleaseContentNamespace(); header_view->ReleaseContentNamespace();
    pending_reservations_.erase(record.package);
    pending_reservations_.erase(record.header_target);
  } else {
    blocked_.store(true, std::memory_order_release);
    pending_reservations_[record.package] = {record.nonce, std::move(package_view)};
    pending_reservations_[record.header_target] = {record.nonce, std::move(header_view)};
  }
  return receipt;
}
std::error_code ContentMutationOwner::RecoverPackageIntent(
    const package_wire::Record& record, std::span<const uint8_t> encoded,
    const std::set<std::filesystem::path>& present,
    std::map<std::filesystem::path, uint64_t>& known_directories) {
  if (record.identity.uuid != wire_identity_.uuid ||
      record.identity.source != wire_identity_.source ||
      record.identity.device != wire_identity_.device ||
      record.identity.content_directory != wire_identity_.content_directory ||
      record.identity.epoch > wire_identity_.epoch ||
      (record.kind != package_wire::Kind::kDelete &&
       (record.kind != package_wire::Kind::kTruncate || record.before.empty() ||
        !record.before.front().directory))) {
    return Error(std::errc::state_not_recoverable);
  }
  known_directories.emplace("package-" + record.nonce + ".tmp", record.op_directory);
  const auto retire_name = "content-" + record.nonce + ".retire";
  if (present.contains(Journal(record.nonce, true))) {
    if (!present.contains(retire_name)) { return Error(std::errc::bad_message); }
    std::error_code error;
    auto retire = records_->CaptureSnapshot(retire_name, 256, error);
    auto done = !error ? records_->CaptureSnapshot(Journal(record.nonce, true), 256, error) : nullptr;
    if (!retire || !done || !package_wire::CheckDone(done->bytes(), encoded, retire->bytes(), error)) {
      return error ? error : Error(std::errc::bad_message);
    }
    const auto observed = records_->ObserveIntentSnapshot(*done);
    if (observed.error || !observed.durable) { return observed.error ? observed.error : Error(std::errc::io_error); }
    const auto empty_operation = staging_->ObserveContentDirectory(
        "package-" + record.nonce + ".tmp", record.op_directory);
    if (empty_operation.error || !empty_operation.durable) {
      return empty_operation.error ? empty_operation.error : Error(std::errc::io_error);
    }
    for (const auto& slot : {record.package, record.header_target}) {
      auto pending = pending_reservations_.find(slot);
      if (pending != pending_reservations_.end() && pending->second.nonce == record.nonce) {
        pending->second.view->ReleaseContentNamespace(); pending_reservations_.erase(pending);
      }
    }
    return {};
  }
  if (record.identity.epoch != wire_identity_.epoch) { return Error(std::errc::state_not_recoverable); }
  std::error_code error;
  std::shared_ptr<HostStorageRoot> package_view, header_view;
  for (const auto& [slot, pending] : pending_reservations_) {
    if (pending.nonce != record.nonce) { continue; }
    if (slot == record.package) { package_view = pending.view; }
    if (slot == record.header_target) { header_view = pending.view; }
  }
  if (!package_view) {
    std::optional<FileHandleInfo> info;
    auto tree = content_->CaptureContentTree(record.package, error);
    if (tree) { info = tree->top_info(); }
    else if (error == std::errc::no_such_file_or_directory) { error.clear(); }
    tree.reset();
    if (!error) { package_view = content_->ReserveContentNamespace(record.package, info, true, error); }
    if (!package_view) { return error; }
  }
  if (!header_view) {
    auto current = package_view->CaptureSnapshot(record.header_target,
        wire::kMaximumMetadataBytes, error, true);
    std::optional<FileHandleInfo> info;
    if (!current && !error) { error = Error(std::errc::io_error); }
    if (current && current->exists()) { info = current->info(); }
    if (!error) { header_view = package_view->ReserveContentNamespace(record.header_target, info, false, error); }
    if (!header_view) {
      pending_reservations_[record.package] = {record.nonce, package_view}; return error;
    }
  }
  return AdvancePackage(record, encoded, std::move(package_view), std::move(header_view), true).error;
}

}  // namespace xe::filesystem
