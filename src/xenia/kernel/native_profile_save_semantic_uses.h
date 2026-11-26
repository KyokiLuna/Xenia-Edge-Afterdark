/** Xenia : Xbox 360 Emulator Research Project; BSD license, see LICENSE. */
#pragma once
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstddef>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

namespace xe::kernel::save_detail {
// Deny-only metadata for a SHORT genuine getter/reference/DB use. Neither an
// unchanged span nor a bound address is source, native-object, read or writer
// authority. The actual family must authenticate every original entry/return
// and separately retain the real native reference and selected storage tuple.
// No native ref, Task, source/model or callback owner lives in this ledger.
class SemanticUses final {
 public:
  ~SemanticUses() {Unavailable();}
  struct Frame {
    uint32_t thread=0,pcr=0,sp=0,lr=0,function=0;
    bool operator==(const Frame&) const=default;
  };
  enum class Target {kProfile,kDatabase,kResource,kUnknown};
  class Span final {
   public:
    // This ONLY reports whether an observed semantic interference/retirement
    // has already withdrawn this span. It never grants permission by itself.
    bool unchanged() const noexcept {
      return !revoked_.load(std::memory_order_acquire) &&
             !observation_lost_->load(std::memory_order_acquire);
    }
   private:
    friend class SemanticUses;
    Span(uint64_t serial,std::shared_ptr<const std::atomic<bool>> lost)
        :serial_(serial),observation_lost_(std::move(lost)) {}
    const uint64_t serial_;
    const std::shared_ptr<const std::atomic<bool>> observation_lost_;
    uint32_t profile_=0,database_=0,resource_=0;
    std::atomic<bool> revoked_{false};
  };
  std::shared_ptr<Span> BeginGetter() {
    std::lock_guard lock(mutex_);Prune();
    if(observation_lost_->load(std::memory_order_acquire) || next_==UINT64_MAX || spans_.size()>=kBound) {Fail();return {};}
    auto result=std::shared_ptr<Span>(new Span(++next_,observation_lost_));
    // Object is still UNKNOWN while original E310 takes its array lock and
    // reference. Any active covered rebind can touch that future object.
    if(!mutations_.empty())result->revoked_.store(true,std::memory_order_release);
    spans_.push_back(result);return result;
  }
  bool BindProfile(const std::shared_ptr<Span>& span,uint32_t profile) {
    std::lock_guard lock(mutex_);
    if(!Owns(span))return false;
    if(!profile || (profile&3) ||
        (span->profile_ && span->profile_!=profile)) {Revoke(span);return false;}
    span->profile_=profile;return span->unchanged();
  }
  bool BindDatabase(const std::shared_ptr<Span>& span,uint32_t database,uint32_t resource) {
    std::lock_guard lock(mutex_);
    if(!Owns(span))return false;
    if(!span->profile_ || !EmbeddedDatabase(span->profile_,database) ||
        (span->database_ && (span->database_!=database || span->resource_!=resource))) {
      Revoke(span);return false;
    }
    span->database_=database;span->resource_=resource;return span->unchanged();
  }
  void Retire(const std::shared_ptr<Span>& span) noexcept {
    // Exact old metadata retirement has no current-source predicate and no
    // callback into PM/Kernel. It does not settle a pending durable intent.
    std::lock_guard lock(mutex_);if(Contains(span))Revoke(span);
  }
  void Enter(Frame frame,Target target,uint32_t address) {
    std::lock_guard lock(mutex_);Prune();
    if(observation_lost_->load(std::memory_order_acquire))return;
    if(!frame.thread || !frame.pcr || !frame.sp || (frame.sp&15) || !frame.lr || !frame.function ||
        !address || (address&3) || mutations_.size()>=kBound ||
        std::any_of(mutations_.begin(),mutations_.end(),[&](const auto& m){return m.frame==frame;})) {
      Fail();return;
    }
    // First retain the real unfinished mutation interval, before publishing
    // its revocations. Allocation failure is deny-only for every live span.
    try {mutations_.push_back({frame,target,address});}
    catch(...) {Fail();throw;}
    for(const auto& weak:spans_)if(const auto span=weak.lock())
      if(Matches(*span,target,address))Revoke(span);
  }
  bool Leave(Frame frame) noexcept {
    std::lock_guard lock(mutex_);
    const auto found=std::find_if(mutations_.rbegin(),mutations_.rend(),
        [&](const auto& mutation){return mutation.frame==frame;});
    if(found==mutations_.rend()) {Fail();return false;}
    mutations_.erase(std::next(found).base());
    // A completed mutation NEVER rehabilitates a previous span or relearns
    // changed identity fields. Only a later genuine new getter can begin one.
    return true;
  }
  // Actual GCRC-held missed cuts must still withdraw every existing use.
  // This path is ONE atomic store: no allocation, metadata-mutex acquisition,
  // native memory access, callback, IO or waiting for another guest thread.
  void Unavailable() noexcept {Fail();}
 private:
  static constexpr size_t kBound=128;
  struct Mutation {Frame frame;Target target;uint32_t address;};
  static bool EmbeddedDatabase(uint32_t profile,uint32_t database) {
    return uint64_t(profile)+0xD8==database || uint64_t(profile)+0x134==database ||
           uint64_t(profile)+0x1248==database;
  }
  static bool Matches(const Span& span,Target target,uint32_t address) {
    if(!span.profile_ || target==Target::kUnknown)return true;
    switch(target) {
      case Target::kProfile:return span.profile_==address;
      case Target::kDatabase:return EmbeddedDatabase(span.profile_,address);
      // Before the actual DB bind an unrelated resource cannot be proved
      // unrelated. After it, only the actual retained resource is matched.
      case Target::kResource:return !span.database_ || span.resource_==address;
      case Target::kUnknown:return true;
    }
    return true;
  }
  bool Contains(const std::shared_ptr<Span>& span) const {
    if(!span)return false;
    return std::any_of(spans_.begin(),spans_.end(),[&](const auto& weak) {
      const auto actual=weak.lock();return actual && actual==span &&
          !actual.owner_before(span) && !span.owner_before(actual);
    });
  }
  bool Owns(const std::shared_ptr<Span>& span) const {
    return !observation_lost_->load(std::memory_order_acquire) && span && span->unchanged() && Contains(span);
  }
  static void Revoke(const std::shared_ptr<Span>& span) noexcept {
    if(span)span->revoked_.store(true,std::memory_order_release);
  }
  void Prune() {std::erase_if(spans_,[](const auto& weak){return weak.expired();});}
  void Fail() noexcept {
    observation_lost_->store(true,std::memory_order_release);
  }
  std::mutex mutex_;
  uint64_t next_=0;
  // Retained scalar loss receipt survives this HOST ledger without retaining
  // Kernel, native object/source, model, Task or the ledger mutex itself.
  const std::shared_ptr<std::atomic<bool>> observation_lost_=
      std::make_shared<std::atomic<bool>>(false);
  std::vector<std::weak_ptr<Span>> spans_;
  std::vector<Mutation> mutations_;
};
}  // namespace xe::kernel::save_detail
