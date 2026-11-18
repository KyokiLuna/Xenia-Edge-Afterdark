/** Xenia : Xbox 360 Emulator Research Project; BSD license, see LICENSE. */
#ifndef XENIA_VFS_DEVICES_MEDIA_REFERENCE_DEVICE_H_
#define XENIA_VFS_DEVICES_MEDIA_REFERENCE_DEVICE_H_
#include "xenia/base/media_selection.h"
#include "xenia/vfs/device.h"
namespace xe::vfs {
// An exact-file allowlist. A folder selection uses an ordinary readonly
// HostPathDevice instead. No native parent path becomes a guest namespace.
class MediaReferenceDevice final : public Device {
 public:
  MediaReferenceDevice(std::string_view,
      std::shared_ptr<filesystem::MediaSelection>);
  bool Initialize() override;
  std::optional<std::vector<filesystem::GuestHostBacking>> GuestBackings() const override;
  void Dump(StringBuffer*) override;
  Entry* ResolvePath(std::string_view) override;
  void Retire() override;
  bool has_open_files() const override;
  bool supports_concurrent_io() const override { return true; }
  const std::string& name() const override { return name_; }
  uint32_t attributes() const override { return 0; }
  uint32_t component_name_max_length() const override { return 255; }
  uint32_t total_allocation_units() const override { return 0; }
  uint32_t available_allocation_units() const override { return 0; }
  uint32_t sectors_per_allocation_unit() const override { return 1; }
  uint32_t bytes_per_sector() const override { return 512; }
  filesystem::HostStorageSnapshot storage_space() const override;
  const std::shared_ptr<filesystem::MediaSelection>& selection() const { return selection_; }
 private:
  std::string name_="FATX";
  std::shared_ptr<filesystem::MediaSelection> selection_;
  std::unique_ptr<Entry> root_;
};
}  // namespace xe::vfs
#endif
