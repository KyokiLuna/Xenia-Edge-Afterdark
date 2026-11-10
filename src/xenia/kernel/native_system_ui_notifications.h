/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia contributors. All rights reserved.                    *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */
#ifndef XENIA_KERNEL_NATIVE_SYSTEM_UI_NOTIFICATIONS_H_
#define XENIA_KERNEL_NATIVE_SYSTEM_UI_NOTIFICATIONS_H_

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>

#include "xenia/base/physical_range_leases.h"
#include "xenia/cpu/ppc/ppc_context.h"
namespace xe::kernel {
class KernelState;
class NativeImportDomain;
class NativeSystemUiStartupCatalog;
namespace test {
class NativeSystemUiNotificationTestAccess;
}

// A native XAM SYS_UI notification, not a Vd graphics notification, a native
// listener handle, an input ownership counter or a rendering-completion token.
// Only the verified native broadcast exit can mint production instances.
class NativeSystemUiVisibilityEvent {
 public:
  const memory::PhysicalAllocationOwner& owner() const { return owner_; }
  uint64_t domain_id() const { return domain_id_; }
  uint64_t root_load_generation() const { return root_generation_; }
  uint64_t sequence() const { return sequence_; }
  uint64_t foreground_generation() const { return foreground_generation_; }
  bool visible() const { return visible_; }

 private:
  friend class NativeSystemUiNotifications;
  friend class test::NativeSystemUiNotificationTestAccess;
  memory::PhysicalAllocationOwner owner_;
  uint64_t domain_id_ = 0, root_generation_ = 0, sequence_ = 0;
  uint64_t foreground_generation_ = 0;
  bool visible_ = false;
};

class NativeSystemUiNotifications
    : public std::enable_shared_from_this<NativeSystemUiNotifications> {
 public:
  struct Config {
    std::shared_ptr<const NativeSystemUiStartupCatalog> catalog;
    std::shared_ptr<NativeImportDomain> domain;
    // Trusted bounded host metadata only. No PPC, GPU submission, waiting for
    // guest work, reentry into this observer or blocking teardown. Capture a
    // weak adapter reference. On failure/stop its authority must be revoked,
    // rather than manufacturing a native hide or leaving a stale plane live.
    std::function<void(const NativeSystemUiVisibilityEvent&)> host_observer;
    // Withdraw/repaint the old host plane on stop/poison. This revokes host
    // authority; it is never reported as a native hide or input counter.
    std::function<void()> host_revoked;
  };
  static std::shared_ptr<NativeSystemUiNotifications> Create(Config);
  static std::shared_ptr<NativeSystemUiNotifications> Lookup(KernelState*);
  ~NativeSystemUiNotifications();

  struct Snapshot {
    uint64_t last_sequence = 0, foreground_generation = 0;
    size_t pending = 0;
    bool known = false, visible = false, accepting = false, poisoned = false;
  };
  Snapshot snapshot() const noexcept;
  bool IsAccepting() const noexcept;
  // HLE listener startup uses the native state when present; no handle passes
  // between the registries. Stopped/unknown state supplies no replacement.
  static std::optional<bool> CurrentVisibility(KernelState*) noexcept;

  class UnlockObservation {
   public:
    UnlockObservation() = default;
    UnlockObservation(UnlockObservation&&) noexcept;
    UnlockObservation& operator=(UnlockObservation&&) noexcept;
    ~UnlockObservation();
    UnlockObservation(const UnlockObservation&) = delete;
    UnlockObservation& operator=(const UnlockObservation&) = delete;
    // Invoke once after the genuine KfReleaseSpinLock operation returned.
    // Existing kernel behavior and native body/return registers are untouched.
    void Complete() noexcept;

   private:
    friend class NativeSystemUiNotifications;
    explicit UnlockObservation(
        std::shared_ptr<NativeSystemUiNotifications> owner, uint64_t sequence);
    std::shared_ptr<NativeSystemUiNotifications> owner_;
    uint64_t sequence_ = 0;
  };
  // Called only by the actual KfReleaseSpinLock import shim, before releasing
  // the supplied native broadcast's final listener lock. Unrelated calls are
  // no-ops. The bounded reservation records native lock order but performs no
  // listener/presenter callback while that guest lock remains held.
  static UnlockObservation BeforeNativeListenerUnlock(
      KernelState*, const cpu::ppc::PPCContext*) noexcept;

  void BeginStop() noexcept;
  // External host only; after new native ingress is closed, with Kernel and
  // providers alive. Drains admitted actual shim frames before reclamation.
  bool Drain() noexcept;

 private:
  friend class test::NativeSystemUiNotificationTestAccess;
  struct State;
  // Host transport tests exercise the same bounded reservation/drainer with
  // no Kernel, guest code, native object or readiness authority.
  static std::shared_ptr<NativeSystemUiNotifications> CreateQueueForTest(
      std::function<void(const NativeSystemUiVisibilityEvent&)> deliver,
      std::function<void()> revoked);
  explicit NativeSystemUiNotifications(std::shared_ptr<State>);
  UnlockObservation Observe(const cpu::ppc::PPCContext*);
  UnlockObservation Reserve(NativeSystemUiVisibilityEvent,
                            std::shared_ptr<void> lifetime);
  UnlockObservation RevokeAfterUnlock();
  void PublishRevocation() noexcept;
  void Finish(uint64_t sequence, bool released) noexcept;
  void RetireAfterKernelStop() noexcept;
  std::shared_ptr<State> state_;
};
}  // namespace xe::kernel
#endif  // XENIA_KERNEL_NATIVE_SYSTEM_UI_NOTIFICATIONS_H_
