/** Xenia : Xbox 360 Emulator Research Project; BSD license, see LICENSE. */
#include "xenia/kernel/xam/xdbf/gpd_snapshot.h"
#include <algorithm>
#include <cstdio>
#include <set>
#include <utility>
#include <vector>
#include "xenia/base/xdbf_view.h"
#include "xenia/kernel/xam/xdbf/gpd_info.h"
namespace xe::kernel::xam {
namespace {
std::error_code BadWire() {
  return std::make_error_code(std::errc::bad_message);
}
bool SameGpdLeaf(const std::filesystem::path& leaf, uint32_t title);
std::filesystem::path GpdLeaf(uint32_t title) {
  char leaf[13];
  std::snprintf(leaf, sizeof(leaf), "%08X.gpd", title);
  return leaf;
}
bool SameGpdLeaf(const std::filesystem::path& leaf, uint32_t title) {
  if (leaf.has_parent_path()) { return false; }
  const auto a = leaf.string(), b = GpdLeaf(title).string();
  auto lower = [](unsigned char c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; };
  return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(),
      [&](unsigned char x, unsigned char y) { return lower(x) == lower(y); });
}
}
std::error_code ValidateGpdSnapshot(std::span<const uint8_t> bytes) {
  if (bytes.size() > GpdSnapshotStore::kMaximumBytes) {
    return std::make_error_code(std::errc::file_too_large);
  }
  const auto view = XdbfTableView::Parse(bytes);
  if (!view || XdbfTableView::U32(bytes.data() + 4) != 0x10000 ||
      !view->free_entries_used) { return BadWire(); }
  const auto* eof = view->free_entries.data() +
                    size_t(view->free_entries_used - 1) * 8;
  const uint64_t data_eof = XdbfTableView::U32(eof);
  const uint64_t tail_size = XdbfTableView::U32(eof + 4);
  // Native GPD tail geometry is {offset, UINT32_MAX-offset}; the tail
  // shrinks as the producer allocates data. Do not compare size alone or
  // permit uint32_t addition to wrap when checking this exact endpoint.
  if (data_eof + tail_size != UINT32_MAX ||
      data_eof > view->data.size()) { return BadWire(); }
  // A safe model load needs unique keys and valid disjoint used/free extents.
  // Free-table/slack bytes are not normalized or discarded by this adapter;
  // the original raw snapshot is what the external recovery copy preserves.
  std::set<std::pair<uint16_t, uint64_t>> keys;
  std::vector<std::pair<uint64_t, uint64_t>> ranges;
  for (size_t i = 0; i != view->entries_used; ++i) {
    const auto* entry = view->entries.data() + i * 18;
    const uint16_t section = uint16_t(entry[0]) << 8 | entry[1];
    const uint64_t id = uint64_t(XdbfTableView::U32(entry + 2)) << 32 |
                        XdbfTableView::U32(entry + 6);
    if (!keys.emplace(section, id).second) { return BadWire(); }
    const uint64_t offset = XdbfTableView::U32(entry + 10);
    const uint64_t length = XdbfTableView::U32(entry + 14);
    if (offset > data_eof || length > data_eof - offset) { return BadWire(); }
    if (length) { ranges.emplace_back(offset, offset + length); }
  }
  for (size_t i = 0; i + 1 < view->free_entries_used; ++i) {
    const auto* entry = view->free_entries.data() + i * 8;
    const uint64_t offset = XdbfTableView::U32(entry);
    const uint64_t length = XdbfTableView::U32(entry + 4);
    if (length == UINT32_MAX || offset > data_eof ||
        length > data_eof - offset) { return BadWire(); }
    if (length) { ranges.emplace_back(offset, offset + length); }
  }
  std::sort(ranges.begin(), ranges.end());
  for (size_t i = 1; i < ranges.size(); ++i) {
    if (ranges[i].first < ranges[i - 1].second) { return BadWire(); }
  }
  return {};
}
GpdSnapshotLoad GpdSnapshotStore::Load(GpdSnapshotIdentity identity,
    std::shared_ptr<const filesystem::HostStorageRoot> view) {
  GpdSnapshotLoad result;
  result.identity = identity;
  if (!identity.xuid || !identity.source_generation || !identity.title_id ||
      !view || view->is_guest_private()) {
    result.error = std::make_error_code(std::errc::invalid_argument);
    return result;
  }
  result.token = view->CaptureSnapshot(GpdLeaf(identity.title_id),
                                        kMaximumBytes, result.error, true);
  if (!result.error && result.token->exists()) {
    result.error = ValidateGpdSnapshot(result.token->bytes());
  }
  // Preserve a raw readable-but-invalid token for recovery/reporting, but it
  // cannot be used through Publish because error is not empty.
  return result;
}
GpdSnapshotWrite GpdSnapshotStore::Publish(
    const filesystem::SnapshotRecoveryOwner& owner,
    const GpdSnapshotLoad& loaded, std::span<const uint8_t> serialized,
    const std::atomic<bool>* canceled) {
  GpdSnapshotWrite result;
  result.identity = loaded.identity;
  result.storage.expected = loaded.token;
  result.storage.error = loaded.error;
  if (result.storage.error) { return result; }
  if (!loaded.identity.xuid || !loaded.identity.source_generation ||
      !loaded.identity.title_id || !loaded.token ||
      !loaded.token->ascii_folded_leaf() ||
      !SameGpdLeaf(loaded.token->relative(), loaded.identity.title_id)) {
    result.storage.error = std::make_error_code(std::errc::invalid_argument);
    return result;
  }
  result.storage.error = ValidateGpdSnapshot(serialized);
  if (result.storage.error) { return result; }
  result.serialized = true;
  result.storage = filesystem::SnapshotPublisher::Publish(owner, loaded.token,
                                                           serialized, canceled);
  return result;
}
GpdSnapshotWrite GpdSnapshotStore::Publish(
    const filesystem::SnapshotRecoveryOwner& owner,
    const GpdSnapshotLoad& loaded, const GpdInfo& model,
    const std::atomic<bool>* canceled) {
  // Load failure takes precedence and must never authorize an empty model.
  if (loaded.error) {
    GpdSnapshotWrite result;
    result.identity = loaded.identity;
    result.storage.expected = loaded.token;
    result.storage.error = loaded.error;
    return result;
  }
  std::vector<uint8_t> serialized;
  const auto error = model.SerializeChecked(serialized, kMaximumBytes);
  if (error) {
    GpdSnapshotWrite result;
    result.identity = loaded.identity;
    result.storage.expected = loaded.token;
    result.storage.error = error;
    return result;
  }
  return Publish(owner, loaded, serialized, canceled);
}
GpdSnapshotDelete GpdSnapshotStore::Delete(
    const filesystem::SnapshotRecoveryOwner& owner,
    const GpdSnapshotLoad& loaded, const std::atomic<bool>* canceled) {
  GpdSnapshotDelete result;
  result.identity = loaded.identity;
  result.storage.kind = filesystem::SnapshotMutationKind::kDelete;
  result.storage.expected = loaded.token;
  result.storage.error = loaded.error;
  if (result.storage.error) {
    return result;
  }
  if (!loaded.identity.xuid || !loaded.identity.source_generation ||
      !loaded.identity.title_id || !loaded.token || !loaded.token->exists() ||
      !loaded.token->ascii_folded_leaf() ||
      !SameGpdLeaf(loaded.token->relative(), loaded.identity.title_id)) {
    result.storage.error = std::make_error_code(std::errc::invalid_argument);
    return result;
  }
  result.storage.error = ValidateGpdSnapshot(loaded.token->bytes());
  if (result.storage.error) {
    return result;
  }
  result.storage =
      filesystem::SnapshotPublisher::Delete(owner, loaded.token, canceled);
  return result;
}
}  // namespace xe::kernel::xam
