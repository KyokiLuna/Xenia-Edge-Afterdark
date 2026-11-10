/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia contributors. All rights reserved.                    *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */
#ifndef XENIA_KERNEL_NATIVE_SYSTEM_GRAPHICS_NOTIFICATIONS_H_
#define XENIA_KERNEL_NATIVE_SYSTEM_GRAPHICS_NOTIFICATIONS_H_

#include <cstdint>
#include <memory>

namespace xe::kernel {
class KernelState;
class NativeImportDomain;
class NativeSystemCallbackDispatcher;
class NativeSystemUiStartupCatalog;

// Transport for the supplied native XAM graphics-notification receiver.
// Native code owns its flags, spinlocks, Tasks and display/resource policy.
// Only the bounded payload-copy variants 1, 3 and 9 are established here.
// In particular, this is not an implementation of the unverified kernel
// notification producers or of the receiver's Task/event handshake variants.
class NativeSystemGraphicsNotifications {
 public:
  struct Config {
    std::shared_ptr<const NativeSystemUiStartupCatalog> catalog;
    std::shared_ptr<NativeImportDomain> domain;
    std::shared_ptr<NativeSystemCallbackDispatcher> dispatcher;
  };
  static std::shared_ptr<NativeSystemGraphicsNotifications> Create(Config);
  static std::shared_ptr<NativeSystemGraphicsNotifications> Lookup(
      KernelState*);
  ~NativeSystemGraphicsNotifications();
  bool Register(uint32_t callback, uint32_t return_address);
  bool IsRegistered() const;
  // Copies the complete stack payload before handing it to the SYSTEM
  // dispatcher and waits cooperatively on the original TITLE/SYSTEM caller.
  // true means the exact registered receiver actually returned. Neither its
  // r3 value nor callback delivery implies a rendered frame or GPU completion.
  bool Call(uint32_t kind, uint32_t payload, uint32_t return_address);
  void BeginStop() noexcept;
  // External host only, after native caller/bootstrap ingress has closed.
  // Providers/dispatcher/Kernel memory remain alive through this drain.
  bool Drain() noexcept;

 private:
  struct State;
  explicit NativeSystemGraphicsNotifications(std::shared_ptr<State>);
  void RetireAfterKernelStop() noexcept;
  std::shared_ptr<State> state_;
};
}  // namespace xe::kernel
#endif  // XENIA_KERNEL_NATIVE_SYSTEM_GRAPHICS_NOTIFICATIONS_H_
