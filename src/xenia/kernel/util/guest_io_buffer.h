/**
 * Xenia : Xbox 360 Emulator Research Project
 * Released under the BSD license - see LICENSE in the root for details.
 */
#ifndef XENIA_KERNEL_UTIL_GUEST_IO_BUFFER_H_
#define XENIA_KERNEL_UTIL_GUEST_IO_BUFFER_H_
#include <cstdint>
#include <span>
#include "xenia/xbox.h"
namespace xe {
class Memory;
namespace kernel::util {
// A request validates the complete supplied span before admission, then checks
// it again at actual access. No borrowed translated pointer crosses a wait.
// Nonempty copies/IOSB stores require no global critical region on entry.
// Unsupported context is rejected before touching bytes or invoking providers;
// helpers never release a caller's lock. The metadata range query is
// nonwaiting.
bool GuestIoRangeValid(Memory&, uint32_t address, uint32_t length,
                       bool writable);
X_STATUS ReadGuestIoBuffer(Memory&, uint32_t address, std::span<uint8_t>);
// Nonwaiting comparison in canonical backing coordinates (physical aliases
// compare against one physical address space). Callers validate ranges first.
bool GuestIoRangesOverlap(Memory&, uint32_t first, uint32_t first_length,
                          uint32_t second, uint32_t second_length);
struct GuestIoWrite {
  uint32_t address;
  std::span<const uint8_t> bytes;
};
// Bounded two-span publication for payload + metadata. No payload is stored
// until every provider wait and complete mapping/alias check succeeds. Sources
// are owned host bytes retained by the caller. Empty spans are ignored.
X_STATUS WriteGuestIoBuffers(Memory&, std::span<const GuestIoWrite>);
X_STATUS WriteGuestIoBuffer(Memory&, uint32_t address,
                            std::span<const uint8_t>);
// Store Information before Status and publish with a release fence, including
// through read-watched physical aliases.
X_STATUS WriteGuestIoStatus(Memory&, uint32_t address, X_STATUS status,
                            uint32_t information);

namespace internal {
// The E physical alias may have a backing host offset that is smaller than
// the host page size. Iterate in that backing coordinate, not by absolute
// guest-address page boundaries. This pure walker is shared with regression
// fixtures using16KiB host pages on a4KiB test host.
template <typename Visitor>
bool VisitGuestIoHostPages(uint32_t address, uint32_t length,
                           uint32_t heap_base, uint32_t host_offset,
                           uint32_t host_page_size, Visitor visitor) {
  if (!length) {
    return true;
  }
  if (address < heap_base || !host_page_size ||
      (host_page_size & (host_page_size - 1)) ||
      uint64_t(address) + length > 0x100000000ull) {
    return false;
  }
  const uint64_t first = uint64_t(address) - heap_base + host_offset;
  const uint64_t end = first + length;
  const uint64_t mask = uint64_t(host_page_size) - 1;
  for (uint64_t cursor = first; cursor < end; cursor = (cursor | mask) + 1) {
    visitor(uint32_t(uint64_t(heap_base) + cursor - host_offset));
  }
  return true;
}
}  // namespace internal
}  // namespace kernel::util
}  // namespace xe
#endif
