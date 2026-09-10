#ifndef CY_AUDIO_ACOUSTICS_H
#define CY_AUDIO_ACOUSTICS_H
// The acoustics backend, its geometry, and the asynchronous simulation. M8.b tasks 10.1 and 10.2.
//
// `audio` names Steam Audio as an `AcousticsBackend` and then constrains it three ways, and the
// constraints are what this file is shaped by:
//
//   "Steam Audio SHALL be optional and capability-gated behind the `CY_AUDIO_STEAM_AUDIO` build
//    option and a runtime capability query. When absent or disabled, the engine SHALL fall back to
//    its own panning, distance attenuation, filter-based occlusion, and reverb sends."
//
//   "Content SHALL NOT depend on Steam Audio being present: it SHALL improve audio quality, never
//    enable or gate gameplay."
//
//   "Audio SHALL consume geometry through an extraction interface and SHALL NOT call into the
//    physics server directly, so the two subsystems remain decoupled."
//
// So: `AcousticsBackend` is the ENGINE'S interface, `FallbackAcoustics` implements it with the
// panning and filtering `cy::audio`'s spatial.h already has, and a Steam Audio implementation is a
// second implementation behind the option. No Steam Audio type appears in this header or in any
// header above it — which is `thirdparty-dependencies`' isolation rule, and the reason a build with
// the option off contains none of its code.
//
// --- THE CALLBACK NEVER WAITS --------------------------------------------------------------------
//
// "Simulation results ... SHALL be published into a double-buffered store. The realtime callback
// SHALL read the most recently completed result and SHALL NOT wait on simulation." `ResultStore` is
// that double buffer: a simulation writes the back buffer and publishes it with one atomic store,
// and `read()` takes whatever is current. There is no lock on the read path, because a lock on the
// read path is a lock the audio callback can block on.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/math/vec.h>
#include <cy/core/memory/array.h>
#include <cy/core/values/name.h>

#include <atomic>

namespace cy::audio {

/// A surface's acoustic properties. `audio`: "An `AcousticMaterial` SHALL define at least:
/// absorption, transmission, and scattering coefficients."
struct AcousticMaterial {
    Name name;
    /// [0, 1]: how much energy the surface absorbs rather than reflecting.
    f32 absorption = 0.1F;
    /// [0, 1]: how much passes THROUGH. A curtain transmits; a concrete wall does not.
    f32 transmission = 0.05F;
    /// [0, 1]: how much of the reflection is diffuse rather than specular.
    f32 scattering = 0.5F;
};

/// One triangle of acoustic geometry, in world space.
struct AcousticTriangle {
    Vec3 a;
    Vec3 b;
    Vec3 c;
    /// An index into the extraction's material table.
    u16 material = 0;
};

/// A source of acoustic geometry. THE DECOUPLING: audio asks this interface, and the host answers
/// from the same collision geometry physics uses. Nothing in this module names a physics server.
class GeometryExtractor {
public:
    GeometryExtractor() = default;
    virtual ~GeometryExtractor() = default;
    GeometryExtractor(const GeometryExtractor&) = delete;
    GeometryExtractor& operator=(const GeometryExtractor&) = delete;
    GeometryExtractor(GeometryExtractor&&) = delete;
    GeometryExtractor& operator=(GeometryExtractor&&) = delete;

    /// Static geometry within `radius` of `centre`, appended to `out`. Called when the cache is
    /// cold or the listener has moved far enough to need more.
    virtual Status extract_static(Vec3 centre, f32 radius,
                                  Array<AcousticTriangle>& out) noexcept = 0;
    /// Geometry that has MOVED since `since_tick`, appended to `out`. "dynamic geometry SHALL be
    /// updated only when it moves."
    virtual Status extract_dynamic(Vec3 centre, f32 radius, u64 since_tick,
                                   Array<AcousticTriangle>& out) noexcept = 0;
};

struct ExtractionReport {
    u32 static_triangles = 0;
    u32 dynamic_triangles = 0;
    /// True when the static cache answered without asking the host — the common frame.
    bool cache_hit = false;
    u32 static_extractions = 0;
    u32 dynamic_extractions = 0;
};

/// The bounded, incremental extractor. "Extraction SHALL be incremental and bounded: only geometry
/// within a configurable radius of active listeners SHALL be extracted, static geometry SHALL be
/// cached, and dynamic geometry SHALL be updated only when it moves."
class GeometryCache {
public:
    explicit GeometryCache(Allocator& allocator) noexcept;

    GeometryCache(const GeometryCache&) = delete;
    GeometryCache& operator=(const GeometryCache&) = delete;

    /// Metres. Geometry beyond this from the listener is not extracted at all.
    f32 radius = 60.0F;
    /// How far the listener may move before the static cache is refilled. Refilling on every metre
    /// would make the cache pointless; never refilling would make it wrong.
    f32 refill_distance = 15.0F;

    [[nodiscard]] Status update(GeometryExtractor& extractor, Vec3 listener, u64 tick,
                                ExtractionReport& report) noexcept;
    [[nodiscard]] Span<const AcousticTriangle> triangles() const noexcept {
        return combined_.span();
    }
    void invalidate() noexcept;

private:
    Array<AcousticTriangle> static_;
    Array<AcousticTriangle> dynamic_;
    Array<AcousticTriangle> combined_;
    Vec3 cached_at_;
    u64 last_tick_ = 0;
    bool primed_ = false;
};

// --- The backend ------------------------------------------------------------------------------

/// What a backend can do. Queried at runtime, never inferred from which one is linked — the same
/// rule `text-and-fonts` states and for the same reason.
struct AcousticsCapabilities {
    bool hrtf = false;
    bool ambisonics = false;
    bool geometry_occlusion = false;
    bool transmission = false;
    bool reflections = false;
    bool reverb = false;
    bool propagation = false;
    /// "Hardware acceleration ... SHALL be capability-gated with a CPU path always available."
    bool hardware_accelerated = false;
};

/// What a simulation was asked about one source.
struct AcousticQuery {
    u64 source = 0;
    Vec3 source_position;
    Vec3 listener_position;
    Vec3 listener_forward{0.0F, 0.0F, -1.0F};
    /// Higher is simulated first when the budget runs out.
    f32 importance = 0.0F;
};

/// What it answered. These are the parameters the realtime path applies.
struct AcousticResult {
    u64 source = 0;
    /// The direction the sound arrives from, in the listener's space. Not the straight line to the
    /// source: a sound arriving through a door comes from the door.
    Vec3 direction{0.0F, 0.0F, -1.0F};
    /// [0, 1]. How much of the direct path is blocked.
    f32 occlusion = 0.0F;
    /// [0, 1]. How much passes through the blocking surface, from its material.
    f32 transmission = 0.0F;
    /// Linear gain for the reflected energy, and the time it arrives over.
    f32 reflection_gain = 0.0F;
    f32 reverb_send = 0.0F;
    f32 reverb_time = 1.0F;
};

/// The engine's acoustics interface. Steam Audio implements it; so does the fallback.
class AcousticsBackend {
public:
    AcousticsBackend() = default;
    virtual ~AcousticsBackend() = default;
    AcousticsBackend(const AcousticsBackend&) = delete;
    AcousticsBackend& operator=(const AcousticsBackend&) = delete;
    AcousticsBackend(AcousticsBackend&&) = delete;
    AcousticsBackend& operator=(AcousticsBackend&&) = delete;

    [[nodiscard]] virtual const char* backend_name() const noexcept = 0;
    [[nodiscard]] virtual AcousticsCapabilities capabilities() const noexcept = 0;
    /// Hand the backend the geometry it should simulate against. The fallback ignores it, which is
    /// exactly the difference between the two.
    virtual void set_geometry(Span<const AcousticTriangle> triangles,
                              Span<const AcousticMaterial> materials) noexcept = 0;
    /// Simulate a batch. Called on a job, never on the audio callback.
    [[nodiscard]] virtual Status simulate(Span<const AcousticQuery> queries,
                                          Span<AcousticResult> results) noexcept = 0;
};

/// The engine's own path: panning, distance attenuation, filter-based occlusion, reverb sends.
///
/// "WHEN the engine is built without Steam Audio THEN all audio SHALL still play, spatialised by
/// the fallback path, with no missing sounds and no gameplay difference." This class is that
/// sentence: it answers every query, it never fails, and its results are the ones
/// `cy::audio::occlusion_filter` and `pan_stereo` already know how to apply.
class FallbackAcoustics final : public AcousticsBackend {
public:
    [[nodiscard]] const char* backend_name() const noexcept override { return "fallback"; }
    [[nodiscard]] AcousticsCapabilities capabilities() const noexcept override;
    void set_geometry(Span<const AcousticTriangle> triangles,
                      Span<const AcousticMaterial> materials) noexcept override;
    [[nodiscard]] Status simulate(Span<const AcousticQuery> queries,
                                  Span<AcousticResult> results) noexcept override;

    /// A distance-based reverb send, since the fallback has no room to measure. A number rather
    /// than a guess dressed up as a simulation.
    f32 reverb_distance = 25.0F;
};

/// Whether a Steam Audio backend is in this build at all. Answers the BUILD question; the runtime
/// question is `AcousticsBackend::capabilities()`.
[[nodiscard]] bool steam_audio_compiled_in() noexcept;

/// Create the Steam Audio backend, or refuse when it is not in this build.
///
/// The refusal is `ErrorCode::Unavailable` and it is not an error a caller has to handle specially:
/// the caller uses `FallbackAcoustics` instead, which is what "content SHALL NOT depend on Steam
/// Audio being present" means in code.
[[nodiscard]] Expected<AcousticsBackend*, Error> create_steam_audio(Allocator& allocator) noexcept;
void destroy_steam_audio(AcousticsBackend* backend, Allocator& allocator) noexcept;

// --- The asynchronous simulation
// ------------------------------------------------------------------

struct SimulationBudget {
    /// How many sources one update may simulate. The rest are deferred to the next, by importance.
    u32 sources_per_update = 32;
    /// Seconds. How long an applied parameter takes to reach a newly published one, so "a change in
    /// simulated acoustics is not audible as a step".
    f32 interpolation_seconds = 0.15F;
};

struct SimulationReport {
    u32 requested = 0;
    u32 simulated = 0;
    /// Sources that did not fit the budget and will be simulated next update. "the highest-
    /// importance sources SHALL be simulated and the remainder deferred, with the deferral
    /// reported."
    u32 deferred = 0;
    u32 published = 0;
};

/// A double-buffered store of results.
///
/// The write side fills the back buffer and publishes; the read side takes whichever buffer was
/// published last. One atomic index, no lock, and the callback never waits.
class ResultStore {
public:
    explicit ResultStore(Allocator& allocator) noexcept;

    ResultStore(const ResultStore&) = delete;
    ResultStore& operator=(const ResultStore&) = delete;

    /// The write side: fill this, then `publish()`.
    [[nodiscard]] Array<AcousticResult>& back() noexcept;
    void publish() noexcept;
    /// The read side. Safe to call from the audio callback: it reads one atomic and returns a span
    /// into a buffer the writer is not touching.
    [[nodiscard]] Span<const AcousticResult> read() const noexcept;
    [[nodiscard]] u64 generation() const noexcept {
        return generation_.load(std::memory_order_acquire);
    }

private:
    Array<AcousticResult> buffers_[2];  // NOLINT(modernize-use-default-member-init): an Array has
                                        // no default constructor; both are built from the
                                        // allocator in the constructor's initialiser list.
    std::atomic<u32> front_{0};
    std::atomic<u64> generation_{0};
};

/// The applied parameters for one source, interpolated toward what the simulation published.
struct AppliedAcoustics {
    AcousticResult applied;
    bool primed = false;

    /// Move toward `target` over `seconds`, frame-rate independently. The same half-life form the
    /// camera's smoothing uses, and for the same reason.
    void advance(const AcousticResult& target, f32 dt, f32 seconds) noexcept;
};

/// Run one simulation update: choose what fits the budget, simulate it, publish it.
[[nodiscard]] Status simulate_update(AcousticsBackend& backend, Span<AcousticQuery> queries,
                                     const SimulationBudget& budget, ResultStore& store,
                                     SimulationReport& report) noexcept;

}  // namespace cy::audio

#endif  // CY_AUDIO_ACOUSTICS_H
