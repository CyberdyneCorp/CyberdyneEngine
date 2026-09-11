// The VFX domain: the type lattice, the operation table, the roots, and the folding rules.
// M8.c task 2.1. See ir.h for why this domain exists rather than a shared one.

#include <cy/vfx/ir.h>

#include <cy/core/base/assert.h>

#include <cmath>

namespace cy::vfx {
namespace {

using graph::FoldOutcome;
using graph::MergeClass;
using graph::OpCategory;
using graph::OpDesc;
using graph::RootDecl;
using graph::TypeDesc;
using graph::TypeQuery;

constexpr TypeDesc kTypes[VfxTypeCount] = {
    {"float", 1, true, 0, false},  {"float2", 2, true, 0, false}, {"float3", 3, true, 0, false},
    {"float4", 4, true, 0, false}, {"int", 1, true, 0, false},    {"bool", 1, false, 0, false},
};

/// An arithmetic operation over `count` operands, commutative or not.
constexpr OpDesc arithmetic(const char* name, u32 arity, bool commutative) noexcept {
    OpDesc desc;
    desc.name = name;
    desc.arity = arity;
    desc.commutative = commutative;
    desc.varying = false;
    desc.merge = MergeClass::Expression;
    desc.category = OpCategory::Arithmetic;
    return desc;
}

constexpr OpDesc leaf(const char* name, bool varying, MergeClass merge) noexcept {
    OpDesc desc;
    desc.name = name;
    desc.arity = 0;
    desc.inline_leaf = true;
    desc.uniform_leaf = !varying;
    desc.varying = varying;
    desc.merge = merge;
    desc.category = OpCategory::Leaf;
    return desc;
}

constexpr OpDesc sampler(const char* name, u32 arity) noexcept {
    OpDesc desc;
    desc.name = name;
    desc.arity = arity;
    desc.varying = true;
    desc.merge = MergeClass::Sample;
    desc.category = OpCategory::Sample;
    return desc;
}

constexpr OpDesc kOps[VfxOpCount] = {
    leaf("constant", false, MergeClass::Leaf),
    leaf("parameter", false, MergeClass::Leaf),
    // VARYING, and this is the sentence ir.h is about: an attribute read is per-particle, and the
    // write that changes it is in the kernel's ordered list rather than in this DAG.
    leaf("attribute", true, MergeClass::Leaf),
    leaf("emitter_input", false, MergeClass::Leaf),
    sampler("sample", 1),
    // NEVER MERGED. Two draws are two values; interning them would correlate what an author wrote
    // as independent, and a smoke plume whose particles all chose the same "random" offset is a
    // defect nobody would look for in a hash-consing policy.
    leaf("random", true, MergeClass::Never),
    sampler("curve", 1),
    sampler("noise", 1),

    arithmetic("add", 2, true),
    arithmetic("sub", 2, false),
    arithmetic("mul", 2, true),
    arithmetic("div", 2, false),
    arithmetic("min", 2, true),
    arithmetic("max", 2, true),
    arithmetic("dot", 2, true),
    arithmetic("cross", 2, false),
    arithmetic("length", 1, false),
    arithmetic("normalize", 1, false),
    arithmetic("sin", 1, false),
    arithmetic("cos", 1, false),
    arithmetic("pow", 2, false),
    arithmetic("saturate", 1, false),
    arithmetic("lerp", 3, false),
    // A CONDITIONAL IS A VALUE AND BOTH ARMS ARE EVALUATED — the fifth property expr.h lists. A
    // particle kernel is data-parallel, so that is the right cost model here rather than a
    // limitation: a wavefront evaluates both arms anyway.
    arithmetic("select", 3, false),
    arithmetic("less", 2, false),
    arithmetic("greater", 2, false),
    arithmetic("and", 2, true),
    arithmetic("or", 2, true),
    arithmetic("not", 1, false),
    arithmetic("make_float3", 3, false),
    arithmetic("make_float4", 4, false),
    arithmetic("swizzle", 1, false),
};

/// Eighteen roots: sixteen writes, the kill predicate and the spawn count. Every one accepts any
/// type, because what a write's type is depends on the attribute and the domain's root table is
/// static.
constexpr RootDecl kRoots[kVfxRootCount] = {
    {"w0", kInvalidType, false},  {"w1", kInvalidType, false},   {"w2", kInvalidType, false},
    {"w3", kInvalidType, false},  {"w4", kInvalidType, false},   {"w5", kInvalidType, false},
    {"w6", kInvalidType, false},  {"w7", kInvalidType, false},   {"w8", kInvalidType, false},
    {"w9", kInvalidType, false},  {"w10", kInvalidType, false},  {"w11", kInvalidType, false},
    {"w12", kInvalidType, false}, {"w13", kInvalidType, false},  {"w14", kInvalidType, false},
    {"w15", kInvalidType, false}, {"kill", kInvalidType, false}, {"spawn", kInvalidType, false},
};

[[nodiscard]] bool is_vector(TypeId type) noexcept {
    return type == Float2 || type == Float3 || type == Float4;
}

[[nodiscard]] u32 component_count(TypeId type) noexcept {
    return type < VfxTypeCount ? kTypes[type].components : 0U;
}

/// The result of an elementwise operation. A scalar beside a vector broadcasts, which is the one
/// implicit conversion this lattice has and the only one an artist expects.
[[nodiscard]] Expected<TypeId, Error> elementwise(Span<const TypeId> operands) noexcept {
    TypeId result = operands[0];
    for (const TypeId operand : operands) {
        if (operand == result) {
            continue;
        }
        if (result == Float && is_vector(operand)) {
            result = operand;
            continue;
        }
        if (operand == Float && is_vector(result)) {
            continue;
        }
        return fail(ErrorCode::InvalidArgument,
                    "vfx: an elementwise operation was given operands of incompatible types");
    }
    return result;
}

[[nodiscard]] f32 component_of(const Immediate& value, u32 index) noexcept {
    switch (index) {
        case 0:
            return value.x;
        case 1:
            return value.y;
        case 2:
            return value.z;
        default:
            return value.w;
    }
}

void set_component(Immediate& value, u32 index, f32 component) noexcept {
    switch (index) {
        case 0:
            value.x = component;
            break;
        case 1:
            value.y = component;
            break;
        case 2:
            value.z = component;
            break;
        default:
            value.w = component;
            break;
    }
}

/// Elementwise arithmetic on two immediates, broadcasting a scalar. The one place a folded value is
/// computed, so the CPU executor and the folder cannot disagree — `runtime.cpp` calls it too.
[[nodiscard]] bool fold_elementwise(OpId op, const Immediate& a, const Immediate& b, u32 components,
                                    bool a_scalar, bool b_scalar, Immediate& out) noexcept {
    for (u32 index = 0; index < components; ++index) {
        const f32 left = component_of(a, a_scalar ? 0U : index);
        const f32 right = component_of(b, b_scalar ? 0U : index);
        f32 result = 0.0F;
        switch (op) {
            case Add:
                result = left + right;
                break;
            case Sub:
                result = left - right;
                break;
            case Mul:
                result = left * right;
                break;
            case Div:
                if (right == 0.0F) {
                    return false;
                }
                result = left / right;
                break;
            case Min:
                result = left < right ? left : right;
                break;
            case Max:
                result = left > right ? left : right;
                break;
            default:
                return false;
        }
        set_component(out, index, result);
    }
    return true;
}

/// Is every component of `value` equal to `target`?
[[nodiscard]] bool all_components(const Immediate& value, u32 components, f32 target) noexcept {
    for (u32 index = 0; index < components; ++index) {
        if (component_of(value, index) != target) {
            return false;
        }
    }
    return true;
}

class VfxDomain final : public Domain {
public:
    [[nodiscard]] std::string_view domain_name() const noexcept override { return "cy.vfx"; }
    [[nodiscard]] Span<const TypeDesc> types() const noexcept override {
        return {kTypes, VfxTypeCount};
    }
    [[nodiscard]] Span<const OpDesc> ops() const noexcept override { return {kOps, VfxOpCount}; }
    [[nodiscard]] Span<const RootDecl> roots() const noexcept override {
        return {kRoots, kVfxRootCount};
    }
    [[nodiscard]] OpId constant_op() const noexcept override { return Constant; }

    [[nodiscard]] Expected<TypeId, Error> result_type(
        const TypeQuery& query) const noexcept override {
        switch (query.op) {
            // Every leaf and every sampler is typed by its HINT: a constant's literal, a
            // parameter's declaration, an attribute's declaration, an interface field's type. There
            // is nothing to derive from operands because they have none that decide a type.
            case Constant:
            case Parameter:
            case Attribute:
            case EmitterInput:
            case Random:
            case Sample:
            case Curve:
            case Noise:
                return query.hint == kInvalidType ? static_cast<TypeId>(Float) : query.hint;
            case Dot:
            case Length:
                return static_cast<TypeId>(Float);
            case Cross:
                return static_cast<TypeId>(Float3);
            case Less:
            case Greater:
            case LogicalAnd:
            case LogicalOr:
            case LogicalNot:
                return static_cast<TypeId>(Bool);
            case MakeVec3:
                return static_cast<TypeId>(Float3);
            case MakeVec4:
                return static_cast<TypeId>(Float4);
            case Swizzle:
                return static_cast<TypeId>(Float);
            case Select:
                // The condition is operand zero and is not part of the result type: `select` is a
                // value, and its type is the type of the arms.
                return query.operands.size() == 3 ? query.operands[1] : query.operands[0];
            // Elementwise over one operand, or over the first of several: the result is what
            // operand zero was. `lerp` is here because its `t` broadcasts against `a` and `b`.
            case Lerp:
            case Pow:
            case Normalize:
            case Sin:
            case Cos:
            case Saturate:
                return query.operands.empty() ? static_cast<TypeId>(Float) : query.operands[0];
            default:
                break;
        }
        if (query.operands.empty()) {
            return fail(ErrorCode::InvalidArgument, "vfx: an arithmetic operation needs operands");
        }
        return elementwise(query.operands);
    }

    [[nodiscard]] FoldOutcome fold(const graph::Builder& out, OpId op, TypeId type,
                                   const Immediate& /*value*/,
                                   Span<const NodeId> operands) const noexcept override {
        FoldOutcome outcome;
        if (op < Add || op > Max || operands.size() != 2) {
            return outcome;
        }
        const graph::Node& left = out.node(operands[0]);
        const graph::Node& right = out.node(operands[1]);
        const u32 components = component_count(type);
        if (components == 0) {
            return outcome;
        }

        if (left.op == Constant && right.op == Constant) {
            Immediate folded;
            if (fold_elementwise(op, left.value, right.value, components,
                                 component_count(left.type) == 1U,
                                 component_count(right.type) == 1U, folded)) {
                outcome.kind = FoldOutcome::Kind::Constant;
                outcome.value = folded;
                outcome.type = type;
            }
            return outcome;
        }

        // The algebraic identities constant folding exposes. Only where the surviving operand
        // already has the result type: replacing `float3 * 1.0` with the scalar `1.0`'s partner is
        // correct, but replacing `float * float3(1)` with the scalar would change the type.
        const auto identity = [&](const graph::Node& constant, const graph::Node& other,
                                  NodeId other_id, f32 unit, f32 zero_is_absorbing) noexcept {
            if (constant.op != Constant || other.type != type) {
                return;
            }
            const u32 constant_components = component_count(constant.type);
            if (all_components(constant.value, constant_components, unit)) {
                outcome.kind = FoldOutcome::Kind::Replace;
                outcome.node = other_id;
                return;
            }
            if (zero_is_absorbing != 0.0F &&
                all_components(constant.value, constant_components, 0.0F)) {
                outcome.kind = FoldOutcome::Kind::Constant;
                outcome.value = Immediate{};
                outcome.type = type;
            }
        };

        if (op == Mul) {
            identity(left, right, operands[1], 1.0F, 1.0F);
            if (outcome.kind == FoldOutcome::Kind::None) {
                identity(right, left, operands[0], 1.0F, 1.0F);
            }
        } else if (op == Add) {
            identity(left, right, operands[1], 0.0F, 0.0F);
            if (outcome.kind == FoldOutcome::Kind::None) {
                identity(right, left, operands[0], 0.0F, 0.0F);
            }
        } else if (op == Sub || op == Div) {
            // Only the right identity: `x - 0` and `x / 1` are `x`, and neither is commutative.
            identity(right, left, operands[0], op == Sub ? 0.0F : 1.0F, 0.0F);
        }
        return outcome;
    }
};

const VfxDomain& domain_instance() noexcept {
    static const VfxDomain instance;
    return instance;
}

}  // namespace

const char* vfx_type_name(TypeId type) noexcept {
    return type < VfxTypeCount ? kTypes[type].name : "?";
}

TypeId vfx_type_from_name(Name name) noexcept {
    const std::string_view text = name.text();
    for (TypeId type = 0; type < VfxTypeCount; ++type) {
        if (text == kTypes[type].name) {
            return type;
        }
    }
    // `vec3` and `float3` are the same type spelled two ways: an artist writes one and a shader
    // writes the other, and refusing either would only teach which table the author read.
    if (text == "vec2") {
        return Float2;
    }
    if (text == "vec3") {
        return Float3;
    }
    if (text == "vec4" || text == "color") {
        return Float4;
    }
    return kInvalidType;
}

const char* vfx_op_name(OpId op) noexcept {
    return op < VfxOpCount ? kOps[op].name : "?";
}

const Domain& vfx_domain() noexcept {
    return domain_instance();
}

bool fold_elementwise_public(OpId op, const Immediate& a, const Immediate& b, u32 components,
                             bool a_scalar, bool b_scalar, Immediate& out) noexcept {
    return fold_elementwise(op, a, b, components, a_scalar, b_scalar, out);
}

f32 immediate_component(const Immediate& value, u32 index) noexcept {
    return component_of(value, index);
}

void set_immediate_component(Immediate& value, u32 index, f32 component) noexcept {
    set_component(value, index, component);
}

u32 vfx_type_components(TypeId type) noexcept {
    return component_count(type);
}

VfxKernel::VfxKernel(Allocator& allocator, const Domain& domain) noexcept
    : expressions_(allocator, domain),
      writes_(allocator),
      events_(allocator),
      program_(allocator),
      write_slots_(allocator),
      event_slots_(allocator) {}

}  // namespace cy::vfx
