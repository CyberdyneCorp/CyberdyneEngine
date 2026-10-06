#pragma once
// The stage: a graphics device, the ENGINE'S FORWARD FRAME, and the PNG that comes back. M8.d,
// samples/09b-animated-character; drawn through the frame since issue #76 stage 3.
//
// ================================================================================================
// WHAT IS DRAWN, AND BY WHAT
// ================================================================================================
//
// One `FrameAssembly`, recorded by `FramePipelines` and `FrameRecorder` with their skinned
// variants on, over a scene of two kinds of instance:
//
//   the ground     a chequerboard of one-metre tiles, each an instance of the frame's own cube in
//                  one of two materials. It exists so that locomotion is VISIBLE: a character
//                  running on a featureless background is indistinguishable from one running on
//                  the spot, and the character's shadow lands on it.
//   the character  a SKINNED DRAW. `skinning::SkinnedScene` holds its bind pose and influences, its
//                  pose buffer is filled from the `PoseWorld`'s dirty range, and one compute pass
//                  skins it; the frame's depth prepass, directional shadow and opaque pass draw the
//                  pass's output through `FramePipelines::skinned_pipeline`, with the
//                  normal-tangent stream bound as the `Rgba16Snorm` the dispatch wrote. Nothing on
//                  the CPU writes the vertices that appear in the picture.
//
// THE BARRIERS ARE THE GRAPH'S. The skinning pass declares its output written; every frame stage
// that draws it declares the read (`FramePassCallback::vertex_reads`), and synchronisation
// validation is on with its error count reported.
//
// Until stage 3 this file had a pipeline of its own that read positions alone and rebuilt normals
// from screen-space derivatives, because `rhi::Format` had no `Rgba16Snorm` for the skinned frame
// stream. The character is now lit like everything else the frame draws: smooth normals, the
// clustered lights, the directional shadow it casts and receives, and per-object motion vectors.

#include <cy/animation/pose_world.h>
#include <cy/backends/rhi/device.h>
#include <cy/backends/rhi/handles.h>
#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/math/matrix.h>
#include <cy/core/math/vec.h>
#include <cy/core/memory/allocator.h>
#include <cy/core/memory/array.h>

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
    /// Matrices the frame copied to the device pose buffer: the world's dirty range, one
    /// skeleton's current half for this one-character scene, never the whole world.
    u32 uploaded_matrices = 0;
    /// Draws the frame recorded from the skinning output, over its depth prepass, shadow and opaque
    /// passes. Three a frame; zero would be a character the frame never drew.
    u32 skinned_draws = 0;
};

/// The device, the frame and the skinned scene behind one picture.
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

    /// Upload the character's bind pose, its skin and its indices, and build the frame around it.
    /// Once, not per frame.
    [[nodiscard]] Status stage_character(const Character& character) noexcept;

    /// Skin the character from `poses` and draw the frame, writing it to `png_path` when that is
    /// not null.
    ///
    /// The world's DIRTY RANGE is uploaded and then cleared, and `handle`'s offset is read now,
    /// because it moves at every publish.
    [[nodiscard]] Status shoot(animation::PoseWorld& poses, animation::PoseHandle handle,
                               const Shot& shot, u64 frame_index, const char* png_path,
                               FrameReport& out) noexcept;

    void close() noexcept;

    // Public because the frame's lookups are plain function pointers.
    struct Device;
    [[nodiscard]] Device* device() const noexcept { return device_; }

private:
    [[nodiscard]] Status create_frame() noexcept;
    [[nodiscard]] Status create_scene(const Character& character) noexcept;
    [[nodiscard]] Status write_png(const char* path) noexcept;

    Allocator* allocator_ = nullptr;
    Device* device_ = nullptr;
    u32 width_ = 0;
    u32 height_ = 0;
    bool available_ = false;
    Array<u32> pixels_;
};

}  // namespace cy::sample::character
