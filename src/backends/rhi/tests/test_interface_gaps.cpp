// SPDX-License-Identifier: MIT
// THE EIGHT METAL GAPS, AS THE INTERFACE THAT CLOSED THEM. M11.d section 1.
//
// `src/backends/rhi-metal/` is a seed that has never been compiled: it exists to name every place
// `cy::rhi` says something a Metal device cannot be asked, WHILE THAT IS STILL CHEAP TO CHANGE.
// M11.d spent that finding — the interface changed on Vulkan and the null backend before either new
// backend exists, because changing `reserve_transient_memory`'s contract after two more backends
// are written is a migration across every pass.
//
// WHAT THIS SUITE IS FOR, and why it is not in the Metal seed's own suite: the seed's suite checks
// that the gaps are RECORDED. This one checks that the interface they name now behaves the way the
// remedy said it would — on a machine with no Metal device, no D3D12 device and no GPU of any kind,
// because every one of these is a decision made from a `DeviceCapabilities` and a description
// rather than from a driver. A hypothetical Metal device and a hypothetical D3D12 device are two
// structs here, and that is the point: the interface can be judged against devices this project
// cannot open.

#include <cy/test/test.h>

#include <cy/backends/rhi/capabilities.h>
#include <cy/backends/rhi/device.h>
#include <cy/backends/rhi/validation.h>

using cy::u32;
using cy::u8;
using cy::rhi::BackendKind;
using cy::rhi::Capability;
using cy::rhi::DeviceCapabilities;
using cy::rhi::Format;
using cy::rhi::FormatFeature;
using cy::rhi::ImageUse;
using cy::rhi::MemoryPoolClass;
using cy::rhi::QueueKind;
using cy::rhi::QueueOwner;
using cy::rhi::ShaderFormat;
using cy::rhi::ShaderModuleDescription;
using cy::rhi::ValidationMessage;

namespace {

/// A device that consumes MSL and has no queue ownership — what a Metal device answers. Built from
/// the capability model alone, which is the only reason this file can assert anything about Metal
/// on a Linux machine with no Apple toolchain.
DeviceCapabilities metal_like() noexcept {
    DeviceCapabilities caps;
    caps.set_backend(BackendKind::Metal);
    caps.set_native_shader_format(ShaderFormat::Msl);
    caps.set_needs_queue_ownership_transfer(false);
    caps.set(Capability::ParallelPassRecording, false);
    // `MTLPixelFormatDepth24Unorm_Stencil8` is in Metal's enumeration and unsupported on every
    // Apple GPU; the M11.d spike confirmed it first-hand on a hosted runner, where
    // `isDepth24Stencil8PixelFormatSupported` reported 0.
    caps.set_format_features(Format::D32Sfloat, FormatFeature::DepthStencilAttachment);
    caps.set_format_features(Format::D32SfloatS8Uint, FormatFeature::DepthStencilAttachment);
    caps.set_format_features(Format::D24UnormS8Uint, FormatFeature::None);
    return caps;
}

DeviceCapabilities d3d12_like() noexcept {
    DeviceCapabilities caps;
    caps.set_backend(BackendKind::D3D12);
    caps.set_native_shader_format(ShaderFormat::Dxil);
    caps.set_needs_queue_ownership_transfer(false);
    return caps;
}

DeviceCapabilities vulkan_like() noexcept {
    DeviceCapabilities caps;
    caps.set_backend(BackendKind::Vulkan);
    caps.set_native_shader_format(ShaderFormat::Spirv);
    caps.set_needs_queue_ownership_transfer(true);
    caps.set_queue_ownership_domain(QueueKind::Graphics, 0);
    caps.set_queue_ownership_domain(QueueKind::AsyncCompute, 2);
    caps.set_queue_ownership_domain(QueueKind::Transfer, 0);
    caps.set_format_features(Format::D24UnormS8Uint, FormatFeature::DepthStencilAttachment);
    caps.set_format_features(Format::D32Sfloat, FormatFeature::DepthStencilAttachment);
    caps.set_format_features(Format::D32SfloatS8Uint, FormatFeature::DepthStencilAttachment);
    return caps;
}

constexpr u32 kSpirvMagic = 0x07230203U;

}  // namespace

// --- GAP 1: the shader interchange form ---------------------------------------------------------

CY_TEST_CASE("gap 1: a module is SPIR-V or the device's own form, and never both or neither") {
    const DeviceCapabilities vulkan = vulkan_like();
    ValidationMessage message;

    ShaderModuleDescription empty;
    empty.name = "empty";
    CY_CHECK_FALSE(validate_shader_module(empty, vulkan, message));

    const u32 words[] = {kSpirvMagic, 0, 0, 0};
    ShaderModuleDescription spirv;
    spirv.name = "spirv";
    spirv.spirv = cy::Span<const u32>(words, 4);
    CY_CHECK(validate_shader_module(spirv, vulkan, message));

    const cy::u8 msl[] = {'v', 'o', 'i', 'd'};
    ShaderModuleDescription both = spirv;
    both.native = cy::Span<const cy::u8>(msl, 4);
    both.native_format = ShaderFormat::Msl;
    // A module is one program. Two forms is a question about which one ran, and nobody could answer
    // it from a bug report.
    CY_CHECK_FALSE(validate_shader_module(both, vulkan, message));
}

CY_TEST_CASE("gap 1: the native form is checked against what the device says it consumes") {
    ValidationMessage message;
    const cy::u8 source[] = {'v', 'o', 'i', 'd'};

    ShaderModuleDescription msl;
    msl.name = "msl";
    msl.native = cy::Span<const cy::u8>(source, 4);
    msl.native_format = ShaderFormat::Msl;

    // A Metal device takes it; a D3D12 device does not, and neither does a Vulkan one. This is the
    // whole of gap 1: before M11.d there was nowhere to put this blob at all, so the translation
    // would have had to happen inside `create_shader_module` on the frame path.
    CY_CHECK(validate_shader_module(msl, metal_like(), message));
    CY_CHECK_FALSE(validate_shader_module(msl, d3d12_like(), message));
    CY_CHECK_FALSE(validate_shader_module(msl, vulkan_like(), message));

    ShaderModuleDescription dxil = msl;
    dxil.native_format = ShaderFormat::Dxil;
    CY_CHECK(validate_shader_module(dxil, d3d12_like(), message));

    // SPIR-V never travels as `native`, on any device, including one whose native form IS SPIR-V.
    // One form has one field, or a cache key has two spellings of the same bytes.
    ShaderModuleDescription smuggled = msl;
    smuggled.native_format = ShaderFormat::Spirv;
    CY_CHECK_FALSE(validate_shader_module(smuggled, vulkan_like(), message));
}

CY_TEST_CASE("gap 1: SPIR-V's magic number is checked where every machine runs it") {
    ValidationMessage message;
    const u32 words[] = {0xDEADBEEFU, 0, 0, 0};
    ShaderModuleDescription broken;
    broken.name = "broken";
    broken.spirv = cy::Span<const u32>(words, 4);
    // A Slang output that never made it through the back end is rejected in continuous integration
    // rather than on the one machine with a GPU.
    CY_CHECK_FALSE(validate_shader_module(broken, vulkan_like(), message));
}

// --- GAP 2: the transient memory pool class -----------------------------------------------------

CY_TEST_CASE("gap 2: the pool class is MET, not compared for equality") {
    // THE NUMBERS ARE MEASURED AND NOT INVENTED. M11.d's spike enumerated this project's own
    // devices: an NVIDIA RTX 5060 answers 0x03 for transient images and 0x1F for transient buffers,
    // an Intel UHD 770 answers 0x07 for everything, and llvmpipe answers 0x01.
    constexpr MemoryPoolClass kNvidiaImages{0x03};
    constexpr MemoryPoolClass kNvidiaBuffers{0x1F};

    // The seed proposed a token "the graph only compares for EQUALITY". That is the case it would
    // have got wrong: these two are not equal, one pool IS legal for both, and an equality would
    // have split the transient heap in two and lost the aliasing the plan exists to report.
    CY_CHECK_FALSE(kNvidiaImages == kNvidiaBuffers);
    const MemoryPoolClass together = meet(kNvidiaImages, kNvidiaBuffers);
    CY_CHECK_FALSE(together.empty());
    CY_CHECK_EQ(together.token, 0x03U);

    // Emptiness is the only question the graph asks of the token, and it is a real answer: two
    // transients that share no pool cannot be placed in one, and the graph refuses while compiling.
    CY_CHECK(meet(MemoryPoolClass{0x01}, MemoryPoolClass{0x02}).empty());

    // A backend with no bitmask at all — Metal, whose MTLHeap picks one storage mode — answers the
    // universal class, and meeting it with anything changes nothing. That is the case that had no
    // workaround while the parameter was a `u32` the graph intersected as Vulkan memory types.
    constexpr MemoryPoolClass kUniversal{};
    CY_CHECK_EQ(meet(kUniversal, kNvidiaImages).token, kNvidiaImages.token);
    CY_CHECK_FALSE(meet(kUniversal, kUniversal).empty());
}

// --- GAP 3: what an image is being used as ------------------------------------------------------

CY_TEST_CASE("gap 3: every image use has a name and the vocabulary is the engine's") {
    for (u32 index = 0; index < static_cast<u32>(ImageUse::Count); ++index) {
        const char* name = cy::rhi::image_use_name(static_cast<ImageUse>(index));
        CY_CHECK(name != nullptr);
        CY_CHECK(name[0] != '\0');
    }
    // The two whose names changed when `ImageLayout` became `ImageUse`, because the old ones were
    // `VkImageLayout`'s and not the engine's: VK_IMAGE_LAYOUT_GENERAL and
    // VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL.
    CY_CHECK_EQ(cy::rhi::image_use_name(ImageUse::Storage), "Storage");
    CY_CHECK_EQ(cy::rhi::image_use_name(ImageUse::SampledRead), "SampledRead");
    CY_CHECK_EQ(cy::rhi::image_use_name(ImageUse::Presentable), "Presentable");
    CY_CHECK_EQ(cy::rhi::image_use_name(static_cast<ImageUse>(ImageUse::Count)), "<invalid>");
}

CY_TEST_CASE("gap 3: two reads of one image can still need a barrier") {
    // THE REASON THE ENGINE KEEPS AN IMAGE-USE VOCABULARY AT ALL, and the reason the seed's remedy
    // — drop the field, derive it in the backend from the access masks — does not work. Sampling an
    // image and reading it as a storage image are both READS, so neither contributes a source
    // access mask to a barrier, and yet they are not the same state on Vulkan or on D3D12. The
    // difference is visible ONLY in this column.
    CY_CHECK_NE(cy::rhi::access_info(cy::rhi::Access::FragmentSampledRead).use,
                cy::rhi::access_info(cy::rhi::Access::FragmentStorageRead).use);
    CY_CHECK_EQ(cy::rhi::access_info(cy::rhi::Access::FragmentSampledRead).use,
                ImageUse::SampledRead);
    CY_CHECK_EQ(cy::rhi::access_info(cy::rhi::Access::FragmentStorageRead).use, ImageUse::Storage);

    // And Present is the intent a derivation from the masks could not answer at all: no stage, no
    // access bits, and its entire content is the state it leaves the image in.
    const cy::rhi::AccessInfo& present = cy::rhi::access_info(cy::rhi::Access::Present);
    CY_CHECK_FALSE(cy::rhi::any(present.access));
    CY_CHECK_FALSE(cy::rhi::any(present.stage));
    CY_CHECK_EQ(present.use, ImageUse::Presentable);
}

// --- GAP 4: queue ownership ---------------------------------------------------------------------

CY_TEST_CASE("gap 4: ownership is a capability and a queue kind, never a family index") {
    const DeviceCapabilities vulkan = vulkan_like();
    CY_CHECK(vulkan.needs_queue_ownership_transfer());
    // Two kinds in different domains: a resource moving between them changes hands.
    CY_CHECK_NE(vulkan.queue_ownership_domain(QueueKind::AsyncCompute),
                vulkan.queue_ownership_domain(QueueKind::Graphics));
    // Two kinds in the SAME domain: no transfer, which is what a device with no dedicated transfer
    // queue answers and is why the single-queue fold needs no special case anywhere.
    CY_CHECK_EQ(vulkan.queue_ownership_domain(QueueKind::Transfer),
                vulkan.queue_ownership_domain(QueueKind::Graphics));

    // A Metal device has `MTLCommandQueue` objects with no family and no ownership concept at all.
    // It says so once, here, instead of inventing an index per queue.
    CY_CHECK_FALSE(metal_like().needs_queue_ownership_transfer());
    CY_CHECK_FALSE(d3d12_like().needs_queue_ownership_transfer());
}

CY_TEST_CASE("gap 4: an unowned resource is a value and not a sentinel") {
    // `kQueueFamilyIgnored` was `~0U` — a Vulkan sentinel in an engine-owned interface. What
    // replaced it says the same thing without a magic number, and two unowned resources compare
    // equal whatever queue kind happens to sit in the unread field.
    constexpr QueueOwner nobody{};
    CY_CHECK_FALSE(nobody.owned);
    CY_CHECK(nobody == QueueOwner{QueueKind::Transfer, false});
    CY_CHECK_FALSE(nobody == QueueOwner{QueueKind::Graphics, true});
    CY_CHECK(QueueOwner{QueueKind::AsyncCompute, true} ==
             QueueOwner{QueueKind::AsyncCompute, true});
}

// --- GAP 5: parallel pass recording -------------------------------------------------------------

CY_TEST_CASE("gap 5: parallel pass recording is a capability, not a precondition") {
    // A device that cannot record one secondary per pass answers false and the graph records
    // sequentially. Stating an ORDERING precondition instead — which is what the seed proposed —
    // would not have helped: this engine's parallelism is ACROSS passes and
    // `MTLParallelRenderCommandEncoder`'s is WITHIN one, and no ordering rule converts one axis
    // into the other.
    CY_CHECK_FALSE(metal_like().has(Capability::ParallelPassRecording));
    DeviceCapabilities vulkan = vulkan_like();
    vulkan.set(Capability::ParallelPassRecording, true);
    CY_CHECK(vulkan.has(Capability::ParallelPassRecording));
}

// --- GAP 7: the engine picks the format ---------------------------------------------------------

CY_TEST_CASE("gap 7: the engine substitutes the depth format, and never drops the stencil") {
    const DeviceCapabilities metal = metal_like();

    // The case the gap is named for. A backend that quietly swapped this would hand the engine a
    // different precision and a different footprint with nothing saying so.
    CY_CHECK_EQ(cy::rhi::select_depth_stencil_format(metal, Format::D24UnormS8Uint),
                Format::D32SfloatS8Uint);
    // A format the device does support comes back unchanged — substitution is not rewriting.
    CY_CHECK_EQ(cy::rhi::select_depth_stencil_format(metal, Format::D32Sfloat), Format::D32Sfloat);
    // A Vulkan device that supports D24UnormS8Uint keeps it, which is the same call answering
    // differently per device rather than per backend.
    CY_CHECK_EQ(cy::rhi::select_depth_stencil_format(vulkan_like(), Format::D24UnormS8Uint),
                Format::D24UnormS8Uint);

    // A device that supports no depth-stencil format at all gets an answer it can act on rather
    // than a format it would then create with.
    DeviceCapabilities nothing;
    CY_CHECK_EQ(cy::rhi::select_depth_stencil_format(nothing, Format::D32Sfloat),
                Format::Undefined);
    // And a colour format is not a depth format, which is a question with no answer rather than a
    // guess.
    CY_CHECK_EQ(cy::rhi::select_depth_stencil_format(metal, Format::Rgba8Unorm), Format::Undefined);
}

CY_TEST_CASE("gap 7: stencil is never silently dropped") {
    // A device with depth-only support and a caller that asked for a stencil aspect. Answering
    // D32Sfloat here would make every stencil test stop happening, which is worse than being told
    // no: `Format::Undefined` is a refusal the caller sees.
    DeviceCapabilities depth_only;
    depth_only.set_format_features(Format::D32Sfloat, FormatFeature::DepthStencilAttachment);
    depth_only.set_format_features(Format::D16Unorm, FormatFeature::DepthStencilAttachment);
    CY_CHECK_EQ(cy::rhi::select_depth_stencil_format(depth_only, Format::D24UnormS8Uint),
                Format::Undefined);
    CY_CHECK_EQ(cy::rhi::select_depth_stencil_format(depth_only, Format::D32Sfloat),
                Format::D32Sfloat);
}

CY_TEST_CASE("gap 7: the preference list is tried in order and can answer nothing") {
    const DeviceCapabilities metal = metal_like();
    const Format preferences[] = {Format::D24UnormS8Uint, Format::D32SfloatS8Uint};
    CY_CHECK_EQ(cy::rhi::select_supported_format(metal, cy::Span<const Format>(preferences, 2),
                                                 FormatFeature::DepthStencilAttachment),
                Format::D32SfloatS8Uint);
    const Format unsupported[] = {Format::D24UnormS8Uint};
    CY_CHECK_EQ(cy::rhi::select_supported_format(metal, cy::Span<const Format>(unsupported, 1),
                                                 FormatFeature::DepthStencilAttachment),
                Format::Undefined);
}
