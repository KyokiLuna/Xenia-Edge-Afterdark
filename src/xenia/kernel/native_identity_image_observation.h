/** Xenia : Xbox 360 Emulator Research Project; BSD license, see LICENSE. */
#pragma once
#include <cstdint>
#include <limits>
#include <optional>
#include "xenia/kernel/native_profile_identity_tasks.h"

namespace xe::kernel::identity_detail {
// Value-only facts in the existing native identity owner. These facts neither
// own nor read native bytes, grant profile authority, or reproduce a cache.
struct EncodedImageSource {
  NativeProfileIdentityTasks::Root root;
  uint64_t provider=0,allocation=0,publication=0,source=0,version=0;
  uint32_t address=0,size=0;
  bool operator==(const EncodedImageSource&) const=default;
};
struct DecodedImageSource {
  EncodedImageSource encoded;
  // A genuine decoder output has independent native ownership. Provider
  // buffer reuse after this receipt does not revoke that copied texture.
  bool operator==(const DecodedImageSource&) const=default;
};
class NativeEncodedImageEpoch final {
 public:
  bool Allocate(NativeProfileIdentityTasks::Root root,uint64_t provider,
      uint32_t address,uint32_t capacity) noexcept {
    live_=false;
    if(!root.valid() || !provider || !address || !capacity ||
       uint64_t(address)+capacity>0x100000000ull || !Advance(allocation_) ||
       !Advance(publication_))return false;
    root_=root;provider_=provider;address_=address;capacity_=capacity;live_=true;return true;
  }
  void Invalidate() noexcept {if(!Advance(publication_))live_=false;}
  void Revoke() noexcept {Invalidate();live_=false;}
  uint64_t allocation() const noexcept {return allocation_;}
  uint64_t publication() const noexcept {return publication_;}
  bool live() const noexcept {return live_;}
  std::optional<EncodedImageSource> Capture(uint64_t provider,uint64_t source,
      uint64_t version,uint32_t size,uint64_t actual_allocation,
      uint64_t actual_publication) const noexcept {
    if(!live_ || unavailable_ || !source || !size || size>capacity_ ||
       provider!=provider_ || !actual_allocation || !actual_publication ||
       actual_allocation!=allocation_ || actual_publication!=publication_)return {};
    return EncodedImageSource{root_,provider_,allocation_,publication_,source,version,
        address_,size};
  }
  bool Current(const EncodedImageSource& actual) const noexcept {
    return live_ && !unavailable_ && actual.root==root_ && actual.provider==provider_ &&
        actual.allocation==allocation_ && actual.publication==publication_ &&
        actual.address==address_ && actual.size && actual.size<=capacity_ &&
        actual.source;
  }
  std::optional<DecodedImageSource> FinishOriginalDecode(
      const EncodedImageSource& admitted,uint32_t actual_address,
      uint32_t actual_size,bool actual_normal_output) const noexcept {
    if(!actual_normal_output || !Current(admitted) ||
       actual_address!=admitted.address || actual_size!=admitted.size)return {};
    return DecodedImageSource{admitted};
  }
 private:
  bool Advance(uint64_t& counter) noexcept {
    if(unavailable_ || counter==std::numeric_limits<uint64_t>::max()){
      unavailable_=true;return false;
    }
    ++counter;return true;
  }
  NativeProfileIdentityTasks::Root root_;
  uint64_t provider_=0,allocation_=0,publication_=0;
  uint32_t address_=0,capacity_=0;
  bool live_=false,unavailable_=false;
};
} // namespace xe::kernel::identity_detail
