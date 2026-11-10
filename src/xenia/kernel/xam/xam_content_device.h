/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XAM_XAM_CONTENT_DEVICE_H_
#define XENIA_KERNEL_XAM_XAM_CONTENT_DEVICE_H_

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace xe {
namespace kernel {
class KernelState;
class XEnumerator;
namespace xam {
class ContentSourceRegistry;
class ManagedFormatSourceProof;

enum class DeviceType : uint32_t {
  Invalid = 0,
  HDD = 1,
  MU = 2,  // support removed in 9199
  System = 3,
  ODD = 4,
  TransferCable = 8,
  Sapphire = 15,
  USBMASS = 16,  // Added in 8955
};

enum class DummyDeviceId : uint32_t {
  HDD = 1,
  ODD = 2,
};

// An owned point-in-time view, safe to retain while a selector is visible.
// Availability and capacity must be observed again before completing a write
// selection. Installed read-only sources can contribute to the same device.
struct StorageDeviceInfo {
  uint32_t device_id = 0;
  DeviceType device_type = DeviceType::Invalid;
  uint64_t total_bytes = 0;
  uint64_t free_bytes = 0;
  std::u16string name;
  std::string guest_root;
  std::string guest_volume_root;
  bool available = false;
  bool read_only = true;
  // Same actual Registry Sources observation as this row, never ID-relooked-up.
  std::shared_ptr<const ManagedFormatSourceProof> managed_format_proof;
};

std::optional<StorageDeviceInfo> GetStorageDeviceInfo(
    const ContentSourceRegistry&, uint32_t device_id);
std::optional<StorageDeviceInfo> GetStorageDeviceInfo(KernelState*,
                                                      uint32_t device_id);
std::vector<StorageDeviceInfo> ListStorageDevices(
    KernelState*, bool include_readonly = false);

// Only the actual synchronous250 path, retaining its real enumerator object,
// may turn the typed row capsule into a Dash delivery correlation.
void ObserveManagedDeviceEnumeration(XEnumerator&, KernelState*, uint32_t output,
                                    uint32_t count) noexcept;

enum class StorageSelectionKind { kAutomatic, kPrompt, kNoDevice, kDiskFull };
struct StorageSelection {
  StorageSelectionKind kind;
  uint32_t device_id = 0;
};
bool CanStoreOnDevice(const StorageDeviceInfo&, uint64_t required_bytes);
// The explicit volume mount is independent of the Content directory. Native
// public611's fourth boolean omits its trailing slash when nonzero.
std::optional<std::string> GetStorageDeviceVolumePath(const StorageDeviceInfo&,
                                                      bool omit_backslash);
// preferred_device comes from validated shared preferences, never the caller's
// output pointer. ForceUI and native flag0x10000 always require the selector.
StorageSelection SelectStorageDevice(std::span<const StorageDeviceInfo>,
                                     uint32_t flags, uint64_t required_bytes,
                                     uint32_t preferred_device = 1);

}  // namespace xam
}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_XAM_XAM_CONTENT_DEVICE_H_
