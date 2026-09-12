#pragma once
// The stage: a graphics device, the skinning compute pass, one lit draw and the PNG that comes
// back. M8.d, samples/09b-animated-character.
//
// ================================================================================================
// WHAT IS DRAWN, AND BY WHAT
// ================================================================================================
//
// Two draws per frame, through ONE pipeline that reads a single `float3` position stream:
//
//   the ground   a static buffer of tiles, in two runs so the light and dark squares take two
//                push colours. It exists so that locomotion is VISIBLE: a character running on a
//                featureless background is indistinguishable from a character running on the spot,
//                and this artefact's whole claim is that the character moves.
//   the character the SKINNING COMPUTE PASS'S OUTPUT BUFFER, bound as vertex buffer 0. Nothing on
//                the CPU writes those vertices. `SkinPass::upload_mesh` writes the bind pose into a
//                separate device-local input buffer; the buffer this draw binds is written only by
//                `skin.slang` and read only by a vertex fetch. If the dispatch did not run, the
//                draw would rasterise whatever the allocator last left in device memory.
//
// THE BARRIER BETWEEN THEM IS THE GRAPH'S. `SkinPass::declare` declares the output buffer written
// by a compute pass; the draw pass below declares it read with `Access::VertexAttributeRead`. This
// file emits no barrier, and synchronisation validation is on with its error count reported — which
// is what makes "the graph derives it" a measured claim rather than a design note.
//
// ================================================================================================
// WHY THIS IS NOT THE ENGINE'S FORWARD FRAME, AND WHAT THAT COSTS
// ================================================================================================
//
// `cy::rendering::pipeline`'s `FramePipelines` is the engine's real forward frame and this artefact
// does not use it. The reason is in `frame_pipelines.h`'s own header: `rhi::Format` has no
// `Rgba16Snorm`, so that pipeline's normal stream is `Rgba16Sfloat` carrying the octahedral pair as
// HALF FLOATS, while the skinning dispatch writes the engine's cooked `PackedNormalTangent`, which
// is 16-bit SNORM. The positions would bind through `FramePipelines` today; the frames would not.
//
// Rather than invent a second frame encoding for one consumer — which `skin_dispatch.h` explicitly
// declines to do — this artefact binds the one stream whose format is unambiguous and derives its
// normals per pixel. So what this file demonstrates is the IMPORT-TO-PIXELS chain: FBX to skeleton
// to clip to retarget to pose program to pose world to compute dispatch to a drawn triangle. It
// does NOT demonstrate a skinned mesh going through `FrameAssembly`, and
// `rendering-geometry-and-resources` must not be advanced on its strength.

#include <cy/backends/rhi/device.h>
#include <cy/backends/rhi/handles.h>
#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/math/matrix.h>
#include <cy/core/math/vec.h>
#include <cy/core/memory/allocator.h>
#include <cy/core/memory/array.h>
#include <cy/rendering/skinning/skin_pass.h>
#include <cy/servers/render/geometry/skinning.h>

#include "character.h"

namespace cy::sample::character {

/// Where the camera is and what it looks at, for one frame.
struct Shot {
    Vec3 eye{0.0F, 0.0F, 0.0F};
    Vec3 target{0.0F, 0.0F, 0.0F};
    f32 fov_y_radians = 0.9F;
};

/// What one frame cost and produced, measured.
struct FrameReport {
    /// Vulkan validation errors seen since the device was created. Any non-zero number is a defect
    /// and the artefact says so rather than burying it in a log.
    u32 validation_errors = 0;
    /// Where the skinned vertices this frame's draw read began. It ALTERNATES between two values,
    /// because the output is double buffered — a run in which it never changes is a run in which
    /// the double buffering is not happening.
    u32 vertex_offset = 0;
    /// Where in the pose world this frame's bones were read from. It alternates for the same reason
    /// — `PoseWorld::matrix_offset` moves on every publish — and a cached value would silently read
    /// the previous frame's pose.
    u32 pose_offset = 0;
};

/// The device, the pipeline, the skinning pass and the buffers behind one picture.
///
/// `open()` reports an ABSENT DEVICE as success with `available()` false rather than as an error,
/// so a machine with no graphics device says what it is missing and exits cleanly. That is the same
/// arrangement `samples/07-fidelity` and `samples/08-vertical-slice` use, and it is why capture is
/// a recipe a person runs and not a test.
class Stage {
public:
    explicit Stage(Allocator& allocator) noexcept;
    ~Stage();

    Stage(const Stage&) = delete;
    Stage& operator=(const Stage&) = delete;

    [[nodiscard]] Status open(u32 width, u32 height) noexcept;
    [[nodiscard]] bool available() const noexcept { return available_; }
    /// Why no device answered, for the message a machine without one prints.
    [[nodiscard]] const char* absence() const noexcept;

    /// Upload the character's bind pose, its skin and its indices. Once, not per frame.
    [[nodiscard]] Status stage_character(const Character& character) noexcept;

    /// Skin `pose` and draw it, writing the frame to `png_path` when that is not null.
    ///
    /// `pose` is what `PoseWorld::matrices()` holds and `pose_offset` is `matrix_offset(handle)`;
    /// both are passed rather than remembered, because the offset moves every publish.
    [[nodiscard]] Status shoot(Span<const Mat4> pose, u32 pose_offset, const Shot& shot,
                               u64 frame_index, const char* png_path, FrameReport& out) noexcept;

    void close() noexcept;

private:
    struct Device;

    [[nodiscard]] Status create_pipeline() noexcept;
    [[nodiscard]] Status create_ground() noexcept;
    [[nodiscard]] Status write_png(const char* path) noexcept;

    Allocator* allocator_ = nullptr;
    Device* device_ = nullptr;
    u32 width_ = 0;
    u32 height_ = 0;
    u32 index_count_ = 0;
    u32 vertex_count_ = 0;
    u32 bone_count_ = 0;
    /// Where the ground's two runs of vertices are. Light squares first, then dark.
    u32 ground_light_first_ = 0;
    u32 ground_light_count_ = 0;
    u32 ground_dark_first_ = 0;
    u32 ground_dark_count_ = 0;
    bool available_ = false;
    Array<u32> pixels_;
};

}  // namespace cy::sample::character
