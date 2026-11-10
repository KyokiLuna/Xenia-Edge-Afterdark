/** Xenia : Xbox 360 Emulator Research Project; BSD license, see LICENSE. */
#include "xenia/kernel/xam/xdbf/gpd_snapshot.h"
#include <algorithm>
#include <cstdlib>
#include <sstream>
#include "third_party/mbedtls/include/mbedtls/sha256.h"
#include "xenia/kernel/xam/xdbf/gpd_info.h"
#include "xenia/base/testing/snapshot_fixture.h"
#include "xenia/base/xdbf_view.h"
#if XE_PLATFORM_LINUX
namespace xe::kernel::test {
namespace {
void Put32(std::vector<uint8_t>& bytes, size_t offset, uint32_t value) {
  for (unsigned i = 0; i != 4; ++i) { bytes[offset + i] = value >> ((3 - i) * 8); }
}
std::vector<uint8_t> Wire() {
  // One unknown section/key, unused table bytes and trailing opaque slack.
  // Every old byte must survive in the recovery copy, not only known records.
  std::vector<uint8_t> bytes(83, 0);
  Put32(bytes, 0, 0x58444246); Put32(bytes, 4, 0x10000);
  Put32(bytes, 8, 2); Put32(bytes, 12, 1);
  Put32(bytes, 16, 2); Put32(bytes, 20, 1);
  bytes[24] = 0xfe; bytes[25] = 0xdc; bytes[33] = 7;
  Put32(bytes, 34, 0); Put32(bytes, 38, 4);
  bytes[42] = 0xa5; bytes[59] = 0x7e;
  Put32(bytes, 60, 4); Put32(bytes, 64, UINT32_MAX - 4);
  bytes[69] = 0xb4;
  bytes[76] = 9; bytes[77] = 0; bytes[78] = 0xff; bytes[79] = 8;
  bytes[80] = 0xc1; bytes[81] = 0xc2; bytes[82] = 0xc3;
  return bytes;
}
const xam::GpdSnapshotIdentity kIdentity{0xe000000000000001ull, 7, 0xfffe07d1};
class TerminalProbe : public xam::GpdInfo {
 public:
  TerminalProbe() : GpdInfo(0x12345678) {}
  void SetTailSize(uint32_t size) { free_entries_.back().size = size; }
};
void RejectMalformedGpdSnapshot(const std::vector<uint8_t>& invalid) {
  // The generic table bounds pass: this specifically exercises GPD semantic
  // rejection, rather than the existing truncated-table/out-of-EOF cases.
  REQUIRE(XdbfTableView::Parse(invalid));
  filesystem::test::SnapshotFixture f;
  const auto original = Wire();
  REQUIRE_FALSE(f.profile->WriteNewFile("FFFE07D1.gpd", original).error);
  const auto valid_load = xam::GpdSnapshotStore::Load(kIdentity, f.profile);
  REQUIRE_FALSE(valid_load.error);
  REQUIRE(valid_load.readable());
  const auto attempted = xam::GpdSnapshotStore::Publish(*f.owner, valid_load, invalid);
  REQUIRE(attempted.storage.error == std::errc::bad_message);
  REQUIRE_FALSE(attempted.serialized);
  REQUIRE_FALSE(attempted.storage.stage_created);
  REQUIRE_FALSE(attempted.storage.recovery_created);
  REQUIRE_FALSE(attempted.storage.published);
  REQUIRE(f.Bytes("FFFE07D1.gpd") == original);

  REQUIRE_FALSE(f.profile->WriteNewFile("12345678.gpd", invalid).error);
  auto invalid_identity = kIdentity;
  invalid_identity.title_id = 0x12345678;
  const auto invalid_load = xam::GpdSnapshotStore::Load(invalid_identity, f.profile);
  REQUIRE(invalid_load.error == std::errc::bad_message);
  REQUIRE(invalid_load.token);
  REQUIRE(invalid_load.token->exists());
  REQUIRE_FALSE(invalid_load.missing());
  REQUIRE(std::vector<uint8_t>(invalid_load.token->bytes().begin(),
                               invalid_load.token->bytes().end()) == invalid);
  const auto replacement = xam::GpdSnapshotStore::Publish(*f.owner, invalid_load, original);
  REQUIRE(replacement.storage.error == std::errc::bad_message);
  REQUIRE_FALSE(replacement.serialized);
  REQUIRE_FALSE(replacement.storage.stage_created);
  REQUIRE_FALSE(replacement.storage.recovery_created);
  REQUIRE_FALSE(replacement.storage.published);
  REQUIRE(f.Bytes("12345678.gpd") == invalid);
  REQUIRE(f.Bytes("FFFE07D1.gpd") == original);
}
}
TEST_CASE("GPD snapshots preserve unknown wire and exact raw recovery payload",
          "[gpd_snapshot]") {
  filesystem::test::SnapshotFixture f;
  auto old = Wire();
  REQUIRE_FALSE(xam::ValidateGpdSnapshot(old));
  REQUIRE_FALSE(f.profile->WriteNewFile("FFFE07D1.gpd", old).error);
  auto load = xam::GpdSnapshotStore::Load(kIdentity, f.profile);
  REQUIRE_FALSE(load.error);
  REQUIRE(load.readable());
  auto next = old; next[76] = 5;
  auto write = xam::GpdSnapshotStore::Publish(*f.owner, load, next);
  REQUIRE(write.serialized);
  REQUIRE_FALSE(write.storage.error);
  REQUIRE(write.storage.published);
  REQUIRE(write.storage.durable);
  REQUIRE(write.identity.xuid == kIdentity.xuid);
  REQUIRE(write.identity.source_generation == kIdentity.source_generation);
  REQUIRE(f.Bytes("FFFE07D1.gpd") == next);
  std::vector<uint8_t> raw;
  REQUIRE_FALSE(f.owner->root()->ReadFile(write.storage.recovery, 4096, raw));
  REQUIRE(raw == old);
}
TEST_CASE("GPD load never turns missing or malformed payload into a default",
          "[gpd_snapshot]") {
  filesystem::test::SnapshotFixture f;
  auto absent = xam::GpdSnapshotStore::Load(kIdentity, f.profile);
  REQUIRE(absent.missing());
  REQUIRE_FALSE(absent.readable());
  const std::array<uint8_t, 5> invalid{'X', 'D', 'B', 'F', 0};
  REQUIRE_FALSE(f.profile->WriteNewFile("FFFE07D1.gpd", invalid).error);
  auto malformed = xam::GpdSnapshotStore::Load(kIdentity, f.profile);
  REQUIRE(malformed.error == std::errc::bad_message);
  REQUIRE(malformed.token);
  REQUIRE(malformed.token->exists());
  auto write = xam::GpdSnapshotStore::Publish(*f.owner, malformed, Wire());
  REQUIRE_FALSE(write.serialized);
  REQUIRE(write.storage.error == std::errc::bad_message);
  REQUIRE_FALSE(write.storage.stage_created);
  REQUIRE_FALSE(write.storage.published);
  REQUIRE(f.Bytes("FFFE07D1.gpd") == std::vector<uint8_t>(invalid.begin(), invalid.end()));
}
TEST_CASE("Malformed serialized GPD tables reject before staging",
          "[gpd_snapshot]") {
  filesystem::test::SnapshotFixture f;
  auto load = xam::GpdSnapshotStore::Load(kIdentity, f.profile);
  auto missing_eof = Wire(); Put32(missing_eof, 20, 0);
  auto bad_extent = Wire(); Put32(bad_extent, 38, 5);
  auto bad_free = Wire(); Put32(bad_free, 16, UINT32_MAX);
  auto bad_version = Wire(); Put32(bad_version, 4, 2);
  for (const auto& bad : {missing_eof, bad_extent, bad_free, bad_version}) {
    auto write = xam::GpdSnapshotStore::Publish(*f.owner, load, bad);
    REQUIRE_FALSE(write.serialized);
    REQUIRE(write.storage.error);
    REQUIRE_FALSE(write.storage.stage_created);
    REQUIRE_FALSE(write.storage.published);
  }
  REQUIRE_FALSE(std::filesystem::exists(f.profile->path() / "FFFE07D1.gpd"));
}
TEST_CASE("Checked GPD serialization rejects invalid models without touching output",
          "[gpd_snapshot]") {
  xam::GpdInfo invalid;
  std::vector<uint8_t> output{7, 8, 9};
  REQUIRE(invalid.SerializeChecked(output, 4096) == std::errc::bad_message);
  REQUIRE(output == std::vector<uint8_t>{7, 8, 9});
  xam::GpdInfo valid(kIdentity.title_id);
  REQUIRE(valid.SerializeChecked(output, 1) == std::errc::file_too_large);
  REQUIRE(output == std::vector<uint8_t>{7, 8, 9});
  REQUIRE_FALSE(valid.SerializeChecked(output, xam::GpdSnapshotStore::kMaximumBytes));
  REQUIRE_FALSE(xam::ValidateGpdSnapshot(output));
}
TEST_CASE("Checked model publication keeps unknown entries and raw original backup",
          "[gpd_snapshot]") {
  filesystem::test::SnapshotFixture f;
  auto original = Wire();
  REQUIRE_FALSE(f.profile->WriteNewFile("FFFE07D1.gpd", original).error);
  auto load = xam::GpdSnapshotStore::Load(kIdentity, f.profile);
  REQUIRE_FALSE(load.error);
  xam::GpdInfo model(kIdentity.title_id, original);
  REQUIRE(model.IsValid());
  auto write = xam::GpdSnapshotStore::Publish(*f.owner, load, model);
  REQUIRE(write.serialized);
  REQUIRE_FALSE(write.storage.error);
  REQUIRE(write.storage.durable);
  const auto published = f.Bytes("FFFE07D1.gpd");
  auto view = XdbfTableView::Parse(published);
  REQUIRE(view);
  auto unknown = view->Find(0xfedc, 7);
  REQUIRE(unknown);
  REQUIRE(std::vector<uint8_t>(unknown->begin(), unknown->end()) ==
          std::vector<uint8_t>{9, 0, 0xff, 8});
  std::vector<uint8_t> recovery;
  REQUIRE_FALSE(f.owner->root()->ReadFile(write.storage.recovery, 4096, recovery));
  REQUIRE(recovery == original);
}
TEST_CASE("A stale GPD source token cannot overwrite competing profile state",
          "[gpd_snapshot]") {
  filesystem::test::SnapshotFixture f;
  auto old = Wire();
  REQUIRE_FALSE(f.profile->WriteNewFile("FFFE07D1.gpd", old).error);
  auto load = xam::GpdSnapshotStore::Load(kIdentity, f.profile);
  auto competing = old; competing[76] = 2;
  std::error_code error;
  auto edit = f.profile->OpenFile("FFFE07D1.gpd", filesystem::FileAccess::kGenericWrite, error);
  REQUIRE(edit);
  REQUIRE_FALSE(edit->WriteResult(0, competing).error);
  REQUIRE_FALSE(edit->FlushResult());
  edit.reset();
  auto write = xam::GpdSnapshotStore::Publish(*f.owner, load, old);
  REQUIRE(write.serialized);
  REQUIRE(write.storage.error == std::errc::state_not_recoverable);
  REQUIRE_FALSE(write.storage.published);
  REQUIRE(f.Bytes("FFFE07D1.gpd") == competing);
}
TEST_CASE("Separate GPD receipts expose a partial pair without a rollback claim",
          "[gpd_snapshot]") {
  filesystem::test::SnapshotFixture f;
  auto old = Wire(), next = old, competing = old;
  next[76] = 4; competing[76] = 6;
  REQUIRE_FALSE(f.profile->WriteNewFile("FFFE07D1.gpd", old).error);
  REQUIRE_FALSE(f.profile->WriteNewFile("12345678.gpd", old).error);
  auto dashboard = xam::GpdSnapshotStore::Load(kIdentity, f.profile);
  auto title_identity = kIdentity; title_identity.title_id = 0x12345678;
  auto title = xam::GpdSnapshotStore::Load(title_identity, f.profile);
  auto first = xam::GpdSnapshotStore::Publish(*f.owner, dashboard, next);
  REQUIRE_FALSE(first.storage.error);
  REQUIRE(first.storage.published);
  REQUIRE(first.storage.durable);
  std::error_code error;
  auto edit = f.profile->OpenFile("12345678.gpd", filesystem::FileAccess::kGenericWrite, error);
  REQUIRE(edit);
  REQUIRE_FALSE(edit->WriteResult(0, competing).error);
  REQUIRE_FALSE(edit->FlushResult());
  edit.reset();
  auto second = xam::GpdSnapshotStore::Publish(*f.owner, title, next);
  REQUIRE(second.storage.error == std::errc::state_not_recoverable);
  REQUIRE_FALSE(second.storage.published);
  REQUIRE(f.Bytes("FFFE07D1.gpd") == next);
  REQUIRE(f.Bytes("12345678.gpd") == competing);
  // No manager/achievement/toast or paired-intent retry implementation here.
}
TEST_CASE("GPD snapshot naming preserves native ASCII case-insensitive identity",
          "[gpd_snapshot]") {
  filesystem::test::SnapshotFixture f;
  auto old = Wire(), next = old; next[76] = 3;
  REQUIRE_FALSE(f.profile->WriteNewFile("fffe07d1.GPD", old).error);
  auto load = xam::GpdSnapshotStore::Load(kIdentity, f.profile);
  REQUIRE_FALSE(load.error);
  REQUIRE(load.readable());
  REQUIRE(load.token->relative() == "fffe07d1.GPD");
  auto write = xam::GpdSnapshotStore::Publish(*f.owner, load, next);
  REQUIRE_FALSE(write.storage.error);
  REQUIRE(f.Bytes("fffe07d1.GPD") == next);
  REQUIRE_FALSE(std::filesystem::exists(f.profile->path() / "FFFE07D1.gpd"));
}
TEST_CASE("GPD folded-slot CAS detects a competing case variant and ambiguity",
          "[gpd_snapshot]") {
  filesystem::test::SnapshotFixture f;
  auto missing = xam::GpdSnapshotStore::Load(kIdentity, f.profile);
  REQUIRE(missing.missing());
  auto bytes = Wire();
  REQUIRE_FALSE(f.profile->WriteNewFile("fffe07d1.gpd", bytes).error);
  auto write = xam::GpdSnapshotStore::Publish(*f.owner, missing, bytes);
  REQUIRE(write.storage.error == std::errc::state_not_recoverable);
  REQUIRE_FALSE(write.storage.published);
  REQUIRE(f.Bytes("fffe07d1.gpd") == bytes);
  REQUIRE_FALSE(std::filesystem::exists(f.profile->path() / "FFFE07D1.gpd"));
  REQUIRE_FALSE(f.profile->WriteNewFile("FFFE07D1.gpd", bytes).error);
  auto ambiguous = xam::GpdSnapshotStore::Load(kIdentity, f.profile);
  REQUIRE(ambiguous.error == std::errc::file_exists);
  REQUIRE_FALSE(ambiguous.token);
  REQUIRE(f.Bytes("fffe07d1.gpd") == bytes);
  REQUIRE(f.Bytes("FFFE07D1.gpd") == bytes);
}
TEST_CASE("Duplicate GPD keys reject despite disjoint zero-length extents",
          "[gpd_snapshot]") {
  auto invalid = Wire();
  Put32(invalid, 12, 2); // Both table entries used.
  std::copy_n(invalid.begin() + 24, 18, invalid.begin() + 42);
  Put32(invalid, 52, 4); // Second extent is empty, exactly at validEOF.
  Put32(invalid, 56, 0); // Its section/id duplicates the first record only.
  RejectMalformedGpdSnapshot(invalid);
}
TEST_CASE("Unique GPD records reject overlapping used extents within EOF",
          "[gpd_snapshot]") {
  auto invalid = Wire();
  Put32(invalid, 12, 2);
  std::copy_n(invalid.begin() + 24, 18, invalid.begin() + 42);
  invalid[51] = 8; // Unique second key; duplicate-key rejection cannot mask it.
  Put32(invalid, 52, 2);
  Put32(invalid, 56, 2); // [2,4) overlaps first [0,4), both within EOF4.
  RejectMalformedGpdSnapshot(invalid);
}
TEST_CASE("GPD used and free overlap rejects within a valid final EOF",
          "[gpd_snapshot]") {
  auto invalid = Wire();
  Put32(invalid, 20, 2); // One finite free range plus the final EOF sentinel.
  Put32(invalid, 60, 1);
  Put32(invalid, 64, 2); // Free [1,3) overlaps used [0,4).
  Put32(invalid, 68, 4);
  Put32(invalid, 72, UINT32_MAX - 4); // Valid EOF4 retained in last used free entry.
  RejectMalformedGpdSnapshot(invalid);
}
TEST_CASE("Actual GPD producer grows and frees data with checked terminal geometry",
          "[gpd_snapshot]") {
  xam::GpdInfo model(0x12345678);
  std::vector<uint8_t> wire;
  REQUIRE_FALSE(model.SerializeChecked(wire, xam::GpdSnapshotStore::kMaximumBytes));
  REQUIRE_FALSE(xam::ValidateGpdSnapshot(wire));
  const std::array<uint8_t, 8> image{1, 0, 3, 0xff, 5, 6, 0, 8};
  model.AddString(0x100, u"short");
  model.AddImage(0x200, image);
  REQUIRE_FALSE(model.SerializeChecked(wire, xam::GpdSnapshotStore::kMaximumBytes));
  REQUIRE_FALSE(xam::ValidateGpdSnapshot(wire));
  // Replacing this real producer entry makes a finite hole; it is not a
  // handcrafted always-sizeMAX terminal fixture.
  model.AddString(0x100, u"a longer changed string with a real freed hole");
  REQUIRE_FALSE(model.SerializeChecked(wire, xam::GpdSnapshotStore::kMaximumBytes));
  REQUIRE_FALSE(xam::ValidateGpdSnapshot(wire));
  const auto view = XdbfTableView::Parse(wire);
  REQUIRE(view);
  REQUIRE(view->free_entries_used >= 2);
  const auto* tail = view->free_entries.data() + (view->free_entries_used - 1) * 8;
  const uint64_t offset = XdbfTableView::U32(tail);
  const uint64_t remaining = XdbfTableView::U32(tail + 4);
  REQUIRE(offset > 0);
  REQUIRE(remaining < UINT32_MAX);
  REQUIRE(offset + remaining == UINT32_MAX);
  xam::GpdInfo reloaded(0x12345678, wire);
  REQUIRE(reloaded.IsValid());
  REQUIRE(reloaded.GetString(0x100) == u"a longer changed string with a real freed hole");
  const auto loaded_image = reloaded.GetImage(0x200);
  REQUIRE(std::vector<uint8_t>(loaded_image.begin(), loaded_image.end()) ==
          std::vector<uint8_t>(image.begin(), image.end()));
  std::vector<uint8_t> roundtrip;
  REQUIRE_FALSE(reloaded.SerializeChecked(roundtrip, xam::GpdSnapshotStore::kMaximumBytes));
  REQUIRE_FALSE(xam::ValidateGpdSnapshot(roundtrip));
  REQUIRE(roundtrip == wire);
}
TEST_CASE("Incorrect terminal remaining lengths reject without changing output or files",
          "[gpd_snapshot]") {
  TerminalProbe model;
  const std::array<uint8_t, 4> payload{1, 2, 3, 4};
  model.AddImage(7, payload); // Real producer offset4/remainingMAX-4.
  model.SetTailSize(UINT32_MAX); // The former synthetic convention is invalid.
  std::vector<uint8_t> output{9, 8, 7};
  REQUIRE(model.SerializeChecked(output, xam::GpdSnapshotStore::kMaximumBytes) ==
          std::errc::bad_message);
  REQUIRE(output == std::vector<uint8_t>{9, 8, 7});
  // Both undersized and overflowing endpoint values retain all existing
  // generic table/used extent geometry. Neither may stage or replace data.
  for (uint32_t wrong : {UINT32_MAX, UINT32_MAX - 3, UINT32_MAX - 5}) {
    auto invalid = Wire();
    Put32(invalid, 64, wrong);
    RejectMalformedGpdSnapshot(invalid);
  }
}
TEST_CASE("Supplied read-only GPD fixture manifest loads and checked-serializes",
          "[.gpd_canonical_wire][gpd_snapshot]") {
  const char* root_env = std::getenv("XENIA_GPD_FIXTURE_ROOT");
  const char* manifest_env = std::getenv("XENIA_GPD_FIXTURE_MANIFEST");
  const char* expected_env = std::getenv("XENIA_GPD_FIXTURE_MANIFEST_SHA256");
  REQUIRE(root_env); REQUIRE(manifest_env); REQUIRE(expected_env);
  const std::filesystem::path root_path(root_env), manifest_path(manifest_env);
  REQUIRE(root_path.is_absolute()); REQUIRE(manifest_path.is_absolute());
  std::error_code error;
  auto manifest_owner = filesystem::HostStorageRoot::Open(
      manifest_path.parent_path(), true, error);
  REQUIRE_FALSE(error); REQUIRE(manifest_owner);
  const auto manifest = manifest_owner->CaptureSnapshot(
      manifest_path.filename(), 32 * 1024, error);
  REQUIRE_FALSE(error); REQUIRE(manifest); REQUIRE(manifest->exists());
  auto fingerprint = [](std::span<const uint8_t> bytes) {
    std::array<uint8_t, 32> hash;
    REQUIRE(mbedtls_sha256(bytes.data(), bytes.size(), hash.data(), 0) == 0);
    constexpr char hex[] = "0123456789abcdef";
    std::string result;
    result.reserve(64);
    for (uint8_t byte : hash) {
      result.push_back(hex[byte >> 4]); result.push_back(hex[byte & 15]);
    }
    return result;
  };
  REQUIRE(fingerprint(manifest->bytes()) == expected_env);
  std::istringstream records(std::string(manifest->bytes().begin(), manifest->bytes().end()));
  std::string magic, relative, xuid_text, count_text;
  REQUIRE(static_cast<bool>(std::getline(records, magic)));
  REQUIRE(magic == "XENIA_GPD_WIRE_FIXTURE_V1");
  REQUIRE(static_cast<bool>(std::getline(records, relative)));
  REQUIRE(static_cast<bool>(std::getline(records, xuid_text)));
  REQUIRE(static_cast<bool>(std::getline(records, count_text)));
  const std::filesystem::path profile_relative(relative);
  REQUIRE_FALSE(profile_relative.empty()); REQUIRE_FALSE(profile_relative.is_absolute());
  for (const auto& part : profile_relative) { REQUIRE(part != ".."); REQUIRE(part != "."); }
  size_t parsed = 0;
  REQUIRE(xuid_text.size() == 16);
  const auto xuid = std::stoull(xuid_text, &parsed, 16);
  REQUIRE(parsed == xuid_text.size()); REQUIRE(xuid != 0);
  const auto count = std::stoull(count_text, &parsed, 10);
  REQUIRE(parsed == count_text.size()); REQUIRE(count > 0); REQUIRE(count <= 128);
  auto root = filesystem::HostStorageRoot::Open(root_path, true, error);
  REQUIRE_FALSE(error); REQUIRE(root);
  auto profile = root->Subroot(profile_relative, true, error);
  REQUIRE_FALSE(error); REQUIRE(profile); REQUIRE(profile->read_only());
  for (size_t i = 0; i != count; ++i) {
    std::string title_text, leaf, size_text, expected_sha;
    REQUIRE(static_cast<bool>(records >> title_text >> leaf >> size_text >> expected_sha));
    INFO(leaf);
    REQUIRE(title_text.size() == 8);
    const auto title = std::stoul(title_text, &parsed, 16);
    REQUIRE(parsed == title_text.size()); REQUIRE(title > 0); REQUIRE(title <= UINT32_MAX);
    const auto size = std::stoull(size_text, &parsed, 10);
    REQUIRE(parsed == size_text.size()); REQUIRE(size <= xam::GpdSnapshotStore::kMaximumBytes);
    REQUIRE_FALSE(std::filesystem::path(leaf).has_parent_path());
    REQUIRE(expected_sha.size() == 64);
    const auto loaded = xam::GpdSnapshotStore::Load(
        {xuid, 1, static_cast<uint32_t>(title)}, profile);
    REQUIRE_FALSE(loaded.error); REQUIRE(loaded.readable());
    REQUIRE(loaded.token->relative() == leaf);
    const auto raw = loaded.token->bytes();
    REQUIRE(raw.size() == size); REQUIRE(fingerprint(raw) == expected_sha);
    xam::GpdInfo model(static_cast<uint32_t>(title),
                       std::vector<uint8_t>(raw.begin(), raw.end()));
    REQUIRE(model.IsValid());
    std::vector<uint8_t> serialized;
    REQUIRE_FALSE(model.SerializeChecked(serialized, xam::GpdSnapshotStore::kMaximumBytes));
    REQUIRE_FALSE(xam::ValidateGpdSnapshot(serialized));
    // All source access is read-only. No Publish/repair/default-profile write.
  }
  std::string extra;
  REQUIRE_FALSE(static_cast<bool>(records >> extra));
}
}  // namespace xe::kernel::test
#endif
