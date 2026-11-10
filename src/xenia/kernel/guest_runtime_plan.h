/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia contributors. All rights reserved.                    *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_GUEST_RUNTIME_PLAN_H_
#define XENIA_KERNEL_GUEST_RUNTIME_PLAN_H_

#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "xenia/kernel/guest_runtime.h"

namespace xe::kernel {

// Explicit diagnostic input, never a guessed constructor list. Image identity,
// code/data ownership and complete function signatures are checked before any
// entry is executed or any bridge is installed.
struct GuestRuntimeImageView {
  uint32_t base = 0;
  // Real loader identity, for explicit "module_handle" operands/checks. This
  // is never inferred from the image base or the HLE module with the same name.
  uint32_t hmodule = 0;
  uint64_t module_hash = 0;
  std::string code_sha1;
  std::span<const uint8_t> bytes;
  std::function<bool(uint32_t address, uint32_t size)> is_code;
  std::function<bool(uint32_t address, uint32_t size)> is_data;
  std::function<uint32_t(uint16_t ordinal)> export_address;
};

struct GuestRuntimeWordCheck {
  uint32_t address = 0;
  uint32_t expected = 0;
  bool nonzero = false;
};

struct ValidatedGuestRuntimePlan {
  enum class ResultPolicy { kVoid, kAny, kZero, kNonzero, kHResult };
  enum class CallerPolicy {
    kSystemOnly,
    kSystemTaskOnly,
    kOriginalTitleOrSystem
  };
  struct Contract {
    ResultPolicy result = ResultPolicy::kVoid;
    std::vector<GuestRuntimeWordCheck> before;
    std::vector<GuestRuntimeWordCheck> after;
    CallerPolicy caller = CallerPolicy::kSystemOnly;
  };
  struct Bridge {
    std::string library;
    uint16_t ordinal = 0;
    // An explicitly verified internal entry, with an allowlisted host service.
    // Neither the address nor the host callback is inferred from guest data.
    uint32_t address = 0;
    uint32_t code_size = 0;
    std::string code_sha256;
    std::string service;
  };
  GuestRuntime::Plan plan;
  // Deferred, validated entry catalog for later owned native API operations.
  // These entries do not run during runtime initialization or imply readiness.
  std::map<std::string, GuestRuntime::Function> catalog;
  std::vector<Bridge> bridges;
  std::map<std::string, Contract> contracts;
};

std::optional<ValidatedGuestRuntimePlan> ParseGuestRuntimePlan(
    std::string_view text, const GuestRuntimeImageView& image,
    std::string* error);

}  // namespace xe::kernel

#endif  // XENIA_KERNEL_GUEST_RUNTIME_PLAN_H_
