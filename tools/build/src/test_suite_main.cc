/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2021 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <locale>
#include <string>
#include <vector>

#include "xenia/base/console_app_main.h"
#include "xenia/base/platform.h"
#include "xenia/base/host_storage.h"

#define CATCH_CONFIG_RUNNER
#include "third_party/catch/single_include/catch2/catch.hpp"

namespace xe {
namespace test_suite {

int test_suite_main(const std::vector<std::string>& args) {
  // Research/build qualification explicitly binds its protected project FS.
  // Absence preserves general-use defaults; never infer storage_root/CWD.
  std::shared_ptr<filesystem::HostStorageRoot> protected_filesystem;
  if (const auto requested = std::getenv("XENIA_HOST_STORAGE_PROTECTED_ROOT")) {
    if (!*requested) { std::cerr << "Empty protected filesystem root\n"; return 2; }
    std::error_code error;
    protected_filesystem = filesystem::HostStorageRoot::Open(
        xe::to_path(requested), true, error, filesystem::kHostStorageReserve);
    if (!protected_filesystem || error) {
      std::cerr << "Protected filesystem unavailable: " << error.message() << '\n';
      return 2;
    }
    const auto space = protected_filesystem->Observe();
    if (!space.available || space.available_bytes < space.reserve_bytes) {
      std::cerr << "Protected filesystem reserve unavailable\n"; return 2;
    }
  }

  // Catch doesn't expose a way to pass a vector of strings, despite building a
  // vector internally.
  int argc = 0;
  std::vector<const char*> argv;
  for (const auto& arg : args) {
    argv.push_back(arg.c_str());
    argc++;
  }

  // Run Catch with compact reporter that shows dots
  Catch::Session session;

  // Use the compact reporter which shows dots for progress
  session.configData().reporterName = "compact";

  return session.run(argc, argv.data());
}

}  // namespace test_suite
}  // namespace xe

#ifndef XE_TEST_SUITE_NAME
#error XE_TEST_SUITE_NAME is undefined!
#endif

XE_DEFINE_CONSOLE_APP_TRANSPARENT(XE_TEST_SUITE_NAME,
                                  xe::test_suite::test_suite_main);
