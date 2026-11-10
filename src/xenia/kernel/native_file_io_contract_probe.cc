/**
 * Xenia : Xbox 360 Emulator Research Project
 * Released under the BSD license - see LICENSE in the root for details.
 */
#include "xenia/kernel/native_file_io_contract_probe.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <functional>
#include <fstream>
#include <initializer_list>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>
#include "third_party/fmt/include/fmt/format.h"
#include "xenia/base/byte_order.h"
#include "xenia/base/filesystem.h"
#include "xenia/base/memory.h"
#include "xenia/base/platform.h"
#include "xenia/base/host_storage.h"
#include "xenia/base/mutex.h"
#include "xenia/cpu/export_resolver.h"
#include "xenia/cpu/processor.h"
#include "xenia/emulator.h"
#include "xenia/kernel/file_io_apc_fixture.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/native_import_domain.h"
#include "xenia/kernel/native_system_allocation_authority.h"
#include "xenia/kernel/util/guest_io_buffer.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xevent.h"
#include "xenia/kernel/xfile.h"
#include "xenia/kernel/xiocompletion.h"
#include "xenia/kernel/xthread.h"
#include "xenia/memory.h"
#include "xenia/vfs/devices/host_path_device.h"
#include "xenia/vfs/devices/host_path_entry.h"
#include "xenia/vfs/devices/host_path_file.h"
#include "xenia/vfs/folder_volume_control.h"
#include "xenia/vfs/virtual_file_system.h"

#if XE_PLATFORM_LINUX
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace xe::kernel {
namespace {
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
void Need(bool value, const char* reason) {
  if (!value) {
    throw std::runtime_error(reason);
  }
}
struct Backend {
  std::mutex mutex;
  std::condition_variable cv;
  bool gated = false, entered = false, released = false;
  bool timed_out = false, flush_error = false;
  std::atomic<uint32_t> flushes{0}, writes{0}, sequence{0};
  std::atomic<uint32_t> write_end{0}, flush_begin{0};
  void Release() {
    {
      std::lock_guard lock(mutex);
      released = true;
    }
    cv.notify_all();
  }
};
// Only host backend fault/gating is substituted. Real HostPathFile and XFile
// still perform permissions, queueing, status publication and notifications.
class ObservedHandle final : public filesystem::FileHandle {
 public:
  ObservedHandle(std::unique_ptr<filesystem::FileHandle> real,
                 std::shared_ptr<Backend> backend)
      : FileHandle(real->path()),
        real_(std::move(real)),
        b_(std::move(backend)) {}
  intptr_t native_handle() const override { return real_->native_handle(); }
  std::shared_ptr<const filesystem::HostStorageFileLease> storage_lease()
      const override {
    return real_->storage_lease();
  }
  std::optional<filesystem::FileHandleInfo> QueryInfo(
      std::error_code& e) const override {
    return real_->QueryInfo(e);
  }
  std::error_code LastError() const override { return real_->LastError(); }
  bool Read(size_t o, void* p, size_t n, size_t* count) override {
    return real_->Read(o, p, n, count);
  }
  bool Write(size_t o, const void* p, size_t n, size_t* count) override {
    const auto r = WriteResult(o, {static_cast<const uint8_t*>(p), n});
    *count = r.bytes;
    return !r.error;
  }
  filesystem::FileIoResult WriteResult(
      size_t o, std::span<const uint8_t> bytes) override {
    ++b_->writes;
    {
      std::unique_lock lock(b_->mutex);
      b_->entered = true;
      b_->cv.notify_all();
      if (b_->gated &&
          !b_->cv.wait_for(lock, 3s, [&] { return b_->released; })) {
        b_->timed_out = true;
        return {std::make_error_code(std::errc::timed_out), 0};
      }
    }
    auto r = real_->WriteResult(o, bytes);
    b_->write_end.store(++b_->sequence);
    return r;
  }
  bool SetLength(size_t n) override { return real_->SetLength(n); }
  bool SetTimes(uint64_t c, uint64_t a, uint64_t w) override {
    return real_->SetTimes(c, a, w);
  }
  void Flush() override { (void)FlushResult(); }
  std::error_code FlushResult() override {
    ++b_->flushes;
    b_->flush_begin.store(++b_->sequence);
    return b_->flush_error ? std::make_error_code(std::errc::io_error)
                           : real_->FlushResult();
  }

 private:
  std::unique_ptr<filesystem::FileHandle> real_;
  std::shared_ptr<Backend> b_;
};
class DiagnosticVolume final : public vfs::HostPathDevice {
 public:
  DiagnosticVolume(std::string mount,
                   std::shared_ptr<filesystem::HostStorageRoot> root,
                   bool queue)
      : HostPathDevice(mount, root->path(), false, false, "FATX", root, root,
                       ControlRole::kFolderVolume),
        queue_(queue) {}
  bool queues_all_requests() const override { return queue_; }
  filesystem::HostStorageSnapshot storage_space() const override {
    ++queries;
    auto s = HostPathDevice::storage_space();
    if (overflow.load() && s.available) {
      s.capacity_bytes = uint64_t(1) << 62;
    }
    return s;
  }
  std::atomic<bool> overflow{false};
  mutable std::atomic<uint32_t> queries{0};

 private:
  bool queue_;
};
// A real guest stack frame on the original current thread. This invokes the
// registered HLE trampoline. Its genuine I/O APC boundary may execute the
// separately checked leaf PPC fixture; this class never dispatches PPC itself.
class TrampolineFrame {
 public:
  TrampolineFrame(XThread& thread, std::span<const uint32_t> args)
      : memory_(*thread.memory()),
        context_(*thread.thread_state()->context()),
        sp_(context_.r[1]),
        lr_(context_.lr) {
    Need(args.size() <= 10 && sp_ <= UINT32_MAX && !(sp_ & 15) &&
             sp_ >= uint64_t(thread.stack_limit()) + kCheckedStackBytes &&
             sp_ <= thread.stack_base(),
         "real guest stack extent");
    frame_ = uint32_t(sp_) - 0x80;
    saved_stack_ = uint32_t(sp_) - kCheckedStackBytes;
    // Includes ProcessApcList's32-byte scratch and Processor::Execute's176-byte
    // nested frame below our actual SP, plus the owned40-byte leaf's ABI room.
    Need(memory_.QueryRangeAccess(saved_stack_, kCheckedStackBytes) ==
             memory::PageAccess::kReadWrite,
         "real guest stack and nested APC extent writable");
    for (size_t i = 0; i < gprs_.size(); ++i) {
      gprs_[i] = context_.r[3 + i];
    }
    std::memcpy(stack_.data(), memory_.TranslateVirtual(saved_stack_),
                stack_.size());
    // Xbox guest unwind uses a BE32 backchain and saved LR at entrySP-8.
    xe::store_and_swap<uint32_t>(memory_.TranslateVirtual(frame_),
                                 uint32_t(sp_));
    xe::store_and_swap<uint32_t>(memory_.TranslateVirtual(uint32_t(sp_) - 8),
                                 uint32_t(lr_));
    context_.r[1] = frame_;
    for (size_t i = 0; i < args.size(); ++i) {
      if (i < 8) {
        context_.r[3 + i] = args[i];
      } else {
        xe::store_and_swap<uint32_t>(
            memory_.TranslateVirtual(frame_ + 0x54 + uint32_t(i - 8) * 8),
            args[i]);
      }
    }
  }
  ~TrampolineFrame() { Restore(); }
  void Restore() {
    if (restored_) {
      return;
    }
    std::memcpy(memory_.TranslateVirtual(saved_stack_), stack_.data(),
                stack_.size());
    for (size_t i = 0; i < gprs_.size(); ++i) {
      context_.r[3 + i] = gprs_[i];
    }
    context_.r[1] = sp_;
    context_.lr = lr_;
    restored_ = true;
  }
  bool IsRestored() const {
    if (!restored_ || context_.r[1] != sp_ || context_.lr != lr_ ||
        std::memcmp(memory_.TranslateVirtual(saved_stack_), stack_.data(),
                    stack_.size())) {
      return false;
    }
    for (size_t i = 0; i < gprs_.size(); ++i) {
      if (context_.r[3 + i] != gprs_[i]) {
        return false;
      }
    }
    return true;
  }

 private:
  Memory& memory_;
  cpu::ppc::PPCContext& context_;
  uint64_t sp_, lr_;
  static constexpr uint32_t kCheckedStackBytes = 0x400;
  uint32_t frame_ = 0, saved_stack_ = 0;
  std::array<uint64_t, 8> gprs_{};
  std::array<uint8_t, kCheckedStackBytes> stack_{};
  bool restored_ = false;
};
struct ApcIoMemory {
  KernelState* kernel = nullptr;
  uint32_t allocation = 0;
  ~ApcIoMemory() {
    // Kernel's resident-resource boundary is after actual CPU/scheduler stop,
    // while Memory remains alive. A queued APC may still reference this IOSB.
    if (allocation) {
      kernel->memory()->SystemHeapFree(allocation);
    }
  }
};
struct Mounted {
  std::string name;
  std::shared_ptr<filesystem::HostStorageRoot> root;
  std::shared_ptr<DiagnosticVolume> device;
  bool registered = true;
};
struct Fixture {
  KernelState* kernel;
  object_ref<XThread> issuer;
  std::shared_ptr<filesystem::HostStorageRoot> parent;
  uint32_t allocation = 0;
  bool apc_memory_resident = false;
  std::vector<Mounted> mounts;
  std::vector<object_ref<XFile>> files;
  std::vector<object_ref<XEvent>> events;
  std::vector<object_ref<XIOCompletion>> ports;
  std::vector<std::shared_ptr<Backend>> backends;
  std::mutex backend_mutex;
  std::atomic<bool> closing{false};
  bool cleaned = false;
  explicit Fixture(KernelState* k, XThread& t)
      : kernel(k), issuer(retain_object(&t)) {
    mounts.reserve(6);  // Four original mounts plus two overwrite children.
    files.reserve(8);
    backends.reserve(5);
  }
  void Close() {
    closing.store(true);
    std::lock_guard lock(backend_mutex);
    for (const auto& b : backends) {
      b->Release();
    }
  }
  bool Idle() {
    for (auto& f : files) {
      if (f->HasPostedIo(issuer.get())) {
        return false;
      }
    }
    return true;
  }
  void RetainIoMemoryForApc() {
    if (apc_memory_resident) {
      return;
    }
    Need(allocation && issuer && XThread::IsInThread(issuer.get()),
         "actual caller owns APC IOSB allocation");
    auto owned = std::make_shared<ApcIoMemory>();
    owned->kernel = kernel;
    // If registration throws, allocation stays owned here. Once registration
    // succeeds the only durable strong owner is Kernel, never this wrapper.
    kernel->RetainNativeResourceUntilTeardown(owned);
    owned->allocation = allocation;
    apc_memory_resident = true;
  }
  bool Cleanup() {
    if (cleaned) {
      return true;
    }
    Close();
    for (auto& f : files) {
      f->CancelPostedIo(issuer.get());
    }
    if (!Idle()) {
      return false;
    }
    const auto release = [&](auto& object) {
      auto lock = global_critical_region::AcquireDirect();
      const auto handle = object->handle();
      Need(object->ReleaseHandle(), "release owned diagnostic handle");
      Need(!kernel->object_table()->LookupObject<XObject>(handle),
           "closed diagnostic handle absent from table");
    };
    for (auto& f : files) {
      release(f);
    }
    files.clear();
    for (auto& e : events) {
      release(e);
    }
    events.clear();
    for (auto& p : ports) {
      release(p);
    }
    ports.clear();
    for (auto& m : mounts) {
      if (m.registered) {
        Need(kernel->emulator()->file_system()->UnregisterDevice(m.name),
             "fixture mount detach");
        m.registered = false;
      }
    }
    mounts.clear();
    if (allocation) {
      const auto released = std::exchange(allocation, 0);
      if (!apc_memory_resident) {
        kernel->memory()->SystemHeapFree(released);
      }
    }
    issuer.reset();
    cleaned = true;
    return true;
  }
  uint32_t Addr(uint32_t offset) const { return allocation + offset; }
  uint32_t Word(uint32_t offset) {
    std::vector<uint8_t> b(4);
    Need(
        XSUCCEEDED(util::ReadGuestIoBuffer(*kernel->memory(), Addr(offset), b)),
        "read fixture guest word");
    return xe::load_and_swap<uint32_t>(b.data());
  }
  void Fill(uint32_t offset, uint32_t count, uint8_t value) {
    std::vector<uint8_t> b(count, value);
    Need(XSUCCEEDED(
             util::WriteGuestIoBuffer(*kernel->memory(), Addr(offset), b)),
         "write fixture guest bytes");
  }
  bool Filled(uint32_t offset, uint32_t count, uint8_t value) {
    std::vector<uint8_t> b(count);
    Need(
        XSUCCEEDED(util::ReadGuestIoBuffer(*kernel->memory(), Addr(offset), b)),
        "read fixture canary");
    return std::ranges::all_of(b, [&](auto c) { return c == value; });
  }
  X_STATUS Call(uint16_t ordinal, const char* name,
                std::initializer_list<uint32_t> args) {
    Need(!closing.load() && XThread::GetCurrentThread() == issuer.get() &&
             kernel_state() == kernel &&
             (!global_critical_region::is_held_by_current_thread() ||
              ordinal == 0xD9 || ordinal == 0xDB),
         "actual export caller");
    auto* entry = kernel->processor()->export_resolver()->GetExportByOrdinal(
        "xboxkrnl.exe", ordinal);
    Need(entry && entry->get_type() == cpu::Export::Type::kFunction &&
             entry->function_data.trampoline &&
             std::string_view(entry->name) == name,
         "actual registered export");
    TrampolineFrame frame(*issuer, {args.begin(), args.size()});
    entry->function_data.trampoline(issuer->thread_state()->context());
    const auto status = X_STATUS(issuer->thread_state()->context()->r[3]);
    frame.Restore();
    Need(frame.IsRestored(), "actual caller ABI frame restored after export");
    return status;
  }
  bool Poll(const std::function<bool()>& done) {
    const auto end = Clock::now() + 3s;
    while (!done()) {
      if (closing.load() || Clock::now() >= end) {
        return false;
      }
      issuer->Delay(0, 0, uint64_t(-10000ll));
    }
    return true;
  }
  X_STATUS FileWait(XFile& f) {
    uint64_t timeout = 0;
    return f.Wait(0, 0, 0, &timeout);
  }
  object_ref<XEvent> Event() {
    auto e = object_ref<XEvent>(new XEvent(kernel));
    e->Initialize(true, true);
    events.push_back(e);
    return e;
  }
  Mounted& Mount(const char* child, bool queue) {
    Need(!parent->Create(child, true), "create exclusive fixture child");
    std::error_code ec;
    auto root = parent->Subroot(child, false, ec);
    Need(bool(root), "fixture child capability");
    const std::array<uint8_t, 4> initial{0, 0, 0, 0};
    Need(!root->WriteNewFile("probe.bin", initial).error, "fixture seed file");
    auto name = std::string("\\Device\\HddIoDiagnostic_") + child;
    auto volume = std::make_unique<DiagnosticVolume>(name, root, queue);
    Need(volume->Initialize(), "fixture volume initialize");
    auto* ptr = volume.get();
    Need(kernel->emulator()->file_system()->RegisterDevice(std::move(volume)),
         "fixture volume registration");
    auto owned = std::dynamic_pointer_cast<DiagnosticVolume>(
        ptr->weak_from_this().lock());
    Need(bool(owned), "fixture tree owner");
    mounts.push_back({std::move(name), std::move(root), std::move(owned)});
    return mounts.back();
  }
  object_ref<XFile> File(
      Mounted& m, bool synchronous, bool root = false,
      uint32_t rights = filesystem::FileAccess::kGenericRead |
                        filesystem::FileAccess::kGenericWrite,
      std::shared_ptr<Backend> b = {}) {
    auto* entry = dynamic_cast<vfs::HostPathEntry*>(
        m.device->ResolvePath(root ? "" : "probe.bin"));
    Need(entry != nullptr, "real fixture entry");
    vfs::File* file = nullptr;
    if (b) {
      std::error_code ec;
      auto h = m.root->OpenFile("probe.bin", rights, ec);
      Need(bool(h), "real observed host handle");
      file = new vfs::HostPathFile(
          rights, entry, std::make_unique<ObservedHandle>(std::move(h), b));
      {
        std::lock_guard lock(backend_mutex);
        if (closing.load()) {
          b->Release();
        }
        backends.push_back(std::move(b));
      }
    } else {
      Need(XSUCCEEDED(entry->Open(rights, &file)) && file,
           "real fixture volume/file open");
    }
    auto result =
        object_ref<XFile>(new XFile(kernel, file, synchronous, false, true));
    files.push_back(result);
    return result;
  }
};
class Probe final : public NativeSystemUiDiagnosticObservation {
 public:
  Probe(KernelState* k,
        std::shared_ptr<NativeSystemUiDiagnosticObservation> inner,
        FILE* report)
      : kernel_(k), inner_(std::move(inner)), report_(report, &std::fclose) {}
  gpu::GpuBackendStopProof backend_stop_proof() const override {
    return inner_->backend_stop_proof();
  }
  GuestRuntime::ExecutionResult PrepareBeforeWorkers(
      const NativeSystemUiServiceContext& c, XThread& t) override {
    return inner_->PrepareBeforeWorkers(c, t);
  }
  GuestRuntime::ExecutionResult ObserveNativePhase(
      NativeSystemUiStartupPhase p, const NativeSystemUiServiceContext& c,
      XThread& t) override {
    return inner_->ObserveNativePhase(p, c, t);
  }
  GuestRuntime::ExecutionResult ObserveAfterNativeBoot(
      const NativeSystemUiServiceContext& c, XThread& t) override {
    try {
      Need(!closing_.load() && c.kernel == kernel_ && XThread::IsInThread(&t) &&
               XThread::GetCurrentThread() == &t && t.thread_state() &&
               cpu::ThreadState::Get() == t.thread_state() &&
               t.thread_state()->context() &&
               t.thread_state()->context()->kernel_state == kernel_ &&
               t.kernel_state() == kernel_ && kernel_state() == kernel_ &&
               t.native_frame_thread_identity().process_type ==
                   X_PROCTYPE_SYSTEM &&
               c.domain && c.domain->active() && c.allocation &&
               c.allocation->IsAccepting() &&
               c.allocation->MatchesDomain(c.domain) &&
               kernel_->guest_scheduler() && kernel_->object_table() &&
               !global_critical_region::is_held_by_current_thread(),
           "live original SYSTEM/cohort context");
      auto f = std::make_shared<Fixture>(kernel_, t);
      {
        std::lock_guard lock(mutex_);
        Need(!fixture_, "diagnostic runs once");
        fixture_ = f;
      }
      std::error_code ec;
      auto storage = filesystem::HostStorageRoot::Open(
          kernel_->emulator()->storage_root(), false, ec);
      Need(bool(storage), "owned isolated storage capability");
      Need(!storage->Create("hdd-io-contract-fixture", true),
           "fresh exclusive diagnostic directory");
      f->parent = storage->Subroot("hdd-io-contract-fixture", false, ec);
      Need(bool(f->parent), "owned fixture root");
      f->allocation = kernel_->memory()->SystemHeapAlloc(12288, 4096);
      Need(f->allocation != 0, "bounded owned guest buffers");
      Write("context", true,
            fmt::format("\"thread_id\":{},\"domain_id\":{},\"root_hmodule\":{},"
                        "\"root_generation\":{},\"guest_bytes\":12288,\"call_"
                        "kind\":\"registered_HLE_trampoline_actual_context\"",
                        t.thread_id(), c.domain->id(), c.domain->root().hmodule,
                        c.domain->root().generation));
      Run(*f);
      Need(f->Poll([&] { return f->Idle(); }), "actual I/O callbacks drain");
      Need(f->Cleanup(), "fixture handles/buffers cleanup");
      Write("all_implemented_cases", true,
            "\"fixture_cleanup_complete\":true,"
            "\"APC_execution\":\"actual_inline_kernel_and_alertable_user_IO\","
            "\"io_memory_retained_until_kernel_CPU_stop\":true,"
            "\"production_ready\":false");
    } catch (const std::exception& e) {
      CloseExternalRequests();
      try {
        Write("diagnostic_failure", false,
              fmt::format("\"reason\":\"{}\"", Escape(e.what())));
      } catch (...) {
      }
      return {false, 0x80004005,
              "File I/O contract diagnostic failed; see separate report"};
    }
    return inner_->ObserveAfterNativeBoot(c, t);
  }
  void CloseExternalRequests() noexcept override {
    closing_.store(true);
    std::shared_ptr<Fixture> f;
    {
      std::lock_guard lock(mutex_);
      f = fixture_;
    }
    if (f) {
      f->Close();
    }
    inner_->CloseExternalRequests();
  }
  void BeginStopTransport() noexcept override {
    CloseExternalRequests();
    inner_->BeginStopTransport();
  }
  bool DrainCallbacksBeforeCpuStop() noexcept override {
    try {
      std::shared_ptr<Fixture> f;
      {
        std::lock_guard lock(mutex_);
        f = fixture_;
      }
      if (f) {
        f->Close();
        for (auto& file : f->files) {
          file->CancelPostedIo(f->issuer.get());
        }
        const auto end = Clock::now() + 5s;
        while (!f->Idle() && Clock::now() < end) {
          std::this_thread::sleep_for(1ms);
        }
        if (!f->Cleanup()) {
          return false;
        }
      }
      return inner_->DrainCallbacksBeforeCpuStop();
    } catch (...) {
      return false;
    }
  }
  bool RetireAfterKernelStop() noexcept override {
    std::lock_guard lock(mutex_);
    if (fixture_ && !fixture_->cleaned) {
      return false;
    }
    return inner_->RetireAfterKernelStop();
  }

 private:
  static std::string Escape(std::string_view s) {
    std::string r;
    for (char c : s) {
      if (c == '"' || c == '\\') {
        r += '\\';
      }
      if (uint8_t(c) >= 32) {
        r += c;
      }
    }
    return r;
  }
  void Write(const char* name, bool pass, std::string fields = {}) {
    const auto text = fmt::format(
        "{{\"event\":\"file_io_contract\",\"case\":\"{}\","
        "\"passed\":{}{}{} }}\n",
        name, pass, fields.empty() ? "" : ",", fields);
    std::lock_guard lock(report_mutex_);
    Need(std::fwrite(text.data(), 1, text.size(), report_.get()) ==
                 text.size() &&
             !std::fflush(report_.get()),
         "exclusive diagnostic report write");
  }
  void Run(Fixture& f);
  void RunOverwriteCases(Fixture& f);
  KernelState* kernel_;
  std::shared_ptr<NativeSystemUiDiagnosticObservation> inner_;
  std::unique_ptr<FILE, decltype(&std::fclose)> report_;
  std::mutex mutex_, report_mutex_;
  std::shared_ptr<Fixture> fixture_;
  std::atomic<bool> closing_{false};
};
void Probe::Run(Fixture& f) {
  constexpr uint32_t kIo = 0, kWriteIo = 16, kOut = 64, kBytes = 128,
                     kOffset = 256;
  constexpr uint32_t kIoCanary = 0x5A5A5A5A;
  auto& synchronous = f.Mount("sync", false);
  auto error_backend = std::make_shared<Backend>();
  error_backend->flush_error = true;
  auto sync = f.File(synchronous, true, false,
                     filesystem::FileAccess::kGenericWrite, error_backend);
  f.Fill(kIo, 8, 0x5A);
  sync->NotifyCompletion(X_STATUS_SUCCESS, 0, 0);
  const auto flush =
      f.Call(0xDB, "NtFlushBuffersFile", {sync->handle(), f.Addr(kIo)});
  Need(flush == X_STATUS_UNSUCCESSFUL && f.Word(kIo) == flush &&
           f.Word(kIo + 4) == 0 && error_backend->flushes == 1 &&
           f.FileWait(*sync) == X_STATUS_SUCCESS,
       "sync flush EIO completion");
  Write(
      "sync_flush_EIO", true,
      fmt::format(
          "\"returned_status\":{},\"iosb_status\":{},\"bytes\":0,\"backend_"
          "attempts\":1,\"actual_file_wait\":0,\"injected_host_error\":\"EIO\"",
          flush, f.Word(kIo)));

  auto& control = f.Mount("control", false);
  auto volume =
      f.File(control, true, true, filesystem::FileAccess::kGenericRead);
  auto no_read = f.File(control, true, true, 0);
  auto event = f.Event();
  auto ioctl = [&](uint32_t handle, uint32_t event_handle, uint32_t iosb,
                   uint32_t output, uint32_t length, uint32_t apc = 0,
                   uint32_t cookie = 0,
                   uint32_t code = vfs::kFolderVolumeDriveGeometry) {
    return f.Call(
        0xD9, "NtDeviceIoControlFile",
        {handle, event_handle, apc, cookie, iosb, code, 0, 0, output, length});
  };
  auto rejection = [&](const char* name, X_STATUS expected,
                       const std::function<X_STATUS()>& issue) {
    f.Fill(kIo, 8, 0x5A);
    f.Fill(kOut, 16, 0xA5);
    event->Set(0, false);
    volume->NotifyCompletion(X_STATUS_SUCCESS, 0, 0);
    const auto queries = control.device->queries.load();
    const auto status = issue();
    uint32_t type = 0, signal = 0;
    event->Query(&type, &signal);
    Need(status == expected && f.Word(kIo) == kIoCanary &&
             f.Word(kIo + 4) == kIoCanary && f.Filled(kOut, 16, 0xA5) &&
             signal == 1 && f.FileWait(*volume) == X_STATUS_SUCCESS &&
             control.device->queries == queries,
         "pre-admission state preservation");
    Write(name, true,
          fmt::format("\"returned_status\":{},\"iosb_unchanged\":true,\"output_"
                      "unchanged\":true,\"event_unchanged\":true,\"file_wait_"
                      "unchanged\":true,\"backend_attempts\":0",
                      status));
  };
  rejection("invalid_handle_rejected", X_STATUS_INVALID_HANDLE, [&] {
    return ioctl(0, event->handle(), f.Addr(kIo), f.Addr(kOut), 8);
  });
  rejection("wrong_event_type_rejected", X_STATUS_INVALID_HANDLE, [&] {
    return ioctl(volume->handle(), volume->handle(), f.Addr(kIo), f.Addr(kOut),
                 8);
  });
  rejection("short_output_rejected", X_STATUS_BUFFER_TOO_SMALL, [&] {
    return ioctl(volume->handle(), event->handle(), f.Addr(kIo), f.Addr(kOut),
                 7);
  });
  rejection("read_rights_rejected", X_STATUS_ACCESS_DENIED, [&] {
    return ioctl(no_read->handle(), event->handle(), f.Addr(kIo), f.Addr(kOut),
                 16, 0, 0, vfs::kFolderVolumePartitionInfo);
  });
  rejection("outer_GCRC_rejected", X_STATUS_INVALID_DEVICE_REQUEST, [&] {
    auto lock = global_critical_region::AcquireDirect();
    return ioctl(volume->handle(), event->handle(), f.Addr(kIo), f.Addr(kOut),
                 8);
  });
  auto* heap = kernel_->memory()->LookupHeap(f.Addr(4096));
  Need(heap != nullptr, "owned guard heap");
  Need(heap->Protect(f.Addr(4096), 4096, kMemoryProtectNoAccess),
       "owned diagnostic guard page");
  try {
    rejection("guard_crossing_rejected", X_STATUS_ACCESS_VIOLATION, [&] {
      return ioctl(volume->handle(), event->handle(), f.Addr(kIo), f.Addr(4092),
                   8);
    });
  } catch (...) {
    heap->Protect(f.Addr(4096), 4096, kMemoryProtectRead | kMemoryProtectWrite);
    throw;
  }
  Need(heap->Protect(f.Addr(4096), 4096,
                     kMemoryProtectRead | kMemoryProtectWrite),
       "guard restoration");
  f.Fill(kIo, 8, 0x5A);
  f.Fill(kOut, 16, 0xA5);
  event->Reset();
  uint32_t event_type = 0, event_signal = 0;
  event->Query(&event_type, &event_signal);
  Need(event_signal == 0, "admitted control event starts unsignalled");
  control.device->overflow.store(true);
  const auto overflow =
      ioctl(volume->handle(), event->handle(), f.Addr(kIo), f.Addr(kOut), 8);
  event->Query(&event_type, &event_signal);
  Need(overflow == X_STATUS(0xC0000095) && f.Word(kIo) == overflow &&
           f.Word(kIo + 4) == 0 && f.Filled(kOut, 16, 0xA5) &&
           event_signal == 1 && f.FileWait(*volume) == X_STATUS_SUCCESS,
       "admitted geometry overflow completion");
  Write(
      "geometry_backend_overflow", true,
      fmt::format("\"returned_status\":{},\"iosb_status\":{},\"bytes\":0,"
                  "\"output_unchanged\":true,\"actual_event_signalled\":true,"
                  "\"actual_file_wait\":0,\"oversized_volume_test_view\":true",
                  overflow, f.Word(kIo)));

  auto posted = [&](const char* child, bool cancel) {
    auto& mounted = f.Mount(child, true);
    auto backend = std::make_shared<Backend>();
    backend->gated = true;
    auto file = f.File(mounted, false, false,
                       filesystem::FileAccess::kGenericWrite, backend);
    f.Fill(kWriteIo, 8, 0x5A);
    f.Fill(kIo, 8, 0x5A);
    f.Fill(kBytes, 4, 0x37);
    f.Fill(kOffset, 8, 0);
    const auto write = f.Call(0xFF, "NtWriteFile",
                              {file->handle(), 0, 0, 0, f.Addr(kWriteIo),
                               f.Addr(kBytes), 4, f.Addr(kOffset)});
    Need(write == X_STATUS_PENDING && f.Poll([&] {
      std::lock_guard lock(backend->mutex);
      return backend->entered;
    }),
         "actual posted write entered backend");
    const auto pend =
        f.Call(0xDB, "NtFlushBuffersFile", {file->handle(), f.Addr(kIo)});
    Need(pend == X_STATUS_PENDING && f.Word(kIo) == X_STATUS_PENDING &&
             backend->flushes == 0,
         "flush queued behind gated write");
    X_STATUS cancelled = X_STATUS_SUCCESS;
    std::atomic<bool> cancellation_observed{false};
    if (cancel) {
      // NtCancelIoFile waits for its already running write. This bounded
      // controller releases that backend only after observing the real queued
      // flush's CANCELLED IOSB, or at deadline on any failure.
      std::thread release([&] {
        try {
          const auto end = Clock::now() + 2s;
          while (Clock::now() < end && !f.closing.load()) {
            if (f.Word(kIo) == X_STATUS_CANCELLED) {
              cancellation_observed.store(true);
              break;
            }
            std::this_thread::sleep_for(1ms);
          }
        } catch (...) {
        }
        backend->Release();
      });
      try {
        cancelled = f.Call(0x2B8, "NtCancelIoFile", {file->handle()});
      } catch (...) {
        backend->Release();
        release.join();
        throw;
      }
      release.join();
    } else {
      backend->Release();
    }
    Need(f.Poll([&] { return f.Word(kIo) != X_STATUS_PENDING && f.Idle(); }),
         "posted requests actual callback drain");
    const auto status = f.Word(kIo);
    Need(!backend->timed_out && backend->writes == 1 &&
             f.Word(kWriteIo) == X_STATUS_SUCCESS &&
             f.Word(kWriteIo + 4) == 4 && f.FileWait(*file) == X_STATUS_SUCCESS,
         "posted write and wait completion");
    std::vector<uint8_t> persisted;
    Need(!mounted.root->ReadFile("probe.bin", 4, persisted) &&
             persisted == std::vector<uint8_t>(4, 0x37),
         "reopened actual host bytes");
    if (cancel) {
      Need(cancelled == X_STATUS_SUCCESS && cancellation_observed.load() &&
               status == X_STATUS_CANCELLED && f.Word(kIo + 4) == 0 &&
               backend->flushes == 0,
           "queued flush actual cancellation");
    } else {
      Need(status == X_STATUS_SUCCESS && f.Word(kIo + 4) == 0 &&
               backend->flushes == 1 &&
               backend->write_end.load() < backend->flush_begin.load(),
           "posted write-before-flush order");
    }
    Write(
        cancel ? "actual_queued_flush_cancel"
               : "actual_posted_write_flush_order",
        true,
        fmt::format("\"write_returned\":{},\"flush_returned\":{},\"flush_iosb_"
                    "status\":{},\"write_end_sequence\":{},\"flush_begin_"
                    "sequence\":{},\"flush_backend_attempts\":{},\"reopened_"
                    "bytes_match\":true,\"callbacks_drained\":true",
                    write, pend, status, backend->write_end.load(),
                    backend->flush_begin.load(), backend->flushes.load()));
  };
  posted("posted", false);
  posted("cancel", true);

  auto async_volume =
      f.File(control, false, true, filesystem::FileAccess::kGenericRead);
  auto port = object_ref<XIOCompletion>(new XIOCompletion(kernel_));
  f.ports.push_back(port);
  constexpr uint32_t key = 0x12345678, cookie = 0x76543210;
  Need(async_volume->RegisterIOCompletionPort(key, port),
       "real completion port binding");
  f.Fill(kIo, 8, 0x5A);
  f.Fill(kOut, 16, 0xA5);
  const auto port_pending =
      ioctl(async_volume->handle(), 0, f.Addr(kIo), f.Addr(kOut), 8, 0, cookie);
  Need(port_pending == X_STATUS_PENDING &&
           f.Poll([&] { return f.Word(kIo) != X_STATUS_PENDING && f.Idle(); }),
       "actual port-bound control completes");
  XIOCompletion::IONotification packet{};
  Need(port->WaitForNotification(0, &packet) && packet.key_context == key &&
           packet.apc_context == cookie &&
           packet.status == X_STATUS(0xC0000095) && packet.num_bytes == 0 &&
           f.Word(kIo) == packet.status && f.Word(kIo + 4) == 0 &&
           f.Filled(kOut, 16, 0xA5) &&
           f.FileWait(*async_volume) == X_STATUS_SUCCESS &&
           !port->WaitForNotification(0, &packet),
       "actual completion port error packet and no duplicate");
  Write("actual_completion_port_failure", true,
        fmt::format("\"returned_status\":{},\"packet_key\":{},\"packet_"
                    "context\":{},\"packet_status\":{},\"packet_bytes\":0,\"no_"
                    "duplicate_packet\":true",
                    port_pending, key, cookie, f.Word(kIo)));
  f.Fill(kIo, 8, 0x5A);
  f.Fill(kOut, 16, 0xA5);
  async_volume->NotifyCompletion(X_STATUS_SUCCESS, 0, 0);
  const auto denied = ioctl(async_volume->handle(), 0, f.Addr(kIo),
                            f.Addr(kOut), 8, f.Addr(512), cookie);
  Need(denied == X_STATUS_INVALID_PARAMETER && f.Word(kIo) == kIoCanary &&
           f.Filled(kOut, 16, 0xA5) &&
           f.FileWait(*async_volume) == X_STATUS_SUCCESS &&
           !port->WaitForNotification(0, &packet),
       "port plus APC rejected before admission");
  Write("port_with_APC_rejected", true,
        "\"iosb_output_file_wait_unchanged\":true,\"no_packet\":true,"
        "\"callback_execution_not_attempted\":true");
  // Completion dispatches the checked PPC code; no direct Execute or host
  // invocation-count write is permitted in this diagnostic.
  f.RetainIoMemoryForApc();
  Need(!volume->HasCompletionPort(), "actual APC file is not port-bound");
  const auto* pcr =
      kernel_->memory()->TranslateVirtual<X_KPCR*>(f.issuer->pcr_ptr());
  const auto* current = f.issuer->guest_object<X_KTHREAD>();
  Need(pcr->current_irql == 0 && !current->apc_disable_count &&
           !current->executing_kernel_apc,
       "actual kernel APC boundary is unmasked without identity mutation");
  const auto prepare_apc = [&]() {
    std::string error;
    auto fixture = FileIoApcFixture::Create(kernel_, *f.issuer, &error);
    Need(bool(fixture), "checked owned PPC APC fixture creation");
    const auto before = fixture->ReadAfterReturn(*f.issuer);
    Need(before && before->iosb_address == 0 && before->reserved == 0 &&
             before->status == 0 && before->information == 0 &&
             before->invocations == 0,
         "new guarded APC observation starts untouched");
    return fixture;
  };
  constexpr std::string_view kApcSourceSha256 =
      "6ad45eca0fb239ec30124a2c9affc6d3fb3ae307c964eb5e7dbc647c6eeafc63";
  auto kernel_apc = prepare_apc();
  const auto kernel_routine = kernel_apc->CheckedRoutine(*f.issuer);
  const auto kernel_context = kernel_apc->CheckedContext(*f.issuer);
  Need(kernel_routine && !(*kernel_routine & 3) && kernel_context,
       "actual checked even APC source and context");
  f.Fill(kIo, 8, 0x5A);
  f.Fill(kOut, 16, 0xA5);
  event->Reset();
  const auto kernel_return =
      ioctl(volume->handle(), event->handle(), f.Addr(kIo), f.Addr(kOut), 8,
            *kernel_routine, *kernel_context);
  const auto kernel_observed = kernel_apc->ReadAfterReturn(*f.issuer);
  Need(kernel_return == X_STATUS(0xC0000095) && kernel_observed &&
           kernel_observed->iosb_address == f.Addr(kIo) &&
           kernel_observed->reserved == 0 &&
           kernel_observed->status == kernel_return &&
           kernel_observed->information == 0 &&
           kernel_observed->invocations == 1 && f.Word(kIo) == kernel_return &&
           f.Word(kIo + 4) == 0 && f.Filled(kOut, 16, 0xA5),
       "actual inline I/O kernel APC executes PPC with final IOSB");
  Write("actual_kernel_io_APC", true,
        fmt::format(
            "\"io_returned_status\":{},\"observed_iosb\":{},\"observed_"
            "reserved\":{},"
            "\"observed_status\":{},\"observed_bytes\":{},\"invocations\":{},"
            "\"routine\":{},\"context\":{},\"PPC_bytes\":40,\"PPC_sha256\":\"{}"
            "\","
            "\"caller_frame_restored\":true,\"source_RO_result_RW_guards_"
            "verified\":true,"
            "\"dispatch_boundary\":\"real_inline_D9_kernel_APC\"",
            kernel_return, kernel_observed->iosb_address,
            kernel_observed->reserved, kernel_observed->status,
            kernel_observed->information, kernel_observed->invocations,
            *kernel_routine, *kernel_context, kApcSourceSha256));

  auto user_apc = prepare_apc();
  const auto user_routine = user_apc->CheckedRoutine(*f.issuer);
  const auto user_context = user_apc->CheckedContext(*f.issuer);
  Need(user_routine && !(*user_routine & 3) && user_context,
       "actual checked user-mode I/O APC source and context");
  control.device->overflow.store(false);
  f.Fill(kIo, 8, 0x5A);
  f.Fill(kOut, 16, 0xA5);
  event->Reset();
  const auto user_io_return =
      ioctl(volume->handle(), event->handle(), f.Addr(kIo), f.Addr(kOut), 8,
            *user_routine | 1u, *user_context);
  const auto before_alertable = user_apc->ReadAfterReturn(*f.issuer);
  Need(user_io_return == X_STATUS_SUCCESS && f.Word(kIo) == X_STATUS_SUCCESS &&
           f.Word(kIo + 4) == 8 && before_alertable &&
           before_alertable->invocations == 0 && f.issuer->HasPendingUserApc(),
       "odd I/O routine queues real user APC without inline execution");
  // Real ordinal5A alertable boundary; mode0 keeps the genuine SYSTEM caller.
  f.Fill(kOffset, 8, 0);
  const auto alertable_return =
      f.Call(0x5A, "KeDelayExecutionThread", {0, 1, f.Addr(kOffset)});
  const auto user_observed = user_apc->ReadAfterReturn(*f.issuer);
  Need(alertable_return == X_STATUS_USER_APC && user_observed &&
           user_observed->iosb_address == f.Addr(kIo) &&
           user_observed->reserved == 0 &&
           user_observed->status == X_STATUS_SUCCESS &&
           user_observed->information == 8 && user_observed->invocations == 1,
       "actual alertable delay executes owned user I/O APC PPC once");
  const auto second_alertable =
      f.Call(0x5A, "KeDelayExecutionThread", {0, 1, f.Addr(kOffset)});
  const auto after_second_boundary = user_apc->ReadAfterReturn(*f.issuer);
  Need(second_alertable == X_STATUS_SUCCESS && after_second_boundary &&
           after_second_boundary->invocations == 1,
       "completed user I/O APC is not executed twice");
  const auto kernel_after_user = kernel_apc->ReadAfterReturn(*f.issuer);
  Need(kernel_after_user && kernel_after_user->invocations == 1,
       "prior kernel APC is not replayed at user boundary");
  Write("actual_user_io_APC_alertable", true,
        fmt::format(
            "\"io_returned_status\":{},\"before_alertable_invocations\":0,"
            "\"alertable_returned_status\":{},\"second_alertable_status\":{},"
            "\"observed_iosb\":{},\"observed_reserved\":{},\"observed_status\":"
            "{},"
            "\"observed_bytes\":{},\"invocations\":{},\"routine_with_user_"
            "bit\":{},"
            "\"context\":{},\"PPC_bytes\":40,\"PPC_sha256\":\"{}\","
            "\"caller_frame_restored\":true,\"source_RO_result_RW_guards_"
            "verified\":true,"
            "\"dispatch_boundary\":\"registered_5A_alertable_delay\"",
            user_io_return, alertable_return, second_alertable,
            user_observed->iosb_address, user_observed->reserved,
            user_observed->status, user_observed->information,
            user_observed->invocations, *user_routine | 1u, *user_context,
            kApcSourceSha256));

  auto namespace_case = [&](const char* label, uint32_t root, X_STATUS expected,
                            bool success) {
    constexpr uint32_t kAttrs = 512, kName = 544, kNameBytes = 576,
                       kOutAnsi = 608, kPath = 640;
    constexpr uint16_t kCapacity = 128;
    constexpr std::string_view child = "probe.bin";
    std::array<uint8_t, 12> attrs{};
    std::array<uint8_t, 8> name{}, output{};
    xe::store_and_swap<uint32_t>(attrs.data(), root);
    xe::store_and_swap<uint32_t>(attrs.data() + 4, f.Addr(kName));
    xe::store_and_swap<uint32_t>(attrs.data() + 8, 0x40);
    xe::store_and_swap<uint16_t>(name.data(), uint16_t(child.size()));
    xe::store_and_swap<uint16_t>(name.data() + 2, uint16_t(child.size()));
    xe::store_and_swap<uint32_t>(name.data() + 4, f.Addr(kNameBytes));
    xe::store_and_swap<uint16_t>(output.data(), 0x5555);
    xe::store_and_swap<uint16_t>(output.data() + 2, kCapacity);
    xe::store_and_swap<uint32_t>(output.data() + 4, f.Addr(kPath));
    auto write = [&](uint32_t offset, std::span<const uint8_t> bytes) {
      Need(XSUCCEEDED(util::WriteGuestIoBuffer(*kernel_->memory(),
                                               f.Addr(offset), bytes)),
           "namespace owned input publication");
    };
    write(kAttrs, attrs);
    write(kName, name);
    write(kNameBytes,
          {reinterpret_cast<const uint8_t*>(child.data()), child.size()});
    write(kOutAnsi, output);
    f.Fill(kPath - 8, kCapacity + 16, 0xA5);
    const auto status = f.Call(0x113, "ObTranslateSymbolicLink",
                               {f.Addr(kAttrs), f.Addr(kOutAnsi)});
    std::array<uint8_t, 8> result{};
    std::array<uint8_t, kCapacity> payload{};
    std::array<uint8_t, 12> attrs_after{};
    Need(XSUCCEEDED(util::ReadGuestIoBuffer(*kernel_->memory(),
                                            f.Addr(kOutAnsi), result)) &&
             XSUCCEEDED(util::ReadGuestIoBuffer(*kernel_->memory(),
                                                f.Addr(kPath), payload)) &&
             XSUCCEEDED(util::ReadGuestIoBuffer(*kernel_->memory(),
                                                f.Addr(kAttrs), attrs_after)),
         "namespace actual outputs readable");
    Need(status == expected && attrs_after == attrs &&
             std::equal(output.begin() + 2, output.end(), result.begin() + 2) &&
             f.Filled(kPath - 8, 8, 0xA5) &&
             f.Filled(kPath + kCapacity, 8, 0xA5),
         "actual registered namespace ABI and guards");
    const std::string expected_path = control.name + "\\probe.bin";
    if (success) {
      Need(xe::load_and_swap<uint16_t>(result.data()) == expected_path.size() &&
               std::equal(expected_path.begin(), expected_path.end(),
                          payload.begin()) &&
               std::ranges::all_of(payload.begin() + expected_path.size(),
                                   payload.end(),
                                   [](auto b) { return b == 0xA5; }),
           "actual relative-root counted path without NUL publication");
    } else {
      Need(result == output && f.Filled(kPath, kCapacity, 0xA5),
           "invalid real namespace root leaves all output bytes unchanged");
    }
    Write(label, true,
          fmt::format(
              "\"returned_status\":{},\"root_handle\":{},"
              "\"counted_length\":{},\"capacity_and_pointer_unchanged\":true,"
              "\"guards_unchanged\":true,\"file_existence_claimed\":false",
              status, root, xe::load_and_swap<uint16_t>(result.data())));
  };
  namespace_case("actual_namespace_directory_root", volume->handle(),
                 X_STATUS_SUCCESS, true);
  constexpr uint32_t invalid_root = 0x7FFFFFFC;
  Need(!kernel_->object_table()->LookupObject<XObject>(invalid_root),
       "namespace invalid-root handle actually absent");
  namespace_case("actual_namespace_invalid_root", invalid_root,
                 X_STATUS_INVALID_HANDLE, false);
  namespace_case("actual_namespace_event_root", event->handle(),
                 X_STATUS_OBJECT_TYPE_MISMATCH, false);
  Need(f.Idle(), "all fixture callbacks drained before busy-mount check");
  const auto retained_root = volume->file()->retained_device();
  Need(bool(retained_root), "common file tree lease");
  Need(!kernel_->emulator()->file_system()->UnregisterDevice(control.name),
       "ordinary folder unmount refuses its live handles");
  Need(kernel_->emulator()->file_system()->GetDevice(control.name) ==
           retained_root.get(),
       "refused unmount preserves the registered root");
  namespace_case("actual_namespace_busy_mount_root", volume->handle(),
                 X_STATUS_SUCCESS, true);
  RunOverwriteCases(f);
  Write(
      "limits", true,
      "\"APC_execution\":\"actual_inline_kernel_and_alertable_user_IO; "
      "asynchronous_delivery_UNVERIFIED\","
      "\"cancel_from_outer_GCRC\":\"UNVERIFIED_deferred_completion_required\","
      "\"stop_and_device_removal_stress\":\"UNVERIFIED\","
      "\"power_loss\":\"UNVERIFIED\",\"macOS_execution\":\"UNVERIFIED\"");
}
// PRIVATE source draft: inserted into the existing owned SYSTEM observer only.
// No execution or readiness claim; no extra guest APC routine is installed.
void Probe::RunOverwriteCases(Fixture& f) {
#if XE_PLATFORM_LINUX
  constexpr uint32_t kHandle = 0x400, kAttrs = 0x410, kAnsi = 0x420,
                     kName = 0x440, kNameCapacity = 160, kOffset = 0x500,
                     kEof = 0x510, kQueryIo = 0x520, kTransferIo = 0x530,
                     kFlushIo = 0x540, kInfo = 0x580, kPayload = 0x600,
                     kRead = 0x640, kReadCapacity = 16;
  constexpr uint32_t kCanary = 0x5A5A5A5A;
  constexpr uint32_t kRights = filesystem::FileAccess::kGenericRead |
                               filesystem::FileAccess::kGenericWrite |
                               0x00100000;  // SYNCHRONIZE
  Need(f.Idle() && f.apc_memory_resident && f.allocation,
       "old I/O/APC groups drained with Kernel-owned memory retained");
  auto put = [&](uint32_t offset, std::span<const uint8_t> bytes) {
    Need(XSUCCEEDED(util::WriteGuestIoBuffer(*kernel_->memory(), f.Addr(offset),
                                            bytes)),
         "owned overwrite guest write");
  };
  auto put64 = [&](uint32_t offset, uint64_t value) {
    std::array<uint8_t, 8> bytes{};
    xe::store_and_swap<uint64_t>(bytes.data(), value);
    put(offset, bytes);
  };
  auto word64 = [&](uint32_t offset) {
    std::array<uint8_t, 8> bytes{};
    Need(XSUCCEEDED(util::ReadGuestIoBuffer(*kernel_->memory(), f.Addr(offset),
                                           bytes)),
         "owned overwrite guest read");
    return xe::load_and_swap<uint64_t>(bytes.data());
  };
  auto mount_seeded = [&](const char* child, std::span<const uint8_t> seed)
      -> Mounted& {
    Need(!f.parent->Create(child, true), "exclusive overwrite fixture child");
    std::error_code error;
    auto root = f.parent->Subroot(child, false, error);
    Need(bool(root) && !root->WriteNewFile("probe.bin", seed).error &&
             !root->open_file_count(),
         "owned overwrite seed without an alias lease");
    auto name = std::string("\\Device\\HddIoDiagnostic_") + child;
    auto volume = std::make_unique<DiagnosticVolume>(name, root, true);
    Need(volume->Initialize(), "overwrite real volume initialize");
    auto* raw = volume.get();
    // Track before registration so every later failure can detach this tree.
    f.mounts.push_back({std::move(name), std::move(root), {}, false});
    auto& m = f.mounts.back();
    Need(kernel_->emulator()->file_system()->RegisterDevice(std::move(volume)),
         "overwrite real volume register");
    m.registered = true;
    m.device = std::dynamic_pointer_cast<DiagnosticVolume>(
        raw->weak_from_this().lock());
    Need(bool(m.device), "overwrite registered tree owner");
    return m;
  };
  auto prepare_name = [&](const Mounted& m) {
    Need(f.Idle(), "no queued use while preparing create structures");
    const auto path = m.name + "\\probe.bin";
    Need(path.size() <= kNameCapacity, "bounded actual create path");
    std::array<uint8_t, 12> attrs{};
    xe::store_and_swap<uint32_t>(attrs.data() + 4, f.Addr(kAnsi));
    xe::store_and_swap<uint32_t>(attrs.data() + 8, 0x40);
    std::array<uint8_t, 8> ansi{};
    xe::store_and_swap<uint16_t>(ansi.data(), uint16_t(path.size()));
    xe::store_and_swap<uint16_t>(ansi.data() + 2, uint16_t(path.size()));
    xe::store_and_swap<uint32_t>(ansi.data() + 4, f.Addr(kName));
    f.Fill(kName - 8, kNameCapacity + 16, 0xA5);
    put(kName, {reinterpret_cast<const uint8_t*>(path.data()), path.size()});
    put(kAttrs, attrs);
    put(kAnsi, ansi);
  };
  auto create = [&](Mounted& m, uint32_t disposition, X_STATUS expected) {
    prepare_name(m);
    f.Fill(kHandle, 4, 0x5A);
    f.Fill(kQueryIo, 8, 0x5A);
    auto track_returned_handle = [&] {
      const uint32_t handle = f.Word(kHandle);
      if (!handle || handle == kCanary) return;
      auto object = kernel_->object_table()->LookupObject<XFile>(handle);
      Need(bool(object) && object->device() == m.device.get() &&
               object->entry()->name() == "probe.bin",
           "actual returned handle belongs to this exact diagnostic tree");
      if (std::ranges::none_of(f.files, [&](const auto& x) {
            return x.get() == object.get();
          })) {
        f.files.push_back(std::move(object));
      }
    };
    X_STATUS status;
    try {
      status = f.Call(
          0xD2, "NtCreateFile",
          {f.Addr(kHandle), kRights, f.Addr(kAttrs), f.Addr(kQueryIo), 0,
           0x80, 7, disposition, 0x40});  // async, FILE_NON_DIRECTORY_FILE
    } catch (...) {
      // A post-call ABI assertion must not strand an already-created handle.
      track_returned_handle();
      throw;
    }
    track_returned_handle();
    const uint32_t handle = f.Word(kHandle);
    Need(status == expected && f.Word(kQueryIo) == expected &&
             f.Word(kQueryIo + 4) ==
                 (expected == X_STATUS_SUCCESS ? (disposition == 1 ? 1 : 3) : 0)
             && f.Filled(kName - 8, 8, 0xA5) &&
             f.Filled(kName + kNameCapacity, 8, 0xA5),
         "actual create return IOSB/action and counted-path guards");
    Need((expected == X_STATUS_SUCCESS) == bool(handle),
         "failed actual overwrite returns zero handle");
    return handle;
  };
  auto await = [&](uint32_t iosb, uint32_t count) {
    Need(f.Poll([&] {
      const auto status = f.Word(iosb);
      return status != kCanary && status != X_STATUS_PENDING && f.Idle();
    }), "overwrite posted completion and callback drain");
    Need(f.Word(iosb) == X_STATUS_SUCCESS && f.Word(iosb + 4) == count,
         "overwrite final IOSB status and actual transfer count");
  };
  auto query = [&](uint32_t handle, uint64_t eof) {
    Need(f.Idle(), "query only after posted work drains");
    f.Fill(kQueryIo, 8, 0x5A);
    f.Fill(kInfo - 8, 56 + 16, 0xA5);
    const auto status = f.Call(0xE8, "NtQueryInformationFile",
        {handle, f.Addr(kQueryIo), f.Addr(kInfo), 56, 34});
    const uint64_t allocation = eof ? 512 : 0;
    Need(status == X_STATUS_SUCCESS && f.Word(kQueryIo) == 0 &&
             f.Word(kQueryIo + 4) == 56 && word64(kInfo + 40) == eof &&
             word64(kInfo + 32) == allocation &&
             f.Filled(kInfo - 8, 8, 0xA5) && f.Filled(kInfo + 56, 8, 0xA5),
         "actual query refresh observes owned EOF/allocation and guards");
  };
  auto resize = [&](uint32_t handle, uint64_t eof) {
    Need(f.Idle(), "SetEOF only after posted work drains");
    put64(kEof, eof);
    f.Fill(kQueryIo, 8, 0x5A);
    Need(f.Call(0xF7, "NtSetInformationFile",
                {handle, f.Addr(kQueryIo), f.Addr(kEof), 8, 20}) ==
             X_STATUS_SUCCESS && f.Word(kQueryIo) == 0 &&
             f.Word(kQueryIo + 4) == 0,
         "actual SetEOF succeeds with truthful synchronous IOSB");
  };
  auto issue_write = [&](uint32_t handle, uint64_t offset, uint8_t value) {
    Need(f.Idle(), "write payload/offset not reused while queued");
    put64(kOffset, offset);
    f.Fill(kPayload, 1, value);
    f.Fill(kTransferIo, 8, 0x5A);
    Need(f.Call(0xFF, "NtWriteFile",
                {handle, 0, 0, 0, f.Addr(kTransferIo), f.Addr(kPayload), 1,
                 f.Addr(kOffset)}) == X_STATUS_PENDING,
         "actual async overwrite write admitted");
  };
  auto issue_flush = [&](uint32_t handle) {
    f.Fill(kFlushIo, 8, 0x5A);
    Need(f.Call(0xDB, "NtFlushBuffersFile", {handle, f.Addr(kFlushIo)}) ==
             X_STATUS_PENDING,
         "actual async overwrite flush admitted");
  };
  auto read = [&](uint32_t handle, uint64_t offset,
                  std::span<const uint8_t> expected) {
    Need(f.Idle() && expected.size() <= kReadCapacity,
         "bounded read after previous work drains");
    put64(kOffset, offset);
    f.Fill(kTransferIo, 8, 0x5A);
    f.Fill(kRead - 8, kReadCapacity + 16, 0xA5);
    Need(f.Call(0xF0, "NtReadFile",
                {handle, 0, 0, 0, f.Addr(kTransferIo), f.Addr(kRead),
                 uint32_t(expected.size()), f.Addr(kOffset)}) == X_STATUS_PENDING,
         "actual async overwrite read admitted");
    await(kTransferIo, uint32_t(expected.size()));
    std::array<uint8_t, kReadCapacity> actual{};
    Need(XSUCCEEDED(util::ReadGuestIoBuffer(*kernel_->memory(), f.Addr(kRead),
                                           actual)) &&
             std::equal(expected.begin(), expected.end(), actual.begin()) &&
             f.Filled(kRead + uint32_t(expected.size()),
                      kReadCapacity - uint32_t(expected.size()), 0xA5) &&
             f.Filled(kRead - 8, 8, 0xA5) &&
             f.Filled(kRead + kReadCapacity, 8, 0xA5),
         "actual read bytes and untouched edge/guard extent");
  };
  auto close_guest = [&](Mounted& m, uint32_t handle) {
    Need(f.Idle(), "all posted requests drained before actual close");
    auto found = std::find_if(f.files.begin(), f.files.end(),
                             [&](const auto& x) { return x->handle() == handle; });
    Need(found != f.files.end(), "actual guest handle belongs to fixture");
    auto* const expected_object = found->get();
    bool removed = false;
    auto retire_tracking_if_closed = [&] {
      if (removed) return true;
      auto current = kernel_->object_table()->LookupObject<XObject>(handle);
      if (current.get() == expected_object) return false;
      f.files.erase(found);  // Never ReleaseHandle again for this closed slot.
      removed = true;
      return true;
    };
    X_STATUS status;
    try {
      status = f.Call(0xCF, "NtClose", {handle});
    } catch (...) {
      (void)retire_tracking_if_closed();
      throw;
    }
    const bool closed = retire_tracking_if_closed();
    Need(status == X_STATUS_SUCCESS && closed,
         "registered NtClose removes exact owned guest slot");
    // A posted worker's captured XFile may briefly outlive its running marker.
    Need(f.Poll([&] {
      return !m.root->open_file_count() && !m.device->has_open_files();
    }), "closed overwrite object refs and backend leases actually released");
  };
  const std::array<uint8_t, 8> seed{'a','b','c','d','e','f','g','h'};
  auto& m = mount_seeded("overwrite", seed);
  const auto path = m.root->path() / "probe.bin";
  struct stat original{};
  Need(!lstat(path.c_str(), &original) && S_ISREG(original.st_mode) &&
           original.st_nlink == 1 && original.st_size == 8,
       "owned overwrite initial regular inode");
  auto physical = [&](uint64_t size) {
    struct stat actual{};
    Need(!m.root->Observe().error && !lstat(path.c_str(), &actual) &&
             S_ISREG(actual.st_mode) && actual.st_nlink == 1 &&
             actual.st_dev == original.st_dev && actual.st_ino == original.st_ino &&
             actual.st_mode == original.st_mode && actual.st_size >= 0 &&
             uint64_t(actual.st_size) == size,
         "actual same owned inode physical EOF");
  };
  uint32_t handle = create(m, 4, X_STATUS_SUCCESS);
  query(handle, 0); physical(8);
  issue_write(handle, 0, 'X'); await(kTransferIo, 1);
  resize(handle, 3); query(handle, 3); physical(8);
  const std::array<uint8_t, 3> retained{'X','b','c'};
  read(handle, 0, retained);
  issue_write(handle, 2, 'Y');
  issue_flush(handle);  // Distinct IOSBs, queued immediately behind write.
  await(kTransferIo, 1); await(kFlushIo, 0);
  query(handle, 3); physical(3);
  resize(handle, 1); query(handle, 1); physical(1);
  resize(handle, 4); query(handle, 4); physical(4);
  const std::array<uint8_t, 4> grown{'X',0,0,0};
  read(handle, 0, grown);
  // NT uses -1, which ValidateRequest maps to XFile's internal EOF sentinel.
  issue_write(handle, UINT64_MAX, 'A'); await(kTransferIo, 1);
  query(handle, 5);
  const std::array<uint8_t, 1> appended{'A'};
  read(handle, 4, appended);
  issue_flush(handle); await(kFlushIo, 0); physical(5);
  close_guest(m, handle);
  const std::vector<uint8_t> final{'X',0,0,0,'A'};
  std::vector<uint8_t> persisted;
  Need(!m.root->ReadFile("probe.bin", 16, persisted) && persisted == final,
       "exact committed bytes after genuine close/ref release");
  Need(kernel_->emulator()->file_system()->UnregisterDevice(m.name),
       "detach only closed overwrite child");
  m.registered = false;
  m.device.reset();
  m.root.reset();
  std::error_code error;
  m.root = f.parent->Subroot("overwrite", false, error);
  Need(bool(m.root), "fresh child state after old child retirement");
  auto remount = std::make_unique<DiagnosticVolume>(m.name, m.root, true);
  Need(remount->Initialize(), "fresh overwrite volume reenumeration");
  auto* raw_remount = remount.get();
  Need(kernel_->emulator()->file_system()->RegisterDevice(std::move(remount)),
       "fresh overwrite volume registration");
  m.registered = true;
  m.device = std::dynamic_pointer_cast<DiagnosticVolume>(
      raw_remount->weak_from_this().lock());
  Need(bool(m.device), "fresh overwrite tree owner");
  handle = create(m, 1, X_STATUS_SUCCESS);
  query(handle, 5); read(handle, 0, final);
  close_guest(m, handle); physical(5);
  Write("actual_overwrite_session_lifecycle", true,
        "\"returned_create_status\":0,\"create_iosb_information\":3,"
        "\"first_flush_iosb_status\":0,\"postflush_eof_sequence\":[1,4,5],"
        "\"postflush_allocation_sequence\":[512,512,512],"
        "\"final_bytes_hex\":\"5800000041\",\"final_eof\":5,"
        "\"final_allocation\":512,\"same_inode\":true,"
        "\"old_tail_resurrected\":false,\"guard_bytes_preserved\":true,"
        "\"actual_guest_close_and_refs_drained\":true,"
        "\"same_process_fresh_device_reopen\":true,"
        "\"cold_process_restart_verified\":false,"
        "\"additional_PPC_APCs\":0,\"production_ready\":false");

  Need(geteuid() != 0, "permission prerequisite: genuine nonroot UID");
  std::ifstream capabilities("/proc/self/status");
  std::string line;
  bool found_capabilities = false;
  while (std::getline(capabilities, line)) {
    if (!line.starts_with("CapEff:")) continue;
    found_capabilities = true;
    Need(!(std::stoull(line.substr(7), nullptr, 16) & 2),
         "permission prerequisite: CAP_DAC_OVERRIDE absent");
    break;
  }
  Need(found_capabilities, "permission prerequisite: readable effective capabilities");
  const std::array<uint8_t, 8> permission_seed{'p','r','e','s','e','r','v','e'};
  auto& permission = mount_seeded("overwrite_permission", permission_seed);
  const auto permission_path = permission.root->path() / "probe.bin";
  struct stat before{};
  Need(!lstat(permission_path.c_str(), &before) && S_ISREG(before.st_mode) &&
           before.st_nlink == 1 && before.st_size == 8,
       "owned permission fixture original inode");
  struct RestoreOwnedMode {
    std::filesystem::path path;
    struct stat original;
    bool active = true;
    bool Restore() noexcept {
      if (!active) return true;
      struct stat current{};
      if (lstat(path.c_str(), &current) || !S_ISREG(current.st_mode) ||
          current.st_dev != original.st_dev || current.st_ino != original.st_ino ||
          chmod(path.c_str(), original.st_mode & 07777)) return false;
      if (lstat(path.c_str(), &current) || current.st_mode != original.st_mode)
        return false;
      active = false;
      return true;
    }
    ~RestoreOwnedMode() { (void)Restore(); }
  } restore{permission_path, before};
  try {
    Need(!chmod(permission_path.c_str(), 0000), "owned fixture mode000");
    {
      std::error_code denied;
      auto preflight = permission.root->OpenFile(
          "probe.bin", filesystem::FileAccess::kGenericRead |
                           filesystem::FileAccess::kGenericWrite, denied);
      Need(!preflight && denied == std::errc::permission_denied,
           "permission prerequisite: actual same-UID RW open denied EACCES");
    }
    for (uint32_t disposition : {4u, 5u}) {
      Need(create(permission, disposition, X_STATUS_ACCESS_DENIED) == 0,
           "supported overwrite failure produces no guest handle");
      struct stat after{};
      Need(!lstat(permission_path.c_str(), &after) &&
               after.st_dev == before.st_dev && after.st_ino == before.st_ino &&
               after.st_size == before.st_size && (after.st_mode & 07777) == 0 &&
               !permission.root->open_file_count(),
           "actual guest permission failure never deletes original inode/name");
    }
    Need(restore.Restore(), "owned permission mode restored and verified");
    std::vector<uint8_t> preserved;
    Need(!permission.root->ReadFile("probe.bin", 16, preserved) &&
             std::equal(preserved.begin(), preserved.end(),
                        permission_seed.begin(), permission_seed.end()) &&
             !permission.root->open_file_count(),
         "actual permission failure preserves every original byte");
  } catch (...) {
    Need(restore.Restore(), "restore owned permission fixture on failure");
    throw;
  }
  Write("actual_overwrite_permission_preserved", true,
        "\"create_dispositions\":[4,5],\"statuses\":[3221225506,3221225506],"
        "\"handle_outputs\":[0,0],\"iosb_information\":[0,0],"
        "\"same_inode_and_name\":true,\"same_size_and_permissions\":true,"
        "\"original_bytes_preserved\":true,\"permissions_restored\":true,"
        "\"no_new_open_handles\":true,\"capability_prerequisite_checked\":true,"
        "\"production_ready\":false");
#else
  Need(false, "overwrite runtime cases require Linux; execution unverified here");
#endif
}

}  // namespace
std::shared_ptr<NativeSystemUiDiagnosticObservation> WrapFileIoContractProbe(
    KernelState* kernel,
    std::shared_ptr<NativeSystemUiDiagnosticObservation> inner,
    const std::filesystem::path& report) {
  if (!kernel || !inner || report.empty() || !report.is_absolute() ||
      inner->backend_stop_proof().stopped()) {
    return {};
  }
  std::unique_ptr<FILE, decltype(&std::fclose)> output{
      filesystem::OpenFile(report, "wbx"), &std::fclose};
  if (!output) {
    return {};
  }
  auto probe = std::make_shared<Probe>(kernel, std::move(inner), output.get());
  output.release();
  return probe;
}
}  // namespace xe::kernel
