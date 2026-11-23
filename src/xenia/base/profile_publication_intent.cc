/** Xenia : Xbox 360 Emulator Research Project; BSD license, see LICENSE. */
#include "xenia/base/profile_publication_intent.h"
#include <algorithm>
#include <cstdio>
#include <map>
#include <new>
#include <random>
#include <set>
#include <string_view>
#include "third_party/mbedtls/include/mbedtls/sha256.h"
namespace xe::filesystem {
namespace detail {
std::error_code AdmitProfileIntentCompletion(size_t occupied, size_t unfinished) {
  // Existing pending completions PLUS this new intent/completion pair. Check
  // subtraction bounds before additions; the immutable grammar cap stays8192.
  constexpr size_t kMaximum = 8192;
  if (occupied > kMaximum || unfinished > kMaximum - occupied ||
      kMaximum - occupied - unfinished < 2) {
    return std::make_error_code(std::errc::no_space_on_device);
  }
  return {};
}
}  // namespace detail
namespace {
constexpr uint64_t kMaximumJournal =
    4 * ProfileGpdSnapshotStore::kMaximumBytes + 16384;
constexpr std::string_view kMagic = "XPGDI03\n", kDoneMagic = "XPGDD03\n";
using Digest = std::array<uint8_t, 32>;
std::error_code Bad() { return std::make_error_code(std::errc::bad_message); }
std::error_code Conflict() {
  return std::make_error_code(std::errc::state_not_recoverable);
}
bool Equal(std::span<const uint8_t> a, std::span<const uint8_t> b) {
  return std::equal(a.begin(), a.end(), b.begin(), b.end());
}
Digest Hash(std::span<const uint8_t> bytes, std::error_code& error) {
  Digest result{};
  if (mbedtls_sha256(bytes.data(), bytes.size(), result.data(), 0)) {
    error = std::make_error_code(std::errc::io_error);
  }
  return result;
}
bool Relative(const std::filesystem::path& p) {
  if (p.empty() || p.is_absolute() || p.has_root_name()) {
    return false;
  }
  for (const auto& part : p) {
    if (part == "." || part == ".." || part.empty()) {
      return false;
    }
  }
  return p.generic_string().size() <= 4096 &&
         p.generic_string().find('\0') == std::string::npos;
}
bool BindingValid(const GpdIntentBinding& b) {
  return !b.source_id.empty() && b.source_id.size() <= 256 && b.device_id &&
         b.xuid && b.source_generation && b.managed_source.directory &&
         b.managed_source.content_epoch &&
         std::any_of(b.managed_source.uuid.begin(), b.managed_source.uuid.end(),
                     [](auto v) { return v != 0; }) &&
         Relative(b.profile_relative) &&
         b.source_id.find('\0') == std::string::npos;
}
bool BindingOwnedBy(const GpdIntentBinding& binding,
                    const ManagedDeviceIdentity& identity) {
  return binding.source_id == identity.source_id() &&
         binding.device_id == identity.device_id() &&
         binding.managed_source ==
             GpdManagedSourceIdentity{identity.uuid(), identity.content_epoch(),
                                      identity.directory_file_id()};
}
bool SameBinding(const GpdIntentBinding& a, const GpdIntentBinding& b) {
  return a.source_id == b.source_id && a.device_id == b.device_id &&
         a.xuid == b.xuid && a.profile_relative == b.profile_relative &&
         a.managed_source == b.managed_source;
}
std::string Hex(std::span<const uint8_t> bytes) {
  constexpr char digits[] = "0123456789abcdef";
  std::string result;
  for (uint8_t c : bytes) {
    result += digits[c >> 4];
    result += digits[c & 15];
  }
  return result;
}
std::string Leaf(uint32_t title) {
  char result[13];
  std::snprintf(result, sizeof(result), "%08X.gpd", title);
  return result;
}
bool SameLeaf(const std::filesystem::path& leaf, uint32_t title) {
  if (leaf.has_parent_path()) {
    return false;
  }
  auto s = leaf.string();
  for (char& c : s) {
    if (c >= 'a' && c <= 'z') {
      c -= 'a' - 'A';
    }
  }
  auto expected = Leaf(title);
  for (char& c : expected) {
    if (c >= 'a' && c <= 'z') {
      c -= 'a' - 'A';
    }
  }
  return s == expected;
}
struct Writer {
  std::vector<uint8_t> bytes;
  void U64(uint64_t n) {
    for (int i = 7; i >= 0; --i) {
      bytes.push_back(uint8_t(n >> (i * 8)));
    }
  }
  void Raw(std::span<const uint8_t> p) {
    bytes.insert(bytes.end(), p.begin(), p.end());
  }
  void Text(std::string_view p) {
    U64(p.size());
    Raw({reinterpret_cast<const uint8_t*>(p.data()), p.size()});
  }
  void Blob(std::span<const uint8_t> p, std::error_code& error) {
    U64(p.size());
    Raw(Hash(p, error));
    Raw(p);
  }
};
struct Reader {
  std::span<const uint8_t> bytes;
  std::error_code error;
  std::span<const uint8_t> Take(size_t n) {
    if (n > bytes.size()) {
      error = Bad();
      return {};
    }
    auto result = bytes.first(n);
    bytes = bytes.subspan(n);
    return result;
  }
  uint64_t U64() {
    auto b = Take(8);
    uint64_t n = 0;
    for (uint8_t c : b) {
      n = n << 8 | c;
    }
    return n;
  }
  std::string Text(size_t maximum) {
    uint64_t n = U64();
    if (n > maximum) {
      error = Bad();
      return {};
    }
    auto b = Take(size_t(n));
    return {b.begin(), b.end()};
  }
  std::vector<uint8_t> Blob() {
    uint64_t n = U64();
    if (n > ProfileGpdSnapshotStore::kMaximumBytes) {
      error = Bad();
      return {};
    }
    auto hash = Take(32), b = Take(size_t(n));
    if (!error && !Equal(hash, Hash(b, error))) {
      error = Bad();
    }
    return {b.begin(), b.end()};
  }
};
}  // namespace
struct ProfilePublicationIntentRecord {
  std::array<uint8_t, 16> id{};
  GpdIntentBinding binding;
  HostStorageRootIdentity root;
  struct File {
    GpdIntentAction action = GpdIntentAction::kReplace;
    uint32_t title = 0;
    std::filesystem::path leaf;
    FileHandleInfo before_info;
    std::vector<uint8_t> before, intended;
  };
  std::vector<File> files;
  Digest journal_hash{};
};
namespace {
constexpr std::string_view kArtifactMagic = "XPGDI04\n", kArtifactDoneMagic = "XPGDD04\n";
constexpr uint64_t kMaximumArtifactPayload = 256ull << 20;
std::string Fold(std::string value) {
  for (auto& c : value) { if (c >= 'A' && c <= 'Z') { c += 'a' - 'A'; } }
  return value;
}
std::filesystem::path CanonicalProfile(uint64_t xuid) {
  if (!xuid) { return {}; }
  char value[17]; std::snprintf(value, sizeof(value), "%016llX", static_cast<unsigned long long>(xuid));
  return std::filesystem::path(value) / "FFFE07D1" / "00010000" / value;
}
bool CanonicalBinding(const GpdIntentBinding& binding) {
  return BindingValid(binding) &&
      Fold(binding.profile_relative.generic_string()) == Fold(CanonicalProfile(binding.xuid).generic_string());
}
std::filesystem::path ArtifactLeaf(ProfileArtifactKind kind, uint32_t title) {
  if (kind == ProfileArtifactKind::kGpd) { return title ? std::filesystem::path{Leaf(title)} : std::filesystem::path{}; }
  if (title) { return {}; }
  switch (kind) {
    case ProfileArtifactKind::kPublic64: return "tile_64.png";
    case ProfileArtifactKind::kPublic32: return "tile_32.png";
    case ProfileArtifactKind::kPersonal64: return "pp_64.png";
    case ProfileArtifactKind::kPersonal32: return "pp_32.png";
    case ProfileArtifactKind::kAvatar64: return "avtr_64.png";
    case ProfileArtifactKind::kAvatar32: return "avtr_32.png";
    case ProfileArtifactKind::kThumbnail: return "__thumbnail.png";
    default: return {};
  }
}
bool ArtifactLeafMatches(ProfileArtifactKind kind, uint32_t title,
                         const std::filesystem::path& actual) {
  const auto expected = ArtifactLeaf(kind, title);
  return !expected.empty() && !actual.has_parent_path() &&
      Fold(actual.generic_string()) == Fold(expected.generic_string());
}
uint64_t ArtifactMaximum(ProfileArtifactKind kind) {
  return kind == ProfileArtifactKind::kThumbnail
      ? content_mutation_wire::kMaximumMetadataBytes : ProfileGpdSnapshotStore::kMaximumBytes;
}
std::error_code ArtifactBytes(ProfileArtifactKind kind, std::span<const uint8_t> bytes) {
  if (bytes.size() > ArtifactMaximum(kind)) { return std::make_error_code(std::errc::file_too_large); }
  return kind == ProfileArtifactKind::kGpd ? ValidateProfileGpdSnapshot(bytes) : std::error_code{};
}
bool HasMagic(std::span<const uint8_t> bytes, std::string_view magic) {
  return bytes.size() >= magic.size() && Equal(bytes.first(magic.size()),
      {reinterpret_cast<const uint8_t*>(magic.data()), magic.size()});
}
struct ArtifactRecord {
  std::array<uint8_t, 16> id{};
  GpdIntentBinding binding;
  HostStorageRootIdentity root;
  struct File {
    ProfileArtifactKind kind = ProfileArtifactKind::kGpd;
    ProfileArtifactAction action = ProfileArtifactAction::kObserve;
    uint32_t title = 0;
    std::filesystem::path leaf;
    bool before_exists = false;
    FileHandleInfo before_info{};
    std::vector<uint8_t> before, intended;
  };
  std::vector<File> files;
  Digest journal_hash{};
};
std::vector<uint8_t> EncodeArtifact(const ArtifactRecord& r, std::error_code& error) {
  Writer w; w.Raw({reinterpret_cast<const uint8_t*>(kArtifactMagic.data()), 8}); w.Raw(r.id);
  w.Text(r.binding.source_id); w.U64(r.binding.device_id); w.U64(r.binding.xuid);
  w.U64(r.binding.source_generation); w.Text(r.binding.profile_relative.generic_string());
  w.Raw(r.binding.managed_source.uuid); w.U64(r.binding.managed_source.content_epoch);
  w.U64(r.binding.managed_source.directory); w.U64(r.files.size());
  w.U64(r.root.filesystem); w.U64(r.root.directory); w.U64(r.root.mount);
  for (const auto& f : r.files) {
    w.U64(uint64_t(f.kind)); w.U64(uint64_t(f.action)); w.U64(f.title);
    w.Text(f.leaf.generic_string()); w.U64(f.before_exists ? 1 : 0);
    w.U64(f.before_info.filesystem_id); w.U64(f.before_info.file_id); w.U64(f.before_info.size);
    w.Blob(f.before, error); w.Blob(f.intended, error);
  }
  w.Raw(Hash(w.bytes, error)); return std::move(w.bytes);
}
std::vector<uint8_t> ReadArtifactBlob(Reader& in, uint64_t maximum) {
  const auto n = in.U64();
  if (n > maximum) { in.error = Bad(); return {}; }
  const auto hash = in.Take(32), bytes = in.Take(size_t(n));
  if (!in.error && !Equal(hash, Hash(bytes, in.error))) { in.error = Bad(); }
  return {bytes.begin(), bytes.end()};
}
std::unique_ptr<ArtifactRecord> DecodeArtifact(std::span<const uint8_t> bytes,
    const std::filesystem::path& journal, std::error_code& error) {
  if (bytes.size() < 56 || bytes.size() > kMaximumJournal || !HasMagic(bytes, kArtifactMagic)) {
    error = Bad(); return {};
  }
  const auto body = bytes.first(bytes.size() - 32);
  const auto hash = Hash(body, error);
  if (error || !Equal(hash, bytes.last(32))) { error = Bad(); return {}; }
  Reader in{body, {}}; in.Take(8); auto r = std::make_unique<ArtifactRecord>();
  const auto id = in.Take(16); std::copy(id.begin(), id.end(), r->id.begin());
  r->binding.source_id = in.Text(256); const auto device = in.U64();
  r->binding.device_id = uint32_t(device); r->binding.xuid = in.U64();
  r->binding.source_generation = in.U64(); r->binding.profile_relative = in.Text(4096);
  const auto uuid = in.Take(16);
  if (uuid.size() == 16) { std::copy(uuid.begin(), uuid.end(), r->binding.managed_source.uuid.begin()); }
  r->binding.managed_source.content_epoch = in.U64(); r->binding.managed_source.directory = in.U64();
  const auto count = in.U64();
  if (in.error || count < 1 || count > 16) { error = Bad(); return {}; }
  r->root = {in.U64(), in.U64(), in.U64()}; r->files.resize(size_t(count));
  std::set<std::string> leaves; uint64_t total = 0; size_t main_members = 0;
  for (auto& f : r->files) {
    const auto kind = in.U64(), action = in.U64(), title = in.U64();
    f.kind = ProfileArtifactKind(kind); f.action = ProfileArtifactAction(action);
    f.title = uint32_t(title); f.leaf = in.Text(32); const auto exists = in.U64();
    f.before_exists = exists == 1; f.before_info.filesystem_id = in.U64();
    f.before_info.file_id = in.U64(); f.before_info.size = in.U64();
    f.before_info.regular = f.before_exists; f.before_info.links = f.before_exists ? 1 : 0;
    f.before = ReadArtifactBlob(in, ArtifactMaximum(f.kind));
    f.intended = ReadArtifactBlob(in, ArtifactMaximum(f.kind));
    if (kind < 1 || kind > 8 || action < 1 || action > 4 || title > UINT32_MAX || exists > 1 ||
        !ArtifactLeafMatches(f.kind, f.title, f.leaf) || !leaves.insert(Fold(f.leaf.generic_string())).second ||
        f.before_info.size != f.before.size() ||
        (f.before_exists ? !f.before_info.file_id :
          (!f.before.empty() || f.before_info.file_id || f.before_info.filesystem_id || f.before_info.size))) {
      in.error = Bad();
    }
    if (f.kind != ProfileArtifactKind::kThumbnail) { ++main_members; }
    if (f.before_exists && ArtifactBytes(f.kind, f.before)) { in.error = Bad(); }
    if (f.action == ProfileArtifactAction::kObserve) {
      if (!f.intended.empty()) { in.error = Bad(); }
    } else if (f.action == ProfileArtifactAction::kDelete) {
      if (!f.before_exists || !f.intended.empty()) { in.error = Bad(); }
    } else {
      if ((f.action == ProfileArtifactAction::kCreate) == f.before_exists ||
          ArtifactBytes(f.kind, f.intended)) { in.error = Bad(); }
    }
    if (f.before.size() > kMaximumArtifactPayload - total) { in.error = Bad(); }
    else { total += f.before.size(); }
    if (f.intended.size() > kMaximumArtifactPayload - total) { in.error = Bad(); }
    else { total += f.intended.size(); }
  }
  if (in.error || !in.bytes.empty() || !main_members || device > UINT32_MAX ||
      !CanonicalBinding(r->binding) || !r->root.directory || !r->root.mount ||
      journal != "intent-" + Hex(r->id) + ".bin") { error = Bad(); return {}; }
  r->journal_hash = Hash(bytes, error); return error ? nullptr : std::move(r);
}
std::vector<uint8_t> ArtifactDone(const ArtifactRecord& r) {
  Writer w; w.Raw({reinterpret_cast<const uint8_t*>(kArtifactDoneMagic.data()), 8});
  w.Raw(r.id); w.Raw(r.journal_hash); return std::move(w.bytes);
}
std::filesystem::path ArtifactDoneLeaf(const ArtifactRecord& r) {
  return "done-" + Hex(r.id) + ".bin";
}
std::vector<uint8_t> Encode(const ProfilePublicationIntentRecord& r,
                            std::error_code& error) {
  Writer w;
  w.Raw({reinterpret_cast<const uint8_t*>(kMagic.data()), kMagic.size()});
  w.Raw(r.id);
  w.Text(r.binding.source_id);
  w.U64(r.binding.device_id);
  w.U64(r.binding.xuid);
  w.U64(r.binding.source_generation);
  w.Text(r.binding.profile_relative.generic_string());
  w.Raw(r.binding.managed_source.uuid);
  w.U64(r.binding.managed_source.content_epoch);
  w.U64(r.binding.managed_source.directory);
  w.U64(r.files.size());
  w.U64(r.root.filesystem);
  w.U64(r.root.directory);
  w.U64(r.root.mount);
  for (const auto& f : r.files) {
    w.U64(uint64_t(f.action));
    w.U64(f.title);
    w.Text(f.leaf.generic_string());
    w.U64(f.before_info.filesystem_id);
    w.U64(f.before_info.file_id);
    w.U64(f.before_info.size);
    w.Blob(f.before, error);
    w.Blob(f.intended, error);
  }
  w.Raw(Hash(w.bytes, error));
  return std::move(w.bytes);
}
std::unique_ptr<ProfilePublicationIntentRecord> Decode(
    std::span<const uint8_t> bytes, const std::filesystem::path& journal,
    std::error_code& error) {
  if (bytes.size() < 56 || bytes.size() > kMaximumJournal) {
    error = Bad();
    return {};
  }
  const auto body = bytes.first(bytes.size() - 32);
  auto digest = Hash(body, error);
  if (error || !Equal(digest, bytes.last(32))) {
    error = Bad();
    return {};
  }
  Reader in{body, {}};
  if (!Equal(in.Take(8),
             {reinterpret_cast<const uint8_t*>(kMagic.data()), 8})) {
    error = Bad();
    return {};
  }
  auto r = std::make_unique<ProfilePublicationIntentRecord>();
  auto id = in.Take(16);
  std::copy(id.begin(), id.end(), r->id.begin());
  r->binding.source_id = in.Text(256);
  const uint64_t device = in.U64();
  r->binding.device_id = uint32_t(device);
  r->binding.xuid = in.U64();
  r->binding.source_generation = in.U64();
  r->binding.profile_relative = in.Text(4096);
  {
    auto uuid = in.Take(16);
    if (uuid.size() == 16) {
      std::copy(uuid.begin(), uuid.end(),
                r->binding.managed_source.uuid.begin());
    }
    r->binding.managed_source.content_epoch = in.U64();
    r->binding.managed_source.directory = in.U64();
  }
  const uint64_t count = in.U64();
  if (in.error || count < 1 || count > 16) {
    error = Bad();
    return {};
  }
  r->files.resize(size_t(count));
  r->root = {in.U64(), in.U64(), in.U64()};
  std::set<uint32_t> titles;
  for (auto& f : r->files) {
    const uint64_t action = in.U64();
    f.action = GpdIntentAction(action);
    const uint64_t title = in.U64();
    f.title = uint32_t(title);
    f.leaf = in.Text(12);
    f.before_info.filesystem_id = in.U64();
    f.before_info.file_id = in.U64();
    f.before_info.size = in.U64();
    f.before_info.regular = true;
    f.before_info.links = 1;
    f.before = in.Blob();
    f.intended = in.Blob();
    if (title > UINT32_MAX || !f.title || !SameLeaf(f.leaf, f.title) ||
        !titles.insert(f.title).second || action < 1 || action > 3 ||
        f.before_info.size != f.before.size()) {
      in.error = Bad();
    }
    if (f.action == GpdIntentAction::kCreate) {
      if (!f.before.empty() || f.before_info.file_id ||
          f.before_info.filesystem_id || ValidateProfileGpdSnapshot(f.intended)) {
        in.error = Bad();
      }
    } else {
      if (!f.before_info.file_id || ValidateProfileGpdSnapshot(f.before) ||
          (f.action == GpdIntentAction::kDelete
               ? !f.intended.empty()
               : bool(ValidateProfileGpdSnapshot(f.intended)))) {
        in.error = Bad();
      }
    }
  }
  if (in.error || !in.bytes.empty() || device > UINT32_MAX ||
      !BindingValid(r->binding) || !r->root.directory || !r->root.mount ||
      journal != "intent-" + Hex(r->id) + ".bin") {
    error = Bad();
    return {};
  }
  r->journal_hash = Hash(bytes, error);
  return error ? nullptr : std::move(r);
}
std::vector<uint8_t> DoneBytes(const ProfilePublicationIntentRecord& r) {
  Writer w;
  w.Raw(
      {reinterpret_cast<const uint8_t*>(kDoneMagic.data()), kDoneMagic.size()});
  w.Raw(r.id);
  w.Raw(r.journal_hash);
  return std::move(w.bytes);
}
std::filesystem::path DoneLeaf(const ProfilePublicationIntentRecord& r) {
  return "done-" + Hex(r.id) + ".bin";
}
std::shared_ptr<const HostStorageSnapshotToken> Capture(
    const ProfileIntentOwner& owner,
    const std::filesystem::path& leaf, uint64_t maximum,
    std::error_code& error) {
  auto token = owner.root()->CaptureSnapshot(leaf, maximum, error);
  if (!error && !token->exists()) {
    error = Bad();
  }
  return token;
}
}  // namespace
struct ProfileArtifactAdmission::Data {
  std::shared_ptr<const ProfileIntentOwner> owner;
  std::shared_ptr<const SnapshotRecoveryOwner> recovery;
  std::shared_ptr<ContentMutationOwner> metadata_owner;
  GpdIntentBinding binding;
  // The actual independent target flock outlives every member reservation.
  std::shared_ptr<const HostStorageIntentLease> target;
  std::shared_ptr<SnapshotPublicationSet> members;
  std::shared_ptr<ContentMutationOwner::ProfileThumbnailParticipant> thumbnail;
  std::vector<ProfileArtifactExpected> expected;
};
ProfileArtifactAdmission::ProfileArtifactAdmission(std::shared_ptr<Data> data) : data_(std::move(data)) {}
ProfileArtifactAdmission::~ProfileArtifactAdmission() = default;
const std::vector<ProfileArtifactExpected>& ProfileArtifactAdmission::expected() const { return data_->expected; }
const std::shared_ptr<SnapshotPublicationSet>& ProfileArtifactAdmission::members() const { return data_->members; }
struct ProfilePublicationIntentStore::ArtifactState {
  std::unique_ptr<ArtifactRecord> record;
  std::shared_ptr<ProfileArtifactAdmission> admission;
};
std::filesystem::path ProfilePublicationIntentStore::ProfileRelative(uint64_t xuid) {
  return CanonicalProfile(xuid);
}
ProfilePublicationIntentStore::~ProfilePublicationIntentStore() = default;
std::vector<GpdIntentSummary> ProfilePublicationIntentStore::Scan(
    const ProfileIntentOwner& owner, std::error_code& error) {
  error = owner.error();
  if (error) {
    return {};
  }
  if (!owner.managed_identity()) {
    error = Bad();
    return {};
  }
  error = owner.managed_identity()->CheckCurrent();
  if (error) {
    return {};
  }
  std::unique_lock lock(owner.mutex_, std::try_to_lock);
  if (!lock.owns_lock()) {
    error = std::make_error_code(std::errc::device_or_resource_busy);
    return {};
  }
  return ScanLocked(owner, error);
}
std::vector<GpdIntentSummary> ProfilePublicationIntentStore::ScanLocked(
    const ProfileIntentOwner& owner, std::error_code& error) {
  auto leaves = owner.List(error);
  if (error) {
    return {};
  }
  std::vector<GpdIntentSummary> result;
  std::set<std::filesystem::path> done_seen;
  for (const auto& leaf : leaves) {
    if (!leaf.string().starts_with("intent-")) {
      continue;
    }
    GpdIntentSummary row;
    row.journal = leaf;
    const auto token = Capture(owner, leaf, kMaximumJournal, row.error);
    bool decoded = false; std::filesystem::path done_leaf; std::vector<uint8_t> expected_done;
    if (token && !row.error && HasMagic(token->bytes(), kArtifactMagic)) {
      auto record = DecodeArtifact(token->bytes(), leaf, row.error);
      if (record) {
        decoded = true; row.format_version = 4; row.binding = record->binding;
        done_leaf = ArtifactDoneLeaf(*record); expected_done = ArtifactDone(*record);
      }
    } else {
      auto record = token && !row.error ? Decode(token->bytes(), leaf, row.error) : nullptr;
      if (record) {
        decoded = true; row.binding = record->binding;
        done_leaf = DoneLeaf(*record); expected_done = DoneBytes(*record);
      }
    }
    if (decoded) {
      if (!BindingOwnedBy(row.binding, *owner.managed_identity())) { row.error = Conflict(); }
      std::error_code done_error;
      auto done = owner.root()->CaptureSnapshot(done_leaf, 56, done_error);
      if (done_error) { row.error = done_error; }
      else if (done->exists()) {
        done_seen.insert(done_leaf);
        if (!Equal(done->bytes(), expected_done)) { row.error = Bad(); }
        else { row.done_record_valid = true; }
      }
    }
    if (row.error && !error) {
      error = row.error;
    }
    result.push_back(std::move(row));
  }
  // An orphan done receipt cannot authorize a missing/invalid intent.
  for (const auto& leaf : leaves) {
    if (leaf.string().starts_with("done-") && !done_seen.contains(leaf)) {
      result.push_back({Bad(), leaf, {}, false});
      if (!error) {
        error = Bad();
      }
    }
  }
  return result;
}
std::unique_ptr<ProfilePublicationIntentStore> ProfilePublicationIntentStore::Prepare(
    std::shared_ptr<const ProfileIntentOwner> owner,
    std::shared_ptr<const SnapshotRecoveryOwner> recovery,
    GpdIntentBinding binding, std::vector<GpdIntentMutation> mutations,
    std::error_code &error, std::filesystem::path &attempted_journal,
    std::span<const std::shared_ptr<const HostStorageSnapshotToken>>
        model_dependencies) {
  error.clear();
  attempted_journal.clear();
  if (!owner || !recovery) {
    error = std::make_error_code(std::errc::invalid_argument);
    return {};
  }
  if (owner->error() || recovery->error()) {
    error = owner->error() ? owner->error() : recovery->error();
    return {};
  }
  if (!owner->managed_identity()) {
    error = Bad();
    return {};
  }
  if (auto identity_error = owner->managed_identity()->CheckCurrent()) {
    error = identity_error;
    return {};
  }
  if (!BindingValid(binding) || mutations.empty() || mutations.size() > 16 ||
      model_dependencies.size() >
          HostStorageRoot::kMaximumSnapshotBatchTokens) {
    error = std::make_error_code(std::errc::invalid_argument);
    return {};
  }
  if (!BindingOwnedBy(binding, *owner->managed_identity())) {
    error = Conflict();
    return {};
  }
  const auto first = mutations.front().before.token;
  if (!first) {
    error = std::make_error_code(std::errc::invalid_argument);
    return {};
  }
  const auto view = first->root();
  error = recovery->CheckTarget(*view);
  if (error) { return {}; }
  if (view->path().lexically_relative(owner->source()->path()) !=
          binding.profile_relative ||
      view->read_only() || view->is_guest_private()) {
    error = std::make_error_code(std::errc::permission_denied);
    return {};
  }
  std::unique_lock lock(owner->mutex_, std::try_to_lock);
  if (!lock.owns_lock()) {
    error = std::make_error_code(std::errc::device_or_resource_busy);
    return {};
  }
  auto target_lease = owner->AcquireTarget(*view, error);
  if (!target_lease) {
    return {};
  }
  auto rows = ScanLocked(*owner, error);
  if (error) {
    return {};
  }
  for (const auto& row : rows) {
    if (!row.done_record_valid &&
        (SameBinding(row.binding, binding) ||
         row.binding.profile_relative == binding.profile_relative)) {
      error = std::make_error_code(std::errc::device_or_resource_busy);
      return {};
    }
  }
  // C includes the actual owner/intent/done leaves; U includes every valid
  // unfinished journal, including OTHER profiles. Their completion slots
  // remain reserved across owner reconstruction because journals persist.
  const auto occupied = owner->List(error);
  if (error) {
    return {};
  }
  const size_t unfinished =
      size_t(std::count_if(rows.begin(), rows.end(), [](const auto& row) {
        return !row.done_record_valid;
      }));
  error = detail::AdmitProfileIntentCompletion(occupied.size(), unfinished);
  if (error) {
    return {};
  }
  auto result =
      std::unique_ptr<ProfilePublicationIntentStore>(new ProfilePublicationIntentStore);
  result->owner_ = owner;
  result->recovery_ = recovery;
  result->view_ = view;
  result->target_lease_ = std::move(target_lease);
  result->record_ = std::make_unique<ProfilePublicationIntentRecord>();
  auto& r = *result->record_;
  r.binding = std::move(binding);
  r.root = owner->Identity(*view, error);
  if (error) {
    return {};
  }
  r.files.resize(mutations.size());
  result->published_.resize(mutations.size());
  uint64_t total_bytes = 0;
  std::set<uint32_t> titles;
  for (size_t i = 0; i != mutations.size(); ++i) {
    auto& m = mutations[i];
    const auto& l = m.before;
    auto& f = r.files[i];
    if (l.error) {
      error = l.error;
      return {};
    }
    if (m.action == GpdIntentAction::kDelete) {
      if (!m.intended.empty()) {
        error = Bad();
        return {};
      }
    } else {
      error = ValidateProfileGpdSnapshot(m.intended);
      if (error) {
        return {};
      }
    }
    if (!l.token || l.token->root() != view ||
        l.identity.xuid != r.binding.xuid ||
        l.identity.source_generation != r.binding.source_generation ||
        !l.identity.title_id || !titles.insert(l.identity.title_id).second ||
        !l.token->ascii_folded_leaf() ||
        !SameLeaf(l.token->relative(), l.identity.title_id) ||
        (m.action == GpdIntentAction::kCreate
             ? !l.missing()
             : (m.action != GpdIntentAction::kReplace &&
                m.action != GpdIntentAction::kDelete) ||
                   !l.readable() ||
                   bool(ValidateProfileGpdSnapshot(l.token->bytes())))) {
      error = Bad();
      return {};
    }
    total_bytes += l.token->bytes().size() + m.intended.size();
    if (total_bytes > (256ull << 20)) {
      error = std::make_error_code(std::errc::file_too_large);
      return {};
    }
    f.action = m.action;
    f.title = l.identity.title_id;
    f.leaf = l.token->relative();
    f.before_info = l.token->info();
    f.before.assign(l.token->bytes().begin(), l.token->bytes().end());
    f.intended = std::move(m.intended);
  }
  std::random_device random;
  for (auto& byte : r.id) {
    byte = uint8_t(random());
  }
  result->journal_ = "intent-" + Hex(r.id) + ".bin";
  auto wire = Encode(r, error);
  if (error) {
    return {};
  }
  r.journal_hash = Hash(wire, error);
  if (error) {
    return {};
  }
  // Exclusive new immutable file; complete durable intent precedes ALL target
  // calls. A partial/uncertain attempt is named and preserved, never adopted.
  std::vector<std::shared_ptr<const HostStorageSnapshotToken>>
      checked_tokens(model_dependencies.begin(), model_dependencies.end());
  for (const auto &mutation : mutations) {
    if (std::find(checked_tokens.begin(), checked_tokens.end(),
                  mutation.before.token) == checked_tokens.end()) {
      checked_tokens.push_back(mutation.before.token);
    }
  }
  // Native/VFS writers use this same filesystem admission. Validation remains
  // owned through the durable intent write, closing the check-to-journal race.
  // No model/GCRC lock or provider callback is held across these native calls.
  {
    auto batch = view->ValidateSnapshotBatch(std::move(checked_tokens), error);
    if (!batch) {
      return {};
    } // No persistent intent on stale baselines.
    attempted_journal = result->journal_;
    const auto write = owner->root()->WriteNewIntentWithSnapshotGuard(
        *batch, result->journal_, wire);
    error = write.error;
    if (!write.created) {
      attempted_journal.clear();
    }
    if (!error && (!write.durable || write.bytes != wire.size())) {
      error = std::make_error_code(std::errc::io_error);
    }
    if (error) {
      return {};
    }
  } // Drop native admission before CaptureSnapshot reacquires it.
  result->journal_token_ =
      Capture(*owner, result->journal_, kMaximumJournal, error);
  if (!error && !Equal(result->journal_token_->bytes(), wire)) {
    error = Conflict();
  }
  if (error) {
    return {};
  }
  return result;
}
std::shared_ptr<ProfileArtifactAdmission> ProfilePublicationIntentStore::AcquireArtifacts(
    std::shared_ptr<const ProfileIntentOwner> owner,
    std::shared_ptr<const SnapshotRecoveryOwner> recovery,
    std::shared_ptr<ContentMutationOwner> metadata, GpdIntentBinding binding,
    std::vector<ProfileArtifactExpected> main, bool thumbnail, std::error_code& error) {
  return AcquireArtifactsInternal(std::move(owner), std::move(recovery), std::move(metadata),
      std::move(binding), std::move(main), thumbnail, error);
}
std::shared_ptr<ProfileArtifactAdmission> ProfilePublicationIntentStore::AcquireArtifactsInternal(
    std::shared_ptr<const ProfileIntentOwner> owner,
    std::shared_ptr<const SnapshotRecoveryOwner> recovery,
    std::shared_ptr<ContentMutationOwner> metadata, GpdIntentBinding binding,
    std::vector<ProfileArtifactExpected> main, bool thumbnail, std::error_code& error,
    const std::filesystem::path& recovering) {
  error.clear();
  if (!owner || !recovery || owner->error() || recovery->error()) {
    error = owner && owner->error() ? owner->error() : recovery && recovery->error()
        ? recovery->error() : std::make_error_code(std::errc::invalid_argument); return {};
  }
  if (!CanonicalBinding(binding) || !owner->managed_identity() ||
      !BindingOwnedBy(binding, *owner->managed_identity()) || main.empty() ||
      main.size() > (thumbnail ? 15u : 16u)) { error = Bad(); return {}; }
  error = owner->managed_identity()->CheckCurrent(); if (error) { return {}; }
  if (thumbnail && (!metadata || metadata->identity_ != owner->managed_identity() ||
      metadata->content_ != owner->source() || metadata->source_lease_ != owner->source_lease_)) {
    error = std::make_error_code(std::errc::permission_denied); return {};
  }
  const auto view = main.front().token ? main.front().token->root() : nullptr;
  if (!view || view->read_only() || view->is_guest_private() ||
      view->path().lexically_relative(owner->source()->path()) != binding.profile_relative ||
      !SnapshotPublicationSet::RootDescendsFrom(view, owner->source())) {
    error = std::make_error_code(std::errc::permission_denied); return {};
  }
  error = recovery->CheckTarget(*view); if (error) { return {}; }
  std::vector<std::shared_ptr<const HostStorageSnapshotToken>> tokens;
  std::set<std::string> leaves; uint64_t bytes = 0;
  for (const auto& expected : main) {
    const auto& token = expected.token;
    if (expected.kind == ProfileArtifactKind::kThumbnail || !token || token->root() != view ||
        !token->ascii_folded_leaf() || !ArtifactLeafMatches(expected.kind, expected.title_id, token->relative()) ||
        !leaves.insert(Fold(token->relative().generic_string())).second ||
        (token->exists() && ArtifactBytes(expected.kind, token->bytes()))) { error = Bad(); return {}; }
    if (token->bytes().size() > kMaximumArtifactPayload - bytes) {
      error = std::make_error_code(std::errc::file_too_large); return {};
    }
    bytes += token->bytes().size(); tokens.push_back(token);
  }
  std::unique_lock lock(owner->mutex_, std::try_to_lock);
  if (!lock.owns_lock()) { error = std::make_error_code(std::errc::device_or_resource_busy); return {}; }
  auto data = std::make_shared<ProfileArtifactAdmission::Data>();
  data->owner = std::move(owner); data->recovery = std::move(recovery);
  data->metadata_owner = std::move(metadata); data->binding = std::move(binding);
  data->target = data->owner->AcquireTarget(*view, error); if (!data->target) { return {}; }
  const auto rows = ScanLocked(*data->owner, error); if (error) { return {}; }
  for (const auto& row : rows) {
    if (!row.done_record_valid && row.journal != recovering &&
        (SameBinding(row.binding, data->binding) ||
         row.binding.profile_relative == data->binding.profile_relative)) {
      error = std::make_error_code(std::errc::device_or_resource_busy); return {};
    }
  }
  data->members = SnapshotPublicationSet::Reserve(view, std::move(tokens), error);
  if (!data->members) { return {}; }
  data->expected = std::move(main);
  if (thumbnail) {
    data->thumbnail = data->metadata_owner->PrepareProfileThumbnail(data->binding.profile_relative,
                                                                   data->members, error);
    if (!data->thumbnail) { return {}; }
    data->expected.push_back({ProfileArtifactKind::kThumbnail, 0, data->thumbnail->expected()});
  }
  return std::shared_ptr<ProfileArtifactAdmission>(new ProfileArtifactAdmission(std::move(data)));
}
std::unique_ptr<ProfilePublicationIntentStore> ProfilePublicationIntentStore::PrepareArtifacts(
    std::shared_ptr<ProfileArtifactAdmission> admission,
    std::vector<ProfileArtifactMutation> mutations, std::error_code& error,
    std::filesystem::path& attempted,
    std::span<const std::shared_ptr<const HostStorageSnapshotToken>> dependencies) {
  error.clear(); attempted.clear();
  if (!admission || !admission->data_ || mutations.empty() || mutations.size() > 16 ||
      dependencies.size() > HostStorageRoot::kMaximumSnapshotBatchTokens) { error = Bad(); return {}; }
  auto& data = *admission->data_;
  if (!data.owner || !data.members || !data.target || mutations.size() != data.expected.size()) {
    error = Bad(); return {};
  }
  error = data.owner->managed_identity()->CheckCurrent();
  if (!error && data.thumbnail) { error = data.thumbnail->CheckCurrent(*data.members); }
  if (error) { return {}; }
  std::unique_lock lock(data.owner->mutex_, std::try_to_lock);
  if (!lock.owns_lock()) { error = std::make_error_code(std::errc::device_or_resource_busy); return {}; }
  const auto rows = ScanLocked(*data.owner, error); if (error) { return {}; }
  for (const auto& row : rows) {
    if (!row.done_record_valid && row.binding.profile_relative == data.binding.profile_relative) {
      error = std::make_error_code(std::errc::device_or_resource_busy); return {};
    }
  }
  const auto occupied = data.owner->List(error); if (error) { return {}; }
  const auto unfinished = size_t(std::count_if(rows.begin(), rows.end(),
      [](const auto& row) { return !row.done_record_valid; }));
  error = detail::AdmitProfileIntentCompletion(occupied.size(), unfinished); if (error) { return {}; }
  auto result = std::unique_ptr<ProfilePublicationIntentStore>(new ProfilePublicationIntentStore);
  result->owner_ = data.owner; result->recovery_ = data.recovery; result->view_ = data.members->base();
  result->target_lease_ = data.target; result->artifacts_ = std::make_unique<ArtifactState>();
  result->artifacts_->admission = std::move(admission);
  result->artifacts_->record = std::make_unique<ArtifactRecord>(); auto& record = *result->artifacts_->record;
  record.binding = data.binding; record.root = data.owner->Identity(*result->view_, error);
  if (error) { return {}; }
  std::set<std::pair<uint64_t, uint32_t>> seen; uint64_t total = 0;
  for (auto& mutation : mutations) {
    const auto key = std::pair{uint64_t(mutation.kind), mutation.title_id};
    const auto before = std::find_if(data.expected.begin(), data.expected.end(), [&](const auto& item) {
      return item.kind == mutation.kind && item.title_id == mutation.title_id;
    });
    if (before == data.expected.end() || !seen.insert(key).second) { error = Bad(); return {}; }
    ArtifactRecord::File file; file.kind = mutation.kind; file.title = mutation.title_id;
    file.action = mutation.action; file.leaf = before->token->relative();
    file.before_exists = before->token->exists();
    if (file.before_exists) { file.before_info = before->token->info(); }
    file.before.assign(before->token->bytes().begin(), before->token->bytes().end());
    file.intended = std::move(mutation.intended);
    const auto action = uint64_t(file.action);
    if (action < 1 || action > 4 || !ArtifactLeafMatches(file.kind, file.title, file.leaf) ||
        (file.before_exists && ArtifactBytes(file.kind, file.before))) { error = Bad(); return {}; }
    if (file.action == ProfileArtifactAction::kObserve || file.action == ProfileArtifactAction::kDelete) {
      if (!file.intended.empty() || (file.action == ProfileArtifactAction::kDelete && !file.before_exists)) {
        error = Bad(); return {};
      }
    } else if ((file.action == ProfileArtifactAction::kCreate) == file.before_exists ||
               ArtifactBytes(file.kind, file.intended)) { error = Bad(); return {}; }
    if (file.before.size() > kMaximumArtifactPayload - total) { error = Bad(); return {}; }
    total += file.before.size();
    if (file.intended.size() > kMaximumArtifactPayload - total) { error = Bad(); return {}; }
    total += file.intended.size(); record.files.push_back(std::move(file));
  }
  std::random_device random; for (auto& byte : record.id) { byte = uint8_t(random()); }
  result->journal_ = "intent-" + Hex(record.id) + ".bin";
  auto encoded = EncodeArtifact(record, error); if (error) { return {}; }
  // Reuse the EXACT decoder before any journal mutation. It closes every new
  // typed action/name/absence rule;03 remains a separate unchanged grammar.
  auto decoded = DecodeArtifact(encoded, result->journal_, error); if (!decoded) { return {}; }
  record.journal_hash = decoded->journal_hash;
  result->published_.resize(record.files.size());
  std::vector<std::shared_ptr<const HostStorageSnapshotToken>> checked(dependencies.begin(), dependencies.end());
  for (const auto& item : data.expected) {
    const auto previous = std::find_if(checked.begin(), checked.end(), [&](const auto& token) {
      return token && token->root() == item.token->root() &&
          Fold(token->relative().generic_string()) == Fold(item.token->relative().generic_string());
    });
    if (previous == checked.end()) { checked.push_back(item.token); continue; }
    // CM issues a new original-view thumbnail projection, while the model's
    // original dependency may already own the same exact observation. Do not
    // submit duplicate leaves to the strict common batch or silently replace
    // a conflicting dependency with the newer admission observation.
    const auto& left = **previous; const auto& right = *item.token;
    if (left.relative() != right.relative() || left.ascii_folded_leaf() != right.ascii_folded_leaf() ||
        left.exists() != right.exists() || !Equal(left.bytes(), right.bytes()) ||
        (left.exists() && (left.info().filesystem_id != right.info().filesystem_id ||
                          left.info().file_id != right.info().file_id ||
                          left.info().size != right.info().size || left.info().links != right.info().links))) {
      error = Conflict(); return {};
    }
  }
  {
    auto batch = data.members->Validate(std::move(checked), error); if (!batch) { return {}; }
    attempted = result->journal_;
    const auto write = data.owner->root()->WriteNewIntentWithSnapshotGuard(*batch, result->journal_, encoded);
    error = write.error; if (!write.created) { attempted.clear(); }
    if (!error && (!write.durable || write.bytes != encoded.size())) { error = std::make_error_code(std::errc::io_error); }
    if (error) { return {}; }
  }
  result->journal_token_ = Capture(*data.owner, result->journal_, kMaximumJournal, error);
  if (!error && !Equal(result->journal_token_->bytes(), encoded)) { error = Conflict(); }
  return error ? nullptr : std::move(result);
}
std::unique_ptr<ProfilePublicationIntentStore> ProfilePublicationIntentStore::Recover(
    std::shared_ptr<const ProfileIntentOwner> owner,
    std::shared_ptr<const SnapshotRecoveryOwner> recovery,
    const std::filesystem::path& journal, GpdIntentBinding binding,
    std::shared_ptr<const HostStorageRoot> view,
    std::error_code& error, std::shared_ptr<ContentMutationOwner> metadata_owner) {
  error.clear();
  if (owner && !owner->error()) {
    const auto probe = Capture(*owner, journal, kMaximumJournal, error);
    if (!probe) { return {}; }
    if (HasMagic(probe->bytes(), kArtifactMagic)) {
      return RecoverArtifacts(std::move(owner), std::move(recovery), std::move(metadata_owner),
                              journal, std::move(binding), std::move(view), error);
    }
  }
  if (!owner || !recovery || !view) {
    error = std::make_error_code(std::errc::invalid_argument);
    return {};
  }
  if (owner->error() || recovery->error()) {
    error = owner->error() ? owner->error() : recovery->error();
    return {};
  }
  if (!owner->managed_identity()) {
    error = Bad();
    return {};
  }
  if (auto identity_error = owner->managed_identity()->CheckCurrent()) {
    error = identity_error;
    return {};
  }
  if (!BindingValid(binding) || view->read_only() || view->is_guest_private()) {
    error = std::make_error_code(std::errc::invalid_argument);
    return {};
  }
  if (!BindingOwnedBy(binding, *owner->managed_identity())) {
    error = Conflict();
    return {};
  }
  error = recovery->CheckTarget(*view);
  if (error) { return {}; }
  std::unique_lock lock(owner->mutex_, std::try_to_lock);
  if (!lock.owns_lock()) {
    error = std::make_error_code(std::errc::device_or_resource_busy);
    return {};
  }
  auto result =
      std::unique_ptr<ProfilePublicationIntentStore>(new ProfilePublicationIntentStore);
  result->owner_ = owner;
  result->recovery_ = recovery;
  result->view_ = view;
  result->journal_ = journal;
  result->target_lease_ = owner->AcquireTarget(*view, error);
  if (!result->target_lease_) {
    return {};
  }
  result->journal_token_ = Capture(*owner, journal, kMaximumJournal, error);
  if (error) {
    return {};
  }
  result->record_ = Decode(result->journal_token_->bytes(), journal, error);
  if (error) {
    return {};
  }
  // Every unknown or malformed record must be surfaced before profile load;
  // opening a selected valid record cannot bypass another unresolved journal.
  const auto rows = ScanLocked(*owner, error);
  if (error) {
    return {};
  }
  for (const auto& row : rows) {
    if (row.journal != journal && !row.done_record_valid &&
        row.binding.profile_relative == binding.profile_relative) {
      error = std::make_error_code(std::errc::device_or_resource_busy);
      return {};
    }
  }
  if (!SameBinding(result->record_->binding, binding) ||
      view->path().lexically_relative(owner->source()->path()) !=
          binding.profile_relative ||
      owner->Identity(*view, error).directory !=
          result->record_->root.directory) {
    if (!error) {
      error = Conflict();
    }
    return {};
  }
  result->record_->root = owner->Identity(*view, error);
  if (error) {
    return {};
  }
  result->recovered_ = true;
  result->published_.resize(result->record_->files.size());
  return result;
}
std::unique_ptr<ProfilePublicationIntentStore> ProfilePublicationIntentStore::RecoverArtifacts(
    std::shared_ptr<const ProfileIntentOwner> owner,
    std::shared_ptr<const SnapshotRecoveryOwner> recovery, std::shared_ptr<ContentMutationOwner> metadata,
    const std::filesystem::path& journal, GpdIntentBinding binding,
    std::shared_ptr<const HostStorageRoot> view, std::error_code& error) {
  error.clear();
  if (!owner || !recovery || !view || owner->error() || recovery->error() ||
      !owner->managed_identity() || !CanonicalBinding(binding) ||
      !BindingOwnedBy(binding, *owner->managed_identity()) ||
      !SnapshotPublicationSet::RootDescendsFrom(view, owner->source()) ||
      view->path().lexically_relative(owner->source()->path()) != binding.profile_relative) {
    error = std::make_error_code(std::errc::permission_denied); return {};
  }
  error = owner->managed_identity()->CheckCurrent(); if (error) { return {}; }
  error = recovery->CheckTarget(*view); if (error) { return {}; }
  auto token = Capture(*owner, journal, kMaximumJournal, error); if (!token) { return {}; }
  auto record = DecodeArtifact(token->bytes(), journal, error); if (!record) { return {}; }
  const auto native = owner->Identity(*view, error);
  if (error || !SameBinding(record->binding, binding) || native.directory != record->root.directory) {
    if (!error) { error = Conflict(); } return {};
  }
  auto result = std::unique_ptr<ProfilePublicationIntentStore>(new ProfilePublicationIntentStore);
  result->owner_ = owner; result->recovery_ = recovery; result->view_ = view;
  result->journal_ = journal; result->journal_token_ = token; result->recovered_ = true;
  result->artifacts_ = std::make_unique<ArtifactState>(); result->artifacts_->record = std::move(record);
  auto& saved = *result->artifacts_->record; saved.root = native;
  // A genuine historical completion never reacquires old before members or
  // overwrites later ordinary data. Verify its exact record before returning.
  const auto done = owner->root()->CaptureSnapshot(ArtifactDoneLeaf(saved), 56, error);
  if (!done) { return {}; }
  if (done->exists()) {
    if (!Equal(done->bytes(), ArtifactDone(saved))) { error = Bad(); return {}; }
    return result;
  }
  std::vector<ProfileArtifactExpected> main; bool thumbnail = false;
  for (const auto& file : saved.files) {
    if (file.kind == ProfileArtifactKind::kThumbnail) { thumbnail = true; continue; }
    auto current = view->CaptureSnapshot(file.leaf, ArtifactMaximum(file.kind), error, true);
    if (!current || (current->exists() && (error = ArtifactBytes(file.kind, current->bytes())))) { return {}; }
    main.push_back({file.kind, file.title, std::move(current)});
  }
  auto admission = AcquireArtifactsInternal(owner, recovery, std::move(metadata), std::move(binding),
      std::move(main), thumbnail, error, journal);
  if (!admission) { return {}; }
  result->target_lease_ = admission->data_->target;
  result->artifacts_->admission = std::move(admission); result->published_.resize(saved.files.size());
  return result;
}
GpdIntentAttempt ProfilePublicationIntentStore::AdvanceArtifacts() {
  GpdIntentAttempt attempt;
  if (!artifacts_ || !artifacts_->record || !owner_ || !journal_token_) {
    attempt.error = Bad(); return attempt;
  }
  auto& record = *artifacts_->record; attempt.journal = journal_; attempt.done = ArtifactDoneLeaf(record);
  attempt.files.resize(record.files.size());
  std::unique_lock lock(owner_->mutex_, std::try_to_lock);
  if (!lock.owns_lock()) { attempt.error = std::make_error_code(std::errc::device_or_resource_busy); return attempt; }
  attempt.error = owner_->managed_identity()->CheckCurrent(); if (attempt.error) { return attempt; }
  const auto journal = Capture(*owner_, journal_, kMaximumJournal, attempt.error);
  if (!journal || !Equal(journal->bytes(), journal_token_->bytes()) ||
      journal->info().file_id != journal_token_->info().file_id) {
    if (!attempt.error) { attempt.error = Conflict(); } return attempt;
  }
  const auto intent = owner_->Observe(*journal); attempt.error = intent.error;
  attempt.intent_durable = intent.durable; if (attempt.error) { return attempt; }
  const auto done = owner_->root()->CaptureSnapshot(attempt.done, 56, attempt.error);
  if (!done) { return attempt; }
  if (done->exists()) {
    if (!Equal(done->bytes(), ArtifactDone(record))) { attempt.error = Bad(); return attempt; }
    const auto observed = owner_->Observe(*done); attempt.error = observed.error;
    attempt.done_durable = observed.durable; attempt.historically_complete = true; return attempt;
  }
  if (!artifacts_->admission || !artifacts_->admission->data_) { attempt.error = Bad(); return attempt; }
  auto& data = *artifacts_->admission->data_;
  if (data.owner != owner_ || data.members->base() != view_ || !SameBinding(data.binding, record.binding)) {
    attempt.error = Conflict(); return attempt;
  }
  if (data.thumbnail && (attempt.error = data.thumbnail->CheckCurrent(*data.members))) { return attempt; }
  std::vector<std::shared_ptr<const HostStorageSnapshotToken>> current(record.files.size());
  std::vector<std::shared_ptr<const HostStorageSnapshotToken>> expected(record.files.size());
  for (size_t i = 0; i != record.files.size(); ++i) {
    const auto& file = record.files[i]; auto& out = attempt.files[i];
    const auto before = std::find_if(data.expected.begin(), data.expected.end(), [&](const auto& item) {
      return item.kind == file.kind && item.title_id == file.title;
    });
    if (before == data.expected.end()) { out.error = Bad(); }
    else { expected[i] = before->token; }
    if (!out.error) { current[i] = data.members->Capture(file.leaf, ArtifactMaximum(file.kind), out.error, true); }
    if (!out.error && current[i]->exists()) { out.error = ArtifactBytes(file.kind, current[i]->bytes()); }
    if (!out.error && !ArtifactLeafMatches(file.kind, file.title, current[i]->relative())) { out.error = Conflict(); }
    if (!out.error) {
      const auto& token = current[i];
      const bool before_matches = token->exists() == file.before_exists &&
          Equal(token->bytes(), file.before) && (!file.before_exists ||
            (token->info().file_id == file.before_info.file_id &&
             (recovered_ || token->info().filesystem_id == file.before_info.filesystem_id)));
      if (file.action == ProfileArtifactAction::kObserve) {
        if (before_matches) { out.state = GpdIntentFileReceipt::State::kIntended; }
        else { out.error = Conflict(); }
      } else if ((file.action == ProfileArtifactAction::kDelete && !token->exists()) ||
          (file.action != ProfileArtifactAction::kDelete && token->exists() && Equal(token->bytes(), file.intended))) {
        out.state = GpdIntentFileReceipt::State::kIntended;
      } else if (before_matches) { out.state = GpdIntentFileReceipt::State::kBefore; }
      else { out.error = Conflict(); }
    }
    if (out.error) {
      out.state = GpdIntentFileReceipt::State::kConflict;
      if (!attempt.error) { attempt.error = out.error; }
    }
  }
  if (attempt.error) { return attempt; } // All members preflight before mutation.
  for (size_t i = 0; i != record.files.size(); ++i) {
    const auto& file = record.files[i]; auto& out = attempt.files[i];
    if (out.state == GpdIntentFileReceipt::State::kBefore) {
      out.publication = file.action == ProfileArtifactAction::kDelete
          ? data.members->Delete(*recovery_, expected[i])
          : data.members->Publish(*recovery_, expected[i], file.intended);
      if (out.publication.published && out.publication.current) { published_[i] = out.publication.current; }
      out.error = out.publication.error;
      if (!out.error && !out.publication.durable) { out.error = Conflict(); }
      if (out.publication.current) { current[i] = out.publication.current; }
    } else if (published_[i]) {
      out.retry = data.members->RetryDurability(published_[i]); out.error = out.retry.error;
      if (out.retry.current) { current[i] = out.retry.current; }
    } else {
      out.observation = data.members->ObserveDurability(current[i]); out.error = out.observation.error;
      if (!out.error && (!out.observation.matched || !out.observation.durable)) { out.error = Conflict(); }
    }
    if (out.error) { attempt.error = out.error; return attempt; }
  }
  attempt.all_durable_sequentially_observed = true;
  // The typed CM member and all main members are rechecked under the SAME
  // genuine guard through Done publication, not only sequentially observed.
  {
    auto batch = data.members->Validate(current, attempt.error); if (!batch) { return attempt; }
    const auto bytes = ArtifactDone(record);
    const auto written = owner_->root()->WriteNewProfileDoneWithSnapshotGuard(*batch, attempt.done, bytes);
    attempt.error = written.error;
    if (!attempt.error && (!written.durable || written.bytes != bytes.size())) {
      attempt.error = std::make_error_code(std::errc::io_error);
    }
    if (attempt.error) { return attempt; }
  }
  const auto verified = Capture(*owner_, attempt.done, 56, attempt.error);
  if (!attempt.error && !Equal(verified->bytes(), ArtifactDone(record))) { attempt.error = Conflict(); }
  attempt.done_durable = !attempt.error; return attempt;
}
std::error_code ProfilePublicationIntentStore::ReplayBeforeGuestAdmission(
    std::shared_ptr<const ProfileIntentOwner> owner,
    std::shared_ptr<const SnapshotRecoveryOwner> recovery,
    std::shared_ptr<ContentMutationOwner> metadata) {
  if (!owner || !recovery) { return std::make_error_code(std::errc::invalid_argument); }
  std::error_code error; const auto rows = Scan(*owner, error); if (error) { return error; }
  for (const auto& row : rows) {
    if (row.done_record_valid) { continue; }
    //03 retains its existing explicit ProfileStorageLease-driven recovery.
    // Its older public producer accepted noncanonical relative directories;
    // startup has no authority to reconstruct that lease from journal text.
    // Scan still validates all03 records and rejects malformed/foreign bytes.
    if (row.format_version == 3) { continue; }
    // Logical canonical identity is independently derived; arbitrary paths in
    // old/private records are never adopted as profile-directory authority.
    if (!CanonicalBinding(row.binding) || !BindingOwnedBy(row.binding, *owner->managed_identity())) { return Bad(); }
    auto view = owner->source()->Subroot(row.binding.profile_relative, false, error);
    if (!view) { return error; }
    auto store = Recover(owner, recovery, row.journal, row.binding, view, error, metadata);
    if (!store) { return error ? error : Conflict(); }
    const auto advanced = store->Advance();
    if (advanced.error || !advanced.done_durable) { return advanced.error ? advanced.error : Conflict(); }
  }
  return {};
}
GpdIntentAttempt ProfilePublicationIntentStore::Advance() {
  if (artifacts_) { return AdvanceArtifacts(); }
  GpdIntentAttempt attempt;
  attempt.journal = journal_;
  attempt.done = DoneLeaf(*record_);
  attempt.files.resize(record_->files.size());
  std::unique_lock lock(owner_->mutex_, std::try_to_lock);
  if (!lock.owns_lock()) {
    attempt.error = std::make_error_code(std::errc::device_or_resource_busy);
    return attempt;
  }
  if (owner_->Identity(*view_, attempt.error) != record_->root) {
    if (!attempt.error) {
      attempt.error = Conflict();
    }
    return attempt;
  }
  attempt.error = owner_->managed_identity()->CheckCurrent();
  if (attempt.error) {
    return attempt;
  }
  auto current_journal =
      Capture(*owner_, journal_, kMaximumJournal, attempt.error);
  if (!attempt.error &&
      (!Equal(current_journal->bytes(), journal_token_->bytes()) ||
       current_journal->info().file_id != journal_token_->info().file_id)) {
    attempt.error = Conflict();
  }
  if (attempt.error) {
    return attempt;
  }
  const auto journal_durable = owner_->Observe(*current_journal);
  attempt.error = journal_durable.error;
  attempt.intent_durable = journal_durable.durable;
  if (attempt.error) {
    return attempt;
  }
  auto done = owner_->root()->CaptureSnapshot(attempt.done, 56, attempt.error);
  if (attempt.error) {
    return attempt;
  }
  if (done->exists()) {
    if (!Equal(done->bytes(), DoneBytes(*record_))) {
      attempt.error = Bad();
      return attempt;
    }
    const auto observed = owner_->Observe(*done);
    attempt.error = observed.error;
    attempt.done_durable = observed.durable;
    attempt.historically_complete = true;
    return attempt;  // Later ordinary profile changes must never be replayed.
  }
  std::vector<ProfileGpdSnapshotLoad> current(record_->files.size());
  // Preflight both members before any target mutation, avoiding a known
  // second-file conflict needlessly publishing the first member.
  for (size_t i = 0; i != record_->files.size(); ++i) {
    const auto& f = record_->files[i];
    auto& out = attempt.files[i];
    current[i] = ProfileGpdSnapshotStore::Load(
        {record_->binding.xuid, record_->binding.source_generation, f.title},
        view_);
    out.error = current[i].error;
    if (!out.error && current[i].token->relative() != f.leaf &&
        !(f.action == GpdIntentAction::kDelete && !current[i].token->exists() &&
          SameLeaf(current[i].token->relative(), f.title))) {
      // A deleted case-folded leaf has no remaining physical spelling. Its
      // canonical missing slot is still the same exact journaled title slot.
      out.error = Conflict();
    }
    if (!out.error) {
      const auto& token = current[i].token;
      if ((f.action == GpdIntentAction::kDelete && !token->exists()) ||
          (f.action != GpdIntentAction::kDelete && token->exists() &&
           Equal(token->bytes(), f.intended))) {
        out.state = GpdIntentFileReceipt::State::kIntended;
      } else if ((f.action == GpdIntentAction::kCreate && !token->exists()) ||
                 ((f.action == GpdIntentAction::kReplace ||
                   f.action == GpdIntentAction::kDelete) &&
                  token->exists() && Equal(token->bytes(), f.before) &&
                  (recovered_ || token->info().filesystem_id ==
                                     f.before_info.filesystem_id) &&
                  token->info().file_id == f.before_info.file_id)) {
        out.state = GpdIntentFileReceipt::State::kBefore;
      } else {
        out.error = Conflict();
      }
    }
    if (out.error) {
      out.state = GpdIntentFileReceipt::State::kConflict;
      if (!attempt.error) {
        attempt.error = out.error;
      }
    }
  }
  if (attempt.error) {
    return attempt;
  }
  for (size_t i = 0; i != record_->files.size(); ++i) {
    auto& out = attempt.files[i];
    if (out.state == GpdIntentFileReceipt::State::kBefore) {
      if (record_->files[i].action == GpdIntentAction::kDelete) {
        auto deleted = ProfileGpdSnapshotStore::Delete(*recovery_, current[i]);
        out.publication = std::move(deleted.storage);
      } else {
        auto written = ProfileGpdSnapshotStore::Publish(*recovery_, current[i],
                                                 record_->files[i].intended);
        out.publication = std::move(written.storage);
      }
      if (out.publication.published) {
        published_[i] = out.publication.current;
      }
      out.error = out.publication.error;
      if (!out.error && !out.publication.durable) {
        out.error = Conflict();
      }
    } else if (published_[i]) {
      // Only an actual prior CAS receipt minted this token. A restart/read
      // token never reaches the publication retry path.
      out.retry = view_->RetrySnapshotDurability(published_[i]);
      out.error = out.retry.error;
    } else {
      // Journal-authorized reconciliation of current intended bytes/absence,
      // not a reconstructed CAS token or proof this process published them.
      out.observation = owner_->Observe(*current[i].token);
      out.error = out.observation.error;
    }
    if (out.error) {
      attempt.error = out.error;
      return attempt;
    }
  }
  attempt.all_durable_sequentially_observed = true;
  // The profile model authority must keep all mutators gated through this
  // marker boundary. Per-member CAS/observations do not lock a two-file view.
  const auto wire = DoneBytes(*record_);
  const auto write = owner_->root()->WriteNewFile(attempt.done, wire);
  attempt.error = write.error;
  if (!attempt.error && write.bytes != wire.size()) {
    attempt.error = std::make_error_code(std::errc::io_error);
  }
  if (attempt.error) {
    return attempt;
  }
  auto verified = Capture(*owner_, attempt.done, 56, attempt.error);
  if (!attempt.error && !Equal(verified->bytes(), wire)) {
    attempt.error = Conflict();
  }
  attempt.done_durable = !attempt.error;
  return attempt;
}
}  // namespace xe::filesystem
