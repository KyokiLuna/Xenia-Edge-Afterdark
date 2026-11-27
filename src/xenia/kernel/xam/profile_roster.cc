/** Xenia : Xbox 360 Emulator Research Project; BSD license, see LICENSE. */
#include "xenia/kernel/xam/profile_roster.h"

#include <atomic>
#include <algorithm>
#include <new>
#include <cstring>
#include "xenia/base/assert.h"
#include "xenia/vfs/devices/host_path_device.h"
#include <limits>
#include <set>
#include <source_location>
#include <utility>
#include "xenia/base/logging.h"
#include "xenia/base/threading.h"
#include "xenia/base/native_system_ui_trace.h"
#include "xenia/cpu/thread_state.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/native_profile_identity_owner.h"
#include "xenia/kernel/xam/content_manager.h"
#include "xenia/kernel/xam/profile_manager.h"
#include "xenia/kernel/xam/user_tracker.h"
#include "xenia/kernel/xthread.h"

namespace xe::kernel::xam {
namespace {
std::error_code Busy() { return std::make_error_code(std::errc::device_or_resource_busy); }
std::error_code Stale() { return std::make_error_code(std::errc::operation_canceled); }
XThread* ActualRosterThread() noexcept {
  auto* thread = XThread::GetCurrentFiberThread();
  return thread ? thread : (XThread::IsInThread() ? XThread::GetCurrentThread() : nullptr);
}
}
// Preallocated diagnostics carry no resource owner. A reader never waits for
// state1 and never reads its payload; release2 publishes a complete first cut.
template <typename T> struct NativeProfileDiagnosticCell {
  std::atomic<uint32_t> state{0}; T payload{};
  void Record(const T& value) noexcept {
    uint32_t empty=0;
    if(!state.compare_exchange_strong(empty,1,std::memory_order_relaxed))return;
    payload=value;state.store(2,std::memory_order_release);
  }
  bool Copy(T& out,uint32_t* observed=nullptr) const noexcept {
    const auto complete=state.load(std::memory_order_acquire);if(observed)*observed=complete;
    if(complete!=2)return false;
    out=payload;return true;
  }
};
// Independent scalar backing: an observation retaining this object cannot
// retain the PM, roster rows, Account/model/storage, Kernel or provider owners.
struct ProfileManager::NativeCreationDiagnosticSink {
  std::array<std::array<NativeProfileDiagnosticCell<NativeCreationDiagnostic>,2>,4> first;
  NativeProfileDiagnosticCell<NativeCreationDiagnostic> consumer_terminal;
  uint64_t original_generation=0;
  std::atomic<uint64_t> original_sequence{1};
  std::atomic<uint32_t> original_next{0};
  std::atomic<bool> original_exhausted{false};
  std::array<std::array<NativeProfileDiagnosticCell<NativeOriginalLogonDiagnostic>,
      NativeOriginalLogonDiagnostic::kStageCount>,8> original;

};
struct NativeProfileProjection {
  uint32_t slot=0,reason=0,available=0,ordinary=0,scoped=0,following=0;
  uint32_t thread=0,pcr=0,process=0,profile_present=0;
  uint64_t lr=0,sp=0,current_epoch=0,selected_epoch=0,selected_xuid=0;
  uint64_t intent=0,operation=0;uint32_t phase=0;
};
struct NativeProfileQuery {
  uint32_t ordinal=0,slot=0,status=0,flags=0,thread=0,pcr=0,ordinary=0,open=0;
  uint64_t value=0,lr=0,sp=0;
};
struct NativeSigninUiObservation {
  NativeProfileQuery caller;
  uint32_t user=0,users=0,flags=0,overlap=0,result=0,route=0,stage=0,native_owned=0;
};
struct HostRoster::State {
  std::array<ProfileRosterSlot, 4> slots;
  uint64_t epoch = 1;
};
// Only this actual PM owns native row resources. Public snapshots, cells,
// intents and tickets can retain this metadata after Close, but Close drains
// entered scopes and detaches every native owner before PM destruction.
struct ProfileRosterAuthority {
  struct Row {
    std::shared_ptr<UserProfile> profile;
    std::shared_ptr<ProfileGpdModelAuthority> model;
    std::shared_ptr<ProfileAccountIdentity> identity;
    std::shared_ptr<const ProfileAccountSnapshot> account;
    std::shared_ptr<ProfileStorageLease> storage;
    // One exact owed mount reference; unchanged rows share it without taking
    // another reference. Only explicit cleanup consumes it.
    std::shared_ptr<ProfileRosterMountTicket> mount;
  };
  struct Envelope {
    std::shared_ptr<const HostRoster> roster;
    std::array<Row, 4> rows;
  };
  struct Operation;
  ProfileManager* owner = nullptr; // usable only under the existing PM gate
  KernelState* kernel = nullptr;  // comparison only in metadata admissions
  std::shared_ptr<ProfileAccountCommitGate> gate;
  std::atomic<bool> open{true}, ordinary{true}, selected_read{true};
  std::atomic<uint64_t> scopes{0};
  std::atomic<bool> diagnostic_enabled{false};
  std::array<NativeProfileDiagnosticCell<NativeProfileProjection>,4> first_projection;
  // Valid slots0..3 plus one invalid-slot bucket, independently per ordinal.
  std::array<std::array<NativeProfileDiagnosticCell<NativeProfileQuery>,5>,2> first_query;
  NativeProfileDiagnosticCell<NativeSigninUiObservation> first_signin_ui;
  // Live host observations are atomic scalars published at ORIGINAL mutation
  // cuts. A diagnostic host reader must never contend with TITLE try-locks.
  std::atomic<uint32_t> diagnostic_available{0},diagnostic_phase{0};
  std::atomic<bool> diagnostic_pending{false};
  std::atomic<uint64_t> diagnostic_current_epoch{0},diagnostic_selected_epoch{0};
  std::atomic<uint64_t> diagnostic_intent{0},diagnostic_operation{0};
  std::array<std::atomic<uint64_t>,4> diagnostic_current_xuid{},diagnostic_selected_xuid{};
  void DiagnosticCurrent(const Envelope* e) noexcept {
    diagnostic_current_epoch.store(e?e->roster->epoch():0,std::memory_order_release);
    for(size_t i=0;i<4;++i)diagnostic_current_xuid[i].store(e?e->roster->slots()[i].xuid:0,std::memory_order_release);
    diagnostic_available.fetch_or(1,std::memory_order_release);
  }
  void DiagnosticSelected(const Envelope* e) noexcept {
    diagnostic_selected_epoch.store(e?e->roster->epoch():0,std::memory_order_release);
    for(size_t i=0;i<4;++i)diagnostic_selected_xuid[i].store(e?e->roster->slots()[i].xuid:0,std::memory_order_release);
    diagnostic_available.fetch_or(2,std::memory_order_release);
  }
  std::atomic<Envelope*> selected{nullptr};
  std::shared_ptr<Envelope> current;
  std::shared_ptr<Operation> pending;
  std::vector<std::shared_ptr<Envelope>> retired;
  // Data handoff removed these operations from pending. They retain only
  // their own delayed cleanup; no later callback may replay old map/pause work.
  std::vector<std::shared_ptr<Operation>> retiring;
  std::weak_ptr<::xe::kernel::NativeProfileIdentityOwner> native_owner;
  std::shared_ptr<const ProfileRosterNativeBinding> binding;
  uint64_t next_nonce = 1;
  uint64_t next_operation = 1;
  std::vector<std::weak_ptr<ProfileRosterNativeReadTicket>> tickets; // metadata only

  // Enter/Leave never acquire a native/PM/model lock or release resources.
  bool EnterScope() noexcept {
    if (!open.load(std::memory_order_acquire)) { return false; }
    auto n = scopes.load(std::memory_order_relaxed);
    do {
      if (n == UINT64_MAX) { return false; }
    } while (!scopes.compare_exchange_weak(n, n + 1, std::memory_order_acq_rel));
    if (open.load(std::memory_order_acquire)) { return true; }
    scopes.fetch_sub(1, std::memory_order_acq_rel); scopes.notify_all();
    return false;
  }
  void LeaveScope() noexcept {
    scopes.fetch_sub(1, std::memory_order_acq_rel); scopes.notify_all();
  }
  static bool LogicalCurrent(const Envelope& e) {
    for (const auto& row : e.rows) {
      if (row.profile && (!row.mount || !row.storage || row.storage->retiring() ||
          !row.identity || row.identity->retired() || !row.account ||
          row.account->identity() != row.identity)) { return false; }
    }
    return true;
  }
};
struct ProfileRosterExpected::Proof {
  std::weak_ptr<ProfileRosterAuthority> authority;
  const HostRoster* exact = nullptr;
};
struct ProfileRosterIntent::State {
  std::weak_ptr<ProfileRosterAuthority> authority;
  std::shared_ptr<const HostRoster> old_roster, new_roster, canceled_empty_roster;
  std::array<uint8_t, 4 * 0x188> native_rows{};
  std::atomic<ProfileRosterPhase> phase{ProfileRosterPhase::kPreparing};
  // Advisory request only. Only CancelRoster's original PM-locked transition
  // may change phase, and entered/published native operations are excluded.
  std::atomic<bool> preparation_cancel_requested{false};
  std::weak_ptr<::xe::kernel::NativeProfileIdentityOwner> preparation_owner;
  ProfileRosterResult result, canceled_empty_result;
  // Zero before publication; 1=requested, 2=canceled-empty. Chosen once before
  // the Published release. Neither prepared metadata nor wire rows change.
  std::atomic<uint32_t> published_choice{0};
  // Set once from the actual held PM envelopes before publishing Prepared.
  // Empty slot numbers alone never substitute for absent native row owners.
  bool empty_unchanged_rows = false;
  bool unchanged_owners = false;
  enum Milestone:uint32_t { kProtocol=1, kRead=2, kMaps=4, kWriters=8, kRetired=16, kDataHandoff=32 };
  std::atomic<uint32_t> milestones{0}, native_protocol_value{0};
  uint64_t operation = 0;
};
struct ProfileRosterPublicationCell::State {
  std::shared_ptr<ProfileRosterAuthority> authority; // revocable metadata
  ProfileRosterAuthority::Envelope* old_envelope = nullptr;
  ProfileRosterAuthority::Envelope* new_envelope = nullptr;
  ProfileRosterAuthority::Envelope* canceled_empty_envelope = nullptr;
  std::array<ProfileGpdModelAuthority::RosterPause*,4> unchanged_model_pauses{};
  std::shared_ptr<ProfileRosterIntent::State> intent;
  std::atomic<bool> armed{false};
};
struct ProfileRosterNativeBinding::State {
  std::weak_ptr<ProfileRosterAuthority> authority;
  uint64_t nonce = 0;
};
struct ProfileRosterNativeCompletion::State {
  enum class Role:uint8_t {kProtocol,kDataHandoff,kJoined};
  Role role=Role::kJoined;
  uint32_t native_value=0;
  std::weak_ptr<ProfileRosterAuthority> authority;
  std::shared_ptr<const ProfileRosterIntent::State> intent; // metadata; prevents stale-address reuse
  const ProfileRosterNativeBinding* binding = nullptr;
  uint64_t nonce = 0;
};
struct ProfileRosterNativeReadTicket::State {
  std::shared_ptr<ProfileRosterAuthority> authority; // metadata after PM close
  ProfileRosterAuthority::Envelope* fixed = nullptr;
  ProfileRosterAuthority::Envelope* writer_old = nullptr;
  ProfileRosterAuthority::Envelope* writer_new = nullptr;
  ProfileRosterAuthority::Envelope* writer_empty = nullptr;
  const ProfileRosterNativeBinding* binding = nullptr;
  uint64_t operation = 0;
  bool following = false;
  std::atomic<bool> open{true};
};
struct ProfileRosterNativeReadScope::State {
  std::shared_ptr<ProfileRosterNativeReadTicket> ticket;
  std::shared_ptr<const ProfileRosterNativeBinding> binding;
  KernelState* kernel = nullptr;
  XThread* thread = nullptr;
  cpu::ThreadState* cpu = nullptr;
  void* previous = nullptr;
  enum class Kind : uint8_t { kTask, kSystemThreadFrame };
  Kind kind = Kind::kTask;
  uint32_t task = 0, root_hmodule = 0;
  uint32_t entry_sp = 0, entry_lr = 0, function = 0;
  uint64_t task_birth = 0, work_generation = 0, root_generation = 0;
  bool entered = false;
};
struct ProfileRosterAuthority::Operation {
  std::shared_ptr<ProfileRosterIntent> intent;
  std::shared_ptr<Envelope> before, after, canceled_empty;
  ProfileRosterDesired desired;
  std::array<bool, 4> staged_mount{}, old_mount_released{}, unused_mount_released{};
  std::array<bool,4> reserved_slots{};
  std::atomic<bool> settling{false};
  std::array<std::unique_ptr<ProfileAccountIdentity::RosterPause>, 8> account_pauses;
  std::array<std::unique_ptr<ProfileGpdModelAuthority::RosterPause>, 4> model_pauses;
  std::vector<std::weak_ptr<ProfileRosterNativeReadTicket>> tickets;
  std::shared_ptr<ProfileRosterPublicationCell> cell;
  // These complete map nodes are allocated before native arm. Settlement only
  // swaps same-allocator containers; displaced native owners release later.
  std::map<uint8_t, std::shared_ptr<UserProfile>> profiles;
  std::map<uint64_t, std::shared_ptr<ProfileAccountIdentity>> identities;
  std::array<decltype(identities)::node_type, 4> displaced_identities;
  std::map<uint64_t, std::shared_ptr<const ProfileAccountSnapshot>> snapshots;
  std::map<uint64_t, std::shared_ptr<const ProfileAccountMetadata>> metadata;
  std::map<uint64_t, X_XAMACCOUNTINFO> accounts;
  std::map<uint64_t, uint64_t> generations;
  std::map<uint64_t, std::shared_ptr<ProfileGpdModelAuthority>> models;
  // Empty destinations are constructed before arm too. Requested maps retain
  // their genuine resources even when the native canceled branch selects none.
  decltype(profiles) empty_profiles;
  decltype(snapshots) empty_snapshots;
  decltype(generations) empty_generations;
  decltype(models) empty_models;
  bool maps_settled = false, effects_applied = false, old_owners_retired = false;
  // Exact per-roster host effects, never an ambient ordinary-writer bypass.
  // Commands and their real selected writer survive Busy/durable-pending IO.
  std::array<bool,4> removed{},added{},title_prepared{},title_done{};
  std::array<ProfileGpdWriter,4> title_writers;
  std::array<std::optional<ProfileGpdMutation>,4> title_effects;
};
namespace {
bool BeforeCurrent(const ProfileRosterAuthority::Operation& op) {
  for (uint8_t slot = 0; slot != 4; ++slot) {
    const auto& row = op.before->rows[slot];
    if (!row.profile) { continue; }
    if (op.intent->result().changed[slot] && row.storage && row.storage->retiring()) {
      // Actual source retirement closes every storage producer. Removing that
      // lost row needs no fabricated Account read or source reacquisition.
      if (!row.identity || !row.identity->writes_drained()) { return false; }
      continue;
    }
    if (!row.identity || row.identity->retired() || !row.storage || row.storage->retiring()) { return false; }
  }
  return true;
}
}
namespace {
bool NativeProfileCaller(NativeProfileQuery& d) noexcept {
  auto* t=ActualRosterThread();
  if(!t || t->native_frame_thread_identity().process_type!=X_PROCTYPE_TITLE)return false;
  d.thread=t->thread_id();d.pcr=t->pcr_ptr();
  if(const auto* c=t->thread_state()->context()){d.lr=c->lr;d.sp=c->r[1];}
  return true;
}
uint32_t NativeProfileBoundary(const char* b) noexcept {
  return b && !std::strcmp(b,"ForegroundReady")?3:b && !std::strcmp(b,"Bootstrap")?1:2;
}
}
void ProfileManager::RecordNativeProfileQuery(uint32_t ordinal,uint32_t slot,
    uint32_t status,uint64_t value,uint32_t flags) const noexcept {
  // Preserve the diagnostic but skip caller/SP/LR capture when disabled.
  if(!cvars::native_system_ui_trace)return;
  auto* a=roster_authority_.get();
  if(!a || !a->diagnostic_enabled.load(std::memory_order_acquire))return;
  NativeProfileQuery d;d.ordinal=ordinal;d.slot=slot;d.status=status;d.value=value;d.flags=flags;
  if(!NativeProfileCaller(d))return;
  d.ordinary=a->ordinary.load(std::memory_order_acquire);d.open=a->open.load(std::memory_order_acquire);
  if(ordinal==0x210 || ordinal==0x20A)a->first_query[ordinal==0x210?0:1][slot<4?slot:4].Record(d);
}
void ProfileManager::RecordNativeSigninUi(uint32_t user,uint32_t users,uint32_t flags,
    uint32_t overlap,uint32_t result,uint32_t route,uint32_t stage,uint32_t native_owned) const noexcept {
  if(!cvars::native_system_ui_trace)return; // Pure diagnostic capture only.
  auto* a=roster_authority_.get();
  if(!a || !a->diagnostic_enabled.load(std::memory_order_acquire))return;
  NativeSigninUiObservation d;
  if(!NativeProfileCaller(d.caller))return;
  d.user=user;d.users=users;d.flags=flags;d.overlap=overlap;d.result=result;
  d.route=route;d.stage=stage;d.native_owned=native_owned;
  d.caller.ordinary=a->ordinary.load(std::memory_order_acquire);
  d.caller.open=a->open.load(std::memory_order_acquire);a->first_signin_ui.Record(d);
}
std::shared_ptr<ProfileManager::NativeCreationDiagnosticSink>
ProfileManager::CreateNativeCreationDiagnosticSink() {
  auto sink=std::make_shared<NativeCreationDiagnosticSink>();
  static std::atomic<uint64_t> next{1};
  static_assert(NativeOriginalLogonDiagnostic::kStageCount>=5 && NativeOriginalLogonDiagnostic::kStageCount<32);
  const uint64_t limit=UINT64_MAX>>NativeOriginalLogonDiagnostic::kStageCount;
  auto serial=next.load(std::memory_order_relaxed);
  while(serial<limit && !next.compare_exchange_weak(serial,serial+1,std::memory_order_relaxed)){}
  if(serial<limit)sink->original_generation=serial;
  else sink->original_exhausted.store(true,std::memory_order_relaxed);
  return sink;
}
void ProfileManager::RecordNativeProfileCreation(
    const std::shared_ptr<NativeCreationDiagnosticSink>& sink,
    const NativeCreationDiagnostic& value) noexcept {
  // This path can run after a counted PM Use has dropped or after a failed
  // admission. Its saved scalar sink is the ONLY object dereferenced here.
  if(!sink || !cvars::native_system_ui_trace || value.operation<1 || value.operation>4)return;
  auto& cells=sink->first[value.operation-1];
  cells[0].Record(value);
  if(value.failure)cells[1].Record(value);
  // Pending234 has no terminal result. A genuine consumed result is captured
  // separately, so an earlier normal pending poll cannot hide terminal success.
  if(value.operation==uint32_t(NativeCreationDiagnostic::Operation::kConsumer234) &&
      value.result_available && value.return_available)sink->consumer_terminal.Record(value);
}
NativeOriginalLogonObservation ProfileManager::BeginNativeOriginalLogon(
    const std::shared_ptr<NativeCreationDiagnosticSink>& sink,uint32_t ordinal,
    uint32_t flags,bool overlap) noexcept {
  NativeOriginalLogonObservation token;
  if(!sink || !sink->original_generation || !cvars::native_system_ui_trace || (ordinal!=0x20B && ordinal!=0x224))return token;
  auto slot=sink->original_next.load(std::memory_order_relaxed);
  do {
    if(slot>=sink->original.size()){sink->original_exhausted.store(true,std::memory_order_release);return token;}
  }while(!sink->original_next.compare_exchange_weak(slot,slot+1,std::memory_order_acq_rel));
  token.sink_=sink;token.slot_=slot;token.ordinal_=ordinal;token.flags_=flags;token.overlap_=overlap;
  NativeOriginalLogonDiagnostic value;value.stage=NativeOriginalLogonDiagnostic::kEntry;
  RecordNativeOriginalLogon(token,value);return token;
}
void ProfileManager::RecordNativeOriginalLogon(const NativeOriginalLogonObservation& token,
    NativeOriginalLogonDiagnostic value) noexcept {
  if(!token.active() || !cvars::native_system_ui_trace || value.stage>=NativeOriginalLogonDiagnostic::kStageCount)return;
  value.journal=token.sink_->original_generation;
  value.ordinal=token.ordinal_;value.flags=token.flags_;value.overlap_present=token.overlap_;
  auto& cell=token.sink_->original[token.slot_][value.stage];
  uint32_t empty=0;
  if(!cell.state.compare_exchange_strong(empty,1,std::memory_order_relaxed))return;
  // At most8*stageCount winners; this observation sequence cannot wrap.
  // It orders scalar capture cuts, not guest scheduling or APC delivery.
  value.capture_sequence=token.sink_->original_sequence.fetch_add(1,std::memory_order_relaxed);
  cell.payload=value;cell.state.store(2,std::memory_order_release);
}
bool ProfileManager::CopyNativeOriginalLogon(const NativeOriginalLogonObservation& token,
    uint32_t stage,NativeOriginalLogonDiagnostic& value,uint32_t* observed) noexcept {
  if(!token.active() || stage>=NativeOriginalLogonDiagnostic::kStageCount){if(observed)*observed=0;return false;}
  return CopyNativeOriginalLogon(token.sink_,token.slot_+1,stage,value,observed);
}
bool ProfileManager::CopyNativeOriginalLogon(const std::shared_ptr<NativeCreationDiagnosticSink>& sink,
    uint32_t attempt,uint32_t stage,NativeOriginalLogonDiagnostic& value,uint32_t* observed) noexcept {
  if(!sink || !attempt || attempt>sink->original.size() || stage>=NativeOriginalLogonDiagnostic::kStageCount){if(observed)*observed=0;return false;}
  return sink->original[attempt-1][stage].Copy(value,observed);
}

void ProfileManager::ReportNativeProfileDiagnostics(const char* boundary) const noexcept {
  // Disabled diagnostics must not capture or format roster/caller payloads.
  if(!cvars::native_system_ui_trace)return;
  if(XThread::GetCurrentFiberThread() || global_critical_region::is_held_by_current_thread())return;
  try {
    const auto a=roster_authority_;
    if(!a || !a->diagnostic_enabled.load(std::memory_order_acquire))return;
    const uint64_t current=a->diagnostic_current_epoch.load(std::memory_order_acquire),
        selected=a->diagnostic_selected_epoch.load(std::memory_order_acquire),
        intent=a->diagnostic_intent.load(std::memory_order_acquire),
        operation=a->diagnostic_operation.load(std::memory_order_acquire);
    const uint32_t phase=a->diagnostic_phase.load(std::memory_order_acquire),
        available=a->diagnostic_available.load(std::memory_order_acquire);
    const bool pending=a->diagnostic_pending.load(std::memory_order_acquire);
    std::array<uint64_t,4> current_xuid{},selected_xuid{};
    const bool ordinary=a->ordinary.load(std::memory_order_acquire),open=a->open.load(std::memory_order_acquire);
    for(size_t i=0;i<4;++i){
      current_xuid[i]=a->diagnostic_current_xuid[i].load(std::memory_order_acquire);
      selected_xuid[i]=a->diagnostic_selected_xuid[i].load(std::memory_order_acquire);
    }
    NativeSigninUiObservation ui;uint32_t uis=0;const bool uia=a->first_signin_ui.Copy(ui,&uis);
    const uint32_t cut=NativeProfileBoundary(boundary);
    // Capture may have happened under inherited GCRC or a PM reservation.
    // This existing reporter has already refused GCRC/fiber entry; no capture
    // logs, waits, rereads guest memory, or acquires any extra lock.
    const auto creation=creation_diagnostics_;
    static std::array<std::array<NativeSystemUiTraceLimiter,3>,4> creation_trace;
    for(size_t op=0;op<4;++op)for(size_t kind=0;kind<3;++kind){
      NativeCreationDiagnostic d;
      const auto* cell=!creation?nullptr:kind<2?&creation->first[op][kind]:
          op==2?&creation->consumer_terminal:nullptr;
      if(!cell || !cell->Copy(d))continue;
      creation_trace[op][kind].EmitTransition(
          (uint64_t(d.reason)<<32)^d.return_value^d.result_status,
          "profile","creation_operation_result",
          {{"boundary",cut},{"operation",d.operation},{"sample_kind",kind},
           {"reason",d.reason},{"failure",d.failure},
           {"flags_available",d.flags_available},{"flags",d.flags},
           {"return_available",d.return_available},{"return_value",d.return_value},
           {"result_available",d.result_available},{"result_status",d.result_status},
           {"published",d.published},{"discovered",d.discovered},{"signed_in",d.signed_in},
           {"std_error_available",d.error_available},{"std_error_category",d.error_category},
           {"std_error_value",d.error_value}});
    }
    // Original-export attempt cells are independent from first-title queries
    // and bootstrap alignment. Never read an in-progress payload or wait here.
    static std::array<std::array<NativeSystemUiTraceLimiter,
        NativeOriginalLogonDiagnostic::kStageCount>,8> logon_trace;
    static NativeSystemUiTraceLimiter logon_capacity;
    static std::array<NativeSystemUiTraceLimiter,8> logon_progress;
    if(creation){
      const auto reserved=std::min<uint32_t>(creation->original_next.load(std::memory_order_acquire),8);
      for(size_t slot=0;slot<reserved;++slot){
        uint32_t captured=0,in_progress=0;
        for(size_t stage=0;stage<NativeOriginalLogonDiagnostic::kStageCount;++stage){
          NativeOriginalLogonDiagnostic d;uint32_t state=0;
          if(!creation->original[slot][stage].Copy(d,&state)){
            if(state==1)in_progress|=1u<<stage;
            continue;
          }
          captured|=1u<<stage;
          logon_trace[slot][stage].EmitTransition(d.journal,"profile","original_logon_attempt",
            {{"boundary",cut},{"journal",d.journal},{"capture_sequence",d.capture_sequence},{"attempt",slot+1},{"ordinal",d.ordinal},{"flags",d.flags},
             {"overlap_present",d.overlap_present},{"stage",d.stage},{"reason",d.reason},{"line",d.line},
             {"disposition_available",d.disposition_available},{"disposition",d.disposition},
             {"result_available",d.result_available},{"result",d.result},
             {"error_available",d.error_available},{"error_category",d.error_category},{"error_value",d.error_value},
             {"evaluated",d.evaluated},{"failed",d.failed},
             {"expected_mask_available",d.expected_mask_available},{"expected_nonempty_mask",d.expected_nonempty_mask},{"expected_nonempty_count",d.expected_nonempty_count},
             {"input_mask_available",d.input_mask_available},{"input_nonempty_mask",d.input_nonempty_mask},{"input_nonempty_count",d.input_nonempty_count},
             {"desired_mask_available",d.desired_mask_available},{"desired_nonempty_mask",d.desired_nonempty_mask},{"desired_nonempty_count",d.desired_nonempty_count},
             {"selected_mask",d.selected_mask},{"remove_mask",d.remove_mask},
             {"latest_available",d.latest_available},{"latest_present",d.latest_present},{"latest_kind",d.latest_kind&0xFu},
             {"latest_function_role",(d.latest_kind>>4)&0xFFu},
             {"latest_caller_available",((d.latest_kind>>12)&3u)!=0},
             {"latest_same_caller",((d.latest_kind>>12)&3u)==1},
             {"latest_sp_relation",(d.latest_kind>>14)&3u},
             {"reentrant_conflict_category",(d.latest_kind>>16)&0xFFu},
             {"reentrant_conflict_function_role",(d.latest_kind>>24)&0xFFu},
             {"latest_read_present",d.latest_read_present},{"latest_borrowed_read_present",d.latest_borrowed_read_present},
             {"latest_same_source",d.latest_same_source},{"entered",d.entered},{"returned",d.returned},
             {"phase_available",d.phase_available},{"phase",d.phase},{"maps_committed",d.maps_committed},
             {"writers_available",d.writers_available},{"completion_error_available",d.completion_error_available},
             {"completion_error",d.completion_error},{"completion_extended",d.completion_extended},{"completion_length",d.completion_length}});
        }
        logon_progress[slot].EmitTransition((creation->original_generation<<NativeOriginalLogonDiagnostic::kStageCount)|captured,
            "profile","original_logon_progress",
            {{"journal",creation->original_generation},{"attempt",slot+1},
             {"captured_stage_mask",captured},{"in_progress_stage_mask",in_progress}});
      }
      const bool exhausted=creation->original_exhausted.load(std::memory_order_acquire);
      logon_capacity.EmitTransition((creation->original_generation<<5)|(uint64_t(reserved)<<1)|exhausted,"profile","original_logon_capacity",
          {{"journal",creation->original_generation},{"reserved_attempts",reserved},{"exhausted",exhausted},{"capacity",8}});
    }
    static NativeSystemUiTraceLimiter state_trace,ui_trace;
    static std::array<NativeSystemUiTraceLimiter,4> projection_trace;
    static std::array<std::array<NativeSystemUiTraceLimiter,5>,2> query_trace;
    state_trace.EmitTransition((uint64_t(ordinary)<<63)^selected^intent^(uint64_t(phase)<<48),"profile","native_projection_state",
      {{"boundary",cut},{"available",available},{"open",open},{"ordinary",ordinary},{"current_epoch",current},{"selected_epoch",selected},
       {"intent",intent},{"operation",operation},{"phase",phase},{"pending",pending}},
      {{"current0",current_xuid[0]},{"current1",current_xuid[1]},{"current2",current_xuid[2]},{"current3",current_xuid[3]},
       {"selected0",selected_xuid[0]},{"selected1",selected_xuid[1]},{"selected2",selected_xuid[2]},{"selected3",selected_xuid[3]}});
    for(size_t slot=0;slot<4;++slot){
    NativeProfileProjection p;uint32_t ps=0;const bool pa=a->first_projection[slot].Copy(p,&ps);
    projection_trace[slot].EmitTransition(pa,"profile","first_title_projection",
      {{"boundary",cut},{"complete",pa},{"capture_state",ps},{"available",p.available},{"reason",p.reason},{"bucket",slot},{"slot",p.slot},{"thread",p.thread},{"pcr",p.pcr},
       {"lr",p.lr},{"sp",p.sp},{"ordinary",p.ordinary},{"scoped",p.scoped},{"following",p.following},{"profile_present",p.profile_present}},
      {{"current_epoch",p.current_epoch},{"selected_epoch",p.selected_epoch},{"selected_xuid",p.selected_xuid},{"intent",p.intent},{"operation",p.operation},{"phase",p.phase}});
    }
    for(size_t index=0;index<2;++index)for(size_t slot=0;slot<5;++slot){
      NativeProfileQuery q;const auto& cell=a->first_query[index][slot];uint32_t qs=0;const bool complete=cell.Copy(q,&qs);
      query_trace[index][slot].EmitTransition(complete,"profile","first_title_query",
      {{"boundary",cut},{"complete",complete},{"capture_state",qs},{"ordinal",index?0x20Au:0x210u},{"bucket",slot},{"slot",q.slot},
       {"status",q.status},{"value",q.value},{"flags",q.flags},{"thread",q.thread},{"pcr",q.pcr},
       {"lr",q.lr},{"sp",q.sp},{"ordinary",q.ordinary},{"open",q.open}});
    }
    ui_trace.EmitTransition(uia,"profile","first_title_signin_ui",
      {{"boundary",cut},{"complete",uia},{"capture_state",uis},{"user",ui.user},{"users",ui.users},{"flags",ui.flags},{"overlap",ui.overlap},{"result",ui.result},
       {"route",ui.route},{"stage",ui.stage},{"native_owned",ui.native_owned},{"thread",ui.caller.thread},{"pcr",ui.caller.pcr},
       {"lr",ui.caller.lr},{"sp",ui.caller.sp},{"ordinary",ui.caller.ordinary},{"open",ui.caller.open}});
  }catch(...){/* Advisory only. No admission, result, or lifetime changes. */}
}
HostRoster::HostRoster(std::shared_ptr<const State> s) : state_(std::move(s)) {}
const std::array<ProfileRosterSlot, 4>& HostRoster::slots() const { return state_->slots; }
uint64_t HostRoster::epoch() const { return state_->epoch; }
ProfileRosterIntent::ProfileRosterIntent(std::shared_ptr<State> s) : state_(std::move(s)) {}
std::shared_ptr<const HostRoster> ProfileRosterIntent::old_roster() const { return state_->old_roster; }
std::shared_ptr<const HostRoster> ProfileRosterIntent::prepared_roster() const { return state_->new_roster; }
ProfileRosterResult ProfileRosterIntent::result() const {
  // Published release follows outcome choice, so a reported Published phase
  // can never be combined with the earlier requested-only result.
  const auto phase=state_->phase.load(std::memory_order_acquire);
  auto out = state_->published_choice.load(std::memory_order_acquire)==2
      ? state_->canceled_empty_result : state_->result;
  out.phase = phase;
  const auto duties=state_->milestones.load(std::memory_order_acquire);
  out.native_protocol_returned=bool(duties&State::kProtocol);
  out.selected_reads_available=bool(duties&State::kRead);
  out.maps_committed=bool(duties&State::kMaps);
  out.writers_available=bool(duties&State::kWriters);
  out.owners_retired=bool(duties&State::kRetired);
  out.native_protocol_value=state_->native_protocol_value.load(std::memory_order_acquire);
  return out;
}
bool ProfileRosterIntent::data_handoff_complete() const noexcept {
  return bool(state_->milestones.load(std::memory_order_acquire)&State::kDataHandoff);
}
std::optional<ProfileRosterPublicationOutcome> ProfileRosterIntent::publication_outcome() const {
  const auto choice=state_->published_choice.load(std::memory_order_acquire);
  if(choice==1)return ProfileRosterPublicationOutcome::kRequested;
  if(choice==2)return ProfileRosterPublicationOutcome::kCanceledEmpty;
  return {};
}
std::shared_ptr<const HostRoster> ProfileRosterIntent::published_roster() const {
  const auto choice=publication_outcome();
  if(!choice)return {};
  return *choice==ProfileRosterPublicationOutcome::kRequested
      ? state_->new_roster : state_->canceled_empty_roster;
}
bool ProfileRosterIntent::HasPublishedEmptyUnchangedRows(uint32_t actual_protocol_value) const {
  return HasPublishedRequestedRows(actual_protocol_value) && state_->empty_unchanged_rows;
}
bool ProfileRosterIntent::HasPublishedUnchangedOwners(uint32_t actual_protocol_value) const {
  return HasPublishedRequestedRows(actual_protocol_value) && state_->unchanged_owners;
}
bool ProfileRosterIntent::HasPublishedRequestedRows(uint32_t actual_protocol_value) const {
  const auto observed = result();
  // This is an immutable publication/protocol fact for this exact intent.
  // Finalizing may precede its SAME still-current job's hidden frame. It is
  // not global-current permission: Owner must still bind exact Work/source/
  // epoch and all provider/frame checks, suppressing prior presentation when
  // a successor takes over. Committed/Uncertain and canceled outcomes refuse.
  return (observed.phase==ProfileRosterPhase::kPublished ||
          observed.phase==ProfileRosterPhase::kFinalizing) && !observed.error &&
      observed.native_protocol_returned && observed.selected_reads_available &&
      observed.maps_committed && observed.writers_available &&
      observed.native_protocol_value == actual_protocol_value &&
      publication_outcome()==ProfileRosterPublicationOutcome::kRequested;
}
const std::array<uint8_t, 4 * 0x188>& ProfileRosterIntent::NativeRowsForPrepared() const {
  return state_->native_rows;
}
ProfileRosterPublicationCell::ProfileRosterPublicationCell(std::shared_ptr<State> s)
    : state_(std::move(s)) {}
bool ProfileRosterPublicationCell::PublishFromNative(
    ProfileRosterPublicationOutcome outcome) noexcept {
  auto* s = state_.get();
  if (!s || !s->armed.load(std::memory_order_acquire) ||
      !s->authority->open.load(std::memory_order_acquire)) { return false; }
  // Exact actual native issuer/frame/Task validation precedes this call. No
  // resource owner is copied/destroyed at this guest publication instruction.
  auto* selected=outcome==ProfileRosterPublicationOutcome::kRequested?s->new_envelope:
      outcome==ProfileRosterPublicationOutcome::kCanceledEmpty?s->canceled_empty_envelope:nullptr;
  if(!selected)return false;
  auto expected = s->old_envelope;
  if (!s->authority->selected.compare_exchange_strong(expected, selected,
          std::memory_order_release, std::memory_order_relaxed)) { return false; }
  s->armed.store(false, std::memory_order_release);
  s->intent->published_choice.store(outcome==ProfileRosterPublicationOutcome::kRequested?1u:2u,
      std::memory_order_release);
  if(outcome==ProfileRosterPublicationOutcome::kRequested)
    for(auto* pause:s->unchanged_model_pauses)if(pause)pause->UnpauseKnownOutcomeFromNative();
  s->intent->phase.store(ProfileRosterPhase::kPublished, std::memory_order_release);
  s->authority->diagnostic_phase.store(uint32_t(ProfileRosterPhase::kPublished),std::memory_order_release);
  s->authority->DiagnosticSelected(selected);
  return true;
}
ProfileRosterNativeBinding::ProfileRosterNativeBinding(std::shared_ptr<State> s)
    : state_(std::move(s)) {}
uint64_t ProfileRosterNativeBinding::nonce() const noexcept { return state_->nonce; }
ProfileRosterNativeCompletion::ProfileRosterNativeCompletion(std::shared_ptr<const State> s)
    : state_(std::move(s)) {}
std::shared_ptr<const ProfileRosterNativeCompletion> ProfileRosterNativeCompletion::ForProtocol(
    const std::shared_ptr<ProfileRosterIntent>& intent,
    const std::shared_ptr<const ProfileRosterNativeBinding>& binding,
    uint64_t nonce, uint32_t actual_native_value) {
  if (!intent || !binding || !nonce || binding->nonce() != nonce ||
      intent->state_->phase.load(std::memory_order_acquire) != ProfileRosterPhase::kPublished) { return {}; }
  const auto authority = intent->state_->authority.lock();
  if (!authority || !authority->open.load(std::memory_order_acquire) ||
      authority->binding != binding || binding->state_->authority.lock() != authority) { return {}; }
  auto state = std::make_shared<State>();
  state->authority = authority; state->intent = intent->state_;
  state->binding = binding.get(); state->nonce = nonce;
  state->role = State::Role::kProtocol; state->native_value = actual_native_value;
  return std::shared_ptr<const ProfileRosterNativeCompletion>(new ProfileRosterNativeCompletion(std::move(state)));
}
std::shared_ptr<const ProfileRosterNativeCompletion> ProfileRosterNativeCompletion::ForDataHandoff(
    const std::shared_ptr<ProfileRosterIntent>& intent,
    const std::shared_ptr<const ProfileRosterNativeBinding>& binding, uint64_t nonce) {
  if (!intent || !binding || !nonce || binding->nonce()!=nonce ||
      (intent->state_->phase.load(std::memory_order_acquire)!=ProfileRosterPhase::kPublished &&
       intent->state_->phase.load(std::memory_order_acquire)!=ProfileRosterPhase::kFinalizing) ||
      !(intent->state_->milestones.load(std::memory_order_acquire)&ProfileRosterIntent::State::kProtocol)) { return {}; }
  const auto authority=intent->state_->authority.lock();
  if (!authority || !authority->open.load(std::memory_order_acquire) ||
      authority->binding!=binding || binding->state_->authority.lock()!=authority) { return {}; }
  auto state=std::make_shared<State>();
  state->authority=authority;state->intent=intent->state_;
  state->binding=binding.get();state->nonce=nonce;state->role=State::Role::kDataHandoff;
  return std::shared_ptr<const ProfileRosterNativeCompletion>(new ProfileRosterNativeCompletion(std::move(state)));
}
std::shared_ptr<const ProfileRosterNativeCompletion> ProfileRosterNativeCompletion::ForJoined(
    const std::shared_ptr<ProfileRosterIntent>& intent,
    const std::shared_ptr<const ProfileRosterNativeBinding>& binding, uint64_t nonce) {
  if (!intent || !binding || !nonce || binding->nonce() != nonce ||
      (intent->state_->phase.load(std::memory_order_acquire) != ProfileRosterPhase::kPublished &&
       intent->state_->phase.load(std::memory_order_acquire) != ProfileRosterPhase::kFinalizing) ||
      !(intent->state_->milestones.load(std::memory_order_acquire) & ProfileRosterIntent::State::kProtocol)) { return {}; }
  const auto authority = intent->state_->authority.lock();
  if (!authority || !authority->open.load(std::memory_order_acquire) ||
      authority->binding != binding || binding->state_->authority.lock() != authority) { return {}; }
  auto s = std::make_shared<State>();
  s->authority = authority; s->intent = intent->state_;
  s->binding = binding.get(); s->nonce = nonce;
  return std::shared_ptr<const ProfileRosterNativeCompletion>(new ProfileRosterNativeCompletion(std::move(s)));
}
ProfileRosterNativeReadTicket::ProfileRosterNativeReadTicket(std::shared_ptr<State> s)
    : state_(std::move(s)) {}
std::shared_ptr<ProfileRosterNativeReadTicket> ProfileRosterIntent::NativeReadTicket(
    bool following, const ProfileRosterNativeBinding& binding, std::error_code* error) {
  if (error) { error->clear(); }
  const auto fail = [&](std::errc reason) -> std::shared_ptr<ProfileRosterNativeReadTicket> {
    if (error) { *error = std::make_error_code(reason); }
    return {};
  };
  const auto authority = state_->authority.lock();
  if (!authority || !authority->open.load(std::memory_order_acquire)) { return fail(std::errc::operation_canceled); }
  auto entered = authority->gate->Acquire();
  if (!entered) { return fail(std::errc::operation_canceled); }
  std::unique_lock lock(entered->owner().profile_mount_mutex_, std::try_to_lock);
  if (!lock.owns_lock()) { return fail(std::errc::device_or_resource_busy); }
  if (!authority->pending ||
      authority->pending->intent.get() != this || authority->binding.get() != &binding ||
      state_->phase.load(std::memory_order_acquire) != ProfileRosterPhase::kPrepared) { return fail(std::errc::operation_canceled); }
  auto s = std::make_shared<ProfileRosterNativeReadTicket::State>();
  s->authority = authority; s->binding = &binding;
  s->operation = state_->operation; s->following = following;
  s->fixed = authority->pending->before.get();
  s->writer_old = s->fixed; s->writer_new = authority->pending->after.get();
  s->writer_empty = authority->pending->canceled_empty.get();
  auto ticket = std::shared_ptr<ProfileRosterNativeReadTicket>(new ProfileRosterNativeReadTicket(std::move(s)));
  if (authority->tickets.size() >= 4096) { return fail(std::errc::no_buffer_space); }
  authority->tickets.push_back(ticket);
  authority->pending->tickets.push_back(ticket);
  return ticket;
}
ProfileRosterNativeReadScope::ProfileRosterNativeReadScope(std::unique_ptr<State> s)
    : state_(std::move(s)) {}
ProfileRosterNativeReadScope::~ProfileRosterNativeReadScope() {
  // An entered wrong-thread scope must be retained by its native work owner;
  // destructing it would leave an actual ThreadState pointing at dead memory.
  assert_true(!state_->entered);
}
std::unique_ptr<ProfileRosterNativeReadScope> ProfileRosterNativeReadScope::Prepare(
    std::shared_ptr<ProfileRosterNativeReadTicket> ticket,
    std::shared_ptr<const ProfileRosterNativeBinding> binding) {
  if (!ticket || !binding || ticket->state_->binding != binding.get() ||
      !ticket->state_->open.load(std::memory_order_acquire) ||
      !ticket->state_->authority->open.load(std::memory_order_acquire)) { return {}; }
  auto s = std::make_unique<State>(); s->ticket = std::move(ticket); s->binding = std::move(binding);
  return std::unique_ptr<ProfileRosterNativeReadScope>(new ProfileRosterNativeReadScope(std::move(s)));
}
bool ProfileRosterNativeReadScope::Enter(KernelState* kernel, XThread* thread,
    cpu::ThreadState* cpu, uint32_t task, uint64_t birth, uint64_t work,
    uint32_t root, uint64_t root_generation) noexcept {
  auto& s = *state_;
  auto& t = *s.ticket->state_;
  if (s.entered || !kernel || !thread || !cpu || !task || !birth || !work ||
      !root || !root_generation || ActualRosterThread() != thread ||
      thread->kernel_state() != kernel || thread->thread_state() != cpu ||
      t.authority->kernel != kernel || !t.open.load(std::memory_order_acquire) ||
      !t.authority->EnterScope()) { return false; }
  if (!t.open.load(std::memory_order_acquire)) { t.authority->LeaveScope(); return false; }
  s.previous = cpu->native_profile_roster_read_.load(std::memory_order_acquire);
  // Nested actual work can borrow only a scope from this same exact issuer.
  if (s.previous) {
    auto* parent = static_cast<ProfileRosterNativeReadScope*>(s.previous);
    if (!parent->state_->entered || parent->state_->kernel != kernel ||
        parent->state_->binding != s.binding || parent->state_->cpu != cpu) {
      t.authority->LeaveScope(); return false;
    }
  }
  s.kind = State::Kind::kTask;
  s.kernel = kernel; s.thread = thread; s.cpu = cpu; s.task = task;
  s.task_birth = birth; s.work_generation = work; s.root_hmodule = root;
  s.root_generation = root_generation; s.entered = true;
  cpu->native_profile_roster_read_.store(this, std::memory_order_release);
  return true;
}
bool ProfileRosterNativeReadScope::EnterSystemThreadFrame(
    KernelState* kernel, XThread* thread, cpu::ThreadState* cpu,
    uint32_t entry_sp, uint32_t entry_lr, uint32_t function,
    uint64_t work, uint32_t root, uint64_t root_generation) noexcept {
  auto& s = *state_;
  auto& t = *s.ticket->state_;
  // All module/domain/code leases remain in the earlier native whole owner.
  // This scalar-only cut joins only the same actual read stack and count.
  if (s.entered || !kernel || !thread || !cpu || !work || !root ||
      !root_generation || !entry_sp || (entry_sp & 15) ||
      entry_lr < 4 || (entry_lr & 3) || function < 4 || (function & 3) ||
      ActualRosterThread() != thread || thread->kernel_state() != kernel ||
      thread->thread_state() != cpu || !cpu->context() ||
      cpu->context()->thread_state != cpu || cpu->context()->kernel_state != kernel ||
      cpu->context()->r[13] != thread->pcr_ptr() ||
      cpu->context()->r[1] != entry_sp || cpu->context()->lr != entry_lr ||
      thread->native_frame_thread_identity().process_type != X_PROCTYPE_SYSTEM ||
      thread->stack_limit() > entry_sp || entry_sp > thread->stack_base() ||
      t.authority->kernel != kernel || !t.open.load(std::memory_order_acquire) ||
      !t.authority->EnterScope()) { return false; }
  if (!t.open.load(std::memory_order_acquire)) { t.authority->LeaveScope(); return false; }
  s.previous = cpu->native_profile_roster_read_.load(std::memory_order_acquire);
  if (s.previous) {
    auto* parent = static_cast<ProfileRosterNativeReadScope*>(s.previous);
    if (!parent->state_->entered || parent->state_->kernel != kernel ||
        parent->state_->binding != s.binding || parent->state_->cpu != cpu) {
      t.authority->LeaveScope(); return false;
    }
  }
  s.kind = State::Kind::kSystemThreadFrame;
  s.kernel = kernel; s.thread = thread; s.cpu = cpu;
  s.task = 0; s.task_birth = 0;
  s.entry_sp = entry_sp; s.entry_lr = entry_lr; s.function = function;
  s.work_generation = work; s.root_hmodule = root;
  s.root_generation = root_generation; s.entered = true;
  cpu->native_profile_roster_read_.store(this, std::memory_order_release);
  return true;
}
bool ProfileRosterNativeReadScope::Leave() noexcept {
  auto& s = *state_;
  if (!s.entered || ActualRosterThread() != s.thread ||
      s.thread->thread_state() != s.cpu ||
      s.cpu->native_profile_roster_read_.load(std::memory_order_acquire) != this) { return false; }
  s.cpu->native_profile_roster_read_.store(s.previous, std::memory_order_release);
  s.entered = false;
  s.ticket->state_->authority->LeaveScope();
  return true;
}
bool ProfileRosterNativeReadScope::DetachAtActualKernelStop() noexcept {
  auto& s = *state_;
  if (!s.entered) { return true; }
  if (!s.cpu || !s.thread || s.thread->thread_state() != s.cpu) { return false; }
  void* expected = this;
  if (!s.cpu->native_profile_roster_read_.compare_exchange_strong(
          expected, s.previous, std::memory_order_acq_rel)) { return false; }
  s.entered = false;
  s.ticket->state_->authority->LeaveScope();
  return true;
}
const ProfileRosterNativeReadScope* ProfileRosterNativeReadScope::FromActualThread(
    KernelState* kernel, cpu::ThreadState* cpu) noexcept {
  if (!kernel || !cpu) { return nullptr; }
  auto* thread = ActualRosterThread();
  if (!thread || thread->kernel_state() != kernel || thread->thread_state() != cpu) { return nullptr; }
  const auto* scope = static_cast<const ProfileRosterNativeReadScope*>(
      cpu->native_profile_roster_read_.load(std::memory_order_acquire));
  return scope && scope->state_->entered && scope->state_->kernel == kernel &&
      scope->state_->cpu == cpu && scope->state_->thread == thread &&
      scope->state_->ticket->state_->authority->open.load(std::memory_order_acquire)
      ? scope : nullptr;
}

void ProfileManager::InitializeRosterAuthority() {
  auto a = std::make_shared<ProfileRosterAuthority>();
  a->owner = this; a->kernel = kernel_state_; a->gate = account_commit_gate_;
  a->current = std::make_shared<ProfileRosterAuthority::Envelope>();
  a->current->roster.reset(new HostRoster(std::make_shared<HostRoster::State>()));
  a->selected.store(a->current.get(), std::memory_order_release);
  a->DiagnosticCurrent(a->current.get());a->DiagnosticSelected(a->current.get());
  roster_authority_ = std::move(a);
}
void ProfileManager::BeginRosterStop() {
  if (roster_authority_) { roster_authority_->open.store(false, std::memory_order_release); }
}
void ProfileManager::CloseRosterAuthority() {
  const auto a = roster_authority_;
  if (!a) { return; }
  a->open.store(false, std::memory_order_release);
  a->ordinary.store(false, std::memory_order_release);
  // Native owning Service has stopped real CPU/GPU work and released entered
  // ThreadState scopes before this PM teardown. Wait joins only real entries.
  auto n = a->scopes.load(std::memory_order_acquire);
  while (n) { a->scopes.wait(n, std::memory_order_acquire); n = a->scopes.load(std::memory_order_acquire); }
  std::shared_ptr<ProfileRosterAuthority::Envelope> current;
  std::shared_ptr<ProfileRosterAuthority::Operation> pending;
  std::vector<std::shared_ptr<ProfileRosterAuthority::Envelope>> retired;
  std::vector<std::shared_ptr<ProfileRosterAuthority::Operation>> retiring;
  {
    // Scalar readers copy selected->roster under this existing region. Only
    // detach preheld owners here; native/resource retirement and destruction
    // must occur after the region, never while a guest reader can be suspended.
    auto global = xe::global_critical_region::AcquireDirect();
    a->selected.store(nullptr, std::memory_order_release);
    current = std::move(a->current);
    pending = std::move(a->pending);
    retired.swap(a->retired);
    retiring.swap(a->retiring);
    a->DiagnosticSelected(nullptr);
    a->diagnostic_pending.store(false,std::memory_order_release);
  }
  auto retire = [](const auto& envelope) {
    if (!envelope) { return; }
    for (const auto& row : envelope->rows) {
      if (row.identity) { row.identity->Retire(); }
      if (row.model) { row.model->Retire(); }
      if (row.storage) { row.storage->Retire(); }
    }
  };
  retire(current);
  for (const auto& old : retired) { retire(old); }
  if (pending) { retire(pending->before); retire(pending->after); retire(pending->canceled_empty); }
  for (const auto& op:retiring) { retire(op->before);retire(op->after);retire(op->canceled_empty); }
  a->native_owner.reset(); a->binding.reset();
  for (auto& weak : a->tickets) {
    if (auto ticket = weak.lock()) { ticket->state_->open.store(false, std::memory_order_release); }
  }
  a->tickets.clear();
  const auto close_operation=[&](const auto& pending) {
    if (!pending) { return; }
    if (pending->cell) { pending->cell->state_->armed.store(false, std::memory_order_release); }
    for (auto& weak : pending->tickets) {
      if (auto t = weak.lock()) { t->state_->open.store(false, std::memory_order_release); }
    }
    pending->intent->state_->phase.store(ProfileRosterPhase::kUncertain, std::memory_order_release);
    a->diagnostic_phase.store(uint32_t(ProfileRosterPhase::kUncertain),std::memory_order_release);
    for (auto& p : pending->account_pauses) { if (p) { p->ReleaseKnownOutcome(); } }
    for (auto& p : pending->model_pauses) { if (p) { p->ReleaseKnownOutcome(); } }
  };
  close_operation(pending);
  for (const auto& op:retiring) { close_operation(op); }
  // Destruction is outside metadata/namespace/native locks, including for a
  // previously uncertain operation. Escaped handles retain metadata only.
  retired.clear(); current.reset(); pending.reset(); retiring.clear(); a->owner = nullptr;
}
ProfileRosterExpected ProfileManager::CaptureRosterExpected(std::error_code& error) const {
  error.clear(); ProfileRosterExpected out;
  std::unique_lock lock(profile_mount_mutex_, std::try_to_lock);
  const auto a = roster_authority_;
  if (!lock.owns_lock()) { error = Busy(); return out; }
  if (!a || !a->open.load(std::memory_order_acquire) || a->pending ||
      !a->ordinary.load(std::memory_order_acquire)) { error = Stale(); return out; }
  out.roster = a->current->roster;
  auto proof = std::make_shared<ProfileRosterExpected::Proof>();
  proof->authority = a; proof->exact = out.roster.get(); out.proof_ = std::move(proof);
  return out;
}
std::shared_ptr<const HostRoster> ProfileManager::CaptureCurrentUserMetadata() const {
  auto global = xe::global_critical_region::AcquireDirect();
  const auto a = roster_authority_;
  if (!a || !a->open.load(std::memory_order_acquire)) { return {}; }
  const auto* selected = a->selected.load(std::memory_order_acquire);
  // Native publication selects only a preheld immutable envelope. Its final
  // release shares this existing region; no PM/model/data lock or read ticket
  // is needed to copy the metadata shared pointer.
  return selected ? selected->roster : nullptr;
}
std::shared_ptr<const HostRoster> ProfileManager::CaptureSelectedRoster(
    NativeProfileProjection* diagnostic) const {
  const auto a = roster_authority_;
  if (!a || !a->open.load(std::memory_order_acquire)) { if(diagnostic)diagnostic->reason=1; return {}; }
  std::unique_lock lock(profile_mount_mutex_, std::try_to_lock);
  if (!lock.owns_lock()) { if(diagnostic)diagnostic->reason=2; return {}; }
  if(diagnostic){
    diagnostic->available|=1;
    diagnostic->current_epoch=a->current?a->current->roster->epoch():0;
    if(a->pending){diagnostic->intent=reinterpret_cast<uintptr_t>(a->pending->intent.get());
      diagnostic->operation=a->pending->intent->state_->operation;
      diagnostic->phase=uint32_t(a->pending->intent->state_->phase.load(std::memory_order_acquire));}
  }
  ProfileRosterAuthority::Envelope* e = nullptr;
  auto* actual = ActualRosterThread();
  const auto* scope = actual ? ProfileRosterNativeReadScope::FromActualThread(kernel_state_, actual->thread_state()) : nullptr;
  if(diagnostic && actual){
    diagnostic->available|=2;diagnostic->thread=actual->thread_id();diagnostic->pcr=actual->pcr_ptr();
    diagnostic->process=actual->native_frame_thread_identity().process_type;
    if(const auto* c=actual->thread_state()->context()){diagnostic->lr=c->lr;diagnostic->sp=c->r[1];}
  }
  if (scope && scope->state_->ticket->state_->authority == a) {
    const auto& t = *scope->state_->ticket->state_;
    if(diagnostic){diagnostic->scoped=1;diagnostic->following=t.following;}
    e = t.following ? a->selected.load(std::memory_order_acquire) : t.fixed;
    if (t.following && e != t.writer_old && e != t.writer_new && e != t.writer_empty) { if(diagnostic)diagnostic->reason=3; return {}; }
  } else {
    const bool ordinary=a->ordinary.load(std::memory_order_acquire);
    if(diagnostic){diagnostic->ordinary=ordinary;diagnostic->available|=16;}
    if(ordinary || a->selected_read.load(std::memory_order_acquire))
      e = a->selected.load(std::memory_order_acquire);
  }
  if(diagnostic){
    diagnostic->reason=e?0:4;
    if(e){diagnostic->available|=4;diagnostic->selected_epoch=e->roster->epoch();
      if(diagnostic->slot<4)diagnostic->selected_xuid=e->roster->slots()[diagnostic->slot].xuid;}
  }
  return e ? e->roster : nullptr;
}
// These owned captures are one projection of the same actual selected roster.
// The pending/current envelopes remain preheld by the owning PM until native
// work has joined; no container mutation happens at the native selector.
bool ProfileManager::CanonicalRosterAccountsAvailable() const {
  const auto a = roster_authority_;
  if (!a || !a->open.load(std::memory_order_acquire)) { return false; }
  std::unique_lock lock(profile_mount_mutex_, std::try_to_lock);
  return lock.owns_lock() && a->ordinary.load(std::memory_order_acquire) &&
      a->current && a->selected.load(std::memory_order_acquire) == a->current.get() &&
      ProfileRosterAuthority::LogicalCurrent(*a->current);
}
bool ProfileManager::RosterIdentityRebindAvailable(uint64_t xuid) const {
  const auto a = roster_authority_;
  if (!a || !a->open.load(std::memory_order_acquire)) { return false; }
  std::unique_lock lock(profile_mount_mutex_, std::try_to_lock);
  if (!lock.owns_lock()) { return false; }
  if (a->pending && !a->pending->old_owners_retired) {
    for (uint8_t slot = 0; slot != 4; ++slot) {
      if (a->pending->reserved_slots[slot] &&
          a->pending->before->roster->slots()[slot].xuid == xuid) { return false; }
    }
  }
  return true;
}
bool ProfileManager::OrdinaryRosterWriterAvailable(
    const std::shared_ptr<ProfileGpdModelAuthority>& model) const {
  const auto a = roster_authority_;
  if (!model || !a || !a->open.load(std::memory_order_acquire)) { return false; }
  std::unique_lock lock(profile_mount_mutex_, std::try_to_lock);
  if (!lock.owns_lock() || !a->ordinary.load(std::memory_order_acquire) ||
      !a->current || a->selected.load(std::memory_order_acquire) != a->current.get() ||
      !ProfileRosterAuthority::LogicalCurrent(*a->current) ||
      preparing_profiles_.contains(model->xuid()) || retiring_profiles_.contains(model->xuid())) { return false; }
  const auto found = gpd_models_.find(model->xuid());
  if (found == gpd_models_.end() || found->second != model) { return false; }
  return std::any_of(a->current->rows.begin(), a->current->rows.end(),
      [&](const auto& row) { return row.model == model && row.storage == model->storage(); });
}
std::shared_ptr<UserProfile> ProfileManager::CaptureRosterProfile(uint8_t slot) const {
  const auto a = roster_authority_; if (!a || slot >= 4) { return {}; }
  NativeProfileProjection diagnostic;diagnostic.slot=slot;
  // Actual caller scalars are available even if either original try-lock fails.
  NativeProfileQuery caller;
  if(NativeProfileCaller(caller)){
    diagnostic.available|=2;diagnostic.process=X_PROCTYPE_TITLE;
    diagnostic.thread=caller.thread;diagnostic.pcr=caller.pcr;diagnostic.lr=caller.lr;diagnostic.sp=caller.sp;
  }
  const auto record=[&]{
    if(a->diagnostic_enabled.load(std::memory_order_acquire) && diagnostic.process==X_PROCTYPE_TITLE)
      a->first_projection[slot].Record(diagnostic);
  };
  const auto roster = CaptureSelectedRoster(&diagnostic); if (!roster) { record(); return {}; }
  std::unique_lock lock(profile_mount_mutex_, std::try_to_lock); if (!lock.owns_lock()) { diagnostic.reason=5;record();return {}; }
  const ProfileRosterAuthority::Envelope* e = nullptr;
  if (a->current && a->current->roster == roster) { e = a->current.get(); }
  if (a->pending) {
    if (a->pending->before->roster == roster) { e = a->pending->before.get(); }
    if (a->pending->after->roster == roster) { e = a->pending->after.get(); }
    if (a->pending->canceled_empty && a->pending->canceled_empty->roster == roster) { e = a->pending->canceled_empty.get(); }
  }
  for (const auto& op:a->retiring) {
    if (op->before && op->before->roster==roster) { e=op->before.get(); }
    if (op->after && op->after->roster==roster) { e=op->after.get(); }
    if (op->canceled_empty && op->canceled_empty->roster==roster) { e=op->canceled_empty.get(); }
  }
  for (const auto& old : a->retired) { if (old->roster == roster) { e = old.get(); } }
  diagnostic.available|=8;diagnostic.profile_present=e && bool(e->rows[slot].profile);
  if(!diagnostic.profile_present)diagnostic.reason=6;
  record();
  return e ? e->rows[slot].profile : nullptr;
}
std::shared_ptr<ProfileGpdModelAuthority> ProfileManager::CaptureRosterModel(uint64_t xuid) const {
  const auto a = roster_authority_; const auto roster = CaptureSelectedRoster(); if (!a || !roster) { return {}; }
  std::unique_lock lock(profile_mount_mutex_, std::try_to_lock); if (!lock.owns_lock()) { return {}; }
  auto find = [&](const auto& e) -> std::shared_ptr<ProfileGpdModelAuthority> {
    if (e && e->roster == roster) { for (const auto& row : e->rows) { if (row.profile && row.profile->xuid() == xuid) { return row.model; } } }
    return {};
  };
  if (auto v = find(a->current)) { return v; }
  if (a->pending) { if (auto v = find(a->pending->before)) { return v; } if (auto v = find(a->pending->after)) { return v; } if (auto v = find(a->pending->canceled_empty)) { return v; } }
  for (const auto& op:a->retiring) {
    if (auto v=find(op->before)) { return v; }
    if (auto v=find(op->after)) { return v; }
    if (auto v=find(op->canceled_empty)) { return v; }
  }
  for (const auto& e : a->retired) { if (auto v = find(e)) { return v; } } return {};
}
std::shared_ptr<const ProfileAccountSnapshot> ProfileManager::CaptureRosterAccount(uint64_t xuid) const {
  const auto a = roster_authority_; const auto roster = CaptureSelectedRoster(); if (!a || !roster) { return {}; }
  std::unique_lock lock(profile_mount_mutex_, std::try_to_lock); if (!lock.owns_lock()) { return {}; }
  auto find = [&](const auto& e) -> std::shared_ptr<const ProfileAccountSnapshot> {
    if (e && e->roster == roster) { for (const auto& row : e->rows) { if (row.profile && row.profile->xuid() == xuid) { return row.account; } } } return {};
  };
  if (auto v = find(a->current)) { return v; }
  if (a->pending) { if (auto v = find(a->pending->before)) { return v; } if (auto v = find(a->pending->after)) { return v; } if (auto v = find(a->pending->canceled_empty)) { return v; } }
  for (const auto& op:a->retiring) {
    if (auto v=find(op->before)) { return v; }
    if (auto v=find(op->after)) { return v; }
    if (auto v=find(op->canceled_empty)) { return v; }
  }
  for (const auto& e : a->retired) { if (auto v = find(e)) { return v; } } return {};
}
std::shared_ptr<ProfileAccountIdentity> ProfileManager::CaptureRosterIdentity(uint64_t xuid) const {
  const auto account = CaptureRosterAccount(xuid); return account ? account->identity() : nullptr;
}

std::shared_ptr<ProfileRosterIntent> ProfileManager::PrepareRoster(
    const ProfileRosterDesired& desired, const ProfileRosterExpected& expected,
    std::error_code& error) {
  return PrepareRosterAttempt(desired, expected, error, nullptr);
}
std::shared_ptr<ProfileRosterIntent> ProfileManager::PrepareRosterAttempt(
    const ProfileRosterDesired& desired, const ProfileRosterExpected& expected,
    std::error_code& error, std::shared_ptr<ProfileRosterIntent>* retained_attempt,
    const ProfileDeletionReservation* deletion,
    const ProfilePreparationProgress* progress) {
  if (retained_attempt) { retained_attempt->reset(); }
  error.clear();
  if (xe::global_critical_region::is_held_by_current_thread()) { error = Busy(); return {}; }
  auto use = account_commit_gate_->Acquire();
  if (!use) { error = Stale(); return {}; }
  const auto a = roster_authority_;
  std::vector<std::shared_ptr<ProfileRosterAuthority::Envelope>> reap;
  std::shared_ptr<ProfileRosterIntent> intent;
  try {
  auto operation = std::make_shared<ProfileRosterAuthority::Operation>();
  auto state = std::make_shared<ProfileRosterIntent::State>();
  auto next = std::make_shared<ProfileRosterAuthority::Envelope>();
  auto metadata = std::make_shared<HostRoster::State>();
  intent = std::shared_ptr<ProfileRosterIntent>(new ProfileRosterIntent(state));
  operation->intent = intent; operation->desired = desired; operation->after = next;
  std::set<uint64_t> unique;
  for (const auto xuid : desired.xuids) {
    if (xuid && !unique.insert(xuid).second) { error = std::make_error_code(std::errc::invalid_argument); return {}; }
  }
  {
    std::unique_lock lock(profile_mount_mutex_, std::try_to_lock);
    if (!lock.owns_lock()) { error = Busy(); return {}; }
    bool deleting = false;
    if (deletion && deletion->owner == this && deletion->xuid &&
        deletion->metadata && deletion->source && expected.roster) {
      const auto entry = retiring_profiles_.find(deletion->xuid);
      const auto known = account_metadata_.find(deletion->xuid);
      const auto& source = *deletion->source;
      deleting = retiring_profiles_.size() == 1 && entry != retiring_profiles_.end() &&
          std::addressof(entry->first) == deletion->retirement &&
          entry->second.get() == deletion &&
          known != account_metadata_.end() && known->second == deletion->metadata &&
          accounts_.contains(deletion->xuid) &&
          !profile_discovery_conflicts_.contains(deletion->xuid) &&
          source.available && source.profile_writable && source.storage &&
          source.managed_identity && !source.storage->retiring() &&
          !source.managed_identity->retiring() &&
          deletion->metadata->xuid() == deletion->xuid &&
          deletion->metadata->source_id() == source.source.id &&
          deletion->metadata->device_id() == source.source.device_id &&
          deletion->metadata->source_generation() == source.generation &&
          deletion->metadata->source_root() == source.managed_identity->source()->path();
      bool removed = false;
      for (uint8_t slot = 0; slot != 4; ++slot) {
        const auto old_xuid = expected.roster->slots()[slot].xuid;
        removed |= old_xuid == deletion->xuid;
        deleting &= !desired.origins[slot] && desired.xuids[slot] ==
            (old_xuid == deletion->xuid ? 0 : old_xuid);
      }
      deleting &= removed;
    }
    if ((deletion && !deleting) || !a || !a->open.load(std::memory_order_acquire) || a->pending ||
        !a->ordinary.load(std::memory_order_acquire) || !expected.proof_ ||
        expected.proof_->authority.lock() != a || !expected.roster ||
        expected.roster != a->current->roster || expected.proof_->exact != expected.roster.get() ||
        preparing_slots_.any() || (!retiring_profiles_.empty() && !deleting) || !a->next_operation) {
      error = Stale(); return {};
    }
    // Only expired/revoked old tickets can be discarded. Real entered scopes
    // hold their native resources until Leave/actual native rundown.
    std::erase_if(a->tickets, [](const auto& weak) { return weak.expired(); });
    if (!a->scopes.load(std::memory_order_acquire)) {
      // The PM gate is already held: never wait PM -> GCRC. A scalar reader
      // may still be copying a previously selected envelope's metadata. Defer
      // this optional reap if its existing region is occupied; retain all
      // owners, and destroy any detached envelopes after both locks unwind.
      auto global = xe::global_critical_region::TryAcquire();
      if (global.owns_lock()) { reap.swap(a->retired); }
    }
    operation->before = a->current;
    state->authority = a; state->operation = a->next_operation++;
    state->preparation_owner = a->native_owner;
    state->old_roster = expected.roster; state->result.before_epoch = expected.roster->epoch();
    metadata->epoch = expected.roster->epoch();
    bool changed = false;
    for (uint8_t slot = 0; slot != 4; ++slot) {
      const auto& old = expected.roster->slots()[slot];
      bool same = old.xuid == desired.xuids[slot];
      if (same && desired.origins[slot] && desired.xuids[slot]) {
        const auto& origin = *desired.origins[slot];
        same = old.device_id == origin.device_id && old.source_id == origin.source_id &&
               old.source_generation == origin.source_generation;
      }
      state->result.changed[slot] = !same; changed |= !same;
      if (same) { next->rows[slot] = operation->before->rows[slot]; metadata->slots[slot] = old; }
      if (!same && desired.xuids[slot]) {
        if (!next_profile_sign_in_generation_ || next_profile_sign_in_generation_ == UINT64_MAX) {
          error = std::make_error_code(std::errc::value_too_large); return {};
        }
        metadata->slots[slot].xuid = desired.xuids[slot];
        metadata->slots[slot].sign_in_generation = next_profile_sign_in_generation_++;
      }
    }
    if (changed) {
      if (metadata->epoch == UINT64_MAX) { error = std::make_error_code(std::errc::value_too_large); return {}; }
      ++metadata->epoch;
    }
    state->result.after_epoch = metadata->epoch;
    next->roster.reset(new HostRoster(metadata)); state->new_roster = next->roster;
    // Allocate all destination-map nodes before native arm. These maps also
    // retain genuine signed-out cached metadata without synthesizing Accounts.
    // Only selected-row nodes are prepared. Unrelated signed-out inventory
    // and Account writers remain independent and cannot be rolled back by a
    // whole-map copy made before native execution.
    if(a->binding){
      auto empty_metadata=std::make_shared<HostRoster::State>();
      empty_metadata->epoch=expected.roster->epoch();
      bool removed=false;
      state->canceled_empty_result.before_epoch=expected.roster->epoch();
      for(size_t slot=0;slot<4;++slot){
        const bool changed=expected.roster->slots()[slot].xuid!=0;
        state->canceled_empty_result.changed[slot]=changed;removed|=changed;
      }
      if(removed){
        if(empty_metadata->epoch==UINT64_MAX){error=std::make_error_code(std::errc::value_too_large);return {};}
        ++empty_metadata->epoch;
      }
      state->canceled_empty_result.after_epoch=empty_metadata->epoch;
      operation->canceled_empty=std::make_shared<ProfileRosterAuthority::Envelope>();
      operation->canceled_empty->roster.reset(new HostRoster(empty_metadata));
      state->canceled_empty_roster=operation->canceled_empty->roster;
    }
    retired_gpd_models_.reserve(retired_gpd_models_.size() + (a->binding?8:4));
    // Reserve destinations for every handed-off operation before native arm.
    a->retired.reserve(a->retired.size() + 2*(a->retiring.size()+1));
    a->retiring.reserve(a->retiring.size()+1);
    a->pending = operation;
    if (retained_attempt) { *retained_attempt = intent; }
    a->diagnostic_intent.store(reinterpret_cast<uintptr_t>(intent.get()),std::memory_order_release);
    a->diagnostic_operation.store(state->operation,std::memory_order_release);
    a->diagnostic_phase.store(uint32_t(ProfileRosterPhase::kPreparing),std::memory_order_release);
    a->diagnostic_pending.store(true,std::memory_order_release);
    a->diagnostic_available.fetch_or(4,std::memory_order_release);
    for (uint8_t slot = 0; slot != 4; ++slot) {
      operation->reserved_slots[slot]=state->result.changed[slot] ||
          (bool(operation->canceled_empty) && bool(operation->before->rows[slot].model));
      if (operation->reserved_slots[slot]) { preparing_slots_.set(slot); }
      if (desired.xuids[slot]) { preparing_profiles_.insert(desired.xuids[slot]); }
    }
  }
  reap.clear(); // native owners released after leaving the PM metadata lock
  uint32_t cache_refusal=0; // optional terminal scalar only, never admission
  auto fail = [&](std::error_code reason,
                  std::source_location where = std::source_location::current()) -> std::shared_ptr<ProfileRosterIntent> {
    // The pending metadata may already be observed by host cleanup retries.
    // Keep it immutable; this failed Prepare returns its real reason directly.
    error = reason ? reason : Stale();
    CancelRoster(intent); // genuine cleanup, retaining canceling if real busy
    // This cut owns no PM/cache/filesystem guard. Observe only the completed
    // original refusal after real cancellation; direct pre-pending exits are
    // deliberately outside this bounded diagnostic.
    static NativeSystemUiTraceLimiter trace;
    trace.EmitTransition((uint64_t(where.line()) << 32) | uint32_t(error.value()),
        "profile", "roster_prepare_refusal",
        {{"line", where.line()}, {"operation", state->operation},
         {"cache_refusal",cache_refusal},
         {"error", uint32_t(error.value())},
         {"category", error.category() == std::generic_category() ? 1u :
             error.category() == std::system_category() ? 2u : 0u}});
    return {};
  };
  auto fail_locked = [&](std::unique_lock<std::recursive_mutex>& lock, std::error_code reason,
                         std::source_location where = std::source_location::current()) {
    if (lock.owns_lock()) { lock.unlock(); }
    return fail(reason, where);
  };
  // Existing old roster stays valid while affected writers are reversibly
  // excluded. No Logout, model retirement or epoch publication occurs here.
  for (uint8_t slot = 0; slot != 4; ++slot) {
    const auto& old = operation->before->rows[slot];
    if (state->result.changed[slot] && old.storage && old.storage->retiring()) {
      if (!old.identity || !old.identity->writes_drained()) { return fail(Busy()); }
      continue; // actual removal already denied storage writes; never reopen it
    }
    if (old.identity) {
      operation->account_pauses[slot] = old.identity->PauseForRoster(error);
      if (!operation->account_pauses[slot]) { return fail(error); }
    }
    if (operation->reserved_slots[slot] && old.model) {
      operation->model_pauses[slot] = old.model->PauseForRoster(error);
      if (!operation->model_pauses[slot]) { return fail(error); }
    }
  }
  for (uint8_t slot = 0; slot != 4; ++slot) {
    auto& row = next->rows[slot];
    const uint64_t xuid = desired.xuids[slot];
    if (!xuid) { continue; }
    if (state->result.changed[slot]) {
      std::string source_id;
      if (desired.origins[slot]) { source_id = desired.origins[slot]->source_id; }
      if (source_id.empty()) {
        std::unique_lock lock(profile_mount_mutex_, std::try_to_lock);
        if (!lock.owns_lock()) { return fail_locked(lock, Busy()); }
        const auto known = account_metadata_.find(xuid);
        if (known == account_metadata_.end() || known->second->source_id().empty() ||
            profile_discovery_conflicts_.contains(xuid)) {
          return fail_locked(lock, std::make_error_code(std::errc::no_such_file_or_directory));
        }
        source_id = known->second->source_id();
      }
      const auto source = content_manager_.sources().Source(source_id);
      if (!source || !source->available || !source->profile_writable || !source->storage ||
          source->storage->retiring() || !source->managed_identity || source->managed_identity->retiring()) {
        return fail(std::make_error_code(std::errc::no_such_device));
      }
      if (desired.origins[slot]) {
        const auto& wanted = *desired.origins[slot];
        if (source->source.device_id != wanted.device_id || source->generation != wanted.source_generation) {
          return fail(Stale());
        }
      }
      // An unsigned old Account writer is not a signed-in slot. Preserve the
      // accepted retirement behavior and refuse until that real writer drains.
      {
        std::unique_lock lock(profile_mount_mutex_, std::try_to_lock);
        if (!lock.owns_lock()) { return fail_locked(lock, Busy()); }
        const auto old = account_identities_.find(xuid);
        if (old != account_identities_.end() && old->second->slot() == 0xFF) {
          old->second->Retire();
          if (!old->second->writes_drained()) { return fail_locked(lock, Busy()); }
        }
        for (const auto& retired : retired_account_identities_) {
          if ((retired->storage()->xuid() == xuid || retired->slot() == slot) &&
              !retired->writes_drained()) { return fail_locked(lock, Busy()); }
        }
      }
      row.mount=AcquireRosterMount(xuid,error,&*source);
      if (!row.mount) { return fail(error?error:Busy()); }
      operation->staged_mount[slot] = true;
      row.storage = AcquireProfileStorageLease(xuid, error);
      while(!row.storage && progress && progress->Retry(error))
        row.storage=AcquireProfileStorageLease(xuid,error);
      if (!row.storage || row.storage->source_id() != source_id ||
          row.storage->source_generation() != source->generation ||
          row.storage->device_id() != source->source.device_id) { return fail(error); }
      const auto generation = metadata->slots[slot].sign_in_generation;
      row.model = ProfileGpdModelAuthority::Create(xuid, slot, generation, row.storage, error);
      const auto owner = content_manager_.ProfileIntents(source_id);
      if (!row.model || !owner || (error = LoadGpdModel(row.model, row.storage, owner,progress))) { return fail(error); }
      row.identity = ProfileAccountIdentity::Capture(row.storage, slot, generation, error);
      if (!row.identity) { return fail(error); }
      operation->account_pauses[4 + slot] = row.identity->PauseForRoster(error);
      if (!operation->account_pauses[4 + slot]) { return fail(error); }
      metadata->slots[slot].source_id = source_id;
      metadata->slots[slot].source_generation = source->generation;
      metadata->slots[slot].device_id = source->source.device_id;
    }
    // Even unchanged/no-op rows must encode CURRENT durable paused Account,
    // never a previously cached name or an invented native cache row.
    row.account = ProfileAccountSnapshot::Load(row.identity, error);
    if (!row.account) { return fail(error); }
    auto bytes = std::span<uint8_t>(state->native_rows).subspan(slot * 0x188, 0x188);
    if ((error = NativeLogonAccountInputs::EncodeRow(*row.account, bytes))) { return fail(error); }
    if (desired.origins[slot] && desired.origins[slot]->original_account_row &&
        !std::equal(bytes.begin(), bytes.end(), desired.origins[slot]->original_account_row->begin())) {
      return fail(Stale());
    }
    std::shared_ptr<vfs::HostPathDevice::PreparedSnapshotCache> cache;
    for(;;){
      if(!cache){
        cache=row.storage->device()->PrepareSnapshotCache({row.account->token()},error);
        if(!cache){
          if(progress && progress->Retry(error))continue;
          if(cvars::native_system_ui_trace && error==std::errc::device_or_resource_busy)
            cache_refusal=uint32_t(vfs::HostPathDevice::SnapshotCacheRefusal::kNamespaceBusy);
          return fail(error);
        }
      }
      auto checked=row.storage->view()->ValidateSnapshotBatch({row.account->token()},error);
      if(!checked){if(progress && progress->Retry(error))continue;return fail(error);}
      std::shared_ptr<const filesystem::HostStorageSnapshotBatch> batch(std::move(checked));
      vfs::HostPathDevice::SnapshotCacheRefusal why;
      auto commit=row.storage->device()->TryCommitSnapshotCache(cache,batch,error,&why);
      if(commit){commit->Apply();break;} // exact bytes, never roster publication
      batch.reset(); // no validation or cache guard survives caller progress
      if(why==vfs::HostPathDevice::SnapshotCacheRefusal::kCapacityChanged)cache.reset();
      if(progress && progress->Retry(error))continue;
      if(cvars::native_system_ui_trace)cache_refusal=uint32_t(why);
      if(!error)error=Stale();return fail(error);
    }
    const auto old_account = row.profile ? row.profile->account_info_.load(std::memory_order_acquire) : nullptr;
    if (!old_account || std::memcmp(old_account.get(), &row.account->account(), sizeof(X_XAMACCOUNTINFO))) {
      auto prepared_profile = std::make_shared<UserProfile>(xuid, &row.account->account(), row.model);
      if (row.profile) {
        // Ordinary Account data refresh is not a sign-in epoch. Preserve real
        // transient properties and cached images while replacing only the
        // detached Account projection before it can become selected.
        std::scoped_lock fields(row.profile->prop_mutex, row.profile->profile_images_mutex_);
        prepared_profile->properties_ = row.profile->properties_;
        prepared_profile->profile_images_ = row.profile->profile_images_;
      }
      row.profile = std::move(prepared_profile);
    }
    operation->profiles.emplace(slot, row.profile);
    operation->models.emplace(xuid, row.model);
    operation->generations.emplace(xuid, metadata->slots[slot].sign_in_generation);
    operation->identities.insert_or_assign(xuid, row.identity);
    operation->snapshots.insert_or_assign(xuid, row.account);
    operation->metadata.insert_or_assign(xuid, std::make_shared<const ProfileAccountMetadata>(*row.account));
    operation->accounts.insert_or_assign(xuid, row.account->account());
  }
  // Drop signed-out identities/snapshots from the prepared signed-in maps;
  // their immutable metadata/account inventory is retained independently.
  for (const auto& row : operation->before->rows) {
    if (row.profile && !operation->models.contains(row.profile->xuid())) {
      operation->identities.erase(row.profile->xuid());
      operation->snapshots.erase(row.profile->xuid());
    }
  }
  {
    std::unique_lock lock(profile_mount_mutex_, std::try_to_lock);
    if (!lock.owns_lock()) { return fail_locked(lock, Busy()); }
    if (a->pending != operation || !a->open.load(std::memory_order_acquire) ||
        a->current->roster != expected.roster || !BeforeCurrent(*operation) ||
        !ProfileRosterAuthority::LogicalCurrent(*next)) { return fail_locked(lock, Stale()); }
    const auto empty = [](const ProfileRosterAuthority::Envelope& envelope) {
      if (!envelope.roster) { return false; }
      for (size_t slot = 0; slot != 4; ++slot) {
        const auto& row = envelope.rows[slot];
        const auto& selected = envelope.roster->slots()[slot];
        if (row.profile || row.model || row.identity || row.account || row.storage || row.mount ||
            selected != ProfileRosterSlot{}) { return false; }
      }
      return true;
    };
    state->empty_unchanged_rows = empty(*operation->before) && empty(*operation->after) &&
        state->result.before_epoch == state->result.after_epoch &&
        std::none_of(state->result.changed.begin(), state->result.changed.end(), [](bool v) { return v; }) &&
        std::all_of(state->native_rows.begin(), state->native_rows.end(), [](uint8_t v) { return v == 0; });
    // Compare the actual held owners once, after CURRENT durable Account loads
    // and existing source/cache validation. An occupied unchanged row is not
    // an absent row or a freshly decoded image. Its shared GPD authority may
    // evolve through ordinary writes; this proves no authority replacement,
    // never a historical GPD version or perpetual byte immutability.
    const auto same_owner = [](const auto& a, const auto& b) {
      return a.get()==b.get() && !a.owner_before(b) && !b.owner_before(a);
    };
    const auto same_rows = [&] {
      bool occupied=false;
      if (operation->before->roster->slots()!=operation->after->roster->slots())return false;
      for (size_t slot=0;slot!=4;++slot) {
        const auto& a=operation->before->rows[slot];
        const auto& b=operation->after->rows[slot];
        if (!same_owner(a.profile,b.profile) || !same_owner(a.model,b.model) ||
            !same_owner(a.identity,b.identity) || !same_owner(a.storage,b.storage) ||
            !same_owner(a.mount,b.mount))return false;
        if (!a.account || !b.account) {
          if (a.account || b.account || a.profile || a.model || a.identity || a.storage || a.mount ||
              operation->before->roster->slots()[slot]!=ProfileRosterSlot{})return false;
          continue;
        }
        if (!a.profile || !a.model || !a.identity || !a.storage ||
            !same_owner(a.account->identity(),a.identity) ||
            !same_owner(b.account->identity(),b.identity))return false;
        occupied=true;
        const auto& at=a.account->token();const auto& bt=b.account->token();
        if (!at || !bt || !at->exists() || !bt->exists() ||
            !same_owner(a.account->identity(),b.account->identity()) ||
            a.account->devkit()!=b.account->devkit() ||
            std::memcmp(&a.account->account(),&b.account->account(),sizeof(X_XAMACCOUNTINFO)) ||
            !same_owner(at->root(),bt->root()) || at->relative()!=bt->relative() ||
            at->info().filesystem_id!=bt->info().filesystem_id ||
            at->info().file_id!=bt->info().file_id || at->info().size!=bt->info().size ||
            !std::ranges::equal(at->bytes(),bt->bytes()))return false;
      }
      return occupied; // All-empty remains solely the older zero-wire proof.
    };
    state->unchanged_owners = state->result.before_epoch==state->result.after_epoch &&
        std::none_of(state->result.changed.begin(),state->result.changed.end(),[](bool v){return v;}) &&
        same_rows();
    state->phase.store(ProfileRosterPhase::kPrepared, std::memory_order_release);
    a->diagnostic_phase.store(uint32_t(ProfileRosterPhase::kPrepared),std::memory_order_release);
  }
  return intent;
  } catch (const std::bad_alloc&) {
    error = std::make_error_code(std::errc::not_enough_memory);
  } catch (...) {
    error = std::make_error_code(std::errc::invalid_argument);
  }
  if (intent) { CancelRoster(intent); }
  return {};
}

std::shared_ptr<ProfileRosterIntent> ProfileManager::PrepareRosterWithProgress(
    const ProfileRosterDesired& desired, const ProfileRosterExpected& expected,
    std::error_code& error, std::chrono::steady_clock::time_point deadline,
    const std::function<bool()>& wait,
    const ProfileDeletionReservation* deletion) {
  error.clear();
  // An inherited namespace lock is a self dependency, not foreign contention.
  if (global_critical_region::is_held_by_current_thread()) { error=Busy();return {}; }
  auto use=account_commit_gate_->Acquire();
  if(!use){error=Stale();return {};}
  const auto authority=roster_authority_;
  const ProfilePreparationProgress phase_progress{deadline,wait,authority?&authority->open:nullptr};
  std::shared_ptr<ProfileRosterIntent> retained;
  const auto retryable=[](const std::error_code& e){
    return e==std::errc::device_or_resource_busy ||
        e==std::errc::resource_unavailable_try_again;
  };
  const auto open=[&]{return authority && authority->open.load(std::memory_order_acquire);};
  const auto progress=[&]{
    return open() && !global_critical_region::is_held_by_current_thread() &&
        std::chrono::steady_clock::now()<deadline && wait && wait() && open();
  };
  try {
    for(;;){
      if(!open()){error=Stale();return {};}
      if(std::chrono::steady_clock::now()>=deadline){error=Busy();return {};}
      auto prepared=PrepareRosterAttempt(desired,expected,error,&retained,deletion,&phase_progress);
      const auto failure=error?error:Stale();
      if(prepared && !error){
        if(open() && std::chrono::steady_clock::now()<deadline)return prepared;
        error=open()?Busy():Stale();CancelRosterPreparation(retained);return {};
      }
      // A null result may have published mounts/pauses. Finish the exact
      // canceled operation before another Prepare, regardless of error kind.
      while(retained && retained->result().phase!=ProfileRosterPhase::kCanceled){
        const auto canceled=CancelRoster(retained);
        if(canceled.phase==ProfileRosterPhase::kCanceled)break;
        if(!retryable(canceled.error) || !progress()){
          CancelRosterPreparation(retained);error=open()?failure:Stale();return {};
        }
      }
      retained.reset();
      if(!retryable(failure) || !progress()){error=open()?failure:Stale();return {};}
      // expected/desired/source origins remain EXACT. The one-shot CAS will
      // reject a changed roster; no new snapshot silently adopts another user.
    }
  } catch(...) {
    if(retained)CancelRosterPreparation(retained);
    error=std::make_error_code(std::errc::invalid_argument);return {};
  }
}
ProfileRosterResult ProfileManager::CancelRosterPreparation(
    const std::shared_ptr<ProfileRosterIntent>& intent) {
  // Capture immutable metadata before attempting a new gate admission. The
  // originating whole preparation/Adopt scope already retains its real Use;
  // after a closing gate refuses, only this independent intent is touched.
  if(!intent || intent->state_->authority.lock()!=roster_authority_){
    ProfileRosterResult result;result.error=Stale();return result;
  }
  auto use=account_commit_gate_->Acquire();
  ProfileRosterResult result;
  if(!use){result=intent->result();result.error=Stale();}
  else try { result=CancelRoster(intent); }
  catch(...) { result=intent->result();result.error=std::make_error_code(std::errc::io_error); }
  if(result.phase==ProfileRosterPhase::kCanceled)return result;
  const auto phase=intent->state_->phase.load(std::memory_order_acquire);
  if(phase!=ProfileRosterPhase::kPreparing && phase!=ProfileRosterPhase::kPrepared &&
      phase!=ProfileRosterPhase::kCanceling)return result; // Native owns outcome.
  intent->state_->preparation_cancel_requested.store(true,std::memory_order_release);
  if(auto owner=intent->state_->preparation_owner.lock())
    owner->RetainPreparationCleanup(intent);
  // The actual PM pending Operation already retains every mount/pause. For
  // an unbound host it remains owned through actual KernelStop and the PM's
  // existing CloseRosterAuthority; it is never an invented canceled result.
  return result;
}

ProfileRosterResult ProfileManager::CancelRoster(const std::shared_ptr<ProfileRosterIntent>& intent) {
  if (!intent) { ProfileRosterResult r; r.error = std::make_error_code(std::errc::invalid_argument); return r; }
  auto use = account_commit_gate_->Acquire();
  if (!use) { auto r = intent->result(); r.error = Stale(); return r; }
  const auto a = roster_authority_;
  std::shared_ptr<ProfileRosterAuthority::Operation> op;
  // A real commit can remove model.pending_ after this capture. Keep its
  // native-origin/storage/snapshot holders until every PM guard unwinds.
  std::array<std::shared_ptr<const ProfileGpdPendingMutation>,4> canceled_pending_models;
  {
    std::unique_lock lock(profile_mount_mutex_, std::try_to_lock);
    if (!lock.owns_lock()) { auto r = intent->result(); r.error = Busy(); return r; }
    if (!a || intent->state_->authority.lock() != a || !a->pending || a->pending->intent != intent) { return intent->result(); }
    op = a->pending;
    auto phase = intent->state_->phase.load(std::memory_order_acquire);
    if (phase == ProfileRosterPhase::kNativeEntered || phase == ProfileRosterPhase::kPublished ||
        phase == ProfileRosterPhase::kFinalizing || phase == ProfileRosterPhase::kUncertain) {
      auto r = intent->result(); r.error = Busy(); return r; // native owns outcome
    }
    intent->state_->phase.store(ProfileRosterPhase::kCanceling, std::memory_order_release);
    a->diagnostic_phase.store(uint32_t(ProfileRosterPhase::kCanceling),std::memory_order_release);
  }
  bool idle = false;
  if (!op->settling.compare_exchange_strong(idle, true, std::memory_order_acq_rel)) {
    auto r = intent->result(); r.error = Busy(); return r;
  }
  auto release = [&op](void*) { op->settling.store(false, std::memory_order_release); };
  std::unique_ptr<void, decltype(release)> serialized(reinterpret_cast<void*>(1), release);
  {
    std::lock_guard lock(profile_mount_mutex_);
    if (a->pending!=op || intent->state_->phase.load(std::memory_order_acquire)!=ProfileRosterPhase::kCanceling) {
      auto result=intent->result();result.error=Stale();return result;
    }
    // Only exact unpublished staged owners may be retired. Never retire a
    // current/shared identity or infer ownership from an XUID alias. Native
    // entry/publication is excluded by the phase transition above.
    for (uint8_t slot=0;slot!=4;++slot) if (op->staged_mount[slot]) {
      const auto& row=op->after->rows[slot];
      if (row.identity && std::none_of(a->current->rows.begin(),a->current->rows.end(),
          [&](const auto& current){return current.identity==row.identity;}) && !row.identity->writes_drained()) {
        auto result=intent->result();result.error=Busy();return result;
      }
      if (row.model && std::none_of(a->current->rows.begin(),a->current->rows.end(),
          [&](const auto& current){return current.model==row.model;})) {
        std::error_code error;
        canceled_pending_models[slot]=row.model->Pending(error);
        if (error || canceled_pending_models[slot]) { auto result=intent->result();result.error=Busy();return result; }
      }
    }
    for (uint8_t slot=0;slot!=4;++slot) if (op->staged_mount[slot]) {
      const auto& row=op->after->rows[slot];
      if (row.identity && std::none_of(a->current->rows.begin(),a->current->rows.end(),
          [&](const auto& current){return current.identity==row.identity;})) { row.identity->Retire(); }
      if (row.model && std::none_of(a->current->rows.begin(),a->current->rows.end(),
          [&](const auto& current){return current.model==row.model;})) { row.model->Retire(); }
    }
  }
  bool busy = false;
  for (uint8_t slot = 0; slot != 4; ++slot) {
    if (!op->staged_mount[slot]) { continue; }
    if (!op->after->rows[slot].mount || !ReleaseRosterMount(op->after->rows[slot].mount)) { busy = true; }
    else { op->staged_mount[slot] = false; }
  }
  if (busy) { auto r = intent->result(); r.error = Busy(); return r; }
  for (auto& pause : op->account_pauses) { pause.reset(); }
  for (auto& pause : op->model_pauses) { pause.reset(); }
  {
    std::lock_guard lock(profile_mount_mutex_);
    if (a->pending != op) { auto r = intent->result(); r.error = Stale(); return r; }
    for (uint8_t slot = 0; slot != 4; ++slot) { preparing_slots_.reset(slot); preparing_generations_[slot] = 0; }
    for (auto xuid : op->desired.xuids) { preparing_profiles_.erase(xuid); }
    for (auto& weak : op->tickets) {
      if (auto ticket = weak.lock()) { ticket->state_->open.store(false, std::memory_order_release); }
    }
    a->pending.reset(); intent->state_->phase.store(ProfileRosterPhase::kCanceled, std::memory_order_release);
    a->diagnostic_phase.store(uint32_t(ProfileRosterPhase::kCanceled),std::memory_order_release);
    a->diagnostic_pending.store(false,std::memory_order_release);
  }
  return intent->result(); // op/native owners release outside metadata locks
}

ProfileRosterResult ProfileManager::CommitHostRoster(const std::shared_ptr<ProfileRosterIntent>& intent) {
  if (!intent) { ProfileRosterResult r; r.error = std::make_error_code(std::errc::invalid_argument); return r; }
  auto use = account_commit_gate_->Acquire();
  if (!use) { auto r = intent->result(); r.error = Stale(); return r; }
  const auto a = roster_authority_;
  std::shared_ptr<ProfileRosterAuthority::Operation> op;
  {
    std::unique_lock lock(profile_mount_mutex_, std::try_to_lock);
    if (!lock.owns_lock()) { auto r = intent->result(); r.error = Busy(); return r; }
    if (!a || !a->open.load(std::memory_order_acquire) || !a->pending ||
        a->pending->intent != intent || a->binding ||
        intent->state_->phase.load(std::memory_order_acquire) != ProfileRosterPhase::kPrepared) {
      auto r = intent->result(); r.error = Stale(); return r;
    }
    op = a->pending;
  }
  for (const auto& row : op->after->rows) {
    if (!row.account) { continue; }
    std::error_code error = row.account->CheckAvailable();
    auto checked = !error ? row.storage->view()->ValidateSnapshotBatch({row.account->token()}, error) : nullptr;
    if (!checked) { auto r = intent->result(); r.error = error ? error : Stale(); return r; }
  }
  {
    std::unique_lock lock(profile_mount_mutex_, std::try_to_lock);
    if (!lock.owns_lock()) { auto r=intent->result();r.error=Busy();return r; }
    if (a->pending != op || a->binding || !BeforeCurrent(*op) ||
        !ProfileRosterAuthority::LogicalCurrent(*op->after) ||
        intent->state_->phase.load(std::memory_order_acquire) != ProfileRosterPhase::kPrepared) {
      auto r = intent->result(); r.error = Stale(); return r;
    }
    a->selected.store(op->after.get(), std::memory_order_release);
    intent->state_->published_choice.store(1,std::memory_order_release);
    a->DiagnosticSelected(op->after.get());
    intent->state_->phase.store(ProfileRosterPhase::kPublished, std::memory_order_release);
    a->diagnostic_phase.store(uint32_t(ProfileRosterPhase::kPublished),std::memory_order_release);
  }
  return SettleRoster(intent, RosterSettlementStage::kHostComplete);
}
ProfileRosterResult ProfileManager::AdoptRoster(const ProfileRosterDesired& desired,
    const ProfileRosterExpected& expected) {
  return AdoptRosterInternal(desired, expected, nullptr);
}
ProfileRosterResult ProfileManager::AdoptRosterInternal(const ProfileRosterDesired& desired,
    const ProfileRosterExpected& expected, const ProfileDeletionReservation* deletion) {
  std::error_code error;
  auto use=account_commit_gate_->Acquire();
  if(!use){ProfileRosterResult result;result.error=Stale();return result;}
  const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(15);
  const auto wait=[] {
    // Legacy guest UserLogon can reach this facade before any Owner public
    // frame is counted, including an unbound fallback. Preserve its real
    // one-shot Busy; never strand a PM Use on an uncounted guest fiber.
    // Original native 20B/224 use TryLogon's already-counted caller adapter.
    if(ActualRosterThread())return false;
    threading::Sleep(std::chrono::milliseconds(16));return true;
  };
  auto intent=PrepareRosterWithProgress(desired,expected,error,deadline,wait,deletion);
  if (!intent) { ProfileRosterResult r; r.error = error; return r; }
  std::shared_ptr<::xe::kernel::NativeProfileIdentityOwner> owner;
  bool claimed = false;
  {
    std::lock_guard lock(profile_mount_mutex_);
    claimed = bool(roster_authority_->binding);
    owner = roster_authority_->native_owner.lock();
  }
  if (!claimed) {
    for(;;){
      auto result=CommitHostRoster(intent);
      if(!result.error || result.phase!=ProfileRosterPhase::kPrepared)return result;
      if((result.error!=std::errc::device_or_resource_busy &&
          result.error!=std::errc::resource_unavailable_try_again) ||
          std::chrono::steady_clock::now()>=deadline ||
          !roster_authority_->open.load(std::memory_order_acquire)){
        CancelRosterPreparation(intent);return result;
      }
      if(!wait()){CancelRosterPreparation(intent);return result;} // all commit guards unwound
    }
  }
  if (!owner) { auto result = CancelRosterPreparation(intent); result.error = Busy(); return result; }
  auto result=owner->SubmitPreparedUntil(intent,deadline); // no PM/native/FS lock across dispatch
  if(result.error && (result.phase==ProfileRosterPhase::kPreparing ||
      result.phase==ProfileRosterPhase::kPrepared || result.phase==ProfileRosterPhase::kCanceling))
    CancelRosterPreparation(intent);
  return result;
}

std::shared_ptr<const ProfileRosterNativeBinding> ProfileManager::ClaimNativeRosterFactory() {
  std::unique_lock lock(profile_mount_mutex_, std::try_to_lock);
  const auto a = roster_authority_;
  if (!lock.owns_lock() || !a || !a->open.load(std::memory_order_acquire) || a->binding || a->pending ||
      !a->retiring.empty() || !a->next_nonce) { return {}; }
  auto s = std::make_shared<ProfileRosterNativeBinding::State>(); s->authority = a; s->nonce = a->next_nonce++;
  auto binding = std::shared_ptr<const ProfileRosterNativeBinding>(new ProfileRosterNativeBinding(std::move(s)));
  a->binding = binding; a->diagnostic_enabled.store(true,std::memory_order_release); return binding;
}
bool ProfileManager::InstallNativeRosterOwner(const std::shared_ptr<const ProfileRosterNativeBinding>& binding,
    std::weak_ptr<::xe::kernel::NativeProfileIdentityOwner> owner) {
  std::unique_lock lock(profile_mount_mutex_, std::try_to_lock);
  const auto a = roster_authority_;
  if (!lock.owns_lock() || !a || !a->open.load(std::memory_order_acquire) ||
      !binding || a->binding != binding || owner.expired() || !a->native_owner.expired()) { return false; }
  a->native_owner = std::move(owner); return true;
}
std::shared_ptr<ProfileRosterNativeReadTicket> ProfileManager::CaptureCurrentNativeReadTicket(
    const std::shared_ptr<const ProfileRosterNativeBinding>& binding, std::error_code& error) {
  error.clear(); std::unique_lock lock(profile_mount_mutex_, std::try_to_lock);
  const auto a = roster_authority_;
  if (!lock.owns_lock()) { error = Busy(); return {}; }
  if (!a || !a->open.load(std::memory_order_acquire) || a->binding != binding ||
      !binding || a->pending || !ProfileRosterAuthority::LogicalCurrent(*a->current)) { error = Stale(); return {}; }
  auto s = std::make_shared<ProfileRosterNativeReadTicket::State>();
  s->authority = a; s->binding = binding.get(); s->fixed = a->current.get();
  if (a->tickets.size() >= 4096) { error = Busy(); return {}; }
  auto ticket = std::shared_ptr<ProfileRosterNativeReadTicket>(new ProfileRosterNativeReadTicket(std::move(s)));
  a->tickets.push_back(ticket);
  return ticket;
}
std::shared_ptr<ProfileRosterNativeReadTicket> ProfileManager::CapturePublishedNativeReadTicket(
    const std::shared_ptr<ProfileRosterIntent>& intent,
    const std::shared_ptr<const ProfileRosterNativeBinding>& binding,std::error_code& error) {
  error.clear();
  auto use=account_commit_gate_->Acquire();
  if (!use) { error=Stale();return {}; }
  std::unique_lock lock(profile_mount_mutex_,std::try_to_lock);
  if (!lock.owns_lock()) { error=Busy();return {}; }
  const auto a=roster_authority_;
  if (!intent || !binding || !a || !a->open.load(std::memory_order_acquire) ||
      a->binding!=binding || !binding->nonce() || binding->state_->authority.lock()!=a ||
      intent->state_->authority.lock()!=a || !a->pending ||
      a->pending->intent!=intent || intent->data_handoff_complete()) { error=Stale();return {}; }
  const auto phase=intent->state_->phase.load(std::memory_order_acquire);
  if ((phase!=ProfileRosterPhase::kPublished && phase!=ProfileRosterPhase::kFinalizing) ||
      !intent->publication_outcome()) { error=Stale();return {}; }
  constexpr auto required=ProfileRosterIntent::State::kProtocol | ProfileRosterIntent::State::kRead |
      ProfileRosterIntent::State::kMaps | ProfileRosterIntent::State::kWriters;
  const auto duties=intent->state_->milestones.load(std::memory_order_acquire);
  if ((duties&required)!=required || !a->pending->maps_settled || !a->pending->effects_applied ||
      !a->ordinary.load(std::memory_order_acquire) || !a->selected_read.load(std::memory_order_acquire)) {
    error=Busy();return {};
  }
  const auto* selected=intent->publication_outcome()==ProfileRosterPublicationOutcome::kCanceledEmpty
      ?a->pending->canceled_empty.get():a->pending->after.get();
  if (!selected || a->selected.load(std::memory_order_acquire)!=selected ||
      a->current.get()!=selected || selected->roster!=intent->published_roster() ||
      !ProfileRosterAuthority::LogicalCurrent(*selected)) { error=Stale();return {}; }
  if (a->tickets.size()>=4096) { error=Busy();return {}; }
  auto state=std::make_shared<ProfileRosterNativeReadTicket::State>();
  state->authority=a;state->binding=binding.get();state->fixed=a->current.get();
  // operation0/fixed: THIS handoff must retain already-current selected reads.
  // The NEXT operation closes it by its exact before-envelope, and actual
  // entered scopes keep that envelope until Leave. Never a following retarget.
  auto ticket=std::shared_ptr<ProfileRosterNativeReadTicket>(new ProfileRosterNativeReadTicket(std::move(state)));
  a->tickets.push_back(ticket);
  return ticket;
}
std::optional<uint64_t> ProfileManager::CaptureInitialEmptyNativeRosterSequence(
    const std::shared_ptr<const ProfileRosterNativeBinding>& binding,
    const ProfileRosterExpected& expected,
    const std::shared_ptr<ProfileRosterNativeReadTicket>& ticket) const {
  std::unique_lock lock(profile_mount_mutex_, std::try_to_lock);
  const auto a = roster_authority_;
  if (!lock.owns_lock() || !a || !a->open.load(std::memory_order_acquire) ||
      !a->ordinary.load(std::memory_order_acquire) || !a->selected_read.load(std::memory_order_acquire) ||
      a->pending || !a->current || a->selected.load(std::memory_order_acquire) != a->current.get() ||
      !binding || a->binding != binding || binding->state_->authority.lock() != a ||
      !expected.roster || !expected.proof_ || expected.proof_->authority.lock() != a ||
      expected.proof_->exact != expected.roster.get() || expected.roster != a->current->roster ||
      !ticket || !ticket->state_ || ticket->state_->authority != a ||
      ticket->state_->binding != binding.get() || !ticket->state_->open.load(std::memory_order_acquire) ||
      ticket->state_->following || ticket->state_->fixed != a->current.get() ||
      ticket->state_->operation || ticket->state_->writer_old || ticket->state_->writer_new ||
      !a->next_operation) { return {}; }
  for (size_t slot = 0; slot != 4; ++slot) {
    const auto& row = a->current->rows[slot];
    if (expected.roster->slots()[slot] != ProfileRosterSlot{} || row.profile ||
        row.model || row.identity || row.account || row.storage || row.mount) { return {}; }
  }
  // A Prepare+Cancel can leave the same exact empty roster. The functional
  // operation sequence still invalidates an earlier one-time startup capture.
  return a->next_operation;
}
std::shared_ptr<ProfileRosterPublicationCell> ProfileManager::EnterNativeRoster(
    const std::shared_ptr<ProfileRosterIntent>& intent,
    const std::shared_ptr<const ProfileRosterNativeBinding>& binding, std::error_code& error) {
  error.clear();
  if (!intent || !binding || xe::global_critical_region::is_held_by_current_thread()) { error = Busy(); return {}; }
  const auto a = roster_authority_;
  std::shared_ptr<ProfileRosterAuthority::Operation> op;
  {
    std::unique_lock lock(profile_mount_mutex_, std::try_to_lock);
    if (!lock.owns_lock()) { error = Busy(); return {}; }
    if (!a || !a->open.load(std::memory_order_acquire) || a->binding != binding ||
        !a->pending || a->pending->intent != intent ||
        intent->state_->phase.load(std::memory_order_acquire) != ProfileRosterPhase::kPrepared) { error = Stale(); return {}; }
    op = a->pending;
  }
  for (const auto& row : op->after->rows) {
    if (!row.account) { continue; }
    error = row.account->CheckAvailable();
    auto checked = !error ? row.storage->view()->ValidateSnapshotBatch({row.account->token()}, error) : nullptr;
    if (!checked) { if (!error) { error = Stale(); } return {}; }
  }
  auto s = std::make_shared<ProfileRosterPublicationCell::State>();
  s->authority = a; s->old_envelope = op->before.get(); s->new_envelope = op->after.get(); s->intent = intent->state_;
  s->canceled_empty_envelope=op->canceled_empty.get();
  for(size_t slot=0;slot<4;++slot)if(!intent->state_->result.changed[slot])
    s->unchanged_model_pauses[slot]=op->model_pauses[slot].get();
  auto cell = std::shared_ptr<ProfileRosterPublicationCell>(new ProfileRosterPublicationCell(s));
  {
    std::unique_lock lock(profile_mount_mutex_, std::try_to_lock);
    if (!lock.owns_lock()) { error=Busy();return {}; }
    if (a->pending != op || !a->open.load(std::memory_order_acquire) ||
        a->binding != binding || a->selected.load(std::memory_order_acquire) != op->before.get() ||
        !BeforeCurrent(*op) || !ProfileRosterAuthority::LogicalCurrent(*op->after) ||
        intent->state_->phase.load(std::memory_order_acquire) != ProfileRosterPhase::kPrepared) {
      error = Stale(); return {};
    }
    for (auto& p : op->account_pauses) { if (p) { p->SealForNativeEntry(); } }
    for (auto& p : op->model_pauses) { if (p) { p->SealForNativeEntry(); } }
    a->ordinary.store(false, std::memory_order_release);
    a->selected_read.store(false, std::memory_order_release);
    op->cell = cell; s->armed.store(true, std::memory_order_release);
    intent->state_->phase.store(ProfileRosterPhase::kNativeEntered, std::memory_order_release);
    a->diagnostic_phase.store(uint32_t(ProfileRosterPhase::kNativeEntered),std::memory_order_release);
  }
  return cell;
}
ProfileRosterResult ProfileManager::PublishNativeRosterProtocol(
    const std::shared_ptr<ProfileRosterIntent>& intent,
    const ProfileRosterNativeCompletion& protocol) {
  const auto a = roster_authority_;
  if (!intent || !protocol.state_ || !a ||
      protocol.state_->role != ProfileRosterNativeCompletion::State::Role::kProtocol ||
      protocol.state_->authority.lock() != a || protocol.state_->intent != intent->state_ ||
      !a->binding || protocol.state_->binding != a->binding.get() ||
      protocol.state_->nonce != a->binding->nonce()) {
    ProfileRosterResult result; result.error = Stale(); return result;
  }
  auto use = account_commit_gate_->Acquire();
  if (!use) { auto result = intent->result(); result.error = Stale(); return result; }
  {
    std::unique_lock lock(profile_mount_mutex_, std::try_to_lock);
    if (!lock.owns_lock()) { auto result = intent->result(); result.error = Busy(); return result; }
    const auto phase = intent->state_->phase.load(std::memory_order_acquire);
    if (!a->open.load(std::memory_order_acquire) || !a->pending ||
        a->pending->intent != intent ||
        (phase != ProfileRosterPhase::kPublished && phase != ProfileRosterPhase::kFinalizing) ||
        !intent->publication_outcome() ||
        a->selected.load(std::memory_order_acquire) !=
            (intent->publication_outcome()==ProfileRosterPublicationOutcome::kCanceledEmpty
                ?a->pending->canceled_empty.get():a->pending->after.get()) ||
        !ProfileRosterAuthority::LogicalCurrent(*a->selected.load(std::memory_order_acquire))) {
      auto result = intent->result(); result.error = Stale(); return result;
    }
    const auto duties = intent->state_->milestones.load(std::memory_order_acquire);
    if ((duties & ProfileRosterIntent::State::kProtocol) &&
        intent->state_->native_protocol_value.load(std::memory_order_acquire) != protocol.state_->native_value) {
      auto result = intent->result(); result.error = Stale(); return result;
    }
    intent->state_->native_protocol_value.store(protocol.state_->native_value, std::memory_order_relaxed);
    intent->state_->milestones.fetch_or(ProfileRosterIntent::State::kProtocol |
        ProfileRosterIntent::State::kRead, std::memory_order_release);
    // Reads select one retained published envelope. This does not admit any
    // ordinary writer or retire an old native/source/model owner.
    a->selected_read.store(true, std::memory_order_release);
  }
  return SettleRoster(intent, RosterSettlementStage::kNativeMaps);
}

ProfileRosterResult ProfileManager::HandoffNativeRoster(
    const std::shared_ptr<ProfileRosterIntent>& intent,
    const ProfileRosterNativeCompletion& completion) {
  const auto a=roster_authority_;
  if (!intent || !completion.state_ || !a ||
      completion.state_->role!=ProfileRosterNativeCompletion::State::Role::kDataHandoff ||
      completion.state_->authority.lock()!=a || completion.state_->intent!=intent->state_ ||
      !a->binding || completion.state_->binding!=a->binding.get() ||
      completion.state_->nonce!=a->binding->nonce() || !a->open.load(std::memory_order_acquire) ||
      !(intent->state_->milestones.load(std::memory_order_acquire)&ProfileRosterIntent::State::kProtocol)) {
    ProfileRosterResult result;result.error=Stale();return result;
  }
  if (intent->data_handoff_complete()) { return intent->result(); }
  return SettleRoster(intent,RosterSettlementStage::kNativeDataHandoff);
}
ProfileRosterResult ProfileManager::FinalizeNativeRoster(
    const std::shared_ptr<ProfileRosterIntent>& intent,const ProfileRosterNativeCompletion& joined) {
  const auto a=roster_authority_;
  if (!intent || !joined.state_ || !a || joined.state_->authority.lock()!=a ||
      joined.state_->intent!=intent->state_ ||
      joined.state_->role!=ProfileRosterNativeCompletion::State::Role::kJoined ||
      !(intent->state_->milestones.load(std::memory_order_acquire)&ProfileRosterIntent::State::kProtocol) ||
      !a->binding || joined.state_->binding!=a->binding.get() ||
      joined.state_->nonce!=a->binding->nonce() || !a->open.load(std::memory_order_acquire)) {
    ProfileRosterResult result;result.error=Stale();return result;
  }
  // A genuine full join includes data closure for legacy callers. It does not
  // turn an unreturned callback/Task into an early data handoff receipt.
  if (!intent->data_handoff_complete()) {
    const auto result=SettleRoster(intent,RosterSettlementStage::kNativeDataHandoff);
    if (result.error) { return result; }
  }
  return RetireRosterOperation(intent);
}

ProfileRosterResult ProfileManager::SettleRoster(const std::shared_ptr<ProfileRosterIntent>& intent,
                                               RosterSettlementStage stage) {
  const bool native_path=stage!=RosterSettlementStage::kHostComplete;
  const bool handoff_data=stage!=RosterSettlementStage::kNativeMaps;
  auto use = account_commit_gate_->Acquire();
  if (!use) { auto r = intent->result(); r.error = Stale(); return r; }
  const auto a = roster_authority_;
  std::shared_ptr<ProfileRosterAuthority::Operation> op;
  std::shared_ptr<ProfileRosterAuthority::Envelope> selected;
  const auto changed=intent->result().changed;
  std::array<std::shared_ptr<const ProfileGpdPendingMutation>, 4> pending_models;
  // At most four old plus four unused requested rows. A nonnull observation
  // refuses immediately, but its native/storage holders outlive the PM lock.
  std::array<std::shared_ptr<const ProfileGpdPendingMutation>,8> retiring_pending_models;
  {
    std::unique_lock lock(profile_mount_mutex_, std::try_to_lock);
    if (!lock.owns_lock()) { auto r = intent->result(); r.error = Busy(); return r; }
    const auto phase = intent->state_->phase.load(std::memory_order_acquire);
    if (!a || !a->open.load(std::memory_order_acquire) || !a->pending ||
        a->pending->intent != intent || (a->binding && !native_path) ||
        (phase != ProfileRosterPhase::kPublished && phase != ProfileRosterPhase::kFinalizing)) {
      auto r = intent->result(); r.error = Stale(); return r;
    }
    op = a->pending;
    selected=intent->publication_outcome()==ProfileRosterPublicationOutcome::kCanceledEmpty
        ?op->canceled_empty:op->after;
    if(!selected){auto result=intent->result();result.error=Stale();return result;}
    // Requested resources stay owned even when none becomes selected. Actual
    // pending IO must finish before their final retirement, never be discarded.
    if (!op->maps_settled || (handoff_data && selected!=op->after)) {
      for (size_t slot = 0; slot != 4; ++slot) {
        if (!op->after->rows[slot].model) { continue; }
        std::error_code pending_error;
        pending_models[slot] = op->after->rows[slot].model->Pending(pending_error);
        if (pending_error || pending_models[slot]) {
          auto result = intent->result(); result.error = pending_error ? pending_error : Busy(); return result;
        }
      }
    }
    bool idle = false;
    if (!op->settling.compare_exchange_strong(idle, true, std::memory_order_acq_rel)) {
      auto r = intent->result(); r.error = Busy(); return r;
    }
    if (a->selected.load(std::memory_order_acquire) != selected.get() ||
        !ProfileRosterAuthority::LogicalCurrent(*selected)) {
      op->settling.store(false, std::memory_order_release);
      intent->state_->phase.store(ProfileRosterPhase::kUncertain, std::memory_order_release);
      a->diagnostic_phase.store(uint32_t(ProfileRosterPhase::kUncertain),std::memory_order_release);
      auto r = intent->result(); r.error = Stale(); return r;
    }
    if (handoff_data) {
      intent->state_->phase.store(ProfileRosterPhase::kFinalizing, std::memory_order_release);
      a->diagnostic_phase.store(uint32_t(ProfileRosterPhase::kFinalizing),std::memory_order_release);
    }
    if (!op->maps_settled) {
      const bool canceled=selected==op->canceled_empty;
      logged_profiles_.swap(canceled?op->empty_profiles:op->profiles);
      account_snapshots_.swap(canceled?op->empty_snapshots:op->snapshots);
      profile_sign_in_generations_.swap(canceled?op->empty_generations:op->generations);
      gpd_models_.swap(canceled?op->empty_models:op->models);
      auto publish_nodes = [](auto& current, auto& prepared) {
        for (auto it = prepared.begin(); it != prepared.end();) {
          auto next = std::next(it);
          const auto found = current.find(it->first);
          if (found == current.end()) { current.insert(prepared.extract(it)); }
          else { std::swap(found->second, it->second); }
          it = next;
        }
      };
      if(!canceled){
        publish_nodes(account_identities_, op->identities);
        publish_nodes(account_metadata_, op->metadata);
        publish_nodes(accounts_, op->accounts);
      } // canceled publication never applies prepared-request inventory data
      for (uint8_t slot = 0; slot != 4; ++slot) {
        const auto old_xuid = op->before->roster->slots()[slot].xuid;
        if (old_xuid && !gpd_models_.contains(old_xuid)) {
          op->displaced_identities[slot] = account_identities_.extract(old_xuid);
        }
      }
      account_publication_->Advance(); op->maps_settled = true;
      a->current = selected;
      a->DiagnosticCurrent(selected.get());
      intent->state_->milestones.fetch_or(ProfileRosterIntent::State::kMaps,
                                          std::memory_order_release);
    }
  }
  auto release = [&op](void*) { op->settling.store(false, std::memory_order_release); };
  std::unique_ptr<void, decltype(release)> serialized(reinterpret_cast<void*>(1), release);
  // Host effects and all resource release happen on the host lane, never at
  // native publisher or while holding PM/GCRC/native storage locks.
  if (!op->effects_applied) {
    for (uint8_t slot = 0; slot != 4; ++slot) {
      if (!changed[slot]) { continue; }
      const auto old = op->before->roster->slots()[slot].xuid;
      if (!op->removed[slot]) {
        if (old) { user_tracker_->RemoveUser(old); }
        op->removed[slot] = true;
      }
    }
    // Complete every removal before any addition: an A/B slot swap must not
    // remove B after the preceding slot already attempted to add that UID.
    for (uint8_t slot = 0; slot != 4; ++slot) {
      if (!changed[slot]) { continue; }
      const auto now = selected->roster->slots()[slot].xuid;
      if (!op->added[slot]) {
        if (now) { user_tracker_->TrackUser(now); }
        op->added[slot] = true;
      }
    }
    // A real title-entry effect belongs to each selected row, including the
    // initial same-row native alignment. UpdateSpaInfo may have been refused
    // while this Operation excluded ordinary writers. Capture one immutable
    // SPA/clock command here and retain its exact model-issued request/writer;
    // tracking insertion alone never proves that the durable effect finished.
    // No-SPA/system-app alignment remains a byte-preserving no-op. Unrelated
    // pending commands remain with their real owner (the pre-map check above),
    // and any later conflicting pending mutation makes this exact Submit busy.
    std::error_code effect_error;
    for (uint8_t slot = 0; slot != 4; ++slot) {
      if (!selected->roster->slots()[slot].xuid) { continue; }
      if (!op->title_prepared[slot]) {
        std::error_code prepare_error;
        auto command = user_tracker_->CapturePlayedTitleMutation(prepare_error);
        if (prepare_error) { if (!effect_error) { effect_error = prepare_error; } continue; }
        if (command) {
          const auto& row = selected->rows[slot];
          const auto volume = content_manager_.Volume(row.storage->source_id());
          const auto owner = content_manager_.ProfileIntents(row.storage->source_id());
          const auto recovery = volume && volume->profile_writable() ? volume->recovery() : nullptr;
          if (!owner || owner->source() != row.storage->source() || !recovery ||
              row.model->storage() != row.storage || row.storage->retiring()) {
            if (!effect_error) { effect_error = std::make_error_code(std::errc::no_such_device); }
            continue;
          }
          command->request_id = row.model->AllocateRequestId(prepare_error);
          if (prepare_error || !command->request_id) {
            if (!effect_error) { effect_error = prepare_error ? prepare_error : Busy(); }
            continue;
          }
          op->title_writers[slot] = {row.model, owner, recovery, volume->mutations()};
          op->title_effects[slot] = std::move(command);
        } else { op->title_done[slot] = true; }
        op->title_prepared[slot] = true;
      }
      if (!op->title_done[slot]) {
        const auto attempt = op->title_writers[slot].Submit(*op->title_effects[slot]);
        using State = ProfileGpdTransactionAttempt::State;
        if (!attempt.error && (attempt.state == State::kCommitted ||
            attempt.state == State::kAlreadyCommitted || attempt.state == State::kAlreadyUnlocked)) {
          op->title_done[slot] = true;
        } else if (!effect_error) { effect_error = attempt.error ? attempt.error : Busy(); }
      }
    }
    if (effect_error) { auto result = intent->result(); result.error = effect_error; return result; }
    if (op->desired.remember_slots) { UpdateConfig(0, 0); }
    if (!native_path && op->desired.notify_host_compatibility &&
        std::any_of(changed.begin(), changed.end(), [](bool b) { return b; })) {
      uint32_t used = 0;
      for (uint8_t slot = 0; slot != 4; ++slot) { if (selected->roster->slots()[slot].xuid) { used |= uint32_t(1) << slot; } }
      // Retains existing host compatibility policy only; not a newly recovered
      // 17559 payload encoder. Native original path sends its own actual event.
      kernel_state_->BroadcastNotification(kXNotificationSystemSignInChanged, used);
    }
    op->effects_applied = true;
  }
  // Canonical maps/effects now own the selected rows. Release only their
  // Account exclusions; changed old models/identities remain paused until the
  // independently authenticated data handoff. No visual readiness is implied.
  for (uint8_t slot = 0; slot != 4; ++slot) {
    if (!changed[slot]) {
      if(op->account_pauses[slot]){op->account_pauses[slot]->ReleaseKnownOutcome();op->account_pauses[slot].reset();}
      if(op->model_pauses[slot]){op->model_pauses[slot]->ReleaseKnownOutcome();op->model_pauses[slot].reset();}
    }
    // Unselected requested identities remain excluded until actual data handoff.
    if (selected==op->after && op->account_pauses[4 + slot]) {
      op->account_pauses[4 + slot]->ReleaseKnownOutcome(); op->account_pauses[4 + slot].reset();
    }
  }
  {
    std::lock_guard lock(profile_mount_mutex_);
    if (a->pending != op || a->current != selected ||
        a->selected.load(std::memory_order_acquire) != selected.get() ||
        !ProfileRosterAuthority::LogicalCurrent(*selected)) {
      auto result = intent->result(); result.error = Stale(); return result;
    }
    for (uint8_t slot = 0; slot != 4; ++slot) { preparing_slots_.reset(slot); preparing_generations_[slot] = 0; }
    // Unused requested rows still belong to entered native work. Keep their
    // Account/rebind exclusion until its real data handoff, despite empty maps.
    if(selected==op->after)
      for (const auto xuid : op->desired.xuids) { preparing_profiles_.erase(xuid); }
    a->selected_read.store(true, std::memory_order_release);
    a->ordinary.store(true, std::memory_order_release);
    intent->state_->milestones.fetch_or(ProfileRosterIntent::State::kRead |
        ProfileRosterIntent::State::kWriters, std::memory_order_release);
  }
  if (!handoff_data) { return intent->result(); }
  {
    std::lock_guard lock(profile_mount_mutex_);
    if (!a->open.load(std::memory_order_acquire) || a->pending!=op || a->current!=selected ||
        a->selected.load(std::memory_order_acquire)!=selected.get() ||
        !ProfileRosterAuthority::LogicalCurrent(*selected)) {
      auto result=intent->result();result.error=Stale();return result;
    }
    if (!op->old_owners_retired) {
      size_t pending_index=0;
      const auto data_closed=[&](const auto& row) {
        const bool selected_identity=row.identity && std::any_of(selected->rows.begin(),selected->rows.end(),
            [&](const auto& current){return current.identity==row.identity;});
        if (row.identity && !selected_identity && !row.identity->writes_drained()) { return false; }
        const bool selected_model=row.model && std::any_of(selected->rows.begin(),selected->rows.end(),
            [&](const auto& current){return current.model==row.model;});
        if (row.model && !selected_model) {
          std::error_code error;
          auto& pending=retiring_pending_models[pending_index++];
          pending=row.model->Pending(error);
          if (error || pending) { return false; }
        }
        return true;
      };
      for (const auto& row:op->before->rows) if (!data_closed(row)) {
        auto result=intent->result();result.error=Busy();return result;
      }
      if (selected!=op->after) for (const auto& row:op->after->rows) if (!data_closed(row)) {
        auto result=intent->result();result.error=Busy();return result;
      }
      std::array<const ProfileAccountIdentity*,8> identities{};
      std::array<const ProfileGpdModelAuthority*,8> models{};
      size_t identity_count=0,model_count=0;
      const auto retire_unselected=[&](const auto& row){
        if(row.identity && std::none_of(selected->rows.begin(),selected->rows.end(),
            [&](const auto& current){return current.identity==row.identity;}) &&
            std::find(identities.begin(),identities.begin()+identity_count,row.identity.get())==identities.begin()+identity_count){
          identities[identity_count++]=row.identity.get();row.identity->Retire();
        }
        if(row.model && std::none_of(selected->rows.begin(),selected->rows.end(),
            [&](const auto& current){return current.model==row.model;}) &&
            std::find(models.begin(),models.begin()+model_count,row.model.get())==models.begin()+model_count){
          models[model_count++]=row.model.get();retired_gpd_models_.push_back(row.model);row.model->Retire();
        }
      };
      for(const auto& old:op->before->rows)retire_unselected(old);
      if(selected!=op->after)for(const auto& unused:op->after->rows)retire_unselected(unused);
      op->old_owners_retired = true;
    }
  }
  for (auto& p : op->account_pauses) { if (p) { p->ReleaseKnownOutcome(); p.reset(); } }
  for (auto& p : op->model_pauses) { if (p) { p->ReleaseKnownOutcome(); p.reset(); } }
  {
    std::lock_guard lock(profile_mount_mutex_);
    if (!a->open.load(std::memory_order_acquire) || a->pending!=op || a->current!=selected ||
        a->selected.load(std::memory_order_acquire)!=selected.get()) {
      auto result=intent->result();result.error=Stale();return result;
    }
    for (auto& weak:a->tickets) {
      if (auto ticket=weak.lock();ticket &&
          (ticket->state_->fixed==op->before.get() || ticket->state_->operation==intent->state_->operation)) {
        ticket->state_->open.store(false,std::memory_order_release);
      }
    }
    for (uint8_t slot=0;slot!=4;++slot) {
      if (op->reserved_slots[slot]) { preparing_slots_.reset(slot);preparing_generations_[slot]=0; }
    }
    for (const auto xuid:op->desired.xuids) { preparing_profiles_.erase(xuid); }
    a->retiring.push_back(op); // capacity was reserved before native arm
    a->pending.reset();
    a->ordinary.store(true,std::memory_order_release);
    a->diagnostic_pending.store(false,std::memory_order_release);
    intent->state_->milestones.fetch_or(ProfileRosterIntent::State::kDataHandoff,std::memory_order_release);
    intent->state_->phase.store(ProfileRosterPhase::kFinalizing,std::memory_order_release);
    a->diagnostic_phase.store(uint32_t(ProfileRosterPhase::kFinalizing),std::memory_order_release);
  }
  if (stage==RosterSettlementStage::kNativeDataHandoff) { return intent->result(); }
  serialized.reset(); // Late cleanup takes the same operation serializer.
  return RetireRosterOperation(intent);
}
ProfileRosterResult ProfileManager::RetireRosterOperation(
    const std::shared_ptr<ProfileRosterIntent>& intent) {
  if (!intent) { ProfileRosterResult result;result.error=Stale();return result; }
  auto use=account_commit_gate_->Acquire();
  if (!use) { auto result=intent->result();result.error=Stale();return result; }
  const auto a=roster_authority_;
  std::shared_ptr<ProfileRosterAuthority::Operation> op;
  {
    std::unique_lock lock(profile_mount_mutex_,std::try_to_lock);
    if (!lock.owns_lock()) { auto result=intent->result();result.error=Busy();return result; }
    if (!a || !a->open.load(std::memory_order_acquire) || intent->state_->authority.lock()!=a ||
        !intent->data_handoff_complete()) { auto result=intent->result();result.error=Stale();return result; }
    if (intent->state_->phase.load(std::memory_order_acquire)==ProfileRosterPhase::kCommitted) { return intent->result(); }
    const auto found=std::find_if(a->retiring.begin(),a->retiring.end(),[&](const auto& retired){
      return retired->intent==intent && retired->intent->state_->operation==intent->state_->operation;
    });
    if (found==a->retiring.end()) { auto result=intent->result();result.error=Stale();return result; }
    op=*found;bool idle=false;
    if (!op->settling.compare_exchange_strong(idle,true,std::memory_order_acq_rel)) {
      auto result=intent->result();result.error=Busy();return result;
    }
  }
  auto release=[&op](void*){op->settling.store(false,std::memory_order_release);};
  std::unique_ptr<void,decltype(release)> serialized(reinterpret_cast<void*>(1),release);
  const auto changed=intent->result().changed;
  bool busy=false;
  for (uint8_t slot=0;slot!=4;++slot) {
    const auto& old=op->before->rows[slot];
    if (changed[slot] && old.profile && !op->old_mount_released[slot]) {
      if (!old.mount || !ReleaseRosterMount(old.mount)) { busy=true; }
      else { op->old_mount_released[slot]=true; }
    }
  }
  if (intent->publication_outcome()==ProfileRosterPublicationOutcome::kCanceledEmpty) {
    for (uint8_t slot=0;slot!=4;++slot) {
      if (!op->staged_mount[slot] || op->unused_mount_released[slot]) { continue; }
      const auto& unused=op->after->rows[slot];
      if (!unused.mount || !ReleaseRosterMount(unused.mount)) { busy=true; }
      else { op->unused_mount_released[slot]=true; }
    }
  }
  if (busy) { auto result=intent->result();result.error=Busy();return result; }
  {
    std::lock_guard lock(profile_mount_mutex_);
    const auto found=std::find(a->retiring.begin(),a->retiring.end(),op);
    if (found==a->retiring.end()) { auto result=intent->result();result.error=Stale();return result; }
    // Entered fixed scopes retain these exact envelopes until Leave. New
    // entries were revoked at handoff; following scopes never retarget to a
    // third roster. Actual owner destruction remains in the GCRC-aware reap.
    a->retired.push_back(op->before);
    if (intent->publication_outcome()==ProfileRosterPublicationOutcome::kCanceledEmpty) { a->retired.push_back(op->after); }
    a->retiring.erase(found); // Local op keeps resource destruction outside PM.
    intent->state_->milestones.fetch_or(ProfileRosterIntent::State::kRetired,std::memory_order_release);
    intent->state_->phase.store(ProfileRosterPhase::kCommitted,std::memory_order_release);
    // A successor may own current state. Never replay old Retire, maps, pauses,
    // reservations, or diagnostic phase after its publication.
    if (!a->pending && a->diagnostic_operation.load(std::memory_order_acquire)==intent->state_->operation) {
      a->diagnostic_phase.store(uint32_t(ProfileRosterPhase::kCommitted),std::memory_order_release);
    }
  }
  return intent->result();
}
std::shared_ptr<const ProfileAccountSnapshot> ProfileManager::RefreshRosterAccount(
    const std::shared_ptr<const ProfileAccountSnapshot>& snapshot) {
  // Called only inside actual PublishAccountCache's existing metadata lock,
  // after genuine durable input validation. Ordinary data does not advance a
  // roster epoch. Return the displaced owner for release after native guards.
  const auto a = roster_authority_;
  if (!a || !a->current || !snapshot) { return {}; }
  for (auto& row : a->current->rows) {
    if (row.identity == snapshot->identity()) {
      auto old = std::move(row.account); row.account = snapshot; return old;
    }
  }
  return {};
}
void ProfileManager::RetryRosterSettlement() {
  if (xe::global_critical_region::is_held_by_current_thread()) { return; }
  std::shared_ptr<ProfileRosterIntent> intent,retiring;
  bool native=false;
  {
    std::unique_lock lock(profile_mount_mutex_,std::try_to_lock);
    if (!lock.owns_lock() || !roster_authority_) { return; }
    const auto a=roster_authority_;native=bool(a->binding);
    if (a->pending) { intent=a->pending->intent; }
    if (!native && !a->retiring.empty()) { retiring=a->retiring.front()->intent; }
  }
  if (retiring) { RetireRosterOperation(retiring); }
  if (!intent) { return; }
  const auto phase=intent->result().phase;
  if (!native && phase==ProfileRosterPhase::kFinalizing) { SettleRoster(intent,RosterSettlementStage::kHostComplete); }
  else if (phase==ProfileRosterPhase::kCanceling ||
      ((phase==ProfileRosterPhase::kPreparing || phase==ProfileRosterPhase::kPrepared) &&
       intent->state_->preparation_cancel_requested.load(std::memory_order_acquire))) { CancelRoster(intent); }
}
} // namespace xe::kernel::xam
