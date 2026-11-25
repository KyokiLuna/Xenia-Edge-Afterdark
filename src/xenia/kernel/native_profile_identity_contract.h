/** Xenia : Xbox 360 Emulator Research Project; BSD license, see LICENSE. */
#pragma once
#include <atomic>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
namespace xe::kernel {
struct ValidatedGuestRuntimePlan;
struct NativeProfileIdentityBridgeProfile {
  uint16_t ordinal;uint32_t address,bytes;std::string_view sha256;
};
std::span<const NativeProfileIdentityBridgeProfile> NativeProfileIdentityBridgeProfiles();
const NativeProfileIdentityBridgeProfile* NativeProfileIdentityBridge(uint16_t);
struct NativeProfileIdentityIngressProfile {
  uint16_t ordinal;std::string_view identity;
  uint32_t address,bytes;std::string_view sha256;
};
std::span<const NativeProfileIdentityIngressProfile> NativeProfileIdentityIngressProfiles();
enum class NativeProfileIdentityPlanFamily {kAbsent,kComplete,kInvalid};
// Explicit coherent optional family only. Guide/Avatar plans without the two
// ingress roles/extra bridge witnesses never claim the common roster factory.
NativeProfileIdentityPlanFamily CheckNativeProfileIdentityPlanFamily(
    const ValidatedGuestRuntimePlan&,std::string* error=nullptr);
namespace identity_detail {
// Host HLE-stack lifetime only. This grants no native Task/read/completion
// authority. The service retains backing through the real CPU worker join.
class NativeIdentityHookRundown final {
  static_assert(std::atomic<uint64_t>::is_always_lock_free);
 public:
  struct Snapshot {
    uint32_t active = 0, parkable = 0;
    bool parkable_sealed = false;
    bool fully_drained() const {
      return parkable_sealed && !active && !parkable;
    }
    bool retain_until_worker_join() const {
      return parkable_sealed && !parkable;
    }
  };
  class Use final {
   public:
    Use() = default;
    Use(const Use&) = delete;
    Use& operator=(const Use&) = delete;
    Use(Use&& other) noexcept
        : control_(std::move(other.control_)),
          backing_(std::move(other.backing_)),
          parkable_(std::exchange(other.parkable_, false)) {}
    Use& operator=(Use&& other) noexcept {
      if (this != &other) {
        Reset();
        control_ = std::move(other.control_);
        backing_ = std::move(other.backing_);
        parkable_ = std::exchange(other.parkable_, false);
      }
      return *this;
    }
    ~Use() { Reset(); }
    bool Acquire(std::shared_ptr<NativeIdentityHookRundown> control,
                 std::shared_ptr<const void> backing,
                 bool potentially_parkable) noexcept {
      if (control_ || !control || !backing) return false;
      auto word = control->word_.load(std::memory_order_acquire);
      for (;;) {
        if ((word & kCountMask) == kCountMask) return false;
        const bool parkable = potentially_parkable && !(word & kSealed);
        const uint64_t added = 1 + (parkable ? kParkOne : 0);
        if (control->word_.compare_exchange_weak(
                word, word + added, std::memory_order_acq_rel,
                std::memory_order_acquire)) {
          control_ = std::move(control);
          backing_ = std::move(backing);
          parkable_ = parkable;
          return true;
        }
      }
    }
    explicit operator bool() const noexcept { return bool(control_); }
    // A sealed new prebody invocation is counted but may ONLY take a proven
    // nonparking path. No token here grants native body/completion authority.
    bool may_park() const noexcept { return control_ && parkable_; }
    bool Matches(const NativeIdentityHookRundown* control,
                 const void* backing) const noexcept {
      return control_.get() == control && backing_.get() == backing;
    }
    void Reset() noexcept {
      auto control = std::move(control_);
      const bool parked = std::exchange(parkable_, false);
      // The independent scalar control survives every owning frame local,
      // including the last backing pin. Publish release only after that drop.
      backing_.reset();
      if (control)
        control->word_.fetch_sub(1 + (parked ? kParkOne : 0),
                                 std::memory_order_release);
    }
   private:
    std::shared_ptr<NativeIdentityHookRundown> control_;
    std::shared_ptr<const void> backing_;
    bool parkable_ = false;
  };
  // Total count, parkable count and its admission seal share one CAS cut.
  // A total-count overflow refuses a new use rather than wrapping a proof.
  Snapshot Capture() const noexcept {
    const auto word = word_.load(std::memory_order_acquire);
    return {uint32_t(word & kCountMask),
            uint32_t((word >> kParkShift) & kCountMask), bool(word & kSealed)};
  }
  void SealParkable() noexcept {
    word_.fetch_or(kSealed, std::memory_order_acq_rel);
  }
 private:
  static constexpr uint32_t kParkShift = 31;
  static constexpr uint64_t kCountMask = (uint64_t{1} << kParkShift) - 1;
  static constexpr uint64_t kParkOne = uint64_t{1} << kParkShift;
  static constexpr uint64_t kSealed = uint64_t{1} << 63;
  std::atomic<uint64_t> word_{0};
};
}  // namespace identity_detail
} // namespace xe::kernel
