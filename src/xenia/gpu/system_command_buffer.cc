/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia contributors. All rights reserved.                    *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/gpu/system_command_buffer.h"

#include <algorithm>
#include <atomic>
#include <limits>
#include <utility>

namespace xe::gpu {
namespace {
std::atomic<uint64_t> next_state_epoch{1};
uint32_t Word(const SystemCommandDescriptor& descriptor, size_t offset) {
  const auto* p = descriptor.bytes.data() + offset;
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
         (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}
bool ValidLeases(const GpuCompletionResources& leases) {
  return !leases.empty() &&
         std::all_of(leases.begin(), leases.end(),
                     [](const auto& lease) { return bool(lease); });
}
}  // namespace

std::optional<SystemCommandDescriptor> SystemCommandDescriptor::Copy(
    std::span<const uint8_t> source) {
  if (source.size() != kSize) {
    return {};
  }
  SystemCommandDescriptor result;
  std::copy(source.begin(), source.end(), result.bytes.begin());
  return result;
}
bool SystemCommandDescriptor::empty() const {
  return std::all_of(bytes.begin(), bytes.end(),
                     [](uint8_t byte) { return !byte; });
}
uint32_t SystemCommandDescriptor::command_address() const {
  return Word(*this, 0);
}
uint32_t SystemCommandDescriptor::command_dwords() const {
  return Word(*this, 4);
}
uint32_t SystemCommandDescriptor::texture_address() const {
  return Word(*this, 8);
}
uint32_t SystemCommandDescriptor::texture_format() const {
  return Word(*this, 0xC);
}
uint32_t SystemCommandDescriptor::width() const { return Word(*this, 0x14); }
uint32_t SystemCommandDescriptor::height() const { return Word(*this, 0x18); }
bool SystemCommandDescriptor::has_texture() const {
  return texture_address() || texture_format() || width() || height();
}

struct SystemCommandBuffer::Session {
  OwnedSession pins;
};
struct SystemCommandBuffer::Publication {
  SystemCommandDescriptor descriptor;
  uint64_t generation;
  std::shared_ptr<Session> session;
  GpuCompletionResources texture_memory;
};
struct SystemCommandBuffer::SwapUse {
  uint64_t owner_epoch;
  uint64_t use_id;
  uint32_t identifier;
  std::shared_ptr<const Publication> publication;
  GpuCompletion completion;
  bool attached = false;
};
struct SystemCommandBuffer::CheckoutEntry {
  uint32_t identifier;
  IdentifierBinding binding;
  std::shared_ptr<const Publication> publication;
  bool ever_queued = false;
  bool failed = false;
  std::vector<std::shared_ptr<SwapUse>> uses;
};

const SystemCommandDescriptor& SystemCommandBuffer::SwapLease::descriptor()
    const {
  return use_->publication->descriptor;
}
uint32_t SystemCommandBuffer::SwapLease::identifier() const {
  return use_->identifier;
}
uint64_t SystemCommandBuffer::SwapLease::owner_epoch() const {
  return use_->owner_epoch;
}
uint64_t SystemCommandBuffer::SwapLease::use_id() const { return use_->use_id; }
uint64_t SystemCommandBuffer::SwapLease::publication_generation() const {
  return use_->publication->generation;
}
GpuCompletionResources SystemCommandBuffer::SwapLease::resources() const {
  auto resources = use_->publication->session->pins.recording_memory;
  resources.push_back(use_->publication->session->pins.device_lifetime);
  const auto& texture = use_->publication->texture_memory;
  resources.insert(resources.end(), texture.begin(), texture.end());
  return resources;
}

SystemCommandBuffer::SystemCommandBuffer()
    : epoch_(next_state_epoch.fetch_add(1, std::memory_order_relaxed)) {}

bool SystemCommandBuffer::BeginOwnedSession(OwnedSession session) {
  if (!session.device_lifetime || !ValidLeases(session.recording_memory) ||
      !session.backend_epoch) {
    return false;
  }
  auto next = std::make_shared<Session>(Session{std::move(session)});
  std::shared_ptr<const Publication> previous;
  {
    std::lock_guard lock(mutex_);
    if (session_ || !checkouts_.empty() || exhausted_) {
      return false;
    }
    session_ = std::move(next);
    previous = std::move(publication_);
    publication_ = std::make_shared<Publication>(
        Publication{{}, next_publication_++, session_, {}});
  }
  return true;
}

void SystemCommandBuffer::CancelUnsubmittedLocked(Retired& retired) {
  for (auto it = checkouts_.begin(); it != checkouts_.end();) {
    if (!it->second->ever_queued) {
      retired.push_back(std::move(it->second));
      it = checkouts_.erase(it);
    } else {
      ++it;
    }
  }
}

void SystemCommandBuffer::CloseOwnedSession() {
  Retired retired;
  std::shared_ptr<Session> previous_session;
  std::shared_ptr<const Publication> previous_publication;
  {
    std::lock_guard lock(mutex_);
    previous_session = std::move(session_);
    previous_publication = std::move(publication_);
    ++binding_.generation;
    binding_.address = 0;
    CancelUnsubmittedLocked(retired);
  }
}

bool SystemCommandBuffer::Publish(SystemCommandDescriptor descriptor,
                                  GpuCompletionResources texture_memory) {
  if ((descriptor.has_texture() || !texture_memory.empty()) &&
      !ValidLeases(texture_memory)) {
    return false;
  }
  std::shared_ptr<const Publication> previous;
  {
    std::lock_guard lock(mutex_);
    if (!session_ || exhausted_ || !next_publication_) {
      return false;
    }
    auto next = std::make_shared<Publication>(
        Publication{std::move(descriptor), next_publication_++, session_,
                    std::move(texture_memory)});
    previous = std::move(publication_);
    publication_ = std::move(next);
  }
  return true;
}

SystemCommandBuffer::IdentifierBinding
SystemCommandBuffer::SetIdentifierAddress(uint32_t address) {
  Retired retired;
  IdentifierBinding result;
  {
    std::lock_guard lock(mutex_);
    binding_ = {address, binding_.generation + 1};
    result = binding_;
    CancelUnsubmittedLocked(retired);
  }
  return result;
}

SystemCommandBuffer::IdentifierBinding SystemCommandBuffer::identifier_binding()
    const {
  std::lock_guard lock(mutex_);
  return binding_;
}

void SystemCommandBuffer::PollLocked(const AckReader& reader,
                                     const AckWriter& writer,
                                     Retired& retired) {
  std::optional<uint32_t> observed;
  if (binding_.address && reader) {
    observed = reader(binding_.address);
    if (observed) {
      const auto found = checkouts_.find(*observed);
      if (found != checkouts_.end() && found->second->binding == binding_ &&
          !found->second->ever_queued) {
        retired.push_back(std::move(found->second));
        checkouts_.erase(found);
      }
    }
  }
  for (auto it = checkouts_.begin(); it != checkouts_.end();) {
    const auto& entry = it->second;
    bool complete = entry->ever_queued && !entry->failed;
    for (const auto& use : entry->uses) {
      if (!use->attached) {
        complete = false;
        continue;
      }
      const auto status = use->completion.status();
      if (status == GpuCompletionStatus::kFailed ||
          status == GpuCompletionStatus::kUnsupported) {
        entry->failed = true;
      }
      complete &= status == GpuCompletionStatus::kComplete;
    }
    complete &= !entry->failed;
    if (complete && entry->binding == binding_) {
      complete = binding_.address && observed && writer &&
                 writer(binding_.address, *observed, entry->identifier);
      if (complete) {
        observed = entry->identifier;
      }
    }
    // An obsolete binding may retire completed work, but never write to the
    // freed/replaced identifier address. Reset cannot cancel GPU-pending work.
    if (complete) {
      retired.push_back(std::move(it->second));
      it = checkouts_.erase(it);
    } else {
      ++it;
    }
  }
}

std::optional<SystemCommandBuffer::Checkout> SystemCommandBuffer::Get(
    const AckReader& reader, const AckWriter& writer) {
  Retired retired;
  std::optional<Checkout> result;
  {
    std::lock_guard lock(mutex_);
    PollLocked(reader, writer, retired);
    if (!session_ || !publication_ || exhausted_) {
      return {};
    }
    result = Checkout{publication_->descriptor, 0, publication_->generation,
                      binding_};
    if (!publication_->descriptor.empty()) {
      if (next_identifier_ > std::numeric_limits<uint32_t>::max()) {
        exhausted_ = true;
        return {};
      }
      result->identifier = uint32_t(next_identifier_++);
      auto entry = std::make_shared<CheckoutEntry>();
      entry->identifier = result->identifier;
      entry->binding = binding_;
      entry->publication = publication_;
      checkouts_.emplace(result->identifier, std::move(entry));
    }
  }
  return result;
}

SystemCommandBuffer::BindResult SystemCommandBuffer::BindSwap(
    uint32_t identifier, const SystemCommandDescriptor& supplied,
    uint32_t acknowledgement_address) {
  std::lock_guard lock(mutex_);
  if (!session_) {
    return {BindStatus::kNoOwnedSession, {}};
  }
  // A native skip can clear the Get copy before VdSwap. No work is executed
  // or ACK manufactured here; its CPU writeback is observed separately.
  if (supplied.empty()) {
    return {BindStatus::kEmpty, {}};
  }
  const auto found = checkouts_.find(identifier);
  if (found == checkouts_.end()) {
    return {BindStatus::kUnknownIdentifier, {}};
  }
  auto& entry = *found->second;
  for (const auto& use : entry.uses) {
    if (use->attached) {
      const auto status = use->completion.status();
      if (status == GpuCompletionStatus::kFailed ||
          status == GpuCompletionStatus::kUnsupported) {
        entry.failed = true;
      }
    }
  }
  if (entry.failed || exhausted_ || !next_use_) {
    return {BindStatus::kFailed, {}};
  }
  if (entry.publication->descriptor != supplied) {
    return {BindStatus::kDescriptorMismatch, {}};
  }
  if (!binding_.address || entry.binding != binding_ ||
      acknowledgement_address != binding_.address) {
    return {BindStatus::kStaleBinding, {}};
  }
  auto use = std::make_shared<SwapUse>(
      SwapUse{epoch_, next_use_++, identifier, entry.publication, {}, false});
  entry.uses.push_back(use);
  entry.ever_queued =
      true;  // Close CPU-skip admission before queueing GPU work.
  return {BindStatus::kBound, SwapLease(std::move(use))};
}

bool SystemCommandBuffer::AttachCompletion(const SwapLease& lease,
                                           GpuCompletion completion) {
  if (!lease.use_ || lease.use_->owner_epoch != epoch_) {
    return false;
  }
  std::lock_guard lock(mutex_);
  const auto found = checkouts_.find(lease.use_->identifier);
  if (found == checkouts_.end() || lease.use_->attached ||
      std::find(found->second->uses.begin(), found->second->uses.end(),
                lease.use_) == found->second->uses.end()) {
    return false;
  }
  const auto status = completion.status();
  if (status != GpuCompletionStatus::kUnsupported &&
      completion.backend_epoch() !=
          lease.use_->publication->session->pins.backend_epoch) {
    return false;
  }
  lease.use_->completion = std::move(completion);
  lease.use_->attached = true;
  if (status == GpuCompletionStatus::kFailed ||
      status == GpuCompletionStatus::kUnsupported) {
    found->second->failed = true;
  }
  return true;
}

void SystemCommandBuffer::Poll(const AckReader& reader,
                               const AckWriter& writer) {
  Retired retired;
  {
    std::lock_guard lock(mutex_);
    PollLocked(reader, writer, retired);
  }
}

bool SystemCommandBuffer::QueryReady(uint32_t mode, const AckReader& reader,
                                     const AckWriter& writer) {
  if (mode != 0) {
    return false;
  }
  Retired retired;
  std::lock_guard lock(mutex_);
  PollLocked(reader, writer, retired);
  return session_ && !exhausted_ && checkouts_.empty();
}

size_t SystemCommandBuffer::outstanding_checkouts() const {
  std::lock_guard lock(mutex_);
  return checkouts_.size();
}

void SystemCommandBuffer::ReleaseAfterGpuStopped() {
  decltype(checkouts_) retired;
  std::shared_ptr<Session> previous_session;
  std::shared_ptr<const Publication> previous_publication;
  {
    std::lock_guard lock(mutex_);
    retired.swap(checkouts_);
    previous_session = std::move(session_);
    previous_publication = std::move(publication_);
    binding_ = {0, binding_.generation + 1};
  }
}

}  // namespace xe::gpu
