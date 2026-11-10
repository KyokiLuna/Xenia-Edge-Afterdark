/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/vfs/devices/xcontent_container_device.h"

#include <utility>
#include "xenia/base/logging.h"
#include "xenia/vfs/host_storage_errors.h"
#include "xenia/vfs/devices/host_path_file.h"
#include "xenia/vfs/devices/xcontent_devices/stfs_container_device.h"
#include "xenia/vfs/devices/xcontent_devices/svod_container_device.h"
#include "xenia/vfs/file.h"

namespace xe {
namespace vfs {

namespace {
class FileHandleReadBacking final : public XContentReadBacking {
 public:
  FileHandleReadBacking(
      std::unique_ptr<filesystem::FileHandle> file,
      const filesystem::FileHandleInfo& info,
      std::optional<std::vector<filesystem::GuestHostBacking>> backings,
      bool concurrent_reads)
      : XContentReadBacking(info.size), file_(std::move(file)), info_(info),
        backings_(std::move(backings)), concurrent_reads_(concurrent_reads) {}
  std::optional<std::vector<filesystem::GuestHostBacking>> GuestBackings()
      const override { return backings_; }
  bool supports_concurrent_io() const override { return concurrent_reads_; }

 protected:
  X_STATUS ReadPartial(size_t offset, std::span<uint8_t> output,
                       size_t* transferred) const override {
    *transferred = 0;
    std::error_code error;
    const auto before = file_->QueryInfo(error);
    if (!before || error) {
      return error ? HostStorageError(error) : X_STATUS_UNSUCCESSFUL;
    }
    if (!SameFile(*before)) { return X_STATUS_UNSUCCESSFUL; }
    const auto result = file_->ReadResult(offset, output);
    // ReadResult may report both bytes and an error. Later currentness failure
    // also cannot erase the fact that this prefix was already copied.
    *transferred = result.bytes;
    if (result.error) { return HostStorageError(result.error); }
    const auto after = file_->QueryInfo(error);
    if (!after || error) {
      return error ? HostStorageError(error) : X_STATUS_UNSUCCESSFUL;
    }
    if (!SameFile(*after)) { return X_STATUS_UNSUCCESSFUL; }
    return X_STATUS_SUCCESS;
  }

 private:
  bool SameFile(const filesystem::FileHandleInfo& value) const {
    return value.regular && !value.directory && value.size == info_.size &&
           value.filesystem_id == info_.filesystem_id &&
           value.file_id == info_.file_id && value.links == info_.links;
  }
  std::unique_ptr<filesystem::FileHandle> file_;
  const filesystem::FileHandleInfo info_;
  const std::optional<std::vector<filesystem::GuestHostBacking>> backings_;
  const bool concurrent_reads_;
};

struct DestroyFile {
  void operator()(File* file) const { file->Destroy(); }
};
using OwnedFile = std::unique_ptr<File, DestroyFile>;

class VfsReadBacking final : public XContentReadBacking {
 public:
  explicit VfsReadBacking(File* file)
      : XContentReadBacking(file->entry()->size()), file_(file) {}
  std::optional<std::vector<filesystem::GuestHostBacking>> GuestBackings()
      const override { return file_->retained_device()->GuestBackings(); }
  bool supports_concurrent_io() const override {
    return file_->retained_device()->supports_concurrent_io();
  }
  const Entry* package_entry() const override { return file_->entry(); }

 protected:
  X_STATUS ReadPartial(size_t offset, std::span<uint8_t> output,
                       size_t* transferred) const override {
    return file_->ReadSync(output, offset, transferred);
  }

 private:
  OwnedFile file_;
};

// The alias control block retains the real caller's owner, rather than giving
// this adapter a second independent File::Destroy responsibility.
class SharedVfsReadBacking final : public XContentReadBacking {
 public:
  SharedVfsReadBacking(std::shared_ptr<File> file,
                       filesystem::FileHandleInfo info)
      : XContentReadBacking(info.size), file_(std::move(file)), info_(info) {}
  std::optional<std::vector<filesystem::GuestHostBacking>> GuestBackings()
      const override { return file_->retained_device()->GuestBackings(); }
  bool supports_concurrent_io() const override {
    return file_->retained_device()->supports_concurrent_io();
  }
  const Entry* package_entry() const override { return file_->entry(); }

 protected:
  X_STATUS ReadPartial(size_t offset, std::span<uint8_t> output,
                       size_t* transferred) const override {
    *transferred = 0;
    const auto* host = static_cast<const HostPathFile*>(file_.get());
    std::error_code error;
    const auto before = host->QueryBackingInfo(error);
    if (!before || error) {
      return error ? HostStorageError(error) : X_STATUS_UNSUCCESSFUL;
    }
    if (!SameFile(*before)) {
      return X_STATUS_UNSUCCESSFUL;
    }
    const auto status = file_->ReadSync(output, offset, transferred);
    if (status != X_STATUS_SUCCESS) { return status; }
    const auto after = host->QueryBackingInfo(error);
    if (!after || error) {
      return error ? HostStorageError(error) : X_STATUS_UNSUCCESSFUL;
    }
    return SameFile(*after) ? X_STATUS_SUCCESS : X_STATUS_UNSUCCESSFUL;
  }

 private:
  bool SameFile(const filesystem::FileHandleInfo& value) const {
    return value.regular && !value.directory && value.size == info_.size &&
           value.filesystem_id == info_.filesystem_id &&
           value.file_id == info_.file_id && value.links == info_.links;
  }
  const std::shared_ptr<File> file_;
  const filesystem::FileHandleInfo info_;
};

bool ReadOnlyFileAdmitted(const File* file) {
  constexpr uint32_t writes = FileAccess::kGenericWrite |
                              FileAccess::kGenericAll |
                              FileAccess::kFileWriteData |
                              FileAccess::kFileAppendData |
                              FileAccess::kFileWriteAttributes;
  if (!file || !file->entry() || !file->retained_device() ||
      file->entry()->delete_on_close() || (file->file_access() & writes) ||
      !(file->file_access() & (FileAccess::kGenericRead |
                              FileAccess::kFileReadData))) {
    return false;
  }
  const auto* host = dynamic_cast<const HostPathFile*>(file);
  return !host || !host->HasMutatingClose();
}
}  // namespace

X_STATUS XContentReadBacking::ReadWithStatus(
    uint64_t offset, std::span<uint8_t> output, size_t* transferred) const {
  *transferred = 0;
  if (offset > extent_ || output.size() > extent_ - offset ||
      offset > SIZE_MAX) {
    return X_STATUS_INVALID_PARAMETER;
  }
  const auto status = ReadPartial(size_t(offset), output, transferred);
  if (*transferred > output.size()) {
    *transferred = 0;  // An invalid provider count is not a trusted prefix.
    return X_STATUS_UNSUCCESSFUL;
  }
  if (status != X_STATUS_SUCCESS) { return status; }
  return *transferred == output.size() ? X_STATUS_SUCCESS
                                       : X_STATUS_UNSUCCESSFUL;
}

bool XContentReadBacking::Read(uint64_t offset,
                               std::span<uint8_t> output) const {
  size_t transferred = 0;
  return ReadWithStatus(offset, output, &transferred) == X_STATUS_SUCCESS;
}

std::unique_ptr<XContentReadBacking> XContentReadBacking::FromFileHandle(
    std::unique_ptr<filesystem::FileHandle> file,
    std::optional<std::vector<filesystem::GuestHostBacking>> backings,
    bool concurrent_reads) {
  if (!file) { return {}; }
  std::error_code error;
  const auto info = file->QueryInfo(error);
  if (!info || error || !info->regular || info->directory ||
      info->size > SIZE_MAX) {
    return {};
  }
  return std::make_unique<FileHandleReadBacking>(
      std::move(file), *info, std::move(backings), concurrent_reads);
}

std::unique_ptr<XContentReadBacking> XContentReadBacking::FromFile(File*& file) {
  // Admission precedes ownership construction: refusal must not finish an
  // overwrite, delete a file or consume the caller's pointer.
  if (!ReadOnlyFileAdmitted(file)) { return {}; }
  auto backing = std::make_unique<VfsReadBacking>(file);
  file = nullptr;
  return backing;
}

std::unique_ptr<XContentReadBacking> XContentReadBacking::FromSharedFile(
    const std::shared_ptr<File>& file, X_STATUS* status) {
  auto fail = [&](X_STATUS value) -> std::unique_ptr<XContentReadBacking> {
    if (status) { *status = value; }
    return {};
  };
  if (global_critical_region::is_held_by_current_thread()) {
    return fail(X_STATUS_INVALID_DEVICE_REQUEST);
  }
  if (!ReadOnlyFileAdmitted(file.get())) { return fail(X_STATUS_ACCESS_DENIED); }
  // Do not use a stale Entry size or infer host ownership for another backend.
  const auto* host = dynamic_cast<const HostPathFile*>(file.get());
  if (!host) { return fail(X_STATUS_NOT_SUPPORTED); }
  std::error_code error;
  const auto info = host->QueryBackingInfo(error);
  if (!info || error) {
    return fail(error ? HostStorageError(error) : X_STATUS_UNSUCCESSFUL);
  }
  if (!info->regular || info->directory || info->size > SIZE_MAX) {
    return fail(X_STATUS_INVALID_PARAMETER);
  }
  if (status) { *status = X_STATUS_SUCCESS; }
  return std::make_unique<SharedVfsReadBacking>(file, *info);
}

std::unique_ptr<XContentReadBacking> XContentReadBacking::Open(
    const std::filesystem::path& path) {
  auto file = filesystem::FileHandle::OpenExisting(
      path, FileAccess::kFileReadData);
  if (!file) { return {}; }
  std::error_code error;
  const auto info = file->QueryInfo(error);
  if (!info || error || !info->regular || info->directory ||
      info->size > SIZE_MAX) { return {}; }
  return std::make_unique<FileHandleReadBacking>(std::move(file), *info,
      std::vector<filesystem::GuestHostBacking>{{path, false, *info}}, true);
}

std::unique_ptr<XContentContainerDevice>
XContentContainerDevice::CreateContentDevice(
    const std::string_view mount_path, const std::filesystem::path& host_path) {
  auto backing = XContentReadBacking::Open(host_path);
  const auto header = backing ? ReadContainerHeader(*backing) : nullptr;
  if (!header || !header->content_header.is_magic_valid()) { return {}; }
  switch (header->content_metadata.volume_type) {
    case XContentVolumeType::kStfs:
      return std::make_unique<StfsContainerDevice>(
          mount_path, std::move(backing), host_path);
    case XContentVolumeType::kSvod:
      // Fragmented SVOD keeps its existing dedicated host-file reader.
      return std::make_unique<SvodContainerDevice>(mount_path, host_path);
    default:
      return {};
  }
}

std::unique_ptr<XContentContainerDevice>
XContentContainerDevice::CreateContentDevice(const std::string_view mount_path,
                                             Entry* entry) {
  if (!entry || (entry->attributes() & kFileAttributeDirectory)) { return {}; }
  std::unique_ptr<XContentReadBacking> backing;
  {
    // Reserve the same metadata interval as ordinary Entry::Open. A pending
    // overwrite can otherwise be inherited by a new handle with only read bits.
    // No header/parser read is performed while this interval is held.
    auto global_lock = global_critical_region::AcquireDirect();
    if (HostPathFile::EntryHasMutatingClose(entry)) { return {}; }
    File* file = nullptr;
    if (entry->Open(FileAccess::kFileReadData, &file) != X_STATUS_SUCCESS) {
      return {};
    }
    backing = XContentReadBacking::FromFile(file);
    if (!backing) {
      // The precheck and Open are atomic against pending mutation adoption.
      // This factory owns only its own ordinary read handle on failure.
      if (file) { file->Destroy(); }
      return {};
    }
  }
  return CreateContentDevice(mount_path, std::move(backing));
}

std::unique_ptr<XContentContainerDevice>
XContentContainerDevice::CreateContentDevice(
    const std::string_view mount_path,
    std::unique_ptr<XContentReadBacking> backing) {
  const auto header = backing ? ReadContainerHeader(*backing) : nullptr;
  if (!header || !header->content_header.is_magic_valid() ||
      header->content_metadata.volume_type != XContentVolumeType::kStfs) {
    return {};
  }
  return std::make_unique<StfsContainerDevice>(mount_path, std::move(backing));
}

XContentContainerDevice::XContentContainerDevice(
    const std::string_view mount_path, const std::filesystem::path& host_path)
    : Device(mount_path),
      name_("XContent"),
      host_path_(host_path),
      files_total_size_(0),
      header_(std::make_unique<XContentContainerHeader>()) {}

XContentContainerDevice::XContentContainerDevice(
    const std::string_view mount_path, File*& package_file)
    : XContentContainerDevice(
          mount_path, XContentReadBacking::FromFile(package_file)) {}

XContentContainerDevice::XContentContainerDevice(
    const std::string_view mount_path,
    std::unique_ptr<XContentReadBacking> backing,
    const std::filesystem::path& host_path)
    : XContentContainerDevice(mount_path, host_path) {
  backing_required_ = true;
  backing_ = std::move(backing);
}

XContentContainerDevice::~XContentContainerDevice() = default;

bool XContentContainerDevice::Initialize() {
  std::unique_ptr<XContentContainerHeader> header;
  if (backing_required_) {
    if (!backing_ || backing_->extent() > SIZE_MAX) { return false; }
    files_total_size_ = size_t(backing_->extent());
    header = ReadContainerHeader(*backing_);
  } else {
    if (!std::filesystem::exists(host_path_)) {
      XELOGE("Path to XContent container does not exist: {}",
             xe::strip_pii(xe::path_to_utf8(host_path_)));
      return false;
    }

    if (std::filesystem::is_directory(host_path_)) {
      return false;
    }

    XELOGI("Loading XContent header file: {}",
           xe::strip_pii(xe::path_to_utf8(host_path_)));
    auto header_file = xe::filesystem::OpenFile(host_path_, "rb");
    if (!header_file) {
      XELOGE("Error opening XContent header file.");
      return false;
    }
    files_total_size_ = std::filesystem::file_size(host_path_);
    header = ReadContainerHeader(header_file);
    fclose(header_file);
  }

  auto header_loading_result = VerifyHeader(std::move(header));
  if (header_loading_result != Result::kSuccess) {
    XELOGE("Error reading XContent header: {}",
           static_cast<int32_t>(header_loading_result));
    return false;
  }

  SetupContainer();

  if (LoadHostFiles() != Result::kSuccess) {
    XELOGE("Error loading XContent host files.");
    return false;
  }

  return Read() == Result::kSuccess;
}

std::unique_ptr<XContentContainerHeader>
XContentContainerDevice::ReadContainerHeader(const XContentReadBacking& backing) {
  auto header = std::make_unique<XContentContainerHeader>();
  if (!backing.Read(0, {reinterpret_cast<uint8_t*>(header.get()),
                        sizeof(*header)})) {
    return {};
  }
  return header;
}

std::unique_ptr<XContentContainerHeader>
XContentContainerDevice::ReadContainerHeader(
    const std::filesystem::path& file_path) {
  auto backing = XContentReadBacking::Open(file_path);
  return backing ? ReadContainerHeader(*backing) : nullptr;
}

std::unique_ptr<XContentContainerHeader>
XContentContainerDevice::ReadContainerHeader(FILE* host_file) {
  std::unique_ptr<XContentContainerHeader> header =
      std::make_unique<XContentContainerHeader>();

  // Read header & check signature
  if (fread(header.get(), sizeof(XContentContainerHeader), 1, host_file) != 1) {
    return nullptr;
  }
  return header;
}

std::unique_ptr<XContentContainerHeader>
XContentContainerDevice::ReadContainerHeader(File* file) {
  auto header = std::make_unique<XContentContainerHeader>();
  size_t bytes_read = 0;
  if (file->ReadSync({reinterpret_cast<uint8_t*>(header.get()),
                      sizeof(XContentContainerHeader)},
                     0, &bytes_read) != X_STATUS_SUCCESS ||
      bytes_read != sizeof(XContentContainerHeader)) {
    return nullptr;
  }
  return header;
}

Entry* XContentContainerDevice::ResolvePath(const std::string_view path) {
  // The filesystem will have stripped our prefix off already, so the path will
  // be in the form:
  // some\PATH.foo
  XELOGFS("StfsContainerDevice::ResolvePath({})", path);
  return root_entry_->ResolvePath(path);
}

void XContentContainerDevice::Dump(StringBuffer* string_buffer) {
  auto global_lock = global_critical_region_.Acquire();
  root_entry_->Dump(string_buffer, 0);
}

kernel::xam::XCONTENT_AGGREGATE_DATA
XContentContainerDevice::ContentDataFromHeader(
    const XContentContainerHeader& header) {
  kernel::xam::XCONTENT_AGGREGATE_DATA data;

  std::memset(&data, 0, sizeof(kernel::xam::XCONTENT_AGGREGATE_DATA));

  data.device_id = 1;
  data.title_id = header.content_metadata.execution_info.title_id;
  data.content_type = header.content_metadata.content_type;

  auto name = header.content_metadata.display_name(XLanguage::kEnglish);
  if (name.empty()) {
    // Find first filled language and use it. It might be incorrect, but meh
    // until stfs support is done.
    for (uint32_t i = 1; i <= header.content_metadata.kNumLanguagesV2; i++) {
      name = header.content_metadata.display_name((XLanguage)i);
      if (!name.empty()) {
        break;
      }
    }
  }

  data.set_display_name(name);

  return data;
}

uint32_t XContentContainerDevice::LicenseMaskFromHeader(
    const XContentContainerHeader& header) {
  uint32_t final_license = 0;
  for (uint8_t i = 0; i < license_count; i++) {
    final_license |= header.content_header.licenses[i].license_bits;
  }
  return final_license;
}

kernel::xam::XCONTENT_AGGREGATE_DATA XContentContainerDevice::content_header()
    const {
  return ContentDataFromHeader(*header_);
}

XContentContainerDevice::Result XContentContainerDevice::VerifyHeader(
    std::unique_ptr<XContentContainerHeader> header) {
  if (files_total_size_ < sizeof(XContentContainerHeader)) {
    return Result::kTooSmall;
  }

  if (header == nullptr) {
    return Result::kReadError;
  }

  if (!header->content_header.is_magic_valid()) {
    // Unexpected format.
    return Result::kFileMismatch;
  }

  header_ = std::move(header);
  return Result::kSuccess;
}

}  // namespace vfs
}  // namespace xe
