// SPDX-License-Identifier: MIT
// Collision, photographed: sparks bouncing down a flight of stairs, and debris settling on it.
// `render.vfx_collision`, Vulkan.
//
// The stairs are drawn by the pipeline suites' own frame (`frame_scene.h`), five of its boxes
// shaped as steps on its floor slab, and collided with as `collision_scene.h`'s exact field of the
// same boxes. The particles are simulated by `SimulationWorld` — the CPU executor, which
// `render.vfx_collision_gpu` holds the device to particle for particle — published through
// `publish_sprites` and drawn in the frame's transparent stage.
//
// Each case asserts what the picture is of rather than comparing it with a golden: the sparks are
// in the frame (a frame without them differs), and the debris has come to REST on the stairs —
// every piece one radius from a surface of the field and still. The pictures are written beside the
// run; `just capture-vfx-collision` copies them into docs/design/images and films the sparks.

#include "collision_effects.h"
#include "collision_scene.h"
#include "frame_scene.h"
#include "golden.h"

#include <cy/backends/rhi/backend.h>
#include <cy/backends/rhi/null/null_device.h>
#include <cy/backends/rhi/vulkan/vulkan_backend.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/test/test.h>
#include <cy/vfx/runtime.h>
#include <cy/vfx/world.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace cy;
using namespace cy::vfx;
using namespace cy::vfx_test;
using cy::pipeline_test::FrameScene;
using cy::pipeline_test::FrameSceneHooks;
using cy::pipeline_test::kHeight;
using cy::pipeline_test::kMaterialColours;
using cy::pipeline_test::kMaterialCount;
using cy::pipeline_test::kWidth;
using cy::pipeline_test::RecordMode;
namespace particles = cy::rendering::particles;
namespace assembly = cy::rendering::assembly;

namespace {

Allocator& allocator() noexcept {
    return system_allocator(MemoryDomain::Gpu);
}

constexpr f32 kStep = 1.0F / 60.0F;
constexpr u32 kRing = 4096;
/// The camera, three quarters on to the stairs and above them.
constexpr Vec3 kEye{2.9F, 2.5F, -2.4F};
constexpr Vec3 kTarget{0.0F, 0.55F, 1.0F};

class Device {
public:
    explicit Device(const char* name) noexcept {
        (void)rhi::vulkan::register_vulkan_backend();
        (void)rhi::null::register_null_backend();
        rhi::DeviceDescription description;
        description.application_name = name;
        description.enable_validation = true;
        device_ = rhi::create_device(allocator(), "vulkan", description, selection_);
    }
    ~Device() {
        if (device_.has_value()) {
            (void)device_.value()->wait_idle();
            rhi::destroy_device(allocator(), device_.value());
        }
    }
    Device(const Device&) = delete;
    Device& operator=(const Device&) = delete;
    Device(Device&&) = delete;
    Device& operator=(Device&&) = delete;

    [[nodiscard]] bool usable() const noexcept {
        return device_.has_value() &&
               device_.value()->capabilities().backend() == rhi::BackendKind::Vulkan;
    }
    [[nodiscard]] rhi::Device& device() const noexcept { return *device_.value(); }
    void report_skip() const noexcept {
        std::fprintf(stderr, "no requested graphics device on this machine; selected '%s': %s\n",
                     selection_.selected != nullptr ? selection_.selected : "(none)",
                     selection_.reason != nullptr ? selection_.reason : "(no reason given)");
    }

private:
    rhi::BackendSelection selection_{};
    Expected<rhi::Device*, Error> device_ = fail(ErrorCode::Unavailable, "not created");
};

/// The stairs in the frame: box 0 is the floor slab, boxes 1 to 5 the steps, and the rest sunk out
/// of sight inside the slab. Stone-grey whatever material a box was given.
struct Stairs {
    Array<particles::ParticleInstance> records{allocator()};

    static void shape(u32 which, Vec3& centre, Vec3& half, void* /*user*/) noexcept {
        if (which == 0) {
            centre = Vec3{0.0F, -0.5F, 1.0F};
            half = Vec3{4.0F, 0.5F, 4.0F};
            return;
        }
        if (which <= kSteps) {
            step_box(which - 1U, centre, half);
            return;
        }
        centre = Vec3{0.0F, -0.6F, 1.0F};
        half = Vec3{0.1F, 0.1F, 0.1F};
    }

    static void tint(u32 which, Vec3& tint, void* /*user*/) noexcept {
        const f32* base = kMaterialColours[which % kMaterialCount];
        // A warm sandstone, a shade darker each step down, against the frame's cool sky light.
        const f32 stone = which == 0 ? 0.5F : 0.78F - (0.04F * static_cast<f32>(which));
        tint = Vec3{stone / base[0], (stone * 0.8F) / base[1], (stone * 0.58F) / base[2]};
    }

    static Span<const particles::ParticleInstance> particles(void* user) noexcept {
        return static_cast<Stairs*>(user)->records.span();
    }

    [[nodiscard]] FrameSceneHooks hooks() noexcept {
        FrameSceneHooks hooks;
        hooks.user = this;
        hooks.shape_box = &Stairs::shape;
        hooks.tint_box = &Stairs::tint;
        hooks.particles = &Stairs::particles;
        hooks.particle_capacity = kRing;
        return hooks;
    }
};

/// One effect, simulated against the stairs' field and drawn in the stairs' frame.
class Shot {
public:
    Shot(Device& gpu, const DropScenario& scenario) noexcept
        : scene_(allocator()), sink_(allocator()), report_(allocator()), world_(allocator()) {
        auto compiled = cook_drop(allocator(), scenario, sink_, report_);
        CY_REQUIRE(compiled.has_value());
        system_ = Expected<CompiledSystem, Error>(std::move(compiled.value()));
        CY_REQUIRE(world_.initialize(WorldDescription{}).has_value());
        world_.set_collision(&field_, nullptr);
        EffectSpawn spawn;
        spawn.simulation_hz = 60.0F;
        CY_REQUIRE(world_.play(system_.value(), spawn).has_value());
        scene_.set_hooks(stairs_.hooks());
        CY_REQUIRE(scene_.build(gpu.device()).has_value());
        scene_.set_camera(kEye, normalize(kTarget - kEye));
        scene_.set_read_back(true);
    }

    void simulate(u32 frames) noexcept {
        for (u32 frame = 0; frame < frames; ++frame) {
            StepReport report;
            CY_REQUIRE(world_.step(kStep, report).has_value());
        }
        PublishReport published;
        CY_REQUIRE(publish_sprites(world_, kEye, kRing, stairs_.records, published).has_value());
        CY_CHECK_EQ(published.dropped, 0U);
    }

    [[nodiscard]] Span<const u32> render(RecordMode mode) noexcept {
        assembly::AssemblyReport report;
        CY_REQUIRE(scene_.render(mode, report).has_value());
        return scene_.pixels();
    }

    [[nodiscard]] SimulationWorld& world() noexcept { return world_; }
    [[nodiscard]] const StairsField& field() const noexcept { return field_; }
    [[nodiscard]] u32 drawn() const noexcept { return static_cast<u32>(stairs_.records.size()); }

private:
    Stairs stairs_;
    StairsField field_;
    FrameScene scene_;
    graph::DiagnosticSink sink_;
    CompileReport report_;
    Expected<CompiledSystem, Error> system_ = fail(ErrorCode::Unavailable, "not cooked");
    SimulationWorld world_;
};

[[nodiscard]] std::string artefact(const char* name) {
    const char* directory = std::getenv("CY_TEST_ARTEFACT_DIR");
    return std::string(directory != nullptr && *directory != '\0' ? directory : ".") + "/" + name;
}

void save(const std::string& path, Span<const u32> pixels) noexcept {
    render_test::Image image(allocator());
    if (render_test::adopt(image, pixels, kWidth, kHeight).has_value() &&
        render_test::write_png(path.c_str(), image).has_value()) {
        std::fprintf(stderr, "wrote %s\n", path.c_str());
    }
}

[[nodiscard]] u32 differing(Span<const u32> a, Span<const u32> b) noexcept {
    u32 count = 0;
    for (usize index = 0; index < a.size() && index < b.size(); ++index) {
        count += a[index] != b[index] ? 1U : 0U;
    }
    return count;
}

/// A fountain of sparks thrown off the top step, bouncing down the treads.
[[nodiscard]] DropScenario sparks() noexcept {
    DropScenario scenario;
    scenario.shape = "sdf";
    scenario.a[0] = 0.0F;
    scenario.capacity = 2048;
    scenario.spawn_per_step = 6;
    scenario.spawn_until = 1.0e6F;
    scenario.lifetime = 2.6F;
    scenario.start = Vec3{0.0F, 1.45F, 1.85F};
    scenario.spread = Vec3{0.3F, 0.05F, 0.05F};
    scenario.velocity = Vec3{0.0F, 1.0F, -1.6F};
    scenario.velocity_spread = Vec3{1.3F, 0.9F, 0.7F};
    scenario.radius = 0.012F;
    scenario.restitution = 0.55F;
    scenario.friction = 0.15F;
    scenario.rest_speed = 0.05F;
    scenario.size = 0.022F;
    scenario.tint = Vec3{1.0F, 0.38F, 0.06F};
    scenario.emission = 12000.0F;
    return scenario;
}

/// A burst of debris thrown over the stairs, landing and coming to rest on them.
[[nodiscard]] DropScenario debris() noexcept {
    DropScenario scenario;
    scenario.shape = "sdf";
    scenario.a[0] = 0.0F;
    scenario.capacity = 320;
    scenario.spawn_per_step = 320;
    scenario.spawn_until = 0.02F;
    scenario.start = Vec3{0.0F, 2.6F, 0.9F};
    scenario.spread = Vec3{1.1F, 0.4F, 1.0F};
    scenario.velocity_spread = Vec3{0.8F, 0.6F, 0.8F};
    scenario.radius = 0.035F;
    scenario.restitution = 0.3F;
    scenario.friction = 0.7F;
    scenario.rest_speed = 0.6F;
    scenario.size = 0.09F;
    scenario.tint = Vec3{0.75F, 0.62F, 0.5F};
    scenario.emission = 1800.0F;
    return scenario;
}

}  // namespace

CY_TEST_CASE("sparks bounce down the stairs, and the picture shows them") {
    Device gpu("cy_test_render_vfx_collision_sparks");
    if (!gpu.usable()) {
        gpu.report_skip();
        return;
    }
    Shot shot(gpu, sparks());
    // FRAMES FOR THE FILM, when the capture recipe asks for them.
    const char* frames = std::getenv("CY_VFX_COLLISION_FRAMES");
    const u32 total = 150;
    if (frames != nullptr && *frames != '\0') {
        for (u32 frame = 0; frame < 300; ++frame) {
            shot.simulate(1);
            const Span<const u32> pixels = shot.render(RecordMode::CallbacksAndParticles);
            char name[64];
            std::snprintf(name, sizeof(name), "/sparks-%04u.png", frame);
            save(std::string(frames) + name, pixels);
        }
    } else {
        shot.simulate(total);
    }
    const Span<const u32> with = shot.render(RecordMode::CallbacksAndParticles);
    save(artefact("vfx-collision-sparks.png"), with);
    std::vector<u32> lit(with.begin(), with.end());
    const Span<const u32> without = shot.render(RecordMode::Callbacks);
    const u32 changed = differing(Span<const u32>(lit.data(), lit.size()), without);
    std::fprintf(stderr, "sparks: %u drawn, %u of %u texels differ from the stairs alone\n",
                 shot.drawn(), changed, kWidth * kHeight);
    CY_CHECK_GT(shot.drawn(), 500U);
    CY_CHECK_GT(changed, 1000U);
}

CY_TEST_CASE("debris settles on the stairs: every piece at rest, one radius from a surface") {
    Device gpu("cy_test_render_vfx_collision_debris");
    if (!gpu.usable()) {
        gpu.report_skip();
        return;
    }
    const DropScenario scenario = debris();
    Shot shot(gpu, scenario);
    const char* frames = std::getenv("CY_VFX_COLLISION_FRAMES");
    if (frames != nullptr && *frames != '\0') {
        for (u32 frame = 0; frame < 240; ++frame) {
            shot.simulate(1);
            char name[64];
            std::snprintf(name, sizeof(name), "/debris-%04u.png", frame);
            save(std::string(frames) + name, shot.render(RecordMode::CallbacksAndParticles));
        }
    } else {
        shot.simulate(240);
    }
    save(artefact("vfx-collision-debris.png"), shot.render(RecordMode::CallbacksAndParticles));

    // AT REST, ON A SURFACE. Every piece is still and sits one radius from the field — on a tread,
    // against a riser, or on the floor — and none is inside the stairs.
    const EffectInstance& instance = shot.world().instances()[0];
    const Name position = Name::intern("position");
    const Name velocity = Name::intern("velocity");
    u32 resting = 0;
    f32 worst_gap = 0.0F;
    f32 fastest = 0.0F;
    for (u32 slot = 0; slot < scenario.capacity; ++slot) {
        const Vec3 at{shot.world().read_attribute(instance, 0, slot, position, 0),
                      shot.world().read_attribute(instance, 0, slot, position, 1),
                      shot.world().read_attribute(instance, 0, slot, position, 2)};
        const Vec3 moving{shot.world().read_attribute(instance, 0, slot, velocity, 0),
                          shot.world().read_attribute(instance, 0, slot, velocity, 1),
                          shot.world().read_attribute(instance, 0, slot, velocity, 2)};
        const f32 gap = shot.field().distance(at) - scenario.radius;
        worst_gap = std::max(worst_gap, std::fabs(gap));
        fastest = std::max(fastest, length(moving));
        resting += (std::fabs(gap) < 2.0e-3F && length(moving) < 1.0e-3F) ? 1U : 0U;
    }
    std::fprintf(stderr, "debris: %u of %u at rest; worst gap %.2e m, fastest %.2e m/s\n", resting,
                 scenario.capacity, static_cast<double>(worst_gap), static_cast<double>(fastest));
    CY_CHECK_EQ(resting, scenario.capacity);
}
