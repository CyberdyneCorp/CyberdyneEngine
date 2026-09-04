// Realising, recording and submitting a compiled graph. Tasks 2.2.4 to 2.2.7.
//
// The barrier-recording calls in this file are the only ones in the engine. They are reachable
// because GraphExecutor is the sole friend of rhi::GraphBarrierKey; the source-level gate in
// tools/layercheck/layercheck.py is the second half of the same rule and covers what a friend
// declaration cannot.

#include <cy/rendering/graph/executor.h>

#include <cy/core/base/assert.h>
#include <cy/core/jobs/job_system.h>
#include <cy/core/jobs/parallel.h>

#include <atomic>
#include <utility>

namespace cy::rendering {
namespace {

/// The memory query bound to a device. `user` is the device, and the resource has already been
/// realised — which is why this can ask the real allocator rather than estimate.
bool device_memory_query(ResourceId resource, const ResourceInfo& info,
                         rhi::MemoryRequirements& out, void* user) noexcept {
    auto* executor = static_cast<GraphExecutor*>(user);
    if (info.is_texture) {
        const rhi::TextureHandle handle = executor->texture(resource);
        if (handle.is_null()) {
            return false;
        }
        Expected<rhi::MemoryRequirements, Error> requirements =
            executor->device().texture_memory_requirements(handle);
        if (!requirements) {
            return false;
        }
        out = *requirements;
        return true;
    }
    const rhi::BufferHandle handle = executor->buffer(resource);
    if (handle.is_null()) {
        return false;
    }
    Expected<rhi::MemoryRequirements, Error> requirements =
        executor->device().buffer_memory_requirements(handle);
    if (!requirements) {
        return false;
    }
    out = *requirements;
    return true;
}

/// Whether a texture's usage admits a view at all.
///
/// A resource only ever copied to and from — a staging image, the readback target of a transfer
/// chain — has neither Sampled nor Storage nor an attachment usage, and Vulkan refuses a view of it
/// (VUID-VkImageViewCreateInfo-image-04441). Copies name the image directly, so there is nothing to
/// create. Found by running the graph on a device with the validation layers on, which is exactly
/// the kind of thing `rhi-and-render-graph` means by "a frame that renders but trips validation is
/// not a frame that works".
bool admits_a_view(rhi::TextureUsage usage) noexcept {
    return rhi::has_usage(usage, rhi::TextureUsage::Sampled) ||
           rhi::has_usage(usage, rhi::TextureUsage::Storage) ||
           rhi::has_usage(usage, rhi::TextureUsage::ColorAttachment) ||
           rhi::has_usage(usage, rhi::TextureUsage::DepthStencilAttachment) ||
           rhi::has_usage(usage, rhi::TextureUsage::InputAttachment) ||
           rhi::has_usage(usage, rhi::TextureUsage::TransientAttachment);
}

/// The state one parallel recording batch shares between workers.
///
/// EACH WORKER ACQUIRES ITS OWN COMMAND BUFFER, and that is not an implementation detail: a
/// backend's command allocator is externally synchronised, so the pool a buffer comes from must be
/// the pool of the thread that records into it. Acquiring them all on one thread and handing them
/// out is the shape that looks right and is a data race — it is what this executor did until a
/// repeated run of the parallel-recording test corrupted the heap.
///
/// Nothing else is shared: index `i` is written by exactly one worker, and `failed` is only ever
/// set from false to true.
struct RecordBatch {
    GraphExecutor* executor = nullptr;
    RenderGraph* graph = nullptr;
    rhi::Device* device = nullptr;
    const Submit* submit = nullptr;
    rhi::CommandBufferHandle* secondaries = nullptr;
    u32 first_schedule_index = 0;
    std::atomic<bool> failed{false};
};

}  // namespace

GraphExecutor::GraphExecutor(Allocator& allocator, rhi::Device& device) noexcept
    : allocator_(&allocator),
      device_(&device),
      textures_(allocator),
      buffers_(allocator),
      views_(allocator),
      submit_signals_(allocator) {}

GraphExecutor::~GraphExecutor() {
    release();
}

// --- Lookups
// ---------------------------------------------------------------------------------------

rhi::TextureHandle GraphExecutor::texture(ResourceId resource) const noexcept {
    return resource < textures_.size() ? textures_[resource] : rhi::TextureHandle{};
}

rhi::BufferHandle GraphExecutor::buffer(ResourceId resource) const noexcept {
    return resource < buffers_.size() ? buffers_[resource] : rhi::BufferHandle{};
}

rhi::TextureViewHandle GraphExecutor::view(ResourceId resource,
                                           rhi::SubresourceRange range) const noexcept {
    for (const ViewEntry& entry : views_) {
        if (entry.resource == resource && entry.range == range) {
            return entry.view;
        }
    }
    return rhi::TextureViewHandle{};
}

rhi::TextureViewHandle GraphExecutor::view(ResourceId resource) const noexcept {
    // The first view recorded for a resource is the one the first pass declared, which for a colour
    // or depth target is the whole image. A caller that needs a specific range asks for it.
    for (const ViewEntry& entry : views_) {
        if (entry.resource == resource) {
            return entry.view;
        }
    }
    return rhi::TextureViewHandle{};
}

// --- Realisation
// -------------------------------------------------------------------------------------

Status GraphExecutor::realise_resources(RenderGraph& graph) noexcept {
    release();

    if (Status sized = textures_.resize(graph.resource_count()); !sized) {
        return sized;
    }
    if (Status sized = buffers_.resize(graph.resource_count()); !sized) {
        return sized;
    }

    for (ResourceId id = 0; id < graph.resource_count(); ++id) {
        const ResourceInfo& info = graph.resource(id);
        if (info.imported) {
            textures_[id] = info.imported_texture;
            buffers_[id] = info.imported_buffer;
            continue;
        }
        if (info.is_texture) {
            rhi::TextureDescription description;
            description.name = info.texture.name;
            description.dimension = info.texture.dimension;
            description.format = info.texture.format;
            description.extent =
                rhi::Extent3D{info.texture.width, info.texture.height, info.texture.depth};
            description.mip_levels = info.texture.mip_levels;
            description.array_layers = info.texture.array_layers;
            description.sample_count = info.texture.sample_count;
            // The usage the declarations implied, unioned by the graph. A pass author never keeps a
            // usage flag in step with the passes written six months later.
            description.usage = info.texture_usage;
            Expected<rhi::TextureHandle, Error> handle =
                device_->create_transient_texture(description);
            if (!handle) {
                return make_unexpected(handle.error());
            }
            textures_[id] = *handle;
        } else {
            rhi::BufferDescription description;
            description.name = info.buffer.name;
            description.size = info.buffer.size;
            description.usage = info.buffer_usage;
            description.memory = info.buffer.memory;
            Expected<rhi::BufferHandle, Error> handle =
                device_->create_transient_buffer(description);
            if (!handle) {
                return make_unexpected(handle.error());
            }
            buffers_[id] = *handle;
        }
    }
    return ok();
}

Status GraphExecutor::bind_and_view(RenderGraph& graph, const CompiledGraph& plan) noexcept {
    if (Status reserved =
            device_->reserve_transient_memory(plan.memory.heap_bytes, plan.memory.memory_type_bits);
        !reserved) {
        return reserved;
    }
    for (const Placement& placement : plan.memory.placements) {
        const ResourceInfo& info = graph.resource(placement.resource);
        const Status bound =
            info.is_texture
                ? device_->bind_transient(textures_[placement.resource], placement.offset)
                : device_->bind_transient(buffers_[placement.resource], placement.offset);
        if (!bound) {
            return bound;
        }
    }

    // One view per DECLARED range, created after binding. Deriving it from the declaration is what
    // makes a descriptor naming a subresource the graph never transitioned unrepresentable.
    //
    // Only the passes that SURVIVED culling. A culled pass's transient is never placed and
    // therefore never bound, and a view needs bound memory — so walking every declared pass here
    // would fail on exactly the resource the graph decided nobody needed.
    for (const Submit& submit : plan.submits) {
        for (const ScheduledPass& scheduled : submit.passes) {
            for (const Use& use : graph.pass_uses(scheduled.pass)) {
                const ResourceInfo& info = graph.resource(use.resource);
                if (!info.is_texture || textures_[use.resource].is_null() ||
                    !admits_a_view(info.texture_usage)) {
                    continue;
                }
                if (!view(use.resource, use.range).is_null()) {
                    continue;
                }
                rhi::TextureViewDescription description;
                description.name = info.name;
                description.texture = textures_[use.resource];
                description.dimension = info.texture.dimension;
                description.format = info.texture.format;
                description.range = use.range;
                Expected<rhi::TextureViewHandle, Error> handle =
                    device_->create_texture_view(description);
                if (!handle) {
                    return make_unexpected(handle.error());
                }
                ViewEntry entry;
                entry.resource = use.resource;
                entry.range = use.range;
                entry.view = *handle;
                if (Status pushed = views_.push_back(entry); !pushed) {
                    return pushed;
                }
            }
        }
    }
    return ok();
}

void GraphExecutor::release() noexcept {
    for (const ViewEntry& entry : views_) {
        device_->destroy_texture_view(entry.view);
    }
    views_.clear();
    device_->release_transient_resources();
    textures_.clear();
    buffers_.clear();
    submit_signals_.clear();
    breadcrumb_next_ = 0;
}

// --- Recording
// ---------------------------------------------------------------------------------------

/// Fill in the backend handles the derivation deliberately left out. Derivation carries resource
/// ids only, which is what keeps it device-free and its output comparable between backends.
void GraphExecutor::patch_batch(RenderGraph& graph, rhi::BarrierBatch& batch) noexcept {
    for (rhi::ImageBarrier& barrier : batch.images) {
        if (barrier.resource < textures_.size()) {
            barrier.texture = textures_[barrier.resource];
        }
    }
    for (rhi::BufferBarrier& barrier : batch.buffers) {
        if (barrier.resource < buffers_.size()) {
            barrier.buffer = buffers_[barrier.resource];
            const ResourceInfo& info = graph.resource(barrier.resource);
            if (barrier.size == 0) {
                barrier.size = info.buffer.size;
            }
        }
    }
}

Status GraphExecutor::record_pass(RenderGraph& graph, PassId pass, u32 schedule_index,
                                  rhi::CommandBufferHandle command_buffer) noexcept {
    rhi::CommandBuffer* commands = device_->command_buffer(command_buffer);
    if (commands == nullptr) {
        return fail(ErrorCode::NotFound, "the graph executor was given a stale command buffer");
    }
    const RecordFn record = graph.pass_record_function(pass);
    if (record == nullptr) {
        // A pass with no callback is legitimate and common: a recordless side-effecting pass is how
        // a host boundary is declared (rhi::Access::HostRead), and it exists so that the graph
        // emits the transfer-to-host barrier rather than the caller relying on coherent memory and
        // a fence.
        return ok();
    }
    PassContext context;
    context.commands = commands;
    context.executor = this;
    context.pass = pass;
    context.schedule_index = schedule_index;
    record(context, graph.pass_record_user(pass));
    return ok();
}

Status GraphExecutor::record_and_submit(RenderGraph& graph, CompiledGraph& plan,
                                        const ExecuteOptions& options,
                                        ExecutionResult& result) noexcept {
    if (Status sized = submit_signals_.resize(plan.submits.size()); !sized) {
        return sized;
    }

    // The passkey. Constructing one is what makes this class the only barrier emitter in the
    // engine; its constructor is private and GraphExecutor is its only friend.
    const rhi::GraphBarrierKey barrier_key;
    rhi::BarrierRecorder& barriers = device_->barrier_recorder(barrier_key);

    Array<rhi::CommandBufferHandle> secondaries(*allocator_);

    for (usize submit_index = 0; submit_index < plan.submits.size(); ++submit_index) {
        Submit& submit = plan.submits[submit_index];

        Expected<rhi::CommandBufferHandle, Error> primary =
            device_->acquire_command_buffer(submit.queue, false);
        if (!primary) {
            return make_unexpected(primary.error());
        }
        if (Status begun = device_->begin_command_buffer(*primary); !begun) {
            return begun;
        }

        // Parallel recording: one secondary per pass, recorded on job workers and executed in PLAN
        // order. The workers decide when a pass is recorded, never where its commands end up, which
        // is what keeps the command stream identical regardless of thread scheduling.
        const bool parallel = options.parallel_recording && options.job_system != nullptr &&
                              submit.passes.size() >= options.parallel_pass_threshold;
        secondaries.clear();
        if (parallel) {
            if (Status sized = secondaries.resize(submit.passes.size()); !sized) {
                return sized;
            }
            RecordBatch batch;
            batch.executor = this;
            batch.graph = &graph;
            batch.device = device_;
            batch.submit = &submit;
            batch.secondaries = secondaries.data();
            batch.first_schedule_index = result.passes_recorded;

            auto body = [&batch](const jobs::TaskContext& context, u64 begin, u64 end) noexcept {
                (void)context;
                for (u64 index = begin; index < end; ++index) {
                    const ScheduledPass& scheduled = batch.submit->passes[index];
                    // Acquired HERE, on the worker, so the command pool is this thread's.
                    Expected<rhi::CommandBufferHandle, Error> secondary =
                        batch.device->acquire_command_buffer(batch.submit->queue, true);
                    if (!secondary || !batch.device->begin_command_buffer(*secondary)) {
                        batch.failed.store(true, std::memory_order_relaxed);
                        continue;
                    }
                    batch.secondaries[index] = *secondary;
                    if (!batch.executor->record_pass(
                            *batch.graph, scheduled.pass,
                            batch.first_schedule_index + static_cast<u32>(index), *secondary)) {
                        batch.failed.store(true, std::memory_order_relaxed);
                    }
                    if (!batch.device->end_command_buffer(*secondary)) {
                        batch.failed.store(true, std::memory_order_relaxed);
                    }
                }
            };
            if (Status ran = jobs::parallel_for(*options.job_system, submit.passes.size(), 1, body,
                                                "render-graph.record");
                !ran) {
                return ran;
            }
            if (batch.failed.load(std::memory_order_relaxed)) {
                return fail(
                    ErrorCode::Internal,
                    "a pass could not be recorded on a job worker; the most likely cause is "
                    "more recording threads than rhi::kMaxRecordingThreads");
            }
            result.secondary_command_buffers += static_cast<u32>(secondaries.size());
        }

        for (usize index = 0; index < submit.passes.size(); ++index) {
            ScheduledPass& scheduled = submit.passes[index];

            // THE BARRIERS. Derived, patched, recorded — and recorded here, in the primary, before
            // the pass's own commands, whether the pass was recorded sequentially or on a worker.
            patch_batch(graph, scheduled.pre);
            if (!scheduled.pre.empty()) {
                barriers.record_barriers(*primary, scheduled.pre);
                ++result.barrier_batches;
                result.barriers += static_cast<u32>(scheduled.pre.count());
                for (const rhi::ImageBarrier& barrier : scheduled.pre.images) {
                    if (barrier.src_queue_family != barrier.dst_queue_family) {
                        ++result.queue_ownership_transfers;
                    }
                }
            }

            rhi::CommandBuffer* commands = device_->command_buffer(*primary);
            if (commands == nullptr) {
                return fail(ErrorCode::NotFound, "the frame's primary command buffer went stale");
            }
            if (options.debug_labels) {
                commands->begin_debug_label(graph.pass_name(scheduled.pass));
            }
            if (options.breadcrumbs) {
                // One 32-bit write per pass into a device-visible buffer. What a device-loss report
                // reads back to name the pass the GPU died in, when the trace tail did not survive.
                commands->write_breadcrumb(breadcrumb_next_, result.passes_recorded + 1);
                ++breadcrumb_next_;
            }

            if (parallel) {
                if (Status executed = device_->execute_secondary(
                        *primary,
                        Span<const rhi::CommandBufferHandle>(secondaries.data() + index, 1));
                    !executed) {
                    return executed;
                }
            } else if (Status recorded =
                           record_pass(graph, scheduled.pass, result.passes_recorded, *primary);
                       !recorded) {
                return recorded;
            }

            if (options.debug_labels) {
                commands->end_debug_label();
            }
            ++result.passes_recorded;
        }

        // Queue-family ownership RELEASES go at the very end of the producing submit's command
        // buffer. Their acquire halves are already in the consuming pass's pre-batch, and the
        // semaphore between the two submits is what orders them.
        patch_batch(graph, submit.release);
        if (!submit.release.empty()) {
            barriers.record_barriers(*primary, submit.release);
            ++result.barrier_batches;
            result.barriers += static_cast<u32>(submit.release.count());
            result.queue_ownership_transfers +=
                static_cast<u32>(submit.release.images.size() + submit.release.buffers.size());
        }

        if (Status ended = device_->end_command_buffer(*primary); !ended) {
            return ended;
        }

        rhi::SubmitInfo info;
        info.queue = submit.queue;
        info.command_buffers = Span<const rhi::CommandBufferHandle>(&*primary, 1);

        Array<rhi::TimelineWait> waits(*allocator_);
        for (const SemaphoreWait& wait : submit.waits) {
            // The plan counts a queue's signals from 1 each frame; the device's timeline is
            // monotonic for the device's life. Translate through the submit that produced it.
            u64 actual = 0;
            for (usize other = 0; other < submit_index; ++other) {
                if (plan.submits[other].queue == wait.queue &&
                    plan.submits[other].signal_value == wait.value) {
                    actual = submit_signals_[other];
                    break;
                }
            }
            if (actual == 0) {
                return fail(ErrorCode::Internal,
                            "a compiled graph waits on a submit that has not been made — the "
                            "scheduler emitted an edge that does not point backwards");
            }
            rhi::TimelineWait translated;
            translated.queue = wait.queue;
            translated.value = actual;
            translated.stage = wait.stage;
            if (Status pushed = waits.push_back(translated); !pushed) {
                return pushed;
            }
        }
        info.waits = Span<const rhi::TimelineWait>(waits.data(), waits.size());

        // Presentation's binary semaphores attach to the first and last submits: the acquire is
        // waited before anything writes the swapchain image, the present is signalled when the last
        // submit completes.
        if (submit_index == 0) {
            info.wait_binary = options.wait_acquire;
        }
        if (submit_index + 1 == plan.submits.size()) {
            info.signal_binary = options.signal_present;
            info.signal_fence = options.signal_fence;
        }

        Expected<u64, Error> signalled = device_->submit(info);
        if (!signalled) {
            return make_unexpected(signalled.error());
        }
        submit_signals_[submit_index] = *signalled;
        result.timeline_signalled[static_cast<u32>(submit.queue)] = *signalled;
        ++result.submits;
    }
    return ok();
}

// --- Entry points
// ---------------------------------------------------------------------------------------

Expected<CompiledGraph, Error> GraphExecutor::compile_only(
    RenderGraph& graph, CompileOptions compile_options) noexcept {
    if (Status realised = realise_resources(graph); !realised) {
        return make_unexpected(realised.error());
    }
    if (compile_options.query_memory == nullptr) {
        compile_options.query_memory = &device_memory_query;
        compile_options.query_user = this;
    }
    Expected<CompiledGraph, Error> plan = graph.compile(compile_options);
    if (!plan) {
        return plan;
    }
    if (Status bound = bind_and_view(graph, *plan); !bound) {
        return make_unexpected(bound.error());
    }
    return plan;
}

Expected<ExecutionResult, Error> GraphExecutor::execute(RenderGraph& graph,
                                                        CompileOptions compile_options,
                                                        const ExecuteOptions& options) noexcept {
    // The device's queue configuration, unless the caller has already stated one. A graph test
    // states its own so that it can exercise a two-queue plan against a one-queue device; a frame
    // does not.
    if (!compile_options.queue_available[static_cast<u32>(rhi::QueueKind::AsyncCompute)] &&
        !compile_options.queue_available[static_cast<u32>(rhi::QueueKind::Transfer)]) {
        for (u32 index = 0; index < rhi::kQueueKindCount; ++index) {
            const auto queue = static_cast<rhi::QueueKind>(index);
            compile_options.queue_available[index] = device_->has_queue(queue);
            compile_options.queue_family[index] = device_->queue_family(queue);
        }
        compile_options.queue_available[static_cast<u32>(rhi::QueueKind::Graphics)] = true;
    }

    Expected<CompiledGraph, Error> plan = compile_only(graph, compile_options);
    if (!plan) {
        return make_unexpected(plan.error());
    }

    ExecutionResult result;
    result.passes_culled = static_cast<u32>(plan->culled.size());
    result.transient_bytes = plan->memory.heap_bytes;
    result.transient_bytes_without_aliasing = plan->memory.naive_bytes;
    result.plan_hash = plan->plan_hash;

    if (Status recorded = record_and_submit(graph, *plan, options, result); !recorded) {
        return make_unexpected(recorded.error());
    }
    return result;
}

}  // namespace cy::rendering
