/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia contributors. All rights reserved.                    *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <array>
#include <memory>
#include <string>
#include <vector>

#include "third_party/catch/include/catch.hpp"
#include "xenia/cpu/guest_import_provider.h"

namespace xe::cpu::test {
namespace {

using Provider = GuestImportProvider;
using Kind = Provider::Kind;
using Disposition = Provider::Disposition;

Provider::Config ProviderConfig() {
  return {{0x80001000, 7},
          std::make_shared<int>(1),
          {{"xam.xex", {1, 2, 3}}},
          {{"xam.xex", 1, Kind::kFunction, 0x90001000},
           {"xam.xex", 2, Kind::kData, 0x90002001}}};
}

bool OwnedAddress(Kind kind, uint32_t address) {
  return kind == Kind::kFunction ? address == 0x90001000
                                 : address == 0x90002001;
}

TEST_CASE("Native import provider preserves family ownership and import kind",
          "[kernel][guest_import_provider]") {
  std::string error = "old error";
  auto config = ProviderConfig();
  const auto provider = Provider::Create(config, OwnedAddress, &error);
  REQUIRE(provider);
  CHECK(error.empty());
  CHECK(provider->owner().hmodule == config.owner.hmodule);
  CHECK(provider->owner().generation == config.owner.generation);
  auto function = provider->Resolve("XAM.XEX", 1, Kind::kFunction);
  CHECK(function.disposition == Disposition::kBound);
  CHECK(function.address == 0x90001000);
  auto data = provider->Resolve("xam.xex", 2, Kind::kData);
  CHECK(data.disposition == Disposition::kBound);
  CHECK(data.address == 0x90002001);
  CHECK(provider->Resolve("xam.xex", 2, Kind::kFunction).disposition ==
        Disposition::kRejected);
  CHECK(provider->Resolve("xam.xex", 3, Kind::kFunction).disposition ==
        Disposition::kRejected);
  CHECK(provider->Resolve("xam.xex", 3, Kind::kFunction).address == 0);
  CHECK(provider->Resolve("xam.xex", 4, Kind::kFunction).disposition ==
        Disposition::kDeclined);
  CHECK(provider->Resolve("xboxkrnl.exe", 1, Kind::kFunction).disposition ==
        Disposition::kDeclined);
  CHECK(provider->Resolve("other/xam.xex", 1, Kind::kFunction).disposition ==
        Disposition::kDeclined);

  config.families[0].ordinals.clear();
  config.bindings[0].address = 0xDEADBEEF;
  CHECK(provider->Resolve("xam.xex", 1, Kind::kFunction).address == 0x90001000);
  CHECK(provider->Resolve("xam.xex", 3, Kind::kFunction).disposition ==
        Disposition::kRejected);
}

TEST_CASE("Import preflight rejects a late native failure before any binding",
          "[kernel][guest_import_provider]") {
  const auto provider = Provider::Create(ProviderConfig(), OwnedAddress);
  REQUIRE(provider);
  std::vector<Provider::Request> requests{
      {"xam.xex", 1, Kind::kFunction},
      {"XAM.XEX", 1, Kind::kFunction},  // Slot and thunk may repeat.
      {"xam.xex", 2, Kind::kData},
      {"xboxkrnl.exe", 1, Kind::kFunction}};
  std::string error = "old error";
  CHECK(provider->Preflight(requests, &error));
  CHECK(error.empty());
  requests.push_back({"xam.xex", 3, Kind::kFunction});
  std::array<uint32_t, 5> consumer_slots{};
  if (provider->Preflight(requests, &error)) {
    for (size_t i = 0; i < requests.size(); ++i) {
      consumer_slots[i] = provider
                              ->Resolve(requests[i].library,
                                        requests[i].ordinal, requests[i].kind)
                              .address;
    }
  }
  CHECK_FALSE(error.empty());
  CHECK((consumer_slots == std::array<uint32_t, 5>{}));
  requests.back() = {"xam.xex", 2, Kind::kFunction};
  CHECK_FALSE(provider->Preflight(requests, &error));
  requests.back() = {"xam.xex", 4, Kind::kFunction};
  CHECK(provider->Preflight(requests, &error));
  CHECK(error.empty());
}

TEST_CASE("Guest import factory validates every supplied native target",
          "[kernel][guest_import_provider]") {
  auto config = ProviderConfig();
  std::vector<std::pair<Kind, uint32_t>> validated;
  auto validate = [&](Kind kind, uint32_t address) {
    validated.emplace_back(kind, address);
    return OwnedAddress(kind, address);
  };
  SECTION("An invalid later data target prevents publication") {
    config.bindings[1].address = config.bindings[0].address;
  }
  SECTION("An unaligned function is not accepted by a permissive validator") {
    config.bindings[0].address += 1;
    std::string error;
    CHECK_FALSE(
        Provider::Create(config, [](Kind, uint32_t) { return true; }, &error));
    CHECK_FALSE(error.empty());
    return;
  }
  SECTION("A null target cannot act as a binding") {
    config.bindings[1].address = 0;
  }
  SECTION("Unknown target kinds cannot be admitted") {
    config.bindings[1].kind = static_cast<Kind>(99);
  }
  SECTION("Bindings outside the claimed family cannot be published") {
    config.bindings[1].ordinal = 4;
  }
  std::string error;
  CHECK_FALSE(Provider::Create(config, validate, &error));
  CHECK_FALSE(error.empty());
  REQUIRE_FALSE(validated.empty());
  CHECK(validated.front().first == Kind::kFunction);
  CHECK(validated.front().second == 0x90001000);
}

TEST_CASE("Guest import provider rejects ambiguous or unowned configurations",
          "[kernel][guest_import_provider]") {
  auto config = ProviderConfig();
  SECTION("Missing module owner") { config.owner.hmodule = 0; }
  SECTION("Missing generation") { config.owner.generation = 0; }
  SECTION("Missing lifetime pin") { config.owner_pin.reset(); }
  SECTION("No family") { config.families.clear(); }
  SECTION("Empty library") { config.families[0].library.clear(); }
  SECTION("Empty family") { config.families[0].ordinals.clear(); }
  SECTION("Case-insensitive duplicate claim") {
    config.families.push_back({"XAM.XEX", {1}});
  }
  SECTION("Duplicate target") { config.bindings.push_back(config.bindings[0]); }
  std::string error;
  CHECK_FALSE(Provider::Create(config, OwnedAddress, &error));
  CHECK_FALSE(error.empty());
}

TEST_CASE("Native import owner remains pinned by every consumer reference",
          "[kernel][guest_import_provider]") {
  bool owner_destroyed = false;
  auto config = ProviderConfig();
  config.owner_pin = std::shared_ptr<int>(new int(1), [&](int* value) {
    delete value;
    owner_destroyed = true;
  });
  auto provider = Provider::Create(std::move(config), OwnedAddress);
  REQUIRE(provider);
  auto consumer = provider;
  provider.reset();
  CHECK_FALSE(owner_destroyed);
  CHECK(consumer->Resolve("xam.xex", 1, Kind::kFunction).disposition ==
        Disposition::kBound);
  consumer.reset();
  CHECK(owner_destroyed);
}

TEST_CASE("Provider creation does not retain the address-validation callback",
          "[kernel][guest_import_provider]") {
  auto validation_lifetime = std::make_shared<int>(1);
  std::weak_ptr<int> weak_validation = validation_lifetime;
  Provider::AddressValidator validator = [validation_lifetime](
                                             Kind kind, uint32_t address) {
    return OwnedAddress(kind, address);
  };
  auto provider = Provider::Create(ProviderConfig(), validator);
  REQUIRE(provider);
  validation_lifetime.reset();
  validator = {};
  CHECK(weak_validation.expired());
  CHECK(provider->Resolve("xam.xex", 2, Kind::kData).disposition ==
        Disposition::kBound);
  std::string error;
  CHECK_FALSE(Provider::Create(ProviderConfig(), {}, &error));
  CHECK_FALSE(error.empty());
}

}  // namespace
}  // namespace xe::cpu::test
