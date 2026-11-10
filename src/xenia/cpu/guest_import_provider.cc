/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia contributors. All rights reserved.                    *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/cpu/guest_import_provider.h"

namespace xe::cpu {
namespace {

std::string LibraryKey(std::string_view library) {
  std::string result(library);
  for (char& c : result) {
    // Library names are ASCII; avoid locale-dependent matching or silently
    // converting paths/extension aliases into a different claimed library.
    if (c >= 'A' && c <= 'Z') {
      c = char(c + ('a' - 'A'));
    }
  }
  return result;
}

bool ValidKind(GuestImportProvider::Kind kind) {
  return kind == GuestImportProvider::Kind::kFunction ||
         kind == GuestImportProvider::Kind::kData;
}

}  // namespace

GuestImportProvider::GuestImportProvider(
    Identity owner, std::shared_ptr<void> owner_pin, std::set<Key> claims,
    std::map<Key, Target> bindings,
    std::map<std::pair<std::string, std::string>, uint16_t> name_aliases)
    : owner_(owner),
      owner_pin_(std::move(owner_pin)),
      claims_(std::move(claims)),
      bindings_(std::move(bindings)),
      name_aliases_(std::move(name_aliases)) {}

std::shared_ptr<const GuestImportProvider> GuestImportProvider::Create(
    Config config, const AddressValidator& validate_address,
    std::string* error) {
  auto fail =
      [&](std::string reason) -> std::shared_ptr<const GuestImportProvider> {
    if (error) {
      *error = std::move(reason);
    }
    return {};
  };
  if (!config.owner.hmodule || !config.owner.generation || !config.owner_pin ||
      !validate_address || config.families.empty()) {
    return fail("Invalid guest import provider owner or family");
  }
  std::set<Key> claims;
  for (const auto& family : config.families) {
    if (family.library.empty() || family.ordinals.empty()) {
      return fail("Empty guest import family");
    }
    const auto library = LibraryKey(family.library);
    for (uint16_t ordinal : family.ordinals) {
      if (!claims.emplace(library, ordinal).second) {
        return fail("Duplicate guest import family claim");
      }
    }
  }
  std::map<Key, Target> bindings;
  for (const auto& binding : config.bindings) {
    const Key key{LibraryKey(binding.library), binding.ordinal};
    if (!claims.contains(key) || !ValidKind(binding.kind) || !binding.address ||
        (binding.kind == Kind::kFunction && (binding.address & 3)) ||
        !validate_address(binding.kind, binding.address)) {
      return fail("Unclaimed or invalid native guest import target");
    }
    if (!bindings.emplace(key, Target{binding.kind, binding.address}).second) {
      return fail("Duplicate native guest import binding");
    }
  }
  std::map<std::pair<std::string, std::string>, uint16_t> aliases;
  for (const auto& alias : config.name_aliases) {
    const auto library = LibraryKey(alias.library);
    if (alias.name.empty() || !claims.contains({library, alias.ordinal}) ||
        !aliases.emplace(std::make_pair(library, alias.name), alias.ordinal)
             .second) {
      return fail("Duplicate or unclaimed native export name alias");
    }
  }
  if (error) {
    error->clear();
  }
  return std::shared_ptr<const GuestImportProvider>(new GuestImportProvider(
      config.owner, std::move(config.owner_pin), std::move(claims),
      std::move(bindings), std::move(aliases)));
}

GuestImportProvider::Resolution GuestImportProvider::Resolve(
    std::string_view library, uint16_t ordinal, Kind kind) const {
  const Key key{LibraryKey(library), ordinal};
  if (!claims_.contains(key)) {
    return {};
  }
  const auto binding = bindings_.find(key);
  if (!ValidKind(kind) || binding == bindings_.end() ||
      binding->second.kind != kind) {
    return {Disposition::kRejected, 0};
  }
  return {Disposition::kBound, binding->second.address};
}

GuestImportProvider::Resolution GuestImportProvider::Resolve(
    std::string_view library, uint16_t ordinal) const {
  const Key key{LibraryKey(library), ordinal};
  if (!claims_.contains(key)) {
    return {};
  }
  const auto binding = bindings_.find(key);
  return binding == bindings_.end()
             ? Resolution{Disposition::kRejected, 0}
             : Resolution{Disposition::kBound, binding->second.address};
}

GuestImportProvider::Resolution GuestImportProvider::ResolveName(
    std::string_view library, std::string_view name, Kind kind) const {
  const auto alias =
      name_aliases_.find({LibraryKey(library), std::string(name)});
  return alias == name_aliases_.end() ? Resolution{}
                                      : Resolve(library, alias->second, kind);
}

GuestImportProvider::Resolution GuestImportProvider::ResolveName(
    std::string_view library, std::string_view name) const {
  const auto alias =
      name_aliases_.find({LibraryKey(library), std::string(name)});
  return alias == name_aliases_.end() ? Resolution{}
                                      : Resolve(library, alias->second);
}

std::vector<std::string> GuestImportProvider::claimed_libraries() const {
  std::vector<std::string> result;
  for (const auto& claim : claims_) {
    if (result.empty() || result.back() != claim.first) {
      result.push_back(claim.first);
    }
  }
  return result;
}

bool GuestImportProvider::Preflight(std::span<const Request> requests,
                                    std::string* error) const {
  for (const auto& request : requests) {
    if (Resolve(request.library, request.ordinal, request.kind).disposition ==
        Disposition::kRejected) {
      if (error) {
        *error = "Claimed guest import is unavailable or has the wrong kind: " +
                 request.library + ":" + std::to_string(request.ordinal);
      }
      return false;
    }
  }
  if (error) {
    error->clear();
  }
  return true;
}

}  // namespace xe::cpu
