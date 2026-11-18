/** Xenia : Xbox 360 Emulator Research Project; BSD license, see LICENSE. */
#ifndef XENIA_KERNEL_XAM_SELECTED_MEDIA_SOURCE_PROOF_H_
#define XENIA_KERNEL_XAM_SELECTED_MEDIA_SOURCE_PROOF_H_
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
namespace xe::filesystem { class MediaSelection; class ConsoleStorageVolume; }
namespace xe::vfs { class Device; }
namespace xe::kernel { class NativeContentStorageDriver; class NativeMediaService; }
namespace xe::kernel::xam {
class ContentSourceRegistry;
struct ContentSourceState;
// Weak metadata for one actual selected source publication. It owns no native
// directory/file/device, and grants no read/path/profile/format authority.
// Native consumers additionally require the actual storage processing receipt,
// original attachment/token/request witnesses and a counted SelectedMediaUse.
class SelectedMediaSourceProof final {
 public:
  ~SelectedMediaSourceProof();
  const std::string& source_id() const { return source_id_; }
  uint64_t generation() const { return generation_; }
 private:
  friend class ContentSourceRegistry;
  friend class xe::kernel::NativeContentStorageDriver;
  friend class xe::kernel::NativeMediaService;
  SelectedMediaSourceProof(std::weak_ptr<const uint8_t> issuer,
      std::string source_id,uint64_t generation,uint32_t device_id,
      std::string guest_volume_root,std::weak_ptr<filesystem::MediaSelection>,
      std::weak_ptr<vfs::Device>,
      std::weak_ptr<const filesystem::ConsoleStorageVolume>,
      std::shared_ptr<const std::atomic<bool>> accepting);
  // Denial-only cached metadata, safe under native locks: no weak native-owner
  // acquisition, root lookup, callback, observing IO or last FD release.
  bool SourceCurrentCached() const noexcept;
  bool MatchesObservedSource(const ContentSourceState&,
      const std::shared_ptr<vfs::Device>&) const noexcept;
  const std::weak_ptr<const uint8_t> issuer_;
  const std::string source_id_;
  const uint64_t generation_;
  const uint32_t device_id_;
  const std::string guest_volume_root_;
  const std::weak_ptr<filesystem::MediaSelection> selection_;
  const std::weak_ptr<vfs::Device> device_;
  const std::weak_ptr<const filesystem::ConsoleStorageVolume> volume_;
  const std::shared_ptr<const std::atomic<bool>> accepting_;
};
} // namespace xe::kernel::xam
#endif
