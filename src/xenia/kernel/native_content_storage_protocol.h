/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */
#ifndef XENIA_KERNEL_NATIVE_CONTENT_STORAGE_PROTOCOL_H_
#define XENIA_KERNEL_NATIVE_CONTENT_STORAGE_PROTOCOL_H_

#include <array>
#include <optional>
#include <string_view>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <string>

#include "xenia/base/byte_order.h"
#include "xenia/kernel/native_content_storage_source.h"

namespace xe::kernel {

// The required output prefix consumed by native17559 worker8167EB60. The
// underlying console driver may have additional opaque state; no meaning is
// assigned to its unused first12 bytes by this virtual provider.
struct NativeContentStorageNotification {
  xe::be<uint32_t> reserved[3]{};
  xe::be<uint32_t> volume_path = 0;
  xe::be<uint32_t> transition = 0;
  xe::be<uint32_t> device_type = 0;
};
static_assert(sizeof(NativeContentStorageNotification) == 0x18);

enum class NativeContentStorageTransition : uint32_t {
  kInserted = 1,
  kRemoved = 2,
};
struct NativeContentStorageChange {
  NativeContentStorageDevice device;
  NativeContentStorageTransition transition;
};

// Metadata only. Its owner serializes all access, including the final guest
// output store and CommitNext. It never creates a native volume or signals an
// event. Changes coalesce against successfully delivered records, so repeated
// catalog refreshes cannot overflow the native worker's bounded scratch list.
class NativeContentStorageQueue {
 public:
  enum class ApplyResult { kApplied, kStale, kInvalid };
  static constexpr size_t kMaximumNativeVolumes = 20;
  class Delivery {
   public:
    const NativeContentStorageChange& change() const { return change_; }

   private:
    friend class NativeContentStorageQueue;
    NativeContentStorageChange change_;
    std::map<std::string, NativeContentStorageDevice> delivered_;
    std::deque<NativeContentStorageChange> pending_;
    const NativeContentStorageQueue* owner_ = nullptr;
    uint64_t serial_ = 0;
  };
  ApplyResult Apply(NativeContentStorageSnapshot snapshot,
                    std::string* error = nullptr);
  const NativeContentStorageChange* Peek() const;
  // Allocate/copy the next owned metadata state BEFORE writing guest output.
  // Under the owner's lock, CommitDelivery then publishes it without
  // allocation.
  std::optional<Delivery> PrepareDelivery() const;
  bool CommitDelivery(Delivery&&) noexcept;
  void CommitNext();
  size_t size() const { return pending_.size(); }
  uint64_t revision() const { return revision_; }
  bool initialized() const { return revision_ != 0; }
  bool IsDesired(const NativeContentStorageDevice&) const;

  // MU/Sapphire insertions expand into two native volume entries. A caller
  // must end the current native drain before its successful insertions can
  // exceed20 IDs, then request another genuine storage-driver callback.
  static size_t NativeVolumeCost(const NativeContentStorageDevice&);
  static bool IsBuiltinHdd(const NativeContentStorageDevice&);

 private:
  using Devices = std::map<std::string, NativeContentStorageDevice>;
  static std::deque<NativeContentStorageChange> Pending(
      const Devices& delivered, const Devices& desired);
  uint64_t revision_ = 0;
  uint64_t serial_ = 0;
  Devices delivered_, desired_;
  std::deque<NativeContentStorageChange> pending_;
};

// Function-specific pairing metadata only, NOT a native processing authority.
// The driver supplies actual SYSTEM/context/code validation and joins returned
// delivery cookies to its own retained host change. No lease crosses PPC here.
struct NativeStorageProcessingFrame {
  uint64_t domain = 0, root_generation = 0;
  uint32_t thread = 0, pcr = 0, sp = 0, lr = 0, packet = 0;
  bool valid() const {
    return domain && root_generation && thread && pcr && sp >= 0x180 &&
           !(sp & 15) && lr && !(lr & 3) && packet && !(packet & 3);
  }
  bool operator==(const NativeStorageProcessingFrame&) const = default;
};
struct NativeStorageProcessingCall {
  NativeStorageProcessingFrame frame;
  uint32_t path_pointer = 0, transition = 0, type = 0;
  std::string path;
  uint64_t delivery_cookie = 0;
};
class NativeStorageProcessingProtocol {
 public:
  struct Receipt {
    NativeStorageProcessingCall call;
    bool succeeded = false;
    uint32_t native_id = 0;  // Meaningful ONLY for a successful insertion.
  };
  static bool SingleVolumeProfile(const NativeStorageProcessingFrame&, uint32_t type);
  bool Begin(NativeStorageProcessingCall);
  std::optional<Receipt> Complete(
      const NativeStorageProcessingFrame&, uint32_t result,
      uint32_t path_pointer, uint32_t transition, uint32_t type,
      uint32_t native_id, std::string_view current_path);
  std::optional<NativeStorageProcessingFrame> Pending(uint32_t thread) const;
  void Stop();
  // Native removal uses a prefix match. Broad overlap refusal is intentional.
  static bool PathsOverlap(std::string_view, std::string_view);
 private:
  struct Slot {
    uint32_t thread = 0;
    bool ambiguous = false;
    std::optional<NativeStorageProcessingCall> call;
  };
  std::array<Slot, 64> slots_{};
  bool stopped_ = false;
};

}  // namespace xe::kernel
#endif  // XENIA_KERNEL_NATIVE_CONTENT_STORAGE_PROTOCOL_H_
