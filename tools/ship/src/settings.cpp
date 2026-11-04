// SPDX-License-Identifier: MIT
// A packaged game's settings file. See settings.h.

#include <cy/ship/settings.h>

#include <fstream>
#include <ranges>
#include <system_error>

namespace cy::ship {

namespace fs = std::filesystem;

namespace {

[[nodiscard]] std::string trimmed(const std::string& text) {
    const usize first = text.find_first_not_of(" \t\r");
    if (first == std::string::npos) {
        return {};
    }
    const usize last = text.find_last_not_of(" \t\r");
    return text.substr(first, last - first + 1);
}

[[nodiscard]] bool write_defaults(const fs::path& file, const Setting* table, usize count,
                                  std::string& error) {
    std::error_code code;
    fs::create_directories(file.parent_path(), code);
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out << "# Settings. One `key = value` per line; a flag on the command line wins over this "
           "file.\n# Delete the file to restore the defaults.\n";
    for (usize index = 0; index < count; ++index) {
        out << "\n# " << table[index].help << "\n"
            << table[index].key << " = " << table[index].fallback << "\n";
    }
    if (!out) {
        error = "cannot write " + file.string();
        return false;
    }
    return true;
}

/// One line read into `out`: a comment or a blank is nothing; anything else is `key = value`.
void read_line(const std::string& raw, usize number, Settings& out) {
    const std::string line = trimmed(raw);
    if (line.empty() || line.front() == '#') {
        return;
    }
    const usize equals = line.find('=');
    if (equals == std::string::npos) {
        out.warnings.push_back("line " + std::to_string(number) + " is not `key = value`");
        return;
    }
    out.values.emplace_back(trimmed(line.substr(0, equals)), trimmed(line.substr(equals + 1)));
}

[[nodiscard]] const Setting* lookup(const Setting* table, usize count, const std::string& key) {
    for (usize index = 0; index < count; ++index) {
        if (key == table[index].key) {
            return &table[index];
        }
    }
    return nullptr;
}

/// The flag a switch's value adds, or a warning when the value is neither `on` nor `off`.
void switch_arguments(const Setting& setting, const std::string& value, Settings& settings,
                      std::vector<std::string>& out) {
    const bool on = value == "on" || value == "true" || value == "yes";
    const bool off = value == "off" || value == "false" || value == "no";
    if (!on && !off) {
        settings.warnings.push_back(std::string(setting.key) + " = " + value +
                                    " is neither on nor off, so it is ignored");
        return;
    }
    const char* flag = on ? setting.on_flag : setting.off_flag;
    if (flag != nullptr) {
        out.emplace_back(flag);
    }
}

[[nodiscard]] bool digits(const std::string& text, usize from, usize until) noexcept {
    if (from >= until || until - from > 5) {
        return false;
    }
    for (usize index = from; index < until; ++index) {
        if (text[index] < '0' || text[index] > '9') {
            return false;
        }
    }
    return text[from] != '0';
}

}  // namespace

bool valid_setting(SettingKind kind, const std::string& value) noexcept {
    switch (kind) {
        case SettingKind::Switch:
            return value == "on" || value == "off";
        case SettingKind::Count:
            return digits(value, 0, value.size());
        case SettingKind::Size: {
            const usize x = value.find('x');
            return x != std::string::npos && digits(value, 0, x) &&
                   digits(value, x + 1, value.size());
        }
    }
    return false;
}

const std::string* Settings::find(const std::string& key) const noexcept {
    // The last line naming a key wins, as it does in every configuration file a person edits.
    for (const auto& [name, value] : std::ranges::reverse_view(values)) {
        if (name == key) {
            return &value;
        }
    }
    return nullptr;
}

bool load_settings(const fs::path& file, const Setting* table, usize count, Settings& out,
                   std::string& error) {
    out = Settings{};
    std::error_code code;
    if (!fs::exists(file, code)) {
        if (!write_defaults(file, table, count, error)) {
            return false;
        }
        out.created = true;
    }
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        error = "cannot read " + file.string();
        return false;
    }
    std::string line;
    for (usize number = 1; std::getline(in, line); ++number) {
        read_line(line, number, out);
    }
    for (const auto& [key, value] : out.values) {
        if (lookup(table, count, key) == nullptr) {
            out.warnings.push_back("'" + key + "' is not a setting this game has");
        }
    }
    return true;
}

void settings_arguments(const Setting* table, usize count, Settings& settings,
                        std::vector<std::string>& out) {
    for (usize index = 0; index < count; ++index) {
        const Setting& setting = table[index];
        const std::string* found = settings.find(setting.key);
        const std::string value = found != nullptr ? *found : std::string(setting.fallback);
        if (setting.kind == SettingKind::Switch) {
            switch_arguments(setting, value, settings, out);
        } else if (!valid_setting(setting.kind, value)) {
            settings.warnings.push_back(std::string(setting.key) + " = " + value +
                                        " is not a value it can take, so it is ignored");
        } else {
            out.emplace_back(setting.on_flag);
            out.push_back(value);
        }
    }
}

}  // namespace cy::ship
