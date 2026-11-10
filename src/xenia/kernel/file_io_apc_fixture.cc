/** Xenia : Xbox 360 Emulator Research Project; BSD license, see LICENSE. */
#include "xenia/kernel/file_io_apc_fixture.h"
#include <algorithm>
#include <array>
#include <cstring>
#include "xenia/base/byte_order.h"
#include "xenia/base/mutex.h"
#include "xenia/cpu/processor.h"
#include "xenia/cpu/raw_module.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/util/guest_io_buffer.h"
#include "xenia/kernel/xthread.h"
#include "xenia/memory.h"
namespace xe::kernel {
namespace {
// PIO_APC_ROUTINE(context=r3, iosb=r4, reserved=r5). Only r6 is clobbered.
// The last store increments the owned result's invocation count, then returns.
constexpr std::array<uint32_t, 10> kCode{
    0x90830000, 0x90A30004, 0x80C40000, 0x90C30008, 0x80C40004,
    0x90C3000C, 0x80C30010, 0x38C60001, 0x90C30010, 0x4E800020};
constexpr uint32_t kDataSize = 128, kResultOffset = 16;
constexpr size_t kResultSize = 20;
}
struct FileIoApcFixture::State {
  KernelState* kernel = nullptr;
  object_ref<XThread> issuer;
  uint32_t code = 0, data = 0, code_extent = 0;
  cpu::RawModule* module = nullptr;
  ~State() {
    // On a failed Create no address has escaped. On success Kernel retains
    // this State until resident_native_resources_ is cleared after CPU stop,
    // while processor, memory and object table still exist. Never execute PPC
    // or free this code at diagnostic callback-drain/fixture-handle cleanup.
    if (module) kernel->processor()->RemoveModule(module);
    if (code) {
      kernel->memory()->LookupHeap(code)->Protect(
          code, code_extent, kMemoryProtectRead | kMemoryProtectWrite);
      kernel->memory()->SystemHeapFree(code);
    }
    if (data) kernel->memory()->SystemHeapFree(data);
  }
  bool Caller(XThread& thread) const {
    return XThread::IsInThread(&thread) && issuer.get() == &thread &&
           thread.kernel_state() == kernel &&
           thread.native_frame_thread_identity().process_type ==
               X_PROCTYPE_SYSTEM &&
           thread.thread_state()->context()->kernel_state == kernel &&
           !global_critical_region::is_held_by_current_thread();
  }
  bool Source(XThread& thread) const {
    if (!Caller(thread) ||
        kernel->memory()->QueryRangeAccess(code, code_extent) !=
            memory::PageAccess::kReadOnly ||
        kernel->memory()->QueryRangeAccess(data, kDataSize) !=
            memory::PageAccess::kReadWrite) return false;
    std::array<uint8_t, kCode.size() * 4> bytes{};
    if (XFAILED(util::ReadGuestIoBuffer(*kernel->memory(), code, bytes)))
      return false;
    for (size_t i = 0; i < kCode.size(); ++i)
      if (xe::load_and_swap<uint32_t>(bytes.data() + i * 4) != kCode[i])
        return false;
    return true;
  }
  std::optional<Observation> Read(XThread& thread) const {
    if (!Source(thread)) return {};
    std::array<uint8_t, kDataSize> bytes{};
    if (XFAILED(util::ReadGuestIoBuffer(*kernel->memory(), data, bytes)))
      return {};
    for (size_t i = 0; i < bytes.size(); ++i)
      if ((i < kResultOffset || i >= kResultOffset + kResultSize) &&
          bytes[i] != 0xA5) return {};
    const auto* result = bytes.data() + kResultOffset;
    return Observation{xe::load_and_swap<uint32_t>(result),
                       xe::load_and_swap<uint32_t>(result + 4),
                       xe::load_and_swap<uint32_t>(result + 8),
                       xe::load_and_swap<uint32_t>(result + 12),
                       xe::load_and_swap<uint32_t>(result + 16)};
  }
};
FileIoApcFixture::FileIoApcFixture(std::weak_ptr<State> state,
                                     const XThread* issuer)
    : issuer_identity_(issuer), state_(std::move(state)) {}
bool FileIoApcFixture::IsOriginalCaller(XThread& thread) const {
  // Compare identity and real current TLS BEFORE promoting the weak owner. A
  // UI/wrong-host call must not keep Kernel-owned teardown resources alive.
  return &thread == issuer_identity_ && XThread::IsInThread(&thread);
}
std::shared_ptr<FileIoApcFixture> FileIoApcFixture::Create(
    KernelState* kernel, XThread& thread, std::string* error) {
  auto fail = [&](const char* text) -> std::shared_ptr<FileIoApcFixture> {
    if (error) *error = text;
    return {};
  };
  if (!kernel || !XThread::IsInThread(&thread) ||
      thread.kernel_state() != kernel ||
      thread.native_frame_thread_identity().process_type != X_PROCTYPE_SYSTEM ||
      global_critical_region::is_held_by_current_thread())
    return fail("Actual original SYSTEM caller required");
  auto state = std::make_shared<State>();
  state->kernel = kernel;
  state->issuer = retain_object(&thread);
  auto* memory = kernel->memory();
  // Own the entire host protection unit, including on a16KiB host. Guest
  // bytes are read-only; RawModule registers executable JIT code separately.
  state->code_extent = std::max<uint32_t>(4096, uint32_t(memory::page_size()));
  state->code = memory->SystemHeapAlloc(state->code_extent, state->code_extent);
  state->data = memory->SystemHeapAlloc(kDataSize);
  if (!state->code || !state->data)
    return fail("Owned APC fixture allocation failed");
  auto* code = memory->TranslateVirtual(state->code);
  for (size_t i = 0; i < kCode.size(); ++i)
    xe::store_and_swap<uint32_t>(code + i * 4, kCode[i]);
  auto* data = memory->TranslateVirtual(state->data);
  std::memset(data, 0xA5, kDataSize);
  std::memset(data + kResultOffset, 0, kResultSize);
  if (!memory->LookupHeap(state->code)->Protect(
          state->code, state->code_extent, kMemoryProtectRead))
    return fail("Read-only PPC source protection failed");
  auto module = std::make_unique<cpu::RawModule>(kernel->processor());
  module->set_name("private-file-io-apc-fixture");
  module->SetAddressRange(state->code, uint32_t(kCode.size() * 4));
  state->module = module.get();
  if (!kernel->processor()->AddModule(std::move(module))) {
    state->module = nullptr;
    return fail("Owned PPC fixture module registration failed");
  }
  auto fixture = std::shared_ptr<FileIoApcFixture>(
      new FileIoApcFixture(state, &thread));
  // Once the callback address escapes, even queued/cancelled APCs cannot
  // outlive it: Kernel owns the whole code/result allocation until CPU stop.
  kernel->RetainNativeResourceUntilTeardown(state);
  if (error) error->clear();
  return fixture;
}
std::optional<uint32_t> FileIoApcFixture::CheckedRoutine(XThread& thread) const {
  if (!IsOriginalCaller(thread)) return {};
  const auto state = state_.lock();
  return state && state->Read(thread) ? std::optional(state->code) : std::nullopt;
}
std::optional<uint32_t> FileIoApcFixture::CheckedContext(XThread& thread) const {
  if (!IsOriginalCaller(thread)) return {};
  const auto state = state_.lock();
  return state && state->Read(thread)
             ? std::optional(state->data + kResultOffset) : std::nullopt;
}
std::optional<FileIoApcFixture::Observation>
FileIoApcFixture::ReadAfterReturn(XThread& thread) const {
  if (!IsOriginalCaller(thread)) return {};
  const auto state = state_.lock();
  return state ? state->Read(thread) : std::nullopt;
}
}  // namespace xe::kernel
