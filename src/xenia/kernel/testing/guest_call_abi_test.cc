/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia contributors. All rights reserved.                    *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <vector>

#include "third_party/catch/include/catch.hpp"
#include "xenia/base/byte_order.h"
#include "xenia/cpu/guest_call_abi.h"
#include "xenia/kernel/native_system_ui_public_calls.h"
#include "xenia/memory.h"
#include "xenia/xbox.h"

namespace xe::cpu::test {
namespace {

using Argument = GuestCallArgument;
constexpr uint32_t kStackBase = 0x90011000;
constexpr uint32_t kPageSize = 0x1000;
constexpr uint32_t kCallerSp = kStackBase + 2 * kPageSize;
constexpr uint32_t kEntrySp = kCallerSp - kGuestCallExecuteStackPadding;
constexpr uint32_t kFunction = 0x81001000;
constexpr uint64_t kCallerLr = 0x123456789ABCDEF0;

// These tests inject a host executor to inspect the actual argument packer.
// The observed export8CF/8DB layouts are ABI inputs, not console goldens or
// claims that either native guest export has executed here.
class AbiFixture {
 public:
  AbiFixture() {
    REQUIRE(memory.Initialize());
    auto* heap = memory.LookupHeap(kStackBase);
    REQUIRE(heap);
    REQUIRE(heap->AllocFixed(kStackBase - kPageSize, 4 * kPageSize, kPageSize,
                             kMemoryAllocationReserve,
                             kMemoryProtectRead | kMemoryProtectWrite));
    REQUIRE(heap->AllocFixed(kStackBase, 2 * kPageSize, kPageSize,
                             kMemoryAllocationCommit,
                             kMemoryProtectRead | kMemoryProtectWrite));
    std::memset(memory.TranslateVirtual(kStackBase), 0xA5, 2 * kPageSize);
    context.r[1] = kCallerSp;
    context.lr = kCallerLr;
    context.virtual_membase = memory.virtual_membase();
    for (size_t i = 0; i < original_gprs.size(); ++i) {
      original_gprs[i] = context.r[3 + i] = 0xFEDCBA9800000100 + i;
    }
    for (size_t i = 0; i < original_fprs.size(); ++i) {
      // Include an exact payload to catch loss during state restoration.
      const uint64_t bits =
          i == 0 ? 0x7FF8000012345678 : std::bit_cast<uint64_t>(100.5 + i);
      original_fprs[i] = bits;
      context.f[1 + i] = std::bit_cast<double>(bits);
    }
    std::memcpy(original_frame.data(), memory.TranslateVirtual(kEntrySp),
                original_frame.size());
  }

  uint32_t Word(uint32_t address) const {
    return xe::load_and_swap<uint32_t>(memory.TranslateVirtual(address));
  }
  GuestCallResult Call(std::span<const Argument> arguments,
                       const GuestCallExecutor& executor) {
    return ExecuteGuestCallWithExecutor(&memory, &context, kFunction, arguments,
                                        executor);
  }
  void CheckRestored() const {
    CHECK(context.r[1] == kCallerSp);
    CHECK(context.lr == kCallerLr);
    for (size_t i = 0; i < original_gprs.size(); ++i) {
      CAPTURE(i);
      CHECK(context.r[3 + i] == original_gprs[i]);
    }
    for (size_t i = 0; i < original_fprs.size(); ++i) {
      CAPTURE(i);
      CHECK(std::bit_cast<uint64_t>(context.f[1 + i]) == original_fprs[i]);
    }
    CHECK(std::memcmp(memory.TranslateVirtual(kEntrySp), original_frame.data(),
                      original_frame.size()) == 0);
  }

  Memory memory;
  ppc::PPCContext context{};
  std::array<uint64_t, 8> original_gprs;
  std::array<uint64_t, kGuestCallMaxFloat32Arguments> original_fprs;
  std::array<uint8_t, kGuestCallExecuteStackPadding> original_frame;
};

TEST_CASE("Checked call packs the observed twelve-DWORD texture header ABI",
          "[kernel][guest_call_abi]") {
  AbiFixture fixture;
  constexpr uint32_t header = 0x90031000;
  const std::array arguments{
      Argument::UInt32(640),        Argument::UInt32(480),
      Argument::UInt32(1),          Argument::UInt32(0),
      Argument::UInt32(0x18280106), Argument::UInt32(0),
      Argument::UInt32(0),          Argument::UInt32(0),
      Argument::UInt32(0),          Argument::UInt32(header),
      Argument::UInt32(0),          Argument::UInt32(0)};
  unsigned calls = 0;
  const auto result = fixture.Call(arguments, [&](uint32_t address) {
    ++calls;
    CHECK(address == kFunction);
    // Mirror only Processor::Execute's documented entry-frame adjustment.
    fixture.context.r[1] -= kGuestCallExecuteStackPadding;
    fixture.context.lr = 0xBCBCBCBC;
    CHECK(fixture.context.r[3] == 640);
    CHECK(fixture.context.r[4] == 480);
    CHECK(fixture.context.r[7] == 0x18280106);
    CHECK(fixture.context.r[10] == 0);
    CHECK(fixture.Word(kEntrySp + 0x54) == 0);       // Logical9.
    CHECK(fixture.Word(kEntrySp + 0x5C) == header);  // Logical10.
    CHECK(fixture.Word(kEntrySp + 0x64) == 0);
    CHECK(fixture.Word(kEntrySp + 0x6C) == 0);
    CHECK(fixture.Word(kEntrySp + 0x58) == 0xA5A5A5A5);
    fixture.context.r[3] = 0x200000;
    return true;
  });
  CHECK(calls == 1);
  CHECK(result.executed);
  CHECK(result.error == GuestCallError::kNone);
  CHECK(result.value == 0x200000);
  fixture.CheckRestored();
}

// These exercise the real checked CPU packer/Memory stack with an executor
// inspection seam. They do not execute native XAM or fabricate a Task/TLS.
TEST_CASE(
    "Original caller MessageBox keeps ninth pointer and native return bits",
    "[kernel][guest_call_abi][native_public]") {
  AbiFixture fixture;
  const auto* profile = kernel::NativeSystemUiPublicProfile(
      kernel::NativeSystemUiPublicCall::kShowMessageBox);
  REQUIRE(profile);
  REQUIRE(profile->argument_count == 9);  // Native load oldSP+54 at816C5514.
  constexpr uint32_t overlap = 0x90035000;
  constexpr uint64_t original_r13 = 0x1234567890001000;
  fixture.context.r[13] = original_r13;
  const std::array arguments{
      Argument::UInt32(0),          Argument::UInt32(0x90031000),
      Argument::UInt32(0x90032000), Argument::UInt32(3),
      Argument::UInt32(0x90033000), Argument::UInt32(2),
      Argument::UInt32(0x100),      Argument::UInt32(0x90034000),
      Argument::UInt32(overlap)};
  const auto returned = GENERATE(0u, 87u, 997u, 0x8007000Eu);
  const auto result = fixture.Call(arguments, [&](uint32_t) {
    fixture.context.r[1] -= kGuestCallExecuteStackPadding;
    fixture.context.lr = 0xABABABAB;
    CHECK(fixture.context.r[3] == 0);
    CHECK(fixture.context.r[10] == 0x90034000);
    CHECK(fixture.Word(kEntrySp + 0x54) == overlap);
    CHECK(fixture.context.r[13] == original_r13);
    fixture.context.r[3] = returned;
    return true;
  });
  CHECK(result.executed);
  CHECK(result.value ==
        returned);  // Raw status, not host boolean/modal result.
  CHECK(fixture.context.r[13] == original_r13);
  fixture.CheckRestored();
}

TEST_CASE("Original caller nine-word call restores stack on failure or throw",
          "[kernel][guest_call_abi][native_public]") {
  AbiFixture fixture;
  const bool throws = GENERATE(false, true);
  std::array<Argument, 9> arguments;
  for (size_t i = 0; i < arguments.size(); ++i) {
    arguments[i] = Argument::UInt32(uint32_t(0x90030000 + i * 0x100));
  }
  const auto execute = [&](uint32_t) -> bool {
    fixture.context.r[1] -= kGuestCallExecuteStackPadding;
    fixture.context.lr = 0;
    CHECK(fixture.Word(kEntrySp + 0x54) == 0x90030800);
    if (throws) {
      throw std::runtime_error("CPU packer restoration only");
    }
    return false;
  };
  if (throws) {
    CHECK_THROWS_AS(fixture.Call(arguments, execute), std::runtime_error);
  } else {
    const auto result = fixture.Call(arguments, execute);
    CHECK_FALSE(result.executed);
    CHECK(result.error == GuestCallError::kExecutionFailed);
  }
  fixture.CheckRestored();
}

TEST_CASE("Native SetState forwards all six original scalar register arguments",
          "[kernel][guest_call_abi][native_public]") {
  AbiFixture fixture;
  const auto* profile = kernel::NativeSystemUiPublicProfile(
      kernel::NativeSystemUiPublicCall::kInputSetState);
  REQUIRE(profile);
  REQUIRE(profile->argument_count == 6);
  const std::array arguments{
      Argument::UInt32(2),          Argument::UInt32(0x80000000),
      Argument::UInt32(0x90031000), Argument::UInt32(0x12345678),
      Argument::UInt32(0xAABBCCDD), Argument::UInt32(0xFEDCBA98)};
  const auto result = fixture.Call(arguments, [&](uint32_t) {
    for (size_t i = 0; i < arguments.size(); ++i) {
      CHECK(fixture.context.r[3 + i] == arguments[i].bits);
    }
    fixture.context.r[3] = 1167;
    return true;
  });
  CHECK(result.executed);
  CHECK(result.value == 1167);
  fixture.CheckRestored();
}

TEST_CASE("Device selector keeps requested bytes in one full PPC register",
          "[kernel][guest_call_abi][native_public]") {
  AbiFixture fixture;
  constexpr auto call = kernel::NativeSystemUiPublicCall::kShowDeviceSelector;
  const auto* profile = kernel::NativeSystemUiPublicProfile(call);
  REQUIRE(profile);
  REQUIRE(profile->argument_count == 6);
  constexpr uint64_t requested = 0x12345678ABCDEF01;
  const std::array<uint64_t, 6> values{2,         1,          0x10000,
                                       requested, 0x90033000, 0x90034000};
  REQUIRE(kernel::NativeSystemUiPublicArgumentsValid(call, values));
  const std::array arguments{
      Argument::UInt32(2),          Argument::UInt32(1),
      Argument::UInt32(0x10000),    Argument::UInt64Register(requested),
      Argument::UInt32(0x90033000), Argument::UInt32(0x90034000)};
  const auto result = fixture.Call(arguments, [&](uint32_t) {
    // Native 2CB moves r6 to r7, and its shared body stores the complete
    // qword. A DWORD pair would also shift out-device and overlap pointers.
    for (size_t i = 0; i < values.size(); ++i) {
      CHECK(fixture.context.r[3 + i] == values[i]);
    }
    CHECK(fixture.context.r[9] == fixture.original_gprs[6]);
    CHECK(fixture.context.r[10] == fixture.original_gprs[7]);
    fixture.context.r[3] = 997;
    return true;
  });
  CHECK(result.executed);
  CHECK(result.value == 997);
  fixture.CheckRestored();

  auto malformed = values;
  for (size_t i : {size_t(0), size_t(1), size_t(2), size_t(4), size_t(5)}) {
    malformed = values;
    malformed[i] |= uint64_t(1) << 32;
    CHECK_FALSE(kernel::NativeSystemUiPublicArgumentsValid(call, malformed));
  }
  const std::array<uint64_t, 1> guide{uint64_t(1) << 32};
  CHECK_FALSE(kernel::NativeSystemUiPublicArgumentsValid(
      kernel::NativeSystemUiPublicCall::kShowGuide, guide));
}

TEST_CASE("Wide register failures restore full caller register state",
          "[kernel][guest_call_abi]") {
  AbiFixture fixture;
  const bool throws = GENERATE(false, true);
  const std::array arguments{Argument::UInt32(0), Argument::Float32(1.25f),
                             Argument::UInt64Register(UINT64_MAX)};
  const auto execute = [&](uint32_t) -> bool {
    CHECK(fixture.context.r[5] == UINT64_MAX);
    CHECK(fixture.context.f[1] == 1.25);
    fixture.context.r[1] = 0;
    fixture.context.lr = 0;
    fixture.context.r[5] = 0;
    if (throws) {
      throw std::runtime_error("wide call interrupted");
    }
    return false;
  };
  if (throws) {
    CHECK_THROWS_AS(fixture.Call(arguments, execute), std::runtime_error);
  } else {
    CHECK(fixture.Call(arguments, execute).error ==
          GuestCallError::kExecutionFailed);
  }
  fixture.CheckRestored();
}

TEST_CASE("Unestablished wide stack and malformed narrow values never execute",
          "[kernel][guest_call_abi]") {
  AbiFixture fixture;
  unsigned calls = 0;
  const auto execute = [&](uint32_t) {
    ++calls;
    return true;
  };
  std::array<Argument, 9> arguments;
  std::fill(arguments.begin(), arguments.end(), Argument::UInt32(1));
  arguments.back() = Argument::UInt64Register(0x100000000);
  CHECK(fixture.Call(arguments, execute).error ==
        GuestCallError::kInvalidArguments);
  for (auto kind : {Argument::Kind::kUInt32, Argument::Kind::kFloat32}) {
    const std::array malformed{Argument{kind, 0x100000000}};
    CHECK(fixture.Call(malformed, execute).error ==
          GuestCallError::kInvalidArguments);
  }
  CHECK(calls == 0);
  fixture.CheckRestored();
}

TEST_CASE(
    "Native193 keeps user and flags while198 keeps a genuine guest index "
    "pointer",
    "[kernel][guest_call_abi][native_public]") {
  AbiFixture fixture;
  const auto* direct = kernel::NativeSystemUiPublicProfile(
      kernel::NativeSystemUiPublicCall::kInputGetKeystroke);
  const auto* indirect = kernel::NativeSystemUiPublicProfile(
      kernel::NativeSystemUiPublicCall::kInputGetKeystrokeEx);
  REQUIRE(direct);
  REQUIRE(indirect);
  CHECK(direct->address == 0x816F4AB8);
  CHECK(direct->argument_count == 3);
  CHECK(indirect->address == 0x816F4778);
  CHECK(indirect->argument_count == 3);
  constexpr uint32_t index = kStackBase + 0x100, stroke = kStackBase + 0x200;
  xe::store_and_swap<uint32_t>(fixture.memory.TranslateVirtual(index), 3);
  const std::array values{Argument::UInt32(3), Argument::UInt32(0x80000001),
                          Argument::UInt32(stroke)};
  const auto a = fixture.Call(values, [&](uint32_t) {
    CHECK(fixture.context.r[3] == 3);
    CHECK(fixture.context.r[4] == 0x80000001);
    CHECK(fixture.context.r[5] == stroke);
    fixture.context.r[3] = X_ERROR_EMPTY;
    return true;
  });
  CHECK(a.executed);
  CHECK(a.value == X_ERROR_EMPTY);
  fixture.CheckRestored();
  const std::array pointers{Argument::UInt32(index),
                            Argument::UInt32(0x40000001),
                            Argument::UInt32(stroke)};
  const auto b = fixture.Call(pointers, [&](uint32_t) {
    CHECK(fixture.context.r[3] == index);
    CHECK(fixture.Word(index) == 3);
    CHECK(fixture.context.r[4] == 0x40000001);
    CHECK(fixture.context.r[5] == stroke);
    xe::store_and_swap<uint32_t>(fixture.memory.TranslateVirtual(index), 2);
    fixture.context.r[3] = X_ERROR_DEVICE_NOT_CONNECTED;
    return true;
  });
  CHECK(b.executed);
  CHECK(b.value == X_ERROR_DEVICE_NOT_CONNECTED);
  CHECK(fixture.Word(index) == 2);
  fixture.CheckRestored();
}

TEST_CASE("Resolve Float32 logical9 leaves its DWORD home untouched",
          "[kernel][guest_call_abi]") {
  AbiFixture fixture;
  const std::array arguments{
      Argument::UInt32(0x40001000), Argument::UInt32(0),
      Argument::UInt32(0x90032000), Argument::UInt32(0x90031000),
      Argument::UInt32(0),          Argument::UInt32(0),
      Argument::UInt32(0),          Argument::UInt32(0),
      Argument::Float32(0.0f),      Argument::UInt32(0),
      Argument::UInt32(0)};
  const auto result = fixture.Call(arguments, [&](uint32_t) {
    fixture.context.r[1] -= kGuestCallExecuteStackPadding;
    CHECK(fixture.context.f[1] == 0.0);
    CHECK_FALSE(std::signbit(fixture.context.f[1]));
    CHECK(fixture.context.r[3] == 0x40001000);
    CHECK(fixture.context.r[5] == 0x90032000);
    CHECK(fixture.context.r[6] == 0x90031000);
    CHECK(fixture.Word(kEntrySp + 0x54) == 0xA5A5A5A5);
    CHECK(fixture.Word(kEntrySp + 0x5C) == 0);
    CHECK(fixture.Word(kEntrySp + 0x64) == 0);
    fixture.context.r[3] = 0;
    return true;
  });
  CHECK(result.executed);
  CHECK(result.value == 0);
  fixture.CheckRestored();
}

TEST_CASE("Mixed logical slots keep integer positions and sequential FPRs",
          "[kernel][guest_call_abi]") {
  AbiFixture fixture;
  const std::array arguments{
      Argument::UInt32(0xFFFFFFFF), Argument::Float32(1.25f),
      Argument::UInt32(3),          Argument::Float32(-2.5f),
      Argument::UInt32(5),          Argument::UInt32(6),
      Argument::UInt32(7),          Argument::UInt32(8),
      Argument::Float32(3.75f),     Argument::UInt32(10),
      Argument::Float32(-0.0f),     Argument::UInt32(12)};
  const auto result = fixture.Call(arguments, [&](uint32_t) {
    CHECK(fixture.context.r[3] == UINT32_MAX);  // No stale upper DWORD.
    CHECK(fixture.context.r[4] == fixture.original_gprs[1]);
    CHECK(fixture.context.r[5] == 3);
    CHECK(fixture.context.r[6] == fixture.original_gprs[3]);
    CHECK(fixture.context.r[7] == 5);
    CHECK(fixture.context.r[10] == 8);
    CHECK(fixture.context.f[1] == 1.25);
    CHECK(fixture.context.f[2] == -2.5);
    CHECK(fixture.context.f[3] == 3.75);
    CHECK(std::signbit(fixture.context.f[4]));
    CHECK(fixture.Word(kEntrySp + 0x54) == 0xA5A5A5A5);
    CHECK(fixture.Word(kEntrySp + 0x5C) == 10);
    CHECK(fixture.Word(kEntrySp + 0x64) == 0xA5A5A5A5);
    CHECK(fixture.Word(kEntrySp + 0x6C) == 12);
    fixture.context.r[3] = 0x80070057;
    return true;
  });
  CHECK(result.executed);  // ABI/result-policy failures are distinct.
  CHECK(result.value == 0x80070057);
  fixture.CheckRestored();
}

TEST_CASE("Sixteen DWORD arguments stop at the final owned stack home",
          "[kernel][guest_call_abi]") {
  AbiFixture fixture;
  std::array<Argument, 16> arguments;
  for (size_t i = 0; i < arguments.size(); ++i) {
    arguments[i] = Argument::UInt32(uint32_t(i + 1));
  }
  const auto result = fixture.Call(arguments, [&](uint32_t) {
    for (size_t i = 0; i < 8; ++i) {
      CHECK(fixture.context.r[3 + i] == i + 1);
      CHECK(fixture.Word(kEntrySp + 0x54 + uint32_t(i) * 8) == i + 9);
    }
    CHECK(fixture.Word(kEntrySp + 0x50) == 0xA5A5A5A5);
    CHECK(fixture.Word(kEntrySp + 0x90) == 0xA5A5A5A5);
    fixture.context.r[3] = 16;
    return true;
  });
  CHECK(result.executed);
  CHECK(result.value == 16);
  fixture.CheckRestored();
}

TEST_CASE("Checked failure and exception restore homes SP LR and arguments",
          "[kernel][guest_call_abi]") {
  AbiFixture fixture;
  std::array<Argument, 10> arguments;
  std::fill(arguments.begin(), arguments.end(), Argument::UInt32(0x11223344));
  const auto clobber = [&] {
    fixture.context.r[1] = kEntrySp - 0x40;
    fixture.context.lr = 0xBCBCBCBC;
    fixture.context.r[3] = 0xDEADBABE;
    fixture.context.f[1] = 200.0;
    xe::store_and_swap<uint32_t>(
        fixture.memory.TranslateVirtual(kEntrySp + 0x54), 0xBAD0);
  };
  SECTION("Execution failure has no synthetic return value") {
    const auto result = fixture.Call(arguments, [&](uint32_t) {
      clobber();
      return false;
    });
    CHECK_FALSE(result.executed);
    CHECK(result.error == GuestCallError::kExecutionFailed);
    CHECK(result.value == 0);
  }
  SECTION("Executor exceptions propagate after restoring owned state") {
    REQUIRE_THROWS_AS(fixture.Call(arguments,
                                   [&](uint32_t) -> bool {
                                     clobber();
                                     throw std::runtime_error(
                                         "injected execution interruption");
                                   }),
                      std::runtime_error);
  }
  fixture.CheckRestored();
}

TEST_CASE("Invalid ABI requests cannot partially modify call state",
          "[kernel][guest_call_abi]") {
  AbiFixture fixture;
  unsigned calls = 0;
  auto execute = [&](uint32_t) {
    ++calls;
    return true;
  };
  std::vector<Argument> arguments(17, Argument::UInt32(1));
  CHECK(fixture.Call(arguments, execute).error ==
        GuestCallError::kInvalidArguments);
  arguments.resize(10);
  arguments.back().kind = static_cast<Argument::Kind>(99);
  CHECK(fixture.Call(arguments, execute).error ==
        GuestCallError::kInvalidArguments);
  arguments.assign(14, Argument::Float32(1.0f));
  CHECK(fixture.Call(arguments, execute).error ==
        GuestCallError::kInvalidArguments);
  CHECK(ExecuteGuestCallWithExecutor(&fixture.memory, &fixture.context, 0, {},
                                     execute)
            .error == GuestCallError::kInvalidInvocation);
  CHECK(ExecuteGuestCallWithExecutor(&fixture.memory, &fixture.context,
                                     kFunction + 1, {}, execute)
            .error == GuestCallError::kInvalidInvocation);
  CHECK(ExecuteGuestCallWithExecutor(&fixture.memory, &fixture.context,
                                     kFunction, {}, {})
            .error == GuestCallError::kInvalidInvocation);
  CHECK(ExecuteGuestCall(nullptr, nullptr, kFunction, {}).error ==
        GuestCallError::kInvalidInvocation);
  CHECK(calls == 0);
  fixture.CheckRestored();
}

TEST_CASE("Stack validation rejects guards readonly and unrepresentable SP",
          "[kernel][guest_call_abi]") {
  AbiFixture fixture;
  unsigned calls = 0;
  auto execute = [&](uint32_t) {
    ++calls;
    return true;
  };
  std::array<Argument, 16> arguments;
  std::fill(arguments.begin(), arguments.end(), Argument::UInt32(0));
  SECTION("Frame crosses a late guard after earlier writable homes") {
    REQUIRE(fixture.memory.LookupHeap(kStackBase)
                ->Protect(kStackBase + kPageSize, kPageSize,
                          kMemoryProtectNoAccess));
    fixture.context.r[1] = kStackBase + kPageSize + 0x50;
    CHECK(fixture.Call(arguments, execute).error ==
          GuestCallError::kStackNotWritable);
    // Logical9/10 would have touched the writable page before logical11 hit
    // the guard. Full preflight must leave those earlier homes untouched.
    CHECK(fixture.Word(kStackBase + kPageSize - 12) == 0xA5A5A5A5);
    CHECK(fixture.Word(kStackBase + kPageSize - 4) == 0xA5A5A5A5);
  }
  SECTION("Read-only argument homes") {
    REQUIRE(fixture.memory.LookupHeap(kStackBase)
                ->Protect(kStackBase, 2 * kPageSize, kMemoryProtectRead));
    CHECK(fixture.Call(arguments, execute).error ==
          GuestCallError::kStackNotWritable);
    fixture.CheckRestored();
  }
  SECTION(
      "SP underflow or unaligned/high bits are rejected without truncation") {
    for (uint64_t invalid_sp :
         {uint64_t(0), uint64_t(128), uint64_t(kCallerSp + 1),
          uint64_t(kCallerSp) + (uint64_t(1) << 32)}) {
      fixture.context.r[1] = invalid_sp;
      CHECK(fixture.Call(arguments, execute).error ==
            GuestCallError::kInvalidStack);
    }
    fixture.context.r[1] = kCallerSp;
    fixture.CheckRestored();
  }
  CHECK(calls == 0);
  CHECK(fixture.context.lr == kCallerLr);
}

TEST_CASE("Checked packer owns its executor until restored return",
          "[kernel][guest_call_abi]") {
  AbiFixture fixture;
  std::unique_ptr<GuestCallExecutor> executor;
  executor = std::make_unique<GuestCallExecutor>([&](uint32_t) {
    executor.reset();
    fixture.context.r[3] = 0x55;
    return true;
  });
  const auto result = fixture.Call({}, *executor);
  CHECK_FALSE(executor);
  CHECK(result.executed);
  CHECK(result.value == 0x55);
  fixture.CheckRestored();
}

TEST_CASE("Native UI-active query has no arguments and preserves DWORD result",
          "[kernel][guest_call_abi][native_public][native_ui_state]") {
  AbiFixture fixture;
  const auto* profile = kernel::NativeSystemUiPublicProfile(
      kernel::NativeSystemUiPublicCall::kIsUiActive);
  REQUIRE(profile);
  CHECK(uint16_t(profile->call) == 0x2EC);
  CHECK(profile->address == 0x816C7D78);
  CHECK(profile->code_size == 16);
  CHECK(profile->argument_count == 0);
  constexpr uint64_t original_tls = 0x1234567890001000;
  fixture.context.r[13] = original_tls;
  const auto returned = GENERATE(0u, 1u, 2u, 0x80000000u);
  unsigned calls = 0;
  const auto result = fixture.Call({}, [&](uint32_t) {
    ++calls;
    // This zero-argument native leaf reads no incoming argument register.
    CHECK(fixture.context.r[3] == fixture.original_gprs[0]);
    CHECK(fixture.context.r[13] == original_tls);
    fixture.context.r[1] -= kGuestCallExecuteStackPadding;
    fixture.context.lr = 0xABABABAB;
    fixture.context.r[3] = returned;
    return true;
  });
  CHECK(calls == 1);
  CHECK(result.executed);
  CHECK(result.value == returned);
  CHECK(fixture.context.r[13] == original_tls);
  fixture.CheckRestored();
}

}  // namespace
}  // namespace xe::cpu::test

namespace xe::cpu::test {
TEST_CASE(
    "Native notification Ex context occupies r10 without shifting other fields",
    "[kernel][guest_call_abi][native_notification]") {
  AbiFixture fixture;
  constexpr uint64_t areas = 0x1234567800000001;
  constexpr uint64_t context = 0xFEDCBA9876543210;
  const std::array arguments{Argument::UInt32(0x1B),
                             Argument::UInt32(3),
                             Argument::UInt64Register(areas),
                             Argument::UInt32(0x90033000),
                             Argument::UInt32(0xFE),
                             Argument::UInt32(0x90034000),
                             Argument::UInt32(0),
                             Argument::UInt64Register(context)};
  const auto result = fixture.Call(arguments, [&](uint32_t) {
    CHECK(fixture.context.r[3] == 0x1B);
    CHECK(fixture.context.r[4] == 3);
    CHECK(fixture.context.r[5] == areas);
    CHECK(fixture.context.r[6] == 0x90033000);
    CHECK(fixture.context.r[7] == 0xFE);
    CHECK(fixture.context.r[8] == 0x90034000);
    CHECK(fixture.context.r[9] == 0);
    CHECK(fixture.context.r[10] == context);
    fixture.context.r[3] = 1;  // Genuine suppressed success, not fallback.
    return true;
  });
  REQUIRE(result.executed);
  CHECK(result.value == 1);
  fixture.CheckRestored();
}
}  // namespace xe::cpu::test
