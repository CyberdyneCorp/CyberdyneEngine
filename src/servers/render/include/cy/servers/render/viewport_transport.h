#pragma once
// The viewport transport's engine-side endpoint: the image's identity, its view state, and what it
// cost. Task 4.1.
//
// `editor-viewport-and-gizmos` — "Viewport transport": the viewport "SHALL obtain its image through
// an **abstract transport**" with a local surface, a shared texture and an encoded stream, and:
//
//   "The transport SHALL carry, alongside the image, the **frame's view state and identifiers**, so
//    that picking, gizmo interaction, and overlay alignment are correct for the frame actually
//    presented rather than for the editor's current state."
//
//   "Latency and frame pacing SHALL be reported per transport, and the editor SHALL surface when it
//    is viewing a stale or degraded stream."
//
// ================================================================================================
// WHAT THIS FILE IS, AND THE LINE IT DOES NOT CROSS
// ================================================================================================
//
// This is the PUBLISHING half, and it is deliberately about the frame's DESCRIPTION rather than its
// pixels. `src/servers/` is layer 2: there is no device here, no swapchain, no image format the RHI
// named and no encoder. What a transport carries here is
//
//   * the frame's identity — one monotonically increasing number, the same one the control path's
//     `FrameId` reconciles against, so a drag's prediction and the image it was drawn over agree
//     about which frame they mean;
//   * the frame's view state — camera pose, projection, viewport rect, layer mask and debug view —
//     captured AT PUBLICATION, so a click resolves against the frame the user was looking at rather
//     than the one the editor's camera has since moved to;
//   * a handle to the image, whose meaning depends on the transport kind and whose bytes are the
//     business of the module that owns a device;
//   * what it cost and whether it is honest — pacing, latency, and the degradation that must be
//     surfaced rather than mistaken for the project's appearance.
//
// The three transport kinds differ in exactly one thing at this layer: what `image` means. A local
// surface and a shared texture name a texture the renderer produced; an encoded stream names none
// and carries `payload_bytes` instead. Every other member is identical across the three, which is
// the requirement that "a viewport feature SHALL work over any transport unless it documents a
// reason not to" expressed as a struct rather than as a promise.
//
// ================================================================================================
// WHY THE LATEST FRAME WINS, AND WHY THAT IS A CORRECTNESS PROPERTY
// ================================================================================================
//
// `publish()` overwrites. There is no queue, and a consumer that was busy for three frames sees the
// newest one rather than the oldest of three — the two frames it missed are counted in
// `ViewportPacing::superseded` and are gone.
//
// A queue would be worse in the exact way that matters. `editor-viewport-and-gizmos` requires the
// editor never to block its interface thread on runtime rendering, so the consumer runs at its own
// rate; a queue between two free-running clocks does not smooth anything, it accumulates. The
// milestone's control-path spike found the same shape on the other side of the boundary — a
// blocking design's p50 was fine and its tail was not — and a viewport queue's failure mode is
// worse than a dropped frame: the image a designer drags against grows steadily older while every
// individual measurement looks healthy. Dropping is visible in one counter; lag is not.
//
// ================================================================================================
// WHAT IS DELIBERATELY NOT HERE
// ================================================================================================
//
// THE CADENCE DECISION. "Each viewport SHALL declare its cost, and the editor SHALL be able to
// limit rendering of unfocused or hidden viewports" — deciding WHETHER a viewport should be
// rendered this frame is the editor deciding what should be shown, so it is the editor's, and it
// arrives here as `requested_interval_micros`. What this file does is record whether the request
// was met, because that is a measurement and measurements belong with the thing being measured.
//
// THE ENCODER, THE SHARED HANDLE AND THE PRESENTATION. All three need a device. This layer names
// the image with a `TextureHandle` and says nothing about how it got there.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/math/transform.h>
#include <cy/core/values/name.h>
#include <cy/servers/render/handles.h>
#include <cy/servers/render/model.h>
#include <cy/servers/render/types.h>

namespace cy::render {

/// How the image reaches the editor.
///
/// `editor-viewport-and-gizmos` names three and requires the editor to treat them uniformly. The
/// enum exists so that pacing and latency can be reported PER TRANSPORT, which is what makes "a
/// shared texture is faster than an encoded stream" a number in a report rather than an assumption
/// in a design document.
enum class ViewportTransportKind : u8 {
    /// The runtime is in the editor's own process and presents into an editor-owned surface.
    LocalSurface = 0,
    /// The runtime is a separate process on this machine, sharing the texture rather than copying.
    SharedTexture,
    /// The runtime is a remote device or a console; the image is compressed and input is forwarded.
    EncodedStream,
    Count,
};

[[nodiscard]] const char* viewport_transport_kind_name(ViewportTransportKind kind) noexcept;

/// Why an image is not what the project actually looks like.
///
/// "Degradation SHALL be surfaced to the user, so that a lower-quality image is never mistaken for
/// the project's appearance." A reason rather than a boolean, because the three reasons want three
/// different words in the interface and a boolean would make them one.
enum class ViewportDegradation : u8 {
    /// Full quality. What the game will look like.
    None = 0,
    /// Rendering below the requested rate to stay inside the budget.
    ReducedRate,
    /// Rendering below the viewport's pixel size and upscaling to fill it.
    ReducedResolution,
    /// Not rendering at all — an unfocused or hidden viewport. The last image stands.
    Paused,
    Count,
};

[[nodiscard]] const char* viewport_degradation_name(ViewportDegradation degradation) noexcept;

/// The frame's identity, monotonically increasing per transport, starting at 1.
///
/// Zero means "no frame has been published", which is why publication starts at 1: a consumer can
/// tell "I have never seen a frame" from "I have seen frame zero" without a second field.
using ViewportFrameId = u64;
inline constexpr ViewportFrameId kNoViewportFrame = 0;

/// The view state a frame was rendered with, captured at publication.
///
/// A COPY, not a handle. The `View` this comes from is mutated by the next frame's prepare stage,
/// so a consumer holding a reference would resolve its click against the camera the editor has
/// moved to rather than the one the image was drawn from — which is exactly the defect the
/// requirement's second scenario describes.
struct ViewportViewState {
    /// The view this frame belongs to, for a caller holding several.
    ViewHandle view;
    /// The view's history identity, which survives a rebuilt view list. See `ViewDescription`.
    u64 history_id = 0;
    Transform camera;
    Projection projection;
    ViewportRect viewport;
    LayerMask layer_mask = kAllLayers;
    DebugViewMode debug_mode = DebugViewMode::Off;

    /// Rebuild the derived `View` this state describes, so that a pick or an overlay can be
    /// resolved against the frame that was presented.
    ///
    /// THIS IS THE FUNCTION THAT MAKES "the hit SHALL be resolved against the view state of the
    /// frame shown" MECHANICAL. `picking.h` takes a `View`; a caller that built one from its
    /// current camera instead of from here would compile and would be wrong, so the transport hands
    /// out the frame's own.
    [[nodiscard]] View to_view() const noexcept;
};

/// One published frame: its identity, its view state, its image and what it cost.
struct PresentedViewportFrame {
    ViewportFrameId frame_id = kNoViewportFrame;
    ViewportViewState state;

    /// The image, for a local surface or a shared texture. Null for an encoded stream, whose bytes
    /// are not a texture this server owns.
    TextureHandle image;
    /// The encoded payload's size, for a stream. Zero for the other two kinds. Reported because a
    /// bitrate is the first thing anyone asks of a stream that feels bad.
    u32 payload_bytes = 0;

    /// The engine's monotonic clock, in microseconds, when the frame finished rendering. The
    /// consumer subtracts it from its own reading of the same clock to get the age of what it is
    /// looking at — which is the number the whole of this file exists to make available.
    u64 produced_micros = 0;

    ViewportDegradation degradation = ViewportDegradation::None;
    /// The fraction of the viewport's pixel size actually rendered. 1 at full resolution.
    f32 resolution_scale = 1.0F;

    [[nodiscard]] constexpr bool valid() const noexcept { return frame_id != kNoViewportFrame; }
    [[nodiscard]] constexpr bool full_quality() const noexcept {
        return degradation == ViewportDegradation::None && resolution_scale >= 1.0F;
    }
};

/// What the editor asked of this transport.
struct ViewportTransportBudget {
    /// The interval the editor wants frames at, in microseconds. A focused viewport asks for the
    /// display's interval; an unfocused one asks for less; a hidden one is not rendered at all and
    /// asks for nothing. Zero means "as fast as you can".
    u32 requested_interval_micros = 16'667;
    /// How old the newest frame may be before the editor is told it is looking at a stale image.
    ///
    /// Three requested intervals by default rather than one: one interval is normal jitter and
    /// warning about it would train a user to ignore the warning. It is the number
    /// `ViewportTransport::is_stale` compares against, and it is a budget rather than a constant
    /// because a remote transport's normal is not a local one's.
    u32 stale_after_micros = 50'000;
};

/// Pacing and latency, per transport.
struct ViewportPacing {
    /// Frames published.
    u64 published = 0;
    /// Frames a consumer never saw, because a newer one replaced them first. See the header comment
    /// for why this is a counter and not a queue.
    u64 superseded = 0;
    /// Frames whose interval exceeded the requested one. What "the budget was not met" means as a
    /// number.
    u64 late = 0;

    /// The interval between the last two publications, in microseconds.
    u32 last_interval_micros = 0;
    /// The longest interval seen. A mean hides exactly the stall a user notices.
    u32 worst_interval_micros = 0;
    /// The mean interval, in microseconds, over every publication since the last reset.
    u32 mean_interval_micros = 0;

    /// The age of the newest frame when the consumer last acknowledged one — the end-to-end number,
    /// covering production, transport and the consumer's own scheduling.
    u32 last_latency_micros = 0;
    u32 worst_latency_micros = 0;
    u32 mean_latency_micros = 0;
    /// Acknowledgements, which is what the two means above are divided by.
    u64 acknowledged = 0;
};

/// A viewport's image channel, from the engine's side.
///
/// NOT THREAD-SAFE, by the same rule as `RenderServer`: it is published to from the frame's present
/// stage and read by whatever hands the frame to the transport implementation, which is one thread.
/// A hosted runtime that publishes from its render thread and serialises from its bridge thread
/// passes the frame between them through its own queue, as `LiveReload` does for the same reason.
class ViewportTransport {
public:
    explicit ViewportTransport(ViewportTransportKind kind) noexcept : kind_(kind) {}

    [[nodiscard]] ViewportTransportKind kind() const noexcept { return kind_; }
    [[nodiscard]] const char* kind_name() const noexcept {
        return viewport_transport_kind_name(kind_);
    }

    /// What the editor asked for. Refused when `stale_after_micros` is zero, which would report
    /// every frame as stale the instant it arrived.
    [[nodiscard]] Status configure(const ViewportTransportBudget& budget) noexcept;
    [[nodiscard]] const ViewportTransportBudget& budget() const noexcept { return budget_; }

    /// Publish a frame, returning its identity.
    ///
    /// `view` is copied into the frame's state, so the caller may mutate it immediately afterwards
    /// — which it will, because the next frame's prepare stage runs before this one is consumed.
    ///
    /// `produced_micros` must not go backwards; an equal reading is accepted, because two frames
    /// can finish inside one tick of a coarse clock. A reading that DOES go backwards is refused
    /// with `ErrorCode::InvalidArgument` rather than silently producing a negative interval that
    /// would wrap into an enormous positive one and make a pacing report meaningless.
    [[nodiscard]] Expected<ViewportFrameId, Error> publish(const View& view, u64 produced_micros,
                                                           TextureHandle image,
                                                           u32 payload_bytes = 0) noexcept;

    /// Declare that the next published frame is degraded, and why.
    ///
    /// Set before `publish`, so that the reason travels WITH the frame it describes. A consumer
    /// that asked the transport separately would be told about the frame after the one it is
    /// looking at, and a quality indicator that is one frame out is one that flickers.
    void set_degradation(ViewportDegradation degradation, f32 resolution_scale) noexcept;

    /// The newest published frame. Invalid until something has been published.
    [[nodiscard]] const PresentedViewportFrame& latest() const noexcept { return latest_; }

    /// How old the newest frame is, in microseconds, at `now_micros`. Zero when nothing has been
    /// published, because "no frame" is not "a frame of age zero" and the caller distinguishes them
    /// with `latest().valid()`.
    [[nodiscard]] u64 age_micros(u64 now_micros) const noexcept;

    /// Whether the editor should be told it is looking at a stale image.
    [[nodiscard]] bool is_stale(u64 now_micros) const noexcept;

    /// Record that the consumer presented `frame_id` at `now_micros`.
    ///
    /// THE ONLY PLACE END-TO-END LATENCY IS MEASURED, and it needs the consumer to say so because
    /// nothing on this side knows when the editor drew it. An acknowledgement for a frame this
    /// transport never published is ignored rather than refused: a consumer reconnecting after a
    /// runtime restart legitimately holds a frame identifier from the previous runtime, and turning
    /// that into an error would make a restart look like a defect.
    void acknowledge(ViewportFrameId frame_id, u64 now_micros) noexcept;

    [[nodiscard]] const ViewportPacing& pacing() const noexcept { return pacing_; }
    /// Forget the statistics, keeping the frame. What a "reset counters" command calls, and what a
    /// measurement calls between cases.
    void reset_pacing() noexcept { pacing_ = ViewportPacing{}; }

private:
    ViewportTransportKind kind_;
    ViewportTransportBudget budget_;
    PresentedViewportFrame latest_;
    ViewportPacing pacing_;

    /// Whether `latest_` has been acknowledged. What `superseded` counts is publications that
    /// replaced an unacknowledged frame.
    bool latest_seen_ = true;

    ViewportDegradation next_degradation_ = ViewportDegradation::None;
    f32 next_resolution_scale_ = 1.0F;

    u64 total_interval_micros_ = 0;
    u64 total_latency_micros_ = 0;
};

}  // namespace cy::render
