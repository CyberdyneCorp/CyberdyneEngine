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
#include <cy/rendering/particles/strip_renderer.h>
#include <cy/rendering/pipeline/frame_recorder.h>
#include <cy/vfx/renderers.h>
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

/// WHAT A SCENE PHOTOGRAPHS, and every default is what the three original cases have always used —
/// so a caller that passes nothing gets the pictures committed in `docs/design/images/` and this
/// struct changes no image that existed before it.
///
/// It exists because `m11c:vfx-in-the-shot` needs the SHOT'S air photographed from the SHOT'S
/// camera: a second harness beside this one would be a second answer to "what does the frame do
/// with a publication", and tests/render/README.md's rule about golden images drifting applies to
/// harnesses as much as to pictures.
struct SceneOptions {
    /// A cooked system to play instead of the spark plume, with one instance at each position in
    /// `spawns`. Null cooks the plume and places the row of six the device suite draws.
    const vfx::CompiledSystem* system = nullptr;
    /// Where the instances go, in the same space `eye` is in.
    Span<const Vec3> spawns;
    /// The camera. Positions reach the device RELATIVE TO `eye` — `publish_sprites` rebases against
    /// it and the view matrix is built at the origin — which is design.md section 3's rule and the
    /// arrangement `samples/12-beauty` makes for the same reason.
    Vec3 eye{0.0F, 0.0F, 0.0F};
    Vec3 target{0.0F, 0.0F, -1.0F};
    f32 fov_y_radians = 0.9F;
    /// Read by the resolve. A particle's colour is a RADIANCE, so a frame graded at zero stops
    /// photographs an effect as a white rectangle.
    f32 exposure_stops = -11.4F;
    /// TRAILS, through `vfx-system`'s `Trail` renderer and `StripRenderer`, drawn in the same stage
    /// before the sprites. Null draws none — every picture committed before M11.c task 6.3.
    const vfx::RendererDecl* trails = nullptr;
    /// Publish the trails every this many `simulate` calls: the cadence `samples/12-beauty` uses,
    /// so a trail here is as long as the one in the published still.
    u32 trail_interval = 1;
};

/// The strip renderer's ring. Eight vertices for each of `kRingCapacity` particles would be 32 768;
/// the shot's air settles under a thousand motes, so this is room for that field twice over.
inline constexpr u32 kStripCapacity = 16384;

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

    /// The same, for a caller that brings its own effect and its own camera.
    [[nodiscard]] Status build(rhi::Device& device, const SceneOptions& options) noexcept;

    /// Draw NOTHING into the transparent stage while keeping every other part of the frame — the
    /// same passes, the same clear, the same resolve, the same exposure. That is the negative
    /// control `m11c:vfx-in-the-shot` is about: "a frame with an effect and the same frame without
    /// it must differ measurably", and the difference is only a measurement if the two frames
    /// differ in the effect and in nothing else.
    void set_draw_particles(bool on) noexcept { draw_particles_ = on; }
    /// The same control for the trails alone: every other part of the frame, the sprites included,
    /// is unchanged. That difference is how much of the picture the STRIP RENDERER is.
    void set_draw_trails(bool on) noexcept { draw_trails_ = on; }
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
    [[nodiscard]] const particles::StripReport& strip_report() const noexcept {
        return strips_.report();
    }
    [[nodiscard]] const vfx::RenderPublishReport& trailed() const noexcept { return trailed_; }
    [[nodiscard]] Span<const particles::StripVertex> trails() const noexcept {
        return trail_vertices_.span();
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
    vfx::RendererDecl trail_decl_;
    vfx::PublicationHistory history_;
    vfx::RenderPublishReport trailed_;
    Array<vfx::RibbonVertex> trail_rows_;
    Array<particles::StripVertex> trail_vertices_;
    u32 trail_interval_ = 1;
    u32 since_trail_ = 0;
    bool trails_enabled_ = false;

    assembly::FrameAssembly assembly_;
    SpatialIndex index_;
    RenderGraph graph_;
    FramePipelines pipelines_;
    FrameBindings bindings_;
    FrameRecorder recorder_;
    particles::ParticleRenderer effect_;
    particles::StripRenderer strips_;
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
    /// The point publication rebases against, and the point the view matrix is built at.
    Vec3 camera_position_{0.0F, 0.0F, 0.0F};
    Vec3 camera_forward_{0.0F, 0.0F, -1.0F};
    f32 fov_y_radians_ = 0.9F;
    f32 exposure_stops_ = -11.4F;
    bool draw_particles_ = true;
    bool draw_trails_ = true;
    bool read_back_ = false;
    bool built_ = false;
};

}  // namespace cy::vfx_test
