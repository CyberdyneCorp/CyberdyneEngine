// The gizmo, drawn into the frame the engine publishes. See overlay.h.

#include "overlay.h"

#include <cmath>
#include <utility>

namespace cy::sample::editor_window {
namespace {

/// The axis triad, from `cy_editor_visual::axis::colour` in the dark theme.
///
/// **The one colour convention shared with every other tool**, which is why the numbers are copied
/// from the editor's own table rather than chosen: a gizmo whose X was a different red from the
/// inspector's X label would be two conventions.
constexpr u32 kAxisX = 0x00F2'615CU;
constexpr u32 kAxisY = 0x004F'C26BU;
constexpr u32 kAxisZ = 0x005B'9BF8U;
/// The screen-space affordances and the selection marker: the interface's own selection colour.
constexpr u32 kScreen = 0x00E8'E6E1U;
constexpr u32 kSelection = 0x00F2'B33DU;

/// The colour a handle is drawn in.
[[nodiscard]] u32 colour_of(render::GizmoHandle handle) noexcept {
    switch (handle) {
        case render::GizmoHandle::AxisX:
        case render::GizmoHandle::RingX:
        case render::GizmoHandle::BoxX:
        case render::GizmoHandle::PlaneYZ:
            return kAxisX;
        case render::GizmoHandle::AxisY:
        case render::GizmoHandle::RingY:
        case render::GizmoHandle::BoxY:
        case render::GizmoHandle::PlaneZX:
            return kAxisY;
        case render::GizmoHandle::AxisZ:
        case render::GizmoHandle::RingZ:
        case render::GizmoHandle::BoxZ:
        case render::GizmoHandle::PlaneXY:
            return kAxisZ;
        default:
            return kScreen;
    }
}

/// The active state, per `editor-visual-language`: **a luminance and saturation lift, never a
/// recolour.** An emphasised X handle is a brighter X, because recolouring would break the one
/// convention the axis triad is for.
[[nodiscard]] u32 emphasise(u32 colour) noexcept {
    u32 lifted = 0;
    // Channel by channel, whatever order they are in: a lift toward white is symmetric, so this
    // one loop is correct for 0xRRGGBB without knowing which byte is which.
    for (u32 shift = 0; shift < 24; shift += 8) {
        const u32 channel = (colour >> shift) & 0xFFU;
        // Toward white by a third: a lift a person reads as "this one", and one that keeps the hue.
        const u32 raised = channel + ((255U - channel) / 3U);
        lifted |= raised << shift;
    }
    return lifted;
}

void blend(const Canvas& canvas, i32 x, i32 y, u32 colour, f32 alpha) noexcept {
    if (x < 0 || y < 0 || std::cmp_greater_equal(x, canvas.width) ||
        std::cmp_greater_equal(y, canvas.height) || alpha <= 0.0F) {
        return;
    }
    const f32 weight = (alpha > 1.0F) ? 1.0F : alpha;
    u8* pixel =
        canvas.pixels + (((static_cast<usize>(y) * canvas.width) + static_cast<usize>(x)) * 4);
    // THE CANVAS IS RGBA AND THE CONSTANTS ARE 0xRRGGBB, so channel 0 is the HIGH byte. Indexing
    // them the other way round is what drew the X axis blue and the Z axis orange for the whole of
    // this artefact's first run — a mistake that is invisible in a diff and unmistakable in a
    // screenshot, which is why `the_axis_triad_is_drawn_in_the_editors_own_colours` exists.
    for (u32 channel = 0; channel < 3; ++channel) {
        const u32 shift = 16U - (channel * 8U);
        const f32 source = static_cast<f32>((colour >> shift) & 0xFFU);
        const f32 destination = static_cast<f32>(pixel[channel]);
        pixel[channel] =
            static_cast<u8>(std::lround((source * weight) + (destination * (1.0F - weight))));
    }
    pixel[3] = 0xFF;
}

/// A filled disc with a one-pixel soft edge, which is what keeps a six-pixel handle from looking
/// like a staircase at the size these are drawn at.
void disc(const Canvas& canvas, f32 centre_x, f32 centre_y, f32 radius, u32 colour) noexcept {
    const i32 first_x = static_cast<i32>(std::floor(centre_x - radius - 1.0F));
    const i32 last_x = static_cast<i32>(std::ceil(centre_x + radius + 1.0F));
    const i32 first_y = static_cast<i32>(std::floor(centre_y - radius - 1.0F));
    const i32 last_y = static_cast<i32>(std::ceil(centre_y + radius + 1.0F));
    for (i32 y = first_y; y <= last_y; ++y) {
        for (i32 x = first_x; x <= last_x; ++x) {
            const f32 dx = static_cast<f32>(x) + 0.5F - centre_x;
            const f32 dy = static_cast<f32>(y) + 0.5F - centre_y;
            const f32 distance = std::sqrt((dx * dx) + (dy * dy));
            blend(canvas, x, y, colour, radius + 0.5F - distance);
        }
    }
}

/// An unfilled ring of `thickness` pixels. What a rotation ring and the selection marker are.
void ring(const Canvas& canvas, f32 centre_x, f32 centre_y, f32 radius, f32 thickness,
          u32 colour) noexcept {
    const i32 first_x = static_cast<i32>(std::floor(centre_x - radius - thickness));
    const i32 last_x = static_cast<i32>(std::ceil(centre_x + radius + thickness));
    const i32 first_y = static_cast<i32>(std::floor(centre_y - radius - thickness));
    const i32 last_y = static_cast<i32>(std::ceil(centre_y + radius + thickness));
    for (i32 y = first_y; y <= last_y; ++y) {
        for (i32 x = first_x; x <= last_x; ++x) {
            const f32 dx = static_cast<f32>(x) + 0.5F - centre_x;
            const f32 dy = static_cast<f32>(y) + 0.5F - centre_y;
            const f32 distance = std::sqrt((dx * dx) + (dy * dy));
            blend(canvas, x, y, colour, thickness - std::abs(distance - radius));
        }
    }
}

/// A line of `thickness` pixels from one point to another: a gizmo's shaft.
void line(const Canvas& canvas, f32 from_x, f32 from_y, f32 to_x, f32 to_y, f32 thickness,
          u32 colour) noexcept {
    const f32 dx = to_x - from_x;
    const f32 dy = to_y - from_y;
    const f32 length = std::sqrt((dx * dx) + (dy * dy));
    if (length < 0.5F) {
        return;
    }
    const i32 steps = static_cast<i32>(length * 2.0F) + 1;
    for (i32 step = 0; step <= steps; ++step) {
        const f32 t = static_cast<f32>(step) / static_cast<f32>(steps);
        disc(canvas, from_x + (dx * t), from_y + (dy * t), thickness, colour);
    }
}

/// A filled square, axis-aligned in screen space: a plane handle and the centre cube.
void square(const Canvas& canvas, f32 centre_x, f32 centre_y, f32 half, u32 colour) noexcept {
    const i32 first_x = static_cast<i32>(std::floor(centre_x - half));
    const i32 last_x = static_cast<i32>(std::ceil(centre_x + half));
    const i32 first_y = static_cast<i32>(std::floor(centre_y - half));
    const i32 last_y = static_cast<i32>(std::ceil(centre_y + half));
    for (i32 y = first_y; y <= last_y; ++y) {
        for (i32 x = first_x; x <= last_x; ++x) {
            blend(canvas, x, y, colour, 1.0F);
        }
    }
}

/// Whether a handle is drawn as a shaft with a head, rather than as a mark on its own.
[[nodiscard]] bool is_axial(render::GizmoHandle handle) noexcept {
    switch (handle) {
        case render::GizmoHandle::AxisX:
        case render::GizmoHandle::AxisY:
        case render::GizmoHandle::AxisZ:
        case render::GizmoHandle::BoxX:
        case render::GizmoHandle::BoxY:
        case render::GizmoHandle::BoxZ:
            return true;
        default:
            return false;
    }
}

[[nodiscard]] bool is_box(render::GizmoHandle handle) noexcept {
    switch (handle) {
        case render::GizmoHandle::BoxX:
        case render::GizmoHandle::BoxY:
        case render::GizmoHandle::BoxZ:
        case render::GizmoHandle::PlaneXY:
        case render::GizmoHandle::PlaneYZ:
        case render::GizmoHandle::PlaneZX:
        case render::GizmoHandle::Uniform:
            return true;
        default:
            return false;
    }
}

}  // namespace

void draw_gizmo(const Canvas& canvas, const render::GizmoLayout& layout,
                render::GizmoHandle hovered) noexcept {
    if (canvas.pixels == nullptr || layout.empty()) {
        return;
    }
    // The shafts first, so that a head is never drawn under the line it belongs to.
    for (const render::GizmoHandleSpot& spot : layout.spots) {
        if (!is_axial(spot.handle)) {
            continue;
        }
        const u32 colour = colour_of(spot.handle);
        line(canvas, layout.centre_x, layout.centre_y, spot.x, spot.y, 1.2F,
             (spot.handle == hovered) ? emphasise(colour) : colour);
    }
    for (const render::GizmoHandleSpot& spot : layout.spots) {
        const bool active = spot.handle == hovered;
        const u32 colour = active ? emphasise(colour_of(spot.handle)) : colour_of(spot.handle);
        switch (spot.handle) {
            case render::GizmoHandle::ScreenRing:
                ring(canvas, spot.x, spot.y, spot.radius, active ? 2.0F : 1.2F, colour);
                break;
            case render::GizmoHandle::RingX:
            case render::GizmoHandle::RingY:
            case render::GizmoHandle::RingZ:
                // A ring's published spot is the point of the circle nearest the camera; the whole
                // circle is what a person sees, and it is drawn about the gizmo's centre through
                // that point. See `cy/servers/render/gizmo.h` for why the layout carries a point.
                ring(canvas, layout.centre_x, layout.centre_y,
                     std::sqrt(((spot.x - layout.centre_x) * (spot.x - layout.centre_x)) +
                               ((spot.y - layout.centre_y) * (spot.y - layout.centre_y))),
                     active ? 2.0F : 1.2F, colour);
                disc(canvas, spot.x, spot.y, spot.radius, colour);
                break;
            case render::GizmoHandle::Screen:
                ring(canvas, spot.x, spot.y, spot.radius, active ? 2.0F : 1.2F, colour);
                break;
            default:
                if (is_box(spot.handle)) {
                    square(canvas, spot.x, spot.y, spot.radius * 0.8F, colour);
                } else {
                    disc(canvas, spot.x, spot.y, spot.radius, colour);
                }
                break;
        }
    }
}

void draw_selection_marker(const Canvas& canvas, f32 x, f32 y, f32 radius) noexcept {
    if (canvas.pixels == nullptr) {
        return;
    }
    ring(canvas, x, y, radius, 1.5F, kSelection);
}

}  // namespace cy::sample::editor_window
