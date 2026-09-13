// The GPU scene as a publication interface. Task 4.1.4.
//
// The cases below are grouped by the scenario in `rendering-architecture` they answer, and three of
// them exist only because of producers that do not arrive until M7 — GPU-side publication, per
// producer retirement, and the one-record-for-every-origin rule. Testing those now is the whole
// point of design.md §4: the second producer is the one that finds the interface wrong, and the
// cheapest moment to be wrong is before it exists.

#include <cy/test/test.h>

#include <cy/core/memory/system_allocator.h>
#include <cy/rendering/scene/gpu_scene.h>

namespace {

using cy::Aabb;
using cy::Mat4;
using cy::MemoryDomain;
using cy::Vec3;
using cy::rendering::AffineTransform3x4;
using cy::rendering::GpuInstance;
using cy::rendering::GpuScene;
using cy::rendering::InstanceFlags;
using cy::rendering::InstanceRange;
using cy::rendering::ProducerKind;
using cy::rendering::PublicationSite;
using cy::rendering::RenderImportance;

cy::Allocator& allocator() noexcept {
    return cy::system_allocator(MemoryDomain::Renderer);
}

GpuInstance instance_at(Vec3 position) noexcept {
    GpuInstance instance;
    instance.transform = AffineTransform3x4::from_mat4(Mat4::from_translation(position));
    instance.bounds = Aabb::from_center_extents(position, Vec3{1.0f, 1.0f, 1.0f});
    return instance;
}

}  // namespace

CY_TEST_CASE("gpu scene: an affine transform round trips through a Mat4") {
    const Mat4 source =
        Mat4::from_trs(Vec3{3.0f, -4.0f, 5.0f}, cy::Quat::identity(), Vec3{2.0f, 2.0f, 2.0f});
    const Mat4 restored = AffineTransform3x4::from_mat4(source).to_mat4();
    for (cy::usize row = 0; row < 4; ++row) {
        for (cy::usize column = 0; column < 4; ++column) {
            CY_CHECK_EQ(restored.at(row, column), source.at(row, column));
        }
    }
}

CY_TEST_CASE("gpu scene: a producer reserves a range and fills it") {
    GpuScene scene(allocator());
    const auto producer =
        scene.register_producer(ProducerKind::Extract, "extract", PublicationSite::Cpu);
    CY_REQUIRE(producer.has_value());

    const auto range = scene.reserve(*producer, 3);
    CY_REQUIRE(range.has_value());
    CY_CHECK_EQ(range->count, 3U);

    const GpuInstance published[3] = {instance_at(Vec3{0.0f, 0.0f, 0.0f}),
                                      instance_at(Vec3{1.0f, 0.0f, 0.0f}),
                                      instance_at(Vec3{2.0f, 0.0f, 0.0f})};
    CY_REQUIRE(scene.write_instances(*producer, *range, cy::Span<const GpuInstance>(published, 3))
                   .has_value());

    CY_CHECK_EQ(scene.live_instances(), 3U);
    const GpuInstance* second = scene.instance(range->first + 1);
    CY_REQUIRE(second != nullptr);
    CY_CHECK(second->live);
    CY_CHECK_EQ(second->transform.m[3], 1.0f);
}

CY_TEST_CASE("gpu scene: the previous transform shifts once per frame, not once per write") {
    GpuScene scene(allocator());
    const auto producer = scene.register_producer(ProducerKind::Vfx, "vfx", PublicationSite::Cpu);
    CY_REQUIRE(producer.has_value());
    const auto range = scene.reserve(*producer, 1);
    CY_REQUIRE(range.has_value());

    // A slot's first publication has no history: previous equals current, so a newly spawned
    // instance does not smear across the screen in its first frame.
    GpuInstance instance = instance_at(Vec3{0.0f, 0.0f, 0.0f});
    CY_REQUIRE(scene.write_instances(*producer, *range, cy::Span<const GpuInstance>(&instance, 1))
                   .has_value());
    CY_CHECK_EQ(scene.instance(range->first)->previous_transform.m[3], 0.0f);

    scene.begin_frame();
    instance = instance_at(Vec3{10.0f, 0.0f, 0.0f});
    CY_REQUIRE(scene.write_instances(*producer, *range, cy::Span<const GpuInstance>(&instance, 1))
                   .has_value());
    CY_CHECK_EQ(scene.instance(range->first)->transform.m[3], 10.0f);
    CY_CHECK_EQ(scene.instance(range->first)->previous_transform.m[3], 0.0f);

    // The second write in the same frame is the case a mesh-particle producer hits: it must not
    // report the delta between its own two writes as motion.
    instance = instance_at(Vec3{20.0f, 0.0f, 0.0f});
    CY_REQUIRE(scene.write_instances(*producer, *range, cy::Span<const GpuInstance>(&instance, 1))
                   .has_value());
    CY_CHECK_EQ(scene.instance(range->first)->transform.m[3], 20.0f);
    CY_CHECK_EQ(scene.instance(range->first)->previous_transform.m[3], 0.0f);
}

CY_TEST_CASE("gpu scene: a GPU-site producer declares bounds instead of writing instances") {
    GpuScene scene(allocator());
    const auto gpu =
        scene.register_producer(ProducerKind::VirtualGeometry, "clusters", PublicationSite::Gpu);
    CY_REQUIRE(gpu.has_value());
    const auto range = scene.reserve(*gpu, 4);
    CY_REQUIRE(range.has_value());

    // The scenario "GPU-side publication": no readback, no per-instance CPU submission. What the
    // CPU keeps is the conservative bound culling needs, and nothing else.
    const Aabb bounds = Aabb::from_center_extents(Vec3{}, Vec3{50.0f, 50.0f, 50.0f});
    CY_REQUIRE(scene.declare_gpu_written(*gpu, *range, bounds).has_value());
    CY_CHECK_EQ(scene.live_instances(), 4U);
    CY_CHECK(
        cy::rendering::has_flag(scene.instance(range->first)->flags, InstanceFlags::GpuAuthored));

    // And the CPU route is refused rather than being a slow path someone reaches for by accident.
    const GpuInstance one = instance_at(Vec3{});
    CY_CHECK_FALSE(
        scene.write_instances(*gpu, *range, cy::Span<const GpuInstance>(&one, 1)).has_value());

    // A GPU-authored range is deliberately not dirty: a CPU transfer over those slots would
    // overwrite what the dispatch produced.
    CY_CHECK(scene.dirty_ranges().empty());
}

CY_TEST_CASE("gpu scene: retiring a producer frees its slots and disturbs no other") {
    GpuScene scene(allocator());
    const auto extract =
        scene.register_producer(ProducerKind::Extract, "extract", PublicationSite::Cpu);
    const auto foliage =
        scene.register_producer(ProducerKind::Foliage, "foliage", PublicationSite::Cpu);
    CY_REQUIRE(extract.has_value());
    CY_REQUIRE(foliage.has_value());

    const auto extract_range = scene.reserve(*extract, 2);
    const auto foliage_range = scene.reserve(*foliage, 3);
    CY_REQUIRE(extract_range.has_value());
    CY_REQUIRE(foliage_range.has_value());

    const GpuInstance keep[2] = {instance_at(Vec3{1.0f, 0.0f, 0.0f}),
                                 instance_at(Vec3{2.0f, 0.0f, 0.0f})};
    const GpuInstance drop[3] = {instance_at(Vec3{}), instance_at(Vec3{}), instance_at(Vec3{})};
    CY_REQUIRE(scene.write_instances(*extract, *extract_range, cy::Span<const GpuInstance>(keep, 2))
                   .has_value());
    CY_REQUIRE(scene.write_instances(*foliage, *foliage_range, cy::Span<const GpuInstance>(drop, 3))
                   .has_value());
    CY_CHECK_EQ(scene.live_instances(), 5U);

    CY_REQUIRE(scene.retire_producer(*foliage).has_value());
    CY_CHECK_EQ(scene.live_instances(), 2U);
    CY_CHECK_EQ(scene.free_slots(), 3U);

    // "Producer removed ... without requiring a full rebuild": the other producer's slots hold
    // exactly what they held, at the indices they held it at.
    CY_CHECK_EQ(scene.instance(extract_range->first)->transform.m[3], 1.0f);
    CY_CHECK_EQ(scene.instance(extract_range->first + 1)->transform.m[3], 2.0f);
    CY_CHECK(scene.instance(extract_range->first)->live);

    // And a stale producer handle is answered, not obeyed.
    CY_CHECK_FALSE(scene.reserve(*foliage, 1).has_value());
}

CY_TEST_CASE("gpu scene: released ranges coalesce and are reused") {
    GpuScene scene(allocator());
    const auto producer =
        scene.register_producer(ProducerKind::InstancedMesh, "grass", PublicationSite::Cpu);
    CY_REQUIRE(producer.has_value());

    const auto first = scene.reserve(*producer, 4);
    const auto second = scene.reserve(*producer, 4);
    CY_REQUIRE(first.has_value());
    CY_REQUIRE(second.has_value());
    CY_CHECK_EQ(second->first, first->end());

    const cy::u32 before = scene.slot_capacity();
    CY_REQUIRE(scene.release(*producer, *first).has_value());
    CY_REQUIRE(scene.release(*producer, *second).has_value());

    // Two adjacent releases became one block, so an eight-slot reservation fits without growing.
    const auto reused = scene.reserve(*producer, 8);
    CY_REQUIRE(reused.has_value());
    CY_CHECK_EQ(reused->first, first->first);
    CY_CHECK_EQ(scene.slot_capacity(), before);
}

CY_TEST_CASE("gpu scene: a range not wholly owned is refused") {
    GpuScene scene(allocator());
    const auto a = scene.register_producer(ProducerKind::Extract, "a", PublicationSite::Cpu);
    const auto b = scene.register_producer(ProducerKind::Ui, "b", PublicationSite::Cpu);
    CY_REQUIRE(a.has_value());
    CY_REQUIRE(b.has_value());
    const auto range_a = scene.reserve(*a, 2);
    const auto range_b = scene.reserve(*b, 2);
    CY_REQUIRE(range_a.has_value());
    CY_REQUIRE(range_b.has_value());

    const InstanceRange straddling{range_a->first, range_a->count + range_b->count};
    const GpuInstance filler[4] = {instance_at(Vec3{}), instance_at(Vec3{}), instance_at(Vec3{}),
                                   instance_at(Vec3{})};
    CY_CHECK_FALSE(
        scene.write_instances(*a, straddling, cy::Span<const GpuInstance>(filler, 4)).has_value());
    CY_CHECK_FALSE(scene.release(*a, straddling).has_value());
}

CY_TEST_CASE("gpu scene: dirty ranges coalesce and describe what a transfer must move") {
    GpuScene scene(allocator());
    const auto producer =
        scene.register_producer(ProducerKind::Extract, "extract", PublicationSite::Cpu);
    CY_REQUIRE(producer.has_value());
    const auto range = scene.reserve(*producer, 4);
    CY_REQUIRE(range.has_value());
    scene.clear_dirty();

    const GpuInstance two[2] = {instance_at(Vec3{}), instance_at(Vec3{})};
    const InstanceRange low{range->first, 2};
    const InstanceRange high{range->first + 2, 2};
    CY_REQUIRE(
        scene.write_instances(*producer, low, cy::Span<const GpuInstance>(two, 2)).has_value());
    CY_REQUIRE(
        scene.write_instances(*producer, high, cy::Span<const GpuInstance>(two, 2)).has_value());

    CY_REQUIRE_EQ(scene.dirty_ranges().size(), 1U);
    CY_CHECK_EQ(scene.dirty_ranges()[0].first, range->first);
    CY_CHECK_EQ(scene.dirty_ranges()[0].count, 4U);

    scene.clear_dirty();
    CY_CHECK(scene.dirty_ranges().empty());
}

CY_TEST_CASE("gpu scene: render importance clamps rather than trusting its producer") {
    CY_CHECK_EQ(RenderImportance::clamped(-3.0f).value, 0.0f);
    CY_CHECK_EQ(RenderImportance::clamped(0.25f).value, 0.25f);
    CY_CHECK_EQ(RenderImportance::clamped(9.0f).value, 1.0f);
}
