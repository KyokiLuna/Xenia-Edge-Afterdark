/**
 * Xenia : Xbox 360 Emulator Research Project
 * Released under the BSD license - see LICENSE in the root for details.
 */
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <initializer_list>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "third_party/catch/include/catch.hpp"
#include "xenia/base/byte_stream.h"
#include "xenia/base/cvar.h"
#include "xenia/base/math.h"
#include "xenia/emulator.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/xam/app_manager.h"
#include "xenia/kernel/xam/content_manager.h"
#include "xenia/kernel/user_module.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xfile.h"
#include "xenia/kernel/xstfs_device.h"
#include "xenia/kernel/xam/native_folder_content.h"
#include "xenia/base/snapshot_publication_set.h"
#include "xenia/vfs/devices/host_path_device.h"
#include "xenia/vfs/devices/stfs_xbox.h"
#include "xenia/vfs/devices/xcontent_devices/stfs_container_device.h"

DECLARE_string(cpu);
DECLARE_bool(debug);
DECLARE_string(system_readonly_source_root);
DECLARE_string(logged_profile_slot_0_xuid);
DECLARE_string(logged_profile_slot_1_xuid);
DECLARE_string(logged_profile_slot_2_xuid);
DECLARE_string(logged_profile_slot_3_xuid);

namespace xe::kernel::xam {
class NativeFolderContentTestAccess final {
 public:
  using Session = filesystem::ContentMutationOwner::ProfileHeaderPublication;
  static std::shared_ptr<NativeFolderContent> Prepare(const ContentSourceState& source,
      std::filesystem::path package,
      std::shared_ptr<const filesystem::HostStorageSnapshotToken> actual,
      const ConsoleSigningIdentity::Certificate& certificate,
      folder_content_detail::PackageIdentity expected, std::error_code& error) {
    return NativeFolderContent::Prepare({source.storage, source.volume,
        source.storage_volume, certificate}, std::move(package), std::move(actual), expected, error);
  }
  static std::shared_ptr<Session> Issue(filesystem::ContentMutationOwner& cm,
      const std::filesystem::path& package,
      std::shared_ptr<const filesystem::HostStorageSnapshotToken> header,
      const std::shared_ptr<filesystem::SnapshotPublicationSet>& members,
      std::error_code& error) { return cm.PrepareProfileHeader(package, std::move(header), members, error); }
  static auto Seal(Session& session, std::error_code& error) { return session.Seal(error); }
  static auto PrepareCommit(Session& session,
      const filesystem::SnapshotPublicationSet& members, std::error_code& error) {
    return session.PrepareCanonical(members, error);
  }
  static auto Commit(Session& session, const filesystem::SnapshotPublicationSet& members) {
    return session.CommitCanonical(members);
  }
  static auto CaptureHeader(vfs::FolderContentPackage& file,
      std::span<const uint8_t> actual_before, std::error_code& error) {
    return file.CaptureHeaderBefore(actual_before, error);
  }
  static X_STATUS Adopt(vfs::FolderContentPackage& file, std::shared_ptr<Session> session,
      std::span<const uint8_t> owned_native_before) {
    return file.AdoptHeaderPublication(std::move(session), owned_native_before);
  }
  static X_STATUS Promote(vfs::FolderContentPackage& file, const std::shared_ptr<Session>& session,
      std::shared_ptr<const filesystem::HostStorageSnapshotToken> current) {
    return file.PromoteCommittedHeader(session, std::move(current));
  }
};
}

namespace xe::kernel {
class NativeStfsDeviceTestAccess final {
 public:
  static X_STATUS WithdrawBeforeCommit(KernelState& kernel, uint32_t packet,
      std::unique_ptr<vfs::Device>* replacement = nullptr) {
    return XStfsDevice::CreateInternal(kernel, packet, 0x58, true, replacement);
  }
};
}

namespace xe::kernel::test {
namespace {
struct Settings {
  std::string cpu = cvars::cpu;
  bool debug = cvars::debug;
  std::array<std::string*, 5> fields{
      &cvars::system_readonly_source_root, &cvars::logged_profile_slot_0_xuid,
      &cvars::logged_profile_slot_1_xuid, &cvars::logged_profile_slot_2_xuid,
      &cvars::logged_profile_slot_3_xuid};
  std::array<std::string, 5> saved;
  Settings() {
    cvars::cpu = "null";
    cvars::debug = false;
    for (size_t i = 0; i != fields.size(); ++i) {
      saved[i] = *fields[i]; fields[i]->clear();
    }
  }
  ~Settings() {
    cvars::cpu.swap(cpu); cvars::debug = debug;
    for (size_t i = 0; i != fields.size(); ++i) { fields[i]->swap(saved[i]); }
  }
};
struct Scratch {
  std::filesystem::path path;
  Scratch() {
    static std::atomic<uint64_t> sequence{0};
    path = std::filesystem::temp_directory_path() /
        ("xenia-native-pirs-" +
         std::to_string(std::chrono::steady_clock::now()
                            .time_since_epoch().count()) + "-" +
         std::to_string(sequence.fetch_add(1)));
    REQUIRE(std::filesystem::create_directory(path));
  }
  ~Scratch() {
    std::error_code error; std::filesystem::remove_all(path, error);
  }
};

// Synthetic structural package, not copyrighted asset bytes or a claimed RSA
// signature. These tests invoke the real Kernel ABI below XAM's verifier.
std::vector<uint8_t> Package(bool single_hash) {
  using namespace vfs;
  auto h = std::make_unique<XContentContainerHeader>();
  h->content_header.magic = XContentPackageType::kPirs;
  h->content_header.header_size = sizeof(*h);
  h->content_metadata.volume_type = XContentVolumeType::kStfs;
  h->content_metadata.content_size = 16;
  auto& descriptor = h->content_metadata.volume_descriptor.stfs;
  descriptor.descriptor_length = sizeof(descriptor);
  descriptor.flags.bits.read_only_format = single_hash;
  descriptor.file_table_block_count = 1;
  descriptor.set_file_table_block_number(0);
  descriptor.total_block_count = 2;
  StfsHashTable hash{};
  hash.entries[0].set_level0_next_block(0xFFFFFF);
  hash.entries[1].set_level0_next_block(0xFFFFFF);
  StfsDirectoryBlock directory{};
  auto& file = directory.entries[0];
  std::memcpy(file.name, "payload", 7);
  file.flags.name_length = 7; file.flags.contiguous = true;
  file.set_valid_data_blocks(1); file.set_allocated_data_blocks(1);
  file.set_start_block_number(1); file.directory_index = 0xFFFF;
  file.length = 16;
  const size_t base = xe::round_up(sizeof(*h), 0x1000);
  const size_t hashes = single_hash ? 1 : 2;
  std::vector<uint8_t> bytes(base + (hashes + 2) * 0x1000);
  std::memcpy(bytes.data(), h.get(), sizeof(*h));
  std::memcpy(bytes.data() + base, &hash, sizeof(hash));
  std::memcpy(bytes.data() + base + hashes * 0x1000, &directory,
              sizeof(directory));
  for (size_t i = 0; i != 16; ++i) {
    bytes[base + (hashes + 1) * 0x1000 + i] = uint8_t(i);
  }
  return bytes;
}

class Fixture {
 public:
  Fixture(bool readonly = true, bool single_hash = true,
          bool nested_package = false,
          filesystem::ConsoleStorageType source_type = filesystem::ConsoleStorageType::Invalid,
          bool content_subroot = false, bool managed_folder = false,
          bool native_content_edge = false) {
    REQUIRE(KernelState::shared() == nullptr);
    for (const auto& name : {"content", "cache", "packages"}) {
      REQUIRE(std::filesystem::create_directory(scratch.path / name));
    }
    bytes = Package(single_hash);
    if (nested_package) {
      const auto inner = Package(true);
      const uint32_t blocks = uint32_t(inner.size() / 0x1000);
      const size_t base = xe::round_up(sizeof(vfs::XContentContainerHeader),
                                     0x1000);
      const size_t hashes = single_hash ? 1 : 2;
      auto* header = reinterpret_cast<vfs::XContentContainerHeader*>(bytes.data());
      header->content_metadata.content_size = inner.size();
      header->content_metadata.volume_descriptor.stfs.total_block_count =
          blocks + 1;
      auto* directory = reinterpret_cast<vfs::StfsDirectoryBlock*>(
          bytes.data() + base + hashes * 0x1000);
      auto& member = directory->entries[0];
      member.length = uint32_t(inner.size());
      member.set_valid_data_blocks(blocks);
      member.set_allocated_data_blocks(blocks);
      bytes.resize(base + (hashes + 1 + blocks) * 0x1000);
      std::copy(inner.begin(), inner.end(),
                bytes.begin() + base + (hashes + 1) * 0x1000);
    }
    {
      auto input = scratch.path / "packages";
      if (content_subroot) { REQUIRE(std::filesystem::create_directory(input / "Content")); input /= "Content"; }
      source_path = content_subroot ? "\\Device\\PirsSource\\Content\\package"
                                    : "\\Device\\PirsSource\\package";
      std::ofstream out(input / "package", std::ios::binary);
      out.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
      REQUIRE(out.good());
    }
    if (managed_folder) {
      // HostPathDevice captures its tree at Initialize. Seed the actual
      // directory/files before source registration/mounting, not afterward.
      const auto root = scratch.path / "packages" / "managed";
      REQUIRE(std::filesystem::create_directory(root));
      {
        std::ofstream out(root / "package", std::ios::binary);
        out.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        REQUIRE(out.good());
      }
      {
        const std::array<uint8_t, 4> seed{0x10, 0x20, 0x30, 0x40};
        std::ofstream out(root / "value.bin", std::ios::binary);
        out.write(reinterpret_cast<const char*>(seed.data()), seed.size());
        REQUIRE(out.good());
      }
    }
    emulator = std::make_unique<Emulator>("", scratch.path,
        scratch.path / "content", scratch.path / "cache");
    REQUIRE(emulator->Setup(nullptr, nullptr, false, {}, {}, {}) ==
            X_STATUS_SUCCESS);
    std::shared_ptr<filesystem::HostStorageRoot> storage;
    std::shared_ptr<const filesystem::ConsoleStorageVolume> volume;
    if (source_type == filesystem::ConsoleStorageType::HDD || source_type == filesystem::ConsoleStorageType::USBMASS) {
      const auto type = source_type == filesystem::ConsoleStorageType::HDD
          ? xam::DeviceType::HDD : xam::DeviceType::USBMASS;
      REQUIRE(kernel().content_manager()->RegisterManagedSource(
          {"native-wire", 3, type, xam::ContentSourceKind::kManaged, scratch.path / "packages",
           native_content_edge ? "\\Device\\PirsVolume\\Content" : "\\Device\\PirsSource",
           u"Native wire", false, 0, "\\Device\\PirsVolume", {}, scratch.path}));
      const auto selected = kernel().content_manager()->sources().Source("native-wire");
      REQUIRE(selected); REQUIRE(selected->available); REQUIRE(selected->storage_volume);
      std::error_code error; storage = selected->storage->Subroot({}, readonly, error);
      REQUIRE(storage); REQUIRE_FALSE(error); volume = selected->storage_volume;
    } else if (source_type == filesystem::ConsoleStorageType::ODD) {
      volume = filesystem::ConsoleStorageVolume::OpticalMedia();
    } else if (source_type == filesystem::ConsoleStorageType::System) {
      volume = filesystem::ConsoleStorageVolume::InstalledSystem();
    }
    auto source = std::make_unique<vfs::HostPathDevice>(
        "\\Device\\PirsSource", scratch.path / "packages", readonly, false, "FATX", storage);
    if (volume) { REQUIRE(source->SetStorageVolume(volume)); }
    REQUIRE(source->Initialize());
    REQUIRE(fs().RegisterDevice(std::move(source)));
    auto* entry = fs().ResolvePath(source_path);
    REQUIRE(entry);
    vfs::File* raw = nullptr;
    uint32_t access = filesystem::FileAccess::kFileReadData;
    if (!readonly) { access |= filesystem::FileAccess::kFileWriteData; }
    REQUIRE(entry->Open(access, &raw) == X_STATUS_SUCCESS);
    backing = object_ref<XFile>(new XFile(&kernel(), raw, true, false, true));
    backing_handle = backing->handle();
    REQUIRE(raw->storage_lease());
    source_lease = raw->storage_lease();
    allocation = memory().SystemHeapAlloc(0x10000, 0x1000);
    REQUIRE(allocation);
    std::memset(memory().TranslateVirtual(allocation), 0xA5, 0x10000);
    std::memset(memory().TranslateVirtual(packet()), 0, 0x58);
    const std::string name = "\\Device\\NativePirs0";
    std::memcpy(memory().TranslateVirtual(allocation + 0x200),
                name.data(), name.size());
    Store16(0, uint16_t(name.size()));
    Store16(2, uint16_t(name.size()));
    Store32(4, allocation + 0x200);
    Store64(8, xe::round_up(sizeof(vfs::XContentContainerHeader), 0x1000));
    Store64(0x10, 0x100000);
    Store32(0x18, backing_handle);
    auto* header = reinterpret_cast<const vfs::XContentContainerHeader*>(
        bytes.data());
    std::memcpy(memory().TranslateVirtual(packet() + 0x1C),
        &header->content_metadata.volume_descriptor.stfs, 36);
    Store32(0x40, allocation + 0x1000);
    Store32(0x44, uint32_t(xe::round_up(sizeof(*header), 0x1000) + 0x192));
    Store32(0x48, 0xA1A2A3A4); Store32(0x4C, 0xB1B2B3B4);
    auto* p = memory().TranslateVirtual(packet());
    p[0x50] = 5; p[0x51] = 0; p[0x52] = 1; p[0x53] = 4;
  }
  ~Fixture() {
    backing.reset();
    // Genuine Emulator teardown retires all registered bodies before Memory.
    // No paused-state injection, dummy Kernel or manual private owner cleanup.
    emulator.reset();
  }
  KernelState& kernel() { return *emulator->kernel_state(); }
  Memory& memory() { return *emulator->memory(); }
  vfs::VirtualFileSystem& fs() { return *emulator->file_system(); }
  uint32_t packet() const { return allocation; }
  void Store16(uint32_t at, uint16_t value) {
    xe::store_and_swap<uint16_t>(memory().TranslateVirtual(packet() + at), value);
  }
  void Store32(uint32_t at, uint32_t value) {
    xe::store_and_swap<uint32_t>(memory().TranslateVirtual(packet() + at), value);
  }
  void Store64(uint32_t at, uint64_t value) {
    xe::store_and_swap<uint64_t>(memory().TranslateVirtual(packet() + at), value);
  }
  uint32_t Load32(uint32_t at) {
    return xe::load_and_swap<uint32_t>(memory().TranslateVirtual(packet() + at));
  }
  uint32_t Call(uint16_t ordinal, std::initializer_list<uint32_t> args) {
    auto* entry = emulator->export_resolver()->GetExportByOrdinal("xboxkrnl.exe",
                                                                ordinal);
    REQUIRE(entry); REQUIRE(entry->function_data.trampoline);
    PPCContext context{};
    context.kernel_state = &kernel();
    context.virtual_membase = memory().virtual_membase();
    std::copy(args.begin(), args.end(), context.r + 3);
    entry->function_data.trampoline(&context);
    return uint32_t(context.r[3]);
  }
  uint32_t Create() { return Call(0x259, {packet(), 0x58}); }
  void CloseBacking() {
    REQUIRE(kernel().object_table()->ReleaseHandle(backing_handle) ==
            X_STATUS_SUCCESS);
    backing.reset();
  }
  std::array<uint8_t, sizeof(X_OBJECT_TYPE)> TypeBytes() {
    std::array<uint8_t, sizeof(X_OBJECT_TYPE)> result{};
    std::memcpy(result.data(), memory().TranslateVirtual(
        kernel().GetKernelGuestGlobals() +
        offsetof(KernelGuestGlobals, IoDeviceObjectType)), result.size());
    return result;
  }
  Settings settings;
  Scratch scratch;
  std::unique_ptr<Emulator> emulator;
  std::vector<uint8_t> bytes;
  object_ref<XFile> backing;
  X_HANDLE backing_handle = 0;
  uint32_t allocation = 0;
  std::weak_ptr<const filesystem::HostStorageFileLease> source_lease;
  std::string source_path;
};

// Wrap the actual readonly fixture file. Failures keep its real bytes,
// parent/device/source lease and Destroy lifetime; no successful read or mount
// is synthesized. This exercises ContentManager's ordinary borrowed VFS lane.
struct HeaderReadFault {
  X_STATUS status = X_STATUS_SUCCESS;
  bool short_read = false;
  size_t reads = 0, opened = 0, closed = 0;
  size_t registration_backings = 0;
  bool invalid_registration_backing = false;
  std::function<void()> before_registration_backings;
};
class HeaderReadFile final : public vfs::File {
 public:
  HeaderReadFile(uint32_t access, vfs::Entry* entry, vfs::File* inner,
                 std::shared_ptr<HeaderReadFault> fault)
      : File(access, entry), inner_(inner), fault_(std::move(fault)) {
    ++fault_->opened;
  }
  ~HeaderReadFile() override { ++fault_->closed; }
  void Destroy() override { delete this; }
  X_STATUS ReadSync(std::span<uint8_t> bytes, size_t offset,
                    size_t* transferred) override {
    ++fault_->reads;
    size_t count = bytes.size();
    if (fault_->short_read && count) { --count; }
    if (fault_->status != X_STATUS_SUCCESS) { count = std::min(count, size_t(8)); }
    const auto status = inner_->ReadSync(bytes.first(count), offset, transferred);
    return status == X_STATUS_SUCCESS ? fault_->status : status;
  }
  X_STATUS WriteSync(std::span<const uint8_t>, size_t,
                     size_t* transferred) override {
    *transferred = 0; return X_STATUS_ACCESS_DENIED;
  }
 private:
  struct DestroyFile { void operator()(vfs::File* file) const { file->Destroy(); } };
  std::unique_ptr<vfs::File, DestroyFile> inner_;
  std::shared_ptr<HeaderReadFault> fault_;
};
class HeaderReadEntry final : public vfs::Entry {
 public:
  HeaderReadEntry(vfs::Device* device, vfs::Entry* inner,
                  std::shared_ptr<HeaderReadFault> fault)
      : Entry(device, nullptr, "package"), inner_(inner), fault_(std::move(fault)) {
    attributes_ = inner->attributes(); size_ = inner->size();
  }
  X_STATUS Open(uint32_t access, vfs::File** output) override {
    vfs::File* file = nullptr;
    const auto status = inner_->Open(access, &file);
    if (status != X_STATUS_SUCCESS) { return status; }
    *output = new HeaderReadFile(access, this, file, fault_);
    return X_STATUS_SUCCESS;
  }
 protected:
  bool DeleteEntryInternal(vfs::Entry*) override { return false; }
 private:
  vfs::Entry* inner_;
  std::shared_ptr<HeaderReadFault> fault_;
};
class HeaderReadDevice final : public vfs::HostPathDevice {
 public:
  HeaderReadDevice(const std::filesystem::path& path,
                    std::shared_ptr<filesystem::HostStorageRoot> storage,
                    std::shared_ptr<vfs::Device> parent,
                    std::shared_ptr<HeaderReadFault> fault)
      : HostPathDevice("\\Device\\HeaderRead", path, true, false, "FATX",
                       std::move(storage)),
        parent_(std::move(parent)), fault_(std::move(fault)) {}
  bool Initialize() override {
    if (!HostPathDevice::Initialize()) { return false; }
    auto* inner = HostPathDevice::ResolvePath("package");
    if (!inner) { return false; }
    entry_ = std::make_unique<HeaderReadEntry>(this, inner, fault_);
    return true;
  }
  vfs::Entry* ResolvePath(std::string_view path) override {
    return path == "\\package" || path == "package"
               ? entry_.get() : HostPathDevice::ResolvePath(path);
  }
  std::shared_ptr<vfs::Device> RetainedBackingParent() const override { return parent_; }
  std::optional<std::vector<filesystem::GuestHostBacking>> GuestBackings() const override {
    ++fault_->registration_backings;
    // Called by genuine common preparation after parser initialization. The
    // initial source mount is admitted normally before either fault is armed.
    auto before = std::move(fault_->before_registration_backings);
    if (before) { before(); }
    if (fault_->invalid_registration_backing) {
      return std::vector<filesystem::GuestHostBacking>{{{}, false}};
    }
    return HostPathDevice::GuestBackings();
  }
 private:
  const std::shared_ptr<vfs::Device> parent_;
  const std::shared_ptr<HeaderReadFault> fault_;
  std::unique_ptr<HeaderReadEntry> entry_;
};

class OpaqueProbe final : public XObject {
 public:
  explicit OpaqueProbe(KernelState& kernel) : XObject(&kernel, Type::Device, true) {
    ReleaseHandle();
  }
  bool Allocate(uint32_t type) {
    return CreateNativeNonDispatch(sizeof(uint32_t), type) != nullptr;
  }
};
}  // namespace

TEST_CASE("Native PIRS retains backing and retires only its own mount",
          "[kernel][stfs][native][ownership]") {
  const bool single_hash = GENERATE(true, false);
  Fixture f(true, single_hash);
  const auto type = f.TypeBytes();
  f.backing->set_position(123);
  REQUIRE(f.Create() == X_STATUS_SUCCESS);
  REQUIRE(f.backing->position() == 123);
  const uint32_t body = f.Load32(0x48);
  const uint32_t context = f.Load32(0x4C);
  REQUIRE(body); REQUIRE(context);
  auto device = XStfsDevice::Lookup(f.kernel(), body);
  REQUIRE(device); REQUIRE(device->type() == XObject::Type::Device);
  REQUIRE(device->handles().empty());
  REQUIRE(device->context_address() == context);
  CHECK(std::memcmp(device->descriptor().data(),
      f.memory().TranslateVirtual(f.packet() + 0x1C), 36) == 0);
  REQUIRE(f.memory().QueryRangeAccess(context, device->context_size()) ==
          memory::PageAccess::kReadWrite);
  REQUIRE(f.TypeBytes() == type);
  auto* member = f.fs().ResolvePath("\\Device\\NativePirs0\\payload");
  REQUIRE(member);
  REQUIRE(member->device()->is_read_only());
  vfs::File* file = nullptr;
  REQUIRE(member->Open(filesystem::FileAccess::kFileReadData, &file) ==
          X_STATUS_SUCCESS);
  std::array<uint8_t, 1> attempted{0xFF};
  size_t written = 99;
  CHECK(file->WriteSync(attempted, 0, &written) == X_STATUS_ACCESS_DENIED);
  CHECK(file->SetLength(0) == X_STATUS_ACCESS_DENIED);
  f.CloseBacking();
  REQUIRE_FALSE(f.source_lease.expired());
  f.Call(0x10F, {body});
  REQUIRE(device->pointer_references() == 1);
  SECTION("caller dereferences before dismount") {
    f.Call(0x105, {body});
    REQUIRE(device->pointer_references() == 0);
    REQUIRE(f.Call(0x3B, {body}) == X_STATUS_SUCCESS);
  }
  SECTION("rollback dismount precedes dereference") {
    REQUIRE(f.Call(0x3B, {body}) == X_STATUS_SUCCESS);
    f.Call(0x105, {body});
    REQUIRE(device->pointer_references() == 0);
  }
  REQUIRE(device->retired());
  REQUIRE_FALSE(f.fs().ResolvePath("\\Device\\NativePirs0\\payload"));
  REQUIRE(f.fs().ResolvePath("\\Device\\PirsSource\\package"));
  vfs::File* refused = nullptr;
  REQUIRE(member->Open(filesystem::FileAccess::kFileReadData, &refused) ==
          XStfsDevice::kDisconnected);
  REQUIRE_FALSE(refused);
  REQUIRE_FALSE(f.source_lease.expired());
  // An existing file pins the actual parser/reader independently of body.
  std::array<uint8_t, 16> output{};
  size_t count = 0;
  REQUIRE(file->ReadSync(output, 0, &count) == X_STATUS_SUCCESS);
  REQUIRE(count == output.size());
  for (size_t i = 0; i != output.size(); ++i) { CHECK(output[i] == i); }
  file->Destroy();
  REQUIRE(f.source_lease.expired());
  REQUIRE(device->backing_drained());
  // Body/context persist, but neither retained source nor readonly mounted
  // namespace does. No old device can keep a managed-format source busy.
  REQUIRE(XStfsDevice::Lookup(f.kernel(), body));
  REQUIRE(f.memory().QueryRangeAccess(context, device->context_size()) ==
          memory::PageAccess::kReadWrite);
  REQUIRE(f.TypeBytes() == type);
  REQUIRE(f.Call(0x25A, {body, 3, f.packet() + 0x300}) ==
          XStfsDevice::kDisconnected);
}

TEST_CASE("Native PIRS refuses unknown controls and state serialization",
          "[kernel][stfs][native][ownership]") {
  Fixture f;
  REQUIRE(f.Create() == X_STATUS_SUCCESS);
  const auto body = f.Load32(0x48);
  std::array<uint8_t, 36> before{};
  std::memcpy(before.data(), f.memory().TranslateVirtual(f.packet() + 0x300),
              before.size());
  for (uint32_t control : {0u, 1u, 2u, 3u, 4u, 5u, 0xFFFFFFFFu}) {
    CHECK(f.Call(0x25A, {body, control, f.packet() + 0x300}) ==
          X_STATUS_INVALID_DEVICE_REQUEST);
  }
  CHECK(std::memcmp(before.data(),
      f.memory().TranslateVirtual(f.packet() + 0x300), before.size()) == 0);
  std::array<uint8_t, 64> storage{};
  ByteStream stream(storage.data(), storage.size());
  REQUIRE_FALSE(f.kernel().Save(&stream));
  CHECK(stream.offset() == 0);
  REQUIRE(f.Call(0x3B, {body}) == X_STATUS_SUCCESS);
  REQUIRE_FALSE(f.kernel().Save(&stream));
  CHECK(stream.offset() == 0);
  REQUIRE_FALSE(f.kernel().Restore(&stream));
  CHECK(stream.offset() == 0);
}

TEST_CASE("Native PIRS rejects RW backing and malformed packets atomically",
          "[kernel][stfs][native][bounds]") {
  const uint32_t mode = GENERATE(0u, 1u, 2u, 3u, 4u);
  Fixture f(mode != 0);
  if (mode == 1) {  // LIVE verifier class is outside this adapter.
    f.memory().TranslateVirtual(f.packet())[0x53] = 3;
  } else if (mode == 2) {  // Logical geometry cannot invent physical bytes.
    f.Store64(8, uint64_t(f.bytes.size()) + 0x1000);
  } else if (mode == 3) {
    f.Store32(0x44, 0x20);
  } else if (mode == 4) {
    f.memory().TranslateVirtual(f.packet())[0x1C + 8] ^= 1;
  }
  REQUIRE(f.Create() == (mode == 0 ? X_STATUS_ACCESS_DENIED
                                    : X_STATUS_INVALID_PARAMETER));
  CHECK(f.Load32(0x48) == 0xA1A2A3A4);
  CHECK(f.Load32(0x4C) == 0xB1B2B3B4);
  CHECK_FALSE(f.fs().GetDevice("\\Device\\NativePirs0"));
  REQUIRE(f.backing);
  std::array<uint8_t, 4> out{};
  size_t count = 0;
  CHECK(f.backing->file()->ReadSync(out, 0, &count) == X_STATUS_SUCCESS);
  CHECK(count == out.size());
  CHECK((out == std::array<uint8_t, 4>{'P', 'I', 'R', 'S'}));
}


TEST_CASE("Native PIRS output refusal and name collisions preserve namespace",
          "[kernel][stfs][native][bounds]") {
  Fixture f;
  SECTION("readonly output page refuses before publishing a device") {
    auto* heap = f.memory().LookupHeap(f.packet());
    REQUIRE(heap);
    REQUIRE(heap->Protect(f.packet(), 0x1000, kMemoryProtectRead));
    CHECK(f.Create() == X_STATUS_ACCESS_VIOLATION);
    REQUIRE(heap->Protect(f.packet(), 0x1000,
                          kMemoryProtectRead | kMemoryProtectWrite));
    CHECK(f.Load32(0x48) == 0xA1A2A3A4);
    CHECK(f.Load32(0x4C) == 0xB1B2B3B4);
    CHECK_FALSE(f.fs().GetDevice("\\Device\\NativePirs0"));
  }
  SECTION("duplicate named admission cannot replace a genuine mount") {
    REQUIRE(f.Create() == X_STATUS_SUCCESS);
    const auto body = f.Load32(0x48);
    auto original = f.fs().AcquireDevice("\\Device\\NativePirs0");
    REQUIRE(original);
    f.Store32(0x48, 0xA1A2A3A4); f.Store32(0x4C, 0xB1B2B3B4);
    REQUIRE(f.Create() == X_STATUS_OBJECT_NAME_COLLISION);
    CHECK(f.Load32(0x48) == 0xA1A2A3A4);
    CHECK(f.Load32(0x4C) == 0xB1B2B3B4);
    CHECK(f.fs().AcquireDevice("\\Device\\NativePirs0") == original);
    original.reset();
    f.CloseBacking();
    REQUIRE(f.Call(0x3B, {body}) == X_STATUS_SUCCESS);
    CHECK(f.source_lease.expired());
  }
  SECTION("old body cannot dismount a namespace replacement") {
    REQUIRE(f.Create() == X_STATUS_SUCCESS);
    const auto body = f.Load32(0x48);
    REQUIRE(f.fs().UnregisterDevice("\\Device\\NativePirs0"));
    auto replacement = std::make_unique<vfs::HostPathDevice>(
        "\\Device\\NativePirs0", f.scratch.path / "packages", true);
    REQUIRE(replacement->Initialize());
    REQUIRE(f.fs().RegisterDevice(std::move(replacement)));
    auto actual = f.fs().AcquireDevice("\\Device\\NativePirs0");
    REQUIRE(actual);
    f.CloseBacking();
    REQUIRE(f.Call(0x3B, {body}) == X_STATUS_SUCCESS);
    CHECK(f.fs().AcquireDevice("\\Device\\NativePirs0") == actual);
    CHECK(f.fs().ResolvePath("\\Device\\NativePirs0\\package"));
    CHECK(f.source_lease.expired());
  }
}


TEST_CASE("Native PIRS member reads report real parent source retirement",
          "[kernel][stfs][native][ownership]") {
  Fixture f;
  REQUIRE(f.Create() == X_STATUS_SUCCESS);
  const auto body = f.Load32(0x48);
  auto* member = f.fs().ResolvePath("\\Device\\NativePirs0\\payload");
  REQUIRE(member);
  vfs::File* file = nullptr;
  REQUIRE(member->Open(filesystem::FileAccess::kFileReadData, &file) ==
          X_STATUS_SUCCESS);
  f.CloseBacking();
  auto source = f.fs().AcquireDevice("\\Device\\PirsSource");
  REQUIRE(source); source->Retire();
  // Real loader read failure must close its temporary native member File.
  // This uses the full path, no fake executable/current-title context.
  CHECK_FALSE(f.kernel().LoadUserModule(
      "\\Device\\NativePirs0\\payload", false));
  std::array<uint8_t, 16> output{}; output.fill(0xCC);
  size_t count = 99;
  CHECK(file->ReadSync(output, 0, &count) == XStfsDevice::kDisconnected);
  CHECK(count == 0);
  CHECK(std::all_of(output.begin(), output.end(),
                     [](uint8_t value) { return value == 0xCC; }));
  REQUIRE(f.Call(0x3B, {body}) == X_STATUS_SUCCESS);
  file->Destroy();
  CHECK(f.source_lease.expired());
}


TEST_CASE("Native PIRS normal Kernel shutdown drains owned files and bodies",
          "[kernel][stfs][native][ownership]") {
  Fixture f(true, true, true);
  const auto type = f.TypeBytes();
  REQUIRE(f.Create() == X_STATUS_SUCCESS);
  auto* member = f.fs().ResolvePath("\\Device\\NativePirs0\\payload");
  REQUIRE(member);
  // A normal VFS-owned nested container also pins a native parent member.
  // Kernel teardown must drain it before deciding a reader escaped externally.
  auto nested = vfs::XContentContainerDevice::CreateContentDevice(
      "\\Device\\NestedPirs", member);
  REQUIRE(nested); REQUIRE(nested->Initialize());
  REQUIRE(f.fs().RegisterDevice(std::move(nested)));
  REQUIRE(f.fs().AcquireDevice("\\Device\\NestedPirs")->mount_parent() ==
          f.fs().AcquireDevice("\\Device\\NativePirs0"));
  REQUIRE(f.fs().ResolvePath("\\Device\\NestedPirs\\payload"));
  vfs::File* raw = nullptr;
  REQUIRE(member->Open(filesystem::FileAccess::kFileReadData, &raw) ==
          X_STATUS_SUCCESS);
  // The ordinary Kernel object table owns this open member and backing handle.
  // No external host File/object_ref is retained across genuine Shutdown.
  auto file = object_ref<XFile>(new XFile(&f.kernel(), raw, true, false, true));
  file.reset();
  f.backing.reset();
  REQUIRE_FALSE(f.source_lease.expired());
  CHECK(f.TypeBytes() == type);
  f.emulator->Shutdown();
  CHECK(KernelState::shared() == nullptr);
  CHECK(f.emulator->kernel_state() == nullptr);
  CHECK(f.emulator->memory() == nullptr);
  CHECK(f.source_lease.expired());
}

TEST_CASE("Opaque native allocation preserves the shared Kernel device type",
          "[kernel][stfs][native][object]") {
  Fixture f;
  const auto before = f.TypeBytes();
  uint32_t body = 0;
  {
    auto object = object_ref<OpaqueProbe>(new OpaqueProbe(f.kernel()));
    REQUIRE(object->Allocate(f.kernel().GetKernelGuestGlobals() +
                            offsetof(KernelGuestGlobals, IoDeviceObjectType)));
    body = object->guest_object();
    REQUIRE(object->handles().empty());
    REQUIRE(f.kernel().object_table()->LookupAllocatedBody(body));
    auto raw = XObject::GetNativeObject(&f.kernel(),
                                       f.memory().TranslateVirtual(body));
    REQUIRE(raw.get() == object.get());
    CHECK_FALSE(XObject::GetNativeObject<XEvent>(
        &f.kernel(), f.memory().TranslateVirtual(body)));
    CHECK(xe::load_and_swap<uint32_t>(f.memory().TranslateVirtual(body)) == 0);
  }
  CHECK_FALSE(f.kernel().object_table()->LookupAllocatedBody(body));
  CHECK(f.TypeBytes() == before);
  // The actual Fixture/Kernel teardown also destroys its typed tombstones
  // through this same borrowed-type path, before freeing global Memory.
  REQUIRE(f.Create() == X_STATUS_SUCCESS);
  CHECK(f.TypeBytes() == before);
}
}  // namespace xe::kernel::test

namespace xe::kernel::test {
namespace {
struct MemberFile {
  vfs::File* file = nullptr;
  explicit MemberFile(vfs::Entry* entry,
      uint32_t access = filesystem::FileAccess::kFileReadData) {
    REQUIRE(entry); REQUIRE(entry->Open(access, &file) == X_STATUS_SUCCESS);
    REQUIRE(file);
  }
  ~MemberFile() { Reset(); }
  void Reset() { if (file) { file->Destroy(); file = nullptr; } }
};
struct KernelMember {
  object_ref<XFile> file;
  KernelMember(Fixture& f, vfs::Entry* entry) {
    REQUIRE(entry); vfs::File* raw = nullptr;
    REQUIRE(entry->Open(filesystem::FileAccess::kFileReadData, &raw) == X_STATUS_SUCCESS);
    REQUIRE(raw); file = object_ref<XFile>(new XFile(&f.kernel(), raw, true, false, true));
  }
  ~KernelMember() { Reset(); }
  void Reset() { if (file) { file->ReleaseHandle(); file.reset(); } }
};
void InnerPacket(Fixture& f, KernelMember& input, const std::string& name) {
  auto header = vfs::XContentContainerDevice::ReadContainerHeader(input.file->file());
  REQUIRE(header); REQUIRE(name.size() <= 255);
  std::memset(f.memory().TranslateVirtual(f.allocation + 0x200), 0, 256);
  std::memcpy(f.memory().TranslateVirtual(f.allocation + 0x200), name.data(), name.size());
  f.Store16(0, uint16_t(name.size())); f.Store16(2, uint16_t(name.size()));
  const auto rounded = (uint64_t(header->content_header.header_size) + 0xFFF) & ~uint64_t(0xFFF);
  const uint64_t bytes = header->content_metadata.content_size;
  f.Store64(8, rounded); f.Store64(0x10, ((bytes ? bytes : 0xFFFFF000ull) + 0xFFFFF) & ~uint64_t(0xFFFFF));
  f.Store32(0x18, input.file->handle());
  std::memcpy(f.memory().TranslateVirtual(f.packet() + 0x1C), &header->content_metadata.volume_descriptor.stfs, 36);
  f.Store32(0x44, uint32_t(rounded + 0x192));
  f.memory().TranslateVirtual<uint8_t*>(f.packet())[0x52] = bytes != 0;
  f.Store32(0x48, 0xA1A2A3A4); f.Store32(0x4C, 0xB1B2B3B4);
}
void ReadPayload(MemberFile& file) {
  std::array<uint8_t, 16> bytes{}; size_t count = 0;
  REQUIRE(file.file->ReadSync(bytes, 0, &count) == X_STATUS_SUCCESS);
  REQUIRE(count == bytes.size());
  for (size_t i = 0; i != bytes.size(); ++i) { REQUIRE(bytes[i] == i); }
}
void QueryTypes(Fixture& f, const std::string& path, uint32_t raw, uint32_t logical) {
  KernelMember member(f, f.fs().ResolvePath(path + "\\payload"));
  std::memset(f.memory().TranslateVirtual(f.allocation + 0x6FC), 0xA5, 20);
  std::memset(f.memory().TranslateVirtual(f.allocation + 0x73C), 0xA5, 16);
  REQUIRE(f.Call(0xEF, {member.file->handle(), f.allocation + 0x740,
      f.allocation + 0x700, 8, 4}) == X_STATUS_SUCCESS);
  REQUIRE(f.Load32(0x700) == raw); REQUIRE(f.Load32(0x704) == 0);
  REQUIRE(f.Load32(0x740) == X_STATUS_SUCCESS); REQUIRE(f.Load32(0x744) == 8);
  REQUIRE(f.Load32(0x6FC) == 0xA5A5A5A5); REQUIRE(f.Load32(0x708) == 0xA5A5A5A5);
  REQUIRE(f.Load32(0x73C) == 0xA5A5A5A5); REQUIRE(f.Load32(0x748) == 0xA5A5A5A5);
  REQUIRE(f.fs().RegisterSymbolicLink("NativeVolume:", path));
  std::memset(f.memory().TranslateVirtual(f.allocation + 0x500), 0, 0x4C);
  const std::string root = "NativeVolume";
  std::memcpy(f.memory().TranslateVirtual(f.allocation + 0x500), root.data(), root.size());
  f.Store32(0x540, 1); f.Store32(0x544, f.allocation + 0x600);
  f.Store32(0x600, 0xA5A5A5A5);
  const auto result = f.kernel().app_manager()->DispatchMessageSync(0xFE, 0x20021, f.allocation + 0x500, 0x4C);
  REQUIRE(result == (logical ? X_E_SUCCESS : X_E_INVALIDARG));
  REQUIRE(f.Load32(0x600) == (logical ? logical : 0xA5A5A5A5));
  REQUIRE(f.fs().UnregisterSymbolicLink("NativeVolume:"));
}
}
TEST_CASE("Native PIRS inherits exact HDD USB optical and explicitly unknown parent volumes",
          "[kernel][stfs][native][volume][abi]") {
  const auto type = GENERATE(filesystem::ConsoleStorageType::HDD,
      filesystem::ConsoleStorageType::USBMASS, filesystem::ConsoleStorageType::ODD,
      filesystem::ConsoleStorageType::Invalid);
  Fixture f(true, true, false, type);
  f.kernel().deployment_type_ = XDeploymentType::kOpticalDisc;
  REQUIRE(f.Create() == X_STATUS_SUCCESS);
  const auto parent = f.fs().AcquireDevice("\\Device\\PirsSource");
  const auto child = f.fs().AcquireDevice("\\Device\\NativePirs0");
  REQUIRE(parent); REQUIRE(child); REQUIRE(child->mount_parent() == parent);
  REQUIRE(child->storage_volume() == parent->storage_volume());
  const uint32_t raw = type == filesystem::ConsoleStorageType::HDD ? 7 :
      type == filesystem::ConsoleStorageType::USBMASS ? 0x2D :
      type == filesystem::ConsoleStorageType::ODD ? 2 : 0x22;
  QueryTypes(f, "\\Device\\NativePirs0", raw, uint32_t(type));
}
TEST_CASE("Native PIRS preparation refuses withdrawn or same-name replaced exact parents",
          "[kernel][stfs][native][ownership][prepare]") {
  Fixture f;
  std::unique_ptr<vfs::Device> replacement;
  SECTION("Exact parent withdrawn before common commit") {}
  SECTION("Different parent reuses the original mount name before commit") {
    auto device = std::make_unique<vfs::HostPathDevice>("\\Device\\PirsSource", f.scratch.path / "packages", true);
    REQUIRE(device->Initialize()); replacement = std::move(device);
  }
  const bool replacing = bool(replacement);
  REQUIRE(NativeStfsDeviceTestAccess::WithdrawBeforeCommit(f.kernel(), f.packet(),
      replacing ? &replacement : nullptr) == XStfsDevice::kDisconnected);
  REQUIRE(f.Load32(0x48) == 0xA1A2A3A4); REQUIRE(f.Load32(0x4C) == 0xB1B2B3B4);
  REQUIRE_FALSE(f.fs().ResolvePath("\\Device\\NativePirs0\\payload"));
  if (replacing) { REQUIRE(f.fs().ResolvePath("\\Device\\PirsSource\\package")); }
  f.backing.reset();
}
TEST_CASE("Parent withdrawal retires native bodies without making bodies own escaped reader lifetime",
          "[kernel][stfs][native][ownership][parent]") {
  Fixture f; REQUIRE(f.Create() == X_STATUS_SUCCESS);
  auto body = XStfsDevice::Lookup(f.kernel(), f.Load32(0x48)); REQUIRE(body);
  auto parent = f.fs().AcquireDevice("\\Device\\PirsSource"); REQUIRE(parent);
  std::weak_ptr<vfs::Device> child = f.fs().AcquireDevice("\\Device\\NativePirs0");
  std::unique_ptr<MemberFile> held;
  SECTION("ABI body alone is a weak observer") {}
  SECTION("Actual open File retains a safe Entry while source reads retire") {
    held = std::make_unique<MemberFile>(f.fs().ResolvePath("\\Device\\NativePirs0\\payload"));
  }
  f.CloseBacking();
  REQUIRE(f.kernel().RetireMountedDeviceTree(parent) == X_STATUS_SUCCESS);
  REQUIRE(body->retired()); REQUIRE(body->Control(3) == XStfsDevice::kDisconnected);
  REQUIRE_FALSE(f.fs().ResolvePath("\\Device\\NativePirs0\\payload"));
  if (held) {
    REQUIRE_FALSE(child.expired()); REQUIRE_FALSE(body->backing_drained());
    REQUIRE(held->file->entry()->device() == child.lock().get());
    vfs::File* refused = nullptr;
    REQUIRE(held->file->entry()->Open(filesystem::FileAccess::kFileReadData, &refused) == XStfsDevice::kDisconnected);
    REQUIRE_FALSE(refused);
    std::array<uint8_t, 16> bytes{}; bytes.fill(0xCC); size_t count = 99;
    REQUIRE(held->file->ReadSync(bytes, 0, &count) == XStfsDevice::kDisconnected);
    REQUIRE(count == 0); REQUIRE(bytes.front() == 0xCC); held.reset();
  }
  REQUIRE(child.expired()); REQUIRE(body->backing_drained()); REQUIRE(f.source_lease.expired());
  REQUIRE(f.Call(0x3B, {body->guest_object()}) == X_STATUS_SUCCESS);
  REQUIRE(f.Call(0x3B, {body->guest_object()}) == XStfsDevice::kDisconnected);
}
TEST_CASE("Mixed native and HLE PIRS descendants use one common withdrawal closure",
          "[kernel][stfs][native][ownership][mixed]") {
  Fixture f(true, true, true, filesystem::ConsoleStorageType::HDD);
  auto* content = f.kernel().content_manager();
  object_ref<XStfsDevice> body;
  std::unique_ptr<KernelMember> input;
  std::unique_ptr<MemberFile> held;
  bool hle_parent = false;
  SECTION("Native parent with actual HLE entry-backed child") {
    REQUIRE(f.Create() == X_STATUS_SUCCESS); body = XStfsDevice::Lookup(f.kernel(), f.Load32(0x48));
    REQUIRE(content->OpenContentFile("HleChild", "\\Device\\NativePirs0\\payload") == X_ERROR_SUCCESS);
    auto child = f.fs().AcquirePathDevice("HleChild:"); REQUIRE(child);
    REQUIRE(child->mount_parent() == f.fs().AcquireDevice("\\Device\\NativePirs0"));
    held = std::make_unique<MemberFile>(f.fs().ResolvePath("HleChild:\\payload"));
  }
  SECTION("HLE parent with actual native child") {
    hle_parent = true;
    REQUIRE(content->OpenContentFile("HleParent", f.source_path) == X_ERROR_SUCCESS);
    input = std::make_unique<KernelMember>(f, f.fs().ResolvePath("HleParent:\\payload"));
    InnerPacket(f, *input, "\\Device\\NativePirs0"); REQUIRE(f.Create() == X_STATUS_SUCCESS);
    body = XStfsDevice::Lookup(f.kernel(), f.Load32(0x48));
    REQUIRE(f.fs().AcquireDevice("\\Device\\NativePirs0")->mount_parent() == f.fs().AcquirePathDevice("HleParent:"));
    held = std::make_unique<MemberFile>(f.fs().ResolvePath("\\Device\\NativePirs0\\payload"));
  }
  REQUIRE(body); ReadPayload(*held); f.CloseBacking();
  auto* entry = held->file->entry();
  if (hle_parent) { REQUIRE(content->CloseContent("HleParent") == X_ERROR_SUCCESS); }
  else { REQUIRE(f.Call(0x3B, {body->guest_object()}) == X_STATUS_SUCCESS); }
  REQUIRE(body->retired()); REQUIRE_FALSE(content->IsRootTaken(hle_parent ? "HleParent" : "HleChild"));
  REQUIRE_FALSE(f.fs().ResolvePath(hle_parent ? "\\Device\\NativePirs0\\payload" : "HleChild:\\payload"));
  REQUIRE(held->file->entry() == entry);
  // Explicit namespace removal retains the established immutable-reader
  // outcome while its actual host source is still live.
  ReadPayload(*held); held.reset(); input.reset(); REQUIRE(body->backing_drained());
}
TEST_CASE("Bootstrap installed-source selection certifies a distinct descriptor and native descendant",
          "[kernel][stfs][native][ownership][installed-source]") {
  const bool unrelated_same_class = GENERATE(false, true);
  Fixture f(true, true, true, filesystem::ConsoleStorageType::System, true);
  auto parent = f.fs().AcquireDevice("\\Device\\PirsSource"); REQUIRE(parent);
  auto* host = dynamic_cast<vfs::HostPathDevice*>(parent.get()); REQUIRE(host);
  std::error_code error; auto root = host->storage()->Subroot("Content", true, error);
  REQUIRE(root); REQUIRE_FALSE(error);
  if (unrelated_same_class) {
    REQUIRE(std::filesystem::create_directory(f.scratch.path / "foreign"));
    root = filesystem::HostStorageRoot::Open(f.scratch.path / "foreign", true, error, 0);
    REQUIRE(root); REQUIRE_FALSE(error);
  }
  auto* content = f.kernel().content_manager();
  REQUIRE(content->sources().RegisterReadOnly(
      {"installed-native-test", 1, xam::DeviceType::HDD, xam::ContentSourceKind::kInstalledSystem,
       root->path(), "\\Device\\PirsSource\\Content", u"Installed", true, 0}, root, 37));
  auto source = content->sources().Source("installed-native-test"); REQUIRE(source); REQUIRE(source->available);
  REQUIRE(source->storage_volume != parent->storage_volume());
  if (unrelated_same_class) {
    REQUIRE(content->OpenContentFile("InstalledPack", f.source_path, &*source) != X_ERROR_SUCCESS);
    REQUIRE_FALSE(content->IsRootTaken("InstalledPack"));
    REQUIRE_FALSE(f.fs().AcquirePathDevice("InstalledPack:"));
    return;
  }
  REQUIRE(content->OpenContentFile("InstalledPack", f.source_path, &*source) == X_ERROR_SUCCESS);
  auto selected = f.fs().AcquirePathDevice("InstalledPack:"); REQUIRE(selected);
  REQUIRE(selected->mount_parent() == parent); REQUIRE(selected->storage_volume() == source->storage_volume);
  KernelMember inner(f, f.fs().ResolvePath("InstalledPack:\\payload"));
  InnerPacket(f, inner, "\\Device\\NativePirs0"); REQUIRE(f.Create() == X_STATUS_SUCCESS);
  auto native = f.fs().AcquireDevice("\\Device\\NativePirs0"); REQUIRE(native);
  REQUIRE(native->mount_parent() == selected); REQUIRE(native->storage_volume() == source->storage_volume);
  QueryTypes(f, "\\Device\\NativePirs0", 7, 1);
  REQUIRE(f.kernel().RetireMountedDeviceTree(parent) == X_STATUS_SUCCESS);
  REQUIRE_FALSE(content->IsRootTaken("InstalledPack"));
  REQUIRE_FALSE(f.fs().ResolvePath("\\Device\\NativePirs0\\payload"));
}
}  // namespace xe::kernel::test

namespace xe::kernel::test {
TEST_CASE("Native PIRS descendants retire through the same graph while retained immutable reads stay valid",
          "[kernel][stfs][native][ownership][nested]") {
  const bool withdraw_source = GENERATE(false, true);
  Fixture f(true, true, true); REQUIRE(f.Create() == X_STATUS_SUCCESS);
  auto outer = XStfsDevice::Lookup(f.kernel(), f.Load32(0x48)); REQUIRE(outer);
  KernelMember input(f, f.fs().ResolvePath("\\Device\\NativePirs0\\payload"));
  InnerPacket(f, input, "\\Device\\NativePirs1"); REQUIRE(f.Create() == X_STATUS_SUCCESS);
  auto inner = XStfsDevice::Lookup(f.kernel(), f.Load32(0x48)); REQUIRE(inner);
  REQUIRE(f.fs().AcquireDevice("\\Device\\NativePirs1")->mount_parent() ==
          f.fs().AcquireDevice("\\Device\\NativePirs0"));
  MemberFile held(f.fs().ResolvePath("\\Device\\NativePirs1\\payload"));
  auto* entry = held.file->entry(); ReadPayload(held); f.CloseBacking();
  if (withdraw_source) {
    const auto source = f.fs().AcquireDevice("\\Device\\PirsSource"); REQUIRE(source);
    REQUIRE(f.kernel().RetireMountedDeviceTree(source) == X_STATUS_SUCCESS);
  } else { REQUIRE(f.Call(0x3B, {outer->guest_object()}) == X_STATUS_SUCCESS); }
  REQUIRE(outer->retired()); REQUIRE(inner->retired());
  REQUIRE_FALSE(f.fs().ResolvePath("\\Device\\NativePirs0\\payload"));
  REQUIRE_FALSE(f.fs().ResolvePath("\\Device\\NativePirs1\\payload"));
  REQUIRE(held.file->entry() == entry);
  if (withdraw_source) {
    std::array<uint8_t, 16> bytes{}; bytes.fill(0xCC); size_t count = 99;
    REQUIRE(held.file->ReadSync(bytes, 0, &count) == XStfsDevice::kDisconnected);
    REQUIRE(count == 0); REQUIRE(bytes.front() == 0xCC);
  } else { ReadPayload(held); }
  vfs::File* refused = nullptr;
  REQUIRE(entry->Open(filesystem::FileAccess::kFileReadData, &refused) == XStfsDevice::kDisconnected);
  REQUIRE_FALSE(refused); held.Reset(); input.Reset();
  REQUIRE(outer->backing_drained()); REQUIRE(inner->backing_drained());
  REQUIRE(f.Call(0x3B, {inner->guest_object()}) == X_STATUS_SUCCESS);
}
}  // namespace xe::kernel::test

namespace xe::kernel::test {
namespace {
std::filesystem::path MutableFixtureFolder(Fixture& f) {
  const auto root = f.scratch.path / "packages" / "managed";
  REQUIRE(std::filesystem::is_directory(root));
  REQUIRE(f.fs().ResolvePath("\\Device\\PirsSource\\managed"));
  REQUIRE(f.fs().ResolvePath("\\Device\\PirsSource\\managed\\package"));
  REQUIRE(f.fs().ResolvePath("\\Device\\PirsSource\\managed\\value.bin"));
  f.CloseBacking();
  return root;
}
}
TEST_CASE("Ordinary HLE close keeps a real raw HostPath writer and closing root alive until retry",
          "[kernel][stfs][native][ownership][hostpath][busy]") {
  Fixture f(false, true, false, filesystem::ConsoleStorageType::HDD,
            false, true);
  const auto folder = MutableFixtureFolder(f);
  auto* content = f.kernel().content_manager();
  REQUIRE(content->OpenContentFile("MutableRoot", "\\Device\\PirsSource\\managed") == X_ERROR_SUCCESS);
  const auto device = f.fs().AcquirePathDevice("MutableRoot:"); REQUIRE(device);
  auto* host = dynamic_cast<vfs::HostPathDevice*>(device.get()); REQUIRE(host);
  REQUIRE_FALSE(host->is_read_only());
  auto* entry = f.fs().ResolvePath("MutableRoot:\\value.bin"); REQUIRE(entry);
  MemberFile writer(entry, filesystem::FileAccess::kFileReadData |
                           filesystem::FileAccess::kFileWriteData);
  // This is a genuine VFS File, intentionally outside the Kernel object table.
  const std::array<uint8_t, 4> changed{1, 3, 5, 7}; size_t count = 99;
  REQUIRE(writer.file->WriteSync(changed, 0, &count) == X_STATUS_SUCCESS);
  REQUIRE(count == changed.size());
  REQUIRE(content->CloseContent("MutableRoot") == X_ERROR_SHARING_VIOLATION);
  REQUIRE(content->IsRootTaken("MutableRoot"));
  REQUIRE(device->namespace_retiring()); REQUIRE_FALSE(host->storage()->retiring());
  REQUIRE(f.fs().AcquireDevice(device->mount_path()) == device);
  REQUIRE_FALSE(f.fs().ResolvePath("MutableRoot:\\value.bin"));
  REQUIRE(content->OpenContentFile("mutableroot", "\\Device\\PirsSource\\managed") == X_ERROR_ALREADY_EXISTS);
  vfs::File* refused = nullptr;
  REQUIRE(entry->Open(filesystem::FileAccess::kFileReadData, &refused) == XStfsDevice::kDisconnected);
  REQUIRE_FALSE(refused);
  const std::array<uint8_t, 4> final{2, 4, 6, 8}; count = 99;
  REQUIRE(writer.file->WriteSync(final, 0, &count) == X_STATUS_SUCCESS);
  REQUIRE(count == final.size()); REQUIRE(writer.file->Flush() == X_STATUS_SUCCESS);
  std::array<uint8_t, 4> read{}; count = 0;
  REQUIRE(writer.file->ReadSync(read, 0, &count) == X_STATUS_SUCCESS);
  REQUIRE(count == read.size()); REQUIRE(read == final);
  writer.Reset();
  REQUIRE(content->CloseContent("MutableRoot") == X_ERROR_SUCCESS);
  REQUIRE_FALSE(content->IsRootTaken("MutableRoot"));
  REQUIRE_FALSE(f.fs().IsSymbolicLinkRegistered("MutableRoot:"));
  REQUIRE_FALSE(f.fs().AcquireDevice(device->mount_path()));
  REQUIRE(host->storage()->retiring());
  std::ifstream original(folder / "value.bin", std::ios::binary);
  original.read(reinterpret_cast<char*>(read.data()), read.size());
  REQUIRE(original.good()); REQUIRE(read == final);
  REQUIRE(content->OpenContentFile("MutableRoot", "\\Device\\PirsSource\\managed") == X_ERROR_SUCCESS);
  const auto replacement = f.fs().AcquirePathDevice("MutableRoot:"); REQUIRE(replacement);
  REQUIRE(replacement != device);
  REQUIRE(content->CloseContent("MutableRoot", device.get()) == X_ERROR_INVALID_HANDLE);
  REQUIRE(f.fs().AcquirePathDevice("MutableRoot:") == replacement);
  REQUIRE(content->CloseContent("MutableRoot", replacement.get()) == X_ERROR_SUCCESS);
}

TEST_CASE("HLE HostPath close releases internal native-chain readers before testing a real escaped ancestor lease",
          "[kernel][stfs][native][ownership][hostpath][nested][busy]") {
  const bool escape_reader = GENERATE(false, true);
  Fixture f(false, true, true, filesystem::ConsoleStorageType::HDD,
            false, true);
  MutableFixtureFolder(f);
  auto* content = f.kernel().content_manager();
  REQUIRE(content->OpenContentFile("MutableParent", "\\Device\\PirsSource\\managed") == X_ERROR_SUCCESS);
  const auto parent = f.fs().AcquirePathDevice("MutableParent:"); REQUIRE(parent);
  auto* host = dynamic_cast<vfs::HostPathDevice*>(parent.get()); REQUIRE(host);
  KernelMember input(f, f.fs().ResolvePath("MutableParent:\\package"));
  InnerPacket(f, input, "\\Device\\NativePirs0"); REQUIRE(f.Create() == X_STATUS_SUCCESS);
  auto outer = XStfsDevice::Lookup(f.kernel(), f.Load32(0x48)); REQUIRE(outer);
  KernelMember nested(f, f.fs().ResolvePath("\\Device\\NativePirs0\\payload"));
  InnerPacket(f, nested, "\\Device\\NativePirs1"); REQUIRE(f.Create() == X_STATUS_SUCCESS);
  auto inner = XStfsDevice::Lookup(f.kernel(), f.Load32(0x48)); REQUIRE(inner);
  std::unique_ptr<MemberFile> held;
  if (escape_reader) {
    held = std::make_unique<MemberFile>(f.fs().ResolvePath("\\Device\\NativePirs1\\payload"));
    ReadPayload(*held);
  }
  input.Reset(); nested.Reset();
  const auto result = content->CloseContent("MutableParent");
  REQUIRE(result == (escape_reader ? X_ERROR_SHARING_VIOLATION : X_ERROR_SUCCESS));
  REQUIRE(outer->retired()); REQUIRE(inner->retired());
  REQUIRE_FALSE(f.fs().ResolvePath("\\Device\\NativePirs0\\payload"));
  REQUIRE_FALSE(f.fs().ResolvePath("\\Device\\NativePirs1\\payload"));
  if (held) {
    REQUIRE(content->IsRootTaken("MutableParent"));
    REQUIRE_FALSE(host->storage()->retiring()); REQUIRE(host->has_open_files());
    REQUIRE(f.fs().AcquireDevice(parent->mount_path()) == parent);
    ReadPayload(*held); // Namespace close did not retire the live ancestor.
    held.reset();
    REQUIRE(content->CloseContent("MutableParent") == X_ERROR_SUCCESS);
  }
  REQUIRE_FALSE(content->IsRootTaken("MutableParent"));
  REQUIRE_FALSE(f.fs().AcquireDevice(parent->mount_path()));
  REQUIRE(host->storage()->retiring());
  REQUIRE(outer->backing_drained()); REQUIRE(inner->backing_drained());
  REQUIRE(f.Call(0x3B, {outer->guest_object()}) == X_STATUS_SUCCESS);
  REQUIRE(f.Call(0x3B, {inner->guest_object()}) == X_STATUS_SUCCESS);
}
}  // namespace xe::kernel::test

namespace xe::kernel::test {
TEST_CASE("A common retirement batch owns exact nodes until finish and never consumes a later replacement",
          "[kernel][stfs][native][ownership][retirement]") {
  Fixture f; REQUIRE(f.Create() == X_STATUS_SUCCESS);
  auto body = XStfsDevice::Lookup(f.kernel(), f.Load32(0x48)); REQUIRE(body);
  auto node = f.fs().AcquireDevice("\\Device\\NativePirs0"); REQUIRE(node);
  X_STATUS status = X_STATUS_SUCCESS;
  auto first = f.fs().PrepareDeviceRetirement(node, true, status);
  REQUIRE(first); REQUIRE(status == X_STATUS_SUCCESS);
  REQUIRE(f.fs().CommitDeviceRetirement(*first) == X_STATUS_SUCCESS);
  auto competing = f.fs().PrepareDeviceRetirement(node, true, status);
  REQUIRE(competing); REQUIRE(status == X_STATUS_SUCCESS);
  REQUIRE(f.fs().CommitDeviceRetirement(*competing) == X_STATUS(0xC0000043));
  std::shared_ptr<vfs::Device> withdrawn;
  REQUIRE(f.fs().FinishDeviceRetirement(*competing, 0, withdrawn) == X_STATUS_INVALID_HANDLE);
  REQUIRE_FALSE(withdrawn);
  REQUIRE(f.fs().FinishDeviceRetirement(*first, 0, withdrawn) == X_STATUS_SUCCESS);
  REQUIRE(withdrawn == node); REQUIRE_FALSE(f.fs().AcquireDevice(node->mount_path()));
  withdrawn->Retire(); withdrawn.reset();
  auto replacement = std::make_unique<vfs::HostPathDevice>(
      "\\Device\\NativePirs0", f.scratch.path / "packages", true);
  REQUIRE(replacement->Initialize()); REQUIRE(f.fs().RegisterDevice(std::move(replacement)));
  const auto current = f.fs().AcquireDevice("\\Device\\NativePirs0"); REQUIRE(current);
  REQUIRE(current != node);
  REQUIRE(f.fs().FinishDeviceRetirement(*first, 0, withdrawn) == X_STATUS_INVALID_HANDLE);
  REQUIRE_FALSE(withdrawn); REQUIRE(f.fs().AcquireDevice("\\Device\\NativePirs0") == current);
  // The body's first explicit acknowledgement addresses its old exact node.
  // It may be busy while the first batch is still live; retry after it ends.
  first.reset(); competing.reset();
  REQUIRE(f.Call(0x3B, {body->guest_object()}) == X_STATUS_SUCCESS);
  REQUIRE(f.fs().AcquireDevice("\\Device\\NativePirs0") == current);
}
}  // namespace xe::kernel::test

namespace xe::kernel::test {
TEST_CASE("Borrowed content preserves attempted header I/O failures and releases its failed root",
          "[kernel][stfs][content][borrowed-header]") {
  const bool short_read = GENERATE(false, true);
  Fixture f(true, true, false, filesystem::ConsoleStorageType::System);
  auto parent = f.fs().AcquireDevice("\\Device\\PirsSource"); REQUIRE(parent);
  auto* host = dynamic_cast<vfs::HostPathDevice*>(parent.get()); REQUIRE(host);
  auto fault = std::make_shared<HeaderReadFault>();
  fault->short_read = short_read;
  fault->status = short_read ? X_STATUS_SUCCESS : X_STATUS_ACCESS_DENIED;
  auto device = std::make_unique<HeaderReadDevice>(
      f.scratch.path / "packages", host->storage(), parent, fault);
  REQUIRE(device->SetStorageVolume(parent->storage_volume()));
  REQUIRE(device->Initialize()); REQUIRE(f.fs().RegisterDevice(std::move(device)));
  auto source = f.fs().AcquireDevice("\\Device\\HeaderRead"); REQUIRE(source);
  auto* content = f.kernel().content_manager();
  std::shared_ptr<vfs::Device> opened;
  xam::BorrowedContentMountDiagnostic diagnostic;
  // STATUS_UNSUCCESSFUL from an unexpected successful short read maps to
  // ERROR_GEN_FAILURE(31); a real denied backend maps to ACCESS_DENIED(5).
  const auto expected = short_read ? X_RESULT(31) : X_ERROR_ACCESS_DENIED;
  REQUIRE(content->OpenContentFile("HeaderRetry", "\\Device\\HeaderRead\\package",
                                  nullptr, 0, &opened, &diagnostic) == expected);
  CHECK(diagnostic.failure_stage==xam::BorrowedContentMountDiagnostic::kCaptureHeader);
  CHECK(diagnostic.failure_result==expected); CHECK(diagnostic.result==expected);
  CHECK((diagnostic.available&128)!=0);
  CHECK(diagnostic.native_status==(short_read?X_STATUS_UNSUCCESSFUL:X_STATUS_ACCESS_DENIED));
  CHECK_FALSE(opened);
  CHECK(fault->reads == 1); CHECK(fault->opened == 1); CHECK(fault->closed == 1);
  CHECK_FALSE(content->IsRootTaken("HeaderRetry"));
  CHECK_FALSE(f.fs().AcquirePathDevice("HeaderRetry:"));
  CHECK(f.fs().AcquireDevice("\\Device\\HeaderRead") == source);
  CHECK(source->mount_parent() == parent);

  // Recovery uses the same retained native fixture bytes, not a valid-content
  // substitute or a forced mount. The failed root claim must have been released.
  fault->short_read = false; fault->status = X_STATUS_SUCCESS;
  xam::BorrowedContentMountDiagnostic retry;
  REQUIRE(content->OpenContentFile("HeaderRetry", "\\Device\\HeaderRead\\package",
                                  nullptr, 0, &opened, &retry) == X_ERROR_SUCCESS);
  REQUIRE(opened); CHECK(opened->mount_parent() == source);
  CHECK(retry.failure_stage==0); CHECK(retry.result==X_ERROR_SUCCESS);
  CHECK(retry.stage==xam::BorrowedContentMountDiagnostic::kCommit);
  CHECK(diagnostic.failure_result==expected);
  auto* entry = f.fs().ResolvePath("HeaderRetry:\\payload"); REQUIRE(entry);
  vfs::File* file = nullptr;
  REQUIRE(entry->Open(vfs::FileAccess::kFileReadData, &file) == X_STATUS_SUCCESS);
  const auto destroy = [](vfs::File* value) { value->Destroy(); };
  std::unique_ptr<vfs::File, decltype(destroy)> owned(file, destroy);
  std::array<uint8_t, 16> bytes{}; size_t transferred = 0;
  REQUIRE(owned->ReadSync(bytes, 0, &transferred) == X_STATUS_SUCCESS);
  REQUIRE(transferred == bytes.size());
  for (size_t i = 0; i != bytes.size(); ++i) { CHECK(bytes[i] == i); }
  owned.reset(); opened.reset();
  REQUIRE(content->CloseContent("HeaderRetry") == X_ERROR_SUCCESS);
  CHECK(fault->opened == fault->closed);
}

TEST_CASE("Borrowed content still rejects a short or malformed actual header without publication",
          "[kernel][stfs][content][borrowed-header]") {
  const bool truncated = GENERATE(false, true);
  Fixture f(true, true, false, filesystem::ConsoleStorageType::System);
  auto bytes = f.bytes;
  if (truncated) { bytes.resize(8); }
  else { std::fill_n(bytes.begin(), 4, uint8_t(0)); }
  {
    // Only this test's scratch package is changed. CaptureBorrowedPackage opens
    // a fresh retained handle and observes the actual current size/header.
    std::ofstream out(f.scratch.path / "packages" / "package", std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    REQUIRE(out.good());
  }
  auto* content = f.kernel().content_manager();
  std::shared_ptr<vfs::Device> opened;
  REQUIRE(content->OpenContentFile("BadHeader", f.source_path, nullptr, 0,
                                  &opened) == X_ERROR_FILE_CORRUPT);
  CHECK_FALSE(opened); CHECK_FALSE(content->IsRootTaken("BadHeader"));
  CHECK_FALSE(f.fs().AcquirePathDevice("BadHeader:"));
  CHECK(f.fs().AcquireDevice("\\Device\\PirsSource"));
}
}  // namespace xe::kernel::test

namespace xe::kernel::test {
TEST_CASE("Borrowed mount preserves a real registration error without consuming its source or root",
          "[kernel][stfs][content][borrowed-prepare]") {
  Fixture f(true, true, false, filesystem::ConsoleStorageType::System);
  auto parent = f.fs().AcquireDevice("\\Device\\PirsSource"); REQUIRE(parent);
  auto* host = dynamic_cast<vfs::HostPathDevice*>(parent.get()); REQUIRE(host);
  auto fault = std::make_shared<HeaderReadFault>();
  auto device = std::make_unique<HeaderReadDevice>(
      f.scratch.path / "packages", host->storage(), parent, fault);
  REQUIRE(device->SetStorageVolume(parent->storage_volume()));
  REQUIRE(device->Initialize()); REQUIRE(f.fs().RegisterDevice(std::move(device)));
  auto source = f.fs().AcquireDevice("\\Device\\HeaderRead"); REQUIRE(source);
  const auto admitted = fault->registration_backings;
  fault->invalid_registration_backing = true;
  auto* content = f.kernel().content_manager();
  std::shared_ptr<vfs::Device> opened;
  xam::BorrowedContentMountDiagnostic diagnostic;
  // Header and STFS parsing use actual valid file bytes. The real common
  // guest-storage admission rejects the invalid empty backing path afterward.
  REQUIRE(content->OpenContentFile("PrepareRetry", "\\Device\\HeaderRead\\package",
                                  nullptr, 0, &opened, &diagnostic) == X_ERROR_INVALID_PARAMETER);
  CHECK(diagnostic.failure_stage==xam::BorrowedContentMountDiagnostic::kRegistration);
  CHECK(diagnostic.failure_result==X_ERROR_INVALID_PARAMETER);
  CHECK(diagnostic.storage.operation==4); CHECK(diagnostic.storage.reason==3);
  CHECK(diagnostic.storage.error_category==1);
  CHECK(diagnostic.storage.error_value==int(std::errc::invalid_argument));
  // VFS/Content wrapping the same real failure must retain the inner cut.
  const auto first_line=diagnostic.storage.line; CHECK(first_line!=0);
  CHECK(fault->registration_backings == admitted + 1);
  CHECK(fault->reads > 1); CHECK(fault->opened == fault->closed);
  CHECK_FALSE(opened); CHECK_FALSE(content->IsRootTaken("PrepareRetry"));
  CHECK_FALSE(f.fs().AcquirePathDevice("PrepareRetry:"));
  CHECK(f.fs().AcquireDevice("\\Device\\HeaderRead") == source);
  CHECK_FALSE(source->namespace_retiring());
  CHECK(source->mount_parent() == parent);
  fault->invalid_registration_backing = false;
  xam::BorrowedContentMountDiagnostic retry;
  REQUIRE(content->OpenContentFile("PrepareRetry", "\\Device\\HeaderRead\\package",
                                  nullptr, 0, &opened, &retry) == X_ERROR_SUCCESS);
  REQUIRE(opened); CHECK(opened->mount_parent() == source);
  CHECK(retry.failure_stage==0); CHECK(retry.storage.line==0);
  CHECK(diagnostic.storage.line==first_line); CHECK(diagnostic.storage.reason==3);
  opened.reset(); REQUIRE(content->CloseContent("PrepareRetry") == X_ERROR_SUCCESS);
  CHECK(fault->opened == fault->closed);
}

TEST_CASE("Borrowed preparation cannot publish a replaced source and retries only the fresh exact generation",
          "[kernel][stfs][content][borrowed-prepare][installed-source]") {
  Fixture f(true, true, false, filesystem::ConsoleStorageType::System);
  auto original = f.fs().AcquireDevice("\\Device\\PirsSource"); REQUIRE(original);
  auto* host = dynamic_cast<vfs::HostPathDevice*>(original.get()); REQUIRE(host);
  std::error_code error;
  auto authority = host->storage()->Subroot({}, true, error);
  REQUIRE(authority); REQUIRE_FALSE(error);
  const xam::ContentSource descriptor{
      "prepare-source", 1, xam::DeviceType::HDD, xam::ContentSourceKind::kInstalledSystem,
      authority->path(), "\\Device\\PrepareSource", u"Prepare source", true, 0};
  auto* content = f.kernel().content_manager();
  REQUIRE(content->sources().RegisterReadOnly(descriptor, authority, 37));
  const auto selected = content->sources().Source(descriptor.id); REQUIRE(selected);
  auto native = std::make_unique<vfs::HostPathDevice>(descriptor.guest_root,
      selected->storage->path(), true, false, "FATX", selected->storage);
  REQUIRE(native->SetStorageVolume(selected->storage_volume));
  REQUIRE(native->Initialize()); REQUIRE(f.fs().RegisterDevice(std::move(native)));
  auto parent = f.fs().AcquireDevice(descriptor.guest_root); REQUIRE(parent);
  auto fault = std::make_shared<HeaderReadFault>();
  auto device = std::make_unique<HeaderReadDevice>(selected->storage->path(),
      selected->storage, parent, fault);
  REQUIRE(device->SetStorageVolume(selected->storage_volume));
  REQUIRE(device->Initialize()); REQUIRE(f.fs().RegisterDevice(std::move(device)));
  const auto before = fault->registration_backings;
  bool replaced = false;
  std::optional<xam::ContentSourceState> current;
  fault->before_registration_backings = [&] {
    // No CHECK/REQUIRE may throw through an offloaded host closure. The
    // original RunBlockingIo join publishes these real registration results.
    std::error_code replacement_error;
    auto next = host->storage()->Subroot({}, true, replacement_error);
    if (next && !replacement_error) {
      replaced = content->sources().RegisterReadOnly(descriptor, next, 38);
      if (replaced) { current = content->sources().Source(descriptor.id); }
    }
  };
  std::shared_ptr<vfs::Device> opened;
  const auto status = content->OpenContentFile("SourceRetry", "\\Device\\HeaderRead\\package",
                                              &*selected, 0, &opened);
  REQUIRE(replaced); REQUIRE(current); REQUIRE(current->available);
  CHECK(fault->registration_backings == before + 1);
  CHECK(current->generation != selected->generation);
  CHECK_FALSE(content->sources().CaptureStorageVolume(*selected));
  CHECK(status == X_ERROR_DEVICE_NOT_CONNECTED);
  CHECK_FALSE(opened); CHECK_FALSE(content->IsRootTaken("SourceRetry"));
  CHECK_FALSE(f.fs().AcquirePathDevice("SourceRetry:"));
  CHECK(fault->opened == fault->closed);
  // Retire only the actual old graph before publishing its fresh same-name
  // VFS source. An old mount/volume is never retagged as the new generation.
  REQUIRE(f.kernel().RetireMountedDeviceTree(parent) == X_STATUS_SUCCESS);
  REQUIRE_FALSE(f.fs().AcquireDevice(descriptor.guest_root));
  auto replacement = std::make_unique<vfs::HostPathDevice>(descriptor.guest_root,
      current->storage->path(), true, false, "FATX", current->storage);
  REQUIRE(replacement->SetStorageVolume(current->storage_volume));
  REQUIRE(replacement->Initialize()); REQUIRE(f.fs().RegisterDevice(std::move(replacement)));
  auto actual = f.fs().AcquireDevice(descriptor.guest_root); REQUIRE(actual); REQUIRE(actual != parent);
  REQUIRE(content->OpenContentFile("SourceRetry", "\\Device\\PrepareSource\\package",
                                  &*current, 0, &opened) == X_ERROR_SUCCESS);
  REQUIRE(opened); CHECK(opened->mount_parent() == actual);
  CHECK(opened->storage_volume() == current->storage_volume);
  opened.reset(); REQUIRE(content->CloseContent("SourceRetry") == X_ERROR_SUCCESS);
  CHECK(f.fs().AcquireDevice(descriptor.guest_root) == actual);
}
}  // namespace xe::kernel::test

#if XE_PLATFORM_LINUX
namespace xe::kernel::test {
namespace {
struct FolderFileCloser { void operator()(vfs::File* file) const { if (file) { file->Destroy(); } } };
using FolderFile = std::unique_ptr<vfs::File, FolderFileCloser>;
class FolderDeniedMember final : public vfs::ManagedFileService {
 public:
  FolderDeniedMember(std::filesystem::path file,
      std::shared_ptr<const filesystem::HostStorageDirectoryIdentity> parent)
      : path_(std::move(file)), parent_(std::move(parent)) {}
  const std::filesystem::path& backing_path() const override { return path_; }
  auto backing_directory() const -> std::shared_ptr<const filesystem::HostStorageDirectoryIdentity> override { return parent_; }
  bool retiring() const override { return false; }
  X_STATUS Open(std::shared_ptr<vfs::Device>, std::string, vfs::FileDisposition,
      uint32_t, bool, vfs::File** output, vfs::FileAction* action) override {
    ++attempts; *output = nullptr; *action = vfs::FileAction::kDoesNotExist; return X_STATUS_ACCESS_DENIED;
  }
  unsigned attempts = 0;
 private:
  std::filesystem::path path_;
  std::shared_ptr<const filesystem::HostStorageDirectoryIdentity> parent_;
};
// Real CM/retained filesystem/crypto/managed registry/VFS/Kernel path below
// XAM's original parser and actual native cache issuer. No PPC body executes,
// no native SAVE/frame/image gate is claimed, and no retail attestation exists.
class FolderFixture {
 public:
  FolderFixture() : f(false, true, false, filesystem::ConsoleStorageType::HDD,
                      false, false, true) {
    const auto selected = f.kernel().content_manager()->sources().Source("native-wire");
    REQUIRE(selected); source = *selected;
    REQUIRE(source.storage); REQUIRE(source.volume); REQUIRE(source.storage_volume);
    REQUIRE(f.kernel().console_signing_identity());
    vfs::XContentContainerHeader value{};
    value.content_header.magic = vfs::XContentPackageType::kCon;
    value.content_header.header_size = sizeof(value) + extra_header_bytes;
    value.content_metadata.metadata_version = 2;
    value.content_metadata.content_type = XContentType::kProfile;
    value.content_metadata.profile_id = xuid;
    value.content_metadata.execution_info.title_id = 0xFFFE07D1;
    value.content_metadata.content_size = payload.size() + protected_bytes.size();
    value.content_metadata.volume_descriptor.stfs.descriptor_length = 36;
    value.content_metadata.volume_descriptor.stfs.file_table_block_count = 1;
    value.content_metadata.volume_descriptor.stfs.total_block_count = 3;
    const auto console_id = f.kernel().console_signing_identity()->console_id();
    std::copy(console_id.begin(), console_id.end(), value.content_metadata.console_id);
    // Existing aligned source fixture: extra Header and NONZERO real padding
    // are owned bytes; the later sidecar tail is preserved but never raw data.
    header.resize(aligned_header_bytes + sidecar_tail_bytes, 0xA9);
    std::memcpy(header.data(), &value, sizeof(value));
    std::fill(header.begin() + sizeof(value), header.begin() + sizeof(value) + extra_header_bytes, 0x7B);
    std::fill(header.begin() + sizeof(value) + extra_header_bytes,
              header.begin() + aligned_header_bytes, 0xD3);
    REQUIRE_FALSE(xam::folder_content_detail::SignNativeHeader(header, *f.kernel().console_signing_identity()));
    std::error_code error;
    auto absent = source.volume->mutations()->CaptureMetadata(filesystem::ContentMutationOwner::Kind::kCreate,
        package, target, error);
    REQUIRE(absent); REQUIRE_FALSE(error); REQUIRE_FALSE(absent->exists());
    const std::array<filesystem::ContentPackageInitialMember, 2> members{{
        {"FFFE07D1.gpd", payload, false}, {"denied.bin", protected_bytes, false}}};
    const auto created = source.volume->mutations()->CreateInitial(package, absent, header, members);
    REQUIRE_FALSE(created.error); REQUIRE(created.complete);
    before = source.volume->mutations()->CaptureMetadata(filesystem::ContentMutationOwner::Kind::kHeader,
        package, target, error);
    REQUIRE(before); REQUIRE_FALSE(error);
    // Exercise the actual source mount producer; no private policy Prepare
    // helper or test registry shortcut supplies this raw CON capability.
    auto actual_package = source.storage->Subroot(package, false, error);
    REQUIRE(actual_package); REQUIRE_FALSE(error);
    denied = std::make_shared<FolderDeniedMember>(source.storage->path() / package / "denied.bin",
                                                 actual_package->directory_identity());
    REQUIRE(f.fs().RegisterManagedFile(denied));
    REQUIRE(f.kernel().content_manager()->MountManagedSourceDevices("native-wire") == X_STATUS_SUCCESS);
    vfs::File* opened = nullptr; vfs::FileAction action{};
    const auto status = f.fs().OpenFile(nullptr, guest_package, vfs::FileDisposition::kOpen,
        filesystem::FileAccess::kFileReadData | filesystem::FileAccess::kFileWriteData,
        false, true, &opened, &action);
    FolderFile close_on_failure(opened);
    REQUIRE(status == X_STATUS_SUCCESS); REQUIRE(opened); REQUIRE(action == vfs::FileAction::kOpened);
    raw = object_ref<XFile>(new XFile(&f.kernel(), close_on_failure.release(), true, false, true));
    raw_handle = raw->handle();
    f.Store32(0x18, raw_handle);
    auto* packet = f.memory().TranslateVirtual(f.packet()); packet[0x53] = 1;
    std::memcpy(packet + 0x1C, &value.content_metadata.volume_descriptor.stfs, 36);
  }
  ~FolderFixture() { raw.reset(); }
  void CloseRawHandle() {
    REQUIRE(f.kernel().object_table()->ReleaseHandle(raw_handle) == X_STATUS_SUCCESS);
    raw.reset(); raw_handle = 0;
  }
  FolderFile OpenMember(std::string_view leaf, uint32_t access, X_STATUS& status) {
    vfs::File* file = nullptr; vfs::FileAction action{};
    status = f.fs().OpenFile(nullptr, std::string("\\Device\\NativePirs0\\") + std::string(leaf),
        vfs::FileDisposition::kOpen, access, false, true, &file, &action);
    return FolderFile(file);
  }
  std::span<const uint8_t> NativeHeader() const {
    return std::span<const uint8_t>(header).first(aligned_header_bytes);
  }
  std::vector<uint8_t> ActualHeader() {
    std::vector<uint8_t> bytes; REQUIRE_FALSE(source.volume->content()->ReadFile(target, 1 << 20, bytes)); return bytes;
  }
  Fixture f;
  static constexpr uint64_t xuid = 0xE000000000000001ull;
  static constexpr uint32_t extra_header_bytes = 37;
  static constexpr uint32_t aligned_header_bytes = 0xA000;
  static constexpr uint32_t sidecar_tail_bytes = 11;
  const std::filesystem::path package = "E000000000000001/FFFE07D1/00010000/E000000000000001";
  const std::filesystem::path target = "E000000000000001/FFFE07D1/Headers/00010000/E000000000000001.header";
  const std::string guest_package = "\\Device\\PirsVolume\\Content\\E000000000000001\\FFFE07D1\\00010000\\E000000000000001";
  const std::vector<uint8_t> payload{0x10, 0, 0x20, 0x30, 0x40, 0, 0x50, 0x60};
  const std::vector<uint8_t> protected_bytes{7, 8, 9};
  xam::ContentSourceState source;
  std::vector<uint8_t> header;
  std::shared_ptr<const filesystem::HostStorageSnapshotToken> before;
  std::shared_ptr<FolderDeniedMember> denied;
  object_ref<XFile> raw;
  X_HANDLE raw_handle = 0;
};
}
TEST_CASE("CM folder Header projection reaches the real Stfs kernel mount and existing member policies",
          "[kernel][stfs][folder_con][ownership]") {
  FolderFixture f;
  auto* const original_file = f.raw->file();
  const auto* const original_entry = original_file->entry();
  vfs::File::Information info{};
  REQUIRE(original_file->QueryInformation(&info) == X_STATUS_SUCCESS);
  // Two real payload blocks + exact one-table descriptor + paired hashes.
  // This is the explicitly owned virtual range, not packed physical storage.
  CHECK(info.end_of_file == 0xF000);
  CHECK_FALSE(info.attributes & vfs::kFileAttributeDirectory);
  std::vector<uint8_t> actual(0xA000); size_t count = 99;
  REQUIRE(original_file->ReadSync(actual, 0, &count) == X_STATUS_SUCCESS);
  REQUIRE(count == actual.size());
  REQUIRE(std::equal(f.NativeHeader().begin(), f.NativeHeader().end(), actual.begin()));
  REQUIRE(actual[sizeof(vfs::XContentContainerHeader) + FolderFixture::extra_header_bytes] == 0xD3);
  CHECK(f.ActualHeader() == f.header); // unknown sidecar tail was not exposed
  std::array<uint8_t, 2> attempted{};
  CHECK(original_file->ReadSync(attempted, 0xA000, &count) == X_STATUS_NOT_SUPPORTED); CHECK(count == 0);
  CHECK(original_file->WriteSync(attempted, 0, &count) == X_STATUS_ACCESS_DENIED); CHECK(count == 0);
  REQUIRE(f.denied->attempts == 0);
  const auto old_content = f.f.fs().AcquireDevice("\\Device\\PirsVolume\\Content");
  REQUIRE(old_content);
  CHECK(XFAILED(f.f.kernel().content_manager()->RefreshManagedContentView("native-wire")));
  CHECK(f.f.fs().AcquireDevice("\\Device\\PirsVolume\\Content") == old_content);
  CHECK(f.raw->file() == original_file);
  f.raw->set_position(37);
  REQUIRE(f.f.Create() == X_STATUS_SUCCESS); CHECK(f.raw->position() == 37);
  const auto body_address = f.f.Load32(0x48); auto body = XStfsDevice::Lookup(f.f.kernel(), body_address);
  REQUIRE(body); REQUIRE_FALSE(body->retired());
  CHECK(xe::load_and_swap<uint32_t>(f.f.memory().TranslateVirtual(body->context_address())) == 0);
  // Kernel only allocates its genuine context; original native code must still
  // populate its own 971A header/context, which this low-level case does not run.
  X_STATUS status; auto member = f.OpenMember("FFFE07D1.gpd", filesystem::FileAccess::kFileReadData, status);
  REQUIRE(status == X_STATUS_SUCCESS); REQUIRE(member);
  std::array<uint8_t, 8> bytes{};
  REQUIRE(member->ReadSync(bytes, 0, &count) == X_STATUS_SUCCESS);
  REQUIRE(count == bytes.size()); REQUIRE(std::equal(bytes.begin(), bytes.end(), f.payload.begin()));
  auto refused = f.OpenMember("denied.bin", filesystem::FileAccess::kFileReadData, status);
  REQUIRE_FALSE(refused); REQUIRE(status == X_STATUS_ACCESS_DENIED); REQUIRE(f.denied->attempts == 1);
  const uint32_t output = f.f.allocation + 0x600;
  REQUIRE(f.f.Call(0x25A, {body_address, 3, output}) == X_STATUS_SUCCESS);
  CHECK(std::memcmp(f.f.memory().TranslateVirtual(output), body->descriptor().data(), 36) == 0);
  CHECK(f.f.Call(0x25A, {body_address, 2, 0}) == X_STATUS_INVALID_DEVICE_REQUEST);
  CHECK(original_file == f.raw->file()); CHECK(original_entry == f.raw->file()->entry());
  member.reset();
  REQUIRE(f.f.Call(0x3B, {body_address}) == X_STATUS_SUCCESS);
  REQUIRE(body->retired());
  CHECK(f.f.Call(0x25A, {body_address, 3, output}) == XStfsDevice::kDisconnected);
  f.CloseRawHandle();
  REQUIRE(f.f.kernel().content_manager()->RefreshManagedContentView("native-wire") == X_STATUS_SUCCESS);
  const auto fresh_content = f.f.fs().AcquireDevice("\\Device\\PirsVolume\\Content");
  REQUIRE(fresh_content); CHECK(fresh_content != old_content);
  CHECK(old_content->namespace_retiring());
  vfs::File* reopened = nullptr; vfs::FileAction action{};
  const auto refreshed = f.f.fs().OpenFile(nullptr, f.guest_package, vfs::FileDisposition::kOpen,
      filesystem::FileAccess::kFileReadData, false, true, &reopened, &action);
  FolderFile close_reopened(reopened);
  REQUIRE(refreshed == X_STATUS_SUCCESS); REQUIRE(close_reopened);
  CHECK(bool(close_reopened->folder_content_package()));
}
TEST_CASE("Native folder mount preparation releases its Header lease and refuses a replaced Header on later mounts",
          "[kernel][stfs][folder_con][source]") {
  FolderFixture f;
  REQUIRE(f.f.Create() == X_STATUS_SUCCESS);
  auto body = XStfsDevice::Lookup(f.f.kernel(), f.f.Load32(0x48)); REQUIRE(body);
  auto changed = f.header;
  auto* metadata = reinterpret_cast<vfs::XContentContainerHeader*>(changed.data());
  metadata->content_metadata.description_raw.uint[0][0] = u'X';
  REQUIRE_FALSE(xam::folder_content_detail::SignNativeHeader(changed, *f.f.kernel().console_signing_identity()));
  const auto committed = f.source.volume->mutations()->WriteMetadata(
      filesystem::ContentMutationOwner::Kind::kHeader, f.package, f.before, changed);
  REQUIRE_FALSE(committed.error); REQUIRE(committed.complete); REQUIRE(f.ActualHeader() == changed);
  // There was no permanently held Header read lease after the actual first
  // kernel publication. That old immutable File is not relabeled as new data.
  const std::string second = "\\Device\\NativeConOld";
  std::memcpy(f.f.memory().TranslateVirtual(f.f.allocation + 0x200), second.data(), second.size());
  f.f.Store16(0, uint16_t(second.size())); f.f.Store16(2, uint16_t(second.size()));
  f.f.Store32(0x48, 0xA1A2A3A4); f.f.Store32(0x4C, 0xB1B2B3B4);
  CHECK(XFAILED(f.f.Create()));
  CHECK(f.f.Load32(0x48) == 0xA1A2A3A4); CHECK(f.f.Load32(0x4C) == 0xB1B2B3B4);
  CHECK_FALSE(f.f.fs().AcquireDevice(second));
  std::vector<uint8_t> owned(f.NativeHeader().size()); size_t count = 0;
  REQUIRE(f.raw->file()->ReadSync(owned, 0, &count) == X_STATUS_SUCCESS);
  CHECK(std::equal(owned.begin(), owned.end(), f.NativeHeader().begin()));
  CHECK(f.ActualHeader() == changed);
  REQUIRE(body->Dismount() == X_STATUS_SUCCESS);
}
TEST_CASE("Native raw Header uses one issued stage and a stable File through genuine common CAS",
          "[kernel][stfs][folder_con][header_stage]") {
  FolderFixture f;
  auto* const file = f.raw->file(); const auto* const entry = file->entry();
  auto source = file->folder_content_package(); REQUIRE(source);
  std::error_code error;
  auto wrong_before = std::vector<uint8_t>(f.NativeHeader().begin(), f.NativeHeader().end());
  wrong_before[0x344] ^= 1;
  CHECK_FALSE(xam::NativeFolderContentTestAccess::CaptureHeader(*source, wrong_before, error));
  CHECK(error == std::errc::permission_denied); CHECK(f.ActualHeader() == f.header);
  // Even actual matching H bytes (or an unknown short sidecar tail) cannot
  // stand in for native ctx+0x98AC's complete retained aligned remainder.
  const auto h = sizeof(vfs::XContentContainerHeader) + FolderFixture::extra_header_bytes;
  CHECK_FALSE(xam::NativeFolderContentTestAccess::CaptureHeader(*source, f.NativeHeader().first(h), error));
  CHECK(error == std::errc::permission_denied);
  CHECK_FALSE(xam::NativeFolderContentTestAccess::CaptureHeader(*source, f.header, error));
  CHECK(error == std::errc::permission_denied); // full sidecar is not flattened raw context
  const auto captured = xam::NativeFolderContentTestAccess::CaptureHeader(*source, f.NativeHeader(), error);
  REQUIRE(captured); REQUIRE_FALSE(error);
  CHECK(captured->root() == f.source.volume->content());
  CHECK(captured->relative() == f.before->relative());
  CHECK(captured->info().file_id == f.before->info().file_id);
  REQUIRE(std::equal(captured->bytes().begin(), captured->bytes().end(), f.header.begin(), f.header.end()));
  auto profile = f.source.storage->Subroot(f.package, false, error); REQUIRE(profile); REQUIRE_FALSE(error);
  auto gpd = profile->CaptureSnapshot("FFFE07D1.gpd", 4096, error, true); REQUIRE(gpd); REQUIRE_FALSE(error);
  auto members = filesystem::SnapshotPublicationSet::Reserve(profile, {gpd}, error);
  REQUIRE(members); REQUIRE_FALSE(error);
  auto session = xam::NativeFolderContentTestAccess::Issue(*f.source.volume->mutations(),
      f.package, captured, members, error);
  REQUIRE(session); REQUIRE_FALSE(error);
  REQUIRE(xam::NativeFolderContentTestAccess::Adopt(*source, session, f.NativeHeader()) == X_STATUS_SUCCESS);
  CHECK_FALSE(xam::NativeFolderContentTestAccess::CaptureHeader(*source, f.NativeHeader(), error));
  CHECK(error == std::errc::device_or_resource_busy);
  auto changed = f.header;
  auto* body = reinterpret_cast<vfs::XContentContainerHeader*>(changed.data());
  body->content_metadata.thumbnail_size = 3;
  body->content_metadata.thumbnail[0] = 0x31;
  body->content_metadata.thumbnail[1] = 0x32;
  body->content_metadata.thumbnail[2] = 0x33;
  REQUIRE_FALSE(xam::folder_content_detail::SignNativeHeader(changed, *f.f.kernel().console_signing_identity()));
  size_t count = 0;
  SECTION("partial prefix keeps the actual old aligned remainder") {
    constexpr size_t dirty_end = 0x171D; // signature/hash + three thumbnail bytes
    REQUIRE(file->WriteSync(std::span<const uint8_t>(changed).first(dirty_end), 0, &count) == X_STATUS_SUCCESS);
    REQUIRE(count == dirty_end);
  }
  SECTION("full native extra-Header write includes its real alignment bytes") {
    REQUIRE(file->WriteSync(std::span<const uint8_t>(changed).first(0x971A), 0, &count) == X_STATUS_SUCCESS);
    REQUIRE(count == 0x971A);
    const auto remainder = std::span<const uint8_t>(changed).subspan(0x971A,
        FolderFixture::aligned_header_bytes - 0x971A);
    REQUIRE(file->WriteSync(remainder, 0x971A, &count) == X_STATUS_SUCCESS);
    REQUIRE(count == remainder.size());
  }
  const std::array<uint8_t, 1> unowned_tail{0};
  CHECK(file->WriteSync(unowned_tail, FolderFixture::aligned_header_bytes, &count) == X_STATUS_NOT_SUPPORTED);
  CHECK(count == 0);
  REQUIRE(file->Flush() == X_STATUS_SUCCESS);
  // Read through the issued original token, not a public-path bypass of its
  // genuine reservation. Complete public bytes remain original before CAS.
  REQUIRE(session->expected()->bytes().size() == f.header.size());
  REQUIRE(std::equal(f.header.begin(), f.header.end(), session->expected()->bytes().begin()));
  const auto sealed = xam::NativeFolderContentTestAccess::Seal(*session, error);
  REQUIRE(sealed); REQUIRE_FALSE(error); REQUIRE(*sealed == changed);
  const auto prepared = xam::NativeFolderContentTestAccess::PrepareCommit(*session, *members, error);
  REQUIRE(prepared); REQUIRE_FALSE(error);
  const auto stage_inode = prepared->stage_info().file_id;
  const auto committed = xam::NativeFolderContentTestAccess::Commit(*session, *members);
  REQUIRE_FALSE(committed.error); REQUIRE(committed.published); REQUIRE(committed.durable);
  REQUIRE(committed.current); CHECK(committed.current->info().file_id == stage_inode);
  // TestAccess exercises this boundary BELOW the genuine native cache issuer;
  // the real NativeProfileSave owner must supply its actual checkpoint first.
  REQUIRE(xam::NativeFolderContentTestAccess::Promote(*source, session, committed.current) == X_STATUS_SUCCESS);
  CHECK(file == f.raw->file()); CHECK(entry == f.raw->file()->entry());
  session.reset(); members.reset();
  auto ordinary = f.source.storage->OpenFile(f.package / "FFFE07D1.gpd",
      filesystem::FileAccess::kFileWriteData, error);
  REQUIRE(ordinary); REQUIRE_FALSE(error); ordinary.reset();
  // The cached File retains only independent Header bytes/layout pins, not
  // the released Header or GPD publication permits/admission/session.
  std::vector<uint8_t> read(FolderFixture::aligned_header_bytes);
  REQUIRE(file->ReadSync(read, 0, &count) == X_STATUS_SUCCESS);
  REQUIRE(std::equal(read.begin(), read.end(), changed.begin()));
  REQUIRE(f.ActualHeader() == changed); // CAS/promotion preserved opaque tail too
  REQUIRE(std::all_of(changed.begin() + FolderFixture::aligned_header_bytes,
                      changed.end(), [](uint8_t b) { return b == 0xA9; }));
}
TEST_CASE("CM folder raw source refuses missing or altered actual alignment bytes without zero fabrication",
          "[kernel][stfs][folder_con][native_header_alignment][source]") {
  FolderFixture f; f.CloseRawHandle();
  auto malformed = f.header;
  SECTION("H-only Header with an unclassified short sidecar tail") {
    malformed.resize(sizeof(vfs::XContentContainerHeader) + FolderFixture::extra_header_bytes + 11);
  }
  SECTION("actual padding changed without the corresponding native digest") {
    malformed[sizeof(vfs::XContentContainerHeader) + FolderFixture::extra_header_bytes + 1] ^= 1;
  }
  const auto written = f.source.volume->mutations()->WriteMetadata(
      filesystem::ContentMutationOwner::Kind::kHeader, f.package, f.before, malformed);
  REQUIRE_FALSE(written.error); REQUIRE(written.complete);
  vfs::File* raw = nullptr; vfs::FileAction action{};
  const auto status = f.f.fs().OpenFile(nullptr, f.guest_package, vfs::FileDisposition::kOpen,
      filesystem::FileAccess::kFileReadData, false, true, &raw, &action);
  FolderFile close(raw);
  CHECK(XFAILED(status)); CHECK_FALSE(close); CHECK(action == vfs::FileAction::kDoesNotExist);
  CHECK(f.ActualHeader() == malformed); // refusal never repairs or rewrites input
}
TEST_CASE("Native folder virtual extent includes structural hash thresholds and refuses unowned geometry",
          "[kernel][stfs][folder_con][virtual_extent]") {
  FolderFixture f;
  f.CloseRawHandle();
  uint32_t total = 170;
  uint64_t virtual_blocks = 174;
  bool single = false, invalid = false;
  enum class Refusal { kNone, kFree, kOverallocated, kCount, kTable, kDomain } refusal{};
  SECTION("paired tables include trailing L1 at170") {}
  SECTION("paired tables include trailing L2 at28900") { total = 28900; virtual_blocks = 29244; }
  SECTION("single-copy format uses the actual L0-only consumer") { single = true; virtual_blocks = 171; }
  SECTION("free-state ownership cannot be inferred from its count") { invalid = true; refusal = Refusal::kFree; }
  SECTION("directory overallocation needs its own real source") { invalid = true; refusal = Refusal::kOverallocated; }
  SECTION("declared blocks cannot enlarge the actual retained layout") { invalid = true; refusal = Refusal::kCount; }
  SECTION("a missing real native file table cannot be manufactured") { invalid = true; refusal = Refusal::kTable; }
  SECTION("unsupported top-level domain never adopts decoder fallback0") { invalid = true; refusal = Refusal::kDomain; }
  // One real sparse member resize supplies the exact retained logical sizes.
  // No payload copy/clone, fake FileHandle, header-only enlarged EOF or guest
  // output success supplies these real member identities and byte lengths.
  std::error_code error;
  {
    auto member = f.source.storage->OpenFile(f.package / "FFFE07D1.gpd",
        filesystem::FileAccess::kFileWriteData, error);
    REQUIRE(member); REQUIRE_FALSE(error);
    REQUIRE_FALSE(member->ResizeResult(size_t(total - 2) * 4096));
    REQUIRE_FALSE(member->FlushResult());
    const auto observed = member->QueryInfo(error);
    REQUIRE(observed); REQUIRE_FALSE(error); REQUIRE(observed->size == uint64_t(total - 2) * 4096);
  }
  auto changed = f.header;
  auto* parsed = reinterpret_cast<vfs::XContentContainerHeader*>(changed.data());
  auto& descriptor = parsed->content_metadata.volume_descriptor.stfs;
  descriptor.total_block_count = total;
  descriptor.flags.bits.read_only_format = single;
  parsed->content_metadata.content_size = virtual_blocks * 4096;
  switch (refusal) {
    case Refusal::kFree: descriptor.free_block_count = 1; break;
    case Refusal::kOverallocated: descriptor.flags.bits.directory_overallocated = 1; break;
    case Refusal::kCount: descriptor.total_block_count = total + 1; break;
    case Refusal::kTable: descriptor.file_table_block_count = 0; break;
    case Refusal::kDomain: descriptor.total_block_count = 4913000; break;
    default: break;
  }
  REQUIRE_FALSE(xam::folder_content_detail::SignNativeHeader(changed,
      *f.f.kernel().console_signing_identity()));
  const auto written = f.source.volume->mutations()->WriteMetadata(
      filesystem::ContentMutationOwner::Kind::kHeader, f.package, f.before, changed);
  REQUIRE_FALSE(written.error); REQUIRE(written.complete);
  vfs::File* raw = nullptr; vfs::FileAction action{};
  const auto opened = f.f.fs().OpenFile(nullptr, f.guest_package, vfs::FileDisposition::kOpen,
      filesystem::FileAccess::kFileReadData, false, true, &raw, &action);
  FolderFile close(raw);
  if (invalid) {
    CHECK(XFAILED(opened)); CHECK_FALSE(close); CHECK(action == vfs::FileAction::kDoesNotExist);
    CHECK(f.ActualHeader() == changed); return;
  }
  REQUIRE(opened == X_STATUS_SUCCESS); REQUIRE(close); CHECK(action == vfs::FileAction::kOpened);
  vfs::File::Information info{};
  REQUIRE(close->QueryInformation(&info) == X_STATUS_SUCCESS);
  CHECK(info.end_of_file == 0xA000 + virtual_blocks * 4096);
  // An owned virtual range is not an invented readable/hash-verified sector.
  std::array<uint8_t, 8> output{}; output.fill(0xA7); size_t count = 99;
  CHECK(close->ReadSync(output, 0xA000, &count) == X_STATUS_NOT_SUPPORTED);
  CHECK(count == 0); CHECK(std::all_of(output.begin(), output.end(), [](auto b) { return b == 0xA7; }));
  CHECK(close->ReadSync(output, size_t(info.end_of_file), &count) == X_STATUS_END_OF_FILE);
  CHECK(count == 0); CHECK(f.ActualHeader() == changed);
}
}  // namespace xe::kernel::test

#endif  // XE_PLATFORM_LINUX: actual protected local signing/storage backend
