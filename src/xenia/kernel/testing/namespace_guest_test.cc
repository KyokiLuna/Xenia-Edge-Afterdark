/** Xenia : Xbox 360 Emulator Research Project; BSD license, see LICENSE. */
#include <array>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include "third_party/catch/include/catch.hpp"
#include "xenia/kernel/util/guest_io_buffer.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xboxkrnl/ob_symbolic_link.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_module.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_private.h"
#include "xenia/memory.h"
#include "xenia/vfs/devices/host_path_device.h"
#include "xenia/vfs/virtual_file_system.h"
namespace xe::kernel::xboxkrnl::test {
namespace {
struct Fixture {
  static constexpr uint32_t base = 0x90028000, oa = base, input = base + 0x40,
                            input_bytes = base + 0x200, output = base + 0x1100,
                            output_bytes = base + 0x2000;
  Memory memory;
  vfs::VirtualFileSystem fs;
  Fixture() {
    REQUIRE(memory.Initialize());
    auto* heap = memory.LookupHeap(base);
    REQUIRE(heap->AllocFixed(base - 0x1000, 0x5000, 0x1000,
                             kMemoryAllocationReserve,
                             kMemoryProtectRead | kMemoryProtectWrite));
    REQUIRE(heap->AllocFixed(base, 0x3000, 0x1000, kMemoryAllocationCommit,
                             kMemoryProtectRead | kMemoryProtectWrite));
    std::memset(memory.TranslateVirtual(base), 0xA5, 0x3000);
    Word(oa, 0);
    Word(oa + 4, input);
    Word(oa + 8, 0x40);
    Name("\\Device\\Missing\\package");
    Half(output, 0xBEEF);
    Half(output + 2, 0x104);
    Word(output + 4, output_bytes);
  }
  void Word(uint32_t at, uint32_t value) {
    xe::store_and_swap<uint32_t>(memory.TranslateVirtual(at), value);
  }
  void Half(uint32_t at, uint16_t value) {
    xe::store_and_swap<uint16_t>(memory.TranslateVirtual(at), value);
  }
  void Name(std::string_view name) {
    Half(input, uint16_t(name.size()));
    Half(input + 2, uint16_t(name.size() + 1));
    Word(input + 4, input_bytes);
    std::memcpy(memory.TranslateVirtual(input_bytes), name.data(), name.size());
  }
  X_STATUS Call(const SymbolicLinkRootResolver& root = {}) {
    return TranslateSymbolicLinkGuest(memory, fs, oa, output, root);
  }
  std::array<uint8_t, 0x3000> Before() {
    std::array<uint8_t, 0x3000> b;
    std::memcpy(b.data(), memory.TranslateVirtual(base), b.size());
    return b;
  }
};
}  // namespace
TEST_CASE("Kernel113 publishes counted ANSI payload and length without a NUL",
          "[kernel][namespace][abi]") {
  Fixture f;
  const std::string expected = "\\Device\\Missing\\package";
  REQUIRE(f.memory.LookupHeap(Fixture::base)
              ->Protect(Fixture::base, 0x1000, kMemoryProtectRead));
  REQUIRE(f.Call() == X_STATUS_SUCCESS);
  CHECK(xe::load_and_swap<uint16_t>(
            f.memory.TranslateVirtual(Fixture::output)) == expected.size());
  CHECK(xe::load_and_swap<uint16_t>(
            f.memory.TranslateVirtual(Fixture::output + 2)) == 0x104);
  CHECK(xe::load_and_swap<uint32_t>(f.memory.TranslateVirtual(
            Fixture::output + 4)) == Fixture::output_bytes);
  CHECK(std::memcmp(f.memory.TranslateVirtual(Fixture::output_bytes),
                    expected.data(), expected.size()) == 0);
  CHECK(f.memory.TranslateVirtual(Fixture::output_bytes)[expected.size()] ==
        0xA5);
  CHECK(f.memory.TranslateVirtual(Fixture::output)[-1] == 0xA5);
  CHECK(f.memory.TranslateVirtual(Fixture::output)[8] == 0xA5);
}
TEST_CASE(
    "Kernel113 DOS aliases and in-place payload use owned input snapshots",
    "[kernel][namespace][abi]") {
  Fixture f;
  f.Word(Fixture::oa, 0xFFFFFFFD);
  f.Name("\\??\\DATA:\\file");
  REQUIRE(f.fs.RegisterSymbolicLink("data:", "\\Device\\Disk"));
  f.Word(Fixture::output + 4, Fixture::input_bytes);
  REQUIRE(f.Call() == X_STATUS_SUCCESS);
  const std::string expected = "\\Device\\Disk\\file";
  CHECK(std::memcmp(f.memory.TranslateVirtual(Fixture::input_bytes),
                    expected.data(), expected.size()) == 0);
}
TEST_CASE("Kernel113 rejected shapes preserve every output byte",
          "[kernel][namespace][abi]") {
  Fixture f;
  SECTION("capacity") { f.Half(Fixture::output + 2, 2); }
  SECTION("readonly descriptor") {
    REQUIRE(f.memory.LookupHeap(Fixture::output)
                ->Protect(Fixture::base + 0x1000, 0x1000, kMemoryProtectRead));
  }
  SECTION("readonly payload") {
    REQUIRE(f.memory.LookupHeap(Fixture::output)
                ->Protect(Fixture::output_bytes, 0x1000, kMemoryProtectRead));
  }
  SECTION("output guard") {
    f.Word(Fixture::output + 4, Fixture::base + 0x2FFF);
  }
  SECTION("input guard") { f.Word(Fixture::input + 4, Fixture::base + 0x2FFF); }
  SECTION("invalid flags") { f.Word(Fixture::oa + 8, 0x40000040); }
  SECTION("overlapping output descriptor") {
    f.Word(Fixture::output + 4, Fixture::output + 4);
  }
  SECTION("overlapping input descriptor") {
    f.Word(Fixture::output + 4, Fixture::input);
  }
  SECTION("invalid count") { f.Half(Fixture::input + 2, 0); }
  SECTION("embedded NUL") { f.Name(std::string("bad\0name", 8)); }
  SECTION("malformed counted ANSI byte") { f.Name(std::string("\xFF", 1)); }
  SECTION("truncated UTF8 counted ANSI") { f.Name(std::string("\xE2\x82", 2)); }
  const auto before = f.Before();
  CHECK(XFAILED(f.Call()));
  CHECK(f.Before() == before);
}
TEST_CASE("Kernel113 relative root retains a real file and device tree",
          "[kernel][namespace][ownership]") {
  Fixture f;
  const auto path =
      std::filesystem::temp_directory_path() /
      ("xenia-namespace-root-" +
       std::to_string(
           std::chrono::steady_clock::now().time_since_epoch().count()));
  REQUIRE(std::filesystem::create_directory(path));
  std::ofstream(path / "item", std::ios::binary) << "x";
  struct Cleanup {
    std::filesystem::path path;
    ~Cleanup() {
      std::error_code e;
      std::filesystem::remove_all(path, e);
    }
  } cleanup{path};
  auto device =
      std::make_unique<vfs::HostPathDevice>("\\Device\\Root", path, true);
  REQUIRE(device->Initialize());
  REQUIRE(f.fs.RegisterDevice(std::move(device)));
  auto* entry = f.fs.ResolvePath("\\Device\\Root");
  REQUIRE(entry);
  auto tree = entry->device()->shared_from_this();
  vfs::File* raw = nullptr;
  REQUIRE(entry->Open(vfs::FileAccess::kFileReadData, &raw) ==
          X_STATUS_SUCCESS);
  std::shared_ptr<vfs::File> file(raw, [](auto* p) { p->Destroy(); });
  struct Owner {
    std::shared_ptr<vfs::Device> tree;
    std::shared_ptr<vfs::File> file;
  };
  std::weak_ptr<Owner> lifetime;
  SymbolicLinkRootResolver root = [&](uint32_t handle,
                                      RetainedSymbolicLinkRoot& output) {
    if (handle != 0x1234) {
      return X_STATUS_INVALID_HANDLE;
    }
    auto owner = std::make_shared<Owner>(Owner{tree, file});
    lifetime = owner;
    output = {
        file->entry()->absolute_path(), owner,
        (file->entry()->attributes() & vfs::kFileAttributeDirectory) != 0};
    return X_STATUS_SUCCESS;
  };
  f.Word(Fixture::oa, 0x1234);
  f.Name("future");
  REQUIRE(f.Call(root) == X_STATUS_SUCCESS);
  CHECK(lifetime.expired());
  const std::string expected = "\\Device\\Root\\future";
  CHECK(std::memcmp(f.memory.TranslateVirtual(Fixture::output_bytes),
                    expected.data(), expected.size()) == 0);
  SECTION("A counted empty name reopens the real root") {
    f.Name("");
    REQUIRE(f.Call(root) == X_STATUS_SUCCESS);
    CHECK(xe::load_and_swap<uint16_t>(f.memory.TranslateVirtual(
              Fixture::output)) == std::string("\\Device\\Root").size());
  }
  SECTION("A null name reopens the real root") {
    f.Word(Fixture::oa + 4, 0);
    REQUIRE(f.Call(root) == X_STATUS_SUCCESS);
  }
  SECTION("A regular-file root cannot receive a relative child") {
    vfs::File* raw_item = nullptr;
    auto* item = f.fs.ResolvePath("\\Device\\Root\\item");
    REQUIRE(item);
    REQUIRE(item->Open(vfs::FileAccess::kFileReadData, &raw_item) ==
            X_STATUS_SUCCESS);
    file = std::shared_ptr<vfs::File>(raw_item, [](auto* p) { p->Destroy(); });
    const auto before = f.Before();
    CHECK(f.Call(root) == X_STATUS_NOT_A_DIRECTORY);
    CHECK(f.Before() == before);
  }
  SECTION("A relative root cannot turn malformed bytes into a host exception") {
    f.Name(std::string("\xFF", 1));
    const auto before = f.Before();
    CHECK(f.Call(root) == X_STATUS_OBJECT_NAME_INVALID);
    CHECK(f.Before() == before);
  }
  f.Word(Fixture::oa, 0x9999);
  const auto before = f.Before();
  CHECK(f.Call(root) == X_STATUS_INVALID_HANDLE);
  CHECK(f.Before() == before);
  file.reset();
  tree.reset();
  f.fs.Clear();
}
TEST_CASE("Kernel113 is registered as the actual two-pointer export",
          "[kernel][namespace][abi]") {
  STATIC_REQUIRE(sizeof(X_OBJECT_ATTRIBUTES) == 12);
  STATIC_REQUIRE(offsetof(X_OBJECT_ATTRIBUTES, attributes) == 8);
  STATIC_REQUIRE(sizeof(X_ANSI_STRING) == 8);
  Fixture f;
  cpu::ExportResolver resolver;
  RegisterObExports(&resolver, nullptr);
  XboxkrnlModule::RegisterExportTable(&resolver);
  const auto* entry = resolver.GetExportByOrdinal("xboxkrnl.exe", 0x113);
  REQUIRE(entry);
  REQUIRE(entry->is_implemented());
  CHECK((entry->tags & cpu::ExportTag::kStub) == 0);
  REQUIRE(entry->function_data.trampoline);
  PPCContext context{};
  context.virtual_membase = f.memory.virtual_membase();
  context.r[3] = Fixture::oa;
  context.r[4] = Fixture::output;
  context.r[5] = 0xFEDCBA9876543210ull;
  const auto before = f.Before();
  entry->function_data.trampoline(&context);
  CHECK(uint32_t(context.r[3]) == X_STATUS_INVALID_PARAMETER);
  CHECK(context.r[4] == Fixture::output);
  CHECK(context.r[5] == 0xFEDCBA9876543210ull);
  CHECK(f.Before() == before);
}
}  // namespace xe::kernel::xboxkrnl::test
