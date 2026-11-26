/** Xenia : Xbox 360 Emulator Research Project; BSD license, see LICENSE. */
#include "xenia/kernel/native_profile_save_request.h"
#include <new>
#include <algorithm>
#include "xenia/kernel/xam/profile_image_key.h"
#include "xenia/xbox.h"
namespace xe::kernel {
namespace {
uint32_t U32(std::span<const uint8_t> bytes, size_t offset) {
  const auto* p = bytes.data() + offset;
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}
std::error_code Bad() { return std::make_error_code(std::errc::invalid_argument); }
}  // namespace
std::optional<uint64_t> NativeProfileSaveRequest::image_key() const {
  if (family_ != Family::kImage) { return {}; }
  return xam::ProfileGpdImageKey(type_,item_);
}
bool NativeProfileSaveRequest::MatchesOriginalSelectionExpansion(std::span<const uint8_t> bytes) const {
  return family_==Family::kSelection && descriptor_.size()==24 && bytes.size()==44 &&
      std::equal(descriptor_.begin(),descriptor_.end(),bytes.begin()) &&
      std::all_of(bytes.begin()+24,bytes.end(),[](uint8_t byte){return byte==0;});
}
std::error_code NativeProfileSaveRequest::CapturePayload(Payload& payload, const Read& read) {
  // NULL is a genuine native mode/selection/delete operand. Native image
  // deletion ignores count; never validate/copy memory through NULL or coerce
  // it into an empty write. Nonnull+zero is retained without touching memory.
  if (!payload.address || !payload.declared_bytes) { return {}; }
  if (payload.declared_bytes > kMaximumPayload ||
      uint64_t(payload.address) + payload.declared_bytes > (uint64_t(1) << 32)) {
    return std::make_error_code(std::errc::file_too_large);
  }
  if (!read) { return std::make_error_code(std::errc::bad_address); }
  payload.bytes.resize(payload.declared_bytes);
  if (read(payload.address, payload.bytes) != X_ERROR_SUCCESS) {
    return std::make_error_code(std::errc::bad_address);
  }
  return {};
}
std::optional<NativeProfileSaveRequest> NativeProfileSaveRequest::CaptureOriginalFb(
    uint32_t message, std::span<const uint8_t> descriptor, const Read& read,
    std::error_code& error) {
  error.clear();
  size_t required = 0;
  switch (message) {
    case 0xB0031: required = 40; break;
    case 0xB0033: required = 24; break;
    case 0xB0037: required = 44; break;
    case 0xB0004: required = 4; break;
    default: error = std::make_error_code(std::errc::not_supported); return {};
  }
  if (descriptor.size() != required) { error = Bad(); return {}; }
  try {
    NativeProfileSaveRequest out;
    out.message_ = message; out.descriptor_.assign(descriptor.begin(), descriptor.end());
    if (message == 0xB0004) {
      out.family_ = Family::kSync; out.user_mask_ = U32(descriptor, 0);
      // Original mask selects only four users; keep ignored high bits as raw
      // provenance. No user index, fabricated flush or barrier success here.
      return out;
    }
    if (message == 0xB0031) {
      out.family_ = Family::kImage; out.type_ = U32(descriptor, 0);
      out.title_ = U32(descriptor, 4);
      out.item_ = uint64_t(U32(descriptor, 8)) << 32 | U32(descriptor, 12);
      out.user_slot_ = U32(descriptor, 0x10);
      out.large_ = {U32(descriptor, 0x14), U32(descriptor, 0x1C), {}};
      if (!out.image_key()) { error = std::make_error_code(std::errc::not_supported); return {}; }
      // +18,+20,+24 are preserved, not falsely demanded zero. The inspected
      // native image leaf consumes +14 pointer and +1C count, not vice versa.
    } else {
      out.family_ = message == 0xB0033 ? Family::kSelection : Family::kPicturePair;
      out.for_enumerate_ = U32(descriptor, 0); out.user_slot_ = U32(descriptor, 4);
      out.title_ = U32(descriptor, 8); out.large_id_ = U32(descriptor, 12);
      out.small_id_ = U32(descriptor, 16); out.flags_ = U32(descriptor, 20);
      if (message == 0xB0037) {
        out.large_ = {U32(descriptor, 24), U32(descriptor, 28), {}};
        out.small_ = {U32(descriptor, 32), U32(descriptor, 36), {}};
      }
    }
    if (out.user_slot_ >= 4) { error = Bad(); return {}; }
    error = CapturePayload(out.large_, read);
    if (!error) { error = CapturePayload(out.small_, read); }
    if (error) { return {}; }
    // Title0 and FFFE07D2 are not guessed from host foreground state. Concrete
    // owner binds the actual native07D00 result before opening a GPD source.
    return out;
  } catch (const std::bad_alloc&) {
    error = std::make_error_code(std::errc::not_enough_memory); return {};
  } catch (...) {
    error = std::make_error_code(std::errc::io_error); return {};
  }
}
}  // namespace xe::kernel
