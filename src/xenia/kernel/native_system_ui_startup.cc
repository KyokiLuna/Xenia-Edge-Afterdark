/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia contributors. All rights reserved.                    *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */
#include "xenia/kernel/native_system_ui_startup.h"
#include <algorithm>
#include <array>
#include <map>
#include <mutex>
#include <string_view>
#include <tuple>
#include <utility>
#include "third_party/mbedtls/include/mbedtls/sha256.h"
#include "third_party/tomlplusplus/toml.hpp"
#include "xenia/base/byte_order.h"
#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/base/mutex.h"
#include "xenia/base/native_system_ui_trace.h"
#include "xenia/cpu/export_resolver.h"
#include "xenia/cpu/guest_call_abi.h"
#include "xenia/cpu/processor.h"
#include "xenia/emulator.h"
#include "xenia/kernel/guest_runtime_call_executor.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/native_import_domain.h"
#include "xenia/kernel/native_system_allocation_authority.h"
#include "xenia/kernel/native_system_content.h"
#include "xenia/kernel/native_system_input_data.h"
#include "xenia/kernel/user_module.h"
#include "xenia/kernel/xthread.h"
#include "xenia/memory.h"
DECLARE_bool(native_system_ui_trace);

namespace xe::kernel {
namespace {
using Phase = NativeSystemUiStartupPhase;
using Policy = ValidatedGuestRuntimePlan::ResultPolicy;
// Execution context is independent of stable diagnostic phase identifiers.
// Only these exact versioned entries enter the once-only startup records.
enum class ExecutionStage { kBootstrap, kNativeTask };
struct Entry {
  const char* identity;
  uint32_t address, length;
  const char* hash;
  Policy policy;
  uint32_t arguments;
  Phase phase;
  ExecutionStage execution;
};
constexpr Entry kEntries[] = {
    {"native-system-task-init", 0x816BA650, 8,
     "dd52e9d5e756776da71d52e0f10d9bc377c157f44ab90e1f3be2489b62473cd1",
     Policy::kHResult, 0, Phase::kTask, ExecutionStage::kBootstrap},
    {"native-system-shared-graphics-init", 0x816C9EA0, 332,
     "cb83632e14ed701d35d42f24e69f5f5bdbe828a25661ada005b51d44d9eff471",
     Policy::kHResult, 0, Phase::kSharedGraphics, ExecutionStage::kBootstrap},
    {"native-system-config-init", 0x816A17B8, 100,
     "d1b3c9104383a0716785b61c2994bd00c15c9be201a76d5b1940e5fc644ab3cd",
     Policy::kNonzero, 0, Phase::kConfig, ExecutionStage::kBootstrap},
    {"native-system-notify-init", 0x816A95A0, 220,
     "bccb7e72058b7ce42fe7f8e2ccb25eaf78966838564c5413c80d3d89e09b45fa",
     Policy::kVoid, 0, Phase::kNotifications, ExecutionStage::kBootstrap},
    {"native-system-input-aux-init", 0x816F44D0, 396,
     "dec3c2185ca94413e1f2e08ea5f1b69436535fd2e85ca27bd1d6c3a318456ad9",
     Policy::kHResult, 0, Phase::kInputAuxiliary, ExecutionStage::kBootstrap},
    {"native-system-update-predicate", 0x8169D060, 24,
     "d6c583f1d7af8bee8eae05d3d9890747a454596b8e4c4a9795611f244b9c87c7",
     Policy::kAny, 0, Phase::kUpdatePredicate, ExecutionStage::kNativeTask},
    {"native-system-fe-init", 0x816BDE90, 172,
     "7689eff393cd3d203d2de6f2b6564b7a2c2649715a6849b41ac25b0974c5a7b8",
     Policy::kHResult, 1, Phase::kFeRegistry, ExecutionStage::kNativeTask},
    {"native-system-ui-manager-init", 0x816CEBD8, 192,
     "99f827bd62230d14b2a5704c3d7f1f340f448d185eb0eaf258f39794e0275f0e",
     Policy::kHResult, 0, Phase::kUiManager, ExecutionStage::kNativeTask},
};
struct Anchor {
  uint32_t address, length;
  const char* hash;
};
// These bodies prove the wrapper, manager/DC symbols, async state-loop route
// and imported hardware flag origin. No private object layout is invented.
constexpr Anchor kAnchors[] = {
    {0x816B7F30, 124,
     "e27edff9c72a8b767aa6cde43ff3d474899fc12efb54c356813fad097f056414"},
    {0x816B8098, 48,
     "e6c9e34282cfbf21ced6b5aa991148d7e52fd0daeaa76782f4850444c1a39b57"},
    {0x816B7B80, 24,
     "bae8c269c47e9baa51a35f7e1663f73501e0ace0306e34e2d55fee6114dde1c8"},
    {0x816BA440, 232,
     "54499cbb31f67e590a4c36e36708688274d2c696f1a03529bd26782955c73fee"},
    {0x816BA668, 72,
     "6f6895fe529c0718cf03788f4b5d5aed2863028fdeda2bb484b9b19a81dd1686"},
    {0x816BA6B0, 8,
     "7078c2eb244a8db92cf1675de972d286cd11b8454203eb98b765ca135dcea927"},
    {0x816B9AA8, 268,
     "994ea4590f49fec02d97874cddc397889d2f88742be12c4e2f1c3ce732f0319b"},
    {0x816BA660, 8,
     "adf01425cd86f55f5deef9388c3a9794fa2af896730a3a0b0987d2fa6101c6f3"},
    {0x816BA058, 200,
     "f287e8c224459f78372dab57c9b18f7b347a0f3467fcd4337fa873260ef49c0d"},
    {0x816B7B98, 68,
     "d774cb5de9cb20e86eda0cc42317b7838590393aa495e700fcc2b468e3b24199"},
    {0x816B8B08, 152,
     "1e6e216190b9d6046af3325bf16deb0e22ff9409b457f920a67bfbb1b756c2de"},

    {0x816B9748, 324,
     "72506e54e11ba77d38d3d634f5bc97027478f88a6f76a8891565aa5941e8702b"},
    {0x816BA120, 400,
     "24796a8ca283d928384f557a960d04ba8731010e25e7da3fe59818beffe4822d"},
    {0x816B9668, 224,
     "c4fbaedefac18a52bf66338990e32f4b80c5d28b11bfb8ae53c6fff00329eae5"},
    {0x816CE8B0, 804,
     "caedda02e6001448ea4bb773a4986f2f4fe27cfa8bcc83014150921970d45494"},
    {0x816C8328, 296,
     "f92f68e758c525b2ad127c092f782539af94c2e0b68f8c0170b63adf426935fd"},
    {0x81696558, 592,
     "2274bb755437b3b81ae716552b93cd87c81d95b108efb4f47a127f66b65edd4d"},
    {0x8183EA40, 216,
     "86c64388db95f9bf5b198ad9fc67f7e568e608378bfe7929307e91d14ad929f9"},
};
// Full supplied native class0 closure and original registration witness.
// These are provenance checks, not executable fragments or new boot roles.
constexpr Anchor kInputAnchors[] = {
    {0x816D8A58, 76,
     "4314bebee568900d71b6080a1f51ad1f6962d58b1ce58b53ab1a5adc328e6a32"},
    {0x816FB668, 92,
     "6176fd764aeced87f0b0953b7d6a418b001e1e2435add135809fc4104bd3133a"},
    {0x816F18C0, 104,
     "152eadac4f88698cbc88bf6ff22a734ee3c453c488613fd58415e76ec7eb5909"},
    {0x816F1928, 96,
     "c7865fa2728e3c5ae98f78fda98e66e5691c0050316bf95c2e96b59f29cfad24"},
    {0x816F1988, 44,
     "7178c67b87b33e541dc4c89526358dcca295df1d5cd8aebff3702f24134a65be"},
    {0x816F2768, 120,
     "75f14c0e412623d764f89009b763135371ac2b7e3aa4f7db8b13f11a658b41f0"},
    {0x816F0410, 500,
     "1a4a94ede38d539ecf44a12598e13ee95005b0f499f7f4629bf1b2dde8478189"},
    {0x816F4088, 524,
     "66817ce97ef82bddc29d4844b602204d5fd897f88a7dcad22636f054d2e7907b"},

    {0x816D8F50, 268,
     "06041d724753596609b1c2989c05e671b3a62165b00901fce021bc1bb372fc75"},
    {0x816F27E0, 20,
     "4fb4fcdf31b089c233c51ad1094fcb5bf304cbe42dd4179378226f8ab455b1e6"},
    {0x816F4410, 192,
     "b81a455df3c75fe41617efd2a9aa90aab38138cab068c6a08a2b74d988c9a9a6"},
    {0x816d8b10, 124,
     "3feb95871a19070e062f7cb8e33f81204966c9056fb9623bf6b4a4caafa67f9f"},
    {0x816d9060, 44,
     "cd890dde58927b2f8a48a61ebf0acc54be6fe40da86a285fb7e528086db56ba3"},
    {0x816d8cd8, 632,
     "3df7f90fde97d2faa445901ba893dfb1e451ca9a3de51ef28583dc1bb6ca6d79"},
    {0x816d9090, 136,
     "44160c02e8471b999626496f366d234f415d1a226a53e89397a4e09b48c4da6f"},
    {0x816f27f8, 84,
     "344c7ccbb8a68af29b169cb61d43cb569a4db6751556ec344c93be73341ed3a2"},
    {0x816fb8b0, 144,
     "718141e93bb8454afa293ef24fa136836c044bc6dffaffe7555d075d32e90fc6"},
    {0x816fb738, 168,
     "20dfd0357f340511f9a8709548cd14cf823b1057191d6b8967df4aa23b2688fc"},
    {0x816f44d0, 396,
     "dec3c2185ca94413e1f2e08ea5f1b69436535fd2e85ca27bd1d6c3a318456ad9"},
    {0x816f4298, 372,
     "502b557223b13d5176d0fb2e25f1d6fa5640802f15ec6a0e7bf66515228010c6"},
    {0x816968a8, 584,
     "d8fa06c3ce8fe42009d6fdc12d449aefc99cacb5ebeee6f36d789ee6fbeb026f"},
};
std::string Digest(const uint8_t* bytes, size_t count) {
  std::array<uint8_t, 32> sum;
  mbedtls_sha256(bytes, count, sum.data(), 0);
  constexpr char hex[] = "0123456789abcdef";
  std::string result;
  for (auto byte : sum) {
    result.push_back(hex[byte >> 4]);
    result.push_back(hex[byte & 15]);
  }
  return result;
}
GuestRuntime::ExecutionResult Fail(std::string detail) {
  return {false, 0, std::move(detail)};
}
bool SameRoot(const GuestRuntimeCallExecutor& calls,
              const NativeSystemAllocationAuthority& allocation) {
  const auto module = calls.module();
  const auto heap = calls.heap_runtime()->GetSnapshot();
  return allocation.IsAccepting() && allocation.kernel() == calls.kernel() &&
         allocation.root_module().get() == module.get() &&
         allocation.root_load_generation() == calls.load_generation() &&
         allocation.owner().module_hmodule == heap.identity.hmodule &&
         allocation.owner().module_generation == heap.identity.generation;
}
bool CodeHash(const GuestRuntimeCallExecutor& calls, uint32_t at,
              uint32_t length, std::string_view expected) {
  auto module = calls.module();
  auto* image = module->xex_module();
  auto* memory = calls.kernel()->memory();
  if (at < image->base_address() ||
      uint64_t(at) + length >
          uint64_t(image->base_address()) + image->image_size() ||
      memory->QueryRangeAccess(at, length) == memory::PageAccess::kNoAccess) {
    return false;
  }
  for (uint64_t page = at; page < uint64_t(at) + length;
       page = (page & ~uint64_t(0xFFF)) + 0x1000) {
    xex2_section_type type;
    if (!image->GetPageSectionType(uint32_t(page), &type) ||
        type != XEX_SECTION_CODE) {
      return false;
    }
  }
  return Digest(memory->TranslateVirtual<const uint8_t*>(at), length) ==
         expected;
}
bool Data(const GuestRuntimeCallExecutor& calls, uint32_t at) {
  xex2_section_type type;
  return calls.module()->xex_module()->GetPageSectionType(at, &type) &&
         (type == XEX_SECTION_DATA || type == XEX_SECTION_READONLY_DATA) &&
         calls.kernel()->memory()->QueryRangeAccess(at, 4) !=
             memory::PageAccess::kNoAccess;
}
std::optional<uint32_t> Word(Memory* memory, uint32_t at) {
  if (!at || (at & 3) ||
      memory->QueryRangeAccess(at, 4) == memory::PageAccess::kNoAccess) {
    return {};
  }
  return xe::load_and_swap<uint32_t>(memory->TranslateVirtual(at));
}
Phase ContentPhase(NativeSystemContentCall call) {
  switch (call) {
    case NativeSystemContentCall::kInitializeStorage:
      return Phase::kSystemStorageInitialized;
    case NativeSystemContentCall::kCompleteStorageBoot:
      return Phase::kSystemStorageBootCompleted;
    case NativeSystemContentCall::kAttachSource:
      return Phase::kSystemContentSource;
    case NativeSystemContentCall::kInitializeIndex:
      return Phase::kSystemContentIndex;
    case NativeSystemContentCall::kLookup:
      return Phase::kSystemContentLookup;
  }
  return Phase::kPrepareSeams;
}
}  // namespace
struct NativeSystemUiStartupCatalog::State {
  std::shared_ptr<GuestRuntimeCallExecutor> calls;
  std::shared_ptr<NativeSystemAllocationAuthority> allocation;
  GuestRuntime::Plan plan;
  GuestRuntime::Plan early_plan, frontend_plan;
  NativeSystemUiStartupSymbols symbols;
  GuestRuntime::Function factory;
  NativeSystemStartupTaskFunctions task_functions;
  GuestRuntime::Function frontend_callback;
  std::string provenance;
  std::shared_ptr<NativeSystemContent> system_content;
};
NativeSystemUiStartupCatalog::NativeSystemUiStartupCatalog(
    std::unique_ptr<State> state)
    : state_(std::move(state)) {}
NativeSystemUiStartupCatalog::~NativeSystemUiStartupCatalog() = default;
std::shared_ptr<const NativeSystemUiStartupCatalog>
NativeSystemUiStartupCatalog::Load(
    const std::shared_ptr<GuestRuntimeCallExecutor>& calls,
    const std::shared_ptr<NativeSystemAllocationAuthority>& allocation,
    std::string* error) {
  const auto fail = [&](const char* reason) {
    if (error) {
      *error = reason;
    }
    return std::shared_ptr<const NativeSystemUiStartupCatalog>{};
  };
  if (!calls || !allocation || !SameRoot(*calls, *allocation)) {
    return fail("Exact startup root/allocation generation required");
  }
  auto module = calls->module();
  auto* image = module->xex_module();
  if (!module->hash() || *module->hash() != 0xDD9939F8B910C18Dull ||
      image->base_address() != 0x815F0000 || image->image_size() != 0x610000 ||
      image->image_code_sha1() != "D8F94A9D7D09F731B12215B7BAB23847564F06AE") {
    return fail("Only the validated supplied 17559 image profile is admitted");
  }
  // Heap's immutable init plan may not already contain this service family.
  for (const auto& phase : calls->validated().plan.phases) {
    for (const auto& function : phase.functions) {
      for (const auto& entry : kEntries) {
        if (function.address == entry.address) {
          return fail("Service call is already a heap initializer");
        }
      }
      for (const auto& entry : NativeSystemContentProfiles()) {
        if (function.address == entry.address) {
          return fail("System content call is already a heap initializer");
        }
      }
    }
  }
  auto state = std::make_unique<State>();
  state->calls = calls;
  state->allocation = allocation;
  state->plan.identity = "17559-native-system-ui-service-startup-v2-notify";
  const auto content_profiles = NativeSystemContentProfiles();
  const auto family =
      CheckNativeSystemContentPlanFamily(calls->validated(), error);
  if (family == NativeSystemContentPlanFamily::kInvalid) {
    return {};
  }
  const bool content_enabled =
      family == NativeSystemContentPlanFamily::kComplete;
  std::vector<GuestRuntime::Function> content_functions;
  if (content_enabled) {
    for (const auto& profile : content_profiles) {
      const auto& function =
          calls->validated().catalog.at(std::string(profile.identity));
      const auto contract =
          calls->validated().contracts.find(std::string(profile.identity));
      if (contract == calls->validated().contracts.end() ||
          function.address != profile.address ||
          function.arguments !=
              std::vector<uint64_t>(profile.argument_count, 0) ||
          function.return_kind != (profile.returns_void
                                       ? GuestRuntime::ReturnKind::kVoid
                                       : GuestRuntime::ReturnKind::kHResult) ||
          contract->second.caller !=
              ValidatedGuestRuntimePlan::CallerPolicy::kSystemTaskOnly ||
          contract->second.result !=
              (profile.returns_void ? Policy::kVoid : Policy::kHResult) ||
          !contract->second.before.empty() || !contract->second.after.empty() ||
          !CodeHash(*calls, profile.address, profile.code_size,
                    profile.code_sha256)) {
        return fail(
            "Native system-content startup prototype/contract mismatch");
      }
      content_functions.push_back(function);
    }
    state->system_content =
        NativeSystemContent::Create(calls, allocation, error);
    if (!state->system_content) {
      return {};
    }
    state->plan.identity = "17559-native-system-hdd-startup-v2-storage-driver";
  }
  GuestRuntime::Phase phase{"native-owned-service-startup", {}};
  for (const auto& entry : kEntries) {
    // Preserve original native Main ordering: source/index setup precedes FE;
    // the queued public lookup waits until FE is genuinely initialized.
    if (content_enabled && entry.phase == Phase::kUpdatePredicate) {
      phase.functions.push_back(content_functions[0]);
      phase.functions.push_back(content_functions[1]);
      phase.functions.push_back(content_functions[2]);
      phase.functions.push_back(content_functions[3]);
    }
    if (content_enabled && entry.phase == Phase::kUiManager) {
      phase.functions.push_back(content_functions[4]);
    }
    const auto found = calls->validated().catalog.find(entry.identity);
    const auto contract = calls->validated().contracts.find(entry.identity);
    if (found == calls->validated().catalog.end() ||
        contract == calls->validated().contracts.end() ||
        found->second.address != entry.address ||
        found->second.arguments.size() != entry.arguments ||
        contract->second.result != entry.policy ||
        contract->second.caller !=
            (entry.execution == ExecutionStage::kBootstrap
                 ? ValidatedGuestRuntimePlan::CallerPolicy::kSystemOnly
                 : ValidatedGuestRuntimePlan::CallerPolicy::kSystemTaskOnly) ||
        !CodeHash(*calls, entry.address, entry.length, entry.hash)) {
      return fail(
          "Validated deferred startup prototype/code/result contract mismatch");
    }
    // FE's one runtime DWORD is supplied only from the actual native predicate;
    // the immutable plan preserves its prototype, not a forged constant result.
    if (entry.arguments && found->second.arguments[0] != 0) {
      return fail("Unexpected FE prototype placeholder");
    }
    auto logical = found->second;
    if (entry.phase == Phase::kUiManager) {
      logical.abi_metadata +=
          "\nservice_condition=actual_XboxHardwareInfo_flags_0x200; "
          "completion_is_conditional_not_constructor_readiness";
    }
    phase.functions.push_back(std::move(logical));
  }
  for (const auto& anchor : kAnchors) {
    if (!CodeHash(*calls, anchor.address, anchor.length, anchor.hash)) {
      return fail("Native startup/data-symbol provenance body mismatch");
    }
  }
  const std::tuple<const char*, uint32_t, uint32_t, Policy,
                   GuestRuntime::Function*>
      task_entries[] = {
          {"native-system-task-schedule", 0x816BA440, 4, Policy::kHResult,
           &state->task_functions.schedule},
          {"native-system-task-create", 0x816BA668, 1, Policy::kHResult,
           &state->task_functions.create},
          {"native-system-task-modify", 0x816BA6B0, 5, Policy::kAny,
           &state->task_functions.modify},
          {"native-system-task-wait", 0x816BA660, 1, Policy::kAny,
           &state->task_functions.wait},
          {"native-system-close-task", 0x816B7B80, 1, Policy::kAny,
           &state->task_functions.close},
          {"native-system-current-task", 0x816B8098, 0, Policy::kAny,
           &state->task_functions.current},
          {"native-system-task-status", 0x816B7B98, 1, Policy::kAny,
           &state->task_functions.status},
  };
  for (const auto& [name, address, count, policy, output] : task_entries) {
    const auto entry = calls->validated().catalog.find(name);
    const auto contract = calls->validated().contracts.find(name);
    if (entry == calls->validated().catalog.end() ||
        contract == calls->validated().contracts.end() ||
        entry->second.address != address ||
        entry->second.arguments.size() != count ||
        contract->second.caller !=
            ValidatedGuestRuntimePlan::CallerPolicy::kSystemOnly ||
        contract->second.result != policy) {
      return fail("Actual native Task transport/reference contract missing");
    }
    *output = entry->second;
  }
  const auto callback =
      calls->validated().catalog.find("native-system-frontend-continuation");
  const auto callback_contract =
      calls->validated().contracts.find("native-system-frontend-continuation");
  if (callback == calls->validated().catalog.end() ||
      callback_contract == calls->validated().contracts.end() ||
      callback->second.address != 0x81696558 ||
      callback->second.arguments.size() != 1 ||
      callback_contract->second.caller !=
          ValidatedGuestRuntimePlan::CallerPolicy::kSystemTaskOnly) {
    return fail("Verified private-root frontend callback prototype missing");
  }
  state->frontend_callback = callback->second;
  const auto factory =
      calls->validated().catalog.find("native-light-device-factory");
  if (factory == calls->validated().catalog.end() ||
      factory->second.address != 0x8183EA40 ||
      factory->second.arguments != std::vector<uint64_t>{0, 2, 0, 0, 0, 0}) {
    return fail("Shared factory prototype/layout catalogue missing");
  }
  const auto end =
      factory->second.abi_metadata.rfind("\nverified_plan_sha256=");
  if (end == std::string::npos) {
    return fail("Shared factory metadata unverified");
  }
  toml::table metadata;
  try {
    metadata = toml::parse(factory->second.abi_metadata.substr(0, end));
  } catch (...) {
    return fail("Shared factory layout metadata malformed");
  }
  const auto* layout = metadata["system_device_layout"].as_table();
  const std::pair<const char*, int64_t> fields[] = {{"version", 1},
                                                    {"device_size", 0x6080},
                                                    {"reference_count", 0x3C},
                                                    {"system_flag", 0x2B0C},
                                                    {"light_flags", 0x2B3C},
                                                    {"light_mask", 0x80},
                                                    {"creator_thread", 0x2B08},
                                                    {"use_counter", 0x2B1C},
                                                    {"gpu_identifier", 0x2B10},
                                                    {"normal_table", 0x5F58},
                                                    {"mode_rva", 0x4E30F8}};
  if (!layout) {
    return fail("Shared factory layout absent");
  }
  for (const auto& [key, value] : fields) {
    if ((*layout)[key].value<int64_t>() != value) {
      return fail("Shared factory layout differs from validated profile");
    }
  }
  state->factory = factory->second;
  state->early_plan.identity = "17559-native-system-early-startup-v3-notify";
  state->frontend_plan.identity = "17559-native-system-frontend-task-v2";
  GuestRuntime::Phase early{"actual-bootstrap", {}},
      frontend{"one-actual-native-task", {}};
  for (const auto& function : phase.functions) {
    const auto role = std::find_if(std::begin(kEntries), std::end(kEntries),
                                   [&](const Entry& entry) {
                                     return function.identity == entry.identity;
                                   });
    (role != std::end(kEntries) && role->execution == ExecutionStage::kBootstrap
         ? early.functions
         : frontend.functions)
        .push_back(function);
  }
  state->early_plan.phases.push_back(std::move(early));
  state->frontend_plan.phases.push_back(std::move(frontend));
  state->plan.phases.push_back(std::move(phase));
  // Proven directly by full hashed constructor/shared-init functions above.
  state->symbols = {0x81AA20E0,
                    0x81AA2110,
                    0x81AA1B14,
                    0x81AA1B00,
                    {0x81AA1B04, 0x81AA1B08, 0x81AA1B0C},
                    0x81AA20E4,
                    0x81AA2108,
                    0x81A82570,
                    0x816B9668,
                    0x81A9F850,
                    {0x81AAC270, 0x81AAC274, 0x81AAC29C, 0x81AAC298}};
  for (auto address :
       {state->symbols.manager_base, state->symbols.dc_cell,
        state->symbols.shared_device_cell, state->symbols.record_context_cell,
        state->symbols.manager_task_cell,
        state->symbols.manager_startup_task_cell, 0x815F0480u, 0x81A82570u,
        0x81A9F850u, 0x81AAC270u, 0x81AAC274u, 0x81AAC29Cu, 0x81AAC298u}) {
    if (!Data(*calls, address)) {
      return fail("Native startup symbol is outside actual root data");
    }
  }
  for (auto address : state->symbols.command_buffer_cells) {
    if (!Data(*calls, address)) {
      return fail("Native command backing cell invalid");
    }
  }
  std::string provenance;
  for (const auto& anchor : kAnchors) {
    provenance += anchor.hash;
  }
  state->provenance = Digest(
      reinterpret_cast<const uint8_t*>(provenance.data()), provenance.size());
  auto catalog = std::shared_ptr<const NativeSystemUiStartupCatalog>(
      new NativeSystemUiStartupCatalog(std::move(state)));
  if (!catalog->ReadHardwareFlags()) {
    return fail("XboxHardwareInfo import/actual variable mapping invalid");
  }
  if (error) {
    error->clear();
  }
  return catalog;
}
GuestRuntime::Plan NativeSystemUiStartupCatalog::PreparedTitleHeapPlan(
    const NativeSystemPreparedTitle& title) const {
  GuestRuntime::Plan plan;
  plan.identity = "17559-prepared-title-heap-v1";
  GuestRuntime::Function function;
  function.identity = "native-prepared-title-heap-initialize";
  function.address = 0x816E6870;
  function.return_kind = GuestRuntime::ReturnKind::kUInt32;
  const auto owner = state_->allocation->owner();
  function.abi_metadata =
      "fixed17559:noargs:NTSTATUS0:retain-until-whole-Kernel-stop:" +
      std::to_string(owner.kernel_epoch) + ":" +
      std::to_string(owner.module_hmodule) + ":" +
      std::to_string(owner.module_generation) + ":" +
      std::to_string(state_->allocation->root_load_generation()) + ":" +
      std::to_string(title.foreground_generation) + ":" +
      std::to_string(title.hmodule) + ":" +
      std::to_string(title.load_generation) + ":" +
      std::to_string(title.main_thread_id) + ":" +
      "9d80e24f0dc87f1897b8e99e1aa567ba1a84dad23ffb833b2b9b54e04467d63d";
  plan.phases.push_back(
      {"actual-suspended-prepared-title", {std::move(function)}});
  return plan;
}

GuestRuntime::ExecutionResult
NativeSystemUiStartupCatalog::InitializePreparedTitleHeap(
    XThread& bootstrap, const NativeSystemPreparedTitle& expected) const {
  const auto& calls = *state_->calls;
  auto* kernel = calls.kernel();
  auto* memory = kernel->memory();
  auto* emulator = kernel->emulator();
  const auto actual_caller = bootstrap.native_frame_thread_identity();
  if (!expected.valid() || !SameRoot(calls, *state_->allocation) ||
      !XThread::IsInThread(&bootstrap) || actual_caller.kernel != kernel ||
      actual_caller.process_type != X_PROCTYPE_SYSTEM ||
      actual_caller.thread_id != state_->allocation->bootstrap_thread_id() ||
      !emulator || !emulator->OwnsTitleExecution(kernel)) {
    return Fail("Actual prepared-Title/bootstrap/root identity invalid");
  }
  auto title = kernel->GetExecutableModule();
  auto title_code =
      title ? kernel->AcquireUserModuleExecutionLease(title) : nullptr;
  auto main = kernel->GetThreadByID(expected.main_thread_id);
  auto root = calls.module();
  const auto domain_caller = kernel->ResolveNativeImportCaller(0x816E6874);
  auto operation = domain_caller.domain ? domain_caller.domain->Acquire()
                                        : NativeImportDomain::Operation{};
  if (!title || !title_code || !main || main->kernel_state() != kernel ||
      main->guest_object<X_KTHREAD>()->process_type != X_PROCTYPE_TITLE ||
      !main->suspend_count() || title.get() == root.get() ||
      title->module_format() != UserModule::kModuleFormatXex ||
      domain_caller.module.get() != root.get() || !operation ||
      !state_->allocation->MatchesDomain(domain_caller.domain)) {
    return Fail("Actual suspended Title/module/native domain unavailable");
  }
  const auto same_title = [&] {
    const auto foreground = emulator->title_execution_state().snapshot();
    const auto current = kernel->GetExecutableModule();
    return emulator->OwnsTitleExecution(kernel) &&
           foreground.phase == TitleExecutionState::Phase::kPreparing &&
           foreground.hmodule == expected.hmodule && main->suspend_count() &&
           SameNativeSystemPreparedTitle(
               expected,
               {foreground.generation, current ? current->hmodule_ptr() : 0,
                current ? current->load_generation() : 0,
                foreground.main_thread_id});
  };
  // Profile addresses are compiled and bounded, never supplied by a public
  // caller or copied from an unvalidated plan. Preserve old923 TOML/31 calls.
  constexpr Anchor profile[] = {
      {0x816E6870, 196,
       "9d80e24f0dc87f1897b8e99e1aa567ba1a84dad23ffb833b2b9b54e04467d63d"},
      {0x816E5B28, 268,
       "26354d03ba9f7ecd61dad696bf65bd0d57bc313f6eb427d2b7bb6c8bbf366727"},
      {0x816E6218, 216,
       "3c4833576fd9bd753a9c4c256c91de29ced15132f63300dd7556d5de0ffc94ef"},
      {0x8171F060, 1468,
       "f7a524ff1ed6a859c6aac14180fe8157563e0aba4bcc5fac95638d7a414bd582"},
      {0x816E6160, 60,
       "e3391e95f87fd034bb15886190ede911ec9f0550e85840c6afce7a984c1814b3"},
      {0x816E5CD8, 128,
       "96111bcccf014e6b0e122a3e3d586fd7c28ef8269db70f4ccd72ba53daa58a7a"},
      {0x816E5C38, 156,
       "eaea856dfab8fedeb4c0506f3915df45cdfab48376c77b6338b2356ee9be9b43"},
  };
  uint32_t requested_reserve = 0x60000;
  {
    auto lock = xe::global_critical_region::AcquireDirect();
    if (!same_title()) {
      return Fail("Prepared Title changed before native heap initialization");
    }
    for (const auto& anchor : profile) {
      if (!CodeHash(calls, anchor.address, anchor.length, anchor.hash)) {
        return Fail("Native prepared-Title heap closure changed");
      }
      for (uint32_t at = anchor.address; at < anchor.address + anchor.length;
           at += 4) {
        if (kernel->GetUserModuleForAddress(at).get() != root.get() ||
            root->xex_module()->TranslateCode(at) !=
                memory->TranslateVirtual<const uint8_t*>(at)) {
          return Fail("Native Title heap closure has a foreign owner/overlay");
        }
      }
    }
    for (uint32_t at : {0x81AA3EE0u, 0x81AABFF8u, 0x81AABFFCu, 0x81AAC000u,
                        0x81AAC004u, 0x81AAC008u, 0x81AAC00Cu, 0x815F063Cu}) {
      if (!Data(calls, at) ||
          kernel->GetUserModuleForAddress(at).get() != root.get()) {
        return Fail("Native Title heap data/import has no exact root owner");
      }
    }
    const auto* executable =
        kernel->processor()->export_resolver()->GetExportByOrdinal(
            "xboxkrnl.exe", 0x193);
    bool exact_import = false;
    for (const auto& library : *root->xex_module()->import_libraries()) {
      if (library.name != "xboxkrnl") {
        continue;
      }
      for (const auto& imported : library.imports) {
        exact_import |= imported.ordinal == 0x193 &&
                        imported.value_address == 0x815F063C &&
                        !imported.thunk_address;
      }
    }
    // Preserve the exact validation and short-circuit read order while
    // distinguishing a loader-metadata refusal from either actual data cell.
    if (!executable || executable->get_type() != cpu::Export::Type::kVariable ||
        !executable->variable_ptr) {
      return Fail("Kernel XexExecutableModuleHandle data export unavailable");
    }
    if (!exact_import) {
      return Fail("Native XexExecutableModuleHandle import metadata changed");
    }
    if (Word(memory, 0x815F063C) != executable->variable_ptr) {
      return Fail("Native XexExecutableModuleHandle import cell mismatch");
    }
    if (Word(memory, executable->variable_ptr) != expected.hmodule) {
      return Fail(
          "Kernel XexExecutableModuleHandle cell does not identify prepared "
          "Title");
    }
    const std::tuple<uint32_t, const char*, uint32_t> imports[] = {
        {0x12B, "RtlImageXexHeaderField", 0x81A725D4},
        {0xCC, "NtAllocateVirtualMemory", 0x81A72FA4},
        {0xDC, "NtFreeVirtualMemory", 0x81A72FC4},
        {0xE1, "NtProtectVirtualMemory", 0x81A72774}};
    for (const auto& [ordinal, name, thunk] : imports) {
      const auto* binding =
          kernel->processor()->export_resolver()->GetExportByOrdinal(
              "xboxkrnl.exe", ordinal);
      bool imported_exactly = false;
      for (const auto& library : *root->xex_module()->import_libraries()) {
        if (library.name != "xboxkrnl") {
          continue;
        }
        for (const auto& imported : library.imports) {
          imported_exactly |=
              imported.ordinal == ordinal && imported.thunk_address == thunk;
        }
      }
      if (!binding || binding->get_type() != cpu::Export::Type::kFunction ||
          (binding->tags & cpu::ExportTag::kStub) ||
          !binding->function_data.trampoline ||
          std::string_view(binding->name) != name || !imported_exactly) {
        return Fail("Native Title heap Kernel import contract unavailable");
      }
    }
    // Same optional DWORD40201 consumed by6870 via real LDR+58/header lookup.
    uint32_t header = 0;
    if (XSUCCEEDED(
            title->GetOptHeader(XEX_HEADER_TITLE_WORKSPACE_SIZE, &header))) {
      const auto bytes = Word(memory, header);
      if (!bytes) {
        return Fail("Actual Title heap-size header is unreadable");
      }
      const uint64_t rounded = (uint64_t(*bytes) + 0x3FFF) & ~uint64_t(0x3FFF);
      if (rounded > UINT32_MAX) {
        return Fail("Native Title heap size overflows");
      }
      requested_reserve = uint32_t(std::max<uint64_t>(0x40000, rounded));
    }
    if (memory->QueryRangeAccess(0x81AABFF8, 24) !=
            memory::PageAccess::kReadWrite ||
        !NativeSystemTitleHeapIsPristine(ReadNativeSystemTitleHeapData(*memory),
                                         root->hmodule_ptr())) {
      return Fail(
          "Native Title heap0 is not pristine; reinitialization denied");
    }
  }
  // No host metadata/Kernel/Global lock crosses PPC. Actual native6870 owns
  // allocation and cleanup. No Task, manual heap fields or success proxy.
  const auto called = cpu::ExecuteGuestCall(
      kernel->processor(), bootstrap.thread_state(), 0x816E6870, {});
  const auto data = ReadNativeSystemTitleHeapData(*memory);
  const bool healthy =
      called.executed && called.value == 0 && same_title() &&
      SameRoot(calls, *state_->allocation) &&
      NativeSystemTitleHeapHasBacking(*memory, data, root->hmodule_ptr(),
                                      requested_reserve);
  if (cvars::native_system_ui_trace) {
    static xe::NativeSystemUiTraceLimiter trace;
    trace.Emit(
        "input", "title_heap_initialization",
        {{"foreground_generation", expected.foreground_generation},
         {"title_hmodule", expected.hmodule},
         {"title_load_generation", expected.load_generation},
         {"main_thread_id", expected.main_thread_id},
         {"root_hmodule", root->hmodule_ptr()},
         {"root_load_generation", calls.load_generation()},
         {"executed", called.executed},
         {"raw_result", called.value},
         {"requested_reserve_bytes", requested_reserve},
         {"heap_identity", data.words[0]},
         {"heap_handle", data.words[1]},
         {"reserved_base", data.words[2]},
         {"reserved_bytes", data.words[3]},
         {"packed_flags", data.words[4]},
         // Descriptor word5 is forwarded unchanged as NtAllocateVirtualMemory
         // argument5. Actual caller process identity is checked separately.
         {"nt_allocate_fifth_argument", data.words[5]},
         {"postcondition_passed", healthy}});
  }
  return {healthy, called.value,
          healthy ? std::string{}
                  : "Native Title heap execution/postcondition uncertain"};
}

const GuestRuntime::Plan& NativeSystemUiStartupCatalog::plan() const {
  return state_->plan;
}
const NativeSystemStartupTaskFunctions&
NativeSystemUiStartupCatalog::task_functions() const {
  return state_->task_functions;
}
const GuestRuntime::Function&
NativeSystemUiStartupCatalog::frontend_callback_entry() const {
  return state_->frontend_callback;
}
const GuestRuntime::Plan& NativeSystemUiStartupCatalog::early_plan() const {
  return state_->early_plan;
}
const GuestRuntime::Plan& NativeSystemUiStartupCatalog::frontend_plan() const {
  return state_->frontend_plan;
}
const std::shared_ptr<GuestRuntimeCallExecutor>&
NativeSystemUiStartupCatalog::calls() const {
  return state_->calls;
}
const std::shared_ptr<NativeSystemAllocationAuthority>&
NativeSystemUiStartupCatalog::allocation() const {
  return state_->allocation;
}
std::shared_ptr<const NativeSystemContent>
NativeSystemUiStartupCatalog::system_content() const {
  return state_->system_content;
}
const NativeSystemUiStartupSymbols& NativeSystemUiStartupCatalog::symbols()
    const {
  return state_->symbols;
}
std::optional<uint32_t> NativeSystemUiStartupCatalog::ReadHardwareFlags()
    const {
  if (!SameRoot(*state_->calls, *state_->allocation)) {
    return {};
  }
  auto* kernel = state_->calls->kernel();
  auto* memory = kernel->memory();
  const auto* exported =
      kernel->processor()->export_resolver()->GetExportByOrdinal("xboxkrnl.exe",
                                                                 0x156);
  if (!exported || exported->get_type() != cpu::Export::Type::kVariable ||
      std::string_view(exported->name) != "XboxHardwareInfo") {
    return {};
  }
  const auto bound = Word(memory, 0x815F0480);
  if (!bound || *bound != exported->variable_ptr) {
    return {};
  }
  return Word(memory, *bound);  // native guard itself reads this first DWORD
}
NativeSystemSharedGraphicsState
NativeSystemUiStartupCatalog::ReadSharedGraphicsState() const {
  NativeSystemSharedGraphicsState result;
  result.symbols = state_->symbols;
  result.factory = state_->factory;
  result.provenance_sha256 = state_->provenance;
  if (!SameRoot(*state_->calls, *state_->allocation)) {
    result.status = NativeSystemSharedGraphicsState::Status::kInvalid;
    return result;
  }
  auto* memory = state_->calls->kernel()->memory();
  auto device = Word(memory, result.symbols.shared_device_cell),
       context = Word(memory, result.symbols.record_context_cell),
       dc = Word(memory, result.symbols.dc_cell);
  if (!device || !context || !dc) {
    result.status = NativeSystemSharedGraphicsState::Status::kInvalid;
    return result;
  }
  result.device = *device;
  result.recording_context = *context;
  result.dc = *dc;
  bool any = result.device || result.recording_context || result.dc;
  for (size_t i = 0; i < 3; ++i) {
    auto value = Word(memory, result.symbols.command_buffer_cells[i]);
    if (!value) {
      result.status = NativeSystemSharedGraphicsState::Status::kInvalid;
      return result;
    }
    result.command_buffers[i] = *value;
    any |= *value != 0;
  }
  if (!any) {
    return result;
  }
  result.status = NativeSystemSharedGraphicsState::Status::kInvalid;
  if (!result.device || !result.recording_context ||
      !Word(memory, result.device) || !Word(memory, result.recording_context)) {
    return result;
  }
  for (size_t i = 0; i < 3; ++i) {
    const auto alias = result.command_buffers[i];
    auto* heap = alias ? memory->LookupHeap(alias) : nullptr;
    if (!heap || heap->heap_type() != HeapType::kGuestPhysical ||
        memory->QueryRangeAccess(alias, 0x40000) ==
            memory::PageAccess::kNoAccess) {
      return result;
    }
    const auto physical = memory->GetPhysicalAddress(alias);
    if (physical >= 0x20000000u || 0x40000u > 0x20000000u - physical) {
      return result;
    }
    result.command_ranges[i] = {physical, 0x40000};
  }
  const auto pin = memory->PinOwnedPhysicalRanges(state_->allocation->owner(),
                                                  result.command_ranges);
  if (!pin) {
    return result;
  }
  result.command_pin = pin.pin;
  if (!result.dc) {
    result.status = NativeSystemSharedGraphicsState::Status::kDcPending;
    return result;
  }
  if (!Word(memory, result.dc)) {
    return result;
  }
  result.status = NativeSystemSharedGraphicsState::Status::kObserved;
  return result;
}
bool NativeSystemUiStartupCatalog::CheckNativeNotificationInitialization(
    std::string* error) const {
  const auto fail = [&](std::string reason) {
    if (error) {
      *error = std::move(reason);
    }
    return false;
  };
  const auto& calls = *state_->calls;
  if (!SameRoot(calls, *state_->allocation)) {
    return fail("Notification root/cohort admission changed");
  }
  auto module = calls.module();
  auto* image = module->xex_module();
  auto* memory = calls.kernel()->memory();
  if (module->load_generation() != calls.load_generation() ||
      calls.kernel()->GetUserModuleForAddress(0x816A95A0).get() !=
          module.get() ||
      !CodeHash(
          calls, 0x816A95A0, 220,
          "bccb7e72058b7ce42fe7f8e2ccb25eaf78966838564c5413c80d3d89e09b45fa")) {
    return fail("Notification initializer code/generation changed");
  }
  // Guest-byte hashes cannot detect a host overlay of the proven initializer
  // or the comparer that it literally installs at 95B0/95B8/95C4.
  for (uint32_t at = 0x816A95A0; at < 0x816A967C; at += 4) {
    if (image->TranslateCode(at) !=
        memory->TranslateVirtual<const uint8_t*>(at)) {
      return fail("Notification initializer has a host overlay");
    }
  }
  xex2_section_type comparer_section;
  constexpr uint32_t comparer = 0x816A8068;
  if (calls.kernel()->GetUserModuleForAddress(comparer).get() != module.get() ||
      !image->GetPageSectionType(comparer, &comparer_section) ||
      comparer_section != XEX_SECTION_CODE ||
      memory->QueryRangeAccess(comparer, 4) == memory::PageAccess::kNoAccess ||
      image->TranslateCode(comparer) !=
          memory->TranslateVirtual<const uint8_t*>(comparer)) {
    return fail("Notification comparer is not the actual native root code");
  }
  const auto root_word = [&](uint32_t at) -> std::optional<uint32_t> {
    if (calls.kernel()->GetUserModuleForAddress(at).get() != module.get() ||
        !Data(calls, at)) {
      return {};
    }
    return Word(memory, at);
  };
  const auto mask_high = root_word(0x81AA06D0);
  const auto mask_low = root_word(0x81AA06D4);
  const auto vector = root_word(0x81AA06E4);
  const auto compare = root_word(0x81AA06E8);
  const auto capacity = root_word(0x81AA06F0);
  if (!mask_high || !mask_low || *mask_high != 0 || *mask_low != 0xBFu ||
      !vector || !*vector || (*vector & 3) || !compare ||
      *compare != comparer || !capacity || *capacity < 0x100 ||
      memory->QueryRangeAccess(*vector, 0x400) ==
          memory::PageAccess::kNoAccess) {
    return fail(
        "Native notification mask/comparer/vector allocation incomplete");
  }
  // Exact register-area calls/table and loop816A7FC8 yield these counts in a
  // pristine root. A zero selected count makes native CreateListener return
  // NULL even when this void initialization call returned normally. No listener
  // object, handle or host notification state is manufactured here.
  struct AreaCounts {
    uint32_t first_cell;
    std::array<uint32_t, 11> values;
  };
  constexpr AreaCounts areas[] = {
      {0x81A9FBD0,
       {0x26, 0x26, 0x26, 0x26, 0x26, 0x26, 0x26, 0x26, 0x26, 0x26, 0x26}},
      {0x81A9FBFC,
       {0xE, 0x11, 0x11, 0x11, 0x12, 0x12, 0x12, 0x13, 0x13, 0x13, 0x13}},
      {0x81A9FC28, {0x6, 0x6, 0x6, 0x6, 0x8, 0x8, 0x8, 0x8, 0x8, 0x8, 0x8}},
      {0x81A9FC54, {0x3, 0x4, 0x4, 0x4, 0x4, 0x4, 0x4, 0x4, 0x4, 0x4, 0x4}},
      {0x81A9FC80, {0x3, 0x3, 0x3, 0x3, 0x3, 0x3, 0x3, 0x3, 0x3, 0x3, 0x3}},
      {0x81A9FCAC, {0xC, 0xC, 0xC, 0xC, 0xC, 0xC, 0xC, 0xC, 0xC, 0xC, 0xC}},
      {0x81A9FD04, {0x1, 0x1, 0x1, 0x1, 0x5, 0x5, 0x5, 0x5, 0x5, 0x5, 0x5}},
  };
  for (const auto& area : areas) {
    for (size_t i = 0; i < area.values.size(); ++i) {
      const auto at = area.first_cell + uint32_t(i) * 4;
      if (root_word(at) != area.values[i]) {
        return fail("Native notification event count mismatch at decimal " +
                    std::to_string(at));
      }
    }
  }
  if (!SameRoot(calls, *state_->allocation)) {
    return fail("Notification cohort closed during observation");
  }
  if (error) {
    error->clear();
  }
  return true;
}

GuestRuntime::ExecutionResult NativeSystemUiStartupSeams::PrepareBeforeWorkers(
    const NativeSystemUiStartupCatalog&, XThread&) {
  return Fail("Native domain/driver/Vd prerequisite adapter missing");
}
GuestRuntime::ExecutionResult NativeSystemUiStartupSeams::ObserveAfter(
    Phase, const NativeSystemUiStartupCatalog&, XThread&) {
  return Fail("Actual native phase ownership observations missing");
}

namespace {
struct StartupTaskReference {
  std::shared_ptr<GuestRuntimeCallExecutor> calls;
  uint32_t handle = 0;
  bool owned = false, release_attempted = false;
  // No destructor PPC: uncertain/abandoned refs survive until stopped Kernel.
};
std::shared_ptr<StartupTaskReference> AcquireTask(
    const std::shared_ptr<GuestRuntimeCallExecutor>& calls, XThread& thread) {
  auto reference = std::make_shared<StartupTaskReference>();
  reference->calls = calls;
  calls->kernel()->RetainNativeResourceUntilTeardown(
      reference);  // before AddRef
  const auto value = calls->ExecuteDeferredSystemFunction(
      thread, "native-system-current-task", {});
  const uint32_t handle = uint32_t(value.return_value);
  if (!value.succeeded || handle < 0x1000 || (handle & 3) != 1) {
    return {};
  }
  reference->handle = handle;
  reference->owned = true;
  return reference;
}
bool CloseTask(XThread& thread,
               const std::shared_ptr<StartupTaskReference>& reference) {
  if (!reference || !reference->owned || reference->release_attempted) {
    return false;
  }
  reference->release_attempted = true;  // never retry an uncertain native Close
  const std::array args{reference->handle};
  const auto result = reference->calls->ExecuteDeferredSystemFunction(
      thread, "native-system-close-task", args);
  if (!result.succeeded) {
    return false;
  }
  reference->owned = false;
  return true;
}
bool SameActualTask(XThread& thread,
                    const std::shared_ptr<StartupTaskReference>& reference) {
  const auto current = AcquireTask(reference->calls, thread);
  if (!current) {
    return false;
  }
  const bool same = current->handle == reference->handle;
  return CloseTask(thread, current) && same;
}
bool Ready(GuestRuntime::InitStatus status) {
  return status == GuestRuntime::InitStatus::kReady ||
         status == GuestRuntime::InitStatus::kAlreadyReady;
}
}  // namespace

GuestRuntime::ExecutionResult
NativeSystemUiStartupSeams::ExecuteFrontendOnNativeTaskAndWait(
    const NativeSystemUiStartupCatalog&, XThread&, FrontendWork) {
  return Fail("Actual native Task continuation transport is not installed");
}
NativeSystemUiStartupRuntime::NativeSystemUiStartupRuntime(
    std::shared_ptr<const NativeSystemUiStartupCatalog> catalog,
    std::shared_ptr<GuestRuntime> early, std::shared_ptr<GuestRuntime> frontend)
    : catalog_(std::move(catalog)),
      early_runtime_(std::move(early)),
      frontend_runtime_(std::move(frontend)) {}
std::shared_ptr<NativeSystemUiStartupRuntime>
NativeSystemUiStartupRuntime::Create(
    std::shared_ptr<const NativeSystemUiStartupCatalog> catalog) {
  if (!catalog || !SameRoot(*catalog->calls(), *catalog->allocation())) {
    return {};
  }
  static std::mutex mutex;
  using Key = std::tuple<uint64_t, uint64_t, uint64_t, uint32_t>;
  static std::map<Key, std::weak_ptr<NativeSystemUiStartupRuntime>> records;
  const auto owner = catalog->allocation()->owner();
  const Key key{owner.kernel_epoch, owner.module_generation, owner.session_id,
                owner.module_hmodule};
  std::lock_guard lock(mutex);
  for (auto it = records.begin(); it != records.end();) {
    if (it->second.expired()) {
      it = records.erase(it);
    } else {
      ++it;
    }
  }
  if (auto old = records[key].lock()) {
    if (old->catalog_->plan() != catalog->plan()) {
      catalog->allocation()->CloseAdmission();
      return {};
    }
    return old;
  }
  const auto identity =
      catalog->calls()->heap_runtime()->GetSnapshot().identity;
  auto early = std::make_shared<GuestRuntime>(identity, catalog->calls());
  auto frontend = std::make_shared<GuestRuntime>(identity, catalog->calls());
  auto startup = std::shared_ptr<NativeSystemUiStartupRuntime>(
      new NativeSystemUiStartupRuntime(catalog, std::move(early),
                                       std::move(frontend)));
  catalog->calls()->kernel()->RetainNativeResourceUntilTeardown(startup);
  records[key] = startup;
  return startup;
}
GuestRuntime::ExecutionResult NativeSystemUiStartupRuntime::InitializeFrontend(
    XThread& thread, const std::shared_ptr<NativeSystemUiStartupSeams>& seams) {
  const auto self = shared_from_this();
  const auto catalog = catalog_;
  const auto fail = [&](std::string detail) {
    catalog->allocation()->CloseAdmission();
    return Fail(std::move(detail));
  };
  if (!SameRoot(*catalog->calls(), *catalog->allocation()) ||
      !XThread::IsInThread(&thread) ||
      thread.native_frame_thread_identity().kernel !=
          catalog->calls()->kernel() ||
      thread.native_frame_thread_identity().process_type != X_PROCTYPE_SYSTEM) {
    return fail(
        "Frontend callback is not the actual native SYSTEM Task worker");
  }
  const auto task = AcquireTask(catalog->calls(), thread);
  if (!task) {
    return fail("Actual native GetCurrentTask owned-reference check failed");
  }
  std::optional<uint32_t> predicate;
  try {
    const auto status = frontend_runtime_->Initialize(
        catalog->frontend_plan(), [&](const GuestRuntime::Function& function) {
          if (!SameRoot(*catalog->calls(), *catalog->allocation()) ||
              !SameActualTask(thread, task)) {
            return fail("Frontend changed its actual native Task/context");
          }
          const auto content_profiles = NativeSystemContentProfiles();
          const auto content_role =
              std::find_if(content_profiles.begin(), content_profiles.end(),
                           [&](const auto& entry) {
                             return function.identity == entry.identity;
                           });
          if (content_role != content_profiles.end()) {
            const auto content = catalog->state_->system_content;
            std::string error;
            const auto arguments =
                content ? content->Prepare(content_role->call, thread, &error)
                        : std::optional<std::vector<uint32_t>>{};
            if (!arguments) {
              return fail("Native system-content preparation: " + error);
            }
            // The real native storage mount already called source attach.
            // Its full data/driver/boot postconditions were checked above;
            // observe that owned nested call instead of attaching it twice.
            const auto called =
                content_role->observe_nested_call
                    ? GuestRuntime::
                          ExecutionResult{true, 0,
                                          "Observed genuine native storage "
                                          "source attachment"}
                    : catalog->calls()->ExecuteDeferredNativeTaskFunction(
                          thread, function.identity, *arguments);
            if (!called.succeeded ||
                !content->Observe(content_role->call, thread, &error)) {
              return fail("Native system-content call/observation " +
                          function.identity + ": " +
                          (called.succeeded ? error : called.detail));
            }
            const auto observed = seams->ObserveAfter(
                ContentPhase(content_role->call), *catalog, thread);
            if (!observed.succeeded || !SameActualTask(thread, task)) {
              return fail("Native system-content cohort postcondition: " +
                          observed.detail);
            }
            return called;
          }
          const Entry* role = nullptr;
          for (const auto& entry : kEntries) {
            if (function.identity == entry.identity) {
              role = &entry;
            }
          }
          if (!role || role->execution != ExecutionStage::kNativeTask) {
            return fail("Unexpected frontend role");
          }
          std::vector<uint32_t> arguments;
          if (role->phase == Phase::kFeRegistry) {
            if (!predicate) {
              return fail("Actual native update predicate missing");
            }
            arguments.push_back(*predicate == 0 ? 1u : 0u);
          }
          const auto flags = catalog->ReadHardwareFlags();
          if (!flags) {
            return fail("Actual hardware guard unreadable");
          }
          GuestRuntime::ExecutionResult called;
          if (role->phase == Phase::kUiManager && !(*flags & 0x200)) {
            called = {
                true, 0,
                "Native hardware condition does not enter UI constructor"};
          } else {
            called = catalog->calls()->ExecuteDeferredNativeTaskFunction(
                thread, function.identity, arguments);
          }
          if (!called.succeeded) {
            catalog->allocation()->CloseAdmission();
            called.detail = "Native frontend call " + function.identity + ": " +
                            called.detail;
            return called;
          }
          if (role->phase == Phase::kUpdatePredicate) {
            if (called.return_value > 1) {
              return fail("Native predicate violated boolean ABI");
            }
            predicate = uint32_t(called.return_value);
          }
          const auto observed =
              seams->ObserveAfter(role->phase, *catalog, thread);
          if (!observed.succeeded) {
            return fail("Frontend actual ownership postcondition: " +
                        observed.detail);
          }
          if (!SameActualTask(thread, task)) {
            return fail("Frontend observer changed native Task");
          }
          return called;
        });
    // Explicit healthy-context release before native callback returns. No PPC
    // release is attempted by an exception/forced-unwind destructor.
    if (!SameActualTask(thread, task) || !CloseTask(thread, task)) {
      return fail("Native Task reference release uncertain");
    }
    return Ready(status) ? GuestRuntime::ExecutionResult{true, 0, {}}
                         : fail("Frontend transaction failed/retired");
  } catch (...) {
    catalog->allocation()->CloseAdmission();
    throw;
  }
}
namespace {
// Caller owns the global critical region and a real module execution lease.
// No Driver/InputSystem/Service lock crosses these observations.
bool CheckInputClosureLocked(const GuestRuntimeCallExecutor& calls,
                             const object_ref<UserModule>& module,
                             std::string* error) {
  const auto fail = [&](const char* text) {
    if (error) {
      *error = text;
    }
    return false;
  };
  auto* memory = calls.kernel()->memory();
  for (const auto& anchor : kInputAnchors) {
    if (!CodeHash(calls, anchor.address, anchor.length, anchor.hash)) {
      return fail("Native input registration/consumer body changed");
    }
    for (uint32_t at = anchor.address; at < anchor.address + anchor.length;
         at += 4) {
      if (calls.kernel()->GetUserModuleForAddress(at).get() != module.get() ||
          module->xex_module()->TranslateCode(at) !=
              memory->TranslateVirtual<const uint8_t*>(at)) {
        return fail("Native input closure has an overlay or foreign owner");
      }
    }
  }
  // Exact loader import records identify the native callsites; implemented
  // adapters are mandatory for newly activated Aux output consumers too.
  const std::pair<uint32_t, const char*> required[] = {
      {0x20D, "DrvSetUserBindingCallback"},
      {0x28F, "DrvSetDeviceConfigChangeCallback"},
      {0x1EC, "XInputdGetDeviceStats"},
      {0x385, "XInputdGetFailedConnectionOrBind"},
      {0x386, "XInputdSetFailedConnectionOrBindCallback"}};
  for (const auto& [ordinal, name] : required) {
    const auto* binding =
        calls.kernel()->processor()->export_resolver()->GetExportByOrdinal(
            "xboxkrnl.exe", ordinal);
    if (!binding || binding->get_type() != cpu::Export::Type::kFunction ||
        (binding->tags & cpu::ExportTag::kStub) ||
        !binding->function_data.trampoline ||
        std::string_view(binding->name) != name) {
      return fail("Native input required Kernel protocol unavailable");
    }
    bool found = false;
    for (const auto& library : *module->xex_module()->import_libraries()) {
      // XexModule stores the extension-free base name in ImportLibrary.
      if (library.name != "xboxkrnl") {
        continue;
      }
      for (const auto& imported : library.imports) {
        if (imported.ordinal == ordinal && imported.thunk_address) {
          if ((ordinal == 0x20D && imported.thunk_address != 0x81A72984) ||
              (ordinal == 0x28F && imported.thunk_address != 0x81A72974) ||
              (ordinal == 0x1EC && imported.thunk_address != 0x81A73014) ||
              (ordinal == 0x385 && imported.thunk_address != 0x81A73074) ||
              (ordinal == 0x386 && imported.thunk_address != 0x81A72944)) {
            return fail("Native input registration import target changed");
          }
          found = true;
        }
      }
    }
    if (!found) {
      return fail("Native input exact Kernel import absent");
    }
  }
  // Every observed data span belongs to this exact loaded native root.
  const std::pair<uint32_t, uint32_t> spans[] = {
      {0x81A82FF4, 0x24}, {0x81AA3D90, 0x100}, {0x81A854B0, 0x58},
      {0x81AAC4B0, 4},    {0x81A85C88, 4},     {0x81AACBF0, 4}};
  for (const auto& [at, length] : spans) {
    for (uint32_t word = at; word < at + length; word += 4) {
      if (!Data(calls, word) ||
          calls.kernel()->GetUserModuleForAddress(word).get() != module.get()) {
        return fail("Native input data has no exact root ownership");
      }
    }
  }
  for (const auto& [at, bytes] :
       {std::pair<uint32_t, uint32_t>{0x81AA3D90, 0x100},
        {0x81A854B0, 0x58},
        {0x81AAC4B0, 4}}) {
    if (memory->QueryRangeAccess(at, bytes) != memory::PageAccess::kReadWrite) {
      return fail("Native mutable input data is not guest writable");
    }
  }
  const auto data = ReadNativeSystemInputBindingData(*memory);
  if (data.status != NativeSystemInputData::Status::kValid) {
    if (error) {
      *error = "Native input actual data unsupported: status=" +
               std::to_string(uint32_t(data.status)) +
               " known=" + std::to_string(data.known) +
               " table=" + std::to_string(data.user_table) +
               " class_table=" + std::to_string(data.class_table) +
               " class0=" + std::to_string(data.class0_callback) +
               " voice=" + std::to_string(data.voice_gate) + "/" +
               std::to_string(data.voice_user);
    }
    return false;
  }
  return true;
}
}  // namespace
bool NativeSystemUiStartupCatalog::CheckInputCallbackContext(
    std::string* error) const {
  const auto& calls = *state_->calls;
  if (!SameRoot(calls, *state_->allocation)) {
    if (error) {
      *error = "Native input root/runtime retired";
    }
    return false;
  }
  const auto module = calls.module();
  const auto lease = calls.kernel()->AcquireUserModuleExecutionLease(module);
  if (!lease) {
    if (error) {
      *error = "Native input code lease unavailable";
    }
    return false;
  }
  auto memory_lock = global_critical_region::AcquireDirect();
  return CheckInputClosureLocked(calls, module, error);
}
std::optional<NativeSystemInputCallbackProfile>
NativeSystemUiStartupCatalog::ReadInputCallbackBootstrap(
    XThread& thread, std::string* error) const {
  const auto fail =
      [&](const char* text) -> std::optional<NativeSystemInputCallbackProfile> {
    if (error) {
      *error = text;
    }
    return {};
  };
  if (!XThread::IsInThread(&thread) ||
      thread.kernel_state() != state_->calls->kernel() ||
      thread.thread_id() != state_->allocation->bootstrap_thread_id() ||
      thread.native_frame_thread_identity().process_type != X_PROCTYPE_SYSTEM) {
    return fail(
        "Native input registration requires the actual SYSTEM bootstrap");
  }
  const auto& calls = *state_->calls;
  if (!SameRoot(calls, *state_->allocation)) {
    return fail("Native input root/runtime retired");
  }
  const auto module = calls.module();
  const auto lease = calls.kernel()->AcquireUserModuleExecutionLease(module);
  if (!lease) {
    return fail("Native input code lease unavailable");
  }
  auto memory_lock = global_critical_region::AcquireDirect();
  if (!CheckInputClosureLocked(calls, module, error)) {
    return {};
  }
  const auto data =
      ReadNativeSystemInputBootstrapData(*state_->calls->kernel()->memory());
  if (data.status != NativeSystemInputData::Status::kValid) {
    memory_lock.unlock();
    XELOGE(
        "Native input bootstrap data: status={} known={:X} table={:08X} "
        "class_table={:08X} class0={:08X} voice={:08X}/{} "
        "event={:08X}/{:08X}/{:08X}/{:08X} timer={:08X} dpc={:08X}/{:08X} "
        "failure={}",
        uint32_t(data.status), data.known, data.user_table, data.class_table,
        data.class0_callback, data.voice_gate, data.voice_user, data.event[0],
        data.event[1], data.event[2], data.event[3], data.timer[0], data.dpc[0],
        data.dpc[3], data.failure_flag);
    return fail("Native input bootstrap records/event were not pristine");
  }
  return NativeSystemInputCallbackProfile{0x816D9060, 0x816D9090, 0x816F27E0};
}
NativeSystemUiStartupRuntime::Status NativeSystemUiStartupRuntime::Initialize(
    XThread& bootstrap, std::shared_ptr<NativeSystemUiStartupSeams> seams) {
  const auto self = shared_from_this();
  const auto catalog = catalog_;
  const auto caller = bootstrap.native_frame_thread_identity();
  if (!catalog || !seams ||
      !SameRoot(*catalog->calls(), *catalog->allocation()) ||
      !XThread::IsInThread(&bootstrap) ||
      caller.kernel != catalog->calls()->kernel() ||
      caller.process_type != X_PROCTYPE_SYSTEM ||
      caller.thread_id != catalog->allocation()->bootstrap_thread_id()) {
    if (catalog) {
      catalog->allocation()->CloseAdmission();
    }
    return Status::kPoisoned;
  }
  const auto fail = [&](std::string detail) {
    catalog->allocation()->CloseAdmission();
    return Fail(std::move(detail));
  };
  try {
    bool prepared = false;
    const auto early = early_runtime_->Initialize(
        catalog->early_plan(), [&](const GuestRuntime::Function& function) {
          if (!SameRoot(*catalog->calls(), *catalog->allocation()) ||
              catalog->calls()->heap_runtime()->GetSnapshot().state !=
                  GuestRuntime::State::kReady) {
            return fail("Heap/early bootstrap admission invalid");
          }
          if (!prepared) {
            const auto ready = seams->PrepareBeforeWorkers(*catalog, bootstrap);
            if (!ready.succeeded) {
              return fail("Pre-worker native seam prerequisites: " +
                          ready.detail);
            }
            if (!catalog->ReadHardwareFlags()) {
              return fail("Actual hardware mapping changed");
            }
            prepared = true;
          }
          const Entry* role = nullptr;
          for (const auto& entry : kEntries) {
            if (function.identity == entry.identity) {
              role = &entry;
            }
          }
          if (!role || role->execution != ExecutionStage::kBootstrap) {
            return fail("Unexpected early role");
          }
          const auto called = catalog->calls()->ExecuteDeferredSystemFunction(
              bootstrap, function.identity, {});
          if (!called.succeeded) {
            return fail("Native early call: " + called.detail);
          }
          const auto observed =
              seams->ObserveAfter(role->phase, *catalog, bootstrap);
          return observed.succeeded
                     ? called
                     : fail("Early native ownership postcondition: " +
                            observed.detail);
        });
    if (early == GuestRuntime::InitStatus::kBusy ||
        early == GuestRuntime::InitStatus::kReentrant) {
      return Status::kBusy;
    }
    if (!Ready(early)) {
      catalog->allocation()->CloseAdmission();
      return Status::kPoisoned;
    }
    {
      std::lock_guard lock(dispatch_mutex_);
      if (frontend_runtime_->GetSnapshot().state ==
          GuestRuntime::State::kReady) {
        return catalog->allocation()->IsAccepting()
                   ? Status::kAlreadyBootAdmitted
                   : Status::kPoisoned;
      }
      if (frontend_dispatch_claimed_) {
        return Status::kBusy;
      }
      frontend_dispatch_claimed_ =
          true;  // never repeat an unknown/native dispatch
    }
    const auto result = seams->ExecuteFrontendOnNativeTaskAndWait(
        *catalog, bootstrap, [self, seams](XThread& actual_task) {
          return self->InitializeFrontend(actual_task, seams);
        });
    if (!result.succeeded ||
        frontend_runtime_->GetSnapshot().state != GuestRuntime::State::kReady) {
      // A missing/failed transport poisons the frontend record without running
      // any native function on the ordinary SYSTEM bootstrap.
      frontend_runtime_->Initialize(catalog->frontend_plan(), [&](const auto&) {
        return fail("Actual native Task continuation failed: " + result.detail);
      });
      catalog->allocation()->CloseAdmission();
      return Status::kPoisoned;
    }
    return catalog->allocation()->IsAccepting() ? Status::kBootAdmitted
                                                : Status::kPoisoned;
  } catch (...) {
    catalog->allocation()->CloseAdmission();
    throw;
  }
}
NativeSystemUiStartupRuntime::Snapshot NativeSystemUiStartupRuntime::snapshot()
    const {
  return {early_runtime_->GetSnapshot(), frontend_runtime_->GetSnapshot()};
}
bool NativeSystemUiStartupRuntime::IsBootAdmitted() const {
  return early_runtime_->GetSnapshot().state == GuestRuntime::State::kReady &&
         frontend_runtime_->GetSnapshot().state ==
             GuestRuntime::State::kReady &&
         catalog_->allocation()->IsAccepting();
}
GuestRuntime::DrainStatus NativeSystemUiStartupRuntime::RetireAndDrain(
    bool allow_wait) {
  // Seal both records before any wait. No per-function Task handoff or native
  // cleanup is issued here; the service owns actual continuation/worker joins.
  const auto early = early_runtime_->RetireAndDrain(false);
  const auto frontend = frontend_runtime_->RetireAndDrain(false);
  if (early == GuestRuntime::DrainStatus::kSelfCall ||
      frontend == GuestRuntime::DrainStatus::kSelfCall) {
    return GuestRuntime::DrainStatus::kSelfCall;
  }
  if (!allow_wait && (early != GuestRuntime::DrainStatus::kDrained ||
                      frontend != GuestRuntime::DrainStatus::kDrained)) {
    return GuestRuntime::DrainStatus::kWouldBlock;
  }
  const auto a = early_runtime_->RetireAndDrain(allow_wait);
  const auto b = frontend_runtime_->RetireAndDrain(allow_wait);
  return a != GuestRuntime::DrainStatus::kDrained ? a : b;
}
}  // namespace xe::kernel
