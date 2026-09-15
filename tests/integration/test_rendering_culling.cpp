// The hierarchical depth buffer ON THE DEVICE, checked against the CPU model. M11.c task 4.1-4.2.
//
// ================================================================================================
// WHAT THIS SUITE IS FOR, AND WHY IT IS AN INTEGRATION SUITE RATHER THAN A MODULE'S OWN
// ================================================================================================
//
// `rendering-culling-and-lod`'s M11.c requirement is one sentence and it is about this file:
//
//   "A claim that occlusion culling works SHALL be evaluated on a device, and the device pass SHALL
//    be checked against the CPU model's answer for the same scene rather than against a screenshot
//    or a counter that reads non-zero. Where no device is available, the claim SHALL be reported as
//    NOT EVALUATED rather than satisfied by the model."
//
// A CPU model of a depth pyramid has been in this tree since M6 — `cy::render::culling::Hzb`,
// layer 2, no texture, no device — and it has passed its own tests for five milestones. **A model
// passing its own tests is the most available way to record a capability no frame performs.** So
// what is asserted here is never "the model is right"; it is that the DEVICE produced the model's
// answer, texel for texel and counter for counter, and that the two were asked the same question.
//
// It lives in tests/integration/ because it is a claim about a seam between three modules —
// `cy::rendering-hzb` builds the pyramid, `cy::servers-render-culling` is the model, and
// `cy::rendering-gpu-culling` is the consumer whose dispatch used to refuse the flag — and no one
// of the three owns it. That is the same argument tests/integration/test_standard_fields.cpp makes.
//
// ================================================================================================
// NOTHING HERE CAN PASS BY BEING EMPTY
// ================================================================================================
//
// Two buffers of zeroes agree perfectly, and so do two culls that occluded nothing. Every case
// below asserts a POSITIVE quantity as well as an agreement: the pyramid must contain at least two
// distinct depths, and the occlusion cull must actually reject something by occlusion. This suite
// exists because of a project history in which seven criteria were green and could not go red.
//
// ================================================================================================
// NO DEVICE IS NOT A PASS
// ================================================================================================
//
// On a machine with no Vulkan device each case reports the backend that was selected instead and
// returns. That is what "not evaluated" looks like from inside a test binary — and it is why the
// ledger criterion that runs this suite carries `requires = "gpu"`, so that a host without one is
// recorded as unable to ask the question rather than as having answered it.

#include <cy/backends/rhi/backend.h>
#include <cy/backends/rhi/device.h>
#include <cy/backends/rhi/null/null_device.h>
#include <cy/backends/rhi/validation.h>
#include <cy/backends/rhi/vulkan/vulkan_backend.h>
#include <cy/core/math/matrix.h>
#include <cy/core/math/projection.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/rendering/gpu_culling/cull_pass.h>
#include <cy/rendering/graph/executor.h>
#include <cy/rendering/graph/graph.h>
#include <cy/rendering/hzb/hzb_pass.h>
#include <cy/servers/render/culling/gpu_cull.h>
#include <cy/servers/render/culling/hzb.h>
#include <cy/test/test.h>

#include <cstdio>
#include <vector>

using cy::f32;
using cy::Mat4;
using cy::u32;
using cy::Vec3;
using cy::render::GpuInstance;
using cy::render::kDefaultLayer;
using cy::render::kInstanceActive;
using cy::render::kInstanceVisible;
using cy::render::culling::cpu_reference_cull;
using cy::render::culling::GpuCullCounters;
using cy::render::culling::GpuCullOutput;
using cy::render::culling::GpuCullScene;
using cy::render::culling::GpuCullView;
using cy::render::culling::GpuLodChain;
using cy::render::culling::GpuMeshLod;
using cy::render::culling::Hzb;
using cy::render::culling::hzb_level_count;
using cy::render::culling::HzbOcclusionTester;
using cy::render::culling::kGpuCullOcclusion;
using cy::render::culling::kNoLodFade;
using cy::render::culling::project_sphere;
using cy::render::culling::ScreenRect;
using cy::render::culling::write_field_of_view;
using cy::render::culling::write_frustum;
using cy::rendering::gpu_culling::GpuCullPass;
using cy::rendering::gpu_culling::GpuCullPassDescription;
using cy::rendering::gpu_culling::GpuCullReadback;
using cy::rendering::hzb::HzbPass;
using cy::rendering::hzb::HzbPassDescription;

namespace {

/// Small on purpose: eight levels is enough to exercise the whole chain, including the
/// odd-dimension fold at 9 -> 5 and 5 -> 3, and the whole pyramid is about 12k floats.
constexpr u32 kWidth = 128;
constexpr u32 kHeight = 72;

cy::Allocator& allocator() noexcept {
    return cy::system_allocator(cy::MemoryDomain::Renderer);
}

void count_validation(cy::rhi::ValidationSeverity severity, const char* message,
                      void* user) noexcept {
    if (severity == cy::rhi::ValidationSeverity::Error && user != nullptr) {
        ++*static_cast<u32*>(user);
    }
    std::fprintf(stderr, "vulkan validation %s: %s\n",
                 severity == cy::rhi::ValidationSeverity::Error ? "error" : "warning",
                 message != nullptr ? message : "");
}

/// A device of its own per case, for the reason tests/render/device.h gives: synchronisation
/// validation keeps per-queue state for the process's lifetime, and recycled handles across two
/// devices in one process produce phantom cross-test hazards that look damning and are not.
class DeviceFixture {
public:
    DeviceFixture() noexcept : allocator_(cy::system_allocator(cy::MemoryDomain::Gpu)) {
        (void)cy::rhi::vulkan::register_vulkan_backend();
        (void)cy::rhi::null::register_null_backend();
        cy::rhi::DeviceDescription description;
        description.application_name = "cy_test_integration_rendering_culling";
        description.enable_validation = true;
        description.enable_synchronisation_validation = true;
        description.request_async_compute = false;
        device_ = cy::rhi::create_device(allocator_, "vulkan", description, selection_);
        if (device_.has_value()) {
            device_.value()->set_validation_callback(&count_validation, &validation_errors_);
        }
    }

    ~DeviceFixture() {
        if (device_.has_value()) {
            (void)device_.value()->wait_idle();
            cy::rhi::destroy_device(allocator_, device_.value());
        }
    }

    DeviceFixture(const DeviceFixture&) = delete;
    DeviceFixture& operator=(const DeviceFixture&) = delete;

    [[nodiscard]] bool has_gpu() const noexcept {
        return device_.has_value() &&
               device_.value()->capabilities().backend() == cy::rhi::BackendKind::Vulkan;
    }
    [[nodiscard]] cy::rhi::Device& device() const noexcept { return *device_.value(); }
    [[nodiscard]] u32 validation_errors() const noexcept { return validation_errors_; }

    void report_skip() const noexcept {
        std::fprintf(stderr,
                     "NOT EVALUATED: no Vulkan device on this machine; the backend selected was "
                     "'%s' because %s. The occlusion claim is not satisfied by the CPU model.\n",
                     selection_.selected != nullptr ? selection_.selected : "(none)",
                     selection_.reason != nullptr ? selection_.reason : "(no reason given)");
    }

private:
    cy::Allocator& allocator_;
    cy::rhi::BackendSelection selection_{};
    u32 validation_errors_ = 0;
    cy::Expected<cy::rhi::Device*, cy::Error> device_ =
        cy::fail(cy::ErrorCode::Unavailable, "not created");
};

/// The camera every case uses: at the origin, looking down -Z, reversed Z, 16:9 to match the
/// pyramid's own aspect so that a projected rectangle means what it says.
Mat4 view_projection() noexcept {
    const Mat4 projection = cy::perspective_reversed_z(
        1.0471975512F, static_cast<f32>(kWidth) / static_cast<f32>(kHeight), 0.1F, 1000.0F);
    const Mat4 look =
        cy::look_at(Vec3{0.0F, 0.0F, 0.0F}, Vec3{0.0F, 0.0F, -1.0F}, Vec3{0.0F, 1.0F, 0.0F});
    return projection * look;
}

GpuCullView forward_view(u32 instance_count) noexcept {
    GpuCullView view;
    write_frustum(view, cy::Frustum::from_view_projection(view_projection()));
    write_field_of_view(view, 1.0471975512F);
    view.camera_forward[0] = 0.0F;
    view.camera_forward[1] = 0.0F;
    view.camera_forward[2] = -1.0F;
    view.instance_count = instance_count;
    view.layer_mask = kDefaultLayer;
    return view;
}

GpuInstance instance_at(Vec3 centre, f32 radius) noexcept {
    GpuInstance instance;
    instance.flags = kInstanceActive | kInstanceVisible;
    instance.layer_mask = kDefaultLayer;
    instance.bounds_center[0] = centre.x;
    instance.bounds_center[1] = centre.y;
    instance.bounds_center[2] = centre.z;
    instance.bounds_radius = radius;
    instance.lod_chain = 0;
    return instance;
}

/// The depth ONE occluder leaves behind, as level 0 of a pyramid.
///
/// Built through `project_sphere` rather than by hand: the occluder is an INPUT and constructing it
/// with the engine's own projection is what makes the case say "a sphere at this position hides
/// what is behind it" instead of "these texels hold this number". Everywhere the occluder is not,
/// the depth is 0 — the far plane under reversed Z, which occludes nothing.
std::vector<f32> depth_behind_one_occluder(Vec3 centre, f32 radius, ScreenRect& rect_out) {
    std::vector<f32> depths(static_cast<size_t>(kWidth) * kHeight, 0.0F);
    const ScreenRect rect = project_sphere(centre, radius, view_projection(), kWidth, kHeight);
    rect_out = rect;
    if (!rect.valid) {
        return depths;
    }
    const auto clamp = [](f32 value, u32 limit) noexcept -> u32 {
        if (!(value > 0.0F)) {
            return 0;
        }
        const auto texel = static_cast<u32>(value);
        return texel < limit ? texel : limit - 1U;
    };
    for (u32 y = clamp(rect.min_y, kHeight); y <= clamp(rect.max_y, kHeight); ++y) {
        for (u32 x = clamp(rect.min_x, kWidth); x <= clamp(rect.max_x, kWidth); ++x) {
            depths[(static_cast<size_t>(y) * kWidth) + x] = rect.nearest_depth;
        }
    }
    return depths;
}

/// The same pyramid, on the processor. `Hzb::level(0)` is written with the caller's depths and
/// `reduce()` fills the rest — which is the model the device is judged against.
[[nodiscard]] bool build_model(Hzb& model, const std::vector<f32>& depths) {
    if (!model.resize(kWidth, kHeight).has_value()) {
        return false;
    }
    const cy::Span<f32> level0 = model.level(0);
    if (level0.size() != depths.size()) {
        return false;
    }
    for (size_t index = 0; index < depths.size(); ++index) {
        level0[index] = depths[index];
    }
    model.reduce();
    model.mark_valid();
    return true;
}

/// A device buffer holding those depths, ready for `HzbPass` to seed level 0 from.
[[nodiscard]] cy::rhi::BufferHandle upload_depth(cy::rhi::Device& device,
                                                 const std::vector<f32>& depths) {
    cy::rhi::BufferDescription description;
    description.name = "test depth";
    description.size = depths.size() * sizeof(f32);
    description.usage = cy::rhi::BufferUsage::Storage | cy::rhi::BufferUsage::TransferSource;
    description.memory = cy::rhi::MemoryUse::Upload;
    cy::Expected<cy::rhi::BufferHandle, cy::Error> buffer = device.create_buffer(description);
    if (!buffer.has_value()) {
        return {};
    }
    void* mapped = device.buffer_mapped_pointer(*buffer);
    if (mapped == nullptr) {
        return {};
    }
    std::memcpy(mapped, depths.data(), depths.size() * sizeof(f32));
    return *buffer;
}

cy::rendering::ResourceId import_depth(cy::rendering::RenderGraph& graph,
                                       cy::rhi::BufferHandle handle, size_t texels) {
    cy::rendering::BufferRequest request;
    request.name = "test depth";
    request.size = texels * sizeof(f32);
    request.extra_usage = cy::rhi::BufferUsage::TransferSource;
    return graph.import_buffer(request, handle);
}

/// How many distinct depths the model holds. A pyramid whose every texel is the same number agrees
/// with any other such pyramid, so an agreement is only evidence when this is at least two.
[[nodiscard]] u32 distinct_depths(const Hzb& model) {
    std::vector<f32> seen;
    for (u32 level = 0; level < model.level_count(); ++level) {
        for (f32 value : model.level(level)) {
            bool known = false;
            for (f32 other : seen) {
                known = known || other == value;
            }
            if (!known) {
                seen.push_back(value);
            }
        }
    }
    return static_cast<u32>(seen.size());
}

}  // namespace

// --- The pyramid itself
// ---------------------------------------------------------------------------

CY_TEST_CASE(
    "a hierarchical depth buffer is built on the device and agrees with the CPU model texel for "
    "texel") {
    DeviceFixture gpu;
    if (!gpu.has_gpu()) {
        gpu.report_skip();
        return;
    }

    ScreenRect occluder{};
    const std::vector<f32> depths =
        depth_behind_one_occluder(Vec3{0.0F, 0.0F, -10.0F}, 3.0F, occluder);
    CY_REQUIRE(occluder.valid);

    Hzb model(allocator());
    CY_REQUIRE(build_model(model, depths));
    // NOT VACUOUS: a pyramid of one repeated value would agree with anything.
    CY_REQUIRE(distinct_depths(model) >= 2);

    const cy::rhi::BufferHandle depth = upload_depth(gpu.device(), depths);
    CY_REQUIRE(!depth.is_null());

    HzbPass pass;
    HzbPassDescription description;
    description.width = kWidth;
    description.height = kHeight;
    description.readback = true;
    CY_REQUIRE(pass.create(allocator(), gpu.device(), description).has_value());

    CY_REQUIRE(gpu.device().begin_frame().has_value());
    cy::rendering::RenderGraph graph(allocator());
    const cy::rendering::ResourceId source = import_depth(graph, depth, depths.size());
    CY_REQUIRE(pass.declare(graph, source).has_value());
    cy::rendering::GraphExecutor executor(allocator(), gpu.device());
    CY_REQUIRE(
        executor.execute(graph, cy::rendering::CompileOptions{}, cy::rendering::ExecuteOptions{})
            .has_value());
    CY_REQUIRE(gpu.device().wait_idle().has_value());
    CY_REQUIRE(gpu.device().end_frame().has_value());
    pass.mark_valid();

    Hzb device_pyramid(allocator());
    CY_REQUIRE(pass.read_back(device_pyramid).has_value());

    CY_REQUIRE(device_pyramid.level_count() == model.level_count());
    CY_REQUIRE(device_pyramid.level_count() == hzb_level_count(kWidth, kHeight));

    // THE COMPARISON IS TEXEL FOR TEXEL, AND THE DISAGREEMENT IS NAMED. A count of differing texels
    // would say "they disagree"; the first one names WHERE, which is the difference between a
    // failing test and a debuggable one. A minimum over floats is exact, so there is no tolerance
    // here and there should not be: a tolerance on a reduction that only ever takes one of its
    // inputs would be absorbing a real divergence.
    u32 differing = 0;
    for (u32 level = 0; level < model.level_count(); ++level) {
        const cy::Span<const f32> want = model.level(level);
        const cy::Span<const f32> got = device_pyramid.level(level);
        CY_REQUIRE(want.size() == got.size());
        for (cy::usize index = 0; index < want.size(); ++index) {
            if (want[index] != got[index]) {
                if (differing == 0) {
                    std::fprintf(stderr,
                                 "hzb level %u texel %zu: the model says %.9g and the device says "
                                 "%.9g\n",
                                 level, static_cast<size_t>(index),
                                 static_cast<double>(want[index]), static_cast<double>(got[index]));
                }
                ++differing;
            }
        }
    }
    std::fprintf(stderr, "hzb: %u level(s), %u distinct depth(s), %u differing texel(s)\n",
                 model.level_count(), distinct_depths(model), differing);
    CY_CHECK(differing == 0);
    CY_CHECK(gpu.validation_errors() == 0);

    gpu.device().destroy_buffer(depth);
}

// --- The occlusion cull the pyramid was built for
// ------------------------------------------------

CY_TEST_CASE(
    "the device occlusion cull rejects the instances the CPU model rejects, and names a "
    "disagreement") {
    DeviceFixture gpu;
    if (!gpu.has_gpu()) {
        gpu.report_skip();
        return;
    }

    ScreenRect occluder{};
    const std::vector<f32> depths =
        depth_behind_one_occluder(Vec3{0.0F, 0.0F, -10.0F}, 3.0F, occluder);
    CY_REQUIRE(occluder.valid);

    Hzb model(allocator());
    CY_REQUIRE(build_model(model, depths));

    // Three instances, and the three answers the requirement distinguishes.
    std::vector<GpuInstance> instances;
    // Behind the occluder and inside its silhouette: OCCLUDED.
    instances.push_back(instance_at(Vec3{0.0F, 0.0F, -200.0F}, 3.0F));
    // In front of it: not occluded, and it is what stops "everything was culled" from passing.
    instances.push_back(instance_at(Vec3{0.0F, 0.0F, -4.0F}, 0.5F));
    // Beside it, where the depth buffer is still at the far plane: not occluded.
    instances.push_back(instance_at(Vec3{18.0F, 0.0F, -40.0F}, 1.0F));

    std::vector<GpuLodChain> chains{GpuLodChain{0, 1}};
    std::vector<GpuMeshLod> levels(1);
    levels[0].index_count = 300;
    levels[0].screen_coverage_threshold = 0.0F;
    std::vector<u32> previous(instances.size(), kNoLodFade);

    GpuCullView view = forward_view(static_cast<u32>(instances.size()));
    view.flags |= kGpuCullOcclusion;

    const auto scene = [&]() noexcept {
        GpuCullScene built;
        built.instances = cy::Span<const GpuInstance>(instances.data(), instances.size());
        built.chains = cy::Span<const GpuLodChain>(chains.data(), chains.size());
        built.mesh_lods = cy::Span<const GpuMeshLod>(levels.data(), levels.size());
        built.previous_levels = cy::Span<u32>(previous.data(), previous.size());
        return built;
    };

    // The model's answer, through the SAME tester `HzbPass` mirrors on the device.
    const HzbOcclusionTester tester(model, view_projection());
    cy::render::culling::GpuCullOptions options;
    options.occlusion = &tester;
    GpuCullOutput expected(allocator());
    CY_REQUIRE(expected.reserve(static_cast<u32>(instances.size()) + 1).has_value());
    CY_REQUIRE(cpu_reference_cull(scene(), view, options, expected).has_value());
    // The reference WRITES the hysteresis state it read, so the device side has to start from the
    // same place the reference did rather than from what the reference left. Re-seeded in place: a
    // `std::vector` copy assignment here is what GCC 13 reports as a potential null dereference
    // under -Werror, and a loop says the same thing without arguing with the optimiser.
    for (u32& level : previous) {
        level = kNoLodFade;
    }

    // NOT VACUOUS: if the model occluded nothing, the device agreeing with it says nothing at all.
    CY_REQUIRE(expected.counters().rejected_by_occlusion >= 1);
    CY_REQUIRE(expected.counters().visible >= 1);

    const cy::rhi::BufferHandle depth = upload_depth(gpu.device(), depths);
    CY_REQUIRE(!depth.is_null());

    HzbPass pyramid;
    HzbPassDescription description;
    description.width = kWidth;
    description.height = kHeight;
    CY_REQUIRE(pyramid.create(allocator(), gpu.device(), description).has_value());
    pyramid.mark_valid();

    GpuCullPass pass;
    GpuCullPassDescription cull_description;
    cull_description.max_instances = static_cast<u32>(instances.size());
    cull_description.max_draws = static_cast<u32>(instances.size());
    cull_description.max_lod_chains = static_cast<u32>(chains.size());
    cull_description.max_mesh_lods = static_cast<u32>(levels.size());
    cull_description.max_visibility_ranges = 0;
    cull_description.max_previous_levels = static_cast<u32>(instances.size());
    CY_REQUIRE(pass.create(allocator(), gpu.device(), cull_description).has_value());
    CY_REQUIRE(pass.set_occlusion(&pyramid, view_projection()).has_value());
    CY_REQUIRE(pass.upload(scene(), view).has_value());

    CY_REQUIRE(gpu.device().begin_frame().has_value());
    cy::rendering::RenderGraph graph(allocator());
    const cy::rendering::ResourceId source = import_depth(graph, depth, depths.size());
    cy::Expected<cy::rendering::ResourceId, cy::Error> pyramid_resource =
        pyramid.declare(graph, source);
    CY_REQUIRE(pyramid_resource.has_value());
    CY_REQUIRE(pass.declare(graph, *pyramid_resource).has_value());
    cy::rendering::GraphExecutor executor(allocator(), gpu.device());
    CY_REQUIRE(
        executor.execute(graph, cy::rendering::CompileOptions{}, cy::rendering::ExecuteOptions{})
            .has_value());
    CY_REQUIRE(gpu.device().wait_idle().has_value());
    CY_REQUIRE(gpu.device().end_frame().has_value());

    cy::Expected<GpuCullReadback, cy::Error> actual = pass.read_back();
    CY_REQUIRE(actual.has_value());

    const GpuCullCounters& want = expected.counters();
    const GpuCullCounters& got = actual->counters;
    std::fprintf(stderr,
                 "occlusion cull: model rejected %u and drew %u; device rejected %u and drew %u\n",
                 want.rejected_by_occlusion, want.visible, got.rejected_by_occlusion, got.visible);
    // A DISAGREEMENT IS REPORTED AS A FAILURE NAMING THE COUNTS, which is the requirement's own
    // scenario, rather than resolved in favour of the device.
    CY_CHECK(got.rejected_by_occlusion == want.rejected_by_occlusion);
    CY_CHECK(got.visible == want.visible);
    CY_CHECK(got.tested == want.tested);
    CY_CHECK(got.draws == want.draws);

    // And the draws themselves, in ascending slot order, because two counters can agree over two
    // different sets of survivors.
    CY_REQUIRE(actual->payloads.size() == expected.payloads().size());
    for (cy::usize index = 0; index < expected.payloads().size(); ++index) {
        CY_CHECK(actual->payloads[index].instance_slot == expected.payloads()[index].instance_slot);
    }
    CY_CHECK(gpu.validation_errors() == 0);

    gpu.device().destroy_buffer(depth);
}

CY_TEST_CASE(
    "an occlusion cull with no pyramid attached is refused rather than reported as "
    "'nothing was occluded'") {
    DeviceFixture gpu;
    if (!gpu.has_gpu()) {
        gpu.report_skip();
        return;
    }

    std::vector<GpuInstance> instances{instance_at(Vec3{0.0F, 0.0F, -20.0F}, 2.0F)};
    std::vector<GpuLodChain> chains{GpuLodChain{0, 1}};
    std::vector<GpuMeshLod> levels(1);
    levels[0].index_count = 300;
    std::vector<u32> previous(instances.size(), kNoLodFade);

    GpuCullScene scene;
    scene.instances = cy::Span<const GpuInstance>(instances.data(), instances.size());
    scene.chains = cy::Span<const GpuLodChain>(chains.data(), chains.size());
    scene.mesh_lods = cy::Span<const GpuMeshLod>(levels.data(), levels.size());
    scene.previous_levels = cy::Span<u32>(previous.data(), previous.size());

    GpuCullView view = forward_view(1);
    view.flags |= kGpuCullOcclusion;

    GpuCullPass pass;
    GpuCullPassDescription description;
    description.max_instances = 1;
    description.max_draws = 1;
    description.max_lod_chains = 1;
    description.max_mesh_lods = 1;
    description.max_visibility_ranges = 0;
    description.max_previous_levels = 1;
    CY_REQUIRE(pass.create(allocator(), gpu.device(), description).has_value());

    // THE REFUSAL M7 WROTE IS STILL THE REFUSAL, narrowed to the case it was written for. A cull
    // that accepted the flag with no pyramid would report "nothing was occluded" and would be
    // indistinguishable from a working occlusion cull.
    CY_CHECK(!pass.upload(scene, view).has_value());

    // And it is only the flag that is refused: the same scene without it culls.
    view.flags &= ~kGpuCullOcclusion;
    CY_CHECK(pass.upload(scene, view).has_value());
}
