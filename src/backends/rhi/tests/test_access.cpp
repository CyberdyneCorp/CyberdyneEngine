// The access table is the engine's entire synchronisation knowledge, so it is the thing to check
// first. Task 2.2.1.
//
// Every barrier in every frame is two rows of this table. A wrong row is not a crash; it is a
// hazard that reproduces once every few thousand frames on one vendor's driver. These cases are
// cheap and they are the only thing standing between an edit to access.cpp and that.

#include <cy/test/test.h>

#include <cy/backends/rhi/access.h>
#include <cy/backends/rhi/validation.h>

using cy::rhi::Access;
using cy::rhi::AccessFlags;
using cy::rhi::AccessInfo;
using cy::rhi::ImageLayout;
using cy::rhi::Stage;

CY_TEST_CASE("every access intent has a row, a name and a stage") {
    for (cy::u32 index = 0; index < cy::rhi::kAccessCount; ++index) {
        const auto access = static_cast<Access>(index);
        const AccessInfo& info = cy::rhi::access_info(access);
        CY_CHECK(info.name != nullptr);
        CY_CHECK(info.name[0] != '\0');
        CY_CHECK_EQ(info.name, cy::rhi::access_name(access));
        // Present is the one intent with no stage and no access: the transition to the presentable
        // layout is ordered by the semaphore the submit signals, not by a destination stage.
        if (access != Access::Present) {
            CY_CHECK(cy::rhi::any(info.stage));
            CY_CHECK(cy::rhi::any(info.access));
        }
        // A row must be usable against something, or nothing could ever declare it.
        CY_CHECK((info.valid_for_image || info.valid_for_buffer));
    }
}

CY_TEST_CASE("the writing intents are exactly the ones whose names say so") {
    CY_CHECK(cy::rhi::is_write(Access::ComputeStorageWrite));
    CY_CHECK(cy::rhi::is_write(Access::ComputeStorageReadWrite));
    CY_CHECK(cy::rhi::is_write(Access::ColorAttachmentWrite));
    CY_CHECK(cy::rhi::is_write(Access::ColorAttachmentReadWrite));
    CY_CHECK(cy::rhi::is_write(Access::DepthStencilAttachmentWrite));
    CY_CHECK(cy::rhi::is_write(Access::TransferWrite));

    CY_CHECK_FALSE(cy::rhi::is_write(Access::ComputeStorageRead));
    CY_CHECK_FALSE(cy::rhi::is_write(Access::FragmentSampledRead));
    CY_CHECK_FALSE(cy::rhi::is_write(Access::DepthStencilAttachmentRead));
    CY_CHECK_FALSE(cy::rhi::is_write(Access::TransferRead));
    CY_CHECK_FALSE(cy::rhi::is_write(Access::HostRead));
    CY_CHECK_FALSE(cy::rhi::is_write(Access::Present));
}

CY_TEST_CASE("a storage access implies GENERAL and a sampled access implies SHADER_READ_ONLY") {
    // The layout is what a barrier transitions to, and it is derived from the intent rather than
    // chosen by a pass. These four rows are the ones every frame uses.
    CY_CHECK_EQ(cy::rhi::access_info(Access::ComputeStorageWrite).layout, ImageLayout::General);
    CY_CHECK_EQ(cy::rhi::access_info(Access::ComputeStorageRead).layout, ImageLayout::General);
    CY_CHECK_EQ(cy::rhi::access_info(Access::FragmentSampledRead).layout,
                ImageLayout::ShaderReadOnly);
    CY_CHECK_EQ(cy::rhi::access_info(Access::ComputeSampledRead).layout,
                ImageLayout::ShaderReadOnly);
    CY_CHECK_EQ(cy::rhi::access_info(Access::ColorAttachmentWrite).layout,
                ImageLayout::ColorAttachment);
    CY_CHECK_EQ(cy::rhi::access_info(Access::TransferRead).layout, ImageLayout::TransferSource);
    CY_CHECK_EQ(cy::rhi::access_info(Access::TransferWrite).layout,
                ImageLayout::TransferDestination);
}

CY_TEST_CASE("depth is tested early and written late, so both stages appear") {
    // Naming only the late stage would let a depth write race the early-fragment test of the pass
    // before it. That hazard reproduces rarely and is expensive to find, which is why it is a test.
    const Stage expected = Stage::EarlyFragmentTests | Stage::LateFragmentTests;
    CY_CHECK_EQ(cy::rhi::access_info(Access::DepthStencilAttachmentWrite).stage, expected);
    CY_CHECK_EQ(cy::rhi::access_info(Access::DepthStencilAttachmentRead).stage, expected);
}

CY_TEST_CASE("a write-after-read hazard needs the read's stage and none of its access bits") {
    // The rule the derivation implements, restated where a reader of the table can see it: a
    // write-after-read needs an execution dependency, not a memory one. This case pins the two
    // pieces the rule is assembled from — that a read row carries a stage, and that the derivation
    // is what decides to drop its access bits (see the graph's own barrier tests).
    const AccessInfo& read = cy::rhi::access_info(Access::FragmentSampledRead);
    CY_CHECK(cy::rhi::any(read.stage));
    CY_CHECK_EQ(read.access, AccessFlags::ShaderSampledRead);
    CY_CHECK_FALSE(read.is_write);
}

CY_TEST_CASE("an intent that has no meaning for a resource kind is rejected, not approximated") {
    // HostRead against an image and a sampled read against a buffer are both declarations that
    // would otherwise produce a barrier with an undefined layout — plausible-looking and wrong.
    CY_CHECK_FALSE(cy::rhi::access_valid_for(Access::HostRead, true));
    CY_CHECK(cy::rhi::access_valid_for(Access::HostRead, false));
    CY_CHECK_FALSE(cy::rhi::access_valid_for(Access::FragmentSampledRead, false));
    CY_CHECK(cy::rhi::access_valid_for(Access::FragmentSampledRead, true));
    CY_CHECK_FALSE(cy::rhi::access_valid_for(Access::ColorAttachmentWrite, false));
    CY_CHECK(cy::rhi::access_valid_for(Access::VertexAttributeRead, false));
    CY_CHECK_FALSE(cy::rhi::access_valid_for(Access::VertexAttributeRead, true));
}

CY_TEST_CASE("a zero count in a subresource range means all remaining, resolved once") {
    using cy::rhi::SubresourceRange;
    const SubresourceRange whole = cy::rhi::resolve_range(SubresourceRange::whole(), 4, 6);
    CY_CHECK_EQ(whole.base_mip, 0);
    CY_CHECK_EQ(whole.mip_count, 4);
    CY_CHECK_EQ(whole.layer_count, 6);

    const SubresourceRange tail = cy::rhi::resolve_range(SubresourceRange{2, 0, 3, 0}, 4, 6);
    CY_CHECK_EQ(tail.mip_count, 2);
    CY_CHECK_EQ(tail.layer_count, 3);

    // A base past the end resolves to nothing rather than to a wrapped count, which is what the
    // declaration check then reports.
    const SubresourceRange past = cy::rhi::resolve_range(SubresourceRange{9, 0, 0, 0}, 4, 6);
    CY_CHECK_EQ(past.mip_count, 0);
}
