/**
 * Xenia : Xbox 360 Emulator Research Project
 * Released under the BSD license - see LICENSE in the root for details.
 */
#include "xenia/kernel/xstfs_device.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "xenia/base/mutex.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/util/guest_io_buffer.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xfile.h"
#include "xenia/vfs/devices/xcontent_container_device.h"
#include "xenia/vfs/devices/xcontent_devices/stfs_container_device.h"

namespace xe::kernel {
namespace {
struct CreatePacket {
  X_ANSI_STRING name;
  xe::be<uint64_t> data_offset;
  xe::be<uint64_t> logical_geometry;
  xe::be<uint32_t> backing_handle;
  std::array<uint8_t, 36> descriptor;
  xe::be<uint32_t> work;
  xe::be<uint32_t> context_bytes;
  xe::be<uint32_t> device_output;
  xe::be<uint32_t> context_output;
  uint8_t pages, owner_option, content_nonzero, signature_class;
  xe::be<uint32_t> reserved;
};
static_assert(sizeof(CreatePacket) == 0x58);
static_assert(offsetof(CreatePacket, device_output) == 0x48);
static_assert(offsetof(CreatePacket, context_output) == 0x4C);

// Preserve physical reader errors through the existing bool parser API. A
// malformed parse with no provider failure remains an ordinary failed mount.
class StatusBacking final : public vfs::XContentReadBacking {
 public:
  StatusBacking(std::unique_ptr<vfs::XContentReadBacking> inner,
                std::shared_ptr<std::atomic<X_STATUS>> error)
      : XContentReadBacking(inner->extent()), inner_(std::move(inner)),
        error_(std::move(error)) {}
  auto GuestBackings() const ->
      std::optional<std::vector<filesystem::GuestHostBacking>> override {
    return inner_->GuestBackings();
  }
  bool supports_concurrent_io() const override {
    return inner_->supports_concurrent_io();
  }
  const vfs::Entry* package_entry() const override {
    return inner_->package_entry();
  }
 protected:
  X_STATUS ReadPartial(size_t offset, std::span<uint8_t> out,
                       size_t* count) const override {
    const auto status = inner_->ReadWithStatus(offset, out, count);
    if (status != X_STATUS_SUCCESS) { error_->store(status); }
    return status;
  }
 private:
  std::unique_ptr<vfs::XContentReadBacking> inner_;
  std::shared_ptr<std::atomic<X_STATUS>> error_;
};

bool ValidName(const std::string& name) {
  constexpr std::string_view prefix = "\\Device\\";
  if (!name.starts_with(prefix) || name.size() == prefix.size() ||
      name.size() > 255) { return false; }
  // One bounded device component. No aliases, nested mounts or host paths.
  return std::all_of(name.begin() + prefix.size(), name.end(), [](char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') || c == '_' || c == '-';
  });
}
}  // namespace

XStfsDevice::XStfsDevice(KernelState& kernel, uint32_t context_size,
                         std::string name, std::array<uint8_t, 36> descriptor)
    : XObject(&kernel, Type::Device, true), context_size_(context_size),
      mount_name_(std::move(name)), descriptor_(descriptor) {
  // No handle is published or stashed into this opaque body. The initial host
  // object_ref remains alive while its temporary host-only table entry leaves.
  ReleaseHandle();
}

XStfsDevice::~XStfsDevice() {
  Dismount();
  if (context_) { memory()->SystemHeapFree(context_); }
}

bool XStfsDevice::AllocateBodyAndContext() {
  const uint32_t type = kernel_state()->GetKernelGuestGlobals() +
      offsetof(KernelGuestGlobals, IoDeviceObjectType);
  // Only opaque identity is needed by the recovered Ob/Io callers. No guessed
  // DEVICE_OBJECT driver fields or dispatcher signature are exposed.
  if (!CreateNativeNonDispatch(sizeof(uint32_t), type)) { return false; }
  context_ = memory()->SystemHeapAlloc(context_size_);
  if (!context_) { return false; }
  memory()->Zero(context_, context_size_);
  return true;
}

object_ref<XStfsDevice> XStfsDevice::Lookup(KernelState& kernel,
                                            uint32_t body) {
  auto object = kernel.object_table()->LookupAllocatedBody(body);
  auto* stfs = object ? dynamic_cast<XStfsDevice*>(object.get()) : nullptr;
  if (!stfs) { return {}; }
  stfs->Retain();
  return object_ref<XStfsDevice>(stfs);
}

void XStfsDevice::ReferencePointer() {
  std::lock_guard lock(state_mutex_);
  if (pointer_references_ != UINT64_MAX) { ++pointer_references_; }
}
void XStfsDevice::DereferencePointer() {
  std::lock_guard lock(state_mutex_);
  // The pre-publication rollback has no matching visible ObReference. Do not
  // invent an initial count or let an unmatched release free an opaque body.
  if (pointer_references_) { --pointer_references_; }
}
uint64_t XStfsDevice::pointer_references() const {
  std::lock_guard lock(state_mutex_);
  return pointer_references_;
}
bool XStfsDevice::retired() const {
  std::lock_guard lock(state_mutex_);
  return retired_;
}
X_STATUS XStfsDevice::Control(uint32_t /*code*/) const {
  std::lock_guard lock(state_mutex_);
  if (retired_ || !active_device_ ||
      static_cast<const vfs::StfsContainerDevice*>(
          active_device_.get())->retired()) { return kDisconnected; }
  // Even descriptor3 is left unsupported in this first consumer slice. The
  // evidenced mode2/3 PIRS mount/read/close route needs none of these controls.
  // In particular, no fake flush, checkpoint or error-poll success is emitted.
  return X_STATUS_INVALID_DEVICE_REQUEST;
}
X_STATUS XStfsDevice::Dismount() {
  std::shared_ptr<vfs::Device> active;
  {
    std::lock_guard lock(state_mutex_);
    if (retired_) { return kDisconnected; }
    retired_ = true;
    active = std::move(active_device_);
  }
  // No state mutex or GCRC is held while an actual reader/FD may be destroyed.
  // A namespace replacement is never removed on the old body's behalf.
  if (active) {
    kernel_state()->file_system()->UnregisterDevice(mount_name_, active.get());
    active->Retire();
  }
  active.reset();
  return X_STATUS_SUCCESS;
}

X_STATUS XStfsDevice::Create(KernelState& kernel, uint32_t address,
                              uint32_t size) {
  if (size != sizeof(CreatePacket)) { return X_STATUS_INVALID_PARAMETER; }
  if (global_critical_region::is_held_by_current_thread()) {
    return X_STATUS_INVALID_DEVICE_REQUEST;
  }
  auto& memory = *kernel.memory();
  CreatePacket packet{};
  auto status = util::ReadGuestIoBuffer(memory, address,
      {reinterpret_cast<uint8_t*>(&packet), sizeof(packet)});
  if (status != X_STATUS_SUCCESS) { return status; }
  const uint32_t output = address + offsetof(CreatePacket, device_output);
  if (!util::GuestIoRangeValid(memory, output, 8, true)) {
    return X_STATUS_ACCESS_VIOLATION;
  }
  if (packet.signature_class != 4 || packet.reserved ||
      packet.owner_option > 1 || packet.content_nonzero > 1 ||
      packet.pages < 5 || packet.pages > 64 ||
      !packet.context_bytes || packet.context_bytes > kMaximumContext ||
      packet.name.length > packet.name.maximum_length ||
      !packet.name.length || packet.name.length > 255) {
    return X_STATUS_INVALID_PARAMETER;
  }
  const uint32_t work_bytes = uint32_t(packet.pages) << 12;
  if (!util::GuestIoRangeValid(memory, packet.work, work_bytes, true)) {
    return X_STATUS_ACCESS_VIOLATION;
  }
  if (util::GuestIoRangesOverlap(memory, address, sizeof(packet),
                                  packet.work, work_bytes)) {
    return X_STATUS_INVALID_PARAMETER;
  }
  std::string name(packet.name.length, '\0');
  status = util::ReadGuestIoBuffer(memory, packet.name.pointer,
      {reinterpret_cast<uint8_t*>(name.data()), name.size()});
  if (status != X_STATUS_SUCCESS) { return status; }
  if (!ValidName(name)) { return X_STATUS_INVALID_PARAMETER; }

  auto file = kernel.object_table()->LookupObject<XFile>(packet.backing_handle);
  if (!file) { return X_STATUS_INVALID_HANDLE; }
  // An alias owner retains the exact XFile while exposing only its existing
  // public File getter. It never invokes File::Destroy independently.
  auto owner = std::make_shared<object_ref<XFile>>(std::move(file));
  std::shared_ptr<vfs::File> shared(owner, (*owner)->file());
  if (!shared) { return X_STATUS_INVALID_HANDLE; }
  std::unique_ptr<vfs::XContentContainerDevice> device;
  const auto read_error = std::make_shared<std::atomic<X_STATUS>>(X_STATUS_SUCCESS);
  const bool concurrent = shared->retained_device() &&
                          shared->retained_device()->supports_concurrent_io();
  kernel.RunBlockingIo([&] {
    auto backing = vfs::XContentReadBacking::FromSharedFile(shared, &status);
    if (!backing) { return; }
    auto checked = std::make_unique<StatusBacking>(std::move(backing), read_error);
    auto header = vfs::XContentContainerDevice::ReadContainerHeader(*checked);
    if (!header) {
      status = read_error->load() != X_STATUS_SUCCESS ? read_error->load()
                                               : X_STATUS_UNSUCCESSFUL;
      return;
    }
    const uint64_t header_size = header->content_header.header_size;
    const uint64_t rounded = (header_size + 0xFFF) & ~uint64_t(0xFFF);
    const uint64_t content = header->content_metadata.content_size;
    if (content > UINT64_MAX - 0xFFFFF) {
      status = X_STATUS_INVALID_PARAMETER;
      return;
    }
    const uint64_t logical = ((content ? content : 0xFFFFF000ull) +
                             0xFFFFFull) & ~uint64_t(0xFFFFF);
    if (header->content_header.magic != vfs::XContentPackageType::kPirs ||
        header->content_metadata.volume_type != vfs::XContentVolumeType::kStfs ||
        header_size < sizeof(vfs::XContentContainerHeader) ||
        rounded > checked->extent() || packet.data_offset != rounded ||
        packet.logical_geometry != logical ||
        packet.context_bytes != rounded + 0x192 ||
        packet.content_nonzero != (content != 0) ||
        std::memcmp(packet.descriptor.data(),
            &header->content_metadata.volume_descriptor.stfs, 36)) {
      status = X_STATUS_INVALID_PARAMETER;
      return;
    }
    device = vfs::XContentContainerDevice::CreateContentDevice(name,
                                                              std::move(checked));
    if (device) {
      static_cast<vfs::StfsContainerDevice*>(device.get())->SetReadOnlyView();
    }
    if (!device || !device->Initialize()) {
      device.reset();
      status = read_error->load() != X_STATUS_SUCCESS ? read_error->load()
                                               : X_STATUS_UNSUCCESSFUL;
      return;
    }
    status = X_STATUS_SUCCESS;
  }, concurrent ? GuestScheduler::BlockingCallClass::kConcurrent
                : GuestScheduler::BlockingCallClass::kSerial);
  if (status != X_STATUS_SUCCESS) { return status; }

  auto stfs = object_ref<XStfsDevice>(new XStfsDevice(
      kernel, packet.context_bytes, name, packet.descriptor));
  stfs->backing_lifetime_ = owner;
  if (!stfs->AllocateBodyAndContext()) { return X_STATUS_NO_MEMORY; }
  if (!kernel.KeepNativeStfsDevice(stfs)) {
    return X_STATUS_INSUFFICIENT_RESOURCES;
  }
  std::shared_ptr<vfs::Device> registered;
  status = kernel.file_system()->RegisterDeviceExclusive(std::move(device),
                                                        registered);
  if (status != X_STATUS_SUCCESS) { return status; }
  {
    std::lock_guard lock(stfs->state_mutex_);
    stfs->active_device_ = registered;
    stfs->retired_ = false;
  }
  if (static_cast<const vfs::StfsContainerDevice*>(
          registered.get())->retired()) {
    stfs->Dismount();
    return kDisconnected;
  }
  const std::array<xe::be<uint32_t>, 2> outputs{
      stfs->guest_object(), stfs->context_};
  status = util::WriteGuestIoBuffer(memory, output,
      {reinterpret_cast<const uint8_t*>(outputs.data()), sizeof(outputs)});
  if (status != X_STATUS_SUCCESS) { stfs->Dismount(); }
  return status;
}
}  // namespace xe::kernel
