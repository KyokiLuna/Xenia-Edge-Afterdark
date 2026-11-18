/** Xenia : Xbox 360 Emulator Research Project; BSD license, see LICENSE. */
#pragma once
#include <cstdint>
#include <cstring>
#include <optional>
#include "xenia/base/byte_order.h"
#include "xenia/base/mutex.h"
#include "xenia/base/memory.h"
#include "xenia/cpu/ppc/ppc_context.h"
#include "xenia/kernel/guest_runtime_call_executor.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/native_import_domain.h"
#include "xenia/kernel/user_module.h"
#include "xenia/kernel/xthread.h"
#include "xenia/memory.h"
namespace xe::kernel {
class NativeProfileIdentityOwner;
// Exact read producer underlying original816B7EF0. This does NOT create a Task
// lifetime/reference, infer a generation, relabel origin or grant native entry.
// The concrete owner first validates the original7EF0/7F30/8098/code/import pins,
// retains its whole hook/root/domain/code lease, and then matches this borrowed
// pointer to its earlier B9A18 actual-output allocation receipt.
class NativeProfileIdentityTaskContext final {
 private:
  friend class NativeProfileIdentityOwner;
  static std::optional<uint32_t> Read(
      const GuestRuntimeCallExecutor& calls,const NativeImportDomain& domain,
      uint32_t exact_function,cpu::ppc::PPCContext& context) {
    auto* kernel=calls.kernel();
    auto* thread=XThread::GetCurrentFiberThread();
    if (!thread) { thread=XThread::GetCurrentThread(); }
    const auto root=calls.module();
    if (!kernel || !thread || !root || !domain.active() ||
        domain.root()!=NativeImportDomain::Member{root->hmodule_ptr(),calls.load_generation()} ||
        kernel->GetUserModuleForAddress(exact_function).get()!=root.get() ||
        thread->kernel_state()!=kernel ||
        thread->native_frame_thread_identity().process_type!=X_PROCTYPE_SYSTEM ||
        thread->thread_state()->context()!=&context || context.kernel_state!=kernel ||
        context.thread_state!=thread->thread_state() || context.processor!=kernel->processor() ||
        context.r[13]!=thread->pcr_ptr() || context.r[1]>UINT32_MAX || (context.r[1]&15)) { return {}; }
    auto word=[&](uint32_t address)->std::optional<uint32_t> {
      if (address<0x1000 || (address&3) || address>UINT32_MAX-3 ||
          kernel->memory()->QueryRangeAccess(address,4)==memory::PageAccess::kNoAccess) { return {}; }
      return xe::load_and_swap<uint32_t>(kernel->memory()->TranslateVirtual(address));
    };
    auto memory_read=global_critical_region::AcquireDirect();
    // Actual KeTlsGetValue154 import at81A72CE4 calls this same XThread reader.
    // Native7EF0 uses [TLS(slot81A82570)]+0 -> worker context, then +C -> Task.
    const auto slot=word(0x81A82570);uint32_t tls=0;
    if (!slot || !thread->GetTLSValue(*slot,&tls)) { return {}; }
    const auto worker=word(tls);
    if (!worker || *worker>UINT32_MAX-0xC) { return {}; }
    const auto task=word(*worker+0xC);
    if (!task || *task<0x1000 || (*task&3)) { return {}; }
    return task;
  }
};
}  // namespace xe::kernel
