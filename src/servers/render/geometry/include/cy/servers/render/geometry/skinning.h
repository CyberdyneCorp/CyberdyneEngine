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
// THE ONE REQUIREMENT THIS MODULE DOES NOT MEET, STATED RATHER THAN QUIETLY MET
// ================================================================================================
//
// "Bone matrices SHALL be read from the **GPU pose world** (see `animation-and-skinning`), the
// shared GPU-side pose representation, rather than from a buffer uploaded independently per
// consumer."
//
// `animation-and-skinning` reaches Working at M8. There is no GPU pose world at M6, so this module
// cannot read one, and it must not invent a second: inventing one is precisely the failure the
// requirement exists to prevent — "WHEN skinning, motion vector generation, and VFX attachment all
// need bone transforms THEN all SHALL read the GPU pose world rather than each maintaining its own
// upload".
//
// What is here instead is the SEAM, and it is named so that M8 fills it rather than works around
// it. `PoseSource` is where a pose comes from; `PoseSource::GpuPoseWorld` is declared and
// `SkinningDescriptor::pose_offset` is the offset into it. Until M8, `PoseSource::UploadedPerSkin`
// is the only value a caller may use, and `validate()` REFUSES `GpuPoseWorld` naming the milestone.
// A build that quietly accepted it would let a second pose upload accumulate consumers, and undoing
// that at M8 would be a migration rather than an edit.
//
// The capability matrix's own rule — a capability may not reach Complete before its prerequisites
// reach Working — is what this note is evidence for. M6 task 10.8 asks the gate to resolve it.

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

/// Where the bone matrices come from. See the header comment: only one value is usable at M6.
enum class PoseSource : u8 {
    /// A buffer this skin uploads for itself. The M6 path, and the one the GPU pose world replaces.
    UploadedPerSkin = 0,
    /// `animation-and-skinning`'s shared GPU-side pose representation. Declared at M6 and REFUSED
    /// by
    /// `validate()` until that capability reaches Working at M8.
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
