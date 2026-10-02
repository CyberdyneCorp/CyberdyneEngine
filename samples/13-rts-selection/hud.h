// SPDX-License-Identifier: MIT
#pragma once
// A strategy game's HUD, built on CyberUI: a resource bar, a minimap with the camera's rectangle,
// and a panel listing the selected units. The game drives it; the engine lays it out, flattens it
// and draws it in the frame's interface stage.
//
// Everything here is game code. It creates elements in the engine's `ElementStore`, attaches their
// text through `TextPainter`, and writes paint and layout inputs when the game's state changes —
// a resource count is a repaint, a different number of selected units a relayout of one panel.
// `render.ui` photographs the same HUD this sample draws, from this file.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/array.h>
#include <cy/ui/store.h>
#include <cy/ui/text/text_painter.h>

namespace cy::sample::rts {

/// What the resource bar shows.
struct Resources {
    u32 gold = 0;
    u32 wood = 0;
    u32 food = 0;
    u32 food_cap = 0;
};

/// One row of the selection panel.
struct SelectedUnit {
    const char* name = "";
    u32 health = 0;
    u32 max_health = 1;
};

enum class Team : u8 { Player, Enemy, Neutral };

/// A unit on the minimap, at a position normalised to the map: (0, 0) top-left, (1, 1)
/// bottom-right.
struct MinimapDot {
    f32 x = 0.0F;
    f32 y = 0.0F;
    Team team = Team::Player;
};

/// The HUD's rows: the selection panel shows at most this many units, and says how many more.
inline constexpr u32 kSelectionRows = 3;
inline constexpr u32 kMinimapDots = 16;

class Hud {
public:
    Hud() noexcept = default;

    Hud(const Hud&) = delete;
    Hud& operator=(const Hud&) = delete;
    Hud(Hud&&) = delete;
    Hud& operator=(Hud&&) = delete;

    /// Build the HUD under `root`, which must lay its children out with `LayoutModel::Absolute`
    /// over the whole screen. The store and the painter must outlive the HUD.
    [[nodiscard]] Status create(ui::ElementStore& store, ui::TextPainter& text,
                                ui::ElementId root) noexcept;

    [[nodiscard]] Status set_resources(const Resources& resources) noexcept;
    [[nodiscard]] Status set_selection(Span<const SelectedUnit> units) noexcept;
    /// The units on the map and the part of it the camera sees, as a rectangle in map coordinates.
    [[nodiscard]] Status set_minimap(Span<const MinimapDot> dots, ui::Rect camera) noexcept;

    [[nodiscard]] ui::ElementId resource_bar() const noexcept { return bar_; }
    [[nodiscard]] ui::ElementId minimap() const noexcept { return minimap_; }
    [[nodiscard]] ui::ElementId camera_frame() const noexcept { return camera_; }
    [[nodiscard]] ui::ElementId selection_panel() const noexcept { return selection_; }

private:
    struct Row {
        ui::ElementId row;
        ui::ElementId name;
        ui::ElementId bar;
        ui::ElementId fill;
        ui::ElementId health;
    };

    [[nodiscard]] Expected<ui::ElementId, Error> element(ui::ElementId parent,
                                                         const char* type) noexcept;
    [[nodiscard]] Expected<ui::ElementId, Error> label(ui::ElementId parent, const char* text,
                                                       u32 colour) noexcept;
    [[nodiscard]] Status create_bar(ui::ElementId root) noexcept;
    [[nodiscard]] Status create_minimap(ui::ElementId root) noexcept;
    [[nodiscard]] Status create_selection(ui::ElementId root) noexcept;
    [[nodiscard]] Status create_row(Row& row) noexcept;
    [[nodiscard]] Status set_text(ui::ElementId element, const char* text, u32 colour) noexcept;

    ui::ElementStore* store_ = nullptr;
    ui::TextPainter* text_ = nullptr;
    ui::ElementId bar_;
    ui::ElementId gold_;
    ui::ElementId wood_;
    ui::ElementId food_;
    ui::ElementId minimap_;
    ui::ElementId camera_;
    ui::ElementId dots_[kMinimapDots];
    ui::ElementId selection_;
    ui::ElementId title_;
    Row rows_[kSelectionRows];
};

}  // namespace cy::sample::rts
