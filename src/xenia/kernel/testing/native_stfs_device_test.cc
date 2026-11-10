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
#include "xenia/kernel/user_module.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xfile.h"
#include "xenia/kernel/xstfs_device.h"
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
          bool nested_package = false) {
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
      std::ofstream out(scratch.path / "packages" / "package", std::ios::binary);
      out.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
      REQUIRE(out.good());
    }
    emulator = std::make_unique<Emulator>("", scratch.path,
        scratch.path / "content", scratch.path / "cache");
    REQUIRE(emulator->Setup(nullptr, nullptr, false, {}, {}, {}) ==
            X_STATUS_SUCCESS);
    auto source = std::make_unique<vfs::HostPathDevice>(
        "\\Device\\PirsSource", scratch.path / "packages", readonly);
    REQUIRE(source->Initialize());
    REQUIRE(fs().RegisterDevice(std::move(source)));
    auto* entry = fs().ResolvePath("\\Device\\PirsSource\\package");
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
