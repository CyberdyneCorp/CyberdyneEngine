// The viewport transport's engine-side endpoint. See cy/servers/render/viewport_transport.h.

#include <cy/servers/render/viewport_transport.h>

#include <cy/core/math/scalar.h>

namespace cy::render {
namespace {

constexpr const char* kTransportNames[] = {"local-surface", "shared-texture", "encoded-stream"};
static_assert(sizeof(kTransportNames) / sizeof(kTransportNames[0]) ==
                  static_cast<usize>(ViewportTransportKind::Count),
              "every transport kind has a name; a kind added without one reports the wrong string");

constexpr const char* kDegradationNames[] = {"full", "reduced-rate", "reduced-resolution",
                                             "paused"};
static_assert(sizeof(kDegradationNames) / sizeof(kDegradationNames[0]) ==
                  static_cast<usize>(ViewportDegradation::Count),
              "every degradation reason has a name the interface can show");

/// Fold one sample into a running mean held as a total and a count.
///
/// The total is a `u64` of microseconds, which overflows after roughly 584,000 years of viewport —
/// a bound worth stating once rather than a running average worth explaining, because an
/// exponentially weighted mean would make "the mean interval" depend on when the reader asked.
[[nodiscard]] u32 mean_of(u64 total, u64 count) noexcept {
    return (count == 0) ? 0U : static_cast<u32>(total / count);
}

/// Clamp a microsecond difference into the `u32` the pacing report holds.
///
/// A gap longer than seventy-one minutes is a runtime that was suspended, not a pacing measurement,
/// and saturating keeps one such gap from wrapping into a small number that reads as healthy.
[[nodiscard]] u32 saturating_micros(u64 delta) noexcept {
    constexpr u64 kMax = 0xFFFF'FFFFULL;
    return static_cast<u32>((delta > kMax) ? kMax : delta);
}

}  // namespace

const char* viewport_transport_kind_name(ViewportTransportKind kind) noexcept {
    const auto index = static_cast<usize>(kind);
    return (index < static_cast<usize>(ViewportTransportKind::Count)) ? kTransportNames[index]
                                                                      : "unknown";
}

const char* viewport_degradation_name(ViewportDegradation degradation) noexcept {
    const auto index = static_cast<usize>(degradation);
    return (index < static_cast<usize>(ViewportDegradation::Count)) ? kDegradationNames[index]
                                                                    : "unknown";
}

View ViewportViewState::to_view() const noexcept {
    View rebuilt;
    rebuilt.desc.purpose = ViewPurpose::EditorViewport;
    rebuilt.desc.camera = camera;
    rebuilt.desc.projection = projection;
    rebuilt.desc.viewport = viewport;
    rebuilt.desc.layer_mask = layer_mask;
    rebuilt.desc.debug_mode = debug_mode;
    rebuilt.desc.history_id = history_id;
    // The derived members — the matrices, the frustum and the camera-relative origin — are
    // recomputed rather than carried, so this state is the seven authored fields and cannot
    // disagree with itself. `refresh()` is the same function the frame's prepare stage calls.
    rebuilt.refresh();
    return rebuilt;
}

Status ViewportTransport::configure(const ViewportTransportBudget& budget) noexcept {
    if (budget.stale_after_micros == 0) {
        return fail(ErrorCode::InvalidArgument,
                    "a stale threshold of zero reports every frame as stale on arrival");
    }
    budget_ = budget;
    return ok();
}

void ViewportTransport::set_degradation(ViewportDegradation degradation,
                                        f32 resolution_scale) noexcept {
    next_degradation_ = degradation;
    next_resolution_scale_ = math::clamp(resolution_scale, 0.0F, 1.0F);
}

Expected<ViewportFrameId, Error> ViewportTransport::publish(const View& view, u64 produced_micros,
                                                            TextureHandle image,
                                                            u32 payload_bytes) noexcept {
    if (latest_.valid() && produced_micros < latest_.produced_micros) {
        return fail(ErrorCode::InvalidArgument,
                    "a frame was published with a clock reading older than the previous frame's");
    }

    if (latest_.valid()) {
        const u32 interval = saturating_micros(produced_micros - latest_.produced_micros);
        pacing_.last_interval_micros = interval;
        pacing_.worst_interval_micros = math::max(pacing_.worst_interval_micros, interval);
        total_interval_micros_ += interval;
        pacing_.mean_interval_micros = mean_of(total_interval_micros_, pacing_.published);
        if (budget_.requested_interval_micros != 0 &&
            interval > budget_.requested_interval_micros) {
            ++pacing_.late;
        }
    }
    if (!latest_seen_) {
        // The consumer never acknowledged the frame this one replaces. See the header: dropping is
        // the correct behaviour and counting it is what keeps the drop visible.
        ++pacing_.superseded;
    }

    PresentedViewportFrame frame;
    frame.frame_id = latest_.frame_id + 1U;
    frame.state.view = ViewHandle{};
    frame.state.history_id = view.desc.history_id;
    frame.state.camera = view.desc.camera;
    frame.state.projection = view.desc.projection;
    frame.state.viewport = view.desc.viewport;
    frame.state.layer_mask = view.desc.layer_mask;
    frame.state.debug_mode = view.desc.debug_mode;
    frame.image = image;
    frame.payload_bytes = payload_bytes;
    frame.produced_micros = produced_micros;
    frame.degradation = next_degradation_;
    frame.resolution_scale = next_resolution_scale_;

    latest_ = frame;
    latest_seen_ = false;
    ++pacing_.published;
    return latest_.frame_id;
}

u64 ViewportTransport::age_micros(u64 now_micros) const noexcept {
    if (!latest_.valid() || now_micros <= latest_.produced_micros) {
        return 0;
    }
    return now_micros - latest_.produced_micros;
}

bool ViewportTransport::is_stale(u64 now_micros) const noexcept {
    if (!latest_.valid()) {
        // Nothing has ever arrived. That is not staleness — it is a transport that has not started,
        // and the editor says something different about it. `latest().valid()` is the question.
        return false;
    }
    return age_micros(now_micros) > budget_.stale_after_micros;
}

void ViewportTransport::acknowledge(ViewportFrameId frame_id, u64 now_micros) noexcept {
    if (frame_id != latest_.frame_id || !latest_.valid()) {
        return;
    }
    latest_seen_ = true;
    const u32 latency = saturating_micros(age_micros(now_micros));
    pacing_.last_latency_micros = latency;
    pacing_.worst_latency_micros = math::max(pacing_.worst_latency_micros, latency);
    ++pacing_.acknowledged;
    total_latency_micros_ += latency;
    pacing_.mean_latency_micros = mean_of(total_latency_micros_, pacing_.acknowledged);
}

}  // namespace cy::render
