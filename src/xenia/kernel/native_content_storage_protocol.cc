/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */
#include "xenia/kernel/native_content_storage_protocol.h"

#include <algorithm>
#include <set>
#include <string_view>
#include <utility>

namespace xe::kernel {
namespace {
std::string Fold(std::string_view path) {
  std::string folded(path);
  for (auto& c : folded) {
    if (c >= 'A' && c <= 'Z') {
      c += 'a' - 'A';
    }
  }
  return folded;
}
bool SupportedType(xam::DeviceType type) {
  switch (type) {
    case xam::DeviceType::HDD:
    case xam::DeviceType::MU:
    case xam::DeviceType::System:
    case xam::DeviceType::ODD:
    case xam::DeviceType::TransferCable:
    case xam::DeviceType::Sapphire:
    case xam::DeviceType::USBMASS:
      return true;
    default:
      return false;
  }
}
bool ValidPath(std::string_view path) {
  // Native mount copies into a64-byte field and appends a separator. Its
  // volume roots are NT device names, never caller-selected host paths.
  if (path.size() < 9 || path.size() > 62 || path.back() == '\\' ||
      !Fold(path).starts_with("\\device\\")) {
    return false;
  }
  for (const unsigned char c : path) {
    if (c < 0x20 || c >= 0x7F || c == '/' || c == ':') {
      return false;
    }
  }
  size_t start = 1;
  while (start < path.size()) {
    const size_t end = path.find('\\', start);
    const auto part = path.substr(start, end == path.npos ? end : end - start);
    if (part.empty() || part == "." || part == "..") {
      return false;
    }
    if (end == path.npos) {
      break;
    }
    start = end + 1;
  }
  return true;
}
}  // namespace

size_t NativeContentStorageQueue::NativeVolumeCost(
    const NativeContentStorageDevice& device) {
  return device.type == xam::DeviceType::MU ||
                 device.type == xam::DeviceType::Sapphire
             ? 2
             : 1;
}
bool NativeContentStorageQueue::IsBuiltinHdd(
    const NativeContentStorageDevice& device) {
  return device.type == xam::DeviceType::HDD &&
         Fold(device.guest_volume_root) == "\\device\\harddisk0\\partition1";
}
std::deque<NativeContentStorageChange> NativeContentStorageQueue::Pending(
    const Devices& delivered, const Devices& desired) {
  std::deque<NativeContentStorageChange> result;
  // Removals precede insertions, including replacement of one physical
  // generation at the same path. Native XAM owns detach and ID reassignment.
  for (const auto& [id, old] : delivered) {
    const auto next = desired.find(id);
    if (next == desired.end() || next->second != old) {
      result.push_back({old, NativeContentStorageTransition::kRemoved});
    }
  }
  for (const auto& [id, next] : desired) {
    const auto old = delivered.find(id);
    if (old == delivered.end() || old->second != next) {
      result.push_back({next, NativeContentStorageTransition::kInserted});
    }
  }
  return result;
}
NativeContentStorageQueue::ApplyResult NativeContentStorageQueue::Apply(
    NativeContentStorageSnapshot snapshot, std::string* error) {
  const auto fail = [&](const char* text) {
    if (error) {
      *error = text;
    }
    return ApplyResult::kInvalid;
  };
  if (!snapshot.revision) {
    return fail("Storage snapshot revision is zero");
  }
  if (snapshot.revision <= revision_) {
    return ApplyResult::kStale;
  }
  if (serial_ == UINT64_MAX) {
    return fail("Storage delivery serial exhausted");
  }
  Devices desired;
  std::set<std::string> paths, identities;
  size_t volumes = 0;
  for (auto& device : snapshot.devices) {
    if (device.identity.empty() || device.identity.size() > 256 ||
        (!device.generation && device.available) ||
        !SupportedType(device.type) || !ValidPath(device.guest_volume_root) ||
        !identities.insert(device.identity).second) {
      return fail("Invalid or duplicate physical storage identity");
    }
    if (!device.available) {
      continue;
    }
    if (!paths.insert(Fold(device.guest_volume_root)).second) {
      return fail("Storage snapshot aliases one native volume path");
    }
    volumes += NativeVolumeCost(device);
    if (volumes > kMaximumNativeVolumes) {
      return fail("Storage snapshot exceeds native volume capacity");
    }
    desired.emplace(device.identity, std::move(device));
  }
  auto delivered = delivered_;
  if (!revision_) {
    // The checked native initializer bootstraps this exact HDD itself. A
    // duplicate initial notification would detach/reinsert it and change its
    // native ID. The startup coordinator still must observe the real native
    // mount/source attachment; this baseline is no readiness assertion.
    for (const auto& [id, device] : desired) {
      if (IsBuiltinHdd(device)) {
        delivered.emplace(id, device);
      }
    }
  }
  auto pending = Pending(delivered, desired);
  delivered_ = std::move(delivered);
  desired_ = std::move(desired);
  pending_ = std::move(pending);
  revision_ = snapshot.revision;
  ++serial_;
  return ApplyResult::kApplied;
}
const NativeContentStorageChange* NativeContentStorageQueue::Peek() const {
  return pending_.empty() ? nullptr : &pending_.front();
}
void NativeContentStorageQueue::CommitNext() {
  auto delivery = PrepareDelivery();
  if (delivery) {
    CommitDelivery(std::move(*delivery));
  }
}
std::optional<NativeContentStorageQueue::Delivery>
NativeContentStorageQueue::PrepareDelivery() const {
  if (pending_.empty() || serial_ == UINT64_MAX) {
    return {};
  }
  Delivery delivery;
  delivery.change_ = pending_.front();
  delivery.delivered_ = delivered_;
  delivery.owner_ = this;
  delivery.serial_ = serial_;
  const auto& next = delivery.change_;
  if (next.transition == NativeContentStorageTransition::kInserted) {
    delivery.delivered_.insert_or_assign(next.device.identity, next.device);
  } else {
    delivery.delivered_.erase(next.device.identity);
  }
  delivery.pending_ = Pending(delivery.delivered_, desired_);
  return delivery;
}
bool NativeContentStorageQueue::CommitDelivery(Delivery&& delivery) noexcept {
  if (delivery.owner_ != this || delivery.serial_ != serial_ ||
      serial_ == UINT64_MAX) {
    return false;
  }
  delivered_.swap(delivery.delivered_);
  pending_.swap(delivery.pending_);
  delivery.owner_ = nullptr;
  ++serial_;
  return true;
}
bool NativeContentStorageQueue::IsDesired(
    const NativeContentStorageDevice& device) const {
  const auto found = desired_.find(device.identity);
  return found != desired_.end() && found->second == device;
}

bool NativeStorageProcessingProtocol::PathsOverlap(
    std::string_view a, std::string_view b) {
  const auto fold = [](char c) {
    return c >= 'A' && c <= 'Z' ? char(c + ('a' - 'A')) : c;
  };
  const size_t count = std::min(a.size(), b.size());
  if (!count) { return true; }  // Unknown path cannot preserve a binding.
  for (size_t i = 0; i != count; ++i) {
    if (fold(a[i]) != fold(b[i])) { return false; }
  }
  return true;
}
bool NativeStorageProcessingProtocol::SingleVolumeProfile(
    const NativeStorageProcessingFrame& frame, uint32_t type) {
  if (!frame.valid()) { return false; }
  return (frame.lr == 0x8167EC40 && uint64_t(frame.sp) + 0x50 == frame.packet &&
          (type == 1 || type == 16)) ||
         (frame.lr == 0x8167EE2C && uint64_t(frame.sp) + 0x60 == frame.packet && type == 1);
}
bool NativeStorageProcessingProtocol::Begin(NativeStorageProcessingCall call) {
  if (stopped_ || !call.frame.valid() || call.path.empty() ||
      call.path.size() > 62 || !call.path_pointer ||
      (call.transition != 1 && call.transition != 2)) { return false; }
  Slot* selected = nullptr;
  for (auto& slot : slots_) {
    if (slot.thread == call.frame.thread) {
      if (slot.ambiguous) { return false; }
      if (slot.call) {
        // An abandoned call and a later identical frame are indistinguishable.
        slot.call.reset();
        slot.ambiguous = true;
        return false;
      }
      selected = &slot;
      break;
    }
    if (!slot.thread && !selected) { selected = &slot; }
  }
  if (!selected) { Stop(); return false; }
  for (auto& slot : slots_) {
    if (slot.call && PathsOverlap(slot.call->path, call.path)) {
      slot.call->delivery_cookie = 0;
      call.delivery_cookie = 0;
    }
  }
  selected->thread = call.frame.thread;
  selected->call = std::move(call);
  return true;
}
std::optional<NativeStorageProcessingProtocol::Receipt>
NativeStorageProcessingProtocol::Complete(
    const NativeStorageProcessingFrame& frame, uint32_t result,
    uint32_t path_pointer, uint32_t transition, uint32_t type,
    uint32_t native_id, std::string_view current_path) {
  if (stopped_ || !frame.valid()) { return {}; }
  for (auto& slot : slots_) {
    if (slot.thread != frame.thread || slot.ambiguous || !slot.call) { continue; }
    if (slot.call->frame != frame) { return {}; }
    auto call = std::move(*slot.call);
    slot.call.reset();
    if ((call.delivery_cookie && !SingleVolumeProfile(frame, type)) ||
        call.path_pointer != path_pointer ||
        call.transition != transition || call.type != type ||
        call.path != current_path || result > 1) { return {}; }
    // Removal's packet+C is unspecified, so never convert it to a device ID.
    const uint32_t id = result && transition == 1 ? native_id : 0;
    if (call.delivery_cookie && result && transition == 1 &&
        (!id || id == 2 || id > 0x1F3)) { return {}; }
    return Receipt{std::move(call), result == 1, id};
  }
  return {};
}
std::optional<NativeStorageProcessingFrame>
NativeStorageProcessingProtocol::Pending(uint32_t thread) const {
  if (stopped_) { return {}; }
  for (const auto& slot : slots_) {
    if (slot.thread == thread && !slot.ambiguous && slot.call) {
      return slot.call->frame;
    }
  }
  return {};
}
void NativeStorageProcessingProtocol::Stop() {
  stopped_ = true;
  for (auto& slot : slots_) { slot.call.reset(); }
}

}  // namespace xe::kernel
