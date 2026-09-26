#pragma once
// SPDX-License-Identifier: MIT
// The water surface: the numbers `shaders/water.slang` is given, and the device objects it draws
// with. `water` — "Water surface shading" and "Caustics".
//
// ================================================================================================
// TWO HALVES, AND WHY THE FIRST NEEDS NO DEVICE
// ================================================================================================
//
// `build_water_params()` and `mirrored_rows()` are arithmetic on the processor: the body's optics
// turned into the shader's extinction and in-scatter, the inverse of the frame's matrix, the wave
// trains the caustics are derived from reduced to phases at this frame's time, and the matrix that
// draws the world mirrored in the water. They are free functions over plain values so that
// tests/render/test_world_water.cpp hands the shader EXACTLY what the stage hands it, and so that a
// reader can check each against the file in src/water/ it restates.
//
// `WaterSurface` is the device half: the water pipeline, its descriptor set, the parameter buffer
// and the four targets the refraction and reflection passes draw into. The stage owns one and asks
// it for each of those when it declares and records a frame.
//
// ================================================================================================
// THE TWO PICTURES, AND WHERE IN THE FRAME THEY ARE DRAWN
// ================================================================================================
//
// Both are drawn by `world.slang`'s own pipeline, in passes the stage declares into the render
// graph BEFORE the assembled frame, which the graph schedules in declaration order:
//
//   refraction   the terrain alone, from the frame's camera. Colour and depth. Under every water
//                pixel it holds the bed, and the depth says how far below the surface the bed is.
//   reflection   the sky, the terrain, the stars and the plants, mirrored in the plane of the
//                water's mean level by `mirrored_rows()`. Colour, and a depth only to test against.
//
// The opaque pass then draws the water with `water.slang` in the run where `world.slang` drew it
// before, and declares the three textures it samples as fragment reads, so the graph orders the two
// passes before it and transitions their targets for sampling.

#include <cy/backends/rhi/device.h>
#include <cy/backends/rhi/handles.h>
#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/math/matrix.h>
#include <cy/core/math/vec.h>
#include <cy/core/memory/array.h>
#include <cy/rendering/graph/executor.h>
#include <cy/rendering/graph/graph.h>
#include <cy/water/body.h>
#include <cy/water/displacement.h>
#include <cy/world/coordinates.h>

namespace cy::sample::world {

/// The most wave trains the caustics sum. `kCausticTrains` in shaders/water.slang.
inline constexpr u32 kCausticTrains = 16;

/// `WaterParams` in shaders/water.slang, field for field. Every member is four floats, so the
/// layout is the same under every target's rules for a structured buffer.
struct WaterParams {
    f32 inverse[4][4] = {};
    f32 viewport[4] = {};
    f32 extinction[4] = {};
    f32 in_scatter[4] = {};
    f32 surface[4] = {};
    f32 caustic[4] = {};
    f32 trains[kCausticTrains][4] = {};
    f32 phases[kCausticTrains] = {};
};

static_assert(sizeof(WaterParams) == sizeof(f32) * 4 * 29,
              "twenty-nine float4s, as the shader declares them");

/// The levers of the look, as opposed to the physics — which is the body's `WaterOptics` and the
/// ocean's own trains.
struct WaterLook {
    /// How far a wave normal of slope one moves the refracted and the reflected reads, as a
    /// fraction of the picture's height, so the look does not change with the resolution.
    f32 refraction_offset = 0.05F;
    f32 reflection_offset = 0.08F;
    /// The column depth, in metres, below which the shoreline foams. Zero draws none.
    f32 foam_band_metres = 1.2F;
    /// How strongly the surface focuses the sun on the bed. One is the physical amount; zero draws
    /// no caustics.
    f32 caustic_strength = 1.0F;
    /// The brightest a caustic may focus to, in multiples of the unfocused sun. The small-slope
    /// Jacobian goes to infinity where a crest brings a ray bundle to a point; a real one is
    /// bounded by the sun's own disc.
    f32 caustic_max_focus = 4.0F;
};

/// What the frame is, for the water: its matrix, its size, the water's level and the clock.
struct WaterView {
    Mat4 world_to_clip = Mat4::identity();
    u32 width = 1;
    u32 height = 1;
    /// The water's mean level, world-relative metres — which for y is absolute metres.
    f32 level = 0.0F;
    /// The world's simulated seconds, for the foam's slow breakup.
    f32 seconds = 0.0F;
    /// Where world-relative coordinates are relative to, and the water's own clock: together they
    /// put each train's phase at the world-relative origin at this instant.
    cy::world::WorldVec3d centre{0.0, 0.0, 0.0};
    f64 water_time = 0.0;
};

/// The trains that bend light the most — the largest amplitude times wavenumber squared, which is
/// each train's share of the surface's curvature — at most `out.size()` of them, strongest first.
/// Rendering's selection: every band, the visual-only ones included.
[[nodiscard]] u32 select_caustic_trains(const water::DisplacementModel& model,
                                        Span<water::WaveTrain> out) noexcept;

/// Everything `water.slang` reads from its parameter buffer. Refuses a singular matrix.
[[nodiscard]] Expected<WaterParams, Error> build_water_params(
    const water::WaterOptics& optics, const WaterLook& look, const WaterView& view,
    Span<const water::WaveTrain> trains) noexcept;

/// The rows of the world-to-clip matrix that draws the world MIRRORED in the plane y = `level`,
/// with its near plane moved onto that plane.
///
/// The mirror is the reflection y -> 2 level - y applied before the frame's matrix. The near plane
/// is the part that needs explaining: mirrored, whatever lay UNDER the water — the bed — lands
/// above it, between the eye and the surface it is supposed to be reflected in. So the third row is
/// replaced by `row3 - k (y - level)`: a vertex whose unmirrored height is below the water then has
/// a reversed-Z depth above one and is clipped, and one above it keeps a depth of
/// `1 - k (y - level) / w`, which falls monotonically along every eye ray — so the depth test still
/// orders the mirrored world correctly. `k` only has to keep the far side, `k (y - level) <= w`,
/// which a quarter does for everything inside the sky dome.
void mirrored_rows(const Mat4& world_to_clip, f32 level, Vec4 (&rows)[4]) noexcept;

/// The textures one frame's water reads, imported into that frame's graph.
struct WaterTargets {
    rendering::ResourceId refraction = rendering::kInvalidResource;
    rendering::ResourceId refraction_depth = rendering::kInvalidResource;
    rendering::ResourceId reflection = rendering::kInvalidResource;
    rendering::ResourceId reflection_depth = rendering::kInvalidResource;
};

/// The device half. See the header comment.
class WaterSurface {
public:
    WaterSurface() = default;
    WaterSurface(const WaterSurface&) = delete;
    WaterSurface& operator=(const WaterSurface&) = delete;

    /// The pipeline, set 1's layout and set, the parameter buffer and the four targets.
    /// `cloud_shadow_layout` is set 0 — world.slang's own — which the water reads too.
    [[nodiscard]] Status create(rhi::Device& device,
                                rhi::DescriptorSetLayoutHandle cloud_shadow_layout,
                                rhi::Format scene_format, u32 width, u32 height) noexcept;
    void destroy(rhi::Device& device) noexcept;

    [[nodiscard]] Status upload(rhi::Device& device, const WaterParams& params) const noexcept;

    /// Import this frame's four targets. Imported as `Undefined`: every pass that writes one
    /// clears it.
    [[nodiscard]] WaterTargets import(rendering::RenderGraph& graph) const noexcept;

    /// Write this frame's views of the three sampled targets into set 1. Called while recording,
    /// before the set is bound, because the graph makes the views.
    [[nodiscard]] Status bind(rhi::Device& device, const rendering::GraphExecutor& executor,
                              const WaterTargets& targets) const noexcept;

    [[nodiscard]] bool ready() const noexcept { return !pipeline_.is_null(); }
    [[nodiscard]] rhi::GraphicsPipelineHandle pipeline() const noexcept { return pipeline_; }
    [[nodiscard]] rhi::PipelineLayoutHandle layout() const noexcept { return layout_; }
    [[nodiscard]] rhi::DescriptorSetHandle set() const noexcept { return set_; }

private:
    [[nodiscard]] Status create_pipeline(rhi::Device& device, rhi::Format scene_format) noexcept;
    [[nodiscard]] Status create_targets(rhi::Device& device, rhi::Format scene_format) noexcept;

    u32 width_ = 0;
    u32 height_ = 0;
    rhi::Format scene_format_ = rhi::Format::Undefined;
    rhi::ShaderModuleHandle vertex_;
    rhi::ShaderModuleHandle fragment_;
    rhi::DescriptorSetLayoutHandle cloud_shadow_layout_;
    rhi::DescriptorSetLayoutHandle set_layout_;
    rhi::PipelineLayoutHandle layout_;
    rhi::GraphicsPipelineHandle pipeline_;
    rhi::DescriptorSetHandle set_;
    rhi::BufferHandle params_;
    rhi::TextureHandle refraction_;
    rhi::TextureHandle refraction_depth_;
    rhi::TextureHandle reflection_;
    rhi::TextureHandle reflection_depth_;
};

}  // namespace cy::sample::world
