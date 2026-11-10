/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia contributors. All rights reserved.                    *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */
#include "xenia/kernel/native_frame_lease_probe.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <exception>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "xenia/base/cvar.h"
#include "xenia/base/filesystem.h"
#include "xenia/base/logging.h"
#include "xenia/base/mutex.h"
#include "xenia/gpu/native_draw_read_inventory.h"
#include "xenia/gpu/native_frame_command_plan.h"
#include "xenia/gpu/native_frame_execution.h"
#include "xenia/gpu/native_shader_read_analysis.h"
#include "xenia/gpu/native_vertex_access_analysis.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/native_frame_input_capture.h"
#include "xenia/kernel/native_frame_owner.h"
#include "xenia/kernel/native_record_capture.h"
#include "xenia/kernel/native_system_allocation_authority.h"
#include "xenia/kernel/xthread.h"
#include "xenia/memory.h"

#if !XE_PLATFORM_WIN32
#include <sys/stat.h>
#include <unistd.h>
#endif

DECLARE_bool(native_system_ui_trace);

namespace xe::kernel {
// The only production caller of Session's private mint. CheckFrame invokes it
// after the existing common packet/shader/read/bounds/pin proofs, never from a
// caller-supplied collection of success booleans.
class NativeFrameLeaseBuilder {
 public:
  static std::unique_ptr<NativeFrameInputCapture> BeginCapture(
      Memory* memory, const std::shared_ptr<NativeFrameOwnerSession>& session,
      const NativeFrameOwnerSession::ThreadIdentity& thread,
      const std::shared_ptr<const NativeFrameSource>& source,
      const std::shared_ptr<const gpu::NativeFrameCommandPlan>& plan,
      memory::PhysicalRange primary, memory::PhysicalRange output) {
    return session->BeginInputCapture(memory, thread, source, plan, primary,
                                      output);
  }

  static bool MatchesCompletedRecord(
      const std::shared_ptr<NativeFrameOwnerSession>& session,
      const NativeFrameOwnerSession::ThreadIdentity& thread,
      const std::shared_ptr<const NativeFrameSource>& source, uint32_t device,
      uint32_t dc) {
    return session->MatchesCompletedRecord(thread, source, device, dc);
  }
  static bool HoldPreparationPins(
      const std::shared_ptr<NativeFrameOwnerSession>& session,
      const NativeFrameOwnerSession::ThreadIdentity& thread,
      const std::shared_ptr<const NativeFrameSource>& source,
      std::shared_ptr<void> pins) {
    return session->HoldPreparationPins(thread, source, std::move(pins));
  }
  static std::shared_ptr<const gpu::NativeFrameExecutionRequest> Mint(
      Memory* guest_memory, const NativeCompletedFrameRecord& record,
      const std::shared_ptr<NativeFrameOwnerSession>& session,
      const NativeFrameOwnerSession::ThreadIdentity& thread,
      const std::shared_ptr<const NativeFrameSource>& source,
      std::shared_ptr<const gpu::NativeFrameCommandPlan> plan,
      memory::PhysicalRange output, std::vector<memory::PhysicalRange> inputs,
      std::vector<gpu::NativeFrameExecutionRequest::Shader> shaders,
      const std::array<uint8_t, 148>* descriptor = nullptr,
      std::shared_ptr<const gpu::NativeFrameInputSnapshot> snapshot = {},
      const std::shared_ptr<const NativeNonmodalOutputProvenance>&
          output_provenance = {},
      const std::optional<gpu::NativeOverlayState>& overlay_input_state = {},
      const std::shared_ptr<const NativeNonmodalControlProvenance>&
          control_provenance = {}) {
    if (plan->is_overlay_control()) {
      if (!descriptor || output_provenance || output.address || output.length ||
          !session->ValidateControlFrame(guest_memory, thread, source, plan,
                                         record, *descriptor,
                                         control_provenance)) {
        return {};
      }
      return session->MintExecutionRequest(
          thread, source, std::move(plan), {}, std::move(inputs),
          std::move(shaders), {}, std::move(snapshot));
    }
    if (control_provenance) {
      return {};
    }
    const auto layout =
        descriptor ? session->SnapshotPm4OutputLayout(
                         guest_memory, thread, source, plan, inputs, record,
                         *descriptor, snapshot, output_provenance,
                         overlay_input_state)
                   : session->SnapshotOutputLayout(guest_memory, thread, source,
                                                   plan, inputs, record);
    // Diagnostic origin retains genuine post-End XG provenance. Production
    // origin derives storage only from real descriptor/PM4/pinned geometry.
    // Host-only models may still use Session's private mint with no layout.
    if (!layout || output.address != record.output_physical_address ||
        output.length != record.output_allocation_bytes) {
      return {};
    }
    return session->MintExecutionRequest(
        thread, source, std::move(plan), output, std::move(inputs),
        std::move(shaders), layout, std::move(snapshot));
  }
};
namespace {
using Inventory = gpu::NativeDrawReadInventory;
using Shader = gpu::NativeShaderReadAnalysis;
using Vertex = gpu::NativeVertexAccessAnalysis;
using Range = memory::PhysicalRange;
using Allocation = memory::PhysicalAllocationIdentity;
using Status = memory::PhysicalLeaseStatus;
using Session = NativeFrameOwnerSession;
constexpr uint32_t kPhysicalSize = 0x20000000;
constexpr uint32_t kMaxRangeBytes = 4 * 1024 * 1024;
constexpr uint32_t kMaxInputBytes = 15 * 1024 * 1024;
constexpr size_t kMaxRanges = 512;
constexpr size_t kMaxReportBytes = 256 * 1024;

bool Valid(Range range) {
  return range.length && range.address < kPhysicalSize &&
         range.length <= kMaxRangeBytes &&
         range.length <= kPhysicalSize - range.address;
}
bool Overlaps(Range a, Range b) {
  return uint64_t(a.address) < uint64_t(b.address) + b.length &&
         uint64_t(b.address) < uint64_t(a.address) + a.length;
}
bool Contains(const Allocation& allocation, Range range) {
  return range.address >= allocation.physical_address &&
         uint64_t(range.address) + range.length <=
             uint64_t(allocation.physical_address) + allocation.length;
}
bool SameAllocation(const Allocation& a, const Allocation& b) {
  return a.allocation_id == b.allocation_id && a.owner == b.owner &&
         a.physical_address == b.physical_address && a.length == b.length &&
         a.virtual_address == b.virtual_address;
}

// Also retained directly by KernelState before acquiring any backing token.
// Unexpected unwind must not make an unattached local token disappear. Normal
// checked Abort clears this holder after the session consumes its own pins.
struct Pins {
  std::shared_ptr<const NativeSystemCompletedFrameRecord> native_record;
  std::vector<std::shared_ptr<const memory::PhysicalRangePin>> tokens;
};
struct RequestedRange {
  const char* kind;
  uint32_t index;
  uint32_t slot;
  Range range;
};
struct DrawProof {
  uint32_t packet;
  bool inputs = false, vertex_bounds = false, constants = false;
  uint32_t vertex_ranges = 0, texture_ranges = 0;
  uint32_t floats = 0, bools = 0, loops = 0;
  const char* error = "none";
  const char* vertex_error = "none";
  const char* texture_error = "none";
  const char* bounds_error = "none";
  const char* constant_error = "none";
  uint32_t constant_error_register = UINT32_MAX;
};
struct Report {
  const char* error = "none";
  Session::Owner owner{};
  uint64_t source = 0;
  uint32_t device = 0, dc = 0;
  bool frozen = false, frozen_update_denied = false;
  bool control_only = false;
  bool output_writable = false, primary_matches = false;
  bool release_refused = false, parent_unchanged = false;
  bool captured = false, abort_attempted = false, aborted = false;
  bool bound_requires_new_update = false, coverage = false;
  bool input_pins_complete = false;
  bool execution_request_prepared = false;
  bool execution_request_admitted = false;
  bool execution_request_canceled = false;
  bool prepared_handoff = false;
  bool packet_profile_checked = false;
  const char* packet_profile_error = "none";
  uint32_t packet_profile_error_offset = UINT32_MAX;
  uint32_t packet_profile_error_register = UINT32_MAX;
  uint32_t packet_count = 0;
  uint32_t shaders = 0, analyzed_shaders = 0;
  const char* pin_phase = "none";
  Status pin_status = Status::kSuccess;
  uint32_t pin_failed_range = UINT32_MAX;
  Range failed_range{};
  uint32_t failed_shader = UINT32_MAX, shader_error_offset = UINT32_MAX;
  const char* shader_error = "none";
  std::vector<RequestedRange> ranges;
  std::map<uint64_t, Allocation> allocations;
  std::vector<DrawProof> draws;
};

// Failure-only, optional host metadata. All strings below are fixed phase or
// existing diagnostic enum/error names, never guest text or exception.what().
// No guest memory, Session authority or native execution is consulted here.
void TraceSystemPreparationFailure(const Report& r, const char* phase,
                                   const char* reason,
                                   bool source_metadata_known) noexcept {
  if (!cvars::native_system_ui_trace) {
    return;
  }
  static std::atomic<uint32_t> emitted{0};
  auto index = emitted.load(std::memory_order_relaxed);
  while (index < 16 && !emitted.compare_exchange_weak(
                           index, index + 1, std::memory_order_relaxed)) {
  }
  if (index >= 16) {
    return;
  }
  try {
    std::ostringstream out;
    out << std::boolalpha
        << "{\"schema\":1,\"component\":\"transport\","
           "\"event\":\"source_preparation_failed\",\"failure_index\":"
        << index + 1 << ",\"failure_limit\":16,\"phase\":\"" << phase
        << "\",\"reason\":\"" << reason << "\",\"report_error\":\"" << r.error
        << "\",\"source_metadata_known\":" << source_metadata_known
        << ",\"report_fields_partial\":true";
    if (source_metadata_known) {
      out << ",\"kernel_epoch\":" << r.owner.kernel_epoch
          << ",\"runtime_module_generation\":" << r.owner.module_generation
          << ",\"owner_session_id\":" << r.owner.session_id
          << ",\"owner_hmodule\":" << r.owner.module_hmodule
          << ",\"source_serial\":" << r.source << ",\"device\":" << r.device
          << ",\"dc\":" << r.dc;
    }
    out << ",\"frozen_update_denied\":" << r.frozen_update_denied
        << ",\"control_only\":" << r.control_only
        << ",\"output_writable\":" << r.output_writable
        << ",\"primary_matches\":" << r.primary_matches
        << ",\"packet_profile_observed\":"
        << (r.packet_profile_checked ||
            std::strcmp(r.packet_profile_error, "none") != 0)
        << ",\"packet_profile_checked\":" << r.packet_profile_checked
        << ",\"packet_profile_error\":\"" << r.packet_profile_error
        << "\",\"packet_error_dword_offset\":" << r.packet_profile_error_offset
        << ",\"packet_error_register\":" << r.packet_profile_error_register
        << ",\"packet_count\":" << r.packet_count
        << ",\"shader_loads\":" << r.shaders
        << ",\"analyzed_shader_loads\":" << r.analyzed_shaders
        << ",\"shader_failure_known\":" << (r.failed_shader != UINT32_MAX)
        << ",\"failed_shader_load\":" << r.failed_shader
        << ",\"shader_error\":\"" << r.shader_error
        << "\",\"shader_error_dword_offset\":" << r.shader_error_offset
        << ",\"pin_phase\":\"" << r.pin_phase
        << "\",\"pin_observed\":" << (std::strcmp(r.pin_phase, "none") != 0);
    if (std::strcmp(r.pin_phase, "none") != 0) {
      out << ",\"pin_status\":" << uint32_t(r.pin_status)
          << ",\"pin_failed_range\":" << r.pin_failed_range
          << ",\"failed_physical_address\":" << r.failed_range.address
          << ",\"failed_length\":" << r.failed_range.length;
    }
    out << ",\"requested_ranges\":" << r.ranges.size()
        << ",\"observed_allocations\":" << r.allocations.size()
        << ",\"draws_checked\":" << r.draws.size()
        << ",\"all_draw_proofs\":" << r.coverage
        << ",\"input_pins_complete\":" << r.input_pins_complete
        << ",\"parent_unchanged\":" << r.parent_unchanged
        << ",\"execution_request_prepared\":" << r.execution_request_prepared
        << ",\"execution_request_admitted\":" << r.execution_request_admitted
        << ",\"last_draw_known\":" << !r.draws.empty();
    if (!r.draws.empty()) {
      const auto& d = r.draws.back();
      out << ",\"last_draw_index\":" << r.draws.size() - 1
          << ",\"last_draw_packet_dword_offset\":" << d.packet
          << ",\"last_draw_error\":\"" << d.error
          << "\",\"last_draw_inputs\":" << d.inputs
          << ",\"last_draw_vertex_bounds\":" << d.vertex_bounds
          << ",\"last_draw_constants\":" << d.constants
          << ",\"last_draw_vertex_error\":\"" << d.vertex_error
          << "\",\"last_draw_texture_error\":\"" << d.texture_error
          << "\",\"last_draw_bounds_error\":\"" << d.bounds_error
          << "\",\"last_draw_constant_error\":\"" << d.constant_error
          << "\",\"last_draw_constant_error_register\":"
          << d.constant_error_register
          << ",\"last_draw_vertex_ranges\":" << d.vertex_ranges
          << ",\"last_draw_texture_ranges\":" << d.texture_ranges
          << ",\"last_draw_float_vectors\":" << d.floats
          << ",\"last_draw_bool_constants\":" << d.bools
          << ",\"last_draw_loop_constants\":" << d.loops;
    }
    out << '}';
    XELOGE("NativeSystemUITrace {}", out.str());
  } catch (...) {
    // Diagnostics cannot change rejection, quarantine, or exception
    // propagation.
  }
}
// First preparation failure with a valid frozen Source only. Preserve owned
// primary/optional already-copied shader CPU bytes, never a live RAM read or
// sealed-input/execution witness. The inner metadata call keeps code[i] alive;
// the central failure fallback handles packet/layout/other preparation errors.
void TracePreparationFailureBytes(
    const Report& report, const char* phase, const char* reason,
    const std::shared_ptr<const NativeFrameSource>& source,
    const Inventory::ShaderLoad* shader = nullptr,
    uint32_t shader_index = UINT32_MAX,
    std::span<const uint8_t> code = {}) noexcept {
  if (!cvars::native_system_ui_trace || !source || !report.owner.valid() ||
      !report.source || source->owner() != report.owner ||
      source->serial() != report.source || source->primary().empty()) {
    return;
  }
  static std::atomic<bool> claimed{false};
  bool expected = false;
  if (!claimed.compare_exchange_strong(expected, true,
                                       std::memory_order_relaxed)) {
    return;
  }
  const auto primary = source->primary();
  constexpr size_t kChunkBytes = 1024;
  constexpr size_t kMaxPrimaryBytes = 128 * 1024;
  constexpr size_t kMaxShaderBytes = 16 * 1024;
  const size_t primary_bytes = std::min(primary.size(), kMaxPrimaryBytes);
  const size_t shader_bytes =
      shader ? std::min(code.size(), kMaxShaderBytes) : 0;
  const size_t primary_chunks = (primary_bytes + kChunkBytes - 1) / kChunkBytes;
  const size_t shader_chunks = (shader_bytes + kChunkBytes - 1) / kChunkBytes;
  uint32_t primary_returned = 0, shader_returned = 0;
  try {
    auto identity = fmt::format(
        "\"schema\":1,\"component\":\"transport\",\"capture_id\":1,"
        "\"source_metadata_known\":true,\"primary_origin\":\"frozen_source\","
        "\"kernel_epoch\":{},\"runtime_module_generation\":{},"
        "\"owner_session_id\":{},\"owner_hmodule\":{},\"source_serial\":{},"
        "\"failure_phase\":\"{}\",\"failure_reason\":\"{}\","
        "\"report_error\":\"{}\",\"report_fields_partial\":true,"
        "\"packet_profile_checked\":{},\"packet_profile_error\":\"{}\","
        "\"packet_error_dword_offset\":{},\"packet_error_register\":{},"
        "\"shader_metadata_known\":{}",
        report.owner.kernel_epoch, report.owner.module_generation,
        report.owner.session_id, report.owner.module_hmodule, report.source,
        phase, reason, report.error, report.packet_profile_checked,
        report.packet_profile_error, report.packet_profile_error_offset,
        report.packet_profile_error_register, shader != nullptr);
    if (shader) {
      identity += fmt::format(
          ",\"shader_load_index\":{},\"shader_stage\":{},"
          "\"shader_packet_dword_offset\":{},\"shader_inline\":{},"
          "\"shader_raw_address\":{},\"shader_physical_address\":{},"
          "\"shader_declared_dwords\":{},\"shader_error\":\"{}\","
          "\"shader_error_dword_offset\":{}",
          shader_index, uint32_t(shader->stage), shader->packet_dword_offset,
          shader->is_inline, shader->raw_address, shader->physical_address,
          shader->dword_count, report.shader_error, report.shader_error_offset);
    }
    const size_t shader_total_bytes = shader ? code.size() : 0;
    XELOGI(
        "NativeSystemUITrace "
        "{{{},\"event\":\"source_preparation_failure_bytes_begin\","
        "\"chunk_bytes\":{},\"primary_total_bytes\":{},"
        "\"primary_captured_bytes\":{},\"primary_chunk_count\":{},"
        "\"primary_truncated\":{},\"shader_total_bytes\":{},"
        "\"shader_captured_bytes\":{},\"shader_chunk_count\":{},"
        "\"shader_truncated\":{},\"snapshot_seal_proven\":false,"
        "\"execution_permission\":false}}",
        identity, kChunkBytes, primary.size(), primary_bytes, primary_chunks,
        primary_bytes < primary.size(), shader_total_bytes, shader_bytes,
        shader_chunks, shader_bytes < shader_total_bytes);
    const auto chunks = [&](const char* stream, std::span<const uint8_t> bytes,
                            size_t count, uint32_t& returned) {
      constexpr char digits[] = "0123456789abcdef";
      for (size_t index = 0; index < count; ++index) {
        const size_t offset = index * kChunkBytes;
        const auto chunk =
            bytes.subspan(offset, std::min(kChunkBytes, bytes.size() - offset));
        std::string hex(chunk.size() * 2, '0');
        for (size_t i = 0; i < chunk.size(); ++i) {
          hex[i * 2] = digits[chunk[i] >> 4];
          hex[i * 2 + 1] = digits[chunk[i] & 15];
        }
        XELOGI(
            "NativeSystemUITrace "
            "{{{},\"event\":\"source_preparation_failure_bytes_chunk\","
            "\"chunk_stream\":\"{}\",\"chunk_index\":{},\"chunk_count\":{},"
            "\"byte_offset\":{},\"byte_count\":{},\"hex\":\"{}\"}}",
            identity, stream, index, count, offset, chunk.size(), hex);
        ++returned;
      }
    };
    chunks("primary", primary.first(primary_bytes), primary_chunks,
           primary_returned);
    if (shader) {
      chunks("shader", code.first(shader_bytes), shader_chunks,
             shader_returned);
    }
    XELOGI(
        "NativeSystemUITrace "
        "{{{},\"event\":\"source_preparation_failure_bytes_end\","
        "\"primary_total_bytes\":{},\"primary_captured_bytes\":{},"
        "\"primary_chunk_count\":{},\"primary_chunks_returned\":{},"
        "\"primary_truncated\":{},\"shader_total_bytes\":{},"
        "\"shader_captured_bytes\":{},\"shader_chunk_count\":{},"
        "\"shader_chunks_returned\":{},\"shader_truncated\":{},"
        "\"all_log_calls_returned\":true,\"snapshot_seal_proven\":false,"
        "\"execution_permission\":false}}",
        identity, primary.size(), primary_bytes, primary_chunks,
        primary_returned, primary_bytes < primary.size(), shader_total_bytes,
        shader_bytes, shader_chunks, shader_returned,
        shader_bytes < shader_total_bytes);
  } catch (...) {
    // Missing/incomplete begin/chunk/end sequences are not a byte witness.
    // Diagnostics cannot change the existing rejection or exception behavior.
    try {
      XELOGE(
          "NativeSystemUITrace {{\"schema\":1,\"component\":\"transport\","
          "\"event\":\"source_preparation_failure_bytes_capture_error\","
          "\"capture_id\":1,\"source_metadata_known\":true,"
          "\"kernel_epoch\":{},\"owner_session_id\":{},\"source_serial\":{},"
          "\"failure_phase\":\"{}\",\"failure_reason\":\"{}\","
          "\"primary_chunks_returned\":{},\"shader_chunks_returned\":{},"
          "\"execution_permission\":false}}",
          report.owner.kernel_epoch, report.owner.session_id, report.source,
          phase, reason, primary_returned, shader_returned);
    } catch (...) {
    }
  }
}

struct SystemPreparationDiagnostics {
  Report& report;
  // CPU-only frozen bytes. No Session, Task, Kernel or module holder.
  std::shared_ptr<const NativeFrameSource> source = {};
  const char* phase = "validate_completed_record";
  bool source_metadata_known = false;
  bool reported = false, success = false;
  const int entering_exceptions =
      cvars::native_system_ui_trace ? std::uncaught_exceptions() : 0;
  void Fail(const char* reason) noexcept {
    if (!reported) {
      reported = true;
      TraceSystemPreparationFailure(report, phase, reason,
                                    source_metadata_known);
      if (source_metadata_known) {
        TracePreparationFailureBytes(report, phase, reason, source);
      }
    }
  }
  ~SystemPreparationDiagnostics() {
    // Holder allocation/reserve and owner checks precede the original Source
    // try block. Preserve their exception behavior while making it observable.
    if (!success && cvars::native_system_ui_trace &&
        std::uncaught_exceptions() > entering_exceptions) {
      Fail("exception");
    }
  }
};

FILE* OpenReport(const std::filesystem::path& path) {
  auto report_path = path;
  report_path += ".lease.json";
  FILE* file = filesystem::OpenFile(report_path, "wbx");
  if (!file) {
    return nullptr;
  }
#if !XE_PLATFORM_WIN32
  if (::fchmod(::fileno(file), S_IRUSR | S_IWUSR)) {
    std::fclose(file);
    return nullptr;
  }
#endif
  return file;
}

bool WriteReport(FILE* file, const Report& r) {
  // Strings are fixed diagnostic enum/error names, never guest text or paths.
  std::ostringstream out;
  out << std::boolalpha << "{\"version\":2,\"execution_ready\":false,"
      << "\"gpu_submitted\":false,\"shader_semantics_verified\":false,"
      << "\"error\":\"" << r.error
      << "\",\"owner\":{\"kernel_epoch\":" << r.owner.kernel_epoch
      << ",\"module_generation\":" << r.owner.module_generation
      << ",\"session_id\":" << r.owner.session_id
      << ",\"module_hmodule\":" << r.owner.module_hmodule
      << "},\"source_serial\":" << r.source << ",\"device\":" << r.device
      << ",\"dc\":" << r.dc << ",\"frozen\":" << r.frozen
      << ",\"frozen_update_denied\":" << r.frozen_update_denied
      << ",\"output_writable\":" << r.output_writable
      << ",\"primary_matches\":" << r.primary_matches
      << ",\"release_returned_pinned\":" << r.release_refused
      << ",\"record_parent_unchanged\":" << r.parent_unchanged
      << ",\"captured\":" << r.captured
      << ",\"abort_attempted\":" << r.abort_attempted
      << ",\"aborted_unsubmitted\":" << r.aborted
      << ",\"bound_requires_new_update\":" << r.bound_requires_new_update
      << ",\"all_draw_input_constant_proofs\":" << r.coverage
      << ",\"input_pins_complete\":" << r.input_pins_complete
      << ",\"execution_request_prepared\":" << r.execution_request_prepared
      << ",\"execution_request_admitted_before_abort\":"
      << r.execution_request_admitted
      << ",\"execution_request_canceled_after_abort\":"
      << r.execution_request_canceled
      << ",\"prepared_handoff\":" << r.prepared_handoff
      << ",\"packet_profile_checked\":" << r.packet_profile_checked
      << ",\"packet_profile_error\":\"" << r.packet_profile_error
      << "\",\"packet_profile_error_dword_offset\":"
      << r.packet_profile_error_offset << ",\"packet_profile_error_register\":"
      << r.packet_profile_error_register
      << ",\"packet_count\":" << r.packet_count
      << ",\"shader_loads\":" << r.shaders
      << ",\"analyzed_shader_loads\":" << r.analyzed_shaders
      << ",\"pin_phase\":\"" << r.pin_phase
      << "\",\"pin_status\":" << uint32_t(r.pin_status)
      << ",\"pin_failed_range\":" << r.pin_failed_range
      << ",\"failed_physical_address\":" << r.failed_range.address
      << ",\"failed_length\":" << r.failed_range.length
      << ",\"failed_shader_load\":" << r.failed_shader << ",\"shader_error\":\""
      << r.shader_error
      << "\",\"shader_error_dword_offset\":" << r.shader_error_offset
      << ",\"allocations\":[";
  bool comma = false;
  for (const auto& [id, allocation] : r.allocations) {
    out << (comma ? "," : "") << "{\"id\":" << id
        << ",\"physical_address\":" << allocation.physical_address
        << ",\"virtual_address\":" << allocation.virtual_address
        << ",\"length\":" << allocation.length << '}';
    comma = true;
  }
  out << "],\"ranges\":[";
  for (size_t i = 0; i < r.ranges.size(); ++i) {
    const auto& range = r.ranges[i];
    out << (i ? "," : "") << "{\"kind\":\"" << range.kind
        << "\",\"index\":" << range.index << ",\"slot\":" << range.slot
        << ",\"physical_address\":" << range.range.address
        << ",\"length\":" << range.range.length << '}';
  }
  out << "],\"draws\":[";
  for (size_t i = 0; i < r.draws.size(); ++i) {
    const auto& draw = r.draws[i];
    out << (i ? "," : "") << "{\"packet_dword_offset\":" << draw.packet
        << ",\"inputs\":" << draw.inputs
        << ",\"vertex_bounds\":" << draw.vertex_bounds
        << ",\"constants\":" << draw.constants
        << ",\"vertex_ranges\":" << draw.vertex_ranges
        << ",\"texture_ranges\":" << draw.texture_ranges
        << ",\"float_vectors\":" << draw.floats
        << ",\"bool_words\":" << draw.bools << ",\"loop_words\":" << draw.loops
        << ",\"error\":\"" << draw.error << "\",\"vertex_error\":\""
        << draw.vertex_error << "\",\"texture_error\":\"" << draw.texture_error
        << "\",\"bounds_error\":\"" << draw.bounds_error
        << "\",\"constant_error\":\"" << draw.constant_error
        << "\",\"constant_error_register\":" << draw.constant_error_register
        << '}';
  }
  out << "]}\n";
  const auto bytes = out.str();
  return bytes.size() <= kMaxReportBytes &&
         std::fwrite(bytes.data(), 1, bytes.size(), file) == bytes.size() &&
         !std::fflush(file);
}

bool CopyPhysical(Memory* memory, Range range, std::span<uint8_t> out) {
  if (!Valid(range) || out.size() != range.length) {
    return false;
  }
  auto lock = global_critical_region::Acquire();
  auto* heap = memory->GetPhysicalHeap();
  if (!heap ||
      heap->QueryRangeAccess(range.address, range.address + range.length - 1) ==
          memory::PageAccess::kNoAccess) {
    return false;
  }
  std::memcpy(out.data(), memory->TranslatePhysical(range.address), out.size());
  return true;
}

bool MatchesPrimary(Memory* memory, uint32_t address,
                    std::span<const uint8_t> primary) {
  std::vector<uint8_t> bytes(primary.size());
  return CopyPhysical(memory, {address, uint32_t(bytes.size())}, bytes) &&
         std::equal(bytes.begin(), bytes.end(), primary.begin(), primary.end());
}

bool Pin(Memory* memory, const std::shared_ptr<Session>& session,
         const Session::ThreadIdentity& thread,
         const std::shared_ptr<const NativeFrameSource>& source,
         std::span<const Range> ranges, const std::shared_ptr<Pins>& retained,
         Report& report, const char* phase) {
  if (ranges.empty()) {
    return true;
  }
  auto result = memory->PinOwnedPhysicalRanges(session->owner(), ranges);
  report.pin_phase = phase;
  report.pin_status = result.status;
  report.pin_failed_range = result.failed_range;
  if (!result) {
    if (result.failed_range < ranges.size()) {
      report.failed_range = ranges[result.failed_range];
    }
    report.error = "owned_backing_pin_refused";
    return false;
  }
  // Retain before any session attachment or report allocation can throw.
  retained->tokens.push_back(result.pin);
  if (!session->AttachPhysicalPin(thread, source, result.pin)) {
    report.error = "session_pin_attachment_refused";
    return false;
  }
  for (const auto& allocation : result.pin->allocations()) {
    if (allocation.owner != session->owner()) {
      report.error = "wrong_backing_owner";
      return false;
    }
    const auto previous = report.allocations.find(allocation.allocation_id);
    if (previous != report.allocations.end() &&
        !SameAllocation(previous->second, allocation)) {
      report.error = "allocation_identity_changed";
      return false;
    }
    report.allocations.emplace(allocation.allocation_id, allocation);
  }
  return true;
}

bool CheckFrame(
    KernelState* kernel, const std::shared_ptr<Session>& session,
    const Session::ThreadIdentity& thread,
    const std::shared_ptr<const NativeFrameSource>& source,
    const NativeCompletedFrameRecord& record, const std::filesystem::path& path,
    const std::shared_ptr<Pins>& retained, Report& report,
    bool abort_after_prepare,
    std::shared_ptr<const gpu::NativeFrameExecutionRequest>* prepared,
    bool diagnostic, const std::array<uint8_t, 148>* descriptor,
    gpu::NativeFrameInputProvenance input_mode,
    const std::shared_ptr<const NativeNonmodalOutputProvenance>&
        output_provenance = {},
    const std::optional<gpu::NativeOverlayState>& overlay_input_state = {},
    const std::shared_ptr<const NativeNonmodalControlProvenance>&
        control_provenance = {}) {
  const bool immutable_inputs =
      input_mode == gpu::NativeFrameInputProvenance::kImmutableSnapshot;
  const auto fail = [&](const char* error) {
    report.error = error;
    return false;
  };
  auto* memory = kernel->memory();
  if ((input_mode != gpu::NativeFrameInputProvenance::kLiveOwnerBacking &&
       input_mode != gpu::NativeFrameInputProvenance::kImmutableSnapshot) ||
      diagnostic != (descriptor == nullptr) ||
      (diagnostic && (output_provenance || overlay_input_state)) ||
      (immutable_inputs && diagnostic)) {
    return fail("output_layout_origin_mismatch");
  }
  if (!diagnostic && !NativeFrameLeaseBuilder::MatchesCompletedRecord(
                         session, thread, source, record.device, record.dc)) {
    return fail("native_device_dc_provenance_mismatch");
  }
  auto denied = session->BeginUpdate(thread);
  report.frozen_update_denied = !denied && !session->AllocationOwner(thread);
  if (!report.frozen_update_denied) {
    return fail("frozen_producer_not_closed");
  }
  const auto packet_plan = gpu::NativeFrameCommandPlan::AnalyzeWithTitleInput(
      source->primary(), source->title_input());
  report.packet_profile_checked = bool(packet_plan);
  report.packet_profile_error =
      gpu::NativeFrameCommandPlan::ErrorName(packet_plan.error);
  report.packet_profile_error_offset = packet_plan.error_dword_offset;
  report.packet_profile_error_register = packet_plan.error_register;
  if (!packet_plan) {
    return fail("complete_packet_profile_refused");
  }
  const bool control = packet_plan.plan->is_overlay_control();
  report.control_only = control;
  if (control ? (diagnostic || !control_provenance || output_provenance)
              : bool(control_provenance)) {
    return fail("control_origin_mismatch");
  }
  const Range record_range{record.record_physical_address,
                           record.record_allocation_bytes};
  const Range output{record.output_physical_address,
                     record.output_allocation_bytes};
  const Range primary{record.primary_physical_address,
                      uint32_t(source->primary().size())};
  if (!record.native_end_completed || !record.device || !record.dc ||
      !Valid(record_range) || !Valid(primary) ||
      (control ? (output.address || output.length || record.output_cpu_address)
               : !Valid(output)) ||
      primary.address < record_range.address ||
      uint64_t(primary.address) + primary.length >
          uint64_t(record_range.address) + record_range.length ||
      Overlaps(record_range, output)) {
    return fail("invalid_completed_record_or_output");
  }
  const std::array<Range, 2> initial{record_range, output};
  report.ranges.push_back({"record_allocation", 0, 0, record_range});
  if (!control) {
    report.ranges.push_back({"output_allocation", 0, 0, output});
  }
  report.ranges.push_back({"primary", 0, 0, primary});
  if (!Pin(memory, session, thread, source,
           std::span(initial).first(control ? 1 : 2), retained, report,
           "record_output")) {
    return false;
  }
  if (!control) {
    auto lock = global_critical_region::Acquire();
    auto* heap = memory->GetPhysicalHeap();
    const auto access =
        heap ? heap->QueryRangeAccess(output.address,
                                      output.address + output.length - 1)
             : memory::PageAccess::kNoAccess;
    report.output_writable = access == memory::PageAccess::kReadWrite ||
                             access == memory::PageAccess::kExecuteReadWrite;
  }
  report.primary_matches =
      immutable_inputs ||
      MatchesPrimary(memory, primary.address, source->primary());
  if ((!control && !report.output_writable) || !report.primary_matches) {
    return fail("physical_record_or_output_access_mismatch");
  }
  std::unique_ptr<NativeFrameInputCapture> capture;
  if (immutable_inputs) {
    capture = NativeFrameLeaseBuilder::BeginCapture(
        memory, session, thread, source, packet_plan.plan, primary, output);
    std::vector<uint8_t> bytes(primary.length);
    if (!capture || !capture->Capture(record_range) ||
        !capture->Read(primary, bytes) ||
        !std::equal(bytes.begin(), bytes.end(), source->primary().begin(),
                    source->primary().end())) {
      return fail("snapshot_primary_changed");
    }
    report.primary_matches = true;
  }
  report.packet_count = uint32_t(packet_plan.plan->packets().size());
  const auto& scan = packet_plan.plan->inventory();
  if (!scan.complete_primary_scan || scan.error != Inventory::Error::kNone ||
      (!control && scan.draws.empty())) {
    return fail("incomplete_draw_inventory");
  }
  report.shaders = uint32_t(scan.shaders.size());
  std::vector<Range> shader_ranges;
  uint64_t input_bytes =
      record_range.length +
      (source->title_input() ? source->title_input()->bytes().size() : 0);
  if (input_bytes > kMaxInputBytes) {
    return fail("input_byte_budget");
  }
  const auto request = [&](const char* kind, uint32_t index, uint32_t slot,
                           Range range, std::vector<Range>& ranges) {
    if (!Valid(range) || Overlaps(range, output) ||
        report.ranges.size() >= kMaxRanges ||
        range.length > kMaxInputBytes - input_bytes) {
      return false;
    }
    input_bytes += range.length;
    ranges.push_back(range);
    report.ranges.push_back({kind, index, slot, range});
    return true;
  };
  for (size_t i = 0; i < scan.shaders.size(); ++i) {
    const auto& shader = scan.shaders[i];
    if (!shader.is_inline &&
        !request("shader", uint32_t(i), 0,
                 {shader.physical_address, shader.dword_count * 4},
                 shader_ranges)) {
      return fail("shader_range_overlap_or_budget");
    }
  }
  if (capture) {
    for (const auto range : shader_ranges) {
      if (!capture->Capture(range)) {
        return fail("snapshot_shader_capture_failed");
      }
    }
  } else if (!Pin(memory, session, thread, source, shader_ranges, retained,
                  report, "external_shaders")) {
    return false;
  }
  std::vector<std::vector<uint8_t>> code(scan.shaders.size());
  std::vector<Shader::Result> metadata(scan.shaders.size());
  for (size_t i = 0; i < scan.shaders.size(); ++i) {
    const auto& shader = scan.shaders[i];
    const size_t size = size_t(shader.dword_count) * 4;
    if (shader.is_inline) {
      const size_t offset = size_t(shader.inline_dword_offset) * 4;
      if (!size || offset > source->primary().size() ||
          size > source->primary().size() - offset) {
        return fail("inline_shader_range_invalid");
      }
      const auto bytes = source->primary().subspan(offset, size);
      code[i].assign(bytes.begin(), bytes.end());
    } else {
      code[i].resize(size);
      const Range range{shader.physical_address, uint32_t(size)};
      if (!(capture ? capture->Read(range, code[i])
                    : CopyPhysical(memory, range, code[i]))) {
        return fail("pinned_shader_copy_failed");
      }
    }
    metadata[i] = Shader::Analyze(shader.stage, code[i]);
    if (!metadata[i].metadata_analyzed ||
        metadata[i].error != Shader::Error::kNone ||
        metadata[i].stage != shader.stage) {
      report.failed_shader = uint32_t(i);
      report.shader_error = Shader::ErrorName(metadata[i].error);
      report.shader_error_offset = metadata[i].error_dword_offset;
      if (cvars::native_system_ui_trace) {
        // The following return unconditionally reports this same failure.
        // Make the fixed report error available to the owned-code capture.
        report.error = "shader_metadata_incomplete";
        TracePreparationFailureBytes(report, "check_frame_and_request",
                                     "shader_metadata_incomplete", source,
                                     &shader, uint32_t(i), code[i]);
      }
      return fail("shader_metadata_incomplete");
    }
    ++report.analyzed_shaders;
  }
  std::vector<Range> remaining;
  std::vector<Range> title_ranges;
  for (const auto& draw : scan.draws) {
    const auto selected =
        [&](std::optional<uint32_t> index,
            gpu::xenos::ShaderType stage) -> const Shader::Result* {
      return index && *index < metadata.size() &&
                     scan.shaders[*index].stage == stage
                 ? &metadata[*index]
                 : nullptr;
    };
    const auto* vertex =
        selected(draw.vertex_shader, gpu::xenos::ShaderType::kVertex);
    const auto* pixel =
        selected(draw.pixel_shader, gpu::xenos::ShaderType::kPixel);
    const auto vertices = Inventory::VertexReads(draw, vertex);
    const auto textures = Inventory::TextureReads(draw, pixel);
    const auto constants = Inventory::ConstantReads(draw, vertex, pixel);
    const std::span<const uint8_t> vertex_code =
        vertex ? std::span<const uint8_t>(code[*draw.vertex_shader])
               : std::span<const uint8_t>{};
    const auto bounds = Vertex::Analyze(draw, vertex_code);
    report.draws.push_back(
        {draw.packet_dword_offset,
         vertices.error == Inventory::Error::kNone &&
             textures.error == Inventory::Error::kNone,
         bounds.vertex_input_bounds_verified &&
             bounds.error == Vertex::Error::kNone,
         constants.used_constant_values_known &&
             constants.error == Inventory::Error::kNone,
         uint32_t(vertices.reads.size()), uint32_t(textures.reads.size()),
         uint32_t(constants.floats.size()), uint32_t(constants.bools.size()),
         uint32_t(constants.loops.size()), "none"});
    auto& proof = report.draws.back();
    proof.vertex_error = Inventory::ErrorName(vertices.error);
    proof.texture_error = Inventory::ErrorName(textures.error);
    proof.bounds_error = Vertex::ErrorName(bounds.error);
    proof.constant_error = Inventory::ErrorName(constants.error);
    proof.constant_error_register = constants.error_register;
    if (!proof.inputs || !proof.vertex_bounds || !proof.constants) {
      proof.error = !proof.inputs          ? "input_inventory_failed"
                    : !proof.vertex_bounds ? "vertex_bounds_failed"
                                           : "used_constants_unknown";
      return fail(proof.error);
    }
    for (const auto& read : vertices.reads) {
      if (!request("vertex", draw.packet_dword_offset, read.slot,
                   {read.physical_address, read.byte_count}, remaining)) {
        return fail("vertex_range_overlap_or_budget");
      }
    }
    for (const auto& read : textures.reads) {
      if (read.from_title_input) {
        const Range range{read.physical_address, read.byte_count};
        if (!source->title_input() || !source->title_input()->Covers(range) ||
            Overlaps(range, output) || Overlaps(range, record_range) ||
            report.ranges.size() >= kMaxRanges) {
          return fail("title_input_provenance_or_alias");
        }
        title_ranges.push_back(range);
        report.ranges.push_back(
            {"title_input", draw.packet_dword_offset, read.slot, range});
        continue;
      }
      if (!request("texture", draw.packet_dword_offset, read.slot,
                   {read.physical_address, read.byte_count}, remaining)) {
        return fail("texture_range_overlap_or_budget");
      }
    }
  }
  report.coverage = report.draws.size() == scan.draws.size();
  if (capture) {
    for (const auto range : remaining) {
      if (!capture->Capture(range)) {
        return fail("snapshot_input_capture_failed");
      }
    }
  } else if (!Pin(memory, session, thread, source, remaining, retained, report,
                  "vertex_texture")) {
    return false;
  }
  if (source->title_input() && title_ranges.empty()) {
    const auto range = source->title_input()->range();
    if (Overlaps(range, output) || Overlaps(range, record_range)) {
      return fail("title_input_provenance_or_alias");
    }
    report.ranges.push_back({"title_input", 0, 15, range});
  }
  report.input_pins_complete = true;
  std::optional<Allocation> record_parent;
  for (const auto& [id, allocation] : report.allocations) {
    if (Contains(allocation, record_range)) {
      record_parent = allocation;
      break;
    }
  }
  if (!record_parent) {
    return fail("record_has_no_single_owned_parent");
  }
  if (diagnostic) {
    report.release_refused =
        memory->ReleaseOwnedPhysicalAllocation(
            session->owner(), record_parent->allocation_id) == Status::kPinned;
    if (!report.release_refused) {
      return fail("pinned_record_release_not_refused");
    }
  }
  const auto checked = memory->PinOwnedPhysicalRanges(
      session->owner(), std::span<const Range>(&record_range, 1));
  if (!checked) {
    return fail("record_parent_recheck_failed");
  }
  for (const auto& allocation : checked.pin->allocations()) {
    report.parent_unchanged |= SameAllocation(*record_parent, allocation);
  }
  if (capture) {
    std::vector<uint8_t> bytes(primary.length);
    report.parent_unchanged &=
        capture->Read(primary, bytes) &&
        std::equal(bytes.begin(), bytes.end(), source->primary().begin(),
                   source->primary().end());
  } else {
    report.parent_unchanged &=
        MatchesPrimary(memory, primary.address, source->primary());
  }
  if (!report.parent_unchanged) {
    return fail("pinned_release_mutated_record");
  }
  if (diagnostic) {
    report.captured = CaptureNativeRecord(memory, path, source->primary());
    if (!report.captured) {
      return fail("host_record_capture_failed");
    }
  }
  std::vector<gpu::NativeFrameExecutionRequest::Shader> prepared_shaders;
  prepared_shaders.reserve(scan.shaders.size());
  for (size_t i = 0; i < scan.shaders.size(); ++i) {
    prepared_shaders.push_back(
        {scan.shaders[i], std::move(code[i]), std::move(metadata[i])});
  }
  std::vector<Range> prepared_inputs;
  prepared_inputs.reserve(report.ranges.size() - 1);
  for (const auto& range : report.ranges) {
    if (range.range.address != output.address ||
        range.range.length != output.length) {
      prepared_inputs.push_back(range.range);
    }
  }
  std::shared_ptr<const gpu::NativeFrameInputSnapshot> snapshot;
  if (capture) {
    snapshot = capture->Seal();
    if (!snapshot) {
      return fail("snapshot_seal_failed");
    }
    capture.reset();
  }
  const auto prepared_request = NativeFrameLeaseBuilder::Mint(
      memory, record, session, thread, source, packet_plan.plan, output,
      std::move(prepared_inputs), std::move(prepared_shaders), descriptor,
      snapshot, output_provenance, overlay_input_state, control_provenance);
  report.execution_request_prepared = bool(prepared_request);
  report.execution_request_admitted =
      prepared_request && prepared_request->IsAdmitted();
  if (!report.execution_request_admitted) {
    return fail("prepared_execution_request_refused");
  }
  *prepared = prepared_request;
  if (!abort_after_prepare) {
    report.prepared_handoff = true;
    return true;
  }
  report.abort_attempted = true;
  report.aborted = session->AbortUnsubmitted(thread, source);
  if (!report.aborted) {
    return fail("unsubmitted_abort_refused");
  }
  const auto after = session->GetSnapshot();
  report.bound_requires_new_update =
      after.phase == Session::Phase::kBound && !after.update_active &&
      !after.operation_active && !after.gpu_attached &&
      !after.submission_attempted && !session->AllocationOwner(thread);
  report.execution_request_canceled =
      !prepared_request->IsAdmitted() &&
      !prepared_request->MarkSubmissionAttempted() &&
      !session->GetSnapshot().submission_attempted;
  if (!report.execution_request_canceled) {
    return fail("aborted_execution_request_still_admitted");
  }
  return report.bound_requires_new_update ||
         fail("abort_did_not_close_old_update");
}
}  // namespace

static bool RunNativeFrameLeaseCheck(
    KernelState* kernel, std::shared_ptr<NativeFrameOwnerSession> session,
    const NativeCompletedFrameRecord& record,
    const std::filesystem::path& capture_path, bool abort_after_prepare,
    std::shared_ptr<const gpu::NativeFrameExecutionRequest>* output_request) {
  if (output_request) {
    output_request->reset();
  }
  if (!kernel || !kernel->memory() || !session || !capture_path.is_absolute() ||
      !capture_path.has_filename() || capture_path.filename() == "." ||
      capture_path.filename() == "..") {
    if (session) {
      session->Revoke();
    }
    return false;
  }
  // A retained raw identity is not dereferenced unless it is the actual
  // current thread; a reused address must also match ID/kernel/process.
  const auto identity = session->GetSnapshot().thread;
  auto* owner_thread = const_cast<XThread*>(identity.thread);
  if (identity.kernel != kernel || !owner_thread ||
      !XThread::IsInThread(owner_thread)) {
    session->Revoke();
    return false;
  }
  const auto thread = owner_thread->native_frame_thread_identity();
  if (!session->MatchesThread(thread)) {
    session->Revoke();
    return false;
  }
  Report report;
  report.owner = session->owner();
  report.device = record.device;
  report.dc = record.dc;
  std::unique_ptr<FILE, decltype(&std::fclose)> file(nullptr, &std::fclose);
  std::shared_ptr<Pins> retained;
  std::shared_ptr<const gpu::NativeFrameExecutionRequest> prepared;
  bool success = false;
  try {
    // First producer-state operation: no guest physical read precedes freeze.
    const auto source = session->FreezeCurrentUpdate(thread, record.primary);
    report.frozen = bool(source);
    report.source = source ? source->serial() : 0;
    file.reset(OpenReport(capture_path));
    if (!file) {
      session->Revoke();
      XELOGE("Native frame lease: exclusive private report creation failed");
      return false;
    }
    if (!source) {
      report.error = "completed_frame_freeze_refused";
    } else {
      retained = std::make_shared<Pins>();
      retained->tokens.reserve(3);
      kernel->RetainNativeResourceUntilTeardown(retained);
      success = CheckFrame(kernel, session, thread, source, record,
                           capture_path, retained, report, abort_after_prepare,
                           &prepared, true, nullptr,
                           gpu::NativeFrameInputProvenance::kLiveOwnerBacking);
    }
  } catch (const std::exception&) {
    session->Revoke();
    report.error = "host_exception_quarantined";
  } catch (...) {
    session->Revoke();
    throw;
  }
  if (!success) {
    session->Revoke();
  }
  try {
    if (!file || !WriteReport(file.get(), report)) {
      session->Revoke();
      return false;
    }
  } catch (...) {
    session->Revoke();
    throw;
  }
  FILE* written = file.release();
  if (std::fclose(written)) {
    session->Revoke();
    return false;
  }
  // Host report finalization belongs to the gate too. Keep the independent
  // Kernel-held pin set even after checked Abort if write/flush/close fails.
  if (success && retained) {
    retained->tokens.clear();
  }
  if (success && output_request) {
    *output_request = std::move(prepared);
  }
  XELOGI(
      "Native frame lease: source={} draws={} coverage={} pinned_parents={} "
      "release_refused={} capture={} abort={} result={}; no GPU submission",
      report.source, report.draws.size(), report.coverage,
      report.allocations.size(), report.release_refused, report.captured,
      report.aborted, report.error);
  return success;
}

bool RunNativeFrameLeaseProbe(
    KernelState* kernel, std::shared_ptr<NativeFrameOwnerSession> session,
    const NativeCompletedFrameRecord& record,
    const std::filesystem::path& capture_path,
    std::shared_ptr<const gpu::NativeFrameExecutionRequest>* canceled_request) {
  return RunNativeFrameLeaseCheck(kernel, std::move(session), record,
                                  capture_path, true, canceled_request);
}

std::shared_ptr<const gpu::NativeFrameExecutionRequest>
PrepareNativeFrameExecutionRequest(
    KernelState* kernel, std::shared_ptr<NativeFrameOwnerSession> session,
    const NativeCompletedFrameRecord& record,
    const std::filesystem::path& capture_path) {
  std::shared_ptr<const gpu::NativeFrameExecutionRequest> request;
  if (!RunNativeFrameLeaseCheck(kernel, std::move(session), record,
                                capture_path, false, &request)) {
    return {};
  }
  return request;
}
std::shared_ptr<const gpu::NativeFrameExecutionRequest>
PrepareNativeSystemFrameExecutionRequest(
    KernelState* kernel,
    const std::shared_ptr<const NativeSystemCompletedFrameRecord>& completed) {
  Report report;
  SystemPreparationDiagnostics diagnostics{report};
  std::shared_ptr<Session> session;
  const auto fail = [&](const char* reason, bool revoke_session_first = false)
      -> std::shared_ptr<const gpu::NativeFrameExecutionRequest> {
    // Emit the existing proof result BEFORE the original revocation/close
    // sequence consumes the failed preparation state. No new authority call.
    diagnostics.Fail(reason);
    if (revoke_session_first && session) {
      session->Revoke();
    }
    if (completed && completed->delegation) {
      completed->delegation->authority()->CloseAdmission();
      if (completed->delegation->session()) {
        completed->delegation->session()->Revoke();
      }
    }
    return {};
  };
  if (!completed || !completed->immutable_primary ||
      !completed->full_record_pin || !completed->full_record_pin->valid()) {
    return fail("completed_record_or_pin_invalid");
  }
  diagnostics.phase = "validate_input_provenance";
  using InputProvenance = NativeSystemCompletedFrameRecord::InputProvenance;
  const bool immutable_inputs =
      completed->input_provenance == InputProvenance::kImmutableSnapshot;
  if (immutable_inputs ? bool(completed->all_writer_exclusion)
                       : (completed->input_provenance !=
                              InputProvenance::kActualWriterGrant ||
                          !completed->all_writer_exclusion)) {
    return fail("input_provenance_invalid");
  }
  const auto& record = completed->record;
  const auto& delegation = completed->delegation;
  diagnostics.phase = "validate_primary_identity";
  if (record.primary.data() != completed->immutable_primary->data() ||
      record.primary.size() != completed->immutable_primary->size()) {
    return fail("primary_snapshot_identity_mismatch");
  }
  diagnostics.phase = "validate_caller";
  if (!kernel || !delegation || !delegation->IsAccepting() ||
      !XThread::IsInThread()) {
    return fail("delegation_or_caller_unavailable");
  }
  session = delegation->session();
  auto* current = XThread::GetCurrentThread();
  if (!session || current->kernel_state() != kernel ||
      !session->MatchesThread(current->native_frame_thread_identity())) {
    return fail("delegated_thread_mismatch", true);
  }
  const auto thread = current->native_frame_thread_identity();
  // Allocate the holder before Source freeze; retain it in Session before
  // physical reads or a staging vector allocation can throw. No per-frame
  // append to KernelState::resident_native_resources_.
  diagnostics.phase = "prepare_pin_holder";
  auto retained = std::make_shared<Pins>();
  retained->tokens.reserve(4);
  retained->native_record = completed;
  diagnostics.phase = "validate_record_pin_owner";
  for (const auto& allocation : completed->full_record_pin->allocations()) {
    if (allocation.owner != delegation->owner()) {
      return fail("record_pin_owner_mismatch", true);
    }
  }
  try {
    diagnostics.phase = "freeze_and_hold_preparation";
    const auto source = session->FreezeCurrentUpdate(thread, record.primary,
                                                     completed->title_input);
    if (source) {
      report.owner = source->owner();
      report.source = source->serial();
      report.device = record.device;
      report.dc = record.dc;
      diagnostics.source_metadata_known = true;
      if (cvars::native_system_ui_trace) {
        diagnostics.source = source;
      }
    }
    if (!source || !NativeFrameLeaseBuilder::HoldPreparationPins(
                       session, thread, source, retained)) {
      return fail("source_freeze_or_pin_hold_refused", true);
    }
    std::shared_ptr<const gpu::NativeFrameExecutionRequest> request;
    const auto expected_input =
        immutable_inputs ? gpu::NativeFrameInputProvenance::kImmutableSnapshot
                         : gpu::NativeFrameInputProvenance::kLiveOwnerBacking;
    diagnostics.phase = "check_frame_and_request";
    // Keep the original short-circuit order and evaluate each existing proof
    // exactly once; labels explain which already-required condition refused.
    const char* refused =
        !CheckFrame(kernel, session, thread, source, record, {}, retained,
                    report, false, &request, false, &completed->vd_descriptor,
                    expected_input, completed->output_provenance,
                    completed->overlay_input_state,
                    completed->control_provenance)
            ? "check_frame_refused"
        : !request ? "prepared_request_missing"
        : request->input_provenance() != expected_input
            ? "prepared_input_provenance_mismatch"
        : bool(request->input_snapshot()) != immutable_inputs
            ? "prepared_snapshot_presence_mismatch"
        : immutable_inputs && !request->input_snapshot()->valid()
            ? "prepared_snapshot_invalid"
        : immutable_inputs &&
                request->input_snapshot()->identity() != request->identity()
            ? "prepared_snapshot_identity_mismatch"
        : immutable_inputs &&
                request->input_snapshot()->plan() != request->plan()
            ? "prepared_snapshot_plan_mismatch"
            : nullptr;
    if (refused) {
      return fail(refused, true);
    }
    diagnostics.success = true;
    return request;
  } catch (...) {
    diagnostics.Fail("exception");
    session->Revoke();
    throw;
  }
}
}  // namespace xe::kernel
