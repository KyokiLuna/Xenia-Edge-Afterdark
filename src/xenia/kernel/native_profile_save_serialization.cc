/** Xenia : Xbox 360 Emulator Research Project; BSD license, see LICENSE. */
#include "xenia/kernel/native_profile_save_serialization.h"
#include <algorithm>
#include <new>
#include <tuple>
#include <vector>
#include "xenia/base/profile_gpd_snapshot.h"
#include "xenia/base/xdbf_view.h"
namespace xe::kernel::save_detail {
namespace {
struct Record {
  uint16_t section;
  uint64_t key;
  std::span<const uint8_t> bytes;
};
std::vector<Record> Records(const XdbfTableView& view) {
  std::vector<Record> out; out.reserve(view.entries_used);
  for (size_t i = 0; i != view.entries_used; ++i) {
    const auto* e = view.entries.data() + i * 18;
    out.push_back({uint16_t(uint16_t(e[0]) << 8 | e[1]),
        uint64_t(XdbfTableView::U32(e+2)) << 32 | XdbfTableView::U32(e+6),
        view.data.subspan(XdbfTableView::U32(e+10), XdbfTableView::U32(e+14))});
  }
  std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) {
    return std::tie(a.section,a.key) < std::tie(b.section,b.key);
  });
  return out;
}
}  // namespace
std::error_code ValidateNativeGpdSerialization(std::span<const uint8_t> intended,
    std::span<const uint8_t> staged, std::span<const uint8_t> native_table) {
  try {
    if (const auto error = filesystem::ValidateProfileGpdSnapshot(intended)) { return error; }
    if (const auto error = filesystem::ValidateProfileGpdSnapshot(staged)) { return error; }
    const auto a = XdbfTableView::Parse(intended), b = XdbfTableView::Parse(staged);
    // Strict validation already bounds each capacity/prefix within64MiB;
    // these widened arithmetic operations cannot overflow on supported hosts.
    const uint64_t prefix = 24 + uint64_t(XdbfTableView::U32(staged.data()+8))*18 +
                           uint64_t(XdbfTableView::U32(staged.data()+16))*8;
    if (prefix > staged.size() || native_table.size() != prefix ||
        !std::equal(native_table.begin(), native_table.end(), staged.begin())) {
      return std::make_error_code(std::errc::bad_message);
    }
    const auto expected = Records(*a), actual = Records(*b);
    if (expected.size() != actual.size()) { return std::make_error_code(std::errc::state_not_recoverable); }
    for (size_t i = 0; i != expected.size(); ++i) {
      const auto& x = expected[i]; const auto& y = actual[i];
      if (x.section != y.section || x.key != y.key || x.bytes.size() != y.bytes.size() ||
          !std::equal(x.bytes.begin(), x.bytes.end(), y.bytes.begin())) {
        return std::make_error_code(std::errc::state_not_recoverable);
      }
    }
    return {};
  } catch (const std::bad_alloc&) {
    return std::make_error_code(std::errc::not_enough_memory);
  } catch (...) { return std::make_error_code(std::errc::io_error); }
}
}  // namespace xe::kernel::save_detail
