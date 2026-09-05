// The rig graph compiler and the custom-node registry. See cy/servers/camera/rig.h.
//
// The compile is a topological walk from the output node backwards, with three-colour marking so a
// cycle is reported rather than looped over. `camera-system` requires graphs to be "**compiled** at
// cook time into a compact rig program"; what that buys at runtime is in rig_evaluate.cpp, and what
// it buys here is that every structural mistake — a dangling edge, a cycle, a node that reaches
// nothing, a `Follow` with no `Target` before it — is reported once, naming the node, instead of
// producing a camera that is subtly in the wrong place.

#include <cy/servers/camera/rig.h>

#include <cy/core/base/assert.h>

namespace cy::camera {
namespace {

/// Kinds that read `RigFrame::anchor`, and therefore need a `Target` node earlier in the program.
///
/// Written as a function rather than a table so that adding a kind to the enum without deciding
/// whether it needs an anchor is a compiler warning about an unhandled case rather than a silent
/// `false`.
[[nodiscard]] bool needs_anchor(RigNodeKind kind) noexcept {
    switch (kind) {
        case RigNodeKind::Follow:
        case RigNodeKind::Orbit:
        case RigNodeKind::LookAt:
        case RigNodeKind::Constraint:
        case RigNodeKind::Collision:
            return true;
        case RigNodeKind::Target:
        case RigNodeKind::Offset:
        case RigNodeKind::Lens:
        case RigNodeKind::Noise:
        case RigNodeKind::Output:
        case RigNodeKind::Custom:
        case RigNodeKind::Count:
            return false;
    }
    return false;
}

enum class Mark : u8 { Unvisited = 0, OnPath, Done };

/// Copy the authored parameters into the op. One function so that a kind added to `RigNodeDesc`
/// and forgotten here shows up as a parameter block of defaults rather than as a compile error
/// nobody sees — which is why `tests/test_rig.cpp` round-trips every parameter block.
void fill_op(const RigNodeDesc& node, RigOp& op) noexcept {
    op.kind = node.kind;
    op.id = node.id;
    op.target = node.target;
    op.follow = node.follow;
    op.orbit = node.orbit;
    op.offset = node.offset;
    op.look_at = node.look_at;
    op.lens = node.lens;
    op.noise = node.noise;
    op.constraint = node.constraint;
    op.collision = node.collision;
}

/// The compile's working state. A struct rather than eight parameters threaded through the walk.
struct Compiler {
    const RigDefinition& definition;
    const RigNodeRegistry& registry;
    Array<Mark>& marks;
    Array<u32>& order;

    [[nodiscard]] Expected<u32, Error> index_of(Name id) const noexcept {
        for (usize i = 0; i < definition.nodes.size(); ++i) {
            if (definition.nodes[i].id == id) {
                return static_cast<u32>(i);
            }
        }
        return fail(ErrorCode::NotFound, "rig node names an input that does not exist");
    }

    /// Depth-first from `index` toward the source, appending in evaluation order.
    [[nodiscard]] Status visit(u32 index) noexcept;
};

Status Compiler::visit(u32 index) noexcept {
    if (marks[index] == Mark::Done) {
        return ok();
    }
    if (marks[index] == Mark::OnPath) {
        return fail(ErrorCode::InvalidArgument, "rig graph contains a cycle");
    }
    marks[index] = Mark::OnPath;

    const RigNodeDesc& node = definition.nodes[index];
    if (!node.input.is_empty()) {
        const Expected<u32, Error> input = index_of(node.input);
        if (!input) {
            return make_unexpected(input.error());
        }
        if (Status visited = visit(*input); !visited) {
            return visited;
        }
    }

    marks[index] = Mark::Done;
    return order.push_back(index);
}

}  // namespace

const char* rig_node_kind_name(RigNodeKind kind) noexcept {
    switch (kind) {
        case RigNodeKind::Target:
            return "target";
        case RigNodeKind::Follow:
            return "follow";
        case RigNodeKind::Orbit:
            return "orbit";
        case RigNodeKind::Offset:
            return "offset";
        case RigNodeKind::LookAt:
            return "look-at";
        case RigNodeKind::Lens:
            return "lens";
        case RigNodeKind::Noise:
            return "noise";
        case RigNodeKind::Constraint:
            return "constraint";
        case RigNodeKind::Collision:
            return "collision";
        case RigNodeKind::Output:
            return "output";
        case RigNodeKind::Custom:
            return "custom";
        case RigNodeKind::Count:
            break;
    }
    return "unknown";
}

const char* collision_response_name(CollisionResponse response) noexcept {
    switch (response) {
        case CollisionResponse::PullIn:
            return "pull-in";
        case CollisionResponse::Slide:
            return "slide";
        case CollisionResponse::SwapShoulder:
            return "swap-shoulder";
        case CollisionResponse::FadeObstacle:
            return "fade-obstacle";
        case CollisionResponse::Ignore:
            return "ignore";
        case CollisionResponse::Count:
            break;
    }
    return "unknown";
}

// --- The registry -------------------------------------------------------------------------------

Status RigNodeRegistry::register_kind(Name name, RigCustomEvalFn eval, void* user) noexcept {
    if (name.is_empty() || eval == nullptr) {
        return fail(ErrorCode::InvalidArgument, "a custom rig node needs a name and an evaluation");
    }
    if (opcode(name) != 0) {
        return fail(ErrorCode::AlreadyExists, "a custom rig node kind of that name is registered");
    }
    // Opcodes are one-based positions in this array, so zero means "not a custom node" in a zeroed
    // op and an opcode is resolved without a search.
    if (entries_.size() >= 0xFFFEU) {
        return fail(ErrorCode::OutOfRange, "too many custom rig node kinds");
    }
    return entries_.push_back(Entry{name, eval, user});
}

u16 RigNodeRegistry::opcode(Name name) const noexcept {
    for (usize i = 0; i < entries_.size(); ++i) {
        if (entries_[i].name == name) {
            return static_cast<u16>(i + 1U);
        }
    }
    return 0;
}

Name RigNodeRegistry::name_of(u16 opcode) const noexcept {
    if (opcode == 0 || opcode > entries_.size()) {
        return Name{};
    }
    return entries_[opcode - 1U].name;
}

void RigNodeRegistry::evaluate(u16 opcode, const RigOp& op, const RigEvaluationInput& input,
                               RigFrame& frame) const noexcept {
    if (opcode == 0 || opcode > entries_.size()) {
        return;
    }
    const Entry& entry = entries_[opcode - 1U];
    entry.eval(op, input, frame, entry.user);
}

// --- The compile --------------------------------------------------------------------------------

Expected<RigProgram, Error> compile(const RigDefinition& definition,
                                    const RigNodeRegistry& registry,
                                    Allocator& allocator) noexcept {
    if (definition.nodes.empty()) {
        return fail(ErrorCode::InvalidArgument, "a camera definition has no nodes");
    }

    // Exactly one output, and no two nodes sharing an identity. Both checked before the walk, so a
    // malformed definition is reported as what it is rather than as a dangling edge.
    u32 output_index = 0;
    u32 output_count = 0;
    for (usize i = 0; i < definition.nodes.size(); ++i) {
        const RigNodeDesc& node = definition.nodes[i];
        if (node.id.is_empty()) {
            return fail(ErrorCode::InvalidArgument, "a rig node has no identity");
        }
        for (usize j = 0; j < i; ++j) {
            if (definition.nodes[j].id == node.id) {
                return fail(ErrorCode::AlreadyExists, "two rig nodes share one identity");
            }
        }
        if (node.kind == RigNodeKind::Output) {
            output_index = static_cast<u32>(i);
            ++output_count;
        }
    }
    if (output_count != 1) {
        return fail(ErrorCode::InvalidArgument,
                    "a camera definition has exactly one output node, and this one does not");
    }

    Array<Mark> marks(allocator);
    if (Status sized = marks.resize(definition.nodes.size()); !sized) {
        return make_unexpected(sized.error());
    }
    for (Mark& mark : marks) {
        mark = Mark::Unvisited;
    }

    Array<u32> order(allocator);
    if (Status reserved = order.reserve(definition.nodes.size()); !reserved) {
        return make_unexpected(reserved.error());
    }

    Compiler compiler{definition, registry, marks, order};
    if (Status walked = compiler.visit(output_index); !walked) {
        return make_unexpected(walked.error());
    }

    // A node the walk did not reach contributes nothing. Reported rather than dropped: a node that
    // was authored and is silently not executed is the hardest kind of camera bug to see, because
    // the camera looks *almost* right.
    if (order.size() != definition.nodes.size()) {
        return fail(ErrorCode::InvalidArgument,
                    "a rig node does not reach the output node and would never be evaluated");
    }

    RigProgram program(allocator);
    if (Status reserved = program.ops.reserve(order.size()); !reserved) {
        return make_unexpected(reserved.error());
    }

    bool saw_target = false;
    for (const u32 index : order) {
        const RigNodeDesc& node = definition.nodes[index];
        if (node.kind == RigNodeKind::Target) {
            saw_target = true;
        } else if (needs_anchor(node.kind) && !saw_target) {
            return fail(ErrorCode::InvalidArgument,
                        "a rig node that frames a target is evaluated before any target node");
        }

        RigOp op;
        fill_op(node, op);
        if (node.kind == RigNodeKind::Custom) {
            op.custom_op = registry.opcode(node.custom_kind);
            if (op.custom_op == 0) {
                return fail(ErrorCode::NotFound,
                            "a custom rig node names a kind that is not registered");
            }
        }
        if (node.kind == RigNodeKind::Collision) {
            program.emits_queries = true;
        }
        if (Status pushed = program.ops.push_back(op); !pushed) {
            return make_unexpected(pushed.error());
        }
    }

    program.has_target = saw_target;
    return program;
}

}  // namespace cy::camera
