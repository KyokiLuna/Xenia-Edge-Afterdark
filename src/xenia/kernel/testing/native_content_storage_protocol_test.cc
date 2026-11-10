/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */
#include "xenia/kernel/native_content_storage_protocol.h"

#include <cstring>

#include "third_party/catch/include/catch.hpp"

namespace xe::kernel::test {
namespace {
NativeContentStorageDevice Hdd() {
  return {"managed-hdd", 1, xam::DeviceType::HDD,
          "\\Device\\Harddisk0\\Partition1", true};
}
NativeContentStorageDevice Usb(uint64_t generation = 1) {
  return {"removable", generation, xam::DeviceType::USBMASS, "\\Device\\Mass0",
          true};
}
using Queue = NativeContentStorageQueue;
using Change = NativeContentStorageTransition;
}  // namespace

TEST_CASE("Storage changes coalesce against genuinely delivered records",
          "[native_storage_protocol]") {
  Queue queue;
  REQUIRE(queue.Apply({1, {Hdd(), Usb()}}) == Queue::ApplyResult::kApplied);
  REQUIRE(queue.size() == 1);
  REQUIRE(queue.Peek()->device.identity == "removable");
  REQUIRE(queue.Peek()->transition == Change::kInserted);
  // Another source refresh before native consumption neither duplicates nor
  // falsely counts a device as delivered.
  REQUIRE(queue.Apply({2, {Hdd(), Usb()}}) == Queue::ApplyResult::kApplied);
  REQUIRE(queue.size() == 1);
  queue.CommitNext();
  REQUIRE_FALSE(queue.Peek());
  REQUIRE(queue.Apply({3, {Hdd(), Usb(2)}}) == Queue::ApplyResult::kApplied);
  REQUIRE(queue.size() == 2);
  REQUIRE(queue.Peek()->transition == Change::kRemoved);
  REQUIRE(queue.Peek()->device.generation == 1);
  queue.CommitNext();
  REQUIRE(queue.Peek()->transition == Change::kInserted);
  REQUIRE(queue.Peek()->device.generation == 2);
  // A third generation arrives after removal but before insertion. Only the
  // current device is announced; no removal for a never-delivered generation.
  REQUIRE(queue.Apply({4, {Hdd(), Usb(3)}}) == Queue::ApplyResult::kApplied);
  REQUIRE(queue.size() == 1);
  REQUIRE(queue.Peek()->transition == Change::kInserted);
  REQUIRE(queue.Peek()->device.generation == 3);
  queue.CommitNext();
  REQUIRE_FALSE(queue.Peek());
}

TEST_CASE("Invalid storage snapshots leave the last owned state intact",
          "[native_storage_protocol]") {
  Queue queue;
  REQUIRE(queue.Apply({10, {Hdd(), Usb()}}) == Queue::ApplyResult::kApplied);
  auto invalid = Usb();
  SECTION("zero generation") { invalid.generation = 0; }
  SECTION("host path") { invalid.guest_volume_root = "/host/media"; }
  SECTION("path escape") {
    invalid.guest_volume_root = "\\Device\\Mass0\\..\\Harddisk0";
  }
  SECTION("embedded NUL") { invalid.guest_volume_root.push_back('\0'); }
  SECTION("native field truncation") {
    invalid.guest_volume_root = "\\Device\\" + std::string(56, 'a');
  }
  SECTION("unsupported device kind") {
    invalid.type = xam::DeviceType::Invalid;
  }
  REQUIRE(queue.Apply({11, {Hdd(), invalid}}) == Queue::ApplyResult::kInvalid);
  REQUIRE(queue.revision() == 10);
  REQUIRE(queue.size() == 1);
  REQUIRE(queue.Peek()->device == Usb());
  REQUIRE(queue.Apply({9, {}}) == Queue::ApplyResult::kStale);
  REQUIRE(queue.revision() == 10);
  REQUIRE(queue.size() == 1);
}

TEST_CASE("Storage volume admission counts native expansion and aliases",
          "[native_storage_protocol]") {
  Queue queue;
  auto duplicate = Usb();
  duplicate.identity = "other-source";
  REQUIRE(queue.Apply({1, {Usb(), duplicate}}) == Queue::ApplyResult::kInvalid);
  NativeContentStorageSnapshot snapshot{1, {Hdd()}};
  for (unsigned i = 0; i < 10; ++i) {
    snapshot.devices.push_back({"mu" + std::to_string(i), 1,
                                xam::DeviceType::MU,
                                "\\Device\\Mu" + std::to_string(i), true});
  }
  REQUIRE(queue.Apply(snapshot) == Queue::ApplyResult::kInvalid);
  snapshot.devices.pop_back();
  REQUIRE(queue.Apply(snapshot) == Queue::ApplyResult::kApplied);
  REQUIRE(queue.size() == 9);
  REQUIRE(Queue::NativeVolumeCost(snapshot.devices.back()) == 2);
}

TEST_CASE("Storage notification prefix preserves guest byte order",
          "[native_storage_protocol]") {
  NativeContentStorageNotification record{};
  record.volume_path = 0x12345678;
  record.transition = uint32_t(Change::kInserted);
  record.device_type = uint32_t(xam::DeviceType::USBMASS);
  const uint8_t expected[24] = {0, 0, 0, 0, 0,    0,    0,    0,
                                0, 0, 0, 0, 0x12, 0x34, 0x56, 0x78,
                                0, 0, 0, 1, 0,    0,    0,    16};
  REQUIRE(std::memcmp(&record, expected, sizeof(record)) == 0);
}

TEST_CASE("Storage output preparation cannot consume a stale or foreign queue",
          "[native_storage_protocol]") {
  Queue queue, other;
  REQUIRE(queue.Apply({1, {Usb()}}) == Queue::ApplyResult::kApplied);
  auto delivery = queue.PrepareDelivery();
  REQUIRE(delivery);
  REQUIRE(queue.size() == 1);  // Guest-output validation may still fail.
  REQUIRE_FALSE(other.CommitDelivery(std::move(*delivery)));
  REQUIRE(queue.size() == 1);
  REQUIRE(queue.Apply({2, {Usb(2)}}) == Queue::ApplyResult::kApplied);
  REQUIRE_FALSE(queue.CommitDelivery(std::move(*delivery)));
  REQUIRE(queue.Peek()->device.generation == 2);
  auto current = queue.PrepareDelivery();
  REQUIRE(current);
  REQUIRE(queue.CommitDelivery(std::move(*current)));
  REQUIRE_FALSE(queue.Peek());
  REQUIRE_FALSE(queue.CommitDelivery(std::move(*current)));
}

TEST_CASE("An unavailable source cannot invent a physical generation or mount",
          "[native_storage_protocol]") {
  Queue queue;
  auto unavailable = Hdd();
  unavailable.available = false;
  unavailable.generation = 0;
  REQUIRE(queue.Apply({1, {unavailable}}) == Queue::ApplyResult::kApplied);
  REQUIRE_FALSE(queue.Peek());
  REQUIRE(queue.Apply({2, {Hdd()}}) == Queue::ApplyResult::kApplied);
  REQUIRE(queue.Peek());
  REQUIRE(queue.Peek()->transition == Change::kInserted);
}
namespace {
NativeStorageProcessingCall ProcessingCall(uint32_t thread = 8) {
  // Metadata model only: these numbers cannot create a driver or common proof.
  return {{1, 2, thread, 0x90001000, 0x70010000, 0x8167EC40, 0x70010050},
          0x90002000, 1, 16, "\\Device\\Mass0", 17};
}
auto FinishProcessing(NativeStorageProcessingProtocol& protocol,
                      const NativeStorageProcessingCall& call,
                      uint32_t result = 1, uint32_t id = 73) {
  return protocol.Complete(call.frame, result, call.path_pointer,
      call.transition, call.type, id, call.path);
}
}  // namespace

TEST_CASE("Native storage receipt metadata requires one completed same invocation",
          "[native_storage_protocol][processing][model]") {
  NativeStorageProcessingProtocol protocol;
  auto call = ProcessingCall();
  REQUIRE(protocol.Begin(call));
  auto wrong = call;
  wrong.frame.sp += 16;
  CHECK_FALSE(FinishProcessing(protocol, wrong));
  CHECK(protocol.Pending(call.frame.thread));
  const auto completed = FinishProcessing(protocol, call);
  REQUIRE(completed);
  CHECK(completed->succeeded);
  CHECK(completed->native_id == 73);
  CHECK(completed->call.delivery_cookie == 17);
  CHECK_FALSE(FinishProcessing(protocol, call));
  CHECK_FALSE(protocol.Pending(call.frame.thread));
  SECTION("failure cannot use the stale output ID") {
    REQUIRE(protocol.Begin(call));
    const auto failed = FinishProcessing(protocol, call, 0, 73);
    REQUIRE(failed);
    CHECK_FALSE(failed->succeeded);
    CHECK(failed->native_id == 0);
  }
  SECTION("removal output is never an insertion receipt") {
    call.transition = 2;
    REQUIRE(protocol.Begin(call));
    const auto removed = FinishProcessing(protocol, call, 1, 73);
    REQUIRE(removed);
    CHECK(removed->succeeded);
    CHECK(removed->native_id == 0);
  }
  SECTION("changed input bytes cancel the receipt") {
    REQUIRE(protocol.Begin(call));
    const auto changed = GENERATE(0u, 1u, 2u, 3u);
    CHECK_FALSE(protocol.Complete(call.frame, 1,
        call.path_pointer + (changed == 0 ? 4 : 0),
        changed == 1 ? 2 : call.transition,
        changed == 2 ? 1 : call.type, 73,
        changed == 3 ? "\\Device\\Mass1" : call.path));
    CHECK_FALSE(FinishProcessing(protocol, call));
  }
}
TEST_CASE("Native storage pending metadata refuses abandoned overlap and stop",
          "[native_storage_protocol][processing][model]") {
  auto call = ProcessingCall();
  NativeStorageProcessingProtocol protocol;
  REQUIRE(protocol.Begin(call));
  auto invalid = call;
  invalid.frame.domain = 0;
  CHECK_FALSE(protocol.Begin(invalid));
  CHECK(protocol.Pending(call.frame.thread));
  // A later identical stack/PCR/LR after an abandoned call is not a serial.
  CHECK_FALSE(protocol.Begin(call));
  CHECK_FALSE(FinishProcessing(protocol, call));
  CHECK_FALSE(protocol.Begin(call));

  NativeStorageProcessingProtocol overlap;
  REQUIRE(overlap.Begin(call));
  auto second = ProcessingCall(9);
  second.delivery_cookie = 18;
  REQUIRE(overlap.Begin(second));
  const auto first_return = FinishProcessing(overlap, call);
  const auto second_return = FinishProcessing(overlap, second);
  REQUIRE(first_return);
  REQUIRE(second_return);
  CHECK(first_return->call.delivery_cookie == 0);
  CHECK(second_return->call.delivery_cookie == 0);
  // Zero-cookie returns are data, never joined processing/binding authority.
  CHECK(NativeStorageProcessingProtocol::PathsOverlap(
      "\\Device\\Mass0", "\\device\\mass0\\Partition1"));
  CHECK_FALSE(NativeStorageProcessingProtocol::PathsOverlap(
      "\\Device\\Mass0", "\\Device\\Mass1"));

  NativeStorageProcessingProtocol stopped;
  REQUIRE(stopped.Begin(call));
  stopped.Stop();
  CHECK_FALSE(FinishProcessing(stopped, call));
  CHECK_FALSE(stopped.Begin(call));
}
TEST_CASE("Native storage processed profiles bound bootstrap IDs and expansion",
          "[native_storage_protocol][processing][model]") {
  auto call = ProcessingCall();
  CHECK(NativeStorageProcessingProtocol::SingleVolumeProfile(call.frame, 16));
  CHECK(NativeStorageProcessingProtocol::SingleVolumeProfile(call.frame, 1));
  CHECK_FALSE(NativeStorageProcessingProtocol::SingleVolumeProfile(call.frame, 2));
  CHECK_FALSE(NativeStorageProcessingProtocol::SingleVolumeProfile(call.frame, 15));
  auto paired = call;
  paired.type = 2;
  NativeStorageProcessingProtocol unsupported;
  REQUIRE(unsupported.Begin(paired));
  CHECK_FALSE(FinishProcessing(unsupported, paired));
  auto bootstrap = call;
  bootstrap.frame.lr = 0x8167EE2C;
  bootstrap.frame.packet = bootstrap.frame.sp + 0x60;
  bootstrap.type = 1;
  CHECK(NativeStorageProcessingProtocol::SingleVolumeProfile(bootstrap.frame, 1));
  CHECK_FALSE(NativeStorageProcessingProtocol::SingleVolumeProfile(bootstrap.frame, 16));
  for (uint32_t id : {0u, 2u, 0x1F4u}) {
    NativeStorageProcessingProtocol protocol;
    REQUIRE(protocol.Begin(call));
    CHECK_FALSE(FinishProcessing(protocol, call, 1, id));
  }
  for (uint32_t id : {1u, 0x1F3u}) {
    NativeStorageProcessingProtocol protocol;
    REQUIRE(protocol.Begin(bootstrap));
    const auto result = FinishProcessing(protocol, bootstrap, 1, id);
    REQUIRE(result);
    CHECK(result->native_id == id);
    CHECK_FALSE(FinishProcessing(protocol, bootstrap, 1, id));
  }
  // A successful previous return cannot be replayed when that numeric ID is
  // later reused. The driver additionally joins a private current common proof.
  NativeStorageProcessingProtocol capacity;
  for (uint32_t thread = 1; thread <= 64; ++thread) {
    auto next = ProcessingCall(thread);
    REQUIRE(capacity.Begin(next));
  }
  CHECK_FALSE(capacity.Begin(ProcessingCall(65)));
  CHECK_FALSE(capacity.Pending(1));
}

}  // namespace xe::kernel::test
