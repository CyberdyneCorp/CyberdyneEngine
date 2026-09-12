// Storms as spatial phenomena, lightning as an event nobody plays, and a wire form that carries no
// clouds. M10 task 3.2.
//
// HOW THIS SUITE WAS SHOWN TO BE ABLE TO FAIL — reported in `verified_failing`: the substream in
// `StormRegistry::emit_strikes()` was keyed on the STORM'S INDEX in the array rather than on its
// identity and seed. "two peers agree about where the lightning struck" went red the moment a storm
// was despawned on one side: the surviving storms renumbered and their strike sequences swapped.
// The substream key was restored.

#include <cy/test/test.h>

#include <cy/weather/storm.h>

#include <cmath>

#include "fixtures.h"

namespace test = cy::weather::test;
using cy::weather::LightningEvent;
using cy::weather::Storm;
using cy::weather::StormContribution;
using cy::weather::StormId;
using cy::weather::StormRegistry;
using cy::weather::StormType;

CY_TEST_CASE("many storms exist independently and a storm is addressed by identity") {
    StormRegistry storms(test::allocator());
    for (cy::u64 id = 1; id <= 32; ++id) {
        CY_REQUIRE(storms
                       .spawn(test::storm_at(id, static_cast<cy::f64>(id) * 5'000.0, 0.0,
                                             StormType::RainBand, 0.5F))
                       .has_value());
    }
    CY_CHECK_EQ(storms.size(), 32u);
    // A duplicate identity is refused rather than shadowing the incumbent.
    CY_CHECK_FALSE(storms.spawn(test::storm_at(7, 0.0, 0.0, StormType::Squall, 0.5F)).has_value());
    CY_REQUIRE(storms.find(StormId{7}) != nullptr);
    CY_REQUIRE(storms.despawn(StormId{7}).has_value());
    CY_CHECK(storms.find(StormId{7}) == nullptr);
    CY_CHECK_FALSE(storms.despawn(StormId{7}).has_value());
    CY_CHECK_EQ(storms.size(), 31u);
}

CY_TEST_CASE("a storm's contribution falls off and reaches zero at its radius") {
    const Storm storm = test::storm_at(1, 0.0, 0.0, StormType::Thunderstorm, 1.0F);
    const StormContribution centre = cy::weather::storm_contribution_at(storm, 0.0, 0.0);
    const StormContribution halfway = cy::weather::storm_contribution_at(storm, 6'000.0, 0.0);
    const StormContribution edge = cy::weather::storm_contribution_at(storm, 12'001.0, 0.0);

    CY_CHECK_LT(centre.pressure_delta, 0.0F);  // a storm is a LOW
    CY_CHECK_GT(centre.precipitation_mm_per_hour, halfway.precipitation_mm_per_hour);
    CY_CHECK_GT(halfway.precipitation_mm_per_hour, 0.0F);
    CY_CHECK_EQ(edge.influence, 0.0F);
    CY_CHECK_EQ(edge.precipitation_mm_per_hour, 0.0F);

    // A cyclone rotates and a squall blows through. The proportions are the type's character, not
    // a switch inside the caller.
    const Storm cyclone = test::storm_at(2, 0.0, 0.0, StormType::Cyclone, 1.0F);
    const StormContribution rotating = cy::weather::storm_contribution_at(cyclone, 5'000.0, 0.0);
    // At a point due east of the centre, a counter-clockwise rotation blows mostly along -z... and
    // the magnitude of the cross-wind must dominate the radial one.
    CY_CHECK_GT(std::fabs(rotating.wind.y), std::fabs(rotating.wind.x) * 0.5F);

    // A sandstorm carries no rain at all, which is what a per-type table is for.
    const Storm sand = test::storm_at(3, 0.0, 0.0, StormType::SandStorm, 1.0F);
    CY_CHECK_EQ(cy::weather::storm_contribution_at(sand, 1'000.0, 0.0).precipitation_mm_per_hour,
                0.0F);
    CY_CHECK_GT(cy::weather::storm_contribution_at(sand, 1'000.0, 0.0).visibility_loss_metres,
                1'000.0F);
}

CY_TEST_CASE("a storm crosses the map and expires on its own lifetime") {
    StormRegistry storms(test::allocator());
    Storm storm = test::storm_at(1, 0.0, 0.0, StormType::RainBand, 0.6F);
    storm.velocity = cy::Vec2{20.0F, 5.0F};
    storm.lifetime_seconds = 100.0F;
    CY_REQUIRE(storms.spawn(storm).has_value());

    CY_REQUIRE(storms.advance(test::at_tick(1), 10.0F, 1).has_value());
    const Storm* moved = storms.find(StormId{1});
    CY_REQUIRE(moved != nullptr);
    CY_CHECK_NEAR(moved->position.x, 200.0, 0.001);
    CY_CHECK_NEAR(moved->position.z, 50.0, 0.001);

    CY_REQUIRE(storms.advance(test::at_tick(2), 200.0F, 1).has_value());
    CY_CHECK_EQ(storms.size(), 0u);
}

CY_TEST_CASE("lightning is an event with a position, an intensity and a seed, and nothing else") {
    // "Lightning SHALL be published as an EVENT with position, intensity, and a seed, consumed by
    // effects, audio, illumination, and gameplay. Weather SHALL NOT play a sound or spawn an effect
    // directly." And: "Thunder timing SHALL be derived by the audio system from distance, not
    // scheduled by weather" — there is no delay in `LightningEvent` for weather to schedule with.
    StormRegistry storms(test::allocator());
    // Stationary, so that "a bolt falls inside the storm" is measured against a fixed centre rather
    // than against wherever the storm had drifted to by the tick that emitted it.
    cy::weather::Storm parked = test::storm_at(1, 0.0, 0.0, StormType::Thunderstorm, 1.0F);
    parked.velocity = cy::Vec2{0.0F, 0.0F};
    CY_REQUIRE(storms.spawn(parked).has_value());
    for (cy::u32 tick = 1; tick <= 200; ++tick) {
        CY_REQUIRE(storms.advance(test::at_tick(tick), 1.0F, 0xC0FFEEULL).has_value());
    }

    cy::Array<LightningEvent> events(test::allocator());
    CY_REQUIRE(storms.drain_lightning(events).has_value());
    CY_REQUIRE((events.size()) > (0u));
    CY_CHECK_EQ(storms.pending_lightning(), 0u);
    for (const LightningEvent& event : events.span()) {
        CY_CHECK(event.storm == StormId{1});
        CY_CHECK_GT(event.intensity, 0.0F);
        CY_CHECK_NE(event.seed, 0u);
        // Inside the storm's own radius: a bolt from a storm falls where the storm is.
        const cy::f64 dx = event.position.x;
        const cy::f64 dz = event.position.z;
        CY_CHECK_LT(std::sqrt((dx * dx) + (dz * dz)), 12'001.0);
    }
}

CY_TEST_CASE("two peers agree about where the lightning struck") {
    // The replication claim: a storm's strikes are a pure function of (session seed, storm seed,
    // tick), so two peers reach the same bolts without exchanging one — and a peer that despawned a
    // different storm first still agrees, because the substream is keyed on identity and not on
    // position in an array.
    const auto run = [](bool despawn_first) {
        StormRegistry storms(test::allocator());
        CY_REQUIRE(
            storms.spawn(test::storm_at(1, 0.0, 0.0, StormType::Thunderstorm, 1.0F)).has_value());
        CY_REQUIRE(
            storms.spawn(test::storm_at(2, 60'000.0, 0.0, StormType::RainBand, 0.4F)).has_value());
        if (despawn_first) {
            CY_REQUIRE(storms.despawn(StormId{2}).has_value());
        }
        for (cy::u32 tick = 1; tick <= 120; ++tick) {
            CY_REQUIRE(storms.advance(test::at_tick(tick), 1.0F, 0xABCDEFULL).has_value());
        }
        cy::Array<LightningEvent> events(test::allocator());
        CY_REQUIRE(storms.drain_lightning(events).has_value());
        cy::Array<cy::u64> seeds(test::allocator());
        for (const LightningEvent& event : events.span()) {
            if (event.storm == StormId{1}) {
                CY_REQUIRE(seeds.push_back(event.seed).has_value());
            }
        }
        return seeds;
    };

    const cy::Array<cy::u64> with_both = run(false);
    const cy::Array<cy::u64> with_one = run(true);

    CY_REQUIRE((with_both.size()) > (0u));
    CY_REQUIRE_EQ(with_both.size(), with_one.size());
    for (cy::usize index = 0; index < with_both.size(); ++index) {
        CY_CHECK_EQ(with_both[index], with_one[index]);
    }

    // AND THE STORM'S OWN SEED IS WHAT DECIDES ITS BOLTS. Two storms of the same identity in two
    // sessions, differing only in `Storm::seed`, strike differently — which is why a replay records
    // the seed and why the draw is a SUBSTREAM of it rather than the shared stream with the
    // identity as its subject. Without this the substream would be unmeasured: the identity alone
    // already separates two storms in one session.
    const auto seeded = [](cy::u64 seed) {
        StormRegistry storms(test::allocator());
        cy::weather::Storm storm = test::storm_at(1, 0.0, 0.0, StormType::Thunderstorm, 1.0F);
        storm.velocity = cy::Vec2{0.0F, 0.0F};
        storm.seed = seed;
        CY_REQUIRE(storms.spawn(storm).has_value());
        for (cy::u32 tick = 1; tick <= 120; ++tick) {
            CY_REQUIRE(storms.advance(test::at_tick(tick), 1.0F, 0xABCDEFULL).has_value());
        }
        cy::Array<LightningEvent> events(test::allocator());
        CY_REQUIRE(storms.drain_lightning(events).has_value());
        cy::Array<cy::u64> seeds(test::allocator());
        for (const LightningEvent& event : events.span()) {
            CY_REQUIRE(seeds.push_back(event.seed).has_value());
        }
        return seeds;
    };
    const cy::Array<cy::u64> first = seeded(0x1111'2222ULL);
    const cy::Array<cy::u64> second = seeded(0x3333'4444ULL);
    CY_REQUIRE((first.size()) > (0u));
    bool differed = first.size() != second.size();
    for (cy::usize index = 0; index < first.size() && index < second.size() && !differed; ++index) {
        differed = first[index] != second[index];
    }
    CY_CHECK(differed);
}

CY_TEST_CASE("a storm costs a constant number of bytes and the wire form round-trips") {
    // "WHEN a storm crosses a multiplayer map THEN its identity, motion, and intensity SHALL be
    // replicated, and clouds SHALL be reconstructed on each client." The cost claim is the byte
    // count; the "no clouds" claim is that there is nothing else in the struct to write.
    StormRegistry storms(test::allocator());
    for (cy::u64 id = 1; id <= 4; ++id) {
        Storm storm = test::storm_at(id, static_cast<cy::f64>(id) * 1'000.0, 500.0,
                                     StormType::Squall, 0.25F * static_cast<cy::f32>(id));
        storm.velocity = cy::Vec2{static_cast<cy::f32>(id), -2.0F};
        CY_REQUIRE(storms.spawn(storm).has_value());
    }
    cy::Array<cy::u8> message(test::allocator());
    CY_REQUIRE(storms.encode(message).has_value());
    CY_CHECK_EQ(message.size(), 8u + (4u * StormRegistry::encoded_storm_bytes()));

    StormRegistry received(test::allocator());
    CY_REQUIRE(received.decode(message.span()).has_value());
    CY_REQUIRE_EQ(received.size(), 4u);
    for (cy::u64 id = 1; id <= 4; ++id) {
        const Storm* sent = storms.find(StormId{id});
        const Storm* got = received.find(StormId{id});
        CY_REQUIRE(sent != nullptr);
        CY_REQUIRE(got != nullptr);
        CY_CHECK_EQ(sent->position.x, got->position.x);
        CY_CHECK_EQ(sent->velocity.x, got->velocity.x);
        CY_CHECK_EQ(sent->intensity, got->intensity);
        CY_CHECK_EQ(sent->seed, got->seed);
        CY_CHECK(sent->type == got->type);
    }
    // The two registries now answer identically, which is the point of replicating the storm rather
    // than the weather it makes.
    CY_CHECK_EQ(storms.contribution_at(2'000.0, 500.0).precipitation_mm_per_hour,
                received.contribution_at(2'000.0, 500.0).precipitation_mm_per_hour);

    // A short message is refused rather than read past its end.
    CY_CHECK_FALSE(received.decode(cy::Span<const cy::u8>(message.begin(), 20)).has_value());
}

CY_TEST_CASE("contributions are reported per storm so an inspector can name one") {
    StormRegistry storms(test::allocator());
    CY_REQUIRE(
        storms.spawn(test::storm_at(1, 0.0, 0.0, StormType::Thunderstorm, 0.9F)).has_value());
    CY_REQUIRE(
        storms.spawn(test::storm_at(2, 8'000.0, 0.0, StormType::RainBand, 0.5F)).has_value());

    StormId ids[4];
    StormContribution parts[4];
    const cy::usize count = storms.contributions_at(4'000.0, 0.0, cy::Span<StormId>(ids, 4),
                                                    cy::Span<StormContribution>(parts, 4));
    CY_REQUIRE_EQ(count, 2u);
    cy::f32 summed = 0.0F;
    for (cy::usize index = 0; index < count; ++index) {
        summed += parts[index].precipitation_mm_per_hour;
    }
    CY_CHECK_NEAR(summed, storms.contribution_at(4'000.0, 0.0).precipitation_mm_per_hour, 0.001F);
}
