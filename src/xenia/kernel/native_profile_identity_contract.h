/** Xenia : Xbox 360 Emulator Research Project; BSD license, see LICENSE. */
#pragma once
#include <atomic>
#include <cstdint>
#include <chrono>
#include <optional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include "xenia/kernel/native_profile_identity_tasks.h"
namespace xe::kernel {
class KernelState;
class XThread;
class NativeProfileIdentityOwner;
class NativeSystemUiService;
namespace test { class NativeIdentityOrdinaryTransportTestAccess; }
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
// The existing Service OS-thread join produces this exact stack witness.
// A terminated KTHREAD, Cancel/Wait return or is_running=false cannot issue it.
// Its one retained actual XThread also prevents object-address reuse.
class NativeIdentityJoinedHostStack final {
 public:
  NativeIdentityJoinedHostStack(const NativeIdentityJoinedHostStack&) = default;
  NativeIdentityJoinedHostStack(NativeIdentityJoinedHostStack&&) = default;
  NativeIdentityJoinedHostStack& operator=(const NativeIdentityJoinedHostStack&) = default;
  NativeIdentityJoinedHostStack& operator=(NativeIdentityJoinedHostStack&&) = default;
 private:
  friend class ::xe::kernel::NativeSystemUiService;
  friend class NativeIdentityOrdinaryTransport;
  friend class ::xe::kernel::test::NativeIdentityOrdinaryTransportTestAccess;
  static std::optional<NativeIdentityJoinedHostStack> JoinActual(
      XThread&, std::chrono::milliseconds);
  NativeIdentityJoinedHostStack(KernelState* kernel, std::shared_ptr<const XThread> thread,
      const void* cpu, const void* host)
      :kernel_(kernel),thread_(std::move(thread)),cpu_(cpu),host_(host) {}
  bool Matches(const XThread&) const noexcept;
  KernelState* kernel_;
  std::shared_ptr<const XThread> thread_;
  const void* cpu_;
  const void* host_;
};
// A routing memento from ONE of the two actual ordinary-shadow constructors.
// It owns no Task, callback, read, Commit, native code/domain or completion.
// The existing Work owns the actual XThread; this immutable tuple cannot be
// retargeted. Only actual whole-Kernel stop may retire an unreturned shadow.
class NativeIdentityOrdinaryTransport final {
 public:
  using Frame = NativeProfileIdentityTasks::Frame;
  using Root = NativeProfileIdentityTasks::Root;
  using CompletionRoute = NativeProfileIdentityTasks::CompletionRoute;
 private:
  friend class ::xe::kernel::NativeProfileIdentityOwner;
  friend class ::xe::kernel::test::NativeIdentityOrdinaryTransportTestAccess;
  enum class Kind {kTaskMetadata,kOrdinaryCompletion};
  enum class Phase {kEntered,kReturned,kUncertain};
  static std::optional<NativeIdentityOrdinaryTransport> CaptureTask(Frame,XThread&) noexcept;
  static std::optional<NativeIdentityOrdinaryTransport> CaptureCompletion(
      Frame,XThread&,const CompletionRoute&) noexcept;
  static std::optional<NativeIdentityOrdinaryTransport> Capture(Frame,XThread&,Kind,const CompletionRoute* = nullptr) noexcept;
  static bool TaskTransportFunction(uint32_t) noexcept;
  bool Matches(Frame,const XThread&) const noexcept;
  bool Return(Frame,const XThread&) noexcept;
  void UnavailableReturn() noexcept {if(phase_!=Phase::kReturned)phase_=Phase::kUncertain;}
  bool RetainThroughActualStop(KernelState*,Root,Frame,const XThread&,
      const CompletionRoute*,const NativeIdentityJoinedHostStack*) const noexcept;
  NativeIdentityOrdinaryTransport(Frame frame,XThread& thread,KernelState* kernel,
      const void* cpu,const void* fiber,const void* host,bool guest,Kind kind,const CompletionRoute* route)
      :frame_(frame),thread_(&thread),kernel_(kernel),cpu_(cpu),fiber_(fiber),host_(host),
       guest_fiber_(guest),kind_(kind),route_(route) {}
  const Frame frame_;
  const XThread* const thread_;
  KernelState* const kernel_;
  const void* const cpu_;
  const void* const fiber_;
  const void* const host_;
  const bool guest_fiber_;
  const Kind kind_;
  // The real Work is heap-stable and owns this exact route until this memento
  // dies. This pointer is only compared, never dereferenced after ownership.
  const CompletionRoute* const route_;
  Phase phase_=Phase::kEntered;
};
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
