/**
 * Xenia : Xbox 360 Emulator Research Project
 * Released under the BSD license - see LICENSE in the root for details.
 */
#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif
#include "xenia/base/host_storage.h"
#include "xenia/base/managed_storage_bootstrap.h"
#include "xenia/base/content_package_mutation_wire.h"
#include "xenia/base/content_mutation_wire.h"
#include "third_party/mbedtls/include/mbedtls/sha256.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <condition_variable>
#include <utility>
#include <fstream>
#include <limits>
#include <map>
#include <mutex>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include "xenia/base/platform.h"
#include "xenia/base/mutex.h"
#include "xenia/base/snapshot_io.h"
#if XE_PLATFORM_WIN32
#include "xenia/base/platform_win.h"
#else
#include <dirent.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#if XE_PLATFORM_LINUX
#include <linux/stat.h>
#include <sys/syscall.h>
#elif XE_PLATFORM_MAC
#include <sys/attr.h>
#include <sys/mount.h>
#include <sys/param.h>
#endif
#endif
namespace xe::filesystem {
namespace {
constexpr uint64_t kMetadataAllowance = 64 * 1024;
const void* StorageThreadToken() {
  static thread_local uint8_t token;
  return &token;
}
std::error_code Error(std::errc e) { return std::make_error_code(e); }
std::error_code NativeError() {
#if XE_PLATFORM_WIN32
  return {int(::GetLastError()), std::system_category()};
#else
  return {errno, std::generic_category()};
#endif
}
bool ContentMutationLeaf(std::string_view leaf) {
  if (leaf == "owner.v1") { return true; }
  if (!leaf.starts_with("content-")) { return false; }
  leaf.remove_prefix(8);
  auto hex = [](std::string_view text) {
    return text.size() == 32 && std::all_of(text.begin(), text.end(), [](char c) {
      return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
  };
  if (leaf.size() < 37 || !hex(leaf.substr(0,32))) { return false; }
  auto suffix = leaf.substr(32);
  const auto partial = suffix.find(".partial-");
  if (partial != std::string_view::npos) {
    if (!hex(suffix.substr(partial+9))) { return false; }
    suffix = suffix.substr(0,partial);
  }
  return suffix == ".intent" || suffix == ".done" || suffix == ".retire" ||
      suffix == ".stream" || suffix == ".plan" || suffix == ".abort" ||
      suffix == ".keep0" || suffix == ".keep1";
}
bool RecoveryLeaf(std::string_view leaf) {
  return leaf == "owner.v1" || (leaf.size() == 45 &&
      leaf.starts_with("snapshot-") && leaf.ends_with(".bin") &&
      std::all_of(leaf.begin() + 9, leaf.end() - 4, [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
      }));
}
bool IntentLeaf(std::string_view leaf) {
  const size_t prefix = leaf.starts_with("intent-") ? 7
                      : leaf.starts_with("done-") ? 5 : 0;
  return leaf == "owner.v1" || (prefix && leaf.size() == prefix + 36 &&
      leaf.ends_with(".bin") &&
      std::all_of(leaf.begin() + prefix, leaf.end() - 4, [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
      }));
}
bool NativeSpelling(const std::filesystem::path& path) {
  // Counted C++ path strings must describe exactly the native C-string path.
  // An embedded NUL would otherwise name its existing prefix at stat/openat.
  return path.native().find(std::filesystem::path::value_type{}) ==
      std::filesystem::path::string_type::npos;
}
bool Relative(const std::filesystem::path& path, bool allow_empty = false) {
  if (!NativeSpelling(path) || (!allow_empty && path.empty()) ||
      path.is_absolute() || path.has_root_name()) {
    return false;
  }
  for (const auto& part : path) {
    if (part == ".." || part == ".") {
      return false;
    }
  }
  return true;
}
#if !XE_PLATFORM_WIN32
FileHandleInfo SnapshotInfo(const struct stat&);
std::error_code GuestRemovalAlias(const HostStorageRoot&, const struct stat&);
struct Fd {
  int value = -1;
  explicit Fd(int fd = -1) : value(fd) {}
  Fd(const Fd&) = delete;
  Fd& operator=(const Fd&) = delete;
  Fd(Fd&& other) noexcept : value(other.Release()) {}
  Fd& operator=(Fd&& other) noexcept {
    if (this != &other) {
      if (value >= 0) {
        close(value);
      }
      value = other.Release();
    }
    return *this;
  }
  ~Fd() {
    if (value >= 0) {
      close(value);
    }
  }
  int Release() {
    const int fd = value;
    value = -1;
    return fd;
  }
};
struct MountIdentity {
  uint64_t id = 0;
#if XE_PLATFORM_MAC
  fsid_t filesystem{};
  std::string mountpoint;
  std::string filesystem_type;
  bool local = false;
#endif
};
std::error_code MountForFd(int fd, MountIdentity& output) {
#if XE_PLATFORM_LINUX
#if defined(SYS_statx) && defined(STATX_MNT_ID)
  struct statx info{};
  if (syscall(SYS_statx, fd, "", AT_EMPTY_PATH | AT_SYMLINK_NOFOLLOW,
              STATX_MNT_ID, &info)) {
    return NativeError();
  }
  if (!(info.stx_mask & STATX_MNT_ID)) {
    return Error(std::errc::operation_not_supported);
  }
  output.id = info.stx_mnt_id;
  return {};
#else
  return Error(std::errc::operation_not_supported);
#endif
#elif XE_PLATFORM_MAC
  struct statfs info{};
  if (fstatfs(fd, &info)) {
    return NativeError();
  }
  output.filesystem = info.f_fsid;
  const auto path_end = std::find(std::begin(info.f_mntonname),
                                  std::end(info.f_mntonname), '\0');
  const auto type_end = std::find(std::begin(info.f_fstypename),
                                  std::end(info.f_fstypename), '\0');
  if (path_end == std::end(info.f_mntonname) ||
      type_end == std::end(info.f_fstypename)) {
    return Error(std::errc::value_too_large);
  }
  output.mountpoint.assign(std::begin(info.f_mntonname), path_end);
  output.filesystem_type.assign(std::begin(info.f_fstypename), type_end);
  output.local = bool(info.f_flags & MNT_LOCAL);
  // Lossless native FSID bits, not an invented generation or a path hash.
  output.id = (uint64_t(uint32_t(info.f_fsid.val[0])) << 32) |
              uint32_t(info.f_fsid.val[1]);
  return {};
#else
  return Error(std::errc::operation_not_supported);
#endif
}
bool SameMount(const MountIdentity& a, const MountIdentity& b) {
#if XE_PLATFORM_MAC
  return a.filesystem.val[0] == b.filesystem.val[0] &&
         a.filesystem.val[1] == b.filesystem.val[1] &&
         a.mountpoint == b.mountpoint &&
         a.filesystem_type == b.filesystem_type && a.local == b.local;
#else
  return a.id == b.id;
#endif
}
std::error_code ConfinedMount(int fd, const MountIdentity& root) {
  MountIdentity current;
  const auto error = MountForFd(fd, current);
  return error                      ? error
         : SameMount(current, root) ? std::error_code{}
                                    : Error(std::errc::cross_device_link);
}
#if XE_PLATFORM_MAC
struct DarwinDirectoryIdentity {
  dev_t device = 0;
  fsid_t filesystem{};
  uint64_t file = 0, parent = 0;
  bool operator==(const DarwinDirectoryIdentity& other) const {
    return device == other.device && file == other.file &&
           parent == other.parent &&
           filesystem.val[0] == other.filesystem.val[0] &&
           filesystem.val[1] == other.filesystem.val[1];
  }
};
struct DarwinRootProof {
  DarwinDirectoryIdentity identity;
  std::filesystem::path physical_path;
  std::vector<uint64_t> ancestors;
  bool operator==(const DarwinRootProof&) const = default;
};
bool SameDarwinVolume(const DarwinDirectoryIdentity& a,
                      const DarwinDirectoryIdentity& b) {
  return a.device == b.device &&
         a.filesystem.val[0] == b.filesystem.val[0] &&
         a.filesystem.val[1] == b.filesystem.val[1];
}
std::error_code DarwinDirectoryForFd(int fd,
                                     DarwinDirectoryIdentity& output) {
#if defined(FSOPT_RETURN_REALDEV) && defined(ATTR_CMN_RETURNED_ATTRS) && \
    defined(ATTR_CMN_FILEID) && defined(ATTR_CMN_PARENTID) && \
    defined(F_FULLFSYNC)
  constexpr attrgroup_t required = ATTR_CMN_DEVID | ATTR_CMN_FSID |
                                   ATTR_CMN_FILEID | ATTR_CMN_PARENTID;
  struct attrlist request{};
  request.bitmapcount = ATTR_BIT_MAP_COUNT;
  request.commonattr = ATTR_CMN_RETURNED_ATTRS | required;
  // Darwin packs even 64-bit attributes at four-byte boundaries. Read scalars
  // with memcpy; never reinterpret this buffer as a naturally aligned struct.
  static_assert(sizeof(attribute_set_t) % 4 == 0 && sizeof(dev_t) % 4 == 0 &&
                sizeof(fsid_t) % 4 == 0);
  constexpr size_t length = sizeof(uint32_t) + sizeof(attribute_set_t) +
                            sizeof(dev_t) + sizeof(fsid_t) + 2 * sizeof(uint64_t);
  std::array<uint8_t, length> raw{};
  if (fgetattrlist(fd, &request, raw.data(), raw.size(), FSOPT_RETURN_REALDEV)) {
    return NativeError();
  }
  size_t offset = 0;
  const auto take = [&](auto& value) {
    std::memcpy(&value, raw.data() + offset, sizeof(value));
    offset += sizeof(value);
  };
  uint32_t returned_length = 0;
  attribute_set_t returned{};
  DarwinDirectoryIdentity found;
  take(returned_length);
  take(returned);
  if (returned_length != length ||
      (returned.commonattr & required) != required ||
      (returned.commonattr & ~(required | ATTR_CMN_RETURNED_ATTRS)) ||
      returned.volattr || returned.dirattr || returned.fileattr ||
      returned.forkattr) {
    return Error(std::errc::operation_not_supported);
  }
  take(found.device);
  take(found.filesystem);
  take(found.file);
  take(found.parent);
  struct stat native;
  MountIdentity mount;
  if (fstat(fd, &native)) {
    return NativeError();
  }
  auto error = MountForFd(fd, mount);
  if (error) {
    return error;
  }
  // The first backend admits only local filesystems with a proved physical
  // namespace. Unknown/nullfs/network topology is not a path-only fallback.
  if (!S_ISDIR(native.st_mode) || !mount.local ||
      (mount.filesystem_type != "apfs" && mount.filesystem_type != "hfs") ||
      !mount.id || !found.file ||
      found.device != native.st_dev || found.file != uint64_t(native.st_ino) ||
      found.filesystem.val[0] != mount.filesystem.val[0] ||
      found.filesystem.val[1] != mount.filesystem.val[1]) {
    return Error(std::errc::operation_not_supported);
  }
  output = found;
  return {};
#else
  static_cast<void>(fd);
  static_cast<void>(output);
  return Error(std::errc::operation_not_supported);
#endif
}
std::error_code DarwinPathForFd(int fd, std::filesystem::path& output) {
#if defined(F_GETPATH_NOFIRMLINK)
  std::array<char, MAXPATHLEN> raw{};
  if (fcntl(fd, F_GETPATH_NOFIRMLINK, raw.data())) {
    return NativeError();
  }
  const auto end = std::find(raw.begin(), raw.end(), '\0');
  if (end == raw.end()) {
    return Error(std::errc::filename_too_long);
  }
  std::filesystem::path found(std::string(raw.begin(), end));
  if (!found.is_absolute() || found.lexically_normal() != found ||
      !Relative(found.relative_path(), true)) {
    return Error(std::errc::operation_not_supported);
  }
  output = std::move(found);
  return {};
#else
  static_cast<void>(fd);
  static_cast<void>(output);
  return Error(std::errc::operation_not_supported);
#endif
}
std::error_code DarwinProofForFd(int fd, DarwinRootProof& output) {
  DarwinRootProof found;
  auto error = DarwinDirectoryForFd(fd, found.identity);
  if (!error) {
    error = DarwinPathForFd(fd, found.physical_path);
  }
  if (error) {
    return error;
  }
  Fd current(open("/", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
  if (current.value < 0) {
    return NativeError();
  }
  DarwinDirectoryIdentity parent;
  error = DarwinDirectoryForFd(current.value, parent);
  if (error) {
    return error;
  }
  bool entered_volume = SameDarwinVolume(parent, found.identity);
  if (entered_volume) {
    found.ancestors.push_back(parent.file);
  }
  size_t depth = 0;
  for (const auto& part : found.physical_path.relative_path()) {
    if (++depth > 64) {
      return Error(std::errc::value_too_large);
    }
    Fd next(openat(current.value, part.c_str(),
                   O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    if (next.value < 0) {
      return NativeError();
    }
    DarwinDirectoryIdentity child;
    error = DarwinDirectoryForFd(next.value, child);
    if (error) {
      return error;
    }
    if (SameDarwinVolume(parent, child) && child.parent != parent.file) {
      return Error(std::errc::state_not_recoverable);
    }
    if (SameDarwinVolume(child, found.identity)) {
      entered_volume = true;
      if (std::find(found.ancestors.begin(), found.ancestors.end(), child.file) !=
          found.ancestors.end()) {
        return Error(std::errc::operation_not_supported);
      }
      found.ancestors.push_back(child.file);
    } else if (entered_volume) {
      return Error(std::errc::cross_device_link);
    }
    close(current.value);
    current.value = next.Release();
    parent = child;
  }
  DarwinDirectoryIdentity final_identity;
  std::filesystem::path final_path;
  error = DarwinDirectoryForFd(fd, final_identity);
  if (!error) {
    error = DarwinPathForFd(fd, final_path);
  }
  if (error) {
    return error;
  }
  if (!(parent == found.identity) || !(final_identity == found.identity) ||
      final_path != found.physical_path || found.ancestors.empty()) {
    return Error(std::errc::state_not_recoverable);
  }
  output = std::move(found);
  return {};
}
std::error_code DarwinProofCurrent(int fd, const DarwinRootProof& expected) {
  DarwinDirectoryIdentity identity;
  std::filesystem::path path;
  auto error = DarwinDirectoryForFd(fd, identity);
  if (!error) {
    error = DarwinPathForFd(fd, path);
  }
  return error ? error
         : identity == expected.identity && path == expected.physical_path
             ? std::error_code{}
             : Error(std::errc::no_such_device);
}
#endif
Fd PinAt(int parent, const char* name) {
#if XE_PLATFORM_LINUX
  return Fd(openat(parent, name, O_PATH | O_NOFOLLOW | O_CLOEXEC));
#else
  return Fd(
      openat(parent, name, O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC));
#endif
}
Fd Parent(int root, const std::filesystem::path& relative, std::string& leaf,
          std::error_code& error) {
  if (!Relative(relative, true)) {
    error = Error(std::errc::invalid_argument);
    return Fd();
  }
  Fd current(dup(root));
  if (current.value < 0) {
    error = NativeError();
    return Fd();
  }
  MountIdentity root_mount;
  error = MountForFd(root, root_mount);
  if (error) {
    return Fd();
  }
  if (relative.empty()) {
    leaf = ".";
    return current;
  }
  leaf = relative.filename().string();
  if (leaf.empty()) {
    error = Error(std::errc::invalid_argument);
    return Fd();
  }
  for (const auto& part : relative.parent_path()) {
    const int next = openat(current.value, part.c_str(),
                            O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (next < 0) {
      error = NativeError();
      return Fd();
    }
    error = ConfinedMount(next, root_mount);
    if (error) {
      close(next);
      return Fd();
    }
    close(current.value);
    current.value = next;
  }
  return current;
}
// Namespace barriers and data barriers are distinct on Darwin. A directory
// fsync is followed by a full flush on a proved SAME mounted-volume directory,
// matching Apple's boot-root publication pattern. Never report a weaker
// fallback as successful full durability.
int SyncStorage(int fd) {
#if XE_PLATFORM_MAC
  if (::fsync(fd)) {
    return -1;
  }
#if defined(F_FULLFSYNC)
  struct stat native;
  if (fstat(fd, &native)) {
    return -1;
  }
  if (!S_ISDIR(native.st_mode)) {
    return ::fcntl(fd, F_FULLFSYNC, 0);
  }
  MountIdentity mount;
  auto error = MountForFd(fd, mount);
  if (error) {
    errno = error.value();
    return -1;
  }
  const std::filesystem::path volume_path(mount.mountpoint);
  if (!volume_path.is_absolute() ||
      volume_path.lexically_normal() != volume_path ||
      !Relative(volume_path.relative_path(), true)) {
    errno = ENOTSUP;
    return -1;
  }
  Fd volume(open("/", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
  if (volume.value < 0) {
    return -1;
  }
  size_t depth = 0;
  for (const auto& part : volume_path.relative_path()) {
    if (++depth > 64) {
      errno = EOVERFLOW;
      return -1;
    }
    Fd next(openat(volume.value, part.c_str(),
                   O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    if (next.value < 0) {
      return -1;
    }
    close(volume.value);
    volume.value = next.Release();
  }
  MountIdentity actual;
  struct stat volume_info;
  error = MountForFd(volume.value, actual);
  if (error || fstat(volume.value, &volume_info)) {
    if (error) { errno = error.value(); }
    return -1;
  }
  if (!SameMount(mount, actual) || native.st_dev != volume_info.st_dev) {
    errno = EXDEV;
    return -1;
  }
  // The independently opened FD is used only as this volume's flush barrier;
  // it does not become a content root, publication target or guest authority.
  const int result = ::fcntl(volume.value, F_FULLFSYNC, 0);
  const int failure = errno;
  const int closed = close(volume.Release());
  if (result) { errno = failure; }
  return result ? result : closed;
#else
  errno = ENOTSUP;
  return -1;
#endif
#else
  return ::fsync(fd);
#endif
}
// Native directory ancestry is independent of guest spelling (including
// case-insensitive/Unicode aliases) and of an ancestor's pathname rename.
std::error_code DirectoryAncestors(int directory, dev_t device,
    const MountIdentity& mount, std::vector<uint64_t>& output) {
  Fd current(openat(directory, ".", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
  if (current.value < 0) { return NativeError(); }
  for (unsigned depth = 0; depth != 64; ++depth) {
    struct stat here;
    if (fstat(current.value, &here)) { return NativeError(); }
    if (here.st_dev != device) { return Error(std::errc::cross_device_link); }
    if (auto error = ConfinedMount(current.value, mount)) { return error; }
    output.push_back(uint64_t(here.st_ino));
    Fd parent(openat(current.value, "..", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    if (parent.value < 0) { return NativeError(); }
    struct stat above;
    MountIdentity parent_mount;
    if (fstat(parent.value, &above)) { return NativeError(); }
    if (auto error = MountForFd(parent.value, parent_mount)) { return error; }
    if (above.st_dev != device || parent_mount.id != mount.id ||
        above.st_ino == here.st_ino) { return {}; }
    if (std::find(output.begin(), output.end(), uint64_t(above.st_ino)) != output.end()) {
      return Error(std::errc::state_not_recoverable);
    }
    close(current.value);
    current.value = parent.Release();
  }
  return Error(std::errc::value_too_large);
}
struct RemovalBudget {
  size_t remaining = 65536;
  unsigned depth = 0;
};
struct RemovalNode {
  std::string name;
  struct stat identity{};
  Fd directory;
  std::vector<RemovalNode> children;
  bool preserve_directory = false;
};
bool SameNode(const struct stat& a, const struct stat& b) {
  return a.st_dev == b.st_dev && a.st_ino == b.st_ino &&
         (a.st_mode & S_IFMT) == (b.st_mode & S_IFMT);
}
// A flush retains readable descriptors, including files whose policy handle
// has already closed. O_PATH pins used by removal cannot provide data barriers.
struct FlushTreeNode {
  std::string name;
  struct stat identity{};
  Fd retained;
  std::vector<FlushTreeNode> children;
};
bool SameFlushTreeStat(const struct stat& a, const struct stat& b) {
  return SameNode(a, b) && a.st_mode == b.st_mode &&
      a.st_nlink == b.st_nlink && a.st_size == b.st_size &&
#if XE_PLATFORM_MAC
      a.st_mtimespec.tv_sec == b.st_mtimespec.tv_sec &&
      a.st_mtimespec.tv_nsec == b.st_mtimespec.tv_nsec &&
      a.st_ctimespec.tv_sec == b.st_ctimespec.tv_sec &&
      a.st_ctimespec.tv_nsec == b.st_ctimespec.tv_nsec;
#else
      a.st_mtim.tv_sec == b.st_mtim.tv_sec &&
      a.st_mtim.tv_nsec == b.st_mtim.tv_nsec &&
      a.st_ctim.tv_sec == b.st_ctim.tv_sec &&
      a.st_ctim.tv_nsec == b.st_ctim.tv_nsec;
#endif
}
std::error_code FlushTreeNodeCurrent(int parent, const FlushTreeNode& node,
                                      const MountIdentity& mount) {
  struct stat named, opened;
  if (fstatat(parent, node.name.c_str(), &named, AT_SYMLINK_NOFOLLOW) ||
      fstat(node.retained.value, &opened)) {
    return NativeError();
  }
  if (!SameFlushTreeStat(named, node.identity) ||
      !SameFlushTreeStat(opened, node.identity)) {
    return Error(std::errc::state_not_recoverable);
  }
  return ConfinedMount(node.retained.value, mount);
}
std::error_code PrepareFlushTree(int parent, std::string name, dev_t device,
    const MountIdentity& mount, FlushTreeNode& node, size_t& remaining,
    unsigned depth = 0) {
  if (!remaining || depth >= 64) {
    return Error(std::errc::value_too_large);
  }
  --remaining;
  node.name = std::move(name);
  if (fstatat(parent, node.name.c_str(), &node.identity,
              AT_SYMLINK_NOFOLLOW)) {
    return NativeError();
  }
  const bool directory = S_ISDIR(node.identity.st_mode);
  if ((!directory && !S_ISREG(node.identity.st_mode)) ||
      (!directory && (node.identity.st_nlink != 1 || node.identity.st_size < 0))) {
    return Error(std::errc::permission_denied);
  }
  if (node.identity.st_dev != device) {
    return Error(std::errc::cross_device_link);
  }
  node.retained.value = openat(parent, node.name.c_str(),
      O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK |
      (directory ? O_DIRECTORY : 0));
  if (node.retained.value < 0) {
    return NativeError();
  }
  if (auto error = FlushTreeNodeCurrent(parent, node, mount)) { return error; }
  if (!directory) { return {}; }
  // A fresh open description keeps each scan independent of previous scans;
  // dup would share the retained directory's seek offset.
  Fd scan(openat(node.retained.value, ".",
                 O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
  if (scan.value < 0) { return NativeError(); }
  DIR* stream = fdopendir(scan.value);
  if (!stream) { return NativeError(); }
  scan.Release();
  std::error_code error;
  while (true) {
    errno = 0;
    const auto* entry = readdir(stream);
    if (!entry) { if (errno) { error = NativeError(); } break; }
    if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) { continue; }
    FlushTreeNode child;
    error = PrepareFlushTree(node.retained.value, entry->d_name, device, mount,
                             child, remaining, depth + 1);
    if (error) { break; }
    node.children.push_back(std::move(child));
  }
  if (closedir(stream) && !error) { error = NativeError(); }
  if (error) { return error; }
  std::sort(node.children.begin(), node.children.end(),
      [](const auto& a, const auto& b) { return a.name < b.name; });
  for (size_t i = 1; i < node.children.size(); ++i) {
    if (node.children[i - 1].name == node.children[i].name) {
      return Error(std::errc::state_not_recoverable);
    }
  }
  return FlushTreeNodeCurrent(parent, node, mount);
}
std::error_code RevalidateFlushTree(int parent, const FlushTreeNode& node,
                                    const MountIdentity& mount) {
  if (auto error = FlushTreeNodeCurrent(parent, node, mount)) { return error; }
  if (!S_ISDIR(node.identity.st_mode)) { return {}; }
  Fd scan(openat(node.retained.value, ".",
                 O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
  if (scan.value < 0) { return NativeError(); }
  DIR* stream = fdopendir(scan.value);
  if (!stream) { return NativeError(); }
  scan.Release();
  std::error_code error;
  size_t count = 0;
  while (true) {
    errno = 0;
    const auto* entry = readdir(stream);
    if (!entry) { if (errno) { error = NativeError(); } break; }
    if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) { continue; }
    const std::string_view name(entry->d_name);
    const auto found = std::lower_bound(node.children.begin(), node.children.end(),
        name, [](const auto& child, auto leaf) { return child.name < leaf; });
    if (++count > node.children.size() || found == node.children.end() ||
        found->name != name) {
      error = Error(std::errc::state_not_recoverable);
      break;
    }
  }
  if (closedir(stream) && !error) { error = NativeError(); }
  if (error) { return error; }
  if (count != node.children.size()) {
    return Error(std::errc::state_not_recoverable);
  }
  for (const auto& child : node.children) {
    if (auto e = RevalidateFlushTree(node.retained.value, child, mount)) { return e; }
  }
  return FlushTreeNodeCurrent(parent, node, mount);
}
bool FlushTreeReserved(HostStorageSpace::MutationGuard& guard,
                        const std::filesystem::path& path,
                        const FlushTreeNode& node,
                        const std::shared_ptr<const HostStorageFileLease>& permitted = {},
                        bool clean_overwrite = false) {
  if (guard.Reserved(path, SnapshotInfo(node.identity), permitted, clean_overwrite)) { return true; }
  for (const auto& child : node.children) {
    if (FlushTreeReserved(guard, path / child.name, child, permitted, clean_overwrite)) { return true; }
  }
  return false;
}
std::error_code SyncFlushTree(const FlushTreeNode& node, bool directories) {
  for (const auto& child : node.children) {
    if (auto error = SyncFlushTree(child, directories)) { return error; }
  }
  if (S_ISDIR(node.identity.st_mode) == directories &&
      SyncStorage(node.retained.value)) {
    return NativeError();
  }
  return {};
}
std::error_code PreflightRemoval(int parent, std::string leaf, bool recursive,
                                 dev_t device, const MountIdentity& mount,
                                 RemovalNode& output,
                                 RemovalBudget* budget = nullptr,
                                 const HostStorageRoot* guest = nullptr,
                                 const std::vector<std::filesystem::path>* exclusions = nullptr,
                                 const std::filesystem::path& relative = {}) {
  if (budget) {
    if (!budget->remaining || budget->depth >= 64) {
      return Error(std::errc::value_too_large);
    }
    --budget->remaining;
  }
  output.name = std::move(leaf);
  if (fstatat(parent, output.name.c_str(), &output.identity,
              AT_SYMLINK_NOFOLLOW)) {
    return NativeError();
  }
  // This includes the final leaf, not merely its intermediate components.
  if (output.identity.st_dev != device) {
    return Error(std::errc::cross_device_link);
  }
  if (guest && S_ISREG(output.identity.st_mode) && output.identity.st_nlink > 1) {
    if (auto error = GuestRemovalAlias(*guest, output.identity)) { return error; }
  }
  // Symlinks are unlinked as leaves, never traversed. Every other leaf is
  // pinned even when st_dev matches: same-filesystem bind mounts are distinct.
  if (S_ISLNK(output.identity.st_mode)) {
    return {};
  }
  auto pinned = PinAt(parent, output.name.c_str());
  if (pinned.value < 0) {
    return NativeError();
  }
  auto mount_error = ConfinedMount(pinned.value, mount);
  if (mount_error) {
    return mount_error;
  }
  struct stat pinned_info;
  if (fstat(pinned.value, &pinned_info)) {
    return NativeError();
  }
  if (!SameNode(pinned_info, output.identity)) {
    return Error(std::errc::state_not_recoverable);
  }
  if (!S_ISDIR(output.identity.st_mode)) {
    output.directory.value = pinned.Release();
    return {};
  }
  output.directory.value =
      openat(parent, output.name.c_str(),
             O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (output.directory.value < 0) {
    return NativeError();
  }
  struct stat opened;
  if (fstat(output.directory.value, &opened)) {
    return NativeError();
  }
  mount_error = ConfinedMount(output.directory.value, mount);
  if (mount_error) {
    return mount_error;
  }
  if (opened.st_dev != device) {
    return Error(std::errc::cross_device_link);
  }
  if (!SameNode(opened, output.identity)) {
    return Error(std::errc::state_not_recoverable);
  }
  if (exclusions && std::find(exclusions->begin(), exclusions->end(), relative) != exclusions->end()) {
    output.preserve_directory = true;
    return {};
  }
  if (!recursive) {
    return {};
  }
  const int iterator_fd = dup(output.directory.value);
  if (iterator_fd < 0) {
    return NativeError();
  }
  DIR* stream = fdopendir(iterator_fd);
  if (!stream) {
    const auto error = NativeError();
    close(iterator_fd);
    return error;
  }
  std::error_code error;
  while (true) {
    errno = 0;
    const auto* entry = readdir(stream);
    if (!entry) {
      if (errno) {
        error = NativeError();
      }
      break;
    }
    if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) {
      continue;
    }
    RemovalNode child;
    if (budget) {
      ++budget->depth;
    }
    error = PreflightRemoval(output.directory.value, entry->d_name, true,
                             device, mount, child, budget, guest, exclusions,
                             relative / entry->d_name);
    if (budget) {
      --budget->depth;
    }
    if (error) {
      break;
    }
    output.preserve_directory = output.preserve_directory || child.preserve_directory;
    output.children.push_back(std::move(child));
  }
  closedir(stream);
  if (error) {
    return error;
  }
  struct stat current;
  if (fstatat(parent, output.name.c_str(), &current, AT_SYMLINK_NOFOLLOW)) {
    return NativeError();
  }
  return SameNode(current, output.identity)
             ? std::error_code{}
             : Error(std::errc::state_not_recoverable);
}
std::error_code RemovePrepared(int parent, const RemovalNode& node,
                               uint64_t* removed = nullptr) {
  struct stat current;
  if (fstatat(parent, node.name.c_str(), &current, AT_SYMLINK_NOFOLLOW)) {
    return NativeError();
  }
  if (!SameNode(current, node.identity)) {
    return Error(std::errc::state_not_recoverable);
  }
  // Traversal uses the descriptors retained during the complete preflight.
  // A later path/mount replacement cannot redirect traversal into its tree.
  for (const auto& child : node.children) {
    const auto error = RemovePrepared(node.directory.value, child, removed);
    if (error) {
      return error;
    }
  }
  if (node.preserve_directory) { return {}; }
  if (unlinkat(parent, node.name.c_str(),
               S_ISDIR(node.identity.st_mode) ? AT_REMOVEDIR : 0)) {
    return NativeError();
  }
  if (removed) {
    ++*removed;
  }
  return {};
}

// The outer subtree check covers handles opened by descendants' current names.
// Check every prepared identity too: an alias outside that pathname may hold
// the same file open. The common guard excludes new cooperating opens through
// the entire preflight/removal interval.
bool RemovalLeaseConflict(HostStorageSpace::MutationGuard& guard,
                          const std::filesystem::path& path,
                          const RemovalNode& node,
                          const std::shared_ptr<const HostStorageFileLease>& permitted = {}) {
  if (guard.Conflicts(path, false, node.identity.st_dev,
                       node.identity.st_ino,permitted)) {
    return true;
  }
  for (const auto& child : node.children) {
    if (RemovalLeaseConflict(guard,path/child.name,child,permitted)) { return true; }
  }
  return false;
}

std::error_code CheckResetContents(int parent, const RemovalNode& directory,
                                   const std::string& retained,
                                   const struct stat* retained_info) {
  struct stat current;
  if (fstatat(parent, directory.name.c_str(), &current, AT_SYMLINK_NOFOLLOW)) {
    return NativeError();
  }
  if (!SameNode(current, directory.identity)) {
    return Error(std::errc::state_not_recoverable);
  }
  const int scan = dup(directory.directory.value);
  if (scan < 0) { return NativeError(); }
  DIR* stream = fdopendir(scan);
  if (!stream) { const auto error = NativeError(); close(scan); return error; }
  rewinddir(stream);
  std::error_code error;
  bool found_retained = false;
  while (!error) {
    errno = 0;
    const auto* entry = readdir(stream);
    if (!entry) { if (errno) { error = NativeError(); } break; }
    if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) { continue; }
    if (!retained_info || retained != entry->d_name || found_retained) {
      error = Error(std::errc::state_not_recoverable); break;
    }
    if (fstatat(directory.directory.value, entry->d_name, &current,
                 AT_SYMLINK_NOFOLLOW)) { error = NativeError(); break; }
    if (!SameNode(current, *retained_info)) {
      error = Error(std::errc::state_not_recoverable); break;
    }
    found_retained = true;
  }
  closedir(stream);
  if (!error && found_retained != bool(retained_info)) {
    error = Error(std::errc::state_not_recoverable);
  }
  return error;
}

#endif
}  // namespace
struct HostStorageDirectoryIdentity::State {
#if !XE_PLATFORM_WIN32
  struct Pin {
    Fd native;
    uint64_t filesystem = 0, directory = 0;
    explicit Pin(Fd fd, uint64_t fs, uint64_t ino)
        : native(std::move(fd)), filesystem(fs), directory(ino) {}
  };
  struct Step {
    std::shared_ptr<const Pin> pin;
    std::filesystem::path name;
    uint64_t mount=0;
    bool alias_edge=false;
  };
  std::vector<Step> lineage;
#endif
  std::filesystem::path path;
};
HostStorageDirectoryIdentity::HostStorageDirectoryIdentity(
    std::unique_ptr<State> state) : state_(std::move(state)) {}
HostStorageDirectoryIdentity::~HostStorageDirectoryIdentity() = default;
const std::filesystem::path& HostStorageDirectoryIdentity::path() const {
  return state_->path;
}
std::shared_ptr<const HostStorageDirectoryIdentity>
HostStorageDirectoryIdentity::Capture(const std::filesystem::path& path,
                                     std::error_code& error) {
  error.clear();
  if (!NativeSpelling(path)) {
    error = Error(std::errc::invalid_argument); return {};
  }
  // Native observation/preparation is never performed by the metadata-only
  // namespace reader. A caller that skipped preparation gets a truthful BUSY.
  if (xe::global_critical_region::is_held_by_current_thread()) {
    error=Error(std::errc::device_or_resource_busy); return {};
  }
#if XE_PLATFORM_WIN32
  error = Error(std::errc::operation_not_supported); return {};
#else
  if (!path.is_absolute() || path.lexically_normal() != path ||
      !Relative(path.relative_path(), true)) {
    error = Error(std::errc::invalid_argument); return {};
  }
  auto state = std::make_unique<State>();
  state->path = path;
  // Pins are shared only after independently opening and fstat'ing a native
  // directory. Weak pool entries convey no lifetime; a live Pin prevents reuse.
  static std::mutex pool_mutex;
  static std::map<std::pair<uint64_t,uint64_t>,
                  std::weak_ptr<const State::Pin>> pool;
  auto retain = [&](Fd fd, std::filesystem::path name) -> bool {
    struct stat info;
    if (fd.value < 0 || fstat(fd.value, &info)) {
      error = NativeError(); return false;
    }
    if (!S_ISDIR(info.st_mode)) {
      error = Error(std::errc::not_a_directory); return false;
    }
    MountIdentity mount;
    if (auto observed=MountForFd(fd.value,mount)) { error=observed; return false; }
    const auto key = std::make_pair(uint64_t(info.st_dev), uint64_t(info.st_ino));
    // A bind mounted across filesystem parents changes BOTH mount and dev.
    // Record every actual mount edge; final-FS equality is checked only when
    // assessing ambiguity against another retained namespace identity.
    const bool alias_edge=!state->lineage.empty() &&
        state->lineage.back().mount!=mount.id;
    auto fresh = std::make_shared<State::Pin>(std::move(fd),key.first,key.second);
    std::shared_ptr<const State::Pin> held;
    {
      std::lock_guard lock(pool_mutex);
      std::erase_if(pool, [](const auto& item) { return item.second.expired(); });
      auto& weak = pool[key];
      held = weak.lock();
      if (!held) { held = fresh; weak = held; }
    }
    state->lineage.push_back({std::move(held),std::move(name),mount.id,alias_edge});
    return true;
  };
  Fd current(open("/", O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC));
  if (current.value<0) { error=NativeError(); return {}; }
  if (!retain(Fd(dup(current.value)), {})) { return {}; }
  for (const auto& part : path.relative_path()) {
    if (state->lineage.size() == 65) {
      error = Error(std::errc::value_too_large); return {};
    }
    // Walk THIS observed mount path, never a pooled alias's descriptor. Pins
    // are pooled for object lifetime only, not for traversal or permissions.
    Fd next(openat(current.value, part.c_str(),
                  O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC));
    if (next.value<0) { error=NativeError(); return {}; }
    if (!retain(Fd(dup(next.value)),part)) { return {}; }
    current=std::move(next);
  }
  return std::shared_ptr<const HostStorageDirectoryIdentity>(
      new HostStorageDirectoryIdentity(std::move(state)));
#endif
}
bool HostStorageDirectoryIdentity::MatchesNativeDirectory(
    uint64_t filesystem, uint64_t directory) const {
#if XE_PLATFORM_WIN32
  return false;
#else
  return !state_->lineage.empty() &&
      state_->lineage.back().pin->filesystem == filesystem &&
      state_->lineage.back().pin->directory == directory;
#endif
}
std::optional<std::filesystem::path> HostStorageDirectoryIdentity::RelativeEntry(
    const HostStorageDirectoryIdentity& other,
    const std::filesystem::path& other_entry) const {
  if (!Relative(other_entry,true)) { return std::nullopt; }
#if XE_PLATFORM_WIN32
  return std::nullopt;
#else
  const auto& own = state_->lineage.back().pin;
  // The deepest equivalent directory wins if the host exposes nested aliases.
  for (size_t n = other.state_->lineage.size(); n-- != 0;) {
    const auto& found = other.state_->lineage[n].pin;
    if (own->filesystem != found->filesystem || own->directory != found->directory) {
      continue;
    }
    std::filesystem::path result;
    for (size_t i=n+1; i<other.state_->lineage.size(); ++i) {
      result /= other.state_->lineage[i].name;
    }
    if (!other_entry.empty()) { result /= other_entry; }
    return result;
  }
  return std::nullopt;
#endif
}
bool HostStorageDirectoryIdentity::OverlapsKnown(
    const std::filesystem::path& entry,
    const HostStorageDirectoryIdentity& other,
    const std::filesystem::path& other_entry, bool structural,
    bool ascii_folded_leaf) const {
  if (!Relative(entry,true) || !Relative(other_entry,true)) { return false; }
  const auto contains = [](const std::filesystem::path& parent,
                           const std::filesystem::path& child) {
    if (parent.empty()) { return true; }
    const auto r = child.lexically_relative(parent);
    return !r.empty() && !r.is_absolute() && *r.begin() != "..";
  };
  const auto same = [ascii_folded_leaf](const std::filesystem::path& a,
                                       const std::filesystem::path& b) {
    if (a == b) { return true; }
    if (!ascii_folded_leaf || a.parent_path() != b.parent_path()) { return false; }
    auto x=a.filename().string(), y=b.filename().string();
    if (x.size()!=y.size()) { return false; }
    const auto fold=[](char c) { return c>='A'&&c<='Z' ? char(c+('a'-'A')) : c; };
    for (size_t i=0;i<x.size();++i) { if (fold(x[i])!=fold(y[i])) { return false; } }
    return true;
  };
  if (auto query = RelativeEntry(other,other_entry)) {
    return same(entry,*query) || contains(entry,*query) ||
        (structural && contains(*query,entry));
  }
  if (auto own = other.RelativeEntry(*this,entry)) {
    return same(*own,other_entry) || contains(*own,other_entry) ||
        (structural && contains(other_entry,*own));
  }
  return false;
}
bool HostStorageDirectoryIdentity::SameEntry(
    const std::filesystem::path& entry,
    const HostStorageDirectoryIdentity& other,
    const std::filesystem::path& other_entry, bool ascii_folded_leaf) const {
  if (!Relative(entry,true) || !Relative(other_entry,true)) { return false; }
  const auto equal=[ascii_folded_leaf](const std::filesystem::path& a,
                                      const std::filesystem::path& b) {
    if (a==b) { return true; }
    if (!ascii_folded_leaf || a.parent_path()!=b.parent_path()) { return false; }
    auto x=a.filename().string(), y=b.filename().string();
    if (x.size()!=y.size()) { return false; }
    for (size_t i=0;i<x.size();++i) {
      const auto fold=[](char c) { return c>='A'&&c<='Z' ? char(c+('a'-'A')) : c; };
      if (fold(x[i])!=fold(y[i])) { return false; }
    }
    return true;
  };
  if (auto query=RelativeEntry(other,other_entry)) { return equal(entry,*query); }
  if (auto own=other.RelativeEntry(*this,entry)) { return equal(*own,other_entry); }
  return false;
}
bool HostStorageDirectoryIdentity::Overlaps(
    const std::filesystem::path& entry,
    const HostStorageDirectoryIdentity& other,
    const std::filesystem::path& other_entry, bool structural,
    bool ascii_folded_leaf) const {
  if (!Relative(entry,true) || !Relative(other_entry,true)) { return true; }
  if (OverlapsKnown(entry,other,other_entry,structural,ascii_folded_leaf)) { return true; }
#if XE_PLATFORM_WIN32
  return true;
#else
  if (state_->lineage.back().pin->filesystem!=
      other.state_->lineage.back().pin->filesystem) { return false; }
  // Both ancestry maps are actual retained pins. A bind root whose physical
  // node is absent from the other map leaves underlying ancestry unknown.
  // Refuse that ambiguity; it can NEVER authorize a service or old-file IO.
  const auto unresolved=[](const State& a,const State& b) {
    for (const auto& step:a.lineage) {
      if (!step.alias_edge) { continue; }
      const bool matched=std::any_of(b.lineage.begin(),b.lineage.end(),
          [&](const auto& candidate) {
            return step.pin->filesystem==candidate.pin->filesystem &&
                   step.pin->directory==candidate.pin->directory;
          });
      if (!matched) { return true; }
    }
    return false;
  };
  return unresolved(*state_,*other.state_) || unresolved(*other.state_,*state_);
#endif
}
class HostStorageFileLease {
 public:
  mutable std::filesystem::path path;
  // Every exposed File lease and every nonzero namespace identity owns a
  // native pin. A lease may outlive its original handle; a stat-only inode may
  // be recycled and must never reserve an unrelated newly allocated file.
  // The table is weak, so retaining its actual admission owner forms no cycle.
  std::shared_ptr<const HostStorageSpace> space;
  // Reserved/Conflicts read these fields only under the filesystem Mutation
  // guard. Namespace-list readers inspect path/active only (or separate atomic
  // physical-entry metadata), never these mutable native identity fields.
  mutable uint64_t filesystem = 0, file = 0;
#if !XE_PLATFORM_WIN32
  mutable Fd identity_pin;
#endif
  bool exclusive = false, namespace_scope = false, content_io = false;
  // Only actual FlushLengthResult under this filesystem's Mutation guard
  // establishes a committed overwrite EOF. Any later write/resize clears it.
  mutable bool clean_overwrite = false;
  mutable uint64_t clean_size = 0;
  mutable std::atomic<bool> active{true};
  std::shared_ptr<const HostStorageFileLease> parent_permit;
  std::shared_ptr<const HostStorageDirectoryIdentity> namespace_origin;
  std::filesystem::path namespace_entry;
  mutable std::atomic<std::shared_ptr<const HostStorageDirectoryIdentity>> namespace_tree;
  bool ascii_folded_leaf = false;
};
namespace {
bool Permitted(const std::shared_ptr<const HostStorageFileLease>& lease,
               std::shared_ptr<const HostStorageFileLease> permitted) {
  for (size_t depth = 0; permitted && depth != 8; ++depth) {
    if (lease == permitted) { return true; }
    permitted = permitted->parent_permit;
  }
  return false;
}
bool NamespaceConflict(const HostStorageFileLease& lease,
    const std::shared_ptr<const HostStorageDirectoryIdentity>& query,
    const std::filesystem::path& relative, bool structural) {
  if (!lease.namespace_origin || !query || !Relative(relative,true)) { return true; }
  const auto tree=lease.namespace_tree.load(std::memory_order_acquire);
  return lease.namespace_origin->Overlaps(lease.namespace_entry,*query,relative,
      structural,lease.ascii_folded_leaf) ||
      (tree && tree->Overlaps({},*query,relative,structural));
}
bool NamespaceKnownConflict(const HostStorageFileLease& lease,
    const std::shared_ptr<const HostStorageDirectoryIdentity>& query,
    const std::filesystem::path& relative, bool structural) {
  if (!lease.namespace_origin || !query || !Relative(relative,true)) { return false; }
  const auto tree=lease.namespace_tree.load(std::memory_order_acquire);
  return lease.namespace_origin->OverlapsKnown(lease.namespace_entry,*query,relative,
      structural,lease.ascii_folded_leaf) ||
      (tree && tree->OverlapsKnown({},*query,relative,structural));
}
#if !XE_PLATFORM_WIN32
// Call only while the SAME filesystem Mutation guard is held. A publication
// may update its exact own entry, never an ancestor subtree or a foreign lease.
std::shared_ptr<const HostStorageFileLease> ExactNamespace(
    std::shared_ptr<const HostStorageFileLease> permitted,
    const std::filesystem::path& path) {
  for (size_t depth = 0; permitted && depth != 8; ++depth) {
    if (permitted->namespace_scope &&
        permitted->active.load(std::memory_order_acquire) &&
        permitted->path == path) {
      return permitted;
    }
    permitted = permitted->parent_permit;
  }
  return {};
}
#endif
bool Descendant(const std::filesystem::path& child,
                const std::filesystem::path& parent) {
  const auto relative = child.lexically_relative(parent);
  return !relative.empty() && !relative.is_absolute() && *relative.begin() != "..";
}
}
namespace {
// BasicLockable with unchanged ordinary lock/try_lock semantics. All original
// users (including Observe and reserve pooling) release through unlock, so an
// admission waiter is notified without polling or losing an unlock race.
class StorageMutex final {
 public:
  void lock() {
    std::unique_lock lock(mutex_);
    available_.wait(lock, [&] { return !held_; });
    held_ = true;
  }
  bool try_lock() {
    std::unique_lock lock(mutex_, std::try_to_lock);
    if (!lock || held_) return false;
    held_ = true;
    return true;
  }
  bool lock_until(const HostStoragePreparationWait& preparation) {
    std::unique_lock lock(mutex_);
    if (!available_.wait_until(lock, preparation.token(), preparation.deadline(),
                               [&] { return !held_; }) || preparation.Check()) {
      return false;
    }
    held_ = true;
    return true;
  }
  void unlock() {
    { std::lock_guard lock(mutex_); held_ = false; }
    available_.notify_all();
  }
 private:
  std::mutex mutex_;
  std::condition_variable_any available_;
  bool held_ = false;
};
}  // namespace
bool HostStoragePreparationWait::LinkCancellation(std::stop_token token) {
  if (!token.stop_possible()) return true;
  if (links_.size() == 4096) return false;
  links_.push_back(std::make_unique<std::stop_callback<ForwardStop>>(
      token, ForwardStop{&stop_}));
  tokens_.push_back(std::move(token));
  return true;
}
bool HostStoragePreparationWait::LinkCancellationFlag(std::shared_ptr<const std::atomic<bool>> flag) {
  if (!flag) return true;
  if (flags_.size()==4096) return false;
  flags_.push_back(std::move(flag));
  return true;
}
std::error_code HostStoragePreparationWait::Check() const {
  if (stop_.stop_requested()) return Error(std::errc::operation_canceled);
  for (const auto& flag:flags_) if (flag->load(std::memory_order_acquire)) return Error(std::errc::operation_canceled);
  if (std::chrono::steady_clock::now() >= deadline_) return Error(std::errc::timed_out);
  return {};
}
void HostStoragePreparationWait::PauseForExternalLock() const {
  // flock has no portable cancellable timed-lock API. Only the LOCK_NB lock
  // acquisition is retried; never the file open/IO or complete preparation.
  std::mutex mutex;
  std::condition_variable_any wake;
  std::unique_lock lock(mutex);
  wake.wait_until(lock, token(), std::min(deadline_,
      std::chrono::steady_clock::now() + std::chrono::milliseconds(10)),
      [] { return false; });
}
struct HostStorageSpace::State {
  mutable StorageMutex mutex;
  std::atomic<const void*> guard_thread{nullptr};
  uint64_t reserve = 0;
  bool isolated = false;
  uint64_t filesystem_id = 0;
  Query query;
  std::vector<std::weak_ptr<const HostStorageFileLease>> open_files;
  mutable std::mutex namespace_mutex;
  std::vector<std::weak_ptr<const HostStorageFileLease>> content_namespaces;
#if !XE_PLATFORM_WIN32
  int lock_fd = -1;
  ~State() {
    if (lock_fd >= 0) {
      close(lock_fd);
    }
  }
#endif
  HostStorageSnapshot Observe(const std::filesystem::path& path) const {
    HostStorageSnapshot result;
    result.read_only = false;
    result.reserve_bytes = reserve;
    const auto info = query(path, result.error);
    if (result.error || info.capacity == UINTMAX_MAX ||
        info.available == UINTMAX_MAX) {
      if (!result.error) {
        result.error = Error(std::errc::io_error);
      }
      return result;
    }
    result.available = true;
    result.capacity_bytes = info.capacity;
    result.available_bytes = info.available;
    result.usable_bytes =
        info.available > reserve ? info.available - reserve : 0;
    return result;
  }
};
struct HostStorageSpace::MutationGuard::State {
  std::shared_ptr<HostStorageSpace> owner;
  std::filesystem::path path;
  std::shared_ptr<const HostStorageDirectoryIdentity> directory_identity;
  std::unique_lock<StorageMutex> lock;
  bool process_locked = false;
  State(std::shared_ptr<HostStorageSpace> value, std::filesystem::path root,
        std::shared_ptr<const HostStorageDirectoryIdentity> identity = {},
        bool try_only = false, HostStoragePreparationWait* preparation = nullptr)
      : owner(std::move(value)),
        path(std::move(root)),
        directory_identity(std::move(identity)),
        lock(owner->state_->mutex, std::defer_lock) {
    if (preparation) {
      if (!owner->state_->mutex.lock_until(*preparation)) return;
      lock = std::unique_lock<StorageMutex>(owner->state_->mutex, std::adopt_lock);
    } else if (try_only) {
      if (!lock.try_lock()) {
        return;
      }
    } else {
      lock.lock();
    }
    if (lock.owns_lock()) {
      owner->state_->guard_thread.store(StorageThreadToken(),
                                        std::memory_order_release);
    }
  }
  ~State() {
#if !XE_PLATFORM_WIN32
    if (process_locked) {
      flock(owner->state_->lock_fd, LOCK_UN);
    }
#endif
    if (lock.owns_lock()) {
      owner->state_->guard_thread.store(nullptr, std::memory_order_release);
    }
  }
};
HostStorageSpace::HostStorageSpace(std::unique_ptr<State> state)
    : state_(std::move(state)) {}
HostStorageSpace::~HostStorageSpace() = default;
HostStorageSpace::MutationGuard::MutationGuard(std::unique_ptr<State> state)
    : state_(std::move(state)) {}
HostStorageSpace::MutationGuard::~MutationGuard() = default;
bool HostStorageSpace::MutationGuard::IsOwnedByCurrentThread() const noexcept {
  return state_ && state_->lock.owns_lock() &&
         state_->owner->state_->guard_thread.load(std::memory_order_acquire) ==
             StorageThreadToken();
}
std::error_code HostStorageSpace::MutationGuard::Admit(uint64_t bound) {
  const auto info = state_->owner->state_->Observe(state_->path);
  if (!info.available) {
    return info.error;
  }
  return bound <= info.usable_bytes ? std::error_code{}
                                    : Error(std::errc::no_space_on_device);
}
std::shared_ptr<const HostStorageFileLease>
HostStorageSpace::MutationGuard::TrackOpen(const std::filesystem::path& path,
                                           const FileHandleInfo& info,
                                           bool exclusive, bool namespace_scope,
                                           std::shared_ptr<const HostStorageFileLease> parent_permit,
                                           bool content_io, bool ascii_folded_leaf,
    std::shared_ptr<const HostStorageDirectoryIdentity> namespace_origin,
    std::filesystem::path namespace_entry,
    std::shared_ptr<const HostStorageDirectoryIdentity> namespace_tree) {
  auto token = std::make_shared<HostStorageFileLease>();
  token->space = state_->owner;
  token->path = path.lexically_normal();
  token->filesystem = info.filesystem_id;
  token->file = info.file_id;
  token->exclusive = exclusive;
  token->namespace_scope = namespace_scope;
  token->parent_permit = std::move(parent_permit);
  token->content_io = content_io;
  token->ascii_folded_leaf = ascii_folded_leaf;
  token->namespace_tree.store(std::move(namespace_tree),std::memory_order_relaxed);
  // A batch may guard a target profile while opening its immutable journal
  // in a different retained root on the same native admission owner. Preserve
  // that explicit coordinate for ordinary leases as well as namespace claims.
  // Never turn an incomplete/malformed explicit coordinate into an untracked
  // file or silently substitute the batch's unrelated profile root.
  if (namespace_scope || namespace_origin || !namespace_entry.empty()) {
    if (!namespace_origin || !Relative(namespace_entry, true) ||
        (namespace_origin->path() / namespace_entry).lexically_normal() !=
            token->path) {
      return {};
    }
    token->namespace_origin = std::move(namespace_origin);
    token->namespace_entry = std::move(namespace_entry);
  } else {
    token->namespace_origin = state_->directory_identity;
    token->namespace_entry = token->path.lexically_relative(state_->path);
  }
  auto& files = state_->owner->state_->open_files;
  std::erase_if(files, [](const auto& weak) { return weak.expired(); });
  files.push_back(token);
  if (namespace_scope) {
    auto& owner = *state_->owner->state_;
    std::lock_guard lock(owner.namespace_mutex);
    // Never acquire a temporary strong lease under namespace_mutex: its
    // last-owner rundown can close native identity pins. Inactive live rows
    // remain inert and are filtered by readers outside this mutex.
    std::erase_if(owner.content_namespaces, [](const auto& w) {
      return w.expired();
    });
    owner.content_namespaces.push_back(token);
  }
  return token;
}
bool HostStorageSpace::MutationGuard::Reserved(
    const std::filesystem::path& path, const FileHandleInfo& info,
    const std::shared_ptr<const HostStorageFileLease>& permitted,
    bool allow_clean_content_overwrite) {
  const auto normalized = path.lexically_normal();
  const auto relative = normalized.lexically_relative(state_->path);
  for (const auto& weak : state_->owner->state_->open_files) {
    const auto lease = weak.lock();
    if (lease && allow_clean_content_overwrite && permitted && permitted->content_io &&
        !lease->namespace_scope && lease->clean_overwrite && info.regular &&
        lease->filesystem == info.filesystem_id && lease->file == info.file_id &&
        lease->clean_size == info.size) { continue; }
    if (lease && !Permitted(lease, permitted) &&
        lease->active.load(std::memory_order_acquire) && lease->exclusive &&
        lease->namespace_scope &&
        NamespaceConflict(*lease,state_->directory_identity,relative,false)) {
      return true;
    }
    if (lease && !Permitted(lease, permitted) && lease->active.load(std::memory_order_acquire) && lease->exclusive &&
        ((lease->filesystem == info.filesystem_id &&
          lease->file == info.file_id) ||
         lease->path == normalized ||
         (lease->namespace_scope && Descendant(normalized, lease->path)))) {
      return true;
    }
  }
  return false;
}
bool HostStorageSpace::MutationGuard::Conflicts(
    const std::filesystem::path& path, bool subtree, uint64_t filesystem,
    uint64_t file,
    const std::shared_ptr<const HostStorageFileLease>& permitted,
    bool allow_existing_content_io,
    std::shared_ptr<const HostStorageDirectoryIdentity> query_identity,
    std::filesystem::path query_relative) {
  const auto normalized = path.lexically_normal();
  if (!query_identity) {
    query_identity=state_->directory_identity;
    query_relative=normalized.lexically_relative(state_->path);
  }
  auto& files = state_->owner->state_->open_files;
  std::erase_if(files, [](const auto& weak) { return weak.expired(); });
  for (const auto& weak : files) {
    const auto lease = weak.lock();
    if (!lease || Permitted(lease, permitted) ||
        !lease->active.load(std::memory_order_acquire)) {
      continue;
    }
    const bool known_coordinate=query_identity && lease->namespace_origin &&
        Relative(query_relative,true);
    const bool physical_match=lease->namespace_scope && known_coordinate &&
        NamespaceKnownConflict(*lease,query_identity,query_relative,subtree);
    const bool physical_namespace=lease->namespace_scope &&
        NamespaceConflict(*lease,query_identity,query_relative,subtree);
    if (known_coordinate && lease->namespace_scope && lease->content_io && allow_existing_content_io &&
        !subtree && permitted && !permitted->namespace_scope &&
        permitted->filesystem == filesystem && permitted->file == file &&
        (physical_match || normalized == lease->path || Descendant(normalized, lease->path))) {
      continue;  // Exact preexisting handle rights, never namespace rename.
    }
    if (physical_namespace) { return true; }
    if (lease->namespace_scope &&
        (lease->path == normalized || Descendant(normalized, lease->path) ||
         (subtree && Descendant(lease->path, normalized)))) { return true; }
    const bool identities =
        filesystem && file && lease->filesystem && lease->file;
    if (identities && lease->filesystem == filesystem && lease->file == file) {
      return true;
    }
    if (!identities && lease->path == normalized) {
      return true;
    }
    if (subtree) {
      const auto relative = lease->path.lexically_relative(normalized);
      if (!relative.empty() && !relative.is_absolute() &&
          *relative.begin() != "..") {
#if XE_PLATFORM_WIN32
        return true;
#else
        // A replaced source generation must not make an old detached handle
        // claim a new directory's files merely because its old path matches.
        struct stat current;
        if (!lstat(lease->path.c_str(), &current)) {
          if (uint64_t(current.st_dev) == lease->filesystem &&
              uint64_t(current.st_ino) == lease->file) {
            return true;
          }
        } else if (errno != ENOENT && errno != ENOTDIR) {
          return true;
        }
#endif
      }
    }
  }
  return false;
}
void HostStorageSpace::MutationGuard::Moved(
    const std::shared_ptr<const HostStorageFileLease>& lease,
    const std::filesystem::path& path) {
  if (lease) {
    lease->path = path.lexically_normal();
  }
}
std::shared_ptr<HostStorageSpace> HostStorageSpace::ForPath(
    const std::filesystem::path& path, uint64_t reserve, std::error_code& error,
    Query query) {
  error.clear();
  if (!NativeSpelling(path)) {
    error = Error(std::errc::invalid_argument); return {};
  }
  std::string identity;
#if XE_PLATFORM_WIN32
  wchar_t volume[MAX_PATH] = {};
  if (!GetVolumePathNameW(path.c_str(), volume, MAX_PATH)) {
    error = NativeError();
    return {};
  }
  DWORD serial;
  if (!GetVolumeInformationW(volume, nullptr, 0, &serial, nullptr, nullptr,
                             nullptr, 0)) {
    error = NativeError();
    return {};
  }
  identity = std::to_string(serial);
#else
  struct stat info;
  if (stat(path.c_str(), &info)) {
    error = NativeError();
    return {};
  }
  identity = std::to_string(uint64_t(info.st_dev));
#endif
  static std::mutex pool_mutex;
  static std::map<std::string, std::weak_ptr<HostStorageSpace>> pool;
  std::lock_guard pool_lock(pool_mutex);
  const bool isolated = bool(query);
  if (!isolated) {
    if (auto existing = pool[identity].lock()) {
      if (!reserve) {
        return existing;
      }
      std::lock_guard lock(existing->state_->mutex);
      existing->state_->reserve = std::max(existing->state_->reserve, reserve);
      return existing;
    }
  }
  auto state = std::make_unique<State>();
  state->reserve = reserve;
  state->isolated = isolated;
  state->filesystem_id = std::stoull(identity);
  state->query = query ? std::move(query) : [](const auto& root, auto& ec) {
    return std::filesystem::space(root, ec);
  };
#if !XE_PLATFORM_WIN32
  if (!isolated) {
    const auto directory = std::filesystem::canonical("/tmp", error) /
                           ("xenia-storage-locks-" + std::to_string(geteuid()));
    if (error) {
      return {};
    }
    struct stat temporary_info;
    if (stat(directory.parent_path().c_str(), &temporary_info)) {
      error = NativeError(); return {};
    }
    uint64_t temporary_reserve = temporary_info.st_dev == info.st_dev ? reserve : 0;
    if (auto protected_space = pool[std::to_string(uint64_t(temporary_info.st_dev))].lock()) {
      // Pool->space is the same existing lock order as reserve pooling above.
      std::lock_guard protected_lock(protected_space->state_->mutex);
      temporary_reserve = std::max(temporary_reserve, protected_space->state_->reserve);
    }
    const auto future_lock = directory / (identity + ".lock");
    struct stat existing_lock;
    if (lstat(future_lock.c_str(), &existing_lock)) {
      if (errno != ENOENT) { error = NativeError(); return {}; }
      const auto temporary_space = std::filesystem::space(directory.parent_path(), error);
      if (error) { return {}; }
      if (temporary_space.available < temporary_reserve ||
          temporary_space.available - temporary_reserve < kMetadataAllowance) {
        error = Error(std::errc::no_space_on_device); return {};
      }
    }
    if (mkdir(directory.c_str(), 0700) && errno != EEXIST) {
      error = NativeError();
      return {};
    }
    struct stat directory_info;
    if (lstat(directory.c_str(), &directory_info) ||
        !S_ISDIR(directory_info.st_mode) ||
        directory_info.st_uid != geteuid() || (directory_info.st_mode & 0077)) {
      error = Error(std::errc::permission_denied);
      return {};
    }
    const auto lock_path = directory / (identity + ".lock");
    state->lock_fd = open(lock_path.c_str(),
                          O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (state->lock_fd < 0) {
      error = NativeError();
      return {};
    }
    struct stat lock_info;
    if (fstat(state->lock_fd, &lock_info) || !S_ISREG(lock_info.st_mode) ||
        lock_info.st_uid != geteuid() || (lock_info.st_mode & 0077) ||
        lock_info.st_nlink != 1) {
      error = Error(std::errc::permission_denied);
      return {};
    }
  }
#endif
  auto result =
      std::shared_ptr<HostStorageSpace>(new HostStorageSpace(std::move(state)));
  if (!isolated) {
    pool[identity] = result;
  }
  return result;
}
bool HostStorageSpace::NativeAuthorityFor(uint64_t filesystem_id) const {
  return !state_->isolated && state_->filesystem_id == filesystem_id;
}
HostStorageSnapshot HostStorageSpace::Observe(
    const std::filesystem::path& path) const {
  if (!NativeSpelling(path)) {
    HostStorageSnapshot result; result.error = Error(std::errc::invalid_argument);
    return result;
  }
  std::lock_guard lock(state_->mutex);
  return state_->Observe(path);
}
std::unique_ptr<HostStorageSpace::MutationGuard> HostStorageSpace::Begin(
    const std::filesystem::path& path, std::error_code& error,
    std::shared_ptr<const HostStorageDirectoryIdentity> identity) {
  error.clear();
  if (!NativeSpelling(path)) {
    error = Error(std::errc::invalid_argument); return {};
  }
  if (identity && identity->path() != path) {
    error=Error(std::errc::invalid_argument); return {};
  }
  auto state = std::make_unique<MutationGuard::State>(shared_from_this(), path,
                                                    std::move(identity));
#if !XE_PLATFORM_WIN32
  if (state_->lock_fd >= 0) {
    while (flock(state_->lock_fd, LOCK_EX)) {
      if (errno != EINTR) {
        error = NativeError();
        return {};
      }
    }
    state->process_locked = true;
  }
#endif
  return std::unique_ptr<MutationGuard>(new MutationGuard(std::move(state)));
}
std::unique_ptr<HostStorageSpace::MutationGuard> HostStorageSpace::BeginPreparation(
    const std::filesystem::path& path, std::error_code& error,
    HostStoragePreparationWait& preparation,
    std::shared_ptr<const HostStorageDirectoryIdentity> identity,
    HostStorageFailureObservation* diagnostic) {
  error.clear();
  HostStorageFailureScope observation{diagnostic,error,1};
  if (diagnostic && !diagnostic->line) {
    diagnostic->space=uint64_t(reinterpret_cast<uintptr_t>(this));
    diagnostic->filesystem=state_->filesystem_id; diagnostic->available|=1;
  }
  observation.At(8);
  if (xe::global_critical_region::is_held_by_current_thread()) {
    error=Error(std::errc::device_or_resource_busy); observation.Capture(__LINE__); return {};
  }
  observation.At(1);
  if (!NativeSpelling(path) || (identity && identity->path()!=path)) {
    error=Error(std::errc::invalid_argument); observation.Capture(__LINE__); return {};
  }
  const auto owner=state_->guard_thread.load(std::memory_order_acquire);
  const auto request=StorageThreadToken();
  if (diagnostic && !diagnostic->line) {
    diagnostic->guard_owner=uint64_t(reinterpret_cast<uintptr_t>(owner));
    diagnostic->guard_request=uint64_t(reinterpret_cast<uintptr_t>(request));
    diagnostic->available|=4;
  }
  observation.At(2);
  if (owner==request) {
    error=Error(std::errc::device_or_resource_busy); observation.Capture(__LINE__); return {};
  }
  observation.At(6);
  if ((error=preparation.Check())) { observation.Capture(__LINE__); return {}; }
  auto state=std::make_unique<MutationGuard::State>(
      shared_from_this(),path,std::move(identity),false,&preparation);
  if (!state->lock.owns_lock() || (error=preparation.Check())) {
    if (!error) error=preparation.Check();
    observation.Capture(__LINE__); return {};
  }
#if !XE_PLATFORM_WIN32
  if (state_->lock_fd>=0) {
    observation.At(7);
    while (flock(state_->lock_fd,LOCK_EX|LOCK_NB)) {
      if (errno!=EWOULDBLOCK && errno!=EAGAIN && errno!=EINTR) {
        error=NativeError(); observation.Capture(__LINE__); return {};
      }
      if ((error=preparation.Check())) { observation.Capture(__LINE__); return {}; }
      preparation.PauseForExternalLock();
    }
    state->process_locked=true;
  }
#endif
  if ((error=preparation.Check())) { observation.Capture(__LINE__); return {}; }
  return std::unique_ptr<MutationGuard>(new MutationGuard(std::move(state)));
}
std::unique_ptr<HostStorageSpace::MutationGuard> HostStorageSpace::TryBegin(
    const std::filesystem::path& path, std::error_code& error,
    std::shared_ptr<const HostStorageDirectoryIdentity> identity,
    HostStorageFailureObservation* diagnostic) {
  error.clear();
  HostStorageFailureScope observation{diagnostic,error,1};
  if(diagnostic && !diagnostic->line){diagnostic->space=uint64_t(reinterpret_cast<uintptr_t>(this));
    diagnostic->filesystem=state_->filesystem_id;diagnostic->available|=1;}
  observation.At(1);
  if (!NativeSpelling(path)) {
    error = Error(std::errc::invalid_argument); observation.Capture(__LINE__); return {};
  }
  const auto guard_owner=state_->guard_thread.load(std::memory_order_acquire);
  const auto guard_request=StorageThreadToken();
  if(diagnostic && !diagnostic->line){diagnostic->guard_owner=uint64_t(reinterpret_cast<uintptr_t>(guard_owner));
    diagnostic->guard_request=uint64_t(reinterpret_cast<uintptr_t>(guard_request));diagnostic->available|=4;}
  observation.At(2);
  if (guard_owner == guard_request) {
    error = Error(std::errc::device_or_resource_busy);
    observation.Capture(__LINE__); return {};
  }
  observation.At(3);
  if (identity && identity->path() != path) {
    error=Error(std::errc::invalid_argument); observation.Capture(__LINE__); return {};
  }
  auto state = std::make_unique<MutationGuard::State>(
      shared_from_this(), path, std::move(identity), true);
  observation.At(4);
  if (!state->lock.owns_lock()) {
    error = Error(std::errc::device_or_resource_busy);
    observation.Capture(__LINE__); return {};
  }
#if !XE_PLATFORM_WIN32
  if (state_->lock_fd >= 0) {
    observation.At(5);
    if (flock(state_->lock_fd, LOCK_EX | LOCK_NB)) {
      error = errno == EWOULDBLOCK ? Error(std::errc::device_or_resource_busy)
                                   : NativeError();
      observation.Capture(__LINE__); return {};
    }
    state->process_locked = true;
  }
#endif
  return std::unique_ptr<MutationGuard>(new MutationGuard(std::move(state)));
}
namespace {
struct ProtectionLocation;
}
// One opaque metadata receipt shared by the exact stage/canonical registered
// protectors of a native bootstrap. Native pins prevent inode reuse. This is
// not a second registry or IO authority; entries remain in Protection::All.
struct BootstrapProtectionGroup {
  std::shared_ptr<const HostStorageDirectoryIdentity> parent_identity;
  uint64_t filesystem = 0, parent_directory = 0, mount = 0, directory = 0;
  std::filesystem::path target, staging;
  std::weak_ptr<const uint8_t> active_owner;  // only under Protection::All mutex
};
struct HostStorageRoot::State {
  std::filesystem::path path;
  bool read_only = true;
  std::atomic<bool> retired{false};
  std::stop_source retirement_stop;
  std::shared_ptr<const std::atomic<bool>> cache_owner_canceled,cache_slot_canceled;
  std::atomic<size_t> open_handles{0};
  std::shared_ptr<HostStorageSpace> space;
  std::shared_ptr<const HostStorageDirectoryIdentity> directory_identity;
  std::shared_ptr<const HostStorageRoot> parent;
  std::shared_ptr<const HostStorageRoot> dependency;
  bool flat_private = false;
  bool recovery_private = false;
  bool intent_private = false;
  bool content_mutation_private = false;
  bool bootstrap_private = false;
  std::weak_ptr<const uint8_t> bootstrap_owner;
  std::shared_ptr<BootstrapProtectionGroup> bootstrap_group;
  bool snapshot_namespace = false;
  std::shared_ptr<const HostStorageFileLease> content_permit;
  std::shared_ptr<const ProtectionLocation> private_location;
  std::shared_ptr<const HostStorageGuestMount> guest_mount;
  std::weak_ptr<const HostStorageGuestProjection> projection;
  bool projection_required = false;
  bool public_projection_scope = false;
  bool format_eligible = true;
#if XE_PLATFORM_WIN32
  HANDLE directory = INVALID_HANDLE_VALUE;
  ~State() {
    if (directory != INVALID_HANDLE_VALUE) {
      CloseHandle(directory);
    }
  }
#else
  int directory = -1;
  dev_t device = 0;
  ino_t inode = 0;
  MountIdentity mount;
#if XE_PLATFORM_MAC
  std::atomic<std::shared_ptr<const DarwinRootProof>> darwin_authority;
#endif
  ~State() {
    if (directory >= 0) {
      close(directory);
    }
  }
#endif
};

struct HostStorageBootstrapDirectory::State {
  std::shared_ptr<const HostStorageRoot> parent;
  std::shared_ptr<const HostStorageIntentLease> lease;
  std::shared_ptr<const uint8_t> owner = std::make_shared<const uint8_t>(0);
  std::shared_ptr<HostStorageRoot> root;
  std::vector<std::shared_ptr<HostStorageRoot>> protections;
  std::shared_ptr<HostStorageRoot> public_view;
  std::weak_ptr<const HostStorageGuestProjection> public_projection;
  std::filesystem::path target, staging;
  uint64_t native_directory = 0;
  bool canonical = false, durable = false, public_admitted = false;
  bool private_settled = false;
};

namespace {
struct ProtectionLocation {
  uint64_t filesystem = 0, inode = 0;
  std::filesystem::path canonical, filesystem_path;
  bool subtree = false;
  std::vector<uint64_t> physical_ancestors;
};
bool Below(const std::filesystem::path& parent,
           const std::filesystem::path& child) {
  const auto relative = child.lexically_relative(parent);
  return !relative.empty() && !relative.is_absolute() &&
         *relative.begin() != "..";
}
bool Overlaps(const ProtectionLocation& a, const ProtectionLocation& b) {
  if (a.canonical == b.canonical ||
      (a.subtree && Below(a.canonical, b.canonical)) ||
      (b.subtree && Below(b.canonical, a.canonical))) {
    return true;
  }
  if (a.filesystem != b.filesystem) {
    return false;
  }
  if ((a.subtree && std::find(b.physical_ancestors.begin(),
                             b.physical_ancestors.end(), a.inode) !=
                        b.physical_ancestors.end()) ||
      (b.subtree && std::find(a.physical_ancestors.begin(),
                             a.physical_ancestors.end(), b.inode) !=
                        a.physical_ancestors.end())) {
    return true;
  }
  return a.inode == b.inode || a.filesystem_path == b.filesystem_path ||
         (a.subtree && Below(a.filesystem_path, b.filesystem_path)) ||
         (b.subtree && Below(b.filesystem_path, a.filesystem_path));
}
#if XE_PLATFORM_LINUX
std::string MountUnescape(std::string_view text) {
  std::string output;
  for (size_t i = 0; i < text.size(); ++i) {
    if (text[i] == '\\' && i + 3 < text.size() && text[i + 1] >= '0' &&
        text[i + 1] <= '7' && text[i + 2] >= '0' && text[i + 2] <= '7' &&
        text[i + 3] >= '0' && text[i + 3] <= '7') {
      output.push_back(char((text[i + 1] - '0') * 64 + (text[i + 2] - '0') * 8 +
                            text[i + 3] - '0'));
      i += 3;
    } else {
      output.push_back(text[i]);
    }
  }
  return output;
}
#endif
}  // namespace
struct HostStorageGuestProjection::State {
  enum class GuestAdmission { kOrdinary, kSourceUnadmitted, kBootstrapAdmitted, kRetiredFormatObservation };
  std::atomic<GuestAdmission> guest_admission{GuestAdmission::kOrdinary};
  // Set only by actual native format drain/retirement, never logical Retire.
  std::atomic<bool> format_drained{false};
  std::shared_ptr<const HostStorageRoot> source;
  ProtectionLocation source_location;
  std::vector<std::pair<std::shared_ptr<const HostStorageRoot>, ProtectionLocation>> public_scopes;
  std::atomic<bool> sealed{false};
  struct Exclusion {
    std::shared_ptr<const HostStorageRoot> root;
    ProtectionLocation location;
    bool private_tree;
  };
  std::vector<Exclusion> exclusions;
};
HostStorageGuestProjection::HostStorageGuestProjection(std::unique_ptr<State> state)
    : state_(std::move(state)) {}
HostStorageGuestProjection::~HostStorageGuestProjection() = default;
namespace {
bool ContainsLocation(const ProtectionLocation& parent,
                      const ProtectionLocation& child) {
  if (parent.filesystem != child.filesystem) { return false; }
  if (parent.inode && parent.inode == child.inode) { return true; }
  if (std::find(child.physical_ancestors.begin(), child.physical_ancestors.end(),
                parent.inode) != child.physical_ancestors.end()) { return true; }
  return parent.subtree && !parent.filesystem_path.empty() &&
      !child.filesystem_path.empty() &&
      (parent.filesystem_path == child.filesystem_path ||
       Below(parent.filesystem_path, child.filesystem_path));
}
}  // namespace
struct HostStorageGuestMount::State {
  bool unknown = false;
  std::vector<ProtectionLocation> locations;
  std::vector<std::shared_ptr<const HostStorageGuestProjection>> projections;
  // Pinned roots/files make admission provenance owned across Clear/handles.
  std::vector<std::shared_ptr<HostStorageRoot>> roots;
  std::vector<std::unique_ptr<FileHandle>> files;
  std::vector<std::shared_ptr<const HostStorageRoot>> private_dependencies;
};
HostStorageGuestMount::HostStorageGuestMount(std::unique_ptr<State> state)
    : state_(std::move(state)) {}
HostStorageGuestMount::~HostStorageGuestMount() = default;
class HostStorageProtection {
 public:
  struct Registry {
    std::mutex mutex;
    std::vector<std::weak_ptr<const HostStorageRoot>> private_roots;
    std::vector<std::weak_ptr<const HostStorageGuestMount>> guests;
    std::vector<std::weak_ptr<const HostStorageGuestProjection>> projections;
  };
  static Registry& All() {
    static Registry value;
    return value;
  }
  static std::vector<std::shared_ptr<const HostStorageRoot>> Private() {
    auto& all = All();
    std::lock_guard lock(all.mutex);
    std::erase_if(all.private_roots, [](const auto& w) { return w.expired(); });
    std::vector<std::shared_ptr<const HostStorageRoot>> result;
    for (const auto& w : all.private_roots) {
      if (auto r = w.lock()) {
        result.push_back(std::move(r));
      }
    }
    return result;
  }
  static std::error_code ProjectionCurrent(const HostStorageGuestProjection& p) {
    auto error = p.state_->source->Validate();
    for (const auto& e : p.state_->exclusions) {
      if (!error) { error = e.root->Validate(); }
    }
    for (const auto& scope : p.state_->public_scopes) {
      if (!error) { error = scope.first->Validate(); }
    }
    return error;
  }
  static bool DrainedFormatGuest(const HostStorageGuestMount& guest, size_t i) {
    const auto& p=guest.state_->projections[i];
    return p && p->state_->format_drained.load(std::memory_order_acquire) &&
        p->state_->source->state_->retired.load(std::memory_order_acquire);
  }
  // Metadata-only under All. Owned old-generation mounts may remain pinned;
  // arbitrary raw/different-projection aliases are not part of that drain.
  static bool ForeignFormatGuest(const HostStorageGuestProjection& previous,
                                const HostStorageGuestMount& guest) {
    if (guest.state_->unknown) {return true;}
    for (size_t i=0;i<guest.state_->locations.size();++i) {
      if (DrainedFormatGuest(guest,i)) {continue;}
      const auto& location=guest.state_->locations[i];
      const bool overlaps=Overlaps(previous.state_->source_location,location) ||
          std::any_of(previous.state_->exclusions.begin(),previous.state_->exclusions.end(),
              [&](const auto& e) {return !e.private_tree && Overlaps(e.location,location);});
      if (overlaps && guest.state_->projections[i].get()!=&previous) {return true;}
    }
    return false;
  }
  static std::error_code CheckFormatGuests(const HostStorageGuestProjection& previous) {
    auto& all=All();std::lock_guard lock(all.mutex);
    for (const auto& weak:all.guests) {
      if (const auto guest=weak.lock();guest && ForeignFormatGuest(previous,*guest)) {
        return Error(std::errc::device_or_resource_busy);
      }
    }
    return {};
  }
  static bool Excluded(const HostStorageGuestProjection& p,
                       const ProtectionLocation& private_location) {
    return std::any_of(p.state_->exclusions.begin(), p.state_->exclusions.end(),
        [&](const auto& e) {
          return e.private_tree && ContainsLocation(e.location, private_location);
        });
  }
  // An explicit volume may contain its managed Content directory. Keep that
  // PUBLIC namespace in the common overlap/seal registry, but do not mistake
  // its proven physical ancestor for an exclusion of the source itself.
  // Equal/disjoint/descendant namespaces retain their existing exclusion.
  // No path spelling or caller flag establishes this relationship.
  static bool SourceNamespaceContainer(
      const HostStorageGuestProjection& p,
      const HostStorageGuestProjection::State::Exclusion& e) {
    const auto& source = p.state_->source_location;
    return !e.private_tree && e.location.filesystem == source.filesystem &&
        e.location.inode && source.inode && e.location.inode != source.inode &&
        std::find(source.physical_ancestors.begin(),
                  source.physical_ancestors.end(), e.location.inode) !=
            source.physical_ancestors.end();
  }
#if !XE_PLATFORM_WIN32
  static std::error_code ProjectedDirectory(const HostStorageGuestProjection& p,
      int directory, dev_t device, const MountIdentity& mount, bool structural,
      bool public_scope = false) {
    std::vector<uint64_t> ancestors;
    auto error = DirectoryAncestors(directory, device, mount, ancestors);
    if (error) { return error; }
    if (ancestors.empty()) { return Error(std::errc::state_not_recoverable); }
    for (const auto& e : p.state_->exclusions) {
      if ((public_scope && !e.private_tree) || SourceNamespaceContainer(p, e)) {
        continue;
      }
      if (e.location.filesystem != uint64_t(device)) { continue; }
      if (std::find(ancestors.begin(), ancestors.end(), e.location.inode) != ancestors.end() ||
          (structural && std::find(e.location.physical_ancestors.begin(),
                                    e.location.physical_ancestors.end(), ancestors.front()) !=
                          e.location.physical_ancestors.end())) {
        return Error(std::errc::permission_denied);
      }
    }
    return {};
  }
#endif
  static bool BackingProjected(const HostStorageGuestProjection& p,
                               const ProtectionLocation& backing) {
    if (ProjectionCurrent(p)) { return false; }
    if (ContainsLocation(p.state_->source_location, backing)) { return true; }
    return std::any_of(p.state_->public_scopes.begin(), p.state_->public_scopes.end(),
        [&](const auto& scope) { return ContainsLocation(scope.second, backing); });
  }
  static bool ProjectionPermits(
      const std::shared_ptr<const HostStorageGuestProjection>& projection,
      const ProtectionLocation& backing,
      const ProtectionLocation& private_location) {
    return projection && BackingProjected(*projection, backing) &&
           Excluded(*projection, private_location);
  }
  static bool Permits(const HostStorageGuestMount& mount, size_t i,
                      const ProtectionLocation& private_location) {
    // This exact old guest is irreversibly inert after native format drain.
    // It may retain pins without permanently denying a new private epoch.
    return DrainedFormatGuest(mount,i) ||
        ProjectionPermits(mount.state_->projections[i],
                          mount.state_->locations[i], private_location);
  }
  // Deny-only preflight for ordinary source-marker creation. The caller holds
  // this exact native filesystem's existing mutation guard, also used by
  // typed Protect before the registry. The source SH/EX directory lease spans
  // ordinary metadata creation versus bootstrap. No guest ownership,
  // projection seal or bootstrap receipt transfer is published by this check.
  static std::error_code CheckManagedSourceCreation(const HostStorageRoot& root) {
    auto error = root.CheckGuestAccess({});
    if (error) { return error; }
    const auto projection = root.Projection();
    ProtectionLocation source;
    error = Location(root, true, source);
    if (error) { return error; }
    if (projection && !BackingProjected(*projection, source)) {
      return Error(std::errc::permission_denied);
    }
    auto& all = All();
    std::lock_guard lock(all.mutex);
    for (const auto& weak : all.projections) {
      const auto p = weak.lock();
      if (!p) { continue; }
      if (p->state_->guest_admission.load(std::memory_order_acquire) ==
              HostStorageGuestProjection::State::GuestAdmission::kSourceUnadmitted &&
          (Overlaps(p->state_->source_location, source) ||
           std::any_of(p->state_->exclusions.begin(), p->state_->exclusions.end(),
               [&](const auto& e) { return !e.private_tree && Overlaps(e.location, source); }))) {
        return Error(std::errc::operation_in_progress);
      }
      for (const auto& exclusion : p->state_->exclusions) {
        if (exclusion.private_tree && Overlaps(exclusion.location, source) &&
            !ProjectionPermits(projection, source, exclusion.location)) {
          return Error(std::errc::permission_denied);
        }
      }
    }
    for (const auto& weak : all.private_roots) {
      const auto private_root = weak.lock();
      if (!private_root) { continue; }
      const auto& location = private_root->state_->private_location;
      if (!location) { return Error(std::errc::operation_not_supported); }
      if (Overlaps(*location, source) && !ProjectionPermits(projection, source, *location)) {
        return Error(std::errc::permission_denied);
      }
    }
    return {};
  }
  // Called only under this exact root filesystem's existing admission guard.
  static std::error_code Contents(const HostStorageRoot& root,
      const FileHandleInfo* guest, bool initial, size_t maximum,
      bool recovery, bool intents = false, bool content = false) {
#if XE_PLATFORM_LINUX || XE_PLATFORM_MAC
    struct stat directory;
    if (fstat(root.state_->directory, &directory)) {
      return NativeError();
    }
    if (directory.st_uid != geteuid() || (directory.st_mode & 077)) {
      return Error(std::errc::permission_denied);
    }
    Fd iterator(dup(root.state_->directory));
    if (iterator.value < 0) {
      return NativeError();
    }
    DIR* stream = fdopendir(iterator.value);
    if (!stream) {
      return NativeError();
    }
    iterator.Release();
    // dup shares the directory offset. Always rewind the owned scan stream.
    rewinddir(stream);
    std::error_code error;
    size_t count = 0;
    std::map<std::pair<uint64_t, uint64_t>, std::pair<size_t, nlink_t>> links;
    while (true) {
      errno = 0;
      const auto* entry = readdir(stream);
      if (!entry) {
        if (errno) {
          error = NativeError();
        }
        break;
      }
      if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) {
        continue;
      }
      if (++count > maximum) {
        error = Error(std::errc::value_too_large);
        break;
      }
      if (recovery || intents || content) {
        if (!(content ? ContentMutationLeaf(entry->d_name) : intents ? IntentLeaf(entry->d_name) : RecoveryLeaf(entry->d_name))) {
          error = Error(std::errc::permission_denied);
          break;
        }
      }
      struct stat info;
      if (fstatat(root.state_->directory, entry->d_name, &info,
                  AT_SYMLINK_NOFOLLOW)) {
        error = NativeError();
        break;
      }
      if (!S_ISREG(info.st_mode) || info.st_uid != geteuid() ||
          (info.st_mode & 077) || info.st_dev != directory.st_dev ||
          ((recovery || intents || content) && info.st_nlink != 1)) {
        error = Error(std::errc::permission_denied);
        break;
      }
      auto pinned = PinAt(root.state_->directory, entry->d_name);
      if (pinned.value < 0) {
        error = NativeError();
        break;
      }
      error = ConfinedMount(pinned.value, root.state_->mount);
      if (error) {
        break;
      }
      auto& inside = links[{uint64_t(info.st_dev), uint64_t(info.st_ino)}];
      ++inside.first;
      inside.second = info.st_nlink;
      if (guest && uint64_t(info.st_dev) == guest->filesystem_id &&
          uint64_t(info.st_ino) == guest->file_id) {
        error = Error(std::errc::permission_denied);
        break;
      }
    }
    closedir(stream);
    if (!error && initial) {
      for (const auto& [identity, inside] : links) {
        if (inside.first != inside.second) {
          return Error(std::errc::permission_denied);
        }
      }
    }
    return error;
#else
    return Error(std::errc::operation_not_supported);
#endif
  }
  static std::error_code Flat(const HostStorageRoot& root,
      const FileHandleInfo* guest = nullptr, bool initial = false) {
    return Contents(root, guest, initial, 64, false);
  }
  static std::error_code BootstrapContents(const HostStorageRoot& root,
      const FileHandleInfo* guest = nullptr) {
#if XE_PLATFORM_LINUX || XE_PLATFORM_MAC
    FlushTreeNode tree; size_t remaining = 512;
    auto error = PrepareFlushTree(root.state_->directory, ".", root.state_->device,
        root.state_->mount, tree, remaining);
    if (error) { return error; }
    const auto check = [&](const auto& self, const FlushTreeNode& node) -> std::error_code {
      if (node.identity.st_uid != geteuid() || (node.identity.st_mode & 077) ||
          (guest && uint64_t(node.identity.st_dev) == guest->filesystem_id &&
                    uint64_t(node.identity.st_ino) == guest->file_id)) {
        return Error(std::errc::permission_denied);
      }
      for (const auto& child : node.children) { if (auto e = self(self, child)) { return e; } }
      return {};
    };
    return check(check, tree);
#else
    return Error(std::errc::operation_not_supported);
#endif
  }
  static std::error_code PrivateContents(const HostStorageRoot& root,
      const FileHandleInfo* guest = nullptr, bool initial = false) {
    if (root.state_->bootstrap_private) { return BootstrapContents(root, guest); }
    return Contents(root, guest, initial,
                    (root.is_recovery_private() || root.is_profile_intent_private() || root.is_content_mutation_private())
                        ? 8192 : 64,
                    root.is_recovery_private(), root.is_profile_intent_private(), root.is_content_mutation_private());
  }
  static std::error_code Location(const HostStorageRoot& root, bool subtree,
                                  ProtectionLocation& output) {
#if XE_PLATFORM_LINUX
    output.filesystem = root.state_->device;
    output.inode = root.state_->inode;
    output.canonical = root.state_->path;
    output.subtree = subtree;
    if (auto error = DirectoryAncestors(root.state_->directory, root.state_->device,
                                        root.state_->mount, output.physical_ancestors)) {
      return error;
    }
    // Mount-ID is pinned by the owned directory FD. Decode that mount's
    // filesystem-root coordinate; path spelling alone misses same-FS binds.
    std::ifstream mounts("/proc/self/mountinfo");
    if (!mounts) {
      return Error(std::errc::operation_not_supported);
    }
    std::string line;
    size_t lines = 0;
    while (std::getline(mounts, line)) {
      if (++lines > 4096) {
        return Error(std::errc::value_too_large);
      }
      if (line.size() > 65536) {
        return Error(std::errc::value_too_large);
      }
      std::istringstream fields(line);
      uint64_t id, parent;
      std::string device, base, mount;
      if (!(fields >> id >> parent >> device >> base >> mount) ||
          id != root.state_->mount.id) {
        continue;
      }
      const std::filesystem::path mount_path = MountUnescape(mount);
      const auto relative = root.state_->path.lexically_relative(mount_path);
      if (relative.empty() || relative.is_absolute() ||
          *relative.begin() == "..") {
        return Error(std::errc::operation_not_supported);
      }
      output.filesystem_path =
          (std::filesystem::path(MountUnescape(base)) / relative)
              .lexically_normal();
      return {};
    }
    return Error(std::errc::no_such_device);
#elif XE_PLATFORM_MAC
    DarwinRootProof proof;
    auto error = DarwinProofForFd(root.state_->directory, proof);
    if (error) {
      return error;
    }
    auto captured = std::make_shared<const DarwinRootProof>(proof);
    std::shared_ptr<const DarwinRootProof> prior;
    if (!root.state_->darwin_authority.compare_exchange_strong(prior, captured) &&
        !(*prior == proof)) {
      return Error(std::errc::no_such_device);
    }
    output.filesystem = uint64_t(proof.identity.device);
    output.inode = proof.identity.file;
    output.canonical = root.state_->path;
    output.filesystem_path = std::move(proof.physical_path);
    output.physical_ancestors = std::move(proof.ancestors);
    output.subtree = subtree;
    return {};
#else
    return Error(std::errc::operation_not_supported);
#endif
  }
  static bool Guest(const HostStorageRoot& root) {
    return bool(root.state_->guest_mount) ||
           (root.state_->parent && Guest(*root.state_->parent));
  }
#if !XE_PLATFORM_WIN32
  static std::error_code PrivateAlias(int directory, const MountIdentity& mount,
      const FileHandleInfo& guest, size_t& remaining, unsigned depth) {
    if (depth > 2) { return Error(std::errc::value_too_large); }
    Fd scan(openat(directory, ".", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    if (scan.value < 0) { return NativeError(); }
    DIR* stream = fdopendir(scan.value);
    if (!stream) { return NativeError(); }
    scan.Release();
    std::error_code error;
    while (true) {
      errno = 0;
      const auto* row = readdir(stream);
      if (!row) { if (errno) { error = NativeError(); } break; }
      if (!strcmp(row->d_name, ".") || !strcmp(row->d_name, "..")) { continue; }
      if (!remaining--) { error = Error(std::errc::value_too_large); break; }
      auto pinned = PinAt(directory, row->d_name);
      if (pinned.value < 0) { error = NativeError(); break; }
      struct stat node;
      if (fstat(pinned.value, &node)) { error = NativeError(); break; }
      if ((error = ConfinedMount(pinned.value, mount))) { break; }
      if (uint64_t(node.st_dev) == guest.filesystem_id &&
          uint64_t(node.st_ino) == guest.file_id) {
        error = Error(std::errc::permission_denied); break;
      }
      if (S_ISDIR(node.st_mode)) {
        Fd child(openat(directory, row->d_name,
                        O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
        if (child.value < 0) { error = NativeError(); break; }
        struct stat actual;
        if (fstat(child.value, &actual) || !SameNode(actual, node)) {
          error = Error(std::errc::state_not_recoverable); break;
        }
        error = PrivateAlias(child.value, mount, guest, remaining, depth + 1);
        if (error) { break; }
      } else if (!S_ISREG(node.st_mode)) {
        error = Error(std::errc::permission_denied); break;
      }
    }
    closedir(stream);
    return error;
  }
#endif
  static std::error_code GuestFile(const HostStorageRoot& root,
                                   const FileHandleInfo& info,
                                   bool declared_guest = false) {
    // With a proved admitted tree and nofollow/no-cross-mount traversal, an
    // outside alias to a private regular inode necessarily adds a hardlink.
    // Ordinary unrelated single-link I/O does not depend on private contents.
    if ((!Guest(root) && !declared_guest) || !info.regular || info.links <= 1) {
      return {};
    }
    std::vector<std::shared_ptr<const HostStorageGuestProjection>> projections;
    {
      auto& all = All();
      std::lock_guard lock(all.mutex);
      for (const auto& w : all.projections) {
        if (auto p = w.lock()) { projections.push_back(std::move(p)); }
      }
    }
    size_t remaining = 65536;
    for (const auto& p : projections) {
      for (const auto& e : p->state_->exclusions) {
        if (!e.private_tree || e.location.filesystem != info.filesystem_id) { continue; }
#if !XE_PLATFORM_WIN32
        auto error = e.root->Validate();
        if (!error) {
          error = PrivateAlias(e.root->state_->directory, e.root->state_->mount,
                               info, remaining, 0);
        }
        if (error) { return error; }
#endif
      }
    }
    for (const auto& private_root : Private()) {
      const auto error = private_root->Validate();
      if (error) {
        return error;
      }
#if !XE_PLATFORM_WIN32
      if (uint64_t(private_root->state_->device) != info.filesystem_id) {
        continue;
      }
#endif
      if (private_root->state_->space.get() != root.state_->space.get()) {
        return Error(std::errc::operation_not_supported);
      }
      const auto flat = PrivateContents(*private_root, &info);
      if (flat) {
        return flat;
      }
    }
    return {};
  }
  static bool BootstrapBindingMatches(const BootstrapProtectionGroup& group,
      const HostStorageRoot& parent, const HostStorageRoot& root,
      const std::filesystem::path& target, const std::filesystem::path& staging) {
#if XE_PLATFORM_LINUX || XE_PLATFORM_MAC
    return group.filesystem == uint64_t(parent.state_->device) &&
        group.parent_directory == uint64_t(parent.state_->inode) &&
        group.mount == parent.state_->mount.id &&
        group.filesystem == uint64_t(root.state_->device) &&
        group.directory == uint64_t(root.state_->inode) &&
        group.mount == root.state_->mount.id && group.target == target &&
        group.staging == staging && group.parent_identity &&
        parent.state_->directory_identity && group.parent_identity->SameEntry(
            {}, *parent.state_->directory_identity, {});
#else
    return false;
#endif
  }
  static std::shared_ptr<BootstrapProtectionGroup> ClaimBootstrap(
      const HostStorageRoot& parent, const HostStorageRoot& root,
      const std::filesystem::path& target, const std::filesystem::path& staging,
      const std::shared_ptr<const uint8_t>& owner, std::error_code& error) {
    error.clear();
#if XE_PLATFORM_LINUX || XE_PLATFORM_MAC
    auto proposed = std::make_shared<BootstrapProtectionGroup>();
    proposed->parent_identity = parent.state_->directory_identity;
    proposed->filesystem = parent.state_->device;
    proposed->parent_directory = parent.state_->inode;
    proposed->mount = parent.state_->mount.id;
    proposed->directory = root.state_->inode;
    proposed->target = target; proposed->staging = staging;
    auto& all = All(); std::lock_guard lock(all.mutex);
    std::shared_ptr<BootstrapProtectionGroup> found;
    for (const auto& weak : all.private_roots) {
      const auto registered = weak.lock();
      if (!registered || !registered->state_->bootstrap_private) { continue; }
      const auto& group = registered->state_->bootstrap_group;
      if (!group || group->filesystem != uint64_t(root.state_->device) ||
          group->directory != uint64_t(root.state_->inode)) { continue; }
      if (!BootstrapBindingMatches(*group, parent, root, target, staging) ||
          (found && found != group)) {
        error = Error(std::errc::permission_denied); return {};
      }
      found = group;
    }
    if (!found) { found = std::move(proposed); }
    const auto prior_owner = found->active_owner.lock();
    if (prior_owner && prior_owner != owner) {
      error = Error(std::errc::device_or_resource_busy); return {};
    }
    found->active_owner = owner; return found;
#else
    error = Error(std::errc::operation_not_supported); return {};
#endif
  }
  static std::optional<std::filesystem::path> RetainedBootstrapStage(
      const HostStorageRoot& parent, const HostStorageRoot& canonical,
      const std::filesystem::path& target, std::error_code& error) {
    error.clear();
#if XE_PLATFORM_LINUX || XE_PLATFORM_MAC
    auto& all = All(); std::lock_guard lock(all.mutex);
    std::shared_ptr<BootstrapProtectionGroup> found;
    for (const auto& weak : all.private_roots) {
      const auto registered = weak.lock();
      if (!registered || !registered->state_->bootstrap_private) { continue; }
      const auto& group = registered->state_->bootstrap_group;
      if (!group || group->filesystem != uint64_t(canonical.state_->device) ||
          group->directory != uint64_t(canonical.state_->inode)) { continue; }
      if (!BootstrapBindingMatches(*group, parent, canonical, target, group->staging) ||
          (found && found != group)) {
        error = Error(std::errc::permission_denied); return {};
      }
      found = group;
    }
    return found ? std::optional(found->staging) : std::nullopt;
#else
    error = Error(std::errc::operation_not_supported); return {};
#endif
  }
  static std::shared_ptr<HostStorageRoot> Protect(const HostStorageRoot& root,
                                                  std::error_code& error,
                                                  unsigned purpose = 0,
                                                  std::shared_ptr<BootstrapProtectionGroup> media_transfer = {}) {
    const bool recovery = purpose == 1, intents = purpose == 2, content = purpose == 3;
    const bool bootstrap = purpose == 4;
    const bool media = bool(media_transfer);
    if (media && purpose != 0) { error = Error(std::errc::permission_denied); return {}; }
    const auto bootstrap_owner = root.state_->bootstrap_owner.lock();
    if (bootstrap && !bootstrap_owner) { error = Error(std::errc::permission_denied); return {}; }
    auto view = root.Subroot({}, false, error);
    if (!view) {
      return {};
    }
#if XE_PLATFORM_LINUX || XE_PLATFORM_MAC
    if (!root.state_->space->NativeAuthorityFor(root.state_->device)) {
      error = Error(std::errc::operation_not_supported);
      return {};
    }
#endif
    auto guard = root.state_->space->TryBegin(root.path(), error,root.state_->directory_identity);
    if (!guard) {
      return {};
    }
    error = root.Validate();
    if (error) {
      return {};
    }
    error = bootstrap ? BootstrapContents(root) :
        Contents(root, nullptr, true, (recovery || intents || content) ? 8192 : 64,
                 recovery, intents, content);
    if (error) {
      return {};
    }
    ProtectionLocation location;
    error = Location(root, true, location);
    if (error) {
      return {};
    }
    auto pinned_location = std::make_shared<ProtectionLocation>(location);
    auto& all = All();
    std::unique_lock lock(all.mutex);
    const auto bootstrap_group = root.state_->bootstrap_group;
    const bool owns_bootstrap = bootstrap_group && bootstrap_owner &&
        bootstrap_group->active_owner.lock() == bootstrap_owner;
    if (bootstrap && !owns_bootstrap) { error = Error(std::errc::permission_denied); return {}; }
    if (media && (!owns_bootstrap || bootstrap_group != media_transfer ||
        std::none_of(all.private_roots.begin(), all.private_roots.end(),
            [&](const auto& weak) {
              const auto registered = weak.lock();
              if (!registered || !registered->state_->bootstrap_private ||
                  registered->state_->bootstrap_group != media_transfer) { return false; }
              const auto& prior = registered->state_->private_location;
              return prior && prior->filesystem == location.filesystem &&
                  prior->inode == location.inode &&
                  prior->filesystem_path == location.filesystem_path;
            }))) {
      error = Error(std::errc::permission_denied); return {};
    }
    std::shared_ptr<BootstrapProtectionGroup> refined_bootstrap;
    const auto retire_refined = [&](const HostStorageRoot* keep = nullptr) {
      if (!refined_bootstrap) { return; }
      std::erase_if(all.private_roots, [&](const auto& weak) {
        const auto prior = weak.lock();
        return prior && prior.get() != keep && prior->state_->bootstrap_private &&
            prior->state_->bootstrap_group == refined_bootstrap;
      });
    };
    std::erase_if(all.guests, [](const auto& w) { return w.expired(); });
    std::erase_if(all.private_roots, [](const auto& w) { return w.expired(); });
    for (const auto& weak : all.private_roots) {
      if (auto existing = weak.lock()) {
        const auto& prior = existing->state_->private_location;
        if (prior && prior->filesystem == location.filesystem &&
            prior->inode == location.inode &&
            prior->filesystem_path == location.filesystem_path) {
          const bool refining_bootstrap = existing->state_->bootstrap_private &&
              (bootstrap || recovery || intents || content || media) && owns_bootstrap &&
              existing->state_->bootstrap_group == bootstrap_group;
          if (!refining_bootstrap && (existing->state_->bootstrap_private != bootstrap ||
              existing->is_recovery_private() != recovery ||
              existing->is_profile_intent_private() != intents ||
              existing->is_content_mutation_private() != content ||
              (!root.read_only() && existing->read_only()))) {
            error = Error(std::errc::permission_denied);
            return {};
          }
          if (!refining_bootstrap) {
            lock.unlock();
            auto reused=existing->Subroot({}, root.read_only(), error);
            if (!reused || !refined_bootstrap) { return reused; }
            lock.lock();
            if (refined_bootstrap->active_owner.lock()!=bootstrap_owner ||
                std::none_of(all.private_roots.begin(),all.private_roots.end(),
                    [&](const auto& weak) { return weak.lock()==existing; })) {
              error=Error(std::errc::permission_denied);return {};
            }
            // Reuse was genuinely observed outside the registry lock before
            // its still-registered typed policy replaces the old receipt.
            retire_refined();return reused;
          }
          // Publish a current native protector first; only then retire the
          // exact receipt's old stage/canonical roots in this same lock. The
          // registry never keeps a vanished stage pathname after rename.
          refined_bootstrap = bootstrap_group;
        }
      }
    }
    if (all.private_roots.size() >= 1024) {
      error = Error(std::errc::too_many_files_open);
      return {};
    }
    for (const auto& w : all.guests) {
      if (auto guest = w.lock()) {
        if (guest->state_->unknown) {
          error = Error(std::errc::permission_denied);
          return {};
        }
        for (size_t i = 0; i != guest->state_->locations.size(); ++i) {
          const auto& other = guest->state_->locations[i];
          if (Overlaps(location, other) && !Permits(*guest, i, location)) {
            error = Error(std::errc::permission_denied);
            return {};
          }
        }
      }
    }
    all.private_roots.reserve(all.private_roots.size() + 1);
    for (const auto& w : all.guests) {
      if (auto guest = w.lock()) {
        guest->state_->private_dependencies.reserve(
            guest->state_->private_dependencies.size() + 1);
      }
    }
    view->state_->private_location = std::move(pinned_location);
    view->state_->flat_private = !recovery && !intents && !content && !bootstrap;
    view->state_->bootstrap_private = bootstrap;
    view->state_->bootstrap_owner = root.state_->bootstrap_owner;
    view->state_->bootstrap_group = root.state_->bootstrap_group;
    view->state_->recovery_private = recovery;
    view->state_->intent_private = intents;
    view->state_->content_mutation_private = content;
    for (const auto& w : all.guests) {
      if (auto guest = w.lock()) {
        guest->state_->private_dependencies.push_back(view);
      }
    }
    all.private_roots.push_back(view);
    // Stage and canonical locations differ, so the old receipt may not have
    // matched the same-location loop above. Its immutable native proof still
    // identifies exactly this family, never an arbitrary same-inode policy.
    if (bootstrap) { refined_bootstrap = bootstrap_group; }
    retire_refined(bootstrap ? view.get() : nullptr);
    return view;
  }
  static std::shared_ptr<const HostStorageGuestMount> Admit(
      std::optional<std::vector<GuestHostBacking>> backings,
      std::error_code& error,
      std::shared_ptr<BootstrapProtectionGroup> bootstrap_transfer = {},
      const HostStorageRoot* bootstrap_source = nullptr,
      HostStorageFailureObservation* diagnostic = nullptr,
      HostStoragePreparationWait* preparation = nullptr) {
    error.clear();
    HostStorageFailureScope observation{diagnostic,error,4};
    const auto transferred = [&](const auto& root) {
      return bootstrap_transfer && root->state_->bootstrap_private &&
          root->state_->bootstrap_group == bootstrap_transfer;
    };
    auto state = std::make_unique<HostStorageGuestMount::State>();
    state->unknown = !backings;
    observation.At(1);
    if (backings && backings->size() > 1024) {
      error = Error(std::errc::value_too_large);
      observation.Capture(__LINE__); return {};
    }
    auto protected_roots = Private();
    std::erase_if(protected_roots, transferred);
    struct PrivateScan {
      std::shared_ptr<HostStorageRoot> root;
      FileHandleInfo info;
      std::vector<std::shared_ptr<const HostStorageRoot>> roots;
    };
    std::vector<PrivateScan> private_scans;
    for (const auto& backing :
         backings.value_or(std::vector<GuestHostBacking>{})) {
      if (preparation) {
        if (backing.source && !backing.source->LinkPreparationCancellation(*preparation)) {
          error=Error(std::errc::value_too_large); observation.Capture(__LINE__); return {};
        }
        if (backing.projection &&
            !backing.projection->state_->source->LinkPreparationCancellation(*preparation)) {
          error=Error(std::errc::value_too_large); observation.Capture(__LINE__); return {};
        }
        if ((error=preparation->Check())) { observation.Capture(__LINE__); return {}; }
      }
      observation.At(2);
      if (backing.requires_projection && !backing.projection) {
        error = Error(std::errc::no_such_device); observation.Capture(__LINE__); return {};
      }
      observation.At(3);
      if (backing.path.empty()) {
        error = Error(std::errc::invalid_argument);
        observation.Capture(__LINE__); return {};
      }
      observation.At(4);
      const auto canonical =
          backing.subtree
              ? std::filesystem::weakly_canonical(backing.path, error)
              : std::filesystem::canonical(backing.path, error);
      if (error) {
        observation.Capture(__LINE__); return {};
      }
      auto existing = backing.subtree ? canonical : canonical.parent_path();
      while (backing.subtree && !std::filesystem::exists(existing, error)) {
        if (error || existing == existing.root_path()) {
          observation.Capture(__LINE__); return {};
        }
        existing = existing.parent_path();
      }
      observation.At(5);
      auto root = HostStorageRoot::Open(existing, true, error, 0);
      if (!root) {
        observation.Capture(__LINE__); return {};
      }
      ProtectionLocation location;
      observation.At(6);
      error = Location(*root, backing.subtree, location);
      if (error && !protected_roots.empty()) {
        observation.Capture(__LINE__); return {};
      }
      if (error) {
        error.clear();
        state->unknown = true;
        location.canonical = canonical;
        location.subtree = backing.subtree;
        state->locations.push_back(std::move(location));
        state->projections.push_back({});
        state->roots.push_back(std::move(root));
        continue;  // Preserve legacy unsupported-platform mounts, without
                   // authority proof.
      }
      if (backing.subtree && existing != canonical) {
        const auto suffix = canonical.lexically_relative(existing);
        location.filesystem_path =
            (location.filesystem_path / suffix).lexically_normal();
        location.canonical = canonical;
        location.inode = 0;
      }
      if (!backing.subtree) {
        observation.At(7);
        auto file = root->OpenFileInternal(canonical.filename(),
                                           FileAccess::kFileReadData, error,
                                           false, false, true, false, nullptr, diagnostic, preparation);
        if (!file) {
          observation.Capture(__LINE__); return {};
        }
        observation.At(8);
        const auto info = file->QueryInfo(error);
        if (!info || !info->regular) {
          if (!error) {
            error = Error(std::errc::invalid_argument);
          }
          observation.Capture(__LINE__); return {};
        }
        if(diagnostic && !diagnostic->line && info){diagnostic->filesystem=info->filesystem_id;
          diagnostic->file=info->file_id;diagnostic->available|=2;}
        observation.At(9);
        if (backing.opened_file &&
            (backing.opened_file->filesystem_id != info->filesystem_id ||
             backing.opened_file->file_id != info->file_id)) {
          error = Error(std::errc::state_not_recoverable);
          observation.Capture(__LINE__); return {};
        }
        location.canonical = canonical;
        location.filesystem_path /= canonical.filename();
        location.inode = info->file_id;
        // Refresh under the SAME acquired filesystem guard: a preparation
        // may have waited while a new private protector was registered.
        observation.At(10);
        auto guard = preparation
            ? root->state_->space->BeginPreparation(root->path(),error,*preparation,
                  root->state_->directory_identity,diagnostic)
            : root->state_->space->TryBegin(root->path(),error,
                  root->state_->directory_identity,diagnostic);
        if (!guard) {
          observation.Capture(__LINE__); return {};
        }
        if (preparation) {
          if ((error=root->Validate())) { observation.Capture(__LINE__); return {}; }
          protected_roots=Private();
          std::erase_if(protected_roots,transferred);
        }
        for (const auto& private_root : protected_roots) {
          if (private_root->state_->space.get() != root->state_->space.get()) {
#if !XE_PLATFORM_WIN32
            if (private_root->state_->device == root->state_->device) {
              error = Error(std::errc::operation_not_supported);
              observation.Capture(__LINE__); return {};
            }
#endif
            continue;
          }
          observation.At(11);
          error = PrivateContents(*private_root, &*info);
          if (error) {
            observation.Capture(__LINE__); return {};
          }
        }
        if (preparation) private_scans.push_back({root,*info,protected_roots});
        state->files.push_back(std::move(file));
      }
      observation.At(12);
      if (backing.projection &&
          (!backing.subtree ||
           !BackingProjected(*backing.projection, location))) {
        error = Error(std::errc::permission_denied);
        observation.Capture(__LINE__); return {};
      }
      state->locations.push_back(std::move(location));
      state->projections.push_back(backing.projection);
      state->roots.push_back(std::move(root));
    }
    if (bootstrap_transfer) {
#if XE_PLATFORM_LINUX || XE_PLATFORM_MAC
      if (!bootstrap_source || state->unknown || state->roots.size() != 1 ||
          state->locations.size() != 1 || !state->locations.front().subtree ||
          state->locations.front().inode != bootstrap_source->state_->inode ||
          state->locations.front().filesystem != uint64_t(bootstrap_source->state_->device) ||
          state->roots.front()->state_->mount.id != bootstrap_source->state_->mount.id) {
        error = Error(std::errc::state_not_recoverable); observation.Capture(__LINE__); return {};
      }
#else
      error = Error(std::errc::operation_not_supported); observation.Capture(__LINE__); return {};
#endif
    }
    auto mount = std::shared_ptr<HostStorageGuestMount>(
        new HostStorageGuestMount(std::move(state)));
    // Retain every newly observed protector through registry unlock, including
    // a concurrent last external-owner release. Bound: initial + eight rescans.
    std::vector<std::shared_ptr<const HostStorageRoot>> observed_protectors;
    if (preparation) observed_protectors.reserve(9*1024);
    auto& all = All();
    std::unique_lock lock(all.mutex);
    // A legitimate concurrent private-root registration after the inode scan
    // causes only that scan to run again. Retain the SAME opened files/device;
    // do not reopen, read headers, redo initialization or restart admission.
    unsigned rescans=0;
    if (preparation) for (;;) {
      observation.At(13);
      if (preparation && (error=preparation->Check())) { observation.Capture(__LINE__); return {}; }
      bool changed=false;
      for (const auto& weak:all.private_roots) {
        if (observed_protectors.size()==observed_protectors.capacity()) {
          error=Error(std::errc::too_many_files_open); observation.Capture(__LINE__); return {};
        }
        const auto& private_root=observed_protectors.emplace_back(weak.lock());
        if (!private_root || transferred(private_root)) continue;
        for (const auto& scan:private_scans) {
          bool same_filesystem=scan.root->state_->space==private_root->state_->space;
#if !XE_PLATFORM_WIN32
          same_filesystem |= scan.info.filesystem_id==uint64_t(private_root->state_->device);
#endif
          if (same_filesystem && std::find(scan.roots.begin(),scan.roots.end(),private_root)==scan.roots.end()) changed=true;
        }
      }
      if (!changed) break;
      if (!preparation || ++rescans>8) {
        error=Error(std::errc::resource_unavailable_try_again);
        observation.Capture(__LINE__); return {};
      }
      lock.unlock();
      for (auto& scan:private_scans) {
        auto guard=scan.root->state_->space->BeginPreparation(scan.root->path(),error,*preparation,
            scan.root->state_->directory_identity,diagnostic);
        if (!guard) { observation.Capture(__LINE__); return {}; }
        if ((error=scan.root->Validate())) { observation.Capture(__LINE__); return {}; }
        auto current=Private();
        std::erase_if(current,transferred);
        for (const auto& private_root:current) {
          if (private_root->state_->space!=scan.root->state_->space) {
#if !XE_PLATFORM_WIN32
            if (private_root->state_->device==scan.root->state_->device) {
              error=Error(std::errc::operation_not_supported); observation.Capture(__LINE__); return {};
            }
#endif
            continue;
          }
          if ((error=PrivateContents(*private_root,&scan.info))) { observation.Capture(__LINE__); return {}; }
        }
        scan.roots=std::move(current);
      }
      lock.lock();
    }
    observation.At(12);
    if (bootstrap_transfer && (!bootstrap_source ||
        bootstrap_source->state_->bootstrap_group != bootstrap_transfer ||
        !bootstrap_source->state_->bootstrap_owner.lock() ||
        bootstrap_transfer->active_owner.lock() != bootstrap_source->state_->bootstrap_owner.lock())) {
      error = Error(std::errc::permission_denied); observation.Capture(__LINE__); return {};
    }
    // A prevalidated old observation may have waited for the exact format
    // admission transfer. Never register it after that generation retires.
    for (const auto& p:mount->state_->projections) {
      if (p && (p->state_->source->retiring() ||
          p->state_->guest_admission.load(std::memory_order_acquire)==
              HostStorageGuestProjection::State::GuestAdmission::kRetiredFormatObservation)) {
        error=Error(std::errc::no_such_device);observation.Capture(__LINE__); return {};
      }
    }
    std::erase_if(all.guests, [](const auto& w) { return w.expired(); });
    std::erase_if(all.private_roots, [](const auto& w) { return w.expired(); });
    if (all.guests.size() >= 512) {
      error = Error(std::errc::too_many_files_open);
      observation.Capture(__LINE__); return {};
    }
    std::erase_if(all.projections, [](const auto& w) { return w.expired(); });
    for (const auto& w : all.projections) {
      const auto p = w.lock();
      if (!p || p->state_->guest_admission.load(std::memory_order_acquire) !=
          HostStorageGuestProjection::State::GuestAdmission::kSourceUnadmitted) { continue; }
      // Every actual guest admission observes this shared generation seal,
      // even a raw backing or an independently reopened/older root that did
      // not carry the initializer's projection. Compare native locations;
      // names and a caller-provided projection cannot establish readiness.
      if (mount->state_->unknown) {
        error=Error(std::errc::operation_in_progress);observation.Capture(__LINE__); return {};
      }
      for (const auto& guest : mount->state_->locations) {
        if (Overlaps(p->state_->source_location, guest) ||
            std::any_of(p->state_->exclusions.begin(),p->state_->exclusions.end(),
                [&](const auto& e) { return !e.private_tree && Overlaps(e.location,guest); })) {
          error = Error(std::errc::operation_in_progress); observation.Capture(__LINE__); return {};
        }
      }
    }
    for (const auto& w : all.private_roots) {
      if (auto private_root = w.lock()) {
        if (transferred(private_root)) { continue; }
        if (mount->state_->unknown) {
          error = Error(std::errc::operation_not_supported);
          observation.Capture(__LINE__); return {};
        }
        if (!private_root->state_->private_location) {
          error = Error(std::errc::operation_not_supported);
          observation.Capture(__LINE__); return {};
        }
        mount->state_->private_dependencies.push_back(private_root);
        const auto& location = *private_root->state_->private_location;
        for (size_t i = 0; i != mount->state_->locations.size(); ++i) {
          const auto& guest = mount->state_->locations[i];
          if (Overlaps(location, guest) && !Permits(*mount, i, location)) {
            error = Error(std::errc::permission_denied);
            observation.Capture(__LINE__); return {};
          }
        }
      }
    }
    std::erase_if(all.projections, [](const auto& w) { return w.expired(); });
    for (const auto& w : all.projections) {
      const auto p = w.lock();
      if (!p) { continue; }
      for (const auto& e : p->state_->exclusions) {
        if (!e.private_tree) { continue; }
        if (mount->state_->unknown) {
          error = Error(std::errc::permission_denied); observation.Capture(__LINE__); return {};
        }
        for (size_t i = 0; i != mount->state_->locations.size(); ++i) {
          if (Overlaps(e.location, mount->state_->locations[i]) &&
              !Permits(*mount, i, e.location)) {
            error = Error(std::errc::permission_denied); observation.Capture(__LINE__); return {};
          }
        }
      }
    }
    // All checks/allocation precede this exact private->guest metadata transfer.
    all.guests.reserve(all.guests.size() + 1);
    if (bootstrap_transfer) {
      std::erase_if(all.private_roots, [&](const auto& weak) {
        const auto root = weak.lock(); return root && transferred(root);
      });
    }
    for (const auto& p : mount->state_->projections) {
      if (p) { p->state_->sealed.store(true, std::memory_order_release); }
    }
    all.guests.push_back(mount);
    return mount;
  }
};
#if !XE_PLATFORM_WIN32
namespace {
std::error_code GuestRemovalAlias(const HostStorageRoot& root, const struct stat& info) {
  return HostStorageProtection::GuestFile(root, SnapshotInfo(info), true);
}
}  // namespace
#endif
std::shared_ptr<HostStorageRoot> HostStorageRoot::ProtectFlat(
    std::error_code& error) const {
  return HostStorageProtection::Protect(*this, error);
}
std::shared_ptr<HostStorageRoot> HostStorageRoot::ProtectRecovery(
    std::error_code& error) const {
  return HostStorageProtection::Protect(*this, error, true);
}
std::shared_ptr<HostStorageRoot> HostStorageRoot::ProtectProfileIntents(
    std::error_code& error) const {
  return HostStorageProtection::Protect(*this, error, 2);
}
bool HostStorageRoot::is_profile_intent_private() const {
  return state_->intent_private ||
         (state_->parent && state_->parent->is_profile_intent_private());
}
bool HostStorageRoot::is_recovery_private() const {
  return state_->recovery_private ||
         (state_->parent && state_->parent->is_recovery_private());
}
bool HostStorageRoot::is_guest_private() const {
  return state_->bootstrap_private || state_->flat_private || state_->content_mutation_private || state_->recovery_private ||
         state_->intent_private ||
         (state_->parent && state_->parent->is_guest_private());
}
bool HostStorageRoot::HasProtectedGuestStorage() {
  return !HostStorageProtection::Private().empty();
}
std::shared_ptr<const HostStorageGuestMount> HostStorageRoot::AdmitGuestMount(
    std::optional<std::vector<GuestHostBacking>> backings,
    std::error_code& error, HostStorageFailureObservation* diagnostic,
    HostStoragePreparationWait* preparation) {
  return HostStorageProtection::Admit(std::move(backings), error, {}, nullptr, diagnostic, preparation);
}
std::shared_ptr<HostStorageRoot> HostStorageRoot::GuestView(
    std::error_code& error) const {
  if (has_guest_projection() && !Projection()) {
    error = Error(std::errc::no_such_device); return {};
  }
  if (has_guest_projection()) {
    // An excluded native root is not an empty admitted guest device. Refuse
    // before sealing the projection or publishing any guest ownership.
    error=CheckGuestAccess({});if (error) { return {}; }
  }
  const auto projection = Projection();
  if (projection && projection->state_->guest_admission.load(std::memory_order_acquire) ==
      HostStorageGuestProjection::State::GuestAdmission::kSourceUnadmitted) {
    error=Error(std::errc::operation_in_progress);return {};
  }
  auto view = Subroot({}, false, error);
  if (!view) {
    return {};
  }
  view->state_->guest_mount =
      AdmitGuestMount(std::vector<GuestHostBacking>{GuestBacking()}, error);
  return view->state_->guest_mount ? view : nullptr;
}

std::shared_ptr<const HostStorageGuestProjection> HostStorageRoot::Projection() const {
  if (state_->projection_required) { return state_->projection.lock(); }
  return state_->parent ? state_->parent->Projection() : nullptr;
}
bool HostStorageRoot::PublicProjectionScope() const {
  return state_->public_projection_scope ||
      (state_->parent && state_->parent->PublicProjectionScope());
}
bool HostStorageRoot::has_guest_projection() const {
  return state_->projection_required ||
      (state_->parent && state_->parent->has_guest_projection());
}
GuestHostBacking HostStorageRoot::GuestBacking() const {
  return {path(), true, std::nullopt, Projection(), has_guest_projection(), shared_from_this()};
}
void HostStorageRoot::SetFormatEligibility(bool allow) {
  state_->format_eligible = allow;
}
bool HostStorageRoot::format_eligible() const {
  return state_->format_eligible && !state_->read_only;
}
std::shared_ptr<HostStorageRoot> HostStorageRoot::CaptureGuestDirectory(
    const std::filesystem::path& directory, bool readonly, bool& matched,
    std::error_code& error) const {
  matched = false;
  if (!NativeSpelling(directory)) {
    // Malformed input is a denial, never an unmatched classification that
    // permits the caller to retry the same truncated path without authority.
    matched = true; error = Error(std::errc::invalid_argument); return {};
  }
  error = Validate();
  if (error) { return {}; }
  // Classification only: no bytes, guest mount, borrowed path authority or
  // write permission is obtained from this temporary native directory probe.
  auto probe = Open(directory, true, error, 0);
  if (!probe) { return {}; }
#if !XE_PLATFORM_WIN32
  if (probe->state_->device != state_->device) { return {}; }
#endif
  ProtectionLocation source, target;
  error = HostStorageProtection::Location(*this, true, source);
  if (!error) { error = HostStorageProtection::Location(*probe, true, target); }
  if (error) { return {}; }
  if (!ContainsLocation(source, target)) { return {}; }
  matched = true;
  auto relative = source.filesystem_path == target.filesystem_path
      ? std::filesystem::path{} : target.filesystem_path.lexically_relative(source.filesystem_path);
  if (!Relative(relative, true)) { error = Error(std::errc::permission_denied); return {}; }
  error = CheckGuestAccess(relative);
  if (error) { return {}; }
  auto view = Subroot(relative, readonly, error);
  if (!view) { return {}; }
#if !XE_PLATFORM_WIN32
  if (view->state_->device != probe->state_->device ||
      view->state_->inode != probe->state_->inode) {
    error = Error(std::errc::state_not_recoverable); return {};
  }
#endif
  return view;
}
std::error_code HostStorageRoot::CheckGuestAccess(
    const std::filesystem::path& relative, bool structural) const {
  if (!ContentNamespaceReady(relative, structural)) {
    return Error(std::errc::device_or_resource_busy);
  }
  if (!Relative(relative, true)) { return Error(std::errc::invalid_argument); }
  const auto p = Projection();
  if (!p && has_guest_projection()) { return Error(std::errc::no_such_device); }
  auto error = Validate();
  if (!error && p) { error = HostStorageProtection::ProjectionCurrent(*p); }
  if (error) { return error; }
  const HostStorageRoot* scope_root = p ? p->state_->source.get() : nullptr;
  const ProtectionLocation* scope_location = p ? &p->state_->source_location : nullptr;
  const bool public_scope = PublicProjectionScope();
  if (p && public_scope) {
    if (p->state_->public_scopes.size() != 1) { return Error(std::errc::no_such_device); }
    scope_root = p->state_->public_scopes.front().first.get();
    scope_location = &p->state_->public_scopes.front().second;
  }
  const auto here = !p || path() == scope_root->path()
      ? std::filesystem::path{} : path().lexically_relative(scope_root->path());
  if (p && !Relative(here, true)) { return Error(std::errc::permission_denied); }
  const auto target = p ? (scope_location->filesystem_path / here / relative).lexically_normal()
                        : std::filesystem::path{};
  if (p) { for (const auto& e : p->state_->exclusions) {
    if ((public_scope && !e.private_tree) ||
        HostStorageProtection::SourceNamespaceContainer(*p, e)) { continue; }
    if (e.location.filesystem != p->state_->source_location.filesystem) { continue; }
    if (target == e.location.filesystem_path || Below(e.location.filesystem_path, target) ||
        (structural && Below(target, e.location.filesystem_path))) {
      return Error(std::errc::permission_denied);
    }
  } }
#if !XE_PLATFORM_WIN32
  std::string leaf;
  auto parent = Parent(state_->directory, relative, leaf, error);
  if (parent.value < 0) {
    // Missing descendants may be destinations; nofollow rejects actual alias
    // parents, and the eventual create/open repeats these same native checks.
    return error == std::errc::no_such_file_or_directory ? std::error_code{} : error;
  }
  if (p) {
    error = HostStorageProtection::ProjectedDirectory(*p, parent.value,
                                                      state_->device, state_->mount, false, PublicProjectionScope());
    if (error) { return error; }
  }
  struct stat info;
  if (fstatat(parent.value, leaf.c_str(), &info, AT_SYMLINK_NOFOLLOW)) {
    return errno == ENOENT ? std::error_code{} : NativeError();
  }
  if (S_ISLNK(info.st_mode)) { return Error(std::errc::permission_denied); }
  if (p && S_ISDIR(info.st_mode)) {
    Fd actual(openat(parent.value, leaf.c_str(),
                     O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    struct stat pinned;
    if (actual.value < 0 || fstat(actual.value, &pinned)) { return NativeError(); }
    if (!SameNode(pinned, info)) { return Error(std::errc::state_not_recoverable); }
    error = HostStorageProtection::ProjectedDirectory(*p, actual.value,
                                                      state_->device, state_->mount, structural, public_scope);
    if (error) { return error; }
  }
  if (S_ISREG(info.st_mode) && info.st_nlink > 1) {
    return HostStorageProtection::GuestFile(*this, SnapshotInfo(info), true);
  }
#endif
  return {};
}
std::shared_ptr<const HostStorageGuestProjection> HostStorageRoot::InstallProjection(
    std::vector<std::shared_ptr<const HostStorageRoot>> private_roots,
    std::vector<std::shared_ptr<const HostStorageRoot>> namespace_roots,
    bool allow_format, std::error_code& error) {
  return InstallProjectionInternal(std::move(private_roots),std::move(namespace_roots),
      allow_format,{},nullptr,ProjectionAdmissionRole::kOrdinary,error);
}
std::shared_ptr<const HostStorageGuestProjection> HostStorageRoot::InstallBootstrapProjection(
    std::vector<std::shared_ptr<const HostStorageRoot>> private_roots,
    std::vector<std::shared_ptr<const HostStorageRoot>> namespace_roots,
    bool allow_format, std::span<HostStorageBootstrapDirectory* const> settled_private,
    HostStorageBootstrapDirectory* exact_public, std::error_code& error) {
  return InstallProjectionInternal(std::move(private_roots),std::move(namespace_roots),
      allow_format,settled_private,exact_public,
      ProjectionAdmissionRole::kBootstrapSourceUnadmitted,error);
}
std::shared_ptr<const HostStorageGuestProjection> HostStorageRoot::InstallFormatProjection(
    const std::shared_ptr<const HostStorageGuestProjection>& previous,
    std::vector<std::shared_ptr<const HostStorageRoot>> private_roots,
    std::vector<std::shared_ptr<const HostStorageRoot>> namespace_roots,
    bool allow_format, std::error_code& error) {
  // Only the native metadata format producer calls this after real drain/EX.
  // Old admitted generations retire before any epoch/data publication.
  return InstallProjectionInternal(std::move(private_roots),std::move(namespace_roots),
      allow_format,{},nullptr,ProjectionAdmissionRole::kOwnedFormatSourceUnadmitted,error,previous.get());
}
std::shared_ptr<const HostStorageGuestProjection> HostStorageRoot::InstallProjectionInternal(
    std::vector<std::shared_ptr<const HostStorageRoot>> private_roots,
    std::vector<std::shared_ptr<const HostStorageRoot>> namespace_roots,
    bool allow_format, std::span<HostStorageBootstrapDirectory* const> settled_private,
    HostStorageBootstrapDirectory* exact_public, ProjectionAdmissionRole role,
    std::error_code& error, const HostStorageGuestProjection* exact_previous_format) {
  error.clear();
  if ((role!=ProjectionAdmissionRole::kOrdinary ||
       !settled_private.empty() || exact_public) && global_critical_region::is_held_by_current_thread()) {
    error=Error(std::errc::operation_not_permitted);return {};
  }
  if (has_guest_projection() || HostStorageProtection::Guest(*this)) {
    error = Error(std::errc::device_or_resource_busy); return {};
  }
  auto s = std::make_unique<HostStorageGuestProjection::State>();
  if (role!=ProjectionAdmissionRole::kOrdinary) {
    s->guest_admission.store(HostStorageGuestProjection::State::GuestAdmission::kSourceUnadmitted,
                             std::memory_order_relaxed);
  }
  s->source = shared_from_this();
  error = HostStorageProtection::Location(*this, true, s->source_location);
  if (error) { return {}; }
  if (role==ProjectionAdmissionRole::kOwnedFormatSourceUnadmitted &&
      (!exact_previous_format ||
       exact_previous_format->state_->source_location.filesystem!=s->source_location.filesystem ||
       exact_previous_format->state_->source_location.inode!=s->source_location.inode ||
       exact_previous_format->state_->source_location.canonical!=s->source_location.canonical ||
       exact_previous_format->state_->source_location.filesystem_path!=s->source_location.filesystem_path ||
       exact_previous_format->state_->source_location.physical_ancestors!=s->source_location.physical_ancestors)) {
    error=Error(std::errc::permission_denied);return {};
  }
  struct CapsuleProof {
    HostStorageBootstrapDirectory* capsule;
    std::shared_ptr<BootstrapProtectionGroup> receipt;
    ProtectionLocation location;
  };
  std::vector<CapsuleProof> private_proofs;
  std::optional<CapsuleProof> public_proof;
  if (settled_private.size()>64) { error=Error(std::errc::value_too_large);return {}; }
  const auto prove_capsule = [&](HostStorageBootstrapDirectory* capsule,
                                 CapsuleProof& proof) -> std::error_code {
    if (!capsule || !capsule->state_) { return Error(std::errc::invalid_argument); }
    auto& native=*capsule->state_;
    if (!native.canonical || !native.durable || native.public_admitted || native.private_settled ||
        !native.root || !native.parent || !native.lease ||
        !native.parent->IntentLeaseMatches(*native.lease)) {
      return Error(std::errc::permission_denied);
    }
    auto failure=native.root->Validate();if (failure) { return failure; }
    const auto& receipt=native.root->state_->bootstrap_group;
    if (!receipt || !HostStorageProtection::BootstrapBindingMatches(
        *receipt,*native.parent,*native.root,native.target,native.staging) ||
        native.native_directory!=receipt->directory) { return Error(std::errc::permission_denied); }
    proof={capsule,receipt,{}};
    failure=HostStorageProtection::Location(*native.root,true,proof.location);
    if (!failure && proof.location.filesystem!=s->source_location.filesystem) {
      failure=Error(std::errc::cross_device_link);
    }
    return failure;
  };
  for (auto* capsule : settled_private) {
    CapsuleProof proof{};error=prove_capsule(capsule,proof);if (error) { return {}; }
    if (std::any_of(private_proofs.begin(),private_proofs.end(),
        [&](const auto& prior) { return prior.receipt==proof.receipt; })) {
      error=Error(std::errc::invalid_argument);return {};
    }
    bool covered=false;
    for (const auto& root : private_roots) {
      if (!root || (error=root->Validate())) { if (!error) { error=Error(std::errc::invalid_argument); }return {}; }
      ProtectionLocation exclusion;error=HostStorageProtection::Location(*root,true,exclusion);
      if (error) { return {}; }
      covered|=ContainsLocation(exclusion,proof.location);
    }
    if (!covered) { error=Error(std::errc::permission_denied);return {}; }
    private_proofs.push_back(std::move(proof));
  }
  if (exact_public) {
    CapsuleProof proof{};error=prove_capsule(exact_public,proof);if (error) { return {}; }
    if (std::none_of(namespace_roots.begin(),namespace_roots.end(),
            [&](const auto& root) { return root.get()==exact_public->state_->root.get(); }) ||
        std::any_of(private_proofs.begin(),private_proofs.end(),
            [&](const auto& prior) { return prior.receipt==proof.receipt; })) {
      error=Error(std::errc::permission_denied);return {};
    }
    for (const auto& root : private_roots) {
      if (!root || (error=root->Validate())) { if (!error) { error=Error(std::errc::invalid_argument); }return {}; }
      ProtectionLocation exclusion;error=HostStorageProtection::Location(*root,true,exclusion);
      if (error) { return {}; }
      if (Overlaps(exclusion,proof.location)) { error=Error(std::errc::permission_denied);return {}; }
    }
    public_proof=std::move(proof);
  }
  const auto settled_receipt = [&](const auto& root) {
    return root->state_->bootstrap_private &&
        (std::any_of(private_proofs.begin(),private_proofs.end(),
            [&](const auto& proof) { return proof.receipt==root->state_->bootstrap_group; }) ||
         (public_proof && public_proof->receipt==root->state_->bootstrap_group));
  };
  // Include every other already-protected authority physically beneath this
  // source. Only these exact proved receipts change their projection roles.
  for (const auto& root : HostStorageProtection::Private()) {
    if (settled_receipt(root)) { continue; }
    if (ContainsLocation(s->source_location, *root->state_->private_location)) {
      private_roots.push_back(root);
    }
  }
  for (bool private_tree : {true, false}) {
    for (auto& root : private_tree ? private_roots : namespace_roots) {
      ProtectionLocation location;
      if (!root || (error = root->Validate()) ||
          (error = HostStorageProtection::Location(*root, true, location))) {
        if (!error) { error = Error(std::errc::invalid_argument); }
        return {};
      }
      // Exclusions outside Content still protect against other guest aliases.
      s->exclusions.push_back({std::move(root), std::move(location), private_tree});
    }
  }
  if (s->exclusions.size() > 1024) {
    error = Error(std::errc::value_too_large); return {};
  }
  auto projection = std::shared_ptr<const HostStorageGuestProjection>(
      new HostStorageGuestProjection(std::move(s)));
  auto& all = HostStorageProtection::All();
  std::unique_lock lock(all.mutex);
  const auto current_receipt = [&](const CapsuleProof& proof) {
    return proof.capsule->state_->root->state_->bootstrap_group==proof.receipt &&
        proof.receipt->active_owner.lock()==proof.capsule->state_->owner &&
        std::any_of(all.private_roots.begin(),all.private_roots.end(),[&](const auto& weak) {
          const auto root=weak.lock();return root && root->state_->bootstrap_private &&
              root->state_->bootstrap_group==proof.receipt;
        });
  };
  if (std::any_of(private_proofs.begin(),private_proofs.end(),
          [&](const auto& proof) { return !current_receipt(proof); }) ||
      (public_proof && !current_receipt(*public_proof))) {
    error=Error(std::errc::permission_denied);return {};
  }
  for (const auto& w : all.guests) {
    if (auto guest = w.lock()) {
      if (guest->state_->unknown) {
        error = Error(std::errc::permission_denied); return {};
      }
      if (role==ProjectionAdmissionRole::kOwnedFormatSourceUnadmitted &&
          HostStorageProtection::ForeignFormatGuest(*exact_previous_format,*guest)) {
        error=Error(std::errc::device_or_resource_busy);return {};
      }
      if (role==ProjectionAdmissionRole::kBootstrapSourceUnadmitted) {
        // An existing raw guest may not share a projection or retain managed
        // metadata. Do not install a pending generation beneath that live
        // reader/writer and pretend it has become unadmitted retroactively.
        for (size_t i=0;i<guest->state_->locations.size();++i) {
          if (HostStorageProtection::DrainedFormatGuest(*guest,i)) {continue;}
          const auto& admitted=guest->state_->locations[i];
          if (Overlaps(projection->state_->source_location,admitted) ||
              std::any_of(projection->state_->exclusions.begin(),projection->state_->exclusions.end(),
                  [&](const auto& e) { return !e.private_tree && Overlaps(e.location,admitted); })) {
            error=Error(std::errc::device_or_resource_busy);return {};
          }
        }
      }
      for (const auto& e : projection->state_->exclusions) {
        if (!e.private_tree) { continue; }
        for (size_t i = 0; i != guest->state_->locations.size(); ++i) {
          if (Overlaps(e.location, guest->state_->locations[i]) &&
              !HostStorageProtection::Permits(*guest, i, e.location)) {
            error = Error(std::errc::permission_denied); return {};
          }
        }
      }
    }
  }
  all.projections.reserve(all.projections.size()+1);
  all.projections.push_back(projection);
  // The permanent native exclusions are registered before their exact private
  // bootstrap receipts retire. Public protection remains until actual Admit.
  std::erase_if(all.private_roots,[&](const auto& weak) {
    const auto root=weak.lock();
    return root && root->state_->bootstrap_private &&
        std::any_of(private_proofs.begin(),private_proofs.end(),
            [&](const auto& proof) { return proof.receipt==root->state_->bootstrap_group; });
  });
  for (const auto& proof : private_proofs) { proof.capsule->state_->private_settled=true; }
  if (public_proof) { public_proof->capsule->state_->public_projection=projection; }
  state_->projection = projection;
  state_->projection_required = true;
  state_->format_eligible = allow_format;
  // No strong Root->projection edge: admitted mounts/the bundle own this
  // capability, which itself retains the native source and exclusions.
  lock.unlock();
  for (const auto& proof : private_proofs) { proof.capsule->state_->protections.clear(); }
  return projection;
}
std::error_code HostStorageRoot::AdmitBootstrapProjection(
    ManagedBootstrapGuestAdmissionPermit& permit) {
  if (global_critical_region::is_held_by_current_thread()) {
    return Error(std::errc::operation_not_permitted);
  }
  const auto projection=Projection();
  if (!projection || projection->state_->source.get()!=this ||
      projection->state_->guest_admission.load(std::memory_order_acquire)!=
          HostStorageGuestProjection::State::GuestAdmission::kSourceUnadmitted) {
    return Error(std::errc::permission_denied);
  }
  std::error_code error;
  {
    // Refuse inherited same-filesystem guards before the coordinator consumer
    // can observe/recover native owners; release this real try-guard first.
    auto unlocked=state_->space->TryBegin(path(),error,state_->directory_identity);
    if (!unlocked) { return error; }
  }
  error=Validate();
  if (!error) { error=HostStorageProtection::ProjectionCurrent(*projection); }
  if (error) { return error; }
  // The opaque coordinator consumer performs actual source/identity/lease and
  // ready mutation-owner observations. Never call it under space/registry or
  // model locks; a committed file alone cannot release this generation.
  error=permit.Consume(*this,*projection);if (error) { return error; }
  auto& all=HostStorageProtection::All();std::lock_guard lock(all.mutex);
  if (!permit.FinalCurrent() || Projection()!=projection || projection->state_->source.get()!=this ||
      state_->retired.load(std::memory_order_acquire) ||
      projection->state_->sealed.load(std::memory_order_acquire) ||
      projection->state_->guest_admission.load(std::memory_order_acquire)!=
          HostStorageGuestProjection::State::GuestAdmission::kSourceUnadmitted ||
      std::none_of(all.projections.begin(),all.projections.end(),
          [&](const auto& weak) { return weak.lock()==projection; })) {
    return Error(std::errc::permission_denied);
  }
  // This same registry lock serializes every actual guest registration. Old
  // projections and stale permits cannot change this exact generation.
  projection->state_->guest_admission.store(
      HostStorageGuestProjection::State::GuestAdmission::kBootstrapAdmitted,
      std::memory_order_release);
  return {};
}
std::error_code HostStorageRoot::AdmitFormatProjection(
    ManagedFormatGuestAdmissionPermit& permit) {
  if (global_critical_region::is_held_by_current_thread()) {
    return Error(std::errc::operation_not_permitted);
  }
  const auto projection=Projection();
  if (!projection || projection!=permit.projection_ || permit.source_.get()!=this ||
      projection->state_->source.get()!=this) {
    return Error(std::errc::permission_denied);
  }
  const auto admission=projection->state_->guest_admission.load(std::memory_order_acquire);
  const bool admitted=admission==HostStorageGuestProjection::State::GuestAdmission::kBootstrapAdmitted;
  if (!admitted && admission!=HostStorageGuestProjection::State::GuestAdmission::kSourceUnadmitted) {
    return Error(std::errc::permission_denied);
  }
  std::error_code error;
  {
    auto unlocked=state_->space->TryBegin(path(),error,state_->directory_identity);
    if (!unlocked) {return error;}
  }
  error=Validate();
  if (!error) {error=HostStorageProtection::ProjectionCurrent(*projection);}
  if (error) {return error;}
  // Actual Handoff/marker/nonce/binding/EX and ready recovered-owner proof all
  // happen outside the registry and filesystem admission locks.
  error=permit.Consume(*this,*projection);if (error) {return error;}
  const auto& old=permit.observation_projection_;
  const auto& old_source=permit.observation_source_;
  if (bool(old)!=bool(old_source)) {return Error(std::errc::permission_denied);}
  if (old) {
#if XE_PLATFORM_LINUX || XE_PLATFORM_MAC
    struct stat retained;
    if (fstat(old_source->state_->directory,&retained)) {return NativeError();}
    if (old==projection || old_source.get()==this ||
        old->state_->source!=old_source || old_source->path()!=path() ||
        !old_source->state_->directory_identity || !state_->directory_identity ||
        !old_source->state_->directory_identity->SameEntry({},*state_->directory_identity,{}) ||
        retained.st_dev!=state_->device || retained.st_ino!=state_->inode ||
        (error=ConfinedMount(old_source->state_->directory,state_->mount))) {
      return error?error:Error(std::errc::permission_denied);
    }
#else
    return Error(std::errc::operation_not_supported);
#endif
  }
  auto& all=HostStorageProtection::All();std::lock_guard lock(all.mutex);
  const auto registered=[&](const auto& p) {
    return std::any_of(all.projections.begin(),all.projections.end(),
        [&](const auto& weak) {return weak.lock()==p;});
  };
  if (!permit.FinalCurrent() || Projection()!=projection ||
      state_->retired.load(std::memory_order_acquire) ||
      (!admitted && projection->state_->sealed.load(std::memory_order_acquire)) ||
      !registered(projection) ||
      projection->state_->guest_admission.load(std::memory_order_acquire)!=admission ||
      (old && (!registered(old) || old->state_->source!=old_source ||
          (admitted && !old_source->state_->retired.load(std::memory_order_acquire)) ||
          old->state_->guest_admission.load(std::memory_order_acquire)!=
              (admitted?HostStorageGuestProjection::State::GuestAdmission::kRetiredFormatObservation:
                        HostStorageGuestProjection::State::GuestAdmission::kSourceUnadmitted)))) {
    return Error(std::errc::permission_denied);
  }
  // Exact metadata-only transfer under the same lock as common guest Admit.
  // A failed bootstrap's unrelated pending projection is never exempted.
  // A retry after real admission still consumed a new exact current permit;
  // retain its admitted state and never reset/reopen the old generation.
  if (admitted) {return {};}
  if (old) {
    old_source->state_->retired.store(true,std::memory_order_release);
    old->state_->guest_admission.store(
        HostStorageGuestProjection::State::GuestAdmission::kRetiredFormatObservation,
        std::memory_order_release);
  }
  projection->state_->guest_admission.store(
      HostStorageGuestProjection::State::GuestAdmission::kBootstrapAdmitted,
      std::memory_order_release);
  return {};
}
std::error_code HostStorageRoot::AttachPublicProjection(const HostStorageRoot& source,
    HostStorageBootstrapDirectory* exact_public) {
  const auto p = source.Projection();
  if (!p || has_guest_projection() || HostStorageProtection::Guest(*this)) {
    return Error(std::errc::invalid_argument);
  }
  std::shared_ptr<BootstrapProtectionGroup> public_receipt;
  if (exact_public) {
    auto& native=*exact_public->state_;
    if (!native.canonical || !native.durable || native.public_admitted || native.private_settled ||
        !native.root || native.public_projection.lock()!=p || p->state_->source.get()!=&source ||
        state_->dependency.get()!=native.root.get() ||
        path()!=native.root->path() || !native.parent || !native.lease ||
        !native.parent->IntentLeaseMatches(*native.lease)) {
      return Error(std::errc::permission_denied);
    }
    public_receipt=native.root->state_->bootstrap_group;
    if (!public_receipt || !HostStorageProtection::BootstrapBindingMatches(
        *public_receipt,*native.parent,*native.root,native.target,native.staging)) {
      return Error(std::errc::permission_denied);
    }
  }
  ProtectionLocation location;
  auto error = Validate();
  if (!error) { error = HostStorageProtection::Location(*this, true, location); }
  if (error) { return error; }
  if (location.filesystem != p->state_->source_location.filesystem ||
      (!format_eligible() && !public_receipt)) {
    return Error(std::errc::permission_denied);
  }
  auto& all = HostStorageProtection::All();
  std::lock_guard lock(all.mutex);
  if (public_receipt && (public_receipt->active_owner.lock()!=exact_public->state_->owner ||
      public_receipt->directory!=location.inode ||
      std::none_of(all.private_roots.begin(),all.private_roots.end(),[&](const auto& weak) {
        const auto root=weak.lock();return root && root->state_->bootstrap_private &&
            root->state_->bootstrap_group==public_receipt;
      }))) { return Error(std::errc::permission_denied); }
  if (p->state_->sealed.load(std::memory_order_acquire) ||
      !p->state_->public_scopes.empty()) {
    return Error(std::errc::device_or_resource_busy);
  }
  // Trusted initializer only, before either scope's first guest admission.
  // Both Content and its public shell retain ONE exclusion/generation owner.
  p->state_->public_scopes.emplace_back(shared_from_this(), std::move(location));
  state_->projection = p;
  state_->projection_required = true;
  state_->public_projection_scope = true;
  state_->format_eligible = false;
  return {};
}
std::error_code HostStorageRoot::AttachRetainedPublicProjection(
    const HostStorageRoot& source,
    const HostStorageGuestProjection& retained) {
  const auto projection = source.Projection();
  if (!projection || projection.get() != &retained ||
      projection->state_->source.get() != &source ||
      has_guest_projection() || HostStorageProtection::Guest(*this) ||
      is_guest_private() || !format_eligible()) {
    return Error(std::errc::permission_denied);
  }
  // Validate retained native identities before the protection lock; no
  // filesystem/provider work or guest callback occurs while it is held.
  auto error = Validate();
  if (!error) { error = HostStorageProtection::ProjectionCurrent(*projection); }
  ProtectionLocation location;
  if (!error) {
    error = HostStorageProtection::Location(*this, true, location);
  }
  if (error) { return error; }
  if (location.filesystem != projection->state_->source_location.filesystem) {
    return Error(std::errc::permission_denied);
  }
  auto& all = HostStorageProtection::All();
  std::lock_guard lock(all.mutex);
  auto& scopes = projection->state_->public_scopes;
  if (scopes.empty()) {
    if (projection->state_->sealed.load(std::memory_order_acquire)) {
      return Error(std::errc::device_or_resource_busy);
    }
    scopes.emplace_back(shared_from_this(), std::move(location));
  } else {
    if (scopes.size() != 1) {
      return Error(std::errc::state_not_recoverable);
    }
    const auto& previous = scopes.front().second;
    if (location.filesystem != previous.filesystem ||
        location.inode != previous.inode ||
        location.canonical != previous.canonical ||
        location.filesystem_path != previous.filesystem_path ||
        location.physical_ancestors != previous.physical_ancestors ||
        location.subtree != previous.subtree) {
      return Error(std::errc::state_not_recoverable);
    }
    // Reuse the immutable existing scope, including its retained native root.
    // In particular, do not unseal, clear exclusions or reactivate old views.
  }
  state_->projection = projection;
  state_->projection_required = true;
  state_->public_projection_scope = true;
  state_->format_eligible = false;
  return {};
}
std::error_code HostStorageRoot::PrivateExcludedFrom(
    const HostStorageRoot& source) const {
  const auto p = source.Projection();
  if (!p || HostStorageProtection::ProjectionCurrent(*p)) {
    return Error(std::errc::permission_denied);
  }
  ProtectionLocation location;
  auto error = HostStorageProtection::Location(*this, true, location);
  return error ? error : HostStorageProtection::Excluded(*p, location)
                            ? std::error_code{}
                            : Error(std::errc::permission_denied);
}
std::error_code HostStorageRoot::CheckStateContainer(bool public_shell) const {
#if !XE_PLATFORM_WIN32
  if (auto error = PrivateDirectory()) { return error; }
  Fd fd(openat(state_->directory, ".", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
  if (fd.value < 0) { return NativeError(); }
  DIR* stream = fdopendir(fd.value);
  if (!stream) { return NativeError(); }
  fd.Release();
  size_t count = 0;
  std::error_code error;
  errno = 0;
  while (auto* row = readdir(stream)) {
    std::string_view name(row->d_name);
    if (name == "." || name == "..") { continue; }
    if (++count > 128) { error = Error(std::errc::value_too_large); break; }
    bool allowed = public_shell ? name == "Content"
        : (name == "owner.v1" || name == ".xenia-recovery-v1");
    const auto uuid_suffix = [](std::string_view value) {
      return value.size() == 32 && std::all_of(value.begin(), value.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
      });
    };
    constexpr std::string_view content_prefix = ".xenia-content-mutations-v1-";
    constexpr std::string_view link_prefix = "content-mutations-v1-";
    const bool content_link = !public_shell && name.size() == link_prefix.size() + 32 + 6 &&
        name.starts_with(link_prefix) && name.ends_with(".owner") &&
        uuid_suffix(name.substr(link_prefix.size(), name.size() - link_prefix.size() - 6));
    if (!public_shell && name.starts_with(content_prefix)) {
      allowed = uuid_suffix(name.substr(content_prefix.size()));
    }
    allowed = allowed || content_link;
    constexpr std::string_view prefix = ".xenia-profile-intents-v3-";
    if (!public_shell && name.starts_with(prefix)) {
      const auto suffix = name.substr(prefix.size());
      allowed = suffix.size() >= 34 && suffix.size() <= 53 && suffix[32] == '-' &&
          std::all_of(suffix.begin(), suffix.begin() + 32, [](char c) {
            return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
          }) && std::all_of(suffix.begin() + 33, suffix.end(), [](char c) {
            return c >= '0' && c <= '9';
          });
    }
    struct stat info;
    if (!allowed || fstatat(state_->directory, row->d_name, &info, AT_SYMLINK_NOFOLLOW)) {
      error = allowed ? NativeError() : Error(std::errc::permission_denied); break;
    }
    const bool marker = !public_shell &&
        (name == "owner.v1" || content_link);
    if ((marker ? !S_ISREG(info.st_mode) || info.st_nlink != 1
                : !S_ISDIR(info.st_mode)) || info.st_uid != geteuid() ||
        (info.st_mode & 077) || info.st_dev != state_->device) {
      error = Error(std::errc::permission_denied); break;
    }
    auto pinned = PinAt(state_->directory, row->d_name);
    if (pinned.value < 0) { error = NativeError(); break; }
    error = ConfinedMount(pinned.value, state_->mount);
    if (error) { break; }
    errno = 0;
  }
  if (!error && errno) { error = NativeError(); }
  closedir(stream);
  return error;
#else
  return Error(std::errc::operation_not_supported);
#endif
}
std::error_code HostStorageRoot::PrivateDirectory() const {
#if !XE_PLATFORM_WIN32
  struct stat info;
  if (fstat(state_->directory, &info)) { return NativeError(); }
  if (!S_ISDIR(info.st_mode) || info.st_uid != geteuid() || (info.st_mode & 077)) {
    return Error(std::errc::permission_denied);
  }
  return Validate();
#else
  return Error(std::errc::operation_not_supported);
#endif
}
std::shared_ptr<HostStorageRoot> HostStorageRoot::IndependentNamespaceView(
    std::error_code& error) const {
  error = Validate();
  if (error) { return {}; }
#if !XE_PLATFORM_WIN32
  auto next = std::make_unique<State>();
  next->directory = openat(state_->directory, ".",
                           O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  struct stat info;
  if (next->directory < 0 || fstat(next->directory, &info)) {
    error = NativeError(); return {};
  }
  if (info.st_dev != state_->device || info.st_ino != state_->inode ||
      (error = ConfinedMount(next->directory, state_->mount))) {
    error = error ? error : Error(std::errc::state_not_recoverable); return {};
  }
  next->device = state_->device;
  next->inode = state_->inode;
  next->mount = state_->mount;
  next->path = state_->path;
  next->directory_identity = state_->directory_identity;
  next->read_only = state_->read_only;
  next->space = state_->space;
  next->dependency = shared_from_this();
  next->format_eligible = state_->format_eligible;
  return std::shared_ptr<HostStorageRoot>(new HostStorageRoot(std::move(next)));
#else
  error = Error(std::errc::operation_not_supported); return {};
#endif
}
std::shared_ptr<HostStorageRoot> HostStorageRoot::RetainBootstrapNamespace(
    const HostStorageIntentLease& lease, std::error_code& error) const {
  error.clear();
  if (global_critical_region::is_held_by_current_thread()) {
    error = Error(std::errc::operation_not_permitted); return {};
  }
#if XE_PLATFORM_LINUX || XE_PLATFORM_MAC
  if (!IntentLeaseMatches(lease) || read_only() || is_guest_private() ||
      HostStorageProtection::Guest(*this) || has_guest_projection()) {
    error = Error(std::errc::permission_denied); return {};
  }
  static_cast<void>(IntentRootIdentity(error));
  if (error) { return {}; }
  auto anchor = CloneNativeLineage(error);
  if (!anchor || !IntentLeaseMatches(lease)) {
    if (!error) { error = Error(std::errc::permission_denied); }
    return {};
  }
  return anchor;
#else
  error = Error(std::errc::operation_not_supported); return {};
#endif
}
std::shared_ptr<HostStorageRoot> HostStorageRoot::CloneNativeLineage(
    std::error_code& error) const {
#if XE_PLATFORM_LINUX || XE_PLATFORM_MAC
  // Retain the same physical ancestor/dependency checks, without retaining
  // their logical generation objects. In particular, an ancestor replaced by
  // a symlink cannot become current merely because the final inode matches.
  // These copies carry no protection, permit, projection or guest authority.
  std::map<const HostStorageRoot*, std::shared_ptr<HostStorageRoot>> copies;
  std::set<const HostStorageRoot*> visiting;
  auto clone = [&](auto&& self, const HostStorageRoot& root,
                   unsigned depth) -> std::shared_ptr<HostStorageRoot> {
    if (const auto found = copies.find(&root); found != copies.end()) {
      return found->second;
    }
    if (depth == 128 || copies.size() + visiting.size() == 128 ||
        !visiting.insert(&root).second) {
      error = Error(std::errc::value_too_large); return {};
    }
    error = root.Validate();
    if (error) { return {}; }
    auto next = std::make_unique<State>();
    next->directory = fcntl(root.state_->directory, F_DUPFD_CLOEXEC, 0);
    struct stat info;
    if (next->directory < 0 || fstat(next->directory, &info)) {
      error = NativeError(); return {};
    }
    if (!S_ISDIR(info.st_mode) || info.st_dev != root.state_->device ||
        info.st_ino != root.state_->inode ||
        (error = ConfinedMount(next->directory, root.state_->mount))) {
      if (!error) { error = Error(std::errc::state_not_recoverable); }
      return {};
    }
    next->path = root.state_->path;
    next->read_only = root.state_->read_only;
    next->space = root.state_->space;
    next->directory_identity = root.state_->directory_identity;
    next->device = root.state_->device;
    next->inode = root.state_->inode;
    next->mount = root.state_->mount;
    next->format_eligible = false;
#if XE_PLATFORM_MAC
    next->darwin_authority.store(root.state_->darwin_authority.load());
#endif
    if (root.state_->parent) {
      next->parent = self(self, *root.state_->parent, depth + 1);
      if (!next->parent) { return {}; }
    }
    if (root.state_->dependency) {
      next->dependency = self(self, *root.state_->dependency, depth + 1);
      if (!next->dependency) { return {}; }
    }
    auto copy = std::shared_ptr<HostStorageRoot>(new HostStorageRoot(std::move(next)));
    error = copy->Validate();
    if (error) { return {}; }
    visiting.erase(&root);
    copies.emplace(&root, copy);
    return copy;
  };
  return clone(clone, *this, 0);
#else
  error = Error(std::errc::operation_not_supported); return {};
#endif
}
std::shared_ptr<HostStorageRoot> HostStorageRoot::MountedFilesystemRoot(
    std::error_code& error) const {
  error = Validate();
  if (error) { return {}; }
  std::filesystem::path mounted;
#if XE_PLATFORM_LINUX
  std::ifstream stream("/proc/self/mountinfo");
  std::string line;
  size_t count = 0;
  while (std::getline(stream, line)) {
    if (++count > 4096 || line.size() > 65536) {
      error = Error(std::errc::value_too_large); return {};
    }
    uint64_t id, parent;
    std::string device, base, mount;
    std::istringstream fields(line);
    if ((fields >> id >> parent >> device >> base >> mount) &&
        id == state_->mount.id) { mounted = MountUnescape(mount); break; }
  }
#elif XE_PLATFORM_MAC
  mounted = state_->mount.mountpoint;
#else
  error = Error(std::errc::operation_not_supported); return {};
#endif
  if (mounted.empty()) { error = Error(std::errc::no_such_device); return {}; }
  auto root = Open(mounted, false, error, 0, state_->space);
#if !XE_PLATFORM_WIN32
  if (root && (root->state_->device != state_->device ||
               root->state_->mount.id != state_->mount.id)) {
    error = Error(std::errc::cross_device_link); return {};
  }
#endif
  return root;
}

HostStorageRoot::HostStorageRoot(std::unique_ptr<State> state)
    : state_(std::move(state)) {}
HostStorageRoot::~HostStorageRoot() = default;
const std::filesystem::path& HostStorageRoot::path() const {
  return state_->path;
}
const std::shared_ptr<const HostStorageDirectoryIdentity>&
HostStorageRoot::directory_identity() const { return state_->directory_identity; }

bool HostStorageRoot::read_only() const { return state_->read_only; }
bool HostStorageRoot::native_filesystem_authority() const {
#if !XE_PLATFORM_WIN32
  return !Validate() && state_->space->NativeAuthorityFor(state_->device);
#else
  return false;
#endif
}
void HostStorageRoot::Retire() {
  state_->retired.store(true);
  state_->retirement_stop.request_stop();
}
bool HostStorageRoot::LinkPreparationCancellation(HostStoragePreparationWait& wait) const {
  return wait.LinkCancellation(state_->retirement_stop.get_token()) &&
      wait.LinkCancellationFlag(state_->cache_owner_canceled) &&
      wait.LinkCancellationFlag(state_->cache_slot_canceled) &&
      (!state_->parent || state_->parent->LinkPreparationCancellation(wait)) &&
      (!state_->dependency || state_->dependency->LinkPreparationCancellation(wait));
}
bool HostStorageRoot::retiring() const {
  return state_->retired.load(std::memory_order_acquire) ||
         (state_->cache_owner_canceled && state_->cache_owner_canceled->load(std::memory_order_acquire)) ||
         (state_->cache_slot_canceled && state_->cache_slot_canceled->load(std::memory_order_acquire)) ||
         (state_->parent && state_->parent->retiring());
}
bool HostStorageRoot::BindCacheCancellation(std::shared_ptr<const std::atomic<bool>> owner,
    std::shared_ptr<const std::atomic<bool>> slot) {
  if(!owner || !slot || owner->load(std::memory_order_acquire) || slot->load(std::memory_order_acquire) ||
      state_->cache_owner_canceled || state_->cache_slot_canceled || !state_->guest_mount)return false;
  state_->cache_owner_canceled=std::move(owner);state_->cache_slot_canceled=std::move(slot);return true;
}
size_t HostStorageRoot::open_file_count() const {
  return state_->open_handles.load();
}
void HostStorageRoot::RetainHandle() const {
  state_->open_handles.fetch_add(1);
  if (state_->parent) {
    state_->parent->RetainHandle();
  }
}
void HostStorageRoot::ReleaseHandle() const {
  state_->open_handles.fetch_sub(1);
  if (state_->parent) {
    state_->parent->ReleaseHandle();
  }
}
std::error_code HostStorageRoot::Validate() const {
  if (retiring()) {
    return Error(std::errc::no_such_device);
  }
  if (state_->parent) {
    const auto error = state_->parent->Validate();
    if (error) {
      return error;
    }
  }
  if (state_->dependency) {
    const auto error = state_->dependency->Validate();
    if (error) {
      return error;
    }
  }
#if XE_PLATFORM_WIN32
  std::error_code error;
  if (!std::filesystem::is_directory(state_->path, error)) {
    return error ? error : Error(std::errc::no_such_device);
  }
#else
  struct stat info;
  if (stat(state_->path.c_str(), &info)) {
    return errno == ENOENT ? Error(std::errc::no_such_device) : NativeError();
  }
  if (!S_ISDIR(info.st_mode) || info.st_dev != state_->device ||
      info.st_ino != state_->inode) {
    return Error(std::errc::no_such_device);
  }
  // A bind replacement can retain both inode and st_dev. Validate the pathname
  // against the root's pinned mount generation as well.
  Fd current(open(state_->path.c_str(),
                  O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
  if (current.value < 0) {
    return NativeError();
  }
  if (const auto error = ConfinedMount(current.value, state_->mount)) {
    return error;
  }
#if XE_PLATFORM_MAC
  if (const auto authority = state_->darwin_authority.load()) {
    if (const auto error = DarwinProofCurrent(state_->directory, *authority)) {
      return error;
    }
  }
#endif
#endif
  return {};
}
std::shared_ptr<HostStorageRoot> HostStorageRoot::Open(
    const std::filesystem::path& path, bool readonly, std::error_code& error,
    uint64_t reserve, std::shared_ptr<HostStorageSpace> space) {
  error.clear();
  if (!NativeSpelling(path)) {
    error = Error(std::errc::invalid_argument); return {};
  }
  auto state = std::make_unique<State>();
  state->path = std::filesystem::canonical(path, error);
  if (error) {
    return {};
  }
  state->read_only = readonly;
#if XE_PLATFORM_WIN32
  state->directory = CreateFileW(
      state->path.c_str(), FILE_READ_ATTRIBUTES,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
      OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
      nullptr);
  if (state->directory == INVALID_HANDLE_VALUE) {
    error = NativeError();
    return {};
  }
#else
  state->directory = open(state->path.c_str(),
                          O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  struct stat info;
  if (state->directory < 0 || fstat(state->directory, &info)) {
    error = NativeError();
    return {};
  }
  state->device = info.st_dev;
  state->inode = info.st_ino;
  state->directory_identity = HostStorageDirectoryIdentity::Capture(state->path,error);
  if (!state->directory_identity ||
      !state->directory_identity->MatchesNativeDirectory(state->device,state->inode)) {
    if (!error) { error = Error(std::errc::state_not_recoverable); } return {};
  }
  error = MountForFd(state->directory, state->mount);
  if (error) {
    return {};
  }
#endif
#if !XE_PLATFORM_WIN32
  Fd ancestor(openat(state->directory, "..",
                     O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
  struct stat parent_info;
  MountIdentity parent_mount;
  if (ancestor.value < 0 || fstat(ancestor.value, &parent_info) ||
      MountForFd(ancestor.value, parent_mount)) {
    state->format_eligible = false;
  } else if (parent_info.st_dev != state->device ||
             parent_mount.id != state->mount.id ||
             parent_info.st_ino == state->inode) {
    // Mounted/reference filesystem roots are never destructively formatable.
    state->format_eligible = false;
  }
#endif
  state->space = space ? std::move(space)
                       : HostStorageSpace::ForPath(state->path, reserve, error);
  if (!state->space) {
    return {};
  }
  return std::shared_ptr<HostStorageRoot>(
      new HostStorageRoot(std::move(state)));
}
HostStorageSnapshot HostStorageRoot::Observe() const {
  HostStorageSnapshot result;
  result.error = Validate();
  if (result.error) {
    return result;
  }
  result = state_->space->Observe(state_->path);
  result.read_only = state_->read_only;
  if (result.read_only) {
    result.usable_bytes = 0;
  }
  return result;
}
std::unique_ptr<HostStorageSpace::MutationGuard> HostStorageRoot::Mutation(
    std::error_code& error) const {
  error = Validate();
  if (error) {
    return {};
  }
  if (state_->read_only) {
    error = Error(std::errc::read_only_file_system);
    return {};
  }
  auto guard = state_->space->Begin(state_->path, error, state_->directory_identity);
  if (!guard) {
    return {};
  }
  error = Validate();
  if (!error && is_guest_private()) {
    error = HostStorageProtection::PrivateContents(*this, nullptr, true);
  }
  return error ? nullptr : std::move(guard);
}
std::shared_ptr<HostStorageRoot> HostStorageRoot::Subroot(
    const std::filesystem::path& relative, bool readonly,
    std::error_code& error, HostStorageFailureObservation* diagnostic) const {
  HostStorageFailureScope observation{diagnostic,error,3};
  observation.At(1);
  if (!ContentNamespaceReady(relative)) {
    error = Error(std::errc::device_or_resource_busy); observation.Capture(__LINE__); return {};
  }
  observation.At(0);
  if (HostStorageProtection::Guest(*this)) {
    error = CheckGuestAccess(relative);
    if (error) { observation.Capture(__LINE__); return {}; }
  }
  error = Validate();
  if (is_guest_private() && !relative.empty()) {
    error = Error(std::errc::permission_denied);
    observation.Capture(__LINE__); return {};
  }
  if (error || !Relative(relative, true)) {
    if (!error) {
      error = Error(std::errc::invalid_argument);
    }
    observation.Capture(__LINE__); return {};
  }
#if XE_PLATFORM_WIN32
  if (!relative.empty()) {
    error = Error(std::errc::operation_not_supported);
    observation.Capture(__LINE__); return {};
  }
  auto next = Open(state_->path, readonly || state_->read_only, error, 0,
                   state_->space);
  if (next) {
    next->state_->parent = shared_from_this();
  }
  return next;
#else
  std::string leaf;
  auto parent = Parent(state_->directory, relative, leaf, error);
  if (parent.value < 0) {
    observation.Capture(__LINE__); return {};
  }
  auto next = std::make_unique<State>();
  next->directory = openat(parent.value, leaf.c_str(),
                           O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  struct stat info;
  if (next->directory < 0 || fstat(next->directory, &info)) {
    error = NativeError();
    observation.Capture(__LINE__); return {};
  }
  error = ConfinedMount(next->directory, state_->mount);
  if (error) {
    observation.Capture(__LINE__); return {};
  }
  next->mount = state_->mount;
  next->device = info.st_dev;
  next->inode = info.st_ino;
  // An independently retired same-root view preserves the producer's exact
  // directory spelling. Appending an empty path adds a trailing component.
  next->path = relative.empty() ? state_->path : state_->path / relative;
  next->directory_identity = relative.empty() ? state_->directory_identity
      : HostStorageDirectoryIdentity::Capture(next->path,error);
  if (!next->directory_identity ||
      !next->directory_identity->MatchesNativeDirectory(next->device,next->inode)) {
    if (!error) { error=Error(std::errc::state_not_recoverable); } observation.Capture(__LINE__); return {};
  }
  next->read_only = readonly || state_->read_only;
  next->format_eligible = relative.empty() ? state_->format_eligible : true;
  next->space = state_->space;
  next->parent = shared_from_this();
  return std::shared_ptr<HostStorageRoot>(new HostStorageRoot(std::move(next)));
#endif
}
std::shared_ptr<HostStorageRoot> HostStorageRoot::WithDependency(
    std::shared_ptr<const HostStorageRoot> source,
    std::error_code& error) const {
  if (!source) {
    error = Error(std::errc::invalid_argument);
    return {};
  }
  error = source->Validate();
  if (error) {
    return {};
  }
  auto view = Subroot({}, false, error);
  if (view) {
    view->state_->read_only = view->state_->read_only || source->read_only();
    view->state_->dependency = std::move(source);
  }
  return view;
}
class PolicyFileHandle final : public FileHandle {
 public:
  PolicyFileHandle(std::shared_ptr<const HostStorageRoot> root,
                   std::unique_ptr<FileHandle> file, bool append,
                   uint32_t access,
                   std::shared_ptr<const HostStorageFileLease> lease)
      : FileHandle(file->path()),
        root_(std::move(root)),
        file_(std::move(file)),
        append_(append),
        access_(access),
        lease_(std::move(lease)) {
    root_->RetainHandle();
  }
  ~PolicyFileHandle() override {
    file_.reset();
    root_->ReleaseHandle();
  }
  intptr_t native_handle() const override { return file_->native_handle(); }
  std::shared_ptr<const HostStorageFileLease> storage_lease() const override {
    return lease_;
  }
  std::optional<FileHandleInfo> QueryInfo(
      std::error_code& error) const override {
    error = root_->Validate();
    return error ? std::nullopt : file_->QueryInfo(error);
  }
  std::optional<uint64_t> QueryAllocatedBytes(
      std::error_code& error) const override {
    error = root_->Validate();
    return error ? std::nullopt : file_->QueryAllocatedBytes(error);
  }
  std::error_code LastError() const override { return last_error_; }
  FileIoResult ReadResult(size_t offset, std::span<uint8_t> bytes) override {
    if (!(access_ & (FileAccess::kGenericRead | FileAccess::kFileReadData |
                     FileAccess::kGenericAll | FileAccess::kGenericExecute))) {
      return {Error(std::errc::permission_denied), 0};
    }
    auto error = root_->Validate();
    if (error) {
      return {error, 0};
    }
    return file_->ReadResult(offset, bytes);
  }
  FileIoResult WriteResult(size_t offset,
                           std::span<const uint8_t> bytes) override {
    if (!(access_ & (FileAccess::kGenericWrite | FileAccess::kFileWriteData |
                     FileAccess::kFileAppendData | FileAccess::kGenericAll))) {
      return {Error(std::errc::permission_denied), 0};
    }
    std::error_code error;
    auto guard = root_->Mutation(error);
    if (!guard) {
      return {error, 0};
    }
    const auto info = file_->QueryInfo(error);
    if (!info) {
      return {error, 0};
    }
    if (info->directory || info->links > 1) {
      return {Error(std::errc::permission_denied), 0};
    }
    const uint64_t begin = append_ ? info->size : offset;
    if (bytes.size() > UINT64_MAX - begin ||
        begin + bytes.size() > uint64_t(INT64_MAX)) {
      return {Error(std::errc::value_too_large), 0};
    }
    const uint64_t growth = begin + bytes.size() > info->size
                                ? begin + bytes.size() - info->size
                                : 0;
    const uint64_t bound = std::max<uint64_t>(growth, bytes.size());
    if (bound > UINT64_MAX - kMetadataAllowance) {
      return {Error(std::errc::value_too_large), 0};
    }
    if (!bytes.empty()) {
      error = guard->Admit(bound + kMetadataAllowance);
      if (error) {
        return {error, 0};
      }
    }
    if (!bytes.empty() && lease_) { lease_->clean_overwrite = false; }
    return file_->WriteResult(offset, bytes);
  }
  FileIoResult WriteAllResult(size_t offset, std::span<const uint8_t> bytes) override {
    if (!(access_ & (FileAccess::kGenericWrite | FileAccess::kFileWriteData |
                     FileAccess::kFileAppendData | FileAccess::kGenericAll))) {
      return {Error(std::errc::permission_denied), 0};
    }
    std::error_code error;
    auto guard = root_->Mutation(error);
    if (!guard) { return {error, 0}; }
    const auto info = file_->QueryInfo(error);
    if (!info) { return {error ? error : Error(std::errc::io_error), 0}; }
    if (!info->regular || info->links != 1 || !lease_ ||
        info->filesystem_id != lease_->filesystem || info->file_id != lease_->file) {
      return {Error(std::errc::permission_denied), 0};
    }
    const uint64_t begin = append_ ? info->size : offset;
    if (bytes.size() > UINT64_MAX - begin || begin + bytes.size() > uint64_t(INT64_MAX)) {
      return {Error(std::errc::value_too_large), 0};
    }
    const uint64_t growth = begin + bytes.size() > info->size ? begin + bytes.size() - info->size : 0;
    const uint64_t bound = std::max<uint64_t>(growth, bytes.size());
    if (bound > UINT64_MAX - kMetadataAllowance) {
      return {Error(std::errc::value_too_large), 0};
    }
    if (!bytes.empty()) {
      error = guard->Admit(bound + kMetadataAllowance);
      if (error) { return {error, 0}; }
      lease_->clean_overwrite = false;
    }
    // Actual native writes share one retained file identity/admission. No
    // second cooperating append/overwrite can interleave between short chunks.
    // Underlying native handles have no guest/provider callback here.
    return file_->WriteAllResult(append_ ? 0 : offset, bytes);
  }
  std::error_code ResizeResult(size_t length) override {
    if (!(access_ & (FileAccess::kGenericWrite | FileAccess::kFileWriteData |
                     FileAccess::kGenericAll))) {
      return Error(std::errc::permission_denied);
    }
    std::error_code error;
    auto guard = root_->Mutation(error);
    if (!guard) {
      return error;
    }
    const auto info = file_->QueryInfo(error);
    if (!info) {
      return error;
    }
    if (info->directory || info->links > 1) {
      return Error(std::errc::permission_denied);
    }
    if (guard->Conflicts(lease_->path, false, info->filesystem_id,
                         info->file_id, lease_, true)) {
      return Error(std::errc::device_or_resource_busy);
    }
    if (length > uint64_t(INT64_MAX)) {
      return Error(std::errc::value_too_large);
    }
    if (length > info->size) {
      const uint64_t growth = length - info->size;
      if (growth > UINT64_MAX - kMetadataAllowance) {
        return Error(std::errc::value_too_large);
      }
      error = guard->Admit(growth + kMetadataAllowance);
      if (error) {
        return error;
      }
    }
    lease_->clean_overwrite = false;
    return file_->ResizeResult(length);
  }
  bool Read(size_t offset, void* buffer, size_t length,
            size_t* transferred) override {
    const auto result =
        ReadResult(offset, {static_cast<uint8_t*>(buffer), length});
    *transferred = result.bytes;
    last_error_ = result.error;
    return !result.error;
  }
  bool Write(size_t offset, const void* buffer, size_t length,
             size_t* transferred) override {
    const auto result =
        WriteResult(offset, {static_cast<const uint8_t*>(buffer), length});
    *transferred = result.bytes;
    last_error_ = result.error;
    return !result.error;
  }
  bool SetLength(size_t length) override {
    last_error_ = ResizeResult(length);
    return !last_error_;
  }
  bool SetTimes(uint64_t create, uint64_t access, uint64_t write) override {
    std::error_code error;
    auto guard = root_->Mutation(error);
    if (!guard) {
      last_error_ = error;
      return false;
    }
    const auto info = file_->QueryInfo(error);
    if (!info) {
      last_error_ = error;
      return false;
    }
    if (!info->directory && info->links > 1) {
      last_error_ = Error(std::errc::permission_denied);
      return false;
    }
    error = guard->Admit(kMetadataAllowance);
    if (error) {
      last_error_ = error;
      return false;
    }
    const bool ok = file_->SetTimes(create, access, write);
    last_error_ = ok ? std::error_code{} : file_->LastError();
    return ok;
  }
  FileLengthFlushResult FlushLengthResult(size_t length) override {
    if (!(access_ & (FileAccess::kGenericWrite | FileAccess::kFileWriteData |
                     FileAccess::kGenericAll))) {
      return {Error(std::errc::permission_denied)};
    }
    if (!lease_ || !lease_->exclusive) {
      return {Error(std::errc::operation_not_supported)};
    }
    std::error_code error;
    auto guard = root_->Mutation(error);
    if (!guard) {
      return {error};
    }
    const auto info = file_->QueryInfo(error);
    if (!info) {
      return {error ? error : Error(std::errc::io_error)};
    }
    if (!info->regular || info->links != 1 ||
        info->filesystem_id != lease_->filesystem ||
        info->file_id != lease_->file) {
      return {Error(std::errc::permission_denied)};
    }
    if (guard->Conflicts(lease_->path, false, info->filesystem_id,
                         info->file_id, lease_, true)) {
      return {Error(std::errc::device_or_resource_busy)};
    }
    if (length > uint64_t(INT64_MAX)) {
      return {Error(std::errc::value_too_large)};
    }
    if (length > info->size) {
      const uint64_t growth = length - info->size;
      if (growth > UINT64_MAX - kMetadataAllowance) {
        return {Error(std::errc::value_too_large)};
      }
      error = guard->Admit(growth + kMetadataAllowance);
      if (error) {
        return {error};
      }
    }
    // Use the same retained native handle, under the same admission/identity
    // guard, for truncation and fsync. No guest/provider callback occurs here.
    lease_->clean_overwrite = false;
    const auto result = file_->FlushLengthResult(length);
    if (!result.error && result.durable && result.published_length == length) {
      lease_->clean_size = length;
      lease_->clean_overwrite = true;
    }
    return result;
  }
  std::error_code FlushResult() override {
    std::error_code error;
    auto guard = root_->Mutation(error);
    if (!guard) {
      return error;
    }
    return file_->FlushResult();
  }
  void Flush() override { last_error_ = FlushResult(); }

 private:
  std::shared_ptr<const HostStorageRoot> root_;
  std::unique_ptr<FileHandle> file_;
  bool append_;
  uint32_t access_;
  std::error_code last_error_;
  std::shared_ptr<const HostStorageFileLease> lease_;
};
std::unique_ptr<FileHandle> HostStorageRoot::OpenFile(
    const std::filesystem::path& relative, uint32_t access,
    std::error_code& error, bool create_exclusive,
    HostStorageFailureObservation* diagnostic) const {
  return OpenFileInternal(relative, access, error, create_exclusive, false,
                          false, false, nullptr, diagnostic);
}
std::unique_ptr<FileHandle> HostStorageRoot::ReserveOverwrite(
    const std::filesystem::path& relative, std::error_code& error) const {
  return OpenFileInternal(relative, FileAccess::kGenericAll, error, false,
                          true);
}
std::unique_ptr<FileHandle> HostStorageRoot::OpenFileInternal(
    const std::filesystem::path& relative, uint32_t access,
    std::error_code& error, bool create_exclusive, bool overwrite,
    bool nonwaiting_admission, bool stage, bool* created,
    HostStorageFailureObservation* diagnostic,
    HostStoragePreparationWait* preparation) const {
  HostStorageFailureScope observation{diagnostic,error,2};
  if(diagnostic && !diagnostic->line){diagnostic->root=uint64_t(reinterpret_cast<uintptr_t>(this));
    diagnostic->space=uint64_t(reinterpret_cast<uintptr_t>(state_->space.get()));diagnostic->available|=24;}
  if (created) { *created = false; }
  if ((is_recovery_private() || is_profile_intent_private() || is_content_mutation_private()) &&
      (!Relative(relative) || relative.has_parent_path() ||
       !(is_content_mutation_private() ? ContentMutationLeaf(relative.string()) : is_profile_intent_private() ? IntentLeaf(relative.string())
                                    : RecoveryLeaf(relative.string())))) {
    error = Error(std::errc::permission_denied);
    observation.Capture(__LINE__); return {};
  }
  if (HostStorageProtection::Guest(*this)) {
    error = CheckGuestAccess(relative);
    if (error) { observation.Capture(__LINE__); return {}; }
  }
  error = Validate();
  if (is_guest_private() && relative.has_parent_path()) {
    error = Error(std::errc::permission_denied);
    observation.Capture(__LINE__); return {};
  }
  if (error || !Relative(relative, true)) {
    if (!error) {
      error = Error(std::errc::invalid_argument);
    }
    observation.Capture(__LINE__); return {};
  }
  observation.At(1);
  auto guard = preparation
                   ? state_->space->BeginPreparation(state_->path,error,*preparation,state_->directory_identity,diagnostic)
               : nonwaiting_admission
                   ? state_->space->TryBegin(state_->path, error, state_->directory_identity, diagnostic)
               : (create_exclusive || overwrite)
                   ? Mutation(error)
                   : state_->space->Begin(state_->path, error, state_->directory_identity);
  if (!guard) {
    observation.Capture(__LINE__); return {};
  }
  error = Validate();
  if (error) {
    observation.Capture(__LINE__); return {};
  }
  if (create_exclusive && (is_recovery_private() || is_profile_intent_private() || is_content_mutation_private())) {
    // Refuse BEFORE creation when no recovery directory slot remains.
    error = HostStorageProtection::Contents(*this, nullptr, true, 8191,
        is_recovery_private(), is_profile_intent_private(), is_content_mutation_private());
    if (error) { observation.Capture(__LINE__); return {}; }
  }
  if (create_exclusive) {
    error = guard->Admit(kMetadataAllowance);
    if (error) {
      observation.Capture(__LINE__); return {};
    }
  }
#if XE_PLATFORM_WIN32
  error = Error(std::errc::operation_not_supported);
  observation.Capture(__LINE__); return {};
#else
  std::string leaf;
  auto parent = Parent(state_->directory, relative, leaf, error);
  if (parent.value < 0) {
    observation.Capture(__LINE__); return {};
  }
  if (HostStorageProtection::Guest(*this)) {
    const auto projection = Projection();
    if (has_guest_projection() && !projection) { error = Error(std::errc::no_such_device); observation.Capture(__LINE__); return {}; }
    if (projection) {
      error = HostStorageProtection::ProjectedDirectory(*projection, parent.value,
                                                        state_->device, state_->mount, false, PublicProjectionScope());
      if (error) { observation.Capture(__LINE__); return {}; }
    }
  }

  const bool read =
      access & (FileAccess::kGenericRead | FileAccess::kFileReadData |
                FileAccess::kGenericExecute | FileAccess::kGenericAll);
  const bool write =
      !state_->read_only &&
      (access & (FileAccess::kGenericWrite | FileAccess::kFileWriteData |
                 FileAccess::kFileAppendData | FileAccess::kGenericAll));
  if ((is_profile_intent_private() || is_content_mutation_private()) && write && !create_exclusive) {
    error = Error(std::errc::permission_denied); observation.Capture(__LINE__); return {};
  }
  const bool append = write && !(access & (FileAccess::kGenericWrite |
                                           FileAccess::kFileWriteData |
                                           FileAccess::kGenericAll));
  int flags = (read && write ? O_RDWR
               : write       ? O_WRONLY
                             : O_RDONLY) |
              O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK;
  if (append) {
    flags |= O_APPEND;
  }
  if (create_exclusive) {
    flags |= O_CREAT | O_EXCL;
  }
  observation.At(2);
  if (!ContentNamespaceReady(relative)) {
    error = Error(std::errc::device_or_resource_busy); observation.Capture(__LINE__); return {};
  }
  observation.At(3);
  const int fd = openat(parent.value, leaf.c_str(), flags, 0600);
  if (fd < 0) {
    error = NativeError();
    observation.Capture(__LINE__); return {};
  }
  if (created && create_exclusive) { *created = true; }
  struct stat opened;
  observation.At(4);
  if (fstat(fd, &opened)) {
    error = NativeError();
    close(fd);
    observation.Capture(__LINE__); return {};
  }
  if ((!S_ISREG(opened.st_mode) && !S_ISDIR(opened.st_mode)) ||
      opened.st_dev != state_->device) {
    error = opened.st_dev != state_->device
                ? Error(std::errc::cross_device_link)
                : Error(std::errc::operation_not_supported);
    close(fd);
    observation.Capture(__LINE__); return {};
  }
  error = ConfinedMount(fd, state_->mount);
  if (error) {
    close(fd);
    observation.Capture(__LINE__); return {};
  }
  auto file = FileHandle::AdoptNative(fd, append);
  observation.At(5);
  const auto info = file->QueryInfo(error);
  if (!info) {
    observation.Capture(__LINE__); return {};
  }
  if(diagnostic && !diagnostic->line && info){diagnostic->filesystem=info->filesystem_id;diagnostic->file=info->file_id;
    diagnostic->available|=2;}
  observation.At(6);
  if ((overwrite || stage) && (!info->regular || info->links > 1)) {
    error = Error(std::errc::permission_denied);
    observation.Capture(__LINE__); return {};
  }
  observation.At(7);
  if ((overwrite || stage) && guard->Conflicts(state_->path / relative, false,
                                     info->filesystem_id, info->file_id, ContentPermit())) {
    error = Error(std::errc::device_or_resource_busy);
    observation.Capture(__LINE__); return {};
  }
  observation.At(8);
  if (guard->Reserved(state_->path / relative, *info, ContentPermit())) {
    error = Error(std::errc::device_or_resource_busy);
    observation.Capture(__LINE__); return {};
  }
  observation.At(0);
  if (is_guest_private()) {
    error = HostStorageProtection::PrivateContents(*this);
  }
  if (!error) {
    error = HostStorageProtection::GuestFile(*this, *info);
  }
  if (error) {
    observation.Capture(__LINE__); return {};
  }
  // storage_lease() is an owning public token and may outlive the FileHandle.
  // Duplicate the actual validated description before publishing its weak row;
  // never leave a live lease whose native inode has already been recycled.
  Fd identity_pin(fcntl(int(file->native_handle()), F_DUPFD_CLOEXEC, 0));
  struct stat pinned;
  if (identity_pin.value < 0 || fstat(identity_pin.value, &pinned)) {
    error = NativeError(); observation.Capture(__LINE__); return {};
  }
  if (uint64_t(pinned.st_dev) != info->filesystem_id ||
      uint64_t(pinned.st_ino) != info->file_id ||
      bool(S_ISREG(pinned.st_mode)) != info->regular ||
      bool(S_ISDIR(pinned.st_mode)) != info->directory) {
    error = Error(std::errc::state_not_recoverable); observation.Capture(__LINE__); return {};
  }
  auto lease = guard->TrackOpen(state_->path / relative, *info,
                                 overwrite || stage, false, ContentPermit());
  lease->identity_pin = std::move(identity_pin);
  return std::make_unique<PolicyFileHandle>(shared_from_this(), std::move(file),
                                            append, access, std::move(lease));
#endif
}
std::error_code HostStorageRoot::Create(const std::filesystem::path& relative,
                                        bool directory) const {
  if (HostStorageProtection::Guest(*this)) {
    if (auto denied = CheckGuestAccess(relative)) { return denied; }
  }
  if (is_profile_intent_private() || is_content_mutation_private()) { return Error(std::errc::permission_denied); }
  if (!directory) {
    std::error_code error;
    auto file = OpenFile(relative, FileAccess::kFileWriteData, error, true);
    return error;
  }
  if (is_guest_private()) {
    return Error(std::errc::permission_denied);
  }
  if (!Relative(relative)) {
    return Error(std::errc::invalid_argument);
  }
  std::error_code error;
  auto guard = Mutation(error);
  if (!guard) {
    return error;
  }
  if (!ContentNamespaceReady(relative)) { return Error(std::errc::device_or_resource_busy); }
  error = guard->Admit(kMetadataAllowance);
  if (error) {
    return error;
  }
#if XE_PLATFORM_WIN32
  return Error(std::errc::operation_not_supported);
#else
  std::string leaf;
  auto parent = Parent(state_->directory, relative, leaf, error);
  if (parent.value < 0) {
    return error;
  }
  if (HostStorageProtection::Guest(*this)) {
    const auto projection = Projection();
    if (has_guest_projection() && !projection) { return Error(std::errc::no_such_device); }
    if (projection) {
      error = HostStorageProtection::ProjectedDirectory(*projection, parent.value,
                                                        state_->device, state_->mount, false, PublicProjectionScope());
      if (error) { return error; }
    }
  }

  return mkdirat(parent.value, leaf.c_str(), 0700) ? NativeError()
                                                   : std::error_code{};
#endif
}
std::error_code HostStorageRoot::Remove(const std::filesystem::path& relative,
                                        bool recursive) const {
  if (HostStorageProtection::Guest(*this)) {
    if (auto denied = CheckGuestAccess(relative, recursive)) { return denied; }
  }
  if (is_profile_intent_private() || is_content_mutation_private()) { return Error(std::errc::permission_denied); }
  if (!Relative(relative)) {
    return Error(std::errc::invalid_argument);
  }
  std::error_code error;
  auto guard = Mutation(error);
  if (!guard) {
    return error;
  }
#if XE_PLATFORM_WIN32
  return Error(std::errc::operation_not_supported);
#else
  std::string leaf;
  auto parent = Parent(state_->directory, relative, leaf, error);
  if (parent.value < 0) {
    return error;
  }
  struct stat target;
  if (fstatat(parent.value, leaf.c_str(), &target, AT_SYMLINK_NOFOLLOW)) {
    return NativeError();
  }
  if (guard->Conflicts(state_->path / relative, S_ISDIR(target.st_mode),
                       target.st_dev, target.st_ino)) {
    return Error(std::errc::device_or_resource_busy);
  }
  RemovalNode prepared;
  RemovalBudget budget;
  error = PreflightRemoval(parent.value, leaf, recursive, state_->device,
                           state_->mount, prepared, &budget,
                           HostStorageProtection::Guest(*this) ? this : nullptr);
  if (error) {
    return error;
  }
  if (RemovalLeaseConflict(*guard, state_->path / relative, prepared)) {
    return Error(std::errc::device_or_resource_busy);
  }
  return RemovePrepared(parent.value, prepared);
#endif
}

std::error_code HostStorageRoot::ResetDirectory(
    const std::filesystem::path& relative,
    const std::filesystem::path& preserved_leaf) const {
  if (!Relative(relative) ||
      (!preserved_leaf.empty() &&
       (!Relative(preserved_leaf) || preserved_leaf.has_parent_path()))) {
    return Error(std::errc::invalid_argument);
  }
  if (HostStorageProtection::Guest(*this)) {
    if (auto denied = CheckGuestAccess(relative, true)) { return denied; }
  }
  if (is_guest_private()) { return Error(std::errc::permission_denied); }
  std::error_code error;
  auto guard = Mutation(error);
  if (!guard) { return error; }
#if XE_PLATFORM_WIN32
  return Error(std::errc::operation_not_supported);
#else
  // Repeat projection/source checks after acquiring the actual filesystem
  // guard, before obtaining any deletion authority.
  if (HostStorageProtection::Guest(*this)) {
    if (auto denied = CheckGuestAccess(relative, true)) { return denied; }
  }
  std::string leaf;
  auto parent = Parent(state_->directory, relative, leaf, error);
  if (parent.value < 0) { return error; }
  RemovalNode prepared;
  RemovalBudget budget;
  error = PreflightRemoval(parent.value, leaf, true, state_->device,
                           state_->mount, prepared, &budget,
                           HostStorageProtection::Guest(*this) ? this : nullptr);
  if (error) { return error; }
  const bool directory = S_ISDIR(prepared.identity.st_mode);
  if (!directory && !S_ISREG(prepared.identity.st_mode)) {
    return Error(std::errc::operation_not_supported);
  }
  std::string retained;
  struct stat retained_info{};
  bool has_retained = false;
  if (directory) {
    prepared.preserve_directory = true;
    if (!preserved_leaf.empty()) {
      retained = preserved_leaf.string();
      const auto keep = std::find_if(prepared.children.begin(),
                                    prepared.children.end(),
          [&](const RemovalNode& child) { return child.name == retained; });
      if (keep != prepared.children.end()) {
        retained_info = keep->identity;
        has_retained = true;
      }
      std::erase_if(prepared.children,
          [&](const RemovalNode& child) { return child.name == retained; });
    }
  }
  if (RemovalLeaseConflict(*guard, state_->path / relative, prepared)) {
    return Error(std::errc::device_or_resource_busy);
  }
  if (!directory) {
    // Replacing a file needs a new directory inode. Deny space/authority
    // failure before destroying the old file; use the same shared reserve.
    error = guard->Admit(kMetadataAllowance);
    if (error) { return error; }
  }
  error = Validate();
  if (!error) { error = RemovePrepared(parent.value, prepared); }
  if (error) { return error; }
  if (directory) {
    // Unlike directory deletion, a kept directory cannot use rmdir's ENOTEMPTY
    // check. Do not report success if an uncooperating writer added/replaced
    // an unprepared member while this operation was removing children.
    return CheckResetContents(parent.value, prepared, retained,
                               has_retained ? &retained_info : nullptr);
  }
  // No path reopen, source substitution or success after a failed mkdir.
  return mkdirat(parent.value, leaf.c_str(), 0700) ? NativeError()
                                                  : std::error_code{};
#endif
}

std::error_code HostStorageRoot::Rename(
    const std::filesystem::path& from, const std::filesystem::path& to,
    bool replace, std::shared_ptr<const HostStorageFileLease> permitted) const {
  if (HostStorageProtection::Guest(*this)) {
    if (auto denied = CheckGuestAccess(from, true)) { return denied; }
    if (auto denied = CheckGuestAccess(to, true)) { return denied; }
  }
  if (is_profile_intent_private() || is_content_mutation_private()) { return Error(std::errc::permission_denied); }
  if (is_recovery_private() &&
      (!RecoveryLeaf(from.string()) || !RecoveryLeaf(to.string()))) {
    return Error(std::errc::permission_denied);
  }
  if (!Relative(from) || !Relative(to)) {
    return Error(std::errc::invalid_argument);
  }
  std::error_code error;
  auto guard = Mutation(error);
  if (!guard) {
    return error;
  }
  error = guard->Admit(kMetadataAllowance);
  if (error) {
    return error;
  }
#if XE_PLATFORM_WIN32
  return Error(std::errc::operation_not_supported);
#else
  std::string first, second;
  auto a = Parent(state_->directory, from, first, error);
  if (a.value < 0) {
    return error;
  }
  auto b = Parent(state_->directory, to, second, error);
  if (b.value < 0) {
    return error;
  }
  struct stat info;
  if (fstatat(a.value, first.c_str(), &info, AT_SYMLINK_NOFOLLOW)) {
    return NativeError();
  }
  if (auto denied = HostStorageProtection::GuestFile(*this, SnapshotInfo(info))) {
    return denied;
  }
  if (permitted && permitted->exclusive) {
    return Error(std::errc::device_or_resource_busy);
  }
  if (permitted && (permitted->filesystem != uint64_t(info.st_dev) ||
                    permitted->file != uint64_t(info.st_ino))) {
    return Error(std::errc::invalid_argument);
  }
  if (guard->Conflicts(state_->path / from, S_ISDIR(info.st_mode), info.st_dev,
                       info.st_ino, permitted)) {
    return Error(std::errc::device_or_resource_busy);
  }
  if (!fstatat(b.value, second.c_str(), &info, AT_SYMLINK_NOFOLLOW)) {
    if (!replace) {
      return Error(std::errc::file_exists);
    }
    if (guard->Conflicts(state_->path / to, S_ISDIR(info.st_mode), info.st_dev,
                         info.st_ino)) {
      return Error(std::errc::device_or_resource_busy);
    }
  } else if (errno != ENOENT) {
    return NativeError();
  }
  if (renameat(a.value, first.c_str(), b.value, second.c_str())) {
    return NativeError();
  }
  guard->Moved(permitted, state_->path / to);
  return {};
#endif
}

namespace {
#if !XE_PLATFORM_WIN32
bool SnapshotAsciiEqual(std::string_view a, std::string_view b) {
  auto lower = [](unsigned char c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; };
  return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(),
      [&](unsigned char x, unsigned char y) { return lower(x) == lower(y); });
}
// Bounded inventory under the SAME common mutation authority as publication.
// No path-only fallback: multiple case spellings are a conflict, not a choice.
std::error_code SnapshotLeaf(int parent, std::string_view requested,
                             std::string& actual, bool& exists) {
  Fd scan(openat(parent, ".", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
  if (scan.value < 0) { return NativeError(); }
  DIR* stream = fdopendir(scan.value);
  if (!stream) { return NativeError(); }
  scan.Release();
  exists = false;
  actual = requested;
  std::error_code error;
  size_t count = 0;
  while (true) {
    errno = 0;
    auto* entry = readdir(stream);
    if (!entry) { if (errno) { error = NativeError(); } break; }
    if (++count > 65536) { error = Error(std::errc::value_too_large); break; }
    if (!SnapshotAsciiEqual(entry->d_name, requested)) { continue; }
    if (exists) { error = Error(std::errc::file_exists); break; }
    exists = true;
    actual = entry->d_name;
  }
  closedir(stream);
  return error;
}
FileHandleInfo SnapshotInfo(const struct stat& info) {
  return {uint64_t(info.st_size), uint64_t(info.st_nlink),
          bool(S_ISDIR(info.st_mode)), bool(S_ISREG(info.st_mode)),
          uint64_t(info.st_dev), uint64_t(info.st_ino)};
}
bool SameSnapshotInfo(const FileHandleInfo& a, const FileHandleInfo& b) {
  return a.regular && b.regular && a.links == 1 && b.links == 1 &&
      a.filesystem_id == b.filesystem_id && a.file_id == b.file_id &&
      a.size == b.size;
}
// The caller owns the common filesystem admission lock and pinned parent.
// This is not a substitute for that authority against arbitrary host writers.
std::error_code SnapshotRead(int fd, uint64_t maximum,
    FileHandleInfo& output_info, std::vector<uint8_t>& output,
    const FileHandleInfo* expected_info = nullptr) {
  struct stat before, after;
  if (fstat(fd, &before)) { return NativeError(); }
  if (!S_ISREG(before.st_mode) || before.st_nlink != 1 || before.st_size < 0) {
    return Error(std::errc::permission_denied);
  }
  // A comparison is bounded by the old token's exact length, not a new
  // capture limit. Classify changed identity/EOF from this same FD observation
  // before enforcing that bound, so ordinary native growth can reload rather
  // than being reported as an oversized GPD. Keep file/type/link errors above
  // and the full bounded byte comparison plus post-read stability check below.
  if (expected_info &&
      !SameSnapshotInfo(SnapshotInfo(before), *expected_info)) {
    return Error(std::errc::state_not_recoverable);
  }
  if (uint64_t(before.st_size) > maximum || uint64_t(before.st_size) > SIZE_MAX) {
    return Error(std::errc::file_too_large);
  }
  std::vector<uint8_t> bytes(size_t(before.st_size));
  size_t done = 0;
  while (done < bytes.size()) {
    const auto count = pread(fd, bytes.data() + done,
        std::min<size_t>(bytes.size() - done, 1024 * 1024), off_t(done));
    if (count < 0) {
      if (errno == EINTR) { continue; }
      return NativeError();
    }
    if (!count) { return Error(std::errc::io_error); }
    done += size_t(count);
  }
  if (fstat(fd, &after)) { return NativeError(); }
  if (!SameSnapshotInfo(SnapshotInfo(before), SnapshotInfo(after)) ||
#if XE_PLATFORM_MAC
      before.st_mtimespec.tv_sec != after.st_mtimespec.tv_sec ||
      before.st_mtimespec.tv_nsec != after.st_mtimespec.tv_nsec ||
      before.st_ctimespec.tv_sec != after.st_ctimespec.tv_sec ||
      before.st_ctimespec.tv_nsec != after.st_ctimespec.tv_nsec
#else
      before.st_mtim.tv_sec != after.st_mtim.tv_sec ||
      before.st_mtim.tv_nsec != after.st_mtim.tv_nsec ||
      before.st_ctim.tv_sec != after.st_ctim.tv_sec ||
      before.st_ctim.tv_nsec != after.st_ctim.tv_nsec
#endif
  ) { return Error(std::errc::state_not_recoverable); }
  output_info = SnapshotInfo(after);
  output = std::move(bytes);
  return {};
}
std::error_code SnapshotMatches(int fd,
    const HostStorageSnapshotToken& expected) {
  FileHandleInfo info;
  std::vector<uint8_t> bytes;
  auto error = SnapshotRead(fd, expected.bytes().size(), info, bytes,
                            &expected.info());
  if (error) { return error; }
  return SameSnapshotInfo(info, expected.info()) &&
      std::equal(bytes.begin(), bytes.end(), expected.bytes().begin(),
                 expected.bytes().end())
      ? std::error_code{} : Error(std::errc::state_not_recoverable);
}
#endif
}  // namespace
struct HostStorageRoot::SnapshotReplacement {
  const HostStorageSnapshotToken& expected;
  HostStorageSnapshotStage& stage;
  std::span<const uint8_t> intended;
  std::shared_ptr<const HostStorageSnapshotToken> recovery;
  // Prepared before rename so no allocation is required after publication.
  mutable std::shared_ptr<const HostStorageSnapshotToken> current;
  PrivateSnapshotPurpose purpose = PrivateSnapshotPurpose::kOrdinary;
};
std::shared_ptr<const HostStorageSnapshotToken> HostStorageRoot::CaptureSnapshot(
    const std::filesystem::path& relative, uint64_t maximum,
    std::error_code& error, bool ascii_folded_leaf) const {
  error.clear();
  if (!Relative(relative) || relative.filename().empty() ||
      (is_guest_private() && relative.has_parent_path())) {
    error = Error(std::errc::invalid_argument);
    return {};
  }
  auto guard = state_->space->Begin(path(), error, state_->directory_identity);
  if (!guard) { return {}; }
  if (HostStorageProtection::Guest(*this)) {
    error = CheckGuestAccess(relative);
    if (error) { return {}; }
  }
  error = Validate();
  if (!error && is_guest_private()) {
    error = HostStorageProtection::PrivateContents(*this, nullptr, true);
  }
  if (error) { return {}; }
#if XE_PLATFORM_WIN32
  error = Error(std::errc::operation_not_supported);
  return {};
#else
  std::string leaf;
  auto parent = Parent(state_->directory, relative, leaf, error);
  if (parent.value < 0) { return {}; }
  bool named_exists = false;
  if (ascii_folded_leaf) {
    std::string actual;
    error = SnapshotLeaf(parent.value, leaf, actual, named_exists);
    if (error) { return {}; }
    leaf = std::move(actual);
  }
  const auto actual_relative = relative.parent_path() / leaf;
  Fd file(openat(parent.value, leaf.c_str(),
                 O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK));
  if (file.value < 0) {
    error = NativeError();
    if (error != std::errc::no_such_file_or_directory) { return {}; }
    if (ascii_folded_leaf && named_exists) {
      error = Error(std::errc::state_not_recoverable); return {};
    }
    // A dangling symlink must not masquerade as an absent target.
    struct stat named;
    if (!fstatat(parent.value, leaf.c_str(), &named, AT_SYMLINK_NOFOLLOW)) {
      error = Error(std::errc::permission_denied);
      return {};
    }
    if (errno != ENOENT) { error = NativeError(); return {}; }
    error.clear();
    return std::shared_ptr<const HostStorageSnapshotToken>(
        new HostStorageSnapshotToken(shared_from_this(), actual_relative, false,
                                     {}, {}, ascii_folded_leaf));
  }
  error = ConfinedMount(file.value, state_->mount);
  if (error) { return {}; }
  FileHandleInfo info;
  std::vector<uint8_t> bytes;
  error = SnapshotRead(file.value, maximum, info, bytes);
  if (!error && guard->Reserved(path() / relative, info, ContentPermit())) {
    error = Error(std::errc::device_or_resource_busy);
  }
  if (!error) { error = HostStorageProtection::GuestFile(*this, info); }
  if (error) { return {}; }
  return std::shared_ptr<const HostStorageSnapshotToken>(
      new HostStorageSnapshotToken(shared_from_this(), actual_relative, true, info,
                                   std::move(bytes), ascii_folded_leaf));
#endif
}
std::unique_ptr<HostStorageSnapshotStage> HostStorageRoot::CreateSnapshotStage(
    const std::filesystem::path& relative, bool& created,
    std::error_code& error) const {
  auto file = OpenFileInternal(relative, FileAccess::kGenericAll, error,
                                true, false, false, true, &created);
  if (!file) { return {}; }
  return std::unique_ptr<HostStorageSnapshotStage>(new HostStorageSnapshotStage(
      shared_from_this(), relative, std::move(file)));
}
FileIoResult HostStorageSnapshotStage::WriteResult(size_t offset,
    std::span<const uint8_t> bytes) {
  std::lock_guard lock(mutex_);
  return sealed_ ? FileIoResult{Error(std::errc::operation_not_permitted), 0}
                 : file_->WriteResult(offset, bytes);
}
std::optional<FileHandleInfo> HostStorageSnapshotStage::QueryInfo(
    std::error_code& error) {
  std::lock_guard lock(mutex_);
  if (sealed_) { error = Error(std::errc::operation_not_permitted); return {}; }
  return file_->QueryInfo(error);
}
std::error_code HostStorageSnapshotStage::FlushResult() {
  std::lock_guard lock(mutex_);
  return sealed_ ? Error(std::errc::operation_not_permitted)
                 : file_->FlushResult();
}
HostStorageRoot::ReplaceResult HostStorageRoot::CompareAndPublishSnapshot(
    const HostStorageSnapshotToken& expected, HostStorageSnapshotStage& stage,
    std::span<const uint8_t> intended,
    std::shared_ptr<const HostStorageSnapshotToken> recovery) const {
  return CompareAndPublishSnapshotInternal(expected, stage, intended,
                                           std::move(recovery), PrivateSnapshotPurpose::kOrdinary);
}
bool HostStorageRoot::IsFlatSnapshotTarget() const {
  if (read_only()) { return false; }
  // ProtectFlat reuse returns an empty Subroot of its actual retained policy.
  // Follow only identical native directories in that ownership chain: a raw
  // reopen, descendant, sibling or other typed private policy gains no grant.
  const HostStorageRoot* protector = this;
  while (!protector->state_->flat_private) {
    const auto& state = *protector->state_;
    const auto* parent = state.parent.get();
    if (state.bootstrap_private || state.recovery_private || state.intent_private ||
        state.content_mutation_private || !parent ||
        state.path != parent->state_->path ||
        state.device != parent->state_->device ||
        state.inode != parent->state_->inode ||
        state.mount.id != parent->state_->mount.id) {
      return false;
    }
    protector = parent;
  }
  return true;
}
bool HostStorageRoot::IsMediaReferenceSnapshotTarget(
    const std::filesystem::path& relative) const {
  if (relative.has_parent_path() || !IsFlatSnapshotTarget()) { return false; }
  const auto leaf = relative.string();
  return leaf.size() == 73 && leaf.starts_with("media-") &&
      leaf.ends_with(".v1") &&
      std::all_of(leaf.begin() + 6, leaf.begin() + 70, [](unsigned char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
      });
}
std::error_code HostStorageRoot::EnsureMediaReferenceReserve() const {
  if (global_critical_region::is_held_by_current_thread() || read_only() ||
      is_guest_private()) {
    return Error(std::errc::operation_not_permitted);
  }
  auto error = Validate();
  if (error) {
    return error;
  }
  // Raise the SAME physical filesystem's existing floor. A path replacement
  // cannot substitute another native space or grant a different parent.
  const auto space =
      HostStorageSpace::ForPath(path(), kHostStorageReserve, error);
  if (!space || error) {
    return error;
  }
  if (space != state_->space) {
    return Error(std::errc::no_such_device);
  }
  return Validate();
}
std::vector<std::filesystem::path> HostStorageRoot::ListMediaReferenceFiles(
    std::error_code& error) const {
  error.clear();
  if (global_critical_region::is_held_by_current_thread()) {
    error = Error(std::errc::operation_not_permitted);
    return {};
  }
  if (!IsFlatSnapshotTarget()) {
    error = Error(std::errc::permission_denied);
    return {};
  }
  auto guard = Mutation(error);
  if (!guard) {
    return {};
  }
#if XE_PLATFORM_LINUX || XE_PLATFORM_MAC
  Fd fd(openat(state_->directory, ".",
               O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
  if (fd.value < 0) {
    error = NativeError();
    return {};
  }
  DIR* stream = fdopendir(fd.value);
  if (!stream) {
    error = NativeError();
    return {};
  }
  fd.Release();
  std::vector<std::filesystem::path> result;
  size_t count = 0;
  while (true) {
    errno = 0;
    auto* entry = readdir(stream);
    if (!entry) {
      if (errno) {
        error = NativeError();
      }
      break;
    }
    const std::string_view name(entry->d_name);
    if (name == "." || name == "..") {
      continue;
    }
    if (++count > 64) {
      error = Error(std::errc::file_too_large);
      break;
    }
    if (name == "owner.v1") {
      continue;
    }
    if (!IsMediaReferenceSnapshotTarget(entry->d_name)) {
      error = Error(std::errc::bad_message);
      break;
    }
    result.emplace_back(entry->d_name);
  }
  closedir(stream);
  std::sort(result.begin(), result.end());
  return error ? std::vector<std::filesystem::path>{} : result;
#else
  error = Error(std::errc::operation_not_supported);
  return {};
#endif
}
bool HostStorageRoot::IsCacheSnapshotTarget(
    const std::filesystem::path& relative) const {
  if (relative.has_parent_path() || !IsFlatSnapshotTarget()) { return false; }
  const auto leaf=relative.string();
  return leaf=="cache-table.v1" || leaf=="cache-format-0.v1" ||
      leaf=="cache-format-1.v1" || leaf=="cache-format-2.v1";
}
bool HostStorageRoot::IsPrivateSnapshotTarget(const std::filesystem::path& relative,
    PrivateSnapshotPurpose purpose) const {
  switch(purpose) {
    case PrivateSnapshotPurpose::kOrdinary:return !is_guest_private();
    case PrivateSnapshotPurpose::kMediaReference:return IsMediaReferenceSnapshotTarget(relative);
    case PrivateSnapshotPurpose::kCacheTable:return IsCacheSnapshotTarget(relative);
  }
  return false;
}
HostStorageRoot::ReplaceResult HostStorageRoot::CompareAndPublishSnapshotInternal(
    const HostStorageSnapshotToken& expected, HostStorageSnapshotStage& stage,
    std::span<const uint8_t> intended,
    std::shared_ptr<const HostStorageSnapshotToken> recovery,
    PrivateSnapshotPurpose purpose) const {
  if (purpose!=PrivateSnapshotPurpose::kOrdinary &&
      !IsPrivateSnapshotTarget(expected.relative(), purpose)) {
    return {Error(std::errc::permission_denied)};
  }
  std::lock_guard stage_lock(stage.mutex_);
  if (stage.sealed_ || expected.root_.get() != this ||
      stage.root_.get() != this ||
      (expected.exists() && (!recovery || !recovery->exists() ||
       !recovery->root()->is_recovery_private() ||
       recovery->bytes().size() != expected.bytes().size() ||
       !std::equal(expected.bytes().begin(), expected.bytes().end(),
                   recovery->bytes().begin()))) ||
      (!expected.exists() && recovery)) {
    return {Error(std::errc::invalid_argument)};
  }
  stage.sealed_ = true;
  SnapshotReplacement checked{expected, stage, intended, recovery, {},
                              purpose};
  const auto& backup_root = recovery ? *recovery->root() : *this;
  return ReplaceFile(stage.relative_, expected.relative_, backup_root,
                     recovery ? recovery->relative() : std::filesystem::path{},
                     false, &checked);
}
// Only the managed identity producer can acquire this file authority. Its
// marker is protected by an actual exclusive common lease and nonblocking
// native flock, not a private subtree that would deny the Content mount.
class HostStorageIntentLease final {
 public:
  ~HostStorageIntentLease() {
#if XE_PLATFORM_LINUX || XE_PLATFORM_MAC
    if (fd_ >= 0) { close(fd_); }
#endif
  }
 private:
  friend class HostStorageRoot;
  friend class HostStorageManagedMetadata;
  HostStorageIntentLease(std::shared_ptr<const HostStorageRoot> root, int fd)
      : root_(std::move(root)), fd_(fd) {}
  std::shared_ptr<const HostStorageRoot> root_;
  int fd_;
};

namespace {
bool BootstrapLeaf(std::string_view leaf) {
  return !leaf.empty() && leaf.size() <= 128 && leaf != "." && leaf != ".." &&
      std::all_of(leaf.begin(), leaf.end(), [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_';
      });
}
bool BootstrapRecordLeaf(std::string_view leaf) {
  const auto partial = leaf.find(".partial-");
  if (partial != std::string_view::npos) {
    if (!content_mutation_wire::ValidNonce(leaf.substr(partial + 9))) { return false; }
    leaf = leaf.substr(0, partial);
  }
  if (leaf == "owner.v1" || leaf == "cache-table.v1" || leaf == "bootstrap.pending.v1" ||
      leaf == "bootstrap.commit.v1" || leaf == "bootstrap-source.v1") { return true; }
  constexpr std::string_view component = "bootstrap.component-";
  if (leaf.size() == component.size() + 5 && leaf.starts_with(component) &&
      leaf.ends_with(".v1")) {
    const auto part = leaf.substr(component.size(), 2);
    return part[0] == '0' && ((part[1] >= '0' && part[1] <= '9') ||
        (part[1] >= 'A' && part[1] <= 'F'));
  }
  constexpr std::string_view preserve = "bootstrap-preserve-", link = "content-mutations-v1-";
  return (leaf.starts_with(preserve) && leaf.ends_with(".v1") &&
          content_mutation_wire::ValidNonce(leaf.substr(preserve.size(), leaf.size()-preserve.size()-3))) ||
      (leaf.starts_with(link) && leaf.ends_with(".owner") &&
          content_mutation_wire::ValidNonce(leaf.substr(link.size(), leaf.size()-link.size()-6)));
}
#if XE_PLATFORM_LINUX || XE_PLATFORM_MAC
std::error_code BootstrapRename(int parent, const char* from, const char* to, bool exchange = false) {
#if XE_PLATFORM_LINUX && defined(SYS_renameat2)
  return syscall(SYS_renameat2, parent, from, parent, to, exchange ? 2u : 1u) ? NativeError() : std::error_code{};
#elif XE_PLATFORM_MAC && defined(RENAME_EXCL) && defined(RENAME_SWAP)
  return renameatx_np(parent, from, parent, to, exchange ? RENAME_SWAP : RENAME_EXCL) ? NativeError() : std::error_code{};
#else
  return Error(std::errc::operation_not_supported);
#endif
}
std::error_code BootstrapWrite(int fd, std::span<const uint8_t> bytes,
    FileHandleInfo& info) {
  for (size_t done = 0; done != bytes.size();) {
    const auto count = pwrite(fd, bytes.data()+done, bytes.size()-done, off_t(done));
    if (count < 0 && errno == EINTR) { continue; }
    if (count <= 0) { return count ? NativeError() : Error(std::errc::io_error); }
    done += size_t(count);
  }
  std::vector<uint8_t> actual;
  auto error = SnapshotRead(fd, 65536, info, actual);
  if (!error && (actual.size() != bytes.size() || !std::equal(actual.begin(),actual.end(),bytes.begin()))) {
    error = Error(std::errc::state_not_recoverable);
  }
  return error ? error : SyncStorage(fd) ? NativeError() : std::error_code{};
}
#endif
}  // namespace

HostStorageBootstrapDirectory::HostStorageBootstrapDirectory(std::unique_ptr<State> state)
    : state_(std::move(state)) {}
HostStorageBootstrapDirectory::~HostStorageBootstrapDirectory() = default;
const std::shared_ptr<HostStorageRoot>& HostStorageBootstrapDirectory::root() const { return state_->root; }
const std::filesystem::path& HostStorageBootstrapDirectory::target() const { return state_->target; }
const std::filesystem::path& HostStorageBootstrapDirectory::staging() const { return state_->staging; }
uint64_t HostStorageBootstrapDirectory::native_directory() const { return state_->native_directory; }
std::error_code HostStorageRoot::CheckBootstrapBrowseDirectory(
    const HostStorageRoot& actual_source) const {
  if (global_critical_region::is_held_by_current_thread()) { return Error(std::errc::operation_not_permitted); }
  auto error=Validate();if (!error) { error=actual_source.Validate(); }if (error) { return error; }
#if XE_PLATFORM_LINUX || XE_PLATFORM_MAC
  if (state_->device!=actual_source.state_->device || state_->mount.id!=actual_source.state_->mount.id) {
    return Error(std::errc::cross_device_link);
  }
  if (state_->inode==actual_source.state_->inode && state_->directory_identity &&
      actual_source.state_->directory_identity && state_->directory_identity->SameEntry(
          {},*actual_source.state_->directory_identity,{})) { return {}; }
  Fd scan(openat(state_->directory,".",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC));
  if (scan.value<0) { return NativeError(); }
  DIR* stream=fdopendir(scan.value);if (!stream) { return NativeError(); }scan.Release();
  while (true) {
    errno=0;const auto* row=readdir(stream);
    if (!row) { if (errno) { error=NativeError(); }break; }
    if (!strcmp(row->d_name,".") || !strcmp(row->d_name,"..")) { continue; }
    error=Error(std::errc::directory_not_empty);break;
  }
  closedir(stream);return error;
#else
  return Error(std::errc::operation_not_supported);
#endif
}
std::error_code HostStorageRoot::CheckCacheBootstrapSource(
    const HostStorageRoot* binding,uint64_t marker_file,
    std::span<const std::shared_ptr<const HostStorageRoot>> preserved) const {
  if(global_critical_region::is_held_by_current_thread())return Error(std::errc::operation_not_permitted);
  auto error=Validate();if(!error && binding)error=binding->Validate();if(error)return error;
#if XE_PLATFORM_LINUX || XE_PLATFORM_MAC
  if(binding && (binding->path().parent_path()!=path() || binding->state_->device!=state_->device ||
      binding->state_->mount.id!=state_->mount.id))return Error(std::errc::permission_denied);
  for(const auto& root:preserved)if(!root || root->path().parent_path()!=path() ||
      root->state_->device!=state_->device || root->state_->mount.id!=state_->mount.id ||
      (error=root->Validate()))return error?error:Error(std::errc::permission_denied);
  Fd scan(openat(state_->directory,".",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC));
  if(scan.value<0)return NativeError();DIR* stream=fdopendir(scan.value);
  if(!stream)return NativeError();scan.Release();size_t count=0;
  while(true){
    errno=0;const auto* row=readdir(stream);if(!row){if(errno)error=NativeError();break;}
    if(!strcmp(row->d_name,".") || !strcmp(row->d_name,".."))continue;
    if(++count>2+preserved.size()){error=Error(std::errc::directory_not_empty);break;}
    struct stat named;
    if(fstatat(state_->directory,row->d_name,&named,AT_SYMLINK_NOFOLLOW)){error=NativeError();break;}
    const bool own_binding=binding && binding->path().filename()==row->d_name && S_ISDIR(named.st_mode) &&
        uint64_t(named.st_dev)==binding->state_->device && uint64_t(named.st_ino)==binding->state_->inode;
    const bool own_marker=marker_file && !strcmp(row->d_name,ManagedDeviceIdentity::kMarker) &&
        S_ISREG(named.st_mode) && named.st_nlink==1 && uint64_t(named.st_dev)==state_->device &&
        uint64_t(named.st_ino)==marker_file;
    const bool opaque=std::any_of(preserved.begin(),preserved.end(),[&](const auto& root){
      return root->path().filename()==row->d_name && S_ISDIR(named.st_mode) &&
          uint64_t(named.st_dev)==root->state_->device && uint64_t(named.st_ino)==root->state_->inode;
    });
    if(!own_binding && !own_marker && !opaque){error=Error(std::errc::directory_not_empty);break;}
  }
  closedir(stream);return error;
#else
  return Error(std::errc::operation_not_supported);
#endif
}
std::filesystem::path HostStorageRoot::MetadataBootstrapTemporary(std::string_view nonce) {
  return content_mutation_wire::ValidNonce(nonce) ?
      std::filesystem::path(".xenia-bootstrap-" + std::string(nonce) + ".tmp") : std::filesystem::path{};
}
std::filesystem::path HostStorageRoot::DirectoryBootstrapTemporary(
    const std::filesystem::path& target, std::string_view nonce) {
  return !target.has_parent_path() && BootstrapLeaf(target.string()) &&
      content_mutation_wire::ValidNonce(nonce) ?
      std::filesystem::path(".xenia-bootstrap-" + std::string(nonce) + ".dir") : std::filesystem::path{};
}
std::shared_ptr<HostStorageRoot> HostStorageRoot::ProtectBootstrap(std::error_code& error) const {
  return HostStorageProtection::Protect(*this, error, 4);
}
std::shared_ptr<HostStorageRoot> HostStorageRoot::RebaseBootstrapRoot(
    const HostStorageRoot& root, const std::filesystem::path& leaf, std::error_code& error) const {
  if (global_critical_region::is_held_by_current_thread()) { error=Error(std::errc::operation_not_permitted);return {}; }
  error = Validate();
  if (error || !BootstrapLeaf(leaf.string())) { if (!error) { error=Error(std::errc::invalid_argument); } return {}; }
#if XE_PLATFORM_LINUX || XE_PLATFORM_MAC
  struct stat named, retained;
  if (fstat(root.state_->directory,&retained) ||
      fstatat(state_->directory,leaf.c_str(),&named,AT_SYMLINK_NOFOLLOW)) { error=NativeError(); return {}; }
  if (!SameNode(named,retained) || !S_ISDIR(named.st_mode) || named.st_uid!=geteuid() ||
      (named.st_mode&077) || named.st_dev!=state_->device) { error=Error(std::errc::state_not_recoverable); return {}; }
  error=ConfinedMount(root.state_->directory,state_->mount); if (error) { return {}; }
  auto next=std::make_unique<State>(); next->path=path()/leaf;
  next->read_only=false; next->space=state_->space; next->parent=shared_from_this();
  next->device=retained.st_dev; next->inode=retained.st_ino; next->mount=state_->mount;
  next->directory=fcntl(root.state_->directory,F_DUPFD_CLOEXEC,0);
  if (next->directory<0) { error=NativeError(); return {}; }
  next->directory_identity=HostStorageDirectoryIdentity::Capture(next->path,error);
  if (!next->directory_identity || !next->directory_identity->MatchesNativeDirectory(next->device,next->inode)) {
    if (!error) { error=Error(std::errc::state_not_recoverable); } return {};
  }
  next->bootstrap_owner=root.state_->bootstrap_owner;
  next->bootstrap_group=root.state_->bootstrap_group; next->format_eligible=false;
  auto result=std::shared_ptr<HostStorageRoot>(new HostStorageRoot(std::move(next)));
  error=result->Validate(); return error ? nullptr : result;
#else
  error=Error(std::errc::operation_not_supported); return {};
#endif
}
std::unique_ptr<HostStorageBootstrapDirectory> HostStorageRoot::BeginBootstrapDirectory(
    const std::filesystem::path& target, std::string_view nonce, std::error_code& error,
    std::shared_ptr<const HostStorageIntentLease> lease) const {
  if (global_critical_region::is_held_by_current_thread()) { error=Error(std::errc::operation_not_permitted);return {}; }
  error=Validate();
  if (error || read_only() || is_guest_private() || target.has_parent_path() ||
      !BootstrapLeaf(target.string()) || !content_mutation_wire::ValidNonce(nonce)) {
    if (!error) { error=Error(std::errc::permission_denied); } return {};
  }
  // Preserve native capability proof before creating even a private stage.
  static_cast<void>(IntentRootIdentity(error));if (error) { return {}; }
  if (lease ? !IntentLeaseMatches(*lease) : false) { error=Error(std::errc::permission_denied); return {}; }
  if (!lease) { lease=AcquireIntentLease(error,true); }
  if (!lease) { return {}; }
  auto state=std::make_unique<HostStorageBootstrapDirectory::State>();
  state->parent=shared_from_this(); state->lease=std::move(lease); state->target=target;
  state->staging=DirectoryBootstrapTemporary(target,nonce);
#if XE_PLATFORM_LINUX || XE_PLATFORM_MAC
  {
    auto guard=Mutation(error); if (!guard) { return {}; }
    struct stat existing;
    if (!fstatat(state_->directory,target.c_str(),&existing,AT_SYMLINK_NOFOLLOW) || errno!=ENOENT) {
      error=Error(std::errc::file_exists); return {};
    }
    error=guard->Admit(kMetadataAllowance);
    if (!error && bootstrap_test_boundary_.load()==4) {
      bootstrap_test_boundary_.store(0);error=Error(std::errc::no_space_on_device);
    }
    if (error) { return {}; }
    if (mkdirat(state_->directory,state->staging.c_str(),0700)) { error=NativeError(); return {}; }
  }
  state->root=Subroot(state->staging,false,error); if (!state->root) { return {}; }
  state->root->state_->bootstrap_owner=state->owner;
  state->root->state_->bootstrap_group=HostStorageProtection::ClaimBootstrap(
      *this,*state->root,state->target,state->staging,state->owner,error);
  if (!state->root->state_->bootstrap_group) { return {}; }
  state->native_directory=state->root->state_->inode;
  auto protection=state->root->ProtectBootstrap(error); if (!protection) { return {}; }
  state->protections.push_back(std::move(protection));
  auto result=std::unique_ptr<HostStorageBootstrapDirectory>(new HostStorageBootstrapDirectory(std::move(state)));
  if (bootstrap_test_boundary_.exchange(0)==1) { error=Error(std::errc::operation_canceled);return result; }
  if (SyncStorage(state_->directory)) { error=NativeError(); }
  return result;
#else
  error=Error(std::errc::operation_not_supported); return {};
#endif
}
std::unique_ptr<HostStorageBootstrapDirectory> HostStorageRoot::ResumeBootstrapDirectory(
    const std::filesystem::path& target, const std::filesystem::path& staging,
    uint64_t directory, std::error_code& error,
    std::shared_ptr<const HostStorageIntentLease> lease) const {
  if (global_critical_region::is_held_by_current_thread()) { error=Error(std::errc::operation_not_permitted);return {}; }
  error=Validate(); const auto text=staging.string();
  constexpr std::string_view prefix=".xenia-bootstrap-";
  if (error || !directory || read_only() || is_guest_private() || target.has_parent_path() ||
      staging.has_parent_path() || !BootstrapLeaf(target.string()) ||
      text.size()!=prefix.size()+32+4 || !text.starts_with(prefix) || !text.ends_with(".dir") ||
      !content_mutation_wire::ValidNonce(std::string_view(text).substr(prefix.size(),32))) {
    if (!error) { error=Error(std::errc::permission_denied); } return {};
  }
  if (lease && !IntentLeaseMatches(*lease)) { error=Error(std::errc::permission_denied); return {}; }
  if (!lease) { lease=AcquireIntentLease(error,true); }
  if (!lease) { return {}; }
#if XE_PLATFORM_LINUX || XE_PLATFORM_MAC
  auto state=std::make_unique<HostStorageBootstrapDirectory::State>();
  state->parent=shared_from_this();state->lease=std::move(lease);state->target=target;state->staging=staging;
  {
    auto guard=Mutation(error);if (!guard) { return {}; }
    struct stat a{},b{};const bool has_stage=!fstatat(state_->directory,staging.c_str(),&a,AT_SYMLINK_NOFOLLOW);
    if (!has_stage && errno!=ENOENT) { error=NativeError(); return {}; }
    const bool has_target=!fstatat(state_->directory,target.c_str(),&b,AT_SYMLINK_NOFOLLOW);
    if (!has_target && errno!=ENOENT) { error=NativeError(); return {}; }
    const auto& actual=has_stage?a:b;
    if (has_stage==has_target || !S_ISDIR(actual.st_mode) || actual.st_dev!=state_->device ||
        uint64_t(actual.st_ino)!=directory || actual.st_uid!=geteuid() || (actual.st_mode&077)) {
      error=Error(std::errc::state_not_recoverable);return {};
    }
    state->canonical=has_target;
  }
  state->root=Subroot(state->canonical?target:staging,false,error);if (!state->root) { return {}; }
  if (uint64_t(state->root->state_->inode)!=directory) { error=Error(std::errc::state_not_recoverable); return {}; }
  state->native_directory=directory;state->root->state_->bootstrap_owner=state->owner;
  state->root->state_->bootstrap_group=HostStorageProtection::ClaimBootstrap(
      *this,*state->root,state->target,state->staging,state->owner,error);
  if (!state->root->state_->bootstrap_group) { return {}; }
  auto protection=state->root->ProtectBootstrap(error);if (!protection) { return {}; }
  state->protections.push_back(std::move(protection));
  return std::unique_ptr<HostStorageBootstrapDirectory>(new HostStorageBootstrapDirectory(std::move(state)));
#else
  error=Error(std::errc::operation_not_supported);return {};
#endif
}
std::unique_ptr<HostStorageBootstrapDirectory> HostStorageRoot::ResumeRetainedBootstrapDirectory(
    const std::filesystem::path& target, const HostStorageRoot& canonical,
    std::error_code& error,
    std::shared_ptr<const HostStorageIntentLease> same_parent_lease) const {
  error.clear();
  if (global_critical_region::is_held_by_current_thread()) { error=Error(std::errc::operation_not_permitted);return {}; }
  error=Validate();if (!error) { error=canonical.Validate(); }
  if (error || target.has_parent_path() || !BootstrapLeaf(target.string()) ||
      canonical.path()!=path()/target || canonical.read_only() || canonical.is_guest_private()) {
    if (!error) { error=Error(std::errc::permission_denied); }return {};
  }
  if (same_parent_lease && !IntentLeaseMatches(*same_parent_lease)) {
    error=Error(std::errc::permission_denied);return {};
  }
  const auto stage=HostStorageProtection::RetainedBootstrapStage(*this,canonical,target,error);
  if (error || !stage) { return {}; }  // no registered receipt means no adoption
  auto lease=std::move(same_parent_lease);
  if (!lease) {lease=AcquireIntentLease(error,true);}
  if (!lease) { return {}; }
#if XE_PLATFORM_LINUX || XE_PLATFORM_MAC
  return ResumeBootstrapDirectory(target,*stage,canonical.state_->inode,error,std::move(lease));
#else
  error=Error(std::errc::operation_not_supported);return {};
#endif
}
HostStorageBootstrapPublication HostStorageRoot::PublishBootstrapDirectory(
    HostStorageBootstrapDirectory& capsule) const {
  HostStorageBootstrapPublication result;
  if (global_critical_region::is_held_by_current_thread()) { result.error=Error(std::errc::operation_not_permitted);return result; }
  auto& state=*capsule.state_;
  if (state.parent.get()!=this || state.public_admitted || state.private_settled || !state.root ||
      !state.lease || !IntentLeaseMatches(*state.lease)) {
    result.error=Error(std::errc::permission_denied);return result;
  }
#if XE_PLATFORM_LINUX || XE_PLATFORM_MAC
  auto guard=Mutation(result.error);if (!guard) { return result; }
  result.error=guard->Admit(kMetadataAllowance);if (result.error) { return result; }
  const auto leaf=state.canonical?state.target:state.staging;
  FlushTreeNode tree;size_t remaining=512;
  result.error=PrepareFlushTree(state_->directory,leaf.string(),state_->device,state_->mount,tree,remaining);
  if (!result.error && uint64_t(tree.identity.st_ino)!=state.native_directory) { result.error=Error(std::errc::state_not_recoverable); }
  if (!result.error) { result.error=HostStorageProtection::BootstrapContents(*state.root); }
  if (!result.error && FlushTreeReserved(*guard,path()/leaf,tree,ContentPermit())) { result.error=Error(std::errc::device_or_resource_busy); }
  if (!result.error) { result.error=RevalidateFlushTree(state_->directory,tree,state_->mount); }
  if (!result.error) { result.error=SyncFlushTree(tree,false); }
  if (!result.error) { result.error=SyncFlushTree(tree,true); }
  if (!result.error) { result.error=RevalidateFlushTree(state_->directory,tree,state_->mount); }
  if (result.error) { return result; }
  if (!state.canonical) {
    result.error=BootstrapRename(state_->directory,state.staging.c_str(),state.target.c_str());
    if (result.error) { return result; }
    state.canonical=true;result.published=true;
  }
  // Rebase only the native capability, never reopen its authority by pathname.
  guard.reset();
  if (state.root->path()!=path()/state.target) {
    auto canonical=RebaseBootstrapRoot(*state.root,state.target,result.error);
    if (!canonical) { return result; }
    auto protection=canonical->ProtectBootstrap(result.error);
    if (!protection) { return result; }
    state.protections.push_back(std::move(protection));state.root=std::move(canonical);
  }
  result.canonical_root=state.root;
  if (bootstrap_test_boundary_.exchange(0)==3) { result.error=Error(std::errc::operation_canceled);return result; }
  result.error=state.root->Validate();
  if (!result.error && SyncStorage(state_->directory)) { result.error=NativeError(); }
  state.durable=result.durable=!result.error;
  return result;
#else
  result.error=Error(std::errc::operation_not_supported);return result;
#endif
}
std::shared_ptr<HostStorageRoot> HostStorageRoot::ProtectMediaReferenceDirectory(
    HostStorageBootstrapDirectory& capsule, std::error_code& error) const {
  error = EnsureMediaReferenceReserve();
  if (error) { return {}; }
  auto& state = *capsule.state_;
  if (state.parent.get() != this || !state.canonical || !state.durable ||
      state.public_admitted || state.private_settled || !state.root ||
      !state.lease || !IntentLeaseMatches(*state.lease) ||
      state.target != ".xenia-media-v1" || state.root->path() != path()/state.target ||
      state.root->state_->bootstrap_owner.lock() != state.owner) {
    error = Error(std::errc::permission_denied); return {};
  }
  error = state.root->Validate();
  if (error) { return {}; }
  const auto receipt = state.root->state_->bootstrap_group;
  if (!receipt || !HostStorageProtection::BootstrapBindingMatches(
          *receipt, *this, *state.root, state.target, state.staging) ||
      state.native_directory != receipt->directory) {
    error = Error(std::errc::permission_denied); return {};
  }
  constexpr std::string_view owner = "XENIA-MEDIA-REFERENCES-1\n";
  const auto marker = state.root->CaptureSnapshot("owner.v1", 128, error);
  if (!marker || !marker->exists() || marker->bytes().size() != owner.size() ||
      !std::equal(marker->bytes().begin(), marker->bytes().end(), owner.begin())) {
    if (!error) { error = Error(std::errc::bad_message); }
    return {};
  }
  const auto observed = state.root->ObserveIntentSnapshot(*marker);
  error = observed.error;
  if (!error && !observed.durable) { error = Error(std::errc::io_error); }
  if (error) { return {}; }
  // Protect publishes the actual flat policy before retiring only this exact
  // registered bootstrap family. Failure retains the capsule and protections.
  auto view = HostStorageProtection::Protect(*state.root, error, 0, receipt);
  if (!view) { return {}; }
  state.private_settled = true;
  state.protections.clear();
  // The caller retains the parent EX lease through its final durable check.
  return view;
}
std::shared_ptr<HostStorageRoot> HostStorageRoot::AdmitBootstrapPublicDirectory(
    HostStorageBootstrapDirectory& capsule,std::error_code& error,
    const HostStorageRoot* source_projection) const {
  error.clear();
  if (global_critical_region::is_held_by_current_thread()) { error=Error(std::errc::operation_not_permitted);return {}; }
  auto& state=*capsule.state_;
  if (state.parent.get()!=this || !state.canonical || !state.durable || state.public_admitted || state.private_settled ||
      !state.root || !state.lease || !IntentLeaseMatches(*state.lease)) {
    error=Error(std::errc::permission_denied);return {};
  }
  error=state.root->Validate();if (error) { return {}; }
  std::shared_ptr<HostStorageRoot> view;
  const auto bound_projection=state.public_projection.lock();
  if (source_projection) {
    if (!bound_projection || source_projection->Projection()!=bound_projection ||
        bound_projection->state_->source.get()!=source_projection) {
      error=Error(std::errc::permission_denied);return {};
    }
    // The construction root inherits Content's namespace projection. Prepare
    // an independent public scope, while retaining its actual native/lifetime
    // dependency and the exact bound projection; no path-root reminting.
    view=state.public_view;
    if (!view) {
      view=state.root->IndependentNamespaceView(error);if (!view) { return {}; }
      error=view->AttachPublicProjection(*source_projection,&capsule);if (error) { return {}; }
      state.public_view=view;
    } else if (view->Projection()!=bound_projection || !view->PublicProjectionScope()) {
      error=Error(std::errc::permission_denied);return {};
    }
  } else {
    if (bound_projection || state.root->has_guest_projection()) {
      error=Error(std::errc::permission_denied);return {};
    }
    view=state.root->Subroot({},false,error);if (!view) { return {}; }
  }
  if (source_projection) {
    // Projection strongly pins its unadmitted public scope. Mount ownership
    // belongs to a separate child: putting it on the scope itself creates
    // projection -> scope -> guest_mount -> projection and lets retiring the
    // published guest poison the scope's immutable native authority.
    view=view->Subroot({},false,error);if (!view) { return {}; }
  }
  auto admission=HostStorageProtection::Admit(
      std::vector<GuestHostBacking>{view->GuestBacking()},error,state.root->state_->bootstrap_group,state.root.get());
  if (!admission) { return {}; }
  view->state_->guest_mount=std::move(admission);state.public_admitted=true;
  // The metadata exchange is complete; release retired protectors outside it.
  state.protections.clear();return view;
}
std::vector<std::filesystem::path> HostStorageRoot::ListBootstrapRecords(std::error_code& error) const {
  if (global_critical_region::is_held_by_current_thread()) { error=Error(std::errc::operation_not_permitted);return {}; }
#if XE_PLATFORM_LINUX || XE_PLATFORM_MAC
  if (!state_->space->NativeAuthorityFor(state_->device)) { error=Error(std::errc::operation_not_supported);return {}; }
#endif
  auto guard=Mutation(error);if (!guard) { return {}; }
#if XE_PLATFORM_LINUX || XE_PLATFORM_MAC
  Fd scan(openat(state_->directory,".",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC));
  if (scan.value<0) { error=NativeError();return {}; }
  DIR* stream=fdopendir(scan.value);if (!stream) { error=NativeError();return {}; }scan.Release();
  std::vector<std::filesystem::path> out;
  while (true) {
    errno=0;const auto* row=readdir(stream);if (!row) { if (errno) { error=NativeError(); }break; }
    const std::string_view leaf=row->d_name;if (leaf=="." || leaf=="..") { continue; }
    if (out.size()==64 || !BootstrapRecordLeaf(leaf)) { error=Error(std::errc::bad_message);break; }
    Fd file(openat(state_->directory,row->d_name,O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC));
    struct stat named,opened;
    if (file.value<0 || fstat(file.value,&opened) || fstatat(state_->directory,row->d_name,&named,AT_SYMLINK_NOFOLLOW)) { error=NativeError();break; }
    if (!SameNode(named,opened) || !S_ISREG(opened.st_mode) || opened.st_nlink!=1 ||
        opened.st_uid!=geteuid() || (opened.st_mode&077) || opened.st_dev!=state_->device || opened.st_size<0 || opened.st_size>65536) {
      error=Error(std::errc::permission_denied);break;
    }
    error=ConfinedMount(file.value,state_->mount);if (error) { break; }out.emplace_back(leaf);
  }
  closedir(stream);std::sort(out.begin(),out.end());return error?std::vector<std::filesystem::path>{}:out;
#else
  error=Error(std::errc::operation_not_supported);return {};
#endif
}
HostStorageBootstrapRecordPublication HostStorageRoot::PublishBootstrapRecord(
    const std::filesystem::path& leaf,std::span<const uint8_t> bytes,std::string_view nonce) const {
  HostStorageBootstrapRecordPublication result;
  if (global_critical_region::is_held_by_current_thread()) { result.error=Error(std::errc::operation_not_permitted);return result; }
  if (read_only() || leaf.has_parent_path() || !BootstrapRecordLeaf(leaf.string()) ||
      leaf.string().find(".partial-")!=std::string::npos || bytes.empty() || bytes.size()>65536 ||
      !content_mutation_wire::ValidNonce(nonce)) { result.error=Error(std::errc::permission_denied);return result; }
#if XE_PLATFORM_LINUX || XE_PLATFORM_MAC
  if (!state_->space->NativeAuthorityFor(state_->device)) { result.error=Error(std::errc::operation_not_supported);return result; }
#endif
  const auto old=CaptureSnapshot(leaf,65536,result.error);if (!old) { return result; }
  if (old->exists()) {
    if (old->bytes().size()!=bytes.size() || !std::equal(bytes.begin(),bytes.end(),old->bytes().begin())) {
      result.error=Error(std::errc::state_not_recoverable);return result;
    }
#if XE_PLATFORM_LINUX || XE_PLATFORM_MAC
    auto guard=Mutation(result.error);if (!guard) { return result; }
    Fd file(openat(state_->directory,leaf.c_str(),O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC));
    struct stat opened,named;
    if (file.value<0 || fstat(file.value,&opened) ||
        fstatat(state_->directory,leaf.c_str(),&named,AT_SYMLINK_NOFOLLOW)) {
      result.error=NativeError();return result;
    }
    if (!SameNode(opened,named) || opened.st_uid!=geteuid() || (opened.st_mode&077)) {
      result.error=Error(std::errc::permission_denied);return result;
    }
    result.error=ConfinedMount(file.value,state_->mount);
    if (!result.error) { result.error=SnapshotMatches(file.value,*old); }
    if (!result.error && guard->Conflicts(path()/leaf,false,old->info().filesystem_id,old->info().file_id,ContentPermit())) {
      result.error=Error(std::errc::device_or_resource_busy);
    }
    if (!result.error && (SyncStorage(file.value) || SyncStorage(state_->directory))) { result.error=NativeError(); }
    result.durable=!result.error;return result;
#else
    result.error=Error(std::errc::operation_not_supported);return result;
#endif
  }
#if XE_PLATFORM_LINUX || XE_PLATFORM_MAC
  auto guard=Mutation(result.error);if (!guard) { return result; }
  result.error=guard->Admit(bytes.size()+kMetadataAllowance);
  if (!result.error && bootstrap_test_boundary_.load()==4) {
    bootstrap_test_boundary_.store(0);result.error=Error(std::errc::no_space_on_device);
  }
  if (result.error) { return result; }
  // Count actual slots before creating a control temporary. A failed write
  // preserves that slot; retries cannot grow an unbounded private inventory.
  Fd scan(openat(state_->directory,".",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC));
  if (scan.value<0) { result.error=NativeError();return result; }
  DIR* entries=fdopendir(scan.value);if (!entries) { result.error=NativeError();return result; }scan.Release();
  size_t slots=0;
  while (true) {
    errno=0;const auto* row=readdir(entries);
    if (!row) { if (errno) { result.error=NativeError(); }break; }
    if (!strcmp(row->d_name,".") || !strcmp(row->d_name,"..")) { continue; }
    if (++slots>=64) { result.error=Error(std::errc::no_space_on_device);break; }
  }
  closedir(entries);if (result.error) { return result; }
  const auto temporary=leaf.string()+".partial-"+std::string(nonce);
  Fd file(openat(state_->directory,temporary.c_str(),O_RDWR|O_CREAT|O_EXCL|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC,0600));
  if (file.value<0) { result.error=NativeError();return result; }
  const unsigned stop=bootstrap_test_boundary_.exchange(0);
  const auto payload=stop==1?bytes.first(std::min<size_t>(17,bytes.size())):bytes;
  FileHandleInfo info;result.error=BootstrapWrite(file.value,payload,info);if (result.error) { return result; }
  if (stop==1 || stop==2) { result.error=Error(std::errc::operation_canceled);return result; }
  struct stat named;if (fstatat(state_->directory,temporary.c_str(),&named,AT_SYMLINK_NOFOLLOW) ||
      !SameSnapshotInfo(SnapshotInfo(named),info)) { result.error=Error(std::errc::state_not_recoverable);return result; }
  result.error=BootstrapRename(state_->directory,temporary.c_str(),leaf.c_str());if (result.error) { return result; }
  result.published=true;
  if (fstatat(state_->directory,leaf.c_str(),&named,AT_SYMLINK_NOFOLLOW) || !SameSnapshotInfo(SnapshotInfo(named),info)) {
    result.error=Error(std::errc::state_not_recoverable);return result;
  }
  if (stop==3) { result.error=Error(std::errc::operation_canceled);return result; }
  if (SyncStorage(state_->directory)) { result.error=NativeError();return result; }
  result.durable=true;return result;
#else
  result.error=Error(std::errc::operation_not_supported);return result;
#endif
}
HostStorageManagedMetadata::HostStorageManagedMetadata(
    std::shared_ptr<const HostStorageRoot> root, std::filesystem::path leaf,
    int fd, FileHandleInfo info,
    std::shared_ptr<const HostStorageFileLease> lease, bool created,
    std::vector<uint8_t> bytes, std::shared_ptr<HostStorageRoot> guest_view)
    : root_(std::move(root)),
      leaf_(std::move(leaf)),
      fd_(fd),
      info_(info),
      lease_(std::move(lease)),
      created_(created),
      bytes_(std::move(bytes)),
      guest_view_(std::move(guest_view)) {
  root_->RetainHandle();
}
HostStorageManagedMetadata::~HostStorageManagedMetadata() { Close(); }
void HostStorageManagedMetadata::Close() {
  if (fd_ < 0) {
    return;
  }
#if !XE_PLATFORM_WIN32
  close(fd_);
#endif
  fd_ = -1;
  lease_.reset();
  guest_view_.reset();
  root_->ReleaseHandle();
}
std::shared_ptr<HostStorageManagedMetadata>
HostStorageRoot::OpenManagedMetadata(const std::filesystem::path& leaf,
    std::error_code& error, ManagedMetadataOpen mode,
    std::span<const uint8_t> complete_initial, std::string_view initializer_nonce,
    std::shared_ptr<const HostStorageIntentLease> same_source_lease) const {
  error.clear();
  if (global_critical_region::is_held_by_current_thread()) { error=Error(std::errc::operation_not_permitted);return {}; }
#if !XE_PLATFORM_LINUX && !XE_PLATFORM_MAC
  error = Error(std::errc::operation_not_supported); return {};
#else
  if (leaf != ".xenia-managed-source-v1" || is_guest_private() || read_only() ||
      !state_->space->NativeAuthorityFor(state_->device)) {
    error = Error(std::errc::permission_denied); return {};
  }
  // GuestView is deliberately deferred, so obtain the native namespace proof
  // independently before any marker creation (including Darwin authorities).
  static_cast<void>(IntentRootIdentity(error));if (error) { return {}; }
  const bool pending = mode == ManagedMetadataOpen::kCreatePending ||
                       mode == ManagedMetadataOpen::kExistingPending;
  const bool create = mode == ManagedMetadataOpen::kCreate ||
                      mode == ManagedMetadataOpen::kCreatePending;
  Fd directory_admission;
  if (pending) {
    if (!same_source_lease || !IntentLeaseMatches(*same_source_lease)) {
      error = Error(std::errc::permission_denied); return {};
    }
  } else {
    // Hold shared directory admission until the exact canonical file flock is
    // established. Migration holds the independent exclusive description.
    directory_admission.value = openat(state_->directory, ".",
        O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (directory_admission.value < 0) { error = NativeError(); return {}; }
    if (flock(directory_admission.value, LOCK_SH | LOCK_NB)) {
      error = errno == EWOULDBLOCK ? Error(std::errc::device_or_resource_busy) : NativeError();
      return {};
    }
    error = ConfinedMount(directory_admission.value, state_->mount);
    if (error) { return {}; }
  }
  auto guard = Mutation(error); if (!guard) { return {}; }
  if (mode == ManagedMetadataOpen::kCreate) {
    // Deferring actual guest admission must not let a refused ordinary source
    // leave its marker/initializer bytes over a live protected private tree.
    // Pending bootstrap creation remains governed by its exact source EX lease.
    error = HostStorageProtection::CheckManagedSourceCreation(*this);
    if (error) { return {}; }
  }
  std::string actual; bool exists = false;
  error = SnapshotLeaf(state_->directory, leaf.string(), actual, exists);
  if (!error && actual != leaf.string()) { error = Error(std::errc::bad_message); }
  if (error) { return {}; }
  Fd owned(openat(state_->directory, leaf.c_str(),
      O_RDWR | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC));
  bool created = false;
  std::string temporary;
  if (owned.value < 0 && errno == ENOENT && !exists && create) {
    if (complete_initial.empty() || complete_initial.size() > 65536) {
      error = Error(std::errc::invalid_argument); return {};
    }
    std::string nonce(initializer_nonce);
    if (nonce.empty()) {
      std::random_device random; constexpr char hex[] = "0123456789abcdef";
      for (unsigned i = 0; i != 32; ++i) { nonce += hex[random() & 15]; }
    }
    if (!content_mutation_wire::ValidNonce(nonce)) {
      error = Error(std::errc::invalid_argument); return {};
    }
    temporary = MetadataBootstrapTemporary(nonce).string();
    error = guard->Admit(complete_initial.size() + kMetadataAllowance);
    if (!error && bootstrap_test_boundary_.load()==4) {
      bootstrap_test_boundary_.store(0);error=Error(std::errc::no_space_on_device);
    }
    if (error) { return {}; }
    owned.value = openat(state_->directory, temporary.c_str(),
        O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC, 0600);
    if (owned.value < 0) { error = NativeError(); return {}; }
    created = true;
    if (bootstrap_test_boundary_.load() == 1) {
      bootstrap_test_boundary_.store(0); error=Error(std::errc::operation_canceled); return {};
    }
  }
  if (owned.value < 0) { error = NativeError(); return {}; }
  struct stat native, named;
  if (fstat(owned.value, &native)) { error = NativeError(); return {}; }
  if (!S_ISREG(native.st_mode) || native.st_nlink != 1 ||
      native.st_uid != geteuid() || (native.st_mode & 077)) {
    error = Error(std::errc::permission_denied); return {};
  }
  error = ConfinedMount(owned.value, state_->mount);
  if (!error && guard->Conflicts(path() / leaf, false, native.st_dev, native.st_ino)) {
    error = Error(std::errc::device_or_resource_busy);
  }
  if (error) { return {}; }
  if (flock(owned.value, LOCK_EX | LOCK_NB)) {
    error = errno == EWOULDBLOCK ? Error(std::errc::device_or_resource_busy) : NativeError();
    return {};
  }
  if (created) {
    size_t written = 0;
    while (written != complete_initial.size()) {
      const auto count = pwrite(owned.value, complete_initial.data() + written,
          complete_initial.size() - written, off_t(written));
      if (count < 0 && errno == EINTR) { continue; }
      if (count <= 0) { error = count ? NativeError() : Error(std::errc::io_error); return {}; }
      written += size_t(count);
    }
  }
  FileHandleInfo info; std::vector<uint8_t> bytes;
  error = SnapshotRead(owned.value, 65536, info, bytes);
  if (!error && created && (bytes.size() != complete_initial.size() ||
      !std::equal(bytes.begin(), bytes.end(), complete_initial.begin()))) {
    error = Error(std::errc::state_not_recoverable);
  }
  const auto& named_leaf = created ? temporary : leaf.string();
  if (!error && (fstatat(state_->directory, named_leaf.c_str(), &named, AT_SYMLINK_NOFOLLOW) ||
      !SameSnapshotInfo(SnapshotInfo(named), info))) { error = Error(std::errc::state_not_recoverable); }
  if (!error && SyncStorage(owned.value)) { error = NativeError(); }
  if (error) { return {}; }
  Fd pin(fcntl(owned.value, F_DUPFD_CLOEXEC, 0));
  if (pin.value < 0) { error = NativeError(); return {}; }
  auto lease = guard->TrackOpen(path() / leaf, info, true);
  if (!lease) { error = Error(std::errc::device_or_resource_busy); return {}; }
  lease->identity_pin = std::move(pin);
  if (created && bootstrap_test_boundary_.load() == 2) {
    bootstrap_test_boundary_.store(0);error=Error(std::errc::operation_canceled);return {};
  }
  if (created) {
#if XE_PLATFORM_LINUX && defined(SYS_renameat2)
    if (syscall(SYS_renameat2, state_->directory, temporary.c_str(),
        state_->directory, leaf.c_str(), 1u)) { error = NativeError(); return {}; }
#elif XE_PLATFORM_MAC && defined(RENAME_EXCL)
    if (renameatx_np(state_->directory, temporary.c_str(), state_->directory,
        leaf.c_str(), RENAME_EXCL)) { error = NativeError(); return {}; }
#else
    error = Error(std::errc::operation_not_supported); return {};
#endif
  }
  auto result = std::shared_ptr<HostStorageManagedMetadata>(
      new HostStorageManagedMetadata(shared_from_this(), leaf, owned.Release(),
          info, std::move(lease), created, std::move(bytes), {}));
  if (fstatat(state_->directory, leaf.c_str(), &named, AT_SYMLINK_NOFOLLOW) ||
      !SameSnapshotInfo(SnapshotInfo(named), info)) {
    error = Error(std::errc::state_not_recoverable); return result;
  }
  if (created && bootstrap_test_boundary_.exchange(0) == 3) {
    error=Error(std::errc::operation_canceled); return result;
  }
  if (SyncStorage(state_->directory)) { error = NativeError(); }
  // No mode admits GuestView before MDI's complete wire/binding validation.
  return result;
#endif
}
std::error_code HostStorageRoot::AdmitManagedMetadata(HostStorageManagedMetadata& metadata) const {
  if (global_critical_region::is_held_by_current_thread()) { return Error(std::errc::operation_not_permitted); }
  if (metadata.root_.get() != this || metadata.fd_ < 0) { return Error(std::errc::permission_denied); }
  auto error = metadata.Check();
  if (error || metadata.guest_view_) { return error; }
  auto view = GuestView(error);
  if (!view) { return error; }
  error = metadata.Check();
  if (!error) { metadata.guest_view_ = std::move(view); }
  return error;
}
std::error_code HostStorageManagedMetadata::Check() const {
#if !XE_PLATFORM_LINUX && !XE_PLATFORM_MAC
  return Error(std::errc::operation_not_supported);
#else
  if (fd_ < 0) {
    return Error(std::errc::operation_canceled);
  }
  std::error_code error;
  auto guard = root_->Mutation(error);
  if (!guard) {
    return error;
  }
  std::string actual;
  bool exists = false;
  error =
      SnapshotLeaf(root_->state_->directory, leaf_.string(), actual, exists);
  if (!error && (!exists || actual != leaf_.string())) {
    error = Error(std::errc::state_not_recoverable);
  }
  struct stat named;
  if (!error && fstatat(root_->state_->directory, leaf_.c_str(), &named,
                        AT_SYMLINK_NOFOLLOW)) {
    error = NativeError();
  }
  if (!error && (!SameSnapshotInfo(SnapshotInfo(named), info_) ||
                 named.st_uid != geteuid() || (named.st_mode & 077))) {
    error = Error(std::errc::state_not_recoverable);
  }
  FileHandleInfo observed;
  std::vector<uint8_t> bytes;
  if (!error) {
    error = SnapshotRead(fd_, 65536, observed, bytes);
  }
  if (!error && (!SameSnapshotInfo(observed, info_) || bytes != bytes_)) {
    error = Error(std::errc::state_not_recoverable);
  }
  if (!error && (SyncStorage(fd_) || SyncStorage(root_->state_->directory))) {
    error = NativeError();
  }
  return error;
#endif
}
HostStorageManagedMetadata::WriteResult HostStorageManagedMetadata::Initialize(
    std::span<const uint8_t> initial) {
#if !XE_PLATFORM_LINUX && !XE_PLATFORM_MAC
  return {Error(std::errc::operation_not_supported)};
#else
  if (!created_ || !bytes_.empty() || initial.empty() ||
      initial.size() > 65536) {
    return {Error(std::errc::invalid_argument)};
  }
  std::error_code error;
  auto guard = root_->Mutation(error);
  if (!guard) {
    return {error};
  }
  error = guard->Admit(initial.size() + kMetadataAllowance);
  if (error) {
    return {error};
  }
  struct stat named;
  if (fstatat(root_->state_->directory, leaf_.c_str(), &named,
              AT_SYMLINK_NOFOLLOW)) {
    return {NativeError()};
  }
  if (!SameSnapshotInfo(SnapshotInfo(named), info_)) {
    return {Error(std::errc::state_not_recoverable)};
  }
  std::vector<uint8_t> immutable(initial.begin(), initial.end());
  size_t done = 0;
  while (done < initial.size()) {
    const auto count =
        pwrite(fd_, initial.data() + done, initial.size() - done, off_t(done));
    if (count < 0 && errno == EINTR) {
      continue;
    }
    if (count <= 0) {
      return {count ? NativeError() : Error(std::errc::io_error), done};
    }
    done += size_t(count);
  }
  bytes_ = std::move(immutable);
  info_.size = bytes_.size();
  if (SyncStorage(fd_) || SyncStorage(root_->state_->directory)) {
    return {NativeError(), done};
  }
  return {{}, done, true};
#endif
}
HostStorageManagedMetadata::BootstrapWriteResult
HostStorageManagedMetadata::AppendBootstrapAtomically(std::span<const uint8_t> suffix,
    std::shared_ptr<const HostStorageIntentLease> same_source_lease,
    std::string_view nonce) {
  BootstrapWriteResult result;
  if (global_critical_region::is_held_by_current_thread()) { result.error=Error(std::errc::operation_not_permitted);return result; }
  if (guest_view_ || !same_source_lease || !root_->IntentLeaseMatches(*same_source_lease) ||
      suffix.empty() || bytes_.size()>65536 || suffix.size()>65536-bytes_.size() ||
      !content_mutation_wire::ValidNonce(nonce)) {
    result.error=Error(std::errc::permission_denied);return result;
  }
  result.error=Check();if (result.error) { return result; }
#if XE_PLATFORM_LINUX || XE_PLATFORM_MAC
  std::vector<uint8_t> complete(bytes_);complete.insert(complete.end(),suffix.begin(),suffix.end());
  result.previous=HostStorageRoot::MetadataBootstrapTemporary(nonce);
  result.previous_file=info_.file_id;
  auto guard=root_->Mutation(result.error);if (!guard) { return result; }
  result.error=guard->Admit(complete.size()+kMetadataAllowance);if (result.error) { return result; }
  FileHandleInfo before;std::vector<uint8_t> before_bytes;
  result.error=SnapshotRead(fd_,65536,before,before_bytes,&info_);
  struct stat named;
  if (!result.error && (before_bytes!=bytes_ ||
      fstatat(root_->state_->directory,leaf_.c_str(),&named,AT_SYMLINK_NOFOLLOW) ||
      !SameSnapshotInfo(SnapshotInfo(named),info_))) { result.error=Error(std::errc::state_not_recoverable); }
  if (result.error) { return result; }
  Fd fresh(openat(root_->state_->directory,result.previous.c_str(),
      O_RDWR|O_CREAT|O_EXCL|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC,0600));
  if (fresh.value<0) { result.error=NativeError();return result; }
  if (flock(fresh.value,LOCK_EX|LOCK_NB)) { result.error=NativeError();return result; }
  FileHandleInfo fresh_info;result.error=BootstrapWrite(fresh.value,complete,fresh_info);
  if (result.error) { return result; }
  if (fstatat(root_->state_->directory,result.previous.c_str(),&named,AT_SYMLINK_NOFOLLOW) ||
      !SameSnapshotInfo(SnapshotInfo(named),fresh_info)) { result.error=Error(std::errc::state_not_recoverable);return result; }
  result.error=ConfinedMount(fresh.value,root_->state_->mount);if (result.error) { return result; }
  if (guard->Conflicts(root_->path()/leaf_,false,fresh_info.filesystem_id,fresh_info.file_id,lease_)) {
    result.error=Error(std::errc::device_or_resource_busy);return result;
  }
  Fd pin(fcntl(fresh.value,F_DUPFD_CLOEXEC,0));if (pin.value<0) { result.error=NativeError();return result; }
  auto fresh_lease=guard->TrackOpen(root_->path()/leaf_,fresh_info,true);
  if (!fresh_lease) { result.error=Error(std::errc::device_or_resource_busy);return result; }
  fresh_lease->identity_pin=std::move(pin);
  result.error=BootstrapRename(root_->state_->directory,result.previous.c_str(),leaf_.c_str(),true);
  if (result.error) { return result; }
  result.published=true;
  struct stat previous,current;
  const bool previous_named=!fstatat(root_->state_->directory,result.previous.c_str(),&previous,AT_SYMLINK_NOFOLLOW);
  const bool current_named=!fstatat(root_->state_->directory,leaf_.c_str(),&current,AT_SYMLINK_NOFOLLOW);
  FileHandleInfo reread;std::vector<uint8_t> old_bytes;
  auto mismatch=SnapshotRead(fd_,65536,reread,old_bytes,&info_);
  if (mismatch || !previous_named || !current_named ||
      !SameSnapshotInfo(SnapshotInfo(previous),info_) || old_bytes!=bytes_ ||
      !SameSnapshotInfo(SnapshotInfo(current),fresh_info)) {
    // Exchange preserved the raced old target rather than unlinking it. Only
    // an exact still-current pair can be exchanged back; no guessed rollback.
    if (previous_named && current_named && SameSnapshotInfo(SnapshotInfo(current),fresh_info)) {
      const auto rollback=BootstrapRename(root_->state_->directory,result.previous.c_str(),leaf_.c_str(),true);
      if (!rollback) {
        struct stat restored;
        if (!fstatat(root_->state_->directory,leaf_.c_str(),&restored,AT_SYMLINK_NOFOLLOW) &&
            SameNode(restored,previous)) { result.published=false; }
      }
    }
    result.error=Error(std::errc::state_not_recoverable);return result;
  }
  // Keep old control inode and complete wire at previous; no unlink or prefix
  // rewrite. The new descriptor was already locked/tracked before exchange.
  guard->Moved(lease_,root_->path()/result.previous);
  const int old_fd=fd_;fd_=fresh.Release();info_=fresh_info;
  auto previous_lease=std::move(lease_);lease_=std::move(fresh_lease);
  bytes_.swap(complete);result.bytes=suffix.size();close(old_fd);
  if (root_->bootstrap_test_boundary_.exchange(0)==3) {
    result.error=Error(std::errc::operation_canceled);return result;
  }
  if (SyncStorage(root_->state_->directory)) { result.error=NativeError();return result; }
  result.durable=true;return result;
#else
  result.error=Error(std::errc::operation_not_supported);return result;
#endif
}
std::error_code HostStorageManagedMetadata::Synchronize() const {
  auto error = Check();
  if (error) { return error; }
#if XE_PLATFORM_LINUX || XE_PLATFORM_MAC
  auto guard = root_->Mutation(error);
  if (!guard) { return error; }
  return SyncStorage(fd_) ? NativeError() : std::error_code{};
#else
  return Error(std::errc::operation_not_supported);
#endif
}
std::shared_ptr<const HostStorageIntentLease>
HostStorageManagedMetadata::ReserveFormatSource(std::error_code& error) const {
  error = Check();
#if XE_PLATFORM_LINUX || XE_PLATFORM_MAC
  if (error) { return {}; }
  if (!root_->format_eligible()) {
    error = Error(std::errc::operation_not_supported); return {};
  }
  auto guard = root_->Mutation(error);
  if (!guard) { return {}; }
  if (guard->Conflicts(root_->path(), true, 0, 0, lease_)) {
    error = Error(std::errc::device_or_resource_busy); return {};
  }
  if (const auto projection=root_->Projection()) {
    error=HostStorageProtection::CheckFormatGuests(*projection);
    if (error) {return {};}
  }
  // NEW independent description, never an upgrade of an MVA lease.
  Fd description(openat(root_->state_->directory, ".",
                       O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
  if (description.value < 0) { error = NativeError(); return {}; }
  if (flock(description.value, LOCK_EX | LOCK_NB)) {
    error = errno == EWOULDBLOCK ? Error(std::errc::device_or_resource_busy)
                                : NativeError();
    return {};
  }
  struct stat info;
  if (fstat(description.value, &info)) { error = NativeError(); return {}; }
  if (!S_ISDIR(info.st_mode) || info.st_dev != root_->state_->device ||
      info.st_ino != root_->state_->inode) {
    error = Error(std::errc::state_not_recoverable); return {};
  }
  error = ConfinedMount(description.value, root_->state_->mount);
  return error ? nullptr : std::shared_ptr<const HostStorageIntentLease>(
      new HostStorageIntentLease(root_, description.Release()));
#else
  error = Error(std::errc::operation_not_supported); return {};
#endif
}
HostStorageManagedMetadata::WriteResult HostStorageManagedMetadata::Append(
    std::span<const uint8_t> record) {
  WriteResult result;
#if XE_PLATFORM_LINUX || XE_PLATFORM_MAC
  result.error = Check();
  if (result.error) { return result; }
  if (record.empty() || bytes_.size() > 65536 ||
      record.size() > 65536 - bytes_.size()) {
    result.error = Error(std::errc::file_too_large); return result;
  }
  auto guard = root_->Mutation(result.error);
  if (!guard) { return result; }
  if (guard->Conflicts(root_->path() / leaf_, false, info_.filesystem_id,
                       info_.file_id, lease_)) {
    result.error = Error(std::errc::device_or_resource_busy); return result;
  }
  result.error = guard->Admit(record.size() + kMetadataAllowance);
  FileHandleInfo observed;
  std::vector<uint8_t> current;
  if (!result.error) {
    result.error = SnapshotRead(fd_, 65536, observed, current);
  }
  if (!result.error &&
      (!SameSnapshotInfo(observed, info_) || current != bytes_)) {
    result.error = Error(std::errc::state_not_recoverable);
  }
  if (result.error) { return result; }
  struct stat named;
  if (fstatat(root_->state_->directory, leaf_.c_str(), &named,
              AT_SYMLINK_NOFOLLOW)) {
    result.error = NativeError(); return result;
  }
  if (!SameSnapshotInfo(SnapshotInfo(named), info_)) {
    result.error = Error(std::errc::state_not_recoverable); return result;
  }
  const auto original_size = bytes_.size();
  bytes_.reserve(original_size + record.size());
  while (result.bytes != record.size()) {
    const auto count = pwrite(fd_, record.data() + result.bytes,
        record.size() - result.bytes, off_t(original_size + result.bytes));
    if (count < 0 && errno == EINTR) { continue; }
    if (count <= 0) {
      result.error = count ? NativeError() : Error(std::errc::io_error); break;
    }
    result.bytes += size_t(count);
  }
  // Preserve every known written byte, including a retained partial frame.
  bytes_.insert(bytes_.end(), record.begin(), record.begin() + result.bytes);
  struct stat final_info;
  if (fstat(fd_, &final_info)) {
    if (!result.error) { result.error = NativeError(); }
  } else {
    info_ = SnapshotInfo(final_info);
  }
  if (!result.error && SyncStorage(fd_)) { result.error = NativeError(); }
  result.durable = !result.error && result.bytes == record.size();
#else
  result.error = Error(std::errc::operation_not_supported);
#endif
  return result;
}
HostStorageManagedMetadata::FormatResult
HostStorageManagedMetadata::PrepareFormatRecoveryGeneration(
    std::shared_ptr<const HostStorageIntentLease> reservation) {
  FormatResult result;
#if XE_PLATFORM_LINUX || XE_PLATFORM_MAC
  result.error = Check();
  if (result.error) { return result; }
  if (!reservation || !root_->IntentLeaseMatches(*reservation) ||
      !root_->format_eligible()) {
    result.error = Error(std::errc::permission_denied); return result;
  }
  const auto old_projection = root_->Projection();
  if (!old_projection) { result.error = Error(std::errc::permission_denied); return result; }
  result.error = HostStorageProtection::ProjectionCurrent(*old_projection);
  if (result.error) { return result; }
  auto guard = root_->Mutation(result.error);
  if (!guard) { return result; }
  result.fresh_source = root_->CloneNativeLineage(result.error);
  if (!result.fresh_source) { return result; }
  auto fresh = std::const_pointer_cast<HostStorageRoot>(result.fresh_source);
  fresh->SetFormatEligibility(root_->format_eligible());
  std::vector<std::shared_ptr<const HostStorageRoot>> private_roots, namespaces;
  for (const auto& e : old_projection->state_->exclusions) {
    (e.private_tree ? private_roots : namespaces).push_back(e.root);
  }
  result.fresh_projection = fresh->InstallFormatProjection(old_projection,std::move(private_roots),
      std::move(namespaces), root_->format_eligible(), result.error);
  if (!result.fresh_projection) { return result; }
  result.fresh_source_lease = fresh->RebindFormatLease(*reservation, result.error);
  if (!result.fresh_source_lease) { return result; }
  // The caller already authenticated the complete epoch/Handoff. Retire only
  // this inspected generation; no user content or marker byte changes here.
  root_->state_->retired.store(true, std::memory_order_release);
  old_projection->state_->format_drained.store(true,std::memory_order_release);
  Close();
#else
  result.error = Error(std::errc::operation_not_supported);
#endif
  return result;
}
HostStorageManagedMetadata::FormatResult HostStorageManagedMetadata::Format(
    std::span<const uint8_t> pending, std::span<const uint8_t> complete,
    bool already_pending, std::atomic<bool>& old_cancellation,
    std::error_code after_epoch_error,
    std::shared_ptr<const HostStorageIntentLease> reservation) {
  FormatResult result;
#if !XE_PLATFORM_LINUX && !XE_PLATFORM_MAC
  result.error = Error(std::errc::operation_not_supported);
  return result;
#else
  if (!root_->format_eligible()) {
    result.error = Error(std::errc::operation_not_supported); return result;
  }
  std::error_code error;
  // Check before acquiring the single guard; no nested admission/lock callback.
  result.error = Check();
  if (result.error) {
    return result;
  }
  auto mount_root = root_->MountedFilesystemRoot(error);
  if (!mount_root || mount_root->state_->inode == root_->state_->inode) {
    result.error = error ? error : Error(std::errc::operation_not_supported);
    return result;
  }
  std::vector<std::filesystem::path> exclusions;
  if (const auto p = root_->Projection()) {
    result.error = HostStorageProtection::ProjectionCurrent(*p);
    if (result.error) { return result; }
    for (const auto& e : p->state_->exclusions) {
      if (!ContainsLocation(p->state_->source_location, e.location)) { continue; }
      auto relative = e.location.filesystem_path.lexically_relative(p->state_->source_location.filesystem_path);
      if (!Relative(relative)) {
        result.error = Error(std::errc::permission_denied); return result;
      }
      exclusions.push_back(std::move(relative));
    }
  } else if (root_->has_guest_projection()) {
    result.error = Error(std::errc::no_such_device); return result;
  }
  auto guard = root_->Mutation(error);
  if (!guard) {
    result.error = error;
    return result;
  }
  if ((!already_pending && pending.empty()) || complete.empty() ||
      bytes_.size() > 65536 || pending.size() > 65536 - bytes_.size() ||
      complete.size() > 65536 - bytes_.size() - pending.size()) {
    result.error = Error(std::errc::file_too_large);
    return result;
  }
  if (guard->Conflicts(root_->path(), true, 0, 0, lease_)) {
    result.error = Error(std::errc::device_or_resource_busy);
    return result;
  }
  if (reservation && !root_->IntentLeaseMatches(*reservation)) {
    result.error = Error(std::errc::permission_denied); return result;
  }
  Fd drain(reservation ? -1 : open(root_->path().c_str(),
                O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
  if (!reservation &&
      (drain.value < 0 || flock(drain.value, LOCK_EX | LOCK_NB))) {
    result.error = NativeError();
    return result;
  }
  result.error = reservation ? std::error_code{}
                             : ConfinedMount(drain.value, root_->state_->mount);
  if (result.error) {
    return result;
  }
  result.error =
      guard->Admit(pending.size() + complete.size() + kMetadataAllowance);
  if (result.error) {
    return result;
  }
  std::vector<RemovalNode> children;
  RemovalBudget budget;
  Fd iterator(openat(root_->state_->directory, ".",
                     O_RDONLY | O_DIRECTORY | O_CLOEXEC));
  if (iterator.value < 0) {
    result.error = NativeError();
    return result;
  }
  DIR* stream = fdopendir(iterator.value);
  if (!stream) {
    result.error = NativeError();
    return result;
  }
  iterator.Release();
  while (true) {
    errno = 0;
    const auto* row = readdir(stream);
    if (!row) {
      if (errno) {
        result.error = NativeError();
      }
      break;
    }
    if (!strcmp(row->d_name, ".") || !strcmp(row->d_name, "..") ||
        leaf_ == row->d_name) {
      continue;
    }
    RemovalNode node;
    result.error = PreflightRemoval(root_->state_->directory, row->d_name, true,
                                    root_->state_->device, root_->state_->mount,
                                    node, &budget, nullptr, &exclusions,
                                    std::filesystem::path(row->d_name));
    if (result.error) {
      break;
    }
    children.push_back(std::move(node));
  }
  closedir(stream);
  if (result.error) {
    return result;
  }
  FileHandleInfo observed;
  std::vector<uint8_t> current;
  result.error = SnapshotRead(fd_, 65536, observed, current);
  if (!result.error &&
      (!SameSnapshotInfo(observed, info_) || current != bytes_)) {
    result.error = Error(std::errc::state_not_recoverable);
  }
  if (result.error) {
    return result;
  }
  result.fresh_source = root_->CloneNativeLineage(result.error);
  if (result.error || !result.fresh_source) {
    return result;
  }
  if (result.fresh_source->state_->device != root_->state_->device ||
      result.fresh_source->state_->inode != root_->state_->inode ||
      result.fresh_source->state_->mount.id != root_->state_->mount.id) {
    result.error = Error(std::errc::state_not_recoverable);
    return result;
  }
  std::const_pointer_cast<HostStorageRoot>(result.fresh_source)->SetFormatEligibility(
      root_->format_eligible());
  if (const auto old_projection = root_->Projection()) {
    std::vector<std::shared_ptr<const HostStorageRoot>> private_roots, namespaces;
    for (const auto& e : old_projection->state_->exclusions) {
      (e.private_tree ? private_roots : namespaces).push_back(e.root);
    }
    auto fresh = std::const_pointer_cast<HostStorageRoot>(result.fresh_source);
    result.fresh_projection = reservation
        ? fresh->InstallFormatProjection(old_projection,std::move(private_roots), std::move(namespaces),
                                        root_->format_eligible(), result.error)
        : fresh->InstallProjection(std::move(private_roots), std::move(namespaces),
                                  root_->format_eligible(), result.error);
    if (!result.fresh_projection) { return result; }
  }
  if (reservation) {
    result.fresh_source_lease = result.fresh_source->RebindFormatLease(
        *reservation, result.error);
    if (!result.fresh_source_lease || result.error) { return result; }
  }
  struct stat named;
  if (fstatat(root_->state_->directory, leaf_.c_str(), &named,
              AT_SYMLINK_NOFOLLOW)) {
    result.error = NativeError();
    return result;
  }
  if (!SameSnapshotInfo(SnapshotInfo(named), info_)) {
    result.error = Error(std::errc::state_not_recoverable);
    return result;
  }
  bytes_.reserve(bytes_.size() + pending.size() + complete.size());
  auto append = [&](std::span<const uint8_t> record) -> std::error_code {
    size_t done = 0;
    while (done != record.size()) {
      const auto count = pwrite(fd_, record.data() + done, record.size() - done,
                                off_t(bytes_.size() + done));
      if (count < 0 && errno == EINTR) {
        continue;
      }
      if (count <= 0) {
        return count ? NativeError() : Error(std::errc::io_error);
      }
      done += size_t(count);
    }
    bytes_.insert(bytes_.end(), record.begin(), record.end());
    info_.size = bytes_.size();
    return SyncStorage(fd_) ? NativeError() : std::error_code{};
  };
  // All work after this point bypasses only this retired root's ordinary I/O;
  // it uses already checked native pins under the same actual admission owner.
  old_cancellation.store(true, std::memory_order_release);
  root_->state_->retired.store(true, std::memory_order_release);
  if (const auto old_projection=root_->Projection()) {
    old_projection->state_->format_drained.store(true,std::memory_order_release);
  }
  if (!already_pending) {
    result.error = append(pending);
    result.epoch_published = bytes_.size() == current.size() + pending.size();
    result.epoch_durable = !result.error;
  } else {
    result.epoch_observed = true;
    result.error = SyncStorage(fd_) ? NativeError() : std::error_code{};
    result.epoch_durable = !result.error;
  }
  if (!result.error) {
    result.error = after_epoch_error;
  }
  if (!result.error) {
    for (const auto& node : children) {
      result.data_started = true;
      result.error = RemovePrepared(root_->state_->directory, node,
                                    &result.removed_entries);
      if (result.error) {
        break;
      }
    }
  }
  if (!result.error && SyncStorage(root_->state_->directory)) {
    result.error = NativeError();
  }
  if (!result.error) {
    result.error = append(complete);
  }
  result.complete = !result.error;
  Close();
  return result;
#endif
}
HostStorageRoot::ReplaceResult HostStorageRoot::CompareAndDeleteSnapshot(
    const HostStorageSnapshotToken& expected,
    std::shared_ptr<const HostStorageSnapshotToken> recovery) const {
  return CompareAndDeleteSnapshotInternal(expected, std::move(recovery), {});
}
HostStorageRoot::ReplaceResult
HostStorageRoot::CompareAndDeleteSnapshotInternal(
    const HostStorageSnapshotToken& expected,
    std::shared_ptr<const HostStorageSnapshotToken> recovery,
    std::error_code post_flush_error, PrivateSnapshotPurpose purpose) const {
  if (purpose!=PrivateSnapshotPurpose::kOrdinary &&
      !IsPrivateSnapshotTarget(expected.relative(), purpose)) {
    return {Error(std::errc::permission_denied)};
  }
  if ((is_guest_private() && purpose==PrivateSnapshotPurpose::kOrdinary) ||
      expected.root_.get() != this ||
      !expected.exists() || !Relative(expected.relative_) ||
      expected.relative_.has_parent_path() || !recovery ||
      !recovery->exists() || !recovery->root()->is_recovery_private() ||
      recovery->bytes().size() != expected.bytes().size() ||
      !std::equal(expected.bytes().begin(), expected.bytes().end(),
                  recovery->bytes().begin())) {
    return {Error(std::errc::invalid_argument)};
  }
  const auto& backup_root = *recovery->root();
  std::error_code error;
  auto guard = Mutation(error);
  if (!guard) {
    return {error};
  }
  error = backup_root.Validate();
  if (!error && backup_root.read_only()) {
    error = Error(std::errc::read_only_file_system);
  }
  if (!error && backup_root.state_->space.get() != state_->space.get()) {
    error = Error(std::errc::cross_device_link);
  }
  if (!error) {
    error = guard->Admit(kMetadataAllowance);
  }
  if (error) {
    return {error};
  }
#if XE_PLATFORM_WIN32
  static_cast<void>(post_flush_error);
  return {Error(std::errc::operation_not_supported)};
#else
  if (backup_root.state_->device != state_->device) {
    return {Error(std::errc::cross_device_link)};
  }
  std::string leaf;
  auto parent = Parent(state_->directory, expected.relative_, leaf, error);
  if (parent.value < 0) {
    return {error};
  }
  if (expected.ascii_folded_leaf()) {
    std::string actual;
    bool exists = false;
    error = SnapshotLeaf(parent.value, leaf, actual, exists);
    if (!error && (!exists || actual != leaf)) {
      error = Error(std::errc::state_not_recoverable);
    }
    if (error) {
      return {error};
    }
  }
  Fd original(openat(parent.value, leaf.c_str(),
                     O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK));
  if (original.value < 0) {
    return {NativeError()};
  }
  error = ConfinedMount(original.value, state_->mount);
  if (!error && guard->Conflicts(path() / expected.relative_, false,
                                 expected.info().filesystem_id,
                                 expected.info().file_id, ContentPermit())) {
    error = Error(std::errc::device_or_resource_busy);
  }
  if (!error) {
    error = SnapshotMatches(original.value, expected);
  }
  if (!error) {
    error = HostStorageProtection::PrivateContents(backup_root, nullptr, true);
  }
  if (error) {
    return {error};
  }
  std::string saved;
  auto backup_parent =
      Parent(backup_root.state_->directory, recovery->relative(), saved, error);
  if (backup_parent.value < 0) {
    return {error};
  }
  Fd copy(openat(backup_parent.value, saved.c_str(),
                 O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK));
  if (copy.value < 0) {
    return {NativeError()};
  }
  error = ConfinedMount(copy.value, backup_root.state_->mount);
  if (!error && guard->Conflicts(backup_root.path() / recovery->relative(),
                                 false, recovery->info().filesystem_id,
                                 recovery->info().file_id,{},false,
                                 backup_root.directory_identity(),recovery->relative())) {
    error = Error(std::errc::device_or_resource_busy);
  }
  if (!error) {
    error = SnapshotMatches(copy.value, *recovery);
  }
  if (error) {
    return {error};
  }
  if (SyncStorage(copy.value) || SyncStorage(backup_parent.value)) {
    return {NativeError()};
  }
  // Allocate before irreversible unlink; no allocation is needed for its
  // truthful receipt even when the following directory flush fails.
  auto current = std::shared_ptr<const HostStorageSnapshotToken>(
      new HostStorageSnapshotToken(shared_from_this(), expected.relative_,
                                   false, {}, {}, expected.ascii_folded_leaf(),
                                   true));
  struct stat named;
  if (fstatat(parent.value, leaf.c_str(), &named, AT_SYMLINK_NOFOLLOW)) {
    return {NativeError()};
  }
  if (!SameSnapshotInfo(SnapshotInfo(named), expected.info())) {
    return {Error(std::errc::state_not_recoverable)};
  }
  error = SnapshotMatches(original.value, expected);
  if (error) {
    return {error};
  }
  const auto committed = CommitSnapshotRemoval(
      [&]() {
        return unlinkat(parent.value, leaf.c_str(), 0) ? NativeError()
                                                       : std::error_code{};
      },
      [&]() { return SyncStorage(parent.value) ? NativeError() : std::error_code{}; },
      post_flush_error);
  if (!committed.published) {
    return {committed.error};
  }
  return {committed.error, true, false, committed.durable, std::move(current)};
#endif
}
HostStorageRoot::ReplaceResult HostStorageRoot::RetrySnapshotDurability(
    std::shared_ptr<const HostStorageSnapshotToken> token) const {
  if (!token || token->root().get() != this || !token->publication_observed_) {
    return {Error(std::errc::invalid_argument)};
  }
  // A retry failure cannot erase the prior publication observation. Keep its
  // token and published=true even when reconciliation/durability is unknown.
  auto failed = [&](std::error_code error) -> ReplaceResult {
    return {error, true, false, false, token};
  };
  std::error_code error;
  auto guard = Mutation(error);
  if (!guard) { return failed(error); }
#if XE_PLATFORM_WIN32
  return failed(Error(std::errc::operation_not_supported));
#else
  std::string leaf;
  auto parent = Parent(state_->directory, token->relative(), leaf, error);
  if (parent.value < 0) { return failed(error); }
  if (token->ascii_folded_leaf()) {
    std::string actual;
    bool exists;
    error = SnapshotLeaf(parent.value, leaf, actual, exists);
    if (!error && (exists != token->exists() || actual != leaf)) {
      error = Error(std::errc::state_not_recoverable);
    }
    if (error) { return failed(error); }
  }
  if (!token->exists()) {
    if (guard->Conflicts(path() / token->relative(), false, 0, 0, ContentPermit())) {
      return failed(Error(std::errc::device_or_resource_busy));
    }
    struct stat named;
    if (!fstatat(parent.value, leaf.c_str(), &named, AT_SYMLINK_NOFOLLOW)) {
      return failed(Error(std::errc::state_not_recoverable));
    }
    if (errno != ENOENT) {
      return failed(NativeError());
    }
    error = SyncStorage(parent.value) ? NativeError() : std::error_code{};
    return {error, true, false, !error, std::move(token)};
  }
  Fd file(openat(parent.value, leaf.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK));
  if (file.value < 0) { return failed(NativeError()); }
  error = ConfinedMount(file.value, state_->mount);
  if (!error && guard->Conflicts(path() / token->relative(), false,
                        token->info().filesystem_id, token->info().file_id, ContentPermit())) {
    error = Error(std::errc::device_or_resource_busy);
  }
  if (!error) { error = SnapshotMatches(file.value, *token); }
  if (error) { return failed(error); }
  if (SyncStorage(file.value) || SyncStorage(parent.value)) { error = NativeError(); }
  return {error, true, false, !error, std::move(token)};
#endif
}
// The distinct intent authority owns an independent directory description.
// flock covers competing owner processes, not arbitrary same-UID host writers.
bool HostStorageRoot::IntentLeaseMatches(
    const HostStorageIntentLease& lease) const {
  return lease.root_.get() == this && lease.fd_ >= 0 && !Validate();
}
std::shared_ptr<const HostStorageIntentLease> HostStorageRoot::RebindFormatLease(
    const HostStorageIntentLease& previous, std::error_code& error) const {
  error = Validate();
#if XE_PLATFORM_LINUX || XE_PLATFORM_MAC
  if (error) { return {}; }
  if (is_guest_private() || read_only() || previous.fd_ < 0 ||
      !previous.root_ || previous.root_->path() != path()) {
    error = Error(std::errc::permission_denied); return {};
  }
  struct stat info;
  if (fstat(previous.fd_, &info)) { error = NativeError(); return {}; }
  if (!S_ISDIR(info.st_mode) || info.st_dev != state_->device ||
      info.st_ino != state_->inode) {
    error = Error(std::errc::state_not_recoverable); return {};
  }
  error = ConfinedMount(previous.fd_, state_->mount);
  if (error) { return {}; }
  // dup keeps the same proven locked description across root retirement.
  const int transferred = fcntl(previous.fd_, F_DUPFD_CLOEXEC, 0);
  if (transferred < 0) { error = NativeError(); return {}; }
  return std::shared_ptr<const HostStorageIntentLease>(
      new HostStorageIntentLease(shared_from_this(), transferred));
#else
  error = Error(std::errc::operation_not_supported); return {};
#endif
}
uint32_t HostStorageRoot::ManagedVolumeEntries(std::string_view key,
                                               std::error_code& error) const {
  error.clear();
  if (key.size() != 64 || std::any_of(key.begin(), key.end(), [](char c) {
        return !((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'));
      })) {
    error = Error(std::errc::invalid_argument);
    return 0;
  }
  auto guard = Mutation(error);
  if (!guard) {
    return 0;
  }
#if XE_PLATFORM_LINUX || XE_PLATFORM_MAC
  Fd fd(openat(state_->directory, ".",
               O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
  if (fd.value < 0) {
    error = NativeError();
    return 0;
  }
  DIR* stream = fdopendir(fd.value);
  if (!stream) {
    error = NativeError();
    return 0;
  }
  fd.Release();
  uint32_t found = 0;
  size_t count = 0;
  while (true) {
    errno = 0;
    const auto* entry = readdir(stream);
    if (!entry) {
      if (errno) {
        error = NativeError();
      }
      break;
    }
    if (++count > 65536) {
      error = Error(std::errc::value_too_large);
      break;
    }
    const std::string_view name(entry->d_name);
    if (!name.starts_with(".xenia-device-")) {
      continue;
    }
    uint32_t bit = 0;
    if (name == ".xenia-device-binding-v1-" + std::string(key)) {
      bit = 1;
    } else if (name == ".xenia-device-state-v1-" + std::string(key)) {
      bit = 2;
    } else if (name == ".xenia-device-volume-v1-" + std::string(key)) {
      bit = 4;
    } else {
      error = Error(std::errc::state_not_recoverable);
      break;
    }
    struct stat info;
    if (fstatat(state_->directory, entry->d_name, &info, AT_SYMLINK_NOFOLLOW)) {
      error = NativeError();
      break;
    }
    if (!S_ISDIR(info.st_mode) || info.st_uid != geteuid() ||
        (info.st_mode & 077) || info.st_dev != state_->device) {
      error = Error(std::errc::permission_denied);
      break;
    }
    Fd typed(openat(state_->directory, entry->d_name,
                    O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    if (typed.value < 0) {
      error = NativeError();
      break;
    }
    struct stat pinned;
    if (fstat(typed.value, &pinned)) {
      error = NativeError();
      break;
    }
    if (!SameNode(info, pinned) || pinned.st_uid != geteuid() ||
        (pinned.st_mode & 077)) {
      error = Error(std::errc::state_not_recoverable);
      break;
    }
    error = ConfinedMount(typed.value, state_->mount);
    if (error) {
      break;
    }
    found |= bit;
  }
  closedir(stream);
  if (!error) {
    error = Validate();
  }
  return error ? 0 : found;
#else
  error = Error(std::errc::operation_not_supported);
  return 0;
#endif
}
HostStorageRootIdentity HostStorageRoot::IntentRootIdentity(
    std::error_code& error) const {
  error.clear();
  auto guard = state_->space->Begin(path(), error, state_->directory_identity);
  if (!guard) { return {}; }
  error = Validate();
#if XE_PLATFORM_MAC
  if (!error && !state_->space->NativeAuthorityFor(state_->device)) {
    error = Error(std::errc::operation_not_supported);
  }
  if (!error) {
    ProtectionLocation proof;
    error = HostStorageProtection::Location(*this, true, proof);
  }
#endif
#if XE_PLATFORM_LINUX || XE_PLATFORM_MAC
  if (!error && !state_->space->NativeAuthorityFor(state_->device)) {
    error = Error(std::errc::operation_not_supported);
  }
  return error ? HostStorageRootIdentity{} : HostStorageRootIdentity{
      uint64_t(state_->device), uint64_t(state_->inode), state_->mount.id};
#else
  if (!error) { error = Error(std::errc::operation_not_supported); }
  return {};
#endif
}
std::error_code HostStorageRoot::IntentDisjointFrom(
    const HostStorageRoot& source) const {
  std::error_code error;
  // No guest/provider callback: compare real pinned mount/inode/filesystem
  // coordinates, including same-FS bind aliases, not only path spelling.
  auto a = IntentRootIdentity(error); if (error) { return error; }
  auto b = source.IntentRootIdentity(error); if (error) { return error; }
  static_cast<void>(a); static_cast<void>(b);
  ProtectionLocation target_location, source_location;
  error = HostStorageProtection::Location(*this, true, target_location);
  if (!error) {
    error = HostStorageProtection::Location(source, true, source_location);
  }
  if (!error && Overlaps(target_location, source_location)) {
    error = PrivateExcludedFrom(source);
  }
  return error;
}
std::shared_ptr<const HostStorageIntentLease> HostStorageRoot::AcquireIntentLease(
    std::error_code& error, bool profile_target) const {
  if (profile_target ? is_guest_private() : !is_profile_intent_private()) {
    error = Error(std::errc::permission_denied); return {};
  }
#if XE_PLATFORM_LINUX || XE_PLATFORM_MAC
  if (!state_->space->NativeAuthorityFor(state_->device)) {
    error = Error(std::errc::operation_not_supported); return {};
  }
  const auto space = state_->space->Observe(path());
  if (space.error) {
    error = space.error;
    return {};
  }
#endif
  auto guard = Mutation(error);
  if (!guard) { return {}; }
#if XE_PLATFORM_LINUX || XE_PLATFORM_MAC
  Fd fd(openat(state_->directory, ".",
               O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
  if (fd.value < 0) { error = NativeError(); return {}; }
  if (flock(fd.value, LOCK_EX | LOCK_NB)) {
    error = errno == EWOULDBLOCK ? Error(std::errc::device_or_resource_busy)
                                : NativeError();
    return {};
  }
  return std::shared_ptr<const HostStorageIntentLease>(
      new HostStorageIntentLease(shared_from_this(), fd.Release()));
#else
  error = Error(std::errc::operation_not_supported); return {};
#endif
}
std::vector<std::filesystem::path> HostStorageRoot::ListIntentFiles(
    std::error_code& error) const {
  if (!is_profile_intent_private()) {
    error = Error(std::errc::permission_denied); return {};
  }
  auto guard = Mutation(error);
  if (!guard) { return {}; }
#if XE_PLATFORM_LINUX || XE_PLATFORM_MAC
  Fd fd(openat(state_->directory, ".",
               O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
  if (fd.value < 0) { error = NativeError(); return {}; }
  DIR* stream = fdopendir(fd.value);
  if (!stream) { error = NativeError(); return {}; }
  fd.Release();
  std::vector<std::filesystem::path> result;
  while (true) {
    errno = 0;
    auto* entry = readdir(stream);
    if (!entry) { if (errno) { error = NativeError(); } break; }
    if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) { continue; }
    if (result.size() == 8192 || !IntentLeaf(entry->d_name)) {
      error = Error(std::errc::bad_message); break;
    }
    result.emplace_back(entry->d_name);
  }
  closedir(stream);
  std::sort(result.begin(), result.end());
  return error ? std::vector<std::filesystem::path>{} : result;
#else
  error = Error(std::errc::operation_not_supported); return {};
#endif
}
std::shared_ptr<HostStorageRoot> HostStorageRoot::ReserveSnapshotFile(
    std::shared_ptr<const HostStorageSnapshotToken> expected,
    std::error_code& error) const {
  error.clear();
  if (!expected || expected->root().get() != this ||
      expected->relative().has_parent_path() || expected->relative().empty()) {
    error = Error(std::errc::invalid_argument);
    return {};
  }
  auto reserved = ReserveContentNamespace(expected->relative(),
      expected->exists() ? std::optional<FileHandleInfo>(expected->info())
                         : std::nullopt, false, error, expected->ascii_folded_leaf());
  if (!reserved) { return {}; }
  auto current = reserved->CaptureSnapshot(expected->relative(),
      expected->bytes().size(), error, expected->ascii_folded_leaf());
  if (!current) { return {}; }
#if XE_PLATFORM_LINUX || XE_PLATFORM_MAC
  if (current->exists() != expected->exists() ||
      current->relative() != expected->relative() ||
      (current->exists() && !SameSnapshotInfo(current->info(), expected->info())) ||
      current->bytes().size() != expected->bytes().size() ||
      !std::equal(current->bytes().begin(), current->bytes().end(), expected->bytes().begin())) {
    error = Error(std::errc::state_not_recoverable);
    return {};
  }
  reserved->state_->snapshot_namespace = true;
  return reserved;
#else
  error = Error(std::errc::operation_not_supported);
  return {};
#endif
}
bool HostStorageRoot::IsSnapshotReservationOf(const HostStorageRoot& parent) const {
  return state_->snapshot_namespace && state_->parent.get() == &parent &&
      state_->content_permit && state_->content_permit->namespace_scope &&
      state_->content_permit->active.load(std::memory_order_acquire);
}
std::error_code HostStorageRoot::ReleaseRetiredSnapshotReservation() const {
  if (!state_->snapshot_namespace || !retiring() || !state_->content_permit ||
      !state_->content_permit->namespace_scope) {
    return Error(std::errc::operation_not_permitted);
  }
  state_->content_permit->active.store(false, std::memory_order_release);
  return {};
}
HostStorageDurableObservation HostStorageRoot::ObserveSnapshotDurability(
    const HostStorageSnapshotToken& token) const {
  return ObserveIntentSnapshot(token);
}
HostStorageDurableObservation HostStorageRoot::ObserveIntentSnapshot(
    const HostStorageSnapshotToken& token) const {
  if (token.root().get() != this) {
    return {Error(std::errc::invalid_argument)};
  }
  std::error_code error;
  auto guard = Mutation(error);
  if (!guard) { return {error}; }
#if XE_PLATFORM_LINUX || XE_PLATFORM_MAC
  std::string leaf;
  auto parent = Parent(state_->directory, token.relative(), leaf, error);
  if (parent.value < 0) { return {error}; }
  if (token.ascii_folded_leaf()) {
    std::string actual; bool exists;
    error = SnapshotLeaf(parent.value, leaf, actual, exists);
    if (!error && (exists != token.exists() || actual != leaf)) {
      error = Error(std::errc::state_not_recoverable);
    }
    if (error) { return {error}; }
  }
  if (!token.exists()) {
    if (guard->Conflicts(path() / token.relative(), false, 0, 0, ContentPermit())) {
      return {Error(std::errc::device_or_resource_busy)};
    }
    struct stat named;
    if (!fstatat(parent.value, leaf.c_str(), &named, AT_SYMLINK_NOFOLLOW)) {
      return {Error(std::errc::state_not_recoverable)};
    }
    if (errno != ENOENT) {
      return {NativeError()};
    }
    error = SyncStorage(parent.value) ? NativeError() : std::error_code{};
    return {error, true, !error};
  }
  Fd file(openat(parent.value, leaf.c_str(),
                 O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK));
  if (file.value < 0) { return {NativeError()}; }
  error = ConfinedMount(file.value, state_->mount);
  if (!error && guard->Conflicts(path() / token.relative(), false,
          token.info().filesystem_id, token.info().file_id, ContentPermit())) {
    error = Error(std::errc::device_or_resource_busy);
  }
  if (!error) { error = SnapshotMatches(file.value, token); }
  if (error) { return {error}; }
  if (SyncStorage(file.value) || SyncStorage(parent.value)) { error = NativeError(); }
  return {error, true, !error};
#else
  return {Error(std::errc::operation_not_supported)};
#endif
}
std::unique_ptr<HostStorageSnapshotBatch> HostStorageRoot::ValidateSnapshotBatch(
    std::vector<std::shared_ptr<const HostStorageSnapshotToken>> tokens,
    std::error_code& error) const {
  error.clear();
  if (tokens.empty() || tokens.size() > kMaximumSnapshotBatchTokens) {
    error = Error(std::errc::invalid_argument); return {};
  }
  // A whole profile may exceed the mutation journal's independent16-file
  // envelope. Bound the read projection first, without overflowing its sum or
  // cloning retained token payloads. No currentness authority is minted here.
  uint64_t total_bytes = 0;
  for (const auto& token : tokens) {
    if (!token) {
      error = Error(std::errc::invalid_argument);
      return {};
    }
    if (token->bytes().size() > kMaximumSnapshotBatchBytes - total_bytes) {
      error = Error(std::errc::file_too_large);
      return {};
    }
    total_bytes += token->bytes().size();
  }
  std::set<std::filesystem::path> leaves;
  for (const auto& token : tokens) {
    if (!token || token->root().get() != this || !Relative(token->relative())) {
      error = Error(std::errc::invalid_argument); return {};
    }
    auto slot = token->relative();
    if (token->ascii_folded_leaf()) {
      auto name = slot.filename().string();
      for (char& c : name) { if (c >= 'A' && c <= 'Z') { c += 'a' - 'A'; } }
      slot = slot.parent_path() / name;
    }
    if (!leaves.insert(slot).second) {
      error = Error(std::errc::invalid_argument); return {};
    }
  }
  std::vector<std::optional<FileInfo>> metadata(tokens.size());
  auto guard = Mutation(error);
  if (!guard) { return {}; }
#if XE_PLATFORM_WIN32
  error = Error(std::errc::operation_not_supported); return {};
#else
  for (size_t i = 0; i != tokens.size(); ++i) {
    const auto& token = tokens[i];
    std::string leaf;
    auto parent = Parent(state_->directory, token->relative(), leaf, error);
    if (parent.value < 0) { return {}; }
    if (token->ascii_folded_leaf()) {
      std::string actual; bool exists = false;
      error = SnapshotLeaf(parent.value, leaf, actual, exists);
      if (!error && (exists != token->exists() || actual != leaf)) {
        error = Error(std::errc::state_not_recoverable);
      }
      if (error) { return {}; }
    }
    struct stat named;
    if (!token->exists()) {
      if (guard->Conflicts(path() / token->relative(), false, 0, 0, ContentPermit())) {
        error = Error(std::errc::device_or_resource_busy);
        return {};
      }
      if (!fstatat(parent.value, leaf.c_str(), &named, AT_SYMLINK_NOFOLLOW) ||
          errno != ENOENT) {
        error = Error(std::errc::state_not_recoverable); return {};
      }
      continue;
    }
    Fd file(openat(parent.value, leaf.c_str(),
                   O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK));
    if (file.value < 0) { error = NativeError(); return {}; }
    error = ConfinedMount(file.value, state_->mount);
    if (!error && guard->Conflicts(path() / token->relative(), false,
            token->info().filesystem_id, token->info().file_id, ContentPermit())) {
      error = Error(std::errc::device_or_resource_busy);
    }
    if (!error) { error = SnapshotMatches(file.value, *token); }
    if (error) { return {}; }
    struct stat observed;
    if (fstat(file.value, &observed)) { error = NativeError(); return {}; }
    if (!SameSnapshotInfo(SnapshotInfo(observed), token->info())) {
      error = Error(std::errc::state_not_recoverable); return {};
    }
    FileInfo information{};
    information.type = FileInfo::Type::kFile;
    information.name = token->relative().filename();
    information.path = path() / token->relative();
    information.total_size = size_t(observed.st_size);
    const auto timestamp = [](time_t value, uint64_t& target) {
      constexpr int64_t epoch = 11644473600;
      if (value < -epoch || value > int64_t(UINT64_MAX / 10000000) - epoch) {
        return false;
      }
      target = uint64_t(value + epoch) * 10000000;
      return true;
    };
    if (!timestamp(observed.st_ctime, information.create_timestamp) ||
        !timestamp(observed.st_atime, information.access_timestamp) ||
        !timestamp(observed.st_mtime, information.write_timestamp)) {
      error = Error(std::errc::value_too_large); return {};
    }
    metadata[i] = std::move(information);
  }
  return std::unique_ptr<HostStorageSnapshotBatch>(new HostStorageSnapshotBatch(
      shared_from_this(), std::move(tokens), std::move(metadata), std::move(guard)));
#endif
}
HostStorageRoot::ReplaceResult HostStorageRoot::AtomicReplaceFile(
    const std::filesystem::path& staged, const std::filesystem::path& target,
    const std::filesystem::path& backup) const {
  if (!backup.empty() && backup.parent_path() != target.parent_path()) {
    return {Error(std::errc::invalid_argument), false};
  }
  return ReplaceFile(staged, target, *this, backup, true);
}
HostStorageRoot::ReplaceResult HostStorageRoot::AtomicReplaceFile(
    const std::filesystem::path& staged, const std::filesystem::path& target,
    std::shared_ptr<const HostStorageRoot> backup_root,
    const std::filesystem::path& backup) const {
  if (!backup_root || !Relative(backup)) {
    return {Error(std::errc::invalid_argument), false};
  }
  return ReplaceFile(staged, target, *backup_root, backup, false);
}
#if XE_PLATFORM_LINUX || XE_PLATFORM_MAC
namespace {
std::error_code ContentPathCase(int root, const std::filesystem::path& relative) {
  Fd current(openat(root, ".", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
  if (current.value < 0) { return NativeError(); }
  for (auto part = relative.begin(); part != relative.end(); ++part) {
    const auto leaf = part->string();
    std::string actual; bool exists = false;
    auto error = SnapshotLeaf(current.value, leaf, actual, exists);
    if (error) { return error; }
    if (!exists) { return {}; }
    if (actual != leaf) { return Error(std::errc::state_not_recoverable); }
    auto next = part; ++next;
    if (next == relative.end()) { return {}; }
    current = Fd(openat(current.value, leaf.c_str(),
        O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    if (current.value < 0) { return NativeError(); }
  }
  return {};
}
}
#endif
std::error_code HostStorageRoot::CheckContentPathCase(
    const std::filesystem::path& relative) const {
  if (!Relative(relative)) { return Error(std::errc::invalid_argument); }
  std::error_code error;
  auto guard = Mutation(error);
  if (!guard) { return error; }
#if XE_PLATFORM_LINUX || XE_PLATFORM_MAC
  return ContentPathCase(state_->directory, relative);
#else
  return Error(std::errc::operation_not_supported);
#endif
}
HostStorageRoot::ReplaceResult HostStorageRoot::ReplaceFile(
    const std::filesystem::path& staged, const std::filesystem::path& target,
    const HostStorageRoot& backup_root, const std::filesystem::path& backup,
    bool reuse_original, const SnapshotReplacement* checked) const {
  if (HostStorageProtection::Guest(*this)) {
    auto error = CheckGuestAccess(staged);
    if (!error) { error = CheckGuestAccess(target); }
    if (error) { return {error}; }
  }
  if (!backup.empty() && HostStorageProtection::Guest(backup_root)) {
    if (auto error = backup_root.CheckGuestAccess(backup)) { return {error}; }
  }
  if (is_profile_intent_private()) {
    return {Error(std::errc::permission_denied)};
  }
  if (!Relative(staged) || !Relative(target) ||
      staged.parent_path() != target.parent_path() ||
      (!backup.empty() && !Relative(backup)) || staged == target ||
      (&backup_root == this && (backup == target || backup == staged))) {
    return {Error(std::errc::invalid_argument), false};
  }
#if !XE_PLATFORM_WIN32
  const bool private_snapshot = checked &&
      checked->purpose!=PrivateSnapshotPurpose::kOrdinary &&
      IsPrivateSnapshotTarget(checked->expected.relative(),checked->purpose);
  if (is_guest_private() && !private_snapshot &&
      (backup_root.state_->device != state_->device ||
       backup_root.state_->inode != state_->inode)) {
    return {Error(std::errc::permission_denied), false};
  }
#endif
  std::error_code error;
  auto guard = Mutation(error);
  if (!guard) {
    return {error, false};
  }
  if (!ContentNamespaceReady(target)) {
    return {Error(std::errc::device_or_resource_busy)};
  }
#if XE_PLATFORM_LINUX || XE_PLATFORM_MAC
  if (ContentPermit()) {
    error = ContentPathCase(state_->directory, target);
    if (error) { return {error}; }
  }
#endif
  error = backup_root.Validate();
  if (error) {
    return {error, false};
  }
  if (backup_root.read_only()) {
    return {Error(std::errc::read_only_file_system), false};
  }
  if (backup_root.state_->space.get() != state_->space.get()) {
    return {Error(std::errc::cross_device_link), false};
  }
  error = guard->Admit(kMetadataAllowance);
  if (error) {
    return {error, false};
  }
#if XE_PLATFORM_WIN32
  return {Error(std::errc::operation_not_supported), false};
#else
  if (backup_root.state_->device != state_->device) {
    return {Error(std::errc::cross_device_link), false};
  }
  std::string source;
  auto parent = Parent(state_->directory, staged, source, error);
  if (parent.value < 0) {
    return {error, false};
  }
  const auto destination = target.filename().string();
  std::string saved;
  Fd backup_parent;
  if (!backup.empty()) {
    auto opened = Parent(backup_root.state_->directory, backup, saved, error);
    if (opened.value < 0) {
      return {error, false};
    }
    backup_parent.value = opened.Release();
    struct stat existing;
    if (!checked && !reuse_original &&
        !fstatat(backup_parent.value, saved.c_str(),
                 &existing, AT_SYMLINK_NOFOLLOW)) {
      return {Error(std::errc::file_exists), false};
    }
    if (!checked && !reuse_original && errno != ENOENT) {
      return {NativeError(), false};
    }
  }
  struct stat info;
  if (fstatat(parent.value, source.c_str(), &info, AT_SYMLINK_NOFOLLOW)) {
    return {NativeError(), false};
  }
  if (!S_ISREG(info.st_mode) || info.st_nlink != 1) {
    return {Error(std::errc::permission_denied), false};
  }
  auto source_pin = PinAt(parent.value, source.c_str());
  if (source_pin.value < 0) {
    return {NativeError(), false};
  }
  error = ConfinedMount(source_pin.value, state_->mount);
  if (error) {
    return {error, false};
  }
  struct stat source_identity;
  if (fstat(source_pin.value, &source_identity)) { return {NativeError()}; }
  if (!SameNode(info, source_identity)) {
    return {Error(std::errc::state_not_recoverable)};
  }
  const auto stage_lease = checked ? checked->stage.file_->storage_lease()
                                   : std::shared_ptr<const HostStorageFileLease>{};
  // Allocate/normalize BEFORE rename. Move assignment of path below is
  // noexcept, so a successful rename cannot lose its receipt to allocation.
  std::filesystem::path published_path = (path() / target).lexically_normal();
  const auto target_namespace = ExactNamespace(ContentPermit(), published_path);
  if (guard->Conflicts(state_->path / staged, false, info.st_dev,
                       info.st_ino, stage_lease)) {
    return {Error(std::errc::device_or_resource_busy), false};
  }
  Fd input(
      openat(parent.value, source.c_str(), O_RDWR | O_NOFOLLOW | O_CLOEXEC));
  if (input.value < 0) { return {NativeError(), false}; }
  if (checked) {
    struct stat owned;
    if (fstat(int(checked->stage.file_->native_handle()), &owned)) {
      return {NativeError()};
    }
    if (owned.st_dev != info.st_dev || owned.st_ino != info.st_ino) {
      return {Error(std::errc::state_not_recoverable)};
    }
    FileHandleInfo complete_info;
    std::vector<uint8_t> complete_bytes;
    error = SnapshotRead(input.value, checked->intended.size(),
                         complete_info, complete_bytes);
    if (error) { return {error}; }
    if (!SameSnapshotInfo(complete_info, SnapshotInfo(owned)) ||
        !std::equal(complete_bytes.begin(), complete_bytes.end(),
                    checked->intended.begin(), checked->intended.end())) {
      return {Error(std::errc::state_not_recoverable)};
    }
    checked->current = std::shared_ptr<const HostStorageSnapshotToken>(
        new HostStorageSnapshotToken(shared_from_this(), target, true,
                                     complete_info, std::move(complete_bytes),
                                     checked->expected.ascii_folded_leaf(), true));
  }
  if (SyncStorage(input.value)) { return {NativeError(), false}; }
  bool backup_created = false;
  if (checked && checked->expected.ascii_folded_leaf()) {
    std::string actual;
    bool exists;
    error = SnapshotLeaf(parent.value, destination, actual, exists);
    if (!error && (exists != checked->expected.exists() ||
                   (exists && actual != destination))) {
      error = Error(std::errc::state_not_recoverable);
    }
    if (error) { return {error}; }
  }
  if (!fstatat(parent.value, destination.c_str(), &info, AT_SYMLINK_NOFOLLOW)) {
    if (checked && !checked->expected.exists()) {
      return {Error(std::errc::state_not_recoverable)};
    }
    if (!S_ISREG(info.st_mode) || backup.empty()) {
      return {Error(std::errc::permission_denied), false};
    }
    auto target_pin = PinAt(parent.value, destination.c_str());
    if (target_pin.value < 0) {
      return {NativeError(), false};
    }
    error = ConfinedMount(target_pin.value, state_->mount);
    if (error) {
      return {error, false};
    }
    if (guard->Conflicts(state_->path / target, false, info.st_dev,
                         info.st_ino, ContentPermit())) {
      return {Error(std::errc::device_or_resource_busy), false};
    }
    if (checked) {
      Fd original(openat(parent.value, destination.c_str(),
                         O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK));
      if (original.value < 0) { return {NativeError()}; }
      error = SnapshotMatches(original.value, checked->expected);
      if (error) { return {error}; }
      // The independent recovery copy was prepared outside guest mounts.
      // Validate its exact bytes/identity again under this same authority.
      error = HostStorageProtection::PrivateContents(backup_root, nullptr, true);
      if (error) { return {error}; }
      Fd recovery(openat(backup_parent.value, saved.c_str(),
                         O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK));
      if (recovery.value < 0) { return {NativeError()}; }
      error = ConfinedMount(recovery.value, backup_root.state_->mount);
      if (!error && guard->Conflicts(backup_root.path() / backup, false,
          checked->recovery->info().filesystem_id,
          checked->recovery->info().file_id,{},false,
          backup_root.directory_identity(),backup)) {
        error = Error(std::errc::device_or_resource_busy);
      }
      if (!error) { error = SnapshotMatches(recovery.value, *checked->recovery); }
      if (error) { return {error}; }
      if (SyncStorage(recovery.value) || SyncStorage(backup_parent.value)) {
        return {NativeError()};
      }
    } else {
      struct stat previous;
      if (!fstatat(backup_parent.value, saved.c_str(), &previous,
                   AT_SYMLINK_NOFOLLOW)) {
        if (!reuse_original || info.st_nlink != 2 ||
            previous.st_dev != info.st_dev || previous.st_ino != info.st_ino) {
          return {Error(std::errc::file_exists), false};
        }
      } else {
        if (errno != ENOENT || info.st_nlink != 1) {
          return {Error(std::errc::permission_denied), false};
        }
        if (linkat(parent.value, destination.c_str(), backup_parent.value,
                   saved.c_str(), 0)) {
          return {NativeError(), false};
        }
        backup_created = true;
      }
      if (SyncStorage(backup_parent.value)) {
        return {NativeError(), false, backup_created};
      }
    }
  } else if (errno != ENOENT) {
    return {NativeError(), false};
  } else if (checked && checked->expected.exists()) {
    return {Error(std::errc::state_not_recoverable)};
  }
  if (renameat(parent.value, source.c_str(), parent.value,
               destination.c_str())) {
    return {NativeError(), false, backup_created};
  }
  if (target_namespace) {
    // The verified source pin was acquired before rename. Install the actual
    // replacement identity before releasing the old pin/admission: Done may
    // allocate the just-unlinked old inode, and aliases must follow the new one.
    // info may describe the old target here; source_identity was captured from
    // the actual staged entry while source_pin continuously retained its inode.
    target_namespace->filesystem = uint64_t(source_identity.st_dev);
    target_namespace->file = uint64_t(source_identity.st_ino);
    target_namespace->identity_pin = std::move(source_pin);
  }
  if (stage_lease) {
    // Same Mutation guard owns the common lease table; no escaping callback.
    stage_lease->path = std::move(published_path);
  }
  error = SyncStorage(parent.value) ? NativeError() : std::error_code{};
  return {error, true, backup_created, !error,
          checked ? checked->current : nullptr};
#endif
}

std::error_code HostStorageRoot::FlushDirectory(
    const std::filesystem::path& relative) const {
  if (!Relative(relative, true)) {
    return Error(std::errc::invalid_argument);
  }
  std::error_code error;
  auto guard = Mutation(error);
  if (!guard) {
    return error;
  }
#if XE_PLATFORM_WIN32
  return Error(std::errc::operation_not_supported);
#else
  std::string leaf;
  auto parent = Parent(state_->directory, relative, leaf, error);
  if (parent.value < 0) {
    return error;
  }
  Fd directory(openat(parent.value, leaf.c_str(),
                      O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
  return directory.value < 0 || SyncStorage(directory.value) ? NativeError()
                                                       : std::error_code{};
#endif
}
std::error_code HostStorageRoot::FlushTree(
    const std::filesystem::path& relative) const {
  if (!Relative(relative, true)) { return Error(std::errc::invalid_argument); }
  std::error_code error;
  auto guard = Mutation(error);
  if (!guard) { return error; }
#if XE_PLATFORM_LINUX || XE_PLATFORM_MAC
  // Repeat projection checks while the common admission authority is owned.
  // Recursive flush may not enter or include an excluded private namespace.
  error = CheckGuestAccess(relative, true);
  if (error) { return error; }
  std::string leaf;
  auto parent = Parent(state_->directory, relative, leaf, error);
  if (parent.value < 0) { return error; }
  struct stat selected;
  if (fstatat(parent.value, leaf.c_str(), &selected, AT_SYMLINK_NOFOLLOW)) {
    return NativeError();
  }
  if (S_ISLNK(selected.st_mode)) { return Error(std::errc::permission_denied); }
  if (!S_ISDIR(selected.st_mode)) { return Error(std::errc::not_a_directory); }
  Fd selected_directory(openat(parent.value, leaf.c_str(),
      O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
  struct stat opened;
  if (selected_directory.value < 0 || fstat(selected_directory.value, &opened)) {
    return NativeError();
  }
  if (!SameFlushTreeStat(selected, opened)) {
    return Error(std::errc::state_not_recoverable);
  }
  error = ConfinedMount(selected_directory.value, state_->mount);
  if (error) { return error; }
  // Retain actual protected owners for this operation, without changing their
  // purpose/grammar. Compare native ancestry, including aliases and roots that
  // were opened before protection was established. No callback runs here.
  std::vector<std::shared_ptr<const HostStorageRoot>> protected_roots;
  std::vector<std::shared_ptr<const HostStorageGuestProjection>> projections;
  auto protected_access = [&]() -> std::error_code {
    // Projection publication itself need not own Space's mutation guard, so
    // refresh both snapshots at the final check rather than reuse old proof.
    protected_roots = HostStorageProtection::Private();
    projections.clear();
    {
      auto& all = HostStorageProtection::All();
      std::lock_guard lock(all.mutex);
      size_t rows = 0;
      for (const auto& weak : all.projections) {
        if (++rows > 4096) { return Error(std::errc::value_too_large); }
        if (auto projection = weak.lock()) {
          projections.push_back(std::move(projection));
        }
      }
    }
    // A raw view captured before an MVA projection was installed has no local
    // projection edge. Its flush still cannot include those actual private
    // exclusions. Never alter/relax projection ownership grammar here.
    for (const auto& projection : projections) {
      for (const auto& exclusion : projection->state_->exclusions) {
        if (!exclusion.private_tree ||
            std::find(protected_roots.begin(), protected_roots.end(),
                      exclusion.root) != protected_roots.end()) { continue; }
        if (protected_roots.size() >= 4096) {
          return Error(std::errc::value_too_large);
        }
        protected_roots.push_back(exclusion.root);
      }
    }
    std::vector<uint64_t> selected_ancestors;
    auto e = DirectoryAncestors(selected_directory.value, state_->device,
                                 state_->mount, selected_ancestors);
    if (e) { return e; }
    for (const auto& root : protected_roots) {
      if (root->state_->device != state_->device) { continue; }
      if (root->state_->space.get() != state_->space.get()) {
        return Error(std::errc::operation_not_supported);
      }
      e = root->Validate();
      if (e) { return e; }
      std::vector<uint64_t> private_ancestors;
      e = DirectoryAncestors(root->state_->directory, root->state_->device,
                               root->state_->mount, private_ancestors);
      if (e) { return e; }
      if (std::find(selected_ancestors.begin(), selected_ancestors.end(),
                    uint64_t(root->state_->inode)) != selected_ancestors.end() ||
          std::find(private_ancestors.begin(), private_ancestors.end(),
                    uint64_t(selected.st_ino)) != private_ancestors.end()) {
        return Error(std::errc::permission_denied);
      }
    }
    return {};
  };
  error = protected_access();
  if (error) { return error; }
  FlushTreeNode tree;
  size_t remaining = 4096;
  error = PrepareFlushTree(parent.value, leaf, state_->device, state_->mount,
                            tree, remaining);
  if (error) { return error; }
  if (!SameFlushTreeStat(selected, tree.identity)) {
    return Error(std::errc::state_not_recoverable);
  }
  // A logical overwrite owner may still have a deferred length. A plain fsync
  // must not be mistaken for that owner's trim/flush completion.
  if (FlushTreeReserved(*guard, state_->path / relative, tree, ContentPermit(),
                         ContentPermit() && ContentPermit()->content_io)) {
    return Error(std::errc::device_or_resource_busy);
  }
  error = RevalidateFlushTree(parent.value, tree, state_->mount);
  if (!error) { error = SyncFlushTree(tree, false); }
  if (!error) { error = SyncFlushTree(tree, true); }
  if (!error) { error = RevalidateFlushTree(parent.value, tree, state_->mount); }
  if (!error) { error = protected_access(); }
  if (!error) { error = CheckGuestAccess(relative, true); }
  if (error) { return error; }
  // Re-resolve the requested path from the retained root as well: a retained
  // old parent alone would miss an intermediate host rename/replacement.
  auto current_parent = Parent(state_->directory, relative, leaf, error);
  if (current_parent.value < 0) { return error; }
  return FlushTreeNodeCurrent(current_parent.value, tree, state_->mount);
#else
  return Error(std::errc::operation_not_supported);
#endif
}
HostStorageRoot::IntentWriteResult
HostStorageRoot::WriteNewIntentWithSnapshotGuard(
    const HostStorageSnapshotBatch &batch,
    const std::filesystem::path &relative,
    std::span<const uint8_t> bytes) const {
  IntentWriteResult out;
  // This authority is intentionally narrower than WriteNewFile: only a flat
  // immutable intent on the same actual common admission owner as its targets.
  if (!batch.root_ || !batch.guard_ ||
      !batch.guard_->IsOwnedByCurrentThread()) {
    out.error = Error(std::errc::operation_not_permitted);
    return out;
  }
  if (!is_profile_intent_private() || read_only() || !Relative(relative) ||
      relative.has_parent_path() || !IntentLeaf(relative.string()) ||
      !relative.string().starts_with("intent-") || bytes.empty() ||
      bytes.size() > kMaximumSnapshotBatchBytes + 16384) {
    out.error = Error(std::errc::invalid_argument);
    return out;
  }
#if !XE_PLATFORM_LINUX && !XE_PLATFORM_MAC
  out.error = Error(std::errc::operation_not_supported);
  return out;
#else
  if (state_->space != batch.root_->state_->space ||
      state_->device != batch.root_->state_->device ||
      !state_->space->NativeAuthorityFor(state_->device)) {
    out.error = Error(std::errc::cross_device_link);
    return out;
  }
  out.error = batch.root_->Validate();
  if (!out.error) {
    out.error = Validate();
  }
  if (!out.error) {
    out.error = HostStorageProtection::Contents(*this, nullptr, true, 8191,
                                                false, true);
  }
  if (!out.error) {
    out.error =
        batch.guard_->Admit(uint64_t(bytes.size()) + kMetadataAllowance);
  }
  if (out.error) {
    return out;
  }
  std::string leaf;
  auto parent = Parent(state_->directory, relative, leaf, out.error);
  if (parent.value < 0) {
    return out;
  }
  if (batch.guard_->Conflicts(path() / relative, false, 0, 0, {}, false,
                              state_->directory_identity, relative)) {
    out.error = Error(std::errc::device_or_resource_busy);
    return out;
  }
  Fd file(openat(parent.value, leaf.c_str(),
                 O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600));
  if (file.value < 0) {
    out.error = NativeError();
    return out;
  }
  out.created = true;
  struct stat opened;
  if (fstat(file.value, &opened)) {
    out.error = NativeError();
    return out;
  }
  if (!S_ISREG(opened.st_mode) || opened.st_nlink != 1 ||
      opened.st_uid != geteuid() || (opened.st_mode & 0077) ||
      opened.st_dev != state_->device) {
    out.error = Error(std::errc::permission_denied);
    return out;
  }
  out.error = ConfinedMount(file.value, state_->mount);
  const auto info = SnapshotInfo(opened);
  // For this O_EXCL-created file the typed conflict query checks every
  // active name/inode/namespace claim (including ordinary nonexclusive opens).
  // An additional untyped Reserved query would reinterpret this journal path
  // relative to the target-profile batch and reject its unknown ".." origin.
  if (!out.error && batch.guard_->Conflicts(
          path() / relative, false, info.filesystem_id, info.file_id, {}, false,
          state_->directory_identity, relative)) {
    out.error = Error(std::errc::device_or_resource_busy);
  }
  if (!out.error) {
    out.error = HostStorageProtection::PrivateContents(*this);
  }
  if (!out.error) {
    out.error = HostStorageProtection::GuestFile(*this, info);
  }
  if (out.error) {
    return out;
  }
  // Real exclusive name/inode lease, retained through the actual write+fsync.
  const auto lease = batch.guard_->TrackOpen(
      path() / relative, info, true, false, {}, false, false,
      state_->directory_identity, relative);
  if (!lease) {
    // O_EXCL already created the leaf. Preserve that factual, uncertain
    // attempt; without a validated retained coordinate no payload is written.
    out.error = Error(std::errc::state_not_recoverable);
    return out;
  }
  while (out.bytes != bytes.size()) {
    const size_t count = std::min<size_t>(bytes.size() - out.bytes, 1u << 20);
    const auto written = pwrite(file.value, bytes.data() + out.bytes, count,
                                static_cast<off_t>(out.bytes));
    if (written < 0) {
      if (errno == EINTR) {
        continue;
      }
      out.error = NativeError();
      return out;
    }
    if (!written) {
      out.error = Error(std::errc::io_error);
      return out;
    }
    out.bytes += size_t(written);
  }
  out.error = Validate();
  if (!out.error) {
    out.error = batch.root_->Validate();
  }
  if (out.error) {
    return out;
  }
  while (SyncStorage(file.value)) {
    if (errno != EINTR) {
      out.error = NativeError();
      return out;
    }
  }
  while (SyncStorage(parent.value)) {
    if (errno != EINTR) {
      out.error = NativeError();
      return out;
    }
  }
  out.error = Validate();
  if (!out.error) {
    out.error = batch.root_->Validate();
  }
  out.durable = !out.error;
  return out;
#endif
}
FileIoResult HostStorageRoot::WriteNewFile(
    const std::filesystem::path& relative, std::span<const uint8_t> bytes,
    bool durable, const std::atomic<bool>* canceled) const {
  if (canceled && canceled->load(std::memory_order_acquire)) {
    return {Error(std::errc::operation_canceled),0};
  }
  std::error_code error;
  auto file = OpenFile(relative, FileAccess::kFileWriteData, error, true);
  if (!file) { return {error,0}; }
  const auto written=WriteSnapshotBytes(*file,bytes,canceled);
  if (written.error) { return written; }
  if (durable) {
    error=file->FlushResult();
    if (!error) { error=FlushDirectory(relative.parent_path()); }
  }
  return {error,written.bytes};
}
std::error_code HostStorageRoot::ReadFile(const std::filesystem::path& relative,
                                          uint64_t maximum,
                                          std::vector<uint8_t>& output) const {
  std::error_code error;
  auto file = OpenFile(relative, FileAccess::kFileReadData, error);
  if (!file) {
    return error;
  }
  auto guard = state_->space->Begin(state_->path, error, state_->directory_identity);
  if (!guard) {
    return error;
  }
  const auto info = file->QueryInfo(error);
  if (!info) {
    return error;
  }
  if (!info->regular) {
    return Error(info->directory ? std::errc::is_a_directory
                                 : std::errc::operation_not_supported);
  }
  if (info->size > maximum || info->size > SIZE_MAX) {
    return Error(std::errc::file_too_large);
  }
  std::vector<uint8_t> temporary(size_t(info->size));
  size_t done = 0;
  while (done < temporary.size()) {
    const auto result = file->ReadResult(
        done,
        std::span(temporary).subspan(
            done, std::min<size_t>(temporary.size() - done, 1024 * 1024)));
    if (result.error) {
      return result.error;
    }
    if (!result.bytes) {
      return Error(std::errc::io_error);
    }
    done += result.bytes;
  }
  const auto after = file->QueryInfo(error);
  if (!after) {
    return error;
  }
  if (after->size != info->size) {
    return Error(std::errc::state_not_recoverable);
  }
  output = std::move(temporary);
  return {};
}
namespace {
class StorageReadMapping final : public MappedMemory {
 public:
  StorageReadMapping(void* data, size_t length, void* base, size_t mapped,
                     std::unique_ptr<FileHandle> owner)
      : MappedMemory(data, length),
        base_(base),
        mapped_(mapped),
        owner_(std::move(owner)) {}
  ~StorageReadMapping() override {
#if !XE_PLATFORM_WIN32
    if (base_) {
      munmap(base_, mapped_);
    }
#endif
  }

 private:
  void* base_;
  size_t mapped_;
  std::unique_ptr<FileHandle> owner_;
};
}  // namespace
std::unique_ptr<MappedMemory> HostStorageRoot::MapRead(
    const std::filesystem::path& relative, size_t offset, size_t length,
    std::error_code& error) const {
  auto file = OpenFile(relative, FileAccess::kFileReadData, error);
  if (!file) {
    return {};
  }
  const auto info = file->QueryInfo(error);
  if (!info) {
    return {};
  }
  if (!info->regular || offset >= info->size ||
      (length && length > info->size - offset)) {
    error = Error(std::errc::invalid_argument);
    return {};
  }
  if (info->size - offset > SIZE_MAX) {
    error = Error(std::errc::value_too_large);
    return {};
  }
  if (!length) {
    length = size_t(info->size - offset);
  }
#if XE_PLATFORM_WIN32
  error = Error(std::errc::operation_not_supported);
  return {};
#else
  const size_t page = size_t(sysconf(_SC_PAGESIZE));
  const size_t start = offset - offset % page;
  const size_t delta = offset - start;
  if (length > SIZE_MAX - delta) {
    error = Error(std::errc::value_too_large);
    return {};
  }
  const size_t mapped = length + delta;
  void* base = mmap(nullptr, mapped, PROT_READ, MAP_SHARED,
                    int(file->native_handle()), off_t(start));
  if (base == MAP_FAILED) {
    error = NativeError();
    return {};
  }
  return std::make_unique<StorageReadMapping>(
      static_cast<uint8_t*>(base) + delta, length, base, mapped,
      std::move(file));
#endif
}

bool HostStorageSpace::ContentNamespaceReserved(
    const std::filesystem::path& path, bool structural,
    const std::shared_ptr<const HostStorageFileLease>& permitted,
    const std::shared_ptr<const HostStorageDirectoryIdentity>& root_identity,
    const std::filesystem::path& relative) const {
  const auto normalized = path.lexically_normal();
  std::vector<std::weak_ptr<const HostStorageFileLease>> namespaces;
  {
    std::lock_guard lock(state_->namespace_mutex);
    namespaces = state_->content_namespaces;
  }
  // Matching and temporary strong-owner rundown may release native pins. Both
  // happen after unlocking; the mutex protects only the weak-row snapshot.
  for (const auto& weak : namespaces) {
    const auto lease = weak.lock();
    if (!lease || Permitted(lease, permitted) ||
        !lease->active.load(std::memory_order_acquire)) { continue; }
    if (NamespaceConflict(*lease,root_identity,relative,structural) ||
        normalized == lease->path || Below(lease->path, normalized) ||
        (structural && Below(normalized, lease->path))) { return true; }
  }
  return false;
}
const std::shared_ptr<const HostStorageFileLease>&
HostStorageRoot::ContentPermit() const {
  if (state_->content_permit) { return state_->content_permit; }
  if (state_->parent) { return state_->parent->ContentPermit(); }
  static const std::shared_ptr<const HostStorageFileLease> empty;
  return empty;
}
bool HostStorageRoot::ContentNamespaceReady(
    const std::filesystem::path& relative, bool structural) const {
  return !state_->space->ContentNamespaceReserved(
      path() / relative, structural, ContentPermit(),
      state_->directory_identity,relative);
}
std::shared_ptr<HostStorageRoot> HostStorageRoot::ProtectContentMutations(
    std::error_code& error) const {
  return HostStorageProtection::Protect(*this, error, 3);
}
bool HostStorageRoot::is_content_mutation_private() const {
  return state_->content_mutation_private ||
      (state_->parent && state_->parent->is_content_mutation_private());
}
std::vector<std::filesystem::path> HostStorageRoot::ListContentMutationFiles(
    std::error_code& error) const {
  if (!is_content_mutation_private()) {
    error = Error(std::errc::permission_denied); return {};
  }
  auto guard = Mutation(error);
  if (!guard) { return {}; }
#if XE_PLATFORM_LINUX || XE_PLATFORM_MAC
  Fd fd(openat(state_->directory, ".", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
  if (fd.value < 0) { error = NativeError(); return {}; }
  DIR* stream = fdopendir(fd.value);
  if (!stream) { error = NativeError(); return {}; }
  fd.Release();
  std::vector<std::filesystem::path> result;
  while (true) {
    errno = 0;
    const auto* row = readdir(stream);
    if (!row) { if (errno) { error = NativeError(); } break; }
    if (!strcmp(row->d_name, ".") || !strcmp(row->d_name, "..")) { continue; }
    if (result.size() == 8192 || !ContentMutationLeaf(row->d_name)) {
      error = Error(std::errc::bad_message); break;
    }
    result.emplace_back(row->d_name);
  }
  closedir(stream);
  std::sort(result.begin(), result.end());
  return error ? std::vector<std::filesystem::path>{} : result;
#else
  error = Error(std::errc::operation_not_supported); return {};
#endif
}
std::error_code HostStorageRoot::AppendContentRecord(
    const std::filesystem::path& canonical, std::span<const uint8_t> bytes,
    std::string_view temporary_nonce, unsigned stop_boundary) const {
  if (!is_content_mutation_private() || canonical.has_parent_path() ||
      !ContentMutationLeaf(canonical.string()) || canonical == "owner.v1" ||
      canonical.string().find(".partial-") != std::string::npos ||
      !content_mutation_wire::ValidNonce(temporary_nonce) || bytes.empty() ||
      bytes.size() > std::max(content_mutation_wire::kMaximumRecordBytes,
                             content_package_mutation_wire::kMaximumRecordBytes)) {
    return Error(std::errc::permission_denied);
  }
  std::error_code error;
  const auto old = CaptureSnapshot(canonical, bytes.size(), error);
  if (!old) { return error; }
  if (old->exists()) {
    if (old->bytes().size() != bytes.size() || !std::equal(bytes.begin(), bytes.end(), old->bytes().begin())) {
      return Error(std::errc::state_not_recoverable);
    }
    const auto observed = ObserveIntentSnapshot(*old);
    return observed.error ? observed.error : !observed.durable ? Error(std::errc::io_error) : std::error_code{};
  }
  const auto temporary = canonical.string() + ".partial-" + std::string(temporary_nonce);
  const auto payload = stop_boundary == 3 ? bytes.first(std::min<size_t>(17,bytes.size())) : bytes;
  const auto written = WriteNewFile(temporary, payload);
  if (written.error || written.bytes != payload.size()) {
    return written.error ? written.error : Error(std::errc::io_error);
  }
  if (stop_boundary == 1 || stop_boundary == 3) { return Error(std::errc::operation_canceled); }
  auto guard = Mutation(error);
  if (!guard) { return error; }
  error = guard->Admit(kMetadataAllowance);
  if (error) { return error; }
#if XE_PLATFORM_LINUX || XE_PLATFORM_MAC
  Fd file(openat(state_->directory, temporary.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC));
  if (file.value < 0) { return NativeError(); }
  struct stat opened, named;
  if (fstat(file.value, &opened) ||
      fstatat(state_->directory, temporary.c_str(), &named, AT_SYMLINK_NOFOLLOW)) { return NativeError(); }
  if (!SameNode(opened,named) || !S_ISREG(opened.st_mode) || opened.st_nlink != 1 ||
      uint64_t(opened.st_dev) != state_->device || uint64_t(opened.st_size) != bytes.size()) {
    return Error(std::errc::state_not_recoverable);
  }
  error = ConfinedMount(file.value, state_->mount);
  if (error) { return error; }
  std::array<uint8_t, 65536> buffer;
  for (size_t offset = 0; offset != bytes.size();) {
    const size_t count = std::min(buffer.size(), bytes.size()-offset);
    const auto read = pread(file.value, buffer.data(), count, off_t(offset));
    if (read < 0) { if (errno == EINTR) { continue; } return NativeError(); }
    if (!read || !std::equal(buffer.begin(), buffer.begin()+read, bytes.begin()+offset)) {
      return Error(std::errc::state_not_recoverable);
    }
    offset += size_t(read);
  }
  if (fstat(file.value,&named) || !SameFlushTreeStat(opened,named)) { return Error(std::errc::state_not_recoverable); }
  if (SyncStorage(file.value)) { return NativeError(); }
  if (fstatat(state_->directory,temporary.c_str(),&named,AT_SYMLINK_NOFOLLOW) ||
      !SameFlushTreeStat(opened,named)) { return Error(std::errc::state_not_recoverable); }
#if XE_PLATFORM_LINUX && defined(SYS_renameat2)
  if (syscall(SYS_renameat2,state_->directory,temporary.c_str(),state_->directory,canonical.c_str(),1u)) { return NativeError(); }
#elif XE_PLATFORM_MAC && defined(RENAME_EXCL)
  if (renameatx_np(state_->directory,temporary.c_str(),state_->directory,canonical.c_str(),RENAME_EXCL)) { return NativeError(); }
#else
  return Error(std::errc::operation_not_supported);
#endif
  if (fstatat(state_->directory,canonical.c_str(),&named,AT_SYMLINK_NOFOLLOW) ||
      !SameNode(opened,named)) { return Error(std::errc::state_not_recoverable); }
  if (stop_boundary == 2) { return Error(std::errc::operation_canceled); }
  return SyncStorage(state_->directory) ? NativeError() : std::error_code{};
#else
  return Error(std::errc::operation_not_supported);
#endif
}
std::error_code HostStorageRoot::CheckContentPreparationPeer(const HostStorageRoot& other) const {
  auto error = Validate();
  if (!error) { error = other.Validate(); }
  if (error) { return error; }
  if (state_->space != other.state_->space) { return Error(std::errc::cross_device_link); }
#if XE_PLATFORM_LINUX || XE_PLATFORM_MAC
  if (state_->device != other.state_->device || state_->mount.id != other.state_->mount.id) {
    return Error(std::errc::cross_device_link);
  }
#else
  return Error(std::errc::operation_not_supported);
#endif
  return {};
}
std::optional<FileHandleInfo> HostStorageRoot::ObservePreparationArtifact(
    const std::filesystem::path& relative, std::error_code& error) const {
  error.clear();
  if (!Relative(relative) || is_guest_private() || read_only()) {
    error = Error(std::errc::permission_denied); return {};
  }
  auto guard = Mutation(error); if (!guard) { return {}; }
#if XE_PLATFORM_LINUX || XE_PLATFORM_MAC
  std::string leaf; auto parent = Parent(state_->directory,relative,leaf,error);
  if (parent.value < 0) { return {}; }
  FlushTreeNode tree; size_t remaining = 65536;
  error = PrepareFlushTree(parent.value,leaf,state_->device,state_->mount,tree,remaining);
  if (error == std::errc::no_such_file_or_directory) { error.clear(); return {}; }
  if (!error && FlushTreeReserved(*guard,path()/relative,tree,ContentPermit())) {
    error = Error(std::errc::device_or_resource_busy);
  }
  if (error) { return {}; }
  return SnapshotInfo(tree.identity);
#else
  error = Error(std::errc::operation_not_supported); return {};
#endif
}
std::error_code HostStorageRoot::PreservePreparationArtifact(
    const std::filesystem::path& relative, uint64_t observed_file, bool directory,
    const HostStorageRoot& staging, const std::filesystem::path& destination) const {
  if (!observed_file || !Relative(relative) || !Relative(destination) || destination.has_parent_path() ||
      !destination.string().starts_with("preserved-") || is_guest_private() || staging.is_guest_private() ||
      read_only() || staging.read_only() || state_->space != staging.state_->space) {
    return Error(std::errc::permission_denied);
  }
  auto error = staging.Validate(); if (error) { return error; }
  auto guard = Mutation(error); if (!guard) { return error; }
  // Preservation consumes no payload copy, but real namespace growth still
  // uses the same floor-aware authority. It cannot invent reclaimed bytes.
  error = guard->Admit(kMetadataAllowance); if (error) { return error; }
#if XE_PLATFORM_LINUX || XE_PLATFORM_MAC
  if (state_->device != staging.state_->device || state_->mount.id != staging.state_->mount.id) {
    return Error(std::errc::cross_device_link);
  }
  std::string leaf; auto parent = Parent(state_->directory,relative,leaf,error);
  if (parent.value < 0) { return error; }
  struct stat source{}, target{};
  const bool source_exists = !fstatat(parent.value,leaf.c_str(),&source,AT_SYMLINK_NOFOLLOW);
  if (!source_exists && errno != ENOENT) { return NativeError(); }
  const bool target_exists = !fstatat(staging.state_->directory,destination.c_str(),&target,AT_SYMLINK_NOFOLLOW);
  if (!target_exists && errno != ENOENT) { return NativeError(); }
  auto matches = [&](const struct stat& info) {
    return uint64_t(info.st_dev) == state_->device && uint64_t(info.st_ino) == observed_file &&
        (directory ? S_ISDIR(info.st_mode) : S_ISREG(info.st_mode));
  };
  if ((source_exists && target_exists) || (!source_exists && (!target_exists || !matches(target))) ||
      (source_exists && !matches(source))) { return Error(std::errc::state_not_recoverable); }
  const int actual_parent = source_exists ? parent.value : staging.state_->directory;
  const std::string actual_leaf = source_exists ? leaf : destination.string();
  FlushTreeNode tree; size_t remaining = 65536;
  error = PrepareFlushTree(actual_parent,actual_leaf,state_->device,state_->mount,tree,remaining);
  if (!error && !matches(tree.identity)) { error = Error(std::errc::state_not_recoverable); }
  if (!error && FlushTreeReserved(*guard,(source_exists ? path()/relative : staging.path()/destination),
                                  tree,ContentPermit())) { error = Error(std::errc::device_or_resource_busy); }
  if (!error) { error = RevalidateFlushTree(actual_parent,tree,state_->mount); }
  if (!error) { error = SyncFlushTree(tree,false); }
  if (!error) { error = SyncFlushTree(tree,true); }
  if (error) { return error; }
  if (source_exists) {
#if XE_PLATFORM_LINUX && defined(SYS_renameat2)
    if (syscall(SYS_renameat2,parent.value,leaf.c_str(),staging.state_->directory,destination.c_str(),1u)) { return NativeError(); }
#elif XE_PLATFORM_MAC && defined(RENAME_EXCL)
    if (renameatx_np(parent.value,leaf.c_str(),staging.state_->directory,destination.c_str(),RENAME_EXCL)) { return NativeError(); }
#else
    return Error(std::errc::operation_not_supported);
#endif
  }
  if (fstatat(staging.state_->directory,destination.c_str(),&target,AT_SYMLINK_NOFOLLOW) ||
      !matches(target)) { return Error(std::errc::state_not_recoverable); }
  if (SyncStorage(parent.value) || SyncStorage(staging.state_->directory)) { return NativeError(); }
  return {};
#else
  return Error(std::errc::operation_not_supported);
#endif
}
std::vector<std::pair<std::filesystem::path, uint64_t>>
HostStorageRoot::ListContentStageDirectories(std::error_code& error) const {
  auto guard = Mutation(error);
  if (!guard) { return {}; }
#if XE_PLATFORM_LINUX || XE_PLATFORM_MAC
  Fd fd(openat(state_->directory, ".", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
  if (fd.value < 0) { error = NativeError(); return {}; }
  DIR* stream = fdopendir(fd.value);
  if (!stream) { error = NativeError(); return {}; }
  fd.Release();
  std::vector<std::pair<std::filesystem::path, uint64_t>> out;
  while (true) {
    errno = 0; const auto* row = readdir(stream);
    if (!row) { if (errno) { error = NativeError(); } break; }
    std::string_view leaf(row->d_name);
    if (leaf == "." || leaf == "..") { continue; }
    const bool preserved = leaf.size() == 44 && leaf.starts_with("preserved-") &&
        (leaf.ends_with("-d") || leaf.ends_with("-h")) &&
        std::all_of(leaf.begin()+10,leaf.begin()+42,[](char c) {
          return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        });
    if (out.size() == 8192 || (!preserved && (leaf.size() != 44 || !leaf.starts_with("package-") ||
        !leaf.ends_with(".tmp") || !std::all_of(leaf.begin()+8, leaf.begin()+40,
          [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); })))) {
      error = Error(std::errc::state_not_recoverable); break;
    }
    struct stat info;
    if (fstatat(dirfd(stream), row->d_name, &info, AT_SYMLINK_NOFOLLOW)) {
      error = NativeError(); break;
    }
    const bool expected_directory = !preserved || leaf.ends_with("-d");
    if ((expected_directory ? !S_ISDIR(info.st_mode) : (!S_ISREG(info.st_mode) || info.st_nlink != 1)) ||
        uint64_t(info.st_dev) != state_->device ||
        (!preserved && (info.st_uid != geteuid() || (info.st_mode & 077)))) {
      error = Error(std::errc::permission_denied); break;
    }
    Fd actual(openat(dirfd(stream), row->d_name,
        O_RDONLY | (expected_directory ? O_DIRECTORY : 0) | O_NOFOLLOW | O_CLOEXEC));
    struct stat pinned;
    if (actual.value < 0 || fstat(actual.value, &pinned)) { error = NativeError(); break; }
    if (!SameNode(info, pinned)) { error = Error(std::errc::state_not_recoverable); break; }
    error = ConfinedMount(actual.value, state_->mount);
    if (error) { break; }
    out.emplace_back(std::filesystem::path(leaf), uint64_t(info.st_ino));
  }
  closedir(stream);
  return error ? std::vector<std::pair<std::filesystem::path,uint64_t>>{} : out;
#else
  error = Error(std::errc::operation_not_supported); return {};
#endif
}
std::shared_ptr<HostStorageRoot> HostStorageRoot::ReserveContentNamespace(
    const std::filesystem::path& relative,
    const std::optional<FileHandleInfo>& expected, bool subtree,
    std::error_code& error, bool ascii_folded_leaf) const {
  error.clear();
  if (!Relative(relative) || !ContentNamespaceReady(relative, subtree) ||
      is_guest_private() || read_only()) {
    error = Error(std::errc::permission_denied); return {};
  }
  // Make the independent owned view before entering the nonrecursive common
  // admission guard. Only the private content owner can mint this permit.
  auto view = Subroot({}, false, error);
  if (!view) { return {}; }
  auto guard = Mutation(error);
  if (!guard) { return {}; }
#if XE_PLATFORM_LINUX || XE_PLATFORM_MAC
  error = ContentPathCase(state_->directory, relative);
  if (error) { return {}; }
  std::string leaf;
  auto parent = Parent(state_->directory, relative, leaf, error);
  if (parent.value < 0) { return {}; }
  struct stat named;
  const bool exists = fstatat(parent.value, leaf.c_str(), &named, AT_SYMLINK_NOFOLLOW) == 0;
  if (!exists && errno != ENOENT) { error = NativeError(); return {}; }
  if (exists != expected.has_value()) {
    error = Error(exists && !expected && subtree ? std::errc::file_exists
                                                : std::errc::state_not_recoverable);
    return {};
  }
  FileHandleInfo info{};
  info.filesystem_id = state_->device;
  Fd namespace_pin;
  if (exists) {
    info = SnapshotInfo(named);
    if ((!info.regular && !info.directory) ||
        (info.regular && info.links != 1) ||
        info.filesystem_id != state_->device ||
        info.filesystem_id != expected->filesystem_id ||
        info.file_id != expected->file_id ||
        (info.regular && info.size != expected->size) ||
        info.directory != expected->directory || info.regular != expected->regular) {
      error = Error(std::errc::state_not_recoverable); return {};
    }
    namespace_pin = PinAt(parent.value, leaf.c_str());
    struct stat pinned;
    if (namespace_pin.value < 0 || fstat(namespace_pin.value, &pinned)) {
      error = NativeError(); return {};
    }
    if (!SameNode(named, pinned)) {
      error = Error(std::errc::state_not_recoverable); return {};
    }
    error = ConfinedMount(namespace_pin.value, state_->mount);
    if (error) { return {}; }
  }
  if (guard->Conflicts(path() / relative, subtree, info.filesystem_id,
                       info.file_id, ContentPermit())) {
    error = Error(std::errc::device_or_resource_busy); return {};
  }
  if (exists && info.directory && subtree) {
    if (xe::global_critical_region::is_held_by_current_thread()) {
      error=Error(std::errc::device_or_resource_busy); return {};
    }
    RemovalNode inventory;
    RemovalBudget budget;
    error=PreflightRemoval(parent.value,leaf,true,state_->device,state_->mount,
        inventory,&budget,HostStorageProtection::Guest(*this) ? this : nullptr);
    if (error) { return {}; }
    if (RemovalLeaseConflict(*guard,path()/relative,inventory,ContentPermit())) {
      error=Error(std::errc::device_or_resource_busy); return {};
    }
  }
  // Fixed parent+leaf owns the namespace even after target CAS. A separate
  // actual directory pin additionally closes aliases rooted inside its tree.
  auto coordinate=HostStorageDirectoryIdentity::Capture(
      (path()/relative).parent_path(),error);
  struct stat coordinate_info;
  if (!coordinate || fstat(parent.value,&coordinate_info)) {
    if (!error) { error=NativeError(); } return {};
  }
  if (!coordinate->MatchesNativeDirectory(coordinate_info.st_dev,
                                           coordinate_info.st_ino)) {
    error=Error(std::errc::state_not_recoverable); return {};
  }
  std::shared_ptr<const HostStorageDirectoryIdentity> tree;
  if (exists && info.directory) {
    tree=HostStorageDirectoryIdentity::Capture(path()/relative,error);
    if (!tree || !tree->MatchesNativeDirectory(info.filesystem_id,info.file_id)) {
      if (!error) { error=Error(std::errc::state_not_recoverable); } return {};
    }
  }
  auto permit = guard->TrackOpen(path()/relative,info,
      true,true,ContentPermit(),false,ascii_folded_leaf,std::move(coordinate),
      relative.filename(),std::move(tree));
  if (!permit) {
    error=Error(std::errc::state_not_recoverable); return {};
  }
  permit->identity_pin = std::move(namespace_pin);
  view->state_->content_permit = std::move(permit);
  return view;
#else
  error = Error(std::errc::operation_not_supported); return {};
#endif
}
std::shared_ptr<HostStorageRoot> HostStorageRoot::ReserveContentIo(
    const std::filesystem::path& relative, std::error_code& error) const {
  if (!Relative(relative) || !ContentNamespaceReady(relative, true) ||
      is_guest_private() || read_only()) {
    error = Error(std::errc::permission_denied); return {};
  }
  auto view = Subroot({}, false, error);
  if (!view) { return {}; }
  auto guard = Mutation(error);
  if (!guard) { return {}; }
#if XE_PLATFORM_LINUX || XE_PLATFORM_MAC
  error = ContentPathCase(state_->directory, relative);
  if (error) { return {}; }
  std::string leaf;
  auto parent = Parent(state_->directory, relative, leaf, error);
  if (parent.value < 0) { return {}; }
  Fd fd(openat(parent.value, leaf.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
  struct stat info;
  if (fd.value < 0 || fstat(fd.value, &info)) { error = NativeError(); return {}; }
  error = ConfinedMount(fd.value, state_->mount);
  if (error) { return {}; }
  // Ordinary existing files retain their right to complete queued I/O. New
  // opens/namespace mutations are closed until actual queued barriers + fsync
  // finish; a preexisting exclusive overwrite still refuses the tree flush.
  auto coordinate=HostStorageDirectoryIdentity::Capture(
      (path()/relative).parent_path(),error);
  struct stat coordinate_info;
  if (!coordinate || fstat(parent.value,&coordinate_info)) {
    if (!error) { error=NativeError(); } return {};
  }
  if (!coordinate->MatchesNativeDirectory(coordinate_info.st_dev,coordinate_info.st_ino)) {
    error=Error(std::errc::state_not_recoverable); return {};
  }
  auto tree=HostStorageDirectoryIdentity::Capture(path()/relative,error);
  if (!tree || !tree->MatchesNativeDirectory(info.st_dev,info.st_ino)) {
    if (!error) { error=Error(std::errc::state_not_recoverable); } return {};
  }
  auto permit = guard->TrackOpen(
      path()/relative,SnapshotInfo(info),true,true,ContentPermit(),true,
      false,std::move(coordinate),relative.filename(),std::move(tree));
  if (!permit) {
    error=Error(std::errc::state_not_recoverable); return {};
  }
  permit->identity_pin = std::move(fd);
  view->state_->content_permit = std::move(permit);
  return view;
#else
  error = Error(std::errc::operation_not_supported); return {};
#endif
}
void HostStorageRoot::ReleaseContentNamespace() const {
  const auto& permit = ContentPermit();
  if (permit && permit->namespace_scope) {
    permit->active.store(false, std::memory_order_release);
  }
}
std::unique_ptr<HostStorageSnapshotStage>
HostStorageRoot::RecoverContentSnapshotStage(
    const std::filesystem::path& relative, uint64_t file_id,
    std::span<const uint8_t> intended, std::error_code& error) const {
  if (!file_id || intended.size() > (128ull << 20)) {
    error = Error(std::errc::invalid_argument); return {};
  }
  auto file = OpenFileInternal(relative, FileAccess::kGenericAll, error,
                               false, false, false, true);
  if (!file) { return {}; }
  auto info = file->QueryInfo(error);
  if (!info || error || !info->regular || info->links != 1 ||
      info->file_id != file_id || info->size != intended.size()) {
    if (!error) { error = Error(std::errc::state_not_recoverable); }
    return {};
  }
  std::vector<uint8_t> bytes(intended.size());
  size_t done = 0;
  while (done < bytes.size()) {
    const auto read = file->ReadResult(done, std::span(bytes).subspan(
        done, std::min<size_t>(bytes.size() - done, 1 << 20)));
    if (read.error || !read.bytes || read.bytes > bytes.size() - done) {
      error = read.error ? read.error : Error(std::errc::io_error); return {};
    }
    done += read.bytes;
  }
  if (!std::equal(bytes.begin(), bytes.end(), intended.begin(), intended.end())) {
    error = Error(std::errc::state_not_recoverable); return {};
  }
  return std::unique_ptr<HostStorageSnapshotStage>(new HostStorageSnapshotStage(
      shared_from_this(), relative, std::move(file)));
}
HostStorageDurableObservation HostStorageRoot::ObserveContentDirectory(
    const std::filesystem::path& relative, uint64_t directory) const {
  if (!directory || !Relative(relative)) {
    return {Error(std::errc::invalid_argument)};
  }
  std::error_code error;
  auto guard = Mutation(error);
  if (!guard) { return {error}; }
#if XE_PLATFORM_LINUX || XE_PLATFORM_MAC
  std::string leaf;
  auto parent = Parent(state_->directory, relative, leaf, error);
  if (parent.value < 0) { return {error}; }
  Fd fd(openat(parent.value, leaf.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
  struct stat info;
  if (fd.value < 0 || fstat(fd.value, &info)) { return {NativeError()}; }
  if (uint64_t(info.st_dev) != state_->device || uint64_t(info.st_ino) != directory) {
    return {Error(std::errc::state_not_recoverable)};
  }
  error = ConfinedMount(fd.value, state_->mount);
  Fd scan(openat(fd.value, ".", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
  if (!error && scan.value < 0) { error = NativeError(); }
  if (!error) {
    DIR* stream = fdopendir(scan.value);
    if (!stream) { error = NativeError(); }
    else {
      scan.Release();
      while (true) {
        errno = 0; const auto* row = readdir(stream);
        if (!row) { if (errno) { error = NativeError(); } break; }
        if (strcmp(row->d_name, ".") && strcmp(row->d_name, "..")) {
          error = Error(std::errc::directory_not_empty); break;
        }
      }
      closedir(stream);
    }
  }
  if (!error && (SyncStorage(fd.value) || SyncStorage(parent.value))) { error = NativeError(); }
  return {error, true, !error};
#else
  return {Error(std::errc::operation_not_supported)};
#endif
}
#if XE_PLATFORM_LINUX || XE_PLATFORM_MAC
namespace {
std::error_code CheckInitialContentTree(const RemovalNode&,
    std::span<const ContentPackageInitialEntry>, HostStorageSpace::MutationGuard&,
    const std::filesystem::path&, std::shared_ptr<const HostStorageFileLease>,
    const std::shared_ptr<const HostStorageDirectoryIdentity>&,
    const std::filesystem::path&, bool);
}
#endif
std::error_code HostStorageRoot::PublishContentDirectory(
    const HostStorageRoot& staged, uint64_t expected_directory,
    const std::filesystem::path& relative, bool& published) const {
  return PublishContentDirectoryImpl(staged,expected_directory,relative,published,{});
}
std::error_code HostStorageRoot::PublishInitialContentDirectory(
    const HostStorageRoot& staged, uint64_t expected_directory,
    const std::filesystem::path& relative, bool& published,
    std::span<const ContentPackageInitialEntry> initial) const {
  if (initial.empty()) { published=false; return Error(std::errc::invalid_argument); }
  return PublishContentDirectoryImpl(staged,expected_directory,relative,published,initial);
}
std::error_code HostStorageRoot::PublishContentDirectoryImpl(
    const HostStorageRoot& staged, uint64_t expected_directory,
    const std::filesystem::path& relative, bool& published,
    std::span<const ContentPackageInitialEntry> initial) const {
  published = false;
  const auto& permit = ContentPermit();
  if (!expected_directory || !permit || !permit->namespace_scope ||
      !permit->active.load(std::memory_order_acquire) ||
      permit->path != (path() / relative).lexically_normal() ||
      !staged.state_->parent || staged.is_guest_private()) {
    return Error(std::errc::permission_denied);
  }
  std::error_code error = staged.Validate();
  if (error) { return error; }
  if (staged.state_->space != state_->space) { return Error(std::errc::cross_device_link); }
  auto guard = Mutation(error);
  if (!guard) { return error; }
#if XE_PLATFORM_LINUX || XE_PLATFORM_MAC
  error = ContentPathCase(state_->directory, relative);
  if (error) { return error; }
  if (staged.state_->device != state_->device || staged.state_->mount.id != state_->mount.id ||
      staged.state_->inode != expected_directory) {
    return Error(std::errc::state_not_recoverable);
  }
  Fd scan(openat(staged.state_->directory, ".", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
  if (scan.value < 0) { return NativeError(); }
  DIR* stream = fdopendir(scan.value);
  if (!stream) { return NativeError(); }
  scan.Release(); bool empty = true;
  while (true) {
    errno = 0; auto* row = readdir(stream);
    if (!row) { if (errno) { error = NativeError(); } break; }
    if (strcmp(row->d_name, ".") && strcmp(row->d_name, "..")) { empty = false; break; }
  }
  closedir(stream);
  if (error) { return error; }
  if (initial.empty() && !empty) { return Error(std::errc::directory_not_empty); }
  if (!initial.empty()) {
    if (initial.front().entry.file != expected_directory ||
        !initial.front().entry.directory || !initial.front().entry.relative.empty()) {
      return Error(std::errc::invalid_argument);
    }
    std::string observed_leaf;
    const auto staged_relative=staged.path().lexically_relative(staged.state_->parent->path());
    auto observed_parent=Parent(staged.state_->parent->state_->directory,
        staged_relative,observed_leaf,error);
    if (observed_parent.value<0) { return error; }
    RemovalNode observed; RemovalBudget budget;
    error=PreflightRemoval(observed_parent.value,observed_leaf,true,state_->device,
        state_->mount,observed,&budget);
    if (!error) { error=CheckInitialContentTree(observed,initial,*guard,staged.path(),
        ContentPermit(),staged.directory_identity(),{},true); }
    if (error) { return error; }
  }
  std::string target_leaf, stage_leaf;
  auto target_parent = Parent(state_->directory, relative, target_leaf, error);
  if (target_parent.value < 0) { return error; }
  const auto stage_relative = staged.path().lexically_relative(staged.state_->parent->path());
  auto stage_parent = Parent(staged.state_->parent->state_->directory,
                             stage_relative, stage_leaf, error);
  if (target_parent.value < 0 || stage_parent.value < 0) { return error; }
  struct stat actual;
  if (fstatat(stage_parent.value, stage_leaf.c_str(), &actual, AT_SYMLINK_NOFOLLOW)) { return NativeError(); }
  if (!S_ISDIR(actual.st_mode) || uint64_t(actual.st_dev) != state_->device ||
      uint64_t(actual.st_ino) != expected_directory) { return Error(std::errc::state_not_recoverable); }
  // Acquire the next identity pin before irreversible publication. The exact
  // existing permit stays absent until the real NOREPLACE rename succeeds.
  auto namespace_pin = PinAt(stage_parent.value, stage_leaf.c_str());
  struct stat pinned;
  if (namespace_pin.value < 0 || fstat(namespace_pin.value, &pinned)) { return NativeError(); }
  if (!SameNode(actual, pinned)) { return Error(std::errc::state_not_recoverable); }
  error = ConfinedMount(namespace_pin.value, state_->mount);
  if (error) { return error; }
  const auto published_tree=staged.directory_identity();
  if (!published_tree || !published_tree->MatchesNativeDirectory(
      state_->device,expected_directory)) {
    return Error(std::errc::state_not_recoverable);
  }
  if (SyncStorage(staged.state_->directory)) { return NativeError(); }
#if XE_PLATFORM_LINUX && defined(SYS_renameat2)
  // RENAME_NOREPLACE is Linux ABI flag1: a third-state destination is never
  // replaced, even by a noncooperating host writer between checks.
  if (syscall(SYS_renameat2, stage_parent.value, stage_leaf.c_str(),
              target_parent.value, target_leaf.c_str(), 1u)) { return NativeError(); }
#elif XE_PLATFORM_MAC && defined(RENAME_EXCL)
  if (renameatx_np(stage_parent.value, stage_leaf.c_str(), target_parent.value,
                   target_leaf.c_str(), RENAME_EXCL)) { return NativeError(); }
#else
  return Error(std::errc::operation_not_supported);
#endif
  permit->filesystem = uint64_t(pinned.st_dev);
  permit->file = uint64_t(pinned.st_ino);
  permit->identity_pin = std::move(namespace_pin);
  published = true;
  // The fixed destination parent+leaf remains immutable. This additional real
  // pin closes aliases rooted INSIDE the directory only after actual rename;
  // no future pathname, durability or readiness is inferred from preparation.
  permit->namespace_tree.store(published_tree,std::memory_order_release);
  if (SyncStorage(target_parent.value) || SyncStorage(stage_parent.value)) { return NativeError(); }
  return {};
#else
  return Error(std::errc::operation_not_supported);
#endif
}


struct HostStorageContentTree::State {
  std::shared_ptr<const HostStorageRoot> root;
  std::shared_ptr<const ManagedDeviceIdentity> identity;
  std::shared_ptr<const HostStorageIntentLease> source_lease;
  std::shared_ptr<const HostStorageRoot> admission_root;
  std::filesystem::path relative;
  FileHandleInfo top_info;
  std::vector<ContentPackageTreeEntry> entries;
#if XE_PLATFORM_LINUX || XE_PLATFORM_MAC
  RemovalNode retained;
#endif
};
HostStorageContentTree::HostStorageContentTree(std::unique_ptr<State> state)
    : state_(std::move(state)) {}
HostStorageContentTree::~HostStorageContentTree() = default;
const std::vector<ContentPackageTreeEntry>& HostStorageContentTree::entries() const {
  return state_->entries;
}
const FileHandleInfo& HostStorageContentTree::top_info() const { return state_->top_info; }
const std::filesystem::path& HostStorageContentTree::relative() const { return state_->relative; }
void HostStorageContentTree::BindIdentity(
    std::shared_ptr<const ManagedDeviceIdentity> identity,
    std::shared_ptr<const HostStorageIntentLease> source_lease,
    std::shared_ptr<const HostStorageRoot> admission_root) {
  state_->identity = std::move(identity);
  state_->source_lease = std::move(source_lease);
  state_->admission_root = std::move(admission_root);
}
bool HostStorageContentTree::admission_retired() const {
  return state_->admission_root && state_->admission_root->retiring();
}
const std::shared_ptr<const ManagedDeviceIdentity>& HostStorageContentTree::identity() const {
  return state_->identity;
}
namespace {
std::array<uint8_t, 32> DigestOpenedContentFile(FileHandle& file, uint64_t length,
                                               std::error_code& error) {
  std::array<uint8_t, 32> digest{};
  if (length > content_mutation_wire::kMaximumStreamBytes || length > SIZE_MAX) {
    error = Error(std::errc::value_too_large); return digest;
  }
  struct Sha {
    mbedtls_sha256_context value;
    Sha() { mbedtls_sha256_init(&value); }
    ~Sha() { mbedtls_sha256_free(&value); }
  } sha;
  if (mbedtls_sha256_starts(&sha.value, 0)) { error = Error(std::errc::io_error); return digest; }
  std::vector<uint8_t> buffer(1u << 20);
  for (uint64_t offset = 0; offset != length;) {
    const auto count = size_t(std::min<uint64_t>(buffer.size(), length - offset));
    const auto read = file.ReadResult(size_t(offset), std::span(buffer).first(count));
    if (read.error || read.bytes != count) {
      error = read.error ? read.error : Error(std::errc::io_error); return digest;
    }
    if (mbedtls_sha256_update(&sha.value, buffer.data(), count)) {
      error = Error(std::errc::io_error); return digest;
    }
    offset += count;
  }
  if (mbedtls_sha256_finish(&sha.value, digest.data())) { error = Error(std::errc::io_error); }
  return digest;
}
}  // namespace
std::vector<ContentPackageInitialEntry> HostStorageRoot::CaptureInitialContentTree(
    const std::filesystem::path& relative, std::error_code& error) const {
  auto captured=CaptureContentTree(relative,error);
  if (!captured) { return {}; }
  auto directory=Subroot(relative,false,error);
  if (!directory) { return {}; }
  std::vector<ContentPackageInitialEntry> result;
  uint64_t total=0;
  for (const auto& entry:captured->entries()) {
    ContentPackageInitialEntry row; row.entry=entry;
    if (!entry.directory) {
      if (entry.bytes>content_mutation_wire::kMaximumStreamBytes-total) {
        error=Error(std::errc::value_too_large); return {};
      }
      total+=entry.bytes;
      auto file = directory->OpenFile(entry.relative, FileAccess::kFileReadData, error);
      const auto info = file ? file->QueryInfo(error) : std::nullopt;
      if (!file || !info || info->file_id != entry.file || info->size != entry.bytes ||
          !info->regular || info->links != 1) {
        if (!error) { error = Error(std::errc::state_not_recoverable); } return {};
      }
      row.digest = DigestOpenedContentFile(*file, entry.bytes, error);
      if (error) { return {}; }
    }
    result.push_back(std::move(row));
  }
  const auto observed=ObserveInitialContentDirectory(relative,captured->top_info().file_id,result);
  error=observed.error;
  return !error && observed.matched && observed.durable ? result
      : std::vector<ContentPackageInitialEntry>{};
}
std::error_code HostStorageRoot::DiscardStreamContentDirectory(
    const std::filesystem::path& relative, uint64_t expected_directory,
    std::span<const ContentPackageInitialEntry> admitted) const {
  if (!Relative(relative) || admitted.empty() || !expected_directory ||
      admitted.front().entry.file != expected_directory) { return Error(std::errc::invalid_argument); }
  std::error_code error; auto guard = Mutation(error);
  if (!guard) { return error; }
#if XE_PLATFORM_LINUX || XE_PLATFORM_MAC
  std::string leaf; auto parent = Parent(state_->directory, relative, leaf, error);
  if (parent.value < 0) { return error; }
  struct stat named;
  if (fstatat(parent.value, leaf.c_str(), &named, AT_SYMLINK_NOFOLLOW)) {
    if (errno == ENOENT) { return SyncStorage(parent.value) ? NativeError() : std::error_code{}; }
    return NativeError();
  }
  if (!S_ISDIR(named.st_mode) || uint64_t(named.st_ino) != expected_directory) {
    return Error(std::errc::state_not_recoverable);
  }
  RemovalNode tree; RemovalBudget budget;
  error = PreflightRemoval(parent.value, leaf, true, state_->device, state_->mount, tree, &budget);
  if (error) { return error; }
  std::map<std::filesystem::path, const ContentPackageTreeEntry*> expected;
  for (const auto& row : admitted) { expected.emplace(row.entry.relative, &row.entry); }
  auto check = [&](auto&& self, const RemovalNode& node,
                   const std::filesystem::path& path) -> bool {
    const auto found = expected.find(path);
    if (found == expected.end()) { return false; }
    const auto& row = *found->second;
    if (row.file != uint64_t(node.identity.st_ino) ||
        row.mode != uint32_t(node.identity.st_mode) ||
        row.directory != bool(S_ISDIR(node.identity.st_mode)) ||
        (!row.directory && (!S_ISREG(node.identity.st_mode) || node.identity.st_nlink != 1 ||
                            uint64_t(node.identity.st_size) > row.bytes))) { return false; }
    for (const auto& child : node.children) {
      if (!self(self, child, path / child.name)) { return false; }
    }
    return true;
  };
  // Missing members can be prior successful rollback steps; any extra or
  // replaced native inode is a third state and remains untouched.
  if (!check(check, tree, {}) || RemovalLeaseConflict(*guard, path() / relative, tree, ContentPermit())) {
    return Error(std::errc::state_not_recoverable);
  }
  error = RemovePrepared(parent.value, tree);
  if (!error && SyncStorage(parent.value)) { error = NativeError(); }
  return error;
#else
  return Error(std::errc::operation_not_supported);
#endif
}
std::error_code HostStorageRoot::DiscardStreamHeaderStage(
    const std::filesystem::path& relative, uint64_t expected_file,
    std::span<const uint8_t> expected_bytes) const {
  if (!Relative(relative) || !expected_file) { return Error(std::errc::invalid_argument); }
  std::error_code error; auto guard = Mutation(error);
  if (!guard) { return error; }
#if XE_PLATFORM_LINUX || XE_PLATFORM_MAC
  std::string leaf; auto parent = Parent(state_->directory, relative, leaf, error);
  if (parent.value < 0) { return error; }
  Fd opened(openat(parent.value, leaf.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC));
  if (opened.value < 0) {
    if (errno == ENOENT) { return SyncStorage(parent.value) ? NativeError() : std::error_code{}; }
    return NativeError();
  }
  FileHandleInfo info; std::vector<uint8_t> bytes;
  error = ConfinedMount(opened.value, state_->mount);
  if (!error) { error = SnapshotRead(opened.value, content_mutation_wire::kMaximumMetadataBytes, info, bytes); }
  if (error) { return error; }
  if (info.file_id != expected_file || bytes.size() != expected_bytes.size() ||
      !std::equal(bytes.begin(), bytes.end(), expected_bytes.begin()) ||
      guard->Conflicts(path() / relative, false, info.filesystem_id, info.file_id, ContentPermit())) {
    return Error(std::errc::state_not_recoverable);
  }
  struct stat named;
  if (fstatat(parent.value, leaf.c_str(), &named, AT_SYMLINK_NOFOLLOW)) { return NativeError(); }
  if (uint64_t(named.st_ino) != info.file_id || !S_ISREG(named.st_mode) || named.st_nlink != 1) {
    return Error(std::errc::state_not_recoverable);
  }
  if (unlinkat(parent.value, leaf.c_str(), 0) || SyncStorage(parent.value)) { return NativeError(); }
  return {};
#else
  return Error(std::errc::operation_not_supported);
#endif
}
HostStorageDurableObservation HostStorageRoot::ObserveInitialContentDirectory(
    const std::filesystem::path& relative,uint64_t expected_directory,
    std::span<const ContentPackageInitialEntry> initial) const {
  if (xe::global_critical_region::is_held_by_current_thread() || !Relative(relative) ||
      initial.empty() || initial.front().entry.file!=expected_directory ||
      !initial.front().entry.directory || !initial.front().entry.relative.empty()) {
    return {Error(std::errc::invalid_argument)};
  }
  std::error_code error; auto guard=Mutation(error);
  if (!guard) { return {error}; }
#if XE_PLATFORM_LINUX || XE_PLATFORM_MAC
  std::string leaf; auto parent=Parent(state_->directory,relative,leaf,error);
  if (parent.value<0) { return {error}; }
  RemovalNode tree; RemovalBudget budget;
  error=PreflightRemoval(parent.value,leaf,true,state_->device,state_->mount,tree,&budget);
  if (!error) { error=CheckInitialContentTree(tree,initial,*guard,path()/relative,
      ContentPermit(),directory_identity(),relative,true); }
  if (!error && SyncStorage(parent.value)) { error=NativeError(); }
  return {error,!error,!error};
#else
  return {Error(std::errc::operation_not_supported)};
#endif
}
std::unique_ptr<HostStorageContentTree> HostStorageRoot::CaptureContentTree(
    const std::filesystem::path& relative, std::error_code& error) const {
  if (xe::global_critical_region::is_held_by_current_thread() ||
      !Relative(relative) || is_guest_private() || read_only()) {
    error = Error(std::errc::operation_not_permitted); return {};
  }
  auto guard = Mutation(error);
  if (!guard) { return {}; }
#if XE_PLATFORM_LINUX || XE_PLATFORM_MAC
  error = ContentPathCase(state_->directory, relative);
  if (!error) { error = CheckGuestAccess(relative, true); }
  if (error) { return {}; }
  std::string leaf;
  auto parent = Parent(state_->directory, relative, leaf, error);
  if (parent.value < 0) { return {}; }
  auto owned = std::make_unique<HostStorageContentTree::State>();
  RemovalBudget budget;
  error = PreflightRemoval(parent.value, leaf, true, state_->device,
                           state_->mount, owned->retained, &budget, this);
  if (error) { return {}; }
  // Persist a deterministic parent-first namespace, independent of readdir
  // ordering. No changed ordinal may masquerade as a changed physical member.
  auto order = [&](auto&& self, RemovalNode& node) -> void {
    std::sort(node.children.begin(), node.children.end(),
        [](const RemovalNode& a, const RemovalNode& b) { return a.name < b.name; });
    for (auto& child : node.children) { self(self, child); }
  };
  order(order, owned->retained);
  // Unlike a generic unlink request, an owned durable content transaction
  // never adopts symlinks, special nodes or multiply-linked foreign payloads.
  uint64_t path_bytes = 0;
  auto collect = [&](auto&& self, const RemovalNode& node,
                     const std::filesystem::path& child) -> bool {
    const auto& info = node.identity;
    if ((!S_ISDIR(info.st_mode) && !S_ISREG(info.st_mode)) ||
        (S_ISREG(info.st_mode) && info.st_nlink != 1)) {
      error = Error(std::errc::operation_not_supported); return false;
    }
    const auto spelling = child.native().size();
    if (std::any_of(child.native().begin(), child.native().end(),
          [](unsigned char c) { return c < 0x20 || c == 0x7f || c == '\\'; })) {
      error = Error(std::errc::operation_not_supported); return false;
    }
    if (spelling > 4096 || path_bytes > (16ull << 20) - spelling) {
      error = Error(std::errc::value_too_large); return false;
    }
    path_bytes += spelling;
    const auto full = child.empty() ? path() / relative : path() / relative / child;
    if (guard->Conflicts(full, S_ISDIR(info.st_mode), info.st_dev, info.st_ino,
                         ContentPermit())) {
      error = Error(std::errc::device_or_resource_busy); return false;
    }
#if XE_PLATFORM_MAC
    const auto stamp = info.st_mtimespec;
#else
    const auto stamp = info.st_mtim;
#endif
    owned->entries.push_back({child, uint64_t(info.st_ino), uint64_t(info.st_size),
        int64_t(stamp.tv_sec), uint32_t(stamp.tv_nsec), uint32_t(info.st_mode),
        bool(S_ISDIR(info.st_mode))});
    std::set<std::string> child_names;
    for (const auto& member : node.children) {
      auto folded = member.name;
      for (auto& c : folded) { if (c >= 'A' && c <= 'Z') { c += 'a' - 'A'; } }
      if (!child_names.emplace(std::move(folded)).second) {
        error = Error(std::errc::state_not_recoverable); return false;
      }
      if (!self(self, member, child / member.name)) { return false; }
    }
    return true;
  };
  if (!collect(collect, owned->retained, {})) { return {}; }
  owned->top_info = SnapshotInfo(owned->retained.identity);
  owned->relative = relative; owned->root = shared_from_this();
  return std::unique_ptr<HostStorageContentTree>(new HostStorageContentTree(std::move(owned)));
#else
  error = Error(std::errc::operation_not_supported); return {};
#endif
}

#if XE_PLATFORM_LINUX || XE_PLATFORM_MAC
namespace {
// The typed journal supplies the entire captured namespace. This check refuses
// unknown/replaced members, types, extra links, changed file data metadata and
// live leases BEFORE any unlink. Only a durable Retire phase may allow missing
// already-retired members; it never authorizes an extra or replacement inode.
std::error_code RecordedContentTree(const RemovalNode& root,
    const std::vector<ContentPackageTreeEntry>& expected,
    HostStorageSpace::MutationGuard& guard,
    const std::filesystem::path& physical,
    std::shared_ptr<const HostStorageFileLease> permit, bool allow_missing,
    const std::shared_ptr<const HostStorageDirectoryIdentity>& query_identity,
    const std::filesystem::path& query_relative) {
  std::map<std::filesystem::path, const ContentPackageTreeEntry*> by_path;
  for (const auto& row : expected) {
    if (!by_path.emplace(row.relative, &row).second) {
      return Error(std::errc::bad_message);
    }
  }
  size_t found = 0;
  auto visit = [&](auto&& self, const RemovalNode& node,
                   const std::filesystem::path& path) -> std::error_code {
    const auto i = by_path.find(path);
    if (i == by_path.end()) { return Error(std::errc::state_not_recoverable); }
    const auto& row = *i->second;
    const auto& info = node.identity;
    if (uint64_t(info.st_ino) != row.file || uint32_t(info.st_mode) != row.mode ||
        bool(S_ISDIR(info.st_mode)) != row.directory ||
        (!S_ISREG(info.st_mode) && !S_ISDIR(info.st_mode)) ||
        (S_ISREG(info.st_mode) && info.st_nlink != 1)) {
      return Error(std::errc::state_not_recoverable);
    }
#if XE_PLATFORM_MAC
    const auto stamp = info.st_mtimespec;
#else
    const auto stamp = info.st_mtim;
#endif
    // Own partial retirement changes directory mtime and allocation size.
    // Regular file content/metadata is never changed by retirement itself.
    if ((!allow_missing || !row.directory) &&
        (uint64_t(info.st_size) != row.bytes ||
         int64_t(stamp.tv_sec) != row.write_seconds ||
         uint32_t(stamp.tv_nsec) != row.write_nanoseconds)) {
      return Error(std::errc::state_not_recoverable);
    }
    const auto full = path.empty() ? physical : physical / path;
    if (guard.Conflicts(full, row.directory, info.st_dev, info.st_ino, permit,
                        false, query_identity, query_relative / path)) {
      return Error(std::errc::device_or_resource_busy);
    }
    ++found;
    for (const auto& child : node.children) {
      if (auto error = self(self, child, path / child.name)) { return error; }
    }
    return {};
  };
  if (auto error = visit(visit, root, {})) { return error; }
  return allow_missing || found == expected.size()
      ? std::error_code{} : Error(std::errc::state_not_recoverable);
}
std::error_code CheckInitialContentTree(const RemovalNode& root,
    std::span<const ContentPackageInitialEntry> initial,
    HostStorageSpace::MutationGuard& guard, const std::filesystem::path& physical,
    std::shared_ptr<const HostStorageFileLease> permit,
    const std::shared_ptr<const HostStorageDirectoryIdentity>& query_identity,
    const std::filesystem::path& query_relative, bool synchronize) {
  if (initial.empty() || initial.size()>content_package_mutation_wire::kMaximumEntries) {
    return Error(std::errc::bad_message);
  }
  std::vector<ContentPackageTreeEntry> entries;
  std::map<std::filesystem::path,const ContentPackageInitialEntry*> by_path;
  uint64_t bytes=0;
  for (const auto& row:initial) {
    if (!row.entry.directory) {
      if (row.entry.bytes>content_mutation_wire::kMaximumStreamBytes-bytes) {
        return Error(std::errc::value_too_large);
      }
      bytes+=row.entry.bytes;
    }
    entries.push_back(row.entry);
    if (!by_path.emplace(row.entry.relative,&row).second) { return Error(std::errc::bad_message); }
  }
  if (auto error=RecordedContentTree(root,entries,guard,physical,permit,false,
                                     query_identity,query_relative)) { return error; }
  auto visit=[&](auto&& self,const RemovalNode& node,int parent,
                  const std::filesystem::path& relative)->std::error_code {
    const auto found=by_path.find(relative);
    if (found==by_path.end()) { return Error(std::errc::state_not_recoverable); }
    if (S_ISDIR(node.identity.st_mode)) {
      for (const auto& child:node.children) {
        if (auto error=self(self,child,node.directory.value,relative/child.name)) { return error; }
      }
      return synchronize && SyncStorage(node.directory.value) ? NativeError() : std::error_code{};
    }
    Fd actual(openat(parent,node.name.c_str(),O_RDONLY|O_NONBLOCK|O_NOFOLLOW|O_CLOEXEC));
    struct stat before,after;
    if (actual.value<0 || fstat(actual.value,&before)) { return NativeError(); }
    if (!SameFlushTreeStat(before,node.identity)) { return Error(std::errc::state_not_recoverable); }
    MountIdentity captured_mount;
    auto mount_error=MountForFd(node.directory.value,captured_mount);
    if (!mount_error) { mount_error=ConfinedMount(actual.value,captured_mount); }
    if (mount_error) { return mount_error; }
    const int copied = dup(actual.value);
    if (copied < 0) { return NativeError(); }
    auto reader = FileHandle::AdoptNative(copied, false);
    std::error_code error;
    const auto digest = DigestOpenedContentFile(*reader, found->second->entry.bytes, error);
    if (error) { return error; }
    if (fstat(actual.value,&after)) { return NativeError(); }
    if (digest!=found->second->digest || !SameFlushTreeStat(before,after)) {
      return Error(std::errc::state_not_recoverable);
    }
    return synchronize && SyncStorage(actual.value) ? NativeError() : std::error_code{};
  };
  return visit(visit,root,-1,{});
}
std::error_code OnlyContentPayload(int directory) {
  Fd scan(openat(directory, ".", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
  if (scan.value < 0) { return NativeError(); }
  DIR* stream = fdopendir(scan.value);
  if (!stream) { return NativeError(); }
  scan.Release(); std::error_code error;
  while (true) {
    errno = 0; const auto* row = readdir(stream);
    if (!row) { if (errno) { error = NativeError(); } break; }
    if (!strcmp(row->d_name, ".") || !strcmp(row->d_name, "..") ||
        !strcmp(row->d_name, "payload")) { continue; }
    error = Error(std::errc::state_not_recoverable); break;
  }
  closedir(stream); return error;
}
struct ContentTruncatePlan {
  const ContentPackageTreeEntry* root = nullptr;
  std::string preserved;
  std::map<std::string, std::vector<ContentPackageTreeEntry>> tops;
};
bool ContentRecordedStamp(const struct stat& info, const ContentPackageTreeEntry& row) {
#if XE_PLATFORM_MAC
  const auto stamp = info.st_mtimespec;
#else
  const auto stamp = info.st_mtim;
#endif
  return uint64_t(info.st_size) == row.bytes && int64_t(stamp.tv_sec) == row.write_seconds &&
      uint32_t(stamp.tv_nsec) == row.write_nanoseconds;
}
std::error_code PlanContentTruncate(
    const content_package_mutation_wire::Record& record, ContentTruncatePlan& plan) {
  namespace wire = content_package_mutation_wire;
  if (record.kind != wire::Kind::kTruncate || record.replacement_directory ||
      record.before.empty() || record.before.size() > wire::kMaximumEntries ||
      !record.before.front().relative.empty() || !record.before.front().directory ||
      !content_mutation_wire::ValidPackage(record.package) ||
      !content_mutation_wire::ValidNonce(record.nonce) || !record.op_directory ||
      record.preserved_leaf.native().size() > 15 ||
      record.header_target.native().size() > 256 ||
      record.header.size() > content_mutation_wire::kMaximumMetadataBytes ||
      (record.header_exists ? !record.header_file : bool(record.header_file || !record.header.empty()))) {
    return Error(std::errc::bad_message);
  }
  content_mutation_wire::Record slot;
  slot.kind = content_mutation_wire::Kind::kHeader;
  slot.package = record.package; slot.target = record.header_target;
  if (content_mutation_wire::Target(slot).empty()) { return Error(std::errc::bad_message); }
  plan.root = &record.before.front(); plan.preserved = record.preserved_leaf.native();
  auto fold = [](std::string value) {
    for (char& c : value) { if (c >= 'A' && c <= 'Z') { c += 'a' - 'A'; } }
    return value;
  };
  if (!plan.preserved.empty() && fold(plan.preserved) != "__thumbnail.png") {
    return Error(std::errc::bad_message);
  }
  std::set<uint64_t> ids;
  for (auto id : {record.identity.content_directory, record.op_directory, record.header_file}) {
    if (id && !ids.insert(id).second) { return Error(std::errc::bad_message); }
  }
  std::map<std::string, bool> paths;
  std::set<std::string> folded;
  uint64_t path_bytes = 0;
  for (size_t index = 0; index < record.before.size(); ++index) {
    const auto& row = record.before[index]; const auto& path = row.relative.native();
    if ((index && path.empty()) || path.size() > wire::kMaximumPathBytes ||
        path.size() > wire::kMaximumAggregatePathBytes - path_bytes ||
        (!path.empty() && (path.front() == '/' || path.back() == '/')) ||
        std::any_of(path.begin(), path.end(), [](unsigned char c) { return c < 0x20 || c == 0x7f || c == '\\'; }) ||
        !row.file || !ids.insert(row.file).second || row.write_nanoseconds >= 1000000000 ||
        (row.mode & ~(uint32_t(S_IFMT) | 07777u)) ||
        (row.mode & S_IFMT) != uint32_t(row.directory ? S_IFDIR : S_IFREG) ||
        !folded.insert(fold(path)).second) { return Error(std::errc::bad_message); }
    path_bytes += path.size();
    if (index) {
      unsigned depth = 0;
      for (size_t begin = 0; begin < path.size();) {
        const auto slash = path.find('/', begin);
        const auto part = path.substr(begin, slash == path.npos ? path.npos : slash - begin);
        if (part.empty() || part == "." || part == ".." || ++depth > wire::kMaximumDepth) {
          return Error(std::errc::bad_message);
        }
        if (slash == path.npos) { break; } begin = slash + 1;
      }
      const auto last = path.rfind('/');
      const auto parent = last == path.npos ? std::string{} : path.substr(0, last);
      const auto found = paths.find(parent);
      if (found == paths.end() || !found->second) { return Error(std::errc::bad_message); }
      const auto first = path.find('/'); const auto top = path.substr(0, first);
      auto within = row;
      within.relative = first == path.npos ? std::filesystem::path{} :
          std::filesystem::path(path.substr(first + 1));
      plan.tops[top].push_back(std::move(within));
    }
    if (!paths.emplace(path, row.directory).second) { return Error(std::errc::bad_message); }
  }
  if (!plan.preserved.empty()) {
    const auto found = plan.tops.find(plan.preserved);
    if (found == plan.tops.end() || found->second.size() != 1 || found->second.front().directory) {
      return Error(std::errc::bad_message);
    }
  }
  return {};
}
// Missing direct children explain retirement-induced directory mtime/size.
// A surviving directory with its complete direct namespace must still match.
std::error_code ContentSurvivorDirectories(const RemovalNode& node,
    const std::vector<ContentPackageTreeEntry>& expected) {
  std::map<std::filesystem::path, const ContentPackageTreeEntry*> rows;
  std::map<std::filesystem::path, size_t> children;
  for (const auto& row : expected) {
    rows.emplace(row.relative, &row);
    if (!row.relative.empty()) { ++children[row.relative.parent_path()]; }
  }
  auto visit = [&](auto&& self, const RemovalNode& here,
                   const std::filesystem::path& path) -> std::error_code {
    const auto found = rows.find(path);
    if (found == rows.end()) { return Error(std::errc::state_not_recoverable); }
    if (S_ISDIR(here.identity.st_mode) && here.children.size() == children[path] &&
        !ContentRecordedStamp(here.identity, *found->second)) {
      return Error(std::errc::state_not_recoverable);
    }
    for (const auto& child : here.children) {
      if (auto error = self(self, child, path / child.name)) { return error; }
    }
    return {};
  };
  return visit(visit, node, {});
}
std::error_code CaptureContentTruncatePrivate(int directory, dev_t device,
    const MountIdentity& mount, RemovalBudget& budget, std::vector<RemovalNode>& output) {
  Fd scan(openat(directory, ".", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
  if (scan.value < 0) { return NativeError(); }
  DIR* stream = fdopendir(scan.value);
  if (!stream) { return NativeError(); } scan.Release();
  std::error_code error;
  while (true) {
    errno = 0; const auto* row = readdir(stream);
    if (!row) { if (errno) { error = NativeError(); } break; }
    if (!strcmp(row->d_name, ".") || !strcmp(row->d_name, "..")) { continue; }
    RemovalNode child;
    budget.depth = 1;  // Each staged top child keeps its original package depth.
    error = PreflightRemoval(directory, row->d_name, true, device, mount, child, &budget);
    budget.depth = 0;
    if (error) { break; } output.push_back(std::move(child));
  }
  closedir(stream); return error;
}
std::error_code CheckContentTruncateInventory(const ContentTruncatePlan& plan,
    const RemovalNode& public_tree, const std::vector<RemovalNode>& private_tops,
    HostStorageSpace::MutationGuard& guard, const std::filesystem::path& public_path,
    const std::filesystem::path& private_path,
    const std::shared_ptr<const HostStorageFileLease>& permit, bool retire,
    const std::shared_ptr<const HostStorageDirectoryIdentity>& public_identity,
    const std::filesystem::path& public_relative,
    const std::shared_ptr<const HostStorageDirectoryIdentity>& private_identity) {
  const auto& root = public_tree.identity;
  if (!S_ISDIR(root.st_mode) || uint64_t(root.st_ino) != plan.root->file ||
      uint32_t(root.st_mode) != plan.root->mode ||
      (public_tree.children.size() == plan.tops.size() && !ContentRecordedStamp(root, *plan.root))) {
    return Error(std::errc::state_not_recoverable);
  }
  if (guard.Conflicts(public_path, true, root.st_dev, root.st_ino, permit,
                      false, public_identity, public_relative)) {
    return Error(std::errc::device_or_resource_busy);
  }
  std::map<std::string, bool> seen;
  auto check = [&](const RemovalNode& node, bool staged) -> std::error_code {
    const auto found = plan.tops.find(node.name);
    const bool preserved = !plan.preserved.empty() && node.name == plan.preserved;
    if (found == plan.tops.end() || !seen.emplace(node.name, staged).second ||
        (staged && preserved) || (!staged && retire && !preserved)) {
      return Error(std::errc::state_not_recoverable);
    }
    auto error = RecordedContentTree(node, found->second, guard,
        (staged ? private_path : public_path) / node.name, permit, retire && !preserved,
        staged ? private_identity : public_identity,
        staged ? std::filesystem::path(node.name) : public_relative / node.name);
    if (!error && retire && !preserved) { error = ContentSurvivorDirectories(node, found->second); }
    return error;
  };
  for (const auto& node : public_tree.children) { if (auto error = check(node, false)) { return error; } }
  for (const auto& node : private_tops) { if (auto error = check(node, true)) { return error; } }
  for (const auto& [name, unused] : plan.tops) {
    const auto found = seen.find(name);
    if ((!retire || name == plan.preserved) && found == seen.end()) {
      return Error(std::errc::state_not_recoverable);
    }
  }
  return {};
}
// Fresh identity, data metadata and exact namespace check, using the retained
// native descriptors. No cooperating IO can enter while Mutation is owned.
std::error_code RevalidateContentTruncate(int parent, const RemovalNode& node,
                                         const MountIdentity& mount) {
  struct stat named, opened;
  if (fstatat(parent, node.name.c_str(), &named, AT_SYMLINK_NOFOLLOW) ||
      fstat(node.directory.value, &opened)) { return NativeError(); }
  if (!SameFlushTreeStat(named, node.identity) || !SameFlushTreeStat(opened, node.identity)) {
    return Error(std::errc::state_not_recoverable);
  }
  if (auto error = ConfinedMount(node.directory.value, mount)) { return error; }
  if (!S_ISDIR(node.identity.st_mode)) { return {}; }
  std::map<std::string, const RemovalNode*> expected;
  for (const auto& child : node.children) { expected.emplace(child.name, &child); }
  Fd scan(openat(node.directory.value, ".", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
  if (scan.value < 0) { return NativeError(); }
  DIR* stream = fdopendir(scan.value); if (!stream) { return NativeError(); } scan.Release();
  std::error_code error; size_t count = 0;
  while (true) {
    errno = 0; const auto* row = readdir(stream);
    if (!row) { if (errno) { error = NativeError(); } break; }
    if (!strcmp(row->d_name, ".") || !strcmp(row->d_name, "..")) { continue; }
    const auto found = expected.find(row->d_name);
    if (found == expected.end() || ++count > expected.size()) { error = Error(std::errc::state_not_recoverable); break; }
  }
  closedir(stream);
  if (!error && count != expected.size()) { error = Error(std::errc::state_not_recoverable); }
  for (const auto& child : node.children) {
    if (!error) { error = RevalidateContentTruncate(node.directory.value, child, mount); }
  }
  if (error) { return error; }
  if (fstatat(parent, node.name.c_str(), &named, AT_SYMLINK_NOFOLLOW) ||
      fstat(node.directory.value, &opened)) { return NativeError(); }
  return SameFlushTreeStat(named, node.identity) && SameFlushTreeStat(opened, node.identity)
      ? std::error_code{} : Error(std::errc::state_not_recoverable);
}
std::error_code RemoveContentTruncate(int parent, const RemovalNode& node,
                                     const MountIdentity& mount) {
  struct stat named, opened;
  if (fstatat(parent, node.name.c_str(), &named, AT_SYMLINK_NOFOLLOW) ||
      fstat(node.directory.value, &opened)) { return NativeError(); }
  if (!SameFlushTreeStat(named, node.identity) || !SameFlushTreeStat(opened, node.identity)) {
    return Error(std::errc::state_not_recoverable);
  }
  if (auto error = ConfinedMount(node.directory.value, mount)) { return error; }
  for (const auto& child : node.children) {
    if (auto error = RemoveContentTruncate(node.directory.value, child, mount)) { return error; }
  }
  // Own child removal changes directory timestamps/link count. Recheck its
  // identity/mode, and let native rmdir refuse any late unrecognized member.
  if (fstatat(parent, node.name.c_str(), &named, AT_SYMLINK_NOFOLLOW) ||
      fstat(node.directory.value, &opened)) { return NativeError(); }
  if (!SameNode(named, node.identity) || !SameNode(opened, node.identity) ||
      named.st_mode != node.identity.st_mode || opened.st_mode != node.identity.st_mode ||
      (!S_ISDIR(named.st_mode) && (!SameFlushTreeStat(named, node.identity) ||
                                   !SameFlushTreeStat(opened, node.identity)))) {
    return Error(std::errc::state_not_recoverable);
  }
  if (unlinkat(parent, node.name.c_str(), S_ISDIR(named.st_mode) ? AT_REMOVEDIR : 0) ||
      SyncStorage(parent)) { return NativeError(); }
  return {};
}
}
#endif
std::error_code HostStorageRoot::QuarantineContentPackage(
    const content_package_mutation_wire::Record& record,
    const HostStorageRoot& operation) const {
  const auto& permit = ContentPermit();
  if (xe::global_critical_region::is_held_by_current_thread() ||
      record.kind != content_package_mutation_wire::Kind::kDelete ||
      record.before.empty() || !permit || !permit->namespace_scope ||
      !permit->active.load(std::memory_order_acquire) ||
      permit->path != (path() / record.package).lexically_normal() ||
      operation.is_guest_private()) {
    return Error(std::errc::permission_denied);
  }
  auto error = operation.Validate();
  if (error) { return error; }
  if (operation.state_->space != state_->space) {
    return Error(std::errc::cross_device_link);
  }
  auto guard = Mutation(error);
  if (!guard) { return error; }
#if XE_PLATFORM_LINUX || XE_PLATFORM_MAC
  if (operation.state_->device != state_->device ||
      operation.state_->mount.id != state_->mount.id ||
      operation.state_->inode != record.op_directory) {
    return Error(std::errc::state_not_recoverable);
  }
  error = CheckGuestAccess(record.package, true);
  if (!error) { error = ContentPathCase(state_->directory, record.package); }
  if (!error) { error = OnlyContentPayload(operation.state_->directory); }
  if (error) { return error; }
  std::string leaf;
  auto parent = Parent(state_->directory, record.package, leaf, error);
  if (parent.value < 0) { return error; }
  struct stat source{}, staged{};
  const bool exists = fstatat(parent.value, leaf.c_str(), &source, AT_SYMLINK_NOFOLLOW) == 0;
  if (!exists && errno != ENOENT) { return NativeError(); }
  const bool moved = fstatat(operation.state_->directory, "payload", &staged, AT_SYMLINK_NOFOLLOW) == 0;
  if (!moved && errno != ENOENT) { return NativeError(); }
  if (exists == moved) { return Error(std::errc::state_not_recoverable); }
  RemovalNode actual;
  RemovalBudget budget;
  error = PreflightRemoval(exists ? parent.value : operation.state_->directory,
      exists ? leaf : "payload", true, state_->device, state_->mount,
      actual, &budget, exists ? this : nullptr);
  if (!error) {
    error = RecordedContentTree(actual, record.before, *guard,
        exists ? path() / record.package : operation.path() / "payload",
        permit, false, exists ? directory_identity() : operation.directory_identity(),
        exists ? record.package : std::filesystem::path("payload"));
  }
  if (error) { return error; }
  if (exists) {
    // No replacement even against a noncooperating host namespace writer.
#if XE_PLATFORM_LINUX && defined(SYS_renameat2)
    if (syscall(SYS_renameat2, parent.value, leaf.c_str(),
                operation.state_->directory, "payload", 1u)) { return NativeError(); }
#elif XE_PLATFORM_MAC && defined(RENAME_EXCL)
    if (renameatx_np(parent.value, leaf.c_str(), operation.state_->directory,
                     "payload", RENAME_EXCL)) { return NativeError(); }
#else
    return Error(std::errc::operation_not_supported);
#endif
    struct stat observed;
    if (fstatat(operation.state_->directory, "payload", &observed,
                 AT_SYMLINK_NOFOLLOW)) { return NativeError(); }
    if (!SameNode(observed, actual.identity)) {
      return Error(std::errc::state_not_recoverable);
    }
  }
  // Both sides of the rename must be durable before recording retirement.
  if (SyncStorage(parent.value) || SyncStorage(operation.state_->directory)) {
    return NativeError();
  }
  return {};
#else
  return Error(std::errc::operation_not_supported);
#endif
}
std::error_code HostStorageRoot::RetireContentPackage(
    const content_package_mutation_wire::Record& record,
    const HostStorageRoot& operation) const {
  const auto& permit = ContentPermit();
  if (xe::global_critical_region::is_held_by_current_thread() ||
      record.kind != content_package_mutation_wire::Kind::kDelete ||
      record.before.empty() || !permit || !permit->namespace_scope ||
      !permit->active.load(std::memory_order_acquire) ||
      permit->path != (path() / record.package).lexically_normal()) {
    return Error(std::errc::permission_denied);
  }
  auto error = operation.Validate();
  if (error) { return error; }
  if (operation.state_->space != state_->space) {
    return Error(std::errc::cross_device_link);
  }
  auto guard = Mutation(error);
  if (!guard) { return error; }
#if XE_PLATFORM_LINUX || XE_PLATFORM_MAC
  if (operation.state_->device != state_->device ||
      operation.state_->mount.id != state_->mount.id ||
      operation.state_->inode != record.op_directory) {
    return Error(std::errc::state_not_recoverable);
  }
  error = CheckGuestAccess(record.package, true);
  if (!error) { error = OnlyContentPayload(operation.state_->directory); }
  if (error) { return error; }
  std::string leaf;
  auto parent = Parent(state_->directory, record.package, leaf, error);
  if (parent.value < 0) { return error; }
  struct stat unexpected;
  if (!fstatat(parent.value, leaf.c_str(), &unexpected, AT_SYMLINK_NOFOLLOW)) {
    return Error(std::errc::state_not_recoverable);
  }
  if (errno != ENOENT) { return NativeError(); }
  struct stat staged;
  const bool exists = !fstatat(operation.state_->directory, "payload", &staged, AT_SYMLINK_NOFOLLOW);
  if (!exists && errno != ENOENT) { return NativeError(); }
  if (exists) {
    RemovalNode actual;
    RemovalBudget budget;
    error = PreflightRemoval(operation.state_->directory, "payload", true,
        state_->device, state_->mount, actual, &budget);
    if (!error) {
      error = RecordedContentTree(actual, record.before, *guard,
          operation.path() / "payload", permit, true,
          operation.directory_identity(), "payload");
    }
    if (!error) { error = RemovePrepared(operation.state_->directory, actual); }
    // Close retained O_PATH/regular/directory descriptors BEFORE returning a
    // completed retirement. Unlinked files may still consume blocks until then.
    actual = RemovalNode();
    if (error) { return error; }
  }
  if (SyncStorage(operation.state_->directory) || SyncStorage(parent.value)) {
    return NativeError();
  }
  return {};
#else
  return Error(std::errc::operation_not_supported);
#endif
}

std::error_code HostStorageRoot::QuarantineContentTruncate(
    const content_package_mutation_wire::Record& record,
    const HostStorageRoot& operation) const {
  return ContentTruncateInternal(record, operation, false);
}
std::error_code HostStorageRoot::RetireContentTruncate(
    const content_package_mutation_wire::Record& record,
    const HostStorageRoot& operation) const {
  return ContentTruncateInternal(record, operation, true);
}
std::error_code HostStorageRoot::ContentTruncateInternal(
    const content_package_mutation_wire::Record& record,
    const HostStorageRoot& operation, bool retire) const {
  const auto& permit = ContentPermit();
  if (xe::global_critical_region::is_held_by_current_thread() ||
      record.kind != content_package_mutation_wire::Kind::kTruncate ||
      record.package.native().size() > 256 ||
      !permit || !permit->namespace_scope || !permit->exclusive || permit->content_io ||
      !permit->active.load(std::memory_order_acquire) ||
      permit->path != (path() / record.package).lexically_normal() ||
      is_guest_private() || operation.is_guest_private() || operation.read_only()) {
    return Error(std::errc::permission_denied);
  }
  if (!record.before.empty() && !record.before.front().directory) {
    return Error(std::errc::operation_not_supported);  // Packed-file truncate needs its actual replacement protocol.
  }
  auto error = operation.PrivateDirectory();
  if (error) { return error; }
  if (operation.state_->space != state_->space) { return Error(std::errc::cross_device_link); }
  auto guard = Mutation(error);
  if (!guard) { return error; }
#if XE_PLATFORM_LINUX || XE_PLATFORM_MAC
  if (operation.state_->device != state_->device ||
      operation.state_->mount.id != state_->mount.id ||
      uint64_t(operation.state_->inode) != record.op_directory ||
      uint64_t(state_->inode) != record.identity.content_directory) {
    return Error(std::errc::state_not_recoverable);
  }
  ContentTruncatePlan plan;
  error = PlanContentTruncate(record, plan);
  if (error) { return error; }
  const auto physical = path() / record.package;
  RemovalNode public_tree;
  std::vector<RemovalNode> private_tops;
  Fd package_parent;
  std::string package_leaf;
  auto capture = [&]() -> std::error_code {
    public_tree = RemovalNode(); private_tops.clear();
    auto failure = Validate();
    if (!failure) { failure = operation.Validate(); }
    if (!failure) { failure = CheckGuestAccess(record.package, true); }
    if (!failure) { failure = ContentPathCase(state_->directory, record.package); }
    if (failure) { return failure; }
    package_parent = Parent(state_->directory, record.package, package_leaf, failure);
    if (package_parent.value < 0) { return failure; }
    struct stat op;
    if (fstat(operation.state_->directory, &op)) { return NativeError(); }
    if (!S_ISDIR(op.st_mode) || op.st_dev != state_->device ||
        uint64_t(op.st_ino) != record.op_directory || op.st_uid != geteuid() || (op.st_mode & 077)) {
      return Error(std::errc::state_not_recoverable);
    }
    if (auto current = ConfinedMount(operation.state_->directory, state_->mount)) { return current; }
    if (guard->Conflicts(operation.path(), true, op.st_dev, op.st_ino, permit,
                         false, operation.directory_identity(), {})) {
      return Error(std::errc::device_or_resource_busy);
    }
    RemovalBudget budget;
    failure = PreflightRemoval(package_parent.value, package_leaf, true, state_->device,
        state_->mount, public_tree, &budget, this);
    if (!failure) {
      failure = CaptureContentTruncatePrivate(operation.state_->directory, state_->device,
          state_->mount, budget, private_tops);
    }
    if (!failure) {
      failure = CheckContentTruncateInventory(plan, public_tree, private_tops,
          *guard, physical, operation.path(), permit, retire,
          directory_identity(), record.package, operation.directory_identity());
    }
    if (!failure) { failure = RevalidateContentTruncate(package_parent.value, public_tree, state_->mount); }
    for (const auto& top : private_tops) {
      if (!failure) { failure = RevalidateContentTruncate(operation.state_->directory, top, state_->mount); }
    }
    return failure;
  };
  // Protection publication uses existing registry synchronization, rather than
  // a new lock/rundown protocol. Native IDs also catch aliases opened before a
  // projection/purpose was registered. The operation's legitimate private
  // projection ancestor is not an original payload subtree.
  auto protected_access = [&]() -> std::error_code {
    auto purposes = HostStorageProtection::Private();
    std::vector<std::shared_ptr<const HostStorageRoot>> exclusions;
    {
      auto& all = HostStorageProtection::All(); std::lock_guard lock(all.mutex);
      size_t rows = 0;
      for (const auto& weak : all.projections) {
        if (++rows > 4096) { return Error(std::errc::value_too_large); }
        if (auto projection = weak.lock()) {
          for (const auto& item : projection->state_->exclusions) {
            if (item.private_tree) {
              if (exclusions.size() == 4096) { return Error(std::errc::value_too_large); }
              exclusions.push_back(item.root);
            }
          }
        }
      }
    }
    if (purposes.size() > 4096) { return Error(std::errc::value_too_large); }
    std::set<uint64_t> payload_directories;
    auto collect = [&](auto&& self, const RemovalNode& node) -> void {
      if (S_ISDIR(node.identity.st_mode)) { payload_directories.insert(uint64_t(node.identity.st_ino)); }
      for (const auto& child : node.children) { self(self, child); }
    };
    collect(collect, public_tree);
    for (const auto& node : private_tops) { collect(collect, node); }
    std::vector<uint64_t> public_ancestors, operation_ancestors;
    auto failure = DirectoryAncestors(public_tree.directory.value, state_->device, state_->mount, public_ancestors);
    if (!failure) {
      failure = DirectoryAncestors(operation.state_->directory, state_->device, state_->mount, operation_ancestors);
    }
    if (failure) { return failure; }
    auto check = [&](const std::shared_ptr<const HostStorageRoot>& root, bool purpose) -> std::error_code {
      if (root->state_->device != state_->device) { return {}; }
      if (auto current = root->Validate()) { return current; }
      const auto id = uint64_t(root->state_->inode);
      if (payload_directories.contains(id) ||
          std::find(public_ancestors.begin(), public_ancestors.end(), id) != public_ancestors.end() ||
          (purpose && std::find(operation_ancestors.begin(), operation_ancestors.end(), id) != operation_ancestors.end())) {
        return Error(std::errc::permission_denied);
      }
      return {};
    };
    for (const auto& root : purposes) { if (auto current = check(root, true)) { return current; } }
    for (const auto& root : exclusions) { if (auto current = check(root, false)) { return current; } }
    return {};
  };
  error = capture();
  if (!error) { error = protected_access(); }
  if (error) { return error; }
  Fd thumbnail;
  struct stat thumbnail_info{};
  if (!plan.preserved.empty()) {
    const auto found = std::find_if(public_tree.children.begin(), public_tree.children.end(),
        [&](const auto& node) { return node.name == plan.preserved; });
    if (found == public_tree.children.end()) { return Error(std::errc::state_not_recoverable); }
    thumbnail_info = found->identity;
    thumbnail = Fd(openat(public_tree.directory.value, plan.preserved.c_str(),
        O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC));
    struct stat opened;
    if (thumbnail.value < 0 || fstat(thumbnail.value, &opened)) { return NativeError(); }
    if (!S_ISREG(opened.st_mode) || !SameFlushTreeStat(opened, thumbnail_info)) {
      return Error(std::errc::state_not_recoverable);
    }
    if (auto current = ConfinedMount(thumbnail.value, state_->mount)) { return current; }
  }
  if (!retire) {
    for (const auto& node : public_tree.children) {
      if (node.name == plan.preserved) { continue; }
      error = RevalidateContentTruncate(public_tree.directory.value, node, state_->mount);
      if (error) { return error; }
#if XE_PLATFORM_LINUX && defined(SYS_renameat2)
      if (syscall(SYS_renameat2, public_tree.directory.value, node.name.c_str(),
                  operation.state_->directory, node.name.c_str(), 1u)) { return NativeError(); }
#elif XE_PLATFORM_MAC && defined(RENAME_EXCL)
      if (renameatx_np(public_tree.directory.value, node.name.c_str(),
                       operation.state_->directory, node.name.c_str(), RENAME_EXCL)) { return NativeError(); }
#else
      return Error(std::errc::operation_not_supported);
#endif
      struct stat moved;
      if (fstatat(operation.state_->directory, node.name.c_str(), &moved, AT_SYMLINK_NOFOLLOW)) { return NativeError(); }
      const auto& expected = plan.tops.at(node.name).front();
      if (!SameNode(moved, node.identity) || uint32_t(moved.st_mode) != expected.mode ||
          !ContentRecordedStamp(moved, expected)) { return Error(std::errc::state_not_recoverable); }
      if (SyncStorage(public_tree.directory.value) || SyncStorage(operation.state_->directory)) { return NativeError(); }
    }
  } else {
    // Entire surviving joint namespace has passed before the first unlink.
    for (const auto& node : private_tops) {
      if (auto current = RemoveContentTruncate(operation.state_->directory, node, state_->mount)) { return current; }
    }
  }
  if (thumbnail.value >= 0) {
    struct stat opened, named;
    if (fstat(thumbnail.value, &opened) || fstatat(public_tree.directory.value,
          plan.preserved.c_str(), &named, AT_SYMLINK_NOFOLLOW)) { return NativeError(); }
    if (!SameFlushTreeStat(opened, thumbnail_info) || !SameFlushTreeStat(named, thumbnail_info)) {
      return Error(std::errc::state_not_recoverable);
    }
    if (SyncStorage(thumbnail.value)) { return NativeError(); }
  }
  if (SyncStorage(public_tree.directory.value) || SyncStorage(operation.state_->directory)) { return NativeError(); }
  // Drop every old payload pin before reporting retirement or opening the final
  // inventory. Caller-owned tree/file pins remain the caller's responsibility.
  public_tree = RemovalNode(); private_tops.clear();
  error = capture();
  if (!error) { error = protected_access(); }
  if (!error && !retire) {
    // Quarantine must finish with precisely the retained thumbnail public.
    for (const auto& node : public_tree.children) {
      if (node.name != plan.preserved) { error = Error(std::errc::state_not_recoverable); break; }
    }
  }
  if (!error && retire && !private_tops.empty()) { error = Error(std::errc::state_not_recoverable); }
  if (!error && thumbnail.value >= 0) {
    struct stat opened, named;
    if (fstat(thumbnail.value, &opened) || fstatat(public_tree.directory.value,
          plan.preserved.c_str(), &named, AT_SYMLINK_NOFOLLOW)) { error = NativeError(); }
    else if (!SameFlushTreeStat(opened, thumbnail_info) || !SameFlushTreeStat(named, thumbnail_info)) {
      error = Error(std::errc::state_not_recoverable);
    }
  }
  return error;
#else
  return Error(std::errc::operation_not_supported);
#endif
}

}  // namespace xe::filesystem
