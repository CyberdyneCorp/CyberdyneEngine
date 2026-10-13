// The radiance cache's gather seam: `ProbeGatherBackend`, `RadianceCache::select`, `apply` and the
// probe revision. Issue #35, stage 3 — the headless half of what `render.gi_gpu_tracing` holds the
// device to.
//
// A backend is handed the selection the host scheduler made and nothing else, and gives back what
// it gathered through `RadianceCache::apply`. So the scheduler's guarantees — the budget, the
// priority order, and above all PROGRESS — hold through a backend only if `select` is the function
// `update` uses and `apply` lands exactly what it is given. These cases check both with a backend
// that records, and no device.

#include <cy/test/test.h>

#include <cy/rendering/gi/radiance_cache.h>

#include <algorithm>
#include <vector>

namespace {

using cy::f32;
using cy::u32;
using cy::u64;
using cy::Vec3;
using namespace cy::rendering::gi;  // NOLINT(google-build-using-namespace)

/// A backend that remembers what it was given and answers each probe with a payload of its own
/// handle, so a test can see which probe received which answer.
struct Recording final : ProbeGatherBackend {
    std::vector<u32> submitted;
    std::vector<u32> revisions;
    u64 frame = 0;
    u32 stride = 0;
    std::vector<f32> payload;

    u32 submit(const RadianceCache& cache, cy::Span<const u32> selected,
               const ProbeUpdateContext& context) noexcept override {
        submitted.assign(selected.begin(), selected.end());
        revisions.clear();
        for (const u32 probe : selected) {
            revisions.push_back(cache.probe(probe).revision);
        }
        frame = context.frame;
        stride = encoding_traits(cache.settings().encoding).floats_per_probe;
        return static_cast<u32>(selected.size());
    }

    u32 retire(RadianceCache& cache) noexcept override {
        u32 written = 0;
        for (cy::usize index = 0; index < submitted.size(); ++index) {
            payload.assign(stride, static_cast<f32>(submitted[index]));
            ProbeGatherResult result;
            result.probe = submitted[index];
            result.revision = revisions[index];
            result.payload = {payload.data(), payload.size()};
            for (f32& distance : result.axis_distance) {
                distance = 3.0F;
            }
            result.error = 0.125F;
            result.frame = frame;
            written += cache.apply(result) ? 1U : 0U;
        }
        submitted.clear();
        return written;
    }
};

void configure(RadianceCache& cache) {
    ProbeCacheSettings settings;
    settings.levels = 1;
    settings.base_spacing_metres = 2.0F;
    settings.half_extent_probes = 3;
    CY_REQUIRE(cache.configure(settings).has_value());
    ProbePlacementContext placement;
    (void)cache.scroll_to(Vec3{0.0F, 0.0F, 0.0F}, placement);
}

}  // namespace

CY_TEST_CASE("a gather backend is handed exactly the selection the host update gathers") {
    RadianceCache host;
    RadianceCache device;
    configure(host);
    configure(device);
    Recording backend;
    device.set_gather_backend(&backend);

    ProbeUpdateContext update;
    update.budget = 8;
    update.rays_per_probe = 4;
    update.max_age_frames = 6;
    const auto selection = [&update](const RadianceCache& cache) {
        cy::Array<u32> chosen;
        ProbeUpdateReport scratch;
        CY_REQUIRE(RadianceCache::select(cache.probes(), update, chosen, scratch).has_value());
        return std::vector<u32>(chosen.begin(), chosen.end());
    };
    for (u64 frame = 1; frame <= 12; ++frame) {
        update.frame = frame;

        // The host: the probes `update` gathered are the ones `select` names. (The two caches'
        // errors differ — the backend answers its own — so each is held to its own selection.)
        std::vector<u32> expected = selection(host);
        const ProbeUpdateReport host_report = host.update(update);
        std::vector<u32> gathered;
        for (u32 handle = 0; handle < host.probes().size(); ++handle) {
            if (host.probe(handle).live && host.probe(handle).last_update_frame == frame) {
                gathered.push_back(handle);
            }
        }
        std::ranges::sort(expected);
        CY_CHECK(gathered == expected);

        // The backend: handed `select`'s answer, in its order, once last frame's results landed.
        (void)device.collect();
        const std::vector<u32> handed = selection(device);
        const ProbeUpdateReport device_report = device.update(update);
        CY_CHECK(backend.submitted == handed);
        CY_CHECK_EQ(device_report.probes_updated, host_report.probes_updated);
        CY_CHECK_EQ(device_report.queue_depth, host_report.queue_depth);
        CY_CHECK_EQ(device_report.rays, host_report.rays);
        CY_CHECK_LE(backend.submitted.size(), static_cast<cy::usize>(update.budget));
    }
}

CY_TEST_CASE("a backend's results land on the next update, and every probe is reached") {
    RadianceCache cache;
    configure(cache);
    Recording backend;
    cache.set_gather_backend(&backend);
    const u32 probes = cache.probe_count();

    ProbeUpdateContext update;
    update.budget = 8;
    update.rays_per_probe = 4;
    update.frame = 1;
    (void)cache.update(update);
    const std::vector<u32> first = backend.submitted;
    CY_REQUIRE_EQ(first.size(), 8U);
    // Nothing lands until the backend retires: the frame that ran it has not completed.
    CY_CHECK_FALSE(cache.probe(first[0]).valid);
    CY_CHECK_EQ(cache.collect(), 8U);
    const Probe& landed = cache.probe(first[0]);
    CY_CHECK(landed.valid);
    CY_CHECK_EQ(landed.last_update_frame, 1U);
    CY_CHECK_EQ(landed.error, 0.125F);
    CY_CHECK_EQ(landed.axis_distance[4], 3.0F);
    CY_CHECK_EQ(cache.probe_payload(first[0])[0], static_cast<f32>(first[0]));

    // THE PROGRESS GUARANTEE, THROUGH A BACKEND. The same bound the host case states: the cache
    // size over the budget, plus the slack the age cap needs to start forcing the oldest forward.
    const u64 needed = (probes + update.budget - 1U) / update.budget;
    update.max_age_frames = needed + 4U;
    for (u64 frame = 2; frame < needed + 8U; ++frame) {
        update.frame = frame;
        (void)cache.update(update);
    }
    (void)cache.collect();
    CY_CHECK_EQ(cache.diagnostics().valid_probes, probes);
}

CY_TEST_CASE("a result for a probe invalidated while it was in flight is dropped") {
    RadianceCache cache;
    configure(cache);
    Recording backend;
    cache.set_gather_backend(&backend);

    ProbeUpdateContext update;
    update.budget = 4;
    update.frame = 1;
    (void)cache.update(update);
    const u32 probe = backend.submitted[0];
    const u32 before = cache.probe(probe).revision;
    // The world changed under the probe between the submit and the retire.
    const Vec3 at = cache.probe(probe).position;
    CY_CHECK_EQ(cache.invalidate(cy::Aabb::from_center_extents(at, Vec3{0.1F, 0.1F, 0.1F})), 1U);
    CY_CHECK_NE(cache.probe(probe).revision, before);
    // Three of the four land; the invalidated one does not, so the invalidation is not undone.
    CY_CHECK_EQ(cache.collect(), 3U);
    CY_CHECK_FALSE(cache.probe(probe).valid);
    CY_CHECK_EQ(cache.probe(probe).error, 1.0F);
}

CY_TEST_CASE("a probe's revision moves with every change the host makes, and only those") {
    RadianceCache cache;
    configure(cache);
    const u32 probe = 0;
    const u32 created = cache.probe(probe).revision;
    CY_CHECK_GT(created, 0U);

    // A host gather moves nothing: a device mirror that uploaded the probe still has it.
    ProbeUpdateContext update;
    update.converged = true;
    update.frame = 1;
    (void)cache.update(update);
    CY_CHECK_EQ(cache.probe(probe).revision, created);

    // A seed rewrites the payload, which a mirror must re-upload.
    const Vec3 direction{0.0F, 1.0F, 0.0F};
    const Vec3 radiance{1.0F, 1.0F, 1.0F};
    CY_REQUIRE(cache.seed(probe, {&direction, 1}, {&radiance, 1}).has_value());
    const u32 seeded = cache.probe(probe).revision;
    CY_CHECK_NE(seeded, created);

    // A scroll that retires the probe moves its slot's revision, and the probe created into the
    // reused slot moves it again: a mirror one scroll behind can never mistake the new probe for
    // the old one.
    ProbePlacementContext placement;
    (void)cache.scroll_to(Vec3{40.0F, 0.0F, 0.0F}, placement);
    CY_CHECK_NE(cache.probe(probe).revision, seeded);

    // `apply` refuses a payload of the wrong length rather than writing past the probe.
    std::vector<f32> short_payload(3, 1.0F);
    ProbeGatherResult result;
    result.probe = probe;
    result.revision = cache.probe(probe).revision;
    result.payload = {short_payload.data(), short_payload.size()};
    CY_CHECK_FALSE(cache.apply(result));

    // A configure starts the slots over, revisions included, so it moves the cache's generation —
    // the one signal a mirror keyed on revisions has that its probes are not the same probes.
    const u64 before = cache.generation();
    const ProbeCacheSettings same = cache.settings();
    CY_REQUIRE(cache.configure(same).has_value());
    CY_CHECK_GT(cache.generation(), before);
}
