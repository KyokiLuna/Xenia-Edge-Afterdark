/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia contributors. All rights reserved.                    *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */
#include "xenia/kernel/native_import_domain.h"

#include <ranges>

#include "xenia/base/string.h"
#include "xenia/cpu/processor.h"
#include "xenia/cpu/xex_module.h"
#include "xenia/kernel/guest_scheduler.h"
#include "xenia/kernel/kernel_module.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/user_module.h"
#include "xenia/kernel/xthread.h"

namespace xe::kernel {
namespace {
NativeImportDomain::Member MemberOf(const UserModule& module) {
  return {module.hmodule_ptr(), module.load_generation()};
}
bool LibraryMatches(std::string_view a, std::string_view b) {
  const auto a_name = utf8::find_name_from_guest_path(a);
  const auto b_name = utf8::find_name_from_guest_path(b);
  return utf8::equal_case(a_name, b_name) ||
         (a_name.find('.') == std::string_view::npos &&
          utf8::equal_case(a_name,
                           utf8::find_base_name_from_guest_path(b_name))) ||
         (b_name.find('.') == std::string_view::npos &&
          utf8::equal_case(utf8::find_base_name_from_guest_path(a_name),
                           b_name));
}
bool ClaimsLibrary(const NativeImportDomain& domain, std::string_view name) {
  for (const auto& library : domain.profile()->claimed_libraries()) {
    if (LibraryMatches(library, name)) {
      return true;
    }
  }
  return false;
}
bool ConflictingRoot(const NativeImportDomain& domain, const UserModule& module,
                     const std::shared_ptr<NativeImportDomain>& other) {
  if (MemberOf(module) == domain.root()) {
    return false;
  }
  if (ClaimsLibrary(domain, module.name())) {
    return true;
  }
  // A separately named native root may be an alias such as $flash_xam.
  if (other && other->root() == MemberOf(module)) {
    for (const auto& library : other->profile()->claimed_libraries()) {
      if (ClaimsLibrary(domain, library)) {
        return true;
      }
    }
  }
  return false;
}
}  // namespace

std::shared_ptr<NativeImportDomain> KernelState::CreateNativeImportDomain(
    const object_ref<UserModule>& root,
    std::shared_ptr<const cpu::GuestImportProvider> profile) {
  auto lock = global_critical_region_.Acquire();
  // The native cohort may create SYSTEM workers. Its teardown requires the
  // actual scheduler's full CPU join; force-terminating host guest threads is
  // not proof that no thread still reaches a native provider or callback.
  if (!GuestScheduler::enabled() || !root || !profile ||
      root->kernel_state() != this || !root->is_system_app_module() ||
      root.get() == executable_module_.get() ||
      root->module_format() != UserModule::kModuleFormatXex ||
      root->unloading_ || root->import_domain_id_ || root->import_provider_ ||
      !root->load_generation() ||
      profile->owner().hmodule != root->hmodule_ptr() ||
      !next_native_import_domain_ || next_native_import_domain_ == UINT64_MAX ||
      std::ranges::find_if(user_modules_, [&](const auto& value) {
        return value.get() == root.get();
      }) == user_modules_.end()) {
    return {};
  }
  // The current scheduler setting cannot reclassify previously created OS
  // guest threads. Their force-termination is not a native CPU-stop proof.
  for (const auto& thread :
       object_table_.GetObjectsByType<XThread>(XObject::Type::Thread)) {
    if (!thread->is_host_thread() && thread->thread()) {
      return {};
    }
  }
  const auto runtime = guest_runtimes_.find(root->hmodule_ptr());
  if (runtime == guest_runtimes_.end()) {
    return {};
  }
  const auto state = runtime->second->GetSnapshot();
  if (state.identity.generation != profile->owner().generation ||
      state.state == GuestRuntime::State::kFailed ||
      state.state == GuestRuntime::State::kRetiring) {
    return {};
  }
  // Root imports are already compiled and cannot be rebound. The separately
  // loaded native XAM has only the actual kernel as an import dependency.
  for (const auto& library : *root->xex_module()->import_libraries()) {
    if (!LibraryMatches(library.name, "xboxkrnl.exe")) {
      return {};
    }
  }
  auto domain = NativeImportDomain::Create(next_native_import_domain_++,
                                           MemberOf(*root), std::move(profile));
  if (!domain) {
    return {};
  }
  native_import_domains_.emplace(domain->id(), domain);
  root->import_domain_ = domain;
  root->import_domain_id_ = domain->id();
  root->native_import_ready_.store(true, std::memory_order_release);
  return domain;
}

bool KernelState::RegisterNativeImportDomainMember(
    const std::shared_ptr<NativeImportDomain>& domain,
    const object_ref<UserModule>& module) {
  auto lock = global_critical_region_.Acquire();
  const auto found = domain ? native_import_domains_.find(domain->id())
                            : native_import_domains_.end();
  if (found == native_import_domains_.end() || found->second != domain ||
      !domain->active() || !module || module->kernel_state() != this ||
      !module->is_system_app_module() || module->unloading_ ||
      module->module_format() != UserModule::kModuleFormatXex ||
      module.get() == executable_module_.get() || !module->load_generation() ||
      std::ranges::find_if(user_modules_,
                           [&](const auto& value) {
                             return value.get() == module.get();
                           }) == user_modules_.end() ||
      (module->import_domain_id_ &&
       module->import_domain_id_ != domain->id()) ||
      (MemberOf(*module) != domain->root() &&
       module->import_provider_ != domain->profile())) {
    return false;
  }
  if (module->import_domain_id_ == domain->id()) {
    return domain->Contains(MemberOf(*module));
  }
  if ((module->is_dll_module() && !module->is_attached_) ||
      !domain->Register(MemberOf(*module))) {
    return false;
  }
  module->import_domain_ = domain;
  module->import_domain_id_ = domain->id();
  module->native_import_ready_.store(true, std::memory_order_release);
  return true;
}

KernelState::NativeImportCaller KernelState::ResolveNativeImportCaller(
    uint32_t return_address) {
  NativeImportCaller result;
  if (return_address < 4 || (return_address & 3)) {
    return result;
  }
  auto lock = global_critical_region_.Acquire();
  result.module = GetUserModuleForAddress(return_address - 4);
  if (!result.module || !result.module->import_domain_id_) {
    return result;
  }
  result.scoped = true;
  auto domain = result.module->import_domain_.lock();
  const auto found =
      native_import_domains_.find(result.module->import_domain_id_);
  xex2_section_type section;
  if (!domain || found == native_import_domains_.end() ||
      found->second != domain || !domain->active() ||
      !domain->Contains(MemberOf(*result.module)) ||
      !result.module->xex_module()->GetPageSectionType(return_address - 4,
                                                       &section) ||
      section != XEX_SECTION_CODE) {
    return result;  // Scoped but denied. Never silently fall back to HLE.
  }
  result.domain = std::move(domain);
  return result;
}

bool KernelState::RetireNativeImportDomain(
    const std::shared_ptr<NativeImportDomain>& domain) {
  auto lock = global_critical_region_.Acquire();
  const auto found = domain ? native_import_domains_.find(domain->id())
                            : native_import_domains_.end();
  return found != native_import_domains_.end() && found->second == domain &&
         domain->Retire();
}

bool KernelState::HasNativeImportDomains() {
  auto lock = global_critical_region_.Acquire();
  return !native_import_domains_.empty();
}

object_ref<XModule> KernelState::GetNativeImportModule(
    const NativeImportCaller& caller, std::string_view name) {
  if (!caller.scoped || !caller.domain || !caller.module) {
    return {};
  }
  auto operation = caller.domain->Acquire();
  if (!operation) {
    return {};
  }
  auto lock = global_critical_region_.Acquire();
  if (!caller.domain->Contains(MemberOf(*caller.module))) {
    return {};
  }
  if (name.empty()) {
    // Preserve XexGetModuleHandle(NULL)'s executable-module semantics. Caller
    // provenance selects a binding profile, not a new definition of "self".
    return caller.domain->active() ? object_ref<XModule>(executable_module_)
                                   : nullptr;
  }
  for (const auto& library : caller.domain->profile()->claimed_libraries()) {
    if (LibraryMatches(library, name)) {
      for (const auto& module : user_modules_) {
        if (MemberOf(*module) == caller.domain->root() && !module->unloading_ &&
            caller.domain->active()) {
          return module;
        }
      }
      return {};
    }
  }
  auto module = GetModule(name);
  if (module && module->module_type() == XModule::ModuleType::kUserModule) {
    auto* user = static_cast<UserModule*>(module.get());
    if (ConflictingRoot(*caller.domain, *user, user->import_domain_.lock())) {
      return {};
    }
  }
  return caller.domain->active() ? module : nullptr;
}

uint32_t KernelState::GetNativeImportProcedure(
    const NativeImportCaller& caller, const object_ref<XModule>& module,
    uint16_t ordinal, std::string_view name) {
  if (!caller.domain || !caller.scoped || !caller.module || !module) {
    return 0;
  }
  auto operation = caller.domain->Acquire();
  if (!operation) {
    return 0;
  }
  auto lock = global_critical_region_.Acquire();
  if (!caller.domain->Contains(MemberOf(*caller.module))) {
    return 0;
  }
  bool root = false;
  if (module->module_type() == XModule::ModuleType::kUserModule) {
    auto* user = static_cast<UserModule*>(module.get());
    root = MemberOf(*user) == caller.domain->root();
    if (ConflictingRoot(*caller.domain, *user, user->import_domain_.lock()) ||
        (root &&
         (user->unloading_ || user->import_domain_.lock() != caller.domain ||
          user->import_domain_id_ != caller.domain->id() ||
          !caller.domain->Contains(MemberOf(*user))))) {
      return 0;
    }
  } else {
    root = ClaimsLibrary(*caller.domain, module->name());
  }
  uint32_t result = 0;
  if (root) {
    // Claimed native exports and unclaimed HLE exports match static imports.
    // The explicitly loaded root may have an alias such as $flash_xam.xex.
    for (const auto& library : caller.domain->profile()->claimed_libraries()) {
      auto resolved =
          name.empty() ? caller.domain->profile()->Resolve(library, ordinal)
                       : caller.domain->profile()->ResolveName(library, name);
      if (resolved.disposition ==
          cpu::GuestImportProvider::Disposition::kRejected) {
        return 0;
      }
      if (resolved.disposition ==
          cpu::GuestImportProvider::Disposition::kBound) {
        result = resolved.address;
        break;
      }
      if (!name.empty()) {
        // Legacy profiles may not contain aliases. A known export name must
        // still honor its ordinal claim rather than fall through a hole.
        for (const auto& table : processor()->export_resolver()->tables()) {
          if (!LibraryMatches(table.module_name(), library)) {
            continue;
          }
          for (const auto* entry : table.exports_by_name()) {
            if (entry && name == entry->name) {
              resolved =
                  caller.domain->profile()->Resolve(library, entry->ordinal);
              if (resolved.disposition ==
                  cpu::GuestImportProvider::Disposition::kRejected) {
                return 0;
              }
              if (resolved.disposition ==
                  cpu::GuestImportProvider::Disposition::kBound) {
                result = resolved.address;
              }
              break;
            }
          }
        }
        if (result) {
          break;
        }
      }
      auto hle = IsKernelModule(library) ? GetKernelModule(library) : nullptr;
      if (hle) {
        result = name.empty() ? hle->GetProcAddressByOrdinal(ordinal)
                              : hle->GetProcAddressByName(name);
      }
    }
  } else {
    result = name.empty() ? module->GetProcAddressByOrdinal(ordinal)
                          : module->GetProcAddressByName(name);
  }
  return caller.domain->active() ? result : 0;
}
}  // namespace xe::kernel
