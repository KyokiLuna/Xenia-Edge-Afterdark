/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia contributors. All rights reserved.                    *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/native_gpu_completion_probe.h"

#include <atomic>
#include <chrono>
#include <memory>

#include "xenia/base/logging.h"
#include "xenia/emulator.h"
#include "xenia/gpu/command_processor.h"
#include "xenia/gpu/gpu_completion.h"
#include "xenia/gpu/graphics_system.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/user_module.h"
#include "xenia/kernel/xthread.h"

namespace xe::kernel {
X_HRESULT RunNativeGpuContextAllocationProbe(
    KernelState* kernel_state, const object_ref<UserModule>& module) {
  auto* thread = XThread::GetCurrentThread();
  if (!kernel_state || !module || !thread ||
      thread->kernel_state() != kernel_state ||
      module->kernel_state() != kernel_state ||
      thread->guest_object<X_KTHREAD>()->process_type != X_PROCTYPE_SYSTEM) {
    return X_E_INVALIDARG;
  }
  auto* graphics = kernel_state->emulator()->graphics_system();
  auto* cp = graphics ? graphics->command_processor() : nullptr;
  if (!cp) {
    return X_E_NOT_IMPLEMENTED;
  }
  struct Observation {
    gpu::GuestRenderBankAllocationResult first, repeated;
    std::atomic<bool> returned{false};
  };
  const auto observation = std::make_shared<Observation>();
  cp->CallInThread([cp, observation, module] {
    (void)module;  // Keep the resident module alive while this work is queued.
    observation->first = cp->AllocateNativeGuestRenderBank();
    if (observation->first.status ==
        gpu::GuestRenderBankAllocationResult::Status::kReady) {
      observation->repeated = cp->AllocateNativeGuestRenderBank();
    }
    observation->returned.store(true, std::memory_order_release);
  });
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(15);
  while (!observation->returned.load(std::memory_order_acquire) &&
         std::chrono::steady_clock::now() < deadline) {
    thread->Delay(0, 0, uint64_t(-100000ll));
  }
  if (!observation->returned.load(std::memory_order_acquire)) {
    XELOGE("Native GPU context probe: CP allocation deadline exceeded");
    return X_E_FAIL;
  }
  const auto& first = observation->first;
  const auto& repeated = observation->repeated;
  const bool ready =
      first.status == gpu::GuestRenderBankAllocationResult::Status::kReady &&
      repeated.status == gpu::GuestRenderBankAllocationResult::Status::kReady &&
      first.independent_resources && repeated.independent_resources &&
      first.title_unchanged && repeated.title_unchanged &&
      first.title_command_state_unchanged &&
      repeated.title_command_state_unchanged &&
      first.private_register_readback && repeated.private_register_readback &&
      first.private_registers_unchanged &&
      repeated.private_registers_unchanged &&
      first.independent_primitive_processor &&
      repeated.independent_primitive_processor &&
      first.independent_texture_cache && repeated.independent_texture_cache &&
      first.title_register_file_identity && first.native_bank_identity &&
      first.native_register_file_identity &&
      first.native_primitive_processor_identity &&
      first.native_texture_cache_identity &&
      first.native_register_file_identity !=
          first.title_register_file_identity &&
      first.title_register_file_identity ==
          repeated.title_register_file_identity &&
      first.native_bank_identity == repeated.native_bank_identity &&
      first.native_register_file_identity ==
          repeated.native_register_file_identity &&
      first.native_primitive_processor_identity ==
          repeated.native_primitive_processor_identity &&
      first.native_texture_cache_identity ==
          repeated.native_texture_cache_identity &&
      repeated.already_allocated && first.edram_bytes &&
      first.edram_bytes == repeated.edram_bytes &&
      first.descriptor_set_count == repeated.descriptor_set_count;
  XELOGI(
      "Native GPU context probe: status={} stage={} repeated_status={} "
      "already_allocated={} independent={} title_unchanged={} "
      "scale={}x{} FSI={} EDRAM_bytes={} descriptor_sets={} "
      "shared_bindings={} ready={}; resource validation only, "
      "this check submits no native GPU commands",
      unsigned(first.status), unsigned(first.stage), unsigned(repeated.status),
      repeated.already_allocated,
      first.independent_resources && repeated.independent_resources,
      first.title_unchanged && repeated.title_unchanged, first.draw_scale_x,
      first.draw_scale_y, first.pixel_shader_interlock, first.edram_bytes,
      first.descriptor_set_count, first.shared_memory_binding_count, ready);
  XELOGI(
      "Native GPU context probe: private_RF={} private_readback={} "
      "private_preserved={} primitive_independent={} texture_independent={} "
      "title_commands_unchanged={} repeated_identities={} "
      "title_RF_identity={:X} native_bank_identity={:X} "
      "native_RF_identity={:X} native_primitive_identity={:X} "
      "native_texture_identity={:X}; private bank validation only, "
      "no register reset or activation by this check",
      first.native_register_file_identity &&
          first.native_register_file_identity !=
              first.title_register_file_identity,
      first.private_register_readback && repeated.private_register_readback,
      first.private_registers_unchanged && repeated.private_registers_unchanged,
      first.independent_primitive_processor &&
          repeated.independent_primitive_processor,
      first.independent_texture_cache && repeated.independent_texture_cache,
      first.title_command_state_unchanged &&
          repeated.title_command_state_unchanged,
      first.native_bank_identity &&
          first.native_bank_identity == repeated.native_bank_identity &&
          first.native_register_file_identity ==
              repeated.native_register_file_identity &&
          first.native_primitive_processor_identity ==
              repeated.native_primitive_processor_identity &&
          first.native_texture_cache_identity ==
              repeated.native_texture_cache_identity,
      first.title_register_file_identity, first.native_bank_identity,
      first.native_register_file_identity,
      first.native_primitive_processor_identity,
      first.native_texture_cache_identity);
  return ready ? X_E_SUCCESS : X_E_FAIL;
}

X_HRESULT RunNativeGpuCompletionProbe(KernelState* kernel_state,
                                      const object_ref<UserModule>& module,
                                      gpu::GpuCompletion* completed) {
  if (!kernel_state || !XThread::IsInThread() || !module ||
      module->kernel_state() != kernel_state) {
    return X_E_INVALIDARG;
  }
  auto* thread = XThread::GetCurrentThread();
  if (thread->guest_object<X_KTHREAD>()->process_type != X_PROCTYPE_SYSTEM) {
    return X_E_INVALIDARG;
  }
  auto* graphics = kernel_state->emulator()->graphics_system();
  auto* cp = graphics ? graphics->command_processor() : nullptr;
  if (!cp || !cp->supports_gpu_completion()) {
    XELOGE("Native GPU completion probe: backend unsupported");
    return X_E_NOT_IMPLEMENTED;
  }
  struct Observation {
    gpu::GpuCompletion token;
    std::atomic<bool> captured{false};
    std::atomic<unsigned> notifications{0};
  };
  const auto observation = std::make_shared<Observation>();
  cp->CaptureGpuCompletionAsync(
      {std::make_shared<object_ref<UserModule>>(module)},
      [observation](const gpu::GpuCompletion& token) {
        observation->token = token;
        observation->captured.store(true, std::memory_order_release);
      },
      [observation](const gpu::GpuCompletion&) {
        observation->notifications.fetch_add(1, std::memory_order_release);
      });
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (std::chrono::steady_clock::now() < deadline) {
    if (observation->captured.load(std::memory_order_acquire) &&
        observation->token.status() != gpu::GpuCompletionStatus::kPending &&
        observation->notifications.load(std::memory_order_acquire)) {
      break;
    }
    thread->Delay(0, 0, uint64_t(-100000ll));
  }
  // Capture may still be queued at the deadline. Do not read an unpublished
  // token or let its asynchronous observer retain a pointer to this stack.
  if (!observation->captured.load(std::memory_order_acquire)) {
    XELOGE("Native GPU completion probe: CP capture deadline exceeded");
    return X_E_FAIL;
  }
  const auto& token = observation->token;
  const unsigned notifications =
      observation->notifications.load(std::memory_order_acquire);
  const bool complete = token.status() == gpu::GpuCompletionStatus::kComplete &&
                        token.backend_epoch() && token.submission() &&
                        notifications == 1;
  XELOGI(
      "Native GPU completion probe: epoch={} submission={} status={} "
      "notifications={} complete={}; preceding title GPU work only",
      token.backend_epoch(), token.submission(), unsigned(token.status()),
      notifications, complete);
  if (complete && completed) {
    *completed = token;
  }
  return complete ? X_E_SUCCESS : X_E_FAIL;
}
}  // namespace xe::kernel
