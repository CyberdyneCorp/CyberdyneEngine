// SPDX-License-Identifier: MIT
// THE OCEAN PATCH BUILT ACROSS WORKERS. M11.c, `m11a:world-budget-on-a-device`.
//
// The world demo builds a 6 534-vertex patch every frame, and spreading its rows over the job
// system is what brought the ocean band inside the frame budget. The claim that makes that
// legitimate is that the parallel build is the serial build: every vertex is a pure function of the
// sea, its lattice position and the time, and each row writes only its own vertices. This case
// checks it bit for bit.
//
// INTEGRATION rather than unit because it starts a real `jobs::JobSystem` — worker threads and
// their scratch — which alone costs more CPU than a unit case's millisecond.

#include <cy/test/test.h>

#include <cy/core/jobs/job_system.h>
#include <cy/water/ocean.h>

#include "fixtures.h"

namespace test = cy::water::test;
using cy::water::OceanSurface;
using cy::water::OceanSurfaceParams;

namespace {

/// Every output of two patches, compared bit for bit. Returns how many vertices differ.
[[nodiscard]] cy::u32 differing_vertices(const OceanSurface& a, const OceanSurface& b) noexcept {
    if (a.positions().size() != b.positions().size()) {
        return 0xFFFF'FFFFu;
    }
    cy::u32 differing = 0;
    for (cy::usize index = 0; index < a.positions().size(); ++index) {
        const cy::Vec3 pa = a.positions()[index];
        const cy::Vec3 pb = b.positions()[index];
        const cy::Vec3 na = a.normals()[index];
        const cy::Vec3 nb = b.normals()[index];
        const bool same = pa.x == pb.x && pa.y == pb.y && pa.z == pb.z && na.x == nb.x &&
                          na.y == nb.y && na.z == nb.z &&
                          a.breaking()[index] == b.breaking()[index];
        differing += same ? 0U : 1U;
    }
    return differing;
}

}  // namespace

CY_TEST_CASE("a patch built across workers is the serial patch, bit for bit") {
    // The world demo builds its 6 534-vertex patch every frame and spreads the rows over the job
    // system. Rows are independent, so the answer must not depend on the schedule: the same patch
    // built serially and in parallel, at two times, holds identical bits in every output.
    cy::jobs::JobSystem jobs;
    cy::jobs::JobSystemConfig config;
    config.worker_count = 4;
    config.task_slots_per_participant = 256;
    config.deque_capacity = 256;
    config.scratch_bytes_per_participant = cy::usize{64} * 1024;
    CY_REQUIRE(jobs.start(config).has_value());

    const auto model = cy::water::build_ocean_model(test::rough_sea(), 23);
    CY_REQUIRE(model.has_value());
    // Small enough for a unit case's CPU budget, large enough that four workers each take rows:
    // 27 rows at `kOceanRowsPerJob` a job is four partitions, the last one short.
    OceanSurfaceParams params;
    params.near_cell_metres = 3.0F;
    params.ring_quads = 8;
    params.rings = 3;
    OceanSurface serial(test::allocator());
    OceanSurface parallel(test::allocator());
    CY_REQUIRE(serial.configure(params).has_value());
    CY_REQUIRE(parallel.configure(params).has_value());
    const cy::world::WorldVec3d camera{905.0, 0.0, -77.25};
    for (const cy::f64 time : {0.0, 311.5}) {
        CY_REQUIRE(serial.build(*model, camera, time).has_value());
        CY_REQUIRE(parallel.build(*model, camera, time, &jobs).has_value());
        CY_CHECK_EQ(differing_vertices(serial, parallel), 0u);
    }
    jobs.shutdown();
}
