/** Xenia : Xbox 360 Emulator Research Project; BSD license, see LICENSE. */
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "third_party/catch/include/catch.hpp"
#include "xenia/apu/audio_ducker_policy.h"
#include "xenia/apu/audio_system.h"
#include "xenia/apu/nop/nop_audio_system.h"
#include "xenia/apu/xma_decoder.h"
#include "xenia/base/byte_order.h"
#include "xenia/base/byte_stream.h"
#include "xenia/base/cvar.h"
#include "xenia/base/platform.h"
#include "xenia/cpu/export_resolver.h"
#include "xenia/cpu/processor.h"
#include "xenia/emulator.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/util/shim_utils.h"

#if XE_PLATFORM_LINUX
DECLARE_string(cpu);
DECLARE_bool(debug);
DECLARE_bool(use_dedicated_xma_thread);
DECLARE_string(system_readonly_source_root);
DECLARE_string(logged_profile_slot_0_xuid);
DECLARE_string(logged_profile_slot_1_xuid);
DECLARE_string(logged_profile_slot_2_xuid);
DECLARE_string(logged_profile_slot_3_xuid);

namespace xe::kernel::test {
namespace {

struct AudioTestSettings {
  std::string cpu = cvars::cpu;
  bool debug = cvars::debug;
  bool dedicated = cvars::use_dedicated_xma_thread;
  std::array<std::string *, 5> fields{
      &cvars::system_readonly_source_root, &cvars::logged_profile_slot_0_xuid,
      &cvars::logged_profile_slot_1_xuid, &cvars::logged_profile_slot_2_xuid,
      &cvars::logged_profile_slot_3_xuid};
  std::array<std::string, 5> saved;
  explicit AudioTestSettings(bool use_dedicated) {
    cvars::use_dedicated_xma_thread = use_dedicated;
    cvars::cpu = "null";
    cvars::debug = false;
    for (size_t i = 0; i != fields.size(); ++i) {
      saved[i] = *fields[i];
      fields[i]->clear();
    }
  }
  ~AudioTestSettings() {
    cvars::cpu.swap(cpu);
    cvars::debug = debug;
    cvars::use_dedicated_xma_thread = dedicated;
    for (size_t i = 0; i != fields.size(); ++i) {
      fields[i]->swap(saved[i]);
    }
  }
};

struct AudioTestRoot {
  std::filesystem::path path;
  AudioTestRoot() {
    static std::atomic<uint64_t> sequence{0};
    path = std::filesystem::temp_directory_path() /
           ("xenia-audio-policy-" +
            std::to_string(
                std::chrono::steady_clock::now().time_since_epoch().count()) +
            "-" + std::to_string(sequence.fetch_add(1)));
    REQUIRE(std::filesystem::create_directory(path));
  }
  ~AudioTestRoot() {
    std::error_code error;
    std::filesystem::remove_all(path, error);
  }
};

// Real Kernel, Memory, Null CPU and ordinary NOP audio worker/decoder. No guest
// PPC is executed, and NOP audio is not evidence of PCM output or audibility.
// Pause invokes the real APU worker fence, never a test-written paused field.
class AudioRuntime {
public:
  explicit AudioRuntime(bool dedicated) : settings_(dedicated) {
    REQUIRE(KernelState::shared() == nullptr);
    REQUIRE(std::filesystem::create_directory(root_.path / "content"));
    REQUIRE(std::filesystem::create_directory(root_.path / "cache"));
    emulator = std::make_unique<Emulator>(
        "", root_.path, root_.path / "content", root_.path / "cache");
    REQUIRE(emulator->Setup(nullptr, nullptr, false,
                            apu::nop::NopAudioSystem::Create, {},
                            {}) == X_STATUS_SUCCESS);
    REQUIRE(emulator->SetupSubsystems() == X_STATUS_SUCCESS);
    REQUIRE(emulator->audio_system());
    REQUIRE(emulator->audio_system()->name() == "NOP");
  }
  ~AudioRuntime() {
    // Shutdown stops and wakes the paused worker without activating callbacks,
    // including assertion-unwind paths with a still-registered test callback.
    emulator.reset();
  }
  apu::AudioSystem &audio() { return *emulator->audio_system(); }
  cpu::Export *Export(uint16_t ordinal) {
    auto *entry = emulator->export_resolver()->GetExportByOrdinal(
        "xboxkrnl.exe", ordinal);
    REQUIRE(entry);
    REQUIRE(entry->is_implemented());
    REQUIRE(entry->function_data.trampoline);
    return entry;
  }
  std::vector<uint8_t> Save() {
    std::array<uint8_t, apu::AudioSystem::kMaxSaveBytes> storage;
    storage.fill(0xA5);
    ByteStream stream(storage.data(), storage.size());
    REQUIRE(audio().Save(&stream));
    return {storage.begin(), storage.begin() + stream.offset()};
  }

private:
  AudioTestSettings settings_;
  AudioTestRoot root_;

public:
  std::unique_ptr<Emulator> emulator;
};

TEST_CASE("Real audio owner serves registered FPR exports coherently",
          "[kernel][audio][abi][runtime_owner]") {
  const bool dedicated = GENERATE(false, true);
  CAPTURE(dedicated);
  AudioRuntime runtime(dedicated);
  PPCContext context{};
  context.virtual_membase = runtime.emulator->memory()->virtual_membase();
  context.f[1] = 0.25;
  context.f[2] = 0.75;
  runtime.Export(0x34E)->function_data.trampoline(&context);
  REQUIRE(uint32_t(context.r[3]) == X_STATUS_SUCCESS);
  REQUIRE(runtime.audio().ducker_policy()->Read()->level == 0.25f);
  for (const auto [ordinal, expected] :
       std::array<std::pair<uint16_t, float>, 5>{{{0x350, 0.25f},
                                                  {0x351, 1.0f},
                                                  {0x353, 0.010f},
                                                  {0x355, 0.250f},
                                                  {0x357, 0.050f}}}) {
    CAPTURE(ordinal);
    context.r[3] = 0x12345678ABCDEF00;
    context.f[1] = -17;
    context.f[2] = 91;
    runtime.Export(ordinal)->function_data.trampoline(&context);
    CHECK(context.f[1] == double(expected));
    CHECK(context.f[2] == 91);
    CHECK(context.r[3] == 0x12345678ABCDEF00);
  }
}

TEST_CASE("Real paused AudioSystem writes and restores bounded XAU2 chunks",
          "[kernel][audio][save][runtime_owner]") {
  const bool dedicated = GENERATE(false, true);
  CAPTURE(dedicated);
  AudioRuntime runtime(dedicated);
  auto policy = runtime.audio().ducker_policy();
  REQUIRE(policy->SetScalar(apu::AudioDuckerPolicy::Scalar::kLevel, 0.25f) ==
          apu::AudioDuckerPolicy::Status::kSuccess);
  REQUIRE(policy->SetCategory(3, 0.5f) ==
          apu::AudioDuckerPolicy::Status::kSuccess);
  // A live decoder keeps its Setup mode even if the setting later changes.
  cvars::use_dedicated_xma_thread = !dedicated;
  runtime.audio().Pause();
  REQUIRE(runtime.audio().xma_decoder()->is_paused());
  REQUIRE(policy->Read()->paused);
  runtime.audio().Pause();  // Repeated control is idempotent.
  for (unsigned cycle = 0; cycle != 3; ++cycle) {
    runtime.audio().Resume();
    REQUIRE_FALSE(runtime.audio().xma_decoder()->is_paused());
    runtime.audio().Pause();  // No sleep between the real resume and pause.
    REQUIRE(runtime.audio().xma_decoder()->is_paused());
  }
  auto bytes = runtime.Save();
  REQUIRE(bytes.size() == 12 + apu::AudioDuckerPolicy::kSavedBytes);
  ByteStream header(bytes.data(), bytes.size());
  CHECK(header.Read<uint32_t>() == make_fourcc("XAU2"));
  CHECK(header.Read<uint32_t>() == 0);
  CHECK(header.Read<uint32_t>() == apu::AudioDuckerPolicy::kSavedBytes);

  std::array<uint8_t, 16> too_small;
  too_small.fill(0xA5);
  const auto before = too_small;
  ByteStream short_output(too_small.data(), too_small.size());
  CHECK_FALSE(runtime.audio().Save(&short_output));
  CHECK(short_output.offset() == 0);
  CHECK(too_small == before);

  runtime.audio().Resume();
  REQUIRE_FALSE(runtime.audio().xma_decoder()->is_paused());
  runtime.audio().Resume();
  REQUIRE(policy->SetScalar(apu::AudioDuckerPolicy::Scalar::kLevel, 0.75f) ==
          apu::AudioDuckerPolicy::Status::kSuccess);
  runtime.audio().Pause();
  REQUIRE(runtime.audio().xma_decoder()->is_paused());
  ByteStream input(bytes.data(), bytes.size());
  REQUIRE(runtime.audio().Restore(&input));
  CHECK(input.offset() == bytes.size());
  CHECK(policy->Read()->level == 0.25f);
  CHECK(policy->Read()->categories[3] == 0.5f);
  CHECK(policy->Read()->paused);
  // Shutdown joins a dedicated worker parked at Resume, or completes inline
  // teardown without ever waiting on a nonexistent worker.
}

TEST_CASE("Legacy actual AudioSystem chunk resets policy with exact cursor",
          "[kernel][audio][save][legacy][runtime_owner]") {
  const bool dedicated = GENERATE(false, true);
  CAPTURE(dedicated);
  AudioRuntime runtime(dedicated);
  auto policy = runtime.audio().ducker_policy();
  REQUIRE(policy->SetScalar(apu::AudioDuckerPolicy::Scalar::kLevel, 0.25f) ==
          apu::AudioDuckerPolicy::Status::kSuccess);
  const auto epoch = policy->Read()->owner_epoch;
  runtime.audio().Pause();
  std::array<uint8_t, 12> bytes;
  ByteStream output(bytes.data(), bytes.size());
  output.Write(apu::kAudioSaveSignature);
  output.Write(uint32_t(0));
  output.Write(uint32_t(0x11223344));
  ByteStream input(bytes.data(), bytes.size());
  REQUIRE(runtime.audio().Restore(&input));
  CHECK(input.offset() == 8);
  CHECK(input.Read<uint32_t>() == 0x11223344);
  const auto reset = policy->Read();
  CHECK(reset->owner_epoch == epoch);
  CHECK(reset->level == 1.0f);
  CHECK_FALSE(reset->enabled);
  CHECK(reset->paused);
}

TEST_CASE("AudioSystem rejects future category acknowledgements before retire",
          "[kernel][audio][save][malformed][runtime_owner]") {
  const bool dedicated = GENERATE(false, true);
  CAPTURE(dedicated);
  AudioRuntime runtime(dedicated);
  runtime.audio().Pause();
  auto *memory = runtime.emulator->memory();
  const uint32_t code = memory->SystemHeapAlloc(0x1000);
  REQUIRE(code);
  // Backed, dynamic registration, never executed. Worker stays at its actual
  // pause fence until the registration has been explicitly unregistered.
  xe::store_and_swap<uint32_t>(memory->TranslateVirtual(code), 0x4E800020);
  size_t index;
  REQUIRE(runtime.audio().RegisterClient(code, code + 16, &index) ==
          X_STATUS_SUCCESS);
  auto policy = runtime.audio().ducker_policy();
  const auto original = policy->Read();
  auto bytes = runtime.Save();
  REQUIRE(bytes.size() == 12 + apu::AudioDuckerPolicy::kSavedBytes + 272);
  auto truncated_bytes = bytes;
  truncated_bytes.pop_back();
  const auto seen_offset = 12 + apu::AudioDuckerPolicy::kSavedBytes + 16;
  const uint64_t future = original->category_versions[0] + 1;
  std::memcpy(bytes.data() + seen_offset, &future, sizeof(future));
  ByteStream input(bytes.data(), bytes.size());
  CHECK_FALSE(runtime.audio().Restore(&input));
  CHECK(policy->Read() == original);
  apu::AudioSystem::ClientPerformance performance{};
  CHECK(runtime.audio().GetClientPerformance(index, &performance));

  ByteStream truncated(truncated_bytes.data(), truncated_bytes.size());
  CHECK_FALSE(runtime.audio().Restore(&truncated));
  CHECK(policy->Read() == original);
  CHECK(runtime.audio().GetClientPerformance(index, &performance));
  REQUIRE(runtime.audio().UnregisterClient(index) == X_STATUS_SUCCESS);
  memory->SystemHeapFree(code);
  // Real cancellation/teardown releases a paused dedicated worker. Later
  // controls cannot reopen it; Emulator's final Shutdown remains idempotent.
  runtime.audio().xma_decoder()->Shutdown();
  runtime.audio().xma_decoder()->Pause();
  REQUIRE_FALSE(runtime.audio().xma_decoder()->is_paused());
  runtime.audio().xma_decoder()->Resume();
  REQUIRE_FALSE(runtime.audio().xma_decoder()->is_paused());
}

} // namespace
} // namespace xe::kernel::test
#endif
