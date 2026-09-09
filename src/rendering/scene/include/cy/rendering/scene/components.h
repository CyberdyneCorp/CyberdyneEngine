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
// REFLECTED AT M8.b, AND WHAT THAT CHANGED — TASK 11.3
// ================================================================================================
//
// These three were registered BY NAME until M8.b, with the note that "the reflection generator's
// annotated-header list lives in src/core/reflect/CMakeLists.txt and the identifiers in
// identity/manifest.toml", so a component whose identifiers this module invented would be a
// component with a fabricated identity.
//
// The cost of that came due at M8.a, and its own artefact report is the evidence: an authored
// sphere was drawn as a unit box, because `MeshRenderer` was a name in `src/scene/`'s node
// catalogue with no `reflect::TypeId` behind it. Nothing downstream could act on it — the template
// was declared and not instantiable, `AuthoringSchema` did not carry it, so `resolve_against` could
// not match a `.cyworld`'s `MeshRenderer` to anything, and the mesh a designer picked reached no
// renderer. So they are reflected here, with identifiers the manifest issued, and the header is
// named in src/core/reflect/CMakeLists.txt's module list.
//
// THE STATE SCHEMA STAYS. Reflection describes a type; `state_schema.h` says which of its fields
// the determinism hash folds and which it must not, and that is a different statement — a reflected
// field is not automatically authoritative, and `importance` must never be hashed. Both are here.
//
// ================================================================================================
// WHAT A `MeshRenderer` NAMES: AN ASSET, AND SEPARATELY A HANDLE
// ================================================================================================
//
// `core-type-system` — "Asset ids are distinct from handles": an asset id is 128-bit persistent
// identity, "handles are runtime-only and are never serialized". Until M8.b this component held
// only the handles, which is why it had nothing an authoring document could carry: a designer picks
// an ASSET, and the slot the render server happened to give it is not a thing a file can hold.
//
// So `mesh` and `material` are asset references and are reflected; `mesh_handle` and
// `material_handle` are what the asset resolved to in this process, are not reflected, and are
// filled in by whoever loaded the asset. The extract stage reads the handles, because that is what
// a snapshot carries; an authoring document reads the references, because that is what survives.
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
#include <cy/core/reflect/annotations.h>
#include <cy/core/values/asset_id.h>
#include <cy/ecs/world.h>
#include <cy/servers/render/model.h>

#include <type_traits>

namespace cy::rendering {

using ecs::ComponentTypeId;
using ecs::Entity;
using ecs::kInvalidComponent;
using ecs::World;

/// A 128-bit asset id in the shape reflection can describe.
///
/// `cy::AssetId` is the engine's asset identity and this is NOT a second one: `to_asset_id()` and
/// `from_asset_id()` are the whole of the relationship, and neither loses a bit. The reason for the
/// two-lane spelling is a rule of the generator rather than a design preference — tools/gen/reflect
/// refuses a field whose type hides its representation, "because `offsetof` would compile from
/// inside the class and not from the generated file, and a type that hides its representation is a
/// type whose representation is not the contract". `AssetId` holds its halves privately, so a field
/// of that type is a field reflection cannot describe, and a component holding one would be a
/// component with no authored mesh in its schema — which is the defect task 11.3 exists to close.
///
/// The two lanes are `high` then `low`, big-endian reading order, exactly as `AssetId` documents
/// them, so the pair reads the same way the canonical text form does.
struct AssetRef {
    u64 high = 0;
    u64 low = 0;

    [[nodiscard]] constexpr bool is_nil() const noexcept { return high == 0 && low == 0; }
    [[nodiscard]] constexpr AssetId to_asset_id() const noexcept { return AssetId{high, low}; }
    [[nodiscard]] static constexpr AssetRef from_asset_id(AssetId id) noexcept {
        return AssetRef{id.high(), id.low()};
    }

    friend constexpr bool operator==(AssetRef, AssetRef) noexcept = default;
};

static_assert(std::is_trivially_copyable_v<AssetRef>);

/// An entity with a mesh to draw.
///
/// Everything the extract stage needs to publish an instance, and nothing it would have to ask a
/// server for. `local_bounds` is a copy of the mesh's own bounds rather than a lookup through
/// `mesh`, deliberately: extraction runs at the commit boundary, on the simulation thread, and a
/// render server is neither thread-safe nor reachable from a system's access declaration. Whoever
/// assigns the mesh copies the bounds with it — one write when the mesh changes, instead of a
/// handle resolution per entity per tick.
struct CY_REFLECT_TYPE(Category("Rendering"), Tooltip("A mesh this entity draws")) MeshRenderer {
    /// The mesh asset this entity draws. AUTHORED: this is what a `.cyworld` carries, what a
    /// dependency tracker follows, and what a rename rewrites.
    ///
    /// The field is called `mesh` because that is the name the editor's own mesh binding looks for
    /// (`cy_editor_services::primitives::MeshBinding::FIELD`), and the two sides being one name is
    /// what makes an authored primitive resolve here instead of round-tripping unread.
    CY_REFLECT_FIELD(AssetRef(Mesh), Category("Rendering"), Persistence(Authoring),
                     Tooltip("The mesh asset this entity draws."))
    AssetRef mesh;
    CY_REFLECT_FIELD(AssetRef(Material), Category("Rendering"), Persistence(Authoring),
                     Tooltip("The material this entity draws with."))
    AssetRef material;
    /// What the two references resolved to in THIS process. Runtime-only and never serialized, so
    /// deliberately not reflected: a handle is a slot index and a generation, and a file holding
    /// one loads into a process where that slot holds something else.
    render::MeshHandle mesh_handle;
    render::MaterialHandle material_handle;
    /// In the mesh's own space. The world bounds are derived from this and the transform, by
    /// whoever needs them; storing world bounds here would make them a second thing to keep in
    /// step.
    CY_REFLECT_FIELD(Category("Rendering"), Persistence(Derived),
                     Tooltip("The mesh's own bounds, in local space"))
    Aabb local_bounds = Aabb::from_center_extents(Vec3{0.0F, 0.0F, 0.0F}, Vec3{0.5F, 0.5F, 0.5F});
    CY_REFLECT_FIELD(Category("Rendering"), Persistence(Authoring),
                     Tooltip("Which layers this instance is drawn in"))
    render::LayerMask layer_mask = render::kDefaultLayer;
    /// Screen-coverage bias applied to LOD selection. Positive keeps more detail.
    CY_REFLECT_FIELD(Category("Rendering"), Persistence(Authoring),
                     Tooltip("Positive keeps more detail"))
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
    ///
    /// NOT REFLECTED, and that is the wrapper's doing rather than an omission: the generator
    /// refuses a field whose members are private, and `Presentation<>` holds its value behind an
    /// accessor precisely so an authoritative system cannot name it. A reflected lane here would be
    /// a second door into the value the wrapper exists to shut.
    determinism::Presentation<f32> importance{1.0F};

    CY_REFLECT_FIELD(Category("Rendering"), Persistence(Authoring)) bool visible = true;
    CY_REFLECT_FIELD(Category("Rendering"), Persistence(Authoring)) bool casts_shadow = true;
    CY_REFLECT_FIELD(Category("Rendering"), Persistence(Authoring)) bool receives_shadow = true;
    CY_REFLECT_FIELD(Category("Rendering"), Persistence(Authoring)) bool two_sided = false;
};

static_assert(std::is_trivially_copyable_v<MeshRenderer>,
              "the ECS refuses a component that is not trivially relocatable");

/// A light attached to an entity. Its placement is the entity's `WorldTransform`.
///
/// The fields are `render::LightDescription`'s, minus the transform and the stable id, which
/// extraction fills in from the entity. Restating them rather than embedding the description keeps
/// the component free of a field a designer must not set — a `stable_id` an author could type would
/// be an identity two entities could share, and the sort key rests on that being impossible.
struct CY_REFLECT_TYPE(Category("Rendering"), Tooltip("A light attached to this entity"))
    LightSource {
    CY_REFLECT_FIELD(Category("Lighting"), Persistence(Authoring),
                     Enum(Directional = 0, Point = 1, Spot = 2))
    render::LightKind kind = render::LightKind::Point;
    /// Linear, un-premultiplied. `intensity` carries the magnitude.
    ///
    /// AN ARRAY IS NOT A REFLECTED FIELD — the generator flattens an aggregate's members and a C
    /// array is not one — so the three channels are not in the schema. `state_schema.cpp` declares
    /// them by offset and does hash them, which is the statement that actually matters here.
    f32 color[3] = {1.0F, 1.0F, 1.0F};
    /// PHYSICAL UNITS, and which one depends on the kind: lux for a directional light, candela for
    /// a point or a spot. `rendering-lighting-and-shadows` requires the unit to be stated where the
    /// value is, because "intensity 5" means two different things for two kinds.
    CY_REFLECT_FIELD(Category("Lighting"), Persistence(Authoring),
                     Tooltip("Lux for a directional light, candela for a point or a spot"))
    f32 intensity = 1000.0F;
    /// Metres. Zero for a directional light.
    CY_REFLECT_FIELD(Category("Lighting"), Persistence(Authoring), Unit(Metres)) f32 range = 10.0F;
    CY_REFLECT_FIELD(Category("Lighting"), Persistence(Authoring), Unit(Radians))
    f32 inner_cone_radians = 0.0F;
    CY_REFLECT_FIELD(Category("Lighting"), Persistence(Authoring), Unit(Radians))
    f32 outer_cone_radians = 0.7853981634F;
    CY_REFLECT_FIELD(Category("Lighting"), Persistence(Authoring))
    render::LayerMask layer_mask = render::kAllLayers;
    CY_REFLECT_FIELD(Category("Lighting"), Persistence(Authoring)) bool casts_shadow = true;
    CY_REFLECT_FIELD(Category("Lighting"), Persistence(Authoring)) bool enabled = true;
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
struct CY_REFLECT_TYPE(Category("Rendering"), Tooltip("A camera this entity carries")) Camera {
    CY_REFLECT_FIELD(Category("Camera"), Persistence(Authoring)) render::Projection projection;
    CY_REFLECT_FIELD(Category("Camera"), Persistence(Authoring)) render::ViewportRect viewport;
    CY_REFLECT_FIELD(Category("Camera"), Persistence(Authoring),
                     Enum(Primary = 0, Shadow = 1, ReflectionProbe = 2, SceneCapture = 3,
                          EditorViewport = 4, Thumbnail = 5, XrEye = 6))
    render::ViewPurpose purpose = render::ViewPurpose::Primary;
    CY_REFLECT_FIELD(Category("Camera"), Persistence(Authoring))
    render::LayerMask layer_mask = render::kAllLayers;
    /// What share of the frame budget this camera's view asks for. A secondary view draws from its
    /// own allocation and degrades before the primary one does.
    CY_REFLECT_FIELD(Category("Camera"), Persistence(Authoring)) f32 importance = 1.0F;
    /// The identity temporal history keys off across frames — reprojection, TAA and any resource a
    /// view keeps between frames. Stable while the camera exists, and distinct from the entity id
    /// so that two views produced from one camera can carry different histories.
    CY_REFLECT_FIELD(Category("Camera"), Persistence(RuntimeState), ReadOnly) u64 history_id = 0;
    CY_REFLECT_FIELD(Category("Camera"), Persistence(Authoring)) bool enabled = true;
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
