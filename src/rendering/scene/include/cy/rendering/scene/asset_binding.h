#pragma once
// AN AUTHORED ASSET REFERENCE BECOMES A RENDERER HANDLE. M8.b task 11.3, the second half.
//
// ================================================================================================
// THE HALF OF THE DEBT THE REFLECTED TYPE DOES NOT CLOSE
// ================================================================================================
//
// `components.h` says what task 11.3's first half did: `MeshRenderer` is reflected, so a `.cyworld`
// carries the mesh a designer picked and `resolve_against` matches it to a real component. That
// makes the reference SURVIVE. It does not make it DRAW.
//
// The chain from an authored node to a pixel has four links, and only the last two existed:
//
//   1. the editor writes an asset reference into `MeshRenderer::mesh`      — M8.a
//   2. the reference resolves onto a real reflected component in a world   — task 11.3, first half
//   3. SOMETHING TURNS THE REFERENCE INTO A `render::MeshHandle`           — this file
//   4. the extract stage publishes the handle and the assembly draws it    — M3, and task 11.2
//
// Link 3 was missing entirely: `extract.cpp` reads `mesh_handle`, nothing wrote it, and a zeroed
// handle is the null handle — so every authored instance reached the renderer naming mesh nothing,
// which is precisely "an authored mesh reaches no renderer" stated one layer further along.
//
// ================================================================================================
// WHY THIS TAKES A RESOLVER AND DOES NOT LOAD ANYTHING
// ================================================================================================
//
// Binding is not loading. WHERE a mesh comes from — a cooked package, a streaming residency budget,
// an editor's in-memory import — is `core-assets-and-io`'s and `asset-import-pipeline`'s, and a
// renderer module that opened a file would be this module deciding a residency policy. What is
// missing is narrower and is entirely the renderer's: the mapping from the reference a component
// holds to the slot this process gave it.
//
// So the caller supplies two lookups and this walks the world. A host that has an asset system
// answers from it; a test answers from a table; neither has to know how the other does it.
//
// ================================================================================================
// WHAT AN UNRESOLVED REFERENCE DOES, AND WHY IT IS NOT LEFT ALONE
// ================================================================================================
//
// A reference the resolver cannot answer CLEARS the handle rather than keeping the last one.
//
// That is the conservative direction and it is the only honest one: a handle is a slot index and a
// generation, the slot is reused when the mesh it named is unloaded, and a stale handle draws
// WHATEVER IS IN THAT SLOT NOW. An instance drawing nothing is a visible, countable, reportable
// gap — `BindReport::unresolved_meshes` — and an instance drawing another asset's geometry is a
// bug that looks like an authoring mistake.
//
// A reference that is NIL is not an unresolved reference: it is a component that names no asset,
// which is what a freshly created node has before a designer picks one. Those are counted
// separately and their handles are left exactly as they are, because a caller that assigned a
// handle directly — which is what M3's own tests and `samples/03-first-light` do — must not have it
// taken away by a binding pass that ran over the same world.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/math/shapes.h>
#include <cy/rendering/scene/components.h>
#include <cy/servers/render/handles.h>

namespace cy::rendering {

/// What a mesh reference resolved to in this process.
///
/// `local_bounds` travels WITH the handle because `components.h` requires it to: the extract stage
/// reads the component's own copy of the mesh's bounds rather than asking a server for them, and
/// "whoever assigns the mesh copies the bounds with it" is the rule that keeps that copy true.
struct MeshResolution {
    render::MeshHandle mesh;
    Aabb local_bounds = Aabb::empty();
    bool found = false;
};

/// What a material reference resolved to. No bounds: a material has none.
struct MaterialResolution {
    render::MaterialHandle material;
    bool found = false;
};

/// The two lookups. Plain function pointers with a user datum, the same shape `SurfaceQueryFn` and
/// every other engine callback take — a `std::function` here would allocate once per host and the
/// renderer's own budget rules forbid it.
using MeshResolverFn = MeshResolution (*)(AssetRef reference, void* user) noexcept;
using MaterialResolverFn = MaterialResolution (*)(AssetRef reference, void* user) noexcept;

struct AssetResolver {
    MeshResolverFn mesh = nullptr;
    MaterialResolverFn material = nullptr;
    void* user = nullptr;
};

/// What one binding pass did. Counters, because "it worked" and "nine of the eleven meshes this
/// level names are not loaded yet" are both things a host has to be able to say.
struct BindReport {
    /// `MeshRenderer` components visited.
    u32 examined = 0;
    u32 meshes_bound = 0;
    u32 materials_bound = 0;
    /// References the resolver did not answer. Their handles were CLEARED — see the header.
    u32 unresolved_meshes = 0;
    u32 unresolved_materials = 0;
    /// Components whose reference is nil: they name no asset, and their handles were left alone.
    u32 unreferenced_meshes = 0;
    u32 unreferenced_materials = 0;
};

/// Fill every `MeshRenderer`'s handles from the asset references it carries.
///
/// Idempotent, and cheap to repeat: a host runs it after a load, after a streaming step, and after
/// an editor transaction that changed a reference, and a component whose reference resolved to the
/// same handle is written with the same value.
///
/// Refuses `InvalidArgument` when `components.mesh_renderer` is not a component of `world`, or when
/// `resolver.mesh` is null — a pass with no mesh lookup would clear every handle in the world and
/// report that it had done its job.
[[nodiscard]] Status bind_render_assets(World& world, const RenderComponents& components,
                                        const AssetResolver& resolver, BindReport& out) noexcept;

}  // namespace cy::rendering
