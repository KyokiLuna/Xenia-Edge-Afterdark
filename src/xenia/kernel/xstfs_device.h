/**
 * Xenia : Xbox 360 Emulator Research Project
 * Released under the BSD license - see LICENSE in the root for details.
 */
#ifndef XENIA_KERNEL_XSTFS_DEVICE_H_
#define XENIA_KERNEL_XSTFS_DEVICE_H_

#include <array>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

#include "xenia/kernel/xobject.h"

namespace xe::vfs { class Device; class FolderContentPackage; }
namespace xe::kernel {

// Native PIRS reader or CM-issued folder-CON member mount. The body and caller context are retained
// conservatively through Kernel stop: the recovered callers do not establish
// retail initial/final reference counts. A retired tombstone owns NO backing,
// VFS Device, XFile or source lease. Existing member Files own their own reader.
class XStfsDevice final : public XObject {
 public:
  static constexpr Type kObjectType = Type::Device;
  static constexpr uint32_t kMaximumContext = 1024 * 1024;
  static constexpr X_STATUS kDisconnected = X_STATUS(0xC000009D);

  static X_STATUS Create(KernelState&, uint32_t params, uint32_t size);
  static object_ref<XStfsDevice> Lookup(KernelState&, uint32_t body);
  ~XStfsDevice() override;

  void ReferencePointer();
  void DereferencePointer();
  X_STATUS Dismount();
  X_STATUS Control(uint32_t code, uint32_t output = 0) const;
  uint32_t context_address() const { return context_; }
  uint32_t context_size() const { return context_size_; }
  uint64_t pointer_references() const;
  bool retired() const;
  bool backing_drained() const { return backing_lifetime_.expired(); }
  const std::array<uint8_t, 36>& descriptor() const { return descriptor_; }

 private:
  friend class NativeStfsDeviceTestAccess;
  // Deny-only test boundary after genuine native parsing/admission. It can
  // withdraw/replace the original parent, never fabricate child success.
  static X_STATUS CreateInternal(KernelState&, uint32_t, uint32_t,
      bool retire_parent_before_commit,
      std::unique_ptr<vfs::Device>* replacement_before_commit = nullptr);
  explicit XStfsDevice(KernelState&, uint32_t context_size,
                       std::string name, std::array<uint8_t, 36> descriptor);
  bool AllocateBodyAndContext();

  const uint32_t context_size_;
  const std::string mount_name_;
  const std::array<uint8_t, 36> descriptor_;
  uint32_t context_ = 0;
  mutable std::mutex state_mutex_;
  uint64_t pointer_references_ = 0;
  bool retired_ = true, dismounting_ = false;
  // VFS owns active mounts; an ABI body is only a weak tombstone observer.
  std::weak_ptr<vfs::Device> active_device_;
  // Weak only: the active Device owns the exact original backing XFile.
  std::weak_ptr<vfs::FolderContentPackage> folder_backend_;
  // No owning reference in a tombstone. Kernel stop checks this after its
  // XFiles drain, before destroying the Kernel an escaped alias would use.
  std::weak_ptr<void> backing_lifetime_;
};
}  // namespace xe::kernel
#endif
