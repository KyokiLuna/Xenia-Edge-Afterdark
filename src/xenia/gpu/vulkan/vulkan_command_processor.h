/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_GPU_VULKAN_VULKAN_COMMAND_PROCESSOR_H_
#define XENIA_GPU_VULKAN_VULKAN_COMMAND_PROCESSOR_H_

#include <algorithm>
#include <array>
#include <atomic>
#include <climits>
#include <cstdint>
#include <cstring>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "xenia/base/assert.h"
#include "xenia/base/hash.h"
#include "xenia/base/logging.h"
#include "xenia/base/memory.h"
#include "xenia/gpu/command_processor.h"
#include "xenia/gpu/draw_util.h"
#include "xenia/gpu/gpu_flags.h"
#include "xenia/gpu/registers.h"
#include "xenia/gpu/spirv_shader_translator.h"
#include "xenia/gpu/vulkan/deferred_command_buffer.h"
#include "xenia/gpu/vulkan/vulkan_graphics_system.h"
#include "xenia/gpu/vulkan/vulkan_native_resource_epoch.h"
#include "xenia/gpu/vulkan/vulkan_pipeline_cache.h"
#include "xenia/gpu/vulkan/vulkan_primitive_processor.h"
#include "xenia/gpu/vulkan/vulkan_query_pool.h"
#include "xenia/gpu/vulkan/vulkan_render_target_cache.h"
#include "xenia/gpu/vulkan/vulkan_shader.h"
#include "xenia/gpu/vulkan/vulkan_shared_memory.h"
#include "xenia/gpu/vulkan/vulkan_texture_cache.h"
#include "xenia/gpu/xenos.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/ui/vulkan/linked_type_descriptor_set_allocator.h"
#include "xenia/ui/vulkan/vulkan_descriptor_pool_chain.h"
#include "xenia/ui/vulkan/vulkan_gpu_completion_timeline.h"
#include "xenia/ui/vulkan/vulkan_presenter.h"
#include "xenia/ui/vulkan/vulkan_provider.h"
#include "xenia/ui/vulkan/vulkan_upload_buffer_pool.h"
#include "xenia/ui/vulkan/vulkan_util.h"

namespace xe {
namespace gpu {
namespace vulkan {

class VulkanCommandProcessor final : public CommandProcessor {
 protected:
#define OVERRIDING_BASE_CMDPROCESSOR
#include "../pm4_command_processor_declare.h"
#undef OVERRIDING_BASE_CMDPROCESSOR
 public:
  // Single-descriptor layouts for use within a single frame.
  enum class SingleTransientDescriptorLayout {
    kStorageBuffer,
    // Scratch buffer plus the destination image of a texture load, for the
    // compute blit that replaces vkCmdCopyBufferToImage.
    kStorageBufferAndStorageImage,
    kCount,
  };

  class ScratchBufferAcquisition {
   public:
    explicit ScratchBufferAcquisition() = default;
    explicit ScratchBufferAcquisition(VulkanCommandProcessor& command_processor,
                                      VkBuffer buffer,
                                      VkPipelineStageFlags stage_mask,
                                      VkAccessFlags access_mask)
        : command_processor_(&command_processor),
          buffer_(buffer),
          stage_mask_(stage_mask),
          access_mask_(access_mask) {}

    ScratchBufferAcquisition(const ScratchBufferAcquisition& acquisition) =
        delete;
    ScratchBufferAcquisition& operator=(
        const ScratchBufferAcquisition& acquisition) = delete;

    ScratchBufferAcquisition(ScratchBufferAcquisition&& acquisition) {
      command_processor_ = acquisition.command_processor_;
      buffer_ = acquisition.buffer_;
      stage_mask_ = acquisition.stage_mask_;
      access_mask_ = acquisition.access_mask_;
      acquisition.command_processor_ = nullptr;
      acquisition.buffer_ = VK_NULL_HANDLE;
      acquisition.stage_mask_ = 0;
      acquisition.access_mask_ = 0;
    }
    ScratchBufferAcquisition& operator=(
        ScratchBufferAcquisition&& acquisition) {
      if (this == &acquisition) {
        return *this;
      }
      command_processor_ = acquisition.command_processor_;
      buffer_ = acquisition.buffer_;
      stage_mask_ = acquisition.stage_mask_;
      access_mask_ = acquisition.access_mask_;
      acquisition.command_processor_ = nullptr;
      acquisition.buffer_ = VK_NULL_HANDLE;
      acquisition.stage_mask_ = 0;
      acquisition.access_mask_ = 0;
      return *this;
    }

    ~ScratchBufferAcquisition() {
      if (buffer_ != VK_NULL_HANDLE) {
        assert_true(command_processor_->scratch_buffer_used_);
        assert_true(command_processor_->scratch_buffer_ == buffer_);
        command_processor_->scratch_buffer_last_stage_mask_ = stage_mask_;
        command_processor_->scratch_buffer_last_access_mask_ = access_mask_;
        command_processor_->scratch_buffer_last_usage_submission_ =
            command_processor_->GetCurrentSubmission();
        command_processor_->scratch_buffer_used_ = false;
      }
    }

    // VK_NULL_HANDLE if failed to acquire or if moved.
    VkBuffer buffer() const { return buffer_; }

    VkPipelineStageFlags GetStageMask() const { return stage_mask_; }
    VkPipelineStageFlags SetStageMask(VkPipelineStageFlags new_stage_mask) {
      VkPipelineStageFlags old_stage_mask = stage_mask_;
      stage_mask_ = new_stage_mask;
      return old_stage_mask;
    }
    VkAccessFlags GetAccessMask() const { return access_mask_; }
    VkAccessFlags SetAccessMask(VkAccessFlags new_access_mask) {
      VkAccessFlags old_access_mask = access_mask_;
      access_mask_ = new_access_mask;
      return old_access_mask;
    }

   private:
    VulkanCommandProcessor* command_processor_ = nullptr;
    VkBuffer buffer_ = VK_NULL_HANDLE;
    VkPipelineStageFlags stage_mask_ = 0;
    VkAccessFlags access_mask_ = 0;
  };

  VulkanCommandProcessor(VulkanGraphicsSystem* graphics_system,
                         kernel::KernelState* kernel_state);
  ~VulkanCommandProcessor();

  void ClearCaches() override;
  void InvalidateGpuMemory() override;
  void ClearReadbackBuffers() override;

  void TracePlaybackWroteMemory(uint32_t base_ptr, uint32_t length) override;

  void InitializeShaderStorage(
      const std::filesystem::path& cache_root, uint32_t title_id, bool blocking,
      std::function<void()> completion_callback = nullptr) override;

  void RestoreEdramSnapshot(const void* snapshot) override;

  void PollCompletedSubmission() override;
  bool SupportsGpuCompletion() const override { return true; }
  GpuCompletion CaptureGpuCompletion(
      GpuCompletionResources resources = {},
      GpuCompletion::Observer observer = {}) override;
  void PollGpuCompletions() override;

  GuestRenderBankAllocationResult AllocateNativeGuestRenderBank() override;

  ui::vulkan::VulkanDevice* GetVulkanDevice() const {
    return static_cast<const ui::vulkan::VulkanProvider*>(
               graphics_system_->provider())
        ->vulkan_device();
  }

  // Returns the deferred drawing command list for the currently open
  // submission.
  DeferredCommandBuffer& deferred_command_buffer() {
    assert_true(submission_open_);
    return deferred_command_buffer_;
  }

  bool submission_open() const { return submission_open_; }
  uint64_t GetCurrentSubmission() const {
    return completion_timeline_.GetUpcomingSubmission();
  }
  uint64_t GetCompletedSubmission() const override {
    return completion_timeline_.GetCompletedSubmissionFromLastUpdate();
  }

  // Sparse binds are:
  // - In a single submission, all submitted in one vkQueueBindSparse.
  // - Sent to the queue without waiting for a semaphore.
  // Thus, multiple sparse binds between the completed and the current
  // submission, and within one submission, must not touch any overlapping
  // memory regions.
  void SparseBindBuffer(VkBuffer buffer, uint32_t bind_count,
                        const VkSparseMemoryBind* binds,
                        VkPipelineStageFlags wait_stage_mask);

  NativeSystemFrameCapacity GetNativeSystemFrameCapacity()
      const noexcept override;
  bool EnableNativeSystemTitleInput(uint64_t backend_epoch) override;
  uint64_t GetCurrentFrame() const { return frame_current_; }
  uint64_t GetCompletedFrame() const { return frame_completed_; }

  // Submission must be open to insert barriers. If no pipeline stages access
  // the resource in a synchronization scope, the stage masks should be 0 (top /
  // bottom of pipe should be specified only if explicitly needed). Returning
  // true if the barrier has actually been inserted and not dropped.
  bool PushBufferMemoryBarrier(
      VkBuffer buffer, VkDeviceSize offset, VkDeviceSize size,
      VkPipelineStageFlags src_stage_mask, VkPipelineStageFlags dst_stage_mask,
      VkAccessFlags src_access_mask, VkAccessFlags dst_access_mask,
      uint32_t src_queue_family_index = VK_QUEUE_FAMILY_IGNORED,
      uint32_t dst_queue_family_index = VK_QUEUE_FAMILY_IGNORED,
      bool skip_if_equal = true);
  // sample_locations has to stay valid until barriers are submitted.
  bool PushImageMemoryBarrier(
      VkImage image, const VkImageSubresourceRange& subresource_range,
      VkPipelineStageFlags src_stage_mask, VkPipelineStageFlags dst_stage_mask,
      VkAccessFlags src_access_mask, VkAccessFlags dst_access_mask,
      VkImageLayout old_layout, VkImageLayout new_layout,
      uint32_t src_queue_family_index = VK_QUEUE_FAMILY_IGNORED,
      uint32_t dst_queue_family_index = VK_QUEUE_FAMILY_IGNORED,
      bool skip_if_equal = true,
      const VkSampleLocationsInfoEXT* sample_locations = nullptr);
  // Returns whether any barriers have been submitted - if true is returned, the
  // render pass will also be closed.
  bool SubmitBarriers(bool force_end_render_pass);

  // If [base_bytes, base_bytes + size_bytes) holds memexport output (which
  // under two-buffer routing lives in the host-imported buffer aliasing guest
  // RAM), copies the pages holding it into the device-local buffer so a
  // following device-buffer read - a texture load - sees it, or all of the
  // range with whole_range. No-op for non-memexport ranges (already in the
  // device buffer) and when the host buffer is unavailable. Called by the
  // texture cache before loading a texture. Ends the render pass.
  // Returns whether it copied, so an upload of the same range can be skipped.
  bool EnsureMemexportRangeInDeviceBuffer(uint32_t base_bytes,
                                          uint32_t size_bytes,
                                          bool whole_range = false);

  // If not started yet, begins a render pass from the render target cache.
  // Submission must be open.
  void SubmitBarriersAndEnterRenderTargetCacheRenderPass(
      VkRenderPass render_pass,
      const VulkanRenderTargetCache::Framebuffer* framebuffer);
  // Overload for transfer operations with dynamic rendering.
  // transfer_dest_view is the destination render target view for the transfer.
  void SubmitBarriersAndEnterRenderTargetCacheRenderPass(
      VkRenderPass render_pass,
      const VulkanRenderTargetCache::Framebuffer* framebuffer,
      VkImageView transfer_dest_view, bool transfer_dest_is_depth);
  // Must be called before doing anything outside the render pass scope,
  // including adding pipeline barriers that are not a part of the render pass
  // scope. Submission must be open.
  void EndRenderPass();

  VkDescriptorSetLayout GetSingleTransientDescriptorLayout(
      SingleTransientDescriptorLayout transient_descriptor_layout) const {
    return descriptor_set_layouts_single_transient_[size_t(
        transient_descriptor_layout)];
  }
  // Queues an image, its view and its memory for destruction once the
  // submission using them now has completed. Any of them may be null.
  void DestroyScratchImageWhenIdle(VkImage image, VkImageView image_view,
                                   VkDeviceMemory memory);
  // Queues a framebuffer for destruction once the submission using it now has
  // completed.
  void DestroyFramebufferWhenIdle(VkFramebuffer framebuffer);

  // A frame must be open.
  VkDescriptorSet AllocateSingleTransientDescriptor(
      SingleTransientDescriptorLayout transient_descriptor_layout);

  // The returned reference is valid until a cache clear.
  VkDescriptorSetLayout GetTextureDescriptorSetLayout(bool is_vertex,
                                                      size_t texture_count,
                                                      size_t sampler_count);
  // The returned reference is valid until a cache clear.
  const VulkanPipelineCache::PipelineLayoutProvider* GetPipelineLayout(
      size_t texture_count_pixel, size_t sampler_count_pixel,
      size_t texture_count_vertex, size_t sampler_count_vertex);

  // Returns a single temporary GPU-side buffer within a submission for tasks
  // like texture untiling and resolving. May push a buffer memory barrier into
  // the initial usage. Submission must be open.
  ScratchBufferAcquisition AcquireScratchGpuBuffer(
      VkDeviceSize size, VkPipelineStageFlags initial_stage_mask,
      VkAccessFlags initial_access_mask);

  // Binds a graphics pipeline for host-specific purposes, invalidating the
  // affected state. keep_dynamic_* must be false (to invalidate the dynamic
  // state after binding the pipeline with the same state being static, or if
  // the caller changes the dynamic state bypassing the VulkanCommandProcessor)
  // unless the caller has these state variables as dynamic and uses the
  // tracking in VulkanCommandProcessor to modify them.
  void BindExternalGraphicsPipeline(VkPipeline pipeline,
                                    bool keep_dynamic_depth_bias = false,
                                    bool keep_dynamic_blend_constants = false,
                                    bool keep_dynamic_stencil_mask_ref = false);
  void BindExternalComputePipeline(VkPipeline pipeline);
  void SetViewport(const VkViewport& viewport);
  void SetScissor(const VkRect2D& scissor);

  // Debug marker methods - public so subsystems can annotate their operations.
  void PushDebugMarker(const char* format, ...);
  void PopDebugMarker();
  void InsertDebugMarker(const char* format, ...);
  bool debug_markers_enabled() const { return debug_markers_enabled_; }

 protected:
  bool SetupContext() override;
  void ShutdownContext() override;
  std::string MakeTitleStateSuffix() const override;
  XE_FORCEINLINE
  void WriteRegister(uint32_t index, uint32_t value) override;
  XE_FORCEINLINE
  virtual void WriteRegistersFromMem(uint32_t start_index, uint32_t* base,
                                     uint32_t num_registers) override;

  void OnGammaRamp256EntryTableValueWritten() override;
  void OnGammaRampPWLValueWritten() override;

  // For command_processor_resolve_readwatch.inc.
  bool EndResolveSubmission() {
    return !submission_open_ || EndSubmission(false);
  }
  uint64_t GetResolveSubmittedThrough() const {
    return resolve_submitted_through_.load(std::memory_order_acquire);
  }
  // Copies resolve output into guest RAM from a fault, on its own command
  // buffer and fence, as the faulting thread can't use the GPU thread's. With
  // zero-copy, only waits for the resolve to have written it.
  bool FaultCopyResolveToGuestRam(
      const std::vector<std::pair<uint32_t, uint32_t>>& ranges,
      bool strict_visibility = false,
      VulkanSharedMemory* explicit_native_source = nullptr);
  void DestroyResolveFaultCopy();
  bool EnsureResolveFaultReadback(uint32_t size);
  // The last submission handed to the GPU, for other threads.
  std::atomic<uint64_t> resolve_submitted_through_{0};
  VkCommandPool resolve_fault_command_pool_ = VK_NULL_HANDLE;
  VkCommandBuffer resolve_fault_command_buffer_ = VK_NULL_HANDLE;
  VkFence resolve_fault_fence_ = VK_NULL_HANDLE;
  std::vector<VkBufferCopy> resolve_fault_copy_regions_;

  // Staging storage for command_processor_readback_staging.inc, which owns the
  // pool itself. Used only without the guest RAM host buffer.
  struct ReadbackStagingBuffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    void* mapped = nullptr;
    uint32_t memory_type = 0;
    VkDeviceSize memory_size = 0;
  };
  bool CreateReadbackStagingBuffer(ReadbackStagingBuffer& buffer,
                                   uint32_t size);
  void DestroyReadbackStagingBuffer(ReadbackStagingBuffer& buffer);
  // Where a fault copy lands without the guest RAM host buffer. Idle between
  // fault copies, each of which is awaited.
  ReadbackStagingBuffer resolve_fault_readback_;
  uint32_t resolve_fault_readback_size_ = 0;
  void PrepareReadbackStagingEviction();
  void InvalidateReadbackStaging(const ReadbackStagingBuffer& buffer);
  // The staging pool, shared with the D3D12 backend. Included here rather than
  // with the other fragments because the declarations below name its
  // ReadbackStagingSlot.
#include "../command_processor_readback_staging.inc"
  void OrderReadbackStagingWrite(VkBuffer staging_buffer);
  void StageMemexportReadback();
  void FlushMemexportStagingReadback();
  void AwaitMemexportSubmission(uint64_t submission) {
    CheckSubmissionCompletionAndDeviceLoss(submission);
  }
  // Without the host buffer or zero-copy, export output reaches guest RAM only
  // through the staging copy, which a wait makes.
  bool CanHoldSignalsForMemexport() const {
    return shared_memory_->host_buffer() != VK_NULL_HANDLE ||
           shared_memory_->is_zero_copy();
  }
  void EndSubmissionForMemexport() { EndResolveSubmission(); }
  // Export ranges staged but not yet copied out, in record order - a later
  // copy of an overlapping range has to win.
  struct MemexportStagedRange {
    uint64_t key;
    uint32_t address;
    uint32_t length;
    // Where address lies in the slot's buffer.
    uint32_t offset;
  };
  std::vector<MemexportStagedRange> memexport_staged_;
  // For EnsureMemexportRangeInDeviceBuffer.
  std::vector<VkBufferCopy> memexport_copy_regions_;

  void IssueSwap(uint32_t frontbuffer_ptr, uint32_t frontbuffer_width,
                 uint32_t frontbuffer_height) override;

  void QueueNativeFrameAtSafeBoundary(
      std::shared_ptr<const NativeFrameExecutionRequest> request,
      std::function<void(NativeFrameExecutionResult)> completed) override;
  NativeFrameReadbackResult ReadCompletedNativeFrame(
      std::shared_ptr<const NativeFrameExecutionRequest> request) override;
  NativeFramePresentationStatus GetNativeFramePresentationStatus(
      std::shared_ptr<const NativeFrameExecutionRequest> request) override;

  void CaptureNativeSystemTitleInput(
      const NativeSystemTitleInput::Identity& identity,
      const NativeSystemTitleInput::Layout& layout) noexcept override;

  void OnPrimaryBufferEnd() override;

  Shader* LoadShader(xenos::ShaderType shader_type,
                     const uint32_t* host_address,
                     uint32_t dword_count) override;

  bool IssueDraw(xenos::PrimitiveType prim_type, uint32_t index_count,
                 IndexBufferInfo* index_buffer_info,
                 bool major_mode_explicit) override;
  bool IssueCopy() override;

  void InitializeTrace() override;
  bool DumpEdramSnapshotToFile(const std::filesystem::path& path) override;

 private:
  // Only the actual swap drain may enter this transaction. A packet plan or
  // successful legacy bool draw is never execution authority.
  NativeFrameExecutionResult ExecutePreparedNativeFrame(
      std::shared_ptr<const NativeFrameExecutionRequest> request);
  bool HasNativeFrameGpuStopRetention() const;
  // All callers serialize the shared pool/fence/staging allocation with
  // resolve_read_mutex_. A nonempty host span forces staging, including when
  // the Title buffer aliases guest RAM. Only the ordinary wrapper mirrors RAM.
  bool CopyResolveOutput(
      const std::vector<std::pair<uint32_t, uint32_t>>& ranges,
      bool strict_visibility, VulkanSharedMemory& source,
      std::span<uint8_t> host_destination = {});
  void RetainFailedNativeFrameGpuWork(
      std::shared_ptr<const NativeFrameExecutionRequest> request,
      uint64_t first_native_submission, uint64_t latest_native_submission,
      bool all_work_submitted) noexcept;
  void PumpQueuedNativeFramesAtSafePoint() override;
  void DrainNativeFrameAtSafePoint();
  void RefreshNativeFrameCapacity() noexcept;
  bool HasNativeQueryIsolation() const;
  enum class NativeQueryParkStatus { kBusy, kReady, kFailed };
  NativeQueryParkStatus ParkTitleQueries(const NativeFrameExecutionRequest&);
  bool RestoreTitleQueries(bool after_gpu_stopped = false);
  bool HasGuestResourceScope() const {
    return frame_open_ ||
           (native_guest_render_bank_selected_ &&
            native_resource_epoch_active_.valid() && HasNativeQueryIsolation());
  }
  void CancelPendingNativeFrame() noexcept;
  void StopNativeGpuBeforeDestruction();
  static void CompleteNativeFrameCallback(
      std::function<void(NativeFrameExecutionResult)> completed,
      NativeFrameExecutionResult result) noexcept;
  std::shared_ptr<const NativeFrameExecutionRequest> native_frame_pending_;
  std::function<void(NativeFrameExecutionResult)>
      native_frame_pending_completed_;
  // Assigned before entering the executor and replaced only after a real
  // Complete fence and successful restoration, or released after GPU stop.
  std::shared_ptr<const NativeFrameExecutionRequest> native_frame_retained_;
  GpuCompletion native_frame_retained_completion_;
  memory::PhysicalRange native_frame_retained_written_range_{};
  // Exact completed GPU output, captured from the helper staging allocation.
  // Guest RAM may change asynchronously after its required visibility mirror.
  std::vector<uint8_t> native_frame_retained_snapshot_output_;
  bool native_frame_retained_submitted_ = false;
  bool native_frame_retained_restored_ = false;
  bool native_frame_boundary_active_ = false;
  bool native_frame_waiting_capacity_ = false;
  static constexpr uint32_t kNativeFrameCapabilitySupported = 0x100;
  static constexpr uint32_t kNativeImmutableSnapshotSupported = 0x200;
  std::atomic<uint32_t> native_frame_capacity_word_{0};
  void SetNativeFrameCapacityState(
      NativeSystemFrameCapacity::State state) noexcept {
    native_frame_capacity_word_.store(
        (native_frame_capacity_word_.load(std::memory_order_relaxed) &
         (kNativeFrameCapabilitySupported |
          kNativeImmutableSnapshotSupported)) |
            uint32_t(state),
        std::memory_order_release);
  }
  void SignalNativeFrameFailure() noexcept;
  bool native_frame_fatal_notified_ = false;
  bool native_frame_fatal_ = false;
  bool native_gpu_work_ever_attempted_ = false;
  bool native_shutdown_retention_installed_ = false;
  bool native_shutdown_in_progress_ = false;
  // Read by fault-copy threads as well as the CP worker.
  std::atomic<bool> native_submissions_sealed_{false};
  // A direct copy submit/wait failure may leave its shared pool/fence pending.
  // Prevent all reuse until teardown without blocking final CP submission.
  std::atomic<bool> resolve_fault_copy_failed_{false};
  uint64_t native_frame_last_submission_ = 0;

  static constexpr uint32_t kMaxFramesInFlight = 3;

  struct NativePresentationImage {
    NativeFrameSubmissionIdentity identity;
    uint64_t backend_epoch = 0, version = 0;
    uint32_t width = 0, height = 0;
    std::shared_ptr<const ui::NativeUiPlaneAuthority> authority;
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;
    VkDescriptorSet copy_set = VK_NULL_HANDLE;
    uint64_t last_reference_submission = 0;
    uint64_t native_write_submission = 0;
    uint64_t composite_submission = 0;
    GpuCompletion native_completion, composite_completion;
    bool copy_recorded = false, published = false, failed = false;
    bool presenter_published = false, publication_withdrawn = false;
  };
  bool EnsureNativePresentationPipelines();
  NativePresentationImage* BeginNativePresentationImage(
      const std::shared_ptr<const NativeFrameExecutionRequest>& request,
      std::shared_ptr<const ui::NativeUiPlaneAuthority> authority);
  bool RecordNativePresentationCopy(NativePresentationImage& image,
                                    const NativeFrameExecutionRequest& request,
                                    memory::PhysicalRange written);
  void CommitNativePresentationImage(NativePresentationImage* image,
                                     const NativeFrameExecutionResult& result);
  void PublishCompletedNativePresentationImages();
  void FailNativePresentation(uint64_t image_version,
                              const char* reason) noexcept;
  void ReclaimNativePresentationImages();
  void DestroyNativePresentationImage(NativePresentationImage& image);
  void DestroyNativePresentationResources();
  std::deque<std::shared_ptr<NativePresentationImage>>
      native_presentation_images_;
  NativePresentationImage* native_presentation_current_ = nullptr;
  uint64_t native_presentation_next_version_ = 1;
  uint64_t native_presentation_published_version_ = 0;
  VkDescriptorSetLayout native_presentation_descriptor_layout_ = VK_NULL_HANDLE;
  VkPipelineLayout native_presentation_pipeline_layout_ = VK_NULL_HANDLE;
  VkPipeline native_presentation_copy_pipeline_ = VK_NULL_HANDLE;

  struct NativeDrawOutcome {
    enum class Status {
      kFailed,
      kRejected,
      kNoRasterWork,
      kNoVertices,
      kPipelineUnavailable,
      kPlaceholderPipeline,
      kTextureUnavailable,
      kRenderTargetPreparationFailed,
      kDrawRecorded,
      kIndexedDrawRecorded,
      kResolveRecorded,
    };
    Status status = Status::kFailed;
    uint32_t draw_commands = 0;
    uint32_t indexed_draw_commands = 0;
    uint32_t host_vertex_count = 0;
    VulkanRenderTargetCache::NativeResolveOutcome resolve;
    bool recorded() const {
      return status == Status::kDrawRecorded ||
             status == Status::kIndexedDrawRecorded ||
             status == Status::kResolveRecorded;
    }
  };
  NativeDrawOutcome IssueNativeDraw(
      xenos::PrimitiveType prim_type, uint32_t index_count,
      IndexBufferInfo* index_buffer_info, bool major_mode_explicit,
      const VulkanRenderTargetCache::NativeResolveExpectation* resolve =
          nullptr);
  bool IssueDrawInternal(
      xenos::PrimitiveType prim_type, uint32_t index_count,
      IndexBufferInfo* index_buffer_info, bool major_mode_explicit,
      NativeDrawOutcome* native_outcome,
      const VulkanRenderTargetCache::NativeResolveExpectation* resolve);
  bool IssueCopyInternal(
      const VulkanRenderTargetCache::NativeResolveExpectation* expectation,
      VulkanRenderTargetCache::NativeResolveOutcome* native_outcome);
  // Select only at an already closed submission in an existing title frame.
  // Refusal to leave an open/failed native submission is not a rollback; the
  // future transaction must stop/quarantine that work before host destruction.
  bool SelectNativeGuestRenderBankAtClosedSubmission(
      bool select_native, bool after_gpu_stopped = false);
  void InvalidateGuestRenderBankBindings();

  struct CommandBuffer {
    VkCommandPool pool;
    VkCommandBuffer buffer;
  };

  struct SparseBufferBind {
    VkBuffer buffer;
    size_t bind_offset;
    uint32_t bind_count;
  };

  union TextureDescriptorSetLayoutKey {
    uint32_t key;
    struct {
      // If texture and sampler counts are both 0, use
      // descriptor_set_layout_empty_ instead as these are owning references.
      uint32_t texture_count : 16;
      uint32_t sampler_count : 15;
      uint32_t is_vertex : 1;
    };

    TextureDescriptorSetLayoutKey() : key(0) {
      static_assert_size(*this, sizeof(key));
    }

    struct Hasher {
      size_t operator()(const TextureDescriptorSetLayoutKey& key) const {
        return std::hash<decltype(key.key)>{}(key.key);
      }
    };
    bool operator==(const TextureDescriptorSetLayoutKey& other_key) const {
      return key == other_key.key;
    }
    bool operator!=(const TextureDescriptorSetLayoutKey& other_key) const {
      return !(*this == other_key);
    }
  };

  union PipelineLayoutKey {
    uint64_t key;
    struct {
      // Pixel textures in the low bits since those are varied much more
      // commonly.
      uint16_t texture_count_pixel;
      uint16_t sampler_count_pixel;
      uint16_t texture_count_vertex;
      uint16_t sampler_count_vertex;
    };

    PipelineLayoutKey() : key(0) { static_assert_size(*this, sizeof(key)); }

    struct Hasher {
      size_t operator()(const PipelineLayoutKey& key) const {
        return std::hash<decltype(key.key)>{}(key.key);
      }
    };
    bool operator==(const PipelineLayoutKey& other_key) const {
      return key == other_key.key;
    }
    bool operator!=(const PipelineLayoutKey& other_key) const {
      return !(*this == other_key);
    }
  };

  class PipelineLayout : public VulkanPipelineCache::PipelineLayoutProvider {
   public:
    explicit PipelineLayout(
        VkPipelineLayout pipeline_layout,
        VkDescriptorSetLayout descriptor_set_layout_textures_vertex_ref,
        VkDescriptorSetLayout descriptor_set_layout_textures_pixel_ref)
        : pipeline_layout_(pipeline_layout),
          descriptor_set_layout_textures_vertex_ref_(
              descriptor_set_layout_textures_vertex_ref),
          descriptor_set_layout_textures_pixel_ref_(
              descriptor_set_layout_textures_pixel_ref) {}
    VkPipelineLayout GetPipelineLayout() const override {
      return pipeline_layout_;
    }
    VkDescriptorSetLayout descriptor_set_layout_textures_vertex_ref() const {
      return descriptor_set_layout_textures_vertex_ref_;
    }
    VkDescriptorSetLayout descriptor_set_layout_textures_pixel_ref() const {
      return descriptor_set_layout_textures_pixel_ref_;
    }

   private:
    VkPipelineLayout pipeline_layout_;
    VkDescriptorSetLayout descriptor_set_layout_textures_vertex_ref_;
    VkDescriptorSetLayout descriptor_set_layout_textures_pixel_ref_;
  };

  struct UsedSingleTransientDescriptor {
    uint64_t frame;
    SingleTransientDescriptorLayout layout;
    VkDescriptorSet set;
  };

  struct UsedTextureTransientDescriptorSet {
    uint64_t frame;
    TextureDescriptorSetLayoutKey layout;
    VkDescriptorSet set;
  };

  enum SwapApplyGammaDescriptorSet : uint32_t {
    kSwapApplyGammaDescriptorSetRamp,
    kSwapApplyGammaDescriptorSetSource,
    kSwapApplyGammaDescriptorSetDest,

    kSwapApplyGammaDescriptorSetCount,
  };

  // BeginSubmission and EndSubmission may be called at any time. If there's an
  // open non-frame submission, BeginSubmission(true) will promote it to a
  // frame. EndSubmission(true) will close the frame no matter whether the
  // submission has already been closed.
  // Unlike on Direct3D 12, submission boundaries do not imply any memory
  // barriers aside from an incoming host write (but not outgoing host read)
  // dependency.

  // Rechecks submission number and reclaims per-submission resources. Pass 0 as
  // the submission to await to simply check status, or pass
  // GetCurrentSubmission() to wait for all queue operations to be completed.
  void CheckSubmissionCompletionAndDeviceLoss(uint64_t await_submission);
  // If is_guest_command is true, a new full frame - with full cleanup of
  // resources and, if needed, starting capturing - is opened if pending (as
  // opposed to simply resuming after mid-frame synchronization). Returns
  // whether a submission is open currently and the device is not lost.
  bool BeginSubmission(bool is_guest_command);
  // If is_swap is true, a full frame is closed - with, if needed, cache
  // clearing and stopping capturing. Returns whether the submission was done
  // successfully, if it has failed, leaves it open.
  bool EndSubmission(bool is_swap);
  bool AwaitAllQueueOperationsCompletion() {
    CheckSubmissionCompletionAndDeviceLoss(GetCurrentSubmission());
    return !submission_open_ &&
           GetCompletedSubmission() + 1u >= GetCurrentSubmission();
  }

  void ClearTransientDescriptorPools();

  void SplitPendingBarrier();

  void DestroyScratchBuffer();

  // Query segments backend for both ZPD occlusion query reports and
  // VIZ visibility surveys & conditional rendering.
  //
  // vkCmdBeginQuery is only valid inside a render pass, so segments split at
  // pass end and resume at the next pass begin. If a report or survey starts
  // outside a pass, the segment waits for the next. FSI queries clear a
  // dedicated counter with vkCmdFillBuffer, so they may need to open before a
  // pass begins or split an active pass around the clear.
  void EnsureQueryResources() override;
  void ShutdownQueryResources() override {
    query_resolves_in_flight_.clear();
    active_query_index_ = UINT32_MAX;
    active_query_generation_ = 0;
    active_query_is_fsi_ = false;
    zpd_fsi_path_ = false;
    zpd_counter_index_force_update_ = true;
    if (host_query_pool_) {
      host_query_pool_->Shutdown();
    }
  }

  bool IsQueryPoolReady() const override;
  bool CanOpenQuery() const override;

  QueryOpenResult OpenQuery(bool can_close_submission) override;
  bool CloseQuery(ReportHandle report_handle, const VIZQueryHandle& viz,
                  uint64_t& out_submission) override;
  void PumpQueryResolves() override;
  bool AwaitQueryResolve(ReportHandle report_handle,
                         uint64_t wait_for_submission) override;

  bool EnsureVIZPredicateBuffer();
  // Drains the queued predicate copies.
  void RecordVIZPredicateCopies();
  // Drains the queued copies if the draw's predicate is among them.
  VkBuffer BindVIZPredicate(VkDeviceSize& offset_out);
  void AwaitVIZQueryResolve(uint64_t wait_for_submission) override;
  void ShutdownVIZQueryResources() {
    viz_pending_copies_.clear();
    const ui::vulkan::VulkanDevice* vulkan_device = GetVulkanDevice();
    const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
    const VkDevice device = vulkan_device->device();
    ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyBuffer, device,
                                           viz_predicate_buffer_);
    ui::vulkan::util::DestroyAndNullHandle(dfn.vkFreeMemory, device,
                                           viz_predicate_buffer_memory_);
    viz_predicate_buffer_failed_ = false;
  }

  void UpdateDynamicState(const draw_util::ViewportInfo& viewport_info,
                          bool primitive_polygonal,
                          reg::RB_DEPTHCONTROL normalized_depth_control,
                          uint32_t draw_resolution_scale_x,
                          uint32_t draw_resolution_scale_y,
                          bool depth_bias_in_pixel_shader,
                          bool window_offset_in_edram);
  void UpdateSystemConstantValues(
      bool primitive_polygonal,
      const PrimitiveProcessor::ProcessingResult& primitive_processing_result,
      bool shader_32bit_index_dma, const draw_util::ViewportInfo& viewport_info,
      uint32_t used_texture_mask, reg::RB_DEPTHCONTROL normalized_depth_control,
      uint32_t normalized_color_mask,
      const draw_util::HostDepthPolygonOffset* host_depth_polygon_offset,
      int32_t window_offset_tiles);
  bool UpdateBindings(const VulkanShader* vertex_shader,
                      const VulkanShader* pixel_shader,
                      bool interpreter_placeholder,
                      bool placeholder_pixel_shader);
  // Allocates a descriptor set and fills one or two VkWriteDescriptorSet
  // structure instances (for images and samplers).
  // The descriptor set layout must be the one for the given is_vertex,
  // texture_count, sampler_count (from GetTextureDescriptorSetLayout - may be
  // already available at the moment of the call, no need to locate it again).
  // Returns how many VkWriteDescriptorSet structure instances have been
  // written, or 0 if there was a failure to allocate the descriptor set or no
  // bindings were requested.
  uint32_t WriteTransientTextureBindings(
      bool is_vertex, uint32_t texture_count, uint32_t sampler_count,
      VkDescriptorSetLayout descriptor_set_layout,
      const VkDescriptorImageInfo* texture_image_info,
      const VkDescriptorImageInfo* sampler_image_info,
      VkWriteDescriptorSet* descriptor_set_writes_out);

  bool device_lost_ = false;

  // Rolling per-submission summary for diagnosing VK_ERROR_DEVICE_LOST.
  // Accumulated into submission_in_progress_ during a submission, snapshotted
  // into submission_history_ on successful submit, dumped on device-loss.
  struct SubmissionSummary {
    uint64_t submission_index = 0;
    uint64_t frame_index = 0;
    uint32_t draw_count = 0;
    uint32_t dispatch_count = 0;
    uint32_t resolve_count = 0;
    uint64_t last_vs_hash = 0;
    uint64_t last_ps_hash = 0;
    uint64_t last_render_pass_key = 0;
  };
  static constexpr size_t kSubmissionHistorySize = 16;
  std::array<SubmissionSummary, kSubmissionHistorySize> submission_history_{};
  size_t submission_history_next_ = 0;
  SubmissionSummary submission_in_progress_{};

  void LogRecentSubmissions(const char* context);

  bool cache_clear_requested_ = false;

  // Host shader types that guest shaders can be translated into - they can
  // access the shared memory (via vertex fetch, memory export, or manual index
  // buffer reading) and textures.
  VkPipelineStageFlags guest_shader_pipeline_stages_ = 0;
  VkShaderStageFlags guest_shader_vertex_stages_ = 0;

  std::vector<VkSemaphore> semaphores_free_;

  struct PendingQueryResolve {
    uint64_t submission = 0;
    uint32_t query_index = UINT32_MAX;
    uint32_t query_generation = 0;
    uint32_t scale_area = 1;
    bool fsi = false;
    bool hybrid = false;
    ReportHandle report_handle = kInvalidReportHandle;
    VIZQueryHandle viz;
  };
  uint32_t active_query_index_ = UINT32_MAX;
  uint32_t active_query_generation_ = 0;
  bool active_query_is_fsi_ = false;
  bool zpd_fsi_path_ = false;
  bool zpd_hybrid_supported_ = false;
  bool zpd_counter_index_force_update_ = true;
  std::deque<PendingQueryResolve> query_resolves_in_flight_;
  // Fallback buffer for ZPD counter descriptor binding 1.
  VkBuffer zpd_counter_sink_buffer_ = VK_NULL_HANDLE;
  VkDeviceMemory zpd_counter_sink_buffer_memory_ = VK_NULL_HANDLE;
  // Currently installed binding 1 buffer.
  VkBuffer zpd_counter_descriptor_buffer_ = VK_NULL_HANDLE;
  VkDeviceSize zpd_counter_descriptor_range_ = 0;

  ui::vulkan::VulkanGPUCompletionTimeline completion_timeline_;
  GpuCompletionQueue gpu_completions_;
  bool submission_open_ = false;
  // In case vkQueueSubmit fails after something like a successful
  // vkQueueBindSparse, to wait correctly on the next attempt.
  std::vector<VkSemaphore> current_submission_wait_semaphores_;
  std::vector<VkPipelineStageFlags> current_submission_wait_stage_masks_;
  std::deque<std::pair<uint64_t, VkSemaphore>>
      submissions_in_flight_semaphores_;

  bool frame_open_ = false;
  // Comparable against the Metal backend's render encoder count: on a TBDR GPU
  // each one is a tile store plus an attachment reload.
  uint64_t render_passes_total_ = 0;
  uint64_t render_passes_window_start_ = 0;
  uint32_t render_pass_window_frames_ = 0;

  // A timestamp pair per in-flight submission, so the summed GPU busy time is
  // comparable against the Metal backend's completion-handler figure.
  static constexpr uint32_t kGpuTimeQuerySubmissions = 8;
  VkQueryPool gpu_time_query_pool_ = VK_NULL_HANDLE;
  uint64_t gpu_time_query_submissions_[kGpuTimeQuerySubmissions] = {};
  uint32_t gpu_time_query_slot_ = 0;
  uint64_t gpu_time_total_ns_ = 0;
  // Submissions that found every query slot busy, so their GPU time is missing
  // from the total.
  uint64_t gpu_time_untimed_submissions_ = 0;
  // Window start for the submission count published beside the busy time,
  // which sums per-submission intervals and misleads without it.
  uint64_t gpu_time_submissions_window_start_ = 0;
  uint64_t gpu_time_window_start_ns_ = 0;
  void CreateGpuTimeQueryPool();
  void DestroyGpuTimeQueryPool();
  void CollectCompletedGpuTimeQueries();
  // Guest frame index, since some transient resources can be reused across
  // submissions. Values updated in the beginning of a frame.
  uint64_t frame_current_ = 1;
  uint64_t frame_completed_ = 0;
  // Submission indices of frames that have already been submitted.
  uint64_t closed_frame_submissions_[kMaxFramesInFlight] = {};

  // <Submission where last used, resource>, sorted by the submission number.
  std::deque<std::pair<uint64_t, VkDeviceMemory>> destroy_memory_;
  std::deque<std::pair<uint64_t, VkBuffer>> destroy_buffers_;
  std::deque<std::pair<uint64_t, VkImage>> destroy_images_;
  std::deque<std::pair<uint64_t, VkImageView>> destroy_image_views_;
  std::deque<std::pair<uint64_t, VkFramebuffer>> destroy_framebuffers_;

  std::vector<CommandBuffer> command_buffers_writable_;
  std::deque<std::pair<uint64_t, CommandBuffer>> command_buffers_submitted_;
  DeferredCommandBuffer deferred_command_buffer_;

  std::vector<VkSparseMemoryBind> sparse_memory_binds_;
  std::vector<SparseBufferBind> sparse_buffer_binds_;
  // SparseBufferBind converted to VkSparseBufferMemoryBindInfo to this buffer
  // on submission (because pBinds should point to a place in std::vector, but
  // it may be reallocated).
  std::vector<VkSparseBufferMemoryBindInfo> sparse_buffer_bind_infos_temp_;
  VkPipelineStageFlags sparse_bind_wait_stage_mask_ = 0;

  // Temporary storage with reusable memory for creating descriptor set layouts.
  std::vector<VkDescriptorSetLayoutBinding> descriptor_set_layout_bindings_;
  // Temporary storage with reusable memory for writing image and sampler
  // descriptors.
  std::vector<VkDescriptorImageInfo> descriptor_write_image_info_;

  std::unique_ptr<ui::vulkan::VulkanUploadBufferPool> uniform_buffer_pool_;

  // Descriptor set layouts used by different shaders.
  VkDescriptorSetLayout descriptor_set_layout_empty_ = VK_NULL_HANDLE;
  VkDescriptorSetLayout descriptor_set_layout_constants_ = VK_NULL_HANDLE;
  std::array<VkDescriptorSetLayout,
             size_t(SingleTransientDescriptorLayout::kCount)>
      descriptor_set_layouts_single_transient_{};
  VkDescriptorSetLayout descriptor_set_layout_shared_memory_and_edram_ =
      VK_NULL_HANDLE;

  // Guards the layout maps below - GetPipelineLayout is called both from the
  // draw thread and from pipeline creation threads (for deferred translation).
  std::mutex pipeline_layouts_mutex_;
  // Descriptor set layouts are referenced by pipeline_layouts_.
  std::unordered_map<TextureDescriptorSetLayoutKey, VkDescriptorSetLayout,
                     TextureDescriptorSetLayoutKey::Hasher>
      descriptor_set_layouts_textures_;
  // Pipeline layouts are referenced by VulkanPipelineCache.
  std::unordered_map<PipelineLayoutKey, PipelineLayout,
                     PipelineLayoutKey::Hasher>
      pipeline_layouts_;

  // No specific reason for 32768, just the "too much" descriptor count from
  // Direct3D 12 PIX warnings.
  static constexpr uint32_t kLinkedTypeDescriptorPoolSetCount = 32768;
  static const VkDescriptorPoolSize kDescriptorPoolSizeUniformBufferDynamic;
  static const VkDescriptorPoolSize kDescriptorPoolSizeStorageBuffer;
  static const VkDescriptorPoolSize kDescriptorPoolSizeStorageBufferAndImage[2];
  static const VkDescriptorPoolSize kDescriptorPoolSizeTextures[2];
  ui::vulkan::LinkedTypeDescriptorSetAllocator
      transient_descriptor_allocator_uniform_buffer_;
  ui::vulkan::LinkedTypeDescriptorSetAllocator
      transient_descriptor_allocator_storage_buffer_;
  ui::vulkan::LinkedTypeDescriptorSetAllocator
      transient_descriptor_allocator_storage_buffer_and_image_;
  std::deque<UsedSingleTransientDescriptor> single_transient_descriptors_used_;
  std::array<std::vector<VkDescriptorSet>,
             size_t(SingleTransientDescriptorLayout::kCount)>
      single_transient_descriptors_free_;
  // <Usage frame, set>.
  std::deque<std::pair<uint64_t, VkDescriptorSet>>
      constants_transient_descriptors_used_;
  std::vector<VkDescriptorSet> constants_transient_descriptors_free_;

  ui::vulkan::LinkedTypeDescriptorSetAllocator
      transient_descriptor_allocator_textures_;
  std::deque<UsedTextureTransientDescriptorSet>
      texture_transient_descriptor_sets_used_;
  std::unordered_map<TextureDescriptorSetLayoutKey,
                     std::vector<VkDescriptorSet>,
                     TextureDescriptorSetLayoutKey::Hasher>
      texture_transient_descriptor_sets_free_;

  std::unique_ptr<VulkanSharedMemory> shared_memory_;
  // Fixed object identity for foreign-thread Title faults across bank swaps.
  VulkanSharedMemory* title_shared_memory_ = nullptr;

  std::unique_ptr<VulkanPrimitiveProcessor> primitive_processor_;

  std::unique_ptr<VulkanRenderTargetCache> render_target_cache_;

  // Separate native allocation namespace. A serial below is not a title frame,
  // source identity, GPU submission, or execution permission. Only an actual
  // CP completion in the captured backend epoch advances its retirement.
  struct NativeTransientResources {
    explicit NativeTransientResources(ui::vulkan::VulkanDevice& device);
    void Reclaim(uint64_t completed_epoch);

    std::unique_ptr<ui::vulkan::VulkanUploadBufferPool> uniform_buffer_pool;
    ui::vulkan::LinkedTypeDescriptorSetAllocator uniform_descriptors;
    ui::vulkan::LinkedTypeDescriptorSetAllocator storage_descriptors;
    ui::vulkan::LinkedTypeDescriptorSetAllocator storage_image_descriptors;
    ui::vulkan::LinkedTypeDescriptorSetAllocator texture_descriptors;
    std::deque<UsedSingleTransientDescriptor> singles_used;
    std::array<std::vector<VkDescriptorSet>,
               size_t(SingleTransientDescriptorLayout::kCount)>
        singles_free;
    std::deque<std::pair<uint64_t, VkDescriptorSet>> constants_used;
    std::vector<VkDescriptorSet> constants_free;
    std::deque<UsedTextureTransientDescriptorSet> textures_used;
    std::unordered_map<TextureDescriptorSetLayoutKey,
                       std::vector<VkDescriptorSet>,
                       TextureDescriptorSetLayoutKey::Hasher>
        textures_free;
  };
  enum class NativeResourceEpochBeginStatus { kRejected, kBusy, kReady };
  struct NativeResourceEpochBeginResult {
    NativeResourceEpochBeginStatus status =
        NativeResourceEpochBeginStatus::kRejected;
    VulkanNativeResourceEpoch epoch;
  };
  enum class NativeResourceEpochClose {
    kCanceledBeforeGpu,
    kSubmitted,
    kUncertain
  };
  struct GuestRenderBank;
  GuestRenderBankAllocationResult AllocateNativeGuestRenderBankForInput(
      bool immutable_snapshot);
  struct NativeResourceEpochRetirement {
    enum class State { kUnused, kReserved, kSubmitted, kCanceled, kPoisoned };
    VulkanNativeResourceEpoch epoch;
    GuestRenderBank* bank = nullptr;
    NativeFrameSubmissionIdentity source;
    uint64_t last_submission = 0;
    State state = State::kUnused;
  };
  // Reserve only from the admitted executor, after the title prefix is closed.
  // A busy result consumes no source and records no native GPU commands.
  NativeResourceEpochBeginResult BeginNativeResourceEpoch(
      const NativeFrameExecutionRequest& request);
  bool CloseNativeResourceEpoch(VulkanNativeResourceEpoch epoch,
                                NativeResourceEpochClose outcome,
                                uint64_t first_submission,
                                uint64_t last_submission) noexcept;
  void ReclaimNativeResourceEpochs();
  uint64_t GetCurrentTransientResourceEpoch() const;
  ui::vulkan::VulkanUploadBufferPool& CurrentUniformBufferPool();
  // Fixed storage: submission/failure finalization never allocates a record.
  static constexpr size_t kMaxNativeResourceEpochsInFlight = 3;
  std::array<NativeResourceEpochRetirement, kMaxNativeResourceEpochsInFlight>
      native_resource_epochs_{};
  size_t native_resource_epoch_head_ = 0;
  size_t native_resource_epoch_count_ = 0;
  uint64_t native_resource_epoch_serial_ = 0;
  uint64_t native_resource_epoch_completed_ = 0;
  VulkanNativeResourceEpoch native_resource_epoch_active_;
  bool native_resource_epochs_poisoned_ = false;

  // Never active in the allocation gate. Vulkan owners stay with the backend,
  // and are destroyed before the common descriptor / pipeline layouts.
  struct GuestRenderBank {
    explicit GuestRenderBank(ui::vulkan::VulkanDevice& device)
        : device(device) {}
    ~GuestRenderBank();

    ui::vulkan::VulkanDevice& device;
    // Never published to GraphicsSystem's MMIO view or selected by the CP in
    // this allocation gate. Constructor reset values are not a native draw
    // baseline. All bank consumers keep this private object's fixed address.
    bool immutable_snapshot = false;
    // Present only for production snapshot banks. Diagnostic bank keeps its
    // historical live-owner SharedMemory references and routing unchanged.
    std::unique_ptr<VulkanSharedMemory> shared_memory;
    std::unique_ptr<RegisterFile> register_file;
    uint64_t initial_register_hash = 0;
    std::unique_ptr<VulkanRenderTargetCache> render_target_cache;
    std::unique_ptr<VulkanPipelineCache> pipeline_cache;
    std::unique_ptr<VulkanPrimitiveProcessor> primitive_processor;
    std::unique_ptr<VulkanTextureCache> texture_cache;
    std::unique_ptr<NativeTransientResources> transient_resources;
    VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;
    VkDescriptorSet descriptor_set = VK_NULL_HANDLE;
    VkDescriptorSet host_descriptor_set = VK_NULL_HANDLE;
    // Valid placeholder only; no query or VIZ execution is admitted.
    VkBuffer zpd_counter_sink_buffer = VK_NULL_HANDLE;
    VkDeviceMemory zpd_counter_sink_memory = VK_NULL_HANDLE;
    // CP-derived values are banked, while validity/bound bits are invalidated
    // on both transitions. Never replay these as guest register writes.
    SpirvShaderTranslator::SystemConstants system_constants{};
    draw_util::GetViewportInfoArgs viewport_info_args{};
    draw_util::ViewportInfo viewport_info{};
    uint32_t stencil_compare_mask_front = UINT8_MAX;
    uint32_t stencil_compare_mask_back = UINT8_MAX;
    uint32_t stencil_write_mask_front = UINT8_MAX;
    uint32_t stencil_write_mask_back = UINT8_MAX;
    uint32_t stencil_reference_front = 0;
    uint32_t stencil_reference_back = 0;
    uint64_t last_frame = UINT64_MAX;
  };
  std::unique_ptr<GuestRenderBank> native_guest_render_bank_;
  std::unique_ptr<GuestRenderBank> native_alternate_guest_render_bank_;
  bool native_guest_render_bank_selected_ = false;
  struct NativeSavedTitleState {
    RegisterFile* register_file = nullptr;
    VulkanSharedMemory* shared_memory = nullptr;
    Shader* vertex_shader = nullptr;
    Shader* pixel_shader = nullptr;
    uint32_t vertex_shader_ucode_address = 0;
  } native_saved_title_state_;

  std::unique_ptr<VulkanQueryPool> host_query_pool_;

  VkBuffer viz_predicate_buffer_ = VK_NULL_HANDLE;
  VkDeviceMemory viz_predicate_buffer_memory_ = VK_NULL_HANDLE;
  bool viz_predicate_buffer_failed_ = false;
  struct PendingVIZCopy {
    uint32_t id;
    uint32_t query_index;
    bool fsi;
  };
  std::vector<PendingVIZCopy> viz_pending_copies_;
  struct ParkedTitleQueryContext {
    NativeTitleQueryLogicalState logical;
    NativeFrameSubmissionIdentity source;
    uint64_t serial = 0, backend_epoch = 0, prefix_submission = 0;
    bool isolated = false;
    RegisterFile* title_register_file = nullptr;
    std::unique_ptr<VulkanQueryPool> pool;
    std::deque<PendingQueryResolve> resolves;
    std::vector<PendingVIZCopy> viz_copies;
    VkBuffer predicate_buffer = VK_NULL_HANDLE;
    VkDeviceMemory predicate_memory = VK_NULL_HANDLE;
    bool predicate_failed = false;
    bool fsi_path = false, counter_dirty = true;
    VkBuffer counter_descriptor_buffer = VK_NULL_HANDLE;
    VkDeviceSize counter_descriptor_range = 0;
  };
  // GPU-owning parked context lives with the CP, never with a stack token or
  // GpuCompletionResources. Failure retains it through actual backend stop.
  std::unique_ptr<ParkedTitleQueryContext> native_parked_title_queries_;
  uint64_t native_query_scope_serial_ = 0;

  std::unique_ptr<VulkanPipelineCache> pipeline_cache_;

  std::unique_ptr<VulkanTextureCache> texture_cache_;

  VkDescriptorPool shared_memory_and_edram_descriptor_pool_ = VK_NULL_HANDLE;
  VkDescriptorSet shared_memory_and_edram_descriptor_set_;
  // Second set identical to the one above except binding 0 points at the
  // host-imported shared-memory buffer (aliasing guest RAM). Bound instead of
  // the device set for memexport-touching draws so their GPU-written geometry
  // stays coherent with the CPU. VK_NULL_HANDLE when the host buffer is
  // unavailable - routing is then disabled and every draw uses the device set.
  VkDescriptorSet shared_memory_host_and_edram_descriptor_set_ = VK_NULL_HANDLE;
  // Two-buffer memexport page tracking (data + methods), shared with the D3D12
  // backend as a class-body fragment so each backend gets its own non-virtual,
  // inlinable copy. Requires <array> and SharedMemory, both included above.
#include "../command_processor_memexport.inc"

  // Bytes 0x0...0x3FF - 256-entry gamma ramp table with B10G10R10X2 data (read
  // as R10G10B10X2 with swizzle).
  // Bytes 0x400...0x9FF - 128-entry PWL R16G16 gamma ramp (R - base, G - delta,
  // low 6 bits of each are zero, 3 elements per entry).
  // kMaxFramesInFlight pairs of gamma ramps if in host-visible memory and
  // uploaded directly, one otherwise.
  VkDeviceMemory gamma_ramp_buffer_memory_ = VK_NULL_HANDLE;
  VkBuffer gamma_ramp_buffer_ = VK_NULL_HANDLE;
  // kMaxFramesInFlight pairs, only when the gamma ramp buffer is not
  // host-visible.
  VkDeviceMemory gamma_ramp_upload_buffer_memory_ = VK_NULL_HANDLE;
  VkBuffer gamma_ramp_upload_buffer_ = VK_NULL_HANDLE;
  VkDeviceSize gamma_ramp_upload_memory_size_;
  uint32_t gamma_ramp_upload_memory_type_;
  // Mapping of either gamma_ramp_buffer_memory_ (if it's host-visible) or
  // gamma_ramp_upload_buffer_memory_ (otherwise).
  void* gamma_ramp_upload_mapping_;
  std::array<VkBufferView, 2 * kMaxFramesInFlight> gamma_ramp_buffer_views_{};
  // UINT32_MAX if outdated.
  uint32_t gamma_ramp_256_entry_table_current_frame_ = UINT32_MAX;
  uint32_t gamma_ramp_pwl_current_frame_ = UINT32_MAX;

  VkDescriptorSetLayout swap_descriptor_set_layout_sampled_image_ =
      VK_NULL_HANDLE;
  VkDescriptorSetLayout swap_descriptor_set_layout_uniform_texel_buffer_ =
      VK_NULL_HANDLE;
  // Storage image descriptor set layout for compute shader destination.
  VkDescriptorSetLayout swap_descriptor_set_layout_storage_image_ =
      VK_NULL_HANDLE;

  // Descriptor pool for allocating descriptors needed for presentation, such as
  // the destination images and the gamma ramps.
  VkDescriptorPool swap_descriptor_pool_ = VK_NULL_HANDLE;
  // Interleaved 256-entry table and PWL texel buffer descriptors.
  // kMaxFramesInFlight pairs of gamma ramps if in host-visible memory and
  // uploaded directly, one otherwise.
  std::array<VkDescriptorSet, 2 * kMaxFramesInFlight>
      swap_descriptors_gamma_ramp_;
  // Sampled images for source.
  std::array<VkDescriptorSet, kMaxFramesInFlight> swap_descriptors_source_;
  // Storage images for destination.
  std::array<VkDescriptorSet, kMaxFramesInFlight> swap_descriptors_dest_;

  // Gamma ramp application compute pipeline constants.
  struct ApplyGammaConstants {
    uint32_t size[2];
  };
  VkPipelineLayout swap_apply_gamma_pipeline_layout_ = VK_NULL_HANDLE;
  VkPipeline swap_apply_gamma_256_entry_table_pipeline_ = VK_NULL_HANDLE;
  VkPipeline swap_apply_gamma_pwl_pipeline_ = VK_NULL_HANDLE;
  // Gamma pipelines that also compute FXAA luma in the alpha channel.
  VkPipeline swap_apply_gamma_256_entry_table_fxaa_luma_pipeline_ =
      VK_NULL_HANDLE;
  VkPipeline swap_apply_gamma_pwl_fxaa_luma_pipeline_ = VK_NULL_HANDLE;

  // FXAA post-processing.
  struct FxaaConstants {
    uint32_t size[2];
    float size_inv[2];
  };
  // Descriptor set layout for FXAA source (combined image sampler).
  VkDescriptorSetLayout fxaa_source_descriptor_set_layout_ = VK_NULL_HANDLE;
  VkPipelineLayout fxaa_pipeline_layout_ = VK_NULL_HANDLE;
  VkPipeline fxaa_pipeline_ = VK_NULL_HANDLE;
  VkPipeline fxaa_extreme_pipeline_ = VK_NULL_HANDLE;
  VkSampler fxaa_sampler_ = VK_NULL_HANDLE;

  // Resolve downscale compute shader for scaled resolution readback.
  // Downscales scaled resolve buffer data back to 1x resolution on the GPU,
  // avoiding expensive CPU-side downscaling and reducing transfer bandwidth.
  VkDescriptorSetLayout resolve_downscale_descriptor_set_layout_ =
      VK_NULL_HANDLE;
  VkPipelineLayout resolve_downscale_pipeline_layout_ = VK_NULL_HANDLE;
  VkPipeline resolve_downscale_pipeline_ = VK_NULL_HANDLE;
  // Descriptor pool chain for resolve downscale shader (2 storage buffers per
  // set). Uses pool chain to avoid mid-frame GPU stalls on pool exhaustion.
  std::unique_ptr<ui::vulkan::VulkanDescriptorPoolChain>
      resolve_downscale_descriptor_pool_chain_;
  // Intermediate buffer for downscaled output (device local, for compute).
  VkBuffer resolve_downscale_buffer_ = VK_NULL_HANDLE;
  VkDeviceMemory resolve_downscale_buffer_memory_ = VK_NULL_HANDLE;
  uint32_t resolve_downscale_buffer_size_ = 0;

  // FXAA source texture - R16G16B16A16_SFLOAT for luma in alpha.
  static constexpr VkFormat kFxaaSourceFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
  VkDeviceMemory fxaa_source_memory_ = VK_NULL_HANDLE;
  VkImage fxaa_source_image_ = VK_NULL_HANDLE;
  VkImageView fxaa_source_image_view_ = VK_NULL_HANDLE;
  uint32_t fxaa_source_width_ = 0;
  uint32_t fxaa_source_height_ = 0;
  uint64_t fxaa_source_last_submission_ = 0;
  // Descriptor sets for FXAA - source as combined image sampler (for FXAA
  // read).
  std::array<VkDescriptorSet, kMaxFramesInFlight> fxaa_source_descriptors_;
  // Descriptor sets for FXAA - source as storage image (for gamma+luma write).
  std::array<VkDescriptorSet, kMaxFramesInFlight>
      fxaa_source_storage_descriptors_;

  // Pending pipeline barriers.
  std::vector<VkBufferMemoryBarrier> pending_barriers_buffer_memory_barriers_;
  std::vector<VkImageMemoryBarrier> pending_barriers_image_memory_barriers_;
  struct PendingBarrier {
    VkPipelineStageFlags src_stage_mask = 0;
    VkPipelineStageFlags dst_stage_mask = 0;
    size_t buffer_memory_barriers_offset = 0;
    size_t image_memory_barriers_offset = 0;
  };
  std::vector<PendingBarrier> pending_barriers_;
  PendingBarrier current_pending_barrier_;

  // GPU-local scratch buffer.
  static constexpr VkDeviceSize kScratchBufferSizeIncrement = 16 * 1024 * 1024;
  VkDeviceMemory scratch_buffer_memory_ = VK_NULL_HANDLE;
  VkBuffer scratch_buffer_ = VK_NULL_HANDLE;
  VkDeviceSize scratch_buffer_size_ = 0;
  VkPipelineStageFlags scratch_buffer_last_stage_mask_ = 0;
  VkAccessFlags scratch_buffer_last_access_mask_ = 0;
  uint64_t scratch_buffer_last_usage_submission_ = 0;
  bool scratch_buffer_used_ = false;

  // The current dynamic state of the graphics pipeline bind point. Note that
  // binding any pipeline to the bind point with static state (even if it's
  // unused, like depth bias being disabled, but the values themselves still not
  // declared as dynamic in the pipeline) invalidates such dynamic state.
  VkViewport dynamic_viewport_;
  VkRect2D dynamic_scissor_;
  // Dynamic fixed-function depth bias, blend constants, stencil state are
  // applicable only to the render target implementations where they are
  // actually involved.
  float dynamic_depth_bias_constant_factor_;
  float dynamic_depth_bias_slope_factor_;
  float dynamic_blend_constants_[4];
  // The stencil values are pre-initialized (to D3D11_DEFAULT_STENCIL_*, and the
  // initial values for front and back are the same for portability subset
  // safety) because they're updated conditionally to avoid changing the back
  // face values when stencil is disabled and the primitive type is changed
  // between polygonal and non-polygonal.
  uint32_t dynamic_stencil_compare_mask_front_ = UINT8_MAX;
  uint32_t dynamic_stencil_compare_mask_back_ = UINT8_MAX;
  uint32_t dynamic_stencil_write_mask_front_ = UINT8_MAX;
  uint32_t dynamic_stencil_write_mask_back_ = UINT8_MAX;
  uint32_t dynamic_stencil_reference_front_ = 0;
  uint32_t dynamic_stencil_reference_back_ = 0;
  bool dynamic_viewport_update_needed_;
  bool dynamic_scissor_update_needed_;
  bool dynamic_depth_bias_update_needed_;
  bool dynamic_blend_constants_update_needed_;
  bool dynamic_stencil_compare_mask_front_update_needed_;
  bool dynamic_stencil_compare_mask_back_update_needed_;
  bool dynamic_stencil_write_mask_front_update_needed_;
  bool dynamic_stencil_write_mask_back_update_needed_;
  bool dynamic_stencil_reference_front_update_needed_;
  bool dynamic_stencil_reference_back_update_needed_;

  // Currently used samplers.
  std::vector<std::pair<VulkanTextureCache::SamplerParameters, VkSampler>>
      current_samplers_vertex_;
  std::vector<std::pair<VulkanTextureCache::SamplerParameters, VkSampler>>
      current_samplers_pixel_;

  // Cache render pass currently started in the command buffer with the
  // framebuffer. For dynamic rendering, current_render_pass_ is VK_NULL_HANDLE
  // but in_render_pass_ is true.
  VkRenderPass current_render_pass_;
  const VulkanRenderTargetCache::Framebuffer* current_framebuffer_;
  // True when inside a render pass or dynamic rendering block.
  bool in_render_pass_ = false;

  // Currently bound graphics pipeline, either from the pipeline cache (with
  // potentially deferred creation - current_external_graphics_pipeline_ is
  // VK_NULL_HANDLE in this case) or a non-Xenos one
  // (current_guest_graphics_pipeline_ is VK_NULL_HANDLE in this case).
  // TODO(Triang3l): Change to a deferred compilation handle.
  VkPipeline current_guest_graphics_pipeline_;
  VkPipeline current_external_graphics_pipeline_;
  VkPipeline current_external_compute_pipeline_;

  // Pipeline layout of the current guest graphics pipeline.
  const PipelineLayout* current_guest_graphics_pipeline_layout_;
  // The bindings are VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, so the infos
  // describe only the pool page and the size - the offset within the page is
  // passed when binding and the info offset stays 0.
  VkDescriptorBufferInfo current_constant_buffer_infos_
      [SpirvShaderTranslator::kConstantBufferCount]{};
  uint32_t current_constant_buffer_dynamic_offsets_
      [SpirvShaderTranslator::kConstantBufferCount]{};
  // Whether up-to-date data has been written to constant (uniform) buffers, and
  // the buffer infos in current_constant_buffer_infos_ point to them.
  uint32_t current_constant_buffers_up_to_date_;
  VkDescriptorSet current_graphics_descriptor_sets_
      [SpirvShaderTranslator::kDescriptorSetCount];
  // Whether descriptor sets in current_graphics_descriptor_sets_ point to
  // up-to-date data.
  uint32_t current_graphics_descriptor_set_values_up_to_date_;
  // Whether the descriptor sets currently bound to the command buffer - only
  // low bits for the descriptor set layouts that remained the same are kept
  // when changing the pipeline layout. May be out of sync with
  // current_graphics_descriptor_set_values_up_to_date_, but should be ensured
  // to be a subset of it at some point when it becomes important; bits for
  // non-existent descriptor set layouts may also be set, but need to be ignored
  // when they start to matter.
  uint32_t current_graphics_descriptor_sets_bound_up_to_date_;
  static_assert(
      SpirvShaderTranslator::kDescriptorSetCount <=
          sizeof(current_graphics_descriptor_set_values_up_to_date_) * CHAR_BIT,
      "Bit fields storing descriptor set validity must be large enough");
  static_assert(
      SpirvShaderTranslator::kDescriptorSetCount <=
          sizeof(current_graphics_descriptor_sets_bound_up_to_date_) * CHAR_BIT,
      "Bit fields storing descriptor set validity must be large enough");
  // For reusing the texture/sampler descriptor sets across draws while their
  // contents stay valid - the shaders and the samplers the currently valid
  // texture descriptor sets were written for (texture image view changes are
  // tracked separately via VulkanTextureCache::texture_bindings_changed).
  const VulkanShader* current_textures_vertex_shader_ = nullptr;
  const VulkanShader* current_textures_pixel_shader_ = nullptr;
  std::vector<std::pair<VulkanTextureCache::SamplerParameters, VkSampler>>
      current_written_samplers_vertex_;
  std::vector<std::pair<VulkanTextureCache::SamplerParameters, VkSampler>>
      current_written_samplers_pixel_;

  // Float constant usage masks of the last draw call.
  uint64_t current_float_constant_map_vertex_[4];
  uint64_t current_float_constant_map_pixel_[4];
  // True if the vertex float constant buffer holds the full unpacked 256 float
  // constants for the ucode interpreter rather than a shader's packed subset.
  // Used to invalidate the binding when switching interpreter <-> normal.
  bool float_constants_vertex_are_full_ = false;

  // System shader constants, including user clip planes and tessellation
  // constants.
  SpirvShaderTranslator::SystemConstants system_constants_;

  // Host viewport of the previous draw, reused while the inputs it was derived
  // from stay the same.
  draw_util::GetViewportInfoArgs previous_viewport_info_args_{};
  draw_util::ViewportInfo previous_viewport_info_{};

  // Temporary storage for memexport stream constants used in the draw.
  std::vector<draw_util::MemExportRange> memexport_ranges_;

  // Backend-agnostic resolve-to-guest-RAM copy decisions and read-watch
  // consumption tracking, shared with the D3D12 backend.
#include "../command_processor_resolve_readwatch.inc"
  // Per-backend trampoline from the memory read callback into the shared
  // MarkResolvePagesRead, PrepareResolvePagesForWrite or DiscardResolvePages.
  static void ResolveReadCallbackThunk(void* context, uint32_t physical_address,
                                       uint32_t length,
                                       Memory::PhysicalAccess access);
  // Downscaling a scaled resolve's output into resolve_downscale_buffer_ for a
  // copy out of it, and writing it into the shared memory buffer where a native
  // resolve would have, for a fault to copy it from there. Declared after the
  // .inc for ScaledResolveReadbackInfo.
  bool DownscaleScaledResolve(uint32_t written_address,
                              const ScaledResolveReadbackInfo& scaled_info);
  bool MirrorScaledResolveToSharedMemory(uint32_t written_address,
                                         uint32_t written_length,
                                         reg::RB_COPY_DEST_INFO copy_dest_info,
                                         uint32_t& mirrored_length_out);

  // Debug marker support for RenderDoc/debug tools.
  bool debug_markers_enabled_ = false;
  void UpdateDebugMarkersEnabled();
};

}  // namespace vulkan
}  // namespace gpu
}  // namespace xe

#endif  // XENIA_GPU_VULKAN_VULKAN_COMMAND_PROCESSOR_H_
