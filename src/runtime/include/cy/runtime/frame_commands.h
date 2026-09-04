#pragma once
// The frame-scoped deferred command queue. Task 4.1.3.
//
// `engine-architecture` — "Deferred command queue": a frame-scoped queue for operations that are
// unsafe to perform in the middle of a stage — entity creation and destruction, component add and
// remove, **node reparenting, and scene loading** — applied at defined flush points (after each
// simulation stage and after the frame stage), in submission order.
//
// --- WHY THIS EXISTS BESIDE ecs::CommandBuffer ---------------------------------------------------
//
// The first three of those operations are the ECS's and already have their answer:
// `ecs::CommandBuffer`, one per system, merged at the stage flush in (system, thread, record)
// order. This queue is for the two that are *not* the ECS's — node reparenting, which has to
// renumber `ChildOrder` and run the tree's lifecycle callbacks, and scene loading, which
// instantiates a whole subtree — plus the deferred call that a subsystem needs for work that is
// neither.
//
// Duplicating the ECS's mechanism here would have been the wrong shape: an entity command recorded
// in this queue would be applied *after* the ECS flush and would see a world one flush stale.
// Everything entity-shaped therefore stays in `ecs::CommandBuffer` and this queue holds what sits
// above it. `Simulation` drains them in that order, and the order is stated at the drain.
//
// --- THE THREADING POSITION, STATED PLAINLY ------------------------------------------------------
//
// This queue is **single-producer**: it is recorded into and drained on the tick thread. It is not
// a lock-free multi-producer queue and does not pretend to be.
//
// That is a real constraint and it is the honest one for M2. `ecs::World` is single-threaded for
// structural change by construction — the ECS's own note says so — and every operation this queue
// carries is structural at the scene level, so a worker recording one would be recording work that
// cannot be applied until the flush anyway. What a worker records instead is an
// `ecs::CommandBuffer` entry, which *is* per-worker and *is* merged deterministically.
//
// The `order` key on every entry is what makes the extension cheap when it is needed: entries are
// applied in (order, sequence), and `order` is the recording system's registration index — never a
// worker or thread identity, which `simulation-and-determinism` forbids in an ordering key. Today
// every caller passes the same order and the sort is a no-op; the day there are per-system queues,
// merging them is a sort by a key that already exists.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/array.h>
#include <cy/ecs/entity.h>
#include <cy/scene/scene.h>

namespace cy::scene {
class SceneTree;
}

namespace cy::runtime {

/// What a deferred frame command does.
enum class FrameCommandKind : u8 {
    /// Move a node under a new parent, keeping its world transform. The scene tree's own
    /// `Node::set_parent`, deferred.
    ReparentNode = 0,
    /// Destroy a node and its subtree, with the lifecycle callbacks the tree defines.
    DestroyNode,
    /// Unload a loaded scene.
    UnloadScene,
    /// An arbitrary callback. The escape hatch for a subsystem whose deferred work is neither of
    /// the above — deliberately last, so that a reader sees the named kinds first.
    Call,
};

const char* frame_command_kind_name(FrameCommandKind kind) noexcept;

using FrameCommandFn = Status (*)(scene::SceneTree& tree, void* user) noexcept;

/// One recorded command. A value: the queue holds no pointers into caller storage except `user`,
/// which is the caller's to keep alive until the flush.
struct FrameCommand {
    FrameCommandKind kind = FrameCommandKind::Call;
    /// The recording system's registration index, or zero. Never a worker or thread id — see the
    /// header comment.
    u32 order = 0;
    /// Assigned by `record`, monotonically. The second half of the sort key, and what makes the
    /// merge stable within one order.
    u32 sequence = 0;

    ecs::Entity subject;
    ecs::Entity parent;
    scene::SceneId scene = 0;
    FrameCommandFn call = nullptr;
    void* user = nullptr;
};

/// What a flush did. Returned rather than accumulated internally, because a caller that ignores it
/// should be visibly ignoring something.
struct FrameFlushReport {
    u32 applied = 0;
    u32 failed = 0;
    /// The first failure, or `ok()`. Recorded and reported after the whole queue has been applied:
    /// stopping at the first would leave the rest of the frame's commands pending into the next
    /// frame, which turns one failure into a growing backlog.
    Status first_error = ok();
};

class FrameCommandQueue {
public:
    explicit FrameCommandQueue(Allocator& allocator) noexcept : commands_(allocator) {}

    FrameCommandQueue(const FrameCommandQueue&) = delete;
    FrameCommandQueue& operator=(const FrameCommandQueue&) = delete;

    [[nodiscard]] Status reparent(ecs::Entity node, ecs::Entity parent, u32 order = 0) noexcept;
    [[nodiscard]] Status destroy_node(ecs::Entity node, u32 order = 0) noexcept;
    [[nodiscard]] Status unload_scene(scene::SceneId scene, u32 order = 0) noexcept;
    [[nodiscard]] Status call(FrameCommandFn function, void* user, u32 order = 0) noexcept;

    /// Apply every pending command against `tree`, in (order, sequence), and empty the queue.
    ///
    /// The queue is emptied *before* the commands run, so that a command which records another one
    /// leaves it for the next flush rather than extending the loop it is inside — an unbounded
    /// flush is the one failure mode a deferred queue must not have.
    [[nodiscard]] FrameFlushReport flush(scene::SceneTree& tree) noexcept;

    [[nodiscard]] u32 pending() const noexcept { return static_cast<u32>(commands_.size()); }
    /// Commands applied over the queue's lifetime, for the tick report.
    [[nodiscard]] u64 total_applied() const noexcept { return total_applied_; }

private:
    [[nodiscard]] Status record(FrameCommand command) noexcept;

    Array<FrameCommand> commands_;
    /// Scratch the flush swaps the pending array into, so that a command recorded during the flush
    /// lands in an empty `commands_` rather than in the one being walked.
    Array<FrameCommand> draining_{commands_.allocator()};
    u32 next_sequence_ = 0;
    u64 total_applied_ = 0;
};

}  // namespace cy::runtime
