/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xboxkrnl/xboxkrnl_modules.h"
#include <array>
#include <atomic>

#include "third_party/mbedtls/include/mbedtls/sha256.h"
#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/base/mutex.h"
#include "xenia/cpu/xex_module.h"
#include "xenia/kernel/guest_runtime.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/native_import_domain.h"
#include "xenia/kernel/user_module.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_private.h"
#include "xenia/kernel/xthread.h"
#include "xenia/xbox.h"

DECLARE_bool(native_system_first_record_probe);

namespace xe {
namespace kernel {
namespace xboxkrnl {

static bool HasExecutablePrivilege(uint32_t privilege) {
  // Privilege is bit position in xe_xex2_system_flags enum - so:
  // Privilege=6 -> 0x00000040 -> XEX_SYSTEM_INSECURE_SOCKETS
  uint32_t mask = 1 << privilege;

  auto module = kernel_state()->GetExecutableModule();
  if (!module) {
    return false;
  }

  uint32_t flags = 0;
  module->GetOptHeader<uint32_t>(XEX_HEADER_SYSTEM_FLAGS, &flags);

  return (flags & mask) > 0;
}

dword_result_t XexCheckExecutablePrivilege_entry(dword_t privilege) {
  // BOOL
  // DWORD Privilege
  return HasExecutablePrivilege(privilege);
}
DECLARE_XBOXKRNL_EXPORT1(XexCheckExecutablePrivilege, kModules, kImplemented);

// Takes the signed part of an XEX2 header's security info, from its RSA
// signature on. The hypervisor checks it and decrypts its image key in place,
// which the dashboard needs to read a disc's executable.
dword_result_t XexTransformImageKey_entry(lpvoid_t security_info,
                                          dword_t size) {
  constexpr uint32_t kImageKeyOffset =
      offsetof(xex2_security_info, aes_key) -
      offsetof(xex2_security_info, rsa_signature);
  if (!HasExecutablePrivilege(0)) {
    return X_STATUS_ACCESS_DENIED;
  }
  if (!security_info || size < kImageKeyOffset + 0x10) {
    return X_STATUS_TOO_MANY_SECRETS;
  }
  uint8_t* key = security_info.as<uint8_t*>() + kImageKeyOffset;
  cpu::DecryptXex2ImageKey(key, key);
  return X_STATUS_SUCCESS;
}
DECLARE_XBOXKRNL_EXPORT1(XexTransformImageKey, kModules, kImplemented);

dword_result_t XexGetModuleHandle(std::string module_name,
                                  xe::be<uint32_t>* hmodule_ptr) {
  object_ref<XModule> module;

  if (module_name.empty()) {
    module = kernel_state()->GetExecutableModule();
  } else {
    module = kernel_state()->GetModule(module_name);
  }

  if (!module) {
    *hmodule_ptr = 0;
    return X_ERROR_NOT_FOUND;
  }

  // NOTE: we don't retain the handle for return.
  *hmodule_ptr = module->hmodule_ptr();

  return X_ERROR_SUCCESS;
}

dword_result_t XexGetModuleHandle_entry(lpstring_t module_name,
                                        lpdword_t hmodule_ptr,
                                        const ppc_context_t& ctx) {
  const auto caller =
      kernel_state()->ResolveNativeImportCaller(uint32_t(ctx->lr));
  if (caller.scoped) {
    if (!hmodule_ptr) {
      return X_STATUS_INVALID_PARAMETER;
    }
    *hmodule_ptr = 0;
    if (!caller.domain) {
      return X_STATUS_ACCESS_DENIED;
    }
    const auto module = kernel_state()->GetNativeImportModule(
        caller, module_name ? module_name.value() : "");
    if (!module) {
      return X_ERROR_NOT_FOUND;
    }
    *hmodule_ptr = module->hmodule_ptr();
    return X_ERROR_SUCCESS;
  }
  return XexGetModuleHandle(module_name ? module_name.value() : "",
                            hmodule_ptr);
}
DECLARE_XBOXKRNL_EXPORT1(XexGetModuleHandle, kModules, kImplemented);

dword_result_t XexGetModuleSection_entry(lpvoid_t hmodule, lpstring_t name,
                                         lpdword_t data_ptr,
                                         lpdword_t size_ptr) {
  X_STATUS result = X_STATUS_SUCCESS;

  auto module = XModule::GetFromHModule(kernel_state(), hmodule);
  if (module) {
    uint32_t section_data = 0;
    uint32_t section_size = 0;
    result = module->GetSection(name.value(), &section_data, &section_size);
    if (XSUCCEEDED(result)) {
      *data_ptr = section_data;
      *size_ptr = section_size;
    }
  } else {
    result = X_STATUS_INVALID_HANDLE;
  }

  return result;
}
DECLARE_XBOXKRNL_EXPORT1(XexGetModuleSection, kModules, kImplemented);

// Returns the image base of the module containing pc (callers pass it to
// RtlImageDirectoryEntryToData to find .pdata) and stores its loader entry. A
// pc in no module returns 0 and leaves ldr_data_ptr alone. The kernel also
// finds its own modules, which have no guest image here.
dword_result_t XexPcToFileHeader_entry(dword_t pc, lpdword_t ldr_data_ptr) {
  auto module = kernel_state()->GetUserModuleForAddress(pc);
  if (!module) {
    return 0;
  }
  if (ldr_data_ptr) {
    *ldr_data_ptr = module->hmodule_ptr();
  }
  return module->xex_module()->base_address();
}
DECLARE_XBOXKRNL_EXPORT1(XexPcToFileHeader, kModules, kImplemented);

inline const std::map<X_MODULE_FLAGS, std::string> xex_load_image_map = {
    {TITLE_PROCESS, "Title Process"},
    {TITLE_IMPORT, "Title Import"},
    {DEBUGGER, "Debugger Loaded"},
    {LOAD_DLL, "Load DLL"},
    {LOAD_PATCH, "Load Patch"},
    {LOAD_FULL_PATCH, "Load Full Patch"},
    {LOAD_DELTA_PATCH, "Load Delta Patch"},
    {BOUND_PATH, "Load By Bound Path"},
    {SILENT_LOAD, "Load Silently"}};

// Takes a reference to the module |module_name| names, loading it with
// |load_callback| first when it isn't loaded. Returns its hmodule, or 0.
static uint32_t LoadImageReference(
    const std::string_view module_name,
    const std::function<object_ref<UserModule>()>& load_callback) {
  uint32_t hmodule = 0;
  if (auto module = kernel_state()->GetModule(module_name)) {
    if (module->module_type() == XModule::ModuleType::kUserModule &&
        static_cast<UserModule*>(module.get())->native_import_domain_id()) {
      return 0;  // Unscoped loads cannot borrow another domain's profile.
    }
    hmodule = module->hmodule_ptr();
  } else if (auto user_module = load_callback()) {
    kernel_state()->ApplyTitleUpdate(user_module);
    kernel_state()->FinishLoadingUserModule(user_module);
    // Give up object ownership, this reference will be released by the last
    // XexUnloadImage call
    hmodule = user_module.release()->hmodule_ptr();
  }
  if (hmodule) {
    kernel_memory()
        ->TranslateVirtual<X_LDR_DATA_TABLE_ENTRY*>(hmodule)
        ->load_count++;
  }
  return hmodule;
}

uint32_t xeXexLoadImage(const std::string_view module_name) {
  return LoadImageReference(module_name, [module_name] {
    return kernel_state()->LoadUserModule(module_name);
  });
}

// The native manager still owns its descriptor/load/close policy. This only
// transports its exact caller domain across the kernel loader boundary.
static uint32_t LoadNativeImageReference(
    const KernelState::NativeImportCaller& caller, std::string_view name,
    const void* bytes = nullptr, size_t length = 0) {
  if (!caller.domain || !caller.module) {
    return 0;
  }
  auto operation = caller.domain->Acquire();
  if (!operation) {
    return 0;
  }
  auto module = kernel_state()->GetNativeImportModule(caller, name);
  bool fresh_reference = false;
  if (module) {
    if (bytes) {
      return 0;
    }
  } else {
    auto user =
        bytes ? kernel_state()->LoadUserModuleFromMemory(name, bytes, length,
                                                         caller.domain)
              : kernel_state()->LoadUserModule(
                    name, false, true, caller.domain->profile(), caller.domain);
    if (!user) {
      return 0;
    }
    const auto result = kernel_state()->FinishLoadingUserModule(user);
    if (XFAILED(result)) {
      // A partial native attach remains owned by the loader/domain. Do not
      // guess that Release/Free or another attach attempt can undo it.
      caller.domain->Retire();
      return 0;
    }
    module = std::move(user);
    fresh_reference = true;
  }
  auto lock = global_critical_region::Acquire();
  if (!caller.domain->active()) {
    return 0;
  }
  if (module->module_type() == XModule::ModuleType::kUserModule) {
    auto user = retain_object(static_cast<UserModule*>(module.get()));
    // Read-only lookup may name a title or unrelated DLL. Loader reference
    // reuse still requires this domain's already admitted, ready generation.
    // Revalidate under the same kernel region as the load-count update, so an
    // unload claim cannot race this reference acquisition.
    if (user->native_import_domain_id() != caller.domain->id() ||
        !user->native_import_ready() ||
        !caller.domain->Contains(
            {user->hmodule_ptr(), user->load_generation()}) ||
        !kernel_state()->RegisterNativeImportDomainMember(caller.domain,
                                                          user)) {
      return 0;
    }
  }
  auto* ldr = kernel_memory()->TranslateVirtual<X_LDR_DATA_TABLE_ENTRY*>(
      module->hmodule_ptr());
  ++ldr->load_count;
  if (fresh_reference) {
    // Match LoadImageReference's one loader reference, released by the final
    // XexUnloadImage rather than by this temporary object_ref.
    module->Retain();
  }
  return module->hmodule_ptr();
}

dword_result_t xeXexLoadImage(
    lpstring_t module_name, dword_t module_flags, dword_t min_version,
    lpdword_t hmodule_ptr,
    const std::function<object_ref<UserModule>()>& load_callback,
    bool isFromMemory) {
  std::string summary = "Loading Xex Image:";
  uint32_t unused_flag = module_flags;

  for (const auto& entry : xex_load_image_map) {
    if (module_flags & entry.first) {
      summary += fmt::format(" {},", entry.second);
      unused_flag &= ~entry.first;
    }
  }
  if (unused_flag) {
    summary += fmt::format(" Unk flag: {:08X}", unused_flag);
  }
  XELOGD("{}", summary);

  if (!hmodule_ptr) {
    return X_ERROR_INVALID_PARAMETER;
  }

  if (isFromMemory && kernel_state()->GetModule(module_name.value())) {
    // Existing module found; return error status.
    *hmodule_ptr = 0;
    return X_STATUS_OBJECT_NAME_COLLISION;
  }
  const uint32_t hmodule =
      LoadImageReference(module_name.value(), load_callback);
  *hmodule_ptr = hmodule;
  return hmodule ? X_STATUS_SUCCESS : X_STATUS_NO_SUCH_FILE;
}

dword_result_t XexLoadImage_entry(lpstring_t module_name, dword_t module_flags,
                                  dword_t min_version, lpdword_t hmodule_ptr,
                                  const ppc_context_t& ctx) {
  const auto caller =
      kernel_state()->ResolveNativeImportCaller(uint32_t(ctx->lr));
  if (caller.scoped) {
    if (!module_name || !hmodule_ptr) {
      return X_STATUS_INVALID_PARAMETER;
    }
    *hmodule_ptr = 0;
    const uint32_t loaded =
        LoadNativeImageReference(caller, module_name.value());
    *hmodule_ptr = loaded;
    return loaded ? X_STATUS_SUCCESS : X_STATUS_ACCESS_DENIED;
  }
  auto load_callback = [module_name] {
    return kernel_state()->LoadUserModule(module_name.value());
  };
  return xeXexLoadImage(module_name, module_flags, min_version, hmodule_ptr,
                        load_callback, false);
}
DECLARE_XBOXKRNL_EXPORT1(XexLoadImage, kModules, kImplemented);

dword_result_t XexLoadImageFromMemory_entry(lpdword_t buffer, dword_t size,
                                            lpstring_t module_name,
                                            dword_t module_flags,
                                            dword_t min_version,
                                            lpdword_t hmodule_ptr,
                                            const ppc_context_t& ctx) {
  const auto caller =
      kernel_state()->ResolveNativeImportCaller(uint32_t(ctx->lr));
  if (caller.scoped) {
    if (!buffer || !size || !module_name || !hmodule_ptr) {
      return X_STATUS_INVALID_PARAMETER;
    }
    *hmodule_ptr = 0;
    if (!caller.domain) {
      return X_STATUS_ACCESS_DENIED;
    }
    if (kernel_state()->GetModule(module_name.value())) {
      return X_STATUS_OBJECT_NAME_COLLISION;
    }
    const uint32_t loaded =
        LoadNativeImageReference(caller, module_name.value(),
                                 static_cast<xe::be<uint32_t>*>(buffer), size);
    *hmodule_ptr = loaded;
    return loaded ? X_STATUS_SUCCESS : X_STATUS_ACCESS_DENIED;
  }
  auto load_callback = [module_name, buffer, size] {
    return kernel_state()->LoadUserModuleFromMemory(module_name.value(), buffer,
                                                    size);
  };
  return xeXexLoadImage(module_name, module_flags, min_version, hmodule_ptr,
                        load_callback, true);
}
DECLARE_XBOXKRNL_EXPORT1(XexLoadImageFromMemory, kModules, kImplemented);

dword_result_t XexLoadExecutable_entry(lpstring_t module_name,
                                       dword_t module_flags,
                                       dword_t min_version,
                                       lpdword_t hmodule_ptr,
                                       const ppc_context_t& ctx) {
  return XexLoadImage_entry(module_name, module_flags, min_version, hmodule_ptr,
                            ctx);
}
DECLARE_XBOXKRNL_EXPORT1(XexLoadExecutable, kModules, kSketchy);

X_STATUS xeXexUnloadImage(uint32_t hmodule) {
  if (!hmodule) {
    return X_STATUS_INVALID_HANDLE;
  }
  auto ldr_data =
      kernel_memory()->TranslateVirtual<X_LDR_DATA_TABLE_ENTRY*>(hmodule);
  auto module = XModule::GetFromHModule(kernel_state(), ldr_data);
  if (!module) {
    return X_STATUS_INVALID_HANDLE;
  }

  // Can't unload kernel modules from user code.
  if (module->module_type() != XModule::ModuleType::kKernelModule) {
    if (kernel_state()->GetGuestRuntime(module->hmodule_ptr()) ||
        static_cast<UserModule*>(module.get())->has_execution_leases()) {
      return X_STATUS_ACCESS_DENIED;
    }
    if (--ldr_data->load_count == 0) {
      // No more references, free it.
      module->Release();
      auto user_module = object_ref<UserModule>(
          reinterpret_cast<UserModule*>(module.release()));
      const auto result = kernel_state()->UnloadUserModule(user_module);
      if (XFAILED(result)) {
        // Preserve the loader reference/count when a resident native runtime
        // refuses partial unload. The explicit Release above removed it.
        user_module->Retain();
        ++ldr_data->load_count;
        return result;
      }
    }
  }

  return X_STATUS_SUCCESS;
}

// Read-only diagnostic for the supplied native initializer's existing failed
// cleanup boundary. One manager probe per process: at most 16 records, once per
// app ID. No Kernel/module pointer or callback survives this synchronous call.
static void ObserveNativeSysAppCleanup(uint32_t hmodule,
                                       const ppc_context_t& context) noexcept {
  if (!cvars::native_system_first_record_probe ||
      uint32_t(context->lr) != 0x816BCF18) {
    return;
  }
  try {
    auto* kernel = kernel_state();
    if (!XThread::IsInThread() || !kernel) {
      return;
    }
    auto* thread = XThread::GetCurrentThread();
    if (thread->kernel_state() != kernel ||
        thread->thread_state()->context() != context.value() ||
        thread->native_frame_thread_identity().process_type !=
            X_PROCTYPE_SYSTEM) {
      return;
    }
    const auto caller =
        kernel->ResolveNativeImportCaller(uint32_t(context->lr));
    if (!caller.scoped || !caller.domain || !caller.module ||
        caller.module->module_format() != UserModule::kModuleFormatXex ||
        !caller.module->native_import_ready() ||
        caller.domain->root() !=
            NativeImportDomain::Member{caller.module->hmodule_ptr(),
                                       caller.module->load_generation()} ||
        caller.domain->profile()->owner().hmodule !=
            caller.module->hmodule_ptr() ||
        caller.module->hash() != UINT64_C(0xDD9939F8B910C18D) ||
        caller.module->xex_module()->image_code_sha1() !=
            "D8F94A9D7D09F731B12215B7BAB23847564F06AE") {
      return;
    }
    auto operation = caller.domain->Acquire();
    auto lease = kernel->AcquireUserModuleExecutionLease(caller.module);
    const auto runtime = kernel->GetGuestRuntime(caller.module->hmodule_ptr());
    if (!operation || !lease || !runtime) {
      return;
    }
    const auto ready = runtime->GetSnapshot();
    if (ready.state != GuestRuntime::State::kReady ||
        ready.identity.hmodule != caller.module->hmodule_ptr() ||
        ready.identity.generation !=
            caller.domain->profile()->owner().generation) {
      return;
    }
    constexpr uint32_t kInitializer = 0x816BCD08, kInitializerSize = 0x258;
    constexpr std::array<uint8_t, 32> kInitializerSha256 = {
        0xA7, 0x63, 0x0E, 0xF9, 0x36, 0x75, 0xD8, 0xDB, 0x35, 0xD1, 0xC1,
        0x3E, 0x15, 0xED, 0x51, 0x7D, 0x54, 0xAD, 0x8E, 0x66, 0x21, 0x1A,
        0x1D, 0x88, 0xAC, 0xB2, 0xF6, 0xAE, 0x8D, 0x43, 0x89, 0x87};
    const uint32_t descriptor = uint32_t(context->r[31]);
    const uint32_t prior = uint32_t(context->r[30]);
    auto* image = caller.module->xex_module();
    const uint64_t end = uint64_t(image->base_address()) + image->image_size();
    if ((descriptor & 3) || descriptor < image->base_address() ||
        uint64_t(descriptor) + 0xBC > end || int32_t(prior) >= 0) {
      return;
    }
    uint32_t app, module, callback, self_module;
    {
      auto global = global_critical_region::Acquire();
      auto* memory = kernel->memory();
      if (memory->QueryRangeAccess(kInitializer, kInitializerSize) ==
              memory::PageAccess::kNoAccess ||
          memory->QueryRangeAccess(descriptor, 0xBC) ==
              memory::PageAccess::kNoAccess) {
        return;
      }
      for (uint32_t i = 0; i < kInitializerSize; i += 4) {
        xex2_section_type type;
        const uint32_t address = kInitializer + i;
        if (kernel->GetUserModuleForAddress(address).get() !=
                caller.module.get() ||
            !image->GetPageSectionType(address, &type) ||
            type != XEX_SECTION_CODE ||
            image->TranslateCode(address) !=
                memory->TranslateVirtual<const uint8_t*>(address)) {
          return;
        }
      }
      for (uint32_t i = 0; i < 0xBC; i += 4) {
        xex2_section_type type;
        if (!image->GetPageSectionType(descriptor + i, &type) ||
            type != XEX_SECTION_DATA) {
          return;
        }
      }
      std::array<uint8_t, 32> digest;
      mbedtls_sha256(memory->TranslateVirtual<const uint8_t*>(kInitializer),
                     kInitializerSize, digest.data(), 0);
      if (digest != kInitializerSha256) {
        return;
      }
      xex2_section_type self_type;
      constexpr uint32_t kSelfModuleCell = 0x81AA3EE0;
      if (!image->GetPageSectionType(kSelfModuleCell, &self_type) ||
          self_type != XEX_SECTION_DATA ||
          memory->QueryRangeAccess(kSelfModuleCell, 4) ==
              memory::PageAccess::kNoAccess) {
        return;
      }
      self_module = xe::load_and_swap<uint32_t>(
          memory->TranslateVirtual(kSelfModuleCell));
      app =
          xe::load_and_swap<uint32_t>(memory->TranslateVirtual(descriptor + 4));
      module =
          xe::load_and_swap<uint32_t>(memory->TranslateVirtual(descriptor + 8));
      callback = xe::load_and_swap<uint32_t>(
          memory->TranslateVirtual(descriptor + 0xC));
    }
    if (module != hmodule || app > 0xFF) {
      return;
    }
    static std::array<std::atomic<bool>, 256> seen{};
    static std::atomic<uint32_t> records{0};
    if (seen[app].exchange(true, std::memory_order_relaxed) ||
        records.fetch_add(1, std::memory_order_relaxed) >= 16) {
      return;
    }
    XELOGE(
        "Native first-record SysApp cleanup: app={:08X} prior_hresult={:08X} "
        "descriptor={:08X} callback={:08X} module={:08X} guest_tid={:08X} "
        "thread_handle={:08X} domain={} root_load_generation={} "
        "runtime_generation={} root_hmodule={:08X} self_hmodule={:08X}",
        app, prior, descriptor, callback, module, thread->thread_id(),
        thread->handle(), caller.domain->id(), caller.module->load_generation(),
        ready.identity.generation, caller.module->hmodule_ptr(), self_module);
  } catch (...) {
    // Optional observation must not replace the real unload result, modify
    // the native caller's original HRESULT, or unwind its guest callback.
  }
}

dword_result_t XexUnloadImage_entry(lpvoid_t hmodule,
                                    const ppc_context_t& ctx) {
  ObserveNativeSysAppCleanup(hmodule.guest_address(), ctx);
  // Preserve the unload API's null-handle rejection before scoped lookup can
  // dereference a loader entry during native initialization failure cleanup.
  if (!hmodule) {
    return X_STATUS_INVALID_HANDLE;
  }
  const auto caller =
      kernel_state()->ResolveNativeImportCaller(uint32_t(ctx->lr));
  if (caller.scoped) {
    if (!caller.domain) {
      return X_STATUS_ACCESS_DENIED;
    }
    auto operation = caller.domain->Acquire();
    if (!operation) {
      return X_STATUS_ACCESS_DENIED;
    }
    const auto target = XModule::GetFromHModule(kernel_state(), hmodule);
    if (!target ||
        (target->module_type() == XModule::ModuleType::kUserModule &&
         (!static_cast<UserModule*>(target.get())->native_import_domain_id() ||
          static_cast<UserModule*>(target.get())->native_import_domain_id() !=
              caller.domain->id() ||
          !caller.domain->Contains(
              {target->hmodule_ptr(),
               static_cast<UserModule*>(target.get())->load_generation()})))) {
      return X_STATUS_ACCESS_DENIED;
    }
    return xeXexUnloadImage(hmodule.guest_address());
  }
  if (const auto target = XModule::GetFromHModule(kernel_state(), hmodule);
      target && target->module_type() == XModule::ModuleType::kUserModule &&
      static_cast<UserModule*>(target.get())->native_import_domain_id()) {
    return X_STATUS_ACCESS_DENIED;
  }
  return xeXexUnloadImage(hmodule.guest_address());
}
DECLARE_XBOXKRNL_EXPORT1(XexUnloadImage, kModules, kImplemented);

dword_result_t XexGetProcedureAddress_entry(lpvoid_t hmodule, dword_t ordinal,
                                            lpdword_t out_function_ptr,
                                            const ppc_context_t& ctx) {
  const auto caller =
      kernel_state()->ResolveNativeImportCaller(uint32_t(ctx->lr));
  if (caller.scoped) {
    if (!out_function_ptr || !ordinal) {
      return X_STATUS_INVALID_PARAMETER;
    }
    *out_function_ptr = 0;
    if (!caller.domain) {
      return X_STATUS_ACCESS_DENIED;
    }
    auto module = hmodule ? XModule::GetFromHModule(kernel_state(), hmodule)
                          : kernel_state()->GetNativeImportModule(caller, "");
    if (!module) {
      return X_STATUS_INVALID_HANDLE;
    }
    const bool by_name = (ordinal & 0xFFFF0000) != 0;
    const auto name =
        by_name ? std::string_view(
                      kernel_memory()->TranslateVirtual<const char*>(ordinal))
                : std::string_view{};
    const uint32_t address = kernel_state()->GetNativeImportProcedure(
        caller, module, uint16_t(ordinal), name);
    if (!address) {
      return X_STATUS_DRIVER_ENTRYPOINT_NOT_FOUND;
    }
    *out_function_ptr = address;
    return X_STATUS_SUCCESS;
  }
  // May be entry point?
  assert_not_zero(ordinal);

  bool is_string_name = (ordinal & 0xFFFF0000) != 0;
  auto string_name =
      reinterpret_cast<const char*>(kernel_memory()->TranslateVirtual(ordinal));

  X_STATUS result = X_STATUS_INVALID_HANDLE;

  object_ref<XModule> module;
  if (!hmodule) {
    module = kernel_state()->GetExecutableModule();
  } else {
    module = XModule::GetFromHModule(kernel_state(), hmodule);
  }
  if (module) {
    uint32_t ptr;
    if (is_string_name) {
      ptr = module->GetProcAddressByName(string_name);
    } else {
      ptr = module->GetProcAddressByOrdinal(ordinal);
    }
    if (ptr) {
      *out_function_ptr = ptr;
      result = X_STATUS_SUCCESS;
    } else {
      if (is_string_name) {
        XELOGW("ERROR: XexGetProcedureAddress export '{}' in '{}' not found!",
               string_name, module->name());
      } else {
        XELOGW(
            "ERROR: XexGetProcedureAddress ordinal {} (0x{:X}) in '{}' not "
            "found!",
            static_cast<uint32_t>(ordinal), static_cast<uint32_t>(ordinal),
            module->name());
      }
      *out_function_ptr = 0;
      result = X_STATUS_DRIVER_ENTRYPOINT_NOT_FOUND;
    }
  }

  return result;
}
DECLARE_XBOXKRNL_EXPORT1(XexGetProcedureAddress, kModules, kImplemented);

void ExRegisterTitleTerminateNotification_entry(
    pointer_t<X_EX_TITLE_TERMINATE_REGISTRATION> reg, dword_t create) {
  if (create) {
    // Adding.
    kernel_state()->RegisterTitleTerminateNotification(
        reg->notification_routine, reg->priority);
  } else {
    // Removing.
    kernel_state()->RemoveTitleTerminateNotification(reg->notification_routine);
  }
}
DECLARE_XBOXKRNL_EXPORT1(ExRegisterTitleTerminateNotification, kModules,
                         kImplemented);
// todo: replace magic numbers
dword_result_t XexLoadImageHeaders_entry(pointer_t<X_ANSI_STRING> path,
                                         pointer_t<xex2_header> header,
                                         dword_t buffer_size,
                                         const ppc_context_t& ctx) {
  if (buffer_size < 0x800) {
    return X_STATUS_BUFFER_TOO_SMALL;
  }
  auto current_kernel = ctx->kernel_state;
  auto target_path = util::TranslateAnsiPath(current_kernel->memory(), path);

  vfs::File* vfs_file = nullptr;
  vfs::FileAction file_action;
  X_STATUS result = current_kernel->file_system()->OpenFile(
      nullptr, target_path, vfs::FileDisposition::kOpen,
      vfs::FileAccess::kGenericRead, false, true, &vfs_file, &file_action);

  if (!vfs_file) {
    return result;
  }
  size_t bytes_read = 0;

  X_STATUS result_status = vfs_file->ReadSync(
      std::span<uint8_t>(reinterpret_cast<uint8_t*>(header.host_address()),
                         2048),
      0, &bytes_read);

  if (result_status < 0) {
    vfs_file->Destroy();
    return result_status;
  }

  if (header->magic != 'XEX2') {
    vfs_file->Destroy();
    return X_STATUS_INVALID_IMAGE_FORMAT;
  }
  unsigned int header_size = header->header_size;

  if (header_size < 0x800 || header_size > 0x10000 ||
      (header_size & 0x7FF) != 0) {
    result_status = X_STATUS_INVALID_IMAGE_FORMAT;
  } else if (header_size <= buffer_size) {
    if (header_size <= 0x800) {
      result_status = X_STATUS_SUCCESS;
    } else {
      result_status = vfs_file->ReadSync(
          std::span<uint8_t>(
              reinterpret_cast<uint8_t*>(header.host_address() + 2048),
              header_size - 2048),
          2048, &bytes_read);
      if (result_status >= X_STATUS_SUCCESS) {
        result_status = X_STATUS_SUCCESS;
      }
    }

  } else {
    result_status = X_STATUS_BUFFER_TOO_SMALL;
  }

  // The security info's offset becomes its address in the buffer, which is
  // how titles reach it.
  if (result_status == X_STATUS_SUCCESS) {
    const uint32_t security_offset = header->security_offset;
    const uint32_t remaining =
        security_offset < header_size ? header_size - security_offset : 0;
    const uint8_t* data = reinterpret_cast<uint8_t*>(header.host_address());
    const uint32_t security_size =
        remaining >= 4 ? xe::load_and_swap<uint32_t>(data + security_offset)
                       : 0;
    // At least everything before the page descriptors.
    if (security_size < 0x184 || security_size > remaining) {
      result_status = X_STATUS_INVALID_IMAGE_FORMAT;
    } else {
      header->security_offset = header.guest_address() + security_offset;
    }
  }

  vfs_file->Destroy();
  return result_status;
}
DECLARE_XBOXKRNL_EXPORT1(XexLoadImageHeaders, kModules, kImplemented);

}  // namespace xboxkrnl
}  // namespace kernel
}  // namespace xe

DECLARE_XBOXKRNL_EMPTY_REGISTER_EXPORTS(Module);
