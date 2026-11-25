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

#include <array>
#include <bitset>
#include <map>
#include <mutex>
#include <optional>
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
#include "xenia/kernel/xam/profile_roster.h"
#include "xenia/kernel/xam/profile_account_snapshot.h"
#include "xenia/kernel/xam/profile_account_file.h"
#include "xenia/kernel/util/guest_io_buffer.h"

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
struct ContentSourceState;
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

class NativeOriginalLogonObservation;
namespace test { class ProfileSettingsReadTestAccess; }
class ProfileManager;
struct NativeProfileProjection; // synchronous POD diagnostic only
struct ProfileSettingsReadLifetime;
struct ProfileSettingsReadUse;
struct ProfileSettingsReadNativeOwners;
// A service/lifetime capability, not a statement that any user is signed in or
// that native module/resource initialization has completed.
class ProfileSettingsReadProvider final {
 public:
  bool Current(const ProfileManager&) const;
 private:
  friend class ProfileManager;
  ProfileSettingsReadProvider(const ProfileManager*,
      std::shared_ptr<ProfileSettingsReadLifetime>);
  const ProfileManager* const owner_;  // comparison only, never a callback
  const std::shared_ptr<ProfileSettingsReadLifetime> lifetime_;
};
// Each user is an independently validated immutable observation. This is not
// a global point-in-time transaction across users or filesystems.
class ProfileSettingsReadBatch final {
 public:
  class User final {
   public:
    uint64_t xuid() const { return xuid_; }
    std::optional<UserSetting> Setting(uint32_t title, uint32_t id,
                                      std::error_code&) const;
   private:
    friend class ProfileManager;
    friend class ProfileSettingsReadBatch;
    uint64_t xuid_ = 0;
    std::weak_ptr<const ProfileSettingsReadUse> use_;
    std::weak_ptr<ProfileSettingsReadNativeOwners> native_;
    std::shared_ptr<const GpdInfoProfile> dashboard_;
    std::shared_ptr<const GpdInfoTitle> title_;
    uint32_t title_id_ = 0;
  };
  const std::vector<User>& users() const { return users_; }
  // Cached logical lifetime/source/account ownership only: later ordinary
  // data changes do not undo a completed immutable read observation.
  bool IdentityCurrent() const;
  X_STATUS Publish(Memory&, std::span<const util::GuestIoWrite>) const;
 private:
  friend class ProfileManager;
  friend class util::GuestIoProfileReadScope;
  bool IdentityCurrentAdmitted() const;
  const ProfileManager* profile_owner_ = nullptr; // actual gate Use only
  ProfileSettingsReadBatch() = default;
  std::shared_ptr<const ProfileSettingsReadUse> use_;
  std::shared_ptr<ProfileAccountCommitGate> publication_gate_;
  std::vector<User> users_;
};

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
  // Advisory scalar observations only; no admission, lookup, or guest IO.
  void RecordNativeProfileQuery(uint32_t ordinal, uint32_t slot,
                                uint32_t status, uint64_t value,
                                uint32_t flags = 0) const noexcept;
  void RecordNativeSigninUi(uint32_t user, uint32_t users, uint32_t flags,
                            uint32_t overlap, uint32_t result, uint32_t route,
                            uint32_t stage, uint32_t native_owned) const noexcept;
  std::shared_ptr<const ProfileSettingsReadProvider>
  CaptureProfileSettingsReadProvider() const;
  std::shared_ptr<const ProfileSettingsReadBatch> CaptureProfileSettingsRead(
      std::span<const uint64_t> xuids, uint32_t title, std::error_code&);
  std::shared_ptr<const ProfileSettingsReadBatch> CaptureProfileSettingsRead(
      uint8_t slot, uint32_t title, std::error_code&);
  // Owned last-known local metadata derived from authentic Account receipts,
  // never a borrowed map/model or synthetic HDD1 identity. These display rows
  // grant no fresh native-source, storage-read or sign-in authority.
  std::vector<X_PROFILEENUMRESULT> CaptureProfileInventory(uint32_t device_id,
                                                          std::error_code&) const;
  void ReapReleasedProfileReads();
  ProfileGpdWriter CaptureGpdWriter(uint64_t xuid, std::error_code&);
  ProfileGpdWriter CaptureGpdWriter(uint8_t slot, std::error_code&);
  std::error_code ReloadGpdModel(uint64_t xuid);

  // Advisory scalar creation observations only. Capture is allocation/lock/IO
  // free; the existing host-only native profile reporter emits them later.
  // No identity, name, guest pointer or Account bytes enter these records.
  struct NativeCreationDiagnostic {
    enum class Operation : uint32_t { kApi230=1, kProducer=2, kConsumer234=3, kTransaction=4 };
    enum class Reason : uint32_t {
      kNone=0, kApiBuffers=1, kApiCopy=2, kApiDelegated=3,
      kProducerGate=100, kProducerOutput=101, kProducerGlobalLock=102,
      kProducerOutputRange=103, kProducerAllocation=104, kProducerOutputWrite=105,
      kProducerAccountFlagsWrite=106, kProducerGeneratedIdentityWrite=107,
      kProducerSuppliedIdentityClass=108, kProducerOnlinePhase=109,
      kProducerDeviceRead=110, kProducerDeviceWriteRange=111,
      kProducerNoDefaultDevice=112, kProducerDeviceWrite=113,
      kProducerDeviceReload=114, kProducerException=115, kProducerCompletion=116,
      kConsumerGate=200, kConsumerInput=201, kConsumerUnknownRecord=202,
      kConsumerPending=203, kConsumerDestination=204, kConsumerWrite=205,
      kConsumerTerminal=206, kConsumerGlobalLock=207,
      kTransactionGate=300, kTransactionInput=301, kTransactionGlobalLock=302,
      kTransactionReservation=303, kTransactionExists=304,
      kTransactionDevice=305, kTransactionPermission=306,
      kTransactionVolume=307, kTransactionInstalled=308, kTransactionRecipe=309,
      kTransactionSource=310, kTransactionPublication=311,
      kTransactionDiscovery=312, kTransactionSignin=313,
    };
    uint32_t operation=0, reason=0, failure=0;
    uint32_t flags_available=0, flags=0;
    uint32_t return_available=0, return_value=0;
    uint32_t result_available=0, result_status=0;
    uint32_t published=0, discovered=0, signed_in=0;
    uint32_t error_available=0, error_category=0, error_value=0;
  };
  // Bounded diagnostics of actual original20B/224 calls. No input bytes,
  // identities, guest pointers or permission are carried by these records.
  struct NativeOriginalLogonDiagnostic {
    enum Stage:uint32_t {kEntry,kAdmissionReturn,kPrepareFailure,kNativeReturn,
      kProtocolReturn,kPublicationFirst,kPublicationReady,kSettlementFirst,
      kSettlementCommitted,kExportReturn,kOverlapBeforeStatus,kOverlapReturn,kStageCount};
    uint64_t journal=0,capture_sequence=0;
    uint32_t stage=0,ordinal=0,flags=0,overlap_present=0,reason=0,line=0;
    uint32_t disposition_available=0,disposition=0,result_available=0,result=0;
    uint32_t error_available=0,error_category=0,error_value=0,evaluated=0,failed=0;
    uint32_t expected_mask_available=0,expected_nonempty_mask=0,expected_nonempty_count=0;
    uint32_t input_mask_available=0,input_nonempty_mask=0,input_nonempty_count=0;
    uint32_t desired_mask_available=0,desired_nonempty_mask=0,desired_nonempty_count=0;
    uint32_t selected_mask=0,remove_mask=0;
    uint32_t latest_available=0,latest_present=0,latest_kind=0;
    uint32_t latest_read_present=0,latest_borrowed_read_present=0,latest_same_source=0;
    uint32_t entered=0,returned=0,phase_available=0,phase=0;
    uint32_t maps_committed=0,writers_available=0;
    uint32_t completion_error_available=0,completion_error=0,completion_extended=0,completion_length=0;
  };
  struct NativeCreationDiagnosticSink;  // Scalar-only; may outlive this PM.
  // Capture at valid entry, before delegation/admission. This is diagnostic
  // ownership only and never grants a borrowed PM/Kernel/provider lifetime.
  std::shared_ptr<NativeCreationDiagnosticSink> CaptureNativeCreationDiagnosticSink() const noexcept {
    return creation_diagnostics_;
  }
  static void RecordNativeProfileCreation(
      const std::shared_ptr<NativeCreationDiagnosticSink>&,
      const NativeCreationDiagnostic&) noexcept;

  static NativeOriginalLogonObservation BeginNativeOriginalLogon(
      const std::shared_ptr<NativeCreationDiagnosticSink>&,uint32_t ordinal,
      uint32_t flags,bool overlap_present) noexcept;
  static void RecordNativeOriginalLogon(const NativeOriginalLogonObservation&,
      NativeOriginalLogonDiagnostic) noexcept;
  static bool CopyNativeOriginalLogon(const NativeOriginalLogonObservation&,
      uint32_t stage,NativeOriginalLogonDiagnostic&,uint32_t* observed=nullptr) noexcept;
  static bool CopyNativeOriginalLogon(const std::shared_ptr<NativeCreationDiagnosticSink>&,
      uint32_t attempt,uint32_t stage,NativeOriginalLogonDiagnostic&,uint32_t* observed=nullptr) noexcept;

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
      uint32_t avatar_preset = 0, uint32_t stock_picture = 0,
      std::optional<uint32_t> selected_device = {});

  // Owned synchronous HLE completion records. These do not mint a retail
  // Task. Status/free accepts only an allocation issued by this manager.
  // One counted local-provider operation spans pending publication, real
  // synchronous creation and terminalization, even when stop seals admission.
  X_HRESULT ProduceProfileCreationResult(std::array<uint8_t, 0xAC0>,
                                          uint32_t output_address);
  uint32_t AllocateProfileCreationResult(std::span<const uint8_t>,
                                         std::error_code&);
  bool CompleteProfileCreationResult(uint32_t, ProfileCreationResult);
  void DiscardProfileCreationResult(uint32_t);
  X_HRESULT ConsumeProfileCreationResult(uint32_t, uint32_t xuid_destination);

  bool CreateProfile(const std::string gamertag, bool autologin,
                     bool default_xuid = false);
  bool CreateProfile(const X_XAMACCOUNTINFO* account_info, uint64_t xuid);

  bool DeleteProfile(const uint64_t xuid);

  // Optional host scalar output names the exact registered mount device. It is
  // stored only after a genuine mount reference has been acquired successfully.
  bool MountProfile(const uint64_t xuid, std::string mount_path = "",
                    uint32_t* mounted_device_id = nullptr);
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

  ProfileRosterExpected CaptureRosterExpected(std::error_code&) const;
  std::shared_ptr<ProfileRosterIntent> PrepareRoster(
      const ProfileRosterDesired&, const ProfileRosterExpected&,
      std::error_code&);
  ProfileRosterResult CommitHostRoster(const std::shared_ptr<ProfileRosterIntent>&);
  ProfileRosterResult CancelRoster(const std::shared_ptr<ProfileRosterIntent>&);
  ProfileRosterResult AdoptRoster(const ProfileRosterDesired&,
                                 const ProfileRosterExpected&);

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
    std::unique_lock lock(profile_mount_mutex_, std::try_to_lock);
    if (!lock.owns_lock()) { return 0; }
    uint32_t count = 0;
    for (const auto& [xuid, account] : accounts_) {
      std::error_code error;
      if (!profile_discovery_conflicts_.contains(xuid) ||
          CaptureCachedAccount(xuid, error)) { ++count; }
    }
    return count;
  }
  bool IsAnyProfileSignedIn() const;

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

  // Host-only typed edit preparation. The returned owner keeps its exact
  // source/snapshot/mount through real commit, cancel or deferred rundown.
  std::shared_ptr<HostProfileAccountEdit> PrepareAccountEdit(
      uint64_t xuid, ProfileAccountFieldEdits, std::error_code&);
  // Legacy full-Account writes retain their existing Live-default policy.
  bool UpdateAccount(const uint64_t xuid, const X_XAMACCOUNTINFO* account);

  std::vector<ScannedTitleInfo> ScanAllProfilesForTitles() const;

  static bool IsGamertagValid(const std::string gamertag);

 private:
  friend class ::xe::kernel::NativeProfileIdentityOwner;
  friend class ::xe::kernel::NativeSystemUiService;
  friend class ProfileRosterIntent;
  friend class ProfileRosterPublicationCell;
  friend class ProfileRosterNativeReadScope;
  void InitializeRosterAuthority();
  void BeginRosterStop();
  void CloseRosterAuthority();
  void RetryRosterSettlement();
  // One originating DeleteProfile call owns this noncopyable proof. The
  // existing retirement-set node and immutable authenticated origin are held
  // before any requested roster removal; no proof escapes the C++ call.
  struct ProfileDeletionReservation {
    ProfileDeletionReservation() = default;
    ProfileDeletionReservation(const ProfileDeletionReservation&) = delete;
    ProfileDeletionReservation& operator=(const ProfileDeletionReservation&) = delete;
    const ProfileManager* owner = nullptr;
    uint64_t xuid = 0;
    const uint64_t* retirement = nullptr;
    std::shared_ptr<const ProfileAccountMetadata> metadata;
    std::shared_ptr<const ContentSourceState> source;
  };
  ProfileRosterResult AdoptRosterInternal(const ProfileRosterDesired&,
      const ProfileRosterExpected&, const ProfileDeletionReservation*);
  // Retain the exact unpublished attempt across real busy cancellation.
  // Public PrepareRoster keeps its existing interface.
  std::shared_ptr<ProfileRosterIntent> PrepareRosterAttempt(
      const ProfileRosterDesired&, const ProfileRosterExpected&,
      std::error_code&, std::shared_ptr<ProfileRosterIntent>* retained_attempt,
      const ProfileDeletionReservation* deletion = nullptr);
  std::shared_ptr<const ProfileAccountSnapshot> RefreshRosterAccount(
      const std::shared_ptr<const ProfileAccountSnapshot>&);
  std::shared_ptr<UserProfile> CaptureRosterProfile(uint8_t) const;
  std::shared_ptr<ProfileGpdModelAuthority> CaptureRosterModel(uint64_t) const;
  std::shared_ptr<ProfileAccountIdentity> CaptureRosterIdentity(uint64_t) const;
  std::shared_ptr<const ProfileAccountSnapshot> CaptureRosterAccount(uint64_t) const;
  std::shared_ptr<const HostRoster> CaptureSelectedRoster(
      NativeProfileProjection* diagnostic = nullptr) const;
  void ReportNativeProfileDiagnostics(const char* boundary) const noexcept;
  enum class RosterSettlementStage:uint8_t {kHostComplete,kNativeMaps,kNativeRetirement};
  ProfileRosterResult SettleRoster(const std::shared_ptr<ProfileRosterIntent>&,
                                  RosterSettlementStage);
  bool OrdinaryRosterWriterAvailable(
      const std::shared_ptr<ProfileGpdModelAuthority>&) const;
  bool CanonicalRosterAccountsAvailable() const;
  bool RosterIdentityRebindAvailable(uint64_t xuid) const;
  std::shared_ptr<ProfileRosterAuthority> roster_authority_;
  std::shared_ptr<const ProfileRosterNativeBinding> ClaimNativeRosterFactory();
  bool InstallNativeRosterOwner(
      const std::shared_ptr<const ProfileRosterNativeBinding>&,
      std::weak_ptr<::xe::kernel::NativeProfileIdentityOwner>);
  std::shared_ptr<ProfileRosterNativeReadTicket> CaptureCurrentNativeReadTicket(
      const std::shared_ptr<const ProfileRosterNativeBinding>&,
      std::error_code&);
  // Read-only constructor-baseline comparison. Caller may retain the existing
  // recursive metadata lock through a separate scalar owner commit.
  std::optional<uint64_t> CaptureInitialEmptyNativeRosterSequence(
      const std::shared_ptr<const ProfileRosterNativeBinding>&,
      const ProfileRosterExpected&,
      const std::shared_ptr<ProfileRosterNativeReadTicket>&) const;
  std::shared_ptr<ProfileRosterPublicationCell> EnterNativeRoster(
      const std::shared_ptr<ProfileRosterIntent>&,
      const std::shared_ptr<const ProfileRosterNativeBinding>&,
      std::error_code&);
  ProfileRosterResult PublishNativeRosterProtocol(
      const std::shared_ptr<ProfileRosterIntent>&,
      const ProfileRosterNativeCompletion&);
  ProfileRosterResult FinalizeNativeRoster(
      const std::shared_ptr<ProfileRosterIntent>&,
      const ProfileRosterNativeCompletion&);

  friend class HostProfileAccountEdit;
  ProfileAccountEditResult CommitAccountEdit(
      const std::shared_ptr<HostProfileAccountEdit::State>&);
  ProfileAccountEditResult CancelAccountEdit(
      const std::shared_ptr<HostProfileAccountEdit::State>&);
  bool ReleaseAccountEditMount(HostProfileAccountEdit::State&);
  void ReapAccountEdits();
  void CloseAccountEdits();
  friend class ProfileSettingsReadProvider;
  friend class test::ProfileSettingsReadTestAccess;
  friend bool ProfileSettingsReadBatch::IdentityCurrentAdmitted() const;
  friend X_STATUS ProfileSettingsReadBatch::Publish(
      Memory&, std::span<const util::GuestIoWrite>) const;
  struct ProfileReadMountNode {
    std::weak_ptr<const ProfileSettingsReadUse> use;
    std::shared_ptr<ProfileSettingsReadNativeOwners> native;
    std::shared_ptr<ProfileReadMountNode> next;
    uint64_t mount_generation = 0;
  };
  // The first read after durable creation has no cached Account origin yet.
  // Keep the exact selected source through that shared discovery path.
  bool LoadAccountInternal(uint64_t, const ContentSourceState* exact_source,
                           std::optional<uint64_t> discovery_revision = {});
  bool MountProfileInternal(uint64_t, std::string,
                            std::shared_ptr<ProfileReadMountNode> = {},
                            const ContentSourceState* exact_source = nullptr,
                            uint32_t* mounted_device_id = nullptr,
                            HostProfileAccountEdit::State* edit = nullptr);
  bool DismountProfileInternal(std::string_view,
                              std::shared_ptr<ProfileReadMountNode> = {},
                              const HostProfileAccountEdit::State* edit = nullptr);
  std::shared_ptr<ProfileSettingsReadLifetime> settings_read_lifetime_;
  std::shared_ptr<const ProfileSettingsReadProvider> settings_read_provider_;
  uint64_t next_profile_mount_generation_ = 1;
  // Issued edit states own exact normal mount references. An abandoned UI
  // cannot drop a busy cleanup or an actual pending publication receipt.
  uint64_t next_account_edit_ = 1;
  std::map<uint64_t, std::shared_ptr<HostProfileAccountEdit::State>> account_edits_;
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
                           std::error_code&,
                           std::optional<uint64_t> discovery_revision = {});
  bool SetLiveDefaults(X_XAMACCOUNTINFO* account) const;

  struct ProfileDiscovery;
  ProfileDiscovery FindProfiles() const;
  // Negative last-known discovery metadata only. It grants no source/storage
  // authority and never retires an already committed exact roster origin.
  void MarkProfileDiscoveryConflictLocked(uint64_t);
  std::set<uint64_t> profile_discovery_conflicts_;
  uint64_t profile_discovery_revision_ = 1;
  std::mutex profile_discovery_mutex_;

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
    uint64_t mount_generation = 0;
    size_t read_references = 0;
    std::shared_ptr<ProfileReadMountNode> read_head;
    // Exact unpublished producer captured before common registration. Map keys
    // are case-folded metadata; they are never a registered-device identity.
    std::shared_ptr<vfs::HostPathDevice> device;
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

  uint32_t AllocateProfileCreationResultEntered(std::span<const uint8_t>,
                                                std::error_code&);
  bool CompleteProfileCreationResultEntered(uint32_t, ProfileCreationResult);
  void DiscardProfileCreationResultEntered(uint32_t);
  struct GuestProfileCreationResult {
    ProfileCreationResult result;
    bool completed = false, consuming = false;
  };
  static std::shared_ptr<NativeCreationDiagnosticSink> CreateNativeCreationDiagnosticSink();
  const std::shared_ptr<NativeCreationDiagnosticSink> creation_diagnostics_ =
      CreateNativeCreationDiagnosticSink();
  std::mutex profile_creation_mutex_;
  std::map<uint32_t, GuestProfileCreationResult> profile_creation_results_;

  KernelState* kernel_state_;
  UserTracker* user_tracker_;
  // XamState is still being constructed during initial account discovery.
  // Use its already constructed catalog without routing back through it.
  ContentManager& content_manager_;
};

class NativeOriginalLogonObservation final {
 public:
  bool active() const noexcept {return sink_ && slot_<8;}
  uint32_t attempt() const noexcept {return active()?slot_+1:0;}
 private:
  friend class ProfileManager;
  std::shared_ptr<ProfileManager::NativeCreationDiagnosticSink> sink_;
  uint32_t slot_=UINT32_MAX,ordinal_=0,flags_=0,overlap_=0;
};

}  // namespace xam
}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_XAM_PROFILE_MANAGER_H_
