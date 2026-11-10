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
#include <thread>
#include <vector>

#include "third_party/catch/include/catch.hpp"
#include "xenia/gpu/gpu_completion.h"

namespace xe::gpu::test {
namespace {

TEST_CASE("Strict GPU completion retains guest leases until its exact fence",
          "[graphics][completion]") {
  GpuCompletionQueue queue;
  auto lease = std::make_shared<int>(1);
  std::weak_ptr<int> lifetime = lease;
  unsigned int notifications = 0;
  const auto token =
      queue.TrackSubmitted(7, {lease}, [&](const GpuCompletion& completed) {
        ++notifications;
        CHECK(completed.status() == GpuCompletionStatus::kComplete);
        CHECK(completed.submission() == 7);
        CHECK_FALSE(lifetime.expired());  // The observer also has a lease.
      });
  lease.reset();
  CHECK(token.status() == GpuCompletionStatus::kPending);
  CHECK_FALSE(lifetime.expired());
  CHECK(notifications == 0);
  queue.CompleteThrough(6);
  CHECK(token.status() == GpuCompletionStatus::kPending);
  CHECK_FALSE(lifetime.expired());
  queue.CompleteThrough(7);
  CHECK(token.status() == GpuCompletionStatus::kComplete);
  CHECK(lifetime.expired());
  CHECK(notifications == 1);
  queue.CompleteThrough(20);
  queue.ReleaseAfterGpuStopped();
  CHECK(token.status() == GpuCompletionStatus::kComplete);
  CHECK(notifications == 1);
}

TEST_CASE("A dropped GPU token cannot release an in-flight guest allocation",
          "[graphics][completion]") {
  GpuCompletionQueue queue;
  auto lease = std::make_shared<int>(1);
  std::weak_ptr<int> lifetime = lease;
  {
    const auto dropped = queue.TrackSubmitted(3, {lease});
    CHECK(dropped.status() == GpuCompletionStatus::kPending);
  }
  lease.reset();
  CHECK_FALSE(lifetime.expired());
  queue.CompleteThrough(2);
  CHECK_FALSE(lifetime.expired());
  queue.CompleteThrough(3);
  CHECK(lifetime.expired());
}

TEST_CASE("An existing submission needs prior fence proof to be Complete",
          "[graphics][completion]") {
  GpuCompletionQueue queue;
  const auto first = queue.TrackSubmitted(4);
  const auto repeated = queue.TrackSubmitted(4);
  CHECK(first.status() == GpuCompletionStatus::kPending);
  CHECK(repeated.status() == GpuCompletionStatus::kPending);
  queue.CompleteThrough(4);
  CHECK(first.status() == GpuCompletionStatus::kComplete);
  CHECK(repeated.status() == GpuCompletionStatus::kComplete);
  unsigned int notifications = 0;
  const auto proven = queue.TrackSubmitted(4, {}, [&](const auto& token) {
    ++notifications;
    CHECK(token.status() == GpuCompletionStatus::kComplete);
  });
  CHECK(proven.status() == GpuCompletionStatus::kComplete);
  CHECK(notifications == 1);
}

TEST_CASE(
    "GPU failure never completes work or releases quarantined guest leases",
    "[graphics][completion]") {
  GpuCompletionQueue queue;
  auto lease = std::make_shared<int>(1);
  std::weak_ptr<int> lifetime = lease;
  unsigned int notifications = 0;
  const auto token = queue.TrackSubmitted(9, {lease}, [&](const auto& failed) {
    ++notifications;
    CHECK(failed.status() == GpuCompletionStatus::kFailed);
    CHECK_FALSE(lifetime.expired());
  });
  lease.reset();
  queue.FailPending();
  CHECK(token.status() == GpuCompletionStatus::kFailed);
  CHECK(notifications == 1);
  CHECK_FALSE(lifetime.expired());
  queue.CompleteThrough(1000);  // A failed marker must never turn Complete.
  queue.FailPending();
  CHECK(token.status() == GpuCompletionStatus::kFailed);
  CHECK(notifications == 1);
  CHECK_FALSE(lifetime.expired());
  queue.ReleaseAfterGpuStopped();
  CHECK(lifetime.expired());
  CHECK(token.status() == GpuCompletionStatus::kFailed);
}

TEST_CASE("Failed guest leases transfer to a device lifetime quarantine",
          "[graphics][completion]") {
  auto lease = std::make_shared<int>(1);
  std::weak_ptr<int> lifetime = lease;
  GpuCompletion token;
  GpuCompletionResources device_quarantine;
  {
    GpuCompletionQueue queue;
    token = queue.TrackSubmitted(5, {lease});
    lease.reset();
    device_quarantine = queue.TakeFailedResources();
    CHECK(token.status() == GpuCompletionStatus::kFailed);
    CHECK_FALSE(lifetime.expired());
  }  // CP/queue destruction is not the device lifetime boundary.
  CHECK_FALSE(lifetime.expired());
  CHECK(token.status() == GpuCompletionStatus::kFailed);
  device_quarantine
      .clear();  // The real backend does this after device destroy.
  CHECK(lifetime.expired());
}

TEST_CASE("Absent submission proof and unsupported backends cannot ACK work",
          "[graphics][completion]") {
  GpuCompletionQueue queue;
  const auto absent = queue.TrackSubmitted(0);
  CHECK(absent.status() == GpuCompletionStatus::kFailed);
  queue.CompleteThrough(99);
  CHECK(absent.status() == GpuCompletionStatus::kFailed);
  unsigned int notifications = 0;
  const auto unsupported = GpuCompletion::Unsupported([&](const auto& token) {
    ++notifications;
    CHECK(token.status() == GpuCompletionStatus::kUnsupported);
    CHECK(token.submission() == 0);
    CHECK(token.backend_epoch() == 0);
  });
  CHECK(unsupported.status() == GpuCompletionStatus::kUnsupported);
  CHECK(notifications == 1);
}

TEST_CASE("GPU completion epochs isolate replacement backends",
          "[graphics][completion]") {
  GpuCompletionQueue old_backend;
  GpuCompletionQueue replacement;
  const auto old = old_backend.TrackSubmitted(1);
  const auto fresh = replacement.TrackSubmitted(1);
  CHECK(old.backend_epoch() != fresh.backend_epoch());
  replacement.CompleteThrough(1);
  CHECK(fresh.status() == GpuCompletionStatus::kComplete);
  CHECK(old.status() == GpuCompletionStatus::kPending);
  old_backend.ReleaseAfterGpuStopped();
  CHECK(old.status() == GpuCompletionStatus::kFailed);
  CHECK(fresh.status() == GpuCompletionStatus::kComplete);
}

TEST_CASE("GPU completion status is readable without dereferencing its backend",
          "[graphics][completion]") {
  GpuCompletionQueue queue;
  const auto token = queue.TrackSubmitted(8);
  std::atomic<bool> stop{false};
  std::atomic<bool> observed_complete{false};
  std::thread observer([&] {
    while (!stop.load(std::memory_order_acquire)) {
      if (token.status() == GpuCompletionStatus::kComplete) {
        observed_complete.store(true, std::memory_order_release);
        return;
      }
      std::this_thread::yield();
    }
    observed_complete.store(token.status() == GpuCompletionStatus::kComplete,
                            std::memory_order_release);
  });
  queue.CompleteThrough(8);
  stop.store(true, std::memory_order_release);
  observer.join();
  CHECK(observed_complete.load(std::memory_order_acquire));
}

TEST_CASE("GPU completion observers run once and can inspect another marker",
          "[graphics][completion]") {
  GpuCompletionQueue queue;
  GpuCompletion second;
  unsigned int notifications = 0;
  const auto first = queue.TrackSubmitted(1, {}, [&](const auto& completed) {
    ++notifications;
    CHECK(completed.status() == GpuCompletionStatus::kComplete);
    second = queue.TrackSubmitted(2);
  });
  queue.CompleteThrough(1);
  CHECK(first.status() == GpuCompletionStatus::kComplete);
  CHECK(second.status() == GpuCompletionStatus::kPending);
  CHECK(notifications == 1);
  queue.CompleteThrough(2);
  CHECK(second.status() == GpuCompletionStatus::kComplete);
  CHECK(notifications == 1);
}

TEST_CASE("Native completion provenance cannot come from a title fence",
          "[graphics][completion][native_frame_completion]") {
  GpuCompletionQueue queue;
  NativeFrameSubmissionIdentity identity{{11, 22, 33, 0x30020000}, 44};
  const auto expected = identity;
  const auto title = queue.TrackSubmitted(7);
  const auto native = queue.TrackNativeFrameSubmitted(8, identity);
  identity.source_serial = 99;
  CHECK_FALSE(title.native_frame_identity());
  REQUIRE(native.native_frame_identity());
  CHECK(*native.native_frame_identity() == expected);
  CHECK(native.native_frame_identity()->valid());
  CHECK(native.backend_epoch() == title.backend_epoch());
  queue.CompleteThrough(7);
  CHECK(title.status() == GpuCompletionStatus::kComplete);
  CHECK(native.status() == GpuCompletionStatus::kPending);
  queue.CompleteThrough(8);
  CHECK(native.status() == GpuCompletionStatus::kComplete);
  CHECK(*native.native_frame_identity() == expected);
  CHECK_FALSE(title.native_frame_identity());
  CHECK_FALSE(GpuCompletion::Unsupported().native_frame_identity());
}

TEST_CASE("Invalid native metadata cannot release submitted resources early",
          "[graphics][completion][native_frame_completion]") {
  GpuCompletionQueue queue;
  auto resource = std::make_shared<int>(1);
  const std::weak_ptr<int> lifetime = resource;
  const auto native = queue.TrackNativeFrameSubmitted(3, {}, {resource});
  resource.reset();
  REQUIRE(native.native_frame_identity());
  CHECK_FALSE(native.native_frame_identity()->valid());
  CHECK(native.status() == GpuCompletionStatus::kPending);
  CHECK_FALSE(lifetime.expired());
  queue.CompleteThrough(2);
  CHECK_FALSE(lifetime.expired());
  queue.CompleteThrough(3);
  CHECK(native.status() == GpuCompletionStatus::kComplete);
  CHECK(lifetime.expired());
  CHECK_FALSE(native.native_frame_identity()->valid());
}

TEST_CASE("Failed native completion preserves source and quarantine",
          "[graphics][completion][native_frame_completion]") {
  GpuCompletionQueue queue;
  const NativeFrameSubmissionIdentity identity{{101, 202, 303, 0x30020000}, 4};
  auto resource = std::make_shared<int>(1);
  const std::weak_ptr<int> lifetime = resource;
  unsigned notifications = 0;
  const auto native = queue.TrackNativeFrameSubmitted(
      9, identity, {resource}, [&](const auto& completed) {
        ++notifications;
        CHECK(completed.status() == GpuCompletionStatus::kFailed);
        REQUIRE(completed.native_frame_identity());
        CHECK(*completed.native_frame_identity() == identity);
        CHECK_FALSE(lifetime.expired());
      });
  resource.reset();
  queue.FailPending();
  CHECK(notifications == 1);
  CHECK(native.status() == GpuCompletionStatus::kFailed);
  CHECK_FALSE(lifetime.expired());
  queue.CompleteThrough(9);
  CHECK_FALSE(lifetime.expired());
  queue.ReleaseAfterGpuStopped();
  CHECK(lifetime.expired());
  CHECK(*native.native_frame_identity() == identity);
  CHECK(notifications == 1);
}

TEST_CASE("Backend stop proof defaults to no retirement authority",
          "[graphics][completion][backend_stop]") {
  const GpuBackendStopProof absent;
  CHECK(absent.backend_epoch() == 0);
  CHECK_FALSE(absent.stopped());
  CHECK(absent.evidence() == GpuBackendStopProof::Evidence::kNotStopped);
}

TEST_CASE("Queue destruction alone cannot publish backend stop proof",
          "[graphics][completion][backend_stop]") {
  GpuBackendStopProof retained;
  uint64_t epoch = 0;
  {
    GpuCompletionQueue queue;
    retained = queue.backend_stop_proof();
    epoch = queue.backend_epoch();
    REQUIRE(epoch != 0);
    CHECK_FALSE(retained.stopped());
  }
  CHECK(retained.backend_epoch() == epoch);
  CHECK_FALSE(retained.stopped());
}

TEST_CASE("Explicit backend stop fails pending work and publishes retirement",
          "[graphics][completion][backend_stop]") {
  GpuCompletionQueue queue;
  const auto proof = queue.backend_stop_proof();
  auto resource = std::make_shared<int>(1);
  std::weak_ptr<int> lifetime = resource;
  unsigned notifications = 0;
  const auto token =
      queue.TrackSubmitted(8, {resource}, [&](const auto& failed) {
        ++notifications;
        CHECK(failed.status() == GpuCompletionStatus::kFailed);
        CHECK(proof.stopped());
        CHECK_FALSE(lifetime.expired());
        CHECK(queue.TrackSubmitted(9).status() == GpuCompletionStatus::kFailed);
      });
  resource.reset();
  queue.ReleaseAfterGpuStopped();
  CHECK(proof.backend_epoch() == token.backend_epoch());
  CHECK(proof.evidence() == GpuBackendStopProof::Evidence::kExplicitGpuStopped);
  CHECK(token.status() == GpuCompletionStatus::kFailed);
  CHECK(lifetime.expired());
  queue.CompleteThrough(100);
  queue.ReleaseAfterGpuStopped();
  CHECK(token.status() == GpuCompletionStatus::kFailed);
  CHECK(notifications == 1);
}

TEST_CASE("Device failure needs an actual stop boundary to retire resources",
          "[graphics][completion][backend_stop]") {
  GpuCompletionQueue queue;
  const auto proof = queue.backend_stop_proof();
  auto resource = std::make_shared<int>(1);
  std::weak_ptr<int> lifetime = resource;
  const auto token = queue.TrackSubmitted(1, {resource});
  resource.reset();
  queue.FailPending();
  queue.CompleteThrough(UINT64_MAX);
  CHECK_FALSE(proof.stopped());
  CHECK_FALSE(lifetime.expired());
  CHECK(token.status() == GpuCompletionStatus::kFailed);
  // Host model of the trusted backend reporting an actual lost-device wait.
  // This does not test Vulkan or manufacture successful guest completion.
  queue.ReleaseAfterDeviceLostWaitIdle();
  CHECK(proof.evidence() == GpuBackendStopProof::Evidence::kDeviceLostWaitIdle);
  CHECK(lifetime.expired());
  CHECK(token.status() == GpuCompletionStatus::kFailed);
}

TEST_CASE(
    "After-device-destroy proof outlives CP and precedes guest lease drain",
    "[graphics][completion][backend_stop]") {
  GpuBackendStopProof proof;
  GpuCompletion token;
  GpuCompletionResources device_quarantine;
  std::function<void()> after_device_destroyed;
  bool device_destroyed = false;
  bool resource_released = false;
  auto resource = std::shared_ptr<int>(new int(1), [&](int* value) {
    CHECK(device_destroyed);
    CHECK(proof.stopped());
    CHECK(proof.evidence() == GpuBackendStopProof::Evidence::kDeviceDestroyed);
    resource_released = true;
    delete value;
  });
  unsigned notifications = 0;
  {
    GpuCompletionQueue queue;
    proof = queue.backend_stop_proof();
    after_device_destroyed = queue.MakeAfterDeviceDestroyedCallback();
    token = queue.TrackSubmitted(5, {resource}, [&](const auto& failed) {
      ++notifications;
      CHECK(failed.status() == GpuCompletionStatus::kFailed);
      CHECK_FALSE(proof.stopped());
    });
    resource.reset();
    device_quarantine = queue.TakeFailedResources();
  }
  CHECK_FALSE(proof.stopped());
  CHECK_FALSE(resource_released);
  CHECK(token.status() == GpuCompletionStatus::kFailed);
  // Host model of VulkanDevice's callback order, not actual Vulkan execution.
  device_destroyed = true;
  after_device_destroyed();
  after_device_destroyed();
  CHECK(proof.stopped());
  CHECK_FALSE(resource_released);
  device_quarantine.clear();
  CHECK(resource_released);
  CHECK(token.status() == GpuCompletionStatus::kFailed);
  CHECK(notifications == 1);
}

TEST_CASE("Dropping an uncalled device-destroy callback proves nothing",
          "[graphics][completion][backend_stop]") {
  GpuBackendStopProof proof;
  {
    GpuCompletionQueue queue;
    proof = queue.backend_stop_proof();
    {
      const auto uncalled = queue.MakeAfterDeviceDestroyedCallback();
    }
    CHECK_FALSE(proof.stopped());
  }
  CHECK_FALSE(proof.stopped());
}

TEST_CASE("First stop evidence stays immutable and cannot stop a new epoch",
          "[graphics][completion][backend_stop]") {
  GpuBackendStopProof old_proof;
  std::function<void()> old_destroyed;
  {
    GpuCompletionQueue queue;
    old_proof = queue.backend_stop_proof();
    old_destroyed = queue.MakeAfterDeviceDestroyedCallback();
    queue.ReleaseAfterDeviceLostWaitIdle();
    queue.ReleaseAfterGpuStopped();
  }
  GpuCompletionQueue replacement;
  const auto fresh = replacement.backend_stop_proof();
  old_destroyed();
  CHECK(old_proof.evidence() ==
        GpuBackendStopProof::Evidence::kDeviceLostWaitIdle);
  CHECK(old_proof.backend_epoch() != fresh.backend_epoch());
  CHECK_FALSE(fresh.stopped());
}

TEST_CASE("Stop proof readers do not dereference the destroyed backend",
          "[graphics][completion][backend_stop]") {
  GpuBackendStopProof proof;
  std::function<void()> after_destroyed;
  {
    GpuCompletionQueue queue;
    proof = queue.backend_stop_proof();
    after_destroyed = queue.MakeAfterDeviceDestroyedCallback();
  }
  std::atomic<bool> done{false};
  std::atomic<bool> observed{false};
  std::thread reader([&] {
    while (!done.load(std::memory_order_acquire)) {
      if (proof.stopped()) {
        observed.store(true, std::memory_order_release);
        return;
      }
      std::this_thread::yield();
    }
    observed.store(proof.stopped(), std::memory_order_release);
  });
  after_destroyed();
  done.store(true, std::memory_order_release);
  reader.join();
  CHECK(observed.load(std::memory_order_acquire));
  CHECK(proof.evidence() == GpuBackendStopProof::Evidence::kDeviceDestroyed);
}

TEST_CASE("Generic and native tokens retain the exact queue stop epoch",
          "[graphics][completion][backend_stop]") {
  GpuCompletionQueue queue;
  const NativeFrameSubmissionIdentity identity{{101, 202, 303, 0x30020000}, 7};
  const auto title = queue.TrackSubmitted(1);
  const auto absent = queue.TrackSubmitted(0);
  auto resource = std::make_shared<int>(1);
  const std::weak_ptr<int> lifetime = resource;
  const auto native = queue.TrackNativeFrameSubmitted(2, identity, {resource});
  resource.reset();
  const auto title_proof = title.backend_stop_proof();
  const auto native_proof = native.backend_stop_proof();
  CHECK(title_proof.backend_epoch() == queue.backend_epoch());
  CHECK(native_proof.backend_epoch() == queue.backend_epoch());
  CHECK(absent.backend_stop_proof().backend_epoch() == queue.backend_epoch());
  CHECK_FALSE(title_proof.stopped());
  CHECK_FALSE(native_proof.stopped());
  queue.CompleteThrough(1);
  CHECK(title.status() == GpuCompletionStatus::kComplete);
  CHECK_FALSE(title_proof.stopped());  // One fence is not a backend stop.
  queue.FailPending();
  CHECK(native.status() == GpuCompletionStatus::kFailed);
  CHECK_FALSE(native_proof.stopped());
  CHECK_FALSE(lifetime.expired());
  // Trusted backend host model: actual device-wide wait returned DEVICE_LOST.
  queue.ReleaseAfterDeviceLostWaitIdle();
  CHECK(title_proof.evidence() ==
        GpuBackendStopProof::Evidence::kDeviceLostWaitIdle);
  CHECK(native_proof.evidence() == title_proof.evidence());
  CHECK(absent.backend_stop_proof().stopped());
  CHECK(lifetime.expired());
  CHECK(title.status() == GpuCompletionStatus::kComplete);
  CHECK(native.status() == GpuCompletionStatus::kFailed);
  REQUIRE(native.native_frame_identity());
  CHECK(*native.native_frame_identity() == identity);
  const auto after_stop = queue.TrackSubmitted(3);
  CHECK(after_stop.status() == GpuCompletionStatus::kFailed);
  CHECK(after_stop.backend_stop_proof().backend_epoch() ==
        queue.backend_epoch());
  CHECK(after_stop.backend_stop_proof().stopped());
}

TEST_CASE("A retained token gains no stop proof from CP queue destruction",
          "[graphics][completion][backend_stop]") {
  GpuCompletion token;
  GpuCompletionResources quarantine;
  std::function<void()> after_destroyed;
  const NativeFrameSubmissionIdentity identity{{11, 22, 33, 0x30020000}, 44};
  auto resource = std::make_shared<int>(1);
  const std::weak_ptr<int> lifetime = resource;
  unsigned notifications = 0;
  {
    GpuCompletionQueue queue;
    token = queue.TrackNativeFrameSubmitted(
        4, identity, {resource}, [&](const auto& failed) {
          ++notifications;
          CHECK(failed.status() == GpuCompletionStatus::kFailed);
          CHECK_FALSE(failed.backend_stop_proof().stopped());
          CHECK_FALSE(lifetime.expired());
        });
    resource.reset();
    after_destroyed = queue.MakeAfterDeviceDestroyedCallback();
    quarantine = queue.TakeFailedResources();
  }
  const auto retained_proof = token.backend_stop_proof();
  CHECK(retained_proof.backend_epoch() == token.backend_epoch());
  CHECK_FALSE(retained_proof.stopped());
  CHECK_FALSE(lifetime.expired());
  CHECK(token.status() == GpuCompletionStatus::kFailed);
  REQUIRE(token.native_frame_identity());
  CHECK(*token.native_frame_identity() == identity);
  // Host model of actual Device destruction; no Queue or CP pointer survives.
  after_destroyed();
  CHECK(retained_proof.evidence() ==
        GpuBackendStopProof::Evidence::kDeviceDestroyed);
  CHECK(token.backend_stop_proof().stopped());
  CHECK_FALSE(lifetime.expired());  // Proof does not destroy the quarantine.
  quarantine.clear();
  CHECK(lifetime.expired());
  CHECK(token.status() == GpuCompletionStatus::kFailed);
  CHECK(notifications == 1);
}

TEST_CASE("Unsupported completions never carry a backend stop proof",
          "[graphics][completion][backend_stop]") {
  const GpuCompletion absent;
  const auto unsupported = GpuCompletion::Unsupported([](const auto& token) {
    CHECK(token.backend_stop_proof().backend_epoch() == 0);
    CHECK_FALSE(token.backend_stop_proof().stopped());
  });
  GpuCompletionQueue unrelated;
  unrelated.ReleaseAfterGpuStopped();
  for (const auto& token : {absent, unsupported}) {
    CHECK(token.status() == GpuCompletionStatus::kUnsupported);
    CHECK(token.backend_epoch() == 0);
    CHECK(token.backend_stop_proof().backend_epoch() == 0);
    CHECK_FALSE(token.backend_stop_proof().stopped());
  }
}

TEST_CASE("Token stop proof survives healthy stop without reaching a new epoch",
          "[graphics][completion][backend_stop]") {
  GpuCompletion old;
  std::function<void()> old_device_destroyed;
  {
    GpuCompletionQueue queue;
    old = queue.TrackSubmitted(1);
    old_device_destroyed = queue.MakeAfterDeviceDestroyedCallback();
    queue.ReleaseAfterGpuStopped();
  }
  const auto old_proof = old.backend_stop_proof();
  CHECK(old_proof.backend_epoch() == old.backend_epoch());
  CHECK(old_proof.evidence() ==
        GpuBackendStopProof::Evidence::kExplicitGpuStopped);
  CHECK(old.status() == GpuCompletionStatus::kFailed);
  GpuCompletionQueue replacement;
  const auto fresh = replacement.TrackSubmitted(1);
  CHECK(old_proof.backend_epoch() !=
        fresh.backend_stop_proof().backend_epoch());
  old_device_destroyed();
  CHECK(old_proof.evidence() ==
        GpuBackendStopProof::Evidence::kExplicitGpuStopped);
  CHECK_FALSE(fresh.backend_stop_proof().stopped());
  replacement.CompleteThrough(1);
  CHECK(fresh.status() == GpuCompletionStatus::kComplete);
  CHECK_FALSE(fresh.backend_stop_proof().stopped());
  CHECK(old.status() == GpuCompletionStatus::kFailed);
}

}  // namespace
}  // namespace xe::gpu::test
