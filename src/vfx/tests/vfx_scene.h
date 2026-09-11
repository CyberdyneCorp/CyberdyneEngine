#pragma once
// The frame `render.vfx` photographs: a simulated effect, drawn. M8.c tasks 2.3 and 2.6.
//
// ================================================================================================
// WHAT THIS PROVES THAT `integration.vfx` CANNOT
// ================================================================================================
//
// `integration.vfx` asserts that the world spawned, moved and killed particles, and reads their
// attributes back through the derived layout. It cannot tell whether the numbers it checked are the
// numbers a renderer would draw — that is one publication and one upload away, and this milestone
// exists because six of them went by without anybody looking.
//
// So this harness takes the SAME cooked effect `integration.vfx_compiler` compiles and
// `integration.vfx` simulates, steps it, publishes it through `publish_sprites`, and hands the
// records to `cy::rendering::particles::ParticleRenderer` — the seam, thirty-two bytes wide —
// inside a real frame on a Vulkan device with validation on. The output image is copied back off
// the device and written as a PNG.
//
// ================================================================================================
// THE SCENE IS DELIBERATELY EMPTY EXCEPT FOR THE EFFECT
// ================================================================================================
//
// No meshes, no materials, no cubes. `src/rendering/pipeline/tests/` already photographs a lit
// scene with a plume in front of it, and a second copy of that scene here would drift from the
// first inside a milestone — the reason tests/render/README.md gives about golden images. What this
// suite is about is the SIMULATION: whether what the world produced reaches the frame, and whether
// the budget controller reducing an effect is visible in the picture rather than only in a counter.
//
// The frame is still the engine's own — `FrameAssembly`, `FramePipelines`, `FrameBindings`,
// `FrameRecorder`, the render graph and its derived barriers — so an empty draw list is a frame
// with nothing opaque in it rather than a shortcut around the frame.

#include <cy/backends/rhi/device.h>
#include <cy/core/base/expected.h>
#include <cy/core/math/matrix.h>
#include <cy/core/memory/array.h>
#include <cy/rendering/assembly/frame_assembly.h>
#include <cy/rendering/particles/particle_renderer.h>
#include <cy/rendering/pipeline/frame_recorder.h>
#include <cy/vfx/runtime.h>
#include <cy/vfx/world.h>

namespace cy::vfx_test {

using namespace cy::rendering;
using namespace cy::rendering::pipeline;

inline constexpr u32 kSceneWidth = 480;
inline constexpr u32 kSceneHeight = 270;
/// The renderer's ring. Larger than the effect's population, so `PublishReport::dropped` is zero in
/// the ordinary case and a non-zero one is a real finding rather than the ring being small.
inline constexpr u32 kRingCapacity = 4096;

/// The world, the frame, and one render of the two together.
class VfxScene {
public:
    explicit VfxScene(Allocator& allocator) noexcept;
    ~VfxScene();

    VfxScene(const VfxScene&) = delete;
    VfxScene& operator=(const VfxScene&) = delete;

    /// Cook the plume, create the frame's pipelines and bindings, and play `instances` copies of
    /// it.
    [[nodiscard]] Status build(rhi::Device& device, u32 instances = 6) noexcept;
    void release() noexcept;

    /// Advance the simulation by one frame at 60 Hz.
    [[nodiscard]] Status simulate(f32 dt = 1.0F / 60.0F) noexcept;

    /// Assemble, record and execute one frame; read the output image back when `read_back` is on.
    [[nodiscard]] Status render(assembly::AssemblyReport& out) noexcept;

    void set_read_back(bool on) noexcept { read_back_ = on; }

    [[nodiscard]] Span<const u32> pixels() const noexcept { return pixels_.span(); }
    [[nodiscard]] u32 differing_texels(Span<const u32> other) const noexcept;

    [[nodiscard]] vfx::SimulationWorld& world() noexcept { return world_; }
    [[nodiscard]] const vfx::StepReport& steps() const noexcept { return steps_; }
    [[nodiscard]] const vfx::PublishReport& published() const noexcept { return published_; }
    /// What was handed to the renderer. Read by the device suite when a picture comes back empty:
    /// "the simulation produced nothing" and "the renderer drew nothing" are different defects and
    /// a black frame alone cannot tell them apart.
    [[nodiscard]] Span<const particles::ParticleInstance> records() const noexcept {
        return records_.span();
    }
    [[nodiscard]] const particles::ParticleReport& particle_report() const noexcept {
        return effect_.report();
    }
    [[nodiscard]] const RecorderReport& recorded() const noexcept { return recorder_.report(); }

private:
    [[nodiscard]] Status create_dummy_geometry(rhi::Device& device) noexcept;
    [[nodiscard]] Status read_pixels() noexcept;

    Allocator* allocator_ = nullptr;
    rhi::Device* device_ = nullptr;

    graph::DiagnosticSink sink_;
    vfx::CompileReport cook_;
    Expected<vfx::CompiledSystem, Error> system_ = fail(ErrorCode::Unavailable, "not cooked");
    vfx::SimulationWorld world_;
    vfx::StepReport steps_;
    vfx::PublishReport published_;
    Array<particles::ParticleInstance> records_;

    assembly::FrameAssembly assembly_;
    SpatialIndex index_;
    RenderGraph graph_;
    FramePipelines pipelines_;
    FrameBindings bindings_;
    FrameRecorder recorder_;
    particles::ParticleRenderer effect_;
    Array<InstanceTransform> instances_;
    Array<render::LightDescription> lights_;
    Array<u32> pixels_;

    rhi::BufferHandle streams_[3];
    rhi::BufferHandle indices_;
    rhi::BufferHandle readback_;
    rhi::TextureHandle output_;
    u32 material_offsets_[4] = {0, 0, 0, 0};
    Mat4 view_ = Mat4::identity();
    Mat4 projection_ = Mat4::identity();
    bool read_back_ = false;
    bool built_ = false;
};

}  // namespace cy::vfx_test
