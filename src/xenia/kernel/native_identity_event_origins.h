/** Xenia : Xbox 360 Emulator Research Project; BSD license, see LICENSE. */
#pragma once
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace xe::kernel { class NativeIdentityListenerBridge; }
namespace xe::kernel::notification_detail {
// Native17559 identity-event metadata only. No method grants native entry,
// profile publication, provider freshness or pixel authority. The bridge must
// authenticate actual code/root/domain/frame and prebind the true listener
// lifetime and whole consuming work BEFORE guest spinlocks. The critical
// enqueue/dequeue methods allocate nothing, wait for nothing, invoke no host
// callbacks and acquire no host mutex. Their caller owns the REAL common
// native listener-area lock; that lock serializes one listener's event slots.
// GetNext's return is deliberately not a provider-work completion method.
// The concrete Root owner serializes producer/work registration, normal
// completion, origin reaping and closure under its metadata mutex OUTSIDE
// guest locks. Each critical invocation already retains its supplied origin
// through that earlier producer/work owner. Passing a stale unretained origin
// is not an alternative registration API. No native bridge is provided here.
class NativeIdentityEventOrigins {
 public:
  static_assert(std::atomic<uint64_t>::is_always_lock_free);
  static_assert(std::atomic<uint32_t>::is_always_lock_free);
  static_assert(std::atomic<bool>::is_always_lock_free);
  static constexpr size_t kOrigins = 32, kListeners = 64;
  static constexpr size_t kEventsPerListener = 16, kWorks = 64;
  static constexpr size_t kOriginsPerWork = 8;
  struct Root {
    uint64_t kernel = 0, domain = 0, generation = 0;
    bool valid() const { return kernel && domain && generation; }
    bool operator==(const Root&) const = default;
  };
  // The public factual producer remains unjoined. Only the private concrete
  // bridge can admit an earlier owner-created prepared source receipt. No
  // mutable Origin enum upgrades a stored origin. Neither kind grants readiness.
  enum class Provenance : uint8_t { kUnjoinedNativeRequest, kPreparedNativeRequest };
  struct Origin {
    Root root;
    uint64_t serial = 0;
    uint32_t slot = 0;
    Provenance provenance = Provenance::kUnjoinedNativeRequest;
    bool operator==(const Origin&) const = default;
  };
  struct Listener {
    Root root;
    uint64_t serial = 0;
    uint32_t slot = 0, object = 0;
    bool operator==(const Listener&) const = default;
  };
  // Taskless ownership is explicit: the concrete owner pins the real SYSTEM
  // XThread/code/domain/frame through its bounded body's own return. It grants
  // no Task generation/reference/descendant authority. Task rules stay exact.
  enum class WorkOwnerKind : uint32_t { kTask=0, kSystemThreadFrame=1 };
  struct WorkKey {
    Root root;
    uint64_t task_allocation_generation = 0, invocation = 0;
    uint32_t thread = 0, pcr = 0, task = 0, sp = 0, lr = 0, function = 0;
    WorkOwnerKind owner_kind = WorkOwnerKind::kTask;
    bool valid() const {
      const bool owner=owner_kind==WorkOwnerKind::kTask ?
          task_allocation_generation && task>=0x1000 && !(task&3) :
          owner_kind==WorkOwnerKind::kSystemThreadFrame && !task_allocation_generation && !task;
      return root.valid() && owner && invocation && thread && pcr &&
             sp && !(sp & 15) && lr >= 4 && !(lr & 3) && function >= 4 && !(function & 3);
    }
    bool operator==(const WorkKey&) const = default;
  };
  struct Work {
    Root root;
    uint64_t serial = 0;
    uint32_t slot = 0;
    bool operator==(const Work&) const = default;
  };
  // An existing queue pin transferred into one preallocated actual API cell.
  // This is delivery only, never a whole consumer/read/Task completion receipt.
  struct ScalarDelivery { Origin origin;uint64_t enqueue_sequence=0; };
  struct Dequeued {
    bool native_success = false, origin_known = false, retained_by_work = false;
    uint64_t enqueue_sequence = 0;
    Origin origin;
    ScalarDelivery scalar_delivery;
    // An existing work's earlier entry origin is never changed by dequeue.
    // Multiple source origins are retained as separate dependencies, not
    // collapsed into a current-user/current-epoch label.
  };
  struct WorkFacts {
    uint64_t entry_origin = 0;
    std::array<uint64_t, kOriginsPerWork> dequeued_origins{};
    uint32_t count = 0;
    bool unknown_input = false;
  };

  explicit NativeIdentityEventOrigins(Root root) : root_(root) {
    if (!root.valid()) {
      lost_.store(true, std::memory_order_relaxed);
      RecordFirstFailure(__LINE__,1);
    }
  }
  NativeIdentityEventOrigins(const NativeIdentityEventOrigins&) = delete;
  NativeIdentityEventOrigins& operator=(const NativeIdentityEventOrigins&) = delete;

  // This factual factory mints ONLY kUnjoinedNativeRequest, never a joined
  // sign-in generation or refresh permission. Register an ALREADY retained
  // producer/request and its native Task
  // descendants, outside guest locks. The opaque Root owner keeps the actual
  // Account/device/input/domain resources in its parallel retained slot. This
  // is a distinct origin serial, never the current notification epoch/XUID.
  std::optional<Origin> BeginProducer() {
    return BeginProducerWithProvenance(Provenance::kUnjoinedNativeRequest);
  }
 private:
  friend class ::xe::kernel::NativeIdentityListenerBridge;
  std::optional<Origin> BeginProducerWithProvenance(Provenance provenance) {
    if (closed_.load(std::memory_order_acquire) || lost()) return {};
    const uint64_t id = Next(next_origin_);
    if (!id) return {};
    for (uint32_t i = 0; i < kOrigins; ++i) {
      uint64_t empty = 0;
      if (!origins_[i].serial.compare_exchange_strong(empty, kReserved)) continue;
      origins_[i].references.store(1, std::memory_order_relaxed);
      origins_[i].producer.store(true, std::memory_order_relaxed);
      origins_[i].orphan_seen.store(false, std::memory_order_relaxed);
      origins_[i].provenance.store(uint32_t(provenance), std::memory_order_relaxed);
      origins_[i].serial.store(id, std::memory_order_release);
      if (closed_.load(std::memory_order_acquire)) {
        origins_[i].producer.store(false, std::memory_order_relaxed);
        origins_[i].references.store(0, std::memory_order_relaxed);
        origins_[i].serial.store(0, std::memory_order_release);
        return {};
      }
      return Origin{root_, id, i, provenance};
    }
    Lose(__LINE__,2); return {};
  }
 public:
  // Means the genuine retained producer AND ALL scheduled/rescheduled/woken
  // descendants actually completed/closed, with transferred event/work
  // references accounted; never DC628 copy exit, public API or Schedule return.
  // Queue/work references remain independently retained afterwards.
  bool FinishProducerNormally(Origin origin) {
    auto* value = Find(origin);
    if (!value || !value->producer.exchange(false)) { Lose(__LINE__,3); return false; }
    return Release(origin);
  }
  // Outside guest locks. Returns only refcount-zero IDs for the concrete owner
  // to release outside its own mutex/guest locks. It never destroys resources.
  size_t TakeCompletedOrigins(std::span<Origin> output) {
    if (lost()) return 0;
    size_t count = 0;
    for (uint32_t i = 0; i < kOrigins && count < output.size(); ++i) {
      const uint64_t id = origins_[i].serial.load(std::memory_order_acquire);
      if (!id || id == kReserved || origins_[i].producer.load() ||
          origins_[i].references.load(std::memory_order_acquire)) continue;
      const auto provenance = Provenance(origins_[i].provenance.load());
      uint64_t expected = id;
      if (origins_[i].serial.compare_exchange_strong(expected, 0))
        output[count++] = Origin{root_, id, i, provenance};
    }
    return count;
  }

  // Original8498 entry occurs before native insertion. A genuine destructor
  // normal return is the retirement seam: it has finished registry unlink and
  // area destruction, while native object-delete still retains the object.
  // Destructor ENTRY is too early: a broadcast may still own registry lock.
  std::optional<Listener> RegisterListener(uint32_t actual_object) {
    if (lost() || !Address(actual_object)) return {};
    for (const auto& value : listeners_)
      if (value.serial.load(std::memory_order_acquire) &&
          value.active.load(std::memory_order_acquire) &&
          value.object.load(std::memory_order_acquire) == actual_object) {
        Lose(__LINE__,4); return {};
      }
    const uint64_t id = Next(next_listener_);
    if (!id) return {};
    for (uint32_t i = 0; i < kListeners; ++i) {
      uint64_t empty = 0;
      auto& value = listeners_[i];
      if (!value.serial.compare_exchange_strong(empty, kReserved)) continue;
      value.events = {};
      value.object.store(actual_object, std::memory_order_relaxed);
      value.active.store(true, std::memory_order_relaxed);
      value.serial.store(id, std::memory_order_release);
      return Listener{root_, id, i, actual_object};
    }
    Lose(__LINE__,4); return {};
  }
  std::optional<Listener> FindListener(uint32_t actual_object) const {
    if (lost()) return {};
    for (uint32_t i = 0; i < kListeners; ++i) {
      const auto& value = listeners_[i];
      const uint64_t id = value.serial.load(std::memory_order_acquire);
      if (id && id != kReserved && value.active.load(std::memory_order_acquire) &&
          value.object.load(std::memory_order_acquire) == actual_object &&
          value.serial.load(std::memory_order_acquire) == id)
        return Listener{root_, id, i, actual_object};
    }
    return {};
  }
  bool RetireListenerAfterNativeDestruction(Listener listener) {
    auto* value = Find(listener);
    if (!value) { Lose(__LINE__,5); return false; }
    value->active.store(false, std::memory_order_release);
    bool orphan = false;
    for (auto& entry : value->events) {
      if (entry.kind == Event::Kind::kOrphan) { orphan = true; continue; }
      if (entry.kind == Event::Kind::kQueued && entry.origin.serial)
        Release(entry.origin);
      entry = {};
    }
    // A consumed event without an earlier work owner is still live/uncertain;
    // destroying its listener does not prove its downstream callback unwound.
    if (!orphan) value->serial.store(0, std::memory_order_release);
    return !lost();
  }

  // These two calls occur ONLY at genuine native area-lock linearization.
  // normalized_id is the native area's ID (r29 on8328 unlock), not a host ID
  // normalized after GetNext or a per-listener FIFO ordinal.
  bool ReplaceQueued(Listener listener, uint32_t area, uint32_t normalized_id,
                     uint32_t actual_data, std::optional<Origin> origin,
                     bool public_identity_hint=false) {
    auto* value = Find(listener);
    if (!value || !Address(area) || !normalized_id || lost()) { Lose(__LINE__,6); return false; }
    Event* destination = nullptr;
    for (auto& entry : value->events)
      if (entry.kind == Event::Kind::kQueued && entry.area == area &&
          entry.id == normalized_id) { destination = &entry; break; }
    if (!destination)
      for (auto& entry : value->events) if (entry.kind == Event::Kind::kEmpty) {
        destination = &entry; break;
      }
    const uint64_t sequence = Next(next_enqueue_);
    if (!destination || !sequence) { Lose(__LINE__,6); return false; }
    if (origin && !Pin(*origin)) { Lose(__LINE__,6); return false; }
    const Origin old = destination->origin;
    // ALWAYS replace origin, even when actual_data is numerically identical.
    // An unowned internal publication overwrites the prior known origin.
    *destination = Event{Event::Kind::kQueued, area, normalized_id, actual_data,
                         sequence, origin.value_or(Origin{}),
                         public_identity_hint && actual_data<=0xFu};
    if (old.serial) Release(old);
    return !lost();
  }
  Dequeued TransferSuccessfulDequeue(Listener listener, uint32_t area,
                                    uint32_t normalized_id,
                                    std::optional<uint32_t> actual_data,
                                    std::optional<Work> consuming_work,
                                    bool allow_scalar_delivery=false) {
    Dequeued result;
    result.native_success = true;
    auto* value = Find(listener);
    if (!value || !Address(area) || !normalized_id || lost()) { Lose(__LINE__,7); return result; }
    for (auto& entry : value->events) {
      if (entry.kind != Event::Kind::kQueued || entry.area != area ||
          entry.id != normalized_id) continue;
      if (actual_data && *actual_data != entry.data) { Lose(__LINE__,7); return result; }
      result.enqueue_sequence = entry.sequence;
      result.origin = entry.origin;
      result.origin_known = bool(entry.origin.serial);
      WorkEntry* work = consuming_work ? Find(*consuming_work) : nullptr;
      if (!entry.origin.serial) {
        if (work) work->unknown_input = true;
        entry = {};
        return result;
      }
      if(!work && !consuming_work && allow_scalar_delivery && entry.identity_hint){
        // Transfer the SAME pin; no Release/Pin gap and no owning resource
        // operation under the native area lock. A real whole Work wins above.
        result.scalar_delivery={entry.origin,entry.sequence};entry={};return result;
      }
      if (!work) {
        // The native queue did consume this entry. Preserve its prior origin
        // as an orphan instead of releasing it at GetNext/API return.
        // Sticky for this exact origin generation: outer GetNext return or
        // listener destruction cannot prove this unowned consumer unwound.
        auto* origin=Find(entry.origin);
        if(!origin){Lose(__LINE__,7);return result;}
        origin->orphan_seen.store(true,std::memory_order_release);
        entry.kind = Event::Kind::kOrphan;
        return result;
      }
      for (uint32_t i = 0; i < work->count; ++i)
        if (work->dequeued[i] == entry.origin) {
          Release(entry.origin);  // Work already owns an independent reference.
          entry = {};
          result.retained_by_work = true;
          return result;
        }
      if (work->count == kOriginsPerWork) { Lose(__LINE__,7); return result; }
      work->dequeued[work->count++] = entry.origin;  // Transfer, no refcount gap.
      entry = {};
      result.retained_by_work = true;
      return result;
    }
    // Native initial/unobserved entries carry no known source. Never adopt
    // the current producer, user or generation to fill that missing origin.
    if (consuming_work) if (auto* work = Find(*consuming_work)) work->unknown_input = true;
    return result;
  }

  // Called before the whole verified consuming provider body snapshots data.
  // Its real native frame/Task owner remains retained externally. It must NOT
  // be created at GetNext return or FE ingress. The optional entry origin is
  // copied from an earlier retained request/dispatch, never reminted here.
  std::optional<Work> BeginWork(WorkKey key, std::optional<Origin> entry_origin,
                                std::optional<Work> earlier_parent = {}) {
    if (!key.valid() || key.root != root_ || lost() ||
        (closed_.load(std::memory_order_acquire) && !entry_origin)) return {};
    for (const auto& value : works_) {
      const uint64_t serial = value.serial.load(std::memory_order_acquire);
      if (!serial || serial == kReserved) continue;
      const auto old = value.key.Load();
      const bool reused_task = old.owner_kind==WorkOwnerKind::kTask &&
          key.owner_kind==WorkOwnerKind::kTask && old.task == key.task &&
          old.task_allocation_generation != key.task_allocation_generation;
      const bool equal_frame = old.thread == key.thread && old.pcr == key.pcr &&
          old.task == key.task && old.sp == key.sp && old.lr == key.lr &&
          old.function == key.function;
      // A new invocation number cannot explain an abandoned equal frame, nor
      // can a recycled native task address retire its earlier live work.
      if (old == key || reused_task || equal_frame) { Lose(__LINE__,8); return {}; }
    }
    WorkEntry* parent = earlier_parent ? Find(*earlier_parent) : nullptr;
    if (earlier_parent) {
      const auto before = parent ? parent->key.Load() : WorkKey{};
      // The bridge binds the exact earlier owner, before this native child
      // enters. Same-address tasks, unrelated threads and later equal frames
      // never become a synchronous consumption parent.
      if (!parent || before.root != key.root || before.owner_kind != key.owner_kind ||
          before.task_allocation_generation != key.task_allocation_generation ||
          before.thread != key.thread || before.pcr != key.pcr ||
          before.task != key.task || before.sp <= key.sp ||
          before.invocation >= key.invocation || parent->children == UINT32_MAX) {
        Lose(__LINE__,8); return {};
      }
    }
    const uint64_t id = Next(next_work_);
    if (!id) return {};
    if (entry_origin && !Pin(*entry_origin)) { Lose(__LINE__,8); return {}; }
    for (uint32_t i = 0; i < kWorks; ++i) {
      auto& value = works_[i];
      uint64_t empty = 0;
      if (!value.serial.compare_exchange_strong(empty, kReserved)) continue;
      value.key.Store(key);
      value.entry = entry_origin.value_or(Origin{});
      value.dequeued = {};
      value.count = 0;
      value.unknown_input = !entry_origin;
      value.parent = earlier_parent;
      value.children = 0;
      if (parent) ++parent->children;
      value.serial.store(id, std::memory_order_release);
      work_count_.fetch_add(1, std::memory_order_relaxed);
      return Work{root_, id, i};
    }
    if (entry_origin) Release(*entry_origin);
    Lose(__LINE__,8); return {};
  }
  // Read on the same prebound consuming thread, OR while the concrete
  // bridge owns its metadata mutex and has excluded EVERY active fast cell.
  // At that latter cut all mutable dependency writers are excluded: fast
  // transfers require a live cell, host Begin/Finish own the same mutex.
  std::optional<WorkFacts> Facts(Work work) const {
    const auto* value = Find(work);
    if (!value || lost()) return {};
    WorkFacts facts;
    facts.entry_origin = value->entry.serial;
    facts.count = value->count;
    facts.unknown_input = value->unknown_input;
    for (uint32_t i = 0; i < value->count; ++i) facts.dequeued_origins[i] = value->dequeued[i].serial;
    return facts;
  }
  // Only the genuine whole provider's matched NORMAL own-return may call.
  // The bound work belongs to its one native thread; Finish cannot race that
  // thread's dequeue. Stop does not disable genuine completion/drain.
  bool FinishWorkNormally(Work work, WorkKey actual_return) {
    auto* value = Find(work);
    if (!value || value->key.Load() != actual_return || value->children || lost()) {
      Lose(__LINE__,9); return false;
    }
    WorkEntry* parent = value->parent ? Find(*value->parent) : nullptr;
    if (value->parent) {
      if (!parent || !parent->children) { Lose(__LINE__,9); return false; }
      // Preflight every distinct dependency and both retained sets before
      // changing either work. Entry provenance is not a dequeue receipt.
      uint32_t additions = 0;
      for (uint32_t i = 0; i < value->count; ++i) {
        if (!Find(value->dequeued[i])) { Lose(__LINE__,9); return false; }
        bool duplicate = false;
        for (uint32_t j = 0; j < parent->count; ++j)
          if (parent->dequeued[j] == value->dequeued[i]) duplicate = true;
        if (!duplicate) ++additions;
      }
      if (parent->count + additions > kOriginsPerWork) { Lose(__LINE__,9); return false; }
      for (uint32_t j = 0; j < parent->count; ++j)
        if (!Find(parent->dequeued[j])) { Lose(__LINE__,9); return false; }
      // Transfer the child's actual native dequeue pins directly, without a
      // release/reacquire gap. Dedup drops only the child's independent pin.
      for (uint32_t i = 0; i < value->count; ++i) {
        bool duplicate = false;
        for (uint32_t j = 0; j < parent->count; ++j)
          if (parent->dequeued[j] == value->dequeued[i]) duplicate = true;
        if (duplicate) Release(value->dequeued[i]);
        else parent->dequeued[parent->count++] = value->dequeued[i];
      }
      parent->unknown_input = parent->unknown_input || value->unknown_input;
      --parent->children;
    } else {
      for (uint32_t i = 0; i < value->count; ++i) Release(value->dequeued[i]);
    }
    if (value->entry.serial) Release(value->entry);
    value->entry = {};
    value->dequeued = {};
    value->count = 0;
    value->parent.reset();
    value->serial.store(0, std::memory_order_release);
    work_count_.fetch_sub(1, std::memory_order_relaxed);
    return !lost();
  }
  // Missing/unrecognized return or ambiguous native lifetime never releases.
  void Lose(uint32_t line=0,uint32_t reason=12) {
    // Publish the original first site BEFORE a lost observer can emit a later
    // external/outer symptom. The original lost store still occurs once.
    RecordFirstFailure(line,reason);
    lost_.store(true, std::memory_order_release);
  }
  // Reason12 is an external Bridge loss; internal sites carry this header's
  // own line. One atomic scalar survives every later bridge/outer symptom.
  uint64_t FirstFailure() const noexcept { return first_failure_.load(std::memory_order_relaxed); }
  bool lost() const { return lost_.load(std::memory_order_acquire); }
  void CloseNewProducers() { closed_.store(true, std::memory_order_release); }
  bool owned_empty() const {
    if (lost() || work_count_.load(std::memory_order_acquire)) return false;
    // The concrete owner holds its registration/completion/closure mutex.
    // Critical transfers never create a reference from zero: their earlier
    // source or work owner remains retained throughout the native invocation.
    for (const auto& origin : origins_)
      if (origin.references.load(std::memory_order_acquire)) return false;
    return true;
  }
 private:
  // Bridge-only matched actual API return. Never touches native Event arrays,
  // re-pins an origin, or clears this generation's earlier orphan history.
  bool FinishScalarDelivery(ScalarDelivery& delivery) {
    if(!delivery.origin.serial || !delivery.enqueue_sequence || lost() || !Find(delivery.origin)){
      Lose(__LINE__,13);return false;
    }
    if(!Release(delivery.origin))return false;
    delivery={};return true;
  }
  static constexpr uint64_t kReserved = UINT64_MAX;
  static bool Address(uint32_t value) { return value >= 0x1000 && !(value & 3); }
  uint64_t Next(std::atomic<uint64_t>& counter) {
    const uint64_t value = counter.fetch_add(1, std::memory_order_relaxed);
    if (!value || value == kReserved) { Lose(__LINE__,10); return 0; }
    return value;
  }
  struct OriginEntry {
    std::atomic<uint64_t> serial{0};
    std::atomic<uint32_t> references{0};
    std::atomic<bool> producer{false};
    std::atomic<bool> orphan_seen{false};
    std::atomic<uint32_t> provenance{uint32_t(Provenance::kUnjoinedNativeRequest)};
  };
  struct Event {
    enum class Kind { kEmpty, kQueued, kOrphan };
    Kind kind = Kind::kEmpty;
    uint32_t area = 0, id = 0, data = 0;
    uint64_t sequence = 0;
    Origin origin;
    bool identity_hint=false; // original public A with mask0..F, never normalized-ID guessing
  };
  struct ListenerEntry {
    std::atomic<uint64_t> serial{0};
    std::atomic<uint32_t> object{0};
    std::atomic<bool> active{false};
    std::array<Event, kEventsPerListener> events{};
  };
  // Atomic keys allow outside-critical duplicate detection while different
  // threads register/finish their separate prebound work slots. The mutable
  // origin set itself is owned by that work's one actual native thread.
  struct AtomicWorkKey {
    std::atomic<uint64_t> kernel{0}, domain{0}, generation{0}, task_generation{0}, invocation{0};
    std::atomic<uint32_t> thread{0}, pcr{0}, task{0}, sp{0}, lr{0}, function{0}, owner_kind{0};
    void Store(const WorkKey& key) {
      kernel.store(key.root.kernel); domain.store(key.root.domain); generation.store(key.root.generation);
      task_generation.store(key.task_allocation_generation); invocation.store(key.invocation);
      thread.store(key.thread); pcr.store(key.pcr);
      task.store(key.task); sp.store(key.sp); lr.store(key.lr); function.store(key.function);
      owner_kind.store(uint32_t(key.owner_kind));
    }
    WorkKey Load() const {
      return {{kernel.load(), domain.load(), generation.load()}, task_generation.load(), invocation.load(),
              thread.load(), pcr.load(), task.load(), sp.load(), lr.load(), function.load(),
              WorkOwnerKind(owner_kind.load())};
    }
  };
  struct WorkEntry {
    std::atomic<uint64_t> serial{0};
    AtomicWorkKey key;
    Origin entry;
    std::array<Origin, kOriginsPerWork> dequeued{};
    uint32_t count = 0, children = 0;
    bool unknown_input = false;
    std::optional<Work> parent;
  };
  OriginEntry* Find(Origin origin) {
    if (origin.root != root_ || !origin.serial || origin.serial == kReserved || origin.slot >= kOrigins ||
        (origin.provenance != Provenance::kUnjoinedNativeRequest &&
         origin.provenance != Provenance::kPreparedNativeRequest)) return nullptr;
    auto& value = origins_[origin.slot];
    return value.serial.load(std::memory_order_acquire) == origin.serial &&
                   value.provenance.load(std::memory_order_acquire) == uint32_t(origin.provenance)
               ? &value : nullptr;
  }
  ListenerEntry* Find(Listener listener) {
    if (listener.root != root_ || !listener.serial || listener.serial == kReserved || listener.slot >= kListeners) return nullptr;
    auto& value = listeners_[listener.slot];
    return value.serial.load(std::memory_order_acquire) == listener.serial &&
                   value.active.load(std::memory_order_acquire) &&
                   value.object.load(std::memory_order_acquire) == listener.object ? &value : nullptr;
  }
  WorkEntry* Find(Work work) {
    if (work.root != root_ || !work.serial || work.serial == kReserved || work.slot >= kWorks) return nullptr;
    auto& value = works_[work.slot];
    return value.serial.load(std::memory_order_acquire) == work.serial ? &value : nullptr;
  }
  const WorkEntry* Find(Work work) const {
    return const_cast<NativeIdentityEventOrigins*>(this)->Find(work);
  }
  bool Pin(Origin origin) {
    auto* value = Find(origin);
    if (!value) return false;
    uint32_t references = value->references.load(std::memory_order_acquire);
    for (size_t attempt = 0; references && references != UINT32_MAX &&
                             attempt < kWorks + kListeners; ++attempt) {
      if (value->serial.load(std::memory_order_acquire) != origin.serial) return false;
      if (value->references.compare_exchange_weak(references, references + 1)) return true;
    }
    return false;
  }
  bool Release(Origin origin) {
    auto* value = Find(origin);
    if (!value) { Lose(__LINE__,11); return false; }
    uint32_t references = value->references.load(std::memory_order_acquire);
    for (size_t attempt = 0; references && attempt < kWorks + kListeners; ++attempt) {
      if (value->references.compare_exchange_weak(references, references - 1)) return true;
    }
    Lose(__LINE__,11); return false;
  }
  void RecordFirstFailure(uint32_t line,uint32_t reason) noexcept {
    uint64_t empty=0;
    first_failure_.compare_exchange_strong(empty,(uint64_t(reason)<<32)|line,
                                          std::memory_order_relaxed);
  }
  const Root root_;
  std::atomic<uint64_t> first_failure_{0};
  std::array<OriginEntry, kOrigins> origins_{};
  std::array<ListenerEntry, kListeners> listeners_{};
  std::array<WorkEntry, kWorks> works_{};
  std::atomic<uint64_t> next_origin_{1}, next_listener_{1}, next_work_{1}, next_enqueue_{1};
  std::atomic<uint32_t> work_count_{0};
  std::atomic<bool> closed_{false}, lost_{false};
};
}  // namespace xe::kernel::notification_detail
