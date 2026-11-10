#ifndef XENIA_VFS_HOST_STORAGE_ERRORS_H_
#define XENIA_VFS_HOST_STORAGE_ERRORS_H_
#include <cerrno>
#include <system_error>
#include "xenia/base/platform.h"
#include "xenia/xbox.h"
#if XE_PLATFORM_WIN32
#include "xenia/base/platform_win.h"
#endif
namespace xe::vfs {
inline X_STATUS HostStorageError(const std::error_code& error) {
  if (!error) {
    return X_STATUS_SUCCESS;
  }
#if XE_PLATFORM_WIN32
  if (error.category() == std::system_category() &&
      error.value() == ERROR_HANDLE_EOF) {
    return X_STATUS_END_OF_FILE;
  }
#endif
  if (error == std::errc::no_space_on_device) {
    return X_STATUS_DISK_FULL;
  }
#ifdef EDQUOT
  if (error.category() == std::generic_category() && error.value() == EDQUOT) {
    return X_STATUS_DISK_FULL;
  }
#endif
  if (error == std::errc::permission_denied ||
      error == std::errc::read_only_file_system ||
      error == std::errc::too_many_symbolic_link_levels) {
    return X_STATUS_ACCESS_DENIED;
  }
  if (error == std::errc::no_such_file_or_directory) {
    return X_STATUS_OBJECT_NAME_NOT_FOUND;
  }
  if (error == std::errc::not_a_directory) {
    return X_STATUS_OBJECT_PATH_NOT_FOUND;
  }
  if (error == std::errc::is_a_directory) {
    return X_STATUS_FILE_IS_A_DIRECTORY;
  }
  if (error == std::errc::file_exists) {
    return X_STATUS_OBJECT_NAME_COLLISION;
  }
  if (error == std::errc::invalid_argument ||
      error == std::errc::value_too_large ||
      error == std::errc::file_too_large) {
    return X_STATUS_INVALID_PARAMETER;
  }
  if (error == std::errc::device_or_resource_busy ||
      error == std::errc::text_file_busy) {
    return X_STATUS(0xC0000043);
  }
  if (error == std::errc::cross_device_link) {
    return X_STATUS_NOT_SAME_DEVICE;
  }
  if (error == std::errc::operation_not_supported) {
    return X_STATUS_NOT_SUPPORTED;
  }
  // Existing xboxkrnl_error.cc maps C000009D to DEVICE_NOT_CONNECTED.
  if (error == std::errc::no_such_device ||
      error == std::errc::no_such_device_or_address) {
    return X_STATUS(0xC000009D);
  }
  return X_STATUS_UNSUCCESSFUL;
}
}  // namespace xe::vfs
#endif
