// The geometry cache, the fallback backend, and the double-buffered simulation.
// M8.b tasks 10.1 and 10.2.

#include <cy/audio/acoustics.h>

#include <algorithm>
#include <cmath>

#if defined(CY_AUDIO_STEAM_AUDIO)
// The ONE translation unit that may name Steam Audio's types, and it does so inside this guard.
// `thirdparty-dependencies`' isolation rule: no third-party type appears above the backend, so a
// build with the option off contains none of its code and no header above this one changes.
#    include <phonon.h>
#endif

namespace cy::audio {
namespace {

[[nodiscard]] f32 clampf(f32 value, f32 low, f32 high) noexcept {
    return std::clamp(value, low, high);
}

}  // namespace

GeometryCache::GeometryCache(Allocator& allocator) noexcept
    : static_(allocator), dynamic_(allocator), combined_(allocator) {}

void GeometryCache::invalidate() noexcept {
    primed_ = false;
    static_.clear();
    dynamic_.clear();
    combined_.clear();
}

Status GeometryCache::update(GeometryExtractor& extractor, Vec3 listener, u64 tick,
                             ExtractionReport& report) noexcept {
    report = ExtractionReport{};

    // THE STATIC CACHE. Refilled only when the listener has moved far enough that the radius no
    // longer covers what it covered before — "static geometry SHALL be cached", and a cache
    // refilled every frame is not one.
    const bool moved = !primed_ || (length(listener - cached_at_) > refill_distance);
    if (moved) {
        static_.clear();
        if (Status extracted = extractor.extract_static(listener, radius, static_); !extracted) {
            return extracted;
        }
        cached_at_ = listener;
        primed_ = true;
        ++report.static_extractions;
    } else {
        report.cache_hit = true;
    }
    report.static_triangles = static_cast<u32>(static_.size());

    // DYNAMIC GEOMETRY, only what has moved since the last update.
    dynamic_.clear();
    if (Status extracted = extractor.extract_dynamic(listener, radius, last_tick_, dynamic_);
        !extracted) {
        return extracted;
    }
    ++report.dynamic_extractions;
    report.dynamic_triangles = static_cast<u32>(dynamic_.size());
    last_tick_ = tick;

    combined_.clear();
    for (const AcousticTriangle& triangle : static_.span()) {
        if (Status pushed = combined_.push_back(triangle); !pushed) {
            return pushed;
        }
    }
    for (const AcousticTriangle& triangle : dynamic_.span()) {
        if (Status pushed = combined_.push_back(triangle); !pushed) {
            return pushed;
        }
    }
    return ok();
}

AcousticsCapabilities FallbackAcoustics::capabilities() const noexcept {
    AcousticsCapabilities capabilities;
    // EVERY ONE OF THESE IS FALSE ON PURPOSE. The fallback pans, attenuates, filters and sends to a
    // reverb bus; it does not trace geometry, and saying it did would be the "capability query"
    // requirement defeated by its own implementation.
    capabilities.reverb = true;  // a send, which is a reverb the mix does rather than a simulation
    return capabilities;
}

void FallbackAcoustics::set_geometry(Span<const AcousticTriangle> triangles,
                                     Span<const AcousticMaterial> materials) noexcept {
    // Deliberately ignored: the fallback has no geometry-aware path, and pretending to keep the
    // triangles would suggest otherwise to anybody reading a profile.
    (void)triangles;
    (void)materials;
}

Status FallbackAcoustics::simulate(Span<const AcousticQuery> queries,
                                   Span<AcousticResult> results) noexcept {
    if (results.size() < queries.size()) {
        return make_unexpected(
            Error{ErrorCode::BufferTooSmall, "one result per query is required", 0});
    }
    for (usize index = 0; index < queries.size(); ++index) {
        const AcousticQuery& query = queries[index];
        AcousticResult& result = results[index];
        result = AcousticResult{};
        result.source = query.source;
        const Vec3 to_source = query.source_position - query.listener_position;
        result.direction = normalized_or(to_source, Vec3{0.0F, 0.0F, -1.0F});
        // NO GEOMETRY, so no occlusion is claimed: a caller that wants filter-based occlusion
        // supplies its own estimate, which is what `cy::audio::occlusion_filter` takes.
        result.occlusion = 0.0F;
        result.transmission = 0.0F;
        // A distance-based send, and it is honest about being one: nearer sources sound drier.
        const f32 distance = length(to_source);
        result.reverb_send =
            clampf(distance / ((reverb_distance > 0.0F) ? reverb_distance : 1.0F), 0.0F, 1.0F);
        result.reverb_time = 1.0F;
    }
    return ok();
}

bool steam_audio_compiled_in() noexcept {
#if defined(CY_AUDIO_STEAM_AUDIO)
    return true;
#else
    return false;
#endif
}

#if defined(CY_AUDIO_STEAM_AUDIO)

namespace {

/// The Steam Audio backend. Every Steam Audio type is inside this class and this translation unit.
class SteamAudioBackend final : public AcousticsBackend {
public:
    [[nodiscard]] const char* backend_name() const noexcept override { return "steam-audio"; }

    [[nodiscard]] AcousticsCapabilities capabilities() const noexcept override {
        AcousticsCapabilities capabilities;
        capabilities.hrtf = true;
        capabilities.ambisonics = true;
        capabilities.geometry_occlusion = true;
        capabilities.transmission = true;
        capabilities.reflections = true;
        capabilities.reverb = true;
        capabilities.propagation = true;
        // Steam Audio's own acceleration is queried from the context at initialisation; until this
        // backend is exercised on a machine with the dependency present, it reports the CPU path,
        // which is the one that is always available.
        capabilities.hardware_accelerated = false;
        return capabilities;
    }

    void set_geometry(Span<const AcousticTriangle> triangles,
                      Span<const AcousticMaterial> materials) noexcept override {
        triangles_ = triangles;
        materials_ = materials;
    }

    [[nodiscard]] Status simulate(Span<const AcousticQuery> queries,
                                  Span<AcousticResult> results) noexcept override {
        // The scene, the simulator and the per-source simulation are Steam Audio's API; this body
        // is where `iplSimulatorRunDirect` and `iplSimulatorRunReflections` are called and their
        // outputs are copied into `AcousticResult`.
        //
        // IT IS NOT WRITTEN, AND THIS RETURN SAYS SO RATHER THAN RETURNING SILENCE. The dependency
        // is declared in deps/manifest.toml and gated by CY_AUDIO_STEAM_AUDIO; it has not been
        // fetched or built on this machine, so the calls below cannot be compiled, let alone
        // checked. A caller receives `NotImplemented` and falls back — which is exactly what the
        // requirement's "content SHALL NOT depend on Steam Audio being present" asks of it.
        (void)queries;
        (void)results;
        return make_unexpected(Error{ErrorCode::NotImplemented,
                                     "the Steam Audio backend is declared and not yet implemented; "
                                     "use FallbackAcoustics",
                                     0});
    }

private:
    Span<const AcousticTriangle> triangles_;
    Span<const AcousticMaterial> materials_;
};

}  // namespace

#endif

Expected<AcousticsBackend*, Error> create_steam_audio(Allocator& allocator) noexcept {
#if defined(CY_AUDIO_STEAM_AUDIO)
    void* memory = allocator.allocate(sizeof(SteamAudioBackend), alignof(SteamAudioBackend));
    if (memory == nullptr) {
        return make_unexpected(
            Error{ErrorCode::OutOfMemory, "no room for the acoustics backend", 0});
    }
    return new (memory) SteamAudioBackend();
#else
    (void)allocator;
    // NOT AN ERROR A CALLER HAS TO HANDLE SPECIALLY. It uses the fallback, and the game sounds
    // slightly worse and behaves identically.
    return make_unexpected(Error{ErrorCode::Unavailable,
                                 "this build has no Steam Audio: configure with "
                                 "-DCY_AUDIO_STEAM_AUDIO=ON",
                                 0});
#endif
}

void destroy_steam_audio(AcousticsBackend* backend, Allocator& allocator) noexcept {
#if defined(CY_AUDIO_STEAM_AUDIO)
    if (backend == nullptr) {
        return;
    }
    backend->~AcousticsBackend();
    allocator.deallocate(backend, sizeof(SteamAudioBackend), alignof(SteamAudioBackend));
#else
    (void)backend;
    (void)allocator;
#endif
}

ResultStore::ResultStore(Allocator& allocator) noexcept
    : buffers_{Array<AcousticResult>(allocator), Array<AcousticResult>(allocator)} {}

Array<AcousticResult>& ResultStore::back() noexcept {
    return buffers_[1U - front_.load(std::memory_order_relaxed)];
}

void ResultStore::publish() noexcept {
    // ONE ATOMIC STORE, and it is a release: everything written into the back buffer happens before
    // the reader can see the index change.
    const u32 next = 1U - front_.load(std::memory_order_relaxed);
    generation_.fetch_add(1, std::memory_order_relaxed);
    front_.store(next, std::memory_order_release);
}

Span<const AcousticResult> ResultStore::read() const noexcept {
    // AND ONE ATOMIC LOAD ON THE READ PATH. No lock, so the audio callback cannot block on the
    // simulation — "The realtime callback SHALL read the most recently completed result and SHALL
    // NOT wait on simulation."
    return buffers_[front_.load(std::memory_order_acquire)].span();
}

void AppliedAcoustics::advance(const AcousticResult& target, f32 dt, f32 seconds) noexcept {
    if (!primed || seconds <= 0.0F || dt <= 0.0F) {
        applied = target;
        primed = true;
        return;
    }
    // A HALF-LIFE, so the same interpolation happens over the same wall-clock time whatever the
    // update rate is — the same form `camera-system` requires of camera smoothing, and for the same
    // reason: a per-update factor is a different sound at a different frame rate.
    const f32 alpha = 1.0F - std::pow(0.5F, dt / seconds);
    applied.source = target.source;
    applied.direction = normalized_or(
        applied.direction + ((target.direction - applied.direction) * alpha), target.direction);
    applied.occlusion += (target.occlusion - applied.occlusion) * alpha;
    applied.transmission += (target.transmission - applied.transmission) * alpha;
    applied.reflection_gain += (target.reflection_gain - applied.reflection_gain) * alpha;
    applied.reverb_send += (target.reverb_send - applied.reverb_send) * alpha;
    applied.reverb_time += (target.reverb_time - applied.reverb_time) * alpha;
}

Status simulate_update(AcousticsBackend& backend, Span<AcousticQuery> queries,
                       const SimulationBudget& budget, ResultStore& store,
                       SimulationReport& report) noexcept {
    report = SimulationReport{};
    report.requested = static_cast<u32>(queries.size());
    if (queries.empty()) {
        return ok();
    }

    // BY IMPORTANCE. "the highest-importance sources SHALL be simulated and the remainder deferred,
    // with the deferral reported."
    std::ranges::stable_sort(queries, [](const AcousticQuery& a, const AcousticQuery& b) noexcept {
        return a.importance > b.importance;
    });

    // A budget of zero means "no limit", which is what a project with headroom configures.
    const usize allowed = (budget.sources_per_update == 0)
                              ? queries.size()
                              : std::min<usize>(queries.size(), budget.sources_per_update);
    report.simulated = static_cast<u32>(allowed);
    report.deferred = static_cast<u32>(queries.size() - allowed);

    Array<AcousticResult>& back = store.back();
    back.clear();
    if (Status sized = back.resize(allowed); !sized) {
        return sized;
    }
    if (Status simulated =
            backend.simulate(Span<const AcousticQuery>(queries.data(), allowed), back.span());
        !simulated) {
        return simulated;
    }
    store.publish();
    report.published = static_cast<u32>(allowed);
    return ok();
}

}  // namespace cy::audio
