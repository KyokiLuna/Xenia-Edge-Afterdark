/** Xenia : Xbox 360 Emulator Research Project; BSD license, see LICENSE. */
#include "xenia/base/managed_volume_authority.h"
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
      REQUIRE(found != before.end()); REQUIRE(found->second == row);
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
    REQUIRE(added == content_purpose);
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
  REQUIRE_FALSE(owner->content()->has_guest_projection());
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
  // The real first transaction owns marker+intent+done (3 actual files).
  // Lower ONLY the private admission budget to4: a one-slot check would allow
  // mutation, while the required two-slot rule must refuse with no new files.
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
TEST_CASE("Interrupted unprepared initial member is retained privately and never adopted",
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
  REQUIRE(owner->Recover()); REQUIRE_FALSE(owner->ready());
  REQUIRE(std::filesystem::exists(staged->path()/"Account"));
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
  REQUIRE_FALSE(error); REQUIRE(encoded.size()>200); REQUIRE(encoded[11]==3);
  content_mutation_wire::Record decoded;
  REQUIRE(content_mutation_wire::Decode(encoded,decoded,error)); REQUIRE_FALSE(error);
  REQUIRE(decoded.initial_tree==actual.initial_tree); REQUIRE(decoded.intended==metadata);
  auto invalid=actual; invalid.initial_tree.front().entry.file=invalid.stage_file;
  REQUIRE(content_mutation_wire::Encode(invalid,error).empty()); REQUIRE(error);
  encoded.back()^=1; decoded.nonce="unchanged";
  REQUIRE_FALSE(content_mutation_wire::Decode(encoded,decoded,error)); REQUIRE(error);
  REQUIRE(decoded.nonce=="unchanged");
  REQUIRE_FALSE(std::filesystem::exists(scratch.path/kInitialPackage));
}
}  // namespace xe::filesystem::test
#endif
