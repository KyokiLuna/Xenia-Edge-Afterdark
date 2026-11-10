/** Xenia : Xbox 360 Emulator Research Project; BSD license, see LICENSE. */
#include "xenia/base/testing/snapshot_fixture.h"
#include <cstdio>
#include <fstream>
#if XE_PLATFORM_LINUX
namespace xe::filesystem::test {
TEST_CASE("Snapshot publication preserves exact old bytes in a private copy",
          "[snapshot_publication]") {
  SnapshotFixture f;
  const std::array<uint8_t, 5> old{0, 0xff, 4, 0, 9}, next{7, 8, 9, 0, 2};
  REQUIRE_FALSE(f.profile->WriteNewFile("data", old).error);
  const auto old_token = f.Token("data");
  auto receipt = SnapshotPublisher::Publish(*f.owner, old_token, next);
  REQUIRE_FALSE(receipt.error);
  REQUIRE(receipt.staged_count == next.size());
  REQUIRE(receipt.recovery_count == old.size());
  REQUIRE(receipt.recovery_created);
  REQUIRE(receipt.recovery_durable);
  REQUIRE(receipt.published);
  REQUIRE(receipt.durable);
  REQUIRE(receipt.current);
  REQUIRE(receipt.current->info().file_id != old_token->info().file_id);
  REQUIRE(f.Bytes("data") == std::vector<uint8_t>(next.begin(), next.end()));
  std::vector<uint8_t> recovery;
  REQUIRE_FALSE(f.owner->root()->ReadFile(receipt.recovery, 4096, recovery));
  REQUIRE(recovery == std::vector<uint8_t>(old.begin(), old.end()));
  struct stat info;
  REQUIRE(stat((f.owner->root()->path() / receipt.recovery).c_str(), &info) == 0);
  REQUIRE((info.st_mode & 077) == 0);
  REQUIRE(info.st_nlink == 1);
  REQUIRE(uint64_t(info.st_ino) != old_token->info().file_id);
  REQUIRE_FALSE(f.profile->Observe().error);
}
TEST_CASE("Absent snapshot creation needs no invented old backup",
          "[snapshot_publication]") {
  SnapshotFixture f;
  auto missing = f.Token("new");
  REQUIRE_FALSE(missing->exists());
  const std::array<uint8_t, 2> bytes{3, 4};
  auto receipt = SnapshotPublisher::Publish(*f.owner, missing, bytes);
  REQUIRE_FALSE(receipt.error);
  REQUIRE(receipt.published);
  REQUIRE(receipt.durable);
  REQUIRE_FALSE(receipt.recovery_created);
  REQUIRE(receipt.recovery.empty());
  REQUIRE(f.Bytes("new") == std::vector<uint8_t>{3, 4});
  REQUIRE_FALSE(f.profile->RetrySnapshotDurability(receipt.current).error);
}
TEST_CASE("Snapshot CAS rejects same-inode changed content and preserves it",
          "[snapshot_publication]") {
  SnapshotFixture f;
  const std::array<uint8_t, 2> old{1, 2}, next{8, 9};
  std::vector<uint8_t> competing;
  SECTION("same length still compares bytes") { competing = {4, 5}; }
  SECTION("native growth is a conflict rather than a capture limit") {
    competing = {4, 5, 6};
  }
  SECTION("native shrink is a conflict too") { competing = {4}; }
  REQUIRE_FALSE(f.profile->WriteNewFile("data", old).error);
  auto token = f.Token("data");
  std::error_code error;
  auto edit = f.profile->OpenFile("data", FileAccess::kGenericWrite, error);
  REQUIRE(edit);
  REQUIRE_FALSE(edit->ResizeResult(competing.size()));
  REQUIRE_FALSE(edit->WriteResult(0, competing).error);
  REQUIRE_FALSE(edit->FlushResult());
  edit.reset();
  REQUIRE_FALSE(f.profile->ValidateSnapshotBatch({token}, error));
  REQUIRE(error == std::errc::state_not_recoverable);
  auto current = f.Token("data");
  REQUIRE(current->info().file_id == token->info().file_id);
  REQUIRE(std::vector<uint8_t>(current->bytes().begin(), current->bytes().end()) ==
          competing);
  {
    auto checked = f.profile->ValidateSnapshotBatch({current}, error);
    REQUIRE(checked);
    REQUIRE_FALSE(error);
  }
  auto receipt = SnapshotPublisher::Publish(*f.owner, token, next);
  REQUIRE(receipt.error == std::errc::state_not_recoverable);
  REQUIRE_FALSE(receipt.published);
  REQUIRE(receipt.recovery_durable);
  REQUIRE(f.Bytes("data") == competing);
  REQUIRE(std::filesystem::exists(f.profile->path() / receipt.stage));
  auto retried = SnapshotPublisher::Publish(*f.owner, current, next);
  REQUIRE_FALSE(retried.error);
  REQUIRE(retried.published);
  REQUIRE(retried.durable);
  REQUIRE(f.Bytes("data") == std::vector<uint8_t>(next.begin(), next.end()));
}
TEST_CASE("Snapshot CAS rejects absent-to-present and replacement identity races",
          "[snapshot_publication]") {
  SnapshotFixture f;
  const std::array<uint8_t, 1> bytes{4};
  auto absent = f.Token("data");
  REQUIRE_FALSE(f.profile->WriteNewFile("data", bytes).error);
  auto conflict = SnapshotPublisher::Publish(*f.owner, absent, bytes);
  REQUIRE(conflict.error == std::errc::state_not_recoverable);
  REQUIRE_FALSE(conflict.published);
  auto original = f.Token("data");
  REQUIRE_FALSE(f.profile->Rename("data", "old", false));
  REQUIRE_FALSE(f.profile->WriteNewFile("data", bytes).error);
  auto replaced = SnapshotPublisher::Publish(*f.owner, original, bytes);
  REQUIRE(replaced.error == std::errc::state_not_recoverable);
  REQUIRE_FALSE(replaced.published);
  REQUIRE(f.Bytes("data") == std::vector<uint8_t>{4});
  REQUIRE(f.Bytes("old") == std::vector<uint8_t>{4});
}
TEST_CASE("Open handles and mappings prevent snapshot replacement",
          "[snapshot_publication]") {
  SnapshotFixture f;
  const std::array<uint8_t, 1> old{1}, next{2};
  REQUIRE_FALSE(f.profile->WriteNewFile("data", old).error);
  auto token = f.Token("data");
  std::error_code error;
  auto read = f.profile->OpenFile("data", FileAccess::kGenericRead, error);
  REQUIRE(read);
  auto first = SnapshotPublisher::Publish(*f.owner, token, next);
  REQUIRE(first.error == std::errc::device_or_resource_busy);
  REQUIRE_FALSE(first.published);
  read.reset();
  auto map = f.profile->MapRead("data", 0, 1, error);
  REQUIRE(map);
  auto second = SnapshotPublisher::Publish(*f.owner, token, next);
  REQUIRE(second.error == std::errc::device_or_resource_busy);
  REQUIRE_FALSE(second.published);
  map.reset();
  auto third = SnapshotPublisher::Publish(*f.owner, token, next);
  REQUIRE_FALSE(third.error);
  REQUIRE(third.durable);
}
TEST_CASE("Exclusive snapshot stages deny aliases and reject truncated payloads",
          "[snapshot_publication]") {
  SnapshotFixture f;
  auto token = f.Token("new");
  std::error_code error;
  bool created = false;
  auto stage = f.profile->CreateSnapshotStage("stage.tmp", created, error);
  REQUIRE(stage);
  REQUIRE(created);
  const std::array<uint8_t, 2> next{5, 6};
  REQUIRE_FALSE(stage->WriteResult(0, next).error);
  REQUIRE_FALSE(f.profile->OpenFile("stage.tmp", FileAccess::kGenericRead, error));
  REQUIRE(error == std::errc::device_or_resource_busy);
  REQUIRE_FALSE(f.profile->MapRead("stage.tmp", 0, 1, error));
  REQUIRE(error == std::errc::device_or_resource_busy);
  // Real owned scratch FD damage; never a simulated successful I/O count.
  REQUIRE(truncate((f.profile->path() / "stage.tmp").c_str(), 1) == 0);
  auto receipt = f.profile->CompareAndPublishSnapshot(*token, *stage, next);
  REQUIRE(receipt.error == std::errc::state_not_recoverable);
  REQUIRE_FALSE(receipt.published);
  REQUIRE(stage->WriteResult(0, next).error == std::errc::operation_not_permitted);
  REQUIRE_FALSE(std::filesystem::exists(f.profile->path() / "new"));
  stage.reset();
  REQUIRE(std::filesystem::exists(f.profile->path() / "stage.tmp"));
}
TEST_CASE("Snapshots distinguish missing unreadable oversized and nonregular data",
          "[snapshot_publication]") {
  SnapshotFixture f;
  const std::array<uint8_t, 2> bytes{1, 2};
  REQUIRE_FALSE(f.profile->WriteNewFile("data", bytes).error);
  std::error_code error;
  REQUIRE_FALSE(f.profile->CaptureSnapshot("data", 1, error));
  REQUIRE(error == std::errc::file_too_large);
  REQUIRE_FALSE(f.profile->Create("directory", true));
  REQUIRE_FALSE(f.profile->CaptureSnapshot("directory", 4096, error));
  REQUIRE(error);
  std::filesystem::create_symlink("absent", f.profile->path() / "dangling");
  REQUIRE_FALSE(f.profile->CaptureSnapshot("dangling", 4096, error));
  REQUIRE(error);
  REQUIRE(chmod((f.profile->path() / "data").c_str(), 0000) == 0);
  auto unreadable = f.profile->CaptureSnapshot("data", 4096, error);
  const auto actual_error = error;
  REQUIRE(chmod((f.profile->path() / "data").c_str(), 0600) == 0);
  if (geteuid() == 0 || unreadable) {
    WARN("EACCES prerequisite unavailable: effective UID/capability bypass");
  } else { REQUIRE(actual_error == std::errc::permission_denied); }
  REQUIRE(f.Bytes("data") == std::vector<uint8_t>{1, 2});
}
TEST_CASE("Source retirement and exact view identity prevent stale publication",
          "[snapshot_publication]") {
  SnapshotFixture f;
  const std::array<uint8_t, 1> bytes{3};
  auto token = f.Token("new");
  std::error_code error;
  bool created;
  auto other = f.profile->Subroot({}, false, error);
  auto stage = other->CreateSnapshotStage("stage.tmp", created, error);
  REQUIRE(stage);
  REQUIRE_FALSE(stage->WriteResult(0, bytes).error);
  REQUIRE(f.profile->CompareAndPublishSnapshot(*token, *stage, bytes).error ==
          std::errc::invalid_argument);
  f.profile->Retire();
  auto receipt = SnapshotPublisher::Publish(*f.owner, token, bytes);
  REQUIRE(receipt.error == std::errc::no_such_device);
  REQUIRE_FALSE(receipt.published);
  REQUIRE_FALSE(std::filesystem::exists(f.profile->path() / "new"));
}
TEST_CASE("Cancellation before publication preserves original and truthful receipt",
          "[snapshot_publication]") {
  SnapshotFixture f;
  const std::array<uint8_t, 1> bytes{3};
  REQUIRE_FALSE(f.profile->WriteNewFile("data", bytes).error);
  auto token = f.Token("data");
  std::atomic<bool> canceled{true};
  auto receipt = SnapshotPublisher::Publish(*f.owner, token, bytes, &canceled);
  REQUIRE(receipt.error == std::errc::operation_canceled);
  REQUIRE_FALSE(receipt.stage_created);
  REQUIRE_FALSE(receipt.published);
  REQUIRE(f.Bytes("data") == std::vector<uint8_t>{3});
}
TEST_CASE("Stage admission failure preserves original and identifies created file",
          "[snapshot_publication]") {
  SnapshotFixture f;
  bool full = false;
  std::error_code error;
  auto space = HostStorageSpace::ForPath(f.profile->path(), kHostStorageReserve,
      error, [&](const auto&, auto&) {
        return std::filesystem::space_info{1ull << 40,
            kHostStorageReserve + (full ? 0 : (1ull << 20)),
            kHostStorageReserve + (full ? 0 : (1ull << 20))};
      });
  auto isolated = HostStorageRoot::Open(f.profile->path(), false, error,
                                        kHostStorageReserve, space);
  REQUIRE(isolated);
  const std::array<uint8_t, 1> bytes{6};
  REQUIRE_FALSE(isolated->WriteNewFile("data", bytes).error);
  auto token = isolated->CaptureSnapshot("data", 4096, error);
  REQUIRE(token);
  REQUIRE_FALSE(error);
  bool created;
  auto stage = isolated->CreateSnapshotStage("stage.tmp", created, error);
  REQUIRE(stage);
  REQUIRE(created);
  full = true;
  auto written = stage->WriteResult(0, bytes);
  REQUIRE(written.error == std::errc::no_space_on_device);
  REQUIRE(written.bytes == 0);
  stage.reset();
  REQUIRE(std::filesystem::exists(f.profile->path() / "stage.tmp"));
  REQUIRE(f.Bytes("data") == std::vector<uint8_t>{6});
  // An isolated query is never falsely admitted as a protected recovery root.
  REQUIRE_FALSE(isolated->ProtectRecovery(error));
  REQUIRE(error == std::errc::operation_not_supported);
}
TEST_CASE("Recovery owner rejects unknown partial directories and protects aliases",
          "[snapshot_publication]") {
  SnapshotScratch scratch;
  auto root = scratch.Root();
  REQUIRE_FALSE(root->Create(SnapshotRecoveryOwner::kDirectory, true));
  std::ofstream(scratch.path / SnapshotRecoveryOwner::kDirectory / "foreign")
      << "preserve";
  auto unknown = SnapshotRecoveryOwner::Open(root);
  REQUIRE(unknown->error());
  REQUIRE(std::filesystem::exists(scratch.path /
      SnapshotRecoveryOwner::kDirectory / "foreign"));
  SnapshotFixture f;
  std::error_code error;
  REQUIRE_FALSE(f.parent->GuestView(error));
  REQUIRE(error == std::errc::permission_denied);
  auto protected_root = f.owner->root();
  f.owner.reset();
  // Existing guest admission keeps protection alive after owner teardown.
  REQUIRE_FALSE(f.parent->GuestView(error));
  REQUIRE(error == std::errc::permission_denied);
  REQUIRE(protected_root->WriteNewFile("unknown", {}).error ==
          std::errc::permission_denied);
  REQUIRE_FALSE(std::filesystem::exists(protected_root->path() / "unknown"));
}
TEST_CASE("Recovery count policy is separate from unchanged console max64",
          "[snapshot_publication]") {
  SnapshotFixture f;
  for (unsigned i = 0; i != 65; ++i) {
    char name[46];
    std::snprintf(name, sizeof(name), "snapshot-%032x.bin", i);
    REQUIRE_FALSE(f.owner->root()->WriteNewFile(name, {}).error);
  }
  std::error_code error;
  auto reopened = SnapshotRecoveryOwner::Open(f.parent);
  REQUIRE_FALSE(reopened->error());
  REQUIRE(reopened->root()->is_recovery_private());
  REQUIRE_FALSE(f.parent->Create("console", true));
  auto console = f.parent->Subroot("console", false, error);
  REQUIRE(console);
  for (unsigned i = 0; i != 65; ++i) {
    std::ofstream(console->path() / std::to_string(i));
    REQUIRE(chmod((console->path() / std::to_string(i)).c_str(), 0600) == 0);
  }
  REQUIRE_FALSE(console->ProtectFlat(error));
  REQUIRE(error == std::errc::value_too_large);
  REQUIRE(std::filesystem::exists(console->path() / "64"));
}
TEST_CASE("Durability retry retains publication observation after source loss",
          "[snapshot_publication]") {
  SnapshotFixture f;
  const std::array<uint8_t, 1> bytes{7};
  auto write = SnapshotPublisher::Publish(*f.owner, f.Token("new"), bytes);
  REQUIRE_FALSE(write.error);
  REQUIRE(write.current);
  f.profile->Retire();
  auto retry = f.profile->RetrySnapshotDurability(write.current);
  REQUIRE(retry.error == std::errc::no_such_device);
  REQUIRE(retry.published);
  REQUIRE_FALSE(retry.durable);
  REQUIRE(retry.current == write.current);
}
TEST_CASE("Fresh owned profile views reload published snapshots and recovery copies",
          "[snapshot_publication]") {
  SnapshotFixture f;
  const std::array<uint8_t, 1> old{3}, next{9};
  REQUIRE_FALSE(f.profile->WriteNewFile("data", old).error);
  auto write = SnapshotPublisher::Publish(*f.owner, f.Token("data"), next);
  REQUIRE_FALSE(write.error);
  auto saved = write.recovery;
  write = {}; // Drop tokens before deliberate view teardown/remount.
  f.profile.reset();
  f.owner.reset();
  auto owner = SnapshotRecoveryOwner::Open(f.parent);
  REQUIRE_FALSE(owner->error());
  std::error_code error;
  auto raw = f.parent->Subroot("profile", false, error);
  auto profile = raw->GuestView(error);
  REQUIRE(profile);
  auto current = profile->CaptureSnapshot("data", 4096, error);
  REQUIRE_FALSE(error);
  REQUIRE(current);
  REQUIRE(std::vector<uint8_t>(current->bytes().begin(), current->bytes().end()) ==
          std::vector<uint8_t>{9});
  std::vector<uint8_t> recovery;
  REQUIRE_FALSE(owner->root()->ReadFile(saved, 4096, recovery));
  REQUIRE(recovery == std::vector<uint8_t>{3});
  // Same-process fresh-owner/view test, not a cold restart/power-loss claim.
}
TEST_CASE("Only an actual published token authorizes snapshot durability retry",
          "[snapshot_publication]") {
  SnapshotFixture f;
  const std::array<uint8_t, 1> bytes{5};
  REQUIRE_FALSE(f.profile->WriteNewFile("data", bytes).error);
  auto ordinary = f.Token("data");
  auto rejected = f.profile->RetrySnapshotDurability(ordinary);
  REQUIRE(rejected.error == std::errc::invalid_argument);
  REQUIRE_FALSE(rejected.published);
  REQUIRE_FALSE(rejected.durable);
  REQUIRE_FALSE(rejected.current);
  auto write = SnapshotPublisher::Publish(*f.owner, ordinary, bytes);
  REQUIRE_FALSE(write.error);
  REQUIRE(write.published);
  REQUIRE(write.current);
  // Reading the just-published inode still does not mint publisher authority.
  auto recaptured = f.Token("data");
  REQUIRE(f.profile->RetrySnapshotDurability(recaptured).error ==
          std::errc::invalid_argument);
  auto retry = f.profile->RetrySnapshotDurability(write.current);
  REQUIRE_FALSE(retry.error);
  REQUIRE(retry.published);
  REQUIRE(retry.durable);
  REQUIRE(retry.current == write.current);
  f.profile->Retire();
  auto unavailable = f.profile->RetrySnapshotDurability(write.current);
  REQUIRE(unavailable.error == std::errc::no_such_device);
  REQUIRE(unavailable.published);
  REQUIRE_FALSE(unavailable.durable);
  REQUIRE(unavailable.current == write.current);
}
}  // namespace xe::filesystem::test
#endif
