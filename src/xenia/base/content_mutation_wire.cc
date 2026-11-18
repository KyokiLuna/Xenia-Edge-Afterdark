/** Xenia : Xbox 360 Emulator Research Project; BSD license, see LICENSE. */
#include "xenia/base/content_mutation_wire.h"
#include <algorithm>
#include <array>
#include <limits>
#include <map>
#include <set>
#include "xenia/base/content_package_mutation_wire.h"
#include <string_view>
#include <utility>
#include "third_party/mbedtls/include/mbedtls/sha256.h"
#include "xenia/base/platform.h"

namespace xe::filesystem::content_mutation_wire {
namespace {
constexpr std::string_view kMagic = "XCMUT02\n", kDoneMagic = "XCMDN02\n";
constexpr uint32_t kVersion = 2;
constexpr uint64_t kPrefixBytes = 164, kDigestBytes = 32;
constexpr size_t kMaximumSource = 256, kMaximumPath = 256;
constexpr size_t kDoneBytes = 8 + 4 + 4 + 8 + 32 + 32;
std::error_code Bad() { return std::make_error_code(std::errc::bad_message); }
std::error_code Invalid() {
  return std::make_error_code(std::errc::invalid_argument);
}
std::error_code TooLarge() {
  return std::make_error_code(std::errc::value_too_large);
}
bool Equal(std::span<const uint8_t> a, std::span<const uint8_t> b) {
  return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin());
}
std::span<const uint8_t> Bytes(std::string_view text) {
  return {reinterpret_cast<const uint8_t*>(text.data()), text.size()};
}
bool Hex(std::string_view text, size_t length, bool lower_only = false) {
  return text.size() == length &&
      std::all_of(text.begin(), text.end(), [lower_only](unsigned char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
            (!lower_only && c >= 'A' && c <= 'F');
      });
}
// Do not call a locale-dependent native->narrow conversion on untrusted paths.
// Wire paths contain ASCII and one exact slash spelling. Physical case stays.
bool PathText(const std::filesystem::path& path, std::string& text) {
  const auto& native = path.native();
  if (native.size() > kMaximumPath) { return false; }
  text.clear();
  text.reserve(native.size());
  for (auto unit : native) {
    const uint32_t value = uint32_t(unit);
    if (!value || value > 0x7f) { return false; }
    char c = char(value);
#if XE_PLATFORM_WIN32
    if (c == '\\') { c = '/'; }
#endif
    text.push_back(c);
  }
  return true;
}
bool PackageParts(const std::filesystem::path& path,
                  std::array<std::string, 4>& parts) {
  std::string text;
  if (!PathText(path, text) || path.is_absolute() || path.has_root_name()) {
    return false;
  }
  size_t begin = 0;
  for (size_t i = 0; i < parts.size(); ++i) {
    const size_t end = text.find('/', begin);
    if ((i == 3) != (end == std::string::npos)) { return false; }
    parts[i] = text.substr(begin, end == std::string::npos ? end : end - begin);
    begin = end == std::string::npos ? text.size() : end + 1;
  }
  if (!Hex(parts[0], 16) || !Hex(parts[1], 8) || !Hex(parts[2], 8) ||
      parts[3].empty() || parts[3].size() > 42 ||
      parts[3] == "." || parts[3] == "..") { return false; }
  constexpr std::string_view forbidden = "\"*+,/:;<=>?\\|";
  return std::all_of(parts[3].begin(), parts[3].end(), [&](unsigned char c) {
    return c >= 0x20 && c <= 0x7e &&
        forbidden.find(char(c)) == std::string_view::npos;
  });
}
bool ValidKind(Kind kind) {
  return kind == Kind::kHeader || kind == Kind::kThumbnail ||
      kind == Kind::kCreate;
}
std::filesystem::path TargetParts(Kind kind,
                                  const std::array<std::string, 4>& p) {
  if (kind == Kind::kThumbnail) {
    return std::filesystem::path(p[0]) / p[1] / p[2] / p[3] / "__thumbnail.png";
  }
  return std::filesystem::path(p[0]) / p[1] / "Headers" / p[2] /
      (p[3] + ".header");
}
bool FoldEqual(std::string_view a, std::string_view b) {
  auto lower = [](unsigned char c) {
    return c >= 'A' && c <= 'Z' ? unsigned(c + ('a' - 'A')) : unsigned(c);
  };
  return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(),
      [&](unsigned char x, unsigned char y) { return lower(x) == lower(y); });
}
bool PhysicalTarget(const Record& record) {
  std::array<std::string, 4> parts;
  std::string target, semantic;
  return ValidKind(record.kind) && PackageParts(record.package, parts) &&
      PathText(record.target, target) &&
      PathText(TargetParts(record.kind, parts), semantic) &&
      FoldEqual(target, semantic);
}
bool ValidInitialRecord(const Record&);
bool ValidRecord(const Record& r, uint64_t before_length,
                 uint64_t intended_length) {
  std::array<std::string, 4> parts;
  if (!ValidKind(r.kind) || !PackageParts(r.package, parts) ||
      !PhysicalTarget(r) || !ValidNonce(r.nonce) || !r.identity.epoch ||
      !r.identity.content_directory || !r.identity.device || r.identity.source.empty() ||
      r.identity.source.size() > kMaximumSource ||
      r.identity.source.find('\0') != std::string::npos ||
      std::all_of(r.identity.uuid.begin(), r.identity.uuid.end(),
                  [](uint8_t v) { return !v; }) ||
      before_length > kMaximumMetadataBytes ||
      intended_length > kMaximumMetadataBytes || !r.stage_file ||
      (r.kind == Kind::kCreate) != bool(r.directory_stage)) {
    return false;
  }
  std::string stage, expected_stage, recovery;
  if (!PathText(r.stage, stage) || !PathText(r.recovery, recovery) ||
      !PathText(r.target.parent_path() /
                    (".xenia-stage-" + r.nonce + ".tmp"), expected_stage) ||
      r.target.parent_path() != r.stage.parent_path() ||
      stage != expected_stage) { return false; }
  if (r.before_exists) {
    if (!r.before_file || !r.recovery_file ||
        recovery != "snapshot-" + r.nonce + ".bin") { return false; }
  } else if (r.before_file || r.recovery_file || before_length ||
             !recovery.empty()) { return false; }
  // These are source-local inode/file IDs in one owned native namespace.
  // A newly created stage, old target, recovery copy and staged directory
  // cannot be aliases of each other or of the Content directory itself.
  const std::array<uint64_t, 5> ids{r.identity.content_directory, r.before_file,
      r.stage_file, r.recovery_file, r.directory_stage};
  for (size_t i = 0; i < ids.size(); ++i) {
    for (size_t j = 0; j < i; ++j) {
      if (ids[i] && ids[i] == ids[j]) { return false; }
    }
  }
  const bool preparation = std::any_of(r.preparation_digest.begin(), r.preparation_digest.end(),
      [](uint8_t byte) { return byte != 0; });
  if (preparation && r.kind != Kind::kCreate) { return false; }
  if (r.stream_pending && !r.streamed) { return false; }
  if (r.streamed && (r.kind != Kind::kCreate || r.before_exists || r.initial_tree.empty())) { return false; }
  return r.initial_tree.empty() || ValidInitialRecord(r);
}
void U32(std::vector<uint8_t>& bytes, uint32_t value) {
  for (int shift = 24; shift >= 0; shift -= 8) {
    bytes.push_back(uint8_t(value >> shift));
  }
}
void U64(std::vector<uint8_t>& bytes, uint64_t value) {
  for (int shift = 56; shift >= 0; shift -= 8) {
    bytes.push_back(uint8_t(value >> shift));
  }
}
void Raw(std::vector<uint8_t>& bytes, std::span<const uint8_t> value) {
  bytes.insert(bytes.end(), value.begin(), value.end());
}
bool Total(const std::array<uint64_t, 7>& lengths, uint64_t& total,
           uint64_t initial = 0, bool preparation = false) {
  total = kPrefixBytes + kDigestBytes;
  if (preparation) { total += 32; }
  if (initial || preparation) {
    if (initial > kMaximumRecordBytes - total - 8) { return false; }
    total += 8 + initial;
  }
  for (const auto length : lengths) {
    if (length > kMaximumRecordBytes - total) { return false; }
    total += length;
  }
  return total <= uint64_t(std::numeric_limits<size_t>::max());
}
struct Reader {
  std::span<const uint8_t> bytes;
  bool bad = false;
  std::span<const uint8_t> Take(size_t count) {
    if (bad || count > bytes.size()) { bad = true; return {}; }
    const auto result = bytes.first(count);
    bytes = bytes.subspan(count);
    return result;
  }
  uint64_t Integer(size_t count) {
    const auto value = Take(count);
    uint64_t result = 0;
    for (uint8_t byte : value) { result = (result << 8) | byte; }
    return result;
  }
  std::string Text(size_t count) {
    const auto value = Take(count);
    return value.empty() ? std::string{} :
        std::string(reinterpret_cast<const char*>(value.data()), value.size());
  }
};
// The same finite POSIX tree/path/identity laws as General package mutation.
// These functions serialize data only; no decoded ID grants live authority.
bool InitialPath(const std::filesystem::path& path, bool empty,
                 std::string& text) {
#if XE_PLATFORM_LINUX || XE_PLATFORM_MAC
  text = path.native();
  if (text.empty()) { return empty; }
  if (text.size() > content_package_mutation_wire::kMaximumPathBytes ||
      text.front() == '/' || text.back() == '/') { return false; }
  unsigned depth = 0;
  for (const auto& part : path) {
    if (part.empty() || part == "." || part == ".." ||
        ++depth > content_package_mutation_wire::kMaximumDepth) { return false; }
  }
  return path.lexically_normal() == path &&
      std::none_of(text.begin(), text.end(), [](unsigned char c) {
        return c < 0x20 || c == 0x7f || c == '\\';
      });
#else
  return false;
#endif
}
std::string InitialFold(std::string text) {
  for (auto& c : text) { if (c >= 'A' && c <= 'Z') { c += 'a' - 'A'; } }
  return text;
}
bool ValidInitialRecord(const Record& r) {
  if (r.kind != Kind::kCreate || r.before_exists || r.initial_tree.empty() ||
      r.initial_tree.size() > content_package_mutation_wire::kMaximumEntries ||
      !r.initial_tree.front().entry.directory ||
      r.initial_tree.front().entry.file != r.directory_stage) { return false; }
  std::set<uint64_t> ids;
  for (auto id : {r.identity.content_directory,r.before_file,r.stage_file,r.recovery_file}) {
    if (id) { ids.insert(id); }
  }
  std::map<std::string, bool> paths;
  std::set<std::string> folded;
  uint64_t path_bytes = 0, bytes = 0;
  for (size_t index = 0; index != r.initial_tree.size(); ++index) {
    const auto& initial = r.initial_tree[index]; const auto& row = initial.entry;
    std::string path;
    if (!InitialPath(row.relative,index == 0,path) ||
        (index == 0 ? !path.empty() : path.empty()) ||
        !row.file || !ids.insert(row.file).second ||
        row.write_nanoseconds >= 1000000000 ||
        (row.mode & ~(0170000u | 07777u)) ||
        (row.mode & 0170000u) != (row.directory ? 0040000u : 0100000u) ||
        path.size() > content_package_mutation_wire::kMaximumAggregatePathBytes-path_bytes ||
        !folded.insert(InitialFold(path)).second) { return false; }
    path_bytes += path.size();
    if (index) {
      std::string parent_path;
      if (!InitialPath(row.relative.parent_path(),true,parent_path)) { return false; }
      auto parent = paths.find(parent_path);
      if (parent == paths.end() || !parent->second) { return false; }
    }
    paths.emplace(path,row.directory);
    if (row.directory) {
      if (std::any_of(initial.digest.begin(),initial.digest.end(),[](uint8_t b){return b;})) { return false; }
    } else {
      const auto maximum = r.streamed ? kMaximumStreamBytes : kMaximumInitialBytes;
      if (row.bytes > maximum - bytes) { return false; }
      if (r.stream_pending && std::any_of(initial.digest.begin(), initial.digest.end(),
          [](uint8_t byte) { return byte != 0; })) { return false; }
      bytes += row.bytes;
    }
  }
  return true;
}
std::vector<uint8_t> EncodeInitial(const Record& r) {
  if (r.initial_tree.empty()) { return {}; }
  std::vector<uint8_t> out; U32(out,uint32_t(r.initial_tree.size()));
  for (const auto& initial : r.initial_tree) {
    const auto& row=initial.entry; std::string path;
    if (!InitialPath(row.relative,true,path)) { return {}; }
    U32(out,uint32_t(path.size())); U32(out,row.directory?1:0);
    U64(out,row.file); U64(out,row.bytes); U64(out,uint64_t(row.write_seconds));
    U32(out,row.write_nanoseconds); U32(out,row.mode); U64(out,0);
    Raw(out,initial.digest); Raw(out,Bytes(path));
  }
  return out;
}
bool DecodeInitial(std::span<const uint8_t> bytes, Record& r) {
  Reader in{bytes}; const auto count=in.Integer(4);
  if (!count || count > content_package_mutation_wire::kMaximumEntries ||
      count > (bytes.size()-4)/80) { return false; }
  r.initial_tree.reserve(size_t(count));
  uint64_t path_bytes=0;
  for (uint64_t i=0;i!=count;++i) {
    ContentPackageInitialEntry initial; auto& row=initial.entry;
    const auto length=in.Integer(4), flags=in.Integer(4);
    row.directory=flags==1; row.file=in.Integer(8); row.bytes=in.Integer(8);
    row.write_seconds=int64_t(in.Integer(8));
    row.write_nanoseconds=uint32_t(in.Integer(4)); row.mode=uint32_t(in.Integer(4));
    if (in.Integer(8) || flags>1 || length>content_package_mutation_wire::kMaximumPathBytes ||
        length>content_package_mutation_wire::kMaximumAggregatePathBytes-path_bytes) { return false; }
    path_bytes+=length; const auto digest=in.Take(32);
    if (in.bad) { return false; }
    std::copy(digest.begin(),digest.end(),initial.digest.begin());
    row.relative=in.Text(size_t(length));
    if (in.bad) { return false; }
    r.initial_tree.push_back(std::move(initial));
  }
  return !in.bad && in.bytes.empty() && ValidInitialRecord(r);
}
struct Parsed {
  Record record;
  std::span<const uint8_t> before, intended;
};
// Validate lengths and integrity before any metadata-sized allocation. The
// returned payloads remain spans into caller-owned bytes until Decode commits.
bool Inspect(std::span<const uint8_t> wire, Parsed& parsed,
              std::error_code& error) {
  auto fail = [&]() { error = Bad(); return false; };
  if (wire.size() < kPrefixBytes + kDigestBytes ||
      wire.size() > kMaximumRecordBytes) { return fail(); }
  const auto checksum=Digest(wire.first(wire.size()-size_t(kDigestBytes)),error);
  if (error || !Equal(checksum,wire.last(size_t(kDigestBytes)))) { return fail(); }
  Reader in{wire.first(wire.size() - size_t(kDigestBytes))};
  const auto magic=in.Take(8); const auto version=in.Integer(4);
  if (!Equal(magic, Bytes(kMagic)) || (version != kVersion && version != 3 && version != 4 && version != 5)) {
    return fail();
  }
#if !(XE_PLATFORM_LINUX || XE_PLATFORM_MAC)
  if (version>=3) { error=std::make_error_code(std::errc::operation_not_supported); return false; }
#endif
  auto& r = parsed.record;
  r.kind = Kind(uint32_t(in.Integer(4)));
  if (in.Integer(8) != wire.size()) { return fail(); }
  const auto uuid = in.Take(16);
  std::copy(uuid.begin(), uuid.end(), r.identity.uuid.begin());
  r.identity.epoch = in.Integer(8);
  r.identity.content_directory = in.Integer(8);
  r.identity.device = uint32_t(in.Integer(4));
  const uint64_t flags = in.Integer(4);
  if (flags > (version == 5 ? 7u : version == 4 ? 3u : 1u)) { return fail(); }
  r.before_exists = (flags & 1) != 0;
  r.streamed = version == 4 || (version == 5 && (flags & 4)); r.stream_pending = (flags & 2) != 0;
  r.before_file = in.Integer(8);
  r.stage_file = in.Integer(8);
  r.recovery_file = in.Integer(8);
  r.directory_stage = in.Integer(8);
  const uint64_t source = in.Integer(4), package = in.Integer(4);
  const uint64_t target = in.Integer(4), stage = in.Integer(4);
  const uint64_t recovery = in.Integer(4);
  const uint64_t before = in.Integer(8), intended = in.Integer(8);
  const uint64_t initial = version >= 3 ? in.Integer(8) : 0;
  if (version == 5) {
    const auto digest = in.Take(32);
    if (in.bad) { return fail(); }
    std::copy(digest.begin(), digest.end(), r.preparation_digest.begin());
  }
  uint64_t total = 0;
  if (in.bad || source > kMaximumSource || package > kMaximumPath ||
      target > kMaximumPath || stage > kMaximumPath || recovery > 45 ||
      before > kMaximumMetadataBytes ||
      intended > kMaximumMetadataBytes ||
      (version >= 3 && ((version != 5 && !initial) || initial > (24ull << 20))) ||
      !Total({source, package, target, stage, recovery, before, intended}, total, initial, version == 5) ||
      total != wire.size()) { return fail(); }
  r.nonce = in.Text(32);
  r.identity.source = in.Text(size_t(source));
  auto path = [&](size_t length, std::filesystem::path& output) {
    const auto text = in.Text(length);
    if (std::any_of(text.begin(), text.end(), [](unsigned char c) {
          return c < 0x20 || c > 0x7e || c == '\\';
        })) { return false; }
    output = std::filesystem::path(text);
    std::string canonical;
    return PathText(output, canonical) && canonical == text;
  };
  if (!path(size_t(package), r.package) || !path(size_t(target), r.target) ||
      !path(size_t(stage), r.stage) || !path(size_t(recovery), r.recovery)) { return fail(); }
  parsed.before = in.Take(size_t(before));
  parsed.intended = in.Take(size_t(intended));
  if (initial && !DecodeInitial(in.Take(size_t(initial)),r)) { return fail(); }
  if (in.bad || !in.bytes.empty() ||
      (version == 5 && std::none_of(r.preparation_digest.begin(),r.preparation_digest.end(),[](uint8_t b){return b != 0;})) ||
      !ValidRecord(r, before, intended)) {
    return fail();
  }
  const auto hash = Digest(wire.first(wire.size() - size_t(kDigestBytes)), error);
  if (error) { return false; }
  return Equal(wire.last(size_t(kDigestBytes)), hash) ? true : fail();
}
}  // namespace

namespace {
constexpr std::string_view kPreparationMagic = "XCMPRE2\n", kLegacyPreparationMagic = "XCMPRE1\n",
                           kPreservationMagic = "XCMKEP1\n";
bool HasDigest(const std::array<uint8_t, 32>& digest) {
  return std::any_of(digest.begin(), digest.end(), [](uint8_t byte) { return byte != 0; });
}
bool ValidIdentity(const Identity& identity) {
  return identity.epoch && identity.content_directory && identity.device &&
      !identity.source.empty() && identity.source.size() <= kMaximumSource &&
      identity.source.find('\0') == std::string::npos &&
      std::any_of(identity.uuid.begin(), identity.uuid.end(), [](uint8_t b) { return b != 0; });
}
void ControlIdentity(std::vector<uint8_t>& out, const Identity& identity,
                     std::string_view nonce) {
  Raw(out, identity.uuid); U64(out, identity.epoch); U64(out, identity.content_directory);
  U32(out, identity.device); U32(out, uint32_t(identity.source.size()));
  Raw(out, Bytes(nonce)); Raw(out, Bytes(identity.source));
}
bool ReadControlIdentity(Reader& in, Identity& identity, std::string& nonce) {
  const auto uuid = in.Take(16);
  if (in.bad) { return false; }
  std::copy(uuid.begin(), uuid.end(), identity.uuid.begin());
  identity.epoch = in.Integer(8); identity.content_directory = in.Integer(8);
  identity.device = uint32_t(in.Integer(4)); const auto source = in.Integer(4);
  if (source > kMaximumSource) { return false; }
  nonce = in.Text(32); identity.source = in.Text(size_t(source));
  return !in.bad && ValidIdentity(identity) && ValidNonce(nonce);
}
bool ControlBytes(std::span<const uint8_t> bytes, std::string_view magic,
                   Reader& in, std::error_code& error) {
  if (bytes.size() < 128 || bytes.size() > 4096) { error = Bad(); return false; }
  const auto hash = Digest(bytes.first(bytes.size()-32), error);
  if (error || !Equal(hash, bytes.last(32))) { error = Bad(); return false; }
  in = Reader{bytes.first(bytes.size()-32)};
  if (!Equal(in.Take(8), Bytes(magic))) { error = Bad(); return false; }
  return true;
}
void ControlDigest(std::vector<uint8_t>& out, std::error_code& error) {
  const auto hash = Digest(out, error);
  if (!error) { Raw(out, hash); }
}
bool ValidPreparation(const Preparation& p) {
  Record slot; slot.kind = Kind::kCreate; slot.package = p.package; slot.target = p.target;
  return ValidIdentity(p.identity) && ValidNonce(p.nonce) && PhysicalTarget(slot) &&
      p.header_bytes && p.header_bytes <= kMaximumMetadataBytes && HasDigest(p.header_digest) &&
      p.before_file != p.identity.content_directory && p.before_bytes <= kMaximumMetadataBytes &&
      (p.before_file ? HasDigest(p.before_digest) : (!p.before_bytes && !HasDigest(p.before_digest)));
}
bool ValidPreservation(const Preservation& p) {
  if (!ValidIdentity(p.identity) || !ValidNonce(p.nonce) || !p.file ||
      (p.header && (p.directory || !HasDigest(p.preparation_digest))) ||
      (!p.header && !p.directory)) { return false; }
  std::string source;
  if (!PathText(p.source, source)) { return false; }
  if (!p.header) { return source == "package-" + p.nonce + ".tmp"; }
  return p.source.filename() == ".xenia-stage-" + p.nonce + ".tmp" &&
      !p.source.is_absolute() && p.source.lexically_normal() == p.source;
}
}  // namespace
std::filesystem::path PreparationStage(const Preparation& p) {
  return p.target.parent_path() / (".xenia-stage-" + p.nonce + ".tmp");
}
std::filesystem::path PreservationDestination(const Preservation& p) {
  return "preserved-" + p.nonce + (p.header ? "-h" : "-d");
}
std::vector<uint8_t> EncodePreparation(const Preparation& p, std::error_code& error) {
  error.clear(); if (!ValidPreparation(p)) { error = Invalid(); return {}; }
  std::string package, target; PathText(p.package, package); PathText(p.target, target);
  std::vector<uint8_t> out; Raw(out, Bytes(kPreparationMagic)); ControlIdentity(out, p.identity, p.nonce);
  U32(out, uint32_t(package.size())); U32(out, uint32_t(target.size())); U64(out, p.header_bytes);
  Raw(out, p.header_digest); U64(out,p.before_file); U64(out,p.before_bytes); Raw(out,p.before_digest);
  Raw(out, Bytes(package)); Raw(out, Bytes(target)); ControlDigest(out, error); return out;
}
bool DecodePreparation(std::span<const uint8_t> bytes, Preparation& p, std::error_code& error) {
  error.clear(); Reader in{}; Preparation parsed;
  const bool legacy = bytes.size() >= 8 && Equal(bytes.first(8),Bytes(kLegacyPreparationMagic));
  if (!ControlBytes(bytes, legacy ? kLegacyPreparationMagic : kPreparationMagic, in, error) ||
      !ReadControlIdentity(in, parsed.identity, parsed.nonce)) { error = Bad(); return false; }
  const auto package = in.Integer(4), target = in.Integer(4); parsed.header_bytes = in.Integer(8);
  if (package > kMaximumPath || target > kMaximumPath) { error = Bad(); return false; }
  const auto digest = in.Take(32); if (in.bad) { error = Bad(); return false; }
  std::copy(digest.begin(), digest.end(), parsed.header_digest.begin());
  if (!legacy) {
    parsed.before_file=in.Integer(8); parsed.before_bytes=in.Integer(8);
    const auto before=in.Take(32); if (in.bad) { error=Bad(); return false; }
    std::copy(before.begin(),before.end(),parsed.before_digest.begin());
  }
  parsed.package = in.Text(size_t(package)); parsed.target = in.Text(size_t(target));
  if (in.bad || !in.bytes.empty() || !ValidPreparation(parsed)) { error = Bad(); return false; }
  p = std::move(parsed); return true;
}
std::vector<uint8_t> EncodePreservation(const Preservation& p, std::error_code& error) {
  error.clear(); if (!ValidPreservation(p)) { error = Invalid(); return {}; }
  std::string source; PathText(p.source, source);
  std::vector<uint8_t> out; Raw(out, Bytes(kPreservationMagic)); ControlIdentity(out, p.identity, p.nonce);
  U32(out, (p.header ? 1u : 0u) | (p.directory ? 2u : 0u)); U32(out, uint32_t(source.size()));
  U64(out, p.file); Raw(out, p.preparation_digest); Raw(out, Bytes(source)); ControlDigest(out, error); return out;
}
bool DecodePreservation(std::span<const uint8_t> bytes, Preservation& p, std::error_code& error) {
  error.clear(); Reader in{}; Preservation parsed;
  if (!ControlBytes(bytes, kPreservationMagic, in, error) ||
      !ReadControlIdentity(in, parsed.identity, parsed.nonce)) { error = Bad(); return false; }
  const auto flags = in.Integer(4), length = in.Integer(4);
  parsed.header = flags & 1; parsed.directory = flags & 2; parsed.file = in.Integer(8);
  if (flags > 3 || length > kMaximumPath) { error = Bad(); return false; }
  const auto digest = in.Take(32); if (in.bad) { error = Bad(); return false; }
  std::copy(digest.begin(), digest.end(), parsed.preparation_digest.begin()); parsed.source = in.Text(size_t(length));
  if (in.bad || !in.bytes.empty() || !ValidPreservation(parsed)) { error = Bad(); return false; }
  p = std::move(parsed); return true;
}
bool MatchesPreparation(const Preparation& p, std::span<const uint8_t> encoded,
                        const Record& r, std::error_code& error) {
  const auto digest = Digest(encoded, error); if (error) { return false; }
  return p.identity == r.identity && p.nonce == r.nonce && p.package == r.package &&
      p.target == r.target && r.kind == Kind::kCreate && r.before_exists == bool(p.before_file) &&
      r.before_file == p.before_file && r.before.size() == p.before_bytes &&
      (!p.before_file || Digest(r.before,error) == p.before_digest) && !error &&
      r.preparation_digest == digest && r.stage == PreparationStage(p) &&
      r.intended.size() == p.header_bytes && Digest(r.intended, error) == p.header_digest && !error;
}

std::error_code ValidateInitialMembers(std::span<const ContentPackageInitialMember> members) {
  if (members.empty() || members.size() >= content_package_mutation_wire::kMaximumEntries) { return Invalid(); }
  std::map<std::string,bool> paths{{"",true}};
  std::set<std::string> folded;
  uint64_t path_bytes=0,bytes=0;
  for (const auto& member:members) {
    std::string path;
    if (!InitialPath(member.relative,false,path) ||
        path.size()>content_package_mutation_wire::kMaximumAggregatePathBytes-path_bytes ||
        member.bytes.size()>kMaximumInitialBytes-bytes ||
        (member.directory && !member.bytes.empty())) { return Invalid(); }
    path_bytes+=path.size(); bytes+=member.bytes.size();
    std::string parent_path;
    if (!InitialPath(member.relative.parent_path(),true,parent_path)) { return Invalid(); }
    const auto parent=paths.find(parent_path);
    if (parent==paths.end() || !parent->second ||
        !folded.emplace(InitialFold(path)).second ||
        !paths.emplace(path,member.directory).second) { return Invalid(); }
  }
  return {};
}
std::error_code ValidateStreamMembers(std::span<const ContentPackageStreamMember> members) {
  if (members.empty() || members.size() >= content_package_mutation_wire::kMaximumEntries) { return Invalid(); }
  std::map<std::string, bool> paths{{"", true}};
  std::set<std::string> folded;
  uint64_t path_bytes = 0, bytes = 0;
  for (const auto& member : members) {
    std::string path, parent_path;
    if (!InitialPath(member.relative, false, path) ||
        path.size() > content_package_mutation_wire::kMaximumAggregatePathBytes - path_bytes ||
        member.size > kMaximumStreamBytes - bytes || (member.directory && member.size) ||
        !InitialPath(member.relative.parent_path(), true, parent_path)) { return Invalid(); }
    path_bytes += path.size(); bytes += member.size;
    const auto parent = paths.find(parent_path);
    if (parent == paths.end() || !parent->second ||
        !folded.emplace(InitialFold(path)).second ||
        !paths.emplace(path, member.directory).second) { return Invalid(); }
  }
  return {};
}
bool ValidPackage(const std::filesystem::path& path) {
  std::array<std::string, 4> parts;
  return PackageParts(path, parts);
}
std::filesystem::path Target(const Record& record) {
  return PhysicalTarget(record) ? record.target : std::filesystem::path{};
}
bool ValidNonce(std::string_view nonce) {
  return Hex(nonce, 32, true) &&
      std::any_of(nonce.begin(), nonce.end(), [](char c) { return c != '0'; });
}
std::array<uint8_t, 32> Digest(std::span<const uint8_t> bytes,
                                std::error_code& error) {
  error.clear();
  std::array<uint8_t, 32> hash{};
  if (bytes.size() > kMaximumRecordBytes) { error = TooLarge(); return hash; }
  if (mbedtls_sha256(bytes.data(), bytes.size(), hash.data(), 0)) {
    error = std::make_error_code(std::errc::io_error);
  }
  return hash;
}
std::vector<uint8_t> Encode(const Record& record, std::error_code& error) {
  error.clear();
  if (record.before.size() > kMaximumMetadataBytes ||
      record.intended.size() > kMaximumMetadataBytes) {
    error = TooLarge(); return {};
  }
  if (!ValidRecord(record, record.before.size(), record.intended.size())) {
    error = Invalid(); return {};
  }
  std::string package, target, stage, recovery;
  if (!PathText(record.package, package) || !PathText(record.target, target) ||
      !PathText(record.stage, stage) || !PathText(record.recovery, recovery)) {
    error = Invalid(); return {};
  }
  const auto initial=EncodeInitial(record);
  const bool preparation = std::any_of(record.preparation_digest.begin(), record.preparation_digest.end(),
      [](uint8_t byte) { return byte != 0; });
  uint64_t total = 0;
  if (!Total({record.identity.source.size(), package.size(), target.size(),
              stage.size(), recovery.size(), record.before.size(),
              record.intended.size()},
             total,initial.size(),preparation)) { error = TooLarge(); return {}; }
  std::vector<uint8_t> wire;
  wire.reserve(size_t(total));
  Raw(wire, Bytes(kMagic));
  U32(wire, preparation ? 5 : record.streamed ? 4 : initial.empty() ? kVersion : 3); U32(wire, uint32_t(record.kind)); U64(wire, total);
  Raw(wire, record.identity.uuid);
  U64(wire, record.identity.epoch); U64(wire, record.identity.content_directory);
  U32(wire, record.identity.device);
  U32(wire, (record.before_exists ? 1u : 0u) | (record.stream_pending ? 2u : 0u) | (preparation && record.streamed ? 4u : 0u));
  U64(wire, record.before_file); U64(wire, record.stage_file);
  U64(wire, record.recovery_file); U64(wire, record.directory_stage);
  U32(wire, uint32_t(record.identity.source.size()));
  U32(wire, uint32_t(package.size())); U32(wire, uint32_t(target.size()));
  U32(wire, uint32_t(stage.size()));
  U32(wire, uint32_t(recovery.size()));
  U64(wire, record.before.size()); U64(wire, record.intended.size());
  if (!initial.empty() || preparation) { U64(wire,initial.size()); }
  if (preparation) { Raw(wire, record.preparation_digest); }
  Raw(wire, Bytes(record.nonce)); Raw(wire, Bytes(record.identity.source));
  Raw(wire, Bytes(package)); Raw(wire, Bytes(target));
  Raw(wire, Bytes(stage)); Raw(wire, Bytes(recovery));
  Raw(wire, record.before); Raw(wire, record.intended); Raw(wire,initial);
  const auto hash = Digest(wire, error);
  if (error) { return {}; }
  Raw(wire, hash);
  return wire;
}
bool Decode(std::span<const uint8_t> bytes, Record& output,
             std::error_code& error) {
  error.clear();
  Parsed parsed;
  if (!Inspect(bytes, parsed, error)) { return false; }
  parsed.record.before.assign(parsed.before.begin(), parsed.before.end());
  parsed.record.intended.assign(parsed.intended.begin(), parsed.intended.end());
  output = std::move(parsed.record);
  return true;
}
std::vector<uint8_t> Done(std::span<const uint8_t> prepared,
                           std::error_code& error) {
  error.clear();
  Parsed parsed;
  if (prepared.size() >= 8 && (Equal(prepared.first(8),Bytes(kPreparationMagic)) ||
                               Equal(prepared.first(8),Bytes(kLegacyPreparationMagic)))) {
    Preparation plan;
    if (!DecodePreparation(prepared,plan,error)) { return {}; }
  } else if (!Inspect(prepared, parsed, error)) { return {}; }
  const auto hash = Digest(prepared, error);
  if (error) { return {}; }
  std::vector<uint8_t> wire;
  wire.reserve(kDoneBytes);
  Raw(wire, Bytes(kDoneMagic)); U32(wire, kVersion); U32(wire, 0);
  U64(wire, prepared.size()); Raw(wire, hash);
  const auto checksum = Digest(wire, error);
  if (error) { return {}; }
  Raw(wire, checksum);
  return wire;
}
bool CheckDone(std::span<const uint8_t> done,
                 std::span<const uint8_t> prepared, std::error_code& error) {
  error.clear();
  if (done.size() != kDoneBytes) { error = Bad(); return false; }
  const auto expected = Done(prepared, error);
  if (error) { return false; }
  if (!Equal(done, expected)) { error = Bad(); return false; }
  return true;
}
}  // namespace xe::filesystem::content_mutation_wire
