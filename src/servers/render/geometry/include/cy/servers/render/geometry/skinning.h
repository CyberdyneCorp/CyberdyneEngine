#pragma once
// Skinning and blend shapes, as the render server holds them. M6 task 8.4.
//
// `rendering-geometry-and-resources` — "Skinning": "Skinned meshes SHALL be transformed by a
// **compute pass** writing into per-instance output vertex buffers, so the result is reusable
// across passes (depth, shadows, main) without re-skinning", with "up to 4 or 8 influences per
// vertex (selectable per mesh) and **dual quaternion** skinning as an option", output buffers
// "double buffered so the previous frame's positions are available for motion vectors", and bounds
// "computed from bone transforms and per-bone bounds, not from the bind pose".
//
// ================================================================================================
// THE ONE REQUIREMENT THIS MODULE DID NOT MEET, AND THE MILESTONE THAT CLOSED IT
// ================================================================================================
//
// "Bone matrices SHALL be read from the **GPU pose world** (see `animation-and-skinning`), the
// shared GPU-side pose representation, rather than from a buffer uploaded independently per
// consumer."
//
// At M6 there was no pose world, so this module could not read one — and it must not invent a
// second, because inventing one is precisely the failure the requirement exists to prevent. What
// was written instead was the SEAM: `PoseSource` is where a pose comes from,
// `PoseSource::GpuPoseWorld` was declared and refused by `validate()` naming M8, and
// `SkinningDescriptor::pose_offset` was the offset into a world that did not exist yet.
//
// **M8.b built it.** `cy::animation::PoseWorld` (src/animation/include/cy/animation/pose_world.h)
// is the shared representation: current and previous bone matrices per instance, instances added
// and removed without a rebuild, and a dirty range for the renderer to transfer.
// `PoseWorld::matrix_offset(handle)` is what `pose_offset` holds, and `validate()` accepts the
// source. What it still refuses is a descriptor that cannot mean anything — a BAKED instance
// claiming a place in the world it has no skeleton in, and a per-skin upload carrying an offset
// that belongs to the shared world.
//
// The seam did its job: nothing had to be migrated, because no second upload path ever accumulated
// a consumer. src/animation/tests/test_pose_world.cpp builds a descriptor from a real pose world's
// offset and validates it, so the join is asserted rather than assumed on both sides.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/math/shapes.h>
#include <cy/core/math/transform.h>
#include <cy/core/memory/allocator.h>
#include <cy/core/memory/array.h>
#include <cy/servers/render/mesh.h>
#include <cy/servers/render/types.h>

#include <cstddef>

namespace cy::render::geometry {

/// How many bones may influence one vertex. "up to 4 or 8 influences per vertex (selectable per
/// mesh)".
enum class InfluenceCount : u8 {
    Four = 4,
    Eight = 8,
};

/// Which blend the compute pass performs.
///
/// Linear blend skinning is the default and the cheap one; dual quaternion avoids the candy-wrapper
/// collapse a twisted joint produces and costs about a third more. It is per mesh because it is a
/// property of how the mesh was WEIGHTED, not of the machine it runs on.
enum class SkinningMethod : u8 {
    LinearBlend = 0,
    DualQuaternion = 1,
};

/// Where the bone matrices come from. See the header comment for how the second value arrived.
enum class PoseSource : u8 {
    /// A buffer this skin uploads for itself. The M6 path, kept for a skin whose pose is not in the
    /// shared world — a preview, a tool, a test — and the reason `pose_offset` must be zero for it.
    UploadedPerSkin = 0,
    /// `cy::animation::PoseWorld`, the shared GPU-side pose representation. Declared at M6, refused
    /// until the world existed, and accepted since M8.b. `pose_offset` is
    /// `PoseWorld::matrix_offset(handle)`, which moves each time the instance publishes because the
    /// world is double buffered.
    GpuPoseWorld = 1,
};

/// `animation-and-skinning`'s tier, as far as skinning has to care.
///
/// "Skinning SHALL respect the instance's animation LOD tier: instances at a baked tier SHALL use
/// pose textures or vertex animation rather than skeletal skinning, and instances at reduced bone
/// LOD SHALL use mesh LODs whose influences reference only retained joints."
enum class AnimationTier : u8 {
    /// Full skeletal evaluation and full skinning.
    Full = 0,
    /// A reduced bone set. The mesh level chosen must reference only retained joints, which
    /// `SkinningDescriptor::retained_bones` records and `validate()` checks the influence bound
    /// against.
    ReducedBones = 1,
    /// A pose texture or vertex animation. The compute pass does not run at all.
    Baked = 2,
};

/// Bytes one output vertex occupies: the skinned position, and the normal-and-tangent frame when
/// the mesh has one.
///
/// A free function rather than a member of the descriptor because it depends on nothing a
/// descriptor holds. The output layout is the renderer's and is the same for every skinned mesh; a
/// member would suggest a mesh could choose it.
[[nodiscard]] u32 skinned_vertex_bytes(bool with_normals) noexcept;

/// What one skinned mesh needs from the compute pass.
///
/// A DESCRIPTION and not a resource: there is no buffer handle here, because layer 2 may not name
/// one. `src/rendering/` allocates the output buffers this describes and dispatches over them.
struct SkinningDescriptor {
    /// The mesh being skinned, as the render server's slot.
    u32 mesh = 0;
    u32 vertex_count = 0;
    u32 bone_count = 0;
    /// How many bones survive the instance's animation tier. Equal to `bone_count` at
    /// `AnimationTier::Full`.
    u32 retained_bones = 0;
    /// Where this skin's pose begins in the pose storage `source` names.
    u32 pose_offset = 0;
    InfluenceCount influences = InfluenceCount::Four;
    SkinningMethod method = SkinningMethod::LinearBlend;
    PoseSource source = PoseSource::UploadedPerSkin;
    AnimationTier tier = AnimationTier::Full;
    /// How many blend shapes the mesh carries, of which only the non-zero-weighted are read.
    u32 blend_shape_count = 0;

    /// Refuse a descriptor the compute pass cannot execute, naming why.
    ///
    /// The list is short and every entry is a bug that would otherwise appear as corrupt geometry:
    /// a pose source this build has no pose world for, a retained-bone count above the bone count,
    /// an eight-influence mesh whose retained set is smaller than its influences can address.
    [[nodiscard]] Status validate() const noexcept;

    /// Device bytes the double-buffered output costs. "Output buffers SHALL be double buffered so
    /// the previous frame's positions are available for motion vectors", so this is twice one
    /// frame's — which is the number a memory report charges and the number a budget is checked
    /// against, and computing it in two places is how a buffer ends up half the size it needs.
    [[nodiscard]] u64 output_byte_size(bool with_normals) const noexcept;

    /// Whether the compute pass runs at all for this instance.
    [[nodiscard]] bool skins() const noexcept { return tier != AnimationTier::Baked; }
};

/// Which of a skin's two output buffers a pass reads.
///
/// The pair is indexed by the frame's parity rather than by a pointer swap, because several passes
/// in one frame must agree about which is current and a swap that happened between two of them
/// would give the depth prepass one set of vertices and the opaque pass another.
struct SkinnedBuffers {
    /// The buffer this frame's compute pass writes and this frame's passes read. 0 or 1.
    u32 current = 0;
    /// The other one, holding the previous frame's positions. What motion vectors read.
    u32 previous = 1;

    [[nodiscard]] static constexpr SkinnedBuffers for_frame(u64 frame_index) noexcept {
        const u32 parity = static_cast<u32>(frame_index & 1U);
        return SkinnedBuffers{parity, parity ^ 1U};
    }
};

/// One bone's contribution to the skinned bounds.
///
/// "WHEN a skinned mesh animates THEN its bounds SHALL be computed from bone transforms and
/// per-bone bounds, not from the bind pose." So a bone carries the box of the vertices it
/// influences, in the bone's own space, and the animated bounds are the union of those boxes
/// transformed by the pose.
struct BoneBounds {
    Aabb local = Aabb::empty();
};

/// The animated bounds of a skinned mesh.
///
/// `poses` and `bounds` are parallel and indexed by bone. A bone with empty bounds — one that
/// influences no vertex — contributes nothing, which is what lets a skeleton carry attachment
/// points without inflating what the cull tests.
///
/// Returns an empty box when every bone is empty, which a caller must treat as "not visible" rather
/// than as "everywhere": an inverted box is `Aabb`'s own empty state and every intersection test
/// answers false for it.
[[nodiscard]] Aabb skinned_bounds(Span<const Transform> poses,
                                  Span<const BoneBounds> bounds) noexcept;

// --- Blend shapes -------------------------------------------------------------------------------

/// One vertex a blend shape moves.
///
/// `rendering-geometry-and-resources` — "Blend shapes": "stored as sparse deltas of position,
/// normal, and tangent ... Sparse storage SHALL record only vertices a shape actually moves."
///
/// 40 bytes: the vertex index and nine floats. Deliberately not compressed — a facial rig's shapes
/// are read by the same compute pass as skinning and the arithmetic is the cost, not the bandwidth,
/// and a half-float delta would need an unpack per lane per active shape.
struct BlendShapeDelta {
    u32 vertex = 0;
    f32 position[3] = {0.0F, 0.0F, 0.0F};
    f32 normal[3] = {0.0F, 0.0F, 0.0F};
    f32 tangent[3] = {0.0F, 0.0F, 0.0F};
};

static_assert(sizeof(BlendShapeDelta) == 40, "BlendShapeDelta is a 40-byte shader-visible record");

/// Where one shape's deltas are in the shared delta array.
struct BlendShapeRange {
    u32 first = 0;
    u32 count = 0;
};

/// A mesh's shapes, and the weights currently applied to them.
///
/// "WHEN 50 blend shapes exist and 5 have non-zero weight THEN only the 5 active shapes' deltas
/// SHALL be read and applied." `active()` is what makes that true: it compacts the shapes whose
/// weight exceeds a threshold into a list the dispatch iterates, so the cost is the active count
/// and not the authored one.
class BlendShapeSet {
public:
    explicit BlendShapeSet(Allocator& allocator) noexcept;

    BlendShapeSet(const BlendShapeSet&) = delete;
    BlendShapeSet& operator=(const BlendShapeSet&) = delete;

    /// Add a shape from its sparse deltas. Returns the shape's index.
    ///
    /// Refuses a delta list that is not sorted by vertex index, because the dispatch reads them in
    /// order and an unsorted list turns a streaming read into a scatter — and because a duplicate
    /// vertex within one shape means two authored deltas for one vertex, which is a content error
    /// no weight can resolve.
    [[nodiscard]] Expected<u32, Error> add(Span<const BlendShapeDelta> deltas) noexcept;

    [[nodiscard]] u32 size() const noexcept { return static_cast<u32>(ranges_.size()); }
    [[nodiscard]] Span<const BlendShapeDelta> deltas_of(u32 shape) const noexcept;
    [[nodiscard]] Span<const BlendShapeDelta> deltas() const noexcept { return deltas_.span(); }
    [[nodiscard]] Span<const BlendShapeRange> ranges() const noexcept { return ranges_.span(); }

    /// Set one shape's weight.
    [[nodiscard]] Status set_weight(u32 shape, f32 weight) noexcept;
    [[nodiscard]] f32 weight(u32 shape) const noexcept;

    /// The shapes whose weight exceeds `threshold`, written into `out` as indices, most-recently
    /// set order preserved by ascending shape index. Returns how many were written.
    ///
    /// A threshold rather than a comparison against zero: a rig driven by a curve leaves a hundred
    /// shapes at 1e-7, and reading a hundred delta lists to move nothing is the cost this function
    /// exists to avoid.
    [[nodiscard]] u32 active(f32 threshold, Span<u32> out) const noexcept;

    /// Deltas the dispatch would read for the current weights. What a statistics view reports and
    /// what a budget is checked against.
    [[nodiscard]] usize active_delta_count(f32 threshold) const noexcept;

    void clear() noexcept;

private:
    Array<BlendShapeDelta> deltas_;
    Array<BlendShapeRange> ranges_;
    Array<f32> weights_;
};

}  // namespace cy::render::geometry
