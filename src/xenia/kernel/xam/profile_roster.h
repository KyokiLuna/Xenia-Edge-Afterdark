/** Xenia : Xbox 360 Emulator Research Project; BSD license, see LICENSE. */
#pragma once
#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <system_error>
namespace xe::cpu { class ThreadState; }
namespace xe::kernel {
class KernelState;
class XThread;
class NativeProfileIdentityOwner;
class NativeSystemUiService;
namespace xam {
namespace test { class ProfileSettingsReadTestAccess; }
class ProfileManager;
struct ProfileRosterAuthority;
class UserProfile;
class ProfileRosterIntent;
class ProfileRosterPublicationCell;
class ProfileRosterNativeCompletion;
class ProfileRosterNativeBinding;
class ProfileRosterNativeReadTicket;
class ProfileRosterNativeReadScope;

struct ProfileRosterSlot final {
  uint64_t xuid = 0, sign_in_generation = 0;
  uint32_t device_id = 0;
  uint64_t source_generation = 0;
  std::string source_id;
  bool operator==(const ProfileRosterSlot&) const = default;
};
// Public snapshots are immutable owned metadata, never strong native/MVA/model
// owners. Internal intent envelopes retain actual row/source/Account/models.
class HostRoster final {
 public:
  const std::array<ProfileRosterSlot, 4>& slots() const;
  uint64_t epoch() const;
 private:
  friend class ProfileManager;
  friend struct ProfileRosterAuthority;
  struct State;
  explicit HostRoster(std::shared_ptr<const State>);
  std::shared_ptr<const State> state_;
};
struct ProfileRosterSelectionOrigin final {
  uint32_t device_id = 0;
  std::string source_id;
  uint64_t source_generation = 0;
  // When supplied by original224, comparison against EncodeRow of the exact
  // selected durable Account is mandatory. These bytes are not authority.
  std::optional<std::array<uint8_t, 0x188>> original_account_row;
};
struct ProfileRosterDesired final {
  // Existing host compatibility policy only, never native ABI/0A encoding.
  bool notify_host_compatibility = true;
  bool remember_slots = true;
  std::array<uint64_t, 4> xuids{};
  std::array<std::optional<ProfileRosterSelectionOrigin>, 4> origins;
};
struct ProfileRosterExpected final {
  std::shared_ptr<const HostRoster> roster;
  // Opaque weak issuer/lifetime proof is private in the production definition.
 private:
  friend class ProfileManager;
  friend struct ProfileRosterAuthority;
  struct Proof;
  std::shared_ptr<const Proof> proof_; // metadata only, weak native references
};
enum class ProfileRosterPhase : uint8_t {
  kPreparing, kPrepared, kNativeEntered, kPublished, kFinalizing,
  kCommitted, kCanceling, kCanceled, kUncertain
};
// Only authenticated native publisher branches select one of these preheld
// outcomes. The requested roster and its wire rows remain immutable.
enum class ProfileRosterPublicationOutcome : uint8_t { kRequested, kCanceledEmpty };
struct ProfileRosterResult final {
  std::error_code error;
  ProfileRosterPhase phase = ProfileRosterPhase::kPreparing;
  uint64_t before_epoch = 0, after_epoch = 0;
  std::array<bool, 4> changed{};
  // Independent factual host duties. Published/map commitment is not native
  // resource retirement or permission to reveal identity pixels.
  bool native_protocol_returned = false, selected_reads_available = false;
  bool maps_committed = false, writers_available = false, owners_retired = false;
  uint32_t native_protocol_value = 0;
};
class ProfileRosterIntent final {
 public:
  std::shared_ptr<const HostRoster> old_roster() const;
  std::shared_ptr<const HostRoster> prepared_roster() const;
  std::shared_ptr<const HostRoster> published_roster() const;
  std::optional<ProfileRosterPublicationOutcome> publication_outcome() const;
  ProfileRosterResult result() const; // value-only, no readiness claim
  bool data_handoff_complete() const noexcept;
 private:
  // Actual PM-issued before/after envelope emptiness plus native protocol/map
  // duties. This is not a frame, provider census, pixel or writer permission.
  bool HasPublishedEmptyUnchangedRows(uint32_t actual_protocol_value) const;
  // Initial alignment may retain exact row owners and complete Account
  // snapshots with at least one occupied row. Empty uses its zero-wire proof.
  // This is no replacement or image-read receipt.
  bool HasPublishedUnchangedOwners(uint32_t actual_protocol_value) const;
  bool HasPublishedRequestedRows(uint32_t actual_protocol_value) const;
  const std::array<uint8_t, 4 * 0x188>& NativeRowsForPrepared() const;
  // Fixed old work never follows the publisher selector. Only exact writer
  // descendants may receive the separately preallocated following ticket.
  std::shared_ptr<ProfileRosterNativeReadTicket> NativeReadTicket(
      bool follow_writer_selector, const ProfileRosterNativeBinding&,
      std::error_code* error = nullptr);
 private:
  friend class ProfileManager;
  friend struct ProfileRosterAuthority;
  friend class ProfileRosterNativeCompletion;
  friend class ProfileRosterPublicationCell;
  friend class ::xe::kernel::NativeProfileIdentityOwner;
  friend class test::ProfileSettingsReadTestAccess;
  struct State;
  explicit ProfileRosterIntent(std::shared_ptr<State>);
  std::shared_ptr<State> state_;
};
// This is not an issuer/source/current-Task proof. Native owner must prebind it
// after actual preparation, native entry and source/frame/Task authentication.
// Both actual resource envelopes are preheld outside this cell before PPC.
class ProfileRosterPublicationCell final {
 private:
  friend class test::ProfileSettingsReadTestAccess;
  friend class ProfileManager;
  friend struct ProfileRosterAuthority;
  friend class ::xe::kernel::NativeProfileIdentityOwner;
  bool PublishFromNative(ProfileRosterPublicationOutcome =
      ProfileRosterPublicationOutcome::kRequested) noexcept; // atomics only
  struct State;
  explicit ProfileRosterPublicationCell(std::shared_ptr<State>);
  // Independently retained revocable metadata; never a dangling intent pointer.
  std::shared_ptr<State> state_;
};
// The concrete native owner issues distinct protocol and full-join roles.
// A protocol own-return permits only selected reads/canonical host commitment;
// it can never substitute for actual resource retirement or visual joins.
class ProfileRosterNativeCompletion final {
 private:
  friend class ::xe::kernel::NativeProfileIdentityOwner;
  friend class ProfileManager;
  friend struct ProfileRosterAuthority;
  friend class test::ProfileSettingsReadTestAccess;
  static std::shared_ptr<const ProfileRosterNativeCompletion> ForProtocol(
      const std::shared_ptr<ProfileRosterIntent>&,
      const std::shared_ptr<const ProfileRosterNativeBinding>&,
      uint64_t claimed_nonce, uint32_t actual_native_value);
  // Owner issues only after the actual returned producer has left every
  // data-use scope. Presentation/Task/envelope reclamation is a later join.
  static std::shared_ptr<const ProfileRosterNativeCompletion> ForDataHandoff(
      const std::shared_ptr<ProfileRosterIntent>&,
      const std::shared_ptr<const ProfileRosterNativeBinding>&,
      uint64_t claimed_nonce);
  static std::shared_ptr<const ProfileRosterNativeCompletion> ForJoined(
      const std::shared_ptr<ProfileRosterIntent>&,
      const std::shared_ptr<const ProfileRosterNativeBinding>&,
      uint64_t claimed_nonce);
  struct State;
  explicit ProfileRosterNativeCompletion(std::shared_ptr<const State>);
  std::shared_ptr<const State> state_;
};

// Factory claim is once-only and specific to the explicit native identity
// family; ordinary Guide/Avatar-only service startup does not claim it.
class ProfileRosterNativeBinding final {
 private:
  friend class test::ProfileSettingsReadTestAccess;
  friend class ProfileRosterNativeCompletion;
  friend class ProfileManager;
  friend struct ProfileRosterAuthority;
  friend class ::xe::kernel::NativeSystemUiService;
  friend class ::xe::kernel::NativeProfileIdentityOwner;
  uint64_t nonce() const noexcept;
  struct State;
  explicit ProfileRosterNativeBinding(std::shared_ptr<State>);
  std::shared_ptr<State> state_; // metadata/weak actual-owner target only
};
class ProfileRosterNativeReadTicket final {
 private:
  friend class ProfileRosterIntent;
  friend class ProfileRosterNativeReadScope;
  friend class ProfileManager;
  friend struct ProfileRosterAuthority;
  friend class ::xe::kernel::NativeProfileIdentityOwner;
  struct State;
  explicit ProfileRosterNativeReadTicket(std::shared_ptr<State>);
  std::shared_ptr<State> state_; // preheld resource envelope, host reaped
};
// Preallocated in the native whole-work record before PPC. ThreadState carries
// this actual object pointer while entered. Enter/Leave are scalar-only; real
// retained whole-work/root/domain/task owners outlive Leave and are host reaped.
class ProfileRosterNativeReadScope final {
 public:
  ~ProfileRosterNativeReadScope();
  ProfileRosterNativeReadScope(const ProfileRosterNativeReadScope&) = delete;
 private:
  friend class ProfileManager;
  friend struct ProfileRosterAuthority;
  friend class ::xe::kernel::NativeProfileIdentityOwner;
  friend class test::ProfileSettingsReadTestAccess;
  static std::unique_ptr<ProfileRosterNativeReadScope> Prepare(
      std::shared_ptr<ProfileRosterNativeReadTicket>,
      std::shared_ptr<const ProfileRosterNativeBinding>);
  struct State;
  explicit ProfileRosterNativeReadScope(std::unique_ptr<State>);
  bool Enter(KernelState*, XThread*, cpu::ThreadState*, uint32_t task,
      uint64_t task_birth_generation, uint64_t whole_work_generation,
      uint32_t root_hmodule, uint64_t root_load_generation) noexcept;
  // A real bounded SYSTEM-thread body may have no XamTask. The native owner
  // already retains its exact code/domain/thread/frame and chooses the same
  // current/prepared ticket before entry. This grants no Task/birth identity.
  bool EnterSystemThreadFrame(KernelState*, XThread*, cpu::ThreadState*,
      uint32_t entry_sp, uint32_t entry_lr, uint32_t function,
      uint64_t whole_work_generation, uint32_t root_hmodule,
      uint64_t root_load_generation) noexcept;
  bool Leave() noexcept;
  // Owner-only final rundown after actual CPU/GPU stop with retained XThreads,
  // in reverse actual entry order. Never an ordinary cancellation shortcut.
  bool DetachAtActualKernelStop() noexcept;
  static const ProfileRosterNativeReadScope* FromActualThread(
      KernelState*, cpu::ThreadState*) noexcept;
  std::unique_ptr<State> state_; // allocated before nativeSchedule/PPC
};

} // namespace xam
} // namespace xe::kernel
