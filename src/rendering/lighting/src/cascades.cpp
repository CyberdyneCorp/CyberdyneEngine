#include <cy/rendering/lighting/cascades.h>

#include <cy/core/math/projection.h>
#include <cy/core/math/scalar.h>

#include <cmath>

namespace cy::rendering {
namespace {

/// The eight world-space corners of the sub-frustum between two distances.
void sub_frustum_corners(const CascadeCamera& camera, f32 near_distance, f32 far_distance,
                         Vec3 (&corners)[8]) noexcept {
    const f32 tan_half = std::tan(camera.fov_y_radians * 0.5F);
    const f32 near_height = tan_half * near_distance;
    const f32 near_width = near_height * camera.aspect;
    const f32 far_height = tan_half * far_distance;
    const f32 far_width = far_height * camera.aspect;

    const Vec3 near_center = camera.position + (camera.forward * near_distance);
    const Vec3 far_center = camera.position + (camera.forward * far_distance);

    corners[0] = near_center + (camera.up * near_height) + (camera.right * near_width);
    corners[1] = near_center + (camera.up * near_height) - (camera.right * near_width);
    corners[2] = near_center - (camera.up * near_height) + (camera.right * near_width);
    corners[3] = near_center - (camera.up * near_height) - (camera.right * near_width);
    corners[4] = far_center + (camera.up * far_height) + (camera.right * far_width);
    corners[5] = far_center + (camera.up * far_height) - (camera.right * far_width);
    corners[6] = far_center - (camera.up * far_height) + (camera.right * far_width);
    corners[7] = far_center - (camera.up * far_height) - (camera.right * far_width);
}

/// The bounding sphere of eight corners. Centre is their centroid, which for a frustum's corners is
/// on the view axis by symmetry; radius is the furthest corner. Not the minimal enclosing sphere,
/// and deliberately so: the minimal sphere's centre moves discontinuously as the support set
/// changes, which reintroduces exactly the swimming this whole file exists to remove.
void bounding_sphere(const Vec3 (&corners)[8], Vec3& center, f32& radius) noexcept {
    center = Vec3{0.0F, 0.0F, 0.0F};
    for (const Vec3& corner : corners) {
        center = center + corner;
    }
    center = center * (1.0F / 8.0F);
    f32 furthest = 0.0F;
    for (const Vec3& corner : corners) {
        furthest = math::max(furthest, length_squared(corner - center));
    }
    radius = std::sqrt(furthest);
}

/// An up vector that is not parallel to the light. Choosing it from the light alone — rather than
/// from the camera — is part of stabilisation: an up vector that followed the camera would rotate
/// the shadow projection every time the camera rolled.
[[nodiscard]] Vec3 stable_up_for(Vec3 light_direction) noexcept {
    return std::abs(light_direction.y) > 0.99F ? Vec3{0.0F, 0.0F, 1.0F} : Vec3{0.0F, 1.0F, 0.0F};
}

}  // namespace

Status compute_cascade_splits(const CascadeSettings& settings, f32 near_plane,
                              Span<f32> out) noexcept {
    if (settings.count == 0 || settings.count > kMaxShadowCascades) {
        return fail(ErrorCode::InvalidArgument, "cascades: count must be 1..kMaxShadowCascades");
    }
    if (out.size() < settings.count) {
        return fail(ErrorCode::InvalidArgument, "cascades: the output span is too small");
    }
    const f32 near_distance = math::max(near_plane, 1e-3F);
    const f32 far_distance = math::max(settings.shadow_distance, near_distance * 2.0F);
    const f32 blend = math::clamp(settings.split_blend, 0.0F, 1.0F);
    const f32 ratio = far_distance / near_distance;
    const f32 range = far_distance - near_distance;

    for (u32 index = 0; index < settings.count; ++index) {
        const f32 fraction = static_cast<f32>(index + 1) / static_cast<f32>(settings.count);
        // The two classical distributions, mixed. Logarithmic matches how perspective distributes
        // detail; uniform keeps the near cascades from collapsing onto the near plane.
        const f32 logarithmic = near_distance * std::pow(ratio, fraction);
        const f32 uniform = near_distance + (range * fraction);
        out[index] = (blend * logarithmic) + ((1.0F - blend) * uniform);
    }
    return ok();
}

Status compute_cascades(const CascadeSettings& settings, const CascadeCamera& camera,
                        Vec3 light_direction, Span<ShadowCascade> out) noexcept {
    if (out.size() < settings.count) {
        return fail(ErrorCode::InvalidArgument, "cascades: the output span is too small");
    }
    if (settings.resolution == 0) {
        return fail(ErrorCode::InvalidArgument, "cascades: resolution must be non-zero");
    }
    f32 splits[kMaxShadowCascades] = {};
    if (Status computed = compute_cascade_splits(settings, camera.near_plane,
                                                 Span<f32>(splits, kMaxShadowCascades));
        !computed) {
        return computed;
    }

    const Vec3 direction = normalize(light_direction);
    const Vec3 up = stable_up_for(direction);
    f32 near_distance = math::max(camera.near_plane, 1e-3F);

    for (u32 index = 0; index < settings.count; ++index) {
        ShadowCascade& cascade = out[index];
        cascade.near_distance = near_distance;
        cascade.far_distance = splits[index];

        Vec3 corners[8];
        sub_frustum_corners(camera, cascade.near_distance, cascade.far_distance, corners);
        bounding_sphere(corners, cascade.center, cascade.radius);
        cascade.radius = math::max(cascade.radius, 1e-3F);

        // The depth range: from behind the casters to the far side of the sphere. Pulling the eye
        // back by `caster_extent_scale · radius` is the "fit the near plane to the actual caster
        // bounds" requirement expressed as a knob rather than as a second scene traversal — the
        // exact caster bounds are the shadow cull's, and re-deriving them here would mean culling
        // twice.
        const f32 extent = cascade.radius * math::max(settings.caster_extent_scale, 0.0F);
        cascade.texel_world_size = (2.0F * cascade.radius) / static_cast<f32>(settings.resolution);

        // TEXEL SNAPPING, AND THE PART THAT IS EASY TO GET WRONG. The centre must be snapped to a
        // lattice that is FIXED IN THE LIGHT'S OWN SPACE, not to one derived from the centre — a
        // projection built with its eye placed at the centre has the centre at the light-space
        // origin by construction, so snapping it there rounds zero to zero and does nothing at all.
        //
        // So the basis is built first, from the light direction alone; the centre is expressed in
        // it; its lateral coordinates are rounded to whole texels; and the eye is placed behind the
        // SNAPPED centre. The lattice is then the same one every frame, and the projection can only
        // move in whole texels — which is what stops the shadow edges crawling.
        const Mat4 basis = look_at(Vec3{0.0F, 0.0F, 0.0F}, direction, up);
        const Vec3 light_right{basis.columns[0].x, basis.columns[1].x, basis.columns[2].x};
        const Vec3 light_up{basis.columns[0].y, basis.columns[1].y, basis.columns[2].y};
        const Vec3 light_back{basis.columns[0].z, basis.columns[1].z, basis.columns[2].z};

        const f32 snapped_x =
            std::floor(dot(light_right, cascade.center) / cascade.texel_world_size) *
            cascade.texel_world_size;
        const f32 snapped_y = std::floor(dot(light_up, cascade.center) / cascade.texel_world_size) *
                              cascade.texel_world_size;
        const Vec3 snapped_center = (light_right * snapped_x) + (light_up * snapped_y) +
                                    (light_back * dot(light_back, cascade.center));

        const Vec3 eye = snapped_center - (direction * (cascade.radius + extent));
        cascade.view = look_at(eye, snapped_center, up);
        cascade.projection =
            orthographic_reversed_z(-cascade.radius, cascade.radius, -cascade.radius,
                                    cascade.radius, 0.0F, (2.0F * cascade.radius) + extent);
        cascade.view_projection = cascade.projection * cascade.view;

        near_distance = cascade.far_distance;
    }
    return ok();
}

CascadeLookup select_cascade(const CascadeSettings& settings, Span<const ShadowCascade> cascades,
                             f32 view_depth) noexcept {
    CascadeLookup lookup;
    if (cascades.empty()) {
        lookup.fade = 0.0F;
        return lookup;
    }
    const auto count = static_cast<u32>(cascades.size());
    lookup.index = count - 1;
    for (u32 index = 0; index < count; ++index) {
        if (view_depth <= cascades[index].far_distance) {
            lookup.index = index;
            break;
        }
    }

    // The transition band, measured back from this cascade's far edge. Inside it both cascades are
    // sampled and blended, which is what removes the seam a hard switch leaves.
    const ShadowCascade& cascade = cascades[lookup.index];
    if (lookup.index + 1 < count && settings.transition_fraction > 0.0F) {
        const f32 extent = cascade.far_distance - cascade.near_distance;
        const f32 band = extent * math::clamp(settings.transition_fraction, 0.0F, 1.0F);
        if (band > 0.0F && view_depth > cascade.far_distance - band) {
            lookup.blend =
                math::clamp((view_depth - (cascade.far_distance - band)) / band, 0.0F, 1.0F);
        }
    }

    // The distant fade. Past the last cascade there is nothing to sample, so shadowing has to reach
    // zero smoothly rather than stop.
    const f32 distance = math::max(settings.shadow_distance, 1e-3F);
    const f32 fade_start = distance * math::clamp(settings.fade_start_fraction, 0.0F, 1.0F);
    if (view_depth > fade_start) {
        const f32 fade_range = math::max(distance - fade_start, 1e-3F);
        lookup.fade = math::clamp(1.0F - ((view_depth - fade_start) / fade_range), 0.0F, 1.0F);
    }
    return lookup;
}

}  // namespace cy::rendering
