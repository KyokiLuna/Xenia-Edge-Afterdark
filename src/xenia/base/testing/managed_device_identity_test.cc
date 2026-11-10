/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project
 ******************************************************************************
 * Released under the BSD license - see LICENSE in the root for more details.
 */
#include "xenia/base/managed_device_identity.h"
#include "xenia/base/content_mutation_wire.h"
#include "xenia/base/content_package_mutation_wire.h"
#include <algorithm>
#include <array>
#include <limits>
#include <string>
#include <vector>
#include <fstream>
#include "xenia/base/testing/snapshot_fixture.h"
#if XE_PLATFORM_LINUX
#include <fcntl.h>
#include <sys/file.h>
namespace xe::filesystem {
// Actual pending epoch append/fsync and root retirement execute before the
// injected interruption. No native operation is replaced with success.
class ManagedDeviceIdentityTestAccess final {
 public:
  static ManagedDeviceFormatReceipt InterruptAfterDurableEpoch(
      std::shared_ptr<const ManagedDeviceIdentity> identity) {
    return ManagedDeviceIdentity::FormatInternal(
        std::move(identity), std::make_error_code(std::errc::io_error));
  }
};
namespace test {
namespace {
struct ManagedFixture {
  SnapshotScratch scratch;
  std::shared_ptr<HostStorageRoot> parent, source;
  ManagedFixture() {
    parent = scratch.Root();
    REQUIRE_FALSE(parent->Create("managed", true));
    std::error_code error;
    source = parent->Subroot("managed", false, error);
    REQUIRE(source);
    REQUIRE_FALSE(error);
  }
  auto Open(std::error_code& error) {
    return ManagedDeviceIdentity::Open(source, "managed-test-A", 1, error);
  }
  auto Fresh(std::error_code& error) {
    auto fresh = HostStorageRoot::Open(source->path(), false, error);
    REQUIRE(fresh);
    REQUIRE_FALSE(error);
    return fresh;
  }
  std::vector<uint8_t> Raw() const {
    std::ifstream input(source->path() / ManagedDeviceIdentity::kMarker,
                        std::ios::binary);
    REQUIRE(input.is_open());
    return {std::istreambuf_iterator<char>(input), {}};
  }
};
constexpr std::array<uint8_t, 5> kData{1, 2, 0, 4, 0xFF};
}  // namespace
TEST_CASE("Managed UUID and epoch reopen on the actual same directory",
          "[managed_identity]") {
  ManagedFixture f;
  std::error_code error;
  auto first = f.Open(error);
  REQUIRE(first);
  REQUIRE_FALSE(error);
  const auto uuid = first->uuid();
  const auto inode = first->directory_file_id();
  REQUIRE(first->content_epoch() == 1);
  REQUIRE_FALSE(first->retiring());
  REQUIRE(first->source() == f.source);
  REQUIRE_FALSE(first->CheckCurrent());
  REQUIRE(first->source_id() == "managed-test-A");
  REQUIRE(first->device_id() == 1);
  REQUIRE(first->configured_location() == f.source->path());
  first.reset();
  auto fresh = f.Fresh(error);
  auto reopened =
      ManagedDeviceIdentity::Open(fresh, "managed-test-A", 1, error);
  REQUIRE(reopened);
  REQUIRE_FALSE(error);
  REQUIRE(reopened->uuid() == uuid);
  REQUIRE(reopened->directory_file_id() == inode);
  REQUIRE(reopened->content_epoch() == 1);
  REQUIRE_FALSE(reopened->pending_format());
  REQUIRE_FALSE(reopened->CheckCurrent());
}
TEST_CASE(
    "Foreign configured bindings and copied managed markers are preserved",
    "[managed_identity][conflict]") {
  ManagedFixture f, other;
  std::error_code error;
  auto identity = f.Open(error);
  REQUIRE(identity);
  REQUIRE_FALSE(error);
  const auto raw = f.Raw();
  identity.reset();
  auto denied = ManagedDeviceIdentity::Open(f.source, "other-source", 1, error);
  REQUIRE_FALSE(denied);
  REQUIRE(error == std::errc::state_not_recoverable);
  denied = ManagedDeviceIdentity::Open(f.source, "managed-test-A", 2, error);
  REQUIRE_FALSE(denied);
  REQUIRE(error == std::errc::state_not_recoverable);
  REQUIRE(f.Raw() == raw);
  std::filesystem::copy_file(
      f.source->path() / ManagedDeviceIdentity::kMarker,
      other.source->path() / ManagedDeviceIdentity::kMarker);
  denied = other.Open(error);
  REQUIRE_FALSE(denied);
  REQUIRE(error == std::errc::state_not_recoverable);
  REQUIRE(other.Raw() == raw);
}
TEST_CASE(
    "Managed marker refuses symlinks hardlinks and case aliases without "
    "adoption",
    "[managed_identity][confinement]") {
  ManagedFixture f;
  std::error_code error;
  auto identity = f.Open(error);
  REQUIRE(identity);
  const auto raw = f.Raw();
  identity.reset();
  const auto marker = f.source->path() / ManagedDeviceIdentity::kMarker;
  const auto kept = f.source->path() / "kept-marker";
  std::filesystem::rename(marker, kept);
  std::filesystem::create_symlink(kept.filename(), marker);
  REQUIRE_FALSE(f.Open(error));
  REQUIRE(error);
  REQUIRE(std::filesystem::is_symlink(marker));
  std::filesystem::remove(marker);
  std::filesystem::rename(kept, marker);
  std::filesystem::create_hard_link(marker, kept);
  REQUIRE_FALSE(f.Open(error));
  REQUIRE(error == std::errc::permission_denied);
  REQUIRE(f.Raw() == raw);
  std::filesystem::remove(kept);
  REQUIRE_FALSE(f.source->WriteNewFile(".XENIA-MANAGED-SOURCE-V1", raw).error);
  REQUIRE_FALSE(f.Open(error));
  REQUIRE(error == std::errc::file_exists);
  REQUIRE(f.Raw() == raw);
}
TEST_CASE(
    "Common guest operations cannot consume or remove a leased source marker",
    "[managed_identity][lease]") {
  ManagedFixture f;
  std::error_code error;
  auto identity = f.Open(error);
  REQUIRE(identity);
  REQUIRE_FALSE(error);
  auto guest = f.source->GuestView(error);
  REQUIRE(guest);
  REQUIRE_FALSE(error);
  auto opened = guest->OpenFile(ManagedDeviceIdentity::kMarker,
                                FileAccess::kGenericRead, error);
  REQUIRE_FALSE(opened);
  REQUIRE(error == std::errc::device_or_resource_busy);
  REQUIRE(guest->Remove(ManagedDeviceIdentity::kMarker, false) ==
          std::errc::device_or_resource_busy);
  REQUIRE(guest->Rename(ManagedDeviceIdentity::kMarker, "moved", false) ==
          std::errc::device_or_resource_busy);
  REQUIRE_FALSE(guest->MapRead(ManagedDeviceIdentity::kMarker, 0, 8, error));
  REQUIRE(error == std::errc::device_or_resource_busy);
  auto competing = f.Open(error);
  REQUIRE_FALSE(competing);
  REQUIRE(error);
  REQUIRE_FALSE(identity->CheckCurrent());
}
TEST_CASE(
    "Managed format preserves UUID marker and retires the previous content "
    "epoch",
    "[managed_identity][format]") {
  ManagedFixture f;
  std::error_code error;
  REQUIRE_FALSE(f.source->Create("nested", true));
  REQUIRE_FALSE(f.source->WriteNewFile("a.bin", kData).error);
  REQUIRE_FALSE(f.source->WriteNewFile("nested/b.bin", kData).error);
  auto identity = f.Open(error);
  REQUIRE(identity);
  REQUIRE_FALSE(error);
  const auto uuid = identity->uuid();
  struct stat before;
  REQUIRE(stat((f.source->path() / ManagedDeviceIdentity::kMarker).c_str(),
               &before) == 0);
  const auto result = ManagedDeviceIdentity::Format(identity);
  REQUIRE_FALSE(result.error);
  REQUIRE(result.epoch_published);
  REQUIRE(result.epoch_durable);
  REQUIRE(result.data_started);
  REQUIRE(result.complete);
  REQUIRE(result.removed_entries == 3);
  REQUIRE(result.current);
  REQUIRE(result.current->uuid() == uuid);
  REQUIRE(result.current->content_epoch() == 2);
  REQUIRE_FALSE(result.current->CheckCurrent());
  REQUIRE(identity->retiring());
  REQUIRE(identity->cancellation()->load());
  REQUIRE(identity->CheckCurrent() == std::errc::operation_canceled);
  REQUIRE_FALSE(f.source->Observe().available);
  struct stat after;
  REQUIRE(stat((f.source->path() / ManagedDeviceIdentity::kMarker).c_str(),
               &after) == 0);
  REQUIRE(before.st_ino == after.st_ino);
  REQUIRE(before.st_dev == after.st_dev);
  REQUIRE_FALSE(std::filesystem::exists(f.source->path() / "a.bin"));
  REQUIRE_FALSE(std::filesystem::exists(f.source->path() / "nested"));
}
TEST_CASE(
    "Actual open files and source directory owners must drain before epoch "
    "mutation",
    "[managed_identity][format][lifetime]") {
  ManagedFixture f;
  std::error_code error;
  REQUIRE_FALSE(f.source->WriteNewFile("a.bin", kData).error);
  auto identity = f.Open(error);
  REQUIRE(identity);
  const auto raw = f.Raw();
  auto held = f.source->OpenFile("a.bin", FileAccess::kGenericRead, error);
  REQUIRE(held);
  auto denied = ManagedDeviceIdentity::Format(identity);
  REQUIRE(denied.error == std::errc::device_or_resource_busy);
  REQUIRE_FALSE(denied.epoch_published);
  REQUIRE_FALSE(denied.data_started);
  REQUIRE_FALSE(identity->retiring());
  REQUIRE(f.Raw() == raw);
  held.reset();
  const int directory =
      open(f.source->path().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  REQUIRE(directory >= 0);
  REQUIRE(flock(directory, LOCK_EX | LOCK_NB) == 0);
  denied = ManagedDeviceIdentity::Format(identity);
  REQUIRE(denied.error);
  REQUIRE_FALSE(denied.epoch_published);
  REQUIRE_FALSE(identity->retiring());
  REQUIRE(f.Raw() == raw);
  close(directory);
  auto complete = ManagedDeviceIdentity::Format(identity);
  REQUIRE_FALSE(complete.error);
  REQUIRE(complete.complete);
}
TEST_CASE(
    "Durable pending epoch blocks normal admission and explicit format resumes",
    "[managed_identity][format][recovery]") {
  ManagedFixture f;
  std::error_code error;
  REQUIRE_FALSE(f.source->WriteNewFile("a.bin", kData).error);
  auto identity = f.Open(error);
  REQUIRE(identity);
  const auto uuid = identity->uuid();
  const auto interrupted =
      ManagedDeviceIdentityTestAccess::InterruptAfterDurableEpoch(identity);
  REQUIRE(interrupted.error == std::errc::io_error);
  REQUIRE(interrupted.epoch_published);
  REQUIRE(interrupted.epoch_durable);
  REQUIRE_FALSE(interrupted.data_started);
  REQUIRE_FALSE(interrupted.complete);
  REQUIRE(identity->retiring());
  REQUIRE(std::filesystem::exists(f.source->path() / "a.bin"));
  auto fresh = f.Fresh(error);
  auto pending = ManagedDeviceIdentity::Open(fresh, "managed-test-A", 1, error);
  REQUIRE(pending);
  REQUIRE(error == std::errc::operation_in_progress);
  REQUIRE(pending->pending_format());
  REQUIRE(pending->retiring());
  REQUIRE(pending->uuid() == uuid);
  REQUIRE(pending->content_epoch() == 2);
  REQUIRE(pending->CheckCurrent() == std::errc::operation_canceled);
  const auto completed = ManagedDeviceIdentity::Format(pending);
  REQUIRE_FALSE(completed.error);
  REQUIRE(completed.complete);
  REQUIRE(completed.current);
  REQUIRE_FALSE(completed.epoch_published);
  REQUIRE(completed.epoch_observed);
  REQUIRE(completed.epoch_durable);
  REQUIRE(completed.current->uuid() == uuid);
  REQUIRE(completed.current->content_epoch() == 2);
  REQUIRE_FALSE(std::filesystem::exists(f.source->path() / "a.bin"));
}
TEST_CASE(
    "Truncated and corrupt source records never authorize empty or repaired "
    "identity",
    "[managed_identity][corruption]") {
  ManagedFixture f;
  std::error_code error;
  REQUIRE_FALSE(f.source->WriteNewFile("a.bin", kData).error);
  auto identity = f.Open(error);
  REQUIRE(identity);
  identity.reset();
  auto raw = f.Raw();
  raw.resize(raw.size() - 1);
  {
    std::ofstream out(f.source->path() / ManagedDeviceIdentity::kMarker,
                      std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(raw.data()), raw.size());
    out.close();
    REQUIRE_FALSE(out.fail());
  }
  REQUIRE_FALSE(f.Open(error));
  REQUIRE(error == std::errc::bad_message);
  REQUIRE(f.Raw() == raw);
  REQUIRE(std::filesystem::exists(f.source->path() / "a.bin"));
}
TEST_CASE(
    "Managed source cannot cover a live protected private recovery authority",
    "[managed_identity][protection]") {
  SnapshotScratch scratch;
  auto root = scratch.Root();
  auto recovery = SnapshotRecoveryOwner::Open(root);
  REQUIRE_FALSE(recovery->error());
  std::error_code error;
  auto denied = ManagedDeviceIdentity::Open(root, "invalid-overlap", 1, error);
  REQUIRE_FALSE(denied);
  REQUIRE(error == std::errc::permission_denied);
  REQUIRE_FALSE(
      std::filesystem::exists(root->path() / ManagedDeviceIdentity::kMarker));
  REQUIRE(std::filesystem::exists(recovery->root()->path() /
                                  SnapshotRecoveryOwner::kMarker));
}
TEST_CASE(
    "Live marker damage and root retirement refuse without formatting user "
    "data",
    "[managed_identity][currentness]") {
  ManagedFixture f;
  std::error_code error;
  REQUIRE_FALSE(f.source->WriteNewFile("a.bin", kData).error);
  auto identity = f.Open(error);
  REQUIRE(identity);
  REQUIRE_FALSE(error);
  auto raw = f.Raw();
  raw[20] ^= 1;
  const int fd =
      open((f.source->path() / ManagedDeviceIdentity::kMarker).c_str(),
           O_WRONLY | O_NOFOLLOW | O_CLOEXEC);
  REQUIRE(fd >= 0);
  REQUIRE(pwrite(fd, raw.data(), raw.size(), 0) == ssize_t(raw.size()));
  REQUIRE(close(fd) == 0);
  REQUIRE(identity->CheckCurrent() == std::errc::state_not_recoverable);
  auto refused = ManagedDeviceIdentity::Format(identity);
  REQUIRE(refused.error == std::errc::state_not_recoverable);
  REQUIRE_FALSE(refused.epoch_published);
  REQUIRE_FALSE(refused.data_started);
  REQUIRE(f.Raw() == raw);
  REQUIRE(std::filesystem::exists(f.source->path() / "a.bin"));
  identity->Retire();
  REQUIRE(identity->retiring());
  REQUIRE(identity->CheckCurrent() == std::errc::operation_canceled);
  f.source->Retire();
  REQUIRE(identity->CheckCurrent());
  REQUIRE_FALSE(
      std::filesystem::exists(f.source->path() / "unexpected-repair"));
}
}  // namespace test
}  // namespace xe::filesystem
#endif

// Pure codec cases also compile on platforms without native managed storage.
namespace xe::filesystem::test {
namespace {
namespace cmw = content_mutation_wire;
cmw::Record ContentWireRecord(cmw::Kind kind = cmw::Kind::kHeader,
                              bool before = true) {
  cmw::Record r;
  for (size_t i = 0; i < r.identity.uuid.size(); ++i) {
    r.identity.uuid[i] = uint8_t(i + 1);
  }
  r.identity.epoch = 42;
  r.identity.content_directory = 1001;
  r.identity.device = 3;
  r.identity.source = "managed-wire-source";
  r.kind = kind;
  r.nonce = "0123456789abcdef0123456789abcdef";
  r.package = "0123456789aBcDeF/Ab12Cd34/00000001/Save.Name";
  r.target = kind == cmw::Kind::kThumbnail
      ? r.package / "__thumbnail.png"
      : std::filesystem::path("0123456789aBcDeF/Ab12Cd34/Headers/00000001/Save.Name.header");
  r.before_exists = before;
  r.before_file = before ? 1002 : 0;
  r.stage_file = 1003;
  r.recovery_file = before ? 1004 : 0;
  if (before) { r.before = {0xff, 0, 0x81, 4, 0, 0x7f}; }
  r.intended = {0, 0xa5, 0xff, 0x80, 1, 0, 0x12, 0x34};
  r.stage = cmw::Target(r).parent_path() /
      (".xenia-stage-" + r.nonce + ".tmp");
  if (before) { r.recovery = "snapshot-" + r.nonce + ".bin"; }
  r.directory_stage = kind == cmw::Kind::kCreate ? 1005 : 0;
  return r;
}
bool SameContentWireRecord(const cmw::Record& a, const cmw::Record& b) {
  return a.identity == b.identity && a.kind == b.kind && a.nonce == b.nonce &&
      a.package == b.package && a.target == b.target &&
      a.before_exists == b.before_exists &&
      a.before_file == b.before_file && a.stage_file == b.stage_file &&
      a.recovery_file == b.recovery_file && a.before == b.before &&
      a.intended == b.intended && a.stage == b.stage &&
      a.recovery == b.recovery && a.directory_stage == b.directory_stage;
}
void ContentWireInteger(std::vector<uint8_t>& bytes, size_t offset,
                        size_t width, uint64_t value) {
  REQUIRE(offset + width <= bytes.size());
  for (size_t i = 0; i < width; ++i) {
    bytes[offset + i] = uint8_t(value >> ((width - 1 - i) * 8));
  }
}
void ContentWireRehash(std::vector<uint8_t>& bytes) {
  REQUIRE(bytes.size() >= 32);
  std::error_code error;
  const auto hash = cmw::Digest(
      std::span<const uint8_t>(bytes).first(bytes.size() - 32), error);
  REQUIRE_FALSE(error);
  std::copy(hash.begin(), hash.end(), bytes.end() - 32);
}
void ContentWireRefuses(const std::vector<uint8_t>& bytes) {
  auto output = ContentWireRecord(cmw::Kind::kCreate, false);
  output.identity.source = "unchanged sentinel";
  output.intended = {9, 8, 7, 0, 0xff};
  const auto saved = output;
  std::error_code error;
  REQUIRE_FALSE(cmw::Decode(bytes, output, error));
  REQUIRE(error);
  REQUIRE(SameContentWireRecord(output, saved));
}
}  // namespace

TEST_CASE("Content mutation wire preserves opaque metadata and physical case",
          "[managed_identity][content_mutation_wire]") {
  for (auto kind : {cmw::Kind::kHeader, cmw::Kind::kThumbnail, cmw::Kind::kCreate}) {
    for (bool before : {false, true}) {
      for (bool empty : {false, true}) {
        auto r = ContentWireRecord(kind, before);
        if (empty) { r.before.clear(); r.intended.clear(); }
        std::error_code error = std::make_error_code(std::errc::io_error);
        const auto bytes = cmw::Encode(r, error);
        REQUIRE_FALSE(error);
        REQUIRE(bytes.size() >= 196);
        REQUIRE(bytes[8] == 0);
        REQUIRE(bytes[11] == 2);  // Big-endian version2.
        REQUIRE(bytes[12] == 0);
        REQUIRE(bytes[15] == uint8_t(kind));
        uint64_t length = 0;
        for (size_t i = 16; i < 24; ++i) { length = (length << 8) | bytes[i]; }
        REQUIRE(length == bytes.size());
        cmw::Record decoded;
        REQUIRE(cmw::Decode(bytes, decoded, error));
        REQUIRE_FALSE(error);
        REQUIRE(SameContentWireRecord(decoded, r));
        REQUIRE(cmw::Encode(decoded, error) == bytes);
        REQUIRE_FALSE(error);
        const auto expected = kind == cmw::Kind::kThumbnail
            ? std::filesystem::path("0123456789aBcDeF/Ab12Cd34/00000001/Save.Name/__thumbnail.png")
            : std::filesystem::path("0123456789aBcDeF/Ab12Cd34/Headers/00000001/Save.Name.header");
        REQUIRE(cmw::Target(decoded) == expected);
        if (before && empty) {
          REQUIRE(decoded.before_exists);
          REQUIRE(decoded.before.empty());
          REQUIRE(decoded.before_file != 0);
          REQUIRE(decoded.recovery_file != 0);
        }
      }
    }
  }
  auto boundary = ContentWireRecord(cmw::Kind::kCreate, true);
  boundary.identity.source.assign(256, 's');
  boundary.package = std::filesystem::path("0123456789aBcDeF/Ab12Cd34/00000001") /
      std::string(42, 'N');
  boundary.target = std::filesystem::path("0123456789aBcDeF/Ab12Cd34/Headers/00000001") /
      (std::string(42, 'N') + ".header");
  boundary.stage = cmw::Target(boundary).parent_path() /
      (".xenia-stage-" + boundary.nonce + ".tmp");
  std::error_code boundary_error;
  const auto boundary_bytes = cmw::Encode(boundary, boundary_error);
  REQUIRE_FALSE(boundary_error);
  cmw::Record boundary_decoded;
  REQUIRE(cmw::Decode(boundary_bytes, boundary_decoded, boundary_error));
  REQUIRE_FALSE(boundary_error);
  REQUIRE(SameContentWireRecord(boundary_decoded, boundary));
  const std::array<uint8_t, 3> abc{'a', 'b', 'c'};
  const std::array<uint8_t, 32> sha256{
      0xba,0x78,0x16,0xbf,0x8f,0x01,0xcf,0xea,0x41,0x41,0x40,0xde,0x5d,0xae,0x22,0x23,
      0xb0,0x03,0x61,0xa3,0x96,0x17,0x7a,0x9c,0xb4,0x10,0xff,0x61,0xf2,0x00,0x15,0xad};
  std::error_code error;
  REQUIRE(cmw::Digest(abc, error) == sha256);
  REQUIRE_FALSE(error);
}

TEST_CASE("Content mutation wire refuses unsafe names kinds and identity aliases",
          "[managed_identity][content_mutation_wire]") {
  const auto valid = ContentWireRecord();
  const std::string prefix = "0123456789aBcDeF/Ab12Cd34/00000001/";
  for (const auto& leaf : {std::string{}, std::string("."), std::string(".."),
       std::string("bad/name"), std::string("bad\\name"), std::string("bad*name"),
       std::string(43, 'a'), std::string("bad\nname"), std::string("bad\0name", 8)}) {
    REQUIRE_FALSE(cmw::ValidPackage(std::filesystem::path(prefix + leaf)));
  }
  REQUIRE(cmw::ValidPackage(std::filesystem::path(prefix + std::string(42, 'a'))));
  for (const auto& package : {"/0123456789abcdef/12345678/00000001/name",
       "../0123456789abcdef/12345678/00000001/name",
       "0123456789abcdeg/12345678/00000001/name",
       "0123456789abcdef/1234567/00000001/name",
       "0123456789abcdef/12345678/00000001/name/extra",
       "0123456789abcdef//12345678/00000001/name"}) {
    REQUIRE_FALSE(cmw::ValidPackage(package));
  }
  REQUIRE_FALSE(cmw::ValidNonce(std::string(32, '0')));
  REQUIRE_FALSE(cmw::ValidNonce(std::string(32, 'A')));
  REQUIRE_FALSE(cmw::ValidNonce(valid.nonce + "0"));
  for (unsigned mutation = 0; mutation < 19; ++mutation) {
    auto r = valid;
    switch (mutation) {
      case 0: r.identity.uuid.fill(0); break;
      case 1: r.identity.epoch = 0; break;
      case 2: r.identity.content_directory = 0; break;
      case 3: r.identity.device = 0; break;
      case 4: r.identity.source.clear(); break;
      case 5: r.identity.source.assign(257, 's'); break;
      case 6: r.identity.source = std::string("bad\0source", 10); break;
      case 7: r.kind = cmw::Kind(0); break;
      case 8: r.nonce[0] = 'A'; break;
      case 9: r.stage_file = 0; break;
      case 10: r.recovery_file = 0; break;
      case 11: r.before_file = 0; break;
      case 12: r.stage = "../escape.tmp"; break;
      case 13: r.recovery = "dir/" + r.recovery.string(); break;
      case 14: r.directory_stage = 1005; break;
      case 15: r.kind = cmw::Kind::kCreate; break;
      case 16: r.before_exists = false; break;
      case 17: r.stage_file = r.before_file; break;
      case 18: r.before_file = r.identity.content_directory; break;
    }
    const auto saved = r;
    std::error_code error;
    REQUIRE(cmw::Encode(r, error).empty());
    REQUIRE(error);
    REQUIRE(SameContentWireRecord(r, saved));
  }
  auto absent = ContentWireRecord(cmw::Kind::kThumbnail, false);
  absent.before.push_back(1);
  std::error_code error;
  REQUIRE(cmw::Encode(absent, error).empty());
  REQUIRE(error);
}

TEST_CASE("Content mutation decode checks authenticated lengths fields and paths",
          "[managed_identity][content_mutation_wire]") {
  const auto record = ContentWireRecord();
  std::error_code error;
  const auto bytes = cmw::Encode(record, error);
  REQUIRE_FALSE(error);
  for (size_t length = 0; length < bytes.size(); ++length) {
    ContentWireRefuses(std::vector<uint8_t>(bytes.begin(), bytes.begin() + length));
  }
  auto tampered = bytes;
  tampered[tampered.size() - 33] ^= 1;
  ContentWireRefuses(tampered);
  auto trailing = bytes;
  trailing.push_back(0);
  ContentWireRefuses(trailing);
  auto old_version = bytes;
  old_version[6] = '1';
  ContentWireInteger(old_version, 8, 4, 1);
  ContentWireRehash(old_version);
  ContentWireRefuses(old_version);
  // Recompute checksum to exercise semantic/length rejection independently
  // of random corruption. No huge allocation is needed for hostile lengths.
  const std::array<std::array<uint64_t, 3>, 19> bad_fields{{
      {{8,4,3}}, {{12,4,99}}, {{12,4,2}}, {{16,8,0}}, {{40,8,0}},
      {{48,8,0}}, {{56,4,0}}, {{60,4,2}}, {{64,8,0}}, {{72,8,0}},
      {{80,8,0}}, {{88,8,1005}}, {{96,4,257}}, {{100,4,257}},
      {{104,4,257}}, {{108,4,257}}, {{112,4,46}},
      {{116,8,cmw::kMaximumMetadataBytes + 1}},
      {{124,8,std::numeric_limits<uint64_t>::max()}}
  }};
  for (const auto& field : bad_fields) {
    auto malformed = bytes;
    ContentWireInteger(malformed, size_t(field[0]), size_t(field[1]), field[2]);
    ContentWireRehash(malformed);
    ContentWireRefuses(malformed);
  }
  for (unsigned mutation = 0; mutation < 5; ++mutation) {
    auto malformed = bytes;
    const size_t package = 164 + record.identity.source.size();
    const size_t stage = package + record.package.generic_string().size() +
        record.target.generic_string().size();
    switch (mutation) {
      case 0: std::fill(malformed.begin() + 24, malformed.begin() + 40, 0); break;
      case 1: malformed[164] = 0; break;
      case 2: malformed[package] = '.'; break;
      case 3: malformed[stage] = '.'; malformed[stage + 1] = '.';
              malformed[stage + 2] = '/'; break;
      case 4: malformed[package + 16] = '\\'; break;
    }
    ContentWireRehash(malformed);
    ContentWireRefuses(malformed);
  }
}


TEST_CASE("Content mutation wire preserves validated physical target spelling",
          "[managed_identity][content_mutation_wire]") {
  for (auto kind : {cmw::Kind::kHeader, cmw::Kind::kThumbnail, cmw::Kind::kCreate}) {
    auto r = ContentWireRecord(kind, true);
    r.target = kind == cmw::Kind::kThumbnail
        ? std::filesystem::path("0123456789ABCDEF/ab12cd34/00000001/sAVE.nAME/__ThUmBnAiL.PnG")
        : std::filesystem::path("0123456789ABCDEF/ab12cd34/headers/00000001/sAVE.nAME.HeAdEr");
    r.stage = r.target.parent_path() / (".xenia-stage-" + r.nonce + ".tmp");
    REQUIRE(cmw::Target(r) == r.target);
    std::error_code error;
    const auto bytes = cmw::Encode(r, error);
    REQUIRE_FALSE(error);
    cmw::Record decoded;
    REQUIRE(cmw::Decode(bytes, decoded, error));
    REQUIRE_FALSE(error);
    REQUIRE(SameContentWireRecord(decoded, r));
    REQUIRE(cmw::Target(decoded) == r.target);
    REQUIRE(decoded.stage.parent_path() == decoded.target.parent_path());
    const auto done = cmw::Done(bytes, error);
    REQUIRE_FALSE(error);
    const auto same_slot_different_spelling = cmw::Encode(ContentWireRecord(kind), error);
    REQUIRE_FALSE(error);
    REQUIRE_FALSE(cmw::CheckDone(done, same_slot_different_spelling, error));
    REQUIRE(error);
    REQUIRE(cmw::Encode(decoded, error) == bytes);
    REQUIRE_FALSE(error);
    for (unsigned mutation = 0; mutation < 5; ++mutation) {
      auto invalid = r;
      switch (mutation) {
        case 0: invalid.target.clear(); break;
        case 1: invalid.target = invalid.target.parent_path() / "other.header"; break;
        case 2: invalid.target = "../" + invalid.target.generic_string(); break;
        case 3: invalid.target = "0123456789ABCDEF/ab12cd34/00000002/other/__thumbnail.png"; break;
        case 4: invalid.stage = ContentWireRecord(kind).stage; break;
      }
      const auto saved = invalid;
      REQUIRE(cmw::Encode(invalid, error).empty());
      REQUIRE(error);
      REQUIRE(SameContentWireRecord(invalid, saved));
      if (mutation < 4) { REQUIRE(cmw::Target(invalid).empty()); }
    }
    // Keep the checksum valid while making the exact physical target differ
    // semantically; failure must preserve every output field, including target.
    auto malformed = bytes;
    const size_t target = 164 + r.identity.source.size() +
        r.package.generic_string().size();
    malformed[target] = 'f';  // Still hex, but a different source-local XUID.
    ContentWireRehash(malformed);
    ContentWireRefuses(malformed);
  }
}

TEST_CASE("Content mutation done binds exact prepared bytes and no foreign record",
          "[managed_identity][content_mutation_wire]") {
  std::error_code error;
  const auto first = cmw::Encode(ContentWireRecord(), error);
  REQUIRE_FALSE(error);
  const auto done = cmw::Done(first, error);
  REQUIRE_FALSE(error);
  REQUIRE(done.size() == 88);
  REQUIRE(cmw::CheckDone(done, first, error));
  REQUIRE_FALSE(error);
  auto foreign = ContentWireRecord();
  foreign.intended.back() ^= 1;
  const auto other = cmw::Encode(foreign, error);
  REQUIRE_FALSE(error);
  REQUIRE_FALSE(cmw::CheckDone(done, other, error));
  REQUIRE(error);
  auto bad_prepared = first;
  bad_prepared.back() ^= 1;
  REQUIRE(cmw::Done(bad_prepared, error).empty());
  REQUIRE(error);
  REQUIRE_FALSE(cmw::CheckDone(done, bad_prepared, error));
  REQUIRE(error);
  for (unsigned mutation = 0; mutation < 3; ++mutation) {
    auto bad_done = done;
    if (mutation == 0) { bad_done.pop_back(); }
    if (mutation == 1) { bad_done.push_back(0); }
    if (mutation == 2) { bad_done[24] ^= 1; }
    REQUIRE_FALSE(cmw::CheckDone(bad_done, first, error));
    REQUIRE(error);
  }
}
}  // namespace xe::filesystem::test

#if XE_PLATFORM_LINUX || XE_PLATFORM_MAC
// Pure package records. These arrays and checksums confer no live tree lease.
namespace xe::filesystem::test {
namespace {
namespace cpw = content_package_mutation_wire;
ContentPackageTreeEntry PackageEntry(std::string path, uint64_t file, bool directory) {
  ContentPackageTreeEntry entry;
  entry.relative = std::filesystem::path(std::move(path));
  entry.file = file; entry.bytes = directory ? 96 : 12345;
  entry.write_seconds = -7; entry.write_nanoseconds = 999999999;
  entry.mode = directory ? 0040700u : 0100600u;
  entry.directory = directory; return entry;
}
cpw::Record PackageWireRecord(cpw::Kind kind = cpw::Kind::kDelete,
                               bool directory = true, bool header = true) {
  cpw::Record r;
  r.identity = ContentWireRecord().identity;
  r.kind = kind; r.nonce = "0123456789abcdef0123456789abcdef";
  r.package = "0123456789aBcDeF/Ab12Cd34/00000001/Save.Name";
  r.op_directory = 2000;
  r.header_target = "0123456789ABCDEF/ab12cd34/headers/00000001/sAVE.nAME.HeAdEr";
  r.header_exists = header; r.header_file = header ? 2001 : 0;
  if (header) { r.header = {0, 0xff, 0x80, 1, 0, 4, 0xa5}; }
  r.before.push_back(PackageEntry({}, 3000, directory));
  if (directory) {
    r.before.push_back(PackageEntry("Data", 3001, true));
    r.before.push_back(PackageEntry("Data/Slot", 3002, false));
    r.before.push_back(PackageEntry("__ThUmBnAiL.PnG", 3003, false));
    r.before.push_back(PackageEntry(std::string("native-") + char(0xff), 3004, false));
  } else if (kind == cpw::Kind::kTruncate) { r.replacement_directory = 2100; }
  return r;
}
void PackageWireRefuses(const std::vector<uint8_t>& bytes) {
  auto output = PackageWireRecord(cpw::Kind::kTruncate, false, false);
  output.identity.source = "unchanged package sentinel";
  const auto saved = output;
  std::error_code error;
  REQUIRE_FALSE(cpw::Decode(bytes, output, error));
  REQUIRE(error); REQUIRE(output == saved);
}
std::vector<size_t> PackageEntryOffsets(const cpw::Record& r) {
  size_t offset = 168 + r.identity.source.size() + r.package.native().size() +
      r.header_target.native().size() + r.preserved_leaf.native().size() + r.header.size();
  std::vector<size_t> offsets;
  for (const auto& entry : r.before) {
    offsets.push_back(offset); offset += 48 + entry.relative.native().size();
  }
  return offsets;
}
}  // namespace

TEST_CASE("Package mutation wire preserves native tree bytes and complete headers",
          "[managed_identity][content_package_mutation_wire]") {
  for (auto kind : {cpw::Kind::kDelete, cpw::Kind::kTruncate}) {
    for (bool directory : {false, true}) {
      for (bool header : {false, true}) {
        for (bool empty : {false, true}) {
          auto r = PackageWireRecord(kind, directory, header);
          if (empty) { r.header.clear(); }
          if (kind == cpw::Kind::kTruncate && directory) { r.preserved_leaf = "__ThUmBnAiL.PnG"; }
          r.before.front().write_seconds = std::numeric_limits<int64_t>::min();
          r.before.back().write_seconds = std::numeric_limits<int64_t>::max();
          const auto saved = r;
          std::error_code error = std::make_error_code(std::errc::io_error);
          const auto bytes = cpw::Encode(r, error);
          REQUIRE_FALSE(error); REQUIRE(bytes.size() >= 248);
          REQUIRE(std::string(bytes.begin(), bytes.begin() + 8) == "XCPKG03\n");
          REQUIRE(bytes[11] == 3); REQUIRE(bytes[15] == uint8_t(kind));
          cpw::Record decoded;
          REQUIRE(cpw::Decode(bytes, decoded, error)); REQUIRE_FALSE(error);
          REQUIRE(decoded == r); REQUIRE(r == saved);
          REQUIRE(cpw::Encode(decoded, error) == bytes); REQUIRE_FALSE(error);
          if (header && empty) { REQUIRE(decoded.header_exists); REQUIRE(decoded.header_file != 0); REQUIRE(decoded.header.empty()); }
        }
      }
    }
  }
}

TEST_CASE("Package mutation wire rejects tree aliases hierarchy and kind contradictions",
          "[managed_identity][content_package_mutation_wire]") {
  const auto valid = PackageWireRecord(cpw::Kind::kTruncate);
  for (unsigned mutation = 0; mutation < 29; ++mutation) {
    auto r = valid;
    switch (mutation) {
      case 0: r.kind = cpw::Kind(0); break;
      case 1: r.kind = cpw::Kind(3); break;
      case 2: r.op_directory = 0; break;
      case 3: r.op_directory = r.identity.content_directory; break;
      case 4: r.op_directory = r.before[0].file; break;
      case 5: r.header_file = r.op_directory; break;
      case 6: r.header_file = r.before[1].file; break;
      case 7: r.header_exists = false; break;
      case 8: r.header_file = 0; break;
      case 9: r.before[2].file = r.before[1].file; break;
      case 10: r.before[2].file = r.identity.content_directory; break;
      case 11: r.before[2].file = 0; break;
      case 12: std::swap(r.before[1], r.before[2]); break;
      case 13: r.before[2].relative = "Missing/Slot"; break;
      case 14: r.before.push_back(PackageEntry("__ThUmBnAiL.PnG/child", 3100, false)); break;
      case 15: r.before.push_back(PackageEntry("data", 3100, true)); break;
      case 16: r.before[2].relative = r.before[1].relative; break;
      case 17: r.before[0].relative = "root"; break;
      case 18: r.before[2].relative.clear(); break;
      case 19: r.before[2].write_nanoseconds = 1000000000; break;
      case 20: r.before[2].mode = 0120600; break;  // Symlink.
      case 21: r.before[2].mode = 0040700; break;  // Type/flag mismatch.
      case 22: r.before[2].mode |= 0x80000000u; break;
      case 23: r.preserved_leaf = "__thumbnail.png"; break;  // Wrong physical case.
      case 24: r.preserved_leaf = "Data/Slot"; break;
      case 25: r.preserved_leaf = "Data"; break;
      case 26: r.replacement_directory = 2100; break;
      case 27: r.header_target = "0123456789abcdef/ab12cd34/Headers/00000002/Save.Name.header"; break;
      case 28: r.preserved_leaf = "__ThUmBnAiL.PnG";
               r.before[3].directory = true; r.before[3].mode = 0040700; break;
    }
    const auto saved = r;
    std::error_code error;
    REQUIRE(cpw::Encode(r, error).empty()); REQUIRE(error); REQUIRE(r == saved);
  }
  for (unsigned mutation = 0; mutation < 5; ++mutation) {
    auto r = PackageWireRecord(cpw::Kind::kTruncate, false);
    switch (mutation) {
      case 0: r.replacement_directory = 0; break;
      case 1: r.replacement_directory = r.op_directory; break;
      case 2: r.replacement_directory = r.before.front().file; break;
      case 3: r.preserved_leaf = "__thumbnail.png"; break;
      case 4: r.kind = cpw::Kind::kDelete; break;
    }
    std::error_code error;
    REQUIRE(cpw::Encode(r, error).empty()); REQUIRE(error);
  }
  auto deletion = PackageWireRecord(); deletion.preserved_leaf = "__ThUmBnAiL.PnG";
  std::error_code error; REQUIRE(cpw::Encode(deletion, error).empty()); REQUIRE(error);
}

TEST_CASE("Package mutation decoding rejects rehashed bounds and semantic corruption",
          "[managed_identity][content_package_mutation_wire]") {
  auto record = PackageWireRecord(cpw::Kind::kTruncate);
  record.preserved_leaf = "__ThUmBnAiL.PnG";
  std::error_code error;
  const auto bytes = cpw::Encode(record, error); REQUIRE_FALSE(error);
  for (size_t length = 0; length < bytes.size(); ++length) {
    PackageWireRefuses(std::vector<uint8_t>(bytes.begin(), bytes.begin() + length));
  }
  auto bad = bytes; bad.back() ^= 1; PackageWireRefuses(bad);
  bad = bytes; bad.push_back(0); PackageWireRefuses(bad);
  const std::array<std::array<uint64_t, 3>, 21> fields{{
      {{8,4,2}}, {{12,4,99}}, {{16,8,cpw::kMaximumRecordBytes + 1}},
      {{40,8,0}}, {{48,8,0}}, {{56,4,0}}, {{60,4,2}}, {{64,8,0}},
      {{72,8,record.before.front().file}}, {{80,8,2100}}, {{88,4,257}},
      {{92,4,0}}, {{96,4,257}}, {{100,4,16}}, {{104,4,0}},
      {{104,4,uint64_t(cpw::kMaximumEntries) + 1}}, {{108,4,1}},
      {{112,8,cmw::kMaximumMetadataBytes + 1}},
      {{120,8,cpw::kMaximumAggregatePathBytes + 1}},
      {{128,8,std::numeric_limits<uint64_t>::max()}}, {{60,4,0}}
  }};
  for (const auto& field : fields) {
    bad = bytes; ContentWireInteger(bad, size_t(field[0]), size_t(field[1]), field[2]);
    ContentWireRehash(bad); PackageWireRefuses(bad);
  }
  const auto offsets = PackageEntryOffsets(record);
  const std::array<std::array<uint64_t, 3>, 9> entry_fields{{
      {{offsets[0],4,cpw::kMaximumPathBytes + 1}}, {{offsets[0] + 4,4,2}},
      {{offsets[2] + 8,8,record.before[1].file}}, {{offsets[2] + 8,8,0}},
      {{offsets[2] + 32,4,1000000000}}, {{offsets[2] + 36,4,0120600}},
      {{offsets[2] + 40,8,1}}, {{offsets[0] + 4,4,0}},
      {{offsets[2] + 8,8,record.op_directory}}
  }};
  for (const auto& field : entry_fields) {
    bad = bytes; ContentWireInteger(bad, size_t(field[0]), size_t(field[1]), field[2]);
    ContentWireRehash(bad); PackageWireRefuses(bad);
  }
  for (unsigned mutation = 0; mutation < 8; ++mutation) {
    bad = bytes;
    if (mutation == 0) { std::fill(bad.begin() + 24, bad.begin() + 40, 0); }
    if (mutation == 1) { bad[136] = 'A'; }
    if (mutation == 2) { bad[offsets[2] + 48] = '\\'; }
    if (mutation == 3) { bad[offsets[2] + 48] = 0; }
    if (mutation == 4) { bad[0] ^= 1; }
    if (mutation == 5) { bad[168] = 0; }
    const size_t target = 168 + record.identity.source.size() + record.package.native().size();
    if (mutation == 6) { bad[target] = 'F'; }
    if (mutation == 7) { bad[target + record.header_target.native().size() + 2] = 't'; }
    ContentWireRehash(bad); PackageWireRefuses(bad);
  }
}

TEST_CASE("Package mutation paths enforce bounded depth and exact relative bytes",
          "[managed_identity][content_package_mutation_wire]") {
  for (const auto& path : {std::string("."), std::string(".."), std::string("/root"),
       std::string("Data/../Slot"), std::string("Data//Slot"), std::string("Data/"),
       std::string("Data\\Slot"), std::string("bad\0name",8), std::string("bad\nname"),
       std::string("bad") + char(0x7f)}) {
    auto r = PackageWireRecord(); r.before[2].relative = std::filesystem::path(path);
    std::error_code error; REQUIRE(cpw::Encode(r, error).empty()); REQUIRE(error);
  }
  auto r = PackageWireRecord(); r.before.resize(1);
  std::string path;
  for (unsigned depth = 1; depth <= cpw::kMaximumDepth; ++depth) {
    path += path.empty() ? "x" : "/x";
    r.before.push_back(PackageEntry(path, 4000 + depth, true));
  }
  std::error_code error;
  auto bytes = cpw::Encode(r, error); REQUIRE_FALSE(error);
  cpw::Record decoded; REQUIRE(cpw::Decode(bytes, decoded, error)); REQUIRE(decoded == r);
  r.before.push_back(PackageEntry(path + "/too-deep", 5000, false));
  REQUIRE(cpw::Encode(r, error).empty()); REQUIRE(error);
  r.before.resize(1);
  r.before.push_back(PackageEntry(std::string(cpw::kMaximumPathBytes, 'n'), 5001, false));
  bytes = cpw::Encode(r, error); REQUIRE_FALSE(error);
  REQUIRE(cpw::Decode(bytes, decoded, error)); REQUIRE(decoded == r);
  r.before.back().relative = std::string(cpw::kMaximumPathBytes + 1, 'n');
  REQUIRE(cpw::Encode(r, error).empty()); REQUIRE(error);
}

TEST_CASE("Package retirement and completion bind the exact prepared stage and kind",
          "[managed_identity][content_package_mutation_wire]") {
  std::error_code error;
  auto record = PackageWireRecord();
  const auto prepared = cpw::Encode(record, error); REQUIRE_FALSE(error);
  const auto retire = cpw::Retire(prepared, error); REQUIRE_FALSE(error); REQUIRE(retire.size() == 88);
  const auto done = cpw::Done(prepared, retire, error); REQUIRE_FALSE(error); REQUIRE(done.size() == 128);
  REQUIRE(cpw::CheckRetire(retire, prepared, error)); REQUIRE_FALSE(error);
  REQUIRE(cpw::CheckDone(done, prepared, retire, error)); REQUIRE_FALSE(error);
  for (unsigned mutation = 0; mutation < 3; ++mutation) {
    auto foreign = record;
    if (mutation == 0) { foreign.header.back() ^= 1; }
    if (mutation == 1) { foreign.kind = cpw::Kind::kTruncate; }
    if (mutation == 2) { foreign.op_directory += 100; }
    const auto other = cpw::Encode(foreign, error); REQUIRE_FALSE(error);
    REQUIRE_FALSE(cpw::CheckRetire(retire, other, error)); REQUIRE(error);
    REQUIRE(cpw::Done(other, retire, error).empty()); REQUIRE(error);
    const auto other_retire = cpw::Retire(other, error); REQUIRE_FALSE(error);
    REQUIRE_FALSE(cpw::CheckDone(done, other, other_retire, error)); REQUIRE(error);
  }
  for (size_t length = 0; length < retire.size(); ++length) {
    REQUIRE_FALSE(cpw::CheckRetire(std::span(retire).first(length), prepared, error)); REQUIRE(error);
  }
  for (size_t length = 0; length < done.size(); ++length) {
    REQUIRE_FALSE(cpw::CheckDone(std::span(done).first(length), prepared, retire, error)); REQUIRE(error);
  }
  for (unsigned mutation = 0; mutation < 4; ++mutation) {
    auto bad_retire = retire;
    if (mutation == 0) { bad_retire[0] ^= 1; }
    if (mutation == 1) { ContentWireInteger(bad_retire, 12, 4, 2); }
    if (mutation == 2) { ContentWireInteger(bad_retire, 16, 8, prepared.size() + 1); }
    if (mutation == 3) { bad_retire[24] ^= 1; }
    ContentWireRehash(bad_retire);
    REQUIRE_FALSE(cpw::CheckRetire(bad_retire, prepared, error)); REQUIRE(error);
    REQUIRE(cpw::Done(prepared, bad_retire, error).empty()); REQUIRE(error);
    auto bad_done = done;
    if (mutation == 0) { bad_done[0] ^= 1; }
    if (mutation == 1) { ContentWireInteger(bad_done, 12, 4, 2); }
    if (mutation == 2) { ContentWireInteger(bad_done, 56, 8, retire.size() + 1); }
    if (mutation == 3) { bad_done[64] ^= 1; }
    ContentWireRehash(bad_done);
    REQUIRE_FALSE(cpw::CheckDone(bad_done, prepared, retire, error)); REQUIRE(error);
  }
  auto malformed = prepared; malformed.back() ^= 1;
  REQUIRE(cpw::Retire(malformed, error).empty()); REQUIRE(error);
  REQUIRE(cpw::Done(malformed, retire, error).empty()); REQUIRE(error);
  auto trailing = done; trailing.push_back(0);
  REQUIRE_FALSE(cpw::CheckDone(trailing, prepared, retire, error)); REQUIRE(error);
}
}  // namespace xe::filesystem::test
#endif
