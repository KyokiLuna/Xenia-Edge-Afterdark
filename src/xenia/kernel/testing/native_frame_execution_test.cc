/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia contributors. All rights reserved.                    *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <random>
#include <string>
#include <functional>
#include <initializer_list>
#include <memory>
#include <thread>
#include <type_traits>
#include <vector>

#include "third_party/catch/include/catch.hpp"
#include "xenia/gpu/command_processor.h"
#include "xenia/gpu/draw_util.h"
#include "xenia/gpu/native_frame_execution.h"
#include "xenia/gpu/native_system_title_input_provider.h"
#include "xenia/gpu/register_file.h"
#include "xenia/gpu/trace_writer.h"
#include "xenia/gpu/ucode.h"
#include "xenia/kernel/native_frame_input_capture.h"
#include "xenia/kernel/native_frame_lease_probe.h"
#include "xenia/kernel/native_frame_owner.h"
#include "xenia/kernel/native_frame_readback_capture.h"
#include "xenia/kernel/testing/native_frame_execution_test_access.h"
#include "xenia/kernel/testing/native_system_title_input_test_access.h"
#include "xenia/memory.h"

namespace xe::kernel::test {
namespace {
using Session = NativeFrameOwnerSession;
using Request = gpu::NativeFrameExecutionRequest;
using Plan = gpu::NativeFrameCommandPlan;
using Range = memory::PhysicalRange;
using Status = memory::PhysicalLeaseStatus;
static_assert(!std::is_default_constructible_v<Request>);
static_assert(!std::is_default_constructible_v<gpu::NativeFrameInputSnapshot>);
static_assert(
    !std::is_default_constructible_v<gpu::NativeFramePm4ResolveProof>);
static_assert(!std::is_copy_constructible_v<Request>);
static_assert(!std::is_move_constructible_v<Request>);

std::vector<uint8_t> Bytes(const std::vector<uint32_t>& words) {
  std::vector<uint8_t> bytes;
  for (uint32_t word : words) {
    bytes.push_back(uint8_t(word >> 24));
    bytes.push_back(uint8_t(word >> 16));
    bytes.push_back(uint8_t(word >> 8));
    bytes.push_back(uint8_t(word));
  }
  return bytes;
}

class ModelGraph final : public NativeFrameGraphAuthority {
 public:
  explicit ModelGraph(Session::Owner owner) : owner_(owner) {}
  bool Matches(const Session::Owner& owner) const override {
    return owner == owner_;
  }
  uint64_t backend_epoch() const override {
    if (on_epoch) {
      on_epoch();
    }
    return epoch;
  }
  bool MatchesTitleInput(
      const gpu::NativeSystemTitleInput& input) const override {
    const auto& id = input.identity();
    return title_input_allowed && input.valid() && id.backend_epoch == epoch &&
           id.foreground_generation == 17 && id.title_hmodule == 0x3001C000 &&
           id.title_load_generation == 23;
  }
  bool CanExecuteNativeFrame(const NativeFrameSource& source) const override {
    return execution_allowed && source.owner() == owner_ && source.serial();
  }
  bool IsPresentationOwnerLive(const Session::Owner& owner) const override {
    if (on_presentation) {
      on_presentation();
    }
    return presentation_allowed && owner == owner_;
  }
  bool AbortUnsubmitted(const NativeFrameSource& source) override {
    return source.owner() == owner_;
  }
  bool ValidateNativeSubmission(
      const NativeFrameSource& source,
      const gpu::GpuCompletion& completion) const override {
    if (on_validate) {
      on_validate();
    }
    const auto tag = completion.native_frame_identity();
    return accepts_model_submission && source.owner() == owner_ && tag &&
           *tag ==
               gpu::NativeFrameSubmissionIdentity{owner_, source.serial()} &&
           completion.backend_epoch() == epoch && completion.submission() &&
           (completion.status() == gpu::GpuCompletionStatus::kPending ||
            completion.status() == gpu::GpuCompletionStatus::kComplete);
  }
  bool RetireComplete(const NativeFrameSource& source,
                      const gpu::GpuCompletion& completion) override {
    return completion.status() == gpu::GpuCompletionStatus::kComplete &&
           ValidateNativeSubmission(source, completion);
  }
  bool RetireAfterKernelStop(const Session::Owner& owner) override {
    return stops && owner == owner_;
  }
  bool title_input_allowed = true;
  bool execution_allowed = true;
  bool presentation_allowed = false;
  bool accepts_model_submission = false, stops = true;
  uint64_t epoch = 42;
  std::function<void()> on_epoch, on_validate, on_presentation;

 private:
  const Session::Owner owner_;
};

struct OutputGeometry {
  uint32_t width = 640, height = 480;
  uint32_t source_pitch = 640, destination_pitch = 640, window_width = 640;
  uint32_t allocation_bytes = 640 * 480 * 4;
};

class Fixture {
 public:
  explicit Fixture(bool inline_shader = false, bool checked_output = false,
                   OutputGeometry geometry = {},
                   bool overlay_model = false)
      : geometry(geometry),
        rectangle_offset(overlay_model ? 0x800 : 0x100) {
    REQUIRE(memory.Initialize());
    // Opaque host identity markers, never dereferenced or guest GPU objects.
    thread = {reinterpret_cast<const KernelState*>(&kernel_marker),
              reinterpret_cast<const XThread*>(&thread_marker), 7, 2};
    runtime = std::make_shared<GuestRuntime>(
        GuestRuntime::Identity{0x30001000, 9}, std::make_shared<int>(1));
    const GuestRuntime::Plan initialization{
        "host-only preparation model",
        {{"startup",
          {{"model entry",
            0x81001000,
            {},
            GuestRuntime::ReturnKind::kVoid,
            "host executor only; no PPC"}}}}};
    REQUIRE(runtime->Initialize(initialization, [](const auto&) {
      return GuestRuntime::ExecutionResult{true, 0, {}};
    }) == GuestRuntime::InitStatus::kReady);
    session = Session::CreateProvisional(thread, 3, runtime);
    REQUIRE(session);
    graph = std::make_shared<ModelGraph>(session->owner());
    REQUIRE(session->BindGraph(thread, graph));
    if (overlay_model) {
      // Keep deliberate output-tail overruns away from command inputs, so the
      // model can distinguish byte-layout validity from real allocation pins.
      Allocate(&output_address, &output_id, &output,
               checked_output ? geometry.allocation_bytes : 0x2000);
      Allocate(&record_address, &record_id, &record);
    } else {
      Allocate(&record_address, &record_id, &record);
      Allocate(&output_address, &output_id, &output,
               checked_output ? geometry.allocation_bytes : 0x2000);
    }
    std::vector<uint32_t> words;
    const auto write = [&](uint32_t reg,
                           std::initializer_list<uint32_t> values) {
      words.push_back(uint32_t(values.size() - 1) << 16 | reg);
      words.insert(words.end(), values);
    };
    if (inline_shader) {
      // The same public ISA bitfields used by the shader-read fixtures: one
      // straight-line vertex fetch, never supplied proprietary shader code.
      const std::vector<uint32_t> code{
          1 | (1u << 12) | (1u << 16),
          uint32_t(gpu::ucode::ControlFlowOpcode::kExecEnd) << 12 |
              uint32_t(gpu::ucode::AddressingMode::kAbsolute) << 11,
          0,
          1u << 19,
          0x688 | (uint32_t(gpu::xenos::VertexFormat::k_32_32_FLOAT) << 16),
          2};
      words.push_back(0xC0000000 | uint32_t(code.size() + 1) << 16 |
                      uint32_t(gpu::xenos::PM4_IM_LOAD_IMMEDIATE) << 8);
      words.push_back(0);
      words.push_back(uint32_t(code.size()));
      words.insert(words.end(), code.begin(), code.end());
    }
    write(gpu::XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0,
          {(record.address + rectangle_offset) |
               uint32_t(gpu::xenos::FetchConstantType::kVertex),
           (6u << 2) | (checked_output ? 2u : 0u)});
    write(gpu::XE_GPU_REG_RB_MODECONTROL,
          {uint32_t(gpu::xenos::EdramMode::kCopy)});
    write(gpu::XE_GPU_REG_PA_SC_VIZ_QUERY, {0});
    write(gpu::XE_GPU_REG_VGT_OUTPUT_PATH_CNTL, {0});
    if (checked_output) {
      write(gpu::XE_GPU_REG_RB_SURFACE_INFO,
            {(geometry.destination_pitch << 18) | geometry.source_pitch});
      write(gpu::XE_GPU_REG_RB_COPY_CONTROL, {0x00100000});
      write(gpu::XE_GPU_REG_RB_COPY_DEST_BASE, {output.address});
      write(gpu::XE_GPU_REG_RB_COPY_DEST_INFO, {0x01000300});
      write(gpu::XE_GPU_REG_RB_COPY_DEST_PITCH,
            {(geometry.height << 16) | geometry.destination_pitch});
      write(gpu::XE_GPU_REG_RB_COPY_SURFACE_SLICE, {0});
      write(gpu::XE_GPU_REG_PA_SU_VTX_CNTL, {4});
      write(gpu::XE_GPU_REG_PA_SC_WINDOW_OFFSET, {0});
      write(gpu::XE_GPU_REG_PA_SC_WINDOW_SCISSOR_TL, {0});
      write(gpu::XE_GPU_REG_PA_SC_WINDOW_SCISSOR_BR,
            {(geometry.height << 16) | geometry.window_width});
      write(gpu::XE_GPU_REG_PA_SC_SCREEN_SCISSOR_TL, {0});
      write(gpu::XE_GPU_REG_PA_SC_SCREEN_SCISSOR_BR, {0x20002000});
    }
    words.push_back(0xC0000000 | uint32_t(gpu::xenos::PM4_DRAW_INDX_2) << 8);
    words.push_back(0x30088);
    if (overlay_model) {
      // Complete synthetic native-profile batches after final copy+flush.
      // These are public register fixtures, not captured native UI bytes.
      words.push_back(0xC0000000 |
                      uint32_t(gpu::xenos::PM4_EVENT_WRITE) << 8);
      words.push_back(gpu::xenos::CACHE_FLUSH);
      const auto enable = [&](uint32_t value) {
        write(gpu::XE_GPU_REG_D1OVL_UPDATE, {0x10000});
        write(gpu::XE_GPU_REG_D1OVL_ALPHA_OUTSIDE_OVL_WINDOW, {0, 255, 0});
        write(gpu::XE_GPU_REG_D1OVL_ENABLE, {value});
        write(gpu::XE_GPU_REG_D1OVL_UPDATE, {0});
      };
      enable(1);
      write(gpu::XE_GPU_REG_D1OVL_UPDATE, {0x10000});
      write(gpu::XE_GPU_REG_D1OVL_CONTROL1, {0});
      write(gpu::XE_GPU_REG_D1OVL_SURFACE_ADDRESS, {output.address});
      write(gpu::XE_GPU_REG_D1OVL_PITCH, {geometry.destination_pitch});
      write(gpu::XE_GPU_REG_D1OVL_SURFACE_OFFSET_X, {256, 576});
      write(gpu::XE_GPU_REG_D1OVL_START,
            {(256u << 16) | 576u,
             ((256u + geometry.width) << 16) | (576u + geometry.height)});
      write(gpu::XE_GPU_REG_D1OVL_UPDATE, {0});
      enable(0);
    }
    primary = Bytes(words);
    REQUIRE(primary.size() <= record.length);
    std::memcpy(memory.TranslatePhysical(record.address), primary.data(),
                primary.size());
    if (checked_output) {
      REQUIRE(primary.size() <= rectangle_offset);
      const std::array<float, 6> vertices{-0.5f,
                                          -0.5f,
                                          float(geometry.width) - 0.5f,
                                          -0.5f,
                                          float(geometry.width) - 0.5f,
                                          float(geometry.height) - 0.5f};
      std::array<uint32_t, 6> words;
      for (size_t i = 0; i < words.size(); ++i) {
        words[i] = gpu::xenos::GpuSwap(std::bit_cast<uint32_t>(vertices[i]),
                                       gpu::xenos::Endian::k8in32);
      }
      std::memcpy(memory.TranslatePhysical(record.address + rectangle_offset),
                  words.data(), sizeof(words));
    }
    const auto analyzed = Plan::Analyze(primary);
    REQUIRE(analyzed);
    plan = analyzed.plan;
    for (const auto& load : plan->inventory().shaders) {
      const auto offset = size_t(load.inline_dword_offset) * 4;
      std::vector<uint8_t> code(
          primary.begin() + offset,
          primary.begin() + offset + load.dword_count * 4);
      auto metadata = gpu::NativeShaderReadAnalysis::Analyze(load.stage, code);
      REQUIRE(metadata.metadata_analyzed);
      REQUIRE(metadata.error == gpu::NativeShaderReadAnalysis::Error::kNone);
      shaders.push_back({load, std::move(code), std::move(metadata)});
    }
    Freeze();
  }
  ~Fixture() {
    graph->on_epoch = {};
    graph->on_validate = {};
    graph->on_presentation = {};
    session->RetireAfterKernelStop([](const auto&) { return true; });
  }
  void Allocate(uint32_t* address, uint64_t* id, Range* range,
                uint32_t size = 0x2000) {
    auto* heap = static_cast<PhysicalHeap*>(memory.LookupHeap(0xE1000000));
    REQUIRE(heap->AllocRangeOwned(
        0xE1000000, 0xE1400000, size, 0x1000,
        kMemoryAllocationReserve | kMemoryAllocationCommit,
        kMemoryProtectRead | kMemoryProtectWrite, true, session->owner(),
        address, id));
    *range = {memory.GetPhysicalAddress(*address), size};
  }
  void Freeze(std::shared_ptr<const gpu::NativeSystemTitleInput> input = {}) {
    auto update = session->BeginUpdate(thread);
    REQUIRE(update);
    source = session->Freeze(thread, update, primary, std::move(input));
    REQUIRE(source);
    const std::array<Range, 2> ranges{record, output};
    const auto pin = memory.PinOwnedPhysicalRanges(session->owner(), ranges);
    REQUIRE(pin);
    REQUIRE(session->AttachPhysicalPin(thread, source, pin.pin));
  }
  std::vector<Range> Inputs() const {
    return {{record.address, uint32_t(primary.size())},
            {record.address + rectangle_offset, 24}};
  }
  std::shared_ptr<const Request> Mint() {
    return NativeFrameExecutionTestAccess::Mint(session, thread, source, plan,
                                                output, Inputs(), shaders);
  }
  std::unique_ptr<NativeFrameInputCapture> CaptureInputs() {
    return NativeFrameExecutionTestAccess::Capture(
        &memory, session, thread, source, plan,
        {record.address, uint32_t(primary.size())}, output);
  }
  NativeCompletedFrameRecord OutputRecord() const {
    NativeCompletedFrameRecord completed;
    completed.primary = primary;
    completed.record_physical_address = record.address;
    completed.record_allocation_bytes = record.length;
    completed.output_physical_address = output.address;
    completed.output_allocation_bytes = output.length;
    completed.native_end_completed = true;
    completed.primary_physical_address = record.address;
    completed.output_cpu_address = output_address;
    completed.output_texture_fetch = {
        0x85000002, (output_address & ~0xFFFu) | 6u, 0x003BE27F, 0x00000C14, 0,
        0x00000200};
    return completed;
  }
  NativeCompletedFrameRecord Pm4Record(bool canonical = false) const {
    auto completed = OutputRecord();
    completed.output_texture_fetch = {};  // No invented/genuine XG header.
    if (canonical) {
      completed.output_cpu_address = output.address;
    }
    return completed;
  }
  static void DescriptorWord(std::array<uint8_t, 148>& bytes, uint32_t at,
                             uint32_t value) {
    for (uint32_t i = 0; i < 4; ++i) {
      bytes[at + i] = uint8_t(value >> (24 - i * 8));
    }
  }
  std::array<uint8_t, 148> Descriptor(
      const NativeCompletedFrameRecord& completed,
      bool canonical_primary = false) const {
    std::array<uint8_t, 148> descriptor{};
    DescriptorWord(descriptor, 0,
                   canonical_primary ? completed.primary_physical_address
                                     : record_address);
    DescriptorWord(descriptor, 4, uint32_t(primary.size() / 4));
    DescriptorWord(descriptor, 8, completed.output_cpu_address);
    DescriptorWord(descriptor, 12, 6);
    DescriptorWord(descriptor, 20, geometry.width);
    DescriptorWord(descriptor, 24, geometry.height);
    return descriptor;
  }
  std::optional<gpu::NativeFrameOutputLayout> Pm4Layout(
      const NativeCompletedFrameRecord& completed,
      const std::array<uint8_t, 148>& descriptor) {
    return NativeFrameExecutionTestAccess::Pm4Layout(&memory, session, thread,
                                                     source, plan, Inputs(),
                                                     completed, descriptor);
  }
  void RewriteRegister(uint32_t index, uint32_t value) {
    std::vector<uint32_t> words(plan->words().begin(), plan->words().end());
    bool found = false;
    for (const auto& write : plan->register_writes()) {
      if (write.index == index) {
        const uint32_t first = words[write.packet_dword_offset] & 0x7FFF;
        words[write.packet_dword_offset + 1 + index - first] = value;
        found = true;
      }
    }
    REQUIRE(found);
    REQUIRE(session->AbortUnsubmitted(thread, source));
    primary = Bytes(words);
    std::memcpy(memory.TranslatePhysical(record.address), primary.data(),
                primary.size());
    const auto analyzed = Plan::Analyze(primary);
    REQUIRE(analyzed);
    plan = analyzed.plan;
    Freeze();
  }
  struct SubmittedFrame {
    std::shared_ptr<const Request> request;
    gpu::GpuCompletion completion;
  };
  SubmittedFrame SubmitModel(gpu::GpuCompletionQueue& queue,
                             bool complete = true) {
    // A real host completion queue models the ticket; no GPU or guest device
    // is created, and these tests do not claim an actual submission/fence.
    graph->epoch = queue.backend_epoch();
    graph->accepts_model_submission = true;
    auto request = Mint();
    REQUIRE(request);
    REQUIRE(request->MarkSubmissionAttempted());
    const auto completion =
        queue.TrackNativeFrameSubmitted(1, request->identity());
    REQUIRE(session->AttachGpu(thread, source, completion));
    if (complete) {
      queue.CompleteThrough(1);
    }
    return {std::move(request), completion};
  }
  const OutputGeometry geometry;
  const uint32_t rectangle_offset;
  Memory memory;
  int kernel_marker = 0, thread_marker = 0;
  Session::ThreadIdentity thread;
  std::shared_ptr<GuestRuntime> runtime;
  std::shared_ptr<Session> session;
  std::shared_ptr<ModelGraph> graph;
  std::shared_ptr<const NativeFrameSource> source;
  std::shared_ptr<const Plan> plan;
  uint32_t record_address = 0, output_address = 0;
  uint64_t record_id = 0, output_id = 0;
  Range record{}, output{};
  std::vector<uint8_t> primary;
  std::vector<Request::Shader> shaders;
};

// Public geometry encoded into real Memory; no supplied rectangle DWORDs,
// native device, Task or GPU stand-in. Source primary/ownership remain genuine
// host test mechanisms; only the pre-seal rectangle payload is changed here.
void WriteResolveRectangle(Fixture& f, float right, float bottom) {
  const std::array<float, 6> vertices{-0.5f, -0.5f,        right - 0.5f,
                                      -0.5f, right - 0.5f, bottom - 0.5f};
  std::array<uint32_t, 6> words;
  for (size_t i = 0; i < words.size(); ++i) {
    words[i] = gpu::xenos::GpuSwap(std::bit_cast<uint32_t>(vertices[i]),
                                   gpu::xenos::Endian::k8in32);
  }
  std::memcpy(f.memory.TranslatePhysical(f.record.address + 0x100),
              words.data(), sizeof(words));
}

gpu::draw_util::ResolveInfo CommonResolveInfo(const Fixture& f) {
  gpu::RegisterFile regs;
  const auto packet = f.plan->inventory().draws.back().packet_dword_offset;
  for (const auto& write : f.plan->register_writes()) {
    if (write.packet_dword_offset > packet) {
      break;
    }
    regs.values[write.index] = write.value;
  }
  // The real CPU resolver reads the real 24B vertices, including endian,
  // fixed16p8 raster conversion, scissor/align8/source-pitch handling and tiled
  // address bounds. This writer is never opened and emits no trace file.
  gpu::TraceWriter trace(f.memory.physical_membase());
  gpu::draw_util::ResolveInfo result;
  REQUIRE(gpu::draw_util::GetResolveInfo(regs, f.memory, trace, 1, 1, false,
                                         false, result));
  return result;
}

// These are CPU-only immutable leases made by the same private test peer used
// by the provider tests. They never claim a real GPU capture or title
// execution.
std::shared_ptr<const gpu::NativeSystemTitleInput> ModelTitleInput(
    uint64_t epoch = 42, uint32_t address = 0x10000000, uint32_t width = 2048,
    uint32_t height = 1024) {
  using Input = gpu::NativeSystemTitleInput;
  gpu::xenos::xe_gpu_texture_fetch_t fetch{};
  fetch.type = gpu::xenos::FetchConstantType::kTexture;
  fetch.format = gpu::xenos::TextureFormat::k_8_8_8_8;
  fetch.dimension = gpu::xenos::DataDimension::k2DOrStacked;
  fetch.pitch = width >> 5;
  fetch.base_address = address >> 12;
  fetch.size_2d.width = width - 1;
  fetch.size_2d.height = height - 1;
  fetch.swizzle = 0x688;
  Input::Swap swap{17, 0x3001C000, 23, {}, width, height};
  std::memcpy(swap.fetch.data(), &fetch, sizeof(fetch));
  const auto layout = Input::GetLayout(swap);
  REQUIRE(layout);
  return gpu::test::NativeSystemTitleInputTestAccess::Create(
      {epoch, 17, 0x3001C000, 23, 1}, *layout,
      std::vector<uint8_t>(layout->range.length, 0xA7));
}

void RefreezeWithTitle(
    Fixture& f, std::shared_ptr<const gpu::NativeSystemTitleInput> input) {
  REQUIRE(f.session->AbortUnsubmitted(f.thread, f.source));
  const auto analyzed = Plan::AnalyzeWithTitleInput(f.primary, input);
  REQUIRE(analyzed);
  f.plan = analyzed.plan;
  f.Freeze(std::move(input));
}

TEST_CASE("Immutable title storage exceeds native read limit without owner pin",
          "[kernel][native_frame_execution][native_title_input]") {
  Fixture f(false, true);
  const auto input = ModelTitleInput();
  REQUIRE(input);
  REQUIRE(input->range().length == 8 * 1024 * 1024);
  RefreezeWithTitle(f, input);
  auto capture = f.CaptureInputs();
  REQUIRE(capture);
  REQUIRE(capture->Capture(f.record));
  const auto snapshot = capture->Seal();
  REQUIRE(snapshot);
  capture.reset();
  REQUIRE(snapshot->title_input() == input);
  CHECK(snapshot->byte_size() == input->range().length + f.record.length);
  CHECK(snapshot->Covers(input->range()));
  std::vector<uint8_t> pixels(input->range().length);
  REQUIRE(snapshot->Read(input->range(), pixels));
  CHECK(pixels == std::vector<uint8_t>(pixels.size(), 0xA7));
  // No guest allocation or native-owned pin backs these imported pixels.
  CHECK_FALSE(f.memory.PinOwnedPhysicalRanges(f.session->owner(),
                                              std::span(&input->range(), 1)));
  auto inputs = f.Inputs();
  inputs.push_back(input->range());
  const auto record = f.Pm4Record();
  const auto layout = NativeFrameExecutionTestAccess::Pm4Layout(
      &f.memory, f.session, f.thread, f.source, f.plan, inputs, record,
      f.Descriptor(record), snapshot);
  REQUIRE(layout);
  const auto request = NativeFrameExecutionTestAccess::Mint(
      f.session, f.thread, f.source, f.plan, f.output, inputs, f.shaders,
      layout, snapshot);
  REQUIRE(request);
  CHECK(request->IsAdmitted());
  // The mechanism behind a successful Query's reservation is a strong CPU
  // lease, not continued membership in the provider cache. Cache withdrawal
  // must not retroactively revoke this admitted Source's immutable pixels.
  gpu::NativeSystemTitleInputProvider provider;
  REQUIRE(provider.Enable(42));
  const auto ticket = provider.Stage(input->layout().swap);
  REQUIRE(ticket);
  REQUIRE(provider.Take(ticket, input->range().address, input->layout().width,
                        input->layout().height));
  REQUIRE(provider.Publish(input));
  const auto reserved = provider.Latest(17);
  REQUIRE(reserved == input);
  auto invalid = input->layout().swap;
  invalid.width = 0;
  CHECK(provider.Stage(invalid) == 0);
  CHECK_FALSE(provider.Latest(17));
  CHECK(reserved->valid());
  CHECK(request->IsAdmitted());
  CHECK_FALSE(f.session->GetSnapshot().submission_attempted);
  // The same lease loses execution when its actual graph identity is revoked.
  f.graph->title_input_allowed = false;
  CHECK_FALSE(request->MarkSubmissionAttempted());
  CHECK_FALSE(f.session->GetSnapshot().submission_attempted);
}

TEST_CASE("Imported title pixels cannot satisfy native-owned capture",
          "[kernel][native_frame_execution][native_title_input]") {
  Fixture f;
  const auto input = ModelTitleInput();
  RefreezeWithTitle(f, input);
  auto capture = f.CaptureInputs();
  REQUIRE(capture);
  CHECK_FALSE(capture->Capture({input->range().address, 4}));
  CHECK_FALSE(capture->Seal());
  CHECK_FALSE(f.session->GetSnapshot().submission_attempted);
}

TEST_CASE("Source title lease rejects foreign Plan and render-output alias",
          "[kernel][native_frame_execution][native_title_input]") {
  Fixture f;
  const auto input = ModelTitleInput();
  RefreezeWithTitle(f, input);
  SECTION("Same metadata and bytes do not substitute exact immutable lease") {
    const auto other = ModelTitleInput();
    REQUIRE(other != input);
    const auto plan = Plan::AnalyzeWithTitleInput(f.primary, other);
    REQUIRE(plan);
    CHECK_FALSE(NativeFrameExecutionTestAccess::Capture(
        &f.memory, f.session, f.thread, f.source, plan.plan,
        {f.record.address, uint32_t(f.primary.size())}, f.output));
  }
  SECTION("Output overlap remains forbidden") {
    const auto alias = ModelTitleInput(42, f.output.address, 32, 32);
    RefreezeWithTitle(f, alias);
    CHECK_FALSE(f.CaptureInputs());
  }
}

TEST_CASE(
    "Only exact Title identity and reserved initial T15 bank are admitted",
    "[kernel][native_frame_execution][native_title_input]") {
  Fixture f;
  const auto input = ModelTitleInput();
  auto words = std::vector<uint32_t>{
      gpu::XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + 15 * 6, input->fetch()[0]};
  const auto old = f.plan->words();
  words.insert(words.end(), old.begin(), old.end());
  CHECK_FALSE(Plan::AnalyzeWithTitleInput(Bytes(words), input));
  REQUIRE(f.session->AbortUnsubmitted(f.thread, f.source));
  auto update = f.session->BeginUpdate(f.thread);
  REQUIRE(update);
  // Actual production graph checks backend + foreground + title load identity;
  // this existing host model exercises rejection without fabricating a Kernel.
  CHECK_FALSE(
      f.session->Freeze(f.thread, update, f.primary, ModelTitleInput(43)));
}

TEST_CASE("Reserved T15 inventory uses exact tiled color lease and loader span",
          "[kernel][native_frame_execution][native_title_input]") {
  using Inventory = gpu::NativeDrawReadInventory;
  using Input = gpu::NativeSystemTitleInput;
  const std::vector<uint32_t> shader_words{
      1u | 1u << 12 | 1u << 16,
      uint32_t(gpu::ucode::ControlFlowOpcode::kExecEnd) << 12 |
          uint32_t(gpu::ucode::AddressingMode::kAbsolute) << 11,
      0,
      uint32_t(gpu::ucode::FetchOpcode::kTextureFetch) | 1u << 19 | 15u << 20 |
          4u << 26,
      0x688 | 3u << 12 | 3u << 14 | 3u << 16 | 7u << 18 | 3u << 24 | 3u << 26 |
          1u << 28,
      uint32_t(gpu::xenos::FetchOpDimension::k2D) << 14};
  const auto metadata = gpu::NativeShaderReadAnalysis::Analyze(
      gpu::xenos::ShaderType::kPixel, Bytes(shader_words));
  REQUIRE(metadata);
  for (const auto format :
       {gpu::xenos::TextureFormat::k_8_8_8_8,
        gpu::xenos::TextureFormat::k_2_10_10_10_AS_16_16_16_16}) {
    auto swap = ModelTitleInput(42, 0x10000000, 64, 33)->layout().swap;
    gpu::xenos::xe_gpu_texture_fetch_t fetch;
    std::memcpy(&fetch, swap.fetch.data(), sizeof(fetch));
    fetch.tiled = true;
    fetch.format = format;
    fetch.swizzle = 0x60A;
    fetch.endianness = gpu::xenos::Endian::k8in32;
    std::memcpy(swap.fetch.data(), &fetch, sizeof(fetch));
    const auto layout = Input::GetLayout(swap);
    REQUIRE(layout);
    const auto input = gpu::test::NativeSystemTitleInputTestAccess::Create(
        {42, 17, 0x3001C000, 23, 1}, *layout,
        std::vector<uint8_t>(layout->range.length, 0xA7));
    REQUIRE(input);
    Inventory::Draw draw{};
    draw.initiator = 3u << 16 |
                     uint32_t(gpu::xenos::SourceSelect::kAutoIndex) << 6 |
                     uint32_t(gpu::xenos::PrimitiveType::kTriangleList);
    draw.state_known.set(0);
    draw.state_known.set(1);
    draw.state_known.set(2);
    draw.state_words[0] = uint32_t(gpu::xenos::EdramMode::kColorDepth);
    draw.pixel_shader = 0;
    draw.title_input = input;
    for (uint32_t i = 0; i < 6; ++i) {
      draw.fetch_known.set(15 * 6 + i);
      draw.fetch_words[15 * 6 + i] = input->fetch()[i];
    }
    const auto reads = Inventory::TextureReads(draw, &metadata);
    REQUIRE(reads.error == Inventory::Error::kNone);
    REQUIRE(reads.reads.size() == 1);
    CHECK(reads.reads[0].from_title_input);
    CHECK(reads.reads[0].byte_count == input->range().length);
    CHECK(reads.reads[0].fetch_words == input->fetch());
    CHECK(reads.reads[0].format == uint32_t(format));
    // Without the typed lease, RGBA may describe an ordinary native-owned
    // candidate, never imported Title authority. Its complete loader span
    // still needs native owner pins and snapshot bytes before a Request.
    draw.title_input.reset();
    const auto ordinary = Inventory::TextureReads(draw, &metadata);
    if (format == gpu::xenos::TextureFormat::k_8_8_8_8) {
      REQUIRE(ordinary.error == Inventory::Error::kNone);
      REQUIRE(ordinary.reads.size() == 1);
      CHECK_FALSE(ordinary.reads[0].from_title_input);
      CHECK_FALSE(ordinary.ownership_verified);
      CHECK_FALSE(ordinary.access_bounds_verified);
      CHECK_FALSE(ordinary.execution_ready);
    } else {
      CHECK(ordinary.error == Inventory::Error::kUnsupportedTexture);
    }
  }
}

TEST_CASE("Host output layout snapshot canonicalizes only the physical base",
          "[kernel][native_frame_execution][native_output_layout]") {
  // Real Memory pins and the bounded resolve packet/vertex pattern, without
  // invoking native code or claiming a genuine guest End/GPU execution.
  Fixture f(false, true);
  const auto record = f.OutputRecord();
  const auto layout = NativeFrameExecutionTestAccess::Layout(
      &f.memory, f.session, f.thread, f.source, f.plan, f.Inputs(), record);
  REQUIRE(layout);
  CHECK(layout->width == 640);
  CHECK(layout->height == 480);
  CHECK(layout->pitch == 640);
  CHECK(layout->required_bytes == 640 * 480 * 4);
  REQUIRE(layout->original_texture_fetch);
  CHECK(*layout->original_texture_fetch == record.output_texture_fetch);
  auto canonical = record.output_texture_fetch;
  canonical[1] = (canonical[1] & 0xFFF) | f.output.address;
  CHECK(layout->texture_fetch == canonical);
  CHECK((canonical[1] & ~0xFFFu) == f.output.address);
  CHECK(f.output.address != gpu::xenos::CpuToGpu(f.output_address));
  CHECK_FALSE(f.session->GetSnapshot().operation_active);
  auto request = NativeFrameExecutionTestAccess::Mint(
      f.session, f.thread, f.source, f.plan, f.output, f.Inputs(), f.shaders,
      layout);
  REQUIRE(request);
  REQUIRE(request->output_layout());
  CHECK(request->output_layout()->texture_fetch == canonical);
  CHECK(request->IsAdmitted());
  f.graph->execution_allowed = false;
  CHECK_FALSE(request->MarkSubmissionAttempted());
  CHECK_FALSE(f.session->GetSnapshot().submission_attempted);
}

TEST_CASE("Host layout snapshot rejects inconsistent target and copy evidence",
          "[kernel][native_frame_execution][native_output_layout]") {
  Fixture f(false, true);
  auto record = f.OutputRecord();
  auto inputs = f.Inputs();
  SECTION("Missing End") { record.native_end_completed = false; }
  SECTION("CPU alias disagrees") { record.output_cpu_address += 0x1000; }
  SECTION("Header base disagrees") { record.output_texture_fetch[1] += 0x1000; }
  SECTION("Swizzle disagrees") { record.output_texture_fetch[3] ^= 2; }
  SECTION("Unsupported endianness") { record.output_texture_fetch[1] |= 64; }
  SECTION("Linear texture") { record.output_texture_fetch[0] &= 0x7FFFFFFF; }
  SECTION("Different geometry") { record.output_texture_fetch[2] -= 1; }
  SECTION("Mip chain") { record.output_texture_fetch[4] |= 1u << 6; }
  SECTION("Output span too small") { record.output_allocation_bytes -= 4; }
  SECTION("Rectangle missing from inputs") { inputs.pop_back(); }
  SECTION("Input has no backing pin") { inputs.back().address = 0x1000; }
  SECTION("Partial rectangle") {
    auto word = gpu::xenos::GpuSwap(std::bit_cast<uint32_t>(478.5f),
                                    gpu::xenos::Endian::k8in32);
    std::memcpy(f.memory.TranslatePhysical(f.record.address + 0x100 + 20),
                &word, sizeof(word));
  }
  SECTION("Nonfinite rectangle") {
    const uint32_t word =
        gpu::xenos::GpuSwap(0x7FC00000u, gpu::xenos::Endian::k8in32);
    std::memcpy(f.memory.TranslatePhysical(f.record.address + 0x100), &word,
                sizeof(word));
  }
  SECTION("Copy writes a different base") {
    f.RewriteRegister(gpu::XE_GPU_REG_RB_COPY_DEST_BASE,
                      f.output.address + 0x1000);
  }
  SECTION("Copy has another pitch") {
    f.RewriteRegister(gpu::XE_GPU_REG_RB_COPY_DEST_PITCH, 0x01E002A0);
  }
  SECTION("Copy has another byte order") {
    f.RewriteRegister(gpu::XE_GPU_REG_RB_COPY_DEST_INFO, 0x01000301);
  }
  SECTION("Multisampled source") {
    f.RewriteRegister(gpu::XE_GPU_REG_RB_SURFACE_INFO, 0x0A010280);
  }
  SECTION("Clipped copy") {
    f.RewriteRegister(gpu::XE_GPU_REG_PA_SC_WINDOW_SCISSOR_BR, 0x01DF0280);
  }
  SECTION("Source already aborted") {
    REQUIRE(f.session->AbortUnsubmitted(f.thread, f.source));
  }
  SECTION("Source revoked") { f.session->Revoke(); }
  CHECK_FALSE(NativeFrameExecutionTestAccess::Layout(
      &f.memory, f.session, f.thread, f.source, f.plan, inputs, record));
  CHECK_FALSE(f.session->GetSnapshot().operation_active);
  CHECK_FALSE(f.session->GetSnapshot().submission_attempted);
}

TEST_CASE("Request validates its optional output descriptor independently",
          "[kernel][native_frame_execution][native_output_layout]") {
  Fixture f(false, true);
  auto layout = NativeFrameExecutionTestAccess::Layout(
      &f.memory, f.session, f.thread, f.source, f.plan, f.Inputs(),
      f.OutputRecord());
  REQUIRE(layout);
  SECTION("Wrong canonical base") {
    layout->texture_fetch[1] =
        (layout->texture_fetch[1] & 0xFFF) | f.output_address;
  }
  SECTION("Aliased CPU mask misses physical page bias") {
    layout->texture_fetch[1] = (layout->texture_fetch[1] & 0xFFF) |
                               gpu::xenos::CpuToGpu(f.output_address);
  }
  SECTION("Insufficient claimed bytes") { layout->required_bytes -= 4; }
  SECTION("Excess claimed bytes") { layout->required_bytes += 4; }
  SECTION("Wrong shape") { --layout->height; }
  SECTION("Wrong pitch") { layout->pitch += 32; }
  SECTION("Original header changed outside base") {
    (*layout->original_texture_fetch)[3] ^= 2;
  }
  CHECK_FALSE(NativeFrameExecutionTestAccess::Mint(
      f.session, f.thread, f.source, f.plan, f.output, f.Inputs(), f.shaders,
      layout));
  CHECK_FALSE(f.session->GetSnapshot().operation_active);
  CHECK_FALSE(f.session->GetSnapshot().submission_attempted);
}

TEST_CASE("CPU admission models can retain an absent output layout",
          "[kernel][native_frame_execution][native_output_layout]") {
  Fixture f;
  const auto request = f.Mint();
  REQUIRE(request);
  CHECK_FALSE(request->output_layout());
  CHECK(request->IsAdmitted());
}

TEST_CASE(
    "Copied image visibility outlives its retired Source only for a live owner",
    "[kernel][native_frame_execution][native_presentation_owner]") {
  Fixture f;
  gpu::GpuCompletionQueue queue;
  const auto frame = f.SubmitModel(queue);
  CHECK_FALSE(frame.request->IsPresentationOwnerLive());
  f.graph->presentation_allowed = true;
  CHECK(frame.request->IsPresentationOwnerLive());
  REQUIRE(f.session->RetireComplete(f.thread, f.source));
  CHECK_FALSE(frame.request->IsAdmitted());
  CHECK(frame.request->IsPresentationOwnerLive());
  auto update = f.session->BeginUpdate(f.thread);
  REQUIRE(update);
  CHECK(frame.request->IsPresentationOwnerLive());
  f.session->Revoke();
  CHECK_FALSE(frame.request->IsPresentationOwnerLive());
}

TEST_CASE(
    "Presentation owner guard rejects ended generations and reentry revocation",
    "[kernel][native_frame_execution][native_presentation_owner]") {
  Fixture f;
  gpu::GpuCompletionQueue queue;
  const auto frame = f.SubmitModel(queue);
  f.graph->presentation_allowed = true;
  REQUIRE(f.session->RetireComplete(f.thread, f.source));
  SECTION("Binding denies ended foreground generation") {
    f.graph->presentation_allowed = false;
  }
  SECTION("Backend generation changed") { ++f.graph->epoch; }
  SECTION("Runtime generation retired") { f.runtime->RetireAndDrain(); }
  SECTION("Session retired") {
    REQUIRE(f.session->RetireAfterKernelStop([](const auto&) { return true; }));
  }
  SECTION("Revoke during unlocked graph check") {
    f.graph->on_presentation = [&] { f.session->Revoke(); };
  }
  SECTION("Runtime retires during unlocked graph check") {
    f.graph->on_presentation = [&] { f.runtime->RetireAndDrain(); };
  }
  SECTION("Backend changes during unlocked graph check") {
    f.graph->on_presentation = [&] { ++f.graph->epoch; };
  }
  SECTION("Graph check throws") {
    f.graph->on_presentation = [] { throw 1; };
  }
  CHECK_FALSE(frame.request->IsPresentationOwnerLive());
  CHECK_FALSE(f.session->GetSnapshot().operation_active);
  CHECK_FALSE(f.session->GetSnapshot().submission_attempted);
}

TEST_CASE("Completed readback scope blocks source reuse through its GPU copy",
          "[kernel][native_frame_execution][native_readback]") {
  Fixture f;
  gpu::GpuCompletionQueue queue;
  const auto frame = f.SubmitModel(queue);
  SECTION("Submitted source") {}
  SECTION("Already retiring source") {
    REQUIRE(f.session->BeginRetire(f.thread, f.source));
  }
  auto admission = frame.request->AcquireCompletedReadback(frame.completion);
  REQUIRE(admission);
  CHECK(f.session->GetSnapshot().operation_active);
  CHECK_FALSE(f.session->BeginUpdate(f.thread));
  CHECK_FALSE(f.session->BeginRetire(f.thread, f.source));
  CHECK_FALSE(f.session->RetireComplete(f.thread, f.source));
  CHECK_FALSE(
      f.session->RetireAfterKernelStop([](const auto&) { return true; }));
  CHECK_FALSE(frame.request->AcquireCompletedReadback(frame.completion));
  CHECK(admission->Finish(
      gpu::NativeFrameReadbackAdmission::Outcome::kGpuComplete));
  CHECK_FALSE(admission->Finish(
      gpu::NativeFrameReadbackAdmission::Outcome::kGpuUncertain));
  CHECK_FALSE(f.session->GetSnapshot().operation_active);
  admission.reset();  // A finished lease must not quarantine its source.
  REQUIRE(f.session->RetireComplete(f.thread, f.source));
  CHECK(f.session->GetSnapshot().phase == Session::Phase::kBound);
  CHECK(f.session->BeginUpdate(f.thread));
}

TEST_CASE("A canceled readback scope releases source retirement once",
          "[kernel][native_frame_execution][native_readback]") {
  Fixture f;
  gpu::GpuCompletionQueue queue;
  const auto frame = f.SubmitModel(queue);
  auto admission = frame.request->AcquireCompletedReadback(frame.completion);
  REQUIRE(admission);
  CHECK(admission->Finish(
      gpu::NativeFrameReadbackAdmission::Outcome::kCanceledBeforeGpu));
  CHECK_FALSE(admission->Finish(
      gpu::NativeFrameReadbackAdmission::Outcome::kGpuComplete));
  admission.reset();
  CHECK(f.session->RetireComplete(f.thread, f.source));
}

TEST_CASE("A retained completed request cannot read a retired or reused source",
          "[kernel][native_frame_execution][native_readback]") {
  Fixture f;
  gpu::GpuCompletionQueue queue;
  const auto frame = f.SubmitModel(queue);
  REQUIRE(f.session->RetireComplete(f.thread, f.source));
  CHECK_FALSE(frame.request->AcquireCompletedReadback(frame.completion));
  auto update = f.session->BeginUpdate(f.thread);
  REQUIRE(update);
  // Retaining the Request pins this same allocation but does not freeze bytes.
  std::memset(f.memory.TranslatePhysical(f.output.address), 0xA5,
              f.output.length);
  CHECK_FALSE(frame.request->AcquireCompletedReadback(frame.completion));
  const auto replacement = f.session->Freeze(f.thread, update, f.primary);
  REQUIRE(replacement);
  CHECK(replacement->serial() != frame.request->identity().source_serial);
  CHECK_FALSE(frame.request->AcquireCompletedReadback(frame.completion));
}

TEST_CASE("Readback rejects nonnative and mismatched completed tickets",
          "[kernel][native_frame_execution][native_readback]") {
  Fixture f;
  gpu::GpuCompletionQueue queue;
  const auto frame = f.SubmitModel(queue);
  auto candidate = frame.completion;
  gpu::GpuCompletionQueue foreign;
  SECTION("Title ticket") { candidate = queue.TrackSubmitted(1); }
  SECTION("Another backend epoch") {
    candidate = foreign.TrackNativeFrameSubmitted(1, frame.request->identity());
    foreign.CompleteThrough(1);
  }
  SECTION("Another source") {
    auto identity = frame.request->identity();
    ++identity.source_serial;
    candidate = queue.TrackNativeFrameSubmitted(1, identity);
  }
  SECTION("Another submission") {
    candidate = queue.TrackNativeFrameSubmitted(2, frame.request->identity());
    queue.CompleteThrough(2);
  }
  SECTION("Unsupported token") { candidate = {}; }
  CHECK_FALSE(frame.request->AcquireCompletedReadback(candidate));
  CHECK_FALSE(f.session->GetSnapshot().operation_active);
  CHECK(f.session->GetSnapshot().phase == Session::Phase::kSubmitted);
}

TEST_CASE("Readback never accepts a pending or failed native frame",
          "[kernel][native_frame_execution][native_readback]") {
  Fixture f;
  gpu::GpuCompletionQueue queue;
  const auto frame = f.SubmitModel(queue, false);
  SECTION("Pending token") {}
  SECTION("Failed token") { queue.FailPending(); }
  CHECK_FALSE(frame.request->AcquireCompletedReadback(frame.completion));
  CHECK_FALSE(f.session->GetSnapshot().operation_active);
  CHECK_FALSE(f.session->RetireComplete(f.thread, f.source));
}

TEST_CASE("Uncertain or abandoned readback quarantines before source reopening",
          "[kernel][native_frame_execution][native_readback]") {
  Fixture f;
  gpu::GpuCompletionQueue queue;
  auto frame = f.SubmitModel(queue);
  auto admission = frame.request->AcquireCompletedReadback(frame.completion);
  REQUIRE(admission);
  SECTION("Abandoned admission") { admission.reset(); }
  SECTION("Unproven auxiliary GPU work") {
    CHECK_FALSE(admission->Finish(
        gpu::NativeFrameReadbackAdmission::Outcome::kGpuUncertain));
    CHECK_FALSE(admission->Finish(
        gpu::NativeFrameReadbackAdmission::Outcome::kGpuComplete));
    admission.reset();
  }
  CHECK(f.session->GetSnapshot().phase == Session::Phase::kQuarantined);
  CHECK_FALSE(f.session->GetSnapshot().operation_active);
  CHECK_FALSE(f.session->RetireComplete(f.thread, f.source));
  CHECK_FALSE(f.session->BeginUpdate(f.thread));
  CHECK(f.memory.ReleaseOwnedPhysicalAllocation(
            f.session->owner(), f.output_id) == Status::kPinned);
  CHECK_FALSE(f.session->RetireAfterKernelStop(
      [&](const auto&) { return queue.backend_stop_proof().stopped(); }));
  queue.ReleaseAfterGpuStopped();  // Explicit host-model stop only.
  REQUIRE(f.session->RetireAfterKernelStop(
      [&](const auto&) { return queue.backend_stop_proof().stopped(); }));
  frame.request.reset();
  CHECK(f.memory.ReleaseOwnedPhysicalAllocation(
            f.session->owner(), f.output_id) == Status::kSuccess);
}

TEST_CASE("Revocation during readback scope prevents successful completion",
          "[kernel][native_frame_execution][native_readback]") {
  Fixture f;
  gpu::GpuCompletionQueue queue;
  const auto frame = f.SubmitModel(queue);
  auto admission = frame.request->AcquireCompletedReadback(frame.completion);
  REQUIRE(admission);
  std::thread revoke([&] { f.session->Revoke(); });
  revoke.join();
  CHECK(f.session->GetSnapshot().operation_active);
  CHECK_FALSE(
      f.session->RetireAfterKernelStop([](const auto&) { return true; }));
  CHECK_FALSE(admission->Finish(
      gpu::NativeFrameReadbackAdmission::Outcome::kGpuComplete));
  CHECK_FALSE(f.session->GetSnapshot().operation_active);
  CHECK(f.session->GetSnapshot().phase == Session::Phase::kQuarantined);
  CHECK_FALSE(f.session->RetireComplete(f.thread, f.source));
}

TEST_CASE("Readback graph validation runs unlocked under reserved admission",
          "[kernel][native_frame_execution][native_readback]") {
  Fixture f;
  gpu::GpuCompletionQueue queue;
  const auto frame = f.SubmitModel(queue);
  std::atomic<bool> entered{false}, release{false}, acquire_done{false};
  f.graph->on_validate = [&] {
    entered.store(true, std::memory_order_release);
    while (!release.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }
  };
  std::unique_ptr<gpu::NativeFrameReadbackAdmission> admission;
  std::thread acquire([&] {
    admission = frame.request->AcquireCompletedReadback(frame.completion);
    acquire_done.store(true, std::memory_order_release);
  });
  while (!entered.load(std::memory_order_acquire) &&
         !acquire_done.load(std::memory_order_acquire)) {
    std::this_thread::yield();
  }
  if (!entered.load(std::memory_order_acquire)) {
    release.store(true, std::memory_order_release);
    acquire.join();
    f.graph->on_validate = {};
    FAIL("Completed source was rejected before graph validation");
  }
  const bool reserved = f.session->GetSnapshot().operation_active;
  const bool retired = f.session->RetireComplete(f.thread, f.source);
  f.session->Revoke();
  release.store(true, std::memory_order_release);
  acquire.join();
  f.graph->on_validate = {};
  CHECK(reserved);
  CHECK_FALSE(retired);
  CHECK_FALSE(admission);
  CHECK_FALSE(f.session->GetSnapshot().operation_active);
  CHECK(f.session->GetSnapshot().phase == Session::Phase::kQuarantined);
}

TEST_CASE("Readback finish rejects revocation and throwing owner validation",
          "[kernel][native_frame_execution][native_readback]") {
  Fixture f;
  gpu::GpuCompletionQueue queue;
  const auto frame = f.SubmitModel(queue);
  auto admission = frame.request->AcquireCompletedReadback(frame.completion);
  REQUIRE(admission);
  SECTION("Owner validation throws") {
    f.graph->on_validate = [] { throw 1; };
  }
  SECTION("Owner revokes inside unlocked validation") {
    f.graph->on_validate = [&] { f.session->Revoke(); };
  }
  CHECK_FALSE(admission->Finish(
      gpu::NativeFrameReadbackAdmission::Outcome::kGpuComplete));
  f.graph->on_validate = {};
  CHECK_FALSE(f.session->GetSnapshot().operation_active);
  CHECK(f.session->GetSnapshot().phase == Session::Phase::kQuarantined);
}

TEST_CASE("CPU preparation alone does not authorize execution consumption",
          "[kernel][native_frame_execution]") {
  Fixture fixture;
  fixture.graph->execution_allowed = false;
  auto request = fixture.Mint();
  REQUIRE(request);
  CHECK(request->IsAdmitted());
  CHECK_FALSE(request->MarkSubmissionAttempted());
  CHECK_FALSE(fixture.session->GetSnapshot().submission_attempted);
  CHECK_FALSE(request->IsAdmitted());
}

TEST_CASE("Prepared source lookup requires the live owning thread and request",
          "[kernel][native_frame_execution]") {
  Fixture fixture;
  auto request = fixture.Mint();
  REQUIRE(request);
  CHECK(fixture.session->SourceForExecutionRequest(fixture.thread, *request) ==
        fixture.source);
  auto foreign_thread = fixture.thread;
  ++foreign_thread.thread_id;
  CHECK_FALSE(
      fixture.session->SourceForExecutionRequest(foreign_thread, *request));
  auto other = Session::CreateProvisional(fixture.thread, 4, fixture.runtime);
  REQUIRE(other);
  CHECK_FALSE(other->SourceForExecutionRequest(fixture.thread, *request));
  REQUIRE(request->MarkSubmissionAttempted());
  CHECK_FALSE(
      fixture.session->SourceForExecutionRequest(fixture.thread, *request));
}

TEST_CASE("Prepared request retains exact data and backing after CPU abort",
          "[kernel][native_frame_execution]") {
  Fixture fixture(true);
  auto request = fixture.Mint();
  REQUIRE(request);
  REQUIRE(request->IsAdmitted());
  CHECK(request->identity().owner == fixture.session->owner());
  CHECK(request->identity().source_serial == fixture.source->serial());
  CHECK(request->backend_epoch() == 42);
  REQUIRE(request->shaders().size() == 1);
  const auto code = request->shaders()[0].code;
  fixture.shaders[0].code.assign(code.size(), 0);
  CHECK(request->shaders()[0].code == code);
  CHECK_FALSE(fixture.Mint());  // No second authority for the same source.
  REQUIRE(fixture.session->AbortUnsubmitted(fixture.thread, fixture.source));
  CHECK_FALSE(request->IsAdmitted());
  CHECK_FALSE(request->MarkSubmissionAttempted());
  CHECK_FALSE(fixture.session->GetSnapshot().submission_attempted);
  CHECK(fixture.memory.ReleaseOwnedPhysicalAllocation(
            fixture.session->owner(), fixture.record_id) == Status::kPinned);
  request.reset();
  CHECK(fixture.memory.ReleaseOwnedPhysicalAllocation(
            fixture.session->owner(), fixture.record_id) == Status::kSuccess);
}

TEST_CASE("Prepared consumption is one use and makes uncertainty sticky",
          "[kernel][native_frame_execution]") {
  Fixture fixture;
  auto request = fixture.Mint();
  REQUIRE(request);
  REQUIRE(request->MarkSubmissionAttempted());
  CHECK(fixture.session->GetSnapshot().submission_attempted);
  CHECK_FALSE(request->MarkSubmissionAttempted());
  CHECK_FALSE(request->IsAdmitted());
  CHECK_FALSE(
      fixture.session->AbortUnsubmitted(fixture.thread, fixture.source));
  CHECK_FALSE(fixture.session->RetireAfterKernelStop([&](const auto&) {
    return !fixture.session->GetSnapshot().submission_attempted;
  }));
  CHECK(fixture.memory.ReleaseOwnedPhysicalAllocation(
            fixture.session->owner(), fixture.record_id) == Status::kPinned);
  // Only a host-model stopped proof is used here; no GPU was ever created.
  REQUIRE(
      fixture.session->RetireAfterKernelStop([](const auto&) { return true; }));
  CHECK_FALSE(request->IsAdmitted());
  CHECK(fixture.memory.ReleaseOwnedPhysicalAllocation(
            fixture.session->owner(), fixture.record_id) == Status::kPinned);
  request.reset();
  CHECK(fixture.memory.ReleaseOwnedPhysicalAllocation(
            fixture.session->owner(), fixture.record_id) == Status::kSuccess);
}

TEST_CASE("Prepared mint rejects mismatched primary and unowned ranges",
          "[kernel][native_frame_execution]") {
  Fixture fixture;
  auto plan = fixture.plan;
  auto output = fixture.output;
  auto inputs = fixture.Inputs();
  SECTION("another owned plan is not this frozen primary") {
    auto different = fixture.primary;
    different.push_back(0x80);
    different.insert(different.end(), 3, 0);
    const auto analyzed = Plan::Analyze(different);
    REQUIRE(analyzed);
    plan = analyzed.plan;
  }
  SECTION("output is not covered by actual pins") { output = {0x1000, 4}; }
  SECTION("input is not covered by actual pins") {
    inputs.push_back({0x1000, 4});
  }
  SECTION("input overlaps output") { inputs.push_back(output); }
  SECTION("missing backend proof") { fixture.graph->epoch = 0; }
  CHECK_FALSE(NativeFrameExecutionTestAccess::Mint(
      fixture.session, fixture.thread, fixture.source, plan, output, inputs));
  CHECK_FALSE(fixture.session->GetSnapshot().submission_attempted);
}

TEST_CASE("Prepared shaders cannot change inline source or metadata stage",
          "[kernel][native_frame_execution]") {
  Fixture fixture(true);
  SECTION("code is not the frozen inline source") {
    fixture.shaders[0].code[0] ^= 1;
  }
  SECTION("metadata stage differs from selected load") {
    fixture.shaders[0].metadata.stage = gpu::xenos::ShaderType::kPixel;
  }
  SECTION("metadata never analyzed") {
    fixture.shaders[0].metadata.metadata_analyzed = false;
  }
  CHECK_FALSE(fixture.Mint());
}

TEST_CASE("Revocation cancels prepared admission without releasing backing",
          "[kernel][native_frame_execution]") {
  Fixture fixture;
  auto request = fixture.Mint();
  REQUIRE(request);
  fixture.session->Revoke();
  CHECK_FALSE(request->IsAdmitted());
  CHECK_FALSE(request->MarkSubmissionAttempted());
  CHECK(fixture.memory.ReleaseOwnedPhysicalAllocation(
            fixture.session->owner(), fixture.record_id) == Status::kPinned);
}

TEST_CASE("Prepared epoch callbacks run unlocked and revocation defeats mint",
          "[kernel][native_frame_execution]") {
  Fixture fixture;
  bool called = false;
  fixture.graph->on_epoch = [&] {
    called = true;
    CHECK(fixture.session->GetSnapshot().operation_active);
    CHECK_FALSE(fixture.session->RetireAfterKernelStop(
        [](const auto&) { return true; }));
    fixture.session->Revoke();
  };
  CHECK_FALSE(fixture.Mint());
  CHECK(called);
  CHECK(fixture.session->GetSnapshot().phase == Session::Phase::kQuarantined);
  CHECK_FALSE(fixture.session->GetSnapshot().submission_attempted);
}

TEST_CASE("Replacement backend cannot re-admit an old prepared request",
          "[kernel][native_frame_execution]") {
  Fixture fixture;
  auto request = fixture.Mint();
  REQUIRE(request);
  fixture.graph->epoch = 43;
  CHECK_FALSE(request->IsAdmitted());
  fixture.graph->epoch = 42;
  CHECK_FALSE(request->IsAdmitted());
  CHECK_FALSE(request->MarkSubmissionAttempted());
}

TEST_CASE("Revocation during consumption cannot mark submission attempted",
          "[kernel][native_frame_execution]") {
  Fixture fixture;
  const auto request = fixture.Mint();
  REQUIRE(request);
  fixture.graph->on_epoch = [&] {
    CHECK(fixture.session->GetSnapshot().operation_active);
    fixture.session->Revoke();
  };
  CHECK_FALSE(request->MarkSubmissionAttempted());
  CHECK_FALSE(fixture.session->GetSnapshot().submission_attempted);
  CHECK(fixture.session->GetSnapshot().phase == Session::Phase::kQuarantined);
  CHECK_FALSE(request->IsAdmitted());
}

TEST_CASE("Shared prepared handles have one consumer across host workers",
          "[kernel][native_frame_execution]") {
  Fixture fixture;
  const auto request = fixture.Mint();
  REQUIRE(request);
  std::atomic<bool> start{false};
  std::atomic<unsigned> consumed{0};
  const auto consume = [&] {
    while (!start.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }
    if (request->MarkSubmissionAttempted()) {
      consumed.fetch_add(1, std::memory_order_relaxed);
    }
  };
  std::thread first(consume), second(consume);
  start.store(true, std::memory_order_release);
  first.join();
  second.join();
  CHECK(consumed.load(std::memory_order_relaxed) == 1);
  CHECK(fixture.session->GetSnapshot().submission_attempted);
  CHECK_FALSE(request->IsAdmitted());
}

TEST_CASE("An old prepared source cannot consume a later frame",
          "[kernel][native_frame_execution]") {
  Fixture fixture;
  const auto old = fixture.Mint();
  REQUIRE(old);
  REQUIRE(fixture.session->AbortUnsubmitted(fixture.thread, fixture.source));
  fixture.Freeze();
  const auto fresh = fixture.Mint();
  REQUIRE(fresh);
  CHECK(fresh->identity().source_serial != old->identity().source_serial);
  CHECK_FALSE(old->MarkSubmissionAttempted());
  CHECK(fresh->IsAdmitted());
  CHECK(fresh->MarkSubmissionAttempted());
}

TEST_CASE(
    "PM4 storage origin uses pinned rectangle and actual biased heap mapping",
    "[kernel][native_frame_execution][native_pm4_layout]") {
  Fixture f(false, true);
  const auto record = f.Pm4Record();
  const auto descriptor = f.Descriptor(record);
  const auto layout = f.Pm4Layout(record, descriptor);
  REQUIRE(layout);
  CHECK(layout->origin == gpu::NativeFrameOutputLayout::Origin::kPm4Resolve);
  CHECK_FALSE(layout->original_texture_fetch);
  REQUIRE(layout->pm4_resolve);
  CHECK(layout->width == 640);
  CHECK(layout->height == 480);
  CHECK(layout->pitch == 640);
  CHECK(layout->required_bytes == 640 * 480 * 4);
  CHECK(layout->pm4_resolve->descriptor() == descriptor);
  CHECK(layout->pm4_resolve->output_mapping().observed == f.output_address);
  CHECK(layout->pm4_resolve->output_mapping().physical == f.output.address);
  // Real E/F heap translation includes its page bias. A simple mask is wrong.
  CHECK((f.output_address & 0x1FFFFFFF) != f.output.address);
  gpu::xenos::xe_gpu_texture_fetch_t fetch{};
  std::memcpy(&fetch, layout->texture_fetch.data(), sizeof(fetch));
  CHECK((uint32_t(fetch.base_address) << 12) == f.output.address);
  const auto request = NativeFrameExecutionTestAccess::Mint(
      f.session, f.thread, f.source, f.plan, f.output, f.Inputs(), f.shaders,
      layout);
  REQUIRE(request);
  CHECK(request->IsAdmitted());
  CHECK_FALSE(f.session->GetSnapshot().submission_attempted);
}

TEST_CASE(
    "PM4 visible geometry supports independent valid source and storage "
    "pitches",
    "[kernel][native_frame_execution][native_pm4_layout]") {
  // Host-only geometry fixtures, not claimed as captured guest vertex bytes.
  // Two shapes ensure this is not an alternative 852-wide hardcoded profile.
  const std::array<OutputGeometry, 2> geometries{
      {{852, 480, 880, 864, 856, 2 * 1024 * 1024},
       {800, 448, 800, 832, 800, 2 * 1024 * 1024}}};
  for (const auto& geometry : geometries) {
    CAPTURE(geometry.width, geometry.height);
    Fixture f(false, true, geometry);
    const auto record = f.Pm4Record();
    const auto layout = f.Pm4Layout(record, f.Descriptor(record));
    REQUIRE(layout);
    CHECK(layout->width == geometry.width);
    CHECK(layout->height == geometry.height);
    CHECK(layout->pitch == geometry.destination_pitch);
    CHECK(gpu::ValidateNativeFrameOutputStorage(*layout, f.output));
    gpu::xenos::xe_gpu_texture_fetch_t fetch{};
    std::memcpy(&fetch, layout->texture_fetch.data(), sizeof(fetch));
    CHECK(uint32_t(fetch.size_2d.width) + 1 == geometry.width);
    CHECK((uint32_t(fetch.pitch) << 5) == geometry.destination_pitch);
    CHECK(NativeFrameExecutionTestAccess::Mint(f.session, f.thread, f.source,
                                               f.plan, f.output, f.Inputs(),
                                               f.shaders, layout));
    CHECK_FALSE(f.session->GetSnapshot().submission_attempted);
  }
}

TEST_CASE("PM4 logical and native padded rectangles agree with real resolve",
          "[kernel][native_frame_execution][native_pm4_layout]") {
  // Existing exact-logical fixtures remain a legitimate public-ISA profile.
  // Native D3D emits ceil8 endpoints instead. Exercise both forms on either
  // side of 8/32 boundaries, keeping the descriptor and fetch logical size.
  const std::array<OutputGeometry, 5> shapes{
      {{852, 480, 880, 864, 856, 2 * 1024 * 1024},
       {7, 9, 80, 32, 8, 0x10000},
       {8, 32, 80, 32, 8, 0x10000},
       {9, 33, 80, 32, 16, 0x10000},
       {33, 41, 80, 64, 40, 0x10000}}};
  for (const auto& shape : shapes) {
    for (bool native_padded : {false, true}) {
      CAPTURE(shape.width, shape.height, native_padded);
      Fixture f(false, true, shape);
      const uint32_t right = (shape.width + 7) & ~7u;
      const uint32_t bottom = (shape.height + 7) & ~7u;
      // Match the producer's temporary padded scissor in the new form.
      if (native_padded) {
        f.RewriteRegister(gpu::XE_GPU_REG_PA_SC_WINDOW_SCISSOR_BR,
                          (bottom << 16) | right);
        WriteResolveRectangle(f, float(right), float(bottom));
      }
      const auto actual = CommonResolveInfo(f);
      CHECK(actual.coordinate_info.width_div_8 * 8 == right);
      CHECK(actual.height_div_8 * 8 == bottom);
      CHECK(actual.copy_dest_coordinate_info.pitch_aligned_div_32 * 32 ==
            shape.destination_pitch);
      CHECK(actual.copy_dest_extent_start == f.output.address);
      REQUIRE(actual.copy_dest_extent_length > 0);
      REQUIRE(actual.copy_dest_extent_length <= f.output.length);
      const auto record = f.Pm4Record();
      const auto layout = f.Pm4Layout(record, f.Descriptor(record));
      REQUIRE(layout);
      CHECK(layout->width == shape.width);
      CHECK(layout->height == shape.height);
      CHECK(layout->pitch == shape.destination_pitch);
      CHECK(layout->required_bytes >= actual.copy_dest_extent_length);
      CHECK(gpu::ValidateNativeFrameOutputStorage(*layout, f.output));
      gpu::xenos::xe_gpu_texture_fetch_t fetch;
      std::memcpy(&fetch, layout->texture_fetch.data(), sizeof(fetch));
      CHECK(uint32_t(fetch.size_2d.width) + 1 == shape.width);
      CHECK(uint32_t(fetch.size_2d.height) + 1 == shape.height);
      CHECK(NativeFrameExecutionTestAccess::Mint(f.session, f.thread, f.source,
                                                 f.plan, f.output, f.Inputs(),
                                                 f.shaders, layout));
      CHECK_FALSE(f.session->GetSnapshot().submission_attempted);
    }
  }
}

TEST_CASE("PM4 native padding requires full owned write coverage",
          "[kernel][native_frame_execution][native_pm4_layout]") {
  Fixture f(false, true, {852, 480, 880, 864, 856, 2 * 1024 * 1024});
  WriteResolveRectangle(f, 856, 480);
  const auto actual = CommonResolveInfo(f);
  REQUIRE(actual.copy_dest_extent_length == 1658880);
  auto record = f.Pm4Record();
  // Same real allocation/pins; constrain the claimed usable output range at
  // the exact hardware footprint rather than assuming all 2MiB are available.
  record.output_allocation_bytes = actual.copy_dest_extent_length;
  const auto layout = f.Pm4Layout(record, f.Descriptor(record));
  REQUIRE(layout);
  CHECK(layout->required_bytes == 1658880);
  CHECK(layout->width == 852);
  CHECK(layout->height == 480);
  --record.output_allocation_bytes;
  CHECK_FALSE(f.Pm4Layout(record, f.Descriptor(record)));
  CHECK_FALSE(f.session->GetSnapshot().submission_attempted);
}

TEST_CASE("PM4 padding rejects unproven endpoints and source clipping",
          "[kernel][native_frame_execution][native_pm4_layout]") {
  Fixture f(false, true, {852, 481, 880, 864, 856, 2 * 1024 * 1024});
  float right = 856, bottom = 488;
  bool source_clipped = false;
  SECTION("unproven endpoint within the same 8-pixel bucket") { right = 853; }
  SECTION("under-resolve right") { right = 848; }
  SECTION("over-resolve right") { right = 864; }
  SECTION("under-resolve bottom") { bottom = 480; }
  SECTION("over-resolve bottom") { bottom = 496; }
  SECTION("fractional exclusive edge must not be naively ceiled") {
    right = 848.5f;
  }
  SECTION("source pitch covers logical but clips its effective padding") {
    source_clipped = true;
    f.RewriteRegister(gpu::XE_GPU_REG_RB_SURFACE_INFO, (864u << 18) | 853);
  }
  SECTION("scissor clips logical width") {
    f.RewriteRegister(gpu::XE_GPU_REG_PA_SC_WINDOW_SCISSOR_BR,
                      (488u << 16) | 848);
  }
  SECTION("scissor clips logical height") {
    f.RewriteRegister(gpu::XE_GPU_REG_PA_SC_WINDOW_SCISSOR_BR,
                      (480u << 16) | 856);
  }
  WriteResolveRectangle(f, right, bottom);
  if (source_clipped) {
    const auto actual = CommonResolveInfo(f);
    CHECK(actual.coordinate_info.width_div_8 * 8 == 848);
  }
  const auto record = f.Pm4Record();
  CHECK_FALSE(f.Pm4Layout(record, f.Descriptor(record)));
  CHECK_FALSE(f.session->GetSnapshot().submission_attempted);
}

TEST_CASE("PM4 padded output keeps the actual sealed rectangle unchanged",
          "[kernel][native_frame_execution][native_pm4_layout][native_input_"
          "snapshot]") {
  Fixture f(false, true, {852, 481, 880, 864, 856, 2 * 1024 * 1024});
  f.RewriteRegister(gpu::XE_GPU_REG_PA_SC_WINDOW_SCISSOR_BR,
                    (488u << 16) | 856);
  WriteResolveRectangle(f, 856, 488);
  std::array<uint8_t, 24> sealed_rectangle;
  std::memcpy(sealed_rectangle.data(),
              f.memory.TranslatePhysical(f.record.address + 0x100),
              sealed_rectangle.size());
  auto capture = f.CaptureInputs();
  REQUIRE(capture);
  REQUIRE(capture->Capture(f.record));
  const auto snapshot = capture->Seal();
  REQUIRE(snapshot);
  capture.reset();
  const auto record = f.Pm4Record();
  const auto descriptor = f.Descriptor(record);
  WriteResolveRectangle(f, 864, 496);  // Mutable RAM is now a different shape.
  CHECK_FALSE(f.Pm4Layout(record, descriptor));
  const auto layout = NativeFrameExecutionTestAccess::Pm4Layout(
      &f.memory, f.session, f.thread, f.source, f.plan, f.Inputs(), record,
      descriptor, snapshot);
  REQUIRE(layout);
  REQUIRE(layout->pm4_resolve);
  CHECK(std::memcmp(layout->pm4_resolve->rectangle_words().data(),
                    sealed_rectangle.data(), sealed_rectangle.size()) == 0);
  CHECK(layout->width == 852);
  CHECK(layout->height == 481);
  CHECK(NativeFrameExecutionTestAccess::Mint(f.session, f.thread, f.source,
                                             f.plan, f.output, f.Inputs(),
                                             f.shaders, layout, snapshot));
  CHECK_FALSE(f.session->GetSnapshot().submission_attempted);
}

TEST_CASE("PM4 output proof uses final-copy state before terminal restoration",
          "[kernel][native_frame_execution][native_pm4_layout]") {
  // Real Memory/Session ownership with public-ISA geometry, not captured guest
  // rectangle bytes and not a Kernel, native Task or GPU execution fixture.
  Fixture f(false, true, {852, 480, 880, 864, 856, 2 * 1024 * 1024});
  std::vector<uint32_t> words(f.plan->words().begin(), f.plan->words().end());
  bool valid_copy = true;
  uint32_t offset = 0, tl = 0, br = 0x03D40354;
  SECTION("native-style restored window differs from resolve coverage") {}
  SECTION("all legal terminal window fields remain isolated") {
    offset = (uint32_t(-7) & 0x7FFF) | (3u << 16);
    tl = 0x80000000 | 5 | (9u << 16);
    br = 0;
  }
  SECTION("valid tail cannot repair insufficient final-copy scissor") {
    valid_copy = false;
    for (const auto& write : f.plan->register_writes()) {
      if (write.index == gpu::XE_GPU_REG_PA_SC_WINDOW_SCISSOR_BR) {
        const auto first = words[write.packet_dword_offset] & 0x7FFF;
        words[write.packet_dword_offset + 1 + write.index - first] =
            (480u << 16) | 800;
      }
    }
  }
  words.insert(words.end(),
               {0xC0004600, uint32_t(gpu::xenos::CACHE_FLUSH), 0x00022080,
                offset, tl, br, uint32_t(gpu::XE_GPU_REG_RB_COPY_CONTROL), 0,
                uint32_t(gpu::XE_GPU_REG_RB_COPY_DEST_INFO), 0});
  REQUIRE(f.session->AbortUnsubmitted(f.thread, f.source));
  f.primary = Bytes(words);
  REQUIRE(f.primary.size() <= 0x100);  // Rectangle begins at record+0x100.
  std::memcpy(f.memory.TranslatePhysical(f.record.address), f.primary.data(),
              f.primary.size());
  const auto analyzed = Plan::Analyze(f.primary);
  REQUIRE(analyzed);
  f.plan = analyzed.plan;
  f.Freeze();
  const auto record = f.Pm4Record();
  const auto layout = f.Pm4Layout(record, f.Descriptor(record));
  if (!valid_copy) {
    CHECK_FALSE(layout);
    return;
  }
  REQUIRE(layout);
  CHECK(layout->width == 852);
  CHECK(layout->height == 480);
  CHECK(layout->pitch == 864);
  REQUIRE(layout->pm4_resolve);
  CHECK(layout->pm4_resolve->copy_packet_dword_offset() ==
        f.plan->inventory().draws.back().packet_dword_offset);
  const auto request = NativeFrameExecutionTestAccess::Mint(
      f.session, f.thread, f.source, f.plan, f.output, f.Inputs(), f.shaders,
      layout);
  REQUIRE(request);
  CHECK(request->IsAdmitted());
  CHECK_FALSE(f.session->GetSnapshot().submission_attempted);
}

TEST_CASE(
    "Dynamic PM4 output rejects mismatched geometry and insufficient storage",
    "[kernel][native_frame_execution][native_pm4_layout]") {
  OutputGeometry geometry{852, 480, 880, 864, 856, 2 * 1024 * 1024};
  SECTION("source pitch smaller than visible width") {
    geometry.source_pitch = 848;
  }
  SECTION("destination pitch smaller than visible width") {
    geometry.destination_pitch = 832;
  }
  SECTION("destination pitch not tile aligned") {
    geometry.destination_pitch = 855;
  }
  SECTION("window clips visible width") { geometry.window_width = 848; }
  SECTION("actual allocation too small") { geometry.allocation_bytes = 0x1000; }
  Fixture f(false, true, geometry);
  const auto record = f.Pm4Record();
  CHECK_FALSE(f.Pm4Layout(record, f.Descriptor(record)));
  CHECK_FALSE(f.session->GetSnapshot().submission_attempted);
}

TEST_CASE(
    "Dynamic PM4 output never substitutes a crop for the sealed rectangle",
    "[kernel][native_frame_execution][native_pm4_layout]") {
  Fixture f(false, true, {852, 480, 880, 864, 856, 2 * 1024 * 1024});
  const auto record = f.Pm4Record();
  auto descriptor = f.Descriptor(record);
  SECTION("cropped visible descriptor") {
    Fixture::DescriptorWord(descriptor, 20, 851);
  }
  SECTION("larger visible descriptor") {
    Fixture::DescriptorWord(descriptor, 20, 853);
  }
  CHECK_FALSE(f.Pm4Layout(record, descriptor));
}

TEST_CASE(
    "Shared output storage validation rejects forged fetch and footprint "
    "metadata",
    "[kernel][native_frame_execution][native_pm4_layout]") {
  Fixture f(false, true, {852, 480, 880, 864, 856, 2 * 1024 * 1024});
  const auto record = f.Pm4Record();
  auto layout = f.Pm4Layout(record, f.Descriptor(record));
  REQUIRE(layout);
  REQUIRE(gpu::ValidateNativeFrameOutputStorage(*layout, f.output));
  SECTION("zero dimension") { layout->width = 0; }
  SECTION("unrepresentable dimension") { layout->height = 8193; }
  SECTION("unrepresentable pitch") { layout->pitch = 16384; }
  SECTION("truncated footprint") { --layout->required_bytes; }
  SECTION("wrong fetch size") { --layout->texture_fetch[2]; }
  SECTION("wrong output swizzle") { layout->texture_fetch[3] ^= 2; }
  SECTION("fabricated original XG claim") {
    layout->origin = gpu::NativeFrameOutputLayout::Origin::kOriginalXgHeader;
    layout->original_texture_fetch = layout->texture_fetch;
    layout->pm4_resolve.reset();
  }
  CHECK_FALSE(gpu::ValidateNativeFrameOutputStorage(*layout, f.output));
}

TEST_CASE(
    "Dynamic output geometry uses the same sealed snapshot after original "
    "mutation",
    "[kernel][native_frame_execution][native_pm4_layout][native_input_"
    "snapshot]") {
  Fixture f(false, true, {852, 480, 880, 864, 856, 2 * 1024 * 1024});
  auto capture = f.CaptureInputs();
  REQUIRE(capture);
  REQUIRE(capture->Capture(f.record));
  const auto snapshot = capture->Seal();
  REQUIRE(snapshot);
  capture.reset();
  const auto record = f.Pm4Record();
  const auto descriptor = f.Descriptor(record);
  std::memset(f.memory.TranslatePhysical(f.record.address + 0x100), 0xFF, 24);
  CHECK_FALSE(f.Pm4Layout(record, descriptor));
  const auto layout = NativeFrameExecutionTestAccess::Pm4Layout(
      &f.memory, f.session, f.thread, f.source, f.plan, f.Inputs(), record,
      descriptor, snapshot);
  REQUIRE(layout);
  CHECK(layout->width == 852);
  CHECK(layout->pitch == 864);
  CHECK(NativeFrameExecutionTestAccess::Mint(f.session, f.thread, f.source,
                                             f.plan, f.output, f.Inputs(),
                                             f.shaders, layout, snapshot));
}

TEST_CASE("PM4 descriptor supports a distinct exact canonical physical branch",
          "[kernel][native_frame_execution][native_pm4_layout]") {
  Fixture f(false, true);
  const auto record = f.Pm4Record(true);
  const auto layout = f.Pm4Layout(record, f.Descriptor(record, true));
  REQUIRE(layout);
  using Kind = gpu::NativeFramePm4ResolveProof::Mapping::Kind;
  CHECK(layout->pm4_resolve->primary_mapping().kind ==
        Kind::kCanonicalPhysical);
  CHECK(layout->pm4_resolve->output_mapping().kind == Kind::kCanonicalPhysical);
  CHECK_FALSE(layout->original_texture_fetch);
}

TEST_CASE(
    "PM4 descriptor must match actual source output and rectangle dimensions",
    "[kernel][native_frame_execution][native_pm4_layout]") {
  Fixture f(false, true);
  auto record = f.Pm4Record();
  auto descriptor = f.Descriptor(record);
  SECTION("empty descriptor") { descriptor = {}; }
  SECTION("another command source") {
    Fixture::DescriptorWord(descriptor, 0, f.record_address + 4);
  }
  SECTION("another word count") {
    Fixture::DescriptorWord(descriptor, 4, uint32_t(f.primary.size() / 4 + 1));
  }
  SECTION("format differs") { Fixture::DescriptorWord(descriptor, 12, 7); }
  SECTION("width cannot come from allocation size") {
    Fixture::DescriptorWord(descriptor, 20, 1280);
  }
  SECTION("height differs") { Fixture::DescriptorWord(descriptor, 24, 479); }
  SECTION("masked alias misses physical heap bias") {
    record.output_cpu_address = f.output_address & 0x1FFFFFFF;
    Fixture::DescriptorWord(descriptor, 8, record.output_cpu_address);
  }
  SECTION("raw alias disagrees with observed producer metadata") {
    record.output_cpu_address += 0x1000;
  }
  SECTION("native recording End not completed") {
    record.native_end_completed = false;
  }
  CHECK_FALSE(f.Pm4Layout(record, descriptor));
  CHECK_FALSE(f.session->GetSnapshot().submission_attempted);
}

TEST_CASE(
    "PM4 snapshot reads actual pinned resolve vertices rather than a shape "
    "flag",
    "[kernel][native_frame_execution][native_pm4_layout]") {
  Fixture f(false, true);
  const auto record = f.Pm4Record();
  const auto descriptor = f.Descriptor(record);
  REQUIRE(f.Pm4Layout(record, descriptor));
  std::array<uint32_t, 6> rectangle;
  std::memcpy(rectangle.data(),
              f.memory.TranslatePhysical(f.record.address + 0x100), 24);
  SECTION("nonfinite vertex") {
    rectangle[0] = gpu::xenos::GpuSwap(0x7FC00000u, gpu::xenos::Endian::k8in32);
  }
  SECTION("shifted origin") {
    rectangle[0] = gpu::xenos::GpuSwap(std::bit_cast<uint32_t>(0.5f),
                                       gpu::xenos::Endian::k8in32);
    rectangle[2] = rectangle[0];
  }
  SECTION("smaller bounds") {
    rectangle[2] = gpu::xenos::GpuSwap(std::bit_cast<uint32_t>(638.5f),
                                       gpu::xenos::Endian::k8in32);
    rectangle[4] = rectangle[2];
  }
  std::memcpy(f.memory.TranslatePhysical(f.record.address + 0x100),
              rectangle.data(), 24);
  CHECK_FALSE(f.Pm4Layout(record, descriptor));
}

TEST_CASE("PM4 layout requires inventoried owner-pinned rectangle and output",
          "[kernel][native_frame_execution][native_pm4_layout]") {
  Fixture f(false, true);
  auto record = f.Pm4Record();
  auto descriptor = f.Descriptor(record);
  auto inputs = f.Inputs();
  SECTION("rectangle missing from bounded input inventory") {
    inputs.pop_back();
  }
  SECTION("output has no actual owner backing pin") {
    record.output_physical_address = 0x1000;
    record.output_cpu_address = 0x1000;
    Fixture::DescriptorWord(descriptor, 8, 0x1000);
  }
  CHECK_FALSE(NativeFrameExecutionTestAccess::Pm4Layout(
      &f.memory, f.session, f.thread, f.source, f.plan, inputs, record,
      descriptor));
}

TEST_CASE("PM4 origin rechecks immutable final-copy state",
          "[kernel][native_frame_execution][native_pm4_layout]") {
  Fixture f(false, true);
  SECTION("destination differs from authoritative output") {
    f.RewriteRegister(gpu::XE_GPU_REG_RB_COPY_DEST_BASE,
                      f.output.address + 0x1000);
  }
  SECTION("wrong source pitch") {
    f.RewriteRegister(gpu::XE_GPU_REG_RB_SURFACE_INFO, 0x0A000260);
  }
  SECTION("different byte order") {
    f.RewriteRegister(gpu::XE_GPU_REG_RB_COPY_DEST_INFO, 0x01000301);
  }
  SECTION("missing inverse red blue mapping") {
    f.RewriteRegister(gpu::XE_GPU_REG_RB_COPY_DEST_INFO, 0x00000300);
  }
  SECTION("different destination pitch") {
    f.RewriteRegister(gpu::XE_GPU_REG_RB_COPY_DEST_PITCH, 0x01E00260);
  }
  const auto record = f.Pm4Record();
  CHECK_FALSE(f.Pm4Layout(record, f.Descriptor(record)));
}

TEST_CASE(
    "PM4 origin cannot be replaced with caller-constructed fetch metadata",
    "[kernel][native_frame_execution][native_pm4_layout]") {
  Fixture f(false, true);
  const auto record = f.Pm4Record();
  auto layout = f.Pm4Layout(record, f.Descriptor(record));
  REQUIRE(layout);
  SECTION("unspecified origin") {
    layout->origin = gpu::NativeFrameOutputLayout::Origin::kUnspecified;
  }
  SECTION("original header claim with no original") {
    layout->origin = gpu::NativeFrameOutputLayout::Origin::kOriginalXgHeader;
  }
  SECTION("invented original header") {
    layout->original_texture_fetch = layout->texture_fetch;
  }
  SECTION("missing sealed mapping proof") { layout->pm4_resolve.reset(); }
  SECTION("tampered canonical host fetch") { layout->texture_fetch[3] ^= 2; }
  SECTION("tampered required span") { --layout->required_bytes; }
  CHECK_FALSE(NativeFrameExecutionTestAccess::Mint(
      f.session, f.thread, f.source, f.plan, f.output, f.Inputs(), f.shaders,
      layout));
}

TEST_CASE("PM4 sealed proof cannot be replayed for a later Source",
          "[kernel][native_frame_execution][native_pm4_layout]") {
  Fixture f(false, true);
  auto record = f.Pm4Record();
  const auto old_layout = f.Pm4Layout(record, f.Descriptor(record));
  REQUIRE(old_layout);
  REQUIRE(f.session->AbortUnsubmitted(f.thread, f.source));
  f.Freeze();
  CHECK_FALSE(NativeFrameExecutionTestAccess::Mint(
      f.session, f.thread, f.source, f.plan, f.output, f.Inputs(), f.shaders,
      old_layout));
  record = f.Pm4Record();
  const auto fresh = f.Pm4Layout(record, f.Descriptor(record));
  REQUIRE(fresh);
  CHECK(fresh->pm4_resolve->identity().source_serial !=
        old_layout->pm4_resolve->identity().source_serial);
  CHECK(NativeFrameExecutionTestAccess::Mint(f.session, f.thread, f.source,
                                             f.plan, f.output, f.Inputs(),
                                             f.shaders, fresh));
}

TEST_CASE("Genuine diagnostic XG provenance remains separate from PM4 storage",
          "[kernel][native_frame_execution][native_pm4_layout]") {
  Fixture f(false, true);
  const auto genuine = f.OutputRecord();
  const auto diagnostic = NativeFrameExecutionTestAccess::Layout(
      &f.memory, f.session, f.thread, f.source, f.plan, f.Inputs(), genuine);
  REQUIRE(diagnostic);
  CHECK(diagnostic->origin ==
        gpu::NativeFrameOutputLayout::Origin::kOriginalXgHeader);
  CHECK(diagnostic->original_texture_fetch == genuine.output_texture_fetch);
  CHECK_FALSE(diagnostic->pm4_resolve);
  const auto without_header = f.Pm4Record();
  CHECK_FALSE(NativeFrameExecutionTestAccess::Layout(
      &f.memory, f.session, f.thread, f.source, f.plan, f.Inputs(),
      without_header));
  CHECK(f.Pm4Layout(without_header, f.Descriptor(without_header)));
}

TEST_CASE("Sealed native inputs survive actual original mutation and release",
          "[kernel][native_frame_execution][native_input_snapshot]") {
  Fixture f(false, true);
  auto capture = f.CaptureInputs();
  REQUIRE(capture);
  REQUIRE(capture->Capture(f.record));
  const auto snapshot = capture->Seal();
  REQUIRE(snapshot);
  capture.reset();
  CHECK(snapshot->valid());
  CHECK(snapshot->identity() == gpu::NativeFrameSubmissionIdentity{
                                    f.session->owner(), f.source->serial()});
  std::array<uint8_t, 24> before, after;
  REQUIRE(snapshot->Read({f.record.address + 0x100, 24}, before));
  std::memset(f.memory.TranslatePhysical(f.record.address), 0xE5,
              f.record.length);
  REQUIRE(snapshot->Read({f.record.address + 0x100, 24}, after));
  CHECK(before == after);
  CHECK(f.memory.ReleaseOwnedPhysicalAllocation(
            f.session->owner(), f.record_id) == Status::kPinned);
  REQUIRE(f.session->AbortUnsubmitted(f.thread, f.source));
  CHECK(f.memory.ReleaseOwnedPhysicalAllocation(
            f.session->owner(), f.record_id) == Status::kSuccess);
  REQUIRE(snapshot->Read({f.record.address + 0x100, 24}, after));
  CHECK(before == after);
  CHECK(
      snapshot->valid());  // No live Memory pointer/physical pin in byte view.
}

TEST_CASE("Overlapping native captures reuse one observation for every byte",
          "[kernel][native_frame_execution][native_input_snapshot]") {
  Fixture f;
  auto* data = f.memory.TranslatePhysical<uint8_t*>(f.record.address + 0x100);
  for (uint32_t i = 0; i < 24; ++i) {
    data[i] = uint8_t(i);
  }
  auto capture = f.CaptureInputs();
  REQUIRE(capture);
  REQUIRE(capture->Capture({f.record.address + 0x100, 16}));
  std::memset(data + 8, 0xA7, 16);
  REQUIRE(capture->Capture({f.record.address + 0x108, 16}));
  const auto snapshot = capture->Seal();
  REQUIRE(snapshot);
  std::array<uint8_t, 24> bytes;
  REQUIRE(snapshot->Read({f.record.address + 0x100, 24}, bytes));
  for (uint32_t i = 0; i < 16; ++i) {
    CHECK(bytes[i] == i);
  }
  for (uint32_t i = 16; i < 24; ++i) {
    CHECK(bytes[i] == 0xA7);
  }
  for (size_t i = 1; i < snapshot->blocks().size(); ++i) {
    CHECK(uint64_t(snapshot->blocks()[i - 1].range.address) +
              snapshot->blocks()[i - 1].range.length <=
          snapshot->blocks()[i].range.address);
  }
}

TEST_CASE(
    "Snapshot reads reject holes and invalid extents without partial writes",
    "[kernel][native_frame_execution][native_input_snapshot]") {
  Fixture f;
  auto capture = f.CaptureInputs();
  REQUIRE(capture);
  REQUIRE(capture->Capture({f.record.address + 0x100, 4}));
  REQUIRE(capture->Capture({f.record.address + 0x108, 4}));
  const auto snapshot = capture->Seal();
  REQUIRE(snapshot);
  std::array<uint8_t, 12> bytes;
  bytes.fill(0xA5);
  CHECK_FALSE(snapshot->Covers({f.record.address + 0x100, 12}));
  CHECK_FALSE(snapshot->Read({f.record.address + 0x100, 12}, bytes));
  for (const auto byte : bytes) {
    CHECK(byte == 0xA5);
  }
  CHECK_FALSE(snapshot->Covers({0x1FFFFFFF, 2}));
  CHECK_FALSE(snapshot->Covers({0, 0}));
  CHECK_FALSE(snapshot->Covers({0, 4 * 1024 * 1024 + 1}));
}

TEST_CASE("Snapshot capture holds the real Source operation and rejects revoke",
          "[kernel][native_frame_execution][native_input_snapshot]") {
  Fixture f;
  auto capture = f.CaptureInputs();
  REQUIRE(capture);
  CHECK(f.session->GetSnapshot().operation_active);
  CHECK_FALSE(f.session->AbortUnsubmitted(f.thread, f.source));
  CHECK_FALSE(f.session->BeginUpdate(f.thread));
  f.session->Revoke();
  CHECK_FALSE(capture->Capture({f.record.address + 0x100, 24}));
  std::array<uint8_t, 4> bytes;
  CHECK_FALSE(capture->Read({f.record.address, 4}, bytes));
  CHECK_FALSE(capture->Seal());
  capture.reset();
  CHECK_FALSE(f.session->GetSnapshot().operation_active);
  CHECK(f.memory.ReleaseOwnedPhysicalAllocation(
            f.session->owner(), f.record_id) == Status::kPinned);
}

TEST_CASE(
    "Snapshot capture accepts only actual owner pinned physical allocations",
    "[kernel][native_frame_execution][native_input_snapshot]") {
  Fixture f;
  auto capture = f.CaptureInputs();
  REQUIRE(capture);
  SECTION("output cannot become an input") {
    CHECK_FALSE(capture->Capture(f.output));
  }
  SECTION("unallocated physical backing") {
    CHECK_FALSE(capture->Capture({0x1000, 32}));
  }
  SECTION("different real allocation owner") {
    auto other = f.session->owner();
    ++other.session_id;
    uint32_t address = 0;
    uint64_t id = 0;
    auto* heap = static_cast<PhysicalHeap*>(f.memory.LookupHeap(0xE1800000));
    REQUIRE(heap->AllocRangeOwned(
        0xE1800000, 0xE1900000, 0x1000, 0x1000,
        kMemoryAllocationReserve | kMemoryAllocationCommit,
        kMemoryProtectRead | kMemoryProtectWrite, true, other, &address, &id));
    CHECK_FALSE(capture->Capture({f.memory.GetPhysicalAddress(address), 32}));
  }
}

TEST_CASE("A changed primary cannot be sealed as the already frozen Plan",
          "[kernel][native_frame_execution][native_input_snapshot]") {
  Fixture f;
  *f.memory.TranslatePhysical<uint8_t*>(f.record.address) ^= 1;
  auto capture = f.CaptureInputs();
  REQUIRE(capture);
  CHECK_FALSE(capture->Seal());
  CHECK_FALSE(f.session->GetSnapshot().submission_attempted);
}

TEST_CASE("PM4 layout consumes the sealed rectangle after original RAM changes",
          "[kernel][native_frame_execution][native_input_snapshot]") {
  Fixture f(GENERATE(false, true), true);
  const auto record = f.Pm4Record();
  auto capture = f.CaptureInputs();
  REQUIRE(capture);
  REQUIRE(capture->Capture(f.record));
  const auto snapshot = capture->Seal();
  REQUIRE(snapshot);
  capture.reset();
  std::memset(f.memory.TranslatePhysical(f.record.address + 0x100), 0xFF, 24);
  const auto layout = NativeFrameExecutionTestAccess::Pm4Layout(
      &f.memory, f.session, f.thread, f.source, f.plan, f.Inputs(), record,
      f.Descriptor(record), snapshot);
  REQUIRE(layout);
  CHECK_FALSE(f.Pm4Layout(
      record, f.Descriptor(record)));  // Live diagnostic path sees NaN.
  const auto request = NativeFrameExecutionTestAccess::Mint(
      f.session, f.thread, f.source, f.plan, f.output, f.Inputs(), f.shaders,
      layout, snapshot);
  REQUIRE(request);
  CHECK(request->IsAdmitted());
  CHECK(request->input_provenance() ==
        gpu::NativeFrameInputProvenance::kImmutableSnapshot);
  CHECK(request->input_snapshot() == snapshot);
  std::array<uint8_t, 24> bytes;
  CHECK(request->input_snapshot()->Read({f.record.address + 0x100, 24}, bytes));
  CHECK_FALSE(f.session->GetSnapshot().submission_attempted);
}

TEST_CASE(
    "Snapshot requests refuse uncovered inputs different Plan output or stale "
    "Source",
    "[kernel][native_frame_execution][native_input_snapshot]") {
  Fixture f;
  auto capture = f.CaptureInputs();
  REQUIRE(capture);
  const auto snapshot = capture->Seal();
  REQUIRE(snapshot);
  capture.reset();
  auto inputs = f.Inputs();
  auto output = f.output;
  auto plan = f.plan;
  SECTION("uncovered vertex bytes") {}
  SECTION("different immutable Plan object") {
    inputs.resize(1);
    plan = Plan::Analyze(f.primary).plan;
    REQUIRE(plan);
    CHECK(plan != f.plan);
  }
  SECTION("different output extent") {
    inputs.resize(1);
    output.length -= 4;
  }
  SECTION("old Source identity") {
    inputs.resize(1);
    REQUIRE(f.session->AbortUnsubmitted(f.thread, f.source));
    f.Freeze();
  }
  CHECK_FALSE(NativeFrameExecutionTestAccess::Mint(
      f.session, f.thread, f.source, plan, output, inputs, f.shaders,
      std::nullopt, snapshot));
}

TEST_CASE(
    "Same Source snapshots cannot mix layout proof from another byte view",
    "[kernel][native_frame_execution][native_input_snapshot]") {
  Fixture f(false, true);
  const auto record = f.Pm4Record();
  auto capture = f.CaptureInputs();
  REQUIRE(capture);
  REQUIRE(capture->Capture(f.record));
  const auto first = capture->Seal();
  REQUIRE(first);
  capture.reset();
  const auto layout = NativeFrameExecutionTestAccess::Pm4Layout(
      &f.memory, f.session, f.thread, f.source, f.plan, f.Inputs(), record,
      f.Descriptor(record), first);
  REQUIRE(layout);
  std::memset(f.memory.TranslatePhysical(f.record.address + 0x100), 0xFF, 24);
  capture = f.CaptureInputs();
  REQUIRE(capture);
  REQUIRE(capture->Capture(f.record));
  const auto second = capture->Seal();
  REQUIRE(second);
  capture.reset();
  CHECK(first->identity() == second->identity());
  CHECK(first->plan() == second->plan());
  CHECK_FALSE(NativeFrameExecutionTestAccess::Mint(
      f.session, f.thread, f.source, f.plan, f.output, f.Inputs(), f.shaders,
      layout, second));
}

TEST_CASE(
    "Snapshot completeness is rederived from actual Plan even when inventory "
    "is omitted",
    "[kernel][native_frame_execution][native_input_snapshot]") {
  Fixture f;
  auto capture = f.CaptureInputs();
  REQUIRE(capture);
  const auto snapshot = capture->Seal();
  REQUIRE(snapshot);
  capture.reset();
  const std::vector<Range> primary_only{
      {f.record.address, uint32_t(f.primary.size())}};
  // Actual final copy still references24B atrecord+100, absent from snapshot.
  CHECK_FALSE(NativeFrameExecutionTestAccess::Mint(
      f.session, f.thread, f.source, f.plan, f.output, primary_only, f.shaders,
      std::nullopt, snapshot));
}

TEST_CASE("Snapshot byte view remains readable after actual Memory destruction",
          "[kernel][native_frame_execution][native_input_snapshot]") {
  std::shared_ptr<const gpu::NativeFrameInputSnapshot> snapshot;
  Range range;
  std::array<uint8_t, 24> before, after;
  {
    Fixture f(false, true);
    range = {f.record.address + 0x100, 24};
    auto capture = f.CaptureInputs();
    REQUIRE(capture);
    REQUIRE(capture->Capture(range));
    snapshot = capture->Seal();
    REQUIRE(snapshot);
    REQUIRE(snapshot->Read(range, before));
  }
  CHECK(snapshot->valid());
  REQUIRE(snapshot->Read(range, after));
  CHECK(before == after);
}

TEST_CASE("Snapshot total byte budget bounds actual owner backed copies",
          "[kernel][native_frame_execution][native_input_snapshot]") {
  Fixture f;
  uint32_t address = 0;
  uint64_t id = 0;
  auto* heap = static_cast<PhysicalHeap*>(f.memory.LookupHeap(0xE1800000));
  REQUIRE(
      heap->AllocRangeOwned(0xE1800000, 0xE3800000, 16 * 1024 * 1024, 0x1000,
                            kMemoryAllocationReserve | kMemoryAllocationCommit,
                            kMemoryProtectRead | kMemoryProtectWrite, true,
                            f.session->owner(), &address, &id));
  const uint32_t physical = f.memory.GetPhysicalAddress(address);
  auto capture = f.CaptureInputs();
  REQUIRE(capture);
  for (uint32_t i = 0; i < 3; ++i) {
    REQUIRE(
        capture->Capture({physical + i * 4 * 1024 * 1024, 4 * 1024 * 1024}));
  }
  CHECK_FALSE(capture->Capture({physical + 12 * 1024 * 1024, 4 * 1024 * 1024}));
  CHECK_FALSE(capture->Seal());
}

TEST_CASE("A prepared handle surviving Memory becomes inert",
          "[kernel][native_frame_execution]") {
  std::shared_ptr<const Request> request;
  {
    Fixture fixture;
    request = fixture.Mint();
    REQUIRE(request);
    REQUIRE(request->IsAdmitted());
  }
  CHECK_FALSE(request->IsAdmitted());
  CHECK_FALSE(request->MarkSubmissionAttempted());
  request.reset();
}
// These are host serializer/admission tests using actual Memory and the
// existing Session/PM4 proof fixture. CompletionQueue tokens are host mechanism
// evidence only; no native device, GPU submission, fence or pixels are claimed.
struct OutputCaptureFrame {
  std::shared_ptr<const Request> request;
  gpu::NativeFrameExecutionResult execution;
};

OutputCaptureFrame PrepareOutputCaptureFrame(Fixture& f,
                                             gpu::GpuCompletionQueue& queue,
                                             bool original_xg = false) {
  f.graph->epoch = queue.backend_epoch();
  f.graph->accepts_model_submission = true;
  std::optional<gpu::NativeFrameOutputLayout> layout;
  std::shared_ptr<const gpu::NativeFrameInputSnapshot> snapshot;
  auto inputs = f.Inputs();
  for (const auto &shader : f.shaders) {
    if (!shader.load.is_inline) {
      inputs.push_back(
          {shader.load.physical_address, uint32_t(shader.code.size())});
    }
  }
  if (const auto &title = f.plan->title_input()) {
    inputs.push_back(title->range());
  }
  if (original_xg) {
    layout = NativeFrameExecutionTestAccess::Layout(&f.memory, f.session,
                                                    f.thread, f.source, f.plan,
                                                    inputs, f.OutputRecord());
  } else {
    auto capture = f.CaptureInputs();
    REQUIRE(capture);
    REQUIRE(capture->Capture(f.record));
    snapshot = capture->Seal();
    REQUIRE(snapshot);
    capture.reset();
    const auto record = f.Pm4Record();
    layout = NativeFrameExecutionTestAccess::Pm4Layout(
        &f.memory, f.session, f.thread, f.source, f.plan, inputs, record,
        f.Descriptor(record), snapshot);
  }
  REQUIRE(layout);
  auto request = NativeFrameExecutionTestAccess::Mint(
      f.session, f.thread, f.source, f.plan, f.output, inputs, f.shaders,
      layout, snapshot);
  REQUIRE(request);
  REQUIRE(request->MarkSubmissionAttempted());
  const auto ticket = queue.TrackNativeFrameSubmitted(11, request->identity());
  REQUIRE(f.session->AttachGpu(f.thread, f.source, ticket));
  REQUIRE(f.session->BeginRetire(f.thread, f.source));
  queue.CompleteThrough(11);
  REQUIRE(ticket.status() == gpu::GpuCompletionStatus::kComplete);
  gpu::NativeFrameExecutionResult execution;
  execution.status = gpu::NativeFrameExecutionResult::Status::kSubmitted;
  execution.copy_operations = 1;
  execution.written_range = {f.output.address, layout->required_bytes};
  execution.title_state_restored = true;
  execution.completion = ticket;
  return {std::move(request), std::move(execution)};
}

gpu::NativeFrameReadbackResult OutputCaptureReadback(
    const OutputCaptureFrame& frame) {
  gpu::NativeFrameReadbackResult readback;
  readback.status = gpu::NativeFrameReadbackResult::Status::kComplete;
  readback.native_completion = frame.execution.completion;
  readback.copied_range = frame.execution.written_range;
  readback.bytes.resize(readback.copied_range.length);
  for (size_t i = 0; i < readback.bytes.size(); ++i) {
    readback.bytes[i] = uint8_t(i * 17 + 3);
  }
  return readback;
}

gpu::NativeFramePresentationStatus OutputCapturePresentation(
    const OutputCaptureFrame& frame) {
  gpu::NativeFramePresentationStatus presentation;
  presentation.status = gpu::NativeFramePresentationStatus::Status::kPending;
  presentation.identity = frame.request->identity();
  presentation.backend_epoch = frame.request->backend_epoch();
  presentation.image_version = 73;  // Deliberately not the Source serial.
  presentation.native_write_submission =
      frame.execution.completion.submission();
  presentation.image_ready = true;
  // An actual image association does not require a completed UI paint or even
  // Presenter acceptance. Those remain independently observed metadata.
  presentation.presenter_accepted = false;
  return presentation;
}

class OutputCaptureScratchDirectory {
 public:
  OutputCaptureScratchDirectory() {
    // Same exclusive temporary-directory mechanism as filesystem_test.cc.
    std::error_code ec;
    const auto root = std::filesystem::temp_directory_path(ec);
    REQUIRE_FALSE(ec);
    std::random_device rng;
    for (unsigned attempt = 0; attempt < 16; ++attempt) {
      auto candidate =
          root / ("xenia_native_output_test_" + std::to_string(rng()) + "_" +
                  std::to_string(rng()));
      if (std::filesystem::create_directory(candidate, ec)) {
        path = std::move(candidate);
        break;
      }
    }
    REQUIRE_FALSE(path.empty());
  }
  ~OutputCaptureScratchDirectory() {
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
  }
  std::filesystem::path path;
};

std::filesystem::path OutputCaptureFile(std::filesystem::path base,
                                        const char* suffix) {
  base += suffix;
  return base;
}
std::vector<uint8_t> OutputCaptureFileBytes(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary);
  REQUIRE(file.is_open());
  return {std::istreambuf_iterator<char>(file),
          std::istreambuf_iterator<char>()};
}
void OutputCaptureWriteMarker(const std::filesystem::path& path) {
  std::ofstream file(path, std::ios::binary);
  REQUIRE(file.is_open());
  file << "existing artifact\n";
  file.close();
  REQUIRE(file.good());
}

TEST_CASE("PM4 output export uses owned bytes after Source retirement",
          "[kernel][native_frame_execution][native_output_capture]") {
  Fixture f(false, true, {852, 480, 880, 864, 856, 2 * 1024 * 1024});
  gpu::GpuCompletionQueue queue;
  auto frame = PrepareOutputCaptureFrame(f, queue);
  const auto snapshot =
      SnapshotNativeFrameOutputCapture(*frame.request, frame.execution);
  REQUIRE(snapshot);
  auto capture = *snapshot;
  CHECK(capture.layout.origin ==
        gpu::NativeFrameOutputLayout::Origin::kPm4Resolve);
  CHECK_FALSE(capture.layout.original_texture_fetch);
  REQUIRE(capture.layout.pm4_resolve);
  CHECK(capture.layout.required_bytes == 1658880);
  CHECK(capture.layout.width == 852);
  CHECK(capture.layout.height == 480);
  CHECK(capture.layout.pitch == 864);
  auto readback = OutputCaptureReadback(frame);
  const auto expected = readback.bytes;
  REQUIRE(CompleteNativeFrameOutputCapture(capture, std::move(readback)));
  REQUIRE(AssociateNativeFrameOutputCapture(capture,
                                            OutputCapturePresentation(frame)));
  CHECK(capture.image_version == 73);
  CHECK(capture.image_version != capture.identity.source_serial);
  CHECK_FALSE(capture.presenter_accepted);
  CHECK(capture.presentation_status ==
        uint32_t(gpu::NativeFramePresentationStatus::Status::kPending));
  REQUIRE(f.session->RetireComplete(f.thread, f.source));
  frame.request.reset();
  // Neither serialization nor decoding may reconstruct pixels from guest RAM.
  std::memset(f.memory.TranslatePhysical(f.output.address), 0, f.output.length);
  std::memset(f.memory.TranslatePhysical(f.record.address), 0, f.record.length);
  OutputCaptureScratchDirectory scratch;
  const auto base = scratch.path / "output";
  REQUIRE(SaveNativeFrameOutputCapture(base, capture));
  CHECK(OutputCaptureFileBytes(OutputCaptureFile(base, ".readback.bin")) ==
        expected);
  const auto json_bytes =
      OutputCaptureFileBytes(OutputCaptureFile(base, ".readback.json"));
  const std::string json(json_bytes.begin(), json_bytes.end());
  CHECK(json.find("\"version\":2") != std::string::npos);
  CHECK(json.find("\"primary_capture\":null") != std::string::npos);
  CHECK_FALSE(std::filesystem::exists(OutputCaptureFile(base, ".primary.bin")));
  CHECK(json.find("\"output_layout_origin_name\":\"pm4_resolve\"") !=
        std::string::npos);
  CHECK(json.find("\"original_texture_fetch\":null") != std::string::npos);
  CHECK(json.find("\"image_version\":73") != std::string::npos);
  CHECK(json.find("\"width\":852") != std::string::npos);
  CHECK(json.find("\"pitch\":864") != std::string::npos);
  CHECK(json.find("\"pixels_verified\":false") != std::string::npos);
  CHECK_FALSE(json.empty());
  CHECK(json.back() == '\n');
  CHECK(f.session->GetSnapshot().phase == Session::Phase::kBound);
}

// These trigger values are host mechanism fixtures, not actual controller or
// native readiness evidence. In particular, the loader generation intentionally
// differs from the runtime module generation in the Source identity.
NativeFrameOutputBDownTrigger OutputCaptureBDownTrigger(
    const NativeFrameOutputCapture& capture) {
  NativeFrameOutputBDownTrigger trigger;
  trigger.input_generation = 11;
  trigger.domain_id = 13;
  trigger.root_load_generation = capture.identity.owner.module_generation + 1;
  trigger.foreground_generation = 17;
  trigger.visibility_sequence = 19;
  trigger.press_sequence = 23;
  trigger.binding_epoch = 29;
  trigger.root_hmodule = capture.identity.owner.module_hmodule;
  trigger.slot = 1;
  trigger.native_user = 1;
  trigger.device_handle = 31;
  // Zero remains a legitimate sampled group/packet value.
  trigger.binding_identity = 0;
  trigger.packet = 0;
  trigger.buttons = 0x2000;
  return trigger;
}

// Replace this fixture's public-ISA inline load with the exact same bytes in
// real native-owned Memory. No renderer, guest module or private Request seam.
void MakeOutputCaptureShaderExternal(Fixture &f) {
  REQUIRE(f.shaders.size() == 1);
  const auto code = f.shaders[0].code;
  const auto old = f.shaders[0].load;
  REQUIRE(old.is_inline);
  REQUIRE(old.packet_dword_offset == 0);
  REQUIRE(f.session->AbortUnsubmitted(f.thread, f.source));
  std::vector<uint32_t> words{
      0xC0010000 | uint32_t(gpu::xenos::PM4_IM_LOAD) << 8,
      f.record.address + 0x200, uint32_t(code.size() / 4)};
  words.insert(words.end(), f.plan->words().begin() + code.size() / 4 + 3,
               f.plan->words().end());
  f.primary = Bytes(words);
  std::memcpy(f.memory.TranslatePhysical(f.record.address), f.primary.data(),
              f.primary.size());
  std::memcpy(f.memory.TranslatePhysical(f.record.address + 0x200), code.data(),
              code.size());
  const auto analyzed = Plan::Analyze(f.primary);
  REQUIRE(analyzed);
  f.plan = analyzed.plan;
  REQUIRE(f.plan->inventory().shaders.size() == 1);
  f.shaders[0].load = f.plan->inventory().shaders[0];
  f.Freeze();
}

TEST_CASE("Execution bundle owns complete native, shader and Title bytes",
          "[kernel][native_frame_execution][native_output_capture]") {
  Fixture f(true, true);
  gpu::GpuCompletionQueue queue;
  f.graph->epoch = queue.backend_epoch();
  const auto title = ModelTitleInput(queue.backend_epoch(), 0x10000000, 64, 64);
  RefreezeWithTitle(f, title);
  auto frame = PrepareOutputCaptureFrame(f, queue);
  auto capture =
      SnapshotNativeFrameOutputCapture(*frame.request, frame.execution);
  REQUIRE(capture);
  REQUIRE(AttachNativeFrameOutputPrimary(*capture, *f.source,
                                         OutputCaptureBDownTrigger(*capture)));
  REQUIRE(AttachNativeFrameOutputInputs(*capture, *frame.request));
  CHECK_FALSE(AttachNativeFrameOutputInputs(*capture, *frame.request));
  REQUIRE(capture->inputs);
  const auto &inputs = *capture->inputs;
  CHECK(inputs.identity == frame.request->identity());
  CHECK(inputs.backend_epoch == queue.backend_epoch());
  CHECK(inputs.snapshot_bytes == frame.request->input_snapshot()->byte_size());
  REQUIRE(inputs.blocks.size() ==
          frame.request->input_snapshot()->blocks().size());
  for (size_t i = 0; i < inputs.blocks.size(); ++i) {
    const auto &block = inputs.blocks[i];
    const auto &original = frame.request->input_snapshot()->blocks()[i];
    CHECK(block.range.address == original.range.address);
    CHECK(block.range.length == original.range.length);
    CHECK(std::vector<uint8_t>(inputs.bytes.begin() + block.offset,
                               inputs.bytes.begin() + block.offset +
                                   block.range.length) == original.bytes);
  }
  REQUIRE(inputs.shaders.size() == 1);
  const auto &shader = inputs.shaders[0];
  CHECK(std::vector<uint8_t>(inputs.bytes.begin() + shader.offset,
                             inputs.bytes.begin() + shader.offset +
                                 shader.length) == f.shaders[0].code);
  REQUIRE(inputs.title);
  CHECK(inputs.title->identity == title->identity());
  CHECK(inputs.title->layout.fetch == title->fetch());
  CHECK(inputs.title->length == title->bytes().size());
  CHECK(std::vector<uint8_t>(inputs.bytes.begin() + inputs.title->offset,
                             inputs.bytes.end()) ==
        std::vector<uint8_t>(title->bytes().begin(), title->bytes().end()));
  const auto expected = inputs.bytes;
  REQUIRE(
      CompleteNativeFrameOutputCapture(*capture, OutputCaptureReadback(frame)));
  REQUIRE(AssociateNativeFrameOutputCapture(*capture,
                                            OutputCapturePresentation(frame)));
  REQUIRE(f.session->RetireComplete(f.thread, f.source));
  frame.request.reset();
  f.source.reset();
  std::memset(f.memory.TranslatePhysical(f.record.address), 0xCD,
              f.record.length);
  OutputCaptureScratchDirectory scratch;
  const auto base = scratch.path / "whole_inputs";
  REQUIRE(SaveNativeFrameOutputCapture(base, *capture));
  CHECK(OutputCaptureFileBytes(OutputCaptureFile(base, ".inputs.bin")) ==
        expected);
  const auto bytes =
      OutputCaptureFileBytes(OutputCaptureFile(base, ".readback.json"));
  const std::string metadata(bytes.begin(), bytes.end());
  CHECK(metadata.find(
            "\"input_capture\":{\"schema\":1,\"write_complete\":true") !=
        std::string::npos);
  CHECK(metadata.find("\"content\":\"immutable_execution_inputs\"") !=
        std::string::npos);
  CHECK(metadata.find("\"title_hmodule\":" +
                      std::to_string(title->identity().title_hmodule)) !=
        std::string::npos);
  CHECK(metadata.find("\"swap_fetch\":[") != std::string::npos);
  CHECK(metadata.find("\"image_version\":73") != std::string::npos);
}

TEST_CASE(
    "Execution bundle exports actual external shader bytes from sealed Memory",
    "[kernel][native_frame_execution][native_output_capture]") {
  Fixture f(true, true);
  MakeOutputCaptureShaderExternal(f);
  gpu::GpuCompletionQueue queue;
  auto frame = PrepareOutputCaptureFrame(f, queue);
  auto capture =
      SnapshotNativeFrameOutputCapture(*frame.request, frame.execution);
  REQUIRE(capture);
  REQUIRE(AttachNativeFrameOutputPrimary(*capture, *f.source,
                                         OutputCaptureBDownTrigger(*capture)));
  REQUIRE(AttachNativeFrameOutputInputs(*capture, *frame.request));
  REQUIRE(capture->inputs);
  REQUIRE(capture->inputs->shaders.size() == 1);
  const auto &shader = capture->inputs->shaders[0];
  CHECK_FALSE(shader.load.is_inline);
  CHECK(shader.load.physical_address == f.record.address + 0x200);
  CHECK(std::vector<uint8_t>(capture->inputs->bytes.begin() + shader.offset,
                             capture->inputs->bytes.begin() + shader.offset +
                                 shader.length) == f.shaders[0].code);
  std::memset(f.memory.TranslatePhysical(f.record.address + 0x200), 0xCD,
              shader.length);
  REQUIRE(
      CompleteNativeFrameOutputCapture(*capture, OutputCaptureReadback(frame)));
  REQUIRE(AssociateNativeFrameOutputCapture(*capture,
                                            OutputCapturePresentation(frame)));
  OutputCaptureScratchDirectory scratch;
  REQUIRE(SaveNativeFrameOutputCapture(scratch.path / "external", *capture));
}

TEST_CASE("Execution bundle rejects foreign request identity without partial "
          "attachment",
          "[kernel][native_frame_execution][native_output_capture]") {
  Fixture f(true, true);
  gpu::GpuCompletionQueue queue;
  const auto frame = PrepareOutputCaptureFrame(f, queue);
  auto capture =
      SnapshotNativeFrameOutputCapture(*frame.request, frame.execution);
  REQUIRE(capture);
  REQUIRE(AttachNativeFrameOutputPrimary(*capture, *f.source,
                                         OutputCaptureBDownTrigger(*capture)));
  SECTION("source") { ++capture->identity.source_serial; }
  SECTION("backend") { ++capture->backend_epoch; }
  SECTION("primary bytes") { capture->primary_bytes.back() ^= 1; }
  CHECK_FALSE(AttachNativeFrameOutputInputs(*capture, *frame.request));
  CHECK_FALSE(capture->inputs);
}

TEST_CASE("Execution bundle validates every complete row and byte before file "
          "creation",
          "[kernel][native_frame_execution][native_output_capture]") {
  Fixture f(true, true);
  MakeOutputCaptureShaderExternal(f);
  gpu::GpuCompletionQueue queue;
  f.graph->epoch = queue.backend_epoch();
  const auto title = ModelTitleInput(queue.backend_epoch(), 0x10000000, 64, 64);
  RefreezeWithTitle(f, title);
  const auto frame = PrepareOutputCaptureFrame(f, queue);
  auto capture =
      SnapshotNativeFrameOutputCapture(*frame.request, frame.execution);
  REQUIRE(capture);
  REQUIRE(AttachNativeFrameOutputPrimary(*capture, *f.source,
                                         OutputCaptureBDownTrigger(*capture)));
  REQUIRE(AttachNativeFrameOutputInputs(*capture, *frame.request));
  REQUIRE(
      CompleteNativeFrameOutputCapture(*capture, OutputCaptureReadback(frame)));
  REQUIRE(AssociateNativeFrameOutputCapture(*capture,
                                            OutputCapturePresentation(frame)));
  auto &inputs = *capture->inputs;
  REQUIRE(!inputs.blocks.empty());
  REQUIRE(inputs.shaders.size() == 1);
  REQUIRE(inputs.title);
  SECTION("foreign owner") { ++inputs.identity.owner.module_generation; }
  SECTION("foreign backend") { ++inputs.backend_epoch; }
  SECTION("primary address") { ++inputs.primary_range.address; }
  SECTION("payload gap") { ++inputs.blocks[0].offset; }
  SECTION("native range exceeds cap") {
    inputs.blocks[0].range.length =
        gpu::NativeFrameInputSnapshot::kMaxReadBytes + 1;
  }
  SECTION("native output alias") {
    inputs.blocks[0].range.address = capture->output_range.address;
  }
  SECTION("missing shader") { inputs.shaders.clear(); }
  SECTION("shader payload gap") { ++inputs.shaders[0].offset; }
  SECTION("external shader byte mismatch") {
    inputs.bytes[inputs.shaders[0].offset] ^= 1;
  }
  SECTION("Title epoch") { ++inputs.title->identity.backend_epoch; }
  SECTION("Title foreground") {
    ++inputs.title->identity.foreground_generation;
  }
  SECTION("Title layout") { ++inputs.title->layout.pitch; }
  SECTION("Title truncated") { --inputs.title->length; }
  SECTION("snapshot total") { ++inputs.snapshot_bytes; }
  SECTION("missing payload tail") { inputs.bytes.pop_back(); }
  SECTION("oversized payload") {
    inputs.bytes.resize(NativeFrameOutputInputs::kMaxBytes + 1);
  }
  OutputCaptureScratchDirectory scratch;
  const auto base = scratch.path / "invalid_inputs";
  CHECK_FALSE(SaveNativeFrameOutputCapture(base, *capture));
  for (const auto suffix :
       {".readback.bin", ".primary.bin", ".inputs.bin", ".readback.json"}) {
    CHECK_FALSE(std::filesystem::exists(OutputCaptureFile(base, suffix)));
  }
}

TEST_CASE("Execution bundle sidecar is exclusive and partial failure publishes "
          "no metadata",
          "[kernel][native_frame_execution][native_output_capture]") {
  Fixture f(true, true);
  gpu::GpuCompletionQueue queue;
  const auto frame = PrepareOutputCaptureFrame(f, queue);
  auto capture =
      SnapshotNativeFrameOutputCapture(*frame.request, frame.execution);
  REQUIRE(capture);
  REQUIRE(AttachNativeFrameOutputPrimary(*capture, *f.source,
                                         OutputCaptureBDownTrigger(*capture)));
  REQUIRE(AttachNativeFrameOutputInputs(*capture, *frame.request));
  REQUIRE(
      CompleteNativeFrameOutputCapture(*capture, OutputCaptureReadback(frame)));
  REQUIRE(AssociateNativeFrameOutputCapture(*capture,
                                            OutputCapturePresentation(frame)));
  OutputCaptureScratchDirectory scratch;
  const auto base = scratch.path / "exclusive_inputs";
  const auto sidecar = OutputCaptureFile(base, ".inputs.bin");
  SECTION("existing file") {
    OutputCaptureWriteMarker(sidecar);
    const auto previous = OutputCaptureFileBytes(sidecar);
    CHECK_FALSE(SaveNativeFrameOutputCapture(base, *capture));
    CHECK(OutputCaptureFileBytes(sidecar) == previous);
  }
  SECTION("existing directory") {
    REQUIRE(std::filesystem::create_directory(sidecar));
    CHECK_FALSE(SaveNativeFrameOutputCapture(base, *capture));
  }
  CHECK_FALSE(
      std::filesystem::exists(OutputCaptureFile(base, ".readback.json")));
  CHECK(OutputCaptureFileBytes(OutputCaptureFile(base, ".readback.bin")) ==
        capture->bytes);
  CHECK(OutputCaptureFileBytes(OutputCaptureFile(base, ".primary.bin")) ==
        capture->primary_bytes);
}

TEST_CASE("B-down export retains the full frozen primary beside exact output",
          "[kernel][native_frame_execution][native_output_capture]") {
  Fixture f(false, true);
  gpu::GpuCompletionQueue queue;
  auto frame = PrepareOutputCaptureFrame(f, queue);
  auto capture =
      SnapshotNativeFrameOutputCapture(*frame.request, frame.execution);
  REQUIRE(capture);
  const auto trigger = OutputCaptureBDownTrigger(*capture);
  const auto primary = std::vector<uint8_t>(f.source->primary().begin(),
                                            f.source->primary().end());
  REQUIRE(AttachNativeFrameOutputPrimary(*capture, *f.source, trigger));
  CHECK(capture->primary_bytes == primary);
  CHECK(capture->primary_identity == frame.request->identity());
  CHECK_FALSE(AttachNativeFrameOutputPrimary(*capture, *f.source, trigger));
  REQUIRE(
      CompleteNativeFrameOutputCapture(*capture, OutputCaptureReadback(frame)));
  REQUIRE(AssociateNativeFrameOutputCapture(*capture,
                                            OutputCapturePresentation(frame)));
  REQUIRE(f.session->RetireComplete(f.thread, f.source));
  frame.request.reset();
  f.source.reset();
  std::memset(f.primary.data(), 0xCD, f.primary.size());
  std::memset(f.memory.TranslatePhysical(f.record.address), 0xCD,
              f.record.length);
  OutputCaptureScratchDirectory scratch;
  const auto base = scratch.path / "with_primary";
  REQUIRE(SaveNativeFrameOutputCapture(base, *capture));
  CHECK(OutputCaptureFileBytes(OutputCaptureFile(base, ".primary.bin")) ==
        primary);
  CHECK(OutputCaptureFileBytes(OutputCaptureFile(base, ".readback.bin")) ==
        capture->bytes);
  const auto bytes =
      OutputCaptureFileBytes(OutputCaptureFile(base, ".readback.json"));
  const std::string json(bytes.begin(), bytes.end());
  CHECK(json.find("\"version\":2") != std::string::npos);
  CHECK(json.find("\"primary_capture\":{\"write_complete\":true,\"bytes\":" +
                  std::to_string(primary.size())) != std::string::npos);
  CHECK(json.find("\"content\":\"source_primary_pm4\"") != std::string::npos);
  CHECK(json.find("\"byte_order\":\"big_endian_dwords\"") != std::string::npos);
  CHECK(json.find("\"truncated\":false") != std::string::npos);
  CHECK(json.find("\"image_version\":73") != std::string::npos);
  CHECK(json.find("\"kind\":\"observed_b_down_before_source_freeze\"") !=
        std::string::npos);
  CHECK(json.find("\"root_load_generation\":" +
                  std::to_string(trigger.root_load_generation)) !=
        std::string::npos);
  CHECK(json.find("\"binding_identity\":0,\"packet\":0,\"buttons\":8192") !=
        std::string::npos);
}

TEST_CASE("Primary attachment rejects unknown trigger or foreign Source facts",
          "[kernel][native_frame_execution][native_output_capture]") {
  Fixture f(false, true);
  gpu::GpuCompletionQueue queue;
  const auto frame = PrepareOutputCaptureFrame(f, queue);
  auto capture =
      SnapshotNativeFrameOutputCapture(*frame.request, frame.execution);
  REQUIRE(capture);
  auto trigger = OutputCaptureBDownTrigger(*capture);
  SECTION("unknown input generation") { trigger.input_generation = 0; }
  SECTION("unknown domain") { trigger.domain_id = 0; }
  SECTION("unknown loader generation") { trigger.root_load_generation = 0; }
  SECTION("unknown foreground") { trigger.foreground_generation = 0; }
  SECTION("unknown visibility episode") { trigger.visibility_sequence = 0; }
  SECTION("unknown observed press") { trigger.press_sequence = 0; }
  SECTION("unknown callback binding") { trigger.binding_epoch = 0; }
  SECTION("foreign root module") { ++trigger.root_hmodule; }
  SECTION("unknown device") { trigger.device_handle = 0; }
  SECTION("unknown user") { trigger.native_user = 0xFF; }
  SECTION("out of range controller slot") { trigger.slot = 4; }
  SECTION("B is not down") { trigger.buttons = 0x1000; }
  SECTION("Source serial does not match output") {
    ++capture->identity.source_serial;
  }
  CHECK_FALSE(AttachNativeFrameOutputPrimary(*capture, *f.source, trigger));
  CHECK(capture->primary_bytes.empty());
  CHECK_FALSE(capture->primary_identity.valid());
  CHECK_FALSE(capture->b_down_trigger);
  CHECK(f.session->RetireComplete(f.thread, f.source));
}

TEST_CASE("Primary writer rejects partial detached primary before any file",
          "[kernel][native_frame_execution][native_output_capture]") {
  Fixture f(false, true);
  gpu::GpuCompletionQueue queue;
  const auto frame = PrepareOutputCaptureFrame(f, queue);
  auto capture =
      SnapshotNativeFrameOutputCapture(*frame.request, frame.execution);
  REQUIRE(capture);
  REQUIRE(AttachNativeFrameOutputPrimary(*capture, *f.source,
                                         OutputCaptureBDownTrigger(*capture)));
  REQUIRE(
      CompleteNativeFrameOutputCapture(*capture, OutputCaptureReadback(frame)));
  REQUIRE(AssociateNativeFrameOutputCapture(*capture,
                                            OutputCapturePresentation(frame)));
  SECTION("missing trigger") { capture->b_down_trigger.reset(); }
  SECTION("missing bytes") { capture->primary_bytes.clear(); }
  SECTION("non DWORD extent") { capture->primary_bytes.pop_back(); }
  SECTION("over bounded extent") { capture->primary_bytes.resize(131076); }
  SECTION("foreign primary serial") {
    ++capture->primary_identity.source_serial;
  }
  SECTION("foreign primary owner") {
    ++capture->primary_identity.owner.session_id;
  }
  SECTION("corrupted trigger") { capture->b_down_trigger->buttons = 0; }
  OutputCaptureScratchDirectory scratch;
  const auto base = scratch.path / "refused_primary";
  CHECK_FALSE(SaveNativeFrameOutputCapture(base, *capture));
  CHECK_FALSE(
      std::filesystem::exists(OutputCaptureFile(base, ".readback.bin")));
  CHECK_FALSE(std::filesystem::exists(OutputCaptureFile(base, ".primary.bin")));
  CHECK_FALSE(
      std::filesystem::exists(OutputCaptureFile(base, ".readback.json")));
  CHECK(f.session->RetireComplete(f.thread, f.source));
}

TEST_CASE("Primary sidecar is exclusive and precedes commit metadata",
          "[kernel][native_frame_execution][native_output_capture]") {
  Fixture f(false, true);
  gpu::GpuCompletionQueue queue;
  const auto frame = PrepareOutputCaptureFrame(f, queue);
  auto capture =
      SnapshotNativeFrameOutputCapture(*frame.request, frame.execution);
  REQUIRE(capture);
  REQUIRE(AttachNativeFrameOutputPrimary(*capture, *f.source,
                                         OutputCaptureBDownTrigger(*capture)));
  REQUIRE(
      CompleteNativeFrameOutputCapture(*capture, OutputCaptureReadback(frame)));
  REQUIRE(AssociateNativeFrameOutputCapture(*capture,
                                            OutputCapturePresentation(frame)));
  OutputCaptureScratchDirectory scratch;
  const auto base = scratch.path / "primary_collision";
  const auto primary = OutputCaptureFile(base, ".primary.bin");
  const auto metadata = OutputCaptureFile(base, ".readback.json");
  SECTION("existing primary is never overwritten") {
    OutputCaptureWriteMarker(primary);
    const auto previous = OutputCaptureFileBytes(primary);
    CHECK_FALSE(SaveNativeFrameOutputCapture(base, *capture));
    CHECK(OutputCaptureFileBytes(primary) == previous);
    CHECK_FALSE(std::filesystem::exists(metadata));
  }
  SECTION("primary directory cannot be a completed file") {
    REQUIRE(std::filesystem::create_directory(primary));
    CHECK_FALSE(SaveNativeFrameOutputCapture(base, *capture));
    CHECK(std::filesystem::is_directory(primary));
    CHECK_FALSE(std::filesystem::exists(metadata));
  }
  SECTION("metadata failure retains both complete diagnostic byte files") {
    REQUIRE(std::filesystem::create_directory(metadata));
    CHECK_FALSE(SaveNativeFrameOutputCapture(base, *capture));
    CHECK(std::filesystem::is_directory(metadata));
    CHECK(OutputCaptureFileBytes(primary) == capture->primary_bytes);
  }
  CHECK(OutputCaptureFileBytes(OutputCaptureFile(base, ".readback.bin")) ==
        capture->bytes);
  CHECK(f.session->RetireComplete(f.thread, f.source));
}

TEST_CASE("Output metadata rejects incomplete or mismatched execution",
          "[kernel][native_frame_execution][native_output_capture]") {
  Fixture f(false, true);
  gpu::GpuCompletionQueue queue;
  const auto frame = PrepareOutputCaptureFrame(f, queue);
  auto execution = frame.execution;
  SECTION("not submitted") {
    execution.status = gpu::NativeFrameExecutionResult::Status::kRejected;
  }
  SECTION("Title state not restored") {
    execution.title_state_restored = false;
  }
  SECTION("insufficient written footprint") {
    --execution.written_range.length;
  }
  SECTION("wrong written base") { ++execution.written_range.address; }
  SECTION("different Source") {
    auto identity = frame.request->identity();
    ++identity.source_serial;
    execution.completion = queue.TrackNativeFrameSubmitted(11, identity);
  }
  SECTION("pending native ticket") {
    execution.completion =
        queue.TrackNativeFrameSubmitted(12, frame.request->identity());
  }
  SECTION("another backend epoch") {
    gpu::GpuCompletionQueue other;
    execution.completion =
        other.TrackNativeFrameSubmitted(11, frame.request->identity());
    other.CompleteThrough(11);
  }
  CHECK_FALSE(SnapshotNativeFrameOutputCapture(*frame.request, execution));
  CHECK(f.session->GetSnapshot().phase == Session::Phase::kRetiring);
  CHECK(f.session->RetireComplete(f.thread, f.source));
}

TEST_CASE("Output readback completion requires exact bytes and native ticket",
          "[kernel][native_frame_execution][native_output_capture]") {
  Fixture f(false, true);
  gpu::GpuCompletionQueue queue;
  const auto frame = PrepareOutputCaptureFrame(f, queue);
  auto capture =
      SnapshotNativeFrameOutputCapture(*frame.request, frame.execution);
  REQUIRE(capture);
  auto readback = OutputCaptureReadback(frame);
  SECTION("rejected readback") {
    readback.status = gpu::NativeFrameReadbackResult::Status::kRejected;
  }
  SECTION("byte count mismatch") { readback.bytes.pop_back(); }
  SECTION("copied range base mismatch") { ++readback.copied_range.address; }
  SECTION("copied range exceeds output") { ++readback.copied_range.length; }
  SECTION("wrong submission") {
    readback.native_completion =
        queue.TrackNativeFrameSubmitted(12, frame.request->identity());
    queue.CompleteThrough(12);
  }
  SECTION("wrong Source") {
    auto identity = frame.request->identity();
    ++identity.source_serial;
    readback.native_completion = queue.TrackNativeFrameSubmitted(11, identity);
  }
  CHECK_FALSE(CompleteNativeFrameOutputCapture(*capture, std::move(readback)));
  CHECK_FALSE(capture->readback_complete);
  CHECK(capture->bytes.empty());
  CHECK(f.session->RetireComplete(f.thread, f.source));
}

TEST_CASE(
    "Output image association is exact and does not infer an image version",
    "[kernel][native_frame_execution][native_output_capture]") {
  Fixture f(false, true);
  gpu::GpuCompletionQueue queue;
  const auto frame = PrepareOutputCaptureFrame(f, queue);
  auto capture =
      SnapshotNativeFrameOutputCapture(*frame.request, frame.execution);
  REQUIRE(capture);
  auto presentation = OutputCapturePresentation(frame);
  SECTION("different Source") { ++presentation.identity.source_serial; }
  SECTION("different backend") { ++presentation.backend_epoch; }
  SECTION("wrong native write submission") {
    ++presentation.native_write_submission;
  }
  SECTION("no actual image version") { presentation.image_version = 0; }
  CHECK_FALSE(
      AssociateNativeFrameOutputCapture(*capture, std::move(presentation)));
  CHECK_FALSE(capture->image_associated);
  CHECK(capture->image_version == 0);
  CHECK(f.session->RetireComplete(f.thread, f.source));
}

TEST_CASE("Output writer rechecks detached metadata before creating files",
          "[kernel][native_frame_execution][native_output_capture]") {
  Fixture f(false, true);
  gpu::GpuCompletionQueue queue;
  const auto frame = PrepareOutputCaptureFrame(f, queue);
  auto capture =
      SnapshotNativeFrameOutputCapture(*frame.request, frame.execution);
  REQUIRE(capture);
  REQUIRE(
      CompleteNativeFrameOutputCapture(*capture, OutputCaptureReadback(frame)));
  REQUIRE(AssociateNativeFrameOutputCapture(*capture,
                                            OutputCapturePresentation(frame)));
  SECTION("incomplete readback") { capture->readback_complete = false; }
  SECTION("unassociated image") { capture->image_associated = false; }
  SECTION("different readback Source") {
    ++capture->readback_identity.source_serial;
  }
  SECTION("different native submission") {
    ++capture->readback_native_submission;
  }
  SECTION("short detached bytes") { capture->bytes.pop_back(); }
  SECTION("invalid canonical fetch base") {
    capture->layout.texture_fetch[1] ^= 0x1000;
  }
  SECTION("proof belongs to another Source") {
    ++capture->identity.source_serial;
  }
  OutputCaptureScratchDirectory scratch;
  const auto base = scratch.path / "refused";
  CHECK_FALSE(SaveNativeFrameOutputCapture(base, *capture));
  CHECK_FALSE(
      std::filesystem::exists(OutputCaptureFile(base, ".readback.bin")));
  CHECK_FALSE(
      std::filesystem::exists(OutputCaptureFile(base, ".readback.json")));
  CHECK(f.session->RetireComplete(f.thread, f.source));
}

TEST_CASE("Output writer preserves existing files and reports partial failure",
          "[kernel][native_frame_execution][native_output_capture]") {
  Fixture f(false, true);
  gpu::GpuCompletionQueue queue;
  const auto frame = PrepareOutputCaptureFrame(f, queue);
  auto capture =
      SnapshotNativeFrameOutputCapture(*frame.request, frame.execution);
  REQUIRE(capture);
  REQUIRE(
      CompleteNativeFrameOutputCapture(*capture, OutputCaptureReadback(frame)));
  REQUIRE(AssociateNativeFrameOutputCapture(*capture,
                                            OutputCapturePresentation(frame)));
  OutputCaptureScratchDirectory scratch;
  auto base = scratch.path / "collision";
  auto raw = OutputCaptureFile(base, ".readback.bin");
  auto json = OutputCaptureFile(base, ".readback.json");
  SECTION("existing raw is never overwritten") {
    OutputCaptureWriteMarker(raw);
    const auto previous = OutputCaptureFileBytes(raw);
    CHECK_FALSE(SaveNativeFrameOutputCapture(base, *capture));
    CHECK(OutputCaptureFileBytes(raw) == previous);
    CHECK_FALSE(std::filesystem::exists(json));
  }
  SECTION("existing metadata is never overwritten after raw completes") {
    OutputCaptureWriteMarker(json);
    const auto previous = OutputCaptureFileBytes(json);
    CHECK_FALSE(SaveNativeFrameOutputCapture(base, *capture));
    CHECK(OutputCaptureFileBytes(json) == previous);
    CHECK(OutputCaptureFileBytes(raw) == capture->bytes);
  }
  SECTION("metadata directory yields incomplete artifact without success") {
    REQUIRE(std::filesystem::create_directory(json));
    CHECK_FALSE(SaveNativeFrameOutputCapture(base, *capture));
    CHECK(std::filesystem::is_directory(json));
    CHECK(OutputCaptureFileBytes(raw) == capture->bytes);
  }
  SECTION("missing parent is not silently created") {
    base = scratch.path / "absent" / "capture";
    CHECK_FALSE(SaveNativeFrameOutputCapture(base, *capture));
    CHECK_FALSE(std::filesystem::exists(base.parent_path()));
  }
  CHECK(f.session->GetSnapshot().phase == Session::Phase::kRetiring);
  CHECK(f.session->RetireComplete(f.thread, f.source));
}

TEST_CASE(
    "Original XG diagnostic serializer retains its independent v1 contract",
    "[kernel][native_frame_execution][native_output_capture]") {
  Fixture f(false, true);
  const auto legacy = SnapshotNativeFrameReadbackLayout(
      &f.memory, *f.plan, f.Inputs(), f.OutputRecord());
  REQUIRE(legacy);
  gpu::GpuCompletionQueue queue;
  const auto frame = PrepareOutputCaptureFrame(f, queue, true);
  REQUIRE(frame.request->output_layout());
  CHECK(frame.request->output_layout()->origin ==
        gpu::NativeFrameOutputLayout::Origin::kOriginalXgHeader);
  const auto readback = OutputCaptureReadback(frame);
  OutputCaptureScratchDirectory scratch;
  const auto base = scratch.path / "legacy";
  REQUIRE(SaveNativeFrameReadback(base, *legacy, *frame.request, readback));
  CHECK(OutputCaptureFileBytes(OutputCaptureFile(base, ".readback.bin")) ==
        readback.bytes);
  const auto bytes =
      OutputCaptureFileBytes(OutputCaptureFile(base, ".readback.json"));
  const std::string json(bytes.begin(), bytes.end());
  CHECK(json.find("\"version\":1") != std::string::npos);
  CHECK(json.find("\"original_texture_fetch\":[") != std::string::npos);
  CHECK(f.session->RetireComplete(f.thread, f.source));
}

// TEST-ONLY NativeOverlay storage models. The fixture uses real Memory,
// allocation pins and Source/snapshot lifetimes with public synthetic PM4.
// The friend factory models only immutable storage proof data: it never
// constructs NativeNonmodalOutputProvenance or proves a native Task/GPU/UI.
OutputGeometry ModelOverlayGeometry() {
  return {729, 90, 736, 736, 729, 0x80000};
}
void ModelDisplayWord(std::array<uint8_t, 0x58>& bytes, uint32_t at,
                      uint32_t value) {
  for (uint32_t i = 0; i < 4; ++i) {
    bytes[at + i] = uint8_t(value >> (24 - i * 8));
  }
}
std::array<uint8_t, 0x58> ModelDisplayInformation() {
  std::array<uint8_t, 0x58> info{};
  const auto half = [&](uint32_t at, uint32_t value) {
    info[at] = uint8_t(value >> 8);
    info[at + 1] = uint8_t(value);
  };
  half(0, 1280);
  half(2, 720);
  for (const auto at : {16u, 24u}) ModelDisplayWord(info, at, 1280);
  for (const auto at : {20u, 28u}) ModelDisplayWord(info, at, 720);
  half(0x48, 1280);
  half(0x4A, 720);
  return info;
}
NativeFrameExecutionTestAccess::ModelPm4ProofData ModelOverlayProofData(
    Fixture& f) {
  // Session's existing VD path checks the real biased alias, owner pins,
  // primary and resolve rectangle before supplying the model's base data.
  const auto record = f.Pm4Record();
  const auto vd = f.Pm4Layout(record, f.Descriptor(record));
  REQUIRE(vd);
  REQUIRE(vd->pm4_resolve);
  auto data = NativeFrameExecutionTestAccess::ModelProofData(*vd->pm4_resolve);
  std::fill(data.descriptor.begin() + 8, data.descriptor.end(), 0);
  data.output = {gpu::NativeFramePm4ResolveProof::Mapping::Kind::kCanonicalPhysical,
                 f.output.address, f.output.address};
  data.origin = gpu::NativeFramePm4ResolveProof::OutputOrigin::kNativeOverlay;
  data.display = ModelDisplayInformation();
  return data;
}
std::optional<gpu::NativeFrameOutputLayout> DeriveModelOverlay(
    const Fixture& f,
    const NativeFrameExecutionTestAccess::ModelPm4ProofData& data,
    std::optional<Range> output = {}) {
  return gpu::DeriveNativePm4OutputLayout(
      *f.plan, output.value_or(f.output), f.Inputs(),
      NativeFrameExecutionTestAccess::ModelProof(data));
}
void ReplaceModelPrimary(Fixture& f, const std::vector<uint32_t>& words) {
  REQUIRE(f.session->AbortUnsubmitted(f.thread, f.source));
  f.primary = Bytes(words);
  REQUIRE(f.primary.size() < f.rectangle_offset);
  std::memcpy(f.memory.TranslatePhysical(f.record.address), f.primary.data(),
              f.primary.size());
  const auto analyzed = Plan::Analyze(f.primary);
  REQUIRE(analyzed);
  f.plan = analyzed.plan;
  f.Freeze();
}
void RewriteModelRegister(Fixture& f, uint32_t index, uint32_t value) {
  std::vector<uint32_t> words(f.plan->words().begin(), f.plan->words().end());
  bool found = false;
  for (uint32_t at = 0; at < words.size();) {
    const uint32_t header = words[at], type = header >> 30;
    const uint32_t count = ((header >> 16) & 0x3FFF) + 1;
    const uint32_t size = type == 2 ? 1 : type == 1 ? 3 : count + 1;
    if (type == 0) {
      const uint32_t first = header & 0x7FFF;
      if (index >= first && index - first < count) {
        words[at + 1 + index - first] = value;
        found = true;
      }
    }
    at += size;
  }
  REQUIRE(found);
  ReplaceModelPrimary(f, words);
}
std::shared_ptr<const gpu::NativeFrameInputSnapshot> ModelOverlaySnapshot(
    Fixture& f) {
  auto capture = f.CaptureInputs();
  REQUIRE(capture);
  REQUIRE(capture->Capture(f.record));
  auto snapshot = capture->Seal();
  REQUIRE(snapshot);
  return snapshot;
}
OutputCaptureFrame PrepareModelOverlayCapture(Fixture& f,
                                              gpu::GpuCompletionQueue& queue) {
  f.graph->epoch = queue.backend_epoch();
  f.graph->accepts_model_submission = true;
  const auto data = ModelOverlayProofData(f);
  const auto layout = DeriveModelOverlay(f, data);
  REQUIRE(layout);
  auto snapshot = ModelOverlaySnapshot(f);
  auto request = NativeFrameExecutionTestAccess::Mint(
      f.session, f.thread, f.source, f.plan, f.output, f.Inputs(), f.shaders,
      layout, snapshot);
  REQUIRE(request);
  REQUIRE(request->MarkSubmissionAttempted());
  const auto ticket = queue.TrackNativeFrameSubmitted(11, request->identity());
  REQUIRE(f.session->AttachGpu(f.thread, f.source, ticket));
  REQUIRE(f.session->BeginRetire(f.thread, f.source));
  queue.CompleteThrough(11);
  gpu::NativeFrameExecutionResult execution;
  execution.status = gpu::NativeFrameExecutionResult::Status::kSubmitted;
  execution.copy_operations = 1;
  execution.written_range = {f.output.address, layout->required_bytes};
  execution.title_state_restored = true;
  execution.completion = ticket;
  return {std::move(request), std::move(execution)};
}

TEST_CASE("Model overlay proof retains real pinned storage and raw zero VD",
          "[kernel][native_overlay_model][native_frame_execution]") {
  Fixture f(false, true, ModelOverlayGeometry(), true);
  REQUIRE(f.primary.size() > 0x100);
  REQUIRE(f.primary.size() < f.rectangle_offset);
  REQUIRE(f.rectangle_offset == 0x800);
  const auto data = ModelOverlayProofData(f);
  const auto layout = DeriveModelOverlay(f, data);
  REQUIRE(layout);
  CHECK(layout->origin ==
        gpu::NativeFrameOutputLayout::Origin::kNativeOverlayResolve);
  CHECK_FALSE(layout->original_texture_fetch);
  REQUIRE(layout->pm4_resolve);
  CHECK(layout->pm4_resolve->output_origin() ==
        gpu::NativeFramePm4ResolveProof::OutputOrigin::kNativeOverlay);
  CHECK(std::all_of(layout->pm4_resolve->descriptor().begin() + 8,
                    layout->pm4_resolve->descriptor().end(),
                    [](uint8_t b) { return !b; }));
  CHECK(layout->width == 729);
  CHECK(layout->height == 90);
  CHECK(layout->pitch == 736);
  REQUIRE(layout->overlay_state);
  CHECK(layout->overlay_state->Has(gpu::NativeOverlayState::kGeometryKnown));
  CHECK(layout->overlay_state->enable == 0);  // Requested, not visible.
  REQUIRE(layout->overlay_presentation);
  CHECK(layout->overlay_presentation->canvas_width == 1280);
  CHECK(layout->overlay_presentation->canvas_height == 720);
  CHECK(layout->overlay_presentation->x == 256);
  CHECK(layout->overlay_presentation->y == 576);
  auto snapshot = ModelOverlaySnapshot(f);
  std::memset(f.memory.TranslatePhysical(f.record.address + f.rectangle_offset),
              0xFF, 24);
  auto request = NativeFrameExecutionTestAccess::Mint(
      f.session, f.thread, f.source, f.plan, f.output, f.Inputs(), f.shaders,
      layout, snapshot);
  REQUIRE(request);
  CHECK(request->IsAdmitted());
  CHECK(request->input_snapshot() == snapshot);
  CHECK(f.memory.ReleaseOwnedPhysicalAllocation(f.session->owner(),
                                                f.output_id) == Status::kPinned);
  CHECK_FALSE(f.session->GetSnapshot().submission_attempted);
}

TEST_CASE("Model overlay rejects altered storage proof metadata",
          "[kernel][native_overlay_model][native_frame_execution]") {
  Fixture f(false, true, ModelOverlayGeometry(), true);
  auto data = ModelOverlayProofData(f);
  SECTION("canonical target cannot name a different physical output") {
    data.output.observed += 0x1000;
  }
  SECTION("primary mapping must be the actual copied command range") {
    data.primary.observed += 0x1000;
  }
  SECTION("optional VD output cannot be fabricated") { data.descriptor[8] = 1; }
  SECTION("all absent VD bytes remain zero") { data.descriptor[147] = 1; }
  SECTION("exact primary command length") { --data.descriptor[7]; }
  SECTION("same final-copy packet") { ++data.copy_packet; }
  SECTION("same owned resolve rectangle") { data.rectangle.address += 4; }
  SECTION("native origin cannot be relabeled as VD") {
    data.origin = gpu::NativeFramePm4ResolveProof::OutputOrigin::kVdTexture;
  }
  CHECK_FALSE(DeriveModelOverlay(f, data));
}

TEST_CASE("Model overlay geometry must match current resolve and placement",
          "[kernel][native_overlay_model][native_frame_execution]") {
  Fixture f(false, true, ModelOverlayGeometry(), true);
  auto data = ModelOverlayProofData(f);
  SECTION("resolve target") {
    RewriteModelRegister(f, gpu::XE_GPU_REG_RB_COPY_DEST_BASE,
                           f.output.address + 0x1000);
  }
  SECTION("resolve pitch") {
    RewriteModelRegister(f, gpu::XE_GPU_REG_RB_COPY_DEST_PITCH,
                           (90u << 16) | 704u);
  }
  SECTION("resolve height") {
    RewriteModelRegister(f, gpu::XE_GPU_REG_RB_COPY_DEST_PITCH,
                           (89u << 16) | 736u);
  }
  SECTION("overlay target") {
    RewriteModelRegister(f, gpu::XE_GPU_REG_D1OVL_SURFACE_ADDRESS,
                           f.output.address + 0x1000);
  }
  SECTION("overlay pitch") {
    RewriteModelRegister(f, gpu::XE_GPU_REG_D1OVL_PITCH, 704);
  }
  SECTION("logical window cannot resize the real resolve") {
    RewriteModelRegister(f, gpu::XE_GPU_REG_D1OVL_END,
                           (986u << 16) | 666u);
  }
  SECTION("placement phase must match native offsets") {
    RewriteModelRegister(f, gpu::XE_GPU_REG_D1OVL_START,
                           (512u << 16) | 576u);
  }
  data.identity = {f.session->owner(), f.source->serial()};
  CHECK_FALSE(DeriveModelOverlay(f, data));
}

TEST_CASE("Model overlay canvas comes from the declared display profile",
          "[kernel][native_overlay_model][native_frame_execution]") {
  Fixture f(false, true, ModelOverlayGeometry(), true);
  auto data = ModelOverlayProofData(f);
  SECTION("unknown canvas") { data.display[0] = data.display[1] = 0; }
  SECTION("different scaler extent") { ModelDisplayWord(data.display, 16, 1279); }
  SECTION("unsupported scaler offset") { ModelDisplayWord(data.display, 8, 1); }
  SECTION("different secondary canvas") { data.display[0x49] ^= 1; }
  SECTION("unsupported display format") { data.display[4] = 1; }
  CHECK_FALSE(DeriveModelOverlay(f, data));
}

TEST_CASE("Prior requested overlay state cannot supply missing current geometry",
          "[kernel][native_overlay_model][native_frame_execution]") {
  Fixture f(false, true, ModelOverlayGeometry(), true);
  auto data = ModelOverlayProofData(f);
  SECTION("valid partial prior state stays unknown until this program writes it") {
    gpu::NativeOverlayState prior;
    prior.Set(gpu::NativeOverlayState::Field::kEnable, 1);
    data.input_state = prior;
    const auto layout = DeriveModelOverlay(f, data);
    REQUIRE(layout);
    CHECK(layout->overlay_state->Has(gpu::NativeOverlayState::kGeometryKnown));
  }
  SECTION("unknown prior mask is refused") {
    gpu::NativeOverlayState prior;
    prior.known_mask = 1u << 31;
    data.input_state = prior;
    CHECK_FALSE(DeriveModelOverlay(f, data));
  }
  SECTION("foreign prior geometry is overridden by complete current geometry") {
    const auto initial = DeriveModelOverlay(f, data);
    REQUIRE(initial);
    auto prior = *initial->overlay_state;
    prior.surface_address += 0x1000;
    prior.pitch += 32;
    data.input_state = prior;
    const auto current = DeriveModelOverlay(f, data);
    REQUIRE(current);
    CHECK(current->overlay_state->surface_address == f.output.address);
    CHECK(current->overlay_state->pitch == 736);
  }
  SECTION("even matching prior geometry cannot replace missing current batch") {
    const auto initial = DeriveModelOverlay(f, data);
    REQUIRE(initial);
    data.input_state = *initial->overlay_state;
    const auto& transaction = f.plan->native_overlay_program()->transactions()[1];
    std::vector<uint32_t> words(f.plan->words().begin(), f.plan->words().end());
    words.erase(words.begin() + transaction.lock_packet_dword_offset,
                words.begin() + transaction.unlock_packet_dword_offset + 2);
    ReplaceModelPrimary(f, words);
    data.identity = {f.session->owner(), f.source->serial()};
    Fixture::DescriptorWord(data.descriptor, 4, uint32_t(f.primary.size() / 4));
    CHECK_FALSE(DeriveModelOverlay(f, data));
  }
}

TEST_CASE("Model overlay layout cannot overstate pinned extent or Source",
          "[kernel][native_overlay_model][native_frame_execution]") {
  Fixture f(false, true, ModelOverlayGeometry(), true);
  auto data = ModelOverlayProofData(f);
  const auto layout = DeriveModelOverlay(f, data);
  REQUIRE(layout);
  const std::array ranges{f.output};
  const auto pin = f.memory.PinOwnedPhysicalRanges(f.session->owner(), ranges);
  REQUIRE(pin);
  REQUIRE(pin.pin->allocations().size() == 1);
  CHECK(pin.pin->allocations()[0].allocation_id == f.output_id);
  CHECK(pin.pin->allocations()[0].length == f.output.length);
  CHECK_FALSE(DeriveModelOverlay(
      f, data, Range{f.output.address, layout->required_bytes - 1}));
  REQUIRE(uint64_t(f.record.address) + f.record.length <= f.output.address);
  auto too_large = f.output;
  ++too_large.length;
  const auto overstated = DeriveModelOverlay(f, data, too_large);
  REQUIRE(overstated);  // Pure byte-layout validation is not allocation authority.
  CHECK_FALSE(NativeFrameExecutionTestAccess::Mint(
      f.session, f.thread, f.source, f.plan, too_large, f.Inputs(), f.shaders,
      overstated));
  ++data.identity.source_serial;
  const auto foreign_source = DeriveModelOverlay(f, data);
  REQUIRE(foreign_source);
  CHECK_FALSE(NativeFrameExecutionTestAccess::Mint(
      f.session, f.thread, f.source, f.plan, f.output, f.Inputs(), f.shaders,
      foreign_source));
}

TEST_CASE("Model overlay request rederives origin state and placement",
          "[kernel][native_overlay_model][native_frame_execution]") {
  Fixture f(false, true, ModelOverlayGeometry(), true);
  const auto data = ModelOverlayProofData(f);
  const auto layout = DeriveModelOverlay(f, data);
  REQUIRE(layout);
  auto changed = *layout;
  SECTION("distinct proof origin") {
    changed.origin = gpu::NativeFrameOutputLayout::Origin::kPm4Resolve;
  }
  SECTION("requested state") { changed.overlay_state->pitch += 32; }
  SECTION("placement") { changed.overlay_presentation->x += 256; }
  SECTION("canvas") { ++changed.overlay_presentation->canvas_width; }
  CHECK_FALSE(NativeFrameExecutionTestAccess::Mint(
      f.session, f.thread, f.source, f.plan, f.output, f.Inputs(), f.shaders,
      changed));
  CHECK(NativeFrameExecutionTestAccess::Mint(
      f.session, f.thread, f.source, f.plan, f.output, f.Inputs(), f.shaders,
      layout));
}

TEST_CASE("Model overlay completed readback serializes distinct truthful origin",
          "[kernel][native_overlay_model][native_output_capture]") {
  Fixture f(false, true, ModelOverlayGeometry(), true);
  gpu::GpuCompletionQueue queue;
  const auto frame = PrepareModelOverlayCapture(f, queue);
  auto capture = SnapshotNativeFrameOutputCapture(*frame.request, frame.execution);
  REQUIRE(capture);
  REQUIRE(CompleteNativeFrameOutputCapture(*capture, OutputCaptureReadback(frame)));
  REQUIRE(AssociateNativeFrameOutputCapture(*capture, OutputCapturePresentation(frame)));
  OutputCaptureScratchDirectory scratch;
  const auto base = scratch.path / "model_overlay";
  REQUIRE(SaveNativeFrameOutputCapture(base, *capture));
  const auto bytes = OutputCaptureFileBytes(OutputCaptureFile(base, ".readback.json"));
  const std::string json(bytes.begin(), bytes.end());
  CHECK(json.find("\"output_layout_origin_name\":\"native_overlay_resolve\"") != std::string::npos);
  CHECK(json.find("\"pm4_output_origin\":\"native_overlay\"") != std::string::npos);
  CHECK(json.find("\"original_texture_fetch\":null") != std::string::npos);
  const auto array = [](const auto& values) {
    std::string result = "[";
    for (const auto value : values) {
      if (result.size() != 1) result += ',';
      result += std::to_string(value);
    }
    return result + ']';
  };
  const auto& proof = *frame.request->output_layout()->pm4_resolve;
  CHECK(json.find("\"vd_descriptor_raw_bytes\":" + array(proof.descriptor())) != std::string::npos);
  CHECK(json.find("\"display_information_raw_bytes\":" + array(proof.display_information())) != std::string::npos);
  CHECK(json.find("\"overlay_output_requested_state\":{") != std::string::npos);
  CHECK(json.find("\"overlay_presentation\":{") != std::string::npos);
  CHECK(json.find("\"pixels_verified\":false") != std::string::npos);
  CHECK(json.find("\"composition_verified\":false") != std::string::npos);
  CHECK_FALSE(capture->presenter_accepted);
  CHECK(f.session->RetireComplete(f.thread, f.source));
}

TEST_CASE("Model overlay readback preserves completion and detached state checks",
          "[kernel][native_overlay_model][native_output_capture]") {
  Fixture f(false, true, ModelOverlayGeometry(), true);
  gpu::GpuCompletionQueue queue;
  const auto frame = PrepareModelOverlayCapture(f, queue);
  auto capture = SnapshotNativeFrameOutputCapture(*frame.request, frame.execution);
  REQUIRE(capture);
  SECTION("completion remains mandatory") {
    auto readback = OutputCaptureReadback(frame);
    readback.status = gpu::NativeFrameReadbackResult::Status::kUnsupported;
    CHECK_FALSE(CompleteNativeFrameOutputCapture(*capture, std::move(readback)));
    CHECK_FALSE(capture->readback_complete);
  }
  SECTION("Source tag cannot be changed in completed readback") {
    auto readback = OutputCaptureReadback(frame);
    auto identity = frame.request->identity();
    ++identity.source_serial;
    readback.native_completion = queue.TrackNativeFrameSubmitted(12, identity);
    queue.CompleteThrough(12);
    CHECK_FALSE(CompleteNativeFrameOutputCapture(*capture, std::move(readback)));
  }
  SECTION("detached state and placement are still layout storage facts") {
    REQUIRE(CompleteNativeFrameOutputCapture(*capture, OutputCaptureReadback(frame)));
    REQUIRE(AssociateNativeFrameOutputCapture(*capture, OutputCapturePresentation(frame)));
    capture->layout.overlay_presentation->canvas_width = 0;
    OutputCaptureScratchDirectory scratch;
    const auto base = scratch.path / "invalid_placement";
    CHECK_FALSE(SaveNativeFrameOutputCapture(base, *capture));
    CHECK_FALSE(std::filesystem::exists(OutputCaptureFile(base, ".readback.bin")));
  }
  CHECK(f.session->RetireComplete(f.thread, f.source));
}

// Host admission/completion model with actual Memory pins. This does not
// construct native control provenance, a native Task, or an actual GPU fence.
void UseControlPrimary(Fixture& f) {
  REQUIRE(f.session->AbortUnsubmitted(f.thread, f.source));
  f.source.reset();
  f.primary = Bytes({0x00022080, 0,          0,          0x03D40354, 0x00000A31,
                     0x03000000, 0x00010A2F, 0x20000000, 0,          0xC0043C00,
                     3,          0xA31,      0,          0x80000000, 8,
                     0x00022080, 0,          0,          0x03D40354, 0x0000186B,
                     0x10000,    0x000218C1, 0,          255,        0,
                     0x00001860, 0,          0x0000186B, 0});
  f.shaders.clear();
  std::memcpy(f.memory.TranslatePhysical(f.record.address), f.primary.data(),
              f.primary.size());
  const auto analyzed = Plan::Analyze(f.primary);
  REQUIRE(analyzed);
  REQUIRE(analyzed.plan->is_overlay_control());
  f.plan = analyzed.plan;
  auto update = f.session->BeginUpdate(f.thread);
  REQUIRE(update);
  f.source = f.session->Freeze(f.thread, update, f.primary);
  REQUIRE(f.source);
  const auto pin = f.memory.PinOwnedPhysicalRanges(
      f.session->owner(), std::span<const Range>(&f.record, 1));
  REQUIRE(pin);
  REQUIRE(f.session->AttachPhysicalPin(f.thread, f.source, pin.pin));
}
std::shared_ptr<const gpu::NativeFrameInputSnapshot> CaptureControl(
    Fixture& f) {
  auto capture = NativeFrameExecutionTestAccess::Capture(
      &f.memory, f.session, f.thread, f.source, f.plan,
      {f.record.address, uint32_t(f.primary.size())}, {});
  REQUIRE(capture);
  REQUIRE(capture->Capture(f.record));
  auto result = capture->Seal();
  REQUIRE(result);
  CHECK(result->valid());
  return result;
}
std::shared_ptr<const Request> MintControl(
    Fixture& f,
    const std::shared_ptr<const gpu::NativeFrameInputSnapshot>& snapshot) {
  return NativeFrameExecutionTestAccess::Mint(
      f.session, f.thread, f.source, f.plan, {}, {f.record}, {}, {}, snapshot);
}
TEST_CASE("Control Source owns immutable inputs without inventing output",
          "[kernel][native_frame_execution][control]") {
  Fixture f;
  UseControlPrimary(f);
  const auto snapshot = CaptureControl(f);
  const auto request = MintControl(f, snapshot);
  REQUIRE(request);
  CHECK(request->IsAdmitted());
  CHECK(request->output_range().address == 0);
  CHECK(request->output_range().length == 0);
  CHECK_FALSE(request->output_layout());
  CHECK(request->shaders().empty());
  CHECK(snapshot->output_range().length == 0);
  CHECK(snapshot->plan() == request->plan());
  CHECK(f.memory.ReleaseOwnedPhysicalAllocation(
            f.session->owner(), f.record_id) == Status::kPinned);
  f.memory.TranslatePhysical<uint8_t*>(f.record.address)[0] ^= 1;
  std::array<uint8_t, 4> first;
  REQUIRE(snapshot->Read({f.record.address, 4}, first));
  CHECK(first[0] == f.primary[0]);
  CHECK(request->IsAdmitted());
}
TEST_CASE("Absent output is exclusive to a complete control Source",
          "[kernel][native_frame_execution][control]") {
  Fixture f;
  SECTION("render cannot omit output") {
    CHECK_FALSE(NativeFrameExecutionTestAccess::Mint(
        f.session, f.thread, f.source, f.plan, {}, f.Inputs(), f.shaders));
  }
  SECTION("control cannot claim output or image layout") {
    UseControlPrimary(f);
    const auto snapshot = CaptureControl(f);
    CHECK_FALSE(NativeFrameExecutionTestAccess::Mint(
        f.session, f.thread, f.source, f.plan, f.output, {f.record}, {}, {},
        snapshot));
    gpu::NativeFrameOutputLayout layout;
    CHECK_FALSE(NativeFrameExecutionTestAccess::Mint(
        f.session, f.thread, f.source, f.plan, {}, {f.record}, {}, layout,
        snapshot));
    CHECK_FALSE(NativeFrameExecutionTestAccess::Capture(
        &f.memory, f.session, f.thread, f.source, f.plan,
        {f.record.address, uint32_t(f.primary.size())}, f.output));
    CHECK_FALSE(NativeFrameExecutionTestAccess::Mint(
        f.session, f.thread, f.source, f.plan, {}, {f.output}, {}, {},
        snapshot));
  }
}
TEST_CASE("Control Source still consumes one host ticket and waits retirement",
          "[kernel][native_frame_execution][control]") {
  Fixture f;
  gpu::GpuCompletionQueue queue;
  f.graph->epoch = queue.backend_epoch();
  f.graph->accepts_model_submission = true;
  UseControlPrimary(f);
  const auto request = MintControl(f, CaptureControl(f));
  REQUIRE(request);
  REQUIRE(request->MarkSubmissionAttempted());
  CHECK_FALSE(request->MarkSubmissionAttempted());
  const auto ticket = queue.TrackNativeFrameSubmitted(11, request->identity());
  REQUIRE(f.session->AttachGpu(f.thread, f.source, ticket));
  CHECK_FALSE(f.session->BeginUpdate(f.thread));
  CHECK_FALSE(f.session->RetireComplete(f.thread, f.source));
  queue.CompleteThrough(11);
  CHECK(ticket.status() == gpu::GpuCompletionStatus::kComplete);
  CHECK_FALSE(request->AcquireCompletedReadback(ticket));
  REQUIRE(f.session->RetireComplete(f.thread, f.source));
  CHECK_FALSE(request->IsAdmitted());
  CHECK(f.session->BeginUpdate(f.thread));
}
TEST_CASE(
    "Control admission rejects cancellation stale backend and foreign inputs",
    "[kernel][native_frame_execution][control]") {
  Fixture f;
  UseControlPrimary(f);
  const auto snapshot = CaptureControl(f);
  SECTION("foreign range is not covered by record pins") {
    CHECK_FALSE(NativeFrameExecutionTestAccess::Mint(
        f.session, f.thread, f.source, f.plan, {}, {{0x100000, 4096}}, {}, {},
        snapshot));
    // A failed range admission did not consume the otherwise valid Source.
    CHECK(MintControl(f, snapshot));
  }
  SECTION("owner canceled before submission") {
    const auto request = MintControl(f, snapshot);
    REQUIRE(request);
    REQUIRE(f.session->AbortUnsubmitted(f.thread, f.source));
    CHECK_FALSE(request->IsAdmitted());
    CHECK_FALSE(request->MarkSubmissionAttempted());
  }
  SECTION("backend generation changed") {
    const auto request = MintControl(f, snapshot);
    REQUIRE(request);
    ++f.graph->epoch;
    CHECK_FALSE(request->IsAdmitted());
    CHECK_FALSE(request->MarkSubmissionAttempted());
  }
}
TEST_CASE("Control operation accounting has no draw-minus-copy underflow",
          "[kernel][native_frame_execution][control]") {
  Fixture f;
  const auto render_plan = f.plan;
  UseControlPrimary(f);
  gpu::NativeFrameExecutionResult result;
  CHECK(gpu::NativeFrameOperationsMatchPlan(*f.plan, result));
  CHECK_FALSE(gpu::NativeFrameOperationsMatchPlan(*render_plan, result));
  SECTION("draw cannot be mislabeled control") { result.draw_commands = 1; }
  SECTION("copy cannot be mislabeled control") { result.copy_operations = 1; }
  SECTION("clear cannot be mislabeled control") { result.clear_operations = 1; }
  SECTION("old output cannot be attached") { result.written_range = f.output; }
  SECTION("nonempty address with zero length is not absent") {
    result.written_range.address = 1;
  }
  CHECK_FALSE(gpu::NativeFrameOperationsMatchPlan(*f.plan, result));
}
}  // namespace

TEST_CASE("Explicit exact-Source diagnostic copies frozen primary and real "
          "inputs without BDown",
          "[kernel][native_frame_execution][native_output_capture]") {
  Fixture f(true, true);
  gpu::GpuCompletionQueue queue;
  f.graph->epoch = queue.backend_epoch();
  const auto title = ModelTitleInput(queue.backend_epoch(), 0x10000000, 64, 64);
  RefreezeWithTitle(f, title);
  auto frame = PrepareOutputCaptureFrame(f, queue);
  auto capture =
      SnapshotNativeFrameOutputCapture(*frame.request, frame.execution);
  REQUIRE(capture);
  const auto serial = f.source->serial();
  REQUIRE(serial);
  const auto primary = std::vector<uint8_t>(f.source->primary().begin(),
                                            f.source->primary().end());
  REQUIRE(AttachNativeFrameOutputExactSourceDiagnostic(
      *capture, *f.source,
      NativeFrameOutputExactSourceDiagnosticTrigger{serial,
                                                    title->identity()}));
  CHECK_FALSE(capture->b_down_trigger);
  REQUIRE(capture->exact_source_diagnostic);
  CHECK(capture->exact_source_diagnostic->source_serial == serial);
  CHECK(capture->primary_identity == frame.request->identity());
  CHECK(capture->primary_bytes == primary);
  REQUIRE(AttachNativeFrameOutputInputs(*capture, *frame.request));
  REQUIRE(capture->inputs);
  REQUIRE(capture->inputs->title);
  CHECK(capture->inputs->title->identity == title->identity());
  CHECK(capture->inputs->title->identity.foreground_generation ==
        capture->exact_source_diagnostic->title_input.foreground_generation);
  REQUIRE(capture->inputs->shaders.size() == frame.request->shaders().size());
  REQUIRE_FALSE(capture->inputs->shaders.empty());
  const auto inputs = capture->inputs->bytes;
  const auto &shader = capture->inputs->shaders.front();
  CHECK(std::vector<uint8_t>(inputs.begin() + shader.offset,
                             inputs.begin() + shader.offset + shader.length) ==
        frame.request->shaders().front().code);
  CHECK_FALSE(AttachNativeFrameOutputExactSourceDiagnostic(
      *capture, *f.source,
      NativeFrameOutputExactSourceDiagnosticTrigger{serial,
                                                    title->identity()}));
  CHECK_FALSE(AttachNativeFrameOutputPrimary(
      *capture, *f.source, OutputCaptureBDownTrigger(*capture)));
  REQUIRE(
      CompleteNativeFrameOutputCapture(*capture, OutputCaptureReadback(frame)));
  REQUIRE(AssociateNativeFrameOutputCapture(*capture,
                                            OutputCapturePresentation(frame)));
  REQUIRE(f.session->RetireComplete(f.thread, f.source));
  frame.request.reset();
  f.source.reset();
  std::memset(f.primary.data(), 0xCD, f.primary.size());
  std::memset(f.memory.TranslatePhysical(f.record.address), 0xCD,
              f.record.length);
  OutputCaptureScratchDirectory scratch;
  const auto base = scratch.path / "exact_source_diagnostic";
  REQUIRE(SaveNativeFrameOutputCapture(base, *capture));
  CHECK(OutputCaptureFileBytes(OutputCaptureFile(base, ".primary.bin")) ==
        primary);
  CHECK(OutputCaptureFileBytes(OutputCaptureFile(base, ".inputs.bin")) ==
        inputs);
  const auto metadata_bytes =
      OutputCaptureFileBytes(OutputCaptureFile(base, ".readback.json"));
  const std::string metadata(metadata_bytes.begin(), metadata_bytes.end());
  CHECK(metadata.find("explicit_exact_source_diagnostic") != std::string::npos);
  CHECK(metadata.find("\"technical_acceptance\":false") != std::string::npos);
  CHECK(metadata.find("\"feature_acceptance\":false") != std::string::npos);
  CHECK(metadata.find("observed_b_down_before_source_freeze") ==
        std::string::npos);
}
TEST_CASE("Exact-Source diagnostic rejects wrong identity trigger and partial "
          "detached input",
          "[kernel][native_frame_execution][native_output_capture]") {
  Fixture f(true, true);
  gpu::GpuCompletionQueue queue;
  f.graph->epoch = queue.backend_epoch();
  const auto title = ModelTitleInput(queue.backend_epoch(), 0x10000000, 64, 64);
  RefreezeWithTitle(f, title);
  auto frame = PrepareOutputCaptureFrame(f, queue);
  auto capture =
      SnapshotNativeFrameOutputCapture(*frame.request, frame.execution);
  REQUIRE(capture);
  auto trigger = NativeFrameOutputExactSourceDiagnosticTrigger{
      f.source->serial(), title->identity()};
  SECTION("missing Title identity") { trigger.title_input = {}; }
  SECTION("missing foreground") {
    trigger.title_input.foreground_generation = 0;
  }
  SECTION("other foreground") { ++trigger.title_input.foreground_generation; }
  SECTION("other Title module") { ++trigger.title_input.title_hmodule; }
  SECTION("other Title load") { ++trigger.title_input.title_load_generation; }
  SECTION("other Title swap") { ++trigger.title_input.swap_serial; }
  SECTION("other Title backend") { ++trigger.title_input.backend_epoch; }
  SECTION("zero requested Source") { trigger.source_serial = 0; }
  SECTION("other requested Source") { ++trigger.source_serial; }
  SECTION("other output owner") { ++capture->identity.owner.session_id; }
  SECTION("other output Source") { ++capture->identity.source_serial; }
  SECTION("already observed BDown") {
    REQUIRE(AttachNativeFrameOutputPrimary(
        *capture, *f.source, OutputCaptureBDownTrigger(*capture)));
  }
  CHECK_FALSE(AttachNativeFrameOutputExactSourceDiagnostic(*capture, *f.source,
                                                           trigger));
  CHECK_FALSE(capture->exact_source_diagnostic);
  CHECK_FALSE(capture->inputs);
  CHECK(f.session->RetireComplete(f.thread, f.source));
}

TEST_CASE("Exact-source Title bundle rejects detached mismatch or missing "
          "bytes before writes",
          "[kernel][native_frame_execution][native_output_capture]") {
  Fixture f(true, true);
  gpu::GpuCompletionQueue queue;
  f.graph->epoch = queue.backend_epoch();
  const auto title = ModelTitleInput(queue.backend_epoch(), 0x10000000, 64, 64);
  RefreezeWithTitle(f, title);
  auto frame = PrepareOutputCaptureFrame(f, queue);
  auto capture =
      SnapshotNativeFrameOutputCapture(*frame.request, frame.execution);
  REQUIRE(capture);
  REQUIRE(AttachNativeFrameOutputExactSourceDiagnostic(
      *capture, *f.source,
      NativeFrameOutputExactSourceDiagnosticTrigger{f.source->serial(),
                                                    title->identity()}));
  REQUIRE(AttachNativeFrameOutputInputs(*capture, *frame.request));
  REQUIRE(capture->inputs->title);
  REQUIRE(
      CompleteNativeFrameOutputCapture(*capture, OutputCaptureReadback(frame)));
  REQUIRE(AssociateNativeFrameOutputCapture(*capture,
                                            OutputCapturePresentation(frame)));
  SECTION("wrong detached foreground") {
    ++capture->exact_source_diagnostic->title_input.foreground_generation;
  }
  SECTION("missing detached identity") {
    capture->exact_source_diagnostic->title_input = {};
  }
  SECTION("missing Title block") { capture->inputs->title.reset(); }
  SECTION("missing entire bundle") { capture->inputs.reset(); }
  SECTION("missing Title bytes") { capture->inputs->bytes.pop_back(); }
  REQUIRE(f.session->RetireComplete(f.thread, f.source));
  frame.request.reset();
  f.source.reset();
  OutputCaptureScratchDirectory scratch;
  const auto base = scratch.path / "bad_exact_source_title";
  CHECK_FALSE(SaveNativeFrameOutputCapture(base, *capture));
  CHECK_FALSE(
      std::filesystem::exists(OutputCaptureFile(base, ".readback.bin")));
  CHECK_FALSE(std::filesystem::exists(OutputCaptureFile(base, ".primary.bin")));
  CHECK_FALSE(std::filesystem::exists(OutputCaptureFile(base, ".inputs.bin")));
  CHECK_FALSE(
      std::filesystem::exists(OutputCaptureFile(base, ".readback.json")));
}

}  // namespace xe::kernel::test
