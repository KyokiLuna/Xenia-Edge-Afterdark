/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xam/xam_content_device.h"

#include <algorithm>
#include <limits>
#include <mutex>
#include "xenia/kernel/native_managed_format_frontdoor.h"
#include "xenia/kernel/xthread.h"

#include "xenia/base/logging.h"
#include "xenia/base/math.h"
#include "xenia/emulator.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xam/content_manager.h"
#include "xenia/kernel/xam/xam_private.h"
#include "xenia/kernel/xenumerator.h"
#include "xenia/vfs/device.h"
#include "xenia/vfs/devices/stfs_xbox.h"
#include "xenia/xbox.h"

namespace xe {
namespace kernel {
namespace xam {

std::optional<StorageDeviceInfo> GetStorageDeviceInfo(
    const ContentSourceRegistry& registry, uint32_t device_id) {
  const auto sources = registry.Sources(device_id);
  if (sources.empty()) {
    return std::nullopt;
  }
  const auto* identity = &sources.front();
  const ContentSourceState* available = nullptr;
  for (const auto& source : sources) {
    if (source.source.device_type != identity->source.device_type) {
      return std::nullopt;
    }
    if (source.source.kind == ContentSourceKind::kManaged) {
      identity = &source;
    }
    if (source.available && (!available || (available->source.read_only &&
                                            !source.source.read_only))) {
      available = &source;
    }
  }
  // An installed overlay is content on the device, not proof that its managed
  // backing is connected. Keep readonly-only source devices observable.
  if (identity->source.kind == ContentSourceKind::kManaged) {
    available = identity->available ? identity : nullptr;
  }
  StorageDeviceInfo result;
  result.device_id = device_id;
  result.device_type = identity->source.device_type;
  result.name = identity->source.name;
  result.guest_root = identity->source.guest_root;
  result.guest_volume_root = identity->source.guest_volume_root;
  if (available) {
    result.available = true;
    result.read_only = available->source.read_only;
    result.managed_format_proof = available->managed_format_proof;
    result.total_bytes = available->capacity_bytes;
    result.free_bytes = result.read_only ? 0 : available->free_bytes;
  }
  return result;
}

std::optional<StorageDeviceInfo> GetStorageDeviceInfo(KernelState* state,
                                                      uint32_t device_id) {
  if (!state) {
    return std::nullopt;
  }
  if (device_id != static_cast<uint32_t>(DummyDeviceId::ODD)) {
    return GetStorageDeviceInfo(state->content_manager()->sources(), device_id);
  }
  StorageDeviceInfo optical;
  optical.device_id = device_id;
  optical.device_type = DeviceType::ODD;
  optical.name = u"Title Media";
  optical.guest_root = state->title_mount_path_ + "\\content";
  optical.guest_volume_root = state->title_mount_path_;
  // Preserve the existing title-media content adapter, without claiming that
  // a physical DVD is present. The current VFS device supplies its geometry.
  const auto* root =
      state->title_mount_path_.empty()
          ? nullptr
          : state->file_system()->ResolvePath(state->title_mount_path_);
  if (root) {
    const auto* device = root->device();
    optical.available = true;
    uint64_t bytes = device->total_allocation_units();
    for (const uint64_t factor :
         {uint64_t(device->sectors_per_allocation_unit()),
          uint64_t(device->bytes_per_sector())}) {
      bytes = factor && bytes > std::numeric_limits<uint64_t>::max() / factor
                  ? std::numeric_limits<uint64_t>::max()
                  : bytes * factor;
    }
    optical.total_bytes = bytes;
  }
  return optical;
}

std::vector<StorageDeviceInfo> ListStorageDevices(KernelState* state,
                                                  bool include_readonly) {
  if (!state) {
    return {};
  }
  auto ids = state->content_manager()->sources().DeviceIds();
  ids.push_back(static_cast<uint32_t>(DummyDeviceId::ODD));
  std::sort(ids.begin(), ids.end());
  ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
  std::vector<StorageDeviceInfo> devices;
  for (const auto id : ids) {
    auto device = GetStorageDeviceInfo(state, id);
    if (!device || !device->available ||
        (!include_readonly && device->read_only)) {
      continue;
    }
    devices.push_back(std::move(*device));
  }
  return devices;
}

bool CanStoreOnDevice(const StorageDeviceInfo& device,
                      uint64_t required_bytes) {
  return device.available && !device.read_only &&
         device.free_bytes >= required_bytes;
}

std::optional<std::string> GetStorageDeviceVolumePath(
    const StorageDeviceInfo& device, bool omit_backslash) {
  if (device.guest_volume_root.empty()) {
    return std::nullopt;
  }
  auto path = device.guest_volume_root;
  if (path.back() != '\\') {
    path.push_back('\\');
  }
  if (omit_backslash) {
    path.pop_back();
  }
  return path;
}

StorageSelection SelectStorageDevice(std::span<const StorageDeviceInfo> devices,
                                     uint32_t flags, uint64_t required_bytes,
                                     uint32_t preferred_device) {
  if (flags & (0x200 | 0x10000)) {
    return {StorageSelectionKind::kPrompt};
  }
  const StorageDeviceInfo* first = nullptr;
  bool connected_writable = false;
  for (const auto& device : devices) {
    connected_writable |= device.available && !device.read_only;
    if (!CanStoreOnDevice(device, required_bytes)) {
      continue;
    }
    if (device.device_id == preferred_device) {
      return {StorageSelectionKind::kAutomatic, device.device_id};
    }
    if (!first || device.device_id < first->device_id) {
      first = &device;
    }
  }
  if (first) {
    return {StorageSelectionKind::kAutomatic, first->device_id};
  }
  if (flags & 0x100) {
    return {StorageSelectionKind::kPrompt};
  }
  return {connected_writable ? StorageSelectionKind::kDiskFull
                             : StorageSelectionKind::kNoDevice};
}

dword_result_t XamContentGetDeviceName_entry(dword_t device_id,
                                             dword_t name_buffer_ptr,
                                             dword_t name_capacity) {
  auto device_info = GetStorageDeviceInfo(kernel_state(), device_id);
  if (!device_info || !device_info->available) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  if (name_capacity > std::numeric_limits<uint32_t>::max() / sizeof(char16_t) ||
      (name_capacity &&
       (!name_buffer_ptr || kernel_memory()->QueryRangeAccess(
                                name_buffer_ptr, uint32_t(name_capacity) * 2) !=
                                memory::PageAccess::kReadWrite))) {
    return X_ERROR_INVALID_PARAMETER;
  }
  if (!name_capacity) {
    return X_ERROR_SUCCESS;
  }
  char16_t* name_buffer =
      kernel_memory()->TranslateVirtual<char16_t*>(name_buffer_ptr);

  // xam truncates a name too long for the buffer and pads a short one.
  const uint32_t capacity = name_capacity;
  std::fill_n(name_buffer, capacity, u'\0');
  xe::string_util::copy_and_swap_truncating(name_buffer, device_info->name,
                                            capacity);
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamContentGetDeviceName, kContent, kImplemented);

dword_result_t XamContentGetDeviceState_entry(dword_t device_id,
                                              lpunknown_t overlapped_ptr) {
  if (overlapped_ptr &&
      kernel_memory()->QueryRangeAccess(overlapped_ptr.guest_address(),
                                        sizeof(XAM_OVERLAPPED)) !=
          memory::PageAccess::kReadWrite) {
    return X_ERROR_INVALID_PARAMETER;
  }
  auto device_info = GetStorageDeviceInfo(kernel_state(), device_id);
  if (!device_info || !device_info->available) {
    if (overlapped_ptr) {
      kernel_state()->CompleteOverlappedImmediateEx(
          overlapped_ptr, X_ERROR_FUNCTION_FAILED,
          X_HRESULT_FROM_WIN32(X_ERROR_DEVICE_NOT_CONNECTED), 0);
      return X_ERROR_IO_PENDING;
    } else {
      return X_ERROR_DEVICE_NOT_CONNECTED;
    }
  }
  if (overlapped_ptr) {
    kernel_state()->CompleteOverlappedImmediate(overlapped_ptr,
                                                X_ERROR_SUCCESS);
    return X_ERROR_IO_PENDING;
  } else {
    return X_ERROR_SUCCESS;
  }
}
DECLARE_XAM_EXPORT1(XamContentGetDeviceState, kContent, kImplemented);

// Report an available mounted device, retaining HDD1 preference by stable ID.
dword_result_t XamContentGetDefaultDevice_entry(lpdword_t device_id_ptr) {
  if (!device_id_ptr ||
      kernel_memory()->QueryRangeAccess(device_id_ptr.guest_address(), 4) !=
          memory::PageAccess::kReadWrite) {
    return X_ERROR_INVALID_PARAMETER;
  }
  const auto devices = ListStorageDevices(kernel_state(), true);
  if (devices.empty()) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  *device_id_ptr = devices.front().device_id;
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamContentGetDefaultDevice, kContent, kImplemented);

typedef struct {
  xe::be<uint32_t> device_id;
  xe::be<uint32_t> device_type;
  xe::be<uint64_t> total_bytes;
  xe::be<uint64_t> free_bytes;
  union {
    xe::be<uint16_t> name[28];
    char16_t name_chars[28];
  };
} X_CONTENT_DEVICE_DATA;
static_assert_size(X_CONTENT_DEVICE_DATA, 0x50);

namespace {
// A device row owns its actual common-registry proof for the whole real enum
// lifetime. Generic enumerators never acquire this capsule by guest type/bytes.
class ManagedDeviceEnumerator final : public XEnumerator {
 public:
  struct Row {
    X_CONTENT_DEVICE_DATA data{};
    std::shared_ptr<const ManagedFormatSourceProof> proof;
  };
  ManagedDeviceEnumerator(KernelState* kernel, size_t count)
      : XEnumerator(kernel, count, sizeof(X_CONTENT_DEVICE_DATA)) {}
  void Append(Row row) { rows_.push_back(std::move(row)); }
  uint32_t WriteItems(uint8_t* buffer, uint32_t size,
                      uint32_t* count) override {
    std::lock_guard lock(mutex_);
    delivered_ = false;
    if (count) *count = 0;
    const auto n = std::min(rows_.size() - cursor_, items_per_enumerate());
    if (!n) return X_ERROR_NO_MORE_FILES;
    if (!buffer || n > size / sizeof(X_CONTENT_DEVICE_DATA))
      return X_ERROR_INSUFFICIENT_BUFFER;
    first_ = cursor_;
    length_ = n;
    output_ = buffer;
    writer_ = XThread::IsInThread() ? XThread::GetCurrentThread() : nullptr;
    for (size_t i = 0; i != n; ++i)
      std::memcpy(buffer + i * sizeof(X_CONTENT_DEVICE_DATA),
                  &rows_[cursor_ + i].data, sizeof(X_CONTENT_DEVICE_DATA));
    cursor_ += n;
    if (count) *count = uint32_t(n);
    delivered_ = true;
    return X_ERROR_SUCCESS;
  }
  void Observe(KernelState* kernel, uint32_t output, uint32_t count) {
    // Copy host capsules under the enum lock, release it before another owner.
    std::vector<Row> rows;
    {
      std::lock_guard lock(mutex_);
      if (!delivered_ || !writer_ || !XThread::IsInThread() ||
          writer_ != XThread::GetCurrentThread() || length_ != count ||
          output_ != kernel->memory()->TranslateVirtual<uint8_t*>(output)) return;
      delivered_ = false;
      rows.assign(rows_.begin() + first_, rows_.begin() + first_ + length_);
    }
    for (size_t i = 0; i != rows.size(); ++i) {
      const auto& row = rows[i];
      if (row.proof && uint64_t(output) + i * 0x50 <= UINT32_MAX)
        NativeManagedFormatFrontdoor::ObserveDeviceEnumeration(
            kernel, handle(), output + uint32_t(i) * 0x50,
            std::span<const uint8_t, 0x50>(
                reinterpret_cast<const uint8_t*>(&row.data), 0x50),
            row.proof, row.data.device_id);
    }
  }
 private:
  std::mutex mutex_;
  std::vector<Row> rows_;
  size_t cursor_ = 0, first_ = 0, length_ = 0;
  uint8_t* output_ = nullptr;
  XThread* writer_ = nullptr;  // Used only during the synchronous caller's entry.
  bool delivered_ = false;
};
}  // namespace
void ObserveManagedDeviceEnumeration(XEnumerator& object, KernelState* kernel,
                                    uint32_t output, uint32_t count) noexcept {
  try {
    if (kernel && object.kernel_state() == kernel)
      if (auto* typed = dynamic_cast<ManagedDeviceEnumerator*>(&object))
        typed->Observe(kernel, output, count);
  } catch (...) {
    // Observation cannot convert a successful enumeration into fake authority.
  }
}

dword_result_t XamContentGetDeviceData_entry(
    dword_t device_id, pointer_t<X_CONTENT_DEVICE_DATA> device_data) {
  auto device_info = GetStorageDeviceInfo(kernel_state(), device_id);
  if (!device_info || !device_info->available) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  if (!device_data ||
      kernel_memory()->QueryRangeAccess(device_data.guest_address(),
                                        sizeof(X_CONTENT_DEVICE_DATA)) !=
          memory::PageAccess::kReadWrite) {
    return X_ERROR_INVALID_PARAMETER;
  }
  device_data.Zero();
  device_data->device_id = static_cast<uint32_t>(device_info->device_id);
  device_data->device_type = static_cast<uint32_t>(device_info->device_type);
  device_data->total_bytes = device_info->total_bytes;
  device_data->free_bytes = device_info->free_bytes;
  xe::string_util::copy_and_swap_truncating(
      device_data->name_chars, device_info->name,
      xe::countof(device_data->name_chars));
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamContentGetDeviceData, kContent, kImplemented);

const std::map<DeviceType, std::u16string_view> XDeviceTypeMap = {
    {DeviceType::HDD, u"Hard Drive"},
    {DeviceType::MU, u"Memory Unit"},
    {DeviceType::System, u"System"},
    {DeviceType::ODD, u"Optical Drive"},
    {DeviceType::TransferCable, u"Transfer Cable"},
    {DeviceType::Sapphire, u"Sapphire"},
    {DeviceType::USBMASS, u"USB Storage"},
};

dword_result_t XamContentGetLocalizedDeviceData_entry(
    dword_t device_id, pointer_t<X_CONTENT_DEVICE_DATA> device_data) {
  auto device_info = GetStorageDeviceInfo(kernel_state(), device_id);
  if (!device_info || !device_info->available) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  if (!device_data ||
      kernel_memory()->QueryRangeAccess(device_data.guest_address(),
                                        sizeof(X_CONTENT_DEVICE_DATA)) !=
          memory::PageAccess::kReadWrite) {
    return X_ERROR_INVALID_PARAMETER;
  }

  X_CONTENT_DEVICE_DATA produced{};
  produced.device_id = static_cast<uint32_t>(device_info->device_id);
  produced.device_type = static_cast<uint32_t>(device_info->device_type);
  produced.total_bytes = device_info->total_bytes;
  produced.free_bytes = device_info->free_bytes;
  // enforces name by device type
  const std::u16string_view device_name =
      XDeviceTypeMap.contains(device_info->device_type)
          ? XDeviceTypeMap.at(device_info->device_type)
          : u"Storage Device";

  xe::string_util::copy_and_swap_truncating(
      produced.name_chars, device_name,
      xe::countof(produced.name_chars));

  *device_data = produced;
  NativeManagedFormatFrontdoor::ObserveLocalizedDeviceData(
      kernel_state(), device_data.guest_address(),
      std::span<const uint8_t, 0x50>(reinterpret_cast<const uint8_t*>(&produced), 0x50),
      device_info->managed_format_proof, device_info->device_id);
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamContentGetLocalizedDeviceData, kContent, kStub);

dword_result_t XamContentCreateDeviceEnumerator_entry(dword_t content_type,
                                                      dword_t content_flags,
                                                      dword_t max_count,
                                                      lpdword_t buffer_size_ptr,
                                                      lpdword_t handle_out) {
  if (!handle_out || max_count > UINT32_MAX / sizeof(X_CONTENT_DEVICE_DATA)) {
    return X_ERROR_INVALID_PARAMETER;
  }

  if (buffer_size_ptr) {
    *buffer_size_ptr = sizeof(X_CONTENT_DEVICE_DATA) * max_count;
  }

  auto e = make_object<ManagedDeviceEnumerator>(kernel_state(),
                                                                 max_count);
  auto result = e->Initialize(XUserIndexNone, 0xFE, 0x2000A, 0x20009, 0);
  if (XFAILED(result)) {
    return result;
  }

  for (const auto& device_info : ListStorageDevices(
           kernel_state(),
           !(content_flags & vfs::XContentFlag::kExcludeReadOnlyDevices))) {
    ManagedDeviceEnumerator::Row row;
    row.data.device_id = device_info.device_id;
    row.data.device_type = static_cast<uint32_t>(device_info.device_type);
    row.data.total_bytes = device_info.total_bytes;
    row.data.free_bytes = device_info.free_bytes;
    xe::string_util::copy_and_swap_truncating(
        row.data.name_chars, device_info.name, xe::countof(row.data.name_chars));
    row.proof = device_info.managed_format_proof;
    e->Append(std::move(row));
  }

  *handle_out = e->handle();
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamContentCreateDeviceEnumerator, kNone, kImplemented);

}  // namespace xam
}  // namespace kernel
}  // namespace xe

DECLARE_XAM_EMPTY_REGISTER_EXPORTS(ContentDevice);
