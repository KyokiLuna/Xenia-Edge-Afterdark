/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia contributors. All rights reserved.                    *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */
#ifndef XENIA_BASE_NATIVE_SYSTEM_UI_TRACE_H_
#define XENIA_BASE_NATIVE_SYSTEM_UI_TRACE_H_

#include <atomic>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <string>

#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"

DECLARE_bool(native_system_ui_trace);

namespace xe {
// Metadata only. Each call site owns its limiter, with no guest/Kernel/GPU
// lifetime. First eight observations, then powers of two: at most 68 sampled
// lines per call site/process. EmitTransition additionally admits at most 64
// changed states. Atomic counter snapshots are metadata, never a causal serial.
// Unknown identities are omitted by the caller, never synthesized as user0.
class NativeSystemUiTraceLimiter {
 public:
  struct Field {
    const char* key;
    uint64_t value;
  };
  void Emit(const char* component, const char* event,
            std::initializer_list<Field> fields,
            std::initializer_list<Field> extra = {}) noexcept {
    EmitImpl(false, 0, component, event, fields, extra);
  }
  void EmitTransition(uint64_t state_key, const char* component,
                      const char* event, std::initializer_list<Field> fields,
                      std::initializer_list<Field> extra = {}) noexcept {
    EmitImpl(true, state_key, component, event, fields, extra);
  }

 private:
  void EmitImpl(bool watch_state, uint64_t state_key, const char* component,
                const char* event, std::initializer_list<Field> fields,
                std::initializer_list<Field> extra) noexcept {
    if (!cvars::native_system_ui_trace) {
      return;
    }
    auto prior = observations_.load(std::memory_order_relaxed);
    while (prior != std::numeric_limits<uint64_t>::max() &&
           !observations_.compare_exchange_weak(prior, prior + 1,
                                                std::memory_order_relaxed)) {
    }
    if (prior == std::numeric_limits<uint64_t>::max()) {
      return;
    }
    const uint64_t observed = prior + 1;
    bool changed = false;
    if (watch_state) {
      changed = last_state_.exchange(state_key, std::memory_order_relaxed) !=
                state_key;
    }
    const bool transition =
        changed && transitions_.fetch_add(uint64_t(changed),
                                          std::memory_order_relaxed) < 64;
    if (!transition && observed > 8 && (observed & (observed - 1))) {
      suppressed_.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    // Diagnostics must never throw through a callback, change admission, or
    // unwind after partial GPU work. Keys/event/component are literal names.
    try {
      std::string json = fmt::format(
          "{{\"schema\":1,\"component\":\"{}\",\"event\":\"{}\","
          "\"observation_count\":{},\"sample_first\":8,"
          "\"sample_powers_of_two\":true,\"sampling_suppressed\":{},"
          "\"state_transition\":{},\"logging_failures\":{}",
          component, event, observed,
          suppressed_.load(std::memory_order_relaxed), transition,
          logging_failures_.load(std::memory_order_relaxed));
      for (const auto& list : {fields, extra}) {
        for (const auto& field : list) {
          json += fmt::format(",\"{}\":{}", field.key, field.value);
        }
      }
      json += '}';
      XELOGI("NativeSystemUITrace {}", json);
    } catch (...) {
      logging_failures_.fetch_add(1, std::memory_order_relaxed);
      // Missing diagnostic output never changes the observed operation.
    }
  }

  std::atomic<uint64_t> observations_{0};
  std::atomic<uint64_t> last_state_{0};
  std::atomic<uint64_t> transitions_{0};
  std::atomic<uint64_t> suppressed_{0};
  std::atomic<uint64_t> logging_failures_{0};
};
}  // namespace xe
#endif  // XENIA_BASE_NATIVE_SYSTEM_UI_TRACE_H_
