/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2024 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xam/xdbf/gpd_info.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xam/user_settings.h"

namespace xe {
namespace kernel {
namespace xam {

GpdInfo::GpdInfo() : XdbfFile(), title_id_(-1) {}
GpdInfo::GpdInfo(const uint32_t title_id) : XdbfFile(), title_id_(title_id) {
  header_.entry_count = 1 * base_entry_count;
  header_.free_count = 1 * base_entry_count;
  header_.free_used = 1;

  // Add free entry at the end
  XdbfFileLoc loc;
  loc.size = 0xFFFFFFFF;
  loc.offset = 0;

  free_entries_.push_back(loc);
}

GpdInfo::GpdInfo(const uint32_t title_id, const std::vector<uint8_t> buffer)
    : XdbfFile({buffer.data(), buffer.size()}), title_id_(title_id) {}

std::span<const uint8_t> GpdInfo::GetImage(uint64_t id) const {
  const Entry* entry = GetEntry(static_cast<uint16_t>(GpdSection::kImage), id);

  if (!entry) {
    return {};
  }

  return {entry->data.data(), entry->data.size()};
}

void GpdInfo::AddImage(uint32_t id, std::span<const uint8_t> image_data) {
  Entry* entry = GetEntry(static_cast<uint16_t>(GpdSection::kImage), id);

  if (entry || !image_data.size()) {
    return;
  }

  Entry new_entry(id, static_cast<uint16_t>(GpdSection::kImage),
                  static_cast<uint32_t>(image_data.size()));

  memcpy(new_entry.data.data(), image_data.data(), image_data.size());

  UpsertEntry(&new_entry);
}

Entry* GpdInfo::GetSettingEntry(uint32_t id) {
  Entry* entry = GetEntry(static_cast<uint16_t>(GpdSection::kSetting), id);

  if (!entry || entry->data.size() < sizeof(X_XDBF_GPD_SETTING_HEADER)) {
    return nullptr;
  }

  return entry;
}

X_XDBF_GPD_SETTING_HEADER* GpdInfo::GetSetting(uint32_t id) {
  Entry* entry = GetSettingEntry(id);

  if (!entry) {
    return nullptr;
  }

  return reinterpret_cast<X_XDBF_GPD_SETTING_HEADER*>(entry->data.data());
}

const X_XDBF_GPD_SETTING_HEADER* GpdInfo::GetSetting(uint32_t id) const {
  const auto* entry = GetEntry(uint16_t(GpdSection::kSetting), id);
  return entry && entry->data.size() >= sizeof(X_XDBF_GPD_SETTING_HEADER)
             ? reinterpret_cast<const X_XDBF_GPD_SETTING_HEADER*>(
                   entry->data.data())
             : nullptr;
}

std::span<const uint8_t> GpdInfo::GetSettingData(uint32_t id) const {
  const Entry* entry = GetEntry(uint16_t(GpdSection::kSetting), id);

  if (!entry || entry->data.size() < sizeof(X_XDBF_GPD_SETTING_HEADER)) {
    return {};
  }

  const auto setting =
      reinterpret_cast<const X_XDBF_GPD_SETTING_HEADER*>(entry->data.data());

  if (setting->setting_type != X_USER_DATA_TYPE::BINARY &&
      setting->setting_type != X_USER_DATA_TYPE::WSTRING) {
    return {};
  }

  // The declared size comes from the file, clamp it to what the entry holds
  const size_t available =
      entry->data.size() - sizeof(X_XDBF_GPD_SETTING_HEADER);
  const size_t size =
      std::min(static_cast<size_t>(setting->base_data.binary.size), available);

  return {entry->data.data() + sizeof(X_XDBF_GPD_SETTING_HEADER), size};
}

void GpdInfo::UpsertSetting(const UserSetting* setting_data) {
  const auto serialized_data = setting_data->Serialize();

  Entry new_entry(setting_data->get_setting_id(),
                  static_cast<uint16_t>(GpdSection::kSetting),
                  static_cast<uint32_t>(serialized_data.size()));

  memcpy(new_entry.data.data(), serialized_data.data(), serialized_data.size());

  UpsertEntry(&new_entry);
}

std::error_code GpdInfo::UpsertSettingPreserving(const UserSetting& setting) {
  const uint32_t id = setting.get_setting_id();
  const auto type = UserData::get_type(id);
  if (!setting.is_valid_type() || setting.get_type() != type ||
      (setting.requires_additional_data() &&
       (setting.get_extended_data().size() > UserData::get_max_size(id) ||
        (type == X_USER_DATA_TYPE::WSTRING &&
         (setting.get_extended_data().size() & 1))))) {
    return std::make_error_code(std::errc::invalid_argument);
  }
  std::vector<uint8_t> bytes = setting.Serialize();
  const auto* submitted =
      reinterpret_cast<const X_XDBF_GPD_SETTING_HEADER*>(bytes.data());
  if (submitted->RequiresBuffer() && size_t(submitted->base_data.binary.size) !=
                                         setting.get_extended_data().size()) {
    return std::make_error_code(std::errc::invalid_argument);
  }
  auto* prior = GetEntry(static_cast<uint16_t>(GpdSection::kSetting), id);
  if (prior) {
    std::optional<UserSetting> checked;
    if (UserSetting::ReadFromGpd(*this, id, checked) != X_ERROR_SUCCESS ||
        !checked || prior->data.size() < sizeof(X_XDBF_GPD_SETTING_HEADER)) {
      return std::make_error_code(std::errc::bad_message);
    }
    X_XDBF_GPD_SETTING_HEADER original;
    memcpy(&original, prior->data.data(), sizeof(original));
    const size_t known =
        sizeof(original) + (original.RequiresBuffer()
                                ? size_t(original.base_data.binary.size)
                                : 0);
    if (known > prior->data.size()) {
      return std::make_error_code(std::errc::bad_message);
    }
    // Capture every potentially relocating byte BEFORE UpsertEntry.
    bytes.insert(bytes.end(), prior->data.begin() + known, prior->data.end());
    auto* next = reinterpret_cast<X_XDBF_GPD_SETTING_HEADER*>(bytes.data());
    next->unknown_1 = original.unknown_1;
    memcpy(next->unknown, original.unknown, sizeof(next->unknown));
    if (original.setting_type == type &&
        (type == X_USER_DATA_TYPE::INT32 || type == X_USER_DATA_TYPE::FLOAT ||
         type == X_USER_DATA_TYPE::CONTEXT ||
         type == X_USER_DATA_TYPE::BINARY ||
         type == X_USER_DATA_TYPE::WSTRING)) {
      memcpy(reinterpret_cast<uint8_t*>(&next->base_data) + 4,
             reinterpret_cast<const uint8_t*>(&original.base_data) + 4, 4);
    }
  }
  if (bytes.size() > UINT32_MAX) {
    return std::make_error_code(std::errc::file_too_large);
  }
  Entry replacement(id, static_cast<uint16_t>(GpdSection::kSetting),
                    static_cast<uint32_t>(bytes.size()));
  replacement.data = std::move(bytes);
  UpsertEntry(&replacement);
  return {};
}

std::u16string GpdInfo::GetString(uint32_t id) const {
  const Entry* entry = GetEntry(static_cast<uint16_t>(GpdSection::kString), id);
  if (!entry) {
    return {};
  }

  return string_util::read_u16string_and_swap(
      reinterpret_cast<const char16_t*>(entry->data.data()),
      entry->data.size() / sizeof(char16_t));
}

void GpdInfo::AddString(uint32_t id, std::u16string string_data) {
  // Check if the string already exists with the same value
  const Entry* existing_entry =
      GetEntry(static_cast<uint16_t>(GpdSection::kString), id);
  if (existing_entry) {
    std::u16string existing_string = string_util::read_u16string_and_swap(
        reinterpret_cast<const char16_t*>(existing_entry->data.data()),
        existing_entry->data.size() / sizeof(char16_t));
    if (existing_string == string_data) {
      // String hasn't changed, no need to update
      return;
    }
  }

  const uint32_t entry_size =
      static_cast<uint32_t>(string_util::size_in_bytes(string_data));

  Entry new_entry(id, static_cast<uint16_t>(GpdSection::kString), entry_size);

  string_util::copy_and_swap_truncating(
      reinterpret_cast<char16_t*>(new_entry.data.data()), string_data,
      string_data.length() + 1);

  UpsertEntry(&new_entry);
}

std::error_code GpdInfo::SerializeChecked(std::vector<uint8_t>& output,
    uint64_t maximum) const {
  const auto bad = std::make_error_code(std::errc::bad_message);
  if (header_.magic != kXdbfSignatureXdbf || header_.version != 0x10000 ||
      entries_.size() != header_.entry_used ||
      entries_.size() > header_.entry_count || free_entries_.empty() ||
      free_entries_.size() != header_.free_used ||
      free_entries_.size() > header_.free_count) { return bad; }
  const uint64_t eof = free_entries_.back().offset;
  const uint64_t tail_size = free_entries_.back().size;
  // FindFreeLocation advances offset and subtracts the same amount from
  // remaining size. The terminal range ends at UINT32_MAX; only an empty
  // producer starts with size==UINT32_MAX. Widen BEFORE adding, no wrap.
  if (eof + tail_size != UINT32_MAX) { return bad; }
  const uint64_t total = sizeof(XdbfHeader) +
      uint64_t(header_.entry_count) * sizeof(XdbfEntry) +
      uint64_t(header_.free_count) * sizeof(XdbfFileLoc) + eof;
  // The unchanged serializer uses uint32_t for all these intermediates.
  if (total > maximum || total > UINT32_MAX || total > SIZE_MAX) {
    return std::make_error_code(std::errc::file_too_large);
  }
  for (const auto& entry : entries_) {
    const uint64_t offset = entry.info.offset, length = entry.info.size;
    if (length != entry.data.size() || offset > eof || length > eof - offset) {
      return bad;
    }
  }
  for (size_t i = 0; i + 1 < free_entries_.size(); ++i) {
    const uint64_t offset = free_entries_[i].offset;
    const uint64_t length = free_entries_[i].size;
    if (length == UINT32_MAX || offset > eof || length > eof - offset) {
      return bad;
    }
  }
  auto temporary = Serialize();
  if (temporary.size() != total) { return bad; }
  output = std::move(temporary);
  return {};
}

std::vector<uint8_t> GpdInfo::Serialize() const {
  // Resize to proper size.
  const uint32_t entries_table_size = sizeof(XdbfEntry) * header_.entry_count;
  const uint32_t free_table_size = sizeof(XdbfFileLoc) * header_.free_count;

  const uint32_t gpd_size = sizeof(XdbfHeader) + entries_table_size +
                            free_table_size + CalculateEntriesSize();

  std::vector<uint8_t> data(gpd_size);

  // Header part
  uint8_t* write_ptr = data.data();
  // Write header
  memcpy(write_ptr, &header_, sizeof(XdbfHeader));
  write_ptr += sizeof(XdbfHeader);

  // Entries in XDBF are sorted by section lowest-to-highest
  std::vector<const Entry*> entries = GetSortedEntries();

  for (const auto& entry : entries) {
    memcpy(write_ptr, &entry->info, sizeof(XdbfEntry));
    write_ptr += sizeof(XdbfEntry);
  }

  const auto empty_entries_count = header_.entry_count - entries.size();
  // Set remaining bytes to 0
  write_ptr =
      std::fill_n(write_ptr, empty_entries_count * sizeof(XdbfEntry), 0);

  // Free header part
  for (const auto& entry : free_entries_) {
    memcpy(write_ptr, &entry, sizeof(XdbfFileLoc));
    write_ptr += sizeof(XdbfFileLoc);
  }

  const auto empty_free_entries_count =
      header_.free_count - free_entries_.size();
  write_ptr =
      std::fill_n(write_ptr, empty_free_entries_count * sizeof(XdbfFileLoc), 0);

  // Entries data
  for (const auto& entry : entries) {
    if (!entry->info.size) {
      continue;
    }

    memcpy(write_ptr + entry->info.offset, entry->data.data(),
           entry->data.size());
  }
  return data;
}

bool GpdInfo::IsSyncEntry(const Entry* const entry) {
  return entry->info.id == 0x100000000 || entry->info.id == 0x200000000;
}

bool GpdInfo::IsEntryOfSection(const Entry* const entry,
                               const GpdSection section) {
  return entry->info.section == static_cast<uint16_t>(section);
}

void GpdInfo::UpsertEntry(Entry* updated_entry) {
  auto entry = GetEntry(updated_entry->info.section, updated_entry->info.id);
  if (entry) {
    DeleteEntry(entry);
  }
  InsertEntry(updated_entry);
}

uint32_t GpdInfo::FindFreeLocation(const uint32_t entry_size) {
  assert_false(free_entries_.empty());

  uint32_t offset = free_entries_.back().offset;

  auto itr = std::ranges::find_if(
      free_entries_,
      [entry_size](XdbfFileLoc entry) { return entry.size == entry_size; });

  // We have exact match, so just get offset and remove entry
  if (itr != free_entries_.cend()) {
    offset = itr->offset;
    header_.free_used--;
    free_entries_.erase(itr);
    return offset;
  }

  // Check for any entry that matches size.
  itr = std::ranges::find_if(free_entries_, [entry_size](XdbfFileLoc entry) {
    return entry.size > entry_size;
  });

  // There is an requirement that there is always at least one entry, so no need
  // to check for valid entry.
  offset = itr->offset;
  itr->offset += entry_size;
  itr->size -= entry_size;

  return offset;
}

void GpdInfo::InsertEntry(Entry* entry) {
  ResizeEntryTable();

  entry->info.offset = FindFreeLocation(entry->info.size);

  entries_.push_back(*entry);

  header_.entry_used++;
}

void GpdInfo::DeleteEntry(const Entry* entry) {
  // Don't really remove entry. Just remove entry in the entry table.
  MarkSpaceAsFree(entry->info.offset, entry->info.size);

  auto itr = std::ranges::find_if(entries_, [entry](Entry first) {
    return entry->info.section == first.info.section &&
           first.info.id == entry->info.id;
  });

  if (itr != entries_.end()) {
    entries_.erase(itr);
  }
  header_.entry_used--;
}

std::vector<const Entry*> GpdInfo::GetSortedEntries() const {
  std::vector<const Entry*> sorted_entries;

  for (auto& entry : entries_) {
    sorted_entries.push_back(&entry);
  }

  std::ranges::sort(sorted_entries,
                    [](const Entry* first, const Entry* second) {
                      if (first->info.section == second->info.section) {
                        return first->info.id < second->info.id;
                      }
                      return first->info.section < second->info.section;
                    });

  return sorted_entries;
}

void GpdInfo::ResizeEntryTable() {
  // There is no need to recalculate offsets as they're in relation to end of
  // this entries count.
  if (header_.entry_used >= header_.entry_count) {
    header_.entry_count =
        xe::round_up(header_.entry_count + 1, base_entry_count, true);
  }

  if (header_.free_used >= header_.free_count) {
    header_.free_used =
        xe::round_up(header_.free_used + 1, base_entry_count, true);
  }
}

void GpdInfo::ReallocateEntry(Entry* entry, uint32_t required_size) {
  MarkSpaceAsFree(entry->info.offset, entry->info.size);

  // Now find new location for out entry
  entry->info.size = required_size;
  entry->info.offset = FindFreeLocation(required_size);
}

void GpdInfo::MarkSpaceAsFree(uint32_t offset, uint32_t size) {
  XdbfFileLoc loc;
  loc.size = size;
  loc.offset = offset;

  ResizeEntryTable();
  free_entries_.emplace(free_entries_.begin(), loc);
  header_.free_used++;
}

}  // namespace xam
}  // namespace kernel
}  // namespace xe
