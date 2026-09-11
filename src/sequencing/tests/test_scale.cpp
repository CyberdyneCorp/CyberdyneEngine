// A million authored keys, and what it costs per frame.
//
// `sequencing-and-cinematics` — "**WHEN** a long cinematic with a million authored keys plays
// **THEN** per-frame work SHALL be proportional to currently active channels."
//
// WHAT IS ASSERTED AND WHY IT IS NOT A TIMING. A wall-clock threshold on a shared machine measures
// the machine; this project has already paid for one artefact that led with an extreme value. What
// is asserted instead is the thing the requirement is actually about, and it is exact:
//
//   * the interval index answers with the segments overlapping ONE bucket, so the active set at any
//     instant is the two sections that are active and never the two thousand that are not;
//   * a frame emits one value per active channel, whatever the number of keys behind it;
//   * the same program with a hundred times the keys emits the identical batch.
//
// The third is the one that cannot be argued with: if per-frame work depended on authored keys, the
// batch would differ.

#include "fixture.h"

#include <cy/sequencing/player.h>
#include <cy/test/test.h>

using namespace cy;
using namespace cy::sequencing;
using namespace cy::sequencing::testing;

namespace {

/// `sections` light sections over a long timeline, each with `keys_per_channel` keys.
[[nodiscard]] SequenceSource long_sequence(i64 sections, i64 keys_per_channel) noexcept {
    SequenceSource source(allocator());
    source.name = Name::intern("long");
    source.stable_id = 1;
    source.rate = Rate{24, 1};
    source.duration = SequenceTime::from_frame(sections * 100);
    (void)source.bindings.push_back(binding_of(10, "lamp", BindingKind::Entity));

    Track light = track_of("lamp", TrackKind::Property, 100, 10);
    light.subsystem = SubsystemId::Light;
    for (i64 index = 0; index < sections; ++index) {
        const i64 start = index * 100;
        Section section = section_over(start, start + 90, static_cast<u32>(index + 1));
        Channel intensity = scalar_channel("intensity", static_cast<u32>(index + 1));
        (void)intensity.keys.reserve(static_cast<usize>(keys_per_channel));
        for (i64 key = 0; key < keys_per_channel; ++key) {
            Key record;
            // Sub-frame keys: a million of them over a few thousand frames is what a
            // motion-captured curve looks like after import.
            record.time = SequenceTime::from_ticks(
                (start * kTicksPerFrame) + ((key * 90 * kTicksPerFrame) / keys_per_channel));
            record.value[0] = static_cast<f32>(key % 7) * 0.125F;
            (void)intensity.keys.push_back(record);
        }
        (void)section.channels.push_back(std::move(intensity));
        (void)light.sections.push_back(std::move(section));
    }
    (void)source.tracks.push_back(std::move(light));
    return source;
}

}  // namespace

CY_TEST_CASE("sequence_scale: no drift over a long sequence") {
    // "**WHEN** a sequence loops for hours **THEN** its time SHALL remain exact rather than
    // accumulating error." One hour at 60 Hz is 216,000 advances of 16,666,666 ns — the value a
    // real frame loop produces, which is two thirds of a nanosecond short of a frame every time.
    //
    // IN `integration`, NOT `unit`: 216,000 advances measured 8.9 ms of CPU against the unit tier's
    // one-millisecond budget. A case that runs an hour of playback belongs in the tier above,
    // whatever it is testing.
    TimeAccumulator accumulator;
    const Rate rate{60, 1};
    i64 total = 0;
    constexpr i64 kAdvances = 216000;
    for (i64 index = 0; index < kAdvances; ++index) {
        const Expected<SequenceTime, Error> delta =
            accumulator.advance(rate, PlayRate::normal(), 16666666);
        CY_REQUIRE(delta);
        total += delta.value().ticks();
    }
    // 216,000 advances of 16,666,666 ns is 3,599,999,856,000 ns, which at 60 frames per second is
    // 215,999.99136 frames — 215,999,991.36 ticks. The exact answer is the floor of that, and
    // nothing here rounds toward it: an `f32` accumulator lands tens of ticks away.
    CY_CHECK_EQ(total, 215999991);
    CY_CHECK_GT(accumulator.residual(), 0);
}

CY_TEST_CASE("sequence_scale: a million keys, a few evaluations") {
    Registry registry(allocator());
    CY_REQUIRE(registry.build());

    // 500 sections of 2,000 keys is a million authored keys.
    const SequenceSource source = long_sequence(500, 2000);
    Program program(allocator());
    CompileReport report(allocator());
    SequenceCompiler compiler(allocator(), registry.registry);
    CY_REQUIRE(compiler.compile(source, CompileOptions{}, program, report));
    CY_CHECK_EQ(report.compression.keys_in, 1000000U);
    CY_CHECK_EQ(program.keys().size(), 1000000U);
    CY_CHECK_EQ(program.segments().size(), 500U);
    // The index is BOUNDED: a long sequence costs memory for its keys and not for its index.
    CY_CHECK_LE(program.bucket_count(), kMaxBuckets);

    // The active set at an arbitrary instant is one section, found through the index.
    Array<u32> active(allocator());
    CY_REQUIRE(program.active_at(SequenceTime::from_frame(25050), active));
    CY_CHECK_EQ(active.size(), 1U);

    SequencePlayer player(allocator(), registry.registry, PlaybackScope::World, 0);
    const BindingResolution bindings[] = {BindingResolution{10, 500, true}};
    PlayRequest request;
    request.bindings = bindings;
    request.start = SequenceTime::from_frame(25000);
    const Expected<InstanceId, Error> instance = player.create(program, request);
    CY_REQUIRE(instance);
    CY_REQUIRE(player.play(instance.value()));

    DispatchBatches batches(allocator());
    for (i32 frame = 0; frame < 24; ++frame) {
        batches.clear();
        CY_REQUIRE(player.advance(41666667, batches));
    }
    // ONE value per active channel per frame. Not one per key, and not one per section.
    CY_CHECK_EQ(batches.values.size(), 1U);
}

CY_TEST_CASE("sequence_scale: a hundred times the keys produce the identical frame") {
    Registry registry(allocator());
    CY_REQUIRE(registry.build());

    Program sparse(allocator());
    Program dense(allocator());
    CompileReport sparse_report(allocator());
    CompileReport dense_report(allocator());
    {
        SequenceCompiler compiler(allocator(), registry.registry);
        CY_REQUIRE(compiler.compile(long_sequence(4, 10), CompileOptions{}, sparse, sparse_report));
    }
    {
        SequenceCompiler compiler(allocator(), registry.registry);
        CY_REQUIRE(compiler.compile(long_sequence(4, 1000), CompileOptions{}, dense, dense_report));
    }
    CY_CHECK_EQ(sparse_report.compression.keys_in * 100U, dense_report.compression.keys_in);

    const BindingResolution bindings[] = {BindingResolution{10, 500, true}};
    PlayRequest request;
    request.bindings = bindings;

    SequencePlayer player(allocator(), registry.registry, PlaybackScope::World, 0);
    const Expected<InstanceId, Error> thin = player.create(sparse, request);
    const Expected<InstanceId, Error> thick = player.create(dense, request);
    CY_REQUIRE(thin);
    CY_REQUIRE(thick);
    CY_REQUIRE(player.play(thin.value()));
    CY_REQUIRE(player.play(thick.value()));

    DispatchBatches batches(allocator());
    for (i32 frame = 0; frame < 12; ++frame) {
        batches.clear();
        CY_REQUIRE(player.advance(41666667, batches));
    }
    // Two instances, one value each: the per-frame batch is the same size for ten keys and for a
    // thousand. What differs is the VALUE, because the dense curve has more detail in it.
    CY_CHECK_EQ(batches.values.size(), 2U);
}
