// SPDX-License-Identifier: MIT
// Temporal upscaling's arithmetic and its seams, with no device. `unit.rendering_upscale`.
//
// The modes and the extents they render at, the jitter sequence an upscale needs, dynamic
// resolution as a composition of the arbiter's allocation and the mode, the constants' one sign
// convention, and the interface a vendor upscaler plugs into — chosen when it is present and
// available, and the built-in one when it is not.

#include <cy/core/math/projection.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/rendering/upscale/upscale.h>
#include <cy/rendering/upscale/upscaler.h>
#include <cy/test/test.h>

#include <cstring>

using namespace cy;
using namespace cy::rendering;
using namespace cy::rendering::upscale;

namespace {

constexpr Extent2D k1440p{2560, 1440};
constexpr Extent2D k4K{3840, 2160};

[[nodiscard]] UpscaleView view_at(Extent2D render, Extent2D output) noexcept {
    UpscaleView view;
    view.render = render;
    view.output = output;
    view.projection = perspective_reversed_z(0.9F, 16.0F / 9.0F, 0.1F, 100.0F);
    view.relative_to_clip = view.projection;
    view.previous_relative_to_clip = view.projection;
    return view;
}

/// An upscaler that records what the frame handed it.
class FakeUpscaler final : public Upscaler {
public:
    FakeUpscaler(UpscalerFamily family, bool available) noexcept
        : family_(family), available_(available) {}

    [[nodiscard]] UpscalerFamily family() const noexcept override { return family_; }
    [[nodiscard]] const char* name() const noexcept override { return "fake"; }
    [[nodiscard]] bool available(const rhi::Device* /*device*/) const noexcept override {
        return available_;
    }
    [[nodiscard]] PassId declare(RenderGraph& /*graph*/,
                                 const ScreenSpaceStageInputs& inputs) noexcept override {
        seen_ = inputs;
        ++declared_;
        return 7U;
    }
    void executed() noexcept override { ++executed_; }

    UpscalerFamily family_;
    bool available_;
    ScreenSpaceStageInputs seen_{};
    u32 declared_ = 0;
    u32 executed_ = 0;
};

}  // namespace

CY_TEST_CASE("the modes render at their named scale, and parse back from their names") {
    CY_CHECK_EQ(upscale_mode_scale(UpscaleMode::NativeAntialiasing), 1.0F);
    CY_CHECK_EQ(upscale_mode_scale(UpscaleMode::Quality), 0.67F);
    CY_CHECK_EQ(upscale_mode_scale(UpscaleMode::Balanced), 0.58F);
    CY_CHECK_EQ(upscale_mode_scale(UpscaleMode::Performance), 0.5F);
    for (u32 index = 0; index < kUpscaleModeCount; ++index) {
        const auto mode = static_cast<UpscaleMode>(index);
        UpscaleMode parsed = UpscaleMode::Count;
        CY_CHECK(parse_upscale_mode(upscale_mode_name(mode), parsed));
        CY_CHECK_EQ(parsed, mode);
    }
    UpscaleMode untouched = UpscaleMode::Balanced;
    CY_CHECK_FALSE(parse_upscale_mode("ultra", untouched));
    CY_CHECK_FALSE(parse_upscale_mode(nullptr, untouched));
    CY_CHECK_EQ(untouched, UpscaleMode::Balanced);
}

CY_TEST_CASE("a mode's render extent is the output times its scale, rounded, within the output") {
    CY_CHECK(render_extent_for(k1440p, 0.67F) == Extent2D{1715, 965});
    CY_CHECK(render_extent_for(k1440p, 0.5F) == Extent2D{1280, 720});
    CY_CHECK(render_extent_for(k4K, 0.5F) == Extent2D{1920, 1080});
    CY_CHECK(render_extent_for(k4K, 0.58F) == Extent2D{2227, 1253});
    CY_CHECK(render_extent_for(k4K, 1.0F) == k4K);
    // Out of range is clamped rather than refused: above one is native, zero or below is native.
    CY_CHECK(render_extent_for(k4K, 1.5F) == k4K);
    CY_CHECK(render_extent_for(k4K, 0.0F) == k4K);
    CY_CHECK(render_extent_for(k4K, -0.5F) == k4K);
    // Never an empty axis.
    CY_CHECK(render_extent_for(Extent2D{3, 1}, 0.1F) == Extent2D{1, 1});
}

CY_TEST_CASE(
    "an upscale needs a jitter sequence as many times longer as the pixels it spreads over") {
    CY_CHECK_EQ(jitter_phase_count(2560, 2560), 8U);
    CY_CHECK_EQ(jitter_phase_count(2560, 1715), 18U);
    CY_CHECK_EQ(jitter_phase_count(3840, 2227), 24U);
    CY_CHECK_EQ(jitter_phase_count(3840, 1920), 32U);
    CY_CHECK_EQ(jitter_phase_count(3840, 0), 8U);
    const JitterConfig config = jitter_config_for(k1440p, Extent2D{1280, 720});
    CY_CHECK_EQ(config.length, 32U);
    CY_CHECK_EQ(config.base_x, 2U);
    CY_CHECK_EQ(config.base_y, 3U);
    // The bias selects the mip the OUTPUT pixel's footprint asks for.
    CY_CHECK_NEAR(texture_lod_bias(1280, 2560), -1.0F, 1e-6F);
    CY_CHECK_NEAR(texture_lod_bias(2560, 2560), 0.0F, 1e-6F);
    CY_CHECK_EQ(texture_lod_bias(0, 2560), 0.0F);
}

CY_TEST_CASE("dynamic resolution is the arbiter's allocation times the mode, within the bounds") {
    const DynamicResolutionBounds bounds;
    // At authored quality the mode decides.
    CY_CHECK_NEAR(dynamic_render_scale(UpscaleMode::Quality, 1.0F, bounds), 0.67F, 1e-6F);
    CY_CHECK_NEAR(dynamic_render_scale(UpscaleMode::NativeAntialiasing, 1.0F, bounds), 1.0F, 1e-6F);
    // The arbiter's ladder lowers it — and the bounds stop it.
    CY_CHECK_NEAR(dynamic_render_scale(UpscaleMode::NativeAntialiasing, 0.78F, bounds), 0.78F,
                  1e-6F);
    CY_CHECK_NEAR(dynamic_render_scale(UpscaleMode::Quality, 0.65F, bounds), 0.5F, 1e-6F);
    DynamicResolutionBounds narrow;
    narrow.minimum_scale = 0.6F;
    narrow.maximum_scale = 0.8F;
    CY_CHECK_NEAR(dynamic_render_scale(UpscaleMode::NativeAntialiasing, 1.0F, narrow), 0.8F, 1e-6F);
    CY_CHECK_NEAR(dynamic_render_scale(UpscaleMode::Performance, 1.0F, narrow), 0.6F, 1e-6F);
    // A non-positive allocation is no allocation.
    CY_CHECK_NEAR(dynamic_render_scale(UpscaleMode::Quality, 0.0F, bounds), 0.67F, 1e-6F);

    // The extent is snapped to the granule and follows the output's aspect; it never grows as the
    // allocation falls.
    u32 previous = k1440p.width + 1U;
    for (const f32 allocation : {1.0F, 0.92F, 0.85F, 0.78F, 0.71F, 0.65F}) {
        const Extent2D extent =
            dynamic_render_extent(k1440p, UpscaleMode::NativeAntialiasing, allocation, bounds);
        CY_CHECK_EQ(extent.width % bounds.granularity, 0U);
        CY_CHECK_LE(extent.width, previous);
        CY_CHECK_LE(extent.width, k1440p.width);
        const f32 aspect = static_cast<f32>(extent.width) / static_cast<f32>(extent.height);
        CY_CHECK_NEAR(aspect, 16.0F / 9.0F, 0.01F);
        previous = extent.width;
    }
    CY_CHECK(dynamic_render_extent(k1440p, UpscaleMode::NativeAntialiasing, 1.0F, bounds) ==
             k1440p);
}

CY_TEST_CASE("the constants carry the jitter as the sample's offset, and refuse what cannot run") {
    UpscaleView view = view_at(Extent2D{1280, 720}, k1440p);
    view.jitter = Vec2{0.25F, -0.125F};
    view.history_valid = true;
    const UpscaleSettings settings;
    const Expected<UpscaleConstants, Error> constants = make_upscale_constants(settings, view);
    CY_REQUIRE(constants.has_value());
    // A projection jittered right and up by (x, y) render pixels shows, at texel i's centre, the
    // surface the unjittered camera sees at i + 0.5 + (-x, +y) in texture orientation.
    CY_CHECK_EQ(constants->sample[0], -0.25F);
    CY_CHECK_EQ(constants->sample[1], -0.125F);
    CY_CHECK(constants->render() == Extent2D{1280, 720});
    CY_CHECK(constants->output() == k1440p);
    CY_CHECK_EQ(constants->extents[2], 0U);
    CY_CHECK_EQ(constants->flags(), kUpscaleHistoryValid | kUpscaleVelocity |
                                        kUpscaleRejectDisocclusion | kUpscaleShadingChange |
                                        kUpscaleLocks);
    // An identity reprojection: this frame's clip space IS last frame's.
    CY_CHECK_NEAR(constants->current_to_previous[0][0], 1.0F, 1e-5F);
    CY_CHECK_NEAR(constants->current_to_previous[2][3], 0.0F, 1e-5F);

    // Each mechanism a setting can turn off is a flag the shader reads.
    UpscaleSettings bare = settings;
    bare.reject_disocclusion = false;
    bare.detect_shading_change = false;
    bare.lock_thin_features = false;
    view.velocity = false;
    view.reactive = true;
    CY_CHECK_EQ(make_upscale_constants(bare, view)->flags(),
                kUpscaleHistoryValid | kUpscaleReactive);

    CY_CHECK_FALSE(make_upscale_constants(settings, view_at(Extent2D{0, 720}, k1440p)).has_value());
    CY_CHECK_FALSE(
        make_upscale_constants(settings, view_at(Extent2D{2561, 720}, k1440p)).has_value());
    UpscaleSettings bad = settings;
    bad.moving_history_weight = bad.max_history_weight + 1.0F;
    CY_CHECK_FALSE(make_upscale_constants(bad, view_at(Extent2D{1280, 720}, k1440p)).has_value());
    bad = settings;
    bad.sharpness = 1.5F;
    CY_CHECK_FALSE(make_upscale_constants(bad, view_at(Extent2D{1280, 720}, k1440p)).has_value());
    bad = settings;
    bad.exposure = 0.0F;
    CY_CHECK_FALSE(make_upscale_constants(bad, view_at(Extent2D{1280, 720}, k1440p)).has_value());
    UpscaleView singular = view_at(Extent2D{1280, 720}, k1440p);
    for (Vec4& column : singular.relative_to_clip.columns) {
        column = Vec4{0.0F, 0.0F, 0.0F, 0.0F};
    }
    CY_CHECK_FALSE(make_upscale_constants(settings, singular).has_value());

    // The packing round-trips the largest extent a sixteen-bit axis holds, and refuses a wider one.
    CY_CHECK(unpack_extent(pack_extent(Extent2D{65535, 4320})) == (Extent2D{65535, 4320}));
    CY_CHECK_FALSE(
        make_upscale_constants(settings, view_at(Extent2D{1280, 720}, Extent2D{70000, 1440}))
            .has_value());

    const SharpenConstants sharpen = make_sharpen_constants(settings, k1440p);
    CY_CHECK_EQ(sharpen.extent[0], 2560U);
    CY_CHECK_EQ(sharpen.control[0], settings.sharpness);
}

CY_TEST_CASE(
    "a vendor upscaler is chosen only when present and available, the built-in otherwise") {
    FakeUpscaler built_in(UpscalerFamily::BuiltIn, true);
    UpscalerRegistry registry(built_in);
    // ABSENT: no module for the family, so the built-in one, with no change to anything else.
    CY_CHECK_EQ(&registry.select(UpscalerFamily::Dlss, nullptr), &built_in);
    CY_CHECK_FALSE(registry.has(UpscalerFamily::Dlss));
    CY_CHECK(registry.has(UpscalerFamily::BuiltIn));

    FakeUpscaler fsr(UpscalerFamily::Fsr, true);
    FakeUpscaler xess(UpscalerFamily::Xess, false);
    CY_REQUIRE(registry.add(fsr).has_value());
    CY_REQUIRE(registry.add(xess).has_value());
    CY_CHECK_EQ(&registry.select(UpscalerFamily::Fsr, nullptr), &fsr);
    // PRESENT BUT UNAVAILABLE: its runtime is missing or the device cannot run it.
    CY_CHECK_EQ(&registry.select(UpscalerFamily::Xess, nullptr), &built_in);
    CY_CHECK_EQ(&registry.select(UpscalerFamily::BuiltIn, nullptr), &built_in);

    // The built-in family is not a vendor slot, and a slot is filled once.
    FakeUpscaler impostor(UpscalerFamily::BuiltIn, true);
    CY_CHECK_FALSE(registry.add(impostor).has_value());
    FakeUpscaler second(UpscalerFamily::Fsr, true);
    CY_CHECK_FALSE(registry.add(second).has_value());
    CY_CHECK_EQ(std::strcmp(upscaler_family_name(UpscalerFamily::MetalFx), "MetalFX"), 0);
}

CY_TEST_CASE("the stage declaration hands the frame's inputs to the upscaler and tells it it ran") {
    FakeUpscaler upscaler(UpscalerFamily::BuiltIn, true);
    const FrameStageDeclaration stage = stage_of(upscaler);
    CY_REQUIRE(stage.declare != nullptr);
    CY_REQUIRE(stage.executed != nullptr);
    ScreenSpaceStageInputs inputs;
    inputs.width = 2560;
    inputs.height = 1440;
    inputs.render_width = 1280;
    inputs.render_height = 720;
    RenderGraph graph(system_allocator(MemoryDomain::Renderer));
    CY_CHECK_EQ(stage.declare(graph, inputs, stage.user), PassId{7});
    CY_CHECK_EQ(upscaler.declared_, 1U);
    CY_CHECK_EQ(upscaler.seen_.render_width, 1280U);
    stage.executed(stage.user);
    CY_CHECK_EQ(upscaler.executed_, 1U);
}
