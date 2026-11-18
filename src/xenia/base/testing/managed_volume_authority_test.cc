/** Xenia : Xbox 360 Emulator Research Project; BSD license, see LICENSE. */
#include "xenia/base/managed_volume_authority.h"
#include "xenia/base/managed_storage_bootstrap.h"
#include "xenia/base/content_mutation_owner.h"
#include <algorithm>
#include <array>
#include <fstream>
#include <iterator>
#include <map>
#include <random>
#include <set>
#include "third_party/catch/include/catch.hpp"
#include "third_party/mbedtls/include/mbedtls/sha256.h"
#if XE_PLATFORM_LINUX || XE_PLATFORM_MAC
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
namespace xe::filesystem {
class ContentMutationOwnerTestAccess final {
 public:
  static std::filesystem::path RecordDirectory(const ContentMutationOwner& owner) { return owner.records_->path(); }
  static std::filesystem::path StageDirectory(const ContentMutationOwner& owner) { return owner.staging_->path(); }
  static std::filesystem::path RecoveryDirectory(const ContentMutationOwner& owner) { return owner.recovery_->root()->path(); }
  static content_mutation_wire::Preparation ReadPreparation(const ContentMutationOwner& owner, const std::string& nonce) {
    std::error_code error;
    const auto token = owner.records_->CaptureSnapshot("content-"+nonce+".plan",4096,error);
    REQUIRE(token); REQUIRE_FALSE(error); REQUIRE(token->exists());
    content_mutation_wire::Preparation plan;
    REQUIRE(content_mutation_wire::DecodePreparation(token->bytes(),plan,error)); REQUIRE_FALSE(error); return plan;
  }
  static void RequirePreparationBinding(const ContentMutationOwner& owner,
      const content_mutation_wire::Record& record) {
    std::error_code error;
    const auto token = owner.records_->CaptureSnapshot(
        "content-" + record.nonce + ".plan", 4096, error);
    REQUIRE(token); REQUIRE_FALSE(error); REQUIRE(token->exists());
    content_mutation_wire::Preparation plan;
    REQUIRE(content_mutation_wire::DecodePreparation(token->bytes(), plan, error));
    REQUIRE_FALSE(error);
    REQUIRE(record.preparation_digest == content_mutation_wire::Digest(token->bytes(), error));
    REQUIRE_FALSE(error);
    REQUIRE(content_mutation_wire::MatchesPreparation(plan, token->bytes(), record, error));
    REQUIRE_FALSE(error);
    // A different digest cannot authenticate the same source, package, Header
    // and intended metadata, even when all the other record fields match.
    auto mismatched = record; mismatched.preparation_digest[0] ^= 1;
    REQUIRE_FALSE(content_mutation_wire::MatchesPreparation(
        plan, token->bytes(), mismatched, error));
    REQUIRE_FALSE(error);
  }
  static void RequireAborted(const ContentMutationOwner& owner, const std::string& nonce) {
    std::error_code error;
    const auto plan = owner.records_->CaptureSnapshot("content-"+nonce+".plan",4096,error);
    REQUIRE(plan); REQUIRE_FALSE(error); REQUIRE(plan->exists());
    const auto terminal = owner.records_->CaptureSnapshot("content-"+nonce+".abort",256,error);
    REQUIRE(terminal); REQUIRE_FALSE(error); REQUIRE(terminal->exists());
    REQUIRE(content_mutation_wire::CheckDone(terminal->bytes(),plan->bytes(),error)); REQUIRE_FALSE(error);
    for (const auto* suffix : {".intent",".stream",".done"}) {
      const auto token = owner.records_->CaptureSnapshot("content-"+nonce+suffix,4096,error);
      REQUIRE(token); REQUIRE_FALSE(error); REQUIRE_FALSE(token->exists());
    }
  }
  static void StopRecord(ContentMutationOwner& owner, std::string suffix, unsigned boundary) {
    owner.record_stop_suffix_ = std::move(suffix); owner.record_stop_boundary_ = boundary;
  }
  static void DenyPreparationSpace(ContentMutationOwner& owner, bool deny) {
    owner.preparation_space_ceiling_ = deny ? std::optional<uint64_t>(0) : std::nullopt;
  }
  static ContentMutationOwner::Receipt StopStreamPreparation(ContentMutationOwner& owner,
      const std::filesystem::path& package, std::shared_ptr<const HostStorageSnapshotToken> header,
      std::span<const uint8_t> metadata, ContentPackageStreamSource& source, unsigned boundary) {
    return owner.StreamInternal(package,std::move(header),metadata,source,nullptr,nullptr,UINT64_MAX,{},nullptr,boundary);
  }
  static std::error_code StopPreservation(ContentMutationOwner& owner, const std::string& nonce, unsigned boundary) {
    const auto plan = ReadPreparation(owner,nonce); std::error_code error;
    const auto encoded = owner.records_->CaptureSnapshot("content-"+nonce+".plan",4096,error);
    REQUIRE(encoded); REQUIRE_FALSE(error);
    return owner.AbortPreparation(plan,encoded->bytes(),owner.pending_reservations_.at(plan.target).view,
                                 owner.pending_reservations_.at(plan.package).view,boundary);
  }
  static ContentMutationOwner::Receipt UncertainInitialStage(ContentMutationOwner& owner,
      const std::filesystem::path& package,std::shared_ptr<const HostStorageSnapshotToken> header,
      std::span<const uint8_t> metadata,std::span<const ContentPackageInitialMember> members) {
    return owner.Mutate(ContentMutationOwner::Kind::kCreate,package,std::move(header),metadata,nullptr,
                         std::make_error_code(std::errc::io_error),members);
  }
  static ContentMutationOwner::Receipt CancelStreamAfterAuthentication(
      ContentMutationOwner& owner, const std::filesystem::path& package,
      std::shared_ptr<const HostStorageSnapshotToken> header,
      std::span<const uint8_t> metadata, ContentPackageStreamSource& source,
      std::atomic<bool>& canceled, std::atomic<uint64_t>& progress) {
    return owner.StreamInternal(package, std::move(header), metadata, source,
        &canceled, &progress, UINT64_MAX, {}, &canceled);
  }
  static ContentMutationOwner::Receipt StopStream(ContentMutationOwner& owner,
      const std::filesystem::path& package,
      std::shared_ptr<const HostStorageSnapshotToken> header,
      std::span<const uint8_t> metadata, ContentPackageStreamSource& source,
      std::atomic<uint64_t>& progress, uint64_t after) {
    return owner.StreamInternal(package, std::move(header), metadata, source,
                                nullptr, &progress, after);
  }
  static ContentMutationOwner::Receipt DenyStreamSpace(ContentMutationOwner& owner,
      const std::filesystem::path& package,
      std::shared_ptr<const HostStorageSnapshotToken> header,
      std::span<const uint8_t> metadata, ContentPackageStreamSource& source) {
    return owner.StreamInternal(package, std::move(header), metadata, source,
                                nullptr, nullptr, UINT64_MAX, uint64_t(0));
  }
  static content_mutation_wire::Record StreamRecord(
      const ContentMutationOwner& owner, const std::string& nonce) {
    std::error_code error;
    auto token = owner.records_->CaptureSnapshot("content-" + nonce + ".stream",
        content_mutation_wire::kMaximumRecordBytes, error);
    REQUIRE(token); REQUIRE_FALSE(error); REQUIRE(token->exists());
    content_mutation_wire::Record record;
    REQUIRE(content_mutation_wire::Decode(token->bytes(), record, error));
    REQUIRE_FALSE(error); return record;
  }
  static void RequireStreamRollbackDone(const ContentMutationOwner& owner,
                                        const std::string& nonce) {
    std::error_code error;
    auto start = owner.records_->CaptureSnapshot("content-" + nonce + ".stream",
        content_mutation_wire::kMaximumRecordBytes, error);
    REQUIRE(start); REQUIRE_FALSE(error); REQUIRE(start->exists());
    auto done = owner.records_->CaptureSnapshot("content-" + nonce + ".done", 256, error);
    REQUIRE(done); REQUIRE_FALSE(error); REQUIRE(done->exists());
    REQUIRE(content_mutation_wire::CheckDone(done->bytes(), start->bytes(), error));
    REQUIRE_FALSE(error);
    const auto empty = std::filesystem::is_empty(owner.staging_->path(), error);
    REQUIRE_FALSE(error);
    REQUIRE(empty);
  }
  static ContentMutationOwner::Receipt StopInitial(
      ContentMutationOwner& owner,const std::filesystem::path& package,
      std::shared_ptr<const HostStorageSnapshotToken> header,
      std::span<const uint8_t> bytes,
      std::span<const ContentPackageInitialMember> members,unsigned boundary) {
    return owner.Mutate(ContentMutationOwner::Kind::kCreate,package,std::move(header),
                        bytes,nullptr,{},members,boundary);
  }

  static ContentMutationOwner::Receipt DenyInitialSpace(
      ContentMutationOwner& owner,const std::filesystem::path& package,
      std::shared_ptr<const HostStorageSnapshotToken> header,
      std::span<const uint8_t> bytes,
      std::span<const ContentPackageInitialMember> members) {
    // Real native owner and Observe still run. Only this operation's usable
    // ceiling is reduced to zero; no capacity/authority or successful IO is
    // fabricated, and no global reserve/query is modified.
    return owner.Mutate(ContentMutationOwner::Kind::kCreate,package,
        std::move(header),bytes,nullptr,{},members,0,uint64_t(0));
  }
  static ContentMutationOwner::PackageReceipt PreparePackageDelete(
      ContentMutationOwner& owner, std::unique_ptr<HostStorageContentTree> tree,
      std::shared_ptr<const HostStorageSnapshotToken> header) {
    // Real production preparation: durable operation directory + typed intent,
    // no public rename/header removal/retirement. Models the exact restart gap.
    return owner.DeleteInternal(std::move(tree), std::move(header), nullptr, true);
  }
  static ContentMutationOwner::PackageReceipt StopPackageDeleteBeforeRetire(
      ContentMutationOwner& owner, std::unique_ptr<HostStorageContentTree> tree,
      std::shared_ptr<const HostStorageSnapshotToken> header, bool header_committed) {
    return owner.DeleteInternal(std::move(tree), std::move(header), nullptr, false, false,
        content_package_mutation_wire::Kind::kDelete,
        header_committed ? ContentMutationOwner::PackageBoundary::kHeaderCommitted
                         : ContentMutationOwner::PackageBoundary::kQuarantined);
  }
  static void NoRetireOrDone(const ContentMutationOwner& owner, const std::string& nonce) {
    std::error_code error;
    for (const auto* suffix : {".retire", ".done"}) {
      auto token = owner.records_->CaptureSnapshot("content-" + nonce + suffix, 256, error);
      REQUIRE(token); REQUIRE_FALSE(error); REQUIRE_FALSE(token->exists());
    }
  }
  static ContentMutationOwner::PackageReceipt PreparePackageTruncate(
      ContentMutationOwner& owner, std::unique_ptr<HostStorageContentTree> tree,
      std::shared_ptr<const HostStorageSnapshotToken> header) {
    return owner.DeleteInternal(std::move(tree), std::move(header), nullptr, true, false,
                                content_package_mutation_wire::Kind::kTruncate);
  }
  static void MoveActualTruncateMember(const ContentMutationOwner& owner,
      const content_package_mutation_wire::Record& record,
      const std::filesystem::path& member) {
    REQUIRE_FALSE(member.empty()); REQUIRE_FALSE(member.has_parent_path());
    const auto found = std::find_if(record.before.begin(), record.before.end(),
        [&](const auto& row) { return row.relative == member; });
    REQUIRE(found != record.before.end());
    const auto source = owner.content_->path() / record.package / member;
    auto operation = PackageOperation(owner, record.nonce);
    {
      auto actual = FileHandle::OpenExisting(source, FileAccess::kFileReadData);
      REQUIRE(actual); std::error_code error; const auto info = actual->QueryInfo(error);
      REQUIRE(info); REQUIRE_FALSE(error); REQUIRE(info->file_id == found->file);
    }
    // This exact owned fixture rename models process loss between two real
    // production top-member moves. It cannot invent journal/identity authority.
    std::filesystem::rename(source, operation->path() / member);
    REQUIRE_FALSE(operation->FlushDirectory());
    const auto held = owner.pending_reservations_.find(record.package);
    REQUIRE(held != owner.pending_reservations_.end());
    REQUIRE_FALSE(held->second.view->FlushDirectory(record.package));
  }
  static ContentMutationOwner::PackageReceipt StopPackageTruncateAtRetire(
      ContentMutationOwner& owner, std::unique_ptr<HostStorageContentTree> tree,
      std::shared_ptr<const HostStorageSnapshotToken> header) {
    return owner.DeleteInternal(std::move(tree), std::move(header), nullptr, false, true,
                                content_package_mutation_wire::Kind::kTruncate);
  }
  static ContentMutationOwner::PackageReceipt StopPackageDeleteAtRetire(
      ContentMutationOwner& owner, std::unique_ptr<HostStorageContentTree> tree,
      std::shared_ptr<const HostStorageSnapshotToken> header) {
    return owner.DeleteInternal(std::move(tree), std::move(header), nullptr, false, true);
  }
  static std::shared_ptr<HostStorageRoot> PackageOperation(
      const ContentMutationOwner& owner, const std::string& nonce) {
    std::error_code error;
    auto operation = owner.staging_->Subroot("package-" + nonce + ".tmp", false, error);
    REQUIRE(operation); REQUIRE_FALSE(error); return operation;
  }
  static void RetireActualMember(const ContentMutationOwner& owner,
      const content_package_mutation_wire::Record& record,
      const std::filesystem::path& member, const std::filesystem::path& prefix = "payload") {
    auto operation = PackageOperation(owner, record.nonce);
    const auto found = std::find_if(record.before.begin(), record.before.end(),
        [&](const auto& row) { return row.relative == member; });
    REQUIRE(found != record.before.end()); REQUIRE_FALSE(found->directory);
    std::error_code error;
    auto token = operation->CaptureSnapshot(prefix / member, 1024, error);
    REQUIRE(token); REQUIRE_FALSE(error); REQUIRE(token->exists());
    REQUIRE(token->info().file_id == found->file);
    REQUIRE_FALSE(operation->Remove(prefix / member, false));
    REQUIRE_FALSE(operation->FlushDirectory(prefix / member.parent_path()));
  }
  static content_package_mutation_wire::Record PackageRecord(
      const ContentMutationOwner& owner, const std::string& nonce) {
    std::error_code error;
    auto token = owner.records_->CaptureSnapshot("content-" + nonce + ".intent",
        content_package_mutation_wire::kMaximumRecordBytes, error);
    REQUIRE(token); REQUIRE_FALSE(error); REQUIRE(token->exists());
    content_package_mutation_wire::Record record;
    REQUIRE(content_package_mutation_wire::Decode(token->bytes(), record, error));
    REQUIRE_FALSE(error); return record;
  }
  static std::set<std::string> InitializationEntries(
      const ManagedDeviceIdentity::Uuid& identity) {
    constexpr char digits[] = "0123456789abcdef";
    std::string uuid;
    for (uint8_t byte : identity) {
      uuid += digits[byte >> 4]; uuid += digits[byte & 15];
    }
    const auto root = ".xenia-content-mutations-v1-" + uuid;
    return {root, root + "/records", root + "/staging",
            root + "/records/owner.v1", "content-mutations-v1-" + uuid + ".owner"};
  }
  static std::set<std::string> InitializationEntries(
      const ContentMutationOwner& owner, const std::filesystem::path& parent) {
    const auto names = InitializationEntries(owner.identity_->uuid());
    const auto root = *names.begin();
    REQUIRE(owner.parent_->path() == parent);
    REQUIRE(owner.records_->path() == parent / root / "records");
    REQUIRE(owner.staging_->path() == parent / root / "staging");
    return names;
  }
  static ContentMutationOwner::Receipt UncertainStageParent(
      ContentMutationOwner& owner, const std::filesystem::path& package,
      std::shared_ptr<const HostStorageSnapshotToken> expected,
      std::span<const uint8_t> bytes) {
    // Exercise the real serialized owner, real file write and both real fsyncs.
    // Only the returned parent-barrier certainty is faulted; no success mock.
    return owner.Mutate(ContentMutationOwner::Kind::kHeader, package,
        std::move(expected), bytes, nullptr,
        std::make_error_code(std::errc::io_error));
  }
  static void NoJournal(const ContentMutationOwner& owner,
                        const std::string& nonce) {
    std::error_code error;
    for (const auto* suffix : {".intent", ".done"}) {
      auto token = owner.records_->CaptureSnapshot(
          "content-" + nonce + suffix,
          content_mutation_wire::kMaximumRecordBytes, error);
      REQUIRE(token); REQUIRE_FALSE(error); REQUIRE_FALSE(token->exists());
    }
  }
  static void NoCompleteIntent(const ContentMutationOwner& owner,
                               const std::string& nonce) {
    std::error_code error;
    const auto token = owner.records_->CaptureSnapshot(
        "content-" + nonce + ".intent",
        content_mutation_wire::kMaximumRecordBytes, error);
    REQUIRE(token); REQUIRE_FALSE(error); REQUIRE_FALSE(token->exists());
  }
  static std::vector<uint8_t> PendingBytes(const ContentMutationOwner& owner,
      const std::filesystem::path& target, const std::filesystem::path& file) {
    const auto found = owner.pending_reservations_.find(target);
    REQUIRE(found != owner.pending_reservations_.end());
    std::error_code error;
    auto token = found->second.view->CaptureSnapshot(
        file, content_mutation_wire::kMaximumMetadataBytes, error);
    REQUIRE(token); REQUIRE_FALSE(error); REQUIRE(token->exists());
    return {token->bytes().begin(), token->bytes().end()};
  }
  static std::vector<uint8_t> RecoveryMarker(const ContentMutationOwner& owner) {
    REQUIRE_FALSE(owner.recovery_->CheckTarget(*owner.content_));
    std::vector<uint8_t> marker;
    REQUIRE_FALSE(owner.recovery_->root()->ReadFile(
        SnapshotRecoveryOwner::kMarker, 256, marker));
    REQUIRE_FALSE(marker.empty());
    return marker;
  }
  static void LimitRecords(ContentMutationOwner& owner, size_t maximum) {
    REQUIRE(maximum >= 3); REQUIRE(maximum <= 8192);
    owner.maximum_records_ = maximum;
  }
  static content_mutation_wire::Record ReadRecord(
      const ContentMutationOwner& owner, const std::string& nonce) {
    std::error_code error;
    const auto token = owner.records_->CaptureSnapshot(
        "content-" + nonce + ".intent", content_mutation_wire::kMaximumRecordBytes, error);
    REQUIRE(token); REQUIRE_FALSE(error); REQUIRE(token->exists());
    content_mutation_wire::Record record;
    REQUIRE(content_mutation_wire::Decode(token->bytes(), record, error));
    REQUIRE_FALSE(error); return record;
  }
  static void LoseDone(const ContentMutationOwner& owner, const std::string& nonce) {
    // Deliberately emulate the real post-publication/pre-Done restart boundary
    // using only this fixture's exact owned journal; never a guest operation.
    const auto leaf = "content-" + nonce + ".done";
    REQUIRE(std::filesystem::remove(owner.records_->path() / leaf));
    REQUIRE_FALSE(owner.records_->FlushDirectory());
  }
  static std::vector<uint8_t> Backup(const ContentMutationOwner& owner,
                                    const content_mutation_wire::Record& record) {
    std::vector<uint8_t> bytes;
    REQUIRE_FALSE(owner.recovery_->root()->ReadFile(record.recovery,
        content_mutation_wire::kMaximumMetadataBytes, bytes));
    return bytes;
  }
};
// Stop at real durable producer boundaries. No native IO result, drain check or
// root identity is mocked; restart below reopens the actual files noncreating.
class ManagedVolumeAuthorityTestAccess final {
 public:
  static std::shared_ptr<const ManagedVolumeAuthority> OpenLegacyProducer(
      ManagedVolumeAuthority::Options options) {
    auto result = std::shared_ptr<ManagedVolumeAuthority>(new ManagedVolumeAuthority);
    auto anchor = options.content;
    std::error_code error;
    result->content_ = anchor->Subroot({}, false, error);
    REQUIRE(result->content_); REQUIRE_FALSE(error);
    // Invoke the genuine existing normal B2 producer, including its real MDI,
    // profile/recovery/mutation factories and final EncodeBinding commit.
    // No modern bootstrap skip flag, hand-built wire or fabricated lease.
    return ManagedVolumeAuthority::OpenLegacyInternal(std::move(options),
        std::move(anchor), std::move(result), false);
  }
  static std::filesystem::path LegacyBindingPath(const ManagedVolumeAuthority& owner) {
    return owner.binding_root_ ? owner.binding_root_->path() : std::filesystem::path{};
  }
  static std::shared_ptr<const ManagedVolumeAuthority> StopBootstrap(
      ManagedVolumeAuthority::Options options,unsigned boundary) {
    return ManagedVolumeAuthority::OpenInternal(std::move(options),false,boundary);
  }
  static unsigned BootstrapBoundaries(const ManagedVolumeAuthority& volume) {
    return ManagedStorageBootstrap::Boundaries(volume);
  }
  static bool HasExactSourceAnchor(const ManagedVolumeAuthority& volume) {
    if (!volume.state_parent_ || !volume.content_ || !volume.source_lease_ ||
        volume.state_parent_.get()==volume.content_.get() ||
        volume.state_parent_->path()!=volume.content_->path() ||
        volume.state_parent_->format_eligible() ||
        !volume.state_parent_->native_filesystem_authority() ||
        !volume.content_->native_filesystem_authority()) {return false;}
    const auto& anchor=volume.state_parent_->directory_identity();
    const auto& source=volume.content_->directory_identity();
    return anchor && source && anchor->SameEntry({},*source,{}) &&
        volume.intents_ && volume.intents_->source()==volume.content_ &&
        !volume.identity_->CheckCurrent() && !volume.intents_->error();
  }
  static std::filesystem::path PublicAuthorityPath(const ManagedVolumeAuthority& volume) {
    return volume.public_authority_?volume.public_authority_->path():std::filesystem::path{};
  }
  static std::vector<uint8_t> WrongBindingTag(std::span<const uint8_t> wire) {
    std::error_code error;
    const auto decoded=ManagedDeviceIdentity::DescribeBootstrapWire(wire,error);
    REQUIRE_FALSE(error);REQUIRE(decoded.binding);REQUIRE(wire.size()>168);
    auto reference=*decoded.binding;reference.binding_directory^=1;
    if (!reference.binding_directory) {reference.binding_directory=2;}
    const auto prefix=wire.first(wire.size()-168);
    auto suffix=ManagedDeviceIdentity::BootstrapSuffix(prefix,reference,error);
    REQUIRE_FALSE(error);REQUIRE(suffix.size()==168);
    std::vector<uint8_t> changed(prefix.begin(),prefix.end());
    changed.insert(changed.end(),suffix.begin(),suffix.end());
    const auto valid=ManagedDeviceIdentity::DescribeBootstrapWire(changed,error);
    REQUIRE_FALSE(error);REQUIRE(valid.uuid==decoded.uuid);REQUIRE(valid.epoch==decoded.epoch);
    REQUIRE(valid.binding->binding_directory!=decoded.binding->binding_directory);
    return changed;
  }
  static std::error_code PrepareOnly(ManagedVolumeFormatHandoff& ticket) {
    ticket.operation_ = ManagedDeviceIdentity::PrepareHandoff(
        ticket.identity_, ticket.binding_);
    ticket.error_ = ticket.operation_->error;
    return ticket.error_;
  }
  struct PreparedObservation { std::error_code error; bool actual_source_lease; };
  static PreparedObservation EnterPreparedBoundary(
      ManagedVolumeFormatHandoff& ticket, const std::atomic<bool>* external) {
    // Same production-used final admission/wiring, after the public precheck.
    // This executes the real independent EX drain before either final read.
    ticket.operation_ = ManagedVolumeAuthority::PrepareFormatHandoff(ticket, external);
    ticket.error_ = ticket.operation_->error;
    return {ticket.error_, bool(ticket.operation_->source_lease)};
  }
  static std::shared_ptr<const HostStorageRoot> ObservedFormatSource(
      const ManagedVolumeFormatHandoff& ticket) {
    return ticket.operation_?ticket.operation_->observation_source:nullptr;
  }
  static std::shared_ptr<const HostStorageRoot> FormatPublicAuthority(
      const ManagedVolumeFormatHandoff& ticket) { return ticket.public_authority_; }
  static std::shared_ptr<const ManagedVolumeAuthority> StopAfterFormatAdmission(
      ManagedVolumeFormatHandoff& ticket) {
    return ManagedVolumeAuthority::FromCompletedFormatInternal(ticket,1);
  }
  static std::shared_ptr<const ManagedVolumeAuthority> OwnersReadyOnly(
      ManagedVolumeFormatHandoff& ticket) {
    auto owner = ManagedVolumeAuthority::BuildFormatVolume(ticket, true);
    if (!owner->error_) {
      owner->error_ = ManagedDeviceIdentity::AdvanceHandoff(*ticket.operation_, false);
    }
    ticket.error_ = owner->error_;
    return owner;
  }
};
}  // namespace xe::filesystem
namespace xe::filesystem::test {
namespace {
struct VolumeScratch {
  std::filesystem::path path;
  std::shared_ptr<HostStorageRoot> root;
  explicit VolumeScratch(
      std::filesystem::path parent = std::filesystem::temp_directory_path()) {
    for (unsigned i = 0; i != 16; ++i) {
      path = parent /
             ("xenia-volume-owner-" + std::to_string(std::random_device{}()));
      if (std::filesystem::create_directory(path)) {
        break;
      }
    }
    REQUIRE(std::filesystem::is_directory(path));
    REQUIRE(chmod(path.c_str(), 0700) == 0);
    std::error_code error;
    root = HostStorageRoot::Open(path, false, error, 0);
    REQUIRE(root);
    REQUIRE_FALSE(error);
    REQUIRE(root->native_filesystem_authority());
  }
  ~VolumeScratch() {
    std::error_code error;
    std::filesystem::remove_all(path, error);
  }
  std::shared_ptr<const ManagedVolumeAuthority> Open(bool nested = false) {
    std::error_code error;
    auto parent = root;
    if (nested) {
      REQUIRE_FALSE(root->Create("hidden-parent", true));
      parent = root->Subroot("hidden-parent", false, error);
      REQUIRE(parent);
    }
    ManagedVolumeAuthority::Options options;
    options.content = root;
    options.source_id = "test-device";
    options.device_id = 3;
    options.state_parent_override = parent;
    auto owner = ManagedVolumeAuthority::Open(std::move(options));
    REQUIRE(owner);
    REQUIRE_FALSE(owner->error());
    REQUIRE(owner->profile_writable());
    return owner;
  }
};
std::vector<uint8_t> BootstrapFile(const std::filesystem::path&);
struct TreeRow {
  std::filesystem::file_type type;
  std::filesystem::perms permissions;
  std::vector<uint8_t> bytes;
  bool operator==(const TreeRow&) const = default;
};
using Tree = std::map<std::string, TreeRow>;
Tree TreeBytes(const std::filesystem::path& root) {
  Tree result;
  for (const auto& entry :
       std::filesystem::recursive_directory_iterator(root)) {
    const auto status = entry.symlink_status();
    std::vector<uint8_t> bytes;
    if (std::filesystem::is_regular_file(status)) {
      std::ifstream file(entry.path(), std::ios::binary);
      REQUIRE(file.good());
      bytes.assign(std::istreambuf_iterator<char>(file), {});
    } else if (std::filesystem::is_symlink(status)) {
      const auto target =
          std::filesystem::read_symlink(entry.path()).generic_string();
      bytes.assign(target.begin(), target.end());
    }
    result.emplace(
        entry.path().lexically_relative(root).generic_string(),
        TreeRow{status.type(), status.permissions(), std::move(bytes)});
  }
  return result;
}
Tree RenamedTree(const Tree& before, std::string_view from,
                 std::string_view to) {
  Tree expected;
  for (const auto& [name, row] : before) {
    expected.emplace(name == from ? std::string(to)
                     : name.starts_with(std::string(from) + "/")
                         ? std::string(to) + name.substr(from.size())
                         : name,
                     row);
  }
  return expected;
}
std::string BindingKey(std::string_view source,
                       const std::filesystem::path& path) {
  const auto text = std::string(source) + "\n" + path.generic_string();
  std::array<uint8_t, 32> bytes{};
  REQUIRE(mbedtls_sha256(reinterpret_cast<const uint8_t*>(text.data()),
                         text.size(), bytes.data(), 0) == 0);
  constexpr char hex[] = "0123456789abcdef";
  std::string key;
  for (auto b : bytes) {
    key += hex[b >> 4];
    key += hex[b & 15];
  }
  return key;
}
std::shared_ptr<const ManagedVolumeAuthority> At(
    VolumeScratch& scratch, const char* leaf,
    std::string id = "rename-device") {
  std::error_code error;
  ManagedVolumeAuthority::Options options;
  options.content = scratch.root->Subroot(leaf, false, error);
  REQUIRE(options.content);
  options.source_id = std::move(id);
  options.device_id = 3;
  options.state_parent_override = scratch.root;
  return ManagedVolumeAuthority::Open(std::move(options));
}
ManagedVolumeAuthority::Options ExistingFormatOptions(VolumeScratch& scratch) {
  std::error_code error;
  auto fresh = HostStorageRoot::Open(scratch.path, false, error, 0);
  REQUIRE(fresh);
  REQUIRE_FALSE(error);
  ManagedVolumeAuthority::Options options;
  options.content = fresh;
  options.source_id = "test-device";
  options.device_id = 3;
  options.state_parent_override = std::move(fresh);
  return options;
}
}  // namespace
TEST_CASE(
    "Managed storage contexts retain exact native parent and recovery "
    "authorities",
    "[managed_volume_authority]") {
  VolumeScratch scratch, foreign;
  std::shared_ptr<const SnapshotRecoveryOwner> recovery =
      SnapshotRecoveryOwner::Open(scratch.root);
  std::shared_ptr<const SnapshotRecoveryOwner> other =
      SnapshotRecoveryOwner::Open(foreign.root);
  REQUIRE(recovery);
  REQUIRE_FALSE(recovery->error());
  REQUIRE(other);
  REQUIRE_FALSE(other->error());
  std::error_code error;
  REQUIRE_FALSE(ManagedStorageContext::Capture(scratch.root, other, {}, error));
  REQUIRE(error == std::errc::permission_denied);
  auto readonly = HostStorageRoot::Open(scratch.path, true, error, 0);
  REQUIRE(readonly);
  REQUIRE_FALSE(ManagedStorageContext::Capture(readonly, recovery, {}, error));
  REQUIRE(error == std::errc::permission_denied);
  REQUIRE_FALSE(
      ManagedStorageContext::Capture(scratch.root, recovery, readonly, error));
  REQUIRE(error == std::errc::permission_denied);
  auto context = ManagedStorageContext::Capture(scratch.root, recovery,
                                                scratch.root, error);
  REQUIRE(context);
  REQUIRE_FALSE(error);
  REQUIRE(context->data_parent() == scratch.root);
  REQUIRE(context->legacy_recovery() == recovery);
  REQUIRE(context->state_parent_override() == scratch.root);
  REQUIRE_FALSE(context->CheckCurrent());
  recovery.reset();
  REQUIRE_FALSE(context->CheckCurrent());
  scratch.root->Retire();
  REQUIRE(context->CheckCurrent());
}
TEST_CASE(
    "Managed source relocation refuses before writes and original namespace "
    "reopens",
    "[managed_volume_authority][initialization]") {
  VolumeScratch scratch;
  REQUIRE_FALSE(scratch.root->Create("A", true));
  ManagedDeviceIdentity::Uuid uuid;
  std::filesystem::path intents, recovery;
  {
    auto owner = At(scratch, "A");
    REQUIRE_FALSE(owner->error());
    uuid = owner->identity()->uuid();
    intents = owner->intents()->root()->path();
    recovery = owner->recovery()->root()->path();
  }
  const auto before = TreeBytes(scratch.path);
  std::filesystem::rename(scratch.path / "A", scratch.path / "B");
  {
    auto refused = At(scratch, "B");
    REQUIRE(refused->error() == std::errc::state_not_recoverable);
    REQUIRE_FALSE(refused->profile_writable());
    REQUIRE(TreeBytes(scratch.path) == RenamedTree(before, "A", "B"));
  }
  std::filesystem::rename(scratch.path / "B", scratch.path / "A");
  const auto owner = At(scratch, "A");
  REQUIRE_FALSE(owner->error());
  REQUIRE(owner->identity()->uuid() == uuid);
  REQUIRE(owner->intents()->root()->path() == intents);
  REQUIRE(owner->recovery()->root()->path() == recovery);
  REQUIRE(TreeBytes(scratch.path) == before);
  std::error_code error;
  const auto guest = owner->content()->GuestView(error);
  REQUIRE(guest);
  const auto binding = ".xenia-device-binding-v1-" +
                       BindingKey("rename-device", scratch.path / "A");
  REQUIRE_FALSE(guest->OpenFile(std::filesystem::path(binding) / "owner.v1",
                                FileAccess::kGenericRead, error));
  REQUIRE(error == std::errc::permission_denied);
}
TEST_CASE(
    "Partial and unknown typed volume bindings never become fresh ownership",
    "[managed_volume_authority][initialization]") {
  VolumeScratch scratch;
  REQUIRE_FALSE(scratch.root->Create("A", true));
  const auto binding = ".xenia-device-binding-v1-" +
                       BindingKey("rename-device", scratch.path / "A");
  REQUIRE(std::filesystem::create_directory(scratch.path / "A" / binding));
  REQUIRE(chmod((scratch.path / "A" / binding).c_str(), 0700) == 0);
  SECTION("Relocated partial binding has no marker and retains all bytes") {
    std::ofstream(scratch.path / "A" / binding / "owner.v1") << "partial";
    const auto before = TreeBytes(scratch.path);
    std::filesystem::rename(scratch.path / "A", scratch.path / "B");
    auto refused = At(scratch, "B");
    REQUIRE(refused->error());
    REQUIRE_FALSE(refused->profile_writable());
    REQUIRE(TreeBytes(scratch.path) == RenamedTree(before, "A", "B"));
    REQUIRE_FALSE(std::filesystem::exists(scratch.path / "B" /
                                          ManagedDeviceIdentity::kMarker));
  }
  SECTION("Same-key partial binding cannot be resumed as fresh") {
    const auto before = TreeBytes(scratch.path);
    auto refused = At(scratch, "A");
    REQUIRE(refused->error());
    REQUIRE(TreeBytes(scratch.path) == before);
    REQUIRE_FALSE(std::filesystem::exists(scratch.path / "A" /
                                          ManagedDeviceIdentity::kMarker));
  }
  SECTION("Unknown typed state is not adopted or hidden by its name") {
    REQUIRE(std::filesystem::remove(scratch.path / "A" / binding));
    std::ofstream(scratch.path / "A/.xenia-device-unknown") << "unowned bytes";
    const auto before = TreeBytes(scratch.path);
    auto refused = At(scratch, "A");
    REQUIRE(refused->error());
    REQUIRE(TreeBytes(scratch.path) == before);
  }
}
TEST_CASE(
    "Committed volume components are noncreating and failed owners retain "
    "exclusion",
    "[managed_volume_authority][initialization]") {
  VolumeScratch scratch;
  REQUIRE_FALSE(scratch.root->Create("A", true));
  std::filesystem::path recovery;
  {
    const auto owner = At(scratch, "A");
    REQUIRE_FALSE(owner->error());
    recovery = owner->recovery()->root()->path();
  }
  std::filesystem::rename(recovery, scratch.path / "parked-recovery");
  const auto before = TreeBytes(scratch.path);
  const auto refused = At(scratch, "A");
  REQUIRE(refused->error());
  REQUIRE_FALSE(refused->profile_writable());
  REQUIRE_FALSE(std::filesystem::exists(recovery));
  REQUIRE(TreeBytes(scratch.path) == before);
  const auto competing = At(scratch, "A");
  REQUIRE(competing->error() == std::errc::device_or_resource_busy);
  REQUIRE(TreeBytes(scratch.path) == before);
}
TEST_CASE("Existing-only source owners never create missing metadata",
          "[managed_volume_authority][initialization]") {
  VolumeScratch scratch;
  REQUIRE_FALSE(scratch.root->Create("content", true));
  std::error_code error;
  auto source = scratch.root->Subroot("content", false, error);
  REQUIRE(source);
  const auto before = TreeBytes(scratch.path);
  REQUIRE_FALSE(ManagedDeviceIdentity::CheckExisting(source, "old", 3, error));
  REQUIRE_FALSE(error);
  REQUIRE_FALSE(ManagedDeviceIdentity::OpenExisting(source, "old", 3, error));
  REQUIRE(error == std::errc::no_such_file_or_directory);
  const auto recovery = SnapshotRecoveryOwner::OpenExisting(scratch.root);
  REQUIRE(recovery->error() == std::errc::no_such_file_or_directory);
  REQUIRE(TreeBytes(scratch.path) == before);
  const auto identity = ManagedDeviceIdentity::Open(source, "old", 3, error);
  REQUIRE(identity);
  const auto marked = TreeBytes(scratch.path);
  const auto missing = ProfileIntentOwner::OpenExisting(scratch.root, identity);
  REQUIRE(missing->error() == std::errc::no_such_file_or_directory);
  REQUIRE(TreeBytes(scratch.path) == marked);
}
TEST_CASE(
    "Explicit existing legacy bundles retain nonprimary HDD and MU ownership",
    "[managed_volume_authority][initialization]") {
  const auto device = GENERATE(1u, 3u);
  VolumeScratch scratch;
  REQUIRE_FALSE(scratch.root->Create("legacy-content", true));
  REQUIRE_FALSE(scratch.root->Create("known-shell", true));
  REQUIRE_FALSE(scratch.root->Create("known-shell/Content", true));
  std::error_code error;
  auto source = scratch.root->Subroot("legacy-content", false, error);
  auto shell = scratch.root->Subroot("known-shell", false, error);
  const std::string id = device == 1 ? "hdd" : "memory-unit";
  std::shared_ptr<const SnapshotRecoveryOwner> recovery =
      SnapshotRecoveryOwner::Open(scratch.root);
  REQUIRE_FALSE(recovery->error());
  ManagedDeviceIdentity::Uuid uuid;
  std::filesystem::path intents;
  {
    const auto identity =
        ManagedDeviceIdentity::Open(source, id, device, error);
    REQUIRE(identity);
    const auto owner = ProfileIntentOwner::Open(scratch.root, identity);
    REQUIRE_FALSE(owner->error());
    uuid = identity->uuid();
    intents = owner->root()->path();
  }
  const auto legacy = TreeBytes(scratch.path);
  struct stat legacy_marker{};
  REQUIRE(stat((source->path()/ManagedDeviceIdentity::kMarker).c_str(),&legacy_marker)==0);
  ManagedVolumeAuthority::Options options;
  options.content = source;
  options.source_id = id;
  options.device_id = device;
  SECTION("Known existing owners remain at their exact legacy locations") {
    std::set<std::string> content_purpose;
    {
      // The legacy fixture predates the separate content mutation purpose.
      // Bootstrap it through the real public owner/identity/recovery/lease path,
      // then close all new owners before testing noncreating reopen semantics.
      ManagedVolumeAuthority::Options bootstrap;
      bootstrap.content = source; bootstrap.source_id = id; bootstrap.device_id = device;
      bootstrap.existing_legacy = ManagedVolumeAuthority::Options::ExistingLegacy{
          scratch.root, shell, recovery};
      const auto owner = ManagedVolumeAuthority::Open(std::move(bootstrap));
      REQUIRE_FALSE(owner->error()); REQUIRE(owner->profile_writable());
      REQUIRE(owner->identity()->uuid() == uuid);
      REQUIRE(owner->intents()->root()->path() == intents);
      REQUIRE(owner->recovery()->root()->path() == recovery->root()->path());
      REQUIRE(owner->public_root()->path() == shell->path());
      REQUIRE(owner->mutations()); REQUIRE(owner->mutations()->ready());
      content_purpose = ContentMutationOwnerTestAccess::InitializationEntries(
          *owner->mutations(), scratch.path);
    }
    const auto before = TreeBytes(scratch.path);
    std::set<std::string> added;
    for (const auto& [path, row] : legacy) {
      const auto found = before.find(path);
      REQUIRE(found != before.end());
      if (path==std::string("legacy-content/")+ManagedDeviceIdentity::kMarker) {
        // Explicit migration appends one checked required-reference frame.
        // Every old byte/unknown field remains the exact prefix; UUID/epoch
        // and purpose roots above are independently required to stay equal.
        REQUIRE(found->second.type==row.type);REQUIRE(found->second.permissions==row.permissions);
        REQUIRE(found->second.bytes.size()==row.bytes.size()+168);
        REQUIRE(std::equal(row.bytes.begin(),row.bytes.end(),found->second.bytes.begin()));
      } else {REQUIRE(found->second==row);}
    }
    for (const auto& [path, row] : before) {
      if (legacy.contains(path)) { continue; }
      added.insert(path);
      if (row.type == std::filesystem::file_type::directory) {
        REQUIRE(row.permissions == std::filesystem::perms::owner_all);
      } else {
        REQUIRE(row.type == std::filesystem::file_type::regular);
        REQUIRE(row.permissions == (std::filesystem::perms::owner_read |
                                    std::filesystem::perms::owner_write));
        REQUIRE_FALSE(row.bytes.empty());
      }
    }
    auto expected_added=content_purpose;
    const auto binding_relative=std::filesystem::path("legacy-content")/
        (".xenia-device-binding-v1-"+BindingKey(id,source->path()));
    REQUIRE(added.contains(binding_relative.generic_string()));
    expected_added.insert(binding_relative.generic_string());
    const std::set<std::string> fixed_controls{"owner.v1","bootstrap-source.v1",
        "bootstrap.pending.v1","bootstrap.commit.v1",
        "bootstrap.component-08.v1","bootstrap.component-09.v1"};
    std::set<std::string> observed_controls;unsigned preservation_records=0,preserved_markers=0;
    for (const auto& path:added) {
      const std::filesystem::path relative(path);
      if (relative.parent_path()==binding_relative) {
        const auto leaf=relative.filename().string();
        if (leaf.starts_with("bootstrap-preserve-") && leaf.ends_with(".v1")) {++preservation_records;}
        else {REQUIRE(fixed_controls.contains(leaf));observed_controls.insert(leaf);}
        expected_added.insert(path);
      } else if (relative.parent_path()=="legacy-content" &&
                 relative.filename().string().starts_with("preserved-") &&
                 relative.filename().string().ends_with("-h")) {
        ++preserved_markers;expected_added.insert(path);
        const auto& old=legacy.at(std::string("legacy-content/")+ManagedDeviceIdentity::kMarker);
        REQUIRE(before.at(path).bytes==old.bytes);
        struct stat preserved{};REQUIRE(stat((scratch.path/relative).c_str(),&preserved)==0);
        REQUIRE(preserved.st_ino==legacy_marker.st_ino);REQUIRE(preserved.st_dev==legacy_marker.st_dev);
      }
    }
    REQUIRE(observed_controls==fixed_controls);REQUIRE(preservation_records==1);REQUIRE(preserved_markers==1);
    REQUIRE(added==expected_added);
    options.existing_legacy = ManagedVolumeAuthority::Options::ExistingLegacy{
        scratch.root, shell, recovery};
    const auto owner = ManagedVolumeAuthority::Open(std::move(options));
    REQUIRE_FALSE(owner->error());
    REQUIRE(owner->identity()->uuid() == uuid);
    REQUIRE(owner->intents()->root()->path() == intents);
    REQUIRE(owner->recovery()->root()->path() == recovery->root()->path());
    REQUIRE(owner->public_root()->path() == shell->path());
    REQUIRE(TreeBytes(scratch.path) == before);
  }
  SECTION("Unproven legacy bundle cannot mint a new journal parent") {
    // This path deliberately has ONLY the original legacy identity/profile/
    // recovery bundle. No content-purpose factory ran before this refusal.
    const auto absent = ContentMutationOwnerTestAccess::InitializationEntries(uuid);
    REQUIRE(TreeBytes(scratch.path) == legacy);
    for (const auto& name : absent) {
      REQUIRE_FALSE(std::filesystem::exists(scratch.path / name));
    }
    const auto refused = ManagedVolumeAuthority::Open(std::move(options));
    REQUIRE(refused->error());
    REQUIRE_FALSE(refused->profile_writable());
    REQUIRE(TreeBytes(scratch.path) == legacy);
    for (const auto& name : absent) {
      REQUIRE_FALSE(std::filesystem::exists(scratch.path / name));
    }
  }
}
TEST_CASE(
    "Native volume owners isolate in-Content state without changing public "
    "files",
    "[managed_volume_authority]") {
  VolumeScratch scratch;
  constexpr std::array<uint8_t, 3> value{1, 2, 3};
  REQUIRE_FALSE(scratch.root->WriteNewFile("Account", value).error);
  auto owner = scratch.Open(true);
  const auto private_relative =
      owner->intents()->private_parent()->path().lexically_relative(
          scratch.path);
  std::error_code error;
  auto guest = owner->content()->GuestView(error);
  REQUIRE(guest);
  REQUIRE_FALSE(error);
  REQUIRE_FALSE(guest->OpenFile(private_relative / "owner.v1",
                                FileAccess::kGenericRead, error));
  REQUIRE(error == std::errc::permission_denied);
  REQUIRE_FALSE(guest->Subroot(private_relative, false, error));
  REQUIRE(guest->Create(private_relative / "new", true) ==
          std::errc::permission_denied);
  REQUIRE(guest->Remove("hidden-parent", true) == std::errc::permission_denied);
  REQUIRE(guest->Rename("hidden-parent", "moved", false) ==
          std::errc::permission_denied);
  REQUIRE_FALSE(
      guest->CaptureSnapshot(private_relative / "owner.v1", 8192, error));
  std::vector<uint8_t> bytes;
  REQUIRE_FALSE(guest->ReadFile("Account", 3, bytes));
  REQUIRE(bytes == std::vector<uint8_t>{1, 2, 3});
  auto raw_declaration = HostStorageRoot::AdmitGuestMount(
      std::vector<GuestHostBacking>{{scratch.path, true}}, error);
  REQUIRE_FALSE(raw_declaration);
  REQUIRE(error == std::errc::permission_denied);
  auto owned_declaration = HostStorageRoot::AdmitGuestMount(
      std::vector<GuestHostBacking>{guest->GuestBacking()}, error);
  REQUIRE(owned_declaration);
  REQUIRE_FALSE(error);
  REQUIRE(owner->public_root()->path() !=
          owner->intents()->private_parent()->path());
  REQUIRE(owner->public_root()->GuestView(error));
  REQUIRE(owner->public_root()->OpenFile("Content", FileAccess::kGenericRead,
                                         error));
  REQUIRE(owner->public_root()->Subroot({}, false, error));
  bool matched = false;
  auto launch = owner->content()->CaptureGuestDirectory(scratch.path, false, matched, error);
  REQUIRE(launch);
  REQUIRE(matched);
  REQUIRE_FALSE(error);
  REQUIRE(launch->has_guest_projection());
  REQUIRE(launch->GuestView(error));
  matched = false;
  REQUIRE_FALSE(owner->content()->CaptureGuestDirectory(
      owner->intents()->private_parent()->path(), false, matched, error));
  REQUIRE(matched);
  REQUIRE(error == std::errc::permission_denied);
  const auto public_relative =
      owner->public_root()->path().lexically_relative(scratch.path);
  REQUIRE_FALSE(guest->OpenFile(public_relative / "Content",
                                FileAccess::kGenericRead, error));
  owner.reset();
  // A retained guest mount keeps the exclusion after the wrapper goes away.
  REQUIRE_FALSE(guest->OpenFile(private_relative / "owner.v1",
                                FileAccess::kGenericRead, error));
  REQUIRE(std::filesystem::is_regular_file(scratch.path / private_relative /
                                           "owner.v1"));
}
TEST_CASE(
    "Private aliases fail but ordinary public hardlinks retain read semantics",
    "[managed_volume_authority]") {
  VolumeScratch scratch;
  auto owner = scratch.Open();
  std::error_code error;
  auto guest = owner->content()->GuestView(error);
  REQUIRE(guest);
  const auto private_file =
      owner->intents()->private_parent()->path() / "owner.v1";
  std::filesystem::create_directory_symlink(private_file.parent_path(),
                                            scratch.path / "private-alias");
  REQUIRE_FALSE(guest->OpenFile("private-alias/owner.v1",
                                FileAccess::kGenericRead, error));
  std::ofstream(scratch.path / "public-A") << "public";
  std::filesystem::create_hard_link(scratch.path / "public-A",
                                    scratch.path / "public-B");
  std::vector<uint8_t> bytes;
  REQUIRE_FALSE(guest->ReadFile("public-B", 16, bytes));
  REQUIRE(bytes == std::vector<uint8_t>{'p', 'u', 'b', 'l', 'i', 'c'});
  std::filesystem::create_hard_link(private_file,
                                    scratch.path / "private-link");
  REQUIRE_FALSE(
      guest->OpenFile("private-link", FileAccess::kGenericRead, error));
  REQUIRE(error == std::errc::permission_denied);
  REQUIRE(guest->Remove("private-link", false));
  REQUIRE(guest->Rename("private-link", "other-link", false));
  std::filesystem::remove(scratch.path / "private-link");
  REQUIRE_FALSE(guest->ReadFile("public-B", 16, bytes));
}
TEST_CASE(
    "Actual case-insensitive filesystem aliases cannot reach private state",
    "[managed_volume_authority]") {
  VolumeScratch scratch;
  const auto owner = scratch.Open();
  const auto private_root = owner->intents()->private_parent()->path();
  auto name = private_root.filename().string();
  for (auto& c : name) {
    if (c >= 'a' && c <= 'z') {
      c = char(c - 'a' + 'A');
    }
  }
  std::error_code error;
  if (!std::filesystem::equivalent(private_root, scratch.path / name, error) ||
      error) {
    WARN(
        "Case-sensitive host: native insensitive/Unicode alias execution "
        "remains unverified");
    return;
  }
  auto guest = owner->content()->GuestView(error);
  REQUIRE(guest);
  REQUIRE_FALSE(guest->OpenFile(std::filesystem::path(name) / "OWNER.V1",
                                FileAccess::kGenericRead, error));
  REQUIRE(error == std::errc::permission_denied);
  REQUIRE_FALSE(guest->Subroot(name, false, error));
  REQUIRE(guest->Create(std::filesystem::path(name) / "new", true) ==
          std::errc::permission_denied);
  REQUIRE(guest->Remove(name, true) == std::errc::permission_denied);
  REQUIRE(guest->Rename(name, "new-state", false) ==
          std::errc::permission_denied);
  std::ofstream(scratch.path / "Public-A") << "public";
  std::vector<uint8_t> bytes;
  REQUIRE_FALSE(guest->ReadFile("PUBLIC-A", 16, bytes));
  REQUIRE(bytes == std::vector<uint8_t>{'p', 'u', 'b', 'l', 'i', 'c'});
}
TEST_CASE(
    "Persistent source binding reopens its recorded parent and rejects a "
    "silent override switch",
    "[managed_volume_authority]") {
  VolumeScratch scratch;
  REQUIRE_FALSE(scratch.root->Create("chosen-parent", true));
  std::error_code error;
  auto chosen = scratch.root->Subroot("chosen-parent", false, error);
  REQUIRE(chosen);
  std::filesystem::path original_intents, original_recovery;
  ManagedDeviceIdentity::Uuid uuid{};
  {
    ManagedVolumeAuthority::Options options;
    options.content = scratch.root;
    options.source_id = "binding-device";
    options.device_id = 3;
    options.state_parent_override = chosen;
    const auto owner = ManagedVolumeAuthority::Open(std::move(options));
    REQUIRE_FALSE(owner->error());
    uuid = owner->identity()->uuid();
    original_intents = owner->intents()->root()->path();
    original_recovery = owner->recovery()->root()->path();
  }
  {
    // Omitted override honors durable binding rather than mount-root
    // preference.
    ManagedVolumeAuthority::Options options;
    options.content = scratch.root;
    options.source_id = "binding-device";
    options.device_id = 3;
    const auto owner = ManagedVolumeAuthority::Open(std::move(options));
    REQUIRE_FALSE(owner->error());
    REQUIRE(owner->identity()->uuid() == uuid);
    REQUIRE(owner->intents()->root()->path() == original_intents);
    REQUIRE(owner->recovery()->root()->path() == original_recovery);
  }
  ManagedVolumeAuthority::Options wrong;
  wrong.content = scratch.root;
  wrong.source_id = "binding-device";
  wrong.device_id = 3;
  wrong.state_parent_override = scratch.root;
  const auto refused = ManagedVolumeAuthority::Open(std::move(wrong));
  REQUIRE(refused->error() == std::errc::state_not_recoverable);
  REQUIRE_FALSE(refused->profile_writable());
  REQUIRE(std::filesystem::is_regular_file(original_intents / "owner.v1"));
  REQUIRE(std::filesystem::is_regular_file(original_recovery / "owner.v1"));
}
TEST_CASE("Published bootstrap guest retirement and release do not own its pinned public scope",
          "[managed_volume_authority][bootstrap][public_view][lifetime]") {
  const bool retire_published=GENERATE(false,true);
  VolumeScratch scratch;
  std::shared_ptr<HostStorageRoot> source,public_guest;
  std::weak_ptr<HostStorageRoot> published;
  std::weak_ptr<const HostStorageGuestProjection> projection;
  ManagedDeviceIdentity::Uuid uuid;
  {
    auto owner=scratch.Open();
    source=owner->content();public_guest=owner->public_root();
    REQUIRE(source);REQUIRE(public_guest);
    uuid=owner->identity()->uuid();published=public_guest;
    projection=public_guest->GuestBacking().projection;
    REQUIRE_FALSE(projection.expired());
    if (retire_published) {
      // Format/teardown logically closes the actual returned guest, not the
      // strongly pinned unadmitted public namespace owned by the projection.
      public_guest->Retire();
      std::error_code error;
      REQUIRE_FALSE(public_guest->GuestView(error));
      REQUIRE(error==std::errc::no_such_device);
      auto source_guest=source->GuestView(error);
      REQUIRE(source_guest);REQUIRE_FALSE(error);
    }
  }
  // An explicitly retained guest is a real external authority. It must keep
  // the projection alive until its own final owner releases it.
  REQUIRE_FALSE(published.expired());
  REQUIRE_FALSE(projection.expired());
  if (!retire_published) {
    const std::array<uint8_t,4> bytes{0x50,0,0xA3,0x7F};
    REQUIRE_FALSE(public_guest->WriteNewFile("retained-public.data",bytes).error);
    std::vector<uint8_t> read;
    REQUIRE_FALSE(public_guest->ReadFile("retained-public.data",16,read));
    REQUIRE(read==std::vector<uint8_t>(bytes.begin(),bytes.end()));
    REQUIRE_FALSE(public_guest->Remove("retained-public.data",false));
  }
  public_guest.reset();
  REQUIRE(published.expired());
  REQUIRE(projection.expired());
  const auto expired=source->GuestBacking();
  REQUIRE(expired.requires_projection);REQUIRE_FALSE(expired.projection);
  std::error_code error;
  REQUIRE_FALSE(source->GuestView(error));
  REQUIRE(error==std::errc::no_such_device);
  const auto reopened=scratch.Open();
  REQUIRE(reopened->profile_writable());
  REQUIRE(reopened->mutations()->ready());
  REQUIRE(reopened->identity()->uuid()==uuid);
}
TEST_CASE(
    "Expired filtered authority cannot be re-admitted as an unfiltered path",
    "[managed_volume_authority]") {
  VolumeScratch scratch;
  std::shared_ptr<HostStorageRoot> captured;
  {
    const auto owner = scratch.Open();
    captured = owner->content();
  }
  REQUIRE(captured->has_guest_projection());
  const auto backing = captured->GuestBacking();
  REQUIRE(backing.requires_projection);
  REQUIRE_FALSE(backing.projection);
  std::error_code error;
  REQUIRE_FALSE(HostStorageRoot::AdmitGuestMount(
      std::vector<GuestHostBacking>{backing}, error));
  REQUIRE(error == std::errc::no_such_device);
  REQUIRE_FALSE(captured->GuestView(error));
}
TEST_CASE(
    "Existing corrupt state is preserved and never replaced by another parent",
    "[managed_volume_authority]") {
  VolumeScratch scratch;
  std::filesystem::path state;
  {
    auto owner = scratch.Open();
    state = owner->intents()->private_parent()->path();
  }
  std::ofstream(state / "owner.v1", std::ios::trunc) << "not an owner";
  ManagedVolumeAuthority::Options options;
  options.content = scratch.root;
  options.source_id = "test-device";
  options.device_id = 3;
  options.state_parent_override = scratch.root;
  const auto failed = ManagedVolumeAuthority::Open(std::move(options));
  REQUIRE(failed->error() == std::errc::bad_message);
  REQUIRE_FALSE(failed->profile_writable());
  REQUIRE_FALSE(failed->intents());
  std::ifstream stored(state / "owner.v1");
  std::string text;
  std::getline(stored, text);
  REQUIRE(text == "not an owner");
  std::error_code error;
  REQUIRE_FALSE(HostStorageRoot::AdmitGuestMount(
      std::vector<GuestHostBacking>{{state, true}}, error));
}
TEST_CASE(
    "Primary HDD bundle preserves exact existing private and public locations",
    "[managed_volume_authority]") {
  VolumeScratch scratch;
  REQUIRE_FALSE(scratch.root->Create("content", true));
  REQUIRE_FALSE(scratch.root->Create("public", true));
  std::error_code error;
  auto content = scratch.root->Subroot("content", false, error);
  auto public_root = scratch.root->Subroot("public", false, error);
  REQUIRE(content);
  REQUIRE(public_root);
  auto recovery = std::shared_ptr<const SnapshotRecoveryOwner>(
      SnapshotRecoveryOwner::Open(scratch.root));
  REQUIRE_FALSE(recovery->error());
  ManagedVolumeAuthority::Options options;
  options.content = content;
  options.source_id = "hdd";
  options.device_id = 1;
  options.preserve_legacy_layout = true;
  options.legacy_private_parent = scratch.root;
  options.legacy_public_root = public_root;
  options.legacy_recovery = recovery;
  const auto owner = ManagedVolumeAuthority::Open(options);
  REQUIRE_FALSE(owner->error());
  REQUIRE(owner->recovery() == recovery);
  REQUIRE(owner->recovery()->root()->path() ==
          scratch.path / SnapshotRecoveryOwner::kDirectory);
  REQUIRE(owner->intents()->private_parent()->path() == scratch.path);
  REQUIRE(owner->public_root()->path() == scratch.path / "public");
  REQUIRE(owner->identity()->configured_location() == content->path());
  REQUIRE(owner->content()->has_guest_projection());
  std::error_code projected_error;
  auto projected=owner->content()->GuestView(projected_error);
  REQUIRE(projected);REQUIRE_FALSE(projected_error);
  const auto binding=".xenia-device-binding-v1-"+BindingKey("hdd",content->path());
  REQUIRE_FALSE(projected->OpenFile(std::filesystem::path(binding)/"owner.v1",FileAccess::kGenericRead,projected_error));
  REQUIRE(projected_error==std::errc::permission_denied);
  options.legacy_recovery.reset();
  const auto incomplete = ManagedVolumeAuthority::Open(options);
  REQUIRE(incomplete->error());
  REQUIRE_FALSE(incomplete->profile_writable());
  REQUIRE_FALSE(incomplete->recovery());
}
TEST_CASE(
    "Managed folder format preserves exact exclusions and unlinks ordinary "
    "symlinks",
    "[managed_volume_authority]") {
  VolumeScratch scratch;
  std::shared_ptr<const ManagedDeviceIdentity> identity;
  std::filesystem::path state, shell;
  {
    const auto owner = scratch.Open();
    identity = owner->identity();
    state = owner->intents()->private_parent()->path();
    shell = owner->public_root()->path();
  }
  std::ofstream(scratch.path / "user-file") << "user data";
  std::filesystem::create_symlink("missing-user-target",
                                  scratch.path / "user-link");
  const auto format = ManagedDeviceIdentity::Format(identity);
  REQUIRE_FALSE(format.error);
  REQUIRE(format.complete);
  REQUIRE(format.current);
  REQUIRE(format.current->uuid() == identity->uuid());
  REQUIRE(format.content_epoch == identity->content_epoch() + 1);
  REQUIRE(std::filesystem::is_regular_file(state / "owner.v1"));
  REQUIRE(std::filesystem::is_directory(shell));
  REQUIRE_FALSE(std::filesystem::exists(scratch.path / "user-file"));
  REQUIRE_FALSE(std::filesystem::is_symlink(scratch.path / "user-link"));
  std::error_code error;
  auto guest = format.current->source()->GuestView(error);
  REQUIRE(guest);
  REQUIRE_FALSE(guest->OpenFile(state.filename() / "owner.v1",
                                FileAccess::kGenericRead, error));
}
TEST_CASE("Managed format preserves native ancestors while retiring only old generations",
          "[managed_volume_authority][format][native_anchor]") {
  const bool replace_ancestor = GENERATE(false, true);
  VolumeScratch scratch;
  REQUIRE_FALSE(scratch.root->Create("parent", true));
  REQUIRE_FALSE(scratch.root->Create("parent/content", true));
  std::error_code error;
  auto parent = scratch.root->Subroot("parent", false, error);
  REQUIRE(parent); REQUIRE_FALSE(error);
  auto source = parent->Subroot("content", false, error);
  REQUIRE(source); REQUIRE_FALSE(error);
  ManagedVolumeAuthority::Options options;
  options.content = source;
  options.source_id = "native-anchor-format";
  options.device_id = 3;
  auto owner = ManagedVolumeAuthority::Open(options);
  REQUIRE(owner); REQUIRE_FALSE(owner->error());
  REQUIRE(ManagedVolumeAuthorityTestAccess::HasExactSourceAnchor(*owner));
  auto old_source = owner->content();
  auto old_guest = old_source->GuestView(error);
  REQUIRE(old_guest); REQUIRE_FALSE(error);
  auto preserved = owner->recovery()->root();
  std::vector<uint8_t> marker;
  REQUIRE_FALSE(preserved->ReadFile(SnapshotRecoveryOwner::kMarker, 256, marker));
  REQUIRE_FALSE(marker.empty());
  auto ticket = owner->CaptureFormatHandoff();
  REQUIRE(ticket); REQUIRE_FALSE(ticket->error());
  ticket->ClosePublishedPublicView();
  owner.reset();
  const auto receipt = ManagedVolumeAuthority::ExecuteFormat(*ticket);
  REQUIRE_FALSE(receipt.error); REQUIRE(receipt.complete);
  auto current = ManagedVolumeAuthority::FromCompletedFormat(*ticket);
  REQUIRE(current); REQUIRE_FALSE(current->error());
  REQUIRE(current->profile_writable());
  std::vector<uint8_t> read;
  REQUIRE(old_source->ReadFile("ordinary.bin", 256, read) ==
          std::errc::no_such_device);
  REQUIRE(old_guest->ReadFile("ordinary.bin", 256, read) ==
          std::errc::no_such_device);
  REQUIRE_FALSE(preserved->ReadFile(SnapshotRecoveryOwner::kMarker, 256, read));
  REQUIRE(read == marker);
  REQUIRE_FALSE(current->identity()->CheckCurrent());
  if (replace_ancestor) {
    // The final directory remains the SAME inode through this ancestor alias.
    // Its original derived-parent O_NOFOLLOW proof must still reject it.
    struct stat before{}, aliased{};
    REQUIRE(stat(preserved->path().c_str(), &before) == 0);
    const auto bytes = TreeBytes(scratch.path / "parent");
    std::filesystem::rename(scratch.path / "parent", scratch.path / "moved");
    std::filesystem::create_directory_symlink("moved", scratch.path / "parent");
    REQUIRE(stat(preserved->path().c_str(), &aliased) == 0);
    REQUIRE(before.st_dev == aliased.st_dev);
    REQUIRE(before.st_ino == aliased.st_ino);
    REQUIRE(preserved->ReadFile(SnapshotRecoveryOwner::kMarker, 256, read));
    REQUIRE(TreeBytes(scratch.path / "moved") == bytes);
    REQUIRE(std::filesystem::remove(scratch.path / "parent"));
    std::filesystem::rename(scratch.path / "moved", scratch.path / "parent");
    REQUIRE_FALSE(preserved->ReadFile(SnapshotRecoveryOwner::kMarker, 256, read));
    REQUIRE(read == marker);
    REQUIRE_FALSE(current->identity()->CheckCurrent());
  }
}
TEST_CASE("Managed format needs real owner drain and restores only fresh views",
          "[managed_volume_authority][format]") {
  VolumeScratch scratch;
  auto owner = scratch.Open();
  const auto old_public = owner->public_root();
  const auto identity = owner->identity();
  const auto epoch = identity->content_epoch();
  const auto uuid = identity->uuid();
  std::ofstream(scratch.path / "save.bin", std::ios::binary) << "persistent save";
  const auto before = TreeBytes(scratch.path);
  auto ticket = owner->CaptureFormatHandoff();
  REQUIRE(ticket);
  REQUIRE_FALSE(ticket->error());
  ticket->ClosePublishedPublicView();
  // This real external old bundle keeps its native source flock. No same-FD
  // upgrade, expired weak pointer or reference count is accepted as drainage.
  const auto refused = ManagedVolumeAuthority::ExecuteFormat(*ticket);
  REQUIRE(refused.error == std::errc::device_or_resource_busy);
  REQUIRE_FALSE(refused.epoch_published);
  REQUIRE_FALSE(refused.data_started);
  REQUIRE_FALSE(refused.current);
  REQUIRE(TreeBytes(scratch.path) == before);
  REQUIRE(old_public->retiring());
  REQUIRE_FALSE(identity->CheckCurrent());
  auto restored = ManagedVolumeAuthority::RestoreUnchangedFormat(*ticket);
  REQUIRE(restored);
  REQUIRE_FALSE(restored->error());
  REQUIRE(restored->profile_writable());
  REQUIRE(restored->public_root() != old_public);
  REQUIRE_FALSE(restored->public_root()->retiring());
  REQUIRE(old_public->retiring());
  REQUIRE(restored->identity() == identity);
  REQUIRE(restored->identity()->uuid() == uuid);
  REQUIRE(restored->identity()->content_epoch() == epoch);
  REQUIRE(TreeBytes(scratch.path) == before);
}
TEST_CASE("Unchanged format restoration requires the original view to retire",
          "[managed_volume_authority][format][recovery]") {
  VolumeScratch scratch;
  auto owner = scratch.Open();
  const auto original = owner->public_root();
  const auto identity = owner->identity();
  std::ofstream(scratch.path / "save.bin") << "unchanged public generation";
  const auto before = TreeBytes(scratch.path);
  auto ticket = owner->CaptureFormatHandoff();
  REQUIRE(ticket);
  REQUIRE_FALSE(ticket->error());
  const auto refused = ManagedVolumeAuthority::RestoreUnchangedFormat(*ticket);
  REQUIRE(refused);
  REQUIRE(refused->error() == std::errc::device_or_resource_busy);
  REQUIRE_FALSE(refused->profile_writable());
  REQUIRE_FALSE(original->retiring());
  REQUIRE_FALSE(identity->CheckCurrent());
  REQUIRE(TreeBytes(scratch.path) == before);
  ticket->ClosePublishedPublicView();
  const auto restored = ManagedVolumeAuthority::RestoreUnchangedFormat(*ticket);
  REQUIRE(restored);
  REQUIRE_FALSE(restored->error());
  REQUIRE(restored->profile_writable());
  REQUIRE(restored->public_root() != original);
  REQUIRE(original->retiring());
  REQUIRE_FALSE(restored->public_root()->retiring());
  REQUIRE(restored->identity()->uuid() == identity->uuid());
  REQUIRE(restored->identity()->content_epoch() == identity->content_epoch());
  REQUIRE(TreeBytes(scratch.path) == before);
}

TEST_CASE("Same-process format recovery reuses only its exact sealed public scope",
          "[managed_volume_authority][format][recovery]") {
  VolumeScratch scratch;
  auto owner = scratch.Open();
  const auto uuid = owner->identity()->uuid();
  const auto epoch = owner->identity()->content_epoch();
  auto ticket = owner->CaptureFormatHandoff();
  REQUIRE(ticket);
  REQUIRE_FALSE(ticket->error());
  ticket->ClosePublishedPublicView();
  owner.reset();
  const auto formatted = ManagedVolumeAuthority::ExecuteFormat(*ticket);
  REQUIRE_FALSE(formatted.error);
  REQUIRE(formatted.complete);
  REQUIRE(ticket->phase() == 1);
  std::ofstream(scratch.path / "after-reset.bin") << "must survive resume";
  const auto preserved = TreeBytes(scratch.path).at("after-reset.bin");
  auto ready = ManagedVolumeAuthorityTestAccess::OwnersReadyOnly(*ticket);
  REQUIRE(ready);
  REQUIRE_FALSE(ready->error());
  REQUIRE(ready->profile_writable());
  REQUIRE(ticket->phase() == 2);
  const auto retired = ready->public_root();
  const auto intents = ready->intents()->root()->path();
  // Model loss of an unpublished phase-two bundle through the same real
  // producer boundary used by restart tests. Release its intent owner; keep
  // the retired public view alive to prove it cannot be reactivated.
  retired->Retire();
  ready.reset();
  const auto resumed = ManagedVolumeAuthority::ResumeFormat(*ticket);
  REQUIRE(resumed);
  REQUIRE_FALSE(resumed->error());
  REQUIRE(resumed->profile_writable());
  REQUIRE(resumed->identity()->uuid() == uuid);
  REQUIRE(resumed->identity()->content_epoch() == epoch + 1);
  REQUIRE(resumed->intents()->root()->path() == intents);
  REQUIRE(resumed->public_root() != retired);
  REQUIRE(retired->retiring());
  REQUIRE_FALSE(resumed->public_root()->retiring());
  REQUIRE(TreeBytes(scratch.path).at("after-reset.bin") == preserved);
}

TEST_CASE("Managed format completes exact next-epoch owners and preserves private redo",
          "[managed_volume_authority][format]") {
  VolumeScratch scratch;
  auto owner = scratch.Open();
  const auto uuid = owner->identity()->uuid();
  const auto epoch = owner->identity()->content_epoch();
  REQUIRE(owner->identity()->binding_required());
  const auto source_prefix=BootstrapFile(scratch.path/ManagedDeviceIdentity::kMarker);
  const auto old_intent = owner->intents()->root()->path();
  const auto recovery = owner->recovery()->root()->path();
  // Retained private files must come through their real namespace producer;
  // arbitrary host filenames/modes are correctly quarantined on fresh reopen.
  constexpr std::array<uint8_t, 4> before{1, 2, 3, 4}, after{5, 6, 7, 8};
  REQUIRE_FALSE(owner->content()->WriteNewFile("save.bin", before).error);
  REQUIRE_FALSE(owner->intents()->root()->WriteNewFile(
      "intent-0123456789abcdef0123456789abcdef.bin", before).error);
  {
    std::error_code error;
    auto expected = owner->content()->CaptureSnapshot("save.bin", 16, error);
    REQUIRE_FALSE(error);
    REQUIRE(expected);
    const auto published = SnapshotPublisher::Publish(*owner->recovery(),
                                                       expected, after);
    REQUIRE_FALSE(published.error);
    REQUIRE(published.published);
    REQUIRE(published.durable);
    REQUIRE(published.recovery_durable);
    REQUIRE(published.recovery_count == before.size());
  }  // Drop all real snapshot tokens before the actual source drain.
  const auto old_redo_bytes = TreeBytes(old_intent);
  const auto recovery_bytes = TreeBytes(recovery);
  auto ticket = owner->CaptureFormatHandoff();
  REQUIRE_FALSE(ticket->error());
  ticket->ClosePublishedPublicView();
  owner.reset();
  const auto receipt = ManagedVolumeAuthority::ExecuteFormat(*ticket);
  REQUIRE_FALSE(receipt.error);
  REQUIRE(receipt.epoch_published);
  REQUIRE(receipt.epoch_durable);
  REQUIRE(receipt.data_started);
  REQUIRE(receipt.complete);
  REQUIRE_FALSE(receipt.current);  // Creator remains private before owner commit.
  REQUIRE(ticket->phase() == 1);
  auto next = ManagedVolumeAuthority::FromCompletedFormat(*ticket);
  REQUIRE(next);
  REQUIRE_FALSE(next->error());
  REQUIRE(next->profile_writable());
  REQUIRE(next->identity()->uuid() == uuid);
  REQUIRE(next->identity()->content_epoch() == epoch + 1);
  REQUIRE(next->identity()->binding_required());
  const auto advanced_source=BootstrapFile(scratch.path/ManagedDeviceIdentity::kMarker);
  REQUIRE(advanced_source.size()>source_prefix.size());
  REQUIRE(std::equal(source_prefix.begin(),source_prefix.end(),advanced_source.begin()));
  REQUIRE(next->intents()->managed_identity() == next->identity());
  REQUIRE(next->intents()->source() == next->content());
  REQUIRE(next->intents()->root()->path() != old_intent);
  REQUIRE_FALSE(next->identity()->CheckCurrent());
  REQUIRE_FALSE(std::filesystem::exists(scratch.path / "save.bin"));
  REQUIRE(TreeBytes(old_intent) == old_redo_bytes);
  REQUIRE(TreeBytes(recovery) == recovery_bytes);
  next.reset();
  ticket.reset();
  auto reopened = ManagedVolumeAuthority::Open(ExistingFormatOptions(scratch));
  REQUIRE(reopened);
  REQUIRE_FALSE(reopened->error());
  REQUIRE(reopened->identity()->uuid() == uuid);
  REQUIRE(reopened->identity()->content_epoch() == epoch + 1);
  REQUIRE(reopened->identity()->binding_required());
  REQUIRE(BootstrapFile(scratch.path/ManagedDeviceIdentity::kMarker)==advanced_source);
  REQUIRE(reopened->intents()->managed_identity() == reopened->identity());
  REQUIRE(TreeBytes(old_intent) == old_redo_bytes);
  REQUIRE(TreeBytes(recovery) == recovery_bytes);
}
TEST_CASE("Managed handoff restart validates each actual durable owner phase",
          "[managed_volume_authority][format][recovery]") {
  const int stop = GENERATE(0, 1, 2);
  VolumeScratch scratch;
  auto owner = scratch.Open();
  const auto uuid = owner->identity()->uuid();
  const auto old_epoch = owner->identity()->content_epoch();
  std::ofstream(scratch.path / "save.bin") << "before format";
  auto ticket = owner->CaptureFormatHandoff();
  REQUIRE_FALSE(ticket->error());
  ticket->ClosePublishedPublicView();
  owner.reset();
  REQUIRE_FALSE(ManagedVolumeAuthorityTestAccess::PrepareOnly(*ticket));
  REQUIRE(ticket->phase() == 1);
  if (stop >= 1) {
    const auto receipt = ManagedVolumeAuthority::ExecuteFormat(*ticket);
    REQUIRE_FALSE(receipt.error);
    REQUIRE(receipt.complete);
    REQUIRE_FALSE(receipt.current);
    REQUIRE_FALSE(std::filesystem::exists(scratch.path / "save.bin"));
    // This fixture-only host write proves recovery of completed identity does
    // not perform another data reset, including after actual OwnersReady.
    std::ofstream(scratch.path / "after-reset.bin") << "must survive resume";
  }
  if (stop == 2) {
    auto ready = ManagedVolumeAuthorityTestAccess::OwnersReadyOnly(*ticket);
    REQUIRE(ready);
    REQUIRE_FALSE(ready->error());
    REQUIRE(ticket->phase() == 2);
    ready.reset();
  }
  ticket.reset();  // Simulated process-owner loss; real leases/metadata close.
  const auto interrupted = TreeBytes(scratch.path);
  auto ordinary = ManagedVolumeAuthority::Open(ExistingFormatOptions(scratch));
  REQUIRE(ordinary);
  REQUIRE(ordinary->error() == std::errc::operation_in_progress);
  REQUIRE_FALSE(ordinary->profile_writable());
  REQUIRE(TreeBytes(scratch.path) == interrupted);
  ordinary.reset();
  auto recovery = ManagedVolumeAuthority::InspectFormatRecovery(
      ExistingFormatOptions(scratch));
  REQUIRE(recovery);
  REQUIRE_FALSE(recovery->error());
  REQUIRE(recovery->phase() == (stop == 2 ? 2 : 1));
  REQUIRE(TreeBytes(scratch.path) == interrupted);
  auto next = ManagedVolumeAuthority::ResumeFormat(*recovery);
  REQUIRE(next);
  REQUIRE_FALSE(next->error());
  REQUIRE(next->identity()->uuid() == uuid);
  REQUIRE(next->identity()->content_epoch() == old_epoch + 1);
  REQUIRE(next->intents()->managed_identity() == next->identity());
  REQUIRE_FALSE(next->identity()->CheckCurrent());
  REQUIRE_FALSE(std::filesystem::exists(scratch.path / "save.bin"));
  REQUIRE(std::filesystem::exists(scratch.path / "after-reset.bin") == (stop >= 1));
}
TEST_CASE("Inspected format keeps common guest admission sealed through real commit and retry",
          "[managed_volume_authority][format][recovery][guest_seal]") {
  const int phase=GENERATE(0,1,2);
  const bool stop_after_admission=GENERATE(false,true);
  VolumeScratch scratch;
  auto owner=scratch.Open();
  const auto uuid=owner->identity()->uuid();
  const auto epoch=owner->identity()->content_epoch();
  const auto public_path=owner->public_root()->path();
  auto ticket=owner->CaptureFormatHandoff();
  REQUIRE(ticket);REQUIRE_FALSE(ticket->error());
  ticket->ClosePublishedPublicView();owner.reset();
  REQUIRE_FALSE(ManagedVolumeAuthorityTestAccess::PrepareOnly(*ticket));
  if (phase>=1) {
    const auto reset=ManagedVolumeAuthority::ExecuteFormat(*ticket);
    REQUIRE_FALSE(reset.error);REQUIRE(reset.complete);
    std::ofstream(scratch.path/"after-reset.bin")<<"must not be reset again";
  }
  if (phase==2) {
    auto ready=ManagedVolumeAuthorityTestAccess::OwnersReadyOnly(*ticket);
    REQUIRE(ready);REQUIRE_FALSE(ready->error());REQUIRE(ticket->phase()==2);
    ready.reset();
  }
  ticket.reset();
  ticket=ManagedVolumeAuthority::InspectFormatRecovery(ExistingFormatOptions(scratch));
  REQUIRE(ticket);REQUIRE_FALSE(ticket->error());
  auto observed=ManagedVolumeAuthorityTestAccess::ObservedFormatSource(*ticket);
  auto public_authority=ManagedVolumeAuthorityTestAccess::FormatPublicAuthority(*ticket);
  REQUIRE(observed);REQUIRE(public_authority);
  const auto old_backing=observed->GuestBacking(); // retain original sealed projection
  const auto before=TreeBytes(scratch.path);
  auto require_pending=[](const std::shared_ptr<const HostStorageRoot>& root) {
    std::error_code error;
    REQUIRE_FALSE(root->GuestView(error));REQUIRE(error==std::errc::operation_in_progress);
    REQUIRE_FALSE(HostStorageRoot::AdmitGuestMount(
        std::vector<GuestHostBacking>{root->GuestBacking()},error));
    REQUIRE(error==std::errc::operation_in_progress);
    auto raw=HostStorageRoot::Open(root->path(),false,error,0);
    REQUIRE(raw);REQUIRE_FALSE(error);
    REQUIRE_FALSE(raw->GuestView(error));REQUIRE(error==std::errc::operation_in_progress);
    REQUIRE_FALSE(HostStorageRoot::AdmitGuestMount(
        std::vector<GuestHostBacking>{{root->path(),true}},error));
    REQUIRE(error==std::errc::operation_in_progress);
  };
  require_pending(observed);require_pending(public_authority);
  REQUIRE(TreeBytes(scratch.path)==before);
  // Resume genuinely completes reset if needed; no fake owner readiness.
  std::shared_ptr<const ManagedVolumeAuthority> current;
  if (stop_after_admission) {
    const auto reset=ManagedVolumeAuthority::ExecuteFormat(*ticket);
    REQUIRE_FALSE(reset.error);REQUIRE(reset.complete);
    auto stopped=ManagedVolumeAuthorityTestAccess::StopAfterFormatAdmission(*ticket);
    REQUIRE(stopped);REQUIRE(stopped->error()==std::errc::operation_canceled);
    REQUIRE(ticket->phase()==3);
    stopped.reset();
    std::ofstream(scratch.path/"after-admit.bin")<<"must survive admitted retry";
    current=ManagedVolumeAuthority::ResumeFormat(*ticket);
    REQUIRE(std::filesystem::is_regular_file(scratch.path/"after-admit.bin"));
  } else {
    current=ManagedVolumeAuthority::ResumeFormat(*ticket);
  }
  REQUIRE(current);REQUIRE_FALSE(current->error());REQUIRE(current->profile_writable());
  REQUIRE(current->identity()->uuid()==uuid);
  REQUIRE(current->identity()->content_epoch()==epoch+1);
  REQUIRE(std::filesystem::exists(scratch.path/"after-reset.bin")==bool(phase>=1));
  std::error_code error;
  REQUIRE_FALSE(observed->GuestView(error));REQUIRE(error==std::errc::no_such_device);
  REQUIRE_FALSE(HostStorageRoot::AdmitGuestMount(std::vector<GuestHostBacking>{old_backing},error));
  REQUIRE(error==std::errc::permission_denied);
  auto fresh=current->content()->GuestView(error);REQUIRE(fresh);REQUIRE_FALSE(error);
  auto raw_public=HostStorageRoot::Open(public_path,false,error,0);
  REQUIRE(raw_public);REQUIRE_FALSE(error);
  auto admitted_public=raw_public->GuestView(error);REQUIRE(admitted_public);REQUIRE_FALSE(error);
  const std::array<uint8_t,3> bytes{0x17,0,0xE4};
  REQUIRE_FALSE(current->public_root()->WriteNewFile("after-format.bin",bytes).error);
  std::vector<uint8_t> read;
  REQUIRE_FALSE(admitted_public->ReadFile("after-format.bin",16,read));
  REQUIRE(read==std::vector<uint8_t>(bytes.begin(),bytes.end()));
  // ticket AND old_backing remain live: no stale global SourceUnadmitted seal.
}
TEST_CASE("Nonpending format inspection cannot retain a false guest admission seal",
          "[managed_volume_authority][format][recovery][guest_seal]") {
  VolumeScratch scratch;
  auto owner=scratch.Open();REQUIRE_FALSE(owner->error());
  const auto uuid=owner->identity()->uuid();owner.reset();
  const auto before=TreeBytes(scratch.path);
  const auto refused=ManagedVolumeAuthority::InspectFormatRecovery(ExistingFormatOptions(scratch));
  REQUIRE(refused);REQUIRE(refused->error());
  REQUIRE(TreeBytes(scratch.path)==before);
  const auto current=ManagedVolumeAuthority::Open(ExistingFormatOptions(scratch));
  REQUIRE(current);REQUIRE_FALSE(current->error());REQUIRE(current->profile_writable());
  REQUIRE(current->identity()->uuid()==uuid);REQUIRE(TreeBytes(scratch.path)==before);
}
TEST_CASE("Owned format refuses a foreign raw public guest before Prepared publication",
          "[managed_volume_authority][format][foreign_guest]") {
  VolumeScratch scratch;
  auto owner=scratch.Open();
  std::error_code error;
  auto raw=HostStorageRoot::Open(owner->public_root()->path(),false,error,0);
  REQUIRE(raw);REQUIRE_FALSE(error);
  auto foreign=raw->GuestView(error);REQUIRE(foreign);REQUIRE_FALSE(error);
  auto ticket=owner->CaptureFormatHandoff();REQUIRE(ticket);REQUIRE_FALSE(ticket->error());
  ticket->ClosePublishedPublicView();owner.reset();
  const auto before=TreeBytes(scratch.path);
  const auto denied=ManagedVolumeAuthority::ExecuteFormat(*ticket);
  REQUIRE(denied.error==std::errc::device_or_resource_busy);
  REQUIRE_FALSE(denied.epoch_published);REQUIRE_FALSE(denied.data_started);
  REQUIRE(ticket->phase()==0);REQUIRE(TreeBytes(scratch.path)==before);
  foreign.reset();raw.reset();
  auto restored=ManagedVolumeAuthority::RestoreUnchangedFormat(*ticket);
  REQUIRE(restored);REQUIRE_FALSE(restored->error());
  restored.reset();
  const auto reset=ManagedVolumeAuthority::ExecuteFormat(*ticket);
  REQUIRE_FALSE(reset.error);REQUIRE(reset.complete);
  const auto current=ManagedVolumeAuthority::FromCompletedFormat(*ticket);
  REQUIRE(current);REQUIRE_FALSE(current->error());REQUIRE(current->profile_writable());
}
TEST_CASE("Managed handoff removal refuses both old and replacement directories unchanged",
          "[managed_volume_authority][format][removal]") {
  VolumeScratch scratch;
  REQUIRE_FALSE(scratch.root->Create("content", true));
  std::error_code error;
  ManagedVolumeAuthority::Options options;
  options.content = scratch.root->Subroot("content", false, error);
  REQUIRE(options.content);
  REQUIRE_FALSE(error);
  options.source_id = "removal-device";
  options.device_id = 3;
  options.state_parent_override = scratch.root;
  auto owner = ManagedVolumeAuthority::Open(std::move(options));
  REQUIRE(owner);
  REQUIRE_FALSE(owner->error());
  std::ofstream(scratch.path / "content/save.bin") << "old device";
  auto ticket = owner->CaptureFormatHandoff();
  REQUIRE(ticket);
  REQUIRE_FALSE(ticket->error());
  ticket->ClosePublishedPublicView();
  owner.reset();
  std::filesystem::rename(scratch.path / "content", scratch.path / "removed");
  REQUIRE(std::filesystem::create_directory(scratch.path / "content"));
  std::ofstream(scratch.path / "content/replacement.bin") << "replacement device";
  const auto before = TreeBytes(scratch.path);
  const auto refused = ManagedVolumeAuthority::ExecuteFormat(*ticket);
  REQUIRE(refused.error);
  REQUIRE_FALSE(refused.epoch_published);
  REQUIRE_FALSE(refused.data_started);
  REQUIRE_FALSE(refused.complete);
  REQUIRE_FALSE(refused.current);
  REQUIRE(ticket->phase() == 0);
  REQUIRE(TreeBytes(scratch.path) == before);
  REQUIRE_FALSE(std::filesystem::exists(scratch.path / "content" /
                                       ManagedDeviceIdentity::kMarker));
}
TEST_CASE("Ordinary primary reopen cannot create a missing committed epoch owner",
          "[managed_volume_authority][format][primary][recovery]") {
  VolumeScratch scratch;
  REQUIRE_FALSE(scratch.root->Create("content", true));
  REQUIRE_FALSE(scratch.root->Create("public", true));
  REQUIRE_FALSE(scratch.root->Create("public/Content", true));
  std::error_code error;
  ManagedVolumeAuthority::Options options;
  options.content = scratch.root->Subroot("content", false, error);
  REQUIRE(options.content);
  REQUIRE_FALSE(error);
  options.source_id = "hdd";
  options.device_id = 1;
  options.preserve_legacy_layout = true;
  options.legacy_private_parent = scratch.root;
  options.legacy_public_root = scratch.root->Subroot("public", false, error);
  REQUIRE(options.legacy_public_root);
  REQUIRE_FALSE(error);
  options.legacy_recovery = SnapshotRecoveryOwner::Open(scratch.root);
  REQUIRE(options.legacy_recovery);
  REQUIRE_FALSE(options.legacy_recovery->error());
  auto original = ManagedVolumeAuthority::Open(options);
  REQUIRE(original);
  REQUIRE_FALSE(original->error());
  const auto uuid = original->identity()->uuid();
  const auto epoch = original->identity()->content_epoch();
  auto ticket = original->CaptureFormatHandoff();
  REQUIRE_FALSE(ticket->error());
  ticket->ClosePublishedPublicView();
  original.reset();
  const auto reset = ManagedVolumeAuthority::ExecuteFormat(*ticket);
  REQUIRE_FALSE(reset.error);
  REQUIRE(reset.complete);
  auto completed = ManagedVolumeAuthority::FromCompletedFormat(*ticket);
  REQUIRE(completed);
  REQUIRE_FALSE(completed->error());
  REQUIRE(completed->identity()->uuid() == uuid);
  REQUIRE(completed->identity()->content_epoch() == epoch + 1);
  const auto committed_epoch = completed->intents()->root()->path();
  REQUIRE(std::filesystem::is_regular_file(committed_epoch / "owner.v1"));
  completed.reset();
  ticket.reset();
  const auto committed_tree = TreeBytes(scratch.path);
  // Existing complete primary owner still reopens noncreating normally.
  auto existing = ManagedVolumeAuthority::Open(options);
  REQUIRE(existing);
  REQUIRE_FALSE(existing->error());
  REQUIRE(existing->intents()->root()->path() == committed_epoch);
  REQUIRE(TreeBytes(scratch.path) == committed_tree);
  existing.reset();
  // Preserve every original byte in fixture quarantine; the exact authorized
  // epoch namespace is now genuinely absent after its Committed certification.
  std::filesystem::rename(committed_epoch, scratch.path / "held-epoch-owner");
  REQUIRE_FALSE(std::filesystem::exists(committed_epoch));
  std::ofstream(scratch.path / "content/after-format.bin") << "preserve data";
  const auto absent = TreeBytes(scratch.path);
  auto refused = ManagedVolumeAuthority::Open(options);
  REQUIRE(refused);
  REQUIRE(refused->error() == std::errc::no_such_file_or_directory);
  REQUIRE_FALSE(refused->profile_writable());
  REQUIRE_FALSE(std::filesystem::exists(committed_epoch));
  REQUIRE(TreeBytes(scratch.path) == absent);
}
TEST_CASE("Final Prepared admission samples both live cancellation sources after drain",
          "[managed_volume_authority][format][cancellation]") {
  const int canceled_source = GENERATE(0, 1, 2);  // Ticket, external, positive control.
  VolumeScratch scratch;
  auto owner = scratch.Open();
  std::ofstream(scratch.path / "save.bin") << "unchanged before Prepared";
  auto ticket = owner->CaptureFormatHandoff();
  REQUIRE_FALSE(ticket->error());
  ticket->ClosePublishedPublicView();
  owner.reset();
  std::atomic<bool> external{false};
  const auto before = TreeBytes(scratch.path);
  // Start with a fresh ticket and external=false, then deterministically model
  // cancellation arriving after a public precheck by entering the SAME final
  // production admission directly. This tests final-stage wiring and actual
  // native drain/Append; it does not claim a concurrent public call was run.
  REQUIRE_FALSE(external.load());
  if (canceled_source == 0) { ticket->Cancel(); }
  if (canceled_source == 1) { external.store(true); }
  const auto admitted = ManagedVolumeAuthorityTestAccess::EnterPreparedBoundary(
      *ticket, &external);
  REQUIRE(admitted.actual_source_lease);
  if (canceled_source != 2) {
    REQUIRE(admitted.error == std::errc::operation_canceled);
    REQUIRE(ticket->phase() == 0);
    REQUIRE(TreeBytes(scratch.path) == before);
    auto restored = ManagedVolumeAuthority::RestoreUnchangedFormat(*ticket);
    REQUIRE(restored);
    REQUIRE_FALSE(restored->error());
    REQUIRE(restored->profile_writable());
    REQUIRE(TreeBytes(scratch.path) == before);
  } else {
    REQUIRE_FALSE(admitted.error);
    REQUIRE(ticket->phase() == 1);
    REQUIRE(TreeBytes(scratch.path) != before);
    const auto reset = ManagedVolumeAuthority::ExecuteFormat(*ticket, &external);
    REQUIRE_FALSE(reset.error);
    REQUIRE(reset.complete);
    auto completed = ManagedVolumeAuthority::FromCompletedFormat(*ticket);
    REQUIRE(completed);
    REQUIRE_FALSE(completed->error());
    REQUIRE_FALSE(std::filesystem::exists(scratch.path / "save.bin"));
  }
}
TEST_CASE("Malformed retained managed handoff never recreates owners or deletes data",
          "[managed_volume_authority][format][recovery]") {
  VolumeScratch scratch;
  auto owner = scratch.Open();
  std::ofstream(scratch.path / "save.bin") << "must remain";
  auto ticket = owner->CaptureFormatHandoff();
  REQUIRE_FALSE(ticket->error());
  ticket->ClosePublishedPublicView();
  owner.reset();
  REQUIRE_FALSE(ManagedVolumeAuthorityTestAccess::PrepareOnly(*ticket));
  ticket.reset();
  const auto marker = scratch.path / ManagedDeviceIdentity::kMarker;
  // Simulate a torn next typed frame in actual on-disk metadata; do not repair
  // the frame or manufacture a new owner permission from its filename.
  std::ofstream(marker, std::ios::binary | std::ios::app).write("XMF", 3);
  const auto torn = TreeBytes(scratch.path);
  auto ordinary = ManagedVolumeAuthority::Open(ExistingFormatOptions(scratch));
  REQUIRE(ordinary);
  REQUIRE(ordinary->error());
  REQUIRE_FALSE(ordinary->profile_writable());
  ordinary.reset();
  auto recovery = ManagedVolumeAuthority::InspectFormatRecovery(
      ExistingFormatOptions(scratch));
  REQUIRE(recovery);
  REQUIRE(recovery->error());
  REQUIRE(TreeBytes(scratch.path) == torn);
  REQUIRE(std::filesystem::exists(scratch.path / "save.bin"));
}
#if XE_PLATFORM_LINUX
TEST_CASE(
    "Real small independent filesystem has native owner authority with "
    "applicable zero reserve",
    "[managed_volume_authority][two_filesystems]") {
  struct stat shared_info{}, temporary_info{};
  if (stat("/dev/shm", &shared_info) ||
      stat(std::filesystem::temp_directory_path().c_str(), &temporary_info) ||
      shared_info.st_dev == temporary_info.st_dev) {
    WARN(
        "Independent tmpfs is unavailable; two-filesystem execution remains "
        "unverified");
    return;
  }
  VolumeScratch main, external("/dev/shm");
  const auto before = external.root->Observe();
  REQUIRE(before.available);
  REQUIRE(before.reserve_bytes == 0);
  if (before.capacity_bytes >= kHostStorageReserve) {
    WARN("This genuine external filesystem is not a small-volume fixture");
  } else {
    REQUIRE(before.capacity_bytes < kHostStorageReserve);
  }
  VolumeScratch context_parent("/dev/shm");
  std::shared_ptr<const SnapshotRecoveryOwner> external_recovery =
      SnapshotRecoveryOwner::Open(context_parent.root);
  REQUIRE(external_recovery);
  REQUIRE_FALSE(external_recovery->error());
  std::error_code context_error;
  const auto context =
      ManagedStorageContext::Capture(context_parent.root, external_recovery,
                                     context_parent.root, context_error);
  REQUIRE(context);
  REQUIRE_FALSE(context_error);
  REQUIRE_FALSE(context->CheckCurrent());
  REQUIRE(context->data_parent()->Observe().reserve_bytes == 0);
  auto owner = external.Open();
  REQUIRE(owner->profile_writable());
  REQUIRE(owner->content()->Observe().reserve_bytes == 0);
  REQUIRE(owner->content()->Observe().capacity_bytes == before.capacity_bytes);
  REQUIRE(owner->recovery()->CheckTarget(*owner->content()) ==
          std::error_code{});
  REQUIRE(owner->recovery()->CheckTarget(*main.root) ==
          std::errc::cross_device_link);
  std::error_code error;
  auto reference = HostStorageRoot::Open("/dev/shm", false, error, 0);
  REQUIRE(reference);
  REQUIRE_FALSE(
      reference->format_eligible());  // no metadata/file creation there
  REQUIRE(owner->format_eligible());
  ManagedVolumeAuthority::Options wrong;
  wrong.content = main.root;
  wrong.source_id = "wrong-filesystem";
  wrong.device_id = 4;
  wrong.state_parent_override = external.root;
  auto refused = ManagedVolumeAuthority::Open(std::move(wrong));
  REQUIRE(refused->error() == std::errc::cross_device_link);
  REQUIRE_FALSE(
      std::filesystem::exists(main.path / ManagedDeviceIdentity::kMarker));
}
#endif
}  // namespace xe::filesystem::test

namespace xe::filesystem::test {
TEST_CASE("Content mutation owner publishes exact metadata and retains raw backup",
          "[base][storage][content_mutation]") {
  VolumeScratch scratch;
  auto volume = scratch.Open();
  auto owner = volume->mutations();
  REQUIRE(owner); REQUIRE(owner->ready());
  const std::filesystem::path package = "0000000000000001/12345678/00000001/save";
  const std::filesystem::path header = "0000000000000001/12345678/headers/00000001/save.header";
  using Kind = ContentMutationOwner::Kind;
  std::error_code error;
  auto absent = owner->CaptureMetadata(Kind::kCreate, package, header, error);
  REQUIRE(absent); REQUIRE_FALSE(error); REQUIRE_FALSE(absent->exists());
  const std::vector<uint8_t> original{0, 1, 0, 3, 255, 97, 98, 0, 99};
  auto created = owner->Create(package, absent, original);
  REQUIRE_FALSE(created.error); REQUIRE(created.complete);
  REQUIRE(created.prepared_durable); REQUIRE(created.metadata_published);
  REQUIRE(created.directory_published); REQUIRE(created.durable);
  REQUIRE(owner->Visible(package));
  auto before = owner->CaptureMetadata(Kind::kHeader, package, header, error);
  REQUIRE(before); REQUIRE_FALSE(error); REQUIRE(before->exists());
  const std::vector<uint8_t> intended{0, 1, 4, 3, 255, 97, 98, 0, 99};
  auto written = owner->WriteMetadata(Kind::kHeader, package, before, intended);
  REQUIRE_FALSE(written.error); REQUIRE(written.complete);
  REQUIRE(written.metadata_published); REQUIRE(written.durable);
  const auto record = ContentMutationOwnerTestAccess::ReadRecord(*owner, written.nonce);
  REQUIRE(record.target == header); REQUIRE(record.before == original);
  REQUIRE(record.intended == intended);
  REQUIRE(ContentMutationOwnerTestAccess::Backup(*owner, record) == original);
  auto actual = owner->CaptureMetadata(Kind::kHeader, package, header, error);
  REQUIRE(actual); REQUIRE_FALSE(error);
  REQUIRE(std::vector<uint8_t>(actual->bytes().begin(), actual->bytes().end()) == intended);
  // An old ordinary load is not publication retry authority or a new CAS.
  auto stale = owner->WriteMetadata(Kind::kHeader, package, before, original);
  REQUIRE(stale.error == std::errc::state_not_recoverable);
  REQUIRE_FALSE(stale.metadata_published); REQUIRE_FALSE(stale.complete);

  SECTION("Foreign Header aliases still refuse before staging and release cleanly") {
    auto alias = HostStorageRoot::Open((scratch.path / header).parent_path(), false, error, 0);
    REQUIRE(alias); REQUIRE_FALSE(error);
    auto foreign = alias->OpenFile(header.filename(), FileAccess::kFileReadData, error);
    REQUIRE(foreign); REQUIRE_FALSE(error);
    auto expected = owner->CaptureMetadata(Kind::kHeader, package, header, error);
    REQUIRE(expected); REQUIRE_FALSE(error);
    const auto unchanged = TreeBytes(scratch.path);
    const std::vector<uint8_t> replacement{9, 0, 8, 7, 255, 6, 5, 0, 4};
    const auto refused = owner->WriteMetadata(Kind::kHeader, package, expected, replacement);
    REQUIRE(refused.error == std::errc::device_or_resource_busy);
    REQUIRE_FALSE(refused.prepared_durable); REQUIRE_FALSE(refused.metadata_published);
    REQUIRE_FALSE(refused.complete); REQUIRE(refused.nonce.empty());
    REQUIRE(TreeBytes(scratch.path) == unchanged);
    REQUIRE(owner->ready()); REQUIRE(owner->idle());
    foreign.reset();
    expected = owner->CaptureMetadata(Kind::kHeader, package, header, error);
    REQUIRE(expected); REQUIRE_FALSE(error);
    const auto retried = owner->WriteMetadata(Kind::kHeader, package, expected, replacement);
    REQUIRE_FALSE(retried.error); REQUIRE(retried.complete); REQUIRE(retried.durable);
    const auto retry_record = ContentMutationOwnerTestAccess::ReadRecord(*owner, retried.nonce);
    REQUIRE(retry_record.before == intended); REQUIRE(retry_record.intended == replacement);
    REQUIRE(ContentMutationOwnerTestAccess::Backup(*owner, retry_record) == intended);
  }

  SECTION("Uncertain stage-parent durability cannot publish a prepared intent") {
    // The successful operations above wrote real Prepared + Done and retained
    // the exact old-byte backup. Failure below uses that SAME owner and target.
    const auto recovery_marker = ContentMutationOwnerTestAccess::RecoveryMarker(*owner);
    auto expected = owner->CaptureMetadata(Kind::kHeader, package, header, error);
    REQUIRE(expected); REQUIRE_FALSE(error);
    const std::vector<uint8_t> uncertain{9, 8, 0, 6, 5, 4, 3, 2, 1};
    const auto refused = ContentMutationOwnerTestAccess::UncertainStageParent(
        *owner, package, expected, uncertain);
    REQUIRE(refused.error == std::errc::io_error);
    REQUIRE(refused.metadata.phase == SnapshotPublicationPhase::kStageFlush);
    REQUIRE(refused.metadata.stage_created);
    REQUIRE(refused.metadata.staged_count == uncertain.size());
    REQUIRE_FALSE(refused.prepared_durable);
    REQUIRE_FALSE(refused.metadata_published);
    REQUIRE_FALSE(refused.directory_published);
    REQUIRE_FALSE(refused.complete);
    REQUIRE_FALSE(refused.metadata.recovery_created);
    ContentMutationOwnerTestAccess::NoJournal(*owner, refused.nonce);
    REQUIRE(ContentMutationOwnerTestAccess::PendingBytes(
        *owner, header, header) == intended);
    REQUIRE(ContentMutationOwnerTestAccess::PendingBytes(
        *owner, header, refused.metadata.stage) == uncertain);
    REQUIRE(ContentMutationOwnerTestAccess::RecoveryMarker(*owner) == recovery_marker);
    REQUIRE(ContentMutationOwnerTestAccess::Backup(*owner, record) == original);
    REQUIRE_FALSE(owner->ready()); REQUIRE_FALSE(owner->Visible(package));
    // No durable intent authenticates this stage. Recovery must retain it and
    // deny availability instead of guessing, deleting it or claiming success.
    REQUIRE(owner->Recover() == std::errc::state_not_recoverable);
    REQUIRE_FALSE(owner->ready());
    ContentMutationOwnerTestAccess::NoJournal(*owner, refused.nonce);
    REQUIRE(ContentMutationOwnerTestAccess::PendingBytes(
        *owner, header, header) == intended);
    REQUIRE(ContentMutationOwnerTestAccess::PendingBytes(
        *owner, header, refused.metadata.stage) == uncertain);
  }
}

TEST_CASE("Content create restart observes its actual published header and directory",
          "[base][storage][content_mutation]") {
  VolumeScratch scratch;
  auto volume = scratch.Open();
  auto owner = volume->mutations();
  const std::filesystem::path package = "0000000000000001/12345678/00000001/restart";
  const std::filesystem::path header = "0000000000000001/12345678/Headers/00000001/restart.header";
  std::error_code error;
  auto expected = owner->CaptureMetadata(ContentMutationOwner::Kind::kCreate, package, header, error);
  REQUIRE(expected); REQUIRE_FALSE(error);
  const std::vector<uint8_t> bytes{1, 2, 3, 0, 4};
  const auto result = owner->Create(package, expected, bytes);
  REQUIRE(result.complete); REQUIRE_FALSE(result.error);
  const auto record = ContentMutationOwnerTestAccess::ReadRecord(*owner, result.nonce);
  ContentMutationOwnerTestAccess::LoseDone(*owner, result.nonce);
  const auto epoch = volume->identity()->content_epoch();
  const auto uuid = volume->identity()->uuid();
  owner.reset(); volume.reset(); expected.reset();
  volume = scratch.Open(); owner = volume->mutations();
  REQUIRE(owner->ready()); REQUIRE(owner->Visible(package));
  REQUIRE(volume->identity()->uuid() == uuid);
  REQUIRE(volume->identity()->content_epoch() == epoch);
  auto current = owner->CaptureMetadata(ContentMutationOwner::Kind::kHeader, package, header, error);
  REQUIRE(current); REQUIRE_FALSE(error);
  REQUIRE(current->info().file_id == record.stage_file);
  REQUIRE(std::vector<uint8_t>(current->bytes().begin(), current->bytes().end()) == bytes);
  // Recovery completion made the real package eligible for subsequent data.
  auto view = volume->content()->Subroot(package, false, error);
  REQUIRE(view); REQUIRE_FALSE(error);
  REQUIRE_FALSE(view->WriteNewFile("later", bytes).error);
  REQUIRE(owner->Recover() == std::error_code{});
  std::vector<uint8_t> later;
  REQUIRE_FALSE(view->ReadFile("later", 64, later)); REQUIRE(later == bytes);
}

TEST_CASE("Content journal preserves an unrecognized published third state",
          "[base][storage][content_mutation]") {
  VolumeScratch scratch; auto volume = scratch.Open(); auto owner = volume->mutations();
  const std::filesystem::path package = "0000000000000001/12345678/00000001/foreign";
  const std::filesystem::path header = "0000000000000001/12345678/Headers/00000001/foreign.header";
  std::error_code error;
  auto expected = owner->CaptureMetadata(ContentMutationOwner::Kind::kCreate, package, header, error);
  REQUIRE(expected); REQUIRE_FALSE(error);
  const std::vector<uint8_t> intended{1,2,3,4};
  const auto result = owner->Create(package, expected, intended);
  REQUIRE(result.complete);
  ContentMutationOwnerTestAccess::LoseDone(*owner, result.nonce);
  // Noncooperating host mutation is not adopted as success or replaced.
  const std::vector<uint8_t> foreign{9,8,7,6};
  { std::ofstream file(scratch.path / header, std::ios::binary | std::ios::trunc);
    REQUIRE(file.good()); file.write(reinterpret_cast<const char*>(foreign.data()), foreign.size()); }
  REQUIRE(owner->Recover() == std::errc::state_not_recoverable);
  REQUIRE_FALSE(owner->ready()); REQUIRE_FALSE(owner->Visible(package));
  std::ifstream actual(scratch.path / header, std::ios::binary);
  REQUIRE(std::vector<uint8_t>(std::istreambuf_iterator<char>(actual), {}) == foreign);
}

TEST_CASE("Content cancellation before preparation leaves no package or publication",
          "[base][storage][content_mutation]") {
  VolumeScratch scratch; auto volume = scratch.Open(); auto owner = volume->mutations();
  const std::filesystem::path package = "0000000000000001/12345678/00000001/canceled";
  const std::filesystem::path header = "0000000000000001/12345678/Headers/00000001/canceled.header";
  std::error_code error;
  auto expected = owner->CaptureMetadata(ContentMutationOwner::Kind::kCreate, package, header, error);
  REQUIRE(expected); REQUIRE_FALSE(error);
  std::atomic<bool> canceled{true};
  const auto before = TreeBytes(scratch.path);
  const std::array<uint8_t,3> bytes{1,2,3};
  const auto result = owner->Create(package, expected, bytes, &canceled);
  REQUIRE(result.error == std::errc::operation_canceled);
  REQUIRE_FALSE(result.prepared_durable); REQUIRE_FALSE(result.metadata_published);
  REQUIRE_FALSE(result.directory_published); REQUIRE_FALSE(result.complete);
  REQUIRE(TreeBytes(scratch.path) == before); REQUIRE(owner->ready());
}

TEST_CASE("Content flush closes new opens while old handles finish real writes",
          "[base][storage][content_mutation]") {
  VolumeScratch scratch; auto volume = scratch.Open(); auto owner = volume->mutations();
  const std::filesystem::path package = "0000000000000001/12345678/00000001/flush";
  const std::filesystem::path header = "0000000000000001/12345678/Headers/00000001/flush.header";
  std::error_code error;
  auto expected = owner->CaptureMetadata(ContentMutationOwner::Kind::kCreate, package, header, error);
  REQUIRE(expected); REQUIRE_FALSE(error);
  const std::array<uint8_t,3> bytes{1,2,3};
  REQUIRE(owner->Create(package, expected, bytes).complete);
  auto view = volume->content()->Subroot(package, false, error);
  REQUIRE(view); REQUIRE_FALSE(error);
  auto old = view->OpenFile("data", FileAccess::kGenericAll, error, true);
  REQUIRE(old); REQUIRE_FALSE(error);
  auto reservation = owner->BeginFlush(package, error);
  REQUIRE(reservation); REQUIRE_FALSE(error); REQUIRE_FALSE(owner->idle());
  REQUIRE_FALSE(volume->format_eligible());
  auto denied = view->OpenFile("new", FileAccess::kGenericAll, error, true);
  REQUIRE_FALSE(denied); REQUIRE(error == std::errc::device_or_resource_busy);
  const auto wrote = old->WriteResult(0, bytes);
  REQUIRE_FALSE(wrote.error); REQUIRE(wrote.bytes == bytes.size());
  REQUIRE_FALSE(owner->Flush(*reservation, header));
  reservation.reset(); REQUIRE(owner->idle());
  auto admitted = view->OpenFile("new", FileAccess::kGenericAll, error, true);
  REQUIRE(admitted); REQUIRE_FALSE(error);
}

TEST_CASE("Content mutation owner refuses case-ambiguous physical ancestry",
          "[base][storage][content_mutation]") {
  VolumeScratch scratch; auto volume = scratch.Open(); auto owner = volume->mutations();
  const std::filesystem::path package = "a000000000000001/12345678/00000001/save";
  const std::filesystem::path header = "a000000000000001/12345678/Headers/00000001/save.header";
  REQUIRE_FALSE(scratch.root->Create("a000000000000001", true));
  REQUIRE_FALSE(scratch.root->Create("A000000000000001", true));
  std::error_code error;
  const auto token = owner->CaptureMetadata(ContentMutationOwner::Kind::kCreate,
                                              package, header, error);
  REQUIRE_FALSE(token); REQUIRE(error);
  REQUIRE_FALSE(std::filesystem::exists(scratch.path / package));
  REQUIRE_FALSE(std::filesystem::exists(scratch.path / header));
  REQUIRE(owner->ready());
}

TEST_CASE("Content thumbnail publication uses its own exact parent reservation",
          "[base][storage][content_mutation]") {
  VolumeScratch scratch; auto volume = scratch.Open(); auto owner = volume->mutations();
  const std::filesystem::path package = "0000000000000001/12345678/00000001/thumb";
  const std::filesystem::path header = "0000000000000001/12345678/Headers/00000001/thumb.header";
  std::error_code error;
  auto expected = owner->CaptureMetadata(ContentMutationOwner::Kind::kCreate, package, header, error);
  REQUIRE(expected); REQUIRE_FALSE(error);
  const std::array<uint8_t,3> bytes{1,2,3};
  REQUIRE(owner->Create(package, expected, bytes).complete);
  const auto target = package / "__thumbnail.png";
  auto missing = owner->CaptureMetadata(ContentMutationOwner::Kind::kThumbnail, package, target, error);
  REQUIRE(missing); REQUIRE_FALSE(error); REQUIRE_FALSE(missing->exists());
  const auto result = owner->WriteMetadata(ContentMutationOwner::Kind::kThumbnail,
                                          package, missing, bytes);
  REQUIRE_FALSE(result.error); REQUIRE(result.complete); REQUIRE(result.metadata_published);
  auto current = owner->CaptureMetadata(ContentMutationOwner::Kind::kThumbnail, package, target, error);
  REQUIRE(current); REQUIRE_FALSE(error);
  const auto empty = owner->WriteMetadata(ContentMutationOwner::Kind::kThumbnail,
                                          package, current, {});
  REQUIRE_FALSE(empty.error); REQUIRE(empty.complete);
  current = owner->CaptureMetadata(ContentMutationOwner::Kind::kThumbnail, package, target, error);
  REQUIRE(current); REQUIRE_FALSE(error); REQUIRE(current->exists()); REQUIRE(current->bytes().empty());
}

TEST_CASE("Content flush commits actual overwrite EOF without weakening dirty leases",
          "[base][storage][content_mutation]") {
  VolumeScratch scratch; auto volume = scratch.Open(); auto owner = volume->mutations();
  const std::filesystem::path package = "0000000000000001/12345678/00000001/overwrite";
  const std::filesystem::path header = "0000000000000001/12345678/Headers/00000001/overwrite.header";
  std::error_code error;
  auto expected = owner->CaptureMetadata(ContentMutationOwner::Kind::kCreate, package, header, error);
  REQUIRE(expected); REQUIRE_FALSE(error);
  const std::array<uint8_t,3> bytes{1,2,3};
  REQUIRE(owner->Create(package, expected, bytes).complete);
  auto view = volume->content()->Subroot(package, false, error);
  REQUIRE(view); REQUIRE_FALSE(error);
  const std::array<uint8_t,8> original{7,7,7,7,7,7,7,7};
  REQUIRE_FALSE(view->WriteNewFile("data", original).error);
  auto overwrite = view->ReserveOverwrite("data", error);
  REQUIRE(overwrite); REQUIRE_FALSE(error);
  auto reservation = owner->BeginFlush(package, error);
  REQUIRE(reservation); REQUIRE_FALSE(error);
  REQUIRE_FALSE(overwrite->WriteResult(0, bytes).error);
  REQUIRE(owner->Flush(*reservation, header) == std::errc::device_or_resource_busy);
  const auto eof = overwrite->FlushLengthResult(bytes.size());
  REQUIRE_FALSE(eof.error); REQUIRE(eof.durable); REQUIRE(eof.published_length == bytes.size());
  REQUIRE_FALSE(owner->Flush(*reservation, header));
  REQUIRE(std::filesystem::file_size(scratch.path / package / "data") == bytes.size());
  REQUIRE_FALSE(overwrite->WriteResult(0, std::span(bytes).first(1)).error);
  REQUIRE(owner->Flush(*reservation, header) == std::errc::device_or_resource_busy);
  REQUIRE_FALSE(overwrite->FlushLengthResult(bytes.size()).error);
  REQUIRE_FALSE(owner->Flush(*reservation, header));
  // Public/unreserved tree flush still refuses an open exclusive owner.
  reservation.reset();
  REQUIRE(view->FlushTree() == std::errc::device_or_resource_busy);
}

TEST_CASE("Content journal reserves intent and done capacity before any stage",
          "[base][storage][content_mutation]") {
  VolumeScratch scratch; auto volume = scratch.Open(); auto owner = volume->mutations();
  const std::filesystem::path package = "0000000000000001/12345678/00000001/budget";
  const std::filesystem::path header = "0000000000000001/12345678/Headers/00000001/budget.header";
  std::error_code error;
  auto expected = owner->CaptureMetadata(ContentMutationOwner::Kind::kCreate, package, header, error);
  REQUIRE(expected); REQUIRE_FALSE(error);
  const std::array<uint8_t,3> bytes{1,2,3};
  REQUIRE(owner->Create(package, expected, bytes).complete);
  // The real first transaction owns marker+plan+intent+done. Lower only the
  // private budget to four: the next atomic intent/Done append must refuse
  // before private stage creation, including its temporary record slots.
  ContentMutationOwnerTestAccess::LimitRecords(*owner, 4);
  auto before = owner->CaptureMetadata(ContentMutationOwner::Kind::kHeader, package, header, error);
  REQUIRE(before); REQUIRE_FALSE(error);
  const auto tree = TreeBytes(scratch.path);
  const std::array<uint8_t,3> changed{4,5,6};
  const auto result = owner->WriteMetadata(ContentMutationOwner::Kind::kHeader,
                                          package, before, changed);
  REQUIRE(result.error == std::errc::no_space_on_device);
  REQUIRE_FALSE(result.prepared_durable); REQUIRE_FALSE(result.metadata_published);
  REQUIRE_FALSE(result.complete); REQUIRE(TreeBytes(scratch.path) == tree);
  REQUIRE(owner->ready());
}

TEST_CASE("Legacy volumes share a real parent without sharing content journals",
          "[base][storage][content_mutation]") {
  VolumeScratch scratch;
  std::error_code error;
  auto recovery = std::shared_ptr<const SnapshotRecoveryOwner>(SnapshotRecoveryOwner::Open(scratch.root));
  REQUIRE(recovery); REQUIRE_FALSE(recovery->error());
  std::vector<std::shared_ptr<const ManagedVolumeAuthority>> volumes;
  for (unsigned i = 0; i != 2; ++i) {
    const auto source = "source" + std::to_string(i);
    const auto shell = "shell" + std::to_string(i);
    REQUIRE_FALSE(scratch.root->Create(source, true));
    REQUIRE_FALSE(scratch.root->Create(shell, true));
    auto content = scratch.root->Subroot(source, false, error);
    REQUIRE(content); REQUIRE_FALSE(error);
    auto public_root = scratch.root->Subroot(shell, false, error);
    REQUIRE(public_root); REQUIRE_FALSE(error);
    REQUIRE_FALSE(public_root->Create("Content", true));
    ManagedVolumeAuthority::Options options;
    options.content = content; options.source_id = source; options.device_id = i + 1;
    options.preserve_legacy_layout = true;
    options.legacy_private_parent = scratch.root;
    options.legacy_public_root = public_root; options.legacy_recovery = recovery;
    auto volume = ManagedVolumeAuthority::Open(std::move(options));
    REQUIRE(volume); REQUIRE_FALSE(volume->error()); REQUIRE(volume->mutations()->ready());
    volumes.push_back(std::move(volume));
  }
  REQUIRE(volumes[0]->identity()->uuid() != volumes[1]->identity()->uuid());
  const std::filesystem::path package = "0000000000000001/12345678/00000001/same";
  const std::filesystem::path header = "0000000000000001/12345678/Headers/00000001/same.header";
  for (unsigned i = 0; i != 2; ++i) {
    auto owner = volumes[i]->mutations();
    auto expected = owner->CaptureMetadata(ContentMutationOwner::Kind::kCreate, package, header, error);
    REQUIRE(expected); REQUIRE_FALSE(error);
    const std::array<uint8_t,1> bytes{uint8_t(i)};
    REQUIRE(owner->Create(package, expected, bytes).complete);
    auto current = owner->CaptureMetadata(ContentMutationOwner::Kind::kHeader, package, header, error);
    REQUIRE(current); REQUIRE_FALSE(error); REQUIRE(current->bytes()[0] == i);
  }
}

TEST_CASE("Content package tree capture retains real bounded source identities",
          "[base][storage][content_tree]") {
  VolumeScratch scratch; auto volume = scratch.Open(); auto owner = volume->mutations();
  const std::filesystem::path package = "0000000000000001/12345678/00000001/tree";
  const std::filesystem::path header = "0000000000000001/12345678/Headers/00000001/tree.header";
  std::error_code error;
  auto absent = owner->CaptureMetadata(ContentMutationOwner::Kind::kCreate, package, header, error);
  REQUIRE(absent); REQUIRE_FALSE(error);
  const std::array<uint8_t,3> data{0,1,2};
  REQUIRE(owner->Create(package, absent, data).complete);
  REQUIRE_FALSE(volume->content()->Create(package / "sub", true));
  REQUIRE_FALSE(volume->content()->WriteNewFile(package / "sub/data", data).error);
  auto tree = owner->CapturePackage(package, error);
  REQUIRE(tree); REQUIRE_FALSE(error);
  REQUIRE(tree->relative() == package); REQUIRE(tree->top_info().directory);
  REQUIRE(tree->entries().size() == 3); REQUIRE(tree->entries()[0].relative.empty());
  const auto file = std::find_if(tree->entries().begin(), tree->entries().end(),
      [](const auto& entry) { return entry.relative == "sub/data"; });
  REQUIRE(file != tree->entries().end()); REQUIRE_FALSE(file->directory);
  REQUIRE(file->file != 0); REQUIRE(file->bytes == data.size());
  REQUIRE(file->write_nanoseconds < 1000000000u);
}
TEST_CASE("Content package tree capture refuses real open members and unknown aliases",
          "[base][storage][content_tree]") {
  VolumeScratch scratch; auto volume = scratch.Open(); auto owner = volume->mutations();
  const std::filesystem::path package = "0000000000000001/12345678/00000001/leases";
  const std::filesystem::path header = "0000000000000001/12345678/Headers/00000001/leases.header";
  std::error_code error;
  auto absent = owner->CaptureMetadata(ContentMutationOwner::Kind::kCreate, package, header, error);
  REQUIRE(absent); REQUIRE_FALSE(error);
  const std::array<uint8_t,3> data{1,2,3};
  REQUIRE(owner->Create(package, absent, data).complete);
  REQUIRE_FALSE(volume->content()->WriteNewFile(package / "data", data).error);
  auto open = volume->content()->OpenFile(package / "data", FileAccess::kFileReadData, error);
  REQUIRE(open); REQUIRE_FALSE(error);
  REQUIRE_FALSE(owner->CapturePackage(package, error));
  REQUIRE(error == std::errc::device_or_resource_busy);
  open.reset();
  auto captured = owner->CapturePackage(package, error);
  REQUIRE(captured); REQUIRE_FALSE(error); captured.reset();
  std::filesystem::create_hard_link(scratch.path / package / "data", scratch.path / package / "alias");
  const auto before = TreeBytes(scratch.path);
  REQUIRE_FALSE(owner->CapturePackage(package, error)); REQUIRE(error);
  REQUIRE(TreeBytes(scratch.path) == before);
  REQUIRE(owner->ready());
}
TEST_CASE("Content package tree capture cannot include a retained protected subtree",
          "[base][storage][content_tree]") {
  VolumeScratch scratch;
  const std::filesystem::path package = "0000000000000001/12345678/00000001/protected";
  for (const auto& relative : {std::filesystem::path("0000000000000001"),
       std::filesystem::path("0000000000000001/12345678"),
       std::filesystem::path("0000000000000001/12345678/00000001"), package,
       package / "private"}) { REQUIRE_FALSE(scratch.root->Create(relative, true)); }
  std::error_code error;
  auto private_root = scratch.root->Subroot(package / "private", false, error);
  REQUIRE(private_root); REQUIRE_FALSE(error);
  auto protected_root = private_root->ProtectFlat(error);
  REQUIRE(protected_root); REQUIRE_FALSE(error);
  const std::array<uint8_t,3> sentinel{5,6,7};
  REQUIRE_FALSE(protected_root->WriteNewFile("sentinel", sentinel).error);
  auto volume = scratch.Open();
  const auto before = TreeBytes(scratch.path);
  REQUIRE_FALSE(volume->mutations()->CapturePackage(package, error)); REQUIRE(error);
  REQUIRE(TreeBytes(scratch.path) == before);
  std::vector<uint8_t> actual;
  REQUIRE_FALSE(protected_root->ReadFile("sentinel", 64, actual));
  REQUIRE(actual == std::vector<uint8_t>(sentinel.begin(), sentinel.end()));
}
}  // namespace xe::filesystem::test
#endif

#if XE_PLATFORM_LINUX || XE_PLATFORM_MAC
namespace xe::filesystem::test {
TEST_CASE("Content delete retires exact package data only after durable typed intent",
          "[base][storage][content_package]") {
  VolumeScratch scratch; auto volume = scratch.Open(); auto owner = volume->mutations();
  const std::filesystem::path package = "0000000000000001/12345678/00000001/delete";
  const std::filesystem::path header = "0000000000000001/12345678/Headers/00000001/delete.header";
  std::error_code error;
  const std::vector<uint8_t> raw_header{0, 4, 0, 9, 255, 1};
  auto missing = owner->CaptureMetadata(ContentMutationOwner::Kind::kCreate, package, header, error);
  REQUIRE(missing); REQUIRE_FALSE(error);
  REQUIRE(owner->Create(package, missing, raw_header).complete);
  auto data = volume->content()->Subroot(package, false, error);
  REQUIRE(data); REQUIRE_FALSE(error);
  REQUIRE_FALSE(data->Create("nested", true));
  const std::array<uint8_t,5> payload{1, 2, 0, 4, 5};
  REQUIRE_FALSE(data->WriteNewFile("nested/save", payload).error);
  REQUIRE_FALSE(data->WriteNewFile("__thumbnail.png", payload).error);
  data.reset(); missing.reset();
  auto tree = owner->CapturePackage(package, error);
  REQUIRE(tree); REQUIRE_FALSE(error); REQUIRE(tree->entries().size() == 4);
  const auto before_entries = tree->entries();
  auto current = owner->CaptureMetadata(ContentMutationOwner::Kind::kHeader, package, header, error);
  REQUIRE(current); REQUIRE_FALSE(error);
  // An unrelated real namespace on the SAME pooled filesystem must stay
  // protected while the content owner checks its private staged payload.
  // Those checks need the operation's retained coordinate, not "../" relative
  // to the public root and not an exception for foreign claims.
  VolumeScratch unrelated;
  const std::array<uint8_t, 4> unrelated_bytes{9, 0, 8, 7};
  REQUIRE_FALSE(unrelated.root->WriteNewFile("Account", unrelated_bytes).error);
  auto unrelated_token = unrelated.root->CaptureSnapshot("Account", 64, error);
  REQUIRE(unrelated_token); REQUIRE_FALSE(error);
  REQUIRE(unrelated_token->info().filesystem_id == current->info().filesystem_id);
  auto unrelated_claim = unrelated.root->ReserveSnapshotFile(unrelated_token, error);
  REQUIRE(unrelated_claim); REQUIRE_FALSE(error);
  REQUIRE(unrelated_claim->IsSnapshotReservationOf(*unrelated.root));
  std::vector<uint8_t> unrelated_actual;
  REQUIRE(unrelated.root->ReadFile("Account", 64, unrelated_actual) ==
          std::errc::device_or_resource_busy);
  const auto deleted = owner->Delete(std::move(tree), current);
  REQUIRE_FALSE(deleted.error); REQUIRE(deleted.complete);
  REQUIRE(deleted.prepared_durable); REQUIRE(deleted.quarantined);
  REQUIRE(deleted.header_removed); REQUIRE(deleted.retirement_durable);
  REQUIRE(deleted.space_retired); REQUIRE_FALSE(deleted.observed);
  REQUIRE_FALSE(std::filesystem::exists(scratch.path / package));
  REQUIRE_FALSE(std::filesystem::exists(scratch.path / header));
  REQUIRE(owner->ready());
  const auto record = ContentMutationOwnerTestAccess::PackageRecord(*owner, deleted.nonce);
  REQUIRE(record.before == before_entries); REQUIRE(record.header == raw_header);
  REQUIRE(record.header_exists); REQUIRE(record.kind == content_package_mutation_wire::Kind::kDelete);
  REQUIRE_FALSE(owner->Recover()); REQUIRE(owner->ready());
  REQUIRE_FALSE(unrelated_claim->ReadFile("Account", 64, unrelated_actual));
  REQUIRE(unrelated_actual == std::vector<uint8_t>(unrelated_bytes.begin(), unrelated_bytes.end()));
  REQUIRE(unrelated.root->ReadFile("Account", 64, unrelated_actual) ==
          std::errc::device_or_resource_busy);
  unrelated_claim->Retire();
  REQUIRE_FALSE(unrelated_claim->ReleaseRetiredSnapshotReservation());
  unrelated_claim.reset(); unrelated_token.reset();
  REQUIRE_FALSE(unrelated.root->ReadFile("Account", 64, unrelated_actual));
  REQUIRE(unrelated_actual == std::vector<uint8_t>(unrelated_bytes.begin(), unrelated_bytes.end()));
  SECTION("A lost Done reconciles actual retired state without fake publication") {
    ContentMutationOwnerTestAccess::LoseDone(*owner, deleted.nonce);
    owner.reset(); volume.reset(); current.reset();
    volume = scratch.Open(); owner = volume->mutations();
    REQUIRE_FALSE(volume->error()); REQUIRE(owner->ready());
    REQUIRE_FALSE(std::filesystem::exists(scratch.path / package));
    REQUIRE_FALSE(std::filesystem::exists(scratch.path / header));
    REQUIRE_FALSE(owner->Recover());
    REQUIRE(ContentMutationOwnerTestAccess::PackageRecord(*owner, deleted.nonce) == record);
  }
}
TEST_CASE("Content delete restart resumes its real prepared tree and raw Header",
          "[base][storage][content_package]") {
  VolumeScratch scratch; auto volume = scratch.Open(); auto owner = volume->mutations();
  const std::filesystem::path package = "0000000000000001/12345678/00000001/restart_delete";
  const std::filesystem::path header = "0000000000000001/12345678/Headers/00000001/restart_delete.header";
  std::error_code error; const std::array<uint8_t,3> bytes{4, 0, 6};
  auto token = owner->CaptureMetadata(ContentMutationOwner::Kind::kCreate, package, header, error);
  REQUIRE(token); REQUIRE_FALSE(error); REQUIRE(owner->Create(package, token, bytes).complete);
  auto data = volume->content()->Subroot(package, false, error);
  REQUIRE(data); REQUIRE_FALSE(error); REQUIRE_FALSE(data->WriteNewFile("save", bytes).error);
  data.reset();
  auto tree = owner->CapturePackage(package, error);
  REQUIRE(tree); REQUIRE_FALSE(error);
  token = owner->CaptureMetadata(ContentMutationOwner::Kind::kHeader, package, header, error);
  REQUIRE(token); REQUIRE_FALSE(error);
  ContentMutationOwner::PackageReceipt prepared;
  bool quarantined = false, header_committed = false;
  SECTION("Prepared with the original package and Header still public") {
    prepared = ContentMutationOwnerTestAccess::PreparePackageDelete(*owner, std::move(tree), token);
  }
  SECTION("Actual quarantine completed but Header is unchanged and no Retire exists") {
    quarantined = true;
    prepared = ContentMutationOwnerTestAccess::StopPackageDeleteBeforeRetire(
        *owner, std::move(tree), token, false);
  }
  SECTION("Actual Header CAS completed after quarantine but no Retire exists") {
    quarantined = true; header_committed = true;
    prepared = ContentMutationOwnerTestAccess::StopPackageDeleteBeforeRetire(
        *owner, std::move(tree), token, true);
  }
  REQUIRE(prepared.error == std::errc::operation_in_progress);
  REQUIRE(prepared.prepared_durable); REQUIRE(prepared.quarantined == quarantined);
  REQUIRE(prepared.header_removed == header_committed);
  REQUIRE_FALSE(prepared.retirement_durable); REQUIRE_FALSE(prepared.complete);
  ContentMutationOwnerTestAccess::NoRetireOrDone(*owner, prepared.nonce);
  REQUIRE(std::filesystem::exists(scratch.path / package / "save") == !quarantined);
  REQUIRE(std::filesystem::exists(scratch.path / header) == !header_committed);
  if (quarantined) {
    auto actual = ContentMutationOwnerTestAccess::PackageOperation(*owner, prepared.nonce);
    std::vector<uint8_t> retained;
    REQUIRE_FALSE(actual->ReadFile("payload/save", 64, retained));
    REQUIRE(retained == std::vector<uint8_t>(bytes.begin(), bytes.end()));
  }
  const auto original_record = ContentMutationOwnerTestAccess::PackageRecord(*owner, prepared.nonce);
  REQUIRE(original_record.header == std::vector<uint8_t>(bytes.begin(), bytes.end()));
  REQUIRE_FALSE(owner->ready());
  owner.reset(); volume.reset(); token.reset();
  volume = scratch.Open(); owner = volume->mutations();
  REQUIRE_FALSE(volume->error()); REQUIRE(owner->ready());
  REQUIRE_FALSE(std::filesystem::exists(scratch.path / package));
  REQUIRE_FALSE(std::filesystem::exists(scratch.path / header));
  REQUIRE_FALSE(owner->Recover());
  const auto record = ContentMutationOwnerTestAccess::PackageRecord(*owner, prepared.nonce);
  REQUIRE(record.header == std::vector<uint8_t>(bytes.begin(), bytes.end()));
}
TEST_CASE("Content delete preserves changed trees open leases and canceled input",
          "[base][storage][content_package]") {
  VolumeScratch scratch; auto volume = scratch.Open(); auto owner = volume->mutations();
  const std::filesystem::path package = "0000000000000001/12345678/00000001/foreign_delete";
  const std::filesystem::path header = "0000000000000001/12345678/Headers/00000001/foreign_delete.header";
  std::error_code error; const std::array<uint8_t,3> bytes{1, 3, 5};
  auto token = owner->CaptureMetadata(ContentMutationOwner::Kind::kCreate, package, header, error);
  REQUIRE(token); REQUIRE_FALSE(error); REQUIRE(owner->Create(package, token, bytes).complete);
  auto data = volume->content()->Subroot(package, false, error);
  REQUIRE(data); REQUIRE_FALSE(error); REQUIRE_FALSE(data->WriteNewFile("save", bytes).error);
  token = owner->CaptureMetadata(ContentMutationOwner::Kind::kHeader, package, header, error);
  REQUIRE(token); REQUIRE_FALSE(error);
  SECTION("A real member lease prevents the initial native tree proof") {
    auto open = data->OpenFile("save", FileAccess::kFileReadData, error);
    REQUIRE(open); REQUIRE_FALSE(error);
    REQUIRE_FALSE(owner->CapturePackage(package, error));
    REQUIRE(error == std::errc::device_or_resource_busy);
    REQUIRE(std::filesystem::exists(scratch.path / package / "save"));
    REQUIRE(owner->ready());
  }
  SECTION("Cancellation before Prepared changes no namespace or bytes") {
    data.reset(); auto tree = owner->CapturePackage(package, error);
    REQUIRE(tree); REQUIRE_FALSE(error);
    const auto before = TreeBytes(scratch.path); std::atomic<bool> canceled{true};
    const auto refused = owner->Delete(std::move(tree), token, &canceled);
    REQUIRE(refused.error == std::errc::operation_canceled);
    REQUIRE_FALSE(refused.prepared_durable); REQUIRE_FALSE(refused.complete);
    REQUIRE(TreeBytes(scratch.path) == before); REQUIRE(owner->ready());
  }
  SECTION("A retired captured guest generation cannot retarget the healthy internal owner") {
    data.reset();
    auto guest = volume->content()->Subroot({}, false, error);
    REQUIRE(guest); REQUIRE_FALSE(error);
    auto tree = owner->CapturePackage(package, error, guest);
    REQUIRE(tree); REQUIRE_FALSE(error);
    const auto before = TreeBytes(scratch.path);
    guest->Retire();
    REQUIRE_FALSE(volume->identity()->retiring());
    const auto refused = owner->Delete(std::move(tree), token);
    REQUIRE(refused.error == std::errc::no_such_device);
    REQUIRE_FALSE(refused.prepared_durable); REQUIRE_FALSE(refused.complete);
    REQUIRE(TreeBytes(scratch.path) == before); REQUIRE(owner->ready());
  }
  SECTION("Three journal slots are reserved before any operation directory") {
    data.reset(); auto tree = owner->CapturePackage(package, error);
    REQUIRE(tree); REQUIRE_FALSE(error);
    ContentMutationOwnerTestAccess::LimitRecords(*owner, 5);
    const auto before = TreeBytes(scratch.path);
    const auto refused = owner->Delete(std::move(tree), token);
    REQUIRE(refused.error == std::errc::no_space_on_device);
    REQUIRE_FALSE(refused.prepared_durable); REQUIRE_FALSE(refused.complete);
    REQUIRE(TreeBytes(scratch.path) == before); REQUIRE(owner->ready());
  }
  SECTION("An added third-state member after durable Prepared is preserved") {
    data.reset(); auto tree = owner->CapturePackage(package, error);
    REQUIRE(tree); REQUIRE_FALSE(error);
    const auto prepared = ContentMutationOwnerTestAccess::PreparePackageDelete(*owner, std::move(tree), token);
    REQUIRE(prepared.prepared_durable);
    // Noncooperating host writer: never use a permit/path fallback in production.
    { std::ofstream foreign(scratch.path / package / "foreign", std::ios::binary);
      REQUIRE(foreign.good()); foreign << "preserve unknown data"; }
    const auto changed = TreeBytes(scratch.path);
    REQUIRE(owner->Recover() == std::errc::state_not_recoverable);
    REQUIRE_FALSE(owner->ready()); REQUIRE(TreeBytes(scratch.path) == changed);
    REQUIRE(std::filesystem::exists(scratch.path / package / "foreign"));
    REQUIRE(std::filesystem::exists(scratch.path / header));
  }
}
}  // namespace xe::filesystem::test
#endif

#if XE_PLATFORM_LINUX || XE_PLATFORM_MAC
namespace xe::filesystem::test {
TEST_CASE("Content delete restart distinguishes real retired members from changed survivors",
          "[base][storage][content_package]") {
  VolumeScratch scratch; auto volume = scratch.Open(); auto owner = volume->mutations();
  const std::filesystem::path package = "0000000000000001/12345678/00000001/retire_phase";
  const std::filesystem::path header = "0000000000000001/12345678/Headers/00000001/retire_phase.header";
  std::error_code error; const std::array<uint8_t,4> bytes{1, 0, 3, 4};
  auto token = owner->CaptureMetadata(ContentMutationOwner::Kind::kCreate, package, header, error);
  REQUIRE(token); REQUIRE_FALSE(error); REQUIRE(owner->Create(package, token, bytes).complete);
  auto data = volume->content()->Subroot(package, false, error);
  REQUIRE(data); REQUIRE_FALSE(error); REQUIRE_FALSE(data->Create("nested", true));
  REQUIRE_FALSE(data->WriteNewFile("nested/first", bytes).error);
  REQUIRE_FALSE(data->WriteNewFile("nested/survivor", bytes).error); data.reset();
  token = owner->CaptureMetadata(ContentMutationOwner::Kind::kHeader, package, header, error);
  REQUIRE(token); REQUIRE_FALSE(error);
  auto tree = owner->CapturePackage(package, error); REQUIRE(tree); REQUIRE_FALSE(error);
  const auto stopped = ContentMutationOwnerTestAccess::StopPackageDeleteAtRetire(
      *owner, std::move(tree), token);
  REQUIRE(stopped.error == std::errc::operation_in_progress);
  REQUIRE(stopped.prepared_durable); REQUIRE(stopped.quarantined);
  REQUIRE(stopped.header_removed); REQUIRE(stopped.retirement_durable);
  REQUIRE_FALSE(stopped.space_retired); REQUIRE_FALSE(stopped.complete);
  REQUIRE_FALSE(std::filesystem::exists(scratch.path / package));
  REQUIRE_FALSE(std::filesystem::exists(scratch.path / header));
  const auto record = ContentMutationOwnerTestAccess::PackageRecord(*owner, stopped.nonce);
  auto operation = ContentMutationOwnerTestAccess::PackageOperation(*owner, stopped.nonce);
  REQUIRE(std::filesystem::exists(operation->path() / "payload/nested/first"));
  REQUIRE(std::filesystem::exists(operation->path() / "payload/nested/survivor"));
  ContentMutationOwnerTestAccess::RetireActualMember(*owner, record, "nested/first");
  REQUIRE_FALSE(std::filesystem::exists(operation->path() / "payload/nested/first"));
  SECTION("A genuine missing retired member is replayable after reopening actual owners") {
    const auto operation_path = operation->path();
    operation.reset(); token.reset(); owner.reset(); volume.reset();
    volume = scratch.Open(); owner = volume->mutations();
    REQUIRE_FALSE(volume->error()); REQUIRE(owner->ready());
    REQUIRE_FALSE(std::filesystem::exists(operation_path / "payload"));
    REQUIRE_FALSE(std::filesystem::exists(scratch.path / package));
    REQUIRE_FALSE(std::filesystem::exists(scratch.path / header));
    REQUIRE(ContentMutationOwnerTestAccess::PackageRecord(*owner, stopped.nonce) == record);
    REQUIRE_FALSE(owner->Recover());
  }
  SECTION("A changed survivor under genuine Retire is not authorized for deletion") {
    { std::ofstream changed(operation->path() / "payload/nested/survivor", std::ios::binary | std::ios::trunc);
      REQUIRE(changed.good()); changed << "foreign surviving bytes after Retire"; }
    const auto before = TreeBytes(scratch.path);
    REQUIRE(owner->Recover() == std::errc::state_not_recoverable);
    REQUIRE_FALSE(owner->ready()); REQUIRE(TreeBytes(scratch.path) == before);
    REQUIRE(std::filesystem::exists(operation->path() / "payload/nested/survivor"));
  }
  SECTION("An unrecorded new member under genuine Retire is preserved") {
    { std::ofstream extra(operation->path() / "payload/nested/unknown", std::ios::binary);
      REQUIRE(extra.good()); extra << "do not delete unknown member"; }
    const auto before = TreeBytes(scratch.path);
    REQUIRE(owner->Recover() == std::errc::state_not_recoverable);
    REQUIRE_FALSE(owner->ready()); REQUIRE(TreeBytes(scratch.path) == before);
  }
}
TEST_CASE("Content delete missing member before Retire cannot be adopted as retirement",
          "[base][storage][content_package]") {
  VolumeScratch scratch; auto volume = scratch.Open(); auto owner = volume->mutations();
  const std::filesystem::path package = "0000000000000001/12345678/00000001/missing_no_retire";
  const std::filesystem::path header = "0000000000000001/12345678/Headers/00000001/missing_no_retire.header";
  std::error_code error; const std::array<uint8_t,3> bytes{1, 0, 9};
  auto token = owner->CaptureMetadata(ContentMutationOwner::Kind::kCreate, package, header, error);
  REQUIRE(token); REQUIRE_FALSE(error); REQUIRE(owner->Create(package, token, bytes).complete);
  auto data = volume->content()->Subroot(package, false, error);
  REQUIRE(data); REQUIRE_FALSE(error);
  REQUIRE_FALSE(data->WriteNewFile("missing", bytes).error);
  REQUIRE_FALSE(data->WriteNewFile("keep", bytes).error); data.reset();
  token = owner->CaptureMetadata(ContentMutationOwner::Kind::kHeader, package, header, error);
  REQUIRE(token); REQUIRE_FALSE(error); auto tree = owner->CapturePackage(package, error);
  REQUIRE(tree); REQUIRE_FALSE(error);
  const auto prepared = ContentMutationOwnerTestAccess::PreparePackageDelete(*owner, std::move(tree), token);
  REQUIRE(prepared.prepared_durable); REQUIRE_FALSE(prepared.retirement_durable);
  // Noncooperating host deletion models absent data without any durable
  // retirement authority. Recovery must not convert it into a success token.
  REQUIRE(std::filesystem::remove(scratch.path / package / "missing"));
  const auto before = TreeBytes(scratch.path);
  REQUIRE(owner->Recover() == std::errc::state_not_recoverable);
  REQUIRE_FALSE(owner->ready()); REQUIRE(TreeBytes(scratch.path) == before);
  REQUIRE(std::filesystem::exists(scratch.path / package / "keep"));
  REQUIRE(std::filesystem::exists(scratch.path / header));
}
}  // namespace xe::filesystem::test
#endif

#if XE_PLATFORM_LINUX || XE_PLATFORM_MAC
namespace xe::filesystem::test {
TEST_CASE("Content truncate preserves exact directory Header thumbnail and later writes",
          "[base][storage][content_package]") {
  VolumeScratch scratch; auto volume = scratch.Open(); auto owner = volume->mutations();
  const std::filesystem::path package = "0000000000000001/12345678/00000001/truncate";
  const std::filesystem::path header = "0000000000000001/12345678/Headers/00000001/truncate.header";
  const std::filesystem::path thumbnail = "__ThUmBnAiL.PnG";
  std::error_code error; const std::vector<uint8_t> raw_header{1, 0, 7, 255, 9, 0, 42};
  const std::vector<uint8_t> raw_thumbnail{137, 80, 0, 255, 4, 0, 2};
  auto token = owner->CaptureMetadata(ContentMutationOwner::Kind::kCreate, package, header, error);
  REQUIRE(token); REQUIRE_FALSE(error); REQUIRE(owner->Create(package, token, raw_header).complete);
  auto view = volume->content()->Subroot(package, false, error);
  REQUIRE(view); REQUIRE_FALSE(error); REQUIRE_FALSE(view->Create("nested", true));
  REQUIRE_FALSE(view->WriteNewFile("nested/data", raw_header).error);
  REQUIRE_FALSE(view->WriteNewFile(thumbnail, raw_thumbnail).error);
  auto tree = owner->CapturePackage(package, error); REQUIRE(tree); REQUIRE_FALSE(error);
  const auto directory_id = tree->entries().front().file;
  const auto thumb = std::find_if(tree->entries().begin(), tree->entries().end(),
      [&](const auto& row) { return row.relative == thumbnail; });
  REQUIRE(thumb != tree->entries().end()); const auto thumbnail_id = thumb->file;
  token = owner->CaptureMetadata(ContentMutationOwner::Kind::kHeader, package, header, error);
  REQUIRE(token); REQUIRE_FALSE(error); const auto header_id = token->info().file_id;
  const auto result = owner->Truncate(std::move(tree), token);
  REQUIRE_FALSE(result.error); REQUIRE(result.complete); REQUIRE(result.prepared_durable);
  REQUIRE(result.quarantined); REQUIRE(result.retirement_durable); REQUIRE(result.space_retired);
  REQUIRE_FALSE(result.header_removed); REQUIRE(owner->ready());
  auto current = owner->CapturePackage(package, error); REQUIRE(current); REQUIRE_FALSE(error);
  REQUIRE(current->entries().size() == 2); REQUIRE(current->entries().front().file == directory_id);
  REQUIRE(current->entries()[1].relative == thumbnail); REQUIRE(current->entries()[1].file == thumbnail_id);
  auto actual_header = owner->CaptureMetadata(ContentMutationOwner::Kind::kHeader, package, header, error);
  REQUIRE(actual_header); REQUIRE_FALSE(error); REQUIRE(actual_header->info().file_id == header_id);
  REQUIRE(std::vector<uint8_t>(actual_header->bytes().begin(), actual_header->bytes().end()) == raw_header);
  std::vector<uint8_t> actual_thumbnail;
  REQUIRE_FALSE(view->ReadFile(thumbnail, 64, actual_thumbnail)); REQUIRE(actual_thumbnail == raw_thumbnail);
  REQUIRE_FALSE(std::filesystem::exists(scratch.path / package / "nested"));
  current.reset(); REQUIRE_FALSE(view->WriteNewFile("later", raw_header).error);
  REQUIRE_FALSE(owner->Recover()); REQUIRE(owner->ready());
  std::vector<uint8_t> later; REQUIRE_FALSE(view->ReadFile("later", 64, later)); REQUIRE(later == raw_header);
}
TEST_CASE("Content truncate restart preserves metadata through actual partial retirement",
          "[base][storage][content_package]") {
  VolumeScratch scratch; auto volume = scratch.Open(); auto owner = volume->mutations();
  const std::filesystem::path package = "0000000000000001/12345678/00000001/truncate_restart";
  const std::filesystem::path header = "0000000000000001/12345678/Headers/00000001/truncate_restart.header";
  std::error_code error; const std::array<uint8_t,4> bytes{7, 0, 5, 255};
  auto token = owner->CaptureMetadata(ContentMutationOwner::Kind::kCreate, package, header, error);
  REQUIRE(token); REQUIRE_FALSE(error); REQUIRE(owner->Create(package, token, bytes).complete);
  auto view = volume->content()->Subroot(package, false, error);
  REQUIRE(view); REQUIRE_FALSE(error); REQUIRE_FALSE(view->Create("nested", true));
  REQUIRE_FALSE(view->WriteNewFile("nested/first", bytes).error);
  REQUIRE_FALSE(view->WriteNewFile("nested/remaining", bytes).error);
  REQUIRE_FALSE(view->WriteNewFile("__thumbnail.png", bytes).error); view.reset();
  auto tree = owner->CapturePackage(package, error); REQUIRE(tree); REQUIRE_FALSE(error);
  const auto package_id = tree->entries().front().file;
  token = owner->CaptureMetadata(ContentMutationOwner::Kind::kHeader, package, header, error);
  REQUIRE(token); REQUIRE_FALSE(error);
  const auto stopped = ContentMutationOwnerTestAccess::StopPackageTruncateAtRetire(*owner, std::move(tree), token);
  REQUIRE(stopped.error == std::errc::operation_in_progress); REQUIRE(stopped.retirement_durable);
  REQUIRE(stopped.quarantined); REQUIRE_FALSE(stopped.header_removed); REQUIRE_FALSE(stopped.complete);
  const auto record = ContentMutationOwnerTestAccess::PackageRecord(*owner, stopped.nonce);
  REQUIRE(record.kind == content_package_mutation_wire::Kind::kTruncate);
  REQUIRE(record.preserved_leaf == "__thumbnail.png");
  auto operation = ContentMutationOwnerTestAccess::PackageOperation(*owner, stopped.nonce);
  REQUIRE(std::filesystem::exists(operation->path() / "nested/first"));
  REQUIRE(std::filesystem::exists(scratch.path / package / "__thumbnail.png"));
  ContentMutationOwnerTestAccess::RetireActualMember(*owner, record, "nested/first", {});
  SECTION("Real missing retired data replays without replacing the public directory") {
    operation.reset(); token.reset(); owner.reset(); volume.reset();
    volume = scratch.Open(); owner = volume->mutations(); REQUIRE_FALSE(volume->error()); REQUIRE(owner->ready());
    auto current = owner->CapturePackage(package, error); REQUIRE(current); REQUIRE_FALSE(error);
    REQUIRE(current->entries().size() == 2); REQUIRE(current->entries().front().file == package_id);
    REQUIRE_FALSE(std::filesystem::exists(scratch.path / package / "nested"));
    std::ifstream original(scratch.path / header, std::ios::binary);
    REQUIRE(std::vector<uint8_t>(std::istreambuf_iterator<char>(original), {}) == std::vector<uint8_t>(bytes.begin(), bytes.end()));
  }
  SECTION("A changed retained thumbnail refuses before deleting any remaining payload") {
    { std::ofstream changed(scratch.path / package / "__thumbnail.png", std::ios::binary | std::ios::trunc);
      REQUIRE(changed.good()); changed << "changed retained metadata"; }
    const auto before = TreeBytes(scratch.path);
    REQUIRE(owner->Recover() == std::errc::state_not_recoverable);
    REQUIRE_FALSE(owner->ready()); REQUIRE(TreeBytes(scratch.path) == before);
    REQUIRE(std::filesystem::exists(operation->path() / "nested/remaining"));
  }
}
TEST_CASE("Content truncate refuses packed mutation and cancellation without changing bytes",
          "[base][storage][content_package]") {
  VolumeScratch scratch; auto volume = scratch.Open(); auto owner = volume->mutations();
  const std::filesystem::path package = "0000000000000001/12345678/00000001/truncate_refuse";
  const std::filesystem::path header = "0000000000000001/12345678/Headers/00000001/truncate_refuse.header";
  std::error_code error; const std::array<uint8_t,3> bytes{9, 0, 7};
  auto token = owner->CaptureMetadata(ContentMutationOwner::Kind::kCreate, package, header, error);
  REQUIRE(token); REQUIRE_FALSE(error); REQUIRE(owner->Create(package, token, bytes).complete);
  token = owner->CaptureMetadata(ContentMutationOwner::Kind::kHeader, package, header, error);
  REQUIRE(token); REQUIRE_FALSE(error);
  SECTION("Packed regular file has no directory-writer permission") {
    REQUIRE(std::filesystem::remove(scratch.path / package));
    { std::ofstream packed(scratch.path / package, std::ios::binary); REQUIRE(packed.good()); packed << "retained raw container"; }
    auto tree = owner->CapturePackage(package, error); REQUIRE(tree); REQUIRE_FALSE(error);
    const auto before = TreeBytes(scratch.path);
    const auto result = owner->Truncate(std::move(tree), token);
    REQUIRE(result.error == std::errc::operation_not_supported);
    REQUIRE_FALSE(result.prepared_durable); REQUIRE_FALSE(result.complete);
    REQUIRE(TreeBytes(scratch.path) == before); REQUIRE(owner->ready());
  }
  SECTION("Cancellation before durable intent changes no directory or metadata") {
    auto tree = owner->CapturePackage(package, error); REQUIRE(tree); REQUIRE_FALSE(error);
    const auto before = TreeBytes(scratch.path); std::atomic<bool> canceled{true};
    const auto result = owner->Truncate(std::move(tree), token, &canceled);
    REQUIRE(result.error == std::errc::operation_canceled);
    REQUIRE_FALSE(result.prepared_durable); REQUIRE(TreeBytes(scratch.path) == before); REQUIRE(owner->ready());
  }
}
}  // namespace xe::filesystem::test
#endif

#if XE_PLATFORM_LINUX || XE_PLATFORM_MAC
namespace xe::filesystem::test {
TEST_CASE("Content truncate recovers only a complete joint namespace before Retire",
          "[base][storage][content_package]") {
  VolumeScratch scratch; auto volume = scratch.Open(); auto owner = volume->mutations();
  const std::filesystem::path package = "0000000000000001/12345678/00000001/truncate_joint";
  const std::filesystem::path header = "0000000000000001/12345678/Headers/00000001/truncate_joint.header";
  std::error_code error; const std::array<uint8_t,4> bytes{5, 0, 3, 255};
  auto token = owner->CaptureMetadata(ContentMutationOwner::Kind::kCreate, package, header, error);
  REQUIRE(token); REQUIRE_FALSE(error); REQUIRE(owner->Create(package, token, bytes).complete);
  auto view = volume->content()->Subroot(package, false, error);
  REQUIRE(view); REQUIRE_FALSE(error); REQUIRE_FALSE(view->Create("nested", true));
  REQUIRE_FALSE(view->WriteNewFile("nested/save", bytes).error);
  REQUIRE_FALSE(view->WriteNewFile("loose", bytes).error);
  REQUIRE_FALSE(view->WriteNewFile("__thumbnail.png", bytes).error); view.reset();
  auto tree = owner->CapturePackage(package, error); REQUIRE(tree); REQUIRE_FALSE(error);
  const auto original_id = tree->entries().front().file;
  token = owner->CaptureMetadata(ContentMutationOwner::Kind::kHeader, package, header, error);
  REQUIRE(token); REQUIRE_FALSE(error);
  const auto stopped = ContentMutationOwnerTestAccess::PreparePackageTruncate(*owner, std::move(tree), token);
  REQUIRE(stopped.prepared_durable); REQUIRE_FALSE(stopped.quarantined); REQUIRE_FALSE(stopped.retirement_durable);
  const auto record = ContentMutationOwnerTestAccess::PackageRecord(*owner, stopped.nonce);
  ContentMutationOwnerTestAccess::NoRetireOrDone(*owner, stopped.nonce);
  SECTION("One real interrupted namespace move can complete from the journal") {
    ContentMutationOwnerTestAccess::MoveActualTruncateMember(*owner, record, "nested");
    REQUIRE_FALSE(std::filesystem::exists(scratch.path / package / "nested"));
    REQUIRE(std::filesystem::exists(scratch.path / package / "loose"));
    // Drop every old reservation/epoch owner before factory recovery. The
    // split public/private tree must authenticate from persisted IDs alone.
    token.reset(); owner.reset(); volume.reset();
    volume = scratch.Open(); owner = volume->mutations();
    REQUIRE_FALSE(volume->error()); REQUIRE(owner->ready());
    auto current = owner->CapturePackage(package, error); REQUIRE(current); REQUIRE_FALSE(error);
    REQUIRE(current->entries().size() == 2); REQUIRE(current->entries().front().file == original_id);
    REQUIRE(std::filesystem::exists(scratch.path / package / "__thumbnail.png"));
    REQUIRE_FALSE(std::filesystem::exists(scratch.path / package / "loose"));
    std::ifstream actual(scratch.path / header, std::ios::binary);
    REQUIRE(std::vector<uint8_t>(std::istreambuf_iterator<char>(actual), {}) == std::vector<uint8_t>(bytes.begin(), bytes.end()));
  }
  SECTION("A missing member with no Retire and no quarantine counterpart is refused") {
    REQUIRE(std::filesystem::remove(scratch.path / package / "loose"));
    const auto before = TreeBytes(scratch.path);
    REQUIRE(owner->Recover() == std::errc::state_not_recoverable);
    REQUIRE_FALSE(owner->ready()); REQUIRE(TreeBytes(scratch.path) == before);
    REQUIRE(std::filesystem::exists(scratch.path / package / "nested/save"));
    REQUIRE(std::filesystem::exists(scratch.path / header));
  }
}
}  // namespace xe::filesystem::test
#endif

#if XE_PLATFORM_LINUX || XE_PLATFORM_MAC
namespace xe::filesystem::test {
namespace {
const std::filesystem::path kInitialPackage="0000000000000001/12345678/00000001/initial";
const std::filesystem::path kInitialHeader="0000000000000001/12345678/Headers/00000001/initial.header";
std::vector<ContentPackageInitialMember> InitialMembers() {
  return {{"Account",{1,2,0,4,255},false},{"nested",{},true},
          {"nested/settings.gpd",{0x58,0x44,0x42,0x46,0,3},false},
          {"empty.bin",{},false}};
}
void CheckInitialBytes(const HostStorageRoot& content,
    std::span<const ContentPackageInitialMember> members) {
  std::error_code error;
  auto package=content.Subroot(kInitialPackage,false,error);
  REQUIRE(package); REQUIRE_FALSE(error);
  for (const auto& member:members) {
    if (member.directory) { REQUIRE(std::filesystem::is_directory(package->path()/member.relative)); }
    else {
      std::vector<uint8_t> actual;
      REQUIRE_FALSE(package->ReadFile(member.relative,128,actual));
      REQUIRE(actual==member.bytes);
    }
  }
}
}
TEST_CASE("Initial content creation publishes the complete native tree and Header together",
          "[base][storage][content_mutation][initial_members]") {
  VolumeScratch scratch; auto volume=scratch.Open(); auto owner=volume->mutations();
  std::error_code error;
  auto header=owner->CaptureMetadata(ContentMutationOwner::Kind::kCreate,
      kInitialPackage,kInitialHeader,error);
  REQUIRE(header); REQUIRE_FALSE(error); REQUIRE_FALSE(header->exists());
  const auto members=InitialMembers(); const std::vector<uint8_t> metadata{7,0,8,9};
  const auto made=owner->CreateInitial(kInitialPackage,header,metadata,members);
  REQUIRE_FALSE(made.error); REQUIRE(made.complete); REQUIRE(made.prepared_durable);
  REQUIRE(made.directory_published); REQUIRE(made.metadata_published); REQUIRE(made.durable);
  REQUIRE(owner->Visible(kInitialPackage)); CheckInitialBytes(*volume->content(),members);
  const auto record=ContentMutationOwnerTestAccess::ReadRecord(*owner,made.nonce);
  REQUIRE(record.initial_tree.size()==members.size()+1);
  REQUIRE(record.initial_tree.front().entry.file==record.directory_stage);
  auto actual=owner->CaptureMetadata(ContentMutationOwner::Kind::kHeader,
      kInitialPackage,kInitialHeader,error);
  REQUIRE(actual); REQUIRE_FALSE(error);
  REQUIRE(std::vector<uint8_t>(actual->bytes().begin(),actual->bytes().end())==metadata);
  // Data rows came from actual native descriptors, not caller-chosen IDs.
  for (const auto& row:record.initial_tree) { REQUIRE(row.entry.file); }
  header.reset(); actual.reset(); owner.reset(); volume.reset();
  volume=scratch.Open(); REQUIRE(volume->mutations()->Visible(kInitialPackage));
  CheckInitialBytes(*volume->content(),members);
}
TEST_CASE("Native private staging retains dependency without inheriting the guest source namespace",
          "[base][storage][content_mutation][initial_members][native_staging]") {
  const bool retire_source=GENERATE(false,true);
  VolumeScratch scratch;auto volume=scratch.Open();auto owner=volume->mutations();
  std::error_code error;
  auto header=owner->CaptureMetadata(ContentMutationOwner::Kind::kCreate,
      kInitialPackage,kInitialHeader,error);
  REQUIRE(header);REQUIRE_FALSE(error);
  auto source_guest=volume->content()->GuestView(error);
  REQUIRE(source_guest);REQUIRE_FALSE(error);
  const auto members=InitialMembers();const std::array<uint8_t,3> metadata{3,0,7};
  auto stopped=ContentMutationOwnerTestAccess::StopInitial(
      *owner,kInitialPackage,header,metadata,members,1);
  REQUIRE(stopped.error==std::errc::operation_canceled);
  REQUIRE(stopped.prepared_durable);REQUIRE_FALSE(stopped.complete);
  REQUIRE_FALSE(owner->ready());
  auto staged=ContentMutationOwnerTestAccess::PackageOperation(*owner,stopped.nonce);
  std::vector<uint8_t> bytes;
  REQUIRE_FALSE(staged->ReadFile("Account",16,bytes));
  REQUIRE(bytes==members.front().bytes);
  const auto private_member=(staged->path()/"Account").lexically_relative(volume->content()->path());
  REQUIRE_FALSE(source_guest->OpenFile(private_member,FileAccess::kGenericRead,error));
  REQUIRE(error==std::errc::permission_denied);
  source_guest.reset();
  // Native capability separation must never become a guest privacy bypass.
  REQUIRE_FALSE(staged->GuestView(error));
  REQUIRE(error==std::errc::permission_denied);
  REQUIRE_FALSE(HostStorageRoot::AdmitGuestMount(
      std::vector<GuestHostBacking>{staged->GuestBacking()},error));
  REQUIRE(error==std::errc::permission_denied);
  const auto account=staged->path()/"Account";
  if (retire_source) {
    // The independent namespace still depends on the original exact child and
    // its source generation. It cannot read through actual source removal.
    volume->content()->Retire();
    REQUIRE(staged->ReadFile("Account",16,bytes)==std::errc::no_such_device);
    std::ifstream actual(account,std::ios::binary);
    const std::vector<uint8_t> preserved{std::istreambuf_iterator<char>(actual),
                                        std::istreambuf_iterator<char>()};
    REQUIRE(preserved==members.front().bytes);
    REQUIRE_FALSE(std::filesystem::exists(scratch.path/kInitialPackage));
    REQUIRE_FALSE(std::filesystem::exists(scratch.path/kInitialHeader));
  } else {
    // Cancellation after genuine durable Prepared retains its obligation.
    // Release actual receipt/native owners, then ordinary recovery completes.
    staged.reset();header.reset();stopped.metadata={};owner.reset();volume.reset();
    volume=scratch.Open();owner=volume->mutations();
    REQUIRE(owner->ready());REQUIRE(owner->Visible(kInitialPackage));
    CheckInitialBytes(*volume->content(),members);
    auto current=owner->CaptureMetadata(ContentMutationOwner::Kind::kHeader,
        kInitialPackage,kInitialHeader,error);
    REQUIRE(current);REQUIRE_FALSE(error);
    REQUIRE(std::vector<uint8_t>(current->bytes().begin(),current->bytes().end())==
        std::vector<uint8_t>(metadata.begin(),metadata.end()));
  }
}
TEST_CASE("Initial content restart finishes genuine Prepared Header and rename boundaries",
          "[base][storage][content_mutation][initial_members][restart]") {
  unsigned boundary=1;bool retain_receipt=false;
  SECTION("Durable complete-tree Prepared before public Header") { boundary=1; }
  SECTION("Actual Header CAS before tree rename") { boundary=2; }
  SECTION("Actual no-replace tree rename before Done") { boundary=3; }
  SECTION("A real retained publication receipt remains busy until released") {
    boundary=2;retain_receipt=true;
  }
  VolumeScratch scratch; auto volume=scratch.Open(); auto owner=volume->mutations();
  std::error_code error; const auto members=InitialMembers();
  auto header=owner->CaptureMetadata(ContentMutationOwner::Kind::kCreate,kInitialPackage,kInitialHeader,error);
  REQUIRE(header); REQUIRE_FALSE(error);
  const std::vector<uint8_t> metadata{9,0,8};
  auto stopped=ContentMutationOwnerTestAccess::StopInitial(*owner,
      kInitialPackage,header,metadata,members,boundary);
  REQUIRE(stopped.prepared_durable); REQUIRE_FALSE(stopped.complete);
  REQUIRE(stopped.error); REQUIRE_FALSE(owner->Visible(kInitialPackage));
  const auto record=ContentMutationOwnerTestAccess::ReadRecord(*owner,stopped.nonce);
  REQUIRE(record.initial_tree.size()==members.size()+1);
  REQUIRE(std::filesystem::exists(scratch.path/kInitialPackage)==(boundary==3));
  REQUIRE(std::filesystem::exists(scratch.path/kInitialHeader)==(boundary>=2));
  header.reset(); owner.reset(); volume.reset();
  if(retain_receipt) {
    // The original process has not lost ALL owners while its publication
    // snapshots are retained. The exact native Header authority must stay busy.
    auto refused=ManagedVolumeAuthority::Open(ExistingFormatOptions(scratch));
    REQUIRE(refused);REQUIRE(refused->error()==std::errc::device_or_resource_busy);
    REQUIRE_FALSE(refused->profile_writable());refused.reset();
  }
  // Actual process loss releases receipt-owned expected/current snapshots too.
  // Reopening while those live tokens remain is not a crash/restart fixture.
  stopped.metadata=SnapshotPublicationReceipt();
  volume=scratch.Open(); owner=volume->mutations(); REQUIRE(owner->Visible(kInitialPackage));
  CheckInitialBytes(*volume->content(),members);
  auto current=owner->CaptureMetadata(ContentMutationOwner::Kind::kHeader,kInitialPackage,kInitialHeader,error);
  REQUIRE(current); REQUIRE_FALSE(error); REQUIRE(current->info().file_id==record.stage_file);
  REQUIRE(std::vector<uint8_t>(current->bytes().begin(),current->bytes().end())==metadata);
}
TEST_CASE("Initial content preparation rejects changed or unknown staged payload before publication",
          "[base][storage][content_mutation][initial_members][conflict]") {
  VolumeScratch scratch; auto volume=scratch.Open(); auto owner=volume->mutations();
  std::error_code error; const auto members=InitialMembers();
  auto header=owner->CaptureMetadata(ContentMutationOwner::Kind::kCreate,kInitialPackage,kInitialHeader,error);
  REQUIRE(header); REQUIRE_FALSE(error);
  const std::vector<uint8_t> metadata{9,8};
  const auto stopped=ContentMutationOwnerTestAccess::StopInitial(*owner,kInitialPackage,
      header,metadata,members,1);
  REQUIRE(stopped.prepared_durable);
  auto staged=ContentMutationOwnerTestAccess::PackageOperation(*owner,stopped.nonce);
  SECTION("Changed recorded bytes with identical length") {
    auto actual=FileHandle::OpenExisting(staged->path()/"Account",FileAccess::kGenericWrite);
    REQUIRE(actual); const std::array<uint8_t,5> foreign{5,4,3,2,1};
    REQUIRE(actual->WriteResult(0,foreign).bytes==foreign.size());
    REQUIRE_FALSE(actual->FlushResult()); actual.reset();
    const auto record=ContentMutationOwnerTestAccess::ReadRecord(*owner,stopped.nonce);
    const auto row=std::find_if(record.initial_tree.begin(),record.initial_tree.end(),
        [](const auto& member){return member.entry.relative=="Account";});
    REQUIRE(row!=record.initial_tree.end());
    // Restore the recorded mtime, so SAME inode/size/mode/mtime cannot hide
    // changed bytes. The production full-content digest must reject them.
    const timespec times[2]{{0,UTIME_OMIT},
        {time_t(row->entry.write_seconds),long(row->entry.write_nanoseconds)}};
    REQUIRE(utimensat(AT_FDCWD,(staged->path()/"Account").c_str(),times,0)==0);
  }
  SECTION("An unrecorded native member") {
    const auto written=staged->WriteNewFile("unknown.bin",metadata);
    REQUIRE_FALSE(written.error); REQUIRE(written.bytes==metadata.size());
  }
  REQUIRE(owner->Recover()); REQUIRE_FALSE(owner->ready());
  REQUIRE_FALSE(std::filesystem::exists(scratch.path/kInitialPackage));
  REQUIRE_FALSE(std::filesystem::exists(scratch.path/kInitialHeader));
  REQUIRE(std::filesystem::exists(staged->path()/"Account"));
}
TEST_CASE("Initial content canceled input and invalid members leave originals unchanged",
          "[base][storage][content_mutation][initial_members][cancel]") {
  VolumeScratch scratch; auto volume=scratch.Open(); auto owner=volume->mutations();
  std::error_code error;
  auto header=owner->CaptureMetadata(ContentMutationOwner::Kind::kCreate,kInitialPackage,kInitialHeader,error);
  REQUIRE(header); REQUIRE_FALSE(error); const auto before=TreeBytes(scratch.path);
  const std::vector<uint8_t> metadata{1,2}; auto members=InitialMembers();
  SECTION("Canceled before admission") {
    std::atomic<bool> canceled{true};
    REQUIRE(owner->CreateInitial(kInitialPackage,header,metadata,members,&canceled).error==std::errc::operation_canceled);
  }
  SECTION("Traversal") { members[0].relative="../escape"; REQUIRE(owner->CreateInitial(kInitialPackage,header,metadata,members).error); }
  SECTION("Case collision") { members.push_back({"ACCOUNT",{2},false}); REQUIRE(owner->CreateInitial(kInitialPackage,header,metadata,members).error); }
  SECTION("Missing explicit parent") { members[0].relative="absent/file"; REQUIRE(owner->CreateInitial(kInitialPackage,header,metadata,members).error); }
  REQUIRE(TreeBytes(scratch.path)==before); REQUIRE(owner->ready());
}
TEST_CASE("Initial content creation respects a real foreign absent Header reservation",
          "[base][storage][content_mutation][initial_members][lease]") {
  VolumeScratch scratch; auto volume=scratch.Open(); auto owner=volume->mutations();
  std::error_code error;
  auto header=owner->CaptureMetadata(ContentMutationOwner::Kind::kCreate,kInitialPackage,kInitialHeader,error);
  REQUIRE(header); REQUIRE_FALSE(error);
  auto parent=volume->content()->Subroot(kInitialHeader.parent_path(),false,error);
  REQUIRE(parent); REQUIRE_FALSE(error);
  auto absent=parent->CaptureSnapshot(kInitialHeader.filename(),128,error);
  REQUIRE(absent); REQUIRE_FALSE(error); REQUIRE_FALSE(absent->exists());
  auto foreign=parent->ReserveSnapshotFile(absent,error);
  REQUIRE(foreign); REQUIRE_FALSE(error);
  const auto members=InitialMembers(); const std::vector<uint8_t> metadata{1,2,3};
  const auto before=TreeBytes(scratch.path);
  REQUIRE_FALSE(owner->CreateInitial(kInitialPackage,header,metadata,members).complete);
  REQUIRE(TreeBytes(scratch.path)==before);
  foreign.reset();
  REQUIRE(owner->CreateInitial(kInitialPackage,header,metadata,members).complete);
  CheckInitialBytes(*volume->content(),members);
}
TEST_CASE("Interrupted unprepared initial member is preserved without blocking the HDD",
          "[base][storage][content_mutation][initial_members][partial]") {
  VolumeScratch scratch; auto volume=scratch.Open(); auto owner=volume->mutations();
  std::error_code error;
  auto header=owner->CaptureMetadata(ContentMutationOwner::Kind::kCreate,kInitialPackage,kInitialHeader,error);
  REQUIRE(header); REQUIRE_FALSE(error);
  const auto members=InitialMembers(); const std::vector<uint8_t> metadata{1,2};
  const auto stopped=ContentMutationOwnerTestAccess::StopInitial(*owner,kInitialPackage,
      header,metadata,members,4);
  REQUIRE(stopped.error); REQUIRE_FALSE(stopped.complete); REQUIRE_FALSE(stopped.prepared_durable);
  REQUIRE_FALSE(std::filesystem::exists(scratch.path/kInitialPackage));
  REQUIRE_FALSE(std::filesystem::exists(scratch.path/kInitialHeader));
  auto staged=ContentMutationOwnerTestAccess::PackageOperation(*owner,stopped.nonce);
  std::vector<uint8_t> actual; REQUIRE_FALSE(staged->ReadFile("Account",128,actual));
  REQUIRE(actual==members.front().bytes);
  REQUIRE_FALSE(std::filesystem::exists(staged->path()/"nested/settings.gpd"));
  const auto old_path = staged->path(); staged.reset();
  REQUIRE_FALSE(owner->Recover()); REQUIRE(owner->ready());
  REQUIRE_FALSE(std::filesystem::exists(old_path));
  const auto preserved = ContentMutationOwnerTestAccess::StageDirectory(*owner)/("preserved-"+stopped.nonce+"-d");
  std::ifstream retained(preserved/"Account",std::ios::binary); REQUIRE(retained.good());
  REQUIRE(std::vector<uint8_t>(std::istreambuf_iterator<char>(retained),{}) == members.front().bytes);
  ContentMutationOwnerTestAccess::RequireAborted(*owner,stopped.nonce);
}
}  // namespace xe::filesystem::test
#endif

#if XE_PLATFORM_LINUX || XE_PLATFORM_MAC
namespace xe::filesystem::test {
TEST_CASE("Initial content common space refusal precedes staged bytes and journal",
          "[base][storage][content_mutation][initial_members][space]") {
  VolumeScratch scratch; std::error_code error;
  // A custom ForPath query is intentionally non-native and cannot initialize
  // managed ownership. Keep the genuine common native filesystem authority.
  REQUIRE(scratch.root->native_filesystem_authority());
  auto volume=scratch.Open(); auto owner=volume->mutations();
  auto header=owner->CaptureMetadata(ContentMutationOwner::Kind::kCreate,kInitialPackage,kInitialHeader,error);
  REQUIRE(header); REQUIRE_FALSE(error); const auto before=TreeBytes(scratch.path);
  const auto members=InitialMembers(); const std::vector<uint8_t> metadata{1,2};
  const auto refused=ContentMutationOwnerTestAccess::DenyInitialSpace(
      *owner,kInitialPackage,header,metadata,members);
  REQUIRE(refused.error==std::errc::no_space_on_device);
  REQUIRE_FALSE(refused.prepared_durable); REQUIRE_FALSE(refused.complete);
  REQUIRE(TreeBytes(scratch.path)==before); REQUIRE(owner->ready());
  REQUIRE(owner->CreateInitial(kInitialPackage,header,metadata,members).complete);
}
}  // namespace xe::filesystem::test
#endif

#if XE_PLATFORM_LINUX || XE_PLATFORM_MAC
namespace xe::filesystem::test {
TEST_CASE("Initial content journal authenticates full tree manifest and refuses invalid identities",
          "[base][storage][content_mutation][initial_members][wire]") {
  VolumeScratch scratch; auto volume=scratch.Open(); auto owner=volume->mutations();
  std::error_code error;
  auto header=owner->CaptureMetadata(ContentMutationOwner::Kind::kCreate,kInitialPackage,kInitialHeader,error);
  REQUIRE(header); REQUIRE_FALSE(error); const auto members=InitialMembers();
  const std::vector<uint8_t> metadata{1,2};
  const auto stopped=ContentMutationOwnerTestAccess::StopInitial(*owner,kInitialPackage,
      header,metadata,members,1);
  REQUIRE(stopped.prepared_durable);
  const auto actual=ContentMutationOwnerTestAccess::ReadRecord(*owner,stopped.nonce);
  auto encoded=content_mutation_wire::Encode(actual,error);
  REQUIRE_FALSE(error); REQUIRE(encoded.size()>200); REQUIRE(encoded[11]==5);
  ContentMutationOwnerTestAccess::RequirePreparationBinding(*owner, actual);
  content_mutation_wire::Record decoded;
  REQUIRE(content_mutation_wire::Decode(encoded,decoded,error)); REQUIRE_FALSE(error);
  REQUIRE(decoded.initial_tree==actual.initial_tree); REQUIRE(decoded.intended==metadata);
  REQUIRE(decoded.preparation_digest==actual.preparation_digest);
  // Retain explicit reader coverage for historical V3 records, which predate
  // the preparation-only plan and therefore have no preparation digest.
  auto legacy=actual; legacy.preparation_digest={};
  const auto legacy_encoded=content_mutation_wire::Encode(legacy,error);
  REQUIRE_FALSE(error); REQUIRE(legacy_encoded[11]==3);
  content_mutation_wire::Record legacy_decoded;
  REQUIRE(content_mutation_wire::Decode(legacy_encoded,legacy_decoded,error));
  REQUIRE_FALSE(error); REQUIRE(legacy_decoded.initial_tree==actual.initial_tree);
  REQUIRE(legacy_decoded.intended==metadata); REQUIRE_FALSE(legacy_decoded.streamed);
  REQUIRE(legacy_decoded.preparation_digest==legacy.preparation_digest);
  auto invalid=actual; invalid.initial_tree.front().entry.file=invalid.stage_file;
  REQUIRE(content_mutation_wire::Encode(invalid,error).empty()); REQUIRE(error);
  encoded.back()^=1; decoded.nonce="unchanged";
  REQUIRE_FALSE(content_mutation_wire::Decode(encoded,decoded,error)); REQUIRE(error);
  REQUIRE(decoded.nonce=="unchanged");
  REQUIRE_FALSE(std::filesystem::exists(scratch.path/kInitialPackage));
}
}  // namespace xe::filesystem::test
#endif

#if XE_PLATFORM_LINUX || XE_PLATFORM_MAC
namespace xe::filesystem::test {
namespace {
// Every successful byte comes from one real originally opened host file.
// Fault controls only cancel/refuse; they cannot manufacture successful reads.
class NativeStreamInput final : public ContentPackageStreamSource {
 public:
  NativeStreamInput(const std::filesystem::path& path, uint64_t size) {
    { std::ofstream create(path, std::ios::binary); REQUIRE(create.good()); }
    auto writer = FileHandle::OpenExisting(path, FileAccess::kGenericRead | FileAccess::kGenericWrite);
    REQUIRE(writer); REQUIRE_FALSE(writer->ResizeResult(size));
    const std::array<uint8_t, 7> first{0, 1, 0x80, 0xFF, 4, 5, 6};
    const std::array<uint8_t, 5> last{9, 0, 7, 0xFE, 5};
    REQUIRE(writer->WriteAllResult(0, first).bytes == first.size());
    REQUIRE(writer->WriteAllResult(size_t(size - last.size()), last).bytes == last.size());
    REQUIRE_FALSE(writer->FlushResult()); writer.reset();
    file = FileHandle::OpenExisting(path, FileAccess::kFileReadData);
    REQUIRE(file); std::error_code error; const auto info = file->QueryInfo(error);
    REQUIRE(info); REQUIRE_FALSE(error); captured = *info;
    rows.push_back({"payload.bin", size, false});
  }
  std::span<const ContentPackageStreamMember> members() const override { return rows; }
  FileIoResult Read(size_t member, uint64_t offset, std::span<uint8_t> output) override {
    if (member || offset > captured.size || output.size() > captured.size - offset || offset > SIZE_MAX) {
      return {std::make_error_code(std::errc::invalid_argument), 0};
    }
    largest_read = std::max(largest_read, output.size()); ++reads;
    const auto result = file->ReadResult(size_t(offset), output);
    if (cancel && reads == cancel_on_read) { cancel->store(true, std::memory_order_release); }
    return result;
  }
  std::error_code CheckCurrent() const override {
    std::error_code error; const auto info = file->QueryInfo(error);
    if (error) { return error; }
    return info && info->regular && info->file_id == captured.file_id &&
        info->filesystem_id == captured.filesystem_id && info->size == captured.size
        ? std::error_code{} : std::make_error_code(std::errc::state_not_recoverable);
  }
  std::unique_ptr<FileHandle> file;
  FileHandleInfo captured;
  std::vector<ContentPackageStreamMember> rows;
  size_t reads = 0, largest_read = 0, cancel_on_read = 2;
  std::atomic<bool>* cancel = nullptr;
};
void CheckStreamPayload(const HostStorageRoot& root, NativeStreamInput& source) {
  std::error_code error;
  auto destination = root.OpenFile(kInitialPackage / "payload.bin", FileAccess::kFileReadData, error);
  REQUIRE(destination); REQUIRE_FALSE(error);
  const auto info = destination->QueryInfo(error); REQUIRE(info); REQUIRE_FALSE(error);
  REQUIRE(info->size == source.captured.size);
  std::vector<uint8_t> expected(1u << 20), actual(1u << 20);
  for (uint64_t offset = 0; offset != info->size;) {
    const auto count = size_t(std::min<uint64_t>(actual.size(), info->size - offset));
    const auto read = destination->ReadResult(size_t(offset), std::span(actual).first(count));
    const auto input = source.file->ReadResult(size_t(offset), std::span(expected).first(count));
    REQUIRE_FALSE(read.error); REQUIRE(read.bytes == count);
    REQUIRE_FALSE(input.error); REQUIRE(input.bytes == count);
    REQUIRE(std::equal(actual.begin(), actual.begin() + count, expected.begin()));
    offset += count;
  }
}
}
TEST_CASE("Streamed content exceeds 128 MiB using retained host input and bounded reads",
          "[base][storage][content_mutation][stream]") {
  VolumeScratch scratch, inputs; auto volume = scratch.Open(); auto owner = volume->mutations();
  NativeStreamInput source(inputs.path / "selected.bin", (129ull << 20) + 17);
  // Replacing the selected pathname cannot change the already retained input.
  std::filesystem::rename(inputs.path / "selected.bin", inputs.path / "retained.bin");
  { std::ofstream(inputs.path / "selected.bin", std::ios::binary) << "replacement"; }
  std::error_code error;
  auto header = owner->CaptureMetadata(ContentMutationOwner::Kind::kCreate, kInitialPackage, kInitialHeader, error);
  REQUIRE(header); REQUIRE_FALSE(error);
  const std::array<uint8_t, 4> metadata{0x58, 0, 0x80, 0xFF};
  std::atomic<uint64_t> progress{UINT64_MAX};
  auto made = owner->CreateStream(kInitialPackage, header, metadata, source, nullptr, &progress);
  REQUIRE_FALSE(made.error); REQUIRE(made.complete); REQUIRE(made.stream_admitted_durable);
  REQUIRE(made.prepared_durable); REQUIRE(owner->ready()); REQUIRE(owner->Visible(kInitialPackage));
  REQUIRE(progress.load() == source.captured.size); REQUIRE(source.reads > 128);
  REQUIRE(source.largest_read <= (1u << 20)); CheckStreamPayload(*volume->content(), source);
  const auto record = ContentMutationOwnerTestAccess::ReadRecord(*owner, made.nonce);
  REQUIRE(record.streamed); REQUIRE_FALSE(record.stream_pending);
  REQUIRE(record.initial_tree.back().entry.bytes == source.captured.size);
  auto encoded = content_mutation_wire::Encode(record, error);
  REQUIRE_FALSE(error); REQUIRE(encoded[11] == 5);
  ContentMutationOwnerTestAccess::RequirePreparationBinding(*owner, record);
  content_mutation_wire::Record decoded;
  REQUIRE(content_mutation_wire::Decode(encoded, decoded, error));
  REQUIRE_FALSE(error); REQUIRE(decoded.streamed); REQUIRE_FALSE(decoded.stream_pending);
  REQUIRE(decoded.initial_tree == record.initial_tree);
  REQUIRE(decoded.preparation_digest == record.preparation_digest);
  // V4 remains readable independently of the new preparation-bound writer.
  auto legacy = record; legacy.preparation_digest = {};
  const auto legacy_encoded = content_mutation_wire::Encode(legacy, error);
  REQUIRE_FALSE(error); REQUIRE(legacy_encoded[11] == 4);
  content_mutation_wire::Record legacy_decoded;
  REQUIRE(content_mutation_wire::Decode(legacy_encoded, legacy_decoded, error));
  REQUIRE_FALSE(error); REQUIRE(legacy_decoded.streamed); REQUIRE_FALSE(legacy_decoded.stream_pending);
  REQUIRE(legacy_decoded.initial_tree == record.initial_tree);
  REQUIRE(legacy_decoded.intended == record.intended);
  REQUIRE(legacy_decoded.preparation_digest == legacy.preparation_digest);
  header.reset(); made.metadata = SnapshotPublicationReceipt(); owner.reset(); volume.reset();
  volume = scratch.Open(); REQUIRE(volume->mutations()->Visible(kInitialPackage));
  CheckStreamPayload(*volume->content(), source);
}
TEST_CASE("Stream cancellation rolls back only authenticated private staging without publication",
          "[base][storage][content_mutation][stream][cancel]") {
  VolumeScratch scratch, inputs; auto volume = scratch.Open(); auto owner = volume->mutations();
  NativeStreamInput source(inputs.path / "selected.bin", 4ull << 20);
  std::error_code error;
  auto header = owner->CaptureMetadata(ContentMutationOwner::Kind::kCreate, kInitialPackage, kInitialHeader, error);
  REQUIRE(header); REQUIRE_FALSE(error);
  std::atomic<bool> canceled{false}; std::atomic<uint64_t> progress{0}; source.cancel = &canceled;
  const std::array<uint8_t, 2> metadata{1, 2};
  const auto stopped = owner->CreateStream(kInitialPackage, header, metadata, source, &canceled, &progress);
  REQUIRE(stopped.error == std::errc::operation_canceled); REQUIRE_FALSE(stopped.complete);
  REQUIRE(stopped.stream_admitted_durable); REQUIRE_FALSE(stopped.prepared_durable);
  REQUIRE(progress.load() == (1u << 20)); REQUIRE(owner->ready());
  REQUIRE_FALSE(std::filesystem::exists(scratch.path / kInitialPackage));
  REQUIRE_FALSE(std::filesystem::exists(scratch.path / kInitialHeader));
  ContentMutationOwnerTestAccess::RequireStreamRollbackDone(*owner, stopped.nonce);
  REQUIRE_FALSE(owner->Recover()); REQUIRE_FALSE(source.CheckCurrent());
  source.cancel = nullptr;
  REQUIRE(owner->CreateStream(kInitialPackage, header, metadata, source).complete);
  CheckStreamPayload(*volume->content(), source);
}
TEST_CASE("Interrupted streamed writes recover their exact private stage without input replay",
          "[base][storage][content_mutation][stream][restart]") {
  VolumeScratch scratch, inputs; auto volume = scratch.Open(); auto owner = volume->mutations();
  NativeStreamInput source(inputs.path / "selected.bin", 4ull << 20);
  std::error_code error;
  auto header = owner->CaptureMetadata(ContentMutationOwner::Kind::kCreate, kInitialPackage, kInitialHeader, error);
  REQUIRE(header); REQUIRE_FALSE(error); std::atomic<uint64_t> progress{0};
  const std::array<uint8_t, 2> metadata{3, 4};
  auto stopped = ContentMutationOwnerTestAccess::StopStream(*owner, kInitialPackage, header, metadata, source, progress, 2ull << 20);
  REQUIRE(stopped.error); REQUIRE(stopped.stream_admitted_durable); REQUIRE_FALSE(stopped.prepared_durable);
  REQUIRE_FALSE(stopped.complete); REQUIRE_FALSE(owner->ready()); REQUIRE(progress.load() == (2ull << 20));
  const auto admission = ContentMutationOwnerTestAccess::StreamRecord(*owner, stopped.nonce);
  REQUIRE(admission.stream_pending);
  {
    auto stage = ContentMutationOwnerTestAccess::PackageOperation(*owner, stopped.nonce);
    REQUIRE(std::filesystem::file_size(stage->path() / "payload.bin") == (2ull << 20));
  }
  const auto reads = source.reads;
  header.reset(); stopped.metadata = SnapshotPublicationReceipt(); owner.reset(); volume.reset();
  volume = scratch.Open(); owner = volume->mutations();
  REQUIRE(owner->ready()); REQUIRE(source.reads == reads);
  REQUIRE_FALSE(std::filesystem::exists(scratch.path / kInitialPackage));
  REQUIRE_FALSE(std::filesystem::exists(scratch.path / kInitialHeader));
  REQUIRE_FALSE(std::filesystem::exists(scratch.path / admission.stage));
  ContentMutationOwnerTestAccess::RequireStreamRollbackDone(*owner, stopped.nonce);
  header = owner->CaptureMetadata(ContentMutationOwner::Kind::kCreate, kInitialPackage, kInitialHeader, error);
  REQUIRE(header); REQUIRE_FALSE(error);
  REQUIRE(owner->CreateStream(kInitialPackage, header, metadata, source).complete);
  CheckStreamPayload(*volume->content(), source);
}
TEST_CASE("Stream recovery preserves foreign members instead of adopting or deleting them",
          "[base][storage][content_mutation][stream][conflict]") {
  VolumeScratch scratch, inputs; auto volume = scratch.Open(); auto owner = volume->mutations();
  NativeStreamInput source(inputs.path / "selected.bin", 4ull << 20);
  std::error_code error;
  auto header = owner->CaptureMetadata(ContentMutationOwner::Kind::kCreate, kInitialPackage, kInitialHeader, error);
  REQUIRE(header); REQUIRE_FALSE(error); std::atomic<uint64_t> progress{0};
  const std::array<uint8_t, 2> metadata{5, 6};
  const auto stopped = ContentMutationOwnerTestAccess::StopStream(*owner, kInitialPackage, header, metadata, source, progress, 1u << 20);
  REQUIRE(stopped.stream_admitted_durable);
  auto stage = ContentMutationOwnerTestAccess::PackageOperation(*owner, stopped.nonce);
  SECTION("Unknown native entry") { std::ofstream(stage->path() / "foreign.bin") << "preserve"; }
  SECTION("Replacement of a recorded inode") {
    std::filesystem::rename(stage->path() / "payload.bin", stage->path() / "retained-original.bin");
    std::ofstream(stage->path() / "payload.bin") << "foreign";
  }
  const auto before = TreeBytes(stage->path());
  REQUIRE(owner->Recover()); REQUIRE_FALSE(owner->ready());
  REQUIRE(TreeBytes(stage->path()) == before);
  REQUIRE_FALSE(std::filesystem::exists(scratch.path / kInitialPackage));
  REQUIRE_FALSE(std::filesystem::exists(scratch.path / kInitialHeader));
}
TEST_CASE("Stream space and existing-destination refusal happen before payload or staging",
          "[base][storage][content_mutation][stream][space]") {
  VolumeScratch scratch, inputs; auto volume = scratch.Open(); auto owner = volume->mutations();
  NativeStreamInput source(inputs.path / "selected.bin", 4ull << 20);
  std::error_code error;
  auto header = owner->CaptureMetadata(ContentMutationOwner::Kind::kCreate, kInitialPackage, kInitialHeader, error);
  REQUIRE(header); REQUIRE_FALSE(error); const std::array<uint8_t, 2> metadata{7, 8};
  SECTION("Deny-only ceiling after actual native space observation") {
    const auto before = TreeBytes(scratch.path);
    const auto refused = ContentMutationOwnerTestAccess::DenyStreamSpace(*owner, kInitialPackage, header, metadata, source);
    REQUIRE(refused.error == std::errc::no_space_on_device); REQUIRE_FALSE(refused.stream_admitted_durable);
    REQUIRE(TreeBytes(scratch.path) == before);
  }
  SECTION("Existing destination is preserved before the first private stage") {
    REQUIRE(std::filesystem::create_directories(scratch.path / kInitialPackage));
    { std::ofstream(scratch.path / kInitialPackage / "original.bin") << "original"; }
    const auto before = TreeBytes(scratch.path);
    const auto refused = owner->CreateStream(kInitialPackage, header, metadata, source);
    REQUIRE(refused.error == std::errc::file_exists); REQUIRE_FALSE(refused.stream_admitted_durable);
    REQUIRE(TreeBytes(scratch.path) == before);
  }
  REQUIRE(source.reads == 0); REQUIRE(owner->ready()); REQUIRE_FALSE(source.CheckCurrent());
}
}  // namespace xe::filesystem::test
#endif

#if XE_PLATFORM_LINUX || XE_PLATFORM_MAC
namespace xe::filesystem::test {
TEST_CASE("Stream cancellation during complete-tree authentication aborts before complete intent",
          "[base][storage][content_mutation][stream][cancel]") {
  VolumeScratch scratch, inputs; auto volume = scratch.Open(); auto owner = volume->mutations();
  NativeStreamInput source(inputs.path / "selected.bin", 4ull << 20);
  std::error_code error;
  auto header = owner->CaptureMetadata(ContentMutationOwner::Kind::kCreate,
      kInitialPackage, kInitialHeader, error);
  REQUIRE(header); REQUIRE_FALSE(error);
  std::atomic<bool> canceled{false}; std::atomic<uint64_t> progress{0};
  const std::array<uint8_t, 2> metadata{0xCA, 0xCE};
  // This seam only raises the genuine cancellation flag after native full-tree
  // hashing has completed. It cannot skip, alter or fabricate successful I/O.
  const auto stopped = ContentMutationOwnerTestAccess::CancelStreamAfterAuthentication(
      *owner, kInitialPackage, header, metadata, source, canceled, progress);
  REQUIRE(canceled.load()); REQUIRE(progress.load() == source.captured.size);
  REQUIRE(source.reads == 4); REQUIRE(source.largest_read == (1u << 20));
  REQUIRE(stopped.error == std::errc::operation_canceled);
  REQUIRE(stopped.stream_admitted_durable); REQUIRE_FALSE(stopped.prepared_durable);
  REQUIRE_FALSE(stopped.complete); REQUIRE(owner->ready());
  REQUIRE_FALSE(std::filesystem::exists(scratch.path / kInitialPackage));
  REQUIRE_FALSE(std::filesystem::exists(scratch.path / kInitialHeader));
  // Cancellation must leave no complete publication intent, while retaining
  // the authenticated stream admission and its required rollback terminal.
  ContentMutationOwnerTestAccess::NoCompleteIntent(*owner, stopped.nonce);
  const auto admission = ContentMutationOwnerTestAccess::StreamRecord(*owner, stopped.nonce);
  REQUIRE(admission.streamed); REQUIRE(admission.stream_pending);
  ContentMutationOwnerTestAccess::RequirePreparationBinding(*owner, admission);
  ContentMutationOwnerTestAccess::RequireStreamRollbackDone(*owner, stopped.nonce);
  REQUIRE_FALSE(owner->Recover()); REQUIRE_FALSE(source.CheckCurrent());
  canceled.store(false);
  REQUIRE(owner->CreateStream(kInitialPackage, header, metadata, source, &canceled).complete);
  CheckStreamPayload(*volume->content(), source);
}
}  // namespace xe::filesystem::test
#endif


#if XE_PLATFORM_LINUX || XE_PLATFORM_MAC
namespace xe::filesystem::test {
namespace {
std::vector<uint8_t> NativeBytes(const std::filesystem::path& path) {
  std::ifstream file(path,std::ios::binary); REQUIRE(file.good());
  return {std::istreambuf_iterator<char>(file),{}};
}
void NoPreparedDestination(const VolumeScratch& scratch) {
  REQUIRE_FALSE(std::filesystem::exists(scratch.path/kInitialPackage));
  REQUIRE_FALSE(std::filesystem::exists(scratch.path/kInitialHeader));
}
}  // namespace
TEST_CASE("Create preparation survives real directory member and Header cuts without publication",
          "[base][storage][content_mutation][preparation][restart]") {
  unsigned boundary = 5;
  SECTION("Plan durable before directory") { boundary = 5; }
  SECTION("Actual empty directory exists") { boundary = 8; }
  SECTION("Actual first member bytes exist") { boundary = 4; }
  SECTION("Actual Header sibling stage is durable") { boundary = 6; }
  VolumeScratch scratch; auto volume = scratch.Open(); auto owner = volume->mutations();
  std::error_code error; const auto members = InitialMembers(); const std::array<uint8_t,3> metadata{1,4,7};
  auto header = owner->CaptureMetadata(ContentMutationOwner::Kind::kCreate,kInitialPackage,kInitialHeader,error);
  REQUIRE(header); REQUIRE_FALSE(error);
  auto stopped = ContentMutationOwnerTestAccess::StopInitial(*owner,kInitialPackage,header,metadata,members,boundary);
  REQUIRE(stopped.error); REQUIRE_FALSE(stopped.prepared_durable); REQUIRE_FALSE(owner->ready()); NoPreparedDestination(scratch);
  const auto plan = ContentMutationOwnerTestAccess::ReadPreparation(*owner,stopped.nonce);
  const auto staging = ContentMutationOwnerTestAccess::StageDirectory(*owner);
  const auto directory = staging/("package-"+stopped.nonce+".tmp");
  const auto preserved = staging/("preserved-"+stopped.nonce+"-d");
  uint64_t inode = 0;
  if (boundary != 5) { struct stat stat{}; REQUIRE(lstat(directory.c_str(),&stat)==0); inode = stat.st_ino; }
  header.reset(); stopped.metadata = {}; owner.reset(); volume.reset();
  volume = scratch.Open(); owner = volume->mutations(); REQUIRE(owner->ready()); NoPreparedDestination(scratch);
  ContentMutationOwnerTestAccess::RequireAborted(*owner,stopped.nonce);
  REQUIRE_FALSE(std::filesystem::exists(directory));
  if (boundary != 5) {
    struct stat stat{}; REQUIRE(lstat(preserved.c_str(),&stat)==0); REQUIRE(uint64_t(stat.st_ino)==inode);
    const auto paths = owner->PreservedArtifacts(); REQUIRE(std::find(paths.begin(),paths.end(),preserved)!=paths.end());
  }
  if (boundary == 4 || boundary == 6) { REQUIRE(NativeBytes(preserved/"Account")==members.front().bytes); }
  if (boundary == 6) {
    const auto header_copy = staging/("preserved-"+stopped.nonce+"-h");
    REQUIRE(NativeBytes(header_copy)==std::vector<uint8_t>(metadata.begin(),metadata.end()));
    REQUIRE_FALSE(std::filesystem::exists(scratch.path/content_mutation_wire::PreparationStage(plan)));
  }
  const auto recovered = TreeBytes(scratch.path); REQUIRE_FALSE(owner->Recover()); REQUIRE(TreeBytes(scratch.path)==recovered);
  header = owner->CaptureMetadata(ContentMutationOwner::Kind::kCreate,kInitialPackage,kInitialHeader,error);
  REQUIRE(header); REQUIRE_FALSE(error); REQUIRE(owner->CreateInitial(kInitialPackage,header,metadata,members).complete);
  CheckInitialBytes(*volume->content(),members);
}
TEST_CASE("Stream preparation recovery requires no selected input or payload replay",
          "[base][storage][content_mutation][stream][preparation]") {
  unsigned boundary = 4;
  SECTION("Plan before directory") { boundary=5; }
  SECTION("Created directory") { boundary=8; }
  SECTION("Precreated actual member") { boundary=4; }
  SECTION("Header before stream admission") { boundary=6; }
  VolumeScratch scratch,inputs; auto volume=scratch.Open(); auto owner=volume->mutations();
  // A real retained sparse host input larger than detached-tree capacity. No
  // payload is read before the canonical stream admission exists.
  auto source=std::make_unique<NativeStreamInput>(inputs.path/"large.bin",(129ull<<20)+17);
  std::error_code error; auto header=owner->CaptureMetadata(ContentMutationOwner::Kind::kCreate,kInitialPackage,kInitialHeader,error);
  REQUIRE(header); REQUIRE_FALSE(error); const std::array<uint8_t,2> metadata{4,9};
  auto stopped=ContentMutationOwnerTestAccess::StopStreamPreparation(*owner,kInitialPackage,header,metadata,*source,boundary);
  REQUIRE(stopped.error); REQUIRE_FALSE(stopped.stream_admitted_durable); REQUIRE(source->reads==0);
  source.reset(); header.reset(); stopped.metadata={}; owner.reset(); volume.reset();
  volume=scratch.Open(); owner=volume->mutations(); REQUIRE(owner->ready()); NoPreparedDestination(scratch);
  ContentMutationOwnerTestAccess::RequireAborted(*owner,stopped.nonce);
  REQUIRE_FALSE(owner->Recover());
}
TEST_CASE("Canonical phase append never treats a partial private record as a committed obligation",
          "[base][storage][content_mutation][preparation][record]") {
  std::string phase=".plan"; unsigned boundary=3; bool stream=false, abort=false, committed=false, uncertain=false;
  SECTION("Partial plan control bytes") { phase=".plan"; boundary=3; }
  SECTION("Full temporary plan before rename") { phase=".plan"; boundary=1; }
  SECTION("Canonical plan before parent barrier") { phase=".plan"; boundary=2; }
  SECTION("Stream temporary admission") { phase=".stream"; boundary=3; stream=true; }
  SECTION("Canonical stream admission before barrier") { phase=".stream"; boundary=2; stream=true; }
  SECTION("Partial complete intent") { phase=".intent"; boundary=3; }
  SECTION("Canonical complete intent before barrier") { phase=".intent"; boundary=2; committed=true; }
  SECTION("Partial Done after real publication") { phase=".done"; boundary=3; committed=true; }
  SECTION("Canonical Done before parent barrier") { phase=".done"; boundary=2; committed=true; }
  SECTION("Partial preservation observation") { phase=".keep0"; boundary=3; abort=true; }
  SECTION("Canonical preservation observation before barrier") { phase=".keep0"; boundary=2; abort=true; }
  SECTION("Partial public Header preservation observation") { phase=".keep1"; boundary=3; uncertain=true; }
  SECTION("Canonical public Header observation before barrier") { phase=".keep1"; boundary=2; uncertain=true; }
  SECTION("Partial abort terminal") { phase=".abort"; boundary=3; abort=true; }
  SECTION("Canonical abort before parent barrier") { phase=".abort"; boundary=2; abort=true; }
  VolumeScratch scratch,inputs; auto volume=scratch.Open(); auto owner=volume->mutations();
  std::error_code error; auto header=owner->CaptureMetadata(ContentMutationOwner::Kind::kCreate,kInitialPackage,kInitialHeader,error);
  REQUIRE(header); REQUIRE_FALSE(error); const auto members=InitialMembers(); const std::array<uint8_t,2> metadata{5,6};
  NativeStreamInput source(inputs.path/"source.bin",4ull<<20);
  ContentMutationOwnerTestAccess::StopRecord(*owner,phase,boundary);
  auto stopped = stream ? owner->CreateStream(kInitialPackage,header,metadata,source) :
      uncertain ? ContentMutationOwnerTestAccess::UncertainInitialStage(*owner,kInitialPackage,header,metadata,members) :
      ContentMutationOwnerTestAccess::StopInitial(*owner,kInitialPackage,header,metadata,members,abort?7:0);
  REQUIRE(stopped.error); REQUIRE_FALSE(stopped.complete); REQUIRE_FALSE(owner->ready());
  const auto records=ContentMutationOwnerTestAccess::RecordDirectory(*owner);
  std::vector<std::filesystem::path> partials;
  for (const auto& entry:std::filesystem::directory_iterator(records)) {
    if (entry.path().filename().string().find(".partial-")!=std::string::npos) { partials.push_back(entry.path()); }
  }
  REQUIRE(partials.empty()==(boundary==2));
  const auto saved = partials.empty()?std::vector<uint8_t>{}:NativeBytes(partials.front());
  header.reset(); stopped.metadata={}; owner.reset(); volume.reset();
  volume=scratch.Open(); owner=volume->mutations(); REQUIRE(owner->ready());
  REQUIRE(std::filesystem::exists(scratch.path/kInitialPackage)==committed);
  REQUIRE(std::filesystem::exists(scratch.path/kInitialHeader)==committed);
  if (committed) { CheckInitialBytes(*volume->content(),members); }
  else if (phase!=".plan" || boundary==2) {
    if (!stream || boundary!=2) { ContentMutationOwnerTestAccess::RequireAborted(*owner,stopped.nonce); }
    else { ContentMutationOwnerTestAccess::RequireStreamRollbackDone(*owner,stopped.nonce); }
  }
  if (!partials.empty()) {
    REQUIRE(NativeBytes(partials.front())==saved);
    const auto paths=owner->PreservedArtifacts(); REQUIRE(std::find(paths.begin(),paths.end(),partials.front())!=paths.end());
  }
  const auto before=TreeBytes(scratch.path); REQUIRE_FALSE(owner->Recover()); REQUIRE(TreeBytes(scratch.path)==before);
}
TEST_CASE("Preparation quarantine replays its actual ticket and rename boundaries",
          "[base][storage][content_mutation][preparation][quarantine]") {
  unsigned boundary=1; SECTION("Ticket before rename") { boundary=1; } SECTION("Rename before abort") { boundary=2; }
  VolumeScratch scratch; auto volume=scratch.Open(); auto owner=volume->mutations(); std::error_code error;
  auto header=owner->CaptureMetadata(ContentMutationOwner::Kind::kCreate,kInitialPackage,kInitialHeader,error);
  REQUIRE(header); REQUIRE_FALSE(error); const auto members=InitialMembers(); const std::array<uint8_t,1> metadata{8};
  auto stopped=ContentMutationOwnerTestAccess::StopInitial(*owner,kInitialPackage,header,metadata,members,4);
  REQUIRE(stopped.error); REQUIRE(ContentMutationOwnerTestAccess::StopPreservation(*owner,stopped.nonce,boundary));
  const auto staging=ContentMutationOwnerTestAccess::StageDirectory(*owner);
  const auto source=staging/("package-"+stopped.nonce+".tmp"), destination=staging/("preserved-"+stopped.nonce+"-d");
  REQUIRE(std::filesystem::exists(source)==(boundary==1)); REQUIRE(std::filesystem::exists(destination)==(boundary==2));
  header.reset(); stopped.metadata={}; owner.reset(); volume.reset();
  volume=scratch.Open(); owner=volume->mutations(); REQUIRE(owner->ready());
  REQUIRE(NativeBytes(destination/"Account")==members.front().bytes); NoPreparedDestination(scratch);
  ContentMutationOwnerTestAccess::RequireAborted(*owner,stopped.nonce);
}
TEST_CASE("Unprepared data conflicts are preserved and floor denial remains retryable",
          "[base][storage][content_mutation][preparation][conflict]") {
  VolumeScratch scratch; auto volume=scratch.Open(); auto owner=volume->mutations(); std::error_code error;
  auto header=owner->CaptureMetadata(ContentMutationOwner::Kind::kCreate,kInitialPackage,kInitialHeader,error);
  REQUIRE(header); REQUIRE_FALSE(error); const auto members=InitialMembers(); const std::array<uint8_t,1> metadata{8};
  auto stopped=ContentMutationOwnerTestAccess::StopInitial(*owner,kInitialPackage,header,metadata,members,4);
  auto stage=ContentMutationOwnerTestAccess::PackageOperation(*owner,stopped.nonce); const auto staged=stage->path(); stage.reset();
  bool floor=false;
  SECTION("Symlink cannot be followed or silently removed") { REQUIRE(symlink("Account",(staged/"link").c_str())==0); }
  SECTION("Hardlink cannot be adopted as owned bytes") { REQUIRE(link((staged/"Account").c_str(),(staged/"alias").c_str())==0); }
  SECTION("Different destination blocks exact no-replace preservation") {
    std::filesystem::create_directory(staged.parent_path()/("preserved-"+stopped.nonce+"-d"));
  }
  SECTION("Deny-only ceiling follows genuine native capacity observation") {
    floor=true; ContentMutationOwnerTestAccess::DenyPreparationSpace(*owner,true);
  }
  const auto before=TreeBytes(scratch.path);
  const auto result=owner->Recover(); REQUIRE(result); REQUIRE_FALSE(owner->ready());
  if (floor) { REQUIRE(result==std::errc::no_space_on_device); }
  // A failed observation can append only its actual private control ticket;
  // every original Account byte and every foreign entry remains intact.
  REQUIRE(NativeBytes(staged/"Account")==members.front().bytes); NoPreparedDestination(scratch);
  if (floor) {
    REQUIRE(TreeBytes(scratch.path)==before); ContentMutationOwnerTestAccess::DenyPreparationSpace(*owner,false);
    REQUIRE_FALSE(owner->Recover()); REQUIRE(owner->ready());
  }
}
TEST_CASE("Ordinary pre-admission cancellation and Header uncertainty preserve data then restore availability",
          "[base][storage][content_mutation][preparation][failure]") {
  bool uncertain=false; SECTION("Cancellation after real first member") {} SECTION("Actual Header write with uncertain parent barrier") { uncertain=true; }
  VolumeScratch scratch; auto volume=scratch.Open(); auto owner=volume->mutations(); std::error_code error;
  auto header=owner->CaptureMetadata(ContentMutationOwner::Kind::kCreate,kInitialPackage,kInitialHeader,error);
  REQUIRE(header); REQUIRE_FALSE(error); const auto members=InitialMembers(); const std::array<uint8_t,2> metadata{2,7};
  auto result=uncertain?ContentMutationOwnerTestAccess::UncertainInitialStage(*owner,kInitialPackage,header,metadata,members):
      ContentMutationOwnerTestAccess::StopInitial(*owner,kInitialPackage,header,metadata,members,7);
  REQUIRE(result.error); REQUIRE(result.aborted_before_publication); REQUIRE_FALSE(result.complete); REQUIRE(owner->ready());
  REQUIRE_FALSE(result.preserved_artifacts.empty()); NoPreparedDestination(scratch);
  ContentMutationOwnerTestAccess::RequireAborted(*owner,result.nonce);
  const auto retained=TreeBytes(scratch.path); REQUIRE_FALSE(owner->Recover()); REQUIRE(TreeBytes(scratch.path)==retained);
}
TEST_CASE("A durable preservation ticket cannot be reminted for a replacement inode",
          "[base][storage][content_mutation][preparation][replacement]") {
  VolumeScratch scratch; auto volume=scratch.Open(); auto owner=volume->mutations(); std::error_code error;
  auto header=owner->CaptureMetadata(ContentMutationOwner::Kind::kCreate,kInitialPackage,kInitialHeader,error);
  REQUIRE(header); REQUIRE_FALSE(error); const auto members=InitialMembers(); const std::array<uint8_t,1> metadata{3};
  auto stopped=ContentMutationOwnerTestAccess::StopInitial(*owner,kInitialPackage,header,metadata,members,4);
  REQUIRE(stopped.error);
  REQUIRE(ContentMutationOwnerTestAccess::StopPreservation(*owner,stopped.nonce,1));
  const auto staging=ContentMutationOwnerTestAccess::StageDirectory(*owner);
  const auto original=staging/("package-"+stopped.nonce+".tmp");
  const auto escaped=scratch.path/"fixture-retained-original";
  std::filesystem::rename(original,escaped); REQUIRE(std::filesystem::create_directory(original));
  { std::ofstream file(original/"foreign.bin",std::ios::binary); file<<"foreign retained bytes"; }
  const auto before=TreeBytes(scratch.path);
  REQUIRE(owner->Recover()); REQUIRE_FALSE(owner->ready()); REQUIRE(TreeBytes(scratch.path)==before);
  REQUIRE(NativeBytes(escaped/"Account")==members.front().bytes);
  REQUIRE(std::filesystem::exists(original/"foreign.bin")); NoPreparedDestination(scratch);
}
TEST_CASE("Legacy private orphans are preserved without touching unclaimed public Header siblings",
          "[base][storage][content_mutation][preparation][legacy]") {
  VolumeScratch scratch; auto volume=scratch.Open(); auto owner=volume->mutations(); std::error_code error;
  auto header=owner->CaptureMetadata(ContentMutationOwner::Kind::kCreate,kInitialPackage,kInitialHeader,error);
  REQUIRE(header); REQUIRE_FALSE(error);
  const std::string nonce="aabbccddeeff00112233445566778899";
  const auto staging=ContentMutationOwnerTestAccess::StageDirectory(*owner);
  const auto directory=staging/("package-"+nonce+".tmp"); REQUIRE(std::filesystem::create_directory(directory));
  REQUIRE(chmod(directory.c_str(),0700)==0);
  const std::array<uint8_t,3> bytes{8,7,6};
  { std::ofstream file(directory/"legacy.bin",std::ios::binary); file.write(reinterpret_cast<const char*>(bytes.data()),bytes.size()); }
  const auto public_stage=scratch.path/kInitialHeader.parent_path()/(".xenia-stage-"+nonce+".tmp");
  { std::ofstream file(public_stage,std::ios::binary); file<<"unclaimed old Header evidence"; }
  const auto public_bytes=NativeBytes(public_stage);
  header.reset(); owner.reset(); volume.reset(); volume=scratch.Open(); owner=volume->mutations();
  REQUIRE(owner->ready()); NoPreparedDestination(scratch);
  const auto destination=staging/("preserved-"+nonce+"-d"); REQUIRE(NativeBytes(destination/"legacy.bin")==std::vector<uint8_t>(bytes.begin(),bytes.end()));
  REQUIRE(NativeBytes(public_stage)==public_bytes);
  const auto before=TreeBytes(scratch.path); REQUIRE_FALSE(owner->Recover()); REQUIRE(TreeBytes(scratch.path)==before);
}
TEST_CASE("Partial planned Header bytes are preserved rather than decoded or adopted",
          "[base][storage][content_mutation][preparation][partial_header]") {
  unsigned boundary=0;
  SECTION("Observe partial Header and preserve directly") {}
  SECTION("Public Header ticket before rename") { boundary=1; }
  SECTION("Public Header moved before terminal") { boundary=2; }
  VolumeScratch scratch; auto volume=scratch.Open(); auto owner=volume->mutations(); std::error_code error;
  auto header=owner->CaptureMetadata(ContentMutationOwner::Kind::kCreate,kInitialPackage,kInitialHeader,error);
  REQUIRE(header); REQUIRE_FALSE(error); const auto members=InitialMembers(); const std::array<uint8_t,2> metadata{1,2};
  auto stopped=ContentMutationOwnerTestAccess::StopInitial(*owner,kInitialPackage,header,metadata,members,5);
  const auto plan=ContentMutationOwnerTestAccess::ReadPreparation(*owner,stopped.nonce);
  const auto stage=scratch.path/content_mutation_wire::PreparationStage(plan);
  { std::ofstream file(stage,std::ios::binary); file<<"incomplete native stage bytes"; }
  REQUIRE(chmod(stage.c_str(),0600)==0); const auto bytes=NativeBytes(stage);
  if (boundary) {
    REQUIRE(ContentMutationOwnerTestAccess::StopPreservation(*owner,stopped.nonce,boundary));
    header.reset(); stopped.metadata={}; owner.reset(); volume.reset();
    volume=scratch.Open(); owner=volume->mutations();
  } else { REQUIRE_FALSE(owner->Recover()); }
  REQUIRE(owner->ready()); NoPreparedDestination(scratch);
  const auto preserved=ContentMutationOwnerTestAccess::StageDirectory(*owner)/("preserved-"+stopped.nonce+"-h");
  REQUIRE(NativeBytes(preserved)==bytes); REQUIRE_FALSE(std::filesystem::exists(stage));
  ContentMutationOwnerTestAccess::RequireAborted(*owner,stopped.nonce);
}
TEST_CASE("Create wire binds the exact preparation and refuses checksum-valid mismatched plans",
          "[base][storage][content_mutation][preparation][wire]") {
  VolumeScratch scratch; auto volume=scratch.Open(); auto owner=volume->mutations(); std::error_code error;
  auto header=owner->CaptureMetadata(ContentMutationOwner::Kind::kCreate,kInitialPackage,kInitialHeader,error);
  REQUIRE(header); REQUIRE_FALSE(error); const auto members=InitialMembers(); const std::array<uint8_t,2> metadata{1,2};
  auto stopped=ContentMutationOwnerTestAccess::StopInitial(*owner,kInitialPackage,header,metadata,members,1);
  REQUIRE(stopped.prepared_durable); auto plan=ContentMutationOwnerTestAccess::ReadPreparation(*owner,stopped.nonce);
  const auto record=ContentMutationOwnerTestAccess::ReadRecord(*owner,stopped.nonce);
  const auto bytes=content_mutation_wire::EncodePreparation(plan,error); REQUIRE_FALSE(error);
  REQUIRE(content_mutation_wire::MatchesPreparation(plan,bytes,record,error)); REQUIRE_FALSE(error);
  plan.header_bytes++;
  const auto mismatch=content_mutation_wire::EncodePreparation(plan,error); REQUIRE_FALSE(error);
  REQUIRE_FALSE(content_mutation_wire::MatchesPreparation(plan,mismatch,record,error));
  const auto path=ContentMutationOwnerTestAccess::RecordDirectory(*owner)/("content-"+stopped.nonce+".plan");
  { std::ofstream file(path,std::ios::binary|std::ios::trunc); file.write(reinterpret_cast<const char*>(mismatch.data()),mismatch.size()); }
  const auto before=TreeBytes(scratch.path); REQUIRE(owner->Recover()); REQUIRE_FALSE(owner->ready());
  REQUIRE(TreeBytes(scratch.path)==before); NoPreparedDestination(scratch);
}
namespace {
std::vector<uint8_t> OrphanHeaderBytes() {
  std::vector<uint8_t> bytes(733);
  for (size_t i=0;i!=bytes.size();++i) { bytes[i]=uint8_t((i*37+11)&255); }
  return bytes;  // Complete format-neutral bytes, including unknown padding/tail.
}
std::shared_ptr<const HostStorageSnapshotToken> MakeOrphanHeader(
    const VolumeScratch& scratch, ContentMutationOwner& owner, std::span<const uint8_t> bytes) {
  std::error_code error;
  auto absent=owner.CaptureMetadata(ContentMutationOwner::Kind::kCreate,kInitialPackage,kInitialHeader,error);
  REQUIRE(absent); REQUIRE_FALSE(error); REQUIRE_FALSE(absent->exists());
  auto made=owner.Create(kInitialPackage,absent,bytes); REQUIRE(made.complete); made.metadata={}; absent.reset();
  REQUIRE(std::filesystem::remove(scratch.path/kInitialPackage));
  auto orphan=owner.CaptureMetadata(ContentMutationOwner::Kind::kCreate,kInitialPackage,kInitialHeader,error);
  REQUIRE(orphan); REQUIRE_FALSE(error); REQUIRE(orphan->exists()); return orphan;
}
void RequireExactOrphan(const VolumeScratch& scratch,uint64_t inode,std::span<const uint8_t> bytes) {
  REQUIRE_FALSE(std::filesystem::exists(scratch.path/kInitialPackage));
  struct stat info{}; REQUIRE(lstat((scratch.path/kInitialHeader).c_str(),&info)==0);
  REQUIRE(uint64_t(info.st_ino)==inode);
  REQUIRE(NativeBytes(scratch.path/kInitialHeader)==std::vector<uint8_t>(bytes.begin(),bytes.end()));
}
}  // namespace
TEST_CASE("Empty create preserves an authenticated orphan Header across plan-only restart cuts",
          "[base][storage][content_mutation][preparation][orphan_header]") {
  unsigned boundary=5;
  SECTION("Plan before package stage") { boundary=5; }
  SECTION("Empty package stage exists") { boundary=8; }
  SECTION("Header stage and exact old backup exist") { boundary=6; }
  VolumeScratch scratch; auto volume=scratch.Open(); auto owner=volume->mutations();
  const auto before=OrphanHeaderBytes(); auto header=MakeOrphanHeader(scratch,*owner,before);
  const auto inode=header->info().file_id; auto intended=before; intended[12]^=0x39;
  auto stopped=ContentMutationOwnerTestAccess::StopInitial(*owner,kInitialPackage,header,intended,{},boundary);
  REQUIRE(stopped.error); REQUIRE_FALSE(stopped.prepared_durable); REQUIRE_FALSE(owner->ready());
  const auto plan=ContentMutationOwnerTestAccess::ReadPreparation(*owner,stopped.nonce);
  REQUIRE(plan.before_file==inode); REQUIRE(plan.before_bytes==before.size());
  RequireExactOrphan(scratch,inode,before);
  const auto backup=ContentMutationOwnerTestAccess::RecoveryDirectory(*owner)/("snapshot-"+stopped.nonce+".bin");
  if (boundary==6) { REQUIRE(NativeBytes(backup)==before); }
  header.reset(); stopped.metadata={}; owner.reset(); volume.reset();
  volume=scratch.Open(); owner=volume->mutations(); REQUIRE(owner->ready()); RequireExactOrphan(scratch,inode,before);
  ContentMutationOwnerTestAccess::RequireAborted(*owner,stopped.nonce);
  if (boundary==6) {
    REQUIRE(NativeBytes(backup)==before);
    const auto paths=owner->PreservedArtifacts(); REQUIRE(std::find(paths.begin(),paths.end(),backup)!=paths.end());
  }
  const auto recovered=TreeBytes(scratch.path); REQUIRE_FALSE(owner->Recover()); REQUIRE(TreeBytes(scratch.path)==recovered);
  std::error_code error; header=owner->CaptureMetadata(ContentMutationOwner::Kind::kCreate,kInitialPackage,kInitialHeader,error);
  REQUIRE(header); REQUIRE_FALSE(error); REQUIRE(header->exists());
  REQUIRE(owner->Create(kInitialPackage,header,intended).complete);
  REQUIRE(NativeBytes(scratch.path/kInitialHeader)==intended); REQUIRE(std::filesystem::is_empty(scratch.path/kInitialPackage));
}
TEST_CASE("Prepared orphan Header creation retains normal publisher backup and commit authority",
          "[base][storage][content_mutation][preparation][orphan_header][commit]") {
  unsigned boundary=1;
  SECTION("Complete intent before Header CAS") { boundary=1; }
  SECTION("Actual Header CAS before package publication") { boundary=2; }
  SECTION("Actual package publication before Done") { boundary=3; }
  VolumeScratch scratch; auto volume=scratch.Open(); auto owner=volume->mutations();
  const auto before=OrphanHeaderBytes(); auto header=MakeOrphanHeader(scratch,*owner,before); auto intended=before; intended[23]^=0x55;
  auto stopped=ContentMutationOwnerTestAccess::StopInitial(*owner,kInitialPackage,header,intended,{},boundary);
  REQUIRE(stopped.error); REQUIRE(stopped.prepared_durable); REQUIRE_FALSE(stopped.complete);
  const auto record=ContentMutationOwnerTestAccess::ReadRecord(*owner,stopped.nonce);
  REQUIRE(record.before_exists); REQUIRE(record.before==before); REQUIRE(record.intended==intended);
  const auto backup=ContentMutationOwnerTestAccess::RecoveryDirectory(*owner)/record.recovery;
  REQUIRE(NativeBytes(backup)==before);
  header.reset(); stopped.metadata={}; owner.reset(); volume.reset();
  volume=scratch.Open(); owner=volume->mutations(); REQUIRE(owner->ready()); REQUIRE(owner->Visible(kInitialPackage));
  REQUIRE(NativeBytes(scratch.path/kInitialHeader)==intended); REQUIRE(NativeBytes(backup)==before);
  REQUIRE(std::filesystem::is_empty(scratch.path/kInitialPackage));
}
TEST_CASE("Plan-only orphan Header abort cannot adopt a replaced or modified semantic Header",
          "[base][storage][content_mutation][preparation][orphan_header][conflict]") {
  VolumeScratch scratch; auto volume=scratch.Open(); auto owner=volume->mutations();
  const auto before=OrphanHeaderBytes(); auto header=MakeOrphanHeader(scratch,*owner,before);
  auto stopped=ContentMutationOwnerTestAccess::StopInitial(*owner,kInitialPackage,header,before,{},5);
  const auto target=scratch.path/kInitialHeader;
  SECTION("Same inode changed raw bytes") { std::ofstream file(target,std::ios::binary|std::ios::app); file<<"external change"; }
  SECTION("Different inode with matching raw bytes") {
    std::filesystem::rename(target,scratch.path/"fixture-retained-orphan");
    std::ofstream file(target,std::ios::binary); file.write(reinterpret_cast<const char*>(before.data()),before.size());
  }
  const auto tree=TreeBytes(scratch.path); REQUIRE(owner->Recover()); REQUIRE_FALSE(owner->ready());
  REQUIRE(TreeBytes(scratch.path)==tree); REQUIRE_FALSE(std::filesystem::exists(scratch.path/kInitialPackage));
}
TEST_CASE("Partial old-Header backup is retained opaquely while the exact semantic Header survives",
          "[base][storage][content_mutation][preparation][orphan_header][partial_backup]") {
  VolumeScratch scratch; auto volume=scratch.Open(); auto owner=volume->mutations();
  const auto before=OrphanHeaderBytes(); auto header=MakeOrphanHeader(scratch,*owner,before); const auto inode=header->info().file_id;
  auto stopped=ContentMutationOwnerTestAccess::StopInitial(*owner,kInitialPackage,header,before,{},5);
  const auto backup=ContentMutationOwnerTestAccess::RecoveryDirectory(*owner)/("snapshot-"+stopped.nonce+".bin");
  const std::vector<uint8_t> partial(before.begin(),before.begin()+17);
  { std::ofstream file(backup,std::ios::binary); file.write(reinterpret_cast<const char*>(partial.data()),partial.size()); }
  REQUIRE(chmod(backup.c_str(),0600)==0);
  header.reset(); stopped.metadata={}; owner.reset(); volume.reset();
  volume=scratch.Open(); owner=volume->mutations(); REQUIRE(owner->ready()); RequireExactOrphan(scratch,inode,before);
  REQUIRE(NativeBytes(backup)==partial); ContentMutationOwnerTestAccess::RequireAborted(*owner,stopped.nonce);
  const auto paths=owner->PreservedArtifacts(); REQUIRE(std::find(paths.begin(),paths.end(),backup)!=paths.end());
}
TEST_CASE("Ordinary pre-intent failure leaves an existing orphan Header byte and identity exact",
          "[base][storage][content_mutation][preparation][orphan_header][failure]") {
  VolumeScratch scratch; auto volume=scratch.Open(); auto owner=volume->mutations();
  const auto before=OrphanHeaderBytes(); auto header=MakeOrphanHeader(scratch,*owner,before); const auto inode=header->info().file_id;
  auto result=ContentMutationOwnerTestAccess::UncertainInitialStage(*owner,kInitialPackage,header,before,{});
  REQUIRE(result.error); REQUIRE(result.aborted_before_publication); REQUIRE(owner->ready());
  RequireExactOrphan(scratch,inode,before); ContentMutationOwnerTestAccess::RequireAborted(*owner,result.nonce);
}
}  // namespace xe::filesystem::test
#endif

#if XE_PLATFORM_LINUX || XE_PLATFORM_MAC

namespace xe::filesystem::test {
namespace {
ManagedVolumeAuthority::Options BootstrapOptions(VolumeScratch& scratch,
                                                std::string id="bootstrap-device") {
  ManagedVolumeAuthority::Options options;
  options.content=scratch.root;options.state_parent_override=scratch.root;
  options.source_id=std::move(id);options.device_id=3;return options;
}
std::vector<uint8_t> BootstrapFile(const std::filesystem::path& path) {
  std::ifstream input(path,std::ios::binary);
  return {std::istreambuf_iterator<char>(input),std::istreambuf_iterator<char>()};
}
}
TEST_CASE("Managed bootstrap resumes actual component publication cuts without replacing source identity",
          "[managed_volume_authority][initialization][bootstrap]") {
  unsigned boundaries=0;
  {
    VolumeScratch complete;
    const auto volume=ManagedVolumeAuthority::Open(BootstrapOptions(complete));
    REQUIRE(volume);REQUIRE_FALSE(volume->error());REQUIRE(volume->profile_writable());
    REQUIRE(volume->identity()->binding_required());
    boundaries=ManagedVolumeAuthorityTestAccess::BootstrapBoundaries(*volume);
    REQUIRE(boundaries>=20);REQUIRE(boundaries<=64);
  }
  for (unsigned cut=1;cut<=boundaries;++cut) {
    CAPTURE(cut,boundaries);
    VolumeScratch scratch;
    const std::array<uint8_t,5> original{0x51,0,0xA7,0xFF,0x2C};
    REQUIRE_FALSE(scratch.root->WriteNewFile("existing-save",original).error);
    struct stat before{};REQUIRE(stat((scratch.path/"existing-save").c_str(),&before)==0);
    auto options=BootstrapOptions(scratch);
    auto stopped=ManagedVolumeAuthorityTestAccess::StopBootstrap(options,cut);
    REQUIRE(stopped);REQUIRE(stopped->error()==std::errc::operation_canceled);
    REQUIRE_FALSE(stopped->profile_writable());
    std::optional<ManagedDeviceIdentity::Uuid> uuid;
    std::vector<uint8_t> source;
    if (stopped->identity()) {
      uuid=stopped->identity()->uuid();source=BootstrapFile(scratch.path/ManagedDeviceIdentity::kMarker);
    }
    const auto binding=scratch.path/(".xenia-device-binding-v1-"+BindingKey(options.source_id,scratch.path));
    if (uuid && !std::filesystem::exists(binding/"bootstrap.commit.v1")) {
      std::error_code error;
      REQUIRE_FALSE(ManagedDeviceIdentity::OpenExisting(scratch.root,options.source_id,3,error));
      REQUIRE(error==std::errc::operation_in_progress);
      REQUIRE(stopped->identity()->CheckCurrent()==std::errc::operation_in_progress);
      REQUIRE_FALSE(stopped->identity()->source()->GuestView(error));
      REQUIRE((error==std::errc::operation_in_progress || error==std::errc::permission_denied));
      const auto blocked=ProfileIntentOwner::Open(scratch.root,stopped->identity());
      REQUIRE(blocked);REQUIRE(blocked->error()==std::errc::operation_in_progress);
    }
    if (uuid && stopped->content()->has_guest_projection()) {
      std::error_code error;REQUIRE_FALSE(stopped->content()->GuestView(error));
      REQUIRE(error==std::errc::operation_in_progress);
      REQUIRE_FALSE(HostStorageRoot::AdmitGuestMount(
          std::vector<GuestHostBacking>{stopped->content()->GuestBacking()},error));
      REQUIRE(error==std::errc::operation_in_progress);
    }
    stopped.reset();
    auto volume=ManagedVolumeAuthority::Open(options);
    REQUIRE(volume);REQUIRE_FALSE(volume->error());REQUIRE(volume->profile_writable());
    REQUIRE(volume->mutations()->ready());REQUIRE(volume->identity()->binding_required());
    if (uuid) {
      REQUIRE(volume->identity()->uuid()==*uuid);
      REQUIRE(BootstrapFile(scratch.path/ManagedDeviceIdentity::kMarker)==source);
    }
    struct stat after{};REQUIRE(stat((scratch.path/"existing-save").c_str(),&after)==0);
    REQUIRE(after.st_ino==before.st_ino);REQUIRE(after.st_dev==before.st_dev);
    REQUIRE(BootstrapFile(scratch.path/"existing-save")==std::vector<uint8_t>(original.begin(),original.end()));
    const auto committed=TreeBytes(scratch.path);
    const auto profile=volume->intents()->root()->path();
    const auto recovery=volume->recovery()->root()->path();
    volume.reset();
    const auto reopened=ManagedVolumeAuthority::Open(options);
    REQUIRE(reopened);REQUIRE_FALSE(reopened->error());REQUIRE(reopened->profile_writable());
    REQUIRE(reopened->intents()->root()->path()==profile);
    REQUIRE(reopened->recovery()->root()->path()==recovery);
    REQUIRE(TreeBytes(scratch.path)==committed);
  }
}
TEST_CASE("Source-identical parent selection retains the exact native write capability",
          "[managed_volume_authority][initialization][bootstrap][source_lease]") {
  const bool explicit_parent=GENERATE(false,true);
  VolumeScratch scratch;
  auto options=BootstrapOptions(scratch);
  std::error_code error;
  if (explicit_parent) {
    // A real separately captured capability, not a fabricated lease/root.
    options.state_parent_override=scratch.root->Subroot({},false,error);
    REQUIRE(options.state_parent_override);
    REQUIRE_FALSE(error);
    REQUIRE(options.state_parent_override.get()!=scratch.root.get());
  } else {
    options.state_parent_override.reset();
  }
  auto volume=ManagedVolumeAuthority::Open(options);
  REQUIRE(volume);
  REQUIRE_FALSE(volume->error());
  REQUIRE(volume->profile_writable());
  REQUIRE(volume->mutations()->ready());
  REQUIRE(ManagedVolumeAuthorityTestAccess::HasExactSourceAnchor(*volume));
  REQUIRE(volume->public_root().get()!=volume->content().get());
  REQUIRE(volume->public_root()->path()!=volume->content()->path());
  const auto stable=TreeBytes(scratch.path);
  volume.reset();
  const auto reopened=ManagedVolumeAuthority::Open(options);
  REQUIRE(reopened);
  REQUIRE_FALSE(reopened->error());
  REQUIRE(reopened->profile_writable());
  REQUIRE(ManagedVolumeAuthorityTestAccess::HasExactSourceAnchor(*reopened));
  REQUIRE(reopened->public_root().get()!=reopened->content().get());
  REQUIRE(TreeBytes(scratch.path)==stable);
}
TEST_CASE("Committed bootstrap reclaims its retained public receipt with the exact parent lease",
          "[managed_volume_authority][initialization][bootstrap][retained_public]") {
  unsigned committed_cut=0;
  {
    VolumeScratch complete;
    const auto ready=ManagedVolumeAuthority::Open(BootstrapOptions(complete));
    REQUIRE(ready);
    REQUIRE_FALSE(ready->error());
    committed_cut=ManagedVolumeAuthorityTestAccess::BootstrapBoundaries(*ready);
    REQUIRE(committed_cut>=20);
  }
  VolumeScratch scratch, unrelated;
  std::error_code error;
  auto unrelated_guest=unrelated.root->GuestView(error);
  REQUIRE(unrelated_guest);
  REQUIRE_FALSE(error);
  auto options=BootstrapOptions(scratch);
  // The final producer cut is after durable binding commit, before ordinary
  // Recover, guest seal release and exact public-capsule admission.
  auto stopped=ManagedVolumeAuthorityTestAccess::StopBootstrap(options,committed_cut);
  REQUIRE(stopped);
  REQUIRE(stopped->error()==std::errc::operation_canceled);
  REQUIRE(stopped->identity());
  REQUIRE(stopped->mutations());
  REQUIRE_FALSE(stopped->mutations()->ready());
  const auto uuid=stopped->identity()->uuid();
  const auto public_path=ManagedVolumeAuthorityTestAccess::PublicAuthorityPath(*stopped);
  REQUIRE_FALSE(public_path.empty());
  struct stat before{};
  REQUIRE(stat(public_path.c_str(),&before)==0);
  const auto binding=scratch.path/(".xenia-device-binding-v1-"+
      BindingKey(options.source_id,scratch.path));
  REQUIRE_FALSE(BootstrapFile(binding/"bootstrap.commit.v1").empty());
  REQUIRE_FALSE(stopped->content()->GuestView(error));
  REQUIRE(error==std::errc::operation_in_progress);
  auto raw_public=HostStorageRoot::Open(public_path,false,error);
  REQUIRE(raw_public);
  REQUIRE_FALSE(error);
  REQUIRE_FALSE(raw_public->GuestView(error));
  REQUIRE(error==std::errc::operation_in_progress);
  const auto committed=TreeBytes(scratch.path);
  stopped.reset();  // actual public receipt stays retained by unrelated guest
  auto reopened=ManagedVolumeAuthority::Open(options);
  REQUIRE(reopened);
  REQUIRE_FALSE(reopened->error());
  REQUIRE(reopened->profile_writable());
  REQUIRE(reopened->mutations()->ready());
  REQUIRE(reopened->identity()->uuid()==uuid);
  REQUIRE(ManagedVolumeAuthorityTestAccess::HasExactSourceAnchor(*reopened));
  REQUIRE(reopened->public_root()->path()==public_path);
  struct stat after{};
  REQUIRE(stat(public_path.c_str(),&after)==0);
  REQUIRE(after.st_dev==before.st_dev);
  REQUIRE(after.st_ino==before.st_ino);
  REQUIRE(TreeBytes(scratch.path)==committed);
  const std::array<uint8_t,3> bytes{0x3A,0,0xA9};
  // The same committed native public inode now has an actual usable guest
  // capability, while the replacement projection still excludes private data.
  REQUIRE_FALSE(reopened->public_root()->WriteNewFile("retained-public.data",bytes).error);
  std::vector<uint8_t> read;
  REQUIRE_FALSE(reopened->public_root()->ReadFile("retained-public.data",16,read));
  REQUIRE(read==std::vector<uint8_t>(bytes.begin(),bytes.end()));
  error.clear();
  // A reused typed protector may retain an older source projection. Open a
  // fresh native view to prove the CURRENT common physical private exclusion.
  auto raw_private=HostStorageRoot::Open(reopened->intents()->root()->path(),false,error);
  REQUIRE(raw_private);REQUIRE_FALSE(error);
  REQUIRE_FALSE(raw_private->GuestView(error));
  REQUIRE(error==std::errc::permission_denied);
  REQUIRE_FALSE(unrelated_guest->WriteNewFile("ordinary.data",bytes).error);
}
TEST_CASE("Interrupted binding resumes its exact retained receipt before flat ownership",
          "[managed_volume_authority][initialization][bootstrap][retained_guest]") {
  VolumeScratch scratch, unrelated;
  std::error_code error;
  auto unrelated_guest=unrelated.root->GuestView(error);
  REQUIRE(unrelated_guest);
  REQUIRE_FALSE(error);
  auto options=BootstrapOptions(scratch);
  // Cut3 is after the complete real binding directory has been published,
  // before any managed source marker or component is created.
  auto stopped=ManagedVolumeAuthorityTestAccess::StopBootstrap(options,3);
  REQUIRE(stopped);
  REQUIRE(stopped->error()==std::errc::operation_canceled);
  REQUIRE_FALSE(stopped->profile_writable());
  REQUIRE_FALSE(std::filesystem::exists(scratch.path/ManagedDeviceIdentity::kMarker));
  const auto binding=scratch.path/(".xenia-device-binding-v1-"+
      BindingKey(options.source_id,scratch.path));
  const auto pending=BootstrapFile(binding/"bootstrap.pending.v1");
  REQUIRE_FALSE(pending.empty());
  struct stat before{};
  REQUIRE(stat(binding.c_str(),&before)==0);
  REQUIRE(S_ISDIR(before.st_mode));
  stopped.reset();  // the unrelated guest retains the old registered protector
  auto volume=ManagedVolumeAuthority::Open(options);
  REQUIRE(volume);
  REQUIRE_FALSE(volume->error());
  REQUIRE(volume->profile_writable());
  REQUIRE(volume->mutations()->ready());
  struct stat after{};
  REQUIRE(stat(binding.c_str(),&after)==0);
  REQUIRE(after.st_dev==before.st_dev);
  REQUIRE(after.st_ino==before.st_ino);
  REQUIRE(BootstrapFile(binding/"bootstrap.pending.v1")==pending);
  REQUIRE(volume->identity()->binding_required());
  const std::array<uint8_t,3> ordinary{7,0,9};
  REQUIRE_FALSE(unrelated_guest->WriteNewFile("ordinary.data",ordinary).error);
  auto raw_binding=scratch.root->Subroot(binding.filename(),false,error);
  REQUIRE(raw_binding);
  REQUIRE_FALSE(error);
  REQUIRE_FALSE(raw_binding->GuestView(error));
  REQUIRE(error==std::errc::permission_denied);
}
TEST_CASE("A required bootstrap reference never becomes legacy when its binding is missing",
          "[managed_volume_authority][initialization][bootstrap]") {
  VolumeScratch scratch;auto options=BootstrapOptions(scratch);
  ManagedDeviceIdentity::Uuid uuid;
  {
    auto volume=ManagedVolumeAuthority::Open(options);REQUIRE_FALSE(volume->error());
    uuid=volume->identity()->uuid();
  }
  const auto binding=scratch.path/(".xenia-device-binding-v1-"+BindingKey(options.source_id,scratch.path));
  const auto source=BootstrapFile(scratch.path/ManagedDeviceIdentity::kMarker);
  std::filesystem::rename(binding,scratch.path/"preserved-binding");
  const auto before=TreeBytes(scratch.path);
  std::error_code error;
  REQUIRE_FALSE(ManagedDeviceIdentity::Open(scratch.root,options.source_id,3,error));
  REQUIRE(error==std::errc::state_not_recoverable);
  REQUIRE_FALSE(ManagedDeviceIdentity::CheckExisting(scratch.root,options.source_id,3,error));
  REQUIRE(error==std::errc::state_not_recoverable);
  const auto refused=ManagedVolumeAuthority::Open(options);
  REQUIRE(refused);REQUIRE(refused->error());REQUIRE_FALSE(refused->profile_writable());
  REQUIRE(BootstrapFile(scratch.path/ManagedDeviceIdentity::kMarker)==source);
  REQUIRE(TreeBytes(scratch.path)==before);
}
TEST_CASE("Fresh primary public browsing mkdir survives before and after its real effect",
          "[managed_volume_authority][initialization][bootstrap]") {
  auto prepare=[](VolumeScratch& scratch) {
    REQUIRE_FALSE(scratch.root->Create("content",true));
    REQUIRE_FALSE(scratch.root->Create("public",true));
    std::error_code error;ManagedVolumeAuthority::Options options;
    options.content=scratch.root->Subroot("content",false,error);
    REQUIRE(options.content);REQUIRE_FALSE(error);
    options.source_id="primary-bootstrap";options.device_id=1;
    options.preserve_legacy_layout=true;options.legacy_private_parent=scratch.root;
    options.legacy_public_root=scratch.root->Subroot("public",false,error);
    options.legacy_recovery=SnapshotRecoveryOwner::Open(scratch.root);
    REQUIRE(options.legacy_public_root);REQUIRE(options.legacy_recovery);
    REQUIRE_FALSE(options.legacy_recovery->error());return options;
  };
  unsigned boundaries=0;
  {
    VolumeScratch scratch;auto volume=ManagedVolumeAuthority::Open(prepare(scratch));
    REQUIRE_FALSE(volume->error());boundaries=ManagedVolumeAuthorityTestAccess::BootstrapBoundaries(*volume);
  }
  bool saw_before=false,saw_after=false;
  for (unsigned cut=1;cut<=boundaries;++cut) {
    VolumeScratch scratch;auto options=prepare(scratch);
    const std::array<uint8_t,2> bytes{0x48,0x44};
    REQUIRE_FALSE(options.legacy_public_root->WriteNewFile("host-file",bytes).error);
    auto stopped=ManagedVolumeAuthorityTestAccess::StopBootstrap(options,cut);
    REQUIRE(stopped);REQUIRE(stopped->error()==std::errc::operation_canceled);
    if (stopped->identity()) {
      const bool exists=std::filesystem::is_directory(scratch.path/"public/Content");
      saw_before|=!exists;saw_after|=exists;
    }
    stopped.reset();
    const auto volume=ManagedVolumeAuthority::Open(options);
    REQUIRE(volume);REQUIRE_FALSE(volume->error());REQUIRE(volume->profile_writable());
    REQUIRE(volume->public_root()->path()==scratch.path/"public");
    REQUIRE(std::filesystem::is_directory(scratch.path/"public/Content"));
    REQUIRE(BootstrapFile(scratch.path/"public/host-file")==std::vector<uint8_t>(bytes.begin(),bytes.end()));
  }
  REQUIRE(saw_before);REQUIRE(saw_after);
}
TEST_CASE("An existing public Content payload is preserved and cannot be silently covered",
          "[managed_volume_authority][initialization][bootstrap]") {
  VolumeScratch scratch;
  REQUIRE_FALSE(scratch.root->Create("content",true));
  REQUIRE_FALSE(scratch.root->Create("public",true));
  REQUIRE_FALSE(scratch.root->Create("public/Content",true));
  const std::array<uint8_t,3> bytes{9,1,7};
  REQUIRE_FALSE(scratch.root->WriteNewFile("public/Content/user-data",bytes).error);
  std::error_code error;ManagedVolumeAuthority::Options options;
  options.content=scratch.root->Subroot("content",false,error);options.source_id="public-conflict";options.device_id=1;
  options.preserve_legacy_layout=true;options.legacy_private_parent=scratch.root;
  options.legacy_public_root=scratch.root->Subroot("public",false,error);
  options.legacy_recovery=SnapshotRecoveryOwner::Open(scratch.root);
  REQUIRE(options.legacy_recovery);REQUIRE_FALSE(options.legacy_recovery->error());
  const auto before=TreeBytes(scratch.path);
  const auto refused=ManagedVolumeAuthority::Open(options);
  REQUIRE(refused);REQUIRE(refused->error()==std::errc::directory_not_empty);
  REQUIRE_FALSE(refused->profile_writable());REQUIRE(TreeBytes(scratch.path)==before);
}
}  // namespace xe::filesystem::test

#endif

#if XE_PLATFORM_LINUX || XE_PLATFORM_MAC
namespace xe::filesystem::test {
TEST_CASE("Legacy migration waits for real canonical metadata and profile owners then remains bound",
          "[managed_volume_authority][initialization][bootstrap]") {
  const bool keep_profile=GENERATE(false,true);
  VolumeScratch scratch;
  REQUIRE_FALSE(scratch.root->Create("legacy",true));
  REQUIRE_FALSE(scratch.root->Create("shell",true));
  REQUIRE_FALSE(scratch.root->Create("shell/Content",true));
  std::error_code error;
  auto source=scratch.root->Subroot("legacy",false,error);REQUIRE(source);
  auto shell=scratch.root->Subroot("shell",false,error);REQUIRE(shell);
  std::shared_ptr<const SnapshotRecoveryOwner> recovery=SnapshotRecoveryOwner::Open(scratch.root);
  REQUIRE(recovery);REQUIRE_FALSE(recovery->error());
  auto identity=ManagedDeviceIdentity::Open(source,"legacy-busy",3,error);
  REQUIRE(identity);REQUIRE_FALSE(error);
  auto profile=ProfileIntentOwner::Open(scratch.root,identity);
  REQUIRE(profile);REQUIRE_FALSE(profile->error());
  const auto uuid=identity->uuid();const auto epoch=identity->content_epoch();
  const auto old_source=BootstrapFile(source->path()/ManagedDeviceIdentity::kMarker);
  if (!keep_profile) {profile.reset();}
  const auto before=TreeBytes(scratch.path);
  ManagedVolumeAuthority::Options options;options.content=source;
  options.source_id="legacy-busy";options.device_id=3;
  options.existing_legacy=ManagedVolumeAuthority::Options::ExistingLegacy{scratch.root,shell,recovery};
  auto busy=ManagedVolumeAuthority::Open(options);
  REQUIRE(busy);REQUIRE(busy->error()==std::errc::device_or_resource_busy);
  REQUIRE_FALSE(busy->profile_writable());REQUIRE(TreeBytes(scratch.path)==before);
  busy.reset();profile.reset();identity.reset();
  auto migrated=ManagedVolumeAuthority::Open(options);
  REQUIRE(migrated);REQUIRE_FALSE(migrated->error());REQUIRE(migrated->profile_writable());
  REQUIRE(migrated->identity()->uuid()==uuid);REQUIRE(migrated->identity()->content_epoch()==epoch);
  REQUIRE(migrated->identity()->binding_required());
  const auto tagged=BootstrapFile(source->path()/ManagedDeviceIdentity::kMarker);
  REQUIRE(tagged.size()==old_source.size()+168);
  REQUIRE(std::equal(old_source.begin(),old_source.end(),tagged.begin()));
  migrated.reset();
  const auto binding=source->path()/(".xenia-device-binding-v1-"+BindingKey("legacy-busy",source->path()));
  std::filesystem::rename(binding,scratch.path/"held-legacy-binding");
  const auto absent=TreeBytes(scratch.path);
  REQUIRE_FALSE(ManagedDeviceIdentity::Open(source,"legacy-busy",3,error));
  REQUIRE(error==std::errc::state_not_recoverable);
  const auto refused=ManagedVolumeAuthority::Open(options);
  REQUIRE(refused);REQUIRE(refused->error());REQUIRE_FALSE(refused->profile_writable());
  REQUIRE(TreeBytes(scratch.path)==absent);
}
}  // namespace xe::filesystem::test
#endif

#if XE_PLATFORM_LINUX || XE_PLATFORM_MAC
namespace xe::filesystem::test {
TEST_CASE("Checksum-valid source references still require the exact native binding",
          "[managed_volume_authority][initialization][bootstrap]") {
  VolumeScratch scratch;auto options=BootstrapOptions(scratch);
  {const auto owner=ManagedVolumeAuthority::Open(options);REQUIRE_FALSE(owner->error());}
  const auto marker=scratch.path/ManagedDeviceIdentity::kMarker;
  const auto changed=ManagedVolumeAuthorityTestAccess::WrongBindingTag(BootstrapFile(marker));
  {std::ofstream output(marker,std::ios::binary|std::ios::trunc);
   output.write(reinterpret_cast<const char*>(changed.data()),std::streamsize(changed.size()));
   REQUIRE(output.good());}
  const auto before=TreeBytes(scratch.path);std::error_code error;
  REQUIRE_FALSE(ManagedDeviceIdentity::OpenExisting(scratch.root,options.source_id,3,error));
  REQUIRE(error==std::errc::state_not_recoverable);
  const auto refused=ManagedVolumeAuthority::Open(options);
  REQUIRE(refused);REQUIRE(refused->error());REQUIRE_FALSE(refused->profile_writable());
  REQUIRE(TreeBytes(scratch.path)==before);
}
}  // namespace xe::filesystem::test
#endif

#if XE_PLATFORM_LINUX || XE_PLATFORM_MAC
namespace xe::filesystem::test {
TEST_CASE("Historical native B2 and explicit legacy formats reconstruct sealed recovery without new ownership",
          "[managed_volume_authority][format][recovery][legacy_b2]") {
  const bool primary = GENERATE(false, true);
  const int stop = GENERATE(0, 1, 2);
  VolumeScratch scratch;
  ManagedVolumeAuthority::Options options;
  options.source_id = primary ? "legacy-primary-format" : "legacy-b2-format";
  options.device_id = primary ? 1 : 3;
  options.state_parent_override = scratch.root;
  std::error_code error;
  if (primary) {
    REQUIRE_FALSE(scratch.root->Create("content", true));
    REQUIRE_FALSE(scratch.root->Create("public", true));
    REQUIRE_FALSE(scratch.root->Create("public/Content", true));
    options.content = scratch.root->Subroot("content", false, error);
    REQUIRE(options.content); REQUIRE_FALSE(error);
    options.preserve_legacy_layout = true;
    options.legacy_private_parent = scratch.root;
    options.legacy_public_root = scratch.root->Subroot("public", false, error);
    REQUIRE(options.legacy_public_root); REQUIRE_FALSE(error);
    options.legacy_recovery = SnapshotRecoveryOwner::Open(scratch.root);
    REQUIRE(options.legacy_recovery); REQUIRE_FALSE(options.legacy_recovery->error());
  } else {
    options.content = scratch.root;
  }
  auto original = ManagedVolumeAuthorityTestAccess::OpenLegacyProducer(options);
  REQUIRE(original); REQUIRE_FALSE(original->error()); REQUIRE(original->profile_writable());
  REQUIRE_FALSE(original->identity()->binding_required());
  const auto uuid = original->identity()->uuid();
  const auto epoch = original->identity()->content_epoch();
  const auto source_path = original->content()->path();
  const auto binding_path = ManagedVolumeAuthorityTestAccess::LegacyBindingPath(*original);
  const auto old_profile_path = original->intents()->root()->path();
  const auto profile_wire = BootstrapFile(old_profile_path/"owner.v1");
  const auto source_wire = BootstrapFile(source_path/ManagedDeviceIdentity::kMarker);
  REQUIRE_FALSE(profile_wire.empty()); REQUIRE_FALSE(source_wire.empty());
  std::vector<uint8_t> binding_wire;
  if (!primary) {
    REQUIRE_FALSE(binding_path.empty());
    binding_wire = BootstrapFile(binding_path/"owner.v1");
    REQUIRE(binding_wire.size() > 8);
    REQUIRE(std::string(binding_wire.begin(), binding_wire.begin()+8) == "XMVOLB2\n");
    REQUIRE_FALSE(std::filesystem::exists(binding_path/"bootstrap.pending.v1"));
  } else {
    REQUIRE(binding_path.empty());
  }
  struct stat old_profile{};
  REQUIRE(stat(old_profile_path.c_str(), &old_profile) == 0);
  std::ofstream(source_path/"before-format.bin") << "reset once";
  auto ticket = original->CaptureFormatHandoff();
  REQUIRE(ticket); REQUIRE_FALSE(ticket->error());
  ticket->ClosePublishedPublicView(); original.reset();
  REQUIRE_FALSE(ManagedVolumeAuthorityTestAccess::PrepareOnly(*ticket));
  REQUIRE(ticket->phase() == 1);
  if (stop >= 1) {
    const auto reset = ManagedVolumeAuthority::ExecuteFormat(*ticket);
    REQUIRE_FALSE(reset.error); REQUIRE(reset.complete);
    REQUIRE_FALSE(std::filesystem::exists(source_path/"before-format.bin"));
    std::ofstream(source_path/"after-reset.bin") << "must survive recovery";
  }
  if (stop == 2) {
    auto ready = ManagedVolumeAuthorityTestAccess::OwnersReadyOnly(*ticket);
    REQUIRE(ready); REQUIRE_FALSE(ready->error()); REQUIRE(ticket->phase() == 2);
    ready.reset();
  }
  ticket.reset();
  const auto interrupted = TreeBytes(scratch.path);
  auto ordinary = ManagedVolumeAuthority::Open(options);
  REQUIRE(ordinary); REQUIRE(ordinary->error()); REQUIRE_FALSE(ordinary->profile_writable());
  REQUIRE(TreeBytes(scratch.path) == interrupted);
  ordinary.reset();
  ticket = ManagedVolumeAuthority::InspectFormatRecovery(options);
  REQUIRE(ticket); REQUIRE_FALSE(ticket->error());
  REQUIRE(ticket->phase() == (stop == 2 ? 2 : 1));
  REQUIRE(TreeBytes(scratch.path) == interrupted);
  const auto observed = ManagedVolumeAuthorityTestAccess::ObservedFormatSource(*ticket);
  REQUIRE(observed);
  REQUIRE_FALSE(observed->GuestView(error));
  REQUIRE(error == std::errc::operation_in_progress);
  REQUIRE_FALSE(HostStorageRoot::AdmitGuestMount(
      std::vector<GuestHostBacking>{{source_path, true}}, error));
  REQUIRE(error == std::errc::operation_in_progress);
  REQUIRE(TreeBytes(scratch.path) == interrupted);
  auto current = ManagedVolumeAuthority::ResumeFormat(*ticket);
  REQUIRE(current); REQUIRE_FALSE(current->error()); REQUIRE(current->profile_writable());
  REQUIRE(current->identity()->uuid() == uuid);
  REQUIRE(current->identity()->content_epoch() == epoch+1);
  REQUIRE_FALSE(current->identity()->binding_required());
  REQUIRE_FALSE(current->identity()->CheckCurrent());
  REQUIRE_FALSE(std::filesystem::exists(source_path/"before-format.bin"));
  REQUIRE(std::filesystem::exists(source_path/"after-reset.bin") == bool(stop>=1));
  struct stat kept_profile{};
  REQUIRE(stat(old_profile_path.c_str(), &kept_profile) == 0);
  REQUIRE(kept_profile.st_dev == old_profile.st_dev);
  REQUIRE(kept_profile.st_ino == old_profile.st_ino);
  REQUIRE(BootstrapFile(old_profile_path/"owner.v1") == profile_wire);
  const auto completed_wire = BootstrapFile(source_path/ManagedDeviceIdentity::kMarker);
  REQUIRE(completed_wire.size() > source_wire.size());
  REQUIRE(std::equal(source_wire.begin(), source_wire.end(), completed_wire.begin()));
  if (!primary) {
    REQUIRE(BootstrapFile(binding_path/"owner.v1") == binding_wire);
    REQUIRE_FALSE(std::filesystem::exists(binding_path/"bootstrap.pending.v1"));
  }
  // A genuine current profile owner exists only because format created it.
  // Removing it cannot turn ordinary reopen/migration into fresh creation.
  const auto current_profile_path = current->intents()->root()->path();
  current.reset(); ticket.reset();
  std::filesystem::rename(current_profile_path, scratch.path/"preserved-current-profile");
  const auto absent = TreeBytes(scratch.path);
  ordinary = ManagedVolumeAuthority::Open(options);
  REQUIRE(ordinary); REQUIRE(ordinary->error()); REQUIRE_FALSE(ordinary->profile_writable());
  REQUIRE_FALSE(std::filesystem::exists(current_profile_path));
  REQUIRE(TreeBytes(scratch.path) == absent);
}
}  // namespace xe::filesystem::test
#endif

#if XE_PLATFORM_LINUX || XE_PLATFORM_MAC
namespace xe::filesystem::test {
TEST_CASE("A failed historical profile proof cannot resume through an unsealed format operation",
          "[managed_volume_authority][format][recovery][legacy_b2]") {
  VolumeScratch scratch;
  ManagedVolumeAuthority::Options options;
  options.content=scratch.root; options.state_parent_override=scratch.root;
  options.source_id="legacy-failed-proof"; options.device_id=3;
  auto original=ManagedVolumeAuthorityTestAccess::OpenLegacyProducer(options);
  REQUIRE(original); REQUIRE_FALSE(original->error()); REQUIRE(original->profile_writable());
  REQUIRE_FALSE(original->identity()->binding_required());
  const auto uuid=original->identity()->uuid();
  const auto epoch=original->identity()->content_epoch();
  const auto old_profile=original->intents()->root()->path();
  std::ofstream(scratch.path/"before-format.bin")<<"must survive failed inspection";
  auto ticket=original->CaptureFormatHandoff();
  REQUIRE(ticket); REQUIRE_FALSE(ticket->error());
  ticket->ClosePublishedPublicView(); original.reset();
  REQUIRE_FALSE(ManagedVolumeAuthorityTestAccess::PrepareOnly(*ticket));
  REQUIRE(ticket->phase()==1); ticket.reset();
  // Move the genuine original component intact. No fake metadata is written,
  // and invalid/missing profile evidence never grants permission to reset data.
  const auto preserved=scratch.path/"preserved-old-profile";
  std::filesystem::rename(old_profile,preserved);
  REQUIRE_FALSE(scratch.root->FlushDirectory());
  const auto before=TreeBytes(scratch.path);
  ticket=ManagedVolumeAuthority::InspectFormatRecovery(options);
  REQUIRE(ticket); REQUIRE(ticket->error()==std::errc::no_such_file_or_directory);
  REQUIRE(TreeBytes(scratch.path)==before);
  auto refused=ManagedVolumeAuthority::ResumeFormat(*ticket);
  REQUIRE(refused); REQUIRE(refused->error()==std::errc::operation_not_permitted);
  REQUIRE_FALSE(refused->profile_writable());
  REQUIRE(TreeBytes(scratch.path)==before);
  REQUIRE(std::filesystem::is_regular_file(scratch.path/"before-format.bin"));
  refused.reset(); ticket.reset();
  // Restore that same native component and perform a NEW full inspection.
  std::filesystem::rename(preserved,old_profile);
  REQUIRE_FALSE(scratch.root->FlushDirectory());
  ticket=ManagedVolumeAuthority::InspectFormatRecovery(options);
  REQUIRE(ticket); REQUIRE_FALSE(ticket->error());
  auto recovered=ManagedVolumeAuthority::ResumeFormat(*ticket);
  REQUIRE(recovered); REQUIRE_FALSE(recovered->error()); REQUIRE(recovered->profile_writable());
  REQUIRE(recovered->identity()->uuid()==uuid);
  REQUIRE(recovered->identity()->content_epoch()==epoch+1);
  REQUIRE_FALSE(std::filesystem::exists(scratch.path/"before-format.bin"));
}
}  // namespace xe::filesystem::test
#endif
