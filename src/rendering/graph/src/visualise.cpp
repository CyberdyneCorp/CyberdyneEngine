// The plan's dumps and its self-check. Task 2.2.8.

#include <cy/rendering/graph/visualise.h>

#include <cstdarg>
#include <cstdio>

namespace cy::rendering {
namespace {

/// Append a formatted line to `out`. The buffer grows; a failure to grow is the only way this
/// fails, and it stops rather than truncating silently, because a truncated plan dump read as a
/// whole one is worse than no dump.
// NOLINTNEXTLINE(cert-dcl50-cpp) — a formatting sink is variadic by nature; see .clang-tidy.
bool append(Array<char>& out, const char* pattern, ...) noexcept {
    char line[512];
    va_list arguments;
    va_start(arguments, pattern);
    const int written = std::vsnprintf(line, sizeof(line), pattern, arguments);
    va_end(arguments);
    if (written <= 0) {
        return written == 0;
    }
    const auto count =
        static_cast<usize>(written) < sizeof(line) ? static_cast<usize>(written) : sizeof(line) - 1;
    for (usize index = 0; index < count; ++index) {
        if (Status pushed = out.push_back(line[index]); !pushed) {
            return false;
        }
    }
    return true;
}

bool terminate(Array<char>& out) noexcept {
    return out.push_back('\0').has_value();
}

const char* stage_summary(rhi::Stage stage) noexcept {
    // The masks a plan actually contains, named. A general mask decoder would be a table of
    // fourteen strings joined at run time, and every barrier this engine derives comes from one row
    // of the access table, so the interesting cases are few and naming them reads better.
    if (stage == rhi::Stage::None) {
        return "none";
    }
    if (stage == rhi::Stage::AllCommands) {
        return "all-commands";
    }
    if (stage == rhi::Stage::ComputeShader) {
        return "compute";
    }
    if (stage == rhi::Stage::FragmentShader) {
        return "fragment";
    }
    if (stage == rhi::Stage::ColorAttachmentOutput) {
        return "colour-output";
    }
    if (stage == rhi::Stage::Copy) {
        return "copy";
    }
    if (stage == rhi::Stage::Host) {
        return "host";
    }
    if (stage == (rhi::Stage::EarlyFragmentTests | rhi::Stage::LateFragmentTests)) {
        return "depth-tests";
    }
    return "mixed";
}

bool dump_batch(Array<char>& out, const char* indent, const rhi::BarrierBatch& batch,
                const RenderGraph& graph) noexcept {
    for (const rhi::ImageBarrier& barrier : batch.images) {
        const bool transfer = barrier.src_queue_family != barrier.dst_queue_family;
        if (!append(out, "%simage  '%s' %s -> %s  mips[%u,%u) layers[%u,%u)  src %s  dst %s%s\n",
                    indent, graph.resource(barrier.resource).name,
                    rhi::image_layout_name(barrier.old_layout),
                    rhi::image_layout_name(barrier.new_layout), barrier.range.base_mip,
                    barrier.range.base_mip + barrier.range.mip_count, barrier.range.base_layer,
                    barrier.range.base_layer + barrier.range.layer_count,
                    stage_summary(barrier.src_stage), stage_summary(barrier.dst_stage),
                    transfer ? "  [queue-family ownership transfer]" : "")) {
            return false;
        }
    }
    for (const rhi::BufferBarrier& barrier : batch.buffers) {
        if (!append(out, "%sbuffer '%s'  src %s  dst %s%s\n", indent,
                    graph.resource(barrier.resource).name, stage_summary(barrier.src_stage),
                    stage_summary(barrier.dst_stage),
                    barrier.src_queue_family != barrier.dst_queue_family
                        ? "  [queue-family ownership transfer]"
                        : "")) {
            return false;
        }
    }
    for (const rhi::MemoryBarrier& barrier : batch.memory) {
        if (!append(out, "%smemory src %s  dst %s\n", indent, stage_summary(barrier.src_stage),
                    stage_summary(barrier.dst_stage))) {
            return false;
        }
    }
    return true;
}

}  // namespace

Expected<usize, Error> dump_text(const RenderGraph& graph, const CompiledGraph& plan,
                                 Array<char>& out) noexcept {
    const usize start = out.size();
    for (usize index = 0; index < plan.submits.size(); ++index) {
        const Submit& submit = plan.submits[index];
        if (!append(out, "submit %zu  queue=%s  signal=%llu\n", index,
                    rhi::queue_kind_name(submit.queue),
                    static_cast<unsigned long long>(submit.signal_value))) {
            return fail(ErrorCode::OutOfMemory, "the graph dump ran out of memory");
        }
        for (const SemaphoreWait& wait : submit.waits) {
            if (!append(out, "    wait timeline[%s] >= %llu\n", rhi::queue_kind_name(wait.queue),
                        static_cast<unsigned long long>(wait.value))) {
                return fail(ErrorCode::OutOfMemory, "the graph dump ran out of memory");
            }
        }
        for (const ScheduledPass& scheduled : submit.passes) {
            if (!append(out, "    pass '%s'\n", graph.pass_name(scheduled.pass)) ||
                !dump_batch(out, "        ", scheduled.pre, graph)) {
                return fail(ErrorCode::OutOfMemory, "the graph dump ran out of memory");
            }
        }
        if (!submit.release.empty()) {
            if (!append(out, "    release (end of submit)\n") ||
                !dump_batch(out, "        ", submit.release, graph)) {
                return fail(ErrorCode::OutOfMemory, "the graph dump ran out of memory");
            }
        }
    }

    if (!append(out, "memory: peak %llu bytes, %llu without aliasing\n",
                static_cast<unsigned long long>(plan.memory.heap_bytes),
                static_cast<unsigned long long>(plan.memory.naive_bytes))) {
        return fail(ErrorCode::OutOfMemory, "the graph dump ran out of memory");
    }
    for (const Placement& placement : plan.memory.placements) {
        if (!append(out, "    '%s' at %llu, %llu bytes, passes [%u, %u]\n",
                    graph.resource(placement.resource).name,
                    static_cast<unsigned long long>(placement.offset),
                    static_cast<unsigned long long>(placement.size), placement.first_pass,
                    placement.last_pass)) {
            return fail(ErrorCode::OutOfMemory, "the graph dump ran out of memory");
        }
    }
    for (const PassId pass : plan.culled) {
        if (!append(out, "culled pass '%s'\n", graph.pass_name(pass))) {
            return fail(ErrorCode::OutOfMemory, "the graph dump ran out of memory");
        }
    }
    if (!terminate(out)) {
        return fail(ErrorCode::OutOfMemory, "the graph dump ran out of memory");
    }
    return out.size() - start - 1;
}

Expected<usize, Error> dump_graphviz(const RenderGraph& graph, const CompiledGraph& plan,
                                     Array<char>& out) noexcept {
    const usize start = out.size();
    if (!append(out, "digraph render_graph {\n  rankdir=LR;\n  node [shape=box];\n")) {
        return fail(ErrorCode::OutOfMemory, "the graph dump ran out of memory");
    }

    for (usize index = 0; index < plan.submits.size(); ++index) {
        const Submit& submit = plan.submits[index];
        if (!append(out, "  subgraph cluster_%zu {\n    label=\"submit %zu (%s)\";\n", index, index,
                    rhi::queue_kind_name(submit.queue))) {
            return fail(ErrorCode::OutOfMemory, "the graph dump ran out of memory");
        }
        for (const ScheduledPass& scheduled : submit.passes) {
            if (!append(out, "    p%u [label=\"%s\\n%u barriers\"];\n", scheduled.pass,
                        graph.pass_name(scheduled.pass), static_cast<u32>(scheduled.pre.count()))) {
                return fail(ErrorCode::OutOfMemory, "the graph dump ran out of memory");
            }
        }
        if (!append(out, "  }\n")) {
            return fail(ErrorCode::OutOfMemory, "the graph dump ran out of memory");
        }
    }

    // Edges within a submit are execution order; edges between submits are the semaphore waits, and
    // they are drawn bold because they are the ones that cost a stall.
    for (const Submit& submit : plan.submits) {
        for (usize index = 1; index < submit.passes.size(); ++index) {
            if (!append(out, "  p%u -> p%u;\n", submit.passes[index - 1].pass,
                        submit.passes[index].pass)) {
                return fail(ErrorCode::OutOfMemory, "the graph dump ran out of memory");
            }
        }
    }
    for (usize index = 0; index < plan.submits.size(); ++index) {
        const Submit& consumer = plan.submits[index];
        if (consumer.passes.empty()) {
            continue;
        }
        for (const SemaphoreWait& wait : consumer.waits) {
            for (usize other = 0; other < index; ++other) {
                const Submit& producer = plan.submits[other];
                if (producer.queue != wait.queue || producer.signal_value != wait.value ||
                    producer.passes.empty()) {
                    continue;
                }
                if (!append(out,
                            "  p%u -> p%u [style=bold,color=firebrick,label=\"semaphore %llu\"];\n",
                            producer.passes[producer.passes.size() - 1].pass,
                            consumer.passes[0].pass, static_cast<unsigned long long>(wait.value))) {
                    return fail(ErrorCode::OutOfMemory, "the graph dump ran out of memory");
                }
            }
        }
    }

    for (const Placement& placement : plan.memory.placements) {
        if (!append(out, "  r%u [shape=note,label=\"%s\\n%llu B @ %llu\"];\n", placement.resource,
                    graph.resource(placement.resource).name,
                    static_cast<unsigned long long>(placement.size),
                    static_cast<unsigned long long>(placement.offset))) {
            return fail(ErrorCode::OutOfMemory, "the graph dump ran out of memory");
        }
    }
    if (!append(out, "}\n") || !terminate(out)) {
        return fail(ErrorCode::OutOfMemory, "the graph dump ran out of memory");
    }
    return out.size() - start - 1;
}

Expected<PlanAudit, Error> validate_plan(const RenderGraph& graph,
                                         const CompiledGraph& plan) noexcept {
    PlanAudit audit;
    audit.submits = static_cast<u32>(plan.submits.size());
    audit.transient_peak_bytes = plan.memory.heap_bytes;
    audit.transient_naive_bytes = plan.memory.naive_bytes;

    // --- Waits point backwards, and at a value that is actually signalled
    // -------------------------
    for (usize index = 0; index < plan.submits.size(); ++index) {
        const Submit& submit = plan.submits[index];
        audit.passes += static_cast<u32>(submit.passes.size());
        audit.semaphore_waits += static_cast<u32>(submit.waits.size());
        for (const SemaphoreWait& wait : submit.waits) {
            bool found = false;
            for (usize other = 0; other < index; ++other) {
                if (plan.submits[other].queue == wait.queue &&
                    plan.submits[other].signal_value == wait.value) {
                    found = true;
                    break;
                }
            }
            if (!found) {
                return fail(ErrorCode::Internal,
                            "a submit waits on a timeline value no earlier submit signals; the "
                            "scheduler emitted an edge that does not point backwards");
            }
        }
    }

    // --- Every release has exactly one matching acquire
    // ---------------------------------------------
    //
    // A validation layer will not catch a missing ownership transfer — M3's spike proved it, with
    // zero errors and correct pixels from a plan that emitted a cross-queue hazard as a plain
    // barrier. It is silently wrong on hardware that compresses, so it is checked here instead.
    for (const Submit& submit : plan.submits) {
        for (const rhi::ImageBarrier& release : submit.release.images) {
            if (release.src_queue_family == release.dst_queue_family) {
                continue;
            }
            ++audit.ownership_releases;
            bool matched = false;
            for (const Submit& consumer : plan.submits) {
                for (const ScheduledPass& scheduled : consumer.passes) {
                    for (const rhi::ImageBarrier& acquire : scheduled.pre.images) {
                        if (acquire.resource != release.resource ||
                            acquire.src_queue_family != release.src_queue_family ||
                            acquire.dst_queue_family != release.dst_queue_family ||
                            acquire.old_layout != release.old_layout ||
                            acquire.new_layout != release.new_layout ||
                            !(acquire.range == release.range)) {
                            continue;
                        }
                        matched = true;
                    }
                }
            }
            if (!matched) {
                return fail(ErrorCode::Internal,
                            "a queue-family ownership release has no matching acquire. Both halves "
                            "are derived from one hazard, so this means the derivation drifted");
            }
        }
        for (const ScheduledPass& scheduled : submit.passes) {
            audit.image_barriers += static_cast<u32>(scheduled.pre.images.size());
            audit.buffer_barriers += static_cast<u32>(scheduled.pre.buffers.size());
            audit.memory_barriers += static_cast<u32>(scheduled.pre.memory.size());
            for (const rhi::ImageBarrier& barrier : scheduled.pre.images) {
                if (barrier.old_layout != barrier.new_layout) {
                    ++audit.layout_transitions;
                }
                if (barrier.src_queue_family != barrier.dst_queue_family) {
                    ++audit.ownership_acquires;
                }
            }
        }
    }
    audit.alias_barriers = plan.stats.alias_barriers;

    // --- Placements stay inside the pool and do not collide
    // ------------------------------------------
    for (usize index = 0; index < plan.memory.placements.size(); ++index) {
        const Placement& a = plan.memory.placements[index];
        if (a.offset + a.size > plan.memory.heap_bytes) {
            return fail(ErrorCode::Internal,
                        "a transient's placement reaches past the pool the plan reserves");
        }
        for (usize other = index + 1; other < plan.memory.placements.size(); ++other) {
            const Placement& b = plan.memory.placements[other];
            const bool memory_overlaps =
                a.offset + a.size > b.offset && b.offset + b.size > a.offset;
            const bool lifetimes_overlap =
                a.last_pass >= b.first_pass && b.last_pass >= a.first_pass;
            if (memory_overlaps && lifetimes_overlap) {
                return fail(
                    ErrorCode::Internal,
                    "two transients whose lifetimes overlap were placed on the same memory");
            }
        }
    }

    // --- Every acquire's resource is one the graph actually has
    // ----------------------------------------
    for (const Submit& submit : plan.submits) {
        for (const ScheduledPass& scheduled : submit.passes) {
            for (const rhi::ImageBarrier& barrier : scheduled.pre.images) {
                if (barrier.resource >= graph.resource_count()) {
                    return fail(ErrorCode::Internal,
                                "a barrier names a resource the graph has not");
                }
            }
        }
    }
    return audit;
}

}  // namespace cy::rendering
