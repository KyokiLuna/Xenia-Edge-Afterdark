/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2024 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <regex>
#include <new>
#include "xenia/kernel/xam/profile_manager.h"
#include "xenia/kernel/xam/local_profile_initial_recipe.h"
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

namespace xe {
namespace kernel {
namespace xam {

ProfileManager::~ProfileManager() {
  account_commit_gate_->CloseAndDrain();
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
  logged_profiles_.clear();
  accounts_.clear();

  try {
    for (const auto account_xuid : FindProfiles()) { LoadAccount(account_xuid); }
  } catch (...) {
    account_commit_gate_->CloseAndDrain();
    throw;
  }
}

void ProfileManager::InitializeProfiles() {
  if (!cvars::logged_profile_slot_0_xuid.empty()) {
    Login(xe::string_util::from_string<uint64_t>(
              cvars::logged_profile_slot_0_xuid, true),
          0);
  }

  if (!cvars::logged_profile_slot_1_xuid.empty()) {
    Login(xe::string_util::from_string<uint64_t>(
              cvars::logged_profile_slot_1_xuid, true),
          1);
  }

  if (!cvars::logged_profile_slot_2_xuid.empty()) {
    Login(xe::string_util::from_string<uint64_t>(
              cvars::logged_profile_slot_2_xuid, true),
          2);
  }

  if (!cvars::logged_profile_slot_3_xuid.empty()) {
    Login(xe::string_util::from_string<uint64_t>(
              cvars::logged_profile_slot_3_xuid, true),
          3);
  }
}

void ProfileManager::ReloadProfile(const uint64_t xuid) {
  LoadAccount(xuid);
}

void ProfileManager::ReloadProfiles() {
  for (const auto account_xuid : FindProfiles()) {
    LoadAccount(account_xuid);
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

void ProfileManager::SyncProfilesWithConfig() {
  // Failed or unavailable disk reads do not erase authenticated cache state.
  // Explicit managed profile deletion owns cache removal.
  for (const auto account_xuid : FindProfiles()) {
    LoadAccount(account_xuid);
  }

  // Build desired login state from current cvar values
  const std::string* profile_cvars[4] = {
      &cvars::logged_profile_slot_0_xuid, &cvars::logged_profile_slot_1_xuid,
      &cvars::logged_profile_slot_2_xuid, &cvars::logged_profile_slot_3_xuid};

  std::map<uint8_t, uint64_t> desired_profiles;
  for (uint8_t slot = 0; slot < 4; slot++) {
    if (!profile_cvars[slot]->empty()) {
      uint64_t xuid =
          xe::string_util::from_string<uint64_t>(*profile_cvars[slot], true);
      if (xuid != 0) {
        desired_profiles[slot] = xuid;
      }
    }
  }

  // Logout profiles that shouldn't be logged in anymore
  std::vector<uint8_t> slots_to_logout;
  for (const auto& [slot, profile] : logged_profiles_) {
    auto desired_it = desired_profiles.find(slot);
    if (desired_it == desired_profiles.end() ||
        desired_it->second != profile->xuid()) {
      // This slot should be logged out (either empty or different profile)
      slots_to_logout.push_back(slot);
    }
  }
  for (uint8_t slot : slots_to_logout) {
    Logout(slot, false);
  }

  // Login profiles that aren't already logged in
  for (const auto& [slot, xuid] : desired_profiles) {
    auto current_profile = logged_profiles_.find(slot);
    if (current_profile == logged_profiles_.end() ||
        current_profile->second->xuid() != xuid) {
      // This slot needs to be logged in (either empty or different profile)
      Login(xuid, slot, false);
    }
  }

  // Send a single notification after all changes
  kernel_state_->BroadcastNotification(kXNotificationSystemSignInChanged,
                                       GetUsedUserSlots().to_ulong());
}

std::shared_ptr<UserProfile> ProfileManager::CaptureProfile(
    uint64_t xuid) const {
  std::unique_lock lock(profile_mount_mutex_, std::try_to_lock);
  if (!lock.owns_lock()) {
    return {};
  }
  auto global = xe::global_critical_region::TryAcquire();
  if (!global.owns_lock()) {
    return {};
  }
  const auto slot = GetUserIndexAssignedToProfile(xuid);
  const auto found = logged_profiles_.find(slot);
  return found == logged_profiles_.end() ? nullptr : found->second;
}
std::shared_ptr<UserProfile> ProfileManager::CaptureProfile(
    uint8_t slot) const {
  std::unique_lock lock(profile_mount_mutex_, std::try_to_lock);
  if (!lock.owns_lock()) {
    return {};
  }
  auto global = xe::global_critical_region::TryAcquire();
  if (!global.owns_lock()) {
    return {};
  }
  const auto found = logged_profiles_.find(slot);
  return found == logged_profiles_.end() ? nullptr : found->second;
}

UserProfile* ProfileManager::GetProfileLive(const uint64_t xuid) const {
  uint8_t user_index = GetUserIndexAssignedToLiveProfile(xuid);

  if (user_index >= XUserMaxUserCount) {
    return nullptr;
  }

  return GetProfile(user_index);
}

UserProfile* ProfileManager::GetProfile(const uint64_t xuid) const {
  const uint8_t user_index = GetUserIndexAssignedToProfile(xuid);
  if (user_index >= XUserMaxUserCount) {
    return nullptr;
  }

  return GetProfile(user_index);
}

UserProfile* ProfileManager::GetProfile(uint8_t user_index) const {
  if (user_index == XUserIndexLatest) {
    user_index = kernel_state_->emulator()->input_system()->GetLastUsedSlot();
  }

  if (user_index == XUserIndexNone) {
    return nullptr;
  }

  if (user_index == XUserIndexAny) {
    for (uint8_t i = 0; i < XUserMaxUserCount; i++) {
      if (!logged_profiles_.count(i)) {
        continue;
      }

      return logged_profiles_.at(i).get();
    }
  }

  if (!logged_profiles_.count(user_index)) {
    return nullptr;
  }

  return logged_profiles_.at(user_index).get();
}

bool ProfileManager::LoadAccount(const uint64_t xuid) {
  auto use = account_commit_gate_->Acquire();
  if (!use) { return false; }
  const std::string mount_name = fmt::format(kDefaultMountFormat, xuid) + ':';
  if (!MountProfile(xuid)) {
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
  if (!snapshot || !PublishAccountCache(std::move(snapshot), error)) {
    XELOGW("Profile {:016X}: durable Account load refused ({})", xuid,
           error.message());
    return false;
  }
  return true;
}

std::unique_ptr<ProfileManager::ManagedFormatIdleReservation>
ProfileManager::TryReserveIdleForManagedFormat(std::error_code& error) {
  error.clear();
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

bool ProfileManager::MountProfile(const uint64_t xuid, std::string mount_path) {
  if (xe::global_critical_region::is_held_by_current_thread()) {
    return false;
  }
  if (mount_path.empty()) {
    mount_path = fmt::format(kDefaultMountFormat, xuid);
  }
  if (mount_path.back() != ':') {
    mount_path += ':';
  }
  const auto key = xe::utf8::lower_ascii(mount_path);
  // Registry observation and new device enumeration happen outside manager,
  // model and namespace locks. The result owns the exact source generation.
  const auto source = content_manager_.sources().Source("hdd");
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
      auto device = std::dynamic_pointer_cast<vfs::HostPathDevice>(
          fs->AcquireDevice(mount_path));
      auto& held = found->second;
    if (held.xuid != xuid || held.source_generation != source->generation ||
        held.source != source_authority ||
        held.guest_generation != source->storage ||
        held.guest_generation->retiring() || held.view->retiring() ||
          held.managed_identity != source->managed_identity || !device ||
          device->storage() != held.view) {
      return false;
    }
    ++held.references;
    return true;
  }
  if (fs->AcquireDevice(mount_path)) {
      return false;
    }
  }
  const auto profile_path = GetProfilePath(xuid);
  std::error_code error;
  auto storage = source->storage->Subroot(
      profile_path.lexically_relative(source->storage->path()), false, error);
  if (!storage) {
    return false;
  }
  auto device = std::make_unique<vfs::HostPathDevice>(
      mount_path, profile_path, false, false, "STFS", storage,
      source->storage);
  if (!device->Initialize()) {
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
  prepared_mount.emplace(key,ProfileMount{xuid,source->generation,1,
      source_authority,view,source->storage,source->source.id,
      source->source.device_id,{},source->managed_identity,account_service});
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
  profile_mounts_.insert(std::move(mount_node));
  return true;
}

bool ProfileManager::DismountProfile(const uint64_t xuid) {
  return DismountProfile(fmt::format(kDefaultMountFormat, xuid));
}

bool ProfileManager::DismountProfile(std::string_view mount_path) {
  if (xe::global_critical_region::is_held_by_current_thread()) {
    return false;
  }
  // Keep native directory/device destructors outside metadata/GCRC locks.
  std::shared_ptr<vfs::HostPathDevice> retiring_device;
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
  if (found == profile_mounts_.end()) {
    return false;
  }
  if (found->second.references > 1) {
    --found->second.references;
    return true;
  }
  for (const auto& retired : retired_account_identities_) {
    if (retired->storage()->xuid() == found->second.xuid &&
        !retired->writes_drained()) { return false; }
  }
  if (auto owner = account_identities_.find(found->second.xuid);
      owner != account_identities_.end() && owner->second->slot() == 0xFF) {
    owner->second->Retire();
    if (!owner->second->writes_drained()) { return false; }
  }
  auto global = xe::global_critical_region::TryAcquire();
  if (!global.owns_lock()) {
    return false;
  }
  retiring_device = std::dynamic_pointer_cast<vfs::HostPathDevice>(
      kernel_state_->file_system()->AcquireDevice(path));
  if (!retiring_device || retiring_device->storage() != found->second.view ||
      !kernel_state_->file_system()->UnregisterDevice(path)) {
    return false;
  }
  if (found->second.publication_lease) {
    found->second.publication_lease->Retire();
  }
  if (auto owner = account_identities_.find(found->second.xuid);
      owner != account_identities_.end() && owner->second->slot() == 0xFF) {
    account_identities_.erase(owner);
  }
  profile_mounts_.erase(found);
  return true;
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
  auto device = std::dynamic_pointer_cast<vfs::HostPathDevice>(
      kernel_state_->file_system()->AcquireDevice(mount));
  if (!device || device->storage() != held.view) {
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
  std::unique_lock profile_lock(profile_mount_mutex_, std::try_to_lock);
  if (!profile_lock.owns_lock()) {
    error = std::make_error_code(std::errc::device_or_resource_busy);
    return {};
  }
  auto global_lock = xe::global_critical_region::TryAcquire();
  if (!global_lock.owns_lock()) {
    error = std::make_error_code(std::errc::device_or_resource_busy);
    return {};
  }
  const uint8_t slot = GetUserIndexAssignedToProfile(xuid);
  const auto generation = profile_sign_in_generations_.find(xuid);
  if (slot >= XUserMaxUserCount ||
      generation == profile_sign_in_generations_.end() || !generation->second) {
    error = std::make_error_code(std::errc::operation_canceled);
    return {};
  }
  if (auto held = gpd_models_.find(xuid); held != gpd_models_.end()) {
    if (held->second->user_slot() != slot ||
        held->second->sign_in_generation() != generation->second) {
      error = std::make_error_code(std::errc::operation_canceled);
      return {};
    }
    return held->second;
  }
  error = std::make_error_code(std::errc::operation_would_block);
  return {};
}
std::shared_ptr<ProfileGpdModelAuthority>
ProfileManager::AcquireProfileGpdModel(uint8_t slot, std::error_code& error) {
  error.clear();
  std::unique_lock profile_lock(profile_mount_mutex_, std::try_to_lock);
  if (!profile_lock.owns_lock()) {
    error = std::make_error_code(std::errc::device_or_resource_busy);
    return {};
  }
  auto global_lock = xe::global_critical_region::TryAcquire();
  if (!global_lock.owns_lock()) {
    error = std::make_error_code(std::errc::device_or_resource_busy);
    return {};
  }
  if (slot >= XUserMaxUserCount || !logged_profiles_.contains(slot) ||
      !logged_profiles_.at(slot)) {
    error = std::make_error_code(std::errc::operation_canceled);
    return {};
  }
  return AcquireProfileGpdModel(logged_profiles_.at(slot)->xuid(), error);
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
  const auto slot = GetUserIndexAssignedToProfile(xuid);
  const auto found = logged_profiles_.find(slot);
  return found == logged_profiles_.end() ? std::vector<uint8_t>{}
                                         : found->second->GetProfileIcon(type);
}

ProfileGpdWriter ProfileManager::CaptureGpdWriter(uint64_t xuid,
                                                  std::error_code& error) {
  auto model = AcquireProfileGpdModel(xuid, error);
  if (!model) {
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
  return {std::move(model), std::move(owner), std::move(recovery)};
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
  return ProfileGpdWriter{model, owner, recovery}.Reload();
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

void ProfileManager::Login(const uint64_t xuid, const uint8_t user_index,
                           bool notify) {
  if (xe::global_critical_region::is_held_by_current_thread()) {
    XELOGW("Profile sign-in requires the owned IO lane");
    return;
  }
  uint8_t slot = XUserIndexAny;
  uint64_t generation = 0;
  X_XAMACCOUNTINFO account{};
  {
    std::unique_lock lock(profile_mount_mutex_, std::try_to_lock);
  if (!lock.owns_lock() || !accounts_.contains(xuid) ||
        preparing_profiles_.contains(xuid) ||
        retiring_profiles_.contains(xuid)) {
    return;
  }
    slot =
      user_index < XUserMaxUserCount ? user_index : FindFirstFreeProfileSlot();
    if (slot >= XUserMaxUserCount || preparing_slots_[slot] ||
        !next_profile_sign_in_generation_) {
      return;
    }
    if (auto it = logged_profiles_.find(slot);
        it != logged_profiles_.end() && it->second->xuid() == xuid) {
      return;
    }
    if (auto unsigned_owner = account_identities_.find(xuid);
        unsigned_owner != account_identities_.end() && unsigned_owner->second->slot() == 0xFF) {
      unsigned_owner->second->Retire();
      if (!unsigned_owner->second->writes_drained()) { return; }
    }
    for (const auto& retired : retired_account_identities_) {
      if ((retired->storage()->xuid() == xuid || retired->slot() == slot) &&
          !retired->writes_drained()) { return; }
    }
    account = accounts_.at(xuid);
    generation = next_profile_sign_in_generation_++;
    preparing_slots_.set(slot);
    preparing_generations_[slot] = generation;
    preparing_profiles_.insert(xuid);
  }
  auto clear_reservation = [this,
         xuid, slot](void*) {
    std::lock_guard lock(profile_mount_mutex_);
    preparing_slots_.reset(slot);
    preparing_generations_[slot] = 0;
    preparing_profiles_.erase(xuid);
  };
  std::unique_ptr<void, decltype(clear_reservation)> reservation(
      reinterpret_cast<void*>(1), clear_reservation);
  uint8_t previous;
  {
    std::lock_guard lock(profile_mount_mutex_);
    previous = GetUserIndexAssignedToProfile(xuid);
  }
  if (previous < XUserMaxUserCount) {
    Logout(previous, notify);
  }
  if (!MountProfile(xuid)) {
    return;
  }
  bool mounted = true;
  auto release_mount = [this, xuid, &mounted](void*) {
    if (mounted) {
      DismountProfile(xuid);
    }
  };
  std::unique_ptr<void, decltype(release_mount)> mount_guard(
      reinterpret_cast<void*>(1), release_mount);
  std::error_code error;
  auto lease = AcquireProfileStorageLease(xuid, error);

  if (!lease) {
    return;
  }
  auto model =
      ProfileGpdModelAuthority::Create(xuid, slot, generation, lease, error);
  auto owner = content_manager_.ProfileIntents(lease->source_id());
  if (!model || (error = LoadGpdModel(model, lease, owner))) {
    XELOGW("Profile {:016X}: GPD load/recovery refused ({})", xuid,
           error.message());
    return;
  }
  auto identity = ProfileAccountIdentity::Capture(lease, slot, generation, error);
  auto snapshot = identity ? ProfileAccountSnapshot::Load(identity, error) : nullptr;
  if (!snapshot) {
    model->Retire();
    return;
  }
  account = snapshot->account();
  auto cache = lease->device()->PrepareSnapshotCache({snapshot->token()}, error);
  if (!cache) { model->Retire(); return; }
  auto profile = std::make_shared<UserProfile>(xuid, &account, model);
  Logout(slot, notify, false);
  auto checked = lease->view()->ValidateSnapshotBatch({snapshot->token()}, error);
  if (!checked) { model->Retire(); return; }
  std::shared_ptr<const filesystem::HostStorageSnapshotBatch> account_batch(
      std::move(checked));
  {
    std::lock_guard lock(profile_mount_mutex_);
    bool account_drained = true;
    for (const auto& retired : retired_account_identities_) {
      if ((retired->storage()->xuid() == xuid || retired->slot() == slot) &&
          !retired->writes_drained()) { account_drained = false; }
    }
    if (!account_drained || logged_profiles_.contains(slot) || lease->retiring() ||
        preparing_generations_[slot] != generation) {
      model->Retire();
      return;
    }
    auto cache_commit = lease->device()->TryCommitSnapshotCache(cache, account_batch, error);
    if (!cache_commit) { model->Retire(); return; }
    logged_profiles_.emplace(slot, std::move(profile));
    account_identities_.insert_or_assign(xuid, std::move(identity));
    account_snapshots_.insert_or_assign(xuid, snapshot);
    account_metadata_.insert_or_assign(xuid,
        std::make_shared<const ProfileAccountMetadata>(*snapshot));
    accounts_.insert_or_assign(xuid, snapshot->account());
    cache_commit->Apply();
    account_publication_->Advance();
    profile_sign_in_generations_.insert_or_assign(xuid, generation);
    gpd_models_.emplace(xuid, model);
    mounted = false;
  }
  account_batch.reset();
  user_tracker_->AddUser(xuid);

  if (notify) {
    kernel_state_->BroadcastNotification(kXNotificationSystemSignInChanged,
                                         GetUsedUserSlots().to_ulong());
  }
  UpdateConfig(xuid, slot);
}

void ProfileManager::Logout(const uint8_t user_index, bool notify,
                            bool cancel_preparing) {
  std::shared_ptr<UserProfile> retired_profile;
  uint32_t used_slots = 0;
  {
  std::lock_guard lock(profile_mount_mutex_);
    if (cancel_preparing && user_index < XUserMaxUserCount) {
      preparing_generations_[user_index] = 0;
    }
    const auto profile = logged_profiles_.find(user_index);
  if (profile == logged_profiles_.end()) {
    return;
  }
    const uint64_t xuid = profile->second->xuid();
    if (auto held = gpd_models_.find(xuid); held != gpd_models_.end()) {
      retired_gpd_models_.push_back(held->second);
      held->second->Retire();
      gpd_models_.erase(held);
    }
    if (auto identity = account_identities_.find(xuid);
        identity != account_identities_.end()) {
      identity->second->Retire();
      if (!identity->second->writes_drained()) {
        retired_account_identities_.push_back(identity->second);
      }
      account_identities_.erase(identity);
    }
    account_snapshots_.erase(xuid);
    profile_sign_in_generations_.erase(xuid);
    account_publication_->Advance();
    user_tracker_->RemoveUser(xuid);
    retired_profile = std::move(profile->second);
  logged_profiles_.erase(profile);
    used_slots = GetUsedUserSlots().to_ulong();
  }
  DismountProfile(retired_profile->xuid());
  if (notify) {
    kernel_state_->BroadcastNotification(kXNotificationSystemSignInChanged,
                                         used_slots);
  }
  UpdateConfig(0, user_index);
}

void ProfileManager::LoginMultiple(
    const std::map<uint8_t, uint64_t>& profiles) {
  int slots_mask = 0;
  for (auto [slot, xuid] : profiles) {
    Login(xuid, slot, false);
    slots_mask |= (1 << slot);
  }

  kernel_state_->BroadcastNotification(kXNotificationSystemSignInChanged,
                                       slots_mask);
}

std::vector<uint64_t> ProfileManager::FindProfiles() const {
  // Info: Profile directory name is also it's offline xuid
  std::vector<uint64_t> profiles_xuids;

  auto profiles_directory = xe::filesystem::FilterByName(
      xe::filesystem::ListDirectories(
          kernel_state_->emulator()->content_root()),
      std::regex("[0-9A-F]{16}"));

  for (const auto& profile : profiles_directory) {
    const std::string profile_xuid = xe::path_to_utf8(profile.name);
    if (profile_xuid == fmt::format("{:016X}", 0)) {
      continue;
    }

    if (!std::filesystem::exists(
            profile.path / profile.name / kDashboardStringID /
            fmt::format("{:08X}",
                        static_cast<uint32_t>(XContentType::kProfile)) /
            profile.name)) {
      XELOGE("Profile {} doesn't have profile package!", profile_xuid);
      continue;
    }

    XELOGI("{}: Adding profile {} to profile list", __func__, profile_xuid);
    profiles_xuids.push_back(
        xe::string_util::from_string<uint64_t>(profile_xuid, true));
  }

  XELOGI("ProfileManager: Found {} Profiles", profiles_xuids.size());
  return profiles_xuids;
}

uint8_t ProfileManager::FindFirstFreeProfileSlot() const {
  if (!IsAnyProfileSignedIn()) {
    return 0;
  }

  std::bitset<XUserMaxUserCount> used_slots = {};
  for (const auto& [index, entry] : logged_profiles_) {
    used_slots.set(index);
  }

  for (uint8_t i = 0; i < used_slots.size(); ++i) {
    if (!used_slots[i]) {
      return i;
    }
  }
  return XUserIndexAny;
}

std::bitset<XUserMaxUserCount> ProfileManager::GetUsedUserSlots() const {
  std::bitset<XUserMaxUserCount> used_slots = {};
  for (const auto& [index, entry] : logged_profiles_) {
    if (!entry) {
      continue;
    }

    used_slots.set(index);
  }

  return used_slots;
}

uint8_t ProfileManager::GetUserIndexAssignedToProfile(
    const uint64_t xuid) const {
  for (const auto& [index, entry] : logged_profiles_) {
    if (!entry) {
      continue;
    }

    if (entry->xuid() != xuid) {
      continue;
    }

    return index;
  }
  // Titles know a user signed in to Live by the online XUID.
  return GetUserIndexAssignedToLiveProfile(xuid);
}

uint8_t ProfileManager::GetUserIndexAssignedToLiveProfile(
    const uint64_t xuid_online) const {
  // GetOnlineXUID() is 0 for a profile without Live, so INVALID_XUID must not
  // match it.
  if (!xuid_online) {
    return XUserIndexAny;
  }
  for (const auto& [index, entry] : logged_profiles_) {
    if (!entry) {
      continue;
    }

    if (entry->GetOnlineXUID() != xuid_online) {
      continue;
    }

    return index;
  }
  return XUserIndexAny;
}

std::filesystem::path ProfileManager::GetProfileContentPath(
    const uint64_t xuid, const uint32_t title_id,
    const XContentType content_type) const {
  std::filesystem::path profile_content_path =
      kernel_state_->emulator()->content_root() / fmt::format("{:016X}", xuid);
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

std::filesystem::path ProfileManager::GetProfilePath(
    const uint64_t xuid) const {
  return kernel_state_->emulator()->content_root() /
         ProfileStorageLease::ProfileRelative(xuid);
}

uint32_t ProfileManager::AllocateProfileCreationResult(
    std::span<const uint8_t> bytes, std::error_code& error) {
  error.clear();
  auto use = account_commit_gate_->Acquire();
  if (!use) { error = std::make_error_code(std::errc::operation_canceled); return 0; }
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

X_HRESULT ProfileManager::ConsumeProfileCreationResult(
    uint32_t address, uint32_t xuid_destination) {
  auto use = account_commit_gate_->Acquire();
  if (!use) { return X_E_FAIL; }
  if (!address || !xuid_destination ||
      global_critical_region::is_held_by_current_thread() ||
      !util::GuestIoRangeValid(*kernel_state_->memory(), xuid_destination, 8, true) ||
      util::GuestIoRangesOverlap(*kernel_state_->memory(), address, 0xAC0,
                                 xuid_destination, 8)) {
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
  std::array<uint8_t, 8> bytes{};
  xe::store_and_swap<uint64_t>(bytes.data(), result.xuid);
  if (XFAILED(util::WriteGuestIoBuffer(*kernel_state_->memory(),
                                      xuid_destination, bytes))) {
    std::lock_guard lock(profile_creation_mutex_);
    profile_creation_results_.at(address).consuming = false;
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
    uint32_t avatar_preset, uint32_t stock_picture) {
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
  const auto source = content_manager_.sources().Source("hdd");
  if (!source || !source->available || !source->profile_writable) {
    result.status = X_ERROR_DEVICE_NOT_CONNECTED;
    return result;
  }
  recipe->descriptor.device_id = source->source.device_id;
  result.status = content_manager_.CreateInitialPackage(
      xuid, recipe->descriptor, recipe->members);
  if (result.status != X_ERROR_SUCCESS) {
    XELOGW("Profile {:016X}: complete initial publication failed ({:08X})",
           xuid, result.status);
    return result;
  }
  result.published = true;
  // Release creation admission before ordinary discovery and sign-in enter
  // their own independent storage/model reservations.
  reservation.reset();
  result.discovered = LoadAccount(xuid);
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
  auto lock = xe::global_critical_region::AcquireDirect();
  copied = accounts_;
  return &copied;
}
const X_XAMACCOUNTINFO* ProfileManager::GetAccount(const uint64_t xuid) {
  thread_local X_XAMACCOUNTINFO copied;
  auto lock = xe::global_critical_region::AcquireDirect();
  const auto account = accounts_.find(xuid);
  if (account == accounts_.end()) { return nullptr; }
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
  auto storage = AcquireProfileStorageLease(xuid, error);
  if (!storage) { return {}; }
  std::vector<std::shared_ptr<ProfileAccountIdentity>> released_owners;
  std::unique_lock lock(profile_mount_mutex_, std::try_to_lock);
  if (!lock.owns_lock()) {
    error = std::make_error_code(std::errc::device_or_resource_busy);
    return {};
  }
  if (retiring_profiles_.contains(xuid) || preparing_profiles_.contains(xuid)) {
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
    if (!lock.owns_lock() || preparing_slots_.any() || !retiring_profiles_.empty()) {
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
  if (!lock.owns_lock() || !out->OwnershipCurrent() || preparing_slots_.any()) {
    error = std::make_error_code(std::errc::operation_canceled);
    return {};
  }
  return out;
}
bool ProfileManager::PublishAccountCache(
    std::shared_ptr<const ProfileAccountSnapshot> snapshot,
    std::error_code& error) {
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
    commit->Apply();
    account_publication_->Advance();
  }
  batch.reset();
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
  {
    std::unique_lock lock(profile_mount_mutex_, std::try_to_lock);
    if (!lock.owns_lock()) {
      return;
    }
    for (const auto& [index, profile] : logged_profiles_) {
      current[index] = profile->xuid();
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
  config::SaveConfig();
}

bool ProfileManager::DeleteProfile(const uint64_t xuid) {
  if (xe::global_critical_region::is_held_by_current_thread()) {
    return false;
  }
  {
    std::unique_lock lock(profile_mount_mutex_, std::try_to_lock);

  if (!lock.owns_lock() || !accounts_.contains(xuid) ||
        preparing_profiles_.contains(xuid) ||
        !retiring_profiles_.insert(xuid).second) {
      return false;
    }
  }
  auto release = [this, xuid](void*) {
    std::lock_guard lock(profile_mount_mutex_);
    retiring_profiles_.erase(xuid);
  };
  std::unique_ptr<void, decltype(release)> reservation(
      reinterpret_cast<void*>(1), release);
  const auto source = content_manager_.sources().Source("hdd");
  const auto owner = content_manager_.ProfileIntents("hdd");
  if (!source || !source->storage || !owner) {
    return false;
  }
  const auto relative =
      GetProfilePath(xuid).lexically_relative(source->storage->path());
  uint8_t slot;
  std::vector<std::shared_ptr<ProfileGpdModelAuthority>> retired;
  {
    std::lock_guard lock(profile_mount_mutex_);
    slot = GetUserIndexAssignedToProfile(xuid);
  }
  if (slot < XUserMaxUserCount) {
    Logout(slot);
  }
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
