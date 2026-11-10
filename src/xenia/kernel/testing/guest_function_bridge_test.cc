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
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "third_party/catch/include/catch.hpp"
#include "third_party/mbedtls/include/mbedtls/sha1.h"
#include "third_party/mbedtls/include/mbedtls/sha256.h"
#include "xenia/base/byte_order.h"
#include "xenia/cpu/export_resolver.h"
#include "xenia/cpu/testing/util.h"
#include "xenia/cpu/xex_module.h"

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
std::vector<uint8_t> MakeXex(const ImageCustomization& customize) {
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

}  // namespace
}  // namespace xe::cpu::test
