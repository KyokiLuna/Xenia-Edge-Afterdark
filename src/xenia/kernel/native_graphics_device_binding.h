/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia contributors. All rights reserved.                    *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */
#ifndef XENIA_KERNEL_NATIVE_GRAPHICS_DEVICE_BINDING_H_
#define XENIA_KERNEL_NATIVE_GRAPHICS_DEVICE_BINDING_H_

#include <cstdint>
#include <memory>
#include <string>

#include "xenia/gpu/gpu_completion.h"
#include "xenia/kernel/guest_runtime.h"
#include "xenia/kernel/xobject.h"

namespace xe::memory {
struct PhysicalAllocationOwner;
}

namespace xe::kernel {
class KernelState;
class UserModule;
class XThread;
class NativeFrameGraphAuthority;
class NativeSystemAllocationAuthority;
class NativeSystemFrameDelegation;
class NativeSystemUiService;
class NativeGraphicsDeviceBinding;
struct NativeSystemUiServiceContext;
struct NativeSystemSharedGraphicsState;

// Minted only by the generation-validated native startup catalogue / actual
// shared renderer readback. Not a public collection of admission booleans.
// The initializer's globals, DC ownership and factory layout must be validated
// before constructing it; none of these private guest fields are inferred by
// the allocation authority. No foreground HUD/VK module pin belongs here.
class NativeSystemGraphicsDeviceRecord {
 private:
  friend class NativeSystemUiService;
  friend class NativeGraphicsDeviceBinding;
  NativeSystemGraphicsDeviceRecord() = default;
  std::shared_ptr<NativeSystemAllocationAuthority> allocation;
  uint64_t root_load_generation = 0;
  GuestRuntime::Function factory;
  uint32_t device = 0, shared_device_cell = 0;
  uint32_t recording_context = 0, recording_context_cell = 0;
  uint32_t dc = 0, dc_cell = 0;
  gpu::GpuBackendStopProof stop_proof;
  std::shared_ptr<NativeGraphicsDeviceBinding> device_publication;
};

// Kernel-resident owner of an actual factory-created SYSTEM recording device.
// The private plan verifies its layout and CPU-resource profile. This does not
// initialize a normal ring or supply a fake native object. GPU execution needs
// a separate checked Source and an explicitly opted-in frame authority.
// Retains native factory ownership without executing PPC from a deleter.
class NativeGraphicsDeviceBinding
    : public std::enable_shared_from_this<NativeGraphicsDeviceBinding> {
 public:
  // Reserve before native heap/CRT startup so allocations and the later real
  // factory binding share one nonreused host epoch.
  static uint64_t ReserveEpoch();
  static std::shared_ptr<NativeGraphicsDeviceBinding> Create(
      KernelState* kernel, const object_ref<UserModule>& module,
      const GuestRuntime::Function& factory, uint32_t device,
      uint64_t backend_epoch, std::string* error, uint64_t reserved_epoch = 0,
      gpu::GpuBackendStopProof stop_proof = {}, bool native_execution = false);
  // Common phase2 prerequisite for production and CPU-only observation. It
  // borrows the actual catalogued light device, takes ONE native AddRef and
  // owns ONE empty-cell CAS publication without initializing a normal ring.
  // Kernel retention precedes either side effect. This staged object is not a
  // usable frame binding until the later actual DC observation joins it.
  static std::shared_ptr<NativeGraphicsDeviceBinding> PublishForNativeSystem(
      const NativeSystemUiServiceContext&, XThread& bootstrap,
      const gpu::GpuBackendStopProof&, std::string* error);
  bool MatchesNativeSystemPublication(
      const NativeSystemUiServiceContext&,
      const NativeSystemSharedGraphicsState&) const;
  struct NativeSystemPublicationMetadata {
    bool addref_attempted = false, addref_owned = false;
    bool published = false, bound = false, retired = false;
    uint32_t cell = 0, before = 0, after = 0, device = 0, addref_result = 0;
    uint64_t serial = 0;
  };
  NativeSystemPublicationMetadata native_system_publication_metadata() const;
  // Join the SAME early owned publication after actual renderer/DC startup.
  // Never reruns the factory, repeats AddRef/CAS or owns a PPC Release.
  // The public cell is compare-cleared only after actual CPU/GPU stop.
  static std::shared_ptr<NativeGraphicsDeviceBinding> CreateForNativeSystem(
      const NativeSystemGraphicsDeviceRecord& record, std::string* error);
  ~NativeGraphicsDeviceBinding();
  bool CheckCpuResourceContext() const;
  bool HasStoppedBackend() const;
  bool HoldGraphOwner(std::shared_ptr<void> owner);
  std::shared_ptr<NativeFrameGraphAuthority> CreateFrameAuthority(
      const memory::PhysicalAllocationOwner& owner);
  std::shared_ptr<NativeFrameGraphAuthority> CreateFrameAuthority(
      const std::shared_ptr<NativeSystemFrameDelegation>& delegation);

 private:
  struct State;
  class FrameAuthority;
  explicit NativeGraphicsDeviceBinding(std::unique_ptr<State> state);
  void RetireAtKernelStop();
  bool CheckCpuResourceContextLocked() const;
  bool CheckDeviceProfileLocked() const;
  bool CheckNativePublicationLocked() const;
  std::unique_ptr<State> state_;
};
}  // namespace xe::kernel
#endif  // XENIA_KERNEL_NATIVE_GRAPHICS_DEVICE_BINDING_H_
