/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia contributors. All rights reserved.                    *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */
#ifndef XENIA_GPU_VULKAN_VULKAN_NATIVE_RESOURCE_EPOCH_H_
#define XENIA_GPU_VULKAN_VULKAN_NATIVE_RESOURCE_EPOCH_H_

#include <cstdint>

namespace xe::gpu::vulkan {
class VulkanCommandProcessor;

// CP-private upload/descriptor retirement namespace. This value is never an
// execution or source lease, and is not a Vulkan submission or title frame.
class VulkanNativeResourceEpoch final {
 public:
  constexpr VulkanNativeResourceEpoch() = default;
  constexpr bool valid() const { return backend_epoch_ && serial_; }
  constexpr uint64_t backend_epoch() const { return backend_epoch_; }
  constexpr uint64_t serial() const { return serial_; }
  constexpr bool operator==(const VulkanNativeResourceEpoch& other) const {
    return backend_epoch_ == other.backend_epoch_ && serial_ == other.serial_;
  }
  constexpr bool operator!=(const VulkanNativeResourceEpoch& other) const {
    return !(*this == other);
  }

 private:
  friend class VulkanCommandProcessor;
  constexpr VulkanNativeResourceEpoch(uint64_t backend_epoch, uint64_t serial)
      : backend_epoch_(backend_epoch), serial_(serial) {}
  uint64_t backend_epoch_ = 0;
  uint64_t serial_ = 0;
};
}  // namespace xe::gpu::vulkan

#endif  // XENIA_GPU_VULKAN_VULKAN_NATIVE_RESOURCE_EPOCH_H_
