#pragma once
// CyberTemporal: the ONE framework. Task 8.3, and the word "one" is the requirement.
//
// `temporal-rendering` — "One temporal framework": temporal reprojection infrastructure SHALL be a
// single engine subsystem, not reimplemented per effect, and it SHALL own the jitter sequence,
// motion vectors, history allocation and lifetime, reprojection, camera-cut detection, disocclusion
// classification, and history invalidation. Temporal antialiasing, temporal upscaling, screen-space
// reflections, screen-space global illumination, ambient occlusion, volumetric integration and
// shadow caching consume it.
//
// M7 `design.md` §5 puts it first in the table of things that must not be retrofitted: "Five
// reprojections that disagree about history invalidation is the defect that cannot be found from a
// screenshot."
//
// ================================================================================================
// HOW THE INTERFACE ENFORCES "ONE"
// ================================================================================================
//
// A rule that is only written down gets broken by the next pass somebody adds in a hurry. Three
// things here make the single framework the path of least resistance rather than a convention:
//
//   1. **A consumer cannot get history without registering.** `declare_history()` takes a
//      `ConsumerId` that only `register_consumer()` mints, so the memory report and the
//      invalidation broadcast cannot have a member the framework does not know about.
//   2. **A consumer cannot get jitter without asking for it, and cannot apply it.** `jitter()` is
//      read-only and `needs_jitter` is declared at registration, which is what makes "WHEN no
//      active effect requires jitter THEN the projection SHALL be unjittered" a computed answer
//      rather than a flag somebody has to remember to clear.
//   3. **Invalidation is broadcast, never polled.** `begin_frame()` compares this frame's view
//      against the last one and invalidates EVERY history in the same frame. A consumer has no way
//      to be told late, because there is no per-consumer notification to miss.
//
// ================================================================================================
// WHAT IS A CUT, AND WHY THE FRAMEWORK DECIDES
// ================================================================================================
//
// "Invalidation SHALL be a framework responsibility rather than a per-effect one, because a missed
// cut is the most common and most visible temporal artefact." The framework detects a projection
// change, a resolution change and a camera translation larger than a declared threshold, and takes
// an explicit `signal_cut()` from gameplay or a cinematic. It does NOT try to be clever about scene
// content: a heuristic that guesses at a cut from image statistics is a heuristic that guesses
// wrong during an explosion, and the explicit signal exists so that it does not have to.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/math/matrix.h>
#include <cy/core/memory/array.h>
#include <cy/rendering/temporal/history.h>
#include <cy/rendering/temporal/jitter.h>
#include <cy/rendering/temporal/motion.h>
#include <cy/rendering/temporal/reprojection.h>

namespace cy::rendering {

/// The causes `temporal-rendering` enumerates, plus the one the framework adds for its own
/// bookkeeping. Reported, because "invalidation events and their causes" is a required diagnostic.
enum class TemporalInvalidation : u8 {
    None = 0,
    CameraCut,
    Teleport,
    ProjectionChange,
    ResolutionChange,
    SceneLoad,
    /// An application-triggered cut. `signal_cut()`.
    Explicit,
    Count,
};

[[nodiscard]] const char* temporal_invalidation_name(TemporalInvalidation cause) noexcept;

/// What the framework is told about the frame. Deliberately not a camera: this is the subset the
/// framework reasons about, and a full camera here would make the framework a second owner of one.
struct TemporalView {
    u32 width = 0;
    u32 height = 0;
    Mat4 view = Mat4::identity();
    /// Unjittered. The framework applies jitter; a caller that pre-jittered would produce motion
    /// vectors carrying a sub-pixel dither — see `motion.h`.
    ///
    /// Separate from `view` rather than pre-multiplied, because "the projection changed" and "the
    /// camera moved" are different invalidation causes and a combined matrix cannot tell them
    /// apart. A field-of-view change mid-shot must invalidate history; walking must not.
    Mat4 projection = Mat4::identity();
    Vec3 camera_position{0.0F, 0.0F, 0.0F};

    [[nodiscard]] Mat4 view_projection() const noexcept { return projection * view; }
};

struct ConsumerId {
    u32 value = 0xFFFFFFFFU;

    [[nodiscard]] constexpr bool valid() const noexcept { return value != 0xFFFFFFFFU; }
};

struct TemporalConfig {
    JitterConfig jitter;
    /// A camera that moves further than this in one frame is a cut. Metres. A cinematic cut and a
    /// gameplay teleport should both signal explicitly; this is the safety net for the ones that
    /// do not.
    f32 teleport_distance = 25.0F;
    /// Depth tolerance handed to `classify_history()` when a consumer does not supply one.
    f32 depth_tolerance = 0.05F;
};

struct TemporalStatistics {
    /// Invalidation events since the last reset, by cause.
    u32 invalidations[static_cast<usize>(TemporalInvalidation::Count)] = {};
    /// The cause of the most recent one. What a developer chasing a smear reads first.
    TemporalInvalidation last_cause = TemporalInvalidation::None;
    u64 last_invalidated_frame = 0;
    /// History memory in use, in bytes, in total and per consumer.
    u64 history_bytes = 0;
    /// Pixel classification counts for the frame, as recorded by consumers.
    ClassificationCounts classification;
};

/// The framework. One per renderer. Not thread-safe: it is stepped once per frame on the frame
/// thread, before any pass that reads history.
class TemporalFramework {
public:
    explicit TemporalFramework(Allocator& allocator) noexcept;

    [[nodiscard]] Status initialize(const TemporalConfig& config) noexcept;

    /// Register a consumer. `needs_jitter` is what makes the projection unjittered when nobody
    /// needs it; `name` is for the diagnostic and must outlive the framework.
    [[nodiscard]] Expected<ConsumerId, Error> register_consumer(const char* name,
                                                                bool needs_jitter) noexcept;

    /// Declare a history resource. The framework allocates, resizes, invalidates and releases it.
    [[nodiscard]] Expected<HistoryId, Error> declare_history(
        ConsumerId consumer, const HistoryDeclaration& declaration) noexcept;

    /// One frame. Detects invalidation, advances the jitter once, and resizes every history.
    void begin_frame(const TemporalView& view) noexcept;

    /// An explicit cut from gameplay, a cinematic, or a scene load.
    void signal_cut(TemporalInvalidation cause) noexcept;

    /// The frame's jitter. Zero when no registered consumer needs it.
    [[nodiscard]] const JitterSequence& jitter() const noexcept { return jitter_; }

    /// Pin the sequence for a golden-image test or a capture.
    void pin_jitter(u32 index) noexcept;
    void unpin_jitter() noexcept;

    /// The jittered projection. The ONE place jitter is applied — a pass that jittered again would
    /// be applying it twice, which is exactly what the requirement's scenario forbids.
    [[nodiscard]] Mat4 jittered_view_projection() const noexcept;

    [[nodiscard]] const TemporalView& view() const noexcept { return view_; }

    [[nodiscard]] const TemporalView& previous_view() const noexcept { return previous_view_; }

    /// True on a frame in which history was invalidated. Every consumer sees the same answer on the
    /// same frame, which is the whole of "none SHALL blend across the cut".
    [[nodiscard]] bool invalidated_this_frame() const noexcept { return invalidated_this_frame_; }

    [[nodiscard]] const HistoryResource* history(HistoryId id) const noexcept;

    /// Record that a consumer wrote its history this frame, which makes it valid to read next
    /// frame and stamps it with this frame's provenance.
    void mark_history_written(HistoryId id) noexcept;

    /// Motion for one surface point, using this frame's and last frame's unjittered matrices. The
    /// framework supplies the matrices so a consumer cannot pair the wrong two.
    [[nodiscard]] SurfaceMotion surface_motion(Vec3 current_world,
                                               Vec3 previous_world) const noexcept;

    /// Classify one pixel's history. Every consumer calls this and none derives its own, which is
    /// what makes two effects agree about whether a pixel's history is valid.
    [[nodiscard]] ReprojectionResult classify(HistoryId id, Vec2 current_uv,
                                              const SurfaceMotion& motion, f32 current_depth,
                                              f32 history_depth) noexcept;

    [[nodiscard]] const TemporalStatistics& statistics() const noexcept { return stats_; }

    /// Bytes of history declared by one consumer. The per-consumer half of the diagnostic.
    [[nodiscard]] u64 history_bytes(ConsumerId consumer) const noexcept;

    [[nodiscard]] u64 frame() const noexcept { return frame_; }

private:
    struct Consumer {
        const char* name = "";
        bool needs_jitter = false;
    };

    void invalidate_all(TemporalInvalidation cause) noexcept;
    void resize_histories() noexcept;

    Array<Consumer> consumers_;
    Array<HistoryResource> histories_;
    JitterSequence jitter_;
    TemporalConfig config_;
    TemporalView view_;
    TemporalView previous_view_;
    TemporalStatistics stats_;
    TemporalInvalidation pending_ = TemporalInvalidation::None;
    u64 frame_ = 0;
    u64 projection_key_ = 0;
    bool invalidated_this_frame_ = false;
    bool has_previous_ = false;
};

}  // namespace cy::rendering
