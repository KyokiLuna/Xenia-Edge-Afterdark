/** Xenia : Xbox 360 Emulator Research Project; BSD license, see LICENSE. */
#ifndef XENIA_KERNEL_XAM_PROFILE_ACCOUNT_SNAPSHOT_H_
#define XENIA_KERNEL_XAM_PROFILE_ACCOUNT_SNAPSHOT_H_
#include <atomic>
#include <array>
#include <bitset>
#include <memory>
#include <span>
#include <vector>
#include "xenia/base/snapshot_publisher.h"
#include "xenia/kernel/xam/profile_gpd_publication.h"
#include "xenia/kernel/xam/xam.h"
namespace xe::kernel::xam {
// An immutable ownership record. Retirement is one-way and independent of GPD
// model revisions. The manager retains the exact record for a signed-in slot.
class ProfileAccountIdentity final
    : public std::enable_shared_from_this<ProfileAccountIdentity> {
 public:
  static std::shared_ptr<ProfileAccountIdentity> Capture(
      std::shared_ptr<ProfileStorageLease>, uint8_t slot,
      uint64_t sign_in_generation, std::error_code&);
  const auto& storage() const { return storage_; }
  uint8_t slot() const { return slot_; }
  uint64_t sign_in_generation() const { return generation_; }
  uint64_t write_serial() const { return write_serial_.load(std::memory_order_acquire); }
  bool retired() const { return retired_.load(std::memory_order_acquire); }
  void Retire() {
    writes_.fetch_or(kClosed, std::memory_order_acq_rel);
    retired_.store(true, std::memory_order_release);
  }
  bool writes_drained() const { return !(writes_.load(std::memory_order_acquire) & ~kClosed); }
  class Write final {
   public:
    ~Write();
   private:
    friend class ProfileAccountIdentity;
    explicit Write(std::shared_ptr<ProfileAccountIdentity> owner)
        : owner_(std::move(owner)) {}
    std::shared_ptr<ProfileAccountIdentity> owner_;
  };
  std::unique_ptr<Write> EnterWrite(std::error_code&);
  const std::atomic<bool>* cancellation() const { return &retired_; }
 private:
  ProfileAccountIdentity(std::shared_ptr<ProfileStorageLease> storage,
                         uint8_t slot, uint64_t generation)
      : storage_(std::move(storage)), slot_(slot), generation_(generation) {}
  const std::shared_ptr<ProfileStorageLease> storage_;
  const uint8_t slot_;
  const uint64_t generation_;
  static constexpr uint64_t kClosed = uint64_t(1) << 63;
  std::atomic<uint64_t> writes_{0};
  std::atomic<uint64_t> write_serial_{1};
  std::atomic<bool> retired_{false};
};
// This receipt is minted only after authenticating the complete encrypted
// prefix AND a real file/parent durability barrier on this exact token. It
// confers no native-provider freshness, profile-creation or Live authority.
class ProfileAccountSnapshot final {
 public:
  static constexpr size_t kPrefixBytes = sizeof(X_XAMACCOUNTINFO) + 0x18;
  static constexpr size_t kMaximumBytes = 1u << 20;
  static std::shared_ptr<const ProfileAccountSnapshot> Load(
      std::shared_ptr<ProfileAccountIdentity>, std::error_code&);
  const auto& identity() const { return identity_; }
  const auto& token() const { return token_; }
  const X_XAMACCOUNTINFO& account() const { return account_; }
  bool devkit() const { return devkit_; }
  std::error_code CheckAvailable() const;
 private:
  friend class ProfileAccountWriter;
  static std::shared_ptr<const ProfileAccountSnapshot> FromDurableToken(
      std::shared_ptr<ProfileAccountIdentity>,
      std::shared_ptr<const filesystem::HostStorageSnapshotToken>,
      std::error_code&);
  ProfileAccountSnapshot(std::shared_ptr<ProfileAccountIdentity> identity,
      std::shared_ptr<const filesystem::HostStorageSnapshotToken> token,
      const X_XAMACCOUNTINFO& account, bool devkit)
      : identity_(std::move(identity)), token_(std::move(token)),
        account_(account), devkit_(devkit) {}
  const std::shared_ptr<ProfileAccountIdentity> identity_;
  const std::shared_ptr<const filesystem::HostStorageSnapshotToken> token_;
  const X_XAMACCOUNTINFO account_;
  const bool devkit_;
};
// Cached metadata can outlive an unmounted profile without pinning its device,
// source lease, private recovery owner or guest admission. It is a value derived
// from a durable authenticated receipt; it grants no currentness authority.
class ProfileAccountMetadata final {
 public:
  explicit ProfileAccountMetadata(const ProfileAccountSnapshot& snapshot)
      : account_(snapshot.account()),
        xuid_(snapshot.identity()->storage()->xuid()),
        device_(snapshot.identity()->storage()->device_id()) {}
  const X_XAMACCOUNTINFO& account() const { return account_; }
  uint64_t xuid() const { return xuid_; }
  uint32_t device_id() const { return device_; }
 private:
  const X_XAMACCOUNTINFO account_;
  const uint64_t xuid_;
  const uint32_t device_;
};
struct ProfileAccountAttempt {
  std::error_code error;
  filesystem::SnapshotPublicationReceipt storage;
  // Non-null only after exact authenticated bytes really reached storage.
  // A later manager/cache refusal does not turn this into native readiness.
  std::shared_ptr<const ProfileAccountSnapshot> durable;
};
class PreparedProfileAccountWrite final {
 public:
  const auto& storage() const { return prepared_->receipt(); }
 private:
  friend class ProfileAccountWriter;
  PreparedProfileAccountWrite() = default;
  std::shared_ptr<ProfileAccountIdentity> identity_;
  std::unique_ptr<ProfileAccountIdentity::Write> ownership_;
  std::unique_ptr<filesystem::PreparedSnapshotPublication> prepared_;
};
class ProfileAccountWriter final {
 public:
  ProfileAccountWriter(std::shared_ptr<ProfileAccountIdentity> identity,
      std::shared_ptr<const filesystem::SnapshotRecoveryOwner> recovery)
      : identity_(std::move(identity)), recovery_(std::move(recovery)) {}
  ProfileAccountAttempt Replace(
      std::shared_ptr<const ProfileAccountSnapshot> expected,
      const X_XAMACCOUNTINFO&) const;
  // Creation is Account-only data work. Its receipt does not advertise a
  // complete profile; callers must keep full creation unavailable separately.
  ProfileAccountAttempt Create(const X_XAMACCOUNTINFO&) const;
  // Shared by typed updates and guest byte sessions: no authenticated old
  // payload is silently replaced, and unknown suffix bytes are never erased.
  std::unique_ptr<PreparedProfileAccountWrite> PrepareRaw(
      std::shared_ptr<const filesystem::HostStorageSnapshotToken> expected,
      std::span<const uint8_t> bytes, std::error_code&) const;
  ProfileAccountAttempt Commit(PreparedProfileAccountWrite&) const;
  ProfileAccountAttempt RetryDurability(const ProfileAccountAttempt&) const;
  ProfileAccountAttempt PublishRaw(
      std::shared_ptr<const filesystem::HostStorageSnapshotToken> expected,
      std::span<const uint8_t> bytes) const;
 private:
  const std::shared_ptr<ProfileAccountIdentity> identity_;
  const std::shared_ptr<const filesystem::SnapshotRecoveryOwner> recovery_;
};
// Bounded guest-write state, distinct from a durability receipt. Writes may be
// split arbitrarily; only Flush can publish a complete authenticated image.
// Dropping/canceling an incomplete session leaves the original disk untouched.
class ProfileAccountByteSession final {
 public:
  ProfileAccountByteSession(ProfileAccountWriter writer,
      std::shared_ptr<const ProfileAccountSnapshot> expected, bool truncate);
  std::error_code Write(size_t offset, std::span<const uint8_t>);
  std::error_code Resize(size_t);
  std::span<const uint8_t> bytes() const { return bytes_; }
  bool prefix_written() const { return prefix_written_.all(); }
  ProfileAccountAttempt Flush();
 private:
  const ProfileAccountWriter writer_;
  std::shared_ptr<const ProfileAccountSnapshot> expected_;
  std::vector<uint8_t> bytes_;
  std::bitset<ProfileAccountSnapshot::kPrefixBytes> prefix_written_;
  bool dirty_ = false;
  ProfileAccountAttempt uncertain_;
};
// Shared committed Account/slot publication stamp. It is NOT the notification
// identity epoch: ordinary Account data changes cannot invalidate an award.
class ProfileAccountPublicationStamp final {
 public:
  uint64_t current() const { return value_.load(std::memory_order_acquire); }
  void Retire() { value_.store(0, std::memory_order_release); }
  bool Advance() {
    auto old = current();
    while (old && old != UINT64_MAX) {
      if (value_.compare_exchange_weak(old, old + 1,
          std::memory_order_acq_rel)) { return true; }
    }
    Retire();
    return false;
  }
 private:
  std::atomic<uint64_t> value_{1};
};
// Owned input ONLY for the recovered original 17559 ordinal224 producer.
// No private native cache rows, guessed Local flags, task receipt or readiness
// claim is generated. The later coordinator must still close/validate/retain
// this exact publication and join the real publisher plus descendant work.
class NativeLogonAccountInputs final {
 public:
  static constexpr size_t kRowBytes = 0x188;
  const std::array<uint8_t, 4 * kRowBytes>& bytes() const { return bytes_; }
  const auto& accounts() const { return accounts_; }
  uint64_t publication() const { return publication_; }
  bool OwnershipCurrent() const;
  // Pure wire codec used by the real input producer, over an authenticated
  // receipt. Only exact full rows are accepted; output is untouched on error.
  static std::error_code EncodeRow(const ProfileAccountSnapshot&,
                                    std::span<uint8_t>);
 private:
  friend class ProfileManager;
  NativeLogonAccountInputs() = default;
  std::array<std::shared_ptr<const ProfileAccountSnapshot>, 4> accounts_{};
  std::array<uint64_t, 4> write_serials_{};
  std::array<uint8_t, 4 * kRowBytes> bytes_{};
  std::shared_ptr<const ProfileAccountPublicationStamp> stamp_;
  uint64_t publication_ = 0;
};
}  // namespace xe::kernel::xam
#endif
