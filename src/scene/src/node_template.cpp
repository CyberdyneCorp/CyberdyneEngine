// Node types as data. Task 3.1.4.
//
// The shipped catalogue is a `constexpr` table at the bottom of this file and nothing else: no
// factory, no registration macro, no class per node type. A project's own template is the same
// shape, declared in the project and passed to `add()`, which is what "users SHALL be able to
// define their own templates without engine changes" reduces to when node types are data.

#include <cy/scene/node_template.h>

#include <cy/ecs/world.h>

namespace cy::scene {
namespace {

/// The components every node carries whatever its template. Ordered as `SceneComponents` registers
/// them, which is also the order an archetype sorts its columns in.
constexpr TemplateComponent kBaseComponents[] = {
    {kNodeNameComponentName, nullptr, 0},       {kChildOrderComponentName, nullptr, 0},
    {kLocalTransformComponentName, nullptr, 0}, {kWorldTransformComponentName, nullptr, 0},
    {kNodeFlagsComponentName, nullptr, 0},      {kNodeStateComponentName, nullptr, 0},
};

// The components the shipped templates name. Several of them belong to milestones that have not
// happened: there is no UI system yet and no effect system yet. Naming them here is the declaration
// `scene-graph-and-nodes` asks the engine to ship; registering them is those milestones' work, and
// until then the template is declared and reports itself as not instantiable rather than pretending
// otherwise. See the header.
//
// ================================================================================================
// M8.b TASK 11.3: FIVE OF THESE NAMED A COMPONENT THAT NEVER EXISTED
// ================================================================================================
//
// The renderer's components have been `cy::rendering::MeshRenderer`, `cy::rendering::Camera` and
// `cy::rendering::LightSource` since M3, in src/rendering/scene/. This table asked for
// `cy::render::MeshRenderer`, `cy::render::Camera` and four separate light components — a namespace
// that is the render SERVER's and a light model the engine does not have — so the templates stayed
// "declared but not instantiable" in a world that had registered every one of them, and an author
// who created a MeshRenderer node got an entity with no renderer component on it. That is the
// smaller half of why M8.a's artefact photograph shows an authored sphere as a unit box.
//
// The three light templates the engine's light model supports now name ONE component: `LightSource`
// carries a `render::LightKind`, and a template is "a component archetype template plus defaults"
// precisely so that three node types can be three defaults over one component rather than three
// structs. THE DEFAULTS THEMSELVES ARE NOT HERE — a `defaults` blob is the component's own bytes
// and this layer may not include the renderer's header. `cy::rendering::declare_render_templates()`
// supplies them through `NodeTemplateRegistry::redeclare`, and until it is called these templates
// instantiate a zeroed component, which for a `LightSource` is a disabled light of zero intensity.
constexpr TemplateComponent kMeshRendererComponents[] = {
    {"cy::rendering::MeshRenderer", nullptr, 0}};
constexpr TemplateComponent kSkinnedMeshComponents[] = {{"cy::render::SkinnedMesh", nullptr, 0}};
constexpr TemplateComponent kInstancedMeshComponents[] = {
    {"cy::render::InstancedMesh", nullptr, 0}};
constexpr TemplateComponent kCameraComponents[] = {{"cy::rendering::Camera", nullptr, 0}};

constexpr TemplateComponent kDirectionalLightComponents[] = {
    {"cy::rendering::LightSource", nullptr, 0}};
constexpr TemplateComponent kPointLightComponents[] = {{"cy::rendering::LightSource", nullptr, 0}};
constexpr TemplateComponent kSpotLightComponents[] = {{"cy::rendering::LightSource", nullptr, 0}};
// AN AREA LIGHT IS NOT A KIND THIS ENGINE HAS. `render::LightKind` is directional, point and spot,
// and giving this template a fourth default would write a number the enumeration does not define.
// It keeps naming a component nothing registers, so `template_status()` reports it as declared and
// not instantiable — which is the true answer and the one the header asks for.
constexpr TemplateComponent kAreaLightComponents[] = {{"cy::rendering::AreaLight", nullptr, 0}};
constexpr TemplateComponent kReflectionProbeComponents[] = {
    {"cy::render::ReflectionProbe", nullptr, 0}};
constexpr TemplateComponent kDecalComponents[] = {{"cy::render::Decal", nullptr, 0}};
constexpr TemplateComponent kEffectComponents[] = {{"cy::vfx::Effect", nullptr, 0}};
constexpr TemplateComponent kAudioListenerComponents[] = {{"cy::audio::Listener", nullptr, 0}};
constexpr TemplateComponent kAudioEmitterComponents[] = {{"cy::audio::Emitter", nullptr, 0}};
constexpr TemplateComponent kRigidBodyComponents[] = {{"cy::physics::RigidBody", nullptr, 0}};
constexpr TemplateComponent kStaticBodyComponents[] = {{"cy::physics::StaticBody", nullptr, 0}};
constexpr TemplateComponent kCharacterControllerComponents[] = {
    {"cy::physics::CharacterController", nullptr, 0}};
constexpr TemplateComponent kColliderComponents[] = {{"cy::physics::Collider", nullptr, 0}};
constexpr TemplateComponent kNavigationRegionComponents[] = {{"cy::nav::Region", nullptr, 0}};
constexpr TemplateComponent kNavigationAgentComponents[] = {{"cy::nav::Agent", nullptr, 0}};
constexpr TemplateComponent kUiHostComponents[] = {{"cy::ui::Host", nullptr, 0}};
constexpr TemplateComponent kSpriteComponents[] = {{"cy::render::Sprite", nullptr, 0}};
constexpr TemplateComponent kTilemapComponents[] = {{"cy::render::Tilemap", nullptr, 0}};

constexpr NodeTemplateDesc kBuiltins[] = {
    // Spatial grouping is the one template every component of which belongs to this module, so it
    // is the one that is live at M2 — and it is the template a bare node is.
    {kSpatialTemplate, Span<const TemplateComponent>(), ""},
    {kMeshRendererTemplate, Span<const TemplateComponent>(kMeshRendererComponents, 1), ""},
    {kSkinnedMeshTemplate, Span<const TemplateComponent>(kSkinnedMeshComponents, 1), ""},
    {kInstancedMeshTemplate, Span<const TemplateComponent>(kInstancedMeshComponents, 1), ""},
    {kCameraTemplate, Span<const TemplateComponent>(kCameraComponents, 1), ""},
    {kDirectionalLightTemplate, Span<const TemplateComponent>(kDirectionalLightComponents, 1), ""},
    {kPointLightTemplate, Span<const TemplateComponent>(kPointLightComponents, 1), ""},
    {kSpotLightTemplate, Span<const TemplateComponent>(kSpotLightComponents, 1), ""},
    {kAreaLightTemplate, Span<const TemplateComponent>(kAreaLightComponents, 1), ""},
    {kReflectionProbeTemplate, Span<const TemplateComponent>(kReflectionProbeComponents, 1), ""},
    {kDecalTemplate, Span<const TemplateComponent>(kDecalComponents, 1), ""},
    {kEffectTemplate, Span<const TemplateComponent>(kEffectComponents, 1), ""},
    {kAudioListenerTemplate, Span<const TemplateComponent>(kAudioListenerComponents, 1), ""},
    {kAudioEmitterTemplate, Span<const TemplateComponent>(kAudioEmitterComponents, 1), ""},
    {kRigidBodyTemplate, Span<const TemplateComponent>(kRigidBodyComponents, 1), ""},
    {kStaticBodyTemplate, Span<const TemplateComponent>(kStaticBodyComponents, 1), ""},
    {kCharacterControllerTemplate, Span<const TemplateComponent>(kCharacterControllerComponents, 1),
     ""},
    {kColliderTemplate, Span<const TemplateComponent>(kColliderComponents, 1), ""},
    {kNavigationRegionTemplate, Span<const TemplateComponent>(kNavigationRegionComponents, 1), ""},
    {kNavigationAgentTemplate, Span<const TemplateComponent>(kNavigationAgentComponents, 1), ""},
    // "Individual UI elements SHALL NOT be node templates": the host is the scene's single point of
    // attachment to a UI document, and the document's elements live in the UI system's storage.
    {kUiHostTemplate, Span<const TemplateComponent>(kUiHostComponents, 1), ""},
    {kSpriteTemplate, Span<const TemplateComponent>(kSpriteComponents, 1), ""},
    {kTilemapTemplate, Span<const TemplateComponent>(kTilemapComponents, 1), ""},
};

}  // namespace

Span<const NodeTemplateDesc> builtin_templates() noexcept {
    return {kBuiltins, sizeof(kBuiltins) / sizeof(kBuiltins[0])};
}

Span<const TemplateComponent> node_base_components() noexcept {
    return {kBaseComponents, sizeof(kBaseComponents) / sizeof(kBaseComponents[0])};
}

Status NodeTemplateRegistry::bind_entry(World& world, Entry& entry) noexcept {
    entry.first_binding = static_cast<u32>(bindings_.size());
    entry.missing_component = "";
    entry.missing_count = 0;
    for (const TemplateComponent& component : entry.declared) {
        const ecs::ComponentInfo* info = world.components().find(component.component_name);
        if (info == nullptr) {
            // Declared but not instantiable in this world. Recorded rather than refused: the
            // catalogue names components several milestones will register, and a registry that
            // refused them would make the catalogue undeclarable until M8. `rebind()` is what turns
            // this back into a binding once the milestone that owns the component arrives.
            ++entry.missing_count;
            if (entry.missing_component[0] == '\0') {
                entry.missing_component = component.component_name;
            }
            continue;
        }
        Binding binding;
        binding.component = info->id;
        binding.defaults = component.defaults;
        binding.defaults_size = component.defaults_size;
        if (Status pushed = bindings_.push_back(binding); !pushed) {
            return pushed;
        }
    }
    entry.binding_count = static_cast<u32>(bindings_.size()) - entry.first_binding;
    return ok();
}

Status NodeTemplateRegistry::add(World& world, const NodeTemplateDesc& desc) noexcept {
    const Name name = Name::intern(desc.name);
    if (contains(name)) {
        return fail(ErrorCode::AlreadyExists, "a node template with this name is registered");
    }

    Entry entry;
    entry.name = name;
    entry.behaviour = desc.behaviour;
    entry.declared = desc.components;
    if (Status bound = bind_entry(world, entry); !bound) {
        return bound;
    }
    return entries_.push_back(entry);
}

Status NodeTemplateRegistry::redeclare(World& world, const NodeTemplateDesc& desc) noexcept {
    const Name name = Name::intern(desc.name);
    Entry* entry = nullptr;
    for (Entry& candidate : entries_) {
        if (candidate.name == name) {
            entry = &candidate;
        }
    }
    if (entry == nullptr) {
        return fail(ErrorCode::NotFound, "no node template with this name is registered");
    }
    entry->behaviour = desc.behaviour;
    entry->declared = desc.components;
    return rebind(world);
}

Status NodeTemplateRegistry::rebind(World& world) noexcept {
    // The whole binding array is rebuilt rather than patched: an entry's bindings are a contiguous
    // run, and a template that gains one has to grow in the middle. Rebuilding is O(templates) with
    // one pass and no special case, and this runs once per subsystem registration rather than per
    // frame.
    bindings_.clear();
    for (Entry& entry : entries_) {
        if (Status bound = bind_entry(world, entry); !bound) {
            return bound;
        }
    }
    return ok();
}

Status NodeTemplateRegistry::add_builtins(World& world) noexcept {
    for (const NodeTemplateDesc& desc : builtin_templates()) {
        if (Status added = add(world, desc); !added) {
            return added;
        }
    }
    return ok();
}

const NodeTemplateRegistry::Entry* NodeTemplateRegistry::find(Name name) const noexcept {
    for (const Entry& entry : entries_) {
        if (entry.name == name) {
            return &entry;
        }
    }
    return nullptr;
}

NodeTemplateStatus NodeTemplateRegistry::status(Name name) const noexcept {
    NodeTemplateStatus status;
    status.name = name;
    const Entry* entry = find(name);
    if (entry == nullptr) {
        status.missing_component = "the template is not registered";
        return status;
    }
    status.instantiable = entry->missing_count == 0;
    status.missing_component = entry->missing_component;
    status.missing_count = entry->missing_count;
    return status;
}

Status NodeTemplateRegistry::statuses(Array<NodeTemplateStatus>& out) const noexcept {
    for (const Entry& entry : entries_) {
        if (Status pushed = out.push_back(status(entry.name)); !pushed) {
            return pushed;
        }
    }
    return ok();
}

Expected<Span<const NodeTemplateRegistry::Binding>, Error> NodeTemplateRegistry::bindings_of(
    Name name) const noexcept {
    const Entry* entry = find(name);
    if (entry == nullptr) {
        return fail(ErrorCode::NotFound, "no node template with this name is registered");
    }
    if (entry->missing_count != 0) {
        return fail(ErrorCode::Unavailable,
                    "this node template names a component this world has not registered");
    }
    return Span<const Binding>(bindings_.data() + entry->first_binding, entry->binding_count);
}

const char* NodeTemplateRegistry::behaviour_of(Name name) const noexcept {
    const Entry* entry = find(name);
    return (entry == nullptr) ? "" : entry->behaviour;
}

}  // namespace cy::scene
