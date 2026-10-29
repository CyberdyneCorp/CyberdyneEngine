// SPDX-License-Identifier: MIT
#ifndef CY_SAMPLE_PLAY_BINDINGS_FILE_H
#define CY_SAMPLE_PLAY_BINDINGS_FILE_H
// bindings_file.h — a player's rebinding, read from a text file into the input user's profile.
//
//     # action          control              slot (optional, 0 is the primary)
//     player.jump       keyboard/f
//     weapon.fire       mouse/right
//     player.crouch     gamepad/east
//
// One override a line: the action by its declared NAME, the control by the input layer's own path
// (`cy::input::cook::parse_control_path` — loading a profile is cook-time work, never the
// evaluation path), and which of the action's bindings in that control's scheme it replaces. The
// authored bindings are untouched: `input-and-actions` keeps a player's changes as overrides in a
// profile, applied over the defaults, and that is where these go.
//
// A line that names an action the game did not declare, or a control the input layer has no path
// for, refuses the whole file and says which line: a typo should not silently leave the old key.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/servers/input/server.h>

#include <string>
#include <string_view>

namespace cy::play {

struct BindingsResult {
    u32 applied = 0;
    /// Empty on success; otherwise the line and why.
    std::string error;
};

/// Parse `text` and store each override in user `user`'s profile. Nothing is stored when any line
/// is refused.
[[nodiscard]] BindingsResult apply_bindings(input::InputServer& server, u32 user,
                                            std::string_view text) noexcept;

/// The file at `path`, read and applied.
[[nodiscard]] BindingsResult apply_bindings_file(input::InputServer& server, u32 user,
                                                 const char* path) noexcept;

}  // namespace cy::play

#endif  // CY_SAMPLE_PLAY_BINDINGS_FILE_H
