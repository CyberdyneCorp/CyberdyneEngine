// The knowledge store: decay, forgetting, eviction, and squad sharing with delay and fidelity
// loss. M8.b task 6.4.

#include <cy/ai/knowledge.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/test/test.h>

using namespace cy;
using namespace cy::ai;

namespace {

Allocator& allocator() noexcept {
    return system_allocator(MemoryDomain::World);
}

[[nodiscard]] Entity subject(u32 index) noexcept {
    return Entity::make(index, 1);
}

[[nodiscard]] KnowledgeParams params() noexcept {
    KnowledgeParams out;
    out.decay_per_tick = 0.05F;
    out.forget_below = 0.2F;
    out.capacity = 4;
    return out;
}

}  // namespace

CY_TEST_CASE("a target that breaks line of sight is remembered with falling confidence") {
    // `ai-system`: "the agent SHALL retain its last known position with decaying confidence,
    // enabling SEARCH BEHAVIOUR rather than instant forgetting".
    KnowledgeStore store(allocator(), params());
    CY_REQUIRE(store
                   .perceive(subject(7), Vec3{1.0F, 0.0F, 2.0F}, Vec3{}, SenseKind::Vision, 1.0F,
                             1.0F, 100)
                   .has_value());
    CY_REQUIRE_EQ(store.size(), 1U);
    CY_REQUIRE(store.find(subject(7)) != nullptr);
    CY_CHECK_NEAR(store.find(subject(7))->confidence, 1.0F, 1e-6F);

    // Eight ticks unseen: 8 * 0.05 = 0.4 lost, and the last known position is still there.
    const KnowledgeUpdate aged = store.age(108);
    CY_CHECK_EQ(aged.forgotten, 0U);
    CY_CHECK_EQ(aged.entries, 1U);
    CY_CHECK_NEAR(store.find(subject(7))->confidence, 0.6F, 1e-5F);
    CY_CHECK_NEAR(store.find(subject(7))->last_position.x, 1.0F, 1e-6F);
    CY_CHECK_EQ(store.find(subject(7))->first_tick, 100U);

    // Below the threshold it is forgotten, which is what makes a search end.
    const KnowledgeUpdate later = store.age(118);
    CY_CHECK_EQ(later.forgotten, 1U);
    CY_CHECK_EQ(store.size(), 0U);
}

CY_TEST_CASE("decay is a function of ticks, so re-running the same ticks gives the same beliefs") {
    const auto run = [](u32 stride) noexcept {
        KnowledgeStore store(allocator(), params());
        CY_REQUIRE(store.perceive(subject(1), Vec3{}, Vec3{}, SenseKind::Vision, 1.0F, 1.0F, 0)
                       .has_value());
        for (u32 tick = stride; tick <= 10; tick += stride) {
            (void)store.age(tick);
        }
        const KnowledgeEntry* entry = store.find(subject(1));
        return (entry != nullptr) ? entry->confidence : -1.0F;
    };
    // Aged one tick at a time, or five ticks at a time, or in one jump: the same answer, because
    // the decay is (tick - last_tick) * rate and not an accumulation.
    CY_CHECK_EQ(run(1), run(5));
    CY_CHECK_EQ(run(1), run(10));
}

CY_TEST_CASE("a full store evicts the least relevant, and refuses something worth less") {
    KnowledgeStore store(allocator(), params());
    for (u32 index = 0; index < 4; ++index) {
        CY_REQUIRE(store
                       .perceive(subject(index), Vec3{}, Vec3{}, SenseKind::Vision, 1.0F,
                                 1.0F + static_cast<f32>(index), 0)
                       .has_value());
    }
    CY_REQUIRE_EQ(store.size(), 4U);

    // More relevant than the worst: it takes its place.
    CY_REQUIRE(
        store.perceive(subject(9), Vec3{}, Vec3{}, SenseKind::Vision, 1.0F, 5.0F, 0).has_value());
    CY_CHECK_EQ(store.size(), 4U);
    CY_CHECK(store.find(subject(9)) != nullptr);
    CY_CHECK(store.find(subject(0)) == nullptr);

    // Less relevant than everything: nothing is displaced for it.
    CY_REQUIRE(
        store.perceive(subject(11), Vec3{}, Vec3{}, SenseKind::Vision, 0.1F, 0.1F, 0).has_value());
    CY_CHECK_EQ(store.size(), 4U);
    CY_CHECK(store.find(subject(11)) == nullptr);
}

CY_TEST_CASE("the best target ranks by confidence times relevance, with a stable tie-break") {
    KnowledgeStore store(allocator(), params());
    CY_REQUIRE(
        store.perceive(subject(3), Vec3{}, Vec3{}, SenseKind::Vision, 0.9F, 1.0F, 0).has_value());
    CY_REQUIRE(
        store.perceive(subject(4), Vec3{}, Vec3{}, SenseKind::Hearing, 0.4F, 3.0F, 0).has_value());
    CY_REQUIRE(store.best_target() != nullptr);
    CY_CHECK_EQ(store.best_target()->subject, subject(4));

    // Two identical scores resolve to the lower entity, in every run.
    KnowledgeStore tied(allocator(), params());
    CY_REQUIRE(
        tied.perceive(subject(8), Vec3{}, Vec3{}, SenseKind::Vision, 0.5F, 2.0F, 0).has_value());
    CY_REQUIRE(
        tied.perceive(subject(2), Vec3{}, Vec3{}, SenseKind::Vision, 0.5F, 2.0F, 0).has_value());
    CY_CHECK_EQ(tied.best_target()->subject, subject(2));
}

CY_TEST_CASE("a squad channel delivers late, degraded, and never back to its author") {
    // `ai-system`: "other members SHALL receive it WITH THE CONFIGURED DELAY, rather than instantly
    // and perfectly".
    ChannelParams channel_params;
    channel_params.delay_ticks = 5;
    channel_params.fidelity = 0.5F;
    channel_params.position_spread = 1.0F;
    KnowledgeChannel channel(allocator(), channel_params);

    KnowledgeStore scout(allocator(), params());
    KnowledgeStore mate(allocator(), params());
    CY_REQUIRE(scout
                   .perceive(subject(20), Vec3{10.0F, 0.0F, 10.0F}, Vec3{}, SenseKind::Vision, 1.0F,
                             2.0F, 40)
                   .has_value());
    CY_REQUIRE(channel.post(subject(1), *scout.find(subject(20)), 40).has_value());
    CY_CHECK_EQ(channel.pending(), 1U);

    // Not yet.
    CY_CHECK_EQ(channel.deliver(subject(2), mate, 44), 0U);
    CY_CHECK_EQ(mate.size(), 0U);

    // On the delivery tick, degraded and displaced.
    CY_CHECK_EQ(channel.deliver(subject(2), mate, 45), 1U);
    CY_REQUIRE(mate.find(subject(20)) != nullptr);
    CY_CHECK_NEAR(mate.find(subject(20))->confidence, 0.5F, 1e-5F);
    CY_CHECK_EQ(mate.find(subject(20))->sense, SenseKind::Shared);
    const f32 error = length(mate.find(subject(20))->last_position - Vec3{10.0F, 0.0F, 10.0F});
    CY_CHECK_GT(error, 0.0F);
    CY_CHECK_LE(error, 1.5F);

    // Never back to the author.
    KnowledgeStore author(allocator(), params());
    CY_CHECK_EQ(channel.deliver(subject(1), author, 45), 0U);

    // And the post is collected once its tick is behind.
    CY_CHECK_EQ(channel.collect(45), 0U);
    CY_CHECK_EQ(channel.collect(46), 1U);
    CY_CHECK_EQ(channel.pending(), 0U);
}

CY_TEST_CASE("sharing is deterministic: two runs deliver the same approximate position") {
    ChannelParams channel_params;
    channel_params.delay_ticks = 2;
    channel_params.position_spread = 3.0F;

    const auto run = [&channel_params]() noexcept {
        KnowledgeChannel channel(allocator(), channel_params);
        KnowledgeStore scout(allocator(), params());
        KnowledgeStore mate(allocator(), params());
        CY_REQUIRE(scout
                       .perceive(subject(31), Vec3{4.0F, 1.0F, 5.0F}, Vec3{}, SenseKind::Vision,
                                 1.0F, 1.0F, 7)
                       .has_value());
        CY_REQUIRE(channel.post(subject(1), *scout.find(subject(31)), 7).has_value());
        CY_REQUIRE_EQ(channel.deliver(subject(2), mate, 9), 1U);
        return mate.find(subject(31))->last_position;
    };
    const Vec3 first = run();
    const Vec3 second = run();
    CY_CHECK_EQ(first.x, second.x);
    CY_CHECK_EQ(first.z, second.z);
    // The height is not spread: an agent that heard where something is does not misremember which
    // floor it was on.
    CY_CHECK_NEAR(first.y, 1.0F, 1e-6F);
}
