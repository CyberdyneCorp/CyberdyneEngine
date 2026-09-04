#pragma once
// The components that make an entity visible to the renderer, and their ids in a world.
// Tasks 4.1.2 and 4.1.4.
//
// `rendering-architecture` — "Simulation-to-render snapshot": the snapshot is "extracted by an
// `Extract` system running at the end of the frame stage" and contains "visible instance data
// (transforms, bounds, material and mesh handles, per-instance parameters), light state, camera
// state, and environment state". These three components are what that system reads.
//
// ================================================================================================
// WHY THESE ARE HERE AND NOT IN src/scene/
// ================================================================================================
//
// A `MeshRenderer` names a `render::MeshHandle`, which is the render server's (layer 2). src/scene/
// is layer 4 like this module and could name one — but a node is a general-purpose thing and the
// renderer is one of several subsystems that will attach data to entities. Physics bodies will do
// the same at M4 and audio emitters at M7, and each belongs with its own subsystem rather than in
// the node layer, or `src/scene/` becomes the union of every subsystem's per-entity data.
//
// ================================================================================================
// REGISTERED BY NAME, NOT REFLECTED — AND THE SAME SEAM src/scene/ RECORDS
// ================================================================================================
//
// The reflection generator's annotated-header list lives in src/core/reflect/CMakeLists.txt and the
// identifiers in identity/manifest.toml. `core-type-system` is explicit that a manifest identifier
// is assigned once and never guessed, so a component whose identifiers this module invented would
// be a component with a fabricated identity. They are registered with `register_builtin` instead,
// exactly as the ECS's `Parent`/`Children` and the scene's twelve are.
//
// THE CONSEQUENCE IS PAID FOR IMMEDIATELY AND NOT DEFERRED. A component registered by name is
// invisible to the state hash unless something declares a schema for it, which is M2's carried-
// forward debt 1.2 and the reason `state_schema.h` in this directory exists and is written in the
// same change as this header. `declare_render_state()` is one call, made where a host already makes
// `ecs::declare_relationship_state` and `scene::declare_scene_state`.
//
// ================================================================================================
// CLASSIFICATION, DECIDED AT THE MOMENT THE STRUCT IS WRITTEN
// ================================================================================================
//
// M2's debt 1.3: `Classified<>` is cheapest at authoring time, because retrofitting it means
// putting a witness into every existing reader. So `importance` — the one field below that the
// renderer computes rather than an author setting — is `Presentation` from its first line, and an
// authoritative system cannot name its value at all.
//
// The handles and the bounds are `Derived` in the state schema for a different reason, stated
// there: a handle's value is the render server's slot allocation order, which is not stable across
// runs.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/determinism/classification.h>
#include <cy/core/math/shapes.h>
#include <cy/ecs/world.h>
#include <cy/servers/render/model.h>

#include <type_traits>

namespace cy::rendering {

using ecs::ComponentTypeId;
using ecs::Entity;
using ecs::kInvalidComponent;
using ecs::World;

/// An entity with a mesh to draw.
///
/// Everything the extract stage needs to publish an instance, and nothing it would have to ask a
/// server for. `local_bounds` is a copy of the mesh's own bounds rather than a lookup through
/// `mesh`, deliberately: extraction runs at the commit boundary, on the simulation thread, and a
/// render server is neither thread-safe nor reachable from a system's access declaration. Whoever
/// assigns the mesh copies the bounds with it — one write when the mesh changes, instead of a
/// handle resolution per entity per tick.
struct MeshRenderer {
    render::MeshHandle mesh;
    render::MaterialHandle material;
    /// In the mesh's own space. The world bounds are derived from this and the transform, by
    /// whoever needs them; storing world bounds here would make them a second thing to keep in
    /// step.
    Aabb local_bounds = Aabb::from_center_extents(Vec3{0.0F, 0.0F, 0.0F}, Vec3{0.5F, 0.5F, 0.5F});
    render::LayerMask layer_mask = render::kDefaultLayer;
    /// Screen-coverage bias applied to LOD selection. Positive keeps more detail.
    f32 lod_bias = 0.0F;

    /// `residency`'s unified render importance: "published once per instance and consumed by every
    /// quality decision — geometry detail, texture page priority, shadow page resolution and
    /// refresh, animation rate, and illumination quality".
    ///
    /// PRESENTATION, AND THE FIREWALL IS THE POINT. The renderer writes it each frame from screen
    /// coverage, distance and view importance — all of which depend on where the camera is. A
    /// gameplay system that read it back would have made simulation depend on the view, which is a
    /// divergence between two clients watching the same match from different angles, and the kind
    /// M9's replay cannot reproduce. `read()` requires a witness and the overload does not exist
    /// for an `AuthoritativeContext`, so that is a compile error rather than a defect to be found.
    ///
    /// Writing it from an authoritative system is legal and stays legal: authority flowing downhill
    /// is how a designer pins an object as important.
    determinism::Presentation<f32> importance{1.0F};

    bool visible = true;
    bool casts_shadow = true;
    bool receives_shadow = true;
    bool two_sided = false;
};

static_assert(std::is_trivially_copyable_v<MeshRenderer>,
              "the ECS refuses a component that is not trivially relocatable");

/// A light attached to an entity. Its placement is the entity's `WorldTransform`.
///
/// The fields are `render::LightDescription`'s, minus the transform and the stable id, which
/// extraction fills in from the entity. Restating them rather than embedding the description keeps
/// the component free of a field a designer must not set — a `stable_id` an author could type would
/// be an identity two entities could share, and the sort key rests on that being impossible.
struct LightSource {
    render::LightKind kind = render::LightKind::Point;
    /// Linear, un-premultiplied. `intensity` carries the magnitude.
    f32 color[3] = {1.0F, 1.0F, 1.0F};
    /// PHYSICAL UNITS, and which one depends on the kind: lux for a directional light, candela for
    /// a point or a spot. `rendering-lighting-and-shadows` requires the unit to be stated where the
    /// value is, because "intensity 5" means two different things for two kinds.
    f32 intensity = 1000.0F;
    /// Metres. Zero for a directional light.
    f32 range = 10.0F;
    f32 inner_cone_radians = 0.0F;
    f32 outer_cone_radians = 0.7853981634F;
    render::LayerMask layer_mask = render::kAllLayers;
    bool casts_shadow = true;
    bool enabled = true;
};

static_assert(std::is_trivially_copyable_v<LightSource>);

/// A camera attached to an entity. Its pose is the entity's `WorldTransform`.
///
/// SEMANTIC, NOT A MATRIX — `rendering-architecture`: "Views SHALL be produced from evaluated
/// cameras, which supply pose, projection semantics, viewport, importance, and history identity.
/// The renderer SHALL construct backend-specific projection matrices from the camera's semantic
/// projection." A component holding a `Mat4` would be a camera that had already chosen a depth
/// convention, and design.md §3 is that reversed-Z is decided in exactly one place
/// (`render::Projection::matrix`).
struct Camera {
    render::Projection projection;
    render::ViewportRect viewport;
    render::ViewPurpose purpose = render::ViewPurpose::Primary;
    render::LayerMask layer_mask = render::kAllLayers;
    /// What share of the frame budget this camera's view asks for. A secondary view draws from its
    /// own allocation and degrades before the primary one does.
    f32 importance = 1.0F;
    /// The identity temporal history keys off across frames — reprojection, TAA and any resource a
    /// view keeps between frames. Stable while the camera exists, and distinct from the entity id
    /// so that two views produced from one camera can carry different histories.
    u64 history_id = 0;
    bool enabled = true;
};

static_assert(std::is_trivially_copyable_v<Camera>);

/// The name each component is registered under. Public because a serializer binds a stream to a
/// world by these names, and because a test asserting on a registration should not spell a string
/// literal a second time.
inline constexpr const char* kMeshRendererComponentName = "cy::rendering::MeshRenderer";
inline constexpr const char* kLightSourceComponentName = "cy::rendering::LightSource";
inline constexpr const char* kCameraComponentName = "cy::rendering::Camera";

/// The renderer's component ids in one world.
///
/// Ids are per world (`ecs-core`), so this is a value the extractor holds and never a static. Two
/// worlds in one process assign different numbers and neither is wrong.
struct RenderComponents {
    ComponentTypeId mesh_renderer = kInvalidComponent;
    ComponentTypeId light_source = kInvalidComponent;
    ComponentTypeId camera = kInvalidComponent;

    /// Register all of them in `world`, in this order, and return the ids.
    ///
    /// Idempotent: `ComponentRegistry::register_builtin` returns the existing id for a name it has
    /// already seen, so a second extractor over one world binds to the same numbers rather than
    /// registering a second set.
    [[nodiscard]] static Expected<RenderComponents, Error> register_all(World& world) noexcept;

    [[nodiscard]] bool registered() const noexcept {
        return mesh_renderer != kInvalidComponent && light_source != kInvalidComponent &&
               camera != kInvalidComponent;
    }
};

}  // namespace cy::rendering
