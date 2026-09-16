// EVERY DECLARED SIGNAL HAS A PRODUCER, AND THE PRODUCER IS WHAT DRIVES THE FRAMEWORK. M11.c 2.5.
//
// ================================================================================================
// WHAT THIS FILE IS THE CONTROL FOR
// ================================================================================================
//
// `denoising` declares five signals, gives each a `SignalConfig` with its own domain, lobe shape,
// history length and edge-stopping tolerances, and requires each to be reconstructed accordingly.
// **Until M11.c nothing in this tree called `Denoiser::denoise()` except the denoiser's own two
// suites.** `src/rendering/gi/` was the only module that named `Denoiser` at all, and what it did
// with it was set its quality position from a budget lever.
//
// The consequence is not that the filter was untested — `unit.render_denoise` and
// `integration.render_denoise_filter` test it hard, including the bit-for-bit case that makes
// "one filter" structural. The consequence is that **the per-signal half of the specification was
// exercised only by buffers written by the test asserting on them**, and "the framework handles
// five signals" was readable off an enumerator and off nothing else. `denoising`'s own scenario
// says so in as many words: a visibility term denoised as occlusion "SHALL be exercised by the
// producer rather than by a synthetic buffer written in the denoiser's own test".
//
// So this suite runs `StochasticSignals` over a real room with a real occluder, and asks the
// framework's own per-signal census who drove it. A signal with no producer shows up as an
// `invocations` of zero, by name.

#include <cy/test/test.h>

#include <cy/core/memory/system_allocator.h>
#include <cy/rendering/gi/signals.h>

#include "support.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace {

using cy::f32;
using cy::u32;
using cy::Vec3;
using cy::rendering::denoise::kSignalCount;
using cy::rendering::denoise::signal_name;
using cy::rendering::denoise::SignalDomain;
using cy::rendering::denoise::SignalKind;
using namespace cy::rendering::gi;  // NOLINT(google-build-using-namespace)

constexpr u32 kWidth = 24;
constexpr u32 kHeight = 18;
constexpr u32 kPixels = kWidth * kHeight;

/// The room, plus a pillar in the middle of it.
///
/// THE PILLAR IS NOT SCENERY. Four of the five signals are measurements of VISIBILITY in some form,
/// and an empty room answers "visible" everywhere: every shadow ray reaches the light, every
/// occlusion ray escapes, and the produced buffers are constants with no variance for a denoiser to
/// reduce and no edge for it to stop on. A producer tested in an empty room is a producer that
/// cannot be distinguished from one that returns a constant.
struct PillarRoom {
    gi_support::RoomField room{Vec3{gi_support::kRoomX, gi_support::kRoomY, gi_support::kRoomZ}};
    gi_support::BoxField pillar{Vec3{0.6F, 1.6F, 0.6F}};
    std::vector<Surfel> surfels = gi_support::room_surfels(1.0F, false);
    std::vector<GiLight> lights = gi_support::room_lights();
    IlluminationSystem system;

    PillarRoom() {
        // AN AREA LIGHT, because a point light casts a shadow with no penumbra and therefore no
        // stochastic shadow signal at all — the producer would draw the same hard edge every frame
        // and the framework would be handed a buffer with nothing to reconstruct.
        lights[0].radius = 0.6F;
        CY_REQUIRE(system.configure(gi_support::room_settings()).has_value());
        CY_REQUIRE(system.field().place(1, room.asset(), cy::Mat4::identity()).has_value());
        CY_REQUIRE(
            system.field()
                .place(2, pillar.asset(), cy::Mat4::from_translation(Vec3{0.0F, -0.4F, 0.0F}))
                .has_value());
        const cy::Aabb bounds = cy::Aabb::from_center_extents(
            Vec3{0.0F, 0.0F, 0.0F},
            Vec3{gi_support::kRoomX + 1.0F, gi_support::kRoomY + 1.0F, gi_support::kRoomZ + 1.0F});
        CY_REQUIRE(
            system.scene().ingest_cell(1, bounds, {surfels.data(), surfels.size()}, 0).has_value());
        CY_REQUIRE(system.surfaces().allocate_from(system.scene(), bounds).has_value());
        system.surfaces().set_lookup_radius(1.2F);
        for (u32 frame = 0; frame < 6; ++frame) {
            FrameContext ctx;
            ctx.camera = Vec3{0.0F, 0.0F, 3.0F};
            ctx.lights = {lights.data(), lights.size()};
            ctx.frame = frame;
            ctx.measured_gi_ms = 1.0F;
            (void)system.update(ctx);
        }
    }
};

/// A visibility buffer over the room's floor: the camera looks down at it, so every pixel carries a
/// surface and the signals have somewhere to be produced.
struct FloorView {
    std::vector<f32> depth = std::vector<f32>(kPixels, 0.0F);
    std::vector<Vec3> position = std::vector<Vec3>(kPixels);
    std::vector<Vec3> normal = std::vector<Vec3>(kPixels, Vec3{0.0F, 1.0F, 0.0F});
    std::vector<f32> roughness = std::vector<f32>(kPixels, 0.45F);
    std::vector<u32> instance = std::vector<u32>(kPixels, 1U);
    std::vector<u32> material = std::vector<u32>(kPixels, 1U);
    Vec3 camera{0.0F, 1.8F, 0.0F};

    FloorView() {
        const f32 span = 3.6F;
        for (u32 y = 0; y < kHeight; ++y) {
            for (u32 x = 0; x < kWidth; ++x) {
                const u32 index = (y * kWidth) + x;
                const f32 u = (static_cast<f32>(x) + 0.5F) / static_cast<f32>(kWidth);
                const f32 v = (static_cast<f32>(y) + 0.5F) / static_cast<f32>(kHeight);
                const Vec3 point{-span + (2.0F * span * u), -gi_support::kRoomY + 0.02F,
                                 -span + (2.0F * span * v)};
                position[index] = point;
                depth[index] = length(point - camera);
            }
        }
    }

    [[nodiscard]] SignalSurfaces surfaces() const noexcept {
        SignalSurfaces out;
        out.width = kWidth;
        out.height = kHeight;
        out.depth = {depth.data(), depth.size()};
        out.position = {position.data(), position.size()};
        out.normal = {normal.data(), normal.size()};
        out.roughness = {roughness.data(), roughness.size()};
        out.instance_id = {instance.data(), instance.size()};
        out.material_id = {material.data(), material.size()};
        out.camera = camera;
        out.occlusion_radius_metres = 2.0F;
        return out;
    }
};

/// The fraction of an occlusion buffer that is still DECIDED: fully lit or fully shadowed rather
/// than somewhere in between.
///
/// WHY THIS AND NOT A CONTRAST BETWEEN THE TAILS. A 0/1 shadow buffer has a fully-lit tenth and a
/// fully-shadowed tenth whatever is done to the middle of it, so the tails answer 1 for any filter
/// including one that smeared the whole penumbra. What "contact detail" means is that a pixel which
/// WAS decided stays decided, and this counts exactly that.
[[nodiscard]] f32 decisive_fraction(cy::Span<const Vec3> values) {
    // CONTINUOUS RATHER THAN A THRESHOLD COUNT. |2v - 1| is one for a pixel that is fully lit or
    // fully shadowed and zero for one the filter has left exactly undecided, so a filter that moved
    // a pixel from 0.05 to 0.2 is counted for having done so. A threshold would have registered
    // nothing until the pixel crossed it, which is most of what a blur actually does.
    f32 total = 0.0F;
    for (const Vec3& value : values) {
        total += std::fabs((2.0F * value.x) - 1.0F);
    }
    return values.empty() ? 0.0F : total / static_cast<f32>(values.size());
}

}  // namespace

CY_TEST_CASE("every declared signal has a producer that routes through the framework") {
    PillarRoom room;
    FloorView view;
    StochasticSignals signals;
    CY_REQUIRE(signals.resize(kWidth, kHeight).has_value());

    // THE CENSUS BEFORE ANYBODY PRODUCED ANYTHING. Without this the assertion below would pass on a
    // denoiser whose counter was initialised to one.
    for (u32 index = 0; index < kSignalCount; ++index) {
        CY_CHECK_EQ(room.system.denoiser().diagnostics(static_cast<SignalKind>(index)).invocations,
                    0U);
    }

    const cy::rendering::denoise::HistoryGuidance history;
    const auto production = signals.produce(
        view.surfaces(), {room.lights.data(), room.lights.size()}, room.system, history, 1);
    CY_REQUIRE(production.has_value());
    const SignalProduction& report = production.value();

    // --- THE TABLE THE REQUIREMENT ASKS FOR: which signals have a producer, BY NAME
    // ---------------
    u32 driven = 0;
    for (u32 index = 0; index < kSignalCount; ++index) {
        const auto kind = static_cast<SignalKind>(index);
        const auto& diagnostics = room.system.denoiser().diagnostics(kind);
        CY_TEST_MESSAGE(std::string(signal_name(kind)), ": produced=", report.produced[index],
                        " pixels=", report.pixels[index], " rays=", report.rays[index],
                        " denoiser invocations=", diagnostics.invocations, " variance ",
                        report.noisy_variance[index], " -> ", report.reconstructed_variance[index]);
        if (report.produced[index] && diagnostics.invocations > 0) {
            driven += 1;
        }
        // PRODUCED IS NOT ENOUGH. A producer that filled a buffer and never handed it over would
        // set the first and leave the second at zero, which is the tree as it stood before M11.c
        // with one extra step in it.
        CY_CHECK(report.produced[index]);
        CY_CHECK_GT(diagnostics.invocations, 0U);
        CY_CHECK_EQ(report.pixels[index], kPixels);
        CY_CHECK_FALSE(diagnostics.bypassed);
        CY_CHECK_GT(diagnostics.cost_ns, 0ULL);
        // The visibility buffer's identities were available, so the filter stopped on exact
        // boundaries rather than inferred ones — which is the guidance this producer supplies and a
        // synthetic buffer would not have.
        CY_CHECK(diagnostics.identity_available);
    }
    CY_CHECK_EQ(driven, kSignalCount);

    // --- AND THE FRAMEWORK ACTUALLY RECONSTRUCTED SOMETHING
    // ---------------------------------------
    //
    // Every one of the five is a one-sample-per-pixel estimate, so every one of them arrives noisy.
    // A signal whose variance did not fall went through a filter that did nothing to it, and a
    // producer feeding such a signal is a producer feeding a constant.
    for (u32 index = 0; index < kSignalCount; ++index) {
        const auto kind = static_cast<SignalKind>(index);
        CY_TEST_MESSAGE("variance of ", std::string(signal_name(kind)), ": ",
                        report.noisy_variance[index], " -> ", report.reconstructed_variance[index]);
        CY_CHECK_GT(report.noisy_variance[index], 0.0F);
        CY_CHECK_LT(report.reconstructed_variance[index], report.noisy_variance[index]);
        CY_CHECK_EQ(signals.reconstructed(kind).size(), static_cast<cy::usize>(kPixels));
    }

    // --- THE SAME FRAME TWICE IS THE SAME NOISE
    // ----------------------------------------------------
    //
    // The sample directions are a permutation of the pixel and the frame rather than a generator,
    // so a stochastic signal is reproducible. Without this a golden image of anything downstream of
    // these five would be a coin toss.
    StochasticSignals again;
    CY_REQUIRE(again.resize(kWidth, kHeight).has_value());
    PillarRoom second;
    const auto repeat = again.produce(view.surfaces(), {second.lights.data(), second.lights.size()},
                                      second.system, history, 1);
    CY_REQUIRE(repeat.has_value());
    for (u32 index = 0; index < kSignalCount; ++index) {
        const auto kind = static_cast<SignalKind>(index);
        const cy::Span<const Vec3> first = signals.noisy(kind);
        const cy::Span<const Vec3> other = again.noisy(kind);
        CY_REQUIRE(first.size() == other.size());
        u32 disagreements = 0;
        for (cy::usize pixel = 0; pixel < first.size(); ++pixel) {
            if (std::fabs(first[pixel].x - other[pixel].x) > 1e-5F) {
                disagreements += 1;
            }
        }
        CY_CHECK_EQ(disagreements, 0U);
    }
}

CY_TEST_CASE("a visibility term is denoised as occlusion rather than as radiance") {
    // `denoising`: "Visibility terms (shadows, ambient occlusion) SHALL be denoised as occlusion
    // rather than as radiance, since blurring them as colour loses contact detail" — and the
    // scenario requires the case to be "exercised by the producer rather than by a synthetic buffer
    // written in the denoiser's own test". The buffer below is the producer's.
    PillarRoom room;
    FloorView view;
    StochasticSignals signals;
    CY_REQUIRE(signals.resize(kWidth, kHeight).has_value());

    const cy::rendering::denoise::HistoryGuidance history;
    const auto production = signals.produce(
        view.surfaces(), {room.lights.data(), room.lights.size()}, room.system, history, 3);
    CY_REQUIRE(production.has_value());

    // The pillar has to actually shadow something, or the comparison below is two filters over one
    // constant. This is the control on the fixture rather than on the filter.
    const f32 raw_decisive = decisive_fraction(signals.noisy(SignalKind::RayTracedShadow));
    const f32 raw_variance =
        luminance_variance(signals.noisy(SignalKind::RayTracedShadow), view.surfaces().depth);
    CY_TEST_MESSAGE("the produced shadow buffer: ", raw_decisive, " of it is decided, variance ",
                    raw_variance);
    CY_REQUIRE(raw_variance > 0.01F);
    CY_REQUIRE(raw_decisive > 0.9F);

    // The declared domains, read off the table the framework ships rather than restated here.
    CY_CHECK_EQ(room.system.denoiser().config(SignalKind::RayTracedShadow).domain,
                SignalDomain::Visibility);
    CY_CHECK_EQ(room.system.denoiser().config(SignalKind::AmbientOcclusion).domain,
                SignalDomain::Visibility);
    CY_CHECK_EQ(room.system.denoiser().config(SignalKind::IndirectDiffuse).domain,
                SignalDomain::Radiance);

    const f32 occlusion_decisive =
        decisive_fraction(signals.reconstructed(SignalKind::RayTracedShadow));

    // THE SAME BUFFER, THROUGH THE SAME FILTER, WITH THE RADIANCE CONFIGURATION. `SignalKind`
    // carries no behaviour — `integration.render_denoise_filter` asserts that bit for bit — so this
    // is the domain and nothing else: a second denoiser, the same kind, the diffuse configuration.
    cy::rendering::denoise::Denoiser as_radiance;
    CY_REQUIRE(as_radiance.resize(kWidth, kHeight).has_value());
    as_radiance.configure(SignalKind::RayTracedShadow,
                          cy::rendering::denoise::default_config(SignalKind::IndirectDiffuse));
    cy::rendering::denoise::GuidanceBuffers guidance;
    const SignalSurfaces surfaces = view.surfaces();
    guidance.width = surfaces.width;
    guidance.height = surfaces.height;
    guidance.depth = surfaces.depth;
    guidance.normal = surfaces.normal;
    guidance.roughness = surfaces.roughness;
    guidance.instance_id = surfaces.instance_id;
    guidance.material_id = surfaces.material_id;
    cy::rendering::denoise::NoisySignal noisy;
    noisy.values = signals.noisy(SignalKind::RayTracedShadow);
    const auto as_colour =
        as_radiance.denoise(SignalKind::RayTracedShadow, noisy, guidance, history);
    CY_REQUIRE(as_colour.has_value());
    const f32 radiance_decisive = decisive_fraction(as_colour.value());

    CY_TEST_MESSAGE("decided fraction of the shadow buffer: raw ", raw_decisive, ", as occlusion ",
                    occlusion_decisive, ", as radiance ", radiance_decisive);
    // Contact hardening is what survives. Reconstructed as colour, the wide value tolerance carries
    // the filter straight across the penumbra and the decided pixels dissolve into it; as occlusion
    // the tight tolerance stops there the way it stops at a geometric edge.
    CY_CHECK_GT(occlusion_decisive, radiance_decisive);
}
