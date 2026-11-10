/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_VFS_DEVICES_XCONTENT_CONTAINER_DEVICE_H_
#define XENIA_VFS_DEVICES_XCONTENT_CONTAINER_DEVICE_H_

#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "xenia/kernel/xam/content_manager.h"
#include "xenia/vfs/device.h"
#include "xenia/vfs/devices/stfs_xbox.h"
#include "xenia/vfs/file.h"

namespace xe {
namespace vfs {

constexpr fourcc_t kLIVESignature = make_fourcc("LIVE");
constexpr fourcc_t kCONSignature = make_fourcc("CON ");
constexpr fourcc_t kPIRSSignature = make_fourcc("PIRS");

// Owned offset reader for a single existing package. It grants no write or
// native-device authority. Providers retain their actual file/source lifetime;
// extent is captured from that object, never a synthetic logical-volume size.
class XContentReadBacking {
 public:
  virtual ~XContentReadBacking() = default;
  uint64_t extent() const { return extent_; }
  // Exact, bounded reads only. Preserve actual provider errors and the bytes
  // already transferred; an unexpected successful short read is an I/O failure,
  // not logical EOF of a file inside the package. Invalid ranges never enter
  // the provider. The bool adapter is only for exact parser reads.
  X_STATUS ReadWithStatus(uint64_t offset, std::span<uint8_t> output,
                          size_t* transferred) const;
  bool Read(uint64_t offset, std::span<uint8_t> output) const;
  virtual std::optional<std::vector<filesystem::GuestHostBacking>>
  GuestBackings() const = 0;
  virtual bool supports_concurrent_io() const { return false; }
  virtual const Entry* package_entry() const { return nullptr; }

  // Transfers an existing handle (including its policy/source lease). QueryInfo
  // supplies the actual extent. Mount provenance is retained, not inferred.
  static std::unique_ptr<XContentReadBacking> FromFileHandle(
      std::unique_ptr<filesystem::FileHandle>,
      std::optional<std::vector<filesystem::GuestHostBacking>>,
      bool concurrent_reads = false);
  // Read-only admission borrows first. Failure leaves the exact caller-owned
  // file untouched; success consumes it and sets the pointer to null. The
  // retained Device keeps the Entry tree and captured logical EOF alive.
  static std::unique_ptr<XContentReadBacking> FromFile(File*&);
  // Nonconsuming admission. The shared alias must retain the actual File's
  // owner (for example its existing XFile); it must not Destroy that borrowed
  // File independently. Failure does not change the caller's owner. Initially
  // supports HostPathFile with real retained-handle size/identity queries.
  // The same read-only/mutating-close policy as FromFile applies.
  static std::unique_ptr<XContentReadBacking> FromSharedFile(
      const std::shared_ptr<File>& file, X_STATUS* status = nullptr);
  static std::unique_ptr<XContentReadBacking> Open(
      const std::filesystem::path&);

 protected:
  explicit XContentReadBacking(uint64_t extent) : extent_(extent) {}
  virtual X_STATUS ReadPartial(size_t offset, std::span<uint8_t>,
                               size_t* transferred) const = 0;

 private:
  const uint64_t extent_;
};

class XContentContainerDevice : public Device {
 public:
  constexpr static uint32_t kBlockSize = 0x1000;

  static std::unique_ptr<XContentContainerDevice> CreateContentDevice(
      const std::string_view mount_path,
      const std::filesystem::path& host_path);

  // A device for the STFS package stored in |entry|, whose device has to
  // outlive it.
  static std::unique_ptr<XContentContainerDevice> CreateContentDevice(
      const std::string_view mount_path, Entry* entry);

  // STFS only: the retained reader supplies the same parser without a host
  // reopen or whole-file mapping. SVOD still needs its fragmented host source.
  static std::unique_ptr<XContentContainerDevice> CreateContentDevice(
      const std::string_view mount_path,
      std::unique_ptr<XContentReadBacking> backing);

  static std::unique_ptr<XContentContainerHeader> ReadContainerHeader(
      const XContentReadBacking& backing);

  static std::unique_ptr<XContentContainerHeader> ReadContainerHeader(
      const std::filesystem::path& file_path);

  static std::unique_ptr<XContentContainerHeader> ReadContainerHeader(
      FILE* host_file);

  static std::unique_ptr<XContentContainerHeader> ReadContainerHeader(
      File* file);

  ~XContentContainerDevice() override;

  bool Initialize() override;
  std::optional<std::vector<filesystem::GuestHostBacking>> GuestBackings()
      const override {
    if (backing_required_) {
      return backing_ ? backing_->GuestBackings() : std::nullopt;
    }
    if (host_path_.empty()) {
      return std::nullopt;
    }
    return std::vector<filesystem::GuestHostBacking>{{host_path_, false}};
  }

  const std::string& name() const override { return name_; }
  uint32_t attributes() const override { return 0; }

  uint32_t sectors_per_allocation_unit() const override { return 8; }
  uint32_t bytes_per_sector() const override { return 0x200; }

  size_t data_size() const {
    if (header_->content_header.header_size) {
      return files_total_size_ -
             xe::round_up(header_->content_header.header_size, kBlockSize);
    }
    return files_total_size_ - sizeof(XContentContainerHeader);
  }

  uint64_t xuid() const { return header_->content_metadata.profile_id; }

  uint32_t title_id() const {
    return header_->content_metadata.execution_info.title_id;
  }

  uint32_t content_type() const {
    return static_cast<uint32_t>(header_->content_metadata.content_type.get());
  }

  static kernel::xam::XCONTENT_AGGREGATE_DATA ContentDataFromHeader(
      const XContentContainerHeader& header);

  static uint32_t LicenseMaskFromHeader(const XContentContainerHeader& header);

  kernel::xam::XCONTENT_AGGREGATE_DATA content_header() const;
  uint32_t license_mask() const { return LicenseMaskFromHeader(*header_); }

  const std::filesystem::path& GetHostPath() const { return host_path_; }
  // The package's file on another device, null for a host file.
  const Entry* package_entry() const {
    return backing_ ? backing_->package_entry() : nullptr;
  }

  const XContentContainerHeader* GetContainerHeader() const {
    return header_.get();
  }

 protected:
  XContentContainerDevice(const std::string_view mount_path,
                          const std::filesystem::path& host_path);
  // Consumes/nulls only after read-only admission; failure leaves the caller
  // responsible for the original file and causes Initialize to fail.
  XContentContainerDevice(const std::string_view mount_path,
                          File*& package_file);
  XContentContainerDevice(const std::string_view mount_path,
                          std::unique_ptr<XContentReadBacking> backing,
                          const std::filesystem::path& host_path = {});
  enum class Result {
    kSuccess = 0,
    kOutOfMemory = -1,
    kReadError = -10,
    kFileMismatch = -30,
    kDamagedFile = -31,
    kTooSmall = -32,
  };

  virtual Result Read() = 0;
  // Load all host files. Usually STFS is only 1 file, meanwhile SVOD is usually
  // multiple file.
  virtual Result LoadHostFiles() = 0;
  // Initialize container specific fields.
  virtual void SetupContainer() {};

  Entry* ResolvePath(const std::string_view path) override;
  void Dump(StringBuffer* string_buffer) override;
  Result VerifyHeader(std::unique_ptr<XContentContainerHeader> header);

  void SetName(std::string name) { name_ = name; }
  const std::string& GetName() const { return name_; }

  void SetFilesSize(uint64_t files_size) { files_total_size_ = files_size; }
  const uint64_t GetFilesSize() const { return files_total_size_; }

  std::string name_;
  std::filesystem::path host_path_;
  // Same owned bounded reader for host STFS and already-open source objects.
  std::unique_ptr<XContentReadBacking> backing_;
  bool backing_required_ = false;

  size_t files_total_size_;
  std::unique_ptr<Entry> root_entry_;
  std::unique_ptr<XContentContainerHeader> header_;
};

}  // namespace vfs
}  // namespace xe

#endif
