/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xfile.h"
#include "xenia/vfs/file_write_checkpoint.h"
#include "xenia/vfs/virtual_file_system.h"

#include <algorithm>
#include <map>
#include <optional>

#include "xenia/base/byte_stream.h"
#include "xenia/base/clock.h"
#include "xenia/base/filesystem.h"
#include "xenia/base/threading.h"
#include "xenia/kernel/guest_scheduler.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/util/guest_io_buffer.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_threading.h"
#include "xenia/kernel/xthread.h"
#include "xenia/vfs/devices/host_path_device.h"
#include "xenia/vfs/devices/host_path_entry.h"
#include "xenia/vfs/devices/host_path_file.h"

namespace xe {
namespace kernel {
struct XFileIoState {
  std::shared_ptr<vfs::File::CheckpointCloseObservation> close;
  XFile* live =
      nullptr;  // dereference only after TryRetain under file_io_mutex_
  std::shared_ptr<vfs::Device> device;
  std::shared_ptr<const filesystem::HostStorageDirectoryIdentity> directory;
  std::shared_ptr<const vfs::Entry::CheckpointCoordinate> coordinate;
  uint64_t generation = 0, acknowledged = 0;
  // Only currently admitted operations occupy the global pending budget.
  // One earliest genuine failure is sufficient to refuse every affected cut;
  // successful later calls cannot erase it or accumulate failed-history rows.
  std::map<uint64_t, X_STATUS> unsettled;
  std::optional<std::pair<uint64_t, X_STATUS>> first_failure;
  bool closing = false, closed = false, lost = false;
};
struct XFileIoOperation {
  KernelState* kernel;
  std::shared_ptr<XFileIoState> state;
  uint64_t generation;
  bool done = false;
  void Finish(X_STATUS status) {
    std::lock_guard lock(kernel->file_io_mutex_);
    if (done) {
      return;
    }
    done = true;
    if (state->unsettled.erase(generation)) {
      --kernel->file_io_operations_;
    }
    if (!XSUCCEEDED(status) || status == X_STATUS_PENDING) {
      if (!state->first_failure || generation < state->first_failure->first) {
        state->first_failure = std::make_pair(
            generation,
            status == X_STATUS_PENDING ? X_STATUS_UNSUCCESSFUL : status);
      }
    }
  }
  ~XFileIoOperation() {
    if (!done) {
      Finish(X_STATUS_UNSUCCESSFUL);
    }
  }
};
void XFile::RegisterCheckpointIo() {
  const auto device = file_->retained_device();
  const auto coordinate=file_->entry()->RetainCheckpointCoordinate();
  if (!coordinate || !device) {
    return;
  }
  const auto directory = device->ManagedDirectoryIdentity();
  if (!directory) {
    return;
  }
  auto state = std::make_shared<XFileIoState>();
  state->live = this;
  state->device = device;
  state->directory = directory;
  state->close=std::make_shared<vfs::File::CheckpointCloseObservation>();
  file_->ObserveCheckpointClose(state->close);
  state->coordinate=coordinate;
  std::lock_guard lock(kernel_state()->file_io_mutex_);
  // Retain actual live Files and unresolved effects only. A lifetime open
  // count is not a package bound: clean completed closes retire below. The
  // bounded package census checks its own matched rows at capture instead.
  kernel_state()->file_io_records_.push_back(state);
  checkpoint_io_ = std::move(state);
}
std::shared_ptr<XFileIoOperation> XFile::BeginCheckpointIo() {
  if (!checkpoint_io_) {
    return {};
  }
  std::shared_ptr<XFileIoOperation> result;
  const auto invoked = InvokeHostCall([&] {
    // Allocate before committing the pending generation.
    result = std::make_shared<XFileIoOperation>();
    result->kernel = kernel_state();
    result->state = checkpoint_io_;
    std::lock_guard lock(kernel_state()->file_io_mutex_);
    if (checkpoint_io_->generation != UINT64_MAX &&
        kernel_state()->file_io_operations_ < 16384) {
      const auto generation = checkpoint_io_->generation + 1;
      checkpoint_io_->unsettled.emplace(generation, X_STATUS_PENDING);
      ++kernel_state()->file_io_operations_;
      checkpoint_io_->generation = generation;
      result->generation = generation;
    } else {
      checkpoint_io_->lost = true;
      result->done = true;
    }
  });
  if (invoked != HostCallOutcome::kReturned) {
    // No authority/result follows incomplete tracking. Release the partially
    // prepared receipt before marking the independently retained record lost.
    if (result) {
      result->done = true;
    }
    result.reset();
    std::lock_guard lock(kernel_state()->file_io_mutex_);
    checkpoint_io_->lost = true;
  }
  return result;
}
std::shared_ptr<XFileIoCheckpoint> XFile::CaptureIoCheckpoint(
    KernelState& kernel, const filesystem::HostStorageRoot& package,
    X_STATUS& status) {
  status = X_STATUS_UNSUCCESSFUL;
  if (global_critical_region::is_held_by_current_thread() ||
      package.retiring() || !package.directory_identity()) {
    return {};
  }
  auto result =
      std::shared_ptr<XFileIoCheckpoint>(new XFileIoCheckpoint(kernel));
  // Reserve all potentially owning vector storage before TryRetain. Last
  // native-resource releases on refusal occur after this guard has unwound.
  result->source_ = package.shared_from_this();
  result->rows_.reserve(4096);
  result->files_.reserve(4096);
  bool refused = false;
  {
    // Same namespace cut as actual HostPathEntry rename/coordinate commit.
    // No filesystem operation or payload read occurs while these guards live.
    auto namespace_lock = global_critical_region::AcquireDirect();
    std::lock_guard lock(kernel.file_io_mutex_);
    refused = kernel.file_io_overflow_;
    for (const auto& state : kernel.file_io_records_) {
      const auto member =
          state->coordinate->path.lexically_relative(state->directory->path());
      auto relative = package.directory_identity()->RelativeEntry(
          *state->directory, member);
      if (!relative) {
        // Reverse only a POSITIVELY matched native directory lineage, then
        // compare member components in that same real directory coordinate.
        const auto package_in_parent =
            state->directory->RelativeEntry(*package.directory_identity(), {});
        if (package_in_parent) {
          const auto suffix = member.lexically_relative(*package_in_parent);
          if (suffix.empty() ||
              (!suffix.is_absolute() && *suffix.begin() != "..")) {
            relative = suffix;
          }
        }
      }
      if (!relative) {
        if (package.directory_identity()->Overlaps({}, *state->directory,
                                                   member, true)) {
          refused = true;
          break;  // Unknown physical alias is not nonmembership.
        }
        continue;
      }
      if (state->closing || state->lost ||
          state->device->namespace_retiring()) {
        refused = true;
        break;
      }
      if (result->rows_.size() == 4096) {
        refused = true;
        break; // This matched package exceeds the actual census bound.
      }
      if (state->live) {
        // Refuse an already-entered destructor instead of resurrecting ref0.
        if (!state->live->TryRetain()) {
          refused = true;
          break;
        }
        result->files_.emplace_back(state->live);
      }
      result->rows_.push_back({state, state->generation});
    }
  }
  if (refused) {
    status = X_STATUS(0xC00000E8);
    return {};
  }
  // A delete-on-close is namespace work, not made complete by fsync. Keep
  // that separate until its genuine File destruction has actually returned.
  for (const auto& file : result->files_) {
    auto lock = global_critical_region::AcquireDirect();
    if (file->entry()->delete_on_close()) {
      status = X_STATUS(0xC00000E8);
      return {};
    }
  }
  result->captured_live_files_ = result->files_.size();
  status = X_STATUS_SUCCESS;
  return result;
}
X_STATUS XFileIoCheckpoint::Status() const {
  if (!source_ || source_->retiring()) {
    return X_STATUS_UNSUCCESSFUL;
  }
  std::lock_guard lock(kernel_.file_io_mutex_);
  for (const auto& row : rows_) {
    if (row.state->lost || row.state->closing ||
        row.state->device->namespace_retiring()) {
      return X_STATUS_UNSUCCESSFUL;
    }
    if (row.close_required && !row.state->closed) {
      return X_STATUS_PENDING;
    }
    if (row.state->closed &&
        (!row.state->close || XFAILED(row.state->close->data_status.load(
                                  std::memory_order_acquire)))) {
      return X_STATUS_UNSUCCESSFUL;
    }
    const auto pending = row.state->unsettled.begin();
    const auto &failed = row.state->first_failure;
    if (pending != row.state->unsettled.end() &&
        pending->first <= row.generation &&
        (!failed || pending->first < failed->first)) {
      return X_STATUS_PENDING;
    }
    if (failed && failed->first <= row.generation) {
      return failed->second;
    }
  }
  return X_STATUS_SUCCESS;
}
bool XFileIoCheckpoint::AcknowledgeSuccessful() {
  if (!source_ || source_->retiring()) {
    return false;
  }
  if (!drain_completed_.load(std::memory_order_acquire) ||
      drain_result_.load(std::memory_order_relaxed) != X_STATUS_SUCCESS) {
    return false;
  }
  std::lock_guard lock(kernel_.file_io_mutex_);
  for (const auto& row : rows_) {
    if (row.state->lost || row.state->closing ||
        row.state->device->namespace_retiring() ||
        (row.close_required && !row.state->closed) ||
        (row.state->closed &&
         (!row.state->close || XFAILED(row.state->close->data_status.load(
                                   std::memory_order_acquire))))) {
      return false;
    }
    if ((!row.state->unsettled.empty() &&
         row.state->unsettled.begin()->first <= row.generation) ||
        (row.state->first_failure &&
         row.state->first_failure->first <= row.generation)) {
      return false;
    }
  }
  for (const auto& row : rows_) {
    row.state->acknowledged = std::max(row.state->acknowledged, row.generation);
  }
  // rows_ keeps every removed record alive until after this mutex unwinds.
  for (const auto& row : rows_) {
    if (row.state->closed && !row.state->lost && row.state->unsettled.empty() &&
        !row.state->first_failure &&
        row.state->acknowledged == row.state->generation) {
      std::erase(kernel_.file_io_records_, row.state);
    }
  }
  return true;
}
X_STATUS XFileIoCheckpoint::Drain(std::function<void(X_STATUS)> completion) {
  if (!completion || drain_started_.exchange(true)) {
    return X_STATUS_INVALID_PARAMETER;
  }
  struct Drain {
    std::shared_ptr<XFileIoCheckpoint> cut;
    std::function<void(X_STATUS)> completion;
    std::vector<std::shared_ptr<util::IoBatchArrival>> arrivals;
    std::atomic<size_t> remaining{0};
    std::atomic<X_STATUS> failure{X_STATUS_SUCCESS};
    std::atomic<bool> armed{false}, finished{false};
    void MaybeComplete() {
      if (armed.load(std::memory_order_acquire) &&
          remaining.load(std::memory_order_acquire) == 0 &&
          !finished.exchange(true, std::memory_order_acq_rel)) {
        completion(failure.load());
      }
    }
    void Arrive(X_STATUS status) {
      if (XFAILED(status)) {
        X_STATUS success = X_STATUS_SUCCESS;
        failure.compare_exchange_strong(success, status);
      }
      remaining.fetch_sub(1, std::memory_order_acq_rel);
      MaybeComplete();
    }
  };
  auto drain = std::make_shared<Drain>();
  drain->cut = shared_from_this();
  drain->completion = [cut = drain->cut,
                       completion = std::move(completion)](X_STATUS status) {
    cut->drain_result_.store(status, std::memory_order_relaxed);
    cut->drain_completed_.store(true, std::memory_order_release);
    completion(status);
  };
  drain->remaining.store(files_.size());
  drain->arrivals.reserve(files_.size());
  for (size_t i = 0; i < files_.size(); ++i) {
    drain->arrivals.push_back(std::make_shared<util::IoBatchArrival>());
  }
  for (size_t i = 0; i < files_.size(); ++i) {
    auto arrive = [drain, arrival = drain->arrivals[i]](X_STATUS status) {
      arrival->CompleteOnce(
          status, [drain](X_STATUS actual) { drain->Arrive(actual); });
    };
    util::SubmitIoBatchItem(
        [&] { return files_[i]->FlushQueuedInternal(arrive, true); }, arrive);
  }
  bool accepted = false;
  const auto invoked = InvokeHostCall([&] {
    accepted = kernel_.guest_scheduler()->PostHostCall(
        [drain] {
          // Strong File pins and all real close/EOF work are released on a
          // host IO worker, never a reclaimable guest fiber or registry lock.
          {
            auto lock = global_critical_region::AcquireDirect();
            std::lock_guard metadata(drain->cut->kernel_.file_io_mutex_);
            for (const auto& file : drain->cut->files_) {
              if (file->handles().empty()) {
                for (auto& row : drain->cut->rows_) {
                  if (row.state == file->checkpoint_io_) {
                    row.close_required = true;
                  }
                }
              }
            }
          }
          drain->cut->files_.clear();
          drain->armed.store(true, std::memory_order_release);
          drain->MaybeComplete();
        },
        GuestScheduler::BlockingCallClass::kConcurrent,
        [drain](GuestScheduler::HostCallDropReason) {
          X_STATUS success = X_STATUS_SUCCESS;
          drain->failure.compare_exchange_strong(success, X_STATUS_CANCELLED);
          drain->cut->files_.clear();
          drain->armed.store(true, std::memory_order_release);
          drain->MaybeComplete();
        });
  });
  return invoked == HostCallOutcome::kReturned && accepted
             ? X_STATUS_SUCCESS
             : X_STATUS_INSUFFICIENT_RESOURCES;
}

XFile::XFile(KernelState* kernel_state, vfs::File* file, bool synchronous,
             bool alertable, bool buffered)
    : XObject(kernel_state, kObjectType),
      file_(file),
      is_synchronous_(synchronous),
      is_alertable_(alertable),
      is_buffered_(buffered) {
  async_event_ = threading::Event::CreateManualResetEvent(false);
  assert_not_null(async_event_);
  if (InvokeHostCall([&] { RegisterCheckpointIo(); }) !=
      HostCallOutcome::kReturned) {
    std::lock_guard lock(this->kernel_state()->file_io_mutex_);
    this->kernel_state()->file_io_overflow_ = true;
  }
}

XFile::XFile() : XObject(kObjectType), completion_port_lock_() {
  async_event_ = threading::Event::CreateManualResetEvent(false);
  assert_not_null(async_event_);
}

XFile::~XFile() {
  // Refcount has already reached zero: a simultaneous snapshot must refuse,
  // never resurrect this File across a real close/EOF-trim operation.
  if (checkpoint_io_) {
    std::lock_guard lock(kernel_state()->file_io_mutex_);
    checkpoint_io_->live = nullptr;
    checkpoint_io_->closing = true;
  }
  auto close = BeginCheckpointIo();
  async_event_->Set();
  file_->Destroy();
  // No Entry dereference after Destroy: last close can remove that child.
  // The real HostPathFile captured this scalar while it still owned Entry.
  if (close) {
    close->Finish(
        checkpoint_io_->close
            ? checkpoint_io_->close->data_status.load(std::memory_order_acquire)
            : X_STATUS_UNSUCCESSFUL);
  }
  if (checkpoint_io_) {
    std::lock_guard lock(kernel_state()->file_io_mutex_);
    checkpoint_io_->closing = false;
    checkpoint_io_->closed = true;
    // All queued owners have released before refcount-zero close; successful
    // FinishOverwrite has now returned too. A future full-tree barrier owns
    // durability, so historical successful IO adds no outstanding duty. Old
    // captured cuts independently retain this exact state/close observation.
    if (!checkpoint_io_->lost && checkpoint_io_->unsettled.empty() &&
        !checkpoint_io_->first_failure && checkpoint_io_->close &&
        XSUCCEEDED(checkpoint_io_->close->data_status.load(
            std::memory_order_acquire))) {
      std::erase(kernel_state()->file_io_records_, checkpoint_io_);
    }
  }
}

GuestScheduler::BlockingCallClass XFile::io_call_class() const {
  return device()->supports_concurrent_io()
             ? GuestScheduler::BlockingCallClass::kConcurrent
             : GuestScheduler::BlockingCallClass::kSerial;
}

HostCallOutcome XFile::RunSynchronousIoOwned(std::function<void()> fn) {
  return kernel_state()->RunBlockingIo(std::move(fn), io_call_class(), is_alertable_);
}

uint64_t XFile::position() const { return position_.load(); }

void XFile::set_position(uint64_t value) { position_.store(value); }

X_STATUS XFile::QueryDirectory(X_FILE_DIRECTORY_INFORMATION* out_info,
                               size_t length, const std::string_view file_name,
                               bool restart) {
  // An I/O worker may already hold file_lock_ for a slow read.
  X_STATUS result = X_STATUS_SUCCESS;
  const auto invoked = RunSynchronousIo([&]() {
    result = QueryDirectoryInternal(out_info, length, file_name, restart);
  });
  if (invoked!=HostCallOutcome::kReturned) return HostCallFailureStatus(invoked);
  return result;
}

X_STATUS XFile::QueryDirectoryInternal(X_FILE_DIRECTORY_INFORMATION* out_info,
                                       size_t length,
                                       const std::string_view file_name,
                                       bool restart) {
  std::lock_guard<std::mutex> lock(file_lock_);
  assert_not_null(out_info);

  // Child selection, name/metadata capture and cached managed projection use
  // the same existing namespace interval. No native filesystem operation runs
  // here. The retained File keeps its complete Device/tree alive.
  auto namespace_lock = global_critical_region::AcquireDirect();
  vfs::Entry* entry = nullptr;

  if (!file_name.empty()) {
    // Only queries in the current directory are supported for now.
    assert_true(utf8::find_any_of(file_name, "\\") == std::string_view::npos);

    find_engine_.SetRule(file_name);

    // Always restart the search?
    find_index_ = 0;
    entry = file_->entry()->IterateChildren(find_engine_, &find_index_);
    if (!entry) {
      return X_STATUS_NO_SUCH_FILE;
    }
  } else {
    if (restart) {
      find_index_ = 0;
    }

    entry = file_->entry()->IterateChildren(find_engine_, &find_index_);
    if (!entry) {
      return X_STATUS_NO_MORE_FILES;
    }
  }

  auto end = reinterpret_cast<uint8_t*>(out_info) + length;
  const auto& entry_name = entry->name();
  if (reinterpret_cast<uint8_t*>(&out_info->file_name[0]) + entry_name.size() >
      end) {
    assert_always("Buffer overflow?");
    return X_STATUS_NO_SUCH_FILE;
  }

  // This optional projection is pure cached metadata, never a native
  // filesystem update() under the directory enumeration lock.
  entry->RefreshNamespaceMetadata();
  out_info->next_entry_offset = 0;
  out_info->file_index = static_cast<uint32_t>(find_index_);
  out_info->creation_time = entry->create_timestamp();
  out_info->last_access_time = entry->access_timestamp();
  out_info->last_write_time = entry->write_timestamp();
  out_info->change_time = entry->write_timestamp();
  out_info->end_of_file = entry->size();
  out_info->allocation_size = entry->allocation_size();
  out_info->attributes = entry->attributes();
  out_info->file_name_length = static_cast<uint32_t>(entry_name.size());
  std::memcpy(out_info->file_name, entry_name.data(), entry_name.size());

  return X_STATUS_SUCCESS;
}

X_STATUS XFile::Read(uint32_t buffer_guest_address, uint32_t buffer_length,
                     uint64_t byte_offset, uint32_t* out_bytes_read,
                     uint32_t apc_context, bool notify_completion, util::IoTransferReceipt* receipt) {
  // file_lock_ is taken inside the closure, on an I/O worker, so it is never
  // held while the calling fiber waits.
  // Booked before the offload, so requests queue in the order they are issued.
  const uint64_t deadline_ms = ReserveDriveTime(byte_offset, buffer_length);
  X_STATUS result = X_STATUS_SUCCESS;
  const auto invoked = RunSynchronousIo([&]() {
    std::lock_guard<std::mutex> lock(file_lock_);
    result = ReadInternal(buffer_guest_address, buffer_length, byte_offset,
                          out_bytes_read, apc_context, notify_completion, receipt);
  });
  if (receipt) { receipt->invocation = invoked; }
  AwaitDriveTime(deadline_ms);
  if (invoked!=HostCallOutcome::kReturned) return HostCallFailureStatus(invoked);
  return result;
}

X_STATUS XFile::ReadInternal(uint32_t buffer_guest_address,
                             uint32_t buffer_length, uint64_t byte_offset,
                             uint32_t* out_bytes_read, uint32_t apc_context,
                             bool notify_completion, util::IoTransferReceipt* receipt) {
  if (byte_offset == uint64_t(-1)) {
    // Read from current position.
    byte_offset = position_.load();
  }

  size_t bytes_read = 0;
  X_STATUS result = X_STATUS_SUCCESS;
  // Zero length means success for a valid file object according to Windows
  // tests.
  if (buffer_length) {
    if (UINT32_MAX - buffer_guest_address < buffer_length) {
      result = X_STATUS_ACCESS_VIOLATION;
    } else {
      // Games often read directly to texture/vertex buffer memory - in this
      // case, invalidation notifications must be sent. However, having any
      // memory callbacks in the range will result in STATUS_ACCESS_VIOLATION at
      // least on Windows, without anything being read or any callbacks being
      // triggered. So for physical memory, host protection must be bypassed,
      // and invalidation callbacks must be triggered manually (it's also wrong
      // to trigger invalidation callbacks before reading in this case, because
      // during the read, the guest may still access the data around the buffer
      // that is located in the same host pages as the buffer's start and end,
      // on the GPU - and that must not trigger a race condition).
      uint32_t buffer_guest_high_address =
          buffer_guest_address + buffer_length - 1;
      xe::BaseHeap* buffer_start_heap =
          memory()->LookupHeap(buffer_guest_address);
      const xe::BaseHeap* buffer_end_heap =
          memory()->LookupHeap(buffer_guest_high_address);
      if (!buffer_start_heap || !buffer_end_heap ||
          (buffer_start_heap->heap_type() == HeapType::kGuestPhysical) !=
              (buffer_end_heap->heap_type() == HeapType::kGuestPhysical) ||
          (buffer_start_heap->heap_type() == HeapType::kGuestPhysical &&
           buffer_start_heap != buffer_end_heap)) {
        result = X_STATUS_ACCESS_VIOLATION;
      } else {
        xe::PhysicalHeap* buffer_physical_heap =
            buffer_start_heap->heap_type() == HeapType::kGuestPhysical
                ? static_cast<xe::PhysicalHeap*>(buffer_start_heap)
                : nullptr;
        if (buffer_physical_heap &&
            buffer_physical_heap->QueryRangeAccess(buffer_guest_address,
                                                   buffer_guest_high_address) !=
                memory::PageAccess::kReadWrite) {
          result = X_STATUS_ACCESS_VIOLATION;
        } else {
          if (buffer_physical_heap) {
            // Resolve output is copied in whole pages, so the pages this read
            // only partly covers get it now, for the read to land over it.
            vfs::File::Information actual;
            result = file_->QueryInformation(&actual);
            if (XSUCCEEDED(result)) {
              buffer_physical_heap->ProvideReadWatchedEdgePages(
                  buffer_guest_address,
                  uint32_t(std::min<uint64_t>(
                      buffer_length, byte_offset < actual.end_of_file
                                         ? actual.end_of_file - byte_offset : 0)));
            }
          }
          // Query failure performs no read/physical output mutation, then
          // reaches the same once-only count/notification path below.
          if (XSUCCEEDED(result)) {
            const uint32_t prefix = receipt ? receipt->completed_prefix : 0;
            if (receipt) { receipt->EnterBackend(prefix); }
            result = file_->ReadSync(
                std::span<uint8_t>(
                    buffer_physical_heap
                        ? memory()->TranslatePhysical(
                              buffer_physical_heap->GetPhysicalAddress(
                                  buffer_guest_address))
                        : memory()->TranslateVirtual(buffer_guest_address),
                    buffer_length),
                size_t(byte_offset), &bytes_read);
            // Record the actual backend return before any physical callbacks.
            if (out_bytes_read) { *out_bytes_read = uint32_t(bytes_read); }
            if (receipt) { receipt->Returned(result, prefix + uint32_t(bytes_read)); }
            if (XSUCCEEDED(result)) {
              if (buffer_physical_heap) {
                // The read has already replaced the contents it covers, the rest
                // of the buffer keeps them.
                uint32_t read_length =
                    uint32_t(std::min(size_t(buffer_length), bytes_read));
                if (read_length) {
                  buffer_physical_heap->TriggerCallbacks(
                      xe::global_critical_region::AcquireDirect(),
                      buffer_guest_address, read_length, true, true, true, false,
                      true);
                }
                if (read_length < buffer_length) {
                  buffer_physical_heap->TriggerCallbacks(
                      xe::global_critical_region::AcquireDirect(),
                      buffer_guest_address + read_length,
                      buffer_length - read_length, true, true);
                }
              }

              position_.store(byte_offset + bytes_read);
            }
          }
        }
      }
    }
  }

  if (out_bytes_read) {
    *out_bytes_read = uint32_t(bytes_read);
  }

  if (notify_completion) {
    if (NotifyCompletion(result, uint32_t(bytes_read), apc_context) !=
        HostCallOutcome::kReturned) { return X_STATUS_UNSUCCESSFUL; }
  }

  return result;
}

void XFile::PostIo(XThread *thread, std::function<void(bool cancelled)> fn,
                   std::function<void()> on_drop,
                   std::function<void()> on_resource_failure,
                   std::function<X_STATUS()> observed_result, bool may_mutate) {
  // Read-only requests still own the same FIFO/runner/File lifetime. They
  // introduce no persistent-byte generation; the genuine queued flush joins
  // them before a package checkpoint may complete.
  PostIoInternal(thread, std::move(fn), std::move(on_drop),
                 std::move(on_resource_failure), std::move(observed_result),
                 may_mutate);
}
void XFile::PostIoInternal(XThread* thread, std::function<void(bool)> fn,
                           std::function<void()> on_drop,
                           std::function<void()> on_resource_failure,
                           std::function<X_STATUS()> observed_result,
                           bool track_checkpoint,
                           std::shared_ptr<PostedIoRelease> release) {
  bool run = false;
  const auto prepared = InvokeHostCall([&] {
    // Keep the actual failure owner here if constructing/pushing the queue
    // entry fails before commit. std::function moves themselves are noexcept.
    PostedIo item{thread,
                  std::move(fn),
                  std::move(on_drop),
                  on_resource_failure,
                  std::move(observed_result),
                  track_checkpoint ? BeginCheckpointIo() : nullptr,
                  std::move(release)};
    std::lock_guard<std::mutex> lock(posted_lock_);
    posted_.push_back(std::move(item));
    if (!posted_running_) { posted_running_ = true; run = true; }
  });
  if (prepared != HostCallOutcome::kReturned) {
    if (on_resource_failure) {
      if (InvokeHostCall(on_resource_failure) != HostCallOutcome::kReturned) {
        (void)InvokeHostCall([] { XELOGE("XFile: resource rejection owner uncertain"); });
      }
    } else { (void)InvokeHostCall([] { XELOGE("XFile: unstarted legacy request resource admission failed"); }); }
    return;
  }
  if (!run) { return; }
  bool accepted = false;
  const auto submitted = InvokeHostCall([&] {
    // Both scheduler alternatives own ONE actual File reference. The taken
    // route withdraws it before any release-join callbacks; the unused drop
    // alternative cannot secretly postpone final File destruction.
    auto runner = std::make_shared<object_ref<XFile>>(retain_object(this));
    accepted = kernel_state()->guest_scheduler()->PostHostCall(
        [runner]() {
          auto file = std::move(*runner);
          auto released = file->RunPostedIo();
          file.reset();
          CompleteReleased(std::move(released));
        },
        io_call_class(),
        [runner](GuestScheduler::HostCallDropReason reason) {
          auto file = std::move(*runner);
          auto released = file->DropPostedIo(reason);
          file.reset();
          CompleteReleased(std::move(released));
        });
  });
  if (submitted != HostCallOutcome::kReturned || !accepted) { FailPostedIoAdmission(); }
}

void XFile::FailPostedIoAdmission() {
  std::deque<PostedIo> failed;
  {
    std::lock_guard<std::mutex> lock(posted_lock_);
    failed.swap(posted_);
    posted_running_thread_ = nullptr;
    posted_executing_ = false;
    posted_running_ = false;
  }
  for (auto& posted : failed) {
    // This runner never entered the scheduler queue. Only the real producer's
    // resource-failure owner may settle it; never call complete(true)/on_drop.
    if (posted.on_resource_failure) {
      if (InvokeHostCall(posted.on_resource_failure) != HostCallOutcome::kReturned) {
        (void)InvokeHostCall([] { XELOGE("XFile: resource failure owner uncertain; continuing remaining owners"); });
      }
    } else { (void)InvokeHostCall([] { XELOGE("XFile: unstarted legacy runner resource admission failed"); }); }
    X_STATUS observed = X_STATUS_INSUFFICIENT_RESOURCES;
    if (posted.observed_result && InvokeHostCall([&] {
                                    observed = posted.observed_result();
                                  }) != HostCallOutcome::kReturned) {
      observed = X_STATUS_UNSUCCESSFUL;
    }
    if (posted.checkpoint) {
      posted.checkpoint->Finish(observed);
    }
  }
}

void XFile::CompleteReleased(std::shared_ptr<PostedIoRelease> released) {
  while (released) {
    auto item = std::move(released);
    released = std::move(item->next);
    (void)InvokeHostCall(item->complete);
  }
}
std::shared_ptr<XFile::PostedIoRelease> XFile::RunPostedIo(
    bool admission_shutdown) {
  std::shared_ptr<PostedIoRelease> released;
  PostedIoRelease* tail = nullptr;
  std::unique_lock<std::mutex> lock(posted_lock_);
  try {
    while (!posted_.empty()) {
      auto posted = std::move(posted_.front());
      posted_.pop_front();
      posted_running_thread_ = posted.thread;
      posted_executing_ = true;
      lock.unlock();
      const auto invoked = InvokeHostCall([&] {
        if (admission_shutdown && posted.on_drop) {
          posted.on_drop();
        } else { posted.complete(false); }
      });
      if (invoked != HostCallOutcome::kReturned) {
        (void)InvokeHostCall([] { XELOGE("XFile: posted request escaped its completion owner"); });
      }
      X_STATUS observed = invoked == HostCallOutcome::kReturned
                              ? X_STATUS_SUCCESS
                              : HostCallFailureStatus(invoked);
      if (invoked == HostCallOutcome::kReturned && posted.observed_result &&
          InvokeHostCall([&] { observed = posted.observed_result(); }) !=
              HostCallOutcome::kReturned) {
        observed = X_STATUS_UNSUCCESSFUL;
      }
      if (posted.checkpoint) {
        posted.checkpoint->Finish(observed);
      }
      lock.lock();
      posted_running_thread_ = nullptr;
      posted_executing_ = false;
      // Destroy authority-bearing captures outside the queue lock.
      lock.unlock();
      posted.complete = {};
      posted.on_drop = {};
      posted.on_resource_failure = {};
      posted.observed_result = {};
      if (posted.release) {
        // Already allocated at real barrier admission: no allocation, File
        // capture destruction or completion callback under posted_lock_.
        if (tail) {
          tail->next = posted.release;
        } else {
          released = posted.release;
        }
        tail = posted.release.get();
      }
      lock.lock();
    }
    posted_running_ = false;
  } catch (...) {
    // Only control-flow sentinels should reach this path. The current request
    // has already started and must never be replayed as on_drop/cancelled.
    // Pending FIFO entries stay intact for shutdown or a later real runner.
    if (!lock.owns_lock()) { lock.lock(); }
    posted_running_thread_ = nullptr;
    posted_executing_ = false;
    posted_running_ = false;
    throw;
  }
  return released;
}

std::shared_ptr<XFile::PostedIoRelease> XFile::DropPostedIo(
    GuestScheduler::HostCallDropReason reason) {
  if (reason == GuestScheduler::HostCallDropReason::kAdmissionShutdown) {
    // Keep the existing inline queue serialization for legacy after-stop
    // submissions, while explicit owned barriers receive only cancellation.
    return RunPostedIo(true);
  }
  std::deque<PostedIo> dropped;
  {
    std::lock_guard<std::mutex> lock(posted_lock_);
    dropped.swap(posted_);
    posted_running_thread_ = nullptr;
    posted_executing_ = false;
    posted_running_ = false;
  }
  // Scheduler invokes this only for an unstarted runner. In-flight callbacks
  // finish normally before Shutdown joins their workers. Legacy callbacks are
  // destroyed, never invoked against possibly expired guest memory.
  for (auto& posted : dropped) {
    const auto invoked = posted.on_drop ? InvokeHostCall(posted.on_drop)
                                        : HostCallOutcome::kReturned;
    X_STATUS observed = invoked == HostCallOutcome::kReturned
                            ? X_STATUS_CANCELLED
                            : HostCallFailureStatus(invoked);
    if (invoked == HostCallOutcome::kReturned && posted.observed_result &&
        InvokeHostCall([&] { observed = posted.observed_result(); }) !=
            HostCallOutcome::kReturned) {
      observed = X_STATUS_UNSUCCESSFUL;
    }
    if (posted.checkpoint) {
      posted.checkpoint->Finish(observed);
    }
    if (invoked != HostCallOutcome::kReturned) {
      (void)InvokeHostCall([] { XELOGE("XFile: unstarted drop owner uncertain; continuing remaining owners"); });
    }
  }
  return {};
}

void XFile::CancelPostedIo(XThread* thread) {
  std::vector<PostedIo> cancelled;
  std::shared_ptr<PostedIoRelease> released;
  PostedIoRelease* tail = nullptr;
  {
    std::lock_guard<std::mutex> lock(posted_lock_);
    for (auto it = posted_.begin(); it != posted_.end();) {
      if (it->thread == thread) {
        cancelled.push_back(std::move(*it));
        it = posted_.erase(it);
      } else {
        ++it;
      }
    }
  }
  for (auto& posted : cancelled) {
    const auto invoked = InvokeHostCall([&] { posted.complete(true); });
    X_STATUS observed = invoked == HostCallOutcome::kReturned
                            ? X_STATUS_CANCELLED
                            : HostCallFailureStatus(invoked);
    if (invoked == HostCallOutcome::kReturned && posted.observed_result &&
        InvokeHostCall([&] { observed = posted.observed_result(); }) !=
            HostCallOutcome::kReturned) {
      observed = X_STATUS_UNSUCCESSFUL;
    }
    if (posted.checkpoint) {
      posted.checkpoint->Finish(observed);
    }
    if (invoked != HostCallOutcome::kReturned) {
      (void)InvokeHostCall([] { XELOGE("XFile: cancellation owner uncertain; continuing remaining owners"); });
    }
    if (posted.release) {
      if (tail) {
        tail->next = posted.release;
      } else {
        released = posted.release;
      }
      tail = posted.release.get();
    }
  }
  // The genuine cancellation body has returned; no queue/file/registry guard
  // is held. The same once-only arrival handles cancellation and real return.
  CompleteReleased(std::move(released));
}

bool XFile::HasPostedIo(XThread* thread) {
  std::lock_guard<std::mutex> lock(posted_lock_);
  if (posted_executing_ && posted_running_thread_ == thread) {
    return true;
  }
  for (const auto& posted : posted_) {
    if (posted.thread == thread) {
      return true;
    }
  }
  return false;
}

uint64_t XFile::ReserveDriveTime(uint64_t byte_offset, uint32_t length) {
  // An async completion runs on a shared I/O worker, which must not block.
  if (GuestScheduler::CurrentThreadIsBlockingCallWorker()) {
    return 0;
  }
  // A caller holding the global lock cannot release it to wait.
  if (xe::global_critical_region::is_held_by_current_thread()) {
    return 0;
  }
  // Neither of these reaches the medium, and both are common size probes.
  if (!length) {
    return 0;
  }
  const uint64_t offset =
      byte_offset == uint64_t(-1) ? position_.load() : byte_offset;
  if (offset >= file_->entry()->size()) {
    return 0;
  }
  return device()->drive_timing().Reserve(file_->entry(), offset, length);
}

void XFile::AwaitDriveTime(uint64_t deadline_ms) {
  if (!deadline_ms) {
    return;
  }
  // Null off a fiber, and GetCurrentThread would assert there.
  XThread* self = XThread::GetCurrentFiberThread();
  if (!self) {
    // Without fibers this is the guest thread, so blocking it is faithful.
    const uint64_t now = Clock::QueryHostUptimeMillis();
    if (now < deadline_ms) {
      threading::Sleep(std::chrono::milliseconds(deadline_ms - now));
    }
    return;
  }
  // The deadline is host time, so it must not pass through a guest-duration
  // API like XThread::Delay, which scales by the guest time scalar.
  auto* scheduler = kernel_state()->guest_scheduler();
  self->set_cooperative_wait_shape(XThread::CooperativeWaitKind::kDelay,
                                   nullptr, 0);
  while (Clock::QueryHostUptimeMillis() < deadline_ms) {
    // A kernel APC runs during the wait, which then goes on.
    if (self->HasDeliverableKernelApc()) {
      self->clear_cooperative_wait_shape();
      xboxkrnl::xeProcessKernelApcs(self->thread_state()->context());
      self->set_cooperative_wait_shape(XThread::CooperativeWaitKind::kDelay,
                                       nullptr, 0);
      continue;
    }
    scheduler->BlockCurrentThread(deadline_ms, 0, false);
  }
  self->clear_cooperative_wait_shape();
}

X_STATUS XFile::ReadScatter(uint32_t segments_guest_address, uint32_t length,
                            uint64_t byte_offset, uint32_t* out_bytes_read,
                            uint32_t apc_context, bool notify_completion, util::IoTransferReceipt* receipt) {
  // The whole loop as one request, so the fiber waits once.
  const uint64_t deadline_ms = ReserveDriveTime(byte_offset, length);
  X_STATUS result = X_STATUS_SUCCESS;
  const auto invoked = RunSynchronousIo([&]() {
    result =
        ReadScatterInternal(segments_guest_address, length, byte_offset,
                            out_bytes_read, apc_context, notify_completion, receipt);
  });
  if (receipt) { receipt->invocation = invoked; }
  AwaitDriveTime(deadline_ms);
  if (invoked!=HostCallOutcome::kReturned) return HostCallFailureStatus(invoked);
  return result;
}

X_STATUS XFile::ReadScatterInternal(uint32_t segments_guest_address,
                                    uint32_t length, uint64_t byte_offset,
                                    uint32_t* out_bytes_read,
                                    uint32_t apc_context,
                                    bool notify_completion, util::IoTransferReceipt* receipt) {
  std::lock_guard<std::mutex> lock(file_lock_);
  X_STATUS result = X_STATUS_SUCCESS;

  // segments points to an array of buffer pointers of type
  // "FILE_SEGMENT_ELEMENT", but they can just be treated as normal pointers
  xe::be<uint32_t>* segments = reinterpret_cast<xe::be<uint32_t>*>(
      memory()->TranslateVirtual(segments_guest_address));

  // TODO: not sure if this is meant to change depending on buffer address?
  // (only game seen using this always seems to use 4096-byte buffers)
  uint32_t page_size = 4096;

  uint32_t read_total = 0;
  uint32_t read_remain = length;
  while (read_remain) {
    uint32_t read_length = read_remain;
    uint32_t read_buffer = *segments;
    if (read_length > page_size) {
      read_length = page_size;
      segments++;
    }

    uint32_t bytes_read = 0;
    if (receipt) { receipt->completed_prefix = read_total; }
    result = ReadInternal(
        read_buffer, read_length,
        byte_offset != uint64_t(-1) ? byte_offset + read_total : byte_offset,
        &bytes_read, apc_context, false, receipt);

    if (result != X_STATUS_SUCCESS) {
      break;
    }

    read_total += bytes_read;
    read_remain -= read_length;
  }

  if (out_bytes_read) {
    *out_bytes_read = uint32_t(read_total);
  }

  if (notify_completion) {
    if (NotifyCompletion(result, read_total, apc_context) !=
        HostCallOutcome::kReturned) { return X_STATUS_UNSUCCESSFUL; }
  }

  return result;
}

X_STATUS XFile::Write(uint32_t buffer_guest_address, uint32_t buffer_length,
                      uint64_t byte_offset, uint32_t* out_bytes_written,
                      uint32_t apc_context, bool notify_completion, util::IoTransferReceipt* receipt,
                      std::shared_ptr<const vfs::FileWriteCheckpointAuthority> write_checkpoint) {
  auto checkpoint = BeginCheckpointIo();
  util::IoTransferReceipt local;
  if (!receipt) {
    receipt = &local;
  }
  X_STATUS result = X_STATUS_SUCCESS;
  const auto invoked = RunSynchronousIo([&, authority=std::move(write_checkpoint)]() mutable {
    if(!authority) {
      result=WriteInternal(buffer_guest_address,buffer_length,byte_offset,
          out_bytes_written,apc_context,notify_completion,receipt);return;
    }
    // IO22 owns this callable. Move the one-shot participant to THIS worker
    // stack and destroy it after all File locks, BEFORE any common notification
    // or RunBlockingIo done/event publication. Native memory stays native-owned.
    X_STATUS refusal=X_STATUS_ACCESS_DENIED;
    auto use=authority->AcquireForActualWrite(*file_,refusal);
    uint32_t actual_bytes=0;
    result=use?WriteInternal(buffer_guest_address,buffer_length,byte_offset,
        &actual_bytes,apc_context,false,receipt,use.get()):
        (XFAILED(refusal)?refusal:X_STATUS_INVALID_DEVICE_REQUEST);
    use.reset();
    if(out_bytes_written)*out_bytes_written=actual_bytes;
    if(notify_completion && NotifyCompletion(result,actual_bytes,apc_context)!=HostCallOutcome::kReturned)
      result=X_STATUS_UNSUCCESSFUL;
  });
  receipt->invocation = invoked;
  if (checkpoint) {
    // The request/notification result is not a mutation receipt. For example,
    // a rejected EOF query never calls WriteSync; a notification exception
    // after returned successful WriteSync does not undo or corrupt its bytes.
    checkpoint->Finish(!receipt->backend_entered ||
                               (receipt->persistent_effect_known &&
                                !receipt->persistent_effect_entered)
                           ? X_STATUS_SUCCESS
                       : receipt->backend_returned ? receipt->backend_status
                                                   : X_STATUS_UNSUCCESSFUL);
  }
  if (invoked!=HostCallOutcome::kReturned) return HostCallFailureStatus(invoked);
  return result;
}

X_STATUS XFile::WriteInternal(uint32_t buffer_guest_address,
                              uint32_t buffer_length, uint64_t byte_offset,
                              uint32_t* out_bytes_written, uint32_t apc_context,
                              bool notify_completion, util::IoTransferReceipt* receipt,
                              vfs::FileWriteCheckpointUse* write_checkpoint) {
  // Physical memory is read through the unprotected physical view, as a host
  // write from a watched page would fail rather than fault into its callbacks.
  // Those are triggered first as for a guest read, which brings in resolve
  // output guest RAM doesn't have yet. Outside the file lock, as this takes the
  // global one.
  uint8_t* buffer = memory()->TranslateVirtual(buffer_guest_address);
  if (!write_checkpoint && buffer_length && UINT32_MAX - buffer_guest_address >= buffer_length) {
    xe::BaseHeap* buffer_heap = memory()->LookupHeap(buffer_guest_address);
    if (buffer_heap && buffer_heap->heap_type() == HeapType::kGuestPhysical &&
        memory()->LookupHeap(buffer_guest_address + buffer_length - 1) ==
            buffer_heap) {
      auto buffer_physical_heap = static_cast<xe::PhysicalHeap*>(buffer_heap);
      buffer_physical_heap->TriggerCallbacks(
          xe::global_critical_region::AcquireDirect(), buffer_guest_address,
          buffer_length, false, true);
      buffer = memory()->TranslatePhysical(
          buffer_physical_heap->GetPhysicalAddress(buffer_guest_address));
    }
  }

  std::lock_guard<std::mutex> lock(file_lock_);
  std::optional<vfs::HostPathFile::CheckpointEffectScope> effect;
  if (checkpoint_io_ && receipt) {
    if (auto *host = dynamic_cast<vfs::HostPathFile *>(file_)) {
      effect.emplace(*host, receipt->persistent_effect_known,
                     receipt->persistent_effect_entered);
    }
  }
  X_STATUS result = X_STATUS_SUCCESS;
  if (byte_offset == uint64_t(-1)) {
    // Write from current position.
    byte_offset = position_.load();
  } else if (byte_offset == kEndOfFile) {
    // Observe THIS retained view under the existing file/IO ownership. A path
    // refresh cannot describe a managed session or a replaced-path old FD.
    vfs::File::Information actual;
    result = file_->QueryInformation(&actual);
    if (XSUCCEEDED(result)) { byte_offset = actual.end_of_file; }
  }

  size_t bytes_written = 0;
  if(XSUCCEEDED(result) && write_checkpoint) {
    result=write_checkpoint->ValidateTransfer(*file_,byte_offset,buffer_length);
    if(XSUCCEEDED(result) && write_checkpoint->input().size()!=buffer_length)result=X_STATUS_INVALID_PARAMETER;
  }
  if (XFAILED(result) && write_checkpoint) { write_checkpoint->TransferRefused(result); }
  if (XSUCCEEDED(result)) {
    if (receipt) { receipt->EnterBackend(); }
    const std::span<const uint8_t> input=write_checkpoint?write_checkpoint->input():
        std::span<const uint8_t>(buffer,buffer_length);
    result = file_->WriteSync(input,size_t(byte_offset),&bytes_written);
    if (receipt) { receipt->Returned(result, uint32_t(bytes_written)); }
    // The receipt remains the actual byte-backend result, separately from a
    // later real durability/model/cache participant's outcome.
    if(write_checkpoint)result=write_checkpoint->AfterTransfer(*file_,result,bytes_written);
    // A zero-length write leaves the position alone.
    if (XSUCCEEDED(result) && buffer_length) {
      position_.store(byte_offset + bytes_written);
    }
  }

  if (out_bytes_written) {
    *out_bytes_written = uint32_t(bytes_written);
  }

  if (notify_completion) {
    if (NotifyCompletion(result, uint32_t(bytes_written), apc_context) !=
        HostCallOutcome::kReturned) { return X_STATUS_UNSUCCESSFUL; }
  }
  return result;
}

X_STATUS XFile::Flush(util::IoTransferReceipt* receipt) {
  if (global_critical_region::is_held_by_current_thread()) {
    return X_STATUS_INVALID_DEVICE_REQUEST;
  }
  auto checkpoint = BeginCheckpointIo();
  util::IoTransferReceipt local;
  if (!receipt) {
    receipt = &local;
  }
  X_STATUS result = X_STATUS_NOT_SUPPORTED;
  const auto invoked = RunSynchronousIo([&]() {
    std::lock_guard<std::mutex> lock(file_lock_);
    std::optional<vfs::HostPathFile::CheckpointEffectScope> effect;
    if (checkpoint_io_) {
      if (auto *host = dynamic_cast<vfs::HostPathFile *>(file_)) {
        effect.emplace(*host, receipt->persistent_effect_known,
                       receipt->persistent_effect_entered);
      }
    }
    receipt->EnterBackend();
    result = file_->Flush();
    receipt->Returned(result, 0);
  });
  receipt->invocation = invoked;
  if (checkpoint) {
    checkpoint->Finish(!receipt->backend_entered ||
                               (receipt->persistent_effect_known &&
                                !receipt->persistent_effect_entered)
                           ? X_STATUS_SUCCESS
                       : receipt->backend_returned ? receipt->backend_status
                                                   : X_STATUS_UNSUCCESSFUL);
  }
  if (invoked != HostCallOutcome::kReturned)
    return HostCallFailureStatus(invoked);
  return result;
}

X_STATUS XFile::FlushQueued(std::function<void(X_STATUS)> completion) {
  return FlushQueuedInternal(std::move(completion), false);
}
X_STATUS XFile::FlushQueuedInternal(std::function<void(X_STATUS)> completion,
                                    bool release_join) {
  if (!completion) { return X_STATUS_INVALID_PARAMETER; }
  if (global_critical_region::is_held_by_current_thread()) {
    return X_STATUS_INVALID_DEVICE_REQUEST;
  }
  // Concurrent dispatch alone is not storage provenance: discs, null devices
  // and container readers also opt into it. Require actual HostPath objects.
  if (!dynamic_cast<vfs::HostPathFile*>(file_) ||
      !dynamic_cast<vfs::HostPathDevice*>(device()) ||
      !device()->supports_concurrent_io()) {
    return X_STATUS_NOT_SUPPORTED;
  }
  auto file = retain_object(this);
  auto thread = XThread::IsInThread()
      ? retain_object(XThread::GetCurrentThread()) : object_ref<XThread>{};
  const bool writable = (file_access() &
      (filesystem::FileAccess::kFileWriteData |
       filesystem::FileAccess::kFileAppendData |
       filesystem::FileAccess::kGenericWrite |
       filesystem::FileAccess::kGenericAll)) != 0;
  std::function<void(bool)> body;
  std::function<void()> dropped, resource_failure;
  std::shared_ptr<PostedIoRelease> released;
  std::shared_ptr<std::atomic<X_STATUS>> released_status;
  const auto prepared = InvokeHostCall([&] {
    if (release_join) {
      released_status =
          std::make_shared<std::atomic<X_STATUS>>(X_STATUS_UNSUCCESSFUL);
      released = std::make_shared<PostedIoRelease>();
      released->complete = [completion, released_status] {
        completion(released_status->load(std::memory_order_acquire));
      };
    }
    body = [file, thread, completion, writable,
            released_status](bool cancelled) {
      (void)thread;  // Own the actual issuer until completion/cancellation.
      X_STATUS status = X_STATUS_CANCELLED;
      if (!cancelled) {
        const auto invoked = InvokeHostCall([&] {
          std::lock_guard<std::mutex> lock(file->file_lock_);
          // Readonly handles have no logical overwrite obligation.
          status = writable ? file->file_->Flush() : X_STATUS_SUCCESS;
          if (XSUCCEEDED(status) && static_cast<vfs::HostPathFile*>(file->file_)
                                        ->HasPendingCheckpointClose()) {
            status = X_STATUS(0xC00000E8);
          }
        });
        if (invoked != HostCallOutcome::kReturned) {
          status = HostCallFailureStatus(invoked);
        }
      }
      // Batch completion can perform further host work/release references.
      // Never call it with file_lock_ or posted_lock_ owned.
      if (released_status) {
        released_status->store(status, std::memory_order_release);
      } else {
        completion(status);
      }
    };
    dropped = [completion]() { completion(X_STATUS_CANCELLED); };
    resource_failure = [completion]() { completion(X_STATUS_INSUFFICIENT_RESOURCES); };
  });
  if (prepared != HostCallOutcome::kReturned) {
    // No PostIo entry/commit occurred. The caller owns its one rejection.
    return X_STATUS_INSUFFICIENT_RESOURCES;
  }
  PostIoInternal(thread.get(), std::move(body), std::move(dropped),
                 std::move(resource_failure), {}, false, std::move(released));
  return X_STATUS_SUCCESS;
}

std::optional<vfs::File::DeviceControlLayout> XFile::DescribeDeviceControl(
    uint32_t code) const {
  return file_->DescribeDeviceControl(code);
}

X_STATUS XFile::DeviceControl(uint32_t code, uint32_t input_address,
                              uint32_t input_length, uint32_t output_address,
                              uint32_t output_length, uint32_t* transferred,
                              util::IoTransferReceipt* receipt) {
  *transferred = 0;
  if (global_critical_region::is_held_by_current_thread()) {
    return X_STATUS_INVALID_DEVICE_REQUEST;
  }
  const auto layout = DescribeDeviceControl(code);
  if (!layout) {
    return X_STATUS_INVALID_DEVICE_REQUEST;
  }
  if (input_length != layout->input_bytes) {
    return X_STATUS_INVALID_PARAMETER;
  }
  if (output_length < layout->output_bytes) {
    return X_STATUS_BUFFER_TOO_SMALL;
  }
  X_STATUS result = file_->CheckDeviceControlAccess(code);
  if (XFAILED(result)) {
    return result;
  }
  // Layouts are trusted backend declarations, not guest allocation requests.
  // No current supported packet exceeds16 bytes.
  std::vector<uint8_t> input(layout->input_bytes), output(layout->output_bytes);
  const auto invoked = RunSynchronousIo([&]() {
    if (!util::GuestIoRangeValid(*memory(), input_address, input_length,
                                 false) ||
        !util::GuestIoRangeValid(*memory(), output_address, output_length,
                                 true)) {
      result = X_STATUS_ACCESS_VIOLATION;
      return;
    }
    result = util::ReadGuestIoBuffer(*memory(), input_address, input);
    if (XFAILED(result)) {
      return;
    }
    uint32_t count = 0;
    {
      std::lock_guard<std::mutex> lock(file_lock_);
      std::optional<vfs::HostPathFile::CheckpointEffectScope> effect;
      if (checkpoint_io_ && receipt) {
        if (auto *host = dynamic_cast<vfs::HostPathFile *>(file_)) {
          effect.emplace(*host, receipt->persistent_effect_known,
                         receipt->persistent_effect_entered);
        }
      }
      if (receipt) { receipt->EnterBackend(); }
      result = file_->DeviceControl(code, input, output, &count);
      if (receipt) { receipt->Returned(result, count); }
    }
    if (XFAILED(result)) {
      return;
    }
    if (count > output.size()) {
      result = X_STATUS_INVALID_DEVICE_REQUEST;
      return;
    }
    result = util::WriteGuestIoBuffer(*memory(), output_address,
                                      std::span(output).first(count));
    if (XSUCCEEDED(result)) {
      *transferred = count;
    }
  });
  if (receipt) { receipt->invocation = invoked; }
  if (invoked!=HostCallOutcome::kReturned) return HostCallFailureStatus(invoked);
  return result;
}

X_STATUS XFile::QueryInformation(vfs::File::Information* output) {
  if (!output) return X_STATUS_INVALID_PARAMETER;
  if (global_critical_region::is_held_by_current_thread()) {
    return X_STATUS_INVALID_DEVICE_REQUEST;
  }
  X_STATUS result = X_STATUS_CANCELLED;
  vfs::File::Information observed{};
  const auto invoked = RunSynchronousIo([&] {
    std::lock_guard<std::mutex> lock(file_lock_);
    result = file_->QueryInformation(&observed);
  });
  if (invoked!=HostCallOutcome::kReturned) return HostCallFailureStatus(invoked);
  if (XSUCCEEDED(result)) *output = observed;
  return result;
}

X_STATUS XFile::SetAllocationSize(uint64_t length) {
  X_STATUS result=X_STATUS_CANCELLED;
  const auto invoked=RunSynchronousIo([&] {
    std::lock_guard<std::mutex> lock(file_lock_);
    result=file_->SetAllocationSize(length);
  });
  return invoked==HostCallOutcome::kReturned ? result : HostCallFailureStatus(invoked);
}
X_STATUS XFile::SetLength(size_t length) {
  auto checkpoint = BeginCheckpointIo();
  bool effect_known = false, effect_entered = false, backend_called = false;
  X_STATUS result = X_STATUS_SUCCESS;
  const auto invoked = RunSynchronousIo([&]() {
    std::lock_guard<std::mutex> lock(file_lock_);
    std::optional<vfs::HostPathFile::CheckpointEffectScope> effect;
    if (checkpoint_io_) {
      if (auto *host = dynamic_cast<vfs::HostPathFile *>(file_)) {
        effect.emplace(*host, effect_known, effect_entered);
      }
    }
    backend_called = true;
    result = file_->SetLength(length);
  });
  if (checkpoint) {
    checkpoint->Finish(!backend_called || (effect_known && !effect_entered)
                           ? X_STATUS_SUCCESS
                       : invoked == HostCallOutcome::kReturned
                           ? result
                           : HostCallFailureStatus(invoked));
  }
  if (invoked != HostCallOutcome::kReturned)
    return HostCallFailureStatus(invoked);
  return result;
}

X_STATUS XFile::SetTimes(uint64_t create_timestamp, uint64_t access_timestamp,
                         uint64_t write_timestamp) {
  auto checkpoint = BeginCheckpointIo();
  bool effect_known = false, effect_entered = false, backend_called = false;
  X_STATUS result = X_STATUS_SUCCESS;
  const auto invoked = RunSynchronousIo([&]() {
    std::lock_guard<std::mutex> lock(file_lock_);
    std::optional<vfs::HostPathFile::CheckpointEffectScope> effect;
    if (checkpoint_io_) {
      if (auto *host = dynamic_cast<vfs::HostPathFile *>(file_)) {
        effect.emplace(*host, effect_known, effect_entered);
      }
    }
    backend_called = true;
    result =
        file_->SetTimes(create_timestamp, access_timestamp, write_timestamp);
  });
  if (checkpoint) {
    checkpoint->Finish(!backend_called || (effect_known && !effect_entered)
                           ? X_STATUS_SUCCESS
                       : invoked == HostCallOutcome::kReturned
                           ? result
                           : HostCallFailureStatus(invoked));
  }
  if (invoked != HostCallOutcome::kReturned)
    return HostCallFailureStatus(invoked);
  return result;
}

X_STATUS XFile::Rename(const std::filesystem::path file_path,
                       bool replace_existing) {
  auto checkpoint = BeginCheckpointIo();
  bool effect_entered = false;
  X_STATUS result = X_STATUS_SUCCESS;
  const auto invoked = RunSynchronousIo([&]() {
    result = kernel_state()->file_system()->RenameEntry(
        entry(), xe::path_to_utf8(file_path), replace_existing,
        file_->storage_lease(), &effect_entered);
  });
  if (checkpoint) {
    checkpoint->Finish(!effect_entered ? X_STATUS_SUCCESS
                       : invoked == HostCallOutcome::kReturned
                           ? result
                           : HostCallFailureStatus(invoked));
  }
  if (invoked != HostCallOutcome::kReturned)
    return HostCallFailureStatus(invoked);
  return result;
}

bool XFile::RegisterIOCompletionPort(uint32_t key,
                                     object_ref<XIOCompletion> port) {
  std::lock_guard<std::mutex> lock(completion_port_lock_);
  if (!completion_ports_.empty()) {
    return false;
  }
  completion_ports_.push_back({key, port});
  return true;
}

bool XFile::HasCompletionPort() {
  std::lock_guard<std::mutex> lock(completion_port_lock_);
  return !completion_ports_.empty();
}

void XFile::RemoveIOCompletionPort(uint32_t key) {
  std::lock_guard<std::mutex> lock(completion_port_lock_);

  for (auto it = completion_ports_.begin(); it != completion_ports_.end();
       it++) {
    if (it->first == key) {
      completion_ports_.erase(it);
      break;
    }
  }
}

bool XFile::Save(ByteStream* stream) {
  // XELOGD("XFile {:08X} ({})", handle(),
  //        file_->entry()->absolute_path().c_str());

  if (!SaveObject(stream)) {
    return false;
  }

  stream->Write(std::string_view(file_->entry()->absolute_path()));
  stream->Write<uint64_t>(position_);
  stream->Write(file_access());
  stream->Write<bool>(
      (file_->entry()->attributes() & vfs::kFileAttributeDirectory) != 0);
  stream->Write<bool>(is_synchronous_);
  stream->Write<bool>(is_buffered_);

  return true;
}

object_ref<XFile> XFile::Restore(KernelState* kernel_state,
                                 ByteStream* stream) {
  auto file = new XFile();
  file->kernel_state_ = kernel_state;
  if (!file->RestoreObject(stream)) {
    delete file;
    return nullptr;
  }

  auto abs_path = stream->Read<std::string>();
  uint64_t position = stream->Read<uint64_t>();
  auto access = stream->Read<uint32_t>();
  auto is_directory = stream->Read<bool>();
  auto is_synchronous = stream->Read<bool>();
  auto is_buffered = stream->Read<bool>();

  // XELOGD("XFile {:08X} ({})", file->handle(), abs_path);

  vfs::File* vfs_file = nullptr;
  vfs::FileAction action;
  auto res = kernel_state->file_system()->OpenFile(
      nullptr, abs_path, vfs::FileDisposition::kOpen, access, is_directory,
      false, &vfs_file, &action);
  if (XFAILED(res)) {
    // XELOGE("Failed to open XFile: error {:08X}", res);
    return object_ref<XFile>(file);
  }

  file->file_ = vfs_file;
  file->position_ = position;
  file->is_synchronous_ = is_synchronous;
  file->is_buffered_ = is_buffered;
  if (InvokeHostCall([&] { file->RegisterCheckpointIo(); }) !=
      HostCallOutcome::kReturned) {
    std::lock_guard lock(kernel_state->file_io_mutex_);
    kernel_state->file_io_overflow_ = true;
  }

  return object_ref<XFile>(file);
}

HostCallOutcome XFile::NotifyCompletion(
    X_STATUS status, uint32_t num_bytes, uint32_t apc_context,
    util::IoNotificationReceipt* receipt) {
  util::IoNotificationReceipt local;
  if (!receipt) { receipt = &local; }
  if (apc_context) {
    XIOCompletion::IONotification notify;
    notify.apc_context = apc_context;
    notify.num_bytes = num_bytes;
    notify.status = status;
    receipt->ports = InvokeHostCall([&] {
      if (NotifyIOCompletionPorts(notify, receipt) != HostCallOutcome::kReturned) {
        receipt->ports = HostCallOutcome::kExecutionFailed;
      }
    });
    // The snapshot may return normally after one or more per-port failures.
    if (receipt->ports_attempted != receipt->ports_returned) {
      receipt->ports = HostCallOutcome::kExecutionFailed;
    }
  }
  receipt->file_event = InvokeHostCall([&] { async_event_->Set(); });
  return receipt->Failed() ? HostCallOutcome::kExecutionFailed
                           : HostCallOutcome::kReturned;
}

HostCallOutcome XFile::NotifyIOCompletionPorts(
    XIOCompletion::IONotification& notification,
    util::IoNotificationReceipt* receipt) {
  std::vector<std::pair<uint32_t, object_ref<XIOCompletion>>> ports;
  {
    std::lock_guard<std::mutex> lock(completion_port_lock_);
    ports.assign(completion_ports_.begin(), completion_ports_.end());
  }
  bool failed = false;
  for (const auto& port : ports) {
    notification.key_context = port.first;
    ++receipt->ports_attempted;
    const auto invoked = InvokeHostCall([&] {
      port.second->QueueNotification(notification);
    });
    if (invoked == HostCallOutcome::kReturned) {
      ++receipt->ports_returned;
    } else { failed = true; }
  }
  return failed ? HostCallOutcome::kExecutionFailed : HostCallOutcome::kReturned;
}

}  // namespace kernel
}  // namespace xe
