/** Xenia : Xbox 360 Emulator Research Project; BSD license, see LICENSE. */
#include "xenia/base/content_package_mutation_wire.h"
#include <algorithm>
#include <array>
#include <bit>
#include <limits>
#include <map>
#include <set>
#include <string_view>
#include <utility>
#include "xenia/base/platform.h"

namespace xe::filesystem::content_package_mutation_wire {
namespace {
namespace metadata = content_mutation_wire;
constexpr std::string_view kMagic = "XCPKG03\n";
constexpr std::string_view kRetireMagic = "XCPRT03\n", kDoneMagic = "XCPDN03\n";
constexpr uint32_t kVersion = 3;
constexpr uint64_t kPrefix = 168, kEntryPrefix = 48, kChecksum = 32;
constexpr size_t kRetireBytes = 88, kDoneBytes = 128, kShortPath = 256;
constexpr uint32_t kTypeMask = 0170000, kDirectory = 0040000, kRegular = 0100000;
#if XE_PLATFORM_LINUX || XE_PLATFORM_MAC
constexpr bool kSupported = true;
#else
constexpr bool kSupported = false;
#endif
std::error_code Bad() { return std::make_error_code(std::errc::bad_message); }
std::error_code Invalid() { return std::make_error_code(std::errc::invalid_argument); }
std::error_code Large() { return std::make_error_code(std::errc::value_too_large); }
std::error_code Unsupported() { return std::make_error_code(std::errc::operation_not_supported); }
std::span<const uint8_t> Bytes(std::string_view value) {
  return {reinterpret_cast<const uint8_t*>(value.data()), value.size()};
}
bool Equal(std::span<const uint8_t> a, std::span<const uint8_t> b) {
  return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin());
}
bool Text(const std::filesystem::path& path, std::string& out,
          size_t maximum = kMaximumPathBytes) {
#if XE_PLATFORM_LINUX || XE_PLATFORM_MAC
  if (path.native().size() > maximum) { return false; }
  out = path.native();  // Exact POSIX bytes, without locale conversion.
  return true;
#else
  static_cast<void>(path); static_cast<void>(out); static_cast<void>(maximum);
  return false;
#endif
}
std::string Fold(std::string_view text) {
  std::string out(text);
  for (char& c : out) { if (c >= 'A' && c <= 'Z') { c += 'a' - 'A'; } }
  return out;
}
bool Relative(std::string_view path, bool empty) {
  if (path.empty()) { return empty; }
  if (path.size() > kMaximumPathBytes || path.front() == '/' || path.back() == '/') { return false; }
  if (std::any_of(path.begin(), path.end(), [](unsigned char c) {
        return c < 0x20 || c == 0x7f || c == '\\';
      })) { return false; }
  uint32_t depth = 0;
  for (size_t begin = 0; begin < path.size();) {
    const size_t slash = path.find('/', begin);
    const auto component = path.substr(begin, slash == path.npos ? path.npos : slash - begin);
    if (component.empty() || component == "." || component == ".." || ++depth > kMaximumDepth) { return false; }
    if (slash == path.npos) { break; }
    begin = slash + 1;
  }
  return true;
}
bool Add(uint64_t& total, uint64_t size) {
  if (size > kMaximumRecordBytes - total) { return false; }
  total += size; return true;
}
bool Size(uint64_t source, uint64_t package, uint64_t target, uint64_t preserve,
          uint64_t header, uint64_t tree, uint64_t& total) {
  total = kPrefix + kChecksum;
  for (auto length : {source, package, target, preserve, header, tree}) {
    if (!Add(total, length)) { return false; }
  }
  return total <= uint64_t(std::numeric_limits<size_t>::max());
}
bool Valid(const Record& r, uint64_t header_bytes, uint64_t& path_bytes) {
  if ((r.kind != Kind::kDelete && r.kind != Kind::kTruncate) ||
      !metadata::ValidNonce(r.nonce) || !metadata::ValidPackage(r.package) ||
      !r.identity.epoch || !r.identity.content_directory || !r.identity.device ||
      r.identity.source.empty() || r.identity.source.size() > 256 ||
      r.identity.source.find('\0') != std::string::npos ||
      std::all_of(r.identity.uuid.begin(), r.identity.uuid.end(), [](uint8_t b) { return !b; }) ||
      !r.op_directory || r.before.empty() || r.before.size() > kMaximumEntries ||
      header_bytes > metadata::kMaximumMetadataBytes ||
      (r.header_exists ? !r.header_file : bool(r.header_file || header_bytes))) { return false; }
  metadata::Record slot;
  slot.kind = metadata::Kind::kHeader; slot.package = r.package; slot.target = r.header_target;
  if (metadata::Target(slot).empty()) { return false; }
  std::string package, target, preserve;
  if (!Text(r.package, package, kShortPath) || !Text(r.header_target, target, kShortPath) ||
      !Text(r.preserved_leaf, preserve, 15) ||
      (!preserve.empty() && Fold(preserve) != "__thumbnail.png")) { return false; }
  std::set<uint64_t> ids;
  for (auto id : {r.identity.content_directory, r.op_directory, r.header_file, r.replacement_directory}) {
    if (id && !ids.insert(id).second) { return false; }
  }
  std::map<std::string, bool> paths;
  std::set<std::string> folded;
  path_bytes = 0;
  for (size_t index = 0; index < r.before.size(); ++index) {
    const auto& entry = r.before[index];
    std::string path;
    if (!Text(entry.relative, path) || !Relative(path, index == 0) ||
        (index == 0 && !path.empty()) || (index && path.empty()) ||
        !entry.file || !ids.insert(entry.file).second || entry.write_nanoseconds >= 1000000000 ||
        (entry.mode & ~(kTypeMask | 07777u)) ||
        (entry.mode & kTypeMask) != (entry.directory ? kDirectory : kRegular) ||
        path.size() > kMaximumAggregatePathBytes - path_bytes ||
        !folded.insert(Fold(path)).second) { return false; }
    path_bytes += path.size();
    if (index) {
      const auto slash = path.rfind('/');
      const std::string parent = slash == path.npos ? std::string{} : path.substr(0, slash);
      const auto found = paths.find(parent);
      if (found == paths.end() || !found->second) { return false; }
    }
    if (!paths.emplace(path, entry.directory).second) { return false; }
  }
  const bool directory = r.before.front().directory;
  if (r.kind == Kind::kDelete) {
    return preserve.empty() && !r.replacement_directory;
  }
  if (!directory) { return preserve.empty() && r.replacement_directory && r.before.size() == 1; }
  if (r.replacement_directory) { return false; }
  if (!preserve.empty()) {
    const auto found = paths.find(preserve);
    if (found == paths.end() || found->second) { return false; }
  }
  return true;
}
void U32(std::vector<uint8_t>& out, uint32_t value) {
  for (int shift = 24; shift >= 0; shift -= 8) { out.push_back(uint8_t(value >> shift)); }
}
void U64(std::vector<uint8_t>& out, uint64_t value) {
  for (int shift = 56; shift >= 0; shift -= 8) { out.push_back(uint8_t(value >> shift)); }
}
void Raw(std::vector<uint8_t>& out, std::span<const uint8_t> bytes) {
  out.insert(out.end(), bytes.begin(), bytes.end());
}
struct Reader {
  std::span<const uint8_t> bytes;
  bool bad = false;
  std::span<const uint8_t> Take(size_t count) {
    if (bad || count > bytes.size()) { bad = true; return {}; }
    auto value = bytes.first(count); bytes = bytes.subspan(count); return value;
  }
  uint64_t Integer(size_t count) {
    uint64_t value = 0;
    for (auto byte : Take(count)) { value = (value << 8) | byte; }
    return value;
  }
  std::string Text(size_t count) {
    const auto value = Take(count);
    return value.empty() ? std::string{} : std::string(reinterpret_cast<const char*>(value.data()), value.size());
  }
};
struct Parsed { Record record; std::span<const uint8_t> header; };
bool Inspect(std::span<const uint8_t> bytes, Parsed& parsed, std::error_code& error) {
  auto fail = [&] { error = Bad(); return false; };
  if (!kSupported) { error = Unsupported(); return false; }
  if (bytes.size() < kPrefix + kChecksum || bytes.size() > kMaximumRecordBytes) { return fail(); }
  // Verify the complete bounded envelope before allocating entry/path arrays.
  const auto hash = metadata::Digest(bytes.first(bytes.size() - kChecksum), error);
  if (error) { return false; }
  if (!Equal(hash, bytes.last(kChecksum))) { return fail(); }
  Reader in{bytes.first(bytes.size() - kChecksum)};
  if (!Equal(in.Take(8), Bytes(kMagic)) || in.Integer(4) != kVersion) { return fail(); }
  auto& r = parsed.record;
  r.kind = Kind(uint32_t(in.Integer(4)));
  if (in.Integer(8) != bytes.size()) { return fail(); }
  const auto uuid = in.Take(16);
  std::copy(uuid.begin(), uuid.end(), r.identity.uuid.begin());
  r.identity.epoch = in.Integer(8); r.identity.content_directory = in.Integer(8);
  r.identity.device = uint32_t(in.Integer(4));
  const auto flags = in.Integer(4);
  if (flags > 1) { return fail(); }
  r.header_exists = flags != 0;
  r.op_directory = in.Integer(8); r.header_file = in.Integer(8);
  r.replacement_directory = in.Integer(8);
  const auto source = in.Integer(4), package = in.Integer(4), target = in.Integer(4), preserve = in.Integer(4);
  const auto entries = in.Integer(4), reserved = in.Integer(4), header = in.Integer(8);
  const auto paths = in.Integer(8), tree = in.Integer(8);
  uint64_t total = 0;
  if (in.bad || reserved || !source || source > 256 || !package || package > kShortPath ||
      !target || target > kShortPath || preserve > 15 || !entries || entries > kMaximumEntries ||
      header > metadata::kMaximumMetadataBytes || paths > kMaximumAggregatePathBytes ||
      tree != entries * kEntryPrefix + paths ||
      !Size(source, package, target, preserve, header, tree, total) || total != bytes.size()) { return fail(); }
  r.nonce = in.Text(32); r.identity.source = in.Text(size_t(source));
  r.package = std::filesystem::path(in.Text(size_t(package)));
  r.header_target = std::filesystem::path(in.Text(size_t(target)));
  r.preserved_leaf = std::filesystem::path(in.Text(size_t(preserve)));
  parsed.header = in.Take(size_t(header));
  r.before.reserve(size_t(entries));
  uint64_t path_total = 0;
  for (uint64_t index = 0; index < entries; ++index) {
    const auto length = in.Integer(4), entry_flags = in.Integer(4);
    ContentPackageTreeEntry entry;
    entry.file = in.Integer(8); entry.bytes = in.Integer(8);
    entry.write_seconds = std::bit_cast<int64_t>(in.Integer(8));
    entry.write_nanoseconds = uint32_t(in.Integer(4)); entry.mode = uint32_t(in.Integer(4));
    const auto entry_reserved = in.Integer(8);
    if (in.bad || length > kMaximumPathBytes || entry_flags > 1 || entry_reserved ||
        length > paths - path_total) { return fail(); }
    path_total += length;
    entry.directory = entry_flags != 0;
    entry.relative = std::filesystem::path(in.Text(size_t(length)));
    if (in.bad) { return fail(); }
    r.before.push_back(std::move(entry));
  }
  uint64_t actual_paths = 0;
  if (in.bad || !in.bytes.empty() || path_total != paths || !Valid(r, header, actual_paths) ||
      actual_paths != paths) { return fail(); }
  return true;
}
std::vector<uint8_t> Marker(std::string_view magic, Kind kind,
                            std::span<const uint8_t> prepared,
                            std::span<const uint8_t> retire, std::error_code& error) {
  const auto prepared_hash = metadata::Digest(prepared, error);
  if (error) { return {}; }
  std::vector<uint8_t> out;
  out.reserve(retire.empty() ? kRetireBytes : kDoneBytes);
  Raw(out, Bytes(magic)); U32(out, kVersion); U32(out, uint32_t(kind));
  U64(out, prepared.size()); Raw(out, prepared_hash);
  if (!retire.empty()) {
    const auto retire_hash = metadata::Digest(retire, error);
    if (error) { return {}; }
    U64(out, retire.size()); Raw(out, retire_hash);
  }
  const auto hash = metadata::Digest(out, error);
  if (error) { return {}; }
  Raw(out, hash); return out;
}
}  // namespace

std::vector<uint8_t> Encode(const Record& r, std::error_code& error) {
  error.clear();
  if (!kSupported) { error = Unsupported(); return {}; }
  if (r.header.size() > metadata::kMaximumMetadataBytes || r.before.size() > kMaximumEntries) {
    error = Large(); return {};
  }
  uint64_t paths = 0;
  if (!Valid(r, r.header.size(), paths)) { error = Invalid(); return {}; }
  std::string package, target, preserve;
  if (!Text(r.package, package, kShortPath) || !Text(r.header_target, target, kShortPath) ||
      !Text(r.preserved_leaf, preserve, 15)) { error = Invalid(); return {}; }
  const uint64_t tree = uint64_t(r.before.size()) * kEntryPrefix + paths;
  uint64_t total = 0;
  if (!Size(r.identity.source.size(), package.size(), target.size(), preserve.size(),
            r.header.size(), tree, total)) { error = Large(); return {}; }
  std::vector<uint8_t> out; out.reserve(size_t(total));
  Raw(out, Bytes(kMagic)); U32(out, kVersion); U32(out, uint32_t(r.kind)); U64(out, total);
  Raw(out, r.identity.uuid); U64(out, r.identity.epoch); U64(out, r.identity.content_directory);
  U32(out, r.identity.device); U32(out, r.header_exists ? 1 : 0);
  U64(out, r.op_directory); U64(out, r.header_file); U64(out, r.replacement_directory);
  U32(out, uint32_t(r.identity.source.size())); U32(out, uint32_t(package.size()));
  U32(out, uint32_t(target.size())); U32(out, uint32_t(preserve.size()));
  U32(out, uint32_t(r.before.size())); U32(out, 0); U64(out, r.header.size()); U64(out, paths); U64(out, tree);
  Raw(out, Bytes(r.nonce)); Raw(out, Bytes(r.identity.source));
  Raw(out, Bytes(package)); Raw(out, Bytes(target)); Raw(out, Bytes(preserve)); Raw(out, r.header);
  for (const auto& entry : r.before) {
    std::string path; Text(entry.relative, path);
    U32(out, uint32_t(path.size())); U32(out, entry.directory ? 1 : 0);
    U64(out, entry.file); U64(out, entry.bytes); U64(out, std::bit_cast<uint64_t>(entry.write_seconds));
    U32(out, entry.write_nanoseconds); U32(out, entry.mode); U64(out, 0); Raw(out, Bytes(path));
  }
  const auto hash = metadata::Digest(out, error);
  if (error) { return {}; }
  Raw(out, hash); return out;
}
bool Decode(std::span<const uint8_t> bytes, Record& output, std::error_code& error) {
  error.clear(); Parsed parsed;
  if (!Inspect(bytes, parsed, error)) { return false; }
  parsed.record.header.assign(parsed.header.begin(), parsed.header.end());
  output = std::move(parsed.record); return true;
}
std::vector<uint8_t> Retire(std::span<const uint8_t> prepared, std::error_code& error) {
  error.clear(); Parsed parsed;
  if (!Inspect(prepared, parsed, error)) { return {}; }
  return Marker(kRetireMagic, parsed.record.kind, prepared, {}, error);
}
bool CheckRetire(std::span<const uint8_t> retire, std::span<const uint8_t> prepared,
                 std::error_code& error) {
  error.clear();
  if (retire.size() != kRetireBytes) { error = Bad(); return false; }
  const auto expected = Retire(prepared, error);
  if (error) { return false; }
  if (!Equal(retire, expected)) { error = Bad(); return false; }
  return true;
}
std::vector<uint8_t> Done(std::span<const uint8_t> prepared, std::span<const uint8_t> retire,
                         std::error_code& error) {
  error.clear(); Parsed parsed;
  if (retire.size() != kRetireBytes) { error = Bad(); return {}; }
  if (!Inspect(prepared, parsed, error)) { return {}; }
  const auto expected = Marker(kRetireMagic, parsed.record.kind, prepared, {}, error);
  if (error) { return {}; }
  if (!Equal(retire, expected)) { error = Bad(); return {}; }
  return Marker(kDoneMagic, parsed.record.kind, prepared, retire, error);
}
bool CheckDone(std::span<const uint8_t> done, std::span<const uint8_t> prepared,
               std::span<const uint8_t> retire, std::error_code& error) {
  error.clear();
  if (done.size() != kDoneBytes) { error = Bad(); return false; }
  const auto expected = Done(prepared, retire, error);
  if (error) { return false; }
  if (!Equal(done, expected)) { error = Bad(); return false; }
  return true;
}
}  // namespace xe::filesystem::content_package_mutation_wire
