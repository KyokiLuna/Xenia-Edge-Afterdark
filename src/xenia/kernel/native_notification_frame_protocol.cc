#include "xenia/kernel/native_notification_frame_protocol.h"
#include <utility>
namespace xe::kernel::notification_detail {
bool FrameSample::valid() const {
  return owner.valid() && domain && root_generation && foreground && thread &&
         task >= 0x1000 && (task & 3) == 1 && frame_sp && !(frame_sp & 15) &&
         (!modal.known || modal.sequence) && (modal.known || !modal.sequence);
}
FrameProtocol::FrameProtocol(memory::PhysicalAllocationOwner owner,
                             uint64_t domain, uint64_t root_generation)
    : owner_(owner), domain_(domain), root_generation_(root_generation) {
  accepting_ = owner.valid() && domain && root_generation;
}
bool FrameProtocol::Owns(const FrameSample& value) const {
  return accepting_ && value.valid() && value.owner == owner_ &&
         value.domain == domain_ && value.root_generation == root_generation_;
}
bool FrameProtocol::SyncModal(ModalSnapshot value) {
  if ((value.known && !value.sequence) || (!value.known && value.sequence)) {
    arm_.reset();
    return false;
  }
  if (modal_ && *modal_ != value) {
    arm_.reset();
    if (value.sequence < modal_->sequence) {
      return false;
    }
  }
  modal_ = value;
  return true;
}
bool FrameProtocol::Arm(const FrameSample& value) {
  if (!Owns(value) || !SyncModal(value.modal) || arm_) {
    arm_.reset();
    return false;
  }
  if (serial_ == UINT64_MAX) {
    BeginStop();
    return false;
  }
  arm_ = ArmState{value, ++serial_, false, {}};
  return true;
}
bool FrameProtocol::ObserveDraw(
    const FrameSample& value,
    std::optional<std::array<uint8_t, 0x58>> display_info) {
  if (!Owns(value) || !SyncModal(value.modal) || !arm_ ||
      arm_->frame != value || arm_->drew) {
    arm_.reset();
    return false;
  }
  arm_->drew = true;
  arm_->display_information = std::move(display_info);
  return true;
}
std::optional<uint64_t> FrameProtocol::PeekControl(
    const FrameSample& value) const {
  if (!Owns(value) || !arm_ || arm_->frame != value || arm_->drew ||
      arm_->display_information) {
    return {};
  }
  return arm_->serial;
}
std::optional<FrameProtocol::DrawSnapshot> FrameProtocol::PeekDraw(
    const FrameSample& value) const {
  if (!Owns(value) || !arm_ || arm_->frame != value || !arm_->drew ||
      !arm_->display_information) {
    return {};
  }
  return DrawSnapshot{arm_->serial, *arm_->display_information};
}
std::optional<uint64_t> FrameProtocol::NextRevision() {
  if (revision_ == UINT64_MAX) {
    BeginStop();
    return {};
  }
  return ++revision_;
}
std::optional<FrameDecision> FrameProtocol::FinishSource(
    const FrameSample& value, const gpu::NativeFrameSubmissionIdentity& source,
    uint64_t modal_r15, uint64_t overlay_r16) {
  if (!Owns(value) || !SyncModal(value.modal) || !arm_ ||
      arm_->frame != value || !source.valid() || source.owner != owner_) {
    arm_.reset();
    return {};
  }
  const auto accepted = std::exchange(arm_, {});
  if (accepted->drew && (modal_r15 || overlay_r16)) {
    return {};  // Unknown/mixed frame, never a guessed inactive result.
  }
  const auto revision = NextRevision();
  if (!revision) {
    return {};
  }
  return FrameDecision{accepted->drew ? FrameDecision::Kind::kEligible
                                      : FrameDecision::Kind::kInactive,
                       value, accepted->serial, *revision, source};
}
std::optional<FrameDecision> FrameProtocol::FinishClear(
    const FrameSample& value) {
  if (!Owns(value) || !SyncModal(value.modal)) {
    return {};
  }
  // The adapter independently authenticated the real clear helper. Its clear
  // often follows an already consumed recorded Set in the same native frame.
  const uint64_t serial = arm_ ? arm_->serial : serial_;
  arm_.reset();
  const auto revision = NextRevision();
  if (!revision) {
    return {};
  }
  return FrameDecision{
      FrameDecision::Kind::kClear, value, serial, *revision, {}};
}
void FrameProtocol::ModalTransition(ModalSnapshot value) {
  if (!accepting_) {
    return;
  }
  arm_.reset();
  SyncModal(value);
}
void FrameProtocol::CancelFrame(const FrameSample& value) {
  if (arm_ && arm_->frame == value) {
    arm_.reset();
  }
}
void FrameProtocol::BeginStop() {
  accepting_ = false;
  arm_.reset();
}
}  // namespace xe::kernel::notification_detail
