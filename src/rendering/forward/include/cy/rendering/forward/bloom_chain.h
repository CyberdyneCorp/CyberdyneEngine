// SPDX-License-Identifier: MIT
#pragma once
// Bloom's graph passes: a progressive downsample and upsample chain, and the composite. Step 9 of
// `rendering-post-processing`'s chain.
//
// ================================================================================================
// WHAT THIS DECLARES
// ================================================================================================
//
// `rendering-post-processing`'s Bloom requirement: "a progressive downsample and upsample chain
// with a soft threshold knee… a Karis average on the first downsample to suppress fireflies, and a
// tent filter on upsample". For `n` levels:
//
//     prefilter     scene (full)  -> down[0]   (1/2)   threshold + 13-tap + Karis average
//     downsample    down[k-1]     -> down[k]   (1/2^(k+1))                13-tap
//     upsample      down[k], up[k+1] (or down[n-1]) -> up[k]              tent, lerp by scatter
//     composite     scene, up[0] (or down[0] when n is 1) -> bloomed (full)
//
// EVERY LEVEL IS ITS OWN GRAPH TEXTURE rather than a mip of one texture, so every pass writes a
// whole resource and reads whole resources: the graph derives each barrier from a declaration with
// no subresource range in it, and the transients alias with the rest of the frame's.
//
// The composite writes a NEW full-resolution target rather than blending into the scene colour. The
// scene colour is read by the prefilter and by the composite; writing it in between would be a
// read-modify-write of a texture two passes of the same chain sample, and a separate target keeps
// every pass a pure function of its inputs.
//
// ================================================================================================
// THIS FILE DECLARES; IT DOES NOT RECORD
// ================================================================================================
//
// The same division `ForwardFrame` keeps. Every pass carries the one callback the caller supplied
// for `FramePassKind::Bloom`, and the callback finds which step it is recording through
// `BloomChain::step_of(context.pass)` — a step carries everything a recording needs: which inputs,
// which target, and both extents. `cy::rendering::pipeline::BloomRenderer` is the recorder the
// engine ships.

#include <cy/backends/rhi/types.h>
#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/rendering/graph/graph.h>

namespace cy::rendering {

struct FramePassCallback;

/// The deepest chain a frame declares. Six is the default and eight reaches a 1-texel level from a
/// 4K frame's 2160 rows.
inline constexpr u32 kMaxBloomLevels = 8;

/// What one pass of the chain does.
enum class BloomStepKind : u8 {
    /// Threshold, 13-tap downsample, Karis average. Reads the scene, writes level 0.
    Prefilter = 0,
    /// 13-tap downsample. Reads level k-1, writes level k.
    Downsample,
    /// Tent upsample of the coarser level, blended over this level's downsample. Writes up[k].
    Upsample,
    /// The redistribution: the scene, minus the scattered part of itself, plus the bloom.
    Composite,
    Count,
};

[[nodiscard]] const char* bloom_step_name(BloomStepKind kind) noexcept;

/// One declared pass, with everything its recording reads.
struct BloomStep {
    BloomStepKind kind = BloomStepKind::Count;
    /// The level written — for `Composite`, the level its bloom input came from.
    u32 level = 0;
    PassId pass = kInvalidPass;
    /// The input every step samples: the scene for `Prefilter` and `Composite`, level k-1 for
    /// `Downsample`, this level's downsample for `Upsample`.
    ResourceId source = kInvalidResource;
    /// The second input: the coarser level for `Upsample`, the bloom for `Composite`.
    ResourceId detail = kInvalidResource;
    ResourceId target = kInvalidResource;
    u32 source_width = 0;
    u32 source_height = 0;
    u32 detail_width = 0;
    u32 detail_height = 0;
    u32 target_width = 0;
    u32 target_height = 0;
};

inline constexpr u32 kMaxBloomSteps = (kMaxBloomLevels * 2U);
static_assert(kMaxBloomLevels == 8, "BloomChain's level arrays are initialised element by element");

/// The declared chain. Empty (`levels == 0`) when the frame has no bloom.
struct BloomChain {
    u32 levels = 0;
    u32 step_count = 0;
    BloomStep steps[kMaxBloomSteps] = {};
    /// Level k's downsample and upsample. `kInvalidResource` past `levels`, and for the coarsest
    /// level's `up`, which does not exist.
    ResourceId down[kMaxBloomLevels] = {kInvalidResource, kInvalidResource, kInvalidResource,
                                        kInvalidResource, kInvalidResource, kInvalidResource,
                                        kInvalidResource, kInvalidResource};
    ResourceId up[kMaxBloomLevels] = {kInvalidResource, kInvalidResource, kInvalidResource,
                                      kInvalidResource, kInvalidResource, kInvalidResource,
                                      kInvalidResource, kInvalidResource};
    /// The composite's target: the scene colour with the bloom redistributed into it.
    ResourceId output = kInvalidResource;

    /// The step a pass records, or null for a pass that is not one of this chain's.
    [[nodiscard]] const BloomStep* step_of(PassId pass) const noexcept;
};

/// The extent of level `level` (0 is half resolution), never below one texel.
[[nodiscard]] u32 bloom_level_extent(u32 full, u32 level) noexcept;

/// How many levels a chain over `width` x `height` gets when `requested` are asked for: at most
/// `kMaxBloomLevels`, and never so many that the coarsest level's shorter side is under two texels,
/// below which a 13-tap footprint is mostly the clamp at the edge. At least one.
[[nodiscard]] u32 bloom_level_count(u32 width, u32 height, u32 requested) noexcept;

/// Declare the chain over `source` into `graph`. `callback` is attached to every pass.
///
/// Refuses a zero extent and an undefined format rather than declaring a chain that renders
/// nothing. `out` is overwritten.
[[nodiscard]] Status declare_bloom_chain(RenderGraph& graph, ResourceId source, u32 width,
                                         u32 height, rhi::Format format, u32 requested_levels,
                                         const FramePassCallback& callback,
                                         BloomChain& out) noexcept;

}  // namespace cy::rendering
