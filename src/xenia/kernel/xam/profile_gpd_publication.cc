/** Xenia : Xbox 360 Emulator Research Project; BSD license, see LICENSE. */
#include "xenia/kernel/xam/profile_gpd_publication.h"
#include "xenia/kernel/xam/profile_account_snapshot.h"
#include "xenia/kernel/xam/profile_manager.h"
#include "xenia/kernel/xam/profile_account_file.h"
#include "xenia/vfs/host_storage_errors.h"
#include <cstring>
#include "xenia/base/math.h"
#include "xenia/base/logging.h"
#include <algorithm>
#include <cstdio>
#include <utility>
#include "xenia/base/mutex.h"
#include "xenia/base/string.h"
#include "xenia/kernel/title_id_utils.h"
#include "xenia/vfs/devices/host_path_device.h"
namespace xe::kernel::xam {
namespace {
std::error_code Error(std::errc e) { return std::make_error_code(e); }
bool DecodeAccount(std::span<const uint8_t>, X_XAMACCOUNTINFO&, bool&);
X_STATUS AccountFileStatus(const std::error_code& error) {
  return error == std::errc::operation_canceled ? X_STATUS_CANCELLED
                                               : vfs::HostStorageError(error);
}

}  // namespace
std::filesystem::path ProfileStorageLease::ProfileRelative(uint64_t xuid) {
  if (!xuid) {
    return {};
  }
  char name[17];
  std::snprintf(name, sizeof(name), "%016llX",
                static_cast<unsigned long long>(xuid));
  return std::filesystem::path(name) / "FFFE07D1" / "00010000" / name;
}
ProfileStorageLease::ProfileStorageLease(
    uint64_t xuid, std::string source_id, uint32_t device_id,
    uint64_t generation, std::shared_ptr<vfs::HostPathDevice> device,
    std::shared_ptr<const filesystem::HostStorageRoot> source,
    std::shared_ptr<const filesystem::HostStorageRoot> view,
    std::shared_ptr<const filesystem::ManagedDeviceIdentity> identity,
    std::shared_ptr<const filesystem::HostStorageRoot> guest_generation)
    : xuid_(xuid),
      source_id_(std::move(source_id)),
      device_id_(device_id),
      source_generation_(generation),
      device_(std::move(device)),
      source_(std::move(source)),
      view_(std::move(view)),
      guest_generation_(std::move(guest_generation)),
      managed_identity_(std::move(identity)) {}
std::shared_ptr<ProfileStorageLease> ProfileStorageLease::Capture(
    uint64_t xuid, std::string source_id, uint32_t device_id,
    uint64_t source_generation, std::shared_ptr<vfs::HostPathDevice> device,
    std::shared_ptr<const filesystem::HostStorageRoot> source,
    std::shared_ptr<const filesystem::HostStorageRoot> view,
    std::error_code& error,
    std::shared_ptr<const filesystem::ManagedDeviceIdentity> identity,
    std::shared_ptr<const filesystem::HostStorageRoot> guest_generation) {
  error.clear();
  const bool explicit_generation = bool(guest_generation);
  if (!guest_generation) {
    guest_generation = view;
  }
  if (identity &&
      (identity->source() != source || identity->source_id() != source_id ||
       identity->device_id() != device_id || identity->retiring() ||
       identity->pending_format())) {
    error = Error(std::errc::invalid_argument);
    return {};
  }
  // No Observe/GetDevice/alias lookup or native I/O in this capture step.
  if (!xuid || source_id.empty() || !device_id || !source_generation ||
      !device || !source || !view || !guest_generation ||
      device->storage() != view || source->retiring() || view->retiring() ||
      guest_generation->retiring() ||
      (explicit_generation && guest_generation->path() != source->path()) ||
      (identity && guest_generation == source) ||
      device->is_read_only() || source->is_guest_private() ||
      view->is_guest_private() || guest_generation->is_guest_private() ||
      source->read_only() || view->read_only() || guest_generation->read_only()) {
    error = Error(std::errc::invalid_argument);
    return {};
  }
  return std::shared_ptr<ProfileStorageLease>(new ProfileStorageLease(
      xuid, std::move(source_id), device_id, source_generation,
      std::move(device), std::move(source), std::move(view),
      std::move(identity), std::move(guest_generation)));
}
std::error_code ProfileStorageLease::CheckAvailable() const {
  if (retiring()) {
    return Error(std::errc::operation_canceled);
  }
  if (managed_identity_) {
    if (auto error = managed_identity_->CheckCurrent()) {
      return error;
    }
  }
  for (const auto& root : {source_, view_, guest_generation_}) {
    const auto observed = root->Observe();
    if (observed.error) {
      return observed.error;
    }
    if (!observed.available) {
      return Error(std::errc::no_such_device);
    }
    if (observed.read_only) {
      return Error(std::errc::read_only_file_system);
    }
  }
  return retiring() ? Error(std::errc::operation_canceled) : std::error_code{};
}
}  // namespace xe::kernel::xam

namespace xe::kernel::xam {
ProfileAccountCommitGate::Use::~Use() {
  std::lock_guard lock(gate_->mutex_);
  if (!--gate_->active_) { gate_->drained_.notify_all(); }
}
std::unique_ptr<ProfileAccountCommitGate::Use> ProfileAccountCommitGate::Acquire() {
  std::lock_guard lock(mutex_);
  if (!owner_ || active_ == SIZE_MAX ||
      closed_.load(std::memory_order_relaxed)) { return {}; }
  auto use = std::unique_ptr<Use>(new Use(shared_from_this(), owner_));
  ++active_;
  return use;
}
void ProfileAccountCommitGate::CloseAndDrain() {
  std::unique_lock lock(mutex_);
  closed_.store(true, std::memory_order_release);
  owner_ = nullptr;
  // No timeout confers release: only actual completion leaves the counted gate.
  drained_.wait(lock, [this] { return active_ == 0; });
}
namespace {
class AccountSessionEntry final : public vfs::Entry {
 public:
  AccountSessionEntry(vfs::Device* device, vfs::Entry* parent,
                       std::string_view path, size_t size)
      : Entry(device, parent, path) {
    attributes_ = vfs::kFileAttributeNormal;
    SetSize(size);
  }
  void SetSize(size_t size) {
    size_ = size;
    allocation_size_ = size ? xe::round_up(size, size_t(512)) : 0;
  }
  bool allows_namespace_mutation() const override { return false; }
  X_STATUS Open(uint32_t, vfs::File**) override { return X_STATUS_ACCESS_DENIED; }
 private:
  bool DeleteEntryInternal(vfs::Entry*) override { return false; }
};
}  // namespace
class ProfileAccountManagedFile final : public vfs::File {
 public:
  ProfileAccountManagedFile(uint32_t access,
      std::unique_ptr<AccountSessionEntry> entry,
      std::shared_ptr<const filesystem::HostStorageRoot> requested_root,
      std::shared_ptr<const ProfileAccountSnapshot> snapshot,
      ProfileAccountWriter writer, bool truncate,
      std::shared_ptr<ProfileAccountCommitGate> gate)
      : File(access, entry.get()), owned_entry_(std::move(entry)),
        requested_root_(std::move(requested_root)), snapshot_(std::move(snapshot)),
        canonical_(snapshot_->identity()->storage()->device()),
        session_(std::move(writer), snapshot_, truncate), gate_(std::move(gate)) {
    device_owner_->RetainManagedFile();
    if (canonical_ != device_owner_) { canonical_->RetainManagedFile(); }
  }
  ~ProfileAccountManagedFile() override {
    // Complete authenticated writes publish before successful WriteSync/Flush.
    // Never perform blocking IO or claim success from an incomplete close.
    device_owner_->ReleaseManagedFile();
    if (canonical_ != device_owner_) { canonical_->ReleaseManagedFile(); }
  }
  void Destroy() override { delete this; }
  X_STATUS ReadSync(std::span<uint8_t> buffer, size_t offset,
                     size_t* transferred) override {
    *transferred = 0;
    if (!(file_access_ & vfs::FileAccess::kFileReadData)) {
      return X_STATUS_ACCESS_DENIED;
    }
    if (auto error = Available()) { return AccountFileStatus(error); }
    const auto bytes = session_.bytes();
    if (offset >= bytes.size()) {
      return buffer.empty() ? X_STATUS_SUCCESS : X_STATUS_END_OF_FILE;
    }
    *transferred = std::min(buffer.size(), bytes.size() - offset);
    std::copy_n(bytes.data() + offset, *transferred, buffer.data());
    return X_STATUS_SUCCESS;
  }
  X_STATUS WriteSync(std::span<const uint8_t> bytes, size_t offset,
                      size_t* transferred) override {
    *transferred = 0;
    if (!(file_access_ & (vfs::FileAccess::kFileWriteData |
                          vfs::FileAccess::kFileAppendData))) {
      return X_STATUS_ACCESS_DENIED;
    }
    if (auto error = Available()) { return AccountFileStatus(error); }
    if (auto error = session_.Write(offset, bytes)) {
      return AccountFileStatus(error);
    }
    owned_entry_->SetSize(session_.bytes().size());
    *transferred = bytes.size();
    // Partial encrypted writes may buffer successfully, but never acquire a
    // durability receipt. The complete authentic image uses the SAME Flush
    // producer before reporting a full write successful, including close-only
    // native callers. Bad/incomplete images preserve the original on disk.
    X_XAMACCOUNTINFO decoded{};
    bool devkit;
    if (DecodeAccount(session_.bytes(), decoded, devkit)) { return Flush(); }
    return session_.prefix_written()
        ? AccountFileStatus(std::make_error_code(std::errc::bad_message))
        : X_STATUS_SUCCESS;
  }
  X_STATUS SetLength(size_t length) override {
    if (!(file_access_ & vfs::FileAccess::kFileWriteData)) {
      return X_STATUS_ACCESS_DENIED;
    }
    if (auto error = Available()) { return AccountFileStatus(error); }
    const auto error = session_.Resize(length);
    if (!error) { owned_entry_->SetSize(length); }
    return AccountFileStatus(error);
  }
  X_STATUS Flush() override {
    if (!(file_access_ & (vfs::FileAccess::kFileWriteData |
                          vfs::FileAccess::kFileAppendData))) {
      return X_STATUS_ACCESS_DENIED;
    }
    auto use = gate_->Acquire();
    if (!use) { return X_STATUS_CANCELLED; }
    if (auto error = Available()) { return AccountFileStatus(error); }
    last_ = session_.Flush();
    if (!last_.durable) {
      if (last_.storage.stage_created || last_.storage.recovery_created) {
        XELOGW("Profile {:016X}: guest Account publication retained ({}, stage={}, recovery={}, published={}, durable={})",
            snapshot_->identity()->storage()->xuid(), last_.error.message(),
            last_.storage.stage.string(), last_.storage.recovery.string(),
            last_.storage.published, last_.storage.durable);
      }
      return AccountFileStatus(last_.error);
    }
    std::error_code error;
    if (!use->owner().PublishAccountCache(last_.durable, error)) {
      return AccountFileStatus(error);
    }
    snapshot_ = last_.durable;
    return X_STATUS_SUCCESS;
  }
 private:
  std::error_code Available() const {
    if (gate_->closed() || requested_root_->retiring()) {
      return std::make_error_code(std::errc::operation_canceled);
    }
    return snapshot_->CheckAvailable();
  }
  std::unique_ptr<AccountSessionEntry> owned_entry_;
  const std::shared_ptr<const filesystem::HostStorageRoot> requested_root_;
  std::shared_ptr<const ProfileAccountSnapshot> snapshot_;
  const std::shared_ptr<vfs::HostPathDevice> canonical_;
  ProfileAccountByteSession session_;
  const std::shared_ptr<ProfileAccountCommitGate> gate_;
  ProfileAccountAttempt last_;
};
X_STATUS ProfileAccountFileService::Open(
    std::shared_ptr<vfs::Device> requested_device, std::string path,
    vfs::FileDisposition disposition, uint32_t access, bool directory,
    vfs::File** out, vfs::FileAction* action) {
  if (!out || !action) { return X_STATUS_INVALID_PARAMETER; }
  *out = nullptr;
  *action = vfs::FileAction::kDoesNotExist;
  if (directory) { return X_STATUS_NOT_A_DIRECTORY; }
  // Defense in depth: this service owns the exact Account leaf, never an
  // arbitrary descendant selected by a conservative conflict-denial policy.
  if (!xe::utf8::equal_case(xe::utf8::find_name_from_guest_path(path),"Account")) {
    return X_STATUS_OBJECT_PATH_NOT_FOUND;
  }
  if (xe::global_critical_region::is_held_by_current_thread()) {
    return X_STATUS_INVALID_DEVICE_REQUEST;
  }
  auto requested = std::dynamic_pointer_cast<vfs::HostPathDevice>(requested_device);
  if (!requested || requested->storage()->retiring() || retiring()) {
    return X_STATUS_CANCELLED;
  }
  if (uint32_t(disposition) > uint32_t(vfs::FileDisposition::kOverwriteIf)) {
    return X_STATUS_INVALID_PARAMETER;
  }
  const bool truncate = disposition == vfs::FileDisposition::kSuperscede ||
      disposition == vfs::FileDisposition::kOverwrite ||
      disposition == vfs::FileDisposition::kOverwriteIf;
  const bool writable = access & (vfs::FileAccess::kFileWriteData |
                                    vfs::FileAccess::kFileAppendData);
  if ((writable && requested->is_read_only()) || (truncate && !writable)) {
    return X_STATUS_ACCESS_DENIED;
  }
  auto use = gate_->Acquire();
  if (!use) { return X_STATUS_CANCELLED; }
  std::error_code error;
  auto snapshot = use->owner().CaptureDurableAccount(xuid_, error);
  if (!snapshot || snapshot->identity()->storage()->view() != view_) {
    return AccountFileStatus(error ? error :
        std::make_error_code(std::errc::operation_canceled));
  }
  if (disposition == vfs::FileDisposition::kCreate) {
    *action = vfs::FileAction::kExists;
    return X_STATUS_OBJECT_NAME_COLLISION;
  }
  {
    // Final metadata-only open admission and real handle counts share the
    // namespace linearization with device unregistration. No IO runs here.
    auto lock = xe::global_critical_region::AcquireDirect();
    if (requested->storage()->retiring() || retiring() ||
        snapshot->identity()->retired() || snapshot->identity()->storage()->retiring()) {
      return X_STATUS_CANCELLED;
    }
    auto* root = requested->ResolvePath("");
    if (!root) { return X_STATUS_OBJECT_PATH_NOT_FOUND; }
    auto entry = std::make_unique<AccountSessionEntry>(requested.get(), root, path,
        truncate ? 0 : snapshot->token()->bytes().size());
    ProfileAccountWriter writer(snapshot->identity(), recovery_);
    *out = new ProfileAccountManagedFile(access, std::move(entry),
        requested->storage(), std::move(snapshot), std::move(writer), truncate, gate_);
  }
  *action = truncate ? (disposition == vfs::FileDisposition::kSuperscede
      ? vfs::FileAction::kSuperseded : vfs::FileAction::kOverwritten)
      : vfs::FileAction::kOpened;
  return X_STATUS_SUCCESS;
}
}  // namespace xe::kernel::xam

namespace xe::kernel::xam {
namespace {
bool AccountLeaf(const std::filesystem::path& path) {
  if (path.has_parent_path()) { return false; }
  const auto name = path.string();
  constexpr std::string_view target = "account";
  if (name.size() != target.size()) { return false; }
  for (size_t i = 0; i != name.size(); ++i) {
    const unsigned char ch = name[i];
    if ((ch >= 'A' && ch <= 'Z' ? ch + ('a' - 'A') : ch) != target[i]) {
      return false;
    }
  }
  return true;
}
bool AccountIoAllowed(std::error_code& error) {
  error.clear();
  if (xe::global_critical_region::is_held_by_current_thread()) {
    error = std::make_error_code(std::errc::operation_would_block);
  }
  return !error;
}
bool DecodeAccount(std::span<const uint8_t> bytes, X_XAMACCOUNTINFO& account,
                   bool& devkit) {
  if (bytes.size() < ProfileAccountSnapshot::kPrefixBytes ||
      bytes.size() > ProfileAccountSnapshot::kMaximumBytes) {
    return false;
  }
  if (ProfileManager::DecryptAccountFile(bytes, &account)) {
    devkit = false;
    return true;
  }
  devkit = true;
  return ProfileManager::DecryptAccountFile(bytes, &account, true);
}
}  // namespace
std::shared_ptr<ProfileAccountIdentity> ProfileAccountIdentity::Capture(
    std::shared_ptr<ProfileStorageLease> lease, uint8_t slot,
    uint64_t generation, std::error_code& error) {
  error.clear();
  // 0xFF/zero describes an unsigned-in durable Account only. It cannot be
  // substituted for any of the four authenticated native logon inputs.
  if (!lease || lease->retiring() ||
      !((slot < 4 && generation) || (slot == 0xFF && !generation))) {
    error = std::make_error_code(std::errc::invalid_argument);
    return {};
  }
  return std::shared_ptr<ProfileAccountIdentity>(
      new ProfileAccountIdentity(std::move(lease), slot, generation));
}
ProfileAccountIdentity::Write::~Write() {
  if (owner_) {
    owner_->writes_.fetch_sub(1, std::memory_order_acq_rel);
    owner_->storage_->device()->ReleaseManagedFile();
  }
}
std::unique_ptr<ProfileAccountIdentity::Write> ProfileAccountIdentity::EnterWrite(
    std::error_code& error) {
  error.clear();
  // Allocate before changing admission accounting. No namespace/model lock or
  // IO is retained by this short atomic lease operation.
  auto use = std::unique_ptr<Write>(new Write(shared_from_this()));
  auto state = writes_.load(std::memory_order_acquire);
  while (!(state & kClosed) && state < 32) {
    if (writes_.compare_exchange_weak(state, state + 1,
          std::memory_order_acq_rel)) {
      storage_->device()->RetainManagedFile();
      if (write_serial_.fetch_add(1, std::memory_order_acq_rel) == UINT64_MAX) {
        Retire();
        use.reset();
        error = std::make_error_code(std::errc::operation_canceled);
        return {};
      }
      return use;
    }
  }
  // The not-admitted temporary must not decrement a ledger it never entered.
  use->owner_.reset();
  use.reset();
  error = std::make_error_code(state & kClosed ? std::errc::operation_canceled
                                               : std::errc::device_or_resource_busy);
  return {};
}
std::error_code ProfileAccountSnapshot::CheckAvailable() const {
  if (identity_->retired()) {
    return std::make_error_code(std::errc::operation_canceled);
  }
  return identity_->storage()->CheckAvailable();
}
std::shared_ptr<const ProfileAccountSnapshot>
ProfileAccountSnapshot::FromDurableToken(
    std::shared_ptr<ProfileAccountIdentity> identity,
    std::shared_ptr<const filesystem::HostStorageSnapshotToken> token,
    std::error_code& error) {
  error.clear();
  X_XAMACCOUNTINFO account{};
  bool devkit = false;
  if (!identity || identity->retired() || identity->storage()->retiring()) {
    error = std::make_error_code(std::errc::operation_canceled);
    return {};
  }
  if (!token || token->root() != identity->storage()->view() ||
      !token->exists() || !AccountLeaf(token->relative()) ||
      !DecodeAccount(token->bytes(), account, devkit)) {
    error = std::make_error_code(std::errc::bad_message);
    return {};
  }
  return std::shared_ptr<const ProfileAccountSnapshot>(new ProfileAccountSnapshot(
      std::move(identity), std::move(token), account, devkit));
}
std::shared_ptr<const ProfileAccountSnapshot> ProfileAccountSnapshot::Load(
    std::shared_ptr<ProfileAccountIdentity> identity, std::error_code& error) {
  if (!AccountIoAllowed(error) || !identity) {
    if (!error) { error = std::make_error_code(std::errc::invalid_argument); }
    return {};
  }
  if (identity->retired() || (error = identity->storage()->CheckAvailable())) {
    if (!error) { error = std::make_error_code(std::errc::operation_canceled); }
    return {};
  }
  auto token = identity->storage()->view()->CaptureSnapshot(
      "Account", kMaximumBytes, error, true);
  if (!token) { return {}; }
  if (!token->exists()) {
    error = std::make_error_code(std::errc::no_such_file_or_directory);
    return {};
  }
  // Authenticate before any successful receipt or cache publication. Missing,
  // truncated, ambiguous and unreadable Accounts never become empty defaults.
  auto snapshot = FromDurableToken(identity, token, error);
  if (!snapshot) { return {}; }
  const auto observed = token->root()->ObserveSnapshotDurability(*token);
  if (observed.error || !observed.matched || !observed.durable) {
    error = observed.error ? observed.error
                           : std::make_error_code(std::errc::io_error);
    return {};
  }
  if ((error = snapshot->CheckAvailable())) { return {}; }
  return snapshot;
}
std::unique_ptr<PreparedProfileAccountWrite> ProfileAccountWriter::PrepareRaw(
    std::shared_ptr<const filesystem::HostStorageSnapshotToken> expected,
    std::span<const uint8_t> bytes, std::error_code& error) const {
  if (!AccountIoAllowed(error)) { return {}; }
  if (!identity_ || !recovery_ || !expected ||
      expected->root() != identity_->storage()->view() ||
      !AccountLeaf(expected->relative())) {
    error = std::make_error_code(std::errc::invalid_argument);
    return {};
  }
  if (identity_->retired() || (error = identity_->storage()->CheckAvailable())) {
    if (!error) { error = std::make_error_code(std::errc::operation_canceled); }
    return {};
  }
  auto ownership = identity_->EnterWrite(error);
  if (!ownership) { return {}; }
  X_XAMACCOUNTINFO decoded{};
  bool devkit;
  if ((expected->exists() && !DecodeAccount(expected->bytes(), decoded, devkit)) ||
      !DecodeAccount(bytes, decoded, devkit)) {
    error = std::make_error_code(std::errc::bad_message);
    return {};
  }
  auto out = std::unique_ptr<PreparedProfileAccountWrite>(new PreparedProfileAccountWrite);
  out->identity_ = identity_;
  out->ownership_ = std::move(ownership);
  // Keep even an unsuccessful real stage/backup receipt. It identifies the
  // actual retained files; cancellation never reports publication or rollback.
  out->prepared_ = filesystem::SnapshotPublisher::Prepare(
      *recovery_, expected, bytes, identity_->cancellation());
  error = out->prepared_->receipt().error;
  return out;
}
ProfileAccountAttempt ProfileAccountWriter::Commit(
    PreparedProfileAccountWrite& prepared) const {
  ProfileAccountAttempt out;
  if (!AccountIoAllowed(out.error)) { return out; }
  if (!identity_ || prepared.identity_ != identity_ || !prepared.prepared_) {
    out.error = std::make_error_code(std::errc::invalid_argument);
    return out;
  }
  out.storage = prepared.prepared_->receipt();
  if (identity_->retired() || (out.error = identity_->storage()->CheckAvailable())) {
    if (!out.error) { out.error = std::make_error_code(std::errc::operation_canceled); }
    return out;
  }
  // Common publication performs exact old-byte/identity CAS and a genuine
  // parent barrier. It can report published-but-uncertain; never normalize it.
  out.storage = filesystem::SnapshotPublisher::Commit(
      *prepared.prepared_, identity_->cancellation());
  out.error = out.storage.error;
  if (out.error || !out.storage.published || !out.storage.durable ||
      !out.storage.current) {
    if (!out.error) { out.error = std::make_error_code(std::errc::io_error); }
    return out;
  }
  out.durable = ProfileAccountSnapshot::FromDurableToken(
      identity_, out.storage.current, out.error);
  if (out.durable && (out.error = out.durable->CheckAvailable())) {
    out.durable.reset();
  }
  return out;
}
ProfileAccountAttempt ProfileAccountWriter::PublishRaw(
    std::shared_ptr<const filesystem::HostStorageSnapshotToken> expected,
    std::span<const uint8_t> bytes) const {
  ProfileAccountAttempt out;
  auto prepared = PrepareRaw(std::move(expected), bytes, out.error);
  return prepared ? Commit(*prepared) : out;
}
ProfileAccountAttempt ProfileAccountWriter::RetryDurability(
    const ProfileAccountAttempt& previous) const {
  auto out = previous;
  out.durable.reset();
  if (!AccountIoAllowed(out.error)) { return out; }
  if (!identity_ || identity_->retired() || !previous.storage.published ||
      previous.storage.durable || !previous.storage.current ||
      previous.storage.current->root() != identity_->storage()->view()) {
    out.error = std::make_error_code(std::errc::operation_not_permitted);
    return out;
  }
  auto ownership = identity_->EnterWrite(out.error);
  if (!ownership) { return out; }
  if ((out.error = identity_->storage()->CheckAvailable())) { return out; }
  const auto retry = identity_->storage()->view()->RetrySnapshotDurability(
      previous.storage.current);
  out.storage.error = out.error = retry.error;
  out.storage.durable = retry.durable;
  if (retry.current) { out.storage.current = retry.current; }
  if (out.error || !retry.durable) {
    if (!out.error) { out.error = std::make_error_code(std::errc::io_error); }
    return out;
  }
  out.durable = ProfileAccountSnapshot::FromDurableToken(
      identity_, out.storage.current, out.error);
  if (out.durable && (out.error = out.durable->CheckAvailable())) {
    out.durable.reset();
  }
  return out;
}
ProfileAccountAttempt ProfileAccountWriter::Replace(
    std::shared_ptr<const ProfileAccountSnapshot> expected,
    const X_XAMACCOUNTINFO& account) const {
  ProfileAccountAttempt out;
  if (!expected || expected->identity() != identity_) {
    out.error = std::make_error_code(std::errc::invalid_argument);
    return out;
  }
  // Keep every opaque plaintext field supplied by the caller AND every raw
  // suffix byte outside the authenticated prefix. Retain retail/devkit flavor.
  std::vector<uint8_t> bytes(expected->token()->bytes().begin(),
                              expected->token()->bytes().end());
  ProfileManager::EncryptAccountFile(&account, bytes.data(), expected->devkit());
  return PublishRaw(expected->token(), bytes);
}
ProfileAccountAttempt ProfileAccountWriter::Create(
    const X_XAMACCOUNTINFO& account) const {
  ProfileAccountAttempt out;
  if (!AccountIoAllowed(out.error) || !identity_) {
    if (!out.error) { out.error = std::make_error_code(std::errc::invalid_argument); }
    return out;
  }
  auto absent = identity_->storage()->view()->CaptureSnapshot(
      "Account", ProfileAccountSnapshot::kMaximumBytes, out.error, true);
  if (!absent) { return out; }
  if (absent->exists()) {
    out.error = std::make_error_code(std::errc::file_exists);
    return out;
  }
  std::vector<uint8_t> bytes(ProfileAccountSnapshot::kPrefixBytes);
  ProfileManager::EncryptAccountFile(&account, bytes.data());
  return PublishRaw(std::move(absent), bytes);
}
ProfileAccountByteSession::ProfileAccountByteSession(
    ProfileAccountWriter writer,
    std::shared_ptr<const ProfileAccountSnapshot> expected, bool truncate)
    : writer_(std::move(writer)), expected_(std::move(expected)) {
  if (!truncate && expected_) {
    bytes_.assign(expected_->token()->bytes().begin(), expected_->token()->bytes().end());
  }
  dirty_ = truncate;
}
std::error_code ProfileAccountByteSession::Write(
    size_t offset, std::span<const uint8_t> bytes) {
  if (!expected_) { return std::make_error_code(std::errc::invalid_argument); }
  if (auto error = expected_->CheckAvailable()) { return error; }
  if (uncertain_.storage.published) {
    return std::make_error_code(std::errc::device_or_resource_busy);
  }
  if (offset > ProfileAccountSnapshot::kMaximumBytes ||
      bytes.size() > ProfileAccountSnapshot::kMaximumBytes - offset) {
    return std::make_error_code(std::errc::file_too_large);
  }
  if (bytes.empty()) { return {}; }
  if (bytes_.size() < offset + bytes.size()) { bytes_.resize(offset + bytes.size()); }
  std::copy(bytes.begin(), bytes.end(), bytes_.begin() + offset);
  for (size_t i = offset; i < std::min(offset + bytes.size(),
                                    ProfileAccountSnapshot::kPrefixBytes); ++i) {
    prefix_written_.set(i);
  }
  dirty_ = true;
  return {};
}
std::error_code ProfileAccountByteSession::Resize(size_t size) {
  if (!expected_) { return std::make_error_code(std::errc::invalid_argument); }
  if (auto error = expected_->CheckAvailable()) { return error; }
  if (uncertain_.storage.published) {
    return std::make_error_code(std::errc::device_or_resource_busy);
  }
  if (size > ProfileAccountSnapshot::kMaximumBytes) {
    return std::make_error_code(std::errc::file_too_large);
  }
  bytes_.resize(size);
  for (size_t i = size; i < ProfileAccountSnapshot::kPrefixBytes; ++i) {
    prefix_written_.reset(i);
  }
  dirty_ = true;
  return {};
}
ProfileAccountAttempt ProfileAccountByteSession::Flush() {
  ProfileAccountAttempt out;
  if (!expected_) {
    out.error = std::make_error_code(std::errc::invalid_argument);
    return out;
  }
  if (!dirty_) {
    out.error = expected_->CheckAvailable();
    if (!out.error) { out.durable = expected_; }
    return out;
  }
  out = uncertain_.storage.published
      ? writer_.RetryDurability(uncertain_)
      : writer_.PublishRaw(expected_->token(), bytes_);
  if (out.durable) {
    expected_ = out.durable;
    dirty_ = false;
    prefix_written_.reset();
    uncertain_ = {};
  } else if (out.storage.published) {
    uncertain_ = out;
  }
  return out;
}
std::error_code NativeLogonAccountInputs::EncodeRow(
    const ProfileAccountSnapshot& snapshot, std::span<uint8_t> output) {
  static_assert(sizeof(X_XAMACCOUNTINFO) == 0x17c);
  if (output.size() != kRowBytes || snapshot.identity()->slot() >= 4 ||
      !snapshot.identity()->sign_in_generation() ||
      snapshot.identity()->retired() || snapshot.identity()->storage()->retiring()) {
    return std::make_error_code(std::errc::invalid_argument);
  }
  std::array<uint8_t, kRowBytes> row{};
  const auto storage = snapshot.identity()->storage();
  const uint64_t xuid = storage->xuid();
  for (size_t i = 0; i != 8; ++i) { row[i] = uint8_t(xuid >> ((7 - i) * 8)); }
  std::memcpy(row.data() + 8, &snapshot.account(), sizeof(X_XAMACCOUNTINFO));
  const uint32_t device = storage->device_id();
  for (size_t i = 0; i != 4; ++i) { row[0x184 + i] = uint8_t(device >> ((3 - i) * 8)); }
  std::copy(row.begin(), row.end(), output.begin());
  return {};
}
bool NativeLogonAccountInputs::OwnershipCurrent() const {
  if (!stamp_ || !publication_ || stamp_->current() != publication_) { return false; }
  for (size_t slot = 0; slot != 4; ++slot) {
    const auto& account = accounts_[slot];
    if (account && (account->identity()->retired() ||
        account->identity()->storage()->retiring() ||
        !account->identity()->writes_drained() ||
        account->identity()->write_serial() != write_serials_[slot])) { return false; }
  }
  return true;
}
}  // namespace xe::kernel::xam
