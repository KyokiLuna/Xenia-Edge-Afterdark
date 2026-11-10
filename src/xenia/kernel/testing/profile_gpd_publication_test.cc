/** Xenia : Xbox 360 Emulator Research Project; BSD license, see LICENSE. */
#include "xenia/kernel/xam/profile_gpd_publication.h"
#include "xenia/kernel/xam/profile_account_snapshot.h"
#include "xenia/kernel/xam/profile_manager.h"
#include "xenia/vfs/devices/host_path_entry.h"
#include <cstring>
#if XE_PLATFORM_LINUX
#include <cerrno>
#include <sys/mount.h>
#endif
#include "xenia/base/testing/snapshot_fixture.h"
#include "xenia/vfs/devices/host_path_device.h"
#include "xenia/vfs/virtual_file_system.h"
#if XE_PLATFORM_LINUX
namespace xe::kernel::test {
TEST_CASE(
    "Profile storage lease retains exact registered device and cancellation "
    "across removal",
    "[profile_gpd_publication]") {
  filesystem::test::SnapshotScratch scratch;
  auto parent = scratch.Root();
  REQUIRE_FALSE(parent->Create("content", true));
  std::error_code error;
  auto source = parent->Subroot("content", false, error);
  REQUIRE(source);
  auto identity =
      filesystem::ManagedDeviceIdentity::Open(source, "managed", 1, error);
  REQUIRE(identity);
  REQUIRE_FALSE(error);
  REQUIRE_FALSE(source->Create("profile", true));
  auto raw = source->Subroot("profile", false, error);
  REQUIRE(raw);
  vfs::VirtualFileSystem fs;
  auto device = std::make_unique<vfs::HostPathDevice>(
      "Lease:", raw->path(), false, false, "STFS", raw, source);
  REQUIRE(device->Initialize());
  REQUIRE(fs.RegisterDevice(std::move(device)));
  auto owned = std::dynamic_pointer_cast<vfs::HostPathDevice>(
      fs.AcquireDevice("Lease:"));
  REQUIRE(owned);
  auto lease = xam::ProfileStorageLease::Capture(
      0xe000000000000001ull, "managed", 1, 7, owned, source, owned->storage(),
      error, identity);
  REQUIRE(lease);
  REQUIRE_FALSE(error);
  REQUIRE_FALSE(lease->CheckAvailable());
  REQUIRE(fs.UnregisterDevice("Lease:"));
  REQUIRE(lease->device() == owned);
  REQUIRE(lease->CheckAvailable());
  lease->Retire();
  REQUIRE(lease->retiring());
}
TEST_CASE(
    "Configured or retiring managed identity cannot be rebound by a profile "
    "lease",
    "[profile_gpd_publication]") {
  filesystem::test::SnapshotScratch scratch;
  auto source = scratch.Root();
  std::error_code error;
  auto identity =
      filesystem::ManagedDeviceIdentity::Open(source, "managed", 1, error);
  REQUIRE(identity);
  vfs::VirtualFileSystem fs;
  auto device = std::make_unique<vfs::HostPathDevice>(
      "Lease:", source->path(), false, false, "STFS", source, source);
  REQUIRE(device->Initialize());
  REQUIRE(fs.RegisterDevice(std::move(device)));
  auto owned = std::dynamic_pointer_cast<vfs::HostPathDevice>(
      fs.AcquireDevice("Lease:"));
  REQUIRE(owned);
  REQUIRE_FALSE(xam::ProfileStorageLease::Capture(
      1, "other", 1, 7, owned, source, owned->storage(), error, identity));
  REQUIRE(error == std::errc::invalid_argument);
  identity->Retire();
  REQUIRE_FALSE(xam::ProfileStorageLease::Capture(
      1, "managed", 1, 7, owned, source, owned->storage(), error, identity));
  REQUIRE(error == std::errc::invalid_argument);
}
TEST_CASE(
    "The production profile-package removal path preserves owned game saves",
    "[profile_gpd_publication]") {
  filesystem::test::SnapshotScratch scratch;
  auto root = scratch.Root();
  std::error_code error;
  constexpr uint64_t xuid = 0xe000000000000001ull;
  const auto profile = xam::ProfileStorageLease::ProfileRelative(xuid);
  REQUIRE(profile ==
          std::filesystem::path(
              "E000000000000001/FFFE07D1/00010000/E000000000000001"));
  const auto save =
      std::filesystem::path("E000000000000001/12345678/00000001/save");
  for (const auto& target : {profile, save.parent_path()}) {
    auto parent = root;
    for (const auto& part : target) {
      auto next = parent->Subroot(part, false, error);
      if (!next) {
        REQUIRE(error == std::errc::no_such_file_or_directory);
        REQUIRE_FALSE(parent->Create(part, true));
        next = parent->Subroot(part, false, error);
      }
      REQUIRE(next);
      parent = std::move(next);
    }
  }
  const std::array<uint8_t, 3> bytes{11, 22, 33};
  REQUIRE_FALSE(root->WriteNewFile(profile / "Account", bytes).error);
  REQUIRE_FALSE(root->WriteNewFile(save, bytes).error);
  REQUIRE_FALSE(root->Remove(profile, true));
  REQUIRE_FALSE(std::filesystem::exists(root->path() / profile));
  std::vector<uint8_t> actual;
  REQUIRE_FALSE(root->ReadFile(save, 16, actual));
  REQUIRE(actual == std::vector<uint8_t>{11, 22, 33});
  REQUIRE(xam::ProfileStorageLease::ProfileRelative(0).empty());
}
TEST_CASE("Profile publication retains independent guest-generation cancellation",
          "[profile_gpd_publication][managed_format]") {
  filesystem::test::SnapshotScratch scratch;
  auto source = scratch.Root();
  std::error_code error;
  auto identity =
      filesystem::ManagedDeviceIdentity::Open(source, "managed", 1, error);
  REQUIRE(identity);
  REQUIRE_FALSE(error);
  auto generation = source->GuestView(error);
  REQUIRE(generation);
  REQUIRE_FALSE(error);
  REQUIRE_FALSE(generation->Create("profile", true));
  auto raw = generation->Subroot("profile", false, error);
  REQUIRE(raw);
  vfs::VirtualFileSystem fs;
  auto device = std::make_unique<vfs::HostPathDevice>(
      "Lease:", raw->path(), false, false, "STFS", raw, generation);
  REQUIRE(device->Initialize());
  REQUIRE(fs.RegisterDevice(std::move(device)));
  auto owned = std::dynamic_pointer_cast<vfs::HostPathDevice>(
      fs.AcquireDevice("Lease:"));
  REQUIRE(owned);
  auto lease = xam::ProfileStorageLease::Capture(
      1, "managed", 1, 7, owned, source, owned->storage(), error, identity,
      generation);
  REQUIRE(lease);
  REQUIRE_FALSE(error);
  REQUIRE(lease->source() == identity->source());
  REQUIRE(lease->guest_generation() == generation);
  REQUIRE_FALSE(lease->CheckAvailable());
  generation->Retire();
  REQUIRE_FALSE(identity->retiring());
  REQUIRE_FALSE(identity->CheckCurrent());
  REQUIRE_FALSE(source->retiring());
  REQUIRE(owned->storage()->retiring());
  REQUIRE(lease->retiring());
  REQUIRE(lease->CheckAvailable() == std::errc::operation_canceled);
  auto next = source->GuestView(error);
  REQUIRE(next);
  REQUIRE_FALSE(error);
  REQUIRE_FALSE(next->retiring());
  REQUIRE(lease->retiring());  // A fresh child cannot revive an old lease.
  REQUIRE_FALSE(xam::ProfileStorageLease::Capture(
      1, "managed", 1, 8, owned, source, owned->storage(), error, identity, next));
  REQUIRE(error == std::errc::invalid_argument);
}
}  // namespace xe::kernel::test
#endif

#if XE_PLATFORM_LINUX
namespace xe::kernel::test {
namespace {
struct AccountFixture {
  filesystem::test::SnapshotScratch scratch;
  std::shared_ptr<filesystem::HostStorageRoot> parent, source, raw;
  std::shared_ptr<const filesystem::SnapshotRecoveryOwner> recovery;
  std::shared_ptr<const filesystem::ManagedDeviceIdentity> source_identity;
  vfs::VirtualFileSystem fs;
  std::shared_ptr<vfs::HostPathDevice> device;
  std::shared_ptr<xam::ProfileStorageLease> storage;
  std::shared_ptr<xam::ProfileAccountIdentity> identity;
  xam::X_XAMACCOUNTINFO account{};
  std::vector<uint8_t> original;
  static constexpr uint64_t kXuid = 0xe000000000000123ull;
  explicit AccountFixture(bool devkit = false, bool seed = true) {
    parent = scratch.Root();
    recovery = filesystem::SnapshotRecoveryOwner::Open(parent);
    REQUIRE_FALSE(recovery->error());
    REQUIRE_FALSE(parent->Create("content", true));
    std::error_code error;
    source = parent->Subroot("content", false, error);
    REQUIRE(source);
    source_identity = filesystem::ManagedDeviceIdentity::Open(
        source, "account-test", 0xcafe, error);
    REQUIRE(source_identity);
    REQUIRE_FALSE(error);
    REQUIRE_FALSE(source->Create("profile", true));
    raw = source->Subroot("profile", false, error);
    REQUIRE(raw);
    // Opaque values remain data, including embedded NULs and non-default bits.
    std::memset(&account, 0x5a, sizeof(account));
    account.gamertag[0] = xe::byte_swap(char16_t('A'));
    account.gamertag[1] = 0;
    account.xuid_online = 0;
    original.resize(xam::ProfileAccountSnapshot::kPrefixBytes);
    xam::ProfileManager::EncryptAccountFile(&account, original.data(), devkit);
    original.insert(original.end(), {0xfa, 0, 0xfe, 0x81, 0, 0x92});
    if (seed) { REQUIRE_FALSE(raw->WriteNewFile("Account", original).error); }
    auto mount = std::make_unique<vfs::HostPathDevice>(
        "Account:", raw->path(), false, false, "STFS", raw, source);
    REQUIRE(mount->Initialize());
    REQUIRE(fs.RegisterDevice(std::move(mount)));
    device = std::dynamic_pointer_cast<vfs::HostPathDevice>(fs.AcquireDevice("Account:"));
    REQUIRE(device);
    storage = xam::ProfileStorageLease::Capture(
        kXuid, "account-test", 0xcafe, 7, device, source, device->storage(),
        error, source_identity);
    REQUIRE(storage);
    identity = xam::ProfileAccountIdentity::Capture(storage, 2, 17, error);
    REQUIRE(identity);
    REQUIRE_FALSE(error);
  }
  xam::ProfileAccountWriter Writer() { return {identity, recovery}; }
  std::shared_ptr<const xam::ProfileAccountSnapshot> Load() {
    std::error_code error;
    auto snapshot = xam::ProfileAccountSnapshot::Load(identity, error);
    REQUIRE(snapshot);
    REQUIRE_FALSE(error);
    return snapshot;
  }
  std::vector<uint8_t> Disk() {
    std::vector<uint8_t> bytes;
    REQUIRE_FALSE(raw->ReadFile("Account", xam::ProfileAccountSnapshot::kMaximumBytes, bytes));
    return bytes;
  }
};
struct ServiceRundownProbe {
  std::atomic<bool> destroyed{false}, under_namespace{false};
};
class ReleasingAccountService final : public vfs::ManagedFileService {
 public:
  ReleasingAccountService(std::shared_ptr<const filesystem::HostStorageRoot> root,
      std::atomic<std::shared_ptr<vfs::ManagedFileService>>& external,
      ServiceRundownProbe& probe)
      : root_(std::move(root)),path_(root_->path()/"Account"),
        external_(external),probe_(probe) {}
  ~ReleasingAccountService() override {
    probe_.under_namespace.store(xe::global_critical_region::is_held_by_current_thread());
    probe_.destroyed.store(true);
  }
  const std::filesystem::path& backing_path() const override { return path_; }
  std::shared_ptr<const filesystem::HostStorageDirectoryIdentity>
  backing_directory() const override { return root_->directory_identity(); }
  bool retiring() const override {
    if (!retired.load()) { return false; }
    // Metadata-only deterministic external rundown: the registry's genuine
    // weak.lock is now the last other owner, matching a concurrent retirement.
    external_.store({},std::memory_order_release);
    return true;
  }
  X_STATUS Open(std::shared_ptr<vfs::Device>,std::string,vfs::FileDisposition,
      uint32_t,bool,vfs::File**,vfs::FileAction*) override { return X_STATUS_CANCELLED; }
  std::atomic<bool> retired{false};
 private:
  const std::shared_ptr<const filesystem::HostStorageRoot> root_;
  const std::filesystem::path path_;
  std::atomic<std::shared_ptr<vfs::ManagedFileService>>& external_;
  ServiceRundownProbe& probe_;
};
class DecliningAccountService final : public vfs::ManagedFileService {
 public:
  DecliningAccountService(std::filesystem::path path,
      std::shared_ptr<const filesystem::HostStorageDirectoryIdentity> parent)
      : path_(std::move(path)), parent_(std::move(parent)) {}
  const std::filesystem::path& backing_path() const override { return path_; }
  std::shared_ptr<const filesystem::HostStorageDirectoryIdentity>
  backing_directory() const override { return parent_; }
  bool retiring() const override { return retired; }
  X_STATUS Open(std::shared_ptr<vfs::Device>, std::string,
      vfs::FileDisposition disposition, uint32_t, bool, vfs::File**,
      vfs::FileAction*) override {
    ++entries;
    held_namespace = xe::global_critical_region::is_held_by_current_thread();
    last = disposition;
    return X_STATUS_ACCESS_DENIED;
  }
  bool retired = false, held_namespace = true;
  size_t entries = 0;
  vfs::FileDisposition last{};
 private:
  std::filesystem::path path_;
  std::shared_ptr<const filesystem::HostStorageDirectoryIdentity> parent_;
};
}  // namespace
TEST_CASE("Account receipt authenticates complete real bytes and retains immutable identity",
          "[profile_account][persistence]") {
  AccountFixture f;
  auto snapshot = f.Load();
  CHECK(snapshot->identity() == f.identity);
  CHECK(snapshot->identity()->storage()->xuid() == AccountFixture::kXuid);
  CHECK(snapshot->identity()->slot() == 2);
  CHECK(snapshot->identity()->sign_in_generation() == 17);
  CHECK(snapshot->identity()->storage()->device_id() == 0xcafe);
  CHECK(std::memcmp(&snapshot->account(), &f.account, sizeof(f.account)) == 0);
  CHECK(std::vector<uint8_t>(snapshot->token()->bytes().begin(),
                              snapshot->token()->bytes().end()) == f.original);
  CHECK_FALSE(snapshot->CheckAvailable());
}
TEST_CASE("Account update preserves opaque plaintext suffix and encryption flavor",
          "[profile_account][persistence][recovery]") {
  bool devkit = false;
  SECTION("retail") {}
  SECTION("devkit") { devkit = true; }
  AccountFixture f(devkit);
  auto expected = f.Load();
  auto next = f.account;
  next.gamertag[0] = xe::byte_swap(char16_t('B'));
  auto result = f.Writer().Replace(expected, next);
  REQUIRE_FALSE(result.error);
  REQUIRE(result.storage.published);
  REQUIRE(result.storage.durable);
  REQUIRE(result.storage.recovery_durable);
  REQUIRE(result.durable);
  CHECK(result.durable->devkit() == devkit);
  CHECK(result.durable->identity() == expected->identity());
  CHECK(std::memcmp(&result.durable->account(), &next, sizeof(next)) == 0);
  auto actual = f.Disk();
  CHECK(actual.size() == f.original.size());
  CHECK(std::equal(f.original.begin() + xam::ProfileAccountSnapshot::kPrefixBytes,
                   f.original.end(), actual.begin() + xam::ProfileAccountSnapshot::kPrefixBytes));
  std::vector<uint8_t> backup;
  REQUIRE_FALSE(f.recovery->root()->ReadFile(result.storage.recovery,
      xam::ProfileAccountSnapshot::kMaximumBytes, backup));
  CHECK(backup == f.original);
  CHECK(expected->token()->info().file_id != result.durable->token()->info().file_id);
}
TEST_CASE("Account creation needs observed absence and never replaces corrupt payloads",
          "[profile_account][persistence][create]") {
  AccountFixture f(false, false);
  auto created = f.Writer().Create(f.account);
  REQUIRE(created.durable);
  REQUIRE_FALSE(created.error);
  CHECK_FALSE(created.storage.recovery_created);
  auto duplicate = f.Writer().Create(f.account);
  CHECK(duplicate.error == std::errc::file_exists);
  CHECK_FALSE(duplicate.storage.published);
  CHECK_FALSE(duplicate.durable);
  auto bytes = f.Disk();
  bytes[0] ^= 0x80;
  std::error_code error;
  auto file = f.raw->OpenFile("Account", filesystem::FileAccess::kGenericWrite, error);
  REQUIRE(file);
  REQUIRE_FALSE(file->WriteResult(0, bytes).error);
  REQUIRE_FALSE(file->FlushResult());
  file.reset();
  auto token = f.device->storage()->CaptureSnapshot("Account", 4096, error, true);
  REQUIRE(token);
  std::vector<uint8_t> valid(xam::ProfileAccountSnapshot::kPrefixBytes);
  xam::ProfileManager::EncryptAccountFile(&f.account, valid.data());
  auto overwrite = f.Writer().PublishRaw(token, valid);
  CHECK(overwrite.error == std::errc::bad_message);
  CHECK_FALSE(overwrite.storage.stage_created);
  CHECK_FALSE(overwrite.durable);
  CHECK(f.Disk() == bytes);
}
TEST_CASE("Account partial byte session never truncates the original on invalid flush or drop",
          "[profile_account][guest_write][partial]") {
  AccountFixture f;
  {
    xam::ProfileAccountByteSession write(f.Writer(), f.Load(), true);
    REQUIRE_FALSE(write.Write(0, std::span<const uint8_t>(f.original).first(19)));
    CHECK(f.Disk() == f.original);
    auto flush = write.Flush();
    CHECK(flush.error == std::errc::bad_message);
    CHECK_FALSE(flush.storage.stage_created);
    CHECK_FALSE(flush.durable);
    CHECK(f.Disk() == f.original);
    CHECK(write.Resize(xam::ProfileAccountSnapshot::kMaximumBytes + 1) ==
          std::errc::file_too_large);
  }
  CHECK(f.Disk() == f.original);
  REQUIRE(f.Load());
}
TEST_CASE("Account split guest bytes share real durable publication and preserve caller suffix",
          "[profile_account][guest_write][persistence]") {
  AccountFixture f;
  auto next = f.account;
  next.gamertag[0] = xe::byte_swap(char16_t('C'));
  auto wire = f.original;
  xam::ProfileManager::EncryptAccountFile(&next, wire.data());
  wire.back() = 0x19;
  xam::ProfileAccountByteSession write(f.Writer(), f.Load(), true);
  REQUIRE_FALSE(write.Write(0, std::span<const uint8_t>(wire).first(53)));
  REQUIRE_FALSE(write.Write(53, std::span<const uint8_t>(wire).subspan(53)));
  CHECK(f.Disk() == f.original);
  auto result = write.Flush();
  REQUIRE(result.durable);
  REQUIRE_FALSE(result.error);
  CHECK(f.Disk() == wire);
  CHECK(result.storage.recovery_durable);
  CHECK(std::memcmp(&result.durable->account(), &next, sizeof(next)) == 0);
  auto clean = write.Flush();
  REQUIRE(clean.durable == result.durable);
  CHECK_FALSE(clean.storage.published);  // No replayed success-shaped write.
  CHECK_FALSE(clean.storage.stage_created);
}
TEST_CASE("Account cancellation after genuine stage and backup retains both and refuses commit",
          "[profile_account][cancel][recovery]") {
  AccountFixture f;
  auto expected = f.Load();
  std::error_code error;
  auto prepared = f.Writer().PrepareRaw(expected->token(), f.original, error);
  REQUIRE(prepared);
  REQUIRE_FALSE(error);
  REQUIRE(prepared->storage().stage_created);
  REQUIRE(prepared->storage().recovery_durable);
  const auto stage = prepared->storage().stage;
  const auto backup = prepared->storage().recovery;
  CHECK_FALSE(f.identity->writes_drained());
  f.identity->Retire();
  auto cancelled = f.Writer().Commit(*prepared);
  CHECK(cancelled.error == std::errc::operation_canceled);
  CHECK_FALSE(cancelled.storage.published);
  CHECK_FALSE(cancelled.durable);
  CHECK(f.Disk() == f.original);
  CHECK(std::filesystem::exists(f.raw->path() / stage));
  CHECK(std::filesystem::exists(f.recovery->root()->path() / backup));
  CHECK_FALSE(f.identity->writes_drained());
  prepared.reset();
  CHECK(f.identity->writes_drained());
}
TEST_CASE("Account prepared CAS rejects a competing same-inode edit and preserves recovery",
          "[profile_account][cas][recovery]") {
  AccountFixture f;
  auto expected = f.Load();
  auto intended = f.original;
  auto next = f.account;
  next.gamertag[0] = xe::byte_swap(char16_t('D'));
  xam::ProfileManager::EncryptAccountFile(&next, intended.data());
  std::error_code error;
  auto prepared = f.Writer().PrepareRaw(expected->token(), intended, error);
  REQUIRE(prepared);
  REQUIRE_FALSE(error);
  auto competitor = f.original;
  competitor.back() ^= 0x40;
  auto file = f.raw->OpenFile("Account", filesystem::FileAccess::kGenericWrite, error);
  REQUIRE(file);
  REQUIRE_FALSE(file->WriteResult(0, competitor).error);
  REQUIRE_FALSE(file->FlushResult());
  file.reset();
  auto result = f.Writer().Commit(*prepared);
  CHECK(result.error == std::errc::state_not_recoverable);
  CHECK_FALSE(result.storage.published);
  CHECK_FALSE(result.durable);
  CHECK(f.Disk() == competitor);
  std::vector<uint8_t> backup;
  REQUIRE_FALSE(f.recovery->root()->ReadFile(result.storage.recovery, 4096, backup));
  CHECK(backup == f.original);
}
TEST_CASE("Account ownership cannot revive after same-slot replacement or source retirement",
          "[profile_account][lifetime][identity]") {
  AccountFixture f;
  auto old = f.Load();
  f.identity->Retire();
  std::error_code error;
  auto next_identity = xam::ProfileAccountIdentity::Capture(f.storage, 2, 18, error);
  REQUIRE(next_identity);
  auto next = xam::ProfileAccountSnapshot::Load(next_identity, error);
  REQUIRE(next);
  CHECK_FALSE(error);
  CHECK(old->CheckAvailable() == std::errc::operation_canceled);
  CHECK_FALSE(f.Writer().Replace(old, f.account).durable);
  CHECK(next->identity()->sign_in_generation() == 18);
  f.device->Retire();
  CHECK(next->CheckAvailable() == std::errc::operation_canceled);
  xam::ProfileAccountWriter writer(next_identity, f.recovery);
  CHECK_FALSE(writer.Replace(next, f.account).durable);
  CHECK(f.Disk() == f.original);
}
TEST_CASE("Account cache needs actual durable current token and retains existing Entry identity",
          "[profile_account][cache][persistence]") {
  AccountFixture f;
  auto* entry = f.device->ResolvePath("Account");
  REQUIRE(entry);
  const auto old_size = entry->size();
  auto expected = f.Load();
  auto wire = f.original;
  wire.insert(wire.end(), {4, 3, 2, 1});
  auto result = f.Writer().PublishRaw(expected->token(), wire);
  REQUIRE(result.durable);
  CHECK(entry->size() == old_size);
  std::error_code error;
  auto stale = f.device->storage()->ValidateSnapshotBatch({expected->token()}, error);
  CHECK_FALSE(stale);
  CHECK(error == std::errc::state_not_recoverable);
  auto prepared = f.device->PrepareSnapshotCache({result.durable->token()}, error);
  REQUIRE(prepared);
  auto current = f.device->storage()->ValidateSnapshotBatch({result.durable->token()}, error);
  REQUIRE(current);
  std::shared_ptr<const filesystem::HostStorageSnapshotBatch> batch(std::move(current));
  auto commit = f.device->TryCommitSnapshotCache(prepared, batch, error);
  REQUIRE(commit);
  commit->Apply();
  CHECK(f.device->ResolvePath("Account") == entry);
  CHECK(entry->size() == wire.size());
}
TEST_CASE("Actual snapshot target reservation excludes foreign base aliases across inode replacement",
          "[profile_account][snapshot_publication][namespace][cas]") {
  AccountFixture f;
  auto old = f.Load();
  std::error_code error;
  auto foreign = f.raw->OpenFile("Account", filesystem::FileAccess::kGenericRead, error);
  REQUIRE(foreign);
  CHECK_FALSE(f.device->storage()->ReserveSnapshotFile(old->token(), error));
  CHECK(error == std::errc::device_or_resource_busy);
  foreign.reset();
  auto reserved = f.device->storage()->ReserveSnapshotFile(old->token(), error);
  REQUIRE(reserved);
  REQUIRE_FALSE(error);
  REQUIRE(reserved->IsSnapshotReservationOf(*f.device->storage()));
  CHECK(reserved->ReleaseRetiredSnapshotReservation() == std::errc::operation_not_permitted);
  auto before = reserved->CaptureSnapshot("Account", 4096, error, true);
  REQUIRE(before);
  REQUIRE_FALSE(error);
  REQUIRE_FALSE(reserved->ObserveSnapshotDurability(*before).error);
  {
    auto checked = reserved->ValidateSnapshotBatch({before}, error);
    REQUIRE(checked);
  }
  CHECK_FALSE(f.raw->OpenFile("Account", filesystem::FileAccess::kGenericWrite, error));
  CHECK(error == std::errc::device_or_resource_busy);
  CHECK(f.source->Remove("profile", true) == std::errc::device_or_resource_busy);
  auto next = f.account;
  next.gamertag[0] = xe::byte_swap(char16_t('N'));
  auto intended = f.original;
  xam::ProfileManager::EncryptAccountFile(&next, intended.data());
  auto receipt = filesystem::SnapshotPublisher::Publish(*f.recovery, before, intended);
  REQUIRE_FALSE(receipt.error);
  REQUIRE(receipt.published);
  REQUIRE(receipt.durable);
  REQUIRE(receipt.current);
  CHECK(receipt.current->info().file_id != before->info().file_id);
  {
    auto checked = reserved->ValidateSnapshotBatch({receipt.current}, error);
    REQUIRE(checked);
  }
  REQUIRE_FALSE(reserved->RetrySnapshotDurability(receipt.current).error);
  CHECK_FALSE(f.raw->CaptureSnapshot("Account", 4096, error));
  CHECK(error == std::errc::device_or_resource_busy);
  CHECK(f.raw->Remove("Account", false) == std::errc::device_or_resource_busy);
  CHECK(f.source->Remove("profile", true) == std::errc::device_or_resource_busy);
  reserved->Retire();
  REQUIRE_FALSE(reserved->ReleaseRetiredSnapshotReservation());
  CHECK(f.Disk() == intended);
}
TEST_CASE("Account closed admission retains every real bounded write owner until actual release",
          "[profile_account][lifetime][cancel][capacity]") {
  AccountFixture f;
  std::error_code error;
  std::vector<std::unique_ptr<xam::ProfileAccountIdentity::Write>> writes;
  for (size_t i = 0; i != 32; ++i) {
    auto write = f.identity->EnterWrite(error);
    REQUIRE(write);
    REQUIRE_FALSE(error);
    writes.push_back(std::move(write));
  }
  CHECK_FALSE(f.identity->EnterWrite(error));
  CHECK(error == std::errc::device_or_resource_busy);
  CHECK_FALSE(f.identity->writes_drained());
  f.identity->Retire();
  CHECK_FALSE(f.identity->EnterWrite(error));
  CHECK(error == std::errc::operation_canceled);
  while (writes.size() > 1) { writes.pop_back(); CHECK_FALSE(f.identity->writes_drained()); }
  writes.clear();
  CHECK(f.identity->writes_drained());
  auto replacement = xam::ProfileAccountIdentity::Capture(f.storage, 2, 18, error);
  REQUIRE(replacement);
  auto current = replacement->EnterWrite(error);
  REQUIRE(current);
  CHECK_FALSE(replacement->writes_drained());
  CHECK(f.identity->writes_drained());
}
TEST_CASE("Original17559 logon Account row is exact188 with full raw Account and stable device",
          "[profile_account][abi][native_logon]") {
  AccountFixture f;
  auto snapshot = f.Load();
  std::array<uint8_t, 0x188> row;
  row.fill(0xcd);
  REQUIRE_FALSE(xam::NativeLogonAccountInputs::EncodeRow(*snapshot, row));
  for (size_t i = 0; i != 8; ++i) {
    CHECK(row[i] == uint8_t(AccountFixture::kXuid >> ((7 - i) * 8)));
  }
  CHECK(std::memcmp(row.data() + 8, &f.account, 0x17c) == 0);
  CHECK(std::vector<uint8_t>(row.begin() + 0x184, row.end()) ==
        std::vector<uint8_t>{0, 0, 0xca, 0xfe});
  const auto before = row;
  CHECK(xam::NativeLogonAccountInputs::EncodeRow(*snapshot,
      std::span<uint8_t>(row).first(0x187)) == std::errc::invalid_argument);
  CHECK(row == before);
  f.identity->Retire();
  CHECK(xam::NativeLogonAccountInputs::EncodeRow(*snapshot, row) ==
        std::errc::invalid_argument);
  CHECK(row == before);
}
TEST_CASE("Account authentication failure never mints a durable default or mutates cache",
          "[profile_account][persistence][malformed]") {
  AccountFixture f;
  std::vector<uint8_t> corrupt;
  SECTION("truncated prefix") { corrupt.assign(0x193, 0); }
  SECTION("zero-filled large file") { corrupt.assign(0x800, 0); }
  SECTION("failed authentic hash") { corrupt = f.original; corrupt[15] ^= 1; }
  std::error_code error;
  auto handle = f.raw->OpenFile("Account", filesystem::FileAccess::kGenericWrite, error);
  REQUIRE(handle);
  REQUIRE_FALSE(handle->ResizeResult(corrupt.size()));
  REQUIRE_FALSE(handle->WriteResult(0, corrupt).error);
  REQUIRE_FALSE(handle->FlushResult());
  handle.reset();
  auto snapshot = xam::ProfileAccountSnapshot::Load(f.identity, error);
  CHECK_FALSE(snapshot);
  CHECK(error == std::errc::bad_message);
  CHECK(f.Disk() == corrupt);
  CHECK(f.device->ResolvePath("Account")->size() == f.original.size());
}
TEST_CASE("Managed Account VFS admission precedes supersede overwrite and raw aliases",
          "[profile_account][vfs][lifetime]") {
  AccountFixture f;
  auto service = std::make_shared<DecliningAccountService>(
      f.raw->path() / "Account",f.raw->directory_identity());
  REQUIRE(f.fs.RegisterManagedFile(service));
  auto alias = std::make_unique<vfs::HostPathDevice>(
      "Alias:", f.raw->path(), false, false, "FATX", f.raw, f.source);
  REQUIRE(alias->Initialize());
  REQUIRE(f.fs.RegisterDevice(std::move(alias)));
  for (auto disposition : {vfs::FileDisposition::kSuperscede,
                            vfs::FileDisposition::kOverwrite,
                            vfs::FileDisposition::kOverwriteIf}) {
    vfs::File* file = nullptr;
    vfs::FileAction action{};
    CHECK(f.fs.OpenFile(nullptr, "Alias:\\account", disposition,
        vfs::FileAccess::kFileWriteData, false, true, &file, &action) ==
        X_STATUS_ACCESS_DENIED);
    CHECK_FALSE(file);
    CHECK(service->last == disposition);
    CHECK_FALSE(service->held_namespace);
    CHECK(f.Disk() == f.original);
  }
  const auto flat_calls=service->entries;
  for (const auto nested : {"Alias:\\Account\\evil", "Alias:\\Account\\Account"}) {
    vfs::File* file=nullptr;
    vfs::FileAction action{};
    CHECK(XFAILED(f.fs.OpenFile(nullptr,nested,vfs::FileDisposition::kOpen,
        vfs::FileAccess::kFileReadData,false,true,&file,&action)));
    CHECK_FALSE(file);
    CHECK(service->entries==flat_calls);  // Exact routing never entered service.
    CHECK(f.Disk()==f.original);
  }
  auto* entry = f.fs.ResolvePath("Alias:\\Account");
  REQUIRE(entry);
  CHECK_FALSE(entry->OverwriteInPlace().status == X_STATUS_SUCCESS);
  vfs::File* direct = nullptr;
  CHECK(entry->Open(vfs::FileAccess::kFileWriteData, &direct) == X_STATUS_ACCESS_DENIED);
  CHECK_FALSE(entry->OpenMapped(MappedMemory::Mode::kRead));
  CHECK_FALSE(entry->Delete());
  CHECK(entry->Rename("Alias:\\Other") == X_STATUS_ACCESS_DENIED);
  CHECK(f.Disk() == f.original);
  const auto calls = service->entries;
  {
    auto namespace_owner = xe::global_critical_region::AcquireDirect();
    vfs::File* file = nullptr;
    vfs::FileAction action{};
    CHECK(XFAILED(f.fs.OpenFile(nullptr, "Alias:\\Account", vfs::FileDisposition::kOpen,
        vfs::FileAccess::kFileWriteData, false, true, &file, &action)));
    CHECK(service->entries == calls);
  }
  service->retired = true;
  // A tombstone still blocks direct raw operations after the old mount stops.
  CHECK_FALSE(entry->OverwriteInPlace().status == X_STATUS_SUCCESS);
}

TEST_CASE("Direct Entry create cannot bypass an absent owning Account transaction fence",
          "[profile_account][vfs][namespace][absent]") {
  AccountFixture f(false,false);
  std::error_code error;
  auto mount=std::make_unique<vfs::HostPathDevice>(
      "Managed:",f.raw->path(),false,false,"STFS",f.raw,f.source);
  REQUIRE(mount->Initialize());
  auto observed=mount->storage()->CaptureSnapshot("Account",4096,error,true);
  REQUIRE(observed);
  REQUIRE_FALSE(observed->exists());
  auto owned=mount->storage()->ReserveSnapshotFile(observed,error);
  REQUIRE(owned);
  REQUIRE(mount->InstallSnapshotReservation(owned));
  auto service=std::make_shared<DecliningAccountService>(
      f.raw->path()/"Account",owned->directory_identity());
  REQUIRE(f.fs.RegisterManagedFile(service));
  REQUIRE(f.fs.RegisterDevice(std::move(mount)));
  auto* root=f.fs.ResolvePath("Managed:");
  REQUIRE(root);
  CHECK_FALSE(root->CreateEntry("Account",vfs::kFileAttributeNormal));
  CHECK_FALSE(root->CreateEntry("account",vfs::kFileAttributeNormal));
  auto untouched=owned->CaptureSnapshot("Account",4096,error,true);
  REQUIRE(untouched);
  CHECK_FALSE(untouched->exists());
  service->retired=true;
  service.reset();
  // Expired policy remains a physically pinned tombstone. The owning storage
  // permit is IO authority only for the authenticated producer, not raw Entry.
  CHECK_FALSE(root->CreateEntry("Account",vfs::kFileAttributeNormal));
  untouched=owned->CaptureSnapshot("Account",4096,error,true);
  REQUIRE(untouched);
  CHECK_FALSE(untouched->exists());
  REQUIRE(f.fs.UnregisterDevice("Managed:"));
}
TEST_CASE("Native directory preparation refuses namespace ownership while cached matching stays pure",
          "[profile_account][namespace][locks]") {
  AccountFixture f;
  const auto cached=f.raw->directory_identity();
  REQUIRE(cached);
  std::error_code error;
  {
    auto namespace_owner=xe::global_critical_region::AcquireDirect();
    auto forbidden=filesystem::HostStorageDirectoryIdentity::Capture(
        f.raw->path()/"does-not-exist",error);
    CHECK_FALSE(forbidden);
    // BUSY precedes even path traversal; no fabricated missing-directory proof.
    CHECK(error==std::errc::device_or_resource_busy);
    CHECK(cached->Overlaps("Account",*cached,"Account",false));
  }
}
TEST_CASE("Retained directory coordinates follow real rename and keep old ancestry alive",
          "[profile_account][namespace][native_lifetime]") {
  AccountFixture f;
  REQUIRE_FALSE(f.parent->Create("old-parent",true));
  REQUIRE_FALSE(f.parent->Create("old-parent/child",true));
  std::error_code error;
  auto parent=filesystem::HostStorageDirectoryIdentity::Capture(
      f.parent->path()/"old-parent",error);
  REQUIRE(parent);
  auto child=filesystem::HostStorageDirectoryIdentity::Capture(
      f.parent->path()/"old-parent/child",error);
  REQUIRE(child);
  CHECK(parent->Overlaps("child",*child,"Account",false));
  // This read-only metadata capability grants no mutation permission. Move the
  // actual child first, unlink the old empty ancestor, then create a new one.
  REQUIRE_FALSE(f.parent->Rename("old-parent/child","moved-child",false));
  REQUIRE_FALSE(f.parent->Remove("old-parent",false));
  REQUIRE_FALSE(f.parent->Create("old-parent",true));
  auto replacement=filesystem::HostStorageDirectoryIdentity::Capture(
      f.parent->path()/"old-parent",error);
  auto moved=filesystem::HostStorageDirectoryIdentity::Capture(
      f.parent->path()/"moved-child",error);
  REQUIRE(replacement);
  REQUIRE(moved);
  CHECK_FALSE(parent->Overlaps({},*replacement,{},false));
  CHECK(child->Overlaps("Account",*moved,"Account",false));
  CHECK_FALSE(replacement->Overlaps("child",*moved,"Account",false));
}
#if XE_PLATFORM_LINUX
// Requires a real isolated bind-mount-capable test environment. A mount error
// is a genuine prerequisite failure, never a skipped/fabricated acceptance.
TEST_CASE("Real bind alias cannot bypass Account owner or tombstone after inode CAS",
          "[.requires_bind_mount][profile_account][namespace][physical_alias]") {
  AccountFixture f;
  REQUIRE_FALSE(f.parent->Create("bind-alias",true));
  struct Binding {
    std::filesystem::path path;
    bool mounted=false;
    ~Binding() {
      if (mounted && ::umount(path.c_str())) {
        // Isolated fixture only; detach prevents generic temp cleanup from
        // traversing a still-mounted target after an assertion unwinds.
        ::umount2(path.c_str(),MNT_DETACH);
      }
    }
  } binding{f.parent->path()/"bind-alias"};
  const int mounted=::mount(f.raw->path().c_str(),binding.path.c_str(),
                            nullptr,MS_BIND,nullptr);
  INFO("real bind mount returned " << mounted << ", errno " << errno);
  REQUIRE(mounted==0);
  binding.mounted=true;
  std::error_code error;
  auto alias=filesystem::HostStorageRoot::Open(binding.path,false,error);
  REQUIRE(alias);
  REQUIRE(alias->path()!=f.raw->path());
  REQUIRE(alias->directory_identity()->Overlaps("Account",
      *f.raw->directory_identity(),"Account",false));
  auto alias_device=std::make_unique<vfs::HostPathDevice>(
      "Physical:",alias->path(),false,false,"FATX",alias,alias);
  REQUIRE(alias_device->Initialize());
  REQUIRE(f.fs.RegisterDevice(std::move(alias_device)));
  auto old=f.Load();
  auto foreign=alias->OpenFile("Account",filesystem::FileAccess::kGenericRead,error);
  REQUIRE(foreign);
  CHECK_FALSE(f.device->storage()->ReserveSnapshotFile(old->token(),error));
  CHECK(error==std::errc::device_or_resource_busy);
  foreign.reset();
  auto owned=f.device->storage()->ReserveSnapshotFile(old->token(),error);
  REQUIRE(owned);
  auto before=owned->CaptureSnapshot("Account",4096,error,true);
  REQUIRE(before);
  auto service=std::make_shared<DecliningAccountService>(
      f.raw->path()/"Account",owned->directory_identity());
  REQUIRE(f.fs.RegisterManagedFile(service));
  auto intended=f.original;
  auto next=f.account;
  next.gamertag[0]=xe::byte_swap(char16_t('P'));
  xam::ProfileManager::EncryptAccountFile(&next,intended.data());
  auto published=filesystem::SnapshotPublisher::Publish(*f.recovery,before,intended);
  REQUIRE_FALSE(published.error);
  REQUIRE(published.durable);
  REQUIRE(published.current);
  CHECK(published.current->info().file_id!=before->info().file_id);
  CHECK_FALSE(alias->OpenFile("Account",filesystem::FileAccess::kGenericWrite,error));
  CHECK(error==std::errc::device_or_resource_busy);
  CHECK(alias->Create("account",false)==std::errc::device_or_resource_busy);
  CHECK(alias->Remove("Account",false)==std::errc::device_or_resource_busy);
  vfs::File* file=nullptr;
  vfs::FileAction action{};
  CHECK(f.fs.OpenFile(nullptr,"Physical:\\account",vfs::FileDisposition::kSuperscede,
      vfs::FileAccess::kFileWriteData,false,true,&file,&action)==X_STATUS_ACCESS_DENIED);
  CHECK_FALSE(file);
  CHECK(service->entries==1);
  auto* root=f.fs.ResolvePath("Physical:");
  REQUIRE(root);
  CHECK_FALSE(root->CreateEntry("account",vfs::kFileAttributeNormal));
  service->retired=true;
  service.reset();
  owned->Retire();
  REQUIRE_FALSE(owned->ReleaseRetiredSnapshotReservation());
  // Common namespace claim is gone. The actual physical registry tombstone
  // still denies raw guest aliases without relying on the replaced old inode.
  CHECK(f.fs.OpenFile(nullptr,"Physical:\\Account",vfs::FileDisposition::kOverwrite,
      vfs::FileAccess::kFileWriteData,false,true,&file,&action)==X_STATUS_CANCELLED);
  CHECK_FALSE(file);
  CHECK_FALSE(root->CreateEntry("account",vfs::kFileAttributeNormal));
  REQUIRE(f.fs.UnregisterDevice("Physical:"));
  alias.reset();
  CHECK(f.Disk()==intended);
  REQUIRE(::umount(binding.path.c_str())==0);
  binding.mounted=false;
}
#endif
TEST_CASE("Managed registry service last-owner rundown follows the outermost namespace release",
          "[profile_account][registry][lifetime][locks]") {
  AccountFixture f;
  ServiceRundownProbe probe;
  std::atomic<std::shared_ptr<vfs::ManagedFileService>> external;
  auto old=std::make_shared<ReleasingAccountService>(f.raw,external,probe);
  external.store(old);
  REQUIRE(f.fs.RegisterManagedFile(old));
  old->retired.store(true);
  old.reset();
  auto next=std::make_shared<DecliningAccountService>(
      f.raw->path()/"Account",f.raw->directory_identity());
  {
    auto outer=xe::global_critical_region::AcquireDirect();
    CHECK_FALSE(f.fs.RegisterManagedFile(next));
    CHECK_FALSE(probe.destroyed.load());
    CHECK(bool(external.load()));
  }
  REQUIRE(f.fs.RegisterManagedFile(next));
  CHECK_FALSE(external.load());
  CHECK(probe.destroyed.load());
  CHECK_FALSE(probe.under_namespace.load());
}
}  // namespace xe::kernel::test
#endif
