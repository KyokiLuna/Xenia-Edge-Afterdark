/** Xenia : Xbox 360 Emulator Research Project; BSD license, see LICENSE. */
#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
namespace xe::kernel {
class NativeProfileIdentityOwner;
class NativeSystemUiService;
class KernelState;
class NativeIdentityTilePublication;
namespace util::test { class NativeImagePublicationTestAccess; }
// One metadata slot of the existing KernelAdmission. Weak ownership prevents
// an observation from extending native services or module lifetimes.
class NativeIdentityTileObserverSlot final {
 public:
  explicit NativeIdentityTileObserverSlot(KernelState* kernel):kernel_(kernel) {}
 private:
  friend class NativeSystemUiService;
  friend class NativeProfileIdentityOwner;
  friend class NativeIdentityTilePublication;
  KernelState* const kernel_;
  std::atomic<std::weak_ptr<NativeProfileIdentityOwner>> current_;
  std::atomic<bool> unavailable_{false};
};
// Factual completion of the existing shared TileService request only. This
// grants no profile, module, native allocation, readiness or Source authority.
// Native owner separately joins the real provider invocation/terminal IO.
class NativeIdentityTileWork final {
 public:
  void RecordPublication(uint32_t status,uint32_t required,bool written) noexcept;
 private:
  friend class NativeProfileIdentityOwner;
  friend class NativeIdentityTilePublication;
  struct Data;
  explicit NativeIdentityTileWork(std::shared_ptr<Data>);
  std::shared_ptr<Data> data_;
};
// Deny-only metadata around the original shared tile stores. All holders are
// inline, so inability to allocate a receipt cannot leave a real write unseen.
// Actual byte validation/stores/status and physical callbacks stay in GuestIo.
class NativeIdentityTilePublication final {
 public:
  struct Span {uint32_t address=0,bytes=0;};
  NativeIdentityTilePublication(KernelState*,std::array<Span,2>,
      const NativeIdentityTileWork* actual_work=nullptr) noexcept;
  ~NativeIdentityTilePublication();
  NativeIdentityTilePublication(const NativeIdentityTilePublication&)=delete;
  NativeIdentityTilePublication& operator=(const NativeIdentityTilePublication&)=delete;
  void BeforeStores() noexcept;
  void StoresCommitted() noexcept;
  void StoresUncertain() noexcept;
 private:
  friend class util::test::NativeImagePublicationTestAccess;
  void Observe(uint32_t phase) noexcept;
  KernelState* kernel_=nullptr;
  const std::array<Span,2> spans_;
  const NativeIdentityTileWork* const actual_work_=nullptr;
  // Initial attempted call, actual prestore and actual poststore. Strong
  // holders survive their GCRC scopes and die only with this outer receipt.
  std::array<std::shared_ptr<NativeIdentityTileObserverSlot>,3> slots_;
  std::array<std::shared_ptr<NativeProfileIdentityOwner>,3> owners_;
  bool borrow_owners_=true;
  bool stores_entered_=false,stores_settled_=false,stores_committed_=false;
};
} // namespace xe::kernel
