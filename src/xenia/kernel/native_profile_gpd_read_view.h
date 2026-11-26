/** Xenia : Xbox 360 Emulator Research Project; BSD license, see LICENSE. */
#pragma once
#include <atomic>
#include <memory>
#include <mutex>
#include <span>
#include "xenia/kernel/xam/profile_gpd_model.h"
#include "xenia/vfs/file.h"
namespace xe::kernel {
class NativeProfileSaveOwner;
class NativeProfileSaveManagedGpd;
class NativeProfileGpdReadFile;
namespace xam::test {class NativeProfileSavePublicationTestAccess;}
// One immutable committed GPD view from common filesystem/source admission or
// a bounded actual native DB use. It is not a writer, Task ancestry or model-age lease.
// Raw descriptor observation is closed before final token/model admission;
// the resulting snapshot never self-conflicts with genuine later publication.
class NativeProfileGpdReadView final {
 private:
  friend class NativeProfileSaveOwner;
  friend class NativeProfileSavePublication;
  friend class NativeProfileSaveManagedGpd;
  friend class NativeProfileGpdReadFile;
  friend class xam::test::NativeProfileSavePublicationTestAccess;
  static std::shared_ptr<NativeProfileGpdReadView> Capture(
      std::shared_ptr<const xam::ProfileGpdModelAuthority>,xam::ProfileGpdReadCapture,
      uint32_t title,std::error_code&,
      const filesystem::SnapshotPublicationSet* members = nullptr);
  std::error_code Available() const;
  // Actual DB-use owner withdraws this exact generation at proved quiescence;
  // ordinary model publication alone must not fake GetFileSize failure.
  void RetireAtActualQuiescence();
  NativeProfileGpdReadView()=default;
  xam::ProfileGpdReadCapture read_;
  std::shared_ptr<const filesystem::HostStorageSnapshotToken> token_;
  vfs::File::Information information_;
  uint64_t native_allocated_bytes_=0;
  std::atomic<bool> retired_{false};
};
}  // namespace xe::kernel
