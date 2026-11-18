/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia contributors. All rights reserved.                    *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */
#include "xenia/kernel/native_system_content.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <tuple>

#include "third_party/mbedtls/include/mbedtls/sha256.h"
#include "xenia/base/byte_order.h"
#include "xenia/base/mutex.h"
#include "xenia/cpu/export_resolver.h"
#include "xenia/cpu/processor.h"
#include "xenia/cpu/xex_module.h"
#include "xenia/kernel/guest_runtime_call_executor.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/native_content_storage_driver.h"
#include "xenia/kernel/native_import_domain.h"
#include "xenia/kernel/native_system_allocation_authority.h"
#include "xenia/kernel/user_module.h"
#include "xenia/kernel/xam/content_manager.h"
#include "xenia/kernel/xthread.h"
#include "xenia/memory.h"
#include "xenia/vfs/devices/host_path_device.h"
#include "xenia/vfs/devices/host_path_entry.h"
#include "xenia/vfs/file.h"
#include "xenia/vfs/virtual_file_system.h"

namespace xe::kernel {
namespace {
constexpr uint32_t kManager = 0x81AAC110;
constexpr uint32_t kStorageTable = 0x81A9CB30;
constexpr uint32_t kStorageWorker = 0x81A9CB28;
constexpr uint32_t kStorageNotificationWorker = 0x81AA279C;
constexpr uint32_t kStorageCounter = 0x81A81328;
constexpr uint32_t kStorageWorkEvent = 0x81A81348;
constexpr uint32_t kStorageAck = 0x81A81358;
constexpr uint32_t kStorageBoot = 0x81A9E0F4;
constexpr std::string_view kBuiltinHdd = "\\Device\\Harddisk0\\Partition1\\";
constexpr std::string_view kVolume = "\\Device\\Harddisk0\\SystemExtPartition";
constexpr std::string_view kPrefix =
    "\\Device\\Harddisk0\\SystemExtPartition\\Content\\";
constexpr char kMember[] = "AvatarAssetPack.toc";
constexpr char kProbeRoot[] = "\\System??\\XeniaNativeIndexProbe";
constexpr uint32_t kScratchBytes = 0x200, kOutputOffset = 0x40;
constexpr uint32_t kCapacity = 0x104, kAuxOffset = 0x150;
constexpr NativeSystemContentCallProfile kProfiles[] = {
    {NativeSystemContentCall::kInitializeStorage,
     "native-system-content-storage-initialize", 0x8167ED90, 192,
     "5f307d586e49332ee2e8846c7a2a0e4edcb19377310d0d4ba821d57ffe910938", 0,
     true},
    {NativeSystemContentCall::kCompleteStorageBoot,
     "native-system-content-storage-boot-complete", 0x816964A8, 172,
     "dd837757da053cffb3b06436971f2acd9eab5b0f464e5a85ef97d0e4676109ce", 0},
    {NativeSystemContentCall::kAttachSource,
     "native-system-content-source-attach", 0x816E9A58, 28,
     "a2599d519d0d1c4914759521605dd39db9c141f304f7c388fe54e1260fc32f6b", 3,
     false, true},
    {NativeSystemContentCall::kInitializeIndex,
     "native-system-content-index-initialize", 0x816EBDE0, 12,
     "154d22e30f059cf00c8ee2a07efbe5cdf3a7a7e315f5b9fb424272e822caf567", 0},
    {NativeSystemContentCall::kLookup, "native-system-content-index-lookup",
     0x816E8D40, 112,
     "cb6b5ad7303c2e667fbbbd42ad3058c4317feff68bfab4ec2efe2fe3a69f9e9f", 4},
};
struct Anchor {
  uint32_t address, size;
  std::string_view hash;
};
constexpr Anchor kAnchors[] = {
    {0x8167B700, 20,
     "725ad74f7d8c53c5b44d139e21ec885f6af1a7ef0ec1ecf98e68929901eddcaf"},
    {0x8167B718, 96,
     "4e388da554fa00d06310c8828ee5cde853f13ec5bb783bdfc34178b886aeeb3d"},
    {0x8167EB60, 560,
     "ecd409041b47ee3d8e2d59cf9dab12edc2cd2329d57153a6cb189b0d2d449caa"},
    {0x8167E918, 236,
     "80a945fa4531bb54b2c4a2a0b1461d6274c70e670d8c5b501bad2086c7bc8fb7"},
    {0x8167E5B8, 420,
     "57eab2570ab0cfb1b7b71112d5ee8419ef4fdda9f3f313dbc28ddecf2b7f5724"},
    {0x8167E760, 440,
     "200cd0da9a04924b9d78096023672b0647cfaf5b74be7e1a082556f2acbc33ff"},
    {0x8167A730, 72,
     "41b9395f3c39259819a1e8c776bf38cffd4bc0bf4b8c2018d5111034645cb6e0"},
    {0x8167B3C8, 152,
     "fc73f0b4656a06f4f8ff9491038d2f213915e175dac777353df709a68b30ab8f"},
    {0x8167E400, 436,
     "bc21460f9dc71d61f1c25179e740e8d0ee012fcd9078817cef64d5ec569685fb"},
    {0x8167B610, 104,
     "dcdc92cd6aeda0f43c3dd6c8bbcbac934d290a94fc860aaf40ce56c71c44aec4"},
    {0x816D1AB0, 88,
     "79a9bc8a89a63d39d6b48cd3ee9691eecb783a7c69a4456bfa98b02a75ad25b7"},
    {0x816D1A20, 144,
     "54ee891560f0c4d2544f7446029134b0375f14a0e4af1637022d5148e3718b24"},
    {0x8167ACF8, 200,
     "729b8b1616ac14a9775099b67203c1fc308da754a7f51c739731bab0c0c981b9"},
    {0x816E0D10, 20,
     "249aea49c318f381b016197efeaf6521f1486945c171e645070494a69aca4321"},
    {0x81A6FF90, 52,
     "216601e0a967eeaba357ccc3c5bc11db3fe1e7dc41263b8474102a1dcc12b50a"},
    {0x816EA660, 152,
     "e0f69623500af8cd3e7ceb2daa6303981c980b991c24ac190196ccc529bc607f"},
    {0x816E91B8, 300,
     "37a8db41a29961e0f1ff60ef32d621bb5cab3ce12623ae60c98eed6d041fea75"},
    {0x816E90A8, 268,
     "b98ddf4d1def4966e5514ff507e0e1b26719a6c72ccb3eadc67451626e8ef4e5"},
    {0x816E8180, 232,
     "da233fc1cac28fdbd933d8fb861d718bcf0e65ceefb0a2d61f262521ff530d1f"},
    {0x816EBBE8, 112,
     "3eccbcda9846e37668c0ec67fc957bf05cb46464464e12a3d78e8c8e3c58e921"},
    {0x816EB760, 1156,
     "c30c23a6ff0a48d44f64155acb1974debd1476e985c17dccde77659a9c6c1922"},
    {0x816EBC58, 380,
     "c6a604123394b3117a713ba9c63a13f9f88b79e65b60cf79e02eeed9220c118d"},
    {0x816E9BF8, 452,
     "d7337a2d9cbd2745054131bc6224a1ac69e2cdb054d0b660c53fe4567ef2a6ee"},
    {0x816ED9C0, 220,
     "e97a8a88113e4c3b8b16b3a8a5d194e9005506c3ed1d5a24997d7f14fc6d2923"},
    {0x816EC4B8, 352,
     "dbcda9cfe0c8c7334e48f7f83f7aea062455ca1b7e199fdab5d7d773b38f2586"},
};
std::string Digest(std::span<const uint8_t> bytes) {
  std::array<uint8_t, 32> digest;
  mbedtls_sha256(bytes.data(), bytes.size(), digest.data(), 0);
  constexpr char hex[] = "0123456789abcdef";
  std::string result;
  for (uint8_t byte : digest) {
    result.push_back(hex[byte >> 4]);
    result.push_back(hex[byte & 15]);
  }
  return result;
}
bool Code(const GuestRuntimeCallExecutor& calls, uint32_t at, uint32_t bytes,
          std::string_view hash) {
  auto module = calls.module();
  auto* memory = calls.kernel()->memory();
  if (!bytes || uint64_t(at) + bytes > (uint64_t(1) << 32) ||
      memory->QueryRangeAccess(at, bytes) == memory::PageAccess::kNoAccess) {
    return false;
  }
  for (uint32_t offset = 0; offset < bytes; offset += 4) {
    xex2_section_type section;
    if (calls.kernel()->GetUserModuleForAddress(at + offset).get() !=
            module.get() ||
        !module->xex_module()->GetPageSectionType(at + offset, &section) ||
        section != XEX_SECTION_CODE ||
        module->xex_module()->TranslateCode(at + offset) !=
            memory->TranslateVirtual<const uint8_t*>(at + offset)) {
      return false;
    }
  }
  return Digest({memory->TranslateVirtual<const uint8_t*>(at), bytes}) == hash;
}
bool SameRoot(const GuestRuntimeCallExecutor& calls,
              const NativeSystemAllocationAuthority& authority) {
  return authority.IsAccepting() && authority.kernel() == calls.kernel() &&
         authority.root_module().get() == calls.module().get() &&
         authority.root_load_generation() == calls.load_generation() &&
         calls.module()->load_generation() == calls.load_generation();
}
bool HardwarePermitsIndex(const GuestRuntimeCallExecutor& calls) {
  auto* kernel = calls.kernel();
  auto* memory = kernel->memory();
  const auto* exported =
      kernel->processor()->export_resolver()->GetExportByOrdinal("xboxkrnl.exe",
                                                                 0x156);
  if (!exported || exported->get_type() != cpu::Export::Type::kVariable ||
      std::string_view(exported->name) != "XboxHardwareInfo" ||
      memory->QueryRangeAccess(0x815F0480, 4) ==
          memory::PageAccess::kNoAccess) {
    return false;
  }
  const auto bound =
      xe::load_and_swap<uint32_t>(memory->TranslateVirtual(0x815F0480));
  if (!bound || bound != exported->variable_ptr ||
      memory->QueryRangeAccess(bound, 16) == memory::PageAccess::kNoAccess) {
    return false;
  }
  // Native Main enters index initialization only when816E0D10 returns0.
  // Preserve its actual imported hardware halfword guard; no boot-mode flag
  // or private native cell is manufactured by the host.
  return !(xe::load_and_swap<uint16_t>(memory->TranslateVirtual(bound + 0xE)) &
           0x400);
}
char Lower(char value) {
  return value >= 'A' && value <= 'Z' ? char(value + ('a' - 'A')) : value;
}
bool Equal(std::string_view a, std::string_view b) {
  return a.size() == b.size() &&
         std::equal(a.begin(), a.end(), b.begin(),
                    [](char x, char y) { return Lower(x) == Lower(y); });
}
bool Hex(std::string_view text, size_t count) {
  return text.size() == count &&
         std::all_of(text.begin(), text.end(), [](char c) {
           return (c >= '0' && c <= '9') ||
                  (Lower(c) >= 'a' && Lower(c) <= 'f');
         });
}
}  // namespace

std::span<const NativeSystemContentCallProfile> NativeSystemContentProfiles() {
  return kProfiles;
}
NativeSystemContentPlanFamily CheckNativeSystemContentPlanFamily(
    const ValidatedGuestRuntimePlan& plan, std::string* error) {
  const auto fail = [&](const char* text) {
    if (error) {
      *error = text;
    }
    return NativeSystemContentPlanFamily::kInvalid;
  };
  size_t count = 0;
  for (const auto& [identity, function] : plan.catalog) {
    if (!std::string_view(identity).starts_with("native-system-content-")) {
      continue;
    }
    const auto profile = std::find_if(
        std::begin(kProfiles), std::end(kProfiles),
        [&](const auto& item) { return item.identity == identity; });
    if (profile == std::end(kProfiles)) {
      return fail("Unknown native system-content role");
    }
    ++count;
    const auto contract = plan.contracts.find(identity);
    if (contract == plan.contracts.end() || function.identity != identity ||
        function.address != profile->address ||
        function.arguments !=
            std::vector<uint64_t>(profile->argument_count, 0) ||
        function.return_kind != (profile->returns_void
                                     ? GuestRuntime::ReturnKind::kVoid
                                     : GuestRuntime::ReturnKind::kHResult) ||
        contract->second.caller !=
            ValidatedGuestRuntimePlan::CallerPolicy::kSystemTaskOnly ||
        contract->second.result !=
            (profile->returns_void
                 ? ValidatedGuestRuntimePlan::ResultPolicy::kVoid
                 : ValidatedGuestRuntimePlan::ResultPolicy::kHResult) ||
        !contract->second.before.empty() || !contract->second.after.empty()) {
      return fail(
          "Native system-content role prototype/caller/return contract "
          "mismatch");
    }
  }
  for (const auto& phase : plan.plan.phases) {
    for (const auto& function : phase.functions) {
      for (const auto& profile : kProfiles) {
        if (function.identity == profile.identity ||
            function.address == profile.address) {
          return fail(
              "Native system-content role cannot be a heap initializer");
        }
      }
    }
  }
  if (!count) {
    return NativeSystemContentPlanFamily::kAbsent;
  }
  if (count != std::size(kProfiles)) {
    return fail("Incomplete native system-content family");
  }
  return NativeSystemContentPlanFamily::kComplete;
}
bool NativeSystemContentManagerData::PristineConstructor() const {
  if (initialized || built || index || load_task || monitor_task ||
      list_links != std::array<uint32_t, 4>{kManager + 0xC4, kManager + 0xC4,
                                            kManager + 0xCC, kManager + 0xCC}) {
    return false;
  }
  return std::all_of(sources.begin(), sources.end(), [](const Source& source) {
    return !source.path && !source.device && !source.type &&
           !source.extended_file;
  });
}
bool NativeSystemContentManagerData::HasBuiltinSource(uint32_t device) const {
  if (!device) {
    return false;
  }
  size_t matches = 0;
  for (const auto& source : sources) {
    if (source.device == device) {
      if (source.path || source.type != 1 || source.extended_file) {
        return false;
      }
      ++matches;
    }
  }
  return matches == 1;
}
std::optional<NativeSystemContentManagerData>
ReadNativeSystemContentManagerData(Memory& memory) {
  if (memory.QueryRangeAccess(kManager, 0xE0) ==
      memory::PageAccess::kNoAccess) {
    return {};
  }
  const auto word = [&](uint32_t offset) {
    return xe::load_and_swap<uint32_t>(
        memory.TranslateVirtual(kManager + offset));
  };
  NativeSystemContentManagerData result;
  result.initialized = word(0);
  result.built = word(4);
  result.index = word(0xA4);
  result.load_task = word(0xB4);
  result.monitor_task = word(0xB8);
  for (size_t i = 0; i < result.sources.size(); ++i) {
    const uint32_t at = 8 + uint32_t(i) * 16;
    result.sources[i] = {word(at), word(at + 4), word(at + 8), word(at + 12)};
  }
  for (size_t i = 0; i < result.list_links.size(); ++i) {
    result.list_links[i] = word(0xC4 + uint32_t(i) * 4);
  }
  return result;
}
bool NativeSystemStorageData::Pristine() const {
  return table_zero && next_device == 1 && !worker_task && !notification_task &&
         work_event == std::array<uint32_t, 4>{0x01000000, 0,
                                               kStorageWorkEvent + 8,
                                               kStorageWorkEvent + 8} &&
         acknowledgement ==
             std::array<uint32_t, 4>{0, 1, kStorageAck + 8, kStorageAck + 8} &&
         boot_event == std::array<uint32_t, 4>{};
}
std::optional<uint32_t> NativeSystemStorageData::FormattedBuiltinHdd() const {
  std::optional<uint32_t> found;
  for (const auto& volume : volumes) {
    if (!Equal(volume.path, kBuiltinHdd)) {
      continue;
    }
    if (found || volume.type != 1 || volume.state != 3 || !volume.device) {
      return {};
    }
    found = volume.device;
  }
  return found;
}
std::optional<NativeSystemStorageData> ReadNativeSystemStorageData(
    Memory& memory) {
  for (const auto [at, bytes] : {std::pair{kStorageTable, 0xC80u},
                                 {kStorageWorker, 8u},
                                 {kStorageNotificationWorker, 4u},
                                 {kStorageCounter, 2u},
                                 {kStorageWorkEvent, 16u},
                                 {kStorageAck, 16u},
                                 {kStorageBoot, 16u}}) {
    if (memory.QueryRangeAccess(at, bytes) != memory::PageAccess::kReadWrite) {
      return {};
    }
  }
  const auto word = [&](uint32_t at) {
    return xe::load_and_swap<uint32_t>(memory.TranslateVirtual(at));
  };
  NativeSystemStorageData result;
  result.next_device =
      xe::load_and_swap<uint16_t>(memory.TranslateVirtual(kStorageCounter));
  result.worker_task = word(kStorageWorker);
  result.notification_task = word(kStorageNotificationWorker);
  for (size_t i = 0; i < 4; ++i) {
    result.work_event[i] = word(kStorageWorkEvent + uint32_t(i) * 4);
    result.acknowledgement[i] = word(kStorageAck + uint32_t(i) * 4);
    result.boot_event[i] = word(kStorageBoot + uint32_t(i) * 4);
  }
  const auto* table = memory.TranslateVirtual<const uint8_t*>(kStorageTable);
  result.table_zero =
      std::all_of(table, table + 0xC80, [](uint8_t value) { return !value; });
  for (size_t i = 0; i < result.volumes.size(); ++i) {
    const uint32_t at = kStorageTable + uint32_t(i) * 0xA0;
    auto& volume = result.volumes[i];
    volume.device = word(at + 0x78);
    volume.type = word(at + 0x7C);
    volume.state = word(at + 0x80);
    if (!volume.state) {
      continue;
    }
    const char* start = memory.TranslateVirtual<const char*>(at);
    const auto* end = static_cast<const char*>(std::memchr(start, 0, 0x40));
    if (!end) {
      return {};
    }
    volume.path.assign(start, end);
  }
  return result;
}
std::optional<std::string> NativeSystemContentRelativePackagePath(
    std::string_view path) {
  if (path.size() <= kPrefix.size() ||
      !Equal(path.substr(0, kPrefix.size()), kPrefix)) {
    return {};
  }
  path.remove_prefix(kPrefix.size());
  std::array<std::string_view, 4> parts;
  for (size_t i = 0; i < parts.size(); ++i) {
    const auto end = path.find('\\');
    if ((i + 1 < parts.size()) != (end != std::string_view::npos)) {
      return {};
    }
    parts[i] = path.substr(0, end);
    if (end != std::string_view::npos) {
      path.remove_prefix(end + 1);
    }
  }
  if (parts[0] != "0000000000000000" || !Hex(parts[1], 8) ||
      !Hex(parts[2], 8) || !Hex(parts[3], 16)) {
    return {};
  }
  std::string relative;
  for (auto part : parts) {
    if (!relative.empty()) {
      relative += '/';
    }
    relative += part;
  }
  return relative;
}

struct NativeSystemContent::State {
  std::shared_ptr<GuestRuntimeCallExecutor> calls;
  std::shared_ptr<NativeSystemAllocationAuthority> authority;
  std::shared_ptr<vfs::HostPathDevice> volume;
  xam::ContentSource source;
  xam::ContentSourceState captured_source;
  // Cached identity checks only. Call with namespace ownership; observation,
  // callbacks and native filesystem work happen before acquiring that owner.
  bool CurrentSource() const {
    auto* kernel = calls->kernel();
    return SameRoot(*calls, *authority) && volume &&
           !volume->namespace_retiring() &&
           kernel->file_system()->AcquireDevice(kVolume) == volume &&
           kernel->content_manager()->sources().CaptureStorageVolume(
               captured_source) == captured_source.storage_volume;
  }
  uint32_t scratch = 0;
  std::mutex mutex;
  Snapshot observed;
};
NativeSystemContent::NativeSystemContent(std::unique_ptr<State> state)
    : state_(std::move(state)) {}
NativeSystemContent::~NativeSystemContent() = default;
NativeSystemContent::Snapshot NativeSystemContent::snapshot() const {
  std::lock_guard lock(state_->mutex);
  return state_->observed;
}
bool NativeSystemContent::CheckStorageDriverContext(uint32_t callback) const {
  if ((callback && callback != NativeContentStorageDriver::kNativeCallback) ||
      !SameRoot(*state_->calls, *state_->authority)) {
    return false;
  }
  const auto& calls = *state_->calls;
  const auto module = calls.module();
  const auto lease = calls.kernel()->AcquireUserModuleExecutionLease(module);
  if (!lease) {
    return false;
  }
  auto lock = global_critical_region::AcquireDirect();
  if (!state_->CurrentSource()) {
    return false;
  }
  for (const auto& anchor : kAnchors) {
    if (!Code(calls, anchor.address, anchor.size, anchor.hash)) {
      return false;
    }
  }
  for (const auto& profile : kProfiles) {
    if (!Code(calls, profile.address, profile.code_size, profile.code_sha256)) {
      return false;
    }
  }
  auto* memory = calls.kernel()->memory();
  for (const auto [at, length] : {std::pair{kStorageTable, 0xC80u},
                                  {kStorageWorker, 8u},
                                  {kStorageNotificationWorker, 4u},
                                  {kStorageCounter, 4u},
                                  {kStorageWorkEvent, 32u},
                                  {kStorageBoot, 16u},
                                  {kManager, 0xE0u},
                                  {0x81A8132Cu, 0x1Cu},
                                  {0x81A82EECu, 0x1Cu},
                                  {0x81AA27ACu, 4u}}) {
    if (memory->QueryRangeAccess(at, length) !=
        memory::PageAccess::kReadWrite) {
      return false;
    }
    for (uint32_t offset = 0; offset < length; offset += 4) {
      if (calls.kernel()->GetUserModuleForAddress(at + offset).get() !=
          module.get()) {
        return false;
      }
    }
  }
  for (const auto& [ordinal, name, thunk] :
       {std::tuple<uint32_t, const char*, uint32_t>{
            0x20E, "DrvSetContentStorageCallback", 0x81A72614},
        {0x210, "DrvGetContentStorageNotification", 0x81A72604}}) {
    const auto* binding =
        calls.kernel()->processor()->export_resolver()->GetExportByOrdinal(
            "xboxkrnl.exe", ordinal);
    if (!binding || binding->get_type() != cpu::Export::Type::kFunction ||
        !binding->is_implemented() || (binding->tags & cpu::ExportTag::kStub) ||
        !binding->function_data.trampoline ||
        std::string_view(binding->name) != name) {
      return false;
    }
    bool found = false;
    for (const auto& library : *module->xex_module()->import_libraries()) {
      if (library.name != "xboxkrnl") {
        continue;
      }
      for (const auto& imported : library.imports) {
        if (imported.ordinal == ordinal && imported.thunk_address == thunk) {
          found = true;
        }
      }
    }
    if (!found) {
      return false;
    }
  }
  if (!HardwarePermitsIndex(calls)) {
    return false;
  }
  const uint32_t hardware =
      xe::load_and_swap<uint32_t>(memory->TranslateVirtual(0x815F0480));
  // This fixed HDD profile preserves the native initializer's real guard.
  return xe::load_and_swap<uint32_t>(memory->TranslateVirtual(hardware)) & 0x20;
}
std::shared_ptr<NativeSystemContent> NativeSystemContent::Create(
    std::shared_ptr<GuestRuntimeCallExecutor> calls,
    std::shared_ptr<NativeSystemAllocationAuthority> authority,
    std::string* error) {
  const auto fail = [&](const char* text) {
    if (error) {
      *error = text;
    }
    return std::shared_ptr<NativeSystemContent>{};
  };
  if (!calls || !authority || !SameRoot(*calls, *authority)) {
    return fail("Native system-content root generation unavailable");
  }
  auto* kernel = calls->kernel();
  const auto execution = kernel->AcquireUserModuleExecutionLease(calls->module());
  if (!execution) {
    return fail("Native system-content module is retiring");
  }
  // Observe outside GCRC and the service mutex. The retained exact generation
  // below, rather than a later numeric device lookup, authorizes the read.
  const auto source = kernel->content_manager()->sources().Source("installed-system");
  if (!source || !source->available || !source->storage ||
      !source->source.read_only ||
      source->source.kind != xam::ContentSourceKind::kInstalledSystem ||
      source->source.device_type != xam::DeviceType::HDD ||
      !Equal(source->source.guest_root, std::string(kVolume) + "\\Content") ||
      !kernel->content_manager()->sources().CaptureStorageVolume(*source)) {
    return fail("Actual read-only installed source unavailable");
  }
  std::shared_ptr<vfs::HostPathDevice> volume;
  {
    auto lock = global_critical_region::AcquireDirect();
    for (const auto& anchor : kAnchors) {
      if (!Code(*calls, anchor.address, anchor.size, anchor.hash)) {
        return fail("Native system-content source/index closure changed");
      }
    }
    for (const auto& profile : kProfiles) {
      if (!Code(*calls, profile.address, profile.code_size, profile.code_sha256)) {
        return fail("Native system-content wrapper changed or overlaid");
      }
    }
    if (calls->module()->GetProcAddressByOrdinal(0x640) != 0x816E8D40 ||
        kernel->GetUserModuleForAddress(kManager).get() != calls->module().get() ||
        kernel->memory()->QueryRangeAccess(kManager, 0xE0) !=
            memory::PageAccess::kReadWrite) {
      return fail("Native content exported entry/data ownership mismatch");
    }
    volume = std::dynamic_pointer_cast<vfs::HostPathDevice>(
        kernel->file_system()->AcquireDevice(kVolume));
    if (!volume || volume->namespace_retiring() || !volume->is_read_only() ||
        !volume->storage() || !volume->storage()->read_only()) {
      return fail("Actual read-only SystemExtPartition unavailable");
    }
  }
  std::error_code ec;
  auto content_root = volume->storage()->Subroot("Content", true, ec);
  if (!content_root || ec) {
    return fail("SystemExtPartition content capability unavailable");
  }
  const auto& content_identity = content_root->directory_identity();
  const auto& source_identity = source->storage->directory_identity();
  // Both identities retain native nofollow directory pins. Compare their
  // actual root entries; paths and matching storage types are insufficient.
  if (!content_identity || !source_identity ||
      !content_identity->SameEntry({}, *source_identity, {})) {
    return fail("System volume and registered installed source differ");
  }
  // Open through the captured native root, never re-open a canonical host path
  // after namespace validation or borrow an Entry across filesystem work.
  auto manifest = volume->storage()->OpenFile(
      "system.manifest", filesystem::FileAccess::kFileReadData, ec);
  const auto info = manifest ? manifest->QueryInfo(ec) : std::nullopt;
  if (!info || ec || info->size < 0x164 || info->size > 0x10000) {
    return fail("Installed system.manifest unavailable or invalid size");
  }
  std::vector<uint8_t> bytes(static_cast<size_t>(info->size));
  const auto read = manifest->ReadResult(0, bytes);
  if (read.error || read.bytes != bytes.size() ||
      std::memcmp(bytes.data(), "XMNP", 4)) {
    return fail("Installed manifest read/magic invalid");
  }
  manifest.reset();
  {
    auto lock = global_critical_region::AcquireDirect();
    if (!SameRoot(*calls, *authority) || volume->namespace_retiring() ||
        kernel->file_system()->AcquireDevice(kVolume) != volume ||
        kernel->content_manager()->sources().CaptureStorageVolume(*source) !=
            source->storage_volume) {
      return fail("Installed source replaced during manifest read");
    }
  }
  auto state = std::make_unique<State>();
  state->calls = std::move(calls);
  state->authority = std::move(authority);
  state->volume = volume;
  state->source = source->source;
  state->captured_source = *source;
  state->observed.source_device = source->source.device_id;
  state->observed.manifest_sha256 = Digest(bytes);
  auto service = std::shared_ptr<NativeSystemContent>(
      new NativeSystemContent(std::move(state)));
  // Root/code/source observations and scratch survive all native tasks. No
  // destructor executes PPC, closes native tasks or frees a borrowed buffer.
  kernel->RetainNativeResourceUntilTeardown(service);
  return service;
}

namespace {
bool NativeBuiltinSource(Memory& memory,
                         const NativeSystemContentManagerData& data,
                         uint32_t device) {
  size_t matches = 0;
  for (const auto& source : data.sources) {
    if (source.device != device) {
      continue;
    }
    if (!source.path || source.type != 1 || source.extended_file ||
        source.path > UINT32_MAX - 64 ||
        memory.QueryRangeAccess(source.path, 64) !=
            memory::PageAccess::kReadWrite) {
      return false;
    }
    const char* start = memory.TranslateVirtual<const char*>(source.path);
    const auto* end = static_cast<const char*>(std::memchr(start, 0, 64));
    if (!end || !Equal(std::string_view(start, end - start), kBuiltinHdd)) {
      return false;
    }
    ++matches;
  }
  return device && matches == 1;
}
bool NativeStorageTasks(const NativeSystemStorageData& storage) {
  return storage.worker_task >= 0x1000 && (storage.worker_task & 3) == 1 &&
         storage.notification_task >= 0x1000 &&
         (storage.notification_task & 3) == 1 &&
         storage.worker_task != storage.notification_task;
}
}  // namespace

std::optional<std::vector<uint32_t>> NativeSystemContent::Prepare(
    NativeSystemContentCall call, XThread& thread, std::string* error) {
  const auto fail =
      [&](const char* text) -> std::optional<std::vector<uint32_t>> {
    if (error) {
      *error = text;
    }
    return {};
  };
  if (!SameRoot(*state_->calls, *state_->authority) ||
      !XThread::IsInThread(&thread) ||
      thread.kernel_state() != state_->calls->kernel() ||
      thread.native_frame_thread_identity().process_type != X_PROCTYPE_SYSTEM) {
    return fail("System content call lost its actual SYSTEM owner");
  }
  auto* kernel = state_->calls->kernel();
  const auto source = kernel->content_manager()->sources().Source(state_->source.id);
  const auto device = call == NativeSystemContentCall::kInitializeStorage
      ? xam::GetStorageDeviceInfo(kernel, state_->source.device_id)
      : std::optional<xam::StorageDeviceInfo>{};
  auto lock = global_critical_region::AcquireDirect();
  if (!HardwarePermitsIndex(*state_->calls)) {
    return fail("Actual native startup hardware mode excludes package index");
  }
  if (!state_->CurrentSource() || !source || !source->available ||
      source->storage_volume != state_->captured_source.storage_volume ||
      kernel->content_manager()->sources().CaptureStorageVolume(*source) !=
          state_->captured_source.storage_volume) {
    return fail("System content source removed/replaced");
  }
  const auto data = ReadNativeSystemContentManagerData(*kernel->memory());
  if (!data) {
    return fail("Native content manager data unreadable");
  }
  const auto observed = snapshot();
  const auto storage = ReadNativeSystemStorageData(*kernel->memory());
  const auto driver = NativeContentStorageDriver::Lookup(kernel);
  const auto caller = kernel->ResolveNativeImportCaller(
      NativeContentStorageDriver::kNativeCallback + 4);
  if (!storage || !driver || !caller.domain ||
      !state_->authority->MatchesDomain(caller.domain) ||
      driver->generation() != caller.domain->id() ||
      !driver->snapshot().accepting) {
    return fail("Actual owned storage driver/data/provider unavailable");
  }
  switch (call) {
    case NativeSystemContentCall::kInitializeStorage: {
      if (observed.status != Status::kPrepared || !storage->Pristine() ||
          !data->PristineConstructor() || !device || !device->available ||
          device->device_type != xam::DeviceType::HDD ||
          !Equal(device->guest_volume_root + "\\", kBuiltinHdd)) {
        return fail("Native storage bootstrap/root capability is not pristine");
      }
      return std::vector<uint32_t>{};
    }
    case NativeSystemContentCall::kCompleteStorageBoot:
      if (observed.status != Status::kStorageInitialized ||
          !NativeStorageTasks(*storage) ||
          !driver->snapshot().callback_registered) {
        return fail(
            "Native storage initializer/driver registration not observed");
      }
      return std::vector<uint32_t>{};
    case NativeSystemContentCall::kAttachSource:
      if (observed.status != Status::kStorageBootCompleted ||
          storage->boot_event[1] != 1 ||
          storage->FormattedBuiltinHdd() != state_->source.device_id ||
          !NativeBuiltinSource(*kernel->memory(), *data,
                               state_->source.device_id)) {
        return fail("Genuine native boot/mount/source attachment not observed");
      }
      return std::vector<uint32_t>{state_->source.device_id, 1, 0};
    case NativeSystemContentCall::kInitializeIndex:
      if (observed.status != Status::kSourceAttached || data->initialized ||
          storage->boot_event[1] != 1 ||
          !NativeBuiltinSource(*kernel->memory(), *data,
                               state_->source.device_id)) {
        return fail("Native builtin source is not attached exactly once");
      }
      return std::vector<uint32_t>{};
    case NativeSystemContentCall::kLookup:
      if (observed.status != Status::kIndexInitialized ||
          data->initialized != 1 ||
          !NativeBuiltinSource(*kernel->memory(), *data,
                               state_->source.device_id)) {
        return fail("Actual native index initialization/source unavailable");
      }
      if (!state_->scratch) {
        state_->scratch = kernel->memory()->SystemHeapAlloc(kScratchBytes);
        if (!state_->scratch) {
          return fail("Index witness scratch allocation failed");
        }
      }
      std::memset(kernel->memory()->TranslateVirtual(state_->scratch), 0,
                  kScratchBytes);
      std::memcpy(kernel->memory()->TranslateVirtual(state_->scratch), kMember,
                  sizeof(kMember));
      return std::vector<uint32_t>{state_->scratch,
                                   state_->scratch + kOutputOffset, kCapacity,
                                   state_->scratch + kAuxOffset};
  }
  return fail("Unknown native system-content role");
}

bool NativeSystemContent::Observe(NativeSystemContentCall call, XThread& thread,
                                  std::string* error) {
  const auto fail = [&](const char* text) {
    if (error) {
      *error = text;
    }
    std::lock_guard lock(state_->mutex);
    state_->observed.status = Status::kFailed;
    return false;
  };
  auto* kernel = state_->calls->kernel();
  if (!SameRoot(*state_->calls, *state_->authority) ||
      !XThread::IsInThread(&thread) || thread.kernel_state() != kernel ||
      thread.native_frame_thread_identity().process_type != X_PROCTYPE_SYSTEM) {
    return fail("System content observation lost its actual SYSTEM owner");
  }
  const auto source = kernel->content_manager()->sources().Source(state_->source.id);
  if (!source || !source->available ||
      source->storage_volume != state_->captured_source.storage_volume) {
    return fail("Installed source observation unavailable or replaced");
  }
  std::string path;
  {
  auto lock = global_critical_region::AcquireDirect();
  const auto data = ReadNativeSystemContentManagerData(*kernel->memory());
  const auto storage = ReadNativeSystemStorageData(*kernel->memory());
  const auto driver = NativeContentStorageDriver::Lookup(kernel);
  if (!state_->CurrentSource() || !data ||
      !storage || !driver || !driver->snapshot().accepting ||
      !NativeStorageTasks(*storage) ||
      storage->FormattedBuiltinHdd() != state_->source.device_id ||
      !NativeBuiltinSource(*kernel->memory(), *data,
                           state_->source.device_id)) {
    return fail("Native system source observation invalid");
  }
  if (call == NativeSystemContentCall::kInitializeStorage) {
    if (!driver->snapshot().callback_registered || data->initialized) {
      return fail("Native storage registration/source initialization invalid");
    }
    std::lock_guard guard(state_->mutex);
    state_->observed.status = Status::kStorageInitialized;
    return true;
  }
  if (call == NativeSystemContentCall::kCompleteStorageBoot) {
    const auto physical = driver->snapshot();
    if (snapshot().status != Status::kStorageInitialized ||
        storage->boot_event[0] || storage->boot_event[1] != 1 ||
        storage->acknowledgement[0] || storage->acknowledgement[1] != 1 ||
        !physical.callback_registered || !physical.empty_drains ||
        physical.pending_changes) {
      return fail("Native boot enumeration/worker acknowledgement unproved");
    }
    std::lock_guard guard(state_->mutex);
    state_->observed.status = Status::kStorageBootCompleted;
    return true;
  }
  if (call == NativeSystemContentCall::kAttachSource) {
    std::lock_guard lock(state_->mutex);
    state_->observed.status = Status::kSourceAttached;
    return true;
  }
  if (data->initialized != 1) {
    return fail("Native package index not initialized");
  }
  if (call == NativeSystemContentCall::kInitializeIndex) {
    std::lock_guard lock(state_->mutex);
    state_->observed.status = Status::kIndexInitialized;
    return true;  // Initialization is deliberately distinct from lookup
                  // readiness.
  }
  if (call != NativeSystemContentCall::kLookup || !state_->scratch) {
    return fail("Native index witness was not prepared");
  }
  const auto* output = kernel->memory()->TranslateVirtual<const char*>(
      state_->scratch + kOutputOffset);
  const auto* end = static_cast<const char*>(std::memchr(output, 0, kCapacity));
  if (!end || end == output) {
    return fail("Native indexed-member output invalid");
  }
  path.assign(output, end);
  }
  const auto relative = NativeSystemContentRelativePackagePath(path);
  if (!relative) {
    return fail("Native indexed package escaped system content source");
  }
  auto* content = kernel->content_manager();
  // The path was produced by native lookup and checked above; common capture
  // binds it to this exact registered generation before reading any package.
  std::shared_ptr<vfs::Device> opened_device;
  if (content->OpenContentFile(kProbeRoot, path, &state_->captured_source,
                               0, &opened_device) != X_ERROR_SUCCESS) {
    return fail("Actual indexed package cannot be mounted by common backend");
  }
  vfs::Entry* package_root = nullptr;
  {
    auto lock = global_critical_region::AcquireDirect();
    if (opened_device && !opened_device->namespace_retiring()) {
      package_root = opened_device->ResolvePath("");
    }
  }
  vfs::File* raw_file = nullptr;
  vfs::FileAction action;
  bool opened = package_root && opened_device->is_read_only() &&
      XSUCCEEDED(kernel->file_system()->OpenFile(
          package_root, kMember, vfs::FileDisposition::kOpen,
          vfs::FileAccess::kFileReadData, false, true, &raw_file, &action));
  std::unique_ptr<vfs::File, void (*)(vfs::File*)> file(
      raw_file, [](vfs::File* value) { if (value) value->Destroy(); });
  uint64_t toc_size = opened ? file->entry()->size() : 0;
  opened = opened && toc_size && toc_size <= 16 * 1024 * 1024;
  std::vector<uint8_t> bytes;
  size_t read = 0;
  if (opened) {
    bytes.resize(static_cast<size_t>(toc_size));
    opened = XSUCCEEDED(file->ReadSync(bytes, 0, &read)) && read == bytes.size();
  }
  file.reset();
  // Exact publication identity: an old probe may never close a reused alias.
  const auto closed = content->CloseContent(kProbeRoot, opened_device.get());
  if (!opened || closed != X_ERROR_SUCCESS) {
    return fail("Actual indexed AvatarAssetPack TOC open/read/close failed");
  }
  {
    auto lock = global_critical_region::AcquireDirect();
    const auto data = ReadNativeSystemContentManagerData(*kernel->memory());
    const auto storage = ReadNativeSystemStorageData(*kernel->memory());
    const auto driver = NativeContentStorageDriver::Lookup(kernel);
    if (!state_->CurrentSource() || !data || data->initialized != 1 ||
        !storage || !NativeStorageTasks(*storage) || !driver ||
        !driver->snapshot().accepting ||
        storage->FormattedBuiltinHdd() != state_->source.device_id ||
        !NativeBuiltinSource(*kernel->memory(), *data, state_->source.device_id)) {
      return fail("Native source/index changed during actual package read");
    }
  }
  std::lock_guard observed_lock(state_->mutex);
  state_->observed.package_path = path;
  state_->observed.toc_size = toc_size;
  state_->observed.toc_sha256 = Digest(bytes);
  state_->observed.status = Status::kLookupAndOpenVerified;
  return true;
}
std::optional<xam::ContentSourceState> NativeSystemContent::CaptureAvatarSource(
    const GuestRuntimeCallExecutor& calls) const {
  const auto observed = snapshot();
  if (&calls != state_->calls.get() ||
      observed.status != Status::kLookupAndOpenVerified ||
      observed.package_path.empty() || observed.toc_sha256.empty() ||
      !observed.toc_size) return {};
  auto* kernel = calls.kernel();
  auto guard = global_critical_region::AcquireDirect();
  const auto data = ReadNativeSystemContentManagerData(*kernel->memory());
  const auto storage = ReadNativeSystemStorageData(*kernel->memory());
  const auto driver = NativeContentStorageDriver::Lookup(kernel);
  if (!state_->CurrentSource() || !data || data->initialized != 1 ||
      !storage || !NativeStorageTasks(*storage) || !driver ||
      !driver->snapshot().accepting ||
      storage->FormattedBuiltinHdd() != state_->source.device_id ||
      !NativeBuiltinSource(*kernel->memory(), *data, state_->source.device_id)) {
    return {};
  }
  return state_->captured_source;
}

}  // namespace xe::kernel
