/** Xenia : Xbox 360 Emulator Research Project; BSD license, see LICENSE. */
#include "xenia/base/testing/snapshot_fixture.h"
#include "xenia/base/snapshot_publication_set.h"
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <map>
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

#if XE_PLATFORM_LINUX
namespace xe::filesystem {
class SnapshotRecoveryOwnerTestAccess final {
 public:
  static auto Cut(std::shared_ptr<const HostStorageRoot> parent,unsigned boundary) {
    return SnapshotRecoveryOwner::OpenInternal(std::move(parent),true,boundary);
  }
  static auto CompleteLive(std::shared_ptr<const HostStorageRoot> parent,
                           const SnapshotRecoveryOwner& initializer) {
    return SnapshotRecoveryOwner::OpenInternal(
        std::move(parent),false,0,initializer.bootstrap_.get());
  }
};
}
namespace xe::filesystem::test {
TEST_CASE("First recovery owner publishes only complete protected directories",
          "[snapshot_publication][bootstrap][recovery][native]") {
  const unsigned boundary=GENERATE(1u,2u,3u);
  SnapshotScratch scratch;auto parent=scratch.Root();
  auto stopped=SnapshotRecoveryOwnerTestAccess::Cut(parent,boundary);
  REQUIRE(stopped);REQUIRE(stopped->error()==std::errc::operation_canceled);
  const auto canonical=scratch.path/SnapshotRecoveryOwner::kDirectory;
  REQUIRE(std::filesystem::exists(canonical)==(boundary==3));
  if (boundary==3) {
    REQUIRE(stopped->created());REQUIRE(stopped->root());
    std::error_code error;REQUIRE_FALSE(stopped->root()->GuestView(error));REQUIRE(error);
    stopped.reset();auto reopened=SnapshotRecoveryOwner::OpenExisting(parent);
    REQUIRE_FALSE(reopened->error());REQUIRE(reopened->root());REQUIRE_FALSE(reopened->created());
  } else {
    REQUIRE_FALSE(stopped->created());REQUIRE_FALSE(stopped->root());stopped.reset();
    std::map<std::filesystem::path,uintmax_t> original;
    for (const auto& entry:std::filesystem::recursive_directory_iterator(scratch.path)) {
      if (entry.is_regular_file()) { original.emplace(entry.path(),entry.file_size()); }
    }
    auto recreated=SnapshotRecoveryOwner::Open(parent);
    REQUIRE_FALSE(recreated->error());REQUIRE(recreated->created());
    for (const auto& [path,size]:original) { REQUIRE(std::filesystem::file_size(path)==size); }
  }
}
TEST_CASE("A live recovery capsule refines only through its exact private owner handoff",
          "[snapshot_publication][bootstrap][recovery][live_handoff]") {
  SnapshotScratch scratch, foreign_scratch;
  auto parent=scratch.Root();
  auto foreign=foreign_scratch.Root();
  auto stopped=SnapshotRecoveryOwnerTestAccess::Cut(parent,4);
  REQUIRE(stopped);
  REQUIRE(stopped->error()==std::errc::operation_canceled);
  REQUIRE(stopped->created());
  REQUIRE_FALSE(stopped->root());
  const auto marker=scratch.path/SnapshotRecoveryOwner::kDirectory/SnapshotRecoveryOwner::kMarker;
  struct stat before{};
  REQUIRE(stat(marker.c_str(),&before)==0);
  REQUIRE(S_ISREG(before.st_mode));
  std::ifstream input(marker,std::ios::binary);
  const std::vector<uint8_t> original{std::istreambuf_iterator<char>(input),
                                      std::istreambuf_iterator<char>()};
  REQUIRE_FALSE(original.empty());
  // Ordinary factories cannot take another still-live initializer's receipt.
  const auto ordinary=SnapshotRecoveryOwner::OpenExisting(parent);
  REQUIRE(ordinary);
  REQUIRE(ordinary->error());
  const auto refused=SnapshotRecoveryOwnerTestAccess::CompleteLive(foreign,*stopped);
  REQUIRE(refused);
  REQUIRE(refused->error()==std::errc::permission_denied);
  REQUIRE_FALSE(refused->root());
  REQUIRE_FALSE(std::filesystem::exists(foreign_scratch.path/SnapshotRecoveryOwner::kDirectory));
  auto owner=SnapshotRecoveryOwnerTestAccess::CompleteLive(parent,*stopped);
  REQUIRE(owner);
  REQUIRE_FALSE(owner->error());
  REQUIRE(owner->root());
  REQUIRE_FALSE(owner->created());
  struct stat after{};
  REQUIRE(stat(marker.c_str(),&after)==0);
  REQUIRE(after.st_dev==before.st_dev);
  REQUIRE(after.st_ino==before.st_ino);
  std::error_code error;
  std::vector<uint8_t> bytes;
  REQUIRE_FALSE(owner->root()->ReadFile(SnapshotRecoveryOwner::kMarker,256,bytes));
  REQUIRE(bytes==original);
  auto raw=parent->Subroot(SnapshotRecoveryOwner::kDirectory,false,error);
  REQUIRE(raw);
  REQUIRE_FALSE(error);
  REQUIRE_FALSE(raw->GuestView(error));
  REQUIRE(error==std::errc::permission_denied);
  stopped.reset();
  const auto reopened=SnapshotRecoveryOwner::OpenExisting(parent);
  REQUIRE(reopened);
  REQUIRE_FALSE(reopened->error());
  REQUIRE(reopened->root());
}
TEST_CASE("Complete recovery reopen reclaims its exact retained bootstrap receipt before typed refinement",
          "[snapshot_publication][bootstrap][recovery][retained_guest]") {
  SnapshotScratch scratch, unrelated_scratch;
  auto parent=scratch.Root();auto unrelated=unrelated_scratch.Root();
  std::error_code error;auto unrelated_guest=unrelated->GuestView(error);
  REQUIRE(unrelated_guest);REQUIRE_FALSE(error);
  auto stopped=SnapshotRecoveryOwnerTestAccess::Cut(parent,4);
  REQUIRE(stopped);REQUIRE(stopped->error()==std::errc::operation_canceled);
  REQUIRE(stopped->created());REQUIRE_FALSE(stopped->root());
  REQUIRE(std::filesystem::exists(scratch.path/SnapshotRecoveryOwner::kDirectory/SnapshotRecoveryOwner::kMarker));
  stopped.reset();  // unrelated guest retains the original stage/canonical roots
  auto reopened=SnapshotRecoveryOwner::OpenExisting(parent);
  REQUIRE(reopened);REQUIRE_FALSE(reopened->error());REQUIRE(reopened->root());REQUIRE_FALSE(reopened->created());
  auto raw=parent->Subroot(SnapshotRecoveryOwner::kDirectory,false,error);REQUIRE(raw);REQUIRE_FALSE(error);
  REQUIRE_FALSE(raw->GuestView(error));REQUIRE(error==std::errc::permission_denied);
  // Once refined, an ordinary typed reopen reuses the same policy without
  // requiring another capsule or weakening private guest exclusion.
  auto second=SnapshotRecoveryOwner::OpenExisting(parent);
  REQUIRE(second);REQUIRE_FALSE(second->error());REQUIRE(second->root());
  const std::array<uint8_t,2> ordinary{7,9};
  REQUIRE_FALSE(unrelated_guest->WriteNewFile("ordinary.data",ordinary).error);
  REQUIRE_FALSE(raw->GuestView(error));REQUIRE(error==std::errc::permission_denied);
}

}
#endif

#if XE_PLATFORM_LINUX
namespace xe::filesystem::test {

TEST_CASE("A scoped publication keeps Account and two artifact reservations behind one genuine original-view batch",
          "[snapshot_publication][scoped_set][lifetime]") {
  SnapshotFixture f;
  const std::array<uint8_t, 3> account{1, 0, 2}, old{3, 0, 4}, next{5, 0, 6};
  REQUIRE_FALSE(f.profile->WriteNewFile("Account", account).error);
  REQUIRE_FALSE(f.profile->WriteNewFile("FFFE07D1.gpd", old).error);
  REQUIRE_FALSE(f.profile->WriteNewFile("tile_64.png", old).error);
  std::error_code error;
  auto base = f.profile->ReserveSnapshotFile(f.Token("Account"), error); REQUIRE(base);
  auto gpd = base->CaptureSnapshot("FFFE07D1.gpd", 4096, error); REQUIRE(gpd);
  auto tile = base->CaptureSnapshot("tile_64.png", 4096, error); REQUIRE(tile);
  auto set = SnapshotPublicationSet::Reserve(base, {gpd, tile}, error);
  REQUIRE(set); REQUIRE_FALSE(error);
  CHECK_FALSE(f.profile->ContentNamespaceReady("Account"));
  CHECK_FALSE(base->ContentNamespaceReady("FFFE07D1.gpd"));
  CHECK_FALSE(base->ContentNamespaceReady("tile_64.png"));
  const auto gpd_write = set->Publish(*f.owner, gpd, next);
  REQUIRE_FALSE(gpd_write.error); REQUIRE(gpd_write.durable); REQUIRE(gpd_write.current);
  CHECK(gpd_write.current->root() == base);
  CHECK_FALSE(base->ContentNamespaceReady("tile_64.png"));
  CHECK(std::ranges::equal(gpd->bytes(), old)); CHECK(std::ranges::equal(tile->bytes(), old));
  const auto tile_write = set->Publish(*f.owner, tile, next);
  REQUIRE_FALSE(tile_write.error); REQUIRE(tile_write.durable); REQUIRE(tile_write.current);
  CHECK(tile_write.current->root() == base);
  auto batch = set->Validate({gpd_write.current, tile_write.current}, error);
  REQUIRE(batch); REQUIRE_FALSE(error);
  CHECK(batch->root() == base);
  CHECK(batch->tokens() == std::vector<std::shared_ptr<const HostStorageSnapshotToken>>{
      gpd_write.current, tile_write.current});
  set.reset();  // Batch retains the exact access owner until its native guard releases.
  CHECK_FALSE(base->ContentNamespaceReady("tile_64.png"));
  batch.reset();
  CHECK(base->ContentNamespaceReady("tile_64.png"));
  CHECK(base->ContentNamespaceReady("FFFE07D1.gpd"));
  CHECK_FALSE(f.profile->ContentNamespaceReady("Account"));
  CHECK(f.Bytes("FFFE07D1.gpd") == std::vector<uint8_t>(next.begin(), next.end()));
  CHECK(f.Bytes("tile_64.png") == std::vector<uint8_t>(next.begin(), next.end()));
  std::vector<uint8_t> bytes;
  REQUIRE_FALSE(base->ReadFile("Account", 4096, bytes));
  CHECK(bytes == std::vector<uint8_t>(account.begin(), account.end()));
  base->Retire(); REQUIRE_FALSE(base->ReleaseRetiredSnapshotReservation());
  CHECK(f.profile->ContentNamespaceReady("Account"));
}
TEST_CASE("Scoped publication refuses changed retries and cannot rebind a fresh token over its retained before bytes",
          "[snapshot_publication][scoped_set][stale]") {
  SnapshotFixture f;
  const std::array<uint8_t, 3> old{1, 0, 2}, next{3, 0, 4}, competing{5, 0, 6};
  REQUIRE_FALSE(f.profile->WriteNewFile("target", old).error);
  auto before = f.Token("target"); std::error_code error;
  auto set = SnapshotPublicationSet::Reserve(f.profile, {before}, error); REQUIRE(set);
  SECTION("same captured operation retries without another physical replacement") {
    const auto first = set->Publish(*f.owner, before, next);
    REQUIRE_FALSE(first.error); REQUIRE(first.current);
    const auto retry = set->RetryDurability(first.current);
    REQUIRE_FALSE(retry.error); REQUIRE(retry.published); REQUIRE(retry.durable);
    REQUIRE(retry.current); CHECK(retry.current == first.current);
    const auto repeated = set->Publish(*f.owner, before, next);
    REQUIRE_FALSE(repeated.error); REQUIRE(repeated.current);
    CHECK(repeated.current == first.current);
    CHECK(repeated.durable); CHECK(repeated.phase == SnapshotPublicationPhase::kComplete);
    CHECK(set->Delete(*f.owner, before).error == std::errc::invalid_argument);
    SnapshotFixture foreign;
    CHECK(set->Publish(*foreign.owner, before, next).error == std::errc::invalid_argument);
    CHECK(repeated.current->info().file_id == first.current->info().file_id);
    CHECK(repeated.recovery == first.recovery); CHECK(repeated.stage == first.stage);
    CHECK(set->Publish(*f.owner, before, competing).error == std::errc::invalid_argument);
    const auto actual = set->Capture("target", 4096, error); REQUIRE(actual);
    CHECK(std::ranges::equal(actual->bytes(), next));
    const auto unowned_retry = set->RetryDurability(actual);
    CHECK(unowned_retry.error == std::errc::invalid_argument);
    CHECK_FALSE(unowned_retry.published);
  }
  SECTION("an uncooperative native write remains preserved rather than becoming a fresh expected token") {
    // Deliberately outside the cooperating HostStorage admission. The common
    // CAS and immutable submission must still refuse changed exact bytes.
    std::ofstream native(f.profile->path() / "target", std::ios::binary | std::ios::trunc);
    native.write(reinterpret_cast<const char*>(competing.data()), competing.size());
    REQUIRE(native.good()); native.close();
    const auto fresh = set->Capture("target", 4096, error); REQUIRE(fresh);
    CHECK(std::ranges::equal(fresh->bytes(), competing));
    const auto rebound = set->Publish(*f.owner, fresh, next);
    CHECK(rebound.error == std::errc::state_not_recoverable);
    CHECK_FALSE(rebound.published);
    const auto original = set->Publish(*f.owner, before, next);
    CHECK(original.error == std::errc::state_not_recoverable); CHECK_FALSE(original.published);
    const auto actual = set->Capture("target", 4096, error); REQUIRE(actual);
    CHECK(std::ranges::equal(actual->bytes(), competing));
  }
}
TEST_CASE("Scoped member admission respects the real parent depth bound and releases partial acquisition on a file conflict",
          "[snapshot_publication][scoped_set][admission]") {
  SnapshotFixture f;
  const std::array<uint8_t, 1> value{0x3a}; std::error_code error;
  std::vector<std::shared_ptr<HostStorageRoot>> parents;
  std::shared_ptr<const HostStorageRoot> base = f.profile;
  for (unsigned i = 0; i != 2; ++i) {
    const auto name = "parent" + std::to_string(i);
    REQUIRE_FALSE(f.profile->WriteNewFile(name, value).error);
    auto token = base->CaptureSnapshot(name, 4096, error); REQUIRE(token);
    auto parent = base->ReserveSnapshotFile(token, error); REQUIRE(parent);
    parents.push_back(parent); base = std::move(parent);
  }
  std::vector<std::shared_ptr<const HostStorageSnapshotToken>> tokens;
  for (unsigned i = 0; i != 7; ++i) {
    const auto name = "member" + std::to_string(i);
    REQUIRE_FALSE(f.profile->WriteNewFile(name, value).error);
    auto token = base->CaptureSnapshot(name, 4096, error); REQUIRE(token); tokens.push_back(token);
  }
  CHECK_FALSE(SnapshotPublicationSet::Reserve(base, tokens, error));
  CHECK(error == std::errc::operation_not_supported);
  CHECK(base->ContentNamespaceReady("member0"));
  SECTION("six additional members genuinely fit the unchanged limit") {
    tokens.resize(6);
    auto set = SnapshotPublicationSet::Reserve(base, tokens, error); REQUIRE(set);
    CHECK_FALSE(error); CHECK_FALSE(base->ContentNamespaceReady("member5"));
    set.reset(); CHECK(base->ContentNamespaceReady("member5"));
  }
  SECTION("a real foreign file open on the second member unwinds only new reservations") {
    auto open = f.profile->OpenFile("member1", FileAccess::kGenericRead, error); REQUIRE(open);
    tokens.resize(2);
    CHECK_FALSE(SnapshotPublicationSet::Reserve(base, tokens, error));
    CHECK(error == std::errc::device_or_resource_busy);
    CHECK(base->ContentNamespaceReady("member0"));
    CHECK_FALSE(f.profile->ContentNamespaceReady("parent0"));
  }
  for (auto i = parents.rbegin(); i != parents.rend(); ++i) {
    (*i)->Retire(); REQUIRE_FALSE((*i)->ReleaseRetiredSnapshotReservation());
  }
}
TEST_CASE("Scoped empty writes remain distinct from deletion and a retired source cannot publish",
          "[snapshot_publication][scoped_set][empty_delete][retirement]") {
  SnapshotFixture f; std::error_code error;
  SECTION("nonnull owned empty data creates an actual file which can later be deleted") {
    auto missing = f.Token("zero"); REQUIRE_FALSE(missing->exists());
    auto create = SnapshotPublicationSet::Reserve(f.profile, {missing}, error); REQUIRE(create);
    const auto written = create->Publish(*f.owner, missing, std::span<const uint8_t>{});
    REQUIRE_FALSE(written.error); REQUIRE(written.published); REQUIRE(written.durable);
    REQUIRE(written.current); CHECK(written.current->exists()); CHECK(written.current->bytes().empty());
    CHECK(create->Delete(*f.owner, missing).error == std::errc::invalid_argument);
    create.reset();
    auto existing = f.Token("zero"); REQUIRE(existing->exists());
    auto remove = SnapshotPublicationSet::Reserve(f.profile, {existing}, error); REQUIRE(remove);
    const auto deleted = remove->Delete(*f.owner, existing);
    REQUIRE_FALSE(deleted.error); REQUIRE(deleted.published); REQUIRE(deleted.durable);
    REQUIRE(deleted.current); CHECK_FALSE(deleted.current->exists());
    CHECK(deleted.kind == SnapshotMutationKind::kDelete);
    CHECK(remove->Publish(*f.owner, existing, std::span<const uint8_t>{}).error ==
          std::errc::invalid_argument);
    const auto retried = remove->RetryDurability(deleted.current);
    REQUIRE_FALSE(retried.error); CHECK(retried.published); CHECK(retried.durable);
    const auto same_delete = remove->Delete(*f.owner, existing);
    REQUIRE_FALSE(same_delete.error); CHECK(same_delete.current == deleted.current);
    remove.reset(); CHECK_FALSE(f.Token("zero")->exists());
  }
  SECTION("a real durability retry after retirement retains the physical publication duty") {
    const std::array<uint8_t, 2> old{1, 2}, next{3, 4};
    REQUIRE_FALSE(f.profile->WriteNewFile("target", old).error);
    const auto before = f.Token("target");
    auto set = SnapshotPublicationSet::Reserve(f.profile, {before}, error); REQUIRE(set);
    const auto first = set->Publish(*f.owner, before, next);
    REQUIRE_FALSE(first.error); REQUIRE(first.published); REQUIRE(first.durable);
    REQUIRE(first.current);
    f.profile->Retire();
    const auto retry = set->RetryDurability(first.current);
    CHECK(retry.error); CHECK(retry.published); CHECK_FALSE(retry.durable);
    CHECK(retry.current == first.current);
    const auto retained = set->Publish(*f.owner, before, next);
    CHECK(retained.error == retry.error); CHECK(retained.published); CHECK_FALSE(retained.durable);
    CHECK(retained.stage == first.stage); CHECK(retained.recovery == first.recovery);
    std::ifstream input(f.profile->path() / "target", std::ios::binary); REQUIRE(input);
    CHECK(std::vector<uint8_t>(std::istreambuf_iterator<char>(input), {}) ==
          std::vector<uint8_t>(next.begin(), next.end()));
  }
  SECTION("actual base retirement cancels the original captured authority") {
    const std::array<uint8_t, 2> old{1, 2}, next{3, 4};
    REQUIRE_FALSE(f.profile->WriteNewFile("target", old).error);
    const auto before = f.Token("target");
    auto set = SnapshotPublicationSet::Reserve(f.profile, {before}, error); REQUIRE(set);
    f.profile->Retire();
    const auto refused = set->Publish(*f.owner, before, next);
    CHECK(refused.error); CHECK_FALSE(refused.published);
    CHECK_FALSE(set->Capture("target", 4096, error)); CHECK(error);
    std::ifstream input(f.profile->path() / "target", std::ios::binary);
    REQUIRE(input);
    CHECK(std::vector<uint8_t>(std::istreambuf_iterator<char>(input), {}) ==
          std::vector<uint8_t>(old.begin(), old.end()));
  }
}

}  // namespace xe::filesystem::test
#endif
