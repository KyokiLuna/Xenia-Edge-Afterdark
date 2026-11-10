/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2024 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XAM_PROFILE_MANAGER_H_
#define XENIA_KERNEL_XAM_PROFILE_MANAGER_H_

#include <bitset>
#include <map>
#include <mutex>
#include <random>
#include <span>
#include <string>
#include <vector>
#include "third_party/fmt/include/fmt/format.h"
#include "xenia/base/string.h"
#include "xenia/kernel/title_id_utils.h"
#include "xenia/kernel/xam/user_profile.h"
#include "xenia/xbox.h"
#include <set>
#include "xenia/kernel/xam/profile_gpd_transaction.h"
#include "xenia/kernel/xam/profile_gpd_model.h"
#include "xenia/kernel/xam/profile_gpd_publication.h"
#include "xenia/kernel/xam/profile_account_snapshot.h"
#include "xenia/kernel/xam/profile_account_file.h"

namespace xe::filesystem {
class HostStorageRoot;
}

namespace xe {
namespace kernel {
class KernelState;
}  // namespace kernel
}  // namespace xe

namespace xe {
namespace kernel {
namespace xam {
class UserTracker;
class ContentManager;
}  // namespace xam
}  // namespace kernel
}  // namespace xe

namespace xe {
namespace kernel {
namespace xam {

struct ScannedTitleInfo {
  uint32_t title_id;
  std::string title_name;
  std::filesystem::path path_to_file;
  std::vector<GpdInfoProfile::DiscInfo> all_discs;
  time_t last_run_time;
};

inline const std::string kDashboardStringID =
    fmt::format("{:08X}", kDashboardID);

constexpr std::string_view kDefaultMountFormat = "User_{:016X}";

const static inline uint64_t GenerateXuid() {
  std::random_device rd;
  std::mt19937 gen(rd());

  return ((uint64_t)0xE03 << 52) + (gen() % (1 << 31));
}

const static inline uint64_t GenerateOnlineXuid() {
  std::random_device rd;
  std::mt19937 gen(rd());

  return (0x9ULL << 48) + (gen() % (1ULL << 31));
}

class ProfileManager {
 public:
  static bool DecryptAccountFile(std::span<const uint8_t> data,
                                 X_XAMACCOUNTINFO* output,
                                 bool devkit = false);

  static void EncryptAccountFile(const X_XAMACCOUNTINFO* input, uint8_t* output,
                                 bool devkit = false);

  // Profile:
  //  - Account
  //  - GPDs (Dashboard, titles)

  // Loading Profile means load everything
  // Loading Account means load basic data
  ProfileManager(KernelState* kernel_state, UserTracker* user_tracker,
                 ContentManager& content_manager);

  ~ProfileManager();
  void InitializeProfiles();
  ProfileGpdWriter CaptureGpdWriter(uint64_t xuid, std::error_code&);
  ProfileGpdWriter CaptureGpdWriter(uint8_t slot, std::error_code&);
  std::error_code ReloadGpdModel(uint64_t xuid);

  struct ProfileCreationResult {
    X_RESULT status = X_ERROR_FUNCTION_FAILED;
    uint64_t xuid = 0;
    bool published = false, discovered = false, signed_in = false;
  };
  // Publication and requested sign-in are distinct outcomes. A completed
  // package is preserved if discovery/sign-in must be retried later.
  ProfileCreationResult CreateProfileDetailed(
      const std::string& gamertag, bool autologin, bool default_xuid = false,
      uint32_t avatar_preset = 0, uint32_t stock_picture = 0);
  ProfileCreationResult CreateProfileDetailed(
      const X_XAMACCOUNTINFO&, uint64_t xuid, bool autologin = false,
      uint32_t avatar_preset = 0, uint32_t stock_picture = 0);

  // Owned synchronous HLE completion records. These do not mint a retail
  // Task. Status/free accepts only an allocation issued by this manager.
  uint32_t AllocateProfileCreationResult(std::span<const uint8_t>,
                                         std::error_code&);
  bool CompleteProfileCreationResult(uint32_t, ProfileCreationResult);
  void DiscardProfileCreationResult(uint32_t);
  X_HRESULT ConsumeProfileCreationResult(uint32_t, uint32_t xuid_destination);

  bool CreateProfile(const std::string gamertag, bool autologin,
                     bool default_xuid = false);
  bool CreateProfile(const X_XAMACCOUNTINFO* account_info, uint64_t xuid);

  bool DeleteProfile(const uint64_t xuid);

  bool MountProfile(const uint64_t xuid, std::string mount_path = "");
  bool DismountProfile(const uint64_t xuid);
  bool DismountProfile(std::string_view mount_path);

  // Owned storage only, not sign-in/model validity. Never performs observing
  // or blocking storage IO while capturing; busy is a truthful retry outcome.
  std::shared_ptr<ProfileStorageLease> AcquireProfileStorageLease(
      uint64_t xuid, std::error_code& error);
  // Actual logged-in slot/XUID and sign-in epoch; no IO or borrowed profile.
  std::shared_ptr<ProfileGpdModelAuthority> AcquireProfileGpdModel(
      uint64_t xuid, std::error_code& error);
  std::shared_ptr<ProfileGpdModelAuthority> AcquireProfileGpdModel(
      uint8_t slot, std::error_code& error);

  void Login(const uint64_t xuid, const uint8_t user_index = XUserIndexAny,
             bool notify = true);
  void Logout(const uint8_t user_index, bool notify = true,
              bool cancel_preparing = true);
  void LoginMultiple(const std::map<uint8_t, uint64_t>& profiles);

  bool LoadAccount(const uint64_t xuid);
  // Exact mounted storage/identity plus authenticated durable bytes, never a
  // borrowed cached Account pointer. IO runs outside profile/namespace locks.
  std::shared_ptr<const ProfileAccountSnapshot> CaptureDurableAccount(
      uint64_t xuid, std::error_code&);
  std::shared_ptr<const NativeLogonAccountInputs> PrepareNativeLogonAccounts(
      std::error_code&);

  void ReloadProfiles();
  void ReloadProfile(const uint64_t xuid);
  void ReloadProfileGpds();
  void SyncProfilesWithConfig();

  UserProfile* GetProfile(const uint64_t xuid) const;
  std::shared_ptr<UserProfile> CaptureProfile(uint64_t xuid) const;
  std::shared_ptr<UserProfile> CaptureProfile(uint8_t slot) const;
  UserProfile* GetProfile(const uint8_t user_index) const;
  std::shared_ptr<const ProfileGpdReadModels> CaptureGpdRead(uint8_t slot,
                                                             std::error_code&);
  std::shared_ptr<const ProfileGpdReadModels> CaptureGpdRead(uint64_t xuid,
                                                             std::error_code&);
  std::vector<uint8_t> CaptureProfileIcon(uint64_t xuid, XTileType) const;
  UserProfile* GetProfileLive(const uint64_t xuid) const;
  uint8_t GetUserIndexAssignedToProfile(const uint64_t xuid) const;
  uint8_t GetUserIndexAssignedToLiveProfile(const uint64_t xuid_online) const;

  // Legacy read projections have thread-local copy lifetime through the next
  // same getter on that thread. Durable/native producers use owned receipts.
  const std::map<uint64_t, X_XAMACCOUNTINFO>* GetAccounts();
  const X_XAMACCOUNTINFO* GetAccount(const uint64_t xuid);
  // Immutable cached metadata, with actual owning device identity. This does
  // not certify current storage or native provider freshness.
  std::shared_ptr<const ProfileAccountMetadata> CaptureCachedAccount(
      uint64_t xuid, std::error_code&) const;

  uint32_t GetAccountCount() const {
    auto lock = xe::global_critical_region::AcquireDirect();
    return static_cast<uint32_t>(accounts_.size());
  }
  bool IsAnyProfileSignedIn() const { return !logged_profiles_.empty(); }

  // Short, nonblocking namespace/profile reservation. It owns only locks and
  // authorizes pure admission closure, never IO, callbacks or owner teardown.
  class ManagedFormatIdleReservation final {
   public:
    ~ManagedFormatIdleReservation() = default;
   private:
    friend class ProfileManager;
    explicit ManagedFormatIdleReservation(std::recursive_mutex& mutex)
        : profiles_(mutex, std::try_to_lock),
          namespace_(xe::global_critical_region::AcquireDeferred()) {}
    std::unique_lock<std::recursive_mutex> profiles_;
    xe::global_unique_lock_type namespace_;
  };
  std::unique_ptr<ManagedFormatIdleReservation> TryReserveIdleForManagedFormat(
      std::error_code&);


  std::filesystem::path GetProfileContentPath(
      const uint64_t xuid, const uint32_t title_id = -1,
      const XContentType content_type = XContentType::kInvalid) const;

  // Writes the account and applies it to the signed-in profile, filling in
  // what a Live-enabled account is missing.
  bool UpdateAccount(const uint64_t xuid, const X_XAMACCOUNTINFO* account);

  std::vector<ScannedTitleInfo> ScanAllProfilesForTitles() const;

  static bool IsGamertagValid(const std::string gamertag);

 private:
  friend class ProfileAccountFileService;
  friend class ProfileAccountManagedFile;
  std::shared_ptr<ProfileAccountCommitGate> account_commit_gate_;
  std::shared_ptr<ProfileAccountPublicationStamp> account_publication_ =
      std::make_shared<ProfileAccountPublicationStamp>();
  std::error_code LoadGpdModel(
      std::shared_ptr<ProfileGpdModelAuthority>,
      std::shared_ptr<ProfileStorageLease>,
      std::shared_ptr<const filesystem::ProfileIntentOwner>);
  std::bitset<XUserMaxUserCount> preparing_slots_;
  std::array<uint64_t, XUserMaxUserCount> preparing_generations_{};
  std::set<uint64_t> preparing_profiles_, retiring_profiles_;
  void UpdateConfig(const uint64_t xuid, const uint8_t slot);
  bool CreateAccount(const uint64_t xuid, const std::string gamertag);
  bool CreateAccount(const uint64_t xuid, const X_XAMACCOUNTINFO* account);
  bool WriteAccount(const uint64_t xuid, const X_XAMACCOUNTINFO* account);
  std::shared_ptr<ProfileAccountIdentity> CaptureAccountIdentity(
      uint64_t xuid, std::error_code&);
  bool PublishAccountCache(std::shared_ptr<const ProfileAccountSnapshot>,
                           std::error_code&);
  bool SetLiveDefaults(X_XAMACCOUNTINFO* account) const;

  std::filesystem::path GetProfilePath(const uint64_t xuid) const;
  std::vector<uint64_t> FindProfiles() const;

  uint8_t FindFirstFreeProfileSlot() const;
  std::bitset<XUserMaxUserCount> GetUsedUserSlots() const;

  std::map<uint64_t, X_XAMACCOUNTINFO> accounts_;
  std::map<uint64_t, std::shared_ptr<ProfileAccountIdentity>> account_identities_;
  // Closed producers remain retained until actual counted writes finish. A
  // new slot/XUID generation never borrows an old producer's in-flight CAS.
  std::vector<std::shared_ptr<ProfileAccountIdentity>> retired_account_identities_;
  std::map<uint64_t, std::shared_ptr<const ProfileAccountSnapshot>> account_snapshots_;
  std::map<uint64_t, std::shared_ptr<const ProfileAccountMetadata>> account_metadata_;
  std::map<uint8_t, std::shared_ptr<UserProfile>> logged_profiles_;
  struct ProfileMount {
    uint64_t xuid = 0, source_generation = 0;
    size_t references = 0;
    std::shared_ptr<filesystem::HostStorageRoot> source, view, guest_generation;
    std::string source_id;
    uint32_t device_id = 0;
    std::shared_ptr<ProfileStorageLease> publication_lease;
    std::shared_ptr<const filesystem::ManagedDeviceIdentity> managed_identity;
    std::shared_ptr<vfs::ManagedFileService> account_service;
  };
  mutable std::recursive_mutex profile_mount_mutex_;
  std::mutex profile_config_mutex_;
  std::map<std::string, ProfileMount> profile_mounts_;
  std::map<uint64_t, uint64_t> profile_sign_in_generations_;
  uint64_t next_profile_sign_in_generation_ = 1;
  std::map<uint64_t, std::shared_ptr<ProfileGpdModelAuthority>> gpd_models_;
  // Until real journal recovery/completion can retire them, pending owners
  // survive the unique UserProfile destroyed by sign-out. Never drop by XUID.
  std::vector<std::shared_ptr<ProfileGpdModelAuthority>> retired_gpd_models_;

  struct GuestProfileCreationResult {
    ProfileCreationResult result;
    bool completed = false, consuming = false;
  };
  std::mutex profile_creation_mutex_;
  std::map<uint32_t, GuestProfileCreationResult> profile_creation_results_;

  KernelState* kernel_state_;
  UserTracker* user_tracker_;
  // XamState is still being constructed during initial account discovery.
  // Use its already constructed catalog without routing back through it.
  ContentManager& content_manager_;
};

}  // namespace xam
}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_XAM_PROFILE_MANAGER_H_
