/** Xenia : Xbox 360 Emulator Research Project; BSD license, see LICENSE. */
#ifndef XENIA_KERNEL_NATIVE_AVATAR_CONTRACT_H_
#define XENIA_KERNEL_NATIVE_AVATAR_CONTRACT_H_
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include <utility>
namespace xe::kernel {
struct ValidatedGuestRuntimePlan;
enum class NativeSystemUiPublicCall : uint16_t;
// Exact selected public-wrapper -> common Start scalar ABI. This metadata is
// not a Request, native Task, profile read or source capability.
struct NativeAvatarRequestStartProfile {
  NativeSystemUiPublicCall call;
  uint32_t stack_bytes,return_lr,input_offset,options_offset;
  uint32_t app,message,payload_bytes;
  uint8_t overlap_argument; // 0xFF means original wrapper passes null.
};
const NativeAvatarRequestStartProfile* NativeAvatarRequestStart(
    NativeSystemUiPublicCall) noexcept;
bool MatchesNativeAvatarRequestStartEdge(NativeSystemUiPublicCall,
    uint32_t public_sp,uint32_t start_sp,uint32_t start_lr,uint32_t input,
    uint32_t parameters) noexcept;
bool MatchesNativeAvatarRequestScheduleEdge(uint32_t start_sp,uint32_t schedule_sp,
    uint32_t schedule_lr,uint32_t packet,uint32_t callback,uint32_t argument,
    uint32_t parameters,uint32_t expected_parameters,uint32_t output) noexcept;
bool MatchesNativeAvatarRequestAllocationEdge(uint32_t schedule_sp,uint32_t allocation_sp,
    uint32_t allocation_lr,uint32_t parameters,uint32_t expected_parameters,
    uint32_t output) noexcept;
struct NativeAvatarBridgeProfile {
  uint16_t ordinal;
  uint32_t address, bytes;
  std::string_view sha256;
};
// Counts from one genuine synchronous 5E0 return, not asset readiness or an
// allocation capability. The Service retains the exact Invocation, caller and
// Session separately and revalidates them before borrowing any 5E1 output.
class NativeAvatarAssetSizes final {
 public:
  static std::optional<NativeAvatarAssetSizes> CaptureActualReturn(
      uint16_t mask, bool returned, uint32_t status,
      std::optional<uint32_t> cpu, std::optional<uint32_t> gpu) noexcept {
    if (!returned || int32_t(status) < 0 || !cpu || !*cpu || !gpu) return {};
    return NativeAvatarAssetSizes(mask, *cpu, *gpu);
  }
  std::optional<std::pair<uint32_t,uint32_t>> ForMask(uint16_t mask) const noexcept {
    if (mask != mask_) return {};
    return std::pair{cpu_, gpu_};
  }
 private:
  NativeAvatarAssetSizes(uint16_t mask, uint32_t cpu, uint32_t gpu)
      : mask_(mask), cpu_(cpu), gpu_(gpu) {}
  uint16_t mask_;
  uint32_t cpu_, gpu_;
};

// One full-entry family: six common profile exports and real content open/close.
std::span<const NativeAvatarBridgeProfile> NativeAvatarBridgeProfiles();
const NativeAvatarBridgeProfile* NativeAvatarBridge(uint16_t ordinal);
struct NativeAvatarStartupProfile {
  std::string_view identity;
  uint32_t address, bytes;
  std::string_view sha256;
  uint32_t arguments;
  bool returns_void;
};
std::span<const NativeAvatarStartupProfile> NativeAvatarStartupProfiles();
// Exact fixed17559 native call-site relations. This is only a metadata check;
// an actual guarded frame, code/root lease, Task and owning FE scope are still
// required. It cannot create a scope or admit a resident/public request.
enum class NativeAvatarResidentEdge {
  kFrontendBuiltin, kBuiltinSetup, kBuiltinLoad, kLoadCallback, kCallbackLock
};
bool MatchesNativeAvatarResidentEdge(NativeAvatarResidentEdge, uint32_t parent_sp,
                                    uint32_t child_sp, uint32_t child_lr);
// Tagged GetCurrentTask owned-reference spelling versus native context+0C
// raw Task body. This comparison does not acquire/close a native reference.
bool NativeAvatarTaskReferenceMatchesBody(uint32_t tagged_reference,
                                         uint32_t raw_body);
// Original Init Start -> Schedule -> Allocate edges. These scalar ABI checks
// require the separate real Request/caller/code/backing and allocation output;
// they cannot create a Task, generation, profile source or native authority.
bool MatchesNativeAvatarInitStartEdge(uint32_t public_sp,uint32_t start_sp,uint32_t start_lr);
bool MatchesNativeAvatarInitScheduleEdge(uint32_t start_sp,uint32_t schedule_sp,
    uint32_t schedule_lr,uint32_t packet,uint32_t callback,uint32_t argument,
    uint32_t options,uint32_t output);
bool MatchesNativeAvatarInitAllocationEdge(uint32_t schedule_sp,uint32_t allocation_sp,
    uint32_t allocation_lr,uint32_t parameters,uint32_t output);

// One authenticated native call attempt. A Request attempt uses a nonzero
// invocation; legacy finite Control/resident pairs keep the original zero.
// These scalar receipts do not issue Task/read/completion authority.
template <typename Frame>
struct NativeAvatarCallPair {
  Frame frame;
  bool entered = false, returned = false;
  uint32_t value = 0;
  uint64_t invocation = 0;
  bool Enter(Frame actual, uint32_t input, uint64_t serial = 0) {
    if (entered || returned) return false;
    frame = actual; value = input; invocation = serial; entered = true; return true;
  }
  bool Finish(Frame actual, uint64_t serial = 0) {
    if (!entered || returned || frame != actual || invocation != serial) return false;
    returned = true; return true;
  }
};
// Strong target metadata from the actual successful Request entry. This is
// independent of the Request registry and physical Frame reuse. The caller
// still proves native entry/code/Task and performs genuine lifetime joins.
template <typename Request, typename Kind>
struct NativeAvatarRequestEntry {
  std::shared_ptr<Request> request;
  uint64_t request_serial = 0, invocation = 0;
  Kind kind{};
  bool Matches(Kind actual) const noexcept {
    return request && request_serial && invocation && actual != Kind{} &&
        kind == actual && request->serial == request_serial;
  }
};
// Pairing metadata for already classified native entries only. A passthrough
// marker cannot admit Avatar work, finish a Request or change native results.
// Binding is the exact target retained by successful entry, never reacquired
// from a recycled physical frame at return. Caller serializes this stack and
// transfers a returned binding into an EMPTY outer holder before lock release.
enum class NativeAvatarEntryReturn { kAbsent, kOwned, kPassthrough, kMismatch };
template <typename Frame, typename Binding = uint64_t>
class NativeAvatarEntryStack final {
 public:
  bool empty() const noexcept { return frames_.empty(); }
  const Frame& expected() const noexcept { return frames_.back().frame; }
  void EnterOwned(const Frame& frame, Binding binding = {}) {
    frames_.push_back({frame, true, std::move(binding)});
  }
  bool EnterPassthroughIfNested(const Frame& frame) {
    if(frames_.empty())return false;
    frames_.push_back({frame,false,{}});return true;
  }
  NativeAvatarEntryReturn ObserveReturn(const Frame& frame,
                                       Binding* returned_binding = nullptr) {
    if(frames_.empty())return NativeAvatarEntryReturn::kAbsent;
    if(frames_.back().frame!=frame)return NativeAvatarEntryReturn::kMismatch;
    const bool owned=frames_.back().owned;
    if (owned && returned_binding)
      *returned_binding = std::move(frames_.back().binding);
    frames_.pop_back();
    return owned?NativeAvatarEntryReturn::kOwned:NativeAvatarEntryReturn::kPassthrough;
  }
 private:
  struct Entry { Frame frame;bool owned;Binding binding; };
  std::vector<Entry> frames_;
};

// 17559 LoadAnimation's caller-owned control and serialized payload. These
// sizes describe valid native-format output, not an arbitrary malformed-asset
// bound. Borrowed pages do not own guest suballocations or freeze field values.
struct NativeAvatarAnimationBuffers {
  static constexpr uint32_t kControlBytes = 0x5990;
  static constexpr uint32_t kCapacityOffset = 0x5988;
  static constexpr uint32_t kDataOffset = 0x598C;
  uint32_t control = 0, capacity = 0, data = 0;
  bool Matches(uint32_t actual_control,uint32_t actual_capacity,
               uint32_t actual_data) const noexcept {
    return control==actual_control && capacity==actual_capacity && data==actual_data;
  }
};
std::optional<NativeAvatarAnimationBuffers> CaptureNativeAvatarAnimationBuffers(
    uint32_t control,uint32_t capacity,uint32_t data) noexcept;
// Only scalar native frame relations, never Task/source/output authority.
enum class NativeAvatarAnimationEdge { kWorkerJob, kJobCore, kCoreWriter };
bool MatchesNativeAvatarAnimationEdge(NativeAvatarAnimationEdge,uint32_t parent_sp,
                                     uint32_t child_sp,uint32_t child_lr) noexcept;

enum class NativeAvatarPlanFamily { kAbsent, kComplete, kInvalid };
NativeAvatarPlanFamily CheckNativeAvatarPlanFamily(
    const ValidatedGuestRuntimePlan&, std::string* error = nullptr);
}  // namespace xe::kernel
#endif
