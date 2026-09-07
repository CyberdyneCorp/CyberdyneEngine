// Teardown under load: the two objects M6 destroys most often. M6 task 8.5.
//
// SEPARATE FROM THE UNIT SUITES, AND NOT BECAUSE THEY ARE A DIFFERENT SUBJECT. Forty rounds of
// building and destroying a culling output and a depth pyramid costs about 3.5 ms of CPU in the
// Debug profile, against the unit taxonomy's one millisecond. `testing-and-quality` says where such
// a test goes — "the next suite up" — so they went there rather than being shrunk until they
// stopped exercising the thing they exist for.
//
// WHY THEY EXIST AT ALL. M5.5's gate found this project's first engine defect in six milestones by
// tearing a subsystem down while a worker was still inside it: Jolt's job bridge destroyed its free
// list underneath a thread that was still releasing a job, one run in forty, as a SIGTRAP with no
// physics call on the stack. M6 creates and destroys worlds CONTINUOUSLY rather than once per
// fixture, so a culling output built for a scene that is being replaced, and a depth pyramid
// resized on every resolution change, are the ordinary case here rather than the exceptional one.
// Forty rounds is the frequency at which that defect appeared.

#include <cy/core/math/matrix.h>
#include <cy/core/math/projection.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/servers/render/culling/hzb.h>
#include <cy/test/test.h>

#include <vector>

using namespace cy::render;
using namespace cy::render::culling;
using cy::f32;
using cy::u32;
using cy::Vec3;

namespace {

cy::Allocator& allocator() noexcept {
    return cy::system_allocator(cy::MemoryDomain::Renderer);
}

GpuCullView forward_view(u32 instance_count) {
    GpuCullView view;
    const cy::Mat4 projection =
        cy::perspective_reversed_z(1.0471975512F, 16.0F / 9.0F, 0.1F, 1000.0F);
    const cy::Mat4 look =
        cy::look_at(Vec3{0.0F, 0.0F, 0.0F}, Vec3{0.0F, 0.0F, -1.0F}, Vec3{0.0F, 1.0F, 0.0F});
    write_frustum(view, cy::Frustum::from_view_projection(projection * look));
    write_field_of_view(view, 1.0471975512F);
    view.camera_forward[2] = -1.0F;
    view.instance_count = instance_count;
    return view;
}

GpuInstance instance_at(Vec3 centre, f32 radius) {
    GpuInstance instance;
    instance.flags = kInstanceActive | kInstanceVisible | kInstanceCastsShadow;
    instance.layer_mask = kDefaultLayer;
    instance.bounds_center[0] = centre.x;
    instance.bounds_center[1] = centre.y;
    instance.bounds_center[2] = centre.z;
    instance.bounds_radius = radius;
    return instance;
}

struct Fixture {
    std::vector<GpuInstance> instances;
    std::vector<GpuLodChain> chains{GpuLodChain{0, 1}};
    std::vector<GpuMeshLod> levels{GpuMeshLod{}};
    std::vector<u32> previous;

    [[nodiscard]] GpuCullScene scene() {
        previous.resize(instances.size(), 0U);
        GpuCullScene built;
        built.instances = cy::Span<const GpuInstance>(instances.data(), instances.size());
        built.chains = cy::Span<const GpuLodChain>(chains.data(), chains.size());
        built.mesh_lods = cy::Span<const GpuMeshLod>(levels.data(), levels.size());
        built.previous_levels = cy::Span<u32>(previous.data(), previous.size());
        return built;
    }
};

/// Fill level 0 with one depth everywhere and build the pyramid from it.
void fill(Hzb& pyramid, f32 depth) {
    for (f32& texel : pyramid.level(0)) {
        texel = depth;
    }
    pyramid.reduce();
    pyramid.mark_valid();
}

}  // namespace

CY_TEST_CASE("gpu cull: a scene and its output are destroyed mid-flight without leaking") {
    // TEARDOWN UNDER LOAD, not steady state. M6 creates and destroys worlds continuously, so a
    // culling output built for a scene that is being replaced is the ordinary case rather than the
    // exceptional one. Forty rounds because that is the frequency at which M5.5's job-bridge defect
    // appeared; a sanitised build is what makes this case worth its runtime.
    for (u32 round = 0; round < 40; ++round) {
        Fixture fixture;
        for (u32 index = 0; index < 24; ++index) {
            fixture.instances.push_back(instance_at(
                Vec3{static_cast<f32>(index % 6) - 3.0F, 0.0F, -8.0F - static_cast<f32>(index)},
                0.4F));
        }
        auto* output = new GpuCullOutput(allocator());
        CY_REQUIRE(output->reserve(64).has_value());
        CY_REQUIRE(cpu_reference_cull(fixture.scene(), forward_view(24), {}, *output).has_value());
        CY_CHECK(output->commands().size() > 0);
        // Destroyed while it still holds a frame's worth of draws, and while the scene that
        // produced them is about to go out of scope underneath it. The output holds no pointer back
        // into the scene, which is what makes this safe — and this case is what says so.
        delete output;
    }
}

CY_TEST_CASE("hzb: a pyramid is resized and destroyed repeatedly without leaking") {
    // Teardown under load: M6 creates and destroys worlds continuously, and a depth pyramid is
    // resized on every resolution change and destroyed with the view that owned it. Its levels are
    // separately allocated, so its destructor is the thing that has to be right.
    for (u32 round = 0; round < 40; ++round) {
        auto* pyramid = new Hzb(allocator());
        CY_REQUIRE(pyramid->resize(64 + (round % 7), 32 + (round % 5)).has_value());
        fill(*pyramid, 0.5F);
        // Resized again while it holds contents, which releases every level and allocates new ones.
        CY_REQUIRE(pyramid->resize(16, 16).has_value());
        CY_CHECK(!pyramid->valid());
        fill(*pyramid, 0.25F);
        delete pyramid;
    }
}
