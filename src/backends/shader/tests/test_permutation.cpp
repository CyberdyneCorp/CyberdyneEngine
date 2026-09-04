// Permutation axes, the key, and the budget report. Task 3.2.
//
// The case that matters most is "specialization costs no compilation": `shader-system` orders the
// three mechanisms and the ordering is only real if the engine can *count* the difference.

#include <cy/backends/shader/permutation.h>
#include <cy/core/memory/scope.h>
#include <cy/test/test.h>

using cy::u32;
using cy::u64;
using cy::usize;
using namespace cy::shader;

namespace {

constexpr u32 kBinary[] = {0, 1};
constexpr u32 kQuality[] = {1, 2, 4, 8};

}  // namespace

CY_TEST_CASE("an axis declares its allowed values, and one value is not an axis") {
    PermutationSet set(cy::current_allocator());
    constexpr u32 single[] = {1};
    CY_CHECK_FALSE(set.add_axis(cy::Name::intern("ONE"), VariationKind::Preprocessor, {single, 1})
                       .has_value());
    CY_CHECK_FALSE(set.add_axis(cy::Name{}, VariationKind::Preprocessor, {kBinary, 2}).has_value());
    CY_REQUIRE(set.add_axis(cy::Name::intern("FOG"), VariationKind::Preprocessor, {kBinary, 2})
                   .has_value());
    // Declared twice is a mistake, not a redefinition.
    CY_CHECK_FALSE(set.add_axis(cy::Name::intern("FOG"), VariationKind::Preprocessor, {kBinary, 2})
                       .has_value());
    CY_CHECK_EQ(set.axis_count(), usize{1});
    CY_CHECK_EQ(set.axis_at(0).values.size(), usize{2});
}

CY_TEST_CASE("specialization multiplies pipelines and not compilations") {
    PermutationSet set(cy::current_allocator());
    CY_REQUIRE(set.add_axis(cy::Name::intern("FOG"), VariationKind::Preprocessor, {kBinary, 2})
                   .has_value());
    CY_REQUIRE(set.add_axis(cy::Name::intern("SHADOW_SAMPLES"), VariationKind::Specialization,
                            {kQuality, 4}, 7)
                   .has_value());
    CY_REQUIRE(set.add_axis(cy::Name::intern("SKINNED"), VariationKind::Generic, {kBinary, 2})
                   .has_value());

    // 2 * 4 * 2 pipelines, but only 2 * 2 compilations: the specialization axis costs none.
    CY_CHECK_EQ(set.pipeline_variants(), u64{16});
    CY_CHECK_EQ(set.compiled_variants(), u64{4});
}

CY_TEST_CASE("the key is dense, reversible and stable") {
    PermutationSet set(cy::current_allocator());
    CY_REQUIRE(set.add_axis(cy::Name::intern("FOG"), VariationKind::Preprocessor, {kBinary, 2})
                   .has_value());
    CY_REQUIRE(set.add_axis(cy::Name::intern("SHADOW_SAMPLES"), VariationKind::Specialization,
                            {kQuality, 4}, 7)
                   .has_value());

    // A bitmask rather than an eight-element array: the analyser cannot see that `key->value < 8`
    // bounds an index, and a shift by a value the same guard bounds is provably in range.
    u32 seen = 0;
    for (u32 fog = 0; fog < 2; ++fog) {
        for (u32 quality = 0; quality < 4; ++quality) {
            const u32 choice[] = {fog, quality};
            auto key = set.encode({choice, 2});
            CY_REQUIRE(key.has_value());
            CY_REQUIRE(key->value < 8);
            const u32 bit = 1U << (key->value & 7U);
            CY_CHECK_EQ(seen & bit, 0U);  // dense: every key is hit exactly once
            seen |= bit;

            u32 decoded[2] = {};
            CY_REQUIRE(set.decode(*key, {decoded, 2}).has_value());
            CY_CHECK_EQ(decoded[0], fog);
            CY_CHECK_EQ(decoded[1], quality);
        }
    }

    const u32 undeclared[] = {0, 9};
    CY_CHECK_FALSE(set.encode({undeclared, 2}).has_value());
    const u32 wrong_arity[] = {0};
    CY_CHECK_FALSE(set.encode({wrong_arity, 1}).has_value());
}

CY_TEST_CASE("two variants differing only in a specialization share one compilation key") {
    PermutationSet set(cy::current_allocator());
    CY_REQUIRE(set.add_axis(cy::Name::intern("FOG"), VariationKind::Preprocessor, {kBinary, 2})
                   .has_value());
    CY_REQUIRE(set.add_axis(cy::Name::intern("SHADOW_SAMPLES"), VariationKind::Specialization,
                            {kQuality, 4}, 7)
                   .has_value());

    const u32 low[] = {1, 0};
    const u32 high[] = {1, 3};
    auto low_key = set.encode({low, 2});
    auto high_key = set.encode({high, 2});
    CY_REQUIRE(low_key.has_value());
    CY_REQUIRE(high_key.has_value());
    CY_CHECK(*low_key != *high_key);

    // Same SPIR-V, two pipelines. This is the whole of "a quality change creates a new pipeline
    // from the same module rather than a new compilation".
    auto low_compilation = set.compilation_key(*low_key);
    auto high_compilation = set.compilation_key(*high_key);
    CY_REQUIRE(low_compilation.has_value());
    CY_REQUIRE(high_compilation.has_value());
    CY_CHECK(*low_compilation == *high_compilation);

    cy::Array<cy::rhi::SpecializationConstant> constants(cy::current_allocator());
    CY_REQUIRE(set.specialization_constants(*high_key, constants).has_value());
    CY_REQUIRE_EQ(constants.size(), usize{1});
    CY_CHECK_EQ(constants[0].id, 7U);
    CY_CHECK_EQ(constants[0].value, 8U);  // the declared value, not the index
}

CY_TEST_CASE("only preprocessor axes become defines") {
    PermutationSet set(cy::current_allocator());
    CY_REQUIRE(set.add_axis(cy::Name::intern("FOG"), VariationKind::Preprocessor, {kBinary, 2})
                   .has_value());
    CY_REQUIRE(set.add_axis(cy::Name::intern("SHADOW_SAMPLES"), VariationKind::Specialization,
                            {kQuality, 4}, 7)
                   .has_value());

    const u32 choice[] = {1, 2};
    auto key = set.encode({choice, 2});
    CY_REQUIRE(key.has_value());

    cy::Array<cy::Name> names(cy::current_allocator());
    cy::Array<u32> values(cy::current_allocator());
    CY_REQUIRE(set.preprocessor_defines(*key, names, values).has_value());
    CY_REQUIRE_EQ(names.size(), usize{1});
    CY_CHECK(names[0] == cy::Name::intern("FOG"));
    CY_CHECK_EQ(values[0], 1U);
}

CY_TEST_CASE("a permutation explosion is a warning with the axis breakdown") {
    PermutationSet set(cy::current_allocator());
    for (u32 index = 0; index < 6; ++index) {
        char name[16] = {};
        name[0] = 'A';
        name[1] = static_cast<char>('0' + index);
        CY_REQUIRE(set.add_axis(cy::Name::intern(name), VariationKind::Preprocessor, {kQuality, 4})
                       .has_value());
    }
    CY_CHECK_EQ(set.compiled_variants(), u64{4096});

    DiagnosticLog diagnostics(cy::current_allocator());
    CY_CHECK_FALSE(set.check_budget(kDefaultPermutationBudget, diagnostics, "expensive"));
    // A warning, not an error: a project may knowingly exceed the budget, and failing the build
    // would only teach people to raise the number.
    CY_CHECK_EQ(diagnostics.error_count(), 0U);
    CY_CHECK_EQ(diagnostics.warning_count(), 1U);
    // One note per axis: "too many variants" is not actionable, the breakdown is.
    CY_CHECK_EQ(diagnostics.size(), usize{7});

    DiagnosticLog quiet(cy::current_allocator());
    CY_CHECK(set.check_budget(u64{100000}, quiet, "expensive"));
    CY_CHECK(quiet.empty());
}

CY_TEST_CASE("a specialization axis whose constant the module does not declare is an error") {
    PermutationSet set(cy::current_allocator());
    CY_REQUIRE(set.add_axis(cy::Name::intern("SHADOW_SAMPLES"), VariationKind::Specialization,
                            {kQuality, 4}, 7)
                   .has_value());
    // Two axes cannot claim one constant either.
    CY_CHECK_FALSE(
        set.add_axis(cy::Name::intern("OTHER"), VariationKind::Specialization, {kQuality, 4}, 7)
            .has_value());

    DiagnosticLog agreeing(cy::current_allocator());
    const u32 declared[] = {7};
    CY_CHECK(set.validate_specialization_ids({declared, 1}, agreeing, "probe"));

    DiagnosticLog disagreeing(cy::current_allocator());
    const u32 elsewhere[] = {3};
    // Without this check the axis silently does nothing, which looks exactly like a shader that
    // ignores a quality setting.
    CY_CHECK_FALSE(set.validate_specialization_ids({elsewhere, 1}, disagreeing, "probe"));
    CY_CHECK_EQ(disagreeing.error_count(), 1U);
}
