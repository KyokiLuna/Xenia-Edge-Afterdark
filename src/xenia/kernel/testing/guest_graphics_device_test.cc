/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia contributors. All rights reserved.                    *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <atomic>
#include <memory>
#include <optional>
#include <stdexcept>
#include <thread>

#include "third_party/catch/include/catch.hpp"
#include "xenia/kernel/guest_graphics_device.h"

namespace xe::kernel::test {
namespace {
using Device = GuestGraphicsDevice;
using Status = Device::RetireStatus;
constexpr uint32_t kCell = 0x90001000;
constexpr uint32_t kForeign = 0x88004000;

Device::Identity Identity() { return {7, 0x30001000, 3, 0x45004000, 2}; }

class Fixture {
 public:
  explicit Fixture(bool cpu_only = false) {
    epoch = cpu_only ? 0 : queue.backend_epoch();
    owner = Device::Create(Identity(), {module, graph}, epoch);
    REQUIRE(owner);
    weak_owner = owner;
  }
  ~Fixture() {
    queue.ReleaseAfterGpuStopped();
    stopped = true;
    readable = true;
    if (auto live = weak_owner.lock()) {
      const auto publication = live->snapshot().publication;
      if (publication.cell_address) {
        live->RetireAfterGpuStopped(publication, Reader(), Cas(), Proof());
      }
    }
  }
  Device::CellReader Reader() {
    return [this](uint32_t address) -> std::optional<uint32_t> {
      if (address != kCell || !readable.load()) {
        return {};
      }
      return cell.load();
    };
  }
  Device::CellCas Cas() {
    return [this](uint32_t address, uint32_t observed, uint32_t desired) {
      ++cas_calls;
      return address == kCell && readable.load() &&
             cell.compare_exchange_strong(observed, desired);
    };
  }
  Device::GpuStopProof Proof() {
    return [this](uint64_t kernel, uint64_t backend) {
      return stopped && kernel == Identity().kernel_epoch && backend == epoch;
    };
  }
  Device::Publication Publish() {
    auto publication = owner->Publish(kCell, Reader(), Cas());
    REQUIRE(publication);
    CHECK(cell == Identity().guest_device);
    return *publication;
  }
  Status Poll(const Device::Publication& publication) {
    return owner->PollRetirement(publication, Reader(), Cas());
  }

  gpu::GpuCompletionQueue queue;
  std::shared_ptr<int> module = std::make_shared<int>(1);
  std::shared_ptr<int> graph = std::make_shared<int>(2);
  std::weak_ptr<int> graph_lifetime = graph;
  std::shared_ptr<Device> owner;
  std::weak_ptr<Device> weak_owner;
  std::atomic<uint32_t> cell{0};
  std::atomic<bool> readable{true};
  std::atomic<unsigned> cas_calls{0};
  uint64_t epoch = 0;
  bool stopped = false;
};

TEST_CASE("Guest device publication requires real owner identities and leases",
          "[graphics][guest_device]") {
  const Device::Leases leases{std::make_shared<int>(1),
                              std::make_shared<int>(2)};
  CHECK_FALSE(Device::Create({}, leases, 0));
  auto identity = Identity();
  identity.guest_device += 1;
  CHECK_FALSE(Device::Create(identity, leases, 0));
  identity = Identity();
  identity.module_hmodule = 0;
  CHECK_FALSE(Device::Create(identity, leases, 0));
  identity = Identity();
  identity.process_type = 3;
  CHECK_FALSE(Device::Create(identity, leases, 0));
  CHECK_FALSE(Device::Create(Identity(), {leases.module, {}}, 0));
  CHECK_FALSE(Device::Create(Identity(), {{}, leases.device_graph}, 0));
  Fixture fixture(true);
  CHECK_FALSE(
      fixture.owner->Publish(kCell + 1, fixture.Reader(), fixture.Cas()));
  CHECK_FALSE(fixture.owner->Publish(kCell, {}, fixture.Cas()));
  CHECK(fixture.cas_calls == 0);
  CHECK(fixture.cell == 0);
  CHECK_FALSE(fixture.owner->snapshot().publication_attempted);
  const auto publication = fixture.Publish();
  CHECK_FALSE(fixture.owner->Publish(kCell, fixture.Reader(), fixture.Cas()));
  REQUIRE(fixture.owner->BeginRetire(publication));
  CHECK(fixture.Poll(publication) == Status::kRetired);
}

TEST_CASE("Guest device retirement drains the last copy of an admitted CPU use",
          "[graphics][guest_device]") {
  Fixture fixture(true);
  const auto publication = fixture.Publish();
  auto use = fixture.owner->AcquireUse(publication);
  REQUIRE(use);
  CHECK(use.identity() == Identity());
  CHECK(use.resources().size() == 2);
  auto copy = use;
  CHECK(fixture.owner->snapshot().cpu_uses == 1);
  fixture.module.reset();
  fixture.graph.reset();
  REQUIRE(fixture.owner->BeginRetire(publication));
  CHECK_FALSE(fixture.owner->AcquireUse(publication));
  CHECK(fixture.Poll(publication) == Status::kPendingCpu);
  CHECK(fixture.cell == Identity().guest_device);
  use = {};
  CHECK(fixture.Poll(publication) == Status::kPendingCpu);
  CHECK_FALSE(fixture.graph_lifetime.expired());
  copy = {};
  CHECK(fixture.Poll(publication) == Status::kRetired);
  CHECK(fixture.cell == 0);
  CHECK(fixture.graph_lifetime.expired());
}

TEST_CASE("Guest device tokens never claim or clear a foreign publication",
          "[graphics][guest_device]") {
  Fixture fixture(true);
  fixture.cell = kForeign;
  CHECK_FALSE(fixture.owner->Publish(kCell, fixture.Reader(), fixture.Cas()));
  const auto rejected = fixture.owner->snapshot().publication;
  CHECK(fixture.cas_calls == 0);
  CHECK_FALSE(fixture.owner->AcquireUse(rejected));
  fixture.cell = 0;
  CHECK_FALSE(fixture.owner->Publish(kCell, fixture.Reader(), fixture.Cas()));
  REQUIRE(fixture.owner->BeginRetire(rejected));
  CHECK(fixture.Poll(rejected) == Status::kRetired);

  Fixture replacement(true);
  const auto published = replacement.Publish();
  CHECK(rejected.serial != published.serial);
  CHECK_FALSE(replacement.owner->AcquireUse(rejected));
  CHECK_FALSE(replacement.owner->BeginRetire(rejected));
  auto forged = published;
  ++forged.identity.module_generation;
  CHECK_FALSE(replacement.owner->BeginRetire(forged));
  replacement.cell = kForeign;
  REQUIRE(replacement.owner->BeginRetire(published));
  const auto before = replacement.cas_calls.load();
  CHECK(replacement.Poll(published) == Status::kRetiredForeign);
  CHECK(replacement.cell == kForeign);
  CHECK(replacement.cas_calls == before);
}

TEST_CASE("A foreign replacement racing exact guest-device clear is preserved",
          "[graphics][guest_device]") {
  Fixture fixture(true);
  const auto publication = fixture.Publish();
  REQUIRE(fixture.owner->BeginRetire(publication));
  auto replace = [&](uint32_t address, uint32_t observed, uint32_t desired) {
    CHECK(address == kCell);
    CHECK(observed == Identity().guest_device);
    CHECK(desired == 0);
    fixture.cell = kForeign;
    return false;
  };
  CHECK(fixture.owner->PollRetirement(publication, fixture.Reader(), replace) ==
        Status::kRetiredForeign);
  CHECK(fixture.cell == kForeign);
}

TEST_CASE("Uncertain publication retains the graph until proven stop and clear",
          "[graphics][guest_device]") {
  Fixture fixture(true);
  SECTION("False CAS can follow an actual store") {
    auto uncertain = [&](uint32_t, uint32_t, uint32_t value) {
      fixture.cell = value;
      return false;
    };
    CHECK_FALSE(fixture.owner->Publish(kCell, fixture.Reader(), uncertain));
  }
  SECTION("An exception can follow an actual store") {
    auto interrupted = [&](uint32_t, uint32_t, uint32_t value) -> bool {
      fixture.cell = value;
      throw std::runtime_error("publication interrupted after write");
    };
    CHECK_THROWS_AS(
        fixture.owner->Publish(kCell, fixture.Reader(), interrupted),
        std::runtime_error);
  }
  const auto publication = fixture.owner->snapshot().publication;
  fixture.module.reset();
  fixture.graph.reset();
  CHECK(fixture.cell == Identity().guest_device);
  CHECK(fixture.owner->snapshot().phase == Device::Phase::kQuarantined);
  CHECK_FALSE(fixture.owner->AcquireUse(publication));
  REQUIRE(fixture.owner->BeginRetire(publication));
  CHECK(fixture.Poll(publication) == Status::kQuarantined);
  CHECK(fixture.owner->RetireAfterGpuStopped(publication, fixture.Reader(),
                                             fixture.Cas(), fixture.Proof()) ==
        Status::kProofRejected);
  CHECK_FALSE(fixture.graph_lifetime.expired());
  fixture.queue.ReleaseAfterGpuStopped();
  fixture.stopped = true;
  CHECK(fixture.owner->RetireAfterGpuStopped(publication, fixture.Reader(),
                                             fixture.Cas(), fixture.Proof()) ==
        Status::kRetired);
  CHECK(fixture.cell == 0);
  CHECK(fixture.graph_lifetime.expired());
}

TEST_CASE("Every actual GPU use needs its own matching strict completion",
          "[graphics][guest_device]") {
  Fixture fixture;
  const auto publication = fixture.Publish();
  auto use = fixture.owner->AcquireUse(publication);
  REQUIRE(use);
  REQUIRE(fixture.owner->TrackGpuUse(use, fixture.queue.TrackSubmitted(7)));
  REQUIRE(fixture.owner->BeginRetire(publication));
  // Already admitted submission may finish attaching after retirement starts.
  REQUIRE(fixture.owner->TrackGpuUse(use, fixture.queue.TrackSubmitted(11)));
  fixture.module.reset();
  fixture.graph.reset();
  CHECK(fixture.Poll(publication) == Status::kPendingCpu);
  use = {};
  CHECK(fixture.Poll(publication) == Status::kPendingGpu);
  fixture.queue.CompleteThrough(7);
  CHECK(fixture.Poll(publication) == Status::kPendingGpu);
  CHECK_FALSE(fixture.graph_lifetime.expired());
  fixture.queue.CompleteThrough(11);
  CHECK(fixture.Poll(publication) == Status::kRetired);
  CHECK(fixture.graph_lifetime.expired());
  CHECK(fixture.cell == 0);
}

TEST_CASE("Unsupported failed and foreign GPU evidence never retires a device",
          "[graphics][guest_device]") {
  Fixture fixture;
  gpu::GpuCompletionQueue foreign_queue;
  const auto publication = fixture.Publish();
  auto use = fixture.owner->AcquireUse(publication);
  REQUIRE(use);
  SECTION("Unsupported completion") {
    CHECK_FALSE(fixture.owner->TrackGpuUse(use, {}));
  }
  SECTION("Foreign backend completion even when already complete") {
    auto completion = foreign_queue.TrackSubmitted(5);
    foreign_queue.CompleteThrough(5);
    CHECK_FALSE(fixture.owner->TrackGpuUse(use, completion));
  }
  SECTION("Real failure observed after initial admission") {
    REQUIRE(fixture.owner->TrackGpuUse(use, fixture.queue.TrackSubmitted(5)));
    fixture.queue.FailPending();
    CHECK_FALSE(fixture.owner->AcquireUse(publication));
  }
  use = {};
  fixture.module.reset();
  fixture.graph.reset();
  REQUIRE(fixture.owner->BeginRetire(publication));
  CHECK(fixture.Poll(publication) == Status::kQuarantined);
  fixture.queue.CompleteThrough(1000);
  CHECK(fixture.Poll(publication) == Status::kQuarantined);
  CHECK_FALSE(fixture.graph_lifetime.expired());
  foreign_queue.ReleaseAfterGpuStopped();
  fixture.queue.ReleaseAfterGpuStopped();
  fixture.stopped = true;
  CHECK(fixture.owner->RetireAfterGpuStopped(publication, fixture.Reader(),
                                             fixture.Cas(), fixture.Proof()) ==
        Status::kRetired);
  CHECK(fixture.cell == 0);
  CHECK(fixture.graph_lifetime.expired());
}

TEST_CASE("A CPU-only device cannot invent a backend for attached GPU work",
          "[graphics][guest_device]") {
  Fixture fixture(true);
  const auto publication = fixture.Publish();
  auto use = fixture.owner->AcquireUse(publication);
  REQUIRE(use);
  CHECK_FALSE(fixture.owner->TrackGpuUse(use, fixture.queue.TrackSubmitted(1)));
  use = {};
  REQUIRE(fixture.owner->BeginRetire(publication));
  fixture.queue.CompleteThrough(1);
  CHECK(fixture.Poll(publication) == Status::kQuarantined);
}

TEST_CASE(
    "GPU stop alone cannot leave a dangling device cell or active CPU use",
    "[graphics][guest_device]") {
  Fixture fixture;
  const auto publication = fixture.Publish();
  auto use = fixture.owner->AcquireUse(publication);
  fixture.queue.ReleaseAfterGpuStopped();
  fixture.stopped = true;
  CHECK(fixture.owner->RetireAfterGpuStopped(publication, fixture.Reader(),
                                             fixture.Cas(), fixture.Proof()) ==
        Status::kPendingCpu);
  CHECK(fixture.cell == Identity().guest_device);
  use = {};
  fixture.readable = false;
  CHECK(fixture.owner->RetireAfterGpuStopped(publication, fixture.Reader(),
                                             fixture.Cas(), fixture.Proof()) ==
        Status::kCellUnavailable);
  CHECK(fixture.cell == Identity().guest_device);
  fixture.readable = true;
  auto no_clear = [](uint32_t, uint32_t, uint32_t) { return false; };
  CHECK(fixture.owner->RetireAfterGpuStopped(publication, fixture.Reader(),
                                             no_clear, fixture.Proof()) ==
        Status::kQuarantined);
  CHECK(fixture.cell == Identity().guest_device);
  CHECK(fixture.owner->RetireAfterGpuStopped(publication, fixture.Reader(),
                                             fixture.Cas(), fixture.Proof()) ==
        Status::kRetired);
  CHECK(fixture.cell == 0);
}

TEST_CASE("Published device lifetime survives a dropped registry handle",
          "[graphics][guest_device]") {
  Fixture fixture(true);
  const auto publication = fixture.Publish();
  fixture.module.reset();
  fixture.graph.reset();
  fixture.owner.reset();
  REQUIRE_FALSE(fixture.weak_owner.expired());
  auto retained = fixture.weak_owner.lock();
  REQUIRE(retained->BeginRetire(publication));
  CHECK(retained->PollRetirement(publication, fixture.Reader(),
                                 fixture.Cas()) == Status::kRetired);
  retained.reset();
  CHECK(fixture.weak_owner.expired());
  CHECK(fixture.graph_lifetime.expired());
}

TEST_CASE(
    "Device graph release occurs outside the state mutex after cell clear",
    "[graphics][guest_device]") {
  std::shared_ptr<Device> owner;
  std::atomic<uint32_t> cell{0};
  bool released = false;
  auto graph = std::shared_ptr<int>(new int(1), [&](int* value) {
    // snapshot() would deadlock if a resource deleter ran under the mutex.
    CHECK(owner->snapshot().phase == Device::Phase::kRetired);
    CHECK(cell == 0);
    released = true;
    delete value;
  });
  owner = Device::Create(Identity(), {std::make_shared<int>(2), graph}, 0);
  auto reader = [&](uint32_t) -> std::optional<uint32_t> {
    return cell.load();
  };
  auto cas = [&](uint32_t, uint32_t expected, uint32_t desired) {
    return cell.compare_exchange_strong(expected, desired);
  };
  auto publication = owner->Publish(kCell, reader, cas);
  REQUIRE(publication);
  graph.reset();
  REQUIRE(owner->BeginRetire(*publication));
  CHECK(owner->PollRetirement(*publication, reader, cas) == Status::kRetired);
  CHECK(released);
}

TEST_CASE("Racing retirement never admits a new device use after closure",
          "[graphics][guest_device]") {
  for (unsigned attempt = 0; attempt < 16; ++attempt) {
    Fixture fixture(true);
    const auto publication = fixture.Publish();
    std::atomic<bool> ready{false};
    std::atomic<bool> go{false};
    Device::UseLease raced_use;
    std::thread consumer([&] {
      ready.store(true, std::memory_order_release);
      while (!go.load(std::memory_order_acquire)) {
        std::this_thread::yield();
      }
      raced_use = fixture.owner->AcquireUse(publication);
    });
    while (!ready.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }
    go.store(true, std::memory_order_release);
    const bool closed = fixture.owner->BeginRetire(publication);
    consumer.join();
    REQUIRE(closed);
    CHECK_FALSE(fixture.owner->AcquireUse(publication));
    const auto status = fixture.Poll(publication);
    CHECK(status == (raced_use ? Status::kPendingCpu : Status::kRetired));
    raced_use = {};
    CHECK(fixture.Poll(publication) == Status::kRetired);
    CHECK(fixture.cell == 0);
  }
}

}  // namespace
}  // namespace xe::kernel::test
