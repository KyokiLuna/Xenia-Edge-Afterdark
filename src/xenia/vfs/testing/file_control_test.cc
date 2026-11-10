/**
 * Xenia : Xbox 360 Emulator Research Project
 * Released under the BSD license - see LICENSE in the root for details.
 */
#include <algorithm>
#include <array>
#include <fstream>
#include <memory>
#include <optional>
#include <vector>
#include <string>
#include <random>
#include "third_party/catch/include/catch.hpp"
#include "xenia/base/platform.h"
#if !XE_PLATFORM_WIN32
#include <sys/stat.h>
#include <unistd.h>
#endif
#include "xenia/kernel/kernel.h"
#include "xenia/vfs/devices/host_path_device.h"
#include "xenia/vfs/devices/host_path_entry.h"
#include "xenia/vfs/devices/host_path_file.h"
#include "xenia/vfs/devices/null_device.h"
#include "xenia/vfs/folder_volume_control.h"
#include "xenia/vfs/virtual_file_system.h"

namespace xe::vfs::test {
static_assert(kFolderVolumeDriveGeometry ==
              kernel::X_IOCTL_DISK_GET_DRIVE_GEOMETRY);
static_assert(kFolderVolumePartitionInfo ==
              kernel::X_IOCTL_DISK_GET_PARTITION_INFO);
namespace {
struct TemporaryDirectory {
  std::filesystem::path path =
      std::filesystem::temp_directory_path() /
      ("xenia-file-control-" + std::to_string(std::random_device{}()));
  TemporaryDirectory() { REQUIRE(std::filesystem::create_directory(path)); }
  ~TemporaryDirectory() {
    std::error_code error;
    std::filesystem::remove_all(path, error);
  }
};
struct FileOwner {
  File* file = nullptr;
  ~FileOwner() {
    if (file) {
      file->Destroy();
    }
  }
};
class FlushFailure final : public filesystem::FileHandle {
 public:
  explicit FlushFailure(bool supported)
      : FileHandle(std::filesystem::path()), supported_(supported) {}
  bool Read(size_t, void*, size_t, size_t* count) override {
    *count = 0;
    return false;
  }
  bool Write(size_t, const void*, size_t, size_t* count) override {
    *count = 0;
    return false;
  }
  bool SetLength(size_t) override { return false; }
  bool SetTimes(uint64_t, uint64_t, uint64_t) override { return false; }
  void Flush() override { ++void_flushes; }
  std::error_code FlushResult() override {
    if (!supported_) {
      return FileHandle::FlushResult();
    }
    ++actual_flushes;
    return std::make_error_code(std::errc::io_error);
  }
  uint32_t actual_flushes = 0, void_flushes = 0;

 private:
  bool supported_;
};
}  // namespace

TEST_CASE(
    "Folder volume controls encode bounded live geometry without mutation",
    "[vfs][file_control]") {
  filesystem::HostStorageSnapshot snapshot;
  snapshot.available = true;
  snapshot.capacity_bytes = 512 * 123456ull + 17;
  snapshot.available_bytes = 9000000;
  snapshot.reserve_bytes = 1000000;
  snapshot.usable_bytes = 8000000;
  std::array<uint8_t, 32> output;
  output.fill(0xA5);
  uint32_t count = 77;
  REQUIRE(EncodeFolderVolumeControl(kFolderVolumeDriveGeometry, snapshot, 512,
                                    {}, output, &count) == X_STATUS_SUCCESS);
  REQUIRE(count == 8);
  CHECK(xe::load_and_swap<uint32_t>(output.data()) == 123456);
  CHECK(xe::load_and_swap<uint32_t>(output.data() + 4) == 512);
  CHECK(std::all_of(output.begin() + 8, output.end(),
                    [](uint8_t value) { return value == 0xA5; }));
  output.fill(0xA5);
  REQUIRE(EncodeFolderVolumeControl(kFolderVolumePartitionInfo, snapshot, 512,
                                    {}, output, &count) == X_STATUS_SUCCESS);
  REQUIRE(count == 16);
  CHECK(xe::load_and_swap<uint64_t>(output.data()) == 0);
  CHECK(xe::load_and_swap<uint64_t>(output.data() + 8) == 512 * 123456ull);
  CHECK(std::all_of(output.begin() + 16, output.end(),
                    [](uint8_t value) { return value == 0xA5; }));

  for (uint32_t code :
       {kFolderVolumeDriveGeometry, kFolderVolumePartitionInfo}) {
    const auto layout = DescribeFolderVolumeControl(code);
    REQUIRE(layout);
    for (uint32_t length : {0u, layout->output_bytes - 1}) {
      output.fill(0xA5);
      const auto before = output;
      count = 77;
      CHECK(EncodeFolderVolumeControl(code, snapshot, 512, {},
                                      std::span(output).first(length),
                                      &count) == X_STATUS_BUFFER_TOO_SMALL);
      CHECK(count == 0);
      CHECK(output == before);
    }
  }
  output.fill(0xA5);
  const auto before = output;
  CHECK(EncodeFolderVolumeControl(0x70042, snapshot, 512, {}, output, &count) ==
        X_STATUS_INVALID_DEVICE_REQUEST);
  CHECK(count == 0);
  CHECK(output == before);
  const std::array<uint8_t, 1> input{0};
  CHECK(EncodeFolderVolumeControl(kFolderVolumeDriveGeometry, snapshot, 512,
                                  input, output,
                                  &count) == X_STATUS_INVALID_PARAMETER);
  CHECK(output == before);
  snapshot.capacity_bytes = (uint64_t(UINT32_MAX) + 1) * 512;
  CHECK(EncodeFolderVolumeControl(kFolderVolumeDriveGeometry, snapshot, 512, {},
                                  output, &count) == X_STATUS(0xC0000095));
  CHECK(count == 0);
  CHECK(output == before);
  snapshot.available = false;
  CHECK(EncodeFolderVolumeControl(kFolderVolumePartitionInfo, snapshot, 512, {},
                                  output, &count) == X_STATUS(0xC000009D));
  snapshot.error = std::make_error_code(std::errc::permission_denied);
  CHECK(EncodeFolderVolumeControl(kFolderVolumePartitionInfo, snapshot, 512, {},
                                  output, &count) == X_STATUS_ACCESS_DENIED);
  CHECK(output == before);
}

#if !XE_PLATFORM_WIN32
TEST_CASE("Only explicit retained folder volume handles have disk controls",
          "[vfs][file_control]") {
  TemporaryDirectory directory;
  std::filesystem::create_directory(directory.path / "sub");
  std::ofstream(directory.path / "file") << "value";
  std::error_code error;
  auto source =
      filesystem::HostStorageRoot::Open(directory.path, false, error, 0);
  REQUIRE(source);
  HostPathDevice ordinary("\\Device\\Harddisk0\\Partition1", directory.path,
                          false, false, "FATX", source);
  REQUIRE(ordinary.Initialize());
  FileOwner denied;
  REQUIRE(ordinary.ResolvePath("")->Open(FileAccess::kFileReadData,
                                         &denied.file) == X_STATUS_SUCCESS);
  CHECK_FALSE(denied.file->DescribeDeviceControl(kFolderVolumeDriveGeometry));

  HostPathDevice volume("\\Device\\Volume", directory.path, false, false,
                        "FATX", source, source,
                        HostPathDevice::ControlRole::kFolderVolume);
  REQUIRE(volume.Initialize());
  FileOwner root;
  REQUIRE(volume.ResolvePath("")->Open(FileAccess::kFileReadData, &root.file) ==
          X_STATUS_SUCCESS);
  REQUIRE(root.file->DescribeDeviceControl(kFolderVolumeDriveGeometry));
  std::array<uint8_t, 32> bytes;
  bytes.fill(0xA5);
  uint32_t count = 0;
  const auto snapshot = volume.storage_space();
  REQUIRE(snapshot.available);
  REQUIRE(root.file->DeviceControl(kFolderVolumePartitionInfo, {}, bytes,
                                   &count) == X_STATUS_SUCCESS);
  CHECK(count == 16);
  CHECK(xe::load_and_swap<uint64_t>(bytes.data() + 8) ==
        snapshot.capacity_bytes / 512 * 512);
  CHECK(bytes[16] == 0xA5);
  for (const auto* name : {"sub", "file"}) {
    FileOwner child;
    REQUIRE(volume.ResolvePath(name)->Open(FileAccess::kFileReadData,
                                           &child.file) == X_STATUS_SUCCESS);
    CHECK_FALSE(child.file->DescribeDeviceControl(kFolderVolumeDriveGeometry));
    CHECK(child.file->DeviceControl(kFolderVolumeDriveGeometry, {}, bytes,
                                    &count) == X_STATUS_INVALID_DEVICE_REQUEST);
    CHECK(count == 0);
  }
  FileOwner attributes_only;
  REQUIRE(volume.ResolvePath("")->Open(0x80,  // FILE_READ_ATTRIBUTES only.
                                       &attributes_only.file) ==
          X_STATUS_SUCCESS);
  CHECK(attributes_only.file->DeviceControl(kFolderVolumePartitionInfo, {},
                                            bytes,
                                            &count) == X_STATUS_ACCESS_DENIED);
  volume.Retire();
  CHECK(root.file->DeviceControl(kFolderVolumePartitionInfo, {}, bytes,
                                 &count) == X_STATUS(0xC000009D));
  CHECK(count == 0);
}

TEST_CASE(
    "Real file flush persists bytes and reports backend and retired errors",
    "[vfs][file_control]") {
  TemporaryDirectory directory;
  std::ofstream(directory.path / "file") << "old";
  std::error_code error;
  auto source =
      filesystem::HostStorageRoot::Open(directory.path, false, error, 0);
  REQUIRE(source);
  {
    HostPathDevice device("\\Device\\Cache", directory.path, false, false,
                          "FATX", source);
    REQUIRE(device.Initialize());
    FileOwner file;
    REQUIRE(device.ResolvePath("file")->Open(
                FileAccess::kFileWriteData | FileAccess::kFileReadData,
                &file.file) == X_STATUS_SUCCESS);
    const std::array<uint8_t, 3> bytes{'n', 'e', 'w'};
    size_t count = 0;
    REQUIRE(file.file->WriteSync(bytes, 0, &count) == X_STATUS_SUCCESS);
    REQUIRE(count == bytes.size());
    REQUIRE(file.file->Flush() == X_STATUS_SUCCESS);
  }
  {
    HostPathDevice reopened("\\Device\\Cache", directory.path, false, false,
                            "FATX", source);
    REQUIRE(reopened.Initialize());
    FileOwner file;
    REQUIRE(reopened.ResolvePath("file")->Open(FileAccess::kFileReadData,
                                               &file.file) == X_STATUS_SUCCESS);
    std::array<uint8_t, 3> bytes{};
    size_t count = 0;
    REQUIRE(file.file->ReadSync(bytes, 0, &count) == X_STATUS_SUCCESS);
    CHECK((bytes == std::array<uint8_t, 3>{'n', 'e', 'w'}));
    CHECK(file.file->Flush() == X_STATUS_ACCESS_DENIED);
    auto* entry = static_cast<HostPathEntry*>(reopened.ResolvePath("file"));
    for (bool supported : {false, true}) {
      auto backend = std::make_unique<FlushFailure>(supported);
      auto* observed = backend.get();
      FileOwner injected;
      injected.file = new HostPathFile(FileAccess::kFileWriteData, entry,
                                       std::move(backend));
      CHECK(injected.file->Flush() ==
            (supported ? X_STATUS_UNSUCCESSFUL : X_STATUS_NOT_SUPPORTED));
      CHECK(observed->actual_flushes == uint32_t(supported));
      CHECK(observed->void_flushes == 0);
    }
  }
  HostPathDevice device("\\Device\\RetiredCache", directory.path, false, false,
                        "FATX", source);
  REQUIRE(device.Initialize());
  FileOwner file;
  REQUIRE(device.ResolvePath("file")->Open(FileAccess::kFileWriteData,
                                           &file.file) == X_STATUS_SUCCESS);
  source->Retire();
  CHECK(file.file->Flush() == X_STATUS(0xC000009D));
}

TEST_CASE("Logical overwrite flush reports actual EOF durability",
          "[vfs][file_control]") {
  TemporaryDirectory directory;
  std::ofstream(directory.path / "file") << "abcdefgh";
  std::error_code error;
  auto source =
      filesystem::HostStorageRoot::Open(directory.path, false, error, 0);
  REQUIRE(source);
  HostPathDevice device("\\Device\\OverwriteFlush", directory.path, false,
                        false, "FATX", source);
  REQUIRE(device.Initialize());
  auto* entry = static_cast<HostPathEntry*>(device.ResolvePath("file"));
  REQUIRE(entry->Overwrite());
  {
    FileOwner file;
    REQUIRE(entry->Open(FileAccess::kFileWriteData, &file.file) ==
            X_STATUS_SUCCESS);
    const std::array<uint8_t, 1> bytes{'x'};
    size_t count = 0;
    REQUIRE(file.file->WriteSync(bytes, 0, &count) == X_STATUS_SUCCESS);
#if XE_PLATFORM_WIN32
    // The Windows overwrite path has no policy reservation/typed commit yet.
    REQUIRE(file.file->Flush() == X_STATUS_NOT_SUPPORTED);
    CHECK(std::filesystem::file_size(directory.path / "file") == 8);
#else
    REQUIRE(file.file->Flush() == X_STATUS_SUCCESS);
    CHECK(std::filesystem::file_size(directory.path / "file") == 1);
#endif
  }
  CHECK(std::filesystem::file_size(directory.path / "file") == 1);
}

TEST_CASE("Named caches persist independently and do not become raw endpoints",
          "[vfs][file_control]") {
  TemporaryDirectory directory;
  for (const auto* folder : {"cache", "cache0", "cache1"}) {
    REQUIRE(std::filesystem::create_directory(directory.path / folder));
    std::ofstream(directory.path / folder / "file") << "old";
  }
  for (bool reopen : {false, true}) {
    VirtualFileSystem fs;
    auto raw = std::make_unique<NullDevice>(
        "\\Device\\Harddisk0", std::initializer_list<std::string>{
                                   "\\Partition0", "\\Cache0", "\\Cache1"});
    REQUIRE(raw->Initialize());
    REQUIRE(fs.RegisterDevice(std::move(raw)));
    uint8_t value = 1;
    for (const auto* folder : {"cache", "cache0", "cache1"}) {
      std::error_code error;
      auto source = filesystem::HostStorageRoot::Open(directory.path / folder,
                                                      false, error, 0);
      REQUIRE(source);
      const auto mount = std::string("\\") + folder;
      auto cache = std::make_unique<HostPathDevice>(
          mount, directory.path / folder, false, false, "FATX", source, source,
          HostPathDevice::ControlRole::kFolderVolume);
      REQUIRE(cache->Initialize());
      REQUIRE(fs.RegisterDevice(std::move(cache)));
      REQUIRE(fs.RegisterSymbolicLink(std::string(folder) + ":", mount));
      FileOwner file;
      FileAction action;
      REQUIRE(
          fs.OpenFile(nullptr, std::string(folder) + ":\\file",
                      FileDisposition::kOpen,
                      FileAccess::kFileReadData | FileAccess::kFileWriteData,
                      false, true, &file.file, &action) == X_STATUS_SUCCESS);
      size_t count = 0;
      if (!reopen) {
        const std::array<uint8_t, 1> bytes{value};
        REQUIRE(file.file->WriteSync(bytes, 0, &count) == X_STATUS_SUCCESS);
        REQUIRE(file.file->Flush() == X_STATUS_SUCCESS);
      } else {
        std::array<uint8_t, 1> bytes{};
        REQUIRE(file.file->ReadSync(bytes, 0, &count) == X_STATUS_SUCCESS);
        REQUIRE(count == 1);
        CHECK(bytes[0] == value);
      }
      ++value;
    }
    // Longest-prefix resolution retains the actual more-specific volume,
    // rather than letting the raw Harddisk0 endpoint consume Partition1.
    std::error_code error;
    auto source = filesystem::HostStorageRoot::Open(directory.path / "cache",
                                                    false, error, 0);
    REQUIRE(source);
    auto partition = std::make_unique<HostPathDevice>(
        "\\Device\\Harddisk0\\Partition1", directory.path / "cache", false,
        false, "FATX", source, source,
        HostPathDevice::ControlRole::kFolderVolume);
    REQUIRE(partition->Initialize());
    REQUIRE(fs.RegisterDevice(std::move(partition)));
    FileOwner file;
    FileAction action;
    REQUIRE(fs.OpenFile(nullptr, "\\Device\\Harddisk0\\Partition1\\file",
                        FileDisposition::kOpen, FileAccess::kFileReadData,
                        false, true, &file.file, &action) == X_STATUS_SUCCESS);
    std::array<uint8_t, 1> bytes{};
    size_t count = 0;
    REQUIRE(file.file->ReadSync(bytes, 0, &count) == X_STATUS_SUCCESS);
    CHECK(bytes[0] == 1);
  }
}
#endif

TEST_CASE("Raw placeholders never acknowledge discarded nonempty storage I/O",
          "[vfs][file_control]") {
  VirtualFileSystem fs;
  auto device = std::make_unique<NullDevice>(
      "\\Device\\Harddisk0", std::initializer_list<std::string>{
                                 "\\Partition0", "\\Cache0", "\\Cache1"});
  REQUIRE(device->Initialize());
  REQUIRE(fs.RegisterDevice(std::move(device)));
  FileOwner file;
  FileAction action;
  REQUIRE(fs.OpenFile(nullptr, "\\Device\\Harddisk0\\Cache0",
                      FileDisposition::kOpen,
                      FileAccess::kFileReadData | FileAccess::kFileWriteData,
                      false, true, &file.file, &action) == X_STATUS_SUCCESS);
  REQUIRE(file.file);
  std::array<uint8_t, 16> bytes;
  bytes.fill(0xA5);
  const auto before = bytes;
  size_t count = 77;
  CHECK(file.file->ReadSync(bytes, 0, &count) == X_STATUS_NOT_SUPPORTED);
  CHECK(count == 0);
  CHECK(bytes == before);
  xe::store_and_swap<uint32_t>(bytes.data(), 0x58544146);  // Real FATX magic.
  xe::store_and_swap<uint32_t>(bytes.data() + 8, 8);
  const auto old_cluster = file.file->entry()->sectors_per_allocation_unit();
  CHECK(file.file->WriteSync(bytes, 0, &count) == X_STATUS_NOT_SUPPORTED);
  CHECK(count == 0);
  CHECK(file.file->entry()->sectors_per_allocation_unit() == old_cluster);
  CHECK(file.file->SetLength(0) == X_STATUS_NOT_SUPPORTED);
  CHECK(file.file->SetLength(512) == X_STATUS_NOT_SUPPORTED);
  CHECK(file.file->Flush() == X_STATUS_NOT_SUPPORTED);
  CHECK_FALSE(file.file->DescribeDeviceControl(kFolderVolumeDriveGeometry));
  CHECK(file.file->ReadSync({}, 0, &count) == X_STATUS_SUCCESS);
  CHECK(count == 0);
  CHECK(file.file->WriteSync({}, 0, &count) == X_STATUS_SUCCESS);
  CHECK(count == 0);
  CHECK_FALSE(fs.UnregisterDevice("\\Device\\Harddisk0"));
  CHECK(fs.RetireDevice("\\Device\\Harddisk0"));
  auto* retired = fs.ResolvePath("\\Device\\Harddisk0\\Cache0");
  REQUIRE(retired);
  FileOwner rejected;
  CHECK(retired->Open(FileAccess::kFileReadData, &rejected.file) ==
        X_STATUS(0xC000009D));
  CHECK(rejected.file == nullptr);
  fs.Clear();
  CHECK(file.file->ReadSync(bytes, 0, &count) == X_STATUS(0xC000009D));
  CHECK(count == 0);
}
}  // namespace xe::vfs::test

#if !XE_PLATFORM_WIN32
// Friend access wraps only the already-open real policy handle. No path,
// identity, reservation, Kernel object or space authority is synthesized.
namespace xe::vfs {
struct OverwriteFlushFaults {
  unsigned resize_failures = 0, flush_failures = 0;
  std::error_code flush_error = std::make_error_code(std::errc::io_error);
  bool publish_before_resize_failure = false;
  size_t partial_write_bytes = 0;
  unsigned query_failures = 0, query_failures_after_resize_failure = 0;
  unsigned query_calls = 0, read_calls = 0, write_calls = 0;
  unsigned resize_calls = 0, flush_calls = 0, length_flush_calls = 0;
  std::optional<filesystem::FileLengthFlushResult> last;
};
class HostPathFileTestAccess {
 public:
  static void Wrap(File& file, std::shared_ptr<OverwriteFlushFaults> faults) {
    auto& host = static_cast<HostPathFile&>(file);
    host.file_handle_ =
        std::make_unique<Observed>(std::move(host.file_handle_),
                                   std::move(faults));
  }
 private:
  class Observed final : public filesystem::FileHandle {
   public:
    Observed(std::unique_ptr<filesystem::FileHandle> real,
             std::shared_ptr<OverwriteFlushFaults> faults)
        : FileHandle(real->path()), real_(std::move(real)),
          faults_(std::move(faults)) {}
    intptr_t native_handle() const override { return real_->native_handle(); }
    std::shared_ptr<const filesystem::HostStorageFileLease> storage_lease()
        const override { return real_->storage_lease(); }
    std::optional<filesystem::FileHandleInfo> QueryInfo(
        std::error_code& error) const override {
      ++faults_->query_calls;
      if (faults_->query_failures) {
        --faults_->query_failures;
        error = std::make_error_code(std::errc::io_error);
        return std::nullopt;
      }
      return real_->QueryInfo(error);
    }
    std::error_code LastError() const override { return real_->LastError(); }
    filesystem::FileIoResult ReadResult(
        size_t offset, std::span<uint8_t> bytes) override {
      ++faults_->read_calls;
      return real_->ReadResult(offset, bytes);
    }
    filesystem::FileIoResult WriteResult(
        size_t offset, std::span<const uint8_t> bytes) override {
      ++faults_->write_calls;
      if (faults_->partial_write_bytes) {
        const auto length = std::min(bytes.size(), faults_->partial_write_bytes);
        faults_->partial_write_bytes = 0;
        const auto actual = real_->WriteResult(offset, bytes.first(length));
        return actual.error ? actual
                            : filesystem::FileIoResult{
                                  std::make_error_code(std::errc::no_space_on_device),
                                  actual.bytes};
      }
      return real_->WriteResult(offset, bytes);
    }
    bool Read(size_t offset, void* data, size_t bytes,
              size_t* count) override {
      return real_->Read(offset, data, bytes, count);
    }
    bool Write(size_t offset, const void* data, size_t bytes,
               size_t* count) override {
      return real_->Write(offset, data, bytes, count);
    }
    std::error_code ResizeResult(size_t length) override {
      ++faults_->resize_calls;
      if (faults_->resize_failures) {
        --faults_->resize_failures;
        if (faults_->publish_before_resize_failure) {
          if (const auto error = real_->ResizeResult(length)) return error;
        }
        faults_->query_failures += faults_->query_failures_after_resize_failure;
        return std::make_error_code(std::errc::io_error);
      }
      return real_->ResizeResult(length);
    }
    filesystem::FileLengthFlushResult FlushLengthResult(
        size_t length) override {
      ++faults_->length_flush_calls;
      // Exercise the production-used typed Query/Resize/Flush helper at the
      // existing virtual backend seam. Its individual real Policy operations
      // retain validation/admission; normal cases below use Policy's atomic
      // single-guard override without this wrapper.
      auto result = FileHandle::FlushLengthResult(length);
      faults_->last = result;
      return result;
    }
    std::error_code FlushResult() override {
      ++faults_->flush_calls;
      if (faults_->flush_failures) {
        --faults_->flush_failures;
        return faults_->flush_error;
      }
      return real_->FlushResult();
    }
    bool SetLength(size_t length) override {
      return !ResizeResult(length);
    }
    bool SetTimes(uint64_t create, uint64_t access, uint64_t write) override {
      return real_->SetTimes(create, access, write);
    }
    void Flush() override { (void)FlushResult(); }
   private:
    std::unique_ptr<filesystem::FileHandle> real_;
    std::shared_ptr<OverwriteFlushFaults> faults_;
  };
};
}  // namespace xe::vfs
namespace xe::vfs::test {
namespace {
struct OverwriteFlushFixture {
  TemporaryDirectory directory;
  uint64_t available = filesystem::kHostStorageReserve + (4ull << 20);
  std::shared_ptr<filesystem::HostStorageRoot> source;
  VirtualFileSystem fs;
  HostPathEntry* entry = nullptr;
  FileOwner file;  // Closed before fs/source/available are destroyed.
  static constexpr const char* kMount = "\\Device\\OverwriteCommit";
  OverwriteFlushFixture() {
    REQUIRE(std::filesystem::create_directory(directory.path / "volume"));
    std::ofstream(directory.path / "volume" / "file") << "abcdefgh";
    std::error_code error;
    auto space = filesystem::HostStorageSpace::ForPath(
        directory.path / "volume", filesystem::kHostStorageReserve, error,
        [this](const auto&, auto& ec) {
          ec.clear();
          return std::filesystem::space_info{64ull << 30, available, available};
        });
    REQUIRE(space);
    source = filesystem::HostStorageRoot::Open(
        directory.path / "volume", false, error,
        filesystem::kHostStorageReserve, std::move(space));
    REQUIRE(source);
    auto device = std::make_unique<HostPathDevice>(
        kMount, directory.path / "volume", false, false, "FATX", source);
    REQUIRE(device->Initialize());
    entry = static_cast<HostPathEntry*>(device->ResolvePath("file"));
    REQUIRE(entry);
    REQUIRE(fs.RegisterDevice(std::move(device)));
    REQUIRE(entry->Overwrite());
    REQUIRE(entry->Open(FileAccess::kFileReadData | FileAccess::kFileWriteData,
                        &file.file) == X_STATUS_SUCCESS);
  }
  void WriteByte(size_t offset = 0) {
    const std::array<uint8_t, 1> value{'x'};
    size_t count = 77;
    REQUIRE(file.file->WriteSync(value, offset, &count) == X_STATUS_SUCCESS);
    REQUIRE(count == value.size());
  }
  std::vector<uint8_t> ReadGuest(size_t size) {
    std::vector<uint8_t> bytes(size, 0xA5);
    size_t count = 77;
    REQUIRE(file.file->ReadSync(bytes, 0, &count) == X_STATUS_SUCCESS);
    REQUIRE(count == size);
    return bytes;
  }
  uint64_t PhysicalSize() const {
    return std::filesystem::file_size(directory.path / "volume" / "file");
  }
  void Close() {
    file.file->Destroy();
    file.file = nullptr;
  }
};
}  // namespace

TEST_CASE("Flush discards the retained tail for every later shrink and growth",
          "[vfs][overwrite_flush]") {
  OverwriteFlushFixture f;
  f.WriteByte();
  REQUIRE(f.file.file->SetLength(3) == X_STATUS_SUCCESS);
  REQUIRE((f.ReadGuest(3) == std::vector<uint8_t>{'x', 'b', 'c'}));
  REQUIRE(f.PhysicalSize() == 8);
  REQUIRE(f.file.file->Flush() == X_STATUS_SUCCESS);
  REQUIRE(f.PhysicalSize() == 3);
  REQUIRE(f.file.file->SetLength(1) == X_STATUS_SUCCESS);
  REQUIRE(f.PhysicalSize() == 1);
  REQUIRE(f.file.file->SetLength(3) == X_STATUS_SUCCESS);
  REQUIRE((f.ReadGuest(3) == std::vector<uint8_t>{'x', 0, 0}));
  REQUIRE(f.file.file->Flush() == X_STATUS_SUCCESS);
  std::error_code error;
  REQUIRE_FALSE(f.source->OpenFile("file", FileAccess::kFileReadData, error));
  REQUIRE(error == std::errc::device_or_resource_busy);
  f.Close();
  std::vector<uint8_t> bytes;
  REQUIRE_FALSE(f.source->ReadFile("file", 8, bytes));
  REQUIRE((bytes == std::vector<uint8_t>{'x', 0, 0}));
}

TEST_CASE("Failed fsync preserves published EOF and admits later real growth",
          "[vfs][overwrite_flush]") {
  OverwriteFlushFixture f;
  auto faults = std::make_shared<OverwriteFlushFaults>();
  faults->flush_failures = 1;
  HostPathFileTestAccess::Wrap(*f.file.file, faults);
  f.WriteByte();
  REQUIRE(f.file.file->SetLength(3) == X_STATUS_SUCCESS);
  REQUIRE(f.file.file->Flush() == X_STATUS_UNSUCCESSFUL);
  REQUIRE(f.PhysicalSize() == 3);
  REQUIRE(faults->last);
  REQUIRE(faults->last->published_length == 3);
  REQUIRE_FALSE(faults->last->durable);
  REQUIRE(faults->last->error == std::errc::io_error);
  f.available = filesystem::kHostStorageReserve;
  REQUIRE(f.file.file->SetLength(8) == X_STATUS_DISK_FULL);
  const std::array<uint8_t, 1> value{'z'};
  size_t count = 77;
  REQUIRE(f.file.file->WriteSync(value, 7, &count) == X_STATUS_DISK_FULL);
  REQUIRE(count == 0);
  REQUIRE(f.PhysicalSize() == 3);
  REQUIRE_FALSE(f.fs.UnregisterDevice(f.kMount));
  REQUIRE(f.file.file->SetLength(1) == X_STATUS_SUCCESS);
  REQUIRE(f.PhysicalSize() == 1);
  f.available += 1ull << 20;
  REQUIRE(f.file.file->SetLength(5) == X_STATUS_SUCCESS);
  REQUIRE((f.ReadGuest(5) == std::vector<uint8_t>{'x', 0, 0, 0, 0}));
  // The active owner publishes the new EOF immediately; retry sync never
  // re-trims to the earlier explicitly flushed length.
  REQUIRE(f.entry->size() == 5);
  f.Close();
  REQUIRE(f.PhysicalSize() == 5);
  REQUIRE(faults->flush_calls == 2);
  REQUIRE(faults->length_flush_calls == 1);
  std::vector<uint8_t> bytes;
  REQUIRE_FALSE(f.source->ReadFile("file", 8, bytes));
  REQUIRE((bytes == std::vector<uint8_t>{'x', 0, 0, 0, 0}));
}

TEST_CASE("Failed durability at close retains the real handle for reopen retry",
          "[vfs][overwrite_flush]") {
  OverwriteFlushFixture f;
  auto faults = std::make_shared<OverwriteFlushFaults>();
  faults->flush_failures = 2;
  HostPathFileTestAccess::Wrap(*f.file.file, faults);
  f.WriteByte();
  REQUIRE(f.file.file->Flush() == X_STATUS_UNSUCCESSFUL);
  REQUIRE(f.PhysicalSize() == 1);
  REQUIRE(f.file.file->SetLength(4) == X_STATUS_SUCCESS);
  f.Close();  // Second fsync fails: Entry owns the same real reserved handle.
  REQUIRE(f.PhysicalSize() == 4);
  REQUIRE(f.source->open_file_count() == 1);
  std::error_code error;
  REQUIRE_FALSE(f.source->OpenFile("file", FileAccess::kFileReadData, error));
  REQUIRE(error == std::errc::device_or_resource_busy);
  REQUIRE(f.entry->Open(FileAccess::kFileReadData | FileAccess::kFileWriteData,
                        &f.file.file) == X_STATUS_SUCCESS);
  REQUIRE(faults->flush_calls == 3);
  REQUIRE(faults->length_flush_calls == 1);
  REQUIRE(f.PhysicalSize() == 4);
  REQUIRE((f.ReadGuest(4) == std::vector<uint8_t>{'x', 0, 0, 0}));
  f.Close();
  REQUIRE(f.source->open_file_count() == 0);
}

TEST_CASE("Repeated overwrite failure retains the exact pending handle",
          "[vfs][overwrite_flush]") {
  OverwriteFlushFixture f;
  auto faults = std::make_shared<OverwriteFlushFaults>();
  faults->flush_failures = 3;
  HostPathFileTestAccess::Wrap(*f.file.file, faults);
  f.WriteByte();
  REQUIRE(f.file.file->Flush() == X_STATUS_UNSUCCESSFUL);
  REQUIRE(f.file.file->SetLength(4) == X_STATUS_SUCCESS);
  f.Close();
  REQUIRE(f.source->open_file_count() == 1);
  REQUIRE_FALSE(f.entry->Overwrite());  // Third fsync fails; preserve owner.
  REQUIRE(faults->flush_calls == 3);
  REQUIRE(f.source->open_file_count() == 1);
  REQUIRE(f.PhysicalSize() == 4);
  std::error_code error;
  REQUIRE_FALSE(f.source->OpenFile("file", FileAccess::kFileReadData, error));
  REQUIRE(error == std::errc::device_or_resource_busy);
  REQUIRE(f.entry->Open(FileAccess::kFileReadData | FileAccess::kFileWriteData,
                        &f.file.file) == X_STATUS_SUCCESS);
  REQUIRE(faults->flush_calls == 4);
  REQUIRE(faults->length_flush_calls == 1);
  REQUIRE((f.ReadGuest(4) == std::vector<uint8_t>{'x', 0, 0, 0}));
  f.Close();
  REQUIRE(f.source->open_file_count() == 0);
}

TEST_CASE("New overwrite retries durability and aborts to actual reserved size",
          "[vfs][overwrite_flush]") {
  OverwriteFlushFixture f;
  auto faults = std::make_shared<OverwriteFlushFaults>();
  faults->flush_failures = 2;
  HostPathFileTestAccess::Wrap(*f.file.file, faults);
  f.WriteByte();
  REQUIRE(f.file.file->Flush() == X_STATUS_UNSUCCESSFUL);
  REQUIRE(f.file.file->SetLength(4) == X_STATUS_SUCCESS);
  f.Close();
  REQUIRE(f.source->open_file_count() == 1);
  REQUIRE(f.entry->Overwrite());  // Retry old fsync, then reserve a new cycle.
  REQUIRE(faults->flush_calls == 3);
  REQUIRE(faults->length_flush_calls == 1);
  REQUIRE(f.source->open_file_count() == 1);
  REQUIRE(f.entry->size() == 0);
  REQUIRE_FALSE(f.entry->Overwrite());  // Unconsumed reservation is preserved.
  REQUIRE(f.source->open_file_count() == 1);
  REQUIRE(f.entry->Open(FileAccess::kFileWriteData, nullptr) ==
          X_STATUS_INVALID_PARAMETER);
  REQUIRE(f.entry->size() == 4);
  REQUIRE(f.PhysicalSize() == 4);
  REQUIRE(f.source->open_file_count() == 0);
  std::vector<uint8_t> bytes;
  REQUIRE_FALSE(f.source->ReadFile("file", 8, bytes));
  REQUIRE((bytes == std::vector<uint8_t>{'x', 0, 0, 0}));
  REQUIRE(f.entry->Overwrite());
  REQUIRE(f.entry->Open(FileAccess::kFileReadData | FileAccess::kFileWriteData,
                        &f.file.file) == X_STATUS_SUCCESS);
  f.WriteByte();
  REQUIRE(f.file.file->Flush() == X_STATUS_SUCCESS);
  f.Close();
  REQUIRE(f.PhysicalSize() == 1);
  REQUIRE(f.source->open_file_count() == 0);
}

TEST_CASE("VFS overwrite permission failure preserves original inode and bytes",
          "[vfs][overwrite_flush][permissions]") {
  bool privileged = geteuid() == 0;
#if XE_PLATFORM_LINUX
  std::ifstream capabilities("/proc/self/status");
  std::string line;
  bool found_capabilities = false;
  while (std::getline(capabilities, line)) {
    if (line.starts_with("CapEff:")) {
      found_capabilities = true;
      privileged = privileged || (std::stoull(line.substr(7), nullptr, 16) & 2);
      break;
    }
  }
  if (!found_capabilities) {
    WARN("UNVERIFIED: permission case requires readable effective capabilities");
    return;
  }
#endif
  if (privileged) {
    WARN("UNVERIFIED: permission case requires nonroot without CAP_DAC_OVERRIDE");
    return;
  }
  for (bool at_floor : {false, true}) {
    TemporaryDirectory directory;
    const auto path = directory.path / "original";
    std::ofstream(path) << "preserve-original";
    REQUIRE(chmod(path.c_str(), 0640) == 0);
    struct stat original{};
    REQUIRE(stat(path.c_str(), &original) == 0);
    std::error_code error;
    const uint64_t available = filesystem::kHostStorageReserve +
                               (at_floor ? 0 : (4ull << 20));
    auto space = filesystem::HostStorageSpace::ForPath(
        directory.path, filesystem::kHostStorageReserve, error,
        [available](const auto&, auto& ec) {
          ec.clear();
          return std::filesystem::space_info{64ull << 30, available, available};
        });
    REQUIRE(space);
    auto source = filesystem::HostStorageRoot::Open(
        directory.path, false, error, filesystem::kHostStorageReserve, space);
    REQUIRE(source);
    VirtualFileSystem fs;
    auto device = std::make_unique<HostPathDevice>(
        "\\Device\\PermissionOverwrite", directory.path, false, false,
        "FATX", source);
    REQUIRE(device->Initialize());
    auto* entry = device->ResolvePath("original");
    REQUIRE(entry);
    REQUIRE(fs.RegisterDevice(std::move(device)));
    struct RestorePermissions {
      std::filesystem::path path;
      ~RestorePermissions() { chmod(path.c_str(), 0640); }
    } restore{path};
    REQUIRE(chmod(path.c_str(), 0000) == 0);
    for (const auto disposition : {FileDisposition::kOverwrite,
                                    FileDisposition::kOverwriteIf}) {
      CAPTURE(at_floor, int(disposition));
      FileOwner file;
      FileAction action = FileAction::kCreated;
      REQUIRE(fs.OpenFile(nullptr,
                          "\\Device\\PermissionOverwrite\\original",
                          disposition, FileAccess::kFileReadData |
                                           FileAccess::kFileWriteData,
                          false, true, &file.file, &action) ==
              X_STATUS_ACCESS_DENIED);
      REQUIRE_FALSE(file.file);
      REQUIRE(action == FileAction::kExists);
      struct stat after{};
      REQUIRE(stat(path.c_str(), &after) == 0);
      REQUIRE(after.st_dev == original.st_dev);
      REQUIRE(after.st_ino == original.st_ino);
      REQUIRE(after.st_size == original.st_size);
      REQUIRE((after.st_mode & 0777) == 0);
      REQUIRE(fs.ResolvePath(
                  "\\Device\\PermissionOverwrite\\original") == entry);
      REQUIRE(source->open_file_count() == 0);
    }
    REQUIRE(chmod(path.c_str(), 0640) == 0);
    std::vector<uint8_t> bytes;
    REQUIRE_FALSE(source->ReadFile("original", 128, bytes));
    REQUIRE(std::string(bytes.begin(), bytes.end()) == "preserve-original");
  }
}

TEST_CASE("VFS supported overwrite retry preserves actual backend failure",
          "[vfs][overwrite_flush]") {
  for (const auto error_kind : {std::errc::io_error,
                                 std::errc::operation_not_supported}) {
    const auto expected = error_kind == std::errc::io_error
                              ? X_STATUS_UNSUCCESSFUL : X_STATUS_NOT_SUPPORTED;
    for (const auto disposition : {FileDisposition::kOverwrite,
                                    FileDisposition::kOverwriteIf}) {
      CAPTURE(int(error_kind), int(disposition));
      OverwriteFlushFixture f;
      auto faults = std::make_shared<OverwriteFlushFaults>();
      faults->flush_failures = 3;
      faults->flush_error = std::make_error_code(error_kind);
      HostPathFileTestAccess::Wrap(*f.file.file, faults);
      f.WriteByte();
      REQUIRE(f.file.file->Flush() == expected);
      REQUIRE(f.file.file->SetLength(4) == X_STATUS_SUCCESS);
      f.Close();
      struct stat before{};
      const auto path = f.directory.path / "volume" / "file";
      REQUIRE(stat(path.c_str(), &before) == 0);
      FileAction action = FileAction::kCreated;
      const auto name = std::string(f.kMount) + "\\file";
      REQUIRE(f.fs.OpenFile(nullptr, name, disposition,
                            FileAccess::kFileReadData | FileAccess::kFileWriteData,
                            false, true, &f.file.file, &action) ==
              expected);
      REQUIRE_FALSE(f.file.file);
      REQUIRE(action == FileAction::kExists);
      REQUIRE(faults->flush_calls == 3);
      REQUIRE(f.source->open_file_count() == 1);
      struct stat after{};
      REQUIRE(stat(path.c_str(), &after) == 0);
      REQUIRE(after.st_ino == before.st_ino);
      REQUIRE(after.st_dev == before.st_dev);
      REQUIRE(after.st_size == before.st_size);
      REQUIRE((after.st_mode & 0777) == (before.st_mode & 0777));
      REQUIRE(f.fs.ResolvePath(name) == f.entry);
      REQUIRE(f.fs.OpenFile(nullptr, name, FileDisposition::kOpen,
                            FileAccess::kFileReadData | FileAccess::kFileWriteData,
                            false, true, &f.file.file, &action) == X_STATUS_SUCCESS);
      REQUIRE(faults->flush_calls == 4);
      REQUIRE((f.ReadGuest(4) == std::vector<uint8_t>{'x', 0, 0, 0}));
      f.Close();
      REQUIRE(f.source->open_file_count() == 0);
    }
  }
}

TEST_CASE("Owned postflush EOF serves metadata read extent and append protocol",
          "[vfs][overwrite_flush][entry_consumers]") {
  for (bool fail_first_sync : {false, true}) {
    CAPTURE(fail_first_sync);
    OverwriteFlushFixture f;
    auto faults = std::make_shared<OverwriteFlushFaults>();
    faults->flush_failures = fail_first_sync ? 1 : 0;
    HostPathFileTestAccess::Wrap(*f.file.file, faults);
    f.WriteByte();
    REQUIRE(f.file.file->Flush() ==
            (fail_first_sync ? X_STATUS_UNSUCCESSFUL : X_STATUS_SUCCESS));
    REQUIRE(f.file.file->SetLength(4) == X_STATUS_SUCCESS);
    REQUIRE(f.entry->size() == 4);
    REQUIRE(f.entry->allocation_size() == 512);
    // This is the actual Entry update/getter protocol used by XFile append
    // and native info consumers, without constructing a fake Kernel/XFile.
    f.entry->update();
    const auto append_offset = f.entry->size();
    REQUIRE(append_offset == 4);
    std::array<uint8_t, 1> edge{0xA5};
    size_t count = 77;
    REQUIRE(f.file.file->ReadSync(edge, append_offset - 1, &count) ==
            X_STATUS_SUCCESS);
    REQUIRE(count == 1);
    REQUIRE(edge[0] == 0);
    const std::array<uint8_t, 1> append{'a'};
    REQUIRE(f.file.file->WriteSync(append, append_offset, &count) ==
            X_STATUS_SUCCESS);
    REQUIRE(count == 1);
    REQUIRE(f.entry->size() == 5);
    REQUIRE(f.entry->allocation_size() == 512);
    REQUIRE(f.file.file->SetLength(513) == X_STATUS_SUCCESS);
    REQUIRE(f.entry->size() == 513);
    REQUIRE(f.entry->allocation_size() == 1024);
    f.entry->update();
    REQUIRE(f.entry->size() == 513);
    const std::array<uint8_t, 3> partial{'p', 'q', 'r'};
    faults->partial_write_bytes = 2;
    REQUIRE(f.file.file->WriteSync(partial, f.entry->size(), &count) ==
            X_STATUS_DISK_FULL);
    REQUIRE(count == 2);
    REQUIRE(f.entry->size() == 515);
    REQUIRE(f.entry->allocation_size() == 1024);
    REQUIRE(f.PhysicalSize() == 515);
    REQUIRE(f.file.file->Flush() == X_STATUS_SUCCESS);
    f.Close();
    f.entry->update();
    REQUIRE(f.entry->size() == 515);
    REQUIRE(f.entry->allocation_size() == 1024);
    std::vector<uint8_t> stored;
    REQUIRE_FALSE(f.source->ReadFile("file", 1024, stored));
    REQUIRE(stored.size() == 515);
    REQUIRE(stored[0] == 'x');
    REQUIRE(stored[4] == 'a');
    REQUIRE(stored[513] == 'p');
    REQUIRE(stored[514] == 'q');
  }
}

TEST_CASE("Unknown postcommit EOF rejects data until explicit flush reconciles",
          "[vfs][overwrite_flush][unknown_eof]") {
  OverwriteFlushFixture f;
  auto faults = std::make_shared<OverwriteFlushFaults>();
  HostPathFileTestAccess::Wrap(*f.file.file, faults);
  f.WriteByte();
  REQUIRE(f.file.file->Flush() == X_STATUS_SUCCESS);
  REQUIRE(f.entry->size() == 1);
  faults->resize_failures = 1;
  faults->publish_before_resize_failure = true;
  faults->query_failures_after_resize_failure = 2;
  REQUIRE(f.file.file->SetLength(4) == X_STATUS_UNSUCCESSFUL);
  REQUIRE(f.PhysicalSize() == 4);
  REQUIRE(f.entry->size() == 1);  // Explicitly last known, not reconciled EOF.
  f.entry->update();
  const auto stale_append_offset = f.entry->size();
  REQUIRE(stale_append_offset == 1);
  const auto reads = faults->read_calls;
  const auto writes = faults->write_calls;
  const auto resizes = faults->resize_calls;
  const auto queries = faults->query_calls;
  const auto syncs = faults->flush_calls;
  std::array<uint8_t, 1> output{0xA5};
  size_t count = 77;
  REQUIRE(f.file.file->ReadSync(output, 0, &count) == X_STATUS_UNSUCCESSFUL);
  REQUIRE(count == 0);
  REQUIRE(output[0] == 0xA5);
  const std::array<uint8_t, 1> append{'z'};
  REQUIRE(f.file.file->WriteSync(append, stale_append_offset, &count) ==
          X_STATUS_UNSUCCESSFUL);
  REQUIRE(count == 0);
  REQUIRE(f.file.file->SetLength(2) == X_STATUS_UNSUCCESSFUL);
  REQUIRE(faults->read_calls == reads);
  REQUIRE(faults->write_calls == writes);
  REQUIRE(faults->resize_calls == resizes);
  REQUIRE(faults->query_calls == queries);  // No implicit data-op recovery.
  REQUIRE(f.PhysicalSize() == 4);
  REQUIRE(f.file.file->Flush() == X_STATUS_UNSUCCESSFUL);
  REQUIRE(faults->flush_calls == syncs);  // Failed observation cannot fsync/ack.
  REQUIRE(f.entry->size() == 1);
  std::error_code error;
  REQUIRE_FALSE(f.source->OpenFile("file", FileAccess::kFileReadData, error));
  REQUIRE(error == std::errc::device_or_resource_busy);
  REQUIRE(f.file.file->Flush() == X_STATUS_SUCCESS);
  REQUIRE(faults->flush_calls == syncs + 1);
  REQUIRE(f.entry->size() == 4);
  REQUIRE(f.entry->allocation_size() == 512);
  REQUIRE((f.ReadGuest(4) == std::vector<uint8_t>{'x', 0, 0, 0}));
  f.entry->update();
  const auto recovered_append_offset = f.entry->size();
  REQUIRE(recovered_append_offset == 4);
  f.WriteByte(recovered_append_offset);
  REQUIRE(f.entry->size() == 5);
  f.Close();
  REQUIRE(f.PhysicalSize() == 5);
  std::vector<uint8_t> stored;
  REQUIRE_FALSE(f.source->ReadFile("file", 16, stored));
  REQUIRE((stored == std::vector<uint8_t>{'x', 0, 0, 0, 'x'}));
}

TEST_CASE("Unknown EOF retains its handle through failed close and reopen query",
          "[vfs][overwrite_flush][unknown_eof]") {
  OverwriteFlushFixture f;
  auto faults = std::make_shared<OverwriteFlushFaults>();
  HostPathFileTestAccess::Wrap(*f.file.file, faults);
  f.WriteByte();
  REQUIRE(f.file.file->Flush() == X_STATUS_SUCCESS);
  const auto syncs = faults->flush_calls;
  faults->resize_failures = 1;
  faults->publish_before_resize_failure = true;
  faults->query_failures_after_resize_failure = 3;
  REQUIRE(f.file.file->SetLength(4) == X_STATUS_UNSUCCESSFUL);
  REQUIRE(f.PhysicalSize() == 4);
  f.Close();
  REQUIRE(faults->flush_calls == syncs);
  REQUIRE(f.source->open_file_count() == 1);
  REQUIRE(f.entry->Open(FileAccess::kFileReadData | FileAccess::kFileWriteData,
                        &f.file.file) == X_STATUS_UNSUCCESSFUL);
  REQUIRE_FALSE(f.file.file);
  REQUIRE(faults->flush_calls == syncs);
  REQUIRE(f.source->open_file_count() == 1);
  REQUIRE(f.PhysicalSize() == 4);
  REQUIRE(f.entry->Open(FileAccess::kFileReadData | FileAccess::kFileWriteData,
                        &f.file.file) == X_STATUS_SUCCESS);
  REQUIRE(faults->flush_calls == syncs + 1);
  REQUIRE(f.entry->size() == 4);
  REQUIRE((f.ReadGuest(4) == std::vector<uint8_t>{'x', 0, 0, 0}));
  f.Close();
  REQUIRE(f.source->open_file_count() == 0);
  REQUIRE(f.PhysicalSize() == 4);
}

TEST_CASE("A failed truncate never acknowledges fsync or releases its lease",
          "[vfs][overwrite_flush]") {
  OverwriteFlushFixture f;
  auto faults = std::make_shared<OverwriteFlushFaults>();
  faults->resize_failures = 1;
  HostPathFileTestAccess::Wrap(*f.file.file, faults);
  f.WriteByte();
  REQUIRE(f.file.file->Flush() == X_STATUS_UNSUCCESSFUL);
  REQUIRE(f.PhysicalSize() == 8);
  REQUIRE(faults->last);
  REQUIRE_FALSE(faults->last->published_length);
  REQUIRE_FALSE(faults->last->durable);
  REQUIRE(faults->flush_calls == 0);
  REQUIRE((f.ReadGuest(1) == std::vector<uint8_t>{'x'}));
  std::error_code error;
  REQUIRE_FALSE(f.source->OpenFile("file", FileAccess::kFileReadData, error));
  REQUIRE(error == std::errc::device_or_resource_busy);
  REQUIRE(f.file.file->Flush() == X_STATUS_SUCCESS);
  REQUIRE(f.PhysicalSize() == 1);
  REQUIRE(faults->resize_calls == 2);
  REQUIRE(faults->flush_calls == 1);
  REQUIRE(faults->last->durable);
}

TEST_CASE("A truncate error after publishing EOF cannot roll back tail mode",
          "[vfs][overwrite_flush]") {
  OverwriteFlushFixture f;
  auto faults = std::make_shared<OverwriteFlushFaults>();
  faults->resize_failures = 1;
  faults->publish_before_resize_failure = true;
  HostPathFileTestAccess::Wrap(*f.file.file, faults);
  f.WriteByte();
  REQUIRE(f.file.file->Flush() == X_STATUS_UNSUCCESSFUL);
  REQUIRE(f.PhysicalSize() == 1);
  REQUIRE(faults->last);
  REQUIRE(faults->last->published_length == 1);
  REQUIRE_FALSE(faults->last->durable);
  REQUIRE(faults->flush_calls == 0);
  f.available = filesystem::kHostStorageReserve;
  REQUIRE(f.file.file->SetLength(8) == X_STATUS_DISK_FULL);
  REQUIRE(f.file.file->Flush() == X_STATUS_SUCCESS);
  REQUIRE(faults->length_flush_calls == 1);
  REQUIRE(faults->flush_calls == 1);
}

TEST_CASE("Zero-length overwrite flush can reclaim space at the reserve floor",
          "[vfs][overwrite_flush]") {
  OverwriteFlushFixture f;
  f.available = filesystem::kHostStorageReserve;
  REQUIRE(f.file.file->Flush() == X_STATUS_SUCCESS);
  REQUIRE(f.PhysicalSize() == 0);
  REQUIRE(f.file.file->SetLength(1) == X_STATUS_DISK_FULL);
  REQUIRE(f.PhysicalSize() == 0);
  f.Close();
  REQUIRE(f.source->Observe().reserve_bytes == filesystem::kHostStorageReserve);
  REQUIRE(f.source->Observe().usable_bytes == 0);
}

TEST_CASE("Replaced root generation cannot redirect overwrite truncation",
          "[vfs][overwrite_flush]") {
  OverwriteFlushFixture f;
  auto faults = std::make_shared<OverwriteFlushFaults>();
  HostPathFileTestAccess::Wrap(*f.file.file, faults);
  f.WriteByte();
  std::filesystem::rename(f.directory.path / "volume",
                           f.directory.path / "retained");
  REQUIRE(std::filesystem::create_directory(f.directory.path / "volume"));
  std::ofstream(f.directory.path / "volume" / "file") << "replacement";
  REQUIRE(f.file.file->Flush() == X_STATUS(0xC000009D));
  REQUIRE(faults->resize_calls == 0);
  REQUIRE(faults->flush_calls == 0);
  REQUIRE(std::filesystem::file_size(f.directory.path / "retained" / "file") == 8);
  REQUIRE(filesystem::ReadAllText(f.directory.path / "volume" / "file") ==
          "replacement");
  f.Close();
  f.fs.Clear();
  REQUIRE(f.source->open_file_count() == 0);
}

TEST_CASE("Retired source fails overwrite flush while the real file owns its tree",
          "[vfs][overwrite_flush]") {
  OverwriteFlushFixture f;
  f.WriteByte();
  const std::weak_ptr<Device> owner = f.file.file->retained_device();
  REQUIRE_FALSE(owner.expired());
  REQUIRE_FALSE(f.fs.UnregisterDevice(f.kMount));
  f.source->Retire();
  REQUIRE(f.file.file->Flush() == X_STATUS(0xC000009D));
  REQUIRE(f.PhysicalSize() == 8);
  f.fs.Clear();
  REQUIRE_FALSE(owner.expired());
  f.Close();
  REQUIRE(owner.expired());
  REQUIRE(f.source->open_file_count() == 0);
}
}  // namespace xe::vfs::test
#endif  // !XE_PLATFORM_WIN32
