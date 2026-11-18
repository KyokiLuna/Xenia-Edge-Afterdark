/** Xenia : Xbox 360 Emulator Research Project; BSD license, see LICENSE. */
#pragma once
#include <cstdint>
#include <memory>
namespace xe::kernel {
class NativeProfileIdentityOwner;
// Factual completion of the existing shared TileService request only. This
// grants no profile, module, native allocation, readiness or Source authority.
// Native owner separately joins the real provider invocation/terminal IO.
class NativeIdentityTileWork final {
 public:
  void RecordPublication(uint32_t status,uint32_t required,bool written) noexcept;
 private:
  friend class NativeProfileIdentityOwner;
  struct Data;
  explicit NativeIdentityTileWork(std::shared_ptr<Data>);
  std::shared_ptr<Data> data_;
};
} // namespace xe::kernel
