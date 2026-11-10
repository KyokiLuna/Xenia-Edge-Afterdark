/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia contributors. All rights reserved.                    *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */
#include <array>
#include <cstring>
#include <initializer_list>
#include <memory>
#include <span>
#include <utility>
#include <vector>

#include "third_party/catch/include/catch.hpp"
#include "xenia/gpu/native_draw_read_inventory.h"
#include "xenia/gpu/native_shader_read_analysis.h"
#include "xenia/gpu/native_vertex_access_analysis.h"
#include "xenia/gpu/texture_info.h"
#include "xenia/gpu/texture_util.h"
#include "xenia/gpu/ucode.h"
#include "xenia/kernel/native_frame_lease_probe.h"
#include "xenia/kernel/testing/native_frame_execution_test_access.h"
#include "xenia/kernel/testing/native_system_title_input_test_access.h"
#include "xenia/memory.h"

namespace xe::kernel::test {
namespace {
using Session = NativeFrameOwnerSession;
using Plan = gpu::NativeFrameCommandPlan;
using Request = gpu::NativeFrameExecutionRequest;
using Inventory = gpu::NativeDrawReadInventory;
using Range = memory::PhysicalRange;
using Words = std::vector<uint32_t>;
using Instruction = std::array<uint32_t, 3>;
namespace xenos = gpu::xenos;
namespace ucode = gpu::ucode;

std::vector<uint8_t> Bytecode(const Words& words) {
  std::vector<uint8_t> bytes;
  for (uint32_t w : words) {
    bytes.push_back(uint8_t(w >> 24));
    bytes.push_back(uint8_t(w >> 16));
    bytes.push_back(uint8_t(w >> 8));
    bytes.push_back(uint8_t(w));
  }
  return bytes;
}
Words Program(std::initializer_list<Instruction> body, uint32_t sequence) {
  Words words{1u | uint32_t(body.size()) << 12 | sequence << 16,
              uint32_t(ucode::ControlFlowOpcode::kExecEnd) << 12 |
                  uint32_t(ucode::AddressingMode::kAbsolute) << 11,
              0};
  for (const auto& op : body) {
    words.insert(words.end(), op.begin(), op.end());
  }
  return words;
}
Words PublicVertexProgram() {
  // Public bitfields, not captured program bytes. Both fetches precede ALU.
  const uint32_t binding = (95u / 3) << 20 | (95u % 3) << 25;
  const auto format = uint32_t(xenos::VertexFormat::k_32_32_FLOAT) << 16;
  const Instruction full{1u << 19 | binding | 1u << 12, format | 0xB08, 4};
  const Instruction mini{1u << 19 | 2u << 12, format | 1u << 30 | 0xFC8,
                         2u << 8 | 4};
  const Instruction position{
      uint32_t(ucode::ExportRegister::kVSPosition) | 1u << 15 | 15u << 16 |
          uint32_t(ucode::AluScalarOpcode::kRetainPrev) << 26,
      0,
      1u << 16 | 1u << 8 | uint32_t(ucode::AluVectorOpcode::kMax) << 24 |
          7u << 29};
  return Program({full, mini, position}, 5);
}
Words PublicPixelProgram() {
  const Instruction sample{
      uint32_t(ucode::FetchOpcode::kTextureFetch) | 1u << 19,
      0x688 | 3u << 12 | 3u << 14 | 3u << 16 | 7u << 18 | 3u << 24 | 3u << 26 |
          1u << 28,
      uint32_t(xenos::FetchOpDimension::k2D) << 14};
  const Instruction color{
      uint32_t(ucode::ExportRegister::kPSColor0) | 1u << 15 | 15u << 16 |
          uint32_t(ucode::AluScalarOpcode::kRetainPrev) << 26,
      0, uint32_t(ucode::AluVectorOpcode::kMax) << 24 | 7u << 29};
  return Program({sample, color}, 1);
}
xenos::xe_gpu_texture_fetch_t OrdinaryFetch(uint32_t address,
                                            bool rgba = false) {
  xenos::xe_gpu_texture_fetch_t f{};
  f.type = xenos::FetchConstantType::kTexture;
  f.dimension = xenos::DataDimension::k2DOrStacked;
  f.clamp_x = f.clamp_y = xenos::ClampMode::kClampToEdge;
  f.format = rgba ? xenos::TextureFormat::k_8_8_8_8 : xenos::TextureFormat::k_8;
  f.pitch = (rgba ? 128 : 256) >> 5;
  f.size_2d.width = rgba ? 99 : 32;
  f.size_2d.height = rgba ? 249 : 1;
  f.base_address = address >> 12;
  f.mag_filter = f.min_filter = f.mip_filter = xenos::TextureFilter::kLinear;
  f.endianness = rgba ? xenos::Endian::k8in32 : xenos::Endian::kNone;
  f.swizzle = rgba ? 0x60A : 0x688;
  return f;
}

class HostGraph final : public NativeFrameGraphAuthority {
 public:
  explicit HostGraph(Session::Owner owner) : owner_(owner) {}
  bool Matches(const Session::Owner& owner) const override {
    return owner == owner_;
  }
  uint64_t backend_epoch() const override { return 42; }
  bool CanExecuteNativeFrame(const NativeFrameSource& source) const override {
    return source.owner() == owner_;
  }
  bool MatchesTitleInput(
      const gpu::NativeSystemTitleInput& input) const override {
    return input.valid() && input.identity().backend_epoch == 42 &&
           input.identity().foreground_generation == 17 &&
           input.identity().title_hmodule == 0x3001C000 &&
           input.identity().title_load_generation == 23;
  }
  bool AbortUnsubmitted(const NativeFrameSource& source) override {
    return source.owner() == owner_;
  }
  bool RetireAfterKernelStop(const Session::Owner& owner) override {
    return owner == owner_;
  }

 private:
  Session::Owner owner_;
};

class ResourceFixture {
 public:
  ResourceFixture() {
    REQUIRE(memory.Initialize());
    // Existing host Session mechanism: opaque markers are never dereferenced.
    // There is no Kernel, native Task/device, CP, GPU or completion stand-in.
    thread = {reinterpret_cast<const KernelState*>(&kernel_marker),
              reinterpret_cast<const XThread*>(&thread_marker), 7, 2};
    runtime = std::make_shared<GuestRuntime>(
        GuestRuntime::Identity{0x30001000, 9}, std::make_shared<int>(1));
    const GuestRuntime::Plan init{"host source preparation",
                                  {{"startup",
                                    {{"host entry",
                                      0x81001000,
                                      {},
                                      GuestRuntime::ReturnKind::kVoid,
                                      "no PPC"}}}}};
    REQUIRE(runtime->Initialize(init, [](const auto&) {
      return GuestRuntime::ExecutionResult{true, 0, {}};
    }) == GuestRuntime::InitStatus::kReady);
    session = Session::CreateProvisional(thread, 3, runtime);
    REQUIRE(session);
    REQUIRE(session->BindGraph(thread,
                               std::make_shared<HostGraph>(session->owner())));
    record = Allocate(session->owner(), 0x4000);
    output = Allocate(session->owner(), 0x2000);
    texture = Allocate(session->owner(), 0x20000);
    std::memset(memory.TranslatePhysical(texture.address), 0xA5,
                texture.length);
  }
  ~ResourceFixture() {
    session->RetireAfterKernelStop([](const auto&) { return true; });
  }
  Range Allocate(const Session::Owner& owner, uint32_t length) {
    auto* heap = static_cast<PhysicalHeap*>(memory.LookupHeap(0xE1000000));
    uint32_t address = 0;
    uint64_t id = 0;
    REQUIRE(heap->AllocRangeOwned(
        0xE1000000, 0xE1800000, length, 0x1000,
        kMemoryAllocationReserve | kMemoryAllocationCommit,
        kMemoryProtectRead | kMemoryProtectWrite, true, owner, &address, &id));
    return {memory.GetPhysicalAddress(address), length};
  }
  void Build(const xenos::xe_gpu_texture_fetch_t& fetch,
             std::shared_ptr<const gpu::NativeSystemTitleInput> title = {}) {
    Words words;
    const auto write = [&](uint32_t index, std::span<const uint32_t> values) {
      words.push_back(uint32_t(values.size() - 1) << 16 | index);
      words.insert(words.end(), values.begin(), values.end());
    };
    const auto scalar = [&](uint32_t index, uint32_t value) {
      write(index, std::span(&value, 1));
    };
    scalar(gpu::XE_GPU_REG_RB_MODECONTROL,
           uint32_t(xenos::EdramMode::kColorDepth));
    scalar(gpu::XE_GPU_REG_PA_SC_VIZ_QUERY, 0);
    scalar(gpu::XE_GPU_REG_VGT_OUTPUT_PATH_CNTL, 0);
    scalar(gpu::XE_GPU_REG_VGT_MAX_VTX_INDX, xenos::kVertexIndexMask);
    scalar(gpu::XE_GPU_REG_VGT_MIN_VTX_INDX, 0);
    scalar(gpu::XE_GPU_REG_VGT_INDX_OFFSET, 0);
    scalar(gpu::XE_GPU_REG_SQ_PROGRAM_CNTL, 0);
    scalar(gpu::XE_GPU_REG_SQ_CONTEXT_MISC, 0);
    const std::array<uint32_t, 2> selectors{0xFF000, 0xFF100};
    write(gpu::XE_GPU_REG_SQ_VS_CONST, selectors);
    std::array<uint32_t, 6> texture_words;
    std::memcpy(texture_words.data(), &fetch, sizeof(fetch));
    write(gpu::XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0, texture_words);
    const std::array<uint32_t, 2> vertex_words{
        (record.address + 0x1000) | uint32_t(xenos::FetchConstantType::kVertex),
        uint32_t(xenos::Endian::k8in32) | 100u << 2};
    write(gpu::XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + 95 * 2, vertex_words);
    const auto shader = [&](xenos::ShaderType stage, const Words& code) {
      words.push_back(0xC0000000 | uint32_t(code.size() + 1) << 16 |
                      uint32_t(xenos::PM4_IM_LOAD_IMMEDIATE) << 8);
      words.push_back(uint32_t(stage));
      words.push_back(uint32_t(code.size()));
      words.insert(words.end(), code.begin(), code.end());
    };
    shader(xenos::ShaderType::kVertex, PublicVertexProgram());
    shader(xenos::ShaderType::kPixel, PublicPixelProgram());
    words.push_back(0xC0010000 | uint32_t(xenos::PM4_DRAW_INDX) << 8);
    words.push_back(0);
    words.push_back(25u << 16 | uint32_t(xenos::SourceSelect::kAutoIndex) << 6 |
                    uint32_t(xenos::PrimitiveType::kTriangleStrip));
    primary = Bytecode(words);
    REQUIRE(primary.size() < 0x1000);
    std::memcpy(memory.TranslatePhysical(record.address), primary.data(),
                primary.size());
    const auto result = Plan::AnalyzeWithTitleInput(primary, title);
    REQUIRE(result);
    plan = result.plan;
    for (const auto& load : plan->inventory().shaders) {
      const size_t first = size_t(load.inline_dword_offset) * 4;
      std::vector<uint8_t> code(primary.begin() + first,
                                primary.begin() + first + load.dword_count * 4);
      const auto metadata =
          gpu::NativeShaderReadAnalysis::Analyze(load.stage, code);
      REQUIRE(metadata);
      shaders.push_back({load, std::move(code), metadata});
    }
    auto update = session->BeginUpdate(thread);
    REQUIRE(update);
    source = session->Freeze(thread, update, primary, std::move(title));
    REQUIRE(source);
    const std::array<Range, 3> ranges{record, output, texture};
    const auto pin = memory.PinOwnedPhysicalRanges(session->owner(), ranges);
    REQUIRE(pin);
    REQUIRE(session->AttachPhysicalPin(thread, source, pin.pin));
    const auto reads = Inventory::TextureReads(plan->inventory().draws[0],
                                               &shaders[1].metadata);
    REQUIRE(reads.error == Inventory::Error::kNone);
    REQUIRE(reads.reads.size() == 1);
    texture_read = reads.reads[0];
  }
  std::unique_ptr<NativeFrameInputCapture> Capture() {
    return NativeFrameExecutionTestAccess::Capture(
        &memory, session, thread, source, plan,
        {record.address, uint32_t(primary.size())}, output);
  }
  std::shared_ptr<const Request> Mint(
      std::shared_ptr<const gpu::NativeFrameInputSnapshot> snapshot) {
    return NativeFrameExecutionTestAccess::Mint(
        session, thread, source, plan, output,
        {{record.address, uint32_t(primary.size())},
         {record.address + 0x1000, 400},
         {texture_read.physical_address, texture_read.byte_count}},
        shaders, {}, std::move(snapshot));
  }
  Memory memory;
  int kernel_marker = 0, thread_marker = 0;
  Session::ThreadIdentity thread;
  std::shared_ptr<GuestRuntime> runtime;
  std::shared_ptr<Session> session;
  Range record{}, output{}, texture{};
  std::vector<uint8_t> primary;
  std::shared_ptr<const Plan> plan;
  std::shared_ptr<const NativeFrameSource> source;
  std::vector<Request::Shader> shaders;
  Inventory::TextureRead texture_read{};
};

TEST_CASE(
    "Native texture Request requires rounded loader bytes in the same snapshot",
    "[kernel][native_strip_texture][native_input_snapshot]") {
  ResourceFixture f;
  f.Build(OrdinaryFetch(f.texture.address));
  REQUIRE(f.texture_read.byte_count == 304);
  auto capture = f.Capture();
  REQUIRE(capture);
  REQUIRE(capture->Capture(f.record));
  uint32_t captured = 304;
  bool complete = true;
  SECTION("complete16-byte final vector") {}
  SECTION("logical289-byte coverage is insufficient") {
    captured = 289;
    complete = false;
  }
  REQUIRE(capture->Capture({f.texture.address, captured}));
  const auto snapshot = capture->Seal();
  REQUIRE(snapshot);
  capture.reset();
  const auto request = f.Mint(snapshot);
  CHECK(bool(request) == complete);
  if (request) {
    CHECK(request->IsAdmitted());
    std::array<uint8_t, 304> bytes;
    std::memset(f.memory.TranslatePhysical(f.texture.address), 0, bytes.size());
    REQUIRE(snapshot->Read({f.texture.address, uint32_t(bytes.size())}, bytes));
    CHECK(bytes[303] == 0xA5);
    CHECK(request->IsAdmitted());
    CHECK_FALSE(f.session->GetSnapshot().submission_attempted);
  }
}

TEST_CASE("Native RGBA base payload and strip bounds use actual owned Memory",
          "[kernel][native_strip_texture][native_input_snapshot]") {
  ResourceFixture f;
  uint32_t payload = f.texture.address;
  SECTION("independent native allocation") {}
  SECTION("payload outside primary within the record parent is legitimate") {
    payload = f.record.address + 0x2000;
  }
  auto fetch = OrdinaryFetch(payload, true);
  // Keep the second form within the actual record parent rather than claiming
  // a 128KiB image there. Both exercise the same four-byte layout family.
  if (payload != f.texture.address) {
    fetch.size_2d.width = 31;
    fetch.size_2d.height = 1;
    fetch.pitch = 64 >> 5;
  }
  f.Build(fetch);
  const auto bounds = gpu::NativeVertexAccessAnalysis::Analyze(
      f.plan->inventory().draws[0], f.shaders[0].code);
  REQUIRE(bounds.vertex_input_bounds_verified);
  REQUIRE(bounds.accesses.size() == 2);
  CHECK(bounds.accesses[0].exclusive_end_word == 98);
  CHECK(bounds.accesses[1].exclusive_end_word == 100);
  CHECK_FALSE(bounds.execution_ready);
  CHECK_FALSE(bounds.ownership_verified);
  auto capture = f.Capture();
  REQUIRE(capture);
  REQUIRE(capture->Capture(f.record));
  REQUIRE(capture->Capture(
      {f.texture_read.physical_address, f.texture_read.byte_count}));
  const auto snapshot = capture->Seal();
  REQUIRE(snapshot);
  capture.reset();
  const auto request = f.Mint(snapshot);
  REQUIRE(request);
  CHECK(request->IsAdmitted());
  CHECK(f.texture_read.endian == uint32_t(xenos::Endian::k8in32));
  CHECK(f.texture_read.fetch_words[3] == fetch.dword_3);
  CHECK_FALSE(f.session->GetSnapshot().submission_attempted);
}

TEST_CASE("Native ordinary texture cannot borrow output or another owner",
          "[kernel][native_strip_texture][native_input_snapshot]") {
  ResourceFixture f;
  uint32_t address = f.output.address;
  bool foreign = false;
  SECTION("output feedback alias") {}
  SECTION("other native allocation owner") {
    foreign = true;
    auto other = f.session->owner();
    ++other.session_id;
    address = f.Allocate(other, 0x2000).address;
    const Range range{address, 304};
    CHECK_FALSE(f.memory.PinOwnedPhysicalRanges(f.session->owner(),
                                                std::span(&range, 1)));
  }
  f.Build(OrdinaryFetch(address));
  auto capture = f.Capture();
  REQUIRE(capture);
  REQUIRE(capture->Capture(f.record));
  CHECK_FALSE(capture->Capture({address, 304}));
  CHECK_FALSE(capture->Seal());
  CHECK_FALSE(f.session->GetSnapshot().submission_attempted);
  CAPTURE(foreign);
}

TEST_CASE("Only exact reserved title input may supply imported texture bytes",
          "[kernel][native_strip_texture][native_title_input]") {
  ResourceFixture f;
  using Input = gpu::NativeSystemTitleInput;
  auto fetch = OrdinaryFetch(f.texture.address, true);
  fetch.pitch = 64 >> 5;
  fetch.size_2d.width = fetch.size_2d.height = 31;
  Input::Swap swap{17, 0x3001C000, 23, {}, 32, 32};
  std::memcpy(swap.fetch.data(), &fetch, sizeof(fetch));
  const auto layout = Input::GetLayout(swap);
  REQUIRE(layout);
  const auto title = gpu::test::NativeSystemTitleInputTestAccess::Create(
      {42, 17, 0x3001C000, 23, 1}, *layout,
      std::vector<uint8_t>(layout->range.length, 0x5A));
  REQUIRE(title);
  // It is a pure CPU test container, not a claim of producer GPU completion.
  f.Build(fetch, title);
  CHECK_FALSE(f.texture_read.from_title_input);  // Used slot0 is not slot15.
  auto capture = f.Capture();
  REQUIRE(capture);
  REQUIRE(capture->Capture(f.record));
  CHECK_FALSE(capture->Capture(
      {f.texture_read.physical_address, f.texture_read.byte_count}));
  CHECK_FALSE(capture->Seal());
  CHECK_FALSE(f.session->GetSnapshot().submission_attempted);
}
TEST_CASE("Native base texture Request covers tiled storage and packed origin",
          "[kernel][native_strip_texture][native_input_snapshot]") {
  ResourceFixture f;
  bool packed = false, complete = true;
  SECTION("tiled full common footprint") {}
  SECTION("tiled logical texels do not cover the tile footprint") {
    complete = false;
  }
  SECTION("packed base includes its nonzero origin") { packed = true; }
  SECTION("packed logical texels at address zero miss the real base") {
    packed = true;
    complete = false;
  }
  auto fetch = OrdinaryFetch(f.texture.address, true);
  fetch.pitch = 64 >> 5;
  fetch.size_2d.width = fetch.size_2d.height = packed ? 1 : 32;
  fetch.tiled = !packed;
  fetch.packed_mips = packed;
  f.Build(fetch);
  const uint32_t width = fetch.size_2d.width + 1;
  const uint32_t height = fetch.size_2d.height + 1;
  const auto storage = gpu::texture_util::GetGuestTextureLayout(
      fetch.dimension, fetch.pitch, width, height, 1, fetch.tiled,
      gpu::GetBaseFormat(fetch.format), fetch.packed_mips, true, 0);
  CHECK(f.texture_read.byte_count ==
        ((storage.base.level_data_extent_bytes + 15) & ~15u));
  const uint32_t logical = width * height * 4;
  REQUIRE(f.texture_read.byte_count > logical);
  uint32_t origin_x = 0, origin_y = 0, origin_z = 0;
  if (packed) {
    REQUIRE(gpu::texture_util::GetPackedMipOffset(
        width, height, 1, gpu::GetBaseFormat(fetch.format), 0, origin_x,
        origin_y, origin_z));
    CHECK((origin_x || origin_y));
  }
  auto capture = f.Capture();
  REQUIRE(capture);
  REQUIRE(capture->Capture(f.record));
  REQUIRE(capture->Capture({f.texture_read.physical_address,
                            complete ? f.texture_read.byte_count : logical}));
  const auto snapshot = capture->Seal();
  REQUIRE(snapshot);
  capture.reset();
  const auto request = f.Mint(snapshot);
  CHECK(bool(request) == complete);
  if (request) {
    CHECK(request->IsAdmitted());
    if (packed) {
      const uint32_t origin =
          origin_y * storage.base.row_pitch_bytes + origin_x * 4;
      std::array<uint8_t, 4> pixel{};
      REQUIRE(
          snapshot->Read({f.texture_read.physical_address + origin, 4}, pixel));
      CHECK(pixel[0] == 0xA5);
    }
  }
  CHECK_FALSE(f.session->GetSnapshot().submission_attempted);
}
}  // namespace
}  // namespace xe::kernel::test

// Pure public-ISA inventory regressions; no captured programs or GPU objects.
namespace xe::gpu::test {
namespace {
using SttAnalysis = NativeShaderReadAnalysis;
using SttInventory = NativeDrawReadInventory;
using SttAccess = NativeVertexAccessAnalysis;
using SttInstruction = std::array<uint32_t, 3>;

std::vector<uint8_t> SttBytes(const std::vector<uint32_t>& words) {
  std::vector<uint8_t> bytes;
  for (uint32_t word : words) {
    bytes.push_back(uint8_t(word >> 24));
    bytes.push_back(uint8_t(word >> 16));
    bytes.push_back(uint8_t(word >> 8));
    bytes.push_back(uint8_t(word));
  }
  return bytes;
}

std::vector<uint32_t> SttShaderWords(
    std::initializer_list<SttInstruction> instructions, uint32_t sequence) {
  std::vector<uint32_t> words{
      1u | uint32_t(instructions.size()) << 12 | sequence << 16,
      uint32_t(ucode::ControlFlowOpcode::kExecEnd) << 12 |
          uint32_t(ucode::AddressingMode::kAbsolute) << 11,
      0};
  for (const auto& instruction : instructions) {
    words.insert(words.end(), instruction.begin(), instruction.end());
  }
  return words;
}

SttInstruction SttFetch(bool mini = false) {
  const uint32_t slot = mini ? 0 : 95;
  return {1u << 12 | 1u << 19 | (slot / 3) << 20 | (slot % 3) << 25,
          0x688u | uint32_t(xenos::VertexFormat::k_32_32_FLOAT) << 16 |
              uint32_t(mini) << 30,
          4u | uint32_t(mini ? 2 : 0) << 8};
}

SttInstruction SttSample(uint32_t slot) {
  return {1u | 1u << 19 | slot << 20,
          0x688u | 3u << 12 | 3u << 14 | 3u << 16 | 7u << 18 | 3u << 24 |
              3u << 26 | 1u << 28,
          uint32_t(xenos::FetchOpDimension::k2D) << 14};
}

struct SttPrimary {
  std::vector<uint32_t> words;
  void Register(uint32_t reg, std::initializer_list<uint32_t> values) {
    words.push_back(uint32_t(values.size() - 1) << 16 | reg);
    words.insert(words.end(), values);
  }
  void Packet(uint32_t opcode, const std::vector<uint32_t>& body) {
    words.push_back(0xC0000000u | uint32_t(body.size() - 1) << 16 |
                    opcode << 8);
    words.insert(words.end(), body.begin(), body.end());
  }
  void Shader(const std::vector<uint32_t>& code, xenos::ShaderType stage) {
    std::vector<uint32_t> body{uint32_t(stage), uint32_t(code.size())};
    body.insert(body.end(), code.begin(), code.end());
    Packet(xenos::PM4_IM_LOAD_IMMEDIATE, body);
  }
  void Draw(xenos::PrimitiveType primitive, uint32_t count,
            bool with_token = true) {
    for (uint32_t i = 0; i < SttInventory::kStateRegisters.size(); ++i) {
      Register(SttInventory::kStateRegisters[i],
               {i == 0   ? uint32_t(xenos::EdramMode::kColorDepth)
                : i == 3 ? xenos::kVertexIndexMask
                         : 0});
    }
    const uint32_t initiator = count << 16 |
                               uint32_t(xenos::SourceSelect::kAutoIndex) << 6 |
                               uint32_t(primitive);
    Packet(with_token ? xenos::PM4_DRAW_INDX : xenos::PM4_DRAW_INDX_2,
           with_token ? std::vector<uint32_t>{0, initiator}
                      : std::vector<uint32_t>{initiator});
  }
  SttInventory::Draw OnlyDraw() const {
    const auto scan = SttInventory::Scan(SttBytes(words));
    REQUIRE(scan.complete_primary_scan);
    REQUIRE(scan.draws.size() == 1);
    return scan.draws[0];
  }
};

SttInventory::Draw SttStripDraw(const std::vector<uint32_t>& code,
                                uint32_t count, uint32_t declared_words,
                                bool with_token = true) {
  SttPrimary primary;
  primary.Shader(code, xenos::ShaderType::kVertex);
  primary.Register(XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + 190,
                   {0x00100003, declared_words << 2 | 2});
  primary.Draw(xenos::PrimitiveType::kTriangleStrip, count, with_token);
  return primary.OnlyDraw();
}

xenos::xe_gpu_texture_fetch_t SttLinearTexture(xenos::TextureFormat format,
                                               uint32_t width, uint32_t height,
                                               uint32_t pitch) {
  xenos::xe_gpu_texture_fetch_t fetch{};
  fetch.type = xenos::FetchConstantType::kTexture;
  fetch.dimension = xenos::DataDimension::k2DOrStacked;
  fetch.format = format;
  fetch.base_address = 0x00100000 >> 12;
  fetch.size_2d.width = width - 1;
  fetch.size_2d.height = height - 1;
  fetch.pitch = pitch >> 5;
  fetch.clamp_x = fetch.clamp_y = xenos::ClampMode::kClampToEdge;
  fetch.mag_filter = fetch.min_filter = fetch.mip_filter =
      xenos::TextureFilter::kLinear;
  fetch.swizzle = 0x60A;
  return fetch;
}

void SttSetTexture(SttInventory::Draw& draw, uint32_t slot,
                   const xenos::xe_gpu_texture_fetch_t& fetch) {
  std::memcpy(draw.fetch_words.data() + slot * 6, &fetch, sizeof(fetch));
  for (uint32_t i = 0; i < 6; ++i) {
    draw.fetch_known.set(slot * 6 + i);
  }
}

SttInventory::Draw SttTextureDraw(const std::vector<uint32_t>& shader) {
  SttPrimary primary;
  primary.Shader(shader, xenos::ShaderType::kPixel);
  primary.Draw(xenos::PrimitiveType::kQuadList, 4);
  return primary.OnlyDraw();
}

std::array<uint32_t, 6> SttTextureWords(
    const xenos::xe_gpu_texture_fetch_t& fetch) {
  std::array<uint32_t, 6> words;
  std::memcpy(words.data(), &fetch, sizeof(fetch));
  return words;
}

TEST_CASE("Auto strip metadata and bounds retain all25 original input IDs",
          "[graphics][native_strip_texture]") {
  const auto words = SttShaderWords({SttFetch(), SttFetch(true)}, 5);
  const auto code = SttBytes(words);
  const auto metadata = SttAnalysis::Analyze(xenos::ShaderType::kVertex, code);
  REQUIRE(metadata);
  REQUIRE(metadata.vertex_fetches.size() == 2);
  CHECK(metadata.vertex_fetches[0].slot == 95);
  CHECK(metadata.vertex_fetches[1].slot == 95);
  CHECK(metadata.vertex_fetches[1].mini);
  CHECK(metadata.vertex_fetches[1].offset_words == 2);
  bool with_token = true;
  SECTION("DRAW_INDX token form") {}
  SECTION("DRAW_INDX_2 without token") { with_token = false; }
  const auto draw = SttStripDraw(words, 25, 100, with_token);
  const auto reads = SttInventory::VertexReads(draw, &metadata);
  REQUIRE(reads.error == SttInventory::Error::kNone);
  REQUIRE(reads.reads.size() == 1);
  CHECK(reads.reads[0].byte_count == 400);
  const auto bounds = SttAccess::Analyze(draw, code);
  REQUIRE(bounds.vertex_input_bounds_verified);
  CHECK(bounds.error == SttAccess::Error::kNone);
  CHECK(bounds.vertex_count == 25);
  CHECK(bounds.uses_auto_index_backend_contract);
  REQUIRE(bounds.accesses.size() == 2);
  CHECK(bounds.accesses[0].exclusive_end_word == 98);
  CHECK(bounds.accesses[1].first_word == 2);
  CHECK(bounds.accesses[1].exclusive_end_word == 100);
  CHECK(bounds.accesses[1].declared_words == 100);
  CHECK_FALSE(bounds.execution_ready);
  CHECK_FALSE(bounds.ownership_verified);
}

TEST_CASE("Auto strip geometry keeps short and nonauto modes closed",
          "[graphics][native_strip_texture]") {
  const auto words = SttShaderWords({SttFetch(), SttFetch(true)}, 5);
  const auto metadata =
      SttAnalysis::Analyze(xenos::ShaderType::kVertex, SttBytes(words));
  REQUIRE(metadata);
  for (uint32_t count : {3u, 4u, 25u}) {
    const auto draw = SttStripDraw(words, count, count * 4);
    REQUIRE(SttInventory::VertexReads(draw, &metadata).error ==
            SttInventory::Error::kNone);
    CHECK(
        SttAccess::Analyze(draw, SttBytes(words)).vertex_input_bounds_verified);
    auto fan = draw;
    fan.initiator =
        (fan.initiator & ~63u) | uint32_t(xenos::PrimitiveType::kTriangleFan);
    CHECK(SttInventory::VertexReads(fan, &metadata).error ==
          SttInventory::Error::kNone);
  }
  auto bad = SttStripDraw(words, 25, 100);
  SECTION("fewer than3 vertices") {
    for (uint32_t count : {0u, 1u, 2u}) {
      bad.initiator = (bad.initiator & 0xFFFFu) | count << 16;
      CHECK(SttInventory::VertexReads(bad, &metadata).error ==
            SttInventory::Error::kUnsupportedGeometry);
    }
  }
  SECTION("DMA selector is a separate contract") {
    bad.initiator &= ~(3u << 6);
  }
  SECTION("immediate selector is a separate contract") {
    bad.initiator = (bad.initiator & ~(3u << 6)) | 1u << 6;
  }
  SECTION("explicit major mode is not inferred") { bad.initiator |= 1u << 8; }
  SECTION("missing local VGT state") { bad.state_known.reset(2); }
  CHECK(SttInventory::VertexReads(bad, &metadata).error !=
        SttInventory::Error::kNone);
}

TEST_CASE("Auto strip full and mini fetch reject a one-word short binding",
          "[graphics][native_strip_texture]") {
  const auto words = SttShaderWords({SttFetch(), SttFetch(true)}, 5);
  const auto result =
      SttAccess::Analyze(SttStripDraw(words, 25, 99), SttBytes(words));
  CHECK(result.error == SttAccess::Error::kOutOfBounds);
  CHECK(result.error_slot == 95);
  CHECK(result.accesses.empty());
  CHECK_FALSE(result.vertex_input_bounds_verified);
}

TEST_CASE("Native linear R8 snapshots cover the last loader vector",
          "[graphics][native_strip_texture]") {
  const auto words = SttShaderWords({SttSample(7)}, 1);
  const auto metadata =
      SttAnalysis::Analyze(xenos::ShaderType::kPixel, SttBytes(words));
  REQUIRE(metadata);
  auto draw = SttTextureDraw(words);
  const auto fetch = SttLinearTexture(xenos::TextureFormat::k_8, 33, 2, 256);
  SttSetTexture(draw, 7, fetch);
  const auto result = SttInventory::TextureReads(draw, &metadata);
  REQUIRE(result.error == SttInventory::Error::kNone);
  REQUIRE(result.reads.size() == 1);
  const auto& read = result.reads[0];
  CHECK(read.logical_texel_bytes == 66);
  CHECK(read.row_pitch_bytes == 256);
  CHECK(read.padded_span_bytes == 512);
  // The actual row/layout extent is289B; the loader reads304B, not only289B
  // and not the512B row allocation or the full storage stride.
  CHECK(read.byte_count == 304);
  CHECK(read.byte_count < read.padded_span_bytes);
  CHECK(read.fetch_words == SttTextureWords(fetch));
  CHECK_FALSE(result.execution_ready);
  CHECK_FALSE(result.access_bounds_verified);
  CHECK_FALSE(result.ownership_verified);
}

TEST_CASE("Native linear RGBA preserves every admitted alias and endian",
          "[graphics][native_strip_texture]") {
  const auto words = SttShaderWords({SttSample(7)}, 1);
  const auto metadata =
      SttAnalysis::Analyze(xenos::ShaderType::kPixel, SttBytes(words));
  REQUIRE(metadata);
  for (const auto format :
       {xenos::TextureFormat::k_8_8_8_8, xenos::TextureFormat::k_8_8_8_8_A,
        xenos::TextureFormat::k_8_8_8_8_AS_16_16_16_16,
        xenos::TextureFormat::k_8_8_8_8_GAMMA_EDRAM}) {
    for (uint32_t endian = 0; endian < 4; ++endian) {
      auto draw = SttTextureDraw(words);
      auto fetch = SttLinearTexture(format, 100, 250, 128);
      fetch.endianness = xenos::Endian(endian);
      SttSetTexture(draw, 7, fetch);
      const auto result = SttInventory::TextureReads(draw, &metadata);
      REQUIRE(result.error == SttInventory::Error::kNone);
      REQUIRE(result.reads.size() == 1);
      const auto& read = result.reads[0];
      CHECK(read.width == 100);
      CHECK(read.height == 250);
      CHECK(read.byte_count == 127888);
      CHECK(read.logical_texel_bytes == 100000);
      CHECK(read.row_pitch_bytes == 512);
      CHECK(read.padded_span_bytes == 128000);
      CHECK(read.format == uint32_t(format));
      CHECK(read.endian == endian);
      CHECK(read.fetch_words == SttTextureWords(fetch));
      CHECK_FALSE(read.from_title_input);
    }
  }
  for (const auto format :
       {xenos::TextureFormat::k_8_A, xenos::TextureFormat::k_8_B}) {
    auto draw = SttTextureDraw(words);
    auto fetch = SttLinearTexture(format, 33, 2, 256);
    fetch.endianness = xenos::Endian::k8in32;
    SttSetTexture(draw, 7, fetch);
    const auto result = SttInventory::TextureReads(draw, &metadata);
    REQUIRE(result.error == SttInventory::Error::kNone);
    REQUIRE(result.reads.size() == 1);
    CHECK(result.reads[0].byte_count == 304);
    CHECK(result.reads[0].fetch_words == SttTextureWords(fetch));
  }
}

TEST_CASE("Native base-only textures retain raw ignored LOD and sampler bits",
          "[graphics][native_strip_texture]") {
  auto sample = SttSample(7);
  // Explicit linear overrides a real point descriptor. Preserve both sources.
  sample[1] = (sample[1] & ~((3u << 12) | (3u << 14))) | 1u << 12 | 1u << 14;
  const auto words = SttShaderWords({sample}, 1);
  const auto metadata =
      SttAnalysis::Analyze(xenos::ShaderType::kPixel, SttBytes(words));
  REQUIRE(metadata);
  auto draw = SttTextureDraw(words);
  auto fetch = SttLinearTexture(xenos::TextureFormat::k_8_8_8_8, 100, 250, 128);
  fetch.mag_filter = fetch.min_filter = xenos::TextureFilter::kPoint;
  fetch.clamp_z = xenos::ClampMode::kRepeat;
  fetch.size_2d.stack_depth = 7;  // Actual non-stacked resource ignores this.
  fetch.border_color = xenos::BorderColor(2);
  fetch.mag_aniso_walk = fetch.min_aniso_walk = 1;
  fetch.aniso_bias = -2;
  fetch.lod_bias = -17;
  fetch.grad_exp_adjust_h = 3;
  fetch.grad_exp_adjust_v = -4;
  SECTION("absent mip resource ignores nonzero raw clamps") {
    fetch.mip_min_level = 3;
    fetch.mip_max_level = 9;
  }
  SECTION("zero effective levels ignore a raw mip page") {
    fetch.mip_address = 0x200;
    fetch.mip_min_level = fetch.mip_max_level = 0;
  }
  SttSetTexture(draw, 7, fetch);
  const auto result = SttInventory::TextureReads(draw, &metadata);
  REQUIRE(result.error == SttInventory::Error::kNone);
  REQUIRE(result.reads.size() == 1);
  CHECK(result.reads[0].fetch_words == SttTextureWords(fetch));
  REQUIRE(result.reads[0].instructions.size() == 1);
  CHECK(result.reads[0].instructions[0].instruction_words == sample);
  CHECK(result.reads[0].byte_count == 127888);
}

TEST_CASE("Native ordinary textures retain tiled and packed base extents",
          "[graphics][native_strip_texture]") {
  const auto words = SttShaderWords({SttSample(7)}, 1);
  const auto metadata =
      SttAnalysis::Analyze(xenos::ShaderType::kPixel, SttBytes(words));
  REQUIRE(metadata);
  auto fetch = SttLinearTexture(xenos::TextureFormat::k_8_8_8_8, 64, 33, 64);
  fetch.tiled = 1;
  uint32_t width = 64, height = 33, expected_bytes = 16384;
  SECTION("RGBA tiled base") {}
  SECTION("RGBA packed linear base at16,0") {
    width = height = 8;
    fetch = SttLinearTexture(xenos::TextureFormat::k_8_8_8_8, 8, 8, 64);
    fetch.packed_mips = 1;
    expected_bytes = 1888;
  }
  SECTION("RGBA packed tiled base") {
    width = height = 8;
    fetch = SttLinearTexture(xenos::TextureFormat::k_8_8_8_8, 8, 8, 64);
    fetch.packed_mips = fetch.tiled = 1;
    expected_bytes = 4096;
  }
  SECTION("R8 tiled base consumes the common storage extent") {
    fetch = SttLinearTexture(xenos::TextureFormat::k_8, 64, 33, 64);
    fetch.tiled = 1;
    // This case checks compatibility with the actual common loader footprint,
    // without imposing guessed byte-per-pixel tile address math.
    const auto layout = texture_util::GetGuestTextureLayout(
        fetch.dimension, fetch.pitch, width, height, 1, fetch.tiled,
        xenos::TextureFormat::k_8, fetch.packed_mips, true, 0);
    expected_bytes =
        uint32_t((layout.base.level_data_extent_bytes + 15) & ~15u);
  }
  auto draw = SttTextureDraw(words);
  SttSetTexture(draw, 7, fetch);
  const auto result = SttInventory::TextureReads(draw, &metadata);
  REQUIRE(result.error == SttInventory::Error::kNone);
  REQUIRE(result.reads.size() == 1);
  CHECK(result.reads[0].byte_count == expected_bytes);
  CHECK(result.reads[0].byte_count >= result.reads[0].logical_texel_bytes);
  CHECK(result.reads[0].width == width);
  CHECK(result.reads[0].height == height);
  CHECK(result.reads[0].fetch_words == SttTextureWords(fetch));
  CHECK_FALSE(result.execution_ready);
  CHECK_FALSE(result.access_bounds_verified);
  CHECK_FALSE(result.ownership_verified);
}

TEST_CASE(
    "Native ordinary profile rejects unclosed resources and numeric modes",
    "[graphics][native_strip_texture]") {
  const auto words = SttShaderWords({SttSample(7)}, 1);
  const auto metadata =
      SttAnalysis::Analyze(xenos::ShaderType::kPixel, SttBytes(words));
  REQUIRE(metadata);
  auto draw = SttTextureDraw(words);
  auto fetch = SttLinearTexture(xenos::TextureFormat::k_8_8_8_8, 100, 250, 128);
  bool missing_word = false;
  auto expected = SttInventory::Error::kUnsupportedTexture;
  SECTION("unhandled format") { fetch.format = xenos::TextureFormat::k_8_8; }
  SECTION("effective extra mip") {
    fetch.mip_address = 0x200;
    fetch.mip_max_level = 1;
  }
  SECTION("stacked array") { fetch.stacked = 1; }
  SECTION("3D") { fetch.dimension = xenos::DataDimension::k3D; }
  SECTION("integer numeric mode") { fetch.num_format = 1; }
  SECTION("signed component") { fetch.sign_x = xenos::TextureSign::kSigned; }
  SECTION("exponent adjustment") { fetch.exp_adjust = 1; }
  SECTION("anisotropy") { fetch.aniso_filter = xenos::AnisoFilter::kMax_2_1; }
  SECTION("effective point sampling") {
    fetch.min_filter = xenos::TextureFilter::kPoint;
  }
  SECTION("pitch below width") {
    fetch.size_2d.width = 128;
    expected = SttInventory::Error::kInvalidTextureLayout;
  }
  SECTION("pitch not a full linear row") {
    fetch.pitch = 3;
    fetch.size_2d.width = 63;
    expected = SttInventory::Error::kInvalidTextureLayout;
  }
  SECTION("missing used descriptor word") {
    missing_word = true;
    expected = SttInventory::Error::kMissingState;
  }
  SECTION("physical range end") {
    fetch.base_address = 0x1FFFF;
    expected = SttInventory::Error::kInvalidMemoryRange;
  }
  SttSetTexture(draw, 7, fetch);
  if (missing_word) {
    draw.fetch_known.reset(7 * 6 + 4);
  }
  const auto result = SttInventory::TextureReads(draw, &metadata);
  CHECK(result.error == expected);
  CHECK(result.error_slot == 7);
  CHECK(result.reads.empty());
}

TEST_CASE("Native texture capture keeps its exact per-read budget boundary",
          "[graphics][native_strip_texture]") {
  const auto words = SttShaderWords({SttSample(7)}, 1);
  const auto metadata =
      SttAnalysis::Analyze(xenos::ShaderType::kPixel, SttBytes(words));
  REQUIRE(metadata);
  auto draw = SttTextureDraw(words);
  auto fetch = SttLinearTexture(xenos::TextureFormat::k_8, 2048, 2048, 2048);
  bool over = false;
  SECTION("exact4MiB") {}
  SECTION("one additional row") {
    fetch.size_2d.height = 2048;
    over = true;
  }
  SttSetTexture(draw, 7, fetch);
  const auto result = SttInventory::TextureReads(draw, &metadata);
  if (over) {
    CHECK(result.error == SttInventory::Error::kLimitExceeded);
    CHECK(result.reads.empty());
  } else {
    REQUIRE(result.error == SttInventory::Error::kNone);
    REQUIRE(result.reads.size() == 1);
    CHECK(result.reads[0].byte_count == 4u * 1024 * 1024);
  }
}
}  // namespace
}  // namespace xe::gpu::test
