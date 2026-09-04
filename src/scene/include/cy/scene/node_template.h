#pragma once
// Node types as data: a component set and its defaults, registered rather than inherited.
// Task 3.1.4.
//
// `scene-graph-and-nodes` — "Node types and components": a node "type" is "a **component archetype
// template** plus an optional behaviour, not ... C++ inheritance", users must be able to define
// their own "without engine changes", and a project's template is "a data declaration listing
// components and defaults, registered at startup".
//
// So there is no class hierarchy here and there is deliberately no `MeshRendererNode` type. A
// template is a name, a list of component *type names*, and a block of default bytes per component.
// Instantiating one creates an entity with that component set — which is the "composition over
// inheritance" scenario: a light that also emits sound is two components on one entity, and needs
// no new template at all.
//
// WHY COMPONENTS ARE NAMED RATHER THAN NUMBERED IN A TEMPLATE. A component id is per world
// (`ecs-core`), so a template that stored ids could only ever be instantiated into the world it was
// built against. A template stores names and is *bound* to a world when it is registered, which is
// also what makes a template a serializable authoring artefact rather than a runtime object.
//
// ------------------------------------------------------------------------------------------------
// WHAT THE SHIPPED CATALOGUE CAN AND CANNOT DO AT M2, STATED PLAINLY
// ------------------------------------------------------------------------------------------------
// The specification lists the templates the engine ships: spatial grouping, mesh rendering, skinned
// and instanced mesh, camera, four kinds of light, reflection probe, decal, VFX effect, audio
// listener and emitter, rigid and static body, character controller, collider shapes, navigation
// region and agent, UI host, sprite and tilemap. `builtin_templates()` returns that list, as data,
// with the component each entry needs.
//
// Most of those components do not exist yet: there is no renderer until M3, no physics or audio
// until M4 and M8, and no UI system until M7. A template naming a component no world has registered
// is **declared but not instantiable**, and `NodeTemplateRegistry::bind` reports exactly that
// rather than pretending. `template_status()` is how a caller asks which of the catalogue is live
// in its world. The alternative — inventing a `MeshRenderer` struct here so the list looks complete
// — would put the renderer's data model in the scene layer, at the wrong layer, a milestone early,
// and M3 would then have two of them.
//
// The spatial-grouping template is fully live at M2, because every component it names belongs to
// this module. That is the one the rest of M2 builds on.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/array.h>
#include <cy/core/values/name.h>
#include <cy/scene/components.h>

namespace cy::scene {

/// One component of a template: the name it is registered under, and the bytes a new instance
/// starts from. `defaults` may be null, which means "whatever the component's zero value is".
struct TemplateComponent {
    const char* component_name = "";
    const void* defaults = nullptr;
    u32 defaults_size = 0;
};

/// A node type, as a data declaration.
///
/// `components` and `behaviour` are borrowed, not copied: a template is normally a `constexpr`
/// array in the module that declares it, and copying the defaults into the registry would put a
/// second copy of every engine default in every world.
struct NodeTemplateDesc {
    const char* name = "";
    Span<const TemplateComponent> components;
    /// The behaviour attached to instances of this template, or empty for none. Resolved against
    /// the tree's behaviour registry at instantiation.
    const char* behaviour = "";
};

/// Whether a registered template can actually be instantiated in this world, and why not.
struct NodeTemplateStatus {
    Name name;
    bool instantiable = false;
    /// The first component the world has not registered. Empty when `instantiable`.
    const char* missing_component = "";
    u32 missing_count = 0;
};

/// One world's node templates.
///
/// Binding is separate from declaration and happens once: a template's component names are resolved
/// to this world's ids at `add()`, so instantiating one is an array walk and never a name lookup.
class NodeTemplateRegistry {
public:
    explicit NodeTemplateRegistry(Allocator& allocator) noexcept
        : entries_(allocator), bindings_(allocator) {}

    /// Register a template and bind its components against `world`.
    ///
    /// Succeeds even when a component is missing: the template is recorded as declared-but-not-
    /// instantiable, which is the state most of the shipped catalogue is in before M3. Refuses
    /// `AlreadyExists` for a name already registered, because two declarations of one node type is
    /// a project configuration error rather than a merge.
    [[nodiscard]] Status add(World& world, const NodeTemplateDesc& desc) noexcept;

    /// Register the whole shipped catalogue. Equivalent to `add()` per entry of
    /// `builtin_templates()`, and what `SceneTree::initialize` calls.
    [[nodiscard]] Status add_builtins(World& world) noexcept;

    [[nodiscard]] bool contains(Name name) const noexcept { return find(name) != nullptr; }
    [[nodiscard]] NodeTemplateStatus status(Name name) const noexcept;
    /// Every registered template's status, in registration order. Appends to `out`.
    [[nodiscard]] Status statuses(Array<NodeTemplateStatus>& out) const noexcept;
    [[nodiscard]] u32 size() const noexcept { return static_cast<u32>(entries_.size()); }

    /// The component ids one instance of `name` needs, and the defaults to write into them.
    struct Binding {
        ComponentTypeId component = kInvalidComponent;
        const void* defaults = nullptr;
        u32 defaults_size = 0;
    };

    /// The bound component list for a template, or an error naming what is missing.
    ///
    /// The span points into the registry's own storage and is valid until the next `add()`. Every
    /// caller uses it immediately, and saying so here is cheaper than a second allocation per
    /// template.
    [[nodiscard]] Expected<Span<const Binding>, Error> bindings_of(Name name) const noexcept;
    [[nodiscard]] const char* behaviour_of(Name name) const noexcept;

private:
    struct Entry {
        Name name;
        const char* behaviour = "";
        u32 first_binding = 0;
        u32 binding_count = 0;
        const char* missing_component = "";
        u32 missing_count = 0;
    };

    [[nodiscard]] const Entry* find(Name name) const noexcept;

    Array<Entry> entries_;
    /// Every template's bindings, back to back. One array rather than one per template: a template
    /// has a handful of components and an allocation each would be more bookkeeping than data.
    Array<Binding> bindings_;
};

/// The names of the templates the engine ships. Spelled as constants because a caller that wants
/// "the spatial one" should not be re-typing a string that a rename would silently break.
inline constexpr const char* kSpatialTemplate = "Spatial";
inline constexpr const char* kMeshRendererTemplate = "MeshRenderer";
inline constexpr const char* kSkinnedMeshTemplate = "SkinnedMesh";
inline constexpr const char* kInstancedMeshTemplate = "InstancedMesh";
inline constexpr const char* kCameraTemplate = "Camera";
inline constexpr const char* kDirectionalLightTemplate = "DirectionalLight";
inline constexpr const char* kPointLightTemplate = "PointLight";
inline constexpr const char* kSpotLightTemplate = "SpotLight";
inline constexpr const char* kAreaLightTemplate = "AreaLight";
inline constexpr const char* kReflectionProbeTemplate = "ReflectionProbe";
inline constexpr const char* kDecalTemplate = "Decal";
inline constexpr const char* kEffectTemplate = "Effect";
inline constexpr const char* kAudioListenerTemplate = "AudioListener";
inline constexpr const char* kAudioEmitterTemplate = "AudioEmitter";
inline constexpr const char* kRigidBodyTemplate = "RigidBody";
inline constexpr const char* kStaticBodyTemplate = "StaticBody";
inline constexpr const char* kCharacterControllerTemplate = "CharacterController";
inline constexpr const char* kColliderTemplate = "Collider";
inline constexpr const char* kNavigationRegionTemplate = "NavigationRegion";
inline constexpr const char* kNavigationAgentTemplate = "NavigationAgent";
inline constexpr const char* kUiHostTemplate = "UiHost";
inline constexpr const char* kSpriteTemplate = "Sprite";
inline constexpr const char* kTilemapTemplate = "Tilemap";

/// The shipped catalogue, as data. Read the header comment before assuming an entry is live.
[[nodiscard]] Span<const NodeTemplateDesc> builtin_templates() noexcept;

/// The components every node has, whatever its template: name, order, transforms, flags and
/// propagation state. A template's own components are added to these.
[[nodiscard]] Span<const TemplateComponent> node_base_components() noexcept;

}  // namespace cy::scene
