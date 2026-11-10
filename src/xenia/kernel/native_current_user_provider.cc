/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia contributors. All rights reserved.                    *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */
#include "xenia/kernel/native_current_user_provider.h"
#include <array>
#include <map>
#include <mutex>
#include <tuple>
#include "xenia/cpu/export_resolver.h"
#include "xenia/cpu/processor.h"
#include "xenia/cpu/xex_module.h"
#include "xenia/kernel/guest_runtime_call_executor.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/native_current_user_presets.h"
#include "xenia/kernel/user_module.h"
#include "xenia/kernel/xthread.h"
namespace xe::kernel {
namespace {
struct Profile {
  uint16_t ordinal;
  uint32_t address, bytes;
  const char* sha;
  const char* name;
};
constexpr std::array<Profile, 3> profiles{{
    {0x210, 0x816DD118, 212,
     "ee6a07ff6b488735f293f41f4832491809c449bdad5aebcddab8b188b7a007e4",
     "XamUserGetSigninState"},
    {0x214, 0x816D8878, 20,
     "a8cbf6802444de76dab277f4ce34b6f2976c15e1e214cdcc53d023dcbc0b59e4",
     "XamSetUserPresetPresenceState"},
    {0x215, 0x816D8860, 20,
     "19a18dad5cc988e265d113f372fc5778801fa72eeaf75a1464ad06d356710087",
     "XamGetUserPresetPresenceState"},
}};
std::mutex registry_mutex;
using Key = std::tuple<KernelState*, uint32_t, uint64_t>;
std::map<Key, std::weak_ptr<NativeCurrentUserProviderBinding>> registry;
}  // namespace
struct NativeCurrentUserProviderBinding::State {
  enum class Phase { kInstalling, kBound, kFailed };
  std::shared_ptr<GuestRuntimeCallExecutor> calls;
  std::shared_ptr<NativeCurrentUserPresets> presets;
  mutable std::mutex mutex;
  Phase phase = Phase::kInstalling;
};
NativeCurrentUserProviderBinding::NativeCurrentUserProviderBinding(
    std::shared_ptr<State> state)
    : state_(std::move(state)) {}
uint64_t NativeCurrentUserProviderBinding::root_generation() const {
  return state_->calls->load_generation();
}
bool NativeCurrentUserProviderBinding::bindings_complete() const {
  std::lock_guard lock(state_->mutex);
  return state_->phase == State::Phase::kBound;
}
std::shared_ptr<NativeCurrentUserProviderBinding>
NativeCurrentUserProviderBinding::InstallBeforeHeapAndWorkers(
    std::shared_ptr<GuestRuntimeCallExecutor> calls, std::string* error) {
  auto fail = [&](const char* text) {
    if (error) {
      *error = text;
    }
    return std::shared_ptr<NativeCurrentUserProviderBinding>{};
  };
  if (!calls || !calls->kernel() || !XThread::IsInThread()) {
    return fail("Actual owned bootstrap required");
  }
  auto* kernel = calls->kernel();
  auto* thread = XThread::GetCurrentThread();
  auto module = calls->module();
  if (thread->kernel_state() != kernel ||
      thread->native_frame_thread_identity().process_type !=
          X_PROCTYPE_SYSTEM ||
      !module || !module->xex_module() || !calls->heap_runtime() ||
      !module->is_system_app_module() ||
      module.get() == kernel->GetExecutableModule().get() ||
      module->load_generation() != calls->load_generation() ||
      calls->heap_runtime()->GetSnapshot().state !=
          GuestRuntime::State::kResourcesLoaded) {
    return fail(
        "Current user bridges require quiescent original SYSTEM bootstrap "
        "before heap/workers");
  }
  const Key key{kernel, module->hmodule_ptr(), calls->load_generation()};
  {
    std::lock_guard lock(registry_mutex);
    for (auto it = registry.begin(); it != registry.end();) {
      if (it->second.expired()) {
        it = registry.erase(it);
      } else {
        ++it;
      }
    }
    if (auto previous = registry[key].lock()) {
      return previous->bindings_complete()
                 ? previous
                 : fail("Current user binding busy/failed");
    }
  }
  auto* resolver = kernel->processor()->export_resolver();
  if (!resolver) {
    return fail("Shared current user export resolver unavailable");
  }
  for (const auto& profile : profiles) {
    const auto* target =
        resolver->GetExportByOrdinal("xam.xex", profile.ordinal);
    if (module->GetProcAddressByOrdinal(profile.ordinal) != profile.address ||
        !target || target->get_type() != cpu::Export::Type::kFunction ||
        !target->is_implemented() || !target->function_data.trampoline ||
        !target->name || std::string_view(target->name) != profile.name) {
      return fail("Exact implemented shared current user exports unavailable");
    }
  }
  auto state = std::make_shared<State>();
  state->calls = calls;
  state->presets = CurrentUserPresetsForKernel(kernel);
  if (!state->presets) {
    return fail("Owned current user preset backend unavailable");
  }
  auto binding = std::shared_ptr<NativeCurrentUserProviderBinding>(
      new NativeCurrentUserProviderBinding(state));
  // Keep the root/backend alive on partial installation too. A failed cohort
  // is never retried or presented as provider readiness.
  kernel->RetainNativeResourceUntilTeardown(binding);
  {
    std::lock_guard lock(registry_mutex);
    if (auto previous = registry[key].lock()) {
      return previous->bindings_complete()
                 ? previous
                 : fail("Current user binding raced another installer");
    }
    registry[key] = binding;
  }
  bool success = false;
  try {
    success = true;
    for (const auto& profile : profiles) {
      if (!module->xex_module()->BindGuestExportToHost(
              profile.ordinal, "xam.xex", profile.ordinal, profile.bytes,
              profile.sha)) {
        success = false;
        break;
      }
    }
  } catch (...) {
    std::lock_guard lock(state->mutex);
    state->phase = State::Phase::kFailed;
    throw;
  }
  {
    std::lock_guard lock(state->mutex);
    state->phase = success ? State::Phase::kBound : State::Phase::kFailed;
  }
  if (!success) {
    return fail(
        "Source checked current user bridge failed; stop the partially bound "
        "cohort");
  }
  if (error) {
    error->clear();
  }
  return binding;
}
}  // namespace xe::kernel
