#pragma once
// The scene both pipeline suites render, and the harness that renders it. M8.c tasks 1b.1 and 1b.2.
//
// ONE SCENE, TWO BACKENDS. `integration.render_pipeline` runs it through the null backend and asks
// what was DECIDED — how many callbacks the sinks carry, how many passes ran, how many draws were
// recorded, how many bytes the Prepare transfer moved. `render.pipeline` runs the same code on a
// Vulkan device with validation and synchronisation validation on and asks what was DRAWN. Neither
// suite has a scene of its own, for the reason tests/render/README.md gives about the golden
// images: a second scene drifts from the first inside a milestone.
//
// THE CONTROL IS BUILT IN. `render(RecordMode::None)` assembles and executes the identical frame
// with an empty `FrameSinks` — which is exactly what every caller in the tree did before this
// milestone. Both suites compare the two, and the device suite writes both pictures out. That is
// task 5.5's before-and-after pair, produced by the same code path rather than by two runs of two
// programs.

#include <cy/backends/rhi/device.h>
#include <cy/core/base/expected.h>
#include <cy/core/math/matrix.h>
#include <cy/core/math/projection.h>
#include <cy/core/memory/array.h>
#include <cy/rendering/assembly/frame_assembly.h>
#include <cy/rendering/particles/particle_renderer.h>
#include <cy/rendering/pipeline/frame_recorder.h>

#include <cmath>
#include <numbers>

namespace cy::pipeline_test {

using namespace cy::rendering;
using namespace cy::rendering::pipeline;

inline constexpr u32 kWidth = 480;
inline constexpr u32 kHeight = 270;
inline constexpr u32 kInstanceCount = 12;
inline constexpr u32 kMaterialCount = 4;
inline constexpr u32 kParticleCount = 512;

/// What the harness supplies to `FrameAssembly::assemble`.
enum class RecordMode : u8 {
    /// An empty `FrameSinks` — the state of every caller in the tree before M8.c. The frame is
    /// assembled, compiled, barriered and executed, and nothing is recorded into it.
    None = 0,
    /// The layer's callbacks.
    Callbacks,
    /// The layer's callbacks plus the particle renderer attached to the transparent stage.
    CallbacksAndParticles,
};

/// A unit cube in the frame's three streams, plus its indices. Twenty-four vertices so each face
/// has its own normal; a shared-vertex cube would shade like a sphere and hide a normal defect.
struct CubeMesh {
    f32 positions[24 * 3] = {};
    u16 normals[24 * 4] = {};
    f32 uvs[24 * 2] = {};
    u16 indices[36] = {};

    CubeMesh() noexcept {
        static constexpr Vec3 kAxes[6] = {Vec3{1, 0, 0},  Vec3{-1, 0, 0}, Vec3{0, 1, 0},
                                          Vec3{0, -1, 0}, Vec3{0, 0, 1},  Vec3{0, 0, -1}};
        usize vertex = 0;
        usize index = 0;
        for (const Vec3 normal : kAxes) {
            // Any unit vector perpendicular to the normal; the frame reads the tangent from the
            // stream but this scene's materials do not use it, and a zero tangent would encode to a
            // direction rather than to nothing.
            const Vec3 tangent =
                std::fabs(normal.y) > 0.5F ? Vec3{1.0F, 0.0F, 0.0F} : Vec3{0.0F, 1.0F, 0.0F};
            const Vec3 bitangent{(normal.y * tangent.z) - (normal.z * tangent.y),
                                 (normal.z * tangent.x) - (normal.x * tangent.z),
                                 (normal.x * tangent.y) - (normal.y * tangent.x)};
            const auto first = static_cast<u16>(vertex);
            for (u32 corner = 0; corner < 4U; ++corner) {
                const f32 u = (corner == 1U || corner == 2U) ? 1.0F : -1.0F;
                const f32 v = (corner >= 2U) ? 1.0F : -1.0F;
                const Vec3 position{
                    (normal.x * 0.5F) + (tangent.x * u * 0.5F) + (bitangent.x * v * 0.5F),
                    (normal.y * 0.5F) + (tangent.y * u * 0.5F) + (bitangent.y * v * 0.5F),
                    (normal.z * 0.5F) + (tangent.z * u * 0.5F) + (bitangent.z * v * 0.5F)};
                positions[(vertex * 3U) + 0] = position.x;
                positions[(vertex * 3U) + 1] = position.y;
                positions[(vertex * 3U) + 2] = position.z;
                pack_normal_stream(normal, tangent, &normals[vertex * 4U]);
                uvs[(vertex * 2U) + 0] = (u * 0.5F) + 0.5F;
                uvs[(vertex * 2U) + 1] = (v * 0.5F) + 0.5F;
                ++vertex;
            }
            const u16 order[6] = {0, 1, 2, 0, 2, 3};
            for (const u16 step : order) {
                indices[index++] = static_cast<u16>(first + step);
            }
        }
    }
};

/// The whole thing: the scene, the assembly, the layer, and one render.
class FrameScene {
public:
    explicit FrameScene(Allocator& allocator) noexcept
        : allocator_(&allocator),
          assembly_(allocator),
          index_(allocator),
          graph_(allocator),
          program_(allocator),
          instances_(allocator),
          particles_(allocator),
          lights_(allocator),
          pixels_(allocator) {}

    ~FrameScene() { release(); }

    FrameScene(const FrameScene&) = delete;
    FrameScene& operator=(const FrameScene&) = delete;

    [[nodiscard]] Status build(rhi::Device& device) noexcept;
    void release() noexcept;

    [[nodiscard]] Status render(RecordMode mode, AssemblyReport& out) noexcept;

    [[nodiscard]] const RecorderReport& recorded() const noexcept { return recorder_.report(); }
    [[nodiscard]] const particles::ParticleReport& particle_report() const noexcept {
        return effect_.report();
    }
    [[nodiscard]] const FrameRecorder& recorder() const noexcept { return recorder_; }
    [[nodiscard]] const FramePipelines& pipelines() const noexcept { return pipelines_; }
    [[nodiscard]] FrameAssembly& assembly() noexcept { return assembly_; }
    /// The last frame's output, Rgba8Unorm, row-major from the top-left. Empty until a device
    /// render with `read_back` on.
    [[nodiscard]] Span<const u32> pixels() const noexcept { return pixels_.span(); }
    void set_read_back(bool on) noexcept { read_back_ = on; }
    /// How many texels differ from `other` by more than one 8-bit step in any channel.
    [[nodiscard]] u32 differing_texels(Span<const u32> other) const noexcept;

    // Public because the geometry lookup is a plain function pointer.
    [[nodiscard]] rhi::BufferHandle index_buffer() const noexcept { return indices_; }

private:
    [[nodiscard]] Status create_geometry(rhi::Device& device) noexcept;
    [[nodiscard]] Status create_materials() noexcept;
    [[nodiscard]] Status fill_index() noexcept;
    [[nodiscard]] Status fill_particles() noexcept;
    [[nodiscard]] Status read_pixels() noexcept;

    Allocator* allocator_ = nullptr;
    rhi::Device* device_ = nullptr;
    FrameAssembly assembly_;
    SpatialIndex index_;
    RenderGraph graph_;
    MaterialProgram program_;
    FramePipelines pipelines_;
    FrameBindings bindings_;
    FrameRecorder recorder_;
    particles::ParticleRenderer effect_;
    Array<InstanceTransform> instances_;
    Array<particles::ParticleInstance> particles_;
    Array<render::LightDescription> lights_;
    Array<u32> pixels_;

    CubeMesh mesh_;
    rhi::BufferHandle positions_;
    rhi::BufferHandle normals_;
    rhi::BufferHandle uvs_;
    rhi::BufferHandle indices_;
    rhi::BufferHandle readback_;
    /// The frame's output, IMPORTED rather than frame-owned. A frame that creates its own output
    /// creates it in the colour format — `Rgba16Sfloat` here — and this suite wants the 8-bit
    /// display-referred image a swapchain would hold, which is also what a PNG is. Importing it is
    /// exactly what a windowed host does with its swapchain image.
    rhi::TextureHandle output_;
    u32 material_offsets_[4] = {0, 0, 0, 0};
    Mat4 view_ = Mat4::identity();
    Mat4 projection_ = Mat4::identity();
    bool read_back_ = false;
    bool built_ = false;
};

}  // namespace cy::pipeline_test
