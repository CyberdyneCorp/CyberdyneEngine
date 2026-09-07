#pragma once
// The seam between the post chain and the temporal framework. Tasks 8.3 and 8.4.
//
// `rendering-post-processing` on anti-aliasing: "TAA SHALL consume the **temporal framework** (see
// `temporal-rendering`) for jitter, motion vectors, history storage, reprojection, disocclusion
// classification, and history invalidation. **It SHALL NOT implement its own.**" And on temporal
// upscaling: the jitter, motion vectors and exposure an upscaler takes are "supplied by the
// temporal framework".
//
// That sentence is only true if the post chain has no way to store history of its own, and this
// file is what makes it true: the chain's temporal stage gets its history by ASKING, through a
// `ConsumerId` only `TemporalFramework::register_consumer()` can mint. There is nothing in
// `src/rendering/post/` that allocates a history buffer, and no second jitter sequence anywhere in
// the module — which is the property M7 `design.md` §5 puts first among the things that must not be
// retrofitted.
//
// The binding is also where "no consumer, no jitter" becomes true for the post chain: a chain with
// neither temporal stage registers nothing, so the framework's own answer to "does anybody need
// jitter" comes back false and the projection is unjittered.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/rendering/post/chain.h>
#include <cy/rendering/temporal/framework.h>

namespace cy::rendering {

/// What the chain got from the framework. Every handle is the framework's; this structure owns
/// nothing.
struct TemporalBinding {
    /// Invalid when the chain has no temporal stage.
    ConsumerId consumer;
    /// The reconstruction history — scene colour at the internal resolution. Invalid when there is
    /// no temporal stage.
    HistoryId colour_history;
    /// The depth history the disocclusion classification reads.
    HistoryId depth_history;
    /// Which stage was bound, or `PostStage::Count` when neither was enabled.
    PostStage stage = PostStage::Count;
    /// The internal resolution scale the upscaler renders at. One for TAA.
    f32 resolution_scale = 1.0F;
};

/// Register the chain's temporal stage with the framework and declare the history it needs.
///
/// A chain with no temporal stage returns an empty binding and registers nothing, which is what
/// leaves the projection unjittered. A chain with both is refused by `build_post_chain()` before
/// this is ever reached, so it is not re-checked here.
///
/// `internal_resolution_scale` is the arbiter's, not this module's: `rendering-post-processing`
/// says internal resolution "SHALL be a budget allocation held by the renderer budget arbiter…
/// not an independent controller measuring frame time", so it arrives as a parameter and is never
/// decided here.
[[nodiscard]] Expected<TemporalBinding, Error> bind_temporal(
    TemporalFramework& framework, const PostChainConfig& config,
    f32 internal_resolution_scale = 1.0F) noexcept;

}  // namespace cy::rendering
