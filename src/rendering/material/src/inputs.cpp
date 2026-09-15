// Which of a material's inputs are textures and which are constants. M11.c task 1.2.
//
// The whole of the analysis is one downward walk per closure operand, and the ordering of
// `InputBinding` is the rule: the strongest thing reachable from an input is what supplies it.

#include <cy/rendering/material/inputs.h>

#include <algorithm>

namespace cy::rendering::material {

namespace {

/// The operand names of every leaf closure, in operand order. `ir.h` fixes both the arity and the
/// meaning — `Specular` is `(colour, roughness)` — and this is that table written down where a
/// report can read it.
struct ClosureInputs {
    Op op = Op::Diffuse;
    const char* prefix = "";
    const char* operands[2] = {nullptr, nullptr};
};

constexpr ClosureInputs kClosureInputs[] = {
    {Op::Diffuse, "diffuse", {"colour", nullptr}},
    {Op::Specular, "specular", {"colour", "roughness"}},
    {Op::Coat, "coat", {"roughness", nullptr}},
    {Op::Transmission, "transmission", {"colour", nullptr}},
    {Op::Subsurface, "subsurface", {"colour", nullptr}},
    {Op::Sheen, "sheen", {"colour", nullptr}},
    {Op::Emission, "emission", {"colour", nullptr}},
};

/// The fixed spellings, so `MaterialInput::name` is a literal with a static lifetime rather than a
/// buffer a report would have to own. Fourteen of them, which is the closure vocabulary.
constexpr const char* kInputNames[] = {
    "diffuse.colour", "specular.colour",     "specular.roughness",
    "coat.roughness", "transmission.colour", "subsurface.colour",
    "sheen.colour",   "emission.colour",     "opacity",
};

[[nodiscard]] const char* input_name(Op op, u32 operand) noexcept {
    switch (op) {
        case Op::Diffuse:
            return kInputNames[0];
        case Op::Specular:
            return operand == 0 ? kInputNames[1] : kInputNames[2];
        case Op::Coat:
            return kInputNames[3];
        case Op::Transmission:
            return kInputNames[4];
        case Op::Subsurface:
            return kInputNames[5];
        case Op::Sheen:
            return kInputNames[6];
        case Op::Emission:
            return kInputNames[7];
        default:
            return "";
    }
}

/// Raise an input's binding to `candidate` when that is the stronger answer.
///
/// The ordering of `InputBinding` IS the rule — see inputs.h — so "what supplies this input" is a
/// maximum over everything the walk reaches, and this is that maximum spelled once.
void raise(InputBinding& binding, InputBinding candidate) noexcept {
    binding = std::max(binding, candidate);
}

/// What supplies `root`, and the first texture reaching it in name order.
///
/// A WALK RATHER THAN A LOOK AT THE NODE ITSELF, because the node at an input is almost never the
/// leaf: `diffuse.colour` is a `Mul` whose operands are a swizzled sample and a parameter, and
/// asking the `Mul` what it is would answer "arithmetic" about every material ever written.
[[nodiscard]] Status classify_one(const Module& module, NodeId root, MaterialInput& out) noexcept {
    Array<NodeId> order(module.allocator());
    if (Status walked = canonical_order(module, Span<const NodeId>(&root, 1), order); !walked) {
        return walked;
    }
    out.value = root;
    out.binding = InputBinding::Constant;
    for (const NodeId id : order) {
        const Node& node = module.node(id);
        switch (node.op) {
            case Op::TextureSample:
                raise(out.binding, InputBinding::Texture);
                // The first in NAME order, so two runs of one material name the same map. Node ids
                // are construction order and would not be stable across front-ends.
                if (out.texture.is_empty() || node.symbol.text() < out.texture.text()) {
                    out.texture = node.symbol;
                }
                break;
            case Op::Parameter:
                raise(out.binding, InputBinding::Parameter);
                break;
            // An attribute or a field is the mesh or the world speaking. `Custom` is Slang the
            // compiler cannot see into and is the SAME answer deliberately: it is not a constant —
            // an author put an expression there precisely because it varies — and calling it one
            // would be the report overstating in the direction this whole file exists to stop.
            case Op::Attribute:
            case Op::Field:
            case Op::Custom:
                raise(out.binding, InputBinding::Varying);
                break;
            default:
                break;
        }
    }
    return ok();
}

[[nodiscard]] Status record(InputReport& report, const MaterialInput& input) noexcept {
    if (Status pushed = report.inputs.push_back(input); !pushed) {
        return pushed;
    }
    switch (input.binding) {
        case InputBinding::Constant:
            ++report.constants;
            break;
        case InputBinding::Varying:
            ++report.varying;
            break;
        case InputBinding::Parameter:
            ++report.parameters;
            break;
        case InputBinding::Texture:
            ++report.textures;
            break;
    }
    return ok();
}

}  // namespace

const char* input_binding_name(InputBinding binding) noexcept {
    switch (binding) {
        case InputBinding::Constant:
            return "constant";
        case InputBinding::Varying:
            return "varying";
        case InputBinding::Parameter:
            return "parameter";
        case InputBinding::Texture:
            return "texture";
    }
    return "constant";
}

Expected<InputReport, Error> classify_inputs(const Module& module) noexcept {
    InputReport report(module.allocator());

    const NodeId surface = module.surface();
    if (surface != kInvalidNode) {
        Array<NodeId> order(module.allocator());
        if (Status walked = canonical_order(module, Span<const NodeId>(&surface, 1), order);
            !walked) {
            return make_unexpected(walked.error());
        }
        for (const NodeId id : order) {
            const Op op = module.node(id).op;
            if (!op_is_leaf_closure(op)) {
                continue;
            }
            const Span<const NodeId> operands = module.operands(id);
            for (const ClosureInputs& entry : kClosureInputs) {
                if (entry.op != op) {
                    continue;
                }
                for (u32 index = 0; index < operands.size(); ++index) {
                    if (entry.operands[index] == nullptr) {
                        continue;
                    }
                    MaterialInput input;
                    input.name = input_name(op, index);
                    if (Status done = classify_one(module, operands[index], input); !done) {
                        return make_unexpected(done.error());
                    }
                    if (Status kept = record(report, input); !kept) {
                        return make_unexpected(kept.error());
                    }
                }
            }
        }
    }

    // OPACITY IS AN INPUT AND IT IS THE ONE A SHADOW PROGRAM IS MADE OF. A module whose opacity
    // folded to the constant one has none — that is `Opaque`, a structural fact the derivation
    // reads, and inventing a constant input for it would count a value that does not exist.
    if (module.opacity() != kInvalidNode) {
        MaterialInput input;
        input.name = kInputNames[8];
        if (Status done = classify_one(module, module.opacity(), input); !done) {
            return make_unexpected(done.error());
        }
        if (Status kept = record(report, input); !kept) {
            return make_unexpected(kept.error());
        }
    }
    return report;
}

}  // namespace cy::rendering::material
