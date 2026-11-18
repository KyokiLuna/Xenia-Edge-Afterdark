/** Xenia : Xbox 360 Emulator Research Project; BSD license, see LICENSE. */
#ifndef XENIA_KERNEL_NATIVE_MEDIA_COUNTED_OBJECT_H_
#define XENIA_KERNEL_NATIVE_MEDIA_COUNTED_OBJECT_H_
#include <atomic>
#include <cstdint>
#include <memory>
#include "xenia/kernel/xobject.h"
#include "xenia/kernel/native_media_object_admission.h"
namespace xe::kernel {
class NativeMediaService;
class XThread;
namespace xboxkrnl { class NativeMediaCountedBodyAccess; }
// Actual ObCreateObject result, prebound to the original native24E invocation.
// The receipt never releases a guest reference from its destructor.
class NativeMediaObjectCreation final {
 public:
  ~NativeMediaObjectCreation();
  void Complete(uint32_t status,uint32_t actual_body) noexcept;
 private:
  friend class NativeMediaService;
  struct State;
  explicit NativeMediaObjectCreation(std::unique_ptr<State>);
  std::unique_ptr<State> state_;
};
// One actual OpenCounted result plus an extra REAL ReferenceCounted reference,
// acquired in the existing Kernel primitive. A wrapper ref is not substituted
// for pointer_count. Source identity/creator/allocation proof is prepared before
// the primitive's GCRC; it is neither looked up nor rebuilt from guest tags.
class NativeMediaObjectInsertion final {
 public:
  ~NativeMediaObjectInsertion();
  // Called after the actual OpenCounted, before its output DWORD delivery.
  // Exact table/header/mapping checks and metadata only under GCRC; no PPC.
  bool AdoptActual(XObject&,uint32_t actual_handle,bool actual_new_wrapper) noexcept;
  // Only explicit normally-returned pre-mutation failure branches in the
  // genuine OpenCounted call report this. Missing object/output is not proof.
  bool ObserveNoInsertionActual(uint32_t actual_body,uint32_t failed_status) noexcept;
 private:
  friend class NativeMediaService;
  friend class NativeMediaObjectFree;
  friend class xboxkrnl::NativeMediaCountedBodyAccess;
  NativeMediaObjectInsertion(std::weak_ptr<NativeMediaService>,KernelState*,
      uint64_t creation,uint32_t body,uint32_t bytes,
      std::shared_ptr<NativeMediaObjectAdmission>,const XThread* insertion_thread,
      const void* insertion_context,uint32_t insertion_sp,uint32_t insertion_lr);
  std::weak_ptr<NativeMediaService> owner_;
  KernelState* kernel_=nullptr;
  const uint64_t creation_;
  const uint32_t body_,bytes_;
  const std::shared_ptr<NativeMediaObjectAdmission> admission_;
  const XThread* const insertion_thread_;
  const void* const insertion_context_;
  const uint32_t insertion_sp_,insertion_lr_;
  enum class ActualOutcome : uint8_t { kPending, kNeverInserted, kInserted };
  std::atomic<ActualOutcome> actual_outcome_{ActualOutcome::kPending};
  uint32_t actual_refusal_status_=0;
  bool NeverInserted() const noexcept {
    return actual_outcome_.load(std::memory_order_acquire)==ActualOutcome::kNeverInserted;
  }
  object_ref<XObject> actual_object_;
  uint32_t actual_handle_=0;
  bool actual_reference_held_=false;
  bool actual_insert_observed_=false;
  bool actual_new_wrapper_=false;
  bool stopped_retired_=false;
};
// A real last pointer-count free attempt. Exact old wrapper mapping is removed
// BEFORE the actual allocator can reuse the address. Its real executed/normal
// result is observed separately from Close and from any stop request.
class NativeMediaObjectFree final {
 public:
  ~NativeMediaObjectFree();
  void BeforeActualFree() noexcept;
  void Complete(bool actually_executed) noexcept;
 private:
  friend class NativeMediaService;
  struct State;
  explicit NativeMediaObjectFree(std::unique_ptr<State>);
  std::unique_ptr<State> state_;
};
namespace xboxkrnl {
class NativeMediaCountedBodyAccess final {
 private:
  friend class xe::kernel::NativeMediaService;
  friend class xe::kernel::NativeMediaObjectInsertion;
  friend class xe::kernel::NativeMediaObjectFree;
  static bool Adopt(NativeMediaObjectInsertion&,XObject&,uint32_t handle,bool actual_new_wrapper) noexcept;
  // Only the owning service's genuine current SYSTEM cleanup Task may release
  // its exact acquired reference. This executes existing DereferenceCounted
  // outside GCRC; real close/free callbacks remain native.
  static bool Release(NativeMediaObjectInsertion&,XThread&) noexcept;
  // Drop only this actual extra pointer reference; ordinary live handles and
  // their native close behavior remain untouched for never-selected objects.
  static bool RelinquishOrdinary(NativeMediaObjectInsertion&,XThread&) noexcept;
  static bool CloseActualHandles(NativeMediaObjectInsertion&,XThread&) noexcept;
  static bool MatchesHandle(const NativeMediaObjectInsertion&,uint32_t) noexcept;
  static bool BindSelected(NativeMediaObjectInsertion&) noexcept;
  static bool ProvisionalHasNoGuestOwner(const NativeMediaObjectInsertion&) noexcept;
  static bool HasActualHandles(const NativeMediaObjectInsertion&) noexcept;
  static bool RetireAfterStoppedKernel(NativeMediaObjectInsertion&) noexcept;
  // Actual body-free or verified stopped-Kernel boundary only; never a stop
  // request, guessed address or destructor-time guest callback.
  static bool WithdrawExactWrapper(NativeMediaObjectInsertion&) noexcept;
  static bool DetachKernelAfterBodyGone(NativeMediaObjectInsertion&) noexcept;
};
} // namespace xboxkrnl
} // namespace xe::kernel
#endif
