/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia contributors. All rights reserved.                    *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */
#include "xenia/kernel/native_hud_module_ownership.h"

#include <array>
#include <atomic>
#include <optional>
#include <string>

#include "third_party/mbedtls/include/mbedtls/sha256.h"
#include "xenia/base/byte_order.h"
#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/base/mutex.h"
#include "xenia/cpu/xex_module.h"
#include "xenia/kernel/guest_runtime_call_executor.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/native_import_domain.h"
#include "xenia/kernel/native_system_allocation_authority.h"
#include "xenia/kernel/native_system_ui_service.h"
#include "xenia/kernel/user_module.h"
#include "xenia/kernel/xthread.h"
#include "xenia/memory.h"
#include "xenia/vfs/device.h"
#include "xenia/vfs/entry.h"

DECLARE_bool(native_system_first_record_probe);

namespace xe::kernel {
namespace {
constexpr uint32_t kHudCell = 0x81AA0B5C, kSelfCell = 0x81AA3EE0;
constexpr uint64_t kAllChecks = (uint64_t(1) << 12) - 1;
constexpr std::string_view kHudSourceSha =
    "37e10612afde211e1d349838704f7cc2648ad5a4d1f8ca459f18ecd2c1f31ac7";
std::optional<uint32_t> Word(Memory& memory, uint32_t at) {
  if (!at || (at & 3) ||
      memory.QueryRangeAccess(at, 4) == xe::memory::PageAccess::kNoAccess) {
    return {};
  }
  return xe::load_and_swap<uint32_t>(memory.TranslateVirtual(at));
}
std::string Sha256(const uint8_t* bytes, size_t size) {
  std::array<uint8_t, 32> digest;
  if (mbedtls_sha256(bytes, size, digest.data(), 0)) {
    return {};
  }
  constexpr char hex[] = "0123456789abcdef";
  std::string result;
  result.reserve(64);
  for (const auto byte : digest) {
    result.push_back(hex[byte >> 4]);
    result.push_back(hex[byte & 15]);
  }
  return result;
}
struct Lifetime {
  NativeImportDomain::Operation operation;
  object_ref<UserModule> root, hud;
  std::shared_ptr<KernelState::UserModuleExecutionLease> root_lease, hud_lease;
};
// The trace samples every current FE predicate input, without becoming an
// admission source. Only four snapshots/four lines each are allowed per probe.
void Trace(const NativeSystemUiServiceContext& c, const Lifetime& hold,
           uint64_t passed, NativeHudOriginPolicy policy,
           const std::string& origin, const std::string& source_sha) noexcept {
  if (!cvars::native_system_first_record_probe || !XThread::IsInThread()) {
    return;
  }
  auto* thread = XThread::GetCurrentThread();
  if (thread->kernel_state() != c.kernel) {
    return;
  }
  static std::atomic<uint32_t> count{0};
  uint32_t record = count.load(std::memory_order_relaxed);
  while (record < 4 && !count.compare_exchange_weak(
                           record, record + 1, std::memory_order_relaxed)) {
  }
  if (record >= 4) {
    return;
  }
  try {
    auto& memory = *c.kernel->memory();
    std::array<uint32_t, 6> words{};
    constexpr std::array<uint32_t, 6> cells{0x81AA0BA4, 0x81AA0BA8, 0x81AA0BAC,
                                            0x81AA0BB0, kHudCell,   kSelfCell};
    uint32_t readable = 0;
    for (size_t i = 0; i < cells.size(); ++i) {
      if (const auto word = Word(memory, cells[i])) {
        readable |= 1u << i;
        words[i] = *word;
      }
    }
    const auto caller = c.kernel->ResolveNativeImportCaller(0x816BD934);
    auto module = hold.root;
    const auto owner_begin = c.kernel->GetUserModuleForAddress(0x816BCCA8);
    const auto owner_end = c.kernel->GetUserModuleForAddress(0x816BCD07);
    bool code_readable = false, code_section = false, original = false;
    std::string ctor_sha;
    if (module && hold.root_lease) {
      xex2_section_type section;
      code_section =
          module->xex_module()->GetPageSectionType(0x816BCCA8, &section) &&
          section == XEX_SECTION_CODE;
      code_readable = memory.QueryRangeAccess(0x816BCCA8, 96) !=
                      xe::memory::PageAccess::kNoAccess;
      if (code_readable) {
        original = true;
        for (uint32_t at = 0x816BCCA8; at < 0x816BCD08; at += 4) {
          original &= module->xex_module()->TranslateCode(at) ==
                      memory.TranslateVirtual<const uint8_t*>(at);
        }
        ctor_sha =
            Sha256(memory.TranslateVirtual<const uint8_t*>(0x816BCCA8), 96);
      }
    }
    const auto domain_root = c.domain->root();
    const std::array<bool, 12> fe_checks{
        (readable & 1) && words[0] == 0xFE,
        (readable & 2) && words[1] == 0xAAAABBBB,
        (readable & 4) && words[2] == 0x816BD930,
        (readable & 8) && words[3] == 1,
        caller.domain == c.domain,
        owner_begin.get() == module.get(),
        owner_end.get() == module.get(),
        code_section,
        code_readable,
        original,
        ctor_sha ==
            "67e2768dffed4e98d377274c666b60c967ce8c20313a3d058bed75da5ac4bc4e",
        passed == kAllChecks};
    uint32_t fe_failed = 0;
    for (size_t i = 0; i < fe_checks.size(); ++i) {
      if (!fe_checks[i]) {
        fe_failed |= 1u << i;
      }
    }
    XELOGI(
        "Native first-record FE ownership: passed={:03X} failed={:03X} "
        "fe_failed={:03X} policy={} tid={:08X} root={:08X}/{} "
        "calls_generation={} "
        "domain={}/root={:08X}/{} runtime_generation={}",
        passed, kAllChecks & ~passed, fe_failed, static_cast<uint32_t>(policy),
        thread->thread_id(), module ? module->hmodule_ptr() : 0,
        module ? module->load_generation() : 0,
        c.catalog->calls()->load_generation(), c.domain->id(),
        domain_root.hmodule, domain_root.generation,
        c.allocation->owner().module_generation);
    XELOGI(
        "Native first-record FE fields: readable={:02X} app={:08X} "
        "sentinel={:08X} callback={:08X} initialized={:08X} "
        "hud_cell={:08X} root_self={:08X} callback_module={:08X} "
        "callback_generation={} callback_domain={}",
        readable, words[0], words[1], words[2], words[3], words[4], words[5],
        caller.module ? caller.module->hmodule_ptr() : 0,
        caller.module ? caller.module->load_generation() : 0,
        caller.domain ? caller.domain->id() : 0);
    XELOGI(
        "Native first-record FE constructor: begin_owner={} end_owner={} "
        "code={} readable={} original={} sha256={}",
        owner_begin.get() == module.get(), owner_end.get() == module.get(),
        code_section, code_readable, original, ctor_sha);
    XELOGI(
        "Native first-record HUD child: hmodule={:08X} generation={} "
        "domain={} ready={} attached={} name={} path={} "
        "loaded_hash={:016X} loaded_code_sha1={} origin={} source_sha256={}",
        hold.hud ? hold.hud->hmodule_ptr() : 0,
        hold.hud ? hold.hud->load_generation() : 0,
        hold.hud ? hold.hud->native_import_domain_id() : 0,
        hold.hud && hold.hud->native_import_ready(),
        hold.hud && hold.hud->is_attached(), hold.hud ? hold.hud->name() : "",
        hold.hud ? hold.hud->path() : "",
        hold.hud && hold.hud_lease && hold.hud->hash() ? *hold.hud->hash() : 0,
        hold.hud && hold.hud_lease ? hold.hud->xex_module()->image_code_sha1()
                                   : "",
        origin, source_sha);
  } catch (...) {
    // Observation must neither replace failure nor unwind the native Task.
  }
}
}  // namespace

NativeHudModuleOwnership ObserveNativeHudModuleOwnership(
    const NativeSystemUiServiceContext& c, NativeHudOriginPolicy policy) {
  NativeHudModuleOwnership result;
  if (!c.kernel || !c.catalog || !c.domain || !c.allocation) {
    return result;
  }
  auto hold = std::make_shared<Lifetime>();
  hold->operation = c.domain->Acquire();
  hold->root = c.catalog->calls()->module();
  uint64_t passed = 0;
  const auto pass = [&](unsigned bit, bool value) {
    if (value) {
      passed |= uint64_t(1) << bit;
    }
    return value;
  };
  auto& memory = *c.kernel->memory();
  uint32_t hud_hmodule = 0;
  if (hold->root &&
      pass(0, hold->root->module_format() == UserModule::kModuleFormatXex &&
                  hold->root->load_generation() ==
                      c.catalog->calls()->load_generation())) {
    const auto root = c.domain->root();
    const auto profile_owner = c.domain->profile()->owner();
    pass(1, hold->operation && c.domain->active() &&
                root ==
                    NativeImportDomain::Member{hold->root->hmodule_ptr(),
                                               hold->root->load_generation()} &&
                c.domain->Contains(root) && hold->root->native_import_ready() &&
                hold->root->native_import_domain_id() == c.domain->id());
    const auto& owner = c.allocation->owner();
    pass(2, c.allocation->IsAccepting() && c.allocation->kernel() == c.kernel &&
                c.allocation->root_module().get() == hold->root.get() &&
                c.allocation->root_load_generation() == root.generation &&
                c.allocation->MatchesDomain(c.domain) &&
                profile_owner.hmodule == owner.module_hmodule &&
                profile_owner.generation == owner.module_generation);
    hold->root_lease = c.kernel->AcquireUserModuleExecutionLease(hold->root);
    pass(3, bool(hold->root_lease));
    pass(4, hold->root_lease &&
                Word(memory, kSelfCell) == hold->root->hmodule_ptr());
    // Resolve the real HMODULE through the existing typed object table only
    // after checking every loader-entry byte GetFromHModule may access.
    auto lock = global_critical_region::AcquireDirect();
    const auto cell = Word(memory, kHudCell);
    if (cell && *cell &&
        memory.QueryRangeAccess(*cell, sizeof(X_LDR_DATA_TABLE_ENTRY)) !=
            xe::memory::PageAccess::kNoAccess) {
      hud_hmodule = *cell;
      auto module =
          XModule::GetFromHModule(c.kernel, memory.TranslateVirtual(*cell));
      if (module && module->hmodule_ptr() == *cell &&
          module->module_type() == XModule::ModuleType::kUserModule) {
        hold->hud = retain_object(static_cast<UserModule*>(module.get()));
      }
    }
    if (pass(5,
             hold->hud && hold->hud.get() != hold->root.get() &&
                 hold->hud->module_format() == UserModule::kModuleFormatXex)) {
      hold->hud_lease = c.kernel->AcquireUserModuleExecutionLease(hold->hud);
      pass(6, bool(hold->hud_lease));
      if (hold->hud_lease) {
        pass(7, hold->hud->load_generation() &&
                    hold->hud->native_import_ready() &&
                    hold->hud->native_import_domain_id() == c.domain->id() &&
                    c.domain->Contains(
                        {hud_hmodule, hold->hud->load_generation()}) &&
                    hold->hud->is_system_app_module() &&
                    hold->hud->is_dll_module() && hold->hud->is_attached());
        pass(8, hold->hud->hash() == UINT64_C(0x83B2411DBA4B3FA5) &&
                    hold->hud->xex_module()->base_address() == 0x913E0000 &&
                    hold->hud->xex_module()->image_size() == 0x49000 &&
                    hold->hud->entry_point() == 0x913F8D00 &&
                    hold->hud->xex_module()->image_code_sha1().size() == 40);
      }
    }
  }
  // File/VFS work stays outside Kernel's global region. Read-only SystemRoot
  // may be the explicit disposable first-record namespace; production still
  // requires its actual installed Flash device and exact current entries.
  std::string origin, source_sha;
  if (hold->root_lease && hold->hud_lease) {
    auto* fs = c.kernel->file_system();
    auto* system = fs ? fs->ResolvePath("\\SystemRoot") : nullptr;
    auto* entry = fs ? fs->ResolvePath("\\SystemRoot\\hud.xex") : nullptr;
    auto* actual = fs ? fs->ResolvePath(hold->hud->path()) : nullptr;
    auto* root_entry = fs ? fs->ResolvePath(hold->root->path()) : nullptr;
    auto* flash = fs ? fs->GetDevice("\\Device\\Flash") : nullptr;
    const bool diagnostic =
        policy == NativeHudOriginPolicy::kFirstRecordSystemRoot &&
        cvars::native_system_first_record_probe;
    if (entry) {
      origin = entry->absolute_path();
    }
    const bool origin_ok = system && entry && actual == entry && root_entry &&
                           entry->parent() == system &&
                           root_entry->parent() == system &&
                           system->device()->is_read_only() &&
                           entry->device() == system->device() &&
                           root_entry->device() == system->device() &&
                           (diagnostic || (flash && system->device() == flash));
    pass(9, origin_ok);
    if (origin_ok && entry->size() == 118784 && entry->can_map()) {
      auto mapped = entry->OpenMapped(MappedMemory::Mode::kRead);
      if (mapped && mapped->size() == 118784) {
        source_sha = Sha256(mapped->data(), mapped->size());
        pass(10, source_sha == kHudSourceSha);
      }
    }
    auto lock = global_critical_region::AcquireDirect();
    pass(11,
         c.domain->active() && c.allocation->IsAccepting() &&
             c.allocation->MatchesDomain(c.domain) &&
             Word(memory, kHudCell) == hud_hmodule &&
             c.domain->root() ==
                 NativeImportDomain::Member{hold->root->hmodule_ptr(),
                                            hold->root->load_generation()} &&
             c.domain->Contains(c.domain->root()) &&
             hold->root->native_import_ready() &&
             hold->root->native_import_domain_id() == c.domain->id() &&
             c.domain->Contains({hud_hmodule, hold->hud->load_generation()}) &&
             hold->hud->native_import_ready() &&
             hold->hud->native_import_domain_id() == c.domain->id());
  }
  Trace(c, *hold, passed, policy, origin, source_sha);
  result.established = passed == kAllChecks;
  result.observation = std::move(hold);
  return result;
}

}  // namespace xe::kernel
