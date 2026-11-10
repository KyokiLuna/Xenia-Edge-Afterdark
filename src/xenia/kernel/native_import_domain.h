/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia contributors. All rights reserved.                    *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */
#ifndef XENIA_KERNEL_NATIVE_IMPORT_DOMAIN_H_
#define XENIA_KERNEL_NATIVE_IMPORT_DOMAIN_H_

#include <cstddef>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <utility>

#include "xenia/cpu/guest_import_provider.h"

namespace xe::kernel {

// Binding/lifetime admission only, not native subsystem or task readiness.
// Kernel owns this record; modules hold weak membership to avoid a cycle
// through profile -> native root module -> domain. No guest addresses/objects
// are manufactured by this class.
class NativeImportDomain
    : public std::enable_shared_from_this<NativeImportDomain> {
 public:
  struct Member {
    uint32_t hmodule = 0;
    uint64_t generation = 0;
    bool operator==(const Member&) const = default;
  };
  class Operation {
   public:
    Operation() = default;
    Operation(Operation&& other) noexcept : domain_(std::move(other.domain_)) {}
    Operation& operator=(Operation&& other) noexcept {
      if (this != &other) {
        Reset();
        domain_ = std::move(other.domain_);
      }
      return *this;
    }
    ~Operation() { Reset(); }
    Operation(const Operation&) = delete;
    Operation& operator=(const Operation&) = delete;
    explicit operator bool() const { return bool(domain_); }

   private:
    friend class NativeImportDomain;
    explicit Operation(std::shared_ptr<NativeImportDomain> domain)
        : domain_(std::move(domain)) {}
    void Reset() {
      if (domain_) {
        std::lock_guard lock(domain_->mutex_);
        --domain_->operations_;
      }
      domain_.reset();
    }
    std::shared_ptr<NativeImportDomain> domain_;
  };

  static std::shared_ptr<NativeImportDomain> Create(
      uint64_t id, Member root,
      std::shared_ptr<const cpu::GuestImportProvider> profile) {
    if (!id || !root.hmodule || !root.generation || !profile ||
        profile->owner().hmodule != root.hmodule) {
      return {};
    }
    return std::shared_ptr<NativeImportDomain>(
        new NativeImportDomain(id, root, std::move(profile)));
  }
  uint64_t id() const { return id_; }
  Member root() const { return root_; }
  const std::shared_ptr<const cpu::GuestImportProvider>& profile() const {
    return profile_;
  }
  bool active() const {
    std::lock_guard lock(mutex_);
    return !retired_;
  }
  bool Contains(Member member) const {
    std::lock_guard lock(mutex_);
    const auto found = members_.find(member.hmodule);
    return found != members_.end() && found->second == member.generation;
  }
  bool Register(Member member) {
    std::lock_guard lock(mutex_);
    if (retired_ || !member.hmodule || !member.generation) {
      return false;
    }
    const auto [found, inserted] =
        members_.emplace(member.hmodule, member.generation);
    return inserted || found->second == member.generation;
  }
  void Unregister(Member member) {
    std::lock_guard lock(mutex_);
    if (member == root_) {
      return;
    }
    const auto found = members_.find(member.hmodule);
    if (found != members_.end() && found->second == member.generation) {
      members_.erase(found);
    }
  }
  Operation Acquire() {
    std::lock_guard lock(mutex_);
    if (retired_ || operations_ == std::numeric_limits<size_t>::max()) {
      return {};
    }
    ++operations_;
    return Operation(shared_from_this());
  }
  // Permanently close new loader/resolver admission. Does not wait or claim
  // native task/CPU/GPU quiescence. False means an admitted loader operation
  // is still finishing; Kernel retains the closed domain and its profile.
  bool Retire() {
    std::lock_guard lock(mutex_);
    retired_ = true;
    return operations_ == 0;
  }

 private:
  NativeImportDomain(uint64_t id, Member root,
                     std::shared_ptr<const cpu::GuestImportProvider> profile)
      : id_(id), root_(root), profile_(std::move(profile)) {
    members_.emplace(root.hmodule, root.generation);
  }
  const uint64_t id_;
  const Member root_;
  const std::shared_ptr<const cpu::GuestImportProvider> profile_;
  mutable std::mutex mutex_;
  bool retired_ = false;
  size_t operations_ = 0;
  std::map<uint32_t, uint64_t> members_;
};

}  // namespace xe::kernel
#endif  // XENIA_KERNEL_NATIVE_IMPORT_DOMAIN_H_
