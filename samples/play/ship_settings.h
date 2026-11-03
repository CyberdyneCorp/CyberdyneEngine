// SPDX-License-Identifier: MIT
#ifndef CY_SAMPLES_PLAY_SHIP_SETTINGS_H
#define CY_SAMPLES_PLAY_SHIP_SETTINGS_H
// The settings file of a game packaged under the desktop play host (issue #87): the window's size,
// full screen and v-sync, translated to the play host's own flags (play_arguments.h). A packaged
// game started with no arguments plays; tools/ship/include/cy/ship/launch.h is the launch.

#include <cy/ship/launch.h>

namespace cy::play {

inline constexpr ship::Setting kShipSettings[] = {
    {"window", "1280x720", ship::SettingKind::Size, "--window", nullptr,
     "The window's size, <width>x<height>."},
    {"fullscreen", "off", ship::SettingKind::Switch, "--fullscreen", nullptr,
     "Borderless at the desktop's size: on or off."},
    {"vsync", "on", ship::SettingKind::Switch, nullptr, "--no-vsync",
     "Wait for the display between frames: on or off."},
};
inline constexpr const char* kShipDefaults[] = {"--play"};

/// The launch of a game the play host plays, by its `just package` name and its directory name.
[[nodiscard]] inline ship::LaunchSpec ship_spec(const char* game,
                                                const char* application) noexcept {
    ship::LaunchSpec spec{game, application};
    spec.settings = kShipSettings;
    spec.setting_count = sizeof(kShipSettings) / sizeof(kShipSettings[0]);
    spec.settings_when = "--play";
    spec.default_arguments = kShipDefaults;
    spec.default_argument_count = 1;
    return spec;
}

}  // namespace cy::play

#endif  // CY_SAMPLES_PLAY_SHIP_SETTINGS_H
