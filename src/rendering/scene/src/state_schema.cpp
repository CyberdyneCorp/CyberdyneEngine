// The renderer's built-in components, declared to the state hash. See
// cy/rendering/scene/state_schema.h, which carries the reasoning for every classification below.

#include <cy/rendering/scene/state_schema.h>

#include <cstddef>

namespace cy::rendering {
namespace {

using determinism::SchemaSubject;
using determinism::SimulationClass;
using determinism::StateEncoding;
using determinism::StateField;

using reflect::FieldKind;

}  // namespace

Status declare_render_state(determinism::StateSchema& schema,
                            const RenderComponents& components) noexcept {
    if (!components.registered()) {
        return fail(ErrorCode::InvalidArgument,
                    "the renderer's components are not registered in this world; a schema over an "
                    "invalid component id would address nothing while reporting coverage");
    }

    // --- MeshRenderer ---------------------------------------------------------------------
    //
    // A handle is one `u64` — a slot index and a generation — and it is declared `Derived` because
    // that number is the render server's allocation order rather than anything about the world. See
    // the header.
    const StateField mesh_fields[] = {
        StateField{"mesh", 1, offsetof(MeshRenderer, mesh), FieldKind::U64,
                   SimulationClass::Derived, StateEncoding::Direct},
        StateField{"material", 2, offsetof(MeshRenderer, material), FieldKind::U64,
                   SimulationClass::Derived, StateEncoding::Direct},
        StateField{"bounds.min.x", 3, offsetof(MeshRenderer, local_bounds) + offsetof(Aabb, min),
                   FieldKind::F32, SimulationClass::Derived, StateEncoding::Direct},
        StateField{"bounds.min.y", 4,
                   offsetof(MeshRenderer, local_bounds) + offsetof(Aabb, min) + 4U, FieldKind::F32,
                   SimulationClass::Derived, StateEncoding::Direct},
        StateField{"bounds.min.z", 5,
                   offsetof(MeshRenderer, local_bounds) + offsetof(Aabb, min) + 8U, FieldKind::F32,
                   SimulationClass::Derived, StateEncoding::Direct},
        StateField{"bounds.max.x", 6, offsetof(MeshRenderer, local_bounds) + offsetof(Aabb, max),
                   FieldKind::F32, SimulationClass::Derived, StateEncoding::Direct},
        StateField{"bounds.max.y", 7,
                   offsetof(MeshRenderer, local_bounds) + offsetof(Aabb, max) + 4U, FieldKind::F32,
                   SimulationClass::Derived, StateEncoding::Direct},
        StateField{"bounds.max.z", 8,
                   offsetof(MeshRenderer, local_bounds) + offsetof(Aabb, max) + 8U, FieldKind::F32,
                   SimulationClass::Derived, StateEncoding::Direct},
        StateField{"layer_mask", 9, offsetof(MeshRenderer, layer_mask), FieldKind::U32,
                   SimulationClass::Authoritative, StateEncoding::Direct},
        StateField{"lod_bias", 10, offsetof(MeshRenderer, lod_bias), FieldKind::F32,
                   SimulationClass::Authoritative, StateEncoding::Direct},
        // The classification here and the `Presentation<>` wrapper on the field itself are the same
        // statement made to two mechanisms: the wrapper stops an authoritative system naming the
        // value, and this stops the hash folding it.
        StateField{"importance", 11, offsetof(MeshRenderer, importance), FieldKind::F32,
                   SimulationClass::Presentation, StateEncoding::Direct},
        StateField{"visible", 12, offsetof(MeshRenderer, visible), FieldKind::Bool,
                   SimulationClass::Authoritative, StateEncoding::Direct},
        StateField{"casts_shadow", 13, offsetof(MeshRenderer, casts_shadow), FieldKind::Bool,
                   SimulationClass::Authoritative, StateEncoding::Direct},
        StateField{"receives_shadow", 14, offsetof(MeshRenderer, receives_shadow), FieldKind::Bool,
                   SimulationClass::Authoritative, StateEncoding::Direct},
        StateField{"two_sided", 15, offsetof(MeshRenderer, two_sided), FieldKind::Bool,
                   SimulationClass::Authoritative, StateEncoding::Direct},
    };
    if (Status declared =
            schema.declare(SchemaSubject{components.mesh_renderer}, kMeshRendererComponentName,
                           Span<const StateField>(mesh_fields, 15));
        !declared) {
        return declared;
    }

    // --- LightSource ----------------------------------------------------------------------
    //
    // Every field authoritative: a light's colour, intensity, range, cone and shadow flag are
    // authored state, and a divergence in any of them is a divergence in the world.
    const StateField light_fields[] = {
        StateField{"kind", 1, offsetof(LightSource, kind), FieldKind::Enum,
                   SimulationClass::Authoritative, StateEncoding::Direct},
        StateField{"color.r", 2, offsetof(LightSource, color), FieldKind::F32,
                   SimulationClass::Authoritative, StateEncoding::Direct},
        StateField{"color.g", 3, offsetof(LightSource, color) + 4U, FieldKind::F32,
                   SimulationClass::Authoritative, StateEncoding::Direct},
        StateField{"color.b", 4, offsetof(LightSource, color) + 8U, FieldKind::F32,
                   SimulationClass::Authoritative, StateEncoding::Direct},
        StateField{"intensity", 5, offsetof(LightSource, intensity), FieldKind::F32,
                   SimulationClass::Authoritative, StateEncoding::Direct},
        StateField{"range", 6, offsetof(LightSource, range), FieldKind::F32,
                   SimulationClass::Authoritative, StateEncoding::Direct},
        StateField{"inner_cone", 7, offsetof(LightSource, inner_cone_radians), FieldKind::F32,
                   SimulationClass::Authoritative, StateEncoding::Direct},
        StateField{"outer_cone", 8, offsetof(LightSource, outer_cone_radians), FieldKind::F32,
                   SimulationClass::Authoritative, StateEncoding::Direct},
        StateField{"layer_mask", 9, offsetof(LightSource, layer_mask), FieldKind::U32,
                   SimulationClass::Authoritative, StateEncoding::Direct},
        StateField{"casts_shadow", 10, offsetof(LightSource, casts_shadow), FieldKind::Bool,
                   SimulationClass::Authoritative, StateEncoding::Direct},
        StateField{"enabled", 11, offsetof(LightSource, enabled), FieldKind::Bool,
                   SimulationClass::Authoritative, StateEncoding::Direct},
    };
    if (Status declared =
            schema.declare(SchemaSubject{components.light_source}, kLightSourceComponentName,
                           Span<const StateField>(light_fields, 11));
        !declared) {
        return declared;
    }

    // --- Camera ---------------------------------------------------------------------------
    //
    // Every field `Presentation`, so the subject is declared and hashes nothing. That is a
    // different fact from "not declared", and the report says which — see the header for why a
    // camera in the hash would make two clients watching from different angles diverge.
    const StateField camera_fields[] = {
        StateField{"projection.kind", 1,
                   offsetof(Camera, projection) + offsetof(render::Projection, kind),
                   FieldKind::Enum, SimulationClass::Presentation, StateEncoding::Direct},
        StateField{"projection.fov_y", 2,
                   offsetof(Camera, projection) + offsetof(render::Projection, fov_y_radians),
                   FieldKind::F32, SimulationClass::Presentation, StateEncoding::Direct},
        StateField{"projection.near", 3,
                   offsetof(Camera, projection) + offsetof(render::Projection, near_plane),
                   FieldKind::F32, SimulationClass::Presentation, StateEncoding::Direct},
        StateField{"projection.far", 4,
                   offsetof(Camera, projection) + offsetof(render::Projection, far_plane),
                   FieldKind::F32, SimulationClass::Presentation, StateEncoding::Direct},
        StateField{"importance", 5, offsetof(Camera, importance), FieldKind::F32,
                   SimulationClass::Presentation, StateEncoding::Direct},
        StateField{"history_id", 6, offsetof(Camera, history_id), FieldKind::U64,
                   SimulationClass::Presentation, StateEncoding::Direct},
        StateField{"enabled", 7, offsetof(Camera, enabled), FieldKind::Bool,
                   SimulationClass::Presentation, StateEncoding::Direct},
    };
    return schema.declare(SchemaSubject{components.camera}, kCameraComponentName,
                          Span<const StateField>(camera_fields, 7));
}

}  // namespace cy::rendering
