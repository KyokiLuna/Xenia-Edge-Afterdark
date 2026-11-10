/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_APU_AUDIO_SYSTEM_H_
#define XENIA_APU_AUDIO_SYSTEM_H_

#include <array>
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

#include "xenia/apu/audio_client_lifecycle.h"
#include "xenia/apu/independent_audio_session.h"
#include "xenia/apu/native_audio_observation.h"
#include "xenia/base/mutex.h"
#include "xenia/base/threading.h"
#include "xenia/cpu/processor.h"
#include "xenia/kernel/xthread.h"
#include "xenia/memory.h"
#include "xenia/xbox.h"

namespace xe {
namespace apu {

constexpr fourcc_t kAudioSaveSignature = make_fourcc("XAUD");

class AudioDriver;
class AudioDuckerPolicy;
class XmaDecoder;

class AudioSystem {
  struct ClientRegistration;
  class ClientOperation;

 public:
  // TODO(gibbed): respect XAUDIO2_MAX_QUEUED_BUFFERS somehow (ie min(64,
  // XAUDIO2_MAX_QUEUED_BUFFERS))
  static constexpr size_t kMinimumQueuedFrames = 4;
  static constexpr size_t kMaximumQueuedFrames = 64;
  static constexpr uint32_t kAudioPumpInterval = 5333u;
  static constexpr uint32_t kAudioIntervalSlack = 400u;

  virtual ~AudioSystem();

  virtual std::string name() const = 0;

  Memory* memory() const { return memory_; }
  cpu::Processor* processor() const { return processor_; }
  XmaDecoder* xma_decoder() const { return xma_decoder_.get(); }

  virtual X_STATUS Setup(kernel::KernelState* kernel_state);
  virtual void Shutdown();
  // Stops the worker that runs the guest audio callbacks. Idempotent, and
  // Shutdown does it too.
  void StopWorker();

  class PreparedClient {
   public:
    PreparedClient() = default;
    PreparedClient(PreparedClient&&) = default;
    PreparedClient& operator=(PreparedClient&&) = default;
    PreparedClient(const PreparedClient&) = delete;
    PreparedClient& operator=(const PreparedClient&) = delete;
    size_t index() const { return index_; }

   private:
    friend class AudioSystem;
    AudioSystem* owner_ = nullptr;
    size_t index_ = 0;
    std::shared_ptr<ClientRegistration> registration_;
    // Also pins AudioSystem lifetime against Shutdown between prepare and
    // the export's guest handle commit / activation or cancellation.
    std::shared_ptr<ClientOperation> operation_;
  };
  X_STATUS PrepareClient(uint32_t callback, uint32_t callback_arg,
                         PreparedClient* prepared);
  // The export commits and activates/cancels synchronously in the same host
  // call. Never park a guest fiber while holding a prepared publication token.
  // Named callback code is leased; dynamic callbacks remain explicitly
  // unclassified. The argument's target allocation is owned by the guest and
  // must outlive callback drain. The 4-byte argument wrapper is owned here.
  X_STATUS ActivateClient(PreparedClient& prepared);
  void CancelPreparedClient(PreparedClient& prepared);
  X_STATUS RegisterClient(uint32_t callback, uint32_t callback_arg,
                          size_t* out_index);
  X_STATUS UnregisterClient(size_t index);

  // Copied identity from the exact prepared registration, before its export
  // commits output/activates. The key owns no module, driver or registration.
  std::shared_ptr<const NativeAudioClientKey> DescribePreparedClient(
      const PreparedClient& prepared);
  // Allocate observation storage outside these calls. These methods perform
  // only exact-current registration admission and bounded copied-data work.
  bool AttachNativeObservation(
      const std::shared_ptr<const NativeAudioClientKey>& key,
      const std::shared_ptr<NativeAudioObservation>& observation);
  bool DetachNativeObservation(
      const std::shared_ptr<const NativeAudioClientKey>& key,
      const std::shared_ptr<NativeAudioObservation>& observation);

  std::shared_ptr<AudioDuckerPolicy> ducker_policy() const {
    return ducker_policy_;
  }
  X_STATUS ReadCategoryVolume(uint32_t category, uint32_t output);
  X_STATUS ReadCategoryChangeMask(size_t index, uint32_t output);
  static constexpr size_t kMaxSaveBytes = 4096;
  // Only the native1F5 boundary can supply a copied actual origin key. A
  // matching key also requires this registration's executing callback thread.
  void SubmitFrame(
      size_t index, float* samples,
      const std::shared_ptr<const NativeAudioClientKey>& native_origin = {});

  // Get performance statistics for a client
  struct ClientPerformance {
    uint32_t frames_submitted;
    uint32_t frames_processed;
    uint32_t frames_dropped;
  };
  bool GetClientPerformance(size_t index, ClientPerformance* out_perf);

  // Creates an independent, non-registered driver instance.
  virtual AudioDriver* CreateDriver(xe::threading::Semaphore* semaphore,
                                    uint32_t frequency, uint32_t channels,
                                    bool need_format_conversion) = 0;

  // Prepared, silent until explicit Start. AudioSystem tracks stop/drain even
  // if the caller retains its session handle beyond shutdown.
  IndependentAudioPreparation PrepareIndependentAudio(PcmAudio pcm);

  bool Save(ByteStream* stream);
  bool Restore(ByteStream* stream);

  bool is_paused() const { return paused_.load(); }
  void Pause();
  void Resume();

 protected:
  explicit AudioSystem(cpu::Processor* processor);

  virtual void Initialize();

  void WorkerThreadMain();

  virtual X_STATUS CreateDriver(size_t index,
                                xe::threading::Semaphore* semaphore,
                                AudioDriver** out_driver) = 0;
  X_STATUS CreateClientDriver(size_t index, xe::threading::Semaphore* semaphore,
                              AudioDriver** out_driver);
  void DestroyDriver(AudioDriver* driver);

  Memory* memory_ = nullptr;
  cpu::Processor* processor_ = nullptr;
  kernel::KernelState* kernel_state_ = nullptr;
  std::unique_ptr<XmaDecoder> xma_decoder_;
  uint32_t queued_frames_;

  std::atomic<bool> worker_running_ = {false};
  kernel::object_ref<kernel::XHostThread> worker_thread_;

  // Guards clients_. The global lock is never taken while holding it, so its
  // holders can't delay the pump.
  std::mutex clients_mutex_;
  static constexpr size_t kMaximumClientCount = 8;
  struct ClientSlot {
    std::shared_ptr<ClientRegistration> registration;
    // Wall-clock deadline for this client's next pump.
    uint64_t next_pump_us = 0;
  };
  ClientSlot clients_[kMaximumClientCount];

  int FindFreeClient();

  std::unique_ptr<xe::threading::Semaphore>
      client_semaphores_[kMaximumClientCount];
  // Wakes the worker to re-scan clients on register, shutdown, and pause.
  std::unique_ptr<xe::threading::Event> pending_work_event_;

  std::atomic<bool> paused_{false};
  threading::Fence pause_fence_;
  std::unique_ptr<threading::Event> resume_event_;

 private:
  std::shared_ptr<ClientOperation> BeginClientOperation();
  void StopIndependentAudio();
  void PauseIndependentAudio(bool paused);
  std::vector<std::weak_ptr<IndependentAudioSession>> independent_audio_;
  uint32_t independent_preparing_ = 0;
  X_STATUS PrepareClientInternal(uint32_t callback, uint32_t callback_arg,
                                 std::optional<size_t> restored_index,
                                 uint32_t restored_wrapper,
                                 PreparedClient* prepared);
  bool CleanupClient(size_t index,
                     const std::shared_ptr<ClientRegistration>& registration);
  const uint64_t owner_epoch_;
  const std::shared_ptr<AudioDuckerPolicy> ducker_policy_;
  uint64_t next_client_generation_ = 1;
  bool shutting_down_ = false;
  uint32_t client_operations_ = 0;
  std::condition_variable client_operations_drained_;
  struct RestoredClient {
    uint32_t index;
    uint32_t callback;
    uint32_t callback_arg;
    uint32_t wrapped_callback_arg;
    std::array<uint64_t, 32> category_seen{};
  };
  std::vector<RestoredClient> restored_clients_;
};

}  // namespace apu
}  // namespace xe

#endif  // XENIA_APU_AUDIO_SYSTEM_H_
