/** Xenia : Xbox 360 Emulator Research Project; BSD license, see LICENSE. */
#pragma once
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
namespace xe::kernel {
class KernelState;
class XThread;
// Diagnostic-only original PPC code, not a native XAM function or an HLE spy.
// This helper never queues or dispatches an APC. The real I/O export and the
// actual issuing thread's existing APC boundary must perform those operations.
class FileIoApcFixture {
 public:
  struct Observation {
    uint32_t iosb_address, reserved, status, information, invocations;
  };
  static std::shared_ptr<FileIoApcFixture> Create(KernelState*, XThread&,
                                                std::string* error);
  // Valid only on the original actual SYSTEM caller, with exact source bytes
  // and owned read/write result guards. No untrusted callback address is used.
  std::optional<uint32_t> CheckedRoutine(XThread&) const;
  std::optional<uint32_t> CheckedContext(XThread&) const;
  std::optional<Observation> ReadAfterReturn(XThread&) const;
 private:
  struct State;
  explicit FileIoApcFixture(std::weak_ptr<State>, const XThread*);
  bool IsOriginalCaller(XThread&) const;
  const XThread* issuer_identity_;
  // Kernel alone retains the code owner until its all-CPU-stopped resource
  // retirement boundary. A diagnostic wrapper cannot extend it past Kernel.
  std::weak_ptr<State> state_;
};
}  // namespace xe::kernel
