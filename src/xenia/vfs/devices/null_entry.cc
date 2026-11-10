/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2021 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/vfs/devices/null_entry.h"

#include "xenia/base/filesystem.h"
#include "xenia/base/logging.h"
#include "xenia/base/mapped_memory.h"
#include "xenia/base/math.h"
#include "xenia/base/string.h"
#include "xenia/vfs/device.h"
#include "xenia/vfs/devices/null_device.h"
#include "xenia/vfs/devices/null_file.h"

namespace xe {
namespace vfs {

NullEntry::NullEntry(Device* device, Entry* parent, std::string path)
    : Entry(device, parent, path) {}

bool NullEntry::has_open_files() const {
  if (open_files_.load()) {
    return true;
  }
  for (const auto& child : children()) {
    if (child->has_open_files()) {
      return true;
    }
  }
  return false;
}

NullEntry::~NullEntry() = default;

NullEntry* NullEntry::Create(Device* device, Entry* parent,
                             const std::string& path) {
  auto entry = new NullEntry(device, parent, path);

  entry->create_timestamp_ = 0;
  entry->access_timestamp_ = 0;
  entry->write_timestamp_ = 0;

  entry->attributes_ = kFileAttributeNormal;

  entry->size_ = 0;
  entry->allocation_size_ = 0;
  return entry;
}

X_STATUS NullEntry::Open(uint32_t desired_access, File** out_file) {
  auto global_lock = global_critical_region_.Acquire();
  if (!out_file) {
    return X_STATUS_INVALID_PARAMETER;
  }
  if (!static_cast<NullDevice*>(device())->available()) {
    return X_STATUS(0xC000009D);
  }
  if (is_read_only() && (desired_access & (FileAccess::kFileWriteData |
                                           FileAccess::kFileAppendData))) {
    XELOGE("Attempting to open file for write access on read-only device");
    return X_STATUS_ACCESS_DENIED;
  }

  *out_file = new NullFile(desired_access, this);
  return X_STATUS_SUCCESS;
}

bool NullEntry::DeleteEntryInternal(Entry* entry) { return false; }

}  // namespace vfs
}  // namespace xe
