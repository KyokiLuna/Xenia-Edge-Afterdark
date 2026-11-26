/** Xenia : Xbox 360 Emulator Research Project; BSD license, see LICENSE. */
#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include "xenia/xbox.h"
namespace xe::vfs {
class File;
// Explicit per-request publication participant carried by the actual File IO
// owner. A native family must authenticate its own original producer/source/
// operation before issuance; this interface grants no namespace/Task/slot right.
// Ordinary writes carry no participant and retain their existing behavior.
class FileWriteCheckpointUse {
 public:
  virtual ~FileWriteCheckpointUse()=default;
  // Immutable actual input snapshotted/validated at the real issuer boundary.
  // It replaces no native future output, overlap/Event/APC or completion owner.
  virtual std::span<const uint8_t> input() const noexcept=0;
  // Called under the SAME actual XFile IO lock before backend entry. A refusal
  // leaves transfer receipt/position unchanged and reaches normal completion.
  virtual X_STATUS ValidateTransfer(File&,uint64_t offset,uint32_t length) const noexcept=0;
  // Query/validation refused before a backend transfer; do not invent a byte
  // receipt or leave the family's scalar outcome looking like successful entry.
  virtual void TransferRefused(X_STATUS) noexcept {}
  // Backend status/count are factual; a genuine final checkpoint may then join
  // durability/model/cache. Returning failure makes no rollback/completion claim.
  // Implementations retain any uncertain/published transaction for real retry.
  virtual X_STATUS AfterTransfer(File&,X_STATUS,size_t transferred) noexcept=0;
};
class FileWriteCheckpointAuthority {
 public:
  virtual ~FileWriteCheckpointAuthority()=default;
  // One-shot: move all actual resources/duties into the worker-local Use (or
  // settle them on refusal). Historical retained handles must own metadata only
  // afterward. No HOST TLS, callback, borrowed guest pointer or inferred source.
  virtual std::unique_ptr<FileWriteCheckpointUse> AcquireForActualWrite(
      File&,X_STATUS& refusal) const noexcept=0;
};
}  // namespace xe::vfs
