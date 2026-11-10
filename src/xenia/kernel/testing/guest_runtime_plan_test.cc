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
#include <iomanip>
#include <memory>
#include <span>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "third_party/catch/include/catch.hpp"
#include "third_party/mbedtls/include/mbedtls/sha1.h"
#include "third_party/mbedtls/include/mbedtls/sha256.h"
#include "xenia/kernel/guest_runtime_plan.h"

namespace xe::kernel::test {
namespace {

std::array<uint8_t, 32> Digest(std::span<const uint8_t> bytes) {
  std::array<uint8_t, 32> digest{};
  mbedtls_sha256(bytes.data(), bytes.size(), digest.data(), 0);
  return digest;
}

std::string Sha256(std::span<const uint8_t> bytes) {
  constexpr char kHex[] = "0123456789abcdef";
  std::string result;
  for (uint8_t byte : Digest(bytes)) {
    result += kHex[byte >> 4];
    result += kHex[byte & 15];
  }
  return result;
}

std::string Sha1(std::span<const uint8_t> bytes) {
  std::array<uint8_t, 20> digest{};
  mbedtls_sha1(bytes.data(), bytes.size(), digest.data());
  constexpr char kHex[] = "0123456789abcdef";
  std::string result;
  for (uint8_t byte : digest) {
    result += kHex[byte >> 4];
    result += kHex[byte & 15];
  }
  return result;
}

class LoadedImage {
 public:
  LoadedImage() {
    for (size_t i = 0; i < bytes.size(); ++i) {
      bytes[i] = uint8_t((i * 37 + 17) ^ (i >> 2));
    }
  }
  static constexpr uint32_t kBase = 0x90010000;
  std::array<uint8_t, 0x200> bytes{};
  uint32_t export_rva = 0x80;
  std::vector<std::pair<uint32_t, uint32_t>> code_queries, data_queries;
  std::vector<uint16_t> export_queries;

  uint64_t Hash() const {
    // Deliberately opaque/constant: full-code identity must protect code that
    // an existing incomplete legacy module fingerprint does not distinguish.
    return 0x123456789ABCDEF0;
  }
  std::string CodeSha1() const {
    return Sha1(std::span<const uint8_t>(bytes).subspan(0x40, 0x60));
  }
  GuestRuntimeImageView View() {
    GuestRuntimeImageView view;
    view.base = kBase;
    view.module_hash = Hash();
    view.code_sha1 = CodeSha1();
    view.bytes = bytes;
    view.is_code = [this](uint32_t address, uint32_t size) {
      code_queries.emplace_back(address, size);
      return address >= kBase + 0x40 && size &&
             uint64_t(address) + size <= uint64_t(kBase) + 0xA0;
    };
    view.is_data = [this](uint32_t address, uint32_t size) {
      data_queries.emplace_back(address, size);
      return address >= kBase + 0x100 && size &&
             uint64_t(address) + size <= uint64_t(kBase) + 0x180;
    };
    view.export_address = [this](uint16_t ordinal) {
      export_queries.push_back(ordinal);
      return ordinal == 7 ? kBase + export_rva : 0;
    };
    return view;
  }
  std::string Header() const {
    std::ostringstream out;
    out << "schema = 1\nidentity = 'loaded fixture'\nmodule_hash = '"
        << std::hex << std::setw(16) << std::setfill('0') << Hash()
        << "'\nimage_code_sha1 = '" << CodeSha1()
        << "'\nimage_size = 512\nlifetime = 'kernel'\n";
    return out.str();
  }
  std::string RvaSelector() const {
    return "rva = 64\ncode_size = 16\ncode_sha256 = '" +
           Sha256(std::span<const uint8_t>(bytes).subspan(0x40, 16)) + "'\n";
  }
};

std::string Call(std::string_view identity, std::string_view selector,
                 std::string_view arguments = "[]",
                 std::string_view result = "void",
                 std::string_view checks = {}) {
  return "[[phase.call]]\nidentity = '" + std::string(identity) + "'\n" +
         std::string(selector) + "arguments = " + std::string(arguments) +
         "\nresult = '" + std::string(result) + "'\n" + std::string(checks);
}

std::string OneCall(const LoadedImage& image, std::string_view checks = {}) {
  return image.Header() + "[[phase]]\nidentity = 'startup'\n" +
         Call("entry", image.RvaSelector(), "[]", "void", checks);
}

std::string InternalBridge(const LoadedImage& image) {
  return "[[bridge]]\nservice = 'active_title_id'\n" + image.RvaSelector() +
         "arguments = []\nresult = 'uint32'\n";
}

TEST_CASE("Runtime internal bridge binds only verified allowlisted ABI",
          "[kernel][guest_runtime_plan]") {
  LoadedImage image;
  const auto text = image.Header() + InternalBridge(image) +
                    "[[phase]]\nidentity = 'startup'\n" +
                    Call("entry", image.RvaSelector());
  std::string error;
  const auto parsed = ParseGuestRuntimePlan(text, image.View(), &error);
  REQUIRE(parsed);
  REQUIRE(parsed->bridges.size() == 1);
  const auto& bridge = parsed->bridges.front();
  CHECK(bridge.address == LoadedImage::kBase + 0x40);
  CHECK(bridge.code_size == 16);
  CHECK(bridge.code_sha256 ==
        Sha256(std::span<const uint8_t>(image.bytes).subspan(0x40, 16)));
  CHECK(bridge.service == "active_title_id");
  CHECK(bridge.library.empty());
  CHECK(bridge.ordinal == 0);
}

std::string Replace(std::string value, std::string_view before,
                    std::string_view after) {
  const size_t offset = value.find(before);
  REQUIRE(offset != std::string::npos);
  value.replace(offset, before.size(), after);
  return value;
}

void Rejected(std::string_view text, const GuestRuntimeImageView& image) {
  std::string error;
  CHECK_FALSE(ParseGuestRuntimePlan(text, image, &error));
  CHECK_FALSE(error.empty());
}

TEST_CASE("Runtime internal bridge rejects identity, service and ABI drift",
          "[kernel][guest_runtime_plan]") {
  LoadedImage image;
  const auto bridge = InternalBridge(image);
  const auto suffix =
      "[[phase]]\nidentity = 'startup'\n" + Call("entry", image.RvaSelector());
  const auto text = image.Header() + bridge + suffix;
  SECTION("code bytes changed") {
    auto view = image.View();
    image.bytes[0x44] ^= 1;
    Rejected(text, view);
  }
  SECTION("hash changed") {
    const auto sha =
        Sha256(std::span<const uint8_t>(image.bytes).subspan(0x40, 16));
    Rejected(Replace(text, sha, std::string(64, '0')), image.View());
  }
  SECTION("unaligned address") {
    Rejected(Replace(text, "rva = 64", "rva = 65"), image.View());
  }
  SECTION("undersized function") {
    Rejected(Replace(text, "code_size = 16", "code_size = 4"), image.View());
  }
  SECTION("floating selector") {
    Rejected(Replace(text, "rva = 64", "rva = 64.0"), image.View());
  }
  SECTION("unknown service") {
    Rejected(Replace(text, "active_title_id", "arbitrary_host_call"),
             image.View());
  }
  SECTION("incompatible arguments") {
    Rejected(Replace(text, "arguments = []", "arguments = [1]"), image.View());
  }
  SECTION("incompatible result") {
    Rejected(Replace(text, "result = 'uint32'", "result = 'void'"),
             image.View());
  }
  SECTION("selector mixing") {
    Rejected(Replace(text, "service = 'active_title_id'",
                     "service = 'active_title_id'\nordinal = 7"),
             image.View());
  }
  SECTION("duplicate function") {
    Rejected(image.Header() + bridge + bridge + suffix, image.View());
  }
}

TEST_CASE("Deferred native ABI catalog supports bounded stack arguments",
          "[kernel][guest_runtime_plan]") {
  LoadedImage image;
  const std::string arguments = "[0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15]";
  const auto deferred =
      "[[phase]]\nidentity = 'stack API'\ndeferred = true\n" +
      Call("stack header", image.RvaSelector(), arguments, "any");
  const auto text = OneCall(image) + deferred;
  std::string error;
  const auto parsed = ParseGuestRuntimePlan(text, image.View(), &error);
  REQUIRE(parsed);
  REQUIRE(parsed->plan.phases.size() == 1);
  CHECK(parsed->plan.phases[0].functions.size() == 1);
  REQUIRE(parsed->catalog.at("stack header").arguments.size() == 16);
  CHECK(parsed->catalog.at("stack header").arguments.back() == 15);
  Rejected(Replace(text, "deferred = true", "deferred = false"), image.View());
  Rejected(
      Replace(text, arguments, "[0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16]"),
      image.View());
}

TEST_CASE(
    "Runtime TOML preserves loaded selectors, ABI and duplicate address order",
    "[kernel][guest_runtime_plan]") {
  LoadedImage image;
  const auto text =
      image.Header() +
      "[[bridge]]\nlibrary = 'xam.xex'\nordinal = 7\n"
      "[[phase]]\nidentity = 'heap'\n" +
      Call("rva entry", image.RvaSelector(), "[0, 4294967295]", "zero") +
      Call("export entry", "ordinal = 7\n", "[]", "hresult") +
      "[[phase]]\nidentity = 'constructors'\n" +
      Call("same address second entry", image.RvaSelector());
  std::string error = "old error";
  const auto parsed = ParseGuestRuntimePlan(text, image.View(), &error);
  REQUIRE(parsed);
  CHECK(error.empty());
  REQUIRE(parsed->plan.phases.size() == 2);
  REQUIRE(parsed->plan.phases[0].functions.size() == 2);
  REQUIRE(parsed->plan.phases[1].functions.size() == 1);
  const auto& first = parsed->plan.phases[0].functions[0];
  const auto& second = parsed->plan.phases[0].functions[1];
  const auto& third = parsed->plan.phases[1].functions[0];
  CHECK(first.identity == "rva entry");
  CHECK(second.identity == "export entry");
  CHECK(third.identity == "same address second entry");
  CHECK(first.address == LoadedImage::kBase + 0x40);
  CHECK(second.address == LoadedImage::kBase + 0x80);
  CHECK(third.address == first.address);
  CHECK((first.arguments == std::vector<uint64_t>{0, 0xFFFFFFFF}));
  CHECK(second.return_kind == GuestRuntime::ReturnKind::kHResult);
  CHECK(third.return_kind == GuestRuntime::ReturnKind::kVoid);
  CHECK_FALSE(first.abi_metadata.empty());
  CHECK(parsed->contracts.size() == 3);
  CHECK(parsed->contracts.at("rva entry").result ==
        ValidatedGuestRuntimePlan::ResultPolicy::kZero);
  CHECK(parsed->contracts.at("export entry").result ==
        ValidatedGuestRuntimePlan::ResultPolicy::kHResult);
  REQUIRE(parsed->bridges.size() == 1);
  CHECK(parsed->bridges[0].library == "xam.xex");
  CHECK(parsed->bridges[0].ordinal == 7);
  REQUIRE(image.export_queries.size() == 1);
  CHECK(image.export_queries[0] == 7);
  GuestRuntime runtime({LoadedImage::kBase, 1}, std::make_shared<int>(1));
  std::vector<std::string> executed;
  CHECK(runtime.Initialize(parsed->plan, [&](const auto& function) {
    executed.push_back(function.identity);
    return GuestRuntime::ExecutionResult{true, 0, {}};
  }) == GuestRuntime::InitStatus::kReady);
  CHECK((executed == std::vector<std::string>{"rva entry", "export entry",
                                              "same address second entry"}));
}

TEST_CASE(
    "Deferred native entries are validated without entering initialization",
    "[kernel][guest_runtime_plan]") {
  LoadedImage image;
  const std::string deferred_selector =
      "rva = 96\ncode_size = 8\ncode_sha256 = '" +
      Sha256(std::span<const uint8_t>(image.bytes).subspan(0x60, 8)) + "'\n";
  const auto deferred_phase =
      "[[phase]]\nidentity = 'owned render API'\ndeferred = true\n" +
      Call("deferred z", deferred_selector, "[7]", "hresult",
           "before = [{rva = 256, nonzero = true}]\n") +
      Call("deferred a", "ordinal = 7\n");
  const auto text = image.Header() + "[[phase]]\nidentity = 'heap'\n" +
                    Call("start z", image.RvaSelector()) +
                    Call("start a", image.RvaSelector()) + deferred_phase +
                    "[[phase]]\nidentity = 'constructors'\ndeferred = false\n" +
                    Call("finish", image.RvaSelector());
  std::string error;
  const auto parsed = ParseGuestRuntimePlan(text, image.View(), &error);
  REQUIRE(parsed);
  CHECK(error.empty());
  REQUIRE(parsed->catalog.size() == 5);
  const auto& deferred = parsed->catalog.at("deferred z");
  CHECK(deferred.address == LoadedImage::kBase + 0x60);
  CHECK(deferred.arguments == std::vector<uint64_t>{7});
  CHECK(deferred.return_kind == GuestRuntime::ReturnKind::kHResult);
  CHECK(parsed->catalog.contains("deferred a"));
  const auto& contract = parsed->contracts.at("deferred z");
  CHECK(contract.result == ValidatedGuestRuntimePlan::ResultPolicy::kHResult);
  REQUIRE(contract.before.size() == 1);
  CHECK(contract.before[0].address == LoadedImage::kBase + 0x100);
  CHECK(contract.before[0].nonzero);
  REQUIRE(parsed->plan.phases.size() == 2);
  CHECK(parsed->plan.phases[0].identity == "heap");
  CHECK(parsed->plan.phases[1].identity == "constructors");

  GuestRuntime runtime({LoadedImage::kBase, 1}, std::make_shared<int>(1));
  std::vector<std::string> executed;
  CHECK(runtime.Initialize(parsed->plan, [&](const auto& function) {
    executed.push_back(function.identity);
    return GuestRuntime::ExecutionResult{true, 0, {}};
  }) == GuestRuntime::InitStatus::kReady);
  CHECK((executed == std::vector<std::string>{"start z", "start a", "finish"}));
  CHECK(runtime.GetSnapshot().total_functions == 3);

  // Deferred ABI changes also change the initialized generation's contract.
  const auto changed =
      ParseGuestRuntimePlan(Replace(text, "arguments = [7]", "arguments = [8]"),
                            image.View(), &error);
  REQUIRE(changed);
  CHECK(runtime.Initialize(changed->plan, [](const auto&) {
    FAIL("A changed deferred contract must not reenter initialization");
    return GuestRuntime::ExecutionResult{};
  }) == GuestRuntime::InitStatus::kPlanMismatch);

  Rejected(image.Header() + deferred_phase, image.View());
  for (const auto* malformed : {"'true'", "1", "[]"}) {
    Rejected(Replace(text, "deferred = true",
                     "deferred = " + std::string(malformed)),
             image.View());
  }
  Rejected(
      Replace(
          text, deferred_selector,
          Replace(
              deferred_selector,
              Sha256(std::span<const uint8_t>(image.bytes).subspan(0x60, 8)),
              std::string(64, '0'))),
      image.View());
}

TEST_CASE(
    "Runtime plans bind full code identity independently of the legacy hash",
    "[kernel][guest_runtime_plan]") {
  LoadedImage image;
  const auto text = OneCall(image);
  REQUIRE(ParseGuestRuntimePlan(text, image.View(), nullptr));
  SECTION("Unselected code byte changes authoritative fingerprint") {
    const uint64_t old_hash = image.Hash();
    image.bytes[0x90] ^= 0x40;  // Not in the selected RVA signature range.
    CHECK(image.Hash() == old_hash);
    Rejected(text, image.View());
  }
  SECTION("Legacy metadata mismatch is still rejected") {
    auto view = image.View();
    view.module_hash ^= 1;
    Rejected(text, view);
  }
  SECTION("Authoritative code fingerprint is mandatory and well formed") {
    Rejected(Replace(text, "image_code_sha1 = '" + image.CodeSha1() + "'", ""),
             image.View());
    Rejected(Replace(text, image.CodeSha1(), std::string(39, '0')),
             image.View());
    Rejected(Replace(text, image.CodeSha1(), std::string(40, 'z')),
             image.View());
    auto view = image.View();
    view.code_sha1.clear();
    Rejected(text, view);
  }
  SECTION("Loaded size mismatch") {
    Rejected(Replace(text, "image_size = 512", "image_size = 511"),
             image.View());
  }
  SECTION("Resident lifetime must be explicit") {
    Rejected(Replace(text, "lifetime = 'kernel'", "lifetime = 'title'"),
             image.View());
  }
  SECTION("Wrapped loaded image span") {
    auto view = image.View();
    view.base = 0xFFFFFFF0;
    Rejected(text, view);
  }
}

TEST_CASE("RVA startup signatures are checked against actual loaded code bytes",
          "[kernel][guest_runtime_plan]") {
  LoadedImage image;
  const auto selector = image.RvaSelector();
  image.bytes[0x45] ^= 0x80;
  // Refresh the full-code identity; the stale RVA signature must still fail.
  Rejected(image.Header() + "[[phase]]\nidentity = 'startup'\n" +
               Call("entry", selector),
           image.View());
  auto valid = OneCall(image);
  REQUIRE(ParseGuestRuntimePlan(valid, image.View(), nullptr));
  Rejected(Replace(valid, "code_size = 16", "code_size = 0"), image.View());
  Rejected(Replace(valid, "code_size = 16", "code_size = 17"), image.View());
  const auto hash =
      Sha256(std::span<const uint8_t>(image.bytes).subspan(0x40, 16));
  Rejected(Replace(valid, hash, std::string(64, 'z')), image.View());
}

TEST_CASE("Runtime selectors cannot escape loaded code or substitute data",
          "[kernel][guest_runtime_plan]") {
  LoadedImage image;
  const auto text = OneCall(image);
  Rejected(Replace(text, "rva = 64", "rva = 256"), image.View());
  Rejected(Replace(text, "rva = 64", "rva = 65"), image.View());
  Rejected(Replace(text, "rva = 64", "rva = 512"), image.View());
  auto permissive = image.View();
  permissive.is_code = [](uint32_t, uint32_t) { return true; };
  // Even a permissive owner callback must not permit a hash read past bytes.
  Rejected(Replace(text, "rva = 64", "rva = 508"), permissive);
  Rejected(Replace(text, "rva = 64", "rva = 4294967292"), permissive);
  const auto exported = image.Header() + "[[phase]]\nidentity = 'export'\n" +
                        Call("entry", "ordinal = 7\n");
  image.export_rva = 0x100;
  Rejected(exported, image.View());
  image.export_rva = 0x300;
  Rejected(exported, image.View());
  image.export_rva = 0x81;
  Rejected(exported, image.View());
}

TEST_CASE("Runtime TOML rejects ambiguous or malformed call selectors",
          "[kernel][guest_runtime_plan]") {
  LoadedImage image;
  const auto text = OneCall(image);
  Rejected(Replace(text, "rva = 64", "rva = 64\nordinal = 7"), image.View());
  Rejected(Replace(text, "rva = 64", "rva = 'bad'\nordinal = 7"), image.View());
  Rejected(Replace(text, "rva = 64", "rva = 64\nordinal = 'bad'"),
           image.View());
  Rejected(Replace(text, "rva = 64", "ordinal = 0"), image.View());
  Rejected(Replace(text, "rva = 64", "ordinal = 65536"), image.View());
  Rejected(Replace(text, "rva = 64", "ordinal = 8"), image.View());
  Rejected(Replace(text, "rva = 64", ""), image.View());
}

TEST_CASE(
    "Runtime ABI is explicit and bounds register argument values and count",
    "[kernel][guest_runtime_plan]") {
  LoadedImage image;
  const auto prefix = image.Header() + "[[phase]]\nidentity = 'startup'\n";
  std::string error;
  const auto valid =
      ParseGuestRuntimePlan(prefix + Call("entry", image.RvaSelector(),
                                          "[0,1,2,3,4,5,6,4294967295]", "any"),
                            image.View(), &error);
  REQUIRE(valid);
  CHECK(valid->plan.phases[0].functions[0].arguments.size() == 8);
  CHECK(valid->contracts.at("entry").result ==
        ValidatedGuestRuntimePlan::ResultPolicy::kAny);
  const auto nonzero = ParseGuestRuntimePlan(
      prefix + Call("entry", image.RvaSelector(), "[]", "nonzero"),
      image.View(), nullptr);
  REQUIRE(nonzero);
  CHECK(nonzero->contracts.at("entry").result ==
        ValidatedGuestRuntimePlan::ResultPolicy::kNonzero);
  for (auto args : {"[0,1,2,3,4,5,6,7,8]", "[-1]", "[4294967296]", "[1.5]",
                    "[true]", "['pointer']"}) {
    CAPTURE(args);
    Rejected(prefix + Call("entry", image.RvaSelector(), args), image.View());
  }
  Rejected(prefix + Call("entry", image.RvaSelector(), "[]", "float"),
           image.View());
  Rejected(Replace(OneCall(image), "arguments = []", ""), image.View());
  Rejected(Replace(OneCall(image), "result = 'void'", ""), image.View());
}

TEST_CASE(
    "Runtime word checks retain exact data ranges and comparison semantics",
    "[kernel][guest_runtime_plan]") {
  LoadedImage image;
  const auto parsed = ParseGuestRuntimePlan(
      OneCall(image,
              "before = [{rva=0x100, expected=0}, {rva=0x17C, nonzero=true}]\n"
              "after = [{rva=0x104, expected=0xFFFFFFFF}]\n"),
      image.View(), nullptr);
  REQUIRE(parsed);
  const auto& contract = parsed->contracts.at("entry");
  REQUIRE(contract.before.size() == 2);
  REQUIRE(contract.after.size() == 1);
  CHECK(contract.before[0].address == LoadedImage::kBase + 0x100);
  CHECK(contract.before[0].expected == 0);
  CHECK_FALSE(contract.before[0].nonzero);
  CHECK(contract.before[1].address == LoadedImage::kBase + 0x17C);
  CHECK(contract.before[1].nonzero);
  CHECK(contract.after[0].expected == 0xFFFFFFFF);
  CHECK_FALSE(contract.after[0].nonzero);
  REQUIRE(image.data_queries.size() == 3);
  const std::array<uint32_t, 3> addresses = {0x100, 0x17C, 0x104};
  for (size_t i = 0; i < addresses.size(); ++i) {
    CHECK(image.data_queries[i].first == LoadedImage::kBase + addresses[i]);
    CHECK(image.data_queries[i].second == 4);
  }
}

TEST_CASE("Runtime word checks reject unknown, conflicting and unowned reads",
          "[kernel][guest_runtime_plan]") {
  LoadedImage image;
  for (auto check :
       {"{rva=0x100}", "{rva=0x100,expected=0,nonzero=true}",
        "{rva=0x100,expected='bad',nonzero=true}",
        "{rva=0x100,expected=0,nonzero='bad'}",
        "{rva=0x100,expected=-1,nonzero=true}", "{rva=0x101,expected=0}",
        "{rva=0x180,expected=0}", "{rva=0x40,expected=0}",
        "{rva=0x200,expected=0}", "{rva=0xFFFFFFFF,expected=0}"}) {
    CAPTURE(check);
    Rejected(OneCall(image, "before = [" + std::string(check) + "]\n"),
             image.View());
  }
  Rejected(OneCall(image, "before = {rva=0x100,expected=0}\n"), image.View());
}

TEST_CASE(
    "Changing a parsed postcondition cannot reuse prior runtime readiness",
    "[kernel][guest_runtime_plan]") {
  LoadedImage image;
  const auto first = ParseGuestRuntimePlan(
      OneCall(image, "after = [{rva=0x104,expected=0}]\n"), image.View(),
      nullptr);
  const auto changed = ParseGuestRuntimePlan(
      OneCall(image, "after = [{rva=0x104,expected=1}]\n"), image.View(),
      nullptr);
  REQUIRE(first);
  REQUIRE(changed);
  CHECK(first->plan.identity == changed->plan.identity);
  CHECK(first->plan != changed->plan);
  CHECK(first->contracts.at("entry").after[0].expected == 0);
  CHECK(changed->contracts.at("entry").after[0].expected == 1);
  GuestRuntime runtime({LoadedImage::kBase, 1}, std::make_shared<int>(1));
  auto execute = [](const auto&) {
    return GuestRuntime::ExecutionResult{true, 0, {}};
  };
  REQUIRE(runtime.Initialize(first->plan, execute) ==
          GuestRuntime::InitStatus::kReady);
  CHECK(runtime.Initialize(changed->plan, execute) ==
        GuestRuntime::InitStatus::kPlanMismatch);
}

TEST_CASE("Malformed runtime TOML and duplicate call identities cannot execute",
          "[kernel][guest_runtime_plan]") {
  LoadedImage image;
  Rejected("[[phase", image.View());
  Rejected(image.Header() + "phase=[]\n", image.View());
  Rejected(OneCall(image) + Call("entry", image.RvaSelector()), image.View());
}

}  // namespace
TEST_CASE(
    "Runtime module operands use the loader handle rather than image base",
    "[guest_runtime_plan]") {
  LoadedImage image;
  auto view = image.View();
  view.hmodule = 0x00ABCDF0;
  const auto source =
      image.Header() + "[[phase]]\nidentity='loader'\n" +
      Call("set identity", image.RvaSelector(), "['module_handle']", "void",
           "[[phase.call.after]]\nrva=256\nexpected='module_handle'\n");
  std::string error;
  const auto parsed = ParseGuestRuntimePlan(source, view, &error);
  REQUIRE(parsed);
  CHECK(parsed->plan.phases[0].functions[0].arguments ==
        std::vector<uint64_t>{view.hmodule});
  CHECK(parsed->contracts.at("set identity").after[0].expected == view.hmodule);
  CHECK(view.hmodule != view.base);
  view.hmodule = 0;
  Rejected(source, view);
  view.hmodule = 0x00ABCDF0;
  Rejected(Replace(source, "['module_handle']", "['image_base']"), view);
  Rejected(Replace(source, "expected='module_handle'", "expected='image_base'"),
           view);
  Rejected(Replace(source, "['module_handle']", "[true]"), view);
  Rejected(Replace(source, "expected='module_handle'", "expected=true"), view);
}

TEST_CASE(
    "Deferred APIs require explicit permission for original TITLE callers",
    "[kernel][guest_runtime_plan][caller_policy]") {
  LoadedImage image;
  const auto ordinary = OneCall(image) +
                        "[[phase]]\nidentity='public API'\ndeferred=true\n" +
                        Call("public", image.RvaSelector(), "[0]", "hresult");
  const auto system = ParseGuestRuntimePlan(ordinary, image.View(), nullptr);
  REQUIRE(system);
  CHECK(system->contracts.at("entry").caller ==
        ValidatedGuestRuntimePlan::CallerPolicy::kSystemOnly);
  CHECK(system->contracts.at("public").caller ==
        ValidatedGuestRuntimePlan::CallerPolicy::kSystemOnly);
  const auto original = ParseGuestRuntimePlan(ordinary + "caller='original'\n",
                                              image.View(), nullptr);
  REQUIRE(original);
  CHECK(original->contracts.at("public").caller ==
        ValidatedGuestRuntimePlan::CallerPolicy::kOriginalTitleOrSystem);
  Rejected(OneCall(image) + "caller='original'\n", image.View());
  const auto task = ParseGuestRuntimePlan(ordinary + "caller='system_task'\n",
                                          image.View(), nullptr);
  REQUIRE(task);
  CHECK(task->contracts.at("public").caller ==
        ValidatedGuestRuntimePlan::CallerPolicy::kSystemTaskOnly);
  Rejected(OneCall(image) + "caller='system_task'\n", image.View());
  Rejected(ordinary + "caller='SYSTEM-or-fake-task'\n", image.View());
  Rejected(ordinary + "caller=true\n", image.View());
}

TEST_CASE("Public nine-DWORD metadata stays deferred and preserves raw status",
          "[kernel][guest_runtime_plan][caller_policy]") {
  LoadedImage image;
  const auto text =
      OneCall(image) +
      "[[phase]]\nidentity='original public UI'\ndeferred=true\n" +
      Call("messagebox", image.RvaSelector(), "[0,0,0,0,0,0,0,0,0]", "any",
           "caller='original'\n");
  const auto parsed = ParseGuestRuntimePlan(text, image.View(), nullptr);
  REQUIRE(parsed);
  CHECK(parsed->catalog.at("messagebox").arguments.size() == 9);
  CHECK(parsed->catalog.at("messagebox").return_kind ==
        GuestRuntime::ReturnKind::kUInt32);
  CHECK(parsed->contracts.at("messagebox").result ==
        ValidatedGuestRuntimePlan::ResultPolicy::kAny);
  CHECK(parsed->contracts.at("messagebox").caller ==
        ValidatedGuestRuntimePlan::CallerPolicy::kOriginalTitleOrSystem);
  REQUIRE(parsed->plan.phases.size() == 1);
  REQUIRE(parsed->plan.phases[0].functions.size() == 1);
  CHECK(parsed->plan.phases[0].functions[0].identity == "entry");
  Rejected(Replace(text, "deferred=true", "deferred=false"), image.View());
}

TEST_CASE(
    "Original-caller permission cannot be added to an already Ready heap plan",
    "[kernel][guest_runtime_plan][caller_policy]") {
  LoadedImage image;
  const auto text = OneCall(image) +
                    "[[phase]]\nidentity='public API'\ndeferred=true\n" +
                    Call("public", image.RvaSelector(), "[]", "hresult");
  const auto system = ParseGuestRuntimePlan(text, image.View(), nullptr);
  const auto original = ParseGuestRuntimePlan(text + "caller='original'\n",
                                              image.View(), nullptr);
  REQUIRE(system);
  REQUIRE(original);
  REQUIRE(system->plan != original->plan);
  GuestRuntime heap({LoadedImage::kBase, 1}, std::make_shared<int>(1));
  unsigned calls = 0;
  const auto execute = [&](const auto&) {
    ++calls;
    return GuestRuntime::ExecutionResult{true, 0, {}};
  };
  REQUIRE(heap.Initialize(system->plan, execute) ==
          GuestRuntime::InitStatus::kReady);
  CHECK(heap.Initialize(original->plan, execute) ==
        GuestRuntime::InitStatus::kPlanMismatch);
  CHECK(calls == 1);
}

TEST_CASE(
    "Service startup uses a distinct once-only record and cannot extend heap "
    "readiness",
    "[kernel][guest_runtime][service_startup]") {
  LoadedImage image;
  const auto parsed =
      ParseGuestRuntimePlan(OneCall(image), image.View(), nullptr);
  REQUIRE(parsed);
  auto pin = std::make_shared<int>(1);
  GuestRuntime heap({LoadedImage::kBase, 1}, pin);
  GuestRuntime service({LoadedImage::kBase, 1}, pin);
  GuestRuntime frontend({LoadedImage::kBase, 1}, pin);
  unsigned heap_calls = 0, service_calls = 0;
  const auto init_heap = [&](const auto&) {
    ++heap_calls;
    return GuestRuntime::ExecutionResult{true, 0, {}};
  };
  auto service_plan = parsed->plan;
  service_plan.identity = "separate service record";
  const auto fail_service = [&](const auto&) {
    ++service_calls;
    return GuestRuntime::ExecutionResult{false, 1,
                                         "actual postcondition failed"};
  };
  REQUIRE(heap.Initialize(parsed->plan, init_heap) ==
          GuestRuntime::InitStatus::kReady);
  REQUIRE(service.Initialize(service_plan, fail_service) ==
          GuestRuntime::InitStatus::kFailed);
  CHECK(service.Initialize(service_plan, fail_service) ==
        GuestRuntime::InitStatus::kFailed);
  CHECK(heap.Initialize(service_plan, init_heap) ==
        GuestRuntime::InitStatus::kPlanMismatch);
  CHECK(heap.Initialize(parsed->plan, init_heap) ==
        GuestRuntime::InitStatus::kAlreadyReady);
  CHECK(heap_calls == 1);
  CHECK(service_calls == 1);
  CHECK(heap.GetSnapshot().state == GuestRuntime::State::kReady);
  CHECK(service.GetSnapshot().state == GuestRuntime::State::kFailed);
  auto frontend_plan = service_plan;
  frontend_plan.identity = "separate frontend subtransaction";
  unsigned frontend_calls = 0;
  const auto init_frontend = [&](const auto&) {
    ++frontend_calls;
    return GuestRuntime::ExecutionResult{true, 0, {}};
  };
  CHECK(frontend.Initialize(frontend_plan, init_frontend) ==
        GuestRuntime::InitStatus::kReady);
  CHECK(frontend.Initialize(frontend_plan, init_frontend) ==
        GuestRuntime::InitStatus::kAlreadyReady);
  CHECK(frontend_calls == 1);
  CHECK(service.Initialize(frontend_plan, init_frontend) ==
        GuestRuntime::InitStatus::kFailed);
  CHECK(heap.GetSnapshot().state == GuestRuntime::State::kReady);
}


TEST_CASE("Managed format bridge accepts only its exact retained request ABI",
          "[kernel][guest_runtime_plan][managed_format]") {
  // Actual17559 formatter bytes, authenticated in the source evidence. Parsing
  // this synthetic image never executes raw format or claims a Kernel owner.
  const std::array<uint8_t, 268> format{
      0x7D, 0x88, 0x02, 0xA6, 0x91, 0x81, 0xFF, 0xF8, 0xFB, 0xC1, 0xFF, 0xE8, 0xFB, 0xE1, 0xFF, 0xF0,
      0x94, 0x21, 0xFE, 0x90, 0x38, 0x80, 0xFF, 0xFF, 0x4B, 0xFF, 0x5F, 0x71, 0x7C, 0x7F, 0x1B, 0x79,
      0x41, 0x82, 0x00, 0xC8, 0x81, 0x7F, 0x00, 0x7C, 0x2B, 0x0B, 0x00, 0x02, 0x41, 0x9A, 0x00, 0x20,
      0x2B, 0x0B, 0x00, 0x10, 0x41, 0x9A, 0x00, 0x18, 0x2B, 0x0B, 0x00, 0x01, 0x41, 0x9A, 0x00, 0x10,
      0x3F, 0xC0, 0x80, 0x07, 0x63, 0xDE, 0x00, 0x57, 0x48, 0x00, 0x00, 0xA8, 0x7F, 0xE3, 0xFB, 0x78,
      0x4B, 0xFF, 0xFD, 0x31, 0x7C, 0x7E, 0x1B, 0x79, 0x41, 0x80, 0x00, 0x98, 0x81, 0x7F, 0x00, 0x7C,
      0x2B, 0x0B, 0x00, 0x02, 0x40, 0x9A, 0x00, 0x8C, 0x39, 0x41, 0x00, 0x50, 0x7F, 0xEB, 0xFB, 0x78,
      0x7D, 0x5F, 0x50, 0x50, 0x89, 0x2B, 0x00, 0x00, 0x7D, 0x28, 0x07, 0x75, 0x7D, 0x2A, 0x59, 0xAE,
      0x39, 0x6B, 0x00, 0x01, 0x40, 0x82, 0xFF, 0xF0, 0x39, 0x61, 0x00, 0x50, 0x7D, 0x6A, 0x5B, 0x78,
      0x89, 0x2B, 0x00, 0x00, 0x39, 0x6B, 0x00, 0x01, 0x2B, 0x09, 0x00, 0x00, 0x40, 0x9A, 0xFF, 0xF4,
      0x7D, 0x6A, 0x58, 0x50, 0x39, 0x41, 0x00, 0x50, 0x39, 0x6B, 0xFF, 0xFF, 0x3D, 0x20, 0x81, 0x5F,
      0x55, 0x6B, 0x00, 0x3E, 0x38, 0xA0, 0x00, 0x01, 0x7D, 0x6B, 0x52, 0x14, 0x38, 0x81, 0x01, 0x84,
      0xE9, 0x49, 0x3A, 0xB4, 0x39, 0x6B, 0xFF, 0xFF, 0x38, 0x61, 0x00, 0x50, 0xF9, 0x4B, 0x00, 0x00,
      0x4B, 0xFF, 0x60, 0x01, 0x28, 0x03, 0x00, 0x00, 0x41, 0x82, 0x00, 0x18, 0x4B, 0xFF, 0xFC, 0xA5,
      0x7C, 0x7E, 0x1B, 0x78, 0x48, 0x00, 0x00, 0x0C, 0x3F, 0xC0, 0x80, 0x07, 0x63, 0xDE, 0x04, 0x8F,
      0x7F, 0xC3, 0xF3, 0x78, 0x38, 0x21, 0x01, 0x70, 0x81, 0x81, 0xFF, 0xF8, 0x7D, 0x88, 0x03, 0xA6,
      0xEB, 0xC1, 0xFF, 0xE8, 0xEB, 0xE1, 0xFF, 0xF0, 0x4E, 0x80, 0x00, 0x20,
  };
  std::vector<uint8_t> bytes(0x100 + format.size());
  std::copy(format.begin(), format.end(), bytes.begin() + 0x100);
  GuestRuntimeImageView view;
  view.base = 0x81684478; view.module_hash = 1;
  view.code_sha1 = Sha1(bytes); view.bytes = bytes;
  view.is_code = [&](uint32_t at, uint32_t count) {
    return at >= view.base && count && uint64_t(at) + count <= uint64_t(view.base) + bytes.size();
  };
  view.is_data = [](uint32_t, uint32_t) { return false; };
  view.export_address = [](uint16_t) { return 0u; };
  const std::string header = "schema=1\nidentity='managed fixture'\nmodule_hash='1'\n"
      "image_code_sha1='" + view.code_sha1 + "'\nimage_size=" + std::to_string(bytes.size()) + "\nlifetime='kernel'\n";
  const std::string bridge = "[[bridge]]\nservice='managed_format_owned_request'\n"
      "rva=256\ncode_size=268\ncode_sha256='" + Sha256(format) + "'\n"
      "arguments=['uint32']\nresult='uint32'\n";
  const std::string init = "[[phase]]\nidentity='parser only'\n" +
      Call("not executed", "rva=0\ncode_size=16\ncode_sha256='" +
           Sha256(std::span<const uint8_t>(bytes).first(16)) + "'\n");
  std::string error;
  const auto parsed = ParseGuestRuntimePlan(header + bridge + init, view, &error);
  REQUIRE(parsed); REQUIRE(error.empty()); REQUIRE(parsed->bridges.size() == 1);
  CHECK(parsed->bridges[0].address == 0x81684578);
  CHECK(parsed->bridges[0].service == "managed_format_owned_request");
  Rejected(header + Replace(bridge, "['uint32']", "[]") + init, view);
  Rejected(header + Replace(bridge, "['uint32']", "['uint64']") + init, view);
  Rejected(header + Replace(bridge, "rva=256", "rva=252") + init, view);
  Rejected(header + Replace(bridge, "result='uint32'", "result='void'") + init, view);
  Rejected(header + Replace(bridge, "managed_format_owned_request", "format_raw_device") + init, view);
}
}  // namespace xe::kernel::test
