// `VirtualTextureSystem` end to end: the mip tail guarantee, feedback becoming residency requests,
// the schedule becoming tiles and production, prefetch, and invalidation. M6 tasks 5.1 to 5.4.
//
// The case this file exists for is "a frame is never missing, only coarser". It is asserted over
// the WHOLE address space of a texture rather than at a few sampled points, because the failure it
// guards against is a hole at one address and a property checked at three addresses is not a
// property.

#include <cy/servers/render/virtual_texturing/system.h>
#include <cy/test/test.h>

#include <atomic>

using namespace cy::render::vt;
using cy::f32;
using cy::u32;
using cy::u64;
using cy::u8;

namespace {

constexpr u32 kBytesPerTile = 256;

/// Writes the mip level into every byte, so a test can tell which page a tile holds.
class FillProducer final : public PageProducer {
public:
    explicit FillProducer(f32 cost = 1.0F) noexcept : cost_(cost) {}

    [[nodiscard]] const char* producer_name() const noexcept override { return "test-fill"; }
    [[nodiscard]] f32 declared_cost_ms() const noexcept override { return cost_; }
    [[nodiscard]] PersistenceClass persistence() const noexcept override { return persistence_; }

    cy::Status produce(const ProductionRequest& request) noexcept override {
        calls_.fetch_add(1, std::memory_order_relaxed);
        if (fail_.load(std::memory_order_relaxed)) {
            return cy::make_unexpected(
                cy::Error{cy::ErrorCode::Io, "test producer was asked to fail"});
        }
        for (u32 index = 0; index < request.bytes; ++index) {
            request.destination[index] = request.address.mip;
        }
        return cy::ok();
    }

    void set_failing(bool failing) noexcept { fail_.store(failing, std::memory_order_relaxed); }
    void set_persistence(PersistenceClass persistence) noexcept { persistence_ = persistence; }
    [[nodiscard]] u64 calls() const noexcept { return calls_.load(std::memory_order_relaxed); }

private:
    f32 cost_;
    PersistenceClass persistence_ = PersistenceClass::Derived;
    std::atomic<bool> fail_{false};
    std::atomic<u64> calls_{0};
};

[[nodiscard]] VirtualTextureDesc terrain(u32 id = 3) noexcept {
    VirtualTextureDesc desc;
    desc.id = id;
    desc.width = 1024;
    desc.height = 1024;
    desc.tile_size = 128;
    desc.border = 4;
    desc.mip_count = 4;  // 8x8, 4x4, 2x2, 1x1 tiles
    desc.layers = 1;
    desc.mip_tail_levels = 2;  // mips 2 and 3: five tiles
    desc.semantic = TextureSemantic::Colour;
    desc.model = ResidencyModel::VirtualStreamed;
    desc.bytes_per_tile = kBytesPerTile;
    return desc;
}

[[nodiscard]] TileCacheDesc cache_desc(u32 slots) noexcept {
    TileCacheDesc desc;
    desc.format = FormatClass::BlockColour;
    desc.tile_size = 128;
    desc.border = 4;
    desc.bytes_per_tile = kBytesPerTile;
    desc.tile_capacity = slots;
    return desc;
}

[[nodiscard]] cy::residency::SubsystemPolicy residency_policy(u64 budget) noexcept {
    cy::residency::SubsystemPolicy policy;
    policy.domain = cy::MemoryDomain::Gpu;
    policy.budget_bytes = budget;
    policy.budget_kind = cy::BudgetKind::Hard;
    policy.min_residency_frames = 0;
    return policy;
}

[[nodiscard]] VirtualAddress at(u32 texture, u8 mip, u32 x, u32 y) noexcept {
    VirtualAddress address;
    address.texture = texture;
    address.mip = mip;
    address.tile_x = static_cast<cy::u16>(x);
    address.tile_y = static_cast<cy::u16>(y);
    return address;
}

/// Every addressable page of a texture, checked. Returns how many were missing.
[[nodiscard]] u32 count_missing(const VirtualTextureSystem& system,
                                const VirtualTextureDesc& desc) noexcept {
    u32 missing = 0;
    for (u8 mip = 0; mip < desc.mip_count; ++mip) {
        for (u32 y = 0; y < desc.tiles_y(mip); ++y) {
            for (u32 x = 0; x < desc.tiles_x(mip); ++x) {
                missing += system.sample(at(desc.id, mip, x, y)).missing ? 1U : 0U;
            }
        }
    }
    return missing;
}

}  // namespace

CY_TEST_CASE("a texture cannot be registered twice, and an invalid one not at all") {
    VirtualTextureSystem system;
    CY_REQUIRE(system.configure_cache(cache_desc(32)));
    CY_REQUIRE(system.register_texture(terrain()));
    CY_CHECK_FALSE(system.register_texture(terrain()));

    VirtualTextureDesc broken = terrain(4);
    broken.mip_tail_levels = 0;
    CY_CHECK_FALSE(system.register_texture(broken));
    CY_CHECK_EQ(system.texture_count(), 1U);
}

CY_TEST_CASE("the mip tail is pinned, and a cache too small to hold it is a configuration error") {
    VirtualTextureSystem system;
    // The tail of `terrain()` is five tiles: 4 at mip 2 and 1 at mip 3.
    CY_REQUIRE(system.configure_cache(cache_desc(4)));
    CY_REQUIRE(system.register_texture(terrain()));
    // A cache that cannot hold the tail is reported rather than run: the alternative is a system
    // that runs and shows holes.
    CY_CHECK_FALSE(system.make_mip_tail_resident(3));
    CY_CHECK_FALSE(system.mip_tail_resident(3));

    CY_REQUIRE(system.configure_cache(cache_desc(16)));
    CY_REQUIRE(system.make_mip_tail_resident(3));
    CY_CHECK(system.mip_tail_resident(3));
    CY_CHECK_EQ(system.cache(FormatClass::BlockColour).pinned_count(), 5U);
}

CY_TEST_CASE("a frame is never missing, only coarser — over the whole address space") {
    VirtualTextureSystem system;
    CY_REQUIRE(system.configure_cache(cache_desc(16)));
    CY_REQUIRE(system.register_texture(terrain()));

    // Before the tail is resident, the fine levels have nothing to fall back to. That is the state
    // the guarantee exists to make unreachable, and it is asserted so that the case below is not
    // vacuously true.
    CY_CHECK_GT(count_missing(system, terrain()), 0U);

    CY_REQUIRE(system.make_mip_tail_resident(3));

    // THE EXIT CRITERION. Every addressable page of the texture — 64 + 16 + 4 + 1 — resolves to a
    // resident level, with nothing but the pinned tail in the cache.
    CY_CHECK_EQ(count_missing(system, terrain()), 0U);

    // And the substitution is recorded rather than hidden: a mip 0 sample reports the deficit.
    const SampleResult coarse = system.sample(at(3, 0, 5, 5));
    CY_CHECK_FALSE(coarse.missing);
    CY_CHECK(coarse.fallback);
    CY_CHECK_EQ(coarse.desired_mip, 0U);
    CY_CHECK_EQ(coarse.resident_mip, 2U);
    CY_CHECK_EQ(coarse.deficit, 2U);
    CY_CHECK_EQ(coarse.resolved.tile_x, 1U);  // 5 >> 2
    CY_CHECK_EQ(coarse.resolved.tile_y, 1U);

    // A sample of the tail itself is not a fallback.
    const SampleResult exact = system.sample(at(3, 2, 1, 1));
    CY_CHECK_FALSE(exact.fallback);
    CY_CHECK_EQ(exact.deficit, 0U);
}

CY_TEST_CASE("a sample of a texture nobody registered is missing rather than a crash") {
    VirtualTextureSystem system;
    CY_REQUIRE(system.configure_cache(cache_desc(8)));
    CY_CHECK(system.sample(at(99, 0, 0, 0)).missing);
    CY_CHECK(system.description(99) == nullptr);
    CY_CHECK(system.page_table(99) == nullptr);
}

CY_TEST_CASE("feedback becomes one residency request per page, whatever the sample count") {
    VirtualTextureSystem system;
    cy::residency::ResidencyServer server;
    CY_REQUIRE(server.register_subsystem(cy::residency::Subsystem::Texture,
                                         residency_policy(64ULL * kBytesPerTile)));
    CY_REQUIRE(system.configure_cache(cache_desc(32)));
    CY_REQUIRE(system.register_texture(terrain()));
    CY_REQUIRE(system.make_mip_tail_resident(3));
    CY_REQUIRE(system.feedback().configure(4096));

    for (u32 index = 0; index < 2000; ++index) {
        system.feedback().record(at(3, 0, 1, 1).encode());
    }
    for (u32 index = 0; index < 50; ++index) {
        system.feedback().record(at(3, 0, 2, 2).encode());
    }

    CY_REQUIRE(system.submit_feedback_requests(server, 0.0));
    // "A per-pixel request stream SHALL NOT reach the CPU": two pages asked for, two requests.
    CY_CHECK_EQ(system.stats().feedback_requests, 2U);
    CY_CHECK_EQ(server.pending_requests(), 2U);
    CY_CHECK_EQ(server.request_submissions(), 2U);
}

CY_TEST_CASE("the residency schedule becomes tiles, production, and pages the sampler can find") {
    VirtualTextureSystem system;
    FillProducer producer;
    cy::residency::ResidencyServer server;
    CY_REQUIRE(server.register_subsystem(cy::residency::Subsystem::Texture,
                                         residency_policy(64ULL * kBytesPerTile)));
    CY_REQUIRE(system.configure_cache(cache_desc(32)));
    CY_REQUIRE(system.register_texture(terrain()));
    CY_REQUIRE(system.producers().register_producer(3, producer));
    CY_REQUIRE(system.make_mip_tail_resident(3));
    CY_REQUIRE(system.feedback().configure(256));

    system.feedback().record(at(3, 0, 1, 1).encode());
    CY_REQUIRE(system.submit_feedback_requests(server, 0.0));

    cy::residency::Schedule frame;
    CY_REQUIRE(server.schedule(cy::residency::ScheduleOptions{0.0, 0}, frame));
    CY_REQUIRE_EQ(frame.admissions.size(), 1U);
    CY_REQUIRE(system.apply(frame, server, 0.0));

    // With no production workers started the job ran on this thread, which is a supported
    // configuration rather than a mode.
    CY_CHECK_EQ(producer.calls(), 1U);
    CY_CHECK_EQ(system.collect_production(server, 0.0), 1U);
    system.end_frame();

    const SampleResult sampled = system.sample(at(3, 0, 1, 1));
    CY_CHECK_FALSE(sampled.fallback);
    CY_CHECK_EQ(sampled.resident_mip, 0U);
    CY_CHECK(server.is_resident(page_key(at(3, 0, 1, 1).encode())));

    // And the bytes the producer wrote are the ones the tile holds.
    const u8* bytes = system.cache(FormatClass::BlockColour).tile_data(sampled.physical_tile);
    CY_REQUIRE(bytes != nullptr);
    CY_CHECK_EQ(bytes[0], 0U);  // the producer writes the mip level
}

CY_TEST_CASE("a producer that fails releases its tile instead of publishing a page") {
    VirtualTextureSystem system;
    FillProducer producer;
    cy::residency::ResidencyServer server;
    CY_REQUIRE(server.register_subsystem(cy::residency::Subsystem::Texture, residency_policy(0)));
    CY_REQUIRE(system.configure_cache(cache_desc(16)));
    CY_REQUIRE(system.register_texture(terrain()));
    CY_REQUIRE(system.producers().register_producer(3, producer));
    CY_REQUIRE(system.make_mip_tail_resident(3));
    producer.set_failing(true);

    cy::residency::Request request;
    request.key = page_key(at(3, 0, 0, 0).encode());
    request.bytes = kBytesPerTile;
    request.inputs.importance = 1.0F;
    CY_REQUIRE(server.request(request));

    cy::residency::Schedule frame;
    CY_REQUIRE(server.schedule(cy::residency::ScheduleOptions{0.0, 0}, frame));
    CY_REQUIRE(system.apply(frame, server, 0.0));
    CY_CHECK_EQ(system.collect_production(server, 0.0), 0U);
    system.end_frame();

    CY_CHECK_EQ(system.stats().production_failures, 1U);
    CY_CHECK_FALSE(server.is_resident(page_key(at(3, 0, 0, 0).encode())));
    // The tile went back: only the pinned tail is occupied.
    CY_CHECK_EQ(system.cache(FormatClass::BlockColour).occupancy(), 5U);
    // And the surface still renders, from the tail.
    CY_CHECK_FALSE(system.sample(at(3, 0, 0, 0)).missing);
}

CY_TEST_CASE("an admission the cache cannot house is refused, not forced") {
    VirtualTextureSystem system;
    FillProducer producer;
    cy::residency::ResidencyServer server;
    // The residency budget is generous; the CACHE is what runs out, and the two are separate
    // facts on purpose.
    CY_REQUIRE(server.register_subsystem(cy::residency::Subsystem::Texture, residency_policy(0)));
    CY_REQUIRE(system.configure_cache(cache_desc(6)));  // five for the tail, one spare
    CY_REQUIRE(system.register_texture(terrain()));
    CY_REQUIRE(system.producers().register_producer(3, producer));
    CY_REQUIRE(system.make_mip_tail_resident(3));

    for (u32 index = 0; index < 4; ++index) {
        cy::residency::Request request;
        request.key = page_key(at(3, 0, index, 0).encode());
        request.bytes = kBytesPerTile;
        request.inputs.importance = 1.0F;
        CY_REQUIRE(server.request(request));
    }
    cy::residency::Schedule frame;
    CY_REQUIRE(server.schedule(cy::residency::ScheduleOptions{0.0, 0}, frame));
    CY_REQUIRE_EQ(frame.admissions.size(), 4U);
    CY_REQUIRE(system.apply(frame, server, 0.0));

    CY_CHECK_EQ(system.stats().admissions_applied, 1U);
    CY_CHECK_EQ(system.stats().admissions_refused, 3U);
    // Nothing was evicted to make room: the cache does not choose victims.
    CY_CHECK_EQ(system.cache(FormatClass::BlockColour).pinned_count(), 5U);
}

CY_TEST_CASE("prefetch asks for coarse pages and leaves the fine ones to feedback") {
    VirtualTextureSystem system;
    cy::residency::ResidencyServer server;
    CY_REQUIRE(server.register_subsystem(cy::residency::Subsystem::Texture, residency_policy(0)));
    CY_REQUIRE(system.configure_cache(cache_desc(32)));
    CY_REQUIRE(system.register_texture(terrain()));
    CY_REQUIRE(system.make_mip_tail_resident(3));

    CY_REQUIRE(system.prefetch(server, 3, 0, 1.5, cy::residency::PredictionSource::WorldCell, 0.0));

    // "prediction covers latency, feedback establishes accuracy": mips 3, 2 and 1 — one, four and
    // sixteen pages — and not the sixty-four of mip 0, which would be guessing at what will be
    // sampled and would evict the pages that turned out right.
    CY_CHECK_EQ(server.pending_requests(), 21U);
    CY_CHECK_EQ(system.stats().prefetched_pages, 21U);
    CY_CHECK_EQ(server.prediction_accuracy().prefetched, 21U);

    CY_CHECK_FALSE(
        system.prefetch(server, 99, 0, 1.0, cy::residency::PredictionSource::WorldCell, 0.0));
}

CY_TEST_CASE("invalidation releases the affected tiles and never the mip tail") {
    VirtualTextureSystem system;
    FillProducer producer;
    cy::residency::ResidencyServer server;
    CY_REQUIRE(server.register_subsystem(cy::residency::Subsystem::Texture, residency_policy(0)));
    CY_REQUIRE(system.configure_cache(cache_desc(32)));
    CY_REQUIRE(system.register_texture(terrain()));
    CY_REQUIRE(system.producers().register_producer(3, producer));
    CY_REQUIRE(system.make_mip_tail_resident(3));

    for (u32 index = 0; index < 4; ++index) {
        cy::residency::Request request;
        request.key = page_key(at(3, 1, index, 0).encode());
        request.bytes = kBytesPerTile;
        request.inputs.importance = 1.0F;
        CY_REQUIRE(server.request(request));
    }
    cy::residency::Schedule frame;
    CY_REQUIRE(server.schedule(cy::residency::ScheduleOptions{0.0, 0}, frame));
    CY_REQUIRE(system.apply(frame, server, 0.0));
    CY_CHECK_EQ(system.collect_production(server, 0.0), 4U);
    system.end_frame();
    CY_CHECK_EQ(system.cache(FormatClass::BlockColour).occupancy(), 9U);

    // A producer's inputs changed for one mip 1 page.
    CY_REQUIRE(system.invalidate(at(3, 1, 0, 0), 0));
    CY_CHECK_EQ(system.cache(FormatClass::BlockColour).occupancy(), 8U);
    CY_CHECK(system.sample(at(3, 1, 0, 0)).fallback);  // back to the tail
    CY_CHECK_FALSE(system.sample(at(3, 1, 1, 0)).fallback);

    // The tail is never taken away by an invalidation: the guarantee is not a producer's to revoke.
    CY_REQUIRE(system.invalidate(at(3, 3, 0, 0), 1));
    CY_CHECK_EQ(system.cache(FormatClass::BlockColour).pinned_count(), 5U);
    CY_CHECK_FALSE(system.sample(at(3, 0, 0, 0)).missing);
}

CY_TEST_CASE("unregistering a texture takes its tiles with it and leaves the others alone") {
    VirtualTextureSystem system;
    CY_REQUIRE(system.configure_cache(cache_desc(32)));
    CY_REQUIRE(system.register_texture(terrain(3)));
    CY_REQUIRE(system.register_texture(terrain(4)));
    CY_REQUIRE(system.make_mip_tail_resident(3));
    CY_REQUIRE(system.make_mip_tail_resident(4));
    CY_CHECK_EQ(system.cache(FormatClass::BlockColour).occupancy(), 10U);

    CY_CHECK(system.unregister_texture(3));
    CY_CHECK_EQ(system.texture_count(), 1U);
    CY_CHECK_EQ(system.cache(FormatClass::BlockColour).occupancy(), 5U);
    CY_CHECK(system.sample(at(3, 0, 0, 0)).missing);
    CY_CHECK_FALSE(system.sample(at(4, 0, 0, 0)).missing);
    CY_CHECK_FALSE(system.unregister_texture(3));
}

CY_TEST_CASE("a runtime page declares how it persists, and a derived one is never saved") {
    FillProducer producer;
    CY_CHECK(producer.persistence() == PersistenceClass::Derived);
    CY_CHECK_FALSE(is_saved(producer.persistence()));

    // "Terraforming persists": a save-game page is a delta over the cooked base.
    producer.set_persistence(PersistenceClass::SaveGame);
    CY_CHECK(is_saved(producer.persistence()));
    // "Derived pages are not saved": wetness produced from a field is regenerated.
    producer.set_persistence(PersistenceClass::Transient);
    CY_CHECK_FALSE(is_saved(producer.persistence()));
    producer.set_persistence(PersistenceClass::Replicated);
    CY_CHECK_FALSE(is_saved(producer.persistence()));
}

CY_TEST_CASE("reset keeps the caches configured and drops everything in them") {
    VirtualTextureSystem system;
    CY_REQUIRE(system.configure_cache(cache_desc(32)));
    CY_REQUIRE(system.register_texture(terrain()));
    CY_REQUIRE(system.make_mip_tail_resident(3));

    system.reset();
    CY_CHECK_EQ(system.texture_count(), 0U);
    CY_CHECK_EQ(system.cache(FormatClass::BlockColour).occupancy(), 0U);
    CY_CHECK(system.cache(FormatClass::BlockColour).configured());
    CY_CHECK_EQ(system.stats().textures, 0U);

    // And it is usable again straight away.
    CY_REQUIRE(system.register_texture(terrain()));
    CY_REQUIRE(system.make_mip_tail_resident(3));
    CY_CHECK_EQ(count_missing(system, terrain()), 0U);
}
