/**
 * Xenia : Xbox 360 Emulator Research Project
 * Released under the BSD license - see LICENSE in the root for details.
 */
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>
#include <filesystem>
#include <initializer_list>
#include <memory>
#include <string>
#include <stdexcept>
#include <new>
#include <utility>
#include <cstring>
#include "third_party/catch/include/catch.hpp"
#include "xenia/base/cvar.h"
#include "xenia/base/memory.h"
#include "xenia/base/platform.h"
#include "xenia/base/host_storage.h"
#include "xenia/emulator.h"
#include "xenia/kernel/info/file.h"
#include "xenia/kernel/kernel.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/xfile.h"
#include "xenia/kernel/xiocompletion.h"
#include "xenia/vfs/devices/host_path_device.h"
#include "xenia/vfs/devices/host_path_file.h"
#include "xenia/vfs/devices/host_path_entry.h"
#include "xenia/kernel/util/host_call_outcome.h"
#include "xenia/kernel/util/guest_io_buffer.h"
#include "xenia/kernel/util/io_request_completion.h"
#include "xenia/vfs/devices/null_device.h"
#include "xenia/vfs/virtual_file_system.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_module.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_private.h"
#include "xenia/memory.h"

namespace xe::kernel {
class GuestSchedulerIoTestAccess {
 public:
  static void FailNext(GuestScheduler& scheduler, uint32_t point) {
    scheduler.io_admission_failure_point_.store(point, std::memory_order_relaxed);
  }
  static std::unique_lock<std::mutex> LockPool(GuestScheduler& scheduler) {
    return std::unique_lock<std::mutex>(scheduler.io_pool_lock_);
  }
};
}  // namespace xe::kernel

namespace xe::kernel::xboxkrnl::test {
TEST_CASE("File control registered ABI rejects absent handles without output",
          "[kernel][file_control][abi]") {
  REQUIRE(kernel_state() == nullptr);
  Memory memory;
  REQUIRE(memory.Initialize());
  constexpr uint32_t page = 0x90029000;
  REQUIRE(memory.LookupHeap(page)->AllocFixed(
      page, 0x1000, 0x1000, kMemoryAllocationReserve | kMemoryAllocationCommit,
      kMemoryProtectRead | kMemoryProtectWrite));
  std::memset(memory.TranslateVirtual(page), 0xA5, 0x1000);
  cpu::ExportResolver resolver;
  RegisterIoExports(&resolver, nullptr);
  XboxkrnlModule::RegisterExportTable(&resolver);
  const auto* control = resolver.GetExportByOrdinal("xboxkrnl.exe", 0xD9);
  const auto* flush = resolver.GetExportByOrdinal("xboxkrnl.exe", 0xDB);
  for (const auto* entry : {control, flush}) {
    REQUIRE(entry);
    REQUIRE(entry->function_data.trampoline);
    REQUIRE(entry->is_implemented());
    REQUIRE_FALSE(entry->tags & cpu::ExportTag::kStub);
    PPCContext context{};
    context.virtual_membase = memory.virtual_membase();
    context.r[1] = page + 0x800;
    for (uint32_t reg = 3; reg < 11; ++reg) {
      context.r[reg] = 0xAABBCCDD00000000ull | (page + reg * 16);
    }
    context.r[3] = 0xDEADBEEF;  // No Kernel/handle authority.
    // Real ninth and tenth arguments use low DWORDs of8-byte stack slots.
    xe::store_and_swap<uint32_t>(memory.TranslateVirtual(page + 0x854),
                                 page + 0x200);
    xe::store_and_swap<uint32_t>(memory.TranslateVirtual(page + 0x85C), 32);
    std::array<uint8_t, 0x1000> before;
    std::memcpy(before.data(), memory.TranslateVirtual(page), before.size());
    entry->function_data.trampoline(&context);
    CHECK(uint32_t(context.r[3]) == X_STATUS_INVALID_HANDLE);
    CHECK(std::memcmp(memory.TranslateVirtual(page), before.data(),
                      before.size()) == 0);
    {
      auto outer = global_critical_region::AcquireDirect();
      context.r[3] = 0xDEADBEEF;
      entry->function_data.trampoline(&context);
      CHECK(uint32_t(context.r[3]) == X_STATUS_INVALID_DEVICE_REQUEST);
      CHECK(global_critical_region::is_held_by_current_thread());
      CHECK(std::memcmp(memory.TranslateVirtual(page), before.data(),
                        before.size()) == 0);
    }
  }
}
}  // namespace xe::kernel::xboxkrnl::test

#if XE_PLATFORM_LINUX
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

DECLARE_string(cpu);
DECLARE_bool(debug);
DECLARE_string(system_readonly_source_root);
DECLARE_string(logged_profile_slot_0_xuid);
DECLARE_string(logged_profile_slot_1_xuid);
DECLARE_string(logged_profile_slot_2_xuid);
DECLARE_string(logged_profile_slot_3_xuid);

namespace xe::kernel::xboxkrnl::test {
namespace {
struct FileInfoSettings {
  std::string cpu = cvars::cpu;
  bool debug = cvars::debug;
  std::array<std::string*, 5> values{
      &cvars::system_readonly_source_root, &cvars::logged_profile_slot_0_xuid,
      &cvars::logged_profile_slot_1_xuid, &cvars::logged_profile_slot_2_xuid,
      &cvars::logged_profile_slot_3_xuid};
  std::array<std::string, 5> saved;
  FileInfoSettings() {
    for (size_t i = 0; i < values.size(); ++i) {
      saved[i] = *values[i];
      values[i]->clear();
    }
    cvars::cpu = "null";
    cvars::debug = false;
  }
  ~FileInfoSettings() {
    cvars::cpu.swap(cpu);
    cvars::debug = debug;
    for (size_t i = 0; i < values.size(); ++i) values[i]->swap(saved[i]);
  }
};
struct FileInfoScratch {
  std::filesystem::path path;
  FileInfoScratch() {
    static std::atomic<uint64_t> sequence{0};
    path = std::filesystem::temp_directory_path() /
           ("xenia-file-info-" +
            std::to_string(std::chrono::steady_clock::now()
                               .time_since_epoch().count()) +
            "-" + std::to_string(sequence.fetch_add(1)));
    REQUIRE(std::filesystem::create_directory(path));
  }
  ~FileInfoScratch() {
    std::error_code error;
    std::filesystem::remove_all(path, error);
  }
};
class FileInfoFixture {
 public:
  static constexpr uint32_t kPage = 0x9002A000;
  static constexpr uint32_t kIosb = kPage + 0x80;
  static constexpr uint32_t kInfo = kPage + 0x100;
  static constexpr const char* kMount = "\\Device\\FileInformationTest";
  static constexpr const char* kUnsupportedMount = "\\Device\\UnimplementedMount";
  const std::array<uint8_t, 6> original{'b', 'a', 'c', 'k', 'e', 'd'};
  FileInfoFixture() {
    REQUIRE(KernelState::shared() == nullptr);
    for (const auto* folder : {"content", "cache", "files"}) {
      REQUIRE(std::filesystem::create_directory(scratch_.path / folder));
    }
    emulator = std::make_unique<Emulator>(
        "", scratch_.path, scratch_.path / "content", scratch_.path / "cache");
    REQUIRE(emulator->Setup(nullptr, nullptr, false, {}, {}, {}) ==
            X_STATUS_SUCCESS);
    // Real Kernel/Memory/VFS/NullBackend; no guest PPC, native Task or TLS.
    REQUIRE(emulator->kernel_state());
    REQUIRE(emulator->memory()->LookupHeap(kPage)->AllocFixed(
        kPage, 0x1000, 0x1000,
        kMemoryAllocationReserve | kMemoryAllocationCommit,
        kMemoryProtectRead | kMemoryProtectWrite));
    std::error_code error;
    storage = filesystem::HostStorageRoot::Open(scratch_.path / "files", false,
                                                error);
    REQUIRE(storage);
    REQUIRE_FALSE(error);
    REQUIRE_FALSE(storage->WriteNewFile("sample.bin", original).error);
    auto device = std::make_unique<vfs::HostPathDevice>(
        kMount, storage->path(), false, false, "FATX", storage);
    REQUIRE(device->Initialize());
    REQUIRE(emulator->file_system()->RegisterDevice(std::move(device)));
    auto* entry = emulator->file_system()->ResolvePath(
        std::string(kMount) + "\\sample.bin");
    REQUIRE(entry);
    vfs::File* opened = nullptr;
    REQUIRE(entry->Open(filesystem::FileAccess::kGenericRead |
                            filesystem::FileAccess::kGenericWrite,
                        &opened) == X_STATUS_SUCCESS);
    REQUIRE(opened);
    file = object_ref<XFile>(new XFile(emulator->kernel_state(), opened, false,
                                      false, true));
    port = object_ref<XIOCompletion>(new XIOCompletion(emulator->kernel_state()));
    export_ = emulator->export_resolver()->GetExportByOrdinal("xboxkrnl.exe", 0xF7);
    REQUIRE(export_);
    REQUIRE(export_->is_implemented());
    REQUIRE(export_->function_data.trampoline);
    query_export_ = emulator->export_resolver()->GetExportByOrdinal("xboxkrnl.exe", 0xE8);
    REQUIRE(query_export_);
    REQUIRE(query_export_->is_implemented());
    REQUIRE(query_export_->function_data.trampoline);
    REQUIRE_FALSE(query_export_->tags & cpu::ExportTag::kStub);
  }
  ~FileInfoFixture() {
    if (file) {
      file->ReleaseHandle();
      file.reset();
    }
    if (port) {
      port->ReleaseHandle();
      port.reset();
    }
    emulator.reset();
  }
  uint8_t* Page() { return emulator->memory()->TranslateVirtual(kPage); }
  uint32_t Call(uint32_t handle, uint32_t iosb, uint32_t info,
                uint32_t length, uint32_t kind) {
    PPCContext context{};
    context.virtual_membase = emulator->memory()->virtual_membase();
    context.r[1] = kPage + 0x800;
    const std::array<uint32_t, 5> args{handle, iosb, info, length, kind};
    for (size_t i = 0; i < args.size(); ++i) {
      context.r[3 + i] = 0xAABBCCDD00000000ull | args[i];
    }
    export_->function_data.trampoline(&context);
    return uint32_t(context.r[3]);
  }
  uint32_t Query(uint32_t handle, uint32_t iosb, uint32_t info,
                 uint32_t length, uint32_t kind = XFileAllocationInformation) {
    PPCContext context{};
    context.virtual_membase = emulator->memory()->virtual_membase();
    context.r[1] = kPage + 0x800;
    const std::array<uint32_t, 5> args{handle, iosb, info, length, kind};
    for (size_t i = 0; i < args.size(); ++i) {
      // Exercise the registered PPC thunk's low-DWORD argument ABI.
      context.r[3 + i] = 0xAABBCCDD00000000ull | args[i];
    }
    query_export_->function_data.trampoline(&context);
    return uint32_t(context.r[3]);
  }
  void CheckBytes() {
    // Same real open FD: a second writable/read lease must not be invented.
    std::array<uint8_t, 7> bytes{};
    size_t count = 0;
    REQUIRE(file->file()->ReadSync(bytes, 0, &count) == X_STATUS_SUCCESS);
    CHECK(count == original.size());
    CHECK(std::equal(original.begin(), original.end(), bytes.begin()));
  }
  void CheckNoCompletion() {
    uint64_t zero_timeout = 0;
    CHECK(file->Wait(0, 0, 0, &zero_timeout) == X_STATUS_TIMEOUT);
    XIOCompletion::IONotification notification{};
    CHECK_FALSE(port->WaitForNotification(0, &notification));
  }
 private:
  // Emulator and its file objects die before scratch removal/cvar restoration.
  FileInfoSettings settings_;
  FileInfoScratch scratch_;
  cpu::Export* export_ = nullptr;
  cpu::Export* query_export_ = nullptr;
 public:
  std::unique_ptr<Emulator> emulator;
  std::shared_ptr<filesystem::HostStorageRoot> storage;
  object_ref<XFile> file;
  object_ref<XIOCompletion> port;
};
}

TEST_CASE("Registered file information refuses unimplemented setters without completion",
          "[kernel][file_control][abi][unsupported]") {
  FileInfoFixture f;
  // Binding uses the actual implemented class first, not a mock completion port.
  std::memset(f.Page(), 0xA5, 0x1000);
  auto* completion = reinterpret_cast<X_FILE_COMPLETION_INFORMATION*>(
      f.Page() + (FileInfoFixture::kInfo - FileInfoFixture::kPage));
  completion->handle = f.port->handle();
  completion->key = 0x12345678;
  REQUIRE(f.Call(f.file->handle(), f.kIosb, f.kInfo, sizeof(*completion),
                 XFileCompletionInformation) == X_STATUS_SUCCESS);
  REQUIRE(f.file->HasCompletionPort());
  f.file->set_position(9);
  const std::array<std::pair<uint32_t, uint32_t>, 5> unsupported{{
      {XFileModeInformation, 4}, {XFileIoPriorityInformation, 4},
      {XFileMountPartitionInformation, 8}, {XFileLinkInformation, 16},
      {XFileMountPartitionsInformation, 152}}};
  for (const auto [kind, minimum] : unsupported) {
    CAPTURE(kind);
    std::memset(f.Page(), 0xA5, 0x1000);
    if (kind == XFileMountPartitionInformation) {
      auto* name = reinterpret_cast<X_ANSI_STRING*>(
          f.Page() + (f.kInfo - f.kPage));
      name->length = uint16_t(std::strlen(f.kUnsupportedMount));
      name->maximum_length = uint16_t(std::strlen(f.kUnsupportedMount) + 1);
      name->pointer = f.kPage + 0x400;
      std::strcpy(reinterpret_cast<char*>(f.Page() + 0x400), f.kUnsupportedMount);
    }
    std::array<uint8_t, 0x1000> before;
    std::memcpy(before.data(), f.Page(), before.size());
    CHECK(f.Call(f.file->handle(), f.kIosb, f.kInfo, minimum, kind) ==
          X_STATUS_NOT_SUPPORTED);
    CHECK(std::memcmp(f.Page(), before.data(), before.size()) == 0);
    CHECK(f.file->position() == 9);
    CHECK(f.emulator->file_system()->GetDevice(f.kUnsupportedMount) == nullptr);
    CHECK_FALSE(f.emulator->file_system()->IsSymbolicLinkRegistered(
        f.kUnsupportedMount));
    f.CheckNoCompletion();
    f.CheckBytes();
  }
}

TEST_CASE("Registered file information preserves validation precedence and position",
          "[kernel][file_control][abi][precedence]") {
  FileInfoFixture f;
  std::memset(f.Page(), 0xA5, 0x1000);
  std::array<uint8_t, 0x1000> before;
  std::memcpy(before.data(), f.Page(), before.size());
  CHECK(f.Call(0xDEADBEEF, f.kIosb, 0, 0, UINT32_MAX) ==
        X_STATUS_INVALID_INFO_CLASS);
  CHECK(f.Call(0xDEADBEEF, f.kIosb, 0, 7, XFileMountPartitionInformation) ==
        X_STATUS_INFO_LENGTH_MISMATCH);
  CHECK(f.Call(0xDEADBEEF, f.kIosb, 0, 8, XFileMountPartitionInformation) ==
        X_STATUS_INVALID_HANDLE);
  // The unsupported branch neither dereferences payload nor publishes an IOSB.
  CHECK(f.Call(f.file->handle(), 0, 0, 8, XFileMountPartitionInformation) ==
        X_STATUS_NOT_SUPPORTED);
  CHECK(std::memcmp(f.Page(), before.data(), before.size()) == 0);
  f.CheckNoCompletion();
  f.CheckBytes();
  CHECK(f.file->position() == 0);

  auto* position = reinterpret_cast<X_FILE_POSITION_INFORMATION*>(
      f.Page() + (f.kInfo - f.kPage));
  position->current_byte_offset = 3;
  REQUIRE(f.Call(f.file->handle(), f.kIosb, f.kInfo, sizeof(*position),
                 XFilePositionInformation) == X_STATUS_SUCCESS);
  const auto* iosb = reinterpret_cast<const X_IO_STATUS_BLOCK*>(
      f.Page() + (f.kIosb - f.kPage));
  CHECK(uint32_t(iosb->status) == X_STATUS_SUCCESS);
  CHECK(uint32_t(iosb->information) == 0);
  CHECK(f.file->position() == 3);
  CHECK(uint64_t(position->current_byte_offset) == 3);
  f.CheckNoCompletion();
  f.CheckBytes();
}
namespace {
// Independent native observation, never the production allocation query or
// Entry's remembered path/EOF. This readonly FD survives pathname replacement.
class NativeAllocationObservation final {
 public:
  explicit NativeAllocationObservation(const std::filesystem::path& path) {
    fd_ = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    REQUIRE(fd_ >= 0);
  }
  ~NativeAllocationObservation() { if (fd_ >= 0) ::close(fd_); }
  NativeAllocationObservation(const NativeAllocationObservation&) = delete;
  NativeAllocationObservation& operator=(const NativeAllocationObservation&) = delete;
  struct stat Read() const {
    struct stat value{};
    REQUIRE(::fstat(fd_, &value) == 0);
    REQUIRE(S_ISREG(value.st_mode));
    REQUIRE(value.st_blocks >= 0);
    return value;
  }
 private:
  int fd_ = -1;
};
uint64_t ExpectedNativeGuestAllocation(const struct stat& native,
                                      const FileInfoFixture& f) {
  REQUIRE(uint64_t(native.st_blocks) <= UINT64_MAX / 512);
  const uint64_t physical = uint64_t(native.st_blocks) * 512;
  const uint64_t unit = uint64_t(f.file->entry()->sectors_per_allocation_unit()) *
                        f.file->device()->bytes_per_sector();
  REQUIRE(unit != 0);
  REQUIRE(physical <= UINT64_MAX - (unit - 1));
  return ((physical + unit - 1) / unit) * unit;
}
void CheckObservedOpenedIdentity(FileInfoFixture& f, const struct stat& native) {
  const auto* host = dynamic_cast<const vfs::HostPathFile*>(f.file->file());
  REQUIRE(host);
  std::error_code error;
  const auto info = host->QueryBackingInfo(error);
  REQUIRE_FALSE(error);
  REQUIRE(info);
  CHECK(info->filesystem_id == uint64_t(native.st_dev));
  CHECK(info->file_id == uint64_t(native.st_ino));
  CHECK(info->size == uint64_t(native.st_size));
}
void CheckAllocationPacket(FileInfoFixture& f, uint64_t expected,
                           uint32_t capacity = 8, bool iosb = true) {
  std::memset(f.Page(), 0xA5, 0x1000);
  std::array<uint8_t, 0x1000> wanted;
  wanted.fill(0xA5);
  xe::store_and_swap<uint64_t>(wanted.data() + (f.kInfo - f.kPage), expected);
  if (iosb) {
    xe::store_and_swap<uint32_t>(wanted.data() + (f.kIosb - f.kPage),
                                 X_STATUS_SUCCESS);
    xe::store_and_swap<uint32_t>(wanted.data() + (f.kIosb - f.kPage) + 4, 8);
  }
  const auto position = f.file->position();
  REQUIRE(f.Query(f.file->handle(), iosb ? f.kIosb : 0, f.kInfo, capacity) ==
          X_STATUS_SUCCESS);
  // Whole-page guards cover bytes immediately before/after both exact spans,
  // and capacity beyond eight must neither be zeroed nor validated as output.
  CHECK(std::memcmp(f.Page(), wanted.data(), wanted.size()) == 0);
  CHECK(f.file->position() == position);
  f.CheckNoCompletion();
}
void WriteNativeReplacement(const std::filesystem::path& path) {
  struct Close {
    int fd = -1;
    ~Close() { if (fd >= 0) ::close(fd); }
  } owner;
  owner.fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC |
                                     O_NOFOLLOW, 0600);
  REQUIRE(owner.fd >= 0);
  std::array<uint8_t, 4096> bytes;
  uint32_t random = 0x6D2B79F5u;
  for (unsigned chunk = 0; chunk < 64; ++chunk) {
    // Continuous deterministic xorshift data prevents transparent compression
    // from making this replacement look physically identical to the tiny file.
    for (auto& byte : bytes) {
      random ^= random << 13;
      random ^= random >> 17;
      random ^= random << 5;
      byte = uint8_t(random);
    }
    size_t written = 0;
    while (written < bytes.size()) {
      const auto result = ::write(owner.fd, bytes.data() + written,
                                   bytes.size() - written);
      REQUIRE(result > 0);
      written += size_t(result);
    }
  }
  REQUIRE(::fsync(owner.fd) == 0);
}
}  // namespace

TEST_CASE("Registered allocation query reports real normal and sparse allocation",
          "[kernel][file_control][allocation][abi][native]") {
  FileInfoFixture f;
  std::memset(f.Page(), 0xA5, 0x1000);
  auto* completion = reinterpret_cast<X_FILE_COMPLETION_INFORMATION*>(
      f.Page() + (f.kInfo - f.kPage));
  completion->handle = f.port->handle();
  completion->key = 0x615119;
  REQUIRE(f.Call(f.file->handle(), f.kIosb, f.kInfo, sizeof(*completion),
                 XFileCompletionInformation) == X_STATUS_SUCCESS);
  REQUIRE(f.file->HasCompletionPort());
  NativeAllocationObservation observer(f.storage->path() / "sample.bin");
  std::array<uint8_t, 64 * 1024> normal;
  for (size_t i = 0; i < normal.size(); ++i) {
    normal[i] = uint8_t((i * 37) ^ (i >> 8));
  }
  size_t normal_written = 0;
  REQUIRE(f.file->file()->WriteSync(normal, 0, &normal_written) == X_STATUS_SUCCESS);
  REQUIRE(normal_written == normal.size());
  REQUIRE(f.file->file()->Flush() == X_STATUS_SUCCESS);
  const auto original = observer.Read();
  REQUIRE(original.st_blocks > 0);
  CheckObservedOpenedIdentity(f, original);
  f.file->set_position(23);
  CheckAllocationPacket(f, ExpectedNativeGuestAllocation(original, f), 32);
  // Only the actual eight-byte output is accessed. A large capacity is not a
  // request to zero or validate the entire guest address space.
  CheckAllocationPacket(f, ExpectedNativeGuestAllocation(original, f), UINT32_MAX,
                         false);

  REQUIRE(f.file->file()->SetLength(0) == X_STATUS_SUCCESS);
  constexpr size_t kSparseBytes = 32 * 1024 * 1024;
  REQUIRE(f.file->file()->SetLength(kSparseBytes) == X_STATUS_SUCCESS);
  const auto sparse = observer.Read();
  REQUIRE(uint64_t(sparse.st_size) == kSparseBytes);
  REQUIRE(sparse.st_blocks == 0);  // Genuine sparse fixture, not an EOF oracle.
  CheckObservedOpenedIdentity(f, sparse);
  CheckAllocationPacket(f, 0);

  std::array<uint8_t, 4096> payload;
  for (size_t i = 0; i < payload.size(); ++i) {
    payload[i] = uint8_t((i * 97) ^ (i >> 3));
  }
  size_t written = 0;
  REQUIRE(f.file->file()->WriteSync(payload, kSparseBytes - payload.size(),
                                    &written) == X_STATUS_SUCCESS);
  REQUIRE(written == payload.size());
  REQUIRE(f.file->file()->Flush() == X_STATUS_SUCCESS);
  const auto allocated = observer.Read();
  REQUIRE(allocated.st_blocks > 0);
  REQUIRE(uint64_t(allocated.st_blocks) * 512 < uint64_t(allocated.st_size));
  CheckObservedOpenedIdentity(f, allocated);
  CheckAllocationPacket(f, ExpectedNativeGuestAllocation(allocated, f));
  CHECK(f.file->position() == 23);
}

TEST_CASE("Registered allocation query follows the open inode after replacement",
          "[kernel][file_control][allocation][abi][retained]") {
  FileInfoFixture f;
  const auto named = f.storage->path() / "sample.bin";
  const auto retired_name = f.storage->path() / "original-retained.bin";
  NativeAllocationObservation observer(named);
  const auto original = observer.Read();
  std::error_code error;
  std::filesystem::rename(named, retired_name, error);
  REQUIRE_FALSE(error);
  // This models a real external namespace replacement while the guest keeps
  // its already-open authority. The new inode must never become its metadata.
  WriteNativeReplacement(named);
  NativeAllocationObservation replacement(named);
  const auto other = replacement.Read();
  REQUIRE(original.st_dev == other.st_dev);
  REQUIRE(original.st_ino != other.st_ino);
  REQUIRE(ExpectedNativeGuestAllocation(original, f) !=
          ExpectedNativeGuestAllocation(other, f));
  const auto still_open = observer.Read();
  CheckObservedOpenedIdentity(f, still_open);
  CheckAllocationPacket(f, ExpectedNativeGuestAllocation(still_open, f));
  f.CheckBytes();

  REQUIRE(std::filesystem::remove(retired_name, error));
  REQUIRE_FALSE(error);
  const auto unlinked = observer.Read();
  REQUIRE(unlinked.st_nlink == 0);
  CheckObservedOpenedIdentity(f, unlinked);
  CheckAllocationPacket(f, ExpectedNativeGuestAllocation(unlinked, f));
  f.CheckBytes();
}

TEST_CASE("Registered allocation query rejects invalid spans without partial output",
          "[kernel][file_control][allocation][abi][validation]") {
  FileInfoFixture f;
  REQUIRE(f.emulator->memory()->QueryRangeAccess(f.kPage + 0x1000, 8) ==
          xe::memory::PageAccess::kNoAccess);
  const auto unchanged = [&](uint32_t handle, uint32_t iosb, uint32_t info,
                              uint32_t length, uint32_t kind,
                              X_STATUS expected) {
    std::memset(f.Page(), 0xA5, 0x1000);
    std::array<uint8_t, 0x1000> before;
    std::memcpy(before.data(), f.Page(), before.size());
    CAPTURE(handle, iosb, info, length, kind);
    CHECK(f.Query(handle, iosb, info, length, kind) == expected);
    CHECK(std::memcmp(f.Page(), before.data(), before.size()) == 0);
    CHECK(f.file->position() == 0);
    f.CheckNoCompletion();
    f.CheckBytes();
  };
  unchanged(0xDEADBEEF, f.kIosb, 0, 0, UINT32_MAX,
            X_STATUS_INVALID_INFO_CLASS);
  unchanged(0xDEADBEEF, f.kIosb, 0, 7, XFileAllocationInformation,
            X_STATUS_INFO_LENGTH_MISMATCH);
  unchanged(0xDEADBEEF, f.kIosb, 0, 8, XFileAllocationInformation,
            X_STATUS_INVALID_HANDLE);
  for (const uint32_t invalid_output :
       {0u, f.kPage + 0x1000, f.kPage + 0xFFC, 0xFFFFFFFCu}) {
    unchanged(f.file->handle(), f.kIosb, invalid_output, 8,
              XFileAllocationInformation, X_STATUS_ACCESS_VIOLATION);
  }
  for (const uint32_t invalid_iosb :
       {f.kPage + 0x1000, f.kPage + 0xFFC, 0xFFFFFFFCu}) {
    unchanged(f.file->handle(), invalid_iosb, f.kInfo, 8,
              XFileAllocationInformation, X_STATUS_ACCESS_VIOLATION);
  }
  // Conservative common GuestIo two-span policy: no aliased publication. This
  // is not claimed as a recovered retail-kernel acceptance/rejection rule.
  for (const uint32_t overlap : {f.kInfo, f.kInfo + 4}) {
    unchanged(f.file->handle(), overlap, f.kInfo, 8,
              XFileAllocationInformation, X_STATUS_INVALID_PARAMETER);
  }
  {
    std::memset(f.Page(), 0xA5, 0x1000);
    std::array<uint8_t, 0x1000> before;
    std::memcpy(before.data(), f.Page(), before.size());
    auto outer = global_critical_region::AcquireDirect();
    CHECK(f.Query(f.file->handle(), f.kIosb, f.kInfo, 8) ==
          X_STATUS_INVALID_DEVICE_REQUEST);
    CHECK(std::memcmp(f.Page(), before.data(), before.size()) == 0);
    CHECK(global_critical_region::is_held_by_current_thread());
  }
  // Host observations and wait helpers run only after the caller's lock ends.
  f.CheckNoCompletion();
  f.CheckBytes();

  constexpr uint32_t kReadOnly = FileInfoFixture::kPage + 0x2000;
  auto* memory = f.emulator->memory();
  REQUIRE(memory->LookupHeap(kReadOnly)->AllocFixed(kReadOnly, 0x1000, 0x1000,
      kMemoryAllocationReserve | kMemoryAllocationCommit,
      kMemoryProtectRead | kMemoryProtectWrite));
  auto* readonly_bytes = memory->TranslateVirtual(kReadOnly);
  std::memset(readonly_bytes, 0xA5, 0x1000);
  std::array<uint8_t, 0x1000> readonly_before;
  std::memcpy(readonly_before.data(), readonly_bytes, readonly_before.size());
  struct RestoreProtection {
    Memory* memory;
    uint32_t page;
    ~RestoreProtection() {
      memory->LookupHeap(page)->Protect(page, 0x1000,
                                        kMemoryProtectRead | kMemoryProtectWrite);
    }
  } restore{memory, kReadOnly};
  REQUIRE(memory->LookupHeap(kReadOnly)->Protect(kReadOnly, 0x1000,
                                                kMemoryProtectRead));
  unchanged(f.file->handle(), kReadOnly, f.kInfo, 8,
            XFileAllocationInformation, X_STATUS_ACCESS_VIOLATION);
  CHECK(std::memcmp(readonly_bytes, readonly_before.data(),
                    readonly_before.size()) == 0);
  unchanged(f.file->handle(), f.kIosb, kReadOnly, 8,
            XFileAllocationInformation, X_STATUS_ACCESS_VIOLATION);
  CHECK(std::memcmp(readonly_bytes, readonly_before.data(),
                    readonly_before.size()) == 0);
}

TEST_CASE("Registered allocation query leaves unsupported raw backend outputs intact",
          "[kernel][file_control][allocation][abi][unsupported]") {
  FileInfoFixture f;
  auto raw = std::make_unique<vfs::NullDevice>(
      FileInfoFixture::kUnsupportedMount, std::initializer_list<std::string>{});
  REQUIRE(raw->Initialize());
  REQUIRE(f.emulator->file_system()->RegisterDevice(std::move(raw)));
  auto* entry = f.emulator->file_system()->ResolvePath(f.kUnsupportedMount);
  REQUIRE(entry);
  vfs::File* opened = nullptr;
  REQUIRE(entry->Open(filesystem::FileAccess::kGenericRead, &opened) ==
          X_STATUS_SUCCESS);
  REQUIRE(opened);
  auto unsupported = object_ref<XFile>(new XFile(f.emulator->kernel_state(),
      opened, false, false, true));
  std::memset(f.Page(), 0xA5, 0x1000);
  std::array<uint8_t, 0x1000> before;
  std::memcpy(before.data(), f.Page(), before.size());
  CHECK(f.Query(unsupported->handle(), f.kIosb, f.kInfo, 8) ==
        X_STATUS_NOT_SUPPORTED);
  CHECK(std::memcmp(f.Page(), before.data(), before.size()) == 0);
  unsupported->ReleaseHandle();
  unsupported.reset();
  f.CheckNoCompletion();
  f.CheckBytes();
}

namespace {
struct FileQueueResults {
  std::mutex mutex;
  std::condition_variable changed;
  std::vector<std::pair<unsigned, X_STATUS>> rows;
  void Record(unsigned sequence, X_STATUS status) {
    { std::lock_guard lock(mutex); rows.emplace_back(sequence, status); }
    changed.notify_all();
  }
  bool Wait(size_t count) {
    std::unique_lock lock(mutex);
    return changed.wait_for(lock, std::chrono::seconds(5),
                             [&] { return rows.size() >= count; });
  }
};
struct FileQueueGate {
  std::mutex mutex;
  std::condition_variable changed;
  unsigned entered = 0;
  bool released = false;
  void Enter() {
    std::unique_lock lock(mutex);
    ++entered; changed.notify_all();
    changed.wait(lock, [&] { return released; });
  }
  bool Wait(unsigned count) {
    std::unique_lock lock(mutex);
    return changed.wait_for(lock, std::chrono::seconds(5),
                             [&] { return entered >= count; });
  }
  void Release() {
    { std::lock_guard lock(mutex); released = true; }
    changed.notify_all();
  }
};
// A failed REQUIRE must release real workers before Emulator's join. Each
// worker owns its shared gate, so release never destroys a live wait frame.
struct FileQueueGateRelease {
  std::shared_ptr<FileQueueGate> gate;
  ~FileQueueGateRelease() { gate->Release(); }
};
}

TEST_CASE("HostPath flush barrier orders actual queued write before completion",
          "[kernel][file_control][queued_flush]") {
  FileInfoFixture f;
  auto results = std::make_shared<FileQueueResults>();
  f.file->PostIo(nullptr, [file = f.file, results](bool cancelled) {
    const std::array<uint8_t, 2> bytes{0x80, 0xff};
    size_t written = 0;
    auto status = cancelled ? X_STATUS_CANCELLED :
        file->file()->WriteSync(bytes, 2, &written);
    if (XSUCCEEDED(status) && written != bytes.size()) { status = X_STATUS_UNSUCCESSFUL; }
    results->Record(1, status);
    // Actual SAME-file queue worker: an asynchronous barrier must append and
    // return, never block waiting behind its own currently running callback.
    const auto admitted = file->FlushQueued([results](X_STATUS flushed) {
      results->Record(2, flushed);
    });
    if (XFAILED(admitted)) { results->Record(2, admitted); }
    file->PostIo(nullptr, [results](bool canceled) {
      results->Record(3, canceled ? X_STATUS_CANCELLED : X_STATUS_SUCCESS);
    });
  });
  REQUIRE(results->Wait(3));
  {
    std::lock_guard lock(results->mutex);
    REQUIRE(results->rows.size() == 3);
    for (size_t i = 0; i < results->rows.size(); ++i) {
      CHECK(results->rows[i].first == i + 1);
      CHECK(results->rows[i].second == X_STATUS_SUCCESS);
    }
  }
  std::vector<uint8_t> read;
  REQUIRE_FALSE(f.storage->ReadFile("sample.bin", 16, read));
  CHECK(read == (std::vector<uint8_t>{'b', 'a', 0x80, 0xff, 'e', 'd'}));
  CHECK(f.file->position() == 0);
  f.CheckNoCompletion();
}

TEST_CASE("Queued writable flush commits actual retained overwrite EOF",
          "[kernel][file_control][queued_flush][overwrite]") {
  FileInfoFixture f;
  f.file->ReleaseHandle();
  f.file.reset();
  vfs::File* opened = nullptr;
  vfs::FileAction action;
  REQUIRE(f.emulator->file_system()->OpenFile(
      nullptr, std::string(FileInfoFixture::kMount) + "\\sample.bin",
      vfs::FileDisposition::kOverwrite, filesystem::FileAccess::kGenericAll,
      false, true, &opened, &action) == X_STATUS_SUCCESS);
  REQUIRE(opened);
  f.file = object_ref<XFile>(new XFile(f.emulator->kernel_state(), opened,
                                       false, false, true));
  auto* host = dynamic_cast<vfs::HostPathFile*>(f.file->file());
  REQUIRE(host);
  const std::array<uint8_t, 2> bytes{0x35, 0xa6};
  size_t written = 0;
  REQUIRE(host->WriteSync(bytes, 0, &written) == X_STATUS_SUCCESS);
  REQUIRE(written == bytes.size());
  std::error_code error;
  auto info = host->QueryBackingInfo(error);
  REQUIRE_FALSE(error);
  REQUIRE(info);
  REQUIRE(info->size == f.original.size());  // Real retained physical tail.
  auto results = std::make_shared<FileQueueResults>();
  REQUIRE(f.file->FlushQueued([results](X_STATUS status) {
    results->Record(1, status);
  }) == X_STATUS_SUCCESS);
  REQUIRE(results->Wait(1));
  {
    std::lock_guard lock(results->mutex);
    REQUIRE(results->rows.size() == 1);
    REQUIRE(results->rows[0].second == X_STATUS_SUCCESS);
  }
  info = host->QueryBackingInfo(error);
  REQUIRE_FALSE(error);
  REQUIRE(info);
  CHECK(info->size == bytes.size());  // CompleteOverwrite used the same FD.
  f.CheckNoCompletion();
  // Completion does not promise the queue runner's last XFile reference has
  // gone. Read the SAME owned FD rather than race a new alias against close.
  std::array<uint8_t, 8> read{};
  size_t count = 0;
  REQUIRE(host->ReadSync(read, 0, &count) == X_STATUS_SUCCESS);
  CHECK(count == bytes.size());
  CHECK(std::equal(bytes.begin(), bytes.end(), read.begin()));
}

TEST_CASE("Readonly queued flush drains while rejected admission never completes",
          "[kernel][file_control][queued_flush]") {
  FileInfoFixture f;
  vfs::File* opened = nullptr;
  REQUIRE(f.file->entry()->Open(filesystem::FileAccess::kGenericRead, &opened) ==
          X_STATUS_SUCCESS);
  REQUIRE(opened);
  auto readonly = object_ref<XFile>(new XFile(f.emulator->kernel_state(), opened,
                                              false, false, true));
  CHECK(readonly->file()->Flush() == X_STATUS_ACCESS_DENIED);
  auto results = std::make_shared<FileQueueResults>();
  REQUIRE(readonly->FlushQueued([results](X_STATUS status) {
    results->Record(1, status);
  }) == X_STATUS_SUCCESS);
  REQUIRE(results->Wait(1));
  {
    std::lock_guard lock(results->mutex);
    REQUIRE(results->rows.size() == 1);
    CHECK(results->rows[0].second == X_STATUS_SUCCESS);
  }
  auto rejected = std::make_shared<std::atomic<unsigned>>(0);
  CHECK(f.file->FlushQueued({}) == X_STATUS_INVALID_PARAMETER);
  {
    auto outer = global_critical_region::AcquireDirect();
    CHECK(f.file->FlushQueued([rejected](X_STATUS) { ++*rejected; }) ==
          X_STATUS_INVALID_DEVICE_REQUEST);
    CHECK(global_critical_region::is_held_by_current_thread());
  }
  CHECK(rejected->load() == 0);
  auto null_device = std::make_unique<vfs::NullDevice>(
      FileInfoFixture::kUnsupportedMount, std::initializer_list<std::string>{});
  REQUIRE(null_device->Initialize());
  REQUIRE(null_device->supports_concurrent_io());
  REQUIRE(f.emulator->file_system()->RegisterDevice(std::move(null_device)));
  auto* null_entry = f.emulator->file_system()->ResolvePath(
      FileInfoFixture::kUnsupportedMount);
  REQUIRE(null_entry);
  vfs::File* null_backend = nullptr;
  REQUIRE(null_entry->Open(filesystem::FileAccess::kGenericRead, &null_backend) ==
          X_STATUS_SUCCESS);
  REQUIRE(null_backend);
  auto null_file = object_ref<XFile>(new XFile(f.emulator->kernel_state(),
      null_backend, false, false, true));
  CHECK(null_file->FlushQueued([rejected](X_STATUS) { ++*rejected; }) ==
        X_STATUS_NOT_SUPPORTED);
  CHECK(rejected->load() == 0);  // Concurrent flag alone never mints support.
  null_file->ReleaseHandle();
  null_file.reset();
  readonly->ReleaseHandle();
  readonly.reset();
  f.CheckNoCompletion();
  f.CheckBytes();
}

TEST_CASE("Queued flush normal cancellation settles exactly once without IO",
          "[kernel][file_control][queued_flush]") {
  FileInfoFixture f;
  auto gate = std::make_shared<FileQueueGate>();
  FileQueueGateRelease release{gate};
  auto results = std::make_shared<FileQueueResults>();
  f.file->PostIo(nullptr, [gate](bool cancelled) { if (!cancelled) { gate->Enter(); } });
  REQUIRE(gate->Wait(1));
  REQUIRE(f.file->FlushQueued([results](X_STATUS status) {
    results->Record(1, status);
  }) == X_STATUS_SUCCESS);
  f.file->CancelPostedIo(nullptr);
  const bool completed = results->Wait(1);
  gate->Release();
  REQUIRE(completed);
  {
    std::lock_guard lock(results->mutex);
    REQUIRE(results->rows.size() == 1);
    CHECK(results->rows[0].second == X_STATUS_CANCELLED);
  }
  f.CheckNoCompletion();
  f.CheckBytes();
}

TEST_CASE("Shutdown drops owned flush while preserving legacy drop and admission",
          "[kernel][file_control][queued_flush][scheduler_drop]") {
  FileInfoFixture f;
  auto* scheduler = f.emulator->kernel_state()->guest_scheduler();
  using Class = GuestScheduler::BlockingCallClass;
  using Reason = GuestScheduler::HostCallDropReason;
  auto gate = std::make_shared<FileQueueGate>();
  FileQueueGateRelease release{gate};
  // Actual current pool bound is four; all four real workers are occupied so
  // the following runner is certainly unstarted when shutdown closes ingress.
  for (unsigned i = 0; i < 4; ++i) {
    scheduler->PostHostCall([gate] { gate->Enter(); }, Class::kConcurrent);
  }
  REQUIRE(gate->Wait(4));
  auto legacy_calls = std::make_shared<std::atomic<unsigned>>(0);
  auto owned_body = std::make_shared<std::atomic<unsigned>>(0);
  auto nested_inline = std::make_shared<std::atomic<unsigned>>(0);
  f.file->PostIo(nullptr, [legacy_calls](bool) { ++*legacy_calls; });
  auto results = std::make_shared<FileQueueResults>();
  REQUIRE(f.file->FlushQueued([results](X_STATUS status) {
    results->Record(1, status);
  }) == X_STATUS_SUCCESS);
  scheduler->PostHostCall([owned_body] { ++*owned_body; }, Class::kConcurrent,
      [results, file = f.file, scheduler, nested_inline](Reason reason) {
        results->Record(2, reason == Reason::kQueuedShutdown
                               ? X_STATUS_SUCCESS : X_STATUS_UNSUCCESSFUL);
        // Reenter both public queue metadata APIs to detect callbacks under
        // scheduler/file queue locks. No guest buffer or raw IOSB is used.
        (void)file->HasPostedIo(nullptr);
        scheduler->PostHostCall([nested_inline] { ++*nested_inline; }, Class::kConcurrent);
      });
  std::thread shutdown([&] { scheduler->Shutdown(); });
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!scheduler->shutting_down() && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::yield();
  }
  const bool closing = scheduler->shutting_down();
  gate->Release();
  shutdown.join();  // No assertions throw while the joinable thread exists.
  REQUIRE(closing);
  REQUIRE(results->Wait(2));
  {
    std::lock_guard lock(results->mutex);
    REQUIRE(results->rows.size() == 2);
    CHECK(results->rows[0] == std::make_pair(1u, X_STATUS_CANCELLED));
    CHECK(results->rows[1] == std::make_pair(2u, X_STATUS_SUCCESS));
  }
  CHECK(legacy_calls->load() == 0);
  CHECK(owned_body->load() == 0);
  CHECK(nested_inline->load() == 1);
  f.file->PostIo(nullptr, [legacy_calls](bool cancelled) {
    if (!cancelled) { ++*legacy_calls; }
  });
  CHECK(legacy_calls->load() == 1);  // Existing after-stop inline behavior.
  auto admission_body = std::make_shared<std::atomic<unsigned>>(0);
  scheduler->PostHostCall([admission_body] { ++*admission_body; }, Class::kConcurrent,
      [results](Reason reason) {
        results->Record(3, reason == Reason::kAdmissionShutdown
                               ? X_STATUS_SUCCESS : X_STATUS_UNSUCCESSFUL);
      });
  CHECK(admission_body->load() == 0);
  REQUIRE(f.file->FlushQueued([results](X_STATUS status) {
    results->Record(4, status);
  }) == X_STATUS_SUCCESS);
  REQUIRE(results->Wait(4));
  {
    std::lock_guard lock(results->mutex);
    REQUIRE(results->rows.size() == 4);
    CHECK(results->rows[2] == std::make_pair(3u, X_STATUS_SUCCESS));
    CHECK(results->rows[3] == std::make_pair(4u, X_STATUS_CANCELLED));
  }
  f.CheckNoCompletion();
  f.CheckBytes();
}

TEST_CASE("Registered retained metadata reports the opened file with exact fixed outputs",
          "[kernel][file_control][file_information][abi]") {
  FileInfoFixture f;
  NativeAllocationObservation observer(f.storage->path()/"sample.bin");
  const auto original_creation=f.file->entry()->create_timestamp();
  const auto retired=f.storage->path()/"retained.bin";
  std::filesystem::rename(f.storage->path()/"sample.bin",retired);
  WriteNativeReplacement(f.storage->path()/"sample.bin");
  // A stale/shared Entry may now describe the replacement; the registered
  // syscall must still match its original FD and readable bytes.
  f.file->entry()->update();
  REQUIRE(f.file->entry()->size()>f.original.size());
  const auto check=[&](bool iosb) {
    const auto native=observer.Read();
    std::memset(f.Page(),0xA5,0x1000);
    const auto position=f.file->position();
    REQUIRE(f.Query(f.file->handle(),iosb?f.kIosb:0,f.kInfo,96,
                    XFileNetworkOpenInformation) == X_STATUS_SUCCESS);
    const auto* info=reinterpret_cast<const X_FILE_NETWORK_OPEN_INFORMATION*>(
        f.Page()+(f.kInfo-f.kPage));
    CHECK(uint64_t(info->end_of_file)==uint64_t(native.st_size));
    CHECK(uint64_t(info->allocation_size)==ExpectedNativeGuestAllocation(native,f));
    CHECK(uint64_t(info->creation_time)==original_creation);
    CHECK(uint64_t(info->last_access_time)==
        (uint64_t(native.st_atim.tv_sec)+11644473600ull)*10000000ull+
         uint64_t(native.st_atim.tv_nsec)/100);
    CHECK(uint64_t(info->last_write_time)==
        (uint64_t(native.st_mtim.tv_sec)+11644473600ull)*10000000ull+
         uint64_t(native.st_mtim.tv_nsec)/100);
    CHECK(uint64_t(info->change_time)==
        (uint64_t(native.st_ctim.tv_sec)+11644473600ull)*10000000ull+
         uint64_t(native.st_ctim.tv_nsec)/100);
    CHECK(uint32_t(info->pad)==0);
    for(size_t i=0;i<0x1000;++i) {
      const bool payload=i>=f.kInfo-f.kPage && i<f.kInfo-f.kPage+56;
      const bool status=iosb && i>=f.kIosb-f.kPage && i<f.kIosb-f.kPage+8;
      if(!payload && !status) REQUIRE(f.Page()[i]==0xA5);
    }
    if(iosb) {
      const auto* status=reinterpret_cast<const X_IO_STATUS_BLOCK*>(f.Page()+(f.kIosb-f.kPage));
      CHECK(uint32_t(status->status)==X_STATUS_SUCCESS);
      CHECK(uint32_t(status->information)==56);
    }
    CHECK(f.file->position()==position);f.CheckNoCompletion();
  };
  check(true);
  REQUIRE(std::filesystem::remove(retired));
  CHECK(observer.Read().st_nlink==0);
  check(false);f.CheckBytes();
}
TEST_CASE("Registered retained metadata rejects bad and retired outputs without mutation",
          "[kernel][file_control][file_information][validation]") {
  FileInfoFixture f;
  const auto unchanged=[&](uint32_t handle,uint32_t iosb,uint32_t output,
                           uint32_t length,X_STATUS expected) {
    std::memset(f.Page(),0xA5,0x1000);
    std::array<uint8_t,0x1000> before;
    std::memcpy(before.data(),f.Page(),before.size());
    CHECK(f.Query(handle,iosb,output,length,XFileNetworkOpenInformation)==expected);
    CHECK(std::memcmp(before.data(),f.Page(),before.size())==0);
    CHECK(f.file->position()==0);
  };
  unchanged(0xDEADBEEF,f.kIosb,0,55,X_STATUS_INFO_LENGTH_MISMATCH);
  unchanged(0xDEADBEEF,f.kIosb,0,56,X_STATUS_INVALID_HANDLE);
  for(uint32_t output:{0u,f.kPage+0x1000,f.kPage+0xFE0,0xFFFFFFFCu})
    unchanged(f.file->handle(),f.kIosb,output,56,X_STATUS_ACCESS_VIOLATION);
  for(uint32_t iosb:{f.kPage+0x1000,f.kPage+0xFFC,0xFFFFFFFCu})
    unchanged(f.file->handle(),iosb,f.kInfo,56,X_STATUS_ACCESS_VIOLATION);
  for(uint32_t iosb:{f.kInfo,f.kInfo+48})
    unchanged(f.file->handle(),iosb,f.kInfo,56,X_STATUS_INVALID_PARAMETER);
  { auto lock=global_critical_region::AcquireDirect();
    unchanged(f.file->handle(),f.kIosb,f.kInfo,56,X_STATUS_INVALID_DEVICE_REQUEST);
  }
  f.storage->Retire();
  unchanged(f.file->handle(),f.kIosb,f.kInfo,56,X_STATUS(0xC000009D));
  f.CheckNoCompletion();
}

TEST_CASE("Host invocation failures are explicit and actual serial/pool workers survive",
          "[kernel][file_control][host_io][failure]") {
  FileInfoFixture f;
  auto* kernel = f.emulator->kernel_state();
  CHECK(kernel->RunBlockingIo([] { throw std::runtime_error("inline IO"); },
      GuestScheduler::BlockingCallClass::kConcurrent) ==
        HostCallOutcome::kExecutionFailed);
  CHECK(HostCallFailureStatus(HostCallOutcome::kExecutionFailed) ==
        X_STATUS_UNSUCCESSFUL);
  using Class = GuestScheduler::BlockingCallClass;
  for (const auto kind : {Class::kSerial, Class::kConcurrent}) {
    CAPTURE(int(kind));
    // Exceed the real four-worker pool bound. Replacing a dead worker with a
    // newly started one cannot make this test pass indefinitely.
    for (unsigned iteration = 0; iteration < 6; ++iteration) {
      CAPTURE(iteration);
      auto results = std::make_shared<FileQueueResults>();
      auto worker_seen = std::make_shared<std::atomic<unsigned>>(0);
      auto entered = std::shared_ptr<threading::Event>(
          threading::Event::CreateManualResetEvent(false));
      REQUIRE(kernel->guest_scheduler()->PostHostCall([worker_seen, entered] {
        if (GuestScheduler::CurrentThreadIsBlockingCallWorker()) { ++*worker_seen; }
        entered->Set();
        throw std::runtime_error("actual worker IO");
      }, kind));
      REQUIRE(threading::Wait(entered.get(), false, std::chrono::seconds(5)) ==
              threading::WaitResult::kSuccess);
      REQUIRE(kernel->guest_scheduler()->PostHostCall([results, worker_seen] {
        if (GuestScheduler::CurrentThreadIsBlockingCallWorker()) { ++*worker_seen; }
        results->Record(1, X_STATUS_SUCCESS);
      }, kind));
      REQUIRE(results->Wait(1));
      CHECK(worker_seen->load() == 2);
    }
  }
  CHECK_FALSE(GuestScheduler::CurrentThreadIsBlockingCallWorker());
  f.CheckNoCompletion();
  f.CheckBytes();
}

TEST_CASE("IO admission resource failure never publishes a borrowed call or drops it as cancelled",
          "[kernel][file_control][host_io][admission]") {
  using Class = GuestScheduler::BlockingCallClass;
  for (uint32_t point : {1u, 2u, 3u, 4u}) {
    CAPTURE(point);
    FileInfoFixture f;
    auto* scheduler = f.emulator->kernel_state()->guest_scheduler();
    auto body = std::make_shared<std::atomic<unsigned>>(0);
    auto dropped = std::make_shared<std::atomic<unsigned>>(0);
    auto owner = std::make_shared<int>(42);
    std::weak_ptr<int> released = owner;
    GuestSchedulerIoTestAccess::FailNext(*scheduler, point);
    const auto kind = point <= 2 ? Class::kSerial : Class::kConcurrent;
    CHECK_FALSE(scheduler->PostHostCall([body, owner = std::move(owner)] { ++*body; }, kind,
        [dropped](GuestScheduler::HostCallDropReason) { ++*dropped; }));
    CHECK(released.expired());
    CHECK(body->load() == 0);
    CHECK(dropped->load() == 0);
    auto next = std::make_shared<FileQueueResults>();
    REQUIRE(scheduler->PostHostCall([next] { next->Record(1, X_STATUS_SUCCESS); }, kind));
    REQUIRE(next->Wait(1));
    CHECK(body->load() == 0);
    CHECK(dropped->load() == 0);
    f.CheckNoCompletion();
  }
}

TEST_CASE("Precommit failure preserves earlier and later actual serial FIFO owners",
          "[kernel][file_control][host_io][admission]") {
  FileInfoFixture f;
  auto* scheduler = f.emulator->kernel_state()->guest_scheduler();
  auto gate = std::make_shared<FileQueueGate>();
  FileQueueGateRelease release{gate};
  auto results = std::make_shared<FileQueueResults>();
  using Class = GuestScheduler::BlockingCallClass;
  REQUIRE(scheduler->PostHostCall([gate] { gate->Enter(); }, Class::kSerial));
  REQUIRE(gate->Wait(1));
  REQUIRE(scheduler->PostHostCall([results] { results->Record(1, X_STATUS_SUCCESS); }, Class::kSerial));
  GuestSchedulerIoTestAccess::FailNext(*scheduler, 2);
  CHECK_FALSE(scheduler->PostHostCall([results] { results->Record(2, X_STATUS_SUCCESS); }, Class::kSerial));
  REQUIRE(scheduler->PostHostCall([results] { results->Record(3, X_STATUS_SUCCESS); }, Class::kSerial));
  gate->Release();
  REQUIRE(results->Wait(2));
  std::lock_guard lock(results->mutex);
  REQUIRE(results->rows.size() == 2);
  CHECK(results->rows[0].first == 1);
  CHECK(results->rows[1].first == 3);
}

namespace {
class CompletionFaultFile final : public vfs::HostPathFile {
 public:
  CompletionFaultFile(uint32_t access, vfs::HostPathEntry* entry,
                      std::unique_ptr<filesystem::FileHandle> handle)
      : HostPathFile(access, entry, std::move(handle)) {}
  bool throw_after_write = false;
  bool throw_after_flush = false;
  std::atomic<X_STATUS> injected_write_status{X_STATUS_UNSUCCESSFUL};
  std::atomic<size_t> injected_write_bytes{0};
  std::atomic<X_STATUS> injected_flush_status{X_STATUS_UNSUCCESSFUL};
  X_STATUS WriteSync(std::span<const uint8_t> bytes, size_t offset,
                     size_t* count) override {
    const auto status = HostPathFile::WriteSync(bytes, offset, count);
    if (throw_after_write) {
      injected_write_status.store(status, std::memory_order_relaxed);
      injected_write_bytes.store(*count, std::memory_order_relaxed);
      throw std::runtime_error("after real backend write");
    }
    return status;
  }
  X_STATUS Flush() override {
    const auto status = HostPathFile::Flush();
    if (std::exchange(throw_after_flush, false)) {
      injected_flush_status.store(status, std::memory_order_relaxed);
      throw std::runtime_error("after actual flush");
    }
    return status;
  }
};
CompletionFaultFile* InstallCompletionFaultFile(FileInfoFixture& f) {
  f.file->ReleaseHandle(); f.file.reset();
  auto* entry = static_cast<vfs::HostPathEntry*>(f.emulator->file_system()->ResolvePath(
      std::string(FileInfoFixture::kMount) + "\\sample.bin"));
  REQUIRE(entry);
  std::error_code error;
  // This direct backend fixture bypasses VirtualFileSystem::OpenFile's
  // generic-mask normalization; give both layers the actual read/write bits.
  constexpr uint32_t access = filesystem::FileAccess::kGenericRead |
                              filesystem::FileAccess::kGenericWrite;
  auto handle = f.storage->OpenFile("sample.bin", access, error);
  REQUIRE(handle); REQUIRE_FALSE(error);
  auto* backend = new CompletionFaultFile(access, entry, std::move(handle));
  f.file = object_ref<XFile>(new XFile(f.emulator->kernel_state(), backend,
                                       false, false, true));
  return backend;
}
}

TEST_CASE("Actual backend IO effects and known counts survive distinct host failure cuts",
          "[kernel][file_control][host_io][completion]") {
  FileInfoFixture f;
  util::IoRequestCompletion state;
  SECTION("Known normal read survives later request postprocessing throw") {
    const auto status = util::CompleteIoRequestOwned(
        state, true, false, util::InlineIoFailurePolicy::kRejectWithoutCompletion,
        [&](uint32_t* count, util::IoTransferReceipt* receipt) {
          const auto read = f.file->Read(f.kInfo, uint32_t(f.original.size()), 0,
                                          count, 0, false, receipt);
          if (XSUCCEEDED(read)) { throw std::runtime_error("postprocessing"); }
          return read;
        },
        [&](X_STATUS result, uint32_t count) {
          return util::WriteGuestIoStatus(*f.emulator->memory(), f.kIosb, result, count);
        },
        [&](X_STATUS result, uint32_t count) {
          f.file->NotifyCompletion(result, count, 0, &state.notifications);
        });
    CHECK(status == X_STATUS_UNSUCCESSFUL);
    CHECK(state.transfer_call == HostCallOutcome::kExecutionFailed);
    CHECK(state.transfer.backend_entered);
    CHECK(state.transfer.backend_returned);
    CHECK(state.transfer.count_known);
    CHECK(state.transfer.bytes == f.original.size());
    CHECK(state.operation_count_known);
    CHECK(state.operation_bytes == f.original.size());
    CHECK(xe::load_and_swap<uint32_t>(f.Page() + 0x80) == X_STATUS_UNSUCCESSFUL);
    CHECK(xe::load_and_swap<uint32_t>(f.Page() + 0x84) == f.original.size());
    CHECK(std::equal(f.original.begin(), f.original.end(), f.Page() + 0x100));
  }
  SECTION("Exception inside actual backend after write is not proof of zero mutation") {
    auto* backend = InstallCompletionFaultFile(f);
    backend->throw_after_write = true;
    const std::array<uint8_t, 2> changed{0xE1, 0xB2};
    std::memcpy(f.Page() + 0x100, changed.data(), changed.size());
    uint32_t count = 0;
    CHECK(f.file->Write(f.kInfo, uint32_t(changed.size()), 0, &count, 0, false,
                         &state.transfer) == X_STATUS_UNSUCCESSFUL);
    CHECK(state.transfer.invocation == HostCallOutcome::kExecutionFailed);
    CHECK(state.transfer.backend_entered);
    CHECK_FALSE(state.transfer.backend_returned);
    CHECK_FALSE(state.transfer.count_known);
    CHECK(backend->injected_write_status.load() == X_STATUS_SUCCESS);
    CHECK(backend->injected_write_bytes.load() == changed.size());
    std::array<uint8_t, 6> actual{};
    size_t actual_count = 0;
    REQUIRE(backend->ReadSync(actual, 0, &actual_count) == X_STATUS_SUCCESS);
    CHECK(actual_count == actual.size());
    CHECK(std::equal(changed.begin(), changed.end(), actual.begin()));
  }
}

TEST_CASE("Attempted IOSB publication is never replayed after real store then throw",
          "[kernel][file_control][host_io][completion]") {
  FileInfoFixture f;
  util::IoRequestCompletion state;
  unsigned stores = 0, notices = 0;
  auto transfer = [&](uint32_t* count, util::IoTransferReceipt* receipt) {
    return f.file->Read(f.kInfo, uint32_t(f.original.size()), 0, count, 0, false, receipt);
  };
  auto publish = [&](X_STATUS status, uint32_t count) {
    ++stores;
    const auto actual = util::WriteGuestIoStatus(*f.emulator->memory(), f.kIosb, status, count);
    if (XSUCCEEDED(actual)) { throw std::runtime_error("after stored IOSB"); }
    return actual;
  };
  auto notify = [&](X_STATUS status, uint32_t count) {
    ++notices;
    CHECK(status == X_STATUS_UNSUCCESSFUL);
    CHECK(count == 0);
    f.file->NotifyCompletion(status, count, 0, &state.notifications);
  };
  CHECK(util::CompleteIoRequestOwned(state, true, false,
      util::InlineIoFailurePolicy::kRejectWithoutCompletion,
      transfer, publish, notify) == X_STATUS_UNSUCCESSFUL);
  CHECK(state.publication_call == HostCallOutcome::kExecutionFailed);
  // These actual prior bytes survive; publication is uncertain, never absent.
  CHECK(xe::load_and_swap<uint32_t>(f.Page() + 0x80) == X_STATUS_SUCCESS);
  CHECK(xe::load_and_swap<uint32_t>(f.Page() + 0x84) == f.original.size());
  CHECK(util::CompleteIoRequestOwned(state, true, false,
      util::InlineIoFailurePolicy::kRejectWithoutCompletion,
      transfer, publish, notify) == X_STATUS_INVALID_PARAMETER);
  CHECK(stores == 1); CHECK(notices == 1);
}

TEST_CASE("Independent completion channels proceed once after ordinary earlier failure",
          "[kernel][file_control][host_io][completion]") {
  FileInfoFixture f;
  util::IoNotificationReceipt receipt;
  auto external = threading::Event::CreateManualResetEvent(false);
  unsigned apc_attempts = 0, file_attempts = 0, event_attempts = 0;
  // This exercises the actual common channel protocol and real file/event
  // signal, not a valid native APC/Task or a fabricated guest thread.
  util::CompleteIoNotifications(receipt, true,
      [&] { ++apc_attempts; throw std::runtime_error("APC allocation"); },
      [&] { ++file_attempts; f.file->NotifyCompletion(X_STATUS_SUCCESS, 0, 0, &receipt); },
      true, [&] { ++event_attempts; external->Set(); });
  CHECK(apc_attempts == 1); CHECK(file_attempts == 1); CHECK(event_attempts == 1);
  CHECK(receipt.apc == HostCallOutcome::kExecutionFailed);
  CHECK(receipt.file_event == HostCallOutcome::kReturned);
  CHECK(receipt.external_event == HostCallOutcome::kReturned);
  uint64_t zero = 0;
  CHECK(f.file->Wait(0, 0, 0, &zero) == X_STATUS_SUCCESS);
  CHECK(threading::Wait(external.get(), false, std::chrono::milliseconds(0)) == threading::WaitResult::kSuccess);
}

TEST_CASE("Actual file FIFO continues after a failed request and an entered flush failure",
          "[kernel][file_control][host_io][queued_flush]") {
  FileInfoFixture f;
  auto* backend = InstallCompletionFaultFile(f);
  backend->throw_after_flush = true;
  auto gate = std::make_shared<FileQueueGate>();
  FileQueueGateRelease release{gate};
  auto results = std::make_shared<FileQueueResults>();
  f.file->PostIo(nullptr, [gate](bool) { gate->Enter(); });
  REQUIRE(gate->Wait(1));
  CHECK(f.file->HasPostedIo(nullptr));
  f.file->PostIo(nullptr, [](bool) { throw std::runtime_error("legacy request body"); });
  REQUIRE(f.file->FlushQueued([results](X_STATUS status) { results->Record(1, status); }) == X_STATUS_SUCCESS);
  REQUIRE(f.file->FlushQueued([results](X_STATUS status) { results->Record(2, status); }) == X_STATUS_SUCCESS);
  gate->Release();
  REQUIRE(results->Wait(2));
  CHECK(backend->injected_flush_status.load() == X_STATUS_SUCCESS);
  { std::lock_guard lock(results->mutex);
    REQUIRE(results->rows.size() == 2);
    CHECK(results->rows[0] == std::make_pair(1u, X_STATUS_UNSUCCESSFUL));
    CHECK(results->rows[1] == std::make_pair(2u, X_STATUS_SUCCESS));
  }
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (f.file->HasPostedIo(nullptr) && std::chrono::steady_clock::now() < deadline) { std::this_thread::yield(); }
  CHECK_FALSE(f.file->HasPostedIo(nullptr));
  f.CheckNoCompletion();
  f.CheckBytes();
}

TEST_CASE("Actual file queue resource failure reports its own unstarted outcome",
          "[kernel][file_control][host_io][admission][queued_flush]") {
  FileInfoFixture f;
  auto results = std::make_shared<FileQueueResults>();
  GuestSchedulerIoTestAccess::FailNext(*f.emulator->kernel_state()->guest_scheduler(), 4);
  REQUIRE(f.file->FlushQueued([results](X_STATUS status) { results->Record(1, status); }) == X_STATUS_SUCCESS);
  REQUIRE(results->Wait(1));
  { std::lock_guard lock(results->mutex);
    REQUIRE(results->rows.size() == 1);
    CHECK(results->rows[0] == std::make_pair(1u, X_STATUS_INSUFFICIENT_RESOURCES));
  }
  CHECK_FALSE(f.file->HasPostedIo(nullptr));
  f.CheckNoCompletion(); f.CheckBytes();
}


namespace {
struct CopyFailureControl {
  unsigned copies = 0, fail_at = 0;
};
struct ThrowingFlushCompletion {
  std::shared_ptr<CopyFailureControl> control;
  std::function<void(X_STATUS)> completion;
  ThrowingFlushCompletion(std::shared_ptr<CopyFailureControl> c,
                          std::function<void(X_STATUS)> fn)
      : control(std::move(c)), completion(std::move(fn)) {}
  ThrowingFlushCompletion(const ThrowingFlushCompletion& from)
      : control(from.control), completion(from.completion) {
    if (++control->copies == control->fail_at) { throw std::bad_alloc(); }
  }
  ThrowingFlushCompletion(ThrowingFlushCompletion&&) noexcept = default;
  void operator()(X_STATUS status) const { completion(status); }
};
}

TEST_CASE("Flush callback preparation failure is an unstarted rejection before file queue admission",
          "[kernel][file_control][host_io][admission][queued_flush]") {
  FileInfoFixture f;
  auto control = std::make_shared<CopyFailureControl>();
  auto results = std::make_shared<FileQueueResults>();
  std::function<void(X_STATUS)> completion = ThrowingFlushCompletion(control,
      [results](X_STATUS status) { results->Record(1, status); });
  // First copy is the caller's argument; second is the actual body capture
  // inside FlushQueued. The rejected body must never reach PostIo.
  control->fail_at = control->copies + 2;
  CHECK(f.file->FlushQueued(completion) == X_STATUS_INSUFFICIENT_RESOURCES);
  { std::lock_guard lock(results->mutex); CHECK(results->rows.empty()); }
  CHECK_FALSE(f.file->HasPostedIo(nullptr));
  f.CheckNoCompletion(); f.CheckBytes();
}

TEST_CASE("Batch callable conversion failure after earlier admission settles one slot and preserves later FIFO",
          "[kernel][file_control][host_io][admission][queued_flush][batch]") {
  FileInfoFixture f;
  auto gate = std::make_shared<FileQueueGate>();
  FileQueueGateRelease release{gate};
  f.file->PostIo(nullptr, [gate](bool) { gate->Enter(); });
  REQUIRE(gate->Wait(1));
  auto results = std::make_shared<FileQueueResults>();
  auto remaining = std::make_shared<std::atomic<unsigned>>(3);
  auto first_error = std::make_shared<std::atomic<X_STATUS>>(X_STATUS_SUCCESS);
  std::array<std::shared_ptr<util::IoBatchArrival>, 3> slots;
  for (auto& slot : slots) { slot = std::make_shared<util::IoBatchArrival>(); }
  const auto make_arrival = [&](unsigned i) {
    return [slot = slots[i], results, remaining, first_error, i](X_STATUS status) {
      slot->CompleteOnce(status, [&](X_STATUS actual) {
        if (XFAILED(actual)) {
          X_STATUS success = X_STATUS_SUCCESS;
          first_error->compare_exchange_strong(success, actual);
        }
        remaining->fetch_sub(1);
        results->Record(i + 1, actual);
      });
    };
  };
  auto first = make_arrival(0), rejected = make_arrival(1), last = make_arrival(2);
  util::SubmitIoBatchItem([&] { return f.file->FlushQueued(first); }, first);
  CHECK(remaining->load() == 3);  // Earlier real item is admitted behind gate.
  auto control = std::make_shared<CopyFailureControl>();
  std::function<void(X_STATUS)> completion = ThrowingFlushCompletion(control, rejected);
  control->fail_at = control->copies + 1;  // Actual caller argument conversion.
  util::SubmitIoBatchItem([&] { return f.file->FlushQueued(completion); }, rejected);
  CHECK(remaining->load() == 2);
  util::SubmitIoBatchItem([&] { return f.file->FlushQueued(last); }, last);
  gate->Release();
  REQUIRE(results->Wait(3));
  // A second observer cannot replay a settled slot or decrement remaining.
  rejected(X_STATUS_SUCCESS);
  CHECK(remaining->load() == 0);
  CHECK(first_error->load() == X_STATUS_INSUFFICIENT_RESOURCES);
  { std::lock_guard lock(results->mutex);
    REQUIRE(results->rows.size() == 3);
    CHECK(results->rows[0] == std::make_pair(2u, X_STATUS_INSUFFICIENT_RESOURCES));
    CHECK(results->rows[1] == std::make_pair(1u, X_STATUS_SUCCESS));
    CHECK(results->rows[2] == std::make_pair(3u, X_STATUS_SUCCESS));
  }
  f.CheckNoCompletion(); f.CheckBytes();
}


TEST_CASE("Unstarted multi-item resource settlement continues after a failed owner without replay",
          "[kernel][file_control][host_io][admission][owners]") {
  FileInfoFixture f;
  auto* scheduler = f.emulator->kernel_state()->guest_scheduler();
  auto lock = GuestSchedulerIoTestAccess::LockPool(*scheduler);
  auto results = std::make_shared<FileQueueResults>();
  auto bodies = std::make_shared<std::atomic<unsigned>>(0);
  std::thread first([&] {
    f.file->PostIo(nullptr, [bodies](bool) { ++*bodies; }, {},
        [results] { results->Record(1, X_STATUS_INSUFFICIENT_RESOURCES);
                    throw std::runtime_error("first resource owner"); });
  });
  struct ReleaseJoin {
    std::unique_lock<std::mutex>& lock;
    std::thread& thread;
    ~ReleaseJoin() { if (lock.owns_lock()) { lock.unlock(); }
                    if (thread.joinable()) { thread.join(); } }
  } release{lock, first};
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!f.file->HasPostedIo(nullptr) && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::yield();
  }
  const bool queued = f.file->HasPostedIo(nullptr);
  if (queued) {
    for (unsigned i : {2u, 3u}) {
      f.file->PostIo(nullptr, [bodies](bool) { ++*bodies; }, {},
          [results, i] { results->Record(i, X_STATUS_INSUFFICIENT_RESOURCES); });
    }
  }
  GuestSchedulerIoTestAccess::FailNext(*scheduler, 4);
  lock.unlock(); first.join();
  REQUIRE(queued); REQUIRE(results->Wait(3));
  { std::lock_guard rows(results->mutex);
    REQUIRE(results->rows.size() == 3);
    for (unsigned i = 0; i < 3; ++i) {
      CHECK(results->rows[i] == std::make_pair(i + 1, X_STATUS_INSUFFICIENT_RESOURCES));
    }
  }
  CHECK(bodies->load() == 0);
  CHECK_FALSE(f.file->HasPostedIo(nullptr));
  REQUIRE(f.file->FlushQueued([results](X_STATUS status) { results->Record(4, status); }) == X_STATUS_SUCCESS);
  REQUIRE(results->Wait(4));
  f.CheckNoCompletion(); f.CheckBytes();
}

TEST_CASE("Unstarted cancellation attempts later owners after an earlier completion throws",
          "[kernel][file_control][host_io][cancel][owners]") {
  FileInfoFixture f;
  auto gate = std::make_shared<FileQueueGate>();
  FileQueueGateRelease release{gate};
  auto results = std::make_shared<FileQueueResults>();
  f.file->PostIo(nullptr, [gate](bool) { gate->Enter(); });
  REQUIRE(gate->Wait(1));
  f.file->PostIo(nullptr, [results](bool cancelled) {
    results->Record(1, cancelled ? X_STATUS_CANCELLED : X_STATUS_SUCCESS);
    throw std::runtime_error("cancelled owner");
  });
  f.file->PostIo(nullptr, [results](bool cancelled) {
    results->Record(2, cancelled ? X_STATUS_CANCELLED : X_STATUS_SUCCESS);
  });
  f.file->CancelPostedIo(nullptr);
  gate->Release();
  REQUIRE(results->Wait(2));
  { std::lock_guard lock(results->mutex);
    REQUIRE(results->rows.size() == 2);
    CHECK(results->rows[0] == std::make_pair(1u, X_STATUS_CANCELLED));
    CHECK(results->rows[1] == std::make_pair(2u, X_STATUS_CANCELLED));
  }
  f.CheckNoCompletion(); f.CheckBytes();
}

TEST_CASE("Actual queued shutdown continues independent file and scheduler drop owners after ordinary failure",
          "[kernel][file_control][host_io][scheduler_drop][owners]") {
  FileInfoFixture f;
  auto* scheduler = f.emulator->kernel_state()->guest_scheduler();
  using Class = GuestScheduler::BlockingCallClass;
  using Reason = GuestScheduler::HostCallDropReason;
  auto gate = std::make_shared<FileQueueGate>();
  FileQueueGateRelease release{gate};
  for (unsigned i = 0; i < 4; ++i) {
    REQUIRE(scheduler->PostHostCall([gate] { gate->Enter(); }, Class::kConcurrent));
  }
  REQUIRE(gate->Wait(4));
  auto results = std::make_shared<FileQueueResults>();
  auto bodies = std::make_shared<std::atomic<unsigned>>(0);
  f.file->PostIo(nullptr, [bodies](bool) { ++*bodies; }, [results] {
    results->Record(1, X_STATUS_CANCELLED);
    throw std::runtime_error("first file drop owner");
  });
  f.file->PostIo(nullptr, [bodies](bool) { ++*bodies; }, [results] {
    results->Record(2, X_STATUS_CANCELLED);
  });
  REQUIRE(scheduler->PostHostCall([bodies] { ++*bodies; }, Class::kConcurrent,
      [results](Reason reason) {
        results->Record(3, reason == Reason::kQueuedShutdown ? X_STATUS_CANCELLED : X_STATUS_UNSUCCESSFUL);
        throw std::runtime_error("first scheduler drop owner");
      }));
  REQUIRE(scheduler->PostHostCall([bodies] { ++*bodies; }, Class::kConcurrent,
      [results](Reason reason) {
        results->Record(4, reason == Reason::kQueuedShutdown ? X_STATUS_CANCELLED : X_STATUS_UNSUCCESSFUL);
      }));
  std::atomic<bool> shutdown_failed{false};
  std::thread shutdown([&] {
    try { scheduler->Shutdown(); }
    catch (const std::exception&) { shutdown_failed.store(true); }
  });
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!scheduler->shutting_down() && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::yield();
  }
  const bool stopping = scheduler->shutting_down();
  gate->Release(); shutdown.join();
  REQUIRE(stopping); CHECK_FALSE(shutdown_failed.load());
  REQUIRE(results->Wait(4));
  { std::lock_guard lock(results->mutex);
    REQUIRE(results->rows.size() == 4);
    for (unsigned i = 0; i < 4; ++i) {
      CHECK(results->rows[i] == std::make_pair(i + 1, X_STATUS_CANCELLED));
    }
  }
  CHECK(bodies->load() == 0);
  CHECK_FALSE(f.file->HasPostedIo(nullptr));
  f.CheckNoCompletion(); f.CheckBytes();
}

namespace {
// A real HostPathFile/managed-root handle with a deliberately stale detached
// Entry. This models an immutable/managed retained view without path refresh;
// actual metadata and bytes still come from the owned backend, not a mock EOF.
class StaleConsumerEntry final : public vfs::HostPathEntry {
 public:
  StaleConsumerEntry(vfs::Device* device,vfs::Entry* parent,
      const std::filesystem::path& path,size_t stale)
      : HostPathEntry(device,parent,"sample.bin",path) {
    size_=stale;allocation_size_=stale;attributes_=vfs::kFileAttributeNormal;
  }
  void update() override {++updates;}
  uint32_t updates=0;
};
class RetainedConsumerFile final : public vfs::File {
 public:
  RetainedConsumerFile(std::unique_ptr<StaleConsumerEntry> entry,
      std::unique_ptr<filesystem::FileHandle> handle)
      : File(filesystem::FileAccess::kGenericRead|filesystem::FileAccess::kGenericWrite,entry.get()),
        entry_owner_(std::move(entry)),
        backend_(std::make_unique<vfs::HostPathFile>(file_access_,entry_owner_.get(),std::move(handle))) {}
  void Destroy() override {delete this;}
  X_STATUS QueryInformation(Information* output) const override {
    ++queries;
    if(XFAILED(query_failure)) {return query_failure;}
    return backend_->QueryInformation(output);
  }
  X_STATUS ReadSync(std::span<uint8_t> bytes,size_t offset,size_t* count) override {
    ++reads;if(before_read) {before_read();}
    return backend_->ReadSync(bytes,offset,count);
  }
  X_STATUS WriteSync(std::span<const uint8_t> bytes,size_t offset,size_t* count) override {
    ++writes;last_write_offset=offset;return backend_->WriteSync(bytes,offset,count);
  }
  X_STATUS Flush() override {return backend_->Flush();}
  uint32_t entry_updates() const {return entry_owner_->updates;}
  mutable uint32_t queries=0;uint32_t reads=0,writes=0;size_t last_write_offset=SIZE_MAX;
  X_STATUS query_failure=X_STATUS_SUCCESS;
  std::function<void()> before_read;
 private:
  // Backend must die first: its destructor releases this actual retained Entry.
  std::unique_ptr<StaleConsumerEntry> entry_owner_;
  std::unique_ptr<vfs::HostPathFile> backend_;
};
RetainedConsumerFile* InstallRetainedConsumerFile(FileInfoFixture& f,size_t stale) {
  f.file->ReleaseHandle();f.file.reset();
  const auto device=f.emulator->file_system()->AcquireDevice(FileInfoFixture::kMount);REQUIRE(device);
  auto* root=device->ResolvePath("");REQUIRE(root);
  std::error_code error;
  auto handle=f.storage->OpenFile("sample.bin",filesystem::FileAccess::kGenericRead|filesystem::FileAccess::kGenericWrite,error);
  REQUIRE(handle);REQUIRE_FALSE(error);
  auto entry=std::make_unique<StaleConsumerEntry>(device.get(),root,f.storage->path()/"sample.bin",stale);
  auto* backend=new RetainedConsumerFile(std::move(entry),std::move(handle));
  f.file=object_ref<XFile>(new XFile(f.emulator->kernel_state(),backend,false,false,true));
  return backend;
}
void BindConsumerCompletion(FileInfoFixture& f) {
  auto* completion=reinterpret_cast<X_FILE_COMPLETION_INFORMATION*>(f.Page()+0x100);
  completion->handle=f.port->handle();completion->key=0x719A5;
  REQUIRE(f.Call(f.file->handle(),f.kIosb,f.kInfo,sizeof(*completion),XFileCompletionInformation)==X_STATUS_SUCCESS);
}
void CheckConsumerCompletion(FileInfoFixture& f,X_STATUS status,uint32_t bytes) {
  XIOCompletion::IONotification notification{};
  REQUIRE(f.port->WaitForNotification(0,&notification));
  CHECK(notification.key_context==0x719A5);CHECK(notification.apc_context==0xC019);
  CHECK(notification.status==status);CHECK(notification.num_bytes==bytes);
  CHECK_FALSE(f.port->WaitForNotification(0,&notification));
  uint64_t timeout=0;CHECK(f.file->Wait(0,0,0,&timeout)==X_STATUS_SUCCESS);
  f.file->ResetWaitState();f.CheckNoCompletion();
}
struct ConsumerPhysicalWatch {
  Memory& memory;uint32_t physical=0,length=0;void* callback=nullptr;
  uint32_t providers=0,supplies=0,unlocked_writes=0,locked_writes=0;
  bool unlocked=false,in_guest_ram=false,supplied_unlocked=false;
  static constexpr uint32_t kAddress=0xE1000000;
  explicit ConsumerPhysicalWatch(Memory& m):memory(m) {
    const auto host_page=uint32_t(xe::memory::page_size());
    length=host_page*2;REQUIRE(length>=0x2000);
    memory.SetPhysicalAliasSkipHostProtect(true);
    REQUIRE(memory.LookupHeap(kAddress)->AllocFixed(kAddress,length,host_page,
        kMemoryAllocationReserve|kMemoryAllocationCommit,kMemoryProtectRead|kMemoryProtectWrite));
    physical=memory.GetPhysicalAddress(kAddress);REQUIRE(physical!=UINT32_MAX);
    std::memset(memory.TranslatePhysical(physical),0x31,length);
    callback=memory.RegisterPhysicalMemoryReadCallback([](void* raw,uint32_t start,uint32_t size,Memory::PhysicalAccess access) {
      auto& self=*static_cast<ConsumerPhysicalWatch*>(raw);
      const uint64_t begin=std::max(uint64_t(start),uint64_t(self.physical));
      const uint64_t end=std::min(uint64_t(start)+size,uint64_t(self.physical)+self.length);
      if(begin>=end || access==Memory::PhysicalAccess::kDiscard) {return;}
      const bool unlocked=!global_critical_region::is_held_by_current_thread();
      ++self.providers;self.unlocked|=unlocked;
      if(access==Memory::PhysicalAccess::kWrite) {
        if(unlocked) {++self.unlocked_writes;} else {++self.locked_writes;}
      }
      // One retained pending resolve owns this whole range. As with actual
      // ProvideResolveOutput, the first supply makes all its owned pages
      // current; the later locked kWrite recheck must not recopy old data
      // over bytes the real backend has just read into the physical alias.
      if(!self.in_guest_ram) {
        std::memset(self.memory.TranslatePhysical(self.physical),0xA5,self.length);
        self.in_guest_ram=true;++self.supplies;self.supplied_unlocked=unlocked;
      }
    },this);
    REQUIRE(callback);memory.EnablePhysicalMemoryAccessCallbacks(physical,length,true,true);
  }
  ~ConsumerPhysicalWatch() {
    if(callback) {memory.UnregisterPhysicalMemoryReadCallback(callback);}
    memory.LookupHeap(kAddress)->Release(kAddress);
  }
};
}
TEST_CASE("XFile append uses actual retained EOF and query refusal completes once without a backend write",
          "[kernel][file_control][retained_metadata][append][completion]") {
  FileInfoFixture f;auto* backend=InstallRetainedConsumerFile(f,1);
  BindConsumerCompletion(f);f.file->set_position(19);
  const std::array<uint8_t,3> suffix{0x71,0,0x93};std::memcpy(f.Page()+0x100,suffix.data(),suffix.size());
  util::IoTransferReceipt receipt;uint32_t count=0xDEADBEEF;
  SECTION("actual retained EOF overrides stale Entry without path refresh") {
    REQUIRE(f.file->Write(f.kInfo,uint32_t(suffix.size()),XFile::kEndOfFile,&count,0xC019,true,&receipt)==X_STATUS_SUCCESS);
    CHECK(count==suffix.size());CHECK(backend->queries==1);CHECK(backend->entry_updates()==0);
    CHECK(backend->writes==1);CHECK(backend->last_write_offset==f.original.size());
    CHECK(f.file->position()==f.original.size()+suffix.size());
    CHECK(receipt.invocation==HostCallOutcome::kReturned);CHECK(receipt.backend_entered);
    CHECK(receipt.backend_returned);CHECK(receipt.count_known);CHECK(receipt.bytes==suffix.size());
    std::array<uint8_t,9> actual{};size_t read=0;
    REQUIRE(f.file->file()->ReadSync(actual,0,&read)==X_STATUS_SUCCESS);REQUIRE(read==actual.size());
    CHECK(std::equal(f.original.begin(),f.original.end(),actual.begin()));
    CHECK(std::equal(suffix.begin(),suffix.end(),actual.begin()+f.original.size()));
    CheckConsumerCompletion(f,X_STATUS_SUCCESS,uint32_t(suffix.size()));
  }
  SECTION("retained query failure has zero transfer unchanged position and one normal failed completion") {
    backend->query_failure=X_STATUS_INVALID_HANDLE;
    REQUIRE(f.file->Write(f.kInfo,uint32_t(suffix.size()),XFile::kEndOfFile,&count,0xC019,true,&receipt)==X_STATUS_INVALID_HANDLE);
    CHECK(count==0);CHECK(f.file->position()==19);CHECK(backend->queries==1);
    CHECK(backend->entry_updates()==0);CHECK(backend->writes==0);
    CHECK(receipt.invocation==HostCallOutcome::kReturned);CHECK_FALSE(receipt.backend_entered);
    CHECK_FALSE(receipt.backend_returned);CHECK_FALSE(receipt.count_known);
    CHECK(std::memcmp(f.Page()+0x100,suffix.data(),suffix.size())==0);
    f.CheckBytes();CheckConsumerCompletion(f,X_STATUS_INVALID_HANDLE,0);
  }
}
TEST_CASE("XFile physical read prepares real partial-page edges from retained EOF and query refusal preserves guest output",
          "[kernel][file_control][retained_metadata][physical][completion]") {
  FileInfoFixture f;auto* backend=InstallRetainedConsumerFile(f,0);
  BindConsumerCompletion(f);f.file->set_position(23);
  ConsumerPhysicalWatch watch(*f.emulator->memory());bool provider_before_backend=false;
  backend->before_read=[&]{
    provider_before_backend=watch.in_guest_ram && watch.supplies==1 &&
        watch.unlocked_writes!=0 && watch.locked_writes==0;
  };
  util::IoTransferReceipt receipt;uint32_t count=0xDEADBEEF;
  const uint32_t requested=uint32_t(xe::memory::page_size());
  SECTION("real retained tiny EOF prepares neighbor page before actual backend read") {
    REQUIRE(f.file->Read(watch.kAddress,requested,0,&count,0xC019,true,&receipt)==X_STATUS_SUCCESS);
    CHECK(count==f.original.size());CHECK(backend->queries==1);CHECK(backend->reads==1);
    CHECK(provider_before_backend);CHECK(watch.providers!=0);CHECK(watch.unlocked);
    CHECK(watch.supplies==1);CHECK(watch.supplied_unlocked);
    CHECK(watch.unlocked_writes!=0);CHECK(watch.locked_writes!=0);
    const auto* actual=watch.memory.TranslatePhysical(watch.physical);
    CHECK(std::equal(f.original.begin(),f.original.end(),actual));
    CHECK(actual[f.original.size()]==0xA5);CHECK(actual[requested-1]==0xA5);
    CHECK(f.file->position()==f.original.size());CHECK(receipt.backend_entered);
    CHECK(receipt.backend_returned);CHECK(receipt.count_known);CHECK(receipt.bytes==f.original.size());
    CheckConsumerCompletion(f,X_STATUS_SUCCESS,uint32_t(f.original.size()));
  }
  SECTION("real query error reaches no edge provider no data backend and preserves every guest byte") {
    backend->query_failure=X_STATUS_INVALID_HANDLE;
    REQUIRE(f.file->Read(watch.kAddress,requested,0,&count,0xC019,true,&receipt)==X_STATUS_INVALID_HANDLE);
    CHECK(count==0);CHECK(f.file->position()==23);CHECK(backend->queries==1);CHECK(backend->reads==0);
    CHECK(watch.providers==0);CHECK_FALSE(provider_before_backend);
    CHECK(watch.supplies==0);CHECK_FALSE(watch.in_guest_ram);
    CHECK(watch.unlocked_writes==0);CHECK(watch.locked_writes==0);
    const auto* actual=watch.memory.TranslatePhysical(watch.physical);
    CHECK(std::all_of(actual,actual+watch.length,[](uint8_t value){return value==0x31;}));
    CHECK(receipt.invocation==HostCallOutcome::kReturned);CHECK_FALSE(receipt.backend_entered);
    CHECK_FALSE(receipt.backend_returned);CHECK_FALSE(receipt.count_known);
    CheckConsumerCompletion(f,X_STATUS_INVALID_HANDLE,0);
  }
}

}  // namespace xe::kernel::xboxkrnl::test
#endif  // XE_PLATFORM_LINUX
