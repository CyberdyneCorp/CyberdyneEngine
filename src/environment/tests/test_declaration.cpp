// Field declaration and the rules it is held to. Task 1.1.

#include <cy/test/test.h>

#include <cy/environment/field.h>

#include "fixtures.h"

using cy::environment::DeclarationProblem;
using cy::environment::FieldDeclaration;
using cy::environment::FieldEncoding;
using cy::environment::FieldInterpolation;
using cy::environment::FieldLevel;
using cy::environment::FieldRegistry;
using cy::environment::FieldResidency;
using cy::environment::FieldType;
using cy::environment::validate_declaration;
namespace test = cy::environment::test;

CY_TEST_CASE("a field's identity is its name, and is the same number everywhere forever") {
    // "An identity and not an index": two systems that independently name one field get one field.
    CY_CHECK_EQ(cy::environment::field_id("moisture").value,
                cy::environment::field_id("moisture").value);
    CY_CHECK_NE(cy::environment::field_id("moisture").value,
                cy::environment::field_id("wetness").value);
    // Resolved at compile time where the name is a literal, which is what makes a field identity
    // usable as a constant in a producer's own translation unit.
    static_assert(cy::environment::field_id("moisture").value != 0);
    static_assert(cy::environment::field_id("moisture").value !=
                  cy::environment::field_id("temperature").value);

    // A field and a producer of the same name are not one number. Nothing compares them, which is
    // exactly why they must not collide in a diagnostic that prints both.
    CY_CHECK_NE(cy::environment::field_id("wind").value,
                cy::environment::producer_id("wind").value);
}

CY_TEST_CASE(
    "a declaration states its meaning, and the substrate refuses the ones that cannot mean "
    "anything") {
    CY_CHECK(validate_declaration(test::moisture_like()) == DeclarationProblem::None);
    CY_CHECK(validate_declaration(test::wind_like()) == DeclarationProblem::None);
    CY_CHECK(validate_declaration(test::biome_like()) == DeclarationProblem::None);

    FieldDeclaration nameless = test::moisture_like();
    nameless.name = "";
    CY_CHECK(validate_declaration(nameless) == DeclarationProblem::NoName);

    FieldDeclaration levelless = test::moisture_like();
    levelless.levels[0] = FieldLevel{};
    levelless.levels[1] = FieldLevel{};
    levelless.levels[2] = FieldLevel{};
    CY_CHECK(validate_declaration(levelless) == DeclarationProblem::NoLevel);

    // Levels must coarsen outward, or "the finest resident level" would be a property of the
    // enumeration's order rather than of the resolutions.
    FieldDeclaration inverted = test::moisture_like();
    inverted.levels[0] = FieldLevel{64.0F, false};
    inverted.levels[2] = FieldLevel{2.0F, true};
    CY_CHECK(validate_declaration(inverted) == DeclarationProblem::LevelsNotOrdered);

    // An averaged biome index names nothing, so the pairing is refused rather than silently
    // blended.
    FieldDeclaration blended = test::biome_like();
    blended.interpolation = FieldInterpolation::Linear;
    CY_CHECK(validate_declaration(blended) == DeclarationProblem::CategoryInterpolated);

    FieldDeclaration mismatched = test::moisture_like();
    mismatched.encoding = FieldEncoding::Uint8;
    CY_CHECK(validate_declaration(mismatched) == DeclarationProblem::EncodingMismatch);

    FieldDeclaration flat = test::moisture_like();
    flat.range_max = flat.range_min;
    CY_CHECK(validate_declaration(flat) == DeclarationProblem::EmptyRange);

    FieldDeclaration thin = test::wind_like();
    thin.vertical_cells = 0;
    CY_CHECK(validate_declaration(thin) == DeclarationProblem::NoVerticalCells);
}

CY_TEST_CASE("a project declares a field the engine has never heard of, and nothing else changes") {
    // "WHEN a project declares a `radiation` field, THEN it SHALL stream, sample, and debug like a
    // standard field with no engine change." Nothing in src/environment/ names this field.
    FieldRegistry registry(test::allocator());
    const FieldDeclaration radiation = test::project_radiation();
    CY_REQUIRE(registry.declare(radiation).has_value());

    const FieldDeclaration* stored = registry.declaration(radiation.id());
    CY_REQUIRE(stored != nullptr);
    CY_CHECK(stored->encoding == FieldEncoding::UNorm16);
    CY_CHECK(stored->type == FieldType::Scalar);
    // "Its meaning, unit, and range SHALL be defined by the field declaration, not inferred from
    // the producer's implementation." The registry keeps all three, and a consumer reads them
    // without knowing who produces the field.
    CY_CHECK(test::same_text(stored->unit, "sieverts/hour"));
    CY_CHECK(test::same_text(stored->semantics, "absorbed dose rate at ground level"));
    CY_CHECK_EQ(stored->range_max, 10.0F);
    CY_CHECK_EQ(registry.size(), 1u);
}

CY_TEST_CASE("an identical re-declaration is accepted and a different one is refused") {
    FieldRegistry registry(test::allocator());
    CY_REQUIRE(registry.declare(test::moisture_like()).has_value());
    // Two modules that both need a field to exist should not have to agree on which declares it.
    CY_CHECK(registry.declare(test::moisture_like()).has_value());
    CY_CHECK_EQ(registry.size(), 1u);

    // Widening the range changes what every already-stored byte means, so it is refused rather than
    // applied to a store that is already holding tiles under the old mapping.
    FieldDeclaration widened = test::moisture_like();
    widened.range_max = 2.0F;
    const cy::Status refused = registry.declare(widened);
    CY_REQUIRE_FALSE(refused.has_value());
    CY_CHECK(refused.error().code == cy::ErrorCode::AlreadyExists);
}

CY_TEST_CASE("the encoding decides the precision two readers are held to") {
    // A quantised field cannot represent a difference finer than one step of its own storage, and
    // claiming a tighter agreement than that would be a claim about the arithmetic rather than the
    // field. `test_gpu.cpp` is what holds the two readers to this number.
    const FieldDeclaration moisture = test::moisture_like();
    CY_CHECK_NEAR(moisture.resolved_precision(), 1.0F / 255.0F, 1.0e-6F);

    const FieldDeclaration radiation = test::project_radiation();
    CY_CHECK_NEAR(radiation.resolved_precision(), 10.0F / 65535.0F, 1.0e-6F);

    // Integers are exact on both sides: nearest sampling reads one stored number and neither reader
    // does arithmetic on it.
    CY_CHECK_EQ(test::biome_like().resolved_precision(), 0.0F);

    FieldDeclaration declared = test::moisture_like();
    declared.precision = 0.5F;
    CY_CHECK_EQ(declared.resolved_precision(), 0.5F);
}

CY_TEST_CASE("the layer rule is declared, not emergent, and every reader applies the same one") {
    using cy::environment::combine_layers;
    using cy::environment::FieldLayerRule;
    const cy::environment::FieldValue base = cy::environment::FieldValue::scalar(0.25F);
    const cy::environment::FieldValue delta = cy::environment::FieldValue::scalar(0.5F);

    CY_CHECK_EQ(combine_layers(FieldLayerRule::Replace, base, delta, 1).x(), 0.5F);
    CY_CHECK_EQ(combine_layers(FieldLayerRule::Add, base, delta, 1).x(), 0.75F);
    CY_CHECK_EQ(combine_layers(FieldLayerRule::Multiply, base, delta, 1).x(), 0.125F);
    CY_CHECK_EQ(combine_layers(FieldLayerRule::Max, base, delta, 1).x(), 0.5F);
    CY_CHECK_EQ(combine_layers(FieldLayerRule::Min, base, delta, 1).x(), 0.25F);

    // Only the declared components are combined: a scalar field's unused components are not
    // arithmetic anyone should be able to observe.
    const cy::environment::FieldValue wide =
        combine_layers(FieldLayerRule::Add, cy::environment::FieldValue::vec3(1.0F, 2.0F, 3.0F),
                       cy::environment::FieldValue::vec3(1.0F, 1.0F, 1.0F), 2);
    CY_CHECK_EQ(wide.x(), 2.0F);
    CY_CHECK_EQ(wide.y(), 3.0F);
    CY_CHECK_EQ(wide.z(), 3.0F);
}
