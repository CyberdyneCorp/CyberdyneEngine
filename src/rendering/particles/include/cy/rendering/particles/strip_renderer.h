// SPDX-License-Identifier: MIT
#pragma once
// SPDX-License-Identifier: MIT
// RIBBONS, TRAILS AND BEAMS, COMPOSITED. M11.c task 6.3.
//
// ================================================================================================
// WHAT WAS MISSING
// ================================================================================================
//
// `src/vfx/README.md`, under "Deliberate limits": *"The six renderer kinds beyond `Sprite` and
// `Mesh` publish rows; nothing composites them ... no pass draws a ribbon strip."* `vfx-system`'s
// `publish_ribbons`, `publish_trails` and `publish_beams` each derive an ordered run of strip
// vertices from particle state, and until this file those vertices went nowhere.
//
// This is the renderer that draws them, and it is `ParticleRenderer`'s sibling in every respect
// that matters: the frame's own set layouts and push range (see `detail/transparent_draw.h`), a
// ring per frame in flight at set 2, a `PassExtension` on the frame's TRANSPARENT stage, and ONE
// DRAW for every strip in the frame however many strips there are.
//
// ================================================================================================
// THE SEAM IS A STRIP VERTEX, AND THIS MODULE DOES NOT KNOW WHAT A PARTICLE IS
// ================================================================================================
//
// `cy::vfx` links this module and not the other way round, so the record here is the renderer's —
// a camera-relative position, a half-width, a radiance, where along its strip the vertex is and
// which strip it belongs to — and `cy::vfx::to_strip_vertices` is where a ribbon, trail or beam
// vertex becomes one. The three kinds differ in how their strips are DERIVED, which is the
// simulation's business; once derived, a strip is a strip.
//
// ================================================================================================
// A STRIP ENDS WHERE ITS IDENTIFIER CHANGES
// ================================================================================================
//
// The draw is one quad per neighbouring pair of vertices, and a pair whose two strip identifiers
// differ collapses to a point in the vertex shader (`cy/strip.slang`). So the ring is simply every
// strip's vertices back to back, and `vfx-system`'s "a ribbon killed mid-chain SHALL TERMINATE
// CLEANLY rather than connecting across the gap" holds in the picture as well as in the rows:
// `publish_ribbons` split the chain into two identifiers, and this is where the gap stays a gap.
// `StripReport::segments` counts the quads that are drawn, so the collapse is a number and not
// only a property of a picture.

#include <cy/backends/rhi/device.h>
#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/rendering/particles/detail/transparent_draw.h>
#include <cy/rendering/pipeline/frame_recorder.h>

namespace cy::rendering::particles {

/// One vertex of a strip, as a publication derives it and this module draws it. 48 bytes.
///
/// CAMERA-RELATIVE, like `ParticleInstance`, and for the same reason: there is no world-space
/// position here and nowhere to put one.
struct alignas(16) StripVertex {
    /// Metres, relative to the view's camera.
    f32 position[3] = {0.0F, 0.0F, 0.0F};
    /// Half the strip's width at this vertex, in metres. Zero is a strip that tapers to nothing.
    f32 half_width = 0.0F;
    /// Linear, un-premultiplied radiance. `a` is the opacity the shader fades along the strip and
    /// across it.
    f32 color[4] = {1.0F, 1.0F, 1.0F, 1.0F};
    /// Where along its strip this vertex is: 0 at the head, 1 at the tail.
    f32 along = 0.0F;
    /// Which strip this vertex belongs to. A strip is a maximal run of equal values.
    u32 strip = 0;
    f32 reserved[2] = {0.0F, 0.0F};
};

static_assert(sizeof(StripVertex) == 48);

/// What one recorded frame of strips did.
struct StripReport {
    /// Vertices in the ring.
    u32 vertices = 0;
    /// Strips among them: runs of equal `strip`.
    u32 strips = 0;
    /// Quads that join two vertices of ONE strip — what is actually drawn. `vertices - strips` when
    /// nothing was dropped, and a number that says the gaps between strips are not bridged.
    u32 segments = 0;
    /// Vertices the ring could not hold. Counted rather than refused, as `ParticleReport` does.
    u32 dropped = 0;
    u32 draws = 0;
};

/// The strip pipeline, its ring, and the extension that puts it in the frame.
class StripRenderer {
public:
    StripRenderer() noexcept = default;
    ~StripRenderer();

    StripRenderer(const StripRenderer&) = delete;
    StripRenderer& operator=(const StripRenderer&) = delete;
    StripRenderer(StripRenderer&&) = delete;
    StripRenderer& operator=(StripRenderer&&) = delete;

    /// Create the pipeline and the ring. `pipelines` must be initialized; see `ParticleRenderer`.
    [[nodiscard]] Status initialize(rhi::Device& device, const pipeline::FramePipelines& pipelines,
                                    u32 capacity) noexcept;
    void shutdown() noexcept;

    [[nodiscard]] bool ready() const noexcept { return ready_; }
    [[nodiscard]] u32 capacity() const noexcept { return capacity_; }

    /// Write one frame's strips into the ring slot `begin_frame()` returned, strip after strip.
    ///
    /// CALL IT INSIDE THE HOST'S DEVICE FRAME and before the frame executes, as
    /// `ParticleRenderer::upload` says. A ring too small for every vertex keeps the leading ones
    /// and counts the rest in `StripReport::dropped`; the last strip it keeps may be shortened,
    /// never joined to another.
    [[nodiscard]] Status upload(u32 frame_slot, Span<const StripVertex> vertices) noexcept;

    /// The extension to hand `FrameRecorder::add_extension`, on the TRANSPARENT stage.
    [[nodiscard]] pipeline::PassExtension extension() noexcept;

    [[nodiscard]] const StripReport& report() const noexcept { return report_; }
    void reset_report() noexcept { report_ = StripReport{}; }

    // --- What the extension's callback reads.

    [[nodiscard]] const detail::TransparentDraw& draw() const noexcept { return draw_; }
    [[nodiscard]] u32 live() const noexcept { return live_; }
    [[nodiscard]] StripReport& mutable_report() noexcept { return report_; }

private:
    detail::TransparentDraw draw_;
    u32 capacity_ = 0;
    u32 live_ = 0;
    StripReport report_;
    bool ready_ = false;
};

/// The strips and drawn segments in `vertices` — the numbers `StripRenderer::upload` reports,
/// exposed so a caller with no device can state them too.
[[nodiscard]] StripReport count_strips(Span<const StripVertex> vertices) noexcept;

}  // namespace cy::rendering::particles
