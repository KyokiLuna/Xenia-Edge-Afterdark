/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/vfs/entry.h"

#include <utility>

#include "xenia/base/filesystem.h"
#include "xenia/base/string.h"
#include "xenia/vfs/device.h"

namespace xe {
namespace vfs {

Entry::Entry(Device* device, Entry* parent, const std::string_view path)
    : device_(device),
      parent_(parent),
      path_(path),
      attributes_(0),
      size_(0),
      allocation_size_(0),
      create_timestamp_(0),
      access_timestamp_(0),
      write_timestamp_(0),
      delete_on_close_(false) {
  assert_not_null(device);
  absolute_path_ = xe::utf8::join_guest_paths(device->mount_path(), path);
  name_ = xe::utf8::find_name_from_guest_path(path);
}

Entry::~Entry() = default;

void Entry::Dump(xe::StringBuffer* string_buffer, int indent) {
  for (int i = 0; i < indent; ++i) {
    string_buffer->Append(' ');
  }
  string_buffer->Append(name());
  string_buffer->Append('\n');
  for (auto& child : children_) {
    child->Dump(string_buffer, indent + 2);
  }
}

bool Entry::is_read_only() const { return device_->is_read_only(); }

uint32_t Entry::sectors_per_allocation_unit() const {
  return device_->sectors_per_allocation_unit();
}

Entry* Entry::GetChild(const std::string_view name) {
  auto global_lock = global_critical_region_.Acquire();
  auto it =
      std::ranges::find_if(std::as_const(children_), [&](const auto& child) {
        return xe::utf8::equal_case(child->name(), name);
      });
  if (it == children_.cend()) {
    return nullptr;
  }
  return (*it).get();
}

Entry* Entry::ResolvePath(const std::string_view path) {
  // Walk the path, one separator at a time.
  Entry* entry = this;
  for (auto& part : xe::utf8::split_path(path)) {
    entry = entry->GetChild(part);
    if (!entry) {
      // Not found.
      return nullptr;
    }
  }
  return entry;
}

Entry* Entry::IterateChildren(const xe::filesystem::WildcardEngine& engine,
                              size_t* current_index) {
  auto global_lock = global_critical_region_.Acquire();
  while (*current_index < children_.size()) {
    auto& child = children_[*current_index];
    *current_index = *current_index + 1;
    if (engine.Match(child->name())) {
      return child.get();
    }
  }
  return nullptr;
}

Entry* Entry::CreateEntry(const std::string_view name, uint32_t attributes) {
  auto global_lock = global_critical_region_.Acquire();
  if (is_read_only()) {
    return nullptr;
  }
  if (GetChild(name)) {
    // Already exists.
    return nullptr;
  }
  auto entry = CreateEntryInternal(name, attributes);
  if (!entry) {
    return nullptr;
  }
  children_.push_back(std::move(entry));
  // TODO(benvanik): resort? would break iteration?
  Touch();
  return children_.back().get();
}

bool Entry::Delete(Entry* entry) {
  auto global_lock = global_critical_region_.Acquire();
  if (is_read_only()) {
    return false;
  }
  if (!entry->allows_namespace_mutation() || entry->has_open_files()) {
    return false;
  }
  if (entry->parent() != this) {
    return false;
  }
  if (!DeleteEntryInternal(entry)) {
    return false;
  }
  for (auto it = children_.begin(); it != children_.end(); ++it) {
    if (it->get() == entry) {
      children_.erase(it);
      break;
    }
  }
  Touch();
  return true;
}

bool Entry::Delete() {
  assert_not_null(parent_);
  return parent_->Delete(this);
}

void Entry::Touch() {
  // TODO(benvanik): update timestamps.
}

X_STATUS Entry::Rename(
    const std::filesystem::path file_path, bool replace_existing,
    std::shared_ptr<const filesystem::HostStorageFileLease> permitted) {
  auto global_lock = global_critical_region_.Acquire();
  // Store the string to ensure string_views from split_path remain valid
  const std::string path_str = xe::path_to_utf8(file_path);
  std::vector<std::string_view> path_parts = xe::utf8::split_path(path_str);
  if (path_parts.size() < 2) {
    return X_STATUS_OBJECT_NAME_INVALID;
  }

  // Remove the root (e.g., "cache:")
  path_parts.erase(path_parts.begin());

  const std::string guest_path = xe::utf8::join_guest_paths(path_parts);
  return Rename(
      device_->ResolvePath(xe::utf8::find_base_guest_path(guest_path)),
      path_parts.back(), replace_existing, std::move(permitted));
}

X_STATUS Entry::Rename(
    Entry* new_parent, std::string_view name, bool replace_existing,
    std::shared_ptr<const filesystem::HostStorageFileLease> permitted) {
  auto global_lock = global_critical_region_.Acquire();
  if (!allows_namespace_mutation() || is_read_only() ||
      (new_parent && new_parent->is_read_only())) {
    return X_STATUS_ACCESS_DENIED;
  }
  if (!parent_) {
    return X_STATUS_OBJECT_NAME_INVALID;
  }
  if (!new_parent || !(new_parent->attributes() & kFileAttributeDirectory)) {
    return X_STATUS_OBJECT_PATH_NOT_FOUND;
  }
  if (new_parent->device() != device_) {
    return X_STATUS_NOT_SAME_DEVICE;
  }
  if (name.empty() || name == "." || name == ".." ||
      name.find_first_of("\\/") != std::string_view::npos) {
    return X_STATUS_OBJECT_NAME_INVALID;
  }
  for (Entry* ancestor = new_parent; ancestor; ancestor = ancestor->parent_) {
    if (ancestor == this) {
      return X_STATUS_INVALID_PARAMETER;
    }
  }
  // The caller may have borrowed a target Entry's name; replacing that target
  // must not invalidate the requested spelling before we commit the tree.
  std::string new_name(name);

  // Renaming to its own name fails too, even a change of case only.
  Entry* existing = new_parent->GetChild(new_name);
  if (existing) {
    if (!replace_existing ||
        (existing->attributes() & kFileAttributeDirectory)) {
      return X_STATUS_OBJECT_NAME_COLLISION;
    }
    if (existing == this || !existing->allows_namespace_mutation() ||
        existing->has_open_files()) {
      return X_STATUS_ACCESS_DENIED;
    }
  }

  struct RebasedEntry {
    Entry* entry;
    std::string path, absolute_path;
  };
  std::vector<RebasedEntry> paths;
  auto prepare = [&](auto&& self, Entry* entry, std::string path) -> void {
    paths.push_back(
        {entry, path, xe::utf8::join_guest_paths(device_->mount_path(), path)});
    for (const auto& child : entry->children_) {
      self(self, child.get(), xe::utf8::join_guest_paths(path, child->name_));
    }
  };
  prepare(prepare, this,
          xe::utf8::join_guest_paths(new_parent->path(), new_name));
  // Perform allocations before native mutation; installing prepared strings
  // and transferring existing ownership then cannot leave a half-rebased tree.
  if (new_parent != parent_) {
    new_parent->children_.reserve(new_parent->children_.size() + 1);
  }

  const auto result =
      RenameEntryInternal(new_parent, new_name, std::move(permitted));
  if (XFAILED(result)) {
    return result;
  }

  if (existing) {
    std::erase_if(new_parent->children_, [existing](const auto& child) {
      return child.get() == existing;
    });
  }
  if (new_parent != parent_) {
    auto it = std::ranges::find(parent_->children_, this,
                                &std::unique_ptr<Entry>::get);
    new_parent->children_.push_back(std::move(*it));
    parent_->children_.erase(it);
    parent_ = new_parent;
  }
  for (auto& rebased : paths) {
    rebased.entry->path_.swap(rebased.path);
    rebased.entry->absolute_path_.swap(rebased.absolute_path);
  }
  name_.swap(new_name);
  return X_STATUS_SUCCESS;
}

}  // namespace vfs
}  // namespace xe
