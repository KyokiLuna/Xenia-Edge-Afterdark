/**
 * Xenia : Xbox 360 Emulator Research Project
 * Released under the BSD license - see LICENSE in the root for details.
 */
#ifndef XENIA_KERNEL_UTIL_GUEST_IO_BUFFER_H_
#define XENIA_KERNEL_UTIL_GUEST_IO_BUFFER_H_
#include <cstdint>
#include <span>
#include <mutex>
#include <memory>
#include <utility>
#include "xenia/xbox.h"
namespace xe {
class Memory;
namespace kernel::xam { class ProfileSettingsReadBatch; }
namespace kernel { class NativeMediaService; namespace test { class NativeMediaDeniedAliasTestAccess; } }
namespace kernel { class NativeIdentityTilePublication; }
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
// Narrow profile-reader publication boundary. Only the actual immutable batch
// can construct it. Admission is nonwaiting metadata only; no native I/O,
// providers, model callbacks or borrowed guest pointers are allowed inside.
class GuestIoProfileReadScope final {
 public:
  X_STATUS Admit();
  void Release() noexcept { if (lock_.owns_lock()) { lock_.unlock(); } }
 private:
  friend class xe::kernel::xam::ProfileSettingsReadBatch;
  GuestIoProfileReadScope(const xam::ProfileSettingsReadBatch& batch,
                          std::recursive_mutex& mutex)
      : batch_(batch), lock_(mutex, std::defer_lock) {}
  const xam::ProfileSettingsReadBatch& batch_;
  std::unique_lock<std::recursive_mutex> lock_;
};
// Exact owned native enum/stream factory out-DWORD. Constructible by that owner;
// final admission is nonwaiting metadata, after provider waits and range checks.
class GuestIoNativeMediaFactoryScope final {
 public:
  X_STATUS Admit();
  void Release() noexcept;
 private:
  friend class xe::kernel::NativeMediaService;
  friend class xe::kernel::test::NativeMediaDeniedAliasTestAccess;
  GuestIoNativeMediaFactoryScope(std::shared_ptr<NativeMediaService> owner,
      uint64_t serial,uint32_t handle,Memory& memory)
      :owner_(std::move(owner)),serial_(serial),handle_(handle),memory_(memory) {}
  const std::shared_ptr<NativeMediaService> owner_;
  const uint64_t serial_;
  const uint32_t handle_;
  Memory& memory_;
  std::unique_lock<std::mutex> lock_;
};
X_STATUS WriteGuestIoBuffers(Memory&, std::span<const GuestIoWrite>);
// The typed observer denies only image provenance. It never changes byte
// validation/status, owns no translated destination, and releases all strong
// holders in its caller after this aggregate writer's GCRC/callback scopes.
X_STATUS WriteGuestIoBuffers(Memory&, std::span<const GuestIoWrite>,
                             NativeIdentityTilePublication&);
X_STATUS WriteGuestIoBuffers(Memory&, std::span<const GuestIoWrite>,
                             GuestIoProfileReadScope&);
X_STATUS WriteGuestIoBuffers(Memory&, std::span<const GuestIoWrite>,
                             GuestIoNativeMediaFactoryScope&);
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
