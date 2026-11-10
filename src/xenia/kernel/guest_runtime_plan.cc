/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia contributors. All rights reserved.                    *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/guest_runtime_plan.h"

#include <array>
#include <charconv>
#include <limits>
#include <sstream>

#include "third_party/mbedtls/include/mbedtls/sha256.h"
#include "third_party/tomlplusplus/toml.hpp"
#include "xenia/cpu/guest_call_abi.h"

namespace xe::kernel {
namespace {
bool Hex(std::string_view text, uint64_t* value) {
  if (text.empty() || text.size() > 16) {
    return false;
  }
  auto parsed =
      std::from_chars(text.data(), text.data() + text.size(), *value, 16);
  return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size();
}

template <typename Node>
std::optional<uint32_t> Dword(const toml::node_view<Node>& node) {
  if (!node.is_integer()) {
    return {};
  }
  auto value = node.template value<int64_t>();
  if (!value || *value < 0 || uint64_t(*value) > UINT32_MAX) {
    return {};
  }
  return uint32_t(*value);
}

bool InImage(const GuestRuntimeImageView& image, uint32_t address,
             uint32_t size) {
  return address >= image.base &&
         uint64_t(address) + size <= uint64_t(image.base) + image.bytes.size();
}

bool CodeHashMatches(const GuestRuntimeImageView& image, uint32_t address,
                     uint32_t size, std::string_view hex) {
  if (!size || size > 1024 * 1024 || hex.size() != 64 ||
      !InImage(image, address, size) || !image.is_code(address, size)) {
    return false;
  }
  std::array<uint8_t, 32> digest;
  mbedtls_sha256(image.bytes.data() + (address - image.base), size,
                 digest.data(), 0);
  for (size_t i = 0; i < digest.size(); ++i) {
    uint64_t expected;
    if (!Hex(hex.substr(i * 2, 2), &expected) || digest[i] != expected) {
      return false;
    }
  }
  return true;
}

bool ReadChecks(const toml::table& call, const char* key,
                const GuestRuntimeImageView& image,
                std::vector<GuestRuntimeWordCheck>& output) {
  auto* checks = call[key].as_array();
  if (!checks) {
    return !call.contains(key);
  }
  if (checks->size() > 256) {
    return false;
  }
  for (const auto& node : *checks) {
    auto* check = node.as_table();
    if (!check) {
      return false;
    }
    auto rva = Dword((*check)["rva"]);
    auto expected = Dword((*check)["expected"]);
    if ((*check)["expected"].value<std::string>() == "module_handle" &&
        image.hmodule) {
      expected = image.hmodule;
    }
    auto nonzero = (*check)["nonzero"].value<bool>();
    if ((check->contains("expected") && !expected) ||
        (check->contains("nonzero") &&
         (!(*check)["nonzero"].is_boolean() || !nonzero)) ||
        !rva || (*rva & 3) || uint64_t(image.base) + *rva > UINT32_MAX ||
        (expected.has_value() == nonzero.value_or(false))) {
      return false;
    }
    const uint32_t address = image.base + *rva;
    if (!InImage(image, address, 4) || !image.is_data(address, 4)) {
      return false;
    }
    output.push_back({address, expected.value_or(0), nonzero.value_or(false)});
  }
  return true;
}
}  // namespace

std::optional<ValidatedGuestRuntimePlan> ParseGuestRuntimePlan(
    std::string_view text, const GuestRuntimeImageView& image,
    std::string* error) {
  auto fail =
      [&](std::string reason) -> std::optional<ValidatedGuestRuntimePlan> {
    if (error) {
      *error = std::move(reason);
    }
    return {};
  };
  if (text.size() > 1024 * 1024 || !image.base || image.bytes.empty() ||
      uint64_t(image.base) + image.bytes.size() > (uint64_t(1) << 32) ||
      !image.is_code || !image.is_data || !image.export_address) {
    return fail("Invalid plan size or loaded image view");
  }
  toml::table root;
  try {
    root = toml::parse(text);
  } catch (const toml::parse_error&) {
    return fail("Malformed runtime plan TOML");
  }
  uint64_t hash = 0;
  const auto hash_string = root["module_hash"].value<std::string>();
  const auto code_hash = root["image_code_sha1"].value<std::string>();
  auto image_size = Dword(root["image_size"]);
  const auto identity = root["identity"].value<std::string>();
  if (!root["schema"].is_integer() || root["schema"].value<int64_t>() != 1 ||
      !identity || identity->empty() || identity->size() > 256 ||
      !hash_string || !Hex(*hash_string, &hash) || !code_hash ||
      code_hash->size() != 40 || *code_hash != image.code_sha1 ||
      hash != image.module_hash || !image_size ||
      *image_size != image.bytes.size() ||
      root["lifetime"].value<std::string>() != "kernel") {
    return fail(
        "Runtime plan image identity/schema/resident lifetime mismatch");
  }
  for (size_t i = 0; i < code_hash->size(); i += 2) {
    uint64_t byte;
    if (!Hex(std::string_view(*code_hash).substr(i, 2), &byte)) {
      return fail("Invalid code image SHA-1");
    }
  }
  ValidatedGuestRuntimePlan result;
  result.plan.identity = *identity;
  std::array<uint8_t, 32> plan_digest;
  mbedtls_sha256(reinterpret_cast<const uint8_t*>(text.data()), text.size(),
                 plan_digest.data(), 0);
  std::string plan_hash;
  constexpr char digits[] = "0123456789abcdef";
  for (const uint8_t byte : plan_digest) {
    plan_hash.push_back(digits[byte >> 4]);
    plan_hash.push_back(digits[byte & 15]);
  }
  if (auto* bridges = root["bridge"].as_array()) {
    if (bridges->size() > 256) {
      return fail("Too many guest export bridges");
    }
    for (const auto& node : *bridges) {
      auto* bridge = node.as_table();
      if (!bridge) {
        return fail("Invalid guest export bridge");
      }
      if (bridge->contains("rva") || bridge->contains("service")) {
        const auto rva = Dword((*bridge)["rva"]);
        const auto size = Dword((*bridge)["code_size"]);
        const auto sha = (*bridge)["code_sha256"].value<std::string>();
        const auto service = (*bridge)["service"].value<std::string>();
        const auto* arguments = (*bridge)["arguments"].as_array();
        if (bridge->contains("ordinal") || bridge->contains("library") ||
            !rva || (*rva & 3) || !size || *size < 8 || (*size & 3) ||
            uint64_t(image.base) + *rva > UINT32_MAX || !sha || !service ||
            !arguments ||
            !((*service == "active_title_id" && arguments->empty()) ||
              (*service == "managed_format_owned_request" &&
               image.base + *rva == 0x81684578 && *size == 268 &&
               *sha == "72b85802c32d4c9ddfb4d535ddde60aee77fc4b13b9b0419dbd1c20dbafd22d8" &&
               arguments->size() == 1 &&
               (*arguments)[0].value<std::string>() == "uint32")) ||
            (*bridge)["result"].value<std::string>() != "uint32") {
          return fail("Invalid internal bridge selector or host service ABI");
        }
        const uint32_t address = image.base + *rva;
        if (!CodeHashMatches(image, address, *size, *sha)) {
          return fail("Internal bridge code identity mismatch");
        }
        for (const auto& previous : result.bridges) {
          if (previous.address &&
              uint64_t(address) <
                  uint64_t(previous.address) + previous.code_size &&
              uint64_t(previous.address) < uint64_t(address) + *size) {
            return fail("Overlapping internal bridge entries");
          }
        }
        ValidatedGuestRuntimePlan::Bridge entry;
        entry.address = address;
        entry.code_size = *size;
        entry.code_sha256 = *sha;
        entry.service = *service;
        result.bridges.push_back(std::move(entry));
        continue;
      }
      auto ordinal = Dword((*bridge)["ordinal"]);
      auto library = (*bridge)["library"].value<std::string>();
      if (!ordinal || !*ordinal || *ordinal > UINT16_MAX || !library ||
          library->empty() || library->size() > 256) {
        return fail("Invalid guest export bridge identity");
      }
      result.bridges.push_back({*library, uint16_t(*ordinal)});
    }
  } else if (root.contains("bridge")) {
    return fail("Invalid bridge array");
  }

  const auto* phases = root["phase"].as_array();
  if (!phases || phases->empty() || phases->size() > 32) {
    return fail("Invalid ordered phases");
  }
  size_t total_calls = 0;
  for (const auto& phase_node : *phases) {
    const auto* phase = phase_node.as_table();
    if (!phase) {
      return fail("Invalid phase table");
    }
    auto phase_id = (*phase)["identity"].value<std::string>();
    const auto deferred = (*phase)["deferred"].value<bool>();
    const auto* calls = (*phase)["call"].as_array();
    if ((phase->contains("deferred") &&
         (!(*phase)["deferred"].is_boolean() || !deferred)) ||
        !phase_id || phase_id->empty() || phase_id->size() > 256 || !calls ||
        calls->empty()) {
      return fail("Invalid phase identity/calls");
    }
    GuestRuntime::Phase owned_phase{*phase_id, {}};
    for (const auto& call_node : *calls) {
      if (++total_calls > 512) {
        return fail("Too many initialization calls");
      }
      const auto* call = call_node.as_table();
      if (!call) {
        return fail("Invalid call table");
      }
      auto call_id = (*call)["identity"].value<std::string>();
      auto rva = Dword((*call)["rva"]);
      auto ordinal = Dword((*call)["ordinal"]);
      if (!call_id || call_id->empty() || call_id->size() > 256 ||
          result.contracts.contains(*call_id) ||
          (call->contains("rva") && !rva) ||
          (call->contains("ordinal") && !ordinal) ||
          call->contains("rva") == call->contains("ordinal")) {
        return fail("Invalid or duplicate call identity/selector");
      }
      uint32_t address = 0;
      if (rva) {
        if (uint64_t(image.base) + *rva > UINT32_MAX) {
          return fail("Call address overflow");
        }
        address = image.base + *rva;
        auto size = Dword((*call)["code_size"]);
        auto sha = (*call)["code_sha256"].value<std::string>();
        if (!size || !sha || !CodeHashMatches(image, address, *size, *sha)) {
          return fail("Native function code identity mismatch");
        }
      } else {
        if (!*ordinal || *ordinal > UINT16_MAX) {
          return fail("Invalid native export ordinal");
        }
        address = image.export_address(uint16_t(*ordinal));
      }
      if ((address & 3) || !InImage(image, address, 4) ||
          !image.is_code(address, 4)) {
        return fail("Native call is not in loaded image code");
      }
      GuestRuntime::Function function;
      function.identity = *call_id;
      function.address = address;
      if (auto* args = (*call)["arguments"].as_array()) {
        const size_t max_arguments =
            deferred.value_or(false) ? cpu::kGuestCallMaxArguments : 8;
        if (args->size() > max_arguments) {
          return fail("Call argument count exceeds its execution profile");
        }
        for (const auto& arg : *args) {
          if (arg.value<std::string>() == "module_handle" && image.hmodule) {
            function.arguments.push_back(image.hmodule);
            continue;
          }
          auto value = arg.value<int64_t>();
          if (!arg.is_integer() || !value || *value < 0 ||
              uint64_t(*value) > UINT32_MAX) {
            return fail("Invalid DWORD argument");
          }
          function.arguments.push_back(uint64_t(*value));
        }
      } else {
        return fail("Explicit argument list required");
      }
      ValidatedGuestRuntimePlan::Contract contract;
      if (call->contains("caller")) {
        const auto caller = (*call)["caller"].value<std::string>();
        if (!caller ||
            (*caller != "system" && *caller != "system_task" &&
             *caller != "original") ||
            (*caller != "system" && !deferred.value_or(false))) {
          return fail(
              "Non-bootstrap caller policy requires an explicit deferred API");
        }
        if (*caller == "original") {
          contract.caller =
              ValidatedGuestRuntimePlan::CallerPolicy::kOriginalTitleOrSystem;
        }
        if (*caller == "system_task") {
          contract.caller =
              ValidatedGuestRuntimePlan::CallerPolicy::kSystemTaskOnly;
        }
      }
      auto policy = (*call)["result"].value<std::string>();
      if (!policy) {
        return fail("Explicit return contract required");
      }
      using Policy = ValidatedGuestRuntimePlan::ResultPolicy;
      function.return_kind = GuestRuntime::ReturnKind::kUInt32;
      if (*policy == "void") {
        contract.result = Policy::kVoid;
        function.return_kind = GuestRuntime::ReturnKind::kVoid;
      } else if (*policy == "any") {
        contract.result = Policy::kAny;
      } else if (*policy == "zero") {
        contract.result = Policy::kZero;
      } else if (*policy == "nonzero") {
        contract.result = Policy::kNonzero;
      } else if (*policy == "hresult") {
        contract.result = Policy::kHResult;
        function.return_kind = GuestRuntime::ReturnKind::kHResult;
      } else {
        return fail("Unsupported return contract");
      }
      if (!ReadChecks(*call, "before", image, contract.before) ||
          !ReadChecks(*call, "after", image, contract.after)) {
        return fail("Invalid owned data pre/postcondition");
      }
      // Include the exact caller-provided contract in plan equality/identity;
      // changing checks or ABI cannot reuse an already initialized generation.
      std::ostringstream metadata;
      metadata << *call;
      metadata << "\nverified_plan_sha256=" << plan_hash;
      function.abi_metadata = metadata.str();
      result.contracts.emplace(*call_id, std::move(contract));
      result.catalog.emplace(*call_id, function);
      owned_phase.functions.push_back(std::move(function));
    }
    if (!deferred.value_or(false)) {
      result.plan.phases.push_back(std::move(owned_phase));
    }
  }
  if (result.plan.phases.empty()) {
    return fail("No runtime initialization phases");
  }
  if (error) {
    error->clear();
  }
  return result;
}
}  // namespace xe::kernel
