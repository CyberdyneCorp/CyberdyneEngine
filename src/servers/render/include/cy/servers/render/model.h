#pragma once
// Scenes, views and instances: the model the render server owns. Tasks 4.1.1 and 4.1.3.
//
// `rendering-architecture` — "Scene, view, and instance model":
//
//   Scene     a renderable world: a spatial index of instances, a light set, GI state, and an
//             environment. Several MAY exist at once.
//   View      a camera into a scene: transform, projection, viewport rect, render target, layer
//             mask, quality settings, importance, a history identity, and post-process
//             configuration. A view MAY have sub-views for stereo or cubemap rendering.
//   Instance  a placement of a renderable into a scene, with a transform, bounds, layer mask, LOD
//             parameters, visibility, and per-instance data.
//
// --- VIEWS ARE FIRST CLASS AND PLURAL, AND THE INTERFACE HAS TO SHOW IT
// ---------------------------
//
// "The main camera is one view among many: shadow views, reflection probe captures, scene captures,
// editor viewports, minimaps, thumbnails, and each XR eye are all views, rendered through the same
// path." So there is no "main view" field anywhere in this file. A view is a handle like any other,
// it names which scene it looks at, and the frame renders every view it is given. A renderer that
// privileged one view would grow a second path for the others, which is the shape the requirement
// exists to prevent.
//
// Views are grouped into FAMILIES that share prepared work — the GPU scene, culling results, shadow
// maps, history. A family is a handle too, and a view names its family; two stereo eyes in one
// family cause the shared work to be prepared once.
//
// --- WHAT THE RENDERER CONSTRUCTS AND WHAT THE CAMERA SUPPLIES
// ------------------------------------
//
// "Views SHALL be produced from evaluated cameras, which supply pose, projection semantics,
// viewport, importance, and history identity. The renderer SHALL construct backend-specific
// projection matrices from the camera's semantic projection." That is why `Projection` below is
// *semantic* — a vertical field of view and a near distance, not a matrix. The matrix is built by
// `projection_matrix()`, which is the one place reversed-Z is applied, so a caller cannot supply a
// conventional-depth matrix and have it look almost right.
//
// Temporal jitter is deliberately absent: the specification puts it in the temporal framework
// rather than the camera, and there is nowhere here to put it.

#include <cy/core/base/types.h>
#include <cy/core/math/projection.h>
#include <cy/core/math/shapes.h>
#include <cy/core/math/transform.h>
#include <cy/core/values/name.h>
#include <cy/servers/render/gpu_scene.h>
#include <cy/servers/render/handles.h>
#include <cy/servers/render/types.h>

namespace cy::render {

// --- Cameras --------------------------------------------------------------------------------

enum class ProjectionKind : u8 {
    Perspective = 0,
    Orthographic,
};

/// A projection's MEANING, not its matrix. See the header comment.
///
/// `far_plane` of `0` means "infinite", which is the engine's default and the one reversed-Z makes
/// reasonable rather than a trick (`core-math`, "Infinite far plane"). It is spelled as zero rather
/// than as infinity so a zeroed struct is the sensible projection rather than a degenerate one.
struct Projection {
    ProjectionKind kind = ProjectionKind::Perspective;
    /// Perspective: the vertical field of view, in radians.
    f32 fov_y_radians = 1.0471975512F;  // 60 degrees
    /// Orthographic: the vertical extent in world units. The horizontal follows from the aspect.
    f32 ortho_height = 10.0F;
    f32 near_plane = 0.1F;
    f32 far_plane = 0.0F;

    /// The matrix, reversed-Z, [0, 1] depth, right-handed. THE ONLY PLACE A PROJECTION MATRIX IS
    /// BUILT in the renderer: design.md §3 requires the convention to be a number rather than a
    /// habit, and a second construction site is how the second one gets it wrong.
    [[nodiscard]] Mat4 matrix(f32 aspect) const noexcept;
};

// --- Views ----------------------------------------------------------------------------------

/// Where a view draws, in target pixels.
struct ViewportRect {
    u32 x = 0;
    u32 y = 0;
    u32 width = 0;
    u32 height = 0;

    [[nodiscard]] constexpr f32 aspect() const noexcept {
        return (height == 0) ? 1.0F : (static_cast<f32>(width) / static_cast<f32>(height));
    }
};

/// What a view is for. Not a switch anything branches on to render — every view goes through the
/// same path — but the attribution a statistics report and a budget allocation need, and the reason
/// a shadow view may skip work a main view may not.
enum class ViewPurpose : u8 {
    Primary = 0,
    Shadow,
    ReflectionProbe,
    SceneCapture,
    EditorViewport,
    Thumbnail,
    /// One eye of a stereo pair. Two of these in one family is the case the family exists for.
    XrEye,
    Count,
};

[[nodiscard]] const char* view_purpose_name(ViewPurpose purpose) noexcept;

/// The tonemapping and post-process chain, per view with a scene-level default.
/// `rendering-architecture`, "Environment and post-process configuration".
struct PostProcessSettings {
    bool auto_exposure = true;
    /// EV100. Used directly when `auto_exposure` is off, and as the starting point when it is on.
    f32 exposure_compensation = 0.0F;
    bool bloom = true;
    f32 bloom_intensity = 0.04F;
    bool depth_of_field = false;
    bool motion_blur = false;
    bool vignette = false;
    bool film_grain = false;
    f32 chromatic_aberration = 0.0F;
    OutputColorSpace output = OutputColorSpace::Srgb;
};

/// Background, ambient and fog. Per scene, overridable per view.
struct EnvironmentSettings {
    /// Constant background colour, used when no sky texture is set.
    f32 background_color[4] = {0.0F, 0.0F, 0.0F, 1.0F};
    TextureHandle sky;
    /// Multiplier on the environment's contribution to indirect lighting.
    f32 ambient_intensity = 1.0F;
    bool fog_enabled = false;
    f32 fog_density = 0.0F;
    f32 fog_height_falloff = 0.0F;
};

/// A view's own share of the frame. `rendering-architecture`: "Each view SHALL carry its own budget
/// allocation and importance, so a secondary view cannot consume the frame budget of the primary
/// one."
struct ViewBudget {
    /// Fraction of the frame's budget this view may spend. The arbiter (M7) distributes these; at
    /// M3 they are recorded and reported, which is what makes the report's "which view spent it"
    /// column real before there is anything to arbitrate.
    f32 allocation = 1.0F;
    f32 measured_ms = 0.0F;
};

using ViewFamilyId = u16;
inline constexpr ViewFamilyId kNoViewFamily = 0xFFFFU;

struct ViewDescription {
    Name name;
    SceneHandle scene;
    ViewPurpose purpose = ViewPurpose::Primary;

    /// The camera's pose. `transform.forward()` is the view direction, which is its local −Z.
    Transform camera;
    Projection projection;
    ViewportRect viewport;

    /// Which layers this view draws. A shadow view for a light that only lights the world draws a
    /// narrower mask than the primary view, and the mask is what expresses that.
    LayerMask layer_mask = kAllLayers;

    /// `rendering-architecture` requires a view to carry a **history identity**, so a temporal
    /// resource follows a view across frames even when the view list is rebuilt. It is supplied by
    /// the camera, never derived from the view's index — which would silently reassign history
    /// between two views the moment one of them is removed.
    u64 history_id = 0;

    /// Views sharing a family share prepared work. `kNoViewFamily` means "shares with nothing".
    ViewFamilyId family = kNoViewFamily;

    /// Higher renders first when the budget cannot cover everything.
    f32 importance = 1.0F;
    ViewBudget budget;

    PostProcessSettings post_process;
    /// Overrides the scene's environment when set.
    bool override_environment = false;
    EnvironmentSettings environment;

    DebugViewMode debug_mode = DebugViewMode::Off;

    /// The render target this view draws into. Null means the swapchain, which is the common case
    /// and the one a zeroed description expresses.
    TextureHandle target;
};

/// A view as the server holds it: the description plus what the renderer derived from it.
struct View {
    ViewDescription desc;

    /// Derived once per frame from `desc.camera` and `desc.projection`, so nothing recomputes them
    /// per pass. `Derived` in the determinism sense: recomputed, never hashed.
    Mat4 view_matrix = Mat4::identity();
    Mat4 projection_matrix = Mat4::identity();
    Mat4 view_projection = Mat4::identity();
    Frustum frustum{};

    /// design.md §3: "Positions reach the GPU relative to the camera." This is the origin the
    /// subtraction is made from — the camera's world position, captured when the matrices were
    /// built, so the view matrix and the instance offsets cannot disagree by a frame.
    Vec3 camera_relative_origin{0.0F, 0.0F, 0.0F};

    /// Recompute every derived member. Called once per frame per view, by the frame's prepare
    /// stage; a caller that mutates `desc` and forgets is the reason `frustum` is not lazy.
    void refresh() noexcept;
};

// --- Instances ------------------------------------------------------------------------------

/// What a caller supplies to place a renderable in a scene.
///
/// Note what is NOT here: a slot, an index, a pointer, or anything else derived from where the
/// instance will land. The server assigns those; sorting reads `stable_id`, which the caller
/// supplies and which is a property of the thing rather than of the placement.
struct InstanceDescription {
    MeshHandle mesh;
    MaterialHandle material;
    Transform transform;
    /// In the mesh's own space. The world bounds are derived; storing world bounds here would make
    /// them a second thing to keep in step with the transform.
    Aabb local_bounds = Aabb::from_center_extents(Vec3{0.0F, 0.0F, 0.0F}, Vec3{0.5F, 0.5F, 0.5F});
    LayerMask layer_mask = kDefaultLayer;
    /// `residency`'s unified render importance. Published once, consumed by every quality decision.
    f32 importance = 1.0F;
    /// Screen-coverage bias applied to LOD selection. Positive keeps more detail.
    f32 lod_bias = 0.0F;
    bool visible = true;
    bool casts_shadow = true;
    bool receives_shadow = true;
    bool two_sided = false;

    /// THE IDENTITY EVERYTHING DETERMINISTIC HANGS ON (design.md §6). An entity id, a
    /// (producer, index) pair, a content hash — anything stable across two runs of the same frame.
    /// A caller that leaves it zero gets a diagnostic from `RenderServer::create_instance`, because
    /// a frame of instances with no stable identity is a frame whose draw order is publication
    /// order, and publication order is what the requirement forbids.
    u64 stable_id = 0;
};

/// An instance as the server holds it.
struct Instance {
    InstanceDescription desc;
    SceneHandle scene;
    /// Where this instance's record lives in the GPU scene. Assigned by the server, never by the
    /// caller, and never used as a sort key.
    u32 gpu_slot = 0;
    /// The previous frame's world transform, kept so motion vectors and the previous bounds are a
    /// frame apart rather than zero.
    Transform previous_transform;
    bool has_previous = false;
};

/// The world-space bounds of a placed instance.
[[nodiscard]] Aabb world_bounds_of(const Transform& transform, const Aabb& local) noexcept;

// --- Lights ---------------------------------------------------------------------------------

/// `rendering-lighting-and-shadows` is at Seed for M3 and owns the shading; what the render server
/// owns is the light as a scene object with **physical units**, because the unit is part of the
/// interface and retrofitting one means rescaling every authored light.
enum class LightKind : u8 {
    Directional = 0,
    Point,
    Spot,
    Count,
};

[[nodiscard]] const char* light_kind_name(LightKind kind) noexcept;

struct LightDescription {
    LightKind kind = LightKind::Directional;
    Transform transform;
    /// Linear, un-premultiplied. The intensity carries the magnitude.
    f32 color[3] = {1.0F, 1.0F, 1.0F};
    /// PHYSICAL UNITS, and which one depends on the kind: lux (lm/m²) for a directional light,
    /// candela (lm/sr) for a point or spot. Named in the field rather than left to a convention,
    /// because "intensity 5" means two different things for two kinds and a comment on a header is
    /// where that stops being ambiguous.
    f32 intensity = 100000.0F;  // roughly midday sun, in lux
    /// Metres. Zero for a directional light.
    f32 range = 0.0F;
    /// Spot cone half-angles, in radians. Ignored for the other kinds.
    f32 inner_cone_radians = 0.0F;
    f32 outer_cone_radians = 0.7853981634F;
    LayerMask layer_mask = kAllLayers;
    bool casts_shadow = true;
    /// The identity a shadow atlas page and a deterministic light order key off. Same contract as
    /// `InstanceDescription::stable_id`.
    u64 stable_id = 0;
};

// --- Scenes ---------------------------------------------------------------------------------

struct SceneDescription {
    Name name;
    /// The GPU scene's slot capacity for this scene. Fixed at creation: see gpu_scene.h for why the
    /// store does not grow.
    u32 instance_capacity = 4096;
    EnvironmentSettings environment;
    PostProcessSettings post_process;
};

}  // namespace cy::render
