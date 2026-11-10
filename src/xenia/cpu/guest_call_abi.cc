/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia contributors. All rights reserved.                    *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/cpu/guest_call_abi.h"

#include <array>
#include <bit>
#include <cstring>

#include "xenia/base/byte_order.h"
#include "xenia/cpu/processor.h"
#include "xenia/cpu/thread_state.h"
#include "xenia/memory.h"

namespace xe::cpu {
namespace {

static_assert(kGuestCallStackArgumentOffset +
                  (kGuestCallMaxArguments - 9) * kGuestCallStackArgumentStride +
                  sizeof(uint32_t) <=
              kGuestCallExecuteStackPadding);

struct StackHome {
  uint32_t address;
  std::array<uint8_t, sizeof(uint32_t)> original;
};

class SavedCallState {
 public:
  SavedCallState(Memory* memory, ppc::PPCContext* context)
      : memory_(memory),
        context_(context),
        sp_(context->r[1]),
        lr_(context->lr) {
    for (size_t i = 0; i < gprs_.size(); ++i) {
      gprs_[i] = context->r[3 + i];
    }
    for (size_t i = 0; i < fprs_.size(); ++i) {
      fprs_[i] = std::bit_cast<uint64_t>(context->f[1 + i]);
    }
  }
  SavedCallState(const SavedCallState&) = delete;
  SavedCallState& operator=(const SavedCallState&) = delete;
  ~SavedCallState() {
    for (size_t i = 0; i < home_count_; ++i) {
      const auto& home = homes_[i];
      std::memcpy(memory_->TranslateVirtual(home.address), home.original.data(),
                  home.original.size());
    }
    for (size_t i = 0; i < gprs_.size(); ++i) {
      context_->r[3 + i] = gprs_[i];
    }
    for (size_t i = 0; i < fprs_.size(); ++i) {
      context_->f[1 + i] = std::bit_cast<double>(fprs_[i]);
    }
    context_->r[1] = sp_;
    context_->lr = lr_;
  }
  void SaveHome(uint32_t address) {
    auto& home = homes_[home_count_++];
    home.address = address;
    std::memcpy(home.original.data(), memory_->TranslateVirtual(address),
                home.original.size());
  }

 private:
  Memory* memory_;
  ppc::PPCContext* context_;
  uint64_t sp_;
  uint64_t lr_;
  std::array<uint64_t, 8> gprs_;
  std::array<uint64_t, kGuestCallMaxFloat32Arguments> fprs_;
  std::array<StackHome, kGuestCallMaxArguments - 8> homes_;
  size_t home_count_ = 0;
};

}  // namespace

GuestCallArgument GuestCallArgument::UInt32(uint32_t value) {
  return {Kind::kUInt32, value};
}

GuestCallArgument GuestCallArgument::Float32(float value) {
  return {Kind::kFloat32, std::bit_cast<uint32_t>(value)};
}

GuestCallArgument GuestCallArgument::UInt64Register(uint64_t value) {
  return {Kind::kUInt64Register, value};
}

GuestCallResult ExecuteGuestCallWithExecutor(
    Memory* memory, ppc::PPCContext* context, uint32_t address,
    std::span<const GuestCallArgument> arguments,
    const GuestCallExecutor& executor) {
  if (!memory || !context || !address || (address & 3) || !executor) {
    return {false, 0, GuestCallError::kInvalidInvocation};
  }
  if (arguments.size() > kGuestCallMaxArguments) {
    return {false, 0, GuestCallError::kInvalidArguments};
  }
  std::array<GuestCallArgument, kGuestCallMaxArguments> owned_arguments;
  size_t float_count = 0;
  for (size_t i = 0; i < arguments.size(); ++i) {
    const auto& argument = arguments[i];
    if (argument.kind == GuestCallArgument::Kind::kUInt64Register) {
      if (i >= 8) {
        return {false, 0, GuestCallError::kInvalidArguments};
      }
    } else if (argument.bits > UINT32_MAX) {
      // Malformed DWORD/pointer/Float32 values must not silently lose bits.
      return {false, 0, GuestCallError::kInvalidArguments};
    } else if (argument.kind == GuestCallArgument::Kind::kFloat32) {
      if (++float_count > kGuestCallMaxFloat32Arguments) {
        return {false, 0, GuestCallError::kInvalidArguments};
      }
    } else if (argument.kind != GuestCallArgument::Kind::kUInt32) {
      return {false, 0, GuestCallError::kInvalidArguments};
    }
    owned_arguments[i] = argument;
  }
  const uint64_t caller_sp = context->r[1];
  if (caller_sp > UINT32_MAX || caller_sp < kGuestCallExecuteStackPadding ||
      (caller_sp & 15)) {
    return {false, 0, GuestCallError::kInvalidStack};
  }
  const uint32_t callee_sp =
      uint32_t(caller_sp) - kGuestCallExecuteStackPadding;
  const auto access =
      memory->QueryRangeAccess(callee_sp, kGuestCallExecuteStackPadding);
  if (access != xe::memory::PageAccess::kReadWrite &&
      access != xe::memory::PageAccess::kExecuteReadWrite) {
    return {false, 0, GuestCallError::kStackNotWritable};
  }

  // Copy before modifying any caller state. A callback may erase the storage
  // that provided it, and argument storage may alias the saved call state.
  GuestCallExecutor owned_executor = executor;
  SavedCallState saved(memory, context);
  for (size_t i = 8; i < arguments.size(); ++i) {
    if (owned_arguments[i].kind == GuestCallArgument::Kind::kUInt32) {
      saved.SaveHome(callee_sp + kGuestCallStackArgumentOffset +
                     uint32_t(i - 8) * kGuestCallStackArgumentStride);
    }
  }
  size_t next_fpr = 1;
  for (size_t i = 0; i < arguments.size(); ++i) {
    const auto& argument = owned_arguments[i];
    if (argument.kind == GuestCallArgument::Kind::kFloat32) {
      context->f[next_fpr++] =
          static_cast<double>(std::bit_cast<float>(uint32_t(argument.bits)));
    } else if (i < 8) {
      context->r[3 + i] = argument.bits;
    } else {
      xe::store_and_swap<uint32_t>(
          memory->TranslateVirtual(callee_sp + kGuestCallStackArgumentOffset +
                                   uint32_t(i - 8) *
                                       kGuestCallStackArgumentStride),
          uint32_t(argument.bits));
    }
  }
  if (!owned_executor(address)) {
    return {false, 0, GuestCallError::kExecutionFailed};
  }
  return {true, context->r[3], GuestCallError::kNone};
}

GuestCallResult ExecuteGuestCall(Processor* processor, ThreadState* thread,
                                 uint32_t address,
                                 std::span<const GuestCallArgument> arguments) {
  if (!processor || !thread || thread->processor() != processor ||
      !processor->memory() || thread->memory() != processor->memory() ||
      !thread->context()) {
    return {false, 0, GuestCallError::kInvalidInvocation};
  }
  return ExecuteGuestCallWithExecutor(
      processor->memory(), thread->context(), address, arguments,
      [processor, thread](uint32_t target) {
        return processor->Execute(thread, target);
      });
}

}  // namespace xe::cpu
