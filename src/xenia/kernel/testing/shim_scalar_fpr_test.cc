/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia contributors. All rights reserved.                    *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <type_traits>

#include "third_party/catch/include/catch.hpp"
#include "xenia/apu/audio_ducker_policy.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_module.h"

namespace xe::kernel::shim::test {
namespace {

// Exercise the actual production export generator and its registered
// trampoline, not a hand-packed Param::Init or a substitute Kernel fixture.
// These value-only calls need no guest Memory, XThread, or KernelState.
template <uint16_t Ordinal, auto Function>
cpu::Export* Register(const char* name) {
  using Registration = std::remove_pointer_t<
      decltype(GetRegister<KernelModuleId::xboxkrnl, Ordinal>(Function))>;
  return Registration::template RegisterExport<
      Function, cpu::ExportTag::kImplemented | cpu::ExportTag::kHighFrequency>(
      name);
}

struct Context {
  Context() : old_logging(cvars::log_high_frequency_kernel_calls) {
    cvars::log_high_frequency_kernel_calls = false;
    for (size_t i = 0; i < gprs.size(); ++i) {
      gprs[i] = context.r[i] = 0xFEDCBA9800000100ull + i;
      context.f[i] = 100.5 + i;
    }
    SnapshotFprs();
  }
  ~Context() { cvars::log_high_frequency_kernel_calls = old_logging; }

  void SnapshotGprs() {
    for (size_t i = 0; i < gprs.size(); ++i) {
      gprs[i] = context.r[i];
    }
  }
  void SnapshotFprs() {
    for (size_t i = 0; i < fprs.size(); ++i) {
      fprs[i] = std::bit_cast<uint64_t>(context.f[i]);
    }
  }
  void CheckUnchanged(bool gpr_result, bool fpr_result) const {
    for (size_t i = 0; i < gprs.size(); ++i) {
      CAPTURE(i);
      if (!gpr_result || i != 3) {
        CHECK(context.r[i] == gprs[i]);
      }
      if (!fpr_result || i != 1) {
        CHECK(std::bit_cast<uint64_t>(context.f[i]) == fprs[i]);
      }
    }
  }
  void Invoke(cpu::Export* entry) {
    REQUIRE(entry);
    REQUIRE(entry->is_implemented());
    REQUIRE(entry->function_data.trampoline);
    entry->function_data.trampoline(&context);
  }

  const bool old_logging;
  PPCContext context{};
  std::array<uint64_t, 32> gprs{}, fprs{};
};

dword_result_t EchoFloat(float_t value) {
  return std::bit_cast<uint32_t>(static_cast<float>(value));
}
qword_result_t EchoDouble(double_t value) {
  return std::bit_cast<uint64_t>(static_cast<double>(value));
}
qword_result_t Mixed(dword_t first, float_t a, dword_t last, double_t b,
                     float_t c) {
  return (uint64_t(first) << 32) |
         uint32_t(last + float(a) * 8 + double(b) * 16 + float(c) * 4);
}
float_result_t SingleThird() { return std::bit_cast<float>(0x3EAAAAABu); }
double_result_t PreciseDouble() {
  return std::bit_cast<double>(0x3FF0000000000001ull);
}
float_result_t NegativeZero() { return std::bit_cast<float>(0x80000000u); }
dword_result_t DwordResult() { return 0x80000001u; }
int_result_t SignedResult() { return -7; }
qword_result_t QwordResult() { return 0xFEDCBA9876543210ull; }
double_result_t Thirteen(float_t a, double_t b, float_t c, double_t d,
                         float_t e, double_t f, float_t g, double_t h,
                         float_t i, double_t j, float_t k, double_t l,
                         float_t m) {
  return double(a) + 2 * double(b) + 3 * double(c) + 4 * double(d) +
         5 * double(e) + 6 * double(f) + 7 * double(g) + 8 * double(h) +
         9 * double(i) + 10 * double(j) + 11 * double(k) + 12 * double(l) +
         13 * double(m);
}

thread_local std::unique_ptr<apu::AudioDuckerPolicy> audio_policy;
dword_result_t StoreAudioLevel(float_t level) {
  return uint32_t(
      audio_policy->SetScalar(apu::AudioDuckerPolicy::Scalar::kLevel, level));
}
float_result_t LoadAudioLevel() { return audio_policy->Read()->level; }
}  // namespace

TEST_CASE("Scalar export arguments start at f1 for float and double",
          "[shim][fpr]") {
  Context fixture;
  SECTION("single uses f1 and its actual float precision") {
    fixture.context.f[1] = 1.0 + std::ldexp(1.0, -30);
    fixture.context.f[2] = -13.5;
    fixture.SnapshotFprs();
    fixture.Invoke(Register<0xF700, EchoFloat>("scalar-test-single"));
    CHECK(fixture.context.r[3] == 0x3F800000u);
  }
  SECTION("double preserves bits that would be lost by float narrowing") {
    fixture.context.f[1] = std::bit_cast<double>(0x3FF0000000000001ull);
    fixture.context.f[2] = -13.5;
    fixture.SnapshotFprs();
    fixture.Invoke(Register<0xF701, EchoDouble>("scalar-test-double"));
    CHECK(fixture.context.r[3] == 0x3FF0000000000001ull);
  }
  fixture.CheckUnchanged(true, false);
}

TEST_CASE("Mixed integer and floating export arguments keep both ABI slots",
          "[shim][fpr]") {
  Context fixture;
  fixture.context.r[3] = 0x10203040;
  fixture.context.r[4] = 0xDEAD0001;  // Float's reserved GPR home, not last.
  fixture.context.r[5] = 128;
  fixture.context.r[6] = 0xDEAD0002;
  fixture.context.r[7] = 0xDEAD0003;
  fixture.context.f[1] = 1.25;
  fixture.context.f[2] = 2.5;
  fixture.context.f[3] = -3.75;
  fixture.context.f[4] = 19.0;
  fixture.SnapshotGprs();
  fixture.SnapshotFprs();
  fixture.Invoke(Register<0xF702, Mixed>("scalar-test-mixed"));
  CHECK(fixture.context.r[3] == 0x10203040000000A3ull);
  fixture.CheckUnchanged(true, false);
}

TEST_CASE("Floating export results use only f1 and preserve GPR results",
          "[shim][fpr]") {
  Context fixture;
  SECTION("single is widened from its real float value") {
    fixture.Invoke(Register<0xF703, SingleThird>("scalar-test-float-result"));
    CHECK(fixture.context.f[1] == double(std::bit_cast<float>(0x3EAAAAABu)));
    CHECK(fixture.context.f[1] != 1.0 / 3.0);
  }
  SECTION("double is not narrowed to single precision") {
    fixture.Invoke(
        Register<0xF704, PreciseDouble>("scalar-test-double-result"));
    CHECK(std::bit_cast<uint64_t>(fixture.context.f[1]) ==
          0x3FF0000000000001ull);
  }
  SECTION("single negative zero keeps its sign") {
    fixture.Invoke(Register<0xF705, NegativeZero>("scalar-test-negative-zero"));
    CHECK(std::bit_cast<uint64_t>(fixture.context.f[1]) ==
          0x8000000000000000ull);
  }
  fixture.CheckUnchanged(false, true);
}

TEST_CASE("Integer export results keep their established GPR extension",
          "[shim][fpr]") {
  Context fixture;
  SECTION("DWORD keeps existing signed extension into r3") {
    fixture.Invoke(Register<0xF706, DwordResult>("scalar-test-dword"));
    CHECK(fixture.context.r[3] == 0xFFFFFFFF80000001ull);
  }
  SECTION("signed result keeps sign extension") {
    fixture.Invoke(Register<0xF707, SignedResult>("scalar-test-signed"));
    CHECK(fixture.context.r[3] == 0xFFFFFFFFFFFFFFF9ull);
  }
  SECTION("QWORD keeps all64 bits") {
    fixture.Invoke(Register<0xF708, QwordResult>("scalar-test-qword"));
    CHECK(fixture.context.r[3] == 0xFEDCBA9876543210ull);
  }
  fixture.CheckUnchanged(true, false);
}

TEST_CASE("Shared scalar FPR stream covers f1 through f13 in source order",
          "[shim][fpr]") {
  Context fixture;
  for (unsigned i = 1; i <= 13; ++i) {
    fixture.context.f[i] = double(i);
  }
  fixture.context.f[14] = 99.0;
  fixture.SnapshotFprs();
  fixture.Invoke(Register<0xF709, Thirteen>("scalar-test-thirteen"));
  CHECK(fixture.context.f[1] == 819.0);  // 1^2 + 2^2 + ... +13^2.
  fixture.CheckUnchanged(false, true);
}

TEST_CASE("Audio scalar state roundtrip uses the actual f1 trampoline ABI",
          "[shim][fpr][audio]") {
  audio_policy = std::make_unique<apu::AudioDuckerPolicy>();
  Context fixture;
  fixture.context.f[1] = 0.25;
  fixture.context.f[2] = 0.75;
  fixture.SnapshotFprs();
  fixture.Invoke(Register<0xF720, StoreAudioLevel>("audio-policy-store-f1"));
  REQUIRE(fixture.context.r[3] == 0);
  fixture.CheckUnchanged(true, false);
  fixture.SnapshotGprs();
  fixture.context.f[1] = -23;
  fixture.context.f[2] = 91;
  fixture.SnapshotFprs();
  fixture.Invoke(Register<0xF721, LoadAudioLevel>("audio-policy-read-f1"));
  CHECK(fixture.context.f[1] == 0.25);
  fixture.CheckUnchanged(false, true);
  audio_policy.reset();
}
TEST_CASE(
    "Confirmed audio ordinals register real trampolines and reject invalid f1",
    "[kernel][audio][abi]") {
  cpu::ExportResolver resolver;
  xboxkrnl::XboxkrnlModule::RegisterExportTable(&resolver);
  for (const uint16_t ordinal :
       {0x1F7, 0x1F8, 0x1F9, 0x34D, 0x34E, 0x350, 0x351, 0x353, 0x355, 0x357}) {
    const auto entry = resolver.GetExportByOrdinal("xboxkrnl.exe", ordinal);
    REQUIRE(entry);
    REQUIRE(entry->is_implemented());
    REQUIRE_FALSE(entry->tags & cpu::ExportTag::kStub);
    REQUIRE(entry->function_data.trampoline);
  }
  for (const uint16_t ordinal : {0x34F, 0x352, 0x354, 0x356, 0x358}) {
    const auto entry = resolver.GetExportByOrdinal("xboxkrnl.exe", ordinal);
    REQUIRE(entry);
    CHECK_FALSE(entry->is_implemented());
  }
  Context fixture;
  fixture.context.f[1] = -0.25;  // Invalid. f2 would be a valid setter value.
  fixture.context.f[2] = 0.5;
  fixture.SnapshotFprs();
  fixture.Invoke(resolver.GetExportByOrdinal("xboxkrnl.exe", 0x34E));
  CHECK(uint32_t(fixture.context.r[3]) == X_E_INVALIDARG);
  fixture.CheckUnchanged(true, false);
  fixture.context.r[3] = 2;
  fixture.SnapshotGprs();
  fixture.Invoke(resolver.GetExportByOrdinal("xboxkrnl.exe", 0x34D));
  CHECK(uint32_t(fixture.context.r[3]) == X_E_INVALIDARG);
  fixture.CheckUnchanged(true, false);
  // No substitute KernelState: these actual entrypoint rejections occur
  // before looking up a runtime owner. Successful kernel routing is a
  // separate real AudioSystem/native acceptance requirement.
}
}  // namespace xe::kernel::shim::test
