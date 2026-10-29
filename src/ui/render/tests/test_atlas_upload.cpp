// SPDX-License-Identifier: MIT
// `integration.ui_atlas_upload`: `UiRenderer::upload_atlas` leaves the device's frame ring to the
// game.
//
// ================================================================================================
// THE DEFECT THIS HOLDS SHUT
// ================================================================================================
//
// `upload_atlas` used to copy a page in through a graph of its own, inside a device frame of its
// own: `begin_frame`, submit, `wait_idle`, `end_frame`. Called between two of the game's frames —
// the RTS battle's minimap, every sixth frame — that frame advanced the ring behind the game's
// back, so the game's next frame ran on the frame slot its previous frame had used, and every
// per-slot ring and retirement epoch in the engine moved under it. On the RTS test map with the GI,
// ambient occlusion and sun shadows on, the frame after the eighteenth such upload lost the device
// (NVRM Xid 109) in its forward pass; an odd period never did, and neither did the same uploads
// with no frame of their own (`fix-ui-atlas-upload-frame`, design.md).
//
// So the property is the ring's, and it is checked on the null device, on every leg: a page made
// with `upload_atlas` begins no frame and submits nothing; the game's frames take the slots in
// order however often a page is re-made between them; and the copy arrives in the next frame's own
// graph, before the interface reads the page.

#include <cy/backends/rhi/device.h>
#include <cy/backends/rhi/null/null_device.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/rendering/graph/executor.h>
#include <cy/rendering/graph/graph.h>
#include <cy/test/test.h>
#include <cy/ui/render/ui_renderer.h>

#include <cstring>
#include <vector>

using namespace cy;
using namespace cy::ui::render;

namespace {

constexpr u32 kExtent = 16;
constexpr u16 kImagePage = 2;

Allocator& allocator() noexcept {
    return system_allocator(MemoryDomain::Engine);
}

/// A null device, which counts the frames it began and the submissions it took.
class NullDevice {
public:
    NullDevice() noexcept
        : allocator_(system_allocator(MemoryDomain::Gpu)),
          device_(rhi::null::create_null_device(allocator_, rhi::DeviceDescription{})) {}
    ~NullDevice() {
        if (device_.has_value()) {
            rhi::null::destroy_null_device(allocator_, device_.value());
        }
    }
    NullDevice(const NullDevice&) = delete;
    NullDevice& operator=(const NullDevice&) = delete;
    NullDevice(NullDevice&&) = delete;
    NullDevice& operator=(NullDevice&&) = delete;

    [[nodiscard]] bool ok() const noexcept { return device_.has_value(); }
    [[nodiscard]] rhi::Device& device() const noexcept { return *device_.value(); }

private:
    Allocator& allocator_;
    Expected<rhi::Device*, Error> device_;
};

/// A solid premultiplied page, `side` texels square.
[[nodiscard]] std::vector<u8> solid_page(u32 side, u8 red) {
    std::vector<u8> texels(static_cast<usize>(side) * side * 4U, 0U);
    for (usize texel = 0; texel < texels.size(); texel += 4U) {
        texels[texel] = red;
        texels[texel + 3U] = 255U;
    }
    return texels;
}

[[nodiscard]] Status upload(UiRenderer& renderer, u32 side, u8 red) {
    const std::vector<u8> texels = solid_page(side, red);
    return renderer.upload_atlas(kImagePage, rhi::Format::Rgba8Unorm, side, side,
                                 Span<const u8>(texels.data(), texels.size()));
}

/// One frame of the game: the interface's stage declared over a target and the graph executed.
/// Returns how many page copies the frame's graph held, or -1 when the frame failed.
class Frame {
public:
    explicit Frame(rhi::Device& device) noexcept : device_(device) {}
    ~Frame() {
        if (!target_.is_null()) {
            device_.destroy_texture(target_);
        }
    }
    Frame(const Frame&) = delete;
    Frame& operator=(const Frame&) = delete;
    Frame(Frame&&) = delete;
    Frame& operator=(Frame&&) = delete;

    [[nodiscard]] bool create() noexcept {
        rhi::TextureDescription description;
        description.name = "ui target";
        description.format = rhi::Format::Rgba8Unorm;
        description.extent = rhi::Extent3D{kExtent, kExtent, 1};
        description.usage = rhi::TextureUsage::ColorAttachment | rhi::TextureUsage::Sampled;
        Expected<rhi::TextureHandle, Error> made = device_.create_texture(description);
        if (made.has_value()) {
            target_ = *made;
        }
        return made.has_value();
    }

    /// The slot the frame began in, and the copies its graph held.
    struct Result {
        u32 slot = 0;
        int copies = -1;
    };

    [[nodiscard]] Result run(UiRenderer& renderer) noexcept {
        Result result;
        Expected<u32, Error> began = device_.begin_frame();
        if (!began.has_value()) {
            return result;
        }
        result.slot = *began;
        rendering::RenderGraph graph(allocator());
        rendering::TextureRequest request;
        request.name = "ui target";
        request.format = rhi::Format::Rgba8Unorm;
        request.width = kExtent;
        request.height = kExtent;
        rendering::ScreenSpaceStageInputs inputs;
        inputs.target = graph.import_texture(request, target_, rhi::ImageUse::Undefined);
        inputs.width = kExtent;
        inputs.height = kExtent;
        const bool declared = renderer.declare(graph, inputs) != rendering::kInvalidPass;
        int copies = 0;
        for (rendering::PassId pass = 0; pass < graph.pass_count(); ++pass) {
            copies += std::strcmp(graph.pass_name(pass), "ui atlas update") == 0 ? 1 : 0;
        }
        bool executed = false;
        {
            rendering::GraphExecutor executor(allocator(), device_);
            executed =
                executor.execute(graph, rendering::CompileOptions{}, rendering::ExecuteOptions{})
                    .has_value() &&
                device_.wait_idle().has_value();
            executor.release();
        }
        const bool ended = device_.end_frame().has_value();
        result.copies = declared && executed && ended ? copies : -1;
        return result;
    }

private:
    rhi::Device& device_;
    rhi::TextureHandle target_;
};

[[nodiscard]] UiRendererDescription description() noexcept {
    UiRendererDescription description;
    description.width = kExtent;
    description.height = kExtent;
    return description;
}

}  // namespace

CY_TEST_CASE("upload_atlas begins no device frame and submits nothing") {
    NullDevice null;
    CY_REQUIRE(null.ok());
    rhi::Device& device = null.device();
    UiRenderer renderer(allocator());
    // `create` makes page zero, the white texel, through `upload_atlas` too.
    CY_REQUIRE(renderer.create(device, description()).has_value());
    CY_CHECK_EQ(device.statistics().frames_begun, 0U);
    CY_CHECK_EQ(device.statistics().submissions, 0U);

    CY_REQUIRE(upload(renderer, 8, 200).has_value());
    CY_REQUIRE(upload(renderer, 8, 100).has_value());
    CY_CHECK(renderer.has_atlas(kImagePage));
    CY_CHECK_EQ(device.statistics().frames_begun, 0U);
    CY_CHECK_EQ(device.statistics().submissions, 0U);
    renderer.destroy();
}

CY_TEST_CASE("the game's frames take the slots in order however often a page is re-made") {
    // THE INVARIANT WHOSE BREAKING LOST THE DEVICE: one device frame per game frame, so frame n
    // runs in slot n mod frames-in-flight whatever happens between frames.
    NullDevice null;
    CY_REQUIRE(null.ok());
    rhi::Device& device = null.device();
    const u32 ring = device.frames_in_flight();
    CY_REQUIRE(ring >= 2U);
    UiRenderer renderer(allocator());
    CY_REQUIRE(renderer.create(device, description()).has_value());
    Frame frame(device);
    CY_REQUIRE(frame.create());
    constexpr u32 kFrames = 12;
    for (u32 index = 0; index < kFrames; ++index) {
        // A page re-made every other frame, at a size that changes, as a minimap that grows does.
        if (index % 2U == 0U) {
            CY_REQUIRE(
                upload(renderer, 4U + ((index % 3U) * 4U), static_cast<u8>(index)).has_value());
        }
        const Frame::Result result = frame.run(renderer);
        CY_REQUIRE(result.copies >= 0);
        CY_CHECK_EQ(result.slot, index % ring);
    }
    CY_CHECK_EQ(device.statistics().frames_begun, u64{kFrames});
    renderer.destroy();
}

CY_TEST_CASE("the next frame copies a new page in before the interface reads it") {
    NullDevice null;
    CY_REQUIRE(null.ok());
    rhi::Device& device = null.device();
    UiRenderer renderer(allocator());
    CY_REQUIRE(renderer.create(device, description()).has_value());
    CY_REQUIRE(upload(renderer, 8, 200).has_value());
    Frame frame(device);
    CY_REQUIRE(frame.create());

    // The first frame copies page zero and the image page; the next copies nothing.
    CY_CHECK_EQ(frame.run(renderer).copies, 2);
    CY_CHECK_EQ(frame.run(renderer).copies, 0);
    // Re-made larger: its staging is made again at the new size, and one copy brings it in.
    CY_REQUIRE(upload(renderer, 16, 50).has_value());
    CY_CHECK_EQ(frame.run(renderer).copies, 1);
    // Staged into the page it has: one copy, no new page.
    const std::vector<u8> texels = solid_page(16, 90);
    CY_REQUIRE(
        renderer.stage_atlas(kImagePage, Span<const u8>(texels.data(), texels.size())).has_value());
    CY_CHECK_EQ(frame.run(renderer).copies, 1);
    renderer.destroy();
}
