/** Xenia : Xbox 360 Emulator Research Project; BSD license, see LICENSE. */
#include "xenia/apu/audio_memory_decoder.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <span>
#include "third_party/catch/include/catch.hpp"
#include "third_party/mbedtls/include/mbedtls/sha256.h"
#include "xenia/apu/media_memory_io.h"
#include "xenia/apu/xma_context_new.h"
#include "xenia/memory.h"
#include "xenia/base/xui_resource.h"
#include "xenia/vfs/xex_metadata.h"
namespace xe::apu::test {
namespace {
std::vector<uint8_t> SilentMp3() {
  // Four synthetically valid 128kbps/44.1kHz MPEG1 layer3 silent frames;
  // zero side-info/main-data. No copyrighted clip or cue duplicate.
  std::vector<uint8_t> bytes(417 * 4);
  for (size_t i = 0; i < 4; ++i) {
    bytes[417 * i] = 0xFF;
    bytes[417 * i + 1] = 0xFB;
    bytes[417 * i + 2] = 0x90;
    bytes[417 * i + 3] = 0;
  }
  return bytes;
}
}  // namespace
TEST_CASE("Owned media AVIO bounds seeks, EOF and caller data lifetime",
          "[audio][avio]") {
  auto io = MediaMemoryIo::Create({1, 2, 3}, 3);
  REQUIRE(io);
  std::array<uint8_t, 4> bytes{9, 9, 9, 9};
  REQUIRE(io->Read(bytes.data(), 2) == 2);
  CHECK(bytes[0] == 1);
  CHECK(bytes[1] == 2);
  CHECK(io->Seek(INT64_MIN, SEEK_CUR) < 0);
  CHECK(io->Seek(INT64_MAX, SEEK_END) < 0);
  REQUIRE(io->Read(bytes.data(), 4) == 1);
  CHECK(bytes[0] == 3);
  CHECK(bytes[1] == 2);
  CHECK(io->Read(bytes.data(), 4) < 0);
  CHECK(io->Seek(0, 0x10000) == 3);
  REQUIRE(io->Seek(-2, SEEK_END) == 1);
  REQUIRE(io->Read(bytes.data(), 1) == 1);
  CHECK(bytes[0] == 2);
  CHECK_FALSE(MediaMemoryIo::Create({1, 2, 3}, 2));
}
TEST_CASE(
    "Shared audio decode drains existing MP3 codec through owned custom IO",
    "[audio][decode]") {
  auto result = DecodeAudioMemory(SilentMp3());
  REQUIRE(result.status == AudioDecodeStatus::kSuccess);
  CHECK(result.pcm.frequency == 44100);
  CHECK(result.pcm.channels == 2);
  CHECK(result.pcm.layout == PcmChannelLayout::kStereo);
  REQUIRE_FALSE(result.pcm.samples.empty());
  CHECK(result.pcm.samples.size() % 2 == 0);
  CHECK(result.pcm.samples.size() <= 4 * 1152 * 2);
}
TEST_CASE(
    "Shared audio decode rejects limits and cancellation without partial "
    "output",
    "[audio][decode]") {
  AudioDecodeLimits limits;
  limits.maximum_input_bytes = 2;
  auto result = DecodeAudioMemory(SilentMp3(), limits);
  CHECK(result.status == AudioDecodeStatus::kLimitExceeded);
  CHECK(result.pcm.samples.empty());
  limits = {};
  limits.maximum_sample_values = 1;
  result = DecodeAudioMemory(SilentMp3(), limits);
  CHECK(result.status != AudioDecodeStatus::kSuccess);
  CHECK(result.pcm.samples.empty());
  std::atomic<bool> cancelled = true;
  result = DecodeAudioMemory(SilentMp3(), {}, &cancelled);
  CHECK(result.status == AudioDecodeStatus::kCancelled);
  CHECK(result.pcm.samples.empty());
  result = DecodeAudioMemory({1, 2, 3, 4});
  CHECK(result.status != AudioDecodeStatus::kSuccess);
  CHECK(result.pcm.samples.empty());
}
TEST_CASE(
    "Decoded formats accept only named sink layouts and retain native identity",
    "[audio][decode][layout]") {
  struct Allowed {
    AudioFrameFormat format;
    PcmChannelLayout expected;
  };
  const Allowed allowed[] = {
      {{44100, 1, AudioChannelOrder::kNative, 0x4}, PcmChannelLayout::kMono},
      {{44100, 2, AudioChannelOrder::kNative, 0x3}, PcmChannelLayout::kStereo},
      {{48000, 6, AudioChannelOrder::kNative, 0x3F},
       PcmChannelLayout::kSurround51Back},
      {{48000, 6, AudioChannelOrder::kNative, 0x60F},
       PcmChannelLayout::kSurround51Side},
      {{44100, 1, AudioChannelOrder::kUnspecified, 0}, PcmChannelLayout::kMono},
      {{44100, 2, AudioChannelOrder::kUnspecified, 0},
       PcmChannelLayout::kStereo}};
  for (const auto& entry : allowed) {
    PcmFormatTracker formats;
    PcmAudio pcm;
    REQUIRE(formats.Accept(entry.format, pcm));
    CHECK(pcm.layout == entry.expected);
    CHECK(PcmLayoutMatchesChannels(pcm.layout, entry.format.channels));
    CHECK(formats.Accept(entry.format, pcm));
  }
  const AudioFrameFormat rejected[] = {
      {48000, 6, AudioChannelOrder::kNative, 0x500F},  // top-front pair
      {48000, 6, AudioChannelOrder::kNative,
       0xCF},  // wide/front-of-center pair
      {48000, 6, AudioChannelOrder::kUnspecified, 0},
      {44100, 1, AudioChannelOrder::kNative,
       0x1},  // a left speaker is not mono/front
      {44100, 2, AudioChannelOrder::kNative, 0xC},  // center/LFE is not stereo
      {48000, 6, AudioChannelOrder::kCustom, 0x3F},
      {48000, 6, AudioChannelOrder::kAmbisonic, 0x3F}};
  for (const auto& entry : rejected) {
    PcmFormatTracker formats;
    PcmAudio pcm;
    CHECK_FALSE(formats.Accept(entry, pcm));
    CHECK(pcm.layout == PcmChannelLayout::kUnknown);
    CHECK(pcm.channels == 0);
    CHECK(pcm.frequency == 0);
    CHECK(pcm.samples.empty());
  }
}
TEST_CASE("Decoded frame layout changes reject before changing existing PCM",
          "[audio][decode][layout]") {
  const AudioFrameFormat initial{48000, 6, AudioChannelOrder::kNative, 0x3F};
  const AudioFrameFormat changes[] = {
      {48000, 6, AudioChannelOrder::kNative, 0x60F},
      {48000, 6, AudioChannelOrder::kNative, 0x500F},
      {44100, 6, AudioChannelOrder::kNative, 0x3F},
      {48000, 2, AudioChannelOrder::kNative, 0x3}};
  for (const auto& changed : changes) {
    PcmFormatTracker formats;
    PcmAudio pcm;
    REQUIRE(formats.Accept(initial, pcm));
    pcm.samples = {1, 2, 3, 4, 5, 6};
    CHECK_FALSE(formats.Accept(changed, pcm));
    CHECK(pcm.layout == PcmChannelLayout::kSurround51Back);
    CHECK(pcm.frequency == 48000);
    CHECK(pcm.channels == 6);
    CHECK(pcm.samples.size() == 6);
    CHECK(formats.Accept(initial, pcm));
  }
  PcmFormatTracker formats;
  PcmAudio pcm;
  REQUIRE(formats.Accept({44100, 2, AudioChannelOrder::kUnspecified, 0}, pcm));
  CHECK_FALSE(formats.Accept({44100, 2, AudioChannelOrder::kNative, 0x3}, pcm));
}
TEST_CASE(
    "Installed stock notification XMA decodes without extraction or playback",
    "[.installed-audio][audio][decode]") {
  const char* source = std::getenv("XENIA_TEST_SYSTEM_XAM_XEX");
  REQUIRE(source);
  const auto size = std::filesystem::file_size(source);
  REQUIRE(size > 0);
  REQUIRE(size <= 16u * 1024u * 1024u);
  std::vector<uint8_t> xex(size);
  std::ifstream file(source, std::ios::binary);
  REQUIRE(static_cast<bool>(file.read(reinterpret_cast<char*>(xex.data()),
                                      std::streamsize(xex.size()))));
  auto skin = vfs::ExtractXexResource(xex, "skin");
  REQUIRE(skin.status == vfs::XexResourceStatus::kLoaded);
  const auto cue =
      resource::LookupXuiPackageFile(skin.bytes, "NotifyPopup.xma");
  REQUIRE(cue.status == resource::XuiPackageStatus::kFound);
  REQUIRE(cue.bytes.size() == 6232);
  std::array<uint8_t, 32> hash{};
  REQUIRE(mbedtls_sha256(cue.bytes.data(), cue.bytes.size(), hash.data(), 0) ==
          0);
  const std::array<uint8_t, 32> expected{
      0xe9, 0xcf, 0xcf, 0x87, 0x1e, 0xb6, 0x0f, 0x80, 0x0d, 0x7a, 0x6c,
      0xfd, 0xf0, 0x4e, 0x5a, 0x29, 0xb8, 0x16, 0xe6, 0x3a, 0x2c, 0x8f,
      0xce, 0xa8, 0x66, 0xe7, 0x96, 0x82, 0x35, 0xc8, 0xc0, 0xf8};
  REQUIRE(hash == expected);
  auto decoded = DecodeAudioMemory(
      std::vector<uint8_t>(cue.bytes.begin(), cue.bytes.end()));
  REQUIRE(decoded.status == AudioDecodeStatus::kSuccess);
  CHECK(decoded.pcm.frequency == 44100);
  CHECK(decoded.pcm.channels == 1);
  CHECK(decoded.pcm.layout == PcmChannelLayout::kMono);
  CHECK_FALSE(decoded.pcm.samples.empty());
  CHECK(decoded.pcm.samples.size() < 44100u * 10u);
}

namespace {
// This fixture remains in the installed XEX. Only transient owned RAM is used;
// no cue, extracted packets or decoded PCM are written to the filesystem.
struct InstalledXmaPackets {
  std::vector<uint8_t> bytes;
  std::array<uint32_t, 3> first_frames{};
};
uint32_t RiffLe32(std::span<const uint8_t> bytes, size_t offset) {
  REQUIRE(offset <= bytes.size());
  REQUIRE(bytes.size() - offset >= 4);
  return uint32_t(bytes[offset]) | (uint32_t(bytes[offset + 1]) << 8) |
         (uint32_t(bytes[offset + 2]) << 16) |
         (uint32_t(bytes[offset + 3]) << 24);
}
uint32_t XmaFixtureBits(std::span<const uint8_t> bytes, uint32_t offset,
                        uint32_t count) {
  REQUIRE(count <= 32);
  REQUIRE(uint64_t(offset) + count <= uint64_t(bytes.size()) * 8);
  uint32_t value = 0;
  for (uint32_t bit = 0; bit < count; ++bit) {
    const uint32_t at = offset + bit;
    value = (value << 1) | ((bytes[at / 8] >> (7 - at % 8)) & 1);
  }
  return value;
}
InstalledXmaPackets ReadInstalledXmaPackets() {
  const char* source = std::getenv("XENIA_TEST_SYSTEM_XAM_XEX");
  REQUIRE(source);
  const auto size = std::filesystem::file_size(source);
  REQUIRE(size > 0);
  REQUIRE(size <= 16u * 1024u * 1024u);
  std::vector<uint8_t> xex(size);
  std::ifstream file(source, std::ios::binary);
  REQUIRE(static_cast<bool>(file.read(reinterpret_cast<char*>(xex.data()),
                                      std::streamsize(xex.size()))));
  auto skin = vfs::ExtractXexResource(xex, "skin");
  REQUIRE(skin.status == vfs::XexResourceStatus::kLoaded);
  const auto cue = resource::LookupXuiPackageFile(skin.bytes, "NotifyPopup.xma");
  REQUIRE(cue.status == resource::XuiPackageStatus::kFound);
  REQUIRE(cue.bytes.size() == 6232);
  std::array<uint8_t, 32> hash{};
  REQUIRE(mbedtls_sha256(cue.bytes.data(), cue.bytes.size(), hash.data(), 0) == 0);
  const std::array<uint8_t, 32> expected{
      0xe9, 0xcf, 0xcf, 0x87, 0x1e, 0xb6, 0x0f, 0x80, 0x0d, 0x7a, 0x6c,
      0xfd, 0xf0, 0x4e, 0x5a, 0x29, 0xb8, 0x16, 0xe6, 0x3a, 0x2c, 0x8f,
      0xce, 0xa8, 0x66, 0xe7, 0x96, 0x82, 0x35, 0xc8, 0xc0, 0xf8};
  REQUIRE(hash == expected);
  REQUIRE(std::memcmp(cue.bytes.data(), "RIFF", 4) == 0);
  REQUIRE(std::memcmp(cue.bytes.data() + 8, "WAVE", 4) == 0);
  REQUIRE(uint64_t(RiffLe32(cue.bytes, 4)) + 8 == cue.bytes.size());
  std::span<const uint8_t> format;
  std::span<const uint8_t> packets;
  size_t offset = 12;
  while (offset < cue.bytes.size()) {
    REQUIRE(cue.bytes.size() - offset >= 8);
    const size_t chunk_size = RiffLe32(cue.bytes, offset + 4);
    REQUIRE(chunk_size <= cue.bytes.size() - offset - 8);
    const auto chunk = cue.bytes.subspan(offset + 8, chunk_size);
    if (std::memcmp(cue.bytes.data() + offset, "fmt ", 4) == 0) {
      REQUIRE(format.empty());
      format = chunk;
    } else if (std::memcmp(cue.bytes.data() + offset, "data", 4) == 0) {
      REQUIRE(packets.empty());
      packets = chunk;
    }
    offset += 8 + chunk_size;
    if (chunk_size & 1) {
      REQUIRE(offset < cue.bytes.size());
      ++offset;
    }
  }
  REQUIRE(offset == cue.bytes.size());
  REQUIRE(format.size() == 32);
  // XMA1 RIFF, pinned above; the installed cue is 44100 Hz mono. The direct
  // XMAFRAMES path receives packet data, never the RIFF header or a fake stereo
  // reinterpretation of this genuine mono stream.
  REQUIRE(format[0] == 0x65);
  REQUIRE(format[1] == 0x01);
  REQUIRE(packets.size() == 3 * XmaContext::kBytesPerPacket);
  InstalledXmaPackets result;
  result.bytes.assign(packets.begin(), packets.end());
  uint32_t at = 32 + ((packets[0] & 3) << 13) + (packets[1] << 5) +
                (packets[2] >> 3);
  // Derive three whole real frame boundaries from the bitstream, independently
  // of XmaContextNew's private parser and next-position helpers.
  for (auto& frame : result.first_frames) {
    frame = at;
    const uint32_t length = XmaFixtureBits(packets, at, 15);
    REQUIRE(length >= 16);
    REQUIRE(length < 0x7FFF);
    REQUIRE(at + length <= XmaContext::kBitsPerPacket);
    REQUIRE(XmaFixtureBits(packets, at + length - 1, 1) == 1);
    at += length;
  }
  return result;
}

class InstalledXmaContext {
 public:
  static constexpr uint32_t kPhysicalWindow = 0xE1800000;
  static constexpr uint32_t kPhysicalBytes = 0x8000;
  static constexpr uint32_t kInputOffset = 0x1000;
  static constexpr uint32_t kOutputOffset = 0x4000;
  static constexpr uint8_t kSentinel = 0xA5;
  explicit InstalledXmaContext(const InstalledXmaPackets& packets) {
    REQUIRE(memory_.Initialize());
    auto* heap = memory_.LookupHeap(kPhysicalWindow);
    REQUIRE(heap);
    REQUIRE(heap->heap_type() == HeapType::kGuestPhysical);
    REQUIRE(heap->AllocFixed(kPhysicalWindow, kPhysicalBytes, 0x1000,
                             kMemoryAllocationReserve | kMemoryAllocationCommit,
                             kMemoryProtectRead | kMemoryProtectWrite));
    physical_ = memory_.GetPhysicalAddress(kPhysicalWindow);
    REQUIRE(physical_ != UINT32_MAX);
    std::memset(memory_.TranslatePhysical(physical_), kSentinel, kPhysicalBytes);
    REQUIRE(packets.bytes.size() <= kOutputOffset - kInputOffset);
    std::memcpy(memory_.TranslatePhysical(physical_ + kInputOffset),
                packets.bytes.data(), packets.bytes.size());
    packet_count_ = static_cast<uint32_t>(packets.bytes.size() /
                                         XmaContext::kBytesPerPacket);
    context_address_ = memory_.SystemHeapAlloc(sizeof(XMA_CONTEXT_DATA));
    REQUIRE(context_address_);
    std::memset(memory_.TranslateVirtual(context_address_), 0,
                sizeof(XMA_CONTEXT_DATA));
    REQUIRE(context_.Setup(0, &memory_, context_address_) == 0);
    context_.set_is_allocated(true);
    context_.Clear();
  }
  ~InstalledXmaContext() {
    context_.Disable();
    if (context_.is_allocated()) {
      context_.Release();
    }
  }
  XMA_CONTEXT_DATA Fresh(uint32_t blocks, uint32_t subframes,
                         uint32_t first_frame) const {
    const std::array<uint8_t, sizeof(XMA_CONTEXT_DATA)> zero{};
    XMA_CONTEXT_DATA data(zero.data());
    data.input_buffer_0_ptr = physical_ + kInputOffset;
    data.input_buffer_0_packet_count = packet_count_;
    data.input_buffer_0_valid = 1;
    data.input_buffer_read_offset = first_frame;
    data.output_buffer_ptr = physical_ + kOutputOffset;
    data.output_buffer_block_count = blocks;
    data.output_buffer_valid = 1;
    data.subframe_decode_count = subframes;
    data.sample_rate = 2;  // Genuine pinned fixture: 44100 Hz mono.
    return data;
  }
  XMA_CONTEXT_DATA Read() const {
    return XMA_CONTEXT_DATA(memory_.TranslateVirtual(context_address_));
  }
  void Store(XMA_CONTEXT_DATA data) {
    data.Store(memory_.TranslateVirtual(context_address_));
  }
  void ResetOutput() {
    std::memset(memory_.TranslatePhysical(physical_ + kOutputOffset), kSentinel,
                XmaContext::kOutputMaxSizeBytes);
  }
  std::vector<uint8_t> Output(size_t bytes) const {
    REQUIRE(bytes <= XmaContext::kOutputMaxSizeBytes);
    const auto* first = memory_.TranslatePhysical(physical_ + kOutputOffset);
    return {first, first + bytes};
  }
  XMA_CONTEXT_DATA Kick() {
    const auto before = Read();
    const size_t end = kOutputOffset +
                       before.output_buffer_block_count *
                           XmaContext::kOutputBytesPerBlock;
    REQUIRE(end <= kPhysicalBytes);
    const auto* physical = memory_.TranslatePhysical(physical_);
    const std::vector<uint8_t> snapshot(physical, physical + kPhysicalBytes);
    context_.Enable();
    REQUIRE(context_.Work());
    CHECK_FALSE(context_.is_enabled());
    CHECK(context_.is_allocated());
    // Check input, prefix and full tail, including bytes outside a short
    // logical ring that still share its committed host pages. Never dump PCM.
    CHECK(std::equal(snapshot.begin(), snapshot.begin() + kOutputOffset,
                      physical));
    CHECK(std::equal(snapshot.begin() + end, snapshot.end(), physical + end));
    return Read();
  }
  void Clear() {
    const auto* physical = memory_.TranslatePhysical(physical_);
    const std::vector<uint8_t> snapshot(physical, physical + kPhysicalBytes);
    context_.Clear();
    CHECK(std::equal(snapshot.begin(), snapshot.end(), physical));
    const auto data = Read();
    CHECK(data.input_buffer_0_valid == 0);
    CHECK(data.input_buffer_1_valid == 0);
    CHECK(data.output_buffer_valid == 0);
    CHECK(data.input_buffer_read_offset == XmaContext::kBitsPerHeader);
    CHECK(data.output_buffer_read_offset == 0);
    CHECK(data.output_buffer_write_offset == 0);
  }
 private:
  Memory memory_;  // Declared first: decoder/codec teardown precedes Memory.
  XmaContextNew context_;
  uint32_t context_address_ = 0;
  uint32_t physical_ = 0;
  uint32_t packet_count_ = 0;
};

void RequireXmaOutputFull(const XMA_CONTEXT_DATA& data) {
  REQUIRE(data.parser_error_set == 0);
  REQUIRE(data.parser_error_status == 0);
  CHECK(data.error_set == 1);
  CHECK(data.error_status == XmaContextNew::kErrorOutputFull);
  CHECK(data.output_buffer_valid == 0);
  CHECK(data.output_buffer_write_offset == 0);
}
}  // namespace

TEST_CASE("XMAFRAMES stock cue preserves two-frame pass admission and batching",
          "[.installed-xmaframes][audio][xmaframes]") {
  const auto packets = ReadInstalledXmaPackets();
  InstalledXmaContext decoder(packets);
  auto data = decoder.Fresh(7, 8, packets.first_frames[0]);
  decoder.Store(data);
  const auto untouched = decoder.Output(7 * XmaContext::kOutputBytesPerBlock);
  const auto short_pass = decoder.Kick();
  CHECK(short_pass.error_set == 1);
  CHECK(short_pass.error_status == XmaContextNew::kErrorOutputSpace);
  CHECK(short_pass.parser_error_set == 0);
  CHECK(short_pass.input_buffer_read_offset == packets.first_frames[0]);
  CHECK(short_pass.decode_subframe == 0);
  CHECK(short_pass.input_buffer_0_valid == 1);
  CHECK(short_pass.output_buffer_write_offset == 0);
  CHECK(short_pass.output_buffer_valid == 1);
  const auto still_untouched = decoder.Output(untouched.size());
  CHECK(std::equal(untouched.begin(), untouched.end(), still_untouched.begin()));

  decoder.Clear();
  decoder.ResetOutput();
  decoder.Store(decoder.Fresh(8, 8, packets.first_frames[0]));
  const auto exact_pass = decoder.Kick();
  RequireXmaOutputFull(exact_pass);
  CHECK(exact_pass.input_buffer_read_offset == packets.first_frames[2]);
  CHECK(exact_pass.decode_subframe == 0);
  CHECK(exact_pass.input_buffer_0_valid == 1);
  const auto batched = decoder.Output(8 * XmaContext::kOutputBytesPerBlock);
  CHECK_FALSE(std::all_of(batched.begin(), batched.end(),
                          [](uint8_t value) { return value == 0xA5; }));

  // The same genuine frames must produce identical PCM whether delivered by
  // an eight-subframe kick or two four-subframe kicks. Clear must remove codec
  // overlap from the preceding run; no hardware PCM oracle is assumed.
  decoder.Clear();
  decoder.ResetOutput();
  decoder.Store(decoder.Fresh(4, 4, packets.first_frames[0]));
  const auto first = decoder.Kick();
  RequireXmaOutputFull(first);
  CHECK(first.input_buffer_read_offset == packets.first_frames[1]);
  auto chunked = decoder.Output(4 * XmaContext::kOutputBytesPerBlock);
  data = decoder.Read();
  data.output_buffer_valid = 1;
  decoder.ResetOutput();
  decoder.Store(data);
  const auto second = decoder.Kick();
  RequireXmaOutputFull(second);
  CHECK(second.input_buffer_read_offset == packets.first_frames[2]);
  const auto tail = decoder.Output(4 * XmaContext::kOutputBytesPerBlock);
  chunked.insert(chunked.end(), tail.begin(), tail.end());
  REQUIRE(chunked.size() == batched.size());
  CHECK(std::equal(chunked.begin(), chunked.end(), batched.begin()));
  decoder.Clear();
}

TEST_CASE("XMAFRAMES stock cue preserves finite loop and held subframe position",
          "[.installed-xmaframes][audio][xmaframes]") {
  const auto packets = ReadInstalledXmaPackets();
  InstalledXmaContext decoder(packets);
  auto data = decoder.Fresh(6, 2, packets.first_frames[0]);
  data.loop_start = packets.first_frames[0];
  data.loop_end = packets.first_frames[1];
  data.loop_count = 1;
  data.loop_subframe_end = 1;  // Cut the second frame after two subframes.
  data.loop_subframe_skip = 1;
  decoder.Store(data);
  const auto looped = decoder.Kick();
  RequireXmaOutputFull(looped);
  CHECK(looped.loop_count == 0);
  CHECK(looped.input_buffer_read_offset == packets.first_frames[0]);
  CHECK(looped.decode_subframe == 1);
  CHECK(looped.input_buffer_0_valid == 1);

  // The next kick starts inside the loop-start frame. A two-block ring holds
  // only two of its three remaining mono subframes: position must stay on that
  // real frame, and its last block must survive the following kick.
  data = decoder.Read();
  data.output_buffer_block_count = 2;
  data.output_buffer_valid = 1;
  decoder.ResetOutput();
  decoder.Store(data);
  const auto held = decoder.Kick();
  RequireXmaOutputFull(held);
  CHECK(held.loop_count == 0);
  CHECK(held.input_buffer_read_offset == packets.first_frames[0]);
  CHECK(held.decode_subframe == 3);

  data = decoder.Read();
  data.output_buffer_block_count = 1;
  data.subframe_decode_count = 1;
  data.output_buffer_valid = 1;
  decoder.ResetOutput();
  decoder.Store(data);
  const auto advanced = decoder.Kick();
  RequireXmaOutputFull(advanced);
  CHECK(advanced.loop_count == 0);
  CHECK(advanced.input_buffer_read_offset == packets.first_frames[1]);
  CHECK(advanced.decode_subframe == 0);
  CHECK(advanced.input_buffer_0_valid == 1);
  decoder.Clear();
}
TEST_CASE("XMAFRAMES accepts declared padding after genuine frame data",
          "[.installed-xmaframes][.installed-xmaframes-tail][audio][xmaframes]") {
  const auto packets = ReadInstalledXmaPackets();
  std::vector<uint8_t> reference;
  {
    InstalledXmaContext decoder(packets);
    decoder.Store(decoder.Fresh(4, 4, packets.first_frames[0]));
    const auto first = decoder.Kick();
    RequireXmaOutputFull(first);
    reference = decoder.Output(4 * XmaContext::kOutputBytesPerBlock);
    decoder.Clear();
  }

  // Preserve the genuine first frame's coded samples. Increase only its
  // declared extent by 32 bits, use those bits as zero padding, and terminate
  // that packet's frame chain at the new end. Later frames are not decoded in
  // this controlled one-frame view. The installed XEX is never modified.
  auto padded = packets;
  const uint32_t start = packets.first_frames[0];
  const uint32_t old_length = XmaFixtureBits(packets.bytes, start, 15);
  const uint32_t new_length = old_length + 32;
  REQUIRE(start + new_length < XmaContext::kBitsPerPacket);
  auto bit = [&](uint32_t at, bool value) {
    REQUIRE(uint64_t(at) < uint64_t(padded.bytes.size()) * 8);
    const uint8_t mask = uint8_t(1u << (7 - at % 8));
    auto& byte = padded.bytes[at / 8];
    byte = value ? uint8_t(byte | mask) : uint8_t(byte & ~mask);
  };
  for (uint32_t i = 0; i != 15; ++i) {
    bit(start + i, (new_length >> (14 - i)) & 1);
  }
  for (uint32_t i = old_length; i != new_length; ++i) {
    bit(start + i, false);
  }
  CHECK(XmaFixtureBits(padded.bytes, start, 15) == new_length);
  CHECK(XmaFixtureBits(padded.bytes, start + new_length - 1, 1) == 0);
  InstalledXmaContext decoder(padded);
  decoder.Store(decoder.Fresh(4, 4, start));
  const auto first = decoder.Kick();
  RequireXmaOutputFull(first);
  const auto output = decoder.Output(reference.size());
  CHECK(output == reference);
  decoder.Clear();
}

TEST_CASE("XMAFRAMES stock cue drains split packets and its padded tail",
          "[.installed-xmaframes][.installed-xmaframes-tail][audio][xmaframes]") {
  const auto packets = ReadInstalledXmaPackets();
  InstalledXmaContext decoder(packets);
  // Seven complete mono frames fit each ring. This is a caller consuming
  // each returned ring in full, not a second decoder or a hardware PCM oracle.
  constexpr uint32_t blocks = 28;
  auto data = decoder.Fresh(blocks, 4, packets.first_frames[0]);
  size_t pcm_bytes = 0;
  bool nonzero_pcm = false, drained = false;
  for (uint32_t kick = 0; kick != 128; ++kick) {
    const std::array<uint32_t, 5> before{
        data.current_buffer, data.input_buffer_0_valid,
        data.input_buffer_1_valid, data.input_buffer_read_offset,
        data.decode_subframe};
    data.output_buffer_valid = 1;
    data.output_buffer_read_offset = 0;
    data.output_buffer_write_offset = 0;
    decoder.ResetOutput();
    decoder.Store(data);
    data = decoder.Kick();
    REQUIRE(data.parser_error_set == 0);
    REQUIRE(data.parser_error_status == 0);
    REQUIRE(data.error_set == 1);
    REQUIRE((data.error_status == XmaContextNew::kErrorOutputFull ||
             data.error_status == XmaContextNew::kErrorNoInput));
    const uint32_t used_blocks = data.output_buffer_valid
                                     ? data.output_buffer_write_offset
                                     : blocks;
    REQUIRE(used_blocks > 0);
    REQUIRE(used_blocks <= blocks);
    const auto pcm = decoder.Output(used_blocks * XmaContext::kOutputBytesPerBlock);
    pcm_bytes += pcm.size();
    REQUIRE(pcm_bytes <= 1024u * 1024u);
    nonzero_pcm |= std::any_of(pcm.begin(), pcm.end(),
                               [](uint8_t value) { return value != 0; });
    if (!data.IsAnyInputBufferValid()) {
      CHECK(data.error_status == XmaContextNew::kErrorNoInput);
      drained = true;
      break;
    }
    const std::array<uint32_t, 5> after{
        data.current_buffer, data.input_buffer_0_valid,
        data.input_buffer_1_valid, data.input_buffer_read_offset,
        data.decode_subframe};
    REQUIRE(after != before);  // No repeated kick on a stuck frame accepted.
  }
  CHECK(drained);
  CHECK(nonzero_pcm);
  CHECK(pcm_bytes > 8 * XmaContext::kOutputBytesPerBlock);
  decoder.Clear();
}

TEST_CASE("XMAFRAMES zero loop end drains the complete stock cue without looping",
          "[.installed-xmaframes][audio][xmaframes]") {
  const auto packets = ReadInstalledXmaPackets();
  // The old clamp treats this genuine first frame as loop_end == 0. Keep the
  // actual installed frame boundary as the regression's prerequisite.
  REQUIRE(packets.first_frames[0] == XmaContextNew::kBitsPerPacketHeader);
  const auto drain = [&](uint32_t loop_count) {
    InstalledXmaContext decoder(packets);
    constexpr uint32_t blocks = 28;
    auto data = decoder.Fresh(blocks, 4, packets.first_frames[0]);
    data.loop_count = loop_count;
    data.loop_start = 0;
    data.loop_end = 0;
    data.loop_subframe_end = 0;
    data.loop_subframe_skip = 0;
    std::vector<uint8_t> pcm;
    bool drained = false;
    for (uint32_t kick = 0; kick != 128; ++kick) {
      const std::array<uint32_t, 5> before{
          data.current_buffer, data.input_buffer_0_valid,
          data.input_buffer_1_valid, data.input_buffer_read_offset,
          data.decode_subframe};
      data.output_buffer_valid = 1;
      data.output_buffer_read_offset = 0;
      data.output_buffer_write_offset = 0;
      decoder.ResetOutput();
      decoder.Store(data);
      data = decoder.Kick();
      REQUIRE(data.parser_error_set == 0);
      REQUIRE(data.parser_error_status == 0);
      REQUIRE(data.error_set == 1);
      REQUIRE((data.error_status == XmaContextNew::kErrorOutputFull ||
               data.error_status == XmaContextNew::kErrorNoInput));
      // Zero loop_end is not a frame and must not consume either a finite or
      // an indefinite loop count while the stream plays through normally.
      REQUIRE(data.loop_count == loop_count);
      const uint32_t used_blocks = data.output_buffer_valid
                                       ? data.output_buffer_write_offset
                                       : blocks;
      REQUIRE(used_blocks > 0);
      REQUIRE(used_blocks <= blocks);
      const auto chunk =
          decoder.Output(used_blocks * XmaContext::kOutputBytesPerBlock);
      REQUIRE(chunk.size() <= 1024u * 1024u - pcm.size());
      pcm.insert(pcm.end(), chunk.begin(), chunk.end());
      if (!data.IsAnyInputBufferValid()) {
        CHECK(data.error_status == XmaContextNew::kErrorNoInput);
        drained = true;
        break;
      }
      const std::array<uint32_t, 5> after{
          data.current_buffer, data.input_buffer_0_valid,
          data.input_buffer_1_valid, data.input_buffer_read_offset,
          data.decode_subframe};
      REQUIRE(after != before);
    }
    REQUIRE(drained);
    REQUIRE(pcm.size() > 8 * XmaContext::kOutputBytesPerBlock);
    REQUIRE(std::any_of(pcm.begin(), pcm.end(),
                         [](uint8_t value) { return value != 0; }));
    decoder.Clear();
    return pcm;
  };
  // Compare the complete real stream, including split packets and its tail,
  // to the same decoder with looping disabled; no hardware PCM oracle or
  // rewritten packet fixture is assumed, and PCM is never written or dumped.
  const auto reference = drain(0);
  for (const uint32_t loop_count : std::array<uint32_t, 2>{1, 255}) {
    CAPTURE(loop_count);
    const auto output = drain(loop_count);
    REQUIRE(output.size() == reference.size());
    CHECK(std::equal(output.begin(), output.end(), reference.begin()));
  }
}

TEST_CASE("XMAFRAMES zero loop start stops at the header after genuine frames",
          "[.installed-xmaframes][audio][xmaframes]") {
  const auto packets = ReadInstalledXmaPackets();
  InstalledXmaContext decoder(packets);
  decoder.Store(decoder.Fresh(8, 4, packets.first_frames[0]));
  const auto ordinary = decoder.Kick();
  RequireXmaOutputFull(ordinary);
  REQUIRE(ordinary.input_buffer_read_offset == packets.first_frames[2]);
  const auto reference = decoder.Output(8 * XmaContext::kOutputBytesPerBlock);
  decoder.Clear();
  decoder.ResetOutput();

  // Play two complete genuine frames, then jump to the requested raw offset
  // zero. It is a packet header, not an implicit seek to the first frame. Leave
  // room for a third frame so an incorrect clamp cannot hide behind a full ring.
  auto data = decoder.Fresh(12, 4, packets.first_frames[0]);
  data.loop_start = 0;
  data.loop_end = packets.first_frames[1];
  data.loop_count = 1;
  data.loop_subframe_end = 3;
  data.loop_subframe_skip = 0;
  decoder.Store(data);
  const auto stopped = decoder.Kick();
  CHECK(stopped.loop_count == 0);
  CHECK(stopped.input_buffer_read_offset == 0);
  CHECK(stopped.decode_subframe == 0);
  CHECK(stopped.input_buffer_0_valid == 1);
  CHECK(stopped.input_buffer_1_valid == 0);
  CHECK(stopped.parser_error_set == 1);
  CHECK(stopped.parser_error_status == XmaContextNew::kParserErrorNoFrame);
  CHECK(stopped.error_set == 0);
  CHECK(stopped.error_status == 0);
  CHECK(stopped.output_buffer_valid == 1);
  CHECK(stopped.output_buffer_read_offset == 0);
  CHECK(stopped.output_buffer_write_offset == 8);
  const auto output = decoder.Output(12 * XmaContext::kOutputBytesPerBlock);
  CHECK(std::equal(reference.begin(), reference.end(), output.begin()));
  CHECK(std::all_of(output.begin() + reference.size(), output.end(),
                    [](uint8_t value) {
                      return value == InstalledXmaContext::kSentinel;
                    }));

  // A further ordinary kick at that unchanged header must not replay or
  // consume any frame or overwrite the remaining output capacity.
  const auto again = decoder.Kick();
  CHECK(again.input_buffer_read_offset == 0);
  CHECK(again.loop_count == 0);
  CHECK(again.parser_error_set == 1);
  CHECK(again.parser_error_status == XmaContextNew::kParserErrorNoFrame);
  CHECK(again.output_buffer_write_offset == 8);
  CHECK(again.input_buffer_0_valid == 1);
  const auto unchanged = decoder.Output(output.size());
  CHECK(std::equal(unchanged.begin(), unchanged.end(), output.begin()));
  decoder.Clear();
}
}  // namespace xe::apu::test
