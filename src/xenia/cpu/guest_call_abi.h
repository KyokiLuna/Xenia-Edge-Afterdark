/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia contributors. All rights reserved.                    *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_CPU_GUEST_CALL_ABI_H_
#define XENIA_CPU_GUEST_CALL_ABI_H_

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>

#include "xenia/cpu/ppc/ppc_context.h"

namespace xe {
class Memory;
namespace cpu {
class Processor;
class ThreadState;

// Processor::Execute(bool) reserves this frame before entering guest code.
// Keep its implementation and stack argument placement on the same constant.
inline constexpr uint32_t kGuestCallExecuteStackPadding = 64 + 112;
inline constexpr uint32_t kGuestCallStackArgumentOffset = 0x54;
inline constexpr uint32_t kGuestCallStackArgumentStride = 8;
inline constexpr size_t kGuestCallMaxArguments = 16;
inline constexpr size_t kGuestCallMaxFloat32Arguments = 13;

struct GuestCallArgument {
  enum class Kind { kUInt32, kFloat32, kUInt64Register };
  Kind kind = Kind::kUInt32;
  uint64_t bits = 0;

  static GuestCallArgument UInt32(uint32_t value);
  static GuestCallArgument Float32(float value);
  // A single PPC GPR, not a pair of DWORD arguments. Stack UInt64 arguments
  // require a separately established ABI and are rejected here.
  static GuestCallArgument UInt64Register(uint64_t value);
};

enum class GuestCallError {
  kNone,
  kInvalidInvocation,
  kInvalidArguments,
  kInvalidStack,
  kStackNotWritable,
  kExecutionFailed,
};

struct GuestCallResult {
  // True only if the checked executor completed. A returned guest HRESULT or
  // zero pointer is still an executed call; the caller owns that ABI policy.
  bool executed = false;
  uint64_t value = 0;
  GuestCallError error = GuestCallError::kNone;
};

// Prototyped UInt32/Float32 and register-only UInt64 calls: no variadic,
// stack UInt64, Float64, vector, aggregate or floating overflow ABI is
// inferred. Float arguments use sequential f1..f13 but consume their original
// logical position; integer arguments use r3..r10 or a DWORD stack home by that
// position. No floating argument is speculatively mirrored into a GPR or stack
// home.
//
// Owns/restores argument GPRs/FPRs, written DWORD homes and caller SP/LR even
// when Execute fails or throws. Exceptions propagate after restoration. The
// caller must keep this guest stack mapping writable for the synchronous call.
GuestCallResult ExecuteGuestCall(Processor* processor, ThreadState* thread,
                                 uint32_t address,
                                 std::span<const GuestCallArgument> arguments);

// Injection seam for packing/restoration tests. The executor follows the same
// contract as Processor::Execute(bool), including its entry-frame padding;
// passing a host callback here is not evidence of actual guest execution.
using GuestCallExecutor = std::function<bool(uint32_t address)>;
GuestCallResult ExecuteGuestCallWithExecutor(
    Memory* memory, ppc::PPCContext* context, uint32_t address,
    std::span<const GuestCallArgument> arguments,
    const GuestCallExecutor& executor);

}  // namespace cpu
}  // namespace xe

#endif  // XENIA_CPU_GUEST_CALL_ABI_H_
