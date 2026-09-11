// The compiled program: the two indexes and channel evaluation.
//
// The claim these cases exist to keep honest is "Cost scales with what is active". A test cannot
// measure a constant factor reliably on a shared machine, so what is asserted instead is the
// STRUCTURE that makes it true — the index answers with the segments in one bucket, and the event
// query is a contiguous span — plus, in test_scale.cpp, that a million keys change neither.

#include "fixture.h"

#include <cy/sequencing/compile.h>
#include <cy/test/test.h>

using namespace cy;
using namespace cy::sequencing;
using namespace cy::sequencing::testing;

namespace {

/// Three light sections, spread out, plus three events and two markers.
[[nodiscard]] SequenceSource spread_sequence() noexcept {
    SequenceSource source(allocator());
    source.name = Name::intern("spread");
    source.stable_id = 1;
    source.rate = Rate{24, 1};
    source.duration = SequenceTime::from_frame(300);
    (void)source.bindings.push_back(binding_of(10, "lamp", BindingKind::Entity));

    Track light = track_of("lamp", TrackKind::Property, 100, 10);
    light.subsystem = SubsystemId::Light;
    for (i64 index = 0; index < 3; ++index) {
        Section section =
            section_over(index * 100, (index * 100) + 20, static_cast<u32>(index + 1));
        Channel intensity = scalar_channel("intensity", static_cast<u32>(index + 1));
        add_key(intensity, index * 100, 0.0F);
        add_key(intensity, (index * 100) + 20, 1.0F);
        (void)section.channels.push_back(std::move(intensity));
        (void)light.sections.push_back(std::move(section));
    }
    for (i64 index = 0; index < 3; ++index) {
        EventDeclaration event;
        event.time = SequenceTime::from_frame((index * 100) + 10);
        event.type = Name::intern("beat");
        event.stable_id = static_cast<u32>(index + 1);
        (void)light.events.push_back(event);
    }
    (void)source.tracks.push_back(std::move(light));
    return source;
}

}  // namespace

CY_TEST_CASE("sequence_program: the interval index answers with what is active and nothing else") {
    Registry registry(allocator());
    CY_REQUIRE(registry.build());
    const SequenceSource source = spread_sequence();
    Program program(allocator());
    CompileReport report(allocator());
    SequenceCompiler compiler(allocator(), registry.registry);
    CY_REQUIRE(compiler.compile(source, CompileOptions{}, program, report));
    CY_REQUIRE_EQ(program.segments().size(), 3U);

    Array<u32> active(allocator());
    CY_REQUIRE(program.active_at(SequenceTime::from_frame(10), active));
    CY_REQUIRE_EQ(active.size(), 1U);
    CY_CHECK_EQ(active[0], 0U);

    // Between two sections: nothing is active, and the answer costs the same as any other.
    CY_REQUIRE(program.active_at(SequenceTime::from_frame(60), active));
    CY_CHECK_EQ(active.size(), 0U);

    CY_REQUIRE(program.active_at(SequenceTime::from_frame(205), active));
    CY_REQUIRE_EQ(active.size(), 1U);
    CY_CHECK_EQ(active[0], 2U);

    // Beyond the end, and before the start.
    CY_REQUIRE(program.active_at(SequenceTime::from_frame(1000), active));
    CY_CHECK_EQ(active.size(), 0U);
    CY_REQUIRE(program.active_at(SequenceTime::from_frame(-10), active));
    CY_CHECK_EQ(active.size(), 0U);
}

CY_TEST_CASE("sequence_program: a pre-roll makes a section active before it starts") {
    Registry registry(allocator());
    CY_REQUIRE(registry.build());
    SequenceSource source = spread_sequence();
    source.tracks[0].sections[1].pre_roll = SequenceTime::from_frame(24);

    Program program(allocator());
    CompileReport report(allocator());
    SequenceCompiler compiler(allocator(), registry.registry);
    CY_REQUIRE(compiler.compile(source, CompileOptions{}, program, report));

    Array<u32> active(allocator());
    CY_REQUIRE(program.active_at(SequenceTime::from_frame(80), active));
    CY_REQUIRE_EQ(active.size(), 1U);
    CY_CHECK_EQ(active[0], 1U);
    CY_CHECK_LT(SequenceTime::from_frame(80).ticks(), program.segments()[1].start);
}

CY_TEST_CASE("sequence_program: events crossed are a contiguous range of the index") {
    Registry registry(allocator());
    CY_REQUIRE(registry.build());
    const SequenceSource source = spread_sequence();
    Program program(allocator());
    CompileReport report(allocator());
    SequenceCompiler compiler(allocator(), registry.registry);
    CY_REQUIRE(compiler.compile(source, CompileOptions{}, program, report));
    CY_REQUIRE_EQ(program.events().size(), 3U);

    // One frame's interval crosses one event.
    Span<const CompiledEvent> crossed =
        program.events_between(SequenceTime::from_frame(9), SequenceTime::from_frame(10));
    CY_CHECK_EQ(crossed.size(), 1U);

    // A four-minute seek crosses all three, in one span, at the same cost.
    crossed = program.events_between(SequenceTime::from_frame(0), SequenceTime::from_frame(300));
    CY_CHECK_EQ(crossed.size(), 3U);

    // Half open at the bottom: an event exactly at `from` belonged to the previous interval, so a
    // paused sequence does not re-fire it every frame.
    crossed = program.events_between(SequenceTime::from_frame(10), SequenceTime::from_frame(10));
    CY_CHECK_EQ(crossed.size(), 0U);

    crossed = program.events_between(SequenceTime::from_frame(30), SequenceTime::from_frame(80));
    CY_CHECK_EQ(crossed.size(), 0U);
}

CY_TEST_CASE("sequence_program: a channel evaluates, and a rotation interpolates as a rotation") {
    Registry registry(allocator());
    CY_REQUIRE(registry.build());
    SequenceSource source = spread_sequence();

    // A quarter turn about Y, keyed from identity, plus its short-arc twin: the second key is the
    // negation of the first's destination, which a component-wise blend would take the long way.
    Track pose = track_of("pose", TrackKind::Animation, 200, 10);
    Section section = section_over(0, 24, 9000);
    Channel rotation(allocator());
    rotation.property = Name::intern("rotation");
    rotation.type = ChannelType::Rotation;
    Key first;
    first.time = SequenceTime::from_frame(0);
    first.value[0] = 0.0F;
    first.value[1] = 0.0F;
    first.value[2] = 0.0F;
    first.value[3] = 1.0F;
    (void)rotation.keys.push_back(first);
    Key second;
    second.time = SequenceTime::from_frame(24);
    second.value[0] = 0.0F;
    second.value[1] = -0.3826834F;
    second.value[2] = 0.0F;
    second.value[3] = -0.9238795F;  // the same orientation as (0, 0.38, 0, 0.92), negated
    (void)rotation.keys.push_back(second);
    (void)section.channels.push_back(std::move(rotation));
    (void)pose.sections.push_back(std::move(section));
    (void)source.tracks.push_back(std::move(pose));

    Program program(allocator());
    CompileReport report(allocator());
    SequenceCompiler compiler(allocator(), registry.registry);
    CY_REQUIRE(compiler.compile(source, CompileOptions{}, program, report));

    // The rotation channel is the one whose compiled type says so — found rather than assumed,
    // because "the last one" is a fact about the compiler's ordering and not about this case.
    u32 channel = Program::kInvalidIndex;
    for (usize index = 0; index < program.channels().size(); ++index) {
        if (program.channels()[index].type == ChannelType::Rotation) {
            channel = static_cast<u32>(index);
        }
    }
    CY_REQUIRE_NE(channel, Program::kInvalidIndex);
    ChannelValue value;
    CY_REQUIRE(program.sample(channel, SequenceTime::from_frame(12), value));
    // Short arc: half way to a 45-degree turn, so y is positive and small. A component-wise blend
    // of the authored keys would give y = -0.19 and w = 0.04 — the long way round.
    CY_CHECK_GT(value.components[1], 0.0F);
    CY_CHECK_LT(value.components[1], 0.25F);
    CY_CHECK_GT(value.components[3], 0.9F);
    const f32 length =
        (value.components[0] * value.components[0]) + (value.components[1] * value.components[1]) +
        (value.components[2] * value.components[2]) + (value.components[3] * value.components[3]);
    CY_CHECK_NEAR(length, 1.0F, 1e-5F);

    // A scalar channel interpolates linearly, and holds outside its keys.
    ChannelValue scalar;
    CY_REQUIRE(program.sample(0, SequenceTime::from_frame(10), scalar));
    CY_CHECK_NEAR(scalar.components[0], 0.5F, 1e-5F);
    CY_REQUIRE(program.sample(0, SequenceTime::from_frame(1000), scalar));
    CY_CHECK_NEAR(scalar.components[0], 1.0F, 1e-5F);
}

CY_TEST_CASE("sequence_program: a marker is found by name and a missing one is not") {
    Registry registry(allocator());
    CY_REQUIRE(registry.build());
    SequenceSource source = spread_sequence();
    MarkerDeclaration marker;
    marker.time = SequenceTime::from_frame(150);
    marker.name = Name::intern("impact");
    (void)source.markers.push_back(marker);

    Program program(allocator());
    CompileReport report(allocator());
    SequenceCompiler compiler(allocator(), registry.registry);
    CY_REQUIRE(compiler.compile(source, CompileOptions{}, program, report));

    const CompiledMarker* found = program.find_marker(Name::intern("impact"));
    CY_REQUIRE(found != nullptr);
    CY_CHECK_EQ(found->ticks, SequenceTime::from_frame(150).ticks());
    CY_CHECK(program.find_marker(Name::intern("nothing")) == nullptr);
}

CY_TEST_CASE("sequence_program: the preload plan says when each asset is needed and released") {
    Registry registry(allocator());
    CY_REQUIRE(registry.build());
    SequenceSource source = spread_sequence();
    source.tracks[0].sections[0].asset = 0xA55E7;
    source.tracks[0].sections[0].pre_roll = SequenceTime::from_frame(12);
    source.tracks[0].sections[2].asset = 0xA55E7;  // the same asset, later
    source.tracks[0].sections[1].asset = 0xBEEF;

    Program program(allocator());
    CompileReport report(allocator());
    SequenceCompiler compiler(allocator(), registry.registry);
    CY_REQUIRE(compiler.compile(source, CompileOptions{}, program, report));

    CY_REQUIRE_EQ(program.preload_plan().size(), 2U);
    // Merged: one entry per asset, required at the earliest need — the pre-roll, not the start —
    // and releasable at the latest. Two shots sharing an asset must not release it between them.
    const PreloadEntry& shared = program.preload_plan()[0];
    CY_CHECK_EQ(shared.asset, 0xA55E7U);
    CY_CHECK_EQ(shared.required_at, SequenceTime::from_frame(-12).ticks());
    CY_CHECK_EQ(shared.releasable_at, SequenceTime::from_frame(220).ticks());
}
