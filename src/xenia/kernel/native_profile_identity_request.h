/** Xenia : Xbox 360 Emulator Research Project; BSD license, see LICENSE. */
#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <system_error>
#include "xenia/base/byte_order.h"
#include "xenia/base/memory.h"
namespace xe::kernel {
class NativeProfileIdentityOwner;
namespace test { class NativeProfileIdentityRequestTestAccess; }
// Detached wire bytes only. This cannot authorize a profile read, native Task,
// Account writer or publication. The concrete owner separately captures one
// genuine common roster Expected and stages the same desired identities.
// Original input and raw flags NEVER become the complete desired native array.
class NativeProfileIdentityRequest final {
 public:
  static constexpr size_t kSlots=4,kRowBytes=0x188,kAccountBytes=0x17C;
  uint16_t ordinal() const { return ordinal_; }
  uint32_t flags() const { return flags_; }
  std::span<const uint8_t> original() const {
    return std::span(bytes_).first(ordinal_==0x20B?32:4*kRowBytes);
  }
  const std::array<uint64_t,kSlots>& desired() const { return desired_; }
 private:
  friend class NativeProfileIdentityOwner;
  friend class test::NativeProfileIdentityRequestTestAccess;
  // `before` must come from the same retained HostRoster Expected. These
  // copied scalar values are parsing inputs only, never an ownership proof.
  static std::optional<NativeProfileIdentityRequest> Capture(
      uint16_t ordinal,std::span<const uint8_t> original,uint32_t flags,
      const std::array<uint64_t,kSlots>& before,std::error_code& error) {
    error.clear();
    if((ordinal!=0x20B && ordinal!=0x224) ||
       original.size()!=(ordinal==0x20B?32:4*kRowBytes)){
      error=std::make_error_code(std::errc::invalid_argument);return {};
    }
    // Preserve the established local modes and the two exact local 17559 SignIn
    // requests. Their service/preflight/UI policy bits execute natively; they
    // do not provision a Live Account or suppress identity publication.
    if(flags&0x4000u){error=std::make_error_code(std::errc::invalid_argument);return {};}
    const bool native_signin=flags==0x00120420u || flags==0x00120424u;
    if((flags&~0x2Fu) && !native_signin){
      error=std::make_error_code(std::errc::operation_not_supported);return {};
    }
    NativeProfileIdentityRequest out;out.ordinal_=ordinal;out.flags_=flags;
    std::copy(original.begin(),original.end(),out.bytes_.begin());
    const bool add=bool(flags&4u),remove=!add && bool(flags&8u);
    if(add || remove)out.desired_=before;
    for(size_t slot=0;slot<kSlots;++slot){
      const size_t stride=ordinal==0x20B?8:kRowBytes;
      const auto xuid=xe::load_and_swap<uint64_t>(original.data()+slot*stride);
      if(!xuid)continue; // Replacement zero removes; incremental zero preserves.
      if(remove){
        if(before[slot]!=xuid){error=std::make_error_code(std::errc::invalid_argument);return {};}
        out.desired_[slot]=0;out.remove_mask_|=uint8_t(1u<<slot);
      }else{out.desired_[slot]=xuid;out.selected_mask_|=uint8_t(1u<<slot);}
    }
    for(size_t slot=0;slot<kSlots;++slot)for(size_t earlier=0;earlier<slot;++earlier)
      if(out.desired_[slot] && out.desired_[slot]==out.desired_[earlier]){
        error=std::make_error_code(std::errc::invalid_argument);return {};
      }
    return out;
  }
  bool MatchesPreparedRows(std::span<const uint8_t> actual_rows) const {
    if(actual_rows.size()!=kSlots*kRowBytes)return false;
    for(size_t slot=0;slot<kSlots;++slot){
      if(xe::load_and_swap<uint64_t>(actual_rows.data()+slot*kRowBytes)!=desired_[slot])return false;
      // 20B genuinely supplies only four XUIDs. Its zeroed Account rows and
      // original native account-loading path must execute unchanged. 224's
      // meaningful add/replacement rows must match actual newly paused durable
      // Account+device bytes; stale UI/native input cannot overwrite a newer
      // Account. Original incremental zero rows are still left untouched.
      if(ordinal_==0x224 && (selected_mask_&(1u<<slot)) &&
         !std::equal(bytes_.begin()+slot*kRowBytes,bytes_.begin()+(slot+1)*kRowBytes,
                     actual_rows.begin()+slot*kRowBytes))return false;
    }
    return true;
  }
  uint16_t ordinal_=0;
  uint32_t flags_=0;
  uint8_t selected_mask_=0,remove_mask_=0;
  std::array<uint8_t,kSlots*kRowBytes> bytes_{};
  std::array<uint64_t,kSlots> desired_{};
};
} // namespace xe::kernel
