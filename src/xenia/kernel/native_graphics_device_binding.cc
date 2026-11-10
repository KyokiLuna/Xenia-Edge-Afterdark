/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia contributors. All rights reserved.                    *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */
#include "xenia/kernel/native_graphics_device_binding.h"

#include <array>
#include <atomic>
#include <cstdlib>
#include <mutex>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

#include "third_party/mbedtls/include/mbedtls/sha256.h"
#include "third_party/tomlplusplus/toml.hpp"
#include "xenia/base/byte_order.h"
#include "xenia/base/logging.h"
#include "xenia/cpu/export_resolver.h"
#include "xenia/cpu/guest_call_abi.h"
#include "xenia/cpu/processor.h"
#include "xenia/cpu/thread_state.h"
#include "xenia/cpu/xex_module.h"
#include "xenia/emulator.h"
#include "xenia/kernel/guest_graphics_device.h"
#include "xenia/kernel/guest_runtime_call_executor.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/native_frame_owner.h"
#include "xenia/kernel/native_import_domain.h"
#include "xenia/kernel/native_system_allocation_authority.h"
#include "xenia/kernel/native_system_ui_service.h"
#include "xenia/kernel/user_module.h"
#include "xenia/kernel/xthread.h"
#include "xenia/memory.h"

namespace xe::kernel {
namespace {
struct Layout {
  uint32_t size, references, system, light_flags, light_mask, thread;
  uint32_t counter, identifier, normal_table, mode_rva;
};

std::string_view PlanTag(std::string_view metadata) {
  constexpr std::string_view key = "\nverified_plan_sha256=";
  const auto at = metadata.rfind(key);
  return at == std::string_view::npos ? std::string_view{}
                                      : metadata.substr(at + key.size());
}

std::optional<Layout> ReadLayout(std::string_view metadata) {
  const auto end = metadata.rfind("\nverified_plan_sha256=");
  if (end == std::string_view::npos) {
    return {};
  }
  toml::table document;
  try {
    document = toml::parse(metadata.substr(0, end));
  } catch (const toml::parse_error&) {
    return {};
  }
  const auto* table = document["system_device_layout"].as_table();
  if (!table || !(*table)["version"].is_integer() ||
      (*table)["version"].value<int64_t>() != 1) {
    return {};
  }
  Layout result{};
  const std::pair<const char*, uint32_t*> fields[] = {
      {"device_size", &result.size},
      {"reference_count", &result.references},
      {"system_flag", &result.system},
      {"light_flags", &result.light_flags},
      {"light_mask", &result.light_mask},
      {"creator_thread", &result.thread},
      {"use_counter", &result.counter},
      {"gpu_identifier", &result.identifier},
      {"normal_table", &result.normal_table},
      {"mode_rva", &result.mode_rva}};
  for (const auto& [name, output] : fields) {
    const auto node = (*table)[name];
    const auto value = node.value<int64_t>();
    if (!node.is_integer() || !value || *value < 0 ||
        uint64_t(*value) > UINT32_MAX) {
      return {};
    }
    *output = uint32_t(*value);
  }
  if (result.size < 4 || result.size > 1024 * 1024 || !result.light_mask ||
      result.light_mask > 255 || result.light_flags >= result.size ||
      (result.mode_rva & 3)) {
    return {};
  }
  for (auto offset : {result.references, result.system, result.thread,
                      result.counter, result.identifier, result.normal_table}) {
    if ((offset & 3) || offset > result.size || result.size - offset < 4) {
      return {};
    }
  }
  return result;
}

bool Writable(Memory* memory, uint32_t address, uint32_t size) {
  const auto access = memory->QueryRangeAccess(address, size);
  return access == xe::memory::PageAccess::kReadWrite ||
         access == xe::memory::PageAccess::kExecuteReadWrite;
}

bool CodeHash(KernelState* kernel, const object_ref<UserModule>& module,
              uint32_t address, uint32_t size, std::string_view expected) {
  auto* memory = kernel->memory();
  if (!size || address > UINT32_MAX - size || expected.size() != 64 ||
      memory->QueryRangeAccess(address, size) ==
          xe::memory::PageAccess::kNoAccess) {
    return false;
  }
  for (uint32_t at = address; at < address + size; at += 4) {
    xex2_section_type section;
    if (kernel->GetUserModuleForAddress(at).get() != module.get() ||
        !module->xex_module()->GetPageSectionType(at, &section) ||
        section != XEX_SECTION_CODE ||
        module->xex_module()->TranslateCode(at) !=
            memory->TranslateVirtual<const uint8_t*>(at)) {
      return false;
    }
  }
  std::array<uint8_t, 32> hash;
  mbedtls_sha256(memory->TranslateVirtual<const uint8_t*>(address), size,
                 hash.data(), 0);
  constexpr char hex[] = "0123456789abcdef";
  for (size_t i = 0; i < hash.size(); ++i) {
    if (expected[2 * i] != hex[hash[i] >> 4] ||
        expected[2 * i + 1] != hex[hash[i] & 15]) {
      return false;
    }
  }
  return true;
}
[[noreturn]] void SharedPublicationFailStop() noexcept {
  try {
    XELOGE(
        "Native shared device publication retirement unproven; retaining "
        "process");
    xe::FlushLog();
  } catch (...) {
  }
  std::quick_exit(EXIT_FAILURE);
}
uint64_t NextEpoch() {
  static std::atomic<uint64_t> next{1};
  auto value = next.load(std::memory_order_relaxed);
  while (value && value != UINT64_MAX) {
    if (next.compare_exchange_weak(value, value + 1,
                                   std::memory_order_relaxed)) {
      return value;
    }
  }
  return 0;
}

struct NativeGraph {
  // Adopts the genuine factory reference; the retained UI renderer/cache graph
  // is reclaimed with guest memory. No PPC Release occurs in a C++ destructor.
  uint32_t device;
  std::mutex mutex;
  std::vector<std::shared_ptr<void>> owners;
};
}  // namespace

struct NativeGraphicsDeviceBinding::State {
  mutable std::mutex lifetime;
  Memory* memory;
  Layout layout;
  uint32_t device, mode, cell;
  std::shared_ptr<NativeGraph> graph;
  std::shared_ptr<GuestGraphicsDevice> owner;
  GuestGraphicsDevice::UseLease use;
  bool retired = false;
  KernelState* kernel = nullptr;
  object_ref<UserModule> module;
  std::shared_ptr<KernelState::UserModuleExecutionLease> frame_module_lease;
  XThread* producer_thread = nullptr;
  GuestRuntime::Identity runtime_identity{};
  uint64_t epoch = 0;
  gpu::GpuBackendStopProof stop_proof;
  bool native_execution = false;
  // Native shared mode owns only the kernel-global publication and an added
  // real reference. Native factory/renderer ownership remains native. The
  // old manually-created exact-thread diagnostic path is unchanged.
  bool native_shared = false, native_shared_bound = false;
  bool native_addref_attempted = false, native_addref_owned = false;
  uint32_t native_addref_result = 0, native_cell_before = 0,
           native_cell_after = 0;
  NativeSystemSharedGraphicsState native_shared_graph;
  std::shared_ptr<NativeSystemAllocationAuthority> allocation;
  uint32_t shared_device_cell = 0, recording_context = 0;
  uint32_t recording_context_cell = 0, dc = 0, dc_cell = 0;

  std::optional<uint32_t> Read(uint32_t address) const {
    if (address != cell || (address & 3) || !Writable(memory, address, 4)) {
      return {};
    }
    auto* word = memory->TranslateVirtual<uint32_t*>(address);
    return xe::byte_swap(
        std::atomic_ref<uint32_t>(*word).load(std::memory_order_acquire));
  }
  bool Cas(uint32_t address, uint32_t expected, uint32_t desired) const {
    if (address != cell || (address & 3) || !Writable(memory, address, 4)) {
      return false;
    }
    auto* word = memory->TranslateVirtual<uint32_t*>(address);
    auto observed = xe::byte_swap(expected);
    return std::atomic_ref<uint32_t>(*word).compare_exchange_strong(
        observed, xe::byte_swap(desired), std::memory_order_acq_rel);
  }
  uint32_t Word(uint32_t address) const {
    return xe::load_and_swap<uint32_t>(memory->TranslateVirtual(address));
  }
};

NativeGraphicsDeviceBinding::NativeGraphicsDeviceBinding(
    std::unique_ptr<State> state)
    : state_(std::move(state)) {}

uint64_t NativeGraphicsDeviceBinding::ReserveEpoch() { return NextEpoch(); }

// Exact native source tickets are checked separately from preceding title
// fences.
class NativeGraphicsDeviceBinding::FrameAuthority final
    : public NativeFrameGraphAuthority {
 public:
  FrameAuthority(std::shared_ptr<NativeGraphicsDeviceBinding> binding,
                 memory::PhysicalAllocationOwner owner,
                 uint64_t foreground_generation, uint32_t title_hmodule = 0,
                 uint64_t title_load_generation = 0)
      : binding_(std::move(binding)),
        owner_(owner),
        foreground_generation_(foreground_generation),
        title_hmodule_(title_hmodule),
        title_load_generation_(title_load_generation) {}

  bool Matches(const memory::PhysicalAllocationOwner& owner) const override {
    if (owner != owner_) {
      return false;
    }
    const auto& data = *binding_->state_;
    std::lock_guard lock(data.lifetime);
    return IdentityMatches(data) && data.frame_module_lease &&
           data.frame_module_lease->hmodule() == owner_.module_hmodule &&
           binding_->CheckCpuResourceContextLocked();
  }
  bool MatchesCompletedRecord(const NativeFrameSource& source, uint32_t device,
                              uint32_t dc) const override {
    const auto& data = *binding_->state_;
    std::lock_guard lock(data.lifetime);
    return data.native_shared && IdentityMatches(data) &&
           source.owner() == owner_ && source.serial() &&
           device == data.device && dc == data.dc &&
           binding_->CheckCpuResourceContextLocked();
  }
  uint64_t backend_epoch() const override {
    const auto& data = *binding_->state_;
    std::lock_guard lock(data.lifetime);
    if (!IdentityMatches(data) || !data.frame_module_lease ||
        data.stop_proof.stopped() ||
        !binding_->CheckCpuResourceContextLocked()) {
      return 0;
    }
    return data.native_shared ? data.stop_proof.backend_epoch()
                              : data.owner->snapshot().backend_epoch;
  }
  bool MatchesTitleInput(
      const gpu::NativeSystemTitleInput& input) const override {
    const auto& id = input.identity();
    if (!input.valid() || !foreground_generation_ || !title_hmodule_ ||
        !title_load_generation_ ||
        id.foreground_generation != foreground_generation_ ||
        id.title_hmodule != title_hmodule_ ||
        id.title_load_generation != title_load_generation_) {
      return false;
    }
    // Temporary module metadata ownership drops after the binding mutex. No
    // persistent Title/Kernel holder, guest read or PPC call is introduced.
    object_ref<UserModule> title;
    const auto& data = *binding_->state_;
    std::lock_guard lock(data.lifetime);
    if (!data.native_execution || data.retired || !IdentityMatches(data) ||
        !data.kernel || !data.frame_module_lease || data.stop_proof.stopped() ||
        id.backend_epoch != data.stop_proof.backend_epoch() ||
        !binding_->CheckCpuResourceContextLocked()) {
      return false;
    }
    const auto* emulator = data.kernel->emulator();
    if (!emulator || !emulator->OwnsTitleExecution(data.kernel)) {
      return false;
    }
    const auto foreground = emulator->title_execution_state().snapshot();
    title = data.kernel->GetExecutableModule();
    // SetExecutableModule itself does not enforce a foreground-generation
    // transition. Require the actual current executable as well as the
    // captured code identity at final Source/one-use execution admission.
    return foreground.generation == foreground_generation_ &&
           foreground.phase == TitleExecutionState::Phase::kRunning && title &&
           title->hmodule_ptr() == title_hmodule_ &&
           title->load_generation() == title_load_generation_ &&
           data.frame_module_lease->hmodule() == owner_.module_hmodule;
  }
  bool CanExecuteNativeFrame(const NativeFrameSource& source) const override {
    return binding_->state_->native_execution && source.owner() == owner_ &&
           source.serial() && Matches(owner_) && backend_epoch();
  }
  bool IsPresentationOwnerLive(
      const memory::PhysicalAllocationOwner& owner) const override {
    if (owner != owner_ || !foreground_generation_) {
      return false;
    }
    const auto& data = *binding_->state_;
    std::lock_guard lock(data.lifetime);
    if (!data.native_execution || data.retired || !IdentityMatches(data) ||
        !data.kernel || !data.frame_module_lease || data.stop_proof.stopped()) {
      return false;
    }
    // Binding retirement holds this same mutex before dropping its Kernel and
    // module references. Check foreground execution before reading guest state.
    const auto* emulator = data.kernel->emulator();
    if (!emulator || !emulator->OwnsTitleExecution(data.kernel)) {
      return false;
    }
    const auto foreground = emulator->title_execution_state().snapshot();
    return foreground.generation == foreground_generation_ &&
           foreground.phase == TitleExecutionState::Phase::kRunning &&
           data.frame_module_lease->hmodule() == owner_.module_hmodule &&
           binding_->CheckCpuResourceContextLocked();
  }
  bool ValidateNativeSubmission(
      const NativeFrameSource& source,
      const gpu::GpuCompletion& completion) const override {
    const auto& data = *binding_->state_;
    std::lock_guard lock(data.lifetime);
    const auto identity = completion.native_frame_identity();
    const auto status = completion.status();
    return data.native_execution && IdentityMatches(data) && !data.retired &&
           data.frame_module_lease &&
           binding_->CheckCpuResourceContextLocked() &&
           source.owner() == owner_ && source.serial() && identity &&
           *identity ==
               gpu::NativeFrameSubmissionIdentity{owner_, source.serial()} &&
           completion.submission() &&
           completion.backend_epoch() == data.stop_proof.backend_epoch() &&
           completion.backend_stop_proof().backend_epoch() ==
               data.stop_proof.backend_epoch() &&
           !data.stop_proof.stopped() &&
           (status == gpu::GpuCompletionStatus::kPending ||
            status == gpu::GpuCompletionStatus::kComplete);
  }
  bool RetireComplete(const NativeFrameSource& source,
                      const gpu::GpuCompletion& completion) override {
    return completion.status() == gpu::GpuCompletionStatus::kComplete &&
           ValidateNativeSubmission(source, completion);
  }
  bool AbortUnsubmitted(const NativeFrameSource& source) override {
    return source.serial() && Matches(source.owner());
  }
  bool RetireAfterKernelStop(
      const memory::PhysicalAllocationOwner& owner) override {
    if (owner != owner_) {
      return false;
    }
    // Kernel has joined producers. Native execution additionally requires the
    // actual same-backend stop boundary, independently of frame success.
    if (binding_->state_->native_execution && !binding_->HasStoppedBackend()) {
      return false;
    }
    binding_->RetireAtKernelStop();
    const auto& data = *binding_->state_;
    std::lock_guard lock(data.lifetime);
    return IdentityMatches(data) && data.retired;
  }

 private:
  bool IdentityMatches(const State& data) const {
    return owner_.valid() && data.epoch == owner_.kernel_epoch &&
           data.runtime_identity.hmodule == owner_.module_hmodule &&
           data.runtime_identity.generation == owner_.module_generation;
  }
  const std::shared_ptr<NativeGraphicsDeviceBinding> binding_;
  const memory::PhysicalAllocationOwner owner_;
  const uint64_t foreground_generation_;
  const uint32_t title_hmodule_;
  const uint64_t title_load_generation_;
};

std::shared_ptr<NativeFrameGraphAuthority>
NativeGraphicsDeviceBinding::CreateFrameAuthority(
    const memory::PhysicalAllocationOwner& owner) {
  const auto self = weak_from_this().lock();
  auto* current = XThread::IsInThread() ? XThread::GetCurrentThread() : nullptr;
  if (!self || !owner.valid() || !current || !current->is_host_thread() ||
      current->native_frame_thread_identity().process_type !=
          X_PROCTYPE_SYSTEM) {
    return {};
  }
  KernelState* kernel;
  object_ref<UserModule> module;
  {
    std::lock_guard lock(state_->lifetime);
    if (state_->retired || current != state_->producer_thread ||
        current->kernel_state() != state_->kernel ||
        state_->epoch != owner.kernel_epoch ||
        state_->runtime_identity.hmodule != owner.module_hmodule ||
        state_->runtime_identity.generation != owner.module_generation ||
        !CheckCpuResourceContextLocked()) {
      return {};
    }
    kernel = state_->kernel;
    module = state_->module;
  }
  auto lease = kernel->AcquireUserModuleExecutionLease(module);
  if (!lease) {
    return {};
  }
  std::shared_ptr<KernelState::UserModuleExecutionLease> previous_lease;
  {
    std::lock_guard lock(state_->lifetime);
    if (state_->retired || current != state_->producer_thread ||
        !CheckCpuResourceContextLocked()) {
      return {};
    }
    previous_lease = std::move(state_->frame_module_lease);
    state_->frame_module_lease = std::move(lease);
  }
  const auto* emulator = kernel->emulator();
  const auto foreground = emulator->title_execution_state().snapshot();
  const uint64_t foreground_generation =
      emulator->OwnsTitleExecution(kernel) &&
              foreground.phase == TitleExecutionState::Phase::kRunning
          ? foreground.generation
          : 0;
  auto authority =
      std::make_shared<FrameAuthority>(self, owner, foreground_generation);
  return authority->Matches(owner) ? authority : nullptr;
}

std::shared_ptr<NativeFrameGraphAuthority>
NativeGraphicsDeviceBinding::CreateFrameAuthority(
    const std::shared_ptr<NativeSystemFrameDelegation>& delegation) {
  auto* thread = XThread::IsInThread() ? XThread::GetCurrentThread() : nullptr;
  if (!thread || !delegation || !delegation->session() ||
      !delegation->session()->MatchesThread(
          thread->native_frame_thread_identity())) {
    return {};
  }
  auto self = shared_from_this();
  KernelState* kernel = nullptr;
  {
    std::lock_guard lock(state_->lifetime);
    if (!state_->native_shared || state_->retired ||
        state_->allocation != delegation->authority() ||
        state_->epoch != delegation->owner().kernel_epoch ||
        !CheckCpuResourceContextLocked()) {
      return {};
    }
    kernel = state_->kernel;
  }
  const auto foreground =
      kernel->emulator()->title_execution_state().snapshot();
  const auto generation =
      kernel->emulator()->OwnsTitleExecution(kernel) &&
              foreground.phase == TitleExecutionState::Phase::kRunning
          ? foreground.generation
          : 0;
  const auto title = kernel->GetExecutableModule();
  auto authority = std::make_shared<FrameAuthority>(
      self, delegation->owner(), generation, title ? title->hmodule_ptr() : 0,
      title ? title->load_generation() : 0);
  return authority->Matches(delegation->owner()) ? authority : nullptr;
}

std::shared_ptr<NativeGraphicsDeviceBinding>
NativeGraphicsDeviceBinding::PublishForNativeSystem(
    const NativeSystemUiServiceContext& c, XThread& bootstrap,
    const gpu::GpuBackendStopProof& stop, std::string* error) {
  const auto fail = [&](const char* text) {
    if (error) {
      *error = text;
    }
    return std::shared_ptr<NativeGraphicsDeviceBinding>{};
  };
  if (!c.kernel || !c.catalog || !c.allocation || !c.domain ||
      c.device_publication || !XThread::IsInThread(&bootstrap) ||
      bootstrap.kernel_state() != c.kernel ||
      bootstrap.thread_id() != c.allocation->bootstrap_thread_id() ||
      bootstrap.native_frame_thread_identity().process_type !=
          X_PROCTYPE_SYSTEM ||
      !c.allocation->IsAccepting() || !c.allocation->MatchesDomain(c.domain) ||
      !c.domain->active() || !stop.backend_epoch() || stop.stopped()) {
    return fail("Native publication lacks exact live bootstrap/cohort/backend");
  }
  auto operation = c.domain->Acquire();
  auto module = c.allocation->root_module();
  auto runtime =
      module ? c.kernel->GetGuestRuntime(module->hmodule_ptr()) : nullptr;
  if (!operation || !module ||
      module.get() != c.catalog->calls()->module().get() ||
      module->load_generation() != c.allocation->root_load_generation() ||
      !runtime || runtime->GetSnapshot().state != GuestRuntime::State::kReady ||
      runtime->GetSnapshot().identity.generation !=
          c.allocation->owner().module_generation) {
    return fail("Native publication root/runtime generation mismatch");
  }
  auto code = c.kernel->AcquireUserModuleExecutionLease(module);
  const auto graph = c.catalog->ReadSharedGraphicsState();
  const auto layout = ReadLayout(graph.factory.abi_metadata);
  if (!code || !layout ||
      (graph.status != NativeSystemSharedGraphicsState::Status::kDcPending &&
       graph.status != NativeSystemSharedGraphicsState::Status::kObserved) ||
      !graph.command_pin || !graph.command_pin->valid() ||
      graph.factory.address != 0x8183EA40 ||
      !CodeHash(
          c.kernel, module, 0x8183EA40, 0xD8,
          "86c64388db95f9bf5b198ad9fc67f7e568e608378bfe7929307e91d14ad929f9") ||
      !CodeHash(
          c.kernel, module, 0x8183E610, 40,
          "124bac340c841c468c3fec726080842099dc5d2b70446a2ede3aff8f7b6427a3")) {
    return fail("Actual native light-device graph/code profile unavailable");
  }
  for (const auto& allocation : graph.command_pin->allocations()) {
    if (allocation.owner != c.allocation->owner()) {
      return fail("Native command backing has a foreign owner");
    }
  }
  auto* memory = c.kernel->memory();
  const uint64_t mode =
      uint64_t(module->xex_module()->base_address()) + layout->mode_rva;
  const auto* cell =
      c.kernel->processor()->export_resolver()->GetExportByOrdinal(
          "xboxkrnl.exe", 0x1BF);
  if (mode > UINT32_MAX || !Writable(memory, uint32_t(mode), 4) || !cell ||
      cell->get_type() != cpu::Export::Type::kVariable ||
      std::string_view(cell->name) != "VdGlobalXamDevice" ||
      !cell->variable_ptr || (cell->variable_ptr & 3) ||
      !Writable(memory, cell->variable_ptr, 4) ||
      c.kernel->GetUserModuleForAddress(0x815F09AC).get() != module.get() ||
      memory->QueryRangeAccess(0x815F09AC, 4) ==
          xe::memory::PageAccess::kNoAccess ||
      xe::load_and_swap<uint32_t>(memory->TranslateVirtual(0x815F09AC)) !=
          cell->variable_ptr) {
    const uint32_t actual_cell =
        cell && cell->get_type() == cpu::Export::Type::kVariable
            ? cell->variable_ptr
            : 0;
    const bool readable_cell =
        actual_cell && memory->QueryRangeAccess(actual_cell, 4) !=
                           xe::memory::PageAccess::kNoAccess;
    const bool readable_import = memory->QueryRangeAccess(0x815F09AC, 4) !=
                                 xe::memory::PageAccess::kNoAccess;
    XELOGE(
        "Native shared publication cell contract failed: variable={} "
        "cell={:08X} readable={} value={:08X} import_readable={} "
        "import_target={:08X}",
        cell && cell->get_type() == cpu::Export::Type::kVariable, actual_cell,
        readable_cell,
        readable_cell
            ? xe::load_and_swap<uint32_t>(memory->TranslateVirtual(actual_cell))
            : UINT32_MAX,
        readable_import,
        readable_import
            ? xe::load_and_swap<uint32_t>(memory->TranslateVirtual(0x815F09AC))
            : UINT32_MAX);
    return fail("Actual kernel native device publication cell unavailable");
  }
  auto data = std::make_unique<State>();
  data->kernel = c.kernel;
  data->module = module;
  data->memory = memory;
  data->runtime_identity = {c.allocation->owner().module_hmodule,
                            c.allocation->owner().module_generation};
  data->epoch = c.allocation->owner().kernel_epoch;
  data->native_execution = true;
  data->native_shared = true;
  data->allocation = c.allocation;
  data->stop_proof = stop;
  data->frame_module_lease = code;
  data->layout = *layout;
  data->device = graph.device;
  data->mode = uint32_t(mode);
  data->cell = cell->variable_ptr;
  data->shared_device_cell = graph.symbols.shared_device_cell;
  data->recording_context = graph.recording_context;
  data->recording_context_cell = graph.symbols.record_context_cell;
  data->native_shared_graph = graph;
  data->graph = std::make_shared<NativeGraph>();
  data->graph->device = graph.device;
  data->owner = GuestGraphicsDevice::Create(
      {data->epoch, module->hmodule_ptr(), data->runtime_identity.generation,
       graph.device, X_PROCTYPE_SYSTEM},
      {code, data->graph}, stop.backend_epoch());
  if (!data->owner) {
    return fail("Native publication identity/model unavailable");
  }
  auto binding = std::shared_ptr<NativeGraphicsDeviceBinding>(
      new NativeGraphicsDeviceBinding(std::move(data)));
  struct KernelRetirement {
    std::shared_ptr<NativeGraphicsDeviceBinding> owner;
    bool armed = false;
    ~KernelRetirement() {
      if (!armed) {
        return;
      }
      if (!owner->HasStoppedBackend()) {
        SharedPublicationFailStop();
      }
      owner->RetireAtKernelStop();
    }
  };
  auto guard = std::make_shared<KernelRetirement>();
  guard->owner = binding;
  c.kernel->RetainNativeResourceUntilTeardown(guard);
  guard->armed =
      true;  // BEFORE native AddRef or the first possible global store.
  auto& state = *binding->state_;
  const bool profile_backing =
      Writable(memory, state.device, state.layout.size) &&
      Writable(memory, state.mode, 4);
  const auto profile_word = [&](uint32_t offset) {
    return profile_backing ? state.Word(state.device + offset) : UINT32_MAX;
  };
  XELOGI(
      "Native shared device profile: backing={} device={:08X} mode={:08X} "
      "refs={} system={} creator={} observer_thread={} counter={} "
      "identifier={} normal_table={:08X} light_flags={:02X}",
      profile_backing, state.device,
      profile_backing ? state.Word(state.mode) : UINT32_MAX,
      profile_word(state.layout.references), profile_word(state.layout.system),
      profile_word(state.layout.thread), bootstrap.thread_id(),
      profile_word(state.layout.counter), profile_word(state.layout.identifier),
      profile_word(state.layout.normal_table),
      profile_backing ? unsigned(memory->TranslateVirtual<const uint8_t*>(
                            state.device)[state.layout.light_flags])
                      : UINT32_MAX);
  if (!binding->CheckDeviceProfileLocked() ||
      !Writable(memory, state.shared_device_cell, 4) ||
      !Writable(memory, state.recording_context_cell, 4) ||
      state.Word(state.shared_device_cell) != state.device ||
      state.Word(state.recording_context_cell) != state.recording_context) {
    return fail("Actual native shared device is outside the CPU light profile");
  }
  state.native_cell_before = state.Read(state.cell).value_or(UINT32_MAX);
  XELOGI(
      "Native shared device publication: root={:08X} load_generation={} "
      "runtime_generation={} device={:08X} native_cell={:08X} "
      "kernel_cell={:08X} before={:08X} native_import=815F09AC "
      "import_target={:08X} backend_epoch={}",
      module->hmodule_ptr(), module->load_generation(),
      state.runtime_identity.generation, state.device, state.shared_device_cell,
      state.cell, state.native_cell_before,
      xe::load_and_swap<uint32_t>(memory->TranslateVirtual(0x815F09AC)),
      stop.backend_epoch());
  // A foreign OR identical unowned pointer is never adopted. There is exactly
  // one publication attempt and no native AddRef on a known occupied cell.
  if (state.native_cell_before != 0) {
    return fail("Kernel native device cell is already owned");
  }
  state.native_addref_attempted = true;
  const std::array args{cpu::GuestCallArgument::UInt32(state.device)};
  const auto added = cpu::ExecuteGuestCall(
      c.kernel->processor(), bootstrap.thread_state(), 0x8183E610, args);
  state.native_addref_result = uint32_t(added.value);
  state.native_addref_owned =
      added.executed && added.value >= 2 && added.value <= UINT32_MAX;
  if (!state.native_addref_owned || !c.allocation->IsAccepting() ||
      stop.stopped()) {
    return fail("Actual native device AddRef/publication admission failed");
  }
  const auto publication = state.owner->Publish(
      state.cell, [&](uint32_t at) { return state.Read(at); },
      [&](uint32_t at, uint32_t before, uint32_t after) {
        return state.Cas(at, before, after);
      });
  state.native_cell_after = state.Read(state.cell).value_or(UINT32_MAX);
  if (!publication) {
    return fail("Native device global publication rejected or uncertain");
  }
  state.use = state.owner->AcquireUse(*publication);
  auto published_context = c;
  published_context.device_publication = binding;
  if (!binding->MatchesNativeSystemPublication(published_context, graph)) {
    return fail("Published native light device lost its exact cohort/profile");
  }
  XELOGI(
      "Native shared device publication established: device={:08X} cell={:08X} "
      "serial={} addref_result={} after={:08X}; DC/Source readiness not "
      "granted",
      state.device, state.cell, publication->serial, state.native_addref_result,
      state.native_cell_after);
  return binding;
}

bool NativeGraphicsDeviceBinding::MatchesNativeSystemPublication(
    const NativeSystemUiServiceContext& c,
    const NativeSystemSharedGraphicsState& graph) const {
  std::lock_guard lock(state_->lifetime);
  const auto& data = *state_;
  return c.device_publication.get() == this && data.native_shared &&
         data.kernel == c.kernel && data.allocation == c.allocation &&
         c.domain && c.domain->active() && c.allocation &&
         c.allocation->MatchesDomain(c.domain) && data.device == graph.device &&
         data.recording_context == graph.recording_context &&
         data.shared_device_cell == graph.symbols.shared_device_cell &&
         data.recording_context_cell == graph.symbols.record_context_cell &&
         data.native_shared_graph.command_buffers == graph.command_buffers &&
         data.native_shared_graph.factory == graph.factory &&
         CheckNativePublicationLocked();
}
NativeGraphicsDeviceBinding::NativeSystemPublicationMetadata
NativeGraphicsDeviceBinding::native_system_publication_metadata() const {
  std::lock_guard lock(state_->lifetime);
  const auto& data = *state_;
  const auto model =
      data.owner ? data.owner->snapshot() : GuestGraphicsDevice::Snapshot{};
  return {
      data.native_addref_attempted,
      data.native_addref_owned,
      model.phase == GuestGraphicsDevice::Phase::kPublished && bool(data.use),
      data.native_shared_bound,
      data.retired,
      data.cell,
      data.native_cell_before,
      data.native_cell_after,
      data.device,
      data.native_addref_result,
      model.publication.serial};
}

std::shared_ptr<NativeGraphicsDeviceBinding>
NativeGraphicsDeviceBinding::CreateForNativeSystem(
    const NativeSystemGraphicsDeviceRecord& record, std::string* error) {
  const auto fail = [&](const char* text) {
    if (error) {
      *error = text;
    }
    return std::shared_ptr<NativeGraphicsDeviceBinding>{};
  };
  auto binding = record.device_publication;
  auto allocation = record.allocation;
  auto* current = XThread::IsInThread() ? XThread::GetCurrentThread() : nullptr;
  if (!binding || !allocation || !current ||
      current->kernel_state() != allocation->kernel() ||
      current->native_frame_thread_identity().process_type !=
          X_PROCTYPE_SYSTEM ||
      record.root_load_generation != allocation->root_load_generation() ||
      !record.stop_proof.backend_epoch() || record.stop_proof.stopped()) {
    return fail(
        "Actual native shared binding lacks its early owned publication");
  }
  std::lock_guard lock(binding->state_->lifetime);
  auto& data = *binding->state_;
  if (!data.native_shared || data.native_shared_bound ||
      data.allocation != allocation ||
      data.stop_proof.backend_epoch() != record.stop_proof.backend_epoch() ||
      data.native_shared_graph.factory != record.factory ||
      data.device != record.device ||
      data.shared_device_cell != record.shared_device_cell ||
      data.recording_context != record.recording_context ||
      data.recording_context_cell != record.recording_context_cell ||
      !record.dc || record.dc > UINT32_MAX - 4 ||
      record.dc_cell != data.native_shared_graph.symbols.dc_cell ||
      !record.dc_cell || !Writable(data.memory, record.dc, 8) ||
      !Writable(data.memory, record.dc_cell, 4) ||
      data.Word(record.dc_cell) != record.dc ||
      !binding->CheckNativePublicationLocked()) {
    return fail(
        "Shared DC observation does not join the exact live publication");
  }
  data.dc = record.dc;
  data.dc_cell = record.dc_cell;
  data.native_shared_bound = true;
  if (!binding->CheckCpuResourceContextLocked()) {
    data.native_shared_bound = false;
    return fail("Shared graph is outside verified CPU-recording profile");
  }
  return binding;
}

std::shared_ptr<NativeGraphicsDeviceBinding>
NativeGraphicsDeviceBinding::Create(KernelState* kernel,
                                    const object_ref<UserModule>& module,
                                    const GuestRuntime::Function& factory,
                                    uint32_t device, uint64_t backend_epoch,
                                    std::string* error, uint64_t reserved_epoch,
                                    gpu::GpuBackendStopProof stop_proof,
                                    bool native_execution) {
  const auto fail = [&](const char* message) {
    if (error) {
      *error = message;
    }
    return std::shared_ptr<NativeGraphicsDeviceBinding>{};
  };
  if (native_execution &&
      (!backend_epoch || stop_proof.backend_epoch() != backend_epoch ||
       stop_proof.stopped())) {
    return fail("Native execution requires a live exact-backend stop proof");
  }
  auto* thread = XThread::GetCurrentThread();
  if (!kernel || !module || !thread || thread->kernel_state() != kernel ||
      !thread->is_host_thread() || module->kernel_state() != kernel ||
      !module->is_system_app_module() ||
      module->module_format() != UserModule::kModuleFormatXex || !device ||
      (device & 3) ||
      thread->guest_object<X_KTHREAD>()->process_type != X_PROCTYPE_SYSTEM) {
    return fail("SYSTEM host thread and real resident device required");
  }
  const auto runtime = kernel->GetGuestRuntime(module->hmodule_ptr());
  if (!runtime) {
    return fail("Resident native runtime absent");
  }
  const auto snapshot = runtime->GetSnapshot();
  const auto layout = ReadLayout(factory.abi_metadata);
  const auto tag = PlanTag(factory.abi_metadata);
  if (snapshot.state != GuestRuntime::State::kReady || !snapshot.plan ||
      snapshot.plan->phases.empty() ||
      snapshot.plan->phases.front().functions.empty() || !layout ||
      tag.size() != 64 ||
      tag !=
          PlanTag(
              snapshot.plan->phases.front().functions.front().abi_metadata) ||
      kernel->GetUserModuleForAddress(factory.address).get() != module.get()) {
    return fail("Device layout or runtime generation not verified");
  }
  auto* memory = kernel->memory();
  auto* image = module->xex_module();
  const uint64_t mode = uint64_t(image->base_address()) + layout->mode_rva;
  xex2_section_type section;
  if (mode > UINT32_MAX ||
      mode + 4 > uint64_t(image->base_address()) + image->image_size() ||
      !image->GetPageSectionType(uint32_t(mode), &section) ||
      section != XEX_SECTION_DATA || !Writable(memory, uint32_t(mode), 4) ||
      !Writable(memory, device, layout->size)) {
    return fail("Device graph or mode ownership unavailable");
  }
  const auto word = [&](uint32_t offset) {
    return xe::load_and_swap<uint32_t>(
        memory->TranslateVirtual(device + offset));
  };
  XELOGI(
      "Native SYSTEM device: factory profile device={:08X} refs={} "
      "system={} creator={} expected_thread={} counter={} identifier={} "
      "table={:08X} light_flags={:02X} mode={}",
      device, word(layout->references), word(layout->system),
      word(layout->thread), thread->thread_id(), word(layout->counter),
      word(layout->identifier), word(layout->normal_table),
      memory->TranslateVirtual<const uint8_t*>(device)[layout->light_flags],
      xe::load_and_swap<uint32_t>(memory->TranslateVirtual(uint32_t(mode))));
  if (word(layout->references) != 1 || word(layout->system) != 1 ||
      word(layout->thread) != thread->thread_id() || word(layout->counter) ||
      word(layout->identifier) || word(layout->normal_table) ||
      !(memory->TranslateVirtual<const uint8_t*>(device)[layout->light_flags] &
        layout->light_mask) ||
      xe::load_and_swap<uint32_t>(memory->TranslateVirtual(uint32_t(mode)))) {
    return fail("Factory result is outside fresh CPU recording profile");
  }
  const auto* cell_export =
      kernel->processor()->export_resolver()->GetExportByOrdinal(
          "xboxkrnl.exe",
          0x1BF);  // Public VdGlobalXamDevice ABI, not image data.
  if (!cell_export || cell_export->get_type() != cpu::Export::Type::kVariable ||
      std::string_view(cell_export->name) != "VdGlobalXamDevice" ||
      !cell_export->variable_ptr || (cell_export->variable_ptr & 3) ||
      !Writable(memory, cell_export->variable_ptr, 4)) {
    return fail("Kernel SYSTEM device cell unavailable");
  }
  auto module_lease = kernel->AcquireUserModuleExecutionLease(module);
  if (!module_lease) {
    return fail("Native device module cannot be leased");
  }
  auto graph = std::make_shared<NativeGraph>();
  graph->device = device;
  const uint64_t epoch = reserved_epoch ? reserved_epoch : ReserveEpoch();
  auto owner = GuestGraphicsDevice::Create(
      {epoch, module->hmodule_ptr(), snapshot.identity.generation, device,
       X_PROCTYPE_SYSTEM},
      {module_lease, graph}, backend_epoch);
  if (!owner) {
    return fail("Native device ownership identity invalid");
  }
  auto state = std::make_unique<State>();
  state->kernel = kernel;
  state->module = module;
  state->producer_thread = thread;
  state->runtime_identity = snapshot.identity;
  state->epoch = epoch;
  state->stop_proof = std::move(stop_proof);
  state->native_execution = native_execution;
  state->memory = memory;
  state->layout = *layout;
  state->device = device;
  state->mode = uint32_t(mode);
  state->cell = cell_export->variable_ptr;
  state->graph = std::move(graph);
  state->owner = std::move(owner);
  auto binding = std::shared_ptr<NativeGraphicsDeviceBinding>(
      new NativeGraphicsDeviceBinding(std::move(state)));
  // Register retirement before the first possible public store, including an
  // uncertain CAS outcome. Kernel clears this after guest consumers stop and
  // before runtimes/modules/memory are destroyed.
  struct KernelRetirement {
    explicit KernelRetirement(
        std::shared_ptr<NativeGraphicsDeviceBinding> owner)
        : owner(std::move(owner)) {}
    ~KernelRetirement() { owner->RetireAtKernelStop(); }
    std::shared_ptr<NativeGraphicsDeviceBinding> owner;
  };
  kernel->RetainNativeResourceUntilTeardown(
      std::make_shared<KernelRetirement>(binding));
  auto& data = *binding->state_;
  const auto publication = data.owner->Publish(
      data.cell, [&](auto address) { return data.Read(address); },
      [&](auto address, auto before, auto after) {
        return data.Cas(address, before, after);
      });
  if (!publication) {
    return fail("SYSTEM device cell already owned or uncertain");
  }
  data.use = data.owner->AcquireUse(*publication);
  if (!data.use || !binding->CheckCpuResourceContext()) {
    return fail("Published native device lost its CPU ownership profile");
  }
  XELOGI(
      "Native SYSTEM device: published device={:08X} cell={:08X} "
      "kernel_epoch={} module_generation={} backend_epoch={}; CPU recording "
      "only",
      device, data.cell, epoch, snapshot.identity.generation, backend_epoch);
  return binding;
}

bool NativeGraphicsDeviceBinding::CheckCpuResourceContext() const {
  std::lock_guard lock(state_->lifetime);
  return CheckCpuResourceContextLocked();
}

bool NativeGraphicsDeviceBinding::CheckDeviceProfileLocked() const {
  const auto& data = *state_;
  if (data.retired || !data.memory ||
      !Writable(data.memory, data.device, data.layout.size) ||
      !Writable(data.memory, data.mode, 4)) {
    return false;
  }
  return data.Word(data.mode) == 0 &&
         data.Word(data.device + data.layout.references) > 0 &&
         (!data.native_shared ||
          data.Word(data.device + data.layout.references) != UINT32_MAX) &&
         data.Word(data.device + data.layout.system) == 1 &&
         data.Word(data.device + data.layout.counter) == 0 &&
         data.Word(data.device + data.layout.identifier) == 0 &&
         data.Word(data.device + data.layout.normal_table) == 0 &&
         (data.memory->TranslateVirtual<const uint8_t*>(
              data.device)[data.layout.light_flags] &
          data.layout.light_mask);
}
bool NativeGraphicsDeviceBinding::CheckNativePublicationLocked() const {
  const auto& data = *state_;
  return data.native_shared && data.native_addref_owned && data.use &&
         data.owner && !data.stop_proof.stopped() &&
         CheckDeviceProfileLocked() && data.Read(data.cell) == data.device &&
         data.allocation && data.allocation->IsAccepting() &&
         Writable(data.memory, data.shared_device_cell, 4) &&
         Writable(data.memory, data.recording_context_cell, 4) &&
         data.Word(data.shared_device_cell) == data.device &&
         data.Word(data.recording_context_cell) == data.recording_context;
}
bool NativeGraphicsDeviceBinding::CheckCpuResourceContextLocked() const {
  const auto& data = *state_;
  if (!data.use || !CheckDeviceProfileLocked() ||
      data.Read(data.cell) != data.device) {
    return false;
  }
  if (!data.native_shared) {
    return true;
  }
  return data.native_shared_bound && CheckNativePublicationLocked() &&
         Writable(data.memory, data.dc_cell, 4) &&
         data.Word(data.dc_cell) == data.dc;
}

bool NativeGraphicsDeviceBinding::HoldGraphOwner(std::shared_ptr<void> owner) {
  std::lock_guard lifetime(state_->lifetime);
  if (!owner || !CheckCpuResourceContextLocked()) {
    return false;
  }
  std::lock_guard lock(state_->graph->mutex);
  state_->graph->owners.push_back(std::move(owner));
  return true;
}

NativeGraphicsDeviceBinding::~NativeGraphicsDeviceBinding() {
  RetireAtKernelStop();
}

bool NativeGraphicsDeviceBinding::HasStoppedBackend() const {
  std::lock_guard lock(state_->lifetime);
  return state_->stop_proof.backend_epoch() && state_->stop_proof.stopped();
}

void NativeGraphicsDeviceBinding::RetireAtKernelStop() {
  auto& data = *state_;
  std::vector<std::shared_ptr<void>> retired_owners;
  std::shared_ptr<NativeGraph> retired_graph;
  object_ref<UserModule> retired_module;
  std::shared_ptr<KernelState::UserModuleExecutionLease> retired_frame_lease;
  std::shared_ptr<NativeSystemAllocationAuthority> retired_allocation;
  std::shared_ptr<GuestGraphicsDevice> retired_publication;
  NativeSystemSharedGraphicsState retired_shared_graph;
  {
    std::lock_guard lifetime(data.lifetime);
    if (data.retired) {
      return;
    }
    if (data.native_execution && !data.stop_proof.stopped()) {
      XELOGE("Native SYSTEM device: GPU stop unproven; retaining graph");
      return;
    }
    if (data.native_shared) {
      data.use = {};
      const auto before = data.owner->snapshot();
      if (before.publication.cell_address) {
        const auto result = data.owner->RetireAfterGpuStopped(
            before.publication, [&](uint32_t at) { return data.Read(at); },
            [&](uint32_t at, uint32_t expected, uint32_t desired) {
              return data.Cas(at, expected, desired);
            },
            [&](uint64_t kernel_epoch, uint64_t backend_epoch) {
              return kernel_epoch == data.epoch &&
                     backend_epoch == data.stop_proof.backend_epoch() &&
                     data.stop_proof.stopped();
            });
        if (result != GuestGraphicsDevice::RetireStatus::kRetired &&
            result != GuestGraphicsDevice::RetireStatus::kRetiredForeign) {
          SharedPublicationFailStop();
        }
      }
      data.retired = true;
      data.native_shared_bound = false;
      retired_publication = std::move(data.owner);
      retired_shared_graph = std::move(data.native_shared_graph);
      retired_graph = std::move(data.graph);
      retired_module = std::move(data.module);
      retired_frame_lease = std::move(data.frame_module_lease);
      retired_allocation = std::move(data.allocation);
      data.kernel = nullptr;
      data.producer_thread = nullptr;
      data.memory = nullptr;
      // Only our exact global publication was compare-cleared, after actual
      // CPU/GPU stop. Native owns its factory/reference graph; no PPC Release.
      // Prepared/failed-before-CAS owners also drop their leases outside locks.
    } else {
      data.retired = true;
      data.use = {};
      const auto before = data.owner->snapshot();
      const auto publication = before.publication;
      auto retired = GuestGraphicsDevice::RetireStatus::kRetired;
      if (publication.cell_address) {
        data.owner->BeginRetire(publication);
        retired = data.owner->RetireAfterGpuStopped(
            publication, [&](auto address) { return data.Read(address); },
            [&](auto address, auto expected, auto desired) {
              return data.Cas(address, expected, desired);
            },
            [&](uint64_t kernel_epoch, uint64_t backend_epoch) {
              // The model owns CPU publication. GPU execution uses the separate
              // source lease and requires actual device retirement here.
              return kernel_epoch == publication.identity.kernel_epoch &&
                     backend_epoch == before.backend_epoch &&
                     before.gpu_uses == 0 &&
                     (!data.native_execution ||
                      (data.stop_proof.backend_epoch() == backend_epoch &&
                       data.stop_proof.stopped()));
            });
      }
      if (retired == GuestGraphicsDevice::RetireStatus::kRetired ||
          retired == GuestGraphicsDevice::RetireStatus::kRetiredForeign) {
        std::lock_guard graph(data.graph->mutex);
        retired_owners.swap(data.graph->owners);
      }
      // Quarantined models keep their self-pinned graph; external handles
      // retain no additional scene/module references and cannot access old
      // Memory.
      retired_graph = std::move(data.graph);
      retired_module = std::move(data.module);
      retired_frame_lease = std::move(data.frame_module_lease);
      data.kernel = nullptr;
      data.producer_thread = nullptr;
      data.memory = nullptr;
      XELOGI(
          "Native SYSTEM device: retirement status={} device={:08X} "
          "cell={:08X}; no PPC teardown from host destructor",
          unsigned(retired), data.device, data.cell);
    }  // diagnostic publication ownership
  }
  // Drop actual scene/module/execution leases while Kernel/Memory still live,
  // and outside the lifetime/model/graph mutexes.
}
}  // namespace xe::kernel
