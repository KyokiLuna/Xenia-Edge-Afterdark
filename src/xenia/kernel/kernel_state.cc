/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <algorithm>
#include <atomic>
#include <chrono>
#include <ranges>

#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/native_system_ui_notifications.h"

#include <cstdlib>

#include "xenia/base/byte_stream.h"
#include "xenia/base/logging.h"
#include "xenia/base/snapshot_publisher.h"
#include "xenia/emulator.h"
#include "xenia/gpu/graphics_system.h"
#include "xenia/hid/input_system.h"
#include "xenia/kernel/guest_scheduler.h"
#include "xenia/kernel/native_import_domain.h"
#include "xenia/kernel/native_notification_preference_coordinator.h"
#include "xenia/kernel/native_system_allocation_authority.h"
#include "xenia/kernel/native_xui_probe.h"
#include "xenia/kernel/smc.h"
#include "xenia/kernel/title_id_utils.h"
#include "xenia/kernel/user_module.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xam/app_manager.h"
#include "xenia/kernel/xam/xam_state.h"
#include "xenia/kernel/xam/xam_system_link.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_memory.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_module.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_ob.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_threading.h"
#include "xenia/kernel/xevent.h"
#include "xenia/kernel/xmodule.h"
#include "xenia/kernel/xnotifylistener.h"
#include "xenia/kernel/xobject.h"
#include "xenia/kernel/xstfs_device.h"
#include "xenia/kernel/xsocket.h"
#include "xenia/kernel/xthread.h"
#include "xenia/kernel/xtimer.h"
#include "xenia/ui/imgui_host_notification.h"

#include "third_party/mbedtls/include/mbedtls/sha1.h"

DEFINE_bool(apply_title_update, true, "Apply title updates.", "Kernel");
DEFINE_bool(allow_incompatible_title_update, false,
            "Allow title updates with mismatched signatures to be applied.",
            "Kernel");

DEFINE_uint32(kernel_build_version, 17559, "Define current kernel version",
              "Kernel");
UPDATE_from_uint32(kernel_build_version, 2026, 10, 15, 4, 1888);

DECLARE_string(cl);
DECLARE_bool(native_guide_bootstrap_probe);
DECLARE_bool(native_xui_probe);
DECLARE_bool(native_xui_render_probe);
DECLARE_bool(native_guide_scene_probe);
DECLARE_bool(native_hud_controller_probe);
DECLARE_bool(native_xui_target_probe);
DECLARE_bool(native_system_device_binding_probe);
DECLARE_bool(native_gpu_context_probe);
DECLARE_bool(native_guide_host_probe);
DECLARE_bool(native_guide_real_time_probe);
DECLARE_bool(native_frame_lease_probe);
DECLARE_bool(native_frame_execute_probe);
DECLARE_bool(native_frame_composite_probe);

namespace xe {
namespace kernel {

constexpr std::chrono::milliseconds kDeferredOverlappedDelayMillis(25);

// This is a global object initialized with the XboxkrnlModule.
// It references the current kernel state object that all kernel methods should
// be using to stash their variables.
KernelState* shared_kernel_state_ = nullptr;

KernelState* kernel_state() { return shared_kernel_state_; }

KernelState::KernelState(
    Emulator* emulator, std::shared_ptr<ConsoleSettingsStore> console_settings,
    std::error_code console_settings_error,
    std::shared_ptr<const filesystem::SnapshotRecoveryOwner> recovery_owner)
    : emulator_(emulator),
      memory_(emulator->memory()),
      dispatch_thread_running_(false),
      dpc_list_(emulator->memory()),
      kernel_trampoline_group_(emulator->processor()->backend()) {
  assert_null(shared_kernel_state_);
  shared_kernel_state_ = this;
  processor_ = emulator->processor();
  file_system_ = emulator->file_system();
  // Explicit authority already protected by Emulator before VFS construction.
  // Keep it independent of XConfig and alive before XamState/profile discovery.
  snapshot_recovery_owner_ = std::move(recovery_owner);
  // Signing in a Live-enabled profile on boot reads the console's country and
  // language.
  notification_preferences_ =
      std::make_shared<NativeNotificationPreferenceCoordinator>(
          console_settings);
  xconfig_ = std::make_unique<XConfig>(std::move(console_settings),
                                       console_settings_error);
  xam_state_ = std::make_unique<xam::XamState>(emulator, this);
  guest_scheduler_ = std::make_unique<GuestScheduler>(this);
  smc_ = std::make_unique<SystemManagementController>();
  nui_ = std::make_unique<NUIController>();

  InitializeKernelGuestGlobals();
  kernel_version_ = KernelVersion(cvars::kernel_build_version);

  auto hc_loc_heap = memory_->LookupHeap(strange_hardcoded_page_);
  bool fixed_alloc_worked = hc_loc_heap->AllocFixed(
      strange_hardcoded_page_, 65536, 0,
      kMemoryAllocationCommit | kMemoryAllocationReserve,
      kMemoryProtectRead | kMemoryProtectWrite);

  xenia_assert(fixed_alloc_worked);
}

KernelState::~KernelState() {
  // Before anything a timer callback touches goes away.
  XTimer::CancelAll();

  ShutdownNativeGuideBootstrapProbe();

  SetExecutableModule(nullptr);

  ShutdownDispatchThread();
  if (xam_state_) {
    app_manager()->ClearGuestApps();
  }
  // Destroy canceled work while module pins, memory and the object table are
  // still valid. No request can be added after the dispatch worker stops.
  decltype(dispatch_queue_) canceled_dispatch;
  {
    std::lock_guard lock(dispatch_mutex_);
    canceled_dispatch.swap(dispatch_queue_);
  }
  canceled_dispatch.clear();

  // Reclaiming leftover fibers releases handles, so run this while the object
  // table is still alive.
  guest_scheduler_->Shutdown();
  // Guest code may have re-armed one since.
  XTimer::CancelAll();
  for (auto& dpcs : processor_dpcs_) {
    dpcs.thread.reset();
    dpcs.event.reset();
  }

  executable_module_.reset();
  native_system_app_leases_.clear();
  // NativeSystemUiService must have closed ingress and supplied actual CPU /
  // graph-specific GPU stop proof before this boundary. No PPC destructor.
  for (const auto& authority : native_system_allocation_authorities_) {
    authority->CloseAdmission();
  }
  // Registered delegations are retained by these resident holders. Keep them
  // discoverable until every Session, including an early Revoke/Freeze failure
  // before production provenance is installed, has actually retired below.
  if (!native_system_allocation_authorities_.empty()) {
    bool cpu_stopped = !XThread::IsInThread();
    for (const auto& thread :
         object_table_.GetObjectsByType<XThread>(XObject::Type::Thread)) {
      cpu_stopped = cpu_stopped && !thread->is_running();
    }
    // Scheduler join above plus the early service / callback host-thread joins
    // must precede dropping cohort lifetime. Graph retirement separately
    // checks the actual same-backend GPU stop token; no destructor-time PPC.
    const bool retired =
        cpu_stopped && RetireNativeSystemAllocationsAfterKernelStop(
                           [&](const memory::PhysicalAllocationOwner& owner) {
                             return cpu_stopped && owner.valid();
                           });
    if (!retired) {
      try {
        XELOGE(
            "Native allocation cohort: stop retirement unproven; retaining "
            "process");
        xe::FlushLog();
      } catch (...) {
      }
      std::quick_exit(EXIT_FAILURE);
    }
  }
  resident_native_resources_.clear();
  native_system_allocation_authorities_.clear();
  for (const auto& [id, domain] : native_import_domains_) {
    domain->Retire();
  }
  native_import_domains_.clear();
  // All runtime consumers and the diagnostic thread have stopped. Release
  // generation pins before the module list and guest memory disappear.
  auto retired_runtimes = std::move(guest_runtimes_);
  for (auto& [hmodule, runtime] : retired_runtimes) {
    runtime->RetireAndDrain();
  }
  retired_runtimes.clear();
  user_modules_.clear();
  kernel_modules_.clear();

  // The table reset would take the pooled I/O events' handles otherwise.
  for (auto& event : idle_io_events_) {
    if (!event->handles().empty()) {
      event->ReleaseHandle();
    }
  }
  idle_io_events_.clear();

  // A reclaimed fiber or a pending wait can keep a socket past the table reset
  // so its host socket closes here.
  for (auto& socket : object_table_.GetObjectsByType<XSocket>()) {
    socket->Abandon();
  }

  // All CPU consumers stopped above. Dismount drops active backing ownership,
  // then the object table drains native member XFiles and their backing aliases.
  for (const auto& device : native_stfs_devices_) { device->Dismount(); }

  // Delete all objects.
  object_table_.Reset();

  xam_state_.reset();
  if (!native_stfs_devices_.empty()) {
    // A normally registered nested container can retain a native parent File.
    // Emulator would clear this VFS immediately after Kernel destruction;
    // drain it here, after modules/XFiles/XamState, while Kernel still exists.
    file_system_->Clear();
  }
  for (const auto& device : native_stfs_devices_) {
    if (!device->backing_drained()) {
      // A host File escaped native rundown. Its alias still retains an XFile
      // requiring this Kernel. Do not free Kernel/Memory underneath that owner.
      XELOGE("Native STFS backing escaped Kernel rundown; retaining process");
      xe::FlushLog();
      std::quick_exit(EXIT_FAILURE);
    }
  }
  native_stfs_devices_.clear();

  assert_true(shared_kernel_state_ == this);
  shared_kernel_state_ = nullptr;
}

namespace {
// What the I/O manager waits with for a title's synchronous request.
constexpr uint32_t kWaitReasonExecutive = 0;
constexpr uint32_t kUserMode = 1;
}  // namespace

void KernelState::RunBlockingIo(const std::function<void()>& fn,
                                GuestScheduler::BlockingCallClass call_class,
                                bool alertable) {
  if (!GuestScheduler::CurrentThreadOffloadsBlockingCalls()) {
    fn();
    return;
  }
  auto event = AcquireIoEvent();
  // The worker writes into this frame so only |done| ends the wait, even on a
  // terminate.
  std::atomic<bool> done{false};
  guest_scheduler_->PostHostCall(
      [&fn, &done, signal = retain_object(event.get())]() {
        fn();
        done.store(true, std::memory_order_release);
        signal->Set(kIoDiskIncrement, false);
      },
      call_class);
  uint32_t wait_alertable = alertable ? 1 : 0;
  while (!done.load(std::memory_order_acquire)) {
    X_STATUS status = event->Wait(kWaitReasonExecutive, kUserMode,
                                  wait_alertable, nullptr, false);
    if (status == X_STATUS_USER_APC) {
      // An alert cannot cancel the host request. The APCs run at the next
      // alertable wait, after the caller writes its status block.
      wait_alertable = 0;
    } else if (status != X_STATUS_SUCCESS) {
      // A failed poll does not wait so park until the next re-poll instead of
      // spinning.
      XELOGW("KernelState: blocking I/O wait returned {:08X}", status);
      guest_scheduler_->BlockCurrentThread(0, 0, false, false);
    }
  }
  ReleaseIoEvent(std::move(event));
}

object_ref<XEvent> KernelState::AcquireIoEvent() {
  {
    std::lock_guard<std::mutex> lock(io_event_lock_);
    if (!idle_io_events_.empty()) {
      auto event = std::move(idle_io_events_.back());
      idle_io_events_.pop_back();
      return event;
    }
  }
  auto event = object_ref<XEvent>(new XEvent(this, true));
  event->Initialize(false, false);
  // One signal per request would crowd the guest signals out of the ring.
  event->set_signal_ring_quiet(true);
  return event;
}

void KernelState::ReleaseIoEvent(object_ref<XEvent> event) {
  // The completion can land after |done|, leaving it armed for the next user.
  event->Reset();
  std::lock_guard<std::mutex> lock(io_event_lock_);
  idle_io_events_.push_back(std::move(event));
}

void KernelState::ShutdownDispatchThread() {
  if (dispatch_thread_running_) {
    {
      std::lock_guard lock(dispatch_mutex_);
      dispatch_thread_running_ = false;
    }
    dispatch_cond_.notify_all();
    dispatch_thread_->Wait(0, 0, 0, nullptr);
  }
}

KernelState* KernelState::shared() { return shared_kernel_state_; }

xam::AchievementManager* KernelState::achievement_manager() const {
  return xam_state()->achievement_manager();
}

xam::AppManager* KernelState::app_manager() const {
  return xam_state()->app_manager();
}

xam::ContentManager* KernelState::content_manager() const {
  return xam_state()->content_manager();
}

uint32_t KernelState::title_id() const {
  if (!executable_module_) {
    return 0;
  }

  assert_not_null(executable_module_);

  xex2_opt_execution_info* exec_info = 0;
  executable_module_->GetOptHeader(XEX_HEADER_EXECUTION_INFO, &exec_info);

  if (exec_info) {
    return exec_info->title_id;
  }

  return 0;
}

bool KernelState::is_title_open() const { return emulator_->is_title_open(); }

const std::unique_ptr<xam::SpaInfo> KernelState::title_xdbf() const {
  return module_xdbf(executable_module_);
}

const std::unique_ptr<xam::SpaInfo> KernelState::module_xdbf(
    object_ref<UserModule> exec_module) const {
  assert_not_null(exec_module);

  uint32_t resource_data = 0;
  uint32_t resource_size = 0;
  if (XSUCCEEDED(exec_module->GetSection(
          fmt::format("{:08X}", exec_module->title_id()).c_str(),
          &resource_data, &resource_size))) {
    return std::make_unique<xam::SpaInfo>(std::span<uint8_t>(
        memory()->TranslateVirtual(resource_data), resource_size));
  }

  return nullptr;
}

uint32_t KernelState::AllocateTLS(cpu::ppc::PPCContext* context) {
  auto globals =
      memory()->TranslateVirtual<KernelGuestGlobals*>(GetKernelGuestGlobals());
  auto tls_lock = &globals->tls_lock;
  auto old_irql = xboxkrnl::xeKeKfAcquireSpinLock(context, tls_lock);

  int result = -1;

  auto current_thread = XThread::GetCurrentThread();
  if (!current_thread) {
    XELOGE("AllocateTLS: No current thread");
    xboxkrnl::xeKeKfReleaseSpinLock(context, tls_lock, old_irql);
    return X_TLS_OUT_OF_INDEXES;
  }

  auto process_ptr = memory()->TranslateVirtual(
      current_thread->guest_object<X_KTHREAD>()->process);
  if (!process_ptr) {
    XELOGE("AllocateTLS: Failed to translate process pointer");
    xboxkrnl::xeKeKfReleaseSpinLock(context, tls_lock, old_irql);
    return X_TLS_OUT_OF_INDEXES;
  }

  // Search for a free TLS slot in the process bitmap
  // Bitmap format: 1 = free, 0 = allocated
  // 8 x 32-bit words = 256 total TLS slots
  for (xe::be<uint32_t>* i = &process_ptr->tls_slot_bitmap[0];
       i < &process_ptr->tls_slot_bitmap[8]; ++i) {
    // Read bitmap value (handles big-endian conversion)
    uint32_t bitmap_value = static_cast<uint32_t>(*i);

    // Find highest free slot using lzcnt (leading zero count)
    // Returns 0-31 if a bit is set, 32 if no bits are set
    uint32_t leading_zeros = xe::lzcnt(bitmap_value);

    if (leading_zeros != 32) {
      // Calculate absolute slot index from bitmap position and bit offset
      // Each bitmap word represents 32 slots
      size_t bitmap_index = i - &process_ptr->tls_slot_bitmap[0];
      uint32_t base_slot = static_cast<uint32_t>(bitmap_index) * 32;
      int calculated_slot = base_slot + leading_zeros;

      // Validate slot is within Xbox 360 TLS range
      if (calculated_slot >= 0 && calculated_slot < 256) {
        result = calculated_slot;

        // Clear the bit to mark as allocated
        // lzcnt returns 0 for bit 31, 31 for bit 0
        uint32_t bit_index = 31 - leading_zeros;
        *i = bitmap_value & ~(1U << bit_index);
        break;
      } else {
        XELOGE("AllocateTLS: Invalid slot calculation: {}", calculated_slot);
      }
    }
  }

  if (result == -1) {
    XELOGW("AllocateTLS: All TLS slots exhausted for current process");
  }

  xboxkrnl::xeKeKfReleaseSpinLock(context, tls_lock, old_irql);
  return static_cast<uint32_t>(result);
}

void KernelState::FreeTLS(cpu::ppc::PPCContext* context, uint32_t slot) {
  if (slot >= 256) {
    XELOGE("FreeTLS: Invalid slot index {}", slot);
    return;
  }

  auto current_thread = XThread::GetCurrentThread();
  if (!current_thread) {
    XELOGE("FreeTLS: No current thread");
    return;
  }

  auto current_kthread = current_thread->guest_object<X_KTHREAD>();
  if (!current_kthread) {
    XELOGE("FreeTLS: Failed to get guest thread object");
    return;
  }

  auto process_ptr = memory()->TranslateVirtual(current_kthread->process);
  if (!process_ptr) {
    XELOGE("FreeTLS: Failed to translate process pointer");
    return;
  }

  auto globals =
      memory()->TranslateVirtual<KernelGuestGlobals*>(GetKernelGuestGlobals());
  auto tls_lock = &globals->tls_lock;
  auto old_irql = xboxkrnl::xeKeKfAcquireSpinLock(context, tls_lock);

  uint32_t bitmap_index = slot / 32;
  uint32_t bit_mask = 1U << (31 - (slot % 32));
  uint32_t bitmap_value =
      static_cast<uint32_t>(process_ptr->tls_slot_bitmap[bitmap_index]);

  if (bitmap_value & bit_mask) {
    XELOGW("FreeTLS: Slot {} is already free", slot);
    xboxkrnl::xeKeKfReleaseSpinLock(context, tls_lock, old_irql);
    return;
  }

  // Clear TLS values in all threads of this process
  const std::vector<object_ref<XThread>> threads =
      object_table()->GetObjectsByType<XThread>();

  uint32_t current_process_ptr = current_kthread->process.m_ptr;
  for (const object_ref<XThread>& thread : threads) {
    if (!thread || !thread->is_guest_thread()) {
      continue;
    }

    auto thread_kthread = thread->guest_object<X_KTHREAD>();
    if (thread_kthread &&
        thread_kthread->process.m_ptr == current_process_ptr) {
      thread->SetTLSValue(slot, 0);
    }
  }

  // Mark slot as free in bitmap
  process_ptr->tls_slot_bitmap[bitmap_index] = bitmap_value | bit_mask;

  xboxkrnl::xeKeKfReleaseSpinLock(context, tls_lock, old_irql);
}

void KernelState::RegisterTitleTerminateNotification(uint32_t routine,
                                                     uint32_t priority) {
  TerminateNotification notify;
  notify.guest_routine = routine;
  notify.priority = priority;

  terminate_notifications_.push_back(notify);
}

void KernelState::RemoveTitleTerminateNotification(uint32_t routine) {
  for (auto it = terminate_notifications_.begin();
       it != terminate_notifications_.end(); it++) {
    if (it->guest_routine == routine) {
      terminate_notifications_.erase(it);
      break;
    }
  }
}

void KernelState::RegisterModule(XModule* module) {}

void KernelState::UnregisterModule(XModule* module) {}

bool KernelState::RegisterUserModule(object_ref<UserModule> module) {
  auto lock = global_critical_region_.Acquire();

  for (auto user_module : user_modules_) {
    if (user_module->path() == module->path()) {
      // Already loaded.
      return false;
    }
  }

  user_modules_.push_back(module);
  return true;
}

void KernelState::UnregisterUserModule(UserModule* module) {
  auto lock = global_critical_region_.Acquire();

  for (auto it = user_modules_.begin(); it != user_modules_.end(); it++) {
    if ((*it)->path() == module->path()) {
      user_modules_.erase(it);
      return;
    }
  }
}

bool KernelState::IsKernelModule(const std::string_view name) {
  if (name.empty()) {
    // Executing module isn't a kernel module.
    return false;
  }
  // NOTE: no global lock required as the kernel module list is static.
  for (auto kernel_module : kernel_modules_) {
    if (kernel_module->Matches(name)) {
      return true;
    }
  }
  return false;
}

bool KernelState::IsModuleLoaded(const std::string_view name) {
  if (name.empty()) {
    return true;
  }

  for (auto kernel_module : kernel_modules_) {
    if (kernel_module->Matches(name)) {
      return true;
    }
  }

  auto global_lock = global_critical_region_.Acquire();

  for (auto user_module : user_modules_) {
    if (user_module->Matches(name)) {
      return true;
    }
  }

  return false;
}

object_ref<KernelModule> KernelState::GetKernelModule(
    const std::string_view name) {
  assert_true(IsKernelModule(name));

  for (auto kernel_module : kernel_modules_) {
    if (kernel_module->Matches(name)) {
      return retain_object(kernel_module.get());
    }
  }

  return nullptr;
}

object_ref<XModule> KernelState::GetModule(const std::string_view name,
                                           bool user_only) {
  if (name.empty()) {
    // NULL name = self.
    // TODO(benvanik): lookup module from caller address.
    return GetExecutableModule();
  } else if (xe::utf8::equal_case(name, "kernel32.dll")) {
    // Some games request this, for some reason. wtf.
    return nullptr;
  }

  auto global_lock = global_critical_region_.Acquire();

  if (!user_only) {
    for (auto kernel_module : kernel_modules_) {
      if (kernel_module->Matches(name)) {
        return retain_object(kernel_module.get());
      }
    }
  }

  auto path(name);

  // Resolve the path to an absolute path.
  auto entry = file_system_->ResolvePath(name);
  if (entry) {
    path = entry->absolute_path();
  }

  for (auto user_module : user_modules_) {
    if (user_module->Matches(path)) {
      return retain_object(user_module.get());
    }
  }
  return nullptr;
}

object_ref<UserModule> KernelState::GetUserModuleForAddress(uint32_t address) {
  auto global_lock = global_critical_region_.Acquire();
  for (auto& user_module : user_modules_) {
    if (user_module->module_format() != UserModule::kModuleFormatXex) {
      continue;
    }
    auto xex_module = user_module->xex_module();
    uint32_t base_address = xex_module->base_address();
    if (address >= base_address &&
        address - base_address < xex_module->xex_security_info()->image_size) {
      return retain_object(user_module.get());
    }
  }
  return nullptr;
}

bool KernelState::RegisterGraphicsInterruptCallback(uint32_t callback,
                                                    uint32_t user_data,
                                                    uint32_t process_type) {
  auto global_lock = global_critical_region_.Acquire();
  auto* graphics = emulator_->graphics_system();
  if (!graphics) {
    return false;
  }
  auto module = callback ? GetUserModuleForAddress(callback) : nullptr;
  if (module && module->unloading_) {
    return false;
  }
  graphics->SetInterruptCallbackForProcess(
      callback, user_data, process_type, module ? module->hmodule_ptr() : 0,
      module ? std::make_shared<object_ref<UserModule>>(module) : nullptr);
  return true;
}

std::shared_ptr<GuestRuntime> KernelState::GetGuestRuntime(uint32_t hmodule) {
  auto lock = global_critical_region_.Acquire();
  const auto found = guest_runtimes_.find(hmodule);
  return found == guest_runtimes_.end() ? nullptr : found->second;
}

std::shared_ptr<GuestRuntime> KernelState::CreateGuestRuntime(
    const object_ref<UserModule>& module) {
  auto lock = global_critical_region_.Acquire();
  if (!module || module->unloading_ || !module->is_system_app_module() ||
      std::ranges::find_if(user_modules_, [&module](const auto& loaded) {
        return loaded.get() == module.get();
      }) == user_modules_.end()) {
    return {};
  }
  auto& runtime = guest_runtimes_[module->hmodule_ptr()];
  if (!runtime) {
    runtime = std::make_shared<GuestRuntime>(
        GuestRuntime::Identity{module->hmodule_ptr(),
                               next_guest_runtime_generation_++},
        std::make_shared<object_ref<UserModule>>(module));
  }
  return runtime;
}

std::shared_ptr<KernelState::UserModuleExecutionLease>
KernelState::AcquireUserModuleExecutionLease(
    const object_ref<UserModule>& module) {
  auto lock = global_critical_region_.Acquire();
  if (!module || module->unloading_ || module->native_teardown_claimed_ ||
      module->execution_leases_.load(std::memory_order_relaxed) == UINT32_MAX ||
      std::ranges::find_if(user_modules_, [&module](const auto& loaded) {
        return loaded.get() == module.get();
      }) == user_modules_.end()) {
    return {};
  }
  auto* lease =
      new UserModuleExecutionLease(this, module.get(), module->hmodule_ptr());
  module->execution_leases_.fetch_add(1, std::memory_order_relaxed);
  return std::shared_ptr<UserModuleExecutionLease>(
      lease, [module](auto* value) {
        module->execution_leases_.fetch_sub(1, std::memory_order_release);
        delete value;
      });
}

void KernelState::RetainNativeResourceUntilTeardown(
    std::shared_ptr<void> resource) {
  auto lock = global_critical_region_.Acquire();
  resident_native_resources_.push_back(std::move(resource));
}

object_ref<XThread> KernelState::LaunchModule(object_ref<UserModule> module) {
  if (!module->is_executable()) {
    return nullptr;
  }

  SetExecutableModule(module);
  XELOGI("KernelState: Launching module...");

  // The boot animation is a DLL with no entry point.
  if (module->is_dll_module() && !module->entry_point()) {
    auto thread = GetKernelModule<xboxkrnl::XboxkrnlModule>("xboxkrnl.exe")
                      ->StartBootAnimation(module, /*main_thread=*/true);
    if (thread) {
      emulator()->processor()->PreLaunch();
    }
    return thread;
  }

  // Create a thread to run in.
  // We start suspended so we can run the debugger prep.
  auto thread = object_ref<XThread>(
      new XThread(kernel_state(), module->stack_size(), 0,
                  module->entry_point(), 0, X_CREATE_SUSPENDED, true, true));

  // We know this is the 'main thread'.
  thread->set_name("Main XThread");

  X_STATUS result = thread->Create();
  if (XFAILED(result)) {
    XELOGE("Could not create launch thread: {:08X}", result);
    return nullptr;
  }

  // Waits for a debugger client, if desired.
  emulator()->processor()->PreLaunch();

  return thread;
}

object_ref<UserModule> KernelState::GetExecutableModule() {
  if (!executable_module_) {
    return nullptr;
  }
  return executable_module_;
}

void KernelState::SetExecutableModule(object_ref<UserModule> module) {
  if (module.get() == executable_module_.get()) {
    return;
  }
  executable_module_ = std::move(module);
  if (!executable_module_) {
    return;
  }

  auto title_process =
      memory_->TranslateVirtual<X_KPROCESS*>(GetTitleProcess());

  InitializeProcess(title_process, X_PROCTYPE_TITLE, 10, 13, 17);

  xex2_opt_tls_info* tls_header = nullptr;
  executable_module_->GetOptHeader(XEX_HEADER_TLS_INFO, &tls_header);
  if (tls_header) {
    title_process->tls_static_data_address = tls_header->raw_data_address;
    title_process->tls_data_size = tls_header->data_size;
    title_process->tls_raw_data_size = tls_header->raw_data_size;
    title_process->tls_slot_size = tls_header->slot_count * 4;
    SetProcessTLSVars(title_process, tls_header->slot_count,
                      tls_header->data_size, tls_header->raw_data_address);
  }

  uint32_t kernel_stacksize = 0;

  executable_module_->GetOptHeader(XEX_HEADER_DEFAULT_STACK_SIZE,
                                   &kernel_stacksize);
  if (kernel_stacksize) {
    kernel_stacksize = (kernel_stacksize + 4095) & 0xFFFFF000;
    if (kernel_stacksize < 0x4000) {
      kernel_stacksize = 0x4000;
    }
    title_process->kernel_stack_size = kernel_stacksize;
  }

  // Setup the kernel's XexExecutableModuleHandle field.
  auto export_entry = processor()->export_resolver()->GetExportByOrdinal(
      "xboxkrnl.exe", ordinals::XexExecutableModuleHandle);
  if (export_entry) {
    assert_not_zero(export_entry->variable_ptr);
    auto variable_ptr = memory()->TranslateVirtual<xe::be<uint32_t>*>(
        export_entry->variable_ptr);
    *variable_ptr = executable_module_->hmodule_ptr();
  }

  // Setup the kernel's ExLoadedImageName field
  export_entry = processor()->export_resolver()->GetExportByOrdinal(
      "xboxkrnl.exe", ordinals::ExLoadedImageName);

  if (export_entry) {
    char* variable_ptr =
        memory()->TranslateVirtual<char*>(export_entry->variable_ptr);
    xe::string_util::copy_truncating(
        variable_ptr, executable_module_->path(),
        xboxkrnl::XboxkrnlModule::kExLoadedImageNameSize);
  }

  // Setup the kernel's ExLoadedCommandLine field
  export_entry = processor()->export_resolver()->GetExportByOrdinal(
      "xboxkrnl.exe", ordinals::ExLoadedCommandLine);
  if (export_entry) {
    char* variable_ptr =
        memory()->TranslateVirtual<char*>(export_entry->variable_ptr);

    std::string module_name =
        fmt::format("\"{}.xex\"", executable_module_->name());
    if (!cvars::cl.empty()) {
      module_name += " " + cvars::cl;
    }

    xe::string_util::copy_truncating(
        variable_ptr, module_name,
        xboxkrnl::XboxkrnlModule::kExLoadedCommandLineSize);
  }

  // Initialize file I/O hooks for XMP volume title-specific patches.
  InitXmpVolumePatch();

  // Spin up deferred dispatch worker.
  // TODO(benvanik): move someplace more appropriate (out of ctor, but around
  // here).
  if (!dispatch_thread_running_) {
    dispatch_thread_running_ = true;
    dispatch_thread_ = object_ref<XHostThread>(new XHostThread(
        this, 128 * 1024, 0,
        [this]() {
          // As we run guest callbacks the debugger must be able to suspend us.
          dispatch_thread_->set_can_debugger_suspend(true);

          while (true) {
            std::function<void()> fn;
            {
              std::unique_lock lock(dispatch_mutex_);
              dispatch_cond_.wait(lock, [this]() {
                return !dispatch_queue_.empty() || !dispatch_thread_running_;
              });
              if (!dispatch_thread_running_) {
                break;
              }
              fn = std::move(dispatch_queue_.front());
              dispatch_queue_.pop_front();
            }
            fn();
          }
          return 0;
        },
        GetSystemProcess()));  // don't think an equivalent exists on real hw
    dispatch_thread_->set_name("Kernel Dispatch");
    dispatch_thread_->Create();
  }
}

void KernelState::LoadKernelModule(object_ref<KernelModule> kernel_module) {
  auto global_lock = global_critical_region_.Acquire();
  kernel_modules_.push_back(std::move(kernel_module));
}

object_ref<UserModule> KernelState::LoadUserModule(
    const std::string_view raw_name, bool call_entry, bool system_app_module,
    std::shared_ptr<const cpu::GuestImportProvider> import_provider,
    std::shared_ptr<NativeImportDomain> import_domain) {
  auto domain_operation = import_domain ? import_domain->Acquire()
                                        : NativeImportDomain::Operation{};
  if (import_domain && !domain_operation) {
    return nullptr;
  }
  if (import_domain) {
    auto lock = global_critical_region_.Acquire();
    const auto found = native_import_domains_.find(import_domain->id());
    if (found == native_import_domains_.end() ||
        found->second != import_domain ||
        (import_provider && import_provider != import_domain->profile())) {
      return nullptr;
    }
    import_provider = import_domain->profile();
    system_app_module = true;
  }
  if (import_provider &&
      (!system_app_module || !XThread::IsInThread() ||
       (!import_domain &&
        XThread::GetCurrentThread()->guest_object<X_KTHREAD>()->process_type !=
            X_PROCTYPE_SYSTEM))) {
    XELOGE("Native guest import provider requires an explicit system module");
    return nullptr;
  }
  // Some games try to load relative to launch module, others specify full path.
  auto name = xe::utf8::find_name_from_guest_path(raw_name);
  std::string path(raw_name);
  if (name == raw_name) {
    if (import_domain) {
      // A native system child belongs to its root's loader namespace, even
      // when the request arrived on a TITLE-process caller thread.
      auto lock = global_critical_region_.Acquire();
      const auto root =
          std::ranges::find_if(user_modules_, [&](const auto& value) {
            return NativeImportDomain::Member{value->hmodule_ptr(),
                                              value->load_generation()} ==
                   import_domain->root();
          });
      if (root == user_modules_.end() || (*root)->path().empty()) {
        return nullptr;
      }
      path = xe::utf8::join_guest_paths(
          xe::utf8::find_base_guest_path((*root)->path()), name);
    } else if (!executable_module_) {
      path = xe::utf8::join_guest_paths(
          xe::utf8::find_base_guest_path((*user_modules_.cbegin())->path()),
          name);
    } else {
      path = xe::utf8::join_guest_paths(
          xe::utf8::find_base_guest_path(executable_module_->path()), name);
    }
  }

  object_ref<UserModule> module;
  {
    auto global_lock = global_critical_region_.Acquire();

    // See if we've already loaded it
    for (auto& existing_module : user_modules_) {
      if (existing_module->Matches(path) ||
          (import_domain && existing_module->Matches(name))) {
        if (existing_module->unloading_ ||
            (import_domain &&
             (existing_module->import_domain_id_ != import_domain->id() ||
              existing_module->import_domain_.lock() != import_domain ||
              !import_domain->Contains(
                  {existing_module->hmodule_ptr(),
                   existing_module->load_generation()}))) ||
            (!import_domain && existing_module->import_domain_id_)) {
          return nullptr;
        }
        if (import_provider &&
            existing_module->import_provider_ != import_provider) {
          XELOGE("Refusing to replace imports of already loaded module {}",
                 path);
          return nullptr;
        }
        return existing_module;
      }
    }

    global_lock.unlock();

    // Module wasn't loaded, so load it.
    // TODO: this read, decrypt and decompress stalls the calling fiber's
    // dispatch thread. Offloading it needs care, it touches kernel state and
    // guest-thread identity.
    module = object_ref<UserModule>(
        new UserModule(this, system_app_module, std::move(import_provider)));
    if (import_domain) {
      if (!import_domain->Register(
              {module->hmodule_ptr(), module->load_generation()})) {
        object_table()->ReleaseHandle(module->handle());
        return nullptr;
      }
      module->import_domain_ = import_domain;
      module->import_domain_id_ = import_domain->id();
    }
    X_STATUS status = module->LoadFromFile(path);
    if (XFAILED(status) ||
        (import_domain &&
         module->module_format() != UserModule::kModuleFormatXex)) {
      if (import_domain) {
        import_domain->Unregister(
            {module->hmodule_ptr(), module->load_generation()});
      }
      object_table()->ReleaseHandle(module->handle());
      return nullptr;
    }

    global_lock.lock();

    if (import_domain && !import_domain->active()) {
      import_domain->Unregister(
          {module->hmodule_ptr(), module->load_generation()});
      object_table()->ReleaseHandle(module->handle());
      return nullptr;
    }

    // Putting into the listing automatically retains.
    user_modules_.push_back(module);
  }
  return module;
}

object_ref<UserModule> KernelState::LoadUserModuleFromMemory(
    const std::string_view raw_name, const void* addr, const size_t length,
    std::shared_ptr<NativeImportDomain> import_domain) {
  auto domain_operation = import_domain ? import_domain->Acquire()
                                        : NativeImportDomain::Operation{};
  if (import_domain && !domain_operation) {
    return nullptr;
  }
  if (import_domain) {
    auto lock = global_critical_region_.Acquire();
    const auto found = native_import_domains_.find(import_domain->id());
    if (found == native_import_domains_.end() ||
        found->second != import_domain) {
      return nullptr;
    }
  }
  auto name = xe::utf8::find_base_name_from_guest_path(raw_name);

  object_ref<UserModule> module;
  {
    auto global_lock = global_critical_region_.Acquire();

    // See if we've already loaded it
    for (auto& existing_module : user_modules_) {
      if (existing_module->Matches(name)) {
        if (import_domain) {
          // A memory-image request is a fresh identity, not permission to
          // silently substitute an already loaded image with the same name.
          return nullptr;
        }
        if (existing_module->unloading_ ||
            (import_domain &&
             (existing_module->import_domain_id_ != import_domain->id() ||
              existing_module->import_domain_.lock() != import_domain ||
              !import_domain->Contains(
                  {existing_module->hmodule_ptr(),
                   existing_module->load_generation()}))) ||
            (!import_domain && existing_module->import_domain_id_)) {
          return nullptr;
        }
        return existing_module;
      }
    }

    global_lock.unlock();

    // Module wasn't loaded, so load it.
    module = object_ref<UserModule>(
        new UserModule(this, bool(import_domain),
                       import_domain ? import_domain->profile() : nullptr));
    if (import_domain) {
      if (!import_domain->Register(
              {module->hmodule_ptr(), module->load_generation()})) {
        object_table()->ReleaseHandle(module->handle());
        return nullptr;
      }
      module->import_domain_ = import_domain;
      module->import_domain_id_ = import_domain->id();
    }
    X_STATUS status = module->LoadFromMemoryNamed(name, addr, length);
    if (XFAILED(status) ||
        (import_domain &&
         module->module_format() != UserModule::kModuleFormatXex)) {
      if (import_domain) {
        import_domain->Unregister(
            {module->hmodule_ptr(), module->load_generation()});
      }
      object_table()->ReleaseHandle(module->handle());
      return nullptr;
    }

    global_lock.lock();
    if (import_domain && !import_domain->active()) {
      import_domain->Unregister(
          {module->hmodule_ptr(), module->load_generation()});
      object_table()->ReleaseHandle(module->handle());
      return nullptr;
    }

    // Putting into the listing automatically retains.
    user_modules_.push_back(module);
  }
  return module;
}

X_RESULT
KernelState::FinishLoadingUserModule(const object_ref<UserModule> module,
                                     bool call_entry, bool compile_discovered) {
  auto domain = module->import_domain_.lock();
  auto domain_operation =
      domain ? domain->Acquire() : NativeImportDomain::Operation{};
  if (module->import_domain_id_ &&
      (!domain_operation ||
       !domain->Contains({module->hmodule_ptr(), module->load_generation()}))) {
    return X_STATUS_ACCESS_DENIED;
  }
  if (domain && module->native_import_ready()) {
    return X_STATUS_SUCCESS;
  }
  if (domain && module->native_import_finishing_.exchange(true)) {
    return X_STATUS_UNSUCCESSFUL;
  }
  struct FinishImportClaim {
    std::atomic<bool>* claim;
    std::shared_ptr<NativeImportDomain> domain;
    bool completed = false;
    ~FinishImportClaim() {
      if (domain && !completed) {
        domain->Retire();
      }
      if (claim) {
        claim->store(false, std::memory_order_release);
      }
    }
  } finish_claim{domain ? &module->native_import_finishing_ : nullptr, domain};
  // TODO(Gliniak): Apply custom patches here
  X_RESULT result = module->LoadContinue();
  if (XFAILED(result)) {
    return result;
  }
  module->Dump();
  emulator_->patcher()->ApplyPatchesForTitle(memory_, module->title_id(),
                                             module->hash());
  emulator_->on_patch_apply();
  if (module->xex_module()) {
    module->xex_module()->Precompile(compile_discovered);
  }

  if (module->is_dll_module() && module->entry_point() && call_entry) {
    if (domain && !domain->active()) {
      return X_STATUS_ACCESS_DENIED;
    }
    // Call DllMain(DLL_PROCESS_ATTACH):
    // https://msdn.microsoft.com/en-us/library/windows/desktop/ms682583%28v=vs.85%29.aspx
    uint64_t args[] = {
        module->hmodule_ptr(),
        1,  // DLL_PROCESS_ATTACH
        0,  // 0 because always dynamic
    };

    module->is_attached_ = true;

    auto thread_state = XThread::GetCurrentThread()->thread_state();
    const bool executed = processor()->Execute(
        thread_state, module->entry_point(), args, xe::countof(args));
    if (domain && (!executed || !uint32_t(thread_state->context()->r[3]))) {
      // An interrupted/failed native attach may already have published state.
      // Keep its module loaded and close future domain admission, never retry
      // attach or invent a safe detach boundary.
      domain->Retire();
      return X_STATUS_UNSUCCESSFUL;
    }
  }
  if (domain && !domain->active()) {
    return X_STATUS_ACCESS_DENIED;
  }
  if (domain) {
    module->native_import_ready_.store(true, std::memory_order_release);
  }
  finish_claim.completed = true;
  return result;
}

X_RESULT
KernelState::ApplyTitleUpdate(const object_ref<UserModule> title_module) {
  const auto title_updates = FindTitleUpdate(title_module->title_id());
  if (title_updates.empty()) {
    return X_STATUS_SUCCESS;
  }

  auto patch_module = LoadTitleUpdate(&title_updates.front(), title_module);
  if (!patch_module) {
    return X_STATUS_SUCCESS;
  }

  if (!patch_module->xex_module()->is_patch()) {
    return X_STATUS_UNSUCCESSFUL;
  }

  if (!IsPatchSignatureProper(title_module, patch_module)) {
    if (!cvars::allow_incompatible_title_update) {
      XELOGW(
          "Skipping incompatible title update for {} due to signature mismatch",
          title_module->name());
      if (!GetExecutableModule()) {
        emulator_->display_window()->app_context().CallInUIThread([&]() {
          new xe::ui::HostNotificationWindow(
              emulator_->imgui_drawer(), "Warning!",
              "Title Update signature doesn't match. Skipping its application!",
              0);
        });
      }
      return X_STATUS_SUCCESS;
    }

    XELOGW(
        "Applying incompatible title update for {} due to enabled "
        "allow_incompatible_title_update config option!",
        title_module->name());

    // First module that is loaded is always main executable. That way we can
    // prevent random message spam in case of loading/unloading.
    if (!GetExecutableModule()) {
      emulator_->display_window()->app_context().CallInUIThread([&]() {
        new xe::ui::HostNotificationWindow(
            emulator_->imgui_drawer(), "Warning!",
            "Title Update signature doesn't match. This can cause unexpected "
            "issues or crashes!",
            0);
      });
    }
  }

  return ApplyTitleUpdate(title_module, patch_module);
}

std::vector<xam::XCONTENT_AGGREGATE_DATA> KernelState::FindTitleUpdate(
    const uint32_t title_id) const {
  if (!cvars::apply_title_update) {
    return {};
  }

  return xam_state_->content_manager()->ListContent(
      1, 0, title_id, xe::XContentType::kInstaller);
}

const object_ref<UserModule> KernelState::LoadTitleUpdate(
    const xam::XCONTENT_AGGREGATE_DATA* title_update,
    const object_ref<UserModule> module) {
  uint32_t disc_number = -1;
  if (module->is_multi_disc_title()) {
    disc_number = module->disc_number();
  }

  uint32_t content_license = 0;
  X_RESULT open_status = content_manager()->OpenContent(
      "UPDATE", 0, *title_update, content_license, disc_number);

  const std::string& mount_path = title_mount_path_;
  if (mount_path.empty()) {
    return nullptr;
  }

  if (!module->path().starts_with(mount_path)) {
    return nullptr;
  }

  std::string resolved_path = "";
  if (!file_system()->FindSymbolicLink(kDefaultUpdateSymbolicLink,
                                       resolved_path)) {
    return nullptr;
  }

  const std::string relative_path =
      module->path().substr(mount_path.size() + 1) + 'p';

  // A multi-disc update mounts at discNNN inside the package, so the target
  // no longer always ends in a separator.
  const std::string patch_guest_path =
      xe::utf8::join_guest_paths(resolved_path, relative_path);

  xe::vfs::Entry* patch_entry =
      kernel_state()->file_system()->ResolvePath(patch_guest_path);

  if (!patch_entry) {
    XELOGI("Loading XEX patch failed. Path doesn't exist {}", patch_guest_path);
    return nullptr;
  }

  const std::string patch_path = patch_entry->absolute_path();
  XELOGI("Loading XEX patch from {}", patch_path);
  auto patch_module = object_ref<UserModule>(new UserModule(this));

  X_RESULT result = patch_module->LoadFromFile(patch_path);
  // Nothing looks the patch up by handle so the last reference frees it.
  patch_module->ReleaseHandle();
  if (result != X_STATUS_SUCCESS) {
    XELOGE("Failed to load XEX patch, code: {}", result);
    return nullptr;
  }

  return patch_module;
}

bool KernelState::IsPatchSignatureProper(
    const object_ref<UserModule> title_module,
    const object_ref<UserModule> patch_module) const {
  xex2_opt_delta_patch_descriptor* patch_header = nullptr;
  patch_module->GetOptHeader(XEX_HEADER_DELTA_PATCH_DESCRIPTOR,
                             reinterpret_cast<void**>(&patch_header));

  if (!patch_header) {
    XELOGE("XEX patch has no delta patch descriptor");
    return false;
  }

  // Compare hash inside delta descriptor to base XEX signature
  uint8_t digest[0x14];
  const auto* signature = reinterpret_cast<const uint8_t*>(
      title_module->xex_module()->xex_security_info()->rsa_signature);
  mbedtls_sha1(signature, 0x100, digest);

  if (memcmp(digest, patch_header->digest_source, 0x14) != 0) {
    XELOGW(
        "XEX patch signature hash doesn't match base XEX signature hash, patch "
        "will likely fail!");

    return false;
  }
  return true;
}

X_RESULT
KernelState::ApplyTitleUpdate(const object_ref<UserModule> title_module,
                              const object_ref<UserModule> patch_module) {
  if (!title_module) {
    XELOGE("{}: No title_module provided!", __FUNCTION__);
    return X_STATUS_UNSUCCESSFUL;
  }

  if (!patch_module) {
    XELOGE("{}: No patch_module provided!", __FUNCTION__);
    return X_STATUS_UNSUCCESSFUL;
  }

  X_STATUS result =
      patch_module->xex_module()->ApplyPatch(title_module->xex_module());
  if (result != X_STATUS_SUCCESS) {
    XELOGE("Failed to apply XEX patch, code: {}", result);
  }
  return result;
}

X_STATUS KernelState::UnloadUserModule(const object_ref<UserModule>& module,
                                       bool call_entry) {
  if (!module) {
    return X_STATUS_INVALID_HANDLE;
  }
  auto domain = module->import_domain_.lock();
  auto domain_operation =
      domain ? domain->Acquire() : NativeImportDomain::Operation{};
  if (module->import_domain_id_ && !domain_operation) {
    return X_STATUS_ACCESS_DENIED;
  }
  {
    auto global_lock = global_critical_region_.Acquire();
    const auto iter = std::ranges::find_if(
        user_modules_,
        [&module](const auto& e) { return e.get() == module.get(); });
    if (iter == user_modules_.end() || module->unloading_) {
      return X_STATUS_SUCCESS;
    }
    if (guest_runtimes_.contains(module->hmodule_ptr()) ||
        module->has_execution_leases()) {
      // A native system runtime has constructed process-lifetime heaps/static
      // objects. No partial-DLL teardown protocol is implied by attachment.
      XELOGW("Retaining {} while native runtime/execution ownership is active",
             module->name());
      return X_STATUS_ACCESS_DENIED;
    }
    // Claim exactly once before draining callbacks or running guest detach.
    // Deferred self-unloads retain this claim and the module's lifetime pin.
    module->unloading_ = true;
  }
  CompleteUserModuleUnload(module, call_entry);
  return X_STATUS_SUCCESS;
}

void KernelState::CompleteUserModuleUnload(const object_ref<UserModule>& module,
                                           bool call_entry) {
  auto domain = module->import_domain_.lock();
  auto domain_operation =
      domain ? domain->Acquire() : NativeImportDomain::Operation{};
  if (module->import_domain_id_ && !domain_operation) {
    return;  // Closed domain retains the claimed module until Kernel stop.
  }
  // Retiring callbacks may drain in-flight guest calls. Do that before taking
  // the kernel critical region, which those calls may themselves need.
  auto* graphics = emulator_->graphics_system();
  const bool graphics_can_wait =
      !graphics ||
      graphics->PrepareInterruptCallbackModuleShutdown(module->hmodule_ptr());
  // A graphics self-unload must not first wait on a SysApp callback that may
  // depend on this graphics invocation. Conversely, a SysApp self-unload must
  // not wait on graphics after its own registry requested a deferred retry.
  const bool apps_drained =
      graphics_can_wait &&
      app_manager()->BeginGuestAppModuleShutdown(module->hmodule_ptr());
  const bool graphics_drained =
      apps_drained &&
      (!graphics ||
       graphics->BeginInterruptCallbackModuleShutdown(module->hmodule_ptr()));
  if (!apps_drained || !graphics_drained) {
    // A callback cannot wait for itself. Keep the image alive and retry on the
    // system dispatch worker after the current guest invocation has returned.
    std::lock_guard lock(dispatch_mutex_);
    if (dispatch_thread_running_) {
      dispatch_queue_.push_back([this, module, call_entry]() {
        CompleteUserModuleUnload(module, call_entry);
      });
      dispatch_cond_.notify_all();
    } else {
      XELOGW(
          "Deferring module unload until teardown while a guest callback is "
          "active");
    }
    return;
  }
  app_manager()->RetireGuestAppsForModule(module->hmodule_ptr());
  if (graphics) {
    graphics->RetireInterruptCallbacksForModule(module->hmodule_ptr());
  }

  if (module->is_dll_module() && module->entry_point() && call_entry) {
    // Call DllMain(DLL_PROCESS_DETACH):
    // https://msdn.microsoft.com/en-us/library/windows/desktop/ms682583%28v=vs.85%29.aspx
    auto* thread_state = XThread::GetCurrentThread()->thread_state();
    thread_state->context()->r[3] = module->hmodule_ptr();
    thread_state->context()->r[4] = 0;  // DLL_PROCESS_DETACH
    thread_state->context()->r[5] = 0;
    if (!processor()->Execute(thread_state, module->entry_point())) {
      if (domain) {
        domain->Retire();
      }
      XELOGE(
          "Module detach execution incomplete for {}; retaining the "
          "unloading generation until kernel teardown",
          module->name());
      RetainNativeResourceUntilTeardown(
          std::make_shared<object_ref<UserModule>>(module));
      return;
    }
  }

  // Do not hold the kernel critical region across guest detach. It can invoke
  // callbacks that need the same region from another thread.
  auto global_lock = global_critical_region_.Acquire();
  if (domain && !domain->active()) {
    return;
  }
  auto iter = std::ranges::find_if(user_modules_, [&module](const auto& e) {
    return e.get() == module.get();
  });
  if (iter == user_modules_.end()) {
    return;
  }
  user_modules_.erase(iter);

  if (auto domain = module->import_domain_.lock()) {
    domain->Unregister({module->hmodule_ptr(), module->load_generation()});
  }

  object_table()->ReleaseHandleInLock(module->handle());
}

namespace {
// Loading/unloading runs guest code, so another operation must fail busy rather
// than block a scheduler fiber on a mutex held by a guest callback.
class SystemAppOperation {
 public:
  explicit SystemAppOperation(std::atomic_flag& active)
      : active_(active),
        acquired_(!active_.test_and_set(std::memory_order_acquire)) {}
  ~SystemAppOperation() {
    if (acquired_) {
      active_.clear(std::memory_order_release);
    }
  }
  bool acquired() const { return acquired_; }

 private:
  std::atomic_flag& active_;
  bool acquired_;
};
}  // namespace

X_HRESULT KernelState::LoadGuestSystemApp(
    uint32_t app_id, std::string_view module_path, uint32_t user_index,
    uint32_t options_ptr,
    std::shared_ptr<const cpu::GuestImportProvider> import_provider) {
  auto* thread = XThread::GetCurrentThread();
  if (!thread ||
      thread->guest_object<X_KTHREAD>()->process_type != X_PROCTYPE_SYSTEM ||
      (user_index >= XUserMaxUserCount && user_index != XUserIndexNone)) {
    return X_E_INVALIDARG;
  }
  SystemAppOperation operation(system_app_operation_active_);
  if (!operation.acquired()) {
    return X_HRESULT_FROM_WIN32(X_ERROR_BUSY);
  }
  auto* manager = app_manager();
  if (const auto existing = manager->guest_apps().Lookup(app_id, true)) {
    if (existing->execution_failed()) {
      return X_E_FAIL;
    }
    return manager->guest_apps().Lookup(app_id)
               ? X_E_SUCCESS
               : X_HRESULT_FROM_WIN32(X_ERROR_BUSY);
  }
  // Do not reinitialize or steal an image already owned by another load path.
  if (GetModule(module_path, true)) {
    return X_HRESULT_FROM_WIN32(X_ERROR_BUSY);
  }
  const bool native_consumer = bool(import_provider);
  auto module =
      LoadUserModule(module_path, false, true, std::move(import_provider));
  if (!module) {
    return X_E_NOTFOUND;
  }
  if (!module->is_system_app_module()) {
    // Another loader may have published it after the check above. Its owner
    // controls initialization and teardown; do not attach or unload it here.
    return X_HRESULT_FROM_WIN32(X_ERROR_BUSY);
  }
  if (native_consumer) {
    auto lease = AcquireUserModuleExecutionLease(module);
    if (!lease) {
      return X_HRESULT_FROM_WIN32(X_ERROR_BUSY);
    }
    auto lock = global_critical_region_.Acquire();
    native_system_app_leases_[module->hmodule_ptr()] = std::move(lease);
  }
  const auto load_result = FinishLoadingUserModule(module, false);
  if (XFAILED(load_result) || !module->is_dll_module() ||
      !module->entry_point() || !manager->PrepareGuestApp(app_id, module)) {
    manager->RetireGuestAppsForModule(module->hmodule_ptr());
    if (native_consumer) {
      auto lock = global_critical_region_.Acquire();
      native_system_app_leases_.erase(module->hmodule_ptr());
    }
    UnloadUserModule(module, false);
    return X_E_FAIL;
  }

  // The descriptor must exist before DllMain calls XamRegisterSysApp. Use the
  // system thread's own PCR/stack; never install this DLL as the title.
  auto* state = thread->thread_state();
  auto* context = state->context();
  context->r[3] = module->hmodule_ptr();
  context->r[4] = 1;  // DLL_PROCESS_ATTACH
  context->r[5] = 0;
  const bool executed = processor()->Execute(state, module->entry_point());
  const bool attached = executed && static_cast<uint32_t>(context->r[3]) != 0;
  module->is_attached_ = attached;
  auto registration = manager->guest_apps().Lookup(app_id, true);
  XELOGI("SysApp {:02X}: attach={} module={:08X} callback={:08X}", app_id,
         attached, module->hmodule_ptr(),
         registration ? registration->callback_address() : 0);
  if (!attached || !registration) {
    if (!native_consumer) {
      UnloadUserModule(module, attached);
    }
    return X_E_FAIL;
  }

  // Verified 17559 initialization ABI: a 16-byte parameter block and a
  // separate writable output word. HUD unconditionally dereferences arg1.
  const uint32_t packet = memory_->SystemHeapAlloc(20);
  if (!packet) {
    if (!native_consumer) {
      UnloadUserModule(module);
    }
    return X_HRESULT_FROM_WIN32(X_ERROR_NOT_ENOUGH_MEMORY);
  }
  auto* words = memory_->TranslateVirtual<xe::be<uint32_t>*>(packet);
  words[0] = 1;  // Primary system app load.
  words[1] = options_ptr;
  words[2] = module->hmodule_ptr();
  words[3] = user_index;
  words[4] = 0;
  const X_HRESULT result =
      registration->DispatchLifecycle(0x80000004, packet, packet + 16);
  const uint32_t output = words[4];
  if (!registration->execution_failed()) {
    memory_->SystemHeapFree(packet);
  }
  XELOGI("SysApp {:02X}: initialize={:08X} output={:08X}", app_id, result,
         output);
  // Native XUI consumers execute under their owning SYSTEM context. Keep
  // normal XMsg admission closed from the start; activation before returning
  // would expose an unguarded TITLE-thread entry during scene construction.
  // Their explicit owner uses lifecycle dispatch until a SYSTEM dispatcher
  // with matching context/lifetime admission is installed.
  if (static_cast<int32_t>(result) < 0 || registration->execution_failed() ||
      (!native_consumer && !manager->guest_apps().Activate(registration))) {
    if (!native_consumer && !registration->execution_failed()) {
      UnloadUserModule(module);
    }
    return static_cast<int32_t>(result) < 0 ? result : X_E_FAIL;
  }
  return result;
}

X_HRESULT KernelState::UnloadGuestSystemApp(uint32_t app_id) {
  return UnloadGuestSystemApp(app_id, {});
}

X_HRESULT KernelState::UnloadGuestSystemApp(
    uint32_t app_id,
    std::shared_ptr<UserModuleExecutionLease> execution_lease) {
  struct FailedLease {
    KernelState* owner;
    std::shared_ptr<UserModuleExecutionLease>& lease;
    ~FailedLease() {
      if (lease) {
        owner->RetainNativeResourceUntilTeardown(std::move(lease));
      }
    }
  } failed_lease{this, execution_lease};
  if (!XThread::IsInThread()) {
    return X_E_INVALIDARG;
  }
  auto* thread = XThread::GetCurrentThread();
  if (!thread ||
      thread->guest_object<X_KTHREAD>()->process_type != X_PROCTYPE_SYSTEM) {
    return X_E_INVALIDARG;
  }
  SystemAppOperation operation(system_app_operation_active_);
  if (!operation.acquired()) {
    return X_HRESULT_FROM_WIN32(X_ERROR_BUSY);
  }
  auto* manager = app_manager();
  auto registration = manager->guest_apps().Lookup(app_id, true);
  if (!registration) {
    return X_E_NOTFOUND;
  }
  auto module = GetUserModuleForAddress(registration->callback_address());
  if (!module || module->hmodule_ptr() != registration->owner_hmodule()) {
    return X_E_FAIL;
  }
  if (registration->execution_failed()) {
    return X_E_FAIL;
  }
  {
    auto lock = global_critical_region_.Acquire();
    const uint32_t expected_leases =
        uint32_t(native_system_app_leases_.contains(module->hmodule_ptr())) +
        uint32_t(bool(execution_lease));
    if (module->unloading_ || module->native_teardown_claimed_ ||
        (execution_lease &&
         (execution_lease.use_count() != 1 ||
          execution_lease->kernel_ != this ||
          execution_lease->owner_ != module.get() ||
          execution_lease->hmodule() != module->hmodule_ptr())) ||
        module->execution_leases_.load(std::memory_order_acquire) !=
            expected_leases) {
      return X_HRESULT_FROM_WIN32(X_ERROR_BUSY);
    }
    module->native_teardown_claimed_ = true;
  }
  // Stop admission without waiting: an older callback may need close03 to
  // finish. Preserve the verified close -> drain -> destroy ordering.
  if (!manager->guest_apps().StopNormalAdmission(registration)) {
    return X_HRESULT_FROM_WIN32(X_ERROR_BUSY);
  }
  const auto close_result = registration->DispatchLifecycle(0x80000003, 0, 0);
  if (registration->execution_failed()) {
    return X_E_FAIL;
  }
  if (!manager->guest_apps().BeginShutdown(registration) &&
      !manager->BeginGuestAppModuleShutdown(module->hmodule_ptr())) {
    return X_HRESULT_FROM_WIN32(X_ERROR_BUSY);
  }
  // Close may unregister its own generation. A retired callback rejects the
  // destructive call, but the pinned owner's module must still be unloaded.
  const auto destroy_result = registration->DispatchLifecycle(0x80000005, 0, 0);
  XELOGI("SysApp {:02X}: shutdown={:08X} destroy={:08X}", app_id, close_result,
         destroy_result);
  if (execution_lease && (XFAILED(close_result) || XFAILED(destroy_result))) {
    RetainNativeResourceUntilTeardown(std::move(execution_lease));
    return XFAILED(destroy_result) ? destroy_result : close_result;
  }
  if (registration->execution_failed()) {
    return X_E_FAIL;
  }
  {
    auto lock = global_critical_region_.Acquire();
    if (native_system_app_leases_.contains(module->hmodule_ptr()) &&
        (XFAILED(close_result) || XFAILED(destroy_result))) {
      return XFAILED(destroy_result) ? destroy_result : close_result;
    }
    native_system_app_leases_.erase(module->hmodule_ptr());
  }
  execution_lease.reset();
  const auto unload_result = UnloadUserModule(module);
  if (XFAILED(unload_result)) {
    return X_HRESULT_FROM_WIN32(X_ERROR_BUSY);
  }
  return static_cast<int32_t>(destroy_result) < 0 ? destroy_result
                                                  : close_result;
}

void KernelState::StartNativeGuideBootstrapProbe(uint32_t user_index) {
  if (native_guide_probe_thread_ || !GetExecutableModule()) {
    return;
  }
  const bool probe_xui =
      cvars::native_xui_probe || cvars::native_xui_render_probe ||
      cvars::native_guide_scene_probe || cvars::native_hud_controller_probe ||
      cvars::native_xui_target_probe ||
      cvars::native_system_device_binding_probe ||
      cvars::native_gpu_context_probe || cvars::native_guide_host_probe ||
      cvars::native_guide_real_time_probe || cvars::native_frame_lease_probe ||
      cvars::native_frame_execute_probe;
  const bool probe_hud = cvars::native_guide_bootstrap_probe;
  const uint64_t title_generation =
      emulator_->title_execution_state().snapshot().generation;
  native_guide_probe_stop_.store(false, std::memory_order_release);
  native_guide_probe_thread_ = object_ref<XHostThread>(new XHostThread(
      this, 1024 * 1024, X_CREATE_SUSPENDED,
      [this, user_index, probe_xui, probe_hud, title_generation]() {
        auto title = GetExecutableModule();
        const auto title_is_running = [&] {
          const auto snapshot = emulator_->title_execution_state().snapshot();
          return snapshot.generation == title_generation &&
                 snapshot.phase == TitleExecutionState::Phase::kRunning &&
                 emulator_->OwnsTitleExecution(this);
        };
        const auto start_deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(12);
        while (!title_is_running() &&
               !native_guide_probe_stop_.load(std::memory_order_acquire) &&
               std::chrono::steady_clock::now() < start_deadline) {
          XThread::GetCurrentThread()->Delay(0, 0, uint64_t(-100000ll));
        }
        if (!title_is_running() ||
            native_guide_probe_stop_.load(std::memory_order_acquire)) {
          XELOGE("Native system UI probe: foreground execution not admitted");
          return 0;
        }
        if (probe_xui) {
          // Cold-start allocation alone cannot demonstrate coexistence. Let the
          // title submit frames before entering the native engine, and keep the
          // wait bounded and cancellable during an in-process title reset.
          const uint32_t first_frame = xe::logging::GetFrameNumber();
          const auto deadline =
              std::chrono::steady_clock::now() + std::chrono::seconds(12);
          while ((!title_is_running() ||
                  uint32_t(xe::logging::GetFrameNumber() - first_frame) < 5) &&
                 !native_guide_probe_stop_.load(std::memory_order_acquire) &&
                 std::chrono::steady_clock::now() < deadline) {
            XThread::GetCurrentThread()->Delay(0, 0, uint64_t(-100000ll));
          }
          if (native_guide_probe_stop_.load(std::memory_order_acquire)) {
            return 0;
          }
          auto* graphics = emulator_->graphics_system();
          const uint32_t ready_frame = xe::logging::GetFrameNumber();
          if (!graphics || !title_is_running() ||
              uint32_t(ready_frame - first_frame) < 5 ||
              !graphics->ring_initialization_generation()) {
            XELOGE(
                "Native system UI probe: title presentation prerequisite "
                "not reached; XUI entry skipped");
            return 0;
          }
          const auto ring_generation =
              graphics->ring_initialization_generation();
          const auto title_interrupt =
              graphics->CaptureInterruptCallback(X_PROCTYPE_TITLE);
          XELOGI(
              "Native system UI probe: title ready frame={} ring={} "
              "execution_generation={} callback_generation={}",
              ready_frame, ring_generation, title_generation,
              title_interrupt ? title_interrupt->generation() : 0);
          const auto result = RunNativeXuiProbe(this);
          const auto final_interrupt =
              graphics->CaptureInterruptCallback(X_PROCTYPE_TITLE);
          XELOGI(
              "Native system UI probe: XUI result={:08X} title_preserved={} "
              "ring_preserved={} title_callback_preserved={} frame={}",
              result, GetExecutableModule().get() == title.get(),
              ring_generation == graphics->ring_initialization_generation(),
              title_interrupt == final_interrupt,
              xe::logging::GetFrameNumber());
          if (XSUCCEEDED(result) && cvars::native_frame_composite_probe) {
            // The diagnostic's Session is owned by this actual SYSTEM thread.
            // Keep that owner alive for presentation observation; thread exit
            // revokes it. This is not a production native-manager update loop.
            while (title_is_running() &&
                   !native_guide_probe_stop_.load(std::memory_order_acquire)) {
              XThread::GetCurrentThread()->Delay(0, 0, uint64_t(-160000ll));
            }
          }
        }
        if (!probe_hud) {
          return 0;
        }
        // Reusing the image address and app ID exercises retirement rather
        // than merely observing a first allocation's zero-filled memory.
        for (uint32_t iteration = 1; iteration <= 2; ++iteration) {
          const auto result =
              LoadGuestSystemApp(0xFF, "\\SystemRoot\\hud.xex", user_index);
          XELOGI("Native guide bootstrap {}: result={:08X} title_preserved={}",
                 iteration, result, GetExecutableModule().get() == title.get());
          if (app_manager()->guest_apps().Lookup(0xFF, true)) {
            const auto unload_result = UnloadGuestSystemApp(0xFF);
            XELOGI("Native guide bootstrap {}: unload={:08X}", iteration,
                   unload_result);
          }
          const bool retired = !GetModule("\\SystemRoot\\hud.xex", true) &&
                               !app_manager()->guest_apps().Lookup(0xFF, true);
          XELOGI("Native guide bootstrap {}: retired={}", iteration, retired);
          if (XFAILED(result) || !retired) {
            break;
          }
        }
        return 0;
      },
      GetSystemProcess()));
  native_guide_probe_thread_->set_name("Native System UI Probe");
  native_guide_probe_thread_->set_can_debugger_suspend(true);
  if (XFAILED(native_guide_probe_thread_->Create())) {
    native_guide_probe_thread_.reset();
    return;
  }
  native_guide_probe_thread_->Resume();
}

void KernelState::ShutdownNativeGuideBootstrapProbe() {
  native_guide_probe_stop_.store(true, std::memory_order_release);
  if (!native_guide_probe_thread_) {
    return;
  }
  // A guest request made by the probe itself must not join its own host thread.
  // Its owner retains the reference and joins before subsystem teardown.
  if (XThread::IsInThread(native_guide_probe_thread_.get())) {
    return;
  }
  native_guide_probe_thread_->Wait(0, 0, false, nullptr);
  native_guide_probe_thread_.reset();
}

void KernelState::InitXmpVolumePatch() {
  xmp_volume_patch_ = XmpVolumePatch::CreateForTitle(title_id(), this);
}

void KernelState::TerminateTitle() {
  XELOGI("KernelState::TerminateTitle");
  emulator_->NotifyTitleExecutionStopped(0, this);
  xe::FlushLog();
  std::quick_exit(EXIT_SUCCESS);
}

// Doesn't return when the host UI's handler took the title down.
static void HandOffToHostUI(Emulator* emulator,
                            const Emulator::ExitToDashboardCallback& handler) {
  // The asynchronous callback contract keeps the executing thread alive until
  // it parks. Do not retain an owning reference on a fiber stack that teardown
  // may discard without unwinding.
  auto* current_thread = XThread::GetCurrentThread();
  auto* owner = current_thread ? current_thread->kernel_state() : nullptr;
  const auto generation =
      emulator->title_execution_state().snapshot().generation;
  if (handler && handler()) {
    if (generation) {
      emulator->NotifyTitleExecutionStopped(generation, owner);
    }
    // Park off guest code until the host's reset or exit ends us; Suspend can
    // return on POSIX, so loop rather than fall through to TerminateTitle.
    if (current_thread) {
      current_thread->Suspend(nullptr);
    }
    while (true) {
      xe::threading::NanoSleep(int64_t(1'000'000'000));
    }
  }
}

void KernelState::ExitToDashboard() {
  XELOGI("KernelState::ExitToDashboard");
  HandOffToHostUI(emulator_, emulator_->on_exit_to_dashboard());
  TerminateTitle();
}

void KernelState::PowerOff() {
  XELOGI("KernelState::PowerOff");
  HandOffToHostUI(emulator_, emulator_->on_power_off());
  TerminateTitle();
}

void KernelState::RegisterThread(XThread* thread) {
  auto global_lock = global_critical_region_.Acquire();
  threads_by_id_[thread->thread_id()] = thread;
}

void KernelState::UnregisterThread(XThread* thread) {
  auto global_lock = global_critical_region_.Acquire();
  auto it = threads_by_id_.find(thread->thread_id());
  if (it != threads_by_id_.end()) {
    threads_by_id_.erase(it);
  }
}

// The kernel only notifies DLLs about threads of the title process. A thread of
// another process taking a DLL's DLL_PROCESS_ATTACH would run the module's
// process init, including its KeTlsAlloc calls, in that process. Host threads
// aren't console threads at all.
static bool NotifiesDlls(XThread* thread) {
  return thread->is_guest_thread() &&
         thread->guest_object<X_KTHREAD>()->process_type == X_PROCTYPE_TITLE;
}

void KernelState::OnThreadExecute(XThread* thread) {
  auto global_lock = global_critical_region_.Acquire();

  // Must be called on executing thread.
  assert_true(XThread::GetCurrentThread() == thread);

  if (!NotifiesDlls(thread)) {
    return;
  }

  // Call DllMain(DLL_THREAD_ATTACH) for each user module:
  // https://msdn.microsoft.com/en-us/library/windows/desktop/ms682583%28v=vs.85%29.aspx
  auto thread_state = thread->thread_state();
  for (auto user_module : user_modules_) {
    if (!user_module->is_system_app_module() && !user_module->unloading_ &&
        user_module->is_dll_module() && user_module->entry_point()) {
      uint64_t args[] = {
          user_module->hmodule_ptr(),
          user_module->is_attached()
              ? static_cast<uint64_t>(2)   // DLL_THREAD_ATTACH - Used to call
                                           // DLL for each thread created.
              : static_cast<uint64_t>(1),  // DLL_PROCESS_ATTACH - Used only
                                           // once for initialization.
          0,                               // 0 because always dynamic
      };

      user_module->is_attached_ = true;

      processor()->Execute(thread_state, user_module->entry_point(), args,
                           xe::countof(args));
    }
  }
}

void KernelState::OnThreadExit(XThread* thread) {
  auto global_lock = global_critical_region_.Acquire();

  // Must be called on executing thread.
  assert_true(XThread::GetCurrentThread() == thread);

  // Call DllMain(DLL_THREAD_DETACH) for each user module:
  // https://msdn.microsoft.com/en-us/library/windows/desktop/ms682583%28v=vs.85%29.aspx
  auto thread_state = thread->thread_state();
  const bool notifies_dlls = NotifiesDlls(thread);
  for (auto user_module : user_modules_) {
    if (notifies_dlls && !user_module->is_system_app_module() &&
        !user_module->unloading_ && user_module->is_dll_module() &&
        user_module->entry_point()) {
      uint64_t args[] = {
          user_module->hmodule_ptr(),
          3,  // DLL_THREAD_DETACH
          0,  // 0 because always dynamic
      };
      processor()->Execute(thread_state, user_module->entry_point(), args,
                           xe::countof(args));
    }
  }

  emulator()->processor()->OnThreadExit(thread->thread_id());
}

object_ref<XThread> KernelState::GetThreadByID(uint32_t thread_id) {
  auto global_lock = global_critical_region_.Acquire();
  XThread* thread = nullptr;
  auto it = threads_by_id_.find(thread_id);
  if (it != threads_by_id_.end()) {
    thread = it->second;
  }
  return retain_object(thread);
}

std::vector<uint32_t> KernelState::GetAllThreadIDs() {
  auto global_lock = global_critical_region_.Acquire();

  auto thread_ids_view =
      threads_by_id_ |
      std::views::transform([](const auto& pair) { return pair.first; });

  std::vector<std::uint32_t> thread_ids(thread_ids_view.begin(),
                                        thread_ids_view.end());

  return thread_ids;
}

void KernelState::RegisterNotifyListener(XNotifyListener* listener) {
  auto global_lock = global_critical_region_.Acquire();
  notify_listeners_.push_back(retain_object(listener));

  // Games seem to expect a few notifications on startup, only for the first
  // listener.
  // https://cs.rin.ru/forum/viewtopic.php?f=38&t=60668&hilit=resident+evil+5&start=375
  if (!has_notified_startup_ && listener->mask() & kXNotifySystem) {
    has_notified_startup_ = true;
    const bool native_visible =
        !listener->is_system() &&
        NativeSystemUiNotifications::CurrentVisibility(this).value_or(false);
    listener->EnqueueNotification(kXNotificationSystemUI,
                                  native_visible || xam_state()->IsUIActive());
    listener->EnqueueNotification(kXNotificationSystemSignInChanged, 1);
    // Sent when a storage device comes or goes. The dashboard rescans its
    // content on it. Content can change on the host between runs so the
    // dashboard gets one at start.
    if (title_id() == kDashboardID) {
      listener->EnqueueNotification(kXNotificationSystemStorageDevicesChanged,
                                    0);
    }
  }
  if (!has_notified_live_startup_ && listener->mask() & kXNotifyLive) {
    has_notified_live_startup_ = true;
    // X_ONLINE_S_LOGON_DISCONNECTED
    listener->EnqueueNotification(kXNotificationLiveConnectionChanged,
                                  0x001510F1L);
    listener->EnqueueNotification(kXNotificationLiveLinkStateChanged,
                                  xam::SystemLinkEnabled());
  }

  if (!has_notified_xmp_startup_ && listener->mask() & kXNotifyXmp) {
    has_notified_xmp_startup_ = true;
    // Playback state is idle until the media player broadcasts a transition
    // of its own, so only the controller is worth priming.
    listener->EnqueueNotification(
        kXNotificationXmpPlaybackControllerChanged,
        emulator()->audio_media_player()->IsTitleInPlaybackControl());
  }
}

void KernelState::UnregisterNotifyListener(XNotifyListener* listener) {
  auto global_lock = global_critical_region_.Acquire();
  for (auto it = notify_listeners_.begin(); it != notify_listeners_.end();
       ++it) {
    if ((*it).get() == listener) {
      notify_listeners_.erase(it);
      break;
    }
  }
}

void KernelState::BroadcastNotification(XNotificationID id, uint32_t data) {
  auto global_lock = global_critical_region_.Acquire();
  const bool native_visible =
      id == kXNotificationSystemUI &&
      NativeSystemUiNotifications::CurrentVisibility(this).value_or(false);
  for (const auto& notify_listener : notify_listeners_) {
    // Closing a host dialog while the native UI remains visible must not
    // report an inactive system UI to TITLE HLE listeners. Native SYSTEM
    // listeners remain in their separate native registry.
    const uint32_t listener_data =
        native_visible && !notify_listener->is_system() ? 1 : data;
    notify_listener->EnqueueNotification(id, listener_data);
  }
}

void KernelState::BroadcastNativeSystemUiNotification(bool visible) {
  auto global_lock = global_critical_region_.Acquire();
  const bool active = visible || xam_state()->IsUIActive();
  for (const auto& listener : notify_listeners_) {
    if (!listener->is_system()) {
      listener->EnqueueNotification(kXNotificationSystemUI, active);
    }
  }
}

void KernelState::CompleteOverlapped(uint32_t overlapped_ptr, X_RESULT result) {
  CompleteOverlappedEx(overlapped_ptr, result, result, 0,
                       XThread::GetCurrentThreadHandle());
}

void KernelState::CompleteOverlappedEx(uint32_t overlapped_ptr, X_RESULT result,
                                       uint32_t extended_error, uint32_t length,
                                       X_HANDLE thread_handle) {
  auto ptr = memory()->TranslateVirtual(overlapped_ptr);
  // Result last, so a caller polling it for completion reads a valid length.
  // A completed request leaves no context behind.
  XOverlappedSetExtendedError(ptr, extended_error);
  XOverlappedSetLength(ptr, length);
  XOverlappedSetContext(ptr, 0);
  std::atomic_thread_fence(std::memory_order_release);
  XOverlappedSetResult(ptr, result);
  // The event is left unsignaled when there is a completion routine.
  X_HANDLE event_handle = XOverlappedGetEvent(ptr);
  if (event_handle && !XOverlappedGetCompletionRoutine(ptr)) {
    auto ev = object_table()->LookupObject<XEvent>(event_handle);
    assert_not_null(ev);
    if (ev) {
      ev->Set(0, false);
    }
  }
  if (XOverlappedGetCompletionRoutine(ptr)) {
    auto thread = object_table()->LookupObject<XThread>(thread_handle);
    if (thread) {
      // Queue APC on the thread that requested the overlapped operation.
      uint32_t routine = XOverlappedGetCompletionRoutine(ptr);
      thread->EnqueueApc(routine, result, length, overlapped_ptr);
    }
  }
}

void KernelState::CompleteOverlappedImmediate(uint32_t overlapped_ptr,
                                              X_RESULT result) {
  // TODO(gibbed): there are games that check 'length' of overlapped as
  // an indication of success. WTF?
  // Setting length to -1 when not success seems to be helping.
  uint32_t length = !result ? 0 : 0xFFFFFFFF;
  CompleteOverlappedImmediateEx(overlapped_ptr, result, result, length);
}

void KernelState::CompleteOverlappedImmediateEx(uint32_t overlapped_ptr,
                                                X_RESULT result,
                                                uint32_t extended_error,
                                                uint32_t length) {
  CompleteOverlappedEx(overlapped_ptr, result, extended_error, length,
                       XThread::GetCurrentThreadHandle());
}

void KernelState::CompleteOverlappedDeferred(
    std::function<void()> completion_callback, uint32_t overlapped_ptr,
    X_RESULT result, std::function<void()> pre_callback,
    std::function<void()> post_callback) {
  CompleteOverlappedDeferredEx(std::move(completion_callback), overlapped_ptr,
                               result, result, 0, pre_callback, post_callback);
}

void KernelState::CompleteOverlappedDeferredEx(
    std::function<void()> completion_callback, uint32_t overlapped_ptr,
    X_RESULT result, uint32_t extended_error, uint32_t length,
    std::function<void()> pre_callback, std::function<void()> post_callback) {
  CompleteOverlappedDeferredEx(
      [completion_callback, result, extended_error, length](
          uint32_t& cb_extended_error, uint32_t& cb_length) -> X_RESULT {
        completion_callback();
        cb_extended_error = extended_error;
        cb_length = length;
        return result;
      },
      overlapped_ptr, pre_callback, post_callback);
}

void KernelState::CompleteOverlappedDeferred(
    std::function<X_RESULT()> completion_callback, uint32_t overlapped_ptr,
    std::function<void()> pre_callback, std::function<void()> post_callback) {
  CompleteOverlappedDeferredEx(
      [completion_callback](uint32_t& extended_error,
                            uint32_t& length) -> X_RESULT {
        auto result = completion_callback();
        extended_error = static_cast<uint32_t>(result);
        length = 0;
        return result;
      },
      overlapped_ptr, pre_callback, post_callback);
}

void KernelState::CompleteOverlappedDeferredEx(
    std::function<X_RESULT(uint32_t&, uint32_t&)> completion_callback,
    uint32_t overlapped_ptr, std::function<void()> pre_callback,
    std::function<void()> post_callback) {
  auto ptr = memory()->TranslateVirtual(overlapped_ptr);
  XOverlappedSetResult(ptr, X_ERROR_IO_PENDING);
  const X_HANDLE thread_handle = XThread::GetCurrentThreadHandle();
  X_HANDLE event_handle = XOverlappedGetEvent(ptr);
  if (event_handle) {
    auto ev = object_table()->LookupObject<XObject>(event_handle);

    assert_not_null(ev);
    if (ev && ev->type() == XObject::Type::Event) {
      ev.get<XEvent>()->Reset();
    }
  }
  std::lock_guard lock(dispatch_mutex_);
  dispatch_queue_.push_back([this, completion_callback, overlapped_ptr,
                             thread_handle, pre_callback, post_callback]() {
    if (pre_callback) {
      pre_callback();
    }
    // 5454082B infinitely loads free roam in netplay without sleep.
    xe::threading::Sleep(kDeferredOverlappedDelayMillis);
    uint32_t extended_error, length;
    auto result = completion_callback(extended_error, length);
    CompleteOverlappedEx(overlapped_ptr, result, extended_error, length,
                         thread_handle);
    if (post_callback) {
      post_callback();
    }
  });
  dispatch_cond_.notify_all();
}

bool KernelState::KeepNativeStfsDevice(
    const object_ref<XStfsDevice>& device) {
  if (!device) { return false; }
  auto lock = global_critical_region_.Acquire();
  constexpr size_t kMaximumBodies = 128;
  constexpr uint64_t kMaximumContextBytes = 16 * 1024 * 1024;
  uint64_t bytes = device->context_size();
  for (const auto& old : native_stfs_devices_) { bytes += old->context_size(); }
  if (native_stfs_devices_.size() >= kMaximumBodies ||
      bytes > kMaximumContextBytes) {
    return false;
  }
  native_stfs_devices_.push_back(device);
  return true;
}

bool KernelState::Save(ByteStream* stream) {
  {
    auto lock = global_critical_region_.Acquire();
    if (!guest_runtimes_.empty() || !native_stfs_devices_.empty()) {
      XELOGE("Native runtime/device save/restore ownership is not implemented");
      return false;
    }
  }
  XELOGD("Serializing the kernel...");
  stream->Write(kKernelSaveSignature);

  // Save the object table
  object_table_.Save(stream);

  // Write the TLS allocation bitmap
  // We save XThreads absolutely first, as they will execute code upon save
  // (which could modify the kernel state)
  auto threads = object_table_.GetObjectsByType<XThread>();
  uint32_t* num_threads_ptr =
      reinterpret_cast<uint32_t*>(stream->data() + stream->offset());
  stream->Write(static_cast<uint32_t>(threads.size()));

  size_t num_threads = threads.size();
  XELOGD("Serializing {} threads...", threads.size());
  for (auto thread : threads) {
    if (!thread->is_guest_thread()) {
      // Don't save host threads. They can be reconstructed on startup.
      num_threads--;
      continue;
    }

    if (!thread->Save(stream)) {
      XELOGD("Failed to save thread \"{}\"", thread->name());
      num_threads--;
    }
  }

  *num_threads_ptr = static_cast<uint32_t>(num_threads);

  // Save all other objects
  auto objects = object_table_.GetAllObjects();
  uint32_t* num_objects_ptr =
      reinterpret_cast<uint32_t*>(stream->data() + stream->offset());
  stream->Write(static_cast<uint32_t>(objects.size()));

  size_t num_objects = objects.size();
  XELOGD("Serializing {} objects...", num_objects);
  for (auto object : objects) {
    auto prev_offset = stream->offset();

    // A user module is a host object only to stay out of the title's handle
    // numbering, so it still saves.
    bool user_module = object->type() == XObject::Type::Module &&
                       static_cast<XModule*>(object.get())->module_type() ==
                           XModule::ModuleType::kUserModule;
    if ((object->is_host_object() && !user_module) ||
        object->type() == XObject::Type::Thread) {
      // Don't save host objects or save XThreads again
      num_objects--;
      continue;
    }

    stream->Write<uint32_t>(static_cast<uint32_t>(object->type()));
    if (!object->Save(stream)) {
      XELOGD("Did not save object of type {}",
             static_cast<uint32_t>(object->type()));
      assert_always();

      // Revert backwards and overwrite if a save failed.
      stream->set_offset(prev_offset);
      num_objects--;
    }
  }

  *num_objects_ptr = static_cast<uint32_t>(num_objects);
  return true;
}

// this only gets triggered once per ms at most, so fields other than tick count
// will probably not be updated in a timely manner for guest code that uses them
void KernelState::UpdateKeTimestampBundle() {
  X_TIME_STAMP_BUNDLE* lpKeTimeStampBundle =
      memory_->TranslateVirtual<X_TIME_STAMP_BUNDLE*>(ke_timestamp_bundle_ptr_);
  uint32_t uptime_ms = Clock::QueryGuestUptimeMillis();
  xe::store_and_swap<uint64_t>(&lpKeTimeStampBundle->interrupt_time,
                               Clock::QueryGuestInterruptTime());
  xe::store_and_swap<uint64_t>(&lpKeTimeStampBundle->system_time,
                               Clock::QueryGuestSystemTime());
  xe::store_and_swap<uint32_t>(&lpKeTimeStampBundle->tick_count, uptime_ms);
}

uint32_t KernelState::GetKeTimestampBundle() {
  XE_LIKELY_IF(ke_timestamp_bundle_ptr_) { return ke_timestamp_bundle_ptr_; }
  else {
    global_critical_region::PrepareToAcquire();
    return CreateKeTimestampBundle();
  }
}

XE_NOINLINE
XE_COLD
uint32_t KernelState::CreateKeTimestampBundle() {
  auto crit = global_critical_region::Acquire();

  // Check again under lock - should have been initialized during boot
  if (ke_timestamp_bundle_ptr_) {
    // Already initialized during InitializeKernelGuestGlobals
    // Timer should also already be running
    assert_not_null(timestamp_timer_);
    return ke_timestamp_bundle_ptr_;
  }

  // Should never reach here - timestamp bundle should be initialized during
  // InitializeKernelGuestGlobals()
  assert_always(
      "CreateKeTimestampBundle called but bundle not initialized during boot");
  return 0;
}

bool KernelState::Restore(ByteStream* stream) {
  {
    auto lock = global_critical_region_.Acquire();
    if (!guest_runtimes_.empty() || !native_stfs_devices_.empty()) {
      XELOGE("Cannot restore over a resident native runtime/device generation");
      return false;
    }
  }
  // Check the magic value.
  if (stream->Read<uint32_t>() != kKernelSaveSignature) {
    return false;
  }

  // Restore the object table
  object_table_.Restore(stream);

  // TLS bitmap is now stored per-process in X_KPROCESS structures (in guest
  // memory) Skip reading old global TLS bitmap if present in old save files
  auto num_bitmap_entries = stream->Read<uint32_t>();
  for (uint32_t i = 0; i < num_bitmap_entries; i++) {
    stream->Read<uint64_t>();  // Discard old data
  }

  uint32_t num_threads = stream->Read<uint32_t>();
  XELOGD("Loading {} threads...", num_threads);
  for (uint32_t i = 0; i < num_threads; i++) {
    auto thread = XObject::Restore(this, XObject::Type::Thread, stream);
    if (!thread) {
      // Can't continue the restore or we risk misalignment.
      assert_always();
      return false;
    }
  }

  uint32_t num_objects = stream->Read<uint32_t>();
  XELOGD("Loading {} objects...", num_objects);
  for (uint32_t i = 0; i < num_objects; i++) {
    uint32_t type = stream->Read<uint32_t>();

    auto obj = XObject::Restore(this, XObject::Type(type), stream);
    if (!obj) {
      // Can't continue the restore or we risk misalignment.
      assert_always();
      return false;
    }
  }

  if (auto* graphics = emulator_->graphics_system()) {
    graphics->RebindRestoredInterruptCallbackModule();
  }
  return true;
}

std::bitset<4> KernelState::GetConnectedUsers() const {
  auto input_sys = emulator_->input_system();

  auto lock = input_sys->lock();

  return input_sys->GetConnectedSlots();
}
// todo: definitely need to do more to pretend to be in a dpc
void DPCImpersonationScope::Enter(X_KPCR& pcr, uint32_t process_type) {
  xenia_assert(pcr.prcb_data.dpc_active == 0);
  previous_irql_ = pcr.current_irql;
  previous_process_type_ = pcr.processtype_value_in_dpc;
  previous_cpu_ = pcr.prcb_data.current_cpu;
  previous_dpc_active_ = pcr.prcb_data.dpc_active;
  pcr.current_irql = 2;
  pcr.prcb_data.dpc_active = 1;
  if (process_type != UINT32_MAX) {
    pcr.processtype_value_in_dpc = static_cast<uint8_t>(process_type);
  }
}

void DPCImpersonationScope::Leave(X_KPCR& pcr) const {
  pcr.current_irql = previous_irql_;
  pcr.processtype_value_in_dpc = previous_process_type_;
  pcr.prcb_data.current_cpu = previous_cpu_;
  pcr.prcb_data.dpc_active = previous_dpc_active_;
}

void DPCImpersonationScope::Run(X_KPCR& pcr, uint32_t process_type, uint8_t cpu,
                                const std::function<void(uint8_t)>& select_cpu,
                                const std::function<void()>& callback,
                                const std::function<void()>& deferred_dpcs) {
  DPCImpersonationScope scope;
  scope.Enter(pcr, process_type);
  struct Restore {
    X_KPCR& pcr;
    const DPCImpersonationScope& scope;
    const std::function<void(uint8_t)>& select_cpu;
    ~Restore() {
      select_cpu(scope.previous_cpu_);
      scope.Leave(pcr);
    }
  } restore{pcr, scope, select_cpu};
  select_cpu(cpu);
  callback();
  deferred_dpcs();
}

void KernelState::BeginDPCImpersonation(cpu::ppc::PPCContext* context,
                                        DPCImpersonationScope& scope) {
  auto kpcr = context->TranslateVirtualGPR<X_KPCR*>(context->r[13]);
  scope.Enter(*kpcr);
}
void KernelState::EndDPCImpersonation(cpu::ppc::PPCContext* context,
                                      DPCImpersonationScope& end_scope) {
  // DPCs the routine queued run before the CPU leaves DISPATCH_LEVEL.
  xboxkrnl::xeRunDeferredDpcs(context);
  auto kpcr = context->TranslateVirtualGPR<X_KPCR*>(context->r[13]);
  xenia_assert(kpcr->prcb_data.dpc_active == 1);
  if (XThread::IsInThread()) {
    XThread::GetCurrentThread()->SetActiveCpu(end_scope.previous_cpu_);
  }
  end_scope.Leave(*kpcr);
}
void KernelState::QueueDpc(uint32_t dpc_ptr, uint32_t arg1, uint32_t arg2) {
  auto target = memory()->TranslateVirtual<XDPC*>(dpc_ptr);
  const uint8_t desired_cpu = target->desired_cpu_number;
  if (target->routine && desired_cpu && desired_cpu <= 6 &&
      processor_dpcs_[desired_cpu - 1].started.load(
          std::memory_order_acquire)) {
    QueueProcessorDpc(desired_cpu - 1, dpc_ptr, arg1, arg2);
    return;
  }
  {
    std::lock_guard lock(dispatch_mutex_);
    if (std::find(dispatch_dpcs_.begin(), dispatch_dpcs_.end(), dpc_ptr) !=
        dispatch_dpcs_.end()) {
      return;
    }
    dispatch_dpcs_.push_back(dpc_ptr);
    dispatch_queue_.push_back([this, dpc_ptr, arg1, arg2]() {
      {
        std::lock_guard lock(dispatch_mutex_);
        auto it =
            std::find(dispatch_dpcs_.begin(), dispatch_dpcs_.end(), dpc_ptr);
        // KeRemoveQueueDpc took it back.
        if (it == dispatch_dpcs_.end()) {
          return;
        }
        dispatch_dpcs_.erase(it);
      }
      auto context = XThread::GetCurrentThread()->thread_state()->context();
      DPCImpersonationScope dpc_scope{};
      BeginDPCImpersonation(context, dpc_scope);
      xboxkrnl::xeRunDpc(context, dpc_ptr, arg1, arg2);
      EndDPCImpersonation(context, dpc_scope);
    });
  }
  dispatch_cond_.notify_all();
}

void KernelState::StartProcessorDpcThread(uint8_t cpu) {
  auto& dpcs = processor_dpcs_[cpu];
  if (dpcs.started.load(std::memory_order_acquire) ||
      !GuestScheduler::enabled()) {
    return;
  }
  auto global_lock = global_critical_region_.Acquire();
  if (dpcs.thread) {
    return;
  }
  dpcs.event = object_ref<XEvent>(new XEvent(this, true));
  dpcs.event->Initialize(false, false);
  dpcs.thread = object_ref<XHostThread>(new XHostThread(
      this, 128 * 1024, (1u << cpu) << 24,
      [this, cpu]() {
        RunProcessorDpcs(cpu);
        return 0;
      },
      GetSystemProcess(), true));
  dpcs.thread->set_can_debugger_suspend(true);
  dpcs.thread->set_name(fmt::format("DPC Processor {}", cpu));
  if (XFAILED(dpcs.thread->Create())) {
    dpcs.thread.reset();
    dpcs.event.reset();
    return;
  }
  // At the top priority, as a DPC interrupts what runs below DISPATCH_LEVEL.
  dpcs.thread->SetPriority(31);
  dpcs.started.store(true, std::memory_order_release);
}

bool KernelState::QueueProcessorDpc(uint8_t cpu, uint32_t dpc_ptr,
                                    uint32_t arg1, uint32_t arg2) {
  auto& dpcs = processor_dpcs_[cpu];
  {
    std::lock_guard lock(dpcs.lock);
    if (std::find(dpcs.queue.begin(), dpcs.queue.end(), dpc_ptr) !=
        dpcs.queue.end()) {
      return false;
    }
    auto dpc = memory()->TranslateVirtual<XDPC*>(dpc_ptr);
    dpc->arg1 = arg1;
    dpc->arg2 = arg2;
    dpcs.queue.push_back(dpc_ptr);
  }
  dpcs.event->Set(0, false);
  return true;
}

bool KernelState::IsProcessorDpcQueued(uint8_t cpu, uint32_t dpc_ptr) {
  auto& dpcs = processor_dpcs_[cpu];
  if (!dpcs.started.load(std::memory_order_acquire)) {
    return false;
  }
  std::lock_guard lock(dpcs.lock);
  return std::find(dpcs.queue.begin(), dpcs.queue.end(), dpc_ptr) !=
         dpcs.queue.end();
}

bool KernelState::RemoveProcessorDpc(uint32_t dpc_ptr) {
  for (auto& dpcs : processor_dpcs_) {
    std::lock_guard lock(dpcs.lock);
    auto it = std::find(dpcs.queue.begin(), dpcs.queue.end(), dpc_ptr);
    if (it != dpcs.queue.end()) {
      dpcs.queue.erase(it);
      return true;
    }
  }
  return false;
}

void KernelState::RunProcessorDpcs(uint8_t cpu) {
  auto& dpcs = processor_dpcs_[cpu];
  auto context = XThread::GetCurrentThread()->thread_state()->context();
  while (true) {
    dpcs.event->Wait(0, 0, 0, nullptr);
    DPCImpersonationScope dpc_scope{};
    BeginDPCImpersonation(context, dpc_scope);
    while (true) {
      // Its arguments are taken as it leaves the queue, so KeInsertQueueDpc
      // may queue it again before it runs.
      uint32_t dpc_ptr, arg1, arg2;
      {
        std::lock_guard lock(dpcs.lock);
        if (dpcs.queue.empty()) {
          break;
        }
        dpc_ptr = dpcs.queue.front();
        dpcs.queue.pop_front();
        auto dpc = memory()->TranslateVirtual<XDPC*>(dpc_ptr);
        arg1 = dpc->arg1;
        arg2 = dpc->arg2;
      }
      xboxkrnl::xeRunDpc(context, dpc_ptr, arg1, arg2);
    }
    EndDPCImpersonation(context, dpc_scope);
  }
}

bool KernelState::RemoveDpc(uint32_t dpc_ptr) {
  std::lock_guard lock(dispatch_mutex_);
  auto it = std::find(dispatch_dpcs_.begin(), dispatch_dpcs_.end(), dpc_ptr);
  if (it == dispatch_dpcs_.end()) {
    return false;
  }
  dispatch_dpcs_.erase(it);
  return true;
}

void KernelState::EmulateCPInterruptDPC(uint32_t interrupt_callback,
                                        uint32_t interrupt_callback_data,
                                        uint32_t source, uint32_t cpu,
                                        uint32_t process_type) {
  // Source 0 is vblank, where the console's graphics DPC enters background
  // mode. Before the callback check, which the console also does without one.
  if (source == 0 && GuestScheduler::enabled()) {
    guest_scheduler()->EnterBackgroundMode();
  }
  if (!interrupt_callback) {
    return;
  }

  auto thread = kernel::XThread::GetCurrentThread();
  assert_not_null(thread);

  // Pick a CPU, if needed. We're going to guess 2. Because.
  if (cpu == 0xFFFFFFFF) {
    cpu = 2;
  }

  /*
    in reality, our interrupt is a callback that is called in a dpc which is
    scheduled by the actual interrupt

    we need to impersonate a dpc
  */
  auto current_context = thread->thread_state()->context();
  auto pcr = memory()->TranslateVirtual<X_KPCR*>(thread->pcr_ptr());
  DPCImpersonationScope::Run(
      *pcr, process_type, static_cast<uint8_t>(cpu),
      [thread](uint8_t selected_cpu) { thread->SetActiveCpu(selected_cpu); },
      [&]() {
        uint64_t args[] = {source, interrupt_callback_data};
        processor_->Execute(thread->thread_state(), interrupt_callback, args,
                            xe::countof(args));
      },
      [&]() { xboxkrnl::xeRunDeferredDpcs(current_context); });
}

uint32_t KernelState::GetBackgroundProcessors() {
  return memory()
      ->TranslateVirtual<KernelGuestGlobals*>(GetKernelGuestGlobals())
      ->background_processors;
}

void KernelState::SetBackgroundProcessors(uint32_t processors) {
  memory()
      ->TranslateVirtual<KernelGuestGlobals*>(GetKernelGuestGlobals())
      ->background_processors = processors;
}

void KernelState::InitializeProcess(X_KPROCESS* process, uint32_t type,
                                    char priority_class, char default_priority,
                                    char max_dynamic_priority) {
  uint32_t guest_kprocess = memory()->HostToGuestVirtual(process);

  uint32_t thread_list_guest_ptr =
      guest_kprocess + offsetof(X_KPROCESS, thread_list);

  process->process_priority_class = priority_class;
  process->default_thread_priority = default_priority;
  process->max_dynamic_priority = max_dynamic_priority;
  util::XeInitializeListHead(&process->thread_list, thread_list_guest_ptr);
  process->quantum = 60;
  // doubt any guest code uses this ptr, which i think probably has something to
  // do with the page table
  process->clrdataa_masked_ptr = 0;
  // clrdataa_ & ~(1U << 31);
  process->thread_count = 0;
  process->disable_quantum_decay = 0x06;
  process->kernel_stack_size = 16 * 1024;
  process->tls_slot_size = 0x80;

  process->process_type = type;
  uint32_t unk_list_guest_ptr = guest_kprocess + offsetof(X_KPROCESS, unk_54);
  // TODO(benvanik): figure out what this list is.
  util::XeInitializeListHead(&process->unk_54, unk_list_guest_ptr);
}

void KernelState::SetProcessTLSVars(X_KPROCESS* process, int num_slots,
                                    int tls_data_size,
                                    int tls_static_data_address) {
  uint32_t slots_padded = (num_slots + 3) & 0xFFFFFFFC;
  process->tls_data_size = tls_data_size;
  process->tls_raw_data_size = tls_data_size;
  process->tls_static_data_address = tls_static_data_address;
  process->tls_slot_size = 4 * slots_padded;
  uint32_t count_div32 = slots_padded / 32;
  for (unsigned word_index = 0; word_index < count_div32; ++word_index) {
    process->tls_slot_bitmap[word_index] = -1;
  }

  // set remainder of bitset
  if (((num_slots + 3) & 0x1C) != 0) {
    process->tls_slot_bitmap[count_div32] = -1
                                            << (32 - ((num_slots + 3) & 0x1C));
  }
}
void AllocateThread(PPCContext* context) {
  uint32_t thread_mem_size = static_cast<uint32_t>(context->r[3]);
  uint32_t a2 = static_cast<uint32_t>(context->r[4]);
  uint32_t a3 = static_cast<uint32_t>(context->r[5]);
  if (thread_mem_size <= 0xFD8) {
    thread_mem_size += 8;
  }
  uint32_t result =
      xboxkrnl::xeAllocatePoolTypeWithTag(context, thread_mem_size, a2, a3);
  if (((unsigned short)result & 0xFFF) != 0) {
    result += 2;
  }

  context->r[3] = static_cast<uint64_t>(result);
}
void FreeThread(PPCContext* context) {
  uint32_t thread_memory = static_cast<uint32_t>(context->r[3]);
  if ((thread_memory & 0xFFF) != 0) {
    thread_memory -= 8;
  }
  xboxkrnl::xeFreePool(context, thread_memory);
}

void SimpleForwardAllocatePoolTypeWithTag(PPCContext* context) {
  uint32_t a1 = static_cast<uint32_t>(context->r[3]);
  uint32_t a2 = static_cast<uint32_t>(context->r[4]);
  uint32_t a3 = static_cast<uint32_t>(context->r[5]);
  context->r[3] = static_cast<uint64_t>(
      xboxkrnl::xeAllocatePoolTypeWithTag(context, a1, a2, a3));
}
void SimpleForwardFreePool(PPCContext* context) {
  xboxkrnl::xeFreePool(context, static_cast<uint32_t>(context->r[3]));
}

void DeleteMutant(PPCContext* context) {
  // todo: this should call kereleasemutant with some specific args

  xe::FatalError("DeleteMutant - need KeReleaseMutant(mutant, 1, 1, 0) ");
}
void DeleteTimer(PPCContext* context) {
  // todo: this should call KeCancelTimer
  xe::FatalError("DeleteTimer - need KeCancelTimer(mutant, 1, 1, 0) ");
}

void DeleteIoCompletion(PPCContext* context) {}

void UnknownProcIoDevice(PPCContext* context) {}

void CloseFileProc(PPCContext* context) {}

void DeleteFileProc(PPCContext* context) {}

void UnknownFileProc(PPCContext* context) {}

void DeleteSymlink(PPCContext* context) {
  X_KSYMLINK* lnk = context->TranslateVirtualGPR<X_KSYMLINK*>(context->r[3]);

  context->r[3] = lnk->refed_object_maybe;
  xboxkrnl::xeObDereferenceObject(context, lnk->refed_object_maybe);
}
void KernelState::InitializeKernelGuestGlobals() {
  kernel_guest_globals_ = memory_->SystemHeapAlloc(sizeof(KernelGuestGlobals));

  KernelGuestGlobals* block =
      memory_->TranslateVirtual<KernelGuestGlobals*>(kernel_guest_globals_);
  memset(block, 0, sizeof(KernelGuestGlobals));

  auto idle_process = memory()->TranslateVirtual<X_KPROCESS*>(GetIdleProcess());
  InitializeProcess(idle_process, X_PROCTYPE_IDLE, 0, 0, 0);
  idle_process->quantum = 0x7F;
  auto system_process =
      memory()->TranslateVirtual<X_KPROCESS*>(GetSystemProcess());
  InitializeProcess(system_process, X_PROCTYPE_SYSTEM, 2, 5, 9);
  SetProcessTLSVars(system_process, 32, 0, 0);

  uint32_t oddobject_offset =
      kernel_guest_globals_ +
      offsetof(KernelGuestGlobals, XboxKernelDefaultObject);

  // init unknown object

  block->XboxKernelDefaultObject.type = EventSynchronizationObject;
  block->XboxKernelDefaultObject.signal_state = 1;
  block->XboxKernelDefaultObject.wait_list.flink_ptr =
      oddobject_offset + offsetof(X_DISPATCH_HEADER, wait_list.flink_ptr);
  block->XboxKernelDefaultObject.wait_list.blink_ptr =
      block->XboxKernelDefaultObject.wait_list.flink_ptr;

  // init thread object
  block->ExThreadObjectType.pool_tag = 0x65726854;
  block->ExThreadObjectType.allocate_proc =
      kernel_trampoline_group_.NewLongtermTrampoline(AllocateThread);

  block->ExThreadObjectType.free_proc =
      kernel_trampoline_group_.NewLongtermTrampoline(FreeThread);

  // several object types just call freepool/allocatepool
  uint32_t trampoline_allocatepool =
      kernel_trampoline_group_.NewLongtermTrampoline(
          SimpleForwardAllocatePoolTypeWithTag);
  uint32_t trampoline_freepool =
      kernel_trampoline_group_.NewLongtermTrampoline(SimpleForwardFreePool);

  // init event object
  block->ExEventObjectType.pool_tag = 0x76657645;
  block->ExEventObjectType.allocate_proc = trampoline_allocatepool;
  block->ExEventObjectType.free_proc = trampoline_freepool;

  // init mutant object
  block->ExMutantObjectType.pool_tag = 0x6174754D;
  block->ExMutantObjectType.allocate_proc = trampoline_allocatepool;
  block->ExMutantObjectType.free_proc = trampoline_freepool;

  block->ExMutantObjectType.delete_proc =
      kernel_trampoline_group_.NewLongtermTrampoline(DeleteMutant);
  // init semaphore obj
  block->ExSemaphoreObjectType.pool_tag = 0x616D6553;
  block->ExSemaphoreObjectType.allocate_proc = trampoline_allocatepool;
  block->ExSemaphoreObjectType.free_proc = trampoline_freepool;
  // init timer obj
  block->ExTimerObjectType.pool_tag = 0x656D6954;
  block->ExTimerObjectType.allocate_proc = trampoline_allocatepool;
  block->ExTimerObjectType.free_proc = trampoline_freepool;
  block->ExTimerObjectType.delete_proc =
      kernel_trampoline_group_.NewLongtermTrampoline(DeleteTimer);
  // iocompletion object
  block->IoCompletionObjectType.pool_tag = 0x706D6F43;
  block->IoCompletionObjectType.allocate_proc = trampoline_allocatepool;
  block->IoCompletionObjectType.free_proc = trampoline_freepool;
  block->IoCompletionObjectType.delete_proc =
      kernel_trampoline_group_.NewLongtermTrampoline(DeleteIoCompletion);
  block->IoCompletionObjectType.unknown_size_or_object_ = oddobject_offset;

  // iodevice object
  block->IoDeviceObjectType.pool_tag = 0x69766544;
  block->IoDeviceObjectType.allocate_proc = trampoline_allocatepool;
  block->IoDeviceObjectType.free_proc = trampoline_freepool;
  block->IoDeviceObjectType.unknown_size_or_object_ = oddobject_offset;
  block->IoDeviceObjectType.unknown_proc =
      kernel_trampoline_group_.NewLongtermTrampoline(UnknownProcIoDevice);

  // file object
  block->IoFileObjectType.pool_tag = 0x656C6946;
  block->IoFileObjectType.allocate_proc = trampoline_allocatepool;
  block->IoFileObjectType.free_proc = trampoline_freepool;
  block->IoFileObjectType.unknown_size_or_object_ =
      0x38;  // sizeof fileobject, i believe
  block->IoFileObjectType.close_proc =
      kernel_trampoline_group_.NewLongtermTrampoline(CloseFileProc);
  block->IoFileObjectType.delete_proc =
      kernel_trampoline_group_.NewLongtermTrampoline(DeleteFileProc);
  block->IoFileObjectType.unknown_proc =
      kernel_trampoline_group_.NewLongtermTrampoline(UnknownFileProc);

  // directory object
  block->ObDirectoryObjectType.pool_tag = 0x65726944;
  block->ObDirectoryObjectType.allocate_proc = trampoline_allocatepool;
  block->ObDirectoryObjectType.free_proc = trampoline_freepool;
  block->ObDirectoryObjectType.unknown_size_or_object_ = oddobject_offset;

  // symlink object
  block->ObSymbolicLinkObjectType.pool_tag = 0x626D7953;
  block->ObSymbolicLinkObjectType.allocate_proc = trampoline_allocatepool;
  block->ObSymbolicLinkObjectType.free_proc = trampoline_freepool;
  block->ObSymbolicLinkObjectType.unknown_size_or_object_ = oddobject_offset;
  block->ObSymbolicLinkObjectType.delete_proc =
      kernel_trampoline_group_.NewLongtermTrampoline(DeleteSymlink);

#define offsetof32(s, m) static_cast<uint32_t>(offsetof(s, m))

  host_object_type_enum_to_guest_object_type_ptr_ = {
      {XObject::Type::Event,
       kernel_guest_globals_ +
           offsetof32(KernelGuestGlobals, ExEventObjectType)},
      {XObject::Type::Semaphore,
       kernel_guest_globals_ +
           offsetof32(KernelGuestGlobals, ExSemaphoreObjectType)},
      {XObject::Type::Thread,
       kernel_guest_globals_ +
           offsetof32(KernelGuestGlobals, ExThreadObjectType)},
      {XObject::Type::File,
       kernel_guest_globals_ +
           offsetof32(KernelGuestGlobals, IoFileObjectType)},
      {XObject::Type::Mutant,
       kernel_guest_globals_ +
           offsetof32(KernelGuestGlobals, ExMutantObjectType)},
      {XObject::Type::Device,
       kernel_guest_globals_ +
           offsetof32(KernelGuestGlobals, IoDeviceObjectType)}};
  xboxkrnl::xeKeSetEvent(&block->UsbdBootEnumerationDoneEvent, 1, 0);

  // Explicit virtual-kernel compatibility policy: healthy STFS and no pending
  // USB driver work start unsignaled. Use manual-reset notification events so
  // work/error state is not implicitly consumed by a wait. Retail reset modes
  // are not established; USB work production and STFS error query/clear remain
  // unsupported. These must never alias the signaled boot-ready/default event.
  for (auto* event :
       {&block->UsbdDriverLoadRequiredEvent, &block->StfsDeviceErrorEvent}) {
    event->header.type = EventNotificationObject;
    event->header.signal_state = 0;
    // Bind the ordinary host wrapper to this Kernel's borrowed global body.
    // No current guest thread/TLS or process-global initializer is required.
    auto object =
        XObject::GetNativeObject<XEvent>(this, event, EventNotificationObject);
    assert_not_null(object);
  }

  // Matches the console's boot value: CPUs 2-5 take background-scheduling
  // windows, and a title can move that with KeSetBackgroundProcessors.
  memory_->TranslateVirtual<KernelGuestGlobals*>(kernel_guest_globals_)
      ->background_processors = 0x3C;

  // Initialize timestamp bundle early to avoid race conditions with update
  // timer and ensure deterministic initial values at kernel boot time
  uint32_t pKeTimeStampBundle =
      memory_->SystemHeapAlloc(sizeof(X_TIME_STAMP_BUNDLE));
  X_TIME_STAMP_BUNDLE* lpKeTimeStampBundle =
      memory_->TranslateVirtual<X_TIME_STAMP_BUNDLE*>(pKeTimeStampBundle);

  xe::store_and_swap<uint64_t>(&lpKeTimeStampBundle->interrupt_time,
                               Clock::QueryGuestInterruptTime());
  xe::store_and_swap<uint64_t>(&lpKeTimeStampBundle->system_time,
                               Clock::QueryGuestSystemTime());
  xe::store_and_swap<uint32_t>(&lpKeTimeStampBundle->tick_count,
                               Clock::QueryGuestUptimeMillis());
  xe::store_and_swap<uint32_t>(&lpKeTimeStampBundle->padding, 0);

  ke_timestamp_bundle_ptr_ = pKeTimeStampBundle;

  // Start the update timer
  timestamp_timer_ = xe::threading::HighResolutionTimer::CreateRepeating(
      std::chrono::milliseconds(1),
      [this]() { this->UpdateKeTimestampBundle(); });
}

void KernelState::InitializeXbdmCpuCounters() {
  constexpr uint32_t counters_base_address = 0x91F00000;

  // These are not confirmed and there seems to be multiple types of counters,
  // but no idea how they're switched. For now this seems to be good enough.
  constexpr std::array<const char*, 0x11> xbdm_counters = {
      "load-hit-stores (S)",
      "instructions committed",
      "i-cache miss cycles",
      "core 0 L2 data misses",
      "core 0 L2 data misses",
      "core 0 L2 data misses",
      "core 0 L2 data misses",
      "core 0 L2 data misses",
      "core 0 L2 data misses",
      "core 0 L2 data misses",
      "core 0 L2 data misses",
      "core 0 L2 data misses",
      "core 0 L2 data misses",
      "core 0 L2 data misses",
      "core 0 L2 data misses",
      "core 0 L2 data misses",
      "Bad counter number - must be 0-15."};

  auto xbdm_range = memory_->LookupHeap(counters_base_address);
  if (!xbdm_range->AllocFixed(
          counters_base_address, 0x1000, 0,
          kMemoryAllocationCommit | kMemoryAllocationReserve,
          kMemoryProtectRead | kMemoryProtectWrite)) {
    return;
  }

  uint32_t address = counters_base_address;

  for (size_t i = 0; i < xbdm_counters.size(); i++) {
    xbdm_counters_address[i] = address;
    const std::string entry = xbdm_counters[i];
    std::memcpy(memory_->TranslateVirtual<char*>(address), entry.c_str(),
                entry.size());
    address += static_cast<uint32_t>(entry.size()) + 1;
  }
}

}  // namespace kernel
}  // namespace xe
