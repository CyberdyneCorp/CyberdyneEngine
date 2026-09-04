#include <cy/runtime/frame_commands.h>

#include <cy/scene/node.h>
#include <cy/scene/tree.h>

#include <utility>

namespace cy::runtime {

const char* frame_command_kind_name(FrameCommandKind kind) noexcept {
    switch (kind) {
        case FrameCommandKind::ReparentNode:
            return "reparent-node";
        case FrameCommandKind::DestroyNode:
            return "destroy-node";
        case FrameCommandKind::UnloadScene:
            return "unload-scene";
        case FrameCommandKind::Call:
            return "call";
    }
    return "unknown";
}

Status FrameCommandQueue::record(FrameCommand command) noexcept {
    command.sequence = next_sequence_++;
    return commands_.push_back(command);
}

Status FrameCommandQueue::reparent(ecs::Entity node, ecs::Entity parent, u32 order) noexcept {
    FrameCommand command;
    command.kind = FrameCommandKind::ReparentNode;
    command.order = order;
    command.subject = node;
    command.parent = parent;
    return record(command);
}

Status FrameCommandQueue::destroy_node(ecs::Entity node, u32 order) noexcept {
    FrameCommand command;
    command.kind = FrameCommandKind::DestroyNode;
    command.order = order;
    command.subject = node;
    return record(command);
}

Status FrameCommandQueue::unload_scene(scene::SceneId scene, u32 order) noexcept {
    FrameCommand command;
    command.kind = FrameCommandKind::UnloadScene;
    command.order = order;
    command.scene = scene;
    return record(command);
}

Status FrameCommandQueue::call(FrameCommandFn function, void* user, u32 order) noexcept {
    if (function == nullptr) {
        return fail(ErrorCode::InvalidArgument, "a deferred call needs a function");
    }
    FrameCommand command;
    command.kind = FrameCommandKind::Call;
    command.order = order;
    command.call = function;
    command.user = user;
    return record(command);
}

FrameFlushReport FrameCommandQueue::flush(scene::SceneTree& tree) noexcept {
    FrameFlushReport report;
    if (commands_.empty()) {
        return report;
    }

    // Swapped rather than moved, so that the capacity the queue has grown to survives the flush and
    // a steady-state frame allocates nothing. `commands_` is empty afterwards, which is what lets a
    // command record another one without extending the loop it is inside.
    std::swap(commands_, draining_);

    // (order, sequence). Insertion sort because the key is already almost sorted — sequences are
    // assigned monotonically and orders are usually equal — so this is a linear pass in the common
    // case and never allocates. Worker identity appears nowhere in the key;
    // `simulation-and-determinism` forbids it, and there is nothing here that could supply one.
    FrameCommand* data = draining_.data();
    for (usize i = 1; i < draining_.size(); ++i) {
        for (usize j = i; j > 0; --j) {
            const FrameCommand& left = data[j - 1];
            const FrameCommand& right = data[j];
            const bool out_of_order = right.order < left.order ||
                                      (right.order == left.order && right.sequence < left.sequence);
            if (!out_of_order) {
                break;
            }
            const FrameCommand held = data[j - 1];
            data[j - 1] = data[j];
            data[j] = held;
        }
    }

    for (const FrameCommand& command : draining_) {
        Status applied = ok();
        switch (command.kind) {
            case FrameCommandKind::ReparentNode:
                applied = tree.node(command.subject).set_parent(tree.node(command.parent));
                break;
            case FrameCommandKind::DestroyNode:
                applied = tree.node(command.subject).destroy();
                break;
            case FrameCommandKind::UnloadScene:
                applied = tree.unload(command.scene);
                break;
            case FrameCommandKind::Call:
                applied = command.call(tree, command.user);
                break;
        }
        if (applied) {
            ++report.applied;
            ++total_applied_;
            continue;
        }
        // Every command is attempted. Stopping at the first failure would leave the rest pending
        // into the next frame, which turns one failure into a growing backlog.
        ++report.failed;
        if (report.first_error) {
            report.first_error = applied;
        }
    }

    draining_.clear();
    return report;
}

}  // namespace cy::runtime
