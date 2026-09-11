// The data-interface registry and the engine's built-in list. M8.c task 2.5. See interfaces.h for
// why the compiler does not know these names.

#include <cy/vfx/interfaces.h>

#include <cy/graph/expr.h>

#include <utility>

namespace cy::vfx {
namespace {

using graph::hash_text;
using graph::hash_u64;
using graph::kHashSeed;

constexpr InterfaceField kDepthFields[] = {{"depth", Float}, {"linear_depth", Float}};
constexpr InterfaceField kNormalFields[] = {{"normal", Float3}, {"roughness", Float}};
constexpr InterfaceField kSdfFields[] = {{"distance", Float}, {"gradient", Float3}};
constexpr InterfaceField kGpuSceneFields[] = {
    {"instance_count", Int}, {"instance_position", Float3}, {"instance_radius", Float}};
constexpr InterfaceField kPhysicsFields[] = {
    {"hit", Float}, {"hit_position", Float3}, {"hit_normal", Float3}, {"hit_distance", Float}};
constexpr InterfaceField kTerrainFields[] = {
    {"height", Float}, {"normal", Float3}, {"material", Int}};
constexpr InterfaceField kStaticMeshFields[] = {
    {"position", Float3}, {"normal", Float3}, {"uv", Float2}, {"triangle_count", Int}};
constexpr InterfaceField kSkeletalMeshFields[] = {
    {"position", Float3}, {"normal", Float3}, {"bone_index", Int}, {"velocity", Float3}};
constexpr InterfaceField kTextureFields[] = {{"sample", Float4}, {"red", Float}};
constexpr InterfaceField kCurveFields[] = {{"value", Float}, {"derivative", Float}};
constexpr InterfaceField kCameraFields[] = {
    {"position", Float3}, {"forward", Float3}, {"up", Float3}, {"fov_y", Float}};
constexpr InterfaceField kFieldFields[] = {{"value", Float}, {"gradient", Float3}};
constexpr InterfaceField kAudioFields[] = {{"level", Float}, {"spectrum", Float}, {"peak", Float}};
constexpr InterfaceField kEcsFields[] = {{"count", Int}, {"position", Float3}, {"value", Float}};
constexpr InterfaceField kBufferFields[] = {{"value", Float}, {"value4", Float4}, {"count", Int}};
constexpr InterfaceField kWindFields[] = {{"velocity", Float3}, {"speed", Float}, {"gust", Float}};
constexpr InterfaceField kGridFields[] = {
    {"density", Float}, {"velocity", Float3}, {"temperature", Float}};

constexpr DataInterfaceDesc kBuiltins[] = {
    {"scene_depth", 1, {kDepthFields, 2}, InterfaceCost::Moderate, false, true},
    {"scene_normals", 1, {kNormalFields, 2}, InterfaceCost::Moderate, false, true},
    {"scene_sdf", 1, {kSdfFields, 2}, InterfaceCost::Expensive, false, true},
    {"gpu_scene", 1, {kGpuSceneFields, 3}, InterfaceCost::Cheap, false, true},
    // The one interface whose CPU form is the authoritative one: a physics query on the CPU path is
    // a real cast through `PhysicsServer`, and on the GPU path it is not available at all. That
    // asymmetry is `vfx-system`'s own — "physics queries FOR THE CPU PATH" — and stating it here is
    // what makes a GPU effect that reaches for one fail at cook rather than at run time.
    {"physics_query", 1, {kPhysicsFields, 4}, InterfaceCost::Expensive, true, false},
    {"terrain", 1, {kTerrainFields, 3}, InterfaceCost::Moderate, true, true},
    {"static_mesh", 1, {kStaticMeshFields, 4}, InterfaceCost::Moderate, true, true},
    {"skeletal_mesh", 1, {kSkeletalMeshFields, 4}, InterfaceCost::Moderate, false, true},
    {"texture", 1, {kTextureFields, 2}, InterfaceCost::Moderate, true, true},
    {"curve", 1, {kCurveFields, 2}, InterfaceCost::Cheap, true, true},
    {"camera", 1, {kCameraFields, 4}, InterfaceCost::Trivial, true, true},
    // `environment-fields` is M10's; the interface is declared so a graph can be authored against
    // it and a cook can say what it costs, and the resource it binds arrives with that capability.
    {"environment_field", 1, {kFieldFields, 2}, InterfaceCost::Moderate, true, true},
    {"audio", 1, {kAudioFields, 3}, InterfaceCost::Cheap, true, true},
    {"ecs_query", 1, {kEcsFields, 3}, InterfaceCost::Cheap, true, true},
    {"structured_buffer", 1, {kBufferFields, 3}, InterfaceCost::Cheap, true, true},
    // THE SHARED WIND FIELD, not a VFX-owned wind model. See the note in interfaces.h.
    {"wind_field", 1, {kWindFields, 3}, InterfaceCost::Cheap, true, true},
    // THE FLUID SEAM. Reserved and not filled; see `kGridInterfaceName` in interfaces.h.
    {kGridInterfaceName, 1, {kGridFields, 3}, InterfaceCost::Expensive, false, true},
};

}  // namespace

const char* interface_cost_name(InterfaceCost cost) noexcept {
    switch (cost) {
        case InterfaceCost::Trivial:
            return "Trivial";
        case InterfaceCost::Cheap:
            return "Cheap";
        case InterfaceCost::Moderate:
            return "Moderate";
        case InterfaceCost::Expensive:
            return "Expensive";
    }
    return "?";
}

u32 interface_cost_weight(InterfaceCost cost) noexcept {
    switch (cost) {
        case InterfaceCost::Trivial:
            return 1;
        case InterfaceCost::Cheap:
            return 4;
        case InterfaceCost::Moderate:
            return 16;
        case InterfaceCost::Expensive:
            return 64;
    }
    return 0;
}

DataInterface::DataInterface(Allocator& allocator, const DataInterfaceDesc& desc) noexcept
    : name_(Name::intern(desc.name)),
      fields_(allocator),
      version_(desc.version),
      cost_(desc.cost),
      cpu_available_(desc.cpu_available),
      gpu_available_(desc.gpu_available) {
    // A push that fails leaves the interface with fewer fields than declared, and `find_field`
    // then reports the missing one as a compile error naming the field — which is the honest
    // failure for an allocator that refused.
    for (const InterfaceField& field : desc.fields) {
        (void)fields_.push_back(field);
    }
}

const InterfaceField* DataInterface::find_field(Name field) const noexcept {
    const std::string_view wanted = field.text();
    for (const InterfaceField& candidate : fields_) {
        if (wanted == candidate.name) {
            return &candidate;
        }
    }
    return nullptr;
}

Status DataInterfaceRegistry::register_interface(const DataInterfaceDesc& desc) noexcept {
    if (desc.name == nullptr || *desc.name == '\0') {
        return fail(ErrorCode::InvalidArgument, "vfx: a data interface needs a name");
    }
    if (!desc.cpu_available && !desc.gpu_available) {
        return fail(ErrorCode::InvalidArgument,
                    "vfx: a data interface available on neither path can never be sampled");
    }
    if (find(Name::intern(desc.name)) != nullptr) {
        return fail(ErrorCode::AlreadyExists, "vfx: that data interface is already registered");
    }
    DataInterface registered(interfaces_.allocator(), desc);
    if (registered.fields().size() != desc.fields.size()) {
        return fail(ErrorCode::OutOfMemory, "vfx: a data interface's field table did not fit");
    }
    return interfaces_.push_back(std::move(registered));
}

const DataInterface* DataInterfaceRegistry::find(Name interface_name) const noexcept {
    for (const DataInterface& candidate : interfaces_) {
        if (candidate.name() == interface_name) {
            return &candidate;
        }
    }
    return nullptr;
}

u64 DataInterfaceRegistry::digest() const noexcept {
    u64 digest = kHashSeed;
    for (const DataInterface& candidate : interfaces_) {
        digest = hash_text(digest, candidate.name().text());
        digest = hash_u64(digest, candidate.version());
    }
    return digest;
}

Status register_builtin_interfaces(DataInterfaceRegistry& registry) noexcept {
    for (const DataInterfaceDesc& desc : kBuiltins) {
        if (Status registered = registry.register_interface(desc); !registered) {
            return registered;
        }
    }
    return ok();
}

}  // namespace cy::vfx
