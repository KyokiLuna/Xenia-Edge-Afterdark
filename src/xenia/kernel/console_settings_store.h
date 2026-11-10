/**
 * Xenia : Xbox 360 Emulator Research Project
 * Released under the BSD license - see LICENSE in the root for details.
 */
#ifndef XENIA_KERNEL_CONSOLE_SETTINGS_STORE_H_
#define XENIA_KERNEL_CONSOLE_SETTINGS_STORE_H_
#include <array>
#include <compare>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <span>
#include <system_error>
#include <vector>
#include "xenia/base/host_storage.h"
namespace xe::kernel {
struct ConsoleSettingKey {
  uint16_t category, setting;
  auto operator<=>(const ConsoleSettingKey&) const = default;
};
struct ConsoleSettingsUpdate {
  std::error_code error;
  bool published = false;
  bool durable = false;
};
// One logical console authority shared by native XConfig and fallback readers.
// Caller must supply a private capability outside ALL guest mounts; a path's
// spelling alone is not evidence of that ownership. Cooperating processes use
// one lock file for each bounded transaction. Contention fails busy rather than
// blocking an out-of-process launch for an entire Kernel lifetime.
class ConsoleSettingsStore {
 public:
  using Publish = std::function<filesystem::HostStorageRoot::ReplaceResult(
      const filesystem::HostStorageRoot&, const std::filesystem::path&,
      const std::filesystem::path&, const std::filesystem::path&)>;
  // Isolated deterministic name seam for recovery/admission tests. Names remain
  // capability-relative and go through the same exclusive WriteNewFile path.
  using StageName = std::function<std::filesystem::path()>;
  static std::shared_ptr<ConsoleSettingsStore> Open(
      std::shared_ptr<filesystem::HostStorageRoot> private_root,
      std::error_code&, Publish isolated_publish = {},
      StageName isolated_stage_name = {});
  ~ConsoleSettingsStore();
  std::error_code Read(ConsoleSettingKey, std::vector<uint8_t>&);
  // A missing record uses migration_seed without writing during a read. First
  // successful Set/RMW persists the resulting complete DWORD; persisted bits
  // thereafter take precedence over every legacy cvar/default.
  std::error_code ReadU32(ConsoleSettingKey, uint32_t migration_seed,
                          uint32_t&);
  ConsoleSettingsUpdate WriteU32(ConsoleSettingKey, uint32_t);
  ConsoleSettingsUpdate ModifyU32(ConsoleSettingKey, uint32_t migration_seed,
                                  uint32_t and_mask, uint32_t or_bits);

 private:
  using Records = std::map<ConsoleSettingKey, std::vector<uint8_t>>;
  struct Loaded {
    Records records;
    uint64_t revision = 0;
    std::array<uint8_t, 32> previous{};
  };
  explicit ConsoleSettingsStore(std::shared_ptr<filesystem::HostStorageRoot>,
                                Publish, StageName);
  static std::error_code Decode(std::span<const uint8_t>, Loaded&);
  static std::error_code Encode(const Loaded&, std::vector<uint8_t>&);
  std::error_code Available() const;
  std::error_code Reload();
  std::error_code CurrentUnchanged() const;
  std::error_code ReadU32Locked(ConsoleSettingKey, uint32_t, uint32_t&) const;
  ConsoleSettingsUpdate Commit(Loaded);
  void PrunePrevious(const Loaded&);
  std::shared_ptr<filesystem::HostStorageRoot> root_;
  std::unique_ptr<filesystem::FileHandle> owner_lock_;
  Publish publish_;
  StageName stage_name_;
  mutable std::mutex mutex_;
  Loaded loaded_;
  std::vector<uint8_t> wire_;
  bool durable_ = true;
};
}  // namespace xe::kernel
#endif
