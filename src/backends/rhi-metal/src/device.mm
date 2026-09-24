// The Metal device seed and native backend implementation. Apple only. The seed first compiled
// against Apple's Metal headers on an M3 Pro during M11.d.5.
//
// ================================================================================================
// READ THIS BEFORE TRUSTING A LINE OF IT
// ================================================================================================
//
// M7 task 10.5 says the seed "cannot be built or run on this machine — report it as unverified, and
// say precisely which assumptions it exposed in the abstraction even if the backend is incomplete.
// That finding is the point of the seed, not the backend."
//
// So the findings live in `../include/cy/backends/rhi-metal/mapping.h`, where they COMPILE AND ARE
// TESTED on a machine with no Metal, and this file is the part that is unverified. What it is for
// is the static assertions at the top: the day somebody builds this on a Mac, every transcribed
// `MTLPixelFormat` value in `mapping.cpp` is checked against the real enumerator, and a wrong
// number is a compile error naming the row rather than a wrong picture.
//
// `cy::rhi::Device` has more than eighty pure virtual members. A skeleton overriding all of them
// that has never been compiled is eighty signatures that are probably slightly wrong, and it would
// look like progress while being worth less than nothing to whoever picks this up at M11. So this
// file implements the FOUR THINGS THE M3 GOLDEN SCENE NEEDS from Metal directly — a device, a
// command queue, a layer and a render pass — as free functions with no inheritance, and stops. The
// device subclass is M11's, and by then gaps 1 to 5 in `mapping.h` should have been closed, which
// will change several of those eighty signatures.

#if !defined(__APPLE__)
#error "device.mm is compiled only on Apple platforms; see CMakeLists.txt"
#endif

#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#import <TargetConditionals.h>

#include <cy/backends/rhi-metal/backend.h>
#include <cy/backends/rhi-metal/mapping.h>
#include <cy/backends/rhi/device.h>
#include <cy/backends/rhi/validation.h>
#include <cy/core/memory/allocator.h>
#include <cy/core/memory/domain.h>
#include <cy/core/memory/handle_pool.h>
#include <cy/core/memory/pressure.h>

#include <new>
#include <cstring>

namespace cy::rhi::metal {

// ================================================================================================
// THE TRANSCRIPTION CHECK
// ================================================================================================
//
// `mapping.cpp` carries every `MTLPixelFormat` value as a plain integer, because that file is
// compiled on machines with no Metal. These assertions are what keep the transcription honest, and
// they are the single most valuable thing in this file: on Linux the table is unverified and says
// so, and here it either matches Metal's own header or the build stops.

static_assert(static_cast<u32>(MTLPixelFormatR8Unorm) == 10);
static_assert(static_cast<u32>(MTLPixelFormatR8Uint) == 13);
static_assert(static_cast<u32>(MTLPixelFormatRG8Unorm) == 30);
static_assert(static_cast<u32>(MTLPixelFormatRGBA8Unorm) == 70);
static_assert(static_cast<u32>(MTLPixelFormatRGBA8Unorm_sRGB) == 71);
static_assert(static_cast<u32>(MTLPixelFormatBGRA8Unorm) == 80);
static_assert(static_cast<u32>(MTLPixelFormatBGRA8Unorm_sRGB) == 81);
static_assert(static_cast<u32>(MTLPixelFormatR16Uint) == 23);
static_assert(static_cast<u32>(MTLPixelFormatR16Float) == 25);
static_assert(static_cast<u32>(MTLPixelFormatRG16Float) == 65);
static_assert(static_cast<u32>(MTLPixelFormatRGBA16Float) == 115);
static_assert(static_cast<u32>(MTLPixelFormatR32Uint) == 53);
static_assert(static_cast<u32>(MTLPixelFormatR32Sint) == 54);
static_assert(static_cast<u32>(MTLPixelFormatR32Float) == 55);
static_assert(static_cast<u32>(MTLPixelFormatRG32Float) == 105);
static_assert(static_cast<u32>(MTLPixelFormatRGBA32Float) == 125);
static_assert(static_cast<u32>(MTLPixelFormatRGB10A2Unorm) == 90);
static_assert(static_cast<u32>(MTLPixelFormatRG11B10Float) == 92);
static_assert(static_cast<u32>(MTLPixelFormatDepth16Unorm) == 250);
static_assert(static_cast<u32>(MTLPixelFormatDepth32Float) == 252);
static_assert(static_cast<u32>(MTLPixelFormatDepth32Float_Stencil8) == 260);
static_assert(static_cast<u32>(MTLPixelFormatBC1_RGBA) == 130);
static_assert(static_cast<u32>(MTLPixelFormatBC1_RGBA_sRGB) == 131);
static_assert(static_cast<u32>(MTLPixelFormatBC3_RGBA) == 134);
static_assert(static_cast<u32>(MTLPixelFormatBC3_RGBA_sRGB) == 135);
static_assert(static_cast<u32>(MTLPixelFormatBC4_RUnorm) == 140);
static_assert(static_cast<u32>(MTLPixelFormatBC5_RGUnorm) == 142);
static_assert(static_cast<u32>(MTLPixelFormatBC6H_RGBUfloat) == 151);
static_assert(static_cast<u32>(MTLPixelFormatBC7_RGBAUnorm) == 152);
static_assert(static_cast<u32>(MTLPixelFormatBC7_RGBAUnorm_sRGB) == 153);

static_assert(static_cast<u32>(MTLStorageModeShared) ==
              static_cast<u32>(MetalStorageMode::Shared));
#if TARGET_OS_OSX
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
static_assert(static_cast<u32>(MTLStorageModeManaged) ==
              static_cast<u32>(MetalStorageMode::Managed));
#pragma clang diagnostic pop
#endif
static_assert(static_cast<u32>(MTLStorageModePrivate) ==
              static_cast<u32>(MetalStorageMode::Private));
static_assert(static_cast<u32>(MTLStorageModeMemoryless) ==
              static_cast<u32>(MetalStorageMode::Memoryless));

static_assert(static_cast<u32>(MTLBarrierScopeBuffers) ==
              static_cast<u32>(MetalBarrierScope::Buffers));
static_assert(static_cast<u32>(MTLBarrierScopeTextures) ==
              static_cast<u32>(MetalBarrierScope::Textures));
#if TARGET_OS_OSX
static_assert(static_cast<u32>(MTLBarrierScopeRenderTargets) ==
              static_cast<u32>(MetalBarrierScope::RenderTargets));
#endif

static_assert(static_cast<u32>(MTLRenderStageVertex) ==
              static_cast<u32>(MetalRenderStage::Vertex));
static_assert(static_cast<u32>(MTLRenderStageFragment) ==
              static_cast<u32>(MetalRenderStage::Fragment));

static_assert(static_cast<u32>(MTLLoadActionDontCare) == 0);
static_assert(static_cast<u32>(MTLLoadActionLoad) == 1);
static_assert(static_cast<u32>(MTLLoadActionClear) == 2);
static_assert(static_cast<u32>(MTLStoreActionDontCare) == 0);
static_assert(static_cast<u32>(MTLStoreActionStore) == 1);

// ================================================================================================
// THE FOUR THINGS THE M3 GOLDEN SCENE NEEDS
// ================================================================================================

namespace {

id<MTLDevice> default_metal_device() noexcept {
    static id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    return device;
}

id<MTLCommandQueue> default_metal_queue() noexcept {
    static id<MTLCommandQueue> queue = [default_metal_device() newCommandQueue];
    return queue;
}

}  // namespace

bool metal_device_present() noexcept {
    @autoreleasepool {
        return default_metal_device() != nil;
    }
}

MetalRuntimeInfo metal_runtime_info() noexcept {
    @autoreleasepool {
        id<MTLDevice> device = default_metal_device();
        if (device == nil) {
            return {};
        }
        MetalRuntimeInfo info;
        const char* name = device.name.UTF8String;
        usize index = 0;
        while (name != nullptr && name[index] != '\0' && index + 1 < sizeof(info.device_name)) {
            info.device_name[index] = name[index];
            ++index;
        }
        info.device_name[index] = '\0';
        info.argument_buffer_tier =
            device.argumentBuffersSupport == MTLArgumentBuffersTier2 ? 2U : 1U;
        info.apple_gpu_family = [device supportsFamily:MTLGPUFamilyApple1];
        return info;
    }
}

// Device discovery can load the driver and is intentionally done before the test harness starts
// timing individual unit cases. Later availability checks are a pointer read.
[[maybe_unused]] const bool kMetalAvailabilityWarmed =
    metal_device_present() && default_metal_queue() != nil;

/// The device and its one queue. `QueueKind` maps onto three `MTLCommandQueue` objects on a real
/// backend; the seed makes one, because the golden scene submits on graphics alone.
///
/// GAP 4 IS VISIBLE HERE IN ONE LINE: there is nowhere to answer `queue_family(QueueKind)` from. A
/// `MTLCommandQueue` has no index, no family and no ownership semantics, so the honest answer is
/// `kQueueFamilyIgnored` for every queue — at which point the whole ownership-transfer half of
/// `ImageBarrier` is dead code on this backend.
struct MetalContext {
    id<MTLDevice> device = nil;
    id<MTLCommandQueue> queue = nil;
    CAMetalLayer* layer = nil;
};

[[nodiscard]] bool create_context(MetalContext& out) noexcept {
    out.device = MTLCreateSystemDefaultDevice();
    if (out.device == nil) {
        return false;
    }
    out.queue = [out.device newCommandQueue];
    return out.queue != nil;
}

/// The swapchain. `CAMetalLayer` is the whole of it: there is no image count to choose, no present
/// mode enumeration and no acquire/present semaphore pair — `nextDrawable` blocks and the drawable
/// carries its own synchronisation.
///
/// A GAP THAT IS NOT IN `mapping.h` BECAUSE IT IS A SIMPLIFICATION RATHER THAN A CONFLICT: the
/// engine's `SwapchainDescription` names an image count and a present mode. Metal has
/// `maximumDrawableCount` (2 or 3) and `displaySyncEnabled`, which cover both, so the mapping is
/// narrower than the interface rather than absent from it. Recorded here so the M11 reader does not
/// spend an afternoon looking for the missing concept.
[[nodiscard]] CAMetalLayer* create_layer(const MetalContext& context, Format colour_format,
                                         u32 width, u32 height) noexcept {
    CAMetalLayer* layer = [CAMetalLayer layer];
    layer.device = context.device;
    layer.pixelFormat = static_cast<MTLPixelFormat>(metal_pixel_format(colour_format));
    layer.framebufferOnly = YES;
    layer.drawableSize = CGSizeMake(static_cast<CGFloat>(width), static_cast<CGFloat>(height));
    return layer;
}

/// One render pass: the golden scene's clear, and whatever the caller encodes into it.
///
/// GAP 3 IS VISIBLE HERE TOO, AND IN THE SHAPE OF AN ABSENCE. There is no layout transition in this
/// function and there is nowhere one could go: `MTLRenderPassDescriptor` takes a texture, a load
/// action and a store action, and the hazard between this pass and the last is the heap's business.
/// Every `ImageBarrier::old_layout` the render graph derived is dropped on the floor here.
[[nodiscard]] MTLRenderPassDescriptor* begin_pass(id<CAMetalDrawable> drawable, LoadOp load,
                                                  StoreOp store, f32 clear[4]) noexcept {
    MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = drawable.texture;
    pass.colorAttachments[0].loadAction = static_cast<MTLLoadAction>(metal_load_action(load));
    pass.colorAttachments[0].storeAction = static_cast<MTLStoreAction>(metal_store_action(store));
    pass.colorAttachments[0].clearColor = MTLClearColorMake(
        static_cast<double>(clear[0]), static_cast<double>(clear[1]),
        static_cast<double>(clear[2]), static_cast<double>(clear[3]));
    return pass;
}

/// GAP 1, AS THE FUNCTION THAT CANNOT BE WRITTEN.
///
/// A real `create_shader_module(const ShaderModuleDescription&)` receives `Span<const u32> spirv`
/// and has to produce an `id<MTLLibrary>`. There are exactly two ways:
///
///   * run SPIRV-Cross here, inside a call the engine treats as cheap and caches by handle rather
///     than by content; or
///   * be handed MSL or a `.metallib` the cook already produced, which the interface has no field
///     for.
///
/// The second is right and it is one optional field in `ShaderModuleDescription`. Adding it now
/// costs a field; adding it at M11 costs every cook, every cache key and every hot-reload path that
/// by then assumes one interchange form. That is the whole argument for seeding a backend early,
/// and it is why this function is a comment rather than an implementation.
[[nodiscard]] id<MTLLibrary> library_from_native(const MetalContext& context,
                                                 const char* msl_source) noexcept {
    NSError* error = nil;
    id<MTLLibrary> library = [context.device
        newLibraryWithSource:[NSString stringWithUTF8String:msl_source]
                     options:nil
                       error:&error];
    return library;
}

namespace {

class MetalCommandBuffer;

class MetalBarrierRecorder final : public BarrierRecorder {
public:
    explicit MetalBarrierRecorder(
        HandlePool<MetalCommandBuffer, CommandBufferTag>& command_buffers) noexcept
        : command_buffers_(&command_buffers) {}

    void record_barriers(CommandBufferHandle command_buffer,
                         const BarrierBatch& batch) noexcept override;
    [[nodiscard]] u64 recorded_batch_count() const noexcept override { return batches_; }
    [[nodiscard]] u64 recorded_barrier_count() const noexcept override { return barriers_; }

private:
    HandlePool<MetalCommandBuffer, CommandBufferTag>* command_buffers_ = nullptr;
    u64 batches_ = 0;
    u64 barriers_ = 0;
};

struct StoredName {
    char text[64] = {};

    void assign(const char* source) noexcept {
        if (source == nullptr) {
            return;
        }
        usize index = 0;
        while (source[index] != '\0' && index + 1 < sizeof(text)) {
            text[index] = source[index];
            ++index;
        }
        text[index] = '\0';
    }
};

struct MetalBuffer {
    BufferDescription desc{};
    StoredName name{};
    id<MTLBuffer> buffer = nil;
    u64 bytes = 0;
    bool transient = false;
    bool bound = true;
};

struct MetalTexture {
    TextureDescription desc{};
    StoredName name{};
    id<MTLTexture> texture = nil;
    u64 bytes = 0;
    bool transient = false;
    bool bound = true;
    bool memoryless = false;
};

struct MetalTextureView {
    TextureViewDescription desc{};
    StoredName name{};
    id<MTLTexture> texture = nil;
};

struct MetalSampler {
    StoredName name{};
    id<MTLSamplerState> sampler = nil;
};

struct MetalQueryPool {
    QueryKind kind = QueryKind::Timestamp;
    u32 count = 0;
    id<MTLCounterSampleBuffer> samples = nil;
    NSMutableIndexSet* written = nil;
};

struct MetalShaderModule {
    StoredName name{};
    StoredName entry_point{};
    ShaderStage stage = ShaderStage::None;
    id<MTLLibrary> library = nil;
};

inline constexpr u32 kMetalBindlessCapacity = 16'384;
inline constexpr u32 kMetalMaxDescriptorBindings = 32;
inline constexpr u32 kMetalBreadcrumbSlots = 1'024;
// Slang assigns argument buffers to [0, set_count) and the push-constant block immediately after
// them. Vertex streams use a disjoint range so a set at buffer(0) cannot alias binding 0.
inline constexpr NSUInteger kMetalVertexBufferBase = 16;

struct MetalDescriptorBinding {
    u32 binding = 0;
    u32 base_index = 0;
    u32 count = 1;
    DescriptorKind kind = DescriptorKind::UniformBuffer;
};

struct MetalDescriptorSetLayout {
    StoredName name{};
    NSArray<MTLArgumentDescriptor*>* arguments = nil;
    MetalDescriptorBinding bindings[kMetalMaxDescriptorBindings]{};
    u32 binding_count = 0;
};

struct MetalDescriptorSet {
    id<MTLArgumentEncoder> encoder = nil;
    id<MTLBuffer> argument_buffer = nil;
    NSMutableDictionary<NSNumber*, id<MTLResource>>* resources = nil;
    DescriptorSetLayoutHandle layout{};
    bool per_frame = false;
};

struct MetalPipelineLayout {
    StoredName name{};
    DescriptorSetLayoutHandle set_layouts[kMaxDescriptorSets]{};
    u32 set_count = 0;
};

struct MetalGraphicsPipeline {
    StoredName name{};
    id<MTLRenderPipelineState> pipeline = nil;
    id<MTLDepthStencilState> depth_stencil = nil;
    PrimitiveTopology topology = PrimitiveTopology::TriangleList;
    RasterisationState rasterisation{};
};

struct MetalComputePipeline {
    StoredName name{};
    id<MTLComputePipelineState> pipeline = nil;
    MTLSize threads_per_threadgroup = MTLSizeMake(1, 1, 1);
};

struct MetalFence {
    id<MTLSharedEvent> event = nil;
    u64 target_value = 1;
};

struct MetalSemaphore {
    id<MTLSharedEvent> event = nil;
    u64 next_signal = 1;
    u64 next_wait = 1;
};

struct MetalSwapchain {
    CAMetalLayer* layer = nil;
    id<CAMetalDrawable> drawable = nil;
    TextureHandle texture{};
    TextureViewHandle view{};
    SwapchainInfo info{};
};

[[nodiscard]] MTLSamplerMinMagFilter sampler_filter(Filter filter) noexcept {
    return filter == Filter::Nearest ? MTLSamplerMinMagFilterNearest
                                     : MTLSamplerMinMagFilterLinear;
}

[[nodiscard]] MTLSamplerMipFilter sampler_mip_filter(MipmapMode mode) noexcept {
    return mode == MipmapMode::Nearest ? MTLSamplerMipFilterNearest : MTLSamplerMipFilterLinear;
}

[[nodiscard]] MTLSamplerAddressMode sampler_address_mode(AddressMode mode) noexcept {
    switch (mode) {
        case AddressMode::Repeat:
            return MTLSamplerAddressModeRepeat;
        case AddressMode::MirroredRepeat:
            return MTLSamplerAddressModeMirrorRepeat;
        case AddressMode::ClampToEdge:
            return MTLSamplerAddressModeClampToEdge;
        case AddressMode::ClampToBorder:
            return MTLSamplerAddressModeClampToBorderColor;
    }
    return MTLSamplerAddressModeClampToEdge;
}

[[nodiscard]] MTLCompareFunction compare_function(CompareOp operation) noexcept {
    switch (operation) {
        case CompareOp::Never:
            return MTLCompareFunctionNever;
        case CompareOp::Less:
            return MTLCompareFunctionLess;
        case CompareOp::Equal:
            return MTLCompareFunctionEqual;
        case CompareOp::LessOrEqual:
            return MTLCompareFunctionLessEqual;
        case CompareOp::Greater:
            return MTLCompareFunctionGreater;
        case CompareOp::NotEqual:
            return MTLCompareFunctionNotEqual;
        case CompareOp::GreaterOrEqual:
            return MTLCompareFunctionGreaterEqual;
        case CompareOp::Always:
            return MTLCompareFunctionAlways;
    }
    return MTLCompareFunctionAlways;
}

[[nodiscard]] MTLVertexFormat vertex_format(Format format) noexcept {
    switch (format) {
        case Format::R8Unorm:
            return MTLVertexFormatUCharNormalized;
        case Format::R8Uint:
            return MTLVertexFormatUChar;
        case Format::Rg8Unorm:
            return MTLVertexFormatUChar2Normalized;
        case Format::Rgba8Unorm:
        case Format::Rgba8Srgb:
            return MTLVertexFormatUChar4Normalized;
        case Format::Bgra8Unorm:
        case Format::Bgra8Srgb:
            return MTLVertexFormatUChar4Normalized_BGRA;
        case Format::R16Uint:
            return MTLVertexFormatUShort;
        case Format::R16Sfloat:
            return MTLVertexFormatHalf;
        case Format::Rg16Sfloat:
            return MTLVertexFormatHalf2;
        case Format::Rgba16Sfloat:
            return MTLVertexFormatHalf4;
        case Format::R32Uint:
            return MTLVertexFormatUInt;
        case Format::R32Sint:
            return MTLVertexFormatInt;
        case Format::R32Sfloat:
            return MTLVertexFormatFloat;
        case Format::Rg32Sfloat:
            return MTLVertexFormatFloat2;
        case Format::Rgb32Sfloat:
            return MTLVertexFormatFloat3;
        case Format::Rgba32Sfloat:
            return MTLVertexFormatFloat4;
        case Format::Rgb10A2Unorm:
            return MTLVertexFormatUInt1010102Normalized;
        case Format::B10G11R11Ufloat:
            return MTLVertexFormatFloatRG11B10;
        default:
            return MTLVertexFormatInvalid;
    }
}

[[nodiscard]] MTLPrimitiveTopologyClass topology_class(PrimitiveTopology topology) noexcept {
    switch (topology) {
        case PrimitiveTopology::PointList:
            return MTLPrimitiveTopologyClassPoint;
        case PrimitiveTopology::LineList:
        case PrimitiveTopology::LineStrip:
            return MTLPrimitiveTopologyClassLine;
        case PrimitiveTopology::TriangleList:
        case PrimitiveTopology::TriangleStrip:
            return MTLPrimitiveTopologyClassTriangle;
    }
    return MTLPrimitiveTopologyClassUnspecified;
}

[[nodiscard]] MTLBlendFactor blend_factor(BlendFactor factor) noexcept {
    switch (factor) {
        case BlendFactor::Zero:
            return MTLBlendFactorZero;
        case BlendFactor::One:
            return MTLBlendFactorOne;
        case BlendFactor::SourceColor:
            return MTLBlendFactorSourceColor;
        case BlendFactor::OneMinusSourceColor:
            return MTLBlendFactorOneMinusSourceColor;
        case BlendFactor::DestinationColor:
            return MTLBlendFactorDestinationColor;
        case BlendFactor::OneMinusDestinationColor:
            return MTLBlendFactorOneMinusDestinationColor;
        case BlendFactor::SourceAlpha:
            return MTLBlendFactorSourceAlpha;
        case BlendFactor::OneMinusSourceAlpha:
            return MTLBlendFactorOneMinusSourceAlpha;
        case BlendFactor::DestinationAlpha:
            return MTLBlendFactorDestinationAlpha;
        case BlendFactor::OneMinusDestinationAlpha:
            return MTLBlendFactorOneMinusDestinationAlpha;
    }
    return MTLBlendFactorOne;
}

[[nodiscard]] MTLBlendOperation blend_operation(BlendOp operation) noexcept {
    switch (operation) {
        case BlendOp::Add:
            return MTLBlendOperationAdd;
        case BlendOp::Subtract:
            return MTLBlendOperationSubtract;
        case BlendOp::ReverseSubtract:
            return MTLBlendOperationReverseSubtract;
        case BlendOp::Min:
            return MTLBlendOperationMin;
        case BlendOp::Max:
            return MTLBlendOperationMax;
    }
    return MTLBlendOperationAdd;
}

[[nodiscard]] MTLColorWriteMask color_write_mask(ColorComponent components) noexcept {
    const u8 bits = static_cast<u8>(components);
    MTLColorWriteMask mask = MTLColorWriteMaskNone;
    if ((bits & static_cast<u8>(ColorComponent::R)) != 0) {
        mask |= MTLColorWriteMaskRed;
    }
    if ((bits & static_cast<u8>(ColorComponent::G)) != 0) {
        mask |= MTLColorWriteMaskGreen;
    }
    if ((bits & static_cast<u8>(ColorComponent::B)) != 0) {
        mask |= MTLColorWriteMaskBlue;
    }
    if ((bits & static_cast<u8>(ColorComponent::A)) != 0) {
        mask |= MTLColorWriteMaskAlpha;
    }
    return mask;
}

[[nodiscard]] MTLPrimitiveType primitive_type(PrimitiveTopology topology) noexcept {
    switch (topology) {
        case PrimitiveTopology::PointList:
            return MTLPrimitiveTypePoint;
        case PrimitiveTopology::LineList:
            return MTLPrimitiveTypeLine;
        case PrimitiveTopology::LineStrip:
            return MTLPrimitiveTypeLineStrip;
        case PrimitiveTopology::TriangleList:
            return MTLPrimitiveTypeTriangle;
        case PrimitiveTopology::TriangleStrip:
            return MTLPrimitiveTypeTriangleStrip;
    }
    return MTLPrimitiveTypeTriangle;
}

class MetalCommandBuffer final : public CommandBuffer {
public:
    MetalCommandBuffer(id<MTLCommandQueue> queue,
                       HandlePool<MetalBuffer, BufferTag>& buffers,
                       HandlePool<MetalTexture, TextureTag>& textures,
                       HandlePool<MetalTextureView, TextureViewTag>& views,
                       HandlePool<MetalDescriptorSet, DescriptorSetTag>& descriptor_sets,
                       HandlePool<MetalPipelineLayout, PipelineLayoutTag>& pipeline_layouts,
                       HandlePool<MetalQueryPool, QueryPoolTag>& query_pools,
                       id<MTLBuffer> breadcrumb_buffer,
                       HandlePool<MetalGraphicsPipeline, GraphicsPipelineTag>& graphics,
                       HandlePool<MetalComputePipeline, ComputePipelineTag>& compute) noexcept
        : queue_(queue), buffers_(&buffers), textures_(&textures), views_(&views),
          descriptor_sets_(&descriptor_sets), pipeline_layouts_(&pipeline_layouts),
          query_pools_(&query_pools),
          breadcrumb_buffer_(breadcrumb_buffer),
          graphics_(&graphics), compute_(&compute) {}

    void set_handle(CommandBufferHandle handle) noexcept { handle_ = handle; }
    [[nodiscard]] CommandBufferHandle handle() const noexcept override { return handle_; }
    [[nodiscard]] id<MTLCommandBuffer> raw() const noexcept { return command_; }
    [[nodiscard]] bool recording() const noexcept { return recording_; }
    [[nodiscard]] bool submitted() const noexcept { return submitted_; }
    void mark_submitted() noexcept { submitted_ = true; }
    void synchronize_resources() noexcept { end_encoder(); }

    Status begin() noexcept {
        if (recording_ || submitted_) {
            return fail(ErrorCode::InvalidArgument,
                        "a Metal command buffer can be recorded and submitted only once");
        }
        command_ = [queue_ commandBuffer];
        if (command_ == nil) {
            return fail(ErrorCode::OutOfMemory, "Metal could not allocate a command buffer");
        }
        recording_ = true;
        breadcrumb_cursor_ = 0;
        return ok();
    }

    Status end() noexcept {
        if (!recording_) {
            return fail(ErrorCode::InvalidArgument, "Metal command buffer is not recording");
        }
        end_encoder();
        recording_ = false;
        return ok();
    }

    void begin_rendering(const RenderingInfo& info) noexcept override {
        end_encoder();
        MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
        for (usize index = 0; index < info.color_attachments.size(); ++index) {
            const RenderAttachment& attachment = info.color_attachments[index];
            const MetalTextureView* view = views_->resolve(attachment.view);
            if (view == nullptr) {
                return;
            }
            MTLRenderPassColorAttachmentDescriptor* color = pass.colorAttachments[index];
            color.texture = view->texture;
            color.loadAction = static_cast<MTLLoadAction>(metal_load_action(attachment.load));
            color.storeAction = static_cast<MTLStoreAction>(metal_store_action(attachment.store));
            color.clearColor = MTLClearColorMake(
                static_cast<double>(attachment.clear.color[0]),
                static_cast<double>(attachment.clear.color[1]),
                static_cast<double>(attachment.clear.color[2]),
                static_cast<double>(attachment.clear.color[3]));
            if (!attachment.resolve_view.is_null()) {
                const MetalTextureView* resolve = views_->resolve(attachment.resolve_view);
                if (resolve == nullptr) {
                    return;
                }
                color.resolveTexture = resolve->texture;
                color.storeAction = attachment.store == StoreOp::Store
                                        ? MTLStoreActionStoreAndMultisampleResolve
                                        : MTLStoreActionMultisampleResolve;
            }
        }
        if (!info.depth_attachment.view.is_null()) {
            const RenderAttachment& attachment = info.depth_attachment;
            const MetalTextureView* view = views_->resolve(attachment.view);
            if (view == nullptr) {
                return;
            }
            pass.depthAttachment.texture = view->texture;
            pass.depthAttachment.loadAction =
                static_cast<MTLLoadAction>(metal_load_action(attachment.load));
            pass.depthAttachment.storeAction =
                static_cast<MTLStoreAction>(metal_store_action(attachment.store));
            pass.depthAttachment.clearDepth =
                static_cast<double>(attachment.clear.depth_stencil.depth);
            if (view->texture.pixelFormat == MTLPixelFormatDepth32Float_Stencil8) {
                pass.stencilAttachment.texture = view->texture;
                pass.stencilAttachment.loadAction = pass.depthAttachment.loadAction;
                pass.stencilAttachment.storeAction = pass.depthAttachment.storeAction;
                pass.stencilAttachment.clearStencil = attachment.clear.depth_stencil.stencil;
            }
        }
        render_ = [command_ renderCommandEncoderWithDescriptor:pass];
    }

    void end_rendering() noexcept override {
        if (render_ != nil) {
            [render_ endEncoding];
            render_ = nil;
        }
    }

    void set_viewport(const Viewport& viewport) noexcept override {
        if (render_ != nil) {
            [render_ setViewport:MTLViewport{static_cast<double>(viewport.x),
                                            static_cast<double>(viewport.y),
                                            static_cast<double>(viewport.width),
                                            static_cast<double>(viewport.height),
                                            static_cast<double>(viewport.min_depth),
                                            static_cast<double>(viewport.max_depth)}];
        }
    }

    void set_scissor(const Rect2D& scissor) noexcept override {
        if (render_ != nil) {
            const NSUInteger x = scissor.x < 0 ? 0 : static_cast<NSUInteger>(scissor.x);
            const NSUInteger y = scissor.y < 0 ? 0 : static_cast<NSUInteger>(scissor.y);
            [render_ setScissorRect:MTLScissorRect{x, y, scissor.width, scissor.height}];
        }
    }

    void bind_graphics_pipeline(GraphicsPipelineHandle handle) noexcept override {
        const MetalGraphicsPipeline* pipeline = graphics_->resolve(handle);
        if (render_ == nil || pipeline == nullptr) {
            return;
        }
        graphics_pipeline_ = pipeline;
        [render_ setRenderPipelineState:pipeline->pipeline];
        [render_ setDepthStencilState:pipeline->depth_stencil];
        const MTLCullMode cull = pipeline->rasterisation.cull_mode == CullMode::Front
                                     ? MTLCullModeFront
                                     : (pipeline->rasterisation.cull_mode == CullMode::Back
                                            ? MTLCullModeBack
                                            : MTLCullModeNone);
        [render_ setCullMode:cull];
        [render_ setFrontFacingWinding:
                     pipeline->rasterisation.front_face == FrontFace::CounterClockwise
                         ? MTLWindingCounterClockwise
                         : MTLWindingClockwise];
        [render_ setTriangleFillMode:pipeline->rasterisation.polygon_mode == PolygonMode::Line
                                         ? MTLTriangleFillModeLines
                                         : MTLTriangleFillModeFill];
        [render_ setDepthBias:pipeline->rasterisation.depth_bias_constant
                  slopeScale:pipeline->rasterisation.depth_bias_slope
                       clamp:0.0F];
    }

    void bind_compute_pipeline(ComputePipelineHandle handle) noexcept override {
        const MetalComputePipeline* pipeline = compute_->resolve(handle);
        if (pipeline == nullptr) {
            return;
        }
        ensure_compute();
        compute_pipeline_ = pipeline;
        [compute_encoder_ setComputePipelineState:pipeline->pipeline];
    }

    void bind_descriptor_sets(PipelineLayoutHandle layout_handle, u32 first_set,
                              Span<const DescriptorSetHandle> handles) noexcept override {
        const MetalPipelineLayout* layout = pipeline_layouts_->resolve(layout_handle);
        if (layout == nullptr || first_set + handles.size() > layout->set_count) {
            return;
        }
        for (usize offset = 0; offset < handles.size(); ++offset) {
            const MetalDescriptorSet* set = descriptor_sets_->resolve(handles[offset]);
            if (set == nullptr || set->layout != layout->set_layouts[first_set + offset]) {
                return;
            }
            const NSUInteger index = first_set + offset;
            if (render_ != nil) {
                [render_ setVertexBuffer:set->argument_buffer offset:0 atIndex:index];
                [render_ setFragmentBuffer:set->argument_buffer offset:0 atIndex:index];
                for (id<MTLResource> resource in set->resources.allValues) {
                    [render_ useResource:resource
                                  usage:MTLResourceUsageRead | MTLResourceUsageWrite
                                 stages:MTLRenderStageVertex | MTLRenderStageFragment];
                }
            }
            if (compute_encoder_ != nil) {
                [compute_encoder_ setBuffer:set->argument_buffer offset:0 atIndex:index];
                for (id<MTLResource> resource in set->resources.allValues) {
                    [compute_encoder_ useResource:resource
                                           usage:MTLResourceUsageRead | MTLResourceUsageWrite];
                }
            }
        }
    }

    void push_constants(PipelineLayoutHandle layout_handle, ShaderStage stages, u32 offset,
                        Span<const u8> data) noexcept override {
        const MetalPipelineLayout* layout = pipeline_layouts_->resolve(layout_handle);
        if (layout == nullptr || offset != 0 || data.empty()) {
            return;
        }
        const NSUInteger push_constant_index = layout->set_count;
        if (render_ != nil && has_stage(stages, ShaderStage::Vertex)) {
            [render_ setVertexBytes:data.data() length:data.size() atIndex:push_constant_index];
        }
        if (render_ != nil && has_stage(stages, ShaderStage::Fragment)) {
            [render_ setFragmentBytes:data.data() length:data.size() atIndex:push_constant_index];
        }
        if (has_stage(stages, ShaderStage::Compute)) {
            ensure_compute();
            [compute_encoder_ setBytes:data.data() length:data.size() atIndex:push_constant_index];
        }
    }

    void bind_vertex_buffers(u32 first_binding, Span<const BufferHandle> handles,
                             Span<const u64> offsets) noexcept override {
        if (render_ == nil || handles.size() != offsets.size()) {
            return;
        }
        for (usize index = 0; index < handles.size(); ++index) {
            const MetalBuffer* buffer = buffers_->resolve(handles[index]);
            if (buffer != nullptr) {
                [render_ setVertexBuffer:buffer->buffer
                                  offset:static_cast<NSUInteger>(offsets[index])
                                 atIndex:kMetalVertexBufferBase + first_binding + index];
            }
        }
    }

    void bind_index_buffer(BufferHandle handle, u64 offset, bool wide) noexcept override {
        const MetalBuffer* buffer = buffers_->resolve(handle);
        index_buffer_ = buffer != nullptr ? buffer->buffer : nil;
        index_offset_ = static_cast<NSUInteger>(offset);
        index_type_ = wide ? MTLIndexTypeUInt32 : MTLIndexTypeUInt16;
    }

    void draw(u32 vertex_count, u32 instance_count, u32 first_vertex,
              u32 first_instance) noexcept override {
        if (render_ != nil && graphics_pipeline_ != nullptr) {
            [render_ drawPrimitives:primitive_type(graphics_pipeline_->topology)
                        vertexStart:first_vertex
                        vertexCount:vertex_count
                      instanceCount:instance_count
                       baseInstance:first_instance];
        }
    }

    void draw_indexed(u32 index_count, u32 instance_count, u32 first_index,
                      i32 vertex_offset, u32 first_instance) noexcept override {
        if (render_ == nil || graphics_pipeline_ == nullptr || index_buffer_ == nil) {
            return;
        }
        const NSUInteger index_size = index_type_ == MTLIndexTypeUInt32 ? 4 : 2;
        [render_ drawIndexedPrimitives:primitive_type(graphics_pipeline_->topology)
                            indexCount:index_count
                             indexType:index_type_
                           indexBuffer:index_buffer_
                     indexBufferOffset:index_offset_ + first_index * index_size
                         instanceCount:instance_count
                            baseVertex:vertex_offset
                          baseInstance:first_instance];
    }

    void draw_indexed_indirect(BufferHandle arguments, u64 offset, u32 draw_count,
                               u32 stride) noexcept override {
        const MetalBuffer* buffer = buffers_->resolve(arguments);
        if (render_ == nil || graphics_pipeline_ == nullptr || index_buffer_ == nil ||
            buffer == nullptr) {
            return;
        }
        for (u32 draw_index = 0; draw_index < draw_count; ++draw_index) {
            [render_ drawIndexedPrimitives:primitive_type(graphics_pipeline_->topology)
                                 indexType:index_type_
                               indexBuffer:index_buffer_
                         indexBufferOffset:index_offset_
                            indirectBuffer:buffer->buffer
                      indirectBufferOffset:static_cast<NSUInteger>(offset) + draw_index * stride];
        }
    }

    void dispatch(u32 groups_x, u32 groups_y, u32 groups_z) noexcept override {
        if (compute_encoder_ != nil && compute_pipeline_ != nullptr) {
            [compute_encoder_ dispatchThreadgroups:MTLSizeMake(groups_x, groups_y, groups_z)
                             threadsPerThreadgroup:compute_pipeline_->threads_per_threadgroup];
        }
    }

    void dispatch_indirect(BufferHandle arguments, u64 offset) noexcept override {
        const MetalBuffer* buffer = buffers_->resolve(arguments);
        if (compute_encoder_ != nil && compute_pipeline_ != nullptr && buffer != nullptr) {
            [compute_encoder_ dispatchThreadgroupsWithIndirectBuffer:buffer->buffer
                                                indirectBufferOffset:offset
                                               threadsPerThreadgroup:compute_pipeline_->threads_per_threadgroup];
        }
    }

    void copy_buffer(BufferHandle source, BufferHandle destination,
                     Span<const BufferCopy> regions) noexcept override {
        const MetalBuffer* src = buffers_->resolve(source);
        const MetalBuffer* dst = buffers_->resolve(destination);
        if (src == nullptr || dst == nullptr) {
            return;
        }
        ensure_blit();
        for (const BufferCopy& region : regions) {
            [blit_ copyFromBuffer:src->buffer
                    sourceOffset:region.source_offset
                        toBuffer:dst->buffer
               destinationOffset:region.destination_offset
                            size:region.size];
        }
    }

    void copy_buffer_to_texture(BufferHandle source, TextureHandle destination,
                                Span<const BufferTextureCopy> regions) noexcept override {
        const MetalBuffer* src = buffers_->resolve(source);
        const MetalTexture* dst = textures_->resolve(destination);
        if (src == nullptr || dst == nullptr) {
            return;
        }
        ensure_blit();
        for (const BufferTextureCopy& region : regions) {
            const u64 row_pixels = region.buffer_row_length == 0
                                       ? region.texture_extent.width
                                       : region.buffer_row_length;
            const u64 rows = region.buffer_image_height == 0
                                 ? region.texture_extent.height
                                 : region.buffer_image_height;
            const u64 bytes_per_row =
                row_pixels * format_info(dst->desc.format).bytes_per_block;
            const u64 bytes_per_image = bytes_per_row * rows;
            for (u16 layer = 0; layer < region.layer_count; ++layer) {
                [blit_ copyFromBuffer:src->buffer
                        sourceOffset:region.buffer_offset + layer * bytes_per_image
                   sourceBytesPerRow:bytes_per_row
                 sourceBytesPerImage:bytes_per_image
                          sourceSize:MTLSizeMake(region.texture_extent.width,
                                                region.texture_extent.height,
                                                region.texture_extent.depth)
                           toTexture:dst->texture
                    destinationSlice:region.base_layer + layer
                    destinationLevel:region.mip_level
                   destinationOrigin:MTLOriginMake(region.texture_offset.x,
                                                   region.texture_offset.y,
                                                   region.texture_offset.z)];
            }
        }
    }

    void copy_texture_to_buffer(TextureHandle source, BufferHandle destination,
                                Span<const BufferTextureCopy> regions) noexcept override {
        const MetalTexture* src = textures_->resolve(source);
        const MetalBuffer* dst = buffers_->resolve(destination);
        if (src == nullptr || dst == nullptr) {
            return;
        }
        ensure_blit();
        for (const BufferTextureCopy& region : regions) {
            const u64 row_pixels = region.buffer_row_length == 0
                                       ? region.texture_extent.width
                                       : region.buffer_row_length;
            const u64 rows = region.buffer_image_height == 0
                                 ? region.texture_extent.height
                                 : region.buffer_image_height;
            const u64 bytes_per_row =
                row_pixels * format_info(src->desc.format).bytes_per_block;
            const u64 bytes_per_image = bytes_per_row * rows;
            for (u16 layer = 0; layer < region.layer_count; ++layer) {
                [blit_ copyFromTexture:src->texture
                           sourceSlice:region.base_layer + layer
                           sourceLevel:region.mip_level
                          sourceOrigin:MTLOriginMake(region.texture_offset.x,
                                                    region.texture_offset.y,
                                                    region.texture_offset.z)
                            sourceSize:MTLSizeMake(region.texture_extent.width,
                                                  region.texture_extent.height,
                                                  region.texture_extent.depth)
                              toBuffer:dst->buffer
                     destinationOffset:region.buffer_offset + layer * bytes_per_image
                destinationBytesPerRow:bytes_per_row
              destinationBytesPerImage:bytes_per_image];
            }
        }
    }

    void begin_debug_label(const char* name) noexcept override {
        if (command_ != nil && name != nullptr) {
            [command_ pushDebugGroup:[NSString stringWithUTF8String:name]];
        }
    }
    void end_debug_label() noexcept override {
        if (command_ != nil) {
            [command_ popDebugGroup];
        }
    }
    void insert_debug_label(const char* name) noexcept override {
        if (command_ != nil && name != nullptr) {
            [command_ pushDebugGroup:[NSString stringWithUTF8String:name]];
            [command_ popDebugGroup];
        }
    }
    void write_timestamp(QueryPoolHandle handle, u32 index) noexcept override {
        MetalQueryPool* pool = query_pools_->resolve(handle);
        if (pool == nullptr || pool->kind != QueryKind::Timestamp || index >= pool->count) {
            return;
        }
        if (render_ != nil) {
            if (![queue_.device supportsCounterSampling:MTLCounterSamplingPointAtDrawBoundary]) {
                return;
            }
            [render_ sampleCountersInBuffer:pool->samples
                              atSampleIndex:index
                                withBarrier:YES];
        } else if (compute_encoder_ != nil) {
            if (![queue_.device
                    supportsCounterSampling:MTLCounterSamplingPointAtDispatchBoundary]) {
                return;
            }
            [compute_encoder_ sampleCountersInBuffer:pool->samples
                                       atSampleIndex:index
                                         withBarrier:YES];
        } else if (blit_ != nil &&
                   [queue_.device supportsCounterSampling:MTLCounterSamplingPointAtBlitBoundary]) {
            [blit_ sampleCountersInBuffer:pool->samples
                            atSampleIndex:index
                              withBarrier:YES];
        } else {
            if (![queue_.device
                    supportsCounterSampling:MTLCounterSamplingPointAtStageBoundary]) {
                return;
            }
            end_encoder();
            MTLBlitPassDescriptor* pass = [MTLBlitPassDescriptor blitPassDescriptor];
            MTLBlitPassSampleBufferAttachmentDescriptor* attachment =
                pass.sampleBufferAttachments[0];
            attachment.sampleBuffer = pool->samples;
            attachment.startOfEncoderSampleIndex = MTLCounterDontSample;
            attachment.endOfEncoderSampleIndex = index;
            id<MTLBlitCommandEncoder> sampler =
                [command_ blitCommandEncoderWithDescriptor:pass];
            [sampler endEncoding];
        }
        [pool->written addIndex:index];
    }
    void reset_queries(QueryPoolHandle handle, u32 first, u32 count) noexcept override {
        MetalQueryPool* pool = query_pools_->resolve(handle);
        if (pool != nullptr && first <= pool->count && count <= pool->count - first) {
            [pool->written removeIndexesInRange:NSMakeRange(first, count)];
        }
    }
    void write_breadcrumb(u32 slot, u32 value) noexcept override {
        if (slot >= kMetalBreadcrumbSlots || breadcrumb_buffer_ == nil ||
            breadcrumb_cursor_ >= kMetalBreadcrumbSlots) {
            return;
        }
        if (breadcrumb_staging_ == nil) {
            breadcrumb_staging_ = [queue_.device
                newBufferWithLength:kMetalBreadcrumbSlots * sizeof(u32)
                            options:MTLResourceStorageModeShared];
            if (breadcrumb_staging_ == nil) {
                return;
            }
        }
        auto* values = static_cast<u32*>(breadcrumb_staging_.contents);
        values[breadcrumb_cursor_] = value;
        ensure_blit();
        [blit_ copyFromBuffer:breadcrumb_staging_
                sourceOffset:breadcrumb_cursor_ * sizeof(u32)
                    toBuffer:breadcrumb_buffer_
           destinationOffset:slot * sizeof(u32)
                        size:sizeof(u32)];
        ++breadcrumb_cursor_;
    }
    [[nodiscard]] void* native_handle() noexcept override {
        return (__bridge void*)command_;
    }

private:
    void end_encoder() noexcept {
        if (render_ != nil) {
            [render_ endEncoding];
            render_ = nil;
        }
        if (compute_encoder_ != nil) {
            [compute_encoder_ endEncoding];
            compute_encoder_ = nil;
        }
        if (blit_ != nil) {
            [blit_ endEncoding];
            blit_ = nil;
        }
    }
    void ensure_compute() noexcept {
        if (compute_encoder_ == nil) {
            end_encoder();
            compute_encoder_ = [command_ computeCommandEncoder];
        }
    }
    void ensure_blit() noexcept {
        if (blit_ == nil) {
            end_encoder();
            blit_ = [command_ blitCommandEncoder];
        }
    }

    CommandBufferHandle handle_{};
    id<MTLCommandQueue> queue_ = nil;
    id<MTLCommandBuffer> command_ = nil;
    id<MTLRenderCommandEncoder> render_ = nil;
    id<MTLComputeCommandEncoder> compute_encoder_ = nil;
    id<MTLBlitCommandEncoder> blit_ = nil;
    HandlePool<MetalBuffer, BufferTag>* buffers_ = nullptr;
    HandlePool<MetalTexture, TextureTag>* textures_ = nullptr;
    HandlePool<MetalTextureView, TextureViewTag>* views_ = nullptr;
    HandlePool<MetalDescriptorSet, DescriptorSetTag>* descriptor_sets_ = nullptr;
    HandlePool<MetalPipelineLayout, PipelineLayoutTag>* pipeline_layouts_ = nullptr;
    HandlePool<MetalQueryPool, QueryPoolTag>* query_pools_ = nullptr;
    id<MTLBuffer> breadcrumb_buffer_ = nil;
    id<MTLBuffer> breadcrumb_staging_ = nil;
    HandlePool<MetalGraphicsPipeline, GraphicsPipelineTag>* graphics_ = nullptr;
    HandlePool<MetalComputePipeline, ComputePipelineTag>* compute_ = nullptr;
    const MetalGraphicsPipeline* graphics_pipeline_ = nullptr;
    const MetalComputePipeline* compute_pipeline_ = nullptr;
    id<MTLBuffer> index_buffer_ = nil;
    NSUInteger index_offset_ = 0;
    MTLIndexType index_type_ = MTLIndexTypeUInt16;
    bool recording_ = false;
    bool submitted_ = false;
    u32 breadcrumb_cursor_ = 0;
};

void MetalBarrierRecorder::record_barriers(CommandBufferHandle handle,
                                           const BarrierBatch& batch) noexcept {
    if (batch.empty()) {
        return;
    }
    MetalCommandBuffer* command_buffer = command_buffers_->resolve(handle);
    if (command_buffer == nullptr) {
        return;
    }
    // All Metal resources in this backend use tracked hazards. Ending the active encoder is the
    // full dependency point between graph passes; Metal then derives cache visibility from the
    // resources each adjacent encoder uses, with no image-layout or ownership state to translate.
    command_buffer->synchronize_resources();
    ++batches_;
    barriers_ += batch.count();
}

[[nodiscard]] MTLTextureType texture_type(const TextureDescription& desc) noexcept {
    switch (desc.dimension) {
        case TextureDimension::Texture1D:
            return desc.array_layers > 1 ? MTLTextureType1DArray : MTLTextureType1D;
        case TextureDimension::Texture2D:
            if (desc.sample_count > 1) {
                return desc.array_layers > 1 ? MTLTextureType2DMultisampleArray
                                             : MTLTextureType2DMultisample;
            }
            return desc.array_layers > 1 ? MTLTextureType2DArray : MTLTextureType2D;
        case TextureDimension::Texture3D:
            return MTLTextureType3D;
        case TextureDimension::Cube:
            return desc.array_layers > 6 ? MTLTextureTypeCubeArray : MTLTextureTypeCube;
    }
    return MTLTextureType2D;
}

[[nodiscard]] MTLTextureUsage texture_usage(TextureUsage usage) noexcept {
    // Every RHI texture may be viewed through TextureViewHandle, including a mip or layer subset.
    // Metal requires that intent at texture creation time.
    MTLTextureUsage result = MTLTextureUsagePixelFormatView;
    if (has_usage(usage, TextureUsage::Sampled) ||
        has_usage(usage, TextureUsage::InputAttachment)) {
        result |= MTLTextureUsageShaderRead;
    }
    if (has_usage(usage, TextureUsage::Storage)) {
        result |= MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
    }
    if (has_usage(usage, TextureUsage::ColorAttachment) ||
        has_usage(usage, TextureUsage::DepthStencilAttachment) ||
        has_usage(usage, TextureUsage::TransientAttachment)) {
        result |= MTLTextureUsageRenderTarget;
    }
    return result;
}

[[nodiscard]] MTLTextureDescriptor* texture_descriptor(const TextureDescription& desc,
                                                        bool memoryless) noexcept {
    MTLTextureDescriptor* descriptor = [[MTLTextureDescriptor alloc] init];
    descriptor.textureType = texture_type(desc);
    descriptor.pixelFormat = static_cast<MTLPixelFormat>(metal_pixel_format(desc.format));
    descriptor.width = static_cast<NSUInteger>(desc.extent.width);
    descriptor.height = static_cast<NSUInteger>(desc.extent.height);
    descriptor.depth = static_cast<NSUInteger>(desc.extent.depth);
    descriptor.mipmapLevelCount = static_cast<NSUInteger>(desc.mip_levels);
    descriptor.arrayLength = desc.dimension == TextureDimension::Cube
                                 ? static_cast<NSUInteger>((desc.array_layers + 5) / 6)
                                 : static_cast<NSUInteger>(desc.array_layers);
    descriptor.sampleCount = static_cast<NSUInteger>(desc.sample_count);
    descriptor.usage = texture_usage(desc.usage);
    descriptor.storageMode = memoryless
                                 ? MTLStorageModeMemoryless
                                 : (desc.memory == MemoryUse::DeviceLocal
                                        ? MTLStorageModePrivate
                                        : MTLStorageModeShared);
    descriptor.hazardTrackingMode = MTLHazardTrackingModeTracked;
    return descriptor;
}

/// The first native-device checkpoint. It owns a real MTLDevice and queue and reports the device's
/// capabilities without pretending the still-red resource and command paths work. Each unsupported
/// method fails by name; subsequent M11.d.5 checkpoints replace these methods with native objects.
class MetalDevice final : public Device {
public:
    MetalDevice(Allocator& allocator, id<MTLDevice> device,
                const DeviceDescription& desc) noexcept
        : device_(device), queue_(default_metal_queue()),
          buffers_(MemoryDomain::Gpu, "rhi.metal.buffers"),
          textures_(MemoryDomain::Gpu, "rhi.metal.textures"),
          views_(MemoryDomain::Gpu, "rhi.metal.views"),
          samplers_(MemoryDomain::Gpu, "rhi.metal.samplers"),
          query_pools_(MemoryDomain::Gpu, "rhi.metal.query-pools"),
          shaders_(MemoryDomain::Gpu, "rhi.metal.shaders"),
          descriptor_set_layouts_(MemoryDomain::Gpu, "rhi.metal.descriptor-set-layouts"),
          descriptor_sets_(MemoryDomain::Gpu, "rhi.metal.descriptor-sets"),
          pipeline_layouts_(MemoryDomain::Gpu, "rhi.metal.pipeline-layouts"),
          graphics_pipelines_(MemoryDomain::Gpu, "rhi.metal.graphics-pipelines"),
          compute_pipelines_(MemoryDomain::Gpu, "rhi.metal.compute-pipelines"),
          command_buffers_(MemoryDomain::Gpu, "rhi.metal.command-buffers"),
          fences_(MemoryDomain::Gpu, "rhi.metal.fences"),
          semaphores_(MemoryDomain::Gpu, "rhi.metal.semaphores"),
          swapchains_(MemoryDomain::Gpu, "rhi.metal.swapchains"),
          transient_textures_(allocator),
          transient_buffers_(allocator),
          barriers_(command_buffers_) {
        for (u32 index = 0; index < kMaxFramesInFlight; ++index) {
            per_frame_descriptor_sets_[index] = Array<DescriptorSetHandle>(allocator);
            per_frame_command_buffers_[index] = Array<CommandBufferHandle>(allocator);
        }
        buffers_.set_allocator(allocator);
        textures_.set_allocator(allocator);
        views_.set_allocator(allocator);
        samplers_.set_allocator(allocator);
        query_pools_.set_allocator(allocator);
        shaders_.set_allocator(allocator);
        descriptor_set_layouts_.set_allocator(allocator);
        descriptor_sets_.set_allocator(allocator);
        pipeline_layouts_.set_allocator(allocator);
        graphics_pipelines_.set_allocator(allocator);
        compute_pipelines_.set_allocator(allocator);
        command_buffers_.set_allocator(allocator);
        fences_.set_allocator(allocator);
        semaphores_.set_allocator(allocator);
        swapchains_.set_allocator(allocator);
        timeline_event_ = [device_ newSharedEvent];
        timeline_event_.label = @"Cyberdyne graphics timeline";
        breadcrumb_buffer_ = [device_ newBufferWithLength:kMetalBreadcrumbSlots * sizeof(u32)
                                                   options:MTLResourceStorageModeShared];
        breadcrumb_buffer_.label = @"Cyberdyne GPU breadcrumbs";
        if (breadcrumb_buffer_ != nil) {
            std::memset(breadcrumb_buffer_.contents, 0,
                        kMetalBreadcrumbSlots * sizeof(u32));
        }
        frames_in_flight_ = desc.frames_in_flight == 0 ? kDefaultFramesInFlight
                                                       : desc.frames_in_flight;
        capabilities_.set_backend(BackendKind::Metal);
        capabilities_.set_device_name(device.name.UTF8String);
        capabilities_.set_vendor_id(0x106BU);
        capabilities_.set_driver_version("Metal");
        capabilities_.set_native_shader_format(ShaderFormat::Msl);
        capabilities_.set_needs_queue_ownership_transfer(false);
        capabilities_.set(Capability::ComputeShaders, true);
        capabilities_.set(Capability::DynamicRendering, true);
        capabilities_.set(Capability::HostVisibleDeviceLocalMemory, device.hasUnifiedMemory);
        capabilities_.set(Capability::DebugMarkers, true);
        capabilities_.set(Capability::ParallelPassRecording, false);
        memory_.device_heap_size = device.recommendedMaxWorkingSetSize;
        memory_.device_heap_budget = device.recommendedMaxWorkingSetSize;

        const bool bindless = device.argumentBuffersSupport == MTLArgumentBuffersTier2;
        capabilities_.set(Capability::Bindless, bindless);
        capabilities_.set(Capability::BindlessPartiallyBound, bindless);
        capabilities_.set(Capability::DescriptorIndexingNonUniform, bindless);

        DeviceLimits& limits = capabilities_.limits();
        limits.max_bound_descriptor_sets = kMaxDescriptorSets;
        limits.max_push_constant_bytes = kMaxPushConstantBytes;
        limits.max_vertex_attributes = kMaxVertexAttributes;
        limits.max_color_attachments = kMaxColorAttachments;
        limits.max_texture_dimension_2d = 16384;
        limits.max_texture_array_layers = 2048;
        limits.max_compute_workgroup_size[0] = 1024;
        limits.max_compute_workgroup_size[1] = 1024;
        limits.max_compute_workgroup_size[2] = 1024;
        limits.max_compute_workgroup_invocations = 1024;
        limits.min_uniform_buffer_offset_alignment = 256;
        limits.min_storage_buffer_offset_alignment = 16;
        limits.optimal_buffer_copy_offset_alignment = 4;
        limits.max_sampler_anisotropy = 16.0F;
        limits.timestamp_period_ns = 1;

        for (u32 value = 1; value < static_cast<u32>(Format::Count); ++value) {
            const Format format = static_cast<Format>(value);
            if (metal_pixel_format(format) == kMetalPixelFormatInvalid) {
                continue;
            }
            FormatFeature features = FormatFeature::SampledImage | FormatFeature::BlitSource |
                                     FormatFeature::BlitDestination;
            if (format_is_depth_stencil(format)) {
                features = features | FormatFeature::DepthStencilAttachment;
            } else if (!format_info(format).is_compressed) {
                features = features | FormatFeature::ColorAttachment |
                           FormatFeature::ColorAttachmentBlend | FormatFeature::StorageImage;
            }
            capabilities_.set_format_features(format, features);
        }
        if (bindless) {
            bindless_ready_ = initialise_bindless_table();
        }
    }

    [[nodiscard]] bool valid() const noexcept {
        return device_ != nil && queue_ != nil && timeline_event_ != nil &&
               breadcrumb_buffer_ != nil &&
               (!capabilities_.has(Capability::Bindless) || bindless_ready_);
    }
    [[nodiscard]] const DeviceCapabilities& capabilities() const noexcept override {
        return capabilities_;
    }
    [[nodiscard]] DescriptorModel descriptor_model() const noexcept override {
        return capabilities_.has(Capability::Bindless) ? DescriptorModel::Bindless
                                                       : DescriptorModel::Compatibility;
    }
    [[nodiscard]] u32 frames_in_flight() const noexcept override { return frames_in_flight_; }
    [[nodiscard]] bool has_queue(QueueKind queue) const noexcept override {
        return queue == QueueKind::Graphics;
    }
    void set_validation_callback(ValidationCallback callback, void* user) noexcept override {
        validation_callback_ = callback;
        validation_user_ = user;
    }

    Expected<BufferHandle, Error> create_buffer(const BufferDescription& desc) override {
        ValidationMessage message;
        if (Status valid = validate_buffer(desc, message); !valid) {
            report_validation(ValidationSeverity::Error, message.text);
            return make_unexpected(valid.error());
        }
        const MTLResourceOptions options = desc.memory == MemoryUse::DeviceLocal
                                               ? MTLResourceStorageModePrivate
                                               : MTLResourceStorageModeShared;
        id<MTLBuffer> native =
            [device_ newBufferWithLength:static_cast<NSUInteger>(desc.size) options:options];
        if (native == nil) {
            return fail(ErrorCode::OutOfMemory, "Metal could not allocate a buffer");
        }
        MetalBuffer buffer;
        buffer.name.assign(desc.name);
        buffer.desc = desc;
        buffer.desc.name = buffer.name.text;
        buffer.buffer = native;
        buffer.bytes = desc.size;
        native.label = [NSString stringWithUTF8String:buffer.name.text];
        Expected<BufferHandle, Error> handle = buffers_.create(buffer);
        if (handle) {
            buffers_.resolve(*handle)->desc.name = buffers_.resolve(*handle)->name.text;
            charge(desc.memory, desc.size);
        }
        return handle;
    }
    void destroy_buffer(BufferHandle handle) noexcept override {
        MetalBuffer* buffer = buffers_.resolve(handle);
        if (buffer == nullptr) {
            report_validation(ValidationSeverity::Error,
                              "destroy_buffer() on a stale or never-issued Metal handle");
            return;
        }
        discharge(buffer->desc.memory, buffer->bytes);
        (void)buffers_.destroy(handle);
        ++stats_.resources_freed;
    }
    [[nodiscard]] bool is_valid(BufferHandle handle) const noexcept override {
        return buffers_.resolve(handle) != nullptr;
    }
    [[nodiscard]] void* buffer_mapped_pointer(BufferHandle handle) noexcept override {
        MetalBuffer* buffer = buffers_.resolve(handle);
        return buffer != nullptr && buffer->desc.memory != MemoryUse::DeviceLocal
                   ? buffer->buffer.contents
                   : nullptr;
    }
    [[nodiscard]] const BufferDescription* buffer_description(
        BufferHandle handle) const noexcept override {
        const MetalBuffer* buffer = buffers_.resolve(handle);
        return buffer != nullptr ? &buffer->desc : nullptr;
    }
    Expected<TextureHandle, Error> create_texture(const TextureDescription& desc) override {
        ValidationMessage message;
        if (Status valid = validate_texture(desc, capabilities_, message); !valid) {
            report_validation(ValidationSeverity::Error, message.text);
            return make_unexpected(valid.error());
        }
        const bool memoryless = has_usage(desc.usage, TextureUsage::TransientAttachment);
        if (memoryless && ![device_ supportsFamily:MTLGPUFamilyApple1]) {
            return fail(ErrorCode::Unsupported,
                        "memoryless attachments require an Apple GPU family");
        }
        MTLTextureDescriptor* descriptor = texture_descriptor(desc, memoryless);
        id<MTLTexture> native = [device_ newTextureWithDescriptor:descriptor];
        if (native == nil) {
            return fail(ErrorCode::OutOfMemory, "Metal could not allocate a texture");
        }
        MetalTexture texture;
        texture.name.assign(desc.name);
        texture.desc = desc;
        texture.desc.name = texture.name.text;
        texture.texture = native;
        texture.bytes = static_cast<u64>(native.allocatedSize);
        texture.memoryless = memoryless;
        native.label = [NSString stringWithUTF8String:texture.name.text];
        Expected<TextureHandle, Error> handle = textures_.create(texture);
        if (handle) {
            textures_.resolve(*handle)->desc.name = textures_.resolve(*handle)->name.text;
            if (!memoryless) {
                charge(desc.memory, texture.bytes);
            }
        }
        return handle;
    }
    void destroy_texture(TextureHandle handle) noexcept override {
        MetalTexture* texture = textures_.resolve(handle);
        if (texture == nullptr) {
            report_validation(ValidationSeverity::Error,
                              "destroy_texture() on a stale or never-issued Metal handle");
            return;
        }
        if (!texture->transient && !texture->memoryless) {
            discharge(texture->desc.memory, texture->bytes);
        }
        (void)textures_.destroy(handle);
        ++stats_.resources_freed;
    }
    [[nodiscard]] bool is_valid(TextureHandle handle) const noexcept override {
        return textures_.resolve(handle) != nullptr;
    }
    [[nodiscard]] const TextureDescription* texture_description(
        TextureHandle handle) const noexcept override {
        const MetalTexture* texture = textures_.resolve(handle);
        return texture != nullptr ? &texture->desc : nullptr;
    }
    Expected<TextureViewHandle, Error> create_texture_view(
        const TextureViewDescription& desc) override {
        MetalTexture* source = textures_.resolve(desc.texture);
        if (source == nullptr || source->texture == nil || !source->bound) {
            return fail(ErrorCode::NotFound,
                        "a Metal texture view requires a live, memory-bound texture");
        }
        ValidationMessage message;
        if (Status valid = validate_texture_view(desc, source->desc, message); !valid) {
            report_validation(ValidationSeverity::Error, message.text);
            return make_unexpected(valid.error());
        }
        const SubresourceRange range =
            resolve_range(desc.range, source->desc.mip_levels, source->desc.array_layers);
        const Format format = desc.format == Format::Undefined ? source->desc.format : desc.format;
        const MTLTextureType type = texture_type(TextureDescription{
            .dimension = desc.dimension,
            .array_layers = static_cast<u16>(range.layer_count),
            .sample_count = source->desc.sample_count,
        });
        const bool identity_view =
            format == source->desc.format && type == source->texture.textureType &&
            range.base_mip == 0 && range.mip_count == source->desc.mip_levels &&
            range.base_layer == 0 && range.layer_count == source->desc.array_layers;
        id<MTLTexture> native = source->texture;
        if (!identity_view) {
            native = [source->texture
                newTextureViewWithPixelFormat:static_cast<MTLPixelFormat>(
                                                  metal_pixel_format(format))
                             textureType:type
                                  levels:NSMakeRange(static_cast<NSUInteger>(range.base_mip),
                                                     static_cast<NSUInteger>(range.mip_count))
                                  slices:NSMakeRange(static_cast<NSUInteger>(range.base_layer),
                                                     static_cast<NSUInteger>(range.layer_count))];
        }
        if (native == nil) {
            return fail(ErrorCode::Unsupported, "Metal refused the requested texture view");
        }
        MetalTextureView view;
        view.name.assign(desc.name);
        view.desc = desc;
        view.desc.name = view.name.text;
        view.texture = native;
        native.label = [NSString stringWithUTF8String:view.name.text];
        Expected<TextureViewHandle, Error> handle = views_.create(view);
        if (handle) {
            views_.resolve(*handle)->desc.name = views_.resolve(*handle)->name.text;
        }
        return handle;
    }
    void destroy_texture_view(TextureViewHandle handle) noexcept override {
        if (views_.resolve(handle) == nullptr) {
            report_validation(ValidationSeverity::Error,
                              "destroy_texture_view() on a stale Metal handle");
            return;
        }
        (void)views_.destroy(handle);
        ++stats_.resources_freed;
    }
    [[nodiscard]] bool is_valid(TextureViewHandle handle) const noexcept override {
        return views_.resolve(handle) != nullptr;
    }
    Expected<SamplerHandle, Error> create_sampler(const SamplerDescription& desc) override {
        ValidationMessage message;
        if (Status valid = validate_sampler(desc, capabilities_.limits(), message); !valid) {
            report_validation(ValidationSeverity::Error, message.text);
            return make_unexpected(valid.error());
        }
        MTLSamplerDescriptor* descriptor = [[MTLSamplerDescriptor alloc] init];
        descriptor.minFilter = sampler_filter(desc.min_filter);
        descriptor.magFilter = sampler_filter(desc.mag_filter);
        descriptor.mipFilter = sampler_mip_filter(desc.mipmap_mode);
        descriptor.sAddressMode = sampler_address_mode(desc.address_u);
        descriptor.tAddressMode = sampler_address_mode(desc.address_v);
        descriptor.rAddressMode = sampler_address_mode(desc.address_w);
        descriptor.lodMinClamp = desc.min_lod;
        descriptor.lodMaxClamp = desc.max_lod;
        descriptor.lodAverage = NO;
        descriptor.supportArgumentBuffers = YES;
        descriptor.maxAnisotropy = static_cast<NSUInteger>(desc.max_anisotropy < 1.0F
                                                                ? 1.0F
                                                                : desc.max_anisotropy);
        descriptor.compareFunction =
            desc.compare_enable ? compare_function(desc.compare_op) : MTLCompareFunctionNever;
        descriptor.borderColor = MTLSamplerBorderColorTransparentBlack;
        descriptor.label = [NSString stringWithUTF8String:desc.name];

        id<MTLSamplerState> native = [device_ newSamplerStateWithDescriptor:descriptor];
        if (native == nil) {
            return fail(ErrorCode::OutOfMemory, "Metal could not create a sampler state");
        }
        MetalSampler sampler;
        sampler.name.assign(desc.name);
        sampler.sampler = native;
        return samplers_.create(sampler);
    }
    void destroy_sampler(SamplerHandle handle) noexcept override {
        if (!samplers_.destroy(handle)) {
            report_validation(ValidationSeverity::Error,
                              "destroy_sampler() on a stale Metal handle");
            return;
        }
        ++stats_.resources_freed;
    }
    Expected<QueryPoolHandle, Error> create_query_pool(
        const QueryPoolDescription& desc) override {
        if (desc.count == 0) {
            return fail(ErrorCode::InvalidArgument, "a query pool of zero queries answers nothing");
        }
        if (desc.kind != QueryKind::Timestamp) {
            return fail(ErrorCode::Unsupported,
                        "Metal currently exposes timestamp query pools only");
        }
        id<MTLCounterSet> timestamp_set = nil;
        for (id<MTLCounterSet> counter_set in device_.counterSets) {
            if ([counter_set.name isEqualToString:MTLCommonCounterSetTimestamp]) {
                timestamp_set = counter_set;
                break;
            }
        }
        const bool sampling_supported =
            [device_ supportsCounterSampling:MTLCounterSamplingPointAtStageBoundary] ||
            [device_ supportsCounterSampling:MTLCounterSamplingPointAtDrawBoundary] ||
            [device_ supportsCounterSampling:MTLCounterSamplingPointAtDispatchBoundary] ||
            [device_ supportsCounterSampling:MTLCounterSamplingPointAtBlitBoundary];
        if (timestamp_set == nil || !sampling_supported) {
            return fail(ErrorCode::Unsupported,
                        "this Metal device cannot sample GPU timestamps at command boundaries");
        }
        MTLCounterSampleBufferDescriptor* descriptor =
            [[MTLCounterSampleBufferDescriptor alloc] init];
        descriptor.counterSet = timestamp_set;
        descriptor.label = [NSString stringWithUTF8String:desc.name];
        descriptor.storageMode = MTLStorageModeShared;
        descriptor.sampleCount = desc.count;
        NSError* error = nil;
        id<MTLCounterSampleBuffer> samples =
            [device_ newCounterSampleBufferWithDescriptor:descriptor error:&error];
        if (samples == nil) {
            report_validation(ValidationSeverity::Error,
                              error.localizedDescription.UTF8String);
            return fail(ErrorCode::Unsupported,
                        "Metal could not allocate the timestamp counter sample buffer");
        }
        MetalQueryPool pool;
        pool.kind = desc.kind;
        pool.count = desc.count;
        pool.samples = samples;
        pool.written = [NSMutableIndexSet indexSet];
        return query_pools_.create(pool);
    }
    void destroy_query_pool(QueryPoolHandle handle) noexcept override {
        if (!query_pools_.destroy(handle)) {
            report_validation(ValidationSeverity::Error,
                              "destroy_query_pool() on a stale Metal handle");
        }
    }
    Expected<u32, Error> read_query_results(QueryPoolHandle handle, u32 first, u32 count,
                                            Span<u64> out) override {
        MetalQueryPool* pool = query_pools_.resolve(handle);
        if (pool == nullptr) {
            return fail(ErrorCode::NotFound, "read_query_results(): stale query pool handle");
        }
        if (first > pool->count || count > pool->count - first) {
            return fail(ErrorCode::OutOfRange, "read_query_results(): range outside the pool");
        }
        const u32 written = count < out.size() ? count : static_cast<u32>(out.size());
        for (u32 index = 0; index < written; ++index) {
            if (![pool->written containsIndex:first + index]) {
                return fail(ErrorCode::Unavailable, "the Metal timestamp query has not resolved");
            }
        }
        if (written == 0) {
            return 0U;
        }
        NSData* resolved = [pool->samples resolveCounterRange:NSMakeRange(first, written)];
        if (resolved == nil ||
            resolved.length < static_cast<NSUInteger>(written) * sizeof(MTLCounterResultTimestamp)) {
            return fail(ErrorCode::Unavailable, "Metal could not resolve the timestamp queries");
        }
        const auto* timestamps = static_cast<const MTLCounterResultTimestamp*>(resolved.bytes);
        for (u32 index = 0; index < written; ++index) {
            if (timestamps[index].timestamp == MTLCounterErrorValue) {
                return fail(ErrorCode::Unavailable, "a Metal timestamp query is not ready");
            }
            out[index] = timestamps[index].timestamp;
        }
        return written;
    }
    Expected<TextureHandle, Error> create_transient_texture(
        const TextureDescription& desc) override {
        ValidationMessage message;
        if (Status valid = validate_texture(desc, capabilities_, message); !valid) {
            return make_unexpected(valid.error());
        }
        MetalTexture texture;
        texture.name.assign(desc.name);
        texture.desc = desc;
        texture.desc.name = texture.name.text;
        texture.transient = true;
        texture.bound = false;
        texture.memoryless = has_usage(desc.usage, TextureUsage::TransientAttachment);
        if (texture.memoryless) {
            if (![device_ supportsFamily:MTLGPUFamilyApple1]) {
                return fail(ErrorCode::Unsupported,
                            "memoryless attachments require an Apple GPU family");
            }
            texture.texture = [device_ newTextureWithDescriptor:texture_descriptor(desc, true)];
            if (texture.texture == nil) {
                return fail(ErrorCode::OutOfMemory,
                            "Metal could not allocate a memoryless attachment");
            }
            texture.bound = true;
        }
        Expected<TextureHandle, Error> handle = textures_.create(texture);
        if (!handle) {
            return handle;
        }
        textures_.resolve(*handle)->desc.name = textures_.resolve(*handle)->name.text;
        if (Status kept = transient_textures_.push_back(*handle); !kept) {
            (void)textures_.destroy(*handle);
            return make_unexpected(kept.error());
        }
        return handle;
    }
    Expected<BufferHandle, Error> create_transient_buffer(
        const BufferDescription& desc) override {
        ValidationMessage message;
        if (Status valid = validate_buffer(desc, message); !valid) {
            return make_unexpected(valid.error());
        }
        MetalBuffer buffer;
        buffer.name.assign(desc.name);
        buffer.desc = desc;
        buffer.desc.name = buffer.name.text;
        buffer.bytes = desc.size;
        buffer.transient = true;
        buffer.bound = false;
        Expected<BufferHandle, Error> handle = buffers_.create(buffer);
        if (!handle) {
            return handle;
        }
        buffers_.resolve(*handle)->desc.name = buffers_.resolve(*handle)->name.text;
        if (Status kept = transient_buffers_.push_back(*handle); !kept) {
            (void)buffers_.destroy(*handle);
            return make_unexpected(kept.error());
        }
        return handle;
    }
    [[nodiscard]] Expected<MemoryRequirements, Error> texture_memory_requirements(
        TextureHandle handle) const override {
        const MetalTexture* texture = textures_.resolve(handle);
        if (texture == nullptr || !texture->transient) {
            return fail(ErrorCode::NotFound, "no transient Metal texture for this handle");
        }
        if (texture->memoryless) {
            return MemoryRequirements{.size = 0, .alignment = 1, .pool_class = {~0ULL}};
        }
        const MTLSizeAndAlign requirements =
            [device_ heapTextureSizeAndAlignWithDescriptor:texture_descriptor(texture->desc, false)];
        return MemoryRequirements{.size = static_cast<u64>(requirements.size),
                                  .alignment = static_cast<u64>(requirements.align),
                                  .pool_class = {~0ULL}};
    }
    [[nodiscard]] Expected<MemoryRequirements, Error> buffer_memory_requirements(
        BufferHandle handle) const override {
        const MetalBuffer* buffer = buffers_.resolve(handle);
        if (buffer == nullptr || !buffer->transient) {
            return fail(ErrorCode::NotFound, "no transient Metal buffer for this handle");
        }
        const MTLSizeAndAlign requirements = [device_
            heapBufferSizeAndAlignWithLength:static_cast<NSUInteger>(buffer->desc.size)
                                  options:MTLResourceStorageModePrivate];
        return MemoryRequirements{.size = static_cast<u64>(requirements.size),
                                  .alignment = static_cast<u64>(requirements.align),
                                  .pool_class = {~0ULL}};
    }
    Status reserve_transient_memory(u64 bytes, MemoryPoolClass pool_class) override {
        if (pool_class.empty()) {
            return fail(ErrorCode::InvalidArgument,
                        "the transient Metal heap has no common memory pool class");
        }
        // A graph made only from imported resources needs no heap. Metal rejects a zero-sized
        // MTLHeapDescriptor, so treating zero as an allocation request makes a compute-only graph
        // fail before its first dispatch.
        if (bytes == 0) {
            return ok();
        }
        if (bytes <= transient_bytes_ && transient_heap_ != nil) {
            return ok();
        }
        MTLHeapDescriptor* descriptor = [[MTLHeapDescriptor alloc] init];
        descriptor.type = MTLHeapTypePlacement;
        descriptor.size = static_cast<NSUInteger>(bytes);
        descriptor.storageMode = MTLStorageModePrivate;
        descriptor.hazardTrackingMode = MTLHazardTrackingModeTracked;
        id<MTLHeap> heap = [device_ newHeapWithDescriptor:descriptor];
        if (heap == nil) {
            return fail(ErrorCode::OutOfMemory, "Metal could not reserve the transient heap");
        }
        const u32 category = static_cast<u32>(GpuMemoryCategory::Transient);
        memory_.live_bytes[category] = bytes;
        if (bytes > memory_.peak_bytes[category]) {
            memory_.peak_bytes[category] = bytes;
        }
        transient_heap_ = heap;
        transient_bytes_ = bytes;
        ++memory_.allocation_count;
        return ok();
    }
    Status bind_transient(TextureHandle handle, u64 offset) override {
        MetalTexture* texture = textures_.resolve(handle);
        if (texture == nullptr || !texture->transient) {
            return fail(ErrorCode::NotFound, "no transient Metal texture for this handle");
        }
        if (texture->memoryless) {
            return ok();
        }
        if (transient_heap_ == nil) {
            return fail(ErrorCode::Unavailable, "the transient Metal heap is not reserved");
        }
        texture->texture = [transient_heap_
            newTextureWithDescriptor:texture_descriptor(texture->desc, false)
                            offset:static_cast<NSUInteger>(offset)];
        if (texture->texture == nil) {
            return fail(ErrorCode::OutOfMemory,
                        "Metal refused a transient texture placement");
        }
        texture->texture.label = [NSString stringWithUTF8String:texture->name.text];
        texture->bound = true;
        texture->bytes = static_cast<u64>(texture->texture.allocatedSize);
        return ok();
    }
    Status bind_transient(BufferHandle handle, u64 offset) override {
        MetalBuffer* buffer = buffers_.resolve(handle);
        if (buffer == nullptr || !buffer->transient) {
            return fail(ErrorCode::NotFound, "no transient Metal buffer for this handle");
        }
        if (transient_heap_ == nil) {
            return fail(ErrorCode::Unavailable, "the transient Metal heap is not reserved");
        }
        buffer->buffer = [transient_heap_
            newBufferWithLength:static_cast<NSUInteger>(buffer->desc.size)
                        options:MTLResourceStorageModePrivate
                         offset:static_cast<NSUInteger>(offset)];
        if (buffer->buffer == nil) {
            return fail(ErrorCode::OutOfMemory, "Metal refused a transient buffer placement");
        }
        buffer->buffer.label = [NSString stringWithUTF8String:buffer->name.text];
        buffer->bound = true;
        return ok();
    }
    void release_transient_resources() noexcept override {
        for (TextureHandle handle : transient_textures_) {
            (void)textures_.destroy(handle);
        }
        for (BufferHandle handle : transient_buffers_) {
            (void)buffers_.destroy(handle);
        }
        transient_textures_.clear();
        transient_buffers_.clear();
    }
    [[nodiscard]] u64 transient_pool_bytes() const noexcept override { return transient_bytes_; }
    Expected<ShaderModuleHandle, Error> create_shader_module(
        const ShaderModuleDescription& desc) override {
        ValidationMessage message;
        if (Status valid = validate_shader_module(desc, capabilities_, message); !valid) {
            report_validation(ValidationSeverity::Error, message.text);
            return make_unexpected(valid.error());
        }
        if (!desc.spirv.empty()) {
            return fail(ErrorCode::Unsupported,
                        "Metal requires MSL produced by the offline shader pipeline");
        }

        NSString* source = [[NSString alloc] initWithBytes:desc.native.data()
                                                    length:desc.native.size()
                                                  encoding:NSUTF8StringEncoding];
        if (source == nil) {
            return fail(ErrorCode::InvalidArgument, "Metal shader source is not valid UTF-8 MSL");
        }
        MTLCompileOptions* options = [[MTLCompileOptions alloc] init];
        NSError* compile_error = nil;
        id<MTLLibrary> library = [device_ newLibraryWithSource:source
                                                      options:options
                                                        error:&compile_error];
        if (library == nil) {
            const char* diagnostic = compile_error.localizedDescription.UTF8String;
            report_validation(ValidationSeverity::Error,
                              diagnostic != nullptr ? diagnostic : "Metal rejected the MSL module");
            return fail(ErrorCode::InvalidArgument, "Metal rejected the MSL shader module");
        }
        NSString* entry = [NSString stringWithUTF8String:desc.entry_point];
        if ([library newFunctionWithName:entry] == nil) {
            return fail(ErrorCode::NotFound,
                        "the requested entry point is absent from the Metal shader module");
        }
        library.label = [NSString stringWithUTF8String:desc.name];
        MetalShaderModule module;
        module.name.assign(desc.name);
        module.entry_point.assign(desc.entry_point);
        module.stage = desc.stage;
        module.library = library;
        return shaders_.create(module);
    }
    void destroy_shader_module(ShaderModuleHandle handle) noexcept override {
        if (!shaders_.destroy(handle)) {
            report_validation(ValidationSeverity::Error,
                              "destroy_shader_module() on a stale Metal handle");
            return;
        }
        ++stats_.resources_freed;
    }
    Expected<DescriptorSetLayoutHandle, Error> create_descriptor_set_layout(
        const DescriptorSetLayoutDescription& desc) override {
        if (desc.bindings.size() > kMetalMaxDescriptorBindings) {
            return fail(ErrorCode::OutOfRange,
                        "Metal descriptor set layouts support at most 32 bindings");
        }

        NSMutableArray<MTLArgumentDescriptor*>* arguments =
            [NSMutableArray arrayWithCapacity:desc.bindings.size()];
        MetalDescriptorSetLayout layout;
        layout.name.assign(desc.name);
        u32 next_argument_index = 0;
        for (const DescriptorBinding& binding : desc.bindings) {
            if (binding.partially_bound &&
                !capabilities_.has(Capability::BindlessPartiallyBound)) {
                return fail(ErrorCode::Unsupported,
                            "a partially bound Metal binding requires Tier 2 argument buffers");
            }
            if (binding.kind == DescriptorKind::CombinedTextureSampler) {
                return fail(ErrorCode::Unsupported,
                            "Metal argument buffers require separate texture and sampler bindings");
            }
            for (u32 prior = 0; prior < layout.binding_count; ++prior) {
                if (layout.bindings[prior].binding == binding.binding) {
                    return fail(ErrorCode::InvalidArgument,
                                "Metal descriptor set layout contains a duplicate binding");
                }
            }

            const u32 count = binding.count == 0 ? kMetalBindlessCapacity : binding.count;
            if (next_argument_index > UINT32_MAX - count) {
                return fail(ErrorCode::OutOfRange,
                            "Metal descriptor array is too large to encode");
            }
            MTLArgumentDescriptor* argument = [MTLArgumentDescriptor argumentDescriptor];
            argument.index = static_cast<NSUInteger>(next_argument_index);
            argument.arrayLength = count == 1 ? 0 : static_cast<NSUInteger>(count);
            argument.access = binding.kind == DescriptorKind::StorageBuffer ||
                                      binding.kind == DescriptorKind::StorageTexture
                                  ? MTLBindingAccessReadWrite
                                  : MTLBindingAccessReadOnly;
            switch (binding.kind) {
                case DescriptorKind::UniformBuffer:
                case DescriptorKind::StorageBuffer:
                    argument.dataType = MTLDataTypePointer;
                    break;
                case DescriptorKind::SampledTexture:
                case DescriptorKind::StorageTexture:
                case DescriptorKind::InputAttachment:
                    argument.dataType = MTLDataTypeTexture;
                    // The RHI layout currently carries no texture dimension. The standard
                    // material table is Texture2D, and non-2D writes are rejected below.
                    argument.textureType = MTLTextureType2D;
                    break;
                case DescriptorKind::Sampler:
                    argument.dataType = MTLDataTypeSampler;
                    break;
                case DescriptorKind::CombinedTextureSampler:
                    break;
            }
            [arguments addObject:argument];
            layout.bindings[layout.binding_count++] = MetalDescriptorBinding{
                .binding = binding.binding,
                .base_index = next_argument_index,
                .count = count,
                .kind = binding.kind,
            };
            next_argument_index += count;
        }
        layout.arguments = [arguments copy];
        return descriptor_set_layouts_.create(layout);
    }
    void destroy_descriptor_set_layout(DescriptorSetLayoutHandle handle) noexcept override {
        if (!handle.is_null() && handle == bindless_layout_handle_) {
            report_validation(ValidationSeverity::Error,
                              "destroy_descriptor_set_layout(): the global texture table's layout "
                              "belongs to the Metal device");
            return;
        }
        if (!descriptor_set_layouts_.destroy(handle)) {
            report_validation(ValidationSeverity::Error,
                              "destroy_descriptor_set_layout() on a stale Metal handle");
        }
    }
    Expected<PipelineLayoutHandle, Error> create_pipeline_layout(
        const PipelineLayoutDescription& desc) override {
        ValidationMessage message;
        if (Status valid = validate_pipeline_layout(desc, message); !valid) {
            report_validation(ValidationSeverity::Error, message.text);
            return make_unexpected(valid.error());
        }
        MetalPipelineLayout layout;
        layout.name.assign(desc.name);
        layout.set_count = static_cast<u32>(desc.set_layouts.size());
        for (u32 index = 0; index < layout.set_count; ++index) {
            if (descriptor_set_layouts_.resolve(desc.set_layouts[index]) == nullptr) {
                return fail(ErrorCode::NotFound,
                            "Metal pipeline layout names a stale descriptor set layout");
            }
            layout.set_layouts[index] = desc.set_layouts[index];
        }
        return pipeline_layouts_.create(layout);
    }
    void destroy_pipeline_layout(PipelineLayoutHandle handle) noexcept override {
        if (!pipeline_layouts_.destroy(handle)) {
            report_validation(ValidationSeverity::Error,
                              "destroy_pipeline_layout() on a stale Metal handle");
        }
    }
    Expected<DescriptorSetHandle, Error> allocate_descriptor_set(DescriptorSetLayoutHandle handle,
                                                                 bool per_frame) override {
        const MetalDescriptorSetLayout* layout = descriptor_set_layouts_.resolve(handle);
        if (layout == nullptr) {
            return fail(ErrorCode::NotFound, "allocate_descriptor_set(): stale layout handle");
        }
        id<MTLArgumentEncoder> encoder =
            [device_ newArgumentEncoderWithArguments:layout->arguments];
        if (encoder == nil) {
            return fail(ErrorCode::Unsupported,
                        "Metal could not create an argument encoder for the descriptor layout");
        }
        id<MTLBuffer> buffer =
            [device_ newBufferWithLength:encoder.encodedLength options:MTLResourceStorageModeShared];
        if (buffer == nil) {
            return fail(ErrorCode::OutOfMemory,
                        "Metal could not allocate the descriptor argument buffer");
        }
        [encoder setArgumentBuffer:buffer offset:0];
        MetalDescriptorSet set;
        set.encoder = encoder;
        set.argument_buffer = buffer;
        set.resources = [NSMutableDictionary dictionary];
        set.layout = handle;
        set.per_frame = per_frame;
        Expected<DescriptorSetHandle, Error> created = descriptor_sets_.create(set);
        if (created && per_frame) {
            if (Status tracked = per_frame_descriptor_sets_[frame_slot_].push_back(*created);
                !tracked) {
                (void)descriptor_sets_.destroy(*created);
                return make_unexpected(tracked.error());
            }
        }
        return created;
    }
    Status update_descriptor_set(DescriptorSetHandle handle,
                                 Span<const DescriptorWrite> writes) override {
        MetalDescriptorSet* set = descriptor_sets_.resolve(handle);
        if (set == nullptr) {
            return fail(ErrorCode::NotFound, "update_descriptor_set(): stale set handle");
        }
        const MetalDescriptorSetLayout* layout = descriptor_set_layouts_.resolve(set->layout);
        if (layout == nullptr) {
            return fail(ErrorCode::NotFound,
                        "update_descriptor_set(): descriptor layout is no longer live");
        }
        for (const DescriptorWrite& write : writes) {
            const MetalDescriptorBinding* binding = nullptr;
            for (u32 index = 0; index < layout->binding_count; ++index) {
                if (layout->bindings[index].binding == write.binding) {
                    binding = &layout->bindings[index];
                    break;
                }
            }
            if (binding == nullptr || binding->kind != write.kind) {
                return fail(ErrorCode::InvalidArgument,
                            "Metal descriptor write does not match its set layout");
            }
            if (write.array_index >= binding->count) {
                return fail(ErrorCode::OutOfRange,
                            "Metal descriptor write array index exceeds its binding count");
            }
            const NSUInteger argument_index = binding->base_index + write.array_index;
            NSNumber* key = @(argument_index);
            switch (write.kind) {
                case DescriptorKind::UniformBuffer:
                case DescriptorKind::StorageBuffer: {
                    MetalBuffer* buffer = buffers_.resolve(write.buffer);
                    if (buffer == nullptr || buffer->buffer == nil) {
                        return fail(ErrorCode::NotFound,
                                    "Metal descriptor write names a stale buffer handle");
                    }
                    if (write.buffer_offset >= buffer->bytes) {
                        return fail(ErrorCode::InvalidArgument,
                                    "Metal descriptor buffer offset is outside the buffer");
                    }
                    [set->encoder setBuffer:buffer->buffer
                                     offset:static_cast<NSUInteger>(write.buffer_offset)
                                    atIndex:argument_index];
                    set->resources[key] = buffer->buffer;
                    break;
                }
                case DescriptorKind::SampledTexture:
                case DescriptorKind::StorageTexture:
                case DescriptorKind::InputAttachment: {
                    MetalTextureView* view = views_.resolve(write.texture_view);
                    if (view == nullptr || view->texture == nil) {
                        return fail(ErrorCode::NotFound,
                                    "Metal descriptor write names a stale texture view handle");
                    }
                    if (view->desc.dimension != TextureDimension::Texture2D) {
                        return fail(ErrorCode::Unsupported,
                                    "Metal descriptor layouts currently encode Texture2D only");
                    }
                    [set->encoder setTexture:view->texture atIndex:argument_index];
                    set->resources[key] = view->texture;
                    break;
                }
                case DescriptorKind::Sampler: {
                    MetalSampler* sampler = samplers_.resolve(write.sampler);
                    if (sampler == nullptr || sampler->sampler == nil) {
                        return fail(ErrorCode::NotFound,
                                    "Metal descriptor write names a stale sampler handle");
                    }
                    [set->encoder setSamplerState:sampler->sampler atIndex:argument_index];
                    break;
                }
                case DescriptorKind::CombinedTextureSampler:
                    return fail(ErrorCode::Unsupported,
                                "Metal requires separate texture and sampler descriptor writes");
            }
        }
        return ok();
    }
    BindlessIndex bind_texture_globally(TextureViewHandle view,
                                        SamplerHandle sampler) noexcept override {
        if (!capabilities_.has(Capability::Bindless) || bindless_set_handle_.is_null()) {
            return kInvalidBindlessIndex;
        }
        if (views_.resolve(view) == nullptr) {
            report_validation(ValidationSeverity::Error,
                              "bind_texture_globally(): stale texture view handle");
            return kInvalidBindlessIndex;
        }
        if (!sampler.is_null() && !set_global_sampler(sampler)) {
            return kInvalidBindlessIndex;
        }
        u32 index = bindless_next_hint_;
        while (index < kMetalBindlessCapacity && bindless_used_[index]) {
            ++index;
        }
        if (index == kMetalBindlessCapacity) {
            index = 0;
            while (index < bindless_next_hint_ && bindless_used_[index]) {
                ++index;
            }
        }
        if (index == kMetalBindlessCapacity || bindless_used_[index]) {
            report_validation(ValidationSeverity::Error,
                              "bind_texture_globally(): the Metal global table is full");
            return kInvalidBindlessIndex;
        }
        const DescriptorWrite write{
            .binding = kGlobalTableTextureBinding,
            .array_index = index,
            .kind = DescriptorKind::SampledTexture,
            .texture_view = view,
        };
        if (Status updated = update_descriptor_set(bindless_set_handle_, {&write, 1}); !updated) {
            report_validation(ValidationSeverity::Error, updated.error().message);
            return kInvalidBindlessIndex;
        }
        bindless_used_[index] = true;
        bindless_next_hint_ = index + 1;
        return index;
    }
    void release_bindless_index(BindlessIndex index) noexcept override {
        if (index == kInvalidBindlessIndex || index >= kMetalBindlessCapacity ||
            !bindless_used_[index]) {
            return;
        }
        MetalDescriptorSet* set = descriptor_sets_.resolve(bindless_set_handle_);
        const MetalDescriptorSetLayout* layout =
            descriptor_set_layouts_.resolve(bindless_layout_handle_);
        if (set != nullptr && layout != nullptr) {
            const MetalDescriptorBinding* textures =
                find_binding(*layout, kGlobalTableTextureBinding);
            if (textures != nullptr) {
                const NSUInteger argument_index = textures->base_index + index;
                [set->encoder setTexture:nil atIndex:argument_index];
                [set->resources removeObjectForKey:@(argument_index)];
            }
        }
        bindless_used_[index] = false;
        if (index < bindless_next_hint_) {
            bindless_next_hint_ = index;
        }
    }
    [[nodiscard]] DescriptorSetLayoutHandle global_texture_table_layout() const noexcept override {
        return bindless_layout_handle_;
    }
    [[nodiscard]] DescriptorSetHandle global_texture_table() const noexcept override {
        return bindless_set_handle_;
    }
    Status set_global_sampler(SamplerHandle sampler) noexcept override {
        if (!capabilities_.has(Capability::Bindless) || bindless_set_handle_.is_null()) {
            return fail(ErrorCode::Unsupported,
                        "the Metal compatibility path has no global texture table");
        }
        if (samplers_.resolve(sampler) == nullptr) {
            report_validation(ValidationSeverity::Error,
                              "set_global_sampler(): stale sampler handle");
            return fail(ErrorCode::NotFound, "set_global_sampler(): stale sampler handle");
        }
        if (!bindless_sampler_.is_null() && !(bindless_sampler_ == sampler)) {
            report_validation(ValidationSeverity::Error,
                              "set_global_sampler(): the global table already has a different "
                              "sampler");
            return fail(ErrorCode::InvalidArgument,
                        "the global texture table already has a different sampler");
        }
        const DescriptorWrite write{
            .binding = kGlobalTableSamplerBinding,
            .kind = DescriptorKind::Sampler,
            .sampler = sampler,
        };
        if (Status updated = update_descriptor_set(bindless_set_handle_, {&write, 1}); !updated) {
            return updated;
        }
        bindless_sampler_ = sampler;
        return ok();
    }
    Expected<GraphicsPipelineHandle, Error> create_graphics_pipeline(
        const GraphicsPipelineDescription& desc) override {
        ValidationMessage message;
        if (Status valid = validate_graphics_pipeline(desc, message); !valid) {
            report_validation(ValidationSeverity::Error, message.text);
            return make_unexpected(valid.error());
        }
        if (pipeline_layouts_.resolve(desc.layout) == nullptr) {
            return fail(ErrorCode::NotFound, "graphics pipeline: stale pipeline-layout handle");
        }
        const MetalShaderModule* vertex = shaders_.resolve(desc.vertex_shader);
        const MetalShaderModule* fragment = shaders_.resolve(desc.fragment_shader);
        if (vertex == nullptr || vertex->stage != ShaderStage::Vertex) {
            return fail(ErrorCode::NotFound,
                        "graphics pipeline: stale or non-vertex vertex-shader handle");
        }
        if (!desc.fragment_shader.is_null() &&
            (fragment == nullptr || fragment->stage != ShaderStage::Fragment)) {
            return fail(ErrorCode::NotFound,
                        "graphics pipeline: stale or non-fragment fragment-shader handle");
        }
        if (desc.view_mask != 0) {
            return fail(ErrorCode::Unsupported,
                        "Metal multiview pipeline creation is not implemented yet");
        }
        if (desc.rasterisation.polygon_mode == PolygonMode::Point) {
            return fail(ErrorCode::Unsupported,
                        "Metal does not provide point polygon fill for triangle pipelines");
        }

        MTLFunctionConstantValues* constants = [[MTLFunctionConstantValues alloc] init];
        for (const SpecializationConstant& constant : desc.specialization) {
            [constants setConstantValue:&constant.value
                                   type:MTLDataTypeUInt
                                atIndex:constant.id];
        }
        NSError* function_error = nil;
        id<MTLFunction> vertex_function = [vertex->library
            newFunctionWithName:[NSString stringWithUTF8String:vertex->entry_point.text]
                 constantValues:constants
                          error:&function_error];
        if (vertex_function == nil) {
            report_validation(ValidationSeverity::Error,
                              function_error.localizedDescription.UTF8String);
            return fail(ErrorCode::InvalidArgument,
                        "Metal could not specialize the vertex function");
        }
        id<MTLFunction> fragment_function = nil;
        if (fragment != nullptr) {
            function_error = nil;
            fragment_function = [fragment->library
                newFunctionWithName:[NSString stringWithUTF8String:fragment->entry_point.text]
                     constantValues:constants
                              error:&function_error];
            if (fragment_function == nil) {
                report_validation(ValidationSeverity::Error,
                                  function_error.localizedDescription.UTF8String);
                return fail(ErrorCode::InvalidArgument,
                            "Metal could not specialize the fragment function");
            }
        }

        MTLRenderPipelineDescriptor* descriptor = [[MTLRenderPipelineDescriptor alloc] init];
        descriptor.label = [NSString stringWithUTF8String:desc.name];
        descriptor.vertexFunction = vertex_function;
        descriptor.fragmentFunction = fragment_function;
        descriptor.inputPrimitiveTopology = topology_class(desc.topology);
        descriptor.rasterSampleCount = desc.sample_count;

        if (!desc.vertex_attributes.empty()) {
            MTLVertexDescriptor* vertices = [[MTLVertexDescriptor alloc] init];
            for (const VertexBinding& binding : desc.vertex_bindings) {
                if (kMetalVertexBufferBase + binding.binding >= 31) {
                    return fail(ErrorCode::OutOfRange,
                                "Metal vertex-buffer binding exceeds the native stream range");
                }
                const NSUInteger metal_binding = kMetalVertexBufferBase + binding.binding;
                vertices.layouts[metal_binding].stride = binding.stride;
                vertices.layouts[metal_binding].stepFunction =
                    binding.input_rate == VertexInputRate::PerInstance
                        ? MTLVertexStepFunctionPerInstance
                        : MTLVertexStepFunctionPerVertex;
                vertices.layouts[metal_binding].stepRate = 1;
            }
            for (const VertexAttribute& attribute : desc.vertex_attributes) {
                if (attribute.location >= 31 ||
                    kMetalVertexBufferBase + attribute.binding >= 31) {
                    return fail(ErrorCode::OutOfRange,
                                "Metal vertex attribute or stream binding is outside its range");
                }
                const MTLVertexFormat format = vertex_format(attribute.format);
                if (format == MTLVertexFormatInvalid) {
                    return fail(ErrorCode::Unsupported,
                                "Metal does not support this format as a vertex attribute");
                }
                vertices.attributes[attribute.location].format = format;
                vertices.attributes[attribute.location].offset = attribute.offset;
                vertices.attributes[attribute.location].bufferIndex =
                    kMetalVertexBufferBase + attribute.binding;
            }
            descriptor.vertexDescriptor = vertices;
        }

        for (usize index = 0; index < desc.color_attachments.size(); ++index) {
            const ColorAttachmentState& attachment = desc.color_attachments[index];
            MTLRenderPipelineColorAttachmentDescriptor* color =
                descriptor.colorAttachments[index];
            color.pixelFormat =
                static_cast<MTLPixelFormat>(metal_pixel_format(attachment.format));
            color.blendingEnabled = attachment.blend_enable;
            color.sourceRGBBlendFactor = blend_factor(attachment.source_color);
            color.destinationRGBBlendFactor = blend_factor(attachment.destination_color);
            color.rgbBlendOperation = blend_operation(attachment.color_op);
            color.sourceAlphaBlendFactor = blend_factor(attachment.source_alpha);
            color.destinationAlphaBlendFactor = blend_factor(attachment.destination_alpha);
            color.alphaBlendOperation = blend_operation(attachment.alpha_op);
            color.writeMask = color_write_mask(attachment.write_mask);
        }
        if (desc.depth_stencil.format != Format::Undefined) {
            const MTLPixelFormat depth_format =
                static_cast<MTLPixelFormat>(metal_pixel_format(desc.depth_stencil.format));
            descriptor.depthAttachmentPixelFormat = depth_format;
            if (format_info(desc.depth_stencil.format).has_stencil) {
                descriptor.stencilAttachmentPixelFormat = depth_format;
            }
        }

        NSError* pipeline_error = nil;
        if (Status archive = ensure_binary_archive(); !archive) {
            return make_unexpected(archive.error());
        }
        descriptor.binaryArchives = @[ binary_archive_ ];
        id<MTLRenderPipelineState> pipeline = nil;
        bool cache_hit = false;
        if (binary_archive_loaded_) {
            pipeline = [device_ newRenderPipelineStateWithDescriptor:descriptor
                                                              options:MTLPipelineOptionFailOnBinaryArchiveMiss
                                                           reflection:nil
                                                                error:&pipeline_error];
            cache_hit = pipeline != nil;
        }
        if (pipeline == nil) {
            pipeline_error = nil;
            if (![binary_archive_ addRenderPipelineFunctionsWithDescriptor:descriptor
                                                                       error:&pipeline_error]) {
                report_validation(ValidationSeverity::Error,
                                  pipeline_error.localizedDescription.UTF8String);
                return fail(ErrorCode::InvalidArgument,
                            "Metal could not add the graphics pipeline to its binary archive");
            }
            pipeline_error = nil;
            pipeline = [device_ newRenderPipelineStateWithDescriptor:descriptor
                                                               error:&pipeline_error];
        }
        if (pipeline == nil) {
            report_validation(ValidationSeverity::Error,
                              pipeline_error.localizedDescription.UTF8String);
            return fail(ErrorCode::InvalidArgument,
                        "Metal rejected the graphics pipeline state");
        }
        MTLDepthStencilDescriptor* depth = [[MTLDepthStencilDescriptor alloc] init];
        depth.depthCompareFunction = desc.depth_stencil.depth_test_enable
                                         ? compare_function(desc.depth_stencil.depth_compare)
                                         : MTLCompareFunctionAlways;
        depth.depthWriteEnabled = desc.depth_stencil.depth_write_enable;
        depth.label = [NSString stringWithUTF8String:desc.name];
        id<MTLDepthStencilState> depth_state = [device_ newDepthStencilStateWithDescriptor:depth];
        if (depth_state == nil) {
            return fail(ErrorCode::OutOfMemory,
                        "Metal could not create the depth-stencil state");
        }

        MetalGraphicsPipeline stored;
        stored.name.assign(desc.name);
        stored.pipeline = pipeline;
        stored.depth_stencil = depth_state;
        stored.topology = desc.topology;
        stored.rasterisation = desc.rasterisation;
        if (cache_hit) {
            ++stats_.pipeline_cache_hits;
        } else {
            ++stats_.pipeline_cache_misses;
        }
        return graphics_pipelines_.create(stored);
    }
    void destroy_graphics_pipeline(GraphicsPipelineHandle handle) noexcept override {
        if (!graphics_pipelines_.destroy(handle)) {
            report_validation(ValidationSeverity::Error,
                              "destroy_graphics_pipeline() on a stale Metal handle");
        }
    }
    Expected<ComputePipelineHandle, Error> create_compute_pipeline(
        const ComputePipelineDescription& desc) override {
        if (pipeline_layouts_.resolve(desc.layout) == nullptr) {
            return fail(ErrorCode::NotFound, "compute pipeline: stale pipeline-layout handle");
        }
        const MetalShaderModule* module = shaders_.resolve(desc.shader);
        if (module == nullptr || module->stage != ShaderStage::Compute) {
            return fail(ErrorCode::NotFound,
                        "compute pipeline: stale or non-compute shader handle");
        }
        MTLFunctionConstantValues* constants = [[MTLFunctionConstantValues alloc] init];
        for (const SpecializationConstant& constant : desc.specialization) {
            [constants setConstantValue:&constant.value
                                   type:MTLDataTypeUInt
                                atIndex:constant.id];
        }
        NSError* function_error = nil;
        id<MTLFunction> function = [module->library
            newFunctionWithName:[NSString stringWithUTF8String:module->entry_point.text]
                 constantValues:constants
                          error:&function_error];
        if (function == nil) {
            report_validation(ValidationSeverity::Error,
                              function_error.localizedDescription.UTF8String);
            return fail(ErrorCode::InvalidArgument,
                        "Metal could not specialize the compute function");
        }
        MTLComputePipelineDescriptor* descriptor = [[MTLComputePipelineDescriptor alloc] init];
        descriptor.label = [NSString stringWithUTF8String:desc.name];
        descriptor.computeFunction = function;
        if (Status archive = ensure_binary_archive(); !archive) {
            return make_unexpected(archive.error());
        }
        descriptor.binaryArchives = @[ binary_archive_ ];
        NSError* pipeline_error = nil;
        id<MTLComputePipelineState> pipeline = nil;
        bool cache_hit = false;
        if (binary_archive_loaded_) {
            pipeline = [device_ newComputePipelineStateWithDescriptor:descriptor
                                                               options:MTLPipelineOptionFailOnBinaryArchiveMiss
                                                            reflection:nil
                                                                 error:&pipeline_error];
            cache_hit = pipeline != nil;
        }
        if (pipeline == nil) {
            pipeline_error = nil;
            if (![binary_archive_ addComputePipelineFunctionsWithDescriptor:descriptor
                                                                        error:&pipeline_error]) {
                report_validation(ValidationSeverity::Error,
                                  pipeline_error.localizedDescription.UTF8String);
                return fail(ErrorCode::InvalidArgument,
                            "Metal could not add the compute pipeline to its binary archive");
            }
            pipeline_error = nil;
            pipeline = [device_ newComputePipelineStateWithDescriptor:descriptor
                                                                options:MTLPipelineOptionNone
                                                             reflection:nil
                                                                  error:&pipeline_error];
        }
        if (pipeline == nil) {
            report_validation(ValidationSeverity::Error,
                              pipeline_error.localizedDescription.UTF8String);
            return fail(ErrorCode::InvalidArgument,
                        "Metal rejected the compute pipeline state");
        }
        MetalComputePipeline stored;
        stored.name.assign(desc.name);
        stored.pipeline = pipeline;
        const u64 thread_count = static_cast<u64>(desc.workgroup_size[0]) *
                                 desc.workgroup_size[1] * desc.workgroup_size[2];
        if (desc.workgroup_size[0] == 0 || desc.workgroup_size[1] == 0 ||
            desc.workgroup_size[2] == 0 ||
            desc.workgroup_size[0] > capabilities_.limits().max_compute_workgroup_size[0] ||
            desc.workgroup_size[1] > capabilities_.limits().max_compute_workgroup_size[1] ||
            desc.workgroup_size[2] > capabilities_.limits().max_compute_workgroup_size[2] ||
            thread_count > pipeline.maxTotalThreadsPerThreadgroup) {
            return fail(ErrorCode::OutOfRange,
                        "Metal compute workgroup size exceeds the pipeline or device limit");
        }
        stored.threads_per_threadgroup = MTLSizeMake(desc.workgroup_size[0],
                                                     desc.workgroup_size[1],
                                                     desc.workgroup_size[2]);
        if (cache_hit) {
            ++stats_.pipeline_cache_hits;
        } else {
            ++stats_.pipeline_cache_misses;
        }
        return compute_pipelines_.create(stored);
    }
    void destroy_compute_pipeline(ComputePipelineHandle handle) noexcept override {
        if (!compute_pipelines_.destroy(handle)) {
            report_validation(ValidationSeverity::Error,
                              "destroy_compute_pipeline() on a stale Metal handle");
        }
    }
    Status save_pipeline_cache(const char* path) override {
        if (path == nullptr || path[0] == '\0') {
            return fail(ErrorCode::InvalidArgument, "save_pipeline_cache(): no path");
        }
        if (Status archive = ensure_binary_archive(); !archive) {
            return archive;
        }
        NSError* error = nil;
        NSURL* url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:path]];
        if (![binary_archive_ serializeToURL:url error:&error]) {
            report_validation(ValidationSeverity::Error, error.localizedDescription.UTF8String);
            return fail(ErrorCode::Io, "Metal could not serialize its binary archive");
        }
        return ok();
    }
    Status load_pipeline_cache(const char* path) override {
        if (path == nullptr || path[0] == '\0') {
            return fail(ErrorCode::InvalidArgument, "load_pipeline_cache(): no path");
        }
        NSString* file_path = [NSString stringWithUTF8String:path];
        if (![[NSFileManager defaultManager] fileExistsAtPath:file_path]) {
            binary_archive_ = nil;
            binary_archive_loaded_ = false;
            return ok();
        }
        MTLBinaryArchiveDescriptor* descriptor = [[MTLBinaryArchiveDescriptor alloc] init];
        descriptor.url = [NSURL fileURLWithPath:file_path];
        NSError* error = nil;
        id<MTLBinaryArchive> archive =
            [device_ newBinaryArchiveWithDescriptor:descriptor error:&error];
        if (archive == nil) {
            report_validation(ValidationSeverity::Error, error.localizedDescription.UTF8String);
            return fail(ErrorCode::Io, "Metal could not load the binary archive");
        }
        binary_archive_ = archive;
        binary_archive_loaded_ = true;
        return ok();
    }

    Expected<u32, Error> begin_frame() override {
        frame_slot_ = static_cast<u32>(frame_index_ % frames_in_flight_);
        const u64 completion = frame_completion_values_[frame_slot_];
        if (completion != 0 && timeline_event_.signaledValue < completion) {
            if (![timeline_event_ waitUntilSignaledValue:completion timeoutMS:~0ULL]) {
                return fail(ErrorCode::Timeout,
                            "Metal timed out waiting to recycle an in-flight frame");
            }
        }
        for (CommandBufferHandle handle : per_frame_command_buffers_[frame_slot_]) {
            (void)command_buffers_.destroy(handle);
        }
        per_frame_command_buffers_[frame_slot_].clear();
        for (DescriptorSetHandle handle : per_frame_descriptor_sets_[frame_slot_]) {
            (void)descriptor_sets_.destroy(handle);
        }
        per_frame_descriptor_sets_[frame_slot_].clear();
        ++stats_.frames_begun;
        return frame_slot_;
    }
    Status end_frame() override {
        frame_completion_values_[frame_slot_] = timeline_;
        ++frame_index_;
        ++stats_.frames_completed;
        return ok();
    }
    [[nodiscard]] u64 frame_index() const noexcept override { return frame_index_; }
    [[nodiscard]] u32 frame_slot() const noexcept override { return frame_slot_; }
    Expected<CommandBufferHandle, Error> acquire_command_buffer(QueueKind queue,
                                                                 bool secondary) override {
        if (queue != QueueKind::Graphics) {
            return fail(ErrorCode::InvalidArgument,
                        "Metal exposes only the graphics command queue on this device");
        }
        if (secondary) {
            return fail(ErrorCode::Unsupported,
                        "Metal records passes sequentially; secondary command buffers are unsupported");
        }
        Expected<CommandBufferHandle, Error> handle = command_buffers_.create(
            queue_, buffers_, textures_, views_, descriptor_sets_, pipeline_layouts_, query_pools_,
            breadcrumb_buffer_, graphics_pipelines_, compute_pipelines_);
        if (handle) {
            command_buffers_.resolve(*handle)->set_handle(*handle);
            if (Status tracked = per_frame_command_buffers_[frame_slot_].push_back(*handle);
                !tracked) {
                (void)command_buffers_.destroy(*handle);
                return make_unexpected(tracked.error());
            }
            ++stats_.command_buffers_recorded;
        }
        return handle;
    }
    [[nodiscard]] CommandBuffer* command_buffer(CommandBufferHandle handle) noexcept override {
        return command_buffers_.resolve(handle);
    }
    Status begin_command_buffer(CommandBufferHandle handle) override {
        MetalCommandBuffer* commands = command_buffers_.resolve(handle);
        return commands != nullptr
                   ? commands->begin()
                   : fail(ErrorCode::NotFound, "begin_command_buffer(): stale handle");
    }
    Status end_command_buffer(CommandBufferHandle handle) override {
        MetalCommandBuffer* commands = command_buffers_.resolve(handle);
        return commands != nullptr
                   ? commands->end()
                   : fail(ErrorCode::NotFound, "end_command_buffer(): stale handle");
    }
    Status execute_secondary(CommandBufferHandle, Span<const CommandBufferHandle>) override {
            return fail(ErrorCode::Unsupported,
                        "Metal records passes sequentially; secondary command buffers are unsupported");
    }
    Expected<u64, Error> submit(const SubmitInfo& info) override {
        if (info.queue != QueueKind::Graphics) {
            return fail(ErrorCode::InvalidArgument, "submit(): Metal queue is not available");
        }
        for (const TimelineWait& wait : info.waits) {
            if (wait.queue != QueueKind::Graphics) {
                return fail(ErrorCode::InvalidArgument,
                            "submit(): Metal wait names an unavailable queue");
            }
        }
        MetalSemaphore* wait_binary = nullptr;
        if (!info.wait_binary.is_null()) {
            wait_binary = semaphores_.resolve(info.wait_binary);
            if (wait_binary == nullptr) {
                return fail(ErrorCode::NotFound, "submit(): stale wait-semaphore handle");
            }
        }
        MetalSemaphore* signal_binary = nullptr;
        if (!info.signal_binary.is_null()) {
            signal_binary = semaphores_.resolve(info.signal_binary);
            if (signal_binary == nullptr) {
                return fail(ErrorCode::NotFound, "submit(): stale signal-semaphore handle");
            }
        }
        MetalFence* signal_fence = nullptr;
        if (!info.signal_fence.is_null()) {
            signal_fence = fences_.resolve(info.signal_fence);
            if (signal_fence == nullptr) {
                return fail(ErrorCode::NotFound, "submit(): stale fence handle");
            }
        }

        const u64 signal_value = timeline_ + 1;
        id<MTLCommandBuffer> last = nil;
        for (usize index = 0; index < info.command_buffers.size(); ++index) {
            CommandBufferHandle handle = info.command_buffers[index];
            MetalCommandBuffer* commands = command_buffers_.resolve(handle);
            if (commands == nullptr) {
                return fail(ErrorCode::NotFound, "submit(): stale command-buffer handle");
            }
            if (commands->recording() || commands->submitted() || commands->raw() == nil) {
                return fail(ErrorCode::InvalidArgument,
                            "submit(): command buffer is recording or was already submitted");
            }
            if (index == 0) {
                for (const TimelineWait& wait : info.waits) {
                    [commands->raw() encodeWaitForEvent:timeline_event_ value:wait.value];
                    ++stats_.semaphore_waits;
                }
                if (wait_binary != nullptr) {
                    [commands->raw() encodeWaitForEvent:wait_binary->event
                                                  value:wait_binary->next_wait++];
                    ++stats_.semaphore_waits;
                }
            }
            last = commands->raw();
            commands->mark_submitted();
        }
        if (last == nil) {
            last = [queue_ commandBuffer];
            if (last == nil) {
                return fail(ErrorCode::OutOfMemory,
                            "Metal could not allocate an empty submission command buffer");
            }
            for (const TimelineWait& wait : info.waits) {
                [last encodeWaitForEvent:timeline_event_ value:wait.value];
                ++stats_.semaphore_waits;
            }
            if (wait_binary != nullptr) {
                [last encodeWaitForEvent:wait_binary->event value:wait_binary->next_wait++];
                ++stats_.semaphore_waits;
            }
        }
        if (signal_binary != nullptr) {
            [last encodeSignalEvent:signal_binary->event value:signal_binary->next_signal++];
        }
        if (signal_fence != nullptr) {
            [last encodeSignalEvent:signal_fence->event value:signal_fence->target_value];
        }
        // The queue timeline is the completion publication for the whole submission. Encode it
        // after every binary/fence signal so a CPU wait on this value observes those signals too.
        [last encodeSignalEvent:timeline_event_ value:signal_value];
        for (CommandBufferHandle handle : info.command_buffers) {
            [command_buffers_.resolve(handle)->raw() commit];
        }
        if (info.command_buffers.empty()) {
            [last commit];
        }
        timeline_ = signal_value;
        ++stats_.submissions;
        return signal_value;
    }
    [[nodiscard]] u64 timeline_value(QueueKind) const noexcept override { return timeline_; }
    Status wait_timeline(QueueKind queue, u64 value, u64 timeout_ns) override {
        if (queue != QueueKind::Graphics) {
            return fail(ErrorCode::InvalidArgument,
                        "wait_timeline(): Metal queue is not available");
        }
        const u64 timeout_ms = timeout_ns == 0 ? ~0ULL : (timeout_ns + 999'999ULL) / 1'000'000ULL;
        return [timeline_event_ waitUntilSignaledValue:value timeoutMS:timeout_ms]
                   ? ok()
                   : fail(ErrorCode::Timeout, "Metal timeline wait timed out");
    }
    Status wait_idle() override {
        id<MTLCommandBuffer> command = [queue_ commandBuffer];
        [command commit];
        [command waitUntilCompleted];
        return command.status == MTLCommandBufferStatusCompleted
                   ? ok()
                   : fail(ErrorCode::Unavailable,
                          "Metal failed while waiting for the queue to idle");
    }
    Expected<FenceHandle, Error> create_fence(bool signalled) override {
        MetalFence fence;
        fence.event = [device_ newSharedEvent];
        if (fence.event == nil) {
            return fail(ErrorCode::OutOfMemory, "Metal could not create a fence event");
        }
        fence.event.signaledValue = signalled ? fence.target_value : 0;
        return fences_.create(fence);
    }
    void destroy_fence(FenceHandle handle) noexcept override {
        if (!fences_.destroy(handle)) {
            report_validation(ValidationSeverity::Error,
                              "destroy_fence() on a stale Metal handle");
        }
    }
    Status wait_fence(FenceHandle handle, u64 timeout_ns) override {
        const MetalFence* fence = fences_.resolve(handle);
        if (fence == nullptr) {
            return fail(ErrorCode::NotFound, "wait_fence(): stale handle");
        }
        const u64 timeout_ms = timeout_ns == 0 ? ~0ULL : (timeout_ns + 999'999ULL) / 1'000'000ULL;
        return [fence->event waitUntilSignaledValue:fence->target_value timeoutMS:timeout_ms]
                   ? ok()
                   : fail(ErrorCode::Timeout, "Metal fence wait timed out");
    }
    Status reset_fence(FenceHandle handle) override {
        MetalFence* fence = fences_.resolve(handle);
        if (fence == nullptr) {
            return fail(ErrorCode::NotFound, "reset_fence(): stale handle");
        }
        ++fence->target_value;
        return ok();
    }
    [[nodiscard]] bool fence_signalled(FenceHandle handle) const noexcept override {
        const MetalFence* fence = fences_.resolve(handle);
        return fence != nullptr && fence->event.signaledValue >= fence->target_value;
    }
    Expected<SemaphoreHandle, Error> create_semaphore() override {
        MetalSemaphore semaphore;
        semaphore.event = [device_ newSharedEvent];
        if (semaphore.event == nil) {
            return fail(ErrorCode::OutOfMemory, "Metal could not create a semaphore event");
        }
        return semaphores_.create(semaphore);
    }
    void destroy_semaphore(SemaphoreHandle handle) noexcept override {
        if (!semaphores_.destroy(handle)) {
            report_validation(ValidationSeverity::Error,
                              "destroy_semaphore() on a stale Metal handle");
        }
    }
    Expected<SwapchainHandle, Error> create_swapchain(
        const SwapchainDescription& desc) override {
        if (desc.native_surface == nullptr) {
            return fail(ErrorCode::InvalidArgument,
                        "a Metal swapchain needs the CAMetalLayer from DisplayServer");
        }
        if (desc.extent.width == 0 || desc.extent.height == 0) {
            return fail(ErrorCode::InvalidArgument,
                        "a Metal swapchain needs a non-zero drawable extent");
        }
        CAMetalLayer* layer = (__bridge CAMetalLayer*)desc.native_surface;
        if (![layer isKindOfClass:[CAMetalLayer class]]) {
            return fail(ErrorCode::InvalidArgument,
                        "the Metal native surface is not a CAMetalLayer");
        }
        Format format = desc.preferred_format;
        if (format != Format::Bgra8Unorm && format != Format::Bgra8Srgb &&
            format != Format::Rgba16Sfloat) {
            format = Format::Bgra8Srgb;
        }
        layer.device = device_;
        layer.pixelFormat = static_cast<MTLPixelFormat>(metal_pixel_format(format));
        layer.framebufferOnly = NO;
        layer.drawableSize = CGSizeMake(static_cast<CGFloat>(desc.extent.width),
                                        static_cast<CGFloat>(desc.extent.height));
        layer.maximumDrawableCount = desc.min_image_count >= 3 ? 3 : 2;
#if TARGET_OS_OSX
        layer.displaySyncEnabled = desc.present_mode != PresentMode::Immediate;
#endif
        layer.allowsNextDrawableTimeout = YES;

        MetalTexture texture;
        texture.name.assign(desc.name);
        texture.desc.name = texture.name.text;
        texture.desc.format = format;
        texture.desc.extent = {desc.extent.width, desc.extent.height, 1};
        texture.desc.usage = TextureUsage::ColorAttachment | TextureUsage::TransferSource |
                             TextureUsage::Sampled;
        texture.bound = false;
        Expected<TextureHandle, Error> texture_handle = textures_.create(texture);
        if (!texture_handle) {
            return make_unexpected(texture_handle.error());
        }
        MetalTextureView view;
        view.name.assign(desc.name);
        view.desc.name = view.name.text;
        view.desc.texture = *texture_handle;
        view.desc.dimension = TextureDimension::Texture2D;
        view.desc.format = format;
        Expected<TextureViewHandle, Error> view_handle = views_.create(view);
        if (!view_handle) {
            (void)textures_.destroy(*texture_handle);
            return make_unexpected(view_handle.error());
        }
        MetalSwapchain swapchain;
        swapchain.layer = layer;
        swapchain.texture = *texture_handle;
        swapchain.view = *view_handle;
        swapchain.info.format = format;
        swapchain.info.present_mode = desc.present_mode == PresentMode::Immediate
                                          ? PresentMode::Immediate
                                          : PresentMode::Fifo;
        swapchain.info.extent = desc.extent;
        // CAMetalLayer owns its drawable queue and exposes only the currently acquired drawable.
        swapchain.info.image_count = 1;
        Expected<SwapchainHandle, Error> handle = swapchains_.create(swapchain);
        if (!handle) {
            (void)views_.destroy(*view_handle);
            (void)textures_.destroy(*texture_handle);
        }
        return handle;
    }
    void destroy_swapchain(SwapchainHandle handle) noexcept override {
        MetalSwapchain* swapchain = swapchains_.resolve(handle);
        if (swapchain == nullptr) {
            report_validation(ValidationSeverity::Error,
                              "destroy_swapchain() on a stale Metal handle");
            return;
        }
        (void)views_.destroy(swapchain->view);
        (void)textures_.destroy(swapchain->texture);
        (void)swapchains_.destroy(handle);
    }
    Status resize_swapchain(SwapchainHandle handle, Extent2D extent) override {
        MetalSwapchain* swapchain = swapchains_.resolve(handle);
        if (swapchain == nullptr) {
            return fail(ErrorCode::NotFound, "resize_swapchain(): stale handle");
        }
        if (extent.width == 0 || extent.height == 0) {
            return fail(ErrorCode::InvalidArgument,
                        "resize_swapchain(): drawable extent must be non-zero");
        }
        swapchain->layer.drawableSize = CGSizeMake(static_cast<CGFloat>(extent.width),
                                                   static_cast<CGFloat>(extent.height));
        swapchain->info.extent = extent;
        MetalTexture* texture = textures_.resolve(swapchain->texture);
        if (texture != nullptr) {
            texture->desc.extent = {extent.width, extent.height, 1};
        }
        return ok();
    }
    [[nodiscard]] SwapchainInfo swapchain_info(SwapchainHandle handle) const noexcept override {
        const MetalSwapchain* swapchain = swapchains_.resolve(handle);
        return swapchain != nullptr ? swapchain->info : SwapchainInfo{};
    }
    Expected<u32, Error> acquire_next_image(SwapchainHandle handle, SemaphoreHandle signal,
                                            u64) override {
        MetalSwapchain* swapchain = swapchains_.resolve(handle);
        if (swapchain == nullptr) {
            return fail(ErrorCode::NotFound, "acquire_next_image(): stale swapchain handle");
        }
        MetalSemaphore* semaphore = semaphores_.resolve(signal);
        if (semaphore == nullptr) {
            return fail(ErrorCode::NotFound, "acquire_next_image(): stale semaphore handle");
        }
        swapchain->drawable = [swapchain->layer nextDrawable];
        if (swapchain->drawable == nil) {
            return fail(ErrorCode::Unavailable,
                        "CAMetalLayer did not provide a drawable before its timeout");
        }
        MetalTexture* texture = textures_.resolve(swapchain->texture);
        MetalTextureView* view = views_.resolve(swapchain->view);
        if (texture == nullptr || view == nullptr) {
            return fail(ErrorCode::Internal, "Metal swapchain image handles were lost");
        }
        texture->texture = swapchain->drawable.texture;
        texture->bound = true;
        texture->bytes = static_cast<u64>(swapchain->drawable.texture.allocatedSize);
        view->texture = swapchain->drawable.texture;
        semaphore->event.signaledValue = semaphore->next_signal++;
        return 0U;
    }
    [[nodiscard]] TextureHandle swapchain_texture(SwapchainHandle handle,
                                                  u32 index) const noexcept override {
        const MetalSwapchain* swapchain = swapchains_.resolve(handle);
        return swapchain != nullptr && index == 0 ? swapchain->texture : TextureHandle{};
    }
    [[nodiscard]] TextureViewHandle swapchain_view(SwapchainHandle handle,
                                                   u32 index) const noexcept override {
        const MetalSwapchain* swapchain = swapchains_.resolve(handle);
        return swapchain != nullptr && index == 0 ? swapchain->view : TextureViewHandle{};
    }
    Status present(SwapchainHandle handle, u32 image_index, SemaphoreHandle wait) override {
        MetalSwapchain* swapchain = swapchains_.resolve(handle);
        if (swapchain == nullptr || image_index != 0 || swapchain->drawable == nil) {
            return fail(ErrorCode::InvalidArgument,
                        "present(): no matching Metal drawable is acquired");
        }
        MetalSemaphore* semaphore = semaphores_.resolve(wait);
        if (semaphore == nullptr) {
            return fail(ErrorCode::NotFound, "present(): stale semaphore handle");
        }
        id<MTLCommandBuffer> command = [queue_ commandBuffer];
        if (command == nil) {
            return fail(ErrorCode::OutOfMemory,
                        "present(): Metal could not allocate a command buffer");
        }
        [command encodeWaitForEvent:semaphore->event value:semaphore->next_wait++];
        [command presentDrawable:swapchain->drawable];
        [command commit];
        ++stats_.semaphore_waits;
        swapchain->drawable = nil;
        MetalTexture* texture = textures_.resolve(swapchain->texture);
        MetalTextureView* view = views_.resolve(swapchain->view);
        if (texture != nullptr) {
            texture->texture = nil;
            texture->bound = false;
        }
        if (view != nullptr) {
            view->texture = nil;
        }
        return ok();
    }
    [[nodiscard]] GpuMemoryReport memory_report() const noexcept override { return memory_; }
    [[nodiscard]] const DeviceStatistics& statistics() const noexcept override { return stats_; }
    void reset_statistics() noexcept override { stats_ = {}; }
    void publish_memory_pressure() noexcept override {
        memory_.device_heap_used = static_cast<u64>(device_.currentAllocatedSize);
        if (memory_.device_heap_budget != 0) {
            const f64 utilisation = static_cast<f64>(memory_.device_heap_used) /
                                    static_cast<f64>(memory_.device_heap_budget);
            PressureLevel level = PressureLevel::Normal;
            if (utilisation >= 0.95) {
                level = PressureLevel::Critical;
            } else if (utilisation >= 0.85) {
                level = PressureLevel::Elevated;
            }
            default_pressure_monitor().report_platform_level(level);
        }
        if (memory_.device_heap_used != reported_gpu_bytes_) {
            if (memory_.device_heap_used > reported_gpu_bytes_) {
                domain_record_allocation(MemoryDomain::Gpu,
                                         memory_.device_heap_used - reported_gpu_bytes_);
            } else {
                domain_record_free(MemoryDomain::Gpu,
                                   reported_gpu_bytes_ - memory_.device_heap_used);
            }
            reported_gpu_bytes_ = memory_.device_heap_used;
        }
        (void)update_memory_pressure();
    }
    [[nodiscard]] BarrierRecorder& barrier_recorder(const GraphBarrierKey&) noexcept override {
        return barriers_;
    }
    [[nodiscard]] void* native_handle() noexcept override { return (__bridge void*)device_; }

private:
    [[nodiscard]] Status ensure_binary_archive() noexcept {
        if (binary_archive_ != nil) {
            return ok();
        }
        MTLBinaryArchiveDescriptor* descriptor = [[MTLBinaryArchiveDescriptor alloc] init];
        NSError* error = nil;
        binary_archive_ = [device_ newBinaryArchiveWithDescriptor:descriptor error:&error];
        if (binary_archive_ == nil) {
            report_validation(ValidationSeverity::Error, error.localizedDescription.UTF8String);
            return fail(ErrorCode::Unavailable,
                        "Metal could not create an empty binary archive");
        }
        return ok();
    }

    [[nodiscard]] static const MetalDescriptorBinding* find_binding(
        const MetalDescriptorSetLayout& layout, u32 binding) noexcept {
        for (u32 index = 0; index < layout.binding_count; ++index) {
            if (layout.bindings[index].binding == binding) {
                return &layout.bindings[index];
            }
        }
        return nullptr;
    }

    [[nodiscard]] bool initialise_bindless_table() noexcept {
        const DescriptorBinding bindings[] = {
            {.binding = kGlobalTableTextureBinding,
             .kind = DescriptorKind::SampledTexture,
             .count = 0,
             .stages = ShaderStage::Vertex | ShaderStage::Fragment | ShaderStage::Compute,
             .partially_bound = true},
            {.binding = kGlobalTableSamplerBinding,
             .kind = DescriptorKind::Sampler,
             .count = 1,
             .stages = ShaderStage::Vertex | ShaderStage::Fragment | ShaderStage::Compute},
        };
        DescriptorSetLayoutDescription layout_description;
        layout_description.name = "Metal global texture table";
        layout_description.bindings = bindings;
        Expected<DescriptorSetLayoutHandle, Error> layout =
            create_descriptor_set_layout(layout_description);
        if (!layout) {
            report_validation(ValidationSeverity::Error, layout.error().message);
            return false;
        }
        Expected<DescriptorSetHandle, Error> set = allocate_descriptor_set(*layout, false);
        if (!set) {
            (void)descriptor_set_layouts_.destroy(*layout);
            report_validation(ValidationSeverity::Error, set.error().message);
            return false;
        }
        bindless_layout_handle_ = *layout;
        bindless_set_handle_ = *set;
        return true;
    }

    [[nodiscard]] static u32 memory_category(MemoryUse use) noexcept {
        if (use == MemoryUse::Upload) {
            return static_cast<u32>(GpuMemoryCategory::Upload);
        }
        if (use == MemoryUse::Readback) {
            return static_cast<u32>(GpuMemoryCategory::Readback);
        }
        return static_cast<u32>(GpuMemoryCategory::Persistent);
    }
    void charge(MemoryUse use, u64 bytes) noexcept {
        const u32 category = memory_category(use);
        memory_.live_bytes[category] += bytes;
        if (memory_.live_bytes[category] > memory_.peak_bytes[category]) {
            memory_.peak_bytes[category] = memory_.live_bytes[category];
        }
        ++memory_.allocation_count;
    }
    void discharge(MemoryUse use, u64 bytes) noexcept {
        const u32 category = memory_category(use);
        memory_.live_bytes[category] =
            memory_.live_bytes[category] >= bytes ? memory_.live_bytes[category] - bytes : 0;
    }
    void report_validation(ValidationSeverity severity, const char* message) noexcept {
        if (severity == ValidationSeverity::Error) {
            ++stats_.validation_errors;
        } else if (severity == ValidationSeverity::Warning) {
            ++stats_.validation_warnings;
        }
        if (validation_callback_ != nullptr) {
            validation_callback_(severity, message, validation_user_);
        }
    }

    id<MTLDevice> device_ = nil;
    id<MTLCommandQueue> queue_ = nil;
    id<MTLHeap> transient_heap_ = nil;
    HandlePool<MetalBuffer, BufferTag> buffers_;
    HandlePool<MetalTexture, TextureTag> textures_;
    HandlePool<MetalTextureView, TextureViewTag> views_;
    HandlePool<MetalSampler, SamplerTag> samplers_;
    HandlePool<MetalQueryPool, QueryPoolTag> query_pools_;
    HandlePool<MetalShaderModule, ShaderModuleTag> shaders_;
    HandlePool<MetalDescriptorSetLayout, DescriptorSetLayoutTag> descriptor_set_layouts_;
    HandlePool<MetalDescriptorSet, DescriptorSetTag> descriptor_sets_;
    HandlePool<MetalPipelineLayout, PipelineLayoutTag> pipeline_layouts_;
    HandlePool<MetalGraphicsPipeline, GraphicsPipelineTag> graphics_pipelines_;
    HandlePool<MetalComputePipeline, ComputePipelineTag> compute_pipelines_;
    HandlePool<MetalCommandBuffer, CommandBufferTag> command_buffers_;
    HandlePool<MetalFence, FenceTag> fences_;
    HandlePool<MetalSemaphore, SemaphoreTag> semaphores_;
    HandlePool<MetalSwapchain, SwapchainTag> swapchains_;
    Array<TextureHandle> transient_textures_;
    Array<BufferHandle> transient_buffers_;
    Array<DescriptorSetHandle> per_frame_descriptor_sets_[kMaxFramesInFlight];
    Array<CommandBufferHandle> per_frame_command_buffers_[kMaxFramesInFlight];
    DeviceCapabilities capabilities_;
    DeviceStatistics stats_{};
    GpuMemoryReport memory_{};
    MetalBarrierRecorder barriers_;
    ValidationCallback validation_callback_ = nullptr;
    void* validation_user_ = nullptr;
    u32 frames_in_flight_ = kDefaultFramesInFlight;
    u64 frame_index_ = 0;
    u32 frame_slot_ = 0;
    u64 frame_completion_values_[kMaxFramesInFlight]{};
    u64 timeline_ = 0;
    u64 transient_bytes_ = 0;
    id<MTLSharedEvent> timeline_event_ = nil;
    id<MTLBuffer> breadcrumb_buffer_ = nil;
    DescriptorSetLayoutHandle bindless_layout_handle_{};
    DescriptorSetHandle bindless_set_handle_{};
    SamplerHandle bindless_sampler_{};
    bool bindless_used_[kMetalBindlessCapacity]{};
    u32 bindless_next_hint_ = 0;
    bool bindless_ready_ = false;
    id<MTLBinaryArchive> binary_archive_ = nil;
    bool binary_archive_loaded_ = false;
    u64 reported_gpu_bytes_ = 0;
};

}  // namespace

Expected<Device*, Error> create_metal_device(Allocator& allocator,
                                             const DeviceDescription& desc) noexcept {
    @autoreleasepool {
        id<MTLDevice> native = default_metal_device();
        if (native == nil) {
            return fail(ErrorCode::Unavailable, "Metal did not provide a system device");
        }
        void* storage = allocator.allocate(sizeof(MetalDevice), alignof(MetalDevice));
        if (storage == nullptr) {
            return fail(ErrorCode::OutOfMemory, "no memory for a Metal device");
        }
        auto* device = ::new (storage) MetalDevice(allocator, native, desc);
        if (!device->valid()) {
            device->~MetalDevice();
            allocator.deallocate(device, sizeof(MetalDevice), alignof(MetalDevice));
            return fail(ErrorCode::Unavailable, "Metal could not create a command queue");
        }
        return static_cast<Device*>(device);
    }
}

void destroy_metal_device(Allocator& allocator, Device* device) noexcept {
    if (device == nullptr) {
        return;
    }
    auto* metal = static_cast<MetalDevice*>(device);
    metal->~MetalDevice();
    allocator.deallocate(metal, sizeof(MetalDevice), alignof(MetalDevice));
}

}  // namespace cy::rhi::metal
