/** Xenia : Xbox 360 Emulator Research Project; BSD license, see LICENSE. */
#ifndef XENIA_KERNEL_CONSOLE_SIGNING_IDENTITY_H_
#define XENIA_KERNEL_CONSOLE_SIGNING_IDENTITY_H_
#include <array>
#include <memory>
#include <span>
#include <system_error>
#include "xenia/base/host_storage.h"
#include "xenia/xbox.h"
namespace xe { class Memory; }
namespace xe::kernel {
// Immutable console identity retained by ConsoleDataOwner across title resets.
// Its secret stays in the existing protected console root and this private
// implementation. It is never a content/catalog/guest-mount participant.
class ConsoleSigningIdentity final {
 public:
  static constexpr size_t kCertificateBytes=0x1A8,kSignatureBytes=0x228;
  static constexpr char kIdentityFile[]="console-signing-v1.bin";
  using Certificate=std::array<uint8_t,kCertificateBytes>;
  using Signature=std::array<uint8_t,kSignatureBytes>;
  ~ConsoleSigningIdentity();
  const Certificate& certificate() const noexcept;
  std::array<uint8_t,5> console_id() const noexcept;
  std::array<uint8_t,12> console_id_text() const noexcept;
  // Exact SHA1 digest. Output remains unchanged if real signing fails.
  bool Sign(std::span<const uint8_t> hash,Signature&) const;
  // Local trust is exact equality with THIS persisted emulator certificate,
  // not Microsoft attestation. Foreign certificates require the actual public
  // MASTER key (272-byte XECRYPT_RSA); absence never accepts a foreign cert.
  // The native third output is zero-same/nonzero-different certificate value.
  bool Verify(std::span<const uint8_t> hash,std::span<const uint8_t> signature,
      uint32_t& certificate_difference,
      std::span<const uint8_t> trusted_master_public_key={}) const;
 private:
  friend class ConsoleDataOwner;
  static std::shared_ptr<ConsoleSigningIdentity> Open(
      std::shared_ptr<filesystem::HostStorageRoot> protected_console,
      std::error_code&);
  struct State;
  explicit ConsoleSigningIdentity(std::unique_ptr<State>);
  std::unique_ptr<State> state_;
};
// Synchronous actual guest copy boundary shared by the typed kernel exports.
// No translated pointer survives a provider wait or the signing mutex.
X_STATUS ConsoleSigningIdGuest(Memory&,const ConsoleSigningIdentity&,uint32_t raw,uint32_t text);
X_STATUS ConsoleSigningCertificateGuest(Memory&,const ConsoleSigningIdentity&,uint32_t output);
bool ConsoleSigningSignGuest(Memory&,const ConsoleSigningIdentity&,uint32_t hash,uint32_t output);
bool ConsoleSigningVerifyGuest(Memory&,const ConsoleSigningIdentity&,uint32_t hash,uint32_t signature,uint32_t difference);
} // namespace xe::kernel
#endif
