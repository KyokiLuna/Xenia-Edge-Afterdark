/** Xenia : Xbox 360 Emulator Research Project; BSD license, see LICENSE. */
#include "xenia/kernel/xam/fresh_profile_con_epoch.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <map>
#include "third_party/mbedtls/include/mbedtls/sha256.h"
#include "xenia/base/mutex.h"
#include "xenia/base/profile_publication_intent.h"
#include "xenia/kernel/xam/content_manager.h"
#include "xenia/kernel/xam/folder_content_header.h"
namespace xe::kernel::xam {
namespace {
namespace fs = filesystem;
namespace wire = fs::content_mutation_wire;
namespace con = folder_content_detail;
constexpr std::array<uint8_t, 8> kMagic{'X', 'E', 'P', 'C', 'M', '0', '0', '1'};
constexpr size_t kAligned = 0xA000;
std::error_code Error(std::errc e) { return std::make_error_code(e); }
bool Same(std::span<const uint8_t> a, std::span<const uint8_t> b) {
  return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin());
}
uint64_t Hex(std::string_view value) {
  uint64_t result = 0;
  for (unsigned char c : value) {
    result = result * 16 + (c <= '9'   ? c - '0'
                            : c <= 'F' ? c - 'A' + 10
                                       : c - 'a' + 10);
  }
  return result;
}
std::optional<con::PackageIdentity> Profile(const std::filesystem::path& p) {
  if (!wire::ValidPackage(p)) {
    return {};
  }
  std::vector<std::string> parts;
  for (const auto& v : p) {
    parts.push_back(v.generic_string());
  }
  if (parts.size() != 4 || Hex(parts[0]) == 0 ||
      !xe::utf8::equal_case(parts[0], parts[3]) ||
      Hex(parts[1]) != 0xFFFE07D1 ||
      Hex(parts[2]) != uint32_t(XContentType::kProfile)) {
    return {};
  }
  return con::PackageIdentity{Hex(parts[0]), 0xFFFE07D1,
                              XContentType::kProfile};
}
struct Writer {
  std::vector<uint8_t> bytes;
  void U64(uint64_t v) {
    for (int n = 7; n >= 0; --n) {
      bytes.push_back(uint8_t(v >> (n * 8)));
    }
  }
  void U32(uint32_t v) {
    for (int n = 3; n >= 0; --n) {
      bytes.push_back(uint8_t(v >> (n * 8)));
    }
  }
  void U16(uint16_t v) {
    bytes.push_back(uint8_t(v >> 8));
    bytes.push_back(uint8_t(v));
  }
  void Blob(std::span<const uint8_t> s) {
    bytes.insert(bytes.end(), s.begin(), s.end());
  }
  void Path(const std::filesystem::path& p) {
    auto text = p.generic_string();
    U32(uint32_t(text.size()));
    Blob({reinterpret_cast<const uint8_t*>(text.data()), text.size()});
  }
};
struct Reader {
  std::span<const uint8_t> bytes;
  bool good = true;
  std::span<const uint8_t> Take(size_t n) {
    if (n > bytes.size()) {
      good = false;
      return {};
    }
    auto out = bytes.first(n);
    bytes = bytes.subspan(n);
    return out;
  }
  uint64_t U64() {
    uint64_t out = 0;
    for (auto v : Take(8)) {
      out = (out << 8) | v;
    }
    return out;
  }
  uint32_t U32() {
    uint32_t out = 0;
    for (auto v : Take(4)) {
      out = (out << 8) | v;
    }
    return out;
  }
  uint16_t U16() {
    uint16_t out = 0;
    for (auto v : Take(2)) {
      out = uint16_t((out << 8) | v);
    }
    return out;
  }
  std::filesystem::path Path() {
    auto n = U32();
    if (n > 4096) {
      good = false;
      return {};
    }
    auto text = Take(n);
    return std::string(reinterpret_cast<const char*>(text.data()), text.size());
  }
};
std::array<uint8_t, 32> MemberDigest(fs::ContentPackageStreamSource& source,
                                     size_t index, std::error_code& error) {
  std::array<uint8_t, 32> digest{};
  if (source.members()[index].directory) {
    return digest;
  }
  struct Sha {
    mbedtls_sha256_context value;
    Sha() { mbedtls_sha256_init(&value); }
    ~Sha() { mbedtls_sha256_free(&value); }
  } sha;
  if (mbedtls_sha256_starts(&sha.value, 0)) {
    error = Error(std::errc::io_error);
    return {};
  }
  std::array<uint8_t, 65536> buffer;
  for (uint64_t offset = 0; offset < source.members()[index].size;) {
    auto count = size_t(std::min<uint64_t>(
        buffer.size(), source.members()[index].size - offset));
    auto io = source.Read(index, offset, std::span(buffer).first(count));
    if (io.error || io.bytes != count) {
      error = io.error ? io.error : Error(std::errc::io_error);
      return {};
    }
    if (mbedtls_sha256_update(&sha.value, buffer.data(), count)) {
      error = Error(std::errc::io_error);
      return {};
    }
    offset += count;
  }
  if (mbedtls_sha256_finish(&sha.value, digest.data())) {
    error = Error(std::errc::io_error);
  }
  return digest;
}
std::vector<uint8_t> Map(const FreshProfileConEpoch::Reopened& facts,
                         const fs::ManagedDeviceIdentity& identity,
                         const std::filesystem::path& package,
                         fs::ContentPackageStreamSource& source,
                         std::error_code& error) {
  Writer w;
  w.Blob(kMagic);
  w.U32(vfs::StfsLogicalSnapshot::kLayoutVersion);
  w.Blob(identity.uuid());
  w.U64(identity.content_epoch());
  w.U64(identity.directory_file_id());
  w.Path(package);
  w.U64(facts.declared_creation);
  w.U32(uint32_t(source.members().size()));
  w.U32(uint32_t(facts.original_legacy_header.size()));
  w.Blob(facts.original_legacy_header);
  std::map<std::filesystem::path, const vfs::StfsSnapshotMemberLayout*>
      assignments;
  for (const auto& row : facts.logical->member_layout()) {
    assignments.emplace(row.relative, &row);
  }
  for (size_t i = 0; i < source.members().size(); ++i) {
    const auto& member = source.members()[i];
    const auto& time = facts.metadata[i];
    w.Path(member.relative);
    w.U32(member.directory ? 1 : 0);
    w.U64(member.size);
    w.U16(time.create_date);
    w.U16(time.create_time);
    w.U16(time.modified_date);
    w.U16(time.modified_time);
    auto assigned = assignments.find(member.relative);
    if (assigned == assignments.end()) {
      error = Error(std::errc::state_not_recoverable);
      return {};
    }
    const auto& layout = *assigned->second;
    w.U32(layout.entry_index);
    w.U32(layout.start_block);
    w.U32(layout.block_count);
    w.Blob(MemberDigest(source, i, error));
    if (error) {
      return {};
    }
  }
  auto digest = wire::Digest(w.bytes, error);
  if (error) {
    return {};
  }
  w.Blob(digest);
  return std::move(w.bytes);
}
std::optional<std::vector<vfs::StfsSnapshotMetadata>> NewTimes(
    const fs::HostStorageContentSnapshot& source, uint64_t declared,
    std::error_code& error) {
  std::vector<vfs::StfsSnapshotMetadata> out;
  std::map<std::filesystem::path, const fs::ContentPackageTreeEntry*> entries;
  for (const auto& row : source.layout().entries()) {
    entries.emplace(row.relative, &row);
  }
  for (const auto& member : source.members()) {
    auto found = entries.find(member.relative);
    if (found == entries.end()) {
      error = Error(std::errc::state_not_recoverable);
      return {};
    }
    const auto* row = found->second;
    if (row->write_seconds < -11644473600ll ||
        row->write_seconds >
            int64_t(UINT64_MAX / 10000000ull) - 11644473600ll ||
        row->write_nanoseconds >= 1000000000u) {
      error = Error(std::errc::operation_not_supported);
      return {};
    }
    fs::FileInfo info{};
    info.create_timestamp =
        declared;  // Explicit NEW canonical-epoch creation only.
    const uint64_t seconds = uint64_t(row->write_seconds + 11644473600ll);
    const uint64_t subsecond = row->write_nanoseconds / 100;
    if (seconds > (UINT64_MAX - subsecond) / 10000000ull) {
      error = Error(std::errc::operation_not_supported);
      return {};
    }
    info.write_timestamp = seconds * 10000000ull + subsecond;
    auto native =
        vfs::StfsSnapshotMetadata::FromFileInfo(member.relative, info, error);
    if (!native) {
      return {};
    }
    out.push_back(*native);
  }
  return out;
}
std::error_code Legacy(std::span<const uint8_t> bytes,
                       con::PackageIdentity expected,
                       const std::filesystem::path& package) {
  if (bytes.size() < sizeof(XCONTENT_AGGREGATE_DATA) ||
      bytes.size() > wire::kMaximumMetadataBytes ||
      !std::memcmp(bytes.data(), "CON ", 4) ||
      !std::memcmp(bytes.data(), "LIVE", 4) ||
      !std::memcmp(bytes.data(), "PIRS", 4)) {
    return Error(std::errc::operation_not_supported);
  }
  XCONTENT_AGGREGATE_DATA old{};
  std::memcpy(&old, bytes.data(), sizeof(old));
  if (uint64_t(old.xuid) != expected.xuid ||
      old.title_id != expected.title_id ||
      old.content_type != expected.content_type ||
      old.display_name_raw.uint[127] ||
      !xe::utf8::equal_case(old.file_name(),
                            package.filename().generic_string())) {
    return Error(std::errc::bad_message);
  }
  return {};
}
// Decode only format facts from an ALREADY owned complete immutable source.
// Both cold read and live-session binding use the exact same digest, FAT,
// allocation and signed descriptor checks. Their distinct final source/Header
// ownership joins remain with their actual caller, never this byte parser.
std::unique_ptr<FreshProfileConEpoch::Reopened> DecodeCanonical(
    std::span<const uint8_t> bytes, const fs::ManagedDeviceIdentity& managed,
    const std::filesystem::path& package, con::PackageIdentity identity,
    fs::HostStorageContentSnapshot& snapshot, std::span<const uint8_t> trusted,
    std::error_code& error) {
  // Both callers check the bounded aligned Header shape BEFORE capturing
  // members. No extra shape pass or malformed-header IO is introduced here.
  Reader r{bytes.subspan(kAligned, bytes.size() - kAligned - 32)};
  auto body = r.bytes;
  auto encoded_digest = wire::Digest(body, error);
  if (error || !Same(encoded_digest, bytes.last(32)) ||
      !Same(r.Take(8), kMagic) ||
      r.U32() != vfs::StfsLogicalSnapshot::kLayoutVersion ||
      !Same(r.Take(16), managed.uuid()) ||
      r.U64() != managed.content_epoch() ||
      r.U64() != managed.directory_file_id() ||
      r.Path() != package) {
    error = Error(std::errc::bad_message);
    return {};
  }
  auto out = std::make_unique<FreshProfileConEpoch::Reopened>();
  out->declared_creation = r.U64();
  fs::FileInfo declared{};
  declared.create_timestamp = declared.write_timestamp = out->declared_creation;
  auto birth =
      vfs::StfsSnapshotMetadata::FromFileInfo("declared", declared, error);
  if (!birth) {
    return {};
  }
  auto count = r.U32(), legacy_size = r.U32();
  auto legacy = r.Take(legacy_size);
  if (count != snapshot.members().size() ||
      count > vfs::StfsLogicalSnapshot::kMaximumMembers || !r.good ||
      Legacy(legacy, identity, package)) {
    error = Error(std::errc::bad_message);
    return {};
  }
  out->original_legacy_header.assign(legacy.begin(), legacy.end());
  std::vector<vfs::StfsSnapshotMemberLayout> layout;
  for (size_t i = 0; i < count; ++i) {
    const auto& member = snapshot.members()[i];
    auto path = r.Path();
    auto directory = r.U32();
    auto size = r.U64();
    vfs::StfsSnapshotMetadata t{path, r.U16(), r.U16(), r.U16(), r.U16()};
    layout.push_back({path, r.U32(), r.U32(), r.U32()});
    auto digest = r.Take(32);
    auto actual = MemberDigest(snapshot, i, error);
    if (error || !r.good || path != member.relative ||
        directory != (member.directory ? 1u : 0u) || size != member.size ||
        t.create_date != birth->create_date ||
        t.create_time != birth->create_time || !Same(digest, actual)) {
      error = Error(std::errc::bad_message);
      return {};
    }
    out->metadata.push_back(t);
  }
  if (!r.good || !r.bytes.empty()) {
    error = Error(std::errc::bad_message);
    return {};
  }
  // Map metadata is not trusted merely because its checksum parses. Rebuild
  // against complete current bytes and compare the actual signed root/layout.
  out->logical =
      vfs::StfsLogicalSnapshot::Build(snapshot, out->metadata, error);
  if (!out->logical) {
    return {};
  }
  std::map<std::filesystem::path, vfs::StfsSnapshotMemberLayout> assignments;
  for (const auto& row : out->logical->member_layout()) {
    assignments.emplace(row.relative, row);
  }
  for (const auto& row : layout) {
    auto actual = assignments.find(row.relative);
    if (actual == assignments.end() || actual->second != row) {
      error = Error(std::errc::bad_message);
      return {};
    }
  }
  auto facts = con::ValidateNativeHeader(
      bytes, identity, snapshot.layout().statistics().data_extent, trusted,
      error);
  // Native metadata+8 is its actual declared size/geometry input, including
  // the genuine zero/default lane. Signature/identity/owned-extent checks
  // above remain mandatory; SUM(member bytes) is not this field's producer.
  if (!facts || facts->descriptor != out->logical->descriptor()) {
    if (!error) {
      error = Error(std::errc::bad_message);
    }
    return {};
  }
  return out;
}
}  // namespace
struct FreshProfileConEpoch::Data {
  Source source;
  std::shared_ptr<fs::ContentMutationOwner> owner;
  std::shared_ptr<fs::HostStorageRoot> header_reservation;
  std::unique_ptr<fs::HostStorageContentSnapshot> snapshot;
  std::unique_ptr<fs::PreparedSnapshotPublication> prepared;
  fs::SnapshotPublicationReceipt failed;
  bool active = false;
  std::mutex mutex;
  ~Data() {
    prepared.reset();
    snapshot.reset();
    if (header_reservation) {
      header_reservation->Retire();
      header_reservation->ReleaseRetiredSnapshotReservation();
    }
    if (active) {
      owner->active_.fetch_sub(1, std::memory_order_acq_rel);
    }
  }
};
FreshProfileConEpoch::FreshProfileConEpoch(std::unique_ptr<Data> data)
    : data_(std::move(data)) {}
FreshProfileConEpoch::~FreshProfileConEpoch() = default;
const fs::SnapshotPublicationReceipt& FreshProfileConEpoch::receipt() const {
  return data_->prepared ? data_->prepared->receipt() : data_->failed;
}
std::unique_ptr<FreshProfileConEpoch> FreshProfileConEpoch::Prepare(
    Source source, std::span<const uint8_t> metadata, uint64_t declared,
    const ConsoleSigningIdentity& signer, std::error_code& error,
    const std::atomic<bool>* canceled) {
  error.clear();
  if (global_critical_region::is_held_by_current_thread() || !source.volume ||
      source.volume->error() || !source.volume->identity() ||
      !source.volume->mutations() || !source.volume->recovery() ||
      !source.members || !source.members->base() ||
      !source.members->base()->directory_identity() ||
      metadata.size() != sizeof(vfs::XContentMetadata)) {
    error = Error(std::errc::invalid_argument);
    return {};
  }
  fs::FileInfo declaration{};
  declaration.create_timestamp = declaration.write_timestamp = declared;
  if (!vfs::StfsSnapshotMetadata::FromFileInfo("declared", declaration,
                                               error)) {
    return {};
  }
  auto expected = Profile(source.package);
  auto owner = source.volume->mutations();
  if (!expected || owner->identity_ != source.volume->identity() ||
      !owner->content_->directory_identity()->SameEntry(
          source.package, *source.members->base()->directory_identity(), {})) {
    error = Error(std::errc::permission_denied);
    return {};
  }
  std::unique_lock operation(owner->operation_mutex_, std::try_to_lock);
  if (!operation.owns_lock()) {
    error = Error(std::errc::device_or_resource_busy);
    return {};
  }
  error = owner->CheckCurrent();
  if (!error &&
      (!owner->ready() || owner->active_.load(std::memory_order_acquire))) {
    error = Error(std::errc::device_or_resource_busy);
  }
  if (error) {
    return {};
  }
  auto data = std::make_unique<Data>();
  data->source = std::move(source);
  data->owner = owner;
  owner->active_.fetch_add(1, std::memory_order_acq_rel);
  data->active = true;
  wire::Record slot;
  slot.kind = wire::Kind::kHeader;
  slot.package = data->source.package;
  auto target = wire::Target(slot);
  // Existing-only repair: unlike the ordinary create producer, this admission
  // must not create missing Headers directories while classifying a before.
  error = owner->content_->CheckContentPathCase(target);
  if (error) {
    return {};
  }
  auto before = owner->content_->CaptureSnapshot(
      target, wire::kMaximumMetadataBytes, error, true);
  if (!before) {
    return {};
  }
  error = Legacy(before->bytes(), *expected, data->source.package);
  if (error) {
    return {};
  }
  auto parent = owner->content_->Subroot(target.parent_path(), false, error);
  if (!parent) {
    return {};
  }
  auto linked = parent->WithDependency(data->source.members->base(), error);
  if (!linked) {
    return {};
  }
  auto local = linked->CaptureSnapshot(
      target.filename(), wire::kMaximumMetadataBytes, error, true);
  if (!local || !local->exists() || !Same(local->bytes(), before->bytes()) ||
      local->info().file_id != before->info().file_id ||
      local->info().filesystem_id != before->info().filesystem_id) {
    if (!error) {
      error = Error(std::errc::state_not_recoverable);
    }
    return {};
  }
  data->header_reservation = linked->ReserveSnapshotFile(local, error);
  if (!data->header_reservation) {
    return {};
  }
  local = data->header_reservation->CaptureSnapshot(
      target.filename(), wire::kMaximumMetadataBytes, error, true);
  if (!local || !local->exists() || !Same(local->bytes(), before->bytes()) ||
      local->info().file_id != before->info().file_id ||
      local->info().filesystem_id != before->info().filesystem_id) {
    if (!error) {
      error = Error(std::errc::state_not_recoverable);
    }
    return {};
  }
  data->snapshot = data->source.members->CaptureContentSnapshotForCM(
      *owner->content_, data->source.package, error);
  if (!data->snapshot) {
    return {};
  }
  // The existing common Header bound covers the complete compact record as
  // well as aligned native bytes. Admit its exact size before allocating or
  // signing; no new arbitrary memory quota or payload duplication is needed.
  uint64_t map_size = 8 + 4 + 16 + 8 + 8 + 4 +
                      data->source.package.generic_string().size() + 8 + 4 + 4 +
                      before->bytes().size() + 32;
  for (const auto& member : data->snapshot->members()) {
    map_size +=
        4 + member.relative.generic_string().size() + 4 + 8 + 8 + 12 + 32;
  }
  if (map_size > wire::kMaximumMetadataBytes - kAligned) {
    error = Error(std::errc::file_too_large);
    return {};
  }
  auto times = NewTimes(*data->snapshot, declared, error);
  if (!times) {
    return {};
  }
  Reopened facts;
  facts.metadata = std::move(*times);
  facts.declared_creation = declared;
  facts.original_legacy_header.assign(before->bytes().begin(),
                                      before->bytes().end());
  facts.logical =
      vfs::StfsLogicalSnapshot::Build(*data->snapshot, facts.metadata, error);
  if (!facts.logical) {
    return {};
  }
  vfs::XContentContainerHeader header{};
  std::memcpy(&header.content_metadata, metadata.data(), metadata.size());
  const auto console_id = signer.console_id();
  if (uint64_t(header.content_metadata.profile_id) != expected->xuid ||
      uint32_t(header.content_metadata.execution_info.title_id) !=
          expected->title_id ||
      header.content_metadata.content_type != expected->content_type ||
      header.content_metadata.volume_type != vfs::XContentVolumeType::kStfs ||
      !std::equal(console_id.begin(), console_id.end(),
                  header.content_metadata.console_id)) {
    error = Error(std::errc::permission_denied);
    return {};
  }
  // Preserve the actual complete creation metadata, especially its +8
  // declared size/geometry (zero selects the native default lane). This
  // legacy repair owns only its real folder extent, not a hypothetical larger
  // raw allocation; a larger declared capacity needs that separate producer.
  if (uint64_t(header.content_metadata.content_size) >
      data->snapshot->layout().statistics().data_extent) {
    error = Error(std::errc::operation_not_supported);
    return {};
  }
  // Native86DA0's fixed first344/new padding and first licensee defaults.
  // This declared canonical transition assigns only its NEW descriptor;
  // complete input metadata and original member bytes remain intact.
  header.content_header.magic = vfs::XContentPackageType::kCon;
  header.content_header.header_size = sizeof(header);
  header.content_header.licenses[0].licensee_id = UINT64_MAX;
  std::memcpy(&header.content_metadata.volume_descriptor.stfs,
              facts.logical->descriptor().data(), 36);
  std::vector<uint8_t> intended(kAligned);
  std::memcpy(intended.data(), &header, sizeof(header));
  error = con::SignNativeHeader(intended, signer);
  if (error) {
    return {};
  }
  auto map = Map(facts, *data->source.volume->identity(), data->source.package,
                 *data->snapshot, error);
  if (error || map.size() > wire::kMaximumMetadataBytes - intended.size()) {
    if (!error) {
      error = Error(std::errc::file_too_large);
    }
    return {};
  }
  intended.insert(intended.end(), map.begin(), map.end());
  // Stage+exact old Header backup precede publication. No profile file copies,
  // Account mutation, guest completion or native control grant are performed.
  data->prepared = fs::SnapshotPublisher::Prepare(
      *data->source.volume->recovery(), local, intended, canceled);
  if (!data->prepared) {
    error = Error(std::errc::not_enough_memory);
    return {};
  }
  error = data->prepared->receipt().error;
  return std::unique_ptr<FreshProfileConEpoch>(
      new FreshProfileConEpoch(std::move(data)));
}
fs::SnapshotPublicationReceipt FreshProfileConEpoch::Commit(
    const std::atomic<bool>* canceled) {
  if (global_critical_region::is_held_by_current_thread()) {
    auto failed = receipt();
    failed.error = Error(std::errc::operation_not_permitted);
    return failed;
  }
  std::lock_guard operation(data_->mutex);
  if (!data_->prepared) {
    return data_->failed;
  }
  if (data_->prepared->receipt().error) {
    return data_->prepared->receipt();
  }
  std::error_code error;
  auto guard = data_->snapshot->Validate(error);
  if (!guard) {
    auto failed = receipt();
    failed.error = error;
    return failed;
  }
  // The full actual flat membership and every immutable byte token remain
  // guarded until the SAME native Header rename and durability barrier finish.
  return fs::SnapshotPublisher::CommitWithSnapshotGuard(*data_->prepared,
                                                        *guard, canceled);
}
std::unique_ptr<FreshProfileConEpoch::Reopened> FreshProfileConEpoch::Open(
    Source source, std::span<const uint8_t> trusted, std::error_code& error) {
  error.clear();
  if (global_critical_region::is_held_by_current_thread() || !source.volume ||
      source.volume->error() || !source.volume->identity() ||
      !source.volume->mutations() || !source.volume->recovery() ||
      !source.members || !source.members->base() ||
      !source.members->base()->directory_identity()) {
    error = Error(std::errc::invalid_argument);
    return {};
  }
  auto identity = Profile(source.package);
  auto owner = source.volume->mutations();
  if (!identity || owner->identity_ != source.volume->identity() ||
      !owner->content_->directory_identity()->SameEntry(
          source.package, *source.members->base()->directory_identity(), {})) {
    error = Error(std::errc::permission_denied);
    return {};
  }
  wire::Record slot;
  slot.kind = wire::Kind::kHeader;
  slot.package = source.package;
  error = owner->CheckCurrent();
  if (error) {
    return {};
  }
  auto header = owner->content_->CaptureSnapshot(
      wire::Target(slot), wire::kMaximumMetadataBytes, error, true);
  if (!header) {
    return {};
  }
  // Header is outside the profile-root member batch. Own its exact existing
  // reservation through parsing/reconstruction and join its complete token
  // under the SAME final source guard; a pre-build read alone is insufficient.
  const auto target = wire::Target(slot);
  auto parent = owner->content_->Subroot(target.parent_path(), false, error);
  if (!parent) {
    return {};
  }
  auto linked = parent->WithDependency(source.members->base(), error);
  if (!linked) {
    return {};
  }
  auto local = linked->CaptureSnapshot(
      target.filename(), wire::kMaximumMetadataBytes, error, true);
  if (!local || !local->exists() || !Same(local->bytes(), header->bytes()) ||
      local->info().file_id != header->info().file_id ||
      local->info().filesystem_id != header->info().filesystem_id) {
    if (!error) {
      error = Error(std::errc::state_not_recoverable);
    }
    return {};
  }
  struct HeaderReadReservation {
    std::shared_ptr<fs::HostStorageRoot> root;
    ~HeaderReadReservation() {
      if (root) {
        root->Retire();
        root->ReleaseRetiredSnapshotReservation();
      }
    }
  } retained{linked->ReserveSnapshotFile(local, error)};
  if (!retained.root) {
    return {};
  }
  local = retained.root->CaptureSnapshot(
      target.filename(), wire::kMaximumMetadataBytes, error, true);
  if (!local || !local->exists() || !Same(local->bytes(), header->bytes()) ||
      local->info().file_id != header->info().file_id ||
      local->info().filesystem_id != header->info().filesystem_id) {
    if (!error) {
      error = Error(std::errc::state_not_recoverable);
    }
    return {};
  }
  auto bytes = local->bytes();
  if (bytes.size() < kAligned + 32 ||
      con::NativeHeaderAlignedSize(bytes) != kAligned) {
    error = Error(std::errc::bad_message);
    return {};
  }
  auto snapshot = source.members->CaptureContentSnapshotForCM(
      *owner->content_, source.package, error);
  if (!snapshot) {
    return {};
  }
  auto out = DecodeCanonical(bytes, *source.volume->identity(), source.package,
                             *identity, *snapshot, trusted, error);
  if (!out) { return {}; }
  // Native birth is the persisted exact FAT row, not generic batch ctime.
  // This read-only reopened format fact still grants NO Session/Control0/2/4.
  auto guard = snapshot->Validate(error);
  if (!guard) {
    return {};
  }
  error = retained.root->ValidateExternalContentHeaderWithSnapshotGuard(
      *guard, *owner->content_, source.package, target, *local);
  if (error) {
    return {};
  }
  return out;
}
struct FreshProfileConEpoch::Live::Data {
  Source source;
  std::shared_ptr<fs::ProfileArtifactAdmission> admission;
  std::shared_ptr<fs::ContentMutationOwner::ProfileHeaderPublication> header;
  std::shared_ptr<const fs::HostStorageSnapshotToken> before;
  std::unique_ptr<fs::HostStorageContentSnapshot> snapshot;
  std::unique_ptr<Reopened> facts;
};
FreshProfileConEpoch::Live::Live(std::shared_ptr<Data> data)
    : data_(std::move(data)) {}
FreshProfileConEpoch::Live::~Live() = default;
const FreshProfileConEpoch::Reopened& FreshProfileConEpoch::Live::facts() const {
  return *data_->facts;
}
const std::shared_ptr<const fs::HostStorageSnapshotToken>&
FreshProfileConEpoch::Live::header_before() const { return data_->before; }
std::unique_ptr<FreshProfileConEpoch::Live::Guard>
FreshProfileConEpoch::Live::Validate(std::error_code& error) const {
  return FreshProfileConEpoch::ValidateLive(*this, error);
}
std::unique_ptr<FreshProfileConEpoch::Live::Guard> FreshProfileConEpoch::ValidateLive(
    const Live& live, std::error_code& error) {
  error.clear();
  if (global_critical_region::is_held_by_current_thread()) {
    error = Error(std::errc::operation_would_block); return {};
  }
  // Allocate retained custody BEFORE acquiring the actual native guard.
  auto joined = std::unique_ptr<Live::Guard>(new Live::Guard(live.data_));
  const auto& data = *live.data_;
  // Never reserve or recapture an external Header beneath its own live
  // participant. The exact retained before token and complete member census
  // are checked under the SAME actual final batch/native mutation guard.
  auto batch = data.snapshot->Validate(error);
  if (!batch) { return {}; }
  error = data.header->CheckWithSnapshotGuard(*data.source.members, *batch,
                                             data.before);
  if (error) { return {}; }
  joined->batch_ = std::move(batch);
  return joined;
}
std::unique_ptr<FreshProfileConEpoch::Live> FreshProfileConEpoch::BindCurrent(
    Source source,
    std::shared_ptr<fs::ProfileArtifactAdmission> admission,
    std::span<const uint8_t> trusted, std::error_code& error) {
  error.clear();
  if (global_critical_region::is_held_by_current_thread() || !source.volume ||
      source.volume->error() || !source.volume->identity() ||
      !source.volume->mutations() || !source.members || !source.members->base() ||
      !source.members->base()->directory_identity() || !admission ||
      !admission->header() || !admission->members()) {
    error = Error(std::errc::invalid_argument); return {};
  }
  const auto& header = admission->header();
  const auto identity = Profile(source.package);
  const auto owner = source.volume->mutations();
  if (!identity || owner->identity_ != source.volume->identity() ||
      admission->members() != source.members ||
      admission->members().owner_before(source.members) ||
      source.members.owner_before(admission->members()) ||
      header->package() != source.package ||
      !owner->content_->directory_identity()->SameEntry(
          source.package, *source.members->base()->directory_identity(), {})) {
    error = Error(std::errc::permission_denied); return {};
  }
  error = header->CheckCurrent(*source.members);
  if (error) { return {}; }
  auto data = std::make_shared<Live::Data>();
  data->source = std::move(source); data->header = header;
  data->admission = std::move(admission);
  data->before = data->header->expected();
  if (!data->before || !data->before->exists()) {
    error = Error(std::errc::operation_not_supported); return {};
  }
  const auto bytes = data->before->bytes();
  if (bytes.size() < kAligned + 32 ||
      con::NativeHeaderAlignedSize(bytes) != kAligned) {
    error = Error(std::errc::bad_message); return {};
  }
  data->snapshot = data->header->CaptureMemberSnapshot(error);
  if (!data->snapshot) { return {}; }
  data->facts = DecodeCanonical(bytes,
      *data->source.volume->identity(), data->source.package, *identity,
      *data->snapshot, trusted, error);
  if (!data->facts) { return {}; }
  auto live = std::unique_ptr<Live>(new Live(std::move(data)));
  // A coherent baseline is issued only after BOTH external Header and every
  // complete immutable member token remain current in the same source cut.
  // Returned facts still contain no staged revision, native ACK or writer.
  const auto batch = ValidateLive(*live, error);
  return batch ? std::move(live) : nullptr;
}
}  // namespace xe::kernel::xam
