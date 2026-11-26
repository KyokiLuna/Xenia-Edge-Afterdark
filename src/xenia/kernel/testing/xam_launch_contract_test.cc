/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia contributors. All rights reserved.                    *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */
#include <array>
#include "xenia/base/platform.h"
#include <memory>
#include <string>
#include <vector>
#include "xenia/emulator_lifecycle.h"

#include "third_party/catch/include/catch.hpp"
#include "xenia/kernel/xam/xam_launch_contract.h"
#include "xenia/kernel/xam/xam_module.h"
#include "xenia/kernel/xam/xam_private.h"

namespace xe::kernel::xam::test {

TEST_CASE("Native HUD reboot imports resolve to real implemented exports",
          "[kernel][xam][launch]") {
  cpu::ExportResolver resolver;
  RegisterInfoExports(&resolver, nullptr);
  XamModule::RegisterExportTable(&resolver);
  for (const auto ordinal : {0x838, 0x839}) {
    const auto* entry = resolver.GetExportByOrdinal("xam.xex", ordinal);
    REQUIRE(entry);
    CHECK(entry->get_type() == cpu::Export::Type::kFunction);
    CHECK(entry->is_implemented());
    CHECK((entry->tags & cpu::ExportTag::kStub) == 0);
    CHECK(entry->function_data.trampoline != nullptr);
  }
}

TEST_CASE("Dashboard reboot owns the full native record and original user",
          "[kernel][xam][launch]") {
  std::array<uint8_t, DashboardRebootRequest::kBytes> source;
  for (size_t i = 0; i < source.size(); ++i) {
    source[i] = uint8_t(i * 37);
  }
  xe::store_and_swap<uint32_t>(&source[4], 0x16);
  xe::store_and_swap<uint32_t>(&source[8], 3);
  const auto original = source;
  auto request = DashboardRebootRequest::Copy(source);
  REQUIRE(request);
  source.fill(0xCD);  // The caller's storage may disappear after dispatch.
  CHECK(std::equal(request->bytes().begin(), request->bytes().end(),
                   original.begin()));
  CHECK(request->reason() == 0x16);
  CHECK(request->user_index() == 3);
  CHECK(request->notification_value() == 0x00030016);
  CHECK(request->launch_flags(0) == 0);
  CHECK_FALSE(DashboardRebootRequest::Copy({}));
  CHECK_FALSE(DashboardRebootRequest::Copy(
      std::span<const uint8_t>(source).first(source.size() - 1)));
}

TEST_CASE("Native dashboard navigation distinguishes notify from restart",
          "[kernel][xam][launch]") {
  std::array<uint8_t, DashboardRebootRequest::kBytes> source{};
  xe::store_and_swap<uint32_t>(&source[4], 0x48);
  auto request = DashboardRebootRequest::Copy(source);
  REQUIRE(request);
  CHECK(request->notify_running_dashboard(true, 0, 0));
  CHECK_FALSE(request->notify_running_dashboard(false, 0, 0));
  CHECK_FALSE(request->notify_running_dashboard(true, 5, 0));
  CHECK(request->notify_running_dashboard(true, 6, 0x7F));
  CHECK_FALSE(request->notify_running_dashboard(true, 0, 0x80));
  CHECK(request->launch_flags(0) == 0x100000);
  CHECK(request->launch_flags(0x7F) == 0x100000);
  CHECK(request->launch_flags(0x80) == 0x1000);
}

TEST_CASE("Native reboot confirmation respects privilege and dash context",
          "[kernel][xam][launch]") {
  CHECK(ShouldConfirmDashboardReboot(false, 0, 0));
  CHECK(ShouldConfirmDashboardReboot(false, 1u << 17, 0));
  CHECK_FALSE(ShouldConfirmDashboardReboot(false, 1u << 18, 0));
  CHECK_FALSE(ShouldConfirmDashboardReboot(true, 0, 0));
  CHECK_FALSE(ShouldConfirmDashboardReboot(true, 1u << 18, 5));
  CHECK(ShouldConfirmDashboardReboot(true, 0, 6));
  CHECK(ShouldConfirmDashboardReboot(false, 1u << 18, 6));
  CHECK(ShouldConfirmDashboardReboot(true, 1u << 18, 6));
}

TEST_CASE("Spawn launch transport preserves every byte including zeros",
          "[kernel][xam][launch]") {
  std::vector<uint8_t> bytes(DashboardRebootRequest::kBytes);
  for (size_t i = 0; i < bytes.size(); ++i) {
    bytes[i] = uint8_t(i);
  }
  const auto encoded = EncodeLaunchData(bytes);
  REQUIRE(encoded.size() == bytes.size() * 2);
  CHECK(encoded.substr(0, 8) == "00010203");
  CHECK(encoded.substr(0xFE * 2, 8) == "feff0001");
  std::vector<uint8_t> decoded;
  for (size_t i = 0; i < bytes.size(); ++i) {
    decoded.push_back(
        uint8_t(std::stoul(encoded.substr(i * 2, 2), nullptr, 16)));
  }
  CHECK(decoded == bytes);
  CHECK(EncodeLaunchData({}).empty());
}

}  // namespace xe::kernel::xam::test

namespace xe::kernel::xam::test {
TEST_CASE(
    "Owned lifecycle launch payload retains moved reservations and exact "
    "launch bytes",
    "[kernel][xam][launch][host-queue]") {
  std::string host = "/copied/title.xex", module = "sub/default.xex";
  std::vector<uint8_t> bytes{0, 0x80, 0xFF, 0};
  auto reservation = std::make_unique<uint32_t>(0x12345678);
  std::string got_host, got_module;
  std::vector<uint8_t> got_bytes;
  uint32_t got_reservation = 0;
  EmulatorLifecycle::Request request;
  request.SetWork([host, module, bytes, reservation = std::move(reservation),
                   &got_host, &got_module, &got_bytes, &got_reservation] {
    got_host = host;
    got_module = module;
    got_bytes = bytes;
    got_reservation = *reservation;
  });
  CHECK_FALSE(reservation);
  host.clear();
  module.clear();
  bytes.clear();
  // SetWork is the production copied host-payload holder, not a launch result.
  auto copy = request.work;
  request.work = {};
  copy();
  CHECK(got_host == "/copied/title.xex");
  CHECK(got_module == "sub/default.xex");
  CHECK((got_bytes == std::vector<uint8_t>{0, 0x80, 0xFF, 0}));
  CHECK(got_reservation == 0x12345678);
}
}  // namespace xe::kernel::xam::test

#if XE_PLATFORM_LINUX
#include <atomic>
#include <chrono>
#include <filesystem>
#include "xenia/base/clock.h"
#include "xenia/base/threading.h"
#include "xenia/emulator.h"
#include "xenia/kernel/guest_scheduler.h"
#include "xenia/kernel/kernel_flags.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/testing/isolated_kernel_runtime_settings.h"
#include "xenia/kernel/user_module.h"
#include "xenia/kernel/xthread.h"
DECLARE_double(time_scalar);
DECLARE_bool(clock_no_scaling);

namespace xe::kernel::xam::test {
namespace {
struct HandoffParkPolicy {
  ::xe::kernel::test::IsolatedKernelRuntimeSettings isolation;
  bool scheduler = cvars::guest_scheduler;
  double configured_time_scalar = cvars::time_scalar;
  double actual_time_scalar = Clock::guest_time_scalar();
  bool clock_no_scaling = cvars::clock_no_scaling;
  HandoffParkPolicy() {
    cvars::guest_scheduler = true;
    cvars::time_scalar = 1.0;
    cvars::clock_no_scaling = false;
  }
  ~HandoffParkPolicy() {
    // No Kernel/dispatch workers exist before this policy or after its user.
    Clock::set_guest_time_scalar(actual_time_scalar);
    cvars::clock_no_scaling = clock_no_scaling;
    cvars::time_scalar = configured_time_scalar;
    cvars::guest_scheduler = scheduler;
  }
};
struct HandoffParkRoot {
  std::filesystem::path path;
  HandoffParkRoot() {
    static std::atomic<uint64_t> sequence{0};
    path = std::filesystem::temp_directory_path() /
        ("xenia-title-handoff-" + std::to_string(std::chrono::steady_clock::now()
          .time_since_epoch().count()) + "-" +
         std::to_string(sequence.fetch_add(1)));
    REQUIRE(std::filesystem::create_directory(path));
    REQUIRE(std::filesystem::create_directory(path / "content"));
    REQUIRE(std::filesystem::create_directory(path / "cache"));
  }
  ~HandoffParkRoot() {
    std::error_code error;
    std::filesystem::remove_all(path, error);
  }
};
}
TEST_CASE("Accepted title handoff stays parked after actual Resume without blocking its co-resident fiber",
          "[kernel][xam][launch][handoff][fiber]") {
  // Real Kernel, XHostThread, CPU0 fiber dispatcher, self-Suspend/Resume,
  // Delay and actual shutdown joins. No native Editor/PPC/Task/GPU/launch
  // acceptance is fabricated by this common park regression.
  HandoffParkPolicy policy;
  HandoffParkRoot root;
  auto emulator = std::make_unique<Emulator>(
      "", root.path, root.path / "content", root.path / "cache");
  REQUIRE(emulator->Setup(nullptr, nullptr, false, {}, {}, {}) == X_STATUS_SUCCESS);
  auto* kernel = emulator->kernel_state();
  REQUIRE(kernel);
  // Setup initializes IDLE/SYSTEM, while the genuine executable producer
  // initializes TITLE's process and thread list. Keep this real minimal PPC
  // module owned through the actual thread/scheduler quiesce below; NullBackend
  // never executes its BLR. Do not fabricate guest process/list state here.
  std::vector<uint8_t> elf(0x104);
  const auto be16 = [&](size_t at, uint16_t value) {
    xe::store_and_swap<uint16_t>(elf.data() + at, value);
  };
  const auto be32 = [&](size_t at, uint32_t value) {
    xe::store_and_swap<uint32_t>(elf.data() + at, value);
  };
  elf[0] = 0x7F; elf[1] = 'E'; elf[2] = 'L'; elf[3] = 'F';
  elf[4] = 1; elf[5] = 2; elf[6] = 1;
  be16(16, 2); be16(18, 20); be32(20, 1);
  be32(24, 0x82000000); be32(28, 52);
  be16(40, 52); be16(42, 32); be16(44, 1);
  be32(52, 1); be32(56, 0x100); be32(60, 0x82000000);
  be32(64, 0x82000000); be32(68, 4); be32(72, 0x1000);
  be32(76, 5); be32(80, 0x1000); be32(0x100, 0x4E800020);
  auto module = object_ref<UserModule>(new UserModule(kernel));
  REQUIRE(module->LoadFromMemoryNamed("title-handoff-fixture", elf.data(),
                                    elf.size()) == X_STATUS_SUCCESS);
  kernel->SetExecutableModule(module);
  auto* title = emulator->memory()->TranslateVirtual<X_KPROCESS*>(
      kernel->GetTitleProcess());
  REQUIRE(title->process_type == X_PROCTYPE_TITLE);
  const uint32_t title_threads = kernel->GetTitleProcess() +
                                offsetof(X_KPROCESS, thread_list);
  REQUIRE(title->thread_list.flink_ptr == title_threads);
  REQUIRE(title->thread_list.blink_ptr == title_threads);
  REQUIRE(title->thread_count == 0);
  auto entered = threading::Event::CreateManualResetEvent(false);
  auto first_peer_ran = threading::Event::CreateManualResetEvent(false);
  auto second_peer_ran = threading::Event::CreateManualResetEvent(false);
  REQUIRE(entered); REQUIRE(first_peer_ran); REQUIRE(second_peer_ran);
  std::atomic<bool> continued{false}, real_delay{false}, same_cpu{false};
  object_ref<XHostThread> caller, first_peer, second_peer;
  bool caller_created = false;
  struct Quiesce {
    KernelState* kernel;
    object_ref<XHostThread>& caller;
    bool& caller_created;
    void Stop() {
      if (!kernel) return;
      if (caller_created) caller->Terminate(0);
      // Actual CPU joins precede reclaim and every borrowed test capture drop.
      kernel->ShutdownDispatchThread();
      kernel->guest_scheduler()->Shutdown();
      kernel = nullptr;
    }
    ~Quiesce() { Stop(); }
  } quiesce{kernel, caller, caller_created};
  constexpr uint32_t cpu0 = 1u << 24;
  caller = object_ref<XHostThread>(new XHostThread(kernel, 128 * 1024, cpu0,
      [&] {
        entered->Set();
        // Use the actual production entry, not a mirrored suspension loop.
        void (*park)() = &ParkAfterTitleHandoff;
        park();
        continued.store(true);
        return 0;
      }, kernel->GetTitleProcess(), true));
  caller_created = caller->Create() == X_STATUS_SUCCESS;
  REQUIRE(caller_created);
  REQUIRE(caller->fiber());
  REQUIRE(threading::Wait(entered.get(), false, std::chrono::seconds(5)) ==
          threading::WaitResult::kSuccess);
  first_peer = object_ref<XHostThread>(new XHostThread(kernel, 128 * 1024, cpu0,
      [&] { first_peer_ran->Set(); return 0; }, kernel->GetTitleProcess(), true));
  REQUIRE(first_peer->Create() == X_STATUS_SUCCESS);
  REQUIRE(first_peer->fiber());
  REQUIRE(threading::Wait(first_peer_ran.get(), false, std::chrono::seconds(5)) ==
          threading::WaitResult::kSuccess);
  // CPU0 could run this peer only after the real self-suspend yielded.
  REQUIRE(caller->suspend_count() == 1);
  CHECK_FALSE(continued.load());
  uint32_t previous = 0;
  REQUIRE(caller->Resume(&previous) == X_STATUS_SUCCESS);
  REQUIRE(previous == 1);
  second_peer = object_ref<XHostThread>(new XHostThread(kernel, 128 * 1024, cpu0,
      [&] {
        auto* self = XThread::GetCurrentThread();
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        do {
          // Both actual fibers are pinned to this same dispatch host thread:
          // the other fiber's saved wait metadata cannot change concurrently.
          if (caller->scheduler_links().wait_kind ==
              uint8_t(XThread::CooperativeWaitKind::kDelay)) {
            real_delay.store(true);
            same_cpu.store(self->active_cpu() == 0 && caller->active_cpu() == 0);
            break;
          }
          self->Delay(1, 0, uint64_t(-10'000ll));
        } while (std::chrono::steady_clock::now() < until);
        second_peer_ran->Set();
        return 0;
      }, kernel->GetTitleProcess(), true));
  REQUIRE(second_peer->Create() == X_STATUS_SUCCESS);
  REQUIRE(second_peer->fiber());
  REQUIRE(threading::Wait(second_peer_ran.get(), false, std::chrono::seconds(5)) ==
          threading::WaitResult::kSuccess);
  CHECK(real_delay.load());
  CHECK(same_cpu.load());
  CHECK_FALSE(continued.load());
  quiesce.Stop();
  CHECK_FALSE(continued.load());
}
}  // namespace xe::kernel::xam::test
#endif  // XE_PLATFORM_LINUX

#if XE_PLATFORM_LINUX
#include "xenia/kernel/native_profile_identity_contract.h"
namespace xe::kernel::test {
// Direct access to the ACTUAL routing/thread/join production helpers. These
// fixtures do not construct a fake Owner, native Task or Service stop result.
class NativeIdentityOrdinaryTransportTestAccess final {
 public:
  using Shadow=identity_detail::NativeIdentityOrdinaryTransport;
  using Joined=identity_detail::NativeIdentityJoinedHostStack;
  using Ledger=identity_detail::NativeProfileIdentityTasks;
  using Frame=Ledger::Frame;
  using Route=Ledger::CompletionRoute;
  static constexpr Ledger::Root root{71,83,97};
  static Frame F(XThread& thread,uint32_t function) {
    return {root,thread.thread_id(),thread.pcr_ptr(),thread.stack_base()-0x100,
        0x8199C68C,function};
  }
  static auto Task(Frame frame,XThread& thread) {return Shadow::CaptureTask(frame,thread);}
  static auto Completion(Frame frame,XThread& thread,const Route& route) {
    return Shadow::CaptureCompletion(frame,thread,route);
  }
  static bool Retain(const Shadow& shadow,KernelState* kernel,Frame frame,const XThread& thread,
      const Route* route=nullptr,const Joined* joined=nullptr) {
    return shadow.RetainThroughActualStop(kernel,root,frame,thread,route,joined);
  }
  static bool Returned(Shadow& shadow,Frame frame,const XThread& thread) {return shadow.Return(frame,thread);}
  static void Unavailable(Shadow& shadow) {shadow.UnavailableReturn();}
  static auto Join(XThread& thread,std::chrono::milliseconds timeout) {return Joined::JoinActual(thread,timeout);}
  static std::weak_ptr<const XThread> JoinedOwner(const Joined& joined) {return joined.thread_;}
  static uint64_t Pending(Ledger& ledger,Frame frame,uint32_t output) {
    return ledger.BeginExecutionCreate(frame,output);
  }
  static bool Closed(const Ledger& ledger) {return ledger.ExecutionClosed();}
};
} // namespace xe::kernel::test
namespace xe::kernel::xam::test {
TEST_CASE("Ordinary routing retains a real guest fiber through shutdown without completing its native Task ledger",
          "[kernel][identity_task][ordinary_transport][thread_stop]") {
  using Access=::xe::kernel::test::NativeIdentityOrdinaryTransportTestAccess;
  HandoffParkPolicy policy;HandoffParkRoot root;
  auto emulator=std::make_unique<Emulator>("",root.path,root.path/"content",root.path/"cache");
  REQUIRE(emulator->Setup(nullptr,nullptr,false,{},{},{})==X_STATUS_SUCCESS);
  auto* kernel=emulator->kernel_state();REQUIRE(kernel);
  // This is an actual pure guest XThread, created suspended by the real
  // scheduler. Its original PPC does not execute in this NullBackend fixture.
  auto thread=object_ref<XThread>(new XThread(kernel,128*1024,0,0x82000000,0,
      (1u<<24)|1u,true,false,kernel->GetSystemProcess()));
  REQUIRE(thread->Create()==X_STATUS_SUCCESS);REQUIRE(thread->is_guest_thread());
  REQUIRE(thread->fiber());REQUIRE_FALSE(thread->thread());
  struct Stop {
    KernelState* kernel;object_ref<XThread>& thread;
    ~Stop(){if(thread)thread->Terminate(0);kernel->ShutdownDispatchThread();kernel->guest_scheduler()->Shutdown();}
  } stop{kernel,thread};
  const auto frame=Access::F(*thread,0x816BA058);
  auto shadow=Access::Task(frame,*thread);REQUIRE(shadow);
  Access::Ledger ledger(Access::root);
  auto create=frame;create.function=0x816BA668;
  REQUIRE(Access::Pending(ledger,create,frame.sp+0x50));
  REQUIRE_FALSE(Access::Closed(ledger));
  REQUIRE(Access::Retain(*shadow,kernel,frame,*thread));
  // Resource retention is not an original return/cancel/free/completion, and
  // never changes the separate real production ledger's pending operation.
  CHECK_FALSE(Access::Closed(ledger));
  auto wrong=frame;++wrong.root.generation;
  CHECK_FALSE(Access::Retain(*shadow,kernel,wrong,*thread));
  CHECK_FALSE(Access::Retain(*shadow,nullptr,frame,*thread));
  auto foreign=Access::F(*thread,0x8199C838);
  CHECK_FALSE(Access::Task(foreign,*thread)); // Dataful profile worker is no routing primitive.
  auto malformed=frame;malformed.sp-=0x10;
  CHECK_FALSE(Access::Returned(*shadow,malformed,*thread));
  CHECK_FALSE(Access::Returned(*shadow,frame,*thread)); // Cannot relearn the missing own-return.
  CHECK(Access::Retain(*shadow,kernel,frame,*thread)); // Still only factual-stop coverage.
  CHECK_FALSE(Access::Closed(ledger));
  auto held=retain_object(thread.get());
  kernel->ShutdownDispatchThread();kernel->guest_scheduler()->Shutdown();thread->Terminate(0);
  CHECK(kernel->guest_scheduler()->shutting_down());
  CHECK(held->thread_state()); // The real holder survives physical worker/fiber retirement.
  CHECK_FALSE(held->is_running());
  thread.reset();CHECK(held->thread_state());held.reset();
}
TEST_CASE("Ordinary CompleteIO routing keeps exact returns and identity separation on an actual thread",
          "[kernel][identity_task][ordinary_transport][completion_route]") {
  using Access=::xe::kernel::test::NativeIdentityOrdinaryTransportTestAccess;
  HandoffParkPolicy policy;HandoffParkRoot root;
  auto emulator=std::make_unique<Emulator>("",root.path,root.path/"content",root.path/"cache");
  REQUIRE(emulator->Setup(nullptr,nullptr,false,{},{},{})==X_STATUS_SUCCESS);
  auto* kernel=emulator->kernel_state();REQUIRE(kernel);
  auto thread=object_ref<XThread>(new XThread(kernel,128*1024,0,0x82000000,0,
      (1u<<24)|1u,true,false,kernel->GetSystemProcess()));
  REQUIRE(thread->Create()==X_STATUS_SUCCESS);
  struct Stop {KernelState* kernel;object_ref<XThread>& thread;
    ~Stop(){if(thread)thread->Terminate(0);kernel->ShutdownDispatchThread();kernel->guest_scheduler()->Shutdown();}
  } stop{kernel,thread};
  const auto frame=Access::F(*thread,0x816A2588);
  auto ordinary=Access::Route::Capture(frame,nullptr,0,0,0,0x404E0A10);
  auto shadow=Access::Completion(frame,*thread,ordinary);REQUIRE(shadow);
  REQUIRE(Access::Retain(*shadow,kernel,frame,*thread,&ordinary));
  CHECK_FALSE(Access::Retain(*shadow,kernel,frame,*thread));
  auto equal_ordinary=Access::Route::Capture(frame,nullptr,0,0,0,0x404E0A10);
  CHECK_FALSE(Access::Retain(*shadow,kernel,frame,*thread,&equal_ordinary));
  auto job=std::make_shared<int>(1);
  const auto identity=Access::Route::Capture(frame,job,7,9,0x404E0A10,0x404E0A10);
  CHECK_FALSE(Access::Completion(frame,*thread,identity));
  CHECK_FALSE(Access::Retain(*shadow,kernel,frame,*thread,&identity));
  auto inner=frame;inner.sp-=0xA0;inner.lr+=4;
  auto nested=Access::Route::Capture(inner,nullptr,0,0,0,0x404E0B10);
  auto inner_shadow=Access::Completion(inner,*thread,nested);REQUIRE(inner_shadow);
  REQUIRE(nested.ReturnOrdinary(inner));REQUIRE(Access::Returned(*inner_shadow,inner,*thread));
  CHECK_FALSE(Access::Retain(*inner_shadow,kernel,inner,*thread,&nested));
  CHECK(Access::Retain(*shadow,kernel,frame,*thread,&ordinary));
  REQUIRE(ordinary.ReturnOrdinary(frame));REQUIRE(Access::Returned(*shadow,frame,*thread));
  CHECK_FALSE(Access::Retain(*shadow,kernel,frame,*thread,&ordinary));
  CHECK_FALSE(Access::Returned(*shadow,frame,*thread));
  CHECK_FALSE(Access::Retain(*shadow,kernel,frame,*thread,&ordinary));
}
TEST_CASE("A real host stack needs its exact completed OS join and an on-fiber host facade is not a guest witness",
          "[kernel][identity_task][ordinary_transport][host_join]") {
  using Access=::xe::kernel::test::NativeIdentityOrdinaryTransportTestAccess;
  HandoffParkPolicy policy;HandoffParkRoot root;
  auto emulator=std::make_unique<Emulator>("",root.path,root.path/"content",root.path/"cache");
  REQUIRE(emulator->Setup(nullptr,nullptr,false,{},{},{})==X_STATUS_SUCCESS);
  auto* kernel=emulator->kernel_state();REQUIRE(kernel);
  auto entered=threading::Event::CreateManualResetEvent(false);
  auto release=threading::Event::CreateManualResetEvent(false);REQUIRE(entered);REQUIRE(release);
  std::optional<Access::Shadow> shadow;std::optional<Access::Frame> frame;
  object_ref<XHostThread> host;
  struct HostJoin {object_ref<XHostThread>& host;threading::Event* release;
    ~HostJoin(){release->Set();if(host && host->thread())threading::Wait(host->thread(),false);}
  } cleanup{host,release.get()};
  host=object_ref<XHostThread>(new XHostThread(kernel,128*1024,0,[&]{
    auto* self=XThread::GetCurrentThread();frame.emplace(Access::F(*self,0x816BA058));
    if(auto fact=Access::Task(*frame,*self))shadow.emplace(std::move(*fact));
    entered->Set();threading::Wait(release.get(),false);return 0;
  },kernel->GetSystemProcess()));
  REQUIRE(host->Create()==X_STATUS_SUCCESS);
  REQUIRE(threading::Wait(entered.get(),false,std::chrono::seconds(5))==threading::WaitResult::kSuccess);
  REQUIRE(shadow);REQUIRE(frame);REQUIRE(host->is_host_thread());
  CHECK_FALSE(Access::Retain(*shadow,kernel,*frame,*host));
  CHECK_FALSE(Access::Join(*host,std::chrono::milliseconds(0))); // Actually still inside its C++ stack.
  release->Set();auto joined=Access::Join(*host,std::chrono::seconds(5));REQUIRE(joined);
  CHECK(Access::Retain(*shadow,kernel,*frame,*host,nullptr,&*joined));
  auto wrong=*frame;wrong.pcr+=0x1000;
  CHECK_FALSE(Access::Retain(*shadow,kernel,wrong,*host,nullptr,&*joined));
  const auto retained=Access::JoinedOwner(*joined);CHECK_FALSE(retained.expired());
  auto facade=object_ref<XHostThread>(new XHostThread(kernel,128*1024,(1u<<24)|1u,
      []{return 0;},kernel->GetSystemProcess(),true));
  REQUIRE(facade->Create()==X_STATUS_SUCCESS);REQUIRE(facade->fiber());
  REQUIRE_FALSE(facade->is_host_thread());REQUIRE_FALSE(facade->is_guest_thread());
  const auto facade_frame=Access::F(*facade,0x816BA058);
  auto facade_shadow=Access::Task(facade_frame,*facade);REQUIRE(facade_shadow);
  CHECK_FALSE(Access::Retain(*facade_shadow,kernel,facade_frame,*facade));
  CHECK_FALSE(Access::Retain(*facade_shadow,kernel,facade_frame,*facade,nullptr,&*joined));
  CHECK_FALSE(Access::Join(*facade,std::chrono::milliseconds(0)));
  facade->Terminate(0);kernel->ShutdownDispatchThread();kernel->guest_scheduler()->Shutdown();
  joined.reset();CHECK(retained.expired());
}
} // namespace xe::kernel::xam::test
#endif // XE_PLATFORM_LINUX
