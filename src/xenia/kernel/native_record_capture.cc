/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia contributors. All rights reserved.                    *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/native_record_capture.h"

#include <array>
#include <cstdio>
#include <cstring>
#include <exception>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "third_party/fmt/include/fmt/format.h"
#include "third_party/mbedtls/include/mbedtls/sha256.h"
#include "xenia/base/filesystem.h"
#include "xenia/base/logging.h"
#include "xenia/base/mutex.h"
#include "xenia/gpu/native_command_stream.h"
#include "xenia/gpu/native_draw_read_inventory.h"
#include "xenia/gpu/native_frame_command_plan.h"
#include "xenia/gpu/native_shader_diagnostic_scan.h"
#include "xenia/gpu/native_shader_read_analysis.h"
#include "xenia/gpu/native_vertex_access_analysis.h"
#include "xenia/memory.h"

#if !XE_PLATFORM_WIN32
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace xe::kernel {
namespace {

using Stream = gpu::NativeCommandStream;
using ShaderScan = gpu::NativeShaderDiagnosticScan;
using DrawInventory = gpu::NativeDrawReadInventory;
using ShaderAnalysis = gpu::NativeShaderReadAnalysis;
using VertexAccess = gpu::NativeVertexAccessAnalysis;
constexpr size_t kMaxPrimaryBytes = 0x40000;
constexpr size_t kMaxDependencyBytes = 4 * 1024 * 1024;
constexpr size_t kMaxPayloadBytes = 15 * 1024 * 1024;
constexpr size_t kMaxManifestBytes = 1024 * 1024;
constexpr uint32_t kPhysicalSize = 0x20000000;

struct WriteResult {
  bool created = false;
  bool complete = false;
  size_t written = 0;
};

WriteResult WriteExclusive(const std::filesystem::path& path,
                           std::span<const uint8_t> bytes) {
  // C11 exclusive creation: unlike exists()+ofstream, this cannot truncate a
  // destination created after the preflight check. Unsupported modes fail.
  FILE* file = filesystem::OpenFile(path, "wbx");
  if (!file) {
    return {};
  }
  WriteResult result{true, false, 0};
#if !XE_PLATFORM_WIN32
  // Restrict the new file before writing any private guest bytes. No path
  // based permission change after opening, so a rename cannot redirect it.
  if (::fchmod(::fileno(file), S_IRUSR | S_IWUSR)) {
    std::fclose(file);
    return result;
  }
#endif
  result.written = std::fwrite(bytes.data(), 1, bytes.size(), file);
  result.complete = result.written == bytes.size() && !std::fflush(file);
  if (std::fclose(file)) {
    result.complete = false;
  }
  return result;
}

std::optional<std::string> Sha256(std::span<const uint8_t> bytes) {
  std::array<uint8_t, 32> digest;
  if (mbedtls_sha256(bytes.data(), bytes.size(), digest.data(), 0)) {
    return {};
  }
  std::string hex;
  hex.reserve(64);
  constexpr char digits[] = "0123456789abcdef";
  for (const auto byte : digest) {
    hex.push_back(digits[byte >> 4]);
    hex.push_back(digits[byte & 15]);
  }
  return hex;
}

const char* ErrorName(Stream::Error error) {
  switch (error) {
    case Stream::Error::kNone:
      return "none";
    case Stream::Error::kInvalidLimits:
      return "invalid_limits";
    case Stream::Error::kInvalidByteCount:
      return "invalid_byte_count";
    case Stream::Error::kLimitExceeded:
      return "limit_exceeded";
    case Stream::Error::kTruncatedPacket:
      return "truncated_packet";
    case Stream::Error::kUnsupportedPacket:
      return "unsupported_packet";
    case Stream::Error::kUnsupportedRegister:
      return "unsupported_register";
    case Stream::Error::kUnsupportedWait:
      return "unsupported_wait";
    case Stream::Error::kInvalidShader:
      return "invalid_shader";
    case Stream::Error::kInvalidDraw:
      return "invalid_draw";
    case Stream::Error::kMissingLocalState:
      return "missing_local_state";
    case Stream::Error::kUnsupportedTexture:
      return "unsupported_texture";
    case Stream::Error::kInvalidMemoryRange:
      return "invalid_memory_range";
    case Stream::Error::kNoDraw:
      return "no_draw";
  }
  return "unknown";
}

// Capture-only read identity. The strict executable-profile dependency type
// deliberately has no texture support; a diagnostic snapshot must not extend
// that profile or make a rejected stream executable.
struct CaptureRead {
  enum class Kind { kShaderCode, kDeclaredVertexBuffer, kTextureStorage };
  Kind kind;
  uint32_t packet_dword_offset;
  uint32_t raw_address;
  uint32_t physical_address;
  uint32_t byte_count;
  uint32_t binding_index;

  static CaptureRead FromStrict(const Stream::MemoryDependency& read) {
    return {read.kind == Stream::MemoryDependency::Kind::kShaderCode
                ? Kind::kShaderCode
                : Kind::kDeclaredVertexBuffer,
            read.packet_dword_offset,
            read.raw_address,
            read.physical_address,
            read.byte_count,
            read.binding_index};
  }
};

struct Snapshot {
  CaptureRead dependency;
  std::optional<ShaderScan::Stage> diagnostic_stage;
  const char* status = "not_read";
  std::string file;
  std::optional<std::string> hash;
  size_t written = 0;
  std::vector<uint8_t> shader_bytes;
  bool diagnostic_vertex_read = false;
  bool diagnostic_texture_read = false;
  bool resolve_rectangle = false;
};

void DigestJson(std::ostream& out, const std::optional<std::string>& hash) {
  if (hash) {
    out << '"' << *hash << '"';
  } else {
    out << "null";
  }
}

void TextureFetchJson(std::ostream& out,
                      const ShaderAnalysis::TextureFetch& fetch) {
  out << "{\"instruction_dword_offset\":" << fetch.instruction_dword_offset
      << ",\"slot\":" << fetch.slot << ",\"dimension\":" << fetch.dimension
      << ",\"normalized_coordinates\":"
      << (fetch.normalized_coordinates ? "true" : "false")
      << ",\"coordinate_register\":" << fetch.coordinate_register
      << ",\"coordinate_swizzle\":" << fetch.coordinate_swizzle
      << ",\"coordinate_relative\":"
      << (fetch.coordinate_relative ? "true" : "false")
      << ",\"dest_register\":" << fetch.dest_register
      << ",\"dest_swizzle\":" << fetch.dest_swizzle
      << ",\"dest_relative\":" << (fetch.dest_relative ? "true" : "false")
      << ",\"fetch_valid_only\":" << (fetch.fetch_valid_only ? "true" : "false")
      << ",\"mag_filter\":" << fetch.mag_filter
      << ",\"min_filter\":" << fetch.min_filter
      << ",\"mip_filter\":" << fetch.mip_filter
      << ",\"aniso_filter\":" << fetch.aniso_filter
      << ",\"arbitrary_filter\":" << fetch.arbitrary_filter
      << ",\"vol_mag_filter\":" << fetch.vol_mag_filter
      << ",\"vol_min_filter\":" << fetch.vol_min_filter
      << ",\"sample_location\":" << fetch.sample_location
      << ",\"use_computed_lod\":" << (fetch.use_computed_lod ? "true" : "false")
      << ",\"use_register_lod\":" << (fetch.use_register_lod ? "true" : "false")
      << ",\"use_register_gradients\":"
      << (fetch.use_register_gradients ? "true" : "false")
      << ",\"lod_bias\":" << fetch.lod_bias
      << ",\"offset_x\":" << fetch.offset_x
      << ",\"offset_y\":" << fetch.offset_y
      << ",\"offset_z\":" << fetch.offset_z << ",\"instruction_words\":["
      << fetch.instruction_words[0] << ',' << fetch.instruction_words[1] << ','
      << fetch.instruction_words[2] << "]}";
}

void TextureReadJson(std::ostream& out,
                     const DrawInventory::TextureRead& read) {
  out << "{\"slot\":" << read.slot << ",\"raw_address\":\""
      << fmt::format("{:08X}", read.raw_address) << "\",\"physical_address\":\""
      << fmt::format("{:08X}", read.physical_address)
      << "\",\"byte_count\":" << read.byte_count
      << ",\"logical_texel_bytes\":" << read.logical_texel_bytes
      << ",\"row_pitch_bytes\":" << read.row_pitch_bytes
      << ",\"padded_span_bytes\":" << read.padded_span_bytes
      << ",\"storage_stride_bytes\":" << read.storage_stride_bytes
      << ",\"width\":" << read.width << ",\"height\":" << read.height
      << ",\"format\":" << read.format << ",\"endian\":" << read.endian
      << ",\"fetch_words\":[";
  for (size_t i = 0; i < read.fetch_words.size(); ++i) {
    out << (i ? "," : "") << read.fetch_words[i];
  }
  const size_t count = read.instructions.size();
  out << "],\"instruction_count\":" << count
      << ",\"instruction_details_complete\":"
      << (count <= 64 ? "true" : "false") << ",\"instructions\":[";
  for (size_t i = 0; i < count && i < 64; ++i) {
    if (i) {
      out << ',';
    }
    TextureFetchJson(out, read.instructions[i]);
  }
  out << "]}";
}

void VertexAccessJson(std::ostream& out, const VertexAccess::Result& proof) {
  out << "{\"vertex_input_bounds_verified\":"
      << (proof.vertex_input_bounds_verified ? "true" : "false")
      << ",\"execution_ready\":false,\"ownership_verified\":false,\"error\":\""
      << VertexAccess::ErrorName(proof.error) << "\",\"error_dword_offset\":";
  if (proof.error_dword_offset == UINT32_MAX) {
    out << "null";
  } else {
    out << proof.error_dword_offset;
  }
  out << ",\"error_slot\":";
  if (proof.error_slot == UINT32_MAX) {
    out << "null";
  } else {
    out << proof.error_slot;
  }
  out << ",\"shader_error\":\"" << ShaderAnalysis::ErrorName(proof.shader_error)
      << "\",\"read_error\":\"" << DrawInventory::ErrorName(proof.read_error)
      << "\",\"uses_auto_index_backend_contract\":"
      << (proof.uses_auto_index_backend_contract ? "true" : "false")
      << ",\"copy_rectangle\":" << (proof.copy_rectangle ? "true" : "false")
      << ",\"vertex_count\":" << proof.vertex_count
      << ",\"access_count\":" << proof.accesses.size()
      << ",\"access_details_complete\":"
      << (proof.accesses.size() <= 64 ? "true" : "false") << ",\"accesses\":[";
  for (size_t i = 0; i < proof.accesses.size() && i < 64; ++i) {
    const auto& access = proof.accesses[i];
    out << (i ? "," : "") << "{\"instruction_dword_offset\":";
    if (access.instruction_dword_offset == UINT32_MAX) {
      out << "null";
    } else {
      out << access.instruction_dword_offset;
    }
    out << ",\"slot\":" << access.slot
        << ",\"stride_words\":" << access.stride_words
        << ",\"offset_words\":" << access.offset_words
        << ",\"first_word\":" << access.first_word
        << ",\"exclusive_end_word\":" << access.exclusive_end_word
        << ",\"declared_words\":" << access.declared_words
        << ",\"physical_address\":\""
        << fmt::format("{:08X}", access.physical_address)
        << "\",\"mini\":" << (access.mini ? "true" : "false") << '}';
  }
  out << "]}";
}

void ConstantReadsJson(std::ostream& out,
                       const DrawInventory::ConstantReadResult& reads) {
  const auto stage_name = [](gpu::xenos::ShaderType stage) {
    return stage == gpu::xenos::ShaderType::kVertex ? "vertex" : "pixel";
  };
  out << "{\"used_constant_values_known\":"
      << (reads.used_constant_values_known ? "true" : "false")
      << ",\"execution_ready\":false,\"shader_semantics_verified\":false,"
         "\"error\":\""
      << DrawInventory::ErrorName(reads.error) << "\",\"error_register\":";
  if (reads.error_register == UINT32_MAX) {
    out << "null";
  } else {
    out << reads.error_register;
  }
  out << ",\"error_stage\":\"" << stage_name(reads.error_stage)
      << "\",\"error_constant_index\":";
  if (reads.error_constant_index == UINT32_MAX) {
    out << "null";
  } else {
    out << reads.error_constant_index;
  }
  out << ",\"float_vectors\":[";
  for (size_t i = 0; i < reads.floats.size(); ++i) {
    const auto& value = reads.floats[i];
    out << (i ? "," : "") << "{\"stage\":\"" << stage_name(value.stage)
        << "\",\"index\":" << value.index
        << ",\"known_mask\":" << value.known_mask << ",\"words\":[";
    for (size_t j = 0; j < 4; ++j) {
      if (j) {
        out << ',';
      }
      if (value.known_mask & (1u << j)) {
        out << value.words[j];
      } else {
        out << "null";
      }
    }
    out << "]}";
  }
  out << "],\"bool_words\":[";
  for (size_t i = 0; i < reads.bools.size(); ++i) {
    const auto& value = reads.bools[i];
    out << (i ? "," : "") << "{\"stage\":\"" << stage_name(value.stage)
        << "\",\"word_index\":" << value.word_index
        << ",\"used_mask\":" << value.used_mask << ",\"value\":";
    if (value.known) {
      out << value.value;
    } else {
      out << "null";
    }
    out << '}';
  }
  out << "],\"loop_words\":[";
  for (size_t i = 0; i < reads.loops.size(); ++i) {
    const auto& value = reads.loops[i];
    out << (i ? "," : "") << "{\"stage\":\"" << stage_name(value.stage)
        << "\",\"index\":" << value.index << ",\"value\":";
    if (value.known) {
      out << value.value;
    } else {
      out << "null";
    }
    out << '}';
  }
  out << "]}";
}

bool Capture(Memory* memory, const std::filesystem::path& path,
             std::span<const uint8_t> input) {
  if (!memory || !path.is_absolute() || !path.has_filename() ||
      path.filename() == "." || path.filename() == ".." || input.empty() ||
      input.size() > kMaxPrimaryBytes) {
    XELOGE(
        "Native record capture: invalid memory, destination or primary size");
    return false;
  }
  auto sidecar = path;
  sidecar += ".capture";
  std::error_code error;
  const auto previous = std::filesystem::symlink_status(sidecar, error);
  if ((error && error != std::errc::no_such_file_or_directory) ||
      std::filesystem::exists(previous)) {
    XELOGE("Native record capture: sidecar destination is not new");
    return false;
  }
  // Own the primary bytes independently of the producer throughout host I/O.
  const std::vector<uint8_t> primary(input.begin(), input.end());
  if (!WriteExclusive(path, primary).complete) {
    XELOGE("Native record capture: exclusive primary write failed");
    return false;
  }
  error.clear();
  if (!std::filesystem::create_directory(sidecar, error) || error) {
    XELOGE("Native record capture: exclusive sidecar creation failed");
    return false;
  }
#if !XE_PLATFORM_WIN32
  std::filesystem::permissions(sidecar, std::filesystem::perms::owner_all,
                               std::filesystem::perm_options::replace, error);
  if (error) {
    XELOGE("Native record capture: private sidecar permissions failed");
    return false;
  }
#endif
  const auto packet_plan = gpu::NativeFrameCommandPlan::Analyze(primary);
  const auto primary_hash = Sha256(primary);
  bool host_success = bool(primary_hash);
  const auto analysis = Stream::Analyze(primary);
  std::optional<ShaderScan::Result> shader_scan;
  std::vector<Stream::MemoryDependency> diagnostic_reads;
  bool use_diagnostic_scan = false;
  if (!analysis) {
    shader_scan = ShaderScan::Scan(primary);
    use_diagnostic_scan = shader_scan->complete_primary_scan &&
                          shader_scan->error == ShaderScan::Error::kNone;
    if (use_diagnostic_scan) {
      for (const auto& candidate : shader_scan->candidates) {
        diagnostic_reads.push_back(
            {Stream::MemoryDependency::Kind::kShaderCode,
             candidate.packet_dword_offset, candidate.raw_address,
             candidate.physical_address, candidate.byte_count,
             candidate.shader_load_index});
      }
    }
  }
  std::vector<Snapshot> snapshots;
  size_t payload_bytes = primary.size();
  bool all_declared_reads = bool(analysis);
  bool all_diagnostic_candidates = use_diagnostic_scan;
  // One checked copy path keeps the shared physical-range and output budgets
  // identical for shader, vertex and texture diagnostics. The heap lock
  // prevents unmapping during the copy; it is not an allocation lease or write
  // barrier.
  const auto capture_read = [&](Snapshot snapshot) {
    const auto& read = snapshot.dependency;
    if (!read.byte_count || read.physical_address >= kPhysicalSize ||
        read.byte_count > kPhysicalSize - read.physical_address) {
      snapshot.status = "invalid_physical_range";
    } else if (read.byte_count > kMaxDependencyBytes ||
               read.byte_count > kMaxPayloadBytes - payload_bytes) {
      snapshot.status = "capture_budget_exceeded";
    } else {
      std::vector<uint8_t> bytes(read.byte_count);
      bool readable = false;
      {
        auto lock = global_critical_region::Acquire();
        auto* physical = memory->GetPhysicalHeap();
        // Addresses are already GPU physical extents. Never apply a CPU
        // virtual-alias conversion or normalize them again.
        readable = physical && physical->QueryRangeAccess(
                                   read.physical_address,
                                   read.physical_address + read.byte_count -
                                       1) != xe::memory::PageAccess::kNoAccess;
        if (readable) {
          std::memcpy(bytes.data(),
                      memory->TranslatePhysical(read.physical_address),
                      bytes.size());
        }
      }
      if (!readable) {
        snapshot.status = "unreadable_or_uncommitted";
      } else {
        const auto filename =
            fmt::format("dependency-{:04}.bin", snapshots.size());
        const auto write = WriteExclusive(sidecar / filename, bytes);
        if (write.created) {
          snapshot.file = filename;
        }
        snapshot.written = write.written;
        payload_bytes += write.written;
        if (!write.complete) {
          snapshot.status = "host_write_failed";
          host_success = false;
        } else {
          snapshot.hash = Sha256(bytes);
          snapshot.status = snapshot.hash ? "captured" : "hash_failed";
          host_success &= bool(snapshot.hash);
          if (snapshot.hash && read.kind == CaptureRead::Kind::kShaderCode) {
            snapshot.shader_bytes = std::move(bytes);
          }
        }
      }
    }
    const auto index = snapshots.size();
    snapshots.push_back(std::move(snapshot));
    return index;
  };
  {
    const auto& reads = analysis              ? analysis.stream->dependencies()
                        : use_diagnostic_scan ? diagnostic_reads
                                              : analysis.prefix_shader_reads;
    for (const auto& dependency : reads) {
      if (snapshots.size() >= 512) {
        all_declared_reads = false;
        all_diagnostic_candidates = false;
        break;
      }
      Snapshot snapshot{};
      snapshot.dependency = CaptureRead::FromStrict(dependency);
      if (use_diagnostic_scan) {
        snapshot.diagnostic_stage =
            shader_scan->candidates[snapshots.size()].stage;
      }
      const auto index = capture_read(std::move(snapshot));
      const bool captured =
          std::string_view(snapshots[index].status) == "captured";
      all_declared_reads &= captured;
      all_diagnostic_candidates &= captured;
    }
  }

  // This inventory is independent of strict parser acceptance. Only immutable
  // copied shader bytes enter the bounded CPU preflight/analysis; no arbitrary
  // guest code is executed or translated and missing state is never
  // zero-filled.
  const auto draw_inventory = DrawInventory::Scan(primary);
  std::vector<std::optional<ShaderAnalysis::Result>> shader_metadata(
      draw_inventory.shaders.size());
  // Keep the exact selected-load bytes stable independently of later snapshot
  // vector growth. Access analysis reparses these owned bytes, not metadata
  // supplied for a different program or a live guest pointer.
  std::vector<std::vector<uint8_t>> shader_code(draw_inventory.shaders.size());
  for (size_t index = 0; index < draw_inventory.shaders.size(); ++index) {
    const auto& shader = draw_inventory.shaders[index];
    std::span<const uint8_t> code;
    if (shader.is_inline) {
      const size_t offset = size_t(shader.inline_dword_offset) * 4;
      const size_t size = size_t(shader.dword_count) * 4;
      if (offset <= primary.size() && size <= primary.size() - offset) {
        code = std::span<const uint8_t>(primary).subspan(offset, size);
      }
    } else {
      for (const auto& snapshot : snapshots) {
        const auto& read = snapshot.dependency;
        if (read.kind == CaptureRead::Kind::kShaderCode &&
            read.binding_index == index &&
            read.packet_dword_offset == shader.packet_dword_offset &&
            read.raw_address == shader.raw_address &&
            read.physical_address == shader.physical_address &&
            snapshot.shader_bytes.size() == size_t(shader.dword_count) * 4) {
          code = snapshot.shader_bytes;
          break;
        }
      }
    }
    if (!code.empty()) {
      shader_code[index].assign(code.begin(), code.end());
      shader_metadata[index] =
          ShaderAnalysis::Analyze(shader.stage, shader_code[index]);
    }
  }
  struct DrawReads {
    DrawInventory::ReadResult inventory;
    DrawInventory::TextureReadResult textures;
    DrawInventory::ConstantReadResult constants;
    VertexAccess::Result vertex_access;
    std::vector<size_t> snapshot_indices;
    std::vector<size_t> texture_snapshot_indices;
    bool dependency_count_exceeded = false;
    bool texture_dependency_count_exceeded = false;
  };
  std::vector<DrawReads> draw_reads;
  for (const auto& draw : draw_inventory.draws) {
    const ShaderAnalysis::Result* vertex = nullptr;
    if (draw.vertex_shader && *draw.vertex_shader < shader_metadata.size() &&
        shader_metadata[*draw.vertex_shader]) {
      vertex = &*shader_metadata[*draw.vertex_shader];
    }
    const ShaderAnalysis::Result* pixel = nullptr;
    if (draw.pixel_shader && *draw.pixel_shader < shader_metadata.size() &&
        shader_metadata[*draw.pixel_shader]) {
      pixel = &*shader_metadata[*draw.pixel_shader];
    }
    DrawReads captured{};
    captured.inventory = DrawInventory::VertexReads(draw, vertex);
    captured.textures = DrawInventory::TextureReads(draw, pixel);
    captured.constants = DrawInventory::ConstantReads(draw, vertex, pixel);
    const std::span<const uint8_t> vertex_code =
        draw.vertex_shader && *draw.vertex_shader < shader_code.size()
            ? std::span<const uint8_t>(shader_code[*draw.vertex_shader])
            : std::span<const uint8_t>{};
    captured.vertex_access = VertexAccess::Analyze(draw, vertex_code);
    for (const auto& read : captured.inventory.reads) {
      // Preserve the strict profile's 512 external-read bound for the whole
      // artifact, not 512 more reads per diagnostic draw. Existing byte budgets
      // include both shader and vertex snapshots.
      if (snapshots.size() >= 512) {
        captured.dependency_count_exceeded = true;
        break;
      }
      Snapshot snapshot{};
      snapshot.dependency = {CaptureRead::Kind::kDeclaredVertexBuffer,
                             draw.packet_dword_offset,
                             read.raw_address,
                             read.physical_address,
                             read.byte_count,
                             read.slot};
      snapshot.diagnostic_vertex_read = true;
      snapshot.resolve_rectangle = read.resolve_rectangle;
      captured.snapshot_indices.push_back(capture_read(std::move(snapshot)));
    }
    for (const auto& read : captured.textures.reads) {
      if (snapshots.size() >= 512) {
        captured.texture_dependency_count_exceeded = true;
        break;
      }
      Snapshot snapshot{};
      snapshot.dependency = {CaptureRead::Kind::kTextureStorage,
                             draw.packet_dword_offset,
                             read.raw_address,
                             read.physical_address,
                             read.byte_count,
                             read.slot};
      snapshot.diagnostic_texture_read = true;
      captured.texture_snapshot_indices.push_back(
          capture_read(std::move(snapshot)));
    }
    draw_reads.push_back(std::move(captured));
  }

  // Values interpolated below are generated ASCII names, enum strings,
  // hexadecimal digests/addresses and integers. No host path or guest text is
  // placed in the JSON, so arbitrary filenames cannot break its encoding.
  std::ostringstream json;
  json << "{\n  \"schema\": 1,\n  \"execution_ready\": false,\n"
          "  \"shader_semantics_verified\": false,\n"
          "  \"owned_memory_leases_verified\": false,\n"
          "  \"gpu_execution_requested\": false,\n"
          "  \"primary\": {\"location\": \"sibling_raw_file\", \"bytes\": "
       << primary.size() << ", \"sha256\": ";
  DigestJson(json, primary_hash);
  json << "},\n  \"parser\": {\"accepted\": " << (analysis ? "true" : "false")
       << ", \"error\": \"" << ErrorName(analysis.error)
       << "\", \"error_dword_offset\": " << analysis.error_dword_offset
       << ", \"missing_state_register\": " << analysis.missing_state_register
       << "},\n"
       << "  \"complete_packet_profile\": {\"accepted\": "
       << (packet_plan ? "true" : "false") << ", \"error\": \""
       << gpu::NativeFrameCommandPlan::ErrorName(packet_plan.error)
       << "\", \"error_dword_offset\": " << packet_plan.error_dword_offset
       << ", \"error_register\": " << packet_plan.error_register
       << ", \"packet_count\": "
       << (packet_plan ? packet_plan.plan->packets().size() : 0)
       << ", \"scope\": \"bounded_CPU_packets_only\", \"execution_ready\": "
          "false},\n"
       << "  \"declared_read_inventory_complete\": "
       << (analysis ? "true" : "false") << ",\n"
       << "  \"read_inventory_scope\": \""
       << (analysis              ? "accepted_profile_declarations"
           : use_diagnostic_scan ? "diagnostic_primary_shader_candidates_only"
                                 : "validated_prefix_shader_reads_only")
       << "\",\n"
       << "  \"diagnostic_shader_candidates_snapshotted\": "
       << (all_diagnostic_candidates ? "true" : "false") << ",\n"
       << "  \"all_declared_reads_snapshotted\": "
       << (all_declared_reads ? "true" : "false") << ",\n"
       << "  \"host_artifacts_complete\": " << (host_success ? "true" : "false")
       << ",\n  \"diagnostic_shader_scan\": ";
  if (shader_scan) {
    json << "{\"complete_primary_scan\": "
         << (shader_scan->complete_primary_scan ? "true" : "false")
         << ", \"error\": \"" << ShaderScan::ErrorName(shader_scan->error)
         << "\", \"error_dword_offset\": " << shader_scan->error_dword_offset
         << ", \"packet_count\": " << shader_scan->packet_count
         << ", \"shader_load_count\": " << shader_scan->shader_load_count
         << ", \"external_candidates\": " << shader_scan->candidates.size()
         << ", \"uninterpreted_type3_packets\": "
         << shader_scan->uninterpreted_type3_packets
         << ", \"indirect_buffer_packets\": "
         << shader_scan->indirect_buffer_packets
         << ", \"read_inventory_complete\": false, \"execution_ready\": false, "
            "\"scope\": \"linear_primary_only_no_control_flow_or_indirect_"
            "buffer_execution\"}";
  } else {
    json << "null";
  }
  json << ",\n  \"limits\": {\"primary_bytes\": " << kMaxPrimaryBytes
       << ", \"per_dependency_bytes\": " << kMaxDependencyBytes
       << ", \"payload_bytes\": " << kMaxPayloadBytes
       << ", \"manifest_bytes\": " << kMaxManifestBytes << "},\n"
       << "  \"payload_bytes_written\": " << payload_bytes
       << ",\n  \"dependencies\": [";
  for (size_t index = 0; index < snapshots.size(); ++index) {
    const auto& snapshot = snapshots[index];
    const auto& dependency = snapshot.dependency;
    json << (index ? ",\n" : "\n") << "    {\"index\": " << index
         << ", \"kind\": \""
         << (snapshot.diagnostic_texture_read
                 ? "diagnostic_used_texture_storage"
             : snapshot.diagnostic_vertex_read ? "diagnostic_used_vertex_span"
             : dependency.kind == CaptureRead::Kind::kShaderCode
                 ? "shader_code"
                 : "declared_vertex_buffer")
         << "\", \"packet_dword_offset\": " << dependency.packet_dword_offset
         << ", \"binding_index\": " << dependency.binding_index
         << ", \"raw_address\": \""
         << fmt::format("{:08X}", dependency.raw_address)
         << "\", \"physical_address\": \""
         << fmt::format("{:08X}", dependency.physical_address)
         << "\", \"bytes\": " << dependency.byte_count << ", \"status\": \""
         << snapshot.status << "\", \"file\": ";
    if (snapshot.file.empty()) {
      json << "null";
    } else {
      json << '"' << snapshot.file << '"';
    }
    json << ", \"written_bytes\": " << snapshot.written << ", \"sha256\": ";
    DigestJson(json, snapshot.hash);
    if (snapshot.diagnostic_vertex_read) {
      json << ", \"diagnostic_only\": true, \"resolve_rectangle\": "
           << (snapshot.resolve_rectangle ? "true" : "false");
    }
    if (snapshot.diagnostic_texture_read) {
      json << ", \"diagnostic_only\": true, \"texture_storage_extent\": true";
    }
    if (snapshot.diagnostic_stage) {
      json << ", \"diagnostic_only\": true, \"shader_stage\": \""
           << (*snapshot.diagnostic_stage == ShaderScan::Stage::kVertex
                   ? "vertex"
                   : "pixel")
           << '"';
    }
    json << '}';
  }
  json << "\n  ],\n  \"shaders\": [";
  if (analysis) {
    const auto& shaders = analysis.stream->shaders();
    for (size_t index = 0; index < shaders.size(); ++index) {
      const auto& shader = shaders[index];
      json << (index ? ",\n" : "\n") << "    {\"index\": " << index
           << ", \"stage\": \""
           << (shader.stage == Stream::ShaderStage::kVertex ? "vertex"
                                                            : "pixel")
           << "\", \"packet_dword_offset\": " << shader.packet_dword_offset
           << ", \"dwords\": " << shader.dword_count
           << ", \"inline\": " << (shader.is_inline ? "true" : "false")
           << ", \"inline_dword_offset\": " << shader.inline_dword_offset
           << '}';
    }
  }
  json << "\n  ],\n";
  json
      << "  \"diagnostic_draw_reads\": {\"complete_primary_scan\": "
      << (draw_inventory.complete_primary_scan ? "true" : "false")
      << ", \"error\": \"" << DrawInventory::ErrorName(draw_inventory.error)
      << "\", \"error_dword_offset\": " << draw_inventory.error_dword_offset
      << ", \"state_invalidations\": " << draw_inventory.state_invalidations
      << ", \"execution_ready\": false, \"access_bounds_verified\": false, "
         "\"ownership_complete\": false, \"shader_semantics_verified\": false, "
         "\"scope\": \"potential_used_vertex_and_base_texture_reads\", "
         "\"shaders\": [";
  for (size_t index = 0; index < draw_inventory.shaders.size(); ++index) {
    const auto& shader = draw_inventory.shaders[index];
    const auto& metadata = shader_metadata[index];
    const bool analyzed = metadata && bool(*metadata);
    json << (index ? ",\n" : "\n") << "    {\"index\": " << index
         << ", \"stage\": \""
         << (shader.stage == gpu::xenos::ShaderType::kVertex ? "vertex"
                                                             : "pixel")
         << "\", \"packet_dword_offset\": " << shader.packet_dword_offset
         << ", \"dwords\": " << shader.dword_count
         << ", \"inline\": " << (shader.is_inline ? "true" : "false")
         << ", \"metadata_analyzed\": " << (analyzed ? "true" : "false")
         << ", \"error\": \""
         << (metadata ? ShaderAnalysis::ErrorName(metadata->error)
                      : "shader_snapshot_missing")
         << "\", \"error_dword_offset\": ";
    if (metadata) {
      json << metadata->error_dword_offset;
    } else {
      json << "null";
    }
    json << ", \"vertex_fetch_bitmap\": ";
    if (analyzed) {
      json << '[' << metadata->vertex_fetch_bitmap[0] << ','
           << metadata->vertex_fetch_bitmap[1] << ','
           << metadata->vertex_fetch_bitmap[2] << ']';
    } else {
      json << "null";
    }
    const size_t fetch_count = analyzed ? metadata->vertex_fetches.size() : 0;
    json << ", \"vertex_fetch_count\": " << fetch_count
         << ", \"fetch_details_complete\": "
         << (fetch_count <= 64 ? "true" : "false") << ", \"vertex_fetches\": [";
    for (size_t i = 0; i < fetch_count && i < 64; ++i) {
      const auto& fetch = metadata->vertex_fetches[i];
      json << (i ? "," : "")
           << "{\"instruction_dword_offset\":" << fetch.instruction_dword_offset
           << ",\"slot\":" << fetch.slot
           << ",\"stride_words\":" << fetch.stride_words
           << ",\"offset_words\":" << fetch.offset_words
           << ",\"format\":" << fetch.format
           << ",\"needed_word_mask\":" << fetch.needed_word_mask
           << ",\"index_register\":" << fetch.index_register
           << ",\"index_component\":" << fetch.index_component
           << ",\"index_relative\":"
           << (fetch.index_relative ? "true" : "false")
           << ",\"index_rounded\":" << (fetch.index_rounded ? "true" : "false")
           << ",\"mini\":" << (fetch.mini ? "true" : "false") << '}';
    }
    json << "],\"texture_fetch_bitmap\":";
    if (analyzed) {
      json << metadata->texture_fetch_bitmap;
    } else {
      json << "null";
    }
    const size_t texture_count =
        analyzed ? metadata->texture_fetches.size() : 0;
    json << ",\"texture_fetch_count\":" << texture_count
         << ",\"texture_fetch_details_complete\":"
         << (texture_count <= 64 ? "true" : "false")
         << ",\"texture_fetches\":[";
    for (size_t i = 0; i < texture_count && i < 64; ++i) {
      if (i) {
        json << ',';
      }
      TextureFetchJson(json, metadata->texture_fetches[i]);
    }
    json << "],\"constant_use\":";
    if (!analyzed) {
      json << "null";
    } else {
      const auto& use = metadata->constant_use;
      json << "{\"float_bitmap_hex\":[";
      for (size_t i = 0; i < use.float_bitmap.size(); ++i) {
        json << (i ? "," : "") << '"'
             << fmt::format("{:016X}", use.float_bitmap[i]) << '"';
      }
      json << "],\"float_count\":" << use.float_count
           << ",\"float_dynamic_addressing\":"
           << (use.float_dynamic_addressing ? "true" : "false")
           << ",\"bool_bitmap\":[";
      for (size_t i = 0; i < use.bool_bitmap.size(); ++i) {
        json << (i ? "," : "") << use.bool_bitmap[i];
      }
      json << "],\"loop_bitmap\":" << use.loop_bitmap << '}';
    }
    json << '}';
  }
  json << "\n  ], \"draws\": [";
  for (size_t index = 0; index < draw_inventory.draws.size(); ++index) {
    const auto& draw = draw_inventory.draws[index];
    const auto& reads = draw_reads[index];
    bool captured_all =
        reads.inventory.error == DrawInventory::Error::kNone &&
        !reads.dependency_count_exceeded &&
        reads.snapshot_indices.size() == reads.inventory.reads.size();
    for (const auto snapshot : reads.snapshot_indices) {
      captured_all &=
          std::string_view(snapshots[snapshot].status) == "captured";
    }
    bool textures_captured =
        reads.textures.error == DrawInventory::Error::kNone &&
        !reads.texture_dependency_count_exceeded &&
        reads.texture_snapshot_indices.size() == reads.textures.reads.size();
    for (const auto snapshot : reads.texture_snapshot_indices) {
      textures_captured &=
          std::string_view(snapshots[snapshot].status) == "captured";
    }
    json << (index ? ",\n" : "\n") << "    {\"index\":" << index
         << ",\"packet_dword_offset\":" << draw.packet_dword_offset
         << ",\"opcode\":" << draw.opcode << ",\"initiator\":" << draw.initiator
         << ",\"viz_token\":" << draw.viz_token << ",\"vertex_shader\":";
    if (draw.vertex_shader) {
      json << *draw.vertex_shader;
    } else {
      json << "null";
    }
    json << ",\"pixel_shader\":";
    if (draw.pixel_shader) {
      json << *draw.pixel_shader;
    } else {
      json << "null";
    }
    json << ",\"geometry_error\":\"" << DrawInventory::ErrorName(draw.error)
         << "\",\"vertex_read_error\":\""
         << DrawInventory::ErrorName(reads.inventory.error)
         << "\",\"error_slot\":";
    if (reads.inventory.error_slot == UINT32_MAX) {
      json << "null";
    } else {
      json << reads.inventory.error_slot;
    }
    json << ",\"vertex_spans_snapshotted\":"
         << (captured_all ? "true" : "false")
         << ",\"dependency_count_exceeded\":"
         << (reads.dependency_count_exceeded ? "true" : "false")
         << ",\"vertex_input_bounds_proof\":";
    VertexAccessJson(json, reads.vertex_access);
    json << ",\"used_constants\":";
    ConstantReadsJson(json, reads.constants);
    json << ",\"constant_selectors\":[";
    for (size_t i = 0; i < draw.constants.selectors.size(); ++i) {
      if (i) {
        json << ',';
      }
      if (draw.constants.selectors_known[i]) {
        json << draw.constants.selectors[i];
      } else {
        json << "null";
      }
    }
    json << "],\"execution_ready\":false,\"access_bounds_verified\":false,"
            "\"state\":[";
    for (size_t i = 0; i < DrawInventory::kStateRegisters.size(); ++i) {
      json << (i ? "," : "")
           << "{\"register\":" << DrawInventory::kStateRegisters[i]
           << ",\"value\":";
      if (draw.state_known[i]) {
        json << draw.state_words[i];
      } else {
        json << "null";
      }
      json << '}';
    }
    json << "],\"fetch_words\":[";
    for (size_t i = 0; i < draw.fetch_words.size(); ++i) {
      if (i) {
        json << ',';
      }
      if (draw.fetch_known[i]) {
        json << draw.fetch_words[i];
      } else {
        json << "null";
      }
    }
    json << "],\"snapshot_indices\":[";
    for (size_t i = 0; i < reads.snapshot_indices.size(); ++i) {
      json << (i ? "," : "") << reads.snapshot_indices[i];
    }
    json << "],\"texture_read_error\":\""
         << DrawInventory::ErrorName(reads.textures.error)
         << "\",\"texture_error_slot\":";
    if (reads.textures.error_slot == UINT32_MAX) {
      json << "null";
    } else {
      json << reads.textures.error_slot;
    }
    json << ",\"texture_spans_snapshotted\":"
         << (textures_captured ? "true" : "false")
         << ",\"texture_dependency_count_exceeded\":"
         << (reads.texture_dependency_count_exceeded ? "true" : "false")
         << ",\"texture_reads\":[";
    for (size_t i = 0; i < reads.textures.reads.size(); ++i) {
      if (i) {
        json << ',';
      }
      TextureReadJson(json, reads.textures.reads[i]);
    }
    json << "],\"texture_snapshot_indices\":[";
    for (size_t i = 0; i < reads.texture_snapshot_indices.size(); ++i) {
      json << (i ? "," : "") << reads.texture_snapshot_indices[i];
    }
    json << "]}";
  }
  json << "\n  ]}";
  json << "\n}\n";
  const std::string manifest = json.str();
  if (manifest.size() > kMaxManifestBytes ||
      !WriteExclusive(
           sidecar / "manifest.json",
           {reinterpret_cast<const uint8_t*>(manifest.data()), manifest.size()})
           .complete) {
    XELOGE("Native record capture: manifest write failed");
    return false;
  }
  XELOGI(
      "Native record capture: primary_bytes={} parser_accepted={} "
      "dependencies={} declared_reads_captured={} diagnostic_scan={} "
      "diagnostic_candidates_captured={} host_complete={}; "
      "no execution permission",
      primary.size(), bool(analysis), snapshots.size(), all_declared_reads,
      use_diagnostic_scan, all_diagnostic_candidates, host_success);
  return host_success;
}

}  // namespace

bool CaptureNativeRecord(Memory* memory, const std::filesystem::path& path,
                         std::span<const uint8_t> bytes) {
  try {
    return Capture(memory, path, bytes);
  } catch (const std::exception&) {
    XELOGE(
        "Native record capture: host capture interrupted; preserve artifacts");
    return false;
  }
}

}  // namespace xe::kernel
