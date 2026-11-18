/** Xenia : Xbox 360 Emulator Research Project; BSD license, see LICENSE. */
#include "xenia/kernel/xam/selected_media_source_proof.h"
#include <utility>
#include "xenia/kernel/xam/content_source.h"
namespace xe::kernel::xam {
SelectedMediaSourceProof::SelectedMediaSourceProof(std::weak_ptr<const uint8_t> issuer,
    std::string id,uint64_t generation,uint32_t device,std::string volume_root,
    std::weak_ptr<filesystem::MediaSelection> selection,std::weak_ptr<vfs::Device> native,
    std::weak_ptr<const filesystem::ConsoleStorageVolume> volume,
    std::shared_ptr<const std::atomic<bool>> accepting)
    :issuer_(std::move(issuer)),source_id_(std::move(id)),generation_(generation),
     device_id_(device),guest_volume_root_(std::move(volume_root)),
     selection_(std::move(selection)),device_(std::move(native)),volume_(std::move(volume)),
     accepting_(std::move(accepting)) {}
SelectedMediaSourceProof::~SelectedMediaSourceProof()=default;
bool SelectedMediaSourceProof::SourceCurrentCached() const noexcept {
  return accepting_ && accepting_->load(std::memory_order_acquire) &&
      !issuer_.expired() && !selection_.expired() && !device_.expired() && !volume_.expired();
}
bool SelectedMediaSourceProof::MatchesObservedSource(const ContentSourceState& source,
    const std::shared_ptr<vfs::Device>& device) const noexcept {
  return SourceCurrentCached() && source.available && !source.observation_error &&
      source.source.kind==ContentSourceKind::kSelectedMedia && source.source.read_only &&
      source.source.id==source_id_ && source.generation==generation_ &&
      source.source.device_id==device_id_ && source.source.guest_volume_root==guest_volume_root_ &&
      source.selected_media && source.storage_volume && device &&
      !selection_.owner_before(source.selected_media) && !source.selected_media.owner_before(selection_) &&
      !volume_.owner_before(source.storage_volume) && !source.storage_volume.owner_before(volume_) &&
      !device_.owner_before(device) && !device.owner_before(device_);
}
} // namespace xe::kernel::xam
