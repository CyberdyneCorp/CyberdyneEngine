// The material compiler: the join. M7 tasks 6.1 to 6.4. See compiler.h.

#include <cy/rendering/material/compiler.h>

#include <cy/rendering/material/validation.h>

#include <algorithm>
#include <utility>

namespace cy::rendering::material {
namespace {

[[nodiscard]] ParameterKind kind_of(ValueType type) noexcept {
    switch (type) {
        case ValueType::Vec2:
            return ParameterKind::Vec2;
        case ValueType::Vec3:
            return ParameterKind::Vec3;
        case ValueType::Vec4:
            return ParameterKind::Vec4;
        case ValueType::Int:
            return ParameterKind::Int;
        case ValueType::Bool:
            return ParameterKind::Bool;
        default:
            break;
    }
    return ParameterKind::Float;
}

/// A parameter is STATIC only when it feeds a decision that cannot be expressed as data: the
/// condition of a `Select`, which is the IR's only branch. Everything else is a number in the
/// material table, and changing it compiles nothing.
[[nodiscard]] bool is_static_use(const Module& module, Name parameter) noexcept {
    for (NodeId id = 0; id < module.size(); ++id) {
        if (module.node(id).op != Op::Select) {
            continue;
        }
        const NodeId condition = module.operands(id)[0];
        if (module.node(condition).op == Op::Parameter &&
            module.node(condition).symbol == parameter) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] bool is_referenced(const Module& module, Name parameter) noexcept {
    for (NodeId id = 0; id < module.size(); ++id) {
        const Node& node = module.node(id);
        if ((node.op == Op::Parameter || node.op == Op::TextureSample) &&
            node.symbol == parameter) {
            return true;
        }
    }
    return false;
}

struct Build {
    CompiledMaterial* material = nullptr;
    const CompileOptions* options = nullptr;
    Allocator* allocator = nullptr;
};

/// Append a diagnostic, once per (code, subject).
///
/// ONCE, because the family is derived twelve times and a texture that stops reaching albedo stops
/// reaching it in every reduced program. Twelve copies of one sentence is a report nobody reads to
/// the end, which is the same failure as not reporting it.
[[nodiscard]] Status say(Array<CompileDiagnostic>& into, DiagnosticSeverity severity,
                         const char* code, const char* detail, Name subject) noexcept {
    for (const CompileDiagnostic& existing : into) {
        if (existing.code == code && existing.subject == subject) {
            return ok();
        }
    }
    CompileDiagnostic diagnostic;
    diagnostic.severity = severity;
    diagnostic.code = code;
    diagnostic.detail = detail;
    diagnostic.subject = subject;
    return into.push_back(diagnostic);
}

/// Fill in the parameter layout and classify every parameter, refusing an annotation that would
/// multiply programs for no structural difference.
[[nodiscard]] Status build_layout(const Module& primary, const Lowered& lowered,
                                  MaterialProgram& layout,
                                  Array<CompileDiagnostic>& diagnostics) noexcept {
    // OPAQUE WHEN OPACITY FOLDED TO ONE. The same fact the shadow derivation reads: a material
    // whose opacity is the constant 1 has no alpha test, and calling it `Masked` would make
    // validation demand an `alpha_cutoff` the depth prepass would never apply.
    const bool opaque =
        primary.opacity() == kInvalidNode || (primary.node(primary.opacity()).op == Op::Constant &&
                                              primary.node(primary.opacity()).value.x >= 1.0F);
    const BlendMode blend = opaque ? BlendMode::Opaque : BlendMode::Masked;
    if (Status started = layout.initialize(primary.name(), lowered.model, blend); !started) {
        return started;
    }
    for (const ParameterDecl& decl : primary.parameters()) {
        const bool statically_used = is_static_use(primary, decl.name);
        if (decl.requested_static && !statically_used) {
            if (Status said =
                    say(diagnostics, DiagnosticSeverity::Error, "static-annotation-refused",
                        "this parameter only scales a value, so making it static would "
                        "compile one program per value for no structural difference",
                        decl.name);
                !said) {
                return said;
            }
        }
        auto declared =
            layout.add_parameter(decl.name.c_str(), kind_of(decl.type), statically_used);
        if (!declared) {
            return make_unexpected(declared.error());
        }
        // `alpha_cutoff` is read by the fixed-function alpha test rather than by the graph, so a
        // masked material's own threshold is not an unused parameter.
        const bool cutoff = !opaque && decl.name == Name::intern("alpha_cutoff");
        layout.mark_referenced(declared.value(), cutoff || is_referenced(primary, decl.name));
    }
    for (const TextureDecl& decl : primary.textures()) {
        auto declared = layout.add_parameter(decl.name.c_str(), ParameterKind::Texture, false);
        if (!declared) {
            return make_unexpected(declared.error());
        }
        layout.mark_referenced(declared.value(), is_referenced(primary, decl.name));
    }
    return ok();
}

/// The profile's refusals. Both are cook-time errors naming what is responsible, which is
/// `material-compiler`'s "SHALL fail with a diagnostic naming the closures responsible".
[[nodiscard]] Status check_profile(const Module& primary, const Lowered& lowered,
                                   const Profile& profile,
                                   Array<CompileDiagnostic>& diagnostics) noexcept {
    if (lowered.generic_evaluator && !profile.supports_generic_evaluator) {
        if (Status said =
                say(diagnostics, DiagnosticSeverity::Error, "generic-evaluator-unsupported",
                    "this closure set matches no shading model and the target profile has "
                    "no generic layered evaluator",
                    primary.name());
            !said) {
            return said;
        }
    }
    const ClosureSet closures = closure_set(primary);
    if ((closures.mask & profile.unsupported_closures) == 0) {
        return ok();
    }
    // Name the closures rather than the material: "WHEN a material uses a node unavailable on the
    // mobile profile THEN the report SHALL name that node, not merely report the material as
    // unsupported."
    for (u32 leaf = static_cast<u32>(Op::Diffuse); leaf <= static_cast<u32>(Op::Emission); ++leaf) {
        const u32 bit = 1U << (leaf - static_cast<u32>(Op::Diffuse));
        if ((closures.mask & profile.unsupported_closures & bit) == 0) {
            continue;
        }
        if (Status said = say(diagnostics, DiagnosticSeverity::Error, "closure-unsupported",
                              op_name(static_cast<Op>(leaf)), primary.name());
            !said) {
            return said;
        }
    }
    return ok();
}

/// Every environment field the material samples must be one the project declared.
[[nodiscard]] Status check_fields(const Module& primary, const CompileOptions& options,
                                  Array<CompileDiagnostic>& diagnostics) noexcept {
    if (!options.check_fields) {
        return ok();
    }
    for (NodeId id = 0; id < primary.size(); ++id) {
        const Node& node = primary.node(id);
        if (node.op != Op::Field) {
            continue;
        }
        bool declared = false;
        for (const Name field : options.declared_fields) {
            declared = declared || field == node.symbol;
        }
        if (declared) {
            continue;
        }
        if (Status said = say(diagnostics, DiagnosticSeverity::Error, "undeclared-field",
                              "this material samples an environment field the project does not "
                              "declare",
                              node.symbol);
            !said) {
            return said;
        }
    }
    return ok();
}

/// Translate M3's material report into the compiler's diagnostics, so there is one validator rather
/// than two answers to "is this material well formed".
[[nodiscard]] Status carry_validation(const MaterialProgram& layout,
                                      Array<CompileDiagnostic>& diagnostics,
                                      Allocator& allocator) noexcept {
    MaterialReport report(allocator);
    MaterialValidationOptions options;
    if (Status validated = validate_material(layout, {}, options, report); !validated) {
        return validated;
    }
    for (const MaterialIssue& issue : report.issues()) {
        if (Status said = say(diagnostics,
                              issue.fatal ? DiagnosticSeverity::Error : DiagnosticSeverity::Warning,
                              material_issue_kind_name(issue.kind), issue.detail, issue.subject);
            !said) {
            return said;
        }
    }
    return ok();
}

/// Derive, optimise, emit and cost one member of the family.
[[nodiscard]] Expected<CompiledProgram, Error> compile_one(const Module& primary,
                                                           const CompileOptions& options,
                                                           ProgramKind kind, QualityTier tier,
                                                           OptimiseReport& optimisation,
                                                           Allocator& allocator) noexcept {
    CompiledProgram program(allocator);
    program.kind = kind;
    program.tier = tier;

    DerivationOptions derivation;
    derivation.kind = kind;
    derivation.tier = tier;
    auto derived = derive_program(primary, derivation);
    if (!derived) {
        return make_unexpected(derived.error());
    }
    auto settled = optimise(derived.value(), options.passes, optimisation);
    if (!settled) {
        return make_unexpected(settled.error());
    }
    program.module = std::move(settled.value());

    auto difference = compare_derivation(primary, program.module);
    if (!difference) {
        return make_unexpected(difference.error());
    }
    program.difference = difference.value();
    program.absent =
        program.module.surface() == kInvalidNode && program.module.opacity() == kInvalidNode;

    const Lowered lowered = match_shading_model(closure_set(program.module));
    program.model = lowered.model;
    program.generic_evaluator = lowered.generic_evaluator;
    if (program.absent) {
        return program;
    }

    EmitOptions emit;
    emit.kind = kind;
    emit.tier = tier;
    emit.shading_model = render::shading_model_name(lowered.model);
    emit.canonical_order = options.passes.canonical_emission_order;
    emit.hoist_uniform = options.passes.uniform_varying;
    auto source = emit_program(program.module, emit);
    if (!source) {
        return make_unexpected(source.error());
    }
    program.source = std::move(source.value());

    if (Status analysed =
            analyse_cost(program.module, program.source, lowered, options.cost_model, program.cost);
        !analysed) {
        return make_unexpected(analysed.error());
    }
    return program;
}

}  // namespace

const char* diagnostic_severity_name(DiagnosticSeverity severity) noexcept {
    switch (severity) {
        case DiagnosticSeverity::Info:
            return "info";
        case DiagnosticSeverity::Warning:
            return "warning";
        case DiagnosticSeverity::Error:
            return "error";
    }
    return "?";
}

CompiledMaterial::CompiledMaterial(Allocator& allocator) noexcept
    : programs_(allocator), diagnostics_(allocator), layout_(allocator), optimisation_(allocator) {}

const CompiledProgram* CompiledMaterial::find(ProgramKind kind, QualityTier tier) const noexcept {
    for (const CompiledProgram& program : programs_) {
        if (program.kind == kind && program.tier == tier) {
            return &program;
        }
    }
    return nullptr;
}

bool CompiledMaterial::failed() const noexcept {
    return std::ranges::any_of(diagnostics_, [](const CompileDiagnostic& diagnostic) noexcept {
        return diagnostic.severity == DiagnosticSeverity::Error;
    });
}

const Module& CompiledMaterial::primary() const noexcept {
    return programs_[0].module;
}

NodeId value_of_origin(const Module& module, u32 authoring_node) noexcept {
    for (NodeId id = 0; id < module.size(); ++id) {
        for (const u32 origin : module.origins(id)) {
            if (origin == authoring_node) {
                return id;
            }
        }
    }
    return kInvalidNode;
}

Expected<GeneratedSource, Error> preview_node(const CompiledProgram& program,
                                              NodeId node) noexcept {
    if (node >= program.module.size()) {
        return make_unexpected(
            Error{ErrorCode::NotFound,
                  "this value is not in the compiled module — the optimiser removed the node, "
                  "which is what the drop report is for",
                  0});
    }
    EmitOptions options;
    options.kind = program.kind;
    options.tier = program.tier;
    options.shading_model = render::shading_model_name(program.model);
    options.preview_root = node;
    return emit_program(program.module, options);
}

Expected<CompiledMaterial, Error> compile_material(const Module& authored,
                                                   const CompileOptions& options,
                                                   Allocator& allocator) noexcept {
    CompiledMaterial material(allocator);

    auto optimised = optimise(authored, options.passes, material.optimisation_);
    if (!optimised) {
        return make_unexpected(optimised.error());
    }
    const Module primary = std::move(optimised.value());

    const Lowered lowered = match_shading_model(closure_set(primary));
    if (Status built = build_layout(primary, lowered, material.layout_, material.diagnostics_);
        !built) {
        return make_unexpected(built.error());
    }
    if (Status checked = check_profile(primary, lowered, options.profile, material.diagnostics_);
        !checked) {
        return make_unexpected(checked.error());
    }
    if (Status checked = check_fields(primary, options, material.diagnostics_); !checked) {
        return make_unexpected(checked.error());
    }
    if (Status carried = carry_validation(material.layout_, material.diagnostics_, allocator);
        !carried) {
        return make_unexpected(carried.error());
    }
    if (material.optimisation_.bisection_build) {
        // design.md §1.4: eight of the nine switches change the compiled program, so a bisection
        // build is NOT the same material compiled more slowly. Saying so is the requirement.
        if (Status said = say(material.diagnostics_, DiagnosticSeverity::Warning, "bisection-build",
                              "an optimisation pass is disabled, and every pass in this pipeline "
                              "changes the compiled program: this material is not the one a "
                              "shipping build produces",
                              primary.name());
            !said) {
            return make_unexpected(said.error());
        }
    }

    const ProgramKind kinds[] = {ProgramKind::Primary, ProgramKind::Secondary,
                                 ProgramKind::FarField, ProgramKind::Shadow};
    const u32 kind_count = options.derive_family ? 4U : 1U;
    const u32 tier_count = options.derive_tiers ? kQualityTierCount : 1U;
    u64 key = hash_u64(kHashSeed, kCompilerVersion);
    key = hash_u64(key, kIrVersion);
    key = hash_u64(key, primary.digest());
    key = hash_text(key, options.profile.name);
    key = hash_u64(key, options.passes.all_enabled() ? 1ULL : 0ULL);

    for (u32 kind = 0; kind < kind_count; ++kind) {
        for (u32 tier = 0; tier < tier_count; ++tier) {
            auto program =
                compile_one(primary, options, kinds[kind], static_cast<QualityTier>(tier),
                            material.optimisation_, allocator);
            if (!program) {
                return make_unexpected(program.error());
            }
            key = hash_u64(key, program.value().source.digest);
            // THE FLAG IS ONLY MEANINGFUL WHERE ALBEDO IS SUPPOSED TO SURVIVE. A far-field program
            // replaces every texture with its declared average by design and a shadow program has
            // no surface at all; warning about either would be warning about the specification.
            // The secondary program and the reduced tiers are the ones an automatic derivation can
            // get wrong, and they are what the specification's scenario is about.
            const bool albedo_matters = kinds[kind] == ProgramKind::Secondary ||
                                        (kinds[kind] == ProgramKind::Primary && tier != 0U);
            if (albedo_matters && program.value().difference.albedo_changed) {
                if (Status said = say(material.diagnostics_, DiagnosticSeverity::Warning,
                                      "derivation-changes-albedo",
                                      "a texture that reaches this material's base colour in the "
                                      "primary program does not reach it in this derived one",
                                      program.value().difference.responsible);
                    !said) {
                    return make_unexpected(said.error());
                }
            }
            if (Status pushed = material.programs_.push_back(std::move(program.value())); !pushed) {
                return make_unexpected(pushed.error());
            }
        }
    }

    const u32 static_bools = material.layout_.static_bool_count();
    for (CompiledProgram& program : material.programs_) {
        count_permutations(static_bools, kind_count, options.profile, options.geometry_sources,
                           program.cost);
    }
    material.cook_key_ = key;
    return material;
}

}  // namespace cy::rendering::material
