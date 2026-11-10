/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia contributors. All rights reserved.                    *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_CPU_GUEST_IMPORT_PROVIDER_H_
#define XENIA_CPU_GUEST_IMPORT_PROVIDER_H_

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace xe::cpu {

// An explicit consumer-local import policy. Kernel/module ownership and native
// runtime readiness are checked by its creator, never inferred from an ordinal.
// A claimed export cannot silently fall back to another object's
// implementation.
class GuestImportProvider {
 public:
  enum class Kind { kFunction, kData };
  enum class Disposition { kDeclined, kBound, kRejected };
  struct Identity {
    uint32_t hmodule = 0;
    uint64_t generation = 0;
  };
  struct Request {
    std::string library;
    uint16_t ordinal = 0;
    Kind kind = Kind::kFunction;
  };
  struct Family {
    std::string library;
    std::vector<uint16_t> ordinals;
  };
  struct Binding {
    std::string library;
    uint16_t ordinal = 0;
    Kind kind = Kind::kFunction;
    uint32_t address = 0;
  };
  struct NameAlias {
    std::string library;
    std::string name;
    uint16_t ordinal = 0;
  };
  struct Resolution {
    Disposition disposition = Disposition::kDeclined;
    uint32_t address = 0;
  };
  struct Config {
    Identity owner;
    std::shared_ptr<void> owner_pin;
    std::vector<Family> families;
    std::vector<Binding> bindings;
    std::vector<NameAlias> name_aliases;
  };
  // The creator verifies actual loaded-image code/data ownership here. This
  // callback is used only during creation and is not retained by the provider.
  using AddressValidator = std::function<bool(Kind, uint32_t address)>;

  static std::shared_ptr<const GuestImportProvider> Create(
      Config config, const AddressValidator& validate_address,
      std::string* error = nullptr);

  Identity owner() const { return owner_; }
  Resolution Resolve(std::string_view library, uint16_t ordinal,
                     Kind kind) const;
  // Dynamic lookup has no caller-supplied function/data kind. Use the exact
  // same claim and stored binding as static imports, including claimed holes.
  Resolution Resolve(std::string_view library, uint16_t ordinal) const;
  Resolution ResolveName(std::string_view library, std::string_view name,
                         Kind kind) const;
  Resolution ResolveName(std::string_view library, std::string_view name) const;
  std::vector<std::string> claimed_libraries() const;
  // Call for the complete consumer import list before changing any slot/thunk.
  // Duplicate requests (an import slot and its thunk) preserve the same result.
  bool Preflight(std::span<const Request> requests,
                 std::string* error = nullptr) const;

 private:
  using Key = std::pair<std::string, uint16_t>;
  struct Target {
    Kind kind;
    uint32_t address;
  };
  GuestImportProvider(
      Identity owner, std::shared_ptr<void> owner_pin, std::set<Key> claims,
      std::map<Key, Target> bindings,
      std::map<std::pair<std::string, std::string>, uint16_t> name_aliases);

  const Identity owner_;
  const std::shared_ptr<void> owner_pin_;
  const std::set<Key> claims_;
  const std::map<Key, Target> bindings_;
  const std::map<std::pair<std::string, std::string>, uint16_t> name_aliases_;
};

}  // namespace xe::cpu

#endif  // XENIA_CPU_GUEST_IMPORT_PROVIDER_H_
