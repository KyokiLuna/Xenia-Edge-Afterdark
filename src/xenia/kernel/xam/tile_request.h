/** Xenia : Xbox 360 Emulator Research Project; BSD license, see LICENSE. */
#ifndef XENIA_KERNEL_XAM_TILE_REQUEST_H_
#define XENIA_KERNEL_XAM_TILE_REQUEST_H_
#include <cstdint>
#include <memory>
#include "xenia/xbox.h"
namespace xe::kernel { class KernelState; }
namespace xe::kernel::xam {
// Owned scalar request ABI only; no caller-supplied profile/readiness authority.
struct TileRequestArguments {
  bool texture = false;
  uint32_t type = 0, title = 0;
  uint64_t item = 0;
  uint32_t user = 0, destination = 0, size_address = 0;
  uint32_t stride = 0, height = 0, overlapped = 0;
};
class OwnedTileRequest final {
 public:
  ~OwnedTileRequest();
  OwnedTileRequest(const OwnedTileRequest&) = delete;
  struct State;
 private:
  explicit OwnedTileRequest(std::unique_ptr<State>);
  std::unique_ptr<State> state_;
  friend std::shared_ptr<const OwnedTileRequest> CaptureTileRequest(
      KernelState&, const TileRequestArguments&, X_HRESULT&);
  friend X_RESULT CompleteTileRequest(KernelState&, const OwnedTileRequest&,
                                     uint32_t&, uint32_t&);
};
bool IsTileMessage(uint32_t message);
std::shared_ptr<const OwnedTileRequest> CaptureTileRequest(
    KernelState&, const TileRequestArguments&, X_HRESULT&);
// Copies the full 17559 28/38-byte outer descriptor and captures every supported
// nested input/provider byte on the ORIGINAL caller before any queue/defer.
std::shared_ptr<const OwnedTileRequest> CaptureTileMessage(
    KernelState&, uint32_t message, uint32_t descriptor, uint32_t length,
    uint32_t overlapped, X_HRESULT&);
// Performs no fresh selector/profile lookup. Actual output mapping is checked
// again after waits. primary0/65B, extendedHRESULT and length0 are shared by
// public wrappers and genuine builtin FB completion.
X_RESULT CompleteTileRequest(KernelState&, const OwnedTileRequest&,
                             uint32_t& extended_error, uint32_t& length);
}
#endif
