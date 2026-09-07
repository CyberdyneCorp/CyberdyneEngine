#include <cy/rendering/virtual_geometry/gpu.h>

#include <cstring>

#include "../shaders/vg_spirv.h"

namespace cy::rendering::vg {

namespace {

/// The workgroup size every pass but `vgPrepare` is compiled with. It appears here because the host
/// computes group counts for the two dispatches whose extent it knows — the reset and the instance
/// cull — and the shader's `numthreads` is the other half of that arithmetic.
constexpr u32 kGroupSize = 64;

/// Counter slots, matching `vg_traversal.slang`'s `kCount*` constants. Duplicated because there is
/// no way to share a constant with Slang, and checked by the suite: `test_gpu_traversal.cpp`
/// compares every one of these against the reference's own count.
enum CounterSlot : u32 {
    kCountQueueA = 0,
    kCountQueueB,
    kCountVisible,
    kCountRequests,
    kCountInstancesVisible,
    kCountNodesVisited,
    kCountPrunedFrustum,
    kCountRejectedCone,
    kCountRejectedFrustum,
    kCountRejectedSize,
    kCountVisibleTriangles,
    kCountMissingPages,
    kCounterWords,
};

/// The binding order of the one descriptor set, matching the `[[vk::binding(n, 0)]]` attributes.
enum Binding : u32 {
    kBindClusters = 0,
    kBindInstances,
    kBindAssets,
    kBindRoots,
    kBindChildren,
    kBindPageTable,
    kBindQueueA,
    kBindQueueB,
    kBindCounters,
    kBindDispatchArgs,
    kBindVisible,
    kBindRequests,
    kBindVisited,
    kBindClusterTriangles,
    kBindingCount,
};

/// Bytes of indirect dispatch arguments: three words per queue parity.
constexpr u64 kDispatchArgsBytes = 6 * sizeof(u32);

}  // namespace

GpuScene::GpuScene(Allocator& allocator) noexcept
    : clusters(allocator),
      cluster_triangles(allocator),
      assets(allocator),
      roots(allocator),
      children(allocator),
      instances(allocator) {}

Expected<u32, Error> GpuScene::add_asset(const DecodedAsset& asset) noexcept {
    GpuAsset record;
    record.cluster_base = static_cast<u32>(clusters.size());
    record.cluster_count = static_cast<u32>(asset.clusters.size());
    record.root_first = static_cast<u32>(roots.size());
    record.page_base = 0;
    record.page_count = static_cast<u32>(asset.pages.size());
    record.child_base = static_cast<u32>(children.size());
    // The page base is the sum of every earlier asset's page count, which is exactly what
    // `GeometryCache` assigns when the assets are registered in the same order. The suite asserts
    // the two agree rather than trusting the coincidence.
    for (const GpuAsset& earlier : assets) {
        record.page_base += earlier.page_count;
    }

    if (Status packed = pack_clusters(asset, clusters); !packed) {
        return make_unexpected(packed.error());
    }
    for (const Cluster& cluster : asset.clusters) {
        if (Status pushed = cluster_triangles.push_back(cluster.index_count / 3U); !pushed) {
            return make_unexpected(pushed.error());
        }
    }
    for (u32 index = 0; index < asset.clusters.size(); ++index) {
        if (asset.clusters[index].parent_error < kRootError) {
            continue;
        }
        if (Status pushed = roots.push_back(index); !pushed) {
            return make_unexpected(pushed.error());
        }
    }
    record.root_count = static_cast<u32>(roots.size()) - record.root_first;
    if (record.root_count == 0) {
        return fail(ErrorCode::InvalidArgument,
                    "GpuScene::add_asset: the asset has no root cluster, so traversal would have "
                    "nothing to start from");
    }
    if (Status appended = children.append(asset.cluster_children.span()); !appended) {
        return make_unexpected(appended.error());
    }

    const u32 index = static_cast<u32>(assets.size());
    if (Status pushed = assets.push_back(record); !pushed) {
        return make_unexpected(pushed.error());
    }
    return index;
}

Status GpuScene::set_instances(Span<const GeometryInstance> source) noexcept {
    instances.clear();
    return pack_instances(source, instances);
}

u32 GpuScene::cluster_stride() const noexcept {
    u32 widest = 0;
    for (const GpuAsset& asset : assets) {
        widest = asset.cluster_count > widest ? asset.cluster_count : widest;
    }
    return widest;
}

TraversalReadback::TraversalReadback(Allocator& allocator) noexcept
    : visible(allocator), requests(allocator) {}

GpuTraversal::GpuTraversal(Allocator& allocator, rhi::Device& device) noexcept
    : device_(device), states_(allocator) {}

GpuTraversal::~GpuTraversal() {
    // The device destroys deferred, so this is safe to call while a frame that used these buffers
    // is still in flight — `rhi-and-render-graph` requires a resource destroyed during frame N to
    // be released only after frame N's fence. That is what makes `test_gpu_teardown.cpp`'s
    // destroy-under-load loop a test of the module rather than of the caller's patience.
    for (const rhi::ComputePipelineHandle pipeline :
         {reset_, instance_cull_, prepare_[0], prepare_[1], traverse_[0], traverse_[1]}) {
        device_.destroy_compute_pipeline(pipeline);
    }
    for (const rhi::ShaderModuleHandle module : modules_) {
        device_.destroy_shader_module(module);
    }
    device_.destroy_pipeline_layout(pipeline_layout_);
    device_.destroy_descriptor_set_layout(set_layout_);
    for (const rhi::BufferHandle buffer :
         {clusters_, cluster_triangles_, instances_, assets_, roots_, children_, page_table_,
          queue_a_, queue_b_, counters_, dispatch_args_, visible_, requests_, visited_, staging_,
          readback_}) {
        device_.destroy_buffer(buffer);
    }
}

Expected<rhi::BufferHandle, Error> GpuTraversal::make_buffer(const char* name, u64 bytes,
                                                             rhi::BufferUsage usage,
                                                             rhi::MemoryUse memory) noexcept {
    rhi::BufferDescription description;
    description.name = name;
    description.size = bytes > 0 ? bytes : 4;
    description.usage = usage;
    description.memory = memory;
    return device_.create_buffer(description);
}

Status GpuTraversal::stage(u32 slot, const void* data, u64 bytes) noexcept {
    // ============================================================================================
    // AN UPLOAD IS A GRAPH PASS, NOT A SUBMIT OF ITS OWN
    // ============================================================================================
    //
    // The first shape of this function copied into the target and submitted, waiting on the
    // timeline before returning. It was wrong, and synchronisation validation said so on the first
    // run: SYNC-HAZARD-READ-AFTER-WRITE, a `vkCmdDispatch` reading what a `vkCmdCopyBuffer` in an
    // earlier submit had written with `write_barriers: 0`. A host-side wait proves the copy
    // COMPLETED; it does not make the write VISIBLE to a shader read, and the two are different
    // things that a fence does not connect.
    //
    // The remedy is the module's own rule applied to itself: nothing here may emit a barrier, so
    // the copy is declared into the graph beside the dispatches that read it and the graph derives
    // the transfer-to-compute dependency. `stage()` therefore writes into a region of the staging
    // buffer and marks the target dirty; `record()` turns whatever is dirty into one transfer pass.
    auto* mapped = static_cast<u8*>(device_.buffer_mapped_pointer(staging_));
    if (mapped == nullptr) {
        return fail(ErrorCode::Internal, "GpuTraversal::stage: the staging buffer is not mapped");
    }
    Upload& upload = uploads_[slot];
    if (bytes > upload.capacity) {
        return fail(ErrorCode::BufferTooSmall,
                    "GpuTraversal::stage: more bytes than this buffer was created for");
    }
    if (bytes > 0) {
        std::memcpy(mapped + upload.staging_offset, data, bytes);
    }
    upload.bytes = bytes;
    upload.dirty = bytes > 0;
    return ok();
}

Expected<rhi::ComputePipelineHandle, Error> GpuTraversal::make_pipeline(const char* name,
                                                                        Span<const u32> spirv,
                                                                        u32 parity) noexcept {
    rhi::ShaderModuleDescription module_description;
    module_description.name = name;
    module_description.stage = rhi::ShaderStage::Compute;
    module_description.spirv = spirv;
    module_description.entry_point = "main";
    Expected<rhi::ShaderModuleHandle, Error> module =
        device_.create_shader_module(module_description);
    if (!module) {
        return make_unexpected(module.error());
    }

    // The ping-pong step is a SPECIALIZATION CONSTANT rather than a push field:
    // `rhi-and-render-graph` — "a feature that can be a specialization constant SHALL be one" — and
    // it means the two directions are two pipelines over one module instead of a branch every
    // thread in every level takes.
    const rhi::SpecializationConstant specialization{0, parity};
    rhi::ComputePipelineDescription description;
    description.name = name;
    description.layout = pipeline_layout_;
    description.shader = *module;
    description.specialization = Span<const rhi::SpecializationConstant>(&specialization, 1);
    Expected<rhi::ComputePipelineHandle, Error> pipeline =
        device_.create_compute_pipeline(description);
    if (!pipeline) {
        device_.destroy_shader_module(*module);
        return make_unexpected(pipeline.error());
    }
    // The module is kept and destroyed with everything else: the RHI owns lifetimes by handle, so
    // releasing it here would be legal in Vulkan and wrong in this model.
    if (module_count_ < 8) {
        modules_[module_count_] = *module;
        ++module_count_;
    }
    return pipeline;
}

Status GpuTraversal::initialise(const GpuScene& scene,
                                const GpuTraversalOptions& options) noexcept {
    if (initialised_) {
        return fail(ErrorCode::AlreadyExists, "GpuTraversal::initialise: already initialised");
    }
    if (scene.assets.empty() || scene.clusters.empty()) {
        return fail(ErrorCode::InvalidArgument,
                    "GpuTraversal::initialise: the scene has no assets");
    }
    options_ = options;
    cluster_stride_ = scene.cluster_stride();

    const u64 marks = static_cast<u64>(scene.instances.size()) * cluster_stride_;
    if (marks > (1ULL << 26U)) {
        return fail(ErrorCode::OutOfRange,
                    "GpuTraversal::initialise: the DAG visit marks are one word per (instance, "
                    "cluster) and this scene needs more than 64 Mi of them; the growth path is a "
                    "hash, and refusing here is better than overflowing on the device");
    }

    // --- The buffers -----------------------------------------------------------------------
    struct Plan {
        rhi::BufferHandle* target;
        const char* name;
        u64 bytes;
        rhi::BufferUsage usage;
        rhi::MemoryUse memory;
    };
    const rhi::BufferUsage storage =
        rhi::BufferUsage::Storage | rhi::BufferUsage::TransferDestination;
    const u64 counter_bytes = kCounterWords * sizeof(u32);
    const u64 readback_bytes =
        counter_bytes + (static_cast<u64>(options.visible_capacity) * sizeof(VisibleCluster)) +
        (static_cast<u64>(options.request_capacity) * sizeof(PageRequest));
    // Every buffer that is ever uploaded gets its own region of one staging allocation, so that a
    // frame which re-stages the page table and the instances together is one transfer pass rather
    // than two overlapping writes into a shared scratch.
    const u64 page_table_bytes = 4096 * sizeof(PageTableEntry);
    const u64 upload_bytes[kUploadCount] = {
        scene.clusters.size() * sizeof(GpuCluster),
        scene.cluster_triangles.size() * sizeof(u32),
        (scene.instances.empty() ? 1 : scene.instances.size()) * sizeof(GpuInstance),
        scene.assets.size() * sizeof(GpuAsset),
        scene.roots.size() * sizeof(u32),
        (scene.children.empty() ? 1 : scene.children.size()) * sizeof(u32),
        page_table_bytes,
    };
    u64 staging_bytes = 0;
    for (u32 index = 0; index < kUploadCount; ++index) {
        uploads_[index].staging_offset = staging_bytes;
        uploads_[index].capacity = upload_bytes[index];
        staging_bytes += upload_bytes[index];
    }

    const Plan plan[] = {
        {&clusters_, "vg.clusters", upload_bytes[kUploadClusters], storage,
         rhi::MemoryUse::DeviceLocal},
        {&cluster_triangles_, "vg.cluster-triangles", upload_bytes[kUploadClusterTriangles],
         storage, rhi::MemoryUse::DeviceLocal},
        {&instances_, "vg.instances", upload_bytes[kUploadInstances], storage,
         rhi::MemoryUse::DeviceLocal},
        {&assets_, "vg.assets", upload_bytes[kUploadAssets], storage, rhi::MemoryUse::DeviceLocal},
        {&roots_, "vg.roots", upload_bytes[kUploadRoots], storage, rhi::MemoryUse::DeviceLocal},
        {&children_, "vg.children", upload_bytes[kUploadChildren], storage,
         rhi::MemoryUse::DeviceLocal},
        {&page_table_, "vg.page-table", page_table_bytes, storage, rhi::MemoryUse::DeviceLocal},
        {&queue_a_, "vg.queue-a", static_cast<u64>(options.queue_capacity) * sizeof(GpuNode),
         storage, rhi::MemoryUse::DeviceLocal},
        {&queue_b_, "vg.queue-b", static_cast<u64>(options.queue_capacity) * sizeof(GpuNode),
         storage, rhi::MemoryUse::DeviceLocal},
        {&counters_, "vg.counters", counter_bytes,
         storage | rhi::BufferUsage::TransferSource | rhi::BufferUsage::Indirect,
         rhi::MemoryUse::DeviceLocal},
        {&dispatch_args_, "vg.dispatch-args", kDispatchArgsBytes,
         storage | rhi::BufferUsage::Indirect, rhi::MemoryUse::DeviceLocal},
        {&visible_, "vg.visible",
         static_cast<u64>(options.visible_capacity) * sizeof(GpuVisibleCluster),
         storage | rhi::BufferUsage::TransferSource, rhi::MemoryUse::DeviceLocal},
        {&requests_, "vg.requests",
         static_cast<u64>(options.request_capacity) * sizeof(PageRequest),
         storage | rhi::BufferUsage::TransferSource, rhi::MemoryUse::DeviceLocal},
        {&visited_, "vg.visited", (marks == 0 ? 1 : marks) * sizeof(u32), storage,
         rhi::MemoryUse::DeviceLocal},
        {&staging_, "vg.staging", staging_bytes, rhi::BufferUsage::TransferSource,
         rhi::MemoryUse::Upload},
        {&readback_, "vg.readback", readback_bytes, rhi::BufferUsage::TransferDestination,
         rhi::MemoryUse::Readback},
    };
    for (const Plan& entry : plan) {
        Expected<rhi::BufferHandle, Error> buffer =
            make_buffer(entry.name, entry.bytes, entry.usage, entry.memory);
        if (!buffer) {
            return make_unexpected(buffer.error());
        }
        *entry.target = *buffer;
    }

    // --- The layout ------------------------------------------------------------------------
    rhi::DescriptorBinding bindings[kBindingCount];
    for (u32 index = 0; index < kBindingCount; ++index) {
        bindings[index] = rhi::DescriptorBinding{};
        bindings[index].binding = index;
        bindings[index].kind = rhi::DescriptorKind::StorageBuffer;
        bindings[index].count = 1;
        bindings[index].stages = rhi::ShaderStage::Compute;
    }
    rhi::DescriptorSetLayoutDescription layout_description;
    layout_description.name = "vg.traversal";
    layout_description.bindings = Span<const rhi::DescriptorBinding>(bindings, kBindingCount);
    Expected<rhi::DescriptorSetLayoutHandle, Error> set_layout =
        device_.create_descriptor_set_layout(layout_description);
    if (!set_layout) {
        return make_unexpected(set_layout.error());
    }
    set_layout_ = *set_layout;

    rhi::PushConstantRange push;
    push.stages = rhi::ShaderStage::Compute;
    push.offset = 0;
    push.size = sizeof(GpuView);
    rhi::PipelineLayoutDescription pipeline_layout_description;
    pipeline_layout_description.name = "vg.traversal";
    pipeline_layout_description.set_layouts =
        Span<const rhi::DescriptorSetLayoutHandle>(&set_layout_, 1);
    pipeline_layout_description.push_constants = Span<const rhi::PushConstantRange>(&push, 1);
    Expected<rhi::PipelineLayoutHandle, Error> pipeline_layout =
        device_.create_pipeline_layout(pipeline_layout_description);
    if (!pipeline_layout) {
        return make_unexpected(pipeline_layout.error());
    }
    pipeline_layout_ = *pipeline_layout;

    Expected<rhi::DescriptorSetHandle, Error> descriptors =
        device_.allocate_descriptor_set(set_layout_, false);
    if (!descriptors) {
        return make_unexpected(descriptors.error());
    }
    descriptors_ = *descriptors;

    const rhi::BufferHandle bound[kBindingCount] = {
        clusters_, instances_,        assets_,   roots_,         children_, page_table_,
        queue_a_,  queue_b_,          counters_, dispatch_args_, visible_,  requests_,
        visited_,  cluster_triangles_};
    rhi::DescriptorWrite writes[kBindingCount];
    for (u32 index = 0; index < kBindingCount; ++index) {
        writes[index] = rhi::DescriptorWrite{};
        writes[index].binding = index;
        writes[index].kind = rhi::DescriptorKind::StorageBuffer;
        writes[index].buffer = bound[index];
    }
    if (Status updated = device_.update_descriptor_set(
            descriptors_, Span<const rhi::DescriptorWrite>(writes, kBindingCount));
        !updated) {
        return updated;
    }

    // --- The pipelines ---------------------------------------------------------------------
    struct PipelinePlan {
        rhi::ComputePipelineHandle* target;
        const char* name;
        Span<const u32> spirv;
        u32 parity;
    };
    const PipelinePlan pipelines[] = {
        {&reset_, "vg.reset", Span<const u32>(kVgResetSpirv), 0},
        {&instance_cull_, "vg.instance-cull", Span<const u32>(kVgInstanceCullSpirv), 0},
        {&prepare_[0], "vg.prepare.a", Span<const u32>(kVgPrepareSpirv), 0},
        {&prepare_[1], "vg.prepare.b", Span<const u32>(kVgPrepareSpirv), 1},
        {&traverse_[0], "vg.traverse.a", Span<const u32>(kVgTraverseSpirv), 0},
        {&traverse_[1], "vg.traverse.b", Span<const u32>(kVgTraverseSpirv), 1},
    };
    for (const PipelinePlan& entry : pipelines) {
        Expected<rhi::ComputePipelineHandle, Error> pipeline =
            make_pipeline(entry.name, entry.spirv, entry.parity);
        if (!pipeline) {
            return make_unexpected(pipeline.error());
        }
        *entry.target = *pipeline;
    }

    // --- The static half of the scene, staged for the first frame's upload pass ----------------
    const struct {
        u32 slot;
        const void* data;
        u64 bytes;
    } staged[] = {
        {kUploadClusters, scene.clusters.data(), scene.clusters.size() * sizeof(GpuCluster)},
        {kUploadClusterTriangles, scene.cluster_triangles.data(),
         scene.cluster_triangles.size() * sizeof(u32)},
        {kUploadAssets, scene.assets.data(), scene.assets.size() * sizeof(GpuAsset)},
        {kUploadRoots, scene.roots.data(), scene.roots.size() * sizeof(u32)},
        {kUploadChildren, scene.children.data(), scene.children.size() * sizeof(u32)},
    };
    for (const auto& entry : staged) {
        if (Status ready = stage(entry.slot, entry.data, entry.bytes); !ready) {
            return ready;
        }
    }
    if (!scene.instances.empty()) {
        if (Status ready = upload_instances(scene.instances.span()); !ready) {
            return ready;
        }
    }
    initialised_ = true;
    return ok();
}

Status GpuTraversal::upload_instances(Span<const GpuInstance> instances) noexcept {
    return stage(kUploadInstances, instances.data(), instances.size() * sizeof(GpuInstance));
}

Status GpuTraversal::upload_page_table(Span<const PageTableEntry> table) noexcept {
    return stage(kUploadPageTable, table.data(), table.size() * sizeof(PageTableEntry));
}

void GpuTraversal::dispatch(const PassContext& context, const PassState& state) noexcept {
    rhi::CommandBuffer& commands = *context.commands;
    if (state.pass == 4) {
        const rhi::BufferHandle targets[kUploadCount] = {
            clusters_, cluster_triangles_, instances_, assets_, roots_, children_, page_table_};
        for (u32 slot = 0; slot < kUploadCount; ++slot) {
            if ((state.upload_mask & (1U << slot)) == 0U) {
                continue;
            }
            rhi::BufferCopy region;
            region.source_offset = uploads_[slot].staging_offset;
            region.destination_offset = 0;
            region.size = uploads_[slot].bytes;
            commands.copy_buffer(staging_, targets[slot], Span<const rhi::BufferCopy>(&region, 1));
        }
        return;
    }
    rhi::ComputePipelineHandle pipeline = traverse_[state.parity];
    if (state.pass == 0) {
        pipeline = reset_;
    } else if (state.pass == 1) {
        pipeline = instance_cull_;
    } else if (state.pass == 2) {
        pipeline = prepare_[state.parity];
    }
    commands.bind_compute_pipeline(pipeline);
    commands.bind_descriptor_sets(pipeline_layout_, 0,
                                  Span<const rhi::DescriptorSetHandle>(&descriptors_, 1));
    commands.push_constants(pipeline_layout_, rhi::ShaderStage::Compute, 0,
                            Span<const u8>(reinterpret_cast<const u8*>(&view_), sizeof(GpuView)));
    if (state.pass == 3) {
        // THE ONLY PLACE THE CPU WOULD HAVE NEEDED A COUNT, and it does not have one: the traversal
        // step is dispatched from arguments the previous pass wrote.
        commands.dispatch_indirect(dispatch_args_, state.args_offset);
        return;
    }
    commands.dispatch(state.groups, 1, 1);
}

void GpuTraversal::record_pass(const PassContext& context, void* user) noexcept {
    auto* state = static_cast<PassState*>(user);
    state->self->dispatch(context, *state);
}

Status GpuTraversal::record(RenderGraph& graph, const TraversalView& view,
                            u32 instance_count) noexcept {
    if (!initialised_) {
        return fail(ErrorCode::Unavailable, "GpuTraversal::record: initialise() has not run");
    }
    view_ = pack_view(view, instance_count, cluster_stride_);

    // Every buffer the passes touch, imported so the graph derives the barriers between the levels.
    struct Imported {
        ResourceId id = kInvalidResource;
        rhi::BufferHandle handle;
        const char* name = "";
    };
    auto import = [&graph](rhi::BufferHandle handle, const char* name, u64 bytes,
                           rhi::BufferUsage usage) noexcept {
        BufferRequest request;
        request.name = name;
        request.size = bytes;
        request.extra_usage = usage;
        return graph.import_buffer(request, handle);
    };
    const rhi::BufferUsage storage = rhi::BufferUsage::Storage;
    const ResourceId queue_a =
        import(queue_a_, "vg.queue-a", static_cast<u64>(options_.queue_capacity) * sizeof(GpuNode),
               storage);
    const ResourceId queue_b =
        import(queue_b_, "vg.queue-b", static_cast<u64>(options_.queue_capacity) * sizeof(GpuNode),
               storage);
    const ResourceId counters =
        import(counters_, "vg.counters", kCounterWords * sizeof(u32), storage);
    const ResourceId args = import(dispatch_args_, "vg.dispatch-args", kDispatchArgsBytes,
                                   storage | rhi::BufferUsage::Indirect);
    const ResourceId visible =
        import(visible_, "vg.visible",
               static_cast<u64>(options_.visible_capacity) * sizeof(GpuVisibleCluster), storage);
    const ResourceId requests =
        import(requests_, "vg.requests",
               static_cast<u64>(options_.request_capacity) * sizeof(PageRequest), storage);
    const u64 marks = static_cast<u64>(instance_count) * cluster_stride_;
    const ResourceId visited =
        import(visited_, "vg.visited", (marks == 0 ? 1 : marks) * sizeof(u32), storage);
    const rhi::BufferHandle upload_targets[kUploadCount] = {
        clusters_, cluster_triangles_, instances_, assets_, roots_, children_, page_table_};
    const char* upload_names[kUploadCount] = {
        "vg.clusters", "vg.cluster-triangles", "vg.instances", "vg.assets",
        "vg.roots",    "vg.children",          "vg.page-table"};
    ResourceId uploaded[kUploadCount];
    for (u32 slot = 0; slot < kUploadCount; ++slot) {
        uploaded[slot] = import(upload_targets[slot], upload_names[slot], uploads_[slot].capacity,
                                storage | rhi::BufferUsage::TransferDestination);
    }
    const ResourceId staging =
        import(staging_, "vg.staging",
               uploads_[kUploadCount - 1].staging_offset + uploads_[kUploadCount - 1].capacity,
               rhi::BufferUsage::TransferSource);

    states_.clear();
    // Reserved before anything points into it: `PassBuilder::record` stores a pointer, and a
    // reallocation would leave every earlier pass pointing at freed memory. This is the one place
    // in the module where a growing array would be a use-after-free rather than a slowdown.
    if (Status reserved = states_.reserve(3 + (2 * static_cast<usize>(options_.max_levels)));
        !reserved) {
        return reserved;
    }

    const u64 counter_words = static_cast<u64>(kCounterWords);
    // The upload pass first, and the traversal passes declare READS of what it wrote — which is
    // what makes the graph emit the transfer-to-compute barrier that a host-side fence does not.
    u32 upload_mask = 0;
    for (u32 slot = 0; slot < kUploadCount; ++slot) {
        upload_mask |= uploads_[slot].dirty ? (1U << slot) : 0U;
    }
    if (upload_mask != 0U) {
        if (Status pushed = states_.push_back(PassState{this, 4, 0, 0, 0, upload_mask}); !pushed) {
            return pushed;
        }
        PassBuilder builder = graph.add_pass("vg.upload", rhi::QueueKind::Graphics);
        builder.read(staging, rhi::Access::TransferRead);
        for (u32 slot = 0; slot < kUploadCount; ++slot) {
            if (uploads_[slot].dirty) {
                builder.write(uploaded[slot], rhi::Access::TransferWrite);
            }
        }
        builder.record(&record_pass, &states_.back());
    }

    const u32 reset_threads = static_cast<u32>(marks > counter_words ? marks : counter_words);
    if (Status pushed = states_.push_back(
            PassState{this, 0, 0, (reset_threads + kGroupSize - 1U) / kGroupSize, 0, 0});
        !pushed) {
        return pushed;
    }
    graph.add_pass("vg.reset", rhi::QueueKind::Graphics)
        .write(counters, rhi::Access::ComputeStorageWrite)
        .write(args, rhi::Access::ComputeStorageWrite)
        .write(visited, rhi::Access::ComputeStorageWrite)
        .record(&record_pass, &states_.back());

    if (Status pushed = states_.push_back(
            PassState{this, 1, 0, (instance_count + kGroupSize - 1U) / kGroupSize, 0, 0});
        !pushed) {
        return pushed;
    }
    graph.add_pass("vg.instance-cull", rhi::QueueKind::Graphics)
        .read(uploaded[kUploadClusters], rhi::Access::ComputeStorageRead)
        .read(uploaded[kUploadInstances], rhi::Access::ComputeStorageRead)
        .read(uploaded[kUploadAssets], rhi::Access::ComputeStorageRead)
        .read(uploaded[kUploadRoots], rhi::Access::ComputeStorageRead)
        .use(counters, rhi::Access::ComputeStorageReadWrite)
        .write(queue_a, rhi::Access::ComputeStorageWrite)
        .record(&record_pass, &states_.back());

    for (u32 level = 0; level < options_.max_levels; ++level) {
        const u32 parity = level % 2U;
        if (Status pushed = states_.push_back(PassState{this, 2, parity, 1, 0, 0}); !pushed) {
            return pushed;
        }
        graph.add_pass("vg.prepare", rhi::QueueKind::Graphics)
            .use(counters, rhi::Access::ComputeStorageReadWrite)
            .write(args, rhi::Access::ComputeStorageWrite)
            .record(&record_pass, &states_.back());

        if (Status pushed = states_.push_back(PassState{
                this, 3, parity, 0, parity == 0 ? 0 : 3 * static_cast<u64>(sizeof(u32)), 0});
            !pushed) {
            return pushed;
        }
        graph.add_pass("vg.traverse", rhi::QueueKind::Graphics)
            .read(args, rhi::Access::IndirectCommandRead)
            .read(uploaded[kUploadClusters], rhi::Access::ComputeStorageRead)
            .read(uploaded[kUploadClusterTriangles], rhi::Access::ComputeStorageRead)
            .read(uploaded[kUploadInstances], rhi::Access::ComputeStorageRead)
            .read(uploaded[kUploadAssets], rhi::Access::ComputeStorageRead)
            .read(uploaded[kUploadChildren], rhi::Access::ComputeStorageRead)
            .read(uploaded[kUploadPageTable], rhi::Access::ComputeStorageRead)
            .use(counters, rhi::Access::ComputeStorageReadWrite)
            .use(queue_a, rhi::Access::ComputeStorageReadWrite)
            .use(queue_b, rhi::Access::ComputeStorageReadWrite)
            .use(visited, rhi::Access::ComputeStorageReadWrite)
            .write(visible, rhi::Access::ComputeStorageWrite)
            .write(requests, rhi::Access::ComputeStorageWrite)
            .record(&record_pass, &states_.back());
    }
    for (Upload& upload : uploads_) {
        upload.dirty = false;
    }
    return graph.status();
}

Status GpuTraversal::read_back(TraversalReadback& out) const noexcept {
    // The three buffers are copied into one host-visible region in one submit. A separate submit
    // per buffer would be three fences to wait on and three chances to read one of them early.
    Expected<rhi::CommandBufferHandle, Error> command =
        device_.acquire_command_buffer(rhi::QueueKind::Graphics, false);
    if (!command) {
        return make_unexpected(command.error());
    }
    if (Status begun = device_.begin_command_buffer(*command); !begun) {
        return begun;
    }
    const u64 counter_bytes = kCounterWords * sizeof(u32);
    const u64 visible_bytes = static_cast<u64>(options_.visible_capacity) * sizeof(VisibleCluster);
    const u64 request_bytes = static_cast<u64>(options_.request_capacity) * sizeof(PageRequest);
    rhi::CommandBuffer& commands = *device_.command_buffer(*command);
    rhi::BufferCopy region;
    region.size = counter_bytes;
    commands.copy_buffer(counters_, readback_, Span<const rhi::BufferCopy>(&region, 1));
    region.destination_offset = counter_bytes;
    region.size = visible_bytes;
    commands.copy_buffer(visible_, readback_, Span<const rhi::BufferCopy>(&region, 1));
    region.destination_offset = counter_bytes + visible_bytes;
    region.size = request_bytes;
    commands.copy_buffer(requests_, readback_, Span<const rhi::BufferCopy>(&region, 1));
    if (Status ended = device_.end_command_buffer(*command); !ended) {
        return ended;
    }
    rhi::SubmitInfo submit;
    submit.queue = rhi::QueueKind::Graphics;
    submit.command_buffers = Span<const rhi::CommandBufferHandle>(&*command, 1);
    Expected<u64, Error> value = device_.submit(submit);
    if (!value) {
        return make_unexpected(value.error());
    }
    if (Status waited = device_.wait_timeline(rhi::QueueKind::Graphics, *value, 5'000'000'000ULL);
        !waited) {
        return waited;
    }

    const auto* bytes = static_cast<const u8*>(device_.buffer_mapped_pointer(readback_));
    if (bytes == nullptr) {
        return fail(ErrorCode::Internal, "read_back: the readback buffer is not mapped");
    }
    u32 counters[kCounterWords] = {};
    std::memcpy(counters, bytes, counter_bytes);

    out.visible.clear();
    out.requests.clear();
    out.stats = TraversalStatistics{};
    out.stats.instances_visible = counters[kCountInstancesVisible];
    out.stats.nodes_visited = counters[kCountNodesVisited];
    out.stats.nodes_pruned_by_frustum = counters[kCountPrunedFrustum];
    out.stats.rejected_by_cone = counters[kCountRejectedCone];
    out.stats.rejected_by_frustum = counters[kCountRejectedFrustum];
    out.stats.rejected_by_size = counters[kCountRejectedSize];
    out.stats.visible_clusters = counters[kCountVisible];
    out.stats.visible_triangles = counters[kCountVisibleTriangles];
    out.stats.missing_pages = counters[kCountMissingPages];
    out.stats.candidates = counters[kCountVisible] + counters[kCountRejectedCone] +
                           counters[kCountRejectedFrustum] + counters[kCountRejectedSize];
    out.levels_exhausted = counters[kCountQueueA] != 0 || counters[kCountQueueB] != 0;
    out.overflowed = counters[kCountVisible] > options_.visible_capacity ||
                     counters[kCountRequests] > options_.request_capacity;

    const u32 visible_count = counters[kCountVisible] < options_.visible_capacity
                                  ? counters[kCountVisible]
                                  : options_.visible_capacity;
    if (Status resized = out.visible.resize(visible_count); !resized) {
        return resized;
    }
    if (visible_count > 0) {
        std::memcpy(out.visible.data(), bytes + counter_bytes,
                    static_cast<usize>(visible_count) * sizeof(VisibleCluster));
    }
    const u32 request_count = counters[kCountRequests] < options_.request_capacity
                                  ? counters[kCountRequests]
                                  : options_.request_capacity;
    if (Status resized = out.requests.resize(request_count); !resized) {
        return resized;
    }
    if (request_count > 0) {
        std::memcpy(out.requests.data(), bytes + counter_bytes + visible_bytes,
                    static_cast<usize>(request_count) * sizeof(PageRequest));
    }
    return ok();
}

}  // namespace cy::rendering::vg
