/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia contributors. All rights reserved.                    *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */
#include "xenia/base/native_ui_backend_route.h"
#include "xenia/kernel/native_system_ui_public_calls.h"
#include "xenia/kernel/native_system_ui_service.h"
#include "xenia/kernel/guest_runtime_plan.h"
#include "xenia/cpu/ppc/ppc_context.h"
#include "xenia/cpu/export_resolver.h"
#include "xenia/kernel/xam/xam_module.h"
#include "xenia/kernel/xam/xam_private.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <memory>
#include <new>
#include <mutex>
#include <thread>
#include <vector>
#include "third_party/catch/include/catch.hpp"
#include "xenia/gpu/command_processor.h"
#include "xenia/gpu/native_system_title_input_provider.h"
#include "xenia/kernel/testing/native_system_title_input_test_access.h"
#include "xenia/kernel/xam/host_ui_fallback.h"
#include "xenia/ui/windowed_app_context.h"
#include "xenia/xbox.h"

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
// CPU ownership endpoints only. A production no-op/queue decision still
// requires the complete original native helper/sender observation; these tests
// do not mint that receipt, native visibility, account selection or readiness.
TEST_CASE("Native signin no-op retires only creation while queued request awaits visibility",
          "[kernel][native_ui_route][native_signin]") {
  Route gate,prepared;
  auto epoch=std::make_shared<const Route::HostOnlyEpoch>();
  const bool queued=GENERATE(false,true);
  auto ingress=gate.ReserveNativeCreation(epoch);REQUIRE(ingress);
  REQUIRE(prepared.Admit(Cohort(),Input::kReady,true).decision==Decision::kNative);
  NativeSigninCreationFacts facts;
  facts.armed=true;facts.entered=true;facts.returned=true;
  facts.sender_entered=queued;facts.sender_returned=queued;
  const auto decision=DecideNativeSigninCreation(facts,true,true,0);
  REQUIRE(decision==(queued?NativeSigninCreationDecision::kQueued:NativeSigninCreationDecision::kNoUi));
  // A normal authenticated public continuation proves completion without
  // fabricating an own-return flag (A64 tail-restore can bypass that hook).
  auto incomplete=facts;incomplete.returned=false;incomplete.sender_returned=false;
  CHECK(DecideNativeSigninCreation(incomplete,true,true,0)==decision);
  CHECK_FALSE(incomplete.returned);CHECK_FALSE(incomplete.sender_returned);
  incomplete=facts;incomplete.entered=false;
  CHECK(DecideNativeSigninCreation(incomplete,true,true,0)==NativeSigninCreationDecision::kUncertain);
  CHECK(DecideNativeSigninCreation(facts,true,false,0)==NativeSigninCreationDecision::kUncertain);
  CHECK(DecideNativeSigninCreation(facts,false,true,0)==NativeSigninCreationDecision::kUncertain);
  incomplete=facts;incomplete.sender_entered=false;incomplete.sender_returned=true;
  CHECK(DecideNativeSigninCreation(incomplete,true,true,0)==NativeSigninCreationDecision::kUncertain);
  incomplete=facts;incomplete.lost=true;
  CHECK(DecideNativeSigninCreation(incomplete,true,true,0)==NativeSigninCreationDecision::kUncertain);
  CHECK(DecideNativeSigninCreation(facts,true,true,5)==NativeSigninCreationDecision::kUncertain);
  if(queued){
    incomplete=facts;incomplete.sender_returned=false;
    CHECK(DecideNativeSigninCreation(incomplete,true,true,0)==NativeSigninCreationDecision::kQueued);
    incomplete=facts;incomplete.sender_value=0x80004005;
    CHECK(DecideNativeSigninCreation(incomplete,true,true,0)==NativeSigninCreationDecision::kUncertain);
    incomplete.helper_value=1627;
    CHECK(DecideNativeSigninCreation(incomplete,true,true,1627)==NativeSigninCreationDecision::kNoUi);
    incomplete.returned=false; // exact adapter proof must still crosscheck raw sender
    CHECK(DecideNativeSigninCreation(incomplete,true,true,5)==NativeSigninCreationDecision::kUncertain);
    incomplete.sender_value=0x8007000E;
    CHECK(DecideNativeSigninCreation(incomplete,true,true,1627)==NativeSigninCreationDecision::kUncertain);
    CHECK(DecideNativeSigninCreation(incomplete,true,true,14)==NativeSigninCreationDecision::kNoUi);
    incomplete.sender_returned=false;
    CHECK(DecideNativeSigninCreation(incomplete,true,true,5)==NativeSigninCreationDecision::kUncertain);
  }
  ingress->MarkExecutionAttempt();ingress->Finish(decision==NativeSigninCreationDecision::kQueued);
  auto next=gate.ReserveNativeCreation(epoch);
  CHECK(bool(next)==!queued);
  // Releasing a temporary no-op claim never unseals the prepared native route.
  CHECK(prepared.Admit(Cohort(),Input::kProvenNoInitialSwap,true).decision==Decision::kDenied);
  if(queued){
    gate.ObserveNativeUiVisibility(true);
    next=gate.ReserveNativeCreation(epoch);REQUIRE(next);next->Finish(false);
    gate.ObserveNativeUiVisibility(false);
  }else next->Finish(false);
  CHECK(prepared.Admit(Cohort(),Input::kProvenNoInitialSwap,true).decision==Decision::kDenied);
}
TEST_CASE("Selected signin cancellation retires ingress before publication and delayed factory",
          "[kernel][native_ui_route][native_signin][host_ui_fallback]") {
  Context app;Route gate,prepared;
  auto epoch=std::make_shared<const Route::HostOnlyEpoch>();
  auto ingress=gate.ReserveNativeCreation(epoch);REQUIRE(ingress);
  auto selected=prepared.Admit(Cohort(),Input::kProvenNoInitialSwap,true).host;
  REQUIRE(selected);selected->RetainHostIngress(std::move(ingress));
  auto delayed=selected;
  auto completion=std::make_shared<xam::HostUiFallbackCompletion>();
  uint32_t factories=0;
  REQUIRE(xam::QueueHostUiFallback(app,selected,completion,[&](const auto&){++factories;}));
  auto finish=std::make_shared<xam::HostUiFallbackOperationCompletion>(selected);
  auto run=[selected,completion,finish=std::move(finish)]() mutable {
    auto before_publication=std::move(finish);
    CHECK(completion->closed.TryWait());
    return selected->TryEnter()?X_ERROR_SUCCESS:X_ERROR_CANCELLED;
  };
  selected->Cancel();
  // Fence auto-resets: the dispatch run is the sole consuming waiter.
  CHECK(run()==X_ERROR_CANCELLED);
  // Actual production dispatch publishes only AFTER run's completion guard.
  auto next=gate.ReserveNativeCreation(epoch);REQUIRE(next);next->Finish(false);
  CHECK_FALSE(delayed->TryEnter());
  app.ExecutePendingFunctionsFromUIThread();CHECK(factories==0);
  CHECK_FALSE(completion->admitted.load());
}

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

namespace xe::kernel::test {
TEST_CASE("Explicit host-only epoch neither invents native readiness nor crosses Kernels",
          "[kernel][native_ui_route][host_ui_fallback]") {
  Route route;
  auto first = std::make_shared<const Route::HostOnlyEpoch>();
  auto replacement = std::make_shared<const Route::HostOnlyEpoch>();
  CHECK_FALSE(route.AdmitHostOnly({}, 0).host);
  auto host = route.AdmitHostOnly(first, 0).host;
  REQUIRE(host);
  CHECK_FALSE(host->cohort().valid());
  CHECK_FALSE(route.AdmitHostOnly(first, 0).host);
  CHECK(route.Admit(Cohort(), Input::kReady, true).decision == Decision::kDenied);
  host.reset();
  CHECK_FALSE(route.AdmitHostOnly(replacement, 0).host);
  auto title = route.AdmitHostOnly(first, 23).host;
  REQUIRE(title);
  title->FinishHostOnly();
  CHECK_FALSE(title->TryEnter());
  auto next = route.AdmitHostOnly(first, 23).host;
  REQUIRE(next);  // Retained stale dialog/result cannot hold the next modal.
  title = next;
  unsigned canceled = 0;
  title->SetCancellation([&] { ++canceled; });
  auto entered = title->TryEnter();
  REQUIRE(entered);
  route.BeginStop();
  CHECK(canceled == 1);
  CHECK_FALSE(title->TryEnter());
  CHECK_FALSE(route.Drain(Now()));  // Own/nested callback may not self-drain.
  entered.reset();
  CHECK(route.Drain(Now()));
  CHECK_FALSE(route.AdmitHostOnly(first, 24).host);
  Route native;
  REQUIRE(native.Admit(Cohort(), Input::kReady, true).decision == Decision::kNative);
  CHECK_FALSE(native.AdmitHostOnly(first, 23).host);
}
}  // namespace xe::kernel::test

namespace xe::kernel::test {
TEST_CASE("Shared Kernel metadata admits exactly one native-or-legacy creating ingress",
          "[kernel][native_ui_route][host_ui_fallback][interleaving]") {
  Route gate;
  auto epoch = std::make_shared<const Route::HostOnlyEpoch>();
  // Native wins the short admission race, before any input/UI publication.
  auto native = gate.ReserveNativeCreation(epoch);
  REQUIRE(native);
  CHECK_FALSE(gate.AdmitHostOnly(epoch, 17).host);
  CHECK_FALSE(gate.ReserveNativeCreation(epoch));
  native->Finish(true);  // Real native call returned queued, no SYS_UI yet.
  CHECK_FALSE(gate.AdmitHostOnly(epoch, 17).host);
  CHECK_FALSE(gate.ReserveNativeCreation(epoch));
  gate.ObserveNativeUiVisibility(true);
  CHECK_FALSE(gate.AdmitHostOnly(epoch, 17).host);
  gate.ObserveNativeUiVisibility(false);
  auto host = gate.AdmitHostOnly(epoch, 17).host;
  REQUIRE(host);
  // Host wins the opposite interleaving. Native cannot pass a stale flag read
  // because its reservation uses this SAME State mutex and host record.
  CHECK_FALSE(gate.ReserveNativeCreation(epoch));
  host->FinishHostOnly();
  auto synchronous = gate.ReserveNativeCreation(epoch);
  REQUIRE(synchronous);
  gate.ObserveNativeUiVisibility(true);
  gate.ObserveNativeUiVisibility(false);
  synchronous->Finish(true);  // Actual open+close already observed inside call.
  REQUIRE(gate.AdmitHostOnly(epoch, 17).host);
}
TEST_CASE("Native-selected host fallback retains shared ingress through queue and modal",
          "[kernel][native_ui_route][host_ui_fallback][interleaving]") {
  Route gate;
  auto epoch = std::make_shared<const Route::HostOnlyEpoch>();
  auto ingress = gate.ReserveNativeCreation(epoch);
  REQUIRE(ingress);
  Route prepared;
  auto fallback = prepared.Admit(Cohort(), Input::kProvenNoInitialSwap, true).host;
  REQUIRE(fallback);
  fallback->RetainHostIngress(std::move(ingress));
  CHECK_FALSE(gate.AdmitHostOnly(epoch, 19).host);
  CHECK_FALSE(gate.ReserveNativeCreation(epoch));
  fallback->Cancel();
  CHECK_FALSE(gate.AdmitHostOnly(epoch, 19).host);
  fallback.reset();
  auto host = gate.AdmitHostOnly(epoch, 19).host;
  REQUIRE(host);
  host->FinishHostOnly();
  auto old = gate.ReserveNativeCreation(epoch);
  REQUIRE(old);
  gate.BeginStop();
  old->Finish(true);  // Stop wins before native publication.
  CHECK_FALSE(gate.AdmitHostOnly(epoch, 20).host);
  CHECK_FALSE(gate.ReserveNativeCreation(epoch));
  Route replacement;
  auto fresh = std::make_shared<const Route::HostOnlyEpoch>();
  auto current = replacement.AdmitHostOnly(fresh, 20).host;
  REQUIRE(current);
  old.reset();
  CHECK(current->TryEnter());
  CHECK_FALSE(replacement.ReserveNativeCreation(epoch));
}
}  // namespace xe::kernel::test

namespace xe::kernel::test {
TEST_CASE("Uncertain native creating execution keeps legacy ingress closed until observation",
          "[kernel][native_ui_route][host_ui_fallback][interleaving]") {
  Route gate;
  auto epoch = std::make_shared<const Route::HostOnlyEpoch>();
  auto call = gate.ReserveNativeCreation(epoch);
  REQUIRE(call);
  call->MarkExecutionAttempt();
  call.reset();  // Exception/uncertain return cannot silently permit host replay.
  CHECK_FALSE(gate.AdmitHostOnly(epoch, 31).host);
  gate.ObserveNativeUiVisibility(false);
  REQUIRE(gate.AdmitHostOnly(epoch, 31).host);
}
}  // namespace xe::kernel::test

namespace xe::kernel::test {
TEST_CASE("Completed selected host modal frees both exact slots while delayed references survive",
          "[kernel][native_ui_route][host_ui_fallback][completion][interleaving]") {
  Route gate;
  auto epoch = std::make_shared<const Route::HostOnlyEpoch>();
  auto ingress = gate.ReserveNativeCreation(epoch);
  REQUIRE(ingress);
  std::vector<bool> visibility;
  Route prepared([&](bool visible) { visibility.push_back(visible); });
  const auto cohort = Cohort();
  auto old = prepared.Admit(cohort, Input::kProvenNoInitialSwap, true).host;
  REQUIRE(old);
  old->RetainHostIngress(std::move(ingress));
  auto delayed_post = old;
  auto delayed_dialog = old;
  old->SetVisible(true);
  old->SetVisible(false);  // Existing actual close observation precedes retire.
  old->FinishHostOperation();
  CHECK(old->cancelled());
  CHECK_FALSE(old->TryEnter());
  REQUIRE(visibility == std::vector<bool>{true, false});

  // Both the Kernel gate and prepared route admit the next request now,
  // without waiting for the old 200ms post or dialog/input references to die.
  auto next_ingress = gate.ReserveNativeCreation(epoch);
  REQUIRE(next_ingress);
  auto next = prepared.Admit(cohort, Input::kProvenNoInitialSwap, true).host;
  REQUIRE(next);
  next->RetainHostIngress(std::move(next_ingress));
  REQUIRE(prepared.CurrentHost(cohort) == next);
  next->SetVisible(true);
  delayed_post->SetVisible(false);
  delayed_dialog->FinishHostOperation();
  delayed_dialog->Cancel();
  CHECK_FALSE(delayed_post->TryEnter());
  CHECK(prepared.CurrentHost(cohort) == next);
  CHECK(next->TryEnter());
  CHECK_FALSE(gate.ReserveNativeCreation(epoch));
  REQUIRE(visibility == std::vector<bool>{true, false, true});
  delayed_post.reset();
  delayed_dialog.reset();
  old.reset();
  CHECK(prepared.CurrentHost(cohort) == next);
  CHECK_FALSE(gate.ReserveNativeCreation(epoch));

  next->SetVisible(false);
  next->FinishHostOperation();
  auto native_request = gate.ReserveNativeCreation(epoch);
  REQUIRE(native_request);
  CHECK(prepared.Admit(cohort, Input::kReady, true).decision == Decision::kNative);
  // Completion never resets the original cohort/readiness seal into a host
  // replay. The retained old lease cannot cancel the new creating receipt.
  CHECK(prepared.Admit(cohort, Input::kProvenNoInitialSwap, true).decision ==
        Decision::kDenied);
  native_request->Finish(false);
  auto host = gate.AdmitHostOnly(epoch, 43).host;
  REQUIRE(host);
  host->FinishHostOnly();
}
}  // namespace xe::kernel::test


namespace xe::kernel::test {
TEST_CASE("SYSTEM Editor loader ABI preserves actual paired stack edges",
          "[kernel][native_ui][editor_handoff][abi]") {
  const auto layout=CaptureNativeEditorLoaderStack(0x70001000,0x70000000,0x70002000);
  REQUIRE(layout);
  CHECK(layout->producer_sp==0x70000BD0);
  CHECK(layout->data==0x70000ED0);
  CHECK(layout->path==0x70000C90);
  CHECK(MatchesNativeEditorLoaderEdge(false,layout->producer_sp,0x70000BD0,0x817F5744));
  CHECK(MatchesNativeEditorLoaderEdge(true,layout->producer_sp,0x70000BD0,0x817F5758));
  CHECK_FALSE(MatchesNativeEditorLoaderEdge(false,layout->producer_sp,0x70000BD0,0x817F5758));
  CHECK_FALSE(MatchesNativeEditorLoaderEdge(true,layout->producer_sp,0x70000BC0,0x817F5758));
  CHECK_FALSE(MatchesNativeEditorLoaderEdge(true,layout->producer_sp,0x170000BD0ull,0x817F5758));
  CHECK_FALSE(MatchesNativeEditorLoaderEdge(true,layout->producer_sp,0x70000BD0,0x1817F5758ull));
  CHECK_FALSE(CaptureNativeEditorLoaderStack(0x420,0,0x70002000));
  CHECK_FALSE(CaptureNativeEditorLoaderStack(0x70001004,0x70000000,0x70002000));
  CHECK_FALSE(CaptureNativeEditorLoaderStack(0x70001000,0x70000BD1,0x70002000));
  CHECK_FALSE(CaptureNativeEditorLoaderStack(0x70001000,0x70000000,0x70000FFF));
  CHECK_FALSE(CaptureNativeEditorLoaderStack(0x70001000,0x70002000,0x70000000));
  // Exercise the actual refusal policy: BOOL false must not be a nonzero
  // HRESULT, while the producer's HRESULT still reports failure.
  CHECK_FALSE(bool(NativeEditorLoaderRefusal(true)));
  CHECK(XFAILED(NativeEditorLoaderRefusal(false)));
}
TEST_CASE("Editor loader pair requires both real deferred SYSTEM source contracts",
          "[kernel][native_ui][editor_handoff][plan]") {
  ValidatedGuestRuntimePlan plan;
  for (const auto& body:NativeEditorLoaderCodeProfiles()) {
    if (!body.arguments) continue;
    GuestRuntime::Function function;
    function.identity=std::string(body.identity);function.address=body.address;
    function.arguments.assign(body.arguments,0);
    function.return_kind=GuestRuntime::ReturnKind::kUInt32;
    plan.catalog.emplace(function.identity,function);
    ValidatedGuestRuntimePlan::Contract contract;
    contract.caller=ValidatedGuestRuntimePlan::CallerPolicy::kSystemTaskOnly;
    contract.result=ValidatedGuestRuntimePlan::ResultPolicy::kAny;
    plan.contracts.emplace(function.identity,contract);
  }
  REQUIRE(NativeEditorLoaderPlanPairValid(plan));
  REQUIRE(plan.catalog.size()==2);
  for (const auto& [name,function]:plan.catalog) {
    auto changed=plan;changed.catalog.erase(name);CHECK_FALSE(NativeEditorLoaderPlanPairValid(changed));
    changed=plan;changed.contracts.erase(name);CHECK_FALSE(NativeEditorLoaderPlanPairValid(changed));
    changed=plan;changed.catalog.at(name).address+=4;CHECK_FALSE(NativeEditorLoaderPlanPairValid(changed));
    changed=plan;changed.catalog.at(name).arguments[0]=1;CHECK_FALSE(NativeEditorLoaderPlanPairValid(changed));
    changed=plan;changed.catalog.at(name).arguments.pop_back();CHECK_FALSE(NativeEditorLoaderPlanPairValid(changed));
    changed=plan;changed.catalog.at(name).return_kind=GuestRuntime::ReturnKind::kVoid;CHECK_FALSE(NativeEditorLoaderPlanPairValid(changed));
    changed=plan;changed.contracts.at(name).caller=ValidatedGuestRuntimePlan::CallerPolicy::kOriginalTitleOrSystem;CHECK_FALSE(NativeEditorLoaderPlanPairValid(changed));
    changed=plan;changed.contracts.at(name).result=ValidatedGuestRuntimePlan::ResultPolicy::kZero;CHECK_FALSE(NativeEditorLoaderPlanPairValid(changed));
    changed=plan;changed.contracts.at(name).before.push_back({0x1000,0,true});CHECK_FALSE(NativeEditorLoaderPlanPairValid(changed));
    changed=plan;changed.contracts.at(name).after.push_back({0x1000,0,true});CHECK_FALSE(NativeEditorLoaderPlanPairValid(changed));
    changed=plan;changed.plan.phases.push_back({"not initialization",{function}});CHECK_FALSE(NativeEditorLoaderPlanPairValid(changed));
  }
  // Metadata never enables the public family or creates a Task/launch owner.
  CHECK_FALSE(NativeSystemPublicFamilyRouteEnabled(NativeSystemPublicFamily::kAvatarEditor));
  cpu::ExportResolver resolver;
  xam::RegisterAvatarExports(&resolver,nullptr);
  xam::XamModule::RegisterExportTable(&resolver);
  const auto* host=resolver.GetExportByOrdinal("xam.xex",0x7DB);
  REQUIRE(host);
  CHECK(host->is_implemented());
  CHECK(host->function_data.trampoline!=nullptr);
}
TEST_CASE("Unclaimed native loader calls keep their original body and registers",
          "[kernel][native_ui][editor_handoff][passthrough]") {
  cpu::ppc::PPCContext context{};
  context.r[3]=0x1122334455667788ull;
  CHECK(NativeSystemUiService::GuardEditorLoaderEntry(nullptr,0x816A0D70)==1);
  for (const auto address:{0x8169F4A8ull,0x816A0D70ull}) {
    CHECK(NativeSystemUiService::GuardEditorLoaderEntry(&context,address)==1);
    CHECK(context.r[3]==0x1122334455667788ull);
  }
}
} // namespace xe::kernel::test

namespace xe::kernel::test {
TEST_CASE("Host signin selection freezes exact values and rejects duplicate or partial choices",
          "[kernel][native_ui_route][host_ui_fallback][selection]") {
  xam::HostUiProfileSelection selection;
  CHECK_FALSE(selection.valid());
  selection.mask=5;selection.xuids={11,0,33,0};REQUIRE(selection.valid());
  xam::HostUiFallbackCompletion completion;
  xam::HostUiFallbackCompletion::Result result;
  result.result=ui::TextEntryResult::kAccepted;result.signin=selection;
  completion.Finish(result);
  selection.xuids[0]=99;result.signin.xuids[2]=99;
  const auto frozen=completion.result();
  CHECK(frozen.signin.xuids[0]==11);CHECK(frozen.signin.xuids[2]==33);
  selection=frozen.signin;selection.xuids[2]=11;CHECK_FALSE(selection.valid());
  selection=frozen.signin;selection.mask|=16;CHECK_FALSE(selection.valid());
  selection=frozen.signin;selection.xuids[1]=22;CHECK_FALSE(selection.valid());
  selection=frozen.signin;selection.xuids[2]=0;CHECK_FALSE(selection.valid());
}
TEST_CASE("Owned host UI ticket releases input holders before real result completion exactly once",
          "[kernel][native_ui_route][host_ui_fallback][completion]") {
  xam::HostUiFallbackWork ticket;
  uint32_t runs=0,cancels=0,completes=0,posts=0;
  X_RESULT actual=X_ERROR_SUCCESS;
  auto held=std::make_shared<uint32_t>(7);std::weak_ptr<uint32_t> weak=held;
  std::function<X_RESULT()> run=[held,&runs]{++runs;return X_ERROR_BUSY;};
  held.reset();
  std::function<void()> cancel=[&]{++cancels;};
  std::function<void(X_RESULT)> complete=[&](X_RESULT result){
    CHECK(weak.expired());++completes;actual=result;CHECK(posts==0);
  };
  std::function<void()> post=[&]{CHECK(completes==1);++posts;};
  ticket.Initialize(std::move(run),std::move(cancel),std::move(complete),std::move(post));
  ticket.Run();ticket.Run();ticket.Reject(X_ERROR_CANCELLED);
  CHECK(runs==1);CHECK(cancels==0);CHECK(completes==1);CHECK(posts==1);CHECK(actual==X_ERROR_BUSY);
}
TEST_CASE("Owned host UI rejection and body failure retain truthful distinct outcomes",
          "[kernel][native_ui_route][host_ui_fallback][completion]") {
  const auto reason=GENERATE(X_ERROR_CANCELLED,X_ERROR_NOT_ENOUGH_MEMORY,X_ERROR_FUNCTION_FAILED);
  xam::HostUiFallbackWork ticket;uint32_t runs=0,cancels=0,completes=0;
  X_RESULT actual=X_ERROR_SUCCESS;
  std::function<X_RESULT()> run=[&]() -> X_RESULT {++runs;throw std::bad_alloc();};
  std::function<void()> cancel=[&]{++cancels;};
  std::function<void(X_RESULT)> complete=[&](X_RESULT result){++completes;actual=result;};
  std::function<void()> post;
  ticket.Initialize(std::move(run),std::move(cancel),std::move(complete),std::move(post));
  if(reason==X_ERROR_FUNCTION_FAILED)ticket.Run();else ticket.Reject(reason);
  ticket.Reject(X_ERROR_SUCCESS);ticket.Run();
  CHECK(actual==reason);CHECK(completes==1);CHECK(cancels==1);
  CHECK(runs==(reason==X_ERROR_FUNCTION_FAILED?1:0));
}
}  // namespace xe::kernel::test

namespace xe::kernel::test {
TEST_CASE("Owned host UI drop releases explicit cleanup before completion even with retained body references",
          "[kernel][native_ui_route][host_ui_fallback][completion]") {
  xam::HostUiFallbackWork ticket;
  auto held=std::make_shared<uint32_t>(1);bool released=false,completed=false;
  std::function<X_RESULT()> work=[held]{return X_ERROR_SUCCESS;};
  std::function<void()> cancel=[&]{released=true;};
  std::function<void(X_RESULT)> complete=[&](X_RESULT result){
    CHECK(released);CHECK(result==X_ERROR_CANCELLED);CHECK(held.use_count()==1);completed=true;
  };
  std::function<void()> post;
  ticket.Initialize(std::move(work),std::move(cancel),std::move(complete),std::move(post));
  ticket.Reject(X_ERROR_CANCELLED);CHECK(completed);CHECK(released);
}
}  // namespace xe::kernel::test

namespace xe::kernel::test {
TEST_CASE("Signin actual failure keeps the modal operation open and retry rejects stale attempt outcomes",
          "[kernel][native_ui_route][host_ui_fallback][signin_attempt]") {
  xam::HostUiFallbackCompletion completed;
  xam::HostUiProfileSelection first;first.mask=1;first.xuids[0]=11;
  const auto id=completed.StartSigninAttempt(first);REQUIRE(id);
  first.xuids[0]=99;
  const auto active=completed.TakeSigninAttempt();REQUIRE(active);
  CHECK(active->selection.xuids[0]==11);
  CHECK_FALSE(completed.StartSigninAttempt(first));
  REQUIRE(completed.CompleteSigninAttempt(id,X_ERROR_BUSY,true));
  CHECK_FALSE(completed.closed_requested());
  CHECK(completed.signin_attempt().status==X_ERROR_BUSY);
  const auto retry=completed.StartSigninAttempt(first);REQUIRE(retry>id);
  CHECK_FALSE(completed.CompleteSigninAttempt(id,X_ERROR_SUCCESS,true));
  CHECK(completed.signin_attempt().id==retry);
  CHECK(completed.signin_attempt().phase==xam::HostUiFallbackCompletion::SigninPhase::kQueued);
  const auto second=completed.TakeSigninAttempt();REQUIRE(second);
  CHECK(second->selection.xuids[0]==99);
  REQUIRE(completed.CompleteSigninAttempt(retry,X_ERROR_SUCCESS,true));
  CHECK_FALSE(completed.closed_requested()); // existing UI must observe success then close
  xam::HostUiFallbackCompletion::Result closed;
  closed.result=ui::TextEntryResult::kAccepted;completed.Finish(closed);
  CHECK(completed.closed_requested());CHECK(completed.result().status==X_ERROR_SUCCESS);
}
TEST_CASE("Signin close before adoption rejects late work and wakes only the request progress waiter",
          "[kernel][native_ui_route][host_ui_fallback][signin_attempt][cancel]") {
  const bool queued=GENERATE(false,true);
  xam::HostUiFallbackCompletion completed;
  xam::HostUiProfileSelection selection;selection.mask=8;selection.xuids[3]=44;
  const auto id=queued?completed.StartSigninAttempt(selection):0;
  xam::HostUiFallbackCompletion::Result cancel;cancel.result=ui::TextEntryResult::kCanceled;
  completed.Finish(cancel);
  CHECK(completed.closed.TryWait());CHECK(completed.progress.TryWait());
  CHECK_FALSE(completed.TakeSigninAttempt());CHECK_FALSE(completed.StartSigninAttempt(selection));
  CHECK_FALSE(completed.CompleteSigninAttempt(id,X_ERROR_SUCCESS,true));
  CHECK(completed.result().result==ui::TextEntryResult::kCanceled);
}
TEST_CASE("Signin cancel retains a real executing outcome and never fabricates rollback or unexecuted success",
          "[kernel][native_ui_route][host_ui_fallback][signin_attempt][cancel]") {
  const auto actual=GENERATE(X_ERROR_SUCCESS,X_ERROR_CANCELLED,X_ERROR_FUNCTION_FAILED);
  const bool close_before_return=GENERATE(false,true);
  xam::HostUiFallbackCompletion completed;
  xam::HostUiProfileSelection selection;selection.mask=2;selection.xuids[1]=22;
  const auto id=completed.StartSigninAttempt(selection);REQUIRE(completed.TakeSigninAttempt());
  xam::HostUiFallbackCompletion::Result cancel;cancel.result=ui::TextEntryResult::kCanceled;
  if(close_before_return)completed.Finish(cancel);
  REQUIRE(completed.CompleteSigninAttempt(id,actual,true));
  if(!close_before_return)completed.Finish(cancel);
  const auto result=completed.result();
  CHECK(completed.signin_attempt().executed);
  CHECK(completed.signin_attempt().status==actual);
  CHECK((result.result==ui::TextEntryResult::kAccepted)==(actual==X_ERROR_SUCCESS));
  if(actual==X_ERROR_SUCCESS){CHECK(result.status==X_ERROR_SUCCESS);CHECK(result.signin.xuids[1]==22);}
  completed.Finish(cancel);CHECK(completed.result().result==result.result);
  xam::HostUiFallbackCompletion unexecuted;
  const auto unused=unexecuted.StartSigninAttempt(selection);REQUIRE(unexecuted.TakeSigninAttempt());
  REQUIRE(unexecuted.CompleteSigninAttempt(unused,X_ERROR_SUCCESS,false));
  CHECK(unexecuted.signin_attempt().status==X_ERROR_FUNCTION_FAILED);
}
TEST_CASE("Existing signin worker consumes a failed attempt then a fresh retry without completing guest operation early",
          "[kernel][native_ui_route][host_ui_fallback][signin_attempt][worker]") {
  Route route;const auto epoch=std::make_shared<const Route::HostOnlyEpoch>();
  const auto lease=route.AdmitHostOnly(epoch,1).host;REQUIRE(lease);
  const auto completed=std::make_shared<xam::HostUiFallbackCompletion>();
  lease->SetCancellation([weak=std::weak_ptr<xam::HostUiFallbackCompletion>(completed)]{if(auto value=weak.lock())value->Finish({});});
  uint32_t calls=0;X_RESULT result=X_ERROR_FUNCTION_FAILED;
  struct Joined {
    std::shared_ptr<xam::HostUiFallbackCompletion> completed;
    std::thread worker;
    ~Joined(){completed->Finish({});if(worker.joinable())worker.join();}
  } joined{completed,std::thread([&]{
    result=xam::RunHostSigninAttempts(lease,completed,
        [&](const xam::HostUiProfileSelection& selected,const std::function<bool()>& canceled){
          CHECK_FALSE(canceled());++calls;
          CHECK(selected.xuids[0]==(calls==1?11:22));
          return calls==1?X_ERROR_BUSY:X_ERROR_SUCCESS;
        });
  })};
  const auto returned=[&](uint64_t id){
    const auto deadline=Now()+std::chrono::seconds(2);
    do{const auto value=completed->signin_attempt();
      if(value.id==id && value.phase==xam::HostUiFallbackCompletion::SigninPhase::kReturned)return true;
      std::this_thread::yield();}while(Now()<deadline);return false;
  };
  xam::HostUiProfileSelection selection;selection.mask=1;selection.xuids[0]=11;
  const auto first=completed->StartSigninAttempt(selection);REQUIRE(returned(first));
  CHECK_FALSE(completed->closed_requested());CHECK(completed->signin_attempt().status==X_ERROR_BUSY);
  selection.xuids[0]=22;const auto next=completed->StartSigninAttempt(selection);REQUIRE(next>first);
  REQUIRE(returned(next));CHECK_FALSE(completed->closed_requested());
  xam::HostUiFallbackCompletion::Result accepted;accepted.result=ui::TextEntryResult::kAccepted;
  completed->Finish(accepted);joined.worker.join();
  CHECK(calls==2);CHECK(result==X_ERROR_SUCCESS);
}
TEST_CASE("Signin worker observes UI cancel during an actual pending publish and leaves exact cleanup owned",
          "[kernel][native_ui_route][host_ui_fallback][signin_attempt][worker][cancel]") {
  Route route;const auto epoch=std::make_shared<const Route::HostOnlyEpoch>();
  const auto lease=route.AdmitHostOnly(epoch,1).host;REQUIRE(lease);
  const auto completed=std::make_shared<xam::HostUiFallbackCompletion>();
  threading::Fence entered,release;std::atomic<bool> canceled{false};X_RESULT result=X_ERROR_SUCCESS;
  struct Joined {
    std::shared_ptr<xam::HostUiFallbackCompletion> completed;threading::Fence& release;
    std::thread worker;
    ~Joined(){completed->Finish({});release.Signal();if(worker.joinable())worker.join();}
  } joined{completed,release,std::thread([&]{
    result=xam::RunHostSigninAttempts(lease,completed,
        [&](const xam::HostUiProfileSelection&,const std::function<bool()>& stop){
          entered.Signal();release.Wait();canceled.store(stop());return X_ERROR_CANCELLED;
        });
  })};
  xam::HostUiProfileSelection selection;selection.mask=1;selection.xuids[0]=11;
  REQUIRE(completed->StartSigninAttempt(selection));
  const auto deadline=Now()+std::chrono::seconds(2);bool seen=false;
  do{seen=entered.TryWait();if(!seen)std::this_thread::yield();}while(!seen && Now()<deadline);
  REQUIRE(seen);completed->Finish({});release.Signal();joined.worker.join();
  CHECK(canceled.load());CHECK(result==X_ERROR_CANCELLED);
  CHECK(completed->signin_attempt().phase==xam::HostUiFallbackCompletion::SigninPhase::kReturned);
}
}  // namespace xe::kernel::test
