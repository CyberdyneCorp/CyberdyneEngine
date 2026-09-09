// The material anchor domain: tables, typing, folding and the closure algebra. Task 2.3.
//
// Every rule here is `src/rendering/material/`'s, re-expressed against `cy::graph::Domain`. Nothing
// is improved and nothing is simplified: the point of the exercise is that the generalised core
// reproduces three measured digests, and a "better" rule would answer a different question.

#include "material_anchor.h"

#include <cmath>

namespace cy::graph::anchor {
namespace {

[[nodiscard]] Error invalid(const char* message) noexcept {
    return Error{ErrorCode::InvalidArgument, message, 0};
}

constexpr TypeDesc kTypes[] = {
    {"float", 1, true, pinned_identity(Float), true},
    {"float2", 2, true, pinned_identity(Vec2), true},
    {"float3", 3, true, pinned_identity(Vec3), true},
    {"float4", 4, true, pinned_identity(Vec4), true},
    {"int", 1, true, pinned_identity(Int), true},
    {"bool", 1, true, pinned_identity(Bool), true},
    {"closure", 0, false, pinned_identity(Closure), true},
};
static_assert(sizeof(kTypes) / sizeof(kTypes[0]) == static_cast<usize>(TypeCount));

/// One row of the operation table. Written as a helper rather than as 31 aggregate initialisers so
/// that the five booleans cannot be transposed silently.
consteval OpDesc leaf_op(const char* name, OpId identity, bool varying) {
    OpDesc desc;
    desc.name = name;
    desc.arity = 0;
    desc.inline_leaf = true;
    desc.varying = varying;
    desc.uniform_leaf = !varying;
    desc.merge = MergeClass::Leaf;
    desc.category = OpCategory::Leaf;
    desc.identity = pinned_identity(identity);
    desc.pinned = true;
    return desc;
}

consteval OpDesc value_op(const char* name, OpId identity, u32 arity, bool commutative,
                          OpCategory category = OpCategory::Arithmetic, bool varying = false) {
    OpDesc desc;
    desc.name = name;
    desc.arity = arity;
    desc.commutative = commutative;
    desc.varying = varying;
    desc.merge = category == OpCategory::Sample ? MergeClass::Sample : MergeClass::Expression;
    desc.category = category;
    desc.identity = pinned_identity(identity);
    desc.pinned = true;
    return desc;
}

consteval OpDesc variadic_op(const char* name, OpId identity, u32 minimum, bool commutative,
                             OpCategory category = OpCategory::Arithmetic, bool varying = false) {
    OpDesc desc = value_op(name, identity, 2, commutative, category, varying);
    desc.arity = kVariadic;
    desc.min_operands = minimum;
    desc.max_operands = kMaxOperands;
    return desc;
}

consteval OpDesc closure_op(const char* name, OpId identity, u32 arity, bool commutative,
                            bool leaf) {
    OpDesc desc = value_op(name, identity, arity, commutative, OpCategory::Aggregate);
    desc.aggregate = true;
    desc.leaf_aggregate = leaf;
    return desc;
}

consteval OpDesc variadic_closure_op(const char* name, OpId identity) {
    OpDesc desc = closure_op(name, identity, 2, true, false);
    desc.arity = kVariadic;
    desc.min_operands = 2;
    desc.max_operands = kMaxOperands;
    return desc;
}

constexpr OpDesc kOps[] = {
    leaf_op("const", OpConstant, false),
    leaf_op("param", OpParameter, false),
    leaf_op("attr", OpAttribute, true),
    leaf_op("field", OpField, true),
    value_op("sample", OpTextureSample, 1, false, OpCategory::Sample, true),
    value_op("add", OpAdd, 2, true),
    value_op("sub", OpSub, 2, false),
    value_op("mul", OpMul, 2, true),
    value_op("div", OpDiv, 2, false),
    value_op("min", OpMin, 2, true),
    value_op("max", OpMax, 2, true),
    value_op("dot", OpDot, 2, false),
    value_op("pow", OpPow, 2, false),
    value_op("saturate", OpSaturate, 1, false),
    value_op("one_minus", OpOneMinus, 1, false),
    value_op("normalize", OpNormalize, 1, false),
    value_op("lerp", OpLerp, 3, false),
    value_op("select", OpSelect, 3, false, OpCategory::Branch),
    value_op("swizzle", OpSwizzle, 1, false),
    variadic_op("combine", OpCombine, 2, false),
    variadic_op("custom", OpCustom, 1, false, OpCategory::Arithmetic, true),
    closure_op("diffuse", OpDiffuse, 1, false, true),
    closure_op("specular", OpSpecular, 2, false, true),
    closure_op("coat", OpCoat, 1, false, true),
    closure_op("transmission", OpTransmission, 1, false, true),
    closure_op("subsurface", OpSubsurface, 1, false, true),
    closure_op("sheen", OpSheen, 1, false, true),
    closure_op("emission", OpEmission, 1, false, true),
    closure_op("closure_scale", OpClosureScale, 2, false, false),
    variadic_closure_op("closure_add", OpClosureAdd),
    closure_op("closure_layer", OpClosureLayer, 2, false, false),
};
static_assert(sizeof(kOps) / sizeof(kOps[0]) == static_cast<usize>(OpCount));

constexpr RootDecl kRoots[] = {
    {"surface", Closure, false},
    {"opacity", Float, false},
};

[[nodiscard]] bool is_numeric(TypeId type) noexcept {
    return type != Closure && type < TypeCount;
}

[[nodiscard]] bool is_vector(TypeId type) noexcept {
    return type == Vec2 || type == Vec3 || type == Vec4;
}

[[nodiscard]] TypeId vector_of(u32 components) noexcept {
    switch (components) {
        case 1:
            return Float;
        case 2:
            return Vec2;
        case 3:
            return Vec3;
        default:
            return Vec4;
    }
}

[[nodiscard]] u32 components_of(TypeId type) noexcept {
    return type < TypeCount ? kTypes[type].components : 0U;
}

// --- Typing ---------------------------------------------------------------------------------

/// The arithmetic rule, stated once: a scalar promotes against a vector, and anything else must
/// agree. Every binary numeric op shares it, which keeps `float3 * float` legal and `float3 *
/// float2` a diagnostic in one place rather than in nine.
[[nodiscard]] Expected<TypeId, Error> combine_numeric(TypeId left, TypeId right) noexcept {
    if (!is_numeric(left) || !is_numeric(right)) {
        return make_unexpected(invalid("an arithmetic operand may not be a closure"));
    }
    if (left == right) {
        return left;
    }
    if (left == Float && is_vector(right)) {
        return right;
    }
    if (right == Float && is_vector(left)) {
        return left;
    }
    return make_unexpected(invalid("these operand types do not combine"));
}

[[nodiscard]] Status require(bool condition, const char* message) noexcept {
    return condition ? Status{} : Status(make_unexpected(invalid(message)));
}

[[nodiscard]] Expected<TypeId, Error> arithmetic_type(OpId op, const Immediate& value,
                                                      Span<const TypeId> types) noexcept {
    switch (op) {
        case OpAdd:
        case OpSub:
        case OpMul:
        case OpDiv:
        case OpMin:
        case OpMax:
        case OpPow:
            return combine_numeric(types[0], types[1]);
        case OpDot:
            if (Status checked = require(types[0] == types[1] && is_vector(types[0]),
                                         "a dot product takes two vectors of one type");
                !checked) {
                return make_unexpected(checked.error());
            }
            return Float;
        case OpSaturate:
        case OpOneMinus:
        case OpNormalize:
            if (Status checked = require(is_numeric(types[0]), "this operand may not be a closure");
                !checked) {
                return make_unexpected(checked.error());
            }
            return types[0];
        case OpLerp: {
            auto blended = combine_numeric(types[0], types[1]);
            if (!blended) {
                return blended;
            }
            if (Status checked = require(types[2] == Float || types[2] == blended.value(),
                                         "a lerp factor is a float or matches the blended type");
                !checked) {
                return make_unexpected(checked.error());
            }
            return blended;
        }
        case OpSelect:
            if (Status checked = require(types[0] == Bool && types[1] == types[2],
                                         "select takes a bool and two values of one type");
                !checked) {
                return make_unexpected(checked.error());
            }
            return types[1];
        case OpSwizzle: {
            const u32 count = (value.mask >> 28U) & 0xFU;
            if (Status checked = require(count >= 1 && count <= 4 && is_numeric(types[0]),
                                         "a swizzle selects one to four components");
                !checked) {
                return make_unexpected(checked.error());
            }
            return vector_of(count);
        }
        case OpCombine:
            for (const TypeId type : types) {
                if (Status checked =
                        require(type == Float, "combine assembles a vector from floats");
                    !checked) {
                    return make_unexpected(checked.error());
                }
            }
            return vector_of(static_cast<u32>(types.size()));
        default:
            break;
    }
    return make_unexpected(invalid("no such operation"));
}

[[nodiscard]] Expected<TypeId, Error> closure_type(OpId op, Span<const TypeId> types) noexcept {
    switch (op) {
        case OpDiffuse:
        case OpTransmission:
        case OpSubsurface:
        case OpSheen:
        case OpEmission:
            if (Status checked = require(types[0] == Vec3, "a closure's colour is a float3");
                !checked) {
                return make_unexpected(checked.error());
            }
            break;
        case OpSpecular:
            if (Status checked = require(types[0] == Vec3 && types[1] == Float,
                                         "specular takes a colour and a roughness");
                !checked) {
                return make_unexpected(checked.error());
            }
            break;
        case OpCoat:
            if (Status checked = require(types[0] == Float, "a coat takes a roughness"); !checked) {
                return make_unexpected(checked.error());
            }
            break;
        case OpClosureScale:
            if (Status checked = require(types[0] == Closure && types[1] == Float,
                                         "a closure scale takes a closure and a weight");
                !checked) {
                return make_unexpected(checked.error());
            }
            break;
        default:
            for (const TypeId type : types) {
                if (Status checked = require(type == Closure, "this operation combines closures");
                    !checked) {
                    return make_unexpected(checked.error());
                }
            }
            break;
    }
    return Closure;
}

// --- Constant folding ---------------------------------------------------------------------------

/// A constant, unpacked into components so folding is written once rather than per type.
struct Scalars {
    f32 component[4] = {};
    u32 count = 1;

    [[nodiscard]] f32 at(u32 index) const noexcept {
        return count == 1 ? component[0] : component[index];
    }
};

[[nodiscard]] Scalars scalars_of(const Node& node) noexcept {
    Scalars value;
    value.count = components_of(node.type);
    value.component[0] = node.value.x;
    value.component[1] = node.value.y;
    value.component[2] = node.value.z;
    value.component[3] = node.value.w;
    if (node.type == Int || node.type == Bool) {
        value.component[0] = static_cast<f32>(node.value.mask);
    }
    return value;
}

[[nodiscard]] Immediate immediate_of(const Scalars& value) noexcept {
    return Immediate{value.component[0], value.component[1], value.component[2], value.component[3],
                     0};
}

[[nodiscard]] bool all_constant(const Builder& out, Span<const NodeId> operands) noexcept {
    for (const NodeId operand : operands) {
        if (out.node(operand).op != OpConstant) {
            return false;
        }
    }
    return !operands.empty();
}

/// The component-wise arithmetic. Returns false for the cases folding must decline: a division by
/// zero, a `pow` of a negative base. Declining is not a failure — the node stays in the program.
[[nodiscard]] bool fold_components(OpId op, const Scalars* in, u32 arity, u32 components,
                                   Scalars& out) noexcept {
    out.count = components;
    for (u32 index = 0; index < components; ++index) {
        const f32 a = in[0].at(index);
        const f32 b = arity > 1 ? in[1].at(index) : 0.0F;
        switch (op) {
            case OpAdd:
                out.component[index] = a + b;
                break;
            case OpSub:
                out.component[index] = a - b;
                break;
            case OpMul:
                out.component[index] = a * b;
                break;
            case OpDiv:
                if (b == 0.0F) {
                    return false;
                }
                out.component[index] = a / b;
                break;
            case OpMin:
                out.component[index] = a < b ? a : b;
                break;
            case OpMax:
                out.component[index] = a > b ? a : b;
                break;
            case OpPow:
                if (a < 0.0F) {
                    return false;
                }
                out.component[index] = std::pow(a, b);
                break;
            case OpSaturate:
                out.component[index] = a > 1.0F ? 1.0F : a;
                out.component[index] = out.component[index] < 0.0F ? 0.0F : out.component[index];
                break;
            case OpOneMinus:
                out.component[index] = 1.0F - a;
                break;
            case OpLerp:
                out.component[index] = a + ((b - a) * in[2].at(index));
                break;
            default:
                return false;
        }
    }
    return true;
}

/// Folding for the operations whose result is not component-wise.
[[nodiscard]] bool fold_special(OpId op, const Immediate& value, const Scalars* in, u32 arity,
                                Scalars& out) noexcept {
    switch (op) {
        case OpDot: {
            f32 sum = 0.0F;
            for (u32 index = 0; index < in[0].count; ++index) {
                sum += in[0].at(index) * in[1].at(index);
            }
            out.count = 1;
            out.component[0] = sum;
            return true;
        }
        case OpNormalize: {
            f32 sum = 0.0F;
            for (u32 index = 0; index < in[0].count; ++index) {
                sum += in[0].at(index) * in[0].at(index);
            }
            if (sum <= 0.0F) {
                return false;
            }
            const f32 length = std::sqrt(sum);
            out.count = in[0].count;
            for (u32 index = 0; index < out.count; ++index) {
                out.component[index] = in[0].at(index) / length;
            }
            return true;
        }
        case OpSwizzle: {
            out.count = (value.mask >> 28U) & 0xFU;
            for (u32 index = 0; index < out.count; ++index) {
                out.component[index] = in[0].at((value.mask >> (index * 4U)) & 0x3U);
            }
            return true;
        }
        case OpCombine:
            out.count = arity;
            for (u32 index = 0; index < arity; ++index) {
                out.component[index] = in[index].at(0);
            }
            return true;
        default:
            return false;
    }
}

[[nodiscard]] bool fold_constant(const Builder& out, OpId op, TypeId result, const Immediate& value,
                                 Span<const NodeId> operands, Immediate& folded) noexcept {
    Scalars in[kMaxOperands];
    const auto arity = static_cast<u32>(operands.size());
    for (u32 index = 0; index < arity; ++index) {
        in[index] = scalars_of(out.node(operands[index]));
    }
    Scalars computed;
    const bool done = fold_special(op, value, in, arity, computed) ||
                      fold_components(op, in, arity, components_of(result), computed);
    if (!done) {
        return false;
    }
    folded = immediate_of(computed);
    return true;
}

/// The identities constant folding exposes when only ONE operand is constant.
///
/// They matter for the exit criterion rather than for speed: an editor emits a weight port and a
/// scale node on every closure and a text definition does not, so `x * 1` has to disappear or the
/// two front-ends produce different programs.
[[nodiscard]] NodeId apply_identity(const Builder& out, OpId op, Span<const NodeId> operands,
                                    TypeId result) noexcept {
    if (operands.size() != 2) {
        return kInvalidNode;
    }
    const auto is_splat = [&out, result](NodeId id, f32 wanted) noexcept {
        const Node& node = out.node(id);
        if (node.op != OpConstant) {
            return false;
        }
        const Scalars value = scalars_of(node);
        for (u32 index = 0; index < components_of(result); ++index) {
            if (value.at(index) != wanted) {
                return false;
            }
        }
        return true;
    };
    const auto surviving = [&out, result](NodeId id) noexcept {
        return out.node(id).type == result ? id : kInvalidNode;
    };
    switch (op) {
        case OpMul:
            if (is_splat(operands[0], 1.0F)) {
                return surviving(operands[1]);
            }
            if (is_splat(operands[1], 1.0F)) {
                return surviving(operands[0]);
            }
            return kInvalidNode;
        case OpAdd:
            if (is_splat(operands[0], 0.0F)) {
                return surviving(operands[1]);
            }
            if (is_splat(operands[1], 0.0F)) {
                return surviving(operands[0]);
            }
            return kInvalidNode;
        case OpSub:
        case OpDiv:
            return is_splat(operands[1], op == OpSub ? 0.0F : 1.0F) ? surviving(operands[0])
                                                                    : kInvalidNode;
        default:
            return kInvalidNode;
    }
}

// --- The closure algebra --------------------------------------------------------------------
//
// NOTE THE ASYMMETRY: a LEAF closure may vanish, and a combinator may only collapse to something
// that is still there. Dropping a `closure_scale`, `closure_add` or `closure_layer` deletes
// everything beneath it, which is how the material spike's first far-field program came out with
// zero closures.

/// Flatten a sum of sums. `add(add(a, b), c)` and `add(a, b, c)` are one material: an editor builds
/// the first, because a node has two inputs, and a text definition builds the second.
[[nodiscard]] u32 flatten_sum(Builder& out, NodeId* live, u32 count, bool& flattened) noexcept {
    NodeId flat[kMaxOperands] = {};
    u32 size = 0;
    flattened = false;
    for (u32 index = 0; index < count && size < kMaxOperands; ++index) {
        const Span<const NodeId> inner = out.operands(live[index]);
        if (out.node(live[index]).op == OpClosureAdd && size + inner.size() <= kMaxOperands) {
            for (const NodeId operand : inner) {
                flat[size++] = operand;
            }
            flattened = true;
            continue;
        }
        flat[size++] = live[index];
    }
    if (!flattened) {
        return count;
    }
    for (u32 index = 0; index < size; ++index) {
        live[index] = flat[index];
    }
    return size;
}

/// The material domain itself.
class MaterialDomain final : public Domain {
public:
    [[nodiscard]] std::string_view domain_name() const noexcept override {
        return "material.anchor";
    }
    [[nodiscard]] Span<const TypeDesc> types() const noexcept override {
        return {kTypes, static_cast<usize>(TypeCount)};
    }
    [[nodiscard]] Span<const OpDesc> ops() const noexcept override {
        return {kOps, static_cast<usize>(OpCount)};
    }
    [[nodiscard]] Span<const RootDecl> roots() const noexcept override { return {kRoots, 2}; }

    [[nodiscard]] Expected<TypeId, Error> result_type(
        const TypeQuery& query) const noexcept override;
    [[nodiscard]] bool normalise(const Builder& builder, OpId& op, Span<const NodeId> operands,
                                 NodeId* out, u32& out_count) const noexcept override;
    [[nodiscard]] OpId constant_op() const noexcept override { return OpConstant; }
    [[nodiscard]] FoldOutcome fold(const Builder& out, OpId op, TypeId type, const Immediate& value,
                                   Span<const NodeId> operands) const noexcept override;
    [[nodiscard]] Expected<NodeId, Error> simplify_aggregate(
        const Node& node, Span<const NodeId> operands, Builder& out,
        u32& simplifications) const noexcept override;
};

Expected<TypeId, Error> MaterialDomain::result_type(const TypeQuery& query) const noexcept {
    // The leaves first: their type cannot be derived and the caller states it.
    switch (query.op) {
        case OpConstant:
        case OpAttribute:
        case OpField:
        case OpCustom:
            if (query.hint == kInvalidType) {
                return make_unexpected(invalid("this operation's result type must be stated"));
            }
            return query.hint;
        case OpParameter:
            return query.hint;
        case OpTextureSample:
            if (Status checked =
                    require(query.operands[0] == Vec2, "a texture coordinate is a float2");
                !checked) {
                return make_unexpected(checked.error());
            }
            return Vec4;
        default:
            break;
    }
    if (kOps[query.op].aggregate) {
        return closure_type(query.op, query.operands);
    }
    return arithmetic_type(query.op, query.value, query.operands);
}

bool MaterialDomain::normalise(const Builder& builder, OpId& op, Span<const NodeId> operands,
                               NodeId* out, u32& out_count) const noexcept {
    // ONE SPELLING PER OPERATION. An editor emits a "one minus" node and a text definition writes
    // `1 - x`; unless one becomes the other in the only constructor, the two front-ends produce
    // different values for the same material.
    if (op != OpSub || operands.size() != 2) {
        return false;
    }
    const Node& left = builder.node(operands[0]);
    if (left.op != OpConstant || left.type == Bool || left.type == Int) {
        return false;
    }
    const f32 component[4] = {left.value.x, left.value.y, left.value.z, left.value.w};
    for (u32 index = 0; index < components_of(left.type); ++index) {
        if (component[index] != 1.0F) {
            return false;
        }
    }
    // `1 - float3` is a one-minus; `float3(1,1,1) - float` is a subtraction producing a float3 and
    // must stay one.
    const TypeId right = builder.node(operands[1]).type;
    if (left.type != Float && left.type != right) {
        return false;
    }
    op = OpOneMinus;
    out[0] = operands[1];
    out_count = 1;
    return true;
}

FoldOutcome MaterialDomain::fold(const Builder& out, OpId op, TypeId type, const Immediate& value,
                                 Span<const NodeId> operands) const noexcept {
    FoldOutcome outcome;
    Immediate folded;
    if (all_constant(out, operands) && fold_constant(out, op, type, value, operands, folded)) {
        outcome.kind = FoldOutcome::Kind::Constant;
        outcome.value = folded;
        outcome.type = type;
        return outcome;
    }
    const NodeId identity = apply_identity(out, op, operands, type);
    if (identity != kInvalidNode) {
        outcome.kind = FoldOutcome::Kind::Replace;
        outcome.node = identity;
    }
    return outcome;
}

Expected<NodeId, Error> MaterialDomain::simplify_aggregate(const Node& node,
                                                           Span<const NodeId> operands,
                                                           Builder& out,
                                                           u32& simplifications) const noexcept {
    NodeId live[kMaxOperands] = {};
    u32 count = 0;
    for (const NodeId operand : operands) {
        if (operand != kInvalidNode) {
            live[count++] = operand;
        }
    }
    switch (node.op) {
        case OpClosureScale: {
            if (count < 2) {
                ++simplifications;
                return kInvalidNode;
            }
            const Node& weight = out.node(live[1]);
            const bool unit = weight.op == OpConstant && weight.value.x == 1.0F;
            const bool zero = weight.op == OpConstant && weight.value.x == 0.0F;
            if (unit || zero) {
                ++simplifications;
                return unit ? live[0] : kInvalidNode;
            }
            break;
        }
        case OpClosureAdd: {
            bool flattened = false;
            count = flatten_sum(out, live, count, flattened);
            simplifications += flattened ? 1U : 0U;
            if (count < 2) {
                ++simplifications;
                return count == 0 ? kInvalidNode : live[0];
            }
            break;
        }
        case OpClosureLayer:
            if (count < 2) {
                ++simplifications;
                return count == 0 ? kInvalidNode : live[0];
            }
            break;
        default:
            // A leaf closure whose colour was dropped cannot happen: derivation drops closures, and
            // a numeric value is never dropped out from under one.
            if (count != operands.size()) {
                return kInvalidNode;
            }
            break;
    }
    return out.make(node.op, node.type, node.symbol, node.value, Span<const NodeId>(live, count));
}

}  // namespace

const Domain& material_domain() noexcept {
    static const MaterialDomain kDomain;
    return kDomain;
}

u32 swizzle_mask(Span<const u8> components) noexcept {
    u32 mask = 0;
    const u32 count = static_cast<u32>(components.size() > 4 ? 4 : components.size());
    for (u32 index = 0; index < count; ++index) {
        mask |= static_cast<u32>(components[index] & 0x3U) << (index * 4U);
    }
    return mask | (count << 28U);
}

}  // namespace cy::graph::anchor
