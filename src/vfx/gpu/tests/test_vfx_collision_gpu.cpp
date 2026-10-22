// SPDX-License-Identifier: MIT
// Particle collision on the device, checked against the CPU executor by comparing buffers.
// `vfx-system` — "Collision". `render.vfx_collision_gpu`, Vulkan.
//
// Every case runs one colliding effect twice — through `SimulationWorld` on the CPU and through
// `VfxGpuPass` on the device — and compares the liveness array slot for slot and the positions and
// velocities particle by particle, as `test_vfx_gpu_pass.cpp` does for the plume. The colliders are
// the four analytic primitives, the scene field — the GI clipmaps' own device copy, bound through
// `GpuGiScene::field_binding`, against the same `DistanceField` the CPU path reads — and a depth
// buffer uploaded to a texture the CPU path reads as an array.
//
// A CONTACT IS A DISCONTINUITY, and that decides the tolerance. A driver that contracts a multiply
// and an add into a fused one moves a particle by a last bit; a particle a last bit from a surface
// can touch on one path and not on the other, and from then on the two are different particles.
// So each case measures how many particles disagree by more than a tight bound and requires none
// to, over a run long enough for every particle to have bounced, slid and come to rest.

#include "collision_effects.h"
#include "collision_scene.h"
#include "gpu_fixture.h"
#include "support.h"

#include <cy/core/memory/system_allocator.h>
#include <cy/rendering/gi/distance_field.h>
#include <cy/rendering/gi_gpu/gpu_scene.h>
#include <cy/vfx/collision.h>
#include <cy/vfx/gpu_layout.h>

#include <cy_features.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

using namespace cy;
using namespace cy::vfx;
using namespace cy::vfx_gpu_test;
using cy::vfx::gpu::GpuCollisionDepth;
using cy::vfx::gpu::GpuCollisionField;
using cy::vfx::gpu::GpuCollisionInputs;
using cy::vfx::gpu::GpuPassDescription;
using cy::vfx::gpu::GpuStepInputs;
using cy::vfx::gpu::VfxGpuPass;
using cy::vfx_test::DropScenario;

namespace {

Allocator& allocator() noexcept {
    return system_allocator(MemoryDomain::Renderer);
}

#if defined(CY_SHADER_SLANG) && CY_SHADER_SLANG
constexpr bool kKernelCompilerAvailable = true;
#else
constexpr bool kKernelCompilerAvailable = false;
#endif

constexpr f32 kSubstep = 1.0F / 60.0F;

/// How far a particle may be apart on the two paths, in metres and metres a second, against an
/// analytic collider and the depth buffer. A contact resolves a penetration to separation zero and
/// a bounce scales a velocity, so a last-bit difference stays a last-bit difference unless it
/// decides a contact — which is what the count of disagreeing particles is for. Measured worst:
/// 1.4e-05 m and 9.9e-06 m/s (analytic), 8.7e-05 m and 3.5e-05 m/s (depth).
constexpr f32 kPositionTolerance = 1.0e-4F * 5.0F;
constexpr f32 kVelocityTolerance = 1.0e-3F;

/// The same, against the scene field. A field normal is a difference of field samples over five
/// centimetres, each a trilinear blend the device may contract into fused multiply-adds, so the two
/// paths' normals differ in the sixth figure — and a particle sliding a metre to rest under
/// friction carries that into where it stops. Measured worst: 1.6 mm and 5.4 mm/s after two
/// seconds; the bound is a fiftieth of the field's quarter-metre voxel, and a field that was not
/// bound at all misses it by twenty metres.
constexpr f32 kFieldPositionTolerance = 5.0e-3F;
constexpr f32 kFieldVelocityTolerance = 2.0e-2F;

/// The GI scene the field cases collide with: the stairs, as boxes in the GI's own field, and its
/// device copy.
struct GiStairs {
    gi_support::BoxField floor{Vec3{4.0F, 0.5F, 4.0F}, 1.0F, 17};
    std::vector<std::unique_ptr<gi_support::BoxField>> steps;
    rendering::gi::DistanceField field;

    GiStairs() {
        rendering::gi::ClipmapSettings settings;
        settings.levels = 2;
        settings.resolution = 32;
        settings.base_extent_metres = 8.0F;
        CY_REQUIRE(field.configure(settings).has_value());
        CY_REQUIRE(field.place(1, floor.asset(), Mat4::from_translation(Vec3{0.0F, -0.5F, 1.0F}))
                       .has_value());
        for (u32 index = 0; index < vfx_test::kSteps; ++index) {
            Vec3 centre;
            Vec3 half;
            vfx_test::step_box(index, centre, half);
            steps.push_back(std::make_unique<gi_support::BoxField>(half, 0.5F, 13));
            CY_REQUIRE(
                field.place(10U + index, steps.back()->asset(), Mat4::from_translation(centre))
                    .has_value());
        }
        (void)field.scroll_to(Vec3{0.0F, 0.5F, 1.0F});
    }
};

/// The CPU path's view of the GI field: `DistanceField::distance`, which the device transcribes.
class GiFieldAdapter final : public CollisionField {
public:
    explicit GiFieldAdapter(const rendering::gi::DistanceField& field) noexcept : field_(&field) {}
    [[nodiscard]] f32 distance(Vec3 point) const noexcept override {
        return field_->distance(point);
    }

private:
    const rendering::gi::DistanceField* field_;
};

/// `GpuGiScene::field_binding`, as the VFX pass takes it.
[[nodiscard]] GpuCollisionField collision_field(const rendering::gi_gpu::FieldBinding& binding) {
    GpuCollisionField field;
    field.page_table = binding.page_table;
    field.bricks = binding.bricks;
    field.levels = binding.levels;
    field.window_bricks = binding.window_bricks;
    for (u32 level = 0; level < binding.levels && level < kGpuCollisionMaxLevels; ++level) {
        field.level[level].voxel_size = binding.level[level].voxel_size;
        field.level[level].brick_size = binding.level[level].brick_size;
        field.level[level].far_distance = binding.level[level].far_distance;
        for (u32 axis = 0; axis < 3U; ++axis) {
            field.level[level].origin_brick[axis] = binding.level[level].origin_brick[axis];
        }
    }
    return field;
}

/// A depth buffer on the device: a texture the frame writes from a staging copy each frame, which
/// is how the case gets a known depth into a sampled image without a raster pass.
class DeviceDepth {
public:
    DeviceDepth(rhi::Device& device, const vfx_test::DepthScene& scene) : device_(&device) {
        rhi::TextureDescription texture;
        texture.name = "collision depth";
        texture.format = rhi::Format::R32Sfloat;
        texture.extent = rhi::Extent3D{scene.view.width, scene.view.height, 1};
        texture.usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::TransferDestination;
        auto made = device.create_texture(texture);
        CY_REQUIRE(made.has_value());
        texture_ = *made;
        rhi::BufferDescription staging;
        staging.name = "collision depth staging";
        staging.size = scene.depths.size() * sizeof(f32);
        staging.usage = rhi::BufferUsage::TransferSource;
        staging.memory = rhi::MemoryUse::Upload;
        auto buffer = device.create_buffer(staging);
        CY_REQUIRE(buffer.has_value());
        staging_ = *buffer;
        void* mapped = device.buffer_mapped_pointer(staging_);
        CY_REQUIRE(mapped != nullptr);
        std::memcpy(mapped, scene.depths.data(), scene.depths.size() * sizeof(f32));
        width_ = scene.view.width;
        height_ = scene.view.height;
        depth_.view_projection = scene.view.view_projection;
        depth_.inverse_view_projection = scene.view.inverse_view_projection;
        depth_.camera = scene.view.camera;
        depth_.width = width_;
        depth_.height = height_;
    }

    ~DeviceDepth() {
        (void)device_->wait_idle();
        device_->destroy_buffer(staging_);
        device_->destroy_texture(texture_);
    }

    DeviceDepth(const DeviceDepth&) = delete;
    DeviceDepth& operator=(const DeviceDepth&) = delete;
    DeviceDepth(DeviceDepth&&) = delete;
    DeviceDepth& operator=(DeviceDepth&&) = delete;

    /// Import the texture into this frame's graph and fill it: the depth the pass will read.
    [[nodiscard]] const GpuCollisionDepth& declare(rendering::RenderGraph& graph) noexcept {
        rendering::TextureRequest request;
        request.name = "collision depth";
        request.format = rhi::Format::R32Sfloat;
        request.width = width_;
        request.height = height_;
        request.extra_usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::TransferDestination;
        depth_.texture = graph.import_texture(request, texture_, rhi::ImageUse::Undefined);
        graph.add_pass("collision depth upload", rhi::QueueKind::Graphics)
            .write(depth_.texture, rhi::Access::TransferWrite)
            .record(&DeviceDepth::record_upload, this);
        return depth_;
    }

private:
    static void record_upload(const rendering::PassContext& context, void* user) noexcept {
        auto* self = static_cast<DeviceDepth*>(user);
        rhi::BufferTextureCopy region;
        region.texture_extent = rhi::Extent3D{self->width_, self->height_, 1};
        context.commands->copy_buffer_to_texture(self->staging_,
                                                 context.executor->texture(self->depth_.texture),
                                                 Span<const rhi::BufferTextureCopy>(&region, 1));
    }

    rhi::Device* device_;
    rhi::TextureHandle texture_;
    rhi::BufferHandle staging_;
    u32 width_ = 0;
    u32 height_ = 0;
    GpuCollisionDepth depth_;
};

/// What one frame collides with on the device.
struct DeviceScene {
    const GpuCollisionField* field = nullptr;
    DeviceDepth* depth = nullptr;
};

/// One sub-step on the device: the collision inputs, the step, the graph, drained.
[[nodiscard]] bool device_frame(DeviceFixture& gpu, VfxGpuPass& pass, const DeviceScene& scene,
                                const GpuStepInputs& inputs) noexcept {
    if (!gpu.device().begin_frame().has_value()) {
        return false;
    }
    rendering::RenderGraph graph(allocator());
    GpuCollisionInputs collision;
    collision.field = scene.field;
    if (scene.depth != nullptr) {
        collision.depth = &scene.depth->declare(graph);
    }
    if (pass.collides() && !pass.set_collision(collision).has_value()) {
        return false;
    }
    if (!pass.step(inputs).has_value() || !pass.declare(graph).has_value()) {
        return false;
    }
    rendering::GraphExecutor executor(allocator(), gpu.device());
    auto executed =
        executor.execute(graph, rendering::CompileOptions{}, rendering::ExecuteOptions{});
    if (!executed.has_value()) {
        std::fprintf(stderr, "execute failed: %s\n", executed.error().message);
        return false;
    }
    return gpu.device().wait_idle().has_value() && gpu.device().end_frame().has_value();
}

/// One scenario on both paths.
struct Comparison {
    u32 compared = 0;
    u32 liveness_mismatches = 0;
    u32 disagreeing = 0;
    u32 touching = 0;
    f32 worst_position = 0.0F;
    f32 worst_velocity = 0.0F;
    u32 cpu_events = 0;
    u32 gpu_events = 0;
};

/// One scenario, run on both paths and compared.
struct PathRun {
    DropScenario scenario;
    /// What the CPU path runs instead, when the case compares the device against a different
    /// authoring — a muted collider, say. Same layout; null runs `scenario` on both.
    const DropScenario* cpu_scenario = nullptr;
    const CollisionField* cpu_field = nullptr;
    const DepthCollisionView* cpu_depth = nullptr;
    DeviceScene device;
    u32 steps = 120;
    /// The device's collision quality. The CPU path runs at the authored quality.
    u8 collision_quality = 3;
    f32 position_tolerance = kPositionTolerance;
    f32 velocity_tolerance = kVelocityTolerance;
};

[[nodiscard]] Comparison compare_paths(DeviceFixture& gpu, const PathRun& run) {
    Comparison result;
    graph::DiagnosticSink sink(allocator());
    CompileReport report(allocator());
    auto system = vfx_test::cook_drop(allocator(), run.scenario, sink, report);
    CY_REQUIRE(system.has_value());
    auto cpu_system = vfx_test::cook_drop(
        allocator(), run.cpu_scenario != nullptr ? *run.cpu_scenario : run.scenario, sink, report);
    CY_REQUIRE(cpu_system.has_value());
    const DeviceScene& device_scene = run.device;

    VfxGpuPass pass;
    GpuPassDescription description;
    description.read_back = true;
    const Status created = pass.create(allocator(), gpu.device(), *system, description);
    if (!created) {
        std::fprintf(stderr, "VfxGpuPass::create refused: %s\n", created.error().message);
    }
    CY_REQUIRE(created.has_value());
    CY_REQUIRE(run_frame(gpu, allocator(), pass, true));

    SimulationWorld world(allocator());
    CY_REQUIRE(world.initialize(WorldDescription{}).has_value());
    world.set_collision(run.cpu_field, run.cpu_depth);
    EffectSpawn spawn;
    spawn.simulation_hz = 1.0F / kSubstep;
    CY_REQUIRE(world.play(*cpu_system, spawn).has_value());
    const u32 capacity = system->emitters()[0].capacity();
    std::vector<u8> touched(capacity, 0);

    BudgetController controller;
    BudgetLevers levers = controller.levers_for(system->importance(), system->scalability());
    levers.collision_quality = run.collision_quality;
    for (u32 step = 0; step < run.steps; ++step) {
        GpuStepInputs inputs;
        inputs.dt = kSubstep;
        // The world ages an instance before it simulates the sub-step; the device is told the same.
        inputs.emitter_age = static_cast<f32>(step + 1U) * kSubstep;
        inputs.levers = levers;
        CY_REQUIRE(device_frame(gpu, pass, device_scene, inputs));
        CY_REQUIRE(pass.read_back_counts().has_value());
        result.gpu_events += pass.report().events[0];
        StepReport cpu_report;
        CY_REQUIRE(world.step(kSubstep, cpu_report).has_value());
        if (const ChannelReport* impacts = world.events().report(Name::intern("impact"));
            impacts != nullptr) {
            result.cpu_events += impacts->raised;
        }
        // EVER TOUCHED, on the CPU path: a particle that bounced off a sphere and fell past it
        // has met the collider as surely as one resting on it.
        for (u32 slot = 0; slot < capacity; ++slot) {
            touched[slot] = static_cast<u8>(
                touched[slot] | (world.read_attribute(world.instances()[0], 0, slot,
                                                      Name::intern("collided"), 0) == 1.0F
                                     ? 1U
                                     : 0U));
        }
    }

    auto alive = pass.read_back_alive();
    auto block = pass.read_back_particles();
    CY_REQUIRE(alive.has_value());
    CY_REQUIRE(block.has_value());
    const AttributeLayout& layout = system->emitters()[0].layout();
    const EffectInstance& instance = world.instances()[0];
    const Span<const u8> cpu_alive = world.alive_flags(0);
    for (u32 slot = 0; slot < capacity; ++slot) {
        const bool host = cpu_alive[slot] != 0;
        const bool device = (*alive)[slot] != 0;
        result.liveness_mismatches += host == device ? 0U : 1U;
        if (!host || !device) {
            continue;
        }
        ++result.compared;
        f32 position = 0.0F;
        f32 velocity = 0.0F;
        for (u32 axis = 0; axis < 3U; ++axis) {
            position = std::fmax(
                position,
                std::fabs(world.read_attribute(instance, 0, slot, Name::intern("position"), axis) -
                          read_gpu(*block, layout, "position", slot, axis, capacity)));
            velocity = std::fmax(
                velocity,
                std::fabs(world.read_attribute(instance, 0, slot, Name::intern("velocity"), axis) -
                          read_gpu(*block, layout, "velocity", slot, axis, capacity)));
        }
        result.touching += touched[slot];
        result.worst_position = std::fmax(result.worst_position, position);
        result.worst_velocity = std::fmax(result.worst_velocity, velocity);
        const bool disagrees =
            position > run.position_tolerance || velocity > run.velocity_tolerance;
        result.disagreeing += disagrees ? 1U : 0U;
        if (disagrees && std::getenv("CY_VFX_COLLISION_VERBOSE") != nullptr) {
            const auto at = [&](const char* name, u32 axis) {
                return static_cast<double>(
                    world.read_attribute(instance, 0, slot, Name::intern(name), axis));
            };
            const auto dev = [&](const char* name, u32 axis) {
                return static_cast<double>(read_gpu(*block, layout, name, slot, axis, capacity));
            };
            std::fprintf(stderr,
                         "slot %u: cpu p (%.5f %.5f %.5f) v (%.5f %.5f %.5f) c %.0f | gpu p (%.5f "
                         "%.5f %.5f) v (%.5f %.5f %.5f) c %.0f\n",
                         slot, at("position", 0), at("position", 1), at("position", 2),
                         at("velocity", 0), at("velocity", 1), at("velocity", 2), at("collided", 0),
                         dev("position", 0), dev("position", 1), dev("position", 2),
                         dev("velocity", 0), dev("velocity", 1), dev("velocity", 2),
                         dev("collided", 0));
        }
    }
    CY_CHECK_EQ(gpu.validation_errors(), 0U);
    return result;
}

void print(const char* label, const Comparison& result) noexcept {
    std::fprintf(stderr,
                 "%s: %u compared, %u touching, %u liveness mismatches, %u disagreeing; worst "
                 "position %.3e m, velocity %.3e m/s; events cpu %u gpu %u\n",
                 label, result.compared, result.touching, result.liveness_mismatches,
                 result.disagreeing, static_cast<double>(result.worst_position),
                 static_cast<double>(result.worst_velocity), result.cpu_events, result.gpu_events);
}

[[nodiscard]] DropScenario scattered_drop(const char* shape) noexcept {
    DropScenario scenario;
    scenario.shape = shape;
    scenario.capacity = 256;
    scenario.spawn_per_step = 256;
    scenario.radius = 0.03F;
    scenario.start = Vec3{0.0F, 2.5F, 0.0F};
    scenario.spread = Vec3{1.2F, 0.5F, 1.2F};
    scenario.velocity = Vec3{0.0F, 0.0F, 0.0F};
    scenario.velocity_spread = Vec3{1.5F, 0.5F, 1.5F};
    scenario.restitution = 0.5F;
    scenario.friction = 0.3F;
    scenario.rest_speed = 0.3F;
    return scenario;
}

[[nodiscard]] bool ready(DeviceFixture& gpu) noexcept {
    if (!gpu.has_gpu()) {
        gpu.report_skip();
        return false;
    }
    if (!kKernelCompilerAvailable) {
        std::fprintf(stderr,
                     "no Slang front end in this build; the generated kernel cannot be "
                     "compiled here\n");
        return false;
    }
    return true;
}

}  // namespace

CY_TEST_CASE("the device collides with the analytic primitives as the CPU executor does") {
    DeviceFixture gpu("cy_test_render_vfx_collision_analytic", false);
    if (!ready(gpu)) {
        return;
    }
    struct Row {
        const char* shape;
        f32 a[4];
        f32 b[4];
    };
    const Row rows[] = {
        {"plane", {0.0F, 1.0F, 0.0F, 0.0F}, {0.0F, 0.0F, 0.0F, 0.0F}},
        {"sphere", {0.2F, 0.0F, -0.1F, 1.0F}, {0.0F, 0.0F, 0.0F, 0.0F}},
        {"box", {0.0F, 0.4F, 0.0F, 0.0F}, {0.8F, 0.4F, 0.6F, 0.0F}},
        {"capsule", {-1.0F, 0.5F, 0.0F, 0.5F}, {1.0F, 0.3F, 0.2F, 0.0F}},
    };
    for (const Row& row : rows) {
        DropScenario scenario = scattered_drop(row.shape);
        for (u32 index = 0; index < 4; ++index) {
            scenario.a[index] = row.a[index];
            scenario.b[index] = row.b[index];
        }
        PathRun run;
        run.scenario = scenario;
        const Comparison result = compare_paths(gpu, run);
        print(row.shape, result);
        CY_CHECK_EQ(result.compared, 256U);
        CY_CHECK_EQ(result.liveness_mismatches, 0U);
        CY_CHECK_EQ(result.disagreeing, 0U);
        // Long enough that the collider has been met: the sphere, box and capsule shed some
        // particles to the void below, which is what makes this more than a plane check.
        CY_CHECK_GT(result.touching, 0U);
    }
}

CY_TEST_CASE("the device collides with the GI distance field as the CPU executor does") {
    // THE SCENE FIELD ON BOTH PATHS IS ONE FIELD: the CPU reads `DistanceField::distance`, the
    // device reads the GI scene's own page table and brick pool through `field_binding()` — bound,
    // not copied — and the two land on the same treads.
    DeviceFixture gpu("cy_test_render_vfx_collision_field", false);
    if (!ready(gpu)) {
        return;
    }
    GiStairs stairs;
    GiFieldAdapter adapter(stairs.field);
    rendering::gi_gpu::GpuGiScene scene;
    CY_REQUIRE(scene.create(allocator(), gpu.device(), rendering::gi_gpu::GpuGiSceneDescription{})
                   .has_value());
    CY_REQUIRE(scene.upload_field(stairs.field).has_value());
    const GpuCollisionField field = collision_field(scene.field_binding());
    CY_REQUIRE_EQ(field.levels, 2U);

    DropScenario scenario = scattered_drop("sdf");
    scenario.start = Vec3{0.0F, 2.0F, 1.0F};
    scenario.spread = Vec3{1.2F, 0.3F, 0.9F};
    PathRun run;
    run.scenario = scenario;
    run.cpu_field = &adapter;
    run.device.field = &field;
    run.position_tolerance = kFieldPositionTolerance;
    run.velocity_tolerance = kFieldVelocityTolerance;
    const Comparison result = compare_paths(gpu, run);
    print("sdf", result);
    CY_CHECK_EQ(result.compared, 256U);
    CY_CHECK_EQ(result.liveness_mismatches, 0U);
    CY_CHECK_EQ(result.disagreeing, 0U);
    CY_CHECK_GT(result.touching, 128U);

    // CONTROL: no field bound on the device, and every particle falls through the stairs there
    // while the CPU still lands them — the comparison sees a missing binding.
    run.device.field = nullptr;
    const Comparison unbound = compare_paths(gpu, run);
    print("sdf unbound", unbound);
    CY_CHECK_GT(unbound.disagreeing, 128U);
}

CY_TEST_CASE("the device collides with the depth buffer as the CPU executor does") {
    DeviceFixture gpu("cy_test_render_vfx_collision_depth", false);
    if (!ready(gpu)) {
        return;
    }
    vfx_test::StairsField stairs;
    const vfx_test::DepthScene seen =
        vfx_test::render_depth(stairs, Vec3{0.0F, 3.0F, -2.5F}, Vec3{0.0F, 0.0F, 1.0F}, 320, 180);
    DeviceDepth depth(gpu.device(), seen);
    DropScenario scenario = scattered_drop("depth");
    scenario.start = Vec3{0.0F, 2.0F, 1.0F};
    scenario.spread = Vec3{1.0F, 0.3F, 0.8F};
    PathRun run;
    run.scenario = scenario;
    run.cpu_depth = &seen.view;
    run.device.depth = &depth;
    const Comparison result = compare_paths(gpu, run);
    print("depth", result);
    CY_CHECK_EQ(result.compared, 256U);
    CY_CHECK_EQ(result.liveness_mismatches, 0U);
    CY_CHECK_EQ(result.disagreeing, 0U);
    CY_CHECK_GT(result.touching, 128U);
}

CY_TEST_CASE("the device raises one impact event per contact, as the CPU executor does") {
    DeviceFixture gpu("cy_test_render_vfx_collision_events", false);
    if (!ready(gpu)) {
        return;
    }
    DropScenario scenario = scattered_drop("plane");
    scenario.a[0] = 0.0F;
    scenario.a[1] = 1.0F;
    scenario.a[2] = 0.0F;
    scenario.a[3] = 0.0F;
    scenario.raise_impacts = true;
    PathRun run;
    run.scenario = scenario;
    run.steps = 150;
    const Comparison result = compare_paths(gpu, run);
    print("events", result);
    CY_CHECK_GT(result.cpu_events, 256U);
    CY_CHECK_EQ(result.gpu_events, result.cpu_events);
    CY_CHECK_EQ(result.disagreeing, 0U);
}

CY_TEST_CASE("the budget's collision quality reaches the device") {
    // QUALITY ZERO ON THE DEVICE IS NO COLLIDER: compared against the CPU path running the same
    // effect with its collider muted, the two agree particle for particle — and against the CPU
    // path at the authored quality, every particle disagrees.
    DeviceFixture gpu("cy_test_render_vfx_collision_quality", false);
    if (!ready(gpu)) {
        return;
    }
    DropScenario muted = scattered_drop("plane");
    muted.mute_collider = true;
    PathRun run;
    run.scenario = scattered_drop("plane");
    run.cpu_scenario = &muted;
    run.steps = 90;
    run.collision_quality = 0;
    const Comparison off = compare_paths(gpu, run);
    print("quality 0 against a muted collider", off);
    CY_CHECK_EQ(off.compared, 256U);
    CY_CHECK_EQ(off.disagreeing, 0U);

    run.cpu_scenario = nullptr;
    const Comparison authored = compare_paths(gpu, run);
    print("quality 0 against the authored collider", authored);
    CY_CHECK_EQ(authored.disagreeing, 256U);
}

CY_TEST_CASE("a depth set for one frame is not bound in the next") {
    // REGRESSION. The depth is a resource of ONE graph; a pass that kept it across frames bound the
    // previous graph's resource identifier into the next graph, where it names something else.
    // A frame that does not call `set_collision` again must collide with no depth, cleanly.
    DeviceFixture gpu("cy_test_render_vfx_collision_stale_depth", false);
    if (!ready(gpu)) {
        return;
    }
    vfx_test::StairsField stairs;
    const vfx_test::DepthScene seen =
        vfx_test::render_depth(stairs, Vec3{0.0F, 3.0F, -2.5F}, Vec3{0.0F, 0.0F, 1.0F}, 64, 36);
    DeviceDepth depth(gpu.device(), seen);
    DropScenario scenario = scattered_drop("depth");
    graph::DiagnosticSink sink(allocator());
    CompileReport report(allocator());
    auto system = vfx_test::cook_drop(allocator(), scenario, sink, report);
    CY_REQUIRE(system.has_value());
    VfxGpuPass pass;
    GpuPassDescription description;
    description.read_back = true;
    CY_REQUIRE(pass.create(allocator(), gpu.device(), *system, description).has_value());
    CY_REQUIRE(run_frame(gpu, allocator(), pass, true));
    BudgetController controller;
    GpuStepInputs inputs;
    inputs.dt = kSubstep;
    inputs.levers = controller.levers_for(system->importance(), system->scalability());
    DeviceScene with_depth;
    with_depth.depth = &depth;
    inputs.emitter_age = kSubstep;
    CY_REQUIRE(device_frame(gpu, pass, with_depth, inputs));
    for (u32 step = 1; step < 4; ++step) {
        inputs.emitter_age = static_cast<f32>(step + 1U) * kSubstep;
        CY_REQUIRE(gpu.device().begin_frame().has_value());
        // No depth in this graph: the identifier the first frame's depth had now names the pass's
        // own particle block.
        rendering::RenderGraph graph(allocator());
        CY_REQUIRE(pass.step(inputs).has_value());
        CY_REQUIRE(pass.declare(graph).has_value());
        rendering::GraphExecutor executor(allocator(), gpu.device());
        CY_REQUIRE(executor.execute(graph, rendering::CompileOptions{}, rendering::ExecuteOptions{})
                       .has_value());
        CY_REQUIRE(gpu.device().wait_idle().has_value());
        CY_REQUIRE(gpu.device().end_frame().has_value());
    }
    CY_CHECK_EQ(gpu.validation_errors(), 0U);
}

CY_TEST_CASE("a GPU pass refuses an emitter that spawns from events, by name") {
    DeviceFixture gpu("cy_test_render_vfx_collision_refusal", false);
    if (!ready(gpu)) {
        return;
    }
    DropScenario scenario = scattered_drop("plane");
    scenario.raise_impacts = true;
    scenario.sparks = true;
    graph::DiagnosticSink sink(allocator());
    CompileReport report(allocator());
    auto system = vfx_test::cook_drop(allocator(), scenario, sink, report);
    CY_REQUIRE(system.has_value());
    VfxGpuPass pass;
    GpuPassDescription description;
    description.emitter = 1;  // the sparks
    const Status created = pass.create(allocator(), gpu.device(), *system, description);
    CY_REQUIRE_FALSE(created.has_value());
    CY_CHECK(std::strstr(created.error().message, "vfx.event_count") != nullptr);
}

namespace {

struct Timing {
    /// Spawn to the end of the update.
    f64 step_ms = -1.0;
    /// The update dispatch alone.
    f64 update_ms = -1.0;
};

/// The device time of one sub-step, averaged over `steps` after `warm` steps to settle, for a
/// colliding effect of `capacity` particles.
[[nodiscard]] Timing timed_step(DeviceFixture& gpu, DropScenario scenario, const DeviceScene& scene,
                                u32 capacity, u32 warm, u32 steps) {
    scenario.capacity = capacity;
    scenario.spawn_per_step = capacity;
    graph::DiagnosticSink sink(allocator());
    CompileReport report(allocator());
    auto system = vfx_test::cook_drop(allocator(), scenario, sink, report);
    CY_REQUIRE(system.has_value());
    VfxGpuPass pass;
    GpuPassDescription description;
    description.timed = true;
    CY_REQUIRE(pass.create(allocator(), gpu.device(), *system, description).has_value());
    CY_REQUIRE(run_frame(gpu, allocator(), pass, true));
    BudgetController controller;
    BudgetLevers levers = controller.levers_for(system->importance(), system->scalability());
    levers.sorted = false;
    f64 total = 0.0;
    f64 update = 0.0;
    u32 measured = 0;
    for (u32 step = 0; step < warm + steps; ++step) {
        GpuStepInputs inputs;
        inputs.dt = kSubstep;
        inputs.emitter_age = static_cast<f32>(step + 1U) * kSubstep;
        inputs.levers = levers;
        CY_REQUIRE(device_frame(gpu, pass, scene, inputs));
        const f64 ms = pass.last_device_ms();
        if (step >= warm && ms >= 0.0) {
            total += ms;
            update += pass.last_update_ms();
            ++measured;
        }
    }
    Timing timing;
    if (measured != 0) {
        timing.step_ms = total / static_cast<f64>(measured);
        timing.update_ms = update / static_cast<f64>(measured);
    }
    return timing;
}

}  // namespace

CY_TEST_CASE("a colliding sub-step is timed on the device") {
    // ALWAYS: a small effect against the plane, timed — the diagnostic answers a time. WITH
    // `CY_VFX_COLLISION_TIMING`: the table the PR and the README quote, 65 536 particles against
    // nothing, the plane, the scene field and a 1920 x 1080 depth buffer.
    DeviceFixture gpu("cy_test_render_vfx_collision_timing", false);
    if (!ready(gpu)) {
        return;
    }
    DropScenario plane = scattered_drop("plane");
    const Timing small = timed_step(gpu, plane, {}, 1024, 4, 8);
    std::fprintf(stderr, "1 024 particles against the plane: %.4f ms a sub-step, update %.4f ms\n",
                 small.step_ms, small.update_ms);
    if (!gpu.device().capabilities().has(rhi::Capability::TimestampQueries)) {
        CY_CHECK_LT(small.step_ms, 0.0);
        return;
    }
    CY_CHECK_GT(small.step_ms, 0.0);
    CY_CHECK_GT(small.update_ms, 0.0);
    CY_CHECK_LE(small.update_ms, small.step_ms);
    if (std::getenv("CY_VFX_COLLISION_TIMING") == nullptr) {
        return;
    }

    constexpr u32 kParticles = 65536;
    GiStairs stairs;
    rendering::gi_gpu::GpuGiScene scene;
    CY_REQUIRE(scene.create(allocator(), gpu.device(), rendering::gi_gpu::GpuGiSceneDescription{})
                   .has_value());
    CY_REQUIRE(scene.upload_field(stairs.field).has_value());
    const GpuCollisionField field = collision_field(scene.field_binding());
    const vfx_test::DepthScene floor =
        vfx_test::render_floor_depth(Vec3{0.0F, 3.0F, -3.0F}, Vec3{0.0F, 0.0F, 1.0F}, 1920, 1080);
    DeviceDepth depth(gpu.device(), floor);

    DropScenario muted = scattered_drop("plane");
    muted.mute_collider = true;
    DeviceScene with_field;
    with_field.field = &field;
    DeviceScene with_depth;
    with_depth.depth = &depth;
    const Timing none = timed_step(gpu, muted, {}, kParticles, 30, 60);
    const Timing analytic = timed_step(gpu, plane, {}, kParticles, 30, 60);
    const Timing sdf = timed_step(gpu, scattered_drop("sdf"), with_field, kParticles, 30, 60);
    const Timing screen = timed_step(gpu, scattered_drop("depth"), with_depth, kParticles, 30, 60);
    std::fprintf(stderr,
                 "65 536 particles, ms (whole sub-step / update dispatch): no collider %.4f / "
                 "%.4f, plane %.4f / %.4f, scene field %.4f / %.4f, depth 1920x1080 %.4f / %.4f\n",
                 none.step_ms, none.update_ms, analytic.step_ms, analytic.update_ms, sdf.step_ms,
                 sdf.update_ms, screen.step_ms, screen.update_ms);
    CY_CHECK_GT(sdf.update_ms, 0.0);
    CY_CHECK_GT(screen.update_ms, 0.0);
}
