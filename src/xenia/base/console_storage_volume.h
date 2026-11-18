/** Xenia : Xbox 360 Emulator Research Project; BSD license, see LICENSE. */
#ifndef XENIA_BASE_CONSOLE_STORAGE_VOLUME_H_
#define XENIA_BASE_CONSOLE_STORAGE_VOLUME_H_
#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <utility>
namespace xe::kernel::xam { class ContentSourceRegistry; }
namespace xe::filesystem {
// Native storage classes, shared by registry/mount producers and XAM consumers.
// These values do not describe a title's deployment or grant filesystem access.
enum class ConsoleStorageType : uint32_t {
  Invalid=0, HDD=1, MU=2, System=3, ODD=4,
  TransferCable=8, Sapphire=15, USBMASS=16,
};
inline bool IsConsoleStorageType(ConsoleStorageType type) {
  switch (type) {
    case ConsoleStorageType::HDD: case ConsoleStorageType::MU:
    case ConsoleStorageType::System: case ConsoleStorageType::ODD:
    case ConsoleStorageType::TransferCable: case ConsoleStorageType::Sapphire:
    case ConsoleStorageType::USBMASS: return true;
    default: return false;
  }
}
// Raw NT device types consumed by the actual 17559 FE volume decoder.
// Unknown legacy classes remain unknown; host disk hardware is not guessed.
inline std::optional<uint32_t> NativeStorageDeviceType(ConsoleStorageType type) {
  switch (type) {
    case ConsoleStorageType::HDD: return 7;
    case ConsoleStorageType::ODD: return 2;
    case ConsoleStorageType::USBMASS: return 0x2D;
    case ConsoleStorageType::MU: return 0x44;
    case ConsoleStorageType::TransferCable: return 0x45;
    default: return {};
  }
}
class ConsoleStorageVolume final {
 public:
  enum class Origin { kRegisteredSource, kExternalPackagePolicy, kOpticalMedia, kInstalledSystemPolicy };
  ConsoleStorageType type() const { return type_; }
  uint32_t device_id() const { return device_id_; }
  const std::string& source_id() const { return source_id_; }
  uint64_t generation() const { return generation_; }
  Origin origin() const { return origin_; }
  // The cached getter performs no FS observation, weak-owner acquisition,
  // callbacks or last native-owner rundown; it is not live media admission.
  // RetireKnownSource below also signals local negative cancellation and
  // condition-variable wake callbacks, without guest/provider calls or IO.
  bool available_cached() const { return available_.load(std::memory_order_acquire); }
  std::stop_token retirement_token() const { return retirement_stop_.get_token(); }
  // A host library package has an explicit logical HDD presentation policy.
  // Zero ID/generation deliberately does not impersonate a registered device.
  static std::shared_ptr<const ConsoleStorageVolume> ExternalPackage() {
    return std::shared_ptr<const ConsoleStorageVolume>(new ConsoleStorageVolume(
        ConsoleStorageType::HDD,0,"",0,Origin::kExternalPackagePolicy));
  }
  // Pre-XAM installed partition mounts cannot claim a registry generation.
  static std::shared_ptr<const ConsoleStorageVolume> InstalledSystem() {
    return std::shared_ptr<const ConsoleStorageVolume>(new ConsoleStorageVolume(
        ConsoleStorageType::HDD,0,"",0,Origin::kInstalledSystemPolicy));
  }
  static std::shared_ptr<const ConsoleStorageVolume> OpticalMedia() {
    return std::shared_ptr<const ConsoleStorageVolume>(new ConsoleStorageVolume(
        ConsoleStorageType::ODD,0,"",0,Origin::kOpticalMedia));
  }
 private:
  friend class xe::kernel::xam::ContentSourceRegistry;
  ConsoleStorageVolume(ConsoleStorageType type,uint32_t device_id,
      std::string source_id,uint64_t generation,Origin origin)
      :type_(type),device_id_(device_id),source_id_(std::move(source_id)),
       generation_(generation),origin_(origin) {}
  void RetireKnownSource() const {
    available_.store(false,std::memory_order_release);
    retirement_stop_.request_stop();
  }
  const ConsoleStorageType type_;
  const uint32_t device_id_;
  const std::string source_id_;
  const uint64_t generation_;
  const Origin origin_;
  mutable std::atomic<bool> available_{true};
  mutable std::stop_source retirement_stop_;
};
}  // namespace xe::filesystem
#endif
