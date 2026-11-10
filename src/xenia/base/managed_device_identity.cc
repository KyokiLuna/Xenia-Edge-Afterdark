/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project
 ******************************************************************************
 * Released under the BSD license - see LICENSE in the root for more details.
 */
#include "xenia/base/managed_device_identity.h"
#include <algorithm>
#include <mutex>
#include <optional>
#include <random>
#include <string_view>
#include "third_party/mbedtls/include/mbedtls/sha256.h"
namespace xe::filesystem {
namespace {
using Digest = std::array<uint8_t, 32>;
constexpr std::string_view kMagic = "XMDI001\n", kEpochMagic = "XMEPO01\n";
constexpr size_t kMaximum = 65536, kRecordBytes = 88;
constexpr std::string_view kFormatMagic = "XMFMT01\n";
constexpr size_t kFormatRecordBytes = 136;
enum class FormatPhase : uint64_t { kPrepared = 1, kOwnersReady = 2, kCommitted = 3 };
struct FormatRecord {
  uint64_t old_epoch = 0;
  FormatPhase phase = FormatPhase::kPrepared;
  ManagedDeviceIdentity::Uuid nonce{};
  Digest binding{};
};
std::error_code Bad() { return std::make_error_code(std::errc::bad_message); }
std::error_code Conflict() {
  return std::make_error_code(std::errc::state_not_recoverable);
}
std::error_code Canceled() {
  return std::make_error_code(std::errc::operation_canceled);
}
bool Equal(std::span<const uint8_t> a, std::span<const uint8_t> b) {
  return std::equal(a.begin(), a.end(), b.begin(), b.end());
}
Digest Hash(std::span<const uint8_t> bytes, std::error_code& error) {
  Digest hash{};
  if (mbedtls_sha256(bytes.data(), bytes.size(), hash.data(), 0)) {
    error = std::make_error_code(std::errc::io_error);
  }
  return hash;
}
void U64(std::vector<uint8_t>& bytes, uint64_t value) {
  for (int shift = 56; shift >= 0; shift -= 8) {
    bytes.push_back(uint8_t(value >> shift));
  }
}
void Raw(std::vector<uint8_t>& bytes, std::span<const uint8_t> value) {
  bytes.insert(bytes.end(), value.begin(), value.end());
}
void Text(std::vector<uint8_t>& bytes, std::string_view value) {
  U64(bytes, value.size());
  Raw(bytes, {reinterpret_cast<const uint8_t*>(value.data()), value.size()});
}
struct Reader {
  std::span<const uint8_t> bytes;
  std::error_code error;
  std::span<const uint8_t> Take(size_t length) {
    if (length > bytes.size()) {
      error = Bad();
      return {};
    }
    auto part = bytes.first(length);
    bytes = bytes.subspan(length);
    return part;
  }
  uint64_t U64() {
    auto value = Take(8);
    if (value.size() != 8) {
      return 0;
    }
    uint64_t result = 0;
    for (auto byte : value) {
      result = (result << 8) | byte;
    }
    return result;
  }
  std::string Text(size_t maximum) {
    auto length = U64();
    if (length > maximum) {
      error = Bad();
      return {};
    }
    auto value = Take(size_t(length));
    if (std::find(value.begin(), value.end(), 0) != value.end()) {
      error = Bad();
      return {};
    }
    return {reinterpret_cast<const char*>(value.data()), value.size()};
  }
};
struct Record {
  ManagedDeviceIdentity::Uuid uuid{};
  uint32_t device = 0;
  uint64_t directory = 0, epoch = 0;
  std::string source, location;
  bool pending = false;
  std::optional<FormatRecord> format;
  bool handoff_pending() const {
    return format && format->phase != FormatPhase::kCommitted;
  }
  Digest last_hash{};
};
std::vector<uint8_t> Epoch(uint64_t epoch, bool pending, const Digest& previous,
                           std::error_code& error) {
  std::vector<uint8_t> wire;
  Raw(wire, {reinterpret_cast<const uint8_t*>(kEpochMagic.data()), 8});
  U64(wire, epoch);
  U64(wire, pending ? 1 : 2);
  Raw(wire, previous);
  Raw(wire, Hash(wire, error));
  return wire;
}
std::vector<uint8_t> FormatFrame(const FormatRecord& record,
    const Digest& previous, std::error_code& error) {
  std::vector<uint8_t> wire;
  Raw(wire, {reinterpret_cast<const uint8_t*>(kFormatMagic.data()), 8});
  U64(wire, record.old_epoch);
  U64(wire, uint64_t(record.phase));
  Raw(wire, record.nonce);
  Raw(wire, record.binding);
  Raw(wire, previous);
  Raw(wire, Hash(wire, error));
  return wire;
}
std::vector<uint8_t> Initial(const Record& r, std::error_code& error) {
  std::vector<uint8_t> body;
  Raw(body, r.uuid);
  U64(body, r.device);
  U64(body, r.directory);
  Text(body, r.source);
  Text(body, r.location);
  std::vector<uint8_t> wire;
  Raw(wire, {reinterpret_cast<const uint8_t*>(kMagic.data()), 8});
  U64(wire, body.size());
  Raw(wire, body);
  const auto hash = Hash(wire, error);
  Raw(wire, hash);
  Raw(wire, Epoch(1, false, hash, error));
  return wire;
}
Record Decode(std::span<const uint8_t> wire, std::error_code& error) {
  Record result;
  if (wire.size() > kMaximum || wire.size() < 16 + 32 + kRecordBytes) {
    error = Bad();
    return {};
  }
  Reader outer{wire, {}};
  if (!Equal(outer.Take(8),
             {reinterpret_cast<const uint8_t*>(kMagic.data()), 8})) {
    error = Bad();
    return {};
  }
  const auto body_length = outer.U64();
  if (body_length > 8192 || body_length > outer.bytes.size()) {
    error = Bad();
    return {};
  }
  const auto body = outer.Take(size_t(body_length));
  auto digest = Hash(wire.first(size_t(body_length) + 16), error);
  if (error || !Equal(outer.Take(32), digest)) {
    error = error ? error : Bad();
    return {};
  }
  Reader in{body, {}};
  auto uuid = in.Take(16);
  if (uuid.size() == 16) {
    std::copy(uuid.begin(), uuid.end(), result.uuid.begin());
  }
  const auto device = in.U64();
  result.device = uint32_t(device);
  result.directory = in.U64();
  result.source = in.Text(256);
  result.location = in.Text(4096);
  if (in.error || !in.bytes.empty() || !device || device > UINT32_MAX ||
      !result.directory || result.source.empty() || result.location.empty() ||
      std::all_of(result.uuid.begin(), result.uuid.end(),
                  [](auto b) { return b == 0; }) ||
      outer.bytes.empty()) {
    error = Bad();
    return {};
  }
  bool first = true;
  while (!outer.bytes.empty()) {
    // Epoch records remain byte-for-byte compatible. Managed handoff records
    // have a separate bounded typed frame, never a filename or length waiver.
    const auto magic = outer.bytes.first(std::min(size_t(8), outer.bytes.size()));
    const bool epoch_record = Equal(magic,
        {reinterpret_cast<const uint8_t*>(kEpochMagic.data()), 8});
    const bool format_record = Equal(magic,
        {reinterpret_cast<const uint8_t*>(kFormatMagic.data()), 8});
    const auto length = epoch_record ? kRecordBytes : kFormatRecordBytes;
    if ((!epoch_record && !format_record) || outer.bytes.size() < length) {
      error = Bad();
      return {};
    }
    const auto raw = outer.Take(length);
    Reader entry{raw, {}};
    entry.Take(8);
    if (epoch_record) {
      const uint64_t epoch = entry.U64(), phase = entry.U64();
      if (!Equal(entry.Take(32), digest)) {
        error = Bad();
        return {};
      }
      auto next = Hash(raw.first(kRecordBytes - 32), error);
      if (error || !Equal(entry.Take(32), next)) {
        error = error ? error : Bad();
        return {};
      }
      if (first ? (epoch != 1 || phase != 2)
                : (result.pending ? (epoch != result.epoch || phase != 2)
                                  : (result.epoch == UINT64_MAX ||
                                     epoch != result.epoch + 1 || phase != 1))) {
        error = Bad();
        return {};
      }
      if (result.handoff_pending() &&
          (result.format->phase != FormatPhase::kPrepared ||
           epoch != result.format->old_epoch + 1 ||
           (!result.pending && result.epoch != result.format->old_epoch))) {
        error = Bad();
        return {};
      }
      result.epoch = epoch;
      result.pending = phase == 1;
      digest = next;
      first = false;
      continue;
    }
    FormatRecord observed;
    observed.old_epoch = entry.U64();
    const auto phase = entry.U64();
    const auto nonce = entry.Take(16);
    const auto binding = entry.Take(32);
    if (nonce.size() != observed.nonce.size() ||
        binding.size() != observed.binding.size()) {
      error = Bad();
      return {};
    }
    std::copy(nonce.begin(), nonce.end(), observed.nonce.begin());
    std::copy(binding.begin(), binding.end(), observed.binding.begin());
    if (first || !observed.old_epoch || observed.old_epoch == UINT64_MAX ||
        phase < uint64_t(FormatPhase::kPrepared) ||
        phase > uint64_t(FormatPhase::kCommitted) ||
        std::all_of(nonce.begin(), nonce.end(), [](auto b) { return !b; }) ||
        std::all_of(binding.begin(), binding.end(), [](auto b) { return !b; }) ||
        !Equal(entry.Take(32), digest)) {
      error = Bad();
      return {};
    }
    auto next = Hash(raw.first(kFormatRecordBytes - 32), error);
    if (error || !Equal(entry.Take(32), next) || entry.error ||
        !entry.bytes.empty()) {
      error = error ? error : Bad();
      return {};
    }
    observed.phase = FormatPhase(phase);
    if (observed.phase == FormatPhase::kPrepared) {
      if (result.pending || result.handoff_pending() ||
          observed.old_epoch != result.epoch) {
        error = Bad();
        return {};
      }
    } else {
      const auto expected = observed.phase == FormatPhase::kOwnersReady
                                ? FormatPhase::kPrepared
                                : FormatPhase::kOwnersReady;
      if (!result.format || result.format->phase != expected ||
          result.pending || result.epoch != observed.old_epoch + 1 ||
          result.format->old_epoch != observed.old_epoch ||
          result.format->nonce != observed.nonce ||
          result.format->binding != observed.binding) {
        error = Bad();
        return {};
      }
    }
    result.format = observed;
    digest = next;
  }
  if (outer.error) {
    error = outer.error;
    return {};
  }
  result.last_hash = digest;
  return result;
}
}  // namespace
struct ManagedDeviceIdentity::State {
  std::mutex mutex;
  std::shared_ptr<HostStorageManagedMetadata> metadata;
  Record record;
};
ManagedDeviceIdentity::ManagedDeviceIdentity(
    std::shared_ptr<const HostStorageRoot> source, std::string source_id,
    uint32_t device_id, Uuid uuid, uint64_t epoch, uint64_t directory,
    std::filesystem::path location, bool pending, std::shared_ptr<State> state)
    : source_(std::move(source)),
      source_id_(std::move(source_id)),
      device_id_(device_id),
      uuid_(uuid),
      content_epoch_(epoch),
      directory_file_id_(directory),
      location_(std::move(location)),
      pending_format_(pending),
      state_(std::move(state)) {
  retired_.store(pending, std::memory_order_release);
}
ManagedDeviceIdentity::~ManagedDeviceIdentity() = default;
std::shared_ptr<const ManagedDeviceIdentity> ManagedDeviceIdentity::Open(
    std::shared_ptr<const HostStorageRoot> source, std::string source_id,
    uint32_t device_id, std::error_code& error) {
  return OpenInternal(std::move(source), std::move(source_id), device_id, error,
                      true, true);
}
std::shared_ptr<const ManagedDeviceIdentity>
ManagedDeviceIdentity::OpenExisting(
    std::shared_ptr<const HostStorageRoot> source, std::string source_id,
    uint32_t device_id, std::error_code& error) {
  return OpenInternal(std::move(source), std::move(source_id), device_id, error,
                      false, true);
}
bool ManagedDeviceIdentity::CheckExisting(
    std::shared_ptr<const HostStorageRoot> source, std::string source_id,
    uint32_t device_id, std::error_code& error) {
  const auto observed = OpenInternal(std::move(source), std::move(source_id),
                                     device_id, error, false, false);
  if (!observed && error == std::errc::no_such_file_or_directory) {
    error.clear();
    return false;
  }
  return observed && !error;
}
std::shared_ptr<const ManagedDeviceIdentity>
ManagedDeviceIdentity::OpenInternal(
    std::shared_ptr<const HostStorageRoot> source, std::string source_id,
    uint32_t device_id, std::error_code& error, bool create, bool admit_guest) {
  error.clear();
  if (!source || source->read_only() || source->is_guest_private() ||
      source_id.empty() || source_id.size() > 256 ||
      source_id.find('\0') != std::string::npos || !device_id ||
      source->path().generic_string().size() > 4096) {
    error = std::make_error_code(std::errc::invalid_argument);
    return {};
  }
  const auto live = source->IntentRootIdentity(error);
  if (error) {
    return {};
  }
  const auto mode = create ? HostStorageRoot::ManagedMetadataOpen::kCreate
                    : admit_guest
                        ? HostStorageRoot::ManagedMetadataOpen::kExisting
                        : HostStorageRoot::ManagedMetadataOpen::kProbeExisting;
  auto metadata = source->OpenManagedMetadata(kMarker, error, mode);
  if (!metadata) {
    return {};
  }
  if (metadata->created_) {
    Record record;
    record.source = source_id;
    record.device = device_id;
    record.directory = live.directory;
    record.location = source->path().generic_string();
    std::random_device random;
    for (auto& byte : record.uuid) {
      byte = uint8_t(random());
    }
    // Explicit random UUID version/variant bits, no host/filesystem ID guess.
    record.uuid[6] = uint8_t((record.uuid[6] & 0x0F) | 0x40);
    record.uuid[8] = uint8_t((record.uuid[8] & 0x3F) | 0x80);
    const auto bytes = Initial(record, error);
    if (error) {
      return {};
    }
    const auto write = metadata->Initialize(bytes);
    error = write.error;
    if (!error && (!write.durable || write.bytes != bytes.size())) {
      error = std::make_error_code(std::errc::io_error);
    }
    if (error) {
      return {};
    }
  }
  auto record = Decode(metadata->bytes_, error);
  if (error) {
    return {};
  }
  if (record.source != source_id || record.device != device_id ||
      record.directory != live.directory ||
      record.location != source->path().generic_string()) {
    error = Conflict();
    return {};
  }
  error = metadata->Check();
  if (error) {
    return {};
  }
  auto state = std::make_shared<State>();
  state->record = record;
  state->metadata = std::move(metadata);
  auto result =
      std::shared_ptr<const ManagedDeviceIdentity>(new ManagedDeviceIdentity(
          std::move(source), std::move(source_id), device_id, record.uuid,
          record.epoch, record.directory, record.location,
          record.pending || record.handoff_pending(), std::move(state)));
  if (record.pending || record.handoff_pending()) {
    error = std::make_error_code(std::errc::operation_in_progress);
  }
  return result;
}
std::error_code ManagedDeviceIdentity::CheckCurrent() const {
  if (retiring()) {
    return Canceled();
  }
  std::lock_guard lock(state_->mutex);
  if (retiring()) {
    return Canceled();
  }
  return state_->metadata->Check();
}
std::unique_ptr<ManagedDeviceIdentity::Handoff>
ManagedDeviceIdentity::PrepareHandoff(
    std::shared_ptr<const ManagedDeviceIdentity> owner, Digest binding,
    const std::atomic<bool>* cancellation,
    const std::atomic<bool>* external_cancellation) {
  auto handoff = std::make_unique<Handoff>();
  handoff->identity = std::move(owner);
  handoff->binding = binding;
  if (!handoff->identity ||
      std::all_of(binding.begin(), binding.end(), [](auto b) { return !b; })) {
    handoff->error = std::make_error_code(std::errc::invalid_argument);
    return handoff;
  }
  auto& identity = *handoff->identity;
  std::unique_lock lock(identity.state_->mutex, std::try_to_lock);
  if (!lock.owns_lock()) {
    handoff->error = std::make_error_code(std::errc::device_or_resource_busy);
    return handoff;
  }
  const auto& record = identity.state_->record;
  if (identity.retiring() || record.pending || record.handoff_pending() ||
      record.epoch == UINT64_MAX) {
    handoff->error = std::make_error_code(std::errc::operation_not_permitted);
    return handoff;
  }
  handoff->old_epoch = record.epoch;
  handoff->source_lease =
      identity.state_->metadata->ReserveFormatSource(handoff->error);
  if (!handoff->source_lease || handoff->error) { return handoff; }
  std::random_device random;
  for (auto& byte : handoff->nonce) { byte = uint8_t(random()); }
  handoff->nonce[6] = uint8_t((handoff->nonce[6] & 15) | 0x40);
  handoff->nonce[8] = uint8_t((handoff->nonce[8] & 63) | 0x80);
  const auto wire = FormatFrame(
      {record.epoch, FormatPhase::kPrepared, handoff->nonce, binding},
      record.last_hash, handoff->error);
  if (handoff->error) { return handoff; }
  // Linearize precommit cancellation at the final checked Prepared admission.
  // A later cancel cannot undo an append whose publication has begun.
  if ((cancellation && cancellation->load(std::memory_order_acquire)) ||
      (external_cancellation &&
       external_cancellation->load(std::memory_order_acquire))) {
    handoff->error = std::make_error_code(std::errc::operation_canceled);
    return handoff;
  }
  const auto written = identity.state_->metadata->Append(wire);
  handoff->error = written.error;
  handoff->prepared_durable = written.durable && written.bytes == wire.size();
  if (written.bytes) {
    std::error_code decoded;
    auto observed = Decode(identity.state_->metadata->bytes_, decoded);
    if (decoded) {
      identity.Retire();  // Retained uncertainty; never ordinary legacy format.
      if (!handoff->error) { handoff->error = decoded; }
    } else {
      identity.state_->record = std::move(observed);
      handoff->phase = uint64_t(identity.state_->record.format->phase);
    }
  }
  if (!handoff->error && !handoff->prepared_durable) {
    handoff->error = std::make_error_code(std::errc::io_error);
  }
  return handoff;
}
std::unique_ptr<ManagedDeviceIdentity::Handoff>
ManagedDeviceIdentity::InspectHandoff(
    std::shared_ptr<const ManagedDeviceIdentity> owner, Digest binding) {
  auto handoff = std::make_unique<Handoff>();
  handoff->identity = std::move(owner);
  handoff->binding = binding;
  if (!handoff->identity) {
    handoff->error = std::make_error_code(std::errc::invalid_argument);
    return handoff;
  }
  auto& identity = *handoff->identity;
  std::unique_lock lock(identity.state_->mutex, std::try_to_lock);
  if (!lock.owns_lock()) {
    handoff->error = std::make_error_code(std::errc::device_or_resource_busy);
    return handoff;
  }
  const auto& record = identity.state_->record;
  if (!record.format || record.format->binding != binding) {
    handoff->error = Conflict(); return handoff;
  }
  handoff->nonce = record.format->nonce;
  handoff->old_epoch = record.format->old_epoch;
  handoff->phase = uint64_t(record.format->phase);
  handoff->source_lease =
      identity.state_->metadata->ReserveFormatSource(handoff->error);
  if (!handoff->source_lease || handoff->error) { return handoff; }
  handoff->error = identity.state_->metadata->Synchronize();
  handoff->prepared_durable = !handoff->error;
  return handoff;
}
std::error_code ManagedDeviceIdentity::ReopenHandoffForResume(Handoff& handoff) {
  if (!handoff.identity || !handoff.source_lease || !handoff.old_epoch) {
    return std::make_error_code(std::errc::operation_not_permitted);
  }
  auto old = handoff.identity;
  auto source = handoff.recovery_source ? handoff.recovery_source : old->source_;
  if (!source->IntentLeaseMatches(*handoff.source_lease)) { return Conflict(); }
  // Preserve the proven source description; only release/reopen marker owner.
  // Partial/foreign frames fail decoding and remain on disk, never repaired.
  {
    std::unique_lock lock(old->state_->mutex, std::try_to_lock);
    if (!lock.owns_lock()) {
      return std::make_error_code(std::errc::device_or_resource_busy);
    }
    old->state_->metadata->Close();
  }
  std::error_code error;
  auto current = OpenInternal(source, old->source_id_, old->device_id_, error,
                              false, false);
  if (!current || (error && error != std::errc::operation_in_progress)) {
    return error ? error : Conflict();
  }
  const auto& record = current->state_->record;
  if (!record.format || record.format->old_epoch != handoff.old_epoch ||
      record.format->nonce != handoff.nonce ||
      record.format->binding != handoff.binding) { return Conflict(); }
  error = current->state_->metadata->Synchronize();
  if (error) { return error; }
  handoff.identity = std::move(current);
  handoff.creator_ready = false;
  handoff.prepared_durable = true;
  handoff.phase = uint64_t(record.format->phase);
  handoff.error.clear();
  return {};
}
ManagedDeviceFormatReceipt ManagedDeviceIdentity::ExecuteHandoff(
    Handoff& handoff) {
  ManagedDeviceFormatReceipt receipt;
  if (handoff.error || !handoff.identity || !handoff.source_lease ||
      !handoff.prepared_durable) {
    receipt.error = handoff.error ? handoff.error
        : std::make_error_code(std::errc::operation_not_permitted);
    return receipt;
  }
  auto owner = handoff.identity;
  std::unique_lock lock(owner->state_->mutex, std::try_to_lock);
  if (!lock.owns_lock()) {
    receipt.error = std::make_error_code(std::errc::device_or_resource_busy);
    return receipt;
  }
  const auto& record = owner->state_->record;
  if (!record.format || record.format->old_epoch != handoff.old_epoch ||
      record.format->nonce != handoff.nonce ||
      record.format->binding != handoff.binding ||
      !owner->source_->IntentLeaseMatches(*handoff.source_lease)) {
    receipt.error = Conflict(); return receipt;
  }
  receipt.content_epoch = handoff.old_epoch + 1;
  if (!record.pending && record.epoch == receipt.content_epoch) {
    // Already reset/identity-complete: explicit recovery never deletes again.
    receipt.epoch_observed = true;
    receipt.epoch_durable = true;
    receipt.complete = true;
  } else {
    if (record.format->phase != FormatPhase::kPrepared ||
        (record.pending ? record.epoch != receipt.content_epoch
                        : record.epoch != handoff.old_epoch)) {
      receipt.error = Conflict(); return receipt;
    }
    auto previous = record.last_hash;
    std::vector<uint8_t> pending;
    if (!record.pending) {
      pending = Epoch(receipt.content_epoch, true, previous, receipt.error);
      previous = Hash(std::span<const uint8_t>(pending).first(kRecordBytes - 32),
                       receipt.error);
    }
    auto complete = Epoch(receipt.content_epoch, false, previous, receipt.error);
    if (receipt.error) { return receipt; }
    const auto result = owner->state_->metadata->Format(
        pending, complete, record.pending, owner->retired_, {},
        handoff.source_lease);
    receipt.error = result.error;
    receipt.epoch_published = result.epoch_published;
    receipt.epoch_observed = result.epoch_observed;
    receipt.epoch_durable = result.epoch_durable;
    receipt.data_started = result.data_started;
    receipt.complete = result.complete;
    receipt.removed_entries = result.removed_entries;
    handoff.recovery_source = result.fresh_source;
    handoff.projection = result.fresh_projection;
    if (result.fresh_source_lease) {
      handoff.source_lease = result.fresh_source_lease;
    }
    if (!result.complete || receipt.error) {
      handoff.error = receipt.error;
      return receipt;
    }
    std::error_code error;
    auto current = OpenInternal(result.fresh_source, owner->source_id_,
                                owner->device_id_, error, false, false);
    if (!current || (error && error != std::errc::operation_in_progress)) {
      receipt.error = error ? error : Conflict();
      receipt.complete = false; handoff.error = receipt.error; return receipt;
    }
    handoff.identity = std::move(current);
  }
  auto current = handoff.identity;
  const auto& actual = current->state_->record;
  if (actual.pending || actual.epoch != receipt.content_epoch ||
      !actual.format || actual.format->nonce != handoff.nonce ||
      actual.format->binding != handoff.binding) {
    receipt.error = Conflict(); receipt.complete = false;
    handoff.error = receipt.error; return receipt;
  }
  // Only this drained, bound, genuinely completed handoff mints a creator.
  // Ordinary marker Open returns a retiring owner while handoff is active.
  auto creator = std::shared_ptr<const ManagedDeviceIdentity>(
      new ManagedDeviceIdentity(current->source_, current->source_id_,
          current->device_id_, current->uuid_, current->content_epoch_,
          current->directory_file_id_, current->location_, false,
          current->state_));
  handoff.identity = creator;
  handoff.creator_ready = true;
  handoff.phase = uint64_t(actual.format->phase);
  receipt.current = std::move(creator);
  return receipt;
}
std::error_code ManagedDeviceIdentity::AdvanceHandoff(
    Handoff& handoff, bool committed) {
  if (handoff.error || !handoff.creator_ready || !handoff.identity ||
      !handoff.source_lease) {
    return handoff.error ? handoff.error
        : std::make_error_code(std::errc::operation_not_permitted);
  }
  auto& identity = *handoff.identity;
  std::unique_lock lock(identity.state_->mutex, std::try_to_lock);
  if (!lock.owns_lock()) {
    return std::make_error_code(std::errc::device_or_resource_busy);
  }
  const auto& record = identity.state_->record;
  const auto expected = committed ? FormatPhase::kOwnersReady
                                  : FormatPhase::kPrepared;
  if (!record.format || record.pending || record.epoch != handoff.old_epoch + 1 ||
      record.format->old_epoch != handoff.old_epoch ||
      record.format->nonce != handoff.nonce ||
      record.format->binding != handoff.binding ||
      record.format->phase != expected ||
      !identity.source_->IntentLeaseMatches(*handoff.source_lease)) {
    return Conflict();
  }
  auto next = *record.format;
  next.phase = committed ? FormatPhase::kCommitted : FormatPhase::kOwnersReady;
  auto wire = FormatFrame(next, record.last_hash, handoff.error);
  if (handoff.error) { return handoff.error; }
  const auto written = identity.state_->metadata->Append(wire);
  handoff.error = written.error;
  std::error_code decoded;
  auto observed = Decode(identity.state_->metadata->bytes_, decoded);
  if (!decoded) {
    identity.state_->record = std::move(observed);
    handoff.phase = uint64_t(identity.state_->record.format->phase);
  }
  if (!handoff.error && (decoded || !written.durable ||
                         written.bytes != wire.size())) {
    handoff.error = decoded ? decoded : std::make_error_code(std::errc::io_error);
  }
  if (handoff.error) { identity.Retire(); }
  return handoff.error;
}
ManagedDeviceFormatReceipt ManagedDeviceIdentity::Format(
    std::shared_ptr<const ManagedDeviceIdentity> owner) {
  return FormatInternal(std::move(owner), {});
}
ManagedDeviceFormatReceipt ManagedDeviceIdentity::FormatInternal(
    std::shared_ptr<const ManagedDeviceIdentity> owner,
    std::error_code after_epoch_error) {
  ManagedDeviceFormatReceipt receipt;
  if (!owner) {
    receipt.error = std::make_error_code(std::errc::invalid_argument);
    return receipt;
  }
  std::unique_lock lock(owner->state_->mutex, std::try_to_lock);
  if (!lock.owns_lock()) {
    receipt.error = std::make_error_code(std::errc::device_or_resource_busy);
    return receipt;
  }
  if (owner->retiring() && !owner->pending_format_) {
    receipt.error = Canceled();
    return receipt;
  }
  const auto& record = owner->state_->record;
  // Legacy low-level epoch tests never authorize managed new-owner creation.
  // A managed handoff is resumed only by its typed reservation/phase producer.
  if (record.handoff_pending()) {
    receipt.error = std::make_error_code(std::errc::operation_in_progress);
    return receipt;
  }
  if (!record.pending && record.epoch == UINT64_MAX) {
    receipt.error = std::make_error_code(std::errc::value_too_large);
    return receipt;
  }
  receipt.content_epoch = record.pending ? record.epoch : record.epoch + 1;
  std::vector<uint8_t> pending;
  auto previous = record.last_hash;
  if (!record.pending) {
    pending = Epoch(receipt.content_epoch, true, previous, receipt.error);
    previous = Hash(std::span<const uint8_t>(pending).first(kRecordBytes - 32),
                    receipt.error);
  }
  auto complete = Epoch(receipt.content_epoch, false, previous, receipt.error);
  if (receipt.error) {
    return receipt;
  }
  const auto result = owner->state_->metadata->Format(
      pending, complete, record.pending, owner->retired_, after_epoch_error);
  receipt.error = result.error;
  receipt.epoch_published = result.epoch_published;
  receipt.epoch_observed = result.epoch_observed;
  receipt.epoch_durable = result.epoch_durable;
  receipt.data_started = result.data_started;
  receipt.complete = result.complete;
  receipt.removed_entries = result.removed_entries;
  if (result.complete) {
    receipt.current = Open(result.fresh_source, owner->source_id_,
                           owner->device_id_, receipt.error);
    if (!receipt.current || receipt.error) {
      receipt.complete = false;
    }
  }
  return receipt;
}
}  // namespace xe::filesystem
