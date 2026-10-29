// SPDX-License-Identifier: MIT
// cy/abi/game/menus.h — the backends behind ABI 1.17's display settings and the player's files.
//
// A settings screen changes what the HOST owns — the window's resolution, the renderer's quality
// preset and upscaler — and keeps the player's choices where the platform allows a game to write.
// `cy_abi` names neither a renderer nor a file system, so both are backends the embedder binds:
// `cy::game_backend::VideoSettingsAdapter` and `cy::game_backend::UserDataDirectory`
// (src/game_backend/include/cy/game_backend/menu_backends.h) are the engine's. Without one, the
// entries answer UNAVAILABLE.
//
// The thunks (src/abi/src/game/menu_thunks.cpp) check the engine, the phase, the binding, the
// pointers, every sized struct and every enumerator and flag; a backend validates what only it
// knows — a resolution the display does not offer, a file that is not there.

#pragma once

#include <cy/abi/cy_abi.h>
#include <cy/core/base/types.h>

namespace cy::abi::game {

/// The display settings a game changes from its settings screen.
class VideoBackend {
public:
    virtual ~VideoBackend() = default;

    [[nodiscard]] virtual CyResult settings(CyVideoSettings& out) const noexcept = 0;
    /// `settings` is whole, its quality, upscaler and flags in range.
    [[nodiscard]] virtual CyResult apply(const CyVideoSettings& settings) noexcept = 0;
    /// Every mode the display offers, in order; `count` is written whatever the capacity.
    [[nodiscard]] virtual CyResult modes(CyVideoMode* out, u32 capacity,
                                         u32& count) const noexcept = 0;
};

/// The player's files, in the game's user-data directory.
class UserDataBackend {
public:
    virtual ~UserDataBackend() = default;

    /// `name` is one plain file name, checked by the thunk.
    [[nodiscard]] virtual CyResult write(const char* name, const void* bytes,
                                         u64 size) noexcept = 0;
    /// The file's whole size into `size`; up to `capacity` bytes into `buffer` when it is not null.
    [[nodiscard]] virtual CyResult read(const char* name, void* buffer, u64 capacity,
                                        u64& size) noexcept = 0;
};

}  // namespace cy::abi::game
