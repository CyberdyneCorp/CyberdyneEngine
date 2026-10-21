// SPDX-License-Identifier: MIT
#pragma once
// Fog of war, the presentation half: one team's visibility as a texture the frame samples.
//
// ================================================================================================
// THE SIMULATION DECIDES, THIS ONLY DRAWS
// ================================================================================================
//
// Which cells a team sees is lockstep state, kept in integers by `cy::gameplay::fog::FogOfWar`
// (src/gameplay/fog/). This module never sees that type: it takes the grid as one state byte a
// cell — 0 unexplored, 1 explored, 2 visible, `FogOfWar::write_states` — and everything it adds is
// presentation, so nothing here can make two peers disagree:
//
//   * a TEMPORAL FADE: a cell revealed brightens over `reveal_seconds` and one left behind darkens
//     over `hide_seconds`, on the frame's clock, so a unit walking does not drag a hard-edged hole;
//   * SMOOTH EDGES: the texture holds one texel a cell and is sampled as four bilinear taps half a
//     cell apart, averaged — a tent two cells wide — the result eased by a smoothstep, so a
//     boundary is a soft ramp that rounds the cells' corners rather than a staircase;
//   * the SHADING: an explored cell is desaturated and darkened, a never-explored one is the
//     unexplored colour, and a visible one is the picture exactly as it was.
//
// Every one of those has a host twin here — `sample_visibility` and `shade_fog` — written in the
// order fog_of_war.slang computes them, which is what `render.fog_of_war` compares the device with.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/math/vec.h>
#include <cy/core/memory/allocator.h>
#include <cy/core/memory/array.h>

namespace cy::rendering::fog_of_war {

/// `cy::gameplay::fog::CellState`, value for value.
inline constexpr u8 kCellUnexplored = 0;
inline constexpr u8 kCellExplored = 1;
inline constexpr u8 kCellVisible = 2;

/// The visibility texture's format: red is how visible a cell is, green how explored, both
/// 0 to 255; blue is zero and alpha one.
inline constexpr u32 kTexelBytes = 4;

struct FogFadeSettings {
    /// Seconds a cell takes to go from not visible to visible, and from unexplored to explored.
    /// Zero snaps.
    f32 reveal_seconds = 0.25F;
    /// Seconds a cell takes to go from visible back to explored. Zero snaps.
    f32 hide_seconds = 0.75F;
};

/// How the fog is drawn. Linear scene-referred colour: the stage runs before exposure.
struct FogOfWarStyle {
    /// An explored cell's brightness, times the desaturated colour. [0, 1].
    f32 explored_brightness = 0.45F;
    /// How much of an explored cell's colour is kept: 0 grey, 1 the colour. [0, 1].
    f32 explored_saturation = 0.2F;
    /// A never-explored cell, whatever is there.
    Vec3 unexplored_colour{0.0F, 0.0F, 0.0F};
};

/// Where the grid lies in the world, in metres: the presentation copy of the simulation's
/// `FogGridDescription`, which holds the same numbers as Q32.32.
struct FogGridPlacement {
    f32 origin_x = 0.0F;
    f32 origin_z = 0.0F;
    f32 cell_size = 1.0F;
};

/// How visible and how explored a point is, each in [0, 1], after the smoothing.
struct FogSample {
    f32 visible = 0.0F;
    f32 explored = 0.0F;
};

/// One team's grid, faded and packed for upload.
class VisibilityField {
public:
    explicit VisibilityField(Allocator& allocator) noexcept;

    VisibilityField(const VisibilityField&) = delete;
    VisibilityField& operator=(const VisibilityField&) = delete;

    /// A grid of `width * height` cells, all unexplored.
    [[nodiscard]] Status create(u32 width, u32 height,
                                const FogFadeSettings& settings = {}) noexcept;
    [[nodiscard]] u32 width() const noexcept { return width_; }
    [[nodiscard]] u32 height() const noexcept { return height_; }
    [[nodiscard]] const FogFadeSettings& settings() const noexcept { return settings_; }

    /// What the simulation says now: one `kCell*` byte a cell, row-major with x fastest. The
    /// texels move toward it on `advance`. A byte past `kCellVisible` is refused.
    [[nodiscard]] Status set_states(Span<const u8> states) noexcept;
    /// Move every cell toward its state by `seconds` of fade.
    void advance(f32 seconds) noexcept;
    /// Every cell at its state at once: a first frame, a cut, a team switched.
    void snap() noexcept;
    /// Whether every cell has reached its state.
    [[nodiscard]] bool settled() const noexcept;

    /// The texture's bytes: `kTexelBytes` a cell, row-major with x fastest.
    [[nodiscard]] Span<const u8> texels() const noexcept { return texels_.span(); }
    /// A cell's faded levels, in [0, 1].
    [[nodiscard]] f32 visible(u32 x, u32 z) const noexcept;
    [[nodiscard]] f32 explored(u32 x, u32 z) const noexcept;

private:
    void pack() noexcept;

    u32 width_ = 0;
    u32 height_ = 0;
    FogFadeSettings settings_{};
    Array<u8> states_;
    /// The faded levels, two a cell: visible then explored.
    Array<f32> levels_;
    Array<u8> texels_;
};

/// Hermite easing, `t * t * (3 - 2 t)`, of a value clamped to [0, 1].
[[nodiscard]] f32 fog_smooth(f32 value) noexcept;

/// The field at a world position on the ground plane, exactly as fog_of_war.slang samples it:
/// grid coordinates from the placement, then four bilinear taps half a cell apart — each between
/// texel centres, the edge clamped, each channel an 8-bit unorm — averaged, then eased. A point
/// off the grid reads the nearest edge cell.
[[nodiscard]] FogSample sample_visibility(Span<const u8> texels, u32 width, u32 height,
                                          const FogGridPlacement& placement, f32 world_x,
                                          f32 world_z) noexcept;

/// The fogged colour, exactly as fog_of_war.slang shades it. A fully visible sample returns
/// `colour` unchanged, bit for bit.
[[nodiscard]] Vec3 shade_fog(Vec3 colour, const FogSample& sample,
                             const FogOfWarStyle& style) noexcept;

}  // namespace cy::rendering::fog_of_war
