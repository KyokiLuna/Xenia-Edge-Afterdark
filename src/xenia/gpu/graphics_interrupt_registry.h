/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_GPU_GRAPHICS_INTERRUPT_REGISTRY_H_
#define XENIA_GPU_GRAPHICS_INTERRUPT_REGISTRY_H_

#include <array>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

namespace xe::kernel {
class XThread;
}
namespace xe {
class ByteStream;
}

namespace xe::gpu {

// Registration provenance is not submission provenance. Existing PM4 and
// vblank producers intentionally use DispatchLegacy until their owner is known.
class GraphicsInterruptRegistry {
 public:
  static constexpr uint32_t kUnclassified = UINT32_MAX;
  struct Pair {
    uint32_t callback = 0;
    uint32_t user_data = 0;
  };
  class Registration {
   public:
    uint32_t callback() const { return callback_; }
    uint32_t user_data() const { return user_data_; }
    uint32_t process_type() const { return process_type_; }
    uint32_t owner_hmodule() const { return owner_hmodule_; }
    uint64_t generation() const { return generation_; }

   private:
    friend class GraphicsInterruptRegistry;
    Registration(Pair pair, uint32_t process_type, uint64_t generation,
                 uint32_t owner_hmodule, std::shared_ptr<void> owner_pin);
    using Executor = std::function<void(const Registration&)>;
    bool Invoke(const Executor& executor) const;
    void Retire() const;
    bool CanWaitOnCurrentThread() const;
    bool StopAndWait() const;
    struct InvocationKey {
      const kernel::XThread* fiber = nullptr;
      std::thread::id host;
    };
    static InvocationKey CurrentInvocationKey();
    void EndCall(const InvocationKey& key) const;
    struct Invocation {
      const Registration* registration;
      InvocationKey key;
      ~Invocation() { registration->EndCall(key); }
    };
    const uint32_t callback_;
    const uint32_t user_data_;
    const uint32_t process_type_;
    const uint32_t owner_hmodule_;
    const uint64_t generation_;
    // Protects code lifetime only, not arbitrary guest device/user_data memory.
    const std::shared_ptr<void> owner_pin_;
    mutable std::mutex call_mutex_;
    mutable std::condition_variable calls_drained_;
    mutable bool retired_ = false;
    mutable size_t active_calls_ = 0;
    mutable std::unordered_map<const kernel::XThread*, size_t> active_fibers_;
    mutable std::unordered_map<std::thread::id, size_t> active_hosts_;
  };
  using Snapshot = std::shared_ptr<const Registration>;
  using Executor = std::function<void(const Registration&)>;

  // The loader serializes module admission with its unload claim. Replacement
  // retires that owner's old generation; other owner registrations survive.
  // Every call also updates the legacy last-registration view, including clear.
  void Set(Pair pair, uint32_t process_type = kUnclassified,
           uint32_t owner_hmodule = 0, std::shared_ptr<void> owner_pin = {});
  Pair legacy_pair() const;
  bool DispatchLegacy(const Executor& executor) const;

  // Explicit routing requires a known process and a pinned registration. No
  // CPU mask, device pointer or host worker can supply that missing provenance.
  Snapshot CaptureOwner(uint32_t process_type) const;
  bool DispatchOwner(const Snapshot& token, uint32_t process_type,
                     uint64_t generation, const Executor& executor) const;

  // Stop admission before destructive module detach, outside the kernel lock.
  // False means a self-call or scheduler fiber must defer the unload retry.
  // Prepare never waits, so the loader can preflight before draining another
  // callback registry whose active calls may depend on this one.
  bool PrepareModuleShutdown(uint32_t hmodule);
  bool BeginModuleShutdown(uint32_t hmodule);
  void RetireModule(uint32_t hmodule);
  // Irreversible whole-kernel/cohort stop. Retire all generations before
  // waiting outside the registry lock. False requires an external retry after
  // the current self/fiber invocation returns. Clear/Restore cannot reopen it.
  bool StopAndWait();
  bool IsStopped() const;
  void Clear();
  // State files still contain only the legacy pair. Restoring invalidates all
  // previous tokens and starts a new unclassified generation.
  void RestoreLegacy(Pair pair);
  void SaveLegacy(ByteStream* stream) const;
  void RestoreLegacy(ByteStream* stream);

 private:
  static size_t OwnerSlot(uint32_t process_type);
  std::vector<Snapshot> ModuleSnapshots(uint32_t hmodule) const;
  mutable std::mutex mutex_;
  bool stopped_ = false;
  uint64_t next_generation_ = 1;
  Pair legacy_pair_;
  Snapshot legacy_;
  std::array<Snapshot, 4> owners_{};
  std::vector<std::weak_ptr<const Registration>> registrations_;
};

}  // namespace xe::gpu

#endif  // XENIA_GPU_GRAPHICS_INTERRUPT_REGISTRY_H_
