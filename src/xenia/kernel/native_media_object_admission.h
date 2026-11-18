/** Xenia : Xbox 360 Emulator Research Project; BSD license, see LICENSE. */
#ifndef XENIA_KERNEL_NATIVE_MEDIA_OBJECT_ADMISSION_H_
#define XENIA_KERNEL_NATIVE_MEDIA_OBJECT_ADMISSION_H_
#include <atomic>
#include <cstdint>
#include <memory>
#include <utility>
namespace xe::kernel {
class KernelState;
class XObject;
class NativeMediaService;
namespace xboxkrnl { class NativeMediaCountedBodyAccess; }
namespace test { class NativeMediaDeniedAliasTestAccess; }
// Preheld denial metadata on the SAME genuine native wrapper. The real guest
// header pointer/handle counts remain owned by existing CountedBody primitives.
// This object owns no Kernel, native reference, code, Task or file capability.
class NativeMediaObjectAdmission final {
 private:
  friend class XObject;
  friend class test::NativeMediaDeniedAliasTestAccess;
  friend class NativeMediaService;
  friend class xboxkrnl::NativeMediaCountedBodyAccess;
  NativeMediaObjectAdmission(std::weak_ptr<const uint8_t> issuer,
      const KernelState* kernel,uint64_t serial)
      :issuer_(std::move(issuer)),kernel_(kernel),serial_(serial) {}
  const std::weak_ptr<const uint8_t> issuer_;
  const KernelState* const kernel_;
  const uint64_t serial_;
  // Set once by the actual validated allocation/insertion receipt before
  // wrapper binding, never by a caller-filled guest tag or plausible address.
  uint32_t body_=0;
  uint32_t publication_handle_=0;
  std::atomic<bool> publication_live_{false};
  std::atomic<bool> selected_{false};
  std::atomic<bool> ingress_closed_{false};
  std::atomic<bool> handles_closed_{false};
  std::atomic<bool> body_detached_{false};
};
} // namespace xe::kernel
#endif
