/** Xenia : Xbox 360 Emulator Research Project; BSD license, see LICENSE. */
#include "xenia/vfs/module_resource.h"
#include <memory>
#include <new>
#include "xenia/base/mutex.h"
#include "xenia/base/xui_resource.h"
#include "xenia/vfs/entry.h"
#include "xenia/vfs/file.h"
#include "xenia/vfs/virtual_file_system.h"
#include "xenia/vfs/xex_metadata.h"
namespace xe::vfs {
X_STATUS ReadModuleResource(VirtualFileSystem& fs,
                            std::string_view guest_module,
                            std::string_view section, std::string_view member,
                            std::vector<uint8_t>& output,
                            uint32_t maximum_resource_bytes) {
  if (global_critical_region::is_held_by_current_thread() ||
      guest_module.empty() || guest_module.size() > 4096 || section.empty() ||
      section.size() > 8 || !maximum_resource_bytes ||
      maximum_resource_bytes > 16u * 1024u * 1024u) {
    return X_STATUS_INVALID_PARAMETER;
  }
  try {
    File* opened = nullptr;
    FileAction action;
    const auto status =
        fs.OpenFile(nullptr, guest_module, FileDisposition::kOpen,
                    FileAccess::kGenericRead, false, true, &opened, &action);
    if (XFAILED(status)) {
      return status;
    }
    std::unique_ptr<File, void (*)(File*)> file(opened, [](File* f) {
      if (f) {
        f->Destroy();
      }
    });
    // NamespaceV2 open-time ownership must precede entry metadata access.
    if (!file || !file->retained_device()) {
      return X_STATUS_NOT_SUPPORTED;
    }
    size_t size;
    {
      global_critical_region lock;
      auto guard = lock.Acquire();
      size = file->entry()->size();
    }
    if (!size) {
      return X_STATUS_INVALID_IMAGE_FORMAT;
    }
    if (size > kMaxOfflineXexFileBytes) {
      return X_STATUS_INSUFFICIENT_RESOURCES;
    }
    std::vector<uint8_t> xex(size);
    size_t read = 0;
    const auto copied = file->ReadSync(xex, 0, &read);
    if (XFAILED(copied)) {
      return copied;
    }
    if (read != size) {
      return X_STATUS_END_OF_FILE;
    }
    auto extracted = ExtractXexResource(xex, section);
    if (extracted.status == XexResourceStatus::kNotFound) {
      return X_STATUS_NOT_FOUND;
    }
    if (extracted.status == XexResourceStatus::kTooLarge) {
      return X_STATUS_INSUFFICIENT_RESOURCES;
    }
    if (extracted.status != XexResourceStatus::kLoaded) {
      return X_STATUS_INVALID_IMAGE_FORMAT;
    }
    auto& resource = extracted.bytes;
    if (member.empty()) {
      if (resource.size() > maximum_resource_bytes) {
        return X_STATUS_BUFFER_TOO_SMALL;
      }
      output = std::move(resource);
    } else {
      const auto found = xe::resource::LookupXuiPackageFile(resource, member);
      if (found.status == xe::resource::XuiPackageStatus::kNotFound) {
        return X_STATUS_NOT_FOUND;
      }
      if (found.status == xe::resource::XuiPackageStatus::kUnsupported) {
        return X_STATUS_NOT_SUPPORTED;
      }
      if (found.status != xe::resource::XuiPackageStatus::kFound) {
        return X_STATUS_INVALID_IMAGE_FORMAT;
      }
      if (found.bytes.size() > maximum_resource_bytes) {
        return X_STATUS_BUFFER_TOO_SMALL;
      }
      std::vector<uint8_t> staged(found.bytes.begin(), found.bytes.end());
      output = std::move(staged);
    }
    return X_STATUS_SUCCESS;
  } catch (const std::bad_alloc&) {
    return X_STATUS_NO_MEMORY;
  }
}
}  // namespace xe::vfs
