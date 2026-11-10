/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia contributors. All rights reserved.                    *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */
#include "xenia/base/native_ui_backend_route.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>
#include "third_party/catch/include/catch.hpp"
#include "xenia/gpu/command_processor.h"
#include "xenia/gpu/native_system_title_input_provider.h"
#include "xenia/kernel/testing/native_system_title_input_test_access.h"
#include "xenia/kernel/xam/host_ui_fallback.h"
#include "xenia/ui/windowed_app_context.h"

namespace xe::kernel::test {
namespace {
using Route = NativeUiBackendRoute;
using Input = Route::Input;
using Decision = Route::Decision;
Route::Cohort Cohort() { return {7, 11, 13, 17, 19, 0x30020000, 0x3001C000}; }
class Context final : public ui::WindowedAppContext {
 protected:
  void NotifyUILoopOfPendingFunctions() override {}
  void PlatformQuitFromUIThread() override {}
};
auto Now() { return std::chrono::steady_clock::now(); }
}  // namespace
TEST_CASE("UI route never treats unknown input or a foreign cohort as fallback",
          "[kernel][native_ui_route]") {
  Route route;
  const auto cohort = Cohort();
  CHECK(route.Admit(cohort, Input::kUnknown, true).decision ==
        Decision::kDenied);
  CHECK(route.Admit(cohort, Input::kProvenNoInitialSwap, false).decision ==
        Decision::kDenied);
  for (unsigned field = 0; field < 7; ++field) {
    auto foreign = cohort;
    switch (field) {
      case 0:
        ++foreign.domain;
        break;
      case 1:
        ++foreign.root_load_generation;
        break;
      case 2:
        ++foreign.backend_epoch;
        break;
      case 3:
        ++foreign.foreground_generation;
        break;
      case 4:
        ++foreign.title_load_generation;
        break;
      case 5:
        ++foreign.root_hmodule;
        break;
      case 6:
        ++foreign.title_hmodule;
        break;
    }
    CHECK(route.Admit(foreign, Input::kProvenNoInitialSwap, true).decision ==
          Decision::kDenied);
  }
  auto host = route.Admit(cohort, Input::kProvenNoInitialSwap, true);
  REQUIRE(host.decision == Decision::kHost);
  REQUIRE(host.host);
  CHECK_FALSE(route.Admit(cohort, Input::kReady, true).host);
  CHECK(route.Admit(cohort, Input::kReady, true).decision == Decision::kDenied);
  const auto serial = host.host->serial();
  REQUIRE(serial);
  host.host.reset();
  auto later = route.Admit(cohort, Input::kProvenNoInitialSwap, true);
  REQUIRE(later.host);
  CHECK(later.host->serial() > serial);
  later.host->Cancel();
  CHECK_FALSE(later.host->TryEnter());
  later.host.reset();
  REQUIRE(route.Admit(cohort, Input::kReady, true).decision ==
          Decision::kNative);
  // A denied/failed subsequent native call cannot turn this into fallback.
  CHECK(route.Admit(cohort, Input::kUnknown, true).decision ==
        Decision::kDenied);
  CHECK(route.Admit(cohort, Input::kProvenNoInitialSwap, true).decision ==
        Decision::kDenied);
  CHECK(route.Admit(cohort, Input::kReady, true).decision == Decision::kNative);
}
TEST_CASE("UI input entry snapshot spans modal close and first Swap readiness",
          "[kernel][native_ui_route]") {
  Route route;
  const auto cohort = Cohort();
  gpu::NativeSystemTitleInputProvider provider;
  REQUIRE(provider.Enable(cohort.backend_epoch));
  using A = gpu::NativeSystemTitleInputProvider::Availability;
  REQUIRE(provider.ObserveAvailability(
              cohort.backend_epoch, cohort.foreground_generation,
              cohort.title_hmodule,
              cohort.title_load_generation) == A::kProvenNoInitialSwap);
  auto modal = route.Admit(cohort, Input::kProvenNoInitialSwap, true).host;
  REQUIRE(modal);
  // Real provider Stage/Take/Publish CPU mechanism. These host bytes do not
  // assert an actual GPU fence; production minting remains backend-private.
  gpu::xenos::xe_gpu_texture_fetch_t fetch{};
  fetch.type = gpu::xenos::FetchConstantType::kTexture;
  fetch.format = gpu::xenos::TextureFormat::k_8_8_8_8;
  fetch.dimension = gpu::xenos::DataDimension::k2DOrStacked;
  fetch.pitch = 2;
  fetch.base_address = 0x01000000 >> 12;
  fetch.size_2d.width = fetch.size_2d.height = 63;
  fetch.swizzle = 0x688;
  gpu::NativeSystemTitleInput::Swap swap{cohort.foreground_generation,
                                         cohort.title_hmodule,
                                         cohort.title_load_generation,
                                         {},
                                         64,
                                         64};
  std::memcpy(swap.fetch.data(), &fetch, sizeof(fetch));
  const auto layout = gpu::NativeSystemTitleInput::GetLayout(swap);
  REQUIRE(layout);
  const auto ticket = provider.Stage(swap);
  REQUIRE(ticket);
  const auto capture = provider.Take(ticket, layout->range.address, 64, 64);
  REQUIRE(capture);
  const auto input = gpu::test::NativeSystemTitleInputTestAccess::Create(
      capture->identity, capture->layout,
      std::vector<uint8_t>(capture->layout.range.length, 0x53));
  REQUIRE(input);
  REQUIRE(provider.Publish(input));
  REQUIRE(provider.ObserveAvailability(
              cohort.backend_epoch, cohort.foreground_generation,
              cohort.title_hmodule, cohort.title_load_generation) == A::kReady);
  CHECK(route.Admit(cohort, Input::kReady, true).decision == Decision::kDenied);
  auto input_entry = route.CurrentHost(cohort);
  REQUIRE(input_entry == modal);
  modal.reset();  // Completion/post released while original input PPC runs.
  CHECK(route.Admit(cohort, Input::kReady, true).decision == Decision::kDenied);
  CHECK(route.CurrentHost(cohort) == input_entry);
  input_entry.reset();
  CHECK(route.Admit(cohort, Input::kReady, true).decision == Decision::kNative);
  route.BeginStop();
  CHECK(route.Admit(cohort, Input::kReady, true).decision == Decision::kDenied);
  CHECK(route.Drain(Now()));
}
TEST_CASE(
    "UI admission excludes failed, unsupported and stopped backend capacity",
    "[kernel][native_ui_route]") {
  gpu::GpuCompletionQueue queue;
  gpu::NativeSystemFrameCapacity capacity;
  capacity.backend_epoch = queue.backend_epoch();
  capacity.backend_stop_proof = queue.backend_stop_proof();
  capacity.supported = true;
  capacity.immutable_input_snapshot_supported = true;
  using S = gpu::NativeSystemFrameCapacity::State;
  for (const auto state :
       {S::kAccepting, S::kQueued, S::kExecuting, S::kBackpressured}) {
    capacity.state = state;
    CHECK(capacity.supports_immutable_input_admission());
  }
  for (const auto state : {S::kUnsupported, S::kStopping, S::kFailed}) {
    capacity.state = state;
    CHECK_FALSE(capacity.supports_immutable_input_admission());
  }
  capacity.state = S::kAccepting;
  capacity.supported = false;
  CHECK_FALSE(capacity.supports_immutable_input_admission());
  capacity.supported = true;
  capacity.immutable_input_snapshot_supported = false;
  CHECK_FALSE(capacity.supports_immutable_input_admission());
  capacity.immutable_input_snapshot_supported = true;
  ++capacity.backend_epoch;
  CHECK_FALSE(capacity.supports_immutable_input_admission());
  capacity.backend_epoch = queue.backend_epoch();
  capacity.backend_stop_proof = {};
  CHECK_FALSE(capacity.supports_immutable_input_admission());
}
TEST_CASE("Canceled queued host factory cannot touch frontend after stop",
          "[kernel][native_ui_route][host_ui_fallback]") {
  Context app;
  Route route;
  auto lease = route.Admit(Cohort(), Input::kProvenNoInitialSwap, true).host;
  REQUIRE(lease);
  auto completion = std::make_shared<xam::HostUiFallbackCompletion>();
  unsigned frontend_accesses = 0;
  REQUIRE(xam::QueueHostUiFallback(app, lease, completion,
                                   [&](const auto&) { ++frontend_accesses; }));
  route.BeginStop();  // This is before existing ClearDialogs.
  REQUIRE(completion->closed.TryWait());  // No UI queue pumping required.
  REQUIRE(route.Drain(Now()));
  app.ExecutePendingFunctionsFromUIThread();
  CHECK(frontend_accesses == 0);
  CHECK_FALSE(completion->admitted.load());
  CHECK(completion->result().result == ui::TextEntryResult::kAborted);
}
TEST_CASE("Host factory rundown spans body and local owner destruction",
          "[kernel][native_ui_route][host_ui_fallback]") {
  Context app;
  Route route;
  auto lease = route.Admit(Cohort(), Input::kProvenNoInitialSwap, true).host;
  REQUIRE(lease);
  auto completion = std::make_shared<xam::HostUiFallbackCompletion>();
  std::mutex mutex;
  std::condition_variable changed;
  bool entered = false, release = false;
  std::atomic<bool> owner_destroyed{false}, early_drain{true},
      final_drain{false};
  struct Owner {
    std::atomic<bool>* destroyed;
    ~Owner() { destroyed->store(true); }
  };
  REQUIRE(xam::QueueHostUiFallback(app, lease, completion, [&](const auto&) {
    auto owner = std::make_unique<Owner>();
    owner->destroyed = &owner_destroyed;
    std::unique_lock lock(mutex);
    entered = true;
    changed.notify_all();
    changed.wait(lock, [&] { return release; });
  }));
  std::thread stop([&] {
    {
      std::unique_lock lock(mutex);
      changed.wait(lock, [&] { return entered; });
    }
    route.BeginStop();
    early_drain.store(route.Drain(Now()));
    {
      std::lock_guard lock(mutex);
      release = true;
      changed.notify_all();
    }
    final_drain.store(route.Drain(Now() + std::chrono::seconds(2)));
  });
  app.ExecutePendingFunctionsFromUIThread();
  stop.join();
  CHECK_FALSE(early_drain.load());
  CHECK(final_drain.load());
  CHECK(owner_destroyed.load());
  CHECK(completion->closed.TryWait());
}
TEST_CASE("Host factory drain refuses a nested callback ancestor",
          "[kernel][native_ui_route][host_ui_fallback]") {
  Context app;
  Route first, second;
  auto a = first.Admit(Cohort(), Input::kProvenNoInitialSwap, true).host;
  auto b = second.Admit(Cohort(), Input::kProvenNoInitialSwap, true).host;
  REQUIRE(a);
  REQUIRE(b);
  bool ancestor_drain = true, self_drain = true;
  REQUIRE(xam::QueueHostUiFallback(
      app, a, std::make_shared<xam::HostUiFallbackCompletion>(),
      [&](const auto&) {
        REQUIRE(xam::QueueHostUiFallback(
            app, b, std::make_shared<xam::HostUiFallbackCompletion>(),
            [&](const auto&) {
              ancestor_drain = first.Drain(Now());
              self_drain = second.Drain(Now());
            }));
        app.ExecutePendingFunctionsFromUIThread();
      }));
  app.ExecutePendingFunctionsFromUIThread();
  CHECK_FALSE(ancestor_drain);
  CHECK_FALSE(self_drain);
  first.BeginStop();
  second.BeginStop();
  CHECK(first.Drain(Now()));
  CHECK(second.Drain(Now()));
}
}  // namespace xe::kernel::test
