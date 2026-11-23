/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2024 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <algorithm>
#include <regex>
#include <new>
#include "xenia/kernel/xam/profile_manager.h"
#include "xenia/kernel/xam/local_profile_initial_recipe.h"
#include "xenia/kernel/xam/content_source.h"
#include "xenia/kernel/util/guest_io_buffer.h"
#include <array>
#include <map>
#include "xenia/base/filesystem.h"
#include "xenia/base/host_storage.h"
#include "xenia/base/logging.h"
#include "xenia/base/utf8.h"
#include "xenia/config.h"
#include "xenia/emulator.h"
#include "xenia/hid/input_system.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/util/crypto_utils.h"
#include "xenia/kernel/xam/xam_state.h"
#include "xenia/kernel/xam/xam_content_device.h"
#include "xenia/kernel/xboxkrnl/xecrypt_sha.h"
#include "xenia/vfs/devices/host_path_device.h"
#include <set>
#include "xenia/base/xdbf_view.h"

DEFINE_string(logged_profile_slot_0_xuid, "",
              "XUID of the profile to load on boot in slot 0", "Profiles");
DEFINE_string(logged_profile_slot_1_xuid, "",
              "XUID of the profile to load on boot in slot 1", "Profiles");
DEFINE_string(logged_profile_slot_2_xuid, "",
              "XUID of the profile to load on boot in slot 2", "Profiles");
DEFINE_string(logged_profile_slot_3_xuid, "",
              "XUID of the profile to load on boot in slot 3", "Profiles");

DEFINE_string(logged_profile_slot_0_source_id, "", "Remembered storage source for profile slot 0", "Profiles");
DEFINE_string(logged_profile_slot_1_source_id, "", "Remembered storage source for profile slot 1", "Profiles");
DEFINE_string(logged_profile_slot_2_source_id, "", "Remembered storage source for profile slot 2", "Profiles");
DEFINE_string(logged_profile_slot_3_source_id, "", "Remembered storage source for profile slot 3", "Profiles");

namespace xe {
namespace kernel {
namespace xam {

struct ProfileSettingsReadLifetime {
  std::atomic<bool> open{true};
};
struct ProfileSettingsReadUse {
  ProfileSettingsReadUse(std::shared_ptr<ProfileSettingsReadLifetime> life,
                         std::shared_ptr<ProfileAccountCommitGate> publication)
      : lifetime(std::move(life)), gate(std::move(publication)) {}
  bool Current() const { return lifetime && lifetime->open.load(std::memory_order_acquire); }
  const std::shared_ptr<ProfileSettingsReadLifetime> lifetime;
  const std::shared_ptr<ProfileAccountCommitGate> gate;
};
// Native owners belong to the existing mount's read node. Escaped batches and
// User values keep only a weak reference, so they cannot retain an old source
// flock through runtime restart. Access is protected by the counted PM gate.
struct ProfileSettingsReadNativeOwners {
  std::shared_ptr<ProfileStorageLease> storage;
  std::shared_ptr<const ProfileAccountSnapshot> account;
  std::vector<std::shared_ptr<const filesystem::HostStorageSnapshotToken>> tokens;
  void Revoke() {
    tokens.clear(); account.reset(); storage.reset();
  }
};
// Issued host edit state. The PM registry retains cleanup and uncertain actual
// publication after a UI drops its handle; it owns no PM or long-lived gate Use.
struct HostProfileAccountEdit::State final {
  std::mutex mutex;
  std::shared_ptr<ProfileAccountCommitGate> gate;
  std::weak_ptr<const HostProfileAccountEdit> handle;
  uint64_t serial = 0, xuid = 0;
  ProfileAccountFieldEdits edits;
  std::string mount_path;
  uint64_t mount_generation = 0, source_generation = 0;
  uint32_t device_id = 0;
  bool mounted = false;
  std::shared_ptr<vfs::HostPathDevice> device;
  std::shared_ptr<const filesystem::HostStorageRoot> source, view, guest_generation;
  std::shared_ptr<const filesystem::ManagedDeviceIdentity> managed_identity;
  std::shared_ptr<const ProfileAccountSnapshot> expected;
  std::shared_ptr<const filesystem::SnapshotRecoveryOwner> recovery;
  X_XAMACCOUNTINFO candidate{};
  ProfileAccountAttempt attempt;
  ProfileAccountEditResult outcome;
  static ProfileAccountEditResult Busy() {
    ProfileAccountEditResult result;
    result.error = std::make_error_code(std::errc::device_or_resource_busy);
    return result;  // Explicitly no observed outcome; not a failed/no-write claim.
  }
  bool Terminal() const {
    return outcome.phase == ProfileAccountEditPhase::kCommitted ||
           outcome.phase == ProfileAccountEditPhase::kCanceled ||
           outcome.phase == ProfileAccountEditPhase::kRetired;
  }
  void ObserveAttempt() {
    outcome.outcome_known = true;
    outcome.published = attempt.storage.published;
    outcome.durable = attempt.storage.durable;
    outcome.stage = attempt.storage.stage;
    outcome.recovery = attempt.storage.recovery;
    outcome.error = attempt.error;
  }
  void RememberUnpublishedArtifacts() {
    const auto& storage = attempt.storage;
    if (storage.stage_created || storage.recovery_created) {
      outcome.prior_artifacts.push_back({storage.stage, storage.recovery,
          attempt.error, storage.stage_created, storage.recovery_created,
          storage.recovery_durable});
    }
  }
  void DropIoResources() {
    // Only after an actual settled release/owner stop, outside PM/GCRC locks.
    attempt = {};
    expected.reset(); recovery.reset();
    device.reset(); source.reset(); view.reset(); guest_generation.reset();
    managed_identity.reset();
    std::memset(&candidate, 0, sizeof(candidate));
  }
};
HostProfileAccountEdit::~HostProfileAccountEdit() {
  // A busy cancellation remains owned by the issued registry. The destructor
  // cannot abandon entered IO or callback into a destroyed manager.
  try { Cancel(); } catch (...) {}
}
ProfileAccountEditResult HostProfileAccountEdit::result() const {
  if (xe::global_critical_region::is_held_by_current_thread()) {
    return State::Busy();
  }
  std::unique_lock lock(state_->mutex, std::try_to_lock);
  return lock.owns_lock() ? state_->outcome : State::Busy();
}
ProfileAccountEditResult HostProfileAccountEdit::Commit() {
  if (xe::global_critical_region::is_held_by_current_thread()) {
    return State::Busy();
  }
  auto state = state_;
  auto use = state->gate ? state->gate->Acquire() : nullptr;
  if (!use) {
    auto observed = result();
    observed.error = std::make_error_code(std::errc::operation_canceled);
    return observed;
  }
  return use->owner().CommitAccountEdit(state);
}
ProfileAccountEditResult HostProfileAccountEdit::Cancel() {
  if (xe::global_critical_region::is_held_by_current_thread()) {
    return State::Busy();
  }
  auto state = state_;
  auto use = state->gate ? state->gate->Acquire() : nullptr;
  if (!use) {
    auto observed = result();
    observed.error = std::make_error_code(std::errc::operation_canceled);
    return observed;
  }
  return use->owner().CancelAccountEdit(state);
}

ProfileSettingsReadProvider::ProfileSettingsReadProvider(
    const ProfileManager* owner, std::shared_ptr<ProfileSettingsReadLifetime> life)
    : owner_(owner), lifetime_(std::move(life)) {}
bool ProfileSettingsReadProvider::Current(const ProfileManager& owner) const {
  return owner_ == &owner && owner.settings_read_provider_.get() == this &&
      lifetime_ && lifetime_->open.load(std::memory_order_acquire);
}
std::shared_ptr<const ProfileSettingsReadProvider>
ProfileManager::CaptureProfileSettingsReadProvider() const {
  return settings_read_provider_ && settings_read_provider_->Current(*this)
      ? settings_read_provider_ : nullptr;
}
bool ProfileSettingsReadBatch::IdentityCurrentAdmitted() const {
  if (!use_ || !use_->Current()) { return false; }
  for (const auto& user : users_) {
    const auto native = user.native_.lock();
    if (!native || !native->storage || native->storage->retiring() ||
        !native->account || native->account->identity()->retired() ||
        native->account->identity()->storage() != native->storage) { return false; }
    const auto& identity = native->account->identity();
    if (identity->slot() < 4 &&
        (!profile_owner_ || profile_owner_->CaptureRosterIdentity(native->storage->xuid()) != identity)) { return false; }
  }
  return true;
}
bool ProfileSettingsReadBatch::IdentityCurrent() const {
  if (xe::global_critical_region::is_held_by_current_thread()) { return false; }
  auto entered = publication_gate_ ? publication_gate_->Acquire() : nullptr;
  return entered && IdentityCurrentAdmitted();
}
X_STATUS ProfileSettingsReadBatch::Publish(
    Memory& memory, std::span<const util::GuestIoWrite> writes) const {
  if (xe::global_critical_region::is_held_by_current_thread()) {
    return X_STATUS_INVALID_DEVICE_REQUEST;
  }
  // The actual counted gate joins entered operations before PM destruction.
  // Native owner access and this exact Memory comparison occur only inside it.
  auto entered = publication_gate_ ? publication_gate_->Acquire() : nullptr;
  if (!entered || &memory != entered->owner().kernel_state_->memory() ||
      !IdentityCurrentAdmitted()) { return X_STATUS_CANCELLED; }
  util::GuestIoProfileReadScope admission(*this, entered->owner().profile_mount_mutex_);
  return util::WriteGuestIoBuffers(memory, writes, admission);
}
std::optional<UserSetting> ProfileSettingsReadBatch::User::Setting(
    uint32_t title, uint32_t id, std::error_code& error) const {
  error.clear();
  if (xe::global_critical_region::is_held_by_current_thread()) {
    error = std::make_error_code(std::errc::operation_would_block); return {};
  }
  const auto use = use_.lock();
  auto entered = use ? use->gate->Acquire() : nullptr;
  const auto native = entered ? native_.lock() : nullptr;
  if (!entered || !use->Current() || !native || !native->storage ||
      native->storage->retiring() || !native->account ||
      native->account->identity()->retired()) {
    error = std::make_error_code(std::errc::operation_canceled); return {};
  }
  std::optional<UserSetting> value;
  if (title_ && title == title_id_) {
    if (UserSetting::ReadFromGpd(*title_, id, value)) {
      error = std::make_error_code(std::errc::bad_message); return {};
    }
    if (value) { return value; }
  }
  if (dashboard_ && UserSetting::ReadFromGpd(*dashboard_, id, value)) {
    error = std::make_error_code(std::errc::bad_message); return {};
  }
  return value;  // Actual absence, never GetDefaultSetting or an empty GPD.
}

ProfileManager::~ProfileManager() {
  BeginRosterStop();
  if (settings_read_lifetime_) {
    settings_read_lifetime_->open.store(false, std::memory_order_release);
  }
  account_commit_gate_->CloseAndDrain();
  CloseRosterAuthority();
  CloseAccountEdits();
  // All observing/copy/publish operations have drained. Release native owners
  // from the SAME existing mount nodes outside all metadata/GCRC locks; closed
  // retained rows keep immutable GPD bytes but cannot hold the source lease.
  for (const auto& [key, mount] : profile_mounts_) {
    for (auto node = mount.read_head; node; node = node->next) {
      if (node->native) { node->native->Revoke(); }
    }
  }
  // No admitted creation operation can still own a completion record here.
  for (const auto& [address, result] : profile_creation_results_) {
    kernel_state_->memory()->SystemHeapFree(address);
  }
  account_publication_->Retire();
  // Actual owning runtime teardown invalidates every externally retained model
  // origin before manager/slot storage disappears. These are atomic stores.
  for (const auto& [xuid, identity] : account_identities_) {
    identity->Retire();
  }
  for (const auto& identity : retired_account_identities_) { identity->Retire(); }
  for (const auto& [xuid, owner] : gpd_models_) {
    owner->Retire();
  }
  for (const auto& owner : retired_gpd_models_) {
    owner->Retire();
  }
}

bool ProfileManager::DecryptAccountFile(std::span<const uint8_t> bytes,
                                        X_XAMACCOUNTINFO* output, bool devkit) {
  // The authentication prefix and confounder are part of the encrypted file.
  // A full Account struct alone is still 24 bytes short. Never touch output
  // until authentication succeeds, so failed repairs retain the old account.
  if (!output || bytes.size() < sizeof(X_XAMACCOUNTINFO) + 0x18) {
    return false;
  }
  const uint8_t* data = bytes.data();
  const uint8_t* key = util::GetXeKey(0x19, devkit);
  if (!key) {
    return false;  // this shouldn't happen...
  }

  // Generate RC4 key from data hash
  uint8_t rc4_key[0x14];
  xboxkrnl::XeCryptHmacSha(key, 0x10, data, 0x10, nullptr, 0, nullptr, 0,
                           rc4_key, 0x14);

  uint8_t dec_data[sizeof(X_XAMACCOUNTINFO) + 8];

  // Decrypt data
  util::RC4(rc4_key, 0x10, data + 0x10, sizeof(dec_data), dec_data,
            sizeof(dec_data));

  // Verify decrypted data against hash
  uint8_t data_hash[0x14];
  xboxkrnl::XeCryptHmacSha(key, 0x10, dec_data, sizeof(dec_data), nullptr, 0,
                           nullptr, 0, data_hash, 0x14);

  if (std::memcmp(data, data_hash, 0x10) == 0) {
    // Copy account data to output
    std::memcpy(output, dec_data + 8, sizeof(X_XAMACCOUNTINFO));
    return true;
  }

  return false;
}

void ProfileManager::EncryptAccountFile(const X_XAMACCOUNTINFO* input,
                                        uint8_t* output, bool devkit) {
  const uint8_t* key = util::GetXeKey(0x19, devkit);
  if (!key) {
    return;  // this shouldn't happen...
  }

  X_XAMACCOUNTINFO* output_acct =
      reinterpret_cast<X_XAMACCOUNTINFO*>(output + 0x18);
  std::memcpy(output_acct, input, sizeof(X_XAMACCOUNTINFO));

  // Set confounder, should be random but meh
  std::memset(output + 0x10, 0xFD, 8);

  // Encrypted data = xam account info + 8 byte confounder
  uint32_t enc_data_size = sizeof(X_XAMACCOUNTINFO) + 8;

  // Set data hash
  uint8_t data_hash[0x14];
  xboxkrnl::XeCryptHmacSha(key, 0x10, output + 0x10, enc_data_size, nullptr, 0,
                           nullptr, 0, data_hash, 0x14);

  std::memcpy(output, data_hash, 0x10);

  // Generate RC4 key from data hash
  uint8_t rc4_key[0x14];
  xboxkrnl::XeCryptHmacSha(key, 0x10, data_hash, 0x10, nullptr, 0, nullptr, 0,
                           rc4_key, 0x14);

  // Encrypt data
  util::RC4(rc4_key, 0x10, output + 0x10, enc_data_size, output + 0x10,
            enc_data_size);
}

ProfileManager::ProfileManager(KernelState* kernel_state,
                               UserTracker* user_tracker,
                               ContentManager& content_manager)
    : account_commit_gate_(std::make_shared<ProfileAccountCommitGate>(this)),
      kernel_state_(kernel_state),
      user_tracker_(user_tracker),
      content_manager_(content_manager) {
  settings_read_lifetime_ = std::make_shared<ProfileSettingsReadLifetime>();
  settings_read_provider_.reset(new ProfileSettingsReadProvider(this, settings_read_lifetime_));
  logged_profiles_.clear();
  accounts_.clear();
  InitializeRosterAuthority();

  try {
    ReloadProfiles();
  } catch (...) {
    account_commit_gate_->CloseAndDrain();
    throw;
  }
}

void ProfileManager::InitializeProfiles() {
  std::error_code error;
  const auto expected = CaptureRosterExpected(error);
  if (error) { return; }
  ProfileRosterDesired desired;
  desired.remember_slots = false;
  const std::array<std::string, 4> configured = {
      cvars::logged_profile_slot_0_xuid, cvars::logged_profile_slot_1_xuid,
      cvars::logged_profile_slot_2_xuid, cvars::logged_profile_slot_3_xuid};
  const std::array<std::string, 4> configured_sources = {
      cvars::logged_profile_slot_0_source_id, cvars::logged_profile_slot_1_source_id,
      cvars::logged_profile_slot_2_source_id, cvars::logged_profile_slot_3_source_id};
  for (uint8_t slot = 0; slot != 4; ++slot) {
    if (!configured[slot].empty()) {
      desired.xuids[slot] = xe::string_util::from_string<uint64_t>(configured[slot], true);
      if (desired.xuids[slot] && !configured_sources[slot].empty()) {
        const auto source = content_manager_.sources().Source(configured_sources[slot]);
        if (!source || !source->available || !source->profile_writable) {
          XELOGW("Remembered profile source {} is unavailable", configured_sources[slot]); return;
        }
        desired.origins[slot] = ProfileRosterSelectionOrigin{
            source->source.device_id, source->source.id, source->generation, {}};
      }
    }
  }
  const auto result = AdoptRoster(desired, expected);
  if (result.error) { XELOGW("Initial local roster refused ({})", result.error.message()); }
}

void ProfileManager::ReloadProfile(const uint64_t xuid) {
  LoadAccount(xuid);
}

struct ProfileManager::ProfileDiscovery {
  std::map<uint64_t, std::vector<ContentSourceState>> candidates;
  // Incomplete source observation cannot prove that a previous duplicate
  // disappeared. Candidate paths remain hints, never Account authority.
  bool complete = true;
};

void ProfileManager::MarkProfileDiscoveryConflictLocked(uint64_t xuid) {
  profile_discovery_conflicts_.insert(xuid);
  if (profile_discovery_revision_ != UINT64_MAX) {
    ++profile_discovery_revision_;
  }
}

void ProfileManager::ReloadProfiles() {
  if (global_critical_region::is_held_by_current_thread()) { return; }
  auto use = account_commit_gate_->Acquire();
  if (!use) { return; }
  // This host-only discovery lane never nests a manager/namespace lock over
  // registry observation or disk IO. Concurrent refresh is a truthful retry.
  std::unique_lock serial(profile_discovery_mutex_, std::try_to_lock);
  if (!serial.owns_lock()) { return; }
  uint64_t before_revision = 0;
  {
    std::unique_lock lock(profile_mount_mutex_, std::try_to_lock);
    if (!lock.owns_lock()) { return; }
    before_revision = profile_discovery_revision_;
  }
  auto discovery = FindProfiles();
  std::optional<uint64_t> reconciliation;
  {
    std::unique_lock lock(profile_mount_mutex_, std::try_to_lock);
    if (!lock.owns_lock()) { return; }
    // A concurrently learned origin conflict cannot be erased by this older
    // scan. Overflow likewise closes reconciliation rather than wrapping.
    discovery.complete &= before_revision == profile_discovery_revision_;
    for (const auto& [xuid, sources] : discovery.candidates) {
      if (sources.size() > 1) { MarkProfileDiscoveryConflictLocked(xuid); }
    }
    if (discovery.complete && profile_discovery_revision_ != UINT64_MAX) {
      reconciliation = profile_discovery_revision_;
    }
  }
  for (const auto& [xuid, sources] : discovery.candidates) {
    if (sources.size() != 1) {
      XELOGW("Profile {:016X}: multiple registered source candidates; unscoped selection refused", xuid);
      continue;
    }
    // Authentication, durability and exact-generation mount/cache admission
    // are the SAME shared path used by creation and ordinary Account reload.
    if (!reconciliation) {
      std::unique_lock lock(profile_mount_mutex_, std::try_to_lock);
      if (!lock.owns_lock()) { return; }
      const auto known = account_metadata_.find(xuid);
      if (known != account_metadata_.end() &&
          (known->second->source_id() != sources.front().source.id ||
           known->second->device_id() != sources.front().source.device_id ||
           known->second->source_root() != sources.front().managed_identity->source()->path())) {
        // One unavailable source must not turn a different last-known origin
        // into a new conflict or an automatic source replacement.
        continue;
      }
    }
    LoadAccountInternal(xuid, &sources.front(), reconciliation);
  }
}

void ProfileManager::ReloadProfileGpds() {
  std::vector<uint64_t> xuids;
  {
    std::lock_guard lock(profile_mount_mutex_);
    for (const auto& [slot, profile] : logged_profiles_) {
      xuids.push_back(profile->xuid());
    }
  }
  for (auto xuid : xuids) {
    if (auto error = ReloadGpdModel(xuid)) {
      XELOGW("Profile {:016X}: GPD reload deferred/refused ({})", xuid,
             error.message());
    }
  }
}

void ProfileManager::SyncProfilesWithConfig() { InitializeProfiles(); }

std::shared_ptr<UserProfile> ProfileManager::CaptureProfile(uint64_t xuid) const {
  const auto slot = GetUserIndexAssignedToProfile(xuid);
  const auto profile = slot < 4 ? CaptureRosterProfile(slot) : nullptr;
  return profile && (profile->xuid() == xuid || profile->GetOnlineXUID() == xuid) ? profile : nullptr;
}
std::shared_ptr<UserProfile> ProfileManager::CaptureProfile(uint8_t slot) const {
  return CaptureRosterProfile(slot);
}
UserProfile* ProfileManager::GetProfileLive(uint64_t xuid) const {
  thread_local std::shared_ptr<UserProfile> held;
  held.reset();
  if (!xuid) { return nullptr; }
  for (uint8_t slot = 0; slot != 4; ++slot) {
    auto row = CaptureRosterProfile(slot);
    if (row && row->GetOnlineXUID() == xuid) { held = std::move(row); break; }
  }
  return held.get();
}
UserProfile* ProfileManager::GetProfile(uint64_t xuid) const {
  thread_local std::shared_ptr<UserProfile> held;
  held = CaptureProfile(xuid);
  return held.get();
}
UserProfile* ProfileManager::GetProfile(uint8_t slot) const {
  if (slot == XUserIndexLatest) { slot = kernel_state_->emulator()->input_system()->GetLastUsedSlot(); }
  if (slot == XUserIndexAny) {
    const auto roster = CaptureSelectedRoster();
    if (!roster) { return nullptr; }
    for (uint8_t i = 0; i != 4; ++i) { if (roster->slots()[i].xuid) { slot = i; break; } }
  }
  // UserProfile owns display bytes/properties and only a weak GPD-model
  // reference. Retaining this legacy borrowed projection through the next
  // invocation cannot retain a dead Kernel's actual storage/model authority.
  thread_local std::shared_ptr<UserProfile> held;
  held = slot < 4 ? CaptureRosterProfile(slot) : nullptr;
  return held.get();
}

bool ProfileManager::LoadAccount(const uint64_t xuid) {
  return LoadAccountInternal(xuid, nullptr);
}

bool ProfileManager::LoadAccountInternal(
    uint64_t xuid, const ContentSourceState* exact_source,
    std::optional<uint64_t> discovery_revision) {
  auto use = account_commit_gate_->Acquire();
  if (!use) { return false; }
  {
    std::unique_lock lock(profile_mount_mutex_, std::try_to_lock);
    if (!lock.owns_lock()) { return false; }
    if (profile_discovery_conflicts_.contains(xuid) &&
        (!discovery_revision || *discovery_revision != profile_discovery_revision_)) {
      std::error_code selected_error;
      // A current exact committed row is an existing data producer, not a
      // fresh XUID-only source choice. Preserve its ordinary read/update path.
      if (!CaptureCachedAccount(xuid, selected_error)) { return false; }
    }
  }
  const std::string mount_name = fmt::format(kDefaultMountFormat, xuid) + ':';
  ReapReleasedProfileReads();
  if (!MountProfileInternal(xuid, {}, {}, exact_source)) {
    return false;
  }
  struct MountScope {
    ProfileManager& manager;
    uint64_t xuid;
    bool owned;
    ~MountScope() {
      if (owned) {
        manager.DismountProfile(xuid);
      }
    }
  } mount{*this, xuid, true};

  std::error_code error;
  auto snapshot = CaptureDurableAccount(xuid, error);
  if (!snapshot || !PublishAccountCache(std::move(snapshot), error,
                                        discovery_revision)) {
    XELOGW("Profile {:016X}: durable Account load refused ({})", xuid,
           error.message());
    return false;
  }
  return true;
}

std::unique_ptr<ProfileManager::ManagedFormatIdleReservation>
ProfileManager::TryReserveIdleForManagedFormat(std::error_code& error) {
  error.clear();
  ReapReleasedProfileReads();
  if (xe::global_critical_region::is_held_by_current_thread()) {
    error = std::make_error_code(std::errc::operation_would_block); return {};
  }
  // Drop only actually drained Account device owners outside metadata/GCRC.
  std::vector<std::shared_ptr<ProfileAccountIdentity>> released_owners;
  {
    std::unique_lock lock(profile_mount_mutex_, std::try_to_lock);
    if (!lock.owns_lock()) {
      error = std::make_error_code(std::errc::device_or_resource_busy);
      return {};
    }
    for (auto& identity : retired_account_identities_) {
      if (identity->writes_drained()) { released_owners.push_back(std::move(identity)); }
    }
    std::erase(retired_account_identities_, nullptr);
  }
  released_owners.clear();
  auto reservation = std::unique_ptr<ManagedFormatIdleReservation>(
      new ManagedFormatIdleReservation(profile_mount_mutex_));
  if (!reservation->profiles_.owns_lock() ||
      !reservation->namespace_.try_lock()) {
    error = std::make_error_code(std::errc::device_or_resource_busy); return {};
  }
  for (const auto& identity : retired_account_identities_) {
    if (!identity->writes_drained()) {
      error = std::make_error_code(std::errc::device_or_resource_busy);
      return {};
    }
  }
  // Initial scope is deliberately idle-only. Retired models may still own
  // redo/publication leases, so their presence is a truthful BUSY as well.
  if (!logged_profiles_.empty() || preparing_slots_.any() ||
      !preparing_profiles_.empty() || !retiring_profiles_.empty() ||
      !profile_mounts_.empty() || !gpd_models_.empty() ||
      !retired_gpd_models_.empty()) {
    error = std::make_error_code(std::errc::device_or_resource_busy); return {};
  }
  return reservation;
}

bool ProfileManager::MountProfile(const uint64_t xuid, std::string mount_path,
                                  uint32_t* mounted_device_id) {
  ReapAccountEdits();
  ReapReleasedProfileReads();
  return MountProfileInternal(xuid, std::move(mount_path), {}, nullptr,
                              mounted_device_id);
}
bool ProfileManager::MountProfileInternal(uint64_t xuid, std::string mount_path,
    std::shared_ptr<ProfileReadMountNode> read,
    const ContentSourceState* exact_source, uint32_t* mounted_device_id,
    HostProfileAccountEdit::State* edit) {
  if (xe::global_critical_region::is_held_by_current_thread()) {
    return false;
  }
  if (edit && (read || edit->mounted || edit->xuid != xuid)) { return false; }
  const auto bind_edit = [&](const ProfileMount& held) {
    if (!edit) { return; }
    // Same successful lock/counter/namespace publication as the real mount.
    // Shared pointer copies only; no issuer/readiness fact is synthesized.
    edit->mount_generation = held.mount_generation;
    edit->source_generation = held.source_generation;
    edit->device_id = held.device_id;
    edit->device = held.device;
    edit->source = held.source; edit->view = held.view;
    edit->guest_generation = held.guest_generation;
    edit->managed_identity = held.managed_identity;
    edit->mounted = true;
  };
  if (mount_path.empty()) {
    mount_path = fmt::format(kDefaultMountFormat, xuid);
  }
  if (mount_path.back() != ':') {
    mount_path += ':';
  }
  const auto key = xe::utf8::lower_ascii(mount_path);
  // Registry observation and new device enumeration happen outside manager,
  // model and namespace locks. The result owns the exact source generation.
  std::string known_source = "hdd";
  std::shared_ptr<ProfileStorageLease> selected_storage;
  if (!exact_source) {
    // Legacy primary discovery/creation has no known row yet. Once a genuine
    // Account row exists its exact owning source, not XUID alone, selects it.
    std::unique_lock lock(profile_mount_mutex_, std::try_to_lock);
    if (!lock.owns_lock()) { return false; }
    if (profile_discovery_conflicts_.contains(xuid)) {
      std::error_code selected_error;
      if (!CaptureCachedAccount(xuid, selected_error)) { return false; }
      const auto selected = CaptureRosterAccount(xuid);
      if (!selected) { return false; }
      selected_storage = selected->identity()->storage();
    }
    if (const auto row = account_metadata_.find(xuid);
        row != account_metadata_.end() && !row->second->source_id().empty()) {
      known_source = row->second->source_id();
    }
  }
  const auto source = exact_source
      ? content_manager_.sources().Source(exact_source->source.id)
      : content_manager_.sources().Source(known_source);
  if (selected_storage && (!source ||
      source->generation != selected_storage->source_generation() ||
      source->source.device_id != selected_storage->device_id() ||
      source->storage != selected_storage->guest_generation() ||
      source->managed_identity != selected_storage->managed_identity())) {
    return false;
  }
  if (exact_source && (!source || source->generation != exact_source->generation ||
      source->source.device_id != exact_source->source.device_id ||
      source->storage != exact_source->storage ||
      source->managed_identity != exact_source->managed_identity)) {
    return false;
  }
  if (!source || !source->available || !source->profile_writable ||
      !source->storage || source->storage->retiring() ||
      !source->managed_identity || source->managed_identity->retiring()) {
    return false;
  }
  // Intent publication keeps the exact internal identity authority. Guest
  // access and its independently retiring generation remain source->storage.
  auto source_authority =
      std::const_pointer_cast<filesystem::HostStorageRoot>(
          source->managed_identity->source());
  auto* fs = kernel_state_->file_system();
  uint64_t mount_generation = 0;
  {
    std::unique_lock lock(profile_mount_mutex_, std::try_to_lock);
    if (!lock.owns_lock()) {
      return false;
    }
    auto global = xe::global_critical_region::TryAcquire();
    if (!global.owns_lock()) {
      return false;
    }
    if (retiring_profiles_.contains(xuid)) {
      return false;
    }
    if (auto found = profile_mounts_.find(key); found != profile_mounts_.end()) {
      auto& held = found->second;
      const auto& device = held.device;
    if (held.xuid != xuid || held.source_generation != source->generation ||
        held.source != source_authority ||
        held.guest_generation != source->storage ||
        held.guest_generation->retiring() || held.view->retiring() ||
          held.managed_identity != source->managed_identity || !device ||
          fs->AcquireDevice(device->mount_path()) != device ||
          device->storage() != held.view) {
      return false;
    }
    if (read) {
      const auto use = read->use.lock();
      if (!use || !use->Current() || read->mount_generation || !held.mount_generation ||
          held.read_references == SIZE_MAX) { return false; }
      read->mount_generation = held.mount_generation;
      read->next = held.read_head;
      held.read_head = std::move(read);
      ++held.read_references;
    } else {
      if (held.references == SIZE_MAX) { return false; }
      ++held.references;
    }
    bind_edit(held);
    if (mounted_device_id) { *mounted_device_id = held.device_id; }
    return true;
  }
  if (fs->AcquireDevice(mount_path) || !next_profile_mount_generation_ ||
      next_profile_mount_generation_ == UINT64_MAX) {
      return false;
    }
    mount_generation = next_profile_mount_generation_++;
  }
  const auto profile_path = source->storage->path() /
                            ProfileStorageLease::ProfileRelative(xuid);
  std::error_code error;
  auto storage = source->storage->Subroot(
      profile_path.lexically_relative(source->storage->path()), false, error);
  if (!storage) {
    return false;
  }
  auto device = std::make_unique<vfs::HostPathDevice>(
      mount_path, profile_path, false, false, "STFS", storage,
      source->storage);
  const auto storage_volume = content_manager_.sources().CaptureStorageVolume(*source);
  if (!device->SetStorageVolume(storage_volume) || !device->Initialize()) {
    return false;
  }
  auto view = device->storage();
  const auto volume = content_manager_.Volume(source->source.id);
  auto recovery = volume && volume->profile_writable() ? volume->recovery() : nullptr;
  if (!recovery) { return false; }
  auto account_service = std::make_shared<ProfileAccountFileService>(
      xuid, view, recovery, account_commit_gate_);
  // First close new raw alias admission under the SAME namespace lock as
  // VFS OpenFile. A preexisting raw open has already completed its native open
  // by that boundary. A later queued open enters this policy before mutation.
  // On subsequent failure the weak registry retains only a closed tombstone.
  if (!fs->RegisterManagedFile(account_service)) { return false; }
  auto account_observed = view->CaptureSnapshot(
      "Account", ProfileAccountSnapshot::kMaximumBytes, error, true);
  if (!account_observed) { return false; }
  // The actual namespace claim is common to VFS and direct base/content IO,
  // remains valid after CAS changes the inode, and refuses all old raw aliases.
  auto reserved = view->ReserveSnapshotFile(account_observed, error);
  if (!reserved || !device->InstallSnapshotReservation(reserved)) { return false; }
  view = std::move(reserved);
  auto final_service = std::make_shared<ProfileAccountFileService>(
      xuid, view, recovery, account_commit_gate_);
  // The temporary fence remains retained and CLOSED throughout replacement;
  // no raw fallback exists in the interval. Registration owns its own GCRC so
  // an old service's last native-storage owner can run down after that release.
  // Neither service nor the actual target reservation is dropped while locked.
  account_service->Retire();
  if (!fs->RegisterManagedFile(final_service)) { return false; }
  account_service = std::move(final_service);
  // Complete actual guest/backing admission BEFORE either manager/GCRC lock.
  // The opaque VFS capsule keeps the exact admitted device on every failure;
  // final publication below performs no native observation or reopen.
  std::unique_ptr<vfs::Device> registration_device = std::move(device);
  auto prepared_registration = fs->PrepareDeviceRegistration(
      registration_device,error);
  if (!prepared_registration) { return false; }
  // Allocate the map node outside the metadata publication boundary as well.
  // Inserting this extracted same-allocator node cannot allocate after the
  // device becomes visible in VFS.
  std::map<std::string,ProfileMount> prepared_mount;
  prepared_mount.emplace(key,ProfileMount{xuid,source->generation,read ? 0u : 1u,
      source_authority,view,source->storage,source->source.id,
      source->source.device_id,{},source->managed_identity,account_service});
  auto& prepared_record = prepared_mount.begin()->second;
  prepared_record.device = std::dynamic_pointer_cast<vfs::HostPathDevice>(
      prepared_registration->device());
  if (!prepared_record.device) { return false; }
  prepared_record.mount_generation = mount_generation;
  if (read) {
    const auto use = read->use.lock();
    if (!use || !use->Current() || read->mount_generation) { return false; }
    read->mount_generation = mount_generation;
    prepared_record.read_references = 1;
    prepared_record.read_head = std::move(read);
  }
  auto mount_node = prepared_mount.extract(prepared_mount.begin());
  // Retain a successful registration through any metadata-only rollback;
  // native device/root destruction must follow release of both locks.
  std::shared_ptr<vfs::Device> registration_keepalive;
  // Destruction of the detached device is ordered after release of this lock.
  std::unique_lock lock(profile_mount_mutex_, std::try_to_lock);
  if (!lock.owns_lock() || retiring_profiles_.contains(xuid) ||
      profile_mounts_.contains(key) || source->storage->retiring() ||
      source->managed_identity->retiring()) {
    return false;
  }
  auto global = xe::global_critical_region::TryAcquire();
  if (!global.owns_lock()) {
    return false;
  }
  if (XFAILED(fs->RegisterPreparedDeviceExclusive(
          *prepared_registration,registration_keepalive))) {
    return false;
  }
  const auto inserted_mount = profile_mounts_.insert(std::move(mount_node));
  bind_edit(inserted_mount.position->second);
  if (mounted_device_id) { *mounted_device_id = source->source.device_id; }
  return true;
}

bool ProfileManager::DismountProfile(const uint64_t xuid) {
  return DismountProfile(fmt::format(kDefaultMountFormat, xuid));
}

bool ProfileManager::DismountProfile(std::string_view mount_path) {
  ReapReleasedProfileReads();
  return DismountProfileInternal(mount_path);
}
bool ProfileManager::DismountProfileInternal(std::string_view mount_path,
    std::shared_ptr<ProfileReadMountNode> expected_read,
    const HostProfileAccountEdit::State* edit) {
  if (xe::global_critical_region::is_held_by_current_thread()) {
    return false;
  }
  // Keep native directory/device destructors outside metadata/GCRC locks.
  std::shared_ptr<vfs::HostPathDevice> retiring_device;
  std::shared_ptr<vfs::Device> detached;
  decltype(profile_mounts_)::node_type retired_mount;
  decltype(account_identities_)::node_type retired_account;
  std::unique_lock lock(profile_mount_mutex_, std::try_to_lock);
  if (!lock.owns_lock() || mount_path.empty()) {
    return false;
  }
  std::string path(mount_path);
  if (path.back() != ':') {
    path += ':';
  }
  const auto key = xe::utf8::lower_ascii(path);
  const auto found = profile_mounts_.find(key);
  if (edit && (expected_read || !edit->mounted || !edit->mount_generation ||
      !edit->device || !edit->view)) { return false; }
  const bool same_edit = !edit || (found != profile_mounts_.end() &&
      found->second.xuid == edit->xuid &&
      found->second.mount_generation == edit->mount_generation &&
      found->second.device == edit->device && found->second.view == edit->view &&
      found->second.source == edit->source &&
      found->second.guest_generation == edit->guest_generation &&
      found->second.managed_identity == edit->managed_identity &&
      found->second.source_generation == edit->source_generation &&
      found->second.device_id == edit->device_id);
  if (edit && !same_edit) {
    // A missing/reused alias alone proves nothing. Only actual old namespace
    // and view retirement plus absence of that exact device can settle it.
    auto global = xe::global_critical_region::TryAcquire();
    return global.owns_lock() && edit->device->namespace_retiring() &&
        edit->view->retiring() &&
        kernel_state_->file_system()->AcquireDevice(edit->mount_path) != edit->device;
  }
  if (found == profile_mounts_.end()) { return false; }
  auto& held = found->second;
  std::shared_ptr<ProfileReadMountNode>* read_link = nullptr;
  if (expected_read) {
    if (!expected_read->use.expired() ||
        expected_read->mount_generation != held.mount_generation) { return false; }
    read_link = &held.read_head;
    while (*read_link && *read_link != expected_read) { read_link = &(*read_link)->next; }
    if (!*read_link || !held.read_references) { return false; }
    if (held.references || held.read_references > 1) {
      *read_link = expected_read->next;
      --held.read_references;
      return true;
    }
  } else {
    if (!held.references) { return false; }
    if (held.references > 1 || held.read_references) {
      --held.references;
      return true;
    }
  }
  for (const auto& retired : retired_account_identities_) {
    if (retired->storage()->xuid() == found->second.xuid &&
        !retired->writes_drained()) { return false; }
  }
  if (!edit) {
    if (auto owner = account_identities_.find(found->second.xuid);
        owner != account_identities_.end() && owner->second->slot() == 0xFF) {
      owner->second->Retire();
      if (!owner->second->writes_drained()) { return false; }
    }
  }
  auto global = xe::global_critical_region::TryAcquire();
  if (!global.owns_lock()) {
    return false;
  }
  retiring_device = held.device;
  if (!retiring_device || retiring_device->storage() != held.view) { return false; }
  const auto registered = kernel_state_->file_system()->AcquireDevice(retiring_device->mount_path());
  const bool edit_already_retired = edit && registered != retiring_device &&
      retiring_device->namespace_retiring() && held.view->retiring();
  if (registered != retiring_device && !edit_already_retired) { return false; }
  const auto account_owner = account_identities_.find(held.xuid);
  const auto exact_edit_account = [&](const auto& owner) {
    const auto& storage = owner->storage();
    return storage->xuid() == held.xuid && storage->device() == held.device &&
        storage->view() == held.view && storage->source() == held.source &&
        storage->guest_generation() == held.guest_generation &&
        storage->managed_identity() == held.managed_identity &&
        storage->source_generation() == held.source_generation &&
        storage->device_id() == held.device_id;
  };
  if (edit) {
    // A new Account owner at the same XUID is not our retirement authority.
    // An older issued identity must already be truly retired before a changed
    // owner can settle its reference; never close a replacement generation.
    if (edit->expected && !edit->expected->identity()->retired() &&
        (account_owner == account_identities_.end() ||
         account_owner->second != edit->expected->identity())) { return false; }
    // Real managed-file Open/count admission shares this namespace guard.
    // Do not close a different raw File/cache producer merely to cancel our
    // editor. Actual typed writers share the atomic Account admission word.
    if (retiring_device->has_open_files() || retiring_device->has_managed_files()) {
      return false;
    }
    if (account_owner != account_identities_.end() &&
        exact_edit_account(account_owner->second) &&
        account_owner->second->slot() == 0xFF &&
        !account_owner->second->RetireIfIdle()) { return false; }
  }
  if (!edit_already_retired &&
      !(detached = kernel_state_->file_system()->DetachDevice(
          retiring_device->mount_path(), retiring_device.get()))) { return false; }
  // A proved external closure has already performed actual old-device
  // retirement. Extract only this stamped old record; leave the current VFS
  // alias/replacement untouched and do not require unavailable-source IO.
  if (found->second.publication_lease) {
    found->second.publication_lease->Retire();
  }
  if (auto owner = account_identities_.find(found->second.xuid);
      owner != account_identities_.end() && owner->second->slot() == 0xFF &&
      (!edit || exact_edit_account(owner->second))) {
    retired_account = account_identities_.extract(owner);
  }
  retired_mount = profile_mounts_.extract(found);
  global.unlock();
  lock.unlock();
  if (detached) { detached->Retire(); }
  return true;
}

void ProfileManager::ReapReleasedProfileReads() {
  if (xe::global_critical_region::is_held_by_current_thread()) { return; }
  std::vector<std::pair<std::string, std::shared_ptr<ProfileReadMountNode>>> expired;
  {
    std::unique_lock lock(profile_mount_mutex_, std::try_to_lock);
    if (!lock.owns_lock()) { return; }
    for (const auto& [key, mount] : profile_mounts_) {
      for (auto read = mount.read_head; read; read = read->next) {
        if (read->use.expired() && mount.device) {
          expired.emplace_back(mount.device->mount_path(), read);
        }
      }
    }
  }
  // Exact generation/node checks happen again in the real dismount. A busy
  // native owner remains in the existing record and is retried later.
  for (auto& [key, read] : expired) { DismountProfileInternal(key, std::move(read)); }
}

std::shared_ptr<ProfileStorageLease> ProfileManager::AcquireProfileStorageLease(
    uint64_t xuid, std::error_code& error) {
  error.clear();
  // Existing mount paths acquire profile mutex before the global namespace.
  // Both acquisitions here are nonwaiting, including when a guest caller
  // already owns GCRC: never invert that order or wait for an IO lane.
  std::unique_lock profile_lock(profile_mount_mutex_, std::try_to_lock);
  if (!profile_lock.owns_lock()) {
    error = std::make_error_code(std::errc::device_or_resource_busy);
    return {};
  }
  auto namespace_lock = xe::global_critical_region::TryAcquire();
  if (!namespace_lock.owns_lock()) {
    error = std::make_error_code(std::errc::device_or_resource_busy);
    return {};
  }
  const auto mount = fmt::format(kDefaultMountFormat, xuid) + ":";
  const auto found = profile_mounts_.find(xe::utf8::lower_ascii(mount));
  if (!xuid || found == profile_mounts_.end() || found->second.xuid != xuid) {
    error = std::make_error_code(std::errc::no_such_device);
    return {};
  }
  auto& held = found->second;
  if (!held.managed_identity || held.managed_identity->retiring() ||
      !held.guest_generation || held.guest_generation->retiring() ||
      held.view->retiring()) {
    error = std::make_error_code(std::errc::operation_canceled);
    return {};
  }
  auto device = held.device;
  if (!device || kernel_state_->file_system()->AcquireDevice(device->mount_path()) != device ||
      device->storage() != held.view) {
    error = std::make_error_code(std::errc::no_such_device);
    return {};
  }
  if (held.publication_lease) {
    if (held.publication_lease->device() != device ||
        held.publication_lease->source() != held.source ||
        held.publication_lease->view() != held.view ||
        held.publication_lease->guest_generation() != held.guest_generation ||
        held.publication_lease->source_generation() != held.source_generation ||
        held.publication_lease->retiring()) {
      error = std::make_error_code(std::errc::operation_canceled);
      return {};
    }
    return held.publication_lease;
  }
  held.publication_lease = ProfileStorageLease::Capture(
      xuid, held.source_id, held.device_id, held.source_generation,
      std::move(device), held.source, held.view, error, held.managed_identity,
      held.guest_generation);
  return held.publication_lease;
}

std::shared_ptr<ProfileGpdModelAuthority>
ProfileManager::AcquireProfileGpdModel(uint64_t xuid, std::error_code& error) {
  error.clear();
  auto model = CaptureRosterModel(xuid);
  if (!model || !model->storage() || model->storage()->retiring()) {
    error = std::make_error_code(std::errc::operation_canceled); return {};
  }
  return model;
}
std::shared_ptr<ProfileGpdModelAuthority>
ProfileManager::AcquireProfileGpdModel(uint8_t slot, std::error_code& error) {
  const auto roster = CaptureSelectedRoster();
  if (!roster || slot >= 4 || !roster->slots()[slot].xuid) {
    error = std::make_error_code(std::errc::operation_canceled); return {};
  }
  const auto model = AcquireProfileGpdModel(roster->slots()[slot].xuid, error);
  if (model && model->user_slot() == slot &&
      model->sign_in_generation() == roster->slots()[slot].sign_in_generation) { return model; }
  error = std::make_error_code(std::errc::operation_canceled); return {};
}

std::shared_ptr<const ProfileGpdReadModels> ProfileManager::CaptureGpdRead(
    uint8_t slot, std::error_code& error) {
  auto model = AcquireProfileGpdModel(slot, error);
  if (!model) {
    return {};
  }
  auto read = CaptureGpdRead(model->xuid(), error);
  if (!read) {
    return {};
  }
  if (AcquireProfileGpdModel(slot, error) != model) {
    if (!error) {
      error = std::make_error_code(std::errc::operation_canceled);
    }
    return {};
  }
  return read;
}
std::shared_ptr<const ProfileGpdReadModels> ProfileManager::CaptureGpdRead(
    uint64_t xuid, std::error_code& error) {
  // Validation may observe files/providers: never release a caller's GCRC or
  // return an indefinitely stale snapshot instead of an honest busy result.
  if (xe::global_critical_region::is_held_by_current_thread()) {
    error = std::make_error_code(std::errc::operation_would_block);
    return {};
  }
  auto model = AcquireProfileGpdModel(xuid, error);
  if (!model) {
    return {};
  }
  auto captured = model->CaptureRead(error);
  if (!captured) {
    return {};
  }
  {
    auto checked = model->storage()->view()->ValidateSnapshotBatch(
        captured->tokens, error);
    if (checked) {
      if (!model->IsCurrent(captured->origin)) {
        error = std::make_error_code(std::errc::operation_canceled);
        return {};
      }
      return captured->models;
    }
  }
  // Real mutation outside Tracker is authoritative. A pending durable intent
  // blocks this reload and remains recoverable; no old model or default
  // escapes.
  if (error != std::errc::state_not_recoverable &&
      error != std::errc::no_such_file_or_directory) {
    return {};
  }
  error = ReloadGpdModel(xuid);
  if (error) {
    return {};
  }
  captured = model->CaptureRead(error);
  if (!captured) {
    return {};
  }
  auto checked =
      model->storage()->view()->ValidateSnapshotBatch(captured->tokens, error);
  if (!checked || !model->IsCurrent(captured->origin)) {
    if (!error) {
      error = std::make_error_code(std::errc::operation_canceled);
    }
    return {};
  }
  return captured->models;
}

std::shared_ptr<const ProfileSettingsReadBatch> ProfileManager::CaptureProfileSettingsRead(
    std::span<const uint64_t> xuids, uint32_t title, std::error_code& error) {
  error.clear();
  if (xe::global_critical_region::is_held_by_current_thread()) {
    error = std::make_error_code(std::errc::operation_would_block); return {};
  }
  if (xuids.empty() || xuids.size() > 16 ||
      !settings_read_provider_ || !settings_read_provider_->Current(*this)) {
    error = std::make_error_code(std::errc::invalid_argument); return {};
  }
  if (!CaptureSelectedRoster()) {
    error = std::make_error_code(std::errc::device_or_resource_busy); return {};
  }
  auto preparation = account_commit_gate_->Acquire();
  if (!preparation) { error = std::make_error_code(std::errc::operation_canceled); return {}; }
  auto use = std::make_shared<ProfileSettingsReadUse>(settings_read_lifetime_, account_commit_gate_);
  // This synchronous preparation owns the real manager admission. Cleanup
  // never escapes it or invokes a raw manager callback from a retained row.
  auto cleanup = [this, &use](void*) { use.reset(); ReapReleasedProfileReads(); };
  std::unique_ptr<void, decltype(cleanup)> preparation_cleanup(
      reinterpret_cast<void*>(1), cleanup);
  ReapReleasedProfileReads();
  auto out = std::shared_ptr<ProfileSettingsReadBatch>(new ProfileSettingsReadBatch);
  out->use_ = use;
  out->publication_gate_ = account_commit_gate_;
  out->profile_owner_ = this; // only dereferenced inside actual counted gate Use
  std::map<uint64_t, size_t> captured;
  uint64_t aggregate = 0;
  for (const auto xuid : xuids) {
    if (const auto previous = captured.find(xuid); previous != captured.end()) {
      auto copy = out->users_[previous->second];
      out->users_.push_back(std::move(copy)); continue;
    }
    const auto selected_account = CaptureRosterAccount(xuid);
    {
      std::unique_lock lock(profile_mount_mutex_, std::try_to_lock);
      auto global = xe::global_critical_region::TryAcquire();
      if (!lock.owns_lock() || !global.owns_lock()) {
        error = std::make_error_code(std::errc::device_or_resource_busy); return {};
      }
      // The explicit local inventory contains only authenticated Accounts.
      // Unknown/remote XUIDs do not become profiles by guessing a path.
      if (!xuid || (!selected_account && (!accounts_.contains(xuid) ||
          !account_metadata_.contains(xuid) || retiring_profiles_.contains(xuid) ||
          preparing_profiles_.contains(xuid)))) {
        error = std::make_error_code(std::errc::no_such_file_or_directory); return {};
      }
    }
    auto read = std::make_shared<ProfileReadMountNode>(); read->use = use;
    auto native = std::make_shared<ProfileSettingsReadNativeOwners>();
    read->native = native;
    std::optional<ContentSourceState> exact_read_source;
    if (selected_account) {
      const auto& selected_storage = selected_account->identity()->storage();
      exact_read_source = content_manager_.sources().Source(selected_storage->source_id());
      if (!exact_read_source || exact_read_source->generation != selected_storage->source_generation() ||
          exact_read_source->source.device_id != selected_storage->device_id()) {
        error = std::make_error_code(std::errc::no_such_device); return {};
      }
    }
    if (!MountProfileInternal(xuid, {}, read, exact_read_source ? &*exact_read_source : nullptr)) {
      error = std::make_error_code(std::errc::device_or_resource_busy); return {};
    }
    ProfileSettingsReadBatch::User user;
    user.xuid_ = xuid; user.use_ = use; user.native_ = native;
    native->account = CaptureDurableAccount(xuid, error);
    if (!native->account) { return {}; }
    native->storage = native->account->identity()->storage();
    const auto volume = content_manager_.Volume(native->storage->source_id());
    const auto owner = content_manager_.ProfileIntents(native->storage->source_id());
    const auto recovery = volume && volume->profile_writable() ? volume->recovery() : nullptr;
    if (!owner || owner->source() != native->storage->source() || !recovery) {
      error = std::make_error_code(std::errc::no_such_device); return {};
    }
    // Existing coupled-GPD recovery is the prerequisite, not a writer/model
    // permission returned to this read-only observation.
    error = ProfileGpdTransaction::ReplayBeforeLoad(native->storage, owner, recovery,
                                                     volume->mutations());
    if (error) { return {}; }
    auto load = [&](uint32_t id, bool dashboard) -> bool {
      const auto snapshot = GpdSnapshotStore::Load(
          {xuid, native->storage->source_generation(), id}, native->storage->view());
      if (snapshot.error || !snapshot.token) {
        error = snapshot.error ? snapshot.error : std::make_error_code(std::errc::io_error);
        return false;
      }
      if (snapshot.token->bytes().size() >
          filesystem::HostStorageRoot::kMaximumSnapshotBatchBytes - aggregate) {
        error = std::make_error_code(std::errc::file_too_large); return false;
      }
      aggregate += snapshot.token->bytes().size();
      native->tokens.push_back(snapshot.token);
      if (!snapshot.readable()) { return true; } // retained real absence
      error = ValidateProfileGpdRecords(snapshot.token->bytes(), dashboard);
      if (error) { return false; }
      const auto bytes = std::vector<uint8_t>(snapshot.token->bytes().begin(),
                                             snapshot.token->bytes().end());
      if (dashboard) { user.dashboard_ = std::make_shared<const GpdInfoProfile>(bytes); }
      else { user.title_ = std::make_shared<const GpdInfoTitle>(id, bytes); user.title_id_ = id; }
      return true;
    };
    if (!load(kDashboardID, true) || (title && title != kDashboardID && !load(title, false))) {
      return {};
    }
    if (native->account->token()->root() != native->storage->view()) {
      error = std::make_error_code(std::errc::state_not_recoverable); return {};
    }
    if (native->account->token()->bytes().size() >
        filesystem::HostStorageRoot::kMaximumSnapshotBatchBytes - aggregate) {
      error = std::make_error_code(std::errc::file_too_large); return {};
    }
    aggregate += native->account->token()->bytes().size();
    native->tokens.push_back(native->account->token());
    error = native->account->CheckAvailable(); if (error) { return {}; }
    {
      // Every token keeps the SAME legitimate reserved profile view; this
      // existing guard validates one user's observation, not a global moment.
      auto checked = native->storage->view()->ValidateSnapshotBatch(native->tokens, error);
      if (!checked) { return {}; }
      if (!use->Current() || native->storage->retiring() || native->account->identity()->retired()) {
        error = std::make_error_code(std::errc::operation_canceled); return {};
      }
    }
    captured.emplace(xuid, out->users_.size()); out->users_.push_back(std::move(user));
  }
  if (!out->IdentityCurrent()) {
    error = std::make_error_code(std::errc::operation_canceled); return {};
  }
  return out;
}
std::shared_ptr<const ProfileSettingsReadBatch> ProfileManager::CaptureProfileSettingsRead(
    uint8_t slot, uint32_t title, std::error_code& error) {
  error.clear(); uint64_t xuid = 0, generation = 0;
  if (xe::global_critical_region::is_held_by_current_thread()) {
    error = std::make_error_code(std::errc::operation_would_block); return {};
  }
  // Keep the real manager alive across both slot observations and the nested
  // explicit-XUID capture, not only its inner observing-I/O phase.
  auto slot_use = account_commit_gate_->Acquire();
  if (!slot_use) {
    error = std::make_error_code(std::errc::operation_canceled); return {};
  }
  const auto selected = CaptureSelectedRoster();
  if (!selected || slot >= 4 || !selected->slots()[slot].xuid) {
    error = std::make_error_code(std::errc::no_such_file_or_directory); return {};
  }
  xuid = selected->slots()[slot].xuid;
  generation = selected->slots()[slot].sign_in_generation;
  const auto out = CaptureProfileSettingsRead(std::span<const uint64_t>(&xuid, 1), title, error);
  if (!out) { return {}; }
  auto entered = account_commit_gate_->Acquire();
  const auto native = entered ? out->users_.front().native_.lock() : nullptr;
  if (!native || !native->account) {
    error = std::make_error_code(std::errc::operation_canceled); return {};
  }
  const auto& identity = native->account->identity();
  if (identity->slot() != slot || identity->sign_in_generation() != generation || !out->IdentityCurrent()) {
    error = std::make_error_code(std::errc::operation_canceled); return {};
  }
  return out;
}
std::vector<X_PROFILEENUMRESULT> ProfileManager::CaptureProfileInventory(
    uint32_t device_id, std::error_code& error) const {
  error.clear(); std::vector<X_PROFILEENUMRESULT> out;
  std::unique_lock lock(profile_mount_mutex_, std::try_to_lock);
  auto global = xe::global_critical_region::TryAcquire();
  if (!lock.owns_lock() || !global.owns_lock()) {
    error = std::make_error_code(std::errc::device_or_resource_busy); return {};
  }
  if (!settings_read_provider_ || !settings_read_provider_->Current(*this)) {
    error = std::make_error_code(std::errc::operation_canceled); return {};
  }
  for (const auto& [xuid, metadata] : account_metadata_) {
    if (!metadata || metadata->xuid() != xuid || !accounts_.contains(xuid) ||
        !metadata->device_id() || retiring_profiles_.contains(xuid)) { continue; }
    std::error_code selected_error;
    if (profile_discovery_conflicts_.contains(xuid) &&
        !CaptureCachedAccount(xuid, selected_error)) { continue; }
    if (device_id && metadata->device_id() != device_id) { continue; }
    out.push_back({xuid, metadata->account(), metadata->device_id()});
  }
  return out;
}

std::vector<uint8_t> ProfileManager::CaptureProfileIcon(uint64_t xuid,
                                                        XTileType type) const {
  std::unique_lock lock(profile_mount_mutex_, std::try_to_lock);
  if (!lock.owns_lock()) {
    return {};
  }
  auto global = xe::global_critical_region::TryAcquire();
  if (!global.owns_lock()) {
    return {};
  }
  const auto roster = CaptureSelectedRoster();
  if (!roster) { return {}; }
  for (uint8_t slot = 0; slot != 4; ++slot) {
    if (roster->slots()[slot].xuid != xuid) { continue; }
    const auto profile = CaptureRosterProfile(slot);
    return profile && profile->xuid() == xuid ? profile->GetProfileIcon(type)
                                            : std::vector<uint8_t>{};
  }
  return {};
}

ProfileGpdWriter ProfileManager::CaptureGpdWriter(uint64_t xuid,
                                                  std::error_code& error) {
  auto model = AcquireProfileGpdModel(xuid, error);
  if (!model || !OrdinaryRosterWriterAvailable(model)) {
    if (!error) { error = std::make_error_code(std::errc::device_or_resource_busy); }
    return {};
  }
  auto lease = AcquireProfileStorageLease(xuid, error);
  if (!lease) {
    return {};
  }
  auto owner = content_manager_.ProfileIntents(lease->source_id());
  const auto volume = content_manager_.Volume(lease->source_id());
  auto recovery = volume && volume->profile_writable() ? volume->recovery() : nullptr;
  if (!owner || owner->source() != lease->source() || !recovery) {
    error = std::make_error_code(std::errc::no_such_device);
    return {};
  }
  if (model->storage() != lease || !OrdinaryRosterWriterAvailable(model)) {
    error = std::make_error_code(std::errc::device_or_resource_busy); return {};
  }
  return {std::move(model), std::move(owner), std::move(recovery), volume->mutations()};
}
ProfileGpdWriter ProfileManager::CaptureGpdWriter(uint8_t slot,
                                                  std::error_code& error) {
  auto model = AcquireProfileGpdModel(slot, error);
  if (!model) {
    return {};
  }
  auto writer = CaptureGpdWriter(model->xuid(), error);
  if (writer.model != model) {
    error = std::make_error_code(std::errc::operation_canceled);
    return {};
  }
  return writer;
}
std::error_code ProfileManager::LoadGpdModel(
    std::shared_ptr<ProfileGpdModelAuthority> model,
    std::shared_ptr<ProfileStorageLease> lease,
    std::shared_ptr<const filesystem::ProfileIntentOwner> owner) {
  if (xe::global_critical_region::is_held_by_current_thread()) {
    return std::make_error_code(std::errc::operation_would_block);
  }
  const auto volume = lease ? content_manager_.Volume(lease->source_id()) : nullptr;
  const auto recovery = volume && volume->profile_writable() ? volume->recovery() : nullptr;
  if (!model || !lease || !owner || !recovery) {
    return std::make_error_code(std::errc::no_such_device);
  }
  std::vector<std::shared_ptr<ProfileGpdModelAuthority>> retired;
  {
    std::unique_lock lock(profile_mount_mutex_, std::try_to_lock);
    if (!lock.owns_lock()) {
      return std::make_error_code(std::errc::device_or_resource_busy);
    }
    retired = retired_gpd_models_;
  }
  for (const auto& old : retired) {
    if (old->xuid() == lease->xuid()) {
      auto error = old->ReleaseRetiredPublication();
      if (error) {
        return error;
      }
    }
  }
  const ProfileGpdWriter writer{model, owner, recovery, volume->mutations()};
  if (const auto error = writer.Reload()) { return error; }
  // Reload's cache/model commit guards have ended before the real repair IO.
  // A failed repair cannot publish a false successful native sign-in row.
  const auto& installed = kernel_state_->emulator()->installed_system_source();
  return writer.RepairLegacyPictureKeys(installed ? installed->snapshot() : nullptr);
}

std::error_code ProfileManager::ReloadGpdModel(uint64_t xuid) {
  std::error_code error;
  auto writer = CaptureGpdWriter(xuid, error);
  if (!writer.model) {
    return error;
  }
  auto lease = AcquireProfileStorageLease(xuid, error);
  if (!lease) {
    return error;
  }
  return LoadGpdModel(writer.model, std::move(lease), writer.owner);
}

void ProfileManager::Login(uint64_t xuid, uint8_t slot, bool notify) {
  RetryRosterSettlement();
  std::error_code error; const auto expected = CaptureRosterExpected(error);
  if (error || !xuid || !expected.roster) { return; }
  ProfileRosterDesired desired; desired.notify_host_compatibility = notify;
  for (uint8_t i = 0; i != 4; ++i) { desired.xuids[i] = expected.roster->slots()[i].xuid; }
  if (slot >= 4) {
    slot = XUserIndexAny;
    for (uint8_t i = 0; i != 4; ++i) { if (!desired.xuids[i]) { slot = i; break; } }
    if (slot >= 4) { return; }
  }
  for (uint8_t i = 0; i != 4; ++i) { if (desired.xuids[i] == xuid) { desired.xuids[i] = 0; } }
  desired.xuids[slot] = xuid;
  const auto result = AdoptRoster(desired, expected);
  if (result.error) { XELOGW("Local roster sign-in refused ({})", result.error.message()); }
}
void ProfileManager::Logout(uint8_t slot, bool notify, bool cancel_preparing) {
  // Cancellation of an entered native operation belongs to that actual owner;
  // this old flag never discards its producer or publishes a host rollback.
  (void)cancel_preparing;
  RetryRosterSettlement();
  std::error_code error; const auto expected = CaptureRosterExpected(error);
  if (error || slot >= 4 || !expected.roster) { return; }
  ProfileRosterDesired desired; desired.notify_host_compatibility = notify;
  for (uint8_t i = 0; i != 4; ++i) { desired.xuids[i] = expected.roster->slots()[i].xuid; }
  desired.xuids[slot] = 0;
  const auto result = AdoptRoster(desired, expected);
  if (result.error) { XELOGW("Local roster sign-out refused ({})", result.error.message()); }
}
void ProfileManager::LoginMultiple(const std::map<uint8_t, uint64_t>& profiles) {
  RetryRosterSettlement();
  std::error_code error; const auto expected = CaptureRosterExpected(error);
  if (error || !expected.roster) { return; }
  ProfileRosterDesired desired;
  for (uint8_t i = 0; i != 4; ++i) { desired.xuids[i] = expected.roster->slots()[i].xuid; }
  for (const auto& [slot, xuid] : profiles) {
    if (slot >= 4) { return; }
    if (xuid) { for (uint8_t i = 0; i != 4; ++i) { if (desired.xuids[i] == xuid) { desired.xuids[i] = 0; } } }
    desired.xuids[slot] = xuid;
  }
  const auto result = AdoptRoster(desired, expected);
  if (result.error) { XELOGW("Local batch roster refused ({})", result.error.message()); }
}

ProfileManager::ProfileDiscovery ProfileManager::FindProfiles() const {
  ProfileDiscovery out;
  // Registry is the only source catalog. Installed readonly content, optical
  // selections and cache roles do not become local profile-producing devices.
  for (const auto device : content_manager_.sources().DeviceIds()) {
    for (const auto& source : content_manager_.sources().Sources(device)) {
      if (source.source.kind != ContentSourceKind::kManaged ||
          source.source.read_only) { continue; }
      if (!source.available || !source.profile_writable || !source.generation ||
          !source.storage || source.storage->retiring() ||
          !source.managed_identity || source.managed_identity->retiring()) {
        out.complete = false;
        continue;
      }
      // Names are discovery hints only. Resolve against this exact retained
      // source generation before the shared mount authenticates Account bytes.
      std::error_code error;
      std::filesystem::directory_iterator it(source.storage->path(), error), end;
      for (; !error && it != end; it.increment(error)) {
        const auto name = xe::path_to_utf8(it->path().filename());
        if (name.size() != 16 || !std::all_of(name.begin(), name.end(), [](char c) {
              return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F');
            })) { continue; }
        const auto xuid = xe::string_util::from_string<uint64_t>(name, true);
        if (!xuid) { continue; }
        std::error_code kind_error;
        if (!it->is_directory(kind_error)) {
          if (kind_error) { out.complete = false; }
          continue;
        }
        const auto relative = ProfileStorageLease::ProfileRelative(xuid);
        const auto path = content_manager_.sources().Resolve(source, relative, false);
        if (!path) { out.complete = false; continue; }
        if (!std::filesystem::is_directory(*path, kind_error)) {
          if (kind_error && kind_error != std::errc::no_such_file_or_directory) {
            out.complete = false;
          }
          continue;
        }
        out.candidates[xuid].push_back(source);
      }
      if (error || source.storage->retiring() || source.managed_identity->retiring()) {
        out.complete = false;
      }
    }
  }
  return out;
}

uint8_t ProfileManager::FindFirstFreeProfileSlot() const {
  const auto roster = CaptureSelectedRoster(); if (!roster) { return XUserIndexAny; }
  for (uint8_t slot = 0; slot != 4; ++slot) { if (!roster->slots()[slot].xuid) { return slot; } }
  return XUserIndexAny;
}
bool ProfileManager::IsAnyProfileSignedIn() const { return GetUsedUserSlots().any(); }
std::bitset<XUserMaxUserCount> ProfileManager::GetUsedUserSlots() const {
  std::bitset<XUserMaxUserCount> used;
  const auto roster = CaptureSelectedRoster();
  if (roster) { for (uint8_t i = 0; i != 4; ++i) { used[i] = bool(roster->slots()[i].xuid); } }
  return used;
}
uint8_t ProfileManager::GetUserIndexAssignedToProfile(uint64_t xuid) const {
  if (!xuid) { return XUserIndexAny; }
  const auto roster = CaptureSelectedRoster();
  if (!roster) { return XUserIndexAny; }
  for (uint8_t i = 0; i != 4; ++i) { if (roster->slots()[i].xuid == xuid) { return i; } }
  return GetUserIndexAssignedToLiveProfile(xuid);
}
uint8_t ProfileManager::GetUserIndexAssignedToLiveProfile(uint64_t xuid) const {
  if (!xuid) { return XUserIndexAny; }
  for (uint8_t i = 0; i != 4; ++i) {
    const auto profile = CaptureRosterProfile(i);
    if (profile && profile->GetOnlineXUID() == xuid) { return i; }
  }
  return XUserIndexAny;
}

std::filesystem::path ProfileManager::GetProfileContentPath(
    const uint64_t xuid, const uint32_t title_id,
    const XContentType content_type) const {
  std::filesystem::path content_root = kernel_state_->emulator()->content_root();
  if (xuid) {
    std::error_code error;
    const auto metadata = CaptureCachedAccount(xuid, error);
    if (!metadata || error || metadata->source_root().empty()) { return {}; }
    content_root = metadata->source_root();
  }
  // Display/navigation path only. Operations still require common retained
  // source/catalog authority; no filesystem observation is performed here.
  std::filesystem::path profile_content_path = content_root / fmt::format("{:016X}", xuid);
  if (title_id != -1 && title_id != 0) {
    profile_content_path =
        profile_content_path / fmt::format("{:08X}", title_id);

    if (content_type != XContentType::kInvalid) {
      profile_content_path =
          profile_content_path /
          fmt::format("{:08X}", static_cast<uint32_t>(content_type));
    }
  }
  return profile_content_path;
}



uint32_t ProfileManager::AllocateProfileCreationResult(
    std::span<const uint8_t> bytes, std::error_code& error) {
  error.clear();
  auto use = account_commit_gate_->Acquire();
  if (!use) { error = std::make_error_code(std::errc::operation_canceled); return 0; }
  return AllocateProfileCreationResultEntered(bytes, error);
}

uint32_t ProfileManager::AllocateProfileCreationResultEntered(
    std::span<const uint8_t> bytes, std::error_code& error) {
  if (bytes.size() != 0xAC0 ||
      global_critical_region::is_held_by_current_thread()) {
    error = std::make_error_code(std::errc::invalid_argument);
    return 0;
  }
  const auto address = kernel_state_->memory()->SystemHeapAlloc(bytes.size());
  if (!address) {
    error = std::make_error_code(std::errc::not_enough_memory);
    return 0;
  }
  if (XFAILED(util::WriteGuestIoBuffer(*kernel_state_->memory(), address, bytes))) {
    kernel_state_->memory()->SystemHeapFree(address);
    error = std::make_error_code(std::errc::bad_address);
    return 0;
  }
  bool registered = false;
  try {
    std::lock_guard lock(profile_creation_mutex_);
    if (profile_creation_results_.size() < 64) {
      registered = profile_creation_results_.emplace(
          address, GuestProfileCreationResult{}).second;
    }
  } catch (const std::bad_alloc&) {}
  if (!registered) {
    kernel_state_->memory()->SystemHeapFree(address);
    error = std::make_error_code(std::errc::not_enough_memory);
    return 0;
  }
  return address;
}

bool ProfileManager::CompleteProfileCreationResult(
    uint32_t address, ProfileCreationResult result) {
  auto use = account_commit_gate_->Acquire();
  if (!use) { return false; }
  return CompleteProfileCreationResultEntered(address, std::move(result));
}

bool ProfileManager::CompleteProfileCreationResultEntered(
    uint32_t address, ProfileCreationResult result) {
  std::lock_guard lock(profile_creation_mutex_);
  const auto found = profile_creation_results_.find(address);
  if (found == profile_creation_results_.end() || found->second.completed) {
    return false;
  }
  found->second.result = result;
  found->second.completed = true;
  return true;
}

void ProfileManager::DiscardProfileCreationResult(uint32_t address) {
  auto use = account_commit_gate_->Acquire();
  if (!use) { return; }
  DiscardProfileCreationResultEntered(address);
}

void ProfileManager::DiscardProfileCreationResultEntered(uint32_t address) {
  bool owned = false;
  {
    std::lock_guard lock(profile_creation_mutex_);
    const auto found = profile_creation_results_.find(address);
    if (found != profile_creation_results_.end() && !found->second.consuming) {
      profile_creation_results_.erase(found);
      owned = true;
    }
  }
  if (owned) { kernel_state_->memory()->SystemHeapFree(address); }
}

X_HRESULT ProfileManager::ProduceProfileCreationResult(
    std::array<uint8_t, 0xAC0> bytes, uint32_t output_address) {
  auto use = account_commit_gate_->Acquire();
  if (!use) { return X_E_FAIL; }
  auto& memory = *kernel_state_->memory();
  if (!output_address || global_critical_region::is_held_by_current_thread() ||
      !util::GuestIoRangeValid(memory, output_address, 4, true)) {
    return X_E_INVALIDARG;
  }
  std::error_code error;
  const auto record = AllocateProfileCreationResultEntered(bytes, error);
  if (!record) { return X_HRESULT_FROM_WIN32(X_ERROR_NOT_ENOUGH_MEMORY); }
  std::array<uint8_t, 4> word{};
  xe::store_and_swap<uint32_t>(word.data(), record);
  if (XFAILED(util::WriteGuestIoBuffer(memory, output_address, word))) {
    DiscardProfileCreationResultEntered(record);
    return X_E_INVALIDARG;
  }
  // This synchronous HLE provider publishes its owned pending result before
  // processing the device DWORD. Retail Schedule may run its native callback
  // before or after its own output store; no native interleaving is asserted.
  ProfileCreationResult result;
  result.xuid = xe::load_and_swap<uint64_t>(bytes.data() + 0xA78);
  const auto process = [&] {
    const uint32_t flags = xe::load_and_swap<uint32_t>(bytes.data());
    const bool high_flag = xe::load_and_swap<uint32_t>(bytes.data() + 0xABC) != 0;
    if ((flags & 1) || high_flag) {
      uint32_t account_flags = xe::load_and_swap<uint32_t>(bytes.data() + 8) | 1;
      if (std::all_of(bytes.begin() + 0x40, bytes.begin() + 0x44,
                      [](uint8_t value) { return value == 0; })) {
        account_flags |= 0x10000000u;
      }
      if (high_flag) {
        account_flags |= 0x40000000u;
        if (flags & 0x40000000u) { account_flags |= 0x80000000u; }
      }
      xe::store_and_swap<uint32_t>(bytes.data() + 8, account_flags);
      if (XFAILED(util::WriteGuestIoBuffer(memory, record + 8,
              std::span<const uint8_t>(bytes).subspan(8, 4)))) {
        result.status = X_ERROR_INVALID_PARAMETER; return;
      }
    }
    // Native17559 generates only for bit0 with original bit31 clear; it
    // replaces even a nonzero supplied XUID. Other paths require an E* XUID.
    if ((flags & 1) && !high_flag) {
      result.xuid = GenerateXuid();
      xe::store_and_swap<uint64_t>(bytes.data() + 0xA78, result.xuid);
      if (XFAILED(util::WriteGuestIoBuffer(memory, record + 0xA78,
              std::span<const uint8_t>(bytes).subspan(0xA78, 8)))) {
        result.status = X_ERROR_INVALID_PARAMETER; return;
      }
    } else if ((result.xuid & 0xF000000000000000ull) != 0xE000000000000000ull) {
      result.status = X_ERROR_INVALID_PARAMETER; return;
    }
    if (flags & 2) {
      // This paired HLE provider creates local profiles. The native machine
      // logon/provisioning phase is unavailable, not an invalid native flag or
      // an invented disconnected/online success. Preserve all supplied data.
      result.status = X_ERROR_NOT_SUPPORTED; return;
    }
    const uint32_t device_address = xe::load_and_swap<uint32_t>(bytes.data() + 4);
    if (XFAILED(util::ReadGuestIoBuffer(memory, device_address, word))) {
      result.status = X_ERROR_INVALID_PARAMETER; return;
    }
    if (!xe::load_and_swap<uint32_t>(word.data())) {
      if (!util::GuestIoRangeValid(memory, device_address, 4, true)) {
        result.status = X_ERROR_INVALID_PARAMETER; return;
      }
      // Use the same real default-device selection as the registered content
      // provider. Its guest write precedes creation and is never rolled back.
      const auto devices = ListStorageDevices(kernel_state_, true);
      if (devices.empty()) { result.status = X_ERROR_DEVICE_NOT_CONNECTED; return; }
      xe::store_and_swap<uint32_t>(word.data(), devices.front().device_id);
      if (XFAILED(util::WriteGuestIoBuffer(memory, device_address, word))) {
        result.status = X_ERROR_INVALID_PARAMETER; return;
      }
    }
    // Retail reloads after the default-device phase. No translated pointer
    // or reference to caller memory crosses real provider/filesystem work.
    if (XFAILED(util::ReadGuestIoBuffer(memory, device_address, word))) {
      result.status = X_ERROR_INVALID_PARAMETER; return;
    }
    const uint32_t device_id = xe::load_and_swap<uint32_t>(word.data());
    X_XAMACCOUNTINFO account{};
    std::memcpy(&account, bytes.data() + 8, sizeof(account));
    result = CreateProfileDetailed(account, result.xuid, false, 0, 0, device_id);
  };
  try { process(); }
  catch (...) { result.status = X_ERROR_FUNCTION_FAILED; }
  // Complete the already-issued result under this operation's original Use,
  // even if admission closed while the genuine transaction was in flight.
  if (!CompleteProfileCreationResultEntered(record, result)) {
    DiscardProfileCreationResultEntered(record);
    return X_E_FAIL;
  }
  return X_E_SUCCESS;
}

X_HRESULT ProfileManager::ConsumeProfileCreationResult(
    uint32_t address, uint32_t xuid_destination) {
  auto use = account_commit_gate_->Acquire();
  if (!use) { return X_E_FAIL; }
  if (!address || global_critical_region::is_held_by_current_thread()) {
    return X_E_INVALIDARG;
  }
  ProfileCreationResult result;
  {
    std::lock_guard lock(profile_creation_mutex_);
    const auto found = profile_creation_results_.find(address);
    if (found == profile_creation_results_.end() || found->second.consuming) {
      return X_E_INVALIDARG;
    }
    if (!found->second.completed) { return 0x8000000Au; }
    found->second.consuming = true;
    result = found->second.result;
  }
  // Retail checks completion before touching the terminal XUID destination.
  // Keep the issued record on a bad terminal pointer so the caller can retry.
  const auto cancel_consumption = [&] {
    std::lock_guard lock(profile_creation_mutex_);
    profile_creation_results_.at(address).consuming = false;
  };
  if (!xuid_destination ||
      !util::GuestIoRangeValid(*kernel_state_->memory(), xuid_destination, 8, true) ||
      util::GuestIoRangesOverlap(*kernel_state_->memory(), address, 0xAC0,
                                 xuid_destination, 8)) {
    cancel_consumption();
    return X_E_INVALIDARG;
  }
  std::array<uint8_t, 8> bytes{};
  xe::store_and_swap<uint64_t>(bytes.data(), result.xuid);
  if (XFAILED(util::WriteGuestIoBuffer(*kernel_state_->memory(),
                                      xuid_destination, bytes))) {
    cancel_consumption();
    return X_E_INVALIDARG;
  }
  {
    std::lock_guard lock(profile_creation_mutex_);
    profile_creation_results_.erase(address);
  }
  kernel_state_->memory()->SystemHeapFree(address);
  return X_HRESULT_FROM_WIN32(result.status);
}

ProfileManager::ProfileCreationResult ProfileManager::CreateProfileDetailed(
    const std::string& gamertag, bool autologin, bool default_xuid,
    uint32_t avatar_preset, uint32_t stock_picture) {
  ProfileCreationResult result;
  if (!IsGamertagValid(gamertag)) {
    result.status = X_ERROR_INVALID_PARAMETER;
    return result;
  }
  X_XAMACCOUNTINFO account{};
  string_util::copy_and_swap_truncating(account.gamertag, xe::to_utf16(gamertag),
                                       xe::countof(account.gamertag));
  return CreateProfileDetailed(account,
      default_xuid ? 0xB13EBABEBABEBABEull : GenerateXuid(), autologin,
      avatar_preset, stock_picture);
}

ProfileManager::ProfileCreationResult ProfileManager::CreateProfileDetailed(
    const X_XAMACCOUNTINFO& account, uint64_t xuid, bool autologin,
    uint32_t avatar_preset, uint32_t stock_picture,
    std::optional<uint32_t> selected_device) {
  ProfileCreationResult result;
  result.xuid = xuid;
  auto use = account_commit_gate_->Acquire();
  if (!use) { result.status = X_ERROR_CANCELLED; return result; }
  if (!xuid || !IsGamertagValid(account.GetGamertagString())) {
    result.status = X_ERROR_INVALID_PARAMETER;
    return result;
  }
  if (global_critical_region::is_held_by_current_thread()) {
    result.status = X_ERROR_BUSY;
    return result;
  }
  {
    std::unique_lock lock(profile_mount_mutex_, std::try_to_lock);
    if (!lock.owns_lock() || preparing_profiles_.contains(xuid) ||
        retiring_profiles_.contains(xuid)) {
      result.status = X_ERROR_BUSY;
      return result;
    }
    if (accounts_.contains(xuid)) {
      result.status = X_ERROR_ALREADY_EXISTS;
      return result;
    }
    preparing_profiles_.insert(xuid);
  }
  auto release = [this, xuid](void*) {
    std::lock_guard lock(profile_mount_mutex_);
    preparing_profiles_.erase(xuid);
  };
  std::unique_ptr<void, decltype(release)> reservation(
      reinterpret_cast<void*>(1), release);
  // An explicit caller-selected device is resolved once. Retain its actual
  // generation through recipe construction and durable publication.
  std::optional<ContentSourceState> source;
  if (selected_device) {
    source = content_manager_.sources().WritableSource(*selected_device);
    if (!source) {
      const auto candidates = content_manager_.sources().Sources(*selected_device);
      const bool has_writable = std::any_of(candidates.begin(), candidates.end(),
          [](const auto& candidate) { return !candidate.source.read_only; });
      result.status = !has_writable && std::any_of(candidates.begin(), candidates.end(),
          [](const auto& candidate) { return candidate.available; })
          ? X_ERROR_ACCESS_DENIED : X_ERROR_DEVICE_NOT_CONNECTED;
      return result;
    }
    if (!source->profile_writable) { result.status = X_ERROR_ACCESS_DENIED; return result; }
    if (!source->available || !content_manager_.sources().CaptureStorageVolume(*source)) {
      result.status = X_ERROR_DEVICE_NOT_CONNECTED;
      return result;
    }
  }
  const auto& installed = kernel_state_->emulator()->installed_system_source();
  if (!installed) {
    result.status = X_ERROR_FILE_NOT_FOUND;
    return result;
  }
  std::error_code error;
  auto recipe = BuildInitialLocalProfile(account, xuid, installed->snapshot(),
                                         avatar_preset, stock_picture, error);
  if (!recipe) {
    XELOGW("Profile {:016X}: complete initial recipe unavailable ({})", xuid,
           error.message());
    result.status = X_ERROR_FUNCTION_FAILED;
    return result;
  }
  // Existing host callers retain their hdd default and its original recipe
  // ordering; the registered ABI passes an explicit resolved device instead.
  if (!selected_device) { source = content_manager_.sources().Source("hdd"); }
  if (!source || !source->available || !source->profile_writable) {
    result.status = X_ERROR_DEVICE_NOT_CONNECTED;
    return result;
  }
  recipe->descriptor.device_id = source->source.device_id;
  // Publication takes its own copy. Retain this exact origin for the first
  // authenticated Account read, before any cached metadata can name it.
  result.status = content_manager_.CreateInitialPackage(
      xuid, recipe->descriptor, recipe->members, source);
  if (result.status != X_ERROR_SUCCESS) {
    XELOGW("Profile {:016X}: complete initial publication failed ({:08X})",
           xuid, result.status);
    return result;
  }
  result.published = true;
  // Release creation admission before ordinary discovery and sign-in enter
  // their own independent storage/model reservations.
  reservation.reset();
  result.discovered = LoadAccountInternal(xuid, &*source);
  // During an actual title/native root, automatic identity publication must
  // go through its native provider join. Preserve the complete unsigned
  // profile instead of mutating an active slot and pretending it refreshed.
  if (result.discovered && autologin &&
      !kernel_state_->emulator()->is_title_open()) {
    Login(xuid);
    result.signed_in = bool(CaptureProfile(xuid));
  }
  if (!result.discovered || (autologin && !result.signed_in)) {
    XELOGW("Profile {:016X} created durably; {} must be retried", xuid,
           result.discovered ? "sign-in" : "discovery");
  }
  return result;
}

bool ProfileManager::CreateProfile(const std::string gamertag, bool autologin,
                                   bool default_xuid) {
  return CreateProfileDetailed(gamertag, autologin, default_xuid).published;
}

bool ProfileManager::CreateProfile(const X_XAMACCOUNTINFO* account_info,
                                   uint64_t xuid) {
  return account_info && CreateProfileDetailed(*account_info, xuid).published;
}

const std::map<uint64_t, X_XAMACCOUNTINFO>* ProfileManager::GetAccounts() {
  thread_local std::map<uint64_t, X_XAMACCOUNTINFO> copied;
  std::unique_lock lock(profile_mount_mutex_, std::try_to_lock);
  if (!lock.owns_lock()) { copied.clear(); return &copied; }
  const auto roster = CaptureSelectedRoster();
  if (!roster) { copied.clear(); return &copied; }
  copied = accounts_;
  for (const auto xuid : profile_discovery_conflicts_) { copied.erase(xuid); }
  // Already selected exact rows remain factual even when a new unscoped
  // choice is ambiguous. Discovery never signs them out or retires native work.
  for (const auto& row : roster->slots()) { if (row.xuid) {
    const auto selected = CaptureRosterAccount(row.xuid);
    if (!selected) { copied.clear(); return &copied; }
    copied.insert_or_assign(row.xuid, selected->account());
  } }
  return &copied;
}
const X_XAMACCOUNTINFO* ProfileManager::GetAccount(const uint64_t xuid) {
  thread_local X_XAMACCOUNTINFO copied;
  std::unique_lock lock(profile_mount_mutex_, std::try_to_lock);
  if (!lock.owns_lock()) { return nullptr; }
  const auto roster = CaptureSelectedRoster();
  if (!roster) { return nullptr; }
  for (const auto& row : roster->slots()) { if (row.xuid == xuid) {
    const auto selected = CaptureRosterAccount(xuid);
    if (!selected) { return nullptr; }
    copied = selected->account(); return &copied;
  } }
  const auto account = accounts_.find(xuid);
  if (account == accounts_.end() || profile_discovery_conflicts_.contains(xuid)) { return nullptr; }
  copied = account->second;
  return &copied;
}

std::shared_ptr<const ProfileAccountMetadata> ProfileManager::CaptureCachedAccount(
    uint64_t xuid, std::error_code& error) const {
  error.clear();
  std::unique_lock lock(profile_mount_mutex_, std::try_to_lock);
  if (!lock.owns_lock()) {
    error = std::make_error_code(std::errc::device_or_resource_busy);
    return {};
  }
  const auto found = account_metadata_.find(xuid);
  if (found == account_metadata_.end()) {
    error = std::make_error_code(std::errc::no_such_file_or_directory);
    return {};
  }
  if (profile_discovery_conflicts_.contains(xuid)) {
    const auto selected = CaptureRosterAccount(xuid);
    const auto held = account_snapshots_.find(xuid);
    const auto& metadata = *found->second;
    const auto storage = selected ? selected->identity()->storage() : nullptr;
    if (!selected || selected->identity()->retired() || !storage || storage->retiring() ||
        held == account_snapshots_.end() || held->second->token() != selected->token() ||
        metadata.xuid() != xuid || metadata.device_id() != storage->device_id() ||
        metadata.source_id() != storage->source_id() ||
        metadata.source_generation() != storage->source_generation() ||
        metadata.source_root() != storage->source()->path() ||
        std::memcmp(&metadata.account(), &selected->account(), sizeof(X_XAMACCOUNTINFO))) {
      error = std::make_error_code(std::errc::file_exists);
      return {};
    }
    // These are the same already-authenticated committed token/Account and
    // owning origin. No IO, fresh source selection or ordinary-read bypass.
  }
  return found->second;
}

bool ProfileManager::CreateAccount(const uint64_t xuid,
                                   const std::string gamertag) {
  X_XAMACCOUNTINFO account = {};
  const std::u16string gamertag_u16 = xe::to_utf16(gamertag);

  string_util::copy_and_swap_truncating(account.gamertag, gamertag_u16,
                                        xe::countof(account.gamertag));

  const bool result = WriteAccount(xuid, &account);
  DismountProfile(xuid);

  return result;
}

bool ProfileManager::CreateAccount(const uint64_t xuid,
                                   const X_XAMACCOUNTINFO* account) {
  const bool result = WriteAccount(xuid, account);
  DismountProfile(xuid);
  return result;
}

std::shared_ptr<HostProfileAccountEdit> ProfileManager::PrepareAccountEdit(
    uint64_t xuid, ProfileAccountFieldEdits edits, std::error_code& error) {
  error = edits.Validate();
  if (error) { return {}; }
  if (!xuid || xe::global_critical_region::is_held_by_current_thread()) {
    error = std::make_error_code(std::errc::invalid_argument); return {};
  }
  auto entered = account_commit_gate_->Acquire();
  if (!entered) {
    error = std::make_error_code(std::errc::operation_canceled); return {};
  }
  ReapAccountEdits();
  // Host editors select an authenticated known row. This API cannot create an
  // absent Account or turn an unreadable payload into an empty profile.
  if (!CaptureCachedAccount(xuid, error)) { return {}; }
  auto state = std::make_shared<HostProfileAccountEdit::State>();
  state->gate = account_commit_gate_; state->xuid = xuid;
  state->edits = std::move(edits);
  state->mount_path = fmt::format(kDefaultMountFormat, xuid) + ":";
  auto handle = std::shared_ptr<HostProfileAccountEdit>(new HostProfileAccountEdit(state));
  state->handle = handle;
  // Preallocate its map node outside the actual metadata admission boundary.
  decltype(account_edits_) pending;
  pending.emplace(0, state);
  {
    std::unique_lock lock(profile_mount_mutex_, std::try_to_lock);
    if (!lock.owns_lock() || account_edits_.size() >= 64 ||
        !next_account_edit_ || next_account_edit_ == UINT64_MAX) {
      error = std::make_error_code(std::errc::device_or_resource_busy); return {};
    }
    state->serial = next_account_edit_++;
    auto node = pending.extract(0); node.key() = state->serial;
    account_edits_.insert(std::move(node));
  }
  std::unique_lock serial(state->mutex);
  state->outcome.phase = ProfileAccountEditPhase::kBeforePublicationFailure;
  state->outcome.outcome_known = true;
  if (!MountProfileInternal(xuid, {}, {}, nullptr, nullptr, state.get())) {
    state->outcome.error = error = std::make_error_code(std::errc::device_or_resource_busy);
    return {};  // Handle destruction cancels after this serial guard releases.
  }
  auto identity = CaptureAccountIdentity(xuid, error);
  state->expected = identity ? ProfileAccountSnapshot::Load(identity, error) : nullptr;
  if (!state->expected) { state->outcome.error = error; return {}; }
  const auto storage = identity->storage();
  if (storage->device() != state->device || storage->view() != state->view ||
      storage->source() != state->source ||
      storage->guest_generation() != state->guest_generation ||
      storage->managed_identity() != state->managed_identity ||
      storage->source_generation() != state->source_generation) {
    state->outcome.error = error = std::make_error_code(std::errc::operation_canceled);
    return {};
  }
  const auto volume = content_manager_.Volume(storage->source_id());
  state->recovery = volume && volume->profile_writable() ? volume->recovery() : nullptr;
  if (!state->recovery) {
    state->outcome.error = error = std::make_error_code(std::errc::read_only_file_system);
    return {};
  }
  error = state->edits.Apply(state->expected->account(), state->candidate);
  if (error) { state->outcome.error = error; return {}; }
  state->outcome = {};
  state->outcome.phase = ProfileAccountEditPhase::kPrepared;
  state->outcome.outcome_known = true;
  state->outcome.mount_release_pending = true;
  return handle;
}

bool ProfileManager::ReleaseAccountEditMount(HostProfileAccountEdit::State& state) {
  if (state.mounted) {
    if (!DismountProfileInternal(state.mount_path, {}, &state)) {
      state.outcome.mount_release_pending = true; return false;
    }
    state.mounted = false;
  }
  state.outcome.mount_release_pending = false;
  state.DropIoResources();
  return true;
}

ProfileAccountEditResult ProfileManager::CancelAccountEdit(
    const std::shared_ptr<HostProfileAccountEdit::State>& state) {
  using Phase = ProfileAccountEditPhase;
  if (!state || state->gate != account_commit_gate_ ||
      xe::global_critical_region::is_held_by_current_thread()) {
    return HostProfileAccountEdit::State::Busy();
  }
  std::unique_lock serial(state->mutex, std::try_to_lock);
  if (!serial.owns_lock()) { return HostProfileAccountEdit::State::Busy(); }
  if (!state->Terminal() && !state->attempt.storage.published) {
    state->outcome.phase = Phase::kCanceled;
    state->outcome.error.clear();
    state->outcome.outcome_known = true;
  } else if (!state->Terminal()) {
    auto observed = state->outcome;
    observed.error = std::make_error_code(std::errc::operation_in_progress);
    return observed;  // Actual publication cannot be described as rolled back.
  }
  if (state->Terminal()) { ReleaseAccountEditMount(*state); }
  auto observed = state->outcome;
  if (state->outcome.phase == Phase::kCommitted) {
    observed.error = std::make_error_code(std::errc::operation_not_permitted);
  }
  return observed;
}

ProfileAccountEditResult ProfileManager::CommitAccountEdit(
    const std::shared_ptr<HostProfileAccountEdit::State>& state) {
  using Phase = ProfileAccountEditPhase;
  if (!state || state->gate != account_commit_gate_ ||
      xe::global_critical_region::is_held_by_current_thread()) {
    return HostProfileAccountEdit::State::Busy();
  }
  std::unique_lock serial(state->mutex, std::try_to_lock);
  if (!serial.owns_lock()) { return HostProfileAccountEdit::State::Busy(); }
  if (state->Terminal()) {
    ReleaseAccountEditMount(*state); return state->outcome;
  }
  if (!state->expected || !state->mounted || !state->recovery) {
    state->outcome.phase = Phase::kRetired;
    state->outcome.error = std::make_error_code(std::errc::operation_canceled);
    state->outcome.outcome_known = true;
    ReleaseAccountEditMount(*state); return state->outcome;
  }
  const auto identity = state->expected->identity();
  if (identity->retired() || identity->storage()->retiring()) {
    state->outcome.phase = Phase::kRetired;
    state->outcome.error = std::make_error_code(std::errc::operation_canceled);
    state->outcome.outcome_known = true;
    ReleaseAccountEditMount(*state); return state->outcome;
  }
  ProfileAccountWriter writer(identity, state->recovery);
  if (!state->attempt.storage.published) {
    if (std::memcmp(&state->expected->account(), &state->candidate,
                     sizeof(state->candidate)) == 0) {
      auto error = state->expected->CheckAvailable();
      auto checked = !error ? state->view->ValidateSnapshotBatch(
                                 {state->expected->token()}, error) : nullptr;
      if (!checked) {
        state->outcome.phase = Phase::kBeforePublicationFailure;
        state->outcome.error = error;
        return state->outcome;
      }
      checked.reset();
      state->outcome.phase = Phase::kCommitted;
      state->outcome.error.clear(); state->outcome.unchanged = true;
      state->outcome.outcome_known = true;
      // No physical write, cache mutation or Account revision for a no-op.
      ReleaseAccountEditMount(*state); return state->outcome;
    }
    // SAME captured Account supplies both unedited bytes and durable expected.
    // Never reload expected after building this candidate; never Live-default.
    state->RememberUnpublishedArtifacts();
    state->attempt = writer.Replace(state->expected, state->candidate);
  } else if (!state->attempt.durable) {
    state->attempt = writer.RetryDurability(state->attempt);
  }
  state->ObserveAttempt();
  if (state->attempt.error == std::errc::operation_canceled ||
      (state->attempt.storage.published && state->attempt.storage.durable &&
       !state->attempt.durable)) {
    state->outcome.phase = Phase::kRetired;
    if (!state->outcome.error) {
      state->outcome.error = std::make_error_code(std::errc::state_not_recoverable);
    }
    ReleaseAccountEditMount(*state); return state->outcome;
  }
  if (!state->attempt.durable) {
    state->outcome.phase = state->attempt.storage.published
        ? Phase::kPublishedPendingDurability : Phase::kBeforePublicationFailure;
    if (!state->outcome.error) {
      state->outcome.error = std::make_error_code(std::errc::io_error);
    }
    return state->outcome;
  }
  state->outcome.phase = Phase::kDurablePendingCache;
  std::error_code error;
  if (!PublishAccountCache(state->attempt.durable, error)) {
    state->outcome.error = error;
    if (error == std::errc::operation_canceled ||
        error == std::errc::state_not_recoverable) {
      // Original bytes really reached storage, but this old receipt may not
      // publish over a retired identity or a later actual Account revision.
      state->outcome.phase = Phase::kRetired;
      ReleaseAccountEditMount(*state);
    }
    return state->outcome;
  }
  state->outcome.phase = Phase::kCommitted;
  state->outcome.error.clear(); state->outcome.cache_published = true;
  ReleaseAccountEditMount(*state);
  return state->outcome;
}

void ProfileManager::ReapAccountEdits() {
  if (xe::global_critical_region::is_held_by_current_thread()) { return; }
  auto entered = account_commit_gate_->Acquire();
  if (!entered) { return; }
  std::vector<std::shared_ptr<HostProfileAccountEdit::State>> abandoned;
  abandoned.reserve(64);
  {
    std::unique_lock lock(profile_mount_mutex_, std::try_to_lock);
    if (!lock.owns_lock()) { return; }
    for (const auto& [serial, state] : account_edits_) {
      if (state->handle.expired()) { abandoned.push_back(state); }
    }
  }
  for (const auto& state : abandoned) {
    // No owner/source registry lock survives actual retry or deferred rundown.
    const auto canceled = CancelAccountEdit(state);
    if (canceled.error == std::errc::operation_in_progress) {
      CommitAccountEdit(state);
    }
  }
  // Erase only states whose exact reference really settled. A shared local
  // keeps destruction outside this metadata lock even for the last UI drop.
  std::vector<std::shared_ptr<HostProfileAccountEdit::State>> released;
  released.reserve(abandoned.size());
  {
    std::unique_lock lock(profile_mount_mutex_, std::try_to_lock);
    if (!lock.owns_lock()) { return; }
    for (const auto& state : abandoned) {
      std::unique_lock serial(state->mutex, std::try_to_lock);
      if (serial.owns_lock() && state->Terminal() && !state->mounted) {
        const auto found = account_edits_.find(state->serial);
        if (found != account_edits_.end() && found->second == state) {
          released.push_back(std::move(found->second));
          account_edits_.erase(found);
        }
      }
    }
  }
}

void ProfileManager::CloseAccountEdits() {
  // Actual manager stop, after its counted gate drained. No entered operation
  // or late callback can use PM; escaped handles keep value observations only.
  auto states = std::move(account_edits_);
  for (auto& [serial, state] : states) {
    std::lock_guard lock(state->mutex);
    if (!state->Terminal()) {
      state->outcome.phase = ProfileAccountEditPhase::kRetired;
      state->outcome.error = std::make_error_code(std::errc::operation_canceled);
      state->outcome.outcome_known = true;
    }
    if (state->expected) { state->expected->identity()->Retire(); }
    state->mounted = false;  // This exact issuing PM is actually retiring.
    state->outcome.mount_release_pending = false;
    state->DropIoResources();
  }
}

bool ProfileManager::UpdateAccount(const uint64_t xuid,
                                   const X_XAMACCOUNTINFO* account) {
  if (!account || xe::global_critical_region::is_held_by_current_thread()) {
    return false;
  }
  auto use = account_commit_gate_->Acquire();
  if (!use) { return false; }
  X_XAMACCOUNTINFO updated = *account;
  if (updated.IsLiveEnabled()) { SetLiveDefaults(&updated); }
  // One owned mount reference, even for a signed-in profile. No borrowed
  // UserProfile/GetAccount survives the observing IO or durable replacement.
  if (!MountProfile(xuid)) { return false; }
  const bool written = WriteAccount(xuid, &updated);
  DismountProfile(xuid);
  return written;
}
std::shared_ptr<ProfileAccountIdentity> ProfileManager::CaptureAccountIdentity(
    uint64_t xuid, std::error_code& error) {
  error.clear();
  if (!CaptureSelectedRoster()) {
    error = std::make_error_code(std::errc::device_or_resource_busy); return {};
  }
  if (auto selected = CaptureRosterIdentity(xuid)) { return selected; }
  if (!RosterIdentityRebindAvailable(xuid)) {
    error = std::make_error_code(std::errc::device_or_resource_busy); return {};
  }
  auto storage = AcquireProfileStorageLease(xuid, error);
  if (!storage) { return {}; }
  std::vector<std::shared_ptr<ProfileAccountIdentity>> released_owners;
  std::unique_lock lock(profile_mount_mutex_, std::try_to_lock);
  if (!lock.owns_lock()) {
    error = std::make_error_code(std::errc::device_or_resource_busy);
    return {};
  }
  if (retiring_profiles_.contains(xuid) || preparing_profiles_.contains(xuid) ||
      !RosterIdentityRebindAvailable(xuid)) {
    error = std::make_error_code(std::errc::operation_canceled);
    return {};
  }
  const auto slot = GetUserIndexAssignedToProfile(xuid);
  if (slot < XUserMaxUserCount) {
    const auto found = account_identities_.find(xuid);
    const auto generation = profile_sign_in_generations_.find(xuid);
    if (found == account_identities_.end() ||
        generation == profile_sign_in_generations_.end() ||
        found->second->storage() != storage || found->second->retired() ||
        found->second->slot() != slot ||
        found->second->sign_in_generation() != generation->second) {
      error = std::make_error_code(std::errc::operation_canceled);
      return {};
    }
    return found->second;
  }
  if (auto old = account_identities_.find(xuid); old != account_identities_.end()) {
    if (old->second->slot() < XUserMaxUserCount) {
      // A fixed-old or following native cohort may intentionally exclude a
      // different committed row. Reading that XUID as signed-out cannot retire
      // another cohort's real signed-in producer or remint it as unsigned.
      error = std::make_error_code(std::errc::device_or_resource_busy); return {};
    }
    if (old->second->storage() == storage && old->second->slot() == 0xFF &&
        !old->second->retired()) { return old->second; }
    old->second->Retire();
    if (!old->second->writes_drained()) {
      error = std::make_error_code(std::errc::device_or_resource_busy);
      return {};
    }
    account_identities_.erase(old);
  }
  for (const auto& retired : retired_account_identities_) {
    if (retired->storage()->xuid() == xuid && !retired->writes_drained()) {
      error = std::make_error_code(std::errc::device_or_resource_busy);
      return {};
    }
  }
  for (auto& old : retired_account_identities_) {
    if (old->writes_drained()) { released_owners.push_back(std::move(old)); }
  }
  std::erase(retired_account_identities_, nullptr);
  auto identity = ProfileAccountIdentity::Capture(std::move(storage), 0xFF, 0, error);
  if (identity) { account_identities_.insert_or_assign(xuid, identity); }
  return identity;
}
std::shared_ptr<const ProfileAccountSnapshot>
ProfileManager::CaptureDurableAccount(uint64_t xuid, std::error_code& error) {
  auto use = account_commit_gate_->Acquire();
  if (!use) {
    error = std::make_error_code(std::errc::operation_canceled);
    return {};
  }
  auto identity = CaptureAccountIdentity(xuid, error);
  return identity ? ProfileAccountSnapshot::Load(std::move(identity), error)
                  : nullptr;
}
std::shared_ptr<const NativeLogonAccountInputs>
ProfileManager::PrepareNativeLogonAccounts(std::error_code& error) {
  error.clear();
  if (!CanonicalRosterAccountsAvailable()) {
    error = std::make_error_code(std::errc::device_or_resource_busy); return {};
  }
  auto use = account_commit_gate_->Acquire();
  if (!use) {
    error = std::make_error_code(std::errc::operation_canceled);
    return {};
  }
  if (xe::global_critical_region::is_held_by_current_thread()) {
    error = std::make_error_code(std::errc::operation_would_block);
    return {};
  }
  auto out = std::shared_ptr<NativeLogonAccountInputs>(new NativeLogonAccountInputs);
  {
    std::unique_lock lock(profile_mount_mutex_, std::try_to_lock);
    if (!lock.owns_lock() || !CanonicalRosterAccountsAvailable() ||
        preparing_slots_.any() || !retiring_profiles_.empty()) {
      error = std::make_error_code(std::errc::device_or_resource_busy);
      return {};
    }
    out->stamp_ = account_publication_;
    out->publication_ = account_publication_->current();
    if (!out->publication_) {
      error = std::make_error_code(std::errc::operation_canceled);
      return {};
    }
    for (const auto& [slot, profile] : logged_profiles_) {
      const auto snapshot = account_snapshots_.find(profile->xuid());
      const auto identity = account_identities_.find(profile->xuid());
      if (slot >= 4 || snapshot == account_snapshots_.end() ||
          identity == account_identities_.end() ||
          snapshot->second->identity() != identity->second ||
          identity->second->slot() != slot || identity->second->retired()) {
        error = std::make_error_code(std::errc::operation_canceled);
        return {};
      }
      out->accounts_[slot] = snapshot->second;
      out->write_serials_[slot] = identity->second->write_serial();
    }
  }
  for (size_t slot = 0; slot != 4; ++slot) {
    const auto& snapshot = out->accounts_[slot];
    if (!snapshot) { continue; }  // An explicit captured empty committed slot.
    if ((error = snapshot->CheckAvailable()) ||
        (error = NativeLogonAccountInputs::EncodeRow(*snapshot,
             std::span<uint8_t>(out->bytes_).subspan(slot * 0x188, 0x188)))) {
      return {};
    }
    auto checked = snapshot->identity()->storage()->view()->ValidateSnapshotBatch(
        {snapshot->token()}, error);
    if (!checked) { return {}; }
  }
  // Exact reserved roots exclude foreign cooperating writers. Each real Account
  // producer advances its retained serial BEFORE IO and keeps its write lease
  // through completion. The unchanged serial + empty ledger check below closes
  // the interval across these per-root validations, including cache-pending IO.
  // This is prepared input, not permission to enter native execution: the
  // later coordinator must hold logical publication ownership and revalidate.
  // No filesystem or profile lock survives return to native/guest execution.
  std::unique_lock lock(profile_mount_mutex_, std::try_to_lock);
  if (!lock.owns_lock() || !CanonicalRosterAccountsAvailable() ||
      !out->OwnershipCurrent() || preparing_slots_.any()) {
    error = std::make_error_code(std::errc::operation_canceled);
    return {};
  }
  return out;
}
bool ProfileManager::PublishAccountCache(
    std::shared_ptr<const ProfileAccountSnapshot> snapshot,
    std::error_code& error, std::optional<uint64_t> discovery_revision) {
  error.clear();
  if (!snapshot) {
    error = std::make_error_code(std::errc::invalid_argument);
    return false;
  }
  if ((error = snapshot->CheckAvailable())) { return false; }
  const auto identity = snapshot->identity();
  const auto lease = identity->storage();
  auto cache = lease->device()->PrepareSnapshotCache({snapshot->token()}, error);
  if (!cache) { return false; }
  // Allocate map nodes before retaining the nonrecursive native storage guard.
  std::map<uint64_t, X_XAMACCOUNTINFO> prepared_account;
  std::map<uint64_t, std::shared_ptr<const ProfileAccountSnapshot>> prepared_snapshot;
  prepared_account.emplace(lease->xuid(), snapshot->account());
  prepared_snapshot.emplace(lease->xuid(), snapshot);
  std::map<uint64_t, std::shared_ptr<const ProfileAccountMetadata>> prepared_metadata;
  prepared_metadata.emplace(lease->xuid(),
      std::make_shared<const ProfileAccountMetadata>(*snapshot));
  auto prepared_profile_account =
      std::make_shared<const X_XAMACCOUNTINFO>(snapshot->account());
  std::shared_ptr<const ProfileAccountSnapshot> displaced_roster_account;
  auto checked = lease->view()->ValidateSnapshotBatch({snapshot->token()}, error);
  if (!checked) { return false; }
  std::shared_ptr<const filesystem::HostStorageSnapshotBatch> batch(std::move(checked));
  bool live_changed = false;
  uint32_t used_slots = 0;
  {
    // No observing IO or guest callbacks while storage ownership is held.
    // Existing manager ordering is metadata -> nonwaiting namespace.
    std::unique_lock lock(profile_mount_mutex_, std::try_to_lock);
    if (!lock.owns_lock()) {
      error = std::make_error_code(std::errc::device_or_resource_busy);
      return false;
    }
    const bool reconcile = discovery_revision &&
        *discovery_revision == profile_discovery_revision_ &&
        profile_discovery_revision_ != UINT64_MAX;
    const auto known = account_metadata_.find(lease->xuid());
    const bool different_origin = known != account_metadata_.end() &&
        (known->second->source_id() != lease->source_id() ||
         known->second->device_id() != lease->device_id() ||
         known->second->source_root() != lease->source()->path());
    const bool selected_data = profile_discovery_conflicts_.contains(lease->xuid()) && !different_origin &&
        CaptureRosterIdentity(lease->xuid()) == identity;
    if (!reconcile && (different_origin ||
        (profile_discovery_conflicts_.contains(lease->xuid()) && !selected_data))) {
      MarkProfileDiscoveryConflictLocked(lease->xuid());
      error = std::make_error_code(std::errc::file_exists);
      return false;
    }
    const auto slot = GetUserIndexAssignedToProfile(lease->xuid());
    if (identity->retired() || lease->retiring() ||
        retiring_profiles_.contains(lease->xuid()) ||
        preparing_profiles_.contains(lease->xuid()) ||
        (identity->slot() < XUserMaxUserCount
             ? slot != identity->slot() ||
                   !account_identities_.contains(lease->xuid()) ||
                   account_identities_.at(lease->xuid()) != identity
             : slot < XUserMaxUserCount)) {
      error = std::make_error_code(std::errc::operation_canceled);
      return false;
    }
    auto commit = lease->device()->TryCommitSnapshotCache(cache, batch, error);
    if (!commit) { return false; }
    if (auto found = accounts_.find(lease->xuid()); found != accounts_.end()) {
      found->second = snapshot->account();
    } else {
      accounts_.insert(prepared_account.extract(lease->xuid()));
    }
    if (auto found = account_metadata_.find(lease->xuid()); found != account_metadata_.end()) {
      found->second = prepared_metadata.begin()->second;
    } else {
      account_metadata_.insert(prepared_metadata.extract(lease->xuid()));
    }
    if (slot < XUserMaxUserCount) {
      if (auto found = account_snapshots_.find(lease->xuid());
          found != account_snapshots_.end()) {
        found->second = snapshot;
      } else {
        account_snapshots_.insert(prepared_snapshot.extract(lease->xuid()));
      }
      auto& profile = logged_profiles_.at(slot);
      live_changed = profile->IsLiveEnabled() != snapshot->account().IsLiveEnabled();
      profile->account_info_.store(std::move(prepared_profile_account),
                                    std::memory_order_release);
      used_slots = GetUsedUserSlots().to_ulong();
    }
    displaced_roster_account = RefreshRosterAccount(snapshot);
    if (reconcile) { profile_discovery_conflicts_.erase(lease->xuid()); }
    commit->Apply();
    account_publication_->Advance();
  }
  batch.reset();
  displaced_roster_account.reset();
  if (live_changed) {
    kernel_state_->BroadcastNotification(kXNotificationSystemSignInChanged,
                                         used_slots);
  }
  return true;
}
bool ProfileManager::WriteAccount(const uint64_t xuid,
                                  const X_XAMACCOUNTINFO* account) {
  if (!account || xe::global_critical_region::is_held_by_current_thread()) {
    return false;
  }
  std::error_code error;
  auto identity = CaptureAccountIdentity(xuid, error);
  if (!identity) { return false; }
  const auto volume = content_manager_.Volume(identity->storage()->source_id());
  auto recovery = volume && volume->profile_writable() ? volume->recovery() : nullptr;
  if (!recovery) { return false; }
  auto expected = ProfileAccountSnapshot::Load(identity, error);
  ProfileAccountWriter writer(identity, std::move(recovery));
  // Only actual absence permits Account-only creation. Authentication or IO
  // failures cannot be converted to a new default profile.
  ProfileAccountAttempt attempt;
  if (expected) {
    attempt = writer.Replace(std::move(expected), *account);
  } else if (error == std::errc::no_such_file_or_directory) {
    attempt = writer.Create(*account);
  } else {
    return false;
  }
  if (!attempt.durable || !PublishAccountCache(attempt.durable, error)) {
    XELOGW("Profile {:016X}: Account transaction retained ({}, stage={}, recovery={}, published={}, durable={})",
           xuid, attempt.error ? attempt.error.message() : error.message(),
           attempt.storage.stage.string(), attempt.storage.recovery.string(),
           attempt.storage.published, attempt.storage.durable);
    return false;
  }
  return true;
}

bool ProfileManager::SetLiveDefaults(X_XAMACCOUNTINFO* account) const {
  const X_XAMACCOUNTINFO original = *account;

  if (!account->xuid_online) {
    account->xuid_online = GenerateOnlineXuid();
  }
  if (account->GetSubscriptionTier() ==
      X_XAMACCOUNTINFO::AccountSubscriptionTier::kSubscriptionTierNone) {
    account->SetSubscriptionTier(
        X_XAMACCOUNTINFO::AccountSubscriptionTier::kSubscriptionTierGold);
  }
  if (!static_cast<uint32_t>(account->GetCountry())) {
    account->SetCountry(static_cast<XOnlineCountry>(
        kernel_state_->xconfig()->ReadSetting<uint8_t>(XCONFIG_USER_CATEGORY,
                                                       XCONFIG_USER_COUNTRY)));
  }
  if (account->GetLanguage() == XLanguage::kInvalid) {
    account->SetLanguage(
        static_cast<XLanguage>(kernel_state_->xconfig()->ReadSetting<uint32_t>(
            XCONFIG_USER_CATEGORY, XCONFIG_USER_LANGUAGE)));
  }

  return std::memcmp(&original, account, sizeof(X_XAMACCOUNTINFO)) != 0;
}

void ProfileManager::UpdateConfig(const uint64_t xuid, const uint8_t slot) {
  if (slot >= XUserMaxUserCount ||
      xe::global_critical_region::is_held_by_current_thread()) {
    return;
  }
  // Serialize only this observing IO lane. Snapshot ACTUAL current slots after
  // admission so a late old sign-out cannot overwrite a newer sign-in config.
  std::lock_guard serial(profile_config_mutex_);
  std::array<uint64_t, XUserMaxUserCount> current{};
  std::array<std::string, XUserMaxUserCount> source_ids{};
  {
    std::unique_lock lock(profile_mount_mutex_, std::try_to_lock);
    if (!lock.owns_lock()) {
      return;
    }
    const auto roster = CaptureSelectedRoster();
    if (!roster) { return; }
    for (uint8_t slot = 0; slot != 4; ++slot) {
      current[slot] = roster->slots()[slot].xuid;
      source_ids[slot] = roster->slots()[slot].source_id;
    }
  }
  OVERRIDE_PERSIST_string(logged_profile_slot_0_xuid,
                          xe::string_util::to_hex_string(current[0]));
  OVERRIDE_PERSIST_string(logged_profile_slot_1_xuid,
                          xe::string_util::to_hex_string(current[1]));
  OVERRIDE_PERSIST_string(logged_profile_slot_2_xuid,
                          xe::string_util::to_hex_string(current[2]));
  OVERRIDE_PERSIST_string(logged_profile_slot_3_xuid,
                          xe::string_util::to_hex_string(current[3]));
  OVERRIDE_PERSIST_string(logged_profile_slot_0_source_id, source_ids[0]);
  OVERRIDE_PERSIST_string(logged_profile_slot_1_source_id, source_ids[1]);
  OVERRIDE_PERSIST_string(logged_profile_slot_2_source_id, source_ids[2]);
  OVERRIDE_PERSIST_string(logged_profile_slot_3_source_id, source_ids[3]);
  config::SaveConfig();
}

bool ProfileManager::DeleteProfile(const uint64_t xuid) {
  if (xe::global_critical_region::is_held_by_current_thread()) {
    return false;
  }
  ReapReleasedProfileReads();
  RetryRosterSettlement();
  std::error_code roster_error;
  const auto expected = CaptureRosterExpected(roster_error);
  if (roster_error || !expected.roster) { return false; }

  ProfileDeletionReservation deletion;
  {
    std::unique_lock lock(profile_mount_mutex_, std::try_to_lock);
    if (!lock.owns_lock() || !accounts_.contains(xuid) ||
        profile_discovery_conflicts_.contains(xuid) ||
        preparing_profiles_.contains(xuid) || retiring_profiles_.contains(xuid)) {
      return false;
    }
    const auto metadata = account_metadata_.find(xuid);
    if (metadata == account_metadata_.end() || !metadata->second ||
        metadata->second->source_id().empty()) { return false; }
    const auto [entry, inserted] = retiring_profiles_.insert(xuid);
    if (!inserted) { return false; }
    deletion.owner = this;
    deletion.xuid = xuid;
    deletion.retirement = std::addressof(*entry);
    deletion.metadata = metadata->second;
  }
  auto release = [this, &deletion](void*) {
    std::lock_guard lock(profile_mount_mutex_);
    const auto found = retiring_profiles_.find(deletion.xuid);
    if (found != retiring_profiles_.end() &&
        std::addressof(*found) == deletion.retirement) {
      retiring_profiles_.erase(found);
    }
  };
  std::unique_ptr<void, decltype(release)> reservation(
      reinterpret_cast<void*>(1), release);
  const auto selected_metadata = deletion.metadata;
  const auto source = content_manager_.sources().Source(selected_metadata->source_id());
  const auto owner = content_manager_.ProfileIntents(selected_metadata->source_id());
  if (!source || !source->available || !source->profile_writable ||
      !source->storage || !source->managed_identity || !owner ||
      source->generation != selected_metadata->source_generation() ||
      source->source.device_id != selected_metadata->device_id() ||
      source->managed_identity->source()->path() != selected_metadata->source_root()) {
    return false;
  }
  deletion.source = std::make_shared<const ContentSourceState>(*source);

  ProfileRosterDesired desired;
  bool signed_in = false;
  for (uint8_t slot = 0; slot != 4; ++slot) {
    desired.xuids[slot] = expected.roster->slots()[slot].xuid;
    if (desired.xuids[slot] == xuid) { desired.xuids[slot] = 0; signed_in = true; }
  }
  if (signed_in) {
    // Only this admitted exact-origin deletion may remove its own reserved
    // row. Public/native roster changes still refuse every retirement.
    const auto outcome = AdoptRosterInternal(desired, expected, &deletion);
    if (outcome.error || outcome.phase != ProfileRosterPhase::kCommitted) { return false; }
  }
  const auto relative = ProfileStorageLease::ProfileRelative(xuid);
  uint8_t slot;
  std::vector<std::shared_ptr<ProfileGpdModelAuthority>> retired;
  {
    std::lock_guard lock(profile_mount_mutex_);
    const auto entry = retiring_profiles_.find(xuid);
    const auto metadata = account_metadata_.find(xuid);
    if (entry == retiring_profiles_.end() ||
        std::addressof(*entry) != deletion.retirement ||
        metadata == account_metadata_.end() || metadata->second != selected_metadata) {
      return false;
    }
    slot = GetUserIndexAssignedToProfile(xuid);
  }
  if (slot < XUserMaxUserCount) { return false; }
  {
    std::lock_guard lock(profile_mount_mutex_);
    for (const auto& [name, mount] : profile_mounts_) {
      if (mount.xuid == xuid) {
        return false;
      }
    }
    for (const auto& identity : retired_account_identities_) {
      if (identity->storage()->xuid() == xuid && !identity->writes_drained()) {
        return false;
      }
    }
    retired = retired_gpd_models_;
  }
  for (const auto& model : retired) {
    if (model->xuid() == xuid && model->ReleaseRetiredPublication()) {
      return false;
    }
  }
  std::error_code error;
  const auto rows = GpdDurableIntentStore::Scan(*owner, error);
  if (error) {
    return false;
  }
  for (const auto& row : rows) {
    if (row.binding.profile_relative == relative && !row.done_record_valid) {
      return false;
    }
  }
  // Profile-only deletion: NEVER remove Content/XUID (it owns game saves).
  // Shared no-follow/preflight/conflict admission handles the exact managed
  // profile package; pending redo must be completed before this operation.
  error = source->storage->Remove(relative, true);
  if (error) {
    XELOGW("Profile-only deletion failed/incomplete ({})", error.message());
    return false;
  }
  {
    std::lock_guard lock(profile_mount_mutex_);
    auto namespace_lock = xe::global_critical_region::TryAcquire();
    if (!namespace_lock.owns_lock()) { return false; }
    accounts_.erase(xuid);
    account_snapshots_.erase(xuid);
    account_metadata_.erase(xuid);
    account_publication_->Advance();
    std::erase_if(retired_account_identities_, [xuid](const auto& identity) {
      return identity->storage()->xuid() == xuid && identity->writes_drained();
    });
    std::erase_if(retired_gpd_models_,
                  [xuid](const auto& model) { return model->xuid() == xuid; });
  }
  return true;
}

std::vector<ScannedTitleInfo> ProfileManager::ScanAllProfilesForTitles() const {
  std::map<uint32_t, ScannedTitleInfo> titles_by_id;
  if (xe::global_critical_region::is_held_by_current_thread()) {
    return {};
  }
  const auto source = content_manager_.sources().Source("hdd");
  const auto owner = content_manager_.ProfileIntents("hdd");
  if (!source || !source->storage || !source->managed_identity || !owner) {
    return {};
  }
  std::error_code scan_error;
  const auto intents = GpdDurableIntentStore::Scan(*owner, scan_error);
  if (scan_error) {
    return {};
  }

  auto content_root = kernel_state_->emulator()->content_root();
  auto profiles_directory = xe::filesystem::FilterByName(
      xe::filesystem::ListDirectories(content_root),
      std::regex("[0-9A-F]{16}"));

  for (const auto& profile_dir : profiles_directory) {
    const std::string profile_xuid = xe::path_to_utf8(profile_dir.name);
    if (profile_xuid == fmt::format("{:016X}", 0)) {
      continue;  // Skip shared content directory
    }

    std::filesystem::path dashboard_gpd_path =
        profile_dir.path / profile_dir.name / kDashboardStringID /
        fmt::format("{:08X}", static_cast<uint32_t>(XContentType::kProfile)) /
        profile_dir.name / fmt::format("{:08X}.gpd", kDashboardID);

    const auto xuid = xe::string_util::from_string<uint64_t>(profile_xuid, true);
    const auto relative = dashboard_gpd_path.parent_path().lexically_relative(
        source->storage->path());
    if (std::any_of(intents.begin(), intents.end(), [&](const auto& row) {
          return row.binding.profile_relative == relative &&
                 !row.done_record_valid;
        })) {
      continue;  // Offline migration cannot bypass pending redo; live load
                 // replays it.
    }
    auto profile_view = source->storage->Subroot(relative, false, scan_error);
    if (!profile_view) {
      continue;
    }
    auto load = GpdSnapshotStore::Load({xuid, source->generation, kDashboardID},
                                       profile_view);
    if (!load.readable() ||
        ValidateProfileGpdRecords(load.token->bytes(), true)) {
      continue;
    }
    std::vector<uint8_t> gpd_data;
    {
      auto checked =
          profile_view->ValidateSnapshotBatch({load.token}, scan_error);
      if (!checked) {
        continue;
      }
      gpd_data.assign(load.token->bytes().begin(), load.token->bytes().end());
    }

    GpdInfoProfile dashboard_gpd(gpd_data);
    if (!dashboard_gpd.IsValid()) {
      continue;
    }

    auto titles_info = dashboard_gpd.GetTitlesInfo();
    for (const auto& title_info : titles_info) {
      uint32_t title_id = title_info->title_id;

      time_t last_played = 0;
      if (title_info->last_played.is_valid()) {
        auto last_played_tp = chrono::WinSystemClock::to_sys(
            title_info->last_played.to_time_point());
        last_played = std::chrono::system_clock::to_time_t(last_played_tp);
      }

      if (titles_by_id.find(title_id) != titles_by_id.end()) {
        // Keep most recent timestamp across profiles
        if (last_played > titles_by_id[title_id].last_run_time) {
          titles_by_id[title_id].last_run_time = last_played;
        }
        continue;
      }

      std::string title_name =
          xe::to_utf8(dashboard_gpd.GetTitleName(title_id));
      if (!title_name.empty() && title_name.back() == '\0') {
        title_name.pop_back();
      }

      std::filesystem::path path_to_file;
      auto gpd_path = dashboard_gpd.GetTitlePath(title_id);
      if (gpd_path.has_value()) {
        path_to_file = *gpd_path;
      }

      auto all_discs = dashboard_gpd.GetTitleDiscs(title_id);
      if (all_discs.size() > 1) {
        std::sort(
            all_discs.begin(), all_discs.end(),
            [](const GpdInfoProfile::DiscInfo& a,
               const GpdInfoProfile::DiscInfo& b) { return a.path < b.path; });
      }

      titles_by_id[title_id] = {title_id, title_name, path_to_file, all_discs,
                                last_played};
    }
  }

  std::vector<ScannedTitleInfo> result;
  for (auto& [title_id, title] : titles_by_id) {
    result.push_back(std::move(title));
  }

  std::sort(result.begin(), result.end(),
            [](const ScannedTitleInfo& a, const ScannedTitleInfo& b) {
              return a.last_run_time > b.last_run_time;
            });

  return result;
}

bool ProfileManager::IsGamertagValid(const std::string gamertag) {
  std::regex pattern(R"(^[A-Za-z][A-Za-z0-9]*( [A-Za-z0-9]+)*$)");

  if (gamertag.length() < 1 || gamertag.length() > 15) {
    return false;
  }

  return std::regex_match(gamertag, pattern);
}

}  // namespace xam
}  // namespace kernel
}  // namespace xe

namespace xe::kernel::util {
X_STATUS GuestIoProfileReadScope::Admit() {
  if (!lock_.try_lock()) { return X_STATUS_CANCELLED; }
  if (!batch_.IdentityCurrentAdmitted()) {
    lock_.unlock(); return X_STATUS_CANCELLED;
  }
  return X_STATUS_SUCCESS;
}
}  // namespace xe::kernel::util
