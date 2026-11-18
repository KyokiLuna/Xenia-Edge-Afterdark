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
struct ProfileRosterResult final {
  std::error_code error;
  ProfileRosterPhase phase = ProfileRosterPhase::kPreparing;
  uint64_t before_epoch = 0, after_epoch = 0;
  std::array<bool, 4> changed{};
};
class ProfileRosterIntent final {
 public:
  std::shared_ptr<const HostRoster> old_roster() const;
  std::shared_ptr<const HostRoster> prepared_roster() const;
  ProfileRosterResult result() const; // value-only, no readiness claim
 private:
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
  friend class ProfileManager;
  friend struct ProfileRosterAuthority;
  friend class ::xe::kernel::NativeProfileIdentityOwner;
  bool PublishFromNative() noexcept; // atomics/scalars only, no release/lookup
  struct State;
  explicit ProfileRosterPublicationCell(std::shared_ptr<State>);
  // Independently retained revocable metadata; never a dangling intent pointer.
  std::shared_ptr<State> state_;
};
// Only concrete native owner mints this after its factual publisher, listener,
// wake/provider output, descendant/task and renderer old/new-origin join.
class ProfileRosterNativeCompletion final {
 private:
  friend class ::xe::kernel::NativeProfileIdentityOwner;
  friend class ProfileManager;
  friend struct ProfileRosterAuthority;
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
  static std::unique_ptr<ProfileRosterNativeReadScope> Prepare(
      std::shared_ptr<ProfileRosterNativeReadTicket>,
      std::shared_ptr<const ProfileRosterNativeBinding>);
  struct State;
  explicit ProfileRosterNativeReadScope(std::unique_ptr<State>);
  bool Enter(KernelState*, XThread*, cpu::ThreadState*, uint32_t task,
      uint64_t task_birth_generation, uint64_t whole_work_generation,
      uint32_t root_hmodule, uint64_t root_load_generation) noexcept;
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
