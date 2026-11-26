/** Xenia : Xbox 360 Emulator Research Project; BSD license, see LICENSE. */
#pragma once
#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <system_error>
#include <vector>
namespace xe::kernel {
class NativeProfileSaveOwner;
namespace test { class NativeProfileSaveRequestTestAccess; }
// An owned snapshot of one ORIGINAL native FB request. Contents do not grant
// source, current-user, native Task, managed-view or completion authority.
// The original native protocol still owns every future guest read/store.
class NativeProfileSaveRequest final {
 public:
  enum class Family { kImage, kSelection, kPicturePair, kSync };
  struct Payload {
    uint32_t address = 0, declared_bytes = 0;
    std::vector<uint8_t> bytes;
    bool null_pointer() const { return address == 0; }
  };
  Family family() const { return family_; }
  uint32_t message() const { return message_; }
  std::span<const uint8_t> descriptor() const { return descriptor_; }
  uint32_t user_slot() const { return user_slot_; }
  uint32_t raw_title() const { return title_; }
  uint32_t type() const { return type_; }
  uint64_t item() const { return item_; }
  uint32_t large_id() const { return large_id_; }
  uint32_t small_id() const { return small_id_; }
  uint32_t flags() const { return flags_; }
  uint32_t for_enumerate() const { return for_enumerate_; }
  uint32_t raw_user_mask() const { return user_mask_; }
  uint32_t selected_user_mask() const { return user_mask_ & 0xF; }
  const Payload& large() const { return large_; }
  const Payload& small() const { return small_; }
  // Exact image mapping established by original819101D0. Null image pointer
  // deletes regardless of count; nonnull zero count is an EMPTY image write.
  std::optional<uint64_t> image_key() const;
 private:
  friend class NativeProfileSaveOwner;
  friend class test::NativeProfileSaveRequestTestAccess;
  using Read = std::function<uint32_t(uint32_t, std::span<uint8_t>)>;
  // Invoked only after original public/copier/FB frame authentication, BEFORE
  // host offload. Descriptor is already an owned exact native copy. Reader
  // validates the actual Memory owner for HOST copies, not page pins for PPC.
  static std::optional<NativeProfileSaveRequest> CaptureOriginalFb(
      uint32_t message, std::span<const uint8_t> descriptor, const Read&,
      std::error_code&);
  // Genuine E360 copies24B and clears the remaining20B before DDE8. Pure
  // byte agreement only; actual original frame/source ownership is separate.
  bool MatchesOriginalSelectionExpansion(std::span<const uint8_t>) const;
  static std::error_code CapturePayload(Payload&, const Read&);
  static constexpr uint32_t kMaximumPayload = 64u << 20;
  Family family_ = Family::kSync;
  uint32_t message_ = 0, user_slot_ = 0, title_ = 0, type_ = 0;
  uint64_t item_ = 0;
  uint32_t large_id_ = 0, small_id_ = 0, flags_ = 0, for_enumerate_ = 0, user_mask_ = 0;
  std::vector<uint8_t> descriptor_;
  Payload large_, small_;
};
}  // namespace xe::kernel
