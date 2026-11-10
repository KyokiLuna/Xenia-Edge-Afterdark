/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia contributors. All rights reserved.                    *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */
#ifndef XENIA_GPU_NATIVE_SYSTEM_TITLE_INPUT_PROVIDER_H_
#define XENIA_GPU_NATIVE_SYSTEM_TITLE_INPUT_PROVIDER_H_
#include <array>
#include <memory>
#include <mutex>
#include <optional>
#include "xenia/gpu/native_system_title_input.h"
namespace xe::gpu {
// CPU-only rendezvous. A ticket authorizes no GPU work and cannot create the
// immutable input; only the real backend can mint one after its copy fence.
class NativeSystemTitleInputProvider final {
 public:
  using Input = NativeSystemTitleInput;
  struct Capture {
    Input::Identity identity;
    Input::Layout layout;
  };
  enum class Availability { kUnknown, kProvenNoInitialSwap, kReady };
  // Exact enabled backend only. Before the first Stage the trusted caller
  // proves foreground/Title against the real Kernel; they aren't yet stored
  // here. Later observations also match the stored cohort. Unsupported,
  // in-flight, stale or closed input is never equivalent to no initial Swap.
  Availability ObserveAvailability(uint64_t backend_epoch, uint64_t foreground,
                                   uint32_t title_hmodule,
                                   uint64_t title_load_generation) const;
  bool Enable(uint64_t backend_epoch);
  bool enabled() const;
  uint64_t Stage(const Input::Swap& swap);
  std::optional<Capture> Take(uint64_t ticket, uint32_t physical,
                              uint32_t width, uint32_t height);
  bool Publish(std::shared_ptr<const Input> input);
  std::shared_ptr<const Input> Latest(uint64_t foreground_generation) const;
  // Irreversible for this backend instance. CPU bytes, not GPU handles, may
  // remain held by already admitted Sources after this cache is cleared.
  void Close();

 private:
  struct Pending {
    uint64_t ticket = 0;
    Input::Swap swap;
  };
  static constexpr size_t kPendingSlots = 64;
  mutable std::mutex mutex_;
  uint64_t backend_epoch_ = 0, foreground_ = 0;
  uint32_t title_hmodule_ = 0;
  uint64_t title_load_generation_ = 0;
  uint64_t next_ticket_ = 1, next_capture_ = 1;
  uint64_t invalidation_revision_ = 0, capturing_revision_ = 0;
  std::optional<Capture> capturing_;
  bool closed_ = false, staged_any_ = false;
  std::array<Pending, kPendingSlots> pending_{};
  std::shared_ptr<const Input> latest_;
};
}  // namespace xe::gpu
#endif
