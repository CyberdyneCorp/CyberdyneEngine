// The derivation: barriers, aliasing, scheduling and semaphores, from declared reads and writes.
// Tasks 2.2.2 and 2.2.3.
//
// THIS FILE MAKES NO DEVICE CALLS. It asks the caller one question — how much memory does this
// transient need — and everything else is arithmetic over the declarations. That is what lets the
// null backend run this unchanged, what lets a unit test compile a whole frame's plan with no GPU,
// and what makes the plan deterministic: two independent processes produced byte-identical plans in
// M3's spike, and CompiledGraph::plan_hash is how a test asserts it here.
//
// --- THE MODEL, IN FULL
// ---------------------------------------------------------------------------
//
// STATE IS PER (MIP, LAYER) CELL. A buffer is one cell. Each cell remembers its layout, its owning
// queue family, the stage and access of the last WRITE, the union of stages and accesses of the
// reads since that write, and which submit last touched it.
//
// THE HAZARD RULES, COMPLETE:
//   * a write:  src_stage = write_stage | read_stage   (write-after-write and write-after-read)
//               src_access = write_access ONLY — a write-after-read needs an execution dependency,
//               not a memory one, so the read side contributes no access bits.
//   * a read:   src_stage = write_stage, src_access = write_access (read-after-write).
//               Read-after-read emits nothing unless the layout differs.
//   * a differing layout, or a differing owning queue family, forces a barrier regardless of
//   hazard.
//   * after a write, the write state is overwritten and the read state is ZEROED; after a read, the
//     read state is unioned into.
//
// COALESCING IS NOT AN OPTIMISATION TO DEFER. Barriers are computed per cell and then merged by
// identical barrier key into rectangles — contiguous layer runs per mip, then adjacent mips whose
// runs match. The two-layer ownership release in M3's hard case must be ONE barrier over
// layers [0, 2), and the merge is what makes it one.
//
// CROSS-QUEUE WORK IS A SEMAPHORE, NEVER A BARRIER. A pipeline barrier synchronises nothing between
// two command streams. When a pass depends on a pass on another queue: seal the producing submit;
// seal this queue's current submit if it has work (so already-recorded work is not delayed behind
// the wait); open a fresh one and record the wait on it. One timeline semaphore per queue.
//
// QUEUE-FAMILY OWNERSHIP TRANSFER is two halves derived from ONE hazard, so they cannot drift: a
// release at the end of the producing submit and an acquire in the consuming pass's pre-batch, with
// identical layouts, families and subresource range. The semaphore between the submits is what
// orders release before acquire; the barriers alone do nothing.
//
// --- WHAT VALIDATION WILL NOT CATCH, AND WHY THAT MATTERS HERE
// ---------------------------------------
//
// M3's spike ran two negative controls that did NOT fire, and they are the important findings:
// emitting a cross-queue hazard as a plain barrier with no ownership transfer produced zero
// validation errors and correct pixels, and removing an alias barrier produced zero validation
// errors. Neither is caught by a layer. They are structural properties of this file, covered by the
// graph's own tests against the derived plan — which is why those tests assert on barrier contents
// and not only on whether a frame rendered.

#include <cy/backends/rhi/validation.h>
#include <cy/core/base/assert.h>
#include <cy/rendering/graph/graph.h>

#include <algorithm>
#include <utility>

namespace cy::rendering {
namespace {

constexpr u64 kFnvOffset = 1469598103934665603ULL;
constexpr u64 kFnvPrime = 1099511628211ULL;

u64 hash_value(u64 seed, u64 value) noexcept {
    u64 hash = seed;
    for (u32 byte = 0; byte < sizeof(value); ++byte) {
        hash ^= (value >> (byte * 8)) & 0xFFU;
        hash *= kFnvPrime;
    }
    return hash;
}

u64 align_up_to(u64 value, u64 alignment) noexcept {
    return alignment <= 1 ? value : (value + alignment - 1) / alignment * alignment;
}

/// One (mip, layer) of an image, or the whole of a buffer.
struct CellState {
    rhi::ImageLayout layout = rhi::ImageLayout::Undefined;
    u32 queue_family = rhi::kQueueFamilyIgnored;
    rhi::Stage write_stage = rhi::Stage::None;
    rhi::AccessFlags write_access = rhi::AccessFlags::None;
    rhi::Stage read_stage = rhi::Stage::None;
    rhi::AccessFlags read_access = rhi::AccessFlags::None;
    i32 last_submit = -1;
};

/// Everything two barriers must agree on to be merged into one.
struct BarrierKey {
    rhi::Stage src_stage = rhi::Stage::None;
    rhi::AccessFlags src_access = rhi::AccessFlags::None;
    rhi::Stage dst_stage = rhi::Stage::None;
    rhi::AccessFlags dst_access = rhi::AccessFlags::None;
    rhi::ImageLayout old_layout = rhi::ImageLayout::Undefined;
    rhi::ImageLayout new_layout = rhi::ImageLayout::Undefined;
    u32 src_queue_family = rhi::kQueueFamilyIgnored;
    u32 dst_queue_family = rhi::kQueueFamilyIgnored;

    [[nodiscard]] friend bool operator==(const BarrierKey& a, const BarrierKey& b) noexcept {
        return a.src_stage == b.src_stage && a.src_access == b.src_access &&
               a.dst_stage == b.dst_stage && a.dst_access == b.dst_access &&
               a.old_layout == b.old_layout && a.new_layout == b.new_layout &&
               a.src_queue_family == b.src_queue_family && a.dst_queue_family == b.dst_queue_family;
    }
};

/// One cell that needs a barrier, before coalescing.
struct PendingImage {
    ResourceId resource = kInvalidResource;
    u16 mip = 0;
    u16 layer = 0;
    BarrierKey key{};
    /// Which submit's release batch this belongs in; -1 for a pre-batch barrier.
    i32 release_submit = -1;
};

/// A pass's last use of a resource, kept for alias hazards and lifetimes.
struct ResourceUse {
    rhi::Stage stage = rhi::Stage::None;
    rhi::AccessFlags access = rhi::AccessFlags::None;
    u32 last_queue_family = rhi::kQueueFamilyIgnored;
    i32 first = -1;  // position in schedule order
    i32 last = -1;
    rhi::QueueKind first_queue = rhi::QueueKind::Graphics;
};

rhi::ImageAspect aspect_of(const ResourceInfo& info) noexcept {
    if (!info.is_texture) {
        return rhi::ImageAspect::Color;
    }
    const rhi::FormatInfo& format = rhi::format_info(info.texture.format);
    if (format.has_depth && format.has_stencil) {
        return rhi::ImageAspect::DepthStencil;
    }
    return format.has_depth ? rhi::ImageAspect::Depth : rhi::ImageAspect::Color;
}

}  // namespace

/// The compiler. A struct rather than a class because every member is state one phase writes and
/// the next reads; there is nothing to encapsulate from anybody.
struct Compiler {
    RenderGraph& graph;
    const CompileOptions& options;
    Allocator& allocator;
    CompiledGraph out;
    Status status;

    Array<usize> cell_base;  // per resource: index of its first cell
    Array<CellState> cells;
    Array<ResourceUse> resource_use;

    Array<u32> dep_offsets;  // per pass: [dep_offsets[p], dep_offsets[p+1]) into dep_storage
    Array<PassId> dep_storage;
    Array<PassId> alias_edges;  // (consumer, producer) pairs appended after placement
    Array<bool> kept;
    Array<PassId> order;   // kept passes, in schedule order
    Array<i32> order_of;   // pass -> position in order, -1 when culled
    Array<i32> submit_of;  // pass -> submit index

    u64 timelines[rhi::kQueueKindCount] = {};
    i32 current_submit[rhi::kQueueKindCount] = {-1, -1, -1};

    Compiler(RenderGraph& g, const CompileOptions& opts) noexcept
        : graph(g),
          options(opts),
          allocator(g.allocator()),
          out(g.allocator()),
          cell_base(g.allocator()),
          cells(g.allocator()),
          resource_use(g.allocator()),
          dep_offsets(g.allocator()),
          dep_storage(g.allocator()),
          alias_edges(g.allocator()),
          kept(g.allocator()),
          order(g.allocator()),
          order_of(g.allocator()),
          submit_of(g.allocator()) {}

    void fail(ErrorCode code, const char* message) noexcept {
        if (status) {
            status = make_unexpected(Error{code, message, 0});
        }
    }

    template <class T>
    bool push(Array<T>& array, const T& value) noexcept {
        if (Status pushed = array.push_back(value); !pushed) {
            fail(ErrorCode::OutOfMemory, "the render graph ran out of memory while compiling");
            return false;
        }
        return true;
    }

    // --- Queues ---------------------------------------------------------------------------------

    /// The queue a pass actually runs on. A pass that asked for a queue the device does not have —
    /// or that async compute is disabled for — folds onto graphics. That fold is the whole of the
    /// single-queue path: the same declarations, one submit, no semaphores, no ownership transfers.
    [[nodiscard]] rhi::QueueKind queue_of(PassId pass) const noexcept {
        const rhi::QueueKind requested = graph.pass_queue(pass);
        if (requested == rhi::QueueKind::Graphics) {
            return requested;
        }
        if (!options.enable_async_compute) {
            return rhi::QueueKind::Graphics;
        }
        const auto index = static_cast<u32>(requested);
        if (index >= rhi::kQueueKindCount || !options.queue_available[index]) {
            return rhi::QueueKind::Graphics;
        }
        return requested;
    }

    [[nodiscard]] u32 family_of(rhi::QueueKind queue) const noexcept {
        const auto index = static_cast<u32>(queue);
        return index < rhi::kQueueKindCount ? options.queue_family[index] : 0;
    }

    // --- Cells ----------------------------------------------------------------------------------

    [[nodiscard]] static usize cells_in(const ResourceInfo& info) noexcept {
        return info.is_texture
                   ? static_cast<usize>(info.texture.mip_levels) * info.texture.array_layers
                   : 1;
    }

    [[nodiscard]] usize cell_index(ResourceId resource, u16 mip, u16 layer) const noexcept {
        const ResourceInfo& info = graph.resource(resource);
        return cell_base[resource] + (static_cast<usize>(mip) * info.texture.array_layers) + layer;
    }

    bool init_cells() noexcept {
        usize total = 0;
        for (ResourceId id = 0; id < graph.resource_count(); ++id) {
            if (!push(cell_base, total)) {
                return false;
            }
            total += cells_in(graph.resource(id));
        }
        if (Status sized = cells.resize(total); !sized) {
            fail(ErrorCode::OutOfMemory, "the render graph could not size its resource state");
            return false;
        }
        for (CellState& cell : cells) {
            cell = CellState{};
        }
        if (Status sized = resource_use.resize(graph.resource_count()); !sized) {
            fail(ErrorCode::OutOfMemory, "the render graph could not size its lifetime table");
            return false;
        }
        for (ResourceUse& use : resource_use) {
            use = ResourceUse{};
        }

        // An imported resource starts in whatever state the caller promised. Getting that wrong is
        // how a first barrier transitions from a layout the image is not in, which is undefined
        // behaviour that looks like a corrupted texture.
        for (ResourceId id = 0; id < graph.resource_count(); ++id) {
            const ResourceInfo& info = graph.resource(id);
            if (!info.imported) {
                continue;
            }
            const usize count = cells_in(info);
            for (usize offset = 0; offset < count; ++offset) {
                CellState& cell = cells[cell_base[id] + offset];
                cell.layout = info.initial_layout;
                cell.queue_family = info.initial_queue_family;
            }
        }
        return true;
    }

    // --- Dependency edges
    // -------------------------------------------------------------------------

    /// For every cell, consecutive uses where at least one writes produce an edge from the later
    /// pass to the earlier one. Read-after-read produces none, which is what lets two sampling
    /// passes overlap.
    bool build_dependencies() noexcept {
        const usize pass_count = graph.pass_count();

        // The sentinel is kInvalidPass rather than -1, so every comparison below stays in the
        // unsigned domain: a cast between signednesses inside a comparison is exactly the shape
        // that hides an off-by-one, and it is worth not having one here.
        struct LastTouch {
            PassId writer = kInvalidPass;
            u32 reader_offset = 0;
            u32 reader_count = 0;
        };
        Array<LastTouch> touch(allocator);
        if (Status sized = touch.resize(cells.size()); !sized) {
            fail(ErrorCode::OutOfMemory, "the render graph could not size its dependency state");
            return false;
        }
        for (LastTouch& entry : touch) {
            entry = LastTouch{};
        }
        // Readers accumulate in one flat array; a cell's readers are a contiguous run that is
        // abandoned (not freed) when a write clears it. A frame's declaration count is bounded and
        // this runs once, so trading the memory for not owning a per-cell container is the right
        // way round.
        Array<PassId> readers(allocator);

        Array<PassId> mine(allocator);
        if (!push(dep_offsets, 0U)) {
            return false;
        }

        for (PassId pass = 0; pass < pass_count; ++pass) {
            mine.clear();
            for (const Use& use : graph.pass_uses(pass)) {
                const ResourceInfo& info = graph.resource(use.resource);
                const bool writes = rhi::is_write(use.access);
                const u16 mips = info.is_texture ? use.range.mip_count : 1;
                const u16 layers = info.is_texture ? use.range.layer_count : 1;
                for (u16 mip = 0; mip < mips; ++mip) {
                    for (u16 layer = 0; layer < layers; ++layer) {
                        const usize index =
                            info.is_texture
                                ? cell_index(use.resource,
                                             static_cast<u16>(use.range.base_mip + mip),
                                             static_cast<u16>(use.range.base_layer + layer))
                                : cell_base[use.resource];
                        LastTouch& entry = touch[index];
                        if (entry.writer != kInvalidPass && entry.writer != pass) {
                            add_unique(mine, entry.writer);
                        }
                        if (writes) {
                            for (u32 offset = 0; offset < entry.reader_count; ++offset) {
                                const PassId reader = readers[entry.reader_offset + offset];
                                if (reader != pass) {
                                    add_unique(mine, reader);
                                }
                            }
                            entry.writer = pass;
                            entry.reader_count = 0;
                        } else {
                            if (entry.reader_count == 0) {
                                entry.reader_offset = static_cast<u32>(readers.size());
                            } else if (entry.reader_offset + entry.reader_count != readers.size()) {
                                // Another cell has appended since this one last did, so the run is
                                // no longer contiguous. Copy it to the end and carry on.
                                const u32 old_offset = entry.reader_offset;
                                const u32 old_count = entry.reader_count;
                                entry.reader_offset = static_cast<u32>(readers.size());
                                for (u32 offset = 0; offset < old_count; ++offset) {
                                    // BY VALUE, NOT BY REFERENCE INTO THE ARRAY BEING GROWN.
                                    // `push()` may reallocate, and `push(readers, readers[i])`
                                    // hands `Array::emplace_back` a reference that the
                                    // reallocation has already freed — a read of freed memory that
                                    // AddressSanitizer reported out of `unit.render_forward` at
                                    // M3's gate. Silent without a sanitizer: the copy usually
                                    // finds the old bytes still intact.
                                    const u32 moved = readers[old_offset + offset];
                                    if (!push(readers, moved)) {
                                        return false;
                                    }
                                }
                            }
                            if (!push(readers, pass)) {
                                return false;
                            }
                            ++entry.reader_count;
                        }
                    }
                }
            }
            for (const PassId dependency : mine) {
                if (!push(dep_storage, dependency)) {
                    return false;
                }
            }
            if (!push(dep_offsets, static_cast<u32>(dep_storage.size()))) {
                return false;
            }
        }
        return true;
    }

    static void add_unique(Array<PassId>& array, PassId value) noexcept {
        for (const PassId existing : array) {
            if (existing == value) {
                return;
            }
        }
        (void)array.push_back(value);
    }

    [[nodiscard]] Span<const PassId> dependencies_of(PassId pass) const noexcept {
        const u32 begin = dep_offsets[pass];
        const u32 end = dep_offsets[pass + 1];
        return {dep_storage.data() + begin, end - begin};
    }

    // --- Culling
    // -----------------------------------------------------------------------------------

    /// Roots are passes with a side effect and passes that write a resource the graph does not own.
    /// The second falls out of the definition: the graph cannot see who reads an imported resource
    /// afterwards, so it must assume somebody does.
    bool cull() noexcept {
        const usize pass_count = graph.pass_count();
        if (Status sized = kept.resize(pass_count); !sized) {
            fail(ErrorCode::OutOfMemory, "the render graph could not size its culling state");
            return false;
        }
        for (bool& value : kept) {
            value = false;
        }

        Array<PassId> stack(allocator);
        for (PassId pass = 0; pass < pass_count; ++pass) {
            bool root = graph.pass_has_side_effect(pass);
            for (const Use& use : graph.pass_uses(pass)) {
                if (rhi::is_write(use.access) && !graph.resource(use.resource).transient) {
                    root = true;
                }
            }
            if (root) {
                kept[pass] = true;
                if (!push(stack, pass)) {
                    return false;
                }
            }
        }
        while (!stack.empty()) {
            const PassId pass = stack[stack.size() - 1];
            stack.pop_back();
            for (const PassId dependency : dependencies_of(pass)) {
                if (!kept[dependency]) {
                    kept[dependency] = true;
                    if (!push(stack, dependency)) {
                        return false;
                    }
                }
            }
        }

        if (Status sized = order_of.resize(pass_count); !sized) {
            fail(ErrorCode::OutOfMemory, "the render graph could not size its schedule");
            return false;
        }
        for (PassId pass = 0; pass < pass_count; ++pass) {
            if (kept[pass]) {
                order_of[pass] = static_cast<i32>(order.size());
                if (!push(order, pass)) {
                    return false;
                }
            } else {
                order_of[pass] = -1;
                if (!push(out.culled, pass)) {
                    return false;
                }
            }
        }
        return true;
    }

    // --- Scheduling
    // ----------------------------------------------------------------------------------

    i32 open_submit(rhi::QueueKind queue) noexcept {
        const auto index = static_cast<u32>(queue);
        Submit submit(allocator);
        submit.queue = queue;
        submit.signal_value = ++timelines[index];
        if (Status pushed = out.submits.push_back(std::move(submit)); !pushed) {
            fail(ErrorCode::OutOfMemory, "the render graph could not record a submit");
            return -1;
        }
        current_submit[index] = static_cast<i32>(out.submits.size() - 1);
        return current_submit[index];
    }

    void close_submit(i32 index) noexcept {
        if (index < 0) {
            return;
        }
        const auto queue = static_cast<u32>(out.submits[static_cast<usize>(index)].queue);
        if (current_submit[queue] == index) {
            current_submit[queue] = -1;
        }
    }

    void reset_schedule() noexcept {
        out.submits.clear();
        for (u64& timeline : timelines) {
            timeline = 0;
        }
        for (i32& submit : current_submit) {
            submit = -1;
        }
    }

    bool schedule() noexcept {
        if (Status sized = submit_of.resize(graph.pass_count()); !sized) {
            fail(ErrorCode::OutOfMemory, "the render graph could not size its submit table");
            return false;
        }
        for (i32& value : submit_of) {
            value = -1;
        }

        Array<i32> cross(allocator);
        for (const PassId pass : order) {
            const rhi::QueueKind queue = queue_of(pass);
            const auto queue_index = static_cast<u32>(queue);

            cross.clear();
            for (const PassId dependency : dependencies_of(pass)) {
                if (!kept[dependency] || queue_of(dependency) == queue) {
                    continue;
                }
                if (!push(cross, submit_of[dependency])) {
                    return false;
                }
            }
            for (usize index = 0; index + 1 < alias_edges.size(); index += 2) {
                if (alias_edges[index] != pass) {
                    continue;
                }
                const PassId producer = alias_edges[index + 1];
                if (!kept[producer] || queue_of(producer) == queue) {
                    continue;
                }
                if (!push(cross, submit_of[producer])) {
                    return false;
                }
            }

            if (!cross.empty()) {
                // Every producing submit must be sealed before this pass's submit may wait on it,
                // and a wait applies to the whole submit — so if this queue has already recorded
                // work, seal that too rather than delaying it behind the wait.
                for (const i32 producer : cross) {
                    close_submit(producer);
                }
                if (current_submit[queue_index] >= 0 &&
                    !out.submits[static_cast<usize>(current_submit[queue_index])].passes.empty()) {
                    close_submit(current_submit[queue_index]);
                }
            }
            if (current_submit[queue_index] < 0 && open_submit(queue) < 0) {
                return false;
            }

            const i32 submit_index = current_submit[queue_index];
            submit_of[pass] = submit_index;
            Submit& submit = out.submits[static_cast<usize>(submit_index)];

            for (const i32 producer_index : cross) {
                if (producer_index < 0) {
                    continue;
                }
                const Submit& producer = out.submits[static_cast<usize>(producer_index)];
                bool merged = false;
                for (SemaphoreWait& wait : submit.waits) {
                    if (wait.queue != producer.queue) {
                        continue;
                    }
                    // One wait per producing queue, at the highest value: a timeline wait for N
                    // subsumes every wait for less than N, so two waits on one timeline would be
                    // one wait and one no-op.
                    wait.value =
                        wait.value > producer.signal_value ? wait.value : producer.signal_value;
                    merged = true;
                    break;
                }
                if (!merged) {
                    SemaphoreWait wait;
                    wait.queue = producer.queue;
                    wait.value = producer.signal_value;
                    wait.stage = rhi::Stage::AllCommands;
                    if (Status pushed = submit.waits.push_back(wait); !pushed) {
                        fail(ErrorCode::OutOfMemory, "the render graph could not record a wait");
                        return false;
                    }
                }
            }

            ScheduledPass scheduled;
            scheduled.pass = pass;
            if (Status pushed = submit.passes.push_back(std::move(scheduled)); !pushed) {
                fail(ErrorCode::OutOfMemory, "the render graph could not record a scheduled pass");
                return false;
            }
        }
        for (usize index = 0; index < out.submits.size(); ++index) {
            close_submit(static_cast<i32>(index));
        }
        return true;
    }

    // --- Lifetimes and placement
    // ------------------------------------------------------------------------

    void compute_lifetimes() noexcept {
        for (ResourceUse& use : resource_use) {
            use.first = -1;
            use.last = -1;
        }
        for (usize position = 0; position < order.size(); ++position) {
            const PassId pass = order[position];
            for (const Use& use : graph.pass_uses(pass)) {
                ResourceUse& record = resource_use[use.resource];
                if (record.first < 0) {
                    record.first = static_cast<i32>(position);
                    record.first_queue = queue_of(pass);
                }
                record.last = static_cast<i32>(position);
            }
        }
    }

    /// Greedy, largest first, at the lowest offset that clears every lifetime-overlapping
    /// placement. Ties break on resource id so that the result does not depend on the sort's
    /// stability, which is one of the two things that make the plan byte-identical between
    /// processes.
    bool place_memory() noexcept {
        Array<ResourceId> transients(allocator);
        Array<rhi::MemoryRequirements> requirements(allocator);
        if (Status sized = requirements.resize(graph.resource_count()); !sized) {
            fail(ErrorCode::OutOfMemory, "the render graph could not size its memory table");
            return false;
        }

        for (ResourceId id = 0; id < graph.resource_count(); ++id) {
            const ResourceInfo& info = graph.resource(id);
            if (!info.transient || resource_use[id].first < 0) {
                continue;
            }
            rhi::MemoryRequirements requirement;
            if (options.query_memory == nullptr ||
                !options.query_memory(id, info, requirement, options.query_user)) {
                continue;
            }
            if (requirement.size == 0) {
                continue;
            }
            requirements[id] = requirement;
            if (!push(transients, id)) {
                return false;
            }
            out.memory.naive_bytes += align_up_to(requirement.size, requirement.alignment);
            out.memory.memory_type_bits &= requirement.memory_type_bits;
        }

        if (!options.enable_aliasing) {
            u64 offset = 0;
            for (const ResourceId id : transients) {
                offset = align_up_to(offset, requirements[id].alignment);
                Placement placement;
                placement.resource = id;
                placement.offset = offset;
                placement.size = requirements[id].size;
                placement.first_pass = static_cast<u32>(resource_use[id].first);
                placement.last_pass = static_cast<u32>(resource_use[id].last);
                if (!push(out.memory.placements, placement)) {
                    return false;
                }
                offset += requirements[id].size;
            }
            out.memory.heap_bytes = offset;
            return true;
        }

        std::ranges::sort(transients, [&](ResourceId a, ResourceId b) noexcept {
            if (requirements[a].size != requirements[b].size) {
                return requirements[a].size > requirements[b].size;
            }
            return a < b;
        });

        Array<u64> blocked_begin(allocator);
        Array<u64> blocked_end(allocator);
        for (const ResourceId id : transients) {
            const rhi::MemoryRequirements& requirement = requirements[id];
            // Every transient in this list has a lifetime — place_memory() skipped the ones that do
            // not — so the -1 sentinel is gone and the comparisons below are unsigned throughout.
            const auto use_first = static_cast<u32>(resource_use[id].first);
            const auto use_last = static_cast<u32>(resource_use[id].last);
            blocked_begin.clear();
            blocked_end.clear();
            for (const Placement& placement : out.memory.placements) {
                const bool lifetimes_overlap =
                    placement.last_pass >= use_first && placement.first_pass <= use_last;
                // The policy knob: aliasing buys memory and spends parallelism, because an alias
                // edge serialises work that shares nothing but bytes. A caller that would rather
                // keep the async overlap refuses to share across queues.
                const bool queues_differ =
                    resource_use[id].first_queue != resource_use[placement.resource].first_queue;
                if (lifetimes_overlap || (!options.alias_across_queues && queues_differ)) {
                    if (!push(blocked_begin, placement.offset) ||
                        !push(blocked_end, placement.offset + placement.size)) {
                        return false;
                    }
                }
            }
            // Insertion sort of the blocked intervals by start. There are a handful of them per
            // resource and this keeps the two parallel arrays in step without a pair type.
            for (usize index = 1; index < blocked_begin.size(); ++index) {
                const u64 begin = blocked_begin[index];
                const u64 end = blocked_end[index];
                usize slot = index;
                while (slot > 0 && blocked_begin[slot - 1] > begin) {
                    blocked_begin[slot] = blocked_begin[slot - 1];
                    blocked_end[slot] = blocked_end[slot - 1];
                    --slot;
                }
                blocked_begin[slot] = begin;
                blocked_end[slot] = end;
            }

            u64 offset = 0;
            bool placed = false;
            for (usize index = 0; index < blocked_begin.size(); ++index) {
                const u64 candidate = align_up_to(offset, requirement.alignment);
                if (candidate + requirement.size <= blocked_begin[index]) {
                    offset = candidate;
                    placed = true;
                    break;
                }
                offset = offset > blocked_end[index] ? offset : blocked_end[index];
            }
            if (!placed) {
                offset = align_up_to(offset, requirement.alignment);
            }

            Placement placement;
            placement.resource = id;
            placement.offset = offset;
            placement.size = requirement.size;
            placement.first_pass = static_cast<u32>(resource_use[id].first);
            placement.last_pass = static_cast<u32>(resource_use[id].last);
            if (!push(out.memory.placements, placement)) {
                return false;
            }
            const u64 reach = offset + requirement.size;
            out.memory.heap_bytes = out.memory.heap_bytes > reach ? out.memory.heap_bytes : reach;
        }

        std::ranges::sort(out.memory.placements,
                          [](const Placement& a, const Placement& b) noexcept {
                              return a.resource < b.resource;
                          });
        return true;
    }

    /// Which finished transients a transient reuses the memory of. Empty when nothing does, which
    /// is every resource in a plan with aliasing off.
    [[nodiscard]] bool aliases(ResourceId consumer, ResourceId producer) const noexcept {
        const Placement* a = nullptr;
        const Placement* b = nullptr;
        for (const Placement& placement : out.memory.placements) {
            if (placement.resource == consumer) {
                a = &placement;
            }
            if (placement.resource == producer) {
                b = &placement;
            }
        }
        if (a == nullptr || b == nullptr) {
            return false;
        }
        const bool memory_overlaps =
            a->offset + a->size > b->offset && b->offset + b->size > a->offset;
        return memory_overlaps && b->last_pass < a->first_pass;
    }

    /// THE STEP THAT COST THE SPIKE THE MOST TIME. Aliasing creates dependencies the resource graph
    /// cannot see: the pass that first uses a transient must be ordered after the last use of every
    /// transient whose memory it reuses. Across queues that ordering can only be a semaphore, so
    /// these edges must exist BEFORE submits are cut — a VkMemoryBarrier2 in the consumer's command
    /// buffer synchronises nothing across a queue.
    ///
    /// It terminates and is sound: placement depends only on pass ORDER, which re-scheduling never
    /// changes (it only moves submit boundaries), and an alias edge always points backwards in pass
    /// order by construction, so it cannot close a cycle.
    bool add_alias_edges() noexcept {
        for (const Placement& consumer : out.memory.placements) {
            for (const Placement& producer : out.memory.placements) {
                if (consumer.resource == producer.resource) {
                    continue;
                }
                if (!aliases(consumer.resource, producer.resource)) {
                    continue;
                }
                const PassId consumer_pass = order[consumer.first_pass];
                const PassId producer_pass = order[producer.last_pass];
                if (consumer_pass == producer_pass) {
                    continue;
                }
                bool exists = false;
                for (usize index = 0; index + 1 < alias_edges.size(); index += 2) {
                    if (alias_edges[index] == consumer_pass &&
                        alias_edges[index + 1] == producer_pass) {
                        exists = true;
                        break;
                    }
                }
                if (exists) {
                    continue;
                }
                if (!push(alias_edges, consumer_pass) || !push(alias_edges, producer_pass)) {
                    return false;
                }
                ++out.stats.alias_edges;
            }
        }
        return true;
    }

    // --- Barrier coalescing
    // ---------------------------------------------------------------------------

    /// Merge per-cell barriers into rectangles: contiguous layer runs per mip, then adjacent mips
    /// whose runs match. This is what turns the two-layer ownership release in M3's hard case into
    /// one barrier over layers [0, 2) rather than two.
    bool flush_image_barriers(Array<PendingImage>& pending, rhi::BarrierBatch& batch) noexcept {
        Array<u8> grid(allocator);
        Array<bool> consumed(allocator);
        if (Status sized = consumed.resize(pending.size()); !sized) {
            fail(ErrorCode::OutOfMemory, "the render graph could not coalesce barriers");
            return false;
        }
        for (bool& value : consumed) {
            value = false;
        }

        for (usize seed = 0; seed < pending.size(); ++seed) {
            if (consumed[seed]) {
                continue;
            }
            const ResourceId resource = pending[seed].resource;
            const BarrierKey key = pending[seed].key;
            const ResourceInfo& info = graph.resource(resource);
            const u16 mip_levels = info.texture.mip_levels;
            const u16 layers = info.texture.array_layers;

            if (Status sized = grid.resize(static_cast<usize>(mip_levels) * layers); !sized) {
                fail(ErrorCode::OutOfMemory, "the render graph could not coalesce barriers");
                return false;
            }
            for (u8& cell : grid) {
                cell = 0;
            }
            for (usize index = seed; index < pending.size(); ++index) {
                if (consumed[index] || pending[index].resource != resource ||
                    !(pending[index].key == key)) {
                    continue;
                }
                consumed[index] = true;
                grid[(static_cast<usize>(pending[index].mip) * layers) + pending[index].layer] = 1;
            }

            // Rows: a run of contiguous layers within one mip.
            struct Row {
                u16 mip = 0;
                u16 base = 0;
                u16 count = 0;
            };
            Array<Row> rows(allocator);
            for (u16 mip = 0; mip < mip_levels; ++mip) {
                u16 layer = 0;
                while (layer < layers) {
                    if (grid[(static_cast<usize>(mip) * layers) + layer] == 0) {
                        ++layer;
                        continue;
                    }
                    const u16 start = layer;
                    while (layer < layers &&
                           grid[(static_cast<usize>(mip) * layers) + layer] != 0) {
                        ++layer;
                    }
                    if (!push(rows, Row{mip, start, static_cast<u16>(layer - start)})) {
                        return false;
                    }
                }
            }

            usize index = 0;
            while (index < rows.size()) {
                const u16 base_mip = rows[index].mip;
                const u16 base_layer = rows[index].base;
                const u16 layer_count = rows[index].count;
                u16 mip_count = 1;
                usize next = index + 1;
                while (next < rows.size() && rows[next].mip == base_mip + mip_count &&
                       rows[next].base == base_layer && rows[next].count == layer_count) {
                    ++mip_count;
                    ++next;
                }

                rhi::ImageBarrier barrier;
                barrier.resource = resource;
                barrier.src_stage = key.src_stage;
                barrier.src_access = key.src_access;
                barrier.dst_stage = key.dst_stage;
                barrier.dst_access = key.dst_access;
                barrier.old_layout = key.old_layout;
                barrier.new_layout = key.new_layout;
                barrier.src_queue_family = key.src_queue_family;
                barrier.dst_queue_family = key.dst_queue_family;
                barrier.aspect = aspect_of(info);
                barrier.range = rhi::SubresourceRange{base_mip, mip_count, base_layer, layer_count};
                barrier.texture = info.imported_texture;
                if (Status pushed = batch.images.push_back(barrier); !pushed) {
                    fail(ErrorCode::OutOfMemory, "the render graph could not record a barrier");
                    return false;
                }
                ++out.stats.image_barriers;
                if (key.src_queue_family != key.dst_queue_family) {
                    ++out.stats.queue_ownership_transfers;
                }
                index = next;
            }
        }
        pending.clear();
        return true;
    }

    // --- Derivation
    // ------------------------------------------------------------------------------------

    bool derive() noexcept {
        Array<bool> alias_seen(allocator);
        if (Status sized = alias_seen.resize(graph.resource_count()); !sized) {
            fail(ErrorCode::OutOfMemory, "the render graph could not size its alias state");
            return false;
        }
        for (bool& value : alias_seen) {
            value = false;
        }

        Array<PendingImage> pre_pending(allocator);
        Array<PendingImage> acquire_pending(allocator);
        Array<PendingImage> release_pending(allocator);

        for (usize submit_index = 0; submit_index < out.submits.size(); ++submit_index) {
            for (usize scheduled_index = 0;
                 scheduled_index < out.submits[submit_index].passes.size(); ++scheduled_index) {
                const PassId pass = out.submits[submit_index].passes[scheduled_index].pass;
                const rhi::QueueKind queue = queue_of(pass);
                const u32 my_family = family_of(queue);

                pre_pending.clear();
                acquire_pending.clear();
                release_pending.clear();
                rhi::BarrierBatch pre;

                for (const Use& use : graph.pass_uses(pass)) {
                    const rhi::AccessInfo& access = rhi::access_info(use.access);
                    const ResourceInfo& info = graph.resource(use.resource);

                    if (!emit_alias_barrier(use, access, my_family, alias_seen, pre)) {
                        return false;
                    }

                    const u16 mips = info.is_texture ? use.range.mip_count : 1;
                    const u16 layers = info.is_texture ? use.range.layer_count : 1;
                    for (u16 mip_offset = 0; mip_offset < mips; ++mip_offset) {
                        for (u16 layer_offset = 0; layer_offset < layers; ++layer_offset) {
                            const auto mip = static_cast<u16>(use.range.base_mip + mip_offset);
                            const auto layer =
                                static_cast<u16>(use.range.base_layer + layer_offset);
                            if (!derive_cell(use, access, info, mip, layer, my_family,
                                             static_cast<i32>(submit_index), pre, pre_pending,
                                             acquire_pending, release_pending)) {
                                return false;
                            }
                        }
                    }

                    ResourceUse& record = resource_use[use.resource];
                    record.stage = access.stage;
                    record.access = access.access;
                    record.last_queue_family = my_family;
                }

                // Acquires first, then the ordinary transitions: an acquire brings the resource
                // into this queue's ownership, and a transition recorded before it would be a
                // transition performed by a queue that does not own the resource.
                if (!flush_image_barriers(acquire_pending, pre) ||
                    !flush_image_barriers(pre_pending, pre)) {
                    return false;
                }
                if (!flush_release_barriers(release_pending)) {
                    return false;
                }
                out.submits[submit_index].passes[scheduled_index].pre = std::move(pre);
            }
        }
        return true;
    }

    /// A transient that reuses memory a finished transient held needs one barrier before its first
    /// use — and validation will not catch its absence, so it is a structural property of this
    /// function rather than something a layer reports.
    bool emit_alias_barrier(const Use& use, const rhi::AccessInfo& access, u32 my_family,
                            Array<bool>& alias_seen, rhi::BarrierBatch& pre) noexcept {
        if (alias_seen[use.resource]) {
            return true;
        }
        alias_seen[use.resource] = true;

        rhi::MemoryBarrier barrier;
        barrier.dst_stage = access.stage;
        barrier.dst_access = access.access;
        bool any_predecessor = false;
        for (const Placement& placement : out.memory.placements) {
            if (placement.resource == use.resource || !aliases(use.resource, placement.resource)) {
                continue;
            }
            // A predecessor whose last use was on another queue is ordered by the semaphore edge
            // add_alias_edges() created. A barrier here would name stages in the wrong command
            // stream and synchronise nothing.
            if (resource_use[placement.resource].last_queue_family != my_family) {
                continue;
            }
            barrier.src_stage |= resource_use[placement.resource].stage;
            barrier.src_access |= resource_use[placement.resource].access;
            any_predecessor = true;
        }
        if (!any_predecessor || !rhi::any(barrier.src_stage)) {
            return true;
        }
        if (Status pushed = pre.memory.push_back(barrier); !pushed) {
            fail(ErrorCode::OutOfMemory, "the render graph could not record an alias barrier");
            return false;
        }
        ++out.stats.memory_barriers;
        ++out.stats.alias_barriers;
        return true;
    }

    bool derive_cell(const Use& use, const rhi::AccessInfo& access, const ResourceInfo& info,
                     u16 mip, u16 layer, u32 my_family, i32 submit_index, rhi::BarrierBatch& pre,
                     Array<PendingImage>& pre_pending, Array<PendingImage>& acquire_pending,
                     Array<PendingImage>& release_pending) noexcept {
        const usize index =
            info.is_texture ? cell_index(use.resource, mip, layer) : cell_base[use.resource];
        CellState& cell = cells[index];

        rhi::Stage source_stage = rhi::Stage::None;
        rhi::AccessFlags source_access = rhi::AccessFlags::None;
        if (access.is_write) {
            // Write-after-write and write-after-read. The read side contributes a STAGE but no
            // access bits: a write-after-read needs an execution dependency, not a memory one, and
            // naming the read's access would ask the implementation to flush caches nothing wrote.
            source_stage = cell.write_stage | cell.read_stage;
            source_access = cell.write_access;
        } else {
            source_stage = cell.write_stage;
            source_access = cell.write_access;
        }

        const bool layout_changes = info.is_texture && cell.layout != access.layout;
        const bool family_changes =
            cell.queue_family != rhi::kQueueFamilyIgnored && cell.queue_family != my_family;
        const bool needed = rhi::any(source_stage) || layout_changes || family_changes;

        if (needed) {
            if (family_changes && info.is_texture) {
                if (!emit_ownership_transfer(use, access, cell, mip, layer, my_family, source_stage,
                                             source_access, acquire_pending, release_pending)) {
                    return false;
                }
            } else if (family_changes) {
                if (!emit_buffer_ownership_transfer(use, access, cell, my_family, source_stage,
                                                    source_access, pre)) {
                    return false;
                }
            } else if (info.is_texture) {
                PendingImage pending;
                pending.resource = use.resource;
                pending.mip = mip;
                pending.layer = layer;
                pending.key.src_stage = source_stage;
                pending.key.src_access = source_access;
                pending.key.dst_stage = access.stage;
                pending.key.dst_access = access.access;
                pending.key.old_layout = cell.layout;
                pending.key.new_layout = access.layout;
                if (!push(pre_pending, pending)) {
                    return false;
                }
            } else if (rhi::any(source_stage)) {
                rhi::MemoryBarrier barrier;
                barrier.src_stage = source_stage;
                barrier.src_access = source_access;
                barrier.dst_stage = access.stage;
                barrier.dst_access = access.access;
                if (Status pushed = pre.memory.push_back(barrier); !pushed) {
                    fail(ErrorCode::OutOfMemory, "the render graph could not record a barrier");
                    return false;
                }
                ++out.stats.memory_barriers;
            }
        }

        cell.layout = info.is_texture ? access.layout : rhi::ImageLayout::Undefined;
        cell.queue_family = my_family;
        cell.last_submit = submit_index;
        if (access.is_write) {
            cell.write_stage = access.stage;
            cell.write_access = access.access;
            cell.read_stage = rhi::Stage::None;
            cell.read_access = rhi::AccessFlags::None;
        } else {
            cell.read_stage |= access.stage;
            cell.read_access |= access.access;
        }
        return true;
    }

    /// Both halves of an ownership transfer, derived from one hazard so they cannot drift.
    ///
    /// The ignored halves are written as ALL_COMMANDS rather than NONE, and that is not sloppiness.
    /// The specification ignores dstStage/dstAccess on the release and srcStage/srcAccess on the
    /// acquire, and NONE is the canonical spelling — but VVL 1.3.275 then models the release's
    /// layout transition as an unbarriered write and reports SYNC-HAZARD-WRITE-AFTER-WRITE against
    /// the acquire's transition even with a correct semaphore. M3's spike isolated it three ways;
    /// writing ALL_COMMANDS into the ignored halves silenced it at no cost, and the dropped-
    /// semaphore control still fired with it on, so it masks no real hazard. Re-test when the
    /// validation layer is newer than 1.3.275.
    bool emit_ownership_transfer(const Use& use, const rhi::AccessInfo& access,
                                 const CellState& cell, u16 mip, u16 layer, u32 my_family,
                                 rhi::Stage source_stage, rhi::AccessFlags source_access,
                                 Array<PendingImage>& acquire_pending,
                                 Array<PendingImage>& release_pending) noexcept {
        PendingImage release;
        release.resource = use.resource;
        release.mip = mip;
        release.layer = layer;
        release.key.src_stage = rhi::any(source_stage) ? source_stage : rhi::Stage::AllCommands;
        release.key.src_access = source_access;
        release.key.dst_stage = rhi::Stage::AllCommands;
        release.key.dst_access = rhi::AccessFlags::None;
        release.key.old_layout = cell.layout;
        release.key.new_layout = access.layout;
        release.key.src_queue_family = cell.queue_family;
        release.key.dst_queue_family = my_family;
        release.release_submit = cell.last_submit;

        PendingImage acquire = release;
        acquire.key.src_stage = rhi::Stage::AllCommands;
        acquire.key.src_access = rhi::AccessFlags::None;
        acquire.key.dst_stage = access.stage;
        acquire.key.dst_access = access.access;
        acquire.release_submit = -1;

        return push(release_pending, release) && push(acquire_pending, acquire);
    }

    bool emit_buffer_ownership_transfer(const Use& use, const rhi::AccessInfo& access,
                                        const CellState& cell, u32 my_family,
                                        rhi::Stage source_stage, rhi::AccessFlags source_access,
                                        rhi::BarrierBatch& pre) noexcept {
        const ResourceInfo& info = graph.resource(use.resource);

        rhi::BufferBarrier release;
        release.resource = use.resource;
        release.src_stage = rhi::any(source_stage) ? source_stage : rhi::Stage::AllCommands;
        release.src_access = source_access;
        release.dst_stage = rhi::Stage::AllCommands;
        release.dst_access = rhi::AccessFlags::None;
        release.src_queue_family = cell.queue_family;
        release.dst_queue_family = my_family;
        release.buffer = info.imported_buffer;

        rhi::BufferBarrier acquire = release;
        acquire.src_stage = rhi::Stage::AllCommands;
        acquire.src_access = rhi::AccessFlags::None;
        acquire.dst_stage = access.stage;
        acquire.dst_access = access.access;

        if (cell.last_submit >= 0) {
            rhi::BarrierBatch& batch = out.submits[static_cast<usize>(cell.last_submit)].release;
            if (Status pushed = batch.buffers.push_back(release); !pushed) {
                fail(ErrorCode::OutOfMemory, "the render graph could not record a release");
                return false;
            }
            ++out.stats.buffer_barriers;
            ++out.stats.queue_ownership_transfers;
        }
        if (Status pushed = pre.buffers.push_back(acquire); !pushed) {
            fail(ErrorCode::OutOfMemory, "the render graph could not record an acquire");
            return false;
        }
        ++out.stats.buffer_barriers;
        return true;
    }

    /// Releases are grouped by the submit that produced them, because that is the command buffer
    /// they have to be appended to.
    bool flush_release_barriers(Array<PendingImage>& release_pending) noexcept {
        Array<PendingImage> group(allocator);
        for (usize seed = 0; seed < release_pending.size(); ++seed) {
            const i32 submit = release_pending[seed].release_submit;
            if (submit < 0) {
                continue;
            }
            group.clear();
            for (usize index = seed; index < release_pending.size(); ++index) {
                if (release_pending[index].release_submit != submit) {
                    continue;
                }
                if (!push(group, release_pending[index])) {
                    return false;
                }
                release_pending[index].release_submit = -1;
            }
            if (!flush_image_barriers(group, out.submits[static_cast<usize>(submit)].release)) {
                return false;
            }
        }
        release_pending.clear();
        return true;
    }

    // --- The plan hash
    // ------------------------------------------------------------------------------------

    /// Every decision in the plan, folded into one number. design.md §6 requires deterministic
    /// submission; this is what makes "the same frame description produces the same plan" a test.
    void hash_plan() noexcept {
        u64 hash = kFnvOffset;
        for (const Submit& submit : out.submits) {
            hash = hash_value(hash, static_cast<u64>(submit.queue));
            hash = hash_value(hash, submit.signal_value);
            for (const SemaphoreWait& wait : submit.waits) {
                hash = hash_value(hash, static_cast<u64>(wait.queue));
                hash = hash_value(hash, wait.value);
                hash = hash_value(hash, static_cast<u64>(wait.stage));
            }
            for (const ScheduledPass& scheduled : submit.passes) {
                hash = hash_value(hash, scheduled.pass);
                hash = hash_batch(hash, scheduled.pre);
            }
            hash = hash_batch(hash, submit.release);
        }
        for (const Placement& placement : out.memory.placements) {
            hash = hash_value(hash, placement.resource);
            hash = hash_value(hash, placement.offset);
            hash = hash_value(hash, placement.size);
        }
        hash = hash_value(hash, out.memory.heap_bytes);
        for (const PassId pass : out.culled) {
            hash = hash_value(hash, pass);
        }
        out.plan_hash = hash;
    }

    static u64 hash_batch(u64 seed, const rhi::BarrierBatch& batch) noexcept {
        u64 hash = seed;
        for (const rhi::ImageBarrier& barrier : batch.images) {
            hash = hash_value(hash, barrier.resource);
            hash = hash_value(hash, static_cast<u64>(barrier.src_stage));
            hash = hash_value(hash, static_cast<u64>(barrier.src_access));
            hash = hash_value(hash, static_cast<u64>(barrier.dst_stage));
            hash = hash_value(hash, static_cast<u64>(barrier.dst_access));
            hash = hash_value(hash, static_cast<u64>(barrier.old_layout));
            hash = hash_value(hash, static_cast<u64>(barrier.new_layout));
            hash = hash_value(hash, barrier.src_queue_family);
            hash = hash_value(hash, barrier.dst_queue_family);
            hash = hash_value(hash, barrier.range.base_mip);
            hash = hash_value(hash, barrier.range.mip_count);
            hash = hash_value(hash, barrier.range.base_layer);
            hash = hash_value(hash, barrier.range.layer_count);
        }
        for (const rhi::BufferBarrier& barrier : batch.buffers) {
            hash = hash_value(hash, barrier.resource);
            hash = hash_value(hash, static_cast<u64>(barrier.src_stage));
            hash = hash_value(hash, static_cast<u64>(barrier.src_access));
            hash = hash_value(hash, static_cast<u64>(barrier.dst_stage));
            hash = hash_value(hash, static_cast<u64>(barrier.dst_access));
            hash = hash_value(hash, barrier.src_queue_family);
            hash = hash_value(hash, barrier.dst_queue_family);
        }
        for (const rhi::MemoryBarrier& barrier : batch.memory) {
            hash = hash_value(hash, static_cast<u64>(barrier.src_stage));
            hash = hash_value(hash, static_cast<u64>(barrier.src_access));
            hash = hash_value(hash, static_cast<u64>(barrier.dst_stage));
            hash = hash_value(hash, static_cast<u64>(barrier.dst_access));
        }
        return hash;
    }

    void collect_statistics() noexcept {
        out.stats.passes_declared = static_cast<u32>(graph.pass_count());
        out.stats.passes_culled = static_cast<u32>(out.culled.size());
        out.stats.submits = static_cast<u32>(out.submits.size());
        for (const Submit& submit : out.submits) {
            out.stats.semaphore_waits += static_cast<u32>(submit.waits.size());
        }
    }
};

Expected<CompiledGraph, Error> RenderGraph::compile(const CompileOptions& options) noexcept {
    if (!status_) {
        // A graph that failed to declare something compiles to nothing. Compiling anyway would
        // produce a plan with a hole in it, and the hole would surface as a missing barrier.
        return make_unexpected(status_.error());
    }

    Compiler compiler(*this, options);
    if (!compiler.init_cells() || !compiler.build_dependencies() || !compiler.cull() ||
        !compiler.schedule()) {
        return make_unexpected(compiler.status.error());
    }
    compiler.compute_lifetimes();
    if (!compiler.place_memory() || !compiler.add_alias_edges()) {
        return make_unexpected(compiler.status.error());
    }

    // The second scheduling pass. It exists only so that alias edges — which did not exist when the
    // first pass ran — can become semaphores rather than barriers that synchronise nothing.
    if (!compiler.alias_edges.empty()) {
        compiler.reset_schedule();
        if (!compiler.schedule()) {
            return make_unexpected(compiler.status.error());
        }
    }

    if (!compiler.derive()) {
        return make_unexpected(compiler.status.error());
    }
    compiler.collect_statistics();
    compiler.hash_plan();
    return std::move(compiler.out);
}

}  // namespace cy::rendering
