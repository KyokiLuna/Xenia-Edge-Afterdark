/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_KERNEL_STATE_H_
#define XENIA_KERNEL_KERNEL_STATE_H_

#include <atomic>
#include <bitset>
#include <condition_variable>
#include <deque>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <system_error>
#include <vector>

#include "xenia/base/bit_map.h"
#include "xenia/base/physical_range_leases.h"
#include "xenia/cpu/backend/backend.h"
#include "xenia/cpu/export_resolver.h"
#include "xenia/kernel/guest_runtime.h"
#include "xenia/kernel/guest_scheduler.h"
#include "xenia/kernel/kernel.h"
#include "xenia/kernel/nui.h"
#include "xenia/kernel/util/kernel_fwd.h"
#include "xenia/kernel/util/native_list.h"
#include "xenia/kernel/util/object_table.h"
#include "xenia/kernel/util/xmp_volume_patch.h"
#include "xenia/kernel/xam/xdbf/spa_info.h"
#include "xenia/kernel/xconfig.h"
#include "xenia/kernel/xevent.h"
#include "xenia/vfs/virtual_file_system.h"

namespace xe {
class ByteStream;
class Emulator;
namespace cpu {
class Processor;
class GuestImportProvider;
}  // namespace cpu
namespace filesystem {
class SnapshotRecoveryOwner;
}  // namespace filesystem
namespace kernel {
class SystemManagementController;
class NativeImportDomain;
class NativeSystemAllocationAuthority;
class NativeNotificationPreferenceCoordinator;
class XStfsDevice;
struct NativeSystemPhysicalAllocationDecision;
namespace xam {
class AchievementManager;
class AppManager;
class ContentManager;
class XamState;
struct XCONTENT_AGGREGATE_DATA;
}  // namespace xam
}  // namespace kernel
}  // namespace xe

namespace xe {
namespace kernel {

constexpr fourcc_t kKernelSaveSignature = make_fourcc("KRNL");

static constexpr const uint16_t kBaseKernelBuildVersion = 1888;

// (?), used by KeGetCurrentProcessType
constexpr uint32_t X_PROCTYPE_IDLE = 0;
constexpr uint32_t X_PROCTYPE_TITLE = 1;
constexpr uint32_t X_PROCTYPE_SYSTEM = 2;

struct X_KPROCESS {
  X_KSPINLOCK thread_list_spinlock;
  // list of threads in this process, guarded by the spinlock above
  X_LIST_ENTRY thread_list;

  // quantum value assigned to each thread of the process
  xe::be<int32_t> quantum;
  // kernel sets this to point to a section of size 0x2F700 called CLRDATAA,
  // except it clears bit 31 of the pointer. in 17559 the address is 0x801C0000,
  // so it sets this ptr to 0x1C0000
  xe::be<uint32_t> clrdataa_masked_ptr;
  xe::be<uint32_t> thread_count;
  uint8_t process_priority_class;
  uint8_t default_thread_priority;
  uint8_t max_dynamic_priority;
  uint8_t disable_quantum_decay;
  xe::be<uint32_t> kernel_stack_size;
  xe::be<uint32_t> tls_static_data_address;
  xe::be<uint32_t> tls_data_size;
  xe::be<uint32_t> tls_raw_data_size;
  xe::be<uint16_t> tls_slot_size;
  // ExCreateThread calls a subfunc references this field, returns
  // X_STATUS_PROCESS_IS_TERMINATING if true
  uint8_t is_terminating;
  // one of X_PROCTYPE_
  uint8_t process_type;
  xe::be<uint32_t> tls_slot_bitmap[8];
  xe::be<uint32_t> unk_50;
  X_LIST_ENTRY unk_54;
  xe::be<uint32_t> unk_5C;
};
static_assert_size(X_KPROCESS, 0x60);

struct TerminateNotification {
  uint32_t guest_routine;
  uint32_t priority;
};

// structure for KeTimeStampBuindle
// a bit like the timers on KUSER_SHARED on normal win32
// https://www.geoffchappell.com/studies/windows/km/ntoskrnl/inc/api/ntexapi_x/kuser_shared_data/index.htm
struct X_TIME_STAMP_BUNDLE {
  // according to Nukernl these are BE
  uint64_t interrupt_time;
  // i assume system_time is in 100 ns intervals like on win32
  uint64_t system_time;
  uint32_t tick_count;
  uint32_t padding;
};

struct KernelGuestGlobals {
  X_OBJECT_TYPE ExThreadObjectType;
  X_OBJECT_TYPE ExEventObjectType;
  X_OBJECT_TYPE ExMutantObjectType;
  X_OBJECT_TYPE ExSemaphoreObjectType;
  X_OBJECT_TYPE ExTimerObjectType;
  X_OBJECT_TYPE IoCompletionObjectType;
  X_OBJECT_TYPE IoDeviceObjectType;
  X_OBJECT_TYPE IoFileObjectType;
  X_OBJECT_TYPE ObDirectoryObjectType;
  X_OBJECT_TYPE ObSymbolicLinkObjectType;
  // a constant buffer that some object types' "unknown_size_or_object" field
  // points to
  X_DISPATCH_HEADER XboxKernelDefaultObject;
  X_KPROCESS idle_process;    // X_PROCTYPE_IDLE. runs in interrupt contexts. is
                              // also the context the kernel starts in?
  X_KPROCESS title_process;   // X_PROCTYPE_TITLE
  X_KPROCESS system_process;  // X_PROCTYPE_SYSTEM. no idea when this runs. can
                              // create threads in this process with
                              // ExCreateThread and the thread flag 0x2

  // locks.
  X_KSPINLOCK dispatcher_lock;  // called the "dispatcher lock" in nt 3.5 ppc
                                // .dbg file. Used basically everywhere that
                                // DISPATCHER_HEADER'd objects appear
  // this lock is only used in some Ob functions. It's odd that it is used at
  // all, as each table already has its own spinlock.
  X_KSPINLOCK ob_lock;
  X_KSPINLOCK tls_lock;  // protects per-process TLS bitmap allocations

  // if LLE emulating Xam, this is needed or you get an immediate freeze
  X_KEVENT UsbdBootEnumerationDoneEvent;
  // CPUs that take a background-scheduling window, read by
  // KeQueryBackgroundProcessors and written by KeSetBackgroundProcessors.
  // 0x3C (CPUs 2-5) is the console's boot value.
  xe::be<uint32_t> background_processors;

  // Virtual-kernel work/error events, shared by SYSTEM and title clients.
  // Appended so existing guest-global offsets remain unchanged.
  X_KEVENT UsbdDriverLoadRequiredEvent;
  X_KEVENT StfsDeviceErrorEvent;
};
struct DPCImpersonationScope {
  uint8_t previous_irql_ = 0;
  uint8_t previous_process_type_ = 0;
  uint8_t previous_cpu_ = 0;
  uint8_t previous_dpc_active_ = 0;
  void Enter(X_KPCR& pcr, uint32_t process_type = UINT32_MAX);
  void Leave(X_KPCR& pcr) const;
  // The same context helper is used by CP dispatch and tests. Deferred work
  // retains the selected process/CPU until the final restoration.
  static void Run(X_KPCR& pcr, uint32_t process_type, uint8_t cpu,
                  const std::function<void(uint8_t)>& select_cpu,
                  const std::function<void()>& callback,
                  const std::function<void()>& deferred_dpcs);
};

struct KernelVersion {
  union {
    xe::be<uint64_t> value;

    struct {
      xe::be<uint16_t> major;
      xe::be<uint16_t> minor;
      xe::be<uint16_t> build;
      xe::be<uint16_t> qfe;
    };
  };

  KernelVersion(uint16_t build_ver = kBaseKernelBuildVersion) {
    major = 2;
    minor = 0;
    build = std::max(kBaseKernelBuildVersion, build_ver);
    qfe = 0;
  }
};

class KernelState {
 public:
  explicit KernelState(
      Emulator* emulator,
      std::shared_ptr<ConsoleSettingsStore> console_settings = {},
      std::error_code console_settings_error = {},
      std::shared_ptr<const filesystem::SnapshotRecoveryOwner> recovery_owner = {});
  ~KernelState();

  static KernelState* shared();

  Emulator* emulator() const { return emulator_; }
  Memory* memory() const { return memory_; }
  cpu::Processor* processor() const { return processor_; }
  vfs::VirtualFileSystem* file_system() const { return file_system_; }
  // Conservative native STFS body/context lifetime; never owns retired mounts
  // or file/source leases. Reclaimed only after actual Kernel CPU shutdown.
  bool KeepNativeStfsDevice(const object_ref<XStfsDevice>& device);

  uint32_t title_id() const;
  bool is_title_open() const;
  const std::unique_ptr<xam::SpaInfo> title_xdbf() const;
  const std::unique_ptr<xam::SpaInfo> module_xdbf(
      object_ref<UserModule> exec_module) const;

  xam::XamState* xam_state() const { return xam_state_.get(); }

  GuestScheduler* guest_scheduler() const { return guest_scheduler_.get(); }

  // Wake boost for a completed file request, NT's IO_DISK_INCREMENT.
  static constexpr uint32_t kIoDiskIncrement = 1;
  // Runs |fn|, a blocking host call such as a file read or open, on an I/O
  // worker while the calling fiber waits on an event its completion signals.
  // Off a fiber or under the global lock it runs inline. With |alertable| a
  // user APC ends one poll of the wait, which still lasts until |fn| is done.
  void RunBlockingIo(const std::function<void()>& fn,
                     GuestScheduler::BlockingCallClass call_class,
                     bool alertable = false);

  SystemManagementController* smc() const { return smc_.get(); }

  NUIController* nui() const { return nui_.get(); }

  xam::AchievementManager* achievement_manager() const;
  xam::AppManager* app_manager() const;
  xam::ContentManager* content_manager() const;

  XmpVolumePatch* xmp_volume_patch() const { return xmp_volume_patch_.get(); }
  void InitXmpVolumePatch();

  XConfig* xconfig() const { return xconfig_.get(); }
  const std::shared_ptr<const filesystem::SnapshotRecoveryOwner>&
  snapshot_recovery_owner() const { return snapshot_recovery_owner_; }
  const std::shared_ptr<NativeNotificationPreferenceCoordinator>&
  notification_preferences() const {
    return notification_preferences_;
  }

  std::bitset<4> GetConnectedUsers() const;

  // Access must be guarded by the global critical region.
  util::ObjectTable* object_table() { return &object_table_; }

  const KernelVersion* GetKernelVersion() const { return &kernel_version_; }

  uint32_t GetSystemProcess() const {
    return kernel_guest_globals_ + offsetof(KernelGuestGlobals, system_process);
  }

  uint32_t GetTitleProcess() const {
    return kernel_guest_globals_ + offsetof(KernelGuestGlobals, title_process);
  }
  // also the "interrupt" process
  uint32_t GetIdleProcess() const {
    return kernel_guest_globals_ + offsetof(KernelGuestGlobals, idle_process);
  }

  uint32_t AllocateTLS(cpu::ppc::PPCContext* context);
  void FreeTLS(cpu::ppc::PPCContext* context, uint32_t slot);

  void RegisterTitleTerminateNotification(uint32_t routine, uint32_t priority);
  void RemoveTitleTerminateNotification(uint32_t routine);

  void RegisterModule(XModule* module);
  void UnregisterModule(XModule* module);
  bool RegisterUserModule(object_ref<UserModule> module);
  void UnregisterUserModule(UserModule* module);
  bool IsKernelModule(const std::string_view name);
  bool IsModuleLoaded(const std::string_view name);
  object_ref<XModule> GetModule(const std::string_view name,
                                bool user_only = false);
  // Returns the loaded XEX module whose image contains the guest address.
  object_ref<UserModule> GetUserModuleForAddress(uint32_t address);
  // Atomically check loaded-module lifetime and publish callback admission
  // against UnloadUserModule's claim. Unclassified callbacks remain legacy.
  bool RegisterGraphicsInterruptCallback(uint32_t callback, uint32_t user_data,
                                         uint32_t process_type);

  object_ref<XThread> LaunchModule(object_ref<UserModule> module);
  object_ref<UserModule> GetExecutableModule();
  void SetExecutableModule(object_ref<UserModule> module);
  object_ref<UserModule> LoadUserModule(
      const std::string_view name, bool call_entry = true,
      bool system_app_module = false,
      std::shared_ptr<const cpu::GuestImportProvider> import_provider = {},
      std::shared_ptr<NativeImportDomain> import_domain = {});
  object_ref<UserModule> LoadUserModuleFromMemory(
      const std::string_view name, const void* addr, const size_t length,
      std::shared_ptr<NativeImportDomain> import_domain = {});
  struct NativeImportCaller {
    object_ref<UserModule> module;
    std::shared_ptr<NativeImportDomain> domain;
    bool scoped = false;
  };
  // Registration is explicit for the KRNL-only root; it does not redirect
  // unrelated callers or establish native task/subsystem readiness.
  std::shared_ptr<NativeImportDomain> CreateNativeImportDomain(
      const object_ref<UserModule>& root,
      std::shared_ptr<const cpu::GuestImportProvider> profile);
  bool RegisterNativeImportDomainMember(
      const std::shared_ptr<NativeImportDomain>& domain,
      const object_ref<UserModule>& module);
  NativeImportCaller ResolveNativeImportCaller(uint32_t return_address);
  bool RetireNativeImportDomain(
      const std::shared_ptr<NativeImportDomain>& domain);
  // Retired domains remain recorded: their native workers may still require
  // an explicit early stop/drain before providers disappear.
  bool HasNativeImportDomains();
  NativeSystemPhysicalAllocationDecision DecideNativeSystemPhysicalAllocation(
      uint32_t return_address);
  // Includes pre-domain partial startup and closed/retained tombstones.
  bool HasNativeSystemAllocationAuthorities();
  void CloseNativeSystemAllocationAdmission();
  bool RetireNativeSystemAllocationsAfterKernelStop(
      const std::function<bool(const memory::PhysicalAllocationOwner&)>& proof);
  object_ref<XModule> GetNativeImportModule(const NativeImportCaller& caller,
                                            std::string_view name);
  uint32_t GetNativeImportProcedure(const NativeImportCaller& caller,
                                    const object_ref<XModule>& module,
                                    uint16_t ordinal,
                                    std::string_view name = {});
  X_RESULT FinishLoadingUserModule(const object_ref<UserModule> module,
                                   bool call_entry = true,
                                   bool compile_discovered = true);
  X_STATUS UnloadUserModule(const object_ref<UserModule>& module,
                            bool call_entry = true);

  // Internal SysApp lifecycle. The caller must be a system-process XThread;
  // this never replaces the title's executable module or launch environment.
  X_HRESULT LoadGuestSystemApp(
      uint32_t app_id, std::string_view module_path, uint32_t user_index,
      uint32_t options_ptr = 0,
      std::shared_ptr<const cpu::GuestImportProvider> import_provider = {});
  X_HRESULT UnloadGuestSystemApp(uint32_t app_id);
  class UserModuleExecutionLease {
   public:
    uint32_t hmodule() const { return hmodule_; }
    UserModuleExecutionLease(const UserModuleExecutionLease&) = delete;
    UserModuleExecutionLease& operator=(const UserModuleExecutionLease&) =
        delete;
    UserModuleExecutionLease(UserModuleExecutionLease&&) = delete;
    UserModuleExecutionLease& operator=(UserModuleExecutionLease&&) = delete;

   private:
    friend class KernelState;
    UserModuleExecutionLease(KernelState* kernel, const UserModule* owner,
                             uint32_t hmodule)
        : kernel_(kernel), owner_(owner), hmodule_(hmodule) {}
    KernelState* const kernel_;
    const UserModule* const owner_;
    const uint32_t hmodule_;
  };
  X_HRESULT UnloadGuestSystemApp(
      uint32_t app_id,
      std::shared_ptr<UserModuleExecutionLease> execution_lease);
  // Protect direct native callbacks/objects against explicit module unload,
  // not merely object destruction. Admission is atomic with the unload claim.
  std::shared_ptr<UserModuleExecutionLease> AcquireUserModuleExecutionLease(
      const object_ref<UserModule>& module);
  // Guest-only resources with uncertain cleanup remain until guest threads
  // have stopped at full kernel teardown. Never pass host GPU handles here.
  void RetainNativeResourceUntilTeardown(std::shared_ptr<void> resource);
  // Explicit diagnostic until native HUD rendering/input is available.
  void StartNativeGuideBootstrapProbe(uint32_t user_index);
  void ShutdownNativeGuideBootstrapProbe();

  // Separately owned native-runtime readiness. Resource loading and the legacy
  // DLL attachment compatibility flag never create a Ready runtime. Native
  // system runtimes are resident until full kernel/memory teardown.
  std::shared_ptr<GuestRuntime> GetGuestRuntime(uint32_t hmodule);
  std::shared_ptr<GuestRuntime> CreateGuestRuntime(
      const object_ref<UserModule>& module);

  object_ref<KernelModule> GetKernelModule(const std::string_view name);
  template <typename T>
  object_ref<KernelModule> LoadKernelModule() {
    auto kernel_module = object_ref<KernelModule>(new T(emulator_, this));
    LoadKernelModule(kernel_module);
    return kernel_module;
  }
  template <typename T>
  object_ref<T> GetKernelModule(const std::string_view name) {
    auto module = GetKernelModule(name);
    return object_ref<T>(reinterpret_cast<T*>(module.release()));
  }

  X_RESULT ApplyTitleUpdate(const object_ref<UserModule> title_module);
  // Terminates a title: Unloads all modules, and kills all guest threads.
  // This DOES NOT RETURN if called from a guest thread!
  void TerminateTitle();

  // Handles a game-requested exit to the dashboard: hands off to the host UI
  // when it registered a handler, otherwise terminates the title.
  // This DOES NOT RETURN.
  void ExitToDashboard();
  // ExitToDashboard for a title that powers off, with the host UI's power off
  // handler.
  // This DOES NOT RETURN.
  void PowerOff();

  // Gracefully stops the dispatch thread. Call before force-terminating
  // threads to avoid corrupting the CV it's blocked on.
  void ShutdownDispatchThread();

  void RegisterThread(XThread* thread);
  void UnregisterThread(XThread* thread);
  void OnThreadExecute(XThread* thread);
  void OnThreadExit(XThread* thread);
  object_ref<XThread> GetThreadByID(uint32_t thread_id);
  std::vector<uint32_t> GetAllThreadIDs();

  void RegisterNotifyListener(XNotifyListener* listener);
  void UnregisterNotifyListener(XNotifyListener* listener);
  void BroadcastNotification(XNotificationID id, uint32_t data);
  // Native XAM already delivered to its own native listeners. Mirror this
  // actual SYS_UI event only into emulator TITLE listener objects.
  void BroadcastNativeSystemUiNotification(bool visible);

  util::NativeList* dpc_list() { return &dpc_list_; }

  void CompleteOverlapped(uint32_t overlapped_ptr, X_RESULT result);
  // The completion routine runs on thread_handle, the requester's thread.
  void CompleteOverlappedEx(uint32_t overlapped_ptr, X_RESULT result,
                            uint32_t extended_error, uint32_t length,
                            X_HANDLE thread_handle);

  void CompleteOverlappedImmediate(uint32_t overlapped_ptr, X_RESULT result);
  void CompleteOverlappedImmediateEx(uint32_t overlapped_ptr, X_RESULT result,
                                     uint32_t extended_error, uint32_t length);

  void CompleteOverlappedDeferred(
      std::function<void()> completion_callback, uint32_t overlapped_ptr,
      X_RESULT result, std::function<void()> pre_callback = nullptr,
      std::function<void()> post_callback = nullptr);
  void CompleteOverlappedDeferredEx(
      std::function<void()> completion_callback, uint32_t overlapped_ptr,
      X_RESULT result, uint32_t extended_error, uint32_t length,
      std::function<void()> pre_callback = nullptr,
      std::function<void()> post_callback = nullptr);

  void CompleteOverlappedDeferred(
      std::function<X_RESULT()> completion_callback, uint32_t overlapped_ptr,
      std::function<void()> pre_callback = nullptr,
      std::function<void()> post_callback = nullptr);
  void CompleteOverlappedDeferredEx(
      std::function<X_RESULT(uint32_t&, uint32_t&)> completion_callback,
      uint32_t overlapped_ptr, std::function<void()> pre_callback = nullptr,
      std::function<void()> post_callback = nullptr);

  bool Save(ByteStream* stream);
  bool Restore(ByteStream* stream);

  uint32_t notification_position_ = 2;
  XDeploymentType deployment_type_ = XDeploymentType::kOther;
  // Launch media mount path, titles can repoint GAME: and D: but not this.
  std::string title_mount_path_;

  // CPUs that take a background-scheduling window. Boots to 0x3C like the
  // console, and KeSetBackgroundProcessors moves it.
  uint32_t GetBackgroundProcessors();
  void SetBackgroundProcessors(uint32_t processors);

  uint32_t GetKeTimestampBundle();

  XE_NOINLINE
  XE_COLD
  uint32_t CreateKeTimestampBundle();
  void UpdateKeTimestampBundle();

  void BeginDPCImpersonation(cpu::ppc::PPCContext* context,
                             DPCImpersonationScope& scope);
  void EndDPCImpersonation(cpu::ppc::PPCContext* context,
                           DPCImpersonationScope& end_scope);

  // Queues a KDPC with |arg1| and |arg2| as its system arguments, for an expiry
  // that fires on a host thread: on its target processor's DPC thread if that
  // has started, else on the dispatch thread. A KDPC already queued there is
  // left as it is.
  void QueueDpc(uint32_t dpc_ptr, uint32_t arg1, uint32_t arg2);
  // Dequeues a KDPC QueueDpc queued if it hasn't started.
  bool RemoveDpc(uint32_t dpc_ptr);

  // Starts the scheduler thread that runs DPCs on guest processor |cpu|, if
  // the guest scheduler runs and it hasn't started. Called from guest code,
  // never from a host timer thread.
  void StartProcessorDpcThread(uint8_t cpu);
  // Queues a KDPC to run at DISPATCH_LEVEL on guest processor |cpu|, whose DPC
  // thread has started. The scheduler runs that thread on the processor ahead
  // of lower priority threads, standing in for the console's DPC interrupt.
  // Returns false, leaving its arguments, if the DPC is already queued.
  bool QueueProcessorDpc(uint8_t cpu, uint32_t dpc_ptr, uint32_t arg1,
                         uint32_t arg2);
  // Whether a KDPC waits in guest processor |cpu|'s DPC queue.
  bool IsProcessorDpcQueued(uint8_t cpu, uint32_t dpc_ptr);
  // Dequeues a KDPC QueueProcessorDpc queued if it hasn't run yet.
  bool RemoveProcessorDpc(uint32_t dpc_ptr);

  void EmulateCPInterruptDPC(uint32_t interrupt_callback,
                             uint32_t interrupt_callback_data, uint32_t source,
                             uint32_t cpu,
                             uint32_t process_type = X_PROCTYPE_TITLE);

  void InitializeXbdmCpuCounters();
  std::array<uint32_t, 0x11> xbdm_counters_address = {};

 private:
  void CompleteUserModuleUnload(const object_ref<UserModule>& module,
                                bool call_entry);
  void LoadKernelModule(object_ref<KernelModule> kernel_module);
  object_ref<XEvent> AcquireIoEvent();
  void ReleaseIoEvent(object_ref<XEvent> event);
  void InitializeProcess(X_KPROCESS* process, uint32_t type,
                         char priority_class, char default_priority,
                         char max_dynamic_priority);
  void SetProcessTLSVars(X_KPROCESS* process, int num_slots, int tls_data_size,
                         int tls_static_data_address);
  void InitializeKernelGuestGlobals();

  std::vector<xam::XCONTENT_AGGREGATE_DATA> FindTitleUpdate(
      const uint32_t title_id) const;
  const object_ref<UserModule> LoadTitleUpdate(
      const xam::XCONTENT_AGGREGATE_DATA* title_update,
      const object_ref<UserModule> module);
  bool IsPatchSignatureProper(const object_ref<UserModule> title_module,
                              const object_ref<UserModule> patch_module) const;

  X_RESULT ApplyTitleUpdate(const object_ref<UserModule> title_module,
                            const object_ref<UserModule> patch_module);

  Emulator* emulator_;
  Memory* memory_;
  cpu::Processor* processor_;
  vfs::VirtualFileSystem* file_system_;
  std::unique_ptr<xam::XamState> xam_state_;
  std::vector<object_ref<XStfsDevice>> native_stfs_devices_;
  object_ref<XHostThread> native_guide_probe_thread_;
  std::atomic<bool> native_guide_probe_stop_{false};
  std::atomic_flag system_app_operation_active_ = ATOMIC_FLAG_INIT;
  std::unique_ptr<GuestScheduler> guest_scheduler_;
  // Pooled completion events for RunBlockingIo so a request creates no kernel
  // object.
  std::mutex io_event_lock_;
  std::vector<object_ref<XEvent>> idle_io_events_;
  std::unique_ptr<SystemManagementController> smc_;
  std::unique_ptr<XmpVolumePatch> xmp_volume_patch_;
  std::unique_ptr<XConfig> xconfig_;
  std::shared_ptr<const filesystem::SnapshotRecoveryOwner> snapshot_recovery_owner_;
  // Console authority and native cache handover outlive the resident service.
  // No service/coordinator ownership cycle or second settings store.
  std::shared_ptr<NativeNotificationPreferenceCoordinator>
      notification_preferences_;
  std::unique_ptr<NUIController> nui_;

  KernelVersion kernel_version_;

  xe::global_critical_region global_critical_region_;

  // Must be guarded by the global critical region.
  util::ObjectTable object_table_;
  std::unordered_map<uint32_t, XThread*> threads_by_id_;
  std::vector<object_ref<XNotifyListener>> notify_listeners_;
  bool has_notified_startup_ = false;
  bool has_notified_live_startup_ = false;
  bool has_notified_xmp_startup_ = false;

  object_ref<UserModule> executable_module_;
  std::vector<object_ref<KernelModule>> kernel_modules_;
  std::vector<object_ref<UserModule>> user_modules_;
  std::unordered_map<uint32_t, std::shared_ptr<GuestRuntime>> guest_runtimes_;
  std::unordered_map<uint64_t, std::shared_ptr<NativeImportDomain>>
      native_import_domains_;
  uint64_t next_native_import_domain_ = 1;
  std::vector<std::shared_ptr<NativeSystemAllocationAuthority>>
      native_system_allocation_authorities_;
  std::vector<std::shared_ptr<void>> resident_native_resources_;
  std::unordered_map<uint32_t, std::shared_ptr<void>> native_system_app_leases_;
  uint64_t next_guest_runtime_generation_ = 1;
  std::vector<TerminateNotification> terminate_notifications_;
  uint32_t kernel_guest_globals_ = 0;

  std::atomic<bool> dispatch_thread_running_;
  object_ref<XHostThread> dispatch_thread_;
  // Must be guarded by the global critical region.
  util::NativeList dpc_list_;
  // Guards the dispatch queue, which a timer expiry pushes to while a timer
  // cancel holding the global lock can be waiting for it.
  std::mutex dispatch_mutex_;
  std::condition_variable dispatch_cond_;
  std::list<std::function<void()>> dispatch_queue_;
  // KDPCs queued on the dispatch thread that haven't started.
  std::vector<uint32_t> dispatch_dpcs_;

  // DPCs queued to each guest processor and the thread that runs them there.
  struct ProcessorDpcs {
    std::mutex lock;
    std::deque<uint32_t> queue;
    object_ref<XEvent> event;
    object_ref<XHostThread> thread;
    std::atomic<bool> started{false};
  };
  ProcessorDpcs processor_dpcs_[6];
  void RunProcessorDpcs(uint8_t cpu);

  uint32_t ke_timestamp_bundle_ptr_ = 0;
  std::unique_ptr<xe::threading::HighResolutionTimer> timestamp_timer_;
  uint32_t quantum_timer_counter_ = 0;
  cpu::backend::GuestTrampolineGroup kernel_trampoline_group_;
  // fixed address referenced by dashboards. Data is currently unknown
  uint32_t strange_hardcoded_page_ = 0x8E038634 & (~0xFFFF);
  uint32_t strange_hardcoded_location_ = 0x8E038634;

  friend class XObject;
  friend class NativeSystemAllocationAuthority;

 public:
  uint32_t dash_context_ = 0;
  std::unordered_map<XObject::Type, uint32_t>
      host_object_type_enum_to_guest_object_type_ptr_;
  uint32_t GetKernelGuestGlobals() const { return kernel_guest_globals_; }
};

}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_KERNEL_STATE_H_
