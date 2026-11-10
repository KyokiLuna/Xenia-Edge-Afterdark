// Private candidate: native route remains closed pending cohesive integration.
// Source metadata only; actual native bodies and input stay unchanged.
#pragma once
#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include "xenia/apu/audio_system.h"
#include "xenia/apu/native_audio_observation.h"
#include "xenia/base/physical_range_leases.h"
#include "xenia/cpu/ppc/ppc_context.h"
#include "xenia/gpu/gpu_completion.h"
#include "xenia/kernel/native_notification_frame_protocol.h"
namespace xe::gpu {
struct SystemCommandDescriptor;
}
namespace xe::kernel {
class KernelState;
class NativeImportDomain;
class NativeSystemUiStartupCatalog;
class NativeSystemGraphicsTransport;
class NativeNotificationPresentationObserver;
class NativeSystemUiVisibilityEvent;

// Detached diagnostic data only. Neither a normal native return nor accepted
// mixed SDL PCM establishes notification isolation or physical audibility.
struct NativeCueObservationResult {
  struct Event {
    notification_detail::NativeCueCall call;
    uint32_t result = 0;
  };
  bool armed = false, sound_enabled = false, metadata_lost = false;
  bool request_seen = false, voice_created = false, buffer_submitted = false;
  bool start_returned = false, start_accepted = false;
  bool stop_returned = false, stop_accepted = false, detached = false;
  bool cleanup_returned = false, release_returned = false, class_destroyed = false;
  bool engine_retiring = false, events_overflow = false, multiple_requests = false;
  uint64_t domain = 0, root_generation = 0, foreground = 0;
  uint64_t publication_serial = 0, control_serial = 0, request_serial = 0;
  uint64_t start_serial = 0;
  uint32_t engine = 0, driver = 0, control = 0, hxui = 0, voice = 0;
  uint32_t cache = 0, suppression = 0, event_count = 0;
  notification_detail::TextSenderText filename;
  std::array<Event, 96> events{};
  std::shared_ptr<const apu::NativeAudioClientKey> registration;
  std::unique_ptr<apu::NativeAudioObservation::Result> audio;
};

// Immutable output provenance from one actual nonmodal draw and its Set.
// Private construction plus same-arm checks prevent a caller-created bitmap
// from standing in for a native display-information read. This grants no
// Source, physical memory, execution, completion or visibility authority.
class NativeNonmodalOutputProvenance {
 public:
  const memory::PhysicalAllocationOwner& owner() const { return owner_; }
  uint64_t domain_id() const { return domain_; }
  uint64_t root_load_generation() const { return root_generation_; }
  uint64_t foreground_generation() const { return foreground_; }
  uint32_t thread_id() const { return thread_; }
  uint32_t native_task() const { return task_; }
  uint32_t frame_sp() const { return frame_sp_; }
  uint32_t native_frame_counter() const { return counter_; }
  uint64_t frame_serial() const { return serial_; }
  const std::array<uint8_t, 0x58>& display_information() const { return info_; }
 private:
  friend class NativeNotificationPresentationObserver;
  memory::PhysicalAllocationOwner owner_;
  uint64_t domain_ = 0, root_generation_ = 0, foreground_ = 0, serial_ = 0;
  uint32_t thread_ = 0, task_ = 0, frame_sp_ = 0, counter_ = 0;
  std::array<uint8_t, 0x58> info_{};
  NativeNonmodalOutputProvenance() = default;
};

// Same armed recorded native frame without Draw/display-info. This grants no
// surface, positive visibility, Source or GPU authority. Only the real observer
// can issue it; the complete control grammar and actual owner are still
// checked.
class NativeNonmodalControlProvenance {
 public:
  const memory::PhysicalAllocationOwner& owner() const { return owner_; }
  uint64_t domain_id() const { return domain_; }
  uint64_t root_load_generation() const { return root_generation_; }
  uint64_t foreground_generation() const { return foreground_; }
  uint32_t thread_id() const { return thread_; }
  uint32_t native_task() const { return task_; }
  uint32_t frame_sp() const { return frame_sp_; }
  uint32_t native_frame_counter() const { return counter_; }
  uint64_t frame_serial() const { return serial_; }

 private:
  friend class NativeNotificationPresentationObserver;
  memory::PhysicalAllocationOwner owner_;
  uint64_t domain_ = 0, root_generation_ = 0, foreground_ = 0, serial_ = 0;
  uint32_t thread_ = 0, task_ = 0, frame_sp_ = 0, counter_ = 0;
  NativeNonmodalControlProvenance() = default;
};

// A completed native CPU frame/clear observation. A draw candidate alone never
// grants presenter visibility: Root must match the exact Source's actual GPU
// output/completion, current modal epoch, and current foreground/root owner.
class NativeNonmodalFrameWitness {
 public:
  enum class Kind { kEligibleDrawCandidate, kInactiveFrame, kNativeEmptyClear };
  Kind kind() const { return kind_; }
  const memory::PhysicalAllocationOwner& owner() const { return owner_; }
  uint64_t domain_id() const { return domain_id_; }
  uint64_t root_load_generation() const { return root_generation_; }
  uint64_t foreground_generation() const { return foreground_generation_; }
  uint64_t revision() const { return revision_; }
  uint64_t frame_serial() const { return frame_serial_; }
  bool modal_visibility_known() const { return modal_known_; }
  uint64_t modal_visibility_sequence() const { return modal_sequence_; }
  uint64_t modal_foreground_generation() const { return modal_foreground_; }
  uint32_t native_frame_counter() const { return frame_counter_; }
  uint32_t thread_id() const { return thread_id_; }
  uint32_t native_task() const { return task_; }
  uint32_t frame_sp() const { return frame_sp_; }
  const std::optional<gpu::NativeFrameSubmissionIdentity>& source() const {
    return source_;
  }

 private:
  friend class NativeNotificationPresentationObserver;
  NativeNonmodalFrameWitness() = default;
  Kind kind_ = Kind::kInactiveFrame;
  memory::PhysicalAllocationOwner owner_;
  uint64_t domain_id_ = 0, root_generation_ = 0, foreground_generation_ = 0;
  uint64_t revision_ = 0, frame_serial_ = 0;
  bool modal_known_ = false;
  uint64_t modal_sequence_ = 0, modal_foreground_ = 0;
  uint32_t frame_counter_ = 0, thread_id_ = 0, task_ = 0, frame_sp_ = 0;
  std::optional<gpu::NativeFrameSubmissionIdentity> source_;
};

class NativeNotificationPresentationObserver
    : public std::enable_shared_from_this<
          NativeNotificationPresentationObserver> {
 public:
  struct Config {
    std::shared_ptr<const NativeSystemUiStartupCatalog> catalog;
    std::shared_ptr<NativeImportDomain> domain;
  };
  static std::shared_ptr<NativeNotificationPresentationObserver> Create(Config);
  static std::shared_ptr<NativeNotificationPresentationObserver> Lookup(
      KernelState*);
  class DrawObservation {
   public:
    DrawObservation() = default;
    DrawObservation(DrawObservation&&) noexcept;
    DrawObservation& operator=(DrawObservation&&) noexcept;
    ~DrawObservation();
    DrawObservation(const DrawObservation&) = delete;
    DrawObservation& operator=(const DrawObservation&) = delete;
    // After the genuine existing display-info operation. No PPC, presentation,
    // sound, input, guest writes, or waiting is performed by this observer.
    void Complete() noexcept;

   private:
    friend class NativeNotificationPresentationObserver;
    struct State;
    explicit DrawObservation(std::shared_ptr<State>);
    std::shared_ptr<State> state_;
  };
  // Existing kernel1BA shim calls before its real operation, then Complete.
  // Foreign/rootless callers, wrong LR/stack/current task, stale frame, modal
  // r15, or mismatched manager->scene produce a no-op invalid observation.
  static DrawObservation BeforeNativeDisplayInformation(
      KernelState*, const cpu::ppc::PPCContext*, uint32_t out_display_info);
  // Narrow non-stopping JIT callbacks for authenticated817C8868 only. These
  // copy diagnostic metadata; they grant no UI/Source/visibility authority.
  static void ObserveTextSenderEntry(void*, uint64_t function) noexcept;
  static void ObserveTextSenderReturn(void*, uint64_t function) noexcept;
  // Exact-caption common measurement only; diagnostic copied values, no UI authority.
  static void ObserveTextMeasureEntry(void*, uint64_t function) noexcept;
  static void ObserveTextMeasureReturn(void*, uint64_t function) noexcept;
  static void ObserveFontAcquireEntry(void*, uint64_t function) noexcept;
  static void ObserveFontAcquireReturn(void*, uint64_t function) noexcept;
  // Called after the actual1F3 handle publication and before activation. The
  // prepared token and native callback origin must both match this runtime.
  static void ObserveNativeAudioPublished(
      void*, apu::AudioSystem::PreparedClient&, uint32_t record, uint32_t output) noexcept;
  static std::shared_ptr<const apu::NativeAudioClientKey> DescribeNativeCueFrame(
      void*, uint32_t handle, uint32_t samples) noexcept;
  static void ObserveNativeCueEntry(void*, uint64_t function) noexcept;
  static void ObserveNativeCueReturn(void*, uint64_t function) noexcept;
  // One opt-in window before the existing fixed290 request. No guest execution
  // or sound is introduced. Allocate/copy/export only outside audio locks.
  bool ArmNativeCueObservation(XThread&, bool sound_enabled) noexcept;
  void CancelNativeCueObservation() noexcept;
  std::unique_ptr<NativeCueObservationResult> CloseNativeCueObservation(XThread&) noexcept;
  bool IsAccepting() const noexcept;
  void BeginStop() noexcept;
  bool Drain() noexcept;

 private:
  friend class NativeSystemGraphicsTransport;
  // Attach only the service-created observer for this exact live cohort.
  static std::shared_ptr<NativeNotificationPresentationObserver> LookupForConfig(
      Config);
  // Only the actual transport owns successful Query and accepted Set/Source.
  // There are no public ready/eligible/closed booleans for a caller to assert.
  bool ObserveSuccessfulNativeFrameQuery(const cpu::ppc::PPCContext&) noexcept;
  // Real unforgeable native SYS_UI9 event only; call outside transport locks.
  void ObserveNativeModalTransition(
      const NativeSystemUiVisibilityEvent&) noexcept;
  class SetObservation {
   public:
    SetObservation() = default;
    SetObservation(SetObservation&&) noexcept;
    SetObservation& operator=(SetObservation&&) noexcept;
    ~SetObservation();
    SetObservation(const SetObservation&) = delete;
    SetObservation& operator=(const SetObservation&) = delete;
    // A genuine same-Set draw snapshot only; caller must independently pin
    // the output/inputs and seal them against the exact immutable Source.
    std::shared_ptr<const NativeNonmodalOutputProvenance>
    output_provenance() const noexcept;
    std::shared_ptr<const NativeNonmodalControlProvenance> control_provenance()
        const noexcept;
    // After the real Set's complete immutable Source was accepted/frozen.
    // Root must supply that exact Source identity, never a guessed serial.
    std::optional<NativeNonmodalFrameWitness> CompleteSource(
        const gpu::NativeFrameSubmissionIdentity&) noexcept;
    // Only a genuine accepted all-zero Set from the checked clear path.
    // No Source identity or fabricated GPU completion is created.
    std::optional<NativeNonmodalFrameWitness> CompleteNativeClear() noexcept;

   private:
    friend class NativeNotificationPresentationObserver;
    struct State;
    explicit SetObservation(std::shared_ptr<State>);
    std::shared_ptr<State> state_;
  };
  // Copies authentic current frame/clear facts while the guest is in its Set
  // shim. It consumes the frame arm exactly once only on Complete. Destruction
  // without completion cannot manufacture inactive/clear or visible output.
  SetObservation BeforeNativeSet(const cpu::ppc::PPCContext&,
                                 const gpu::SystemCommandDescriptor&) noexcept;
  void RetireAfterKernelStop() noexcept;
  struct State;
  explicit NativeNotificationPresentationObserver(std::shared_ptr<State>);
  std::shared_ptr<State> state_;
};
}  // namespace xe::kernel
