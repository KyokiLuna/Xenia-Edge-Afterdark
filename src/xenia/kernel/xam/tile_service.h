/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia contributors. All rights reserved.                    *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */
#ifndef XENIA_KERNEL_XAM_TILE_SERVICE_H_
#define XENIA_KERNEL_XAM_TILE_SERVICE_H_

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

#include "xenia/vfs/installed_system_source.h"
#include "xenia/xbox.h"

namespace xe::kernel::xam {

// Owned inputs/results only. Guest buffers, async completion, key parsing and
// profile/title admission belong to the caller. No native boot or profile is
// needed to read a stock image from the selected installed-system authority.
class TileService final {
 public:
  static constexpr uint32_t kStockCount = 13;
  static constexpr uint32_t kTextureEncodedLimit = 0x4000;
  static constexpr uint32_t kDecodedLimit = 16u * 1024u * 1024u;
  static constexpr uint32_t kDimensionLimit = 16384;
  struct StockPicture {
    uint32_t index = 0;
    bool small = false;
    bool operator==(const StockPicture&) const = default;
  };
  class StockSource final {
   public:
    uint64_t generation() const { return source_->generation(); }

   private:
    friend class TileService;
    StockSource() = default;
    std::shared_ptr<const vfs::InstalledSystemSource::Snapshot> source_;
    std::shared_ptr<filesystem::HostStorageRoot> flash_;
    std::vector<uint8_t> package_;
  };
  // One selected installed-system authority, retained across idle installation
  // refreshes. Each capture selects its actual immutable snapshot. Cached bytes
  // never substitute for a missing, invalid or newly selected generation.
  class StockProvider final {
   public:
    explicit StockProvider(std::shared_ptr<vfs::InstalledSystemSource> owner)
        : owner_(std::move(owner)) {}
    std::shared_ptr<const StockSource> Capture(X_HRESULT& status) const;

   private:
    const std::shared_ptr<vfs::InstalledSystemSource> owner_;
    mutable std::mutex mutex_;
    mutable std::shared_ptr<const vfs::InstalledSystemSource::Snapshot>
        cached_snapshot_;
    mutable std::shared_ptr<const StockSource> cached_source_;
  };
  struct OwnedPng {
    X_HRESULT status = X_E_FAIL;
    std::vector<uint8_t> bytes;
    uint64_t source_generation = 0;
  };
  struct RawResult {
    X_HRESULT status = X_E_FAIL;
    uint32_t required = 0;
    std::vector<uint8_t> payload;
    uint64_t source_generation = 0;
  };
  struct TextureResult {
    X_HRESULT status = X_E_FAIL;
    uint32_t width = 0, height = 0, row_bytes = 0;
    // Last byte touched + 1, including row stride, NOT packed RGBA length.
    uint32_t destination_bytes = 0;
    std::vector<uint8_t> rgba;
    uint64_t source_generation = 0;
  };

  // Captures shrdres through ReadModuleResource using a private read-only VFS
  // view derived from this exact Flash capability, never reopening by path.
  static std::shared_ptr<const StockSource> CaptureStockSource(
      std::shared_ptr<const vfs::InstalledSystemSource::Snapshot> source,
      X_HRESULT& status);
  // Proven 17559 stock selectors 7/8/9 with canonical dashboard title only.
  // By-key callers normalize their owned parsed key into this selection.
  static std::optional<StockPicture> SelectStock(uint32_t tile_type,
                                                 uint32_t title_id,
                                                 uint64_t item_id);
  static OwnedPng ReadStock(const StockSource& source, StockPicture picture);
  static OwnedPng ReadStock(const StockProvider& provider,
                            StockPicture picture);
  static OwnedPng FromOwnedPng(std::vector<uint8_t> png);
  static RawResult PrepareRaw(OwnedPng png, uint32_t capacity);
  // Canonical owned RGBA, not a guest pixel-layout assertion. The caller owns
  // any guest ARGB conversion and all-or-nothing watched row publication.
  static TextureResult PrepareTexture(OwnedPng png, uint32_t stride,
                                      uint32_t declared_height);
};

}  // namespace xe::kernel::xam
#endif  // XENIA_KERNEL_XAM_TILE_SERVICE_H_
