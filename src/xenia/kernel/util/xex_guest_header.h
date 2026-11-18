/** Xenia : Xbox 360 Emulator Research Project; BSD license, see LICENSE. */
#ifndef XENIA_KERNEL_UTIL_XEX_GUEST_HEADER_H_
#define XENIA_KERNEL_UTIL_XEX_GUEST_HEADER_H_

#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>

#include "xenia/base/memory.h"
#include "xenia/kernel/util/xex2_info.h"

namespace xe {
// The serialized file and CPU metadata keep a relative security offset. A
// resident guest header instead exposes the address of that SAME retained
// security block (as XexLoadImageHeaders already does). Callers provide a
// fresh serialized copy exactly once; this helper has no relocation marker.
// Failure leaves every byte unchanged.
inline bool RelocateXexSecurityInfoForGuest(std::span<uint8_t> bytes,
                                           uint32_t guest_base) {
  constexpr size_t kPrefix = offsetof(xex2_header, headers);
  if (bytes.size() < kPrefix ||
      (reinterpret_cast<uintptr_t>(bytes.data()) & 3) || guest_base < 0x1000 ||
      (guest_base & 3)) {
    return false;
  }
  const uint32_t magic = load_and_swap<uint32_t>(bytes.data());
  size_t security_prefix = 0;
  switch (magic) {
    case 0x58455831:  // XEX1
      security_prefix = offsetof(xex1_security_info, page_descriptors);
      break;
    case 0x58455832:  // XEX2
      security_prefix = offsetof(xex2_security_info, page_descriptors);
      break;
    case 0x58455825:  // XEX%
      security_prefix = offsetof(xex25_security_info, page_descriptors);
      break;
    default:
      return false;
  }
  const uint32_t size = load_and_swap<uint32_t>(bytes.data() + 8);
  const uint32_t count = load_and_swap<uint32_t>(bytes.data() + 0x14);
  if (size < kPrefix || size > bytes.size() ||
      size - 1 > std::numeric_limits<uint32_t>::max() - guest_base ||
      count > (size - kPrefix) / sizeof(xex2_opt_header)) {
    return false;
  }
  const uint32_t offset = load_and_swap<uint32_t>(bytes.data() + 0x10);
  const size_t optional_end = kPrefix + size_t(count) * sizeof(xex2_opt_header);
  if ((offset & 3) || offset < optional_end || offset > size ||
      security_prefix > size - offset) {
    return false;
  }
  const uint32_t security_size = load_and_swap<uint32_t>(bytes.data() + offset);
  if (security_size < security_prefix || security_size > size - offset) {
    return false;
  }
  const uint32_t pages = load_and_swap<uint32_t>(
      bytes.data() + offset + security_prefix - sizeof(uint32_t));
  if (pages > (security_size - security_prefix) / sizeof(xex2_page_descriptor)) {
    return false;
  }
  store_and_swap<uint32_t>(bytes.data() + 0x10, guest_base + offset);
  return true;
}
}  // namespace xe

#endif  // XENIA_KERNEL_UTIL_XEX_GUEST_HEADER_H_
