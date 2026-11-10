/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project
 ******************************************************************************
 * Released under the BSD license - see LICENSE in the root for more details.
 */
#include "third_party/catch/include/catch.hpp"
#include "xenia/base/cvar.h"

namespace cvar::test {
TEST_CASE("Offline config resolution preserves the active title and precedence",
          "[config][library]") {
  int32_t active = 0;
  ConfigVar<int32_t> variable("license_mask", &active, "", "", "Content",
                              false, false);
  CHECK(variable.ResolveForGameConfig(std::nullopt) == 0);
  variable.SetConfigValue(1);
  variable.SetGameConfigValue(7);  // A different running title.
  REQUIRE(active == 7);
  CHECK(variable.ResolveForGameConfig(std::nullopt) == 1);
  CHECK(variable.ResolveForGameConfig(-1) == -1);
  CHECK(variable.ResolveForGameConfig(0) == 0);
  CHECK(active == 7);
  CHECK(variable.GetTypedConfigValue() == 1);

  // Explicit command-line zero still wins when equal to the global value.
  // Comparing stringified config/command-line values cannot detect this case.
  variable.SetConfigValue(0);
  variable.SetCommandLineValue(0);
  CHECK(variable.ResolveForGameConfig(1) == 0);
  CHECK(active == 0);
  variable.ClearGameConfigValue();
  CHECK(variable.ResolveForGameConfig(-1) == 0);
  variable.OverrideConfigValue(1);  // Deliberately drops higher priorities.
  CHECK(variable.ResolveForGameConfig(-1) == -1);
  CHECK(active == 1);
}
}  // namespace cvar::test
