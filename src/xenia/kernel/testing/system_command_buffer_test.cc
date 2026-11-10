/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia contributors. All rights reserved.                    *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <memory>
#include <unordered_map>
#include <utility>
#include <vector>

#include "third_party/catch/include/catch.hpp"
#include "xenia/base/byte_order.h"
#include "xenia/base/memory.h"
#include "xenia/gpu/system_command_buffer.h"

namespace xe::gpu::test {
namespace {
using State = SystemCommandBuffer;
constexpr uint32_t kAck = 0x90001000;
constexpr uint32_t kOtherAck = 0x90002000;

SystemCommandDescriptor Record(uint32_t address = 0xC1234000) {
  SystemCommandDescriptor result;
  xe::store_and_swap<uint32_t>(result.bytes.data(), address);
  xe::store_and_swap<uint32_t>(result.bytes.data() + 4, 3);
  return result;
}
State::OwnedSession Session(uint64_t backend_epoch = 1) {
  return {std::make_shared<int>(1), {std::make_shared<int>(2)}, backend_epoch};
}

class Fixture {
 public:
  Fixture() {
    REQUIRE(state.BeginOwnedSession(Session(gpu.backend_epoch())));
    state.SetIdentifierAddress(kAck);
    memory[kAck] = memory[kOtherAck] = 0;
    REQUIRE(state.Publish(Record()));
  }
  ~Fixture() {
    gpu.ReleaseAfterGpuStopped();
    state.ReleaseAfterGpuStopped();
  }
  State::AckReader Reader() {
    return [this](uint32_t address) -> std::optional<uint32_t> {
      const auto it = memory.find(address);
      return it == memory.end() ? std::nullopt
                                : std::optional<uint32_t>(it->second);
    };
  }
  State::AckWriter Writer() {
    return [this](uint32_t address, uint32_t observed, uint32_t identifier) {
      auto it = memory.find(address);
      if (it == memory.end() || it->second != observed) {
        return false;
      }
      it->second = identifier;
      writes.emplace_back(address, identifier);
      return true;
    };
  }
  State::Checkout Get() {
    auto result = state.Get(Reader(), Writer());
    REQUIRE(result);
    return *result;
  }
  State::SwapLease Bind(const State::Checkout& checkout) {
    auto result = state.BindSwap(checkout.identifier, checkout.descriptor,
                                 checkout.binding.address);
    REQUIRE(result.status == State::BindStatus::kBound);
    REQUIRE(result.lease);
    return result.lease;
  }
  bool Ready() { return state.QueryReady(0, Reader(), Writer()); }
  State state;
  GpuCompletionQueue gpu;
  std::unordered_map<uint32_t, uint32_t> memory;
  std::vector<std::pair<uint32_t, uint32_t>> writes;
};

TEST_CASE("System command records preserve the exact guest-endian descriptor",
          "[graphics][system_command]") {
  auto record = Record();
  for (size_t i = 0x20; i < record.bytes.size(); ++i) {
    record.bytes[i] = uint8_t(i ^ 0xA5);
  }
  xe::store_and_swap<uint32_t>(record.bytes.data() + 8, 0xA1234000);
  xe::store_and_swap<uint32_t>(record.bytes.data() + 0xC, 6);
  xe::store_and_swap<uint32_t>(record.bytes.data() + 0x14, 1280);
  xe::store_and_swap<uint32_t>(record.bytes.data() + 0x18, 720);
  const auto copied = SystemCommandDescriptor::Copy(record.bytes);
  REQUIRE(copied);
  CHECK(*copied == record);
  CHECK(copied->command_address() == 0xC1234000);
  CHECK(copied->command_dwords() == 3);
  CHECK(copied->texture_address() == 0xA1234000);
  CHECK(copied->texture_format() == 6);
  CHECK(copied->width() == 1280);
  CHECK(copied->height() == 720);
  CHECK(copied->has_texture());
  CHECK_FALSE(copied->empty());
  CHECK_FALSE(SystemCommandDescriptor::Copy(
      std::span(record.bytes).first(record.bytes.size() - 1)));
  record.bytes.fill(0);
  CHECK(record.empty());
  CHECK_FALSE(record.has_texture());
  CHECK_FALSE(copied->empty());  // No retained caller stack pointer.
}

TEST_CASE("System publication is unavailable without owned device and memory",
          "[graphics][system_command]") {
  State state;
  CHECK_FALSE(state.QueryReady());
  CHECK_FALSE(state.Get());
  CHECK_FALSE(state.Publish({}));
  CHECK_FALSE(state.BeginOwnedSession({}));
  CHECK_FALSE(state.BeginOwnedSession({std::make_shared<int>(1), {}}));
  CHECK_FALSE(state.BeginOwnedSession(Session(0)));
  REQUIRE(state.BeginOwnedSession(Session()));
  CHECK(state.QueryReady());
  CHECK_FALSE(state.QueryReady(1));
  REQUIRE(state.Publish({}));
  const auto empty = state.Get();
  REQUIRE(empty);
  CHECK(empty->identifier == 0);
  CHECK(state.QueryReady());
  auto texture = Record();
  xe::store_and_swap<uint32_t>(texture.bytes.data() + 0x14, 640);
  CHECK_FALSE(state.Publish(texture));
  CHECK(state.Publish(texture, {std::make_shared<int>(3)}));
  state.ReleaseAfterGpuStopped();
}

TEST_CASE(
    "Repeated Get checkouts preserve their publication and unique identity",
    "[graphics][system_command]") {
  Fixture f;
  const auto first = f.Get();
  const auto second = f.Get();
  CHECK(first.identifier != second.identifier);
  CHECK(first.publication_generation == second.publication_generation);
  CHECK(f.state.outstanding_checkouts() == 2);
  const auto replacement = Record(0xC5678000);
  REQUIRE(f.state.Publish(replacement));
  const auto third = f.Get();
  CHECK(third.publication_generation > first.publication_generation);
  CHECK(first.descriptor == Record());
  CHECK(third.descriptor == replacement);
  CHECK(f.state.BindSwap(first.identifier, replacement, kAck).status ==
        State::BindStatus::kDescriptorMismatch);
  const auto use = f.Bind(first);
  CHECK(use.descriptor() == first.descriptor);
  f.memory[kAck] = third.identifier;
  CHECK_FALSE(f.Ready());
  CHECK(f.state.outstanding_checkouts() ==
        2);  // First/second still outstanding.
}

TEST_CASE("Get samples a CPU skip ACK before issuing another checkout",
          "[graphics][system_command]") {
  Fixture f;
  const auto first = f.Get();
  f.memory[kAck] = first.identifier;
  const auto second = f.Get();
  CHECK(second.identifier != first.identifier);
  CHECK(f.state.outstanding_checkouts() == 1);
  CHECK_FALSE(f.Ready());
  f.memory[kAck] = second.identifier;
  CHECK(f.Ready());
  CHECK(f.writes.empty());
}

TEST_CASE("CPU ACK and clear never retire a queued or GPU-pending system use",
          "[graphics][system_command]") {
  Fixture f;
  const auto checkout = f.Get();
  auto use = f.Bind(checkout);
  f.memory[kAck] = checkout.identifier;
  CHECK_FALSE(f.Ready());
  REQUIRE(f.state.Publish({}));
  CHECK_FALSE(f.Ready());
  CHECK(f.writes.empty());
  const auto marker = f.gpu.TrackSubmitted(7, use.resources());
  REQUIRE(f.state.AttachCompletion(use, marker));
  f.gpu.CompleteThrough(6);
  CHECK_FALSE(f.Ready());
  CHECK(f.writes.empty());
  f.gpu.CompleteThrough(7);
  CHECK(f.Ready());
  REQUIRE(f.writes.size() == 1);
  CHECK(f.writes[0] ==
        std::pair<uint32_t, uint32_t>{kAck, checkout.identifier});
  CHECK(f.state.outstanding_checkouts() == 0);
}

TEST_CASE("Repeated Swap uses cannot replace an earlier completion fence",
          "[graphics][system_command]") {
  Fixture f;
  const auto checkout = f.Get();
  const auto first = f.Bind(checkout);
  const auto second = f.Bind(checkout);
  CHECK(first.use_id() != second.use_id());
  CHECK(first.owner_epoch() == second.owner_epoch());
  const auto a = f.gpu.TrackSubmitted(1, first.resources());
  const auto b = f.gpu.TrackSubmitted(2, second.resources());
  REQUIRE(f.state.AttachCompletion(first, a));
  CHECK_FALSE(f.state.AttachCompletion(first, b));
  REQUIRE(f.state.AttachCompletion(second, b));
  f.gpu.CompleteThrough(1);
  CHECK_FALSE(f.Ready());
  CHECK(f.writes.empty());
  f.gpu.CompleteThrough(2);
  CHECK(f.Ready());
  CHECK(f.writes.size() == 1);
  CHECK(
      f.state.BindSwap(checkout.identifier, checkout.descriptor, kAck).status ==
      State::BindStatus::kUnknownIdentifier);
}

TEST_CASE("Identifier reset preserves GPU work and suppresses stale ACK stores",
          "[graphics][system_command]") {
  Fixture f;
  const auto checkout = f.Get();
  const auto use = f.Bind(checkout);
  REQUIRE(
      f.state.AttachCompletion(use, f.gpu.TrackSubmitted(2, use.resources())));
  const auto old_binding = checkout.binding;
  const auto new_binding = f.state.SetIdentifierAddress(kOtherAck);
  CHECK(new_binding.generation != old_binding.generation);
  CHECK_FALSE(f.Ready());
  f.gpu.CompleteThrough(2);
  CHECK(f.Ready());
  CHECK(f.writes.empty());
  const auto fresh = f.Get();
  CHECK(fresh.identifier != checkout.identifier);
  CHECK(f.state.BindSwap(fresh.identifier, fresh.descriptor, kAck).status ==
        State::BindStatus::kStaleBinding);
}

TEST_CASE("Failed or unsupported system work remains blocked until GPU stop",
          "[graphics][system_command]") {
  Fixture f;
  const auto checkout = f.Get();
  const auto use = f.Bind(checkout);
  SECTION("Unsupported backend") {
    REQUIRE(f.state.AttachCompletion(use, GpuCompletion::Unsupported()));
  }
  SECTION("Actual failed completion token") {
    REQUIRE(f.state.AttachCompletion(use,
                                     f.gpu.TrackSubmitted(3, use.resources())));
    f.gpu.FailPending();
    f.gpu.CompleteThrough(100);
  }
  f.memory[kAck] = checkout.identifier;
  CHECK_FALSE(f.Ready());
  f.state.SetIdentifierAddress(kOtherAck);
  REQUIRE(f.state.Publish({}));
  CHECK_FALSE(f.Ready());
  CHECK(f.writes.empty());
  f.state.CloseOwnedSession();
  CHECK_FALSE(f.state.BeginOwnedSession(Session()));
  f.gpu.ReleaseAfterGpuStopped();
  f.state.ReleaseAfterGpuStopped();
  REQUIRE(f.state.BeginOwnedSession(Session()));
  CHECK(f.state.QueryReady());
}

TEST_CASE(
    "GPU ACK compare-exchange preserves an unobserved concurrent CPU skip",
    "[graphics][system_command]") {
  Fixture f;
  const auto gpu_checkout = f.Get();
  const auto use = f.Bind(gpu_checkout);
  REQUIRE(
      f.state.AttachCompletion(use, f.gpu.TrackSubmitted(5, use.resources())));
  const auto cpu_checkout = f.Get();
  f.gpu.CompleteThrough(5);
  bool injected = false;
  const auto compare_exchange = f.Writer();
  const State::AckWriter racing_writer = [&](uint32_t address,
                                             uint32_t observed,
                                             uint32_t completed) {
    if (!injected) {
      injected = true;
      f.memory[address] = cpu_checkout.identifier;  // CPU store after sample.
    }
    return compare_exchange(address, observed, completed);
  };
  CHECK_FALSE(f.state.QueryReady(0, f.Reader(), racing_writer));
  CHECK(f.memory[kAck] == cpu_checkout.identifier);
  CHECK(f.writes.empty());
  CHECK(f.state.outstanding_checkouts() == 2);
  CHECK(
      f.Ready());  // Samples CPU skip, then safely ACKs only completed GPU use.
  CHECK(f.writes.size() == 1);
}

TEST_CASE("A system completion cannot be attached through another state",
          "[graphics][system_command]") {
  Fixture first;
  Fixture second;
  const auto use = first.Bind(first.Get());
  CHECK_FALSE(second.state.AttachCompletion(
      use, second.gpu.TrackSubmitted(1, use.resources())));
  CHECK_FALSE(first.Ready());
  CHECK(second.Ready());  // An unconsumed publication has no outstanding use.
}

TEST_CASE("A replacement backend fence cannot retire an old system session",
          "[graphics][system_command]") {
  Fixture f;
  const auto use = f.Bind(f.Get());
  GpuCompletionQueue replacement;
  const auto wrong = replacement.TrackSubmitted(1, use.resources());
  replacement.CompleteThrough(1);
  REQUIRE(wrong.status() == GpuCompletionStatus::kComplete);
  CHECK_FALSE(f.state.AttachCompletion(use, wrong));
  CHECK_FALSE(f.Ready());
  const auto actual = f.gpu.TrackSubmitted(1, use.resources());
  REQUIRE(f.state.AttachCompletion(use, actual));
  CHECK_FALSE(f.Ready());
  f.gpu.CompleteThrough(1);
  CHECK(f.Ready());
  replacement.ReleaseAfterGpuStopped();
}

}  // namespace
}  // namespace xe::gpu::test
