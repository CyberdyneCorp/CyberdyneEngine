#include <cy/backends/rhi-metal/mapping.h>

namespace cy::rhi::metal {
namespace {

/// One row of the format table. The `metal` column is a transcription of Metal's own enumerator
/// value; `src/device.mm` asserts every one of them against `MTLPixelFormat` when this module is
/// built on a Mac, so a wrong number there is a compile error naming the row.
struct FormatRow {
    Format engine;
    MetalEnum metal;
    /// The format a backend must use instead when `metal` is invalid. `Format::Undefined` when no
    /// substitution is needed.
    Format substitute;
};

constexpr FormatRow kFormats[] = {
    {Format::Undefined, kMetalPixelFormatInvalid, Format::Undefined},

    {Format::R8Unorm, 10, Format::Undefined},     // MTLPixelFormatR8Unorm
    {Format::R8Uint, 13, Format::Undefined},      // MTLPixelFormatR8Uint
    {Format::Rg8Unorm, 30, Format::Undefined},    // MTLPixelFormatRG8Unorm
    {Format::Rgba8Unorm, 70, Format::Undefined},  // MTLPixelFormatRGBA8Unorm
    {Format::Rgba8Srgb, 71, Format::Undefined},   // MTLPixelFormatRGBA8Unorm_sRGB
    {Format::Bgra8Unorm, 80, Format::Undefined},  // MTLPixelFormatBGRA8Unorm
    {Format::Bgra8Srgb, 81, Format::Undefined},   // MTLPixelFormatBGRA8Unorm_sRGB

    {Format::R16Uint, 23, Format::Undefined},        // MTLPixelFormatR16Uint
    {Format::R16Sfloat, 25, Format::Undefined},      // MTLPixelFormatR16Float
    {Format::Rg16Sfloat, 65, Format::Undefined},     // MTLPixelFormatRG16Float
    {Format::Rgba16Sfloat, 115, Format::Undefined},  // MTLPixelFormatRGBA16Float

    {Format::R32Uint, 53, Format::Undefined},      // MTLPixelFormatR32Uint
    {Format::R32Sint, 54, Format::Undefined},      // MTLPixelFormatR32Sint
    {Format::R32Sfloat, 55, Format::Undefined},    // MTLPixelFormatR32Float
    {Format::Rg32Sfloat, 105, Format::Undefined},  // MTLPixelFormatRG32Float

    // THREE-COMPONENT 32-BIT FLOAT HAS NO METAL PIXEL FORMAT. Metal has RGB9E5 and RG11B10 but no
    // RGB32Float: a texture of three 32-bit floats does not exist, and the substitution is the
    // four-component one at a third more memory. Vulkan has VK_FORMAT_R32G32B32_SFLOAT and the
    // engine's table inherited it.
    {Format::Rgb32Sfloat, kMetalPixelFormatInvalid, Format::Rgba32Sfloat},
    {Format::Rgba32Sfloat, 125, Format::Undefined},  // MTLPixelFormatRGBA32Float

    {Format::Rgb10A2Unorm, 90, Format::Undefined},     // MTLPixelFormatRGB10A2Unorm
    {Format::B10G11R11Ufloat, 92, Format::Undefined},  // MTLPixelFormatRG11B10Float

    {Format::D16Unorm, 250, Format::Undefined},   // MTLPixelFormatDepth16Unorm
    {Format::D32Sfloat, 252, Format::Undefined},  // MTLPixelFormatDepth32Float
    // GAP 7. MTLPixelFormatDepth24Unorm_Stencil8 exists in the enumeration and is UNSUPPORTED on
    // every Apple GPU — `depth24Stencil8PixelFormatSupported` is false there. Substituting
    // Depth32Float_Stencil8 costs a third more depth memory and gains precision, and it is a
    // decision the engine should be making rather than a backend making it quietly.
    {Format::D24UnormS8Uint, kMetalPixelFormatInvalid, Format::D32SfloatS8Uint},
    {Format::D32SfloatS8Uint, 260, Format::Undefined},  // MTLPixelFormatDepth32Float_Stencil8

    // The BC family is supported on Apple silicon from the M1 onwards and NOT on iOS GPUs, which is
    // a capability query rather than a table entry. The values are the desktop enumerators.
    {Format::Bc1RgbaUnorm, 130, Format::Undefined},  // MTLPixelFormatBC1_RGBA
    {Format::Bc1RgbaSrgb, 131, Format::Undefined},   // MTLPixelFormatBC1_RGBA_sRGB
    {Format::Bc3Unorm, 134, Format::Undefined},      // MTLPixelFormatBC3_RGBA
    {Format::Bc3Srgb, 135, Format::Undefined},       // MTLPixelFormatBC3_RGBA_sRGB
    {Format::Bc4Unorm, 140, Format::Undefined},      // MTLPixelFormatBC4_RUnorm
    {Format::Bc5Unorm, 142, Format::Undefined},      // MTLPixelFormatBC5_RGUnorm
    {Format::Bc6HUfloat, 151, Format::Undefined},    // MTLPixelFormatBC6H_RGBUfloat
    {Format::Bc7Unorm, 152, Format::Undefined},      // MTLPixelFormatBC7_RGBAUnorm
    {Format::Bc7Srgb, 153, Format::Undefined},       // MTLPixelFormatBC7_RGBAUnorm_sRGB
};

static_assert(sizeof(kFormats) / sizeof(kFormats[0]) == static_cast<usize>(Format::Count),
              "every engine format has a row, including the ones with no Metal equivalent — a "
              "format added to types.h without a row here would silently map to invalid");

constexpr MetalGapRecord kGaps[] = {
    {MetalGap::ShaderInterchangeIsSpirv, "ShaderModuleDescription::spirv (Span<const u32>)",
     "MSL source, or a compiled .metallib, through newLibraryWithSource: or newLibraryWithData:",
     "add an optional `Span<const u8> native` beside the SPIR-V and a `native_shader_format()` "
     "capability, so a cook can produce the form the device actually consumes",
     true, MetalGapStatus::Closed,
     ""},
    {MetalGap::TransientMemoryTypeBits,
     "Device::reserve_transient_memory(u64 bytes, u32 memory_type_bits)",
     "",  // nothing at all: MTLHeap picks one MTLStorageMode and there is no bitmask of types
     "replace the bitmask with an opaque `MemoryPoolClass` the backend defines and the graph only "
     "compares for equality — the graph never interprets the bits, it only intersects them",
     false, MetalGapStatus::Closed,
     "M11.d task 1.1: `MemoryPoolClass`, MET rather than compared for equality. The remedy above "
     "says equality and that was measured WRONG: an NVIDIA RTX 5060 answers 0x03 for transient "
     "images and 0x1F for transient buffers, so an equality would split the transient heap in two "
     "and lose the aliasing the plan reports. The graph meets the token and tests it for empty, "
     "and never interprets it"},
    {MetalGap::ImageLayoutHasNoEquivalent, "ImageBarrier::old_layout / new_layout (ImageLayout)",
     "",  // Metal has no image layouts; a tracked heap needs no transition and an untracked one a
          // MTLFence
     "derive the layout inside the Vulkan backend from the access masks `access.h` already "
     "carries, and drop `ImageLayout` from the barrier — the engine has the information without it",
     true, MetalGapStatus::Closed,
     "M11.d task 1.3: `ImageLayout` became `ImageUse`, and inside cy::rhi `VkImageLayout` now "
     "appears only in vulkan_translate.{h,cpp} — the editor's viewport publisher is a native "
     "Vulkan client that never goes through this interface. The remedy above is NOT IMPLEMENTABLE "
     "and that is the finding: a "
     "barrier's `src_access` carries only the WRITE access, because a write-after-read needs an "
     "execution dependency and not a memory one, so it is not the resource's current state and a "
     "backend deriving from it would transition from the wrong one"},
    {MetalGap::QueueFamilyIndex, "Device::queue_family(QueueKind) -> u32, kQueueFamilyIgnored",
     "",  // MTLCommandQueue objects have no family index and no ownership transfer
     "replace with `bool needs_queue_ownership_transfer()` on DeviceCapabilities; the family index "
     "never leaves the Vulkan backend",
     true, MetalGapStatus::Closed,
     "M11.d task 1.3, as proposed. `Device::queue_family()` and `kQueueFamilyIgnored` are gone; a "
     "barrier carries `QueueKind`s and an `ownership_transfer` flag; `DeviceCapabilities` answers "
     "`needs_queue_ownership_transfer()` and an opaque `queue_ownership_domain(QueueKind)` the "
     "graph only compares"},
    {MetalGap::SecondaryCommandBufferInheritance,
     "Device::execute_secondary(CommandBufferHandle primary, ...)",
     "MTLParallelRenderCommandEncoder, whose sub-encoders exist only inside a live encoder",
     "make 'the pass is begun before its secondaries are recorded' a stated precondition; the "
     "render graph very likely already satisfies it and nothing in the interface says so",
     false, MetalGapStatus::Closed,
     "M11.d task 1.2: `Capability::ParallelPassRecording`, one term in executor.cpp. The remedy "
     "above does not address the mismatch: this engine records one secondary PER PASS, across "
     "passes, and MTLParallelRenderCommandEncoder parallelises WITHIN one pass. Different axes; no "
     "ordering rule converts one into the other. A device that answers false records sequentially "
     "and produces the identical command stream"},
    {MetalGap::PipelineCacheIsABlob, "Device::save_pipeline_cache(Span<u8> out)",
     "MTLBinaryArchive, serialised to a URL",
     "take a path, or an opaque backend-defined token the engine stores and hands back", true,
     MetalGapStatus::Closed,
     "M11.d task 1.3: both calls take a path, and an absent file is a cold start rather than an "
     "error. AND NOTHING IN THE TREE CALLS EITHER OF THEM — the requirement they serve, a cache "
     "persisted across runs, is unimplemented above the RHI, which a signature change does not "
     "fix"},
    {MetalGap::Depth24Stencil8Unavailable, "Format::D24UnormS8Uint",
     "MTLPixelFormatDepth24Unorm_Stencil8, unsupported on every Apple GPU",
     "a per-format support query on DeviceCapabilities, so the engine chooses the substitute "
     "rather than the backend making it quietly",
     true, MetalGapStatus::Closed,
     "M11.d task 1.3, and it was never an interface change: `format_features()` has answered per "
     "format since M3 with no consumer above src/backends/rhi/. The consumer is the fix — "
     "`select_depth_stencil_format()` is the engine picking, FrameAssembly calls it, and "
     "`validate_texture` refuses an unsupported depth target rather than letting a backend "
     "substitute quietly"},
    {MetalGap::PushConstantRangeOffsets, "PushConstantRange::offset with a multi-stage mask",
     "setBytes:length:atIndex: binds a whole block to one stage's argument table",
     "nothing: a range shared between two stages becomes two small uploads, and the sizes involved "
     "make that free. Recorded so the next reader does not re-derive that",
     true, MetalGapStatus::NoChangeNeeded,
     "M11.d task 1.4: measured again and still nothing to do. Recorded rather than silently "
     "skipped, which is the difference between a gap somebody closed and a gap somebody forgot"},
};

static_assert(sizeof(kGaps) / sizeof(kGaps[0]) == kMetalGapCount,
              "every MetalGap enumerator has a record; a gap with no text is a gap nobody reads");

}  // namespace

MetalEnum metal_pixel_format(Format format) noexcept {
    const auto index = static_cast<usize>(format);
    return index < static_cast<usize>(Format::Count) ? kFormats[index].metal
                                                     : kMetalPixelFormatInvalid;
}

Format metal_format_substitution(Format format) noexcept {
    const auto index = static_cast<usize>(format);
    return index < static_cast<usize>(Format::Count) ? kFormats[index].substitute
                                                     : Format::Undefined;
}

MetalStorageMode metal_storage_mode(bool host_visible, bool transient_only) noexcept {
    if (transient_only) {
        // A depth or MSAA target that is never read outside its own pass lives in TILE MEMORY and
        // never reaches system memory at all. Vulkan expresses this with
        // VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT and lazily allocated memory; on a tiler it is the
        // difference between allocating a full-resolution depth buffer and allocating nothing.
        return MetalStorageMode::Memoryless;
    }
    // `Managed` is deliberately never returned. It exists only on Intel Macs, where the CPU and GPU
    // have separate copies that must be synchronised by hand — and a backend that returned it would
    // owe every write a `didModifyRange:`. Apple silicon has unified memory and `Shared` is both
    // correct and fast there. An Intel Mac gets a slower `Shared` rather than a subtly wrong
    // `Managed`, which is the right trade for a seed.
    return host_visible ? MetalStorageMode::Shared : MetalStorageMode::Private;
}

MetalBarrierScope metal_barrier_scope(AccessFlags access) noexcept {
    auto scope = static_cast<u32>(MetalBarrierScope::None);
    const auto bits = static_cast<u64>(access);

    // Twenty engine access bits collapse into three Metal ones. The collapse is lossless FOR METAL:
    // it tracks hazards per resource, so knowing that a buffer was read as an index buffer rather
    // than as a storage buffer buys a Metal backend nothing. It is lossy for anyone hoping to share
    // one barrier derivation between the two backends, which is why the graph derives and the
    // backend applies.
    constexpr u64 kBufferAccess =
        static_cast<u64>(AccessFlags::IndirectCommandRead) |
        static_cast<u64>(AccessFlags::IndexRead) |
        static_cast<u64>(AccessFlags::VertexAttributeRead) |
        static_cast<u64>(AccessFlags::UniformRead) |
        static_cast<u64>(AccessFlags::ShaderStorageRead) |
        static_cast<u64>(AccessFlags::ShaderStorageWrite) |
        static_cast<u64>(AccessFlags::TransferRead) | static_cast<u64>(AccessFlags::TransferWrite) |
        static_cast<u64>(AccessFlags::HostRead) | static_cast<u64>(AccessFlags::HostWrite);
    constexpr u64 kTextureAccess = static_cast<u64>(AccessFlags::ShaderSampledRead) |
                                   static_cast<u64>(AccessFlags::ShaderStorageRead) |
                                   static_cast<u64>(AccessFlags::ShaderStorageWrite) |
                                   static_cast<u64>(AccessFlags::TransferRead) |
                                   static_cast<u64>(AccessFlags::TransferWrite);
    constexpr u64 kRenderTargetAccess = static_cast<u64>(AccessFlags::ColorAttachmentRead) |
                                        static_cast<u64>(AccessFlags::ColorAttachmentWrite) |
                                        static_cast<u64>(AccessFlags::DepthStencilAttachmentRead) |
                                        static_cast<u64>(AccessFlags::DepthStencilAttachmentWrite);

    if ((bits & kBufferAccess) != 0U) {
        scope |= static_cast<u32>(MetalBarrierScope::Buffers);
    }
    if ((bits & kTextureAccess) != 0U) {
        scope |= static_cast<u32>(MetalBarrierScope::Textures);
    }
    if ((bits & kRenderTargetAccess) != 0U) {
        scope |= static_cast<u32>(MetalBarrierScope::RenderTargets);
    }
    return static_cast<MetalBarrierScope>(scope);
}

MetalRenderStage metal_render_stage(Stage stage) noexcept {
    auto stages = static_cast<u32>(MetalRenderStage::None);
    const auto bits = static_cast<u64>(stage);

    constexpr u64 kVertexStages = static_cast<u64>(Stage::DrawIndirect) |
                                  static_cast<u64>(Stage::VertexInput) |
                                  static_cast<u64>(Stage::VertexShader);
    constexpr u64 kFragmentStages =
        static_cast<u64>(Stage::FragmentShader) | static_cast<u64>(Stage::EarlyFragmentTests) |
        static_cast<u64>(Stage::LateFragmentTests) | static_cast<u64>(Stage::ColorAttachmentOutput);

    if ((bits & kVertexStages) != 0U) {
        stages |= static_cast<u32>(MetalRenderStage::Vertex);
    }
    if ((bits & kFragmentStages) != 0U) {
        stages |= static_cast<u32>(MetalRenderStage::Fragment);
    }
    // COMPUTE AND TRANSFER HAVE NO RENDER STAGE, and that is not an omission: in Metal they are
    // separate encoders, and the fence between two encoders needs no stage at all. A caller that
    // asked for the render stage of a compute dispatch is asking a question with no answer, and
    // `None` is that answer rather than a guess.
    return static_cast<MetalRenderStage>(stages);
}

MetalEnum metal_load_action(LoadOp op) noexcept {
    switch (op) {
        case LoadOp::Load:
            return 1;  // MTLLoadActionLoad
        case LoadOp::Clear:
            return 2;  // MTLLoadActionClear
        case LoadOp::DontCare:
            break;
    }
    return 0;  // MTLLoadActionDontCare
}

MetalEnum metal_store_action(StoreOp op) noexcept {
    switch (op) {
        case StoreOp::Store:
            return 1;  // MTLStoreActionStore
        case StoreOp::DontCare:
            break;
    }
    return 0;  // MTLStoreActionDontCare
}

const MetalGapRecord& metal_gap(MetalGap gap) noexcept {
    const auto index = static_cast<usize>(gap);
    return kGaps[index < kMetalGapCount ? index : 0];
}

Span<const MetalGapRecord> metal_gaps() noexcept {
    return {kGaps, kMetalGapCount};
}

u32 metal_open_gap_count() noexcept {
    u32 open = 0;
    for (const MetalGapRecord& record : kGaps) {
        open += record.status == MetalGapStatus::Open ? 1U : 0U;
    }
    return open;
}

u32 metal_blocking_gap_count() noexcept {
    u32 blocking = 0;
    for (const MetalGapRecord& record : kGaps) {
        // OPEN AND WITH NO WORKAROUND. A closed gap is not something M11 pays for, and counting it
        // would make this number unable to move — which is the defect task 1.5 names: a gap closed
        // in prose and not in the data is a gap that will be re-found at the first Metal compile.
        blocking += (record.status == MetalGapStatus::Open && !record.workaroundable) ? 1U : 0U;
    }
    return blocking;
}

}  // namespace cy::rhi::metal
