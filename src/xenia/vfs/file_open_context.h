/** Xenia : Xbox 360 Emulator Research Project; BSD license, see LICENSE. */
#ifndef XENIA_VFS_FILE_OPEN_CONTEXT_H_
#define XENIA_VFS_FILE_OPEN_CONTEXT_H_
#include <memory>
namespace xe::vfs {
class Device;
// Opaque policy-specific capability. The concrete producer must issue it from
// the actual authenticated request and retain its real source/view lifetime.
// A filename, access mask, caller-filled flag or this base type grants nothing.
class ManagedFileOpenAuthority {
 public:
  virtual ~ManagedFileOpenAuthority() = default;
  ManagedFileOpenAuthority(const ManagedFileOpenAuthority&) = delete;
  ManagedFileOpenAuthority& operator=(const ManagedFileOpenAuthority&) = delete;
 protected:
  ManagedFileOpenAuthority() = default;
};
// Owned value captured on the genuine caller BEFORE host IO offload, alongside
// the actual root XFile. Independent issuers contribute constraints; neither
// wins precedence over another. VFS authenticates expected_device against the
// actual registered graph before any disposition/managed dispatch. A managed
// policy consumes only its own issued authority and refuses all others.
struct FileOpenContext {
  std::shared_ptr<Device> expected_device;
  std::shared_ptr<const ManagedFileOpenAuthority> managed_authority;
};
}  // namespace xe::vfs
#endif
