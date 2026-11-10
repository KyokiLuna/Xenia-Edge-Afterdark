/**
 * Xenia : Xbox 360 Emulator Research Project
 * Released under the BSD license - see LICENSE in the root for details.
 */
#include "xenia/kernel/util/io_request_completion.h"
#include <array>
#include <chrono>
#include <cstring>
#include <fstream>
#include <random>
#include <string>
#include <vector>
#include "third_party/catch/include/catch.hpp"
#include "xenia/base/threading.h"
#include "xenia/kernel/util/guest_io_buffer.h"
#include "xenia/memory.h"
#include "xenia/vfs/devices/host_path_device.h"
#include "xenia/vfs/devices/host_path_entry.h"
#include "xenia/vfs/devices/host_path_file.h"
#include "xenia/vfs/folder_volume_control.h"

namespace xe::kernel::util::test {
namespace {
// Actual shared completion code plus real guest Memory and host event. This
// fixture deliberately has no fake Kernel/XThread and makes no claim about
// valid-Kernel APC/port delivery or scheduler cancellation execution.
struct CompletionFixture {
  static constexpr uint32_t kStatus = 0x9002B000;
  Memory memory;
  std::unique_ptr<threading::Event> completion =
      threading::Event::CreateManualResetEvent(false);
  std::vector<std::string> order;
  X_STATUS notified_status = X_STATUS_PENDING;
  uint32_t notified_count = UINT32_MAX;
  bool saw_published_before_notification = false;
  CompletionFixture() {
    REQUIRE(memory.Initialize());
    REQUIRE(completion);
    REQUIRE(memory.LookupHeap(kStatus)->AllocFixed(
        kStatus, 0x1000, 0x1000,
        kMemoryAllocationReserve | kMemoryAllocationCommit,
        kMemoryProtectRead | kMemoryProtectWrite));
    std::memset(memory.TranslateVirtual(kStatus), 0xA5, 0x1000);
  }
  X_STATUS Publish(X_STATUS status, uint32_t count) {
    order.push_back("publish");
    return WriteGuestIoStatus(memory, kStatus, status, count);
  }
  void Notify(X_STATUS status, uint32_t count) {
    order.push_back("notify");
    notified_status = status;
    notified_count = count;
    saw_published_before_notification =
        xe::load_and_swap<uint32_t>(memory.TranslateVirtual(kStatus)) ==
            status &&
        xe::load_and_swap<uint32_t>(memory.TranslateVirtual(kStatus + 4)) ==
            count;
    completion->Set();
  }
  template <typename Transfer>
  X_STATUS Run(
      Transfer transfer,
      InlineIoFailurePolicy policy = InlineIoFailurePolicy::kCompleteAdmitted,
      bool posted = false, bool cancelled = false) {
    return CompleteIoRequest(
        posted, cancelled, policy, transfer,
        [&](X_STATUS status, uint32_t count) { return Publish(status, count); },
        [&](X_STATUS status, uint32_t count) { Notify(status, count); });
  }
};
#if !XE_PLATFORM_WIN32
struct Directory {
  std::filesystem::path path =
      std::filesystem::temp_directory_path() /
      ("xenia-completion-" + std::to_string(std::random_device{}()));
  Directory() { REQUIRE(std::filesystem::create_directory(path)); }
  ~Directory() {
    std::error_code error;
    std::filesystem::remove_all(path, error);
  }
};
class FlushEio final : public filesystem::FileHandle {
 public:
  explicit FlushEio(uint32_t& calls)
      : FileHandle(std::filesystem::path()), calls_(calls) {}
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
  void Flush() override {}
  std::error_code FlushResult() override {
    ++calls_;
    return std::make_error_code(std::errc::io_error);
  }

 private:
  uint32_t& calls_;
};
struct FileOwner {
  vfs::File* file = nullptr;
  ~FileOwner() {
    if (file) {
      file->Destroy();
    }
  }
};
#endif
}  // namespace

#if !XE_PLATFORM_WIN32
TEST_CASE(
    "Admitted synchronous flush backend EIO publishes failure before signal",
    "[kernel][file_control][completion]") {
  CompletionFixture fixture;
  Directory directory;
  std::ofstream(directory.path / "file") << "retained";
  std::error_code error;
  auto source =
      filesystem::HostStorageRoot::Open(directory.path, false, error, 0);
  REQUIRE(source);
  vfs::HostPathDevice device("\\Device\\FlushEio", directory.path, false, false,
                             "FATX", source);
  REQUIRE(device.Initialize());
  auto* entry = static_cast<vfs::HostPathEntry*>(device.ResolvePath("file"));
  REQUIRE(entry);
  uint32_t flushes = 0;
  FileOwner file;
  file.file = new vfs::HostPathFile(filesystem::FileAccess::kFileWriteData,
                                    entry, std::make_unique<FlushEio>(flushes));
  REQUIRE(fixture.Run([&](uint32_t* count) {
    fixture.order.push_back("backend");
    *count = 0;
    return file.file->Flush();
  }) == X_STATUS_UNSUCCESSFUL);
  CHECK(flushes == 1);
  CHECK((fixture.order ==
         std::vector<std::string>{"backend", "publish", "notify"}));
  CHECK(fixture.notified_status == X_STATUS_UNSUCCESSFUL);
  CHECK(fixture.notified_count == 0);
  CHECK(fixture.saw_published_before_notification);
  CHECK(threading::Wait(fixture.completion.get(), false,
                        std::chrono::milliseconds(0)) ==
        threading::WaitResult::kSuccess);
}

TEST_CASE(
    "Admitted synchronous retired-volume geometry completes with no output",
    "[kernel][file_control][completion]") {
  CompletionFixture fixture;
  Directory directory;
  std::error_code error;
  auto source =
      filesystem::HostStorageRoot::Open(directory.path, false, error, 0);
  REQUIRE(source);
  vfs::HostPathDevice device("\\Device\\RetiredControl", directory.path, false,
                             false, "FATX", source, source,
                             vfs::HostPathDevice::ControlRole::kFolderVolume);
  REQUIRE(device.Initialize());
  FileOwner file;
  REQUIRE(device.ResolvePath("")->Open(filesystem::FileAccess::kFileReadData,
                                       &file.file) == X_STATUS_SUCCESS);
  REQUIRE(file.file->DescribeDeviceControl(vfs::kFolderVolumePartitionInfo));
  device
      .Retire();  // Backend generation changed after successful prevalidation.
  std::array<uint8_t, 16> output;
  output.fill(0xC7);
  const auto before = output;
  REQUIRE(fixture.Run([&](uint32_t* count) {
    return file.file->DeviceControl(vfs::kFolderVolumePartitionInfo, {}, output,
                                    count);
  }) == X_STATUS(0xC000009D));
  CHECK(output == before);
  CHECK(fixture.notified_status == X_STATUS(0xC000009D));
  CHECK(fixture.notified_count == 0);
  CHECK(fixture.saw_published_before_notification);
  CHECK(threading::Wait(fixture.completion.get(), false,
                        std::chrono::milliseconds(0)) ==
        threading::WaitResult::kSuccess);
}
#endif

TEST_CASE("Legacy inline failure policy remains separate from admitted errors",
          "[kernel][file_control][completion]") {
  CompletionFixture fixture;
  uint32_t calls = 0;
  CHECK(fixture.Run(
            [&](uint32_t* count) {
              ++calls;
              *count = 3;
              return X_STATUS_DISK_FULL;
            },
            InlineIoFailurePolicy::kRejectWithoutCompletion) ==
        X_STATUS_DISK_FULL);
  CHECK(calls == 1);
  CHECK(fixture.order.empty());
  CHECK(xe::load_and_swap<uint32_t>(fixture.memory.TranslateVirtual(
            CompletionFixture::kStatus)) == 0xA5A5A5A5);
  CHECK(threading::Wait(fixture.completion.get(), false,
                        std::chrono::milliseconds(0)) ==
        threading::WaitResult::kTimeout);
  // Posted legacy failures keep their real partial transfer count.
  CHECK(fixture.Run(
            [&](uint32_t* count) {
              ++calls;
              *count = 3;
              return X_STATUS_DISK_FULL;
            },
            InlineIoFailurePolicy::kRejectWithoutCompletion,
            true) == X_STATUS_DISK_FULL);
  CHECK(calls == 2);
  CHECK(fixture.notified_status == X_STATUS_DISK_FULL);
  CHECK(fixture.notified_count == 3);
  CHECK(fixture.saw_published_before_notification);
}

TEST_CASE("Posted cancellation and failed IOSB stores never fabricate success",
          "[kernel][file_control][completion]") {
  CompletionFixture fixture;
  uint32_t backend_calls = 0;
  SECTION("Cancelled posted request does not call its backend") {
    REQUIRE(fixture.Run(
                [&](uint32_t*) {
                  ++backend_calls;
                  return X_STATUS_SUCCESS;
                },
                InlineIoFailurePolicy::kCompleteAdmitted, true,
                true) == X_STATUS_CANCELLED);
    CHECK(backend_calls == 0);
    CHECK(fixture.notified_status == X_STATUS_CANCELLED);
    CHECK(fixture.notified_count == 0);
    CHECK(fixture.saw_published_before_notification);
  }
  SECTION(
      "Invalidated IOSB reports available-channel failure without writing it") {
    REQUIRE(
        fixture.memory.LookupHeap(CompletionFixture::kStatus)
            ->Protect(CompletionFixture::kStatus, 0x1000, kMemoryProtectRead));
    REQUIRE(fixture.Run([&](uint32_t* count) {
      ++backend_calls;
      *count = 16;
      return X_STATUS_SUCCESS;
    }) == X_STATUS_ACCESS_VIOLATION);
    CHECK(backend_calls == 1);
    CHECK(fixture.notified_status == X_STATUS_ACCESS_VIOLATION);
    CHECK(fixture.notified_count == 0);
    CHECK_FALSE(fixture.saw_published_before_notification);
    CHECK(xe::load_and_swap<uint32_t>(fixture.memory.TranslateVirtual(
              CompletionFixture::kStatus)) == 0xA5A5A5A5);
  }
}
}  // namespace xe::kernel::util::test
