// Lowering: closure sets, shading models, the program family, quality tiers. M7 task 6.3.
//
// See lowering.h for the four findings this file is shaped by.

#include <cy/rendering/material/lowering.h>

#include <utility>

#include "closure_algebra.h"
#include "rebuild.h"

namespace cy::rendering::material {
namespace {

[[nodiscard]] u32 closure_bit(Op leaf) noexcept {
    return op_is_leaf_closure(leaf) ? 1U << (static_cast<u32>(leaf) - static_cast<u32>(Op::Diffuse))
                                    : 0U;
}

/// The leaves a derived program drops, by kind. Everything else about derivation is either a
/// texture substitution or nothing at all.
[[nodiscard]] u32 dropped_leaves(ProgramKind kind, QualityTier tier) noexcept {
    u32 mask = 0;
    if (kind == ProgramKind::Secondary) {
        // Secondary closures whose contribution to outgoing radiance is small: a coat is a
        // specular lobe the surface cache does not resolve, and sheen is a grazing-angle term.
        mask |= closure_bit(Op::Coat) | closure_bit(Op::Sheen);
    }
    if (kind == ProgramKind::FarField) {
        mask |= closure_bit(Op::Coat) | closure_bit(Op::Sheen) | closure_bit(Op::Specular) |
                closure_bit(Op::Transmission) | closure_bit(Op::Subsurface);
    }
    if (tier == QualityTier::Low) {
        mask |= closure_bit(Op::Coat) | closure_bit(Op::Sheen);
    }
    return mask;
}

/// Whether a texture sample is replaced by its declared average.
///
/// A far-field program replaces every sample — "its far-field program SHALL supply averaged
/// constants rather than sampling textures". A secondary program and the two lower tiers replace
/// the samples an author marked as microdetail, unless the same author also marked the node as
/// contributing to base reflectance, which is the override the specification requires.
[[nodiscard]] bool substitutes_texture(const Module& module, NodeId id, ProgramKind kind,
                                       QualityTier tier) noexcept {
    if (kind == ProgramKind::FarField) {
        return true;
    }
    const NodeFlags flags = module.flags(id);
    if (has_flag(flags, NodeFlags::BaseReflectance)) {
        return false;
    }
    const bool microdetail = has_flag(flags, NodeFlags::Microdetail);
    return microdetail && (kind == ProgramKind::Secondary || tier != QualityTier::High);
}

struct DeriveState {
    ProgramKind kind = ProgramKind::Primary;
    QualityTier tier = QualityTier::High;
    u32 dropped = 0;
    u32 simplifications = 0;
};

[[nodiscard]] Expected<NodeId, Error> derive_node(const Module& source, NodeId id,
                                                  Span<const NodeId> operands, Builder& out,
                                                  DeriveState& state) noexcept {
    const Node& node = source.node(id);
    if (op_is_leaf_closure(node.op) && (state.dropped & closure_bit(node.op)) != 0 &&
        !has_flag(source.flags(id), NodeFlags::BaseReflectance)) {
        return kInvalidNode;
    }
    if (op_is_closure(node.op)) {
        return detail::simplify_closure(node, operands, out, state.simplifications);
    }
    if (node.op == Op::TextureSample && substitutes_texture(source, id, state.kind, state.tier)) {
        const TextureDecl* declared = source.find_texture(node.symbol);
        const Immediate average = declared != nullptr ? declared->average : Immediate{};
        return out.make(Op::Constant, ValueType::Vec4, Name{}, average, {});
    }
    return detail::rebuild_as_is(source, id, operands, out);
}

/// Collect the texture names reachable from a node, as an insertion into a sorted set.
[[nodiscard]] Status collect_textures(const Module& module, NodeId root,
                                      Array<Name>& out) noexcept {
    Array<NodeId> order(module.allocator());
    if (Status walked = canonical_order(module, Span<const NodeId>(&root, 1), order); !walked) {
        return walked;
    }
    for (const NodeId id : order) {
        if (module.node(id).op != Op::TextureSample) {
            continue;
        }
        const Name texture = module.node(id).symbol;
        usize position = 0;
        bool present = false;
        while (position < out.size() && out[position].text() < texture.text()) {
            ++position;
        }
        present = position < out.size() && out[position] == texture;
        if (present) {
            continue;
        }
        // Sorted by TEXT, never by `Name::index()`: interning order is not stable across runs and
        // this set is compared between two compilations.
        if (Status pushed = out.push_back(texture); !pushed) {
            return pushed;
        }
        for (usize index = out.size() - 1; index > position; --index) {
            const Name swap = out[index - 1];
            out[index - 1] = out[index];
            out[index] = swap;
        }
    }
    return ok();
}

}  // namespace

bool ClosureSet::has(Op leaf) const noexcept {
    return (mask & closure_bit(leaf)) != 0;
}

u32 ClosureSet::count() const noexcept {
    u32 total = 0;
    for (u32 bit = 0; bit < 32U; ++bit) {
        total += (mask >> bit) & 1U;
    }
    return total;
}

ClosureSet closure_set(const Module& module) noexcept {
    ClosureSet set;
    const NodeId root = module.surface();
    if (root == kInvalidNode) {
        return set;
    }
    Array<NodeId> order(module.allocator());
    if (!canonical_order(module, Span<const NodeId>(&root, 1), order)) {
        return set;
    }
    for (const NodeId id : order) {
        set.mask |= closure_bit(module.node(id).op);
    }
    return set;
}

Lowered match_shading_model(const ClosureSet& closures) noexcept {
    const u32 diffuse = closure_bit(Op::Diffuse);
    const u32 specular = closure_bit(Op::Specular);
    const u32 emission = closure_bit(Op::Emission);
    const u32 coat = closure_bit(Op::Coat);
    const u32 sheen = closure_bit(Op::Sheen);
    const u32 subsurface = closure_bit(Op::Subsurface);
    const u32 transmission = closure_bit(Op::Transmission);
    // Emission never decides a model: it is an additive term every model carries.
    const u32 core = closures.mask & ~emission;

    Lowered lowered;
    if (core == diffuse || core == (diffuse | specular)) {
        lowered.model = ShadingModel::Lit;
        return lowered;
    }
    if (core == (diffuse | specular | coat)) {
        lowered.model = ShadingModel::ClearCoat;
        return lowered;
    }
    if (core == (diffuse | specular | sheen)) {
        lowered.model = ShadingModel::Cloth;
        return lowered;
    }
    if (core == (diffuse | specular | subsurface) || core == (diffuse | subsurface)) {
        lowered.model = ShadingModel::SubsurfaceScattering;
        return lowered;
    }
    if (core == (diffuse | specular | transmission)) {
        lowered.model = ShadingModel::Water;
        return lowered;
    }
    if (core == 0) {
        lowered.model = ShadingModel::Unlit;
        return lowered;
    }
    lowered.model = ShadingModel::Lit;
    lowered.generic_evaluator = true;
    // The generic layered evaluator walks the closure tree at run time instead of evaluating a
    // fixed model. The multiple grows with the number of distinct leaves, which is what makes the
    // cook report's number a statement about THIS material rather than a constant.
    lowered.generic_cost_multiple = 1.0F + (0.35F * static_cast<f32>(closures.count()));
    return lowered;
}

Profile desktop_profile() noexcept {
    return Profile{"desktop", true, 0, true};
}

Profile mobile_profile() noexcept {
    // No generic evaluator, no transmission and no subsurface, and a vertex-stage pipeline — so a
    // material used on four geometry sources compiles four variants and the report says so.
    return Profile{"mobile", false, closure_bit(Op::Transmission) | closure_bit(Op::Subsurface),
                   false};
}

Expected<Module, Error> derive_program(const Module& primary,
                                       const DerivationOptions& options) noexcept {
    Builder builder(primary.allocator(), primary.name());
    for (const ParameterDecl& decl : primary.parameters()) {
        if (Status declared = builder.declare_parameter(decl); !declared) {
            return make_unexpected(declared.error());
        }
    }
    for (const TextureDecl& decl : primary.textures()) {
        TextureDecl stored = decl;
        // The shadow program's textures are shadow-critical: `residency` must keep a coarse level
        // of them resident so shadow rasterisation never waits on a texture.
        stored.shadow_critical = decl.shadow_critical || options.kind == ProgramKind::Shadow;
        if (Status declared = builder.declare_texture(stored); !declared) {
            return make_unexpected(declared.error());
        }
    }

    DeriveState state;
    state.kind = options.kind;
    state.tier = options.tier;
    state.dropped = dropped_leaves(options.kind, options.tier);

    // The shadow program's ONLY root is opacity. Not "the surface root with the closures removed":
    // a surface root that survived would carry every texture the material samples into a pass that
    // exists to answer a silhouette question.
    const bool shadow = options.kind == ProgramKind::Shadow;
    const NodeId roots[] = {shadow ? kInvalidNode : primary.surface(), primary.opacity()};

    Array<NodeId> mapping(primary.allocator());
    const auto visit = [&state](const Module& module, NodeId id, Span<const NodeId> operands,
                                Builder& out) noexcept {
        return derive_node(module, id, operands, out, state);
    };
    if (Status rebuilt =
            detail::rebuild_module(primary, Span<const NodeId>(roots, 2), builder, mapping, visit);
        !rebuilt) {
        return make_unexpected(rebuilt.error());
    }

    if (!shadow && primary.surface() != kInvalidNode &&
        mapping[primary.surface()] != kInvalidNode) {
        if (Status set = builder.set_surface(mapping[primary.surface()]); !set) {
            return make_unexpected(set.error());
        }
    }
    if (primary.opacity() != kInvalidNode && mapping[primary.opacity()] != kInvalidNode) {
        const Node& opacity = primary.node(primary.opacity());
        const bool opaque = opacity.op == Op::Constant && opacity.value.x >= 1.0F;
        // An opaque material has no shadow program at all. The module comes back with no roots,
        // which is the structural form of "no fragment work".
        if (!(shadow && opaque)) {
            if (Status set = builder.set_opacity(mapping[primary.opacity()]); !set) {
                return make_unexpected(set.error());
            }
        }
    }
    return builder.finish();
}

Status albedo_textures(const Module& module, Array<Name>& out) noexcept {
    out.clear();
    const NodeId root = module.surface();
    if (root == kInvalidNode) {
        return ok();
    }
    Array<NodeId> order(module.allocator());
    if (Status walked = canonical_order(module, Span<const NodeId>(&root, 1), order); !walked) {
        return walked;
    }
    for (const NodeId id : order) {
        const Op op = module.node(id).op;
        if (op != Op::Diffuse && op != Op::Specular) {
            continue;
        }
        // Operand 0 is the colour of both. The roughness operand of `Specular` is deliberately not
        // followed: a texture that only reaches roughness does not change albedo.
        if (Status collected = collect_textures(module, module.operands(id)[0], out); !collected) {
            return collected;
        }
    }
    return ok();
}

Expected<DerivationDifference, Error> compare_derivation(const Module& primary,
                                                         const Module& derived) noexcept {
    DerivationDifference difference;
    difference.empty_program =
        derived.surface() == kInvalidNode && derived.opacity() == kInvalidNode;

    Array<Name> before(primary.allocator());
    Array<Name> after(derived.allocator());
    if (Status collected = albedo_textures(primary, before); !collected) {
        return make_unexpected(collected.error());
    }
    if (Status collected = albedo_textures(derived, after); !collected) {
        return make_unexpected(collected.error());
    }
    difference.primary_albedo_textures = static_cast<u32>(before.size());
    difference.derived_albedo_textures = static_cast<u32>(after.size());
    for (const Name texture : before) {
        bool present = false;
        for (const Name candidate : after) {
            present = present || candidate == texture;
        }
        if (!present) {
            difference.albedo_changed = true;
            if (difference.responsible.is_empty()) {
                difference.responsible = texture;
            }
        }
    }

    const bool primary_opaque = primary.opacity() == kInvalidNode;
    const bool derived_opaque = derived.opacity() == kInvalidNode;
    if (primary_opaque != derived_opaque) {
        difference.opacity_changed = !difference.empty_program;
    } else if (!primary_opaque) {
        difference.opacity_changed =
            primary.node(primary.opacity()).hash != derived.node(derived.opacity()).hash;
    }
    return difference;
}

QualityTier select_tier(QualityTier current, f32 weight, const TierSelection& thresholds) noexcept {
    const f32 up = 1.0F + thresholds.hysteresis;
    const f32 down = 1.0F - thresholds.hysteresis;
    // Improving asks for the threshold plus a margin; worsening asks for it minus one. The two
    // margins straddle each boundary, so a weight sitting on a threshold holds the tier it has.
    switch (current) {
        case QualityTier::High:
            if (weight < thresholds.low_threshold * down) {
                return QualityTier::Low;
            }
            return weight < thresholds.medium_threshold * down ? QualityTier::Medium
                                                               : QualityTier::High;
        case QualityTier::Medium:
            if (weight >= thresholds.medium_threshold * up) {
                return QualityTier::High;
            }
            return weight < thresholds.low_threshold * down ? QualityTier::Low
                                                            : QualityTier::Medium;
        case QualityTier::Low:
        case QualityTier::Count:
            break;
    }
    if (weight >= thresholds.medium_threshold * up) {
        return QualityTier::High;
    }
    return weight >= thresholds.low_threshold * up ? QualityTier::Medium : QualityTier::Low;
}

}  // namespace cy::rendering::material
