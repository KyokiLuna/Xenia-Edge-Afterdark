/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia contributors. All rights reserved.                    *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */
#include "xenia/kernel/native_managed_format_io.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <utility>
#include <vector>
#include "third_party/mbedtls/include/mbedtls/sha256.h"
#include "xenia/base/byte_order.h"
#include "xenia/base/mutex.h"
#include "xenia/base/threading.h"
#include "xenia/cpu/thread_state.h"
#include "xenia/kernel/guest_runtime_call_executor.h"
#include "xenia/kernel/guest_scheduler.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/native_import_domain.h"
#include "xenia/kernel/native_managed_format_frontdoor.h"
#include "xenia/kernel/native_system_ui_public_calls.h"
#include "xenia/kernel/user_module.h"
#include "xenia/kernel/util/native_wait_snapshot.h"
#include "xenia/kernel/xam/content_manager.h"
#include "xenia/kernel/xthread.h"
namespace xe::kernel {
namespace {
constexpr uint32_t kStart = 0x816A2CF0, kCopy = 0x816A2A48,
                   kCancel = 0x816A26F8, kComplete = 0x816A2588,
                   kFormat = 0x81684578;
constexpr uint32_t kInvalid = 0x80070057, kCanceled = 0x800704C7,
                   kFailed = 0x80004005, kBusy = 0x800700AA;
std::mutex installed_mutex;
// Host-worker self-drain guard only, never guest/fiber invocation identity.
thread_local const void* host_io_owner = nullptr;
struct HostOwnerScope {
  const void* previous;
  explicit HostOwnerScope(const void* owner) : previous(host_io_owner) {
    host_io_owner = owner;
  }
  ~HostOwnerScope() { host_io_owner = previous; }
};
// A failed Drain deliberately retains this strong owner. Removing a live
// managed bridge and letting its callback fall through to raw format is unsafe.
std::map<KernelState*, std::shared_ptr<NativeManagedFormatIo>> installed;
struct CodeProfile {
  uint32_t at, bytes;
  const char* sha;
};
constexpr CodeProfile code_profiles[] = {
    {0x8172D390, 164,
     "15f88a986a14963be0b342b2d3477b8091b165de9061444c3879aab1e52e7b5c"},
    {kStart, 656,
     "e00dfeaa77f3f52c68a5f89f5bea3f6a259c38f2fb3e15ae02f922803f75339a"},
    {kCopy, 128,
     "7ebaa0f5422b037352ac5cfb6a5bd2a3954d4e6fdc3901d5e00a9f0d3c4f053d"},
    {kCancel, 352,
     "b1af64f353766148e34db02ed938743371561d2ff3d694bb5a8209e5c205051a"},
    {kComplete, 364,
     "cb8e83d7817263c223f0ac9719175540bbd78746a39ce1b5a38221550bf953d1"},
    {kFormat, 268,
     "72b85802c32d4c9ddfb4d535ddde60aee77fc4b13b9b0419dbd1c20dbafd22d8"},
    {0x816A2C80, 108,
     "21da95341366e83781ab2e46696f0c04afa43d71ae7c11f91dc8e108d1b1db94"},
    {0x816B8B08, 152,
     "1e6e216190b9d6046af3325bf16deb0e22ff9409b457f920a67bfbb1b756c2de"},
};
GuestRuntime::ExecutionResult Failure(const char* text) {
  return {false, kFailed, text};
}
std::optional<uint32_t> Pointer(uint64_t value) {
  const auto low = uint32_t(value);
  if (value != low && value != uint64_t(int64_t(int32_t(low)))) {
    return {};
  }
  return low;
}
std::optional<uint32_t> Word(Memory& memory, uint32_t at) {
  std::array<uint8_t, 4> raw;
  return internal::ReadNativeWaitSnapshot(memory, at, raw)
             ? std::optional(xe::load_and_swap<uint32_t>(raw.data()))
             : std::nullopt;
}
bool CheckCode(KernelState& kernel, UserModule& module) {
  auto guard = global_critical_region::AcquireDirect();
  auto* memory = kernel.memory();
  for (const auto& p : code_profiles) {
    if (memory->QueryRangeAccess(p.at, p.bytes) ==
        memory::PageAccess::kNoAccess) {
      return false;
    }
    for (uint32_t at = p.at; at < p.at + p.bytes; at += 4) {
      xex2_section_type type;
      if (kernel.GetUserModuleForAddress(at).get() != &module ||
          !module.xex_module()->GetPageSectionType(at, &type) ||
          type != XEX_SECTION_CODE ||
          (p.at != kFormat &&
           module.xex_module()->TranslateCode(at) !=
               memory->TranslateVirtual<const uint8_t*>(at))) {
        return false;
      }
    }
    std::array<uint8_t, 32> digest;
    mbedtls_sha256(memory->TranslateVirtual<const uint8_t*>(p.at), p.bytes,
                   digest.data(), 0);
    constexpr char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < digest.size(); ++i) {
      if (p.sha[2 * i] != hex[digest[i] >> 4] ||
          p.sha[2 * i + 1] != hex[digest[i] & 15]) {
        return false;
      }
    }
  }
  // Real native epilogs restore their entry SP/LR through these canonical
  // helpers. A symbol may legitimately be inlined without any compiled helper.
  // Inspect owned metadata only; never QueryFunction/Resolve/compile here.
  for (const auto& [index, save, restore] :
       {std::array<uint32_t, 3>{24, 0x8172D3B8, 0x8172D408},
        std::array<uint32_t, 3>{27, 0x8172D3C4, 0x8172D414},
        std::array<uint32_t, 3>{28, 0x8172D3C8, 0x8172D418}}) {
    for (const bool returning : {false, true}) {
      auto* symbol =
          module.xex_module()->LookupSymbol(returning ? restore : save, false);
      if (!symbol || symbol->type() != cpu::Symbol::Type::kFunction) {
        return false;
      }
      auto* function = static_cast<cpu::Function*>(symbol);
      if (!function->IsSaverest() ||
          function->SaverestType() != cpu::SaveRestoreType::GPR ||
          function->SaverestIndex() != index ||
          (returning ? !function->IsRestore() : !function->IsSave()) ||
          function->behavior() != (returning
                                       ? cpu::Function::Behavior::kEpilogReturn
                                       : cpu::Function::Behavior::kProlog)) {
        return false;
      }
    }
  }
  return true;
}
bool ProfileAvailable(const GuestRuntimeCallExecutor& calls,
                      NativeSystemUiPublicCall call) {
  const auto* p = NativeSystemUiPublicProfile(call);
  if (!p || p->family != NativeSystemPublicFamily::kManagedFormatIo) {
    return false;
  }
  const auto& v = calls.validated();
  const auto f = v.catalog.find(std::string(p->identity));
  const auto c = v.contracts.find(std::string(p->identity));
  return f != v.catalog.end() && c != v.contracts.end() &&
         f->second.address == p->address &&
         f->second.arguments.size() == p->argument_count &&
         c->second.caller ==
             ValidatedGuestRuntimePlan::CallerPolicy::kOriginalTitleOrSystem &&
         c->second.result == ValidatedGuestRuntimePlan::ResultPolicy::kAny;
}
// Explicit virtual-backend HRESULT policy, not a claim about every retail
// filesystem error. Native FE still maps negative HRESULT to public 65B plus
// this actual extended error through its original Complete routine.
uint32_t HResult(std::error_code error) {
  if (!error) {
    return 0;
  }
  if (error == std::errc::operation_canceled) {
    return kCanceled;
  }
  if (error == std::errc::device_or_resource_busy ||
      error == std::errc::operation_would_block) {
    return kBusy;
  }
  if (error == std::errc::no_such_device) {
    return 0x8007048F;
  }
  if (error == std::errc::permission_denied ||
      error == std::errc::operation_not_permitted) {
    return 0x80070005;
  }
  if (error == std::errc::no_space_on_device) {
    return 0x80070070;
  }
  return kFailed;
}
}  // namespace
struct NativeManagedFormatIo::State {
  struct Request {
    std::shared_ptr<const NativeManagedFormatSelection> selection;
    object_ref<XThread> caller;
    NativeImportDomain::Operation domain;
    std::shared_ptr<KernelState::UserModuleExecutionLease> caller_code;
    std::array<uint64_t, 6> args;
    Pair copy, completion;
    XThread* completing_thread = nullptr;
    uint32_t task = 0, handler_result = kFailed;
    bool handler_entered = false, handler_done = false, completed = false,
         uncertain = false, retiring = false, retired = false;
    std::atomic<bool> canceled{false};
    // The only operation published here is the real backend handle. Cancel
    // takes no filesystem action and never destroys it under native/GCRC locks.
    std::mutex operation_mutex;
    std::shared_ptr<xam::ContentManager::ManagedFormat> operation;
    void Cancel() {
      canceled.store(true, std::memory_order_release);
      std::lock_guard lock(operation_mutex);
      if (operation) {
        operation->Cancel();
      }
    }
  };
  struct Scope {
    XThread* caller;
    NativeSystemUiPublicCall call;
    std::vector<uint64_t> args;
    std::shared_ptr<Request> request;
  };
  explicit State(Config value) : config(std::move(value)) {}
  Config config;
  std::mutex mutex, drain_mutex;
  std::condition_variable changed;
  bool stopping = false, retirement_stop = false, retirement_failed = false;
  std::thread retirement;
  size_t active_calls = 0;
  std::map<uint32_t, std::shared_ptr<Request>>
      requests;  // actual original overlap
  std::map<XThread*, std::shared_ptr<Scope>>
      scopes;  // real fiber identity, NOT host TLS
  std::set<uint64_t> consumed_selections;
  bool Context(cpu::ppc::PPCContext& c, UserModule& module, bool system) {
    if (!XThread::IsInThread()) {
      return false;
    }
    auto* t = XThread::GetCurrentThread();
    const auto sp = Pointer(c.r[1]), pcr = Pointer(c.r[13]);
    return t && XThread::IsInThread(t) && sp && *sp && !(*sp & 15) && pcr &&
           *pcr == t->pcr_ptr() && t->kernel_state() == config.kernel &&
           t->thread_state() && t->thread_state()->context() == &c &&
           c.thread_state == t->thread_state() &&
           c.kernel_state == config.kernel &&
           c.processor == config.kernel->processor() &&
           (!system || t->native_frame_thread_identity().process_type ==
                           X_PROCTYPE_SYSTEM) &&
           &module == config.calls->module().get() &&
           module.load_generation() == config.calls->load_generation() &&
           module.native_import_ready() && config.domain->active() &&
           config.authorize_native_context(c, module) &&
           CheckCode(*config.kernel, module);
  }
  Scope* CurrentScope(XThread* t, NativeSystemUiPublicCall call) {
    const auto f = scopes.find(t);
    return f != scopes.end() && f->second->call == call ? f->second.get()
                                                        : nullptr;
  }
  std::optional<uint32_t> CurrentTask() {
    auto* t = XThread::GetCurrentThread();
    auto& memory = *config.kernel->memory();
    const auto slot = Word(memory, 0x81A82570);
    uint32_t tls = 0;
    if (!t || !slot || !t->GetTLSValue(*slot, &tls)) {
      return {};
    }
    const auto context = Word(memory, tls);
    return context && *context && *context <= UINT32_MAX - 0xC
               ? Word(memory, *context + 0xC)
               : std::nullopt;
  }
  std::shared_ptr<Request> FindTaskPacket(uint32_t task, uint32_t packet) {
    for (const auto& [overlap, request] : requests) {
      if (request->copy.bound && !request->copy.poisoned &&
          request->copy.packet == packet && !request->completed &&
          (!request->task || request->task == task)) {
        return request;
      }
    }
    return {};
  }
  bool CanRetire(const std::shared_ptr<Request>& request) const {
    if (!request->completed || request->uncertain || request->retiring ||
        request->retired) {
      return false;
    }
    return std::none_of(scopes.begin(), scopes.end(), [&](const auto& item) {
      return item.second->request == request;
    });
  }
  void Reap() {
    // This runs only on our owned host retirement worker / external Drain.
    // Move destructible owners off records before callbacks can be their last
    // shared owner; no native/GCRC/State lock crosses any graph teardown.
    std::vector<std::shared_ptr<Request>> retired;
    {
      std::lock_guard lock(mutex);
      for (const auto& [overlap, request] : requests) {
        if (CanRetire(request)) {
          retired.push_back(request);
          request->retiring = true;
        }
      }
    }
    for (const auto& request : retired) {
      std::shared_ptr<xam::ContentManager::ManagedFormat> operation;
      {
        std::lock_guard lock(request->operation_mutex);
        operation = std::move(request->operation);
      }
      operation.reset();
      request->caller_code.reset();
      request->caller.reset();
      request->domain = NativeImportDomain::Operation{};
      {
        std::lock_guard lock(mutex);
        request->retired = true;
        request->retiring = false;
      }
    }
  }
  void RetireLoop() noexcept try {
    HostOwnerScope host(this);
    for (;;) {
      {
        std::unique_lock lock(mutex);
        changed.wait(lock, [&] {
          return retirement_stop ||
                 std::any_of(
                     requests.begin(), requests.end(),
                     [&](const auto& item) { return CanRetire(item.second); });
        });
        if (retirement_stop) {
          return;
        }
      }
      Reap();
      changed.notify_all();
    }
  } catch (...) {
    std::lock_guard lock(mutex);
    retirement_failed = true;
    changed.notify_all();
  }
};
NativeManagedFormatIo::NativeManagedFormatIo(std::shared_ptr<State> state)
    : state_(std::move(state)) {}
NativeManagedFormatIo::~NativeManagedFormatIo() = default;
std::shared_ptr<NativeManagedFormatIo> NativeManagedFormatIo::Create(
    Config config) {
  bool owned_bridge = false;
  if (config.calls) {
    for (const auto& b : config.calls->validated().bridges) {
      if (b.address == kFormat && b.code_size == 268 &&
          b.code_sha256 ==
              "72b85802c32d4c9ddfb4d535ddde60aee77fc4b13b9b0419dbd1c20dbafd22d"
              "8" &&
          b.service == "managed_format_owned_request") {
        owned_bridge = true;
      }
    }
  }
  if (!owned_bridge || !config.kernel || !config.calls || !config.domain ||
      !config.content || !config.authorize_original_caller ||
      !config.authorize_native_context ||
      config.calls->kernel() != config.kernel || !config.domain->active() ||
      config.domain->root() !=
          NativeImportDomain::Member{config.calls->module()->hmodule_ptr(),
                                     config.calls->load_generation()} ||
      !ProfileAvailable(*config.calls,
                        NativeSystemUiPublicCall::kManagedFormatStart) ||
      !ProfileAvailable(*config.calls,
                        NativeSystemUiPublicCall::kManagedFormatCancel) ||
      !CheckCode(*config.kernel, *config.calls->module())) {
    return {};
  }
  return std::shared_ptr<NativeManagedFormatIo>(
      new NativeManagedFormatIo(std::make_shared<State>(std::move(config))));
}
std::unique_ptr<NativeManagedFormatIo::Installation>
NativeManagedFormatIo::Install(std::shared_ptr<NativeManagedFormatIo> owner) {
  if (!owner) {
    return {};
  }
  std::lock_guard lock(installed_mutex);
  if (installed.contains(owner->state_->config.kernel)) {
    return {};
  }
  auto result = std::unique_ptr<Installation>(new Installation(owner));
  installed.emplace(owner->state_->config.kernel, owner);
  try {
    owner->state_->retirement =
        std::thread([state = owner->state_] { state->RetireLoop(); });
  } catch (...) {
    installed.erase(owner->state_->config.kernel);
    // Disarm the temporary Installation without running a guest-context Drain.
    result->owner_.reset();
    return {};
  }
  return result;
}
NativeManagedFormatIo::Installation::~Installation() {
  if (!owner_) {
    return;
  }
  owner_->BeginStop();
  if (!owner_->Drain()) {
    return;  // Retain installed strong owner on uncertainty.
  }
  std::lock_guard lock(installed_mutex);
  const auto f = installed.find(owner_->state_->config.kernel);
  if (f != installed.end() && f->second == owner_) {
    installed.erase(f);
  }
}
std::shared_ptr<NativeManagedFormatIo> NativeManagedFormatIo::Lookup(
    KernelState* kernel) {
  std::lock_guard lock(installed_mutex);
  const auto f = installed.find(kernel);
  return f == installed.end() ? nullptr : f->second;
}
bool NativeManagedFormatIo::CurrentScopeMatches(
    GuestRuntimeCallExecutor* calls, XThread& t, NativeSystemUiPublicCall call,
    std::span<const uint64_t> args) {
  auto owner = Lookup(t.kernel_state());
  if (!owner || calls != owner->state_->config.calls.get() ||
      !XThread::IsInThread(&t)) {
    return false;
  }
  std::lock_guard lock(owner->state_->mutex);
  auto* scope = owner->state_->CurrentScope(&t, call);
  return scope && scope->args.size() == args.size() &&
         std::equal(args.begin(), args.end(), scope->args.begin());
}
GuestRuntime::ExecutionResult NativeManagedFormatIo::StartOriginal(
    XThread& thread,
    std::shared_ptr<const NativeManagedFormatSelection> selection,
    std::span<const uint64_t> args) {
  if (global_critical_region::is_held_by_current_thread() || !selection ||
      !selection->proof() || !XThread::IsInThread(&thread) ||
      args.size() != 6 ||
      !NativeSystemUiPublicArgumentsValid(
          NativeSystemUiPublicCall::kManagedFormatStart, args) ||
      args[0] != 0xFE || args[1] != 0x2000B ||
      args[2] != selection->overlap() || !args[2] || args[4] != 4 || !args[3] ||
      !args[5] || !selection->serial() ||
      !state_->config.authorize_original_caller(thread, *selection, false)) {
    return Failure("Managed format original caller/selection denied");
  }
  auto& memory = *state_->config.kernel->memory();
  std::array<uint8_t, 0x1C> overlap_bytes;
  if (!internal::ReadNativeWaitSnapshot(memory, uint32_t(args[2]),
                                        overlap_bytes) ||
      memory.QueryRangeAccess(uint32_t(args[2]), overlap_bytes.size()) !=
          memory::PageAccess::kReadWrite) {
    return Failure("Managed format overlap backing denied");
  }
  const auto device = Word(memory, uint32_t(args[3]));
  const auto options = Word(memory, uint32_t(args[5]));
  const auto reserved = args[5] <= UINT32_MAX - 4
                            ? Word(memory, uint32_t(args[5]) + 4)
                            : std::nullopt;
  if (!device || *device != selection->registry_device_id() || !options ||
      *options != 0x80002 || !reserved || *reserved) {
    return Failure("Managed format exact native payload/options denied");
  }
  auto request = std::make_shared<State::Request>();
  request->selection = std::move(selection);
  request->caller = retain_object(&thread);
  auto title = state_->config.kernel->GetExecutableModule();
  if (!title || title->hmodule_ptr() != request->selection->title_hmodule() ||
      title->load_generation() != request->selection->title_load_generation()) {
    return Failure("Managed format title generation changed");
  }
  request->caller_code =
      state_->config.kernel->AcquireUserModuleExecutionLease(title);
  if (!request->caller_code) {
    return Failure("Managed format title code admission denied");
  }
  std::copy(args.begin(), args.end(), request->args.begin());
  request->domain = state_->config.domain->Acquire();
  if (!request->domain) {
    return Failure("Managed format domain closed");
  }
  auto scope = std::make_shared<State::Scope>(
      State::Scope{&thread, NativeSystemUiPublicCall::kManagedFormatStart,
                   std::vector<uint64_t>(args.begin(), args.end()), request});
  {
    std::lock_guard lock(state_->mutex);
    const auto prior = state_->requests.find(uint32_t(args[2]));
    const auto live =
        std::count_if(state_->requests.begin(), state_->requests.end(),
                      [](const auto& item) { return !item.second->retired; });
    if (state_->stopping || live >= 16 ||
        state_->consumed_selections.size() >= 1024 ||
        (prior != state_->requests.end() && !prior->second->retired) ||
        state_->scopes.contains(&thread) ||
        !state_->consumed_selections.insert(request->selection->serial())
             .second) {
      return Failure("Managed format duplicate/busy ingress");
    }
    state_->requests.insert_or_assign(uint32_t(args[2]), request);
    state_->scopes.emplace(&thread, scope);
    ++state_->active_calls;
  }
  auto result = Failure("Managed format native Start did not return");
  try {
    result =
        state_->config.calls->ExecuteManagedFormatRegistersOnOriginalCaller(
            thread, scope->call, args);
  } catch (...) {
    request->Cancel();
  }
  {
    std::lock_guard lock(state_->mutex);
    state_->scopes.erase(&thread);
    --state_->active_calls;
    if (!result.succeeded ||
        (int32_t(result.return_value) >= 0 && !request->copy.bound)) {
      request->uncertain = true;
    }
    // A verified normal negative Start returns through its original cleanup;
    // it never schedules this request, so no native Complete is expected.
    if (result.succeeded && int32_t(result.return_value) < 0) {
      request->completed = true;
    }
    state_->changed.notify_all();
  }
  return result;
}
std::optional<GuestRuntime::ExecutionResult>
NativeManagedFormatIo::CancelOriginal(XThread& thread,
                                      std::span<const uint64_t> args) {
  if (global_critical_region::is_held_by_current_thread() || args.size() != 2 ||
      !NativeSystemUiPublicArgumentsValid(
          NativeSystemUiPublicCall::kManagedFormatCancel, args) ||
      !XThread::IsInThread(&thread)) {
    return {};
  }
  std::shared_ptr<State::Request> request;
  {
    std::lock_guard lock(state_->mutex);
    const auto f = state_->requests.find(uint32_t(args[0]));
    if (f == state_->requests.end() || state_->scopes.contains(&thread)) {
      return {};
    }
    request = f->second;
  }
  if (!state_->config.authorize_original_caller(thread, *request->selection,
                                                true)) {
    return Failure("Managed format cancel caller denied");
  }
  uint32_t owned_packet;
  bool completing;
  {
    std::lock_guard lock(state_->mutex);
    owned_packet = request->copy.bound && !request->copy.poisoned
                       ? request->copy.packet
                       : 0;
    completing = request->completion.entered || request->completed;
  }
  const auto association =
      args[0] <= UINT32_MAX - 8
          ? Word(*state_->config.kernel->memory(), uint32_t(args[0]) + 8)
          : std::nullopt;
  if (!association || !owned_packet ||
      ((*association & ~1u) != owned_packet &&
       !(!*association && completing))) {
    return Failure("Managed cancel overlap no longer names its actual request");
  }
  auto domain_operation = state_->config.domain->Acquire();
  if (!domain_operation) {
    return Failure("Managed format cancel domain closed");
  }
  // Set before original native Cancel can wait for this actual Task.
  request->Cancel();
  auto scope = std::make_shared<State::Scope>(
      State::Scope{&thread, NativeSystemUiPublicCall::kManagedFormatCancel,
                   std::vector<uint64_t>(args.begin(), args.end()), request});
  {
    std::lock_guard lock(state_->mutex);
    if (state_->scopes.contains(&thread)) {
      return Failure("Managed format nested cancel");
    }
    state_->scopes.emplace(&thread, scope);
    ++state_->active_calls;
  }
  auto result = Failure("Managed format native Cancel did not return");
  try {
    result =
        state_->config.calls->ExecuteManagedFormatRegistersOnOriginalCaller(
            thread, scope->call, args);
  } catch (...) {
  }
  {
    std::lock_guard lock(state_->mutex);
    state_->scopes.erase(&thread);
    --state_->active_calls;
    if (!result.succeeded) {
      request->uncertain = true;
    }
    state_->changed.notify_all();
  }
  return result;
}
void NativeManagedFormatIo::ObserveOwnedPacketEntry(cpu::ppc::PPCContext& c,
                                                    UserModule& module,
                                                    uint32_t at) noexcept try {
  auto owner = Lookup(c.kernel_state);
  if (!owner || at != kCopy || !owner->state_->Context(c, module, false) ||
      uint32_t(c.lr) != 0x816A2D30) {
    return;
  }
  auto& s = *owner->state_;
  auto* thread = XThread::GetCurrentThread();
  std::lock_guard lock(s.mutex);
  auto* scope =
      s.CurrentScope(thread, NativeSystemUiPublicCall::kManagedFormatStart);
  if (!scope ||
      Pointer(c.r[3]) != std::optional(uint32_t(scope->request->args[3])) ||
      c.r[4] != 4 || !Pointer(c.r[5]) || uint32_t(c.r[1]) > UINT32_MAX - 0x50 ||
      uint32_t(c.r[5]) != uint32_t(c.r[1]) + 0x50) {
    return;
  }
  if (!scope->request->copy.Enter(uint32_t(c.r[1]), uint32_t(c.lr),
                                  uint32_t(c.r[5]))) {
    scope->request->uncertain = true;
  }
} catch (...) {
}
void NativeManagedFormatIo::ObserveOwnedPacketReturn(cpu::ppc::PPCContext& c,
                                                     UserModule& module,
                                                     uint32_t at) noexcept try {
  auto owner = Lookup(c.kernel_state);
  if (!owner || at != kCopy || !owner->state_->Context(c, module, false)) {
    return;
  }
  auto& s = *owner->state_;
  std::shared_ptr<State::Request> request;
  uint32_t output = 0;
  {
    std::lock_guard lock(s.mutex);
    auto* scope = s.CurrentScope(XThread::GetCurrentThread(),
                                 NativeSystemUiPublicCall::kManagedFormatStart);
    if (!scope) {
      return;
    }
    request = scope->request;
    output = request->copy.output;
  }
  if (int32_t(uint32_t(c.r[3])) < 0) {
    return;  // Original allocator failed; no packet was published.
  }
  auto& memory = *s.config.kernel->memory();
  const auto packet = Word(memory, output);
  const auto input = packet && *packet <= UINT32_MAX - 0x1C
                         ? Word(memory, *packet + 0xC)
                         : std::nullopt;
  const auto device = packet && *packet <= UINT32_MAX - 0x1C
                          ? Word(memory, *packet + 0x1C)
                          : std::nullopt;
  std::lock_guard lock(s.mutex);
  if (!packet || !*packet || (*packet & 3) || !input ||
      *input != *packet + 0x1C || !device ||
      *device != request->selection->registry_device_id() ||
      !request->copy.Bind(uint32_t(c.r[1]), uint32_t(c.lr), *packet)) {
    request->copy.poisoned = true;
    request->uncertain = true;
  }
} catch (...) {
}
void NativeManagedFormatIo::ObserveCancelEntry(cpu::ppc::PPCContext& c,
                                               UserModule& module,
                                               uint32_t at) noexcept try {
  auto owner = Lookup(c.kernel_state);
  if (!owner || at != kCancel || !owner->state_->Context(c, module, false)) {
    return;
  }
  auto& s = *owner->state_;
  const auto overlap = Pointer(c.r[3]);
  if (!overlap || !*overlap) {
    return;
  }
  std::shared_ptr<State::Request> request;
  bool scope_matches = false;
  uint32_t owned_packet = 0;
  {
    std::lock_guard lock(s.mutex);
    const auto f = s.requests.find(*overlap);
    if (f == s.requests.end()) {
      return;
    }
    request = f->second;
    owned_packet = request->copy.bound ? request->copy.packet : 0;
    auto* scope =
        s.CurrentScope(XThread::GetCurrentThread(),
                       NativeSystemUiPublicCall::kManagedFormatCancel);
    scope_matches = scope && scope->request == request;
  }
  // The original public-caller scope is itself authenticated. An internal
  // cancel needs actual native-domain code ancestry, never an arbitrary ID.
  if (!scope_matches &&
      (s.config.kernel->ResolveNativeImportCaller(uint32_t(c.lr)).domain !=
           s.config.domain ||
       s.config.kernel->GetUserModuleForAddress(uint32_t(c.lr) - 4).get() !=
           &module)) {
    return;
  }
  const auto packet = *overlap <= UINT32_MAX - 8
                          ? Word(*s.config.kernel->memory(), *overlap + 8)
                          : std::nullopt;
  if (owned_packet && packet && (*packet & ~1u) == owned_packet) {
    request->Cancel();
  }
} catch (...) {
}
std::optional<uint32_t> NativeManagedFormatIo::TryHandleFormat(
    cpu::ppc::PPCContext& c, UserModule& module, uint32_t at) noexcept try {
  auto owner = Lookup(c.kernel_state);
  if (!owner || at != kFormat || !owner->state_->Context(c, module, true)) {
    return {};
  }
  auto& s = *owner->state_;
  // Only RAM/code reads occur in this phase. No State lock crosses a GCRC
  // acquire, cached registry check or filesystem/backend operation.
  const auto task = s.CurrentTask();
  auto& memory = *s.config.kernel->memory();
  const auto callback = task && *task <= UINT32_MAX - 0x34
                            ? Word(memory, *task + 0x30)
                            : std::nullopt;
  const auto packet = task && *task <= UINT32_MAX - 0x34
                          ? Word(memory, *task + 0x34)
                          : std::nullopt;
  if (!task || !*task || !callback || *callback != 0x816A2C80 || !packet) {
    return {};
  }
  std::shared_ptr<State::Request> request;
  {
    std::lock_guard lock(s.mutex);
    request = s.FindTaskPacket(*task, *packet);
  }
  if (!request) {
    return {};
  }
  if (*packet > UINT32_MAX - 0x1C) {
    return kInvalid;
  }
  const auto app = Word(memory, *packet), message = Word(memory, *packet + 4),
             overlap = Word(memory, *packet + 8),
             input = Word(memory, *packet + 0xC),
             association = Word(memory, *packet + 0x18),
             device = Word(memory, *packet + 0x1C);
  bool stopped;
  {
    std::lock_guard lock(s.mutex);
    if (request->uncertain || request->handler_entered || request->completed ||
        uint32_t(c.lr) != 0x81687D28 || !app || *app != 0xFE || !message ||
        *message != 0x2000B || !overlap ||
        *overlap != request->selection->overlap() || !input ||
        *input != *packet + 0x1C || !association ||
        (*association && (*association & ~1u) != *task) || !device ||
        *device != request->selection->registry_device_id() ||
        Pointer(c.r[3]) != device ||
        !s.consumed_selections.contains(request->selection->serial())) {
      return kInvalid;
    }
    request->task = *task;
    request->handler_entered = true;
    ++s.active_calls;
    stopped = s.stopping;
  }
  uint32_t result = kFailed;
  struct Finish {
    State& s;
    State::Request& r;
    uint32_t& result;
    ~Finish() {
      std::lock_guard lock(s.mutex);
      r.handler_result = result;
      r.handler_done = true;
      --s.active_calls;
      s.changed.notify_all();
    }
  } finish{s, *request, result};
  if (stopped || request->canceled.load(std::memory_order_acquire)) {
    return result = kCanceled;
  }
  if (global_critical_region::is_held_by_current_thread()) {
    return result = kBusy;
  }
  // This borrowed current Task remains inside its original callback and keeps
  // AppInfo's original shared execution pin. WaitOnFence parks a guest fiber
  // noninterruptibly, leaving the scheduler available for genuine Cancel/Wait.
  threading::Fence completed;
  std::thread worker;
  try {
    worker = std::thread([&] {
      HostOwnerScope host(&s);
      try {
        if (request->canceled.load(std::memory_order_acquire)) {
          result = kCanceled;
        } else {
          std::error_code error;
          auto operation = s.config.content->BeginIdleManagedFormat(
              request->selection->proof(), error);
          if (!operation) {
            result = HResult(error ? error
                                   : std::make_error_code(std::errc::io_error));
          } else {
            auto active = std::shared_ptr<xam::ContentManager::ManagedFormat>(
                std::move(operation));
            {
              std::lock_guard lock(request->operation_mutex);
              request->operation = active;
              if (request->canceled.load(std::memory_order_acquire)) {
                active->Cancel();
              }
            }
            const auto receipt = active->Execute();
            result = HResult(receipt.error);
            if (!receipt.error && !receipt.complete) {
              result = kFailed;
            }
          }
        }
      } catch (...) {
        result = kFailed;
      }
      std::shared_ptr<xam::ContentManager::ManagedFormat> release;
      {
        std::lock_guard lock(request->operation_mutex);
        release = std::move(request->operation);
      }
      release.reset();
      completed.Signal();
      if (auto* scheduler = s.config.kernel->guest_scheduler()) {
        scheduler->WakeAll();
      }
    });
    GuestScheduler::WaitOnFence(completed);
    worker.join();  // No work survives the handler, even on cancellation.
  } catch (...) {
    request->Cancel();
    if (worker.joinable()) {
      worker.join();
    }
    result = kFailed;
  }
  return result;
} catch (...) {
  return kFailed;
}
void NativeManagedFormatIo::ObserveCompletionEntry(cpu::ppc::PPCContext& c,
                                                   UserModule& module,
                                                   uint32_t at) noexcept try {
  auto owner = Lookup(c.kernel_state);
  if (!owner || at != kComplete || !owner->state_->Context(c, module, true) ||
      uint32_t(c.lr) != 0x81687F54) {
    return;
  }
  auto& s = *owner->state_;
  const auto task = s.CurrentTask(), overlap = Pointer(c.r[3]);
  if (!task || !overlap) {
    return;
  }
  std::lock_guard lock(s.mutex);
  const auto f = s.requests.find(*overlap);
  if (f == s.requests.end()) {
    return;
  }
  auto& r = *f->second;
  if (!r.handler_done || r.task != *task ||
      c.r[4] != (int32_t(r.handler_result) < 0 ? 0x65Bu : 0u) ||
      uint32_t(c.r[5]) != r.handler_result || c.r[6] != 0 ||
      !r.completion.Enter(uint32_t(c.r[1]), uint32_t(c.lr), *overlap)) {
    r.uncertain = true;
    return;
  }
  r.completing_thread = XThread::GetCurrentThread();
} catch (...) {
}
void NativeManagedFormatIo::ObserveCompletionReturn(cpu::ppc::PPCContext& c,
                                                    UserModule& module,
                                                    uint32_t at) noexcept try {
  auto owner = Lookup(c.kernel_state);
  if (!owner || at != kComplete || !owner->state_->Context(c, module, true)) {
    return;
  }
  auto& s = *owner->state_;
  const auto task = s.CurrentTask();
  if (!task) {
    return;
  }
  std::lock_guard lock(s.mutex);
  for (const auto& [overlap, r] : s.requests) {
    if (r->task != *task ||
        r->completing_thread != XThread::GetCurrentThread() || r->completed) {
      continue;
    }
    if (!r->completion.Bind(uint32_t(c.r[1]), uint32_t(c.lr), overlap) ||
        r->uncertain) {
      r->uncertain = true;
      return;
    }
    // Original native Complete has now returned normally after its own event,
    // APC queue/ref handling and request free. No freed packet is read here.
    r->completed = true;
    s.changed.notify_all();
    return;
  }
} catch (...) {
}
void NativeManagedFormatIo::BeginStop() noexcept try {
  std::lock_guard lock(state_->mutex);
  state_->stopping = true;
  // Cancel only sets the real operation's atomic. No callbacks/IO/owner
  // destruction occur while these short metadata locks are held.
  for (const auto& [overlap, request] : state_->requests) {
    request->Cancel();
  }
} catch (...) {
}

bool NativeManagedFormatIo::Drain() noexcept try {
  if (XThread::IsInThread() || host_io_owner == state_.get() ||
      global_critical_region::is_held_by_current_thread()) {
    return false;
  }
  std::lock_guard drain(state_->drain_mutex);
  BeginStop();
  bool done;
  {
    std::unique_lock lock(state_->mutex);
    done = state_->changed.wait_for(lock, std::chrono::seconds(30), [&] {
      if (state_->retirement_failed || state_->active_calls ||
          !state_->scopes.empty()) {
        return false;
      }
      return std::all_of(state_->requests.begin(), state_->requests.end(),
                         [](const auto& item) {
                           return item.second->completed &&
                                  !item.second->uncertain;
                         });
    });
    state_->retirement_stop = true;
  }
  state_->changed.notify_all();
  if (state_->retirement.joinable()) {
    state_->retirement.join();
  }
  if (!done) {
    return false;  // Active/uncertain requests and installation stay owned.
  }
  state_->Reap();
  return true;
} catch (...) {
  return false;
}
}  // namespace xe::kernel
