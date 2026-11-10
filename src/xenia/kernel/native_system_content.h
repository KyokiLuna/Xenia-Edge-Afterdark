/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia contributors. All rights reserved.                    *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */
#ifndef XENIA_KERNEL_NATIVE_SYSTEM_CONTENT_H_
#define XENIA_KERNEL_NATIVE_SYSTEM_CONTENT_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace xe {
class Memory;
namespace kernel {
class GuestRuntimeCallExecutor;
class NativeSystemAllocationAuthority;
class NativeSystemUiStartupCatalog;
class NativeSystemUiStartupRuntime;
class XThread;
struct ValidatedGuestRuntimePlan;

enum class NativeSystemContentCall {
  kInitializeStorage,
  kCompleteStorageBoot,
  kAttachSource,
  kInitializeIndex,
  kLookup
};
struct NativeSystemContentCallProfile {
  NativeSystemContentCall call;
  std::string_view identity;
  uint32_t address, code_size;
  std::string_view code_sha256;
  size_t argument_count;
  bool returns_void = false;
  // This entry was already called by the native storage mount. The startup
  // role observes that source instead of attaching the same device twice.
  bool observe_nested_call = false;
};
std::span<const NativeSystemContentCallProfile> NativeSystemContentProfiles();
enum class NativeSystemContentPlanFamily { kAbsent, kComplete, kInvalid };
NativeSystemContentPlanFamily CheckNativeSystemContentPlanFamily(
    const ValidatedGuestRuntimePlan&, std::string* error = nullptr);

// Read-only observations of the hashed 17559 constructor/source layout. These
// are not setters, synthetic source descriptors, or permission to run a call.
struct NativeSystemContentManagerData {
  uint32_t initialized = 0, built = 0, index = 0;
  uint32_t load_task = 0, monitor_task = 0;
  std::array<uint32_t, 4> list_links{};
  struct Source {
    uint32_t path = 0, device = 0, type = 0, extended_file = 0;
  };
  std::array<Source, 8> sources{};
  bool PristineConstructor() const;
  bool HasBuiltinSource(uint32_t device) const;
};
std::optional<NativeSystemContentManagerData>
ReadNativeSystemContentManagerData(Memory&);
struct NativeSystemStorageData {
  struct Volume {
    std::string path;
    uint32_t device = 0, type = 0, state = 0;
  };
  uint16_t next_device = 0;
  uint32_t worker_task = 0, notification_task = 0;
  std::array<uint32_t, 4> work_event{}, acknowledgement{}, boot_event{};
  std::array<Volume, 20> volumes{};
  bool table_zero = false;
  bool Pristine() const;
  std::optional<uint32_t> FormattedBuiltinHdd() const;
};
std::optional<NativeSystemStorageData> ReadNativeSystemStorageData(Memory&);
// Only a system content-package path below the actual fixed native volume.
// Native lookup remains authoritative; this merely rejects an escaping or
// foreign output before the common backend opens it.
std::optional<std::string> NativeSystemContentRelativePackagePath(
    std::string_view native_path);

class NativeSystemContent {
 public:
  enum class Status {
    kPrepared,
    kStorageInitialized,
    kStorageBootCompleted,
    kSourceAttached,
    kIndexInitialized,
    kLookupAndOpenVerified,
    kFailed
  };
  struct Snapshot {
    Status status = Status::kPrepared;
    uint32_t source_device = 0;
    std::string manifest_sha256, package_path, toc_sha256;
    uint64_t toc_size = 0;
    // A common-backend STFS/TOC open is separate from an admitted native F2
    // content-provider bridge. This snapshot never grants avatar admission.
  };
  ~NativeSystemContent();
  Snapshot snapshot() const;
  // Exact fixed closure/root/data/import ownership. This validates driver
  // registration and later callback entry without requiring pristine mutable
  // state or claiming source/index readiness.
  bool CheckStorageDriverContext(uint32_t callback = 0) const;

 private:
  friend class NativeSystemUiStartupCatalog;
  friend class NativeSystemUiStartupRuntime;
  static std::shared_ptr<NativeSystemContent> Create(
      std::shared_ptr<GuestRuntimeCallExecutor>,
      std::shared_ptr<NativeSystemAllocationAuthority>, std::string* error);
  std::optional<std::vector<uint32_t>> Prepare(NativeSystemContentCall,
                                               XThread&, std::string* error);
  bool Observe(NativeSystemContentCall, XThread&, std::string* error);
  struct State;
  explicit NativeSystemContent(std::unique_ptr<State>);
  std::unique_ptr<State> state_;
};
}  // namespace kernel
}  // namespace xe
#endif
