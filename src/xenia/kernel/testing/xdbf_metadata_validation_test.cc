/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Emulator. All rights reserved.                        *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */
#include <vector>

#include "third_party/catch/include/catch.hpp"
#include "xenia/kernel/xam/xdbf/spa_info.h"
#include "xenia/kernel/xam/xdbf/xdbf_io.h"

namespace xe::kernel::test {
TEST_CASE("Runtime XDBF rejects truncated tables before copying entries",
          "[xdbf]") {
  std::vector<uint8_t> malformed(32, 0);
  malformed[0] = 'X';
  malformed[1] = 'D';
  malformed[2] = 'B';
  malformed[3] = 'F';
  malformed[4] = 0;
  malformed[5] = 1;
  // Both capacity and used count claim one entry, but its bytes are absent.
  malformed[11] = malformed[15] = 1;
  const xam::XdbfFile file(malformed);
  REQUIRE(file.GetEntry(1, xam::kXdbfIdXthd) == nullptr);
  xam::SpaInfo spa(malformed);
  REQUIRE(spa.title_id() == 0);
  REQUIRE_FALSE(spa.is_demo());
}
}  // namespace xe::kernel::test
