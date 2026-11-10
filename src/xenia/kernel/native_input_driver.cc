/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia contributors. All rights reserved.                    *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/native_input_driver.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <exception>
#include <limits>
#include <string_view>
#include <vector>
#include "third_party/mbedtls/include/mbedtls/sha256.h"
#include "xenia/base/byte_order.h"
#include "xenia/base/mutex.h"
#include "xenia/base/native_system_ui_trace.h"
#include "xenia/cpu/export_resolver.h"
#include "xenia/cpu/ppc/ppc_context.h"
#include "xenia/cpu/processor.h"
#include "xenia/cpu/thread_state.h"
#include "xenia/cpu/xex_module.h"
#include "xenia/kernel/native_system_ui_notifications.h"
#include "xenia/kernel/native_text_input_data.h"
#include "xenia/kernel/user_module.h"
#include "xenia/kernel/xthread.h"

#include "xenia/emulator.h"
#include "xenia/hid/input_system.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/native_import_domain.h"
#include "xenia/kernel/native_system_callback_dispatcher.h"

DECLARE_bool(native_system_ui_output_capture_b_down);

namespace xe::kernel {
namespace {
constexpr X_STATUS kDeviceNotConnected = 0xC000009D;
std::mutex installed_mutex;
std::map<KernelState*, std::weak_ptr<NativeInputDriver>> installed;
// Driver-owned opaque tokens, not guest pointers, user indices or host slots.
// The supported class0 controller profile does not interpret their high bits.
std::atomic<uint32_t> next_device_token{0x10000};
std::atomic<uint64_t> next_transport_generation{1};
thread_local const NativeInputDriver* callback_driver = nullptr;
using Trace = xe::NativeSystemUiTraceLimiter;
std::atomic<uint64_t> next_input_trace_id{1};
uint64_t InputTraceId() noexcept {
  if (!cvars::native_system_ui_trace) {
    return 0;
  }
  auto value = next_input_trace_id.load(std::memory_order_relaxed);
  while (value != UINT64_MAX &&
         !next_input_trace_id.compare_exchange_weak(
             value, value + 1, std::memory_order_relaxed)) {
  }
  return value == UINT64_MAX ? 0 : value;
}
void InputTrace(Trace& limiter, const NativeInputDriver::Config& config,
                uint64_t generation, const char* event,
                std::initializer_list<Trace::Field> fields) noexcept {
  if (!cvars::native_system_ui_trace) {
    return;
  }
  // Trace-only paths may run after ordinary admission was denied. Inert
  // contexts must not be dereferenced just to format optional identities.
  const auto domain = config.dispatcher ? config.dispatcher->domain() : nullptr;
  if (domain) {
    limiter.Emit("input", event,
                 {{"domain_id", domain->id()},
                  {"root_hmodule", domain->root().hmodule},
                  {"root_load_generation", domain->root().generation},
                  {"input_generation", generation}},
                 fields);
  } else {
    limiter.Emit("input", event, {{"input_generation", generation}}, fields);
  }
}
template <class F>
struct TraceExit {
  F emit;
  ~TraceExit() { emit(); }
};

uint32_t AllocateToken() {
  uint32_t token = next_device_token.load();
  while (token < 0x0FFFFFFF) {
    if (next_device_token.compare_exchange_weak(token, token + 1)) {
      return token | 0x10000000;
    }
  }
  return 0;
}

bool CanonicalGuestPointer(uint64_t value, uint32_t address) {
  return uint32_t(value) == address &&
         (value == uint64_t(address) ||
          value == uint64_t(int64_t(int32_t(address))));
}

// Exact supplied17559 consumer/scene/Task ABI. Called only with a retained
// domain operation and actual root execution lease, under metadata mapping
// lock.
bool TextProfileMatches(KernelState& kernel, UserModule& module) {
  struct Profile {
    uint32_t at, bytes;
    const char* hash;
  };
  constexpr Profile profiles[] = {
      {0x816F1DC0, 428,
       "4beadb155444cb473f5e6139153f3a8b438c3bcd3b7efc9dcc4102ce74cce3b6"},
      {0x816F4778, 828,
       "cad0c322f7489a999baaf9ee37119d22b391d94ea6a2aaf7d196868bea20f0b5"},
      {0x816D8A58, 76,
       "4314bebee568900d71b6080a1f51ad1f6962d58b1ce58b53ab1a5adc328e6a32"},
      {0x816D8CD8, 632,
       "3df7f90fde97d2faa445901ba893dfb1e451ca9a3de51ef28583dc1bb6ca6d79"},
      {0x816D8F50, 268,
       "06041d724753596609b1c2989c05e671b3a62165b00901fce021bc1bb372fc75"},
      {0x816D9060, 44,
       "cd890dde58927b2f8a48a61ebf0acc54be6fe40da86a285fb7e528086db56ba3"},
      {0x816F2890, 12,
       "ddb04eadcee3461a07fce23d855e489363d4a5f0ca22d765d63481fff4bb14ba"},
      {0x816F1D48, 72,
       "eb155ea9079c04285a1e174b0c2e5dcaa344f6d897552792811a2c2e00bde1eb"},
      {0x816F2ED0, 248,
       "32fbae94e72bda3479b261d5e65ae35faef59cd51f464f7c57401a85ec6933da"},
      {0x816F28A8, 360,
       "b3a8fd98755314a7b610a28493fb51907abd7bc03e55697cfee654b42def8ca9"},
      {0x816B7EF0, 64,
       "bdd3d338605bedc752f4c86293f2e6eda8f0f4679b6c92b6d97d2f0157ec5633"},
      {0x816B8B08, 152,
       "1e6e216190b9d6046af3325bf16deb0e22ff9409b457f920a67bfbb1b756c2de"},
      {0x816CE108, 1956,
       "dae999d52ae924106fb27a841eea9bf9b98caa634f4e6f9d6b21ad97d24ffe69"},
      {0x81693E08, 16,
       "cd53510cd00cd3fb02a3b025a52f8c22e9c76edc23fe2b03bcdf362f5755e386"},
      {0x81692440, 32,
       "9751854e4a0c830664348ad065fa221bd7b72014e05162a6e1cb363f963b8aa1"},
      {0x81692C60, 92,
       "a53ad7cbe7a083667df52518763dbd040bf7b8fecebcaa7482e9aafa2bbc2f6c"},
      {0x81694830, 312,
       "a93727bb11a5f51c7130ddb95f871ab6cf72716f7bfdf01f37a7edfcccfb2aef"},
      {0x81694D90, 64,
       "858991edb6f360cb4c72857f5e0f7ef951c035bc5369cbcb30392eb1a4b7f561"},
      {0x817AE868, 152,
       "b1a795c8a4b104b60c9e4bc17da9bc756b168710c1abfa9412d5b3ee2736a4ba"},
      {0x817AE6F8, 140,
       "472d6ae78f65c27afbf95307b081e24dfcde5c2132a3f3df03683a85534f784a"},
      {0x817BCC38, 148,
       "b8d4914f02bae135c8f45a2874e76bb60d1920f55d07b675b03d1a6d3009dcfb"},
      {0x817D4468, 544,
       "d5cc558afe5f6edb0e417457d81e85edc2c6267c84292ae9ef3adc28817bb8a7"},
      {0x816929B8, 196,
       "2e4be38f85aa6b64dba62af0a3ccb76f37715d7c745060b4b1709f164a616200"},
      {0x81693E40, 172,
       "e8fbbdcefc3e4b0415509dbd214e27fa85d51389ecf811e3f9b555053c3d57e4"},
  };
  auto* memory = kernel.memory();
  constexpr char hex[] = "0123456789abcdef";
  for (const auto& profile : profiles) {
    if (memory->QueryRangeAccess(profile.at, profile.bytes) ==
        memory::PageAccess::kNoAccess) {
      return false;
    }
    for (uint32_t offset = 0; offset < profile.bytes; offset += 4) {
      xex2_section_type type;
      const uint32_t at = profile.at + offset;
      if (kernel.GetUserModuleForAddress(at).get() != &module ||
          !module.xex_module()->GetPageSectionType(at, &type) ||
          type != XEX_SECTION_CODE ||
          module.xex_module()->TranslateCode(at) !=
              memory->TranslateVirtual<const uint8_t*>(at)) {
        return false;
      }
    }
    std::array<uint8_t, 32> digest{};
    mbedtls_sha256(memory->TranslateVirtual<const uint8_t*>(profile.at),
                   profile.bytes, digest.data(), 0);
    for (size_t i = 0; i < digest.size(); ++i) {
      if (profile.hash[2 * i] != hex[digest[i] >> 4] ||
          profile.hash[2 * i + 1] != hex[digest[i] & 15]) {
        return false;
      }
    }
  }
  struct Import {
    uint32_t ordinal, at;
    const char* name;
  };
  constexpr Import imports[] = {
      {0x277, 0x81A730C4, "XInputdReadTextKeystroke"},
      {0x2C8, 0x81A73104, "XInputdSetTextDeviceKeyLocks"},
      {0x2C9, 0x81A730B4, "XInputdGetTextDeviceKeyLocks"},
      {0x2AD, 0x81A730F4, "XInputdGetLastTextInputTime"},
      {0x154, 0x81A72CE4, "KeTlsGetValue"},
  };
  constexpr std::array<uint8_t, 16> thunk{0x44, 0x00, 0x00, 0x42, 0x4E, 0x80,
                                          0x00, 0x20, 0x60, 0x00, 0x00, 0x00,
                                          0x60, 0x00, 0x00, 0x00};
  for (const auto& required : imports) {
    const auto* binding =
        kernel.processor()->export_resolver()->GetExportByOrdinal(
            "xboxkrnl.exe", required.ordinal);
    bool imported = false;
    for (const auto& library : *module.xex_module()->import_libraries()) {
      if (library.name != "xboxkrnl") {
        continue;
      }
      for (const auto& entry : library.imports) {
        imported |= entry.ordinal == required.ordinal &&
                    entry.thunk_address == required.at;
      }
    }
    if (!imported || !binding ||
        binding->get_type() != cpu::Export::Type::kFunction ||
        (binding->tags & cpu::ExportTag::kStub) ||
        !binding->function_data.trampoline ||
        std::string_view(binding->name) != required.name ||
        kernel.GetUserModuleForAddress(required.at).get() != &module ||
        memory->QueryRangeAccess(required.at, 16) ==
            memory::PageAccess::kNoAccess ||
        std::memcmp(memory->TranslateVirtual(required.at), thunk.data(), 16)) {
      return false;
    }
  }
  for (uint32_t at : {0x81A82570u, 0x81A82FF4u, 0x81A83014u, 0x81A83000u,
                      0x81AA20E0u, 0x81A9DE68u, 0x81AC8328u}) {
    if (kernel.GetUserModuleForAddress(at).get() != &module ||
        memory->QueryRangeAccess(at, 4) == memory::PageAccess::kNoAccess) {
      return false;
    }
  }
  return xe::load_and_swap<uint32_t>(memory->TranslateVirtual(0x81A82FF4)) ==
             0x81AA3D90 &&
         xe::load_and_swap<uint32_t>(memory->TranslateVirtual(0x81A83014)) ==
             0x81A82FF8 &&
         xe::load_and_swap<uint32_t>(memory->TranslateVirtual(0x81A83000)) ==
             0x816F2890 &&
         module.GetProcAddressByOrdinal(0x198) == 0x816F4778 &&
         module.GetProcAddressByOrdinal(0x3C9) == 0x81692440;
}

}  // namespace

std::shared_ptr<NativeInputDriver> NativeInputDriver::Create(Config config) {
  if (!config.kernel || !config.generation || !config.dispatcher ||
      !config.authorize_current_caller || !config.validate_callback ||
      !config.allow_physical_input || !config.validate_device_context ||
      !config.protocol_failed ||
      config.dispatcher->domain()->id() != config.generation ||
      !config.dispatcher->IsAccepting() ||
      !config.kernel->emulator()->input_system()) {
    return {};
  }
  uint64_t transport_generation = next_transport_generation.load();
  while (transport_generation && transport_generation < UINT64_MAX &&
         !next_transport_generation.compare_exchange_weak(
             transport_generation, transport_generation + 1)) {
  }
  if (!transport_generation || transport_generation == UINT64_MAX) {
    return {};
  }
  auto driver = std::shared_ptr<NativeInputDriver>(
      new NativeInputDriver(std::move(config), transport_generation));
  return driver;
}

std::unique_ptr<NativeInputDriver::Installation> NativeInputDriver::Install(
    const std::shared_ptr<NativeInputDriver>& driver) {
  if (!driver || !driver->IsCurrent()) {
    return {};
  }
  auto installation = std::unique_ptr<Installation>(new Installation(driver));
  std::shared_ptr<NativeInputDriver> existing;
  {
    std::lock_guard lock(installed_mutex);
    existing = installed[driver->config_.kernel].lock();
  }
  if (existing) {
    return {};
  }
  // Kernel retention precedes publication/transport. The registries are weak;
  // a partial-start caller dropping its handle must not lose the later stop
  // and capture-release target while InputSystem persists across relaunch.
  driver->config_.kernel->RetainNativeResourceUntilTeardown(driver);
  {
    std::lock_guard lock(installed_mutex);
    existing = installed[driver->config_.kernel].lock();
    if (!existing) {
      installed[driver->config_.kernel] = driver;
    }
  }
  if (existing) {
    return {};
  }
  installation->armed_ = true;
  auto* input = driver->config_.kernel->emulator()->input_system();
  auto input_lock = input->lock();
  {
    std::lock_guard lock(driver->mutex_);
    driver->transport_started_ = true;
  }
  const std::weak_ptr<NativeInputDriver> weak = driver;
  if (!input->BeginNativeInputTransport(
          driver->transport_generation_,
          [weak](uint32_t slot) {
            auto owner = weak.lock();
            return owner ? owner->CaptureGuideButtonEdge(slot) : nullptr;
          },
          [weak] {
            if (auto owner = weak.lock()) {
              try {
                if (!owner->RefreshDevices()) {
                  owner->ProtocolFailed(0, 1);
                }
              } catch (...) {
                owner->ProtocolFailed(0, 2);
              }
            }
          })) {
    return {};
  }
  return installation;
}

NativeInputDriver::Installation::~Installation() {
  if (armed_) {
    driver_->BeginStop();
  }
}

std::shared_ptr<NativeInputDriver> NativeInputDriver::Lookup(
    KernelState* kernel) {
  std::lock_guard lock(installed_mutex);
  const auto found = installed.find(kernel);
  return found == installed.end() ? nullptr : found->second.lock();
}

void NativeInputDriver::BeginStopForKernel(KernelState* kernel) {
  if (auto driver = Lookup(kernel)) {
    driver->BeginStop();
  }
}

bool NativeInputDriver::DrainForKernel(KernelState* kernel) {
  auto driver = Lookup(kernel);
  return !driver || driver->Drain();
}

bool NativeInputDriver::ReleaseInputTransportAfterProducersStoppedForKernel(
    KernelState* kernel) {
  auto driver = Lookup(kernel);
  return !driver || driver->ReleaseInputTransportAfterProducersStopped();
}

NativeInputDriver::~NativeInputDriver() {
  BeginStop();
  std::lock_guard lock(installed_mutex);
  auto found = installed.find(config_.kernel);
  if (found != installed.end() && found->second.expired()) {
    installed.erase(found);
  }
}

bool NativeInputDriver::IsCurrent() const {
  {
    std::lock_guard lock(mutex_);
    if (stopping_) {
      return false;
    }
  }
  return config_.dispatcher->domain()->active() &&
         config_.dispatcher->domain()->id() == generation() &&
         config_.dispatcher->IsAccepting();
}

bool NativeInputDriver::AuthorizeCurrentCaller() const {
  try {
    return IsCurrent() && config_.authorize_current_caller();
  } catch (...) {
    return false;
  }
}

bool NativeInputDriver::SetCallback(Callback kind, uint32_t address) {
  auto poll = AcquirePoll();
  if (!poll || size_t(kind) >= registrations_.size() ||
      !AuthorizeCurrentCaller()) {
    return false;
  }
  try {
    if (address && !config_.validate_callback(address)) {
      return false;
    }
  } catch (...) {
    return false;
  }
  Registration observed;
  {
    std::lock_guard lock(mutex_);
    auto& registration = registrations_[size_t(kind)];
    if (stopping_ || registration.epoch == UINT64_MAX) {
      return false;
    }
    if (registration.address != address) {
      ++registration.epoch;
      registration.address = address;
    }
    observed = registration;
  }
  if (cvars::native_system_ui_trace) {
    static Trace trace;
    InputTrace(trace, config_, transport_generation_, "callback_registration",
               {{"kind", uint32_t(kind)},
                {"address", observed.address},
                {"requested_address", address},
                {"registration_epoch", observed.epoch}});
  }
  if ((kind == Callback::kUserBinding || kind == Callback::kDeviceConfig) &&
      address) {
    try {
      // A failed initial enumeration is partial startup: retain registration
      // and any queued work, but never report it as successful readiness.
      if (!RefreshDevices()) {
        return false;
      }
    } catch (...) {
      return false;
    }
  }
  if (kind == Callback::kFailedConnectionOrBind && address &&
      !AnnounceFailedBinding()) {
    return false;
  }
  return true;
}

bool NativeInputDriver::EnableGuideIntent(
    std::function<bool()> guide_requests_allowed,
    std::function<NativeUiBackendRoute::Admission()> admit_ui) {
  auto poll = AcquirePoll();
  if (!poll || !AuthorizeCurrentCaller() || !guide_requests_allowed ||
      !admit_ui) {
    return false;
  }
  struct Profile {
    uint32_t at, bytes;
    const char* hash;
  };
  constexpr Profile profiles[] = {
      {0x816F2E60, 16,
       "3d408140adc24cc68219d06a9741eb0d1d249a990f7dceedd879427f809d01bb"},
      {0x816F0400, 12,
       "5e90ab06f7154f396221e0d33916356d56663c1eef11b0f5a10495c5e3d38b9f"},
      {0x816F0368, 148,
       "6ead51eb75492692b88d7adf19263c7c74186f338daac9cc88b07c425ef1893c"},
      {0x816F09E8, 24,
       "c3abf2d0272c70dfe675e92f4887574733f4e5aa64f67d45e01be30e5e1cbb99"},
      {0x816F06E8, 184,
       "a901d77026e5e55e996de0ac831ec4741116bd5105e36983ab6d3c6a4284594a"},
      {0x816F0AE0, 764,
       "e9c2f0321448092466e8b97a03e21d70c40d9f93d92e2893720ca3e90604c702"},
      {0x816F0DF0, 128,
       "879bc519c5ba8b56e0a0a4e288f9d5736e6a4b17596333556a94438162d0fae6"},
      {0x816F0E70, 180,
       "4a909f7a2505c327c3bc537be2d156824fad32d39ca6d7c054958eb934b8636a"},
  };
  auto* kernel = config_.kernel;
  auto* memory = kernel->memory();
  auto module = kernel->GetUserModuleForAddress(0x816F2E60);
  if (!module ||
      module->hmodule_ptr() != config_.dispatcher->domain()->root().hmodule ||
      module->load_generation() !=
          config_.dispatcher->domain()->root().generation ||
      module->GetProcAddressByOrdinal(0x506) != 0x816F2E60) {
    return false;
  }
  for (const auto& profile : profiles) {
    if (memory->QueryRangeAccess(profile.at, profile.bytes) ==
        memory::PageAccess::kNoAccess) {
      return false;
    }
    for (uint64_t at = profile.at; at < uint64_t(profile.at) + profile.bytes;
         at += 4) {
      xex2_section_type type;
      if (kernel->GetUserModuleForAddress(uint32_t(at)).get() != module.get() ||
          !module->xex_module()->GetPageSectionType(uint32_t(at), &type) ||
          type != XEX_SECTION_CODE ||
          module->xex_module()->TranslateCode(uint32_t(at)) !=
              memory->TranslateVirtual<const uint8_t*>(uint32_t(at))) {
        return false;
      }
    }
    std::array<uint8_t, 32> digest{};
    mbedtls_sha256(memory->TranslateVirtual<const uint8_t*>(profile.at),
                   profile.bytes, digest.data(), 0);
    constexpr char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < digest.size(); ++i) {
      if (profile.hash[2 * i] != hex[digest[i] >> 4] ||
          profile.hash[2 * i + 1] != hex[digest[i] & 15]) {
        return false;
      }
    }
  }
  {
    std::lock_guard lock(mutex_);
    if (stopping_) {
      return false;
    }
    guide_intent_enabled_ = true;
    guide_requests_allowed_ = std::move(guide_requests_allowed);
    admit_ui_ = std::move(admit_ui);
  }
  auto* input = config_.kernel->emulator()->input_system();
  auto input_lock = input->lock();
  const bool enabled = input->EnableNativeGuideTransport(transport_generation_);
  static Trace trace;
  InputTrace(trace, config_, transport_generation_, "guide_channel_enabled",
             {{"enabled", enabled}});
  return enabled;
}
bool NativeInputDriver::GuideOwnerMatches(uint32_t owner, uint32_t task) const {
  if (!owner || owner > UINT32_MAX - 0x24 || task < 0x1000 || (task & 3) != 1) {
    return false;
  }
  auto* memory = config_.kernel->memory();
  auto memory_lock = global_critical_region::AcquireDirect();
  return memory->QueryRangeAccess(0x81AAC260, 4) !=
             memory::PageAccess::kNoAccess &&
         memory->QueryRangeAccess(owner, 0x24) !=
             memory::PageAccess::kNoAccess &&
         xe::load_and_swap<uint32_t>(memory->TranslateVirtual(0x81AAC260)) ==
             owner &&
         *memory->TranslateVirtual<const uint8_t*>(owner) == 1 &&
         xe::load_and_swap<uint32_t>(memory->TranslateVirtual(owner + 0x1C)) ==
             task;
}
std::shared_ptr<const hid::NativeGuideButtonEdge>
NativeInputDriver::CaptureGuideButtonEdge(uint32_t slot) {
  auto poll = AcquirePoll();
  const uint64_t trace_id = InputTraceId();
  uint32_t trace_reason = 1;
  static Trace trace;
  TraceExit trace_exit{[&] {
    if (trace_id) {
      InputTrace(trace, config_, transport_generation_, "guide_capture",
                 {{"input_trace_id", trace_id},
                  {"slot", slot},
                  {"reason", trace_reason}});
    }
  }};
  if (!poll || slot >= XUserMaxUserCount || !IsCurrent()) {
    return {};
  }
  std::shared_ptr<Device> device;
  Registration system;
  uint64_t binding = 0;
  uint32_t user = XUserIndexAny;
  {
    std::lock_guard lock(mutex_);
    device = slots_[slot];
    system = registrations_[size_t(Callback::kSystemRequest)];
    binding = registrations_[size_t(Callback::kUserBinding)].epoch;
    if (!guide_intent_enabled_ || system.address != 0x816F09E8 || !device ||
        !device->connected || device->native_user_registration != binding ||
        device->native_user >= XUserMaxUserCount) {
      trace_reason = !guide_intent_enabled_                        ? 2
                     : system.address != 0x816F09E8                ? 3
                     : !device                                     ? 4
                     : !device->connected                          ? 5
                     : device->native_user_registration != binding ? 6
                                                                   : 7;
      return {};
    }
    user = device->native_user;
  }
  uint32_t owner = 0, task = 0;
  {
    auto memory_lock = global_critical_region::AcquireDirect();
    auto* memory = config_.kernel->memory();
    if (memory->QueryRangeAccess(0x81AAC260, 4) ==
        memory::PageAccess::kNoAccess) {
      trace_reason = 8;
      return {};
    }
    owner = xe::load_and_swap<uint32_t>(memory->TranslateVirtual(0x81AAC260));
    if (!owner || owner > UINT32_MAX - 0x24 ||
        memory->QueryRangeAccess(owner, 0x24) ==
            memory::PageAccess::kNoAccess) {
      trace_reason = 9;
      return {};
    }
    task = xe::load_and_swap<uint32_t>(memory->TranslateVirtual(owner + 0x1C));
  }
  if (!GuideOwnerMatches(owner, task)) {
    trace_reason = 10;
    return {};
  }
  static Trace identity_trace;
  if (trace_id) {
    InputTrace(identity_trace, config_, transport_generation_,
               "guide_capture_identity",
               {{"input_trace_id", trace_id},
                {"slot", slot},
                {"native_user", user},
                {"device_handle", device->handle},
                {"binding_identity", device->binding_identity},
                {"binding_epoch", binding},
                {"sysreq_address", system.address},
                {"sysreq_epoch", system.epoch},
                {"native_owner", owner},
                {"native_task", task}});
  }
  const std::weak_ptr<NativeInputDriver> weak = shared_from_this();
  auto edge = std::shared_ptr<hid::NativeGuideButtonEdge>(
      new hid::NativeGuideButtonEdge(
          transport_generation_, slot,
          [weak, device, user, binding, system, owner, task, trace_id] {
            auto driver = weak.lock();
            return driver ? driver->DispatchGuideButtonEdge(
                                device, user, binding, system, owner, task,
                                trace_id)
                          : hid::NativeGuideDispatch{};
          }));
  edge->trace_id_ = trace_id;
  trace_reason = 0;
  return edge;
}
hid::NativeGuideDispatch NativeInputDriver::DispatchGuideButtonEdge(
    const std::shared_ptr<Device>& device, uint32_t user, uint64_t binding,
    Registration system, uint32_t owner, uint32_t task, uint64_t trace_id) {
  auto poll = AcquirePoll();
  if (!poll || user >= XUserMaxUserCount || !IsCurrent()) {
    return {};
  }
  const std::weak_ptr<NativeInputDriver> weak = shared_from_this();
  const auto eligible = [weak, device, user, binding, system, owner, task,
                         trace_id] {
    auto self = weak.lock();
    uint32_t reason = 1;
    static Trace trace;
    TraceExit trace_exit{[&] {
      if (trace_id && self) {
        InputTrace(trace, self->config_, self->transport_generation_,
                   "guide_eligible",
                   {{"input_trace_id", trace_id},
                    {"slot", device->slot},
                    {"native_user", user},
                    {"reason", reason}});
      }
    }};
    if (!self || !self->IsCurrent() || !self->config_.allow_physical_input()) {
      return false;
    }
    std::function<bool()> guide_requests_allowed;
    {
      std::lock_guard lock(self->mutex_);
      guide_requests_allowed = self->guide_requests_allowed_;
    }
    if (!guide_requests_allowed || !guide_requests_allowed()) {
      reason = 2;
      return false;
    }
    {
      auto* input = self->config_.kernel->emulator()->input_system();
      auto lock = input->lock();
      if (input->IsUIInputBlocked()) {
        reason = 3;
        return false;
      }
    }
    if (!self->GuideOwnerMatches(owner, task)) {
      reason = 4;
      return false;
    }
    std::lock_guard lock(self->mutex_);
    const auto actual = self->registrations_[size_t(Callback::kSystemRequest)];
    const bool allowed =
        !self->stopping_ && self->guide_intent_enabled_ &&
        actual.epoch == system.epoch && actual.address == system.address &&
        self->registrations_[size_t(Callback::kUserBinding)].epoch == binding &&
        device->connected && self->slots_[device->slot] == device &&
        device->native_user_registration == binding &&
        device->native_user == user;
    reason = allowed ? 0 : 5;
    return allowed;
  };
  if (!eligible()) {
    return {};
  }
  std::function<NativeUiBackendRoute::Admission()> admit;
  {
    std::lock_guard lock(mutex_);
    admit = admit_ui_;
  }
  if (!admit) {
    return {};
  }
  auto route = admit();
  if (route.decision == NativeUiBackendRoute::Decision::kHost) {
    if (!eligible() || !route.host || route.host->cancelled()) {
      return {};
    }
    return {hid::NativeGuideDisposition::kHostFallback, std::move(route.host)};
  }
  if (route.decision != NativeUiBackendRoute::Decision::kNative) {
    return {};
  }
  bool accepted = false;
  const std::array args{user};
  QueueCallback(Callback::kSystemRequest, args, {}, 0x816F2E60, eligible,
                &accepted, trace_id);
  static Trace trace;
  if (trace_id) {
    InputTrace(trace, config_, transport_generation_, "guide_enqueue",
               {{"input_trace_id", trace_id},
                {"slot", device->slot},
                {"native_user", user},
                {"accepted", accepted}});
  }
  return {accepted ? hid::NativeGuideDisposition::kAccepted
                   : hid::NativeGuideDisposition::kDenied,
          {}};
}

bool NativeInputDriver::QueueCallback(
    Callback kind, std::span<const uint32_t> arguments,
    std::function<void(bool, uint32_t)> completed, uint32_t address_override,
    std::function<bool()> eligible, bool* accepted_out, uint64_t trace_id) {
  if (!trace_id) {
    trace_id = InputTraceId();
  }
  if (accepted_out) {
    *accepted_out = false;
  }
  if (!IsCurrent()) {
    return false;
  }
  Registration registration;
  {
    std::lock_guard lock(mutex_);
    registration = registrations_[size_t(kind)];
    if (stopping_ || !registration.address) {
      return false;
    }
    ++pending_callbacks_;
  }
  auto self = shared_from_this();
  auto finish = [self, kind, trace_id, completed = std::move(completed)](
                    bool entered, uint32_t result) {
    try {
      if (completed) {
        completed(entered, result);
      }
    } catch (...) {
      // A failed host completion must still release the drain reservation.
    }
    static std::array<Trace, 4> traces;
    if (trace_id) {
      InputTrace(traces[size_t(kind)], self->config_,
                 self->transport_generation_, "callback_complete",
                 {{"input_trace_id", trace_id},
                  {"kind", uint32_t(kind)},
                  {"executor_succeeded", entered},
                  {"raw_result", result}});
    }
    std::lock_guard lock(self->mutex_);
    --self->pending_callbacks_;
    self->drained_.notify_all();
  };
  struct Reservation {
    explicit Reservation(const NativeInputDriver* value)
        : previous(callback_driver) {
      callback_driver = value;
    }
    ~Reservation() { callback_driver = previous; }
    const NativeInputDriver* previous;
  };
  auto before = [self, kind, registration,
                 eligible = std::move(eligible)]() -> std::shared_ptr<void> {
    if (!self->IsCurrent() || (eligible && !eligible())) {
      return {};
    }
    std::lock_guard lock(self->mutex_);
    const auto& current = self->registrations_[size_t(kind)];
    if (self->stopping_ || current.epoch != registration.epoch ||
        current.address != registration.address) {
      return {};
    }
    return std::make_shared<Reservation>(self.get());
  };
  bool accepted = false;
  try {
    accepted = config_.dispatcher->Enqueue(
        address_override ? address_override : registration.address, arguments,
        finish, std::move(before), trace_id);
  } catch (...) {
  }
  if (accepted_out) {
    *accepted_out = accepted;
  }
  if (!accepted) {
    finish(false, 0);
  }
  // Once finish has been called, it owns any caller cleanup even on rejection.
  return true;
}

NativeInputDriver::InitialBindingSnapshot
NativeInputDriver::initial_binding_snapshot() const {
  const bool current = IsCurrent();
  std::lock_guard lock(mutex_);
  InitialBindingSnapshot out;
  out.accepting = current && !stopping_;
  out.failed = protocol_failed_;
  out.callbacks_registered =
      registrations_[size_t(Callback::kUserBinding)].address == 0x816D9060 &&
      registrations_[size_t(Callback::kDeviceConfig)].address == 0x816D9090 &&
      registrations_[size_t(Callback::kFailedConnectionOrBind)].address ==
          0x816F27E0;
  out.enumeration_observed = enumeration_observed_;
  out.pending_callbacks = pending_callbacks_;
  const auto epoch = registrations_[size_t(Callback::kUserBinding)].epoch;
  for (uint32_t slot = 0; slot < XUserMaxUserCount; ++slot) {
    const auto& device = slots_[slot];
    if (!device || !device->connected) {
      continue;
    }
    out.connected_mask |= 1u << slot;
    out.failed |= device->binding_rejected;
    if (device->native_user < XUserMaxUserCount &&
        device->native_user_registration == epoch) {
      out.bound_mask |= 1u << slot;
    }
  }
  return out;
}
void NativeInputDriver::ProtocolFailed(uint32_t handle,
                                       uint32_t reason) noexcept {
  bool notify = false;
  {
    std::lock_guard lock(mutex_);
    if (!stopping_ && !protocol_failed_) {
      protocol_failed_ = true;
      notify = true;
    }
  }
  if (!notify) {
    return;
  }
  static Trace trace;
  InputTrace(trace, config_, transport_generation_, "device_protocol_failed",
             {{"device_handle", handle}, {"reason", reason}});
  try {
    config_.protocol_failed();
  } catch (...) {
  }
}
bool NativeInputDriver::DeviceCallbackEligible(
    const std::shared_ptr<Device>& device, bool removed) const {
  if (!IsCurrent() || !config_.validate_device_context()) {
    return false;
  }
  std::lock_guard lock(mutex_);
  return !stopping_ && !protocol_failed_ &&
         (removed || (device->connected && slots_[device->slot] == device));
}
bool NativeInputDriver::AnnounceDevice(const std::shared_ptr<Device>& device,
                                       bool removed) {
  uint64_t registration_epoch = 0;
  {
    std::lock_guard lock(mutex_);
    registration_epoch = device->announced_registration;
  }
  auto* memory = config_.kernel->memory();
  struct GuestOutput {
    Memory* memory;
    uint32_t address = 0;
    ~GuestOutput() {
      if (address) {
        memory->SystemHeapFree(address);
      }
    }
  };
  auto output = std::make_shared<GuestOutput>();
  output->memory = memory;
  output->address = memory->SystemHeapAlloc(4);
  if (!output->address) {
    return false;
  }
  *memory->TranslateVirtual<uint8_t*>(output->address) = uint8_t(device->slot);
  // Verified17559 callback: physical group identity, opaque driver handle,
  // device class0, removal boolean, in/out requested/assigned user byte.
  const std::array<uint32_t, 5> args = {device->binding_identity,
                                        device->handle, 0, removed ? 1u : 0u,
                                        output->address};
  auto self = shared_from_this();
  const uint64_t trace_id = InputTraceId();
  static Trace request_trace;
  if (trace_id) {
    InputTrace(request_trace, config_, transport_generation_, "binding_request",
               {{"input_trace_id", trace_id},
                {"slot", device->slot},
                {"device_handle", device->handle},
                {"binding_identity", device->binding_identity},
                {"registration_epoch", registration_epoch},
                {"removed", removed},
                {"requested_byte", device->slot}});
  }
  const bool queued = QueueCallback(
      Callback::kUserBinding, args,
      [self, device, output, removed, registration_epoch, trace_id](
          bool entered, uint32_t result) {
        uint32_t assigned = XUserIndexAny;
        if (entered && result == 0 && !removed) {
          auto memory_guard = global_critical_region::AcquireDirect();
          assigned = *output->memory->TranslateVirtual<const uint8_t*>(
              output->address);
        }
        bool adopted = false, failed = false, native_rejected = false;
        {
          std::lock_guard lock(self->mutex_);
          const auto binding =
              self->registrations_[size_t(Callback::kUserBinding)];
          const bool expected =
              !self->stopping_ && !removed && device->connected &&
              self->slots_[device->slot] == device &&
              binding.epoch == registration_epoch &&
              device->announced_registration == registration_epoch;
          native_rejected = expected && entered && int32_t(result) < 0;
          const bool current_epoch =
              !self->stopping_ && binding.epoch == registration_epoch;
          const bool unknown_remove = removed && current_epoch && !entered;
          failed = unknown_remove ||
                   (expected && !native_rejected &&
                    (!entered || result != 0 || assigned >= XUserMaxUserCount));
          if (expected) {
            device->binding_rejected = native_rejected;
          }
          if (!expected || !entered || result != 0 || removed ||
              !device->connected || binding.epoch != registration_epoch ||
              device->announced_registration != registration_epoch ||
              self->slots_[device->slot] != device ||
              assigned >= XUserMaxUserCount) {
            device->native_user = XUserIndexAny;
            device->native_user_registration = 0;
            if (!removed && device->connected &&
                device->announced_registration == registration_epoch) {
              device->announced_registration = 0;
            }
          } else {
            device->native_user = assigned;
            device->native_user_registration = registration_epoch;
            adopted = true;
          }
        }
        if (native_rejected) {
          self->RecordFailedBinding(device);
        }
        if (failed) {
          self->ProtocolFailed(device->handle, 3);
        }
        static Trace trace;
        if (trace_id) {
          if (entered && result == 0 && !removed) {
            InputTrace(trace, self->config_, self->transport_generation_,
                       "binding_output",
                       {{"input_trace_id", trace_id},
                        {"slot", device->slot},
                        {"device_handle", device->handle},
                        {"registration_epoch", registration_epoch},
                        {"executor_succeeded", entered},
                        {"raw_result", result},
                        {"assigned_byte", assigned},
                        {"adopted", adopted}});
          } else {
            InputTrace(trace, self->config_, self->transport_generation_,
                       "binding_output",
                       {{"input_trace_id", trace_id},
                        {"slot", device->slot},
                        {"device_handle", device->handle},
                        {"registration_epoch", registration_epoch},
                        {"executor_succeeded", entered},
                        {"raw_result", result},
                        {"assigned_byte_known", 0},
                        {"adopted", adopted}});
          }
        }
      },
      0,
      [self, device, removed] {
        return self->DeviceCallbackEligible(device, removed);
      },
      nullptr, trace_id);
  return queued;
}

void NativeInputDriver::RecordFailedBinding(
    const std::shared_ptr<Device>& device) {
  {
    std::lock_guard lock(mutex_);
    if (stopping_ || protocol_failed_ || !device->connected ||
        slots_[device->slot] != device) {
      return;
    }
    // One actual opaque-handle latch, not a user mask. Only a known executed
    // native ADD callback's negative HRESULT reaches this path.
    failed_connection_or_bind_ = device->handle;
    failure_notification_pending_ = true;
  }
  if (!AnnounceFailedBinding()) {
    ProtocolFailed(device->handle, 5);
  }
}
bool NativeInputDriver::AnnounceFailedBinding() {
  uint64_t epoch = 0;
  {
    std::lock_guard lock(mutex_);
    const auto registration =
        registrations_[size_t(Callback::kFailedConnectionOrBind)];
    if (stopping_ || protocol_failed_) {
      return false;
    }
    if (!failure_notification_pending_ || failure_callback_queued_ ||
        !registration.address) {
      return true;
    }
    epoch = registration.epoch;
    failure_callback_queued_ = true;
    failure_notification_pending_ = false;
  }
  auto self = shared_from_this();
  const std::array<uint32_t, 0> args{};
  const bool result = QueueCallback(
      Callback::kFailedConnectionOrBind, args,
      [self, epoch](bool executed, uint32_t) {
        bool failed = false, again = false;
        {
          std::lock_guard lock(self->mutex_);
          self->failure_callback_queued_ = false;
          const bool current =
              !self->stopping_ && !self->protocol_failed_ &&
              self->registrations_[size_t(Callback::kFailedConnectionOrBind)]
                      .epoch == epoch;
          failed = current && !executed;
          again = current && executed && self->failure_notification_pending_;
        }
        // Native F27E0 is a no-argument notification; its tail KeSetEvent
        // return is not an HRESULT and is intentionally not interpreted.
        if (failed) {
          self->ProtocolFailed(0, 6);
        }
        if (again && !self->AnnounceFailedBinding()) {
          self->ProtocolFailed(0, 7);
        }
      });
  if (!result) {
    std::lock_guard lock(mutex_);
    failure_callback_queued_ = false;
    failure_notification_pending_ = true;
  }
  return result;
}

bool NativeInputDriver::AnnounceConfiguration(
    const std::shared_ptr<Device>& device, uint64_t revision) {
  auto self = shared_from_this();
  uint64_t epoch;
  {
    std::lock_guard lock(mutex_);
    epoch = registrations_[size_t(Callback::kDeviceConfig)].epoch;
  }
  // Full native D9090 consumes only R4handle/R5class; R3 is ignored. Use the
  // actual binding identity, not an invented config packet or user index.
  const std::array<uint32_t, 3> args{device->binding_identity, device->handle,
                                     0};
  return QueueCallback(
      Callback::kDeviceConfig, args,
      [self, device, revision, epoch](bool executed, uint32_t result) {
        bool failed = false;
        {
          std::lock_guard lock(self->mutex_);
          const bool current =
              !self->stopping_ && device->connected &&
              self->slots_[device->slot] == device &&
              self->registrations_[size_t(Callback::kDeviceConfig)].epoch ==
                  epoch;
          failed = current && (!executed || result != 0);
          if (current && executed && result == 0) {
            device->config_delivered_revision =
                std::max(device->config_delivered_revision, revision);
          } else if (device->config_announced_revision == revision &&
                     device->config_announced_epoch == epoch) {
            device->config_announced_revision = 0;
          }
        }
        if (failed) {
          self->ProtocolFailed(device->handle, 4);
        }
      },
      0,
      [self, device] { return self->DeviceCallbackEligible(device, false); });
}

bool NativeInputDriver::RefreshDevices() {
  auto poll = AcquirePoll();
  if (!poll || !IsCurrent()) {
    return false;
  }
  if (!config_.validate_device_context()) {
    return false;
  }
  std::lock_guard refresh_lock(refresh_mutex_);
  {
    std::lock_guard lock(mutex_);
    // Notifications may precede actual bootstrap registration. They grant no
    // device creation and are harmless; SetCallback does the initial sample.
    if (!registrations_[size_t(Callback::kUserBinding)].address) {
      return true;
    }
    if (protocol_failed_) {
      return false;
    }
  }
  std::array<bool, XUserMaxUserCount> connected{};
  std::array<hid::X_INPUT_CAPABILITIES, XUserMaxUserCount> capabilities{};
  auto* input = config_.kernel->emulator()->input_system();
  {
    auto lock = input->lock();
    for (uint32_t slot = 0; slot < XUserMaxUserCount; ++slot) {
      connected[slot] =
          input->GetCapabilities(slot, hid::InputType::Controller,
                                 &capabilities[slot]) == X_ERROR_SUCCESS;
    }
  }
  std::vector<std::pair<std::shared_ptr<Device>, bool>> events;
  std::vector<std::pair<std::shared_ptr<Device>, uint64_t>> config_events;
  uint32_t connected_mask = 0;
  {
    std::lock_guard lock(mutex_);
    const auto registration = registrations_[size_t(Callback::kUserBinding)];
    if (stopping_ || !registration.address || protocol_failed_) {
      return false;
    }
    const auto config = registrations_[size_t(Callback::kDeviceConfig)];
    for (uint32_t slot = 0; slot < XUserMaxUserCount; ++slot) {
      auto& device = slots_[slot];
      if (device && (!connected[slot] || !device->connected)) {
        device->connected = false;
        events.emplace_back(device, true);
        handles_.erase(device->handle);
        device.reset();
      }
      if (!connected[slot]) {
        continue;
      }
      connected_mask |= 1u << slot;
      if (!device) {
        const uint32_t handle = AllocateToken(), identity = AllocateToken();
        if (!handle || !identity) {
          return false;
        }
        device = std::make_shared<Device>();
        device->handle = handle;
        device->binding_identity = identity;
        device->slot = slot;
        device->capabilities = capabilities[slot];
        handles_.emplace(handle, device);
      } else if (std::memcmp(&device->capabilities, &capabilities[slot],
                             sizeof(capabilities[slot])) != 0) {
        if (device->capability_revision == UINT64_MAX) {
          return false;
        }
        device->capabilities = capabilities[slot];
        ++device->capability_revision;
      }
      if (device->announced_registration != registration.epoch) {
        device->announced_registration = registration.epoch;
        events.emplace_back(device, false);
      }
      // New devices already get native class0 bind notification. Only real
      // capabilities changes warrant the separate native config callback.
      if (native_notifications_initialized_ && config.address &&
          device->capability_revision > device->config_delivered_revision &&
          (device->config_announced_revision != device->capability_revision ||
           device->config_announced_epoch != config.epoch)) {
        device->config_announced_revision = device->capability_revision;
        device->config_announced_epoch = config.epoch;
        config_events.emplace_back(device, device->capability_revision);
      }
    }
    enumeration_observed_ = true;
  }
  bool success = true;
  for (const auto& [device, removed] : events) {
    if (!AnnounceDevice(device, removed)) {
      std::lock_guard lock(mutex_);
      device->announced_registration = 0;
      success = false;
    }
  }
  for (const auto& [device, revision] : config_events) {
    if (!AnnounceConfiguration(device, revision)) {
      std::lock_guard lock(mutex_);
      device->config_announced_revision = 0;
      success = false;
    }
  }
  static Trace trace;
  trace.EmitTransition(connected_mask, "input", "devices_refreshed",
                       {{"input_generation", transport_generation_},
                        {"connected_mask", connected_mask},
                        {"binding_events", events.size()},
                        {"config_events", config_events.size()},
                        {"queued_healthy", success}});
  return success;
}

bool NativeInputDriver::ObserveNativeNotificationsInitialized() {
  auto poll = AcquirePoll();
  if (!poll || !IsCurrent()) {
    return false;
  }
  {
    std::lock_guard lock(mutex_);
    if (stopping_ || protocol_failed_) {
      return false;
    }
    native_notifications_initialized_ = true;
  }
  return RefreshDevices();
}

bool NativeInputDriver::SetNativeUiOwner(bool owns_input) {
  auto poll = AcquirePoll();
  if (!poll || !IsCurrent()) {
    return false;
  }
  auto* input = config_.kernel->emulator()->input_system();
  auto lock = input->lock();
  // Input -> Driver is the established lock order. Recheck after acquiring
  // Input: an earlier show/hide may have waited behind a newer event or stop.
  const auto foreground =
      config_.kernel->emulator()->title_execution_state().snapshot();
  {
    std::lock_guard driver_lock(mutex_);
    const bool current_owner =
        !stopping_ && !text_visibility_revoked_ && text_visibility_known_ &&
        text_visible_ &&
        config_.kernel->emulator()->OwnsTitleExecution(config_.kernel) &&
        foreground.phase == TitleExecutionState::Phase::kRunning &&
        foreground.generation == text_foreground_;
    if (owns_input != current_owner) {
      return false;
    }
  }
  return input->SetNativeInputOwner(transport_generation_, owns_input);
}

void NativeInputDriver::BeginStop() {
  bool started = false;
  {
    std::lock_guard lock(mutex_);
    if (stopping_) {
      return;
    }
    stopping_ = true;
    started = transport_started_;
    text_visible_ = false;
    text_owner_readable_ = false;
    text_owner_.reset();
    output_capture_b_down_.reset();
    for (auto& registration : registrations_) {
      registration.address = 0;
    }
  }
  std::shared_ptr<hid::NativeDeviceNotificationRundown> rundown;
  bool failed = false;
  // Close the subscriber outside DriverMutex/Input lock inversion. Its active
  // reservation begins BEFORE weak Driver acquisition and lasts through the
  // entire owner callback/strong-owner destruction, not merely Refresh Poll.
  try {
    if (started) {
      auto* input = config_.kernel->emulator()->input_system();
      if (input) {
        auto input_lock = input->lock();
        input->SetNativeInputOwner(transport_generation_, false);
        input->SetNativeTextOwner(transport_generation_, 0, UINT32_MAX, false);
        rundown = input->StopNativeDeviceNotifications(transport_generation_);
      }
      failed = !rundown;
    }
  } catch (...) {
    failed = true;
  }
  {
    std::lock_guard lock(mutex_);
    notification_rundown_ = std::move(rundown);
    notification_stop_failed_ = failed;
    // A concurrent BeginStop caller may have seen stopping_ already. Drain
    // must await this publication, never infer proof from a still-null token.
    notification_stop_published_ = true;
    drained_.notify_all();
  }
  // Keep TITLE capture until the coordinator's actual guest producer stop.
}
bool NativeInputDriver::Drain() {
  if (callback_driver == this) {
    return false;
  }
  BeginStop();
  std::shared_ptr<hid::NativeDeviceNotificationRundown> rundown;
  {
    std::unique_lock lock(mutex_);
    drained_.wait(lock, [&] { return notification_stop_published_; });
    if (notification_stop_failed_) {
      return false;
    }
    rundown = notification_rundown_;
  }
  // No input/Driver lock while waiting. Self/ancestor callback drain declines
  // rather than deadlocking or reporting a false Kernel lifetime proof.
  if (rundown && !rundown->Drain()) {
    return false;
  }
  std::unique_lock lock(mutex_);
  drained_.wait(lock, [&] { return !pending_callbacks_ && !active_polls_; });
  return true;
}

bool NativeInputDriver::ReleaseInputTransportAfterProducersStopped() {
  if (!Drain()) {
    return false;
  }
  bool started = false;
  {
    std::lock_guard lock(mutex_);
    if (!stopping_ || !notification_stop_published_ ||
        notification_stop_failed_ || pending_callbacks_ || active_polls_) {
      return false;
    }
    if (transport_released_) {
      return true;
    }
    transport_released_ = true;
    started = transport_started_;
  }
  auto* input = config_.kernel->emulator()->input_system();
  if (input && started) {
    auto lock = input->lock();
    input->EndNativeInputTransport(transport_generation_);
  }
  return true;
}

NativeInputDriver::Poll NativeInputDriver::AcquirePoll() {
  std::lock_guard lock(mutex_);
  if (stopping_) {
    return Poll();
  }
  ++active_polls_;
  return Poll(this);
}

NativeInputDriver::Poll::~Poll() {
  if (owner_) {
    std::lock_guard lock(owner_->mutex_);
    --owner_->active_polls_;
    owner_->drained_.notify_all();
  }
}

X_STATUS NativeInputDriver::GetDeviceStats(
    uint32_t handle, uint32_t selector,
    std::array<uint8_t, kVirtualDeviceStatsBytes>* stats, uint32_t* opaque) {
  auto poll = AcquirePoll();
  X_STATUS status = kDeviceNotConnected;
  uint32_t slot = 0;
  bool slot_known = false;
  const int exceptions =
      cvars::native_system_ui_trace ? std::uncaught_exceptions() : 0;
  TraceExit exit{[&] {
    if (!cvars::native_system_ui_trace) {
      return;
    }
    static Trace trace;
    const bool known = exceptions == std::uncaught_exceptions();
    if (known) {
      InputTrace(trace, config_, transport_generation_, "device_stats_return",
                 {{"device_handle", handle},
                  {"selector", selector},
                  {"return_status_known", 1},
                  {"status", status},
                  {"slot_known", slot_known},
                  {"slot", slot}});
    } else {
      InputTrace(trace, config_, transport_generation_, "device_stats_return",
                 {{"device_handle", handle},
                  {"selector", selector},
                  {"return_status_known", 0},
                  {"slot_known", slot_known}});
    }
  }};
  if (!poll || !stats || !AuthorizeCurrentCaller()) {
    return status;
  }
  if (selector > 1) {
    return status = X_STATUS_NOT_SUPPORTED;
  }
  std::shared_ptr<Device> device;
  {
    std::lock_guard lock(mutex_);
    const auto found = handles_.find(handle);
    if (stopping_ || found == handles_.end() || !found->second->connected) {
      return status;
    }
    device = found->second;
    slot = device->slot;
    slot_known = true;
  }
  hid::X_INPUT_CAPABILITIES capabilities{};
  X_RESULT result = X_ERROR_DEVICE_NOT_CONNECTED;
  try {
    auto* input = config_.kernel->emulator()->input_system();
    auto lock = input->lock();
    // Physical presence only: no UI visibility/modal/counter/Running gate.
    result =
        input->GetCapabilities(slot, hid::InputType::Controller, &capabilities);
  } catch (...) {
  }
  std::array<uint8_t, kVirtualDeviceStatsBytes> sampled{};
  // Explicit ordinary virtual RGC model, not retail counters/battery/radio.
  xe::store_and_swap<uint32_t>(sampled.data() + 0xC, 0x10000000);
  xe::store_and_swap<uint32_t>(sampled.data() + 0x10, slot);
  {
    std::lock_guard lock(mutex_);
    const auto found = handles_.find(handle);
    if (stopping_ || result != X_ERROR_SUCCESS || found == handles_.end() ||
        found->second != device || !device->connected) {
      return status;
    }
    *stats = sampled;
    if (opaque) {
      *opaque = 0;  // Defined FALSE for the ordinary virtual profile.
    }
  }
  return status = X_STATUS_SUCCESS;
}
X_STATUS NativeInputDriver::TakeFailedConnectionOrBind(
    uint32_t* actual_handle) {
  if (actual_handle) {
    *actual_handle = 0;
  }
  auto poll = AcquirePoll();
  X_STATUS status = kDeviceNotConnected;
  uint32_t value = 0;
  const int exceptions =
      cvars::native_system_ui_trace ? std::uncaught_exceptions() : 0;
  TraceExit exit{[&] {
    if (!cvars::native_system_ui_trace) {
      return;
    }
    static Trace trace;
    const bool known = exceptions == std::uncaught_exceptions();
    if (known) {
      InputTrace(trace, config_, transport_generation_, "failed_binding_query",
                 {{"return_status_known", 1},
                  {"status", status},
                  {"returned_handle", value}});
    } else {
      InputTrace(trace, config_, transport_generation_, "failed_binding_query",
                 {{"return_status_known", 0}});
    }
  }};
  if (!poll || !actual_handle || !AuthorizeCurrentCaller()) {
    return status;
  }
  {
    std::lock_guard lock(mutex_);
    if (stopping_) {
      return status;
    }
    value = std::exchange(failed_connection_or_bind_, 0);
    *actual_handle = value;
  }
  return status = X_STATUS_SUCCESS;
}

X_STATUS NativeInputDriver::ReadState(uint32_t handle,
                                      hid::X_INPUT_STATE* state) {
  auto poll = AcquirePoll();
  uint32_t trace_reason = 1, trace_result = kDeviceNotConnected, trace_slot = 0;
  bool trace_slot_known = false, trace_allowed = false,
       trace_allowed_known = false;
  bool trace_sample_attempted = false, trace_sample_returned = false;
  const int trace_uncaught =
      cvars::native_system_ui_trace ? std::uncaught_exceptions() : 0;
  TraceExit trace_exit{[&] {
    if (!cvars::native_system_ui_trace) {
      return;
    }
    static Trace trace;
    const bool returned_normally = std::uncaught_exceptions() == trace_uncaught;
    const uint64_t key =
        uint64_t(trace_result) | (uint64_t(trace_reason) << 32) |
        (uint64_t(trace_allowed) << 48) | (uint64_t(trace_slot_known) << 49) |
        (uint64_t(trace_allowed_known) << 50);
    if (!returned_normally) {
      trace.EmitTransition(key | (uint64_t(1) << 51), "input",
                           "read_state_scope_exception",
                           {{"input_generation", transport_generation_},
                            {"domain_id", config_.generation},
                            {"device_handle", handle},
                            {"returned_normally", 0},
                            {"return_status_known", 0},
                            {"reason", trace_reason},
                            {"sample_attempted", trace_sample_attempted},
                            {"sample_returned", trace_sample_returned}});
      return;
    }
    const auto emit = [&](std::initializer_list<Trace::Field> extra) noexcept {
      trace.EmitTransition(key, "input", "read_state_return",
                           {{"input_generation", transport_generation_},
                            {"returned_normally", 1},
                            {"return_status_known", 1},
                            {"domain_id", config_.generation},
                            {"device_handle", handle},
                            {"status", trace_result},
                            {"reason", trace_reason},
                            {"physical_allowed_known", trace_allowed_known},
                            {"sample_attempted", trace_sample_attempted},
                            {"sample_returned", trace_sample_returned}},
                           extra);
    };
    if (trace_allowed_known && trace_slot_known) {
      emit({{"slot", trace_slot}, {"physical_allowed", trace_allowed}});
    } else if (trace_slot_known) {
      emit({{"slot", trace_slot}});
    } else {
      emit({});
    }
  }};
  if (!poll || !state || !AuthorizeCurrentCaller()) {
    trace_reason = !poll ? 1 : !state ? 2 : 3;
    return kDeviceNotConnected;
  }
  std::shared_ptr<Device> device;
  {
    std::lock_guard lock(mutex_);
    const auto found = handles_.find(handle);
    if (stopping_ || found == handles_.end() || !found->second->connected) {
      trace_reason = stopping_ ? 4 : found == handles_.end() ? 5 : 6;
      return kDeviceNotConnected;
    }
    device = found->second;
    trace_slot = device->slot;
    trace_slot_known = true;
  }
  X_RESULT result = X_ERROR_DEVICE_NOT_CONNECTED;
  hid::X_INPUT_STATE sampled{};
  bool capture_b_sample_allowed = false;
  bool allowed = false;
  try {
    allowed = config_.allow_physical_input();
    trace_allowed = allowed;
    trace_allowed_known = true;
  } catch (...) {
  }
  try {
    auto* input = config_.kernel->emulator()->input_system();
    auto lock = input->lock();
    trace_sample_attempted = true;
    result = input->GetStateForNativeDriver(
        device->slot, hid::InputType::Controller, &sampled);
    trace_sample_returned = true;
    if (result == X_ERROR_SUCCESS) {
      const bool blocked = input->IsUIInputBlocked();
      if (cvars::native_system_ui_output_capture_b_down) {
        capture_b_sample_allowed = allowed && !blocked;
      }
      if (blocked || !allowed) {
        const uint32_t packet = sampled.packet_number;
        sampled = {};
        sampled.packet_number = packet;
      }
    }
  } catch (...) {
  }
  {
    std::lock_guard lock(mutex_);
    if (result != X_ERROR_SUCCESS) {
      device->connected = false;
    }
    if (stopping_ || !device->connected || result != X_ERROR_SUCCESS) {
      trace_reason = stopping_                   ? 7
                     : !trace_sample_returned    ? 8
                     : result != X_ERROR_SUCCESS ? 9
                                                 : 10;
      return kDeviceNotConnected;
    }
    *state = sampled;
    if (cvars::native_system_ui_output_capture_b_down) {
      if (!capture_b_sample_allowed) {
        device->capture_b_sample_known = false;
        device->capture_b_down = false;
        if (output_capture_b_down_ &&
            output_capture_b_down_->device_handle == device->handle) {
          output_capture_b_down_.reset();
        }
      } else {
        const bool down =
            (sampled.gamepad.buttons & hid::X_INPUT_GAMEPAD_B) != 0;
        const bool edge =
            device->capture_b_sample_known && !device->capture_b_down && down;
        device->capture_b_sample_known = true;
        device->capture_b_down = down;
        if (!down && output_capture_b_down_ &&
            output_capture_b_down_->device_handle == device->handle) {
          output_capture_b_down_.reset();
        }
        const auto registration =
            registrations_[size_t(Callback::kUserBinding)];
        if (edge && !output_capture_b_down_ && text_visibility_known_ &&
            text_visible_ && !text_visibility_revoked_ && text_foreground_ &&
            text_visibility_sequence_ && registration.address == 0x816D9060 &&
            device->native_user_registration == registration.epoch &&
            device->native_user < XUserMaxUserCount &&
            output_capture_b_sequence_ != UINT64_MAX) {
          const auto domain = config_.dispatcher->domain();
          output_capture_b_down_ =
              BDownCaptureObservation{transport_generation_,
                                      config_.generation,
                                      domain->root().generation,
                                      text_foreground_,
                                      text_visibility_sequence_,
                                      ++output_capture_b_sequence_,
                                      registration.epoch,
                                      domain->root().hmodule,
                                      device->slot,
                                      device->native_user,
                                      device->handle,
                                      device->binding_identity,
                                      sampled.packet_number,
                                      sampled.gamepad.buttons};
        }
      }
    }
  }
  static std::array<Trace, XUserMaxUserCount> traces;
  traces[device->slot].EmitTransition(
      uint64_t(sampled.gamepad.buttons) | (uint64_t(allowed) << 32), "input",
      "physical_state",
      {{"input_generation", transport_generation_},
       {"domain_id", config_.generation},
       {"slot", device->slot},
       {"device_handle", handle},
       {"native_read_status", X_STATUS_SUCCESS},
       {"allowed", allowed},
       {"packet", sampled.packet_number},
       {"buttons", sampled.gamepad.buttons}});
  trace_reason = 0;
  trace_result = X_STATUS_SUCCESS;
  return X_STATUS_SUCCESS;
}

std::optional<NativeInputDriver::BDownCaptureObservation>
NativeInputDriver::GetBDownCaptureObservation() noexcept {
  try {
    auto poll = AcquirePoll();
    if (!cvars::native_system_ui_output_capture_b_down || !poll ||
        !IsCurrent()) {
      return {};
    }
    std::lock_guard lock(mutex_);
    if (stopping_ || text_visibility_revoked_ || !text_visibility_known_ ||
        !text_visible_ || !output_capture_b_down_) {
      return {};
    }
    const auto& event = *output_capture_b_down_;
    const auto found = handles_.find(event.device_handle);
    const auto registration = registrations_[size_t(Callback::kUserBinding)];
    if (found == handles_.end() || !found->second->connected ||
        found->second->slot >= slots_.size() ||
        slots_[found->second->slot] != found->second ||
        !found->second->capture_b_down ||
        found->second->native_user != event.native_user ||
        found->second->binding_identity != event.binding_identity ||
        found->second->native_user_registration != event.binding_epoch ||
        registration.epoch != event.binding_epoch ||
        registration.address != 0x816D9060 ||
        text_foreground_ != event.foreground_generation ||
        text_visibility_sequence_ != event.visibility_sequence) {
      return {};
    }
    return event;
  } catch (...) {
    return {};
  }
}

X_STATUS NativeInputDriver::GetCapabilities(uint32_t handle,
                                            hid::X_INPUT_CAPABILITIES* caps) {
  auto poll = AcquirePoll();
  if (!poll || !caps || !AuthorizeCurrentCaller()) {
    return kDeviceNotConnected;
  }
  uint32_t slot = 0;
  {
    std::lock_guard lock(mutex_);
    const auto found = handles_.find(handle);
    if (stopping_ || found == handles_.end() || !found->second->connected) {
      return kDeviceNotConnected;
    }
    slot = found->second->slot;
  }
  hid::X_INPUT_CAPABILITIES sampled{};
  X_RESULT result = X_ERROR_DEVICE_NOT_CONNECTED;
  try {
    auto* input = config_.kernel->emulator()->input_system();
    auto lock = input->lock();
    result = input->GetCapabilities(slot, hid::InputType::Controller, &sampled);
  } catch (...) {
  }
  std::lock_guard lock(mutex_);
  const auto device = handles_.find(handle);
  if (result != X_ERROR_SUCCESS && device != handles_.end()) {
    device->second->connected = false;
  }
  if (stopping_ || !handles_.contains(handle) || result != X_ERROR_SUCCESS) {
    return kDeviceNotConnected;
  }
  *caps = sampled;
  return X_STATUS_SUCCESS;
}

bool NativeInputDriver::ReadNativeTextUser(const cpu::ppc::PPCContext& context,
                                           uint32_t* user, uint64_t* foreground,
                                           bool* ui_task) const {
  if (!ui_task) {
    return false;
  }
  *ui_task = false;
  if (!user || !foreground || !XThread::IsInThread() || !IsCurrent()) {
    return false;
  }
  auto* thread = XThread::GetCurrentThread();
  auto* kernel = config_.kernel;
  if (thread->kernel_state() != kernel ||
      thread->thread_state()->context() != &context ||
      context.kernel_state != kernel ||
      context.processor != kernel->processor() ||
      context.thread_state != thread->thread_state() ||
      thread->native_frame_thread_identity().process_type !=
          X_PROCTYPE_SYSTEM ||
      !CanonicalGuestPointer(context.r[13], thread->pcr_ptr())) {
    return false;
  }
  const auto actual = kernel->emulator()->title_execution_state().snapshot();
  if (!kernel->emulator()->OwnsTitleExecution(kernel) ||
      actual.phase != TitleExecutionState::Phase::kRunning) {
    return false;
  }
  const auto domain = config_.dispatcher->domain();
  auto operation = domain->Acquire();
  auto module = kernel->GetUserModuleForAddress(0x816F1DC0);
  if (!operation || !module ||
      module->module_format() != UserModule::kModuleFormatXex ||
      module->hmodule_ptr() != domain->root().hmodule ||
      module->load_generation() != domain->root().generation) {
    return false;
  }
  auto code = kernel->AcquireUserModuleExecutionLease(module);
  if (!code) {
    return false;
  }
  uint32_t task = 0, scope_stage = 1;
  NativeUiTextFocusData data;
  const bool inspected = [&] {
    auto global = global_critical_region::AcquireDirect();
    auto* memory = kernel->memory();
    if (!TextProfileMatches(*kernel, *module) ||
        memory->QueryRangeAccess(0x81AA20E0, 0xB8) ==
            memory::PageAccess::kNoAccess) {
      return false;
    }
    const auto readable = [&](uint32_t at, uint32_t bytes) {
      return at && uint64_t(at) + bytes <= 0x100000000ull &&
             memory->QueryRangeAccess(at, bytes) !=
                 memory::PageAccess::kNoAccess;
    };
    const auto word = [&](uint32_t at) {
      return xe::load_and_swap<uint32_t>(memory->TranslateVirtual(at));
    };
    // Same actual UI Task, not merely a SYSTEM process or a SYSTEM input flag.
    // XThread's checked storage reader validates dynamic TLS bounds/mapping.
    scope_stage = 2;
    uint32_t tls = 0;
    if (!readable(0x81A82570, 4) ||
        !thread->GetTLSValue(word(0x81A82570), &tls) || !readable(tls, 4)) {
      return false;
    }
    const uint32_t work = word(tls);
    if (!readable(work, 0x10)) {
      return false;
    }
    task = word(work + 0xC);
    const uint32_t tagged = word(0x81AA2108);
    scope_stage = 3;
    if (!task || (task & 3) || (tagged & 3) != 1 || tagged != (task | 1u) ||
        !readable(task, 0x34) || word(task + 0x30) != 0x816CE108) {
      return false;
    }
    *ui_task = true;
    scope_stage = 4;
    // The resident callback serializes scene creation/destruction. Within this
    // no-PPC/no-yield borrow, fresh handle generations and parent attachment
    // validate the live child; a raw global alias is never a scene lease.
    data = ReadNativeUiTextFocusData(*memory);
    scope_stage = 5;
    if (!data.compatible() || word(work + 0xC) != task ||
        word(0x81AA2108) != tagged) {
      return false;
    }
    scope_stage = 6;
    return true;
  }();
  static Trace trace;
  trace.EmitTransition((uint64_t(data.status) << 32) | data.user, "input",
                       "text_focus_observed",
                       {{"input_generation", transport_generation_},
                        {"foreground_generation", actual.generation},
                        {"status", uint32_t(data.status)},
                        {"user_observed", data.compatible()},
                        {"ui_task_known", *ui_task},
                        {"borrow_valid", inspected},
                        {"scope_stage", scope_stage},
                        {"native_user", data.user},
                        {"native_task", task},
                        {"context", data.context},
                        {"scene", data.scene},
                        {"parent_handle", data.parent_handle},
                        {"child_handle", data.child_handle}});
  if (!inspected) {
    return false;
  }
  *user = data.user;
  *foreground = actual.generation;
  return true;
}

void NativeInputDriver::ObserveNativeUiVisibility(
    const NativeSystemUiVisibilityEvent& event) noexcept {
  try {
    auto poll = AcquirePoll();
    const auto domain = config_.dispatcher->domain();
    if (!poll || !IsCurrent() || event.domain_id() != domain->id() ||
        event.root_load_generation() != domain->root().generation ||
        !event.sequence()) {
      return;
    }
    const auto actual =
        config_.kernel->emulator()->title_execution_state().snapshot();
    if (!config_.kernel->emulator()->OwnsTitleExecution(config_.kernel) ||
        actual.phase != TitleExecutionState::Phase::kRunning ||
        actual.generation != event.foreground_generation()) {
      return;
    }
    {
      std::lock_guard lock(mutex_);
      if (stopping_ || text_visibility_revoked_ ||
          event.sequence() <= text_visibility_sequence_) {
        return;
      }
      text_visibility_known_ = true;
      text_visible_ = event.visible();
      text_foreground_ = actual.generation;
      text_visibility_sequence_ = event.sequence();
      if (!text_visible_) {
        text_owner_.reset();
        text_owner_readable_ = false;
        output_capture_b_down_.reset();
      }
    }
    // Native counters still choose TITLE/SYSTEM audiences. This host owner
    // only preserves the common held-button handoff at genuine visibility
    // close; native UI physical reads remain raw until that close.
    SetNativeUiOwner(event.visible());
    // Visibility can close a host lease. It supplies neither user nor audience.
    ApplyHostTextOwner();
    PumpTextBinding();
  } catch (...) {
  }
}

void NativeInputDriver::RevokeNativeTextVisibility() noexcept {
  try {
    auto poll = AcquirePoll();
    if (!poll) {
      return;  // BeginStop already clears the host claim.
    }
    {
      std::lock_guard lock(mutex_);
      text_visibility_revoked_ = true;
      text_visibility_known_ = false;
      text_visible_ = false;
      text_owner_readable_ = false;
      text_owner_.reset();
      output_capture_b_down_.reset();
    }
    SetNativeUiOwner(false);
    ApplyHostTextOwner();
    PumpTextBinding();
  } catch (...) {
  }
}

bool NativeInputDriver::RefreshNativeTextOwner() noexcept {
  try {
    auto poll = AcquirePoll();
    if (!poll || !AuthorizeCurrentCaller() || !XThread::IsInThread()) {
      return false;
    }
    auto* input = config_.kernel->emulator()->input_system();
    bool available;
    {
      auto input_lock = input->lock();
      available = input->GetNativeTextState(transport_generation_).available;
    }
    {
      std::lock_guard lock(mutex_);
      if (stopping_ || text_visibility_revoked_ ||
          !native_notifications_initialized_) {
        return false;
      }
      if (!text_visibility_known_ || !text_visible_ || !available) {
        text_owner_.reset();
        text_owner_readable_ = false;
      }
    }
    if (!available) {
      ApplyHostTextOwner();
      PumpTextBinding();
      return false;
    }
    {
      std::lock_guard lock(mutex_);
      if (!text_visible_) {
        return false;
      }
    }
    uint32_t user = UINT32_MAX;
    uint64_t foreground = 0;
    bool ui_task = false;
    const bool known = ReadNativeTextUser(
        *XThread::GetCurrentThread()->thread_state()->context(), &user,
        &foreground, &ui_task);
    if (!ui_task) {
      return false;  // Unknown callers cannot move the owner lease.
    }
    bool selected = false;
    {
      std::lock_guard lock(mutex_);
      if (stopping_) {
        return false;
      }
      text_owner_readable_ = false;
      if (known && text_visible_ && foreground == text_foreground_) {
        const auto binding = registrations_[size_t(Callback::kUserBinding)];
        uint32_t group = 0;
        bool ambiguous = false;
        for (const auto& device : slots_) {
          if (!device || !device->connected ||
              device->native_user_registration != binding.epoch ||
              device->native_user != user) {
            continue;
          }
          ambiguous |= group && group != device->binding_identity;
          group = device->binding_identity;
        }
        // First supported endpoint follows a genuine already-adopted group.
        // No fixed passthrough slot, synthesized controller or guessed user.
        if (!ambiguous && group && binding.address == 0x816D9060) {
          if (!text_owner_ || text_owner_->user != user ||
              text_owner_->group != group ||
              text_owner_->foreground != foreground ||
              text_owner_->binding_epoch != binding.epoch) {
            if (text_owner_revision_ == UINT64_MAX) {
              return false;
            }
            text_owner_ = TextOwner{user, group, foreground, binding.epoch,
                                    ++text_owner_revision_};
          }
          text_owner_readable_ = true;
          selected = true;
        } else {
          text_owner_.reset();
        }
      }
    }
    ApplyHostTextOwner();
    PumpTextBinding();
    return selected;
  } catch (...) {
    return false;
  }
}

void NativeInputDriver::ApplyHostTextOwner() noexcept {
  try {
    auto* input = config_.kernel->emulator()->input_system();
    uint64_t revision = 0;
    uint32_t user = UINT32_MAX;
    bool enabled = false;
    bool applied = false;
    {
      auto input_lock = input->lock();
      {
        // Input->Driver order only; no caller holds DriverMutex across Input.
        // Recheck under Input lock so a late completion cannot reenable
        // afterStop.
        std::lock_guard lock(mutex_);
        if (!stopping_ && !text_visibility_revoked_ && text_visible_ &&
            text_owner_) {
          revision = text_owner_->revision;
          user = text_owner_->user;
          enabled = text_owner_readable_ && text_endpoint_ &&
                    !text_endpoint_->failed && text_endpoint_->present &&
                    text_endpoint_->owner == *text_owner_ &&
                    text_endpoint_->assigned_user == user;
        }
      }
      // A nonzero revision claims exclusive keyboard mappings even while ADD is
      // pending. enabled controls edge/commit admission, not group fabrication.
      applied = input->SetNativeTextOwner(transport_generation_, revision, user,
                                          enabled);
    }
    static Trace trace;
    trace.EmitTransition((revision << 1) | uint64_t(enabled), "input",
                         "text_owner_mode",
                         {{"input_generation", transport_generation_},
                          {"domain_id", config_.generation},
                          {"owner_revision", revision},
                          {"native_user", user},
                          {"requested_enabled", enabled},
                          {"host_owner_applied", applied}});
  } catch (...) {
  }
}

bool NativeInputDriver::QueueTextBinding(
    const std::shared_ptr<TextEndpoint>& endpoint, TextOwner owner,
    bool removed) {
  struct Output {
    Memory* memory;
    uint32_t address = 0;
    ~Output() {
      if (address) {
        memory->SystemHeapFree(address);
      }
    }
  };
  struct Admission {
    std::atomic<bool> rejected{false};
  };
  auto output = std::make_shared<Output>();
  output->memory = config_.kernel->memory();
  output->address = output->memory->SystemHeapAlloc(4);
  if (!output->address) {
    return false;
  }
  *output->memory->TranslateVirtual<uint8_t*>(output->address) =
      uint8_t(owner.user);
  auto admission = std::make_shared<Admission>();
  auto self = shared_from_this();
  const std::array<uint32_t, 5> args{owner.group, endpoint->handle, 2,
                                     removed ? 1u : 0u, output->address};
  const auto eligible = [self, endpoint, owner, removed, admission] {
    bool allowed = self->IsCurrent() && self->config_.validate_device_context();
    {
      std::lock_guard lock(self->mutex_);
      const auto registration =
          self->registrations_[size_t(Callback::kUserBinding)];
      allowed &= !self->stopping_ && !endpoint->failed &&
                 self->native_notifications_initialized_ &&
                 registration.address == 0x816D9060 &&
                 registration.epoch == owner.binding_epoch &&
                 self->text_endpoint_ == endpoint && endpoint->pending;
      if (!removed) {
        allowed &= self->text_owner_ && *self->text_owner_ == owner &&
                   self->text_visible_;
      }
    }
    if (!allowed) {
      admission->rejected.store(true, std::memory_order_release);
    }
    return allowed;
  };
  static Trace request_trace;
  InputTrace(request_trace, config_, transport_generation_,
             "text_binding_request",
             {{"device_handle", endpoint->handle},
              {"binding_identity", owner.group},
              {"requested_user", owner.user},
              {"owner_revision", owner.revision},
              {"removed", removed}});
  return QueueCallback(
      Callback::kUserBinding, args,
      [self, endpoint, owner, removed, output, admission](bool executed,
                                                          uint32_t result) {
        uint32_t assigned = UINT32_MAX;
        if (executed && result == 0) {
          auto global = global_critical_region::AcquireDirect();
          assigned = *output->memory->TranslateVirtual<const uint8_t*>(
              output->address);
        }
        bool uncertain = false, adopted = false, notify_protocol = false;
        {
          std::lock_guard lock(self->mutex_);
          endpoint->pending = false;
          if (executed && result == 0 && assigned < XUserMaxUserCount) {
            if (removed) {
              uncertain = assigned != endpoint->assigned_user;
              if (!uncertain) {
                endpoint->present = false;
                endpoint->assigned_user = UINT32_MAX;
              }
            } else {
              // Record an actual successful ADD even if the desired owner
              // changed meanwhile; genuine REMOVE must clean that native fact.
              endpoint->present = true;
              endpoint->assigned_user = assigned;
              endpoint->owner = owner;
              endpoint->owner.user = assigned;
              adopted = assigned == owner.user;
              if (!adopted) {
                endpoint->rejected_revision = owner.revision;
              }
            }
          } else if (!executed &&
                     admission->rejected.load(std::memory_order_acquire)) {
            // Typed eligibility rejected BEFORE PPC. Do not spin/retry a
            // closed registration/profile. A newer desired revision may add.
            if (removed) {
              endpoint->failed = true;
            } else {
              endpoint->rejected_revision = owner.revision;
            }
          } else if (!removed && executed && int32_t(result) < 0) {
            // A logical UI association refusal is not a physical device fault.
            endpoint->rejected_revision = owner.revision;
          } else {
            uncertain = true;
          }
          endpoint->failed |= uncertain;
          // Stop retains any unknown native fact until actual CPU/Kernel
          // reclamation; it does not run cleanup PPC or claim an unbind ACK.
          notify_protocol = uncertain && !self->stopping_;
        }
        static Trace complete_trace;
        InputTrace(complete_trace, self->config_, self->transport_generation_,
                   "text_binding_complete",
                   {{"device_handle", endpoint->handle},
                    {"removed", removed},
                    {"owner_revision", owner.revision},
                    {"executor_succeeded", executed},
                    {"result_known", executed},
                    {"raw_result", result},
                    {"assigned_user_known", assigned < XUserMaxUserCount},
                    {"assigned_user", assigned},
                    {"adopted", adopted},
                    {"uncertain", uncertain}});
        if (notify_protocol) {
          self->ProtocolFailed(endpoint->handle, 20);
        }
        self->ApplyHostTextOwner();
        self->PumpTextBinding();
      },
      0, eligible);
}

void NativeInputDriver::PumpTextBinding() noexcept {
  auto poll = AcquirePoll();
  if (!poll) {
    return;
  }
  {
    std::lock_guard lock(mutex_);
    if (text_pump_active_) {
      return;
    }
    text_pump_active_ = true;
  }
  try {
    for (;;) {
      std::shared_ptr<TextEndpoint> endpoint;
      TextOwner owner;
      bool removed = false;
      {
        std::lock_guard lock(mutex_);
        // Clearing the pump flag in this same critical section prevents a
        // completion from losing its wake between pending-check and return.
        if (stopping_ || !native_notifications_initialized_ ||
            (text_endpoint_ &&
             (text_endpoint_->pending || text_endpoint_->failed))) {
          text_pump_active_ = false;
          return;
        }
        if (!text_endpoint_ && text_owner_) {
          text_endpoint_ = std::make_shared<TextEndpoint>();
          text_endpoint_->handle = AllocateToken();
          if (!text_endpoint_->handle) {
            text_endpoint_->failed = true;
            text_pump_active_ = false;
            return;
          }
        }
        endpoint = text_endpoint_;
        if (!endpoint) {
          text_pump_active_ = false;
          return;
        }
        if (endpoint->present &&
            (!text_owner_ || endpoint->owner != *text_owner_ ||
             endpoint->assigned_user != text_owner_->user)) {
          owner = endpoint->owner;
          owner.user = endpoint->assigned_user;
          removed = true;
        } else if (!endpoint->present && text_owner_ &&
                   endpoint->rejected_revision != text_owner_->revision) {
          owner = *text_owner_;
        } else {
          text_pump_active_ = false;
          return;
        }
        endpoint->pending = true;
      }
      bool queued = false;
      try {
        queued = QueueTextBinding(endpoint, owner, removed);
      } catch (...) {
      }
      if (!queued) {
        std::lock_guard lock(mutex_);
        endpoint->pending = false;
        endpoint->rejected_revision = owner.revision;
        if (removed) {
          endpoint->failed = true;
        }
      }
      ApplyHostTextOwner();
    }
  } catch (...) {
    std::lock_guard lock(mutex_);
    text_owner_readable_ = false;
    text_pump_active_ = false;
  }
}

X_RESULT NativeInputDriver::ReadText(uint32_t user,
                                     hid::X_INPUT_KEYSTROKE* stroke,
                                     const cpu::ppc::PPCContext& context) {
  auto poll = AcquirePoll();
  X_RESULT result = X_ERROR_DEVICE_NOT_CONNECTED;
  size_t audience = 0;
  uint64_t revision = 0, invalidation_epoch = 0;
  bool audience_known = false, host_state_known = false;
  bool host_available = false, host_focused = false, host_enabled = false;
  const int uncaught =
      cvars::native_system_ui_trace ? std::uncaught_exceptions() : 0;
  TraceExit trace_exit{[&] {
    if (!cvars::native_system_ui_trace) {
      return;
    }
    static Trace trace;
    const bool returned = std::uncaught_exceptions() == uncaught;
    const uint64_t key = (uint64_t(audience_known ? audience + 1 : 0) << 32) |
                         (returned ? result : UINT32_MAX);
    if (returned) {
      trace.EmitTransition(key, "input", "text_read_result",
                           {{"input_generation", transport_generation_},
                            {"domain_id", config_.generation},
                            {"native_user", user},
                            {"audience_known", audience_known},
                            {"audience", audience},
                            {"owner_revision", revision},
                            {"invalidation_epoch", invalidation_epoch},
                            {"host_state_known", host_state_known},
                            {"host_available", host_available},
                            {"host_focused", host_focused},
                            {"host_enabled", host_enabled},
                            {"result_known", 1},
                            {"native_read_status", result}});
    } else {
      trace.EmitTransition(key, "input", "text_read_exception",
                           {{"input_generation", transport_generation_},
                            {"domain_id", config_.generation},
                            {"native_user", user},
                            {"audience_known", audience_known},
                            {"result_known", 0}});
    }
  }};
  auto read = [&]() -> X_RESULT {
    if (!poll || !stroke || user >= XUserMaxUserCount ||
        !AuthorizeCurrentCaller() || !XThread::IsInThread() ||
        XThread::GetCurrentThread()->thread_state()->context() != &context ||
        context.lr != 0x00000000816F1E48ull) {
      return X_ERROR_DEVICE_NOT_CONNECTED;
    }
    if (CanonicalGuestPointer(context.r[31], 0x81AAC2E0)) {
      audience = 1;
    } else if (CanonicalGuestPointer(context.r[31], 0x81AAC3C8)) {
      audience = 0;
    } else {
      return X_ERROR_DEVICE_NOT_CONNECTED;
    }
    // Refresh proves actual context equality, exact body/import/cohort and same
    // resident UI Task before any queue or invalidation state can be consumed.
    if (!RefreshNativeTextOwner()) {
      return X_ERROR_DEVICE_NOT_CONNECTED;
    }
    audience_known = true;
    uint32_t native_handle;
    {
      std::lock_guard lock(mutex_);
      if (stopping_ || !text_owner_readable_ || !text_owner_ ||
          text_owner_->user != user || !text_endpoint_ ||
          text_endpoint_->failed || !text_endpoint_->present ||
          text_endpoint_->owner != *text_owner_ ||
          text_endpoint_->assigned_user != user) {
        return X_ERROR_DEVICE_NOT_CONNECTED;
      }
      revision = text_owner_->revision;
      native_handle = text_endpoint_->handle;
    }
    {
      auto global = global_critical_region::AcquireDirect();
      auto* memory = config_.kernel->memory();
      const uint32_t record = 0x81AA3D90 + user * 0x40;
      if (memory->QueryRangeAccess(record, 0x20) ==
              memory::PageAccess::kNoAccess ||
          !xe::load_and_swap<uint32_t>(
              memory->TranslateVirtual(record + 0x18)) ||
          xe::load_and_swap<uint32_t>(
              memory->TranslateVirtual(record + 0x1C)) != native_handle) {
        return X_ERROR_DEVICE_NOT_CONNECTED;
      }
    }
    bool allowed = false;
    try {
      allowed = config_.allow_physical_input();
    } catch (...) {
    }
    auto* input = config_.kernel->emulator()->input_system();
    auto input_lock = input->lock();
    bool changed;
    {
      std::lock_guard lock(mutex_);
      changed = physical_text_allowed_ != allowed;
      physical_text_allowed_ = allowed;
    }
    if (changed) {
      input->ResetNativeText(transport_generation_);
    }
    const auto state = input->GetNativeTextState(transport_generation_);
    host_state_known = true;
    host_available = state.available;
    host_focused = state.focused;
    host_enabled = state.enabled;
    invalidation_epoch = state.invalidation_epoch;
    if (!allowed || !state.available || !state.focused || !state.enabled ||
        state.user != user || state.owner_revision != revision ||
        !state.invalidation_epoch || input->IsUIInputBlocked()) {
      return X_ERROR_DEVICE_NOT_CONNECTED;
    }
    {
      std::lock_guard lock(mutex_);
      if (stopping_ || !text_visible_ || !text_owner_ ||
          text_owner_->revision != revision || !text_owner_readable_) {
        return X_ERROR_DEVICE_NOT_CONNECTED;
      }
      if (text_epochs_[user][audience] != state.invalidation_epoch) {
        text_epochs_[user][audience] = state.invalidation_epoch;
        return X_ERROR_DEVICE_NOT_CONNECTED;
      }
    }
    return input->GetNativeTextKeystroke(transport_generation_, user, stroke);
  };
  result = read();
  return result;
}

bool NativeInputDriver::HasText(uint32_t user) {
  auto poll = AcquirePoll();
  if (!poll || user >= XUserMaxUserCount || !AuthorizeCurrentCaller()) {
    return false;
  }
  std::lock_guard lock(mutex_);
  return !stopping_ && text_endpoint_ && !text_endpoint_->failed &&
         text_endpoint_->present && text_endpoint_->assigned_user == user;
}

bool NativeInputDriver::GetTextKeyLocks(uint32_t user, uint16_t* word) {
  auto poll = AcquirePoll();
  if (!poll || !word || user >= XUserMaxUserCount ||
      !AuthorizeCurrentCaller()) {
    return false;
  }
  std::lock_guard lock(mutex_);
  if (stopping_ || !text_endpoint_ || text_endpoint_->failed ||
      !text_endpoint_->present || text_endpoint_->assigned_user != user) {
    return false;
  }
  *word = text_key_locks_;
  return true;
}

X_STATUS NativeInputDriver::SetTextKeyLocks(uint32_t user, uint16_t value,
                                            uint16_t mask) {
  auto poll = AcquirePoll();
  if (!poll || user >= XUserMaxUserCount || !AuthorizeCurrentCaller()) {
    return kDeviceNotConnected;
  }
  std::lock_guard lock(mutex_);
  if (stopping_ || !text_endpoint_ || text_endpoint_->failed ||
      !text_endpoint_->present || text_endpoint_->assigned_user != user) {
    return kDeviceNotConnected;
  }
  text_key_locks_ = uint16_t((text_key_locks_ & ~mask) | (value & mask));
  return X_STATUS_SUCCESS;
}

X_STATUS NativeInputDriver::GetLastTextInputTime(uint32_t user,
                                                 uint64_t* time) {
  auto poll = AcquirePoll();
  if (!poll || !time || user >= XUserMaxUserCount ||
      !AuthorizeCurrentCaller()) {
    return kDeviceNotConnected;
  }
  {
    std::lock_guard lock(mutex_);
    if (stopping_ || !text_endpoint_ || text_endpoint_->failed ||
        !text_endpoint_->present || text_endpoint_->assigned_user != user) {
      return kDeviceNotConnected;
    }
  }
  auto* input = config_.kernel->emulator()->input_system();
  auto input_lock = input->lock();
  const auto state = input->GetNativeTextState(transport_generation_);
  if (!state.available || state.user != user) {
    return kDeviceNotConnected;
  }
  *time = state.last_input_time;
  return X_STATUS_SUCCESS;
}
}  // namespace xe::kernel
