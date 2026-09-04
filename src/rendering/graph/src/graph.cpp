// The declaration side of the render graph: resources, passes and their uses. Task 2.2.1.
//
// Nothing here derives anything. This file's whole job is to record what a pass said it reads and
// writes, in a form the compiler can walk deterministically, and to refuse a declaration that could
// only produce a plausible-looking wrong barrier — a read declared with a writing intent, a sampled
// read of a buffer, a range outside the resource.
//
// FAILURES ACCUMULATE RATHER THAN RETURNING. A declaration is a chain of calls and threading a
// Status through each of them would bury the declaration in error handling; instead the first
// failure is kept on the graph and compile() refuses. That is the same trade the engine's other
// builder-shaped interfaces make, and it holds only because compile() genuinely does refuse.

#include <cy/rendering/graph/graph.h>

#include <cy/backends/rhi/validation.h>
#include <cy/core/base/assert.h>

namespace cy::rendering {
namespace {

/// The usage a declared access implies. The graph unions these over every use of a resource and
/// creates it with the result, so a caller never has to keep a usage flag in step with the passes
/// that were written six months later.
rhi::TextureUsage texture_usage_for(rhi::Access access) noexcept {
    switch (access) {
        case rhi::Access::ComputeStorageWrite:
        case rhi::Access::ComputeStorageRead:
        case rhi::Access::ComputeStorageReadWrite:
        case rhi::Access::VertexStorageRead:
        case rhi::Access::FragmentStorageRead:
            return rhi::TextureUsage::Storage;
        case rhi::Access::ComputeSampledRead:
        case rhi::Access::FragmentSampledRead:
            return rhi::TextureUsage::Sampled;
        case rhi::Access::ColorAttachmentWrite:
        case rhi::Access::ColorAttachmentReadWrite:
            return rhi::TextureUsage::ColorAttachment;
        case rhi::Access::DepthStencilAttachmentWrite:
        case rhi::Access::DepthStencilAttachmentRead:
            return rhi::TextureUsage::DepthStencilAttachment;
        case rhi::Access::TransferRead:
            return rhi::TextureUsage::TransferSource;
        case rhi::Access::TransferWrite:
            return rhi::TextureUsage::TransferDestination;
        case rhi::Access::ComputeUniformRead:
        case rhi::Access::VertexAttributeRead:
        case rhi::Access::IndexRead:
        case rhi::Access::IndirectCommandRead:
        case rhi::Access::VertexUniformRead:
        case rhi::Access::FragmentUniformRead:
        case rhi::Access::HostRead:
        case rhi::Access::Present:
        case rhi::Access::Count:
            break;
    }
    return rhi::TextureUsage::None;
}

rhi::BufferUsage buffer_usage_for(rhi::Access access) noexcept {
    switch (access) {
        case rhi::Access::ComputeStorageWrite:
        case rhi::Access::ComputeStorageRead:
        case rhi::Access::ComputeStorageReadWrite:
        case rhi::Access::VertexStorageRead:
        case rhi::Access::FragmentStorageRead:
            return rhi::BufferUsage::Storage;
        case rhi::Access::ComputeUniformRead:
        case rhi::Access::VertexUniformRead:
        case rhi::Access::FragmentUniformRead:
            return rhi::BufferUsage::Uniform;
        case rhi::Access::VertexAttributeRead:
            return rhi::BufferUsage::Vertex;
        case rhi::Access::IndexRead:
            return rhi::BufferUsage::Index;
        case rhi::Access::IndirectCommandRead:
            return rhi::BufferUsage::Indirect;
        case rhi::Access::TransferRead:
            return rhi::BufferUsage::TransferSource;
        case rhi::Access::TransferWrite:
            return rhi::BufferUsage::TransferDestination;
        case rhi::Access::ComputeSampledRead:
        case rhi::Access::FragmentSampledRead:
        case rhi::Access::ColorAttachmentWrite:
        case rhi::Access::ColorAttachmentReadWrite:
        case rhi::Access::DepthStencilAttachmentWrite:
        case rhi::Access::DepthStencilAttachmentRead:
        case rhi::Access::HostRead:
        case rhi::Access::Present:
        case rhi::Access::Count:
            break;
    }
    return rhi::BufferUsage::None;
}

/// A plausible device alignment for the synthetic query. The exact value does not matter; that
/// there IS one does, because a plan computed against alignment 1 places transients a real
/// allocator would refuse and the aliasing measurement would then be optimistic.
constexpr u64 kSyntheticAlignment = 256;
constexpr u32 kSyntheticMemoryTypeBits = 0xFU;

u64 align_up_to(u64 value, u64 alignment) noexcept {
    return (value + alignment - 1) / alignment * alignment;
}

}  // namespace

bool synthetic_memory_query(ResourceId resource, const ResourceInfo& info,
                            rhi::MemoryRequirements& out, void* user) noexcept {
    (void)resource;
    (void)user;
    u64 size = 0;
    if (info.is_texture) {
        const TextureRequest& request = info.texture;
        for (u16 mip = 0; mip < request.mip_levels; ++mip) {
            const u32 width = request.width >> mip;
            const u32 height = request.height >> mip;
            const u32 depth = request.depth >> mip;
            size += rhi::format_byte_size(request.format, width != 0 ? width : 1,
                                          height != 0 ? height : 1) *
                    (depth != 0 ? depth : 1);
        }
        size *= request.array_layers;
    } else {
        size = info.buffer.size;
    }
    if (size == 0) {
        return false;
    }
    out.size = align_up_to(size, kSyntheticAlignment);
    out.alignment = kSyntheticAlignment;
    out.memory_type_bits = kSyntheticMemoryTypeBits;
    return true;
}

// --- RenderGraph
// ----------------------------------------------------------------------------------

RenderGraph::RenderGraph(Allocator& allocator) noexcept
    : allocator_(&allocator), resources_(allocator), passes_(allocator), uses_(allocator) {}

void RenderGraph::set_failure(ErrorCode code, const char* message) noexcept {
    // The FIRST failure is kept, not the last: the first one is the cause and everything after it
    // is usually a consequence of declaring against a resource that was never created.
    if (status_) {
        status_ = make_unexpected(Error{code, message, 0});
    }
}

ResourceId RenderGraph::create_texture(const TextureRequest& request) noexcept {
    ResourceInfo info;
    info.name = request.name;
    info.is_texture = true;
    info.transient = true;
    info.texture = request;
    info.texture_usage = request.extra_usage;
    if (Status pushed = resources_.push_back(info); !pushed) {
        set_failure(ErrorCode::OutOfMemory, "the render graph could not record a texture");
        return kInvalidResource;
    }
    return static_cast<ResourceId>(resources_.size() - 1);
}

ResourceId RenderGraph::create_buffer(const BufferRequest& request) noexcept {
    ResourceInfo info;
    info.name = request.name;
    info.is_texture = false;
    info.transient = true;
    info.buffer = request;
    info.buffer_usage = request.extra_usage;
    if (Status pushed = resources_.push_back(info); !pushed) {
        set_failure(ErrorCode::OutOfMemory, "the render graph could not record a buffer");
        return kInvalidResource;
    }
    return static_cast<ResourceId>(resources_.size() - 1);
}

ResourceId RenderGraph::import_texture(const TextureRequest& request, rhi::TextureHandle texture,
                                       rhi::ImageLayout current_layout,
                                       u32 owning_queue_family) noexcept {
    ResourceInfo info;
    info.name = request.name;
    info.is_texture = true;
    info.transient = false;
    info.imported = true;
    info.texture = request;
    info.texture_usage = request.extra_usage;
    info.initial_layout = current_layout;
    info.initial_queue_family = owning_queue_family;
    info.imported_texture = texture;
    if (Status pushed = resources_.push_back(info); !pushed) {
        set_failure(ErrorCode::OutOfMemory,
                    "the render graph could not record an imported texture");
        return kInvalidResource;
    }
    return static_cast<ResourceId>(resources_.size() - 1);
}

ResourceId RenderGraph::import_buffer(const BufferRequest& request, rhi::BufferHandle buffer,
                                      u32 owning_queue_family) noexcept {
    ResourceInfo info;
    info.name = request.name;
    info.is_texture = false;
    info.transient = false;
    info.imported = true;
    info.buffer = request;
    info.buffer_usage = request.extra_usage;
    info.initial_queue_family = owning_queue_family;
    info.imported_buffer = buffer;
    if (Status pushed = resources_.push_back(info); !pushed) {
        set_failure(ErrorCode::OutOfMemory, "the render graph could not record an imported buffer");
        return kInvalidResource;
    }
    return static_cast<ResourceId>(resources_.size() - 1);
}

PassBuilder RenderGraph::add_pass(const char* name, rhi::QueueKind queue) noexcept {
    Pass pass;
    pass.name = name != nullptr ? name : "pass";
    pass.queue = queue;
    pass.first_use = uses_.size();
    if (Status pushed = passes_.push_back(pass); !pushed) {
        set_failure(ErrorCode::OutOfMemory, "the render graph could not record a pass");
        return {this, kInvalidPass};
    }
    return {this, static_cast<PassId>(passes_.size() - 1)};
}

void RenderGraph::note_usage(ResourceId resource, rhi::Access access) noexcept {
    ResourceInfo& info = resources_[resource];
    if (info.is_texture) {
        info.texture_usage |= texture_usage_for(access);
    } else {
        info.buffer_usage = info.buffer_usage | buffer_usage_for(access);
    }
}

void RenderGraph::add_use(PassId pass, ResourceId resource, rhi::Access access,
                          rhi::SubresourceRange range) noexcept {
    if (pass == kInvalidPass || pass >= passes_.size()) {
        set_failure(ErrorCode::InvalidArgument, "a use was declared against no pass");
        return;
    }
    if (resource == kInvalidResource || resource >= resources_.size()) {
        set_failure(ErrorCode::NotFound,
                    "a pass declared a use of a resource the graph does not have — the create call "
                    "that should have produced it failed");
        return;
    }
    // Uses live in one flat array indexed by (first_use, use_count), so a pass's uses must be
    // contiguous. Declaring a use against an earlier pass would silently land in the latest one.
    if (pass + 1 != passes_.size()) {
        set_failure(ErrorCode::InvalidArgument,
                    "uses must be declared against the pass being built; add_pass() again after "
                    "another pass has been declared is not the same pass");
        return;
    }

    const ResourceInfo& info = resources_[resource];
    if (!rhi::access_valid_for(access, info.is_texture)) {
        set_failure(ErrorCode::InvalidArgument,
                    info.is_texture
                        ? "an access intent that has no meaning for an image was declared against "
                          "a texture"
                        : "an access intent that has no meaning for a buffer was declared against "
                          "a buffer");
        return;
    }

    Use use;
    use.resource = resource;
    use.access = access;
    // Resolved here and nowhere else, so that everything downstream — the dependency edges, the
    // per-cell tracker, the coalescer — sees explicit counts. Two ranges cannot be compared for
    // adjacency while either of them still means "whatever is left".
    use.range = info.is_texture
                    ? rhi::resolve_range(range, info.texture.mip_levels, info.texture.array_layers)
                    : rhi::SubresourceRange{0, 1, 0, 1};
    if (info.is_texture) {
        const u32 mip_end = static_cast<u32>(use.range.base_mip) + use.range.mip_count;
        const u32 layer_end = static_cast<u32>(use.range.base_layer) + use.range.layer_count;
        if (use.range.mip_count == 0 || use.range.layer_count == 0 ||
            mip_end > info.texture.mip_levels || layer_end > info.texture.array_layers) {
            set_failure(ErrorCode::OutOfRange,
                        "a pass declared a subresource range outside the texture it names");
            return;
        }
    }

    if (Status pushed = uses_.push_back(use); !pushed) {
        set_failure(ErrorCode::OutOfMemory, "the render graph could not record a use");
        return;
    }
    ++passes_[pass].use_count;
    note_usage(resource, access);
}

void RenderGraph::set_record(PassId pass, RecordFn function, void* user) noexcept {
    if (pass == kInvalidPass || pass >= passes_.size()) {
        set_failure(ErrorCode::InvalidArgument, "record() on no pass");
        return;
    }
    passes_[pass].record = function;
    passes_[pass].user = user;
}

void RenderGraph::set_side_effect(PassId pass) noexcept {
    if (pass == kInvalidPass || pass >= passes_.size()) {
        set_failure(ErrorCode::InvalidArgument, "side_effect() on no pass");
        return;
    }
    passes_[pass].side_effect = true;
}

const ResourceInfo& RenderGraph::resource(ResourceId id) const noexcept {
    CY_ASSERT_MSG(id < resources_.size(), "resource id outside the graph");
    static const ResourceInfo kEmpty;
    return id < resources_.size() ? resources_[id] : kEmpty;
}

const char* RenderGraph::pass_name(PassId pass) const noexcept {
    return pass < passes_.size() ? passes_[pass].name : "<invalid>";
}

rhi::QueueKind RenderGraph::pass_queue(PassId pass) const noexcept {
    return pass < passes_.size() ? passes_[pass].queue : rhi::QueueKind::Graphics;
}

Span<const Use> RenderGraph::pass_uses(PassId pass) const noexcept {
    if (pass >= passes_.size()) {
        return {};
    }
    const Pass& stored = passes_[pass];
    return {uses_.data() + stored.first_use, stored.use_count};
}

bool RenderGraph::pass_has_side_effect(PassId pass) const noexcept {
    return pass < passes_.size() && passes_[pass].side_effect;
}

RecordFn RenderGraph::pass_record_function(PassId pass) const noexcept {
    return pass < passes_.size() ? passes_[pass].record : nullptr;
}

void* RenderGraph::pass_record_user(PassId pass) const noexcept {
    return pass < passes_.size() ? passes_[pass].user : nullptr;
}

void RenderGraph::reset() noexcept {
    resources_.clear();
    passes_.clear();
    uses_.clear();
    status_ = ok();
}

// --- PassBuilder
// ------------------------------------------------------------------------------------

PassBuilder& PassBuilder::read(ResourceId resource, rhi::Access access,
                               rhi::SubresourceRange range) noexcept {
    if (rhi::is_write(access)) {
        graph_->set_failure(ErrorCode::InvalidArgument,
                            "read() was given a writing access intent. The declaration is what the "
                            "barrier is derived from, so a mistyped one produces a barrier that "
                            "looks right and is not");
        return *this;
    }
    graph_->add_use(pass_, resource, access, range);
    return *this;
}

PassBuilder& PassBuilder::write(ResourceId resource, rhi::Access access,
                                rhi::SubresourceRange range) noexcept {
    if (!rhi::is_write(access)) {
        graph_->set_failure(ErrorCode::InvalidArgument,
                            "write() was given a reading access intent");
        return *this;
    }
    graph_->add_use(pass_, resource, access, range);
    return *this;
}

PassBuilder& PassBuilder::use(ResourceId resource, rhi::Access access,
                              rhi::SubresourceRange range) noexcept {
    graph_->add_use(pass_, resource, access, range);
    return *this;
}

PassBuilder& PassBuilder::record(RecordFn function, void* user) noexcept {
    graph_->set_record(pass_, function, user);
    return *this;
}

PassBuilder& PassBuilder::side_effect() noexcept {
    graph_->set_side_effect(pass_);
    return *this;
}

}  // namespace cy::rendering
