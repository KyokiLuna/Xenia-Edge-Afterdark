/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_CONFIG_H_
#define XENIA_CONFIG_H_

#include <filesystem>
#include <functional>
#include <optional>
#include <string_view>
#include "third_party/tomlplusplus/toml.hpp"

namespace xe {
class Emulator;
}  // namespace xe

toml::parse_result ParseFile(const std::filesystem::path& filename);

namespace config {
extern std::filesystem::path config_folder;
extern std::string game_config_suffix;

void SetupConfig(const std::filesystem::path& config_folder);
toml::table LoadGameConfig(uint32_t title_id);
// Read an arbitrary title's integer config with runtime precedence, leaving all
// active cvars and pending type-warning state untouched. Nullopt means unknown
// cvar or a non-int32 cvar, rather than a synthetic default.
std::optional<int32_t> ResolveInt32GameConfig(uint32_t title_id,
                                           std::string_view cvar_name);
std::filesystem::path GetBundledDataPath(const std::string& subdirectory);

// Extract title_id from a game file and load its config overrides.
// Returns the title_id if successful, 0 otherwise.
uint32_t LoadGameConfigForFile(const std::filesystem::path& game_path);
void SaveConfig();
void SaveGameConfig(uint32_t title_id, const toml::table& config_table);
// Resolves a possibly dotted category ("GPU.Debug") to a nested table.
toml::table* ResolveSectionTable(toml::table& root, std::string_view section);
void SaveGameConfigSetting(xe::Emulator* emulator, const char* section,
                           const char* cvar_name, const std::string& value);
void SaveGameConfigSetting(xe::Emulator* emulator, const char* section,
                           const char* cvar_name, bool value);
void SaveGameConfigSetting(xe::Emulator* emulator, const char* section,
                           const char* cvar_name, int32_t value);
void SaveGameConfigSetting(xe::Emulator* emulator, const char* section,
                           const char* cvar_name, uint32_t value);
void SaveGameConfigSetting(xe::Emulator* emulator, const char* section,
                           const char* cvar_name, double value);
void ReloadConfig();
void SetConfigSavedCallback(std::function<void()> callback);
}  // namespace config

#endif  // XENIA_CONFIG_H_
