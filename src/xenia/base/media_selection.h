/** Xenia : Xbox 360 Emulator Research Project; BSD license, see LICENSE. */
#ifndef XENIA_BASE_MEDIA_SELECTION_H_
#define XENIA_BASE_MEDIA_SELECTION_H_
#include <atomic>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <system_error>
#include <vector>
#include "xenia/base/host_storage.h"
namespace xe::kernel::xam { class ContentSourceRegistry; }
namespace xe::filesystem {
// Explicit host selection, not package, profile, format or write authority.
// File selections never expose their parent root or a path-resolution API.
// Owned rows may escape; retirement revokes and releases native owners while
// already admitted reads retain only their exact file until completion.
class MediaSelection final {
 public:
  struct FileInput {
    std::string guest_path;
    std::shared_ptr<HostStorageRoot> root;
    std::filesystem::path relative;
  };
  struct Entry {
    std::string path;
    uint64_t bytes = 0;
  };
  static std::shared_ptr<MediaSelection> CaptureFolder(
      const std::shared_ptr<HostStorageRoot>&, std::error_code&);
  static std::shared_ptr<MediaSelection> CaptureFiles(
      std::vector<FileInput>, std::error_code&);
  ~MediaSelection();
  MediaSelection(const MediaSelection&) = delete;
  MediaSelection& operator=(const MediaSelection&) = delete;
  bool folder() const;
  std::vector<Entry> entries() const;
  // Cached only; no filesystem work or last native-owner destruction.
  bool current_cached() const;
  HostStorageSnapshot Observe() const;
  // Close new reads now; keep actual backend pins for already admitted native
  // cursor close/rundown. The registry calls Retire only after those drain.
  void CloseIngress() const noexcept;
  void Retire() const noexcept;
  FileIoResult Read(size_t entry, uint64_t offset, std::span<uint8_t>,
                    const std::atomic<bool>* canceled = nullptr) const;
  std::optional<uint64_t> QueryAllocatedBytes(size_t entry,std::error_code&) const;
  // Folder selection deliberately grants the selected subtree. Files return
  // null, including when all selected files share one native parent.
  std::shared_ptr<HostStorageRoot> CaptureFolderRoot(std::error_code&) const;
  std::vector<GuestHostBacking> GuestBackings(std::error_code&) const;
  // Denial only for raw directory routing; no native parent is returned.
  bool IntersectsDirectory(const std::filesystem::path&) const;
 private:
  friend class MediaReferenceStore;
  friend class xe::kernel::xam::ContentSourceRegistry;
  // An actual registry lifetime issues each binding. A matching numeric ID and
  // generation in another registry cannot share this revocable authority.
  bool Bind(const std::shared_ptr<const uint8_t>& issuer,
            const std::string&, uint64_t);
  static bool ObservationRefused(const std::error_code&) noexcept;
  HostStorageSnapshot ProbeRetained() const;
  struct ReferenceInput { FileInput input; FileHandleInfo opened; };
  std::vector<ReferenceInput> ReferenceInputs(std::error_code&) const;
  struct State;
  explicit MediaSelection(std::shared_ptr<State>);
  std::shared_ptr<State> state_;
};
}  // namespace xe::filesystem
#endif
