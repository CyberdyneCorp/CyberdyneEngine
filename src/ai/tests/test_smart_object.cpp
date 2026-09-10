// Smart objects: affordances found by capability, slot contention, and reservations released three
// ways. M8.b task 6.4.

#include <cy/ai/smart_object.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/test/test.h>

using namespace cy;
using namespace cy::ai;

namespace {

Allocator& allocator() noexcept {
    return system_allocator(MemoryDomain::World);
}

[[nodiscard]] Entity agent(u32 index) noexcept {
    return Entity::make(index, 1);
}

[[nodiscard]] Affordance recharge(Entity provider, Vec3 at, u16 capacity = 1) noexcept {
    Affordance affordance;
    affordance.capability = Name::intern("Recharge");
    affordance.provider = provider;
    affordance.interaction_point = at;
    affordance.capacity = capacity;
    return affordance;
}

/// A liveness predicate over a fixed set, standing in for `World::is_alive`.
struct Graveyard {
    u32 dead = 0;
};

bool alive(Entity entity, void* user) noexcept {
    return entity.index() != static_cast<const Graveyard*>(user)->dead;
}

}  // namespace

CY_TEST_CASE("an agent asks for a capability and never for an object class") {
    // `ai-system`: "WHEN a new charging station type is added advertising `Recharge` THEN existing
    // agents SHALL use it with no change to any AI graph." The registry is keyed on the name, so a
    // second, different provider is found by the same query.
    SmartObjectRegistry registry(allocator());
    CY_REQUIRE(registry.advertise(recharge(agent(100), Vec3{5.0F, 0.0F, 0.0F})).has_value());

    Array<AffordanceCandidate> found(allocator());
    CY_REQUIRE(registry.find(Name::intern("Recharge"), Vec3{}, 20.0F, ~u64{0}, found).has_value());
    CY_REQUIRE_EQ(found.size(), usize{1});
    CY_CHECK_EQ(found[0].provider, agent(100));

    CY_REQUIRE(registry.advertise(recharge(agent(101), Vec3{2.0F, 0.0F, 0.0F})).has_value());
    CY_REQUIRE(registry.find(Name::intern("Recharge"), Vec3{}, 20.0F, ~u64{0}, found).has_value());
    CY_REQUIRE_EQ(found.size(), usize{2});
    // Nearest first.
    CY_CHECK_EQ(found[0].provider, agent(101));

    // A different capability finds nothing, which is what makes the key meaningful.
    CY_REQUIRE(registry.find(Name::intern("Seat"), Vec3{}, 20.0F, ~u64{0}, found).has_value());
    CY_CHECK(found.empty());
}

CY_TEST_CASE("an affordance an agent lacks the capability for is not offered") {
    SmartObjectRegistry registry(allocator());
    Affordance gated = recharge(agent(100), Vec3{1.0F, 0.0F, 0.0F});
    gated.requires_capabilities = u64{1} << 4U;
    CY_REQUIRE(registry.advertise(gated).has_value());

    Array<AffordanceCandidate> found(allocator());
    CY_REQUIRE(registry.find(Name::intern("Recharge"), Vec3{}, 20.0F, 0, found).has_value());
    CY_CHECK(found.empty());
    CY_REQUIRE(
        registry.find(Name::intern("Recharge"), Vec3{}, 20.0F, u64{1} << 4U, found).has_value());
    CY_CHECK_EQ(found.size(), usize{1});
}

CY_TEST_CASE("exactly one agent reserves a single-occupancy slot and the other takes the next") {
    // `ai-system`'s contention scenario, in full: "exactly one SHALL reserve it and the other SHALL
    // receive the next candidate."
    SmartObjectRegistry registry(allocator());
    const Expected<SlotId, Error> near =
        registry.advertise(recharge(agent(100), Vec3{1.0F, 0.0F, 0.0F}));
    const Expected<SlotId, Error> far =
        registry.advertise(recharge(agent(101), Vec3{6.0F, 0.0F, 0.0F}));
    CY_REQUIRE(near.has_value());
    CY_REQUIRE(far.has_value());

    Array<AffordanceCandidate> found(allocator());
    CY_REQUIRE(registry.find(Name::intern("Recharge"), Vec3{}, 20.0F, ~u64{0}, found).has_value());
    CY_REQUIRE_EQ(found.size(), usize{2});

    CY_REQUIRE(registry.reserve(found[0].slot, agent(1), 10).has_value());
    const Status refused = registry.reserve(found[0].slot, agent(2), 10);
    CY_REQUIRE_FALSE(refused.has_value());
    CY_CHECK_EQ(refused.error().code, ErrorCode::AlreadyExists);
    CY_CHECK(registry.reserve(found[1].slot, agent(2), 10).has_value());

    // And the taken one is no longer offered.
    CY_REQUIRE(registry.find(Name::intern("Recharge"), Vec3{}, 20.0F, ~u64{0}, found).has_value());
    CY_CHECK(found.empty());
}

CY_TEST_CASE("a slot with room for two takes two and refuses a third") {
    SmartObjectRegistry registry(allocator());
    const Expected<SlotId, Error> doorway =
        registry.advertise(recharge(agent(100), Vec3{1.0F, 0.0F, 0.0F}, 2));
    CY_REQUIRE(doorway.has_value());
    CY_CHECK_EQ(registry.free_capacity(*doorway), 2U);
    CY_REQUIRE(registry.reserve(*doorway, agent(1), 0).has_value());
    CY_CHECK_EQ(registry.free_capacity(*doorway), 1U);
    CY_REQUIRE(registry.reserve(*doorway, agent(2), 0).has_value());
    CY_CHECK_EQ(registry.free_capacity(*doorway), 0U);
    CY_CHECK_FALSE(registry.reserve(*doorway, agent(3), 0).has_value());
}

CY_TEST_CASE("a reservation is released on completion, on failure and on destruction") {
    SmartObjectRegistry registry(allocator());
    const Expected<SlotId, Error> slot =
        registry.advertise(recharge(agent(100), Vec3{1.0F, 0.0F, 0.0F}));
    CY_REQUIRE(slot.has_value());

    // Completion or failure: the same call, because from here they are the same fact.
    CY_REQUIRE(registry.reserve(*slot, agent(1), 0).has_value());
    CY_REQUIRE(registry.release(agent(1)).has_value());
    CY_CHECK_EQ(registry.free_capacity(*slot), 1U);
    CY_CHECK_FALSE(registry.release(agent(1)).has_value());

    // Destruction: the holder cannot release anything itself, so the registry sweeps.
    CY_REQUIRE(registry.reserve(*slot, agent(2), 0).has_value());
    CY_CHECK(registry.reservation_of(agent(2)) != nullptr);
    Graveyard graveyard;
    graveyard.dead = 2;
    CY_CHECK_EQ(registry.release_missing(&alive, &graveyard), 1U);
    CY_CHECK_EQ(registry.free_capacity(*slot), 1U);
    CY_CHECK(registry.reservation_of(agent(2)) == nullptr);
}

CY_TEST_CASE("withdrawing an object releases every claim on it") {
    SmartObjectRegistry registry(allocator());
    const Expected<SlotId, Error> slot =
        registry.advertise(recharge(agent(100), Vec3{1.0F, 0.0F, 0.0F}, 2));
    CY_REQUIRE(slot.has_value());
    CY_REQUIRE(registry.reserve(*slot, agent(1), 0).has_value());
    CY_REQUIRE(registry.reserve(*slot, agent(2), 0).has_value());
    CY_CHECK_EQ(registry.reservations().size(), usize{2});

    CY_REQUIRE(registry.withdraw(*slot).has_value());
    CY_CHECK_EQ(registry.size(), 0U);
    CY_CHECK(registry.reservations().empty());
    CY_CHECK(registry.affordance(*slot) == nullptr);
}

CY_TEST_CASE("an affordance with no capability name or no room is refused") {
    SmartObjectRegistry registry(allocator());
    Affordance nameless;
    nameless.capacity = 1;
    CY_CHECK_FALSE(registry.advertise(nameless).has_value());
    Affordance full = recharge(agent(100), Vec3{});
    full.capacity = 0;
    CY_CHECK_FALSE(registry.advertise(full).has_value());
}
