#pragma once
// THE FRAME, RECORDED. M8.c tasks 1b.2, 5.4, 5.5 and 5.6.
//
// ================================================================================================
// THIS IS THE FILE THAT TURNS THE SLICE'S PICTURE FROM DRAWN INTO CAPTURED
// ================================================================================================
//
// M8.b's README carries a section called "Why the picture is drawn and not captured", and its
// answer was correct at the time: `FrameAssembly` hands each pass's record callback to its CALLER,
// the sample supplied none, and writing them would have meant a second renderer beside
// `samples/03-first-light`'s. M8.c's section 1b built that layer once, in the engine —
// `cy::rendering-pipeline` — so what is left here is what a GAME does with it: fill in the
// geometry, the materials and the exposure, attach the particle renderer, and read the image back.
//
// Everything in the committed pictures comes out of the frame:
//
//   * the silhouettes are per MESH ASSET, keyed by the handle `SceneIndex::surface_of` published,
//     so a resolver that answered one handle for every reference would draw every prop with the
//     ground's proportions. That is M8.b's gate finding — the sample's picture used to take its
//     silhouettes from the sample's own table and would have drawn a defect correctly — carried
//     into pixels rather than left in a projection;
//   * the colours are the GPU material table's, indexed by the material slot the same query
//     published;
//   * the lights are the assembly's, clustered by the assembly;
//   * the particles are `publish_sprites`' records, uploaded into the particle renderer's ring and
//     drawn by its extension inside the frame's transparent stage.
//
// ================================================================================================
// AND THE CONTROL IS THE SAME CODE PATH WITH THE CALLBACKS TAKEN OUT
// ================================================================================================
//
// `CaptureMode::Assembled` supplies an EMPTY `FrameSinks` — exactly what every caller in this tree
// supplied before M8.c — and executes the identical graph. Task 5.5's before-and-after pair is
// therefore two calls of one function rather than two programs, which is the only arrangement in
// which the difference between the two images is the record callbacks and nothing else.
//
// A machine with no Vulkan device is a REPORTED gap and not a failure: `available()` answers false,
// says which backend was selected instead and why, and `--capture` exits non-zero having said so.

#include <cy/backends/rhi/device.h>
#include <cy/core/base/expected.h>
#include <cy/core/math/matrix.h>
#include <cy/core/memory/array.h>
#include <cy/rendering/particles/particle_renderer.h>
#include <cy/rendering/pipeline/frame_recorder.h>

#include "capture_report.h"
#include "slice.h"

namespace cy::sample::slice {

class Presentation;
struct ViewState;

/// The device, the pipelines, the geometry and the readback.
class FrameCapture {
public:
    explicit FrameCapture(Allocator& allocator) noexcept;
    ~FrameCapture();

    FrameCapture(const FrameCapture&) = delete;
    FrameCapture& operator=(const FrameCapture&) = delete;

    /// Create the device and everything that hangs off it. Answers ok() with `available()` false
    /// when the machine has no Vulkan device — a reported gap, not a failure.
    [[nodiscard]] Status open(cy::u32 width, cy::u32 height) noexcept;
    void close() noexcept;

    [[nodiscard]] bool available() const noexcept { return available_; }
    /// Why not, when not. Never null.
    [[nodiscard]] const char* unavailable_reason() const noexcept;

    /// Build the geometry and the material table from the level the slice authored. One box per
    /// distinct MESH ASSET, with the asset's own bounds — see the header comment.
    [[nodiscard]] Status describe(const Slice& slice, const Presentation& presentation) noexcept;

    /// Assemble, record and execute one frame over the presentation's own scene index, then write
    /// it out. `path` may be null, in which case the frame is recorded and not saved.
    ///
    /// `readout` is drawn INTO THE FRAME as sprite instances through the same particle renderer the
    /// effect uses — one particle per lit cell of a 3x5 glyph — so that task 5.5's "with its budget
    /// on screen" is the engine's own output rather than text a script painted over a photograph.
    /// It is a deliberate small thing: a real game's heads-up display goes through `cy::ui` and a
    /// text pass, and this sample has neither attached to a recorded frame. `slice.py` is told what
    /// the numbers are so nothing here is the only record of them.
    [[nodiscard]] Status shoot(const Presentation& presentation,
                               Span<const cy::rendering::particles::ParticleInstance> particles,
                               Span<const char* const> readout, CaptureMode mode, const char* path,
                               CaptureReport& out) noexcept;

    [[nodiscard]] Span<const cy::u32> pixels() const noexcept { return pixels_.span(); }

    /// The device and everything that hangs off it. Public because the surface query and the
    /// geometry lookup are PLAIN FUNCTION POINTERS — `SurfaceQueryFn` and `DrawGeometryFn` both
    /// are, deliberately, so that neither seam needs a virtual call per draw — and a free function
    /// cannot see a private nested type. `FrameRecorder` publishes its own state for the same
    /// reason and says so.
    struct Device;

private:
    [[nodiscard]] Status create_geometry() noexcept;
    [[nodiscard]] Status create_materials() noexcept;
    [[nodiscard]] Status read_pixels() noexcept;

    // `shoot` is four things in a row and each of them is one paragraph of the header comment
    // above; they are separate functions because a single one measured 51 on this project's
    // cognitive-complexity scale — most of it the `if (Status s = f(); !s) return s;` idiom
    // repeated fifteen times — against a band this project holds systems code to at 25 to 35.
    [[nodiscard]] Status rebuild_scene(const Presentation& presentation,
                                       const ViewState& view) noexcept;
    [[nodiscard]] Status attach_particles(
        cy::u32 frame_slot, Span<const cy::rendering::particles::ParticleInstance> particles,
        Span<const char* const> readout, const Mat4& relative_view, f32 aspect,
        f32 vertical_fov) noexcept;
    void collect(CaptureReport& out) noexcept;
    [[nodiscard]] Status save(const char* path, CaptureReport& out) noexcept;

    Allocator* allocator_ = nullptr;
    Device* device_ = nullptr;
    bool available_ = false;
    cy::u32 width_ = 0;
    cy::u32 height_ = 0;
    Array<cy::u32> pixels_;
    Array<cy::u32> previous_;
    /// The caller's particles followed by the readout's glyph cells. One array so the renderer's
    /// ring takes one upload.
    Array<cy::rendering::particles::ParticleInstance> composed_;
};

}  // namespace cy::sample::slice
