// The bus graph: routing, cycle rejection, solo, sends and the compiled order. Task 4.3.5.
//
// `audio` — "Bus graph". The three cases that carry the requirement are the cycle that is refused,
// the solo that silences everything not feeding it, and the compiled order that puts a send's
// source before its target — the last is what makes a reverb send arrive rather than lag a block.

#include <cy/core/memory/system_allocator.h>
#include <cy/servers/audio/bus.h>
#include <cy/test/test.h>

using cy::f32;
using cy::Name;
using namespace cy::audio;

namespace {

cy::Allocator& allocator() {
    return cy::system_allocator(cy::MemoryDomain::Audio);
}

[[nodiscard]] BusDescription named(const char* name) {
    BusDescription description;
    description.name = Name::intern(name);
    return description;
}

[[nodiscard]] cy::usize position_of(const BusGraph& graph, BusHandle handle) {
    const cy::Span<const BusHandle> order = graph.order();
    for (cy::usize i = 0; i < order.size(); ++i) {
        if (order[i] == handle) {
            return i;
        }
    }
    return order.size();
}

}  // namespace

CY_TEST_CASE("a graph starts with Master, and Master is the root") {
    BusGraph graph(allocator());
    CY_CHECK(graph.alive(graph.master()));
    CY_CHECK_EQ(graph.size(), 1U);
    CY_REQUIRE(graph.description(graph.master()) != nullptr);
    CY_CHECK(graph.description(graph.master())->output.is_null());
    // Master is not destroyable and routes nowhere: it is what every path ends at.
    CY_CHECK_FALSE(static_cast<bool>(graph.destroy(graph.master())));
    CY_CHECK_FALSE(static_cast<bool>(graph.set_output(graph.master(), graph.master())));
}

CY_TEST_CASE("a stale bus handle answers no rather than resolving to its replacement") {
    BusGraph graph(allocator());
    const auto first = graph.create(named("sfx"));
    CY_REQUIRE(static_cast<bool>(first));
    CY_REQUIRE(static_cast<bool>(graph.destroy(*first)));
    CY_CHECK_FALSE(graph.alive(*first));

    const auto second = graph.create(named("music"));
    CY_REQUIRE(static_cast<bool>(second));
    CY_CHECK_EQ(first->index(), second->index());  // the slot was reused
    CY_CHECK_FALSE(graph.alive(*first));           // the generation was not
    CY_CHECK(graph.alive(*second));
}

CY_TEST_CASE("a send that would close a loop is refused with a diagnostic") {
    // `audio`: "**WHEN** a send would create a cycle **THEN** the configuration SHALL be rejected
    // with a diagnostic." A cycle in a mixing graph is an infinite mix on the realtime thread.
    BusGraph graph(allocator());
    const auto sfx = graph.create(named("sfx"));
    const auto reverb = graph.create(named("reverb"));
    CY_REQUIRE(static_cast<bool>(sfx));
    CY_REQUIRE(static_cast<bool>(reverb));
    CY_REQUIRE(static_cast<bool>(graph.set_output(*sfx, graph.master())));
    CY_REQUIRE(static_cast<bool>(graph.set_output(*reverb, graph.master())));

    CY_REQUIRE(static_cast<bool>(graph.add_send(*sfx, *reverb, 0.3F)));
    // reverb already reaches sfx? No — but sfx reaches reverb, so the reverse send closes a loop.
    const cy::Status closed = graph.add_send(*reverb, *sfx, 0.1F);
    CY_REQUIRE_FALSE(static_cast<bool>(closed));
    CY_CHECK_EQ(closed.error().code, cy::ErrorCode::InvalidArgument);

    // And so does routing the output backwards.
    CY_CHECK_FALSE(static_cast<bool>(graph.set_output(*reverb, *sfx)));
    // A bus cannot send to itself either.
    CY_CHECK_FALSE(static_cast<bool>(graph.add_send(*sfx, *sfx, 0.5F)));
}

CY_TEST_CASE("a bus that something still routes into cannot be destroyed") {
    // A dangling output would silently drop a submix. Rerouting to Master is a decision a sound
    // designer makes, not one a destroy makes.
    BusGraph graph(allocator());
    const auto submix = graph.create(named("submix"));
    const auto child = graph.create(named("child"));
    CY_REQUIRE(static_cast<bool>(submix));
    CY_REQUIRE(static_cast<bool>(child));
    CY_REQUIRE(static_cast<bool>(graph.set_output(*child, *submix)));

    CY_CHECK_FALSE(static_cast<bool>(graph.destroy(*submix)));
    CY_REQUIRE(static_cast<bool>(graph.set_output(*child, graph.master())));
    CY_CHECK(static_cast<bool>(graph.destroy(*submix)));
}

CY_TEST_CASE("two sends to one target are refused, because their sum would depend on order") {
    BusGraph graph(allocator());
    const auto sfx = graph.create(named("sfx"));
    const auto reverb = graph.create(named("reverb"));
    CY_REQUIRE(static_cast<bool>(sfx));
    CY_REQUIRE(static_cast<bool>(reverb));
    CY_REQUIRE(static_cast<bool>(graph.add_send(*sfx, *reverb, 0.3F)));
    const cy::Status again = graph.add_send(*sfx, *reverb, 0.2F);
    CY_REQUIRE_FALSE(static_cast<bool>(again));
    CY_CHECK_EQ(again.error().code, cy::ErrorCode::AlreadyExists);

    CY_REQUIRE(static_cast<bool>(graph.set_send_level(*sfx, *reverb, 0.5F)));
    CY_REQUIRE_EQ(graph.sends(*sfx).size(), 1U);
    CY_CHECK_NEAR(graph.sends(*sfx)[0].level, 0.5F, 1e-6F);
    CY_REQUIRE(static_cast<bool>(graph.remove_send(*sfx, *reverb)));
    CY_CHECK_EQ(graph.sends(*sfx).size(), 0U);
}

CY_TEST_CASE("a bus takes at most its declared number of sends") {
    BusGraph graph(allocator());
    const auto source = graph.create(named("source"));
    CY_REQUIRE(static_cast<bool>(source));
    for (cy::u32 i = 0; i < BusGraph::kMaxSends; ++i) {
        const auto target = graph.create(named("t"));
        CY_REQUIRE(static_cast<bool>(target));
        CY_CHECK(static_cast<bool>(graph.add_send(*source, *target, 0.1F)));
    }
    const auto one_more = graph.create(named("one-more"));
    CY_REQUIRE(static_cast<bool>(one_more));
    const cy::Status refused = graph.add_send(*source, *one_more, 0.1F);
    CY_REQUIRE_FALSE(static_cast<bool>(refused));
    CY_CHECK_EQ(refused.error().code, cy::ErrorCode::OutOfRange);
}

CY_TEST_CASE("the compiled order puts every source before what it feeds") {
    // This is what makes a reverb send arrive in the same block rather than a block late.
    BusGraph graph(allocator());
    const auto sfx = graph.create(named("sfx"));
    const auto reverb = graph.create(named("reverb"));
    const auto voice = graph.create(named("voice"));
    CY_REQUIRE(static_cast<bool>(sfx));
    CY_REQUIRE(static_cast<bool>(reverb));
    CY_REQUIRE(static_cast<bool>(voice));
    CY_REQUIRE(static_cast<bool>(graph.set_output(*sfx, graph.master())));
    CY_REQUIRE(static_cast<bool>(graph.set_output(*reverb, graph.master())));
    CY_REQUIRE(static_cast<bool>(graph.set_output(*voice, *sfx)));
    CY_REQUIRE(static_cast<bool>(graph.add_send(*voice, *reverb, 0.3F)));

    CY_REQUIRE(static_cast<bool>(graph.compile()));
    CY_CHECK(graph.compiled());
    CY_REQUIRE_EQ(graph.order().size(), 4U);
    CY_CHECK_LT(position_of(graph, *voice), position_of(graph, *sfx));
    CY_CHECK_LT(position_of(graph, *voice), position_of(graph, *reverb));
    CY_CHECK_LT(position_of(graph, *sfx), position_of(graph, graph.master()));
    CY_CHECK_LT(position_of(graph, *reverb), position_of(graph, graph.master()));
}

CY_TEST_CASE("the compiled order is a function of the graph, not of when compile ran") {
    BusGraph first(allocator());
    BusGraph second(allocator());
    for (BusGraph* graph : {&first, &second}) {
        const auto a = graph->create(named("a"));
        const auto b = graph->create(named("b"));
        CY_REQUIRE(static_cast<bool>(a));
        CY_REQUIRE(static_cast<bool>(b));
        CY_REQUIRE(static_cast<bool>(graph->set_output(*a, graph->master())));
        CY_REQUIRE(static_cast<bool>(graph->set_output(*b, *a)));
        CY_REQUIRE(static_cast<bool>(graph->compile()));
    }
    CY_REQUIRE_EQ(first.order().size(), second.order().size());
    for (cy::usize i = 0; i < first.order().size(); ++i) {
        CY_CHECK_EQ(first.order()[i].bits(), second.order()[i].bits());
    }
}

CY_TEST_CASE("a structural change invalidates the compiled order") {
    BusGraph graph(allocator());
    CY_REQUIRE(static_cast<bool>(graph.compile()));
    CY_CHECK(graph.compiled());
    const auto bus = graph.create(named("late"));
    CY_REQUIRE(static_cast<bool>(bus));
    CY_CHECK_FALSE(graph.compiled());
}

CY_TEST_CASE("a soloed bus silences everything that does not feed it") {
    // `audio`: "**WHEN** a bus is soloed **THEN** buses that do not feed it SHALL be silenced for
    // the mix." Feeding is a reachability question, which is why it is asked of the graph.
    BusGraph graph(allocator());
    const auto music = graph.create(named("music"));
    const auto sfx = graph.create(named("sfx"));
    const auto footsteps = graph.create(named("footsteps"));
    CY_REQUIRE(static_cast<bool>(music));
    CY_REQUIRE(static_cast<bool>(sfx));
    CY_REQUIRE(static_cast<bool>(footsteps));
    CY_REQUIRE(static_cast<bool>(graph.set_output(*music, graph.master())));
    CY_REQUIRE(static_cast<bool>(graph.set_output(*sfx, graph.master())));
    CY_REQUIRE(static_cast<bool>(graph.set_output(*footsteps, *sfx)));

    CY_CHECK_FALSE(graph.any_solo());
    CY_CHECK(graph.audible(*music));

    CY_REQUIRE(static_cast<bool>(graph.set_solo(*sfx, true)));
    CY_CHECK(graph.any_solo());
    CY_CHECK(graph.audible(*sfx));
    CY_CHECK(graph.audible(*footsteps));  // it feeds the soloed bus
    CY_CHECK_FALSE(graph.audible(*music));
    CY_CHECK_FALSE(graph.audible(graph.master()));  // Master does not feed sfx

    CY_REQUIRE(static_cast<bool>(graph.set_solo(*sfx, false)));
    CY_CHECK(graph.audible(*music));
}

CY_TEST_CASE("a muted bus is silent whether or not anything is soloed") {
    BusGraph graph(allocator());
    const auto music = graph.create(named("music"));
    CY_REQUIRE(static_cast<bool>(music));
    CY_REQUIRE(static_cast<bool>(graph.set_mute(*music, true)));
    CY_CHECK_FALSE(graph.audible(*music));
    CY_REQUIRE(static_cast<bool>(graph.set_solo(*music, true)));
    CY_CHECK_FALSE(graph.audible(*music));
}

CY_TEST_CASE("effect latency accumulates along the path to Master") {
    // `audio`: "Effects SHALL… report their latency so the engine can compensate."
    BusGraph graph(allocator());
    const auto sfx = graph.create(named("sfx"));
    CY_REQUIRE(static_cast<bool>(sfx));
    CY_REQUIRE(static_cast<bool>(graph.set_output(*sfx, graph.master())));

    BusEffect convolution;
    convolution.kind = EffectKind::Gain;
    convolution.latency_frames = 256;
    CY_REQUIRE(static_cast<bool>(graph.add_effect(*sfx, convolution)));
    BusEffect master_limiter;
    master_limiter.kind = EffectKind::Limiter;
    master_limiter.latency_frames = 64;
    CY_REQUIRE(static_cast<bool>(graph.add_effect(graph.master(), master_limiter)));

    CY_CHECK_EQ(graph.latency_frames(*sfx), 320U);
    CY_CHECK_EQ(graph.latency_frames(graph.master()), 64U);

    // A bypassed effect reports no latency, because it is not processing.
    CY_REQUIRE(static_cast<bool>(graph.clear_effects(*sfx)));
    convolution.bypass = true;
    CY_REQUIRE(static_cast<bool>(graph.add_effect(*sfx, convolution)));
    CY_CHECK_EQ(graph.latency_frames(*sfx), 64U);
}

CY_TEST_CASE("a bus takes at most its declared number of effects") {
    BusGraph graph(allocator());
    for (cy::u32 i = 0; i < BusGraph::kMaxEffects; ++i) {
        CY_CHECK(static_cast<bool>(graph.add_effect(graph.master(), BusEffect{})));
    }
    CY_CHECK_FALSE(static_cast<bool>(graph.add_effect(graph.master(), BusEffect{})));
}
