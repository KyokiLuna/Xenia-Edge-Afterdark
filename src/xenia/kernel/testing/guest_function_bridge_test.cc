/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia contributors. All rights reserved.                    *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <algorithm>
#include <array>
#include <cstdint>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>
#include <utility>

#include "third_party/catch/include/catch.hpp"
#include "third_party/mbedtls/include/mbedtls/sha1.h"
#include "third_party/mbedtls/include/mbedtls/sha256.h"
#include "xenia/base/byte_order.h"
#include "xenia/cpu/export_resolver.h"
#include "xenia/cpu/testing/util.h"
#include "xenia/cpu/xex_module.h"
#include "xenia/emulator.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/kernel.h"
#include "xenia/kernel/testing/isolated_kernel_runtime_settings.h"
#include "xenia/kernel/user_module.h"
#include "xenia/kernel/util/xex_guest_header.h"
#include "xenia/kernel/xmodule.h"
#include "xenia/vfs/devices/host_path_device.h"
#include "xenia/vfs/virtual_file_system.h"

namespace xe::cpu::test {
namespace {

constexpr uint32_t kPageSize = 0x10000;
constexpr uint32_t kHeaderSize = 0x1000;
constexpr uint32_t kImageBase = 0x82000000;
constexpr uint32_t kCode = kImageBase + kPageSize;
constexpr uint32_t kData = kCode + kPageSize;
constexpr uint32_t kImageSize = 3 * kPageSize;
constexpr uint32_t kTarget = kCode + 0x100;
constexpr uint32_t kTargetSize = 8;
constexpr uint32_t kCaller = kCode + 0x200;
constexpr uint32_t kTailCaller = kCode + 0x300;
constexpr uint32_t kTailReturn = kCode + 0x400;
constexpr uint32_t kExports = kData + 0x100;
constexpr uint32_t kTaskSchedule = kCode + 0x500;
constexpr uint32_t kTaskIndirect = kCode + 0x600;
constexpr uint32_t kTaskRegistrationSize = 9 * 4;
constexpr uint32_t kStaticMarker = 0xA500;
constexpr uint32_t kExportMarker = 0xB600;
constexpr std::string_view kHostLibrary = "bridge-test.xex";

void WriteBe(std::vector<uint8_t>& bytes, size_t offset, uint32_t value) {
  xe::store_and_swap<uint32_t>(bytes.data() + offset, value);
}

void WriteLe16(std::vector<uint8_t>& bytes, size_t offset, uint16_t value) {
  bytes[offset] = uint8_t(value);
  bytes[offset + 1] = uint8_t(value >> 8);
}

void WriteLe32(std::vector<uint8_t>& bytes, size_t offset, uint32_t value) {
  for (size_t i = 0; i < 4; ++i) {
    bytes[offset + i] = uint8_t(value >> (8 * i));
  }
}

void ImageWord(std::vector<uint8_t>& image, uint32_t address, uint32_t value) {
  WriteBe(image, address - kImageBase, value);
}

uint32_t Branch(uint32_t from, uint32_t to, bool link) {
  return 0x48000000 | ((to - from) & 0x03FFFFFC) | uint32_t(link);
}

std::string Sha256(const uint8_t* bytes, size_t size) {
  std::array<uint8_t, 32> digest;
  mbedtls_sha256(bytes, size, digest.data(), 0);
  constexpr char kHex[] = "0123456789abcdef";
  std::string result;
  result.reserve(64);
  for (uint8_t byte : digest) {
    result.push_back(kHex[byte >> 4]);
    result.push_back(kHex[byte & 15]);
  }
  return result;
}

using ImageCustomization = std::function<void(std::vector<uint8_t>&)>;

// The unencrypted, no-import image follows tools/xex/xexbuild.py's layout.
// All bytes, including the PE and chained page/header hashes, are generated
// here. No console image, filesystem, KernelState, or fake executor is used.
std::vector<uint8_t> MakeXex(const ImageCustomization& customize,
                           const ImageCustomization& customize_header = {}) {
  std::vector<uint8_t> image(kImageSize);
  image[0] = 'M';
  image[1] = 'Z';
  image[2] = 0x90;
  WriteLe32(image, 0x3C, 0x80);
  std::memcpy(image.data() + 0x80, "PE\0\0", 4);
  WriteLe16(image, 0x84, 0x1F2);  // POWERPCBE.
  WriteLe16(image, 0x86, 2);      // .text and .data.
  WriteLe16(image, 0x94, 0xE0);
  WriteLe16(image, 0x96, 0x102);  // Executable, 32-bit.
  constexpr size_t kOptional = 0x98;
  WriteLe16(image, kOptional, 0x10B);
  WriteLe32(image, kOptional + 4, kPageSize);
  WriteLe32(image, kOptional + 8, kPageSize);
  WriteLe32(image, kOptional + 0x10, kCaller - kImageBase);
  WriteLe32(image, kOptional + 0x14, kCode - kImageBase);
  WriteLe32(image, kOptional + 0x18, kData - kImageBase);
  WriteLe32(image, kOptional + 0x1C, kImageBase);
  WriteLe32(image, kOptional + 0x20, kPageSize);
  WriteLe32(image, kOptional + 0x24, 0x200);
  WriteLe32(image, kOptional + 0x38, kImageSize);
  WriteLe32(image, kOptional + 0x3C, 0x400);
  WriteLe16(image, kOptional + 0x44, 0xE);  // Xbox subsystem.
  WriteLe32(image, kOptional + 0x48, 0x40000);
  WriteLe32(image, kOptional + 0x4C, 0x40000);
  WriteLe32(image, kOptional + 0x50, 0x100000);
  WriteLe32(image, kOptional + 0x54, 0x1000);
  WriteLe32(image, kOptional + 0x5C, 16);
  const auto section = [&](size_t offset, const char* name, uint32_t rva,
                           uint32_t flags) {
    std::memcpy(image.data() + offset, name, std::strlen(name));
    WriteLe32(image, offset + 8, kPageSize);
    WriteLe32(image, offset + 12, rva);
    WriteLe32(image, offset + 16, kPageSize);
    WriteLe32(image, offset + 20, rva);
    WriteLe32(image, offset + 36, flags);
  };
  section(kOptional + 0xE0, ".text", kCode - kImageBase, 0x60000020);
  section(kOptional + 0xE0 + 40, ".data", kData - kImageBase, 0xC0000040);

  ImageWord(image, kTarget, 0x38600007);      // li r3,7 (unbridged result).
  ImageWord(image, kTarget + 4, 0x4E800020);  // blr.
  ImageWord(image, kCaller, 0x7C0802A6);      // mflr r0.
  ImageWord(image, kCaller + 4, 0x9001FFF8);  // stw r0,-8(r1).
  ImageWord(image, kCaller + 8, Branch(kCaller + 8, kTarget, true));
  ImageWord(image, kCaller + 12, 0x38630001);  // addi r3,r3,1.
  ImageWord(image, kCaller + 16, 0x8001FFF8);  // lwz r0,-8(r1).
  ImageWord(image, kCaller + 20, 0x7C0803A6);  // mtlr r0.
  ImageWord(image, kCaller + 24, 0x4E800020);
  ImageWord(image, kTailCaller, Branch(kTailCaller, kTarget, false));
  ImageWord(image, kTailReturn, 0x4E800020);
  ImageWord(image, kExports + 0x20, kImageBase >> 16);
  ImageWord(image, kExports + 0x24, 1);
  ImageWord(image, kExports + 0x28, 1);
  ImageWord(image, kExports + 0x2C, kTarget - kImageBase);
  if (customize) {
    customize(image);
  }

  std::vector<uint8_t> xex(kHeaderSize + image.size());
  std::copy(image.begin(), image.end(), xex.begin() + kHeaderSize);
  constexpr size_t kSecurity = 0x100;
  constexpr size_t kDescriptors = kSecurity + 0x184;
  WriteBe(xex, 0, 0x58455832);
  WriteBe(xex, 4, XEX_MODULE_TITLE);
  WriteBe(xex, 8, kHeaderSize);
  WriteBe(xex, 0x10, kSecurity);
  WriteBe(xex, 0x14, 4);
  constexpr std::array<uint32_t, 4> kKeys = {
      XEX_HEADER_FILE_FORMAT_INFO, XEX_HEADER_ENTRY_POINT,
      XEX_HEADER_IMAGE_BASE_ADDRESS, XEX_HEADER_DEFAULT_STACK_SIZE};
  constexpr std::array<uint32_t, 4> kValues = {0x40, kCaller, kImageBase,
                                               0x40000};
  for (size_t i = 0; i < kKeys.size(); ++i) {
    WriteBe(xex, 0x18 + i * 8, kKeys[i]);
    WriteBe(xex, 0x1C + i * 8, kValues[i]);
  }
  WriteBe(xex, 0x40, 8);  // File-format size, encryption/compression both NONE.
  WriteBe(xex, kSecurity, 0x184 + 3 * 24);
  WriteBe(xex, kSecurity + 4, kImageSize);
  WriteBe(xex, kSecurity + 0x108, 0x174);
  WriteBe(xex, kSecurity + 0x110, kImageBase);
  WriteBe(xex, kSecurity + 0x160, kExports);
  WriteBe(xex, kSecurity + 0x178, 0xFFFFFFFF);
  WriteBe(xex, kSecurity + 0x17C, 0xFFFFFFFF);
  WriteBe(xex, kSecurity + 0x180, 3);
  constexpr std::array<uint32_t, 3> kTypes = {
      XEX_SECTION_READONLY_DATA, XEX_SECTION_CODE, XEX_SECTION_DATA};
  for (size_t i = 0; i < kTypes.size(); ++i) {
    WriteBe(xex, kDescriptors + i * 24, 0x10 | kTypes[i]);
  }
  const auto page_digest = [&](size_t page, size_t descriptor, uint8_t* out) {
    std::vector<uint8_t> bytes(image.begin() + page * kPageSize,
                               image.begin() + (page + 1) * kPageSize);
    bytes.insert(bytes.end(), xex.begin() + descriptor,
                 xex.begin() + descriptor + 24);
    mbedtls_sha1(bytes.data(), bytes.size(), out);
  };
  for (size_t i = 2; i > 0; --i) {
    page_digest(i, kDescriptors + i * 24,
                xex.data() + kDescriptors + (i - 1) * 24 + 4);
  }
  page_digest(0, kDescriptors, xex.data() + kSecurity + 0x114);
  // Optional fixture metadata is generated before its REAL header digest.
  // Default callers retain identical bytes; this is not a patch/TU executor.
  if (customize_header) {
    customize_header(xex);
  }
  std::vector<uint8_t> header_digest(xex.begin() + kSecurity + 0x17C,
                                     xex.begin() + kHeaderSize);
  header_digest.insert(header_digest.end(), xex.begin(),
                       xex.begin() + kSecurity + 8);
  mbedtls_sha1(header_digest.data(), header_digest.size(),
               xex.data() + kSecurity + 0x164);
  return xex;
}

void StaticHandler(ppc::PPCContext* context, kernel::KernelState*) {
  context->r[3] =
      kStaticMarker + uint32_t(context->r[3]) + uint32_t(context->r[4]);
  ++context->r[9];
}

void ExportHandler(ppc::PPCContext* context) {
  context->r[3] =
      kExportMarker + uint32_t(context->r[3]) + uint32_t(context->r[4]);
  ++context->r[9];
}

// A second owner claiming the same code tests identity rather than merely
// whether the requested address happens to lie inside this XEX's allocation.
class ForeignOwner final : public Module {
 public:
  explicit ForeignOwner(Processor* processor) : Module(processor) {}
  const std::string& name() const override { return name_; }
  bool is_executable() const override { return false; }
  bool ContainsAddress(uint32_t address) override {
    return address >= kCode && address < kData;
  }

 protected:
  std::unique_ptr<Function> CreateFunction(uint32_t address) override {
    return std::unique_ptr<Function>(
        processor_->backend()->CreateGuestFunction(this, address));
  }

 private:
  const std::string name_ = "foreign-owner";
};

class BridgeFixture {
 public:
  explicit BridgeFixture(const ImageCustomization& customize = {},
                         bool foreign_owner = false) {
    REQUIRE(memory.Initialize());
    callable.function_data.trampoline = ExportHandler;
    variable.variable_ptr = kData;
    undefined.function_data.trampoline = ExportHandler;
    exports = {nullptr, &callable, &variable, &undefined, &null_handler};
    resolver.RegisterTable(kHostLibrary, &exports);
    processor = std::make_unique<Processor>(&memory, &resolver);
    auto backend = testing::CreateBackend();
    REQUIRE(backend);
    REQUIRE(processor->Setup(std::move(backend)));
    if (foreign_owner) {
      REQUIRE(processor->AddModule(
          std::make_unique<ForeignOwner>(processor.get())));
    }
    auto file = MakeXex(customize);
    original_image.assign(file.begin() + kHeaderSize, file.end());
    original_image_hash = Sha256(original_image.data(), original_image.size());
    auto image_module = std::make_unique<XexModule>(processor.get(), nullptr);
    module = image_module.get();
    REQUIRE(module->Load("bridge-fixture.xex", "bridge-fixture.xex",
                         file.data(), file.size()));
    REQUIRE(module->LoadContinue());
    REQUIRE(processor->AddModule(std::move(image_module)));
    stack = memory.SystemHeapAlloc(kStackSize);
    REQUIRE(stack != 0);
    thread = std::make_unique<ThreadState>(processor.get(), 0x100,
                                           stack + kStackSize);
    CheckGuestBytes();
  }

  ~BridgeFixture() {
    thread.reset();
    if (stack) {
      memory.SystemHeapFree(stack);
    }
    if (module) {
      module->Unload();
    }
    processor.reset();
  }

  std::string Hash(uint32_t address = kTarget,
                   uint32_t size = kTargetSize) const {
    return Sha256(memory.TranslateVirtual<const uint8_t*>(address), size);
  }
  bool BindStatic(uint32_t address = kTarget, uint32_t size = kTargetSize,
                  std::string_view hash = {}) {
    const std::string expected =
        hash.empty() ? Hash(address, size) : std::string(hash);
    return module->BindGuestFunctionToHost(address, size, expected,
                                           StaticHandler, "StaticHandler");
  }
  void CheckGuestBytes() const {
    const auto* live = memory.TranslateVirtual<const uint8_t*>(kImageBase);
    CHECK(std::memcmp(live, original_image.data(), original_image.size()) == 0);
    CHECK(Sha256(live, original_image.size()) == original_image_hash);
  }
  void CheckNoOverlay() const {
    CHECK(module->TranslateCode(kTarget) ==
          memory.TranslateVirtual<const uint8_t*>(kTarget));
    CheckGuestBytes();
  }
  uint32_t Call(uint32_t address, uint32_t a = 11, uint32_t b = 23) {
    auto* context = thread->context();
    context->r[3] = a;
    context->r[4] = b;
    context->r[9] = 0;
    REQUIRE(processor->Execute(thread.get(), address));
    return uint32_t(context->r[3]);
  }

  Memory memory;
  ExportResolver resolver;
  Export callable{1, Export::Type::kFunction, "ExportHandler",
                  ExportTag::kImplemented};
  Export variable{2, Export::Type::kVariable, "Variable",
                  ExportTag::kImplemented};
  Export undefined{3, Export::Type::kFunction, "Undefined"};
  Export null_handler{4, Export::Type::kFunction, "NullHandler",
                      ExportTag::kImplemented};
  std::vector<Export*> exports;
  std::unique_ptr<Processor> processor;
  XexModule* module = nullptr;
  static constexpr uint32_t kStackSize = 64 * 1024;
  uint32_t stack = 0;
  std::unique_ptr<ThreadState> thread;
  std::vector<uint8_t> original_image;
  std::string original_image_hash;
};

// This is a generated real XEX/PE/PowerPC fixture for bridge installation
// and dispatch, not an implementation or simulation of native Task services.
void TaskCallbackImage(std::vector<uint8_t>& image) {
  // Replace the ordinary direct caller with the version-verified native
  // registration shape. There is deliberately NO direct BL to kTarget.
  ImageWord(image, kCaller, 0x39610058);  // addi r11,r1,args
  ImageWord(image, kCaller + 4,
            0x3D400000 | uint16_t((kTarget + 0x8000) >> 16));
  ImageWord(image, kCaller + 8, 0x38C10050);   // addi r6,r1,out
  ImageWord(image, kCaller + 12, 0x38A10058);  // addi r5,r1,args
  ImageWord(image, kCaller + 16, 0x38800000);  // li r4,0
  ImageWord(image, kCaller + 20, 0xFBEB0000);  // std r31,0(r11)
  ImageWord(image, kCaller + 24, 0x93C10058);  // stw r30,args(r1)
  ImageWord(image, kCaller + 28, 0x386A0000 | uint16_t(kTarget));
  ImageWord(image, kCaller + 32, Branch(kCaller + 32, kTaskSchedule, true));
  ImageWord(image, kTaskSchedule, 0x38600000);
  ImageWord(image, kTaskSchedule + 4, 0x4E800020);
  constexpr size_t kDataSection = 0x98 + 0xE0 + 40;
  std::memset(image.data() + kDataSection, 0, 8);
  std::memcpy(image.data() + kDataSection, ".pdata", 6);
  WriteLe32(image, kDataSection + 8, 16);
  WriteLe32(image, kDataSection + 16, 16);
  ImageWord(image, kData, kTarget);
  ImageWord(image, kData + 4, 0x40000203);  // Full two-instruction extent.
  ImageWord(image, kData + 8, kTaskSchedule);
  ImageWord(image, kData + 12, 0x40000203);
  ImageWord(image, kTaskIndirect, 0x7C0802A6);  // mflr r0
  ImageWord(image, kTaskIndirect + 4, 0x9001FFF8);
  ImageWord(image, kTaskIndirect + 8,
            0x3D800000 | uint16_t((kTarget + 0x8000) >> 16));
  ImageWord(image, kTaskIndirect + 12, 0x398C0000 | uint16_t(kTarget));
  ImageWord(image, kTaskIndirect + 16, 0x7D8903A6);  // mtctr r12
  ImageWord(image, kTaskIndirect + 20, 0x4E800421);  // bctrl
  ImageWord(image, kTaskIndirect + 24, 0x8001FFF8);
  ImageWord(image, kTaskIndirect + 28, 0x7C0803A6);
  ImageWord(image, kTaskIndirect + 32, 0x4E800020);
}

using TaskWitness = XexModule::GuestTaskCallbackRegistration;
bool BindTaskCallback(BridgeFixture& fixture,
                      const std::function<void(TaskWitness&)>& customize = {}) {
  const auto registration_hash = fixture.Hash(kCaller, kTaskRegistrationSize);
  const auto schedule_hash = fixture.Hash(kTaskSchedule, 8);
  TaskWitness witness{
      kCaller, kTaskRegistrationSize, registration_hash, kTaskSchedule,
      8,       schedule_hash};
  if (customize) {
    customize(witness);
  }
  return fixture.module->BindGuestTaskCallbackToHost(
      kTarget, kTargetSize, fixture.Hash(), witness, StaticHandler,
      "TaskContinuation");
}

TEST_CASE("Task callback bridge accepts owned PE entry and native registration",
          "[kernel][guest_function_bridge]") {
  BridgeFixture fixture(TaskCallbackImage);
  CHECK_FALSE(
      fixture.BindStatic());  // Direct-BL API retains its stricter rule.
  fixture.CheckNoOverlay();
  REQUIRE(BindTaskCallback(fixture));
  CHECK(fixture.Call(kTaskIndirect) == kStaticMarker + 11 + 23);
  CHECK(fixture.thread->context()->r[9] == 1);
  fixture.CheckGuestBytes();
  Function* interior = nullptr;
  CHECK(fixture.module->DeclareFunction(kTarget + 4, &interior) ==
        Symbol::Status::kFailed);
  CHECK(interior == nullptr);
}

TEST_CASE("Task callback bridge rejects absent or mismatched registration",
          "[kernel][guest_function_bridge]") {
  const auto variant = GENERATE(0, 1, 2, 3, 4, 5, 6, 7);
  CAPTURE(variant);
  BridgeFixture fixture([variant](std::vector<uint8_t>& image) {
    TaskCallbackImage(image);
    if (variant == 0) {
      ImageWord(image, kCaller + 28, 0x386A0000 | uint16_t(kTarget + 4));
    } else if (variant == 1) {
      ImageWord(image, kCaller + 32,
                Branch(kCaller + 32, kTaskSchedule + 4, true));
    } else if (variant == 2) {
      ImageWord(image, kCaller + 32,
                Branch(kCaller + 32, kTaskSchedule, false));
    } else if (variant == 3) {
      // Even a freshly hashed witness cannot use a clobbered callback high
      // half.
      ImageWord(image, kCaller + 24, 0x39400000);  // li r10,0
    }
  });
  CHECK_FALSE(BindTaskCallback(fixture, [variant](TaskWitness& witness) {
    if (variant == 4) {
      witness.registration_size = 0;
    } else if (variant == 5) {
      witness.registration_sha256 = std::string_view{};
    } else if (variant == 6) {
      witness.schedule_sha256 = std::string_view{};
    } else if (variant == 7) {
      witness.registration_address += 4;
    }
  }));
  CHECK(fixture.module->LookupSymbol(kTarget, false) == nullptr);
  fixture.CheckNoOverlay();
}

TEST_CASE("Task callback bridge requires exact complete runtime entries",
          "[kernel][guest_function_bridge]") {
  const auto variant = GENERATE(0, 1, 2, 3, 4, 5, 6, 7);
  CAPTURE(variant);
  BridgeFixture fixture([variant](std::vector<uint8_t>& image) {
    TaskCallbackImage(image);
    if (variant == 0) {
      constexpr size_t kDataSection = 0x98 + 0xE0 + 40;
      std::memcpy(image.data() + kDataSection, ".data", 6);
    } else if (variant == 1) {
      ImageWord(image, kData, kTarget - 4);
    } else if (variant == 2) {
      ImageWord(image, kData + 4, 0x40000103);  // Truncated callback extent.
    } else if (variant == 3) {
      ImageWord(image, kData + 8, kTaskSchedule + 4);
    } else if (variant == 4) {
      ImageWord(image, kData + 12, 0x40000103);  // Truncated scheduler extent.
    } else if (variant == 5) {
      ImageWord(image, kTailCaller, Branch(kTailCaller, kTarget + 4, false));
    } else {
      constexpr size_t kDataSection = 0x98 + 0xE0 + 40;
      WriteLe32(image, kDataSection + 8, 24);
      WriteLe32(image, kDataSection + 16, 24);
      ImageWord(image, kData + 16,
                (variant == 6 ? kTarget : kTaskSchedule) - 4);
      ImageWord(image, kData + 20, 0x40000403);  // Prior overlapping interval.
    }
  });
  CHECK_FALSE(BindTaskCallback(fixture));
  CHECK(fixture.module->LookupSymbol(kTarget, false) == nullptr);
  fixture.CheckNoOverlay();
}

TEST_CASE("Task callback witness cannot rely on host-replaced native code",
          "[kernel][guest_function_bridge]") {
  const bool replace_scheduler = GENERATE(false, true);
  CAPTURE(replace_scheduler);
  BridgeFixture fixture([replace_scheduler](std::vector<uint8_t>& image) {
    TaskCallbackImage(image);
    ImageWord(image, kExports + 0x24, 2);
    ImageWord(image, kExports + 0x30,
              (replace_scheduler ? kTaskSchedule : kCaller) - kImageBase);
  });
  REQUIRE(fixture.module->BindGuestExportToHost(2, kHostLibrary, 1));
  fixture.CheckGuestBytes();  // Guest hashes alone still match after binding.
  CHECK_FALSE(BindTaskCallback(fixture));
  CHECK(fixture.module->LookupSymbol(kTarget, false) == nullptr);
  fixture.CheckNoOverlay();
}

TEST_CASE(
    "Admitted Task witness protects native dependencies from later bridges",
    "[kernel][guest_function_bridge]") {
  const bool replace_scheduler = GENERATE(false, true);
  CAPTURE(replace_scheduler);
  BridgeFixture fixture([replace_scheduler](std::vector<uint8_t>& image) {
    TaskCallbackImage(image);
    ImageWord(image, kExports + 0x24, 2);
    ImageWord(image, kExports + 0x30,
              (replace_scheduler ? kTaskSchedule : kCaller) - kImageBase);
  });
  REQUIRE(BindTaskCallback(fixture));
  CHECK_FALSE(fixture.module->BindGuestExportToHost(2, kHostLibrary, 1));
  const uint32_t dependency = replace_scheduler ? kTaskSchedule : kCaller;
  CHECK(fixture.module->LookupSymbol(dependency, false) == nullptr);
  CHECK(fixture.module->TranslateCode(dependency) ==
        fixture.memory.TranslateVirtual<const uint8_t*>(dependency));
  CHECK(fixture.Call(kTaskIndirect) == kStaticMarker + 11 + 23);
  fixture.CheckGuestBytes();
}

TEST_CASE("Task callback bridge refuses compiled target and foreign ownership",
          "[kernel][guest_function_bridge]") {
  const bool foreign_owner = GENERATE(false, true);
  CAPTURE(foreign_owner);
  BridgeFixture fixture(TaskCallbackImage, foreign_owner);
  if (!foreign_owner) {
    auto* target = fixture.processor->ResolveFunction(kTarget);
    REQUIRE(target);
    REQUIRE(static_cast<GuestFunction*>(target)->machine_code());
  }
  CHECK_FALSE(BindTaskCallback(fixture));
  fixture.CheckNoOverlay();
}

TEST_CASE("Strict export bridge accepts a complete leaf without direct BL",
          "[kernel][guest_function_bridge][strict_export]") {
  BridgeFixture fixture([](std::vector<uint8_t>& image) {
    ImageWord(image, kCaller + 8, 0x60000000);
    ImageWord(image, kTarget + 4, 0x38630001);
    ImageWord(image, kTarget + 8, 0x60000000);
    ImageWord(image, kTarget + 12, 0x60000000);
    ImageWord(image, kTarget + 16, 0x4E800020);
  });
  const auto hash = fixture.Hash(kTarget, 20);
  CHECK_FALSE(fixture.module->BindGuestFunctionToHost(kTarget, 20, hash,
                                                      kHostLibrary, 1));
  fixture.CheckNoOverlay();
  REQUIRE(fixture.module->BindGuestExportToHost(1, kHostLibrary, 1, 20, hash));
  CHECK(fixture.Call(kTarget, 3, 0xA50F0200) == kExportMarker + 3 + 0xA50F0200);
  CHECK(fixture.thread->context()->r[9] == 1);
  Function* interior = nullptr;
  CHECK(fixture.module->DeclareFunction(kTarget + 16, &interior) ==
        Symbol::Status::kFailed);
  CHECK(interior == nullptr);
  fixture.CheckGuestBytes();
}

TEST_CASE("Strict export failure leaves bytes symbols and overlay untouched",
          "[kernel][guest_function_bridge][strict_export]") {
  BridgeFixture fixture;
  auto wrong = fixture.Hash();
  wrong[63] = wrong[63] == '0' ? '1' : '0';
  for (const auto& hash :
       {std::string(), std::string(63, '0'), std::string(64, 'g'), wrong}) {
    CAPTURE(hash);
    CHECK_FALSE(fixture.module->BindGuestExportToHost(1, kHostLibrary, 1,
                                                      kTargetSize, hash));
    fixture.CheckNoOverlay();
    CHECK(fixture.module->LookupSymbol(kTarget, false) == nullptr);
  }
  for (uint16_t host : {uint16_t(0), uint16_t(2), uint16_t(3), uint16_t(4)}) {
    CHECK_FALSE(fixture.module->BindGuestExportToHost(
        1, kHostLibrary, host, kTargetSize, fixture.Hash()));
    fixture.CheckNoOverlay();
  }
  CHECK_FALSE(fixture.module->BindGuestExportToHost(
      2, kHostLibrary, 1, kTargetSize, fixture.Hash()));
  CHECK_FALSE(fixture.module->BindGuestExportToHost(1, kHostLibrary, 1, 4,
                                                    fixture.Hash(kTarget, 4)));
  fixture.CheckNoOverlay();
  REQUIRE(fixture.module->BindGuestExportToHost(1, kHostLibrary, 1, kTargetSize,
                                                fixture.Hash()));
  CHECK(fixture.Call(kTarget) == kExportMarker + 11 + 23);
}

TEST_CASE("Strict export retains source ownership and complete entry checks",
          "[kernel][guest_function_bridge][strict_export]") {
  const auto variant = GENERATE(0, 1, 2, 3, 4);
  CAPTURE(variant);
  BridgeFixture fixture(
      [variant](std::vector<uint8_t>& image) {
        if (variant == 0) {
          ImageWord(image, kTarget + 4, 0x60000000);
        } else if (variant == 1) {
          ImageWord(image, kExports + 0x24, 2);
          ImageWord(image, kExports + 0x30, kTarget + 4 - kImageBase);
        } else if (variant == 2) {
          ImageWord(image, kTailCaller,
                    Branch(kTailCaller, kTarget + 4, false));
        }
      },
      variant == 3);
  if (variant == 4) {
    REQUIRE(fixture.processor->ResolveFunction(kTarget));
  }
  CHECK_FALSE(fixture.module->BindGuestExportToHost(
      1, kHostLibrary, 1, kTargetSize, fixture.Hash()));
  fixture.CheckNoOverlay();
}

enum class BridgeKind { kStaticInternal, kExportInternal, kExportOrdinal };

TEST_CASE(
    "Guest bridges dispatch entry, compiled direct and tail calls without "
    "patching bytes",
    "[kernel][guest_function_bridge]") {
  const auto kind =
      GENERATE(BridgeKind::kStaticInternal, BridgeKind::kExportInternal,
               BridgeKind::kExportOrdinal);
  CAPTURE(static_cast<int>(kind));
  BridgeFixture fixture;
  if (kind == BridgeKind::kStaticInternal) {
    REQUIRE(fixture.BindStatic());
  } else if (kind == BridgeKind::kExportInternal) {
    REQUIRE(fixture.module->BindGuestFunctionToHost(
        kTarget, kTargetSize, fixture.Hash(), kHostLibrary, 1));
  } else {
    REQUIRE(fixture.module->BindGuestExportToHost(1, kHostLibrary, 1));
  }
  const uint32_t marker =
      kind == BridgeKind::kStaticInternal ? kStaticMarker : kExportMarker;
  // Resolve the target first so later direct/tail callers can embed its actual
  // native code address, exercising the path that metadata-only tests miss.
  REQUIRE(fixture.processor->ResolveFunction(kTarget));
  REQUIRE(fixture.processor->ResolveFunction(kCaller));
  REQUIRE(fixture.processor->ResolveFunction(kTailCaller));
  for (uint32_t address : {kTarget, kCaller, kTailCaller}) {
    CAPTURE(address);
    CHECK(fixture.Call(address) == marker + 11 + 23 + (address == kCaller));
    CHECK(fixture.thread->context()->r[9] == 1);
    fixture.CheckGuestBytes();
  }
  Function* interior = nullptr;
  CHECK(fixture.module->DeclareFunction(kTarget + 4, &interior) ==
        Symbol::Status::kFailed);
  CHECK(interior == nullptr);
}

TEST_CASE("Caller compiled before bridge uses its unresolved target safely",
          "[kernel][guest_function_bridge]") {
  BridgeFixture fixture;
  auto* caller = fixture.processor->ResolveFunction(kCaller);
  REQUIRE(caller);
  auto* target = fixture.module->LookupSymbol(kTarget);
  REQUIRE(target);
  CHECK(target->status() == Symbol::Status::kDeclared);
  CHECK(fixture.processor->QueryFunction(kTarget) == nullptr);
  REQUIRE(fixture.BindStatic());
  CHECK(fixture.processor->QueryFunction(kCaller) == caller);
  CHECK(fixture.Call(kCaller) == kStaticMarker + 11 + 23 + 1);
  CHECK(fixture.thread->context()->r[9] == 1);
  fixture.CheckGuestBytes();
}

TEST_CASE(
    "Bridge rejection validates hash, bounds and host destination before "
    "mutation",
    "[kernel][guest_function_bridge]") {
  BridgeFixture fixture;
  const auto valid_hash = fixture.Hash();
  auto wrong_hash = valid_hash;
  wrong_hash[0] = wrong_hash[0] == '0' ? '1' : '0';
  for (const auto& hash : {wrong_hash, std::string(), std::string(63, '0'),
                           std::string(64, 'g')}) {
    CHECK_FALSE(fixture.module->BindGuestFunctionToHost(
        kTarget, kTargetSize, hash, StaticHandler, "StaticHandler"));
    fixture.CheckNoOverlay();
  }
  for (const auto& [address, size] :
       std::array<std::array<uint32_t, 2>, 6>{{{kTarget + 1, 8},
                                               {kTarget, 7},
                                               {kTarget, 4},
                                               {0xFFFFFFFC, 8},
                                               {kData - 4, 8},
                                               {kData, 8}}}) {
    CAPTURE(address, size);
    const uint64_t end = uint64_t(address) + size;
    const auto hash =
        address >= kImageBase && end <= uint64_t(kImageBase) + kImageSize
            ? fixture.Hash(address, size)
            : valid_hash;
    CHECK_FALSE(fixture.module->BindGuestFunctionToHost(
        address, size, hash, StaticHandler, "StaticHandler"));
    fixture.CheckNoOverlay();
  }
  for (uint16_t ordinal :
       {uint16_t(0), uint16_t(2), uint16_t(3), uint16_t(4), uint16_t(9)}) {
    CAPTURE(ordinal);
    CHECK_FALSE(fixture.module->BindGuestFunctionToHost(
        kTarget, kTargetSize, valid_hash, kHostLibrary, ordinal));
    fixture.CheckNoOverlay();
  }
  CHECK_FALSE(fixture.module->BindGuestFunctionToHost(
      kTarget, kTargetSize, valid_hash, "missing-library", 1));
  CHECK_FALSE(fixture.module->BindGuestFunctionToHost(
      kTarget, kTargetSize, valid_hash, nullptr, "Null"));
  CHECK_FALSE(fixture.module->BindGuestFunctionToHost(
      kTarget, kTargetSize, valid_hash, StaticHandler, ""));
  CHECK(fixture.module->LookupSymbol(kTarget, false) == nullptr);
  CHECK(fixture.processor->QueryFunction(kTarget) == nullptr);
  fixture.CheckNoOverlay();
  REQUIRE(
      fixture.BindStatic());  // Rejections did not poison a later valid bind.
}

TEST_CASE("Bridge rejects foreign module ownership",
          "[kernel][guest_function_bridge]") {
  BridgeFixture fixture({}, true);
  REQUIRE(fixture.processor->LookupModule(kTarget) != fixture.module);
  CHECK_FALSE(fixture.BindStatic());
  CHECK(fixture.module->LookupSymbol(kTarget, false) == nullptr);
  fixture.CheckNoOverlay();
}

TEST_CASE("Bridge rejects unreadable mapped code before hashing it",
          "[kernel][guest_function_bridge]") {
  BridgeFixture fixture;
  const auto hash = fixture.Hash();
  auto* heap = fixture.memory.LookupHeap(kCode);
  REQUIRE(heap);
  uint32_t original_protection = 0;
  REQUIRE(heap->QueryProtect(kCode, &original_protection));
  struct RestoreProtection {
    BaseHeap* heap;
    uint32_t protection;
    ~RestoreProtection() { heap->Protect(kCode, kPageSize, protection); }
  } restore{heap, original_protection};
  REQUIRE(heap->Protect(kCode, kPageSize, kMemoryProtectNoAccess));
  CHECK_FALSE(fixture.module->BindGuestFunctionToHost(
      kTarget, kTargetSize, hash, StaticHandler, "StaticHandler"));
  // Do not inspect or hash any live code while its page is inaccessible.
  REQUIRE(heap->Protect(kCode, kPageSize, original_protection));
  CHECK(fixture.module->LookupSymbol(kTarget, false) == nullptr);
  fixture.CheckNoOverlay();
  REQUIRE(fixture.BindStatic());
}

TEST_CASE(
    "Bridge requires a concrete call target and terminal function boundary",
    "[kernel][guest_function_bridge]") {
  const auto variant = GENERATE(0, 1, 2, 3, 4, 5);
  CAPTURE(variant);
  BridgeFixture fixture([variant](std::vector<uint8_t>& image) {
    if (variant == 0) {  // A tail reference alone is not a concrete BL target.
      ImageWord(image, kCaller + 8, 0x60000000);
    } else if (variant == 1) {
      ImageWord(image, kTarget + 4, 0x60000000);  // Truncated before return.
    } else if (variant == 2) {
      ImageWord(image, kTailCaller, Branch(kTailCaller, kTarget + 4, false));
    } else if (variant == 3) {
      ImageWord(image, kExports + 0x24, 2);
      ImageWord(image, kExports + 0x30, kTarget + 4 - kImageBase);
    } else if (variant == 4) {
      // The mapped data page supplies one PE runtime entry. The independent
      // XEX export table stays at kExports, outside this small PE section.
      constexpr size_t kDataSection = 0x98 + 0xE0 + 40;
      std::memset(image.data() + kDataSection, 0, 8);
      std::memcpy(image.data() + kDataSection, ".pdata", 6);
      WriteLe32(image, kDataSection + 8, 8);
      WriteLe32(image, kDataSection + 16, 8);
      ImageWord(image, kData, kTarget + 4);
    } else {
      // An external conditional branch into the original body is also an
      // interior entry, even though it is not the BL proving the main entry.
      ImageWord(image, kTailCaller,
                0x41820000 | ((kTarget + 4 - kTailCaller) & 0xFFFC));
    }
  });
  CHECK_FALSE(fixture.BindStatic());
  CHECK(fixture.module->LookupSymbol(kTarget, false) == nullptr);
  fixture.CheckNoOverlay();
}

TEST_CASE("Bridge accepts a verified terminal tail branch",
          "[kernel][guest_function_bridge]") {
  BridgeFixture fixture([](std::vector<uint8_t>& image) {
    ImageWord(image, kTarget + 4, Branch(kTarget + 4, kTailReturn, false));
  });
  REQUIRE(fixture.BindStatic());
  CHECK(fixture.Call(kTarget) == kStaticMarker + 11 + 23);
  CHECK(fixture.thread->context()->r[9] == 1);
  fixture.CheckGuestBytes();
}

TEST_CASE(
    "Bridge rejects overlapping symbol identities and incomplete declaration "
    "states",
    "[kernel][guest_function_bridge]") {
  const auto variant = GENERATE(0, 1, 2, 3, 4, 5, 6, 7);
  CAPTURE(variant);
  BridgeFixture fixture;
  if (variant == 0) {
    Symbol* symbol = nullptr;
    REQUIRE(fixture.module->DeclareVariable(kTarget + 4, &symbol) ==
            Symbol::Status::kNew);
    symbol->set_status(Symbol::Status::kDeclared);
  } else {
    const uint32_t entry = variant == 1   ? kTarget + 4
                           : variant == 2 ? kTarget - 4
                                          : kTarget;
    Function* function = nullptr;
    REQUIRE(fixture.module->DeclareFunction(entry, &function) ==
            Symbol::Status::kNew);
    function->set_status(Symbol::Status::kDeclared);
    if (variant <= 2 || variant == 3) {
      function->set_end_address(kTarget + 8);
    }
    if (variant == 4) {
      function->set_status(Symbol::Status::kDeclaring);
    }
    if (variant == 5) {
      function->set_status(Symbol::Status::kDefining);
    }
    if (variant == 6) {
      function->set_status(Symbol::Status::kFailed);
    }
    if (variant == 7) {
      function->set_behavior(Function::Behavior::kProlog);
    }
  }
  CHECK_FALSE(fixture.BindStatic());
  fixture.CheckNoOverlay();
}

TEST_CASE(
    "Bridge rejects compiled targets including forgotten retained generations",
    "[kernel][guest_function_bridge]") {
  const bool retire = GENERATE(false, true);
  CAPTURE(retire);
  BridgeFixture fixture;
  auto* target = fixture.processor->ResolveFunction(kTarget);
  REQUIRE(target);
  REQUIRE(static_cast<GuestFunction*>(target)->machine_code());
  REQUIRE(fixture.processor->ResolveFunction(kCaller));
  if (retire) {
    fixture.processor->InvalidateCodeRange(kTarget, kTargetSize);
    CHECK(fixture.processor->QueryFunction(kTarget) == nullptr);
    CHECK(fixture.module->LookupSymbol(kTarget, false) == nullptr);
    bool retained = false;
    fixture.module->ForEachFunction(
        [&](Function* function) { retained |= function == target; });
    REQUIRE(retained);
  }
  CHECK_FALSE(fixture.BindStatic());
  fixture.CheckNoOverlay();
  if (!retire) {
    CHECK(fixture.Call(kCaller) ==
          8);  // Existing literal native target remains original.
    CHECK(fixture.thread->context()->r[9] == 0);
  }
}

TEST_CASE("Duplicate bridge rejection preserves the admitted handler",
          "[kernel][guest_function_bridge]") {
  BridgeFixture fixture;
  REQUIRE(fixture.BindStatic());
  CHECK_FALSE(fixture.module->BindGuestFunctionToHost(
      kTarget, kTargetSize, fixture.Hash(), kHostLibrary, 1));
  CHECK(fixture.Call(kTarget) == kStaticMarker + 11 + 23);
  CHECK(fixture.thread->context()->r[9] == 1);
  fixture.CheckGuestBytes();
}

// Host-side loader/data regression: actual Kernel/Memory/UserModule, no guest
// execution, native startup, activation or service readiness is fabricated.
TEST_CASE("Loaded XEX security uses the real executable LDR address chain",
          "[kernel][xex][guest_header]") {
  REQUIRE(kernel::KernelState::shared() == nullptr);
  kernel::test::IsolatedKernelRuntimeSettings settings;
  struct Scratch {
    std::filesystem::path root = std::filesystem::temp_directory_path() /
        ("xenia-guest-header-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    Scratch() {
      REQUIRE(std::filesystem::create_directory(root));
      REQUIRE(std::filesystem::create_directory(root / "content"));
      REQUIRE(std::filesystem::create_directory(root / "cache"));
    }
    ~Scratch() { std::error_code ignored; std::filesystem::remove_all(root, ignored); }
  } scratch;
  auto emulator = std::make_unique<Emulator>("", scratch.root,
      scratch.root / "content", scratch.root / "cache");
  REQUIRE(emulator->Setup(nullptr, nullptr, false, {}, {}, {}) == X_STATUS_SUCCESS);
  auto* state = emulator->kernel_state();
  auto module = kernel::object_ref<kernel::UserModule>(new kernel::UserModule(state));
  const auto raw = MakeXex({});
  const auto original = raw;
  REQUIRE(module->LoadFromMemoryNamed("guest-header-fixture", raw.data(), raw.size()) == X_STATUS_PENDING);
  REQUIRE(module->LoadContinue() == X_STATUS_SUCCESS);
  state->SetExecutableModule(module);
  auto* memory = emulator->memory();
  const auto word = [&](uint32_t address) {
    return xe::load_and_swap<uint32_t>(memory->TranslateVirtual(address));
  };
  const auto* exported = emulator->processor()->export_resolver()->GetExportByOrdinal(
      "xboxkrnl.exe", 0x193); // Actual XexExecutableModuleHandle variable.
  REQUIRE(exported); REQUIRE(exported->variable_ptr);
  const uint32_t ldr = word(exported->variable_ptr);
  REQUIRE(ldr == module->hmodule_ptr());
  const uint32_t resident = word(ldr + offsetof(kernel::X_LDR_DATA_TABLE_ENTRY, xex_header_base));
  REQUIRE(resident == module->guest_xex_header());
  // Exact native E1E40 chain after the imported export cell: LDR+58,
  // resident+10, security+10C. Guard the expected address before dereference so
  // the old raw-offset defect fails this assertion rather than faulting host.
  const uint32_t security = word(resident + 0x10);
  REQUIRE(security == resident + 0x100);
  CHECK(word(security + 0x10C) == 0);
  CHECK((word(security + 0x10C) & XEX_IMAGE_ONLINE_ACTIVATION_REQUIRED) == 0);
  CHECK(module->xex_module()->xex_header()->security_offset == 0x100);
  CHECK(module->xex_module()->xex_security_info()->image_flags == 0);
  CHECK(raw == original);
  uint32_t entry = 0;
  REQUIRE(module->GetOptHeader(XEX_HEADER_ENTRY_POINT, &entry) == X_STATUS_SUCCESS);
  CHECK(entry == kCaller); // Optional-header offsets/immediates stay unchanged.
  uint32_t format = 0;
  REQUIRE(module->GetOptHeader(XEX_HEADER_FILE_FORMAT_INFO, &format) == X_STATUS_SUCCESS);
  REQUIRE(format == resident + 0x40);
  CHECK(word(format) == 8);
  REQUIRE(module->LoadContinue() == X_STATUS_SUCCESS);
  CHECK(module->guest_xex_header() == resident);
  CHECK(word(resident + 0x10) == security); // Real producer returns its once-published view.
}

TEST_CASE("Prepared title metadata uses exact resident execution and optional header inputs",
          "[kernel][xex][guest_header][native_title_metadata]") {
  REQUIRE(kernel::KernelState::shared() == nullptr);
  kernel::test::IsolatedKernelRuntimeSettings settings;
  const bool has_execution = GENERATE(false, true);
  const bool has_services = GENERATE(false, true);
  // A completed TU changes this optional header before LoadContinue. These
  // two synthetic final headers test its real loading/projection boundary;
  // no delta-patch package or native logon setter execution is claimed here.
  const uint32_t final_version = GENERATE(0x00010000u, 0x00020304u);
  constexpr uint32_t kTitle = 0x58410A71, kMedia = 0x261A2859;
  constexpr uint32_t kExecution = 0x80, kServices = 0xA0;
  constexpr uint32_t kRegion = 0x00FF00FF, kMediaFallback = 0x13572468;
  const std::array<uint32_t, 5> services{20, 0x01020304, 0xA2B3C4D5,
                                        0x10203040, 0x50607080};
  struct Scratch {
    std::filesystem::path root = std::filesystem::temp_directory_path() /
        ("xenia-title-header-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    Scratch() {
      REQUIRE(std::filesystem::create_directory(root));
      REQUIRE(std::filesystem::create_directory(root / "content"));
      REQUIRE(std::filesystem::create_directory(root / "cache"));
    }
    ~Scratch() { std::error_code ignored; std::filesystem::remove_all(root, ignored); }
  } scratch;
  auto emulator = std::make_unique<Emulator>("", scratch.root,
      scratch.root / "content", scratch.root / "cache");
  REQUIRE(emulator->Setup(nullptr, nullptr, false, {}, {}, {}) == X_STATUS_SUCCESS);
  auto* state = emulator->kernel_state();
  const auto raw = MakeXex({}, [&](std::vector<uint8_t>& bytes) {
    // Six entries need the old file-format block moved out of their table.
    // All pointers remain real header offsets, never host addresses.
    WriteBe(bytes, 0x1C, 0x60);
    WriteBe(bytes, 0x60, 8); WriteBe(bytes, 0x64, 0);
    uint32_t count = 4;
    const auto entry = [&](uint32_t key, uint32_t offset) {
      WriteBe(bytes, 0x18 + count * 8, key);
      WriteBe(bytes, 0x1C + count * 8, offset); ++count;
    };
    if (has_execution) {
      entry(XEX_HEADER_EXECUTION_INFO, kExecution);
      WriteBe(bytes, kExecution, kMedia);
      WriteBe(bytes, kExecution + 4, final_version);
      WriteBe(bytes, kExecution + 8, 0x00010000);
      WriteBe(bytes, kExecution + 12, kTitle);
      bytes[kExecution + 16] = 2; bytes[kExecution + 17] = 0;
      bytes[kExecution + 18] = 1; bytes[kExecution + 19] = 2;
      WriteBe(bytes, kExecution + 20, 0x12345678);
    }
    if (has_services) {
      entry(XEX_HEADER_SERVICE_ID_LIST, kServices);
      for (size_t i = 0; i < services.size(); ++i) WriteBe(bytes, kServices + i * 4, services[i]);
    }
    WriteBe(bytes, 0x14, count);
    WriteBe(bytes, 0x100 + 0x178, kRegion);
    WriteBe(bytes, 0x100 + 0x14C, kMediaFallback);
  });
  const auto original = raw;
  auto module = kernel::object_ref<kernel::UserModule>(new kernel::UserModule(state));
  REQUIRE(module->LoadFromMemoryNamed("title-metadata-fixture", raw.data(), raw.size()) == X_STATUS_PENDING);
  REQUIRE(module->LoadContinue() == X_STATUS_SUCCESS);
  state->SetExecutableModule(module);
  auto* memory = emulator->memory();
  const uint32_t resident = module->guest_xex_header();
  REQUIRE(resident); REQUIRE(resident != kImageBase);
  const auto word = [&](uint32_t address) {
    return xe::load_and_swap<uint32_t>(memory->TranslateVirtual(address));
  };
  const uint32_t security = word(resident + 0x10);
  REQUIRE(security == resident + 0x100);
  CHECK(word(security + 0x178) == kRegion);
  CHECK(word(security + 0x14C) == kMediaFallback); // Raw input, not interpreted native fallback.
  uint32_t execution = 0xDEADBEEF;
  const auto status = module->GetOptHeader(XEX_HEADER_EXECUTION_INFO, &execution);
  xex2_opt_execution_info* host_execution = nullptr;
  const auto host_status = module->GetOptHeader(XEX_HEADER_EXECUTION_INFO, &host_execution);
  const uint32_t result = memory->SystemHeapAlloc(4);
  REQUIRE(result); xe::store_and_swap<uint32_t>(memory->TranslateVirtual(result), 0xA5A5A5A5);
  const auto* getter = emulator->processor()->export_resolver()->GetExportByOrdinal("xam.xex", 0x280);
  REQUIRE(getter); REQUIRE(getter->function_data.trampoline); REQUIRE(getter->is_implemented());
  ppc::PPCContext context{}; context.kernel_state = state;
  context.virtual_membase = memory->virtual_membase(); context.r[3] = result;
  getter->function_data.trampoline(&context);
  CHECK(uint32_t(context.r[3]) == status);
  if (has_execution) {
    REQUIRE(status == X_STATUS_SUCCESS); REQUIRE(host_status == X_STATUS_SUCCESS); REQUIRE(host_execution);
    REQUIRE(execution == resident + kExecution);
    CHECK(state->title_id() == kTitle); CHECK(module->title_id() == kTitle);
    CHECK(word(result) == execution); // Actual HLE returns this same resident header, no parallel copy.
    CHECK(std::memcmp(memory->TranslateVirtual(execution), raw.data() + kExecution, sizeof(xex2_opt_execution_info)) == 0);
    CHECK(uint32_t(host_execution->version_value) == final_version);
    CHECK(word(execution + 4) == final_version); CHECK(word(execution + 12) == kTitle);
    CHECK(uint32_t(host_execution->media_id) == kMedia);
  } else {
    CHECK(status == X_STATUS_NOT_FOUND); CHECK(host_status == X_STATUS_NOT_FOUND);
    CHECK(execution == 0); CHECK(host_execution == nullptr);
    CHECK(word(result) == 0xA5A5A5A5); // Missing optional metadata never manufactures output/success.
    CHECK(state->title_id() == 0); CHECK(module->title_id() == 0);
  }
  uint32_t service = 0;
  const auto service_status = module->GetOptHeader(XEX_HEADER_SERVICE_ID_LIST, &service);
  if (has_services) {
    REQUIRE(service_status == X_STATUS_SUCCESS); REQUIRE(service == resident + kServices);
    REQUIRE(uint64_t(kServices) + services.size() * 4 <= kHeaderSize);
    for (size_t i = 0; i < services.size(); ++i) CHECK(word(service + uint32_t(i * 4)) == services[i]);
    // Opaque declared20-byte record: no invented field/count semantics and no
    // claim that a native setter consumed or accepted these synthetic values.
  } else {
    CHECK(service_status == X_STATUS_NOT_FOUND); CHECK(service == 0);
  }
  std::vector<uint8_t> captured(kHeaderSize);
  std::memcpy(captured.data(), memory->TranslateVirtual(resident), captured.size());
  REQUIRE(module->LoadContinue() == X_STATUS_SUCCESS);
  CHECK(module->guest_xex_header() == resident);
  CHECK(std::memcmp(captured.data(), memory->TranslateVirtual(resident), captured.size()) == 0);
  CHECK(raw == original);
  memory->SystemHeapFree(result);
}

TEST_CASE("Guest XEX security projection preserves every supported raw layout",
          "[kernel][xex][guest_header]") {
  struct Format { uint32_t magic; size_t prefix, flags; };
  const std::array formats{
      Format{0x58455831, offsetof(xex1_security_info, page_descriptors), offsetof(xex1_security_info, image_flags)},
      Format{0x58455832, offsetof(xex2_security_info, page_descriptors), offsetof(xex2_security_info, image_flags)},
      Format{0x58455825, offsetof(xex25_security_info, page_descriptors), offsetof(xex25_security_info, image_flags)}};
  Memory memory; REQUIRE(memory.Initialize());
  for (const auto& format : formats) {
    std::vector<uint8_t> raw(0x1000, 0xA7);
    WriteBe(raw, 0, format.magic); WriteBe(raw, 8, uint32_t(raw.size()));
    WriteBe(raw, 0x10, 0x100); WriteBe(raw, 0x14, 1);
    WriteBe(raw, 0x18, XEX_HEADER_ENTRY_POINT); WriteBe(raw, 0x1C, kCaller);
    WriteBe(raw, 0x100, uint32_t(format.prefix + sizeof(xex2_page_descriptor)));
    WriteBe(raw, 0x100 + format.prefix - 4, 1);
    WriteBe(raw, 0x100 + format.flags, XEX_IMAGE_ONLINE_ACTIVATION_REQUIRED);
    const auto original = raw;
    const uint32_t guest = memory.SystemHeapAlloc(uint32_t(raw.size())); REQUIRE(guest);
    struct Release { Memory& memory; uint32_t guest; ~Release() { memory.SystemHeapFree(guest); } } release{memory, guest};
    auto* destination = memory.TranslateVirtual<uint8_t*>(guest);
    std::memcpy(destination, raw.data(), raw.size());
    REQUIRE(RelocateXexSecurityInfoForGuest({destination, raw.size()}, guest));
    const uint32_t security = xe::load_and_swap<uint32_t>(destination + 0x10);
    REQUIRE(security == guest + 0x100);
    CHECK(xe::load_and_swap<uint32_t>(memory.TranslateVirtual(security + uint32_t(format.flags))) == XEX_IMAGE_ONLINE_ACTIVATION_REQUIRED);
    std::vector<uint8_t> projected(destination, destination + raw.size());
    WriteBe(projected, 0x10, 0x100);
    CHECK(projected == original); // The one pointer field is the only change.
    CHECK(raw == original);
    const std::vector<uint8_t> once(destination, destination + raw.size());
    CHECK_FALSE(RelocateXexSecurityInfoForGuest({destination, raw.size()}, guest));
    CHECK(std::equal(once.begin(), once.end(), destination)); // This actual resident address is outside raw file-offset bounds.
  }
}

TEST_CASE("Malformed guest XEX security projection refuses without partial writes",
          "[kernel][xex][guest_header]") {
  auto raw = MakeXex({}); raw.resize(kHeaderSize);
  for (uint32_t fault = 0; fault < 13; ++fault) {
    auto bad = raw; uint32_t guest = 0x30010000;
    if (fault == 0) bad.resize(0x10);
    if (fault == 1) WriteBe(bad, 0, 0x58455830); // Unsupported XEX0.
    if (fault == 2) WriteBe(bad, 8, uint32_t(bad.size()) + 4);
    if (fault == 3) WriteBe(bad, 0x14, UINT32_MAX);
    if (fault == 4) WriteBe(bad, 0x10, 0x18); // Overlaps optional table.
    if (fault == 5) WriteBe(bad, 0x10, 0xFFC); // Security prefix truncated.
    if (fault == 6) WriteBe(bad, 0x100, 0x180);
    if (fault == 7) WriteBe(bad, 0x100, 0x1000);
    if (fault == 8) WriteBe(bad, 0x280, UINT32_MAX); // Descriptors out of extent.
    if (fault == 9) guest = 0;
    if (fault == 10) guest |= 1; // A handle/tag is not a guest header address.
    if (fault == 11) guest |= 2;
    if (fault == 12) guest = 0xFFFFF004; // 32-bit address extent would wrap.
    const auto original = bad;
    CHECK_FALSE(RelocateXexSecurityInfoForGuest(bad, guest));
    CHECK(bad == original);
  }
}

TEST_CASE("XexLoadImageHeaders rejects real short reads before publishing a pointer",
          "[kernel][xex][guest_header][exports]") {
  REQUIRE(kernel::KernelState::shared() == nullptr);
  kernel::test::IsolatedKernelRuntimeSettings settings;
  struct Scratch {
    std::filesystem::path root = std::filesystem::temp_directory_path() /
        ("xenia-header-reads-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    Scratch() {
      REQUIRE(std::filesystem::create_directory(root));
      for (const auto* name : {"content", "cache", "inputs"})
        REQUIRE(std::filesystem::create_directory(root / name));
    }
    ~Scratch() { std::error_code ignored; std::filesystem::remove_all(root, ignored); }
  } scratch;
  const auto raw = MakeXex({});
  for (const auto& file : {std::pair{"first.xex", size_t(0x200)},
                           std::pair{"tail.xex", size_t(0x900)},
                           std::pair{"full.xex", size_t(kHeaderSize)}}) {
    std::ofstream stream(scratch.root / "inputs" / file.first, std::ios::binary);
    stream.write(reinterpret_cast<const char*>(raw.data()), file.second);
    stream.close(); REQUIRE(stream.good());
  }
  auto emulator = std::make_unique<Emulator>("", scratch.root,
      scratch.root / "content", scratch.root / "cache");
  REQUIRE(emulator->Setup(nullptr, nullptr, false, {}, {}, {}) == X_STATUS_SUCCESS);
  auto device = std::make_unique<vfs::HostPathDevice>(
      "\\Device\\XexHeaders", scratch.root / "inputs", true);
  REQUIRE(device->Initialize());
  auto* observed_device = device.get();
  REQUIRE(emulator->kernel_state()->file_system()->RegisterDevice(std::move(device)));
  auto* memory = emulator->memory();
  const uint32_t page = memory->SystemHeapAlloc(0x3000); REQUIRE(page);
  struct Release { Memory* memory; uint32_t page; ~Release() { memory->SystemHeapFree(page); } } release{memory, page};
  const uint32_t header = page + 0x1000;
  auto* entry = emulator->export_resolver()->GetExportByOrdinal("xboxkrnl.exe", 0x19B);
  REQUIRE(entry); REQUIRE(entry->function_data.trampoline);
  for (const auto* name : {"first.xex", "tail.xex", "full.xex"}) {
    const std::string path = std::string("\\Device\\XexHeaders\\") + name;
    std::memcpy(memory->TranslateVirtual(page + 0x100), path.c_str(), path.size() + 1);
    auto* ansi = memory->TranslateVirtual<kernel::X_ANSI_STRING*>(page);
    ansi->length = uint16_t(path.size()); ansi->maximum_length = uint16_t(path.size() + 1);
    ansi->pointer = page + 0x100;
    // An incomplete successful read must not validate or relocate these old
    // caller bytes, even though their raw header/security data is well formed.
    std::memcpy(memory->TranslateVirtual(header), raw.data(), kHeaderSize);
    ppc::PPCContext context{};
    context.kernel_state = emulator->kernel_state();
    context.virtual_membase = memory->virtual_membase();
    context.r[3] = page; context.r[4] = header; context.r[5] = kHeaderSize;
    entry->function_data.trampoline(&context);
    const bool complete = std::string_view(name) == "full.xex";
    CHECK(uint32_t(context.r[3]) == (complete ? X_STATUS_SUCCESS : X_STATUS_INVALID_IMAGE_FORMAT));
    const auto security = xe::load_and_swap<uint32_t>(memory->TranslateVirtual(header + 0x10));
    CHECK(security == (complete ? header + 0x100 : 0x100));
    CHECK_FALSE(observed_device->has_open_files());
  }
}

}  // namespace
}  // namespace xe::cpu::test
namespace xe::cpu::test {
namespace {
// Exact retail17559 native cache-query instructions, relocated together into
// the generated XEX. Only the two relative save/restore thunk targets change.
// The real fixed native cache address stays81AA28E0. This fixture deliberately
// keeps native cache policy28DC zero, so8470's external policy calls are not
// taken; no provider, Task, native publication or renderer is simulated.
constexpr uint32_t kNativeQueryBase = kCode + 0x1000;
constexpr uint32_t QueryAddress(uint32_t native) {
  return kNativeQueryBase + native - 0x816D8470;
}
constexpr std::array<uint32_t, 39> kNativeQueryValidity{{
    0x7D8802A6, 0x9181FFF8, 0xFBC1FFE8, 0xFBE1FFF0, 0x9421FF90, 0x7C7F1B78,
    0x7C9E2378, 0x2B030004, 0x41980010, 0x3C608007, 0x60630057, 0x48000058,
    0x3D6081AA, 0x816B28DC, 0x2F0B0000, 0x40990028, 0x4BFDFCE1, 0x2C030000,
    0x4182001C, 0x480087AD, 0x2C030000, 0x40820010, 0x3C608007, 0x60630525,
    0x48000024, 0x1D7F0290, 0x7D6BF02E, 0x556B07FE, 0x3D408007, 0x316BFFFF,
    0x614A0525, 0x7D6B5910, 0x7D635038, 0x38210070, 0x8181FFF8, 0x7D8803A6,
    0xEBC1FFE8, 0xEBE1FFF0, 0x4E800020,
}};

constexpr std::array<uint32_t, 31> kNativeGetXuidBody{{
    0x7D8802A6, 0x48054EB5, 0x9421FF80, 0x7C9D2378, 0x7CC43378, 0x7C7F1B78,
    0x7CBC2B78, 0x7CDE3378, 0x4BFFFF41, 0x2C030000, 0x41800044, 0x1D7F0290,
    0x7D6BF214, 0x814B0000, 0x7D4AE838, 0x554907BD, 0x4182000C, 0xE96B003C,
    0x48000028, 0x5549077B, 0x4082FFF4, 0x554A07FF, 0x4182000C, 0xE96B0008,
    0x48000010, 0x3C608007, 0x60630525, 0x39600000, 0xF97C0000, 0x38210080,
    0x48054E90,
}};

constexpr std::array<uint32_t, 3> kNativeGetXuidEntry{{
    0x3D6081AA,
    0x38CB28E0,
    0x4BFFFF78,
}};

constexpr std::array<uint32_t, 32> kNativeGetIndexBody{{
    0x39400000, 0x3966003C, 0x812BFFC4, 0x7D292038, 0x552807BD, 0x41820010,
    0xE90B0000, 0x7F234040, 0x419A0040, 0x5528077B, 0x41820010, 0xE90B0000,
    0x7F234040, 0x419A002C, 0x552907FF, 0x41820010, 0xE92BFFCC, 0x7F234840,
    0x419A0018, 0x394A0001, 0x396B0290, 0x2B0A0004, 0x4198FFB0, 0x4800000C,
    0x2B0A0004, 0x41980010, 0x3C608007, 0x60630525, 0x4E800020, 0x91450000,
    0x38600000, 0x4E800020,
}};

constexpr std::array<uint32_t, 3> kNativeGetIndexEntry{{
    0x3D6081AA,
    0x38CB28E0,
    0x4BFFFF78,
}};

constexpr std::array<uint32_t, 6> kNativeSave28{{
    0xFB81FFD8,
    0xFBA1FFE0,
    0xFBC1FFE8,
    0xFBE1FFF0,
    0x9181FFF8,
    0x4E800020,
}};

constexpr std::array<uint32_t, 7> kNativeRestore28{{
    0xEB81FFD8,
    0xEBA1FFE0,
    0xEBC1FFE8,
    0xEBE1FFF0,
    0x8181FFF8,
    0x7D8803A6,
    0x4E800020,
}};

void NativeCacheQueriesImage(std::vector<uint8_t>& image) {
  const auto copy = [&](uint32_t address, const auto& words) {
    for (size_t i = 0; i < words.size(); ++i) {
      ImageWord(image, address + uint32_t(i * 4), words[i]);
    }
  };
  copy(QueryAddress(0x816D8470), kNativeQueryValidity);
  copy(QueryAddress(0x816D8510), kNativeGetXuidBody);
  copy(QueryAddress(0x816D8590), kNativeGetXuidEntry);
  copy(QueryAddress(0x816D8618), kNativeGetIndexBody);
  copy(QueryAddress(0x816D8698), kNativeGetIndexEntry);
  copy(kCode + 0x1400, kNativeSave28);
  copy(kCode + 0x1440, kNativeRestore28);
  ImageWord(image, QueryAddress(0x816D8514),
            Branch(QueryAddress(0x816D8514), kCode + 0x1400, true));
  ImageWord(image, QueryAddress(0x816D8588),
            Branch(QueryAddress(0x816D8588), kCode + 0x1440, false));
}
}  // namespace

TEST_CASE(
    "Native cache queries do not expose host selected users before publication",
    "[cpu][guest_function_bridge][native_cache_query]") {
  BridgeFixture fixture(NativeCacheQueriesImage);
  constexpr uint32_t cache_block = 0x81AA0000, cache = 0x81AA28E0;
  auto* heap = fixture.memory.LookupHeap(cache_block);
  REQUIRE(heap);
  REQUIRE(heap->AllocFixed(cache_block, 0x10000, 0x10000,
                           kMemoryAllocationReserve | kMemoryAllocationCommit,
                           kMemoryProtectRead | kMemoryProtectWrite));
  std::memset(fixture.memory.TranslateVirtual(cache_block), 0, 0x10000);
  const auto slot = GENERATE(0u, 1u, 2u, 3u);
  const auto mask = GENERATE(1u, 2u, 4u);
  constexpr uint64_t host_selected_xuid = 0xE0000078AABBCCDDull;
  constexpr uint64_t online_xuid = 0x0009000011223344ull;
  const auto expected_xuid = mask == 1 ? host_selected_xuid : online_xuid;
  constexpr uint32_t output = cache_block + 0x8000;
  const auto store32 = [&](uint32_t address, uint32_t value) {
    xe::store_and_swap<uint32_t>(fixture.memory.TranslateVirtual(address),
                                 value);
  };
  const auto store64 = [&](uint32_t address, uint64_t value) {
    xe::store_and_swap<uint64_t>(fixture.memory.TranslateVirtual(address),
                                 value);
  };
  const auto call = [&](uint32_t native, uint64_t first) {
    auto* c = fixture.thread->context();
    c->r[3] = first;
    c->r[4] = mask;
    c->r[5] = output;
    REQUIRE(
        fixture.processor->Execute(fixture.thread.get(), QueryAddress(native)));
    return uint32_t(c->r[3]);
  };
  // A selected host identity alone is not a native-cache hit. The native
  // index getter leaves its output untouched on no-match; XUID getter clears
  // it.
  store64(output, 0xAABBCCDDEEFF0011ull);
  CHECK(call(0x816D8698, expected_xuid) == 0x80070525u);
  CHECK(xe::load_and_swap<uint64_t>(fixture.memory.TranslateVirtual(output)) ==
        0xAABBCCDDEEFF0011ull);
  CHECK(call(0x816D8590, slot) == 0x80070525u);
  CHECK(xe::load_and_swap<uint64_t>(fixture.memory.TranslateVirtual(output)) ==
        0);

  // Only the actual native row fields make either native query succeed.
  const auto row = cache + slot * 0x290;
  store32(row, 1 | mask);
  store64(row + 8, host_selected_xuid);
  store64(row + 0x3C, online_xuid);
  store32(output, 0xAABBCCDD);
  CHECK(call(0x816D8698, expected_xuid) == 0);
  CHECK(xe::load_and_swap<uint32_t>(fixture.memory.TranslateVirtual(output)) ==
        slot);
  CHECK(call(0x816D8590, slot) == 0);
  CHECK(xe::load_and_swap<uint64_t>(fixture.memory.TranslateVirtual(output)) ==
        expected_xuid);
  CHECK(call(0x816D8590, 4) == 0x80070057u);
  CHECK(xe::load_and_swap<uint64_t>(fixture.memory.TranslateVirtual(output)) ==
        0);
  // No bridge overlay is installed. The native cache is independent from the
  // generated image's immutable bytes and from the ordinary title-import HLE.
  fixture.CheckNoOverlay();
  CHECK(fixture.Hash(QueryAddress(0x816D8698), 12) ==
        "766cd6615f5d7580131ee3c03e89d7c9e83ea3a78027cf618f78909fa3886151");
  CHECK(fixture.Hash(QueryAddress(0x816D8590), 12) ==
        "766cd6615f5d7580131ee3c03e89d7c9e83ea3a78027cf618f78909fa3886151");
}
}  // namespace xe::cpu::test
