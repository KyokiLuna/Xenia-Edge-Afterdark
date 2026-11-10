/** Xenia : Xbox 360 Emulator Research Project; BSD license, see LICENSE. */
#include "xenia/kernel/xam/xdbf/gpd_durable_intent.h"
#include <algorithm>
#include <cstdio>
#include <map>
#include <random>
#include <set>
#include <string_view>
#include "third_party/mbedtls/include/mbedtls/sha256.h"
namespace xe::kernel::xam {
using filesystem::HostStorageSnapshotToken;
namespace detail {
std::error_code AdmitGpdIntentCompletion(size_t occupied, size_t unfinished) {
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
    4 * GpdSnapshotStore::kMaximumBytes + 16384;
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
                    const filesystem::ManagedDeviceIdentity& identity) {
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
    if (n > GpdSnapshotStore::kMaximumBytes) {
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
struct GpdDurableIntentRecord {
  std::array<uint8_t, 16> id{};
  GpdIntentBinding binding;
  filesystem::HostStorageRootIdentity root;
  struct File {
    GpdIntentAction action = GpdIntentAction::kReplace;
    uint32_t title = 0;
    std::filesystem::path leaf;
    filesystem::FileHandleInfo before_info;
    std::vector<uint8_t> before, intended;
  };
  std::vector<File> files;
  Digest journal_hash{};
};
namespace {
std::vector<uint8_t> Encode(const GpdDurableIntentRecord& r,
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
std::unique_ptr<GpdDurableIntentRecord> Decode(
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
  auto r = std::make_unique<GpdDurableIntentRecord>();
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
          f.before_info.filesystem_id || ValidateGpdSnapshot(f.intended)) {
        in.error = Bad();
      }
    } else {
      if (!f.before_info.file_id || ValidateGpdSnapshot(f.before) ||
          (f.action == GpdIntentAction::kDelete
               ? !f.intended.empty()
               : bool(ValidateGpdSnapshot(f.intended)))) {
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
std::vector<uint8_t> DoneBytes(const GpdDurableIntentRecord& r) {
  Writer w;
  w.Raw(
      {reinterpret_cast<const uint8_t*>(kDoneMagic.data()), kDoneMagic.size()});
  w.Raw(r.id);
  w.Raw(r.journal_hash);
  return std::move(w.bytes);
}
std::filesystem::path DoneLeaf(const GpdDurableIntentRecord& r) {
  return "done-" + Hex(r.id) + ".bin";
}
std::shared_ptr<const HostStorageSnapshotToken> Capture(
    const filesystem::ProfileIntentOwner& owner,
    const std::filesystem::path& leaf, uint64_t maximum,
    std::error_code& error) {
  auto token = owner.root()->CaptureSnapshot(leaf, maximum, error);
  if (!error && !token->exists()) {
    error = Bad();
  }
  return token;
}
}  // namespace
GpdDurableIntentStore::~GpdDurableIntentStore() = default;
std::vector<GpdIntentSummary> GpdDurableIntentStore::Scan(
    const filesystem::ProfileIntentOwner& owner, std::error_code& error) {
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
std::vector<GpdIntentSummary> GpdDurableIntentStore::ScanLocked(
    const filesystem::ProfileIntentOwner& owner, std::error_code& error) {
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
    auto record =
        token && !row.error ? Decode(token->bytes(), leaf, row.error) : nullptr;
    if (record) {
      row.binding = record->binding;
      if (record->binding.source_id != owner.managed_identity()->source_id() ||
          record->binding.device_id != owner.managed_identity()->device_id() ||
          record->binding.managed_source !=
              GpdManagedSourceIdentity{
                  owner.managed_identity()->uuid(),
                  owner.managed_identity()->content_epoch(),
                  owner.managed_identity()->directory_file_id()}) {
        row.error = Conflict();
      }
      const auto done_leaf = DoneLeaf(*record);
      std::error_code done_error;
      auto done = owner.root()->CaptureSnapshot(done_leaf, 56, done_error);
      if (done_error) {
        row.error = done_error;
      } else if (done->exists()) {
        done_seen.insert(done_leaf);
        if (!Equal(done->bytes(), DoneBytes(*record))) {
          row.error = Bad();
        } else {
          row.done_record_valid = true;
        }
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
std::unique_ptr<GpdDurableIntentStore> GpdDurableIntentStore::Prepare(
    std::shared_ptr<const filesystem::ProfileIntentOwner> owner,
    std::shared_ptr<const filesystem::SnapshotRecoveryOwner> recovery,
    GpdIntentBinding binding, std::vector<GpdIntentMutation> mutations,
    std::error_code &error, std::filesystem::path &attempted_journal,
    std::span<const std::shared_ptr<const filesystem::HostStorageSnapshotToken>>
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
          filesystem::HostStorageRoot::kMaximumSnapshotBatchTokens) {
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
  error = detail::AdmitGpdIntentCompletion(occupied.size(), unfinished);
  if (error) {
    return {};
  }
  auto result =
      std::unique_ptr<GpdDurableIntentStore>(new GpdDurableIntentStore);
  result->owner_ = owner;
  result->recovery_ = recovery;
  result->view_ = view;
  result->target_lease_ = std::move(target_lease);
  result->record_ = std::make_unique<GpdDurableIntentRecord>();
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
      error = ValidateGpdSnapshot(m.intended);
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
                   bool(ValidateGpdSnapshot(l.token->bytes())))) {
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
  std::vector<std::shared_ptr<const filesystem::HostStorageSnapshotToken>>
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
std::unique_ptr<GpdDurableIntentStore> GpdDurableIntentStore::Recover(
    std::shared_ptr<const filesystem::ProfileIntentOwner> owner,
    std::shared_ptr<const filesystem::SnapshotRecoveryOwner> recovery,
    const std::filesystem::path& journal, GpdIntentBinding binding,
    std::shared_ptr<const filesystem::HostStorageRoot> view,
    std::error_code& error) {
  error.clear();
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
      std::unique_ptr<GpdDurableIntentStore>(new GpdDurableIntentStore);
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
GpdIntentAttempt GpdDurableIntentStore::Advance() {
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
  std::vector<GpdSnapshotLoad> current(record_->files.size());
  // Preflight both members before any target mutation, avoiding a known
  // second-file conflict needlessly publishing the first member.
  for (size_t i = 0; i != record_->files.size(); ++i) {
    const auto& f = record_->files[i];
    auto& out = attempt.files[i];
    current[i] = GpdSnapshotStore::Load(
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
        auto deleted = GpdSnapshotStore::Delete(*recovery_, current[i]);
        out.publication = std::move(deleted.storage);
      } else {
        auto written = GpdSnapshotStore::Publish(*recovery_, current[i],
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
}  // namespace xe::kernel::xam
