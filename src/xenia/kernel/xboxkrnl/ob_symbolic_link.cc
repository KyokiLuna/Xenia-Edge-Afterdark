/** Xenia : Xbox 360 Emulator Research Project; BSD license, see LICENSE. */
#include "xenia/kernel/xboxkrnl/ob_symbolic_link.h"
#include <array>
#include <span>
#include <utility>
#include <vector>
#include "xenia/base/byte_order.h"
#include "xenia/base/mutex.h"
#include "xenia/base/utf8.h"
#include "xenia/kernel/util/guest_io_buffer.h"
#include "xenia/memory.h"
#include "xenia/vfs/virtual_file_system.h"
namespace xe::kernel::xboxkrnl {
X_STATUS TranslateSymbolicLinkGuest(
    Memory& memory, vfs::VirtualFileSystem& fs, uint32_t attributes,
    uint32_t output_ansi, const SymbolicLinkRootResolver& resolve_root) {
  if (global_critical_region::is_held_by_current_thread()) {
    return X_STATUS_INVALID_DEVICE_REQUEST;
  }
  if (!attributes || !output_ansi) {
    return X_STATUS_INVALID_PARAMETER;
  }
  std::array<uint8_t, 12> oa{};
  std::array<uint8_t, 8> input{}, output{};
  auto status = util::ReadGuestIoBuffer(memory, attributes, oa);
  if (XFAILED(status)) {
    return status;
  }
  const uint32_t root = xe::load_and_swap<uint32_t>(oa.data());
  const uint32_t name = xe::load_and_swap<uint32_t>(oa.data() + 4);
  const uint32_t flags = xe::load_and_swap<uint32_t>(oa.data() + 8);
  // Actual callers use OBJ_CASE_INSENSITIVE. Zero is accepted consistently
  // with existing case-insensitive VFS behavior; other object flags are not
  // silently reinterpreted as a namespace or ownership grant.
  if (flags & ~uint32_t(0x40)) {
    return X_STATUS_INVALID_PARAMETER;
  }
  if (!name && (root == 0 || root == 0xFFFFFFFD)) {
    return X_STATUS_OBJECT_NAME_INVALID;
  }
  if (name) {
    status = util::ReadGuestIoBuffer(memory, name, input);
    if (XFAILED(status)) {
      return status;
    }
  }
  status = util::ReadGuestIoBuffer(memory, output_ansi, output);
  if (XFAILED(status)) {
    return status;
  }
  const uint16_t length = xe::load_and_swap<uint16_t>(input.data());
  const uint16_t input_capacity = xe::load_and_swap<uint16_t>(input.data() + 2);
  const uint32_t input_buffer = xe::load_and_swap<uint32_t>(input.data() + 4);
  const uint16_t capacity = xe::load_and_swap<uint16_t>(output.data() + 2);
  const uint32_t buffer = xe::load_and_swap<uint32_t>(output.data() + 4);
  if (length > input_capacity || (!input_buffer && length) || !capacity ||
      !buffer) {
    return X_STATUS_INVALID_PARAMETER;
  }
  if (!util::GuestIoRangeValid(memory, output_ansi, 8, true) ||
      !util::GuestIoRangeValid(memory, buffer, capacity, true)) {
    return X_STATUS_ACCESS_VIOLATION;
  }
  // Snapshot all inputs before any output publication. In-place payloads are
  // safe, but descriptor/control overlap has no unambiguous ownership.
  for (const auto [address, bytes] :
       {std::pair{output_ansi, 8u}, std::pair{buffer, uint32_t(capacity)}}) {
    if (util::GuestIoRangesOverlap(memory, address, bytes, attributes, 12) ||
        (name && util::GuestIoRangesOverlap(memory, address, bytes, name, 8))) {
      return X_STATUS_INVALID_PARAMETER;
    }
  }
  if (util::GuestIoRangesOverlap(memory, buffer, capacity, output_ansi, 8)) {
    return X_STATUS_INVALID_PARAMETER;
  }
  std::vector<uint8_t> bytes(length);
  status = util::ReadGuestIoBuffer(memory, input_buffer, bytes);
  if (XFAILED(status)) {
    return status;
  }
  std::string path(bytes.begin(), bytes.end());
  if (path.find('\0') != std::string::npos) {
    return X_STATUS_OBJECT_NAME_INVALID;
  }
  RetainedSymbolicLinkRoot retained;
  if (root != 0 && root != 0xFFFFFFFD) {
    if (!resolve_root) {
      return X_STATUS_INVALID_HANDLE;
    }
    status = resolve_root(root, retained);
    if (XFAILED(status)) {
      return status;
    }
    if (!retained.owner || retained.guest_path.empty()) {
      return X_STATUS_INVALID_HANDLE;
    }
    if (!path.empty() && !retained.directory) {
      return X_STATUS_NOT_A_DIRECTORY;
    }
    if (!path.empty() &&
        (path.front() == '\\' || path.find(':') != std::string::npos)) {
      return X_STATUS_OBJECT_NAME_INVALID;
    }
    path =
        path.empty() ? retained.guest_path : retained.guest_path + "\\" + path;
  } else if (path.empty()) {
    return X_STATUS_OBJECT_NAME_INVALID;
  }
  std::string translated;
  status = fs.TranslateSymbolicPath(path, translated);
  if (XFAILED(status)) {
    return status;
  }
  if (translated.size() > capacity) {
    return X_STATUS_BUFFER_TOO_SMALL;
  }
  xe::store_and_swap<uint16_t>(output.data(), uint16_t(translated.size()));
  const auto payload = std::span<const uint8_t>(
      reinterpret_cast<const uint8_t*>(translated.data()), translated.size());
  const std::array<util::GuestIoWrite, 2> writes{
      {{buffer, payload}, {output_ansi, output}}};
  // The complete original descriptor is republished with only length changed,
  // so all eight bytes remain checked through the final provider wait.
  return util::WriteGuestIoBuffers(memory, writes);
}
}  // namespace xe::kernel::xboxkrnl
