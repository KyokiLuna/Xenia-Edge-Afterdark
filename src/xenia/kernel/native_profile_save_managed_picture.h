/** Xenia : Xbox 360 Emulator Research Project; BSD license, see LICENSE. */
#pragma once
#include <atomic>
#include <memory>
#include <optional>
#include <system_error>
#include <utility>
#include <string>
#include "xenia/vfs/managed_file_service.h"
#include "xenia/vfs/file_open_context.h"
#include "xenia/base/profile_publication_intent.h"
namespace xe::kernel {
class NativeProfileSaveOwner;
class NativeProfileSavePublication;
class NativeProfileSavePictureStage;
class NativeProfileSaveSource;
namespace xam {class ProfileGpdModelAuthority;struct ProfileGpdReadCapture;}
namespace xam::test {class NativeProfileSavePublicationTestAccess;}
// Stable source-bound exact fixed-PNG policy. An actual original native
// producer issues each explicit open capability; path/access/Task or a former
// read cannot create it. No missing/unknown-authority raw-open fallback.
class NativeProfileSaveManagedPicture final : public vfs::ManagedFileService,
    public std::enable_shared_from_this<NativeProfileSaveManagedPicture> {
 public:
  const std::filesystem::path& backing_path() const override {return path_;}
  std::shared_ptr<const filesystem::HostStorageDirectoryIdentity> backing_directory() const override;
  bool retiring() const override;
  X_STATUS Open(std::shared_ptr<vfs::Device>,std::string,vfs::FileDisposition,
      uint32_t,bool,vfs::File**,vfs::FileAction*) override;
  X_STATUS OpenWithContext(std::shared_ptr<vfs::Device>,std::string,vfs::FileDisposition,
      uint32_t,bool,vfs::File**,vfs::FileAction*,const vfs::FileOpenContext&) override;
 private:
  friend class NativeProfileSaveOwner;
  friend class xam::test::NativeProfileSavePublicationTestAccess;
  class ReadAuthority;
  X_STATUS OpenCommitted(std::shared_ptr<vfs::Device>,std::string,vfs::FileDisposition,
      uint32_t,bool,vfs::File**,vfs::FileAction*,const vfs::FileOpenContext&);
  std::error_code JoinCommittedReadModel(const std::shared_ptr<xam::ProfileGpdModelAuthority>&);
  class Authority final : public vfs::ManagedFileOpenAuthority {
   private:
    friend class NativeProfileSaveManagedPicture;
    Authority(std::weak_ptr<NativeProfileSaveManagedPicture>,std::shared_ptr<NativeProfileSavePublication>,
        std::shared_ptr<NativeProfileSavePictureStage>,std::shared_ptr<vfs::Device>,std::string,std::shared_ptr<void> = {},bool read_only=false);
    const std::weak_ptr<NativeProfileSaveManagedPicture> service_;
    const std::shared_ptr<NativeProfileSavePublication> publication_;
    const std::shared_ptr<NativeProfileSavePictureStage> stage_;
    const std::shared_ptr<vfs::Device> requested_;const std::string path_;
    const std::shared_ptr<void> actual_open_use_;
    const bool read_only_;
    mutable std::atomic<bool> consumed_{false};
  };
  static std::shared_ptr<NativeProfileSaveManagedPicture> CreateSource(
      std::shared_ptr<xam::ProfileGpdModelAuthority>,filesystem::ProfileArtifactKind,std::error_code&,
      std::shared_ptr<const std::atomic<bool>> family_closed={});
  std::shared_ptr<const vfs::ManagedFileOpenAuthority> IssueForActualOpen(
      const std::shared_ptr<NativeProfileSavePublication>&,
      std::shared_ptr<vfs::Device>,std::string,std::error_code&);
  std::shared_ptr<const vfs::ManagedFileOpenAuthority> IssueParentForActualOpen(
      const std::shared_ptr<NativeProfileSavePublication>&,std::shared_ptr<vfs::Device>,
      std::string,std::shared_ptr<void>,std::error_code&);
  std::shared_ptr<const vfs::ManagedFileOpenAuthority> IssueParentStageReadForActualOpen(
      const std::shared_ptr<NativeProfileSavePublication>&,std::shared_ptr<vfs::Device>,
      std::string,std::shared_ptr<void>,std::error_code&);
  std::shared_ptr<const vfs::ManagedFileOpenAuthority> IssueReadForActualOpen(
      xam::ProfileGpdReadCapture,std::shared_ptr<vfs::Device>,std::string,std::error_code&);
  X_STATUS OpenRead(const std::shared_ptr<const ReadAuthority>&,
      const std::shared_ptr<vfs::Device>&,const std::string&,vfs::FileDisposition,
      uint32_t,vfs::File**,vfs::FileAction*,const vfs::FileOpenContext&);
  NativeProfileSaveManagedPicture(std::shared_ptr<NativeProfileSaveSource>,
      filesystem::ProfileArtifactKind,std::filesystem::path);
  const std::shared_ptr<NativeProfileSaveSource> source_;
  const filesystem::ProfileArtifactKind kind_;
  const std::filesystem::path path_;
};
}  // namespace xe::kernel
