/** Xenia : Xbox 360 Emulator Research Project; BSD license, see LICENSE. */
#include "xenia/kernel/xam/xdbf/gpd_durable_intent.h"
#include <algorithm>
#include <cstdio>
#include <set>
#include "xenia/base/managed_volume_authority.h"
#include "xenia/base/testing/snapshot_fixture.h"
#include "xenia/kernel/xam/xdbf/gpd_info.h"
#include "xenia/vfs/devices/host_path_device.h"
#include "xenia/vfs/virtual_file_system.h"
#if XE_PLATFORM_LINUX
namespace xe::filesystem {
// Test-only negative-I/O seam. Real CAS admission, unlink, and parent fsync
// always run. The injected post-effect error cannot manufacture a token or
// success from an ordinary read. No Kernel, provider callback, or fake fd.
class HostStorageSnapshotTestAccess final {
 public:
  static HostStorageRoot::ReplaceResult DeleteWithReportedFlushError(
      const HostStorageRoot& root, const HostStorageSnapshotToken& expected,
      std::shared_ptr<const HostStorageSnapshotToken> recovery) {
    return root.CompareAndDeleteSnapshotInternal(
        expected, std::move(recovery),
        std::make_error_code(std::errc::io_error));
  }
};
}  // namespace xe::filesystem
namespace xe::kernel::xam::detail {
// The exact production-used fixed-cap admission policy, not a parallel mirror.
std::error_code AdmitGpdIntentCompletion(size_t occupied, size_t unfinished);
}  // namespace xe::kernel::xam::detail
namespace xe::kernel::xam::test {
namespace {
constexpr uint64_t kXuid = 0xE000000000000001;
constexpr uint32_t kDashboard = 0xFFFE07D1, kTitle = 0x12345678;
std::string Leaf(uint32_t title) {
  char p[13];
  std::snprintf(p, sizeof(p), "%08X.gpd", title);
  return p;
}
std::vector<uint8_t> Wire(uint32_t title, std::u16string text) {
  GpdInfo model(title);
  model.AddString(0x100, std::move(text));
  std::vector<uint8_t> wire;
  REQUIRE_FALSE(model.SerializeChecked(wire, GpdSnapshotStore::kMaximumBytes));
  REQUIRE_FALSE(ValidateGpdSnapshot(wire));
  return wire;
}
struct Fixture {
  filesystem::test::SnapshotScratch scratch;
  std::shared_ptr<filesystem::HostStorageRoot> parent, source, profile;
  std::shared_ptr<const filesystem::ProfileIntentOwner> owner;
  std::shared_ptr<const filesystem::ManagedDeviceIdentity> identity;
  std::shared_ptr<const filesystem::SnapshotRecoveryOwner> recovery;
  GpdIntentBinding binding;
  const std::vector<uint8_t> old_dash = Wire(kDashboard, u"old dashboard");
  const std::vector<uint8_t> old_title = Wire(kTitle, u"old title");
  const std::vector<uint8_t> next_dash = Wire(kDashboard, u"next dashboard");
  const std::vector<uint8_t> next_title = Wire(kTitle, u"next title");
  explicit Fixture(std::shared_ptr<const filesystem::ManagedVolumeAuthority> volume = {}) {
    std::error_code error;
    if (volume) {
      REQUIRE_FALSE(volume->error());
      parent = std::const_pointer_cast<filesystem::HostStorageRoot>(volume->intents()->private_parent());
      source = volume->content();
      identity = volume->identity();
      owner = volume->intents();
      recovery = volume->recovery();
    } else {
      parent = scratch.Root();
      REQUIRE_FALSE(parent->Create("content", true));
      source = parent->Subroot("content", false, error);
      REQUIRE(source);
      identity = filesystem::ManagedDeviceIdentity::Open(
          source, "managed-device-A", 1, error);
      REQUIRE(identity);
      REQUIRE_FALSE(error);
      owner = filesystem::ProfileIntentOwner::Open(parent, identity);
      recovery = filesystem::SnapshotRecoveryOwner::Open(parent);
    }
    REQUIRE_FALSE(owner->error());
    REQUIRE_FALSE(recovery->error());
    REQUIRE_FALSE(source->Create("profile", true));
    auto raw = source->Subroot("profile", false, error);
    REQUIRE(raw);
    profile = raw->GuestView(error);
    REQUIRE(profile);
    REQUIRE_FALSE(error);
    binding = {"managed-device-A",
               1,
               kXuid,
               1,
               "profile",
               {identity->uuid(), identity->content_epoch(),
                identity->directory_file_id()}};
    REQUIRE_FALSE(profile->WriteNewFile(Leaf(kDashboard), old_dash).error);
    REQUIRE_FALSE(profile->WriteNewFile(Leaf(kTitle), old_title).error);
  }
  GpdSnapshotLoad Load(uint32_t title) {
    auto load = GpdSnapshotStore::Load({kXuid, 1, title}, profile);
    REQUIRE_FALSE(load.error);
    return load;
  }
  std::vector<GpdIntentMutation> Pair() {
    return {{GpdIntentAction::kReplace, Load(kDashboard), next_dash},
            {GpdIntentAction::kReplace, Load(kTitle), next_title}};
  }
  auto Prepare(std::error_code& error, std::filesystem::path& path) {
    return GpdDurableIntentStore::Prepare(owner, recovery, binding, Pair(),
                                          error, path);
  }
  std::vector<uint8_t> Bytes(uint32_t title) {
    std::vector<uint8_t> bytes;
    REQUIRE_FALSE(profile->ReadFile(Leaf(title), 1 << 20, bytes));
    return bytes;
  }
  auto Recover(const std::filesystem::path& path, std::error_code& error) {
    auto fresh_binding = binding;
    fresh_binding.source_generation = 17;
    return GpdDurableIntentStore::Recover(owner, recovery, path, fresh_binding,
                                          profile, error);
  }
};
}  // namespace
TEST_CASE(
    "Actual whole Content HostPathDevice mounts beside external private "
    "journals",
    "[gpd_durable_intent]") {
  Fixture f;
  vfs::VirtualFileSystem fs;
  auto content = std::make_unique<vfs::HostPathDevice>(
      "IntentContent:", f.source->path(), false, false, "FATX", f.source,
      f.source);
  REQUIRE(content->Initialize());
  REQUIRE(fs.RegisterDevice(std::move(content)));
  REQUIRE(fs.ResolvePath("IntentContent:\\profile\\FFFE07D1.gpd"));
  std::error_code error;
  REQUIRE_FALSE(f.owner->root()->GuestView(error));
  REQUIRE(error == std::errc::permission_denied);
}
TEST_CASE(
    "Fixed intent capacity reserves all pending completions without relaxing "
    "cap",
    "[gpd_durable_intent]") {
  REQUIRE_FALSE(detail::AdmitGpdIntentCompletion(1, 0));
  REQUIRE_FALSE(detail::AdmitGpdIntentCompletion(8190, 0));
  REQUIRE(detail::AdmitGpdIntentCompletion(8191, 0) ==
          std::errc::no_space_on_device);
  // Many pending intents occupy only4096 leaves, but all4095 owed done files
  // must remain reserved. Looking only at two currently free slots is wrong.
  REQUIRE(detail::AdmitGpdIntentCompletion(4096, 4095) ==
          std::errc::no_space_on_device);
  REQUIRE_FALSE(detail::AdmitGpdIntentCompletion(4095, 4094));
  REQUIRE(detail::AdmitGpdIntentCompletion(SIZE_MAX, SIZE_MAX) ==
          std::errc::no_space_on_device);
}
TEST_CASE("Durable intent is complete before either real GPD changes",
          "[gpd_durable_intent]") {
  Fixture f;
  std::error_code error;
  std::filesystem::path journal;
  auto intent = f.Prepare(error, journal);
  REQUIRE(intent);
  REQUIRE_FALSE(error);
  REQUIRE(f.Bytes(kDashboard) == f.old_dash);
  REQUIRE(f.Bytes(kTitle) == f.old_title);
  std::vector<uint8_t> raw;
  REQUIRE_FALSE(f.owner->root()->ReadFile(journal, 1 << 20, raw));
  REQUIRE(std::search(raw.begin(), raw.end(), f.old_dash.begin(),
                      f.old_dash.end()) != raw.end());
  REQUIRE(std::search(raw.begin(), raw.end(), f.next_title.begin(),
                      f.next_title.end()) != raw.end());
  auto rows = GpdDurableIntentStore::Scan(*f.owner, error);
  REQUIRE_FALSE(error);
  REQUIRE(rows.size() == 1);
  REQUIRE_FALSE(rows[0].done_record_valid);
  auto result = intent->Advance();
  REQUIRE_FALSE(result.error);
  REQUIRE(result.intent_durable);
  REQUIRE(result.all_durable_sequentially_observed);
  REQUIRE(result.done_durable);
  REQUIRE(result.files[0].publication.published);
  REQUIRE(result.files[1].publication.durable);
  REQUIRE(f.Bytes(kDashboard) == f.next_dash);
  REQUIRE(f.Bytes(kTitle) == f.next_title);
}

TEST_CASE("Actual durable GPD intent publishes beside the mounted Account reservation",
          "[gpd_durable_intent][namespace][account]") {
  Fixture f;
  const std::array<uint8_t, 7> account_bytes{0x41, 0, 0x82, 0x13, 0xF4, 5, 6};
  REQUIRE_FALSE(f.profile->WriteNewFile("Account", account_bytes).error);
  const auto raw_profile = f.profile;
  std::error_code error;
  auto account = f.profile->CaptureSnapshot("Account", 4096, error);
  REQUIRE(account);
  f.profile = f.profile->ReserveSnapshotFile(account, error);
  REQUIRE(f.profile);
  REQUIRE_FALSE(error);
  std::filesystem::path journal;
  auto intent = f.Prepare(error, journal);
  REQUIRE(intent);
  REQUIRE_FALSE(error);
  REQUIRE_FALSE(journal.empty());
  REQUIRE(f.Bytes(kDashboard) == f.old_dash);
  REQUIRE(f.Bytes(kTitle) == f.old_title);
  const auto result = intent->Advance();
  REQUIRE_FALSE(result.error);
  REQUIRE(result.intent_durable);
  REQUIRE(result.all_durable_sequentially_observed);
  REQUIRE(result.done_durable);
  REQUIRE(f.Bytes(kDashboard) == f.next_dash);
  REQUIRE(f.Bytes(kTitle) == f.next_title);
  std::vector<uint8_t> bytes;
  REQUIRE_FALSE(f.profile->ReadFile("Account", 4096, bytes));
  REQUIRE(std::equal(bytes.begin(), bytes.end(), account_bytes.begin(),
                     account_bytes.end()));
  auto denied = raw_profile->OpenFile("Account", filesystem::FileAccess::kFileWriteData, error);
  REQUIRE_FALSE(denied);
  REQUIRE(error == std::errc::device_or_resource_busy);
  // Actual durable intent reconstruction must see the same completion while
  // Account remains reserved, rather than treating a callback as durability.
  const auto rows = GpdDurableIntentStore::Scan(*f.owner, error);
  REQUIRE_FALSE(error);
  REQUIRE(rows.size() == 1);
  REQUIRE(rows[0].done_record_valid);
}

TEST_CASE("Restart before first publish replays exact complete intent",
          "[gpd_durable_intent]") {
  Fixture f;
  std::error_code error;
  std::filesystem::path journal;
  auto intent = f.Prepare(error, journal);
  REQUIRE(intent);
  intent.reset();
  // Reopen the actual fixed-child owner and reacquire its real directory lock;
  // no live CAS token or old in-memory intent object survives this boundary.
  f.owner.reset();
  f.owner = filesystem::ProfileIntentOwner::Open(f.parent, f.identity);
  REQUIRE_FALSE(f.owner->error());
  auto recovered = f.Recover(journal, error);
  REQUIRE(recovered);
  REQUIRE_FALSE(error);
  auto result = recovered->Advance();
  REQUIRE_FALSE(result.error);
  REQUIRE(result.done_durable);
  REQUIRE(result.files[0].publication.published);
  REQUIRE(f.Bytes(kTitle) == f.next_title);
}
TEST_CASE("Restart partial publication observes intended without inventing CAS",
          "[gpd_durable_intent]") {
  Fixture f;
  std::error_code error;
  std::filesystem::path journal;
  auto intent = f.Prepare(error, journal);
  REQUIRE(intent);
  auto first =
      GpdSnapshotStore::Publish(*f.recovery, f.Load(kDashboard), f.next_dash);
  REQUIRE_FALSE(first.storage.error);
  REQUIRE(first.storage.published);
  intent.reset();
  auto recovered = f.Recover(journal, error);
  REQUIRE(recovered);
  auto result = recovered->Advance();
  REQUIRE_FALSE(result.error);
  REQUIRE(result.files[0].observation.matched);
  REQUIRE(result.files[0].observation.durable);
  REQUIRE_FALSE(result.files[0].publication.published);
  REQUIRE_FALSE(result.files[0].retry.published);
  REQUIRE(result.files[1].publication.published);
  REQUIRE(result.done_durable);
  auto ordinary = f.Load(kDashboard);
  REQUIRE(f.profile->RetrySnapshotDurability(ordinary.token).error ==
          std::errc::invalid_argument);
}
TEST_CASE(
    "Historical done receipt prevents replay of later normal profile data",
    "[gpd_durable_intent]") {
  Fixture f;
  std::error_code error;
  std::filesystem::path journal;
  auto intent = f.Prepare(error, journal);
  REQUIRE(intent);
  REQUIRE(intent->Advance().done_durable);
  intent.reset();
  auto changed = Wire(kDashboard, u"subsequent ordinary update");
  auto write =
      GpdSnapshotStore::Publish(*f.recovery, f.Load(kDashboard), changed);
  REQUIRE_FALSE(write.storage.error);
  auto recovered = f.Recover(journal, error);
  REQUIRE(recovered);
  auto result = recovered->Advance();
  REQUIRE_FALSE(result.error);
  REQUIRE(result.historically_complete);
  REQUIRE(result.done_durable);
  REQUIRE_FALSE(result.all_durable_sequentially_observed);
  REQUIRE(f.Bytes(kDashboard) == changed);
}
TEST_CASE("Second file conflict blocks known unnecessary first publication",
          "[gpd_durable_intent]") {
  Fixture f;
  std::error_code error;
  std::filesystem::path journal;
  auto intent = f.Prepare(error, journal);
  REQUIRE(intent);
  auto changed = Wire(kTitle, u"competing title update");
  REQUIRE_FALSE(GpdSnapshotStore::Publish(*f.recovery, f.Load(kTitle), changed)
                    .storage.error);
  auto result = intent->Advance();
  REQUIRE(result.error == std::errc::state_not_recoverable);
  REQUIRE_FALSE(result.files[0].publication.published);
  REQUIRE_FALSE(result.done_durable);
  REQUIRE(f.Bytes(kDashboard) == f.old_dash);
  REQUIRE(f.Bytes(kTitle) == changed);
}
TEST_CASE("Equal before bytes on a foreign replacement inode are a conflict",
          "[gpd_durable_intent]") {
  Fixture f;
  std::error_code error;
  std::filesystem::path journal;
  auto intent = f.Prepare(error, journal);
  REQUIRE(intent);
  REQUIRE_FALSE(f.profile->Rename(Leaf(kTitle), "retained-old", false));
  REQUIRE_FALSE(f.profile->WriteNewFile(Leaf(kTitle), f.old_title).error);
  auto result = intent->Advance();
  REQUIRE(result.error == std::errc::state_not_recoverable);
  REQUIRE_FALSE(result.done_durable);
  REQUIRE(f.Bytes(kDashboard) == f.old_dash);
}
TEST_CASE(
    "An actual open member lease causes partial result and restart recovery",
    "[gpd_durable_intent]") {
  Fixture f;
  std::error_code error;
  std::filesystem::path journal;
  auto intent = f.Prepare(error, journal);
  REQUIRE(intent);
  auto opened = f.profile->OpenFile(
      Leaf(kTitle), filesystem::FileAccess::kGenericRead, error);
  REQUIRE(opened);
  auto result = intent->Advance();
  REQUIRE(result.error == std::errc::device_or_resource_busy);
  REQUIRE(result.files[0].publication.published);
  REQUIRE_FALSE(result.files[1].publication.published);
  REQUIRE_FALSE(result.done_durable);
  REQUIRE(f.Bytes(kDashboard) == f.next_dash);
  opened.reset();
  intent.reset();
  auto recovered = f.Recover(journal, error);
  REQUIRE(recovered);
  auto completed = recovered->Advance();
  REQUIRE_FALSE(completed.error);
  REQUIRE(completed.files[0].observation.durable);
  REQUIRE(completed.done_durable);
}
TEST_CASE(
    "Typed create uses observed absence and delete rejects nonempty intended "
    "data",
    "[gpd_durable_intent]") {
  Fixture f;
  std::error_code error;
  std::filesystem::path journal;
  constexpr uint32_t other = 0x87654321;
  auto missing = f.Load(other);
  REQUIRE(missing.missing());
  auto wire = Wire(other, u"explicitly created title");
  auto created = GpdDurableIntentStore::Prepare(
      f.owner, f.recovery, f.binding,
      {{GpdIntentAction::kCreate, missing, wire}}, error, journal);
  REQUIRE(created);
  REQUIRE_FALSE(error);
  auto result = created->Advance();
  REQUIRE_FALSE(result.error);
  REQUIRE(result.files.size() == 1);
  REQUIRE(result.files[0].publication.published);
  REQUIRE_FALSE(result.files[0].publication.recovery_created);
  REQUIRE(result.done_durable);
  created.reset();
  journal.clear();
  auto denied = GpdDurableIntentStore::Prepare(
      f.owner, f.recovery, f.binding,
      {{GpdIntentAction::kDelete, f.Load(kTitle), f.next_title}}, error,
      journal);
  REQUIRE_FALSE(denied);
  REQUIRE(error == std::errc::bad_message);
  REQUIRE(journal.empty());
  REQUIRE(f.Bytes(kTitle) == f.old_title);
}
TEST_CASE(
    "Unknown or partial intent and orphan done remain preserved and block",
    "[gpd_durable_intent]") {
  Fixture f;
  const std::array<uint8_t, 3> partial{1, 2, 3};
  constexpr auto leaf = "intent-0123456789abcdef0123456789abcdef.bin";
  REQUIRE_FALSE(f.owner->root()->WriteNewFile(leaf, partial).error);
  std::error_code error;
  auto rows = GpdDurableIntentStore::Scan(*f.owner, error);
  REQUIRE(error == std::errc::bad_message);
  REQUIRE(rows.size() == 1);
  REQUIRE(rows[0].error);
  std::filesystem::path journal;
  REQUIRE_FALSE(f.Prepare(error, journal));
  REQUIRE(error);
  REQUIRE(journal.empty());
  REQUIRE_FALSE(
      f.owner->root()
          ->WriteNewFile("done-abcdefabcdefabcdefabcdefabcdefab.bin", partial)
          .error);
  rows = GpdDurableIntentStore::Scan(*f.owner, error);
  REQUIRE(error);
  REQUIRE(rows.size() == 2);
  REQUIRE(f.Bytes(kTitle) == f.old_title);
}
TEST_CASE(
    "Foreign source binding retired view and competing target owner refuse",
    "[gpd_durable_intent]") {
  Fixture f;
  std::error_code error;
  std::filesystem::path journal;
  auto intent = f.Prepare(error, journal);
  REQUIRE(intent);
  std::filesystem::path another;
  REQUIRE_FALSE(f.Prepare(error, another));
  REQUIRE(error == std::errc::device_or_resource_busy);
  intent.reset();
  std::vector<uint8_t> journal_before;
  REQUIRE_FALSE(f.owner->root()->ReadFile(journal, 1 << 20, journal_before));
  auto malformed = f.binding;
  malformed.source_id.clear();
  std::filesystem::path rejected;
  REQUIRE_FALSE(GpdDurableIntentStore::Prepare(
      f.owner, f.recovery, malformed, f.Pair(), error, rejected));
  REQUIRE(error == std::errc::invalid_argument);
  REQUIRE(rejected.empty());
  REQUIRE_FALSE(GpdDurableIntentStore::Recover(
      f.owner, f.recovery, journal, malformed, f.profile, error));
  REQUIRE(error == std::errc::invalid_argument);
  auto foreign = f.binding;
  foreign.source_id = "foreign-source";
  REQUIRE_FALSE(GpdDurableIntentStore::Prepare(
      f.owner, f.recovery, foreign, f.Pair(), error, rejected));
  REQUIRE(error == std::errc::state_not_recoverable);
  REQUIRE(rejected.empty());
  REQUIRE_FALSE(GpdDurableIntentStore::Recover(f.owner, f.recovery, journal,
                                               foreign, f.profile, error));
  REQUIRE(error == std::errc::state_not_recoverable);
  std::vector<uint8_t> journal_after;
  REQUIRE_FALSE(f.owner->root()->ReadFile(journal, 1 << 20, journal_after));
  REQUIRE(journal_after == journal_before);
  REQUIRE(f.Bytes(kDashboard) == f.old_dash);
  REQUIRE(f.Bytes(kTitle) == f.old_title);
  auto recovered = f.Recover(journal, error);
  REQUIRE(recovered);
  f.profile->Retire();
  auto result = recovered->Advance();
  REQUIRE(result.error == std::errc::no_such_device);
  REQUIRE_FALSE(result.done_durable);
}
TEST_CASE(
    "A partial completion marker is retained without replay or false success",
    "[gpd_durable_intent]") {
  Fixture f;
  std::error_code error;
  std::filesystem::path journal;
  auto intent = f.Prepare(error, journal);
  REQUIRE(intent);
  auto completed = intent->Advance();
  REQUIRE(completed.done_durable);
  // Damage only our scratch file to model an incomplete persisted receipt.
  // No fixture pretends this is an actual injected fsync result.
  REQUIRE(truncate((f.owner->root()->path() / completed.done).c_str(), 3) == 0);
  intent.reset();
  auto rows = GpdDurableIntentStore::Scan(*f.owner, error);
  REQUIRE(error == std::errc::bad_message);
  REQUIRE(rows[0].error);
  REQUIRE_FALSE(f.Recover(journal, error));
  REQUIRE(error);
  REQUIRE(f.Bytes(kDashboard) == f.next_dash);
  REQUIRE(f.Bytes(kTitle) == f.next_title);
  REQUIRE(std::filesystem::file_size(f.owner->root()->path() /
                                     completed.done) == 3);
}
TEST_CASE("Journal checksum corruption blocks before any target operation",
          "[gpd_durable_intent]") {
  Fixture f;
  std::error_code error;
  std::filesystem::path journal;
  auto intent = f.Prepare(error, journal);
  REQUIRE(intent);
  intent.reset();
  const auto native = f.owner->root()->path() / journal;
  const auto length = std::filesystem::file_size(native);
  REQUIRE(truncate(native.c_str(), off_t(length - 1)) == 0);
  REQUIRE_FALSE(f.Recover(journal, error));
  REQUIRE(error == std::errc::bad_message);
  REQUIRE(f.Bytes(kDashboard) == f.old_dash);
  REQUIRE(f.Bytes(kTitle) == f.old_title);
  REQUIRE(std::filesystem::file_size(native) == length - 1);
}
TEST_CASE("Journal admission permission failure cannot touch either original",
          "[gpd_durable_intent]") {
  Fixture f;
  std::error_code error;
  std::filesystem::path journal;
  REQUIRE(chmod(f.owner->root()->path().c_str(), 0500) == 0);
  auto intent = f.Prepare(error, journal);
  const auto observed = error;
  REQUIRE(chmod(f.owner->root()->path().c_str(), 0700) == 0);
  if (geteuid() == 0 || intent) {
    WARN("Actual EACCES unavailable under current host privilege");
  } else {
    REQUIRE(observed == std::errc::permission_denied);
  }
  REQUIRE(f.Bytes(kDashboard) == f.old_dash);
  REQUIRE(f.Bytes(kTitle) == f.old_title);
}

TEST_CASE(
    "Durable mixed replacement and deletion retain before bytes and typed "
    "absent receipt",
    "[gpd_durable_intent][delete]") {
  Fixture f;
  std::error_code error;
  std::filesystem::path journal;
  auto intent = GpdDurableIntentStore::Prepare(
      f.owner, f.recovery, f.binding,
      {{GpdIntentAction::kReplace, f.Load(kDashboard), f.next_dash},
       {GpdIntentAction::kDelete, f.Load(kTitle), {}}},
      error, journal);
  REQUIRE(intent);
  REQUIRE_FALSE(error);
  REQUIRE(f.Bytes(kDashboard) == f.old_dash);
  REQUIRE(f.Bytes(kTitle) == f.old_title);
  std::vector<uint8_t> intent_bytes;
  REQUIRE_FALSE(f.owner->root()->ReadFile(journal, 1 << 20, intent_bytes));
  REQUIRE(std::search(intent_bytes.begin(), intent_bytes.end(),
                      f.old_title.begin(),
                      f.old_title.end()) != intent_bytes.end());
  auto result = intent->Advance();
  REQUIRE_FALSE(result.error);
  REQUIRE(result.intent_durable);
  REQUIRE(result.done_durable);
  REQUIRE(result.files[0].publication.kind ==
          filesystem::SnapshotMutationKind::kWrite);
  const auto& deleted = result.files[1].publication;
  REQUIRE(deleted.kind == filesystem::SnapshotMutationKind::kDelete);
  REQUIRE(deleted.published);
  REQUIRE(deleted.durable);
  REQUIRE(deleted.current);
  REQUIRE_FALSE(deleted.current->exists());
  REQUIRE(deleted.stage.empty());
  REQUIRE_FALSE(deleted.stage_created);
  REQUIRE(f.Load(kTitle).missing());
  REQUIRE(f.Bytes(kDashboard) == f.next_dash);
  std::vector<uint8_t> saved;
  REQUIRE_FALSE(f.recovery->root()->ReadFile(deleted.recovery, 1 << 20, saved));
  REQUIRE(saved == f.old_title);
}
TEST_CASE(
    "Same mount recovery observes deleted case folded GPD without inventing "
    "publication",
    "[gpd_durable_intent][delete][recovery]") {
  Fixture f;
  std::error_code error;
  std::filesystem::path journal;
  REQUIRE_FALSE(f.profile->Rename(Leaf(kTitle), "12345678.GPD", false));
  auto intent = GpdDurableIntentStore::Prepare(
      f.owner, f.recovery, f.binding,
      {{GpdIntentAction::kReplace, f.Load(kDashboard), f.next_dash},
       {GpdIntentAction::kDelete, f.Load(kTitle), {}}},
      error, journal);
  REQUIRE(intent);
  REQUIRE_FALSE(error);
  {
    const auto deleted = GpdSnapshotStore::Delete(*f.recovery, f.Load(kTitle));
    REQUIRE_FALSE(deleted.storage.error);
    REQUIRE(deleted.storage.published);
    REQUIRE(deleted.storage.current);
    REQUIRE_FALSE(deleted.storage.current->exists());
  }
  intent.reset();
  f.owner.reset();
  f.recovery.reset();
  f.owner = filesystem::ProfileIntentOwner::Open(f.parent, f.identity);
  f.recovery = filesystem::SnapshotRecoveryOwner::Open(f.parent);
  REQUIRE_FALSE(f.owner->error());
  REQUIRE_FALSE(f.recovery->error());
  auto restored = f.Recover(journal, error);
  REQUIRE(restored);
  REQUIRE_FALSE(error);
  auto result = restored->Advance();
  REQUIRE_FALSE(result.error);
  REQUIRE(result.done_durable);
  REQUIRE(result.files[1].state == GpdIntentFileReceipt::State::kIntended);
  REQUIRE(result.files[1].observation.matched);
  REQUIRE(result.files[1].observation.durable);
  REQUIRE_FALSE(result.files[1].publication.published);
  REQUIRE_FALSE(result.files[1].retry.published);
  REQUIRE(f.Load(kTitle).missing());
  REQUIRE(f.Bytes(kDashboard) == f.next_dash);
  auto absent = f.Load(kTitle);
  REQUIRE(f.profile->RetrySnapshotDurability(absent.token).error ==
          std::errc::invalid_argument);
}
TEST_CASE(
    "Journal delete recovery preserves recreated third state and unmodified "
    "first member",
    "[gpd_durable_intent][delete][conflict]") {
  Fixture f;
  std::error_code error;
  std::filesystem::path journal;
  auto intent = GpdDurableIntentStore::Prepare(
      f.owner, f.recovery, f.binding,
      {{GpdIntentAction::kReplace, f.Load(kDashboard), f.next_dash},
       {GpdIntentAction::kDelete, f.Load(kTitle), {}}},
      error, journal);
  REQUIRE(intent);
  const auto deleted = GpdSnapshotStore::Delete(*f.recovery, f.Load(kTitle));
  REQUIRE_FALSE(deleted.storage.error);
  REQUIRE(deleted.storage.published);
  const auto foreign = Wire(kTitle, u"foreign valid recreation");
  REQUIRE_FALSE(f.profile->WriteNewFile(Leaf(kTitle), foreign).error);
  intent.reset();
  auto restored = f.Recover(journal, error);
  REQUIRE(restored);
  auto result = restored->Advance();
  REQUIRE(result.error == std::errc::state_not_recoverable);
  REQUIRE(result.files[1].state == GpdIntentFileReceipt::State::kConflict);
  REQUIRE_FALSE(result.files[0].publication.published);
  REQUIRE(f.Bytes(kDashboard) == f.old_dash);
  REQUIRE(f.Bytes(kTitle) == foreign);
  REQUIRE_FALSE(std::filesystem::exists(f.owner->root()->path() / result.done));
}
TEST_CASE(
    "Pending mixed intent retries a busy actual deletion after prior member "
    "published",
    "[gpd_durable_intent][delete][lifetime]") {
  Fixture f;
  std::error_code error;
  std::filesystem::path journal;
  auto intent = GpdDurableIntentStore::Prepare(
      f.owner, f.recovery, f.binding,
      {{GpdIntentAction::kReplace, f.Load(kDashboard), f.next_dash},
       {GpdIntentAction::kDelete, f.Load(kTitle), {}}},
      error, journal);
  REQUIRE(intent);
  auto held = f.profile->OpenFile(Leaf(kTitle),
                                  filesystem::FileAccess::kGenericRead, error);
  REQUIRE(held);
  REQUIRE_FALSE(error);
  auto partial = intent->Advance();
  REQUIRE(partial.error == std::errc::device_or_resource_busy);
  REQUIRE(partial.files[0].publication.published);
  REQUIRE_FALSE(partial.files[1].publication.published);
  REQUIRE(f.Bytes(kDashboard) == f.next_dash);
  REQUIRE(f.Bytes(kTitle) == f.old_title);
  held.reset();
  auto completed = intent->Advance();
  REQUIRE_FALSE(completed.error);
  REQUIRE(completed.done_durable);
  REQUIRE(completed.files[0].retry.published);
  REQUIRE(completed.files[0].retry.durable);
  REQUIRE(completed.files[1].publication.published);
  REQUIRE(completed.files[1].publication.kind ==
          filesystem::SnapshotMutationKind::kDelete);
  REQUIRE(f.Load(kTitle).missing());
}
TEST_CASE(
    "Journal authority reconciles an actual deleted GPD after reported post "
    "effect I O failure",
    "[gpd_durable_intent][delete][durability]") {
  Fixture f;
  std::error_code error;
  std::filesystem::path journal;
  auto loaded = f.Load(kTitle);
  auto intent = GpdDurableIntentStore::Prepare(
      f.owner, f.recovery, f.binding, {{GpdIntentAction::kDelete, loaded, {}}},
      error, journal);
  REQUIRE(intent);
  constexpr auto leaf = "snapshot-abcdef0123456789abcdef0123456789.bin";
  REQUIRE_FALSE(
      f.recovery->root()->WriteNewFile(leaf, loaded.token->bytes()).error);
  auto backup = f.recovery->root()->CaptureSnapshot(
      leaf, loaded.token->bytes().size(), error);
  REQUIRE(backup);
  REQUIRE_FALSE(error);
  auto deleted =
      filesystem::HostStorageSnapshotTestAccess::DeleteWithReportedFlushError(
          *f.profile, *loaded.token, backup);
  REQUIRE(deleted.error == std::errc::io_error);
  REQUIRE(deleted.published);
  REQUIRE_FALSE(deleted.durable);
  REQUIRE(deleted.current);
  auto ordinary = f.Load(kTitle);
  REQUIRE(ordinary.missing());
  REQUIRE(f.profile->RetrySnapshotDurability(ordinary.token).error ==
          std::errc::invalid_argument);
  intent.reset();
  auto restored = f.Recover(journal, error);
  REQUIRE(restored);
  auto result = restored->Advance();
  REQUIRE_FALSE(result.error);
  REQUIRE(result.done_durable);
  REQUIRE(result.files[0].observation.matched);
  REQUIRE(result.files[0].observation.durable);
  REQUIRE_FALSE(result.files[0].publication.published);
  REQUIRE_FALSE(result.files[0].retry.published);
  REQUIRE(f.Load(kTitle).missing());
}
TEST_CASE(
    "Failed intent owners and wrong configured identity never dereference or "
    "publish",
    "[gpd_durable_intent]") {
  Fixture f;
  std::error_code error;
  std::filesystem::path attempted;
  auto failed_unique = filesystem::ProfileIntentOwner::Open(f.parent, {});
  REQUIRE(failed_unique);
  REQUIRE(failed_unique->error());
  std::shared_ptr<const filesystem::ProfileIntentOwner> failed(
      std::move(failed_unique));
  REQUIRE_FALSE(GpdDurableIntentStore::Prepare(failed, f.recovery, f.binding,
                                               f.Pair(), error, attempted));
  REQUIRE(error);
  REQUIRE(attempted.empty());
  REQUIRE_FALSE(GpdDurableIntentStore::Recover(
      failed, f.recovery, "intent-00000000000000000000000000000000.bin",
      f.binding, f.profile, error));
  REQUIRE(error);
  for (bool device : {false, true}) {
    auto foreign = f.binding;
    if (device) {
      foreign.device_id = 99;
    } else {
      foreign.source_id = "other-source";
    }
    REQUIRE_FALSE(GpdDurableIntentStore::Prepare(f.owner, f.recovery, foreign,
                                                 f.Pair(), error, attempted));
    REQUIRE(error == std::errc::state_not_recoverable);
    REQUIRE(attempted.empty());
  }
  REQUIRE(f.Bytes(kDashboard) == f.old_dash);
  REQUIRE(f.Bytes(kTitle) == f.old_title);
}
TEST_CASE(
    "Retired managed proof blocks public journal scan before historical "
    "interpretation",
    "[gpd_durable_intent]") {
  Fixture f;
  std::error_code error;
  f.identity->Retire();
  REQUIRE(GpdDurableIntentStore::Scan(*f.owner, error).empty());
  REQUIRE(error == std::errc::operation_canceled);
}
TEST_CASE(
    "Managed format epoch prevents old pending profile redo from restoring "
    "formatted data",
    "[gpd_durable_intent]") {
  Fixture f;
  std::error_code error;
  std::filesystem::path journal;
  auto intent = f.Prepare(error, journal);
  REQUIRE(intent);
  REQUIRE_FALSE(error);
  const auto old_journal_path = f.owner->root()->path() / journal;
  const auto uuid = f.identity->uuid();
  const auto old_epoch = f.identity->content_epoch();
  intent.reset();
  f.owner.reset();
  f.profile.reset();
  auto formatted = filesystem::ManagedDeviceIdentity::Format(f.identity);
  REQUIRE_FALSE(formatted.error);
  REQUIRE(formatted.complete);
  REQUIRE(formatted.current);
  REQUIRE(formatted.current->uuid() == uuid);
  REQUIRE(formatted.current->content_epoch() == old_epoch + 1);
  f.identity = formatted.current;
  f.source = std::const_pointer_cast<filesystem::HostStorageRoot>(
      f.identity->source());
  f.owner = filesystem::ProfileIntentOwner::Open(f.parent, f.identity);
  REQUIRE_FALSE(f.owner->error());
  REQUIRE(GpdDurableIntentStore::Scan(*f.owner, error).empty());
  REQUIRE_FALSE(error);
  REQUIRE(
      std::filesystem::exists(old_journal_path));  // Historical evidence stays
                                                   // outside formatted Content.
  REQUIRE_FALSE(
      std::filesystem::exists(f.source->path() / "profile" / Leaf(kDashboard)));
}

TEST_CASE(
    "Stale changed target creates no journal or attempted persistent poison",
    "[gpd_durable_intent][snapshot_batch]") {
  Fixture f;
  auto pair = f.Pair();
  std::error_code error;
  auto file = f.profile->OpenFile(Leaf(kTitle),
                                  filesystem::FileAccess::kGenericWrite, error);
  REQUIRE(file);
  REQUIRE_FALSE(file->ResizeResult(f.next_title.size()));
  const auto write = file->WriteResult(0, f.next_title);
  REQUIRE_FALSE(write.error);
  REQUIRE(write.bytes == f.next_title.size());
  REQUIRE_FALSE(file->FlushResult());
  file.reset();
  std::filesystem::path attempted;
  REQUIRE_FALSE(GpdDurableIntentStore::Prepare(
      f.owner, f.recovery, f.binding, std::move(pair), error, attempted));
  REQUIRE(error);
  REQUIRE(attempted.empty());
  REQUIRE(GpdDurableIntentStore::Scan(*f.owner, error).empty());
  REQUIRE_FALSE(error);
  REQUIRE(f.Bytes(kDashboard) == f.old_dash);
  REQUIRE(f.Bytes(kTitle) == f.next_title);
}
TEST_CASE("Unchanged model dependency is checked atomically but not journaled",
          "[gpd_durable_intent][snapshot_batch]") {
  Fixture f;
  auto title = f.Load(kTitle);
  auto dash = f.Load(kDashboard);
  std::error_code error;
  auto file = f.profile->OpenFile(Leaf(kDashboard),
                                  filesystem::FileAccess::kGenericWrite, error);
  REQUIRE(file);
  REQUIRE_FALSE(file->ResizeResult(f.next_dash.size()));
  REQUIRE_FALSE(file->WriteResult(0, f.next_dash).error);
  REQUIRE_FALSE(file->FlushResult());
  file.reset();
  std::filesystem::path attempted;
  const std::vector<std::shared_ptr<const filesystem::HostStorageSnapshotToken>>
      deps{dash.token, title.token};
  REQUIRE_FALSE(GpdDurableIntentStore::Prepare(
      f.owner, f.recovery, f.binding,
      {{GpdIntentAction::kReplace, title, f.next_title}}, error, attempted,
      deps));
  REQUIRE(error);
  REQUIRE(attempted.empty());
  REQUIRE(GpdDurableIntentStore::Scan(*f.owner, error).empty());
  REQUIRE_FALSE(error);
  auto current_dash = f.Load(kDashboard);
  const std::vector<std::shared_ptr<const filesystem::HostStorageSnapshotToken>>
      fresh{current_dash.token, title.token};
  auto intent = GpdDurableIntentStore::Prepare(
      f.owner, f.recovery, f.binding,
      {{GpdIntentAction::kReplace, title, f.next_title}}, error, attempted,
      fresh);
  REQUIRE(intent);
  const auto applied = intent->Advance();
  REQUIRE_FALSE(applied.error);
  REQUIRE(applied.done_durable);
  REQUIRE(applied.files.size() == 1);
  REQUIRE(f.Bytes(kDashboard) == f.next_dash);
}

TEST_CASE(
    "Refused managed relocation preserves genuine pending paired GPD redo",
    "[gpd_durable_intent][initialization]") {
  filesystem::test::SnapshotScratch scratch;
  auto parent = scratch.Root();
  REQUIRE_FALSE(parent->Create("A", true));
  std::error_code error;
  auto open = [&](const char* leaf) {
    filesystem::ManagedVolumeAuthority::Options options;
    options.content = parent->Subroot(leaf, false, error);
    options.state_parent_override = parent;
    options.source_id = "managed-device-A";
    options.device_id = 1;
    return filesystem::ManagedVolumeAuthority::Open(std::move(options));
  };
  auto volume = open("A");
  REQUIRE_FALSE(volume->error());
  std::filesystem::path journal, journal_relative;
  GpdIntentBinding binding;
  std::vector<uint8_t> journal_before, old_dash, old_title, next_dash,
      next_title;
  {
    Fixture f(volume);
    auto intent = f.Prepare(error, journal);
    REQUIRE(intent);
    REQUIRE_FALSE(error);
    REQUIRE_FALSE(f.owner->root()->ReadFile(journal, 1 << 20, journal_before));
    journal_relative =
        (f.owner->root()->path() / journal).lexically_relative(scratch.path);
    binding = f.binding;
    old_dash = f.old_dash;
    old_title = f.old_title;
    next_dash = f.next_dash;
    next_title = f.next_title;
  }
  volume.reset();
  auto names = [&] {
    std::set<std::string> result;
    for (const auto& entry :
         std::filesystem::recursive_directory_iterator(scratch.path)) {
      result.insert(
          entry.path().lexically_relative(scratch.path).generic_string());
    }
    return result;
  };
  const auto before = names();
  std::set<std::string> moved;
  for (const auto& name : before) {
    moved.insert(name == "A"              ? "B"
                 : name.starts_with("A/") ? "B" + name.substr(1)
                                          : name);
  }
  std::filesystem::rename(scratch.path / "A", scratch.path / "B");
  {
    const auto refused = open("B");
    REQUIRE(refused->error() == std::errc::state_not_recoverable);
    REQUIRE_FALSE(refused->profile_writable());
    REQUIRE(names() == moved);
    std::vector<uint8_t> bytes;
    REQUIRE_FALSE(parent->ReadFile(journal_relative, 1 << 20, bytes));
    REQUIRE(bytes == journal_before);
  }
  std::filesystem::rename(scratch.path / "B", scratch.path / "A");
  volume = open("A");
  REQUIRE_FALSE(volume->error());
  REQUIRE(names() == before);
  std::vector<uint8_t> bytes;
  REQUIRE_FALSE(volume->intents()->root()->ReadFile(journal, 1 << 20, bytes));
  REQUIRE(bytes == journal_before);
  auto profile = volume->content()->Subroot("profile", false, error);
  REQUIRE(profile);
  REQUIRE_FALSE(profile->ReadFile(Leaf(kDashboard), 1 << 20, bytes));
  REQUIRE(bytes == old_dash);
  REQUIRE_FALSE(profile->ReadFile(Leaf(kTitle), 1 << 20, bytes));
  REQUIRE(bytes == old_title);
  binding.source_generation = 17;  // Same existing replay contract as Fixture.
  auto recovered = GpdDurableIntentStore::Recover(
      volume->intents(), volume->recovery(), journal, binding, profile, error);
  REQUIRE(recovered);
  REQUIRE_FALSE(error);
  const auto applied = recovered->Advance();
  REQUIRE_FALSE(applied.error);
  REQUIRE(applied.done_durable);
  REQUIRE_FALSE(profile->ReadFile(Leaf(kDashboard), 1 << 20, bytes));
  REQUIRE(bytes == next_dash);
  REQUIRE_FALSE(profile->ReadFile(Leaf(kTitle), 1 << 20, bytes));
  REQUIRE(bytes == next_title);
}

TEST_CASE("Native two-filesystem volume pairs GPDs with its own same-FS redo and recovery owners",
          "[gpd_durable_intent][two_filesystems]") {
  struct stat external_info{}, local_info{};
  if (stat("/dev/shm", &external_info) ||
      stat(std::filesystem::temp_directory_path().c_str(), &local_info) ||
      external_info.st_dev == local_info.st_dev) {
    WARN("No genuine independent tmpfs; cross-FS acceptance remains unverified"); return;
  }
  const auto path = std::filesystem::path("/dev/shm") /
      ("xenia-gpd-volume-" + std::to_string(std::random_device{}()));
  REQUIRE(std::filesystem::create_directory(path));
  std::error_code error;
  auto root = filesystem::HostStorageRoot::Open(path, false, error, 0);
  REQUIRE(root);
  filesystem::ManagedVolumeAuthority::Options options;
  options.content = root;
  options.state_parent_override = root;
  options.source_id = "managed-device-A";
  options.device_id = 1;
  auto volume = filesystem::ManagedVolumeAuthority::Open(std::move(options));
  REQUIRE_FALSE(volume->error());
  {
    Fixture f(volume);
    std::filesystem::path journal;
    auto intent = f.Prepare(error, journal);
    REQUIRE(intent);
    REQUIRE_FALSE(error);
    REQUIRE_FALSE(journal.empty());
    REQUIRE(std::filesystem::exists(f.owner->root()->path() / journal));
    REQUIRE(f.Bytes(kDashboard) == f.old_dash);
    REQUIRE(f.Bytes(kTitle) == f.old_title);
    intent.reset();
    // No in-memory store/CAS publication survives. Re-decode the genuine redo
    // on this small independent FS and complete both real GPD publications.
    auto recovered = f.Recover(journal, error);
    REQUIRE(recovered);
    REQUIRE_FALSE(error);
    auto applied = recovered->Advance();
    REQUIRE_FALSE(applied.error);
    REQUIRE(applied.done_durable);
    REQUIRE(f.Bytes(kDashboard) == f.next_dash);
    REQUIRE(f.Bytes(kTitle) == f.next_title);
    REQUIRE(f.source->Observe().reserve_bytes == 0);
    Fixture foreign;
    auto refused = GpdDurableIntentStore::Prepare(
        f.owner, foreign.recovery, f.binding, f.Pair(), error, journal);
    REQUIRE_FALSE(refused);
    REQUIRE(error == std::errc::cross_device_link);
    REQUIRE(journal.empty());
  }
  volume.reset();
  std::filesystem::remove_all(path, error);
}
}  // namespace xe::kernel::xam::test
#endif
