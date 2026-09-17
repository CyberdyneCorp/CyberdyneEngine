// The parallel cull: the second half of `rendering-culling-and-lod`'s "Frustum culling". M11.c.
//
// "Culling SHALL be parallelised across job workers above a configurable instance-count threshold,
// each worker producing a local visible list merged by pointer transfer", with the scenario
// "WHEN culling runs on eight workers THEN merging SHALL transfer page or range ownership rather
// than copying instance data".
//
// ================================================================================================
// WHY THIS IS ITS OWN SUITE AND NOT A CASE IN test_cull.cpp
// ================================================================================================
//
// A unit case is budgeted at 1 ms of its own CPU time, and starting eight worker threads costs more
// than that before a single instance is tested — in CPU time across those threads, which is what
// `cy::test::BudgetGuard` measures. The subject here is the same cull the unit suite covers, run
// over a real job system, so it is integration by cost rather than by depth.
//
// ================================================================================================
// WHAT THE CASE CAN AND CANNOT OBSERVE
// ================================================================================================
//
// "Merged by pointer transfer" is a statement about what the merge does NOT do — copy per-instance
// payloads across a thread boundary — and cull.h's own header says so plainly: each partition fills
// an `Array<VisibleInstance>` of 40-byte visibility records, and the merge appends those arrays
// into the typed lists in PARTITION ORDER. What is observable from outside, and what this case
// asserts, is the property that order buys: the parallel run and the serial run produce the same
// lists in the same order, element for element. An implementation that merged in completion order
// would pass the set comparison and fail this one, which is the regression the ordering rule exists
// to prevent.

#include <cy/test/test.h>

#include <cy/core/jobs/job_system.h>
#include <cy/core/math/projection.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/rendering/culling/cull.h>

namespace {

using cy::rendering::CullOptions;
using cy::rendering::CullResults;
using cy::rendering::CullView;
using cy::rendering::CullWorkspace;
using cy::rendering::SpatialEntry;
using cy::rendering::SpatialIndex;
using cy::rendering::VisibleInstance;

cy::Allocator& allocator() noexcept {
    return cy::system_allocator(cy::MemoryDomain::Renderer);
}

CullView make_view() noexcept {
    CullView view;
    const cy::Mat4 projection = cy::perspective_reversed_z(1.0471975512F, 1.0F, 0.1F, 1000.0F);
    const cy::Mat4 camera = cy::look_at(cy::Vec3{0.0F, 0.0F, 0.0F}, cy::Vec3{0.0F, 0.0F, -1.0F});
    view.frustum = cy::Frustum::from_view_projection(projection * camera);
    view.camera_position = cy::Vec3{0.0F, 0.0F, 0.0F};
    view.camera_forward = cy::Vec3{0.0F, 0.0F, -1.0F};
    view.lod.hysteresis = 0.0F;
    return view;
}

/// A job system for the duration of a scope. One running system per process is the rule
/// `core-jobs-and-concurrency` sets, and doctest runs cases one at a time.
class ScopedJobSystem {
public:
    explicit ScopedJobSystem(cy::u32 workers) noexcept {
        cy::jobs::JobSystemConfig config;
        config.worker_count = workers;
        config.task_slots_per_participant = 2048;
        config.deque_capacity = 2048;
        config.scratch_bytes_per_participant = cy::usize{256} * 1024;
        started_ = system_.start(config).has_value();
    }
    ~ScopedJobSystem() { system_.shutdown(); }

    ScopedJobSystem(const ScopedJobSystem&) = delete;
    ScopedJobSystem& operator=(const ScopedJobSystem&) = delete;
    ScopedJobSystem(ScopedJobSystem&&) = delete;
    ScopedJobSystem& operator=(ScopedJobSystem&&) = delete;

    [[nodiscard]] bool started() const noexcept { return started_; }
    [[nodiscard]] cy::jobs::JobSystem& get() noexcept { return system_; }

private:
    cy::jobs::JobSystem system_;
    bool started_ = false;
};

/// A scene laid out across the frustum and behind it, so that every partition holds a mix of
/// survivors and rejections rather than one partition holding all of either.
void populate(SpatialIndex& index, cy::u32 count) noexcept {
    for (cy::u32 instance = 0; instance < count; ++instance) {
        SpatialEntry entry;
        const auto step = static_cast<cy::f32>(instance);
        const cy::f32 z = (instance % 4U == 0U) ? 10.0F : -5.0F - (step * 0.05F);
        entry.bounds = cy::Aabb::from_center_extents(
            cy::Vec3{(step - (static_cast<cy::f32>(count) * 0.5F)) * 0.05F, 0.0F, z},
            cy::Vec3{0.2F, 0.2F, 0.2F});
        entry.stable_id = instance + 1U;
        entry.gpu_slot = instance;
        if (instance % 7U == 0U) {
            entry.flags |= cy::rendering::kSpatialTransparent;
        }
        if (instance % 11U == 0U) {
            entry.layer_mask = 1U << 5U;  // rejected by the view's mask
        }
        CY_REQUIRE(index.insert(entry).has_value());
    }
}

[[nodiscard]] bool same_list(cy::Span<const VisibleInstance> left,
                             cy::Span<const VisibleInstance> right) noexcept {
    if (left.size() != right.size()) {
        return false;
    }
    for (cy::usize at = 0; at < left.size(); ++at) {
        if (left[at].slot != right[at].slot || left[at].stable_id != right[at].stable_id ||
            left[at].gpu_slot != right[at].gpu_slot) {
            return false;
        }
    }
    return true;
}

}  // namespace

CY_TEST_CASE("eight workers produce the serial visible list, in the serial order") {
    constexpr cy::u32 kInstances = 4096;

    SpatialIndex index(allocator());
    populate(index, kInstances);
    const CullView view = make_view();

    // The serial answer first: no job system at all, which is what a caller without one gets.
    CullWorkspace serial_workspace(allocator());
    CullResults serial(allocator());
    CY_REQUIRE(cull_view(index, view, CullOptions{}, serial_workspace, serial).has_value());
    CY_REQUIRE_EQ(serial.stats.partitions, 1U);
    CY_REQUIRE(serial.opaque.size() > 0U);
    CY_REQUIRE(serial.transparent.size() > 0U);

    ScopedJobSystem jobs(8);
    CY_REQUIRE(jobs.started());

    // "above a configurable instance-count threshold": the threshold and the grain are the
    // configuration, and they are what decides the partitioning — never the worker count, which is
    // what makes the merged order reproducible on a machine with a different core count.
    CullOptions options;
    options.jobs = &jobs.get();
    options.parallel_threshold = 64;
    options.grain = 256;

    CullWorkspace parallel_workspace(allocator());
    CullResults parallel(allocator());
    CY_REQUIRE(cull_view(index, view, options, parallel_workspace, parallel).has_value());

    CY_CHECK_EQ(parallel.stats.partitions, (kInstances + options.grain - 1U) / options.grain);
    CY_CHECK_GT(parallel.stats.partitions, 1U);

    // Element for element, in the same order, in every typed list.
    CY_CHECK(same_list(parallel.opaque.span(), serial.opaque.span()));
    CY_CHECK(same_list(parallel.transparent.span(), serial.transparent.span()));
    CY_CHECK(same_list(parallel.motion.span(), serial.motion.span()));
    CY_CHECK(same_list(parallel.lights.span(), serial.lights.span()));

    // And the same report: a parallel cull that lost a rejection would still produce the right
    // visible set.
    CY_CHECK_EQ(parallel.stats.tested, serial.stats.tested);
    CY_CHECK_EQ(parallel.stats.rejected_by_layer, serial.stats.rejected_by_layer);
    CY_CHECK_EQ(parallel.stats.rejected_by_frustum, serial.stats.rejected_by_frustum);
    CY_CHECK_EQ(parallel.stats.rejected_by_range, serial.stats.rejected_by_range);
    CY_CHECK_EQ(parallel.stats.visible, serial.stats.visible);

    CY_TEST_MESSAGE("8 workers, " << parallel.stats.partitions << " partitions over " << kInstances
                                  << " instances: " << parallel.stats.visible
                                  << " visible, identical to the serial run");
}

CY_TEST_CASE("below the threshold the same call runs serially, whatever the job system offers") {
    // The threshold is the configuration the requirement names, so it has to DO something: a scene
    // under it runs on the calling thread even with eight workers waiting.
    SpatialIndex index(allocator());
    populate(index, 128);

    ScopedJobSystem jobs(8);
    CY_REQUIRE(jobs.started());

    CullOptions options;
    options.jobs = &jobs.get();
    options.parallel_threshold = 4096;
    options.grain = 32;

    CullWorkspace workspace(allocator());
    CullResults results(allocator());
    CY_REQUIRE(cull_view(index, make_view(), options, workspace, results).has_value());
    CY_CHECK_EQ(results.stats.partitions, 1U);
    CY_CHECK_GT(results.stats.visible, 0U);
}
