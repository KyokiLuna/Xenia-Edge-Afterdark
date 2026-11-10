/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia contributors. All rights reserved.                    *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */
#include "xenia/kernel/xam/tile_service.h"
#include "xenia/kernel/xam/tile_request.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/user_module.h"
#include "xenia/kernel/xthread.h"
#include "xenia/base/threading.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include "xenia/kernel/xam/xam_state.h"
#include "xenia/kernel/xam/profile_manager.h"
#include "xenia/kernel/xam/app_manager.h"
#include "xenia/kernel/testing/isolated_kernel_runtime_settings.h"
#include "xenia/emulator.h"
#include "xenia/memory.h"
#include "xenia/base/byte_order.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <random>
#include <string>
#include <vector>

#include "third_party/catch/include/catch.hpp"
#include "xenia/base/mutex.h"
#include "xenia/base/platform.h"
#include "xenia/kernel/util/xex2_info.h"

namespace xe::kernel::xam::test {
namespace {
constexpr X_HRESULT kNotFound = X_HRESULT_FROM_WIN32(X_ERROR_FILE_NOT_FOUND);
constexpr X_HRESULT kNotReady = X_HRESULT_FROM_WIN32(X_ERROR_NOT_READY);
constexpr X_HRESULT kNoMemory = 0x8007000E;
constexpr uint32_t kDashboard = 0xFFFE07D1;

// Original synthetic 2x2 RGBA fixture: red, half-alpha green, quarter-alpha
// blue, transparent white. No installed dashboard or profile asset bytes.
std::vector<uint8_t> Png() {
  return {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00,
          0x00, 0x0D, 0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x02,
          0x00, 0x00, 0x00, 0x02, 0x08, 0x06, 0x00, 0x00, 0x00, 0x72,
          0xB6, 0x0D, 0x24, 0x00, 0x00, 0x00, 0x17, 0x49, 0x44, 0x41,
          0x54, 0x78, 0x9C, 0x63, 0xF8, 0xCF, 0xC0, 0xF0, 0x1F, 0x08,
          0x1B, 0x18, 0x80, 0xB4, 0xC3, 0xFF, 0xFF, 0xFF, 0x19, 0x00,
          0x40, 0x18, 0x07, 0xBA, 0xE6, 0xFD, 0xE8, 0x64, 0x00, 0x00,
          0x00, 0x00, 0x49, 0x45, 0x4E, 0x44, 0xAE, 0x42, 0x60, 0x82};
}
void BE(std::vector<uint8_t>& bytes, size_t at, uint32_t value,
        size_t width = 4) {
  for (size_t i = 0; i < width; ++i) {
    bytes[at + width - 1 - i] = uint8_t(value >> (8 * i));
  }
}
void ChunkCrc(std::vector<uint8_t>& bytes, size_t at, size_t data_size) {
  uint32_t crc = UINT32_MAX;
  for (size_t i = at + 4; i < at + 8 + data_size; ++i) {
    crc ^= bytes[i];
    for (unsigned bit = 0; bit < 8; ++bit) {
      crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1)));
    }
  }
  BE(bytes, at + 8 + data_size, ~crc);
}
std::vector<uint8_t> WithDimensions(uint32_t width, uint32_t height) {
  auto bytes = Png();
  BE(bytes, 16, width);
  BE(bytes, 20, height);
  ChunkCrc(bytes, 8, 13);
  return bytes;
}
std::vector<uint8_t> InflatedBeyondImage() {
  // zlib stream for 256 zero bytes, exceeding this 2x2 image's admitted
  // decompressed bound. Reusing a real codec must not permit arbitrary growth.
  constexpr std::array<uint8_t, 12> compressed = {
      0x78, 0x9C, 0x63, 0x60, 0x18, 0xD9, 0x00, 0x00, 0x01, 0x00, 0x00, 0x01};
  const auto original = Png();
  std::vector<uint8_t> bytes(33 + 12 + compressed.size() + 12);
  std::copy_n(original.begin(), 33, bytes.begin());
  BE(bytes, 33, uint32_t(compressed.size()));
  BE(bytes, 37, 0x49444154);
  std::copy(compressed.begin(), compressed.end(), bytes.begin() + 41);
  ChunkCrc(bytes, 33, compressed.size());
  std::copy(original.end() - 12, original.end(), bytes.end() - 12);
  return bytes;
}

#if XE_PLATFORM_LINUX || XE_PLATFORM_MAC
std::vector<uint8_t> XamResource() {
  const std::array<std::string, 4> names = {
      "64_fffe07d10002000000010000.png", "32_fffe07d10002000100010001.png",
      "64_fffe07d10002000a0001000a.png", "32_fffe07d10002000c0001000c.png"};
  const auto png = Png();
  size_t directory_bytes = 0;
  for (const auto& name : names) {
    directory_bytes += 9 + name.size();
  }
  std::vector<uint8_t> package(0x16 + directory_bytes +
                               names.size() * png.size());
  BE(package, 0, 0x5855495A);
  BE(package, 4, 3);
  BE(package, 0x10, uint32_t(directory_bytes));
  BE(package, 0x14, uint32_t(names.size()), 2);
  size_t at = 0x16;
  for (size_t i = 0; i < names.size(); ++i) {
    BE(package, at, uint32_t(png.size()));
    BE(package, at + 4, uint32_t(i * png.size()));
    package[at + 8] = uint8_t(names[i].size());
    std::copy(names[i].begin(), names[i].end(), package.begin() + at + 9);
    std::copy(png.begin(), png.end(),
              package.begin() + 0x16 + directory_bytes + i * png.size());
    at += 9 + names[i].size();
  }
  // Same minimal offline XEX/PE resource layout used by module_resource_test.
  std::vector<uint8_t> xex(0x400 + 0x1000);
  BE(xex, 0, 0x58455832);
  BE(xex, 8, 0x400);
  BE(xex, 16, 0x100);
  BE(xex, 20, 2);
  BE(xex, 24, XEX_HEADER_FILE_FORMAT_INFO);
  BE(xex, 28, 0x40);
  BE(xex, 32, XEX_HEADER_RESOURCE_INFO);
  BE(xex, 36, 0x80);
  BE(xex, 0x40, 8);
  BE(xex, 0x80, 20);
  std::memcpy(xex.data() + 0x84, "shrdres", 7);
  BE(xex, 0x8C, 0x82000100);
  BE(xex, 0x90, uint32_t(package.size()));
  BE(xex, 0x104, 0x1000);
  BE(xex, 0x210, 0x82000000);
  xex[0x400] = 'M';
  xex[0x401] = 'Z';
  xex[0x43C] = 0x80;
  xex[0x480] = 'P';
  xex[0x481] = 'E';
  std::copy(package.begin(), package.end(), xex.begin() + 0x500);
  return xex;
}
struct InstalledFixture {
  std::filesystem::path owned_parent;
  std::filesystem::path root;
  std::shared_ptr<vfs::InstalledSystemSource> owner;
  InstalledFixture() {
    std::random_device rng;
    for (unsigned attempt = 0; attempt < 16; ++attempt) {
      const auto next =
          std::filesystem::temp_directory_path() /
          ("xenia-tile-" + std::to_string(rng()) + "-" + std::to_string(rng()));
      if (std::filesystem::create_directory(next)) {
        owned_parent = std::filesystem::canonical(next);
        root = owned_parent / "image";
        std::filesystem::create_directory(root);
        break;
      }
    }
    REQUIRE_FALSE(root.empty());
    std::filesystem::create_directory(root / "flash");
  }
  ~InstalledFixture() {
    owner.reset();
    std::error_code ignored;
    std::filesystem::remove_all(owned_parent, ignored);
  }
  void Write(const std::vector<uint8_t>& bytes) {
    std::ofstream stream(root / "flash" / "xam.xex", std::ios::binary);
    stream.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    REQUIRE(stream.good());
  }
  void Open(bool shared = false) {
    std::error_code error;
    owner = vfs::InstalledSystemSource::Create(
        shared ? owned_parent / "unused-local" : root,
        shared ? root : std::filesystem::path{}, error);
    REQUIRE(owner);
    REQUIRE_FALSE(error);
  }
};
#endif
}  // namespace

TEST_CASE("Raw tile preparation preserves capacity and owned payload contract",
          "[tile][resource]") {
  const auto original = Png();
  auto small = TileService::PrepareRaw(TileService::FromOwnedPng(original), 79);
  CHECK(small.status == X_E_INSUFFICIENT_BUFFER);
  CHECK(small.required == original.size());
  CHECK(small.payload.empty());
  auto full = TileService::PrepareRaw(TileService::FromOwnedPng(original), 80);
  CHECK(full.status == X_E_SUCCESS);
  CHECK(full.required == original.size());
  CHECK(full.payload == original);
  auto failed = TileService::PrepareRaw({X_E_ACCESS_DENIED, original, 17}, 80);
  CHECK(failed.status == X_E_ACCESS_DENIED);
  CHECK(failed.required == 0);
  CHECK(failed.payload.empty());
  CHECK(failed.source_generation == 17);
  CHECK(TileService::PrepareRaw(TileService::FromOwnedPng({}), 0).status ==
        kNotFound);
}

TEST_CASE("Tile texture preparation owns complete RGBA and row extent",
          "[tile][texture]") {
  auto decoded =
      TileService::PrepareTexture(TileService::FromOwnedPng(Png()), 12, 2);
  REQUIRE(decoded.status == X_E_SUCCESS);
  CHECK(decoded.width == 2);
  CHECK(decoded.height == 2);
  CHECK(decoded.row_bytes == 8);
  CHECK(decoded.destination_bytes == 20);
  const std::vector<uint8_t> expected = {255, 0, 0,   255, 0,   255, 0,   128,
                                         0,   0, 255, 64,  255, 255, 255, 0};
  CHECK(decoded.rgba == expected);
  CHECK(TileService::PrepareTexture(TileService::FromOwnedPng(Png()), 8, 4)
            .destination_bytes == 16);
}

TEST_CASE("Tile geometry rejects complete-row overflow instead of clipping",
          "[tile][texture]") {
  uint32_t stride = 8, height = 2;
  SECTION("short row") { stride = 7; }
  SECTION("short height with apparently sufficient packed area") {
    stride = 16;
    height = 1;
  }
  SECTION("zero stride") { stride = 0; }
  SECTION("zero height") { height = 0; }
  SECTION("overflowing declared capacity") { stride = 0x80000000; }
  SECTION("overflowing last row") { stride = UINT32_MAX; }
  auto rejected = TileService::PrepareTexture(TileService::FromOwnedPng(Png()),
                                              stride, height);
  CHECK(rejected.status == X_E_INVALIDARG);
  CHECK(rejected.rgba.empty());
  CHECK(rejected.destination_bytes == 0);
}

TEST_CASE("Tile texture admission rejects corrupt oversized and non-PNG data",
          "[tile][texture]") {
  auto png = Png();
  X_HRESULT expected = X_E_FAIL;
  SECTION("non-PNG") { png[0] = 0; }
  SECTION("truncated compressed data") { png.resize(44); }
  SECTION("forged huge chunk length before decoder allocation") {
    BE(png, 33, 0x10000000);
  }
  SECTION("compressed payload exceeds image-derived bound") {
    png = InflatedBeyondImage();
    expected = kNoMemory;
  }
  SECTION("native encoded capacity") {
    png.resize(TileService::kTextureEncodedLimit + 1);
    expected = X_E_INSUFFICIENT_BUFFER;
  }
  SECTION("bounded dimension admission") {
    png = WithDimensions(TileService::kDimensionLimit + 1, 2);
    expected = kNoMemory;
  }
  SECTION("bounded decoded size before allocation") {
    png = WithDimensions(4096, 4096);
    expected = kNoMemory;
  }
  auto result =
      TileService::PrepareTexture(TileService::FromOwnedPng(png), 8, 2);
  CHECK(result.status == expected);
  CHECK(result.rgba.empty());
  CHECK(result.width == 0);
}

TEST_CASE("Stock tile keys retain full item halves and selection flags",
          "[tile][resource]") {
  CHECK(TileService::SelectStock(7, kDashboard, (uint64_t(3) << 32) | 9) ==
        TileService::StockPicture{3, false});
  CHECK(TileService::SelectStock(8, kDashboard, (uint64_t(3) << 32) | 9) ==
        TileService::StockPicture{9, true});
  CHECK(TileService::SelectStock(9, kDashboard, 0x20004) ==
        TileService::StockPicture{4, false});
  CHECK(TileService::SelectStock(9, kDashboard, 4) ==
        TileService::StockPicture{4, true});
  CHECK(TileService::SelectStock(8, kDashboard, 13) ==
        TileService::StockPicture{13, true});
  CHECK_FALSE(TileService::SelectStock(7, 0x12345678, 0));
  CHECK_FALSE(TileService::SelectStock(12, kDashboard, 0));
}

#if XE_PLATFORM_LINUX || XE_PLATFORM_MAC
TEST_CASE("Stock tiles use retained readonly XAM resources without a profile",
          "[tile][resource][storage]") {
  InstalledFixture fixture;
  const auto module = XamResource();
  fixture.Write(module);
  fixture.Open(true);
  const auto snapshot = fixture.owner->snapshot();
  X_HRESULT status;
  const auto source = TileService::CaptureStockSource(snapshot, status);
  REQUIRE(source);
  REQUIRE(status == X_E_SUCCESS);
  CHECK(source->generation() == snapshot->generation());
  auto large = TileService::ReadStock(*source, {0, false});
  REQUIRE(large.status == X_E_SUCCESS);
  CHECK(large.bytes == Png());
  CHECK(large.source_generation == snapshot->generation());
  CHECK(TileService::ReadStock(*source, {1, true}).bytes == Png());
  CHECK(TileService::ReadStock(*source, {10, false}).bytes == Png());
  CHECK(TileService::ReadStock(*source, {12, true}).bytes == Png());
  CHECK(TileService::ReadStock(*source, {0, true}).status == kNotFound);
  CHECK(TileService::ReadStock(*source, {13, false}).status == kNotFound);
  std::error_code error;
  auto flash = snapshot->PartitionRoot(
      vfs::InstalledSystemSource::Partition::kFlash, error);
  REQUIRE(flash);
  CHECK(flash->read_only());
  CHECK(flash->Create("not-authorized", false) ==
        std::errc::read_only_file_system);
  std::ifstream stream(fixture.root / "flash" / "xam.xex", std::ios::binary);
  const std::vector<uint8_t> after{std::istreambuf_iterator<char>(stream), {}};
  CHECK(after == module);
  CHECK_FALSE(std::filesystem::exists(fixture.owned_parent / "unused-local"));
}

TEST_CASE("Stock captures retire with their installed generation not a path",
          "[tile][resource][storage]") {
  InstalledFixture fixture;
  fixture.Write(XamResource());
  fixture.Open();
  X_HRESULT status;
  const auto old =
      TileService::CaptureStockSource(fixture.owner->snapshot(), status);
  REQUIRE(old);
  auto completed = TileService::ReadStock(*old, {0, false});
  REQUIRE(completed.status == X_E_SUCCESS);
  std::error_code error;
  const auto next = fixture.owner->PrepareRefresh(error, true);
  REQUIRE(next);
  REQUIRE(fixture.owner->Commit(next, error));
  CHECK(TileService::ReadStock(*old, {0, false}).status == kNotReady);
  auto already_owned = TileService::PrepareRaw(std::move(completed), 80);
  CHECK(already_owned.status == X_E_SUCCESS);
  CHECK(already_owned.payload == Png());
  const auto fresh = TileService::CaptureStockSource(next, status);
  REQUIRE(fresh);
  CHECK(fresh->generation() > old->generation());
  CHECK(TileService::ReadStock(*fresh, {0, false}).status == X_E_SUCCESS);
}

TEST_CASE(
    "Stock provider created before installation follows the committed source",
    "[tile][resource][storage]") {
  InstalledFixture fixture;
  const auto installed = fixture.root / "not-yet-installed";
  std::error_code error;
  fixture.owner = vfs::InstalledSystemSource::Create(installed, {}, error);
  REQUIRE(fixture.owner);
  REQUIRE_FALSE(error);
  TileService::StockProvider provider(fixture.owner);
  X_HRESULT status;
  CHECK_FALSE(provider.Capture(status));
  CHECK(status == kNotReady);
  CHECK(TileService::ReadStock(provider, {0, false}).status == kNotReady);
  CHECK_FALSE(std::filesystem::exists(installed));

  REQUIRE(std::filesystem::create_directories(installed / "flash"));
  {
    const auto bytes = XamResource();
    std::ofstream stream(installed / "flash" / "xam.xex", std::ios::binary);
    stream.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    REQUIRE(stream.good());
  }
  const auto next = fixture.owner->PrepareRefresh(error, true);
  REQUIRE(next);
  REQUIRE_FALSE(error);
  REQUIRE(fixture.owner->Commit(next, error));
  REQUIRE_FALSE(error);
  const auto captured = provider.Capture(status);
  REQUIRE(captured);
  CHECK(status == X_E_SUCCESS);
  CHECK(captured->generation() == next->generation());
  CHECK(provider.Capture(status) == captured);
  const auto png = TileService::ReadStock(provider, {0, false});
  CHECK(png.status == X_E_SUCCESS);
  CHECK(png.bytes == Png());
  CHECK(png.source_generation == next->generation());
}

TEST_CASE("Stock provider replacement never revives a retired cached source",
          "[tile][resource][storage]") {
  InstalledFixture fixture;
  fixture.Write(XamResource());
  fixture.Open();
  TileService::StockProvider provider(fixture.owner);
  X_HRESULT status;
  const auto old = provider.Capture(status);
  REQUIRE(old);
  auto completed = TileService::ReadStock(*old, {0, false});
  REQUIRE(completed.status == X_E_SUCCESS);
  std::error_code error;
  const auto next = fixture.owner->PrepareRefresh(error, true);
  REQUIRE(next);
  REQUIRE(fixture.owner->Commit(next, error));
  const auto fresh = provider.Capture(status);
  REQUIRE(fresh);
  CHECK(fresh != old);
  CHECK(fresh->generation() == next->generation());
  CHECK(fresh->generation() > old->generation());
  CHECK(TileService::ReadStock(*old, {0, false}).status == kNotReady);
  CHECK(TileService::ReadStock(provider, {0, false}).status == X_E_SUCCESS);

  fixture.Write({1, 2, 3, 4});
  const auto broken = fixture.owner->PrepareRefresh(error, true);
  REQUIRE(broken);
  REQUIRE(fixture.owner->Commit(broken, error));
  CHECK_FALSE(provider.Capture(status));
  CHECK(status == X_E_FAIL);
  const auto failed = TileService::ReadStock(provider, {0, false});
  CHECK(failed.status == X_E_FAIL);
  CHECK(failed.bytes.empty());
  CHECK(TileService::ReadStock(*fresh, {0, false}).status == kNotReady);

  // A source read failure is not a permanent cache entry. Retrying this same
  // selected generation after the resource is available performs real IO.
  fixture.Write(XamResource());
  const auto repaired = provider.Capture(status);
  REQUIRE(repaired);
  CHECK(repaired->generation() == broken->generation());
  CHECK(TileService::ReadStock(provider, {0, false}).bytes == Png());
  // Already detached async bytes keep their original generation; no recapture
  // can change their identity or revive the retired StockSource.
  const auto owned = TileService::PrepareRaw(std::move(completed), 80);
  CHECK(owned.status == X_E_SUCCESS);
  CHECK(owned.payload == Png());
  CHECK(owned.source_generation == old->generation());
}

TEST_CASE(
    "Stock providers keep independent authorities and reject GCRC capture",
    "[tile][resource][storage]") {
  InstalledFixture available;
  available.Write(XamResource());
  available.Open();
  InstalledFixture missing;
  missing.Open();
  REQUIRE(available.owner->snapshot()->generation() ==
          missing.owner->snapshot()->generation());
  TileService::StockProvider good(available.owner), absent(missing.owner);
  X_HRESULT status;
  const auto captured = good.Capture(status);
  REQUIRE(captured);
  CHECK_FALSE(absent.Capture(status));
  CHECK(status == kNotFound);
  CHECK(TileService::ReadStock(absent, {0, false}).bytes.empty());
  {
    global_critical_region region;
    auto guard = region.Acquire();
    CHECK_FALSE(good.Capture(status));
    CHECK(status == X_E_INVALIDARG);
    CHECK(TileService::ReadStock(good, {0, false}).status == X_E_INVALIDARG);
  }
  CHECK(good.Capture(status) == captured);
  CHECK(status == X_E_SUCCESS);
}

TEST_CASE("Stock source failures do not adopt globals or wait under GCRC",
          "[tile][resource][storage]") {
  X_HRESULT status = X_E_SUCCESS;
  CHECK_FALSE(TileService::CaptureStockSource({}, status));
  CHECK(status == kNotReady);
  InstalledFixture fixture;
  fixture.Open();
  const auto image = fixture.owner->snapshot();
  CHECK_FALSE(TileService::CaptureStockSource(image, status));
  CHECK(status == kNotFound);
  fixture.Write({1, 2, 3, 4});
  CHECK_FALSE(TileService::CaptureStockSource(image, status));
  CHECK(status == X_E_FAIL);
  {
    global_critical_region region;
    auto guard = region.Acquire();
    CHECK_FALSE(TileService::CaptureStockSource(image, status));
    CHECK(status == X_E_INVALIDARG);
  }
}

namespace {
struct TileApiRuntime {
  xe::kernel::test::IsolatedKernelRuntimeSettings settings;
  InstalledFixture installed;
  std::unique_ptr<Emulator> emulator;
  TileApiRuntime() {
    installed.Write(XamResource());
    cvars::system_readonly_source_root = installed.root;
    const auto root = installed.owned_parent / "runtime";
    REQUIRE(std::filesystem::create_directory(root));
    REQUIRE(std::filesystem::create_directory(root / "content"));
    REQUIRE(std::filesystem::create_directory(root / "cache"));
    REQUIRE(KernelState::shared() == nullptr);
    emulator = std::make_unique<Emulator>("", root, root / "content", root / "cache");
    REQUIRE(emulator->Setup(nullptr, nullptr, false, {}, {}, {}) == X_STATUS_SUCCESS);
    REQUIRE(emulator->kernel_state()->xam_state()->profile_manager()->GetAccountCount() == 0);
  }
  void StartDispatcher() {
    // A structural PPC32 ELF, never executed by NullBackend, starts the real
    // Kernel dispatch worker through the same production title setup method.
    std::vector<uint8_t> elf(0x104);
    const auto b16=[&](size_t at,uint16_t value){xe::store_and_swap<uint16_t>(elf.data()+at,value);};
    const auto b32=[&](size_t at,uint32_t value){xe::store_and_swap<uint32_t>(elf.data()+at,value);};
    elf[0]=0x7F;elf[1]='E';elf[2]='L';elf[3]='F';elf[4]=1;elf[5]=2;elf[6]=1;
    b16(16,2);b16(18,20);b32(20,1);b32(24,0x82000000);b32(28,52);
    b16(40,52);b16(42,32);b16(44,1);
    b32(52,1);b32(56,0x100);b32(60,0x82000000);b32(64,0x82000000);
    b32(68,4);b32(72,0x1000);b32(76,5);b32(80,0x1000);b32(0x100,0x4E800020);
    auto module=object_ref<UserModule>(new UserModule(&kernel()));
    REQUIRE(module->LoadFromMemoryNamed("tile-async-fixture",elf.data(),elf.size())==X_STATUS_SUCCESS);
    kernel().SetExecutableModule(module);
  }
  ~TileApiRuntime() { emulator.reset(); }
  KernelState& kernel() { return *emulator->kernel_state(); }
};
struct TileApiPage {
  Memory& memory;
  uint32_t address;
  explicit TileApiPage(Memory& mem) : memory(mem), address(mem.SystemHeapAlloc(0x4000, 0x1000)) {
    REQUIRE(address); std::memset(mem.TranslateVirtual(address), 0xA5, 0x4000);
  }
  ~TileApiPage() { memory.SystemHeapFree(address); }
  uint32_t packet() const { return address + 0x200; }
  uint32_t size() const { return address + 0x80; }
  uint32_t output() const { return address + 0x1000; }
  void Word(uint32_t at, uint32_t value) { xe::store_and_swap<uint32_t>(memory.TranslateVirtual(at), value); }
  uint32_t Word(uint32_t at) { return xe::load_and_swap<uint32_t>(memory.TranslateVirtual(at)); }
  void Packet(uint32_t message, uint32_t capacity = 128) {
    std::memset(memory.TranslateVirtual(output() - 4), 0xA5, 264);
    std::array<uint8_t, 0x38> bytes{};
    const auto word = [&](size_t at, uint32_t value) { xe::store_and_swap<uint32_t>(bytes.data() + at, value); };
    word(0, 7); word(4, kDashboard);
    // Only large image A exists, not large image1. Lost high32 bits fail.
    xe::store_and_swap<uint64_t>(bytes.data() + 8, (uint64_t(0xA) << 32) | 1);
    word(0x10, 0); word(0x14, output()); word(0x18, size());
    word(0x20, 12); word(0x24, 2);
    std::memcpy(memory.TranslateVirtual(packet()), bytes.data(), bytes.size());
    Word(size(), capacity);
    (void)message;
  }
  bool Marker(uint32_t at, size_t count) {
    const auto* first = memory.TranslateVirtual<const uint8_t*>(at);
    return std::all_of(first, first + count, [](uint8_t x) { return x == 0xA5; });
  }
};
}
TEST_CASE("Builtin FB tile raw and texture messages share genuine owned provider bytes",
          "[kernel][tile][fb][abi]") {
  TileApiRuntime runtime; TileApiPage page(*runtime.emulator->memory());
  for (const auto message : {0xB0030u, 0xB0032u, 0xB003Au, 0xB003Bu}) {
    DYNAMIC_SECTION("message " << message) {
      page.Packet(message);
      const bool texture = message == 0xB0032 || message == 0xB003B;
      const auto length = message >= 0xB003A ? 0x38u : 0x28u;
      REQUIRE(runtime.kernel().app_manager()->DispatchMessageSync(0xFB, message, page.packet(), length) == X_E_SUCCESS);
      const auto* pixels = page.memory.TranslateVirtual<const uint8_t*>(page.output());
      if (texture) {
        const std::array<uint8_t, 8> first{255,255,0,0, 128,0,255,0};
        const std::array<uint8_t, 8> second{64,0,0,255, 0,255,255,255};
        CHECK(std::equal(first.begin(), first.end(), pixels));
        CHECK(std::equal(second.begin(), second.end(), pixels + 12));
        CHECK(page.Marker(page.output() + 8, 4)); CHECK(page.Marker(page.output() + 20, 4));
      } else {
        const auto png = Png(); CHECK(std::equal(png.begin(), png.end(), pixels));
        CHECK(page.Word(page.size()) == png.size()); CHECK(page.Marker(page.output() + png.size(), 4));
      }
      CHECK(page.Marker(page.output() - 4, 4));
    }
  }
}
TEST_CASE("Builtin FB tile capacity and unsupported Ex fields preserve output guards",
          "[kernel][tile][fb][abi]") {
  TileApiRuntime runtime; TileApiPage page(*runtime.emulator->memory());
  page.Packet(0xB0030, 1);
  CHECK(runtime.kernel().app_manager()->DispatchMessageSync(0xFB, 0xB0030, page.packet(), 0x28) == X_E_INSUFFICIENT_BUFFER);
  CHECK(page.Word(page.size()) == Png().size()); CHECK(page.Marker(page.output(), 128));
  page.Packet(0xB003A); page.Word(page.packet() + 0x28, 1);
  CHECK(runtime.kernel().app_manager()->DispatchMessageSync(0xFB, 0xB003A, page.packet(), 0x38) == X_E_NOT_IMPLEMENTED);
  CHECK(page.Word(page.size()) == 128); CHECK(page.Marker(page.output(), 128));
  page.Packet(0xB003B); page.Word(page.packet(), 12);
  CHECK(runtime.kernel().app_manager()->DispatchMessageSync(0xFB, 0xB003B, page.packet(), 0x38) == X_E_NOT_IMPLEMENTED);
  CHECK(page.Marker(page.output(), 128));
  page.Packet(0xB0030);
  CHECK(runtime.kernel().app_manager()->DispatchMessageSync(0xFB, 0xB0030, page.packet(), 0x20) == X_E_INVALIDARG);
  CHECK(page.Word(page.size()) == 128); CHECK(page.Marker(page.output(), 128));
}
TEST_CASE("Owned FB tile capture survives nested input reuse before deferred publication",
          "[kernel][tile][fb][async]") {
  TileApiRuntime runtime; TileApiPage page(*runtime.emulator->memory());
  page.Packet(0xB0030, 128);
  const auto overlap = page.address + 0x140;
  std::memset(page.memory.TranslateVirtual(overlap), 0, sizeof(XAM_OVERLAPPED));
  X_HRESULT status = X_E_FAIL;
  auto request = CaptureTileMessage(runtime.kernel(), 0xB0030, page.packet(),
                                    0x28, overlap, status);
  REQUIRE(request); REQUIRE(status == X_E_SUCCESS);
  // This is the exact production request retained by the deferred callback.
  // No fake executable/worker is installed in this headless ownership test.
  std::memset(page.memory.TranslateVirtual(page.packet()), 0, 0x38);
  page.Word(page.size(), 1); // original128-byte capacity was already captured
  uint32_t error = X_E_FAIL, length = 77;
  REQUIRE(CompleteTileRequest(runtime.kernel(), *request, error, length) == X_ERROR_SUCCESS);
  CHECK(error == X_E_SUCCESS); CHECK(length == 0);
  CHECK(page.Word(page.size()) == Png().size());
  const auto png = Png(); CHECK(std::equal(png.begin(), png.end(), page.memory.TranslateVirtual<const uint8_t*>(page.output())));
  CHECK(page.Marker(page.output() + png.size(), 4));
  CHECK(page.Word(overlap) == 0); // caller/Kernel owns completion-record writes
  // Destination/capacity and OVERLAPPED may not overlap, including aliases.
  page.Packet(0xB0030, 128); page.Word(page.packet() + 0x14, overlap);
  CHECK_FALSE(CaptureTileMessage(runtime.kernel(), 0xB0030, page.packet(), 0x28, overlap, status));
  CHECK(status == X_E_INVALIDARG);
}
TEST_CASE("Actual FB async dispatch owns nested input until Kernel completion",
          "[kernel][tile][fb][async]") {
  TileApiRuntime runtime; TileApiPage page(*runtime.emulator->memory());
  runtime.StartDispatcher();
  struct Barrier {
    std::mutex mutex; std::condition_variable changed;
    bool entered=false, release=false, finished=false;
  };
  auto barrier=std::make_shared<Barrier>();
  const auto blocker=page.address+0x100, overlap=page.address+0x140, fence=page.address+0x180;
  for (auto at:{blocker,overlap,fence}) {std::memset(page.memory.TranslateVirtual(at),0,sizeof(XAM_OVERLAPPED));}
  struct Observed {X_HRESULT returned=X_E_FAIL;bool pending=false,finished=false;} observed;
  // CompleteOverlappedDeferredEx captures the genuine calling guest thread.
  // Every queue/capture call originates on this real owned title XHostThread.
  auto producer=object_ref<XHostThread>(new XHostThread(&runtime.kernel(),128*1024,0,[&]{
    runtime.kernel().CompleteOverlappedDeferredEx([barrier](uint32_t& error,uint32_t& length){
      std::unique_lock lock(barrier->mutex);barrier->entered=true;barrier->changed.notify_all();
      const bool released=barrier->changed.wait_for(lock,std::chrono::seconds(5),[&]{return barrier->release;});
      error=released?X_E_SUCCESS:X_E_FAIL;length=0;
      return released?X_ERROR_SUCCESS:X_ERROR_FUNCTION_FAILED;
    },blocker);
    {
      std::unique_lock lock(barrier->mutex);
      if (!barrier->changed.wait_for(lock,std::chrono::seconds(4),[&]{return barrier->entered;})) {return 1;}
    }
    page.Packet(0xB0030,128);
    observed.returned=runtime.kernel().app_manager()->DispatchMessageAsync(0xFB,0xB0030,page.packet(),0x28,overlap);
    if (observed.returned!=X_ERROR_IO_PENDING) {return 2;}
    observed.pending=page.Word(overlap)==X_ERROR_IO_PENDING;
    std::memset(page.memory.TranslateVirtual(page.packet()),0,0x38);
    page.Word(page.size(),1);
    runtime.kernel().CompleteOverlappedDeferredEx([](uint32_t& error,uint32_t& length){
      error=0;length=0;return X_ERROR_SUCCESS;
    },fence,{},[barrier]{
      {std::lock_guard lock(barrier->mutex);barrier->finished=true;}
      barrier->changed.notify_all();
    });
    {
      std::unique_lock lock(barrier->mutex);barrier->release=true;barrier->changed.notify_all();
      observed.finished=barrier->changed.wait_for(lock,std::chrono::seconds(5),[&]{return barrier->finished;});
    }
    return observed.finished?0:3;
  },runtime.kernel().GetTitleProcess()));
  bool started=false;
  struct Drain {
    KernelState& kernel;std::shared_ptr<Barrier> barrier;
    object_ref<XHostThread>& producer;bool& started;
    ~Drain() {
      {std::lock_guard lock(barrier->mutex);barrier->release=true;}
      barrier->changed.notify_all();
      kernel.ShutdownDispatchThread(); // sole dispatch join owner
      if (started) {xe::threading::Wait(producer->thread(),false);}
      // No borrowed guest page/callback capture may die before BOTH joins.
    }
  } drain{runtime.kernel(),barrier,producer,started};
  const auto created=producer->Create();started=created==X_STATUS_SUCCESS;
  REQUIRE(started);
  REQUIRE(xe::threading::Wait(producer->thread(),false,std::chrono::seconds(12))==xe::threading::WaitResult::kSuccess);
  REQUIRE(observed.returned==X_ERROR_IO_PENDING);REQUIRE(observed.pending);REQUIRE(observed.finished);
  // FIFO post-completion mutex + actual producer join orders all guest stores
  // before these host reads; no polling/data race or fabricated TLS is used.
  CHECK(page.Word(overlap)==X_ERROR_SUCCESS);CHECK(page.Word(overlap+0x18)==X_E_SUCCESS);
  CHECK(page.Word(overlap+4)==0);CHECK(page.Word(page.size())==Png().size());
  const auto png=Png();CHECK(std::equal(png.begin(),png.end(),page.memory.TranslateVirtual<const uint8_t*>(page.output())));
  CHECK(page.Marker(page.output()+png.size(),4));
}
TEST_CASE("Registered guest FB retains sync precedence and asynchronous refusal",
          "[kernel][tile][fb][lifecycle]") {
  TileApiRuntime runtime; TileApiPage page(*runtime.emulator->memory());
  auto& registry = runtime.kernel().app_manager()->guest_apps();
  const auto pin = std::make_shared<uint32_t>(17); unsigned calls = 0;
  REQUIRE(registry.Prepare(0xFB, 0x1234, pin));
  REQUIRE(registry.Register(0x1234, 0xFB, 0x12340000, 0,
      [&](uint32_t message, uint32_t, uint32_t) {
        CHECK(message == 0xB0030); ++calls;
        return GuestAppRegistry::ExecutionResult{true, 0x81234567};
      }, pin) == X_ERROR_SUCCESS);
  const auto registration = registry.Lookup(0xFB, true); REQUIRE(registration);
  REQUIRE(registry.Activate(registration));
  // Invalid descriptor is deliberate: the builtin must not inspect it when
  // the earlier registered guest provider owns the route.
  CHECK(runtime.kernel().app_manager()->DispatchMessageSync(0xFB, 0xB0030, 0, 0x28) == 0x81234567);
  CHECK(calls == 1);
  CHECK(runtime.kernel().app_manager()->DispatchMessageAsync(0xFB, 0xB0030, 0, 0x28,
      page.address + 0x100) == X_E_NOT_IMPLEMENTED);
  CHECK(calls == 1); CHECK(page.Marker(page.address + 0x100, sizeof(XAM_OVERLAPPED)));
}

#endif
}  // namespace xe::kernel::xam::test
