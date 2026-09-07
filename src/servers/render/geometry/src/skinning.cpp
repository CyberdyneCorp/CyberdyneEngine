#include <cy/servers/render/geometry/skinning.h>

#include <cy/core/math/matrix.h>

namespace cy::render::geometry {

Status SkinningDescriptor::validate() const noexcept {
    if (source == PoseSource::GpuPoseWorld) {
        // See the header comment. Accepting this quietly would let a second pose upload accumulate
        // consumers before the shared one exists, and undoing that would be a migration.
        return fail(
            ErrorCode::NotImplemented,
            "the GPU pose world belongs to animation-and-skinning, which reaches Working at "
            "M8; until then a skin uploads its own pose and says so");
    }
    if (vertex_count == 0) {
        return fail(ErrorCode::InvalidArgument, "a skin with no vertices skins nothing");
    }
    if (bone_count == 0) {
        return fail(ErrorCode::InvalidArgument, "a skin with no bones is not a skin");
    }
    if (retained_bones > bone_count) {
        return fail(ErrorCode::InvalidArgument,
                    "an animation tier cannot retain more bones than the skeleton has");
    }
    if (tier == AnimationTier::Full && retained_bones != bone_count) {
        return fail(ErrorCode::InvalidArgument,
                    "the full animation tier retains every bone, by definition");
    }
    if (retained_bones == 0 && tier != AnimationTier::Baked) {
        return fail(ErrorCode::InvalidArgument,
                    "a tier that retains no bone must be the baked tier, which does not skin");
    }
    if (tier == AnimationTier::ReducedBones && retained_bones < static_cast<u32>(influences)) {
        // "instances at reduced bone LOD SHALL use mesh LODs whose influences reference only
        // retained joints" — a mesh level that can address eight joints against a retained set of
        // four would index outside the pose.
        return fail(ErrorCode::InvalidArgument,
                    "this level of detail addresses more joints than the animation tier retains; "
                    "select a level whose influences reference only retained joints");
    }
    return ok();
}

u32 skinned_vertex_bytes(bool with_normals) noexcept {
    // A skinned position is full precision: quantisation is relative to a surface's bind-pose
    // bounding box (`render::quantise_position`) and an animated vertex leaves it.
    u32 bytes = vertex_stream_byte_size(VertexStream::Position, false);
    if (with_normals) {
        bytes += vertex_stream_byte_size(VertexStream::NormalTangent, false);
    }
    return bytes;
}

u64 SkinningDescriptor::output_byte_size(bool with_normals) const noexcept {
    if (!skins()) {
        return 0;
    }
    return static_cast<u64>(skinned_vertex_bytes(with_normals)) * vertex_count * 2U;
}

Aabb skinned_bounds(Span<const Transform> poses, Span<const BoneBounds> bounds) noexcept {
    Aabb total = Aabb::empty();
    const usize count = poses.size() < bounds.size() ? poses.size() : bounds.size();
    for (usize bone = 0; bone < count; ++bone) {
        if (bounds[bone].local.is_empty()) {
            continue;
        }
        // The transformed box's bounds, not the transform of the bounds: a rotated box is no longer
        // axis-aligned, and `transformed` is the absolute-value form that is conservative in the
        // right direction.
        total.grow(transformed(bounds[bone].local, poses[bone].to_matrix()));
    }
    return total;
}

// --- Blend shapes -------------------------------------------------------------------------------

BlendShapeSet::BlendShapeSet(Allocator& allocator) noexcept
    : deltas_(allocator), ranges_(allocator), weights_(allocator) {}

Expected<u32, Error> BlendShapeSet::add(Span<const BlendShapeDelta> deltas) noexcept {
    for (usize index = 1; index < deltas.size(); ++index) {
        if (deltas[index].vertex <= deltas[index - 1].vertex) {
            return make_unexpected(Error{
                ErrorCode::InvalidArgument,
                "a blend shape's deltas must be sorted by vertex index and hold each vertex "
                "once: the dispatch reads them in order, and a duplicate is a content error"});
        }
    }
    BlendShapeRange range;
    range.first = static_cast<u32>(deltas_.size());
    range.count = static_cast<u32>(deltas.size());
    if (Status appended = deltas_.append(deltas); !appended) {
        return make_unexpected(appended.error());
    }
    if (Status pushed = ranges_.push_back(range); !pushed) {
        return make_unexpected(pushed.error());
    }
    if (Status pushed = weights_.push_back(0.0F); !pushed) {
        return make_unexpected(pushed.error());
    }
    return static_cast<u32>(ranges_.size() - 1);
}

Span<const BlendShapeDelta> BlendShapeSet::deltas_of(u32 shape) const noexcept {
    if (shape >= ranges_.size()) {
        return {};
    }
    const BlendShapeRange& range = ranges_[shape];
    return {deltas_.data() + range.first, range.count};
}

Status BlendShapeSet::set_weight(u32 shape, f32 weight) noexcept {
    if (shape >= weights_.size()) {
        return fail(ErrorCode::OutOfRange, "no such blend shape");
    }
    weights_[shape] = weight;
    return ok();
}

f32 BlendShapeSet::weight(u32 shape) const noexcept {
    return shape < weights_.size() ? weights_[shape] : 0.0F;
}

u32 BlendShapeSet::active(f32 threshold, Span<u32> out) const noexcept {
    u32 written = 0;
    for (u32 shape = 0; shape < weights_.size(); ++shape) {
        const f32 magnitude = weights_[shape] < 0.0F ? -weights_[shape] : weights_[shape];
        if (magnitude <= threshold) {
            continue;
        }
        if (written >= out.size()) {
            break;
        }
        out[written++] = shape;
    }
    return written;
}

usize BlendShapeSet::active_delta_count(f32 threshold) const noexcept {
    usize total = 0;
    for (u32 shape = 0; shape < weights_.size(); ++shape) {
        const f32 magnitude = weights_[shape] < 0.0F ? -weights_[shape] : weights_[shape];
        if (magnitude > threshold) {
            total += ranges_[shape].count;
        }
    }
    return total;
}

void BlendShapeSet::clear() noexcept {
    deltas_.clear();
    ranges_.clear();
    weights_.clear();
}

}  // namespace cy::render::geometry
