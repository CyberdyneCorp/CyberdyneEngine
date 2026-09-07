// The Metal seed's findings, tested on a machine with no Metal — which is the point of the seed
// being shaped this way. M7 task 10.5.
//
// What this suite CAN check: that every engine format has a row, that the two with no Metal
// equivalent are named rather than substituted quietly, that the access and stage collapses are
// total, that the gap table is complete and legible, and that the registration seam refuses
// cleanly on a platform with no Metal. What it CANNOT check is `src/device.mm`, which has never
// been compiled anywhere.

#include <cy/test/test.h>

#include <cy/backends/rhi-metal/backend.h>
#include <cy/backends/rhi-metal/mapping.h>

namespace {

using cy::u32;
using cy::rhi::AccessFlags;
using cy::rhi::Format;
using cy::rhi::LoadOp;
using cy::rhi::Stage;
using cy::rhi::StoreOp;
using cy::rhi::metal::kMetalGapCount;
using cy::rhi::metal::kMetalPixelFormatInvalid;
using cy::rhi::metal::metal_barrier_scope;
using cy::rhi::metal::metal_blocking_gap_count;
using cy::rhi::metal::metal_format_substitution;
using cy::rhi::metal::metal_gap;
using cy::rhi::metal::metal_gaps;
using cy::rhi::metal::metal_load_action;
using cy::rhi::metal::metal_pixel_format;
using cy::rhi::metal::metal_render_stage;
using cy::rhi::metal::metal_seed_status;
using cy::rhi::metal::metal_storage_mode;
using cy::rhi::metal::metal_store_action;
using cy::rhi::metal::MetalBarrierScope;
using cy::rhi::metal::MetalGap;
using cy::rhi::metal::MetalRenderStage;
using cy::rhi::metal::MetalStorageMode;
using cy::rhi::metal::register_metal_backend;

}  // namespace

CY_TEST_CASE("metal seed: every engine format maps or is named as unmappable") {
    u32 mapped = 0;
    u32 substituted = 0;
    for (u32 index = 1; index < static_cast<u32>(Format::Count); ++index) {
        const auto format = static_cast<Format>(index);
        const bool has_metal = metal_pixel_format(format) != kMetalPixelFormatInvalid;
        const bool has_substitute = metal_format_substitution(format) != Format::Undefined;
        // Exactly one of the two. A format that had both would be a substitution nobody needed; one
        // with neither would be a format a Metal backend silently could not create.
        CY_CHECK_NE(has_metal, has_substitute);
        mapped += has_metal ? 1U : 0U;
        substituted += has_substitute ? 1U : 0U;
    }
    CY_TEST_MESSAGE("formats: ", mapped, " map directly, ", substituted,
                    " have no Metal equivalent and are substituted explicitly");
    CY_CHECK_EQ(substituted, 2U);
    CY_CHECK_EQ(metal_pixel_format(Format::Undefined), kMetalPixelFormatInvalid);
}

CY_TEST_CASE("metal seed: the two unmappable formats are the ones the finding names") {
    // GAP 7 and its neighbour, as a test rather than as a paragraph.
    //
    // D24UnormS8Uint: `MTLPixelFormatDepth24Unorm_Stencil8` exists in the enumeration and is
    // unsupported on every Apple GPU. Rgb32Sfloat: Metal has no three-component 32-bit float pixel
    // format at all. Both substitutions cost memory and neither is a decision a backend should be
    // making without the engine knowing.
    CY_CHECK_EQ(metal_pixel_format(Format::D24UnormS8Uint), kMetalPixelFormatInvalid);
    CY_CHECK_EQ(metal_format_substitution(Format::D24UnormS8Uint), Format::D32SfloatS8Uint);
    CY_CHECK_EQ(metal_pixel_format(Format::Rgb32Sfloat), kMetalPixelFormatInvalid);
    CY_CHECK_EQ(metal_format_substitution(Format::Rgb32Sfloat), Format::Rgba32Sfloat);

    // And the substitutes themselves map, or the substitution is a loop.
    CY_CHECK_NE(metal_pixel_format(Format::D32SfloatS8Uint), kMetalPixelFormatInvalid);
    CY_CHECK_NE(metal_pixel_format(Format::Rgba32Sfloat), kMetalPixelFormatInvalid);
}

CY_TEST_CASE("metal seed: twenty access bits collapse into three, and the collapse is total") {
    // Every engine access bit reaches at least one Metal scope. A bit that reached none would be a
    // hazard a Metal backend never fenced.
    for (u32 bit = 0; bit < 15; ++bit) {
        const auto access = static_cast<AccessFlags>(1ULL << bit);
        CY_CHECK_NE(metal_barrier_scope(access), MetalBarrierScope::None);
    }
    CY_CHECK_EQ(metal_barrier_scope(AccessFlags::None), MetalBarrierScope::None);

    // A colour attachment write is a render target and not a buffer, which is the distinction the
    // three scopes exist to make.
    CY_CHECK_EQ(metal_barrier_scope(AccessFlags::ColorAttachmentWrite),
                MetalBarrierScope::RenderTargets);
    // A storage image write is both a buffer and a texture in Metal's vocabulary, because Metal
    // does not distinguish the two at the scope level.
    const auto storage = metal_barrier_scope(AccessFlags::ShaderStorageWrite);
    CY_CHECK_NE(static_cast<u32>(storage) & static_cast<u32>(MetalBarrierScope::Textures), 0U);
}

CY_TEST_CASE("metal seed: a compute stage has no render stage, and that is an answer") {
    CY_CHECK_EQ(metal_render_stage(Stage::VertexShader), MetalRenderStage::Vertex);
    CY_CHECK_EQ(metal_render_stage(Stage::FragmentShader), MetalRenderStage::Fragment);
    CY_CHECK_EQ(metal_render_stage(Stage::ColorAttachmentOutput), MetalRenderStage::Fragment);
    // In Metal a compute dispatch is its own encoder and the fence between two encoders needs no
    // stage at all. `None` is the answer rather than a guess — a mapping that returned Vertex here
    // would put a render-stage fence on a compute encoder, which is not a thing.
    CY_CHECK_EQ(metal_render_stage(Stage::ComputeShader), MetalRenderStage::None);
    CY_CHECK_EQ(metal_render_stage(Stage::Copy), MetalRenderStage::None);
}

CY_TEST_CASE("metal seed: a transient attachment is memoryless, which is the tiler's whole point") {
    // A depth or MSAA target never read outside its own pass lives in TILE MEMORY and never reaches
    // system memory. On a tiler that is the difference between allocating a full-resolution depth
    // buffer and allocating nothing, and it is the one place where Metal's model gives the engine
    // something Vulkan's transient-attachment path only approximates.
    CY_CHECK_EQ(metal_storage_mode(false, true), MetalStorageMode::Memoryless);
    CY_CHECK_EQ(metal_storage_mode(false, false), MetalStorageMode::Private);
    CY_CHECK_EQ(metal_storage_mode(true, false), MetalStorageMode::Shared);
    // `Managed` is never returned: it exists only on Intel Macs and owes every write a
    // `didModifyRange:`. An Intel Mac gets a slower `Shared` rather than a subtly wrong `Managed`.
    CY_CHECK_NE(metal_storage_mode(true, false), MetalStorageMode::Managed);
}

CY_TEST_CASE("metal seed: load and store actions map exactly, which is worth recording") {
    // Attachment load and store operations are a tiler's own vocabulary and Vulkan borrowed them.
    // This is one of the three clean mappings, and a clean mapping is worth as much to the M11
    // reader as a gap.
    CY_CHECK_EQ(metal_load_action(LoadOp::Load), 1U);
    CY_CHECK_EQ(metal_load_action(LoadOp::Clear), 2U);
    CY_CHECK_EQ(metal_load_action(LoadOp::DontCare), 0U);
    CY_CHECK_EQ(metal_store_action(StoreOp::Store), 1U);
    CY_CHECK_EQ(metal_store_action(StoreOp::DontCare), 0U);
}

CY_TEST_CASE("metal seed: every gap has a record, a remedy and a verdict") {
    const auto gaps = metal_gaps();
    CY_REQUIRE_EQ(gaps.size(), kMetalGapCount);

    for (u32 index = 0; index < kMetalGapCount; ++index) {
        const auto& record = gaps[index];
        // The enumerator and the row agree, so the table cannot be reordered into a lie.
        CY_CHECK_EQ(static_cast<u32>(record.gap), index);
        CY_CHECK_EQ(&metal_gap(static_cast<MetalGap>(index)), &record);
        // Each names the interface element it is about and what to do. A gap with no remedy is a
        // complaint rather than a finding.
        CY_CHECK(record.interface_element[0] != '\0');
        CY_CHECK(record.remedy[0] != '\0');
        // An EMPTY `metal_equivalent` is a real value and the more expensive kind of finding:
        // "Metal has nothing at all here", as opposed to "Metal has something with different
        // semantics".
    }

    // Three of the eight have no Metal equivalent whatever, and two of the eight cannot be worked
    // around at all. Those two are what M11 pays for if nothing changes before then, and this
    // number is the reason to seed a backend four milestones early.
    u32 no_equivalent = 0;
    for (u32 index = 0; index < kMetalGapCount; ++index) {
        no_equivalent += gaps[index].metal_equivalent[0] == '\0' ? 1U : 0U;
    }
    CY_TEST_MESSAGE("gaps: ", kMetalGapCount, " total, ", no_equivalent,
                    " where Metal has no equivalent at all, ", metal_blocking_gap_count(),
                    " with no workaround");
    CY_CHECK_EQ(no_equivalent, 3U);
    CY_CHECK_EQ(metal_blocking_gap_count(), 2U);
}

CY_TEST_CASE("metal seed: the registration seam refuses cleanly where there is no Metal") {
    const auto status = metal_seed_status();
    CY_CHECK_EQ(status.gaps, kMetalGapCount);
    CY_CHECK_EQ(status.blocking_gaps, metal_blocking_gap_count());
    CY_REQUIRE(status.renders != nullptr);
    CY_CHECK(status.renders[0] != '\0');

    if (status.compiled_with_metal) {
        CY_CHECK(register_metal_backend());
        return;
    }
    // On this machine, and on every non-Apple build: refused, not registered-and-broken. A
    // registration that exists and cannot work turns "asked for metal, ran null" into a runtime
    // surprise instead of a configuration answer.
    const auto refused = register_metal_backend();
    CY_CHECK_FALSE(refused);
    CY_CHECK_EQ(refused.error().code, cy::ErrorCode::Unsupported);
    CY_CHECK_FALSE(cy::rhi::metal::metal_backend_available());
}
