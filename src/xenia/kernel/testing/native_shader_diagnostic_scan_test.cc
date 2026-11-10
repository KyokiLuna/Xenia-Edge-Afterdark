/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia contributors. All rights reserved.                    *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <algorithm>
#include <cstdint>
#include <initializer_list>
#include <vector>

#include "third_party/catch/include/catch.hpp"
#include "xenia/gpu/native_command_stream.h"
#include "xenia/gpu/native_shader_diagnostic_scan.h"
#include "xenia/gpu/xenos.h"

namespace xe::gpu::test {
namespace {

using Scan = NativeShaderDiagnosticScan;
using Error = Scan::Error;

// Synthetic framing/operands only, never native shader code or proof of an
// executable stream. No guest memory or backend is supplied to the scanner.
struct Primary {
  std::vector<uint32_t> words;
  uint32_t Packet(uint32_t opcode, std::initializer_list<uint32_t> payload) {
    REQUIRE(payload.size());
    const uint32_t offset = uint32_t(words.size());
    words.push_back(0xC0000000 | uint32_t(payload.size() - 1) << 16 |
                    opcode << 8);
    words.insert(words.end(), payload);
    return offset;
  }
  std::vector<uint8_t> Bytes() const {
    std::vector<uint8_t> bytes;
    for (const auto word : words) {
      bytes.push_back(uint8_t(word >> 24));
      bytes.push_back(uint8_t(word >> 16));
      bytes.push_back(uint8_t(word >> 8));
      bytes.push_back(uint8_t(word));
    }
    return bytes;
  }
  Scan::Result Analyze() const { return Scan::Scan(Bytes()); }
};

TEST_CASE("Diagnostic shader candidates survive unsupported primary draws",
          "[graphics][native_shader_diagnostic]") {
  Primary primary;
  primary.Packet(xenos::PM4_DRAW_INDX_2, {3u << 16 | 0x88});
  primary.Packet(xenos::PM4_IM_LOAD_IMMEDIATE, {0, 3, 0, 0, 0});
  const auto offset = primary.Packet(xenos::PM4_IM_LOAD, {0xE0100001, 9});
  auto bytes = primary.Bytes();
  // Host input alignment is irrelevant; the PM4 words themselves are read BE.
  bytes.insert(bytes.begin(), 0xAB);
  const auto input = std::span<const uint8_t>(bytes).subspan(1);
  const auto strict_before = NativeCommandStream::Analyze(input);
  REQUIRE_FALSE(strict_before);
  CHECK(strict_before.error == NativeCommandStream::Error::kUnsupportedPacket);
  CHECK(strict_before.error_dword_offset == 0);
  const auto result = Scan::Scan(input);
  REQUIRE(result.complete_primary_scan);
  CHECK(result.error == Error::kNone);
  CHECK(result.packet_count == 3);
  CHECK(result.shader_load_count == 2);
  CHECK(result.uninterpreted_type3_packets == 1);
  REQUIRE(result.candidates.size() == 1);
  const auto candidate = result.candidates.front();
  CHECK(candidate.stage == Scan::Stage::kPixel);
  CHECK(candidate.packet_dword_offset == offset);
  CHECK(candidate.shader_load_index == 1);
  CHECK(candidate.raw_address == 0xE0100000);
  CHECK(candidate.physical_address == 0x00100000);
  CHECK(candidate.byte_count == 36);
  const auto strict_after = NativeCommandStream::Analyze(input);
  CHECK(strict_after.error == strict_before.error);
  CHECK(strict_after.error_dword_offset == strict_before.error_dword_offset);
  CHECK_FALSE(strict_after.stream);
  std::fill(bytes.begin(), bytes.end(), 0);
  CHECK(result.candidates.front().raw_address == 0xE0100000);
}

TEST_CASE("Diagnostic scan does not recurse or parse payload as primary PM4",
          "[graphics][native_shader_diagnostic]") {
  Primary primary;
  // Type0 values look exactly like an external load packet.
  primary.words = {2u << 16 | 0x2000, 0xC0012700, 0x00100001, 9};
  // Type1 likewise contains arbitrary register values, not nested packets.
  primary.words.insert(primary.words.end(), {0x40000000, 0xC0012700, 9});
  primary.Packet(xenos::PM4_INDIRECT_BUFFER, {0x00100000, 3});
  primary.Packet(xenos::PM4_COND_EXEC, {0xC0012700, 0x00100001, 9, 0});
  // Even inside known inline load data, these words are not primary packets.
  primary.Packet(xenos::PM4_IM_LOAD_IMMEDIATE,
                 {0, 3, 0xC0012700, 0x00100001, 9});
  primary.words.push_back(0x80000000);
  const auto offset = primary.Packet(xenos::PM4_IM_LOAD, {0x00200000, 1});
  const auto result = primary.Analyze();
  REQUIRE(result.complete_primary_scan);
  CHECK(result.indirect_buffer_packets == 1);
  CHECK(result.uninterpreted_type3_packets == 2);
  CHECK(result.shader_load_count == 2);
  REQUIRE(result.candidates.size() == 1);
  CHECK(result.candidates.front().stage == Scan::Stage::kVertex);
  CHECK(result.candidates.front().packet_dword_offset == offset);
  CHECK(result.candidates.front().physical_address == 0x00200000);
}

TEST_CASE("A malformed primary tail discards diagnostic candidates",
          "[graphics][native_shader_diagnostic]") {
  Primary primary;
  primary.Packet(xenos::PM4_IM_LOAD, {0x00100001, 9});
  const uint32_t tail = uint32_t(primary.words.size());
  Error expected = Error::kTruncatedPacket;
  SECTION("type0 missing its register payload") {
    primary.words.push_back(1u << 16 | 0x2000);
  }
  SECTION("type1 missing its second payload word") {
    primary.words.insert(primary.words.end(), {0x40000000, 0});
  }
  SECTION("unsupported type3 still requires its complete payload") {
    primary.words.insert(primary.words.end(), {0xC0027E00, 0});
  }
  SECTION("external shader load must have its complete operand payload") {
    primary.words.insert(primary.words.end(), {0xC0012700, 0x00100001});
  }
  SECTION("inline shader load must have its complete code payload") {
    primary.words.insert(primary.words.end(), {0xC0042B00, 0, 3, 0});
  }
  SECTION("noncanonical padding is refused") {
    primary.words.push_back(0x80000001);
    expected = Error::kInvalidPadding;
  }
  const auto result = primary.Analyze();
  CHECK_FALSE(result.complete_primary_scan);
  CHECK(result.error == expected);
  CHECK(result.error_dword_offset == tail);
  CHECK(result.candidates.empty());
}

TEST_CASE("Diagnostic shader loads preserve strict operand and length checks",
          "[graphics][native_shader_diagnostic]") {
  Primary primary;
  primary.Packet(xenos::PM4_IM_LOAD, {0x00200000, 1});
  uint32_t offset = 0;
  SECTION("external stage2 is not a vertex address with tolerated alignment") {
    offset = primary.Packet(xenos::PM4_IM_LOAD, {0x00100002, 9});
  }
  SECTION("external stage3 is invalid") {
    offset = primary.Packet(xenos::PM4_IM_LOAD, {0x00100003, 9});
  }
  SECTION("header predication is not inferred for a known load") {
    offset = primary.Packet(xenos::PM4_IM_LOAD, {0x00100001, 9});
    primary.words[offset] |= 1;
  }
  SECTION("nonzero shader start is refused") {
    offset = primary.Packet(xenos::PM4_IM_LOAD, {0x00100001, 0x10009});
  }
  SECTION("zero shader size is refused") {
    offset = primary.Packet(xenos::PM4_IM_LOAD, {0x00100001, 0});
  }
  SECTION("shader size above profile maximum is refused") {
    offset = primary.Packet(xenos::PM4_IM_LOAD, {0x00100001, 4097});
  }
  SECTION("external load missing its size operand") {
    offset = primary.Packet(xenos::PM4_IM_LOAD, {0x00100001});
  }
  SECTION("external load has extra payload") {
    offset = primary.Packet(xenos::PM4_IM_LOAD, {0x00100001, 9, 0});
  }
  SECTION("inline stage is the complete operand") {
    offset = primary.Packet(xenos::PM4_IM_LOAD_IMMEDIATE, {0x00100000, 1, 0});
  }
  SECTION("inline load payload must match shader size") {
    offset = primary.Packet(xenos::PM4_IM_LOAD_IMMEDIATE, {0, 2, 0});
  }
  const auto result = primary.Analyze();
  CHECK(result.error == Error::kInvalidShaderLoad);
  CHECK(result.error_dword_offset == offset);
  CHECK_FALSE(result.complete_primary_scan);
  CHECK(result.candidates.empty());
}

TEST_CASE("Diagnostic reads bound the actual GPU operand without CPU bias",
          "[graphics][native_shader_diagnostic]") {
  Primary primary;
  SECTION("last physical DWORD is representable through a mirror") {
    primary.Packet(xenos::PM4_IM_LOAD, {0xFFFFFFFD, 1});
    const auto result = primary.Analyze();
    REQUIRE(result.complete_primary_scan);
    REQUIRE(result.candidates.size() == 1);
    CHECK(result.candidates[0].raw_address == 0xFFFFFFFC);
    CHECK(result.candidates[0].physical_address == 0x1FFFFFFC);
    CHECK(result.candidates[0].byte_count == 4);
  }
  SECTION("a shader crossing the end of physical memory is refused") {
    primary.Packet(xenos::PM4_IM_LOAD, {0x1FFFFFFD, 2});
    CHECK(primary.Analyze().error == Error::kInvalidMemoryRange);
    CHECK(primary.Analyze().candidates.empty());
  }
  SECTION("GPU address zero is inventoried without inventing readability") {
    primary.Packet(xenos::PM4_IM_LOAD, {0, 1});
    const auto result = primary.Analyze();
    REQUIRE(result.complete_primary_scan);
    REQUIRE(result.candidates.size() == 1);
    CHECK(result.candidates[0].physical_address == 0);
  }
}

TEST_CASE("Diagnostic scan rejects invalid input and enforces bounded budgets",
          "[graphics][native_shader_diagnostic]") {
  Primary primary;
  primary.Packet(xenos::PM4_IM_LOAD, {0x00100001, 2});
  primary.Packet(xenos::PM4_IM_LOAD, {0x00100001, 2});
  Scan::Limits limits;
  SECTION("empty input") {
    CHECK(Scan::Scan({}).error == Error::kInvalidByteCount);
  }
  SECTION("byte count must be a DWORD multiple") {
    auto bytes = primary.Bytes();
    bytes.pop_back();
    CHECK(Scan::Scan(bytes).error == Error::kInvalidByteCount);
  }
  SECTION("primary DWORD count") {
    limits.max_dwords = 3;
    CHECK(Scan::Scan(primary.Bytes(), limits).error == Error::kLimitExceeded);
  }
  SECTION("packet count") {
    limits.max_packets = 1;
    CHECK(Scan::Scan(primary.Bytes(), limits).error == Error::kLimitExceeded);
  }
  SECTION("every duplicate declaration consumes a candidate slot") {
    limits.max_external_reads = 1;
    const auto result = Scan::Scan(primary.Bytes(), limits);
    CHECK(result.error == Error::kLimitExceeded);
    CHECK(result.candidates.empty());
  }
  SECTION("shader size") {
    limits.max_shader_dwords = 1;
    CHECK(Scan::Scan(primary.Bytes(), limits).error ==
          Error::kInvalidShaderLoad);
  }
  SECTION("zero limits") {
    limits.max_packets = 0;
    CHECK(Scan::Scan(primary.Bytes(), limits).error == Error::kInvalidLimits);
    limits = {};
    limits.max_external_reads = 0;
    CHECK(Scan::Scan(primary.Bytes(), limits).error == Error::kInvalidLimits);
  }
  SECTION("hard limits cannot be raised") {
    limits.max_shader_dwords = 4097;
    CHECK(Scan::Scan(primary.Bytes(), limits).error == Error::kInvalidLimits);
    limits = {};
    limits.max_dwords = 0x10001;
    CHECK(Scan::Scan(primary.Bytes(), limits).error == Error::kInvalidLimits);
  }
}

}  // namespace
}  // namespace xe::gpu::test
