// SPDX-License-Identifier: MIT
#pragma once
// Fog of war on the device: the visibility texture uploaded, and every surface of the frame shaded
// by it.
//
// ================================================================================================
// THE SHAPE, AND WHERE IT SITS IN THE FRAME
// ================================================================================================
//
//   field texels ─► upload (transfer) ─► visibility texture ─┐
//                                                            ├─► fog of war (compute) ─► target
//   temporal colour ─► source ──────────── depth ────────────┘                           │
//                                                       depth of field, bloom, exposure ◄─┘
//
// Two graph passes, declared at the frame's `FramePassKind::FogOfWar` stage through
// `FrameStageDeclaration`: a transfer that copies this frame's texels from a host-visible ring into
// the visibility texture — one texel a grid cell, `Rgba8Unorm` — and a dispatch that reconstructs
// each pixel's world position from the depth, samples the texture there and writes the fogged
// colour into the stage's target, `FrameResources::fog_of_war`, which the post chain continues
// from. LINEAR HDR, BEFORE EXPOSURE, as the volumetric fog is: the fog is a property of the
// surfaces, so depth of field blurs it and bloom sees a darkened light as the dimmer light it is.
// With the stage off nothing is declared and the frame is the frame before this module, byte for
// byte; with it on and the team seeing everything, every pixel is written as it was read.
//
// ================================================================================================
// WHAT A CALLER DOES
// ================================================================================================
//
//     pass.create(device, {width, height, grid_width, grid_height});
//     // per frame, with `AssemblyDescription::fog_of_war`:
//     fog.write_states(team, states);          // the simulation's grid, read-only
//     field.set_states(states); field.advance(dt);
//     pass.set_frame({view, placement, style, &field});
//     sinks.fog_of_war = pass.stage();
//
// Which units of other teams are drawn is not this pass's decision: a unit the team does not see
// is left out of the draw list, and a remembered structure is drawn where it was last seen
// (`cy::gameplay::fog::FogOfWar::sight`), where this pass then greys it like the ground it is on.

#include <cy/backends/rhi/device.h>
#include <cy/backends/rhi/handles.h>
#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/math/matrix.h>
#include <cy/core/memory/array.h>
#include <cy/rendering/fog_of_war/visibility_field.h>
#include <cy/rendering/forward/frame.h>
#include <cy/rendering/graph/graph.h>

namespace cy::rendering::fog_of_war {

/// fog_of_war.slang's `words`, as `pack_fog_of_war_constants` lays them out.
inline constexpr u32 kFogOfWarConstantWords = 9;

struct FogOfWarConstants {
    Vec4 words[kFogOfWarConstantWords] = {};
};

/// The camera the frame was drawn from. `projection` is the unjittered projection and
/// `relative_view` the view's rotation alone — the frame's rows are camera-relative — so a pixel's
/// world position is `eye` plus the camera-relative point the two invert to.
struct FogOfWarView {
    Mat4 projection = Mat4::identity();
    Mat4 relative_view = Mat4::identity();
    Vec3 eye{0.0F, 0.0F, 0.0F};
};

/// The words the dispatch reads. Refused when the camera is singular, a cell has no size, the
/// style is out of range or an extent is zero.
[[nodiscard]] Expected<FogOfWarConstants, Error> pack_fog_of_war_constants(
    const FogOfWarView& view, const FogGridPlacement& placement, const FogOfWarStyle& style,
    u32 target_width, u32 target_height, u32 grid_width, u32 grid_height) noexcept;

struct FogOfWarPassDescription {
    /// The stage's extent: the frame's output extent.
    u32 width = 0;
    u32 height = 0;
    /// The visibility texture's: the fog grid's cells.
    u32 grid_width = 0;
    u32 grid_height = 0;
    /// Whether `read_back_visibility` and `read_back_colour` may be called: host copies of the
    /// visibility texture and of the colour the stage wrote, which a test compares with the host
    /// twins. (The colour the stage READ is the frame's — an imported history a transfer may not
    /// read — and a frame whose team sees everything writes it back unchanged, bit for bit.)
    bool readback = false;
};

/// One frame's fog.
struct FogOfWarFrame {
    FogOfWarView view;
    FogGridPlacement placement;
    FogOfWarStyle style;
    /// The team's field. Its texels are copied by `set_frame`, so it may change afterwards.
    const VisibilityField* field = nullptr;
};

/// What the last declared and recorded frame did.
struct FogOfWarReport {
    /// Graph passes declared: the upload and the dispatch, and the readback's two with `readback`.
    u32 passes = 0;
    /// Bytes the upload copied into the visibility texture.
    u64 uploaded_bytes = 0;
    bool dispatched = false;
};

class FogOfWarPass {
public:
    FogOfWarPass() = default;
    ~FogOfWarPass();

    FogOfWarPass(const FogOfWarPass&) = delete;
    FogOfWarPass& operator=(const FogOfWarPass&) = delete;
    FogOfWarPass(FogOfWarPass&&) = delete;
    FogOfWarPass& operator=(FogOfWarPass&&) = delete;

    /// A compute stage and a native shader format this module ships: SPIR-V or MSL.
    [[nodiscard]] static bool supported(const rhi::Device& device) noexcept;

    [[nodiscard]] Status create(Allocator& allocator, rhi::Device& device,
                                const FogOfWarPassDescription& desc) noexcept;
    void destroy() noexcept;
    [[nodiscard]] const FogOfWarPassDescription& description() const noexcept { return desc_; }

    /// This frame's camera, grid, style and field. The field must be the grid this pass was
    /// created for.
    [[nodiscard]] Status set_frame(const FogOfWarFrame& frame) noexcept;
    [[nodiscard]] const FogOfWarConstants& constants() const noexcept { return constants_; }

    /// The frame's hook: the stage declared as this module's passes.
    [[nodiscard]] FrameStageDeclaration stage() noexcept;
    /// Declare the upload and the dispatch. Returns the upload, or `kInvalidPass` when the inputs
    /// are not the ones this pass was created for or no frame was set.
    [[nodiscard]] PassId declare(RenderGraph& graph, const ScreenSpaceStageInputs& inputs) noexcept;

    [[nodiscard]] const FogOfWarReport& report() const noexcept { return report_; }
    /// The device time of the last executed frame's stage, from the upload's start to the
    /// dispatch's end, in milliseconds; negative where the device has no timestamp queries or no
    /// frame has been timed. Call after its fence.
    [[nodiscard]] f64 last_device_ms() const noexcept;

    /// The visibility texture as the last executed frame left it, `kTexelBytes` a cell. Call after
    /// its fence; only with `FogOfWarPassDescription::readback`.
    [[nodiscard]] Status read_back_visibility(Span<u8> out) const noexcept;
    /// The colour the last executed frame's stage wrote, `rgba16f` as four half-floats a pixel,
    /// row-major from the top-left, `width * height * 4` halves.
    [[nodiscard]] Status read_back_colour(Span<u16> target) const noexcept;

    // Public because `RecordFn` is a plain function pointer and the callbacks are free functions.
    void record_upload(const PassContext& context) noexcept;
    void record_dispatch(const PassContext& context) noexcept;
    void record_readback(const PassContext& context) noexcept;

private:
    [[nodiscard]] Status create_pipeline() noexcept;
    [[nodiscard]] Status create_resources() noexcept;
    [[nodiscard]] u64 texel_bytes() const noexcept;
    [[nodiscard]] u64 colour_bytes() const noexcept;
    static PassId declare_stage(RenderGraph& graph, const ScreenSpaceStageInputs& inputs,
                                void* user) noexcept;

    rhi::Device* device_ = nullptr;
    FogOfWarPassDescription desc_{};
    FogOfWarConstants constants_{};
    FogOfWarReport report_{};
    Array<u8> texels_;
    bool framed_ = false;

    rhi::ShaderModuleHandle shader_;
    rhi::DescriptorSetLayoutHandle set_layout_;
    rhi::PipelineLayoutHandle pipeline_layout_;
    rhi::ComputePipelineHandle pipeline_;
    rhi::TextureHandle visibility_;
    /// The texels and the constants, one host-visible buffer of each per frame in flight.
    rhi::BufferHandle staging_[rhi::kMaxFramesInFlight];
    rhi::BufferHandle words_[rhi::kMaxFramesInFlight];
    rhi::BufferHandle readback_;
    rhi::BufferHandle readback_target_;
    rhi::QueryPoolHandle timer_;
    bool timed_ = false;

    // This frame's resources, from `declare`.
    ResourceId source_ = kInvalidResource;
    ResourceId depth_ = kInvalidResource;
    ResourceId visibility_resource_ = kInvalidResource;
    ResourceId target_ = kInvalidResource;
    ResourceId readback_resource_ = kInvalidResource;
    ResourceId readback_target_resource_ = kInvalidResource;
};

}  // namespace cy::rendering::fog_of_war
