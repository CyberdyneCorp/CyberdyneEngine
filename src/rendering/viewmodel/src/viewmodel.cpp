// SPDX-License-Identifier: MIT
// The first-person weapon view. See cy/rendering/viewmodel/viewmodel.h.

#include <cy/rendering/viewmodel/viewmodel.h>

#include <cy/core/math/shapes.h>

#include <cmath>
#include <numbers>

namespace cy::rendering::viewmodel {
namespace {

/// Copy a `Mat4` out as four ROWS — `cy/frame.slang` dots rows, for the reason its header gives.
/// The same transcription `pipeline::upload_for` makes for the world's rows.
void write_rows(const Mat4& matrix, f32 out[16]) noexcept {
    for (u32 row = 0; row < 4; ++row) {
        for (u32 column = 0; column < 4; ++column) {
            out[(row * 4U) + column] = matrix.columns[column][row];
        }
    }
}

}  // namespace

Status validate(const ViewmodelSettings& settings) noexcept {
    const f32 fov = settings.vertical_fov_radians;
    if (!std::isfinite(fov) || fov <= 0.0F || fov >= std::numbers::pi_v<f32>) {
        return fail(ErrorCode::InvalidArgument,
                    "a weapon view's field of view must be between zero and pi radians");
    }
    if (!std::isfinite(settings.near_plane) || settings.near_plane <= 0.0F) {
        return fail(ErrorCode::InvalidArgument, "a weapon view's near plane must be positive");
    }
    if (!std::isfinite(settings.far_plane) || settings.far_plane <= settings.near_plane) {
        return fail(ErrorCode::InvalidArgument,
                    "a weapon view's far plane must be finite and beyond its near plane: the "
                    "weapon view is culled against it");
    }
    const f32 fraction = settings.depth_fraction;
    if (!std::isfinite(fraction) || fraction <= 0.0F || fraction > 0.5F) {
        return fail(ErrorCode::InvalidArgument,
                    "a weapon view's depth fraction must be in (0, 0.5]: none leaves the weapon no "
                    "depth to order itself by, and more than half takes the range from the world");
    }
    return ok();
}

Mat4 compress_depth(const Mat4& projection, f32 fraction) noexcept {
    Mat4 squeezed = projection;
    const f32 keep = 1.0F - fraction;
    for (Vec4& column : squeezed.columns) {
        column.z = (keep * column.w) + (fraction * column.z);
    }
    return squeezed;
}

Mat4 viewmodel_projection(const ViewmodelSettings& settings, f32 aspect, bool compressed) noexcept {
    render::Projection lens;
    lens.kind = render::ProjectionKind::Perspective;
    lens.fov_y_radians = settings.vertical_fov_radians;
    lens.near_plane = settings.near_plane;
    lens.far_plane = settings.far_plane;
    const Mat4 projection = lens.matrix(aspect);
    return compressed ? compress_depth(projection, settings.depth_fraction) : projection;
}

f32 guaranteed_clear_distance(f32 world_near_plane, f32 fraction) noexcept {
    return world_near_plane / (1.0F - fraction);
}

Expected<ViewmodelSettings, Error> settings_from_views(
    const render::ViewDescription& primary, const render::ViewDescription& weapon,
    const ViewmodelShadowPolicy& shadows) noexcept {
    if (weapon.purpose != render::ViewPurpose::Viewmodel ||
        primary.purpose == render::ViewPurpose::Viewmodel) {
        return fail(ErrorCode::InvalidArgument,
                    "a weapon view is a view of purpose Viewmodel beside a primary view");
    }
    // ONE EVALUATED CAMERA, which is the requirement's own sentence: "both SHALL be render views
    // derived from one evaluated camera". `camera::produce_view` copies the pose, so two views of
    // one camera carry the same pose bit for bit; anything else is a second camera.
    if (!(primary.camera == weapon.camera)) {
        return fail(
            ErrorCode::InvalidArgument,
            "the weapon view and the primary view are not one camera's: their poses differ");
    }
    if (primary.history_id == weapon.history_id) {
        return fail(ErrorCode::InvalidArgument,
                    "the weapon view shares the primary view's history identity; their temporal "
                    "histories must stay apart (`camera::RenderViewRequest::view_key`)");
    }
    if (weapon.projection.kind != render::ProjectionKind::Perspective) {
        return fail(ErrorCode::InvalidArgument, "a weapon view is a perspective view");
    }
    if (weapon.layer_mask == render::kNoLayers) {
        return fail(ErrorCode::InvalidArgument, "a weapon view that draws no layer draws nothing");
    }
    ViewmodelSettings settings;
    settings.layers = weapon.layer_mask;
    settings.vertical_fov_radians = weapon.projection.fov_y_radians;
    settings.near_plane = weapon.projection.near_plane;
    // An infinite far plane — the camera's default — is a finite one here, because the weapon view
    // is culled against it. See `kDefaultViewmodelFar`.
    settings.far_plane = weapon.projection.far_plane > weapon.projection.near_plane
                             ? weapon.projection.far_plane
                             : kDefaultViewmodelFar;
    settings.shadows = shadows;
    if (Status valid = validate(settings); !valid) {
        return make_unexpected(valid.error());
    }
    return settings;
}

assembly::AssemblyViewmodel assembly_view(const ViewmodelSettings& settings, const Mat4& world_view,
                                          f32 aspect) noexcept {
    assembly::AssemblyViewmodel view;
    if (!settings.enabled()) {
        return view;
    }
    view.layers = settings.layers;
    view.frustum =
        Frustum::from_view_projection(viewmodel_projection(settings, aspect, false) * world_view);
    view.fov_y_radians = settings.vertical_fov_radians;
    view.cast_shadows = settings.shadows.cast;
    view.receive_shadows = settings.shadows.receive;
    return view;
}

ViewmodelMatrices ViewmodelFrame::advance(const ViewmodelSettings& settings, f32 aspect,
                                          const Mat4& relative_view,
                                          const TemporalFramework& temporal) noexcept {
    const Mat4 projection = viewmodel_projection(settings, aspect, true);
    // LAST FRAME'S LENS, WHATEVER HAPPENED, as the world's previous rows are last frame's view
    // whatever happened: a resolve told its history is invalid reads none of it, and motion blur
    // reads the motion as drawn. Only the first frame, and a `reset` — a weapon swapped for
    // another, whose lens was never this one's — have no last lens, and then the lens has not
    // moved.
    if (!has_previous_) {
        previous_projection_ = projection;
    }
    ViewmodelMatrices matrices;
    matrices.relative_to_clip = projection * relative_view;
    matrices.previous_relative_to_clip =
        previous_projection_ * previous_camera_relative_view(temporal);
    previous_projection_ = projection;
    has_previous_ = true;
    return matrices;
}

void write_viewmodel(const ViewmodelMatrices& matrices, const ViewmodelSettings& settings,
                     Vec2 jitter, u32 width, u32 height, pipeline::FrameViewData& out) noexcept {
    // THE WORLD'S JITTER, BY THE WORLD'S ARITHMETIC: `upload_for` offsets each column by the
    // jitter in NDC times its w, so the weapon is sampled at the same sub-pixel offset as the wall
    // behind it and the temporal resolve sees one jittered frame rather than two.
    Mat4 jittered = matrices.relative_to_clip;
    if (width > 0 && height > 0) {
        const f32 x = jitter.x * 2.0F / static_cast<f32>(width);
        const f32 y = jitter.y * 2.0F / static_cast<f32>(height);
        for (Vec4& column : jittered.columns) {
            column.x += x * column.w;
            column.y += y * column.w;
        }
    }
    write_rows(jittered, out.viewmodel_to_clip);
    write_rows(matrices.previous_relative_to_clip, out.previous_viewmodel_to_clip);
    out.viewmodel_control[0] = settings.shadows.contact ? kViewmodelContactShadows : 0U;
    out.viewmodel_control[1] = 0;
    out.viewmodel_control[2] = 0;
    out.viewmodel_control[3] = 0;
}

}  // namespace cy::rendering::viewmodel
