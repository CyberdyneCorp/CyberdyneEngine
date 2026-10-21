// SPDX-License-Identifier: MIT
// render.fog_of_war — fog of war on a device, through the assembled frame.
//
// `pipeline_test::FrameScene` shaped into a small strategy field — a 32 x 24 m ground, two blue
// scouts, three red units and a red barracks — seen from above, with blue's fog in the frame. The
// grid is `cy::gameplay::fog::FogOfWar`'s, updated by two fixed steps of the scouts' patrol; the
// renderer only reads it. What is asserted:
//
//   (a) the visibility texture on the device is the gameplay grid, texel for texel;
//   (b) every pixel the dispatch wrote is the host twin's: `shade_fog` of the colour it read at
//       `sample_visibility` of the world position its depth reconstructs, and a visible pixel is
//       the colour it read, bit for bit;
//   (c) in the picture, visible ground is the frame without fog, explored ground is darker and
//       greyer, and ground no blue unit ever saw is black;
//   (d) the edges are smooth: no two neighbouring pixels across a boundary differ by more than the
//       ramp allows, where a hard-edged fog would jump the whole contrast in one pixel;
//   (e) red units outside blue's vision are not drawn, the barracks blue saw is drawn as its ghost
//       where it stood, and a red unit inside vision is drawn as it is;
//   (f) off is the frame before: the stage absent is byte for byte the committed reference, and
//       the stage present with blue seeing everything is byte for byte the stage absent;
//   (g) the fade: a cell revealed this frame is part way between explored and visible.
//
// Vulkan with validation and synchronisation validation on. Without a GPU the suite SKIPS LOUDLY.
// It writes its pictures to CY_TEST_ARTEFACT_DIR (or the build directory).

#include "frame_scene.h"
#include "golden.h"

#include <cy/backends/rhi/backend.h>
#include <cy/backends/rhi/null/null_device.h>
#include <cy/backends/rhi/validation.h>
#include <cy/backends/rhi/vulkan/vulkan_backend.h>
#include <cy/core/math/projection.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/gameplay/fog/fog_of_war.h>
#include <cy/rendering/fog_of_war/fog_of_war_pass.h>
#include <cy/rendering/fog_of_war/minimap.h>
#include <cy/test/test.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

using namespace cy;
using namespace cy::pipeline_test;
using namespace cy::rendering::fog_of_war;
namespace gfog = cy::gameplay::fog;

namespace {

Allocator& allocator() noexcept {
    return system_allocator(MemoryDomain::Renderer);
}

void count_validation(rhi::ValidationSeverity severity, const char* message, void* user) noexcept {
    if (severity == rhi::ValidationSeverity::Error && user != nullptr) {
        ++*static_cast<u32*>(user);
    }
    std::fprintf(stderr, "graphics validation %s: %s\n",
                 severity == rhi::ValidationSeverity::Error ? "error" : "warning",
                 message != nullptr ? message : "");
}

class DeviceFixture {
public:
    DeviceFixture() noexcept : allocator_(system_allocator(MemoryDomain::Gpu)) {
        (void)rhi::vulkan::register_vulkan_backend();
        (void)rhi::null::register_null_backend();
        rhi::DeviceDescription description;
        description.application_name = "cy_test_render_fog_of_war";
        description.enable_validation = true;
        description.enable_synchronisation_validation = true;
        device_ = rhi::create_device(allocator_, "vulkan", description, selection_);
        if (device_.has_value()) {
            device_.value()->set_validation_callback(&count_validation, &errors_);
        }
    }

    ~DeviceFixture() {
        if (device_.has_value()) {
            (void)device_.value()->wait_idle();
            rhi::destroy_device(allocator_, device_.value());
        }
    }

    DeviceFixture(const DeviceFixture&) = delete;
    DeviceFixture& operator=(const DeviceFixture&) = delete;

    [[nodiscard]] bool has_gpu() const noexcept {
        return device_.has_value() &&
               device_.value()->capabilities().backend() == rhi::BackendKind::Vulkan;
    }
    [[nodiscard]] rhi::Device& device() const noexcept { return *device_.value(); }
    [[nodiscard]] u32 validation_errors() const noexcept { return errors_; }

    void report_skip() const noexcept {
        std::fprintf(stderr,
                     "no requested graphics device on this machine; the backend selected was '%s' "
                     "because %s\n",
                     selection_.selected != nullptr ? selection_.selected : "(none)",
                     selection_.reason != nullptr ? selection_.reason : "(no reason given)");
    }

private:
    Allocator& allocator_;
    rhi::BackendSelection selection_{};
    u32 errors_ = 0;
    Expected<rhi::Device*, Error> device_ = fail(ErrorCode::Unavailable, "not created");
};

[[nodiscard]] f32 from_half(u16 half) noexcept {
    const u32 sign = (static_cast<u32>(half) & 0x8000U) << 16U;
    const u32 exponent = (static_cast<u32>(half) >> 10U) & 0x1FU;
    const u32 mantissa = static_cast<u32>(half) & 0x3FFU;
    u32 bits = 0;
    if (exponent == 0) {
        if (mantissa == 0) {
            bits = sign;
        } else {
            const f32 value = std::ldexp(static_cast<f32>(mantissa), -24);
            return sign != 0 ? -value : value;
        }
    } else if (exponent == 0x1F) {
        bits = sign | 0x7F800000U | (mantissa << 13U);
    } else {
        bits = sign | ((exponent + 112U) << 23U) | (mantissa << 13U);
    }
    f32 value = 0.0F;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

// --- The field ---------------------------------------------------------------------------------

/// The ground: 32 x 24 m, its top at y = 0, from x = -16 to 16 and z = -24 to 0.
constexpr Vec3 kGroundCentre{0.0F, -0.25F, -12.0F};
constexpr Vec3 kGroundHalf{16.0F, 0.25F, 12.0F};
constexpr u32 kGridWidth = 32;
constexpr u32 kGridHeight = 24;
constexpr FogGridPlacement kPlacement{-16.0F, -24.0F, 1.0F};
constexpr Vec3 kEye{0.0F, 16.0F, 0.0F};
constexpr Vec3 kUnitHalf{0.4F, 0.6F, 0.4F};
constexpr Vec3 kBarracksHalf{1.1F, 1.0F, 1.1F};
constexpr Vec3 kParked{0.0F, -60.0F, -12.0F};

[[nodiscard]] Vec3 camera_forward() noexcept {
    return normalize(Vec3{0.0F, -0.8F, -0.6F});
}

/// The cast: boxes 1 to 6, their team, where they stand, how far they see.
enum Box : u32 {
    kScoutA = 1,
    kScoutB = 2,
    kRedNear = 3,
    kRedExplored = 4,
    kRedFar = 5,
    kBarracks = 6,
};
constexpr u32 kCast = 7;
constexpr gameplay::TeamId kBlue = 1;
constexpr gameplay::TeamId kRed = 2;

struct Placement {
    gameplay::TeamId team = gameplay::kNoTeam;
    f32 x = 0.0F;
    f32 z = 0.0F;
    f32 radius = 0.0F;
    bool ghost = false;
};

/// Where each stands at the second tick. Scout B stood at (7, -8) at the first tick, which is how
/// blue explored the barracks and the red unit beside it before walking away.
const Placement kCastAt[kCast] = {
    {},
    {kBlue, -6.0F, -12.0F, 6.0F, false},  // scout A
    {kBlue, -7.0F, -21.0F, 3.0F, false},  // scout B, after its walk
    {kRed, -3.0F, -13.0F, 2.0F, false},   // inside A's sight
    {kRed, 5.5F, -9.5F, 2.0F, false},     // where B saw it, now unseen
    {kRed, 12.0F, -20.0F, 2.0F, false},   // nowhere blue has been
    {kRed, 8.5F, -5.5F, 3.0F, true},      // the barracks B saw
};
constexpr Vec3 kScoutBFirst{7.0F, 0.0F, -8.0F};

[[nodiscard]] i64 raw(f32 metres) noexcept {
    return static_cast<i64>(std::llround(static_cast<f64>(metres) * 4294967296.0));
}

[[nodiscard]] gfog::FogUnit fog_unit(u32 box, f32 x, f32 z) noexcept {
    gfog::FogUnit unit;
    unit.id = box;
    unit.team = kCastAt[box].team;
    unit.x_raw = raw(x);
    unit.z_raw = raw(z);
    unit.reveal_radius_raw = raw(kCastAt[box].radius);
    unit.leaves_ghost = kCastAt[box].ghost;
    return unit;
}

/// The simulation: two fixed steps of the patrol.
[[nodiscard]] bool run_simulation(gfog::FogOfWar& fog) {
    gfog::FogGridDescription grid;
    grid.width = kGridWidth;
    grid.height = kGridHeight;
    grid.cell_size_raw = raw(kPlacement.cell_size);
    grid.origin_x_raw = raw(kPlacement.origin_x);
    grid.origin_z_raw = raw(kPlacement.origin_z);
    if (!fog.create(grid).has_value()) {
        return false;
    }
    gfog::FogUnit units[kCast - 1];
    for (u32 box = 1; box < kCast; ++box) {
        units[box - 1] = fog_unit(box, kCastAt[box].x, kCastAt[box].z);
    }
    units[kScoutB - 1] = fog_unit(kScoutB, kScoutBFirst.x, kScoutBFirst.z);
    if (!fog.update(units, 1).has_value()) {
        return false;
    }
    units[kScoutB - 1] = fog_unit(kScoutB, kCastAt[kScoutB].x, kCastAt[kScoutB].z);
    return fog.update(units, 2).has_value();
}

[[nodiscard]] std::vector<u8> states_of(const gfog::FogOfWar& fog, gameplay::TeamId team) {
    std::vector<u8> states(static_cast<usize>(kGridWidth) * kGridHeight);
    (void)fog.write_states(team, {states.data(), states.size()});
    return states;
}

// --- The frame ---------------------------------------------------------------------------------

/// How one render is configured.
struct Run {
    bool fog = true;
    /// Which boxes stand where: the cast by blue's sight, all of it, or the ground alone.
    enum class Cast : u8 { Sighted, Everything, GroundOnly } cast = Cast::Sighted;
    const gfog::FogOfWar* simulation = nullptr;
    const VisibilityField* field = nullptr;
    FogOfWarPass* pass = nullptr;
    FrameScene* scene = nullptr;
    FogOfWarStyle style{};
    /// The sub-pixel offset the frame's projection was jittered by, from its report.
    Vec2 jitter{0.0F, 0.0F};
};

void configure(rendering::assembly::AssemblyDescription& description, void* user) noexcept {
    description.fog_of_war = static_cast<Run*>(user)->fog;
}

void shape_box(u32 which, Vec3& centre, Vec3& half, void* /*user*/) noexcept {
    if (which == 0) {
        centre = kGroundCentre;
        half = kGroundHalf;
        return;
    }
    centre = kParked;
    half = which == kBarracks ? kBarracksHalf : kUnitHalf;
}

void tint_box(u32 which, Vec3& tint, void* /*user*/) noexcept {
    Vec3 albedo{0.28F, 0.42F, 0.22F};
    if (which >= 1 && which < kCast) {
        albedo =
            kCastAt[which].team == kBlue ? Vec3{0.20F, 0.35F, 0.85F} : Vec3{0.85F, 0.22F, 0.15F};
    }
    const f32* material = kMaterialColours[which % kMaterialCount];
    tint = Vec3{albedo.x / material[0], albedo.y / material[1], albedo.z / material[2]};
}

Status before_assemble(RenderGraph& /*graph*/, rendering::assembly::AssemblyView& /*view*/,
                       FrameSinks& sinks, void* user) noexcept {
    auto* run = static_cast<Run*>(user);
    if (!run->fog) {
        return ok();
    }
    FogOfWarFrame frame;
    frame.view.projection = run->scene->projection();
    frame.view.relative_view = run->scene->relative_view();
    frame.view.eye = run->scene->eye();
    frame.placement = kPlacement;
    frame.style = run->style;
    frame.field = run->field;
    if (Status set = run->pass->set_frame(frame); !set) {
        return set;
    }
    sinks.fog_of_war = run->pass->stage();
    return ok();
}

/// Where box `which` is drawn this run.
[[nodiscard]] Vec3 drawn_at(const Run& run, u32 which) noexcept {
    const Placement& cast = kCastAt[which];
    const f32 lift = which == kBarracks ? kBarracksHalf.y : kUnitHalf.y;
    const Vec3 at{cast.x, lift, cast.z};
    switch (run.cast) {
        case Run::Cast::GroundOnly:
            return kParked;
        case Run::Cast::Everything:
            return at;
        case Run::Cast::Sighted:
            break;
    }
    // THE GAME'S DECISION, read from the simulation: a unit blue does not see is not drawn, and a
    // structure it remembers is drawn where it was last seen.
    switch (run.simulation->sight(kBlue, which)) {
        case gfog::UnitSight::Visible:
            return at;
        case gfog::UnitSight::Ghost: {
            const gfog::FogGhost* ghost = run.simulation->ghost(kBlue, which);
            return Vec3{static_cast<f32>(static_cast<f64>(ghost->x_raw) / 4294967296.0), lift,
                        static_cast<f32>(static_cast<f64>(ghost->z_raw) / 4294967296.0)};
        }
        case gfog::UnitSight::Hidden:
            break;
    }
    return kParked;
}

/// Render once; the 8-bit output into `pixels`.
[[nodiscard]] bool render_field(DeviceFixture& fixture, Run& run, std::vector<u32>& pixels) {
    FrameScene scene(allocator());
    run.scene = &scene;
    FrameSceneHooks hooks;
    hooks.user = &run;
    hooks.configure = &configure;
    hooks.before_assemble = &before_assemble;
    hooks.shape_box = &shape_box;
    hooks.tint_box = &tint_box;
    scene.set_hooks(hooks);
    if (!scene.build(fixture.device()).has_value()) {
        return false;
    }
    scene.set_camera(kEye, camera_forward());
    for (u32 box = 1; box < kInstanceCount; ++box) {
        const Vec3 at = box < kCast ? drawn_at(run, box) : kParked;
        if (!scene.move_box(box, at).has_value()) {
            return false;
        }
    }
    scene.set_read_back(true);
    rendering::assembly::AssemblyReport report;
    if (!scene.render(RecordMode::Callbacks, report).has_value()) {
        return false;
    }
    const PassId stage = scene.assembly().frame().pass_of(FramePassKind::FogOfWar);
    if ((stage != kInvalidPass) != run.fog) {
        std::fprintf(stderr, "the fog of war stage was %s\n", run.fog ? "absent" : "present");
        return false;
    }
    pixels.assign(scene.pixels().begin(), scene.pixels().end());
    run.jitter = report.jitter;
    run.scene = nullptr;
    return true;
}

void save(const char* name, const std::vector<u32>& pixels) {
    render_test::Image image(allocator());
    if (!render_test::adopt(image, Span<const u32>(pixels.data(), pixels.size()), kWidth, kHeight)
             .has_value()) {
        return;
    }
    const char* directory = std::getenv("CY_TEST_ARTEFACT_DIR");
    if (directory == nullptr || *directory == '\0') {
        directory = CY_TEST_BINARY_DIR;
    }
    char path[1024];
    (void)std::snprintf(path, sizeof(path), "%s/%s", directory, name);
    if (render_test::write_png(path, image).has_value()) {
        std::fprintf(stderr, "wrote %s (%ux%u)\n", path, kWidth, kHeight);
    }
}

[[nodiscard]] f32 luminance(u32 texel) noexcept {
    return (0.2126F * static_cast<f32>(texel & 0xFFU)) +
           (0.7152F * static_cast<f32>((texel >> 8U) & 0xFFU)) +
           (0.0722F * static_cast<f32>((texel >> 16U) & 0xFFU));
}

[[nodiscard]] f32 spread(u32 texel) noexcept {
    const f32 r = static_cast<f32>(texel & 0xFFU);
    const f32 g = static_cast<f32>((texel >> 8U) & 0xFFU);
    const f32 b = static_cast<f32>((texel >> 16U) & 0xFFU);
    return std::max({r, g, b}) - std::min({r, g, b});
}

/// What the dispatch does to find a pixel's world position, done on the host: the floor's depth
/// where the JITTERED sample of the pixel meets it, unprojected at the pixel's centre through the
/// constants the dispatch read. Returns false where the pixel does not see the ground.
[[nodiscard]] bool ground_under(const FogOfWarConstants& words, const Mat4& projection,
                                const Mat4& relative_view, Vec2 jitter, u32 x, u32 y,
                                Vec3& world) noexcept {
    const f32 ndc_x = (((static_cast<f32>(x) + 0.5F) / static_cast<f32>(kWidth)) * 2.0F) - 1.0F;
    const f32 ndc_y = 1.0F - (((static_cast<f32>(y) + 0.5F) / static_cast<f32>(kHeight)) * 2.0F);
    const auto unproject = [&words](f32 px, f32 py, f32 pz) {
        const Vec4 back =
            (words.words[0] * px) + (words.words[1] * py) + (words.words[2] * pz) + words.words[3];
        return Vec3{back.x / back.w, back.y / back.w, back.z / back.w};
    };
    // The surface the jittered projection put at this pixel's centre lies on the ray through the
    // unjittered point `ndc - offset`.
    const f32 sx = ndc_x - ((jitter.x * 2.0F) / static_cast<f32>(kWidth));
    const f32 sy = ndc_y - ((jitter.y * 2.0F) / static_cast<f32>(kHeight));
    const Vec3 a = unproject(sx, sy, 1.0F);
    const Vec3 b = unproject(sx, sy, 0.01F);
    const Vec3 eye{words.words[4].x, words.words[4].y, words.words[4].z};
    const f32 dy = b.y - a.y;
    if (std::fabs(dy) < 1.0e-6F) {
        return false;
    }
    const f32 t = (-eye.y - a.y) / dy;
    const Vec3 hit{a.x + ((b.x - a.x) * t), -eye.y, a.z + ((b.z - a.z) * t)};
    const Vec3 hit_world = hit + eye;
    if (t <= 0.0F || hit_world.x < -16.0F || hit_world.x > 16.0F || hit_world.z < -24.0F ||
        hit_world.z > 0.0F) {
        return false;
    }
    const Vec4 clip = (projection * relative_view) * Vec4{hit.x, hit.y, hit.z, 1.0F};
    const f32 depth = clip.z / clip.w;
    world = unproject(ndc_x, ndc_y, depth) + eye;
    return true;
}

}  // namespace

CY_TEST_CASE(
    "(a)(b) the device's visibility texture is the grid, and every pixel the host twin's") {
    DeviceFixture fixture;
    if (!fixture.has_gpu()) {
        fixture.report_skip();
        return;
    }
    gfog::FogOfWar simulation(allocator());
    CY_REQUIRE(run_simulation(simulation));
    const std::vector<u8> states = states_of(simulation, kBlue);
    VisibilityField field(allocator());
    CY_REQUIRE(field.create(kGridWidth, kGridHeight).has_value());
    CY_REQUIRE(field.set_states({states.data(), states.size()}).has_value());
    field.snap();

    FogOfWarPass pass;
    CY_REQUIRE(pass.create(allocator(), fixture.device(),
                           FogOfWarPassDescription{kWidth, kHeight, kGridWidth, kGridHeight, true})
                   .has_value());
    Run run;
    run.cast = Run::Cast::GroundOnly;
    run.simulation = &simulation;
    run.field = &field;
    run.pass = &pass;
    std::vector<u32> pixels;
    CY_REQUIRE(render_field(fixture, run, pixels));
    CY_CHECK_EQ(pass.report().passes, 4U);
    CY_CHECK(pass.report().dispatched);
    CY_CHECK_EQ(pass.report().uploaded_bytes, u64{kGridWidth} * kGridHeight * kTexelBytes);

    // (a) Texel for texel.
    std::vector<u8> texels(static_cast<usize>(kGridWidth) * kGridHeight * kTexelBytes);
    CY_REQUIRE(pass.read_back_visibility({texels.data(), texels.size()}).has_value());
    u32 wrong = 0;
    for (usize cell = 0; cell < states.size(); ++cell) {
        const u8 visible = states[cell] == kCellVisible ? 255U : 0U;
        const u8 explored = states[cell] == kCellUnexplored ? 0U : 255U;
        wrong += texels[cell * 4U] != visible || texels[(cell * 4U) + 1U] != explored ? 1U : 0U;
    }
    CY_CHECK_EQ(wrong, 0U);
    CY_CHECK(std::equal(texels.begin(), texels.end(), field.texels().begin()));

    // (b) The host twin at every pixel that sees the ground. The colour the stage read is the
    // colour it writes for a team that sees everything — (f) holds that bit for bit — so the same
    // frame drawn once more with every cell visible is the source.
    std::vector<u16> target(static_cast<usize>(kWidth) * kHeight * 4U);
    CY_REQUIRE(pass.read_back_colour({target.data(), target.size()}).has_value());
    VisibilityField open(allocator());
    CY_REQUIRE(open.create(kGridWidth, kGridHeight).has_value());
    const std::vector<u8> all_visible(states.size(), kCellVisible);
    CY_REQUIRE(open.set_states({all_visible.data(), all_visible.size()}).has_value());
    open.snap();
    Run unfogged = run;
    unfogged.field = &open;
    std::vector<u32> unfogged_pixels;
    CY_REQUIRE(render_field(fixture, unfogged, unfogged_pixels));
    std::vector<u16> source(target.size());
    CY_REQUIRE(pass.read_back_colour({source.data(), source.size()}).has_value());
    // The scene's view, as the pass was given it: a fresh scene with the same camera.
    FrameScene probe(allocator());
    FrameSceneHooks hooks;
    hooks.shape_box = &shape_box;
    probe.set_hooks(hooks);
    CY_REQUIRE(probe.build(fixture.device()).has_value());
    probe.set_camera(kEye, camera_forward());
    const Vec2 jitter = run.jitter;
    u32 ground = 0;
    u32 sky = 0;
    u32 visible_exact = 0;
    u32 mismatched = 0;
    f32 worst = 0.0F;
    for (u32 y = 0; y < kHeight; ++y) {
        for (u32 x = 0; x < kWidth; ++x) {
            const usize at = ((static_cast<usize>(y) * kWidth) + x) * 4U;
            Vec3 world{};
            if (!ground_under(pass.constants(), probe.projection(), probe.relative_view(), jitter,
                              x, y, world)) {
                // Off the ground: the sky or the ground's edge. The sky's depth is zero, and the
                // dispatch writes it as it read it.
                bool same = true;
                for (u32 c = 0; c < 4U; ++c) {
                    same = same && source[at + c] == target[at + c];
                }
                sky += same ? 1U : 0U;
                continue;
            }
            ++ground;
            const Vec3 colour{from_half(source[at]), from_half(source[at + 1U]),
                              from_half(source[at + 2U])};
            const f32 got[3] = {from_half(target[at]), from_half(target[at + 1U]),
                                from_half(target[at + 2U])};
            // The twin at the reconstructed point and a centimetre either way along each axis —
            // what the device's depth precision can move a surface by — so the device must land
            // inside the band the twin spans there, plus half precision.
            f32 low[3] = {1.0e30F, 1.0e30F, 1.0e30F};
            f32 high[3] = {-1.0e30F, -1.0e30F, -1.0e30F};
            bool surely_visible = true;
            constexpr f32 kSlack = 0.01F;
            const Vec2 offsets[5] = {Vec2{0.0F, 0.0F}, Vec2{kSlack, 0.0F}, Vec2{-kSlack, 0.0F},
                                     Vec2{0.0F, kSlack}, Vec2{0.0F, -kSlack}};
            for (const Vec2 offset : offsets) {
                const FogSample sample =
                    sample_visibility(field.texels(), kGridWidth, kGridHeight, kPlacement,
                                      world.x + offset.x, world.z + offset.y);
                surely_visible = surely_visible && sample.visible >= 1.0F;
                const Vec3 expected = shade_fog(colour, sample, run.style);
                const f32 want[3] = {expected.x, expected.y, expected.z};
                for (u32 c = 0; c < 3U; ++c) {
                    low[c] = std::min(low[c], want[c]);
                    high[c] = std::max(high[c], want[c]);
                }
            }
            for (u32 c = 0; c < 3U; ++c) {
                const f32 half_ulp =
                    (std::max(std::fabs(low[c]), std::fabs(high[c])) * 1.0e-3F) + 1.0e-5F;
                const f32 error = std::max(low[c] - half_ulp - got[c], got[c] - high[c] - half_ulp);
                worst = std::max(worst, error / std::max(std::fabs(high[c]), 1.0e-3F));
                mismatched += error > 0.0F ? 1U : 0U;
            }
            if (surely_visible) {
                bool same = true;
                for (u32 c = 0; c < 4U; ++c) {
                    same = same && source[at + c] == target[at + c];
                }
                visible_exact += same ? 1U : 0U;
                CY_CHECK(same);
            }
        }
    }
    std::fprintf(stderr,
                 "(b) ground pixels %u, of which visible and unchanged %u; off-ground unchanged "
                 "%u; mismatched channels %u, worst excess %.5f of the value\n",
                 ground, visible_exact, sky, mismatched, static_cast<f64>(worst));
    CY_CHECK_GT(ground, 60000U);
    CY_CHECK_GT(visible_exact, 5000U);
    CY_CHECK_EQ(mismatched, 0U);
    CY_CHECK_EQ(fixture.validation_errors(), 0U);
}

CY_TEST_CASE("(c)(d)(e) explored greyed, unexplored hidden, enemy units by sight, smooth edges") {
    DeviceFixture fixture;
    if (!fixture.has_gpu()) {
        fixture.report_skip();
        return;
    }
    gfog::FogOfWar simulation(allocator());
    CY_REQUIRE(run_simulation(simulation));
    // The simulation's verdicts the picture must follow.
    CY_CHECK(simulation.sight(kBlue, kRedNear) == gfog::UnitSight::Visible);
    CY_CHECK(simulation.sight(kBlue, kRedExplored) == gfog::UnitSight::Hidden);
    CY_CHECK(simulation.sight(kBlue, kRedFar) == gfog::UnitSight::Hidden);
    CY_CHECK(simulation.sight(kBlue, kBarracks) == gfog::UnitSight::Ghost);
    const std::vector<u8> states = states_of(simulation, kBlue);
    VisibilityField field(allocator());
    CY_REQUIRE(field.create(kGridWidth, kGridHeight).has_value());
    CY_REQUIRE(field.set_states({states.data(), states.size()}).has_value());
    field.snap();
    FogOfWarPass pass;
    CY_REQUIRE(pass.create(allocator(), fixture.device(),
                           FogOfWarPassDescription{kWidth, kHeight, kGridWidth, kGridHeight, false})
                   .has_value());

    Run plain;
    plain.fog = false;
    plain.cast = Run::Cast::Everything;
    std::vector<u32> without;
    CY_REQUIRE(render_field(fixture, plain, without));
    save("fog-of-war-off.png", without);

    Run sighted;
    sighted.simulation = &simulation;
    sighted.field = &field;
    sighted.pass = &pass;
    std::vector<u32> with;
    CY_REQUIRE(render_field(fixture, sighted, with));
    save("fog-of-war-on.png", with);

    Run everything = sighted;
    everything.cast = Run::Cast::Everything;
    std::vector<u32> all_drawn;
    CY_REQUIRE(render_field(fixture, everything, all_drawn));
    save("fog-of-war-everything-drawn.png", all_drawn);

    Run ground = sighted;
    ground.cast = Run::Cast::GroundOnly;
    std::vector<u32> bare;
    CY_REQUIRE(render_field(fixture, ground, bare));
    save("fog-of-war-ground.png", bare);

    Run plain_ground;
    plain_ground.fog = false;
    plain_ground.cast = Run::Cast::GroundOnly;
    std::vector<u32> ground_without;
    CY_REQUIRE(render_field(fixture, plain_ground, ground_without));

    // (c) Ground by class, at the centre of every cell whose eight neighbours share its state —
    // away from the ramps — where the pixel under that centre sees the ground.
    FrameScene probe(allocator());
    FrameSceneHooks hooks;
    hooks.shape_box = &shape_box;
    probe.set_hooks(hooks);
    CY_REQUIRE(probe.build(fixture.device()).has_value());
    probe.set_camera(kEye, camera_forward());
    const Mat4 to_clip = probe.projection() * probe.view();
    const auto pixel_of = [&](f32 x, f32 z, u32& px, u32& py) {
        const Vec4 clip = to_clip * Vec4{x, 0.0F, z, 1.0F};
        if (clip.w <= 0.0F) {
            return false;
        }
        const f32 sx = (((clip.x / clip.w) * 0.5F) + 0.5F) * static_cast<f32>(kWidth);
        const f32 sy = (0.5F - ((clip.y / clip.w) * 0.5F)) * static_cast<f32>(kHeight);
        if (sx < 0.0F || sy < 0.0F || sx >= static_cast<f32>(kWidth) ||
            sy >= static_cast<f32>(kHeight)) {
            return false;
        }
        px = static_cast<u32>(sx);
        py = static_cast<u32>(sy);
        return true;
    };
    u32 counted[3] = {0, 0, 0};
    u32 failures = 0;
    for (u32 cz = 1; cz + 1U < kGridHeight; ++cz) {
        for (u32 cx = 1; cx + 1U < kGridWidth; ++cx) {
            const u8 state = states[(cz * kGridWidth) + cx];
            bool uniform = true;
            for (u32 n = 0; n < 9U; ++n) {
                const u32 nx = cx + (n % 3U) - 1U;
                const u32 nz = cz + (n / 3U) - 1U;
                uniform = uniform && states[(nz * kGridWidth) + nx] == state;
            }
            u32 px = 0;
            u32 py = 0;
            if (!uniform || !pixel_of(kPlacement.origin_x + static_cast<f32>(cx) + 0.5F,
                                      kPlacement.origin_z + static_cast<f32>(cz) + 0.5F, px, py)) {
                continue;
            }
            const u32 off = ground_without[(static_cast<usize>(py) * kWidth) + px];
            const u32 on = bare[(static_cast<usize>(py) * kWidth) + px];
            counted[state] += 1U;
            if (state == kCellVisible) {
                failures += on != off ? 1U : 0U;
            } else if (state == kCellExplored) {
                const bool darker = luminance(on) < luminance(off) * 0.85F;
                const bool greyer = spread(on) < spread(off) * 0.6F;
                failures += darker && greyer ? 0U : 1U;
            } else {
                failures += (on & 0x00FFFFFFU) == 0U ? 0U : 1U;
            }
        }
    }
    std::fprintf(stderr,
                 "(c) cells measured: visible %u, explored %u, unexplored %u; failures %u\n",
                 counted[2], counted[1], counted[0], failures);
    CY_CHECK_GT(counted[0], 20U);
    CY_CHECK_GT(counted[1], 5U);
    CY_CHECK_GT(counted[2], 20U);
    CY_CHECK_EQ(failures, 0U);

    // (d) Smooth edges: on every row, the largest step between neighbours in the fogged ground is
    // a fraction of the contrast the fog spans on that row. A hard-edged fog would put the whole
    // contrast into one step.
    u32 rows = 0;
    f32 worst_ratio = 0.0F;
    for (u32 y = kHeight / 4U; y < kHeight; ++y) {
        f32 low = 1.0e9F;
        f32 high = -1.0e9F;
        f32 step = 0.0F;
        for (u32 x = 1; x < kWidth; ++x) {
            const usize at = (static_cast<usize>(y) * kWidth) + x;
            // Only where the plain ground is itself smooth: its own neighbours within one step.
            if (std::fabs(luminance(ground_without[at]) - luminance(ground_without[at - 1U])) >
                1.5F) {
                continue;
            }
            const f32 here = luminance(bare[at]);
            low = std::min(low, here);
            high = std::max(high, here);
            step = std::max(step, std::fabs(here - luminance(bare[at - 1U])));
        }
        if (high - low < 40.0F) {
            continue;
        }
        rows += 1U;
        worst_ratio = std::max(worst_ratio, step / (high - low));
    }
    std::fprintf(stderr, "(d) rows crossing a boundary %u, worst step / contrast %.3f\n", rows,
                 static_cast<f64>(worst_ratio));
    CY_CHECK_GT(rows, 50U);
    CY_CHECK_LT(worst_ratio, 0.25F);

    // (e) Units by sight. The hidden red unit on explored ground is drawn, greyed, when everything
    // is drawn, and not at all when blue's sight decides; the far one likewise; the barracks
    // stands as its ghost; the red unit in sight is the same in both.
    const auto footprint_differs = [&](const std::vector<u32>& a, const std::vector<u32>& b,
                                       u32 box) {
        u32 px = 0;
        u32 py = 0;
        if (!pixel_of(kCastAt[box].x, kCastAt[box].z, px, py)) {
            return u32{0};
        }
        u32 differing = 0;
        for (u32 dy = 0; dy < 5U; ++dy) {
            for (u32 dx = 0; dx < 5U; ++dx) {
                const usize at = ((static_cast<usize>(py + dy) - 2U) * kWidth) + px + dx - 2U;
                differing += a[at] != b[at] ? 1U : 0U;
            }
        }
        return differing;
    };
    CY_CHECK_GT(footprint_differs(all_drawn, with, kRedExplored), 10U);
    CY_CHECK_EQ(footprint_differs(bare, with, kRedExplored), 0U);
    CY_CHECK_EQ(footprint_differs(bare, with, kRedFar), 0U);
    CY_CHECK_GT(footprint_differs(bare, with, kBarracks), 10U);
    CY_CHECK_EQ(footprint_differs(all_drawn, with, kRedNear), 0U);
    CY_CHECK_GT(footprint_differs(bare, with, kRedNear), 10U);
    CY_CHECK_EQ(fixture.validation_errors(), 0U);
}

CY_TEST_CASE("(f) off is the frame before, and a team that sees everything changes nothing") {
    DeviceFixture fixture;
    if (!fixture.has_gpu()) {
        fixture.report_skip();
        return;
    }
    Run plain;
    plain.fog = false;
    plain.cast = Run::Cast::Everything;
    std::vector<u32> without;
    CY_REQUIRE(render_field(fixture, plain, without));

    render_test::Image rendered(allocator());
    CY_REQUIRE(render_test::adopt(rendered, Span<const u32>(without.data(), without.size()), kWidth,
                                  kHeight)
                   .has_value());
    char reference_path[1024];
    (void)std::snprintf(reference_path, sizeof(reference_path), "%s/references/fog_of_war_off.png",
                        CY_FOG_OF_WAR_TEST_DIR);
    const char* update = std::getenv("CY_RENDER_UPDATE_GOLDEN");
    if (update != nullptr && update[0] != '\0' && update[0] != '0') {
        CY_CHECK(render_test::write_png(reference_path, rendered).has_value());
        std::fprintf(stderr, "wrote %s — look at it, then commit it. This run FAILS on purpose.\n",
                     reference_path);
        CY_TEST_FAIL_CHECK("references were regenerated; this mode never passes");
        return;
    }
    render_test::Image reference(allocator());
    CY_REQUIRE(render_test::read_png(reference_path, reference).has_value());
    const render_test::Comparison comparison = render_test::compare(reference, rendered);
    std::fprintf(stderr, "(f) the stage absent against the frame before: %u differing, worst %u\n",
                 comparison.differing, comparison.max_channel_delta);
    CY_CHECK(comparison.comparable);
    CY_CHECK_EQ(comparison.differing, 0U);
    CY_CHECK_EQ(comparison.max_channel_delta, 0U);

    // Every cell visible: the stage declared, uploaded and dispatched, and every pixel as it was.
    VisibilityField field(allocator());
    CY_REQUIRE(field.create(kGridWidth, kGridHeight).has_value());
    const std::vector<u8> all_visible(static_cast<usize>(kGridWidth) * kGridHeight, kCellVisible);
    CY_REQUIRE(field.set_states({all_visible.data(), all_visible.size()}).has_value());
    field.snap();
    FogOfWarPass pass;
    CY_REQUIRE(pass.create(allocator(), fixture.device(),
                           FogOfWarPassDescription{kWidth, kHeight, kGridWidth, kGridHeight, false})
                   .has_value());
    gfog::FogOfWar unused(allocator());
    Run seen;
    seen.cast = Run::Cast::Everything;
    seen.simulation = &unused;
    seen.field = &field;
    seen.pass = &pass;
    std::vector<u32> with;
    CY_REQUIRE(render_field(fixture, seen, with));
    CY_CHECK(pass.report().dispatched);
    usize differing = 0;
    for (usize index = 0; index < with.size(); ++index) {
        differing += with[index] != without[index] ? 1U : 0U;
    }
    CY_CHECK_EQ(differing, usize{0});
    CY_CHECK_EQ(fixture.validation_errors(), 0U);
}

CY_TEST_CASE("(g) a cell revealed this frame fades in between explored and visible") {
    DeviceFixture fixture;
    if (!fixture.has_gpu()) {
        fixture.report_skip();
        return;
    }
    gfog::FogOfWar simulation(allocator());
    CY_REQUIRE(run_simulation(simulation));
    const std::vector<u8> states = states_of(simulation, kBlue);
    // Every explored cell, then the simulation's states a quarter of the way through the reveal.
    std::vector<u8> before(states);
    for (u8& state : before) {
        state = state == kCellUnexplored ? kCellUnexplored : kCellExplored;
    }
    VisibilityField field(allocator());
    FogFadeSettings fade;
    fade.reveal_seconds = 0.4F;
    CY_REQUIRE(field.create(kGridWidth, kGridHeight, fade).has_value());
    CY_REQUIRE(field.set_states({before.data(), before.size()}).has_value());
    field.snap();
    CY_REQUIRE(field.set_states({states.data(), states.size()}).has_value());
    field.advance(0.1F);
    CY_CHECK_FALSE(field.settled());

    FogOfWarPass pass;
    CY_REQUIRE(pass.create(allocator(), fixture.device(),
                           FogOfWarPassDescription{kWidth, kHeight, kGridWidth, kGridHeight, false})
                   .has_value());
    Run run;
    run.cast = Run::Cast::GroundOnly;
    run.simulation = &simulation;
    run.field = &field;
    run.pass = &pass;
    std::vector<u32> quarter;
    CY_REQUIRE(render_field(fixture, run, quarter));
    save("fog-of-war-fade-quarter.png", quarter);
    field.snap();
    std::vector<u32> settled;
    CY_REQUIRE(render_field(fixture, run, settled));
    Run plain;
    plain.fog = false;
    plain.cast = Run::Cast::GroundOnly;
    std::vector<u32> without;
    CY_REQUIRE(render_field(fixture, plain, without));

    // Scout A's own cell: brighter than explored, darker than visible.
    FrameScene probe(allocator());
    FrameSceneHooks hooks;
    hooks.shape_box = &shape_box;
    probe.set_hooks(hooks);
    CY_REQUIRE(probe.build(fixture.device()).has_value());
    probe.set_camera(kEye, camera_forward());
    const Vec4 clip = (probe.projection() * probe.view()) *
                      Vec4{kCastAt[kScoutA].x + 1.5F, 0.0F, kCastAt[kScoutA].z, 1.0F};
    const u32 px = static_cast<u32>((((clip.x / clip.w) * 0.5F) + 0.5F) * kWidth);
    const u32 py = static_cast<u32>((0.5F - ((clip.y / clip.w) * 0.5F)) * kHeight);
    const usize at = (static_cast<usize>(py) * kWidth) + px;
    std::fprintf(stderr,
                 "(g) luminance under scout A: settled %.1f, a quarter in %.1f, plain %.1f\n",
                 static_cast<f64>(luminance(settled[at])), static_cast<f64>(luminance(quarter[at])),
                 static_cast<f64>(luminance(without[at])));
    CY_CHECK_EQ(settled[at], without[at]);
    CY_CHECK_LT(luminance(quarter[at]), luminance(settled[at]) - 5.0F);
    CY_CHECK_GT(spread(quarter[at]), 0.0F);
    CY_CHECK_EQ(fixture.validation_errors(), 0U);
}
