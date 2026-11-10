/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia contributors. All rights reserved.                    *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */
#include "xenia/gpu/vulkan/vulkan_command_processor.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <limits>

#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/base/native_system_ui_trace.h"
#include "xenia/gpu/native_frame_execution.h"
#include "xenia/ui/vulkan/vulkan_util.h"

DECLARE_bool(native_frame_composite_probe);

namespace xe::gpu::vulkan {

#define TRACE_NATIVE_REQUEST(component, event, request, ...)               \
  do {                                                                     \
    if (cvars::native_system_ui_trace && request) {                        \
      static xe::NativeSystemUiTraceLimiter limiter;                       \
      const auto& trace_identity = request->identity();                    \
      limiter.Emit(component, event,                                       \
                   {{"kernel_epoch", trace_identity.owner.kernel_epoch},   \
                    {"runtime_module_generation",                          \
                     trace_identity.owner.module_generation},              \
                    {"owner_session_id", trace_identity.owner.session_id}, \
                    {"root_hmodule", trace_identity.owner.module_hmodule}, \
                    {"source_serial", trace_identity.source_serial},       \
                    {"backend_epoch", request->backend_epoch()}},          \
                   {__VA_ARGS__});                                         \
    }                                                                      \
  } while (false)

namespace {
namespace shaders {
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/native_frame_copy_cs.h"
}  // namespace shaders
constexpr VkFormat kNativePresentationFormat = VK_FORMAT_R8G8B8A8_UNORM;
struct NativePresentationCopyConstants {
  uint32_t size[2];
  uint32_t associate_overlay_rgb;
  uint32_t reserved;
};
static_assert(sizeof(NativePresentationCopyConstants) == 16);
static_assert(offsetof(NativePresentationCopyConstants, associate_overlay_rgb) == 8);
static_assert(offsetof(NativePresentationCopyConstants, reserved) == 12);
}  // namespace

bool VulkanCommandProcessor::EnsureNativePresentationPipelines() {
  auto* vd = GetVulkanDevice();
  if (native_submissions_sealed_ ||
      vd->properties().apiVersion < VK_MAKE_API_VERSION(0, 1, 2, 0) ||
      !vd->properties().imageViewFormatSwizzle) {
    return false;
  }
  const auto& dfn = vd->functions();
  const VkDevice device = vd->device();
  for (const VkFormat format : {kNativePresentationFormat}) {
    VkFormatProperties properties;
    vd->vulkan_instance()->functions().vkGetPhysicalDeviceFormatProperties(
        vd->physical_device(), format, &properties);
    VkFormatFeatureFlags required = VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT;
    if (format == kNativePresentationFormat) {
      required |= VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
    }
    if ((properties.optimalTilingFeatures & required) != required) {
      return false;
    }
  }
  if (native_presentation_descriptor_layout_ == VK_NULL_HANDLE) {
    std::array<VkDescriptorSetLayoutBinding, 2> bindings{};
    bindings[0] = {0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1,
                   VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    bindings[1] = {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1,
                   VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo info = {
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    info.bindingCount = uint32_t(bindings.size());
    info.pBindings = bindings.data();
    if (dfn.vkCreateDescriptorSetLayout(
            device, &info, nullptr, &native_presentation_descriptor_layout_) !=
        VK_SUCCESS) {
      return false;
    }
  }
  if (native_presentation_pipeline_layout_ == VK_NULL_HANDLE) {
    VkPushConstantRange constants = {
        VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(NativePresentationCopyConstants)};
    VkPipelineLayoutCreateInfo info = {
        VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    info.setLayoutCount = 1;
    info.pSetLayouts = &native_presentation_descriptor_layout_;
    info.pushConstantRangeCount = 1;
    info.pPushConstantRanges = &constants;
    if (dfn.vkCreatePipelineLayout(device, &info, nullptr,
                                   &native_presentation_pipeline_layout_) !=
        VK_SUCCESS) {
      return false;
    }
  }
  const auto create_pipeline = [&](const uint32_t* code, size_t bytes,
                                   VkPipeline& pipeline) {
    if (pipeline != VK_NULL_HANDLE) {
      return true;
    }
    VkShaderModuleCreateInfo module_info = {
        VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    module_info.codeSize = bytes;
    module_info.pCode = code;
    VkShaderModule module = VK_NULL_HANDLE;
    if (dfn.vkCreateShaderModule(device, &module_info, nullptr, &module) !=
        VK_SUCCESS) {
      return false;
    }
    VkComputePipelineCreateInfo info = {
        VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    info.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    info.stage.module = module;
    info.stage.pName = "main";
    info.layout = native_presentation_pipeline_layout_;
    const VkResult created = dfn.vkCreateComputePipelines(
        device, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline);
    dfn.vkDestroyShaderModule(device, module, nullptr);
    return created == VK_SUCCESS;
  };
  return create_pipeline(shaders::native_frame_copy_cs,
                         sizeof(shaders::native_frame_copy_cs),
                         native_presentation_copy_pipeline_);
}

VulkanCommandProcessor::NativePresentationImage*
VulkanCommandProcessor::BeginNativePresentationImage(
    const std::shared_ptr<const NativeFrameExecutionRequest>& request,
    std::shared_ptr<const ui::NativeUiPlaneAuthority> authority) {
  ReclaimNativePresentationImages();
  if (!request || !native_presentation_next_version_ ||
      native_presentation_next_version_ == UINT64_MAX) {
    return nullptr;
  }
  // This authority was captured before GPU work. A concurrent hide/withdraw
  // only suppresses later publication, never invalidates the Source
  // transaction.
  if (!authority ||
      authority->owner().allocation_owner != request->identity().owner ||
      authority->owner().backend_epoch != request->backend_epoch()) {
    return nullptr;
  }
  auto image = std::make_shared<NativePresentationImage>();
  image->identity = request->identity();
  image->backend_epoch = request->backend_epoch();
  image->version = native_presentation_next_version_++;
  image->authority = std::move(authority);
  if (request->output_layout()) {
    image->width = request->output_layout()->width;
    image->height = request->output_layout()->height;
  }
  auto* result = image.get();
  native_presentation_images_.push_back(std::move(image));
  TRACE_NATIVE_REQUEST("presentation", "image_version_created", request,
                       {"image_version", result->version},
                       {"foreground_generation",
                        result->authority->owner().foreground_generation},
                       {"width", result->width}, {"height", result->height});
  return result;
}

bool VulkanCommandProcessor::RecordNativePresentationCopy(
    NativePresentationImage& image, const NativeFrameExecutionRequest& request,
    memory::PhysicalRange written) {
  if (!native_guest_render_bank_selected_ || !submission_open_ ||
      !request.output_layout() ||
      !ValidateNativeFrameOutputStorage(*request.output_layout(),
                                        request.output_range()) ||
      image.width != request.output_layout()->width ||
      image.height != request.output_layout()->height ||
      image.width > GetVulkanDevice()->properties().maxImageDimension2D ||
      image.height > GetVulkanDevice()->properties().maxImageDimension2D ||
      !EnsureNativePresentationPipelines()) {
    return false;
  }
  auto* vd = GetVulkanDevice();
  const auto& dfn = vd->functions();
  const VkDevice device = vd->device();
  // Every handle is stored in its CP-owned version before any GPU reference.
  VkImageCreateInfo image_info = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  image_info.imageType = VK_IMAGE_TYPE_2D;
  image_info.format = kNativePresentationFormat;
  image_info.extent = {image.width, image.height, 1};
  image_info.mipLevels = image_info.arrayLayers = 1;
  image_info.samples = VK_SAMPLE_COUNT_1_BIT;
  image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
  image_info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
  image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  if (dfn.vkCreateImage(device, &image_info, nullptr, &image.image) !=
      VK_SUCCESS) {
    return false;
  }
  VkMemoryRequirements requirements;
  dfn.vkGetImageMemoryRequirements(device, image.image, &requirements);
  VkMemoryAllocateInfo memory_info = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  memory_info.allocationSize = requirements.size;
  memory_info.memoryTypeIndex = ui::vulkan::util::ChooseMemoryType(
      vd->memory_types(), requirements.memoryTypeBits,
      ui::vulkan::util::MemoryPurpose::kDeviceLocal);
  if (memory_info.memoryTypeIndex == UINT32_MAX ||
      dfn.vkAllocateMemory(device, &memory_info, nullptr, &image.memory) !=
          VK_SUCCESS ||
      dfn.vkBindImageMemory(device, image.image, image.memory, 0) !=
          VK_SUCCESS) {
    return false;
  }
  VkImageViewCreateInfo view_info = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  view_info.image = image.image;
  view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
  view_info.format = kNativePresentationFormat;
  view_info.subresourceRange = ui::vulkan::util::InitializeSubresourceRange();
  if (dfn.vkCreateImageView(device, &view_info, nullptr, &image.view) !=
      VK_SUCCESS) {
    return false;
  }
  constexpr uint32_t kSetCount = 1;
  const VkDescriptorPoolSize sizes[2] = {
      {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, kSetCount},
      {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, kSetCount}};
  VkDescriptorPoolCreateInfo pool_info = {
      VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  pool_info.maxSets = kSetCount;
  pool_info.poolSizeCount = 2;
  pool_info.pPoolSizes = sizes;
  if (dfn.vkCreateDescriptorPool(device, &pool_info, nullptr,
                                 &image.descriptor_pool) != VK_SUCCESS) {
    return false;
  }
  std::array<VkDescriptorSetLayout, kSetCount> layouts;
  layouts.fill(native_presentation_descriptor_layout_);
  std::array<VkDescriptorSet, kSetCount> sets{};
  VkDescriptorSetAllocateInfo allocation = {
      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  allocation.descriptorPool = image.descriptor_pool;
  allocation.descriptorSetCount = kSetCount;
  allocation.pSetLayouts = layouts.data();
  if (dfn.vkAllocateDescriptorSets(device, &allocation, sets.data()) !=
      VK_SUCCESS) {
    return false;
  }
  image.copy_set = sets[0];
  VkImageView source = texture_cache_->RequestNativePresentationTexture(
      *request.output_layout(), request.output_range(), written);
  if (source == VK_NULL_HANDLE) {
    return false;
  }
  VkDescriptorImageInfo source_info = {
      VK_NULL_HANDLE, source, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
  VkDescriptorImageInfo destination_info = {VK_NULL_HANDLE, image.view,
                                            VK_IMAGE_LAYOUT_GENERAL};
  VkWriteDescriptorSet writes[2] = {};
  for (uint32_t i = 0; i < 2; ++i) {
    writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[i].dstSet = image.copy_set;
    writes[i].dstBinding = i;
    writes[i].descriptorCount = 1;
    writes[i].descriptorType =
        i ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    writes[i].pImageInfo = i ? &destination_info : &source_info;
  }
  dfn.vkUpdateDescriptorSets(device, 2, writes, 0, nullptr);
  image.last_reference_submission = GetCurrentSubmission();
  PushImageMemoryBarrier(
      image.image, ui::vulkan::util::InitializeSubresourceRange(),
      VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
      0, VK_ACCESS_SHADER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
      VK_IMAGE_LAYOUT_GENERAL);
  SubmitBarriers(true);
  BindExternalComputePipeline(native_presentation_copy_pipeline_);
  deferred_command_buffer_.CmdVkBindDescriptorSets(
      VK_PIPELINE_BIND_POINT_COMPUTE, native_presentation_pipeline_layout_, 0,
      1, &image.copy_set, 0, nullptr);
  const NativePresentationCopyConstants constants{
      {image.width, image.height},
      request.output_layout()->origin ==
              NativeFrameOutputLayout::Origin::kNativeOverlayResolve
          ? 1u
          : 0u,
      0u};
  deferred_command_buffer_.CmdVkPushConstants(
      native_presentation_pipeline_layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0,
      sizeof(constants), &constants);
  deferred_command_buffer_.CmdVkDispatch((image.width + 15) / 16,
                                         (image.height + 7) / 8, 1);
  ++submission_in_progress_.dispatch_count;
  image.copy_recorded = true;
  PushImageMemoryBarrier(
      image.image, ui::vulkan::util::InitializeSubresourceRange(),
      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
      VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
      VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_LAYOUT_GENERAL,
      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  return true;
}

void VulkanCommandProcessor::CommitNativePresentationImage(
    NativePresentationImage* image, const NativeFrameExecutionResult& result) {
  if (!image) {
    return;
  }
  image->native_completion = result.completion;
  image->native_write_submission = result.completion.submission();
  image->last_reference_submission = std::max(image->last_reference_submission,
                                              image->native_write_submission);
  const auto tag = result.completion.native_frame_identity();
  image->failed =
      result.status != NativeFrameExecutionResult::Status::kSubmitted ||
      !result.title_state_restored || !image->copy_recorded || !tag ||
      *tag != image->identity ||
      result.completion.backend_epoch() != image->backend_epoch;
  image->published = !image->failed;
  if (image->published) {
    native_presentation_current_ = image;
  }
  try {
    XELOGI(
        "Native presentation image: source={} epoch={} version={} write={} "
        "copy={} published={}",
        image->identity.source_serial, image->backend_epoch, image->version,
        image->native_write_submission, image->copy_recorded, image->published);
  } catch (...) {
  }
}

void VulkanCommandProcessor::PublishCompletedNativePresentationImages() {
  auto* base_presenter = graphics_system_->presenter();
  if (!base_presenter || native_submissions_sealed_ || native_frame_fatal_) {
    return;
  }
  auto* presenter = static_cast<ui::vulkan::VulkanPresenter*>(base_presenter);
  // Only the latest fully recorded image is published. A slower next source
  // leaves the last good immutable plane intact until its replacement is ready.
  for (auto it = native_presentation_images_.rbegin();
       it != native_presentation_images_.rend(); ++it) {
    auto& image = *it;
    if (image->version <= native_presentation_published_version_) {
      break;
    }
    if (!image->published || image->failed || image->presenter_published ||
        image->publication_withdrawn ||
        image->native_completion.status() != GpuCompletionStatus::kComplete) {
      continue;
    }
    if (!image->authority || !image->authority->IsLive() ||
        presenter->GetNativeUiPlaneAuthority(
            image->identity.owner, image->backend_epoch) != image->authority) {
      static xe::NativeSystemUiTraceLimiter limiter;
      limiter.Emit(
          "presentation", "publication_authority_withdrawn",
          {{"kernel_epoch", image->identity.owner.kernel_epoch},
           {"runtime_module_generation",
            image->identity.owner.module_generation},
           {"owner_session_id", image->identity.owner.session_id},
           {"root_hmodule", image->identity.owner.module_hmodule},
           {"source_serial", image->identity.source_serial},
           {"backend_epoch", image->backend_epoch},
           {"image_version", image->version},
           {"native_write_submission", image->native_write_submission},
           {"completion_status", uint32_t(image->native_completion.status())}});
      image->publication_withdrawn = true;
      continue;
    }
    ui::vulkan::VulkanPresenter::NativeUiPlaneImage publication;
    publication.device = GetVulkanDevice();
    publication.authority = image->authority;
    publication.identity = image->identity;
    publication.backend_epoch = image->backend_epoch;
    publication.version = image->version;
    publication.width = image->width;
    publication.height = image->height;
    publication.image = image->image;
    publication.view = image->view;
    publication.lifetime = image;
    publication.producer_completion = image->native_completion;
    try {
      image->presenter_published =
          presenter->PublishNativeUiPlane(std::move(publication));
      image->failed = !image->presenter_published;
      static xe::NativeSystemUiTraceLimiter limiter;
      limiter.Emit(
          "presentation", "image_published_to_presenter",
          {{"kernel_epoch", image->identity.owner.kernel_epoch},
           {"runtime_module_generation",
            image->identity.owner.module_generation},
           {"owner_session_id", image->identity.owner.session_id},
           {"root_hmodule", image->identity.owner.module_hmodule},
           {"source_serial", image->identity.source_serial},
           {"backend_epoch", image->backend_epoch},
           {"image_version", image->version},
           {"native_write_submission", image->native_write_submission},
           {"presenter_published", image->presenter_published},
           {"completion_status", uint32_t(image->native_completion.status())}});
      if (image->presenter_published) {
        native_presentation_published_version_ = image->version;
      }
    } catch (...) {
      // The Source fence is already complete; publication does no GPU work.
      // Report failed presentation without inventing another source ticket.
      image->failed = true;
    }
    break;
  }
}

void VulkanCommandProcessor::FailNativePresentation(
    uint64_t version, const char* reason) noexcept {
  for (auto& image : native_presentation_images_) {
    if (!version || image->version == version) {
      image->failed = true;
    }
  }
  RetainFailedNativeFrameGpuWork(native_frame_retained_, GetCurrentSubmission(),
                                 GetCurrentSubmission() - 1, false);
  try {
    XELOGE(
        "Native presentation failed: version={} reason={}; retained and halted",
        version, reason);
    xe::FlushLog();
  } catch (...) {
  }
}

NativeFramePresentationStatus
VulkanCommandProcessor::GetNativeFramePresentationStatus(
    std::shared_ptr<const NativeFrameExecutionRequest> request) {
  NativeFramePresentationStatus status;
  if (!request) {
    return status;
  }
  status.identity = request->identity();
  status.backend_epoch = request->backend_epoch();
  if (!native_frame_fatal_ && !native_submissions_sealed_) {
    PollGpuCompletions();
    PublishCompletedNativePresentationImages();
  }
  for (auto it = native_presentation_images_.rbegin();
       it != native_presentation_images_.rend(); ++it) {
    const auto& image = **it;
    if (image.identity != request->identity() ||
        image.backend_epoch != request->backend_epoch()) {
      continue;
    }
    status.image_version = image.version;
    status.native_write_submission = image.native_write_submission;
    status.image_ready =
        image.published && image.view != VK_NULL_HANDLE &&
        image.native_completion.status() == GpuCompletionStatus::kComplete;
    bool failed = image.failed || native_frame_fatal_;
    if (auto* presenter = graphics_system_->presenter()) {
      const auto paint =
          presenter->GetNativeUiPlaneStatus(image.identity, image.version);
      status.presentation_backend_epoch = paint.presentation_backend_epoch;
      status.composite_submission = paint.paint_submission;
      status.composite_completion = paint.paint_completion;
      status.presenter_accepted = paint.presenter_accepted;
      failed |= paint.failed;
    }
    status.status = failed ? NativeFramePresentationStatus::Status::kFailed
                    : status.image_ready && status.presenter_accepted &&
                            status.composite_completion.status() ==
                                GpuCompletionStatus::kComplete
                        ? NativeFramePresentationStatus::Status::kComplete
                        : NativeFramePresentationStatus::Status::kPending;
    return status;
  }
  if (cvars::native_frame_composite_probe) {
    status.status = NativeFramePresentationStatus::Status::kFailed;
  }
  return status;
}

void VulkanCommandProcessor::DestroyNativePresentationImage(
    NativePresentationImage& image) {
  const auto& dfn = GetVulkanDevice()->functions();
  const VkDevice device = GetVulkanDevice()->device();
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyDescriptorPool, device,
                                         image.descriptor_pool);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyImageView, device,
                                         image.view);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyImage, device,
                                         image.image);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkFreeMemory, device,
                                         image.memory);
}

void VulkanCommandProcessor::ReclaimNativePresentationImages() {
  for (auto& image : native_presentation_images_) {
    if (image.get() != native_presentation_current_ &&
        (image->failed || image->published) && image.use_count() == 1 &&
        image->last_reference_submission <= GetCompletedSubmission()) {
      DestroyNativePresentationImage(*image);
    }
  }
  while (native_presentation_images_.size() > 64 &&
         native_presentation_images_.front().get() !=
             native_presentation_current_ &&
         native_presentation_images_.front()->image == VK_NULL_HANDLE) {
    native_presentation_images_.pop_front();
  }
}

void VulkanCommandProcessor::DestroyNativePresentationResources() {
  // Shutdown has already obtained the actual device-wide stop proof whenever
  // native commands entered this CP. Never put these handles in guest leases.
  for (auto& image : native_presentation_images_) {
    DestroyNativePresentationImage(*image);
  }
  native_presentation_images_.clear();
  native_presentation_current_ = nullptr;
  const auto& dfn = GetVulkanDevice()->functions();
  const VkDevice device = GetVulkanDevice()->device();
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyPipeline, device,
                                         native_presentation_copy_pipeline_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyPipelineLayout, device,
                                         native_presentation_pipeline_layout_);
  ui::vulkan::util::DestroyAndNullHandle(
      dfn.vkDestroyDescriptorSetLayout, device,
      native_presentation_descriptor_layout_);
}
}  // namespace xe::gpu::vulkan
