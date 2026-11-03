// SPDX-License-Identifier: MIT
#ifndef CY_SHIP_SETTINGS_H
#define CY_SHIP_SETTINGS_H
// A packaged game's settings file, in the user's configuration directory. Issue #87.
//
// `key = value` lines, `#` comments, nothing else — a file a player edits in a text editor. The
// first launch writes it with every setting at its default and a line saying what each does, so
// the file documents itself. A setting is a translation to the game's own command-line flags: the
// settings are applied BEFORE the player's arguments, so a flag typed on the command line wins over
// the file, and a game that already parses `--no-vsync` needs no second way of being told.
//
// A value the game cannot use is reported and ignored rather than refused: a typo in a settings
// file must not stop a game from starting.

#include <cy/core/base/types.h>

#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace cy::ship {

enum class SettingKind : u8 {
    /// `on` or `off`: `on_flag` or `off_flag` is added, either of which may be null.
    Switch,
    /// A whole number, passed after `on_flag`.
    Count,
    /// `<width>x<height>`, passed after `on_flag`.
    Size,
};

struct Setting {
    const char* key;
    const char* fallback;
    SettingKind kind;
    const char* on_flag;
    const char* off_flag;
    const char* help;
};

/// True when `value` is one this kind of setting can take.
[[nodiscard]] bool valid_setting(SettingKind kind, const std::string& value) noexcept;

struct Settings {
    std::vector<std::pair<std::string, std::string>> values;
    std::vector<std::string> warnings;
    bool created = false;

    [[nodiscard]] const std::string* find(const std::string& key) const noexcept;
};

/// Read `file`, writing it with every default first when it does not exist yet.
[[nodiscard]] bool load_settings(const std::filesystem::path& file, const Setting* table,
                                 usize count, Settings& out, std::string& error);

/// The flags the settings translate to, in table order. Unusable values become warnings.
void settings_arguments(const Setting* table, usize count, Settings& settings,
                        std::vector<std::string>& out);

}  // namespace cy::ship

#endif  // CY_SHIP_SETTINGS_H
