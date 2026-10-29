// SPDX-License-Identifier: MIT
// cy/game_backend/menu_backends.h — the display settings and the player's files behind ABI 1.17.
//
// `VideoSettingsAdapter` holds the display settings a game's settings screen chooses and the modes
// the host's display offers. The HOST applies them — it owns the window, the renderer's quality
// preset and the upscaler — so the adapter validates, stores and counts: a host reads `settings()`
// when `revision()` moved and rebuilds what changed. `UserDataDirectory` keeps the player's files
// in one directory the host names (the platform's user-data directory, or a test's), each written
// atomically: a crash mid-save leaves the previous file whole.

#pragma once

#include <cy/abi/game/menus.h>
#include <cy/abi/host.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/allocator.h>
#include <cy/core/memory/array.h>

#include <string_view>

namespace cy::game_backend {

/// The `video_*` entries over settings the host applies.
class VideoSettingsAdapter final : public cy::abi::game::VideoBackend {
public:
    explicit VideoSettingsAdapter(Allocator& allocator) noexcept : modes_(allocator) {}

    /// The modes the display offers, and the settings in effect. `initial`'s resolution must be
    /// one of `modes`.
    [[nodiscard]] Status start(Span<const CyVideoMode> modes,
                               const CyVideoSettings& initial) noexcept;

    [[nodiscard]] CyResult settings(CyVideoSettings& out) const noexcept override;
    [[nodiscard]] CyResult apply(const CyVideoSettings& settings) noexcept override;
    [[nodiscard]] CyResult modes(CyVideoMode* out, u32 capacity,
                                 u32& count) const noexcept override;

    /// The settings in effect, for the host.
    [[nodiscard]] const CyVideoSettings& current() const noexcept { return current_; }
    /// Moves every time a game applies settings that differ from those in effect.
    [[nodiscard]] u64 revision() const noexcept { return revision_; }

private:
    Array<CyVideoMode> modes_;
    CyVideoSettings current_{};
    u64 revision_ = 0;
};

/// The `user_data_*` entries over one directory.
class UserDataDirectory final : public cy::abi::game::UserDataBackend {
public:
    explicit UserDataDirectory(Allocator& allocator) noexcept : root_(allocator) {}

    /// Keep the player's files in `root`, creating it when it does not exist.
    [[nodiscard]] Status start(std::string_view root) noexcept;

    [[nodiscard]] CyResult write(const char* name, const void* bytes, u64 size) noexcept override;
    [[nodiscard]] CyResult read(const char* name, void* buffer, u64 capacity,
                                u64& size) noexcept override;

private:
    /// `root/name` as a C string, into `out`.
    [[nodiscard]] bool path_of(const char* name, Array<char>& out) const noexcept;

    Array<char> root_;
};

/// Bind `video` and `files` as `host`'s 1.17 display and user-data backends, or unbind either with
/// null. The adapters must outlive the binding.
void bind(cy::abi::Host& host, VideoSettingsAdapter* video, UserDataDirectory* files) noexcept;

}  // namespace cy::game_backend
