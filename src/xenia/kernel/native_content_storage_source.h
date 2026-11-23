/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_NATIVE_CONTENT_STORAGE_SOURCE_H_
#define XENIA_KERNEL_NATIVE_CONTENT_STORAGE_SOURCE_H_

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include <utility>

#include "xenia/kernel/xam/xam_content_device.h"

namespace xe::filesystem { class ConsoleStorageVolume; }
namespace xe::vfs { class Device; }
namespace xe::kernel {
namespace xam { class ManagedFormatSourceProof; class SelectedMediaSourceProof; }

// Metadata from the genuine native type4 optical processing producer. This
// is NOT an extra source/physical device and is never a notification input.
// The actual registered optical VFS Device owns backing/lifetime; this weak
// capsule only binds the observed native row to that same mounted generation.
class NativeOpticalStorageProof final {
 public:
  const std::shared_ptr<const filesystem::ConsoleStorageVolume>& volume() const {return volume_;}
  const std::string& guest_root() const {return root_;}
  uint64_t title_generation() const {return title_generation_;}
 private:
  friend class NativeContentStorageDriver;
  friend class NativeContentResolve;
  NativeOpticalStorageProof(std::shared_ptr<const filesystem::ConsoleStorageVolume> volume,
      std::weak_ptr<vfs::Device> device,const vfs::Device* identity,std::string root,uint64_t generation)
      :volume_(std::move(volume)),device_(std::move(device)),identity_(identity),
       root_(std::move(root)),title_generation_(generation) {}
  std::shared_ptr<const filesystem::ConsoleStorageVolume> volume_;
  std::weak_ptr<vfs::Device> device_;
  const vfs::Device* identity_=nullptr; // comparison only, never dereferenced
  std::string root_;
  uint64_t title_generation_=0;
};

// An owned physical-volume projection from the common content-source catalog.
// Installed-system content on an HDD contributes to the same device. Host
// paths, native private volume entries and native-generated device IDs are not
// part of this driver input.
struct NativeContentStorageDevice {
  std::string identity;
  // Actual backing generation. Zero is valid only before a never-available
  // source has acquired its first physical capability.
  uint64_t generation = 0;
  xam::DeviceType type = xam::DeviceType::Invalid;
  std::string guest_volume_root;
  bool available = false;
  // Opaque proof from the actual common registry. It holds no old MVA/source
  // lease; absence/retirement never becomes format permission from path data.
  std::shared_ptr<const xam::ManagedFormatSourceProof> managed_format_proof;
  // Weak selected-source publication metadata. Native device/USB attachment
  // receipts and cursor use admission remain separate, mandatory proofs.
  std::shared_ptr<const xam::SelectedMediaSourceProof> selected_media_proof;
  // Exact registry generation metadata, never a host path/root/format permit.
  // The source registry retires this very object before replacing a backing.
  // Native processing may bind its actual generated ID to this identity even
  // when a full/read-only device is ineligible for formatting.
  std::shared_ptr<const filesystem::ConsoleStorageVolume> source_volume;
  // Only the authentic native optical producer may create this capsule.
  std::shared_ptr<const NativeOpticalStorageProof> optical_source_proof;
  bool operator==(const NativeContentStorageDevice&) const = default;
};

struct NativeContentStorageSnapshot {
  // Nonzero and strictly increasing within this source's lifetime. A callback
  // that raced Connect's return may precede the initial snapshot at the sink;
  // receivers must reject stale revisions rather than replaying old devices.
  uint64_t revision = 0;
  std::vector<NativeContentStorageDevice> devices;
};

class NativeContentStorageSource {
 public:
  using Sink = std::function<void(NativeContentStorageSnapshot)>;
  class Subscription {
   public:
    virtual ~Subscription() = default;
    // Close new producer admission without waiting or calling the sink.
    virtual void BeginStop() noexcept = 0;
    // Join already admitted sink calls outside catalog/driver locks. False
    // means self-join or an otherwise unproved producer rundown; retain owner.
    virtual bool Drain() noexcept = 0;
  };
  struct Connection {
    NativeContentStorageSnapshot initial;
    std::shared_ptr<Subscription> subscription;
  };

  virtual ~NativeContentStorageSource() = default;
  // Catalog snapshot and subscription registration form one transaction. No
  // source mutex crosses a sink call, and no asynchronous host filesystem work
  // is performed on the SYSTEM callback dispatcher. A failed connection must
  // leave no admitted sink work behind.
  virtual std::optional<Connection> Connect(Sink sink) = 0;
};

}  // namespace xe::kernel

#endif  // XENIA_KERNEL_NATIVE_CONTENT_STORAGE_SOURCE_H_
