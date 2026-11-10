/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia contributors. All rights reserved.                    *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */
#ifndef XENIA_KERNEL_NATIVE_MANAGED_FORMAT_FRONTDOOR_H_
#define XENIA_KERNEL_NATIVE_MANAGED_FORMAT_FRONTDOOR_H_
#include <array>
#include <cstdint>
#include <memory>
#include <map>
#include <vector>
#include <optional>
#include <span>
#include "xenia/kernel/guest_runtime.h"
namespace xe::kernel {
class KernelState;
class NativeImportDomain;
class NativeManagedFormatIo;
class GuestRuntimeCallExecutor;
class XThread;
namespace test { class NativeManagedFormatFrontdoorTestAccess; }
namespace xam { class ManagedFormatSourceProof; class ContentManager; }
// Only a genuine registry-backed enumeration delivery followed by the exact
// loaded Dash constructors and scene factories can mint this selection. Guest
// records, numeric IDs and object addresses cannot construct one.
class NativeManagedFormatSelection final {
 public:
  const std::shared_ptr<const xam::ManagedFormatSourceProof>& proof() const { return proof_; }
  uint32_t registry_device_id() const { return device_; }
  uint32_t progress_object() const { return progress_; }
  uint32_t overlap() const { return overlap_; }
  uint32_t title_hmodule() const { return title_; }
  uint64_t title_load_generation() const { return generation_; }
  uint64_t serial() const { return serial_; }
 private:
  friend class NativeManagedFormatFrontdoor;
  NativeManagedFormatSelection() = default;
  std::shared_ptr<const xam::ManagedFormatSourceProof> proof_;
  uint32_t device_ = 0, progress_ = 0, overlap_ = 0, title_ = 0;
  uint64_t generation_ = 0, serial_ = 0;
};
class NativeManagedFormatFrontdoor final : public std::enable_shared_from_this<NativeManagedFormatFrontdoor> {
 public:
  struct Config {
    KernelState* kernel = nullptr;
    std::shared_ptr<GuestRuntimeCallExecutor> calls;
    std::shared_ptr<NativeImportDomain> domain;
    xam::ContentManager* content = nullptr;
  };
  static std::shared_ptr<NativeManagedFormatFrontdoor> Create(Config);
  ~NativeManagedFormatFrontdoor();
  // Only a successful, synchronous real XamEnumerate250 delivery from the
  // typed device enumerator calls this. The proof is the row's retained host
  // capsule; output bytes are checked for equality, never used to mint proof.
  static void ObserveDeviceEnumeration(KernelState*, uint32_t enumerator,
      uint32_t output, std::span<const uint8_t, 0x50>,
      std::shared_ptr<const xam::ManagedFormatSourceProof>, uint32_t registry_id) noexcept;
  static constexpr bool HooksFunction(uint32_t at) {
    switch (at) {
      case 0x92264450: case 0x92264280: case 0x922F73B8: case 0x92263BF8:
      case 0x922F9B08: case 0x922F7930: case 0x922F78B0:
      case 0x922F58D0: case 0x922F63D8: case 0x922F6398:
      case 0x9218D9E8: case 0x816A2A48: case 0x816A26F8:
      case 0x816A2588: return true;
      default: return false;
    }
  }
  // Actual successful639 output, with bytes serialized in owned host storage.
  // Only an already-armed authenticated native row refresh may consume it.
  static void ObserveLocalizedDeviceData(KernelState*, uint32_t output,
      std::span<const uint8_t, 0x50>,
      std::shared_ptr<const xam::ManagedFormatSourceProof>, uint32_t device) noexcept;
  static void ObserveEntry(void*, uint64_t function) noexcept;
  static void ObserveReturn(void*, uint64_t function) noexcept;
  static std::optional<GuestRuntime::ExecutionResult> TryStart(KernelState*,
      const std::array<uint64_t, 6>&) noexcept;
  static std::optional<GuestRuntime::ExecutionResult> TryCancel(KernelState*,
      const std::array<uint64_t, 2>&) noexcept;
  void BeginStop() noexcept;
  bool Drain() noexcept;
 private:
  friend class test::NativeManagedFormatFrontdoorTestAccess;
  // Exact Dash lifetime protocol, not guest authority. Only the outer reader
  // supplies authenticated frames/bytes and actual typed-enumerator capsules.
  struct Frame {
    uint32_t title = 0, thread = 0, pcr = 0, sp = 0, lr = 0;
    uint64_t generation = 0;
    bool operator==(const Frame&) const = default;
    bool valid() const { return title && thread && pcr && sp && lr && generation; }
  };
  using Raw = std::array<uint8_t, 0x50>;
  struct Object {
    Raw raw{};
    std::shared_ptr<const xam::ManagedFormatSourceProof> proof;
    uint32_t device = 0;
    uint64_t serial = 0;
  };
  struct Snapshot {
    std::optional<Raw> raw;
    uint32_t object = 0, hxui = 0, manager = 0, selected = 0;
  };
  struct Geometry {
    bool cached = false;
    uint64_t total = 0, used = 0;
  };
  struct Call {
    Frame frame;
    std::array<uint32_t, 5> args{};
    Object source;
    uint64_t serial = 0, scene_serial = 0;
    uint32_t progress = 0, hxui = 0;
    bool ambiguous = false, refresh_constructor = false;
    uint64_t refresh_parent_serial = 0;
    std::optional<Raw> localized;
  };
  class Lineage {
   public:
    bool Enter(Frame, uint32_t function, std::array<uint32_t, 5>, const Snapshot&);
    void Leave(Frame, uint32_t function, uint32_t result, const Snapshot&);
    void Deliver(Frame, uint32_t handle, uint32_t output, const Raw&,
                 std::shared_ptr<const xam::ManagedFormatSourceProof>, uint32_t);
    std::optional<Call> Pending(Frame, uint32_t function) const;
    std::optional<Call> RefreshForLocalized(Frame, uint32_t output) const;
    bool Localized(Frame, uint32_t output, const Raw&,
        std::shared_ptr<const xam::ManagedFormatSourceProof>, uint32_t device,
        Geometry);
    std::optional<Object> Progress(uint32_t object) const;
    uint64_t ProgressSerial(uint32_t object) const;
    void Stop();
    bool idle() const { return calls_.empty(); }
   private:
    struct Delivery { Frame frame; uint32_t output = 0, handle = 0; Object row; };
    struct Scene { Object row; uint64_t serial = 0; uint32_t hxui = 0; bool transferred = false; };
    uint64_t Serial();
    bool stopped_ = false;
    uint64_t next_ = 1;
    std::map<uint32_t, Delivery> deliveries_;
    std::map<uint32_t, Object> objects_, selected_, progress_;
    std::map<uint32_t, uint64_t> constructing_;
    std::map<uint32_t, Scene> scenes_;
    std::map<std::pair<uint32_t, uint32_t>, Call> calls_;
  };
  struct State;
  explicit NativeManagedFormatFrontdoor(std::shared_ptr<State>);
  std::shared_ptr<State> state_;
};
}  // namespace xe::kernel
#endif
