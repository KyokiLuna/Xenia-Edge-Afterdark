/** Xenia : Xbox 360 Emulator Research Project; BSD license, see LICENSE. */
#include "xenia/base/profile_intent_owner.h"
#include "xenia/base/testing/snapshot_fixture.h"
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <fstream>
#include <thread>
#include <chrono>
#if XE_PLATFORM_LINUX
#include <sys/mount.h>
#endif
#if XE_PLATFORM_LINUX
namespace xe::filesystem {
// Test-only negative-I/O seam. Real CAS admission, unlink, and parent fsync
// always run. The injected post-effect error cannot manufacture a token or
// success from an ordinary read. No Kernel, provider callback, or fake fd.
class HostStorageSnapshotTestAccess final {
 public:
  static std::shared_ptr<HostStorageRoot> ReserveTarget(
      const HostStorageSnapshotToken& token, std::error_code& error) {
    std::optional<FileHandleInfo> before;
    if (token.exists()) { before = token.info(); }
    return token.root()->ReserveContentNamespace(token.relative(), before,
                                                false, error);
  }
  static std::shared_ptr<HostStorageRoot> ReserveDirectory(
      const HostStorageRoot& root, const std::filesystem::path& target,
      bool io, std::error_code& error) {
    if (io) { return root.ReserveContentIo(target, error); }
    auto file = root.OpenFile(target, FileAccess::kFileReadData, error);
    if (!file) { return {}; }
    auto info = file->QueryInfo(error);
    file.reset();
    if (!info || error) { return {}; }
    return root.ReserveContentNamespace(target, info, true, error);
  }
  static void ReleaseNamespace(const HostStorageRoot& root) {
    root.ReleaseContentNamespace();
  }
  static HostStorageRoot::ReplaceResult DeleteWithReportedFlushError(
      const HostStorageRoot& root, const HostStorageSnapshotToken& expected,
      std::shared_ptr<const HostStorageSnapshotToken> recovery) {
    return root.CompareAndDeleteSnapshotInternal(
        expected, std::move(recovery),
        std::make_error_code(std::errc::io_error));
  }
};
}  // namespace xe::filesystem
namespace xe::filesystem::test {
namespace {
std::shared_ptr<HostStorageRoot> IntentSource(
    const std::shared_ptr<HostStorageRoot>& parent) {
  REQUIRE_FALSE(parent->Create("content", true));
  std::error_code error;
  auto source = parent->Subroot("content", false, error);
  REQUIRE(source);
  REQUIRE_FALSE(error);
  return source;
}
std::shared_ptr<const ManagedDeviceIdentity> Managed(
    const std::shared_ptr<HostStorageRoot>& source) {
  std::error_code error;
  auto proof =
      ManagedDeviceIdentity::Open(source, "managed-device-A", 1, error);
  REQUIRE(proof);
  REQUIRE_FALSE(error);
  return proof;
}
std::string SourceDirectory(const ManagedDeviceIdentity& source) {
  constexpr char h[] = "0123456789abcdef";
  std::string uuid;
  for (auto b : source.uuid()) {
    uuid += h[b >> 4];
    uuid += h[b & 15];
  }
  return std::string(ProfileIntentOwner::kDirectoryPrefix) + uuid + "-" +
         std::to_string(source.content_epoch());
}
}  // namespace
TEST_CASE("External intent authority permits whole Content guest mount",
          "[profile_intent_owner]") {
  SnapshotScratch scratch;
  auto parent = scratch.Root(), source = IntentSource(parent);
  auto proof = Managed(source);
  auto first = ProfileIntentOwner::Open(parent, proof);
  REQUIRE_FALSE(first->error());
  REQUIRE(first->root()->is_profile_intent_private());
  REQUIRE_FALSE(first->root()->is_recovery_private());
  REQUIRE(first->source_identity().directory);
  std::error_code error;
  REQUIRE_FALSE(first->root()->GuestView(error));
  REQUIRE(error == std::errc::permission_denied);
  auto whole_content = source->GuestView(error);
  REQUIRE(whole_content);
  REQUIRE_FALSE(error);
  // Different external parents cannot establish a SECOND LIVE source owner.
  SnapshotScratch alternate;
  auto competing = ProfileIntentOwner::Open(alternate.Root(), proof);
  REQUIRE(competing->error() == std::errc::device_or_resource_busy);
  competing.reset();
  first.reset();
  auto restarted = ProfileIntentOwner::Open(parent, proof);
  REQUIRE_FALSE(restarted->error());
  auto recovery = SnapshotRecoveryOwner::Open(parent);
  REQUIRE_FALSE(recovery->error());
  REQUIRE(recovery->root()->is_recovery_private());
  REQUIRE_FALSE(recovery->root()->is_profile_intent_private());
}
TEST_CASE("Partial and copied intent ownership markers are never adopted",
          "[profile_intent_owner]") {
  SnapshotScratch scratch;
  auto parent = scratch.Root(), managed = IntentSource(parent);
  auto proof = Managed(managed);
  const auto child = SourceDirectory(*proof);
  REQUIRE_FALSE(parent->Create(child, true));
  auto partial = ProfileIntentOwner::Open(parent, proof);
  REQUIRE(partial->error() == std::errc::bad_message);
  REQUIRE(partial->root());
  REQUIRE_FALSE(std::filesystem::exists(partial->root()->path() / "owner.v1"));
  SnapshotScratch a, b;
  auto a_parent = a.Root(), b_parent = b.Root();
  auto a_source = IntentSource(a_parent), b_source = IntentSource(b_parent);
  auto a_proof = Managed(a_source), b_proof = Managed(b_source);
  auto source = ProfileIntentOwner::Open(a_parent, a_proof);
  REQUIRE_FALSE(source->error());
  const auto destination_leaf = SourceDirectory(*b_proof);
  REQUIRE_FALSE(b_parent->Create(destination_leaf, true));
  std::vector<uint8_t> marker;
  REQUIRE_FALSE(source->root()->ReadFile("owner.v1", 1024, marker));
  std::error_code error;
  auto destination = b_parent->Subroot(destination_leaf, false, error);
  REQUIRE(destination);
  REQUIRE_FALSE(destination->WriteNewFile("owner.v1", marker).error);
  auto foreign = ProfileIntentOwner::Open(b_parent, b_proof);
  REQUIRE(foreign->error() == std::errc::bad_message);
  REQUIRE(foreign->root());
  std::vector<uint8_t> retained;
  REQUIRE_FALSE(destination->ReadFile("owner.v1", 1024, retained));
  REQUIRE(retained == marker);
}
TEST_CASE("Intent namespace permits only new immutable grammar files",
          "[profile_intent_owner]") {
  SnapshotScratch scratch;
  auto parent = scratch.Root(), source = IntentSource(parent);
  auto proof = Managed(source);
  auto owner = ProfileIntentOwner::Open(parent, proof);
  REQUIRE_FALSE(owner->error());
  constexpr auto leaf = "intent-0123456789abcdef0123456789abcdef.bin";
  const std::array<uint8_t, 3> raw{1, 2, 3};
  REQUIRE_FALSE(owner->root()->WriteNewFile(leaf, raw).error);
  std::error_code error;
  REQUIRE_FALSE(owner->root()->OpenFile(leaf, FileAccess::kGenericAll, error));
  REQUIRE(error == std::errc::permission_denied);
  REQUIRE(owner->root()->Remove(leaf, false) == std::errc::permission_denied);
  REQUIRE(owner->root()->Rename(leaf, "owner.v1", true) ==
          std::errc::permission_denied);
  REQUIRE(owner->root()->Create("nested", true) ==
          std::errc::permission_denied);
  REQUIRE(owner->root()->WriteNewFile("unknown.bin", raw).error ==
          std::errc::permission_denied);
  std::vector<uint8_t> retained;
  REQUIRE_FALSE(owner->root()->ReadFile(leaf, 4096, retained));
  REQUIRE(retained == std::vector<uint8_t>{1, 2, 3});
}
TEST_CASE("Intent factory rejects its old child-inside-Content topology",
          "[profile_intent_owner]") {
  SnapshotScratch scratch;
  auto parent = scratch.Root(), source = IntentSource(parent);
  auto proof = Managed(source);
  auto denied = ProfileIntentOwner::Open(source, proof);
  REQUIRE(denied->error() == std::errc::permission_denied);
  REQUIRE_FALSE(denied->root());
  std::error_code error;
  auto whole_content = source->GuestView(error);
  REQUIRE(whole_content);
  REQUIRE_FALSE(error);
}

namespace {
std::shared_ptr<const HostStorageSnapshotToken> DeleteBackup(
    const SnapshotRecoveryOwner& owner, std::span<const uint8_t> bytes) {
  static std::atomic<uint64_t> sequence{1};
  char leaf[64];
  std::snprintf(leaf, sizeof(leaf), "snapshot-%032llx.bin",
                static_cast<unsigned long long>(sequence.fetch_add(1)));
  const auto written = owner.root()->WriteNewFile(leaf, bytes);
  REQUIRE_FALSE(written.error);
  REQUIRE(written.bytes == bytes.size());
  std::error_code error;
  auto token = owner.root()->CaptureSnapshot(leaf, bytes.size(), error);
  REQUIRE_FALSE(error);
  REQUIRE(token);
  REQUIRE(token->exists());
  return token;
}
constexpr std::array<uint8_t, 9> kOpaqueBefore{0, 0xDE, 0xAD, 0, 0xFF,
                                               4, 3,    2,    1};
constexpr std::array<uint8_t, 5> kIntentWire{9, 0, 7, 0xfe, 1};
}  // namespace
TEST_CASE(
    "Typed snapshot deletion preserves raw recovery and never writes an empty "
    "target",
    "[snapshot_delete][profile_intent_owner]") {
  SnapshotFixture f;
  REQUIRE_FALSE(f.profile->WriteNewFile("raw.bin", kOpaqueBefore).error);
  auto before = f.Token("raw.bin");
  const auto result = SnapshotPublisher::Delete(*f.owner, before);
  REQUIRE_FALSE(result.error);
  REQUIRE(result.published);
  REQUIRE(result.durable);
  REQUIRE(result.kind == SnapshotMutationKind::kDelete);
  REQUIRE_FALSE(result.stage_created);
  REQUIRE(result.stage.empty());
  REQUIRE(result.staged_count == 0);
  REQUIRE(result.recovery_durable);
  REQUIRE(result.recovery_count == kOpaqueBefore.size());
  REQUIRE(result.current);
  REQUIRE_FALSE(result.current->exists());
  std::vector<uint8_t> saved;
  REQUIRE_FALSE(f.owner->root()->ReadFile(result.recovery, 4096, saved));
  REQUIRE(saved ==
          std::vector<uint8_t>(kOpaqueBefore.begin(), kOpaqueBefore.end()));
  auto ordinary = f.Token("raw.bin");
  REQUIRE_FALSE(ordinary->exists());
  const auto forbidden = f.profile->RetrySnapshotDurability(ordinary);
  REQUIRE(forbidden.error == std::errc::invalid_argument);
  REQUIRE_FALSE(forbidden.published);
  REQUIRE_FALSE(forbidden.current);
  const auto retry = f.profile->RetrySnapshotDurability(result.current);
  REQUIRE_FALSE(retry.error);
  REQUIRE(retry.published);
  REQUIRE(retry.durable);
}
TEST_CASE(
    "Actual CAS unlink retains its absent publication token after reported "
    "flush failure",
    "[snapshot_delete][durability]") {
  SnapshotFixture f;
  REQUIRE_FALSE(f.profile->WriteNewFile("raw.bin", kOpaqueBefore).error);
  auto before = f.Token("raw.bin"),
       backup = DeleteBackup(*f.owner, before->bytes());
  const auto failed =
      HostStorageSnapshotTestAccess::DeleteWithReportedFlushError(
          *f.profile, *before, backup);
  REQUIRE(failed.error == std::errc::io_error);
  REQUIRE(failed.published);
  REQUIRE_FALSE(failed.durable);
  REQUIRE(failed.current);
  REQUIRE_FALSE(failed.current->exists());
  REQUIRE_FALSE(std::filesystem::exists(f.profile->path() / "raw.bin"));
  auto ordinary = f.Token("raw.bin");
  REQUIRE(f.profile->RetrySnapshotDurability(ordinary).error ==
          std::errc::invalid_argument);
  auto retry = f.profile->RetrySnapshotDurability(failed.current);
  REQUIRE_FALSE(retry.error);
  REQUIRE(retry.published);
  REQUIRE(retry.durable);
  REQUIRE_FALSE(f.profile->WriteNewFile("raw.bin", kOpaqueBefore).error);
  retry = f.profile->RetrySnapshotDurability(failed.current);
  REQUIRE(retry.error == std::errc::state_not_recoverable);
  REQUIRE(retry.published);
  REQUIRE_FALSE(retry.durable);
  REQUIRE(retry.current == failed.current);
  REQUIRE(f.Bytes("raw.bin") ==
          std::vector<uint8_t>(kOpaqueBefore.begin(), kOpaqueBefore.end()));
}
TEST_CASE(
    "Snapshot delete rejects changed target and damaged recovery without "
    "removing either",
    "[snapshot_delete][cas]") {
  SnapshotFixture f;
  REQUIRE_FALSE(f.profile->WriteNewFile("raw.bin", kOpaqueBefore).error);
  auto before = f.Token("raw.bin"),
       backup = DeleteBackup(*f.owner, before->bytes());
  auto changed = kOpaqueBefore;
  changed[0] = 1;
  {
    std::ofstream out(f.profile->path() / "raw.bin",
                      std::ios::binary | std::ios::trunc);
    REQUIRE(out.is_open());
    out.write(reinterpret_cast<const char*>(changed.data()), changed.size());
    out.close();
    REQUIRE_FALSE(out.fail());
  }
  auto result = f.profile->CompareAndDeleteSnapshot(*before, backup);
  REQUIRE(result.error == std::errc::state_not_recoverable);
  REQUIRE_FALSE(result.published);
  REQUIRE_FALSE(result.current);
  REQUIRE(f.Bytes("raw.bin") ==
          std::vector<uint8_t>(changed.begin(), changed.end()));
  {
    std::ofstream out(f.profile->path() / "raw.bin",
                      std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(kOpaqueBefore.data()),
              kOpaqueBefore.size());
    out.close();
    REQUIRE_FALSE(out.fail());
    std::ofstream damaged(f.owner->root()->path() / backup->relative(),
                          std::ios::binary | std::ios::trunc);
    damaged.write(reinterpret_cast<const char*>(changed.data()),
                  changed.size());
    damaged.close();
    REQUIRE_FALSE(damaged.fail());
  }
  result = f.profile->CompareAndDeleteSnapshot(*before, backup);
  REQUIRE(result.error == std::errc::state_not_recoverable);
  REQUIRE_FALSE(result.published);
  REQUIRE_FALSE(result.current);
  REQUIRE(f.Bytes("raw.bin") ==
          std::vector<uint8_t>(kOpaqueBefore.begin(), kOpaqueBefore.end()));
}
TEST_CASE(
    "Deleted names with live actual file leases remain busy for CAS and batch "
    "currentness",
    "[snapshot_delete][snapshot_batch][lifetime]") {
  SnapshotFixture f;
  REQUIRE_FALSE(f.profile->WriteNewFile("raw.bin", kOpaqueBefore).error);
  auto before = f.Token("raw.bin"),
       backup = DeleteBackup(*f.owner, before->bytes());
  std::error_code error;
  auto held = f.profile->OpenFile("raw.bin", FileAccess::kGenericRead, error);
  REQUIRE(held);
  REQUIRE_FALSE(error);
  auto denied = f.profile->CompareAndDeleteSnapshot(*before, backup);
  REQUIRE(denied.error == std::errc::device_or_resource_busy);
  REQUIRE_FALSE(denied.published);
  // Owned fixture only: simulate an external unlink while its common lease
  // remains live. The missing path is not proof that the name is unreserved.
  REQUIRE(std::filesystem::remove(f.profile->path() / "raw.bin"));
  auto absent = f.Token("raw.bin");
  REQUIRE_FALSE(absent->exists());
  auto batch = f.profile->ValidateSnapshotBatch({absent}, error);
  REQUIRE_FALSE(batch);
  REQUIRE(error == std::errc::device_or_resource_busy);
  held.reset();
  batch = f.profile->ValidateSnapshotBatch({absent}, error);
  REQUIRE(batch);
  REQUIRE_FALSE(error);
  REQUIRE(batch->metadata().size() == 1);
  REQUIRE_FALSE(batch->metadata()[0]);
  batch.reset();
  REQUIRE(f.profile->RetrySnapshotDurability(absent).error ==
          std::errc::invalid_argument);
}
TEST_CASE(
    "Snapshot delete preserves originals on cancellation readonly and source "
    "retirement",
    "[snapshot_delete][admission]") {
  SnapshotFixture f;
  REQUIRE_FALSE(f.profile->WriteNewFile("raw.bin", kOpaqueBefore).error);
  auto before = f.Token("raw.bin");
  std::atomic<bool> canceled{true};
  auto denied = SnapshotPublisher::Delete(*f.owner, before, &canceled);
  REQUIRE(denied.error == std::errc::operation_canceled);
  REQUIRE_FALSE(denied.recovery_created);
  REQUIRE_FALSE(denied.published);
  REQUIRE(f.Bytes("raw.bin") ==
          std::vector<uint8_t>(kOpaqueBefore.begin(), kOpaqueBefore.end()));
  std::error_code error;
  auto readonly = f.parent->Subroot("profile", true, error);
  REQUIRE(readonly);
  auto ro_token = readonly->CaptureSnapshot("raw.bin", 4096, error);
  REQUIRE(ro_token);
  denied = SnapshotPublisher::Delete(*f.owner, ro_token);
  REQUIRE(denied.error);
  REQUIRE_FALSE(denied.published);
  auto backup = DeleteBackup(*f.owner, before->bytes());
  f.profile->Retire();
  const auto retired = f.profile->CompareAndDeleteSnapshot(*before, backup);
  REQUIRE(retired.error);
  REQUIRE_FALSE(retired.published);
  REQUIRE(std::filesystem::is_regular_file(f.profile->path() / "raw.bin"));
}
TEST_CASE(
    "Snapshot batches cover a whole profile and reject bounded aggregate "
    "overflow",
    "[snapshot_batch][bounds]") {
  SnapshotFixture f;
  std::error_code error;
  std::vector<std::shared_ptr<const HostStorageSnapshotToken>> tokens;
  for (unsigned i = 0; i != 32; ++i) {
    const auto name = "file-" + std::to_string(i) + ".bin";
    REQUIRE_FALSE(f.profile->WriteNewFile(name, kOpaqueBefore).error);
    tokens.push_back(f.Token(name));
  }
  auto batch = f.profile->ValidateSnapshotBatch(tokens, error);
  REQUIRE(batch);
  REQUIRE_FALSE(error);
  REQUIRE(batch->tokens().size() == 32);
  REQUIRE(std::all_of(batch->metadata().begin(), batch->metadata().end(),
                      [](const auto& row) { return row.has_value(); }));
  batch.reset();
  std::vector<std::shared_ptr<const HostStorageSnapshotToken>> excessive(
      HostStorageRoot::kMaximumSnapshotBatchTokens + 1, tokens.front());
  REQUIRE_FALSE(f.profile->ValidateSnapshotBatch(excessive, error));
  REQUIRE(error == std::errc::invalid_argument);
  std::vector<uint8_t> bounded(65537, 0xA5);
  REQUIRE_FALSE(f.profile->WriteNewFile("large.bin", bounded).error);
  auto large = f.Token("large.bin", bounded.size());
  excessive.assign(HostStorageRoot::kMaximumSnapshotBatchTokens, large);
  REQUIRE_FALSE(f.profile->ValidateSnapshotBatch(excessive, error));
  REQUIRE(error == std::errc::file_too_large);
}

TEST_CASE(
    "Conditional protected intent creation retains actual batch admission",
    "[profile_intent_owner][snapshot_batch]") {
  SnapshotScratch scratch;
  auto parent = scratch.Root(), source = IntentSource(parent);
  auto proof = Managed(source);
  auto owner = ProfileIntentOwner::Open(parent, proof);
  REQUIRE_FALSE(owner->error());
  REQUIRE_FALSE(source->WriteNewFile("before.bin", kOpaqueBefore).error);
  std::error_code error;
  auto token = source->CaptureSnapshot("before.bin", 4096, error);
  REQUIRE(token);
  auto batch = source->ValidateSnapshotBatch({token}, error);
  REQUIRE(batch);
  constexpr auto leaf = "intent-00000000000000000000000000000001.bin";
  const auto write =
      owner->root()->WriteNewIntentWithSnapshotGuard(*batch, leaf, kIntentWire);
  REQUIRE_FALSE(write.error);
  REQUIRE(write.created);
  REQUIRE(write.durable);
  REQUIRE(write.bytes == kIntentWire.size());
  const auto duplicate = owner->root()->WriteNewIntentWithSnapshotGuard(
      *batch, leaf, kOpaqueBefore);
  REQUIRE(duplicate.error == std::errc::file_exists);
  REQUIRE_FALSE(duplicate.created);
  REQUIRE_FALSE(duplicate.durable);
  batch.reset();
  std::vector<uint8_t> bytes;
  REQUIRE_FALSE(owner->root()->ReadFile(leaf, 4096, bytes));
  REQUIRE(std::equal(bytes.begin(), bytes.end(), kIntentWire.begin(),
                     kIntentWire.end()));
}

TEST_CASE("Cross-root immutable journal coexists with a persistent Account reservation",
          "[profile_intent_owner][snapshot_batch][namespace][origin]") {
  SnapshotScratch scratch;
  auto parent = scratch.Root(), source = IntentSource(parent);
  auto proof = Managed(source);
  auto owner = ProfileIntentOwner::Open(parent, proof);
  REQUIRE_FALSE(owner->error());
  REQUIRE_FALSE(source->WriteNewFile("Account", kOpaqueBefore).error);
  REQUIRE_FALSE(source->WriteNewFile("before.bin", kOpaqueBefore).error);
  std::error_code error;
  auto account = source->CaptureSnapshot("Account", 4096, error);
  REQUIRE(account);
  auto reserved = source->ReserveSnapshotFile(account, error);
  REQUIRE(reserved);
  REQUIRE_FALSE(error);
  auto token = reserved->CaptureSnapshot("before.bin", 4096, error);
  REQUIRE(token);
  constexpr auto leaf = "intent-00000000000000000000000000000004.bin";
  auto space = HostStorageSpace::ForPath(source->path(), 0, error);
  REQUIRE(space);
  {
    auto unbound = space->Begin(source->path(), error);
    REQUIRE(unbound);
    // A missing query origin must still fail closed against the live Account
    // namespace. The journal fix supplies its real root, never a waiver.
    REQUIRE(unbound->Conflicts(owner->root()->path() / leaf, false, 0, 0));
    REQUIRE_FALSE(unbound->TrackOpen(
        owner->root()->path() / leaf, account->info(), true, false, {}, false,
        false, owner->root()->directory_identity(), "../invalid"));
  }
  {
    auto known = space->Begin(source->path(), error, source->directory_identity());
    REQUIRE(known);
    REQUIRE_FALSE(known->Conflicts(owner->root()->path() / leaf, false, 0, 0,
        {}, false, owner->root()->directory_identity(), leaf));
    REQUIRE(known->Conflicts(source->path() / "Account", false,
        account->info().filesystem_id, account->info().file_id, {}, false,
        source->directory_identity(), "Account"));
    // Real retained inode identity also denies a differently spelled query.
    REQUIRE(known->Conflicts(owner->root()->path() / leaf, false,
        account->info().filesystem_id, account->info().file_id, {}, false,
        owner->root()->directory_identity(), leaf));
  }
  auto batch = reserved->ValidateSnapshotBatch({token}, error);
  REQUIRE(batch);
  const auto write = owner->root()->WriteNewIntentWithSnapshotGuard(
      *batch, leaf, kIntentWire);
  REQUIRE_FALSE(write.error);
  REQUIRE(write.created);
  REQUIRE(write.durable);
  REQUIRE(write.bytes == kIntentWire.size());
  const auto duplicate = owner->root()->WriteNewIntentWithSnapshotGuard(
      *batch, leaf, kOpaqueBefore);
  REQUIRE(duplicate.error == std::errc::file_exists);
  REQUIRE_FALSE(duplicate.created);
  batch.reset();
  std::vector<uint8_t> bytes;
  REQUIRE_FALSE(owner->root()->ReadFile(leaf, 4096, bytes));
  REQUIRE(std::equal(bytes.begin(), bytes.end(), kIntentWire.begin(),
                     kIntentWire.end()));
  REQUIRE_FALSE(reserved->ReadFile("Account", 4096, bytes));
  REQUIRE(std::equal(bytes.begin(), bytes.end(), kOpaqueBefore.begin(),
                     kOpaqueBefore.end()));
  auto denied = source->OpenFile("Account", FileAccess::kFileWriteData, error);
  REQUIRE_FALSE(denied);
  REQUIRE(error == std::errc::device_or_resource_busy);
}

TEST_CASE("Conditional intent rejects retired and foreign-thread batch without "
          "creation",
          "[profile_intent_owner][snapshot_batch]") {
  SnapshotScratch scratch;
  auto parent = scratch.Root(), source = IntentSource(parent);
  auto proof = Managed(source);
  auto owner = ProfileIntentOwner::Open(parent, proof);
  REQUIRE_FALSE(owner->error());
  // Use an independent actual view so retiring it does not retire the owner.
  std::error_code error;
  auto target = source->WithDependency(source, error);
  REQUIRE(target);
  auto token = target->CaptureSnapshot("missing.bin", 4096, error);
  REQUIRE(token);
  auto batch = target->ValidateSnapshotBatch({token}, error);
  REQUIRE(batch);
  constexpr auto leaf = "intent-00000000000000000000000000000002.bin";
  HostStorageRoot::IntentWriteResult foreign;
  std::thread caller([&] {
    foreign = owner->root()->WriteNewIntentWithSnapshotGuard(*batch, leaf,
                                                             kIntentWire);
  });
  caller.join();
  REQUIRE(foreign.error == std::errc::operation_not_permitted);
  REQUIRE_FALSE(foreign.created);
  target->Retire();
  const auto retired =
      owner->root()->WriteNewIntentWithSnapshotGuard(*batch, leaf, kIntentWire);
  REQUIRE(retired.error == std::errc::no_such_device);
  REQUIRE_FALSE(retired.created);
  batch.reset();
  REQUIRE_FALSE(std::filesystem::exists(owner->root()->path() / leaf));
}
TEST_CASE("Conditional intent refuses a genuine different-filesystem batch",
          "[profile_intent_owner][snapshot_batch]") {
  SnapshotScratch scratch;
  auto parent = scratch.Root(), source = IntentSource(parent);
  auto proof = Managed(source);
  auto owner = ProfileIntentOwner::Open(parent, proof);
  REQUIRE_FALSE(owner->error());
  std::error_code error;
  auto other = HostStorageRoot::Open("/dev/shm", false, error);
  REQUIRE(other); // Linux test prerequisite: actual independent tmpfs root.
  REQUIRE_FALSE(error);
  auto token =
      other->CaptureSnapshot("xenia-conditional-intent-absent", 1, error);
  REQUIRE(token);
  REQUIRE_FALSE(token->exists());
  auto batch = other->ValidateSnapshotBatch({token}, error);
  REQUIRE(batch);
  constexpr auto leaf = "intent-00000000000000000000000000000003.bin";
  const auto result =
      owner->root()->WriteNewIntentWithSnapshotGuard(*batch, leaf, kIntentWire);
  REQUIRE(result.error == std::errc::cross_device_link);
  REQUIRE_FALSE(result.created);
  REQUIRE_FALSE(result.durable);
  batch.reset();
  REQUIRE_FALSE(std::filesystem::exists(owner->root()->path() / leaf));
}

namespace {
// Observe only this test process's genuine descriptors. No fake FileHandleInfo
// or allocator-reuse assumption can stand in for an actual retained inode.
std::vector<int> NativePins(const FileHandleInfo& expected) {
  std::error_code error;
  std::filesystem::directory_iterator entries("/proc/self/fd", error), end;
  REQUIRE_FALSE(error);
  std::vector<int> matches;
  size_t count = 0;
  for (; entries != end; entries.increment(error)) {
    REQUIRE_FALSE(error);
    REQUIRE(++count <= 4096);
    const auto text = entries->path().filename().string();
    if (text.empty() || text.size() > 9 ||
        !std::all_of(text.begin(), text.end(), [](char c) {
          return c >= '0' && c <= '9';
        })) { continue; }
    const int fd = std::stoi(text);
    struct stat info;
    if (!fstat(fd, &info) && uint64_t(info.st_dev) == expected.filesystem_id &&
        uint64_t(info.st_ino) == expected.file_id) {
      matches.push_back(fd);
    }
  }
  REQUIRE_FALSE(error);
  return matches;
}
void CheckPublishedNamespaceAlias(
    SnapshotFixture& f, const std::shared_ptr<HostStorageRoot>& reserved,
    const SnapshotPublicationReceipt& receipt, std::span<const uint8_t> bytes) {
  REQUIRE_FALSE(receipt.error);
  REQUIRE(receipt.published);
  REQUIRE(receipt.durable);
  REQUIRE(receipt.current);
  REQUIRE(receipt.current->exists());
  REQUIRE(receipt.current->root() == reserved);
  // SnapshotPublisher::Publish has returned: its staged FileHandle and its
  // exclusive file lease are gone. Only the retained namespace can deny this
  // new physical alias of the replacement inode.
  const auto alias = f.parent->path() / "replacement-alias";
  REQUIRE(link((f.profile->path() / "data").c_str(), alias.c_str()) == 0);
  std::error_code error;
  REQUIRE_FALSE(f.parent->OpenFile("replacement-alias",
                                  FileAccess::kFileReadData, error));
  REQUIRE(error == std::errc::device_or_resource_busy);
  std::vector<uint8_t> actual;
  REQUIRE_FALSE(reserved->ReadFile("data", 4096, actual));
  REQUIRE(actual == std::vector<uint8_t>(bytes.begin(), bytes.end()));
  // An unrelated real allocation is not part of the reserved target. The V47
  // failure rejected exactly this kind of durable Done publication after CAS.
  const std::array<uint8_t, 4> done{9, 0, 7, 6};
  const auto written = f.parent->WriteNewFile("unrelated.done", done);
  REQUIRE_FALSE(written.error);
  REQUIRE(written.bytes == done.size());
  REQUIRE_FALSE(f.parent->ReadFile("unrelated.done", 4096, actual));
  REQUIRE(actual == std::vector<uint8_t>(done.begin(), done.end()));
  HostStorageSnapshotTestAccess::ReleaseNamespace(*reserved);
  auto detached = f.parent->OpenFile("replacement-alias",
                                     FileAccess::kFileReadData, error);
  REQUIRE(detached);
  REQUIRE_FALSE(error);
  // A genuine File lease remains authoritative even after its old name is
  // unlinked. Do not fix namespace reuse by ignoring detached native handles.
  REQUIRE(unlink(alias.c_str()) == 0);
  REQUIRE(f.profile->Remove("data", false) ==
          std::errc::device_or_resource_busy);
  detached.reset();
  REQUIRE_FALSE(f.profile->Remove("data", false));
}
}  // namespace
TEST_CASE("Namespace reservations retain real native identity pins until rundown",
          "[snapshot_publication][namespace_identity]") {
  SnapshotFixture f;
  std::error_code error;
  std::filesystem::path target;
  FileHandleInfo expected;
  std::shared_ptr<HostStorageRoot> reserved;
  SECTION("Existing regular target") {
    target = "data";
    REQUIRE_FALSE(f.profile->WriteNewFile(target, kOpaqueBefore).error);
    const auto token = f.Token(target);
    expected = token->info();
    reserved = HostStorageSnapshotTestAccess::ReserveTarget(*token, error);
  }
  SECTION("Existing directory namespace") {
    target = "directory";
    REQUIRE_FALSE(f.profile->Create(target, true));
    reserved = HostStorageSnapshotTestAccess::ReserveDirectory(
        *f.profile, target, false, error);
  }
  SECTION("Actual package I/O directory") {
    target = "directory";
    REQUIRE_FALSE(f.profile->Create(target, true));
    reserved = HostStorageSnapshotTestAccess::ReserveDirectory(
        *f.profile, target, true, error);
  }
  SECTION("An owning File lease outlives its original handle and root") {
    // A distinct, real queried Space makes the lifetime assertion independent
    // of the process-wide project Space. It grants no managed/private owner.
    auto space = HostStorageSpace::ForPath(f.profile->path(), 0, error,
        [](const auto& path, auto& error) {
          return std::filesystem::space(path, error);
        });
    REQUIRE(space);
    REQUIRE_FALSE(error);
    std::weak_ptr<HostStorageSpace> weak_space = space;
    auto source = HostStorageRoot::Open(f.profile->path(), false, error, 0, space);
    REQUIRE(source);
    REQUIRE_FALSE(error);
    REQUIRE_FALSE(source->WriteNewFile("bare", kOpaqueBefore).error);
    auto file = source->OpenFile("bare", FileAccess::kFileReadData, error);
    REQUIRE(file);
    REQUIRE_FALSE(error);
    const auto info = file->QueryInfo(error);
    REQUIRE(info);
    REQUIRE_FALSE(error);
    auto bare_lease = file->storage_lease();
    REQUIRE(bare_lease);
    REQUIRE(link((source->path() / "bare").c_str(),
                 (source->path() / "bare-alias").c_str()) == 0);
    REQUIRE(unlink((source->path() / "bare").c_str()) == 0);
    file.reset();
    source.reset();
    space.reset();
    REQUIRE_FALSE(weak_space.expired());
    const auto pins = NativePins(*info);
    REQUIRE_FALSE(pins.empty());
    auto retained_space = weak_space.lock();
    auto foreign = HostStorageRoot::Open(f.profile->path(), false, error, 0,
                                         retained_space);
    REQUIRE(foreign);
    REQUIRE_FALSE(error);
    REQUIRE(foreign->Remove("bare-alias", false) ==
            std::errc::device_or_resource_busy);
    bare_lease.reset();
    for (const int fd : pins) {
      struct stat released;
      errno = 0;
      REQUIRE(fstat(fd, &released) == -1);
      REQUIRE(errno == EBADF);
    }
    REQUIRE_FALSE(foreign->Remove("bare-alias", false));
    foreign.reset();
    retained_space.reset();
    REQUIRE(weak_space.expired());
    return;
  }
  REQUIRE(reserved);
  REQUIRE_FALSE(error);
  if (!expected.file_id) {
    auto file = reserved->OpenFile(target, FileAccess::kFileReadData, error);
    REQUIRE(file);
    REQUIRE_FALSE(error);
    const auto info = file->QueryInfo(error);
    REQUIRE(info);
    REQUIRE_FALSE(error);
    expected = *info;
  }
  const auto pins = NativePins(expected);
  REQUIRE_FALSE(pins.empty());
  REQUIRE((expected.directory ? rmdir((f.profile->path() / target).c_str())
                              : unlink((f.profile->path() / target).c_str())) == 0);
  for (const int fd : pins) {
    struct stat retained;
    REQUIRE(fstat(fd, &retained) == 0);
    REQUIRE(uint64_t(retained.st_dev) == expected.filesystem_id);
    REQUIRE(uint64_t(retained.st_ino) == expected.file_id);
    REQUIRE(retained.st_nlink == 0);
  }
  HostStorageSnapshotTestAccess::ReleaseNamespace(*reserved);
  // Release closes admission, not borrowed view lifetime. Native pins cannot
  // disappear while the actual reservation object is still retained.
  for (const int fd : pins) {
    struct stat retained;
    REQUIRE(fstat(fd, &retained) == 0);
  }
  reserved.reset();
  // No filesystem opens occur between last-owner destruction and this fstat.
  for (const int fd : pins) {
    struct stat released;
    errno = 0;
    REQUIRE(fstat(fd, &released) == -1);
    REQUIRE(errno == EBADF);
  }
  const auto written = f.profile->WriteNewFile(target, kOpaqueBefore);
  REQUIRE_FALSE(written.error);
  REQUIRE(written.bytes == kOpaqueBefore.size());
}
TEST_CASE("Namespace CAS follows replacement aliases without reserving unrelated files",
          "[snapshot_publication][namespace_identity]") {
  SnapshotFixture f;
  REQUIRE_FALSE(f.profile->WriteNewFile("data", kOpaqueBefore).error);
  const auto before = f.Token("data");
  std::error_code error;
  auto reserved = HostStorageSnapshotTestAccess::ReserveTarget(*before, error);
  REQUIRE(reserved);
  REQUIRE_FALSE(error);
  auto expected = reserved->CaptureSnapshot("data", 4096, error);
  REQUIRE(expected);
  REQUIRE_FALSE(error);
  const std::array<uint8_t, 5> next{4, 0, 9, 255, 3};
  const auto receipt = SnapshotPublisher::Publish(*f.owner, expected, next);
  CheckPublishedNamespaceAlias(f, reserved, receipt, next);
}
TEST_CASE("Absent namespace adopts an inode only after real CAS publication",
          "[snapshot_publication][namespace_identity]") {
  SnapshotFixture f;
  const auto before = f.Token("data");
  REQUIRE_FALSE(before->exists());
  std::error_code error;
  auto reserved = HostStorageSnapshotTestAccess::ReserveTarget(*before, error);
  REQUIRE(reserved);
  REQUIRE_FALSE(error);
  auto expected = reserved->CaptureSnapshot("data", 4096, error);
  REQUIRE(expected);
  REQUIRE_FALSE(error);
  REQUIRE_FALSE(expected->exists());
  const std::array<uint8_t, 5> next{4, 0, 9, 255, 3};
  const auto receipt = SnapshotPublisher::Publish(*f.owner, expected, next);
  REQUIRE_FALSE(receipt.recovery_created);
  CheckPublishedNamespaceAlias(f, reserved, receipt, next);
}


TEST_CASE("Existing native file IO needs a known directory coordinate during content IO reservation",
          "[profile_intent_owner][namespace][existing_io][origin]") {
  SnapshotScratch scratch;
  auto parent=scratch.Root();
  REQUIRE_FALSE(parent->Create("package",true));
  std::error_code error;
  auto package=parent->Subroot("package",false,error);
  REQUIRE(package);
  const std::array<uint8_t,4> initial{1,2,3,4}, changed{9,8,7,6};
  REQUIRE_FALSE(package->WriteNewFile("save",initial).error);
  auto existing=package->OpenFile("save",FileAccess::kGenericAll,error);
  REQUIRE(existing);
  const auto info=existing->QueryInfo(error);
  REQUIRE(info);
  auto io=HostStorageSnapshotTestAccess::ReserveDirectory(*parent,"package",true,error);
  REQUIRE(io);
  auto space=HostStorageSpace::ForPath(package->path(),0,error);
  REQUIRE(space);
  {
    auto unbound=space->Begin(package->path(),error);
    REQUIRE(unbound);
    CHECK(unbound->Conflicts(package->path()/"save",false,info->filesystem_id,
        info->file_id,existing->storage_lease(),true));
  }
  {
    auto known=space->Begin(package->path(),error,package->directory_identity());
    REQUIRE(known);
    CHECK_FALSE(known->Conflicts(package->path()/"save",false,info->filesystem_id,
        info->file_id,existing->storage_lease(),true));
  }
  // This is the actual PolicyFileHandle write path under its genuine Root
  // origin and preexisting native lease, not only an abstract ledger query.
  auto written=existing->WriteResult(0,changed);
  REQUIRE_FALSE(written.error);
  REQUIRE(written.bytes==changed.size());
  REQUIRE_FALSE(existing->FlushResult());
  existing.reset();
  HostStorageSnapshotTestAccess::ReleaseNamespace(*io);
  std::vector<uint8_t> actual;
  REQUIRE_FALSE(package->ReadFile("save",4,actual));
  CHECK(std::equal(changed.begin(),changed.end(),actual.begin(),actual.end()));
}
TEST_CASE("Directory namespace reservation inventories actual foreign child leases before publication",
          "[profile_intent_owner][namespace][child_lease]") {
  SnapshotScratch scratch;
  auto parent=scratch.Root();
  REQUIRE_FALSE(parent->Create("package",true));
  REQUIRE_FALSE(parent->WriteNewFile("package/save",std::array<uint8_t,1>{7}).error);
  std::error_code error;
  auto child=parent->Subroot("package",false,error);
  REQUIRE(child);
  auto foreign=child->OpenFile("save",FileAccess::kGenericRead,error);
  REQUIRE(foreign);
  CHECK_FALSE(HostStorageSnapshotTestAccess::ReserveDirectory(*parent,"package",false,error));
  CHECK(error==std::errc::device_or_resource_busy);
  foreign.reset();
  auto actual=HostStorageSnapshotTestAccess::ReserveDirectory(*parent,"package",false,error);
  REQUIRE(actual);
  REQUIRE_FALSE(error);
  HostStorageSnapshotTestAccess::ReleaseNamespace(*actual);
}
TEST_CASE("Deep real bind alias cannot hide an ordinary child from structural namespace admission",
          "[.requires_bind_mount][profile_intent_owner][namespace][child_alias]") {
  SnapshotScratch scratch;
  auto parent=scratch.Root();
  REQUIRE_FALSE(parent->Create("package",true));
  REQUIRE_FALSE(parent->Create("package/nested",true));
  REQUIRE_FALSE(parent->WriteNewFile("package/nested/save",std::array<uint8_t,1>{3}).error);
  REQUIRE_FALSE(parent->Create("alias",true));
  struct Bind {
    std::filesystem::path path;
    bool active=false;
    ~Bind() { if (active && ::umount(path.c_str())) { ::umount2(path.c_str(),MNT_DETACH); } }
  } bind{parent->path()/"alias"};
  const int mounted=::mount((parent->path()/"package/nested").c_str(),
      bind.path.c_str(),nullptr,MS_BIND,nullptr);
  INFO("real isolated bind mount returned " << mounted << ", errno " << errno);
  REQUIRE(mounted==0);
  bind.active=true;
  std::error_code error;
  auto alias=HostStorageRoot::Open(bind.path,false,error);
  REQUIRE(alias);
  auto foreign=alias->OpenFile("save",FileAccess::kGenericRead,error);
  REQUIRE(foreign);
  // Directory inode != child inode and all native path spellings differ.
  CHECK_FALSE(HostStorageSnapshotTestAccess::ReserveDirectory(*parent,"package",false,error));
  CHECK(error==std::errc::device_or_resource_busy);
  foreign.reset();
  auto reserved=HostStorageSnapshotTestAccess::ReserveDirectory(*parent,"package",false,error);
  REQUIRE(reserved);
  REQUIRE_FALSE(error);
  CHECK_FALSE(alias->OpenFile("save",FileAccess::kGenericRead,error));
  CHECK(error==std::errc::device_or_resource_busy);
  HostStorageSnapshotTestAccess::ReleaseNamespace(*reserved);
  auto reopened=alias->OpenFile("save",FileAccess::kGenericRead,error);
  REQUIRE(reopened);
  reopened.reset();
  alias.reset();
  REQUIRE(::umount(bind.path.c_str())==0);
  bind.active=false;
}

TEST_CASE("Cross-filesystem bind parent cannot hide same-final-filesystem namespace ancestry",
          "[.requires_bind_mount][.requires_two_filesystems][profile_intent_owner][namespace][cross_fs_alias]") {
  SnapshotScratch scratch;
  auto alias_parent=scratch.Root();
  struct Source {
    std::filesystem::path path;
    ~Source() { if (!path.empty()) { std::error_code e; std::filesystem::remove_all(path,e); } }
  } source;
  static std::atomic<uint64_t> sequence{0};
  for (size_t n=0;n!=16;++n) {
    auto candidate=std::filesystem::path("/dev/shm")/
        ("xenia-native-alias-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+
         "-"+std::to_string(sequence.fetch_add(1)));
    std::error_code error;
    if (std::filesystem::create_directory(candidate,error)) { source.path=std::move(candidate); break; }
  }
  REQUIRE_FALSE(source.path.empty());  // Genuine separate small fixture FS.
  struct stat a{},b{};
  REQUIRE(::stat(source.path.c_str(),&a)==0);
  REQUIRE(::stat(alias_parent->path().c_str(),&b)==0);
  INFO("This case requires distinct source and mount-parent filesystems");
  REQUIRE(a.st_dev!=b.st_dev);
  std::error_code error;
  auto native_source=HostStorageRoot::Open(source.path,false,error);
  REQUIRE(native_source);
  REQUIRE_FALSE(native_source->Create("package",true));
  REQUIRE_FALSE(native_source->Create("package/nested",true));
  REQUIRE_FALSE(native_source->WriteNewFile("package/nested/save",std::array<uint8_t,1>{4}).error);
  REQUIRE_FALSE(alias_parent->Create("alias",true));
  struct Bind {
    std::filesystem::path path;
    bool active=false;
    ~Bind() { if (active && ::umount(path.c_str())) { ::umount2(path.c_str(),MNT_DETACH); } }
  } bind{alias_parent->path()/"alias"};
  const int mounted=::mount((native_source->path()/"package/nested").c_str(),
      bind.path.c_str(),nullptr,MS_BIND,nullptr);
  INFO("real cross-filesystem-parent bind returned " << mounted << ", errno " << errno);
  REQUIRE(mounted==0);
  bind.active=true;
  auto alias=HostStorageRoot::Open(bind.path,false,error);
  REQUIRE(alias);
  auto reserved=HostStorageSnapshotTestAccess::ReserveDirectory(*native_source,"package",false,error);
  REQUIRE(reserved);
  REQUIRE_FALSE(error);
  // The alias root and namespace are on source FS_A; the mountpoint parent is
  // FS_B. Neither exact known lineage contains the other's final root.
  CHECK_FALSE(alias->OpenFile("save",FileAccess::kGenericRead,error));
  CHECK(error==std::errc::device_or_resource_busy);
  HostStorageSnapshotTestAccess::ReleaseNamespace(*reserved);
  auto read=alias->OpenFile("save",FileAccess::kGenericRead,error);
  REQUIRE(read);
  read.reset();
  alias.reset();
  REQUIRE(::umount(bind.path.c_str())==0);
  bind.active=false;
}
}  // namespace xe::filesystem::test
#endif
