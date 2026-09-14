#pragma once
// The character: four Mixamo FBX files, imported, retargeted onto one rig, and turned into the four
// clips a compiled locomotion program plays. M8.d, samples/09b-animated-character.
//
// ================================================================================================
// WHAT THIS FILE JOINS, AND WHY IT IS THE FIRST THING THAT DOES
// ================================================================================================
//
// M8.d landed four pieces that had never met:
//
//   tools/import/       `import_fbx_skeleton` (step 7) and `import_fbx_animations` (step 8) produce
//                       a cooked skeleton record and a cooked clip record from an FBX.
//   src/animation/      `Skeleton`, `Clip`, `RetargetProfile` and `retarget_build.h`'s
//                       `build_retarget_profile` turn those records into a rig that can be posed.
//   src/graph/          `compile_locomotion` compiles a four-state machine over four named clips.
//   src/rendering/skinning/  `SkinPass` moves a vertex by a bone matrix, on the device.
//
// Every one of them was reachable only from its own test suite. THIS file is the first caller that
// has all four in one translation unit, which is the only way to find out whether the seams between
// them line up — and two of them did not until this artefact was written. They are recorded in
// README.md rather than smoothed over here.
//
// ================================================================================================
// THE ONE SUBSTITUTION — AND M11.b REMOVED THE REASON FOR IT
// ================================================================================================
//
// THE SKIN WEIGHTS WERE NOT THE ARTIST'S. Through M10, `tools/import/`'s `MeshData` had no
// joint-index or joint-weight arrays and `write_cooked_mesh` had no attribute bit for them, so the
// cooked mesh this artefact reads carried positions, normals, UVs and indices and NOTHING about
// which bone moves which vertex. `Walking.fbx` has that information — one `ufbx_skin_deformer`, 65
// clusters, 14,634 skinned vertices at up to six influences each — and the import pipeline dropped
// it, which its own `skipped-rig` diagnostic said out loud.
//
// M11.b task 6.1 gave `MeshData` those arrays and taught both model importers to fill them, so this
// artefact now PREFERS the artist's weights and falls back to deriving them. `SkinReport::imported`
// says which happened and the printed report names it, because the alternative — a picture that
// looks the same either way — is exactly how a substitution stops being noticed. The fallback is
// kept rather than deleted: it is what runs when the cooked mesh predates the format version, and
// deleting it would turn an old cache entry into a character-shaped explosion instead of a note.
//
// `derive_influences` below BINDS THE MESH ITSELF, from the cooked positions and the skeleton's
// bind pose, by distance to each bone's segment. Everything downstream of it is the engine's real
// path — the same `GpuSkinInfluence` layout `render::VertexStream::Skin` defines, the same compute
// dispatch, the same bone matrices out of the same `PoseWorld`.
//
// A derived bind is visibly worse than an artist's in exactly one way and it is worth naming: it
// has no notion of which limb a vertex belongs to, only which bone is nearest, so where two limbs
// are close in the bind pose — the inner thighs of a T-posed character — a few vertices take weight
// from the wrong leg and stretch between them. That was visible in the video at the hips during the
// run, and it is this substitution's fault rather than the skinning dispatch's — so when
// `imported` is true it should be gone.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/math/vec.h>
#include <cy/core/memory/allocator.h>
#include <cy/core/memory/array.h>
#include <cy/core/values/name.h>
#include <cy/import/mesh.h>
#include <cy/servers/render/geometry/skin_dispatch.h>
#include <cy/servers/render/mesh.h>

#include <cy/animation/clip.h>
#include <cy/animation/skeleton.h>

#include <string>
#include <vector>

namespace cy::sample::character {

/// Which of the four clips a source file supplies. The order is the order
/// `graph::pose::LocomotionState` numbers its states, so an index here is a state index there.
enum class Motion : u32 { Idle = 0, Walk, Run, Die, Count };

inline constexpr u32 kMotionCount = static_cast<u32>(Motion::Count);

/// `idle`, `walk`, `run`, `die` — the names `LocomotionSpec` refers to its clips by and therefore
/// the names `AnimationRig::bind` matches the clip table against.
[[nodiscard]] Name motion_name(Motion motion) noexcept;

/// Where the four source files live. Paths rather than bytes: each is 1–17 MB and lives OUTSIDE the
/// repository, because they are Mixamo exports under Mixamo's licence.
struct CharacterSources {
    /// The directory holding the four files.
    std::string directory;
    /// The file supplying each motion, indexed by `Motion`. The one that also carries the mesh and
    /// the skin is `mesh_from`.
    std::string files[kMotionCount];
    /// Which motion's file the character's mesh and rig are taken from. `Walking.fbx` is the only
    /// one of the four with a mesh at all.
    Motion mesh_from = Motion::Walk;
};

/// The default: the four files this artefact was written against, by their Mixamo names.
[[nodiscard]] CharacterSources default_sources(std::string_view directory) noexcept;

/// What importing one file produced, measured rather than assumed. Printed by the artefact so a
/// reader can tell an empty picture from an empty import.
struct SourceReport {
    std::string file;
    /// Sub-assets the importer produced, by kind.
    u32 meshes = 0;
    u32 materials = 0;
    u32 skeletons = 0;
    u32 animations = 0;
    u32 warnings = 0;
    /// The skeleton's joint count, and how many of the 22 standard humanoid slots it filled.
    u32 joints = 0;
    u32 humanoid_mapped = 0;
    /// The clip, before any retargeting.
    f32 duration = 0.0F;
    u32 tracks = 0;
    u32 keys = 0;
    /// True when this file's rig had to be retargeted onto the character's — which is every file
    /// but the one the mesh came from.
    bool retargeted = false;
    /// What `compare_rigs` measured between this file's rig and the character's, before the
    /// retarget reconciled them. Degrees and metres.
    f32 rest_difference_degrees = 0.0F;
    f32 rest_difference_metres = 0.0F;
    /// `RetargetBuildReport::pairs`, and the height scale the profile derived.
    u32 retarget_pairs = 0;
    f32 height_scale = 1.0F;
    /// The clip as the rig will play it, after the retarget bake and the codec.
    u32 final_tracks = 0;
    u32 final_keys = 0;
    f32 worst_rotation_degrees = 0.0F;
};

/// What binding the mesh to the rig produced.
struct SkinReport {
    u32 vertices = 0;
    u32 triangles = 0;
    /// True when the weights came from the FILE rather than from `derive_influences`. M11.b.
    ///
    /// The whole point of carrying it: a picture of a skinned character looks the same either way,
    /// so the only way a reader can tell an imported skin from a derived one is if the artefact
    /// says. It is printed in the report and asserted by the sample's own checks.
    bool imported = false;
    /// Bones the bind actually gave weight to. A number well below the joint count means whole
    /// limbs are rigid, which is a defect a picture would hide.
    u32 bones_used = 0;
    /// The worst distance from a vertex to the nearest bone segment, in metres. A large number
    /// means the mesh and the skeleton are not in the same space, which is the failure this
    /// artefact would otherwise show as a character-shaped explosion. Zero when the weights were
    /// imported: nothing measured a distance, because nothing needed to.
    f32 worst_bind_distance = 0.0F;
    /// The mesh's bounds and the rig's, so a reader can see for themselves that they agree.
    Vec3 mesh_min{0.0F, 0.0F, 0.0F};
    Vec3 mesh_max{0.0F, 0.0F, 0.0F};
    Vec3 rig_min{0.0F, 0.0F, 0.0F};
    Vec3 rig_max{0.0F, 0.0F, 0.0F};
};

/// One imported, retargeted, skinned character.
///
/// Holds the runtime objects rather than the cooked records: the records are read, converted and
/// dropped inside `load_character`, because nothing downstream of it wants a payload.
struct Character {
    explicit Character(Allocator& allocator) noexcept;

    Character(const Character&) = delete;
    Character& operator=(const Character&) = delete;

    Allocator* allocator = nullptr;

    animation::Skeleton skeleton;
    animation::SkeletonProfile humanoid;

    /// The four clips, indexed by `Motion`, each already bound to `skeleton`'s joint indices.
    std::vector<animation::Clip> clips;

    /// The cooked mesh, in the skeleton's bind-model space.
    Array<Vec3> positions;
    Array<render::PackedNormalTangent> frames;
    Array<u32> indices;
    /// One record per vertex: four bone indices and four weights, `VertexStream::Skin`'s layout.
    Array<render::geometry::GpuSkinInfluence> influences;
    /// The artist's bindings as the cooked mesh carried them, or empty when it carried none. Held
    /// between `read_mesh` and the bind because the conversion needs the skeleton's joint count and
    /// the skeleton is built after the mesh is read. M11.b.
    Array<import::SkinInfluence> imported_skin;

    SourceReport sources[kMotionCount];
    SkinReport skin;
};

/// Import the four files, build the rig, retarget three clips onto it, and bind the mesh.
///
/// Every failure is a returned error naming what could not be done; nothing here degrades quietly,
/// because an artefact whose picture is a character standing still must not be reachable by a
/// silently skipped step. A missing source file is the one exception the caller handles rather than
/// this function: it reports `NotFound` naming the path, and the artefact prints where it looked.
[[nodiscard]] Status load_character(Allocator& allocator, const CharacterSources& sources,
                                    Character& out) noexcept;

/// Bind `positions` to `skeleton` by distance to each bone's segment, filling `out`.
///
/// Exposed for the reason the header comment gives at length: this is the ONE substitution the
/// artefact makes for a missing pipeline step, and a substitution buried in a file's private
/// namespace is one a reader has to go looking for.
///
/// `influences_per_vertex` is four, which is `SkinningDescriptor::InfluenceCount::Four` and the
/// layout of one `GpuSkinInfluence`. The weights are normalised to sum to 255 exactly, because the
/// dispatch deliberately does not renormalise — `skin_dispatch.h` calls a stream that does not sum
/// to 255 the importer's defect to fix at cook time rather than a division the GPU performs per
/// vertex per frame forever.
[[nodiscard]] Status derive_influences(const animation::Skeleton& skeleton,
                                       Span<const Vec3> vertices,
                                       Array<render::geometry::GpuSkinInfluence>& out,
                                       SkinReport& report) noexcept;

/// Convert the importer's per-vertex bindings into the stream the dispatch reads. M11.b.
///
/// `import::SkinInfluence` holds 16-bit joint indices and float weights; `GpuSkinInfluence` holds
/// four bytes of each. THE QUANTISATION IS WHERE THE CARE IS: `skin_dispatch.h` states that the
/// dispatch does not renormalise, and that a vertex whose bytes do not sum to 255 is darker or
/// brighter than it should be. So the bytes are computed by largest-remainder — round every weight
/// down, then hand the leftover out to the largest remainders — which sums to exactly 255 for any
/// input that sums to one, rather than to 254 or 256 as four independent roundings do.
///
/// Fails with `OutOfRange` on a joint index past 255: the stream's index is one byte, and a rig
/// over that cap cannot be addressed by it. That is a real limit of `VertexStream::Skin` rather
/// than of this function, and it is refused here rather than silently truncated to a wrong bone.
[[nodiscard]] Status convert_influences(Span<const import::SkinInfluence> influences,
                                        u32 joint_count,
                                        Array<render::geometry::GpuSkinInfluence>& out,
                                        SkinReport& report) noexcept;

}  // namespace cy::sample::character
