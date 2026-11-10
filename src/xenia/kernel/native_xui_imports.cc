/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia contributors. All rights reserved.                    *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/native_xui_imports.h"

#include <string_view>

#include "xenia/base/logging.h"
#include "xenia/base/string.h"
#include "xenia/cpu/export_resolver.h"
#include "xenia/cpu/processor.h"
#include "xenia/kernel/guest_runtime.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/user_module.h"
#include "xenia/kernel/xthread.h"

namespace xe::kernel {
namespace {
bool IsNativeUiFamily(const cpu::Export& entry) {
  const std::string_view name(entry.name);
  if (name.starts_with("Xui") || name.starts_with("XUI")) {
    return true;
  }
  // Resource locators and gamer-card controls cross native XUI ownership too.
  switch (entry.ordinal) {
    case 0x315:  // XamBuildSharedSystemResourceLocator
    case 0x31B:  // XamBuildResourceLocator
    case 0x31C:  // XamBuildLegacySystemResourceLocator
    case 0x31D:  // XamBuildGamercardResourceLocator
    case 0x31E:  // XamBuildDynamicResourceLocator
    case 0x31F:  // XamBuildXamResourceLocator
    case 0x3C9:  // XamHudGetUserIndex (same native controller owner)
    case 0x46A:  // XamHudSetUserIndex (paired actor/refresh operation)
    case 0x3E4:  // GamerCardStartup
    case 0x3E5:  // GamerCardCleanup
    case 0x3E6:  // GamerCardRegisterControls
    case 0x3E7:  // GamerCardUnregisterControls
      return true;
    default:
      return false;
  }
}
bool IsNativeSystemUiFamily(const cpu::Export& entry) {
  if (IsNativeUiFamily(entry)) {
    return true;
  }
  const std::string_view name(entry.name);
  if (name.starts_with("XamTask") || name.starts_with("XMsg") ||
      name.starts_with("XNotify") || name.starts_with("XamNotify") ||
      name.starts_with("XamInput") || name.starts_with("XamApp") ||
      (name.starts_with("XamShow") && name.ends_with("UI"))) {
    return true;
  }
  switch (entry.ordinal) {
    case 0x196:  // XamEnableSystemAppInput, same native counter/cache owner.
    case 0x1AB:  // XamLoaderGetGameInfo, same native config/metadata owner.
    case 0x1EA:  // XamAlloc.
    case 0x1EB:  // XamAllocEx.
    case 0x1EC:  // XamFree.
    case 0x1ED:  // XamAllocSize.
    case 0x24B:  // XamRegisterSysApp.
    case 0x24C:  // XamUnregisterSysApp.
    case 0x251:  // XamLoadSysApp.
    case 0x252:  // XamUnloadSysApp.
    case 0x253:  // XamReloadSysApp.
    case 0x2E9:  // XamShowMessageBox, callback-owned SYSTEM selector warning.
    case 0x460:  // lstrcpynW, native UI UTF16 result-buffer copy.
    case 0x489:  // XMemAlloc.
    case 0x48A:  // XMemFree aliases native XamFree.
    case 0x48B:  // XMemSize.
    case 0x4A7:  // XamLoaderGetState, paired native loader metadata query.
      return true;
    default:
      return name == "XamGetOnlineSchema";
  }
}

std::shared_ptr<const cpu::GuestImportProvider> CreateNativeImports(
    KernelState* kernel_state, const object_ref<UserModule>& native_xam,
    bool system_profile, std::string* error) {
  const auto fail = [&](const char* message)
      -> std::shared_ptr<const cpu::GuestImportProvider> {
    if (error) {
      *error = message;
    }
    return {};
  };
  if (!kernel_state || !XThread::IsInThread() || !native_xam) {
    return fail("Native XUI imports require a SYSTEM guest thread");
  }
  auto* thread = XThread::GetCurrentThread();
  if (thread->guest_object<X_KTHREAD>()->process_type != X_PROCTYPE_SYSTEM ||
      native_xam->kernel_state() != kernel_state ||
      !native_xam->is_system_app_module() ||
      native_xam.get() == kernel_state->GetExecutableModule().get() ||
      native_xam->module_format() != UserModule::kModuleFormatXex) {
    return fail("Native XUI imports require a separate SYSTEM-owned XAM");
  }
  auto runtime = kernel_state->GetGuestRuntime(native_xam->hmodule_ptr());
  if (!runtime) {
    return fail("Native XUI imports require a resident Ready runtime");
  }
  const auto snapshot = runtime->GetSnapshot();
  if (snapshot.state != GuestRuntime::State::kReady ||
      snapshot.initialization_active ||
      snapshot.identity.hmodule != native_xam->hmodule_ptr()) {
    return fail("Native XUI imports require a resident Ready runtime");
  }
  auto* image = native_xam->xex_module();
  if (!image->loaded() ||
      kernel_state->GetUserModuleForAddress(image->base_address()).get() !=
          native_xam.get()) {
    return fail("Native XUI import owner is not the loaded module generation");
  }
  using Provider = cpu::GuestImportProvider;
  const auto valid_address = [&](Provider::Kind kind, uint32_t address) {
    xex2_section_type section;
    return address && !(address & 3) && address >= image->base_address() &&
           uint64_t(address) + 4 <=
               uint64_t(image->base_address()) + image->image_size() &&
           image->GetPageSectionType(address, &section) &&
           (kind == Provider::Kind::kFunction
                ? section == XEX_SECTION_CODE
                : (section == XEX_SECTION_DATA ||
                   section == XEX_SECTION_READONLY_DATA)) &&
           kernel_state->GetUserModuleForAddress(address).get() ==
               native_xam.get();
  };
  Provider::Config config;
  config.owner = {snapshot.identity.hmodule, snapshot.identity.generation};
  config.owner_pin = std::make_shared<object_ref<UserModule>>(native_xam);
  Provider::Family family{"xam.xex", {}};
  for (const auto& table :
       kernel_state->processor()->export_resolver()->tables()) {
    if (table.module_name() !=
        utf8::find_base_name_from_guest_path("xam.xex")) {
      continue;
    }
    for (const auto* entry : table.exports_by_ordinal()) {
      if (!entry || !(system_profile ? IsNativeSystemUiFamily(*entry)
                                     : IsNativeUiFamily(*entry))) {
        continue;
      }
      family.ordinals.push_back(entry->ordinal);
      // A claimed hole must reject name lookup just like ordinal lookup.
      config.name_aliases.push_back({"xam.xex", entry->name, entry->ordinal});
      const uint32_t address =
          native_xam->GetProcAddressByOrdinal(entry->ordinal);
      const auto kind = entry->get_type() == cpu::Export::Type::kFunction
                            ? Provider::Kind::kFunction
                            : Provider::Kind::kData;
      if (!valid_address(kind, address)) {
        // A zero export-table offset can resolve to the PE base. Keep the
        // family claim, but omit missing/non-code function bindings. Actual
        // consumers still fail preflight if they request one of these holes.
        XELOGD("Native XUI imports: unavailable ordinal={:03X} address={:08X}",
               entry->ordinal, address);
        continue;
      }
      config.bindings.push_back({"xam.xex", entry->ordinal, kind, address});
    }
  }
  if (family.ordinals.empty()) {
    return fail("No XAM XUI family is registered");
  }
  const size_t claims = family.ordinals.size();
  config.families.push_back(std::move(family));
  auto result = Provider::Create(std::move(config), valid_address, error);
  if (result) {
    XELOGI("Native {} imports: owner={:08X} generation={} family_claims={}",
           system_profile ? "SYSTEM UI" : "XUI", snapshot.identity.hmodule,
           snapshot.identity.generation, claims);
  }
  return result;
}
}  // namespace

std::shared_ptr<const cpu::GuestImportProvider> CreateNativeXuiImportProvider(
    KernelState* kernel_state, const object_ref<UserModule>& native_xam,
    std::string* error) {
  return CreateNativeImports(kernel_state, native_xam, false, error);
}

std::shared_ptr<const cpu::GuestImportProvider>
CreateNativeSystemUiImportProvider(KernelState* kernel_state,
                                   const object_ref<UserModule>& native_xam,
                                   std::string* error) {
  return CreateNativeImports(kernel_state, native_xam, true, error);
}
}  // namespace xe::kernel
