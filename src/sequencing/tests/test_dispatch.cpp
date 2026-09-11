// Batches, their stable order, and arbitration.
//
// The case that matters most here is the last one: the same contributions offered in a DIFFERENT
// ORDER produce an identical batch. That is the only way to demonstrate that the order came from
// the identities rather than from the traversal, and it is what
// `sequencing-and-cinematics`'s "never from worker identity" asks for.

#include "fixture.h"

#include <cy/sequencing/dispatch.h>
#include <cy/test/test.h>

using namespace cy;
using namespace cy::sequencing;
using namespace cy::sequencing::testing;

namespace {

[[nodiscard]] ValueRequest value_of(f32 value, i32 priority, f32 weight, u32 instance, u32 track,
                                    BlendMode blend = BlendMode::Absolute) noexcept {
    ValueRequest request;
    request.subsystem = SubsystemId::Light;
    request.target = 77;
    request.property = ResolvedProperty{1, kIntensity};
    request.value = ChannelValue::scalar(value);
    request.priority = priority;
    request.weight = weight;
    request.blend = blend;
    request.provenance.instance = instance;
    request.provenance.track_stable_id = track;
    return request;
}

}  // namespace

CY_TEST_CASE("sequence_dispatch: one target, one property, one resolved value") {
    DispatchBatches batches(allocator());
    (void)batches.values.push_back(value_of(0.0F, 0, 1.0F, 1, 10));
    (void)batches.values.push_back(value_of(1.0F, 100, 1.0F, 2, 20));

    ArbitrationReport report(allocator());
    CY_REQUIRE(arbitrate(batches, report));
    CY_REQUIRE_EQ(report.values.size(), 1U);
    // The higher priority wins at full weight, and the lower one is still ATTRIBUTABLE.
    CY_CHECK_NEAR(report.values[0].value.components[0], 1.0F, 1e-5F);
    CY_CHECK_EQ(report.values[0].contribution_count, 2U);
    CY_CHECK_EQ(report.contributions[0].provenance.instance, 1U);
    CY_CHECK_EQ(report.contributions[1].provenance.instance, 2U);
}

CY_TEST_CASE("sequence_dispatch: a half-weight cinematic is half of the result") {
    DispatchBatches batches(allocator());
    (void)batches.values.push_back(value_of(0.0F, 0, 1.0F, 1, 10));
    (void)batches.values.push_back(value_of(1.0F, 100, 0.5F, 2, 20));

    ArbitrationReport report(allocator());
    CY_REQUIRE(arbitrate(batches, report));
    CY_REQUIRE_EQ(report.values.size(), 1U);
    CY_CHECK_NEAR(report.values[0].value.components[0], 0.5F, 1e-5F);
}

CY_TEST_CASE("sequence_dispatch: two sequences at one priority are normalised, not applied twice") {
    // "**WHEN** a value is driven by two sequences **THEN** the debugger SHALL show each
    // contribution and its weight."
    DispatchBatches batches(allocator());
    (void)batches.values.push_back(value_of(0.0F, 0, 1.0F, 1, 10));
    (void)batches.values.push_back(value_of(1.0F, 50, 1.0F, 2, 20));
    (void)batches.values.push_back(value_of(0.0F, 50, 1.0F, 3, 30));

    ArbitrationReport report(allocator());
    CY_REQUIRE(arbitrate(batches, report));
    CY_REQUIRE_EQ(report.values.size(), 1U);
    CY_CHECK_NEAR(report.values[0].value.components[0], 0.5F, 1e-5F);

    CY_REQUIRE_EQ(report.contributions.size(), 3U);
    for (const Contribution& contribution : report.contributions.span()) {
        if (contribution.priority == 50) {
            CY_CHECK_NEAR(contribution.effective_weight, 0.5F, 1e-5F);
            CY_CHECK_NEAR(contribution.requested_weight, 1.0F, 1e-5F);
        }
    }
}

CY_TEST_CASE("sequence_dispatch: an additive contribution adds over what is below") {
    DispatchBatches batches(allocator());
    (void)batches.values.push_back(value_of(1.0F, 0, 1.0F, 1, 10));
    (void)batches.values.push_back(value_of(0.25F, 10, 1.0F, 2, 20, BlendMode::Additive));

    ArbitrationReport report(allocator());
    CY_REQUIRE(arbitrate(batches, report));
    CY_REQUIRE_EQ(report.values.size(), 1U);
    CY_CHECK_NEAR(report.values[0].value.components[0], 1.25F, 1e-5F);
}

CY_TEST_CASE("sequence_dispatch: two properties of one target are two resolved values") {
    DispatchBatches batches(allocator());
    ValueRequest colour = value_of(0.5F, 0, 1.0F, 1, 10);
    colour.property = ResolvedProperty{1, kColor};
    (void)batches.values.push_back(value_of(1.0F, 0, 1.0F, 1, 10));
    (void)batches.values.push_back(colour);

    ArbitrationReport report(allocator());
    CY_REQUIRE(arbitrate(batches, report));
    CY_CHECK_EQ(report.values.size(), 2U);
}

CY_TEST_CASE("sequence_dispatch: a boss cinematic outranks the gameplay camera") {
    // "**WHEN** a high-priority sequence takes the camera **THEN** the camera system SHALL receive
    // one resolved result, and the gameplay camera's contribution SHALL be blended or suspended as
    // declared."
    DispatchBatches batches(allocator());
    CameraRequest gameplay;
    gameplay.binding = 11;
    gameplay.rig = 500;
    gameplay.priority = 0;
    gameplay.provenance.instance = 1;
    CameraRequest cinematic;
    cinematic.binding = 11;
    cinematic.rig = 501;
    cinematic.priority = 100;
    cinematic.provenance.instance = 2;
    (void)batches.cameras.push_back(gameplay);
    (void)batches.cameras.push_back(cinematic);

    ArbitrationReport report(allocator());
    CY_REQUIRE(arbitrate(batches, report));
    CY_REQUIRE_EQ(report.cameras.size(), 2U);
    u32 winners = 0;
    u32 displaced = 0;
    for (const CameraRequest& request : report.cameras.span()) {
        if (request.release) {
            ++displaced;
            CY_CHECK_EQ(request.rig, 500U);  // blended out as declared, not deleted
        } else {
            ++winners;
            CY_CHECK_EQ(request.rig, 501U);
        }
    }
    CY_CHECK_EQ(winners, 1U);
    CY_CHECK_EQ(displaced, 1U);
}

CY_TEST_CASE("sequence_dispatch: an exclusive group displaces across bindings") {
    DispatchBatches batches(allocator());
    CameraRequest first;
    first.binding = 11;
    first.rig = 500;
    first.priority = 10;
    first.exclusive_group = Name::intern("cinematic");
    first.provenance.instance = 1;
    CameraRequest second;
    second.binding = 12;
    second.rig = 501;
    second.priority = 20;
    second.exclusive_group = Name::intern("cinematic");
    second.provenance.instance = 2;
    (void)batches.cameras.push_back(first);
    (void)batches.cameras.push_back(second);

    ArbitrationReport report(allocator());
    CY_REQUIRE(arbitrate(batches, report));
    u32 released = 0;
    for (const CameraRequest& request : report.cameras.span()) {
        released += request.release ? 1U : 0U;
    }
    CY_CHECK_EQ(released, 1U);
}

CY_TEST_CASE("sequence_dispatch: the order comes from identity, not from the traversal") {
    // The requirement's own words: "results merged in a **stable order** derived from sequence
    // instance, track, section, and event index — never from worker identity."
    DispatchBatches forward(allocator());
    DispatchBatches shuffled(allocator());
    for (u32 index = 0; index < 8; ++index) {
        (void)forward.values.push_back(
            value_of(static_cast<f32>(index), 0, 1.0F, (index % 3) + 1, index + 1));
    }
    for (u32 index = 8; index > 0; --index) {
        (void)shuffled.values.push_back(
            value_of(static_cast<f32>(index - 1), 0, 1.0F, ((index - 1) % 3) + 1, index));
    }
    forward.sort();
    shuffled.sort();

    CY_REQUIRE_EQ(forward.values.size(), shuffled.values.size());
    for (usize index = 0; index < forward.values.size(); ++index) {
        CY_CHECK_EQ(forward.values[index].provenance.instance,
                    shuffled.values[index].provenance.instance);
        CY_CHECK_EQ(forward.values[index].provenance.track_stable_id,
                    shuffled.values[index].provenance.track_stable_id);
        CY_CHECK_EQ(forward.values[index].value.components[0],
                    shuffled.values[index].value.components[0]);
    }

    // And arbitration over the two orders produces the same resolved value.
    ArbitrationReport a(allocator());
    ArbitrationReport b(allocator());
    CY_REQUIRE(arbitrate(forward, a));
    CY_REQUIRE(arbitrate(shuffled, b));
    CY_REQUIRE_EQ(a.values.size(), b.values.size());
    CY_CHECK_EQ(a.values[0].value.components[0], b.values[0].value.components[0]);
}

CY_TEST_CASE("sequence_dispatch: a subsystem's values are a range rather than a filter") {
    DispatchBatches batches(allocator());
    ValueRequest animation = value_of(1.0F, 0, 1.0F, 1, 10);
    animation.subsystem = SubsystemId::Animation;
    (void)batches.values.push_back(value_of(1.0F, 0, 1.0F, 1, 11));
    (void)batches.values.push_back(animation);
    (void)batches.values.push_back(value_of(2.0F, 0, 1.0F, 1, 12));
    batches.sort();

    CY_CHECK_EQ(batches.values_for(SubsystemId::Light).size(), 2U);
    CY_CHECK_EQ(batches.values_for(SubsystemId::Animation).size(), 1U);
    CY_CHECK_EQ(batches.values_for(SubsystemId::Audio).size(), 0U);
}
