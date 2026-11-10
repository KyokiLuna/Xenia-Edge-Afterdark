/** Xenia : Xbox 360 Emulator Research Project; BSD license, see LICENSE. */
#include "xenia/kernel/native_managed_format_frontdoor.h"
#include <algorithm>
#include <mutex>
#include <set>
#include <utility>
#include "third_party/mbedtls/include/mbedtls/sha256.h"
#include "xenia/base/byte_order.h"
#include "xenia/base/mutex.h"
#include "xenia/cpu/function.h"
#include "xenia/cpu/xex_module.h"
#include "xenia/kernel/guest_runtime_call_executor.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/native_import_domain.h"
#include "xenia/kernel/native_managed_format_io.h"
#include "xenia/kernel/user_module.h"
#include "xenia/kernel/util/native_wait_snapshot.h"
#include "xenia/kernel/xam/content_manager.h"
#include "xenia/kernel/xam/content_source.h"
#include "xenia/kernel/xthread.h"
#include "xenia/memory.h"
namespace xe::kernel {
namespace {
std::mutex installations_mutex;
std::map<KernelState*, std::weak_ptr<NativeManagedFormatFrontdoor>> installations;
std::set<KernelState*> installing;
struct InstallationReservation {
  KernelState* kernel;
  bool committed = false;
  ~InstallationReservation() {
    if (!committed) {
      std::lock_guard lock(installations_mutex);
      installing.erase(kernel);
    }
  }
};
std::optional<uint32_t> Pointer(uint64_t value) {
  const auto low = uint32_t(value);
  return low && (value == low || value == uint64_t(int64_t(int32_t(low))))
             ? std::optional(low) : std::nullopt;
}
std::optional<uint32_t> Word(Memory& memory, uint32_t at) {
  std::array<uint8_t, 4> b;
  if (!internal::ReadNativeWaitSnapshot(memory, at, b)) return {};
  return xe::load_and_swap<uint32_t>(b.data());
}
std::shared_ptr<NativeManagedFormatFrontdoor> Lookup(KernelState* kernel) {
  std::lock_guard lock(installations_mutex);
  const auto i = installations.find(kernel);
  return i == installations.end() ? nullptr : i->second.lock();
}
}  // namespace
uint64_t NativeManagedFormatFrontdoor::Lineage::Serial() {
  if (!next_ || next_ == UINT64_MAX) { Stop(); return 0; }
  return next_++;
}
void NativeManagedFormatFrontdoor::Lineage::Stop() {
  stopped_ = true; calls_.clear(); deliveries_.clear(); objects_.clear();
  selected_.clear(); scenes_.clear(); progress_.clear(); constructing_.clear();
}
std::optional<NativeManagedFormatFrontdoor::Call>
NativeManagedFormatFrontdoor::Lineage::Pending(Frame f, uint32_t pc) const {
  const auto i = calls_.find({f.thread, pc});
  return i != calls_.end() && i->second.frame == f ? std::optional(i->second) : std::nullopt;
}
void NativeManagedFormatFrontdoor::Lineage::Deliver(Frame f, uint32_t handle,
    uint32_t output, const Raw& raw, std::shared_ptr<const xam::ManagedFormatSourceProof> proof, uint32_t id) {
  if (stopped_ || !f.valid() || !proof || !id || !handle || f.lr != 0x922645C4 ||
      f.sp > UINT32_MAX - 0x60 || output != f.sp + 0x60 ||
      xe::load_and_swap<uint32_t>(raw.data()) != id) return;
  const auto owner = calls_.find({f.thread, 0x92264450});
  if (owner == calls_.end() || owner->second.frame.title != f.title ||
      owner->second.frame.generation != f.generation || owner->second.frame.pcr != f.pcr ||
      owner->second.frame.sp < 0xF0 || owner->second.frame.sp - 0xF0 != f.sp) return;
  if (deliveries_.size() >= 32 && !deliveries_.contains(f.thread)) { Stop(); return; }
  const auto serial = Serial();
  if (!serial) return;
  deliveries_[f.thread] = {f, output, handle, {raw, std::move(proof), id, serial}};
}
bool NativeManagedFormatFrontdoor::Lineage::Enter(Frame f, uint32_t pc,
    std::array<uint32_t, 5> args, const Snapshot& observed) {
  if (stopped_ || !f.valid()) return false;
  if (pc == 0x9218D9E8 || pc == 0x922F63D8 || pc == 0x922F6398) {
    const auto scene = scenes_.find(args[0]);
    const uint64_t scene_serial = scene == scenes_.end() ? 0 : scene->second.serial;
    const bool pending = std::any_of(calls_.begin(), calls_.end(), [&](const auto& item) {
      const auto function = item.first.second;
      const auto& call = item.second;
      // A registered scene destructor during a factory is ambiguous before
      // its out pointer is published. Refuse that lineage, not a later return.
      // Ordinary allocator deletes of unrelated member objects do not do this.
      return ((pc == 0x922F63D8 || pc == 0x922F6398) &&
              (function == 0x922F7930 || function == 0x922F78B0 || function == 0x922F58D0)) ||
             (scene_serial && call.scene_serial == scene_serial) ||
             (call.progress && call.progress == args[0]);
    });
    if (pending) { Stop(); return false; }
    objects_.erase(args[0]); scenes_.erase(args[0]); progress_.erase(args[0]);
    constructing_.erase(args[0]); return false;
  }
  // A new authenticated entry cancels ambiguous abandoned pairs. SP/LR are
  // correlations, never unique invocation serials.
  const auto key = std::pair{f.thread, pc};
  if (calls_.contains(key) || calls_.size() >= 128 || objects_.size() >= 512 ||
      scenes_.size() >= 32 || progress_.size() >= 32) { Stop(); return false; }
  Call call{f, args}; call.serial = Serial();
  if (!call.serial) return false;
  switch (pc) {
    case 0x92264450: deliveries_.erase(f.thread); break;
    case 0x92264280: {
      objects_.erase(args[0]); constructing_.erase(args[0]);
      const auto d = deliveries_.find(f.thread);
      if (d == deliveries_.end()) return false;
      auto delivered = std::move(d->second); deliveries_.erase(d);
      if (f.lr != 0x92264570 || args[1] != delivered.output ||
          f.title != delivered.frame.title || f.generation != delivered.frame.generation ||
          f.pcr != delivered.frame.pcr || f.sp != delivered.frame.sp || !observed.raw ||
          *observed.raw != delivered.row.raw) return false;
      call.source = std::move(delivered.row); break;
    }
    case 0x92263BF8: {
      const auto ctor = calls_.find({f.thread, 0x92264280});
      const auto object = objects_.find(args[0]);
      if (ctor != calls_.end() && ctor->second.args[0] == args[0]) {
        // Derived64280 and base64210 each own0x60 bytes before this call.
        if (f.lr != 0x92264264 || ctor->second.frame.sp < 0xC0 ||
            f.sp != ctor->second.frame.sp - 0xC0 ||
            f.title != ctor->second.frame.title || f.generation != ctor->second.frame.generation ||
            f.pcr != ctor->second.frame.pcr) return false;
        call.source = ctor->second.source;
        call.refresh_parent_serial = ctor->second.serial;
        call.refresh_constructor = true;
      } else if (object != objects_.end()) {
        call.source = object->second;
        call.refresh_parent_serial = object->second.serial;
      } else return false;
      if (!call.source.proof || !observed.raw || *observed.raw != call.source.raw) return false;
      break;
    }
    case 0x922F73B8: {
      objects_.erase(args[0]); constructing_.erase(args[0]);
      const auto source = objects_.find(args[1]);
      if (source == objects_.end() || !observed.raw || *observed.raw != source->second.raw) return false;
      call.source = source->second; break;
    }
    case 0x922F9B08: {
      selected_.erase(args[0]);
      const auto source = objects_.find(args[1]);
      if (source != objects_.end() && observed.raw && *observed.raw == source->second.raw) call.source = source->second;
      break;
    }
    case 0x922F7930: {
      const auto selected = selected_.find(observed.manager);
      const auto object = objects_.find(observed.selected);
      if (selected == selected_.end() || object == objects_.end() ||
          selected->second.serial != object->second.serial || !observed.raw ||
          *observed.raw != object->second.raw) return false;
      call.source = object->second; break;
    }
    case 0x922F58D0: {
      if (f.lr != 0x923083BC || args[1] != 0x9202E0CC || args[2] || args[3] || args[4]) return false;
      Scene* scene = nullptr;
      for (auto& [object, candidate] : scenes_) if (candidate.hxui == args[0]) {
        if (scene) { Stop(); return false; } scene = &candidate;
      }
      if (!scene || scene->transferred) return false;
      scene->transferred = true;
      call.source = scene->row; call.scene_serial = scene->serial; break;
    }
    case 0x922F78B0: {
      const auto loader = calls_.find({f.thread, 0x922F58D0});
      if (loader == calls_.end() || !loader->second.source.proof || loader->second.ambiguous ||
          loader->second.frame.pcr != f.pcr || loader->second.frame.title != f.title ||
          loader->second.frame.generation != f.generation || loader->second.frame.sp <= f.sp) return false;
      if (loader->second.progress) { loader->second.ambiguous = true; return false; }
      call.source = loader->second.source; call.scene_serial = loader->second.serial; break;
    }
    default: return false;
  }
  if (pc == 0x92264280 || pc == 0x922F73B8) constructing_[args[0]] = call.serial;
  calls_.emplace(key, std::move(call)); return true;
}
void NativeManagedFormatFrontdoor::Lineage::Leave(Frame f, uint32_t pc, uint32_t result,
    const Snapshot& observed) {
  const auto found = calls_.find({f.thread, pc});
  if (stopped_ || found == calls_.end()) return;
  if (!(found->second.frame == f)) { Stop(); return; }
  auto call = std::move(found->second); calls_.erase(found);
  switch (pc) {
    case 0x92264450: deliveries_.erase(f.thread); break;
    case 0x92264280: case 0x922F73B8:
      if (const auto claim = constructing_.find(call.args[0]);
          claim != constructing_.end() && claim->second == call.serial) {
        constructing_.erase(claim);
        if (!call.ambiguous && (pc != 0x92264280 || call.localized) &&
            call.source.proof && result == call.args[0] && observed.raw && *observed.raw == call.source.raw) {
          call.source.serial = call.serial; objects_[result] = std::move(call.source);
        }
      }
      break;
    case 0x92263BF8: {
      const bool accepted = !call.ambiguous && call.localized && observed.raw &&
                            *observed.raw == *call.localized;
      if (call.refresh_constructor) {
        const auto ctor = calls_.find({f.thread, 0x92264280});
        const auto claim = constructing_.find(call.args[0]);
        if (ctor == calls_.end() || claim == constructing_.end() ||
            ctor->second.serial != call.refresh_parent_serial ||
            claim->second != call.refresh_parent_serial) break;
        if (accepted && ctor->second.source.proof == call.source.proof) {
          ctor->second.source.raw = *call.localized;
          ctor->second.localized = *call.localized;
        } else {
          ctor->second.ambiguous = true;
          constructing_.erase(claim);
        }
      } else {
        const auto object = objects_.find(call.args[0]);
        if (object == objects_.end() || object->second.serial != call.refresh_parent_serial) break;
        if (accepted && object->second.proof == call.source.proof)
          object->second.raw = *call.localized;  // Same proof AND creation serial.
        else objects_.erase(object);           // Never upgrade by numeric ID.
      }
      break;
    }
    case 0x922F9B08: {
      const auto object = objects_.find(observed.selected);
      if (call.source.proof && object != objects_.end() && object->second.proof == call.source.proof &&
          object->second.raw == call.source.raw && object->second.serial != call.source.serial &&
          observed.raw && *observed.raw == call.source.raw) selected_[call.args[0]] = object->second;
      break;
    }
    case 0x922F7930:
      if (!int32_t(result) && call.source.proof && observed.object && observed.hxui == call.args[0])
        scenes_[observed.object] = {std::move(call.source), call.serial, observed.hxui, false};
      break;
    case 0x922F78B0: {
      if (int32_t(result) || !observed.object || observed.hxui != call.args[0]) break;
      const auto loader = calls_.find({f.thread, 0x922F58D0});
      if (loader == calls_.end() || loader->second.serial != call.scene_serial) break;
      if (loader->second.progress) { loader->second.ambiguous = true; break; }
      loader->second.progress = observed.object; loader->second.hxui = observed.hxui;
      loader->second.source.serial = call.serial;
      break;
    }
    case 0x922F58D0:
      if (int32_t(result) >= 0 && call.progress && !call.ambiguous &&
          observed.hxui == call.hxui && call.source.proof)
        progress_[call.progress] = std::move(call.source);
      break;
  }
}
std::optional<NativeManagedFormatFrontdoor::Call>
NativeManagedFormatFrontdoor::Lineage::RefreshForLocalized(Frame f, uint32_t output) const {
  const auto i = calls_.find({f.thread, 0x92263BF8});
  if (stopped_ || i == calls_.end()) return {};
  const auto& call = i->second;
  if (f.title != call.frame.title || f.generation != call.frame.generation ||
      f.pcr != call.frame.pcr || f.lr != 0x92263C2C || call.frame.sp < 0x90 ||
      f.sp != call.frame.sp - 0x90 || call.args[0] > UINT32_MAX - 0x100 ||
      output != call.args[0] + 0x100) return {};
  return call;
}
bool NativeManagedFormatFrontdoor::Lineage::Localized(Frame f, uint32_t output,
    const Raw& raw, std::shared_ptr<const xam::ManagedFormatSourceProof> proof,
    uint32_t device, Geometry geometry) {
  const auto pending = RefreshForLocalized(f, output);
  if (!pending) return false;  // Invalid/untrusted input never consumes a pair.
  auto& call = calls_.find({f.thread, 0x92263BF8})->second;
  if (call.ambiguous) return false;
  if (call.localized || !call.source.proof || device != call.source.device ||
      xe::load_and_swap<uint32_t>(raw.data()) != device) {
    call.ambiguous = true; return false;
  }
  if (proof != call.source.proof) {
    // A genuine current639 observation found replacement/unavailability. It
    // may revoke the expected old capability, never upgrade an old row/menu
    // using the same numeric ID. Active IO still owns its real cancellation
    // and native completion; these maps only close new selection admission.
    const auto retired = call.source.proof;
    const auto erase_rows = [&](auto& rows) {
      std::erase_if(rows, [&](const auto& item) { return item.second.proof == retired; });
    };
    erase_rows(objects_); erase_rows(selected_); erase_rows(progress_);
    std::erase_if(scenes_, [&](const auto& item) { return item.second.row.proof == retired; });
    for (auto& [key, pending_call] : calls_) if (pending_call.source.proof == retired) {
      pending_call.ambiguous = true;
      pending_call.source.proof.reset();
      pending_call.localized.reset();
    }
    return false;
  }
  if (!proof || xe::load_and_swap<uint32_t>(raw.data() + 4) !=
          xe::load_and_swap<uint32_t>(call.source.raw.data() + 4)) {
    call.ambiguous = true; return false;
  }
  auto expected = raw;
  // These are the exact native stores92263C38..54, authenticated by the whole
  // refresh profile. No field is exempted from the final80-byte comparison.
  if (geometry.cached) {
    xe::store_and_swap<uint64_t>(expected.data() + 8, geometry.total);
    xe::store_and_swap<uint64_t>(expected.data() + 0x10,
        geometry.used < geometry.total ? geometry.total - geometry.used : 0);
  }
  call.localized = expected;
  return true;
}
std::optional<NativeManagedFormatFrontdoor::Object>
NativeManagedFormatFrontdoor::Lineage::Progress(uint32_t at) const {
  const auto i = progress_.find(at);
  return stopped_ || i == progress_.end() ? std::nullopt : std::optional(i->second);
}
uint64_t NativeManagedFormatFrontdoor::Lineage::ProgressSerial(uint32_t at) const {
  const auto value = Progress(at); return value ? value->serial : 0;
}

namespace {
struct Profile { uint32_t at, bytes; const char* sha; };
constexpr Profile dash_profiles[] = {
 {0x92263BF8, 476, "ccf100c6f09db42fdbaac3130716725c226f7138bd2cd9e3448495710fed5312"},
 {0x92198C6C, 40, "b8fccdaa02d7e723b870874165494a80d5aed7dc4a1873105ecf874e775b9a01"},
 {0x92264450, 652, "95086cf1b50df6e52cb5073bc19e028b7d4e1cedb78c4264504062d736af44fd"},
 {0x92264280, 104, "b34f0476ae6e3750fe2de95f6261d0d136f273d0556f446bae1237ee12165aff"},
 {0x922F73B8, 144, "b1c0165b4419f6624987ec0153f6f43564bd2f292802c14283768c527dc6874e"},
 {0x922F9B08, 108, "de520e72d406440c7597700ae35e22639a58e55c86888c05a91b50a84dbcc1a4"},
 {0x922F7930, 124, "9950649b36900b749637f53343e4119253c0a3b2befe6b090108a849f4e350f1"},
 {0x922F78B0, 128, "9985742a381765eafe56aff7abb67933aa9ed59677dcadd6e25aecb93419df91"},
 {0x922F63D8, 64, "02f349f7025bca72e67ce789449fb6fa990df02f8652cdb742174fe833ad7edb"},
 {0x922F6398, 64, "52b14482139ea164aea318ac2894fe7b4719b99bf66a3f3211729eace39e4271"},
 {0x922F58D0, 228, "da56e16125a3147e81d4fdc7b6130a5777f8ffa94708db793f2e416a58d5cdf2"},
 {0x923082F0, 232, "3be645731b331852079ea17fc1be6468de7891e2710b9945901a09e5ca84dd4a"},
 {0x922FE2E0, 396, "ae9fc3b04bd86d4831446d8e524afa42f7545aacd40b44328d78738e49af67ee"},
 {0x9218D9E8, 4, "c71ac7b9b462975936deed7fade03f35e974d3857b80bc2f8a9fd407eee5a79b"},
 {0x92198C1C, 112, "21c42317f3c8b7c26f7432bb0ffbeb4d23f01e79a0260d320c56b553c99f7058"},
 {0x92264210, 108, "0c28c6410047e7fbe209c81a3a6e2da726784f79a746ace54e7b0ece09973050"},
 {0x922F5FF0, 312, "9d62336f936977690bf0a9025de824fbb69949facd5df72ae370751c56650c6a"},
};
bool Code(KernelState& kernel, UserModule& module) {
  for (const auto& p : dash_profiles) {
    if (kernel.memory()->QueryRangeAccess(p.at, p.bytes) == memory::PageAccess::kNoAccess) return false;
    for (uint32_t a = p.at; a != p.at + p.bytes; a += 4) {
      xex2_section_type type;
      if (kernel.GetUserModuleForAddress(a).get() != &module ||
          !module.xex_module()->GetPageSectionType(a, &type) || type != XEX_SECTION_CODE ||
          module.xex_module()->TranslateCode(a) != kernel.memory()->TranslateVirtual<const uint8_t*>(a)) return false;
    }
    std::array<uint8_t, 32> hash{};
    mbedtls_sha256(kernel.memory()->TranslateVirtual<const uint8_t*>(p.at), p.bytes, hash.data(), 0);
    constexpr char hex[] = "0123456789abcdef";
    for (size_t i = 0; i != hash.size(); ++i)
      if (p.sha[i*2] != hex[hash[i] >> 4] || p.sha[i*2+1] != hex[hash[i] & 15]) return false;
  }
  for (const auto [at, first] : {std::pair{0x92198C6Cu, 25u}, std::pair{0x92198C78u, 28u}, std::pair{0x92198C7Cu, 29u}}) {
    const auto* symbol = module.xex_module()->LookupSymbol(at, false);
    if (!symbol || symbol->type() != cpu::Symbol::Type::kFunction || symbol->module() != module.xex_module()) return false;
    const auto* f = static_cast<const cpu::Function*>(symbol);
    if (!f->IsSaverest() || !f->IsRestore() || f->SaverestType() != cpu::SaveRestoreType::GPR ||
        f->SaverestIndex() != first || f->behavior() != cpu::Function::Behavior::kEpilogReturn) return false;
  }
  return true;
}
}  // namespace
struct NativeManagedFormatFrontdoor::State {
  explicit State(Config value) : config(std::move(value)) {}
  Config config;
  std::mutex mutex;
  Lineage lineage;
  bool stopping = false;
  object_ref<UserModule> dash;
  std::shared_ptr<KernelState::UserModuleExecutionLease> dash_code;
  std::shared_ptr<NativeManagedFormatIo> io;
  std::unique_ptr<NativeManagedFormatIo::Installation> io_installation;
  std::optional<Frame> ReadFrame(cpu::ppc::PPCContext& ctx, uint32_t pc,
                                 bool closing_cancel = false) {
    if (!XThread::IsInThread()) return {};
    auto* thread = XThread::GetCurrentThread();
    const auto sp = Pointer(ctx.r[1]), pcr = Pointer(ctx.r[13]);
    if (!sp || (*sp & 15) || !pcr || thread->kernel_state() != config.kernel ||
        thread->thread_state()->context() != &ctx || ctx.thread_state != thread->thread_state() ||
        ctx.kernel_state != config.kernel || ctx.processor != config.kernel->processor() ||
        *pcr != thread->pcr_ptr() || !uint32_t(ctx.lr)) return {};
    const auto process = thread->native_frame_thread_identity().process_type;
    if (process != X_PROCTYPE_TITLE && process != X_PROCTYPE_SYSTEM) return {};
    auto global = global_critical_region::AcquireDirect();
    auto module = config.kernel->GetExecutableModule();
    if (!module || module->module_format() != UserModule::kModuleFormatXex || module->title_id() != 0xFFFE07D1 ||
        config.kernel->GetUserModuleForAddress(pc).get() != module.get() || !Code(*config.kernel, *module)) return {};
    const auto caller = config.kernel->GetUserModuleForAddress(uint32_t(ctx.lr) - 4);
    xex2_section_type section;
    if (!caller || caller->module_format() != UserModule::kModuleFormatXex ||
        !caller->xex_module()->GetPageSectionType(uint32_t(ctx.lr) - 4, &section) || section != XEX_SECTION_CODE) return {};
    // Only actual native XUI may call the registered Dash scene factories.
    // Ordinary Dash does not become a SYSTEM domain member by this check.
    if (caller.get() != module.get() &&
        ((pc != 0x922F7930 && pc != 0x922F78B0 && pc != 0x922F63D8 && pc != 0x922F6398) ||
         caller.get() != config.calls->module().get() ||
         config.kernel->ResolveNativeImportCaller(uint32_t(ctx.lr)).domain != config.domain)) return {};
    // A closing cancellation uses the already-retained exact Dash code lease;
    // it cannot acquire/adopt another generation after admission is sealed.
    auto lease = closing_cancel ? std::shared_ptr<KernelState::UserModuleExecutionLease>{}
                                : config.kernel->AcquireUserModuleExecutionLease(module);
    if (!closing_cancel && !lease) return {};
    {
      std::lock_guard lock(mutex);
      if ((!closing_cancel && stopping) || (closing_cancel && (!dash || !dash_code)) ||
          (dash && (dash.get() != module.get() || dash->load_generation() != module->load_generation()))) return {};
      if (!dash) { dash = module; dash_code = std::move(lease); }
    }
    return Frame{module->hmodule_ptr(), thread->thread_id(), *pcr, *sp, uint32_t(ctx.lr), module->load_generation()};
  }
  bool NativeContext(cpu::ppc::PPCContext& ctx, UserModule& module) {
    if (!XThread::IsInThread() || &module != config.calls->module().get() ||
        module.load_generation() != config.calls->load_generation() || !config.domain->active()) return false;
    auto* t = XThread::GetCurrentThread(); const auto pcr = Pointer(ctx.r[13]);
    const auto process = t->native_frame_thread_identity().process_type;
    return pcr && *pcr == t->pcr_ptr() && t->kernel_state() == config.kernel &&
        t->thread_state()->context() == &ctx && ctx.thread_state == t->thread_state() &&
        ctx.kernel_state == config.kernel && ctx.processor == config.kernel->processor() &&
        (process == X_PROCTYPE_TITLE || process == X_PROCTYPE_SYSTEM);
  }
  bool Original(XThread& t, const NativeManagedFormatSelection& s, bool cancel) {
    auto* ctx = t.thread_state()->context();
    const auto f = ctx ? ReadFrame(*ctx, 0x922FE2E0, cancel) : std::nullopt;
    if (!f || f->title != s.title_hmodule() || f->generation != s.title_load_generation() ||
        (!cancel && t.native_frame_thread_identity().process_type != X_PROCTYPE_TITLE) ||
        (!cancel && f->lr != 0x922FE414)) return false;
    std::lock_guard lock(mutex);
    // Already-admitted Cancel/Wait must remain available during BeginStop so
    // the real Progress destructor cannot free its overlap before native drain.
    // The IO owner has already joined this exact existing request; this path
    // cannot create a fresh selection or Start.
    return cancel || (!stopping && lineage.ProgressSerial(s.progress_object()) == s.serial());
  }
};
NativeManagedFormatFrontdoor::NativeManagedFormatFrontdoor(std::shared_ptr<State> value) : state_(std::move(value)) {}
NativeManagedFormatFrontdoor::~NativeManagedFormatFrontdoor() { BeginStop(); }
std::shared_ptr<NativeManagedFormatFrontdoor> NativeManagedFormatFrontdoor::Create(Config config) {
  if (!config.kernel || !config.calls || !config.domain || !config.content ||
      config.calls->kernel() != config.kernel || config.calls->module()->hmodule_ptr() != config.domain->root().hmodule ||
      config.calls->load_generation() != config.domain->root().generation) return {};
  // Reserve/preallocate before IO installation starts its owned host worker.
  // Never hold this registry lock across code/GCRC checks or worker creation.
  {
    std::lock_guard lock(installations_mutex);
    auto [slot, inserted] = installations.try_emplace(config.kernel);
    if (!slot->second.expired() || installing.contains(config.kernel)) return {};
    installing.insert(config.kernel);
  }
  InstallationReservation reservation{config.kernel};
  auto state = std::make_shared<State>(std::move(config));
  auto owner = std::shared_ptr<NativeManagedFormatFrontdoor>(new NativeManagedFormatFrontdoor(state));
  std::weak_ptr<State> weak = state;
  NativeManagedFormatIo::Config io;
  io.kernel = state->config.kernel; io.calls = state->config.calls;
  io.domain = state->config.domain; io.content = state->config.content;
  io.authorize_original_caller = [weak](XThread& t, const NativeManagedFormatSelection& s, bool cancel) {
    const auto self = weak.lock(); return self && self->Original(t, s, cancel);
  };
  io.authorize_native_context = [weak](cpu::ppc::PPCContext& ctx, UserModule& m) {
    const auto self = weak.lock(); return self && self->NativeContext(ctx, m);
  };
  state->io = NativeManagedFormatIo::Create(std::move(io));
  state->io_installation = state->io ? NativeManagedFormatIo::Install(state->io) : nullptr;
  if (!state->io_installation) return {};
  {
    std::lock_guard lock(installations_mutex);
    // The reserved slot already exists, so this publication cannot allocate
    // after the IO worker is installed. A duplicate factory never starts IO.
    installations.find(state->config.kernel)->second = owner;
    installing.erase(state->config.kernel);
    reservation.committed = true;
  }
  return owner;
}
void NativeManagedFormatFrontdoor::ObserveDeviceEnumeration(KernelState* kernel,
    uint32_t handle, uint32_t output, std::span<const uint8_t, 0x50> bytes,
    std::shared_ptr<const xam::ManagedFormatSourceProof> proof, uint32_t id) noexcept {
  try {
    const auto self = Lookup(kernel);
    if (!self || !proof || !XThread::IsInThread()) return;
    auto& ctx = *XThread::GetCurrentThread()->thread_state()->context();
    const auto frame = self->state_->ReadFrame(ctx, 0x92264450);
    if (!frame || frame->sp > UINT32_MAX - 0x60 ||
        ctx.r[3] != handle || ctx.r[4] != 2 || ctx.r[5] != output ||
        ctx.r[6] != 0x50 || ctx.r[7] != frame->sp + 0x5C || ctx.r[8] ||
        Word(*kernel->memory(), frame->sp + 0x5C) != std::optional(1u)) return;
    Raw actual;
    if (!internal::ReadNativeWaitSnapshot(*kernel->memory(), output, actual) || !std::equal(bytes.begin(), bytes.end(), actual.begin())) return;
    std::lock_guard lock(self->state_->mutex);
    self->state_->lineage.Deliver(*frame, handle, output, actual, std::move(proof), id);
  } catch (...) {}
}
void NativeManagedFormatFrontdoor::ObserveLocalizedDeviceData(KernelState* kernel,
    uint32_t output, std::span<const uint8_t, 0x50> produced,
    std::shared_ptr<const xam::ManagedFormatSourceProof> proof, uint32_t device) noexcept {
  try {
    const auto self = Lookup(kernel);
    if (!self || !XThread::IsInThread()) return;
    auto& ctx = *XThread::GetCurrentThread()->thread_state()->context();
    const auto frame = self->state_->ReadFrame(ctx, 0x92263BF8);
    if (!frame || uint32_t(ctx.r[3]) != device || Pointer(ctx.r[4]) != std::optional(output)) return;
    std::optional<Call> call;
    { std::lock_guard lock(self->state_->mutex);
      call = self->state_->lineage.RefreshForLocalized(*frame, output); }
    if (!call) return;
    Raw actual;
    std::array<uint8_t, 20> cached;
    if (!internal::ReadNativeWaitSnapshot(*kernel->memory(), output, actual) ||
        !std::equal(produced.begin(), produced.end(), actual.begin()) ||
        call->args[0] > UINT32_MAX - 0x1E4 ||
        !internal::ReadNativeWaitSnapshot(*kernel->memory(), call->args[0] + 0x1E4, cached)) return;
    const Geometry geometry{xe::load_and_swap<uint32_t>(cached.data()) != 0,
        xe::load_and_swap<uint64_t>(cached.data()+4),
        xe::load_and_swap<uint64_t>(cached.data()+12)};
    std::lock_guard lock(self->state_->mutex);
    self->state_->lineage.Localized(*frame, output, actual, std::move(proof), device, geometry);
  } catch (...) { if (auto self = Lookup(kernel)) self->BeginStop(); }
}
void NativeManagedFormatFrontdoor::ObserveEntry(void* raw, uint64_t value) noexcept {
  try {
    auto* ctx = static_cast<cpu::ppc::PPCContext*>(raw);
    if (!ctx || !ctx->kernel_state || !HooksFunction(uint32_t(value))) return;
    if (value < 0x92000000) {
      auto module = ctx->kernel_state->GetUserModuleForAddress(uint32_t(value));
      if (!module) return;
      if (value == 0x816A2A48) NativeManagedFormatIo::ObserveOwnedPacketEntry(*ctx, *module, uint32_t(value));
      else if (value == 0x816A26F8) NativeManagedFormatIo::ObserveCancelEntry(*ctx, *module, uint32_t(value));
      else if (value == 0x816A2588) NativeManagedFormatIo::ObserveCompletionEntry(*ctx, *module, uint32_t(value));
      return;
    }
    const auto self = Lookup(ctx->kernel_state);
    if (!self) return;
    const auto frame = self->state_->ReadFrame(*ctx, uint32_t(value));
    if (!frame) return;
    std::array<uint32_t, 5> args{};
    const size_t count = value == 0x922F58D0 ? 5 :
        (value == 0x92264280 ? 3 :
         (value == 0x922F73B8 || value == 0x922F9B08 ||
          value == 0x922F7930 || value == 0x922F78B0 ? 2 : 1));
    for (size_t i = 0; i != count; ++i) {
      const auto v = ctx->r[i+3];
      if (v != uint32_t(v) && v != uint64_t(int64_t(int32_t(v)))) return;
      args[i] = uint32_t(v);
    }
    Snapshot observed; auto& memory = *ctx->kernel_state->memory();
    if (value == 0x92264280 || value == 0x922F73B8 || value == 0x922F9B08 || value == 0x92263BF8) {
      const uint64_t at = value == 0x92263BF8 ? uint64_t(args[0]) + 0x100 :
          uint64_t(args[1]) + (value == 0x92264280 ? 0 : 0x100);
      Raw bytes;
      if (at <= UINT32_MAX && internal::ReadNativeWaitSnapshot(memory, uint32_t(at), bytes)) observed.raw = bytes;
    } else if (value == 0x922F7930) {
      observed.manager = Word(memory, 0x92A46B80).value_or(0);
      if (observed.manager && observed.manager <= UINT32_MAX - 0x304)
        observed.selected = Word(memory, observed.manager + 0x304).value_or(0);
      Raw bytes;
      if (observed.selected && observed.selected <= UINT32_MAX - 0x100 &&
          internal::ReadNativeWaitSnapshot(memory, observed.selected + 0x100, bytes)) observed.raw = bytes;
    }
    std::lock_guard lock(self->state_->mutex);
    self->state_->lineage.Enter(*frame, uint32_t(value), args, observed);
  } catch (...) { if (raw) if (auto self = Lookup(static_cast<cpu::ppc::PPCContext*>(raw)->kernel_state)) self->BeginStop(); }
}
void NativeManagedFormatFrontdoor::ObserveReturn(void* raw, uint64_t value) noexcept {
  try {
    auto* ctx = static_cast<cpu::ppc::PPCContext*>(raw);
    if (!ctx || !ctx->kernel_state || !HooksFunction(uint32_t(value))) return;
    if (value < 0x92000000) {
      auto module = ctx->kernel_state->GetUserModuleForAddress(uint32_t(value));
      if (!module) return;
      if (value == 0x816A2A48) NativeManagedFormatIo::ObserveOwnedPacketReturn(*ctx, *module, uint32_t(value));
      else if (value == 0x816A2588) NativeManagedFormatIo::ObserveCompletionReturn(*ctx, *module, uint32_t(value));
      return;
    }
    const auto self = Lookup(ctx->kernel_state);
    if (!self) return;
    const auto frame = self->state_->ReadFrame(*ctx, uint32_t(value));
    if (!frame) return;
    std::optional<Call> call;
    { std::lock_guard lock(self->state_->mutex); call = self->state_->lineage.Pending(*frame, uint32_t(value)); }
    if (!call) return;
    Snapshot observed; auto& memory = *ctx->kernel_state->memory(); Raw bytes;
    if ((value == 0x92264280 || value == 0x922F73B8 || value == 0x92263BF8) && call->args[0] <= UINT32_MAX - 0x100 &&
        internal::ReadNativeWaitSnapshot(memory, call->args[0] + 0x100, bytes)) observed.raw = bytes;
    if (value == 0x922F9B08 && call->args[0] <= UINT32_MAX - 0x304) {
      observed.selected = Word(memory, call->args[0] + 0x304).value_or(0);
      if (observed.selected && observed.selected <= UINT32_MAX - 0x100 &&
          internal::ReadNativeWaitSnapshot(memory, observed.selected + 0x100, bytes)) observed.raw = bytes;
    }
    if (value == 0x922F7930 || value == 0x922F78B0) {
      observed.object = Word(memory, call->args[1]).value_or(0);
      if (observed.object && observed.object <= UINT32_MAX - 8) observed.hxui = Word(memory, observed.object + 8).value_or(0);
    }
    if (value == 0x922F58D0 && frame->sp >= 0x150) observed.hxui = Word(memory, frame->sp - 0x150).value_or(0);
    std::lock_guard lock(self->state_->mutex);
    self->state_->lineage.Leave(*frame, uint32_t(value), uint32_t(ctx->r[3]), observed);
  } catch (...) { if (raw) if (auto self = Lookup(static_cast<cpu::ppc::PPCContext*>(raw)->kernel_state)) self->BeginStop(); }
}
std::optional<GuestRuntime::ExecutionResult> NativeManagedFormatFrontdoor::TryStart(
    KernelState* kernel, const std::array<uint64_t, 6>& args) noexcept {
  if (args[0] != 0xFE || args[1] != 0x2000B) return {};
  const GuestRuntime::ExecutionResult denied{true, uint32_t(X_E_NOT_IMPLEMENTED), {}};
  try {
    const auto self = Lookup(kernel);
    if (!self || !XThread::IsInThread()) return denied;
    for (const auto arg : args) if (arg > UINT32_MAX) return denied;
    auto* t = XThread::GetCurrentThread(); auto* ctx = t->thread_state()->context();
    const auto frame = self->state_->ReadFrame(*ctx, 0x922FE2E0);
    if (!frame || frame->lr != 0x922FE414 || args[4] != 4 || args[2] < 0xE0 ||
        args[3] != args[2] - 0xE0 + 0x58) return denied;
    const uint32_t progress = uint32_t(args[2] - 0xE0);
    const auto payload = Word(*kernel->memory(), uint32_t(args[3]));
    std::array<uint8_t, 8> options;
    if (!payload || !internal::ReadNativeWaitSnapshot(*kernel->memory(), uint32_t(args[5]), options) ||
        xe::load_and_swap<uint32_t>(options.data()) != 0x00080002 || xe::load_and_swap<uint32_t>(options.data()+4)) return denied;
    auto selected = std::shared_ptr<NativeManagedFormatSelection>(new NativeManagedFormatSelection);
    {
      std::lock_guard lock(self->state_->mutex);
      const auto row = self->state_->lineage.Progress(progress);
      if (self->state_->stopping || !row || row->device != *payload) return denied;
      selected->proof_ = row->proof; selected->device_ = row->device;
      selected->progress_ = progress; selected->overlap_ = uint32_t(args[2]);
      selected->title_ = frame->title; selected->generation_ = frame->generation; selected->serial_ = row->serial;
    }
    return self->state_->io->StartOriginal(*t, std::move(selected), args);
  } catch (...) { return denied; }
}
std::optional<GuestRuntime::ExecutionResult> NativeManagedFormatFrontdoor::TryCancel(
    KernelState* kernel, const std::array<uint64_t, 2>& args) noexcept {
  try {
    const auto self = Lookup(kernel);
    return self && XThread::IsInThread() ? self->state_->io->CancelOriginal(*XThread::GetCurrentThread(), args) : std::nullopt;
  } catch (...) { return GuestRuntime::ExecutionResult{false, uint32_t(X_E_FAIL), "Managed Cancel exception"}; }
}
void NativeManagedFormatFrontdoor::BeginStop() noexcept {
  { std::lock_guard lock(state_->mutex); state_->stopping = true; state_->lineage.Stop(); }
  if (state_->io) state_->io->BeginStop();
}
bool NativeManagedFormatFrontdoor::Drain() noexcept {
  BeginStop();
  if (state_->io && !state_->io->Drain()) return false;
  state_->io_installation.reset(); return true;
}
}  // namespace xe::kernel
