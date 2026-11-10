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

namespace xe::vfs { class Device; }
namespace xe::kernel {

// PIRS-only immutable native mount. The body and caller context are retained
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
  X_STATUS Control(uint32_t code) const;
  uint32_t context_address() const { return context_; }
  uint32_t context_size() const { return context_size_; }
  uint64_t pointer_references() const;
  bool retired() const;
  bool backing_drained() const { return backing_lifetime_.expired(); }
  const std::array<uint8_t, 36>& descriptor() const { return descriptor_; }

 private:
  explicit XStfsDevice(KernelState&, uint32_t context_size,
                       std::string name, std::array<uint8_t, 36> descriptor);
  bool AllocateBodyAndContext();

  const uint32_t context_size_;
  const std::string mount_name_;
  const std::array<uint8_t, 36> descriptor_;
  uint32_t context_ = 0;
  mutable std::mutex state_mutex_;
  uint64_t pointer_references_ = 0;
  bool retired_ = true;
  std::shared_ptr<vfs::Device> active_device_;
  // No owning reference in a tombstone. Kernel stop checks this after its
  // XFiles drain, before destroying the Kernel an escaped alias would use.
  std::weak_ptr<void> backing_lifetime_;
};
}  // namespace xe::kernel
#endif
