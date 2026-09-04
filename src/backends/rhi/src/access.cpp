// The access table — the engine's entire synchronisation knowledge. Task 2.2.1.
//
// One row per intent. Every barrier the engine emits is two rows of this table and a comparison:
// the source half comes from what the resource was last used as, the destination half from what the
// next pass declares. Nothing else in the engine knows what a pipeline stage is.
//
// The rows were validated on a device by M3's scheduling spike (13 cases, RTX 5060, driver
// 580.95.05, VK_LAYER_KHRONOS_validation with synchronisation validation explicitly enabled, 0
// errors and 0 warnings), including the hard case of two compute passes writing different array
// layers of one image that a graphics pass then samples across a queue-family boundary.

#include <cy/backends/rhi/access.h>

#include <cy/core/base/assert.h>

namespace cy::rhi {
namespace {

// Written out rather than assembled, so the table reads as a table. The order matches the Access
// enumerators exactly and the static_assert below is what keeps it that way after an insertion.
constexpr AccessInfo kAccessTable[kAccessCount] = {
    // --- Compute -------------------------------------------------------------------------------
    {Stage::ComputeShader, AccessFlags::ShaderStorageWrite, ImageLayout::General, true, true, true,
     "ComputeStorageWrite"},
    {Stage::ComputeShader, AccessFlags::ShaderStorageRead, ImageLayout::General, false, true, true,
     "ComputeStorageRead"},
    {Stage::ComputeShader, AccessFlags::ShaderStorageRead | AccessFlags::ShaderStorageWrite,
     ImageLayout::General, true, true, true, "ComputeStorageReadWrite"},
    {Stage::ComputeShader, AccessFlags::ShaderSampledRead, ImageLayout::ShaderReadOnly, false, true,
     false, "ComputeSampledRead"},
    {Stage::ComputeShader, AccessFlags::UniformRead, ImageLayout::Undefined, false, false, true,
     "ComputeUniformRead"},

    // --- Graphics ------------------------------------------------------------------------------
    {Stage::VertexInput, AccessFlags::VertexAttributeRead, ImageLayout::Undefined, false, false,
     true, "VertexAttributeRead"},
    {Stage::VertexInput, AccessFlags::IndexRead, ImageLayout::Undefined, false, false, true,
     "IndexRead"},
    {Stage::DrawIndirect, AccessFlags::IndirectCommandRead, ImageLayout::Undefined, false, false,
     true, "IndirectCommandRead"},
    {Stage::VertexShader, AccessFlags::UniformRead, ImageLayout::Undefined, false, false, true,
     "VertexUniformRead"},
    {Stage::VertexShader, AccessFlags::ShaderStorageRead, ImageLayout::General, false, true, true,
     "VertexStorageRead"},
    {Stage::FragmentShader, AccessFlags::UniformRead, ImageLayout::Undefined, false, false, true,
     "FragmentUniformRead"},
    {Stage::FragmentShader, AccessFlags::ShaderSampledRead, ImageLayout::ShaderReadOnly, false,
     true, false, "FragmentSampledRead"},
    {Stage::FragmentShader, AccessFlags::ShaderStorageRead, ImageLayout::General, false, true, true,
     "FragmentStorageRead"},
    {Stage::ColorAttachmentOutput, AccessFlags::ColorAttachmentWrite, ImageLayout::ColorAttachment,
     true, true, false, "ColorAttachmentWrite"},
    {Stage::ColorAttachmentOutput,
     AccessFlags::ColorAttachmentRead | AccessFlags::ColorAttachmentWrite,
     ImageLayout::ColorAttachment, true, true, false, "ColorAttachmentReadWrite"},

    // Depth is tested before the fragment shader and written after it, so both stages appear on
    // both rows. Naming only the late stage would let a depth write race the early-fragment test of
    // the pass before it, which is a hazard that reproduces once every few thousand frames.
    {Stage::EarlyFragmentTests | Stage::LateFragmentTests,
     AccessFlags::DepthStencilAttachmentRead | AccessFlags::DepthStencilAttachmentWrite,
     ImageLayout::DepthStencilAttachment, true, true, false, "DepthStencilAttachmentWrite"},
    {Stage::EarlyFragmentTests | Stage::LateFragmentTests, AccessFlags::DepthStencilAttachmentRead,
     ImageLayout::DepthStencilReadOnly, false, true, false, "DepthStencilAttachmentRead"},

    // --- Transfer ------------------------------------------------------------------------------
    {Stage::Copy, AccessFlags::TransferRead, ImageLayout::TransferSource, false, true, true,
     "TransferRead"},
    {Stage::Copy, AccessFlags::TransferWrite, ImageLayout::TransferDestination, true, true, true,
     "TransferWrite"},

    // --- Host and presentation -------------------------------------------------------------------
    {Stage::Host, AccessFlags::HostRead, ImageLayout::Undefined, false, false, true, "HostRead"},

    // Present carries no stage and no access on purpose. The transition to the presentable layout
    // is ordered against the presentation engine by the semaphore the submit signals, not by a
    // destination stage — naming one here would be a barrier that claims to synchronise work the
    // command buffer does not contain.
    {Stage::None, AccessFlags::None, ImageLayout::Present, false, true, false, "Present"},
};

static_assert(sizeof(kAccessTable) / sizeof(kAccessTable[0]) == kAccessCount,
              "every Access enumerator needs a row, and the rows must be in enumerator order");

}  // namespace

const AccessInfo& access_info(Access access) noexcept {
    const auto index = static_cast<u32>(access);
    CY_ASSERT_MSG(index < kAccessCount, "Access value outside the enumeration");
    return kAccessTable[index < kAccessCount ? index : 0];
}

const char* access_name(Access access) noexcept {
    const auto index = static_cast<u32>(access);
    return index < kAccessCount ? kAccessTable[index].name : "<invalid>";
}

}  // namespace cy::rhi
