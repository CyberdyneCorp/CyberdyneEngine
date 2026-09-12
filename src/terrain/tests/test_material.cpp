// Terrain materials: the bounded texel and the report of what the bound dropped, the frequency
// composition, the rules that read environment fields, painting that wins locally, and the page
// producer that evaluates once and invalidates locally. M10 task 2.1.
//
// HOW THIS SUITE WAS SHOWN TO BE ABLE TO FAIL: `MaterialPageCache::invalidate()` had its
// `overlaps()` test replaced by an unconditional drop, and "a local edit re-produces locally" went
// red on `CY_CHECK_EQ(cache.produced_pages(), 8U)` with all nine pages dropped for an edit inside
// one of them. It was then restored. The sequence is reported in this milestone's
// `verified_failing`.

#include <cy/test/test.h>

#include <cy/environment/field.h>
#include <cy/environment/store.h>

#include "fixtures.h"

namespace test = cy::terrain::test;
using namespace cy::terrain;

namespace {

/// A quantised scalar in [0, 1], macro resident everywhere: the shape a moisture field has. It is
/// declared HERE rather than taken from src/environment/'s fixtures because the meaning and the
/// resolution of moisture belong to whichever row produces it, and terrain's suite should not
/// pretend to know them.
[[nodiscard]] cy::environment::FieldDeclaration moisture(const char* name,
                                                         cy::determinism::SimulationClass klass) {
    cy::environment::FieldDeclaration declaration;
    declaration.name = name;
    declaration.unit = "fraction";
    declaration.semantics = "water held in the top soil layer";
    declaration.type = cy::environment::FieldType::Scalar;
    declaration.encoding = cy::environment::FieldEncoding::UNorm8;
    declaration.range_min = 0.0F;
    declaration.range_max = 1.0F;
    declaration.default_value = cy::environment::FieldValue::scalar(0.0F);
    declaration.levels[2] = cy::environment::FieldLevel{16.0F, true};
    declaration.classification = klass;
    declaration.gameplay_level = cy::environment::FieldResidency::Macro;
    return declaration;
}

}  // namespace

CY_TEST_CASE("a texel keeps its dominant layers, and the cooker names what it dropped and where") {
    const TileLayout shape = test::layout();
    LayerCompositor compositor(test::allocator(), shape, kMaxTexelLayers);

    const LayerBlend blends[6] = {{1, 0.30F}, {2, 0.25F}, {3, 0.20F},
                                  {4, 0.15F}, {5, 0.07F}, {6, 0.03F}};
    MaterialTexel texel;
    CY_REQUIRE(
        compositor.compose(TileCoord{2, 3, 0}, 8, 9, cy::Span<const LayerBlend>(blends, 6), texel)
            .has_value());

    CY_CHECK_EQ(texel.used(), 4U);
    CY_CHECK_EQ(texel.dominant(), 1U);
    cy::u32 total = 0;
    for (unsigned char slot : texel.weight) {
        total += slot;
    }
    CY_CHECK_EQ(total, 255U);

    // "cooking SHALL report it and NAME THE LOCATION, rather than silently dropping a layer."
    CY_REQUIRE_EQ(compositor.overflows().size(), 2U);
    const LayerOverflow& first = compositor.overflows()[0];
    CY_CHECK_EQ(first.dropped_layer, 5U);
    CY_CHECK_EQ(first.requested, 6U);
    CY_CHECK_EQ(first.tile.x, 2);
    CY_CHECK_EQ(first.texel_x, 8U);
    const TerrainPoint expected = sample_position(shape, TileCoord{2, 3, 0}, 8, 9);
    CY_CHECK_NEAR(static_cast<cy::f32>(first.world_x), static_cast<cy::f32>(expected.x), 0.001F);
}

CY_TEST_CASE("the per-texel bound is configurable below the storage bound") {
    const TileLayout shape = test::layout();
    LayerCompositor compositor(test::allocator(), shape, 2);
    CY_CHECK_EQ(compositor.bound(), 2U);

    const LayerBlend blends[3] = {{1, 0.5F}, {2, 0.3F}, {3, 0.2F}};
    MaterialTexel texel;
    CY_REQUIRE(
        compositor.compose(TileCoord{0, 0, 0}, 0, 0, cy::Span<const LayerBlend>(blends, 3), texel)
            .has_value());
    CY_CHECK_EQ(texel.used(), 2U);
    CY_CHECK_EQ(compositor.overflows().size(), 1U);

    // Two strokes of one layer are one layer, not two slots: an author painting grass twice has
    // not exceeded a bound of two.
    compositor.clear();
    const LayerBlend repeated[3] = {{1, 0.3F}, {1, 0.3F}, {2, 0.4F}};
    CY_REQUIRE(
        compositor.compose(TileCoord{0, 0, 0}, 0, 0, cy::Span<const LayerBlend>(repeated, 3), texel)
            .has_value());
    CY_CHECK_EQ(texel.used(), 2U);
    CY_CHECK(compositor.overflows().empty());
}

CY_TEST_CASE("frequencies are composed, and which band dominates depends on the footprint") {
    const FrequencyStack stack;
    // The default stack's periods are deliberately incommensurate, so the composition has no period
    // of its own to repeat at. A cooker checks this rather than an artist looking from two
    // distances.
    CY_CHECK(stack.composes_without_repeating());

    FrequencyStack commensurate = stack;
    commensurate.bands[1].metres_per_period = 512.0F;  // exactly a quarter of the macro band
    CY_CHECK_FALSE(commensurate.composes_without_repeating());

    // From the ground, the micro band is in play; from a mountain top its whole period fits inside
    // one pixel's footprint and it contributes its mean instead.
    CY_CHECK_EQ(stack.dominant(0.01F), Frequency::Macro);
    const cy::f32 near_value = stack.compose(1234, 10.0, 20.0, 0.01F);
    const cy::f32 far_value = stack.compose(1234, 10.0, 20.0, 64.0F);
    CY_CHECK_NE(near_value, far_value);
    // And the composition is deterministic: the same seed and position answer the same twice.
    CY_CHECK_EQ(near_value, stack.compose(1234, 10.0, 20.0, 0.01F));
    CY_CHECK_NE(near_value, stack.compose(9999, 10.0, 20.0, 0.01F));
}

CY_TEST_CASE("a rule reads an environment field as a first-class input") {
    cy::environment::FieldRegistry registry(test::allocator());
    const cy::environment::FieldDeclaration declaration =
        moisture("test.terrain.moisture", cy::determinism::SimulationClass::Persistent);
    CY_REQUIRE(registry.declare(declaration).has_value());
    cy::Expected<cy::environment::ProducerToken, cy::Error> token =
        registry.claim(declaration.id(), "test.hydrology", cy::environment::ProducerKind::System);
    CY_REQUIRE(token.has_value());

    cy::environment::FieldStore fields(test::allocator(), registry, test::partition());
    cy::Expected<cy::environment::FieldWriter, cy::Error> writer =
        fields.open_writer(token.value());
    CY_REQUIRE(writer.has_value());
    cy::environment::TileAddress address;
    address.field = declaration.id();
    address.level = static_cast<cy::u8>(cy::environment::FieldResidency::Macro);
    CY_REQUIRE(writer.value().stage(address).has_value());
    CY_REQUIRE(writer.value().fill(address, cy::environment::FieldValue::scalar(0.9F)).has_value());
    CY_REQUIRE(writer.value().publish().has_value());

    MaterialRuleSet rules(test::allocator());
    MaterialRule mud;
    mud.name = "wet ground is mud";
    mud.input = RuleInput::Field;
    mud.field = declaration.id();
    mud.low = 0.6F;
    mud.high = 1.0F;
    mud.layer = 4;
    CY_REQUIRE(rules.add_rule(mud).has_value());

    // The field terrain reads is declared, so a caller can register the consumption once.
    cy::Array<cy::environment::FieldId> read(test::allocator());
    CY_REQUIRE(rules.fields_read(read).has_value());
    CY_REQUIRE_EQ(read.size(), 1U);
    CY_CHECK_EQ(read[0], declaration.id());

    RuleContext context;
    context.x = 10.0;
    context.z = 10.0;
    cy::Array<LayerBlend> blends(test::allocator());
    CY_REQUIRE(rules.evaluate(context, &fields, blends).has_value());
    CY_REQUIRE_EQ(blends.size(), 1U);
    CY_CHECK_EQ(blends[0].layer, 4U);

    // Outside the field's band there is no mud — the rule is a band and not a switch.
    cy::Expected<cy::environment::FieldWriter, cy::Error> second =
        fields.open_writer(token.value());
    CY_REQUIRE(second.has_value());
    CY_REQUIRE(second.value().stage(address).has_value());
    CY_REQUIRE(second.value().fill(address, cy::environment::FieldValue::scalar(0.1F)).has_value());
    CY_REQUIRE(second.value().publish().has_value());
    CY_REQUIRE(rules.evaluate(context, &fields, blends).has_value());
    CY_CHECK(blends.empty());

    // And a rule whose field is unavailable is COUNTED rather than silently contributing.
    CY_REQUIRE(rules.evaluate(context, nullptr, blends).has_value());
    CY_CHECK_EQ(rules.skipped_field_rules(), 1U);
}

CY_TEST_CASE("rules and painting compose, and painting wins locally") {
    MaterialRuleSet rules(test::allocator());
    MaterialRule rock;
    rock.name = "steep slopes are rock";
    rock.input = RuleInput::Slope;
    rock.low = 40.0F;
    rock.high = 90.0F;
    rock.layer = 2;
    CY_REQUIRE(rules.add_rule(rock).has_value());

    PaintStroke exception;
    exception.bounds = TerrainBounds{0.0, 0.0, 10.0, 10.0};
    exception.layer = 8;
    exception.exclusive = true;
    CY_REQUIRE(rules.add_stroke(exception).has_value());

    cy::Array<LayerBlend> blends(test::allocator());
    RuleContext steep;
    steep.slope_degrees = 55.0F;
    steep.x = 500.0;
    steep.z = 500.0;
    CY_REQUIRE(rules.evaluate(steep, nullptr, blends).has_value());
    CY_REQUIRE_EQ(blends.size(), 1U);
    CY_CHECK_EQ(blends[0].layer, 2U);  // the rule applies away from the stroke

    RuleContext painted = steep;
    painted.x = 5.0;
    painted.z = 5.0;
    CY_REQUIRE(rules.evaluate(painted, nullptr, blends).has_value());
    CY_REQUIRE_EQ(blends.size(), 1U);
    CY_CHECK_EQ(blends[0].layer, 8U);  // the painted value wins inside it, and the rule does not
}

namespace {

struct ProducerCount {
    cy::u64 calls = 0;
};

[[nodiscard]] cy::Status counting_producer(void* user, cy::f64 x, cy::f64 z,
                                           MaterialTexel& out) noexcept {
    auto* count = static_cast<ProducerCount*>(user);
    ++count->calls;
    out = MaterialTexel{};
    out.layer[0] = static_cast<cy::u8>((static_cast<cy::i64>(x + z) / 8) & 0x7);
    out.weight[0] = 255;
    return cy::ok();
}

}  // namespace

CY_TEST_CASE("the material graph is evaluated once per page, and sampling it is a fetch") {
    const TileLayout shape = test::layout();
    MaterialPageCache cache(test::allocator(), shape, 64.0F, true);
    ProducerCount count;

    const MaterialPage page = cache.page_at(10.0, 10.0, 0);
    CY_REQUIRE(cache.produce(page, counting_producer, &count).has_value());
    CY_CHECK_EQ(count.calls, kPageTexelCount);
    CY_CHECK_EQ(cache.productions(), kPageTexelCount);

    // Sampling it a thousand times costs no evaluations at all.
    for (cy::u32 index = 0; index < 1000; ++index) {
        CY_REQUIRE(cache.sample(page, 10.0 + static_cast<cy::f64>(index % 50), 10.0) != nullptr);
    }
    CY_CHECK_EQ(count.calls, kPageTexelCount);

    // And producing it again is a no-op rather than a second evaluation.
    CY_REQUIRE(cache.produce(page, counting_producer, &count).has_value());
    CY_CHECK_EQ(count.calls, kPageTexelCount);
}

CY_TEST_CASE("a local edit re-produces locally") {
    const TileLayout shape = test::layout();
    MaterialPageCache cache(test::allocator(), shape, 64.0F, true);
    ProducerCount count;

    for (cy::i32 z = 0; z < 3; ++z) {
        for (cy::i32 x = 0; x < 3; ++x) {
            CY_REQUIRE(cache.produce(MaterialPage{x, z, 0}, counting_producer, &count).has_value());
        }
    }
    CY_REQUIRE_EQ(cache.produced_pages(), 9U);
    const cy::u64 evaluated = count.calls;

    // A crater two metres across, inside the middle page. "only the pages covering it SHALL be
    // invalidated and re-produced."
    const cy::u32 dropped = cache.invalidate(TerrainBounds{80.0, 80.0, 82.0, 82.0});
    CY_CHECK_EQ(dropped, 1U);
    CY_CHECK_EQ(cache.produced_pages(), 8U);
    CY_CHECK_FALSE(cache.is_produced(MaterialPage{1, 1, 0}));
    CY_CHECK(cache.is_produced(MaterialPage{0, 0, 0}));
    CY_CHECK(cache.is_produced(MaterialPage{2, 2, 0}));

    // Re-producing costs one page, not nine.
    CY_REQUIRE(cache.produce(MaterialPage{1, 1, 0}, counting_producer, &count).has_value());
    CY_CHECK_EQ(count.calls, evaluated + kPageTexelCount);
}

CY_TEST_CASE("without virtual texturing the path degrades and says what it cost") {
    const TileLayout shape = test::layout();
    MaterialPageCache available(test::allocator(), shape, 64.0F, true);
    CY_CHECK(available.path().virtual_texturing);
    CY_CHECK_EQ(available.path().unique_detail, 1.0F);

    MaterialPageCache degraded(test::allocator(), shape, 64.0F, false);
    const MaterialPathReport report = degraded.path();
    CY_CHECK_FALSE(report.virtual_texturing);
    CY_CHECK_LT(report.unique_detail, 1.0F);
    CY_REQUIRE(report.limitation != nullptr);
    CY_CHECK_NE(report.limitation[0], '\0');
}
