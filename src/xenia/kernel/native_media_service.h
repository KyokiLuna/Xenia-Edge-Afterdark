/** Xenia : Xbox 360 Emulator Research Project; BSD license, see LICENSE. */
#ifndef XENIA_KERNEL_NATIVE_MEDIA_SERVICE_H_
#define XENIA_KERNEL_NATIVE_MEDIA_SERVICE_H_
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include "xenia/kernel/guest_runtime.h"
#include "xenia/xbox.h"
#include "xenia/cpu/ppc/ppc_context.h"
namespace xe { class Memory; class NativeAvatarBackingLease; namespace vfs { class File; } }
namespace xe::kernel {
namespace util { class GuestIoNativeMediaFactoryScope; }
class KernelState;
class XThread;
class XObject;
class XFile;
template <typename T> class object_ref;
namespace test { class NativeMediaDirectTestAccess; }
class NativeImportDomain;
class NativeSystemAllocationAuthority;
class NativeSystemUiStartupCatalog;
class NativeSystemUiService;
class NativeSystemTaskContinuation;
class NativeMediaObjectCreation;
class NativeMediaObjectInsertion;
class NativeMediaObjectFree;
namespace xboxkrnl { class NativeMediaCountedBodyAccess; }
// One existing SYSTEM service owns the original FA/USB/cursor protocol. Native
// objects, headers, query rows, sync/async dispatch and codecs stay native.
// Source publication/processing, actual attachment/token creation, query init,
// handle admission, read completion, close and body free are separate facts.
class NativeMediaService final : public std::enable_shared_from_this<NativeMediaService> {
 public:
  ~NativeMediaService();
  // Null is unrelated; a selected exact family with an unavailable origin
  // returns a truthful error rather than manufacturing an empty host enum.
  static std::optional<uint32_t> TryEnumerationOnOriginalCaller(
      KernelState*,uint16_t ordinal,std::span<const uint32_t>) noexcept;
  static std::optional<uint32_t> TryMessageOnOriginalCaller(
      KernelState*,uint16_t ordinal,std::span<const uint32_t>) noexcept;
  // Authenticated fixed entry/own-return observations only. No fake PCR,
  // process, native Task, attachment counter, slot or service initialization.
  static constexpr bool HooksFunction(uint32_t address) {
    switch(address) {
      case 0x816F0F78: case 0x816F1040: case 0x816F1070:
      case 0x816F1200: case 0x816A2F88: case 0x816A2CF0: case 0x816A2FF8:
      case 0x81706478:
      case 0x8187AE98: case 0x818621F0: return true;
      default: return false;
    }
  }
  static uint64_t GuardEntry(void*,uint64_t) noexcept;
  static void ObserveReturn(void*,uint64_t) noexcept;
  static void BeforeCompletionPublication(void*) noexcept;
  // Receipts wrap actual existing object-manager calls; ordinary and all
  // other native CountedBody roles are untouched. Destructors execute no PPC.
  static std::unique_ptr<NativeMediaObjectCreation> CaptureObjectCreation(
      KernelState*,const cpu::ppc::PPCContext*,uint32_t type,uint32_t size,
      uint32_t output) noexcept;
  static std::shared_ptr<NativeMediaObjectInsertion> CaptureObjectInsertion(
      KernelState*,const cpu::ppc::PPCContext*,uint32_t body,uint32_t output) noexcept;
  static std::unique_ptr<NativeMediaObjectFree> CaptureObjectFree(
      KernelState*,const cpu::ppc::PPCContext*,uint32_t body,uint32_t type,
      uint32_t allocation,uint32_t free_procedure) noexcept;
  // Existing actual IO producer, before its new guest handle is published.
  // An unrelated open is unchanged. A claimed direct C open must retain the
  // exact previously admitted selected Device; no post-lookup adoption.
  static X_STATUS ValidateDirectFileOpen(KernelState*,const vfs::File&,
                                         uint32_t guest_output) noexcept;
  static void ObserveDirectFileOpen(KernelState*,const object_ref<XFile>&,
                                    uint32_t guest_output) noexcept;
  void BeginStop() noexcept;
  // Actual callbacks/entered calls/tasks drained. Unavailable-backend body
  // resources may remain for verified stopped-Kernel rundown; this is NOT a
  // successful native Close/free/readiness receipt.
  bool Drained() const noexcept;
 private:
  friend class NativeSystemUiService;
  friend class test::NativeMediaDirectTestAccess;
  friend class util::GuestIoNativeMediaFactoryScope;
  friend class NativeSystemTaskContinuation;
  friend class NativeMediaObjectCreation;
  friend class NativeMediaObjectInsertion;
  friend class NativeMediaObjectFree;
  friend class xboxkrnl::NativeMediaCountedBodyAccess;
  struct Config {
    std::shared_ptr<const NativeSystemUiStartupCatalog> catalog;
    std::shared_ptr<NativeImportDomain> domain;
    std::shared_ptr<NativeSystemAllocationAuthority> allocation;
  };
  static std::shared_ptr<NativeMediaService> CreateBeforeWorkers(Config,std::string*);
  static std::shared_ptr<NativeMediaService> Lookup(KernelState*);
  bool BindContinuation(std::shared_ptr<NativeSystemTaskContinuation>);
  bool ObserveFeReady(XThread&,std::string*);
  bool OwnsScheduledWork(uint64_t serial) const noexcept;
  bool FactoryPublicationCurrent(uint64_t serial,uint32_t handle,const Memory&) const noexcept;
  void FactoryPublicationCommitted(uint64_t serial,uint32_t value) noexcept;
  static std::shared_ptr<const NativeAvatarBackingLease> BorrowDirectBacking(
      Memory&,uint32_t message,uint32_t payload,std::span<const uint32_t>);
  bool StepOnBootstrap(XThread&) noexcept;
  bool StopOnBootstrap(XThread&) noexcept;
  // Same service's actual joined CPU/Kernel stop boundary, while Memory and
  // table still exist. Never selected by a stop-request bool or destructor.
  bool RetireAfterActualKernelStop() noexcept;
  struct State;
  explicit NativeMediaService(std::shared_ptr<State>);
  std::shared_ptr<State> state_;
};
} // namespace xe::kernel
#endif
