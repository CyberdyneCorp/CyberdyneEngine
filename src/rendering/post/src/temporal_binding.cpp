#include <cy/rendering/post/temporal_binding.h>

#include <cy/core/math/scalar.h>

namespace cy::rendering {

Expected<TemporalBinding, Error> bind_temporal(TemporalFramework& framework,
                                               const PostChainConfig& config,
                                               f32 internal_resolution_scale) noexcept {
    TemporalBinding binding;
    if (config.temporal_antialiasing && config.temporal_upscaling) {
        return fail(ErrorCode::InvalidArgument,
                    "post: temporal antialiasing and temporal upscaling are the same step");
    }
    if (!config.temporal_antialiasing && !config.temporal_upscaling) {
        // Nothing registered, so the framework's own answer to "does anybody need jitter" stays
        // false and the projection is unjittered. That is the requirement, not an early return.
        return binding;
    }

    binding.stage =
        config.temporal_upscaling ? PostStage::TemporalUpscaling : PostStage::TemporalAntiAliasing;
    binding.resolution_scale =
        config.temporal_upscaling ? math::clamp(internal_resolution_scale, 0.25F, 1.0F) : 1.0F;

    const Expected<ConsumerId, Error> consumer = framework.register_consumer(
        config.temporal_upscaling ? "post.temporal_upscaling" : "post.temporal_antialiasing", true);
    if (!consumer) {
        return make_unexpected(consumer.error());
    }
    binding.consumer = consumer.value();

    HistoryDeclaration colour;
    colour.format = HistoryFormat::Rgba16F;
    // The history is kept at the OUTPUT resolution even when the scene is rendered smaller: that is
    // what a temporal upscaler reconstructs into, and declaring it at the internal scale would give
    // the upscaler a history it cannot accumulate detail in.
    colour.resolution_scale = 1.0F;
    colour.frames = 2;
    const Expected<HistoryId, Error> colour_history =
        framework.declare_history(binding.consumer, colour);
    if (!colour_history) {
        return make_unexpected(colour_history.error());
    }
    binding.colour_history = colour_history.value();

    HistoryDeclaration depth;
    depth.format = HistoryFormat::R32F;
    // Depth history is at the INTERNAL resolution, because that is where the depth was written and
    // resampling it would move the very edges the disocclusion test is looking at.
    depth.resolution_scale = binding.resolution_scale;
    depth.frames = 2;
    const Expected<HistoryId, Error> depth_history =
        framework.declare_history(binding.consumer, depth);
    if (!depth_history) {
        return make_unexpected(depth_history.error());
    }
    binding.depth_history = depth_history.value();
    return binding;
}

}  // namespace cy::rendering
