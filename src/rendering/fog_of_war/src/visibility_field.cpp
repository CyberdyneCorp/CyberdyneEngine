// SPDX-License-Identifier: MIT
#include <cy/rendering/fog_of_war/visibility_field.h>

#include <algorithm>
#include <cmath>

namespace cy::rendering::fog_of_war {
namespace {

/// Each level as the 8-bit unorm the device reads: rounded to nearest.
[[nodiscard]] u8 to_unorm(f32 level) noexcept {
    const f32 clamped = std::clamp(level, 0.0F, 1.0F);
    return static_cast<u8>(std::lround(clamped * 255.0F));
}

/// `current` moved toward `target` by `step`, never past it.
[[nodiscard]] f32 approach(f32 current, f32 target, f32 step) noexcept {
    if (current < target) {
        return std::min(current + step, target);
    }
    return std::max(current - step, target);
}

/// How far a fade of `seconds` moves in `elapsed`: everything when the fade is a snap.
[[nodiscard]] f32 fade_step(f32 elapsed, f32 seconds) noexcept {
    return seconds > 0.0F ? elapsed / seconds : 1.0F;
}

[[nodiscard]] f32 mix(f32 a, f32 b, f32 t) noexcept {
    return a + ((b - a) * t);
}

[[nodiscard]] Vec3 mix(Vec3 a, Vec3 b, f32 t) noexcept {
    return Vec3{mix(a.x, b.x, t), mix(a.y, b.y, t), mix(a.z, b.z, t)};
}

}  // namespace

VisibilityField::VisibilityField(Allocator& allocator) noexcept
    : states_(allocator), levels_(allocator), texels_(allocator) {}

Status VisibilityField::create(u32 width, u32 height, const FogFadeSettings& settings) noexcept {
    if (width == 0 || height == 0) {
        return fail(ErrorCode::InvalidArgument, "fog of war: the visibility field is empty");
    }
    if (!(settings.reveal_seconds >= 0.0F) || !(settings.hide_seconds >= 0.0F)) {
        return fail(ErrorCode::InvalidArgument, "fog of war: a fade time is negative");
    }
    const usize cells = static_cast<usize>(width) * height;
    if (Status sized = states_.resize(cells); !sized) {
        return sized;
    }
    if (Status sized = levels_.resize(cells * 2U); !sized) {
        return sized;
    }
    if (Status sized = texels_.resize(cells * kTexelBytes); !sized) {
        return sized;
    }
    std::ranges::fill(states_, kCellUnexplored);
    std::ranges::fill(levels_, 0.0F);
    width_ = width;
    height_ = height;
    settings_ = settings;
    pack();
    return ok();
}

Status VisibilityField::set_states(Span<const u8> states) noexcept {
    if (states.size() != states_.size()) {
        return fail(ErrorCode::InvalidArgument,
                    "fog of war: the states are not one byte a cell of the field");
    }
    for (const u8 state : states) {
        if (state > kCellVisible) {
            return fail(ErrorCode::InvalidArgument, "fog of war: a cell state is past visible");
        }
    }
    std::ranges::copy(states, states_.begin());
    return ok();
}

void VisibilityField::advance(f32 seconds) noexcept {
    const f32 elapsed = std::max(seconds, 0.0F);
    const f32 reveal = fade_step(elapsed, settings_.reveal_seconds);
    const f32 hide = fade_step(elapsed, settings_.hide_seconds);
    for (usize cell = 0; cell < states_.size(); ++cell) {
        const u8 state = states_[cell];
        f32& visible = levels_[cell * 2U];
        f32& explored = levels_[(cell * 2U) + 1U];
        const f32 visible_target = state == kCellVisible ? 1.0F : 0.0F;
        visible = approach(visible, visible_target, visible < visible_target ? reveal : hide);
        // Exploration only grows in the simulation; a cell that went back to unexplored is a new
        // session or another team, and snaps.
        explored = state == kCellUnexplored ? 0.0F : approach(explored, 1.0F, reveal);
    }
    pack();
}

void VisibilityField::snap() noexcept {
    for (usize cell = 0; cell < states_.size(); ++cell) {
        levels_[cell * 2U] = states_[cell] == kCellVisible ? 1.0F : 0.0F;
        levels_[(cell * 2U) + 1U] = states_[cell] == kCellUnexplored ? 0.0F : 1.0F;
    }
    pack();
}

bool VisibilityField::settled() const noexcept {
    for (usize cell = 0; cell < states_.size(); ++cell) {
        const f32 visible = states_[cell] == kCellVisible ? 1.0F : 0.0F;
        const f32 explored = states_[cell] == kCellUnexplored ? 0.0F : 1.0F;
        if (levels_[cell * 2U] != visible || levels_[(cell * 2U) + 1U] != explored) {
            return false;
        }
    }
    return true;
}

f32 VisibilityField::visible(u32 x, u32 z) const noexcept {
    return x < width_ && z < height_ ? levels_[((static_cast<usize>(z) * width_) + x) * 2U] : 0.0F;
}

f32 VisibilityField::explored(u32 x, u32 z) const noexcept {
    return x < width_ && z < height_ ? levels_[(((static_cast<usize>(z) * width_) + x) * 2U) + 1U]
                                     : 0.0F;
}

void VisibilityField::pack() noexcept {
    for (usize cell = 0; cell < states_.size(); ++cell) {
        u8* texel = texels_.data() + (cell * kTexelBytes);
        texel[0] = to_unorm(levels_[cell * 2U]);
        texel[1] = to_unorm(levels_[(cell * 2U) + 1U]);
        texel[2] = 0;
        texel[3] = 255;
    }
}

f32 fog_smooth(f32 value) noexcept {
    const f32 t = std::clamp(value, 0.0F, 1.0F);
    return t * t * (3.0F - (2.0F * t));
}

namespace {

/// The texels bilinearly between their centres at `(gx, gz)`, in texels, the edge clamped: the
/// visible and the explored level.
void bilinear(Span<const u8> texels, u32 width, u32 height, f32 gx, f32 gz,
              f32 (&level)[2]) noexcept {
    const f32 fx_floor = std::floor(gx);
    const f32 fz_floor = std::floor(gz);
    const f32 fx = gx - fx_floor;
    const f32 fz = gz - fz_floor;
    const auto clamp_x = [width](f32 value) {
        return static_cast<usize>(std::clamp(value, 0.0F, static_cast<f32>(width - 1U)));
    };
    const auto clamp_z = [height](f32 value) {
        return static_cast<usize>(std::clamp(value, 0.0F, static_cast<f32>(height - 1U)));
    };
    const usize x0 = clamp_x(fx_floor);
    const usize x1 = clamp_x(fx_floor + 1.0F);
    const usize z0 = clamp_z(fz_floor);
    const usize z1 = clamp_z(fz_floor + 1.0F);
    const auto channel = [&](usize x, usize z, usize which) {
        return static_cast<f32>(texels[(((z * width) + x) * 4U) + which]) / 255.0F;
    };
    for (usize which = 0; which < 2U; ++which) {
        const f32 near = mix(channel(x0, z0, which), channel(x1, z0, which), fx);
        const f32 far = mix(channel(x0, z1, which), channel(x1, z1, which), fx);
        level[which] = mix(near, far, fz);
    }
}

}  // namespace

FogSample sample_visibility(Span<const u8> texels, u32 width, u32 height,
                            const FogGridPlacement& placement, f32 world_x, f32 world_z) noexcept {
    if (width == 0 || height == 0 || texels.size() < static_cast<usize>(width) * height * 4U ||
        !(placement.cell_size > 0.0F)) {
        return FogSample{};
    }
    // Grid coordinates with texel i's centre at i: then four bilinear taps half a cell apart,
    // averaged — a tent two cells wide that rounds the cells' corners — and eased.
    const f32 inverse_cell = 1.0F / placement.cell_size;
    const f32 gx = ((world_x - placement.origin_x) * inverse_cell) - 0.5F;
    const f32 gz = ((world_z - placement.origin_z) * inverse_cell) - 0.5F;
    f32 sum[2] = {0.0F, 0.0F};
    for (u32 tap = 0; tap < 4U; ++tap) {
        const f32 ox = (tap & 1U) != 0U ? 0.5F : -0.5F;
        const f32 oz = (tap & 2U) != 0U ? 0.5F : -0.5F;
        f32 level[2] = {0.0F, 0.0F};
        bilinear(texels, width, height, gx + ox, gz + oz, level);
        sum[0] += level[0];
        sum[1] += level[1];
    }
    FogSample sample;
    sample.visible = fog_smooth(sum[0] * 0.25F);
    sample.explored = fog_smooth(sum[1] * 0.25F);
    return sample;
}

Vec3 shade_fog(Vec3 colour, const FogSample& sample, const FogOfWarStyle& style) noexcept {
    if (sample.visible >= 1.0F) {
        return colour;
    }
    const f32 luma = (0.2126F * colour.x) + (0.7152F * colour.y) + (0.0722F * colour.z);
    const Vec3 grey{luma, luma, luma};
    const Vec3 muted = mix(grey, colour, style.explored_saturation);
    const Vec3 explored{muted.x * style.explored_brightness, muted.y * style.explored_brightness,
                        muted.z * style.explored_brightness};
    const Vec3 hidden = mix(style.unexplored_colour, explored, sample.explored);
    return mix(hidden, colour, sample.visible);
}

}  // namespace cy::rendering::fog_of_war
