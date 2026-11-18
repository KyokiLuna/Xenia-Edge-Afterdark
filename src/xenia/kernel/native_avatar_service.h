/** Xenia : Xbox 360 Emulator Research Project; BSD license, see LICENSE. */
#ifndef XENIA_KERNEL_NATIVE_AVATAR_SERVICE_H_
#define XENIA_KERNEL_NATIVE_AVATAR_SERVICE_H_
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include "xenia/cpu/ppc/ppc_context.h"
#include "xenia/kernel/guest_runtime.h"
#include "xenia/kernel/native_system_title_heap_data.h"
#include "xenia/kernel/native_system_ui_public_calls.h"
#include "xenia/kernel/xam/content_source.h"
namespace xe::kernel {
class GuestRuntimeCallExecutor;
class KernelState;
class NativeImportDomain;
class NativeProfileIdentityOwner;
class NativeSystemAllocationAuthority;
class NativeSystemContent;
class NativeSystemUiService;
class NativeSystemUiStartupRuntime;
class XThread;
namespace xam { class ProfileSettingsReadProvider; }
namespace test { class NativeAvatarServiceTestAccess; }
// The native 17559 engine owns geometry, allocators, jobs and callback ABI.
// This owner retains the actual caller/source/backing through its native
// protocol. One F2 engine per Kernel/root, with one exclusive title lease;
// resident F3 is independent. It never creates a Task/TLS or a renderer.
class NativeAvatarService final
    : public std::enable_shared_from_this<NativeAvatarService> {
 public:
  ~NativeAvatarService();
  // Inactive means no explicit owned avatar family. Once installed, a denied
  // or failed call cannot fall back to a fabricated HLE avatar result.
  static NativeSystemUiPublicCallResult TryInvokeOnOriginalCaller(
      KernelState*, NativeSystemUiPublicCall, std::span<const uint64_t>) noexcept;
  // Fixed native-entry guard, used before ANY original PPC instruction, and
  // paired own normal-return observation. Default/absent plans are unchanged.
  // A false result has already supplied a truthful ABI error in guest r3.
  static uint64_t GuardEntry(void*, uint64_t address) noexcept;
  static void ObserveReturn(void*, uint64_t address) noexcept;
  // Fixed HIR builtin immediately before the source-pinned original Status
  // store. Retires caller backing only; no status-derived readiness or APC
  // delivery claim and no release of native job/Task/module drain ownership.
  static void BeforeCompletionPublication(void*) noexcept;
  static constexpr bool HooksFunction(uint32_t at) {
    switch (at) {
      case 0x816755E8: case 0x81674940: case 0x81674A70:
      case 0x81674AD8: case 0x81675258: case 0x81674950:
      case 0x81674DE0: case 0x816752F0: case 0x81675358:
      case 0x81674D48: case 0x81964E70: case 0x81968390: case 0x81977310:
      case 0x816A2CF0: case 0x816A2A48: case 0x816A2C80:
      case 0x816A2588: case 0x819657E0: case 0x819659D0:
      case 0x81965AC0: case 0x81981740: case 0x81981560:
      case 0x819652F8: case 0x81965040: case 0x819650F8:
      case 0x816BD7A0: case 0x816BCC38: case 0x816BCD08:
      case 0x816BCF60: case 0x816BD6A0: case 0x8196B5F0:
      case 0x8197BEB0: case 0x816BD620: case 0x816BD4D8:
      case 0x816BD560: case 0x816BA440:
      case 0x816BDE90: case 0x816BD270: case 0x81982088: return true;
      default: return false;
    }
  }
  // Common content bridge joins only the real admitted F2 package-open call.
  // A null optional leaves unrelated native/HLE content behavior unchanged.
  static std::optional<uint32_t> TryOpenAssetPack(
      KernelState*, uint32_t user, std::string_view root,
      std::string_view package, uint32_t flags, uint32_t unknown,
      uint32_t license_output, uint32_t overlap) noexcept;
  static std::optional<uint32_t> TryCloseAssetPack(
      KernelState*, std::string_view root, uint32_t overlap) noexcept;
  void BeginStop() noexcept;
  // External host observer only; native close must have run while the actual
  // SYSTEM bootstrap, scheduler, callbacks and providers were still healthy.
  bool Drained() const noexcept;

 private:
  friend class GuestRuntimeCallExecutor;
  friend class NativeSystemUiService;
  // Advisory scalar receipts (including first uncertainty) only, at verified
  // Service host phases outside guest fibers and GCRC. No lifetime lookup.
  void ReportDiagnosticsOnHost(const char* boundary) const noexcept;
  friend class NativeProfileIdentityOwner;
  friend class NativeSystemUiStartupRuntime;
  friend class test::NativeAvatarServiceTestAccess;
  // Opaque source-observed creation admission for original load/unload Control
  // or Init Request Start→Schedule→B9A18. The legacy type name also covers
  // creation; it carries no profile/source/read authority.
  // Capture may validate owners; Matches is pure captured scalar metadata and
  // can be rechecked under identity's mutex. Destroy holders outside locks.
  class CleanupTaskCreationProof final {
   public:
    bool Matches(const cpu::ppc::PPCContext&) const noexcept;
    bool IsUnload() const noexcept;
   private:
    friend class NativeAvatarService;
    friend class NativeProfileIdentityOwner;
    // Only the primary's factual stopped-retirement lane, never native proof.
    // Revoke resource fields in place so escaped metadata aliases are inert.
    void RetireAfterActualKernelStop() const noexcept;
    struct Data;
    explicit CleanupTaskCreationProof(std::shared_ptr<Data>);
    std::shared_ptr<Data> data_;
  };
  static std::shared_ptr<const CleanupTaskCreationProof>
      CaptureOwnedTaskAllocation(KernelState*,const cpu::ppc::PPCContext&) noexcept;
  struct Config {
    KernelState* kernel = nullptr;
    std::shared_ptr<GuestRuntimeCallExecutor> calls;
    std::shared_ptr<NativeImportDomain> domain;
    std::shared_ptr<NativeSystemAllocationAuthority> allocation;
    std::shared_ptr<const NativeSystemContent> content;
    std::shared_ptr<const xam::ProfileSettingsReadProvider> profile_reader;
    NativeSystemPreparedTitle title;
  };
  static std::shared_ptr<NativeAvatarService> Create(Config, std::string*);
  // The existing owned frontend Task retains its genuine reference throughout
  // this synchronous FE scope. Native entry/own-return observations prove the
  // resident builtin lifecycle; none of these methods supplies asset authority.
  bool PrepareFrontendResident(XThread&, uint32_t owned_task, uint32_t argument,
                               std::string*);
  bool ObserveFrontendResident(XThread&, uint32_t owned_task,
                               const GuestRuntime::ExecutionResult&,
                               std::string*);
  // Existing deferred plan roles adopt those once-only native receipts AFTER
  // the actual queued lookup/open proves the package source. No setup/init PPC.
  bool AdoptStartup(std::string_view identity, XThread&, std::string*);
  // Explicit failure path, still on the same healthy frontend Task and before
  // allocation admission closes. Real original unload or truthful retention.
  bool AbortFrontendResident(XThread&, uint32_t owned_task) noexcept;
  // After BeginStop and BEFORE this same actual SYSTEM bootstrap can return.
  // Original Shutdown schedules/waits its own real native close Task. There is
  // no destructor PPC or arbitrary reuse of startup/diagnostic continuations.
  bool ShutdownOnBootstrap(XThread&) noexcept;
  // Only an ALREADY strictly Drained owner, at Service's actual CPU/IO/GPU
  // boundary. Revoke native holders even if host metadata escaped; no PPC or
  // uncertain/session/request abandonment, Close/free or fake shutdown fact.
  bool RetireAfterActualKernelStop() noexcept;
  static bool OwnsOriginalCall(GuestRuntimeCallExecutor*, XThread&,
                               NativeSystemUiPublicCall,
                               std::span<const uint64_t>);
  static std::shared_ptr<NativeAvatarService> Lookup(KernelState*);
  struct State;
  explicit NativeAvatarService(std::shared_ptr<State>);
  std::shared_ptr<State> state_;
};
}  // namespace xe::kernel
#endif
