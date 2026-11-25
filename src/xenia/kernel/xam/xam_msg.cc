/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <array>

#include "xenia/base/logging.h"
#include "xenia/base/mutex.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/native_media_service.h"
#include "xenia/kernel/native_managed_format_frontdoor.h"
#include "xenia/kernel/user_module.h"
#include "xenia/kernel/util/guest_io_buffer.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xam/app_manager.h"
#include "xenia/kernel/xam/xam.h"
#include "xenia/kernel/xam/xam_private.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_error.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_threading.h"
#include "xenia/kernel/xevent.h"
#include "xenia/kernel/xthread.h"
#include "xenia/memory.h"
#include "xenia/xbox.h"

namespace xe {
namespace kernel {
namespace xam {

dword_result_t XamRegisterSysApp_entry(dword_t hmodule, dword_t app,
                                       dword_t callback,
                                       dword_t resource_slot) {
  // Only loaded guest code can own a callback here. This host validation is
  // stricter than the console's unchecked pointer store, and prevents a bad
  // registration from jumping into unmapped memory later.
  auto module = kernel_state()->GetUserModuleForAddress(callback);
  if (!callback || !module || module->hmodule_ptr() != hmodule) {
    return X_ERROR_FUNCTION_FAILED;
  }
  return kernel_state()->app_manager()->RegisterGuestApp(
      kernel_state(), module, app, callback, resource_slot);
}
DECLARE_XAM_EXPORT2(XamRegisterSysApp, kNone, kImplemented, kSketchy);

dword_result_t XamUnregisterSysApp_entry(dword_t hmodule, dword_t app,
                                         dword_t resource_slot) {
  return kernel_state()->app_manager()->guest_apps().Unregister(hmodule, app,
                                                                resource_slot);
}
DECLARE_XAM_EXPORT2(XamUnregisterSysApp, kNone, kImplemented, kSketchy);

dword_result_t XMsgInProcessCall_entry(dword_t app, dword_t message,
                                       dword_t arg1, dword_t arg2) {
  if(const auto media=NativeMediaService::TryMessageOnOriginalCaller(kernel_state(),0x1F4,
      std::array<uint32_t,4>{app,message,arg1,arg2}))return *media;
  auto result = kernel_state()->app_manager()->DispatchMessageSync(app, message,
                                                                   arg1, arg2);
  if (result == X_ERROR_NOT_FOUND) {
    XELOGE("XMsgInProcessCall: app {:08X} undefined",
           static_cast<uint32_t>(app));
  }
  return result;
}
DECLARE_XAM_EXPORT1(XMsgInProcessCall, kNone, kImplemented);

dword_result_t XMsgSystemProcessCall_entry(dword_t app, dword_t message,
                                           dword_t buffer,
                                           dword_t buffer_length) {
  if(const auto media=NativeMediaService::TryMessageOnOriginalCaller(kernel_state(),0x1F6,
      std::array<uint32_t,4>{app,message,buffer,buffer_length}))return *media;
  auto result = kernel_state()->app_manager()->DispatchMessageAsync(
      app, message, buffer, buffer_length);
  if (result == X_ERROR_NOT_FOUND) {
    XELOGE("XMsgSystemProcessCall: app {:08X} undefined",
           static_cast<uint32_t>(app));
  }
  return result;
}
DECLARE_XAM_EXPORT1(XMsgSystemProcessCall, kNone, kImplemented);

X_HRESULT xeXMsgStartIORequestEx(uint32_t app, uint32_t message,
                                 uint32_t overlapped_ptr, uint32_t buffer_ptr,
                                 uint32_t buffer_length,
                                 X_TASK_ARGS* xam_task_args_ptr) {
  auto result = kernel_state()->app_manager()->DispatchMessageAsync(
      app, message, buffer_ptr, buffer_length, overlapped_ptr);
  if (result == X_E_NOTFOUND) {
    XELOGE("XMsgStartIORequestEx: app {:08X} undefined", app);
    result = X_E_INVALIDARG;
    XThread::SetLastError(X_ERROR_NOT_FOUND);
    if (overlapped_ptr) {
      kernel_state()->CompleteOverlappedImmediate(overlapped_ptr, result);
      result = X_ERROR_IO_PENDING;
    }
  }
  if (result == X_ERROR_SUCCESS || result == X_ERROR_IO_PENDING) {
    XThread::SetLastError(0);
  }
  return result;
}

dword_result_t XMsgStartIORequestEx_entry(
    dword_t app, dword_t message, pointer_t<XAM_OVERLAPPED> overlapped_ptr,
    dword_t buffer_ptr, dword_t buffer_length,
    pointer_t<X_TASK_ARGS> xam_task_args_ptr) {
  if(const auto media=NativeMediaService::TryMessageOnOriginalCaller(kernel_state(),0x1FC,
      std::array<uint32_t,6>{app,message,overlapped_ptr.guest_address(),buffer_ptr,buffer_length,
                            xam_task_args_ptr.guest_address()}))return *media;
  if (const auto managed = NativeManagedFormatFrontdoor::TryStart(
          kernel_state(),
          std::array<uint64_t, 6>{app, message, overlapped_ptr.guest_address(),
                                  buffer_ptr, buffer_length,
                                  xam_task_args_ptr.guest_address()})) {
    return uint32_t(managed->return_value);
  }
  return xeXMsgStartIORequestEx(app, message, overlapped_ptr, buffer_ptr,
                                buffer_length, xam_task_args_ptr);
}
DECLARE_XAM_EXPORT1(XMsgStartIORequestEx, kNone, kImplemented);

dword_result_t XMsgStartIORequest_entry(
    dword_t app, dword_t message, pointer_t<XAM_OVERLAPPED> overlapped_ptr,
    dword_t buffer_ptr, dword_t buffer_length) {
  if(const auto media=NativeMediaService::TryMessageOnOriginalCaller(kernel_state(),0x1F7,
      std::array<uint32_t,5>{app,message,overlapped_ptr.guest_address(),buffer_ptr,buffer_length}))return *media;
  return xeXMsgStartIORequestEx(app, message, overlapped_ptr, buffer_ptr,
                                buffer_length, nullptr);
}
DECLARE_XAM_EXPORT1(XMsgStartIORequest, kNone, kImplemented);

dword_result_t XMsgCancelIORequest_entry(
    pointer_t<XAM_OVERLAPPED> overlapped_ptr, dword_t wait) {
  if (const auto managed = NativeManagedFormatFrontdoor::TryCancel(
          kernel_state(),
          std::array<uint64_t, 2>{overlapped_ptr.guest_address(), wait})) {
    return uint32_t(managed->return_value);
  }
  X_HANDLE event_handle = XOverlappedGetEvent(overlapped_ptr);
  if (event_handle && wait) {
    auto ev =
        kernel_state()->object_table()->LookupObject<XEvent>(event_handle);
    if (ev) {
      ev->Wait(0, 0, true, nullptr);
    }
  }

  return 0;
}
DECLARE_XAM_EXPORT1(XMsgCancelIORequest, kNone, kImplemented);

dword_result_t XMsgCompleteIORequest_entry(
    pointer_t<XAM_OVERLAPPED> overlapped_ptr, dword_t result,
    dword_t extended_error, dword_t length) {
  kernel_state()->CompleteOverlappedImmediateEx(overlapped_ptr, result,
                                                extended_error, length);
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT2(XMsgCompleteIORequest, kNone, kImplemented, kSketchy);

// Shared with the Win32 query below; use its existing checked caller-PCR
// error behavior rather than XThread's unrelated current-TLS error slot.
dword_result_t WaitForSingleObject_entry(dword_t handle, dword_t milliseconds,
                                        const ppc_context_t& context);
qword_result_t RtlGetLastError_entry(const ppc_context_t& context);

namespace {
enum class OverlappedWaitApi { kNt, kWin32 };

// Native queries access only these DWORDs, in this order. Ordinary virtual
// scalars also work with inherited GCRC; physical/watched IO uses the common
// buffer path. No mapped pointer or newly acquired lock survives a wait.
bool OverlappedWord(Memory& memory, uint32_t address, uint32_t& value,
                     bool write = false) {
  if (!address || address > UINT32_MAX - 3) { return false; }
  if (auto* heap = memory.LookupHeap(address);
      heap && heap->heap_type() == HeapType::kGuestVirtual &&
      memory.LookupHeap(address + 3) == heap) {
    auto lock = global_critical_region::AcquireDirect();
    if (!util::GuestIoRangeValid(memory, address, 4, write)) { return false; }
    auto* word = memory.TranslateVirtual<xe::be<uint32_t>*>(address);
    if (write) { *word = value; } else { value = *word; }
    return true;
  }
  std::array<uint8_t, 4> bytes{};
  if (write) {
    xe::store_and_swap<uint32_t>(bytes.data(), value);
    return XSUCCEEDED(util::WriteGuestIoBuffer(memory, address, bytes));
  }
  if (XFAILED(util::ReadGuestIoBuffer(memory, address, bytes))) { return false; }
  value = xe::load_and_swap<uint32_t>(bytes.data());
  return true;
}

uint32_t QueryOverlappedResult(uint32_t overlap, uint32_t length, uint32_t wait,
                                const ppc_context_t& context,
                                OverlappedWaitApi api) {
  auto& memory = *kernel_state()->memory();
  uint32_t result = 0;
  if (!OverlappedWord(memory, overlap, result)) {
    return X_ERROR_INVALID_PARAMETER;
  }
  if (result == X_ERROR_IO_PENDING) {
    if (!wait) { return X_ERROR_IO_INCOMPLETE; }
    uint32_t event = 0;
    if (overlap > UINT32_MAX - 0xF ||
        !OverlappedWord(memory, overlap + 0xC, event)) {
      return X_ERROR_INVALID_PARAMETER;
    }
    if (!event) { return X_ERROR_IO_INCOMPLETE; }
    // These are genuine wait results, distinct from the operation's DWORD.
    // A completed high-bit operation error is never remapped as a wait error.
    if (api == OverlappedWaitApi::kWin32) {
      const uint32_t waited =
          WaitForSingleObject_entry(event, UINT32_MAX, context);
      if (waited == X_STATUS_TIMEOUT) { return X_ERROR_IO_INCOMPLETE; }
      if (waited != X_STATUS_SUCCESS) {
        return uint32_t(uint64_t(RtlGetLastError_entry(context)));
      }
    } else {
      const uint32_t waited =
          xboxkrnl::NtWaitForSingleObjectEx(event, 1, 0, nullptr);
      if (int32_t(waited) < 0) {
        return xboxkrnl::xeRtlNtStatusToDosError(waited);
      }
    }
  }
  // Match both supplied 17559 bodies: optional length store FIRST, then read
  // result again. In particular length==overlap intentionally changes result.
  if (length) {
    uint32_t completed_length = 0;
    if (overlap > UINT32_MAX - 7 ||
        !OverlappedWord(memory, overlap + 4, completed_length) ||
        !OverlappedWord(memory, length, completed_length, true)) {
      return X_ERROR_INVALID_PARAMETER;
    }
  }
  return OverlappedWord(memory, overlap, result) ? result
                                               : X_ERROR_INVALID_PARAMETER;
}
}  // namespace

// Both native completion paths use lwz r3, including high-bit DWORD results.
// Avoid the generic DWORD shim's sign extension without changing shared ABI.
qword_result_t XamGetOverlappedResult_entry(
    pointer_t<XAM_OVERLAPPED> overlapped_ptr, lpdword_t length_ptr,
    dword_t wait, const ppc_context_t& context) {
  return QueryOverlappedResult(overlapped_ptr.guest_address(),
      length_ptr.guest_address(), wait, context, OverlappedWaitApi::kNt);
}
DECLARE_XAM_EXPORT2(XamGetOverlappedResult, kNone, kImplemented, kBlocking);

qword_result_t XGetOverlappedResult_entry(
    pointer_t<XAM_OVERLAPPED> overlapped_ptr, lpdword_t length_ptr,
    dword_t wait, const ppc_context_t& context) {
  return QueryOverlappedResult(overlapped_ptr.guest_address(),
      length_ptr.guest_address(), wait, context, OverlappedWaitApi::kWin32);
}
DECLARE_XAM_EXPORT2(XGetOverlappedResult, kNone, kImplemented, kBlocking);

}  // namespace xam
}  // namespace kernel
}  // namespace xe

DECLARE_XAM_EMPTY_REGISTER_EXPORTS(Msg);
