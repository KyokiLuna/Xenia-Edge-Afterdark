/** Xenia : Xbox 360 Emulator Research Project; BSD license, see LICENSE. */
#pragma once

#include <array>
#include <cstdint>
#include <cstddef>
#include <filesystem>
#include <string>
#include <utility>

#include "xenia/base/cvar.h"
#include "xenia/base/platform.h"

DECLARE_string(cpu);
DECLARE_bool(debug);
DECLARE_bool(scribble_heap);
DECLARE_bool(protect_on_release);
DECLARE_bool(protect_zero);
DECLARE_path(host_storage_protected_root);
DECLARE_path(managed_device_state_root);
DECLARE_string(system_readonly_source_root);
DECLARE_string(logged_profile_slot_0_xuid);
DECLARE_string(logged_profile_slot_1_xuid);
DECLARE_string(logged_profile_slot_2_xuid);
DECLARE_string(logged_profile_slot_3_xuid);
DECLARE_bool(mount_memory_unit);
DECLARE_bool(allow_plugins);
DECLARE_bool(kernel_debug_monitor);
DECLARE_bool(kernel_cert_monitor);
DECLARE_bool(trace_function_coverage);
DECLARE_string(cl);
namespace xe::hid {
DECLARE_string(slot_bindings_passthrough);
}  // namespace xe::hid
DECLARE_int32(priority_class);
DECLARE_int32(console_type);
#if XE_PLATFORM_GNU_LINUX
// This selector is consumed by the GNU/Linux memory_posix implementation.
// It has no definition in memory_win and is not read by the Mac mapping path.
DECLARE_bool(use_shm_open);
#endif

namespace xe::kernel::test {

// Process-global test policy for a serial, fixture-owned Kernel/Memory setup.
// Declare before fixture roots and Emulator; destroy after both. Keeping this
// owner alive across Emulator reset/recreation preserves the same policy.
// No persistent ConfigVar layer or public runtime setting API is modified.
class IsolatedKernelRuntimeSettings {
 public:
  IsolatedKernelRuntimeSettings() { Swap(); }
  ~IsolatedKernelRuntimeSettings() noexcept { Swap(); }
  IsolatedKernelRuntimeSettings(const IsolatedKernelRuntimeSettings&) = delete;
  IsolatedKernelRuntimeSettings& operator=(
      const IsolatedKernelRuntimeSettings&) = delete;
  IsolatedKernelRuntimeSettings(IsolatedKernelRuntimeSettings&&) = delete;
  IsolatedKernelRuntimeSettings& operator=(
      IsolatedKernelRuntimeSettings&&) = delete;

 private:
  void Swap() noexcept {
    // Every potentially allocating replacement/copy/current_path lookup is a
    // member initializer, before the first global mutation. All swaps below
    // are nonthrowing, so even construction failure cannot leave half a policy.
    cvars::cpu.swap(cpu_);
    cvars::host_storage_protected_root.swap(protected_root_);
    cvars::managed_device_state_root.swap(managed_state_root_);
    cvars::system_readonly_source_root.swap(system_source_);
    for (std::size_t i = 0; i != profiles_.size(); ++i) {
      profile_fields_[i]->swap(profiles_[i]);
    }
    cvars::cl.swap(command_line_);
    ::xe::hid::cvars::slot_bindings_passthrough.swap(input_bindings_);
    std::swap(cvars::debug, debug_);
    std::swap(cvars::scribble_heap, scribble_);
    std::swap(cvars::protect_on_release, release_protection_);
    std::swap(cvars::protect_zero, zero_protection_);
    std::swap(cvars::mount_memory_unit, memory_unit_);
    std::swap(cvars::allow_plugins, plugins_);
    std::swap(cvars::kernel_debug_monitor, debug_monitor_);
    std::swap(cvars::kernel_cert_monitor, cert_monitor_);
    std::swap(cvars::trace_function_coverage, trace_coverage_);
    std::swap(cvars::priority_class, priority_);
    std::swap(cvars::console_type, console_type_);
#if XE_PLATFORM_GNU_LINUX
    std::swap(cvars::use_shm_open, shared_memory_);
#endif
  }

  // The direct runner uses the actual project filesystem as its CWD. Setup
  // opens this real authority with kHostStorageReserve (20 GiB); never clear
  // the reserve or substitute the fixture's potentially different temp FS.
  std::filesystem::path protected_root_ = std::filesystem::current_path();
  std::filesystem::path managed_state_root_;
  std::string cpu_{"null"};  // Setup(false) now selects the actual NullBackend.
  std::string system_source_;
  std::array<std::string, 4> profiles_{};
  const std::array<std::string*, 4> profile_fields_{
      &cvars::logged_profile_slot_0_xuid, &cvars::logged_profile_slot_1_xuid,
      &cvars::logged_profile_slot_2_xuid, &cvars::logged_profile_slot_3_xuid};
  std::string command_line_;
  std::string input_bindings_;
  bool debug_ = false;
  bool scribble_ = false;
  bool release_protection_ = false;
  bool zero_protection_ = true;
  bool memory_unit_ = false;
  bool plugins_ = false;
  bool debug_monitor_ = false;
  bool cert_monitor_ = false;
  bool trace_coverage_ = false;
  int32_t priority_ = 0;
  int32_t console_type_ = -1;
#if XE_PLATFORM_GNU_LINUX
  bool shared_memory_ = false;
#endif
};

}  // namespace xe::kernel::test
