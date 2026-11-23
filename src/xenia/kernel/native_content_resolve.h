/** Xenia : Xbox 360 Emulator Research Project; BSD license, see LICENSE. */
#ifndef XENIA_KERNEL_NATIVE_CONTENT_RESOLVE_H_
#define XENIA_KERNEL_NATIVE_CONTENT_RESOLVE_H_
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include "xenia/xbox.h"
namespace xe { namespace vfs { class Device; struct FileOpenContext; } }
namespace xe::kernel {
class KernelState;
class NativeImportDomain;
class NativeSystemAllocationAuthority;
class NativeSystemUiStartupCatalog;
class NativeSystemUiService;
namespace test { class NativeContentResolveTestAccess; }
// Owns the original17559 268/276 -> FE20016 request, not a host replacement
// formatter/dispatcher. The actual native producer deep-copies its packet,
// preserves the real caller's Task/namespace, and completes/cancels itself.
class NativeContentResolve final : public std::enable_shared_from_this<NativeContentResolve> {
 public:
  ~NativeContentResolve();
  // Activated only for genuine native SYSTEM268/276 callers in this
  // foundation. TITLE HLE exports keep their canonical device-ID policy until
  // the complete selector/namespace adapter is independently integrated.
  static constexpr bool HooksFunction(uint32_t at) {
    return at==0x816EE7B0 || at==0x8167F988;
  }
  static constexpr uint32_t kProducerFunction=0x816EE7B0;
  static constexpr uint32_t kProducerInstruction=0x816EE87C;
  static constexpr uint32_t kProducerWord=0x4BFB494D;
  static void ObserveProducerInstruction(void*,uint32_t) noexcept;
  static uint64_t GuardEntry(void*, uint64_t) noexcept;
  static void ObserveReturn(void*, uint64_t) noexcept;
  // Captures the actual admitted worker's exact volume before a real blocking
  // file open. nullopt leaves unrelated IO unchanged. A success must pass the
  // same expected Device through VFS::OpenFile, BEFORE any filesystem mutation.
  static std::optional<X_STATUS> CaptureOpenSource(
      KernelState*, std::string_view path, vfs::FileOpenContext&) noexcept;
  // Only the exact original worker->816E0400->Ob producer can claim this edge.
  // The original output copy has already happened; duplicate/source failures
  // remain genuine errors. A textual existing alias never proves ownership.
  static std::optional<X_STATUS> TryCreateSymbolicLink(
      KernelState*, std::string_view alias, std::string_view target) noexcept;
  void BeginStop() noexcept;
  bool Drained() const noexcept;
  bool RetainedForActualKernelStop() const noexcept;
 private:
  friend class NativeSystemUiService;
  friend class test::NativeContentResolveTestAccess;
  struct Config {
    std::shared_ptr<const NativeSystemUiStartupCatalog> catalog;
    std::shared_ptr<NativeImportDomain> domain;
    std::shared_ptr<NativeSystemAllocationAuthority> allocation;
  };
  static std::shared_ptr<NativeContentResolve> CreateBeforeWorkers(Config, std::string*);
  static std::shared_ptr<NativeContentResolve> Lookup(KernelState*);
  // Service-only factual whole CPU/IO/GPU-stopped boundary. This is never
  // inferred from Stop or an overlap result and never executes guest cleanup.
  bool RetireAfterActualKernelStop() noexcept;
  struct State;
  explicit NativeContentResolve(std::shared_ptr<State>);
  std::shared_ptr<State> state_;
};
}  // namespace xe::kernel
#endif
