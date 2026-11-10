/**
 * Xenia : Xbox 360 Emulator Research Project
 * Released under the BSD license - see LICENSE in the root for details.
 */
#include "xenia/kernel/console_settings_store.h"
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <limits>
#include <string>
#include "third_party/mbedtls/include/mbedtls/sha256.h"
#include "xenia/base/platform.h"
#if XE_PLATFORM_LINUX || XE_PLATFORM_MAC
#include <sys/file.h>
#endif
namespace xe::kernel {
namespace {
constexpr size_t kMaximumBytes = 64 * 1024;
constexpr size_t kHeader = 52, kChecksum = 32;
constexpr char kTarget[] = "xconfig.bin", kLock[] = "xconfig.lock";
uint64_t BE(std::span<const uint8_t> data) {
  uint64_t result = 0;
  for (auto byte : data) {
    result = (result << 8) | byte;
  }
  return result;
}
void Put(std::vector<uint8_t>& data, uint64_t value, size_t count) {
  for (size_t i = 0; i < count; ++i) {
    data.push_back(uint8_t(value >> ((count - i - 1) * 8)));
  }
}
std::array<uint8_t, 32> Hash(std::span<const uint8_t> bytes) {
  std::array<uint8_t, 32> digest{};
  mbedtls_sha256(bytes.data(), bytes.size(), digest.data(), 0);
  return digest;
}
std::error_code Invalid() {
  return std::make_error_code(std::errc::bad_message);
}
std::filesystem::path Backup(uint64_t revision) {
  return "xconfig.backup." + std::to_string(revision);
}
std::filesystem::path Stage() {
  static std::atomic<uint64_t> sequence{0};
  const auto ticks =
      std::chrono::steady_clock::now().time_since_epoch().count();
  return "xconfig.stage." + std::to_string(ticks) + "." +
         std::to_string(sequence.fetch_add(1));
}
class ConsoleFileLock {
 public:
  ConsoleFileLock(intptr_t handle, bool write, std::error_code& error) {
    error.clear();
#if XE_PLATFORM_LINUX || XE_PLATFORM_MAC
    if (flock(int(handle), (write ? LOCK_EX : LOCK_SH) | LOCK_NB)) {
      error = (errno == EWOULDBLOCK || errno == EAGAIN)
                  ? std::make_error_code(std::errc::device_or_resource_busy)
                  : std::error_code(errno, std::generic_category());
    } else {
      handle_ = int(handle);
    }
#else
    error = std::make_error_code(std::errc::operation_not_supported);
#endif
  }
  ConsoleFileLock(const ConsoleFileLock&) = delete;
  ConsoleFileLock& operator=(const ConsoleFileLock&) = delete;
  ~ConsoleFileLock() {
#if XE_PLATFORM_LINUX || XE_PLATFORM_MAC
    if (handle_ >= 0) {
      while (flock(handle_, LOCK_UN) == -1 && errno == EINTR) {
      }
    }
#endif
  }

 private:
  int handle_ = -1;
};
}  // namespace
ConsoleSettingsStore::ConsoleSettingsStore(
    std::shared_ptr<filesystem::HostStorageRoot> root, Publish publish,
    StageName stage_name)
    : root_(std::move(root)),
      publish_(std::move(publish)),
      stage_name_(std::move(stage_name)) {
  if (!stage_name_) {
    stage_name_ = Stage;
  }
  if (!publish_) {
    publish_ = [](const auto& root, const auto& staged, const auto& target,
                  const auto& backup) {
      return root.AtomicReplaceFile(staged, target, backup);
    };
  }
}
ConsoleSettingsStore::~ConsoleSettingsStore() = default;
std::shared_ptr<ConsoleSettingsStore> ConsoleSettingsStore::Open(
    std::shared_ptr<filesystem::HostStorageRoot> root, std::error_code& error,
    Publish publish, StageName stage_name) {
  error.clear();
  if (!root) {
    error = std::make_error_code(std::errc::invalid_argument);
    return {};
  }
  auto store = std::shared_ptr<ConsoleSettingsStore>(new ConsoleSettingsStore(
      std::move(root), std::move(publish), std::move(stage_name)));
  error = store->Available();
  if (error) {
    return {};
  }
#if XE_PLATFORM_LINUX || XE_PLATFORM_MAC
  constexpr uint32_t read = filesystem::FileAccess::kFileReadData;
  constexpr uint32_t write = filesystem::FileAccess::kFileWriteData;
  bool created = false;
  // Existing preferences remain readable even when the allocation reserve is
  // exhausted. Do not attempt a create/admission before looking up the lock.
  store->owner_lock_ = store->root_->OpenFile(kLock, read, error);
  if (!store->owner_lock_ && error == std::errc::no_such_file_or_directory &&
      !store->root_->read_only()) {
    store->owner_lock_ =
        store->root_->OpenFile(kLock, read | write, error, true);
    created = bool(store->owner_lock_);
    if (!store->owner_lock_ && error == std::errc::file_exists) {
      store->owner_lock_ = store->root_->OpenFile(kLock, read, error);
    }
  }
  if (!store->owner_lock_) {
    return {};
  }
  const auto info = store->owner_lock_->QueryInfo(error);
  if (!info || !info->regular || info->links != 1 ||
      store->owner_lock_->native_handle() < 0) {
    if (!error) {
      error = std::make_error_code(std::errc::permission_denied);
    }
    return {};
  }
  if (created) {
    error = store->owner_lock_->FlushResult();
    if (!error) {
      error = store->root_->FlushDirectory();
    }
    if (error) {
      return {};
    }
  }
#else
  error = std::make_error_code(std::errc::operation_not_supported);
  return {};
#endif
  ConsoleFileLock locked(store->owner_lock_->native_handle(), false, error);
  if (error) {
    return {};
  }
  error = store->Reload();
  return error ? nullptr : store;
}
std::error_code ConsoleSettingsStore::Decode(std::span<const uint8_t> bytes,
                                             Loaded& output) {
  if (bytes.size() < kHeader + kChecksum || bytes.size() > kMaximumBytes ||
      std::memcmp(bytes.data(), "XCFG", 4) || BE(bytes.subspan(4, 4)) != 1 ||
      !BE(bytes.subspan(8, 8))) {
    return Invalid();
  }
  const auto digest = Hash(bytes.first(bytes.size() - kChecksum));
  if (!std::equal(digest.begin(), digest.end(), bytes.end() - kChecksum)) {
    return Invalid();
  }
  Loaded next;
  next.revision = BE(bytes.subspan(8, 8));
  std::copy_n(bytes.begin() + 16, 32, next.previous.begin());
  const uint32_t count = uint32_t(BE(bytes.subspan(48, 4)));
  if (count > 1024) {
    return Invalid();
  }
  size_t cursor = kHeader;
  for (uint32_t i = 0; i < count; ++i) {
    const size_t end = bytes.size() - kChecksum;
    if (cursor > end || end - cursor < 8) {
      return Invalid();
    }
    ConsoleSettingKey key{uint16_t(BE(bytes.subspan(cursor, 2))),
                          uint16_t(BE(bytes.subspan(cursor + 2, 2)))};
    const auto length = size_t(BE(bytes.subspan(cursor + 4, 4)));
    cursor += 8;
    if (length > 4096 || length > end - cursor) {
      return Invalid();
    }
    if (!next.records
             .emplace(key,
                      std::vector<uint8_t>(bytes.begin() + cursor,
                                           bytes.begin() + cursor + length))
             .second) {
      return Invalid();
    }
    cursor += length;
  }
  if (cursor != bytes.size() - kChecksum) {
    return Invalid();
  }
  const auto retail = next.records.find({3, 0xC});
  if (retail != next.records.end() && retail->second.size() != 4) {
    return Invalid();
  }
  output = std::move(next);
  return {};
}
std::error_code ConsoleSettingsStore::Encode(const Loaded& value,
                                             std::vector<uint8_t>& output) {
  if (!value.revision || value.records.size() > 1024) {
    return Invalid();
  }
  std::vector<uint8_t> next{'X', 'C', 'F', 'G'};
  Put(next, 1, 4);
  Put(next, value.revision, 8);
  next.insert(next.end(), value.previous.begin(), value.previous.end());
  Put(next, value.records.size(), 4);
  for (const auto& [key, bytes] : value.records) {
    if (bytes.size() > 4096 ||
        next.size() + 8 + bytes.size() > kMaximumBytes - kChecksum) {
      return std::make_error_code(std::errc::value_too_large);
    }
    Put(next, key.category, 2);
    Put(next, key.setting, 2);
    Put(next, bytes.size(), 4);
    next.insert(next.end(), bytes.begin(), bytes.end());
  }
  const auto digest = Hash(next);
  next.insert(next.end(), digest.begin(), digest.end());
  output = std::move(next);
  return {};
}
std::error_code ConsoleSettingsStore::Available() const {
  const auto state = root_->Observe();
  return state.available
             ? std::error_code{}
             : (state.error ? state.error
                            : std::make_error_code(std::errc::no_such_device));
}
std::error_code ConsoleSettingsStore::Reload() {
  auto error = Available();
  if (error) {
    return error;
  }
  std::vector<uint8_t> bytes;
  error = root_->ReadFile(kTarget, kMaximumBytes, bytes);
  if (wire_.empty() && !loaded_.revision &&
      error == std::errc::no_such_file_or_directory) {
    return {};
  }
  if (error) {
    return error;
  }
  if (!bytes.empty() && bytes == wire_) {
    return {};
  }
  Loaded next;
  error = Decode(bytes, next);
  if (error) {
    return error;
  }
  if (loaded_.revision && next.revision <= loaded_.revision) {
    return std::make_error_code(std::errc::state_not_recoverable);
  }
  loaded_ = std::move(next);
  wire_ = std::move(bytes);
  durable_ = true;
  return {};
}
std::error_code ConsoleSettingsStore::CurrentUnchanged() const {
  auto error = Available();
  if (error) {
    return error;
  }
  std::vector<uint8_t> bytes;
  error = root_->ReadFile(kTarget, kMaximumBytes, bytes);
  if (wire_.empty()) {
    if (error == std::errc::no_such_file_or_directory) {
      return {};
    }
    return error ? error
                 : std::make_error_code(std::errc::state_not_recoverable);
  }
  if (error) {
    return error;
  }
  return bytes == wire_
             ? std::error_code{}
             : std::make_error_code(std::errc::state_not_recoverable);
}
std::error_code ConsoleSettingsStore::Read(ConsoleSettingKey key,
                                           std::vector<uint8_t>& bytes) {
  std::lock_guard lock(mutex_);
  std::error_code error;
  ConsoleFileLock locked(owner_lock_->native_handle(), false, error);
  if (error) {
    return error;
  }
  error = Reload();
  if (error) {
    return error;
  }
  const auto found = loaded_.records.find(key);
  if (found == loaded_.records.end()) {
    return std::make_error_code(std::errc::no_such_file_or_directory);
  }
  bytes = found->second;
  return {};
}
std::error_code ConsoleSettingsStore::ReadU32Locked(ConsoleSettingKey key,
                                                    uint32_t seed,
                                                    uint32_t& output) const {
  const auto error = Available();
  if (error) {
    return error;
  }
  const auto found = loaded_.records.find(key);
  if (found == loaded_.records.end()) {
    output = seed;
    return {};
  }
  if (found->second.size() != 4) {
    return Invalid();
  }
  output = uint32_t(BE(found->second));
  return {};
}
std::error_code ConsoleSettingsStore::ReadU32(ConsoleSettingKey key,
                                              uint32_t seed, uint32_t& output) {
  std::lock_guard lock(mutex_);
  std::error_code error;
  ConsoleFileLock locked(owner_lock_->native_handle(), false, error);
  if (error) {
    return error;
  }
  error = Reload();
  if (error) {
    return error;
  }
  return ReadU32Locked(key, seed, output);
}
ConsoleSettingsUpdate ConsoleSettingsStore::WriteU32(ConsoleSettingKey key,
                                                     uint32_t value) {
  std::lock_guard lock(mutex_);
  std::error_code error;
  ConsoleFileLock locked(owner_lock_->native_handle(), true, error);
  if (error) {
    return {error};
  }
  error = Reload();
  if (error) {
    return {error};
  }
  auto next = loaded_;
  std::vector<uint8_t> bytes;
  Put(bytes, value, 4);
  next.records.insert_or_assign(key, std::move(bytes));
  return Commit(std::move(next));
}
ConsoleSettingsUpdate ConsoleSettingsStore::ModifyU32(ConsoleSettingKey key,
                                                      uint32_t seed,
                                                      uint32_t and_mask,
                                                      uint32_t or_bits) {
  std::lock_guard lock(mutex_);
  std::error_code locked_error;
  ConsoleFileLock locked(owner_lock_->native_handle(), true, locked_error);
  if (locked_error) {
    return {locked_error};
  }
  locked_error = Reload();
  if (locked_error) {
    return {locked_error};
  }
  uint32_t old;
  const auto error = ReadU32Locked(key, seed, old);
  if (error) {
    return {error};
  }
  auto next = loaded_;
  std::vector<uint8_t> bytes;
  Put(bytes, (old & and_mask) | or_bits, 4);
  next.records.insert_or_assign(key, std::move(bytes));
  return Commit(std::move(next));
}
ConsoleSettingsUpdate ConsoleSettingsStore::Commit(Loaded next) {
  auto error = CurrentUnchanged();
  if (error) {
    return {error};
  }
  if (root_->read_only()) {
    return {std::make_error_code(std::errc::read_only_file_system)};
  }
  if (next.records == loaded_.records) {
    // Another process may have published this exact value but reported a
    // directory-flush failure. An explicit no-op Set still retries durability.
    auto current =
        root_->OpenFile(kTarget, filesystem::FileAccess::kFileReadData, error);
    if (current) {
      error = current->FlushResult();
    }
    if (!error) {
      error = root_->FlushDirectory();
    }
    durable_ = !error;
    return {error, false, durable_};
  }
  if (loaded_.revision == std::numeric_limits<uint64_t>::max()) {
    return {std::make_error_code(std::errc::value_too_large)};
  }
  next.revision = loaded_.revision + 1;
  next.previous = wire_.empty() ? std::array<uint8_t, 32>{} : Hash(wire_);
  std::vector<uint8_t> wire;
  error = Encode(next, wire);
  if (error) {
    return {error};
  }
  std::filesystem::path staged;
  for (int attempt = 0; attempt < 16; ++attempt) {
    staged = stage_name_();
    const auto written = root_->WriteNewFile(staged, wire, true);
    error = written.error;
    if (error != std::errc::file_exists) {
      break;
    }
  }
  if (error) {
    // WriteNewFile can fail before exclusive creation (for example admission
    // can fail before O_EXCL). Its result does not prove that this path is
    // ours, even for errors other than EEXIST. Preserve uncertain recovery
    // stages, including any partial stage from a failed write or flush.
    return {error};
  }
  const Loaded prior = loaded_;
  const auto result = publish_(
      *root_, staged, kTarget,
      loaded_.revision ? Backup(loaded_.revision) : std::filesystem::path{});
  if (!result.published) {
    root_->Remove(staged, false);
    return {result.error ? result.error
                         : std::make_error_code(std::errc::io_error)};
  }
  // A parent-directory flush can fail AFTER publication. Never report rollback
  // or keep stale in-memory bits when the new current file is already visible.
  loaded_ = std::move(next);
  wire_ = std::move(wire);
  durable_ = !result.error;
  if (!result.error) {
    try {
      PrunePrevious(prior);
    } catch (...) {
      // Optional pruning cannot invalidate an already durable transaction.
      // Keep extra recovery copies if allocation or cleanup fails.
    }
  }
  return {result.error, true, durable_};
}
void ConsoleSettingsStore::PrunePrevious(const Loaded& prior) {
  if (prior.revision < 2) {
    return;
  }
  const auto path = Backup(prior.revision - 1);
  std::vector<uint8_t> bytes;
  if (root_->ReadFile(path, kMaximumBytes, bytes) ||
      Hash(bytes) != prior.previous) {
    return;  // Preserve anything not proven to be our previous generation.
  }
  Loaded decoded;
  if (!Decode(bytes, decoded) && decoded.revision + 1 == prior.revision) {
    root_->Remove(path, false);  // Failure retains an extra recovery copy.
  }
}
}  // namespace xe::kernel
