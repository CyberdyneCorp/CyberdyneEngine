// The Metal device seed. Apple only, and NEVER COMPILED as of M7 — this project's machine is
// Linux and there is no Apple toolchain on it.
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

#include <cy/backends/rhi-metal/mapping.h>

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
static_assert(static_cast<u32>(MTLStorageModeManaged) ==
              static_cast<u32>(MetalStorageMode::Managed));
static_assert(static_cast<u32>(MTLStorageModePrivate) ==
              static_cast<u32>(MetalStorageMode::Private));
static_assert(static_cast<u32>(MTLStorageModeMemoryless) ==
              static_cast<u32>(MetalStorageMode::Memoryless));

static_assert(static_cast<u32>(MTLBarrierScopeBuffers) ==
              static_cast<u32>(MetalBarrierScope::Buffers));
static_assert(static_cast<u32>(MTLBarrierScopeTextures) ==
              static_cast<u32>(MetalBarrierScope::Textures));
static_assert(static_cast<u32>(MTLBarrierScopeRenderTargets) ==
              static_cast<u32>(MetalBarrierScope::RenderTargets));

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

bool metal_device_present() noexcept {
    @autoreleasepool {
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        return device != nil;
    }
}

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
    pass.colorAttachments[0].clearColor =
        MTLClearColorMake(clear[0], clear[1], clear[2], clear[3]);
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

}  // namespace cy::rhi::metal
