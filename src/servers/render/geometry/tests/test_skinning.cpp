// Skinning descriptors, skinned bounds and blend shapes. M6 task 8.4.

#include <cy/core/memory/system_allocator.h>
#include <cy/servers/render/geometry/skinning.h>
#include <cy/test/test.h>

#include <vector>

using namespace cy::render::geometry;
using cy::Aabb;
using cy::f32;
using cy::Transform;
using cy::u32;
using cy::usize;
using cy::Vec3;

namespace {

cy::Allocator& allocator() noexcept {
    return cy::system_allocator(cy::MemoryDomain::Renderer);
}

SkinningDescriptor full_skin(u32 vertices, u32 bones) {
    SkinningDescriptor descriptor;
    descriptor.vertex_count = vertices;
    descriptor.bone_count = bones;
    descriptor.retained_bones = bones;
    return descriptor;
}

BlendShapeDelta delta(u32 vertex, f32 x) {
    BlendShapeDelta value;
    value.vertex = vertex;
    value.position[0] = x;
    return value;
}

}  // namespace

CY_TEST_CASE(
    "skinning: the GPU pose world is accepted, and the two ways of naming it wrongly are "
    "not") {
    // THIS CASE WAS THE REFUSAL. `rendering-geometry-and-resources` requires bone matrices to come
    // from the GPU pose world, which is `animation-and-skinning`'s; at M6 there was none, and
    // `validate()` refused the value naming M8 so that a second pose upload could not accumulate
    // consumers in the meantime. M8.b built the world — `cy::animation::PoseWorld` — so the
    // refusal is gone and what is asserted here is what a caller can still get wrong.
    SkinningDescriptor descriptor = full_skin(100, 8);
    CY_CHECK(descriptor.validate().has_value());

    descriptor.source = PoseSource::GpuPoseWorld;
    descriptor.pose_offset = 1024;
    CY_CHECK(descriptor.validate().has_value());

    // A baked instance has no skeleton in the world: it is a pose texture, and it skins nothing.
    SkinningDescriptor baked = descriptor;
    baked.tier = AnimationTier::Baked;
    baked.retained_bones = 0;
    const cy::Status refused_baked = baked.validate();
    CY_REQUIRE(!refused_baked.has_value());
    CY_CHECK(refused_baked.error().code == cy::ErrorCode::InvalidArgument);

    // An offset into the shared world on a descriptor that says it uploads its own pose would read
    // another instance's bones.
    SkinningDescriptor confused = descriptor;
    confused.source = PoseSource::UploadedPerSkin;
    const cy::Status refused_offset = confused.validate();
    CY_REQUIRE(!refused_offset.has_value());
    CY_CHECK(refused_offset.error().code == cy::ErrorCode::InvalidArgument);

    // And a range that wraps the address space.
    SkinningDescriptor wrapping = descriptor;
    wrapping.pose_offset = 0xFFFFFFFFU - 2;
    CY_CHECK(!wrapping.validate().has_value());
}

CY_TEST_CASE("skinning: a descriptor the compute pass cannot execute is refused, naming why") {
    CY_CHECK(!full_skin(0, 8).validate().has_value());
    CY_CHECK(!full_skin(100, 0).validate().has_value());

    SkinningDescriptor over = full_skin(100, 4);
    over.retained_bones = 8;
    CY_CHECK(!over.validate().has_value());

    // "instances at reduced bone LOD SHALL use mesh LODs whose influences reference only retained
    // joints": eight influences against four retained joints would index outside the pose.
    SkinningDescriptor reduced = full_skin(100, 32);
    reduced.tier = AnimationTier::ReducedBones;
    reduced.retained_bones = 4;
    reduced.influences = InfluenceCount::Eight;
    CY_CHECK(!reduced.validate().has_value());
    reduced.influences = InfluenceCount::Four;
    CY_CHECK(reduced.validate().has_value());

    // The full tier retains every bone by definition, so a descriptor that says otherwise is
    // describing two different things.
    SkinningDescriptor inconsistent = full_skin(100, 32);
    inconsistent.retained_bones = 30;
    CY_CHECK(!inconsistent.validate().has_value());
}

CY_TEST_CASE("skinning: a baked instance does not skin, and costs no output buffer") {
    // "instances at a baked tier SHALL use pose textures or vertex animation rather than skeletal
    // skinning".
    SkinningDescriptor baked = full_skin(1000, 16);
    baked.tier = AnimationTier::Baked;
    baked.retained_bones = 0;
    CY_CHECK(baked.validate().has_value());
    CY_CHECK(!baked.skins());
    CY_CHECK(baked.output_byte_size(true) == 0);
}

CY_TEST_CASE("skinning: the output buffer is double buffered, for motion vectors") {
    // "Output buffers SHALL be double buffered so the previous frame's positions are available for
    // motion vectors."
    const SkinningDescriptor descriptor = full_skin(1000, 16);
    const cy::u64 one_frame =
        static_cast<cy::u64>(skinned_vertex_bytes(true)) * descriptor.vertex_count;
    CY_CHECK(descriptor.output_byte_size(true) == one_frame * 2);
    // Positions only is cheaper, which is what a mesh with no normal-tangent frame costs.
    CY_CHECK(descriptor.output_byte_size(false) < descriptor.output_byte_size(true));
}

CY_TEST_CASE("skinning: the current and previous buffers swap by frame parity, not by a pointer") {
    // Several passes in one frame must agree about which buffer is current, and a swap that
    // happened between two of them would give the depth prepass one set of vertices and the opaque
    // pass another.
    const SkinnedBuffers even = SkinnedBuffers::for_frame(10);
    const SkinnedBuffers odd = SkinnedBuffers::for_frame(11);
    CY_CHECK(even.current == 0);
    CY_CHECK(even.previous == 1);
    CY_CHECK(odd.current == 1);
    CY_CHECK(odd.previous == 0);
    // Asking twice in one frame gives one answer, which is the property that matters.
    CY_CHECK(SkinnedBuffers::for_frame(10).current == even.current);
}

CY_TEST_CASE("skinning: bounds come from the bone transforms, not from the bind pose") {
    // "WHEN a skinned mesh animates THEN its bounds SHALL be computed from bone transforms and
    // per-bone bounds, not from the bind pose."
    std::vector<BoneBounds> bounds(2);
    bounds[0].local = Aabb::from_min_max(Vec3{-1, -1, -1}, Vec3{1, 1, 1});
    bounds[1].local = Aabb::from_min_max(Vec3{-1, -1, -1}, Vec3{1, 1, 1});

    std::vector<Transform> bind(2);
    const Aabb rest = skinned_bounds(cy::Span<const Transform>(bind.data(), bind.size()),
                                     cy::Span<const BoneBounds>(bounds.data(), bounds.size()));
    CY_CHECK(rest.max.x <= 1.001F);

    // Raise the second bone. The bounds must follow it.
    std::vector<Transform> posed(2);
    posed[1].translation = Vec3{0.0F, 10.0F, 0.0F};
    const Aabb animated = skinned_bounds(cy::Span<const Transform>(posed.data(), posed.size()),
                                         cy::Span<const BoneBounds>(bounds.data(), bounds.size()));
    CY_CHECK(animated.max.y > 10.0F);
    CY_CHECK(animated.min.y < 0.0F);
}

CY_TEST_CASE("skinning: a bone that influences nothing contributes nothing to the bounds") {
    // A skeleton carries attachment points, and inflating what the cull tests with them would make
    // every character's bounds the size of its weapon socket's reach.
    std::vector<BoneBounds> bounds(2);
    bounds[0].local = Aabb::from_min_max(Vec3{-1, -1, -1}, Vec3{1, 1, 1});
    // bounds[1] stays empty.
    std::vector<Transform> posed(2);
    posed[1].translation = Vec3{0.0F, 1000.0F, 0.0F};

    const Aabb animated = skinned_bounds(cy::Span<const Transform>(posed.data(), posed.size()),
                                         cy::Span<const BoneBounds>(bounds.data(), bounds.size()));
    CY_CHECK(animated.max.y < 2.0F);

    // Every bone empty answers an EMPTY box, which every intersection test rejects — "not visible",
    // and not "everywhere".
    std::vector<BoneBounds> none(2);
    const Aabb nothing = skinned_bounds(cy::Span<const Transform>(posed.data(), posed.size()),
                                        cy::Span<const BoneBounds>(none.data(), none.size()));
    CY_CHECK(nothing.is_empty());
}

CY_TEST_CASE("blend shapes: only the active shapes' deltas are read") {
    // "WHEN 50 blend shapes exist and 5 have non-zero weight THEN only the 5 active shapes' deltas
    // SHALL be read and applied."
    BlendShapeSet shapes(allocator());
    for (u32 shape = 0; shape < 50; ++shape) {
        const BlendShapeDelta deltas[3] = {delta(0, 1.0F), delta(1, 1.0F), delta(2, 1.0F)};
        CY_REQUIRE(shapes.add(cy::Span<const BlendShapeDelta>(deltas, 3)).has_value());
    }
    CY_CHECK(shapes.size() == 50);
    CY_CHECK(shapes.active_delta_count(1.0e-4F) == 0);

    for (const u32 shape : {3U, 9U, 12U, 40U, 49U}) {
        CY_REQUIRE(shapes.set_weight(shape, 0.5F).has_value());
    }
    u32 active[64] = {};
    CY_CHECK(shapes.active(1.0e-4F, cy::Span<u32>(active, 64)) == 5);
    CY_CHECK(active[0] == 3);
    CY_CHECK(active[4] == 49);
    CY_CHECK(shapes.active_delta_count(1.0e-4F) == 15);

    // A rig driven by a curve leaves a hundred shapes at 1e-7, and reading a hundred delta lists to
    // move nothing is what the threshold exists to avoid.
    CY_REQUIRE(shapes.set_weight(7, 1.0e-7F).has_value());
    CY_CHECK(shapes.active(1.0e-4F, cy::Span<u32>(active, 64)) == 5);
    CY_CHECK(shapes.active(0.0F, cy::Span<u32>(active, 64)) == 6);

    // A negative weight is a shape applied backwards, which is active.
    CY_REQUIRE(shapes.set_weight(20, -0.8F).has_value());
    CY_CHECK(shapes.active(1.0e-4F, cy::Span<u32>(active, 64)) == 6);
}

CY_TEST_CASE("blend shapes: storage is sparse, and an unsorted or duplicated list is refused") {
    BlendShapeSet shapes(allocator());
    // Sparse: a shape over a thousand-vertex mesh that moves two vertices stores two deltas.
    const BlendShapeDelta sparse[2] = {delta(17, 1.0F), delta(400, -1.0F)};
    const auto index = shapes.add(cy::Span<const BlendShapeDelta>(sparse, 2));
    CY_REQUIRE(index.has_value());
    CY_CHECK(shapes.deltas_of(index.value()).size() == 2);
    CY_CHECK(shapes.deltas_of(index.value())[1].vertex == 400);

    const BlendShapeDelta unsorted[2] = {delta(400, 1.0F), delta(17, 1.0F)};
    CY_CHECK(!shapes.add(cy::Span<const BlendShapeDelta>(unsorted, 2)).has_value());

    const BlendShapeDelta duplicated[2] = {delta(17, 1.0F), delta(17, 2.0F)};
    CY_CHECK(!shapes.add(cy::Span<const BlendShapeDelta>(duplicated, 2)).has_value());

    // The refusals left the set as it was.
    CY_CHECK(shapes.size() == 1);
}
