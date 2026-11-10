/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia contributors. All rights reserved.                    *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */
#include <atomic>
#include <memory>
#include <thread>

#include "third_party/catch/include/catch.hpp"
#include "xenia/kernel/native_import_domain.h"

namespace xe::kernel::test {
namespace {
using Domain = NativeImportDomain;
using Provider = cpu::GuestImportProvider;
using Kind = Provider::Kind;
using Disposition = Provider::Disposition;

std::shared_ptr<const Provider> Profile(std::shared_ptr<void> pin = {}) {
  Provider::Config config;
  config.owner = {0x80001000, 71};
  config.owner_pin = pin ? std::move(pin) : std::make_shared<int>(1);
  config.families = {{"xam.xex", {1, 2, 3}}};
  config.bindings = {{"xam.xex", 1, Kind::kFunction, 0x90001000},
                     {"xam.xex", 2, Kind::kData, 0x90002000}};
  config.name_aliases = {{"xam.xex", "NativeCall", 1},
                         {"XAM.XEX", "NativeData", 2},
                         {"xam.xex", "Unavailable", 3}};
  return Provider::Create(std::move(config), [](auto, auto) { return true; });
}

TEST_CASE("Dynamic names and ordinals preserve the static native claim",
          "[kernel][native_import_domain]") {
  const auto profile = Profile();
  REQUIRE(profile);
  const auto function = profile->Resolve("XAM.XEX", 1, Kind::kFunction);
  CHECK(profile->Resolve("xam.xex", 1).address == function.address);
  CHECK(profile->ResolveName("XAM.XEX", "NativeCall").address ==
        function.address);
  CHECK(profile->ResolveName("xam.xex", "NativeData", Kind::kData).address ==
        profile->Resolve("xam.xex", 2, Kind::kData).address);
  CHECK(profile->ResolveName("xam.xex", "NativeData", Kind::kFunction)
            .disposition == Disposition::kRejected);
  CHECK(profile->ResolveName("xam.xex", "Unavailable").disposition ==
        Disposition::kRejected);
  CHECK(profile->Resolve("xam.xex", 3).disposition == Disposition::kRejected);
  CHECK(profile->ResolveName("xam.xex", "nativecall").disposition ==
        Disposition::kDeclined);
  CHECK(profile->ResolveName("other/xam.xex", "NativeCall").disposition ==
        Disposition::kDeclined);
  CHECK(profile->Resolve("xboxkrnl.exe", 1).disposition ==
        Disposition::kDeclined);
  CHECK(profile->claimed_libraries() == std::vector<std::string>{"xam.xex"});
}

TEST_CASE("Native aliases cannot introduce an unclaimed allocation family",
          "[kernel][native_import_domain]") {
  Provider::Config config;
  config.owner = {0x80001000, 1};
  config.owner_pin = std::make_shared<int>(1);
  config.families = {{"xam.xex", {1}}};
  config.bindings = {{"xam.xex", 1, Kind::kFunction, 0x90001000}};
  config.name_aliases = {{"xam.xex", "Call", 1}};
  SECTION("Unclaimed ordinal") { config.name_aliases[0].ordinal = 2; }
  SECTION("Unclaimed library") { config.name_aliases[0].library = "other.xex"; }
  SECTION("Duplicate alias") {
    config.name_aliases.push_back(config.name_aliases[0]);
  }
  SECTION("Empty name") { config.name_aliases[0].name.clear(); }
  CHECK_FALSE(
      Provider::Create(std::move(config), [](auto, auto) { return true; }));
}

TEST_CASE("Native domain membership cannot reuse a stale module generation",
          "[kernel][native_import_domain]") {
  const Domain::Member root{0x80001000, 4}, first{0x80002000, 5},
      replacement{0x80002000, 6};
  auto domain = Domain::Create(1, root, Profile());
  REQUIRE(domain);
  CHECK(domain->Contains(root));
  REQUIRE(domain->Register(first));
  CHECK(domain->Register(first));
  CHECK_FALSE(domain->Register(replacement));
  CHECK_FALSE(domain->Contains(replacement));
  domain->Unregister(replacement);
  CHECK(domain->Contains(first));
  domain->Unregister(first);
  REQUIRE(domain->Register(replacement));
  CHECK_FALSE(domain->Contains(first));
  domain->Unregister(first);
  CHECK(domain->Contains(replacement));
  domain->Unregister(root);
  CHECK(domain->Contains(root));
}

TEST_CASE("Retiring a native domain closes admission while operations drain",
          "[kernel][native_import_domain]") {
  auto domain = Domain::Create(2, {0x80001000, 9}, Profile());
  REQUIRE(domain);
  auto operation = domain->Acquire();
  REQUIRE(operation);
  CHECK_FALSE(domain->Retire());
  CHECK_FALSE(domain->active());
  CHECK_FALSE(domain->Acquire());
  CHECK_FALSE(domain->Register({0x80002000, 1}));
  // A tombstone retains member identity so an old scoped caller is denied,
  // rather than reclassified as an ordinary HLE caller.
  CHECK(domain->Contains(domain->root()));
  auto moved = std::move(operation);
  CHECK_FALSE(operation);
  CHECK_FALSE(domain->Retire());
  moved = {};
  CHECK(domain->Retire());
  CHECK_FALSE(domain->Acquire());
}

TEST_CASE("Concurrent domain retirement cannot admit a later loader operation",
          "[kernel][native_import_domain]") {
  auto domain = Domain::Create(3, {0x80001000, 9}, Profile());
  REQUIRE(domain);
  std::atomic<bool> admitted{false}, retired{false}, later_admitted{true};
  std::thread loader([&] {
    auto operation = domain->Acquire();
    admitted.store(bool(operation), std::memory_order_release);
    while (!retired.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }
    later_admitted.store(bool(domain->Acquire()), std::memory_order_release);
  });
  while (!admitted.load(std::memory_order_acquire)) {
    std::this_thread::yield();
  }
  CHECK_FALSE(domain->Retire());
  retired.store(true, std::memory_order_release);
  loader.join();
  CHECK_FALSE(later_admitted.load(std::memory_order_acquire));
  CHECK(domain->Retire());
}

TEST_CASE("Weak module membership needs no native root self-pin cycle",
          "[kernel][native_import_domain]") {
  auto owner = std::make_shared<int>(42);
  std::weak_ptr<int> root_lifetime = owner;
  auto profile = Profile(owner);
  auto domain = Domain::Create(4, {0x80001000, 2}, profile);
  std::weak_ptr<Domain> module_membership = domain;
  owner.reset();
  profile.reset();
  CHECK_FALSE(root_lifetime.expired());
  CHECK(domain->Retire());
  domain.reset();
  CHECK(module_membership.expired());
  CHECK(root_lifetime.expired());
}

TEST_CASE("Invalid native domain identity never becomes loader admission",
          "[kernel][native_import_domain]") {
  CHECK_FALSE(Domain::Create(0, {0x80001000, 1}, Profile()));
  CHECK_FALSE(Domain::Create(1, {0x80001000, 0}, Profile()));
  CHECK_FALSE(Domain::Create(1, {0x80002000, 1}, Profile()));
  CHECK_FALSE(Domain::Create(1, {0x80001000, 1}, {}));
}
}  // namespace
}  // namespace xe::kernel::test
