// Metadata protocol used by the native observer. It grants no GPU/input
// authority.
#pragma once
#include <array>
#include <cstdint>
#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include "xenia/kernel/util/native_wait_snapshot.h"
#include "xenia/base/physical_range_leases.h"
#include "xenia/gpu/gpu_completion.h"
namespace xe::kernel::notification_detail {
struct ModalSnapshot {
  bool known = false;
  uint64_t sequence = 0, foreground = 0;
  bool operator==(const ModalSnapshot&) const = default;
};
struct FrameSample {
  memory::PhysicalAllocationOwner owner;
  uint64_t domain = 0, root_generation = 0, foreground = 0;
  uint32_t thread = 0, task = 0, frame_sp = 0, counter = 0;
  ModalSnapshot modal;
  bool valid() const;
  bool operator==(const FrameSample&) const = default;
};
struct FrameDecision {
  enum class Kind { kEligible, kInactive, kClear };
  Kind kind;
  FrameSample frame;
  uint64_t serial = 0, revision = 0;
  std::optional<gpu::NativeFrameSubmissionIdentity> source;
};
class FrameProtocol {
 public:
  explicit FrameProtocol(memory::PhysicalAllocationOwner owner, uint64_t domain,
                         uint64_t root_generation);
  bool Arm(const FrameSample&);
  bool ObserveDraw(const FrameSample&,
                   std::optional<std::array<uint8_t, 0x58>> display_info = {});
  std::optional<uint64_t> PeekControl(const FrameSample&) const;
  struct DrawSnapshot {
    uint64_t serial;
    std::array<uint8_t, 0x58> display_information;
  };
  // Read-only same-arm metadata; never visibility, Source or GPU authority.
  std::optional<DrawSnapshot> PeekDraw(const FrameSample&) const;
  std::optional<FrameDecision> FinishSource(
      const FrameSample&, const gpu::NativeFrameSubmissionIdentity&,
      uint64_t actual_cached_modal_r15, uint64_t actual_overlay_r16);
  std::optional<FrameDecision> FinishClear(const FrameSample&);
  void ModalTransition(ModalSnapshot);
  void CancelFrame(const FrameSample&);
  void BeginStop();
  bool accepting() const { return accepting_; }
  bool armed() const { return bool(arm_); }

 private:
  bool Owns(const FrameSample&) const;
  bool SyncModal(ModalSnapshot);
  std::optional<uint64_t> NextRevision();
  memory::PhysicalAllocationOwner owner_;
  uint64_t domain_, root_generation_, serial_ = 0, revision_ = 0;
  struct ArmState {
    FrameSample frame;
    uint64_t serial;
    bool drew = false;
    std::optional<std::array<uint8_t, 0x58>> display_information;
  };
  std::optional<ArmState> arm_;
  std::optional<ModalSnapshot> modal_;
  bool accepting_ = true;
};

// One function-specific diagnostic slot, not native object/UI authority. The
// observer supplies its existing mutex. No lease/pointer is retained here.
inline constexpr uint32_t kTextSenderAddress = 0x817C8868;
inline constexpr uint32_t kTextSenderCaller = 0x816AA91C;
struct TextSenderIdentity {
  uint64_t kernel = 0, domain = 0, root_generation = 0, foreground = 0;
  uint32_t thread = 0, pcr = 0, sp = 0, lr = 0;
  bool valid() const {
    return kernel && domain && root_generation && foreground && thread &&
           pcr && sp >= 0xB0 && !(sp & 15) && lr == kTextSenderCaller;
  }
  bool operator==(const TextSenderIdentity&) const = default;
};
struct TextSenderText {
  std::array<uint16_t, 256> units{};
  uint16_t count = 0;
  bool terminated = false, read_failed = false;
};
struct TextSenderEntry {
  TextSenderIdentity identity;
  uint64_t handle = 0, raw_text_pointer = 0;
  uint32_t text_pointer = 0, task = 0;
  bool task_known = false, text_pointer_known = false;
  TextSenderText text;
};
class TextSenderDiagnostic {
 public:
  bool Claim(TextSenderEntry entry) {
    if (stopped_ || !entry.identity.valid()) { return false; }
    // SP/LR identity is not an invocation serial. A new authenticated entry
    // while pending may follow an abandoned call (or be nested), so no later
    // return can safely complete the old slot. Stop only this diagnostic.
    if (pending_) { Stop(); return false; }
    if (claimed_) { return false; }
    claimed_ = true;
    pending_ = std::move(entry);
    return true;
  }
  void RecordEntry(TextSenderEntry entry) {
    if (!stopped_ && pending_ && pending_->identity == entry.identity) {
      pending_ = std::move(entry);
    }
  }
  std::optional<TextSenderEntry> Complete(const TextSenderIdentity& identity) {
    if (stopped_ || !pending_ || pending_->identity != identity) { return {}; }
    return std::exchange(pending_, {});
  }
  bool pending() const { return bool(pending_); }
  bool claimed() const { return claimed_; }
  bool Stop() {
    const bool pending = bool(pending_);
    stopped_ = true;
    pending_.reset();
    return pending;
  }
 private:
  bool claimed_ = false, stopped_ = false;
  std::optional<TextSenderEntry> pending_;
};

inline TextSenderText ReadTextSenderText(Memory& memory, uint32_t address) {
  TextSenderText result;
  // Read one unit at a time so a NUL just before an unmapped page succeeds
  // without reading the next page. The RAM-only reader invokes no provider.
  for (size_t i = 0; i != result.units.size(); ++i) {
    std::array<uint8_t, 2> bytes;
    if (!address || uint64_t(address) + i * 2 + 2 > 0x100000000ull ||
        !internal::ReadNativeWaitSnapshot(
            memory, address + uint32_t(i * 2), bytes)) {
      result.read_failed = true;
      return result;
    }
    const uint16_t unit = uint16_t(bytes[0]) << 8 | bytes[1];
    if (!unit) {
      result.terminated = true;
      return result;
    }
    result.units[result.count++] = unit;
  }
  return result;  // Bounded unterminated text, never a fabricated terminator.
}
struct TextSenderMessage {
  bool readable = false, matches = false;
  uint32_t header_bytes = 0, id = 0, handled = 0, payload_bytes = 0;
  uint32_t payload = 0, text_pointer = 0;
};
inline TextSenderMessage ReadTextSenderMessage(
    Memory& memory, const TextSenderEntry& entry) {
  TextSenderMessage result;
  if (!entry.identity.valid()) { return result; }
  // The sender's real B0 frame has message+50/payload+70. The hook runs
  // before its caller resumes, after SP has already been restored.
  const uint32_t frame = entry.identity.sp - 0xB0;
  std::array<uint8_t, 0x48> bytes;
  if (!internal::ReadNativeWaitSnapshot(memory, frame + 0x50, bytes)) {
    return result;
  }
  auto word = [&](size_t at) {
    return uint32_t(bytes[at]) << 24 | uint32_t(bytes[at + 1]) << 16 |
           uint32_t(bytes[at + 2]) << 8 | bytes[at + 3];
  };
  result.readable = true;
  // Native initializer818062A8 stores size at+0 and ID at+4; the
  // attachment helper817E16A8 stores payload extent at+C and pointer at+10.
  result.header_bytes = word(0);
  result.id = word(4);
  result.handled = word(8);
  result.payload_bytes = word(0xC);
  result.payload = word(0x10);
  result.text_pointer = word(0x2C);
  result.matches = entry.text_pointer_known && result.header_bytes == 0x14 &&
                   result.id == 0x7E0 && result.payload_bytes == 0x28 &&
                   result.payload == frame + 0x70 &&
                   !word(0x20) && !word(0x24) && !word(0x28) &&
                   result.text_pointer == entry.text_pointer;
  return result;
}

// Values copied at the actual common measurement entry. Matching caption text
// is diagnostic correlation only, never proof of a notification receiver.
inline constexpr uint32_t kTextMeasureAddress = 0x817BEBD0;
inline constexpr uint32_t kTextMeasureWrapperCaller = 0x817C077C;
inline constexpr uint32_t kTextMeasureExportCaller = 0x817C1330;
inline constexpr uint32_t kMaximumTextMeasureObservations = 8;
struct TextMeasureIdentity {
  uint64_t kernel = 0, domain = 0, root_generation = 0, foreground = 0;
  uint32_t thread = 0, pcr = 0, sp = 0, lr = 0;
  bool valid() const {
    return kernel && domain && root_generation && foreground && thread &&
           pcr && sp >= 0x90 && !(sp & 15) &&
           (lr == kTextMeasureWrapperCaller || lr == kTextMeasureExportCaller);
  }
  bool operator==(const TextMeasureIdentity&) const = default;
};
struct TextMeasureEntry {
  TextMeasureIdentity identity;
  uint32_t state = 0, object = 0, text_pointer = 0, output = 0, count = 0;
  TextSenderText text;
};
class TextMeasureDiagnostic {
 public:
  bool SetCaption(const TextSenderEntry& entry) {
    if (stopped_ || caption_ || !entry.identity.valid() ||
        !ExactText(entry.text)) { return false; }
    caption_ = entry;
    return true;
  }
  bool Claim(TextMeasureEntry entry) {
    // A reused SP/LR is not an invocation serial. Cancel before any matching
    // decision so a nested, abandoned or concurrent entry cannot complete A.
    if (pending_) { Stop(); return false; }
    if (stopped_ || !caption_ ||
        observed_ >= kMaximumTextMeasureObservations ||
        !entry.identity.valid() || entry.count != UINT32_MAX ||
        !ExactText(entry.text)) { return false; }
    const auto& sender = caption_->identity;
    const auto& measured = entry.identity;
    if (measured.kernel != sender.kernel || measured.domain != sender.domain ||
        measured.root_generation != sender.root_generation ||
        measured.foreground != sender.foreground ||
        entry.text.count != caption_->text.count) { return false; }
    for (size_t i = 0; i != entry.text.count; ++i) {
      if (entry.text.units[i] != caption_->text.units[i]) { return false; }
    }
    pending_ = std::move(entry);
    return true;
  }
  std::optional<TextMeasureEntry> Complete(const TextMeasureIdentity& identity) {
    if (stopped_ || !pending_ || pending_->identity != identity) { return {}; }
    ++observed_;
    return std::exchange(pending_, {});
  }
  bool pending() const { return bool(pending_); }
  bool Stop() {
    const bool pending = bool(pending_);
    stopped_ = true;
    pending_.reset();
    return pending;
  }
  const std::optional<TextSenderEntry>& caption() const { return caption_; }
  uint32_t observed() const { return observed_; }
  bool accepting() const {
    return !stopped_ && caption_ && observed_ < kMaximumTextMeasureObservations;
  }
 private:
  static bool ExactText(const TextSenderText& text) {
    return text.count && text.count <= text.units.size() && text.terminated &&
           !text.read_failed;
  }
  bool stopped_ = false;
  uint32_t observed_ = 0;
  std::optional<TextSenderEntry> caption_;
  std::optional<TextMeasureEntry> pending_;
};

struct TextMeasureState {
  bool readable = false;
  std::array<uint32_t, 9> words{};
};
struct TextMeasureBounds {
  bool readable = false;
  std::array<uint32_t, 4> bits{};
};
struct TextMeasureFont {
  bool cache_readable = false;
  std::array<uint32_t, 19> cache_words{};
  bool descriptor_readable = false;
  std::array<uint32_t, 5> descriptor{};
  std::array<TextSenderText, 4> names{};
};
// Each fixed span is admitted completely before decoding any word. These
// copied values grant no lifetime pin and are not atomic across guest writers.
template <size_t N>
inline bool ReadTextMeasureWords(Memory& memory, uint32_t address,
                                std::array<uint32_t, N>& words) {
  static_assert(N * sizeof(uint32_t) <= 256);
  std::array<uint8_t, N * sizeof(uint32_t)> bytes;
  if (!internal::ReadNativeWaitSnapshot(memory, address, bytes)) { return false; }
  for (size_t i = 0; i != N; ++i) {
    const size_t at = i * sizeof(uint32_t);
    words[i] = uint32_t(bytes[at]) << 24 | uint32_t(bytes[at + 1]) << 16 |
               uint32_t(bytes[at + 2]) << 8 | bytes[at + 3];
  }
  return true;
}
inline TextMeasureState ReadTextMeasureState(Memory& memory, uint32_t address) {
  TextMeasureState result;
  result.readable = ReadTextMeasureWords(memory, address, result.words);
  return result;
}
inline TextMeasureBounds ReadTextMeasureBounds(Memory& memory, uint32_t address) {
  TextMeasureBounds result;
  result.readable = ReadTextMeasureWords(memory, address, result.bits);
  return result;
}
inline TextMeasureFont ReadTextMeasureFont(Memory& memory, uint32_t address) {
  TextMeasureFont result;
  result.cache_readable = ReadTextMeasureWords(memory, address, result.cache_words);
  if (!result.cache_readable) { return result; }
  result.descriptor_readable =
      ReadTextMeasureWords(memory, result.cache_words[0], result.descriptor);
  if (!result.descriptor_readable) { return result; }
  constexpr std::array<size_t, 4> name_words{0, 1, 2, 4};
  for (size_t i = 0; i != name_words.size(); ++i) {
    result.names[i] = ReadTextSenderText(memory, result.descriptor[name_words[i]]);
  }
  return result;
}

// One bounded font-acquisition tree inside an already matched common caption
// measurement. These copied records grant no object, font or rendering right.
enum class FontAcquireSite : uint32_t {
  kCacheCtor, kEngineCreate, kEngineMetrics, kAtlas, kRenderAllocate,
  kBackendAllocate, kFaceLoad, kFileLoad, kFileInit, kContainer, kVerify,
  kDecompress, kParse, kFaceInstance, kHeaderRead, kHeaderCompat, kGlyphInit
};
struct FontAcquireCallSite {
  FontAcquireSite site;
  uint32_t function, caller1, caller2;
};
inline constexpr std::array<FontAcquireCallSite, 17> kFontAcquireSites{{
    {FontAcquireSite::kCacheCtor, 0x817925A0, 0x81792D40, 0},
    {FontAcquireSite::kEngineCreate, 0x8178F5C8, 0x81792688, 0},
    {FontAcquireSite::kEngineMetrics, 0x8178EAD0, 0x817926E8, 0},
    {FontAcquireSite::kAtlas, 0x81791898, 0x817927B4, 0},
    {FontAcquireSite::kRenderAllocate, 0x81783938, 0x81791908, 0},
    {FontAcquireSite::kBackendAllocate, 0x81798218, 0x81783980, 0},
    {FontAcquireSite::kFaceLoad, 0x8178EED8, 0x8178F67C, 0},
    {FontAcquireSite::kFileLoad, 0x817963B0, 0x8178EF3C, 0x8178F000},
    {FontAcquireSite::kFileInit, 0x817962E8, 0x8179649C, 0},
    {FontAcquireSite::kContainer, 0x817960A8, 0x81796378, 0},
    {FontAcquireSite::kVerify, 0x81795B68, 0x81796230, 0},
    {FontAcquireSite::kDecompress, 0x817A2B18, 0x81796274, 0},
    {FontAcquireSite::kParse, 0x817A40F8, 0x817964B4, 0},
    {FontAcquireSite::kFaceInstance, 0x81795038, 0x8178F148, 0x8178F1CC},
    {FontAcquireSite::kHeaderRead, 0x81794EE8, 0x8178EFB0, 0x8178F090},
    {FontAcquireSite::kHeaderCompat, 0x81794E78, 0x8178F068, 0},
    {FontAcquireSite::kGlyphInit, 0x81791AC8, 0x8179282C, 0x8179285C},
}};
inline const FontAcquireCallSite* FindFontAcquireSite(uint32_t function) {
  for (const auto& site : kFontAcquireSites) {
    if (site.function == function) { return &site; }
  }
  return nullptr;
}
struct FontAcquireWords {
  bool readable = false;
  uint32_t count = 0;
  std::array<uint32_t, 64> words{};
};
inline FontAcquireWords ReadFontAcquireWords(Memory& memory, uint32_t at,
                                            uint32_t count) {
  FontAcquireWords result;
  if (!count || count > result.words.size()) { return result; }
  std::array<uint8_t, 256> bytes;
  if (!internal::ReadNativeWaitSnapshot(memory, at,
          std::span<uint8_t>(bytes).first(count * 4))) { return result; }
  for (uint32_t i = 0; i != count; ++i) {
    const uint32_t p = i * 4;
    result.words[i] = uint32_t(bytes[p]) << 24 | uint32_t(bytes[p + 1]) << 16 |
                      uint32_t(bytes[p + 2]) << 8 | bytes[p + 3];
  }
  result.readable = true;
  result.count = count;
  return result;
}
inline uint32_t FontAcquireOffset(uint32_t base, uint32_t offset) {
  return base && base <= UINT32_MAX - offset ? base + offset : 0;
}
struct FontAcquireDescriptor {
  FontAcquireWords data;
  std::array<TextSenderText, 4> names{};
};
inline FontAcquireDescriptor ReadFontAcquireDescriptor(Memory& memory,
                                                       uint32_t at) {
  FontAcquireDescriptor result;
  result.data = ReadFontAcquireWords(memory, at, 5);
  if (result.data.readable) {
    constexpr std::array<size_t, 4> fields{0, 1, 2, 4};
    for (size_t i = 0; i != fields.size(); ++i) {
      result.names[i] = ReadTextSenderText(memory, result.data.words[fields[i]]);
    }
  }
  return result;
}
struct FontAcquireEngine {
  FontAcquireWords cell, object, table, default_name_cell;
  TextSenderText default_name;
};
inline FontAcquireEngine ReadFontAcquireEngine(Memory& memory) {
  FontAcquireEngine result;
  result.cell = ReadFontAcquireWords(memory, 0x81AC7E20, 1);
  if (result.cell.readable) {
    result.object = ReadFontAcquireWords(memory, result.cell.words[0], 8);
    if (result.object.readable) {
      result.table = ReadFontAcquireWords(memory, result.object.words[0], 9);
    }
  }
  result.default_name_cell = ReadFontAcquireWords(memory, 0x81ACA5A0, 1);
  if (result.default_name_cell.readable) {
    result.default_name = ReadTextSenderText(memory, result.default_name_cell.words[0]);
  }
  return result;
}
struct FontAcquireGraph {
  uint32_t device = 0, table = 0, atlas_target = 0;
  uint32_t backend = 0, backend_table = 0, allocate_target = 0;
  bool device_known = false, atlas_known = false, backend_known = false;
};
inline FontAcquireGraph ReadFontAcquireGraph(Memory& memory, uint32_t device) {
  FontAcquireGraph result;
  result.device = device;
  const auto table = ReadFontAcquireWords(memory, device, 1);
  if (!table.readable) { return result; }
  result.device_known = true;
  result.table = table.words[0];
  const auto atlas = ReadFontAcquireWords(memory, FontAcquireOffset(result.table, 0x10C), 1);
  if (atlas.readable) { result.atlas_known = true; result.atlas_target = atlas.words[0]; }
  // +1CC is a field of the authenticated native concrete renderer class,
  // not part of the arbitrary interface layout.
  if (result.table != 0x81604CC0) { return result; }
  const auto backend = ReadFontAcquireWords(memory, FontAcquireOffset(device, 0x1CC), 1);
  if (!backend.readable) { return result; }
  result.backend = backend.words[0];
  const auto backend_table = ReadFontAcquireWords(memory, result.backend, 1);
  if (!backend_table.readable) { return result; }
  result.backend_table = backend_table.words[0];
  const auto allocate = ReadFontAcquireWords(memory, FontAcquireOffset(result.backend_table, 0x1C), 1);
  if (allocate.readable) { result.backend_known = true; result.allocate_target = allocate.words[0]; }
  return result;
}
struct FontAcquireIdentity {
  TextMeasureIdentity measure;
  uint32_t measure_ordinal = 0, task = 0, function = 0, sp = 0, lr = 0;
  bool valid() const {
    const auto* site = FindFontAcquireSite(function);
    return measure.valid() && measure_ordinal && measure_ordinal <= kMaximumTextMeasureObservations &&
           task && sp >= 0x400 && !(sp & 15) && sp < measure.sp &&
           site && (lr == site->caller1 || (site->caller2 && lr == site->caller2));
  }
  bool operator==(const FontAcquireIdentity&) const = default;
};
struct FontAcquireEntry {
  FontAcquireIdentity identity;
  uint32_t ordinal = 0, parent = 0;
  std::array<uint64_t, 8> arguments{};
  uint64_t f1_bits = 0;
  bool gcrc_held = false;
  FontAcquireDescriptor descriptor;
  FontAcquireEngine engine;
  FontAcquireGraph graph;
  FontAcquireWords object;
  TextSenderText filename;
  uint32_t output_address = 0, output_words = 0;
};
struct FontAcquireResult {
  FontAcquireEntry entry;
  uint32_t raw_result = 0;
  FontAcquireWords output, object_after;
};
static_assert(sizeof(FontAcquireEntry) <= 8 * 1024);
struct FontAcquireReport {
  TextMeasureEntry measure;
  uint32_t measure_ordinal = 0, task = 0, return_task = 0, common_result = 0, completed = 0, pending_at_end = 0;
  bool normal_return = false, overflow = false, ambiguous = false, canceled = false;
  FontAcquireEngine engine_at_measure;
  TextMeasureState state_at_measure, state_at_return;
  TextMeasureBounds bounds_at_return;
  std::array<FontAcquireResult, 64> results{};
};
static_assert(sizeof(FontAcquireReport) <= 512 * 1024);
class FontAcquireDiagnostic {
 public:
  // Only the observer's host creation path prepares storage. Native callbacks
  // allocate nothing and only move one report out at common normal return.
  void Prepare() noexcept try {
    if (prepared_ || stopped_) { return; }
    for (auto& report : available_) { report = std::make_unique<FontAcquireReport>(); }
    prepared_ = true;
  } catch (...) { stopped_ = true; }
  bool Begin(const TextMeasureEntry& measure, uint32_t ordinal, uint32_t task,
             const TextMeasureState& state, FontAcquireEngine engine) {
    if (active_) { Cancel(true); return false; }
    if (!prepared_ || stopped_ || next_ == available_.size() || !measure.identity.valid() ||
        !ordinal || ordinal > kMaximumTextMeasureObservations ||
        !task || !state.readable || state.words[5]) { return false; }
    active_ = std::move(available_[next_++]);
    active_->measure = measure;
    active_->measure_ordinal = ordinal;
    active_->task = task;
    active_->state_at_measure = state;
    active_->engine_at_measure = std::move(engine);
    serial_ = 0;
    return true;
  }
  const FontAcquireReport* active() const { return active_.get(); }
  bool accepting() const { return active_ && !stopped_; }
  std::optional<std::pair<uint32_t, uint32_t>> Claim(const FontAcquireIdentity& id) {
    if (!active_ || stopped_ || !id.valid() || id.measure != active_->measure.identity ||
        id.task != active_->task || id.measure_ordinal != active_->measure_ordinal) { return {}; }
    const FontAcquireEntry* parent = nullptr;
    for (const auto& value : pending_) {
      if (!value) { continue; }
      if (value->identity == id || value->identity.sp <= id.sp) { Cancel(true); return {}; }
      if (!parent || value->identity.sp < parent->identity.sp) { parent = &*value; }
    }
    const auto site = FindFontAcquireSite(id.function)->site;
    if (!ParentMatches(site, parent)) { Cancel(true); return {}; }
    if (serial_ == 64) { active_->overflow = true; Cancel(false); return {}; }
    for (auto& value : pending_) {
      if (!value) {
        value.emplace();
        value->identity = id;
        value->ordinal = ++serial_;
        value->parent = parent ? parent->ordinal : 0;
        return std::pair{value->ordinal, value->parent};
      }
    }
    active_->overflow = true;
    Cancel(false);
    return {};
  }
  bool Record(FontAcquireEntry entry) {
    if (!active_ || stopped_) { return false; }
    for (auto& value : pending_) {
      if (value && value->identity == entry.identity && value->ordinal == entry.ordinal &&
          value->parent == entry.parent) { value = std::move(entry); return true; }
    }
    return false;
  }
  std::optional<FontAcquireEntry> Complete(const FontAcquireIdentity& id) {
    if (!active_ || stopped_) { return {}; }
    for (auto& value : pending_) {
      if (!value || value->identity != id) { continue; }
      for (const auto& child : pending_) {
        if (child && child->identity.sp < id.sp) { Cancel(true); return {}; }
      }
      return std::exchange(value, {});
    }
    return {};
  }
  void RecordReturn(FontAcquireResult result) {
    if (!active_ || stopped_ || result.entry.identity.measure != active_->measure.identity ||
        result.entry.identity.task != active_->task ||
        result.entry.identity.measure_ordinal != active_->measure_ordinal) { return; }
    if (active_->completed == active_->results.size()) {
      active_->overflow = true; Cancel(false); return;
    }
    active_->results[active_->completed++] = std::move(result);
  }
  std::unique_ptr<FontAcquireReport> Finish(const TextMeasureIdentity& id,
      uint32_t ordinal, uint32_t task, uint32_t result, TextMeasureState state,
      TextMeasureBounds bounds) {
    if (!active_ || active_->measure.identity != id || active_->measure_ordinal != ordinal) { return {}; }
    active_->common_result = result;
    active_->return_task = task;
    active_->state_at_return = state;
    active_->bounds_at_return = bounds;
    active_->normal_return = true;
    if (task != active_->task) { active_->canceled = stopped_ = true; }
    for (auto& value : pending_) {
      if (value) { ++active_->pending_at_end; value.reset(); }
    }
    if (active_->pending_at_end) { active_->canceled = stopped_ = true; }
    return std::move(active_);
  }
  void Stop() { stopped_ = true; Cancel(false); }
 private:
  static bool ParentMatches(FontAcquireSite site, const FontAcquireEntry* parent) {
    const auto p = parent ? std::optional(FindFontAcquireSite(parent->identity.function)->site) : std::nullopt;
    using S = FontAcquireSite;
    switch (site) {
      case S::kCacheCtor: return !p || p == S::kCacheCtor;
      case S::kEngineCreate: case S::kEngineMetrics: case S::kAtlas: case S::kGlyphInit: return p == S::kCacheCtor;
      case S::kRenderAllocate: return p == S::kAtlas;
      case S::kBackendAllocate: return p == S::kRenderAllocate;
      case S::kFaceLoad: return p == S::kEngineCreate;
      case S::kFileLoad: case S::kFaceInstance: case S::kHeaderRead: case S::kHeaderCompat: return p == S::kFaceLoad;
      case S::kFileInit: case S::kParse: return p == S::kFileLoad;
      case S::kContainer: return p == S::kFileInit;
      case S::kVerify: case S::kDecompress: return p == S::kContainer;
    }
    return false;
  }
  void Cancel(bool ambiguous) {
    if (active_) { active_->canceled = true; active_->ambiguous |= ambiguous; }
    for (auto& value : pending_) {
      if (value && active_) { ++active_->pending_at_end; }
      value.reset();
    }
    stopped_ = true;
  }
  std::array<std::unique_ptr<FontAcquireReport>, 8> available_{};
  std::unique_ptr<FontAcquireReport> active_;
  std::array<std::optional<FontAcquireEntry>, 16> pending_{};
  uint32_t next_ = 0, serial_ = 0;
  bool prepared_ = false, stopped_ = false;
};

// Native sound diagnostics retain copied metadata only. The owning observer
// validates the actual module/domain/context; these records grant no UI,
// guest-object, voice, registration or allocation authority.
inline constexpr std::array<uint32_t, 13> kNativeCueFunctions{
    0x817DE300, 0x817DE388, 0x817DE098, 0x817DDD38,
    0x819A3D10, 0x819A3878, 0x819A3930, 0x819A3978,
    0x817DDAC8, 0x817DDCA0, 0x819A3640, 0x819A51F8, 0x819AC540};
inline constexpr uint32_t kNativeCueSubmitFunction = 0x819AC740;
struct NativeCueCallIdentity {
  uint64_t kernel = 0, domain = 0, root_generation = 0, foreground = 0;
  uint32_t thread = 0, pcr = 0, sp = 0, lr = 0, function = 0;
  bool valid() const {
    if (!kernel || !domain || !root_generation || !thread || !pcr ||
        sp < 0x180 || (sp & 15) || !lr || (lr & 3)) { return false; }
    for (const auto at : kNativeCueFunctions) {
      if (at == function) { return true; }
    }
    return function == kNativeCueSubmitFunction;
  }
  bool operator==(const NativeCueCallIdentity&) const = default;
};
struct NativeCueWords {
  bool readable = false;
  std::array<uint32_t, 5> words{};
};
// Complete metadata-only RAM admission for the existing native6x256 block.
// This is neither a provider synchronization nor an atomic guest snapshot.
inline bool NativeCueFrameRamReadable(Memory& memory, uint32_t address) {
  constexpr uint32_t bytes = 6 * 256 * 4;
  if (!address || uint64_t(address) + bytes > 0x100000000ull) { return false; }
  auto lock = global_critical_region::AcquireDirect();
  auto* heap = memory.LookupHeap(address);
  if (!heap || memory.LookupHeap(address + bytes - 1) != heap ||
      memory.QueryRangeAccess(address, bytes) == memory::PageAccess::kNoAccess) { return false; }
  if (heap->heap_type() == HeapType::kGuestPhysical) {
    auto* physical = static_cast<PhysicalHeap*>(heap);
    const uint32_t first = physical->GetPhysicalAddress(address);
    const uint32_t last = physical->GetPhysicalAddress(address + bytes - 1);
    if (first >= 0x20000000 || bytes > 0x20000000 - first ||
        uint64_t(first) + bytes - 1 != last ||
        physical->parent_heap()->QueryRangeAccess(first, first + bytes - 1) ==
            memory::PageAccess::kNoAccess) { return false; }
  }
  return true;
}
inline NativeCueWords ReadNativeCueWords(Memory& memory, uint32_t address) {
  NativeCueWords result;
  std::array<uint8_t, 20> bytes;
  if (!internal::ReadNativeWaitSnapshot(memory, address, bytes)) { return result; }
  for (size_t i = 0; i != result.words.size(); ++i) {
    const size_t at = i * 4;
    result.words[i] = uint32_t(bytes[at]) << 24 | uint32_t(bytes[at + 1]) << 16 |
                      uint32_t(bytes[at + 2]) << 8 | bytes[at + 3];
  }
  result.readable = true;
  return result;
}
struct NativeCueBuffer {
  bool readable = false;
  std::array<uint32_t, 22> words{};
};
inline NativeCueBuffer ReadNativeCueBuffer(Memory& memory, uint32_t address) {
  NativeCueBuffer result;
  std::array<uint8_t, 88> bytes;
  if (!internal::ReadNativeWaitSnapshot(memory, address, bytes)) { return result; }
  for (size_t i = 0; i != result.words.size(); ++i) {
    const size_t at = i * 4;
    result.words[i] = uint32_t(bytes[at]) << 24 | uint32_t(bytes[at + 1]) << 16 |
                      uint32_t(bytes[at + 2]) << 8 | bytes[at + 3];
  }
  result.readable = true;
  return result;
}
struct NativeCueCall {
  NativeCueCallIdentity identity;
  uint64_t audio_sequence_at_entry = 0;
  uint32_t queued_frames_at_entry = 0, sink_frames_at_entry = 0;
  std::array<uint32_t, 3> arguments{};
  NativeCueWords instance, message;
  NativeCueBuffer buffer;
  TextSenderText filename;
};
class NativeCueCalls {
 public:
  bool Claim(const NativeCueCallIdentity& identity) {
    if (stopped_ || !identity.valid()) { return false; }
    for (const auto& entry : pending_) {
      if (entry && entry->identity == identity) { Stop(); return false; }
    }
    for (auto& entry : pending_) {
      if (!entry) {
        entry.emplace();
        entry->identity = identity;
        return true;
      }
    }
    Stop();
    return false;
  }
  bool Record(NativeCueCall entry) {
    if (stopped_) { return false; }
    for (auto& value : pending_) {
      if (value && value->identity == entry.identity) {
        value = std::move(entry);
        return true;
      }
    }
    return false;
  }
  std::optional<NativeCueCall> Complete(const NativeCueCallIdentity& identity) {
    if (stopped_) { return {}; }
    for (auto& entry : pending_) {
      if (entry && entry->identity == identity) { return std::exchange(entry, {}); }
    }
    return {};
  }
  void Stop() {
    stopped_ = true;
    for (auto& entry : pending_) { entry.reset(); }
  }
  bool accepting() const { return !stopped_; }
 private:
  std::array<std::optional<NativeCueCall>, 64> pending_{};
  bool stopped_ = false;
};

}  // namespace xe::kernel::notification_detail
