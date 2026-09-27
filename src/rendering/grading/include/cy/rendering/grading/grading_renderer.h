// SPDX-License-Identifier: MIT
#pragma once
// EXPOSURE AND COLOUR GRADING, ON THE DEVICE: the metering chain and the graded resolve.
//
// `rendering-post-processing` — "Exposure": manual exposure in EV (or aperture, shutter and ISO)
// and auto-exposure "driven by a luminance histogram computed by compute shaders over the scene
// colour", with percentile target selection, EV clamps, two adaptation speeds and a compensation
// curve, "applied as a scalar multiply before tonemapping". "Colour grading": a 3D LUT applied in
// log space, the parametric grade "bakeable into a single 3D LUT … so the runtime cost is one
// texture lookup". And "Chain order and colour space": metering at step 6, the exposure multiply at
// step 10, the curve at step 11 and the grade at step 12, on display-referred colour.
//
// ================================================================================================
// HOW IT REACHES A FRAME
// ================================================================================================
//
//   before `assemble`    `import_state(graph)`, and `post_process()` as the frame's
//                        `FramePassKind::PostProcess` callback — in place of the frame's own
//                        resolve. The callback declares its read of the exposure state, so the
//                        graph orders it after the previous frame's metering wrote it.
//   after `assemble`     `declare_metering(graph, resources, …)`. In automatic mode, three compute
//                        passes: clear, histogram over the scene-referred colour BEFORE bloom (the
//                        temporal history when there is one, else the shading target), adapt. They
//                        write the state the NEXT frame's resolve reads — a frame never waits on
//                        its own metering. In manual mode, nothing.
//   the settings         `PostChainConfig::auto_exposure` and `::colour_grading` are the caller's
//   to
//                        set, so the frame's manifest names the stages this renderer runs.
//
// ================================================================================================
// WHAT IS BYTE-IDENTICAL, AND WHY
// ================================================================================================
//
// With no table applied and the exposure pushed, the graded resolve is `fullscreenResolve`: the
// same library calls, `applyExposure` and the same curve, in the same order. So a grade that bakes
// to the identity (`display_lut_is_identity`) is not applied — `set_lut` reports it and leaves the
// lookup off — and a frame graded neutrally is the frame without grading, byte for byte.
// render.grading asserts both, and asserts the frame without this renderer against a reference
// drawn before it existed.
//
// ================================================================================================
// WHAT IT OWNS
// ================================================================================================
//
// One graphics pipeline over the library's `fullscreenVertex`, three compute pipelines, their four
// set layouts, a clamped linear sampler, the grading table (a 3D `Rgba16Sfloat` texture, a 2³
// identity until a grade is set), the 256-bin histogram and the one-`float4` exposure state, and —
// with `readback` — two host-visible copies of those for a test to read. D3D12 is not a target:
// nothing in this module embeds DXIL, as for every other device pass in the tree.

#include <cy/backends/rhi/device.h>
#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/math/vec.h>
#include <cy/rendering/forward/frame.h>

namespace cy::rendering {
// Declared rather than included: only the settings' builder and this module's source need the
// definition, and every frame that installs the resolve includes this header.
struct AutoExposureSettings;
}  // namespace cy::rendering

namespace cy::rendering::grading {

/// The histogram's bins: one a thread of the histogram dispatch's 16 x 16 group.
inline constexpr u32 kExposureBins = 256;

/// The tone curve the graded resolve applies — `fullscreenResolve`'s `kTonemapOperator`, with the
/// same values, because the two must agree for the neutral frame to be the frame's own.
enum class ResolveCurve : u8 {
    None = 0,
    Reinhard = 1,
    AcesFit = 2,
    Count = 3,
};

[[nodiscard]] const char* resolve_curve_name(ResolveCurve curve) noexcept;

/// exposure_common.slang's `CyExposureConstants`, word for word.
struct alignas(16) ExposureConstants {
    /// Lowest and highest EV100 of the histogram, then the low and high percentiles.
    f32 histogram[4] = {};
    /// The metered EV100's clamps, then EV per second brightening and darkening.
    f32 limits[4] = {};
    f32 curve_ev[4] = {};
    f32 curve_compensation[4] = {};
    /// Seconds since the last frame, 1 to restart from z, the restarting EV100, unused.
    f32 frame[4] = {};
    /// The metered image's extent.
    u32 extent[4] = {};
};

static_assert(sizeof(ExposureConstants) == 96, "exposure_common.slang's constants are 96 bytes");

/// graded_resolve.slang's `CyGradedResolvePush`, word for word.
struct alignas(16) GradedResolvePush {
    /// Stops when pushed, 1 to read the metered EV100 instead, log2(1.2), unused.
    f32 exposure[4] = {};
    /// 1 when the table is applied, its edge, unused, unused.
    f32 lut[4] = {};
};

static_assert(sizeof(GradedResolvePush) == 32, "graded_resolve.slang's push block is 32 bytes");

/// The push block of one metering frame. A free function so the arithmetic that turns settings into
/// shader numbers is checkable without a device. The histogram spans the settings' own EV clamps.
[[nodiscard]] ExposureConstants exposure_constants(const AutoExposureSettings& settings,
                                                   f32 delta_seconds, bool restart,
                                                   f32 restart_ev100, u32 width,
                                                   u32 height) noexcept;

/// What the metering chain last wrote, read back when the renderer was created with `readback`.
struct ExposureReadback {
    /// The EV100 in force after the frame's adaptation — what the next frame's resolve applies.
    f32 current_ev100 = 0.0F;
    /// The clamped, compensated target it adapted toward.
    f32 target_ev100 = 0.0F;
    /// The percentile mean before compensation and clamps.
    f32 metered_ev100 = 0.0F;
    /// Frames adapted since the last restart, counting this one.
    f32 frames = 0.0F;
    u32 histogram[kExposureBins] = {};
};

struct GradingRendererDescription {
    /// The frame's output format: the resolve's colour attachment.
    rhi::Format output_format = rhi::Format::Rgba8Unorm;
    ResolveCurve curve = ResolveCurve::Reinhard;
    /// Copy the histogram and the state to host-visible buffers every metered frame.
    bool readback = false;
};

/// What the recording did, read off the recording.
struct GradingReport {
    u32 resolves = 0;
    u32 dispatches = 0;
    /// Resolves recorded with the table applied.
    u32 graded_resolves = 0;
    /// Resolves recorded with the metered exposure.
    u32 metered_resolves = 0;
};

class GradingRenderer {
public:
    GradingRenderer() noexcept = default;
    ~GradingRenderer();

    GradingRenderer(const GradingRenderer&) = delete;
    GradingRenderer& operator=(const GradingRenderer&) = delete;
    GradingRenderer(GradingRenderer&&) = delete;
    GradingRenderer& operator=(GradingRenderer&&) = delete;

    /// Create the pipelines, the buffers and the identity table. Uploads the table in a submission
    /// of its own, so call it outside a device frame.
    [[nodiscard]] Status initialize(rhi::Device& device, Allocator& allocator,
                                    const GradingRendererDescription& description) noexcept;
    /// Release everything. Idempotent; the caller waits for the device first.
    void shutdown() noexcept;
    [[nodiscard]] bool ready() const noexcept { return ready_; }

    // --- The grade ----------------------------------------------------------------------------

    /// Upload a baked table — `bake_display_lut`'s output, `size`³ encoded colours, x fastest — in
    /// a submission of its own (call it outside a device frame). An identity table is not uploaded
    /// and the lookup is left off: `applied` says which happened.
    [[nodiscard]] Status set_lut(Span<const Vec3> table, u32 size, bool& applied) noexcept;
    /// Upload even an identity table and apply it. What a test that measures the lookup itself
    /// uses; a frame wants `set_lut`.
    [[nodiscard]] Status force_lut(Span<const Vec3> table, u32 size) noexcept;
    /// Stop applying the table.
    void clear_lut() noexcept { lut_applied_ = false; }
    [[nodiscard]] bool lut_applied() const noexcept { return lut_applied_; }

    // --- Exposure -----------------------------------------------------------------------------

    /// Manual exposure, in the stops `cy/fullscreen.slang`'s resolve multiplies by.
    void set_manual_stops(f32 stops) noexcept;
    /// Manual exposure as an EV100 — or a camera's, through `ev100_from_camera`.
    void set_manual_ev100(f32 ev100) noexcept;
    /// Automatic exposure, starting — on the next frame — from `starting_ev100`.
    void set_automatic(const AutoExposureSettings& settings, f32 starting_ev100) noexcept;
    /// Start the adaptation again from `ev100` on the next frame: a camera cut.
    void restart(f32 ev100) noexcept;
    void set_delta_seconds(f32 seconds) noexcept { delta_seconds_ = seconds; }
    [[nodiscard]] bool automatic() const noexcept { return automatic_; }

    // --- One frame ----------------------------------------------------------------------------

    /// Import the exposure state into this frame's graph. Before `assemble`.
    [[nodiscard]] ResourceId import_state(RenderGraph& graph) noexcept;
    /// The `FramePassKind::PostProcess` callback: the graded resolve, declaring its read of the
    /// state `import_state` imported.
    [[nodiscard]] FramePassCallback post_process() noexcept;
    /// After `assemble`: name the resolve's source and output, and in automatic mode declare the
    /// metering passes over the frame's scene-referred colour before bloom.
    [[nodiscard]] Status declare_metering(RenderGraph& graph, const FrameResources& resources,
                                          u32 width, u32 height) noexcept;
    /// The same, metering an explicit image — what the device suite's histogram case uses.
    [[nodiscard]] Status declare_metering_of(RenderGraph& graph, ResourceId source, u32 width,
                                             u32 height) noexcept;

    /// The last metered frame's histogram and state. Requires `readback`, and a completed frame.
    [[nodiscard]] Status read_exposure(ExposureReadback& out) const noexcept;

    [[nodiscard]] const GradingReport& report() const noexcept { return report_; }
    void reset_report() noexcept { report_ = GradingReport{}; }

    // Public because `RecordFn` is a plain function pointer.
    void record_resolve(const PassContext& context) noexcept;
    void record_clear(const PassContext& context) noexcept;
    void record_histogram(const PassContext& context) noexcept;
    void record_adapt(const PassContext& context) noexcept;
    void record_readback(const PassContext& context) noexcept;

private:
    [[nodiscard]] Status create_modules() noexcept;
    [[nodiscard]] Status create_layouts() noexcept;
    [[nodiscard]] Status create_pipelines() noexcept;
    [[nodiscard]] Status create_buffers() noexcept;
    [[nodiscard]] Status upload_table(Span<const Vec3> table, u32 size) noexcept;
    void destroy_table() noexcept;
    [[nodiscard]] Status write_set(rhi::DescriptorSetLayoutHandle layout,
                                   Span<const rhi::DescriptorWrite> writes,
                                   rhi::DescriptorSetHandle& out) noexcept;
    void dispatch(const PassContext& context, u32 which, rhi::DescriptorSetHandle set, u32 groups_x,
                  u32 groups_y) noexcept;

    rhi::Device* device_ = nullptr;
    Allocator* allocator_ = nullptr;
    GradingRendererDescription description_;

    rhi::ShaderModuleHandle vertex_;
    rhi::ShaderModuleHandle fragment_;
    rhi::ShaderModuleHandle compute_[3];
    rhi::DescriptorSetLayoutHandle resolve_set_;
    rhi::DescriptorSetLayoutHandle compute_sets_[3];
    rhi::PipelineLayoutHandle resolve_layout_;
    rhi::PipelineLayoutHandle compute_layouts_[3];
    rhi::GraphicsPipelineHandle resolve_;
    rhi::ComputePipelineHandle compute_pipelines_[3];
    rhi::SamplerHandle sampler_;

    rhi::TextureHandle table_;
    rhi::TextureViewHandle table_view_;
    u32 table_size_ = 0;
    bool lut_applied_ = false;

    rhi::BufferHandle histogram_;
    rhi::BufferHandle state_;
    rhi::BufferHandle histogram_readback_;
    rhi::BufferHandle state_readback_;

    // Exposure settings.
    bool automatic_ = false;
    f32 manual_stops_ = 0.0F;
    /// The automatic settings as the push block carries them; the frame fields are filled per
    /// frame.
    ExposureConstants settings_constants_;
    f32 restart_ev100_ = 0.0F;
    bool restart_pending_ = false;
    f32 delta_seconds_ = 1.0F / 60.0F;

    // This frame.
    ResourceId state_resource_ = kInvalidResource;
    FrameResourceRead state_read_;
    ResourceId histogram_resource_ = kInvalidResource;
    ResourceId metered_source_ = kInvalidResource;
    ResourceId scene_ = kInvalidResource;
    ResourceId output_ = kInvalidResource;
    ResourceId histogram_copy_ = kInvalidResource;
    ResourceId state_copy_ = kInvalidResource;
    u32 width_ = 0;
    u32 height_ = 0;
    u32 metered_width_ = 0;
    u32 metered_height_ = 0;
    GradedResolvePush resolve_push_;
    ExposureConstants constants_;

    GradingReport report_;
    bool ready_ = false;
};

}  // namespace cy::rendering::grading
