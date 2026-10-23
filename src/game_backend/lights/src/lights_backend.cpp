// SPDX-License-Identifier: MIT
#include <cy/game_backend/lights_backend.h>

#include <cy/abi/errors.h>
#include <cy/abi/game/services.h>
#include <cy/core/math/quat.h>

#include <cmath>

namespace cy::game_backend {
namespace {

// THE ABI'S KINDS ARE THE ENGINE'S NUMBERS, asserted where both are in view: `light_of` casts one
// to the other.
static_assert(CY_LIGHT_POINT == static_cast<u32>(render::LightKind::Point));
static_assert(CY_LIGHT_SPOT == static_cast<u32>(render::LightKind::Spot));
static_assert(CY_LIGHT_RECT == static_cast<u32>(render::LightKind::Rect));
static_assert(CY_LIGHT_DISC == static_cast<u32>(render::LightKind::Disc));
static_assert(CY_LIGHT_SPHERE == static_cast<u32>(render::LightKind::Sphere));
static_assert(CY_LIGHT_TUBE == static_cast<u32>(render::LightKind::Tube));

[[nodiscard]] Quat rotation_of(const CyPose& pose) noexcept {
    const Quat rotation{pose.rotation[0], pose.rotation[1], pose.rotation[2], pose.rotation[3]};
    const f32 norm = (rotation.x * rotation.x) + (rotation.y * rotation.y) +
                     (rotation.z * rotation.z) + (rotation.w * rotation.w);
    // An all-zero quaternion is the identity, as cy_abi.h says of every `CyPose`.
    if (!(norm > 1.0e-12F)) {
        return Quat::identity();
    }
    const f32 scale = 1.0F / std::sqrt(norm);
    return Quat{rotation.x * scale, rotation.y * scale, rotation.z * scale, rotation.w * scale};
}

[[nodiscard]] rendering::TransientFade fade_of(u32 fade) noexcept {
    switch (fade) {
        case CY_LIGHT_FADE_NONE:
            return rendering::TransientFade::None;
        case CY_LIGHT_FADE_LINEAR:
            return rendering::TransientFade::Linear;
        default:
            return rendering::TransientFade::Quadratic;
    }
}

[[nodiscard]] u32 fade_word(rendering::TransientFade fade) noexcept {
    switch (fade) {
        case rendering::TransientFade::None:
            return CY_LIGHT_FADE_NONE;
        case rendering::TransientFade::Linear:
            return CY_LIGHT_FADE_LINEAR;
        case rendering::TransientFade::Quadratic:
        case rendering::TransientFade::Count:
            break;
    }
    return CY_LIGHT_FADE_QUADRATIC;
}

/// The engine's error as the ABI's code, reported.
[[nodiscard]] CyResult refused(const Error& error) noexcept {
    switch (error.code) {
        case ErrorCode::OutOfRange:
            return abi::report(CY_RESULT_OUT_OF_RANGE, error.message);
        case ErrorCode::NotFound:
            return abi::report(CY_RESULT_NOT_FOUND, error.message);
        default:
            return abi::report(CY_RESULT_INVALID_ARGUMENT, error.message);
    }
}

}  // namespace

render::LightDescription light_of(const CyLightDesc& desc) noexcept {
    render::LightDescription light;
    // The ABI's kinds are the engine's numbers: see the assertions at the top of this file.
    light.kind = static_cast<render::LightKind>(desc.kind);
    light.transform.translation =
        Vec3{desc.pose.position[0], desc.pose.position[1], desc.pose.position[2]};
    light.transform.rotation = rotation_of(desc.pose);
    light.color[0] = desc.color[0];
    light.color[1] = desc.color[1];
    light.color[2] = desc.color[2];
    light.intensity = desc.intensity;
    light.range = desc.range;
    light.inner_cone_radians = desc.inner_cone;
    light.outer_cone_radians = desc.outer_cone;
    light.source_width = desc.size[0];
    light.source_height = desc.size[1];
    light.two_sided = (desc.flags & CY_LIGHT_TWO_SIDED) != 0U;
    light.layer_mask = desc.layer_mask == 0U ? render::kAllLayers : desc.layer_mask;
    light.casts_shadow = false;
    light.mobility = render::LightMobility::Movable;
    return light;
}

CyLightDesc desc_of(const render::LightDescription& light, f32 lifetime,
                    rendering::TransientFade fade) noexcept {
    CyLightDesc desc{};
    desc.struct_size = static_cast<u32>(sizeof(CyLightDesc));
    desc.kind = static_cast<u32>(light.kind);
    desc.pose.position[0] = light.transform.translation.x;
    desc.pose.position[1] = light.transform.translation.y;
    desc.pose.position[2] = light.transform.translation.z;
    desc.pose.rotation[0] = light.transform.rotation.x;
    desc.pose.rotation[1] = light.transform.rotation.y;
    desc.pose.rotation[2] = light.transform.rotation.z;
    desc.pose.rotation[3] = light.transform.rotation.w;
    desc.color[0] = light.color[0];
    desc.color[1] = light.color[1];
    desc.color[2] = light.color[2];
    desc.intensity = light.intensity;
    desc.range = light.range;
    desc.inner_cone = light.inner_cone_radians;
    desc.outer_cone = light.outer_cone_radians;
    desc.size[0] = light.source_width;
    desc.size[1] = light.source_height;
    desc.lifetime = lifetime;
    desc.fade = fade_word(fade);
    desc.flags = light.two_sided ? CY_LIGHT_TWO_SIDED : 0U;
    desc.layer_mask = light.layer_mask == render::kAllLayers ? 0U : light.layer_mask;
    return desc;
}

LightsAdapter::LightsAdapter(Allocator& allocator) noexcept : pool_(allocator) {}

Status LightsAdapter::create(u32 capacity) noexcept {
    return pool_.reserve(capacity);
}

void LightsAdapter::advance(f32 seconds) noexcept {
    pool_.advance(seconds);
}

Expected<u32, Error> LightsAdapter::append_to(Array<render::LightDescription>& out) const noexcept {
    return pool_.append_to(out);
}

CyResult LightsAdapter::spawn(const CyLightDesc& desc, CyLight& out) noexcept {
    rendering::TransientLightDesc transient;
    transient.light = light_of(desc);
    transient.lifetime_seconds = desc.lifetime;
    transient.fade = fade_of(desc.fade);
    Expected<rendering::TransientLight, Error> made = pool_.spawn(transient);
    if (!made.has_value()) {
        return refused(made.error());
    }
    out = *made;
    return CY_RESULT_OK;
}

CyResult LightsAdapter::update(CyLight light, const CyLightDesc& desc) noexcept {
    if (Status updated = pool_.update(light, light_of(desc)); !updated) {
        return refused(updated.error());
    }
    return CY_RESULT_OK;
}

CyResult LightsAdapter::destroy(CyLight light) noexcept {
    if (Status killed = pool_.kill(light); !killed) {
        return refused(killed.error());
    }
    return CY_RESULT_OK;
}

CyResult LightsAdapter::alive(CyLight light, u32& out) const noexcept {
    out = pool_.alive(light) ? 1U : 0U;
    return CY_RESULT_OK;
}

CyResult LightsAdapter::status(CyLightStatus& out) const noexcept {
    const rendering::TransientLightStatus& status = pool_.status();
    out.live = status.live;
    out.capacity = status.capacity;
    out.spawned = status.spawned;
    out.expired = status.expired;
    out.refused = status.refused;
    return CY_RESULT_OK;
}

void bind_lights(cy::abi::Host& host, LightsAdapter* adapter) noexcept {
    host.game.lights = adapter;
}

}  // namespace cy::game_backend
