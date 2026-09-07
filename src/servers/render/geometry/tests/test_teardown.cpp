// Teardown under load: the two objects M6 rebuilds most often. M6 task 8.4.
//
// SEPARATE FROM THE UNIT SUITE, for the reason the culling module's own teardown suite gives: forty
// rounds of filling and destroying a blend-shape set and a resource ledger costs about 2 ms of CPU
// in the Debug profile, against the unit taxonomy's one millisecond, and `testing-and-quality` puts
// a test that expensive in the next suite up.
//
// WHAT THEY ARE FOR. M6 streams cells in and out continuously. A facial rig's shape set is rebuilt
// whenever a character streams in or out; a resource ledger with pending releases outstanding is
// the ordinary state rather than the exceptional one, and a ledger destroyed in that state must not
// be holding anything the device still needs to be told about. M5.5's gate found this project's
// first engine defect by tearing a subsystem down mid-flight, one run in forty — which is where the
// round count comes from.

#include <cy/core/memory/system_allocator.h>
#include <cy/servers/render/geometry/resources.h>
#include <cy/servers/render/geometry/skinning.h>
#include <cy/test/test.h>

#include <vector>

using namespace cy::render::geometry;
using cy::f32;
using cy::u32;

namespace {

cy::Allocator& allocator() noexcept {
    return cy::system_allocator(cy::MemoryDomain::Renderer);
}

BlendShapeDelta delta(u32 vertex, f32 x) {
    BlendShapeDelta value;
    value.vertex = vertex;
    value.position[0] = x;
    return value;
}

}  // namespace

CY_TEST_CASE("blend shapes: a set is filled and destroyed repeatedly, mid-flight") {
    // Teardown under load. A facial rig's shape set is rebuilt whenever a character streams in or
    // out, which under M6's continuous streaming is every few seconds.
    for (u32 round = 0; round < 40; ++round) {
        auto* shapes = new BlendShapeSet(allocator());
        for (u32 shape = 0; shape < 16; ++shape) {
            std::vector<BlendShapeDelta> deltas;
            deltas.reserve(32);
            for (u32 vertex = 0; vertex < 32; ++vertex) {
                deltas.push_back(delta((vertex * 3) + shape, static_cast<f32>(vertex)));
            }
            CY_REQUIRE(shapes->add(cy::Span<const BlendShapeDelta>(deltas.data(), deltas.size()))
                           .has_value());
            CY_REQUIRE(shapes->set_weight(shape, 0.25F).has_value());
        }
        CY_CHECK(shapes->active_delta_count(0.1F) == 16 * 32);
        // Cleared and refilled before destruction, which is what a re-stream does.
        shapes->clear();
        CY_CHECK(shapes->size() == 0);
        delete shapes;
    }
}

CY_TEST_CASE("ledger: resources churn and the ledger is destroyed mid-flight") {
    // Teardown under load. M6 streams cells in and out continuously, so a ledger with pending
    // releases outstanding is the ordinary state rather than the exceptional one — and a ledger
    // destroyed in that state must not be holding anything the device still needs to be told about.
    for (u32 round = 0; round < 40; ++round) {
        auto* ledger = new ResourceLedger(allocator());
        ledger->set_frames_in_flight(3);
        ResourceId ids[32] = {};
        for (u32 index = 0; index < 32; ++index) {
            const auto id =
                ledger->acquire(index % 2 == 0 ? MemoryCategory::Meshes : MemoryCategory::Textures,
                                1024ULL * (index + 1), true);
            CY_REQUIRE(id.has_value());
            ids[index] = id.value();
        }
        for (u32 index = 0; index < 32; index += 2) {
            CY_REQUIRE(ledger->release(ids[index], round).has_value());
        }
        // Retired only part way, so half the released resources are still pending when the ledger
        // goes away.
        (void)ledger->retire(round + 1);
        CY_CHECK(ledger->report().pending_release > 0);
        delete ledger;
    }
}
