// Closure lowering, the program family, quality tiers, cost and binning. M7 tasks 6.2 and 6.3.

#include <cy/core/memory/system_allocator.h>
#include <cy/rendering/material/binning.h>
#include <cy/rendering/material/compiler.h>
#include <cy/test/test.h>

#include <algorithm>
#include <cstring>
#include <utility>

#include "fixtures.h"

using namespace cy;
using namespace cy::rendering::material;
using cy::rendering::BlendMode;
using cy::rendering::MaterialProgram;
using cy::rendering::MaterialTable;
using cy::rendering::parameter_id;
using cy::rendering::ParameterId;
using cy::rendering::material::testing::build_reference_graph;
using cy::rendering::material::testing::GraphIds;
using cy::rendering::material::testing::reference_text;

namespace {

Allocator& allocator() noexcept {
    return system_allocator(MemoryDomain::Renderer);
}

[[nodiscard]] Expected<Module, Error> from_text(std::string_view source) noexcept {
    ParseDiagnostic diagnostic(allocator());
    return parse_material(source, allocator(), diagnostic);
}

[[nodiscard]] bool has_diagnostic(const CompiledMaterial& material, const char* code) noexcept {
    return std::ranges::any_of(material.diagnostics(),
                               [code](const CompileDiagnostic& diagnostic) noexcept {
                                   return std::strcmp(diagnostic.code, code) == 0;
                               });
}

/// A layered car paint: coat over diffuse, specular and sheen. The spike's own generic-evaluator
/// case (design.md §1.5).
constexpr const char* kCarPaint = R"(
material car_paint {
    param base   : float3 = (0.1, 0.02, 0.02);
    param flake  : float3 = (0.6, 0.6, 0.7);
    param sheen_colour : float3 = (0.3, 0.3, 0.3);
    param rough  : float = 0.2;
    surface = layer(coat(0.05), diffuse(base) + specular(flake, rough) + sheen(sheen_colour));
    opacity = 1.0;
}
)";

/// Alpha-tested foliage: opacity is a texture, so a shadow program exists and carries only it.
constexpr const char* kFoliage = R"(
material foliage {
    param tint : float3 = (0.2, 0.4, 0.1);
    param rough : float = 0.6;
    param alpha_cutoff : float = 0.5;
    texture leaf_mask average (1.0, 1.0, 1.0, 1.0) shadow_critical;
    texture leaf_colour average (0.2, 0.4, 0.1, 1.0);
    attribute uv0 : float2;

    let mask = sample(leaf_mask, uv0);
    surface = diffuse(sample(leaf_colour, uv0).xyz * tint) + specular(tint, rough);
    opacity = mask.x;
}
)";

/// Microdetail that does NOT reach albedo: the derivation drops it and nothing is flagged.
constexpr const char* kHonestDetail = R"(
material honest_detail {
    param tint : float3 = (0.7, 0.7, 0.7);
    texture base_map average (0.5, 0.5, 0.5, 1.0);
    texture detail_rough average (0.4, 0.4, 0.4, 1.0);
    attribute uv0 : float2;

    let base = sample(base_map, uv0).xyz * tint;
    @microdetail let detail = sample(detail_rough, uv0);
    surface = diffuse(base) + specular(base, detail.x);
    opacity = 1.0;
}
)";

/// Two structural switches, so the permutation count is the specification's own example.
constexpr const char* kTwoSwitches = R"(
material switched {
    param has_coat : bool = true;
    param has_sheen : bool = true;
    param base : float3 = (0.5, 0.5, 0.5);
    param rough : float = 0.3;
    let coat_weight  = select(has_coat, 1.0, 0.0);
    let sheen_weight = select(has_sheen, 0.5, 0.0);
    surface = diffuse(base) * coat_weight + specular(base, rough) * sheen_weight;
    opacity = 1.0;
}
)";

}  // namespace

CY_TEST_CASE("material_lowering: a matched closure set costs what its shading model costs") {
    auto module = from_text(reference_text());
    CY_REQUIRE(module.has_value());
    OptimiseReport report(allocator());
    auto optimised = optimise(module.value(), PassSwitches{}, report);
    CY_REQUIRE(optimised.has_value());

    const ClosureSet closures = closure_set(optimised.value());
    CY_CHECK(closures.has(Op::Diffuse));
    CY_CHECK(closures.has(Op::Specular));
    CY_CHECK_FALSE(closures.has(Op::Emission));
    CY_CHECK_EQ(closures.count(), 2U);

    const Lowered lowered = match_shading_model(closures);
    CY_CHECK_EQ(lowered.model, cy::render::ShadingModel::Lit);
    CY_CHECK_FALSE(lowered.generic_evaluator);
    CY_CHECK_EQ(lowered.generic_cost_multiple, 1.0F);
}

CY_TEST_CASE("material_lowering: generality is priced, not hidden") {
    auto module = from_text(kCarPaint);
    CY_REQUIRE(module.has_value());
    OptimiseReport report(allocator());
    auto optimised = optimise(module.value(), PassSwitches{}, report);
    CY_REQUIRE(optimised.has_value());

    const Lowered lowered = match_shading_model(closure_set(optimised.value()));
    CY_CHECK(lowered.generic_evaluator);
    CY_CHECK_GT(lowered.generic_cost_multiple, 1.0F);

    // On a profile with no generic evaluator, cooking fails naming the closures responsible.
    CompileOptions options;
    options.profile = mobile_profile();
    options.derive_family = false;
    options.derive_tiers = false;
    auto compiled = compile_material(optimised.value(), options, allocator());
    CY_REQUIRE(compiled.has_value());
    CY_CHECK(compiled.value().failed());
    CY_CHECK(has_diagnostic(compiled.value(), "generic-evaluator-unsupported"));
}

CY_TEST_CASE("material_lowering: an opaque material has no shadow program at all") {
    auto module = from_text(reference_text());
    CY_REQUIRE(module.has_value());
    CompileOptions options;
    auto compiled = compile_material(module.value(), options, allocator());
    CY_REQUIRE(compiled.has_value());

    // Four programs times three tiers.
    CY_CHECK_EQ(compiled.value().programs().size(), 12U);

    const CompiledProgram* shadow = compiled.value().find(ProgramKind::Shadow, QualityTier::High);
    CY_REQUIRE(shadow != nullptr);
    CY_CHECK(shadow->absent);
    CY_CHECK_EQ(shadow->source.statements, 0U);
    CY_CHECK(shadow->difference.empty_program);

    const CompiledProgram* primary = compiled.value().find(ProgramKind::Primary, QualityTier::High);
    CY_REQUIRE(primary != nullptr);
    CY_CHECK_GT(primary->source.statements, 0U);
    CY_CHECK_GT(primary->cost.texture_samples, 0U);

    // The far-field program samples no texture: it is constants and averages.
    const CompiledProgram* far = compiled.value().find(ProgramKind::FarField, QualityTier::High);
    CY_REQUIRE(far != nullptr);
    CY_CHECK_EQ(far->cost.texture_samples, 0U);
    CY_CHECK_LT(far->cost.full_screen_ms, primary->cost.full_screen_ms);
}

CY_TEST_CASE("material_lowering: an alpha-tested material's shadow program carries only opacity") {
    auto module = from_text(kFoliage);
    CY_REQUIRE(module.has_value());
    CompileOptions options;
    auto compiled = compile_material(module.value(), options, allocator());
    CY_REQUIRE(compiled.has_value());

    const CompiledProgram* shadow = compiled.value().find(ProgramKind::Shadow, QualityTier::High);
    CY_REQUIRE(shadow != nullptr);
    CY_CHECK_FALSE(shadow->absent);
    CY_CHECK_EQ(shadow->cost.closures, 0U);
    CY_CHECK_EQ(shadow->cost.texture_samples, 1U);
    CY_CHECK_EQ(shadow->module.surface(), kInvalidNode);
    CY_CHECK_NE(shadow->module.opacity(), kInvalidNode);

    // Every texture the shadow program can sample is marked shadow-critical, so `residency` keeps
    // a coarse level of it resident and shadow rasterisation never waits on a texture.
    for (const TextureDecl& texture : shadow->module.textures()) {
        CY_CHECK(texture.shadow_critical);
    }
}

CY_TEST_CASE(
    "material_lowering: a derivation that changes albedo is flagged, and one that does not is "
    "not") {
    // design.md §1.5: this is a REACHABILITY question. The reference material's microdetail `grime`
    // also feeds albedo, so dropping it changes the material's base colour and the cook report says
    // so; `honest_detail`'s microdetail only reaches roughness and is dropped silently.
    auto flagged = from_text(reference_text());
    CY_REQUIRE(flagged.has_value());
    CompileOptions options;
    auto compiled = compile_material(flagged.value(), options, allocator());
    CY_REQUIRE(compiled.has_value());
    const CompiledProgram* secondary =
        compiled.value().find(ProgramKind::Secondary, QualityTier::High);
    CY_REQUIRE(secondary != nullptr);
    CY_CHECK(secondary->difference.albedo_changed);
    CY_CHECK(secondary->difference.responsible == Name::intern("grime_map"));
    CY_CHECK(has_diagnostic(compiled.value(), "derivation-changes-albedo"));

    auto honest = from_text(kHonestDetail);
    CY_REQUIRE(honest.has_value());
    auto quiet = compile_material(honest.value(), options, allocator());
    CY_REQUIRE(quiet.has_value());
    const CompiledProgram* quiet_secondary =
        quiet.value().find(ProgramKind::Secondary, QualityTier::High);
    CY_REQUIRE(quiet_secondary != nullptr);
    CY_CHECK_FALSE(quiet_secondary->difference.albedo_changed);
    CY_CHECK_FALSE(has_diagnostic(quiet.value(), "derivation-changes-albedo"));
    // It is still cheaper: the microdetail sample became its declared average.
    const CompiledProgram* quiet_primary =
        quiet.value().find(ProgramKind::Primary, QualityTier::High);
    CY_REQUIRE(quiet_primary != nullptr);
    CY_CHECK_LT(quiet_secondary->cost.texture_samples, quiet_primary->cost.texture_samples);
}

CY_TEST_CASE("material_lowering: an author's override survives the heuristic") {
    // "WHEN an author marks a node as contributing to base reflectance THEN it SHALL be retained in
    // the corresponding program regardless of the automatic heuristic."
    MaterialGraph graph(allocator(), Name::intern("worn_metal"));
    GraphIds ids;
    CY_REQUIRE(build_reference_graph(graph, ids));
    CY_REQUIRE(graph.annotate(ids.grime_sample, NodeFlags::BaseReflectance).has_value());
    auto authored = lower_graph(graph, allocator());
    CY_REQUIRE(authored.has_value());

    CompileOptions options;
    auto compiled = compile_material(authored.value(), options, allocator());
    CY_REQUIRE(compiled.has_value());
    const CompiledProgram* secondary =
        compiled.value().find(ProgramKind::Secondary, QualityTier::High);
    CY_REQUIRE(secondary != nullptr);
    CY_CHECK_FALSE(secondary->difference.albedo_changed);
    CY_CHECK_EQ(secondary->cost.texture_samples, 2U);
}

CY_TEST_CASE("material_lowering: tiers are three, declared, and counted") {
    auto module = from_text(kTwoSwitches);
    CY_REQUIRE(module.has_value());
    CompileOptions options;
    auto compiled = compile_material(module.value(), options, allocator());
    CY_REQUIRE(compiled.has_value());

    // "WHEN a material has three tiers and two static parameters with two values each THEN the
    // reported permutation count SHALL be twelve."
    CY_CHECK_EQ(compiled.value().layout().static_bool_count(), 2U);
    const CompiledProgram* primary = compiled.value().find(ProgramKind::Primary, QualityTier::High);
    CY_REQUIRE(primary != nullptr);
    CY_CHECK_EQ(primary->cost.permutation_count, 12U);
    CY_CHECK_EQ(primary->cost.program_count, 4U);
    CY_CHECK_EQ(primary->cost.total_programs, 48U);
    CY_CHECK_EQ(primary->cost.geometry_variants, 1U);
}

CY_TEST_CASE("material_lowering: a purely numeric parameter cannot be forced static") {
    constexpr const char* kForced = R"(
material forced {
    static param rough : float = 0.3;
    param base : float3 = (0.5, 0.5, 0.5);
    surface = diffuse(base) + specular(base, rough);
    opacity = 1.0;
}
)";
    auto module = from_text(kForced);
    CY_REQUIRE(module.has_value());
    CompileOptions options;
    options.derive_family = false;
    options.derive_tiers = false;
    auto compiled = compile_material(module.value(), options, allocator());
    CY_REQUIRE(compiled.has_value());
    CY_CHECK(compiled.value().failed());
    CY_CHECK(has_diagnostic(compiled.value(), "static-annotation-refused"));
    CY_CHECK_EQ(compiled.value().layout().static_bool_count(), 0U);
}

CY_TEST_CASE("material_lowering: a field the project does not declare is a cook-time failure") {
    constexpr const char* kWet = R"(
material wet_rock {
    param base : float3 = (0.4, 0.4, 0.4);
    field wetness : float;
    field snow_depth : float;
    surface = diffuse(base * (1 - wetness) * (1 - snow_depth));
    opacity = 1.0;
}
)";
    auto module = from_text(kWet);
    CY_REQUIRE(module.has_value());

    const Name declared[] = {Name::intern("wetness")};
    CompileOptions options;
    options.derive_family = false;
    options.derive_tiers = false;
    options.check_fields = true;
    options.declared_fields = Span<const Name>(declared, 1);
    auto compiled = compile_material(module.value(), options, allocator());
    CY_REQUIRE(compiled.has_value());
    CY_CHECK(compiled.value().failed());
    CY_CHECK(has_diagnostic(compiled.value(), "undeclared-field"));

    const Name both[] = {Name::intern("wetness"), Name::intern("snow_depth")};
    options.declared_fields = Span<const Name>(both, 2);
    auto accepted = compile_material(module.value(), options, allocator());
    CY_REQUIRE(accepted.has_value());
    CY_CHECK_FALSE(accepted.value().failed());
}

CY_TEST_CASE("material_lowering: the vertex-stage variant count is reported rather than hidden") {
    auto module = from_text(reference_text());
    CY_REQUIRE(module.has_value());
    CompileOptions options;
    options.profile = mobile_profile();
    options.geometry_sources = 4;
    options.derive_tiers = false;
    auto compiled = compile_material(module.value(), options, allocator());
    CY_REQUIRE(compiled.has_value());
    const CompiledProgram* primary = compiled.value().find(ProgramKind::Primary, QualityTier::High);
    CY_REQUIRE(primary != nullptr);
    CY_CHECK_EQ(primary->cost.geometry_variants, 4U);
    CY_CHECK_EQ(primary->cost.permutation_count, 12U);
}

CY_TEST_CASE("material_lowering: cost is attributed to the authoring nodes that caused it") {
    MaterialGraph graph(allocator(), Name::intern("worn_metal"));
    GraphIds ids;
    CY_REQUIRE(build_reference_graph(graph, ids));
    auto authored = lower_graph(graph, allocator());
    CY_REQUIRE(authored.has_value());
    CompileOptions options;
    options.derive_family = false;
    options.derive_tiers = false;
    auto compiled = compile_material(authored.value(), options, allocator());
    CY_REQUIRE(compiled.has_value());
    const CompiledProgram* primary = compiled.value().find(ProgramKind::Primary, QualityTier::High);
    CY_REQUIRE(primary != nullptr);

    CY_CHECK_GT(primary->cost.per_node.size(), 0U);
    // The most expensive entry is a texture sample, and it names a graph node rather than an IR
    // one.
    CY_CHECK_GT(primary->cost.per_node[0].texture_samples, 0U);
    CY_CHECK_NE(primary->cost.per_node[0].origin, kUnattributed);
    CY_CHECK_GT(primary->cost.full_screen_ms, 0.0F);
    CY_CHECK_GT(primary->cost.estimated_registers, 0U);

    // The two graph nodes that sampled `base_color_map` are BOTH attributed, because interning
    // merged them into one value and an editor has to highlight both.
    bool first = false;
    bool second = false;
    for (const NodeCost& entry : primary->cost.per_node) {
        first = first || entry.origin == ids.albedo_sample;
        second = second || entry.origin == ids.second_sample;
    }
    CY_CHECK(first);
    CY_CHECK(second);
}

CY_TEST_CASE("material_lowering: the compiled layout is the GPU material table's") {
    // The seam M3's material.h described and left open: "A material authored as data at M3 fills
    // that shape directly; a material compiled from a closure graph at M7 fills the same shape, and
    // nothing downstream changes." So the compiler's output is used HERE the way a renderer uses
    // it — a slot, a parameter written by its compile-time identifier, a read back — rather than
    // inspected as a description.
    auto module = from_text(reference_text());
    CY_REQUIRE(module.has_value());
    CompileOptions options;
    options.derive_family = false;
    options.derive_tiers = false;
    auto compiled = compile_material(module.value(), options, allocator());
    CY_REQUIRE(compiled.has_value());
    const MaterialProgram& layout = compiled.value().layout();

    // Every declared parameter and texture has a slot, and the shading model is the lowered one.
    CY_CHECK_EQ(layout.parameters().size(), 6U);  // four parameters, two textures
    CY_CHECK_EQ(layout.model(), cy::render::ShadingModel::Lit);
    CY_CHECK_EQ(layout.blend(), BlendMode::Opaque);

    MaterialTable table(allocator());
    CY_REQUIRE(table.initialize(4).has_value());
    auto slot = table.allocate();
    CY_REQUIRE(slot.has_value());
    constexpr ParameterId kRoughness = parameter_id("roughness");
    CY_REQUIRE(table.set_float(layout, slot.value(), kRoughness, 0.6F).has_value());
    auto read = table.get_float(layout, slot.value(), kRoughness);
    CY_REQUIRE(read.has_value());
    CY_CHECK_EQ(read.value(), 0.6F);

    // An instance is a slot plus the same program: no compilation, and the parameter comes with it.
    auto instance = table.instantiate(slot.value());
    CY_REQUIRE(instance.has_value());
    auto inherited = table.get_float(layout, instance.value(), kRoughness);
    CY_REQUIRE(inherited.has_value());
    CY_CHECK_EQ(inherited.value(), 0.6F);

    // `emissive` is declared and nothing reads it, which is what the unused-parameter warning the
    // compiler carries out of M3's validator is about.
    CY_CHECK(has_diagnostic(compiled.value(), "UnusedParameter"));
}

CY_TEST_CASE("material_lowering: tier selection does not oscillate over a sweep of weights") {
    // design.md §2.1's methodological finding, applied here: "a control law over a discrete ladder
    // MUST be certified over a sweep of loads, never one load ... The spike's own first draft was
    // certified on one load and oscillated on 27 of 71." A tier boundary is the same discrete
    // ladder, so this walks 81 weights across both thresholds with +/-3% noise on each frame and
    // counts the tier changes after the weight has settled.
    const TierSelection thresholds;
    u32 oscillating = 0;
    u32 worst = 0;
    for (u32 step = 0; step < 81U; ++step) {
        const f32 weight = 0.02F + (static_cast<f32>(step) * 0.005F);
        QualityTier tier = QualityTier::High;
        u32 changes = 0;
        u32 noise = 12345U + step;
        for (u32 frame = 0; frame < 200U; ++frame) {
            noise = (noise * 1664525U) + 1013904223U;
            const f32 jitter =
                1.0F + (((static_cast<f32>((noise >> 16U) & 0xFFU) / 255.0F) - 0.5F) * 0.06F);
            const QualityTier next = select_tier(tier, weight * jitter, thresholds);
            if (frame >= 50U && next != tier) {
                ++changes;
            }
            tier = next;
        }
        oscillating += changes > 0 ? 1U : 0U;
        worst = changes > worst ? changes : worst;
    }
    CY_CHECK_EQ(oscillating, 0U);
    CY_CHECK_EQ(worst, 0U);

    // And it still moves when the weight really moves.
    CY_CHECK_EQ(select_tier(QualityTier::High, 0.02F, thresholds), QualityTier::Low);
    CY_CHECK_EQ(select_tier(QualityTier::Low, 0.9F, thresholds), QualityTier::High);
}

CY_TEST_CASE("material_binning: cost scales with programs, not with instances") {
    // "WHEN ten thousand instances of one material program are visible THEN they SHALL be evaluated
    // in one bin", and "WHEN a view contains a thousand material instances derived from twelve
    // programs THEN material evaluation SHALL dispatch twelve bins."
    Array<u32> pixels(allocator());
    CY_REQUIRE(pixels.resize(10000).has_value());
    for (u32& pixel : pixels) {
        pixel = 3;  // one program, ten thousand instances behind it
    }
    MaterialBinning binning(allocator());
    CY_REQUIRE(binning.classify(pixels.span(), 12, 64).has_value());
    CY_CHECK_EQ(binning.dispatch_count(), 1U);
    CY_CHECK_EQ(binning.bins()[3].count, 10000U);
    CY_CHECK_EQ(binning.bins()[3].groups, 157U);
    CY_CHECK_EQ(binning.classified_pixels(), 10000U);

    for (usize index = 0; index < pixels.size(); ++index) {
        pixels[index] = static_cast<u32>(index % 12);
    }
    CY_REQUIRE(binning.classify(pixels.span(), 12, 64).has_value());
    CY_CHECK_EQ(binning.dispatch_count(), 12U);

    // The compacted list is a permutation of the covered pixels, and each bin holds its own.
    Array<u8> seen(allocator());
    CY_REQUIRE(seen.resize(pixels.size()).has_value());
    for (u8& mark : seen) {
        mark = 0;
    }
    for (const MaterialBin& bin : binning.bins()) {
        for (u32 offset = 0; offset < bin.count; ++offset) {
            const u32 pixel = binning.pixels()[bin.first + offset];
            CY_CHECK_EQ(pixels[pixel], bin.program);
            seen[pixel] = static_cast<u8>(seen[pixel] + 1U);
        }
    }
    bool once_each = true;
    for (const u8 count : seen) {
        once_each = once_each && count == 1U;
    }
    CY_CHECK(once_each);

    // A pixel no material covers is classified into nothing, and an out-of-range program is
    // refused rather than written past the end of a bin.
    pixels[0] = kNoProgram;
    CY_REQUIRE(binning.classify(pixels.span(), 12, 64).has_value());
    CY_CHECK_EQ(binning.classified_pixels(), 9999U);
    pixels[0] = 99;
    CY_CHECK_FALSE(binning.classify(pixels.span(), 12, 64).has_value());
}
