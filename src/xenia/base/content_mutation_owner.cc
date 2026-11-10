/**
 * Xenia : Xbox 360 Emulator Research Project
 * Released under the BSD license - see LICENSE in the root for details.
 */
#include "xenia/base/content_mutation_owner.h"
#include <algorithm>
#include <array>
#include <random>
#include <set>
#include "xenia/base/mutex.h"
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
struct Active {
  std::atomic<uint64_t>& value;
  explicit Active(std::atomic<uint64_t>& v) : value(v) { value.fetch_add(1, std::memory_order_acq_rel); }
  ~Active() { value.fetch_sub(1, std::memory_order_acq_rel); }
};
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
    bool initialize) {
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
  error = out->identity_->CheckCurrent();
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
    } else { out->staging_ = std::move(child); }
    if (error) { return out; }
  }
  std::vector<uint8_t> marker{'X','C','M','O','W','N','1','\n'};
  marker.insert(marker.end(), out->identity_->uuid().begin(), out->identity_->uuid().end());
  Number(marker, out->identity_->directory_file_id());
  Number(marker, out->identity_->device_id());
  const auto& source = out->identity_->source_id();
  Number(marker, source.size()); marker.insert(marker.end(), source.begin(), source.end());
  for (const auto& owned : {root, out->records_, out->staging_}) {
    const auto id = owned->IntentRootIdentity(error);
    if (error) { return out; } Number(marker, id.directory);
  }
  auto digest = wire::Digest(marker, error);
  if (error) { return out; }
  marker.insert(marker.end(), digest.begin(), digest.end());
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
  if (!error) { error = out->RecoverLocked(); }
  out->blocked_.store(bool(error), std::memory_order_release);
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
    if (!error && (kind!=Kind::kCreate || expected->exists())) { error=Error(std::errc::file_exists); }
    if (error) { return receipt; }
    uint64_t bound=uint64_t(bytes.size())*2+131072;
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
  if (maximum_records_ < 2 || records.size() > maximum_records_ - 2) {
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
  if (kind == Kind::kCreate) {
    error = EnsureParents(package.parent_path());
    if (!error) { package_reservation = content_->ReserveContentNamespace(package, {}, true, error); }
    if (!package_reservation) { release(); return receipt; }
    const auto leaf = "package-" + record.nonce + ".tmp";
    error = staging_->Create(leaf, true);
    const bool directory_created = !error;
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
        // Actual post-member boundary models loss before durable Prepared. Such
        // a private orphan is preserved/refused, never guessed into authority.
        if (!error && stop_boundary==4) { error=Error(std::errc::operation_canceled); }
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
      if (directory_created) {
        blocked_.store(true, std::memory_order_release);
        pending_reservations_[record.target] = {record.nonce, reserved};
        pending_reservations_[record.package] = {record.nonce, package_reservation};
      } else { release(); }
      return receipt;
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
    if (staged_directory || receipt.metadata.stage_created || receipt.metadata.recovery_created) {
      // Retain uncertain, unrecorded staging. It is not a completed operation
      // and cannot be recovered by guessing intended bytes after the fact.
      blocked_.store(true, std::memory_order_release);
      pending_reservations_[record.target] = {record.nonce, reserved};
      if (package_reservation) { pending_reservations_[record.package] = {record.nonce, package_reservation}; }
    } else { release(); }
    return receipt;
  }
  record.before_exists = current->exists();
  record.before_file = current->exists() ? current->info().file_id : 0;
  record.before.assign(current->bytes().begin(), current->bytes().end());
  record.intended.assign(prepared->intended().begin(), prepared->intended().end());
  record.stage = prepared->receipt().stage;
  record.stage_file = prepared->stage_info().file_id;
  record.recovery = prepared->receipt().recovery;
  if (record.before_exists) { record.recovery_file = prepared->recovery_->info().file_id; }
  const auto encoded = wire::Encode(record, error);
  if (!error) {
    const auto written = records_->WriteNewFile(Journal(record.nonce), encoded);
    error = written.error;
    if (!error && written.bytes != encoded.size()) { error = Error(std::errc::io_error); }
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
  return Advance(record, encoded, std::move(reserved), std::move(package_reservation),
                 std::move(prepared), false,stop_boundary);
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
      const auto written = records_->WriteNewFile(Journal(record.nonce, true), done);
      error = written.error;
      if (!error && written.bytes != done.size()) { error = Error(std::errc::io_error); }
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
  for (const auto& name : names) {
    const auto text = name.string();
    if (text == "owner.v1") { continue; }
    if (text.ends_with(".done") || text.ends_with(".retire")) {
      if (!present.contains(Journal(text.substr(8, 32)))) { return Error(std::errc::bad_message); }
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
      // No durable intent authenticates this orphan. Preserve it privately,
      // never adopt/delete it or publish a success-shaped empty package.
      return Error(std::errc::state_not_recoverable);
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
  if (!error && (maximum_records_ < 3 || names.size() > maximum_records_ - 3)) {
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
    const auto written = records_->WriteNewFile(Journal(record.nonce), encoded);
    error = written.error;
    if (!error && written.bytes != encoded.size()) { error = Error(std::errc::io_error); }
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
      const auto written = records_->WriteNewFile(retire_leaf, bytes);
      error = written.error;
      if (!error && written.bytes != bytes.size()) { error = Error(std::errc::io_error); }
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
      const auto written = records_->WriteNewFile(Journal(record.nonce, true), bytes);
      error = written.error;
      if (!error && written.bytes != bytes.size()) { error = Error(std::errc::io_error); }
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
