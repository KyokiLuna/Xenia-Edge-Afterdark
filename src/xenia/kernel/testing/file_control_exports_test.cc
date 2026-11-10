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
#include <utility>
#include <cstring>
#include "third_party/catch/include/catch.hpp"
#include "xenia/base/cvar.h"
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
#include "xenia/vfs/devices/null_device.h"
#include "xenia/vfs/virtual_file_system.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_module.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_private.h"
#include "xenia/memory.h"

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
}  // namespace xe::kernel::xboxkrnl::test
#endif  // XE_PLATFORM_LINUX
