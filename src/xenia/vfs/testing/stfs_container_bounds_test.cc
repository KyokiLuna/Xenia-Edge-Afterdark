/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/vfs/devices/xcontent_container_device.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <iterator>
#include <random>
#include <string_view>
#include <vector>

#include "third_party/catch/include/catch.hpp"
#include "xenia/base/filesystem.h"
#include "xenia/base/math.h"
#include "xenia/base/host_storage.h"
#include "xenia/vfs/devices/host_path_device.h"
#include "xenia/vfs/devices/xcontent_container_entry.h"
#include "xenia/vfs/devices/xcontent_devices/stfs_container_device.h"
#include "xenia/vfs/entry.h"
#include "xenia/vfs/stfs_metadata.h"
#include "xenia/vfs/virtual_file_system.h"

namespace xe::vfs::test {
namespace {

struct StfsScratch {
  std::filesystem::path root;
  StfsScratch() {
    std::random_device rng;
    for (unsigned i = 0; i < 16; ++i) {
      auto candidate = std::filesystem::temp_directory_path() /
                       ("xenia_stfs_bounds_" + std::to_string(rng()) + "_" +
                        std::to_string(rng()));
      if (std::filesystem::create_directory(candidate)) {
        root = std::move(candidate);
        break;
      }
    }
    REQUIRE_FALSE(root.empty());
  }
  ~StfsScratch() {
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
  }
};

void SetFile(StfsDirectoryEntry &entry, std::string_view name,
             uint32_t length = 16, uint16_t parent = 0xFFFF) {
  REQUIRE(name.size() <= sizeof(entry.name));
  entry = {};
  std::memcpy(entry.name, name.data(), name.size());
  entry.flags.name_length = uint8_t(name.size());
  entry.flags.contiguous = true;
  const uint32_t blocks = uint32_t((uint64_t(length) + 0xFFF) / 0x1000);
  entry.set_valid_data_blocks(blocks);
  entry.set_allocated_data_blocks(blocks);
  entry.set_start_block_number(1);
  entry.directory_index = parent;
  entry.length = length;
}

struct StfsFixture {
  static constexpr size_t kBlock = 0x1000;
  std::unique_ptr<XContentContainerHeader> header =
      std::make_unique<XContentContainerHeader>();
  StfsHashTable hash{};
  StfsDirectoryBlock directory{};
  bool read_only;

  explicit StfsFixture(bool readonly = true) : read_only(readonly) {
    header->content_header.magic =
        readonly ? XContentPackageType::kPirs : XContentPackageType::kCon;
    header->content_header.header_size = sizeof(*header);
    header->content_metadata.volume_type = XContentVolumeType::kStfs;
    header->content_metadata.content_type = XContentType::kXbox360Title;
    header->content_metadata.execution_info.title_id = 0x584111F7;
    auto &volume = header->content_metadata.volume_descriptor.stfs;
    volume.descriptor_length = sizeof(StfsVolumeDescriptor);
    volume.flags.bits.read_only_format = readonly;
    volume.file_table_block_count = 1;
    volume.set_file_table_block_number(0);
    volume.total_block_count = 2;
    hash.entries[0].set_level0_next_block(0xFFFFFF);
    hash.entries[1].set_level0_next_block(0xFFFFFF);
    SetFile(directory.entries[0], "payload");
  }

  std::vector<uint8_t> Bytes() const {
    const size_t base = xe::round_up(sizeof(*header), kBlock);
    const size_t hash_blocks = read_only ? 1 : 2;
    std::vector<uint8_t> bytes(base + (hash_blocks + 2) * kBlock);
    std::memcpy(bytes.data(), header.get(), sizeof(*header));
    std::memcpy(bytes.data() + base, &hash, sizeof(hash));
    std::memcpy(bytes.data() + base + hash_blocks * kBlock, &directory,
                sizeof(directory));
    const size_t payload = base + (hash_blocks + 1) * kBlock;
    for (size_t i = 0; i < 16; ++i) {
      bytes[payload + i] = uint8_t(i);
    }
    return bytes;
  }
};

void Write(const std::filesystem::path &path,
           const std::vector<uint8_t> &bytes) {
  std::ofstream file(path, std::ios::binary);
  file.write(reinterpret_cast<const char *>(bytes.data()), bytes.size());
  REQUIRE(file.good());
}

struct BackingReadState {
  size_t calls = 0;
  std::optional<uint64_t> short_at;
  std::optional<uint64_t> error_at;
  X_STATUS error_status = X_STATUS_ACCESS_DENIED;
  size_t error_prefix = 0;
};

// Faults wrap real package reads; no successful bytes or mount authority are
// invented. The public bounded Read must reject an invalid range before this.
class ObservedBacking final : public XContentReadBacking {
 public:
  ObservedBacking(std::unique_ptr<XContentReadBacking> inner,
                  std::shared_ptr<BackingReadState> state)
      : XContentReadBacking(inner->extent()), inner_(std::move(inner)),
        state_(std::move(state)) {}
  std::optional<std::vector<filesystem::GuestHostBacking>> GuestBackings()
      const override { return inner_->GuestBackings(); }

 protected:
  X_STATUS ReadPartial(size_t offset, std::span<uint8_t> output,
                       size_t* transferred) const override {
    ++state_->calls;
    *transferred = 0;
    size_t count = output.size();
    if (state_->short_at == offset && count) { --count; }
    if (state_->error_at == offset) {
      count = std::min(count, state_->error_prefix);
    }
    const auto status = inner_->ReadWithStatus(offset, output.first(count),
                                              transferred);
    if (status != X_STATUS_SUCCESS) { return status; }
    return state_->error_at == offset ? state_->error_status : X_STATUS_SUCCESS;
  }

 private:
  std::unique_ptr<XContentReadBacking> inner_;
  std::shared_ptr<BackingReadState> state_;
};

} // namespace

TEST_CASE("STFS keeps valid nested directories and payload reads",
          "[stfs][container-bounds]") {
  StfsScratch scratch;
  StfsFixture fixture;
  auto &directory = fixture.directory.entries[0];
  SetFile(directory, "Assets", 0);
  directory.flags.directory = true;
  SetFile(fixture.directory.entries[1], "payload", 16, 0);
  const auto path = scratch.root / "valid";
  Write(path, fixture.Bytes());
  auto device = XContentContainerDevice::CreateContentDevice("", path);
  REQUIRE(device);
  REQUIRE(device->Initialize());
  auto *entry = static_cast<Device &>(*device).ResolvePath("Assets\\payload");
  REQUIRE(entry);
  REQUIRE(entry->size() == 16);
  File *raw = nullptr;
  REQUIRE(entry->Open(xe::filesystem::FileAccess::kFileReadData, &raw) ==
          X_STATUS_SUCCESS);
  std::unique_ptr<File, void (*)(File *)> file(raw,
                                               [](File *f) { f->Destroy(); });
  std::array<uint8_t, 16> bytes{};
  size_t read = 0;
  REQUIRE(file->ReadSync(bytes, 0, &read) == X_STATUS_SUCCESS);
  REQUIRE(read == bytes.size());
  for (size_t i = 0; i < bytes.size(); ++i) {
    REQUIRE(bytes[i] == i);
  }
}

TEST_CASE(
    "STFS malformed directory metadata fails without an out-of-range access",
    "[stfs][container-bounds]") {
  StfsScratch scratch;
  StfsFixture fixture;
  SECTION("First entry references itself") {
    fixture.directory.entries[0].directory_index = 0;
  }
  SECTION("Parent index is outside prior entries") {
    fixture.directory.entries[0].directory_index = 0xFFFE;
  }
  SECTION("A file cannot be a parent directory") {
    SetFile(fixture.directory.entries[1], "child", 16, 0);
  }
  SECTION("Name length exceeds its physical field") {
    fixture.directory.entries[0].flags.name_length = 41;
  }
  SECTION("A nonempty slot cannot have a zero name length") {
    fixture.directory.entries[0].flags.name_length = 0;
  }
  const auto path = scratch.root / "malformed-directory";
  Write(path, fixture.Bytes());
  auto device = XContentContainerDevice::CreateContentDevice("", path);
  REQUIRE(device);
  REQUIRE_FALSE(device->Initialize());
  const auto metadata = ExtractStfsMetadata(path, XLanguage::kEnglish, true);
  REQUIRE(metadata);
  REQUIRE(metadata->title_resources_status == TitleResourcesStatus::kInvalid);
}

TEST_CASE("STFS writable hash-linked data keeps valid block reads",
          "[stfs][container-bounds]") {
  StfsScratch scratch;
  StfsFixture fixture(false);
  fixture.header->content_metadata.volume_descriptor.stfs.total_block_count = 3;
  SetFile(fixture.directory.entries[0], "payload", 0x1001);
  fixture.directory.entries[0].flags.contiguous = false;
  fixture.hash.entries[1].set_level0_next_block(2);
  fixture.hash.entries[2].set_level0_next_block(0xFFFFFF);
  auto bytes = fixture.Bytes();
  bytes.resize(bytes.size() + StfsFixture::kBlock);
  bytes[bytes.size() - StfsFixture::kBlock] = 0xA5;
  const auto path = scratch.root / "valid-writable";
  Write(path, bytes);
  auto device = XContentContainerDevice::CreateContentDevice("", path);
  REQUIRE(device);
  REQUIRE(device->Initialize());
  auto *entry = static_cast<Device &>(*device).ResolvePath("payload");
  REQUIRE(entry);
  File *raw = nullptr;
  REQUIRE(entry->Open(xe::filesystem::FileAccess::kFileReadData, &raw) ==
          X_STATUS_SUCCESS);
  std::unique_ptr<File, void (*)(File *)> file(raw,
                                               [](File *f) { f->Destroy(); });
  std::array<uint8_t, 2> output{};
  size_t read = 0;
  REQUIRE(file->ReadSync(output, 0xFFF, &read) == X_STATUS_SUCCESS);
  REQUIRE(read == output.size());
  REQUIRE((output == std::array<uint8_t, 2>{0, 0xA5}));
}

TEST_CASE("STFS malformed table chains report failure instead of asserting",
          "[stfs][container-bounds]") {
  StfsScratch scratch;
  StfsFixture fixture;
  auto &volume = fixture.header->content_metadata.volume_descriptor.stfs;
  SECTION("No file table") { volume.file_table_block_count = 0; }
  SECTION("File table count exceeds volume blocks") {
    volume.file_table_block_count = 3;
  }
  SECTION("File table starts beyond the volume") {
    volume.set_file_table_block_number(2);
  }
  SECTION("File table chain ends prematurely") {
    volume.file_table_block_count = 2;
  }
  SECTION("File table chain exceeds its declared count") {
    fixture.hash.entries[0].set_level0_next_block(1);
  }
  SECTION("Repeated table does not duplicate entries indefinitely") {
    volume.file_table_block_count = 2;
    fixture.hash.entries[0].set_level0_next_block(0);
  }
  const auto path = scratch.root / "malformed-table";
  Write(path, fixture.Bytes());
  auto device = XContentContainerDevice::CreateContentDevice("", path);
  REQUIRE(device);
  REQUIRE_FALSE(device->Initialize());
}

TEST_CASE("STFS malformed data blocks fail before publishing file extents",
          "[stfs][container-bounds]") {
  StfsScratch scratch;
  enum class Fault { kAllocation, kIndex, kExtent, kShortChain };
  Fault fault = Fault::kAllocation;
  SECTION("Allocated block count disagrees with length") {
    fault = Fault::kAllocation;
  }
  SECTION("Contiguous block number is outside the volume") {
    fault = Fault::kIndex;
  }
  SECTION("Contiguous bytes extend past the physical package") {
    fault = Fault::kExtent;
  }
  SECTION("A writable file chain ends before its data") {
    fault = Fault::kShortChain;
  }
  StfsFixture fixture(fault != Fault::kShortChain);
  auto &entry = fixture.directory.entries[0];
  switch (fault) {
  case Fault::kAllocation:
    entry.set_allocated_data_blocks(2);
    break;
  case Fault::kIndex:
    entry.set_start_block_number(2);
    break;
  case Fault::kExtent:
    break;
  case Fault::kShortChain:
    SetFile(entry, "payload", 0x1001);
    entry.flags.contiguous = false;
    break;
  }
  auto bytes = fixture.Bytes();
  if (fault == Fault::kExtent) {
    bytes.resize(bytes.size() - StfsFixture::kBlock + 8);
  }
  const auto path = scratch.root / "malformed-data";
  Write(path, bytes);
  auto device = XContentContainerDevice::CreateContentDevice("", path);
  REQUIRE(device);
  REQUIRE_FALSE(device->Initialize());
}

TEST_CASE("Detached STFS files retain the actual container and root tree",
          "[stfs][namespace][ownership]") {
  StfsScratch scratch;
  StfsFixture fixture;
  const auto path = scratch.root / "detached";
  Write(path, fixture.Bytes());
  VirtualFileSystem fs;
  auto device =
      XContentContainerDevice::CreateContentDevice("\\Device\\Detached", path);
  REQUIRE(device);
  REQUIRE(device->Initialize());
  REQUIRE(fs.RegisterDevice(std::move(device)));
  auto* registered = fs.GetDevice("\\Device\\Detached");
  REQUIRE(registered);
  std::weak_ptr<Device> lifetime = registered->weak_from_this();
  auto* entry = registered->ResolvePath("payload");
  auto* root = registered->ResolvePath("");
  REQUIRE(entry);
  REQUIRE(root);
  File* file = nullptr;
  File* root_file = nullptr;
  REQUIRE(entry->Open(xe::filesystem::FileAccess::kFileReadData, &file) ==
          X_STATUS_SUCCESS);
  REQUIRE(root->Open(xe::filesystem::FileAccess::kFileReadData, &root_file) ==
          X_STATUS_SUCCESS);
  REQUIRE(file->retained_device());
  REQUIRE(root_file->retained_device());
  SECTION("Unregister can detach a container with live file handles") {
    REQUIRE(fs.UnregisterDevice("\\Device\\Detached"));
  }
  SECTION(
      "Clear detaches the same container without destroying its live tree") {
    fs.Clear();
  }
  CHECK(fs.GetDevice("\\Device\\Detached") == nullptr);
  CHECK_FALSE(lifetime.expired());
  CHECK((root_file->entry()->attributes() & kFileAttributeDirectory) != 0);
  CHECK(root_file->entry()->absolute_path() == "\\Device\\Detached");
  std::array<uint8_t, 16> bytes{};
  size_t read = 0;
  REQUIRE(file->ReadSync(bytes, 0, &read) == X_STATUS_SUCCESS);
  REQUIRE(read == bytes.size());
  for (size_t i = 0; i < bytes.size(); ++i) {
    CHECK(bytes[i] == i);
  }
  file->Destroy();
  CHECK_FALSE(lifetime.expired());
  root_file->Destroy();
  CHECK(lifetime.expired());
}


TEST_CASE("STFS owned backing rejects extent overflow before provider reads",
          "[stfs][backing][container-bounds]") {
  StfsScratch scratch;
  StfsFixture fixture;
  const auto wire = fixture.Bytes();
  const auto path = scratch.root / "bounded";
  Write(path, wire);
  auto real = XContentReadBacking::Open(path);
  REQUIRE(real);
  REQUIRE(real->extent() == wire.size());
  auto state = std::make_shared<BackingReadState>();
  ObservedBacking backing(std::move(real), state);
  std::array<uint8_t, 2> output{0xA5, 0xA5};
  size_t transferred = 99;
  REQUIRE(backing.ReadWithStatus(wire.size(), output, &transferred) ==
          X_STATUS_INVALID_PARAMETER);
  REQUIRE(transferred == 0);
  REQUIRE_FALSE(backing.Read(wire.size(), output));
  REQUIRE_FALSE(backing.Read(wire.size() - 1, output));
  REQUIRE_FALSE(backing.Read(UINT64_MAX, output));
  REQUIRE(state->calls == 0);
  REQUIRE((output == std::array<uint8_t, 2>{0xA5, 0xA5}));
  REQUIRE(backing.Read(wire.size(), {}));
  REQUIRE(state->calls == 1);
  REQUIRE(backing.Read(0, output));
  REQUIRE(state->calls == 2);
  REQUIRE(output[0] == uint8_t('P'));
  REQUIRE(output[1] == uint8_t('I'));
}

TEST_CASE("STFS owned backing rejects short header directory and hash reads",
          "[stfs][backing][container-bounds]") {
  StfsScratch scratch;
  StfsFixture fixture;
  const auto path = scratch.root / "short";
  Write(path, fixture.Bytes());
  auto real = XContentReadBacking::Open(path);
  REQUIRE(real);
  auto state = std::make_shared<BackingReadState>();
  const uint64_t base = xe::round_up(sizeof(XContentContainerHeader),
                                    StfsFixture::kBlock);
  bool header = false;
  SECTION("Short header") { state->short_at = 0; header = true; }
  SECTION("Short directory block") {
    state->short_at = base + StfsFixture::kBlock;
  }
  SECTION("Short hash block") { state->short_at = base; }
  auto device = XContentContainerDevice::CreateContentDevice(
      "", std::make_unique<ObservedBacking>(std::move(real), state));
  REQUIRE(state->calls != 0);
  if (header) {
    REQUIRE_FALSE(device);
  } else {
    REQUIRE(device);
    REQUIRE_FALSE(device->Initialize());
  }
}

TEST_CASE("STFS host and owned backing use identical read-only and paired hashes",
          "[stfs][backing][container-bounds]") {
  StfsScratch scratch;
  const bool readonly_format = GENERATE(true, false);
  StfsFixture fixture(readonly_format);
  const auto path = scratch.root / "same-parser";
  Write(path, fixture.Bytes());
  std::unique_ptr<XContentContainerDevice> device;
  SECTION("Existing host path factory") {
    device = XContentContainerDevice::CreateContentDevice("", path);
    REQUIRE(device);
    REQUIRE(device->GetHostPath() == path);
  }
  SECTION("Already-open owned backing") {
    auto backing = XContentReadBacking::Open(path);
    REQUIRE(backing);
    device = XContentContainerDevice::CreateContentDevice("", std::move(backing));
  }
  REQUIRE(device);
  REQUIRE(device->Initialize());
  REQUIRE(device->title_id() == 0x584111F7);
  REQUIRE(device->GetContainerHeader()
              ->content_metadata.volume_descriptor.stfs.total_block_count == 2);
  auto* entry = static_cast<Device&>(*device).ResolvePath("payload");
  REQUIRE(entry);
  File* raw = nullptr;
  REQUIRE(entry->Open(FileAccess::kFileReadData, &raw) == X_STATUS_SUCCESS);
  std::unique_ptr<File, void (*)(File*)> file(raw,
                                             [](File* f) { f->Destroy(); });
  std::array<uint8_t, 16> bytes{};
  size_t read = 0;
  REQUIRE(file->ReadSync(bytes, 0, &read) == X_STATUS_SUCCESS);
  REQUIRE(read == bytes.size());
  for (size_t i = 0; i != bytes.size(); ++i) { REQUIRE(bytes[i] == i); }
  size_t written = 0;
  REQUIRE(file->WriteSync(bytes, 0, &written) == X_STATUS_ACCESS_DENIED);
  REQUIRE(file->SetLength(0) == X_STATUS_ACCESS_DENIED);
}

TEST_CASE("STFS file reads preserve backing status partial bytes and logical EOF",
          "[stfs][backing][container-bounds]") {
  StfsScratch scratch;
  StfsFixture fixture(false);
  fixture.header->content_metadata.volume_descriptor.stfs.total_block_count = 4;
  SetFile(fixture.directory.entries[0], "payload", 0x1004);
  fixture.directory.entries[0].flags.contiguous = false;
  fixture.hash.entries[1].set_level0_next_block(3);
  fixture.hash.entries[3].set_level0_next_block(0xFFFFFF);
  const size_t base = xe::round_up(sizeof(XContentContainerHeader),
                                 StfsFixture::kBlock);
  const size_t first = base + 3 * StfsFixture::kBlock;
  const size_t second = base + 5 * StfsFixture::kBlock;
  auto wire = fixture.Bytes();
  wire.resize(base + 6 * StfsFixture::kBlock);
  for (size_t i = 0; i < StfsFixture::kBlock; ++i) {
    wire[first + i] = uint8_t(i);
  }
  const std::array<uint8_t, 4> tail{0xA1, 0xA2, 0xA3, 0xA4};
  std::copy(tail.begin(), tail.end(), wire.begin() + second);
  const auto path = scratch.root / "partial-records";
  Write(path, wire);
  auto real = XContentReadBacking::Open(path);
  REQUIRE(real);
  auto state = std::make_shared<BackingReadState>();
  auto device = XContentContainerDevice::CreateContentDevice(
      "", std::make_unique<ObservedBacking>(std::move(real), state));
  REQUIRE(device);
  REQUIRE(device->Initialize());
  auto* entry = static_cast<Device&>(*device).ResolvePath("payload");
  REQUIRE(entry);
  REQUIRE(static_cast<XContentContainerEntry*>(entry)->block_list().size() == 2);
  File* raw = nullptr;
  REQUIRE(entry->Open(FileAccess::kFileReadData, &raw) == X_STATUS_SUCCESS);
  std::unique_ptr<File, void (*)(File*)> file(raw,
                                             [](File* f) { f->Destroy(); });
  const std::array<uint8_t, 8> expected{0xFC, 0xFD, 0xFE, 0xFF,
                                       0xA1, 0xA2, 0xA3, 0xA4};
  std::array<uint8_t, 8> output{};
  size_t transferred = 0;
  REQUIRE(file->ReadSync(output, 0xFFC, &transferred) == X_STATUS_SUCCESS);
  REQUIRE(transferred == output.size());
  REQUIRE(output == expected);

  X_STATUS expected_status = X_STATUS_UNSUCCESSFUL;
  size_t expected_count = 7;
  SECTION("Unexpected physical short after an earlier record") {
    state->short_at = second;
  }
  SECTION("Provider failure after a copied prefix") {
    state->error_at = second;
    state->error_prefix = 2;
    state->error_status = X_STATUS_ACCESS_DENIED;
    expected_status = X_STATUS_ACCESS_DENIED;
    expected_count = 6;
  }
  SECTION("Provider failure before the second record") {
    state->error_at = second;
    state->error_status = X_STATUS(0xC000009D);
    expected_status = X_STATUS(0xC000009D);
    expected_count = 4;
  }
  output.fill(0xCC);
  transferred = 99;
  REQUIRE(file->ReadSync(output, 0xFFC, &transferred) == expected_status);
  REQUIRE(transferred == expected_count);
  REQUIRE(std::equal(output.begin(), output.begin() + expected_count,
                     expected.begin()));
  REQUIRE(std::all_of(output.begin() + expected_count, output.end(),
                      [](uint8_t b) { return b == 0xCC; }));

  // The same open child recovers when this test-only read fault is removed;
  // it is not remounted, manually refreshed or replaced by another source.
  state->short_at.reset();
  state->error_at.reset();
  REQUIRE(file->ReadSync(output, 0xFFC, &transferred) == X_STATUS_SUCCESS);
  REQUIRE(transferred == output.size());
  REQUIRE(output == expected);
  const auto calls = state->calls;
  transferred = 99;
  REQUIRE(file->ReadSync(output, entry->size(), &transferred) ==
          X_STATUS_END_OF_FILE);
  REQUIRE(transferred == 0);
  REQUIRE(state->calls == calls);
  REQUIRE(file->ReadSync(output, entry->size() + 1, &transferred) ==
          X_STATUS_END_OF_FILE);
  REQUIRE(transferred == 0);
  REQUIRE(state->calls == calls);
  REQUIRE(file->ReadSync({}, 0, &transferred) == X_STATUS_SUCCESS);
  REQUIRE(transferred == 0);
  REQUIRE(state->calls == calls);
  output.fill(0xCC);
  REQUIRE(file->ReadSync(output, entry->size() - 2, &transferred) ==
          X_STATUS_SUCCESS);
  REQUIRE(transferred == 2);
  REQUIRE(output[0] == 0xA3);
  REQUIRE(output[1] == 0xA4);
  REQUIRE(std::all_of(output.begin() + 2, output.end(),
                      [](uint8_t b) { return b == 0xCC; }));
}

#if XE_PLATFORM_LINUX || XE_PLATFORM_MAC
TEST_CASE("STFS owned handle avoids reopen and survives detached child reads",
          "[stfs][backing][namespace][ownership]") {
  StfsScratch scratch;
  StfsFixture fixture;
  const auto path = scratch.root / "original";
  Write(path, fixture.Bytes());
  std::error_code error;
  auto source = filesystem::HostStorageRoot::Open(scratch.root, true, error);
  REQUIRE(source);
  REQUIRE_FALSE(error);
  std::weak_ptr<filesystem::HostStorageRoot> lifetime = source;
  auto backing = XContentReadBacking::FromFileHandle(
      source->OpenFile("original", FileAccess::kFileReadData, error),
      std::vector<filesystem::GuestHostBacking>{source->GuestBacking()});
  REQUIRE(backing);
  REQUIRE_FALSE(error);
  SECTION("Same owned source") {}
  SECTION("Host pathname replacement does not replace the opened object") {
    std::filesystem::rename(path, scratch.root / "retained-original");
    auto replacement = fixture.Bytes();
    replacement[replacement.size() - StfsFixture::kBlock] = 0xEE;
    Write(path, replacement);
  }
  source.reset();
  REQUIRE_FALSE(lifetime.expired());
  VirtualFileSystem fs;
  auto device = XContentContainerDevice::CreateContentDevice(
      "\\Device\\OwnedStfs", std::move(backing));
  REQUIRE(device);
  REQUIRE(device->Initialize());
  REQUIRE(fs.RegisterDevice(std::move(device)));
  auto* entry = fs.ResolvePath("\\Device\\OwnedStfs\\payload");
  REQUIRE(entry);
  File* file = nullptr;
  REQUIRE(entry->Open(FileAccess::kFileReadData, &file) == X_STATUS_SUCCESS);
  REQUIRE(fs.UnregisterDevice("\\Device\\OwnedStfs"));
  REQUIRE_FALSE(lifetime.expired());
  std::array<uint8_t, 16> bytes{};
  size_t read = 0;
  REQUIRE(file->ReadSync(bytes, 0, &read) == X_STATUS_SUCCESS);
  REQUIRE(read == bytes.size());
  for (size_t i = 0; i != bytes.size(); ++i) { REQUIRE(bytes[i] == i); }
  file->Destroy();
  REQUIRE(lifetime.expired());
}

TEST_CASE("STFS late short or changed sources never invent payload bytes",
          "[stfs][backing][ownership]") {
  StfsScratch scratch;
  StfsFixture fixture;
  Write(scratch.root / "source", fixture.Bytes());
  std::error_code error;
  auto source = filesystem::HostStorageRoot::Open(scratch.root, true, error);
  REQUIRE(source);
  REQUIRE_FALSE(error);
  auto backing = XContentReadBacking::FromFileHandle(
      source->OpenFile("source", FileAccess::kFileReadData, error),
      std::vector<filesystem::GuestHostBacking>{source->GuestBacking()});
  REQUIRE(backing);
  REQUIRE_FALSE(backing->supports_concurrent_io());
  auto state = std::make_shared<BackingReadState>();
  auto device = XContentContainerDevice::CreateContentDevice(
      "", std::make_unique<ObservedBacking>(std::move(backing), state));
  REQUIRE(device);
  REQUIRE(device->Initialize());
  auto* entry = static_cast<Device&>(*device).ResolvePath("payload");
  REQUIRE(entry);
  File* raw = nullptr;
  REQUIRE(entry->Open(FileAccess::kFileReadData, &raw) == X_STATUS_SUCCESS);
  std::unique_ptr<File, void (*)(File*)> file(raw,
                                             [](File* f) { f->Destroy(); });
  const size_t payload = xe::round_up(sizeof(XContentContainerHeader),
                                     StfsFixture::kBlock) +
                         2 * StfsFixture::kBlock;
  bool short_read = false;
  X_STATUS expected = X_STATUS_UNSUCCESSFUL;
  SECTION("Retired source") {
    source->Retire();
    expected = X_STATUS(0xC000009D);  // Existing no_such_device mapping.
  }
  SECTION("Removed source namespace") {
    std::filesystem::rename(scratch.root, scratch.root.string() + "-removed");
    // Keep the bytes under the scratch owner's teardown path, not under the
    // old retained capability spelling. No file is removed by this control.
    scratch.root = scratch.root.string() + "-removed";
    expected = X_STATUS(0xC000009D);
  }
  SECTION("Shrunk backing") {
    std::filesystem::resize_file(scratch.root / "source", payload + 8);
  }
  SECTION("Grown backing") {
    std::filesystem::resize_file(scratch.root / "source",
                                  fixture.Bytes().size() + 1);
  }
  SECTION("Short payload after initialization") {
    state->short_at = payload;
    short_read = true;
  }
  std::array<uint8_t, 16> output;
  output.fill(0xA5);
  size_t transferred = 99;
  REQUIRE(file->ReadSync(output, 0, &transferred) == expected);
  REQUIRE(transferred == (short_read ? 15 : 0));
  REQUIRE(output.back() == 0xA5);
  if (short_read) {
    REQUIRE(output.front() == 0);
    REQUIRE(output[14] == 14);
  } else {
    REQUIRE(std::all_of(output.begin(), output.end(),
                        [](uint8_t b) { return b == 0xA5; }));
  }
}

TEST_CASE("STFS already-open VFS backing retains its retiring parent device",
          "[stfs][backing][namespace][ownership]") {
  StfsScratch scratch;
  StfsFixture fixture;
  Write(scratch.root / "source", fixture.Bytes());
  VirtualFileSystem parents;
  auto parent = std::make_unique<HostPathDevice>("Backing:", scratch.root, true);
  REQUIRE(parent->Initialize());
  REQUIRE(parents.RegisterDevice(std::move(parent)));
  auto held_parent = parents.AcquireDevice("Backing:");
  std::weak_ptr<Device> lifetime = held_parent;
  auto* package = parents.ResolvePath("Backing:\\source");
  REQUIRE(package);
  auto child = XContentContainerDevice::CreateContentDevice("Child:", package);
  REQUIRE(child);
  REQUIRE(child->package_entry() == package);
  REQUIRE(child->Initialize());
  VirtualFileSystem children;
  REQUIRE(children.RegisterDevice(std::move(child)));
  auto* entry = children.ResolvePath("Child:\\payload");
  REQUIRE(entry);
  File* file = nullptr;
  REQUIRE(entry->Open(FileAccess::kFileReadData, &file) == X_STATUS_SUCCESS);
  held_parent.reset();
  parents.Clear();
  children.Clear();
  REQUIRE_FALSE(lifetime.expired());
  std::array<uint8_t, 16> output;
  output.fill(0xA5);
  size_t read = 0;
  REQUIRE(file->ReadSync(output, 0, &read) == X_STATUS(0xC000009D));
  REQUIRE(read == 0);
  REQUIRE(std::all_of(output.begin(), output.end(),
                      [](uint8_t b) { return b == 0xA5; }));
  file->Destroy();
  REQUIRE(lifetime.expired());
}


TEST_CASE("STFS read-only admission preserves rejected mutable file ownership",
          "[stfs][backing][ownership]") {
  StfsScratch scratch;
  StfsFixture fixture;
  const auto wire = fixture.Bytes();
  const auto path = scratch.root / "source";
  Write(path, wire);
  VirtualFileSystem fs;
  auto parent = std::make_unique<HostPathDevice>("Backing:", scratch.root, false);
  REQUIRE(parent->Initialize());
  REQUIRE(fs.RegisterDevice(std::move(parent)));

  // A normal read on a writable source remains useful; no blanket readonly
  // source policy is used to make the rejection controls pass.
  File* reader = nullptr;
  FileAction action;
  REQUIRE(fs.OpenFile(nullptr, "Backing:\\source", FileDisposition::kOpen,
                      FileAccess::kFileReadData, false, true, &reader,
                      &action) == X_STATUS_SUCCESS);
  auto safe = XContentReadBacking::FromFile(reader);
  REQUIRE(safe);
  REQUIRE(reader == nullptr);
  std::array<uint8_t, 4> magic{};
  REQUIRE(safe->Read(0, magic));
  REQUIRE((magic == std::array<uint8_t, 4>{'P', 'I', 'R', 'S'}));
  safe.reset();

  enum class Mode { kReadWrite, kReadAppend, kGenericAll,
                    kReadOverwrite, kPendingEntry, kDeleteOnClose };
  const auto mode = GENERATE(Mode::kReadWrite, Mode::kReadAppend,
                             Mode::kGenericAll, Mode::kReadOverwrite,
                             Mode::kPendingEntry, Mode::kDeleteOnClose);
  uint32_t access = FileAccess::kFileReadData;
  if (mode == Mode::kReadWrite) { access |= FileAccess::kFileWriteData; }
  if (mode == Mode::kReadAppend) { access |= FileAccess::kFileAppendData; }
  if (mode == Mode::kGenericAll) { access = FileAccess::kGenericAll; }
  if (mode == Mode::kPendingEntry) {
    auto* pending = fs.ResolvePath("Backing:\\source");
    REQUIRE(pending);
    const auto reserved = pending->OverwriteInPlace();
    REQUIRE(reserved.supported);
    REQUIRE(reserved.status == X_STATUS_SUCCESS);
    REQUIRE_FALSE(XContentContainerDevice::CreateContentDevice("", pending));
    REQUIRE(std::filesystem::file_size(path) == wire.size());
  }
  File* file = nullptr;
  REQUIRE(fs.OpenFile(nullptr, "Backing:\\source",
                      mode == Mode::kReadOverwrite
                          ? FileDisposition::kOverwrite
                          : FileDisposition::kOpen,
                      access, false, true, &file, &action) == X_STATUS_SUCCESS);
  REQUIRE(file);
  if (mode == Mode::kDeleteOnClose) { file->entry()->SetForDeletion(true); }
  if (mode == Mode::kReadOverwrite || mode == Mode::kPendingEntry) {
    REQUIRE(file->file_access() == FileAccess::kFileReadData);
    REQUIRE(file->entry()->size() == 0);
    REQUIRE_FALSE(XContentContainerDevice::CreateContentDevice("", file->entry()));
  }
  File* const original = file;
  auto rejected = XContentReadBacking::FromFile(file);
  REQUIRE_FALSE(rejected);
  REQUIRE(file == original);
  REQUIRE(std::filesystem::file_size(path) == wire.size());
  {
    std::ifstream physical(path, std::ios::binary);
    const std::vector<uint8_t> actual{std::istreambuf_iterator<char>(physical),
                                      std::istreambuf_iterator<char>()};
    REQUIRE(actual == wire);
  }

  // Only the original owner closes it. The real pending overwrite/deletion
  // then commits, proving rejection neither closed nor prematurely repaired it.
  file->Destroy();
  file = nullptr;
  if (mode == Mode::kReadOverwrite || mode == Mode::kPendingEntry) {
    REQUIRE(std::filesystem::file_size(path) == 0);
  } else if (mode == Mode::kDeleteOnClose) {
    REQUIRE_FALSE(std::filesystem::exists(path));
  } else {
    REQUIRE(std::filesystem::file_size(path) == wire.size());
  }
}
#endif  // XE_PLATFORM_LINUX || XE_PLATFORM_MAC


TEST_CASE("STFS shared backing borrows owner and captures real opened extent",
          "[stfs][backing][ownership]") {
  StfsScratch scratch;
  StfsFixture fixture;
  auto bytes = fixture.Bytes();
  Write(scratch.root / "package", bytes);
  VirtualFileSystem fs;
  auto source = std::make_unique<HostPathDevice>(
      "\\Device\\SharedPackageSource", scratch.root, true);
  REQUIRE(source->Initialize()); REQUIRE(fs.RegisterDevice(std::move(source)));
  auto* entry = fs.ResolvePath("\\Device\\SharedPackageSource\\package");
  REQUIRE(entry);
  const auto cached_size = entry->size();
  // Capture actual opened EOF after a genuine host grow, not cached Entry size.
  bytes.resize(bytes.size() + 0x1000, 0xCC);
  Write(scratch.root / "package", bytes);
  File* raw = nullptr;
  REQUIRE(entry->Open(FileAccess::kFileReadData, &raw) == X_STATUS_SUCCESS);
  auto owner = std::shared_ptr<File>(raw, [](File* file) { file->Destroy(); });
  std::weak_ptr<File> lifetime = owner;
  auto backing = XContentReadBacking::FromSharedFile(owner);
  REQUIRE(backing);
  CHECK(backing->extent() == bytes.size());
  CHECK(backing->extent() > cached_size);
  auto alias = owner;
  owner.reset();
  REQUIRE_FALSE(lifetime.expired());
  auto device = XContentContainerDevice::CreateContentDevice(
      "\\Device\\SharedPackage", std::move(backing));
  REQUIRE(device); REQUIRE(device->Initialize());
  REQUIRE(fs.RegisterDevice(std::move(device)));
  auto* member = fs.ResolvePath("\\Device\\SharedPackage\\payload");
  REQUIRE(member);
  File* child = nullptr;
  REQUIRE(member->Open(FileAccess::kFileReadData, &child) == X_STATUS_SUCCESS);
  alias.reset();
  REQUIRE(fs.UnregisterDevice("\\Device\\SharedPackage"));
  REQUIRE_FALSE(lifetime.expired());
  std::array<uint8_t, 16> out{};
  size_t read = 0;
  REQUIRE(child->ReadSync(out, 0, &read) == X_STATUS_SUCCESS);
  REQUIRE(read == out.size());
  for (size_t i = 0; i != out.size(); ++i) { CHECK(out[i] == i); }
  child->Destroy();
  CHECK(lifetime.expired());
}

TEST_CASE("STFS shared backing refusal leaves mutable ownership untouched",
          "[stfs][backing][ownership]") {
  StfsScratch scratch;
  StfsFixture fixture;
  const auto bytes = fixture.Bytes();
  Write(scratch.root / "package", bytes);
  VirtualFileSystem fs;
  auto source = std::make_unique<HostPathDevice>(
      "\\Device\\SharedMutable", scratch.root, false);
  REQUIRE(source->Initialize()); REQUIRE(fs.RegisterDevice(std::move(source)));
  auto* entry = fs.ResolvePath("\\Device\\SharedMutable\\package");
  REQUIRE(entry);
  File* raw = nullptr;
  REQUIRE(entry->Open(FileAccess::kFileReadData | FileAccess::kFileWriteData,
                       &raw) == X_STATUS_SUCCESS);
  auto owner = std::shared_ptr<File>(raw, [](File* file) { file->Destroy(); });
  const auto before = owner.use_count();
  CHECK_FALSE(XContentReadBacking::FromSharedFile(owner));
  CHECK(owner.get() == raw);
  CHECK(owner.use_count() == before);
  std::array<uint8_t, 4> out{};
  size_t read = 0;
  REQUIRE(raw->ReadSync(out, 0, &read) == X_STATUS_SUCCESS);
  CHECK((out == std::array<uint8_t, 4>{'P', 'I', 'R', 'S'}));
  CHECK(read == out.size());
  owner.reset();
  std::ifstream input(scratch.root / "package", std::ios::binary);
  std::vector<uint8_t> actual((std::istreambuf_iterator<char>(input)), {});
  CHECK(actual == bytes);
}

} // namespace xe::vfs::test
