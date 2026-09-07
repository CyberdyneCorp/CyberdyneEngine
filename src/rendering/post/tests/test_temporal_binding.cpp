// The post chain consumes the temporal framework rather than growing one of its own. Tasks 8.3
// and 8.4.
//
// `rendering-post-processing`: TAA "SHALL consume the temporal framework… **It SHALL NOT implement
// its own.**" A test that only checked TAA worked would pass against a chain with its own history
// buffer, so what is asserted here is the consumption: the history is the framework's, the
// invalidation reaches it without the chain doing anything, and a chain with no temporal stage
// leaves the projection unjittered because it registered nobody.

#include <cy/test/test.h>

#include <cy/core/math/projection.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/rendering/post/temporal_binding.h>

namespace {

using cy::rendering::bind_temporal;
using cy::rendering::PostChainConfig;
using cy::rendering::PostStage;
using cy::rendering::TemporalBinding;
using cy::rendering::TemporalConfig;
using cy::rendering::TemporalFramework;
using cy::rendering::TemporalInvalidation;
using cy::rendering::TemporalView;

cy::Allocator& allocator() noexcept {
    return cy::system_allocator(cy::MemoryDomain::Renderer);
}

TemporalView view(cy::u32 width = 1920, cy::u32 height = 1080) noexcept {
    TemporalView state;
    state.width = width;
    state.height = height;
    state.view = cy::look_at(cy::Vec3{0.0F, 0.0F, 0.0F}, cy::Vec3{0.0F, 0.0F, -1.0F});
    state.projection = cy::perspective_reversed_z(
        cy::math::radians(60.0F), static_cast<cy::f32>(width) / static_cast<cy::f32>(height), 0.1F,
        1000.0F);
    return state;
}

}  // namespace

CY_TEST_CASE("a chain with no temporal stage registers nobody, so the projection is unjittered") {
    TemporalFramework framework(allocator());
    CY_REQUIRE(framework.initialize(TemporalConfig{}).has_value());

    const cy::Expected<TemporalBinding, cy::Error> binding =
        bind_temporal(framework, PostChainConfig{});
    CY_REQUIRE(binding.has_value());
    CY_CHECK_FALSE(binding->consumer.valid());
    CY_CHECK_FALSE(binding->colour_history.valid());
    CY_CHECK_EQ(binding->stage, PostStage::Count);

    framework.begin_frame(view());
    CY_CHECK_FALSE(framework.jitter().enabled());
    CY_CHECK(framework.jittered_view_projection() == framework.view().view_projection());
}

CY_TEST_CASE(
    "the temporal stage's history belongs to the framework, and the chain never owns one") {
    TemporalFramework framework(allocator());
    CY_REQUIRE(framework.initialize(TemporalConfig{}).has_value());
    framework.begin_frame(view());

    PostChainConfig config;
    config.temporal_antialiasing = true;
    config.motion_vectors_available = true;
    const cy::Expected<TemporalBinding, cy::Error> binding = bind_temporal(framework, config);
    CY_REQUIRE(binding.has_value());
    CY_CHECK_EQ(binding->stage, PostStage::TemporalAntiAliasing);
    CY_REQUIRE(binding->consumer.valid());
    CY_REQUIRE(binding->colour_history.valid());
    CY_REQUIRE(binding->depth_history.valid());

    // The resources are the framework's, and it is the framework that reports what they cost.
    CY_CHECK(framework.history(binding->colour_history) != nullptr);
    CY_CHECK_GT(framework.history_bytes(binding->consumer), 0ULL);
    CY_CHECK_EQ(framework.history(binding->colour_history)->consumer, binding->consumer.value);

    // Jitter is on because a consumer asked for it, not because the chain switched it on.
    framework.begin_frame(view());
    CY_CHECK(framework.jitter().enabled());

    // And a cut reaches the chain's history without the chain doing anything at all: no code in
    // src/rendering/post/ observes an invalidation, which is the whole of "it shall not implement
    // its own".
    framework.mark_history_written(binding->colour_history);
    CY_REQUIRE(framework.history(binding->colour_history)->valid);
    framework.signal_cut(TemporalInvalidation::CameraCut);
    framework.begin_frame(view());
    CY_CHECK_FALSE(framework.history(binding->colour_history)->valid);
}

CY_TEST_CASE(
    "an upscaler's depth history follows the internal resolution and its colour does not") {
    TemporalFramework framework(allocator());
    CY_REQUIRE(framework.initialize(TemporalConfig{}).has_value());
    framework.begin_frame(view(1920, 1080));

    PostChainConfig config;
    config.temporal_upscaling = true;
    config.motion_vectors_available = true;
    // The scale is the ARBITER's — `rendering-post-processing` says internal resolution is a budget
    // allocation the arbiter holds, "not an independent controller measuring frame time" — so it
    // arrives as a parameter and is never decided in this module.
    const cy::Expected<TemporalBinding, cy::Error> binding = bind_temporal(framework, config, 0.5F);
    CY_REQUIRE(binding.has_value());
    CY_CHECK_EQ(binding->stage, PostStage::TemporalUpscaling);
    CY_CHECK_NEAR(binding->resolution_scale, 0.5F, 1e-6F);

    // Colour history at OUTPUT resolution: that is what the upscaler reconstructs into, and a
    // history at the internal scale would give it nowhere to accumulate detail.
    CY_CHECK_EQ(framework.history(binding->colour_history)->width, 1920U);
    // Depth history at the internal one, because that is where the depth was written and resampling
    // it would move the edges the disocclusion test is looking at.
    CY_CHECK_EQ(framework.history(binding->depth_history)->width, 960U);

    // The framework resizes both when the output changes; the chain is not told and does not care.
    framework.begin_frame(view(1280, 720));
    CY_CHECK_EQ(framework.history(binding->colour_history)->width, 1280U);
    CY_CHECK_EQ(framework.history(binding->depth_history)->width, 640U);
    CY_CHECK(framework.invalidated_this_frame());

    // Both stages at once is refused here as well as by `build_post_chain()`, because this entry
    // point can be reached without that one.
    config.temporal_antialiasing = true;
    CY_CHECK_FALSE(bind_temporal(framework, config).has_value());
}
