// Engine-side transform-gizmo geometry. See cy/servers/render/gizmo.h.

#include <cy/servers/render/gizmo.h>

#include <cy/core/math/matrix.h>
#include <cy/core/math/scalar.h>
#include <cy/servers/render/picking.h>

#include <cmath>
#include <cstring>

namespace cy::render {
namespace {

// --- The editor's codec ---------------------------------------------------------------------
//
// `cy_editor_core::codec`, restated: little-endian throughout, `f32` by its bits, a `u32` count
// before a list. Restated rather than shared because the two ends are two languages, and kept
// true by `cy_test_integration_editor_gizmo_wire`, which decodes these bytes with the editor's
// own decoder rather than with a second copy of this one.

class WireReader {
public:
    explicit WireReader(Span<const u8> bytes) noexcept : bytes_(bytes) {}

    [[nodiscard]] bool u8_value(u8& out) noexcept {
        if (offset_ + 1 > bytes_.size()) {
            return false;
        }
        out = bytes_[offset_++];
        return true;
    }

    [[nodiscard]] bool u32_value(u32& out) noexcept { return little_endian(out); }
    [[nodiscard]] bool u64_value(u64& out) noexcept { return little_endian(out); }

    [[nodiscard]] bool f32_value(f32& out) noexcept {
        u32 bits = 0;
        if (!little_endian(bits)) {
            return false;
        }
        std::memcpy(&out, &bits, sizeof(out));
        return true;
    }

    [[nodiscard]] bool exhausted() const noexcept { return offset_ == bytes_.size(); }
    [[nodiscard]] usize remaining() const noexcept { return bytes_.size() - offset_; }

private:
    template <class T>
    [[nodiscard]] bool little_endian(T& out) noexcept {
        if (offset_ + sizeof(T) > bytes_.size()) {
            return false;
        }
        T value = 0;
        for (usize index = 0; index < sizeof(T); ++index) {
            value |= static_cast<T>(static_cast<T>(bytes_[offset_ + index])
                                    << static_cast<T>(index * 8));
        }
        offset_ += sizeof(T);
        out = value;
        return true;
    }

    Span<const u8> bytes_;
    usize offset_ = 0;
};

class WireWriter {
public:
    explicit WireWriter(Array<u8>& out) noexcept : out_(&out) {}

    [[nodiscard]] Status u8_value(u8 value) noexcept { return out_->push_back(value); }
    [[nodiscard]] Status u32_value(u32 value) noexcept { return little_endian(value); }
    [[nodiscard]] Status u64_value(u64 value) noexcept { return little_endian(value); }

    [[nodiscard]] Status f32_value(f32 value) noexcept {
        u32 bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        return little_endian(bits);
    }

private:
    template <class T>
    [[nodiscard]] Status little_endian(T value) noexcept {
        for (usize index = 0; index < sizeof(T); ++index) {
            const auto byte = static_cast<u8>((value >> (index * 8)) & 0xFFU);
            if (Status pushed = out_->push_back(byte); !pushed) {
                return pushed;
            }
        }
        return ok();
    }

    Array<u8>* out_;
};

/// The layout encoding's version, refused rather than guessed at by the editor's reader.
constexpr u8 kLayoutVersion = 1;

[[nodiscard]] bool mode_of_code(u8 code, GizmoMode& out) noexcept {
    if (code >= static_cast<u8>(GizmoMode::Count)) {
        return false;
    }
    out = static_cast<GizmoMode>(code);
    return true;
}

[[nodiscard]] bool pivot_of_code(u8 code, GizmoPivot& out) noexcept {
    if (code >= static_cast<u8>(GizmoPivot::Count)) {
        return false;
    }
    out = static_cast<GizmoPivot>(code);
    return true;
}

// --- Placement ------------------------------------------------------------------------------

struct Placement {
    const View* view = nullptr;
    Vec3 pivot{0.0F, 0.0F, 0.0F};
    Quat axes = Quat::identity();
    /// The world distance one pixel spans at the pivot. Every offset below is a multiple of it,
    /// which is the whole of screen-constant sizing.
    f32 per_pixel = 0.0F;
    f32 centre_depth = 0.0F;
    GizmoStyle style{};
};

/// The camera-space depth of a world point: how far in front of the eye it is, in world units.
///
/// Positive in front. The camera looks down its local -Z, so the view-space z is negated.
[[nodiscard]] f32 depth_of(const View& view, Vec3 world) noexcept {
    const Vec4 view_space = view.view_matrix * Vec4{world.x, world.y, world.z, 1.0F};
    return -view_space.z;
}

/// Add one handle at a world point, or add nothing when the point does not project.
///
/// A handle behind the camera is DROPPED rather than clamped to an edge. A clamped handle is worse
/// than a missing one: it is drawn somewhere the geometry is not, and a click on it manipulates an
/// axis pointing away from the user.
[[nodiscard]] Status add_world(Array<GizmoHandleSpot>& spots, const Placement& placement,
                               GizmoHandle handle, Vec3 world, f32 radius) noexcept {
    Vec2 pixel{0.0F, 0.0F};
    if (!project_to_pixel(*placement.view, world, pixel)) {
        return ok();
    }
    return spots.push_back(
        GizmoHandleSpot{handle, pixel.x, pixel.y, radius, depth_of(*placement.view, world)});
}

/// Add one of the three concentric centre affordances, at the pivot's own pixel.
///
/// The depth is biased so that a click at the exact centre resolves cube, then circle, then outer
/// ring. `GizmoLayout::hit` on the editor's side orders by distance and then by depth, and at the
/// centre every distance is zero — so without this the list order would decide, which is the defect
/// `the_centres_three_affordances_stay_separately_targetable` exists to catch.
[[nodiscard]] Status add_centre(Array<GizmoHandleSpot>& spots, const Placement& placement,
                                GizmoHandle handle, f32 radius, f32 depth_steps) noexcept {
    Vec2 pixel{0.0F, 0.0F};
    if (!project_to_pixel(*placement.view, placement.pivot, pixel)) {
        return ok();
    }
    const f32 depth = placement.centre_depth - (depth_steps * placement.style.centre_depth_bias);
    return spots.push_back(GizmoHandleSpot{handle, pixel.x, pixel.y, radius, depth});
}

[[nodiscard]] Vec3 axis_of(const Placement& placement, u32 index) noexcept {
    constexpr Vec3 kUnits[3] = {Vec3{1.0F, 0.0F, 0.0F}, Vec3{0.0F, 1.0F, 0.0F},
                                Vec3{0.0F, 0.0F, 1.0F}};
    return placement.axes * kUnits[index % 3];
}

[[nodiscard]] Status add_axes(Array<GizmoHandleSpot>& spots, const Placement& placement) noexcept {
    const f32 reach = placement.style.extent_pixels * placement.per_pixel;
    for (u32 index = 0; index < 3; ++index) {
        const auto handle =
            static_cast<GizmoHandle>(static_cast<u8>(GizmoHandle::AxisX) + static_cast<u8>(index));
        if (Status added = add_world(spots, placement, handle,
                                     placement.pivot + (axis_of(placement, index) * reach),
                                     placement.style.axis_radius);
            !added) {
            return added;
        }
    }
    return ok();
}

/// The plane handles. A plane's handle sits on the diagonal between the two axes that span it.
///
/// `PlaneXY` is spanned by axes 0 and 1, `PlaneYZ` by 1 and 2, `PlaneZX` by 2 and 0 — the editor's
/// own naming, which is why the pairs are written out rather than derived from the index.
[[nodiscard]] Status add_planes(Array<GizmoHandleSpot>& spots,
                                const Placement& placement) noexcept {
    struct Pair {
        GizmoHandle handle;
        u32 first;
        u32 second;
    };
    constexpr Pair pairs[] = {
        {GizmoHandle::PlaneXY, 0, 1},
        {GizmoHandle::PlaneYZ, 1, 2},
        {GizmoHandle::PlaneZX, 2, 0},
    };
    const f32 reach =
        placement.style.extent_pixels * placement.style.plane_fraction * placement.per_pixel;
    for (const Pair& pair : pairs) {
        const Vec3 corner = placement.pivot + (axis_of(placement, pair.first) * reach) +
                            (axis_of(placement, pair.second) * reach);
        if (Status added =
                add_world(spots, placement, pair.handle, corner, placement.style.plane_radius);
            !added) {
            return added;
        }
    }
    return ok();
}

/// How many points a ring is sampled at when the nearest one is looked for.
///
/// Sixty-four: the answer moves by at most 2.8 degrees along the ring, which is well inside the
/// editor's twelve-pixel acquisition slop at every gizmo size this file draws.
constexpr u32 kRingSamples = 64;

/// The ring handles. See the header: a ring is a curve and a spot is a disc, so the published point
/// is the ring's point NEAREST THE CAMERA — the part drawn unoccluded, and the part a person aims
/// at. Deterministic: the samples are fixed and ties go to the lower index.
[[nodiscard]] Status add_rings(Array<GizmoHandleSpot>& spots, const Placement& placement) noexcept {
    const f32 reach = placement.style.extent_pixels * placement.per_pixel;
    for (u32 index = 0; index < 3; ++index) {
        const Vec3 first = axis_of(placement, (index + 1) % 3);
        const Vec3 second = axis_of(placement, (index + 2) % 3);
        Vec3 nearest{0.0F, 0.0F, 0.0F};
        f32 nearest_depth = 0.0F;
        bool found = false;
        for (u32 sample = 0; sample < kRingSamples; ++sample) {
            const f32 angle =
                (static_cast<f32>(sample) / static_cast<f32>(kRingSamples)) * 2.0F * math::kPi;
            const Vec3 point = placement.pivot + (first * (std::cos(angle) * reach)) +
                               (second * (std::sin(angle) * reach));
            const f32 depth = depth_of(*placement.view, point);
            if (depth <= 0.0F) {
                continue;
            }
            if (!found || depth < nearest_depth) {
                nearest = point;
                nearest_depth = depth;
                found = true;
            }
        }
        if (!found) {
            continue;
        }
        const auto handle =
            static_cast<GizmoHandle>(static_cast<u8>(GizmoHandle::RingX) + static_cast<u8>(index));
        if (Status added =
                add_world(spots, placement, handle, nearest, placement.style.ring_radius);
            !added) {
            return added;
        }
    }
    return ok();
}

[[nodiscard]] Status add_boxes(Array<GizmoHandleSpot>& spots, const Placement& placement) noexcept {
    const f32 reach =
        placement.style.extent_pixels * placement.style.box_fraction * placement.per_pixel;
    for (u32 index = 0; index < 3; ++index) {
        const auto handle =
            static_cast<GizmoHandle>(static_cast<u8>(GizmoHandle::BoxX) + static_cast<u8>(index));
        if (Status added = add_world(spots, placement, handle,
                                     placement.pivot + (axis_of(placement, index) * reach),
                                     placement.style.box_radius);
            !added) {
            return added;
        }
    }
    return ok();
}

/// The forms each mode presents, which is `cy_editor_visual::gizmo::GizmoMode::forms` read from the
/// engine's side: arrows and plane quads for Move, rings for Rotate, boxes and the centre box for
/// Scale, all five for Universal. The two screen-space affordances go with the form whose plane
/// they are — the centre circle with free movement, the outer ring with screen rotation.
struct ModeForms {
    bool arrows = false;
    bool planes = false;
    bool rings = false;
    bool boxes = false;
    bool centre_circle = false;
    bool centre_cube = false;
    bool screen_ring = false;
};

[[nodiscard]] constexpr ModeForms forms_of(GizmoMode mode) noexcept {
    switch (mode) {
        case GizmoMode::Translate:
            return ModeForms{true, true, false, false, true, false, false};
        case GizmoMode::Rotate:
            return ModeForms{false, false, true, false, false, false, true};
        case GizmoMode::Scale:
            return ModeForms{false, false, false, true, false, true, false};
        case GizmoMode::Universal:
            return ModeForms{true, true, true, true, true, true, true};
        case GizmoMode::Count:
            break;
    }
    return ModeForms{};
}

}  // namespace

const char* gizmo_mode_name(GizmoMode mode) noexcept {
    switch (mode) {
        case GizmoMode::Translate:
            return "translate";
        case GizmoMode::Rotate:
            return "rotate";
        case GizmoMode::Scale:
            return "scale";
        case GizmoMode::Universal:
            return "universal";
        case GizmoMode::Count:
            break;
    }
    return "unknown";
}

const char* gizmo_handle_name(GizmoHandle handle) noexcept {
    switch (handle) {
        case GizmoHandle::AxisX:
            return "axis-x";
        case GizmoHandle::AxisY:
            return "axis-y";
        case GizmoHandle::AxisZ:
            return "axis-z";
        case GizmoHandle::PlaneXY:
            return "plane-xy";
        case GizmoHandle::PlaneYZ:
            return "plane-yz";
        case GizmoHandle::PlaneZX:
            return "plane-zx";
        case GizmoHandle::RingX:
            return "ring-x";
        case GizmoHandle::RingY:
            return "ring-y";
        case GizmoHandle::RingZ:
            return "ring-z";
        case GizmoHandle::ScreenRing:
            return "screen-ring";
        case GizmoHandle::BoxX:
            return "box-x";
        case GizmoHandle::BoxY:
            return "box-y";
        case GizmoHandle::BoxZ:
            return "box-z";
        case GizmoHandle::Screen:
            return "screen";
        case GizmoHandle::Uniform:
            return "uniform";
        case GizmoHandle::Count:
            break;
    }
    return "unknown";
}

const GizmoHandleSpot* GizmoLayout::spot(GizmoHandle handle) const noexcept {
    for (const GizmoHandleSpot& candidate : spots) {
        if (candidate.handle == handle) {
            return &candidate;
        }
    }
    return nullptr;
}

Status decode_gizmo_intent(Span<const u8> bytes, GizmoIntent& out) noexcept {
    WireReader reader(bytes);
    GizmoIntent intent;
    u8 code = 0;
    if (!reader.u64_value(intent.frame_id) || !reader.u8_value(code)) {
        return fail(ErrorCode::InvalidArgument, "a gizmo intent ends before its mode");
    }
    if (!mode_of_code(code, intent.mode)) {
        return fail(ErrorCode::InvalidArgument, "a gizmo intent names a mode this build has not");
    }
    if (!reader.u8_value(code) || code >= static_cast<u8>(GizmoSpace::Count)) {
        return fail(ErrorCode::InvalidArgument, "a gizmo intent names a space this build has not");
    }
    intent.space = static_cast<GizmoSpace>(code);
    if (intent.space == GizmoSpace::Custom) {
        f32 lanes[4] = {0.0F, 0.0F, 0.0F, 0.0F};
        for (f32& lane : lanes) {
            if (!reader.f32_value(lane)) {
                return fail(ErrorCode::InvalidArgument,
                            "a gizmo intent's custom space ends before its four lanes");
            }
        }
        intent.custom_space = Quat{lanes[0], lanes[1], lanes[2], lanes[3]};
    }
    if (!reader.u8_value(code) || !pivot_of_code(code, intent.pivot)) {
        return fail(ErrorCode::InvalidArgument, "a gizmo intent names a pivot this build has not");
    }
    u32 count = 0;
    if (!reader.u32_value(count)) {
        return fail(ErrorCode::InvalidArgument, "a gizmo intent ends before its selection count");
    }
    // Refused against what is actually left rather than against a constant: a count that claims
    // more identities than the message holds is a truncated message or a different protocol, and
    // reserving for it would be an allocation sized by a peer.
    if (static_cast<usize>(count) * sizeof(u64) > reader.remaining()) {
        return fail(ErrorCode::InvalidArgument,
                    "a gizmo intent claims more identities than its bytes hold");
    }
    if (Status reserved = intent.identities.reserve(count); !reserved) {
        return reserved;
    }
    for (u32 index = 0; index < count; ++index) {
        u64 identity = 0;
        if (!reader.u64_value(identity)) {
            return fail(ErrorCode::InvalidArgument, "a gizmo intent ends inside its selection");
        }
        if (Status pushed = intent.identities.push_back(identity); !pushed) {
            return pushed;
        }
    }
    // Optional, and absent from an editor that predates the field. Read together or not at all: a
    // width with no height is a truncated message rather than a partial one.
    u32 width = 0;
    u32 height = 0;
    if (reader.u32_value(width) && reader.u32_value(height)) {
        intent.viewport_width = width;
        intent.viewport_height = height;
    }
    // The camera, also optional and also read as a GROUP: a position with no rotation is a
    // truncated message rather than a partial one, and half a camera would be worse than none.
    f32 lanes[9] = {};
    bool complete = true;
    for (f32& lane : lanes) {
        complete = complete && reader.f32_value(lane);
    }
    if (complete) {
        for (u32 index = 0; index < 3; ++index) {
            intent.camera_position[index] = lanes[index];
        }
        for (u32 index = 0; index < 4; ++index) {
            intent.camera_rotation[index] = lanes[3 + index];
        }
        intent.fov_y_radians = lanes[7];
        intent.near_plane = lanes[8];
    }
    out = std::move(intent);
    return ok();
}

Status encode_gizmo_layout(const GizmoLayout& layout, Array<u8>& out) noexcept {
    out.clear();
    WireWriter writer(out);
    if (Status written = writer.u8_value(kLayoutVersion); !written) {
        return written;
    }
    if (Status written = writer.u64_value(layout.frame_id); !written) {
        return written;
    }
    if (Status written = writer.u8_value(static_cast<u8>(layout.mode)); !written) {
        return written;
    }
    for (const f32 value : {layout.centre_x, layout.centre_y, layout.extent}) {
        if (Status written = writer.f32_value(value); !written) {
            return written;
        }
    }
    if (Status written = writer.u32_value(static_cast<u32>(layout.spots.size())); !written) {
        return written;
    }
    for (const GizmoHandleSpot& spot : layout.spots) {
        if (Status written = writer.u8_value(static_cast<u8>(spot.handle)); !written) {
            return written;
        }
        for (const f32 value : {spot.x, spot.y, spot.radius, spot.depth}) {
            if (Status written = writer.f32_value(value); !written) {
                return written;
            }
        }
    }
    return ok();
}

f32 world_per_pixel(const View& view, Vec3 world_point) noexcept {
    const u32 height = view.desc.viewport.height;
    if (height == 0) {
        return 0.0F;
    }
    const Projection& projection = view.desc.projection;
    if (projection.kind == ProjectionKind::Orthographic) {
        return projection.ortho_height / static_cast<f32>(height);
    }
    const f32 depth = depth_of(view, world_point);
    if (!(depth > 0.0F)) {
        return 0.0F;
    }
    // The frustum's height at that depth, over the viewport's height in pixels. This is the whole
    // of screen-constant sizing: a length of `n * world_per_pixel` projects to `n` pixels whatever
    // the camera distance, because the distance is in the numerator.
    const f32 frustum_height = 2.0F * std::tan(projection.fov_y_radians * 0.5F) * depth;
    return frustum_height / static_cast<f32>(height);
}

void rescale_gizmo_layout(GizmoLayout& layout, u32 from_width, u32 from_height, u32 to_width,
                          u32 to_height) noexcept {
    if (from_width == 0 || from_height == 0 || to_width == 0 || to_height == 0 || layout.empty()) {
        return;
    }
    const f32 across = static_cast<f32>(to_width) / static_cast<f32>(from_width);
    const f32 down = static_cast<f32>(to_height) / static_cast<f32>(from_height);
    const f32 uniform = (across < down) ? across : down;
    layout.centre_x *= across;
    layout.centre_y *= down;
    layout.extent *= uniform;
    for (GizmoHandleSpot& spot : layout.spots) {
        spot.x *= across;
        spot.y *= down;
        spot.radius *= uniform;
    }
}

Quat gizmo_axes(const GizmoIntent& intent, const View& view, Quat local) noexcept {
    switch (intent.space) {
        case GizmoSpace::World:
            return Quat::identity();
        case GizmoSpace::Local:
        case GizmoSpace::Parent:
            return local;
        case GizmoSpace::View:
            return view.desc.camera.rotation;
        case GizmoSpace::Custom:
            return intent.custom_space;
        case GizmoSpace::Count:
            break;
    }
    return Quat::identity();
}

GizmoLayout build_gizmo_layout(const View& view, Vec3 pivot, Quat axes, GizmoMode mode,
                               u64 frame_id, const GizmoStyle& style) noexcept {
    GizmoLayout layout;
    layout.frame_id = frame_id;
    layout.mode = mode;

    Vec2 centre{0.0F, 0.0F};
    const f32 per_pixel = world_per_pixel(view, pivot);
    // A pivot behind the camera, a degenerate view, or a projection that cannot be inverted all
    // reach here, and all produce an empty layout naming the frame. That is a legitimate answer the
    // editor already handles — it takes the gizmo off the screen — and it is the honest one: there
    // is nowhere on this frame to draw the handles.
    if (!(per_pixel > 0.0F) || !project_to_pixel(view, pivot, centre)) {
        return layout;
    }
    layout.centre_x = centre.x;
    layout.centre_y = centre.y;
    layout.extent = style.extent_pixels;

    const Placement placement{&view, pivot, axes, per_pixel, depth_of(view, pivot), style};
    const ModeForms forms = forms_of(mode);

    // Each of these returns a Status only because the array can fail to grow. A partial layout is
    // returned rather than none: half a gizmo the user can still grab an axis of is better than no
    // gizmo, and an allocation failure here is not a reason to make the viewport unusable.
    Status added = ok();
    if (forms.arrows && added) {
        added = add_axes(layout.spots, placement);
    }
    if (forms.planes && added) {
        added = add_planes(layout.spots, placement);
    }
    if (forms.rings && added) {
        added = add_rings(layout.spots, placement);
    }
    if (forms.boxes && added) {
        added = add_boxes(layout.spots, placement);
    }
    // The centre, outermost first, so that the depth bias below reads in the same order as the
    // picture: the outer ring is furthest, then the circle, then the cube nearest the camera.
    if (forms.screen_ring && added) {
        added = add_centre(layout.spots, placement, GizmoHandle::ScreenRing,
                           style.extent_pixels * style.screen_ring_fraction, 0.0F);
    }
    if (forms.centre_circle && added) {
        added = add_centre(layout.spots, placement, GizmoHandle::Screen, style.screen_radius, 1.0F);
    }
    if (forms.centre_cube && added) {
        added =
            add_centre(layout.spots, placement, GizmoHandle::Uniform, style.uniform_radius, 2.0F);
    }
    return layout;
}

}  // namespace cy::render
