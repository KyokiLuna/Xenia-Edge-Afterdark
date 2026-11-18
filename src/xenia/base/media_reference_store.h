/** Xenia : Xbox 360 Emulator Research Project; BSD license, see LICENSE. */
#ifndef XENIA_BASE_MEDIA_REFERENCE_STORE_H_
#define XENIA_BASE_MEDIA_REFERENCE_STORE_H_
#include "xenia/base/media_selection.h"
#include "xenia/base/snapshot_publisher.h"
namespace xe::filesystem {
// Host metadata only. These values are never a root/FD or guest IO capability.
// Loading reopens readonly, proves the original directory/file identities and
// repeats actual guest/private admission. Missing inputs remain in reference.
struct MediaReference {
  struct Member {
    std::string guest_path;
    std::filesystem::path root, relative;
    uint64_t filesystem=0,directory=0,file=0;
    bool operator==(const Member&) const = default;
  };
  std::string id,name;
  std::string guest_volume_root;
  uint32_t device_id=0;
  bool folder=false;
  std::vector<Member> members;
  std::vector<uint8_t> unknown;
  bool operator==(const MediaReference&) const = default;
};
struct LoadedMediaReference {
  std::error_code error;
  std::optional<MediaReference> reference;
  std::shared_ptr<MediaSelection> selection;
};
class MediaReferenceStore final {
 public:
  // Real protected host metadata owner. Failed bootstrap keeps its exact
  // capsule/protection; never adopts an unknown partial directory or marker.
  class Metadata final {
   public:
    const auto& root() const { return root_; }
    const auto& error() const { return error_; }

   private:
    friend class MediaReferenceStore;
    std::unique_ptr<HostStorageBootstrapDirectory> bootstrap_;
    std::shared_ptr<HostStorageRoot> root_;
    std::error_code error_;
  };
  static constexpr size_t kMaximumReferences = 48;
  static std::shared_ptr<Metadata> OpenMetadata(
      std::shared_ptr<HostStorageRoot>);
  static std::string NewId();
  // Complete bounded native inventory. Metadata only: reopening/readonly
  // identity checks still belong to Load; malformed input is not an empty list.
  static std::vector<MediaReference> List(const HostStorageRoot&,
                                          std::error_code&);
  static constexpr uint64_t kMaximumWireBytes=32ull<<20;
  static std::filesystem::path Leaf(std::string_view id);
  static LoadedMediaReference Load(const HostStorageRoot& metadata,
                                  std::string_view id);
  static SnapshotPublicationReceipt Save(
      const HostStorageRoot& metadata, const SnapshotRecoveryOwner&,
      std::string id, uint32_t device_id, std::string name,
      std::string guest_volume_root, const MediaSelection&,
      const std::atomic<bool>* canceled = nullptr,
      MediaReference* attempted = nullptr);
  static SnapshotPublicationReceipt Remove(
      const HostStorageRoot& metadata,const SnapshotRecoveryOwner&,
      std::string_view id,const std::atomic<bool>* canceled=nullptr);
 private:
  static std::optional<MediaReference> Describe(
      std::string,uint32_t,std::string,std::string,const MediaSelection&,std::error_code&);
};
}  // namespace xe::filesystem
#endif
