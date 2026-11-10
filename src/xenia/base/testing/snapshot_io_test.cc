/** Xenia : Xbox 360 Emulator Research Project; BSD license, see LICENSE. */
#include "xenia/base/snapshot_io.h"
#include "xenia/base/testing/snapshot_fixture.h"
#if XE_PLATFORM_LINUX
namespace xe::filesystem::test {
namespace {
struct StageFaultWriter {
  HostStorageSnapshotStage& real;
  size_t maximum = 1;
  bool fail = false, zero = false;
  std::atomic<bool>* cancel_after_write = nullptr;
  FileIoResult WriteResult(size_t offset, std::span<const uint8_t> bytes) {
    if (zero) { return {}; }
    auto result = real.WriteResult(offset, bytes.first(std::min(maximum, bytes.size())));
    if (!result.error && fail) { result.error = std::make_error_code(std::errc::io_error); }
    if (cancel_after_write) { cancel_after_write->store(true, std::memory_order_release); }
    return result;
  }
};
}
TEST_CASE("Snapshot transfer reports real partial bytes when the backend then fails",
          "[snapshot_publication][snapshot_transfer]") {
  SnapshotFixture f;
  bool created;
  std::error_code error;
  auto stage = f.profile->CreateSnapshotStage("stage.tmp", created, error);
  REQUIRE(stage);
  StageFaultWriter fault{*stage, 2, true, false, nullptr};
  const std::array<uint8_t, 4> next{4, 5, 6, 7};
  auto result = WriteSnapshotBytes(fault, next);
  REQUIRE(result.error == std::errc::io_error);
  REQUIRE(result.bytes == 2);
  stage.reset();
  REQUIRE(f.Bytes("stage.tmp") == std::vector<uint8_t>{4, 5});
}
TEST_CASE("Snapshot transfer loops actual short writes without assuming completion",
          "[snapshot_publication][snapshot_transfer]") {
  SnapshotFixture f;
  bool created;
  std::error_code error;
  auto stage = f.profile->CreateSnapshotStage("stage.tmp", created, error);
  REQUIRE(stage);
  StageFaultWriter fault{*stage};
  const std::array<uint8_t, 4> next{4, 5, 6, 7};
  auto result = WriteSnapshotBytes(fault, next);
  REQUIRE_FALSE(result.error);
  REQUIRE(result.bytes == 4);
  REQUIRE_FALSE(stage->FlushResult());
  stage.reset();
  REQUIRE(f.Bytes("stage.tmp") == std::vector<uint8_t>{4, 5, 6, 7});
}
TEST_CASE("Snapshot transfer zero progress returns an error with no invented bytes",
          "[snapshot_publication][snapshot_transfer]") {
  SnapshotFixture f;
  bool created;
  std::error_code error;
  auto stage = f.profile->CreateSnapshotStage("stage.tmp", created, error);
  REQUIRE(stage);
  StageFaultWriter fault{*stage, 1, false, true, nullptr};
  const std::array<uint8_t, 1> next{8};
  auto result = WriteSnapshotBytes(fault, next);
  REQUIRE(result.error == std::errc::io_error);
  REQUIRE(result.bytes == 0);
  stage.reset();
  REQUIRE(f.Bytes("stage.tmp").empty());
}
TEST_CASE("Snapshot transfer cancellation preserves real completed prefix",
          "[snapshot_publication][snapshot_transfer]") {
  SnapshotFixture f;
  bool created;
  std::error_code error;
  auto stage = f.profile->CreateSnapshotStage("stage.tmp", created, error);
  REQUIRE(stage);
  std::atomic<bool> canceled{false};
  StageFaultWriter fault{*stage, 2, false, false, &canceled};
  const std::array<uint8_t, 4> next{4, 5, 6, 7};
  auto result = WriteSnapshotBytes(fault, next, &canceled);
  REQUIRE(result.error == std::errc::operation_canceled);
  REQUIRE(result.bytes == 2);
  stage.reset();
  REQUIRE(f.Bytes("stage.tmp") == std::vector<uint8_t>{4, 5});
}
}  // namespace xe::filesystem::test
#endif
